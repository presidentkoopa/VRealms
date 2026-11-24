# Things worked by hand (VR physics milestone 2)

2026-11-24. Doors, levers and keys move along their track in the hand, and fire the ORIGINAL's use when they pass
the threshold. The original's game logic underneath is unchanged: the chain on the door's face (or the lever, or the
lock) decides what happens, exactly as a click would. Part of docs/VR_PHYSICS.md (its "Next" item 1).

Marks: **[V: how]** verified on the owner's PC, and how; **[R]** reasoning only, not seen.

## 1. What a hand does now

**Doors** (every swinging door of a level: the 167 room doors and the 8 cupboard and wardrobe leaves are one kind in
the original, docs/WORLD_DESIGN.md 1.1, docs/OPENABLES.md 2):
- A free hand closing within 10 cm of a door's face, on its latch half, takes hold of the door. (A hand with an item
  drawn in it takes hold too; a hand whose item is the key that door tests is left to the key, below.)
- **Shut door:** the leaf does not move. The hand drawn 4 cm along the door's swing (WORLD_DESIGN.md 1.3, the owner's
  door gesture) is the original's use on the door's face, once per grip, from that hand (its item selected for the
  use). The chain decides. If the door opens, the leaf is the hand's from that moment.
- **Open door:** the leaf follows the hand about its hinge, between shut and 90 degrees. The hand is drawn where it holds
  the leaf. The door's own swing waits meanwhile.
- **Let go:** a leaf swung harder than the original's swing (98 degrees a second) carries the hand's swing on and slows
  (VRLever's RetainMomentum); then the original's own swing finishes its arc from where the leaf is, at its speed, with
  its dwell and its close as before. Driven swings land on exactly 90 degrees.
- **Close by hand:** a leaf brought back within its latch of shut (the same 4 cm of arc) and let go there is the
  original's use on the open door's leaf: toggle_door_open_state (below). It closes from there.
- **Too far from the controller:** 30 cm off its hold on the leaf, the hand lets go [R: the number].
- **Slack** (roth_track.h Slack, ours): a hand closing on a door may be drawn on its face while its controller has gone
  on into it. The controller must come back to the drawn hand before it moves the door along that line; motion across
  counts at once. Without it, a player leaning into a door and relaxing reads as a pull.
- Which way a door opens from where the player stands is SwingDoor's own decision (the hinge marker, doors.c:523-531).

**Levers** (the owner's table: MAS4 #3, MAUSO1EA #18 #30 #101, MAS6 #20): the lever is an arm 20 cm long [R: no model
yet] out from the wall to the hand, hinged at the wall. Pulled down along its arc by the owner's 8 cm (Q17, about 23
degrees), it is thrown: the original's use once. The hand stays on the arm's end, on its arc. The lever's picture is the
original's, a flat picture; its thrown picture is the original's own 0x0d swap. A lever that visibly swings waits for its
3D model.

**Keys in locks:** an item the use at a spot tests in hand (its record's chain has a 0x27 keyed to that item,
raw_commands.c:4760) is a key for that spot. The owner's Q15/Q16 table had two (the Shrive, Key i22); now every key and
every lock: the 15 KEY doors (their handle plate is the lock), the locked cupboards, any socket that tests an item. Set it
there with the grip closed and turn it 30 degrees (the owner's key turn, Q16) either way: the original's use from that
hand. The hand is drawn in the lock while it turns. The Statuette and the Orb are set down, not turned (THE RELEASE).

**The original's close, ported** (`roth_runtime.cpp` ToggleDoor, from E/doors.c:905-912 toggle_door_open_state,
E/audio.c:956-970 restart_door_open_sound, E/doors.c:1031-1195 tick_swinging_doors; source
`E:\DOOMWork\VRealms\tools\ROTH.C\roth_c\src\engine`). A use on an OPEN door's leaf (the original's door object,
pick type 6, E/input.c:1087-1098) toggles it: opening or standing open -> closing from where it is; closing -> opening
again. Its sound is restarted as the original's (the door's +0x26 sound). The port had never had it: a second click on
an open door used to re-fire the room face's chain. And a blocked swing now WAITS and tries again, opening or closing,
as the original's tick reverts its step (doors.c:1094-1099, 1166-1173); the engine's Hexen door turned a blocked closing
swing back open. Engine: `po_man.h` EV_TogglePolySwing, EV_PolySwingWaits (DPolyDoor::m_WaitIfBlocked), inert until
called. A door standing open for good (dwell 0, no swing left) gets a new closing swing at the speed it was opened with.

