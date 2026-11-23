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

## 1. What the original's internals say, and what each one means in VR

The owner (2026-11-23): design it from the game's internals; his answers are wishes about the experience, not
technical specifications. Each line below is a fact of the original (docs/GAME_inventory.md 1 and 4, GAME_core.md 4),
then the VR design that follows from it.

1. **Two hands already.** The original has a weapon hand (0x81038, `activate_weapon_item`) and an item hand
   (`g_selected_item_primary`, the selected item), each a slot on the inventory panel.
   - **VR:** what a hand holds IS that hand's slot. The weapon in a hand is the equipped weapon. The item in a hand is
     the selected item.
   - Holding two items: the item of the hand that makes the use is selected for that use (roth_handuse.h, DoUse; this
     exists).
2. **The item in hand is used implicitly.** There is no item-versus-object hit test. A use runs the target's chain,
   and the chain's 0x27 asks whether the right item is in hand (raw_commands.c:4785-4788).
   - **VR:** hold the key and use the door. Nothing extra to aim; the key in your hand is what the game checks.
3. **The auto-select.** With the item carried but not in hand, the chain selects it itself, interrupts, and that click
   does nothing; the next succeeds (raw_commands.c:4747-4752).
   - **VR:** the game's own logic, kept 1:1. In presentation: you use the door, the key comes out of your coat into
     your free hand, and the next squeeze opens it.
4. **There is no drop.** No code path returns an item to the world (GAME_inventory.md 4, Drop).
   - **VR:** letting go of an item can only put it away. It goes back into the coat (the inventory). This is the
     owner's wish (decision 5 below), and it is also the only thing the game can do.
5. **A pickup flies to the camera and lands in a hand.** The sprite leaves the world and flies 0x1c ticks toward the
   view (inventory.c:106-139). A weapon is equipped at once (:418-428). Anything else becomes the selected item
   (:433-434).
   - **VR:** the same flight, aimed at the hand that picked it up. It lands in that hand.
   - A weapon goes to the main hand if it is free, otherwise to its body slot.
6. **Use on yourself** (`use_item_on_self`, inventory.c:691-776: the item's OnUse block with target 0; potions, food).
   - **VR:** bring the item to your mouth and squeeze. The world hands' drink gesture already does this for the vial.
7. **Combine two items** (`combine_held_item_with_target`, inventory.c:787-928; the recipe decides the order).
   - **VR:** an item in each hand, touch them together, squeeze.
8. **Examine is a deliberate click,** and it can change the game. The original's examine (a right click) fires the
   target's examine records, which can run chains (raw_commands.c:3102-3200), and plays Adam's comment. It reaches
   2,467 units.
   - **VR:** examine must never happen by accident. It is a button, or bringing a held item to your eyes.
   - Holding a point on something (dwell) would fire examine chains and comments whenever you happen to point. Dwell is
     therefore NOT recommended, although the owner asked for it (decision 6); that is his call.
9. **Examining an item in hand opens the close-up:** the item's DBASE300 video, its name, Adam's comment, the
   information topics and document pages (dialogue_ui.c:1299-1541).
   - **VR:** bring it to your eyes and the original's close-up window opens in front of you, drawn from the install by
     the original's own drawing code (CLAUDE.md: never design what the original already has).
   - The documents-in-both-hands idea (chapter 2, Q22) is the later version of the same thing.
10. **Examining an item in the world** plays only its OnInspect comment, no popup (input.c:995-1012). The VR button on
    a pointed item does the same.
11. **The cursor tells what a click would do.** classify_cursor_target_object (input.c:1111-1258) chooses the cursor
    picture: usable in reach (0x248, 0x250, 0x278), too far (0x398, 0x3a0), nothing (0x240), blocked or keyed.
    - **VR:** the marker where your finger points is the original's own cursor picture for that target, from the
      install, on the surface itself.
    - A thing your hand touches also gets a soft rim, because a cursor picture cannot sit on something inside your
      palm.
12. **Floors, ceilings and wall faces are usable,** not just objects: 0x18 wall-face records, 0x19 floor and
    platform-top records, and painted wall buttons with no object (raw_commands.c:3018-3057).
    - **VR:** pointing and squeezing works on any surface. A low floor spot can also be touched (crouch and press).
13. **Reach is measured from the body, along your facing** (depth = 2z + 64; REMAROTH_MEASURED.md 13).
    - **VR:** from the body (decision 2), as the horizontal distance to the point (D2 below).
14. **Keyed objects never get picked up.** def+9&1 runs the key chain instead (input.c:1053-1069), and an object with a
    use chain is used, not taken. The VR squeeze changes nothing here: the original decides whether a squeeze takes,
    uses or is refused.

## 2. The owner's wishes (2026-11-23), as decisions

1. VRealms may now diverge from the flat port for VR. "We are clear to engage VR Realms of the Haunting now, we want
   to make a terrific VR experience for a game which has such an unusual interface." The game logic still runs
   underneath.
2. Reach is measured from the body.
3. Too far stays too far; you walk up to it.
4. A picked-up item stays in the hand until stowed.
5. Letting go of a held item anywhere stows it.
6. Examine by bringing to the eyes, by a button, and by holding a point. Dwell is not recommended (1.8).
7. The hit test is the model's shape once a thing is a 3D model (the original: a sprite's opaque texels).
8. Later, with a player model: the trenchcoat. Open the coat to stow objects and weapons in it and on the body.

