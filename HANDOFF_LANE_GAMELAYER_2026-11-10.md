# Lane handoff: GAME LAYER, 2026-11-10

Branch `lane/gamelayer-dbase100`, off `workingbuild` at `84a873c`. Not merged and no PR.

**The goal block in `LANE_START.md` still decides what matters.** This lane built the gate for README
section H: a DBASE100.DAT reader. It also built the part of the inventory model the docs state
precisely enough to port. There was no retail data and no engine build in this sandbox. Read §3
before trusting anything here.

---

## 0. THE ONE THING TO DO FIRST

Run the dump tool on the retail `DATA` directory (the folder with `ROTH.RES` and `DBASE100.DAT`, per
`ROTH_BETTER.md` §18). It settles the open questions in §4 in one go.

**Windows (MSVC), from the repo root:**

```
tools\dbase100_test\build.cmd "E:\path\to\retail\DATA"
```

That builds the tools into `build-dxr\dbase100_test\`, runs both test suites, prints the summary dump,
and writes the full dump to `build-dxr\dbase100_test\dbase100_dump.txt`. To re-run the dump on its own:

```
build-dxr\dbase100_test\dbase100_dump.exe "E:\path\to\retail\DATA"
build-dxr\dbase100_test\dbase100_dump.exe "E:\path\to\retail\DATA" --all > dump.txt
build-dxr\dbase100_test\dbase100_dump.exe "E:\path\to\retail\DATA" --item 12 --action 3
```

You can also give it the file itself instead of the folder: `dbase100_dump.exe "...\DBASE100.DAT"`.

**Linux / MSYS (g++):**

```
tools/dbase100_test/build.sh
tools/dbase100_test/out/dbase100_dump /path/to/DATA --all
```

Exit code 0 means `LOADED CLEAN`. 3 means it loaded but counted anomalies, and the dump lists them.
1 means it could not load. The tool writes nothing.

The engine's own selftest now checks DBASE100 as well: `src\roth\build_selftest.cmd "<ROTH folder>"`
expects 281 items, 694 actions and zero anomalies.

---

## 1. What was built

| file | what |
|---|---|
| `src/roth/roth_dbase100.{h,cpp}` | The reader. **Uses only the C++ standard library.** It reads the header, the four tables (inventory, action, cutscene, interface), inventory records with their trigger blocks, action records and the command word. |
| `src/roth/roth_dbase100_install.cpp` | The adapter: `LoadDbase100FromInstall(install, db, log)` and `TheDbase100()`. It uses only `roth::Install` and `roth::Log`, which are both engine-free. |
| `src/roth/roth_install.{h,cpp}` | Adds `Install::DataFile(name)`, a file beside `ROTH.RES`. It tries the name as given, then upper case, then lower case. |
| `src/roth/roth_inventory.{h,cpp}` | The inventory slot model (§2). |
| `src/roth/roth_runtime.cpp` | `BeginLevel` loads DBASE100 once, and the report goes into the first map's log. Nothing reads it yet. Also fixes the map-log line that said no reader exists. |
| `src/CMakeLists.txt` | Adds the three new `.cpp` files. |
| `src/roth/roth_selftest.cpp`, `build_selftest.cmd` | DBASE100 checks are added. |
| `tools/dbase100_test/` | `fixture.h` (byte-level builder), `dbase100_test.cpp`, `inventory_test.cpp`, `dbase100_dump.cpp`, `build.sh`, `build.cmd`. |

**The reader's rules, all taken from the docs:**
- **Ids.** Inventory and action ids are **1-based**: `record(id) = base + table[id-1]`, from the -4
  bias in `HANDOFF_REMAROTH.md` §14. Cutscenes are 0-based, because op 0x07 plays `operand-1`.
  Interface entries are exposed 0-based and raw.
- **The image.** The reader keeps a private, mutable copy. ROTH.C writes into it at run time: item
  `+7` bit 0x80 (examined) and cutscene `+0x13` (gallery). `SetItemRuntime` and
  `SetCutsceneGallery` do the same here.
- **Failures.** A table that does not fit in the file **fails the load**. A fault inside one record
  is **counted by name** in `Anomalies()`, and that record is not served (`Item(id)` returns null,
  `ItemSlot(id)` still shows it). An item record with no size-0 end marker is not served.
- **Notes.** `Notes()` holds the cross-checks that answer the open questions in §4.

## 2. The inventory model, and where it stops

**Built** (`docs/GAME_inventory.md` §3-4 and §9):
- `find_free_inventory_slot`, which **increments the count itself**;
- `is_item_id_pickable`;
- `query_player_inventory` in both modes, including the trap where select mode auto-selects and
  **still returns 0**, the easy flag, and flags&2;
- `remove_item` / `remove_inventory_item`: the hands first, then stack decrement;
- the 0x42 filter: bit 15 hidden, category 2 exempt, both hands cleared;
- `swap`;
- save chunk 7, with the count recomputed on load.

Effects that belong to other systems go through `InventorySink`. Its defaults do nothing.

**Not built, on purpose.** `give_item` is the next thing this needs, and the spec only summarises it:
- the quantity a plain non-stackable item gets;
- what the category-2 early return returns;
- how WeaponAction attrs+4 becomes the ammo;
- the order `give_item_by_dbase_id` searches in.

Each of those is one guess away from 1:1, so **transcribe `give_item` from `E/inventory.c:372-462`**.
Use, combine, the random auto-select (its RNG seed is open), tabs (the 0x7123c bytes are open), the
label, the pickup lock and examine all need the interpreter or code that isn't transcribed.

## 3. How it was verified, and what was not

**Verified here:**
- **The reader.** 233 checks pass (`dbase100_test`), in a plain build and in an ASan+UBSan build. The
  fixtures are built byte by byte from the documented layout and share no code with the reader. The
  checks cover:
  - every header field;
  - the id bias;
  - each kind of structural failure, including table counts that would overflow 32 bits;
  - every named record anomaly;
  - a sweep of **every truncation** of a good file: none loads silently;
  - 20,000 random corruptions under the sanitizers;
  - the install adapter, against a fake `DATA` directory with a lower-case `dbase100.dat`.
- **The inventory model.** 337 checks pass (`inventory_test`, ASan+UBSan), following
  `GAME_inventory.md` §10 tests 3, 4, 5, 9 and 10.
- **The dump tool** ran on the synthetic fixture and reported `LOADED CLEAN`. It ran in both modes,
  folder and `.DAT`.
- **`roth_selftest.cpp`** compiles and links with g++ here, because it is engine-free. It ran against
  a synthetic install.
- **Compiler flags.** All of the above builds with g++ 13 using `-std=c++17 -Wall -Wextra -Werror`.

**NOT verified:**
- **No retail file has been read.** Every layout claim is the docs' claim. §0 is the check.
- **No engine build.** The edits to `src/CMakeLists.txt` and `roth_runtime.cpp` have never been
  through a compiler. The runtime edit is two includes and a three-line call. The CMake edit is
  three source lines placed beside the other readers.
- **`build.cmd` has never been run.** It mirrors `build_selftest.cmd`.

## 4. UNVERIFIED, and how the dump settles each one

| question | where the dump answers it |
|---|---|
| Which header count sizes the flag bitmap? The docs say "433 records", which is neither 281 nor 694 | "flag bitmap size by candidate count" marks any field equal to 433. A guess, not a finding: `+0x0C` is the only unexplained field that could be a count |
| Does item `+0` length include the size-0 end marker? | the note "inventory +0 length: N equal the span through the end marker, M the span before it" |
| Do the `+4`/`+5` "has block" flag bits really mean what the spec says? (some are [inferred]) | the seven "flag ... both / bit without block / block without bit" lines |
| Does ROTH.C's render-id lookup order matter? | "N share a render id". If N is 0, the order is moot |
| Is the "Creator's Staff, index 11" in the BTI docs 0- or 1-based? | spot checks: render 0x294 → id 11 (1-based) or 12 (0-based). Also expect OnInspect `say 0xF38C` and a `0x20` word with operand `0x8002` |
| Does `resolve_dbase100_text` bias the interface index? | `--interface`. Entries 0x28 to 0x2d should read "combined", "can't combine", "can't use", "used" (`GAME_inventory.md` §2.4), or sit one entry off |
| What do records 1-4 contain? (`ROTH_GAME_PORT.md` §7) | printed in full, under "records 1-4" |
| Does `+0x30` equal 2? | "expected by the docs" |
| Upper id bound: is it `> count` or `>= count`? | not answerable from data. Retail keys never reach the edge (`ROTH_COMMANDS.md`) |
| Does ROTH.C check the "DBASE100" signature? | not answerable from data. This reader refuses a file without it |
| Do the cutscene names match `DATA\GDV\*.GDV`? | `--cutscenes` counts names with no file |

**Not done, and noted in the header:** `resolve_dbase100_sound_ids`. ROTH.C rewrites the operands of
sound opcodes {0x19, 0x21, 0x29, 0x2a, 0x31, 0x32} in place at startup (`audio.c:1893-1924`). That
needs the FX bank, so it belongs with step 3 (sound).

## 5. The next step

1. **Run §0** and correct whatever the dump contradicts. If anything in the reader is wrong, it is in
   one of the rows of §4.
2. **The DBASE100 interpreter**, `execute_dbase100_chain` (`ROTH_GAME_PORT.md` §4 step 1;
   `GAME_dialogue.md` §2.3 and `GAME_core.md` §5.4). It reads commands through
   `Dbase100::Action(id)` and `Commands()`. It shares the flag bitmap that `roth_runtime.cpp`
   already has, and that bitmap is still **hard-sized to 448 bits**. Size it from the count §4 names,
   using `Dbase100::FlagBitmapBytes`. After that, `RunDbase100Record` (RAW 0x2b) has something to
   call.
3. **`give_item`, transcribed from ROTH.C** into `roth_inventory.cpp`. After that, RAW 0x29 (187 of
   239 records) and DBASE100 op 0x11.

## 6. Questions for the owner

- **Where does `DBASE100.DAT` sit in your retail `DATA` directory?** It should be beside `ROTH.RES`;
  that is where `Install::DataFile` looks. Is `ROTH.RES` actually in that folder? The dump accepts a
  `.DAT` path as a fallback.
- **Is the ROTH.C source (`E/inventory.c`, `E/dbase100.c`) available to the next lane?** `give_item`
  and the interpreter should be transcribed from it, not reconstructed from the summaries in `docs/`.
