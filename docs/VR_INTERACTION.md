# VR interaction: one system, built on the original's cursor (design, 2026-11-23)

Status: P1 BUILT (2026-11-23, section 8 lists the commits and the evidence; seen in flat on the owner's PC, NOT yet in
the headset). P2-P4 are design. This replaces the RSQ Distance Grab port, which is removed.

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
- **D7 (replaced by 3b, the backpack):** stowing shows the item drawn into the coat over 0.2 s. The inventory panel's hand slots and cells take an
  item out into the hand.

## 3b. The backpack, until the trenchcoat (owner, 2026-11-23)

"For now we will have to put item over our shoulder like a backpack system until I find a player model with an
openable coat." This replaces D7's coat:
- **To stow:** bring a held item over either shoulder (behind the head's plane, above the shoulder) and let go. It goes
  into the backpack: the inventory, which already counts it. Feedback: a soft pulse and the original's inventory
  sound, if it has one.
- **Letting go anywhere else** also stows it. The original has no drop (1.4), so nothing can stay on the floor. The
  item is shown flying back over the shoulder, so the backpack is where things always go.
- **To take out:** reach over a shoulder and squeeze. The original's inventory panel (drawn from the install, CLAUDE.md)
  opens in front of you. Choosing an item puts it in that hand. A weapon goes to the main hand, or to its body slot.
- **Later:** the trenchcoat replaces the shoulder with the coat (decision 8).

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

**Built (P1): src/roth/roth_handstate.h** (engine-free, tools/handstate_test) and roth_vrpick.cpp L2Tick. One owner per
hand per tic. The standing owners first, in this order; each keeps the hand while it lasts:
1. the Colt's reload action (the off hand at the magazine button, the pouch or the slide, a magazine in it, racking):
   ACTING, the Colt;
2. a world gesture held (a pull, a twist, the hour hand): ACTING, the gesture;
3. the off hand on a held weapon's support (the Shotgun's fore-end latch): **SUPPORTING**, the weapon. It gives way the
   moment the off hand starts a reload action (1 is checked first);
4. a held object that owns the grip (the Lamp by its bail, the open Book): HOLDING, held;
5. an item squeezed into the hand: HOLDING, the item.

Then the holdings that keep the hand's trigger and drawing but leave its grip to the claims: a weapon drawn in the main
hand, the lit match, the carried item (6-8). A closing grip asks, in order: the Lamp's bail, the Colt, the weapon (the
fore-end: SUPPORTING; a body slot), a world gesture (a lever's pull, a socket's twist), then the picker's squeeze. Every
owner asks `HandMayAct` before it acts (the weapons' latch and slots, the Colt's off hand, the prints, the hour hand, the
twist, the fingertip press, the fists).

**THE TARGETING RULE (owner 2026-11-23, D3 with numbers).** A usable thing within **5 cm** of the palm or the fingertip is
TOUCHED: it is the target, it gets the rim and the light tick (every tic), and it beats whatever the index points at.
With no touch, the extended index's ray is the target. With neither, a squeeze still searches the in-reach zone it
always did: an object's box within **15 cm** of the palm, a surface **20 cm** along the hand, a wall or door within
**10 cm** of the palm, a flat **10 cm** under it. The zone only chooses WHAT is used; HOW FAR the use reaches is always
L1's, from the body (D2, 268 units). A squeeze pointed at something too far or blocked is refused (the double buzz).

