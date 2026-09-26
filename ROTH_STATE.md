# ROTH work — current state
*Written 2026-09-25. Read this first when picking the work back up.*

**What this is.** ROTH.C calls itself a modern implementation of the original game.
So is this -- the difference is that it runs inside the GZDoom VR fork, so it inherits
the VR mechanics already built there. The project is **ROTHxr** / **VRealms**, not
UZDXREMA. ROTH.C is the specification: where it and the fork disagree, ROTH.C wins and
the engine changes to suit. What we learn here is meant to flow back into the main
engine, so additions belong in it as general capabilities, not as Realms special cases.
**`ROTH_BETTER.md` is the living record of those** -- things Realms does that we could do
differently or better. Append to it whenever one turns up; do not let it go stale.

Two efforts exist side by side. **The native loader is the destination; the Python
pipeline is now a test oracle, not the product.**

---

## 1. Native loader — the real path

The engine opens the player's own Realms install and builds the level in memory.
No converted files, nothing generated, nothing shipped.

**Working today:**

```
doomxr.exe -iwad <any iwad> -rothpath "D:\...\Realms of the Haunting\ROTH" +map STUDY1
```

The path must be quoted — it contains spaces, and an unquoted one silently truncates
at the first space. `roth_path` also exists as an archived cvar.

That produces `roth_STUDY1.log` beside the exe: a full report of what was opened, what
was in it, stage timings, and counters for anything unhandled.

**Code, all new:**

| File | Does |
|---|---|
| `src/roth/roth_raw.h/.cpp` | `.RAW` map reader. Bounds-checked throughout. |
| `src/roth/roth_das.h/.cpp` | `.DAS` artwork: plain, animated (delta decoder), image packs, 3D meshes |
| `src/roth/roth_palette.cpp` | the built-in fallback palette |
| `src/roth/roth_install.h/.cpp` | finds the install, reads `ROTH.RES` |
| `src/roth/roth_log.h/.cpp` | per-load diagnostics |
| `src/roth/roth_selftest.cpp` | standalone checks — **not** part of the engine target |
| `src/maploader/rothmap.cpp` | `LoadRothMap` — builds sectors, lines, sides |

Touched: `p_openmap.cpp` (detection, the `rothpath` argument, checksum),
`p_setup.h` (`MapData::isRoth`), `maploader.cpp` (the format branch),
`maploader.h`, `src/CMakeLists.txt`.

**Verify the readers any time:**
```
src\roth\build_selftest.cmd "D:\...\Realms of the Haunting\ROTH"
```
Builds out of tree and checks against known-good totals: 44 maps, 16,906 sectors,
82,210 faces, 5,324 objects, 2,299 mid-platforms, zero open loops. It must print
ALL CHECKS PASS. **Never build it beside the sources** — stray object files make the
engine build fail with a PDB error.

**Stages 1-5 done, stage 6 part-done.** STUDY1 loads from the retail install with
geometry, textures, 242 objects (199 sprites, 43 real 3D meshes), 15 mid-platforms as
3D floors, and 25 doors whose hinges are all resolved. Zero warnings.

The stage-3 notes worth keeping: **the 90-degree transpose is free** (GZDoom's paletted
pixels are column-major, so handing the bytes over with width and height exchanged undoes
the rotation with no pixel shuffling), and **the stored wall extent is NOT covered by the
Python oracle's visual validation** -- `wall_u_repeats` in `build_map.py` is dead code, so
the oracle's known-good screenshots used a flat 0.5.

---

## 2. Python pipeline — the oracle

`roth_pipeline/` converts maps to loadable `.pk3` files. It is **superseded** as a
product but remains valuable: it is proven across all 44 maps and its numbers are what
the C++ is checked against.

```
python build.py --roth "<...>/ROTH" STUDY1      # or --all
```

`generated/` is disposable and gitignored. `authored/` is hand work a rebuild must
never touch.

Its output looks close to the original: geometry, textures, 259 of 274 objects
including 47 real 3D furniture pieces, doors that open, animated textures.

---

## 3. What is verified about the format

Everything here came from reading ROTH.C, not from inferring. See
`ROTH_CONVERSION_PLAN.md` and `ROTH_COMMANDS.md` for the detail.

- **Wall art is stored rotated 90°.** Transposing it was the single biggest visual
  improvement. Flats transpose too. Sprites and model skins do not.
- **Two world units per texture pixel.**
- **The player is 144 units**, not the 72 in the map file — the original doubles it at
  load. Getting this wrong makes the whole world read as twice its size, and is a
  player bug, *not* a reason to rescale geometry.
