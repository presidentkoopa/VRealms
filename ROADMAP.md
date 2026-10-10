# REMAROTH — the road to playable

**Written 2026-10-10.** This is the plan. If you are on the coding team, start
here, follow the milestones in order, and stop reading other documents until a
milestone sends you to one.

> **The goal, the owner's words:** Realms running in VR with sprite weapons and
> the WorldHands beam, playable start to finish.
>
> **The line that settles arguments:** the **logic** is 1:1 with the original —
> what fires when, items, dialogue, flags, combat, saves. **Presentation and
> interface are free** to be better, but not broken. (`ROTH_BETTER.md` §13)

---

## 1. Where we are, in numbers

The **loader is done**: maps, art, lighting, doors, triggers, the hands, 60 fps
in the headset. The **game is not**. Measured today over all 44 retail maps
with `tools/rothdiff/opcodecensus` (its table brought up to date with the
dispatcher):

**Of 3,588 command records that have to do something, 1,879 run (52.4%).**
The 1,709 that do not, grouped by the system they need:

| system | opcodes | records | maps (at least) | in STUDY1 |
|---|---|---|---|---|
| **Dialogue** | 0x2b | **371** | **all 44** | 48 |
| **Inventory** | 0x29, 0x27, 0x2a, 0x42 | **458** | 34 | 74 |
| **Actors: spawn, change, kill** | 0x3c, 0x16, 0x3a | **481** | 28 | 62 |
| Counters / loops | 0x1e, 0x15, 0x1f, 0x22 | 105 | 15 | 11 |
| Sound nodes | 0x10 | 94 | 22 | 19 |
| Player effects | 0x33, 0x3f, 0x35, 0x41 | 81 | 24 | 5 |
| Particles | 0x2d | 76 | 17 | 9 |
| Moving sectors | 0x09 | 32 | 5 | 0 |
| Texture odds | 0x2e, 0x1c, 0x20 | 11 | 7 | 4 |

STUDY1, the first map, is at **47.4%: 209 of 441 live records run**, and the
232 missing are exactly the rows above.

**Dialogue and inventory together are half of everything missing, and both read
`DBASE100.DAT`, which nothing in the tree can read yet.** That file is the
keystone. Actors are the other big block, and monster stats come from DBASE100
too.

---

## 2. Decisions — made, so nobody has to ask

