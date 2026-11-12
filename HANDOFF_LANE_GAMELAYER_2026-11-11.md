# Lane handoff: GAME LAYER, 2026-11-11

Branch `lane/gamelayer-dbase100`. Supersedes `HANDOFF_LANE_GAMELAYER_2026-11-10.md` wherever the
two disagree; that file's §2 ("not built, on purpose") and §4 (the open questions) are now history.

**Read §1 first. This branch is built on a `workingbuild` that is a month stale.**

---

## 1. THE BRANCH POINT IS WRONG, and what that means

This lane branched from GitHub's `workingbuild` at `84a873c` (2026-10-09). The owner's local
`E:\DOOMWork\REMAROTH` is on `workingbuild` with **about 25 commits GitHub has never seen**, all
from 2026-10-10: the pick and the object channel, M0 (the tree links, `FORCE_NO_LTO`), the player's
own radius, reach and collision height, the census fix, `ROADMAP.md`, and the rendering diagnosis
(`RENDERING_NEXT_STEP.md`). "Only the owner pushes", and they were never pushed.

So:

- **`ROADMAP.md` is the plan, and it is not on this branch.** It exists only locally. This lane's
  work is its M2, plus the rule halves of M4 and M5.
- **`src/roth/roth_runtime.cpp` here is the old one.** Do not wire the game core into the copy on
  this branch. Wire it into the local one, after the rebase.
- Everything this lane added is in NEW files, so the rebase should be small: three lines in
  `src/CMakeLists.txt`, a few in `roth_runtime.cpp` (two includes and the `BeginLevel` load),
  `roth_install.{h,cpp}`, `roth_selftest.cpp`, `build_selftest.cmd`, `.gitignore`, and one include in
  `roth_objects.h`.

**THE EXACT NEXT STEP:** the owner pushes local `workingbuild`. Then rebase this branch onto it,
run `tools/dbase100_test/build.sh` and `tools/roth_syntaxcheck.sh`, and only then start §5.

---

## 2. What exists now

All engine-free (C++17, standard library only), all in `src/roth/`:

| file | what | lines |
|---|---|---|
| `roth_dbase100.{h,cpp}` | the DBASE100.DAT reader | (2026-11-10, one fix) |
| `roth_dbase100_interp.{h,cpp}` | `execute_dbase100_chain`, the action queue, the choice builder, the flag bitmap | 1,434 |
| `roth_inventory.{h,cpp}` | `give_item`, query, remove, use, combine, hands, examine, the filter, labels, saves | 1,790 |
| `roth_gamecore.{h,cpp}` | the two joined: each one's sink answered by the other, one shared RNG | new |

`roth::GameCore` is `ROADMAP.md` M2's "the core, but only for new state" in its first form. It is in
`src/roth/`, not `src/roth/core/`; move it when convenient, nothing depends on the path.

Tools: `tools/dbase100_test/` (five test programs and the dump), `tools/roth_syntaxcheck.sh`,
`tools/claude_bridge/`.

## 3. Verified, and how

Run in a Linux sandbox against the owner's **Steam retail install** (`...\Realms of the Haunting\DATA`):

| check | result |
|---|---|
| `dbase100_test` | 241 checks, plain and ASan+UBSan |
| `dbase100_dump` on retail | exit 0, `LOADED CLEAN`, 0 anomalies |
| `interp_test` | 1,275 checks, both builds; every retail action (694) and item block (497) run |
| `inventory_test` | 4,707 checks, both builds; all 281 retail items given |
| `gamecore_test` | 19 checks: new game through the real interpreter into the real inventory |
| `roth_selftest` on retail | 44 maps, 16,906 sectors, 5,531 commands, 82,210 faces: ALL CHECKS PASS |
| `roth_syntaxcheck.sh` | all `src/roth/*.cpp` parse and type-check against the engine headers with g++ |

**A new game, by the original's own script (action 3):** plays CHPTR1, starts music chunk
`0x293cad`, and gives Adam Randall (68), Shards and Seals (1), the Lamp (15) and the Matchbox (16).
The Matchbox ends in the right hand.

**NOT verified, and nothing here should be read as if it were:**

