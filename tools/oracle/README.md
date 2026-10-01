# tools/oracle — measure REMAROTH against the original, pixel by pixel

The ROTH.C oracle. It runs the original engine headless, makes it paint every
texture with its own texel coordinates, reads back which texel it drew at every
pixel, and fits the mapping rule from that. Built and first run 2026-10-01.
Results: `REMAROTH_ORACLE_RESULTS.md` in the VRealms project docs.

## Pieces

| file | what |
|---|---|
| `oracle.c` | ROTH.C mod (SDK plugin). Skips every GDV and answers the main menu with "new game", so the game boots straight into the first map. Paints each loaded DAS image with a 6-bit slice of `col | row<<8 | fatIndex<<16` (pass 0..4 via `ORACLE_PASS`), makes all shade/remap tables identity, tags flat spans with sector + fill word, pins the camera (`ORACLE_POSE="x y angle sectorOffset z"`, ROTH units: angle 512/turn CCW from +Y), and dumps the indexed frame + tags (`ORACLE_OUT`). `ORACLE_PAINT=0` captures real art. |
| `capture.sh GAMEDIR PREFIX "POSE"` | runs the 5 passes + a real-art pass, **sequentially** (the host's shared-memory framebuffer is global; parallel runs corrupt each other). |
| `mkgame.sh MAP PACK` | a symlinked game dir whose ROTH.RES lists MAP first, so new-game starts there. |
| `decode.py`, `common.py` | decode the 5 passes into (texture, col, row) per pixel. |
| `rawmap.py` | minimal RAW reader (sectors, faces, texture maps, vertices). |
| `flatfit.py` | per sector: fits ROTH's flat mapping (axis, sign, scale, offset) from the pixels. |
| `walls.py` | assigns wall pixels to faces by 2D ray cast and fits the wall mapping. |
| `e2e.py` | end to end: ROTH.C pixels vs a model of GZDoom's flat path fed by REMAROTH's own `FlatToEngine`. |
| `poses.py` | places a camera square-on to a chosen face. |

## Camera model (solved from geometry, not assumed)

eye = playerZ + 144 (2 x playerHeight), fx = 309.8 (horizontal FOV 91.8), fy = 355.1,
centre (320.5, 240.5) on the 640x480 frame. Facing theta = 90deg + angle*360/512,
right = (sin theta, -cos theta).

## Gotchas found the hard way

- Run captures one at a time (shared `/roth_fb`).
- The paint hook must only paint blocks the call actually loaded: placeholder ids
  return success without loading and leave the previous block current.
- Images wider than 256 lose col bit 8 in this encoding; compare low 8 bits.
- The original sometimes nudges the player after a pose is set; fit the camera
  rather than trusting the header position when residuals scale uniformly.
