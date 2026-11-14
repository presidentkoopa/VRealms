# VR changes ledger: every change we made, and the chapters it reaches

One row per change. A chapter doc's "Changes made for this chapter" section points here and lists the rows that touch
it. Add a row when a change lands (commit), and fill **Evidence** only with named evidence (a capture, a log line from
the owner's PC, a differential-test count). "—" means not seen yet.

Kinds: **1:1** = a fix that brings the port closer to ROTH.C; **VR** = a VR presentation or input change the owner
approved; **tool** = a test or debug hook that changes no game behaviour.

Chapters: "all" means it applies from the first frame of the game; a number list means only those chapters'
entries use it.

## Built

| # | change | kind | owner decision | commits | evidence | chapters (entries) |
|---|---|---|---|---|---|---|
| 1 | Hands are the FBX (hand_final.fbx, read by ufbx); RS_Hands and the IQM hands removed | VR | FBX hands only | 69e2e065, 2e58b3e4, cac07ed1 | engine flat captures (README) | all |
| 2 | Left hand wears the right's skin (mirrorskin); the palm is not driven, only the 15 finger joints | VR | owner, in the headset | b5e2257f, b60ded40 | headset (owner) | all |
| 3 | Fingers driven by each hand's authored take poses (50 + `fbx_pinch` 694, `fbx_pinch_tight` 312) | VR | walk the frames, don't trust tables | 0bd9acc0, 212d6dcd, 13de3c47 | `docs/images/hands/take_walk_*`, `pinch_frames_*` | all |
| 4 | Held-object and weapon-grip poses ported from UZDXREMA VAP:476-656; missing frames copied from the other hand | VR | "just copy the poses" | a933c698, 13de3c47 | captures 967e825f (`engine_holds*.png`) | all; ch1 (7, 8, 22, 35) |
| 5 | The item in the RIGHT hand, as the original; the weapon stays where it is | VR | owner | 13de3c47 | captures 967e825f | all |
| 6 | VR scale from the original's eye (feet + 144) on the measured head | VR | owner | 71d92ff2 | — (headset) | all |
| 7 | UI text in the original's font | 1:1 | owner | 1f8c3aa5 | — | all |
| 8 | The hands point and touch through the original's cursor and verbs (classify_cursor_target_object ported, 786,432 cases, 0 differ) | VR over 1:1 | VR interaction authorized | 15c9da27, 10e884f6, 79dc269c | `tools/vrpick_test` count | all |
| 9 | Trigger = the original's primary (examine), grip = its secondary (use); pointing at range (≤ 268 planar), grab in reach; the 0.8 s dwell dropped | VR | owner's buttons | 983ef7b8, 25b13e97, e2c2fee7 | log on the owner's PC; `handclick_test` 140 | all |
| 10 | A use carries the item of the hand that makes it (the right-hand item on the world) | VR | owner | 471eb9d7, b0a9fff8, ad6d1ec5 | captures dd507632 | all; ch1 (31, 33, 36) |
| 11 | Inventory by hand: each hand is the original's mouse (laser, index tap/hold, fingertip touch/hold) | VR | owner | 2ef7ef11, a80512e5, 47adae3b | captures 75aef0fa, b987b551; `invhand_test` 117 | all |
| 12 | The trigger attacks only with a weapon | VR | owner | d34fb269 | log | all |
| 13 | The inventory pauses the world (mode 3, frozenstate bit 4; the hands keep moving) | 1:1 | "like the original" | d34fb269 | log: 0 actor ticks while open | all |
| 14 | A click during a playing line skips it | 1:1 | "like the original" | d34fb269, ae33678b | log | all |
| 15 | 0x0d's key-0 path: the art swap happens (the 0x3a mint carried across the 0x12 delay) | 1:1 fix | — (a bug) | 09469d04 | captures e20464e7 | all; ch1 (31, 33, 34) |
| 16 | Candles wick by wick, in any order; the first wick fires the original's one use; unlit wicks catch after 15 s; one growing light per holder | VR over 1:1 | owner: any order, 15 s, growing light | fbb84e20, c2275576 | captures acb11adb; `wicks_test` (2162 scenarios) | ch1 (31, 33, 34); ch2+ every candelabrum |
| 17 | `roth_darktest`: RS_Radiance's darkness pass, set from the game (no cvars) | tool | darkness available, **chapter 1 uses none** | 24edb2bd, c28161c3 | captures aa3c465f | none yet |
| 33 | Guns aim along the gun: in VR the shot starts at the main hand (when the original's map reaches it from the player's start, else the original's start) and leaves along the hand; the stored attack pose's convention fixed in WeaponTick (a VR shot went 90 degrees aside, pitch mirrored); rate, damage, ammo, reload the original's. Test-only `roth_gunaimtest` | VR | W1 | 2f54599b | — (here: weapon replay item 9 vs the original 8053 states / 0 differ, identical to HEAD; `tools/gunaim_test` 206,401 walk cases on 44 maps, 0 wrong; cfg `gunaim_flat.cfg` not yet run) | all with a gun |
| 34 | The off-hand mount: in VR the original's subtitle line (and the death wait's) and its health panel ("low" picture included), drawn by the original's routines, stand over the off hand's wrist facing the eye; the rest of the HUD where it was; below half health a red screen edge (GLDEFS `RothLowHealth`) and a heartbeat on both controllers. The death wait now shows its line and the HUD as the original's mode 0x20 does (1:1). Non-archived `roth_mount_mm` (0.55 mm an original pixel) / `roth_mount_maxcm` (28) / `roth_mount_liftcm` (6), in metres like the hands; test-only `roth_mounttest`, `roth_sethealth` | VR (+ 1:1 death text) | S1, H1 | 913583f9 | — (here: huddraw_test vs the original unchanged; cfg `mount_flat.cfg` not yet run) | all |
| 36 | Quicksave / quickload: a double tap of the left controller's X / Y (engine/vr/commonbinds.txt defaults) runs the original's F9 / F10 (slot 0); F9 / F10 and the commands act in play only, as the original's keymap is read (game_core.c:869-879) | VR (+ 1:1 gate) | H2 | 55f11bb4 | — (cfg `quicksave_flat.cfg` not yet run) | all |

**Removed / to remove**

| # | change | why | commits |
|---|---|---|---|
| R1 | Grip on empty air fires the original's secondary-on-nothing attack (a punch) | owner: physical melee only | built in d34fb269; removal in the gestures agent's work |


## Flat PC evidence, 2026-11-13 late (owner's PC, flat, through the bridge; nothing in the headset)

| row | change | lane commit(s) | seen on the PC (log / capture) | still open |
|---|---|---|---|---|
| 18 | record surface boxes, 1:1 | 5fc3b774, a2e585d3 | surfbox_flat: texel the click paths compute = the test texel; 21,131 OUTSIDE, 22,131 INSIDE → #385 "Damn. Power's off in here."; 25,135 examine → #372 "Good job I've got the lantern."; flat +use through the eye probe → #385. Match test 757,120 cases 0 differ vs the original | palm-on-plate case re-run (cfg fixed in a2e585d3) |
| 19-23 | gestures | 03548253 … 66243f00 | knock "Old suit of armour…"; sofa #314, desk #288, typewriter #286, flies #337, door sweep #455 "Where's the lock on this damn thing?"; ward tingle 0.04 → 0.16; switch punch = use; tall wick in reach (planar 100 of 268); grip on nothing no longer attacks | creature punch damage; re-check after the movement port (66243f00 run pending) |
| 24-29 | held objects | 462f0709 | STRUCK 0.50 m/s, match lights wick 3 then 1 and both tall candles; Lamp off the hook and back; item 17 in the item hand; Book in both hands with the popup beside it; forced turn logged | the niche's third light change rises to 255 instead of −30; the match not visible in the capture |
| 30 | the original's movement 1:1 | 1d6b0e7a, 77c34c5e, 367c710c (link fix), 7a0c3e69 | move_flat (run): 161 of 161 tic lines equal the engine-free port's schedule (regenerated independently by the lead: 0 diff); the port vs the running original: 14 scenarios, 9,686 ticks, 0 differ | VR parts (snap 45, real crouch, teleport, roomscale) not seen |
| 33 | guns along the gun | 0e75a804, 057c8524 | gunaim_flat: hand shot at yaw 0 travels engine yaw 180 = the view's and the original's flat shot; +20° / −30° ends 150.6° / +20.1°; −15° / +25° ends 205.4° / −14.8° | headset |
| 34 | off-hand mount + low-health cue | 0cc88be9, 057c8524 | "low-health cue on" logged; the mount drawn on the off wrist | the red edge not visible in captures; the line's readability on the wrist |
| 36 | quicksave / quickload | f1411ce1, 057c8524 | saved, refused twice with the inventory open, loaded; after the load 4 items and no Colt (given after the save) | the double-tap binding in the headset |

**Found on the way:** examine records with no chain were never bound, so no object/face/floor examine line played by mouse or hand (fixed e145db53, gameproof and dialogue_test identical after); a use on a face fired every use binding there instead of the one record the original stops at (fixed 5fc3b774); an MSVC link trap (`struct player_t` forward declaration, fixed 367c710c). The Book's first look never gives the silver key in the original (its OnInspect's `if-flag 4` / `!if-flag 4` can't both pass): Q8's "key into the palm" has nothing to show (VR_CHAPTER01_SHADOWS entry 7).

## In progress (agents running)

**Gestures (rows 19-23), first PC run of 03548253 (2026-11-13, flat, gestures_flat.cfg):**
- seen working (log on the owner's PC): ward tingle 0.04 at 0.15 m to 0.16 at 0.03 m (`docs/images/gestures_ward_near_03548253.png`);
  a punch into the lounge switch fires the original's use ("acted"); the grip on nothing no longer attacks; tall-wick
  point + grip lit wick 3 through the original's use and the light grew.
- seen FAILING: the knock (no rap detected; a slow move was taken as a rap); the attempts (zones found, but "nothing to
  examine": the original's line never played); the flies wave and the door sweep (nothing detected); the punch on a
  creature fired the attack but the creature's health did not change (`gestures_punch_creature_03548253.png`); the
  tall wick lit from 4 m (reach not enforced). Sent back to the agent.

| # | change | kind | owner decision | chapters (entries) |
|---|---|---|---|---|
| 18 | Record surface boxes (+0x0c..+0x12) tested against the hit texel | 1:1 fix (flat first) | — (a 1:1 gap) | all; ch1 (10, 11, 23, 26, 36) |
| 19 | Physical melee from RSQ Melee's idea (no combos, uppercut, slap, headlock); original damage; a punch into a switch or door is a use | VR | Q6 | all; ch1 (13, 16, 24, 38) |
| 20 | Knock to examine (two raps of an open, empty hand) | VR | Q3 | all |
| 21 | A physical attempt plays the original's examine line (pattern) | VR | Q4 | all; ch1 (4, 5, 10, 14, 21, 28) |
| 22 | Ward tingle haptic | VR | Q9 | all; ch1 (9 and the wards) |
| 23 | Tall wicks: point the lit match at a wick in reach + grip | VR | Q13 | ch1 (33) |
| 24 | Strike a real match (lit match in the off hand, 20 s) | VR | Q1 | all; ch1 (31, 33) |
| 25 | The Lamp on a hip hook, grabbed when needed | VR | Q2 | all |
| 26 | The picked-up item shows in the item hand | VR | Q12 | all |
| 27 | The Book opens in both hands; the key drops into the palm | VR | Q8 | ch1 (7) |
| 28 | The niche flash as a real dynamic light (three pulses) | VR | Q10 | ch1 (34) |
| 29 | The forced turn (0x3f) snaps during the black frames | VR | Q7 | ch1 (37); every 0x3f |

## Decided, to build next (chapter 1 second pass, owner 2026-11-13)

| # | change | kind | owner decision | chapters |
|---|---|---|---|---|
| 30 | The original's movement ported 1:1 (step-up 65 / 16 crouched, crouch to 0x30, the 19-phase jump, speeds, turn ramp), diffed against the original | 1:1 | L4, "now" | all |
| 31 | Locomotion: smooth stick (default) or teleport, a setting; snap turn 45° | VR | L1, L2 | all |
| 32 | A real crouch drives the original's crouch; a button for seated players | VR | L3 | all; ch1 (27) |
| 35 | Movies on a world-locked screen | VR | M1 | all |
| 37 | A held item brought to the eyes plays its examine | VR | X1 | all |

## Decided, LATER

| change | owner decision |
|---|---|
| Real-parts reload with high-quality meshes (drop mag, insert, rack the slide, world collision, nothing on a sprite layer); still one magazine item per reload | W2 |
| On-gun ammo display, from REMASelacoVR's ideas | G1 |
| Dedicated left-hand support | Q11 |

## Decided, nothing to build

| decision | chapters |
|---|---|
| Mirrors keep the original's painted, breakable glass (no live reflection) | ch1 (13, 38); every mirror |
| Far things (the casements): the pointer is enough | ch1 (18) |
| Candle light strength judged in the headset | ch1+ |
| Item 3D models parked; sprites for now; raise the sprite limit if models get there | all |
| Desks (STUDY1 roll-top, CHURCH1) never open in the original; opening them would be new content (owner's call later) | ch2+ |
| Dedicated left-hand support later; until then the item is the right-side hand | all |