- **No engine build.** `roth_syntaxcheck.sh` is not a build: nothing links, and it is g++, not MSVC.
- **No side-by-side run against ROTH.C.** Correctness rests on reading the source and on tests
  written from that reading. The oracle rig (`tools/oracle`) has not been pointed at any of this.
- **Nothing in the engine calls the core.** `RunDbase100Record` is still the stub. The census has
  not moved: this lane changed no dispatcher code.
- **`tools/claude_bridge/CLAUDE_BRIDGE_REMAROTH.ps1` has never run.** There was no PowerShell.

## 4. What the retail file and the source settled

- Header `+0x0C` is 433 and sizes the flag bitmap: 56 bytes, the 448 bits the runtime hard-codes.
- Item `+0` length includes the end marker (281 of 281). All seven "has block" flag bits agree with
  the blocks present.
- **Ids:** items and actions are 1-based; `id == count` is valid (the bound is `id > count`,
  dbase100.c:549). The Creator's Staff is id 12; the docs' "index 11" is 0-based.
- `resolve_dbase100_text` does not bias its index. Interface 0x28-0x2a read "Using %s with %s.",
  "I cannot use %s with %s.", "I cannot use %s."
- **27 action records have a zero length word** and are deliberate stubs. The interpreter's loop is
  do-while, so the original reads the one word after each. Maps call three of them (281 from CAVERNS,
  550 from RAQUIA5, 603 from RAQUIA1); in each the extra word is harmless.
- **Action 543 sits in the last 4 bytes of the file**, so its extra word is past EOF. No map calls it.
- **SOULST3 has a `0x2b`/`0x36` with key 0.** In ROTH.C id 0 passes the signed bound and reads the
  table one entry low. Not yet looked at: decide what key 0 means there before M5 is called done.
- **The RNG is not seeded from a clock.** Its state is initialised data (`0xB785`) and is shared by
  the inventory's auto-select and the interpreter's op 0x0b. One instance, in `GameCore`.
- 162 item records share a render id, so the order `give_item_by_dbase_id` searches in matters.

## 5. Next, in order (this is ROADMAP M4 and M5's engine half)

1. **Rebase** (§1).
2. **One `GameCore` in the runtime**, attached in `BeginLevel` where `TheDbase100()` is loaded today.
   Move the runtime's flag bitmap into it: RAW `0x26`/`0x28` call
   `Interp().Dbase100BitmapTestSet/Clear` and `TestDbase100RecordFlag`.
3. **RAW `0x2b` and `0x36`**: `RunDbase100Record` becomes `core.RunAction(id)`. Update
   `IsImplemented()` and the census table together (`ROADMAP.md` §4 step 3).
4. **RAW `0x29`, `0x27`, `0x2a`, `0x42`**: `Items().GiveItem`, `Query`, `RemoveItem`, `SetFilter`. These
   handlers own `g_last_item_record` and the chain-interrupt results; transcribe them from
   `raw_commands.c`, do not infer them from the model.
5. **A host sink that shows text.** `ReadNextDialogueLine` must return the DBASE400 line. Until it
   does, do not run hub dialogue: with no text there are no visible choices, and eight menu actions
   loop (583 598 604 605 606 613 616 620). The simplest host prints to the console; the panel comes
   after.
6. **The interpreter's back-calls into RAW**: `RunObjectCommandsById` (op 0x36) and
   `ResetCommandChainNoSource` (op 0x23) go to the runtime's chain runner.
7. Then the census, before and after.

## 6. The bridge

`tools/claude_bridge/` is a copy of the owner's request/response bridge for this repo alone: its own
request folder, this repo's `build-dxr`, and never the shared `doomxr.ini`. A copy installed outside
the repo (`E:\DOOMWork\tools\claude_bridge_remaroth\`) reads `repopath.txt`, so starting it does not
depend on which branch is checked out. Actions: `build`, `packiwad`, `selftest`, `dbtest`, `run`,
`git` (no push), `readfile`.

**`run` launches the game. Clear it with the owner first, and never while he is in the headset**
(`ROADMAP.md` §4).

## 7. Unchanged from before, still true

- The reader refuses a file without the "DBASE100" signature; whether ROTH.C checks is unknown.
- `resolve_dbase100_sound_ids` is not ported: op 0x19 operands are the raw file values until the FX
  bank is read (sound, M8).
- The 173 cutscene names have not been compared with the 164 `.GDV` files on disk.
