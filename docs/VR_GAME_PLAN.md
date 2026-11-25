# VRealms: the game plan (Realms of the Haunting, reimagined for VR)

Written 2026-11-24 by the lead, after the owner's direction of the same day: "realms of the haunting reimagined to
take advantage of full 6dof for a shooter / adventure game hybrid"; "object / world / hand / player interaction is the
single most important thing for this project"; "you're welcome to do anything you want in order to achieve this goal".

**This file is the top of the plan.** Every session picks its work from section 5, in order, and updates section 7 when
a milestone moves. The subsystem documents (`VR_PHYSICS.md`, `VR_HANDWORKS.md`, `VR_INTERACTION.md`,
`VR_CHANGES_LEDGER.md`, the chapter dossiers) hold the detail; this file says what they are for and in what order.
Work that is not in this plan is added to it first.

Marks: **[S: how]** seen, with the evidence named; **[R]** reasoning only, not seen.

---

## 1. For the owner, in plain words

**What the game should feel like.** You are Adam, in the house, with your own two hands. Everything you can see you can
reach for. You open doors by their handles, light candles with a match you strike, turn keys in locks, pull levers,
put the Orb in its door and the statuettes on their plates. You fight with a gun you aim with your hand, reload by hand,
a sword you really swing, a shield on your arm, a shotgun you hold with both hands. Monsters' shots are things you can
duck and dodge. What you drop stays where it lands; what you throw flies. Underneath, it is still Realms of the
Haunting: the same puzzles, the same story, the same rules for what opens what.

**How we get there.** One slice first: chapters 1 and 2 (the house, the Study, the cellars, the mausoleum, Aelf). They
contain almost every kind of thing the game ever asks you to do (section 4). We make that slice feel great in the
headset, with you playing it at every milestone, then carry the same systems through chapters 3 to 20.

**What you do.** At each milestone you get a short "try this" list for the headset (15-20 minutes). What you report
back decides what gets fixed. For things the original never had (throwing, swinging doors, dodging), your hands are the
test. For the game's own rules, we keep proving them against the original program.

---

## 2. The pillars

1. **Hands do everything.** Every use, take, examine and puzzle step can be done by the hands on the thing itself;
   the original's point-and-click stays available underneath (owner, 2026-11-13: "keep vanilla functionality but have
   our VR stuff be the goal", `ITEM_DESIGN.md`).
2. **A shooter you hold.** Weapons live on the body and in the hands; aim, swing, block and reload are physical.
3. **An adventure you handle.** Puzzle items are objects: placed, turned, inserted, lit, poured, read.
4. **Your body is Adam.** Crouch, lean, duck, reach; the pack (later the trenchcoat) is on your body.
5. **A solid, haunted world.** Hands and held things stop at walls; loose things fall and rest; light comes from what
   you carry; the darkness is real.
6. **The original underneath.** Chains, pickups, keys, reach, damage numbers, monster rules: 1:1 with ROTH.C and proven
   by differential tests (CLAUDE.md). VR changes how a verb is performed, never what it does, unless the owner decides
   otherwise (section 6).

---

## 3. Where we stand (2026-11-24)

Everything below is built. "Flat" means seen on the owner's PC through the bridge with the test rig driving the
controllers (`roth_xrpose`), not in the headset. Rows refer to `VR_CHANGES_LEDGER.md`.

