# RSQ Distance Grab in VRealms: the copy's checklist (2026-11-16)

Source: `presidentkoopa/RSQ_UBQSeries` origin/main **eb0136239e**; `DistanceGrab/`'s newest commit there is
**b1c2a8f90b** (flight.zs +14/-1, the fallback word as a toast), after 9eb2766d ("the engine laser where there is
one (RemaSelaco), model beam elsewhere"). The owner's instruction: copy it completely, not a rewrite. Nothing in this
copy adds behaviour the package does not have: the zscript and every lump but CVARINFO are byte-identical to that
tree, and CVARINFO differs only by `noarchive` -- except the owner's two decisions of 2026-11-16, the only permitted
edits to the package, each marked below "changed: owner 2026-11-16": **"Scale to Realms"** (the package's metre made a
real metre in VRealms: reach.zs:497-503, mass.zs:40-41, and the CVARINFO defaults in map units) and **"Map to VRealms
grips"** (the grip key codes' defaults).

Where it lives: `wadsrc/static/filter/vrealms/` (the engine pk3). The engine moves `filter/<game>/` files to the end
of its pk3 under their own names when the game is VRealms (`resourcefile.cpp` PostProcessArchive, the IWAD's autoname
`vrealms`), so the package's own `zscript.txt` (its `version "4.14"`), `MAPINFO.txt`, `KEYCONF.txt`, `MENUDEF.txt`
... load after the engine's, exactly as the package's pk3 would load as a mod. Nothing to install on the owner's PC:
the normal build packs it.

Status words: **copied unchanged** (byte-identical, `cmp`), **changed** (why), **engine supplies** (where VRealms
provides what the package reads), **not copied** (why), **open** (a known divergence, not fixed here).

## A. Files

| # | Source (DistanceGrab/) | VRealms | Status |
|---|---|---|---|
| 1 | `zscript.txt` (31 lines) | `wadsrc/static/filter/vrealms/zscript.txt` | copied unchanged |
| 2 | `zscript/rsqdg/a11y.zs` (166) | `.../filter/vrealms/zscript/rsqdg/a11y.zs` | copied unchanged |
| 3 | `zscript/rsqdg/aim.zs` (448) | same path | copied unchanged |
| 4 | `zscript/rsqdg/cb.zs` (55) | same path | copied unchanged |
| 5 | `zscript/rsqdg/cone.zs` (196) | same path | copied unchanged |
| 6 | `zscript/rsqdg/door.zs` (504) | same path | **changed: owner 2026-11-16, scale to Realms** ("scale them too": x 107.4 / 34, each line marked `VREALMS`, the old value in it): `TOUCH` 7 -> 22.1 (hand to door face). The rest unchanged |
| 7 | `zscript/rsqdg/feel.zs` (725) | same path | **changed: owner 2026-11-16, scale to Realms** ("scale them too": x 107.4 / 34, each line marked `VREALMS`, the old value in it): `TREMOR_RADIUS` 64 -> 202 (the rumble quake's radius). The rest unchanged |
| 8 | `zscript/rsqdg/flight.zs` (1423, b1c2a8f90b) | same path | re-copied 2026-11-16 from b1c2a8f90b; **changed: owner 2026-11-16, scale to Realms** ("scale them too": x 107.4 / 34, each line marked `VREALMS`, the old value in it): `DUCK_MARGIN` 2 -> 6.3. The rest unchanged |
| 9 | `zscript/rsqdg/glow.zs` (100) | same path | copied unchanged |
| 10 | `zscript/rsqdg/grab.zs` (546) | same path | copied unchanged |
| 11 | `zscript/rsqdg/handsync.zs` (160) | same path | copied unchanged |
| 12 | `zscript/rsqdg/hold.zs` (901) | same path | **changed: owner 2026-11-16, scale to Realms** ("scale them too": x 107.4 / 34, each line marked `VREALMS`, the old value in it): `PALM_JUMP` 64 -> 202.2; the swing sweep's slack + 8 -> + 25.3; the throw assist's search 1200 -> 3790.6 and its 32 / 1200 band -> 101.1 / 3790.6. The rest unchanged |
| 13 | `zscript/rsqdg/laser.zs` (628) | same path | **changed: owner 2026-11-16, scale to Realms** ("scale them too": x 107.4 / 34, each line marked `VREALMS`, the old value in it): `LEAD_JUMP` 17 -> 53.7. NOT `CONE_L` / `BEAM_L` (256): they are the authored lengths of the two MESHES (tools/gen_cone.py), divided into the drawn length (`len / BEAM_L`, `reach / CONE_L`), so the beam already runs the real distance and the cone the real reach; scaling them would draw both 3.16x too short. (The 2026-11-16 note in #56 that called them a 2.4 m beam was wrong.) The rest unchanged |
| 14 | `zscript/rsqdg/lockon.zs` (445) | same path | **changed: owner 2026-11-16, scale to Realms** ("scale them too": x 107.4 / 34, each line marked `VREALMS`, the old value in it): `PUNCH_RANGE` 64 -> 202.2 and its search slack + 32 -> + 101.1; `Meets`' body slack + 8 -> + 25.3 (three places). The rest unchanged |
| 15 | `zscript/rsqdg/mass.zs` (415) | same path | **changed: owner 2026-11-16, scale to Realms**: mass.zs:40-41 `UNITS_PER_METRE` 32.0 -> 107.4 (a Realms object's size in real metres for the volume guess); the rest unchanged |
| 16 | `zscript/rsqdg/menurows.zs` (31) | same path | copied unchanged |
| 17 | `zscript/rsqdg/near.zs` (289) | same path | **changed: owner 2026-11-16, scale to Realms** ("scale them too": x 107.4 / 34, each line marked `VREALMS`, the old value in it): the walk-up search's slack + 8 -> + 25.3. The rest unchanged |
| 18 | `zscript/rsqdg/policy.zs` (594) | same path | copied unchanged |
| 19 | `zscript/rsqdg/reach.zs` (547) | same path | **changed: owner 2026-11-16, scale to Realms**: reach.zs:497-503 `UNITS_PER_METRE_XY` 34.0 -> 107.4, `PIXELSTRETCH` 1.2 -> 1.1462 (a comment above them says why). The package has no hook to read the engine's live metre (they are `const`, read by name in cone/flight/hold/throw/laser/near/lockon/grab.zs), so the constants are set: 107.4 is VRealms' VR scale (docs/ITEM_DESIGN.md); 1.1462 is Realms' FY/FX, the level's pixelstretch in the headset (rothmap.cpp ROTH_VIEW_VSTRETCH; flat keeps 1.0). The rest unchanged |
| 20 | `zscript/rsqdg/rsmenu.zs` (508) | same path | copied unchanged |
| 21 | `zscript/rsqdg/tell.zs` (280) | same path | copied unchanged |
| 22 | `zscript/rsqdg/throw.zs` (920) | same path | **changed: owner 2026-11-16, scale to Realms** ("scale them too": x 107.4 / 34, each line marked `VREALMS`, the old value in it): the pass-through search's slack + 8 -> + 25.3. The rest unchanged |
| 23 | `zscript/rsqdg/voxel.zs` (103) | same path | copied unchanged |
| 24 | `CVARINFO.txt` (110 cvars) | `.../filter/vrealms/CVARINFO.txt` | **changed**: every declaration carries `noarchive` (and a 4-line note at the top). VRealms shares the owner's `doomxr.ini`; an archived cvar would be written there on exit. Names, types, `user`/`server` scopes and defaults are the package's. Consequence: settings changed in its menu last for the session only. **Changed: owner 2026-11-16, scale to Realms** (each line marked `[VREALMS]`, the old value in it): `rsqdg_reach` 512 -> 1617 (15.06 m either way), `rsqdg_catch_radius` 24 -> 75.8, `rsqdg_flight_radius` 2.0 -> 6.3, `rsqdg_hold_vmax` 40 -> 126.4 (map units x 107.4 / 34); `rsqdg_air_flutter` 0.35 -> 1.1 (the same); `rsqdg_air_gravity` 0.27 -> 0.75 (9.81 m/s^2 at 107.4 / 1.1462 = 93.7 vertical units a metre, over Doom's 1 unit/tic^2 at 35 tics/s). Every cvar in metres, m/s, kg or degrees is unchanged: the package converts those through reach.zs's constants. **Changed: owner 2026-11-16, map to VRealms grips**: `rsqdg_keys_main` "446" -> "445", `rsqdg_keys_off` "443 436" -> "444 436" (see #43). Still `noarchive`. |
| 25 | `KEYCONF.txt` | same | copied unchanged (two key rows, no default binds, four aliases) |
| 26 | `MAPINFO.txt` | same | copied unchanged (the 15 handlers in the package's order) |
| 27 | `MASSDEF.txt` | same | copied unchanged |
| 28 | `MENUDEF.txt` | same | **changed: owner 2026-11-16, scale to Realms** ("scale them too": x 107.4 / 34, each line marked `VREALMS`, the old value in it): the Catch Distance slider 8..64 step 4 -> 25.3..202.2 step 12.6 (one decimal), so the default 75.8 is inside it. The Reach slider (64..2048) already holds 1617 and is unchanged. The rest unchanged |
| 29 | `MODELDEF.txt` | same | copied unchanged |
| 30 | `SNDINFO.txt` | same | copied unchanged; **open**: it aliases Doom's `dsskeswg` and `dspunch`, which a Realms install does not have, so the whoosh and the thud are silent |
| 31 | `TRNSLATE.txt` | same | copied unchanged |
| 32 | `CREDITS.txt` | same | copied unchanged |
| 33 | `graphics/rsqdglo.png` | same | copied unchanged |
| 34 | `models/rsqdg/rsqbeam.md3`, `rsqbeam.png`, `rsqcone.md3`, `rsqcone.png` | same | copied unchanged |
| 35 | `sprites/rsqdg/*.png` (50) | same | copied unchanged |
| 36 | `README.md`, `docs/DECISIONS.md`, `docs/PORT_SCOPE.md`, `docs/HANDOFF_2026-10-26.md` | `docs/rsq_distancegrab/` | copied unchanged (documentation, not in the pk3) |
| 37 | `docs/readme/*.jpg`, `tools/gen_cone.py`, `tools/rsqcone_12deg.obj`, `tools/rigtest/*`, `build.ps1`, `.gitignore` | -- | **not copied**: the README picture, the cone generator (its output, `rsqcone.md3`, is copied), the QuestZDoom Linux rig harness and the Quest pk3 build script. None of it is loaded at run time. |

## B. What the package reads from the engine, and where VRealms supplies it

| # | The package reads | Source | VRealms | Status |
|---|---|---|---|---|
| 38 | `AttackPos` / `OffhandPos` (the palm) | reach.zs:443 `Palm` | the OpenXR device writes them every rendered frame (vk_openxrdevice.cpp, the gameplay block); the playsim keeps the main one from the controller (p_user.cpp UpdateCanonicalMainHandPose) | engine supplies (unchanged) |
| 39 | `AttackAngle`+90 / `OffhandAngle`+90 (yaw), `AttackPitch` / `OffhandPitch` (pitch, positive up) | reach.zs:33, :54 | the same writers; the stored convention is the one reach.zs:19-29 describes (world yaw less 90, pitch positive up) | engine supplies (unchanged) |
| 40 | `AttackRoll` (main) / `OffhandRoll` (off): the palm gate's quarter turn | reach.zs:65, :114 `PalmPose` | `AttackRoll` was zeroed by the playsim every tic (netplay determinism), so the main hand could never pass the palm gate. Single-player VR now keeps the device's roll there: p_user.cpp `if (!multiplayer && isLocalVr) AttackRoll = MainHandRoll` | **engine supplies (new)** |
| 41 | `OverrideAttackPosDir` | handsync.zs:67, reach.zs:457 | p_user.cpp (VR, or the controller-pose test) | engine supplies |
| 42 | `WF_TWOHANDSTABILIZED`, `OffhandWeapon`, `ReadyWeapon` | reach.zs TwoHandWhy, BusyWhy | present in the engine | engine supplies |
| 43 | Grip keys as raw key codes, defaults "446" (main) / "443 436" (off) | near.zs:64-65, CVARINFO | VRealms' OpenXR posts the grips as `KEY_PAD_LSHOULDER`/`RSHOULDER` + hand (444 left, 445 right). With the package's defaults its grip reads (walk-up grab, grip takes a carry, grip takes a door) see nothing, so VRealms' own in-reach grip (= use, the owner's rule) is the only grip. To try the package's grip: `rsqdg_keys_main 445; rsqdg_keys_off 444` (session only). | **changed: owner 2026-11-16, map to VRealms grips**: defaults `rsqdg_keys_main` "445" (KEY_PAD_RSHOULDER, the right grip = the main hand right-handed) and `rsqdg_keys_off` "444 436" (KEY_PAD_LSHOULDER; and KEY_PAD_DPAD_UP, which the device sends for the off grip while the main grip is held as the shift, vk_openxrdevice.cpp `gripAltKey` -- the package's own "443 436" carried the same alternate). The main grip under `vr_secondary_button_mappings` (default on) reaches the package as a clean tap's one-frame pulse at its release (vk_openxrdevice.cpp suppressGripButton), not at the press. **Changed: owner 2026-11-16, "grab on squeeze"** (glue, the package unchanged): on that hand, with that setting, with live controllers, VRealms hands the package the press itself on the ANALOG squeeze -- its own `rsqdg_ngrip` network event, hand 0, 1 (what near.zs InputProcess sends for the key), and 0 on the analog release -- when the package is ready for that hand (the same `GrabGlueTakesGrip` that keeps the double-fire guard), and swallows the tap's late key pulse before the event handlers (g_game.cpp G_Responder -> roth_grabglue.cpp `GrabGlueResponder`) so the press is not sent twice. No mapping or setting changes. The off grip and the main grip with secondary mappings off already send their key at the press: untouched. ONE MOD PER PRESS: the package eats the press whenever its walk-up grip would take something (near.zs InputProcess, `ready[h]`); VRealms' own in-reach grip reads the analog grip, not the key, so it asks the package first and stands down for that press (roth_vrpick.cpp TickHand -> roth_grabglue.cpp `GrabGlueTakesGrip`, reading `RSQDG_Near.ready`): the package takes an offered item (and a carry its palm caught); VRealms' grip keeps doors, switches, levers, faces and every object that is not an offered item. |
| 44 | Haptics: the pulse (P_LineAttack's buzz on the firing hand) and the rumble (A_QuakeEx -> controller) | feel.zs:22-130 | p_map.cpp:5204-5215 (`Vibrate(150, hand, 0.8)` before the trace), r_utility.cpp:1213-1219 (`vr_quake_haptic_level`) | engine supplies (unchanged) |
| 45 | The engine laser: `ServiceIterator.Find("RSLaserService")`, "available", "beam", "beam.off" (slots 24/25) | laser.zs:141-196 | **copied from RemaSelaco 383ef89c**: `src/rendering/hwrenderer/scene/hw_rslaser.cpp` (drawn from hw_drawinfo.cpp DM_MAINVIEW beside UZDXREMA's own laser sight) and `wadsrc/static/zscript/rs_laser.zs`. One change: its six `rs_laser*` cvars are not archived (the ini rule). With it the package draws UZDXREMA's laser look; `rs_laser 0` gives the package's model beam and sprite dot. | engine supplies (new, copied) |
| 46 | `Wads.FindLump("MASSDEF")`, `Wads.ns_voxels`, `VOXELDEF` | mass.zs:307, voxel.zs:62-70 | the engine's Wads API | engine supplies |
| 47 | `ServiceIterator`, `BlockThingsIterator.CreateFromPos`, `CheckSight`, `GetReplacee`, `A_QuakeEx`, `A_SetSize`, `LineTrace` ... | throughout | every identifier the package calls was checked present in VRealms' engine ZScript (a grep over its call sites; not a compile) | engine supplies (checked by name only) |
| 48 | Doom's classes in the table (`ExplosiveBarrel`, `HealthBonus`, `Weapon`, `Ammo`, `Key`, ...) | policy.zs:214-297 | present in the engine's ZScript; none is ever spawned in a Realms map, so those rows never match | engine supplies |

## C. The glue (what the grab may take, what a catch does, in Realms terms)

| # | Behaviour | Package | VRealms | Status |
|---|---|---|---|---|
| 49 | What a hand may pull: the policy table, first match wins; `Inventory` is its catch-all row ("a pickup in the world", weight 1) | policy.zs:211-297 `Build`, :368 `Decide` | an object whose picture IS an item (is_item_id_pickable: its render id is a pickable DBASE100 record's) and whose +9 has neither 0x12 bit is spawned as `RealmsTakeable : Inventory` (roth_objects.cpp `Pending::plainPicture`, `TakeableClass`; realmsgrab.zs) -- LoaderProp's flags, NOINTERACTION included. Meshes stay LoaderProp. An item hidden at load and shown later by the level logic (STUDY1: 9 of its 28, among them the Matchbox 271/1 and six candles of item 4) is given its actor by the actor rules (roth_actors_engine.cpp -> `SpawnObjectActor(..., item)`) as RealmsTakeable too. DBASE100 is loaded before the objects are prepared (it used to load only at BeginLevel, after them: on the first map, STUDY1, no object was an item -- the owner's PC run of 6b8abd03, "may take 0") | glue |
| 50 | Never a NOINTERACTION actor, whatever the table says | policy.zs:374 | every Realms object keeps NOINTERACTION -- refused by the package's own Decide -- unless the original's left click picks it up: roth_runtime.cpp `ObjectIsPickupItem`, after ROTH.C activate_targeted_object (input.c:1026-1066): it is an item (above), and either a use record of its id (0x08/0x02) has a chain holding a 0x29 give_item of THAT item or of key 0 (the clicked object) -- the Key 21, the Book 17, item 4, item 45, item 51 (key 0) ... in STUDY1 -- or it has no record and the type-4 branch would give it (`InterfaceObjectTakeable`). Those lose NOINTERACTION and take their pick box as their size (roth_grabglue.cpp `MakeTakeable`, once a second). Not offered: an object whose use gives some OTHER item (a box, a drawer, a picture that hands something over), and one whose id binds only an examine record (this port's ActivateObject does nothing on its use; the original would give it -- not changed here) | glue |
| 51 | A caught pickup is used: `TakeInto` asks every "GrabEventService" first ("pickup.first": 1 = given to this hand and taken away) | flight.zs:601-625, feel.zs:714 `RSQDG_Events.Ask` | `RealmsGrabEventService` (realmsgrab.zs) answers for a RealmsTakeable: `RealmsGrab.TakeCaught` -> roth_vrpick.cpp `VRPickTakeCaught` -> `DoUse` -> `Activate(Use)` -> the type-4 branch -> `give_item_by_dbase_id` (InterfaceObjectAction), gated by the original's cursor from the palm. Log: `[roth_grab] ... caught Realms object s/i`, `[roth_vrpick] ... distance grab catch: USE ... -> acted`, `[roth_ui] picked up item N (name)` | glue |
| 52 | The original refuses (an item still flying to the inventory, GiveItem's own answer) | flight.zs:644-678 (the pickup refused -> claim -> carry) | the service answers 0; `RealmsTakeable.TryPickup` is false; the package carries it as anything it caught | glue |
| 53 | A Realms item at rest | -- | RealmsTakeable does not tick until something moves it (realmsgrab.zs `Tick`), so P_ZMovement never lifts one the map placed below its floor (loaderprop.zs's measured reason for NOINTERACTION) | glue |
| 54 | Walking into an item | Inventory +SPECIAL | `-SPECIAL`: the original takes an item by its click only | glue |
| 55 | The at-range pointing | -- | KEPT (owner, "far things: Pointer fine"; lead 2026-11-16): the finger beam and ring marker, the trigger's examine at range and the grip's use at range on what the grab does not take (switches, casements, doors beyond reach), along the corrected finger ray. A takeable item at range is the grab's: the grip while pointing at one logs "RSQ Distance Grab takes it" and does nothing (roth_vrpick.cpp TickHand, `ObjectIsPickupItem`). In reach, unchanged | glue |

## D. Known divergences (open)

| # | What | Why it matters |
|---|---|---|
| 56 | **Changed: owner 2026-11-16, scale to Realms** (#15, #19, #24). What remains: the headset's live metre is the eye anchor (rothmap.cpp: 144 x 1.1462 / the player's measured head height, e.g. 103 at 1.60 m, 94 at 1.75 m), and the package's `const` 107.4 cannot follow it; flat keeps pixelstretch 1.0 and vr_vunits_per_meter 34 unless `roth_vr_units_per_metre` is set (the bridge cfgs set 107.4). The map-unit literals in the package's code: **changed: owner 2026-11-16, scale to Realms** ("scale them too"; #6, #7, #8, #12, #13, #14, #17, #22, #28), except laser.zs CONE_L / BEAM_L, which are mesh lengths, not distances (#13). Left: the iterators' `checkh` 128 and door.zs's 8 (the height portal groups are collected over, not a distance test). | A real metre is now a real metre at 107.4 units; at a head whose anchor gives another metre the error is that ratio (about 4-14%). The literals are now Realms-sized: a door touch at 22.1 units (0.21 m, as 7 was at 34), a punch range of 202.2 (1.9 m, Doom's MELEERANGE in metres). |
| 57 | Mesh objects (EntryKind::Object3D) stay LoaderProp: their model frames are bound to that class | a Realms item drawn as a mesh is not offered to the grab |
| 58 | The chain's own conditions are not evaluated (a 0x29 behind an if-flag counts) | the catch runs the original's use itself; if it gives nothing the package carries the thing |
| 59 | The pick tracer now meets takeable items' cylinders (they left NOINTERACTION) as well as their boxes | the nearer of the two wins; both name the same object |
| 60 | ZScript compile | no engine binary here: the package and the glue were not compiled. The first PC run is the compile check (the engine stops with the error and its line if anything is wrong). |
