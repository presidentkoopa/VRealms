# tools/surfbox_test -- the RAW records' surface box against the running original

A differential test of `src/roth/roth_surfbox.h`: where on a wall face or a floor a click must land for a RAW
click record (0x18 use wall, 0x19 use floor, 0x32 examine wall, 0x31 examine floor) to fire. The ORIGINAL is
ROTH.C built headless (`/home/claude/oracle_build`); `boxprobe.c` is its mod; `surfbox_test.cpp` is the port side
(engine-free: g++ and the `src/roth` readers).

    tools/surfbox_test/run.sh match            # every map: the match cases; exit 0 = no difference
    tools/surfbox_test/run.sh texel STUDY1     # one map: the texel cases
    SURFBOX_MUT=1 tools/surfbox_test/run.sh match   # a mutation (below): must FAIL

Outputs under `$SURFBOX_WORK` (default `/tmp/surfbox_test`); nothing into the repo. Logs are derived from retail
data: never commit them.

## Files

| file | what |
|---|---|
| `boxprobe.c` | the ROTH.C mod. `P` poses the player and logs the original's cursor pick (`render_world_view`, its whole 0x24-byte record) over a screen grid; `M` builds a pick record and hands it to `dispatch_entry_command_trigger` and `_b`, logging the record each returns; `X` clears the spent bit of every record in the four directories; `GEOM` (once) is the live geometry buffer. `SB_NOCHAINS=1` stops command chains and movies while the texel script poses the player (a pose drops the player into sectors whose enter triggers would move geometry or start a cutscene). |
| `surfbox_test.cpp` | `census`, `match-script`/`match-check`, `texel-script`/`texel-check`. |
| `oracle_run.sh` | builds the mod, makes the game folder, runs one script through the original under `/tmp/oracle_shot.lock`. |
| `run.sh` | all of it, per map. |

## The match cases (roth_surfbox.h checklist 1-8): exact

For every map, every target of a record with a box -- each face whose texture map's faceID is the key, each
sector whose command id is the key -- every pick type (faces: type 3 with each band index 0-3; sectors: types 2,
8, 1, 7) and the texel points of every box on that target (each corner, each edge, one in and one out of each
edge, the middle) plus (0,0), (-32768,-32768) and (32767,32767). Twice: as loaded, and with every record of the
four directories enabled (`X`), which reaches records a chain enables later and the walk's fall-through past
them. The port side walks the same records in RECORD order (as the engine binds them; `census` checks that every
directory is in record order) with `SurfFirstMatch`, and gates the one it stops at with `WalkChainFlow`. The
marker bytes the original tests first are modelled as "a record of that channel names the target" (what the
engine's bindings give); that equivalence is under test too.

Result (2026-11-13): 757,120 cases on 43 maps, 757,120 identical, 0 differ; 299 of the 309 retail boxes matched
in some case (the other 10: 3 name no geometry, 2 are disabled at load and shadowed, 5 are shadowed by an
earlier record). RAQUIA2 has no box (and the headless original does not load it).

Mutations, each must fail: `1` the box's upper x edge exclusive -> 48,565 differ; `2` "no box" read from +0x0c
alone (the ROTH_COMMANDS.md summary) instead of the dword -> 240,424 differ; `3` out of the box stops the walk
-> 9,933 differ.

## The texel cases (checklist 9-13): exact where the camera is not in question

The original's own cursor pick over screen grids, from poses square on to every boxed face (64, 110, 170 units
off, faces along an axis) and over every boxed floor (from its middle and 60 / 120 units back along each axis,
looking down at the original's pitch limit, -126), each pose clear of every face by 64 units. For every pick on
a face or a flat the hit point is rebuilt with the original's camera and the port's texel computed there:
`WallPickTexel` over `WallPieceRule` (the face rule the loader draws with), `FlatPickTexel`.

The camera is a FIT, not the original's projection, so the test says how far it trusts it:

- FX 309.77, FY 355.06, X0 320.48, Y0 240.54 (REMAROTH_MEASURED.md 1); the eye 2 x the map's player height
  - 0.06 above the pose's z (143.94 at height 72, measured; 128 on the height-64 maps); pitch a shear of the
  rows, 2.777 px per unit (fitted here: 2.765 / 2.774 / 2.78 / 2.79 give 9,643 / 4,941 / 4,876 / 9,384 wall
  differences on STUDY1); with pitch 0 the original's applied pitch (0x8c108, eased toward the floor clearance)
  is held at 0 by the probe.
- the ray at sub-pixel offset 0 across for walls and 1 across for flats (with X0 320.48), 0.5 down (fitted on
  STUDY1; the best of nine pairs each).
- the pose is the one the frame was DRAWN with (read as run_gameplay_frame is entered): the game's own movement
  pushes a written pose off an object it lands in before the 3D pass the pick reuses (VICAR1 face 422: 27 units).
- a pick whose pixel, one pixel either way, straddles a texel edge is counted "at an edge", not compared;
  so are picks outside the centre of the view (|dx| > 128, |dy| > 220), where the original's columns drift off
  a pinhole by more than a pixel and the first and last rows of a clipped span start a texel off (both
  measured on STUDY1).
- picks not seen directly (a face in front of the hit: a mirror) are counted apart.
- on a piece whose row count is not a power of two and that runs past its last row only the column is
  compared (the original's row mapper sticks there; roth_surface.cpp WrapRow).

Mutations on STUDY1, each must fail: `4` FLIP_X not mirrored on a wall's +4 -> 29,237 wall picks differ; `5` a
flat's +4 / +6 swapped -> 30,175 flat picks differ.

What is reported per map: identical picks, picks at an edge, differences, each class's identical count (wall
flags, flat sizes) and the classes with differences.

Result (2026-11-13, all maps; `run.sh texel`, exit 1). 0 differ on 21 of the 30 maps that get a pose: STUDY1
294,437 wall and 131,954 flat picks identical; CHURCH1 421,438 / 136,210 (158 wall differ); every one-sided and
two-sided class, FLIP_X, IMAGE_FIT, PIN_BOTTOM, HALF_PIXEL, TRANSPARENT mids; flats 32x32 to 256x256, s1 to s3,
opaque256. What still differs, not explained:
- SOULST2: 535,787 floor picks (256x256 s3 opaque256 floors, e.g. sector 434: the row off by a constant 98
  texels). The same class agrees on SOULST3 (55,670 identical), so a map- or pose-specific cause is likely;
  not found.
- SOULST3: 51,948 mid-platform-top picks (type 8, 128x128 / 64x64 s1, shifts 128,128): nothing alike. A boxed
  floor record sits on these sectors (the poses were made for it).
- under 0.15 % of the picks of a few faces on CAVERNS3 (676), VICAR (232), CHURCH1 (158), GRAVE (158),
  SOULST2 (32), SOULST3 (21), MAS3 (19), RAQUIA5 (4): two-sided upper pieces and some mids, one texel; and
  one AQUA1 floor pick.
- LRINTH1 and SALVAT carry boxes but get no pose (a 64-unit clearance, axis-aligned faces, one sector at the
  stand point): their boxes are covered by the match cases only.

## What this does NOT prove

- The ENGINE's assembly of the same inputs (roth_runtime.cpp `WallHitTexel`, `FlatHitTexel`: the loader's
  `WallReg`, the side's direction, the registered flat's size, the portal plan's sector shift) is not run here.
  That is checked by `tools/claude_bridge/cfg/surfbox_flat.cfg` on the owner's PC.
- The camera fit (above): the texel rule is proven on the picks the fit can place.