| Question | Decision |
|---|---|
| **The Realms core (`ROTH_PORT_STATUS.md`'s open question)** | **Build it, but only for new state.** `src/roth/core/` gets a `RealmsState` with no engine headers, and every new system lives there: the DBASE100 reader, the flag bitmap (moved in), inventory, the dialogue queue, then actors. The existing world code in `roth_runtime.cpp` (doors, lights, heights, textures) **stays where it is** and moves only when someone has to change it anyway. That gets the diffable state the 1:1 proof needs, without a big refactor that stalls everything again. |
| Player movement (`ROTH_GAME_PORT.md` step 2) | **VR locomotion stays the engine's**: it is interface. Port the parts that are rules: use range, fall damage, hazards, crouch and step heights. Record this in `ROTH_BETTER.md`. |
| Interface for inventory and dialogue | **The simplest VR panel that works**, attached to the hand or floating in front of the player, driven by the beam. The trenchcoat inventory stays parked until the game is playable (`ROTH_BETTER.md` §15). |
| Visual polish (the right-wall lighting, sky column, bay pane, storm glow, door art) | **Parked.** Everything is listed in `LANE_START.md` §5 Step 2. Touch an item only if it blocks play. |
| Order | **The milestones below, in order.** Two of them can run alongside each other; §4 says which. |

The owner can overrule any line. Until he does, it stands.

---

## 3. The milestones

Each milestone ends with a **headset check**: a short list the owner can do
himself in ten minutes. A milestone is done when he has done it, not when it
compiles.

### M0 — The tree builds again *(hours)*

The tree **does not link right now**: `LNK1103` on `roth_runtime.obj`
(`build-band.log`).

1. **Turn link-time optimisation off for the working build.** It is switched on
   by the top-level `CMakeLists.txt` (`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO`)
   unless `FORCE_NO_LTO` is set. Its incremental database is what keeps
   corrupting. Reconfigure `build-dxr` once with `-DFORCE_NO_LTO=ON`, then do one
   full build. The `/INCREMENTAL:NO` already applied is not enough on its own.
   Expect a slightly slower exe and much faster, reliable links. If that build
   *still* hits LNK1103, delete `build-dxr\src\zdoom.dir\RelWithDebInfo\doomxr.iobj`
   and `doomxr.ipdb` and record that it happened.
2. **The uncommitted band-gate count in `roth_runtime.cpp`.** Build it. If the
   load report prints the count, commit it. If not, revert the file
   (`git checkout -- src/roth/roth_runtime.cpp`). Don't leave it loose.
3. Confirm by the **exe's timestamp**, not the exit code.
4. Read the load results in `doomxr-log.txt`. `roth_<MAP>.log` is currently
   truncated by a second load (`HANDOFF_SESSION_2026-10-10.md` §8). Fixing that
   truncation is a ten-minute job worth doing here: open the report once per
   *real* load, not per call.

**Done when:** a fresh `doomxr.exe` loads STUDY1, and the band count is either in
the report or gone.

### M1 — Point and press *(owner, one headset session)*

Already built (`84d54c5c33`), never run. Launch with **`RothHands.cmd`**, then
`roth_pick 1 ; roth_useray_debug 1 ; roth_trigger_debug 1`
(`HANDOFF_SESSION_2026-10-10.md` §3).

**Headset check:**

- [ ] point at a door across the room, press: it opens
- [ ] point down at a floor plate, press: it fires
- [ ] a light switch: the light changes and the switch art flips
- [ ] the beam you see and the thing that reacts are the same thing
- [ ] note any trigger that worked before and now refuses (that is the band gate; M0's count says how many to expect)

Fix what fails. Then, and only if the beam and the pick disagree, do
`HANDOFF_SESSION_2026-10-09.md` §4 step 3 (one hand ray per tic). Once the pick
is trusted, do step 5 (delete the three old half-measures).

**Done when:** all five boxes are ticked.

### M2 — The core and DBASE100 *(no visible change; the keystone)*

1. `src/roth/core/`: `RealmsState`, with names taken from ROTH.C's `g_names.h`
   (`ROTH_GAME_PORT.md` §2.1). 16-bit and 16.16 integers, as the original.
   **No engine headers in this folder.**
2. **The DBASE100 reader.** Read the file from the retail install
   (`-rothpath` at the retail `DATA`, `HANDOFF_SESSION_2026-10-09.md` §8), in
   memory only, never cached. Inventory and dialogue tables, **ids 1-based**:
   the table pointer is pre-biased by −4 (`HANDOFF_REMAROTH.md` §14, the first
   item). Spec: `docs/GAME_inventory.md` §2, `docs/GAME_dialogue.md` §2,
   `docs/GAME_core.md` §5–6.
3. Move the flag bitmap into `RealmsState`; `0x26`/`0x28` call it there.
4. **A test that can fail:** dump record counts and a few known records, and
   check them against the file: 433 records, 281 items, 694 dialogue entries
   (`ROTH_COMMANDS.md`). Then break the −4 bias on purpose and watch the test
   fail.

**Done when:** that test passes, and fails with the bias removed.

### M3 — Counters *(a day; can run alongside M2)*

`0x15`, `0x1e`, `0x1f`, `0x22`: 105 records of pure control flow, no new
system. Spec: `docs/GAME_core.md` §5. Re-run the census afterwards.

**Done when:** the census shows them implemented, and a STUDY1 chain that uses
one runs to the end with `roth_trigger_debug 1`.

### M4 — Inventory *(after M2)*

Follow `HANDOFF_REMAROTH.md` §14's implementation table in its own order
(steps 0, 1, 3–8). Read its warning first: every earlier audit of
`docs/GAME_inventory.md` invented errors that were not there. Interface: a beam
panel listing what you carry. Selecting an item makes it the right-hand item,
which is what `0x27` tests.

**Headset check:**

- [ ] pick up an item in STUDY1: it appears in the panel
- [ ] the locked door that needs a key refuses without it, and opens with it selected
- [ ] the "first click auto-selects, second click works" behaviour matches the original (`ROTH_GAME_PORT.md` §4 step 6)

**Done when:** those pass, and the census shows 0x29, 0x27 and 0x2a implemented.
Step 9 in §14 (key-0 `0x29`) waits on the object channel, which now exists, so
check it as well.

### M5 — Dialogue *(after M2; can run alongside M4)*

The DBASE100 dialogue interpreter (`eval_dialogue_record_by_id`), the queue
(maximum 8), choice menus, and the DBASE400 text shown as subtitles on a panel.
Spec: `docs/GAME_dialogue.md` §2 and §4. Speech (DBASE500) comes in M8 with
sound. `RunDbase100Record` in `roth_runtime.cpp` is the stub to replace.

**Headset check:**

- [ ] an examine in STUDY1 shows the original's line
- [ ] a conversation with choices offers the same choices, and picking one sets the same flags (compare with ROTH.C, same save)

**Done when:** those pass, and 0x2b no longer appears in the census.

### M6 — Actors, weapons, monsters *(after M2)*

1. Spawning: `0x16`, `0x3c` and `0x3a` (481 records). Realms props are already
   engine actors, so the hand mods can grab them without changes.
2. Weapons and player health, as sprite weapons: `ROTH_GAME_PORT.md` §4 step 7,
   `docs/GAME_combat.md` §2–3.
3. Monsters and projectiles: step 8, `docs/GAME_combat.md` §4–5. A monster comes
   alive the first time it is drawn; in VR that has to be checked per tic, not
   per eye.
4. The muzzle flash comes in here (`HANDOFF_REMAROTH.md` §6).

**Headset check:** shoot a monster with each weapon. Hits-to-kill matches the
original's table (`docs/GAME_combat.md` §9).

### M7 — Saves and game flow *(after M4 and M5)*

`ROTH_GAME_PORT.md` §4 step 9: new game, death and reload, save and load with
every visited map's state, map-to-map arrival. Without this nothing is playable
start to finish.

**Headset check:** save in STUDY1 with an item carried and a door open. Quit,
load: both are still true. Die: you go back the way the original sends you.

### M8 — Sound, speech, cutscenes

`ROTH_GAME_PORT.md` §4 steps 3 and 5. The 94 sound-node records, speech from
DBASE500, and the 164 GDV movies. Do not use Doom sounds as placeholders
(`HANDOFF_SESSION_2026-10-10.md` §6).

### Then: play it through

One map at a time, in story order, with the census per map as the to-do list.
CHURCH1 holds the most missing logic in the game (`HANDOFF_REMAROTH.md` §10
item 4). Each map is done when the owner has played through it in the headset.

---

## 4. How the team works

**Parallel lanes.** M2 blocks M4, M5 and M6. M3 can run alongside M2. Once M2
lands, M4, M5 and M6 can each have their own lane.

**One machine has one build directory, and two builds at once kill each other**
(`HANDOFF_REMAROTH.md` §9). Each parallel lane needs its own `git worktree` and
its own build directory, or the lanes take turns building. Expect the first full
build in a new directory to take a while.

**Each lane, every session:**

1. Read this file's milestone, then only the documents it names.
2. Build. Clear any launch with the owner first, and never launch while he is in
   the headset.
3. Re-run `opcodecensus` before and after, and put both numbers in the handoff.
   **Update the census tool's table whenever you change the dispatcher**; it
   mirrors `IsImplemented` by hand.
4. End the session with the milestone's headset check written out for the owner.
5. Write `HANDOFF_SESSION_<date>.md`: what moved, the census before and after,
   the exact next step. If a rule changed, correct the document it lives in.

**With the owner:**

- Decide, then report. Ask only when a choice can't be undone, and then as one
  short question.
- He checks the work in the headset. A milestone is done when he has seen it.
- Never comment on his hours.
- Spend agents on building the game, not on reviewing your own work.

**Reference, when a milestone sends you there:**

- `LANE_START.md`: which document to trust, where everything is, and how to run
  the original side by side.
- `HANDOFF_REMAROTH.md` §0: the hard constraints. Nothing from the game is
  written to disk; ROTH.C is never edited; the fork is never distributed.
- `docs/REMAROTH_MEASURED.md`: everything already measured.
- `ROTH_GAME_PORT.md` and the six `docs/GAME_*.md` specs: the rules for each
  system, with ROTH.C line references and acceptance tests.