- **Player facing is 512 units per turn; object facing is 256**, opposite sense.
- **Flats scale per sector** from bits 4-5 (floor) and 2-3 (ceiling): 2^s units/texel.
- **3D prop vertices are (x, up, y)** — the middle value is vertical. Do not mirror X.
- **Doors** are closed at load, and are **hinged walls that swing to a target point**,
  not floor or ceiling movers -- `cmd_open_door` resolves a face and gives it a target
  vector. Three sentinels in the sector-ID space, not two: `0xFFFD` secondary pool,
  `0xFFFE` door-capable with the wall undrawn, `0xFFFF` primary pool. Doors spawn in
  pairs for double doors.
- **A command's key is an ID, not an index** -- see `ROTH_COMMANDS.md`. The field we
  called `floorTriggerID` is the sector's `commandID`; walls are named by a two-hop
  lookup through `faceID` on the extended texture-map record.
- **The level logic system is decoded** — see `ROTH_COMMANDS.md`.

**roth-editor approximates** and is wrong about floor anchoring, wall `textureFit`,
and the X mirror. Prefer ROTH.C every time.

---

## 4. The arguments: answered

Done, 2026-09-25, and written up in `ROTH_COMMANDS.md` -- the sections "what the key at
`+0x08` actually names", "`cmd_open_door`", "flags, items and dialogue" and
"`cmd_map_transition`". The short version:

1. **The key is an ID that gets searched for, never an array index.** Sectors match on
   `commandID`; walls take **two hops** -- the `faceID` on an extended texture-map record,
   then the face pointing at that record. `key == 0` means "the thing the player just
   used", which is why one command record can serve many doors.
2. **`cmd_open_door`** names a face, a swing extent, a sound id (plus one; `0` is silent)
   and a **target vector** the wall moves toward.
3. **`65535` never appears as a key** in any of the 44 maps. It is an argument value only.
4. **Flags are DBASE100 record ids** in a global 448-bit bitmap that is saved, so they
   persist across levels.
5. **Map transitions** store an 8-byte NUL-terminated ASCII map name plus an arrival point.

Checked against all 44 retail maps: geometry opcodes resolve almost perfectly (`0x2f`
open-door: 116 records, **zero** unresolved), and the logic opcodes' key ranges each land
just inside the DBASE100 table they belong to.

**Two keys still unidentified:** `0x2d` particle effect (24 distinct values, 102-791) and
`0x0e`/`0x0f` texture scroll (values to 16898, so `+8` is probably not a key at all).

**Also still open:** opcode `0x30` sets an object marker nothing was found to read; 220
triggers against 224 entry points in STUDY1.

---

## 5. What the door finding costs us

Realms doors cannot be GZDoom door sectors. They are wall geometry moved toward a stored
target point, which the fork has no equivalent for. This is the first place the native
loader needs a real engine addition rather than a translation -- and it is worth building
as a general "move this wall toward a point" capability, since UZD mapping has no such
thing either.

---

## 6. Where it actually stands (2026-09-26, end of session)

**Done:** readers (1), geometry (2), textures (3), objects and 3D props (4),
mid-platforms as 3D floors (5).

**Half:** stage 6 (30 door panels built and hinged; nothing opens them; lighting
transcribed but switched off) and stage 7 (logic parses, every key resolves, all
1,937 chains execute -- but no handlers, so nothing happens yet).

**Stage 8 is close and blocked** -- see 6b below.

**Doors.** A Realms door is a four-walled slab that swings about a hinge corner toward a
stored target point. The hinge rule is **verified across all 44 maps: 141 door sectors,
every one with exactly four faces, every one with exactly one hinge** -- the face whose
mapping record is extended and whose `faceID` is a door sentinel. In STUDY1 all 25
resolve, and 14 build as polyobjects using the zero-delta anchor trick (spawn spot and
anchor both on the hinge, so `TranslateToStartSpot` moves nothing and the polyobject is
built in place -- no void room, no engine change).

The other 11 fail because **sister-face merging can collapse two of a slab's four faces
into one line**, so the door comes up short of four. That is the next door job.

**THE DOOR WORK WAS REVERTED** (commit `e1f7ba66ba`). Building the slabs as polyobjects
tore the geometry apart: GZDoom pulls polyobject lines out of the BSP and renders them
specially, and a Realms slab's faces are TWO-SIDED -- shared with the rooms either side.
Two-sided polyobject lines are not something the renderer copes with, so doorways and
whatever the BSP was splitting with those lines both broke. Un-flattening the leaves at
the same time made it worse.

The hinge detection is KEPT and is correct: it is pure data, 25/25 in STUDY1 against
141/141 across all 44 maps.