## 3. The designer's choices (the owner may overrule any)

- **D1:** each hand has its own target and acts on its own.
- **D2:** reach in VR is the horizontal distance from the player's position to the hit point.
  - The original measures along the view yaw, which would give endless reach to the side.
  - Straight ahead the two are equal: 268 / 968 / 2467 units.
- **D3:** a touch beats a point. When the palm or fingertip is on a thing (about 5 cm), that thing is the target.
  Otherwise the target is what the extended index points at.
- **D4:** the marker is the original's cursor picture (1.11). Touched things get a rim.
- **D5:** the examine button is the hand's upper face button (B on the right, Y on the left). Bring-to-eyes needs
  20 cm for 0.5 s. Dwell, if kept, takes 0.8 s and is off by default.
- **D6:** from Distance Grab only the flight survives, as the original's pickup fly (1.5).
- **D7:** stowing shows the item drawn into the coat over 0.2 s. The inventory panel's hand slots and cells take an
  item out into the hand.

## 4. The four layers

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
  HOLDING (1.5).
- **Use with a held item:** use the target while holding the item (1.2, 1.3).
- **Use on yourself:** the item to the mouth (1.6). **Combine:** an item in each hand, touched together (1.7).
- **Examine:** the button, or a held item to the eyes (1.8-1.10, D5). Every one calls Examine once.
- **Let go of a held item:** it is stowed (1.4, D7).
- **Weapons:** body slots and the trigger, as now (docs/REALMS_WEAPONS.md).

### L4. What you see
- **Highlight:** D4.
- **Pickup:** the flight into the hand (D6).
- **Stow:** D7.
- **The original's own results stay:** sounds, messages, the examine popup, dialogue, the door swing.

## 4b. Feedback: every state and every result is seen, heard or felt

The owner (2026-11-23): "as long as I have the visual feedback I need to know what is happening". Every event below
has a sign. The original's own sign is used first, where one exists. The haptic pulse is new: the controller
vibrates on the hand concerned.

| What happens | Seen | Heard | Felt |
|---|---|---|---|
| The finger points at something | the original's cursor picture on it: usable, too far or locked (1.11) | | |
| The hand touches something usable | a soft rim on it | | a light tick |
| A squeeze uses it | the original's result: the door swings, the chain's effect | the original's sound | a firm pulse |
| A squeeze is refused (too far, locked, wrong item) | the cursor picture stays too far or locked; the original's message, if it prints one, on the wrist mount | the original's refusal sound, if any | a double buzz |
| An item is picked up | it flies into the hand and stays there | the original's pickup sound | a pulse on landing |
| The game fetches a carried item (auto-select, 1.3) | the item comes out of the coat into the free hand | | a pulse |
| An item is put away | it is drawn into the coat | | a soft pulse |
| Examine | Adam's comment as a subtitle on the wrist mount, or the original's close-up window | Adam's voice | |
| Use on yourself, combine | the original's timed message ("used X", "can't combine X and Y") on the wrist mount | | a pulse |

Where the original shows nothing, the hand still feels the squeeze land or be refused.

## 5. What is removed or folded

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

## 6. Proof (CLAUDE.md: named evidence)

1. **L1 against the original.** With the same eye rays, the original (tools rangeprobe: render_world_view,
   classify_cursor_target_object, activate_targeted_object, examine_object_under_cursor) and L1 must give the same
   record, class and result. The differential count goes in the ledger.
2. **L2 and L3 engine-free.** Scripted hands, as tools/colt_test does, through every state and gesture.
3. **On the PC.** Flat captures through `roth_xrpose` for each verb; the log lines of each Use and Examine.
4. **In the headset.** The owner.

## 7. Phases

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
  - use with a held item and the auto-select's presentation;
  - use on yourself;
  - combine;
  - the close-up at the eyes;
  - taking an item out of the inventory into the hand.
- **Later:** the trenchcoat (with the player model).
