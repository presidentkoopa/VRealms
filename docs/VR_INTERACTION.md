# VR interaction: one system, built on the original's cursor (design, 2026-11-23)

Status: DESIGN. Nothing here is built yet unless a line says so. This replaces the RSQ Distance Grab port.

## 0. Why

Realms of the Haunting already has one interaction system: a cursor. Each frame the original finds what is under
the cursor, classifies it, and offers two verbs (docs/GAME_core.md 4.3-4.5):
- **The pick:** `render_world_view` returns the first opaque texel under the cursor. The record holds the hit type
  (wall face, floor, ceiling, mid-platform, object, door), its ids and a depth word.
- **The cursor class:** `classify_cursor_target_object` (input.c:1111-1258) sets nothing / usable / blocked-or-keyed.
- **Reach:** the depth word gives use < 268 units, "too far" < 968, examine < 2467 (REMAROTH_MEASURED.md 13).
- **Use:** `activate_targeted_object` (input.c:1026-1100) does one of:
  - runs the object's chain, or else picks the object up;
  - toggles a door;
  - fires a wall's 0x18 records or a floor's 0x19 records.
- **Examine:** `examine_object_under_cursor` (input.c:979-1024) fires the 0x30/0x31/0x32 records or the item's OnInspect.

The RSQ Distance Grab port was a second, unrelated targeting system:
- it chose targets by its own cone and score, with its own reach (metres beyond the original's 268);
- it saw only takeable actors, never walls, floors, doors or chain objects;
- it competed for the grip.

The patches it needed are the symptom:
- a grip claim order (Lamp, package, weapons, world hands, picker);
- the swallowed late button pulse (GrabGlueResponder);
- items existing twice as actors.

It is removed. The hands already pick the original's way (roth_vrpick: a finger ray through ClassifyPick, DoUse,
DoExamine). This design makes that the only way.

## 1. The owner's decisions (2026-11-23)

1. **VRealms may now diverge from the flat port for VR.** "We are clear to engage VR Realms of the Haunting now, we
   want to make a terrific VR experience for a game which has such an unusual interface." The original's game logic
   still runs underneath: chains, pickups, keys, what a use does. VR changes how you reach, handle and see things.
2. **Reach is measured from the body**, like the original. Your arm does not change what is in range.
3. **Too far stays too far.** No pulling things from across the room. You walk up to it.
4. **A picked-up item stays in the hand until stowed.** The game counts it carried from the moment of the use (the
   original's give), so the inventory is right at all times.
5. **Letting go of a held item anywhere stows it** into the inventory. Nothing is lost on the floor.
6. **Examine three ways:** bring a held item to the eyes, hold a point on a thing, or press a button.
7. **The hit test is the model's shape** once a thing is a 3D model (the original: a sprite's opaque texels).
8. **Later, with a player model:** the trenchcoat. Open the coat to stow objects and weapons in it and on the body.

## 2. The designer's choices (mine; the owner may overrule any)

- **D1:** each hand has its own target and acts on its own. Two hands can use two things.
- **D2:** reach in VR is the HORIZONTAL DISTANCE from the player's position to the hit point.
  - The original measures along the view yaw (depth = 2z + 64). For a ray off to the side of where you face, that
    would give endless sideways reach.
  - Straight ahead the two are equal: 268 / 968 / 2467 units.
- **D3:** a touch beats a point. When the palm or fingertip is on a thing (about 5 cm from the skin), that thing is
  the target. Otherwise the target is what the extended index points at.
- **D4:** the highlight is the original's cursor class drawn on the thing itself, not a laser or cursor sprite:
  - usable: a soft white rim;
  - too far: a dim grey rim;
  - locked or blocked: a dull red rim.
  - The pointing beam stays thin and short and appears only while the index is extended.
- **D5:** the examine timings and button:
  - dwell 0.8 s on a pointed thing;
  - a held item within 20 cm of the eyes for 0.5 s;
  - the button is the hand's upper face button (B on the right, Y on the left), acting on that hand's target.
- **D6:** from Distance Grab only the pickup FLIGHT survives, as the presentation of the original's pickup: the item
  flies the last stretch into the hand that used it. Its selection, reach, flick and catch are gone.
- **D7:** stowing shows the item shrinking into the coat (toward the chest) over 0.2 s. A held item can be taken back
  out from the inventory menu into the hand that opens it.

