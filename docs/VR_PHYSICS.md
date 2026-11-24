# VR physics: hands, objects, the world (VRealms)

Owner, 2026-11-24: "object / world / hand / player interaction is the single most important thing for this
project", and "our 1:1 rebuild has been satisfied, and now we are in the fun zone ... realms of the haunting
reimagined to take advantage of full 6dof for a shooter / adventure game hybrid". Milestones are reported to the owner;
rig checks only at milestones.

## What it is

- **Jolt Physics** (MIT, v5.3.0, `libraries/JoltPhysics`, vendored as is) runs a rigid-body world beside the game:
  `src/roth/roth_physics_core.{h,cpp}` (engine-free, `tools/physics_test`) and `src/roth/roth_physics.{h,cpp}` (the
  engine side). The game logic underneath is the original's: what can be picked up, reach, keys, chains. Physics
  changes how the hands do it.
- **The level is solid.** Every sector's floors and ceilings (its GL subsectors) and the solid parts of its lines
  (one-sided walls, steps, lintels, a closed door's whole height) are static triangles, metres, both faces. A sector
  that moves (a door, a lift) is rebuilt with its neighbours the same tic.
- **The hands are bodies** (after Godot XR Tools' collision hand and MinimalHandPhysics, both MIT): a dynamic body
  without gravity, driven each tic at the velocity that reaches the tracked hand in one tic, capped at 12 m/s and
  40 rad/s. Free, it is where the controller is. Against a wall or a desk it stops, and the drawn hand stops with it:
  `VRHandPose_SetBlocked` puts a correction in front of the tracked frame, so the drawn hand, what it holds, the
  picker's ray and the touch all stay on the surface at draw rate. A hit is felt (a buzz scaled by the closing speed).
  More than 0.6 m from its controller it is put back on the controller, but never through the level (unless 1.5 m
  off: the player has gone elsewhere). A hand a reach does not shove loose items away: it meets them only above
  2.5 m/s (a swipe knocks them over). Held 12 cm or more from the controller for 0.3 s, the hand slips through (the
  player means to reach there: an item behind a lip the original never had) and is solid again once it fits.
- **What a hand holds is part of the hand's body** (a compound shape), so a held item meets the world as the hand does.
- **Letting go anywhere but over the shoulder drops the item** (owner: "stays where it lands"). It leaves with the
  hand's velocity over the last three tics (the wrist's spin carries its far end), falls, tumbles, rests, and is drawn
  turned any way (the model's pitch and roll). The game puts it away by the same calls as a stow: carried, not
  selected. Over the shoulder (behind the face, above the chest) it is stowed into the pack as before.
- **A free palm that squeezes within 8 cm of a loose item takes it where it touched it**: the item keeps its place in
  the hand (the held model's seat), and the original's own calls select it into that hand.
- **A loose item goes back into the pack** when the player is 10 m from it, when it falls out of the level, when the
  game uses or removes it, when it is selected from the inventory, or when the level ends. Up to 16 lie loose at once.

## Units and frames

- Metres, map axes (x east, y north, z up), `VRPickUnitsPerMetre` map units a metre; the Realms maps' pixel stretch
  is 1.0 (`vrealms_iwad/MAPINFO`).
- A hand's frame: the grip socket, X along the hand, Y the back of the hand, Z = X x Y (`roth_physics_core.h`).
- An item's body frame: its model as drawn at yaw 0 in the world (`roth_img3d.cpp`: glTF (x, y, z) -> (z, x, y) times
  the metres per mesh metre). The loose actor's origin is the body's origin; its angles come from the body's rotation
  by `phys::ModelAngles` (GZDoom's model turn: Rz(yaw) Ry(-pitch) Rx(roll) in the map).

## Limits now

- One item in the hands at a time: the original selects one item. A free hand reaching for a loose item while the
  other hand holds one is refused (logged).
- Items still lying where the level put them are not bodies yet (picked up as before, by touch or point); only items
  that have been in a hand and let go are loose.
- Items without a 3D model are stowed when let go (no shape to drop).
- Sliding platforms (`roth_platforms`), polyobjects and 3D floors are not in the static world.
- A held item's weight does not slow the hand.

## Next

1. Doors, drawers, levers, switches, wheels: grabbed and moved along their track by hand (UltimateXR's translation
   and rotation constraints are the reference), firing the original's use at the original's threshold.
2. Sockets: an item brought to its lock, slot or keyhole is the original's use with that item (UltimateXR anchors).
3. Two hands on one object; two items held at once (the item in the hand that makes a use is selected at that use).
4. The level's own lying items as bodies (knocked, pushed), kept in step with the original's objects.
5. Cloth (the trenchcoat, curtains): XPBD, after Matthias Mueller's Ten Minute Physics cloth (the reference the owner
   linked, yesongO/cloth-vr-simulation, is a WebXR port of it).

## Proof

- `tools/physics_test/build.sh`: 18 checks, engine-free (a box rests 5 cm up, a hand stops on a wall's face, a held
  rod stops at its tip, a sweep pushes a box, a throw lands, the model angles come back exactly).
- `tools/claude_bridge/cfg/physics_flat.cfg` on the owner's PC: the hand stopped by a wall, the Helm let go and
  resting, taken again, thrown, stowed over the shoulder.