**The next door job is geometry, not tagging:** a slab has to become four ONE-SIDED lines
of its own before it can be a polyobject. This was flagged earlier in the session ("a
two-sided polyobject line is trouble in GZDoom") and then built anyway without solving it.
Do not simply re-apply the revert.

**Nothing opens them yet.** That needs stage 7.

---

## 6b. Stage 8 -- standalone: close, blocked on visibility

`vrealms_iwad/` is the game package: a marker lump, an IWADINFO, a generated
PLAYPAL and COLORMAP, and an additive MAPINFO. **It carries nothing from Realms**
-- the palette is a neutral colour cube of our own, and the real one is still read
from the player's `.DAS` at map load.

```
doomxr.exe -iwad vrealms.pk3 -rothpath "<install>" +map STUDY1
```

**How far it gets:** the engine boots on that package alone, identifies it as
VRealms, and finds all 44 Realms maps in the install. It then **halts inside base
MAPINFO parsing**, before any map loads, with no error on stdout or stderr --
GZDoom reports fatal startup errors in a modal dialog.

**Already ruled out, so do not repeat these:**

- A palette IS required. Without `PLAYPAL`/`COLORMAP` the engine stops silently
  right after `W_Init`. Generating them got us past that.
- **Our MAPINFO is not the cause.** It halts identically with ours, with the
  engine's baseline copied in, and with no MAPINFO at all.
- **The `Mapinfo =` field is not the cause.** Same halt with and without it.
- One real error was found and fixed on the way: copying the engine's own
  `mindefaults.txt` in as our MAPINFO produces *"MAPINFO file is processed more
  than once"*, because it `include`s files the engine has already read. **Ours
  must be additive only** -- a `defaultmap`, a `map` entry, a `clusterdef`, and
  nothing included.

**The blocker is that the engine will not say what it objects to.** The next step
is to make that message reachable -- get `I_FatalError` into the log, or bisect
the baseline mapinfo -- rather than keep guessing at MAPINFO contents, which is
what stalled this.

---

## 7. Open, with what is known about each

- **`roth_objectangle`** -- object facing is a cvar because three attempts to deduce the
  constant from descriptions gave three contradictory answers, each fixing one piece of
  furniture and breaking another. Dial it in game, reload the map, bake in the number.
  ROTH.C's own formula is `angle512 = 2 * (rotation + 0x40) - viewAngle`.
- **Textures that do not line up.** ROTH.C has two extent paths and the computed one reads
  a byte of **repeat nibbles**: high nibble multiplies the horizontal extent by `1+n`, low
  by `1+n` vertically (`renderer.c:13345-13358`). We apply neither. Traced as far as
  `renderer.c:13115`, which reads it from a RUNTIME surface record at `+0xf`; the field it
  comes from in the file is not yet found. **It is not the texture-map record's high byte
  -- that is zero in all 910 of STUDY1's mappings.** Vertical `FF_IMAGE_FIT` is now solved
  (903 unfitted pieces down to 662, and those have no gap to fill).
- **"One suit of armour is massive."** Every sprite measures sanely (tallest 175 against a
  154-unit player) and mesh `4109` -- the detailed one, 97 vertices -- is 150 tall, which
  is right. The only outsized props are `DEMO[4123]` and `[4128]`: **542 tall, 460 wide,
  420 deep, 14 vertices, 12 faces**. The file header's bounding box agrees, so the parse is
  not wrong. Either those are genuinely large furniture, or mesh units are not 1:1 -- and
  note the 1:1 claim came from **roth-editor**, not ROTH.C.
- **Lighting is deliberately OFF.** `ROTH_LIGHTING.md` has the whole model transcribed and
  both engine pieces are in place but inert while `ShadeFalloffShift` is zero. Turning it
  on is two lines. It is off because a faithfully dark manor is unusable to work in.
  Also found: GZDoom's Build mode was being distance-fogged ON TOP of its own shading,
  which is half of why the first attempt was unreadable.
- **Parallax sky.** `FF_EDGE_MAP` does not mean "this wall is sky" -- the face draws its own
  texture and the sky fills the open region ABOVE the wall top. Modelling it needs the
  wall's own top height, which the loader does not carry: we build every wall
  floor-to-ceiling, so there is no region above it. That is geometry work, not texturing.

---

## 8. Two build traps that will cost an hour each

- **LNK1318** from stray `.obj` files beside the sources. Never build the selftest in tree.
- **LNK1103**, "debugging information corrupt", from stale incremental LTCG. Deleting the
  `.obj` does NOT help -- delete `build-dxr/src/zdoom.dir/RelWithDebInfo/doomxr.iobj` and
  `doomxr.ipdb`. Editing a source file while a build is compiling it seeds this.

Desktop shortcuts **ROTHxr - STUDY1** and **ROTHxr - CHURCH1** launch the current build.
