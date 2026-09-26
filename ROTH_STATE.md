# ROTH work — current state
*Written 2026-09-25. Read this first when picking the work back up.*

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

**Stage 2 of 8 done.** Geometry loads: 507 sectors, 1,608 lines for STUDY1, matching
the Python exactly. Textures, objects, platforms, lighting and logic are all still
ahead.

**Two traps already hit, in case they recur:** `GetChecksum` reads Doom lumps a
Realms map does not have (fixed by hashing the `.RAW`), and command-line arguments in
this fork must be declared with `FARG` rather than passed as strings.

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
- **Doors** are marked by `floorTriggerID` 0xFFFD/0xFFFF and are closed at load.
- **The level logic system is decoded** — see `ROTH_COMMANDS.md`.

**roth-editor approximates** and is wrong about floor anchoring, wall `textureFit`,
and the X mirror. Prefer ROTH.C every time.

---

## 4. Next up: what the arguments point at

The instruction set is decoded but its **operands are not**. "Open door" does not say
*which* door — the arguments index into tables that have not been verified.

Specific questions to answer:

1. **Key field (`+0x08`) on a trigger** — a sector id, face id or object id depending
   on the opcode. Which index space, and is it the raw array index or an id looked up
   by `find_geometry_record` / `find_face_record` / `gather_faces_by_id`?
2. **`cmd_open_door` arguments** — which door, and what do the other operands set
   (speed, wait, direction)?
3. **Sentinels.** `65535` appears as an argument; elsewhere it means "all" or "none".
   Confirm per opcode rather than assuming.
4. **`cmd_set_flag` / `cmd_if_not_flag`** — where does the flag live, how many are
   there, and does it persist across levels?
5. **`cmd_map_transition`** — how a destination map and arrival point are named.

`raw_commands.c` has helpers named `find_geometry_record`, `find_face_record` and
`gather_faces_by_id` which almost certainly answer the first question directly.

**Open from the last pass:** opcode `0x30` sets an object marker nothing was found to
read; 220 triggers against 224 entry points; and 27 "water/lava" triggers in a manor
with no water, suggesting that opcode is a more general sector-link mechanism.