**Bookcases and the pit's chains** keep the owner's own rules (Q30: a leaf pulled 4 cm from its edge; the chains 8 cm):
the general door rule leaves the owner's pull table alone.

**No drawers, wheels or valves exist in the game** (docs/OPENABLES.md 1: "There are no drawers that open anywhere"; the
three drawer lines are refusals). The clock's hour hand is the one dial (THE HOUR HAND, unchanged).

## 2. Where it lives

| piece | file |
|---|---|
| the tracks, engine-free (Hinge, Dial, Slider, Button), ported from VRExpansionPlugin | `src/roth/roth_track.h`, test `tools/track_test/run.sh` (52 checks) |
| doors in the hand | `src/roth/roth_handworks.{h,cpp}` |
| levers on their hinge, any key in its lock | `src/roth/roth_worldhands.cpp` (THE LEVER ON ITS HINGE, LockTests, kAnyKey) |
| a hand held on a track (drawn there, meeting nothing) | `src/roth/roth_physics.{h,cpp}` PhysicsHandPin |
| a door's hinge, the door of a pick, a record's chain | `src/roth/roth_runtime.{h,cpp}` DoorHingeOf, DoorTagOfPick, RecordChain, DoorHold, DoorTurnHeld |
| **engine:** a polyobject turned by an outside driver | `src/playsim/po_man.{h,cpp}` EV_HoldPoly, EV_TurnHeldPoly, EV_PolySwingState; FPolyObj::bHeldByDriver; DPolyDoor::m_Driven |

The engine change is general and inert until called: nothing but a caller of EV_HoldPoly sets the hold, and a swing
nothing has driven steps exactly as before. Also new in the physics: a controller that jumps 0.6 m or more in one tic
(the player moved by a script, a teleport or the level) takes its hand with it, through whatever is between.

Test instruments: `roth_handworks_doors` (every door of the map, its hinge, and what the use on each face beside it does:
opens / tests an item / refusal line), `roth_worldtest spot door <tag> [far]`, `roth_worldtest spot <record> handle [far]`.
Configs: `tools/claude_bridge/cfg/handworks_flat.cfg`, `handworks_key_flat.cfg`, `handworks_doors_census.cfg`.

## 3. The porting checklist (CLAUDE.md's porting rule)

Source: VRExpansionPlugin (MIT, Joshua Statzer), github.com/mordentral/VRExpansionPlugin at commit 3ced69d0, folder
`VRExpansionPlugin/Source/VRExpansionPlugin/{Private,Public}/Interactibles/`. The whole of VRLeverComponent.cpp (867
lines), VRDialComponent.cpp (680), VRSliderComponent.cpp (1045), VRButtonComponent.cpp (462), VRMountComponent.cpp (633),
their headers and VRInteractibleFunctionLibrary.h (303) were read before porting. Licence copied to
`docs/licenses/VRExpansionPlugin.txt`. "rt" = `src/roth/roth_track.h`.

