# tools/wallrule

Where every wall texel comes from: ROTH.C's rule, what the loader makes the
engine draw, and the original's own pixels.

| file | what |
|---|---|
| `wallrule.cpp` | Over the retail maps, for every wall piece: the texel ROTH.C samples (an independent transcription of `renderer.c`, cited inline), the texel the loader as it was before the wallrule patch made the engine sample (`HEAD`), and the texel the patched loader does (`FIXED`, which calls `roth_surface`'s `WallPieceRule` / `WallToEngine` / `WallBakeMaps` -- the same functions `rothmap.cpp` calls -- and pushes the result through a model of `hw_walls.cpp` / `gametexture.cpp`). Reports the pieces that differ, by cause, and every pair of adjacent coplanar pieces whose picture runs on across the join in ROTH.C and does not in a loader model (compared in pixels, so two different images cut to continue each other count). |
| `build.sh` | `sh tools/wallrule/build.sh` -> `/tmp/wallrule`. Plain g++, no engine. |
| `oracle_capture.sh` | The original engine's painted passes at one pose (needs the oracle build). |
| `oracle_cmp.py` | Per pixel: the texel the original drew against the rule (`wallrule -export`). `0` = ROTH rule, `1` = HEAD model, `2` = FIXED model; a fourth argument fits the camera first (the original nudges the player after a pose is set). |
| `facepose.py` | A pose square-on to a face, `dist` units off it. |

```
/tmp/wallrule "<...>/Realms of the Haunting/DATA"                 # all maps, totals
/tmp/wallrule "<...>/DATA" STUDY1                                 # one map, with lists
/tmp/wallrule "<...>/DATA" STUDY1 -export /tmp/STUDY1.pieces
tools/wallrule/oracle_capture.sh STUDY1 c3 448 560 256 /tmp/or
python3 tools/wallrule/oracle_cmp.py /tmp/STUDY1.pieces /tmp/or/c3 0 fit
```

What the comparison tolerates: one texel either way (the original floors a
half-texel accumulator; the engine samples continuously), on a 9 x 9 grid of
points per piece.

Not covered: lighting, door leaves (polyobjects), mid-platform edges (3D
floors), the parallax sky above FF_EDGE_MAP faces, floors and ceilings, and
anything that moves after the load -- ROTH.C re-derives every piece from the
live sector heights each frame.

`-export` writes the FIXED model's baked pieces with the baked image's size and
without its column / row maps, so `oracle_cmp.py ... 2` is only meaningful on
pieces that are not baked; `0` (the rule itself) is the comparison that matters.

Every run also lists every wall image the patched loader registers -- plain,
mirrored or baked, with its width and height as `roth_texture.cpp` Build makes
it -- and checks each against what the engine accepts: both sizes above 0 and
below 65536 (`FTexture` keeps them in `uint16_t`; Vulkan refuses a 0), every
bake map entry inside the picture, every animation frame as large as the
picture. `WALLRULE_REGS=1` prints them all. Over the 44 maps: 3,106 distinct
images, 0 bad.
