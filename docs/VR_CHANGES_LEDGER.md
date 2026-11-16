# VR changes ledger: every change we made, and the chapters it reaches

One row per change. A chapter doc's "Changes made for this chapter" section points here and lists the rows that touch
it (chapter 1: `VR_CHAPTER01_SHADOWS.md` 9; chapter 2: `VR_CHAPTER02_SIGNS_AND_PORTENTS.md` 9, entry numbers below
are that file's). Add a row when a change lands (commit), and fill **Evidence** only with named evidence (a capture, a log line from
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
| 4 | Held-object and weapon-grip poses ported from UZDXREMA VAP:476-656; missing frames copied from the other hand | VR | "just copy the poses" | a933c698, 13de3c47 | captures 967e825f (`engine_holds*.png`) | all; ch1 (7, 8, 22, 35); ch2 (4, 6, 7, 8, 28, 32, 49, 58) |
| 5 | The item in the RIGHT hand, as the original; the weapon stays where it is | VR | owner | 13de3c47 | captures 967e825f | all |
| 6 | VR scale from the original's eye (feet + 144) on the measured head | VR | owner | 71d92ff2 | — (headset) | all |
| 7 | UI text in the original's font | 1:1 | owner | 1f8c3aa5 | — | all |
| 8 | The hands point and touch through the original's cursor and verbs (classify_cursor_target_object ported, 786,432 cases, 0 differ) | VR over 1:1 | VR interaction authorized | 15c9da27, 10e884f6, 79dc269c | `tools/vrpick_test` count | all |
| 9 | Trigger = the original's primary (examine), grip = its secondary (use); pointing at range (≤ 268 planar), grab in reach; the 0.8 s dwell dropped | VR | owner's buttons | 983ef7b8, 25b13e97, e2c2fee7 | log on the owner's PC; `handclick_test` 140 | all |
| 10 | A use carries the item of the hand that makes it (the right-hand item on the world) | VR | owner | 471eb9d7, b0a9fff8, ad6d1ec5 | captures dd507632 | all; ch1 (31, 33, 36); ch2 (5, 29, 34, 39, 61: the sockets) |
| 11 | Inventory by hand: each hand is the original's mouse (laser, index tap/hold, fingertip touch/hold) | VR | owner | 2ef7ef11, a80512e5, 47adae3b | captures 75aef0fa, b987b551; `invhand_test` 117 | all |
| 12 | The trigger attacks only with a weapon | VR | owner | d34fb269 | log | all |
| 13 | The inventory pauses the world (mode 3, frozenstate bit 4; the hands keep moving) | 1:1 | "like the original" | d34fb269 | log: 0 actor ticks while open | all |
| 14 | A click during a playing line skips it | 1:1 | "like the original" | d34fb269, ae33678b | log | all |
| 15 | 0x0d's key-0 path: the art swap happens (the 0x3a mint carried across the 0x12 delay) | 1:1 fix | — (a bug) | 09469d04 | captures e20464e7 | all; ch1 (31, 33, 34); ch2 (3) |
| 16 | Candles wick by wick, in any order; the first wick fires the original's one use; unlit wicks catch after 15 s; one growing light per holder | VR over 1:1 | owner: any order, 15 s, growing light | fbb84e20, c2275576 | captures acb11adb; `wicks_test` (2162 scenarios) | ch1 (31, 33, 34); ch2 (3: the four Study candelabra, STUDY1 #18); ch2+ every candelabrum |
| 17 | `roth_darktest`: RS_Radiance's darkness pass, set from the game (no cvars) | tool | darkness available, **chapter 1 uses none** | 24edb2bd, c28161c3 | captures aa3c465f | none yet |
| 33 | Guns aim along the gun: in VR the shot starts at the main hand (when the original's map reaches it from the player's start, else the original's start) and leaves along the hand; the stored attack pose's convention fixed in WeaponTick (a VR shot went 90 degrees aside, pitch mirrored); rate, damage, ammo, reload the original's. Test-only `roth_gunaimtest` | VR | W1 | 2f54599b | — (here: weapon replay item 9 vs the original 8053 states / 0 differ, identical to HEAD; `tools/gunaim_test` 206,401 walk cases on 44 maps, 0 wrong; cfg `gunaim_flat.cfg` not yet run) | all with a gun; ch2 (22, 32 and every fight: the first creatures) |
| 34 | The off-hand mount: in VR the original's subtitle line (and the death wait's) and its health panel ("low" picture included), drawn by the original's routines, stand over the off hand's wrist facing the eye; the rest of the HUD where it was; below half health a red screen edge (GLDEFS `RothLowHealth`) and a heartbeat on both controllers. The death wait now shows its line and the HUD as the original's mode 0x20 does (1:1). Non-archived `roth_mount_mm` (0.55 mm an original pixel) / `roth_mount_maxcm` (28) / `roth_mount_liftcm` (6), in metres like the hands; test-only `roth_mounttest`, `roth_sethealth` | VR (+ 1:1 death text) | S1, H1 | 913583f9 | — (here: huddraw_test vs the original unchanged; cfg `mount_flat.cfg` not yet run) | all |
| 36 | Quicksave / quickload: a double tap of the left controller's X / Y (engine/vr/commonbinds.txt defaults) runs the original's F9 / F10 (slot 0); F9 / F10 and the commands act in play only, as the original's keymap is read (game_core.c:869-879) | VR (+ 1:1 gate) | H2 | 55f11bb4 | — (cfg `quicksave_flat.cfg` not yet run) | all |
| 38 | **The Sword (i29) is swung, never triggered**: the fists' reader (RSQ Melee's, roth_gesture.h, unchanged) fed the blade's striking point (0.65 of an 0.85 m blade along the aim pose [R: no model]); the contact along the whole blade swept between two tics (roth_vrweapon.h BladeSweptBoxContact); the blow is the original's attack aimed at the contact (trigger_weapon_fire with the Sword: bullet i164, damage 200, life 10 ticks; rate: 11- or 7-frame swing pictures at 4 ticks, a swing inside the cycle refused by the original's fire lock) or its use where the cursor says use, as the fist; the fire button held back while the Sword is the weapon. Test-only `roth_vrweapontest` | VR | Q6 | (this commit) | here: tools/vrweapon_test (blade cases); weapon replay i29 vs the original 7845 weapon states / 456 ticks, 0 differ; PC cfg `vrweapons_sword_flat.cfg` not yet run | ch2 (Sword: every creature after STUDY1) |
| 39 | **The Shield (i28) worn on the off forearm whenever carried** (VR only): the original's in-hand test (RAW 0x27 select mode, query_player_inventory) counts it as the weapon hand does (Inventory::SetWorn) -- the Shield's ONLY effect in the original (ANUBIS #83 "no shield in hand: damage", #11 / #49 "no shield: spawn, else #112"; its WeaponAction has no bullet: 25 pulls in the running original, 0 shots); its card (or model) on the off forearm | VR | Q7 | (this commit) | here: inventory_test (worn and the weapon click, 16 checks; 4723 checks 0 failed), weapon replay i28 7315 states 0 differ (1 inventory line garbled in the original's log); cmdcore ANUBIS identical; PC cfgs `vrweapons_shield_vr.cfg` / `_flat.cfg` not yet run | ch2 (given in STUDY1); ANUBIS (#11, #49, #83) |
| 40 | **Body slots for the main hand**: RSQ Tactical Rig's six gun holsters (hips, shoulders, chest; its places, nearest-centre and rank, 3-tic dwell, quick swap, 1 s free, assignment by first use, "Into the Wheel" swap) -- a draw is the inventory's weapon click (activate_weapon_item(slot, id)), a weapon put away activate_weapon_item(0, 0), the inventory's own; the Shield never takes one. Partial port (checklist in roth_vrweapon.h): no off hand, rings, pictures, torso, profiles, saved assignments | VR | Q8 | (this commit) | here: tools/vrweapon_test (holster scenarios); PC cfg `vrweapons_slots_flat.cfg` not yet run | all with weapons |
| 41 | **The Shotgun (i38) in two hands**: the off hand takes the fore-end exactly as the engine's support hold (vk_openxrdevice.cpp:4513-4600 ported as SupportLatch: 120,000 random tics, 0 differ from that block) and the shot leaves along the rear -> fore line; its grip is then the gun's (no use, no punch; WF_TWOHANDSTABILIZED so RSQ Distance Grab stands down); fire, ammo (magazine 6, item i48), rate the original's; the pump not built (meshes) | VR | Q9 | (this commit) | here: tools/vrweapon_test; weapon replay i38 7890 states 0 differ; PC cfg `vrweapons_shotgun_flat.cfg` not yet run | ch2 (32) and on |
| 42 | **The Staff (i12) swung and triggered**: the original's Staff is a recharging projectile ONLY (bullet i59, damage 205, charge 6, rate 2; no melee block) -- its trigger keeps that projectile along the staff (row 33). The swing (the reader and contact of row 38 on the Staff's 1.20 m [R]) has no numbers in the original: **swing numbers borrowed from Adam's fists (owner 2026-11-16: the staff can be swung); the trigger keeps the original's projectile** -- the blow is his fists' attack as the original has it (his own WeaponAction table, first block: bullet i107, damage 100, life 8, its sound), aimed at the contact, the next blow after that block's cycle; the Staff's own lock and charge untouched, a blow refused while its own shot cycles | VR | Q10 | (this commit) | here: tools/vrweapon_test (staff cases); weapon replay i12 8776 states 0 differ; PC cfg `vrweapons_staff_flat.cfg` not yet run | ch2 (MAUSO1EB, the altar) and on |

**Removed / to remove**

| # | change | why | commits |
|---|---|---|---|
| R1 | Grip on empty air fires the original's secondary-on-nothing attack (a punch) | owner: physical melee only | built in d34fb269; removal in the gestures agent's work |


## Flat PC evidence, 2026-11-13 late (owner's PC, flat, through the bridge; nothing in the headset)

| row | change | lane commit(s) | seen on the PC (log / capture) | still open |
|---|---|---|---|---|
| 18 | record surface boxes, 1:1 | 5fc3b774, a2e585d3 | surfbox_flat: texel the click paths compute = the test texel; 21,131 OUTSIDE, 22,131 INSIDE → #385 "Damn. Power's off in here."; 25,135 examine → #372 "Good job I've got the lantern."; flat +use through the eye probe → #385. Match test 757,120 cases 0 differ vs the original | all seen on 2579eca5, the palm too: palm at 35,135 OUTSIDE, palm at 25,135 INSIDE → #385 fires (surfbox_pc_run_2579eca5) |
| 19-23 | gestures | 03548253 … 66243f00 | knock "Old suit of armour…"; sofa #314, desk #288, typewriter #286, flies #337, door sweep #455 "Where's the lock on this damn thing?"; ward tingle 0.04 → 0.16; switch punch = use; tall wick in reach (planar 100 of 268); grip on nothing no longer attacks | all seen again on 66243f00 with the movement port; the creature punch: "HIT AN OBJECT 169/5", health 500 → 403 → 317 (gest_pc_run_66243f00) |
| 24-29 | held objects | 462f0709 | STRUCK 0.50 m/s, match lights wick 3 then 1 and both tall candles; Lamp off the hook and back; item 17 in the item hand; Book in both hands with the popup beside it; forced turn logged | niche fixed on 6b849e01 (a 1:1 bug: the 0x1d step is signed; 23 down-ramps in 9 maps rose in flat too): 78 → 118 → 148 → 118 as the original; the match's flame logged 0.038 m beyond the fingertip, not visible in a capture yet |
| 30 | the original's movement 1:1 | 1d6b0e7a, 77c34c5e, 367c710c (link fix), 7a0c3e69 | move_flat (run): 161 of 161 tic lines equal the engine-free port's schedule (regenerated independently by the lead: 0 diff); the port vs the running original: 14 scenarios, 9,686 ticks, 0 differ | VR parts (snap 45, real crouch, teleport, roomscale) not seen |
| 33 | guns along the gun | 0e75a804, 057c8524 | gunaim_flat: hand shot at yaw 0 travels engine yaw 180 = the view's and the original's flat shot; +20° / −30° ends 150.6° / +20.1°; −15° / +25° ends 205.4° / −14.8° | headset |
| 34 | off-hand mount + low-health cue | 0cc88be9, 057c8524 | "low-health cue on" logged; the mount drawn on the off wrist | red edge SEEN on 6af79ee2 (`mount_low_health_edge_6af79ee2.png`): the flat frame had presented the image from before the last post-process pass (vk_renderdevice.cpp, 46c37300 → 6af79ee2); the line's readability on the wrist is for the headset |
| 35 | movies on a world-locked screen | e69d358b | built; VR only (flat unchanged): `movie_world_vr.cfg` is for the headset | the headset |
| 37 | an item brought to the eyes plays its examine | e69d358b | held_eyes_flat: item 16 at 0.08 m for 0.5 s → the inventory's right-hand box clicked → the Matchbox popup → closed; re-armed at 0.37 m; nothing at 0.20 m; again at 0.10 m (eyes_pc_run_e69d358b) | the headset |
| 36 | quicksave / quickload | f1411ce1, 057c8524 | saved, refused twice with the inventory open, loaded; after the load 4 items and no Colt (given after the save) | the double-tap binding in the headset |

**Found on the way:** examine records with no chain were never bound, so no object/face/floor examine line played by mouse or hand (fixed e145db53, gameproof and dialogue_test identical after); a use on a face fired every use binding there instead of the one record the original stops at (fixed 5fc3b774); an MSVC link trap (`struct player_t` forward declaration, fixed 367c710c). The Book's first look never gives the silver key in the original (its OnInspect's `if-flag 4` / `!if-flag 4` can't both pass): Q8's "key into the palm" has nothing to show (VR_CHAPTER01_SHADOWS entry 7).


## 2026-11-16: the headset report, the hand frame, RSQ Distance Grab (owner's PC, flat, real controller path via roth_xrpose)

| change | commits | seen on the PC | still open |
|---|---|---|---|
| Every hand reader (rays, hand-locked billboards, held cards, mount, UseByHand) rides the DRAWN hand's frame (they read the gun's aim pose with a flipped pitch: lasers sideways/behind, billboards orbiting) | a3f36ce7 | rays: "ray vs drawn finger 0.00 deg, start 0.00 units" at every pose; pins: "error 0.00 units, worst 0.00" through tilt, roll, moves, player and head turns | the headset |
| RSQ Distance Grab copied whole from RSQ_UBQSeries/DistanceGrab (origin/main b1c2a8f90b; byte-identical but CVARINFO noarchive); a catch = the original's pickup; replaces at-range taking | cb7ad5dd, 6b8abd03 (ZScript fix), 5efafc2a, 5d569d2f, b8f514e1 | dgrab_study1_flat: 19 takeable; LOCKED → LAUNCHED dist 125 → "the off hand caught Realms object 412/0" → "412/0 (id 57) base 0x08 use -> firing" → "right hand: item 17 (Book)" → "caught and USED"; then 18 takeable | the headset; the package's metre is Doom's (reach/catch/throw speeds ~3x in Realms units) and its grip key codes are QuestZDoom's — owner's call |
| At-range pointing kept for what the grab doesn't take: trigger at range = examine, grip at range = use on switches/doors | 5efafc2a | — | — |
| 1:1 runtime fix: each Realms object had TWO actors (the binder ran before the loader spawned the props); now one ("242 object(s) bound to their loaded actors") | b8f514e1 | gestures_flat and held_flat re-run after it: every line as before (knock, attempts, punch 500 → 403 → 317, match, candles, niche 78 → 118 → 148 → 118, Lamp, Book, forced turn) | objects the logic moves/repaints beyond those runs |
| Item models (img3d): generated GLBs replace item pictures in the world and in the hand; pipeline on D:\VRealms_img3d | 6108c51c, 5c371b9c, a8f983b2 | placeholder boxes drawn on the table and in the hand | the owner's SETUP (CUDA 12.4 toolkit) and the first real models |