### 3.1 The helpers (VRInteractibleFunctionLibrary.h and the Unreal maths they call)
1. GetAtan2Angle Axis_Z (VIFL.h:71-92) -> rt:111 Atan2AngleZ. Axis_X / Axis_Y: not ported; every caller puts its axis on
   +Z of its own frame (the source's three cases differ only in which two coordinates go into atan2).
2. GetDeltaAngle / GetDeltaAngleFromTransforms (VIFL.h:94-109) -> not needed: the port keeps each thing's angle as a
   number (its state) instead of reading it back from a transform.
3. SetAxisValueRot / Vec, GetAxisValue (VIFL.h:111-190) -> not needed (one axis).
4. Interactible_GetThresholdSnappedValue (VIFL.h:291-299) -> rt:113 ThresholdSnapped.
5. FRotator::NormalizeAxis / ClampAxis, FMath::FindDeltaAngleDegrees, FInterpConstantTo, FInterpTo, VInterpTo, GridSnap
   (Unreal, not in the plugin) -> rt:58-109. FMath::ClampAngle -> rt:74 [R: reproduced from Unreal's library as
   remembered; track_test checks the lever's cases].
6. Interactible_GetCurrentParentTransform, InitialRelativeTransform (every component) -> the caller's rest frame: the
   hand's point arrives already in it (rt header comment).

### 3.2 VRLeverComponent -> rt Hinge (rt:133-325)
1. Defaults (VRLever.cpp:28-69): limits 0 / 90, toggle 0.8, ReturnToZero, return speed 50, frames 3, friction 5, max
   momentum 180, break 100 cm -> rt:136-149 (break in metres).
2. ProccessCurrentState (:222-250): the toggle at limit x percentage, events only when not lerping (or always),
   bUngripAtTargetRotation -> rt:174 ProcessState.
3. CheckAutoDrop (:257-276): the grip point on the component vs the controller in the component's frame -> rt:221
   (UnturnZ for the component's turn). OnGripOutOfRange broadcast: not ported (no listener; the drop is the outcome).
4. TickGrip single axis (:278-375, the Axis_X/Y/Z case :354-362) -> rt:228 TickGrip; locked + autodrop (:280-289) ported.
5. TickGrip Axis_XY / FlightStick_XY (:299-353) -> NOT ported: no two-axis lever or flight stick in the game.
6. The momentum sample taken in TickComponent while held (:200-209) -> inside rt TickGrip (one tick per tic here).
7. OnGrip single axis (:377-452): InitialGripRot, RotAtGrab, InitialInteractorDropLocation, first-tick flag, momentum
   reset -> rt:208 Grip. FlightStick offsets (:388-404): not ported (5).
8. OnGripRelease (:454-470) -> rt:252 Release.
9. CalculateCurrentAngle single axis (:726-734) -> the angle is the state (rt `angle`); XY branch (:688-725) not ported (5).
10. LerpAxis (:738-812): LerpToMax, LerpToMaxIfOverThreshold, RetainMomentum (friction, |momentum| as speed),
    ReturnToZero, restitution bounce -> rt:259 LerpAxis.
11. CalcAngle (:814-840): atan2 less the grip's, plus the angle at grab, ClampAngle to the limits, the 360-lever case,
    the flip guard -> rt:190 CalcAngle.
12. TickComponent while not held (:139-220): the lock ends a lerp, the lerp, the state, the finished-lerp event ->
    rt:296 Tick.
13. SetLeverAngle (:648-668) -> rt:312 SetAngle (the angle set by the original's swing or a stop).
14. Replication, gameplay tags, grip interface plumbing (slots, priorities, stiffness, late updates, teleport
    behaviour, secondary grips, OnUsed...): not ported -- Unreal engine plumbing with no game meaning here; the hand's
    claim is roth_handstate.h's (L2), the network is not in this lane.
**Used by:** doors (roth_handworks.cpp:292 HandworksGripTaken, :187 TickHold, :250 TickCoast; limits 0..90, toggle 1.0,
RetainMomentum, break 0.30 m) and levers (roth_worldhands.cpp:803-836, :960-1000; toggle at the owner's 8 cm of arc,
LerpToMaxIfOverThreshold, break 0.30 m).

### 3.3 VRDialComponent -> rt Dial (rt:327-466)
1. Defaults (VRDial.cpp:27-57) -> rt:330-339.
2. TickGrip, both modes (:146-209): the point going round (limit test against both maxima) and direct hand rotation ->
   rt:405 TickGrip. Direct mode: the source subtracts two Euler rotators (:188-190); the caller here measures the hand's
   own turn about the axis (a correct twist where Euler differences mix axes away from identity).
3. OnGrip (:211-239) -> rt:393 Grip.
4. OnGripRelease, the increment snap (:272-312) -> rt:433 Release; the snap LIST (:243-271, :564-594) not ported (no
   dial in the game has listed stops).
5. AddDialAngle (:504-631), the limits, rollover, increment snapping and its event -> rt:351 AddAngle.
6. TickComponent, the lerp back (:115-144) -> rt:454 Tick. SetDialAngle (:498-502) -> rt:466.
**Used by:** nothing in the game yet. The keys keep the owner's tested TWIST rule (roth_worldhandslogic.h wh::Twist, the
same as the Dial's direct rotation without limits; worldhands_test checks it), now with the hand held in the lock.

### 3.4 VRSliderComponent -> rt Slider (rt:476-622)
1. Defaults (VRSlider.cpp, constructor) -> rt:479-494.
2. ClampSlideVector (:743-774) non-legacy, snap -> rt:507 Clamp.
3. GetCurrentSliderProgress no spline (:847-893) -> rt:516 ProgressOf.
4. CheckSliderProgress (the hit-point events and the event-throw threshold) -> rt:530 CheckProgress.
5. OnGrip / TickGrip / OnGripRelease / TickComponent's non-spline lerp with restitution -> rt:550-622.
6. Splines (SplineComponentToFollow, lerped keys, linearity) and per-axis progress: NOT ported (no curved track in the
   game).
**Used by:** nothing yet: the game has no drawer or slide that opens (docs/OPENABLES.md 1; the Study chest slides by its
own script).

### 3.5 VRButtonComponent -> rt Button (rt:628-702)
1. Defaults, Press / Toggle_Return / Toggle_Stay, engage depth, min time between, the return lerp, the press-button
   check during the lerp (VRButton.cpp, TickComponent) -> rt:655 Tick. The overlap begin/end (OnOverlapBegin/End) is the
   caller's `inside` each tic. IsValidOverlap / interacting-actor bookkeeping: not ported (one player's hands).
**Used by:** nothing yet: MAS6's hand prints keep the owner's tested PRESS rule (wh::Press: the same engage-once-per-push).

### 3.6 VRMountComponent -> NOT ported
A two-axis gun mount with flip zones (VRMount.cpp:116-371). The game has no mounted gun or turret.

### 3.7 Ours, not the source's (named so nothing here is mistaken for a port)
- The latch (roth_handworks.cpp TickHold): a shut leaf does not move until the original's use opens it.
- The handover to the original's swing (EV_HoldPoly / EV_TurnHeldPoly, the m_Dist bookkeeping, m_Driven's last step).
- The hold starting where the hand is drawn (Hold::handAtGrab, VRSlider's InitialGripLoc idea applied to every track).
- The lever's arm (20 cm, out from the wall at the hand) [R].
- Any key in any lock (LockTests over the record's chain).

## 4. Proof

**Engine-free:** `tools/track_test/run.sh` 52 / 0 (helpers; a door's hinge: angle, toggle once, stops, no flip, height
ignored, break; return to zero in 28 tics, LerpToMax, LerpToMaxIfOverThreshold, momentum carrying on; a lever thrown at
its arc; a key's twist, its stops, its lerp back; a knob turned by the point; notches; a slider's progress, its ends'
events, momentum; buttons toggle and press). `tools/worldhands_test` 87 / 0 (unchanged rules).

**On the owner's PC, flat** (build 2026-11-24 17:5x, the bridge's hidden desktop):
- [V: log-handworks_flat.txt] Door 6 pulled by its handle: "takes door 6 by its handle, shut (0.69 m from its hinge)",
  the latch at 3.4 degrees -> the original's use, "Open. Good.", "the leaf is the hand's"; swung by the hand to 25.8
  degrees; let go; the original's swing finished it at 90 degrees. Door 5 pushed from its far side: the latch at 4.3
  degrees, "Open. Good.", swung to 21.6 degrees, finished at 90. Captures hw_A1_*, hw_A2_* in the bridge's logs.
- [V: log-handworks_key_flat.txt] Key i21 set in the Study door's lock (door 4, its keyed side, record #11) with the grip
  closed, turned 31 degrees: "the original's use -> acted", the original's 0x27 passed ("right hand 21 -> 21"), the
  door opened to 90 degrees. A key that is not in the owner's table: the generalisation works.
- [V: log-worldhands_mas4.txt] The MAS4 store-room lever on its hinge, 0.20 m from its pivot, pulled down: thrown at 24.9
  degrees, "the original's use -> acted".
- [V: log-worldhands_mauso.txt] MAUSO1EA: Key i22 turned 32 degrees in the lock lever -> acted; levers #18, #30, #101
  thrown -> acted.
- [V: log-worldhands_study1.txt] The bookcase leaf (#353) keeps the owner's pull: the middle "nothing", the edge pulled
  0.04 m -> acted. Plates #119 and #124 fire.
- [V: log-physics_flat.txt, log-interact_flat.txt] M1 unchanged: the wall stop, the Helm dropped LOOSE, taken where it
  lay, thrown at 3.91 m/s, stowed over the shoulder; touch and point pickups, the too-far refusal, the keyed object.
- [V: handworks_doors_census] STUDY1: 30 swinging doors found, each with its hinge and its faces' uses.

- [V: log-handworks_key_flat.txt] A LOCKED door through the hand: door 2 (record #408), the handle drawn 3.8 degrees ->
  the original's use, "Locked.", "the door stays shut" (0 degrees after). Key i21 then opens the Study door (door 4).
- [V: log-handworks_close_flat.txt] THE CLOSE. Door 6 open at 90, a use on its leaf (`roth_handworks_leafuse 6`, the
  hand's RulesUse): "toggle_door_open_state -> toggled", "now CLOSING"; censuses at 1, 2, 5, 10, 16, 26 tics: 87, 84, 76,
  62, 45, 17 degrees, at 46 tics 0 with no swing left -- 90 less 2.8125 a tic, the original's step at M 1 (one 1.40625
  degree unit per 70 Hz tick, doors.c:1043-1045, 1113-1125), exactly. Door 5 pushed open by the hand to about 20
  degrees, brought back to shut and let go: "brings door 5 back to 0.0 degrees ... the original's use on the leaf -> it
  closes"; then 0, no swing.

**Not seen, or not yet working:**
- The close is checked against the original's arithmetic (above), not diffed against the running original (no ROTH.C
  oracle run was made: that needs a probe mod in the headless build). The closing sound is the port's door-sound tracker's
  (roth_sound.cpp TickDoors), not listened to.
- The statuette's own "set down" line in worldhands_study1 says "nothing within 0.05 m" although the plates fire; the
  code it runs was not touched here, and whether it predates this work was not checked (no pre-change build was run).
- A door let go of within its latch of shut is closed; let go anywhere else, an opening door finishes
  opening (the game's state is "open"); a door standing open for good stays where the hand leaves it (no swing to finish).
- The test hands (roth_worldtest) jerk the controller when a hand starts being held on a track; the door numbers above are
  the doors' own lines.
- Nothing in the headset. Haptics are logged, not felt. The test hands are driven by roth_worldtest, which fights a
  hand held on a track (its correction reads the controller then); the numbers above are the doors' own lines.
- Sounds: the original's door sound plays as before (the use's own 0x2f); nothing new while the hand swings it.
