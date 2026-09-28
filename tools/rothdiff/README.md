# rothdiff

Compare what ROTH.C draws against what REMAROTH draws, **per pixel**, as data.

## Why

Every texture decision on this port that was settled by looking at two
screenshots has had to be reversed at least once. A screenshot carries only
colour, so a wrong texture, a wrong scale and a wrong light all look the same,
and none of them can be compared by a machine afterwards.

rothdiff compares **identity buffers**: for a given camera pose, each engine
records which surface it drew at every pixel and out of which texture. The
output is a match percentage, the mismatches grouped by cause, and the worst
offending surfaces by pixel count. A regression is an exit code.

**Run it after every loader change. Never report a fix without its output, and
always state the match percentage before and after.**

## The pieces

| | |
|---|---|
| `rothdiff_plugin.c` + `idbuffer.inc.c` | ROTH.C's side: a headless command mode. Pins a camera pose, renders one frame of the **visible** pass, writes the buffer, exits. |
| `src/roth/roth_diff.cpp` | REMAROTH's side: the `rothdiff_dump` console command. |
| `rothdiff.py` | `diff`, `probe`, `poses`. |
| `run_rothdiff.py` | Drives both engines over a pose set, prints before/after, exits non-zero on regression. |

## How each side gets its answer

**ROTH.C** is exact and derives nothing. Every span fill inner loop takes
`(count, destination offset)` and writes exactly that many bytes at
`g_render_target_buffer + edi`, so the pixel range a span covers is *handed to
us*. All seven fill loops are hooked and shadow the same range with the surface
identity read from live globals.

**REMAROTH** raycasts, one ray per pixel, through the engine's own `Trace()`.
It reports what the loader actually built — which sector or line is there, and
which texture the loader hung on it. That is the thing under test.

### What rothdiff does NOT test

- **Anything after surface selection**: lighting, fog, translucency, sprite
  sorting. A high match percentage says nothing about these.
- **Walls.** v1 scores **flats only**. ROTH's wall fill word has not been read
  yet, and inventing one would put a made-up number into the file this tool
  exists to trust. Wall pixels are marked out of scope on both sides and are
  **not** counted as matches — scoring them as agreement would inflate the
  percentage with exactly the surfaces the tool is blind to.
- **Per-pixel u,v.** Deriving those means replaying the fills' self-modified
  shift immediates, and that fixed-point split is where this project has already
  produced three wrong constants. Per-span texture state is dumped by
  `oraclelog`'s `flatspans.inc.c` instead, where every term is read.

## Use

```
# 1. capture from ROTH.C -- one headless run per pose
python run_rothdiff.py --capture-roth

# 2. capture from REMAROTH -- one run, all poses
python run_rothdiff.py --capture-remaroth      # writes captures/rothdiff.cfg
doomxr.exe ... +exec <path to captures/rothdiff.cfg>

# 3. compare
python run_rothdiff.py                 # table of before/after, exit 1 on regression
python run_rothdiff.py --save-baseline # bless the current numbers
```

Single pose, single pixel:

```
python rothdiff.py diff a.ridb b.ridb --json report.json
python rothdiff.py probe a.ridb b.ridb 160 100
```

Both engines **must** capture at the same resolution. The tool refuses
otherwise: comparing 640x480 against 320x200 is about a 16% field-of-view
difference, and that was once read as a projection defect.

## The `.ridb` format

```
magic  "RIDB"   u32     0x42444952
version 1       u32
width           u32
height          u32
pose x, y, ang  i32 x3
w*h records of  u16 fill, u16 flags, u16 id, u16 tex
```

`fill` is ROTH's span fill word (span record +0x16): high nibble `0x3x` ceiling
/ `0xbx` floor, bit `0x08` textured, bits `0x06` the mirrors. `flags` is
`g_world_surface_draw_flags`, whose `0x20` / `0x200` bits pick the span driver;
REMAROTH sets it to `0xFFFF` to mark a wall pixel as out of scope.

Captures land in `captures/` and are **not** committed — a stale capture
silently compared against new code is precisely the trap this removes.

## Exit codes

`0` match or better than baseline &nbsp;·&nbsp; `1` regression &nbsp;·&nbsp;
`2` the buffers cannot be compared (size or pose mismatch)

## Tests

`rothdiff.py` was verified against synthetic buffers with four defects injected
(wrong texture, a mirror bit, a missing pixel, a floor drawn as a ceiling); it
names all four causes, holds 100% on a self-diff, refuses a size mismatch with
exit 2, and returns exit 1 against a better baseline.

`test_roth_surface.cpp` covers the surface rules with no map loaded:

```
i686-w64-mingw32-g++ -std=c++17 -O1 -I ../../src/roth \
    -o test_roth_surface.exe test_roth_surface.cpp ../../src/roth/roth_surface.cpp
./test_roth_surface.exe
```