| pillar | built | seen how |
|---|---|---|
| Hands do everything | the original's cursor and verbs under each hand (row 8, 786,432 cases 0 differ); trigger = examine, grip = use (9); a use carries the hand's item (10); the inventory by hand (11); one owner per hand per tic (`VR_INTERACTION.md` L2, P1); pickup flies in as the 3D model, stow over the shoulder; knock to examine (20); attempts play the examine line (21); an item at the eyes examines (37) | flat [S: logs, captures named in the ledger]; headset: the hands themselves (row 2) and the owner's 2026-11-16 report, nothing since |
| A shooter you hold | guns aim along the hand (33); physical melee, a punch into a switch is a use (19); the Sword swung (38); the Staff shot and swung (42); the Shield on the forearm (39); six holsters on the body (40); the Shotgun in two hands (41) | flat [S: weapon replays 0 differ, PC logs zzzx13-zzzx38] |
| An adventure you handle | strike a match, light wick by wick (16, 23, 24); the Lamp on the hip, its light from the lamp (25); the Book opened in two hands (27); statuettes and the Orb released onto their places; the Shrive and keys twisted; levers pulled; hand prints pressed; sit; drink a vial; the clock's hour hand turned (2026-11-16 section); doors swung by the handle, levers on hinges, keys turned in locks (`VR_HANDWORKS.md`, M2) | flat [S: log-worldhands_*, log-handworks_*] |
| Your body is Adam | the original's movement 1:1 (30, 9,686 ticks 0 differ); smooth or teleport, snap turn (31); real crouch (32) | movement diffed; the VR parts not seen |
| A solid world | Jolt physics: the level solid, hands stop at walls, drop / throw / take again (`VR_PHYSICS.md`, M1) | flat [S: log-physics_flat, phys6_*] |
| Presentation | 3D item models for chapters 1-2 (glass, glow, gems); movies on a world-locked screen (35); the subtitle and health panel on the off wrist (34); candles' growing light (16) | flat captures |

**The honest gaps.**
- **Almost nothing has been played in the headset.** Since the 2026-11-16 report, every system above was built and
  checked in flat. Feel (weight, timing, haptics, reach) cannot be judged there. This is the largest risk in the
  project and the reason milestone 3 comes first.
- **Grown separately.** Grabbing is decided in several places: the hand state (`roth_handstate.h`), the world
  gestures (`roth_worldhands.cpp`), the tracks (`roth_handworks.cpp`), the physics hands (`roth_physics.cpp`), the
  holsters and the melee reader (`roth_vrweapon.h`, `roth_gesture.h`). Whether every one of them asks the L2 owner
  first is [R], not audited.
- **Combat is the original's, dressed.** Monsters are sprites that always know where Adam is, walk straight at him and
  fire projectiles (`GAME_combat.md` 4.3); a hit is a 24-tick flinch. No hit reactions where struck, no physical
  deaths, no blocking. Weapons are not yet solid (the physics hand carries items, not the weapon).
- **Reloading by hand, the Shotgun's pump** and the gun models are "later" (ledger, Decided LATER, W2).
- **Thrown items do nothing to the game** (they fall and rest).

---

## 4. Why chapters 1-2 are the slice

