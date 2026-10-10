# Where the game port actually stands — measured 2026-10-10

**Read `LANE_START.md`'s goal block first. This file answers one question:**
of `ROTH_GAME_PORT.md` §4's ten build steps, which exist in the code?

Measured by reading the tree, not the documents — the documents disagreed with
each other, which is what made this worth doing. Every verdict below is a name
that is either present in the source or absent from it.

---

## The short answer

**The game port is at step 0, and step 0 has not been started.** What exists is
the *loader* (reading the player's RAW/DAS files and building a level) plus a
real but partial command interpreter. Everything from step 2 onward — player,
sound, dialogue, cutscenes, inventory, weapons, monsters, saves — is absent.

| § | step | state |
|---|---|---|
| 0 | foundations: `RealmsState`, mutable raw image, `Core_AdvanceTicks`, `IEngineSink` | **not started** — none of those names exist anywhere in `src/` |
| 0 | fix the trigger bindings | **done** (2026-09; `ROTH_GAME_PORT.md` §1's banner is right) |
| 0 | the side-by-side comparison rig | **mostly done** — `tools/rothdiff` + `tools/oraclelog`, see `LANE_START.md` §5 Step 1 |
| 0 | lighting on | **done** |
| 1 | the command interpreter, ≈4,000 lines | **part built, in the wrong shape** — see below |
| 2 | the player: movement, collision, cursor, map transitions, ≈2,500 | **not started** — no `RealmsPlayer`, no ported movement |
| 3 | sound and music, ≈750 | **not started** — no SFX bank, no ambient nodes, no music hook |
| 4 | text, speech, dialogue, ≈1,450 | **not started** — no DBASE400/500 reader, no dialogue queue |
| 5 | cutscenes, the GDV codec, ≈750 | **not started** — no `GdvPlayer` |
| 6 | inventory and examine, ≈800 | **not started** — no DBASE100 item reader at all |
| 7 | weapons and player health, ≈900 | **not started** |
| 8 | monsters and projectiles, ≈1,500 | **not started** — no actor or projectile pool |
| 9 | saves, menus, HUD, game flow, ≈1,500 | **not started** |

So of §4's ≈15,000 lines of *rule* logic, what is written is the part of step 1
inside `roth_runtime.cpp` (4,876 lines total, much of it binding and dispatch
rather than rules).

**What IS built, and is not on this list, is the loader** — `roth_raw`,
`roth_das`, `roth_texture`, `roth_surface`, `roth_objects`, `roth_palshade`,
`rothmap.cpp`: ~11,700 lines in `src/roth/` plus 2,387 in
`src/maploader/rothmap.cpp`. That is why a map loads, renders and runs at 60 fps
with no game logic behind it. Do not read "step 2 of 8" (the loader's own
stages, `ROTH_STATE.md`) as "step 2 of 9" here; they are different ladders.

---

## The decision this surfaces, and it is the owner's

`ROTH_GAME_PORT.md` §2 asks for **one Realms core** in `src/roth/core/` that
owns all game state and all rules, with the engine as a **mirror** driven
through a single `IEngineSink`, and presentation talking to it only through
intents and queries. The stated reason is that two authorities over the same
puppet is the main risk in the design, and that the core/presentation split is
what makes VR a swap rather than a rewrite.

**That core does not exist.** `RealmsState`, `Core_AdvanceTicks` and
`IEngineSink` appear nowhere. The rule logic written so far lives in
`roth_runtime.cpp` and works directly against engine structures — the shape the
plan explicitly set out to avoid.

Two ways forward, and they cost very differently:

1. **Do step 0 first.** Build the core, move what exists in `roth_runtime.cpp`
   into it, then port steps 1-9 into it. Expensive up front, and it is what
   makes the later steps diffable against ROTH.C tick by tick (§5), which is
   how "1:1" is supposed to be proven at all.
2. **Keep growing `roth_runtime.cpp`.** Cheaper now. Gets harder at every step
   that needs state the engine does not have a place for — inventory, the flag
   bitmap, dialogue queues, the actor pool — and gives up the side-by-side log
   rig, so 1:1 becomes a judgement instead of a diff.

**This has not been decided.** It should be, before step 1 is finished, because
step 1 is the thing that would have to move.

---

## Step 1 in detail — what the interpreter already does

Present in `roth_runtime.cpp`:

- chain walking and execution (the coder's `WalkChainFlow` / `ExecChain` spine,
  which §2.1 says to keep)
- key resolution, face↔side mapping, 1:1 sector identity
- trigger binding and dispatch with the correct event per opcode, all four
  reachable channels verified firing (`docs/REMAROTH_MEASURED.md` §11)
- the `0x3088c` per-frame effect tick table and the `0x30998` load-time init
  table are both referenced and partly walked
- hinged door swings (`SwingDoor`) — doors open from the use key
- scripted map transitions — entering STUDY1's sector 174 warps to STUDY3
- unhandled opcodes are **counted and reported**, not silently skipped

Known gaps inside step 1:

- **the object channel does not exist.** Opcodes `0x08` (click an object) and
  `0x30` (examine an object) reach `unclassified++` and `EventForOpcode`
  returns `None`. This is why **light switches authored as objects cannot
  work**, and it is the owner's own second example of what he wants working.
  One authored as a wall face should already work.
- **the wall-band gate never runs.** `roth::ActivateLine` has a single caller
  (`p_spec.cpp:141`) which passes `nullptr`, so the band is always 0. Turning
  it on needs a real hit position from a dedicated pick — and will start
  *refusing* triggers that fire today, so it is a measured decision.
- doors only open; closing needs the door's own state word
  (`toggle_door_open_state` toggles, the port only opens)
- no DBASE100 interpreter, so every opcode that calls into dialogue or items
  has nothing to call

---

## What this means for "playable start to finish"

The goal needs steps 1, 2, 4, 6, 7, 8 and 9 at minimum — dialogue and
inventory are not optional in Realms, and step 9 is what makes a playthrough
survive being switched off. Sound (3) and cutscenes (5) are the only ones a
first playthrough could technically do without.

The honest read: the loader is in good shape and the hands now run, so the
engine half is further along than the game half by a wide margin. Nothing in
the game half is blocked on the engine. It is blocked on the step 0 decision
above.

---

*Method: every "not started" above is a grep for that system's own names across
`src/` and `wadsrc/static/zscript/` returning nothing — not an inference from a
document. `src/roth/` is 11,765 lines, `rothmap.cpp` 2,387, and the ZScript side
is one file (`loaderprop.zs`). If a later reader finds one of these systems, the
grep was wrong and this file should be corrected in place.*