## 2026-11-16: chapter 2's world by hand (the owner's Q1-Q32), built; flat PC runs pending (the PC build was down)

Rules engine-free in `src/roth/roth_worldhandslogic.h` (`tools/worldhands_test`: 80 checks, 0 failed); the engine half
`src/roth/roth_worldhands.cpp` (its port checklist names each record and the click path it ends in). Every one ends in ONE
of the original's clicks through the hands' use path (VRPickUseAt) or its inventory clicks; no game logic added.
Test-only `roth_worldtest` (spot / stand / hold / move / grip / roll / head) on roth_xrpose's real controller path.

| change | kind | owner decision | chapter 2 records | evidence |
|---|---|---|---|---|
| Release a held Statuette (its base: the bottom of the card in the hand) or the Orb within 5 cm of a floor / face = the original's use there (the record at that texel, box included; the base's 3 cm disc). One release = one use | VR | Q1-Q3, Q14 | STUDY1 #119, #124-#129; MAS3 #4 | — (`worldhands_study1_flat`, `worldhands_mas3_flat`) |
| A placed statuette taken back = RSQ Distance Grab's walk-up grip (it offers them: their click gives i4), whose catch is the original's click #121 / #136-#141; the Orb in the door = the picker's grip on the face (#24) | VR (built) | Q3, Q14 | #121, #136-#141; MAS3 #24 | — (same cfgs) |
| Twist: the Shrive set (5 cm) and turned 180 degrees / Key i22 in the lock lever turned 30 degrees = the use from the item hand; a grip closing in the socket makes no use | VR | Q15, Q16 | AELF #80, MAS3 #113; MAUSO1EA #76 | — (`worldhands_aelf_flat`, `_mas3_`, `_mauso_`) |
| Pull: grip a lever / the chains and pull 8 cm; a bookcase leaf's edge (15 cm) or the clock's case door 4 cm = one use; the grip's close makes none (a leaf's middle: nothing) | VR | Q17, Q30, Q24 | MAS4 #3; MAUSO1EA #18, #30, #101; MAS6 #20; MAS7 #22; STUDY1 #353, #420, #75, #354, #35 | — |
| Press: the open palm 2 cm past a hand print's face = its click | VR | Q18 | MAS6 #42, #8, #41 | — (`worldhands_mas6_flat`) |
| Sit: the head 35 cm below standing, within 45 cm of the seat, facing away from its back = the use on the seat | VR | Q19 | MAS7 #7 / #33 | — (`worldhands_mas7_flat`) |
| Drink: a Vial within 10 cm of the mouth tipped 60 degrees for 6 tics = the original's use on self, by its own inventory clicks (InterfaceUseRightHandOnSelf); the eyes' examine waits while it is tipped | VR | Q26 | i143, i206, i170, i199 | — (`worldhands_mas3_flat`) |
| Contact disc: a fingertip (1 cm) / palm (4 cm) contact matching no record goes to the first point of its disc that does; the picker's palm also finds the flat under it (10 cm) | VR | Q21 | STUDY1 #57 (4 x 3 texels), every boxed record | — |
| Attempts: an empty palm pushed through a face fires the original's use there, else its examine; tomb lids pressed = their examine; Claymore / Scimitar tugged = their examine | VR | Q32 | STUDY1 #304, #325, #326; MAUSO1EA #5, #108; MAS6 #62; MAS7 #39; AELF #1, #95, #96, #107 | — |
| Ward tingle on Aelf's wards DEMO4[4273] only (followed while shown) | VR | Q31 | AELF obj 2 | — (`worldhands_aelf_flat`) |
| roth_held counts roth_xrpose as a headset (the item card was not drawn under it) | tool | — | all | — |
| Who owns a grip: a CLOSE goes to the Lamp's bail, then RSQ Distance Grab (an offered lying item: every chapter-2 statuette, the Orb on its ledge, the vials), then the pull / twist claims, then the picker's use; an OPENING with a held item is the release's (the package has no carry in the Realms: its catch is the original's use at once) | VR | 2026-11-16 grab mapping | all | — |