## 3. The four layers

### L1. The rules (engine-free, the original's)
One interface; nothing else in the game gives, opens or triggers anything:
- `Pick(origin, dir) -> Record`
  - Fields: hit type, face, sector, object, door, point.
  - The first surface or model the ray meets. Sprites: opaque texels only. Models: the mesh triangles (decision 7).
- `Classify(record, body) -> Nothing | Usable | TooFar | Blocked`. These are the original's tests (input.c:1111-1258)
  with reach by D2 from the body (decision 2).
- `Use(record, heldItem) -> result`: `activate_targeted_object`'s port. The held item is the item the original uses
  on the target (roth_handuse.h, DoUse).
- `Examine(record) -> result`: `examine_object_under_cursor`'s port, with the examine reach.

The code exists: roth_runtime ClassifyPick, roth_vrpick DoUse / DoExamine. L1 is that code with its own clean entry,
so every caller below goes through it.

### L2. Each hand's state (one owner per hand per tic)
```
FREE -> TARGETING -> ACTING -> back to FREE or TARGETING
                     \-> HOLDING (an item from a use) -> let go: stowed -> FREE
HOLDING a weapon: the weapon (the Colt's reload, the blades) owns the hand until put away
```
- **FREE:** nothing near, index not extended.
- **TARGETING:** a target by D3, highlighted by D4.
- **ACTING:** a gesture on the target. It ends in exactly one Use or Examine, through L1.
- **HOLDING:** an item or weapon in the hand. Its own handler owns the hand.

This replaces the claim order (Lamp, package, weapons, world hands, picker) and the pulse swallowing.

### L3. Gestures are the verbs
- **Use:**
  - squeeze the grip on a usable target, touched or pointed at in reach;
  - or the target's own gesture from the world hands: pull a lever, twist a key, press a button, push a lid, sit,
    drink.
  - Every one calls Use once.
- **Pickup:** Use on an item. The original gives it, the flight (D6) brings it to the hand, and the hand goes to
  HOLDING (decision 4).
- **Use with a held item:** a held key, match or the like brought onto a target and squeezed. That is Use(record,
  heldItem), the original's use with the selected item.
- **Examine:** the three ways of decision 6 and D5. Every one calls Examine once.
- **Let go of a held item:** it is stowed (decision 5, D7).
- **Weapons:** body slots and the trigger, as now (docs/REALMS_WEAPONS.md).

### L4. What you see
- **Highlight:** D4.
- **Pickup:** the flight into the hand (D6).
- **Stow:** D7.
- **The original's own results stay:** sounds, messages, the examine popup, dialogue, the door swing.

## 4. What is removed or folded

- **Removed:**
  - the RSQ Distance Grab copy (wadsrc/static/filter/vrealms, its CVARINFO entries);
  - roth_grabglue.cpp (RealmsTakeable, GrabGlueTakesGrip, GrabGlueResponder);
  - realmsgrab.zs;
  - the grip claim order.
- **Kept:**
  - roth_vrpick (it becomes L1's callers and L2);
  - the world hands' gestures (routed through L2);
  - the weapons lane;
  - the Colt;
  - one actor per object (ActorsBindLoadedObjects), which is not Distance Grab's.

## 5. Proof (CLAUDE.md: named evidence)

1. **L1 against the original.** With the same eye rays, the original (tools rangeprobe: render_world_view,
   classify_cursor_target_object, activate_targeted_object, examine_object_under_cursor) and L1 must give the same
   record, class and result. The differential count goes in the ledger.
2. **L2 and L3 engine-free.** Scripted hands, as tools/colt_test does, through every state and gesture.
3. **On the PC.** Flat captures through `roth_xrpose` for each verb; the log lines of each Use and Examine.
4. **In the headset.** The owner.

## 6. Phases

- **P1:**
  - L1's entry;
  - L2's state per hand;
  - Distance Grab removed;
  - pickup by squeeze with the flight into the hand;
  - letting go stows.
- **P2:**
  - examine three ways;
  - the D4 highlight.
- **P3:** the model-shape hit test (img3d models and MODELDEF meshes).
- **P4:**
  - use with a held item;
  - taking an item out of the inventory into the hand.
- **Later:** the trenchcoat (with the player model).