Their records (`CHAPTERS.md` 2 and 4, census-read) contain: a locked door and key (STUDY1 #11, i21), keys in locks
(MAUSO1EA #76, i22), placing items (the Orb in MAS3 #4, the serpent statuettes), lighting with the Matchbox, levers
(MAS4, MAUSO1EA, MAS6 #20), hand prints (MAS6 #42/#8/#41), a timed lever (MAS6 #29), a kill counter (MAS6 #48, AELF
#9), a dialogue choice (MAUSO1EB act174), the Shrive twisted (AELF #80), sitting on a throne (MAS7), drinking vials,
the first creatures (STUDY1 #362, #265) and the Colt, Shotgun, Sword, Shield and Staff. Both chapters already have
VR dossiers (`VR_CHAPTER01_SHADOWS.md`, `VR_CHAPTER02_SIGNS_AND_PORTENTS.md`). Of the puzzle kinds in the whole game
(`CHAPTERS.md` 4), only these are absent from the slice: the Mask's travel (ch 4, 9), the wards revealed by the Torch
(ch 12), dials (ch 17), the mirrors' attacks (ch 16), the thrown chalice (ch 7).

---

## 5. The milestones, in order

Each milestone ends with: the regression suite (CLAUDE.md), one bridge run of its configs, a commit, a handoff, and
a **"try this" list for the owner's headset** written in plain words. The owner's report opens the next milestone.

### M3: play what exists (the first headset pass) and one hand owner
- Write `docs/PLAYTEST_CH1-2.md`: a 20-minute route through chapters 1-2 naming each thing to try (doors, candles,
  match, Lamp, Book, levers, keys, the Orb, statuettes, drop and throw, holsters, Sword, Shotgun, Shield, crouch) and
  what should happen.
- Audit every grab and gesture path against the L2 owner; route the ones that bypass it through it. Evidence: a test
  that drives both hands through every claim pair (`handstate_test` extended).
- **Candles up close (owner, 2026-11-24: "ideally i would be able to get super close to it and light each candle").**
  The candelabras are flat pictures that turn to face the eye, so their wicks (`roth_candles.cpp` WickAt: five
  points across the picture, `OPENABLES.md` 6) swing with the head when it is close. Hold a candle's facing still
  while a flame is within reach of it; judge the 3 cm wick reach (`kWickReach`) in the headset. A real candelabra
  model with fixed arms belongs to the asset pass (M7).
- **The owner's first headset session (2026-11-24), fixed or scheduled:**
  - "the interaction indicator is jumping all over ... some dotted thing": the dotted beam replaced by Godot XR Tools'
    pointer target (a marker held in the world at the hit, shown only where the hand can act; `roth_vrpick.cpp`
    THE DRAWING). Built, not yet seen.
  - "massive headbobbing": the original's walk bob kept off the VR eye (`roth_playermove_engine.cpp` WritePawn). Built.
  - "no analog movement": the stick reached the game as four exclusive keys (`commonbinds.txt` axis1/axis2,
    `vk_openxrdevice.cpp` cardinal zones). In VR the original's key path now takes the device's analog movement stick
    (direction and push; `MoveInput::analog`). Built; `move_test` vs the original still 14 scenarios, 0 differ (flat).
  - "i cant touch the main menu buttons": a menu box took clicks only from the use key; now the trigger or grip of the
    hand pointing at it clicks (`roth_interface.cpp` InterfaceMenuHandClick). Built. If the menu he meant is the
    engine's own start menu, that is a different path: ask.
  - "no clue what the controls are": an in-game controls card (to build: on the off wrist, beside the mount, or the
    first time the game starts); until then the controls in chat and `PLAYTEST_CH1-2.md`.
  - "disable the use key": with the VR hands live the use key uses nothing in the world (`p_map.cpp` P_UseLines). Built.
  - No way to open the inventory in the headset (no controller button carries the 'I' key): the backpack's "take
    out" (`VR_INTERACTION.md` 3b, decided 2026-11-23, not built until now): an empty hand squeezing over a shoulder
    opens the inventory (`roth_vrpick.cpp` BackpackGrip). Built.
- **Locomotion from a proven source (owner: "are we looking at available open source movement-solutions ... have we
  considered roomscale"; CLAUDE.md, existing solutions first).** Port Godot XR Tools' movement (MIT, on hand:
  `movement_direct.gd` 107 lines, `movement_turn.gd` 128 (snap or smooth), `movement_crouch.gd`, `movement_sprint.gd`,
  `movement_jog.gd` (arm-swing), `effects/vignette.gd` (comfort), and `player/player_body.gd` 1,093 (the body that
  follows the headset through the world for roomscale)), with the original's collision underneath (each move goes
  through `PlayerMove`, as the roomscale step already does, `PLAYER_MOVEMENT_PORT.md` row 70). Porting checklist first.
- Fix what the owner's session finds, before anything new.
- Done when: the owner has played the route and his list is fixed or scheduled.

### M4: combat you feel
- **Shots hit your real body.** Check that the headset's roomscale position and the real crouch move the player box
  the original's projectile test uses (`GAME_combat.md` 5, CP:43-72; `PLAYER_MOVEMENT_PORT.md` row 70 is [R]). If so,
  leaning and ducking already dodge; prove it with a flat run (a ghoul's shot, the head moved 40 cm aside, no hit).
- **Weapons are solid.** The weapon in the hand joins the physics hand (a compound, as held items), so a gun or sword
  stops at walls and the drawn weapon with it.
- **Hits read in 3D (presentation only).** The creature reacts at the side it was struck (a push of its sprite away
  from the blow for the original's 24-tick pain); a death falls along the blow's direction before the original's
  corpse timer turns it into a prop (`GAME_combat.md` 4.3). Damage, timing and the corpse rule unchanged.
- **Reload by hand** on placeholder shapes (the magazine out of the pouch into the Colt; the Shotgun's pump), the
  original's reload rule underneath (one magazine item per reload, ledger W2). The models come with the asset pass.
- The Shield blocks monsters' shots; thrown items stun, and a hard throw hurts a little (section 6, decided).

### M5: the adventure by hand, complete for the slice
- **Sockets** (`VR_PHYSICS.md` next 2): every "place" and "key" unit of chapters 1-2 accepts the item brought to its
  spot, snapping into place, firing the original's use (generalises the release and key rules already built).
- **Two hands** (`VR_PHYSICS.md` next 3): an item in each hand; both hands on one object.
- **The inventory by hand finished** (`VR_INTERACTION.md` P2-P4): examine three ways, use on yourself, combine (the
  original's 24 recipes) by bringing two items together, taking an item out of the pack into the hand.
- Done when: every progress unit of chapters 1-2 (`chapter_census -chgates`) is reachable by hand, listed with its
  bridge log line.

### M6: a physical world
- The level's own lying items as bodies, in step with the original's objects (`VR_PHYSICS.md` next 4).
- Light from what you carry, chapter by chapter darkness (`roth_darktest`, ledger row 17).
- Remaining switches on the ported Button, the clock on the ported Dial (`HANDOFF_..._M2.md` 4.3).

### M7: the slice's look (assets, with the owner's green light)
- What is needed and why, sent to the owner before any generation: the weapons (reload parts, grips), the creatures
  of chapters 1-2 (3D instead of sprites), levers, keys; hand poses per hold.
- Cloth for the trenchcoat once there is a player body (`VR_PHYSICS.md` next 5).

### M8 onward: chapters 3-20
For each chapter, in the game's order: its dossier (as chapters 1-2), every unit mapped to a system that exists, the
new kinds built where they first appear (the Mask's travel ch 4; the thrown chalice ch 7; the Torch's beam ch 12,
owner decision 2026-11-13; the mirrors ch 16; dials ch 17), a bridge run per map, the owner's headset pass.

---

## 6. The owner's decisions

Answered 2026-11-24 (owner: "yes, hurt a little or stun for a momoent, and yes"):
1. **The Shield blocks monsters' shots: YES.** A monster's projectile that meets the Shield on the forearm is stopped
   there and does no damage. New game logic, by the owner's decision; the Shield's original effect (the "shield in
   hand" checks, ledger row 39) is unchanged. M4.
2. **Thrown things: "hurt a little or stun for a moment".** A thrown item that strikes a creature fast enough stuns
   it for the original's pain time (24 ticks, `GAME_combat.md` 4.3); a hard throw also does a little damage (a
   fraction of Adam's fists' blow, row 42's borrowed numbers; the exact speed thresholds and amount are tuned in the
   headset). M4.
3. **A `main` branch: YES.** Created at the lane's tip; `lane/gamelayer` stays the working branch and `main` is
   moved up to it at each milestone.

New questions go here, asked once, in plain words, when their milestone starts.

---

## 7. Status

| milestone | state | evidence |
|---|---|---|
| M1 solid hands, drop, throw | built, flat | `VR_PHYSICS.md` Proof |
| M2 doors, levers, keys by hand | built, flat | `VR_HANDWORKS.md`; M2 commits 41abfc59, ff7f2d3d; Linux re-run by the lead 2026-11-24 at ff7f2d3d: physics_test 24/0, track_test 59/0, worldhands_test 87/0, the four changed roth files syntax-checked; the ROTH.C differential suites unchanged (gameproof modes 0 1 2 4 7 all match, dialogue_test 4 IDENTICAL, cmdcore STUDY1 IDENTICAL, actors STUDY1 0 diff; none of them compiles a file M2 changed, so they show the game logic untouched, not M2 itself) |
| M3 headset pass, one hand owner | next | |
| M4-M8 | planned | |