**Nothing built changes** falls (Q20), forced turns (Q29), darkness (Q27) or the torches' light (Q28): no file of those paths
was touched. **Q24, the clock's 0x1f, read in ROTH.C:** see the chapter doc entry 11.

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
| 18 | Record surface boxes (+0x0c..+0x12) tested against the hit texel | 1:1 fix (flat first) | — (a 1:1 gap) | all; ch1 (10, 11, 23, 26, 36); ch2 (5, 9, 13, 18, 29; floor boxes not yet seen) |
| 19 | Physical melee from RSQ Melee's idea (no combos, uppercut, slap, headlock); original damage; a punch into a switch or door is a use | VR | Q6 | all; ch1 (13, 16, 24, 38); ch2 (17, 22, every creature; the Sword needs N19) |
| 20 | Knock to examine (two raps of an open, empty hand) | VR | Q3 | all |
| 21 | A physical attempt plays the original's examine line (pattern) | VR | Q4 | all; ch1 (4, 5, 10, 14, 21, 28); ch2 (5, 13, 19, 41, 44, 53, 56, 61, 64) |
| 22 | Ward tingle haptic | VR | Q9 | all; ch1 (9 and the wards); ch2 (60: AELF's wards, Q31) |
| 23 | Tall wicks: point the lit match at a wick in reach + grip | VR | Q13 | ch1 (33); ch2 (3) |
| 24 | Strike a real match (lit match in the off hand, 20 s) | VR | Q1 | all; ch1 (31, 33); ch2 (3) |
| 25 | The Lamp on a hip hook, grabbed when needed | VR | Q2 | all |
| 26 | The picked-up item shows in the item hand | VR | Q12 | all |
| 27 | The Book opens in both hands; the key drops into the palm | VR | Q8 | ch1 (7) |
| 28 | The niche flash as a real dynamic light (three pulses) | VR | Q10 | ch1 (34) |
| 29 | The forced turn (0x3f) snaps during the black frames | VR | Q7 | ch1 (37); ch2 (1, 52, 58, 62, 65); every 0x3f |

## Decided, to build next (chapter 1 second pass, owner 2026-11-13)

| # | change | kind | owner decision | chapters |
|---|---|---|---|---|
| 30 | The original's movement ported 1:1 (step-up 65 / 16 crouched, crouch to 0x30, the 19-phase jump, speeds, turn ramp), diffed against the original | 1:1 | L4, "now" | all |
| 31 | Locomotion: smooth stick (default) or teleport, a setting; snap turn 45° | VR | L1, L2 | all |
| 32 | A real crouch drives the original's crouch; a button for seated players | VR | L3 | all; ch1 (27) |
| 35 | Movies on a world-locked screen | VR | M1 | all |
| 37 | A held item brought to the eyes plays its examine | VR | X1 | all |

## Decided 2026-11-16 (RSQ Distance Grab in VRealms)

| change | owner decision |
|---|---|
| Scale the package's metre to Realms' (reach.zs's two metre constants and its unit settings), so its reach, catch radius and throw speeds are real metres in VRealms — the ONE edit to the package | "Scale to Realms" |
| The package's grip keys default to VRealms' grips (445 right, 444 left), so its walk-up grab, grip-to-hold and door grip work | "Map to VRealms grips" |

## Decided, LATER

| change | owner decision |
|---|---|
| Real-parts reload with high-quality meshes (drop mag, insert, rack the slide, world collision, nothing on a sprite layer); still one magazine item per reload | W2 |
| On-gun ammo display, from REMASelacoVR's ideas | G1 |
| Dedicated left-hand support | Q11 |
| Openable books: the original only shows a book's page as code/popup; real books that open in the hands (pages to turn) are NEW CONTENT to build (owner 2026-11-16: "books do not open in game, it is just code. we will need to build openable books") | owner |
| Item models for the typewriter and other map-built furniture: the typewriter is made of map sectors (s306, s313-s316), not an item picture, so the image-to-3D pipeline needs a captured image of it as input | owner asked for a typewriter mesh, 2026-11-16 |

## Decided, nothing to build

| decision | chapters |
|---|---|
| Mirrors keep the original's painted, breakable glass (no live reflection) | ch1 (13, 38); every mirror |
| Far things (the casements): the pointer is enough | ch1 (18) |
| Candle light strength judged in the headset | ch1+ |
| Item 3D models parked; sprites for now; raise the sprite limit if models get there | all |
| Desks (STUDY1 roll-top, CHURCH1) never open in the original; opening them would be new content (owner's call later) | ch2 (19: STUDY1 #288, chapter 2 Q23); ch12 (CHURCH1) |
| Dedicated left-hand support later; until then the item is the right-side hand | all |
