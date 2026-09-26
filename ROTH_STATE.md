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

**Stage 3 of 8 done.** Geometry and textures load: 507 sectors, 1,608 lines and 363
registered images for STUDY1, animated textures included, zero warnings. Objects,
platforms, doors and logic are still ahead.

The stage-3 write-up lives in the commit message; the parts worth knowing here are that
**the 90-degree transpose is free** (GZDoom's paletted pixels are column-major, so handing
the bytes over with width and height exchanged undoes the rotation with no pixel
shuffling), and that **the stored wall extent is newly applied and is NOT covered by the
Python oracle's visual validation** -- `wall_u_repeats` in `build_map.py` turned out to be
dead code, so the oracle's known-good screenshots used a flat 0.5.

**Traps already hit, in case they recur:** `GetChecksum` reads Doom lumps a Realms map
does not have (fixed by hashing the `.RAW`); command-line arguments in this fork must be
declared with `FARG` rather than passed as strings; and the sector `memset` in
`LoadRothMap` left `Colormap.LightColor` black, which rendered the whole level black
except where a dynamic light reached it -- when adding a field there, compare against
`maploader.cpp:1072-1139` rather than trusting zero to be a sane default.

**Build hazard: LNK1103, distinct from the LNK1318 one above.** Symptom is
`<file>.obj : fatal error LNK1103: debugging information corrupt; recompile module` on
whichever file you just edited, and deleting that `.obj` does not fix it -- it recurs
deterministically. Cause is `/Z7` plus **incremental LTCG**: stale `doomxr.iobj` /
`doomxr.ipdb` make the linker say `0 of N functions were compiled, the rest were copied
from previous compilation`, so it never generates code for the changed module and then
rejects its debug info. Fix:

```
rm build-dxr/src/zdoom.dir/RelWithDebInfo/doomxr.iobj build-dxr/src/zdoom.dir/RelWithDebInfo/doomxr.ipdb
```

Rebuild; the log should read `Previous IPDB not found, fall back to full compilation`.
Costs one full LTCG pass, around four minutes. **Editing a source file while a build is
compiling it can seed the bad state**, so do not.

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

## 6. Stages left on the native loader

Stage 2 of 8 is done. Remaining: textures, objects and 3D props, mid-platforms as 3D
floors, doors and lighting, the command system, then standalone. Doors now have their
semantics pinned down; the command system has its operands pinned down. Textures and
objects are next and neither depends on the logic work.