**THE ITEM IN THE HAND (owner 2026-11-23):**
- An item the game puts in a hand (the inventory panel, the auto-select, start items, a chain's give) is drawn in that
  hand with the grip free (the hand on the right side unless a pickup put it elsewhere), and a squeeze on a target uses
  with it. Releasing does NOT stow it, because no grip was closed on it: during the auto-select the player lets go of
  the door between the first squeeze (the key appears) and the second (it opens).
- A squeeze on NOTHING while such an item is in the hand takes hold of it (HOLDING). Releasing that hold stows it.
- So closing the hand on the item and letting go puts it away, whether a squeeze or the game put it there.
- Not while a weapon is drawn in that hand: the weapon's grip wins.

**WHICH HAND A PICKUP LANDS IN (owner, via the lead, 2026-11-23).** A pickup is the inventory changing inside one hand's
own L1 Use: a snapshot is taken immediately before that use's `Activate()` and compared immediately after it returns
(roth_vrpick.cpp DoUse). What that use's own chain gives -- the drawer's Matchbox from the squeeze that opened it --
lands in that hand; that is the VR feel wanted. An item given by anything else (a timer's chain, a dialogue, a combine,
the start items, `roth_fire`) never passes through a hand's use and is the game's: it goes where the game puts it.

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
| The hand closes on the item it carries (a squeeze on nothing) | it stays in the hand, held | | a pulse |
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

## 8. P1 as built (2026-11-23)

### Commits (lane/gamelayer)
- 553a9c05 L1: one rules entry (roth_vrrules.h; RulesPick / RulesClassify / RulesUse / RulesExamine in roth_vrpick.h);
  D2 reach; the dead hand-ray use (roth_pick, UseByHand) removed; roth_l1diff and userange.py l1diff.
- 6c926e59 RSQ Distance Grab removed (the filter/vrealms copy, roth_grabglue, realmsgrab.zs, the RS laser copied for it,
  its test and cfgs). One actor per object (ActorsBindLoadedObjects) kept.
- 3e39115f L2 (roth_handstate.h), the pickup into the hand, letting go stows, the feedback pulses.
- 2e022212 an opaque face ends L1's pick; pickups found by the inventory; roth_xrpose pointat / standat.
- d200e498, f7e9d35d, bfad0e32 tests and cfgs (a chain gives while a hand squeezes; the view at the target; no closing on
  the carried item over a drawn weapon; worldhands_study1 stands the player in reach of plate #124).
- 3b5a9210 the picked object's own picture for the fly-in; this section.
- 0efaf6f4 worldhands_study1: the stand at plate #124 with a bearing (#124 fires again on the PC).
- 5ec84a26, 94233338 the band of a face in the pick (not effective on the two misses below; the rangeprobe logs +0x1a).

### Evidence
- **L1 against the original** (tools/userange_test l1diff: the original's eye rays from the headless ROTH.C rangeprobe,
  through roth_l1diff on the owner's PC, build 2e022212). 1800 eye rays, 120 random STUDY1 poses x 15 cursor points:
  record the same 1497 / 1800 -- walls 1219 / 1378 (102 before the opaque-face fix), floors 168 / 189, ceilings
  82 / 96, objects 28 / 61, the parallax sky and no-record rays 0 / 75 (L1 meets a ceiling or a face there; the class is
  "nothing" either way), platform top 0 / 1. No door leaf (type 6) was among the original's picks in either set: a
  closed door picks as its room face (type 3). Class the same, by the original's own measure: 1482 / 1800; by D2:
  1481 / 1800, the centre column (where D2 is the original's z) 481 / 600. 488 aimed rays at every use / examine
  target: record 346 / 488 (walls 282 / 385, floors 27 / 51, objects 37 / 43); what a use or examine reaches the same
  367 / 488 (use: chain 56 / 86, give 0 / 3, nothing 114 / 155; examine: record 39 / 55, nothing 158 / 189).
  The centre column (where D2 is the original's z), 119 class misses of 600: 117 have a different record (P3's boxes
  against texels: L1 an object where the original a face 31; the sky or no record against a face 19; another face 18;
  ...), and **2 have the same record and a different class -- an L1 bug, NOT resolved**: STUDY1 faces 1361 (pose r97,
  dy -80) and 1527 (r104, dy -170), hit above the sister sector's ceiling. The original's pick record +0x1a there is 1
  (dir_mask 4: the upper band; the rangeprobe now logs it as band1a) and the faces' 0x18 records #13 and #555 admit
  only the mid band (+6 0x01), so the original shows no use; L1 matches the record (too far, 0x398), so the port's band
  at those hits is mid or unknown. Two fixes were tried and neither changed them (5ec84a26: the band by WallPartBit;
  94233338: by the .RAW sister when the engine line has no back sector; 94233338's totals are 2e022212's in every
  category). Next step: log the pick's band and the line's EffectiveBackSector at those two hits on the PC.
  The main remaining cause: L1 picks an object's box where the original saw the wall through the sprite's transparent
  texels (92 of the random set's 159 remaining wall differences) -- decision 7's model and texel shape is P3. The result
  comparison is of the dispatchers' matched record against the calls the original reached (its HOOK lines); its
  disagreements (35 uses where L1 matches a chain and the original reached none, 22 such examines) are NOT yet run down.
- **L2 engine-free:** tools/handstate_test, 2970 checks, 0 failed: every state, every owner, every claim order (32
  combinations), the pickup hold and its end, the carried item, the standing owners, SUPPORTING giving way to the Colt,
  the twist on a held item, the lit match, the weapon hand; and L1's rules (bands, D2, the four classes against the
  original's use gate).
- **On the owner's PC, flat, roth_xrpose** (tools/claude_bridge/cfg/interact_flat.cfg, log-interact_flat.txt): a
  pickup by touch (STUDY1 object 169/0, the Helm, i45: "TOUCHES object ... the rim and the light tick", "PICKUP ...
  HOLDING", let go: "STOWED into the coat ... right hand slot now -1"), by point (41/1, i51, depth 346: "grip while
  pointing: USE object", PICKUP, STOWED), refused too far (face 1386 at the original's wall_270 pose: "refused -- not
  usable: depth 604 >= 600 (planar 270 >= 268) (too far)" and both buzzes), a keyed object (109/2: the original's use,
  no PICKUP), a carried item closed on and stowed (i4), a weapon with the main hand free (the desk Colt 305/0: "PICKUP:
  the original equipped weapon i9 ... the main hand is free"), a chain's give while a hand squeezes (roth_fire 210:
  "right hand: item 43", no PICKUP).
- **Regressions on the PC (build 3e39115f), against the same cfgs' earlier logs:** colt_flat identical (13 shots, 13
  cases, magazine dropped 3 / taken 2 / seated 2, reload pulled 1, slide back 2 / forward 4, dry click 1);
  vrweapons_sword_flat (lands, refused in its cycle, lands again); vrweapons_shotgun_flat, worldhands_mas4 (the lever),
  worldhands_mauso (the key's twist) the same acts; worldhands_study1 the statuette set down on #119 as before. Changed
  on purpose: the RSQ catch is gone; one use per squeeze where the fist's punch used to add a second; plate #124 was
  refused by D2 because the cfg held the off hand 378 units from the player (now the cfg stands him in reach).
- Engine-free suites: gameproof modes 0 1 2 4 7 (H 6156 / 6156, actions 694 / 694), vrpick_test (786432 cursor cases, 0
  differ), worldhands_test 87 / 0, vrweapon_test 70 / 0, gesture_test 63 / 0, held_test 84 / 0, colt_test PASS,
  hands_test 13 / 0.

### Not done or not seen
- **Nothing here is seen in the headset.** The haptics are logged, not felt (no headset in the flat runs).
- The fly-in: the port finds no picture for these items' render ids, so the pickups landed at once ("no picture to
  fly"); 3b5a9210's fallback to the object's own picture did not help either (the PC run of 3b5a9210 still says "no
  picture to fly"): NOT done. A chain's give (0x29) still
  starts no fly-in in the port (roth_runtime.cpp, the [unverified] give context).
- The rim and the stow's coat flight are drawn but not yet looked at in a capture beside anything.
- The use / examine result differences above; the object box vs the original's opaque texels (P3).

### Porting checklist (CLAUDE.md porting rule)
L1 -- roth_vrrules.h carries the numbered list (ROTH.C file:line -> port file:line). In short:
1. the pick record (E/renderer.c:8962-9090, 3283, 4510, 13007) -> PickFrom / PickRecord (roth_runtime.cpp);
2. the first opaque texel (E/renderer.c:3261-3283, 4480-4500) -> the first drawn face, flat or object box (not texels);
3. depth 2z + 64 -> rules::DepthOfZ(rules::ReachZ) (D2);
4. the bands 0x258 / 0x7d0 / 0x1388 (E/input.c:1031, 1123-1217, 1145) -> rules::kUseDepth / kFarDepth / kExamineDepth;
5. classify_cursor_target_object (E/input.c:1111-1245) -> roth_cursor.h ClassifyCursorFacts (unchanged);
6. the click gates (E/game_core.c:604-699) -> CursorClass::CanUse / CanExamine (unchanged);
7. the cursor pictures -> rules::ClassOf;
8. activate_targeted_object (E/input.c:1026-1100) -> roth_runtime.cpp Activate behind RulesUse (unchanged);
9. the item the use carries (E/raw_commands.c:4760-4789) -> roth_handuse.h behind RulesUse (the hand is ItemHandRole);
10. examine_object_under_cursor (E/input.c:979-1024) -> ExaminePick behind RulesExamine (unchanged).
The pickup and the stow, the original's own calls:
11. give_item selects / equips (E/inventory.c:416-434) -> unchanged; the hand that made the use holds it (VR);
12. the fly-in (E/inventory.c:106-139, 0x1c ticks, the fractions at 0x7115c) -> roth_interface.cpp TickFly, aimed at the
    palm (VR); 
13. the stow: the right hand's cell taken onto the cursor (E/dialogue_ui.c:1767-1787) and put back (:1743-1744) ->
    Inventory::PickUpToCursor + DropHeld (roth_vrpick.cpp Stow);
14. a weapon to its body slot: the inventory's weapon click / activate_weapon_item(0, 0) (E/inventory.c:2183-2192,
    E/dialogue_ui.c:1776-1778) -> VRWeaponsToBodySlot (roth_vrweapons.cpp).
