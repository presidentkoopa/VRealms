# VRealms item design dossiers (2026-11-13)

The owner authorised VR design of items and interactions on 2026-11-13: "innovative VR ideas for a 30 year old game
never meant to be in true 3d"; "take our time and get it right". His brief for this file: think about each item,
"like the lantern ... ask where adam would hold this and the kind of pose it would need".

Nothing here is built. This file is a design to choose from, with its facts checked.

**Marks used throughout**
- **[F: how]**: a fact, with the check that shows it.
- **[R]**: reasoning only, not seen or measured.
- **[P]**: a proposal: a design decision for the owner to accept or change.

Sources:
- the player's own install, read through `src/roth` by `tools/item_dossier` and `tools/dbase100_test`;
- the close-up films, decoded with `src/roth/roth_gdv` (`tools/gdv_test/gdvtool`, frames to /tmp only);
- ROTH.C, the original's reconstruction (`/home/claude/oracle_build/rothc/roth_c/src/engine`, cited as `C/<file>:<line>`).

Nothing from the retail art or text is in the repository beyond names and short quotes.

---

## 1. The rule: VR changes how a verb is performed, never what it does

Every interaction below ends in one of the original's verbs (docs/GAME_inventory.md 4-5, docs/GAME_core.md 4-5).
VR changes only how the player performs it.

| original verb | the original's call | what VR may change |
|---|---|---|
| pick up | `activate_targeted_object` → `give_item_by_dbase_id` (C/input.c:1026-1101) | the gesture (phase 1: grip squeeze in reach) |
| use the item in hand on the world | the item is the selected right-hand item (`g_selected_item_primary`); clicking a target runs the target's own RAW chain, which tests the item with RAW 0x27 (C/raw_commands.c:4760-4789) | how the target is clicked: the item's own tip touches it, or the hand points and taps |
| use on self | `use_item_on_self` (C/inventory.c:691-776): an OnUse block with target 0 | the gesture: bring the item to the body |
| combine | `combine_held_item_with_target` (C/inventory.c:787-928) | the gesture: bring two items together |
| examine an item | the close-up popup: GDV film, first-view OnInspect comment, magnifier, "i" topics, document pages (C/file_config.c:900-1144, C/dialogue_ui.c:1299-1541) | how it is opened: bring the item to the face. The popup and film stay the original's |
| examine the world | `examine_object_under_cursor` (C/input.c:979-1015) | phase 1: point and dwell 0.8 s |

Use and examine ranges are the original's (docs/REMAROTH_MEASURED.md 13). They are planar distances along the view yaw:
- use: z < 268 world units;
- the "too far" band: z < 968;
- examine: z < 2467.

At the VR scale of 107.4 units per metre (HANDOFF_LANE_GAMELAYER_2026-11-13_HANDS.md; not yet judged in the
headset), use reaches 2.5 m and examine reaches 23 m.

**A touch is always inside the use range** [R]: a hand-held item reaches about 0.8 m. So "touch the item to the
target" never fires anything the original would refuse. It is stricter than the original, which uses from 2.5 m.

**[P]** Keep the phase-1 pointer path (15c9da27: point and tap = use) for every item as well. The original's reach
then stays available, and touch is the VR way, not the only way.

Consequences of the original's logic that VR must keep:
- **There is no drop.** ROTH has no code path that returns an item to the world (docs/GAME_inventory.md 4,
  "Drop: none"). So an item released in VR goes back to the pack; it never stays where it fell.
  - The only "put down" is a script that removes the item (RAW 0x2a, e.g. a statuette set on its plinth). In VR
    that is a use on the plinth, and the release is just how it looks.
- **Two hand slots.**
  - The original's left hand is the equipped weapon (`g_selected_item_secondary`): Adam's own fists until a
    weapon is equipped.
  - The right hand is the selected item (`g_selected_item_primary`), C/inventory.c:416-434.
  - **[P]** These map onto Adam's two VR hands: the weapon slot to the hand that holds `RealmsHeld`
    (docs/REALMS_WEAPONS.md), the item slot to the other. Which physical hand is which is the player's choice.
- **Select mode.** Most item tests are select mode: the item must be in a hand slot. If it is carried but not in a
  hand, the original auto-selects it, interrupts the chain, and the click does nothing (C/raw_commands.c:4745-4752).
  - In VR the item is in the hand because the player is holding it, so a touch with it never meets that path.
    A pointer click with an empty hand meets it exactly as the original does.
  - Tests marked `in-hand,no-autoselect` (RAW 0x27 flags 0x22/0x23) need the item already in a slot.
- **Category 2** (characters and important info; Adam, Rebecca, the Breastplate, ...) is never in a hand
  (C/inventory.c:407-409). It has no VR object to hold.

## 2. The template

Each item section has:
- **Data**: id, name, category; how it is obtained; where the scripts use it (RAW 0x27 tests with mode and the chain
  that follows, removes, gives, DBASE100 gates); the original's behaviour (OnInspect, OnUse/combine, document pages,
  topics, weapon). This is what `tools/item_dossier` prints.
- **Object**: what the close-up film shows (looked at, 5 frames per film); size and weight [R estimates]; materials.
- **VR** [P unless marked]:
  - the grip point and the hand pose; one or two hands; how it is carried;
  - each original verb performed in VR;
  - natural extras that change no game state;
  - light and emission, sounds;
  - what the voxel needs;
  - open questions.

## 3. The hands: the poses that exist

The hands are the owner's `hand_final.fbx`, read by the engine (docs/FBX_LOADER.md). Poses are authored frames of
its take; no pose is made by bending joints. The owner's pose table for this FBX is
`/home/claude/uzdxrema/tools/avatar/fbx_poses.json` [F: read]. Frame numbers below are Blender frames, as the table
gives them (take index = Blender − 1).

**Raw frames**

| name | frame | hand | what it is |
|---|---|---|---|
| open | 1 | both | |
| relaxed | 154 | both | |
| halfcurl | 193 | both | |
| curl | 206 | both | fingers hooked, palm flat |
| crossed | 304 | both | |
| fist | 319 | both | |
| twofinger_point | 762 | A | |
| fingergun | 803 | A | |
| wrap_half | 820 | B | round a thick handle, loose |
| wrap_tight | 837 | B | round a handle, closed |
| thumb_out | 944 | B | |
| cup | 985 | B | index and middle folded at the middle joint, thumb in |
| twohand_gun / twohand_support | 985 | A / B | |

**Composed** (each finger taken from one raw frame):
- `gun_grip`, `grip_pistol` and the other `grip_*` weapon grips;
- `melee_grip` (fist);
- `pointing` (fingergun index, rest fist);
- `hold_magazine` (wrap_half);
- `hold_round` and `hold_shell`: the cup. The table itself says "NO PINCH IN THE FBX".
- `hold_slide` and `hold_forend` (wrap_tight);
- `hold_foregrip` (fist);
- `hold_support` (twohand_support);
- `hold_inserting` (relaxed).

**Wired in VRealms today** [F: HANDOFF_..._HANDS.md, CURRENT STATE]: open, fist, point, thumb up, finger gun and
trigger, picked by the capacitive touches. The images are `docs/images/hands/poses_closeup.png` and `walk_Hand_Low.png`.
**Item grips wired (2026-11-13)** [F: tools/hands_test test_holds, tools/fbxload_test H0-H2; not yet seen in the
engine]: UZDXREMA's selection (vr_avatar_pose.cpp:466-656) and the whole table, each hand from its own take. The
held item names its grip in `src/roth/roth_handhold.h`, from the dossiers below; the hand takes that pose while the
item is held (the original's right-hand item in the VR off hand, the weapon at the main hand: section 1's [P]).
- **The right take has no wrap_half, wrap_tight or cup** (it holds its two-finger gun from frame ~760 on while the
  left wraps): on the right hand every wrap or cup pose is absent and the controller's pose stays. The left take has
  no fingergun frame. Measured: `tools/fbxload_test/walk_poses.py` (check_hold_table). New frames for the right hand
  are the owner's to author.
- The pinch items use the cup (hold_round), UZDXREMA's own fallback; they still need the pinch frames below.

**What is missing** [R, from the 40 items below]: a **pinch**. Of the first 40 items, 17 are small things a person
holds between thumb and finger: a match, keys, gems, a feather, earplugs, rings, the wand's tip.
- The cup (985) is a palm hold, not a pinch.
- The engine's "trigger" state (walk sheet: index from the fist, the others open) brings the index down to the thumb's
  base. It may serve as a **lateral (key) pinch**, the way one holds a key. Not checked: no render of a key in it exists.
- **[P]** Ask the owner for two authored frames in the take:
  - a **lateral pinch**: thumb pad on the side of the curled index; for keys, coins, a match, a ring held to place;
  - a **tip pinch**: thumb tip to index tip; for a feather, an earplug, a match taken from the box.

  Until they exist, the fallback is the cup, with the item placed between thumb and index.

## 4. What this engine can do for an item's light, and its shadows

Read in `src/` and `wadsrc/static/shaders/glsl/` [F: read, cited]. Nothing here was run for this file.

**Dynamic lights**
- Point lights, attenuated, additive or subtractive; spot lights (`LF_SPOT`).
- Pulse and flicker types: `a_dynlight.h:37-47` (`PulseLight`, `FlickerLight`, `RandomFlickerLight`, ...).
- The flags at `a_dynlight.h:59-76`. A spot is a cone: `spotLightAttenuation`, main.fp:1114-1119.
- Effect lights (sparks and embers that belong to no actor): main.fp:1120-1135.

**Shadows, two kinds**
1. **1D shadow maps** (`hw_shadowmap.cpp:26-51`). One texture row per light. A ray per texel against an AABB tree of
   **level line segments**. Only walls (one-sided lines) cast; it is 2D, so floors and objects do not.
2. **Ray-traced** (`gl_light_castshadows 2`, `hw_shadowmap.cpp:61-79`; `traceHit`, main.fp:891-897; Vulkan only,
   from the next start).
   - The acceleration structure is `DoomLevelMesh`, built from sides and subsector floors and ceilings (3D floors
     included) **as they are at map start** (`doom_levelmesh.cpp:22-29`).
   - **Models, sprites and voxels are not in it.**

**What is not there**
- **No shadow from a model, sprite or voxel.** No path anywhere puts a model in the shadow test.
- **No projected texture, cookie or gobo.** The only thing that shapes a light is the spot cone.

**Where it may not reach** [R, not checked]: whether a dynamic light adds anything on surfaces drawn with the
original's palette shading (ROTH_PALETTE). Palette shading maps the result through a colormap row
(`gRothRow`, main.fp:4814-4816).

### The lantern's bar shadows: what is possible

- **Today: none.** The bars are part of the lamp's model, and models cast nothing.
- **Real fix A: an analytic gobo for this one light** [P, renderer work, small].
  - A light flag plus the lamp's axis. The two spot slots of the light record (`lights[i+2]`, `lights[i+3]`) carry
    the axis and the bars' angular positions and width.
  - The fragment shader takes the direction from the light, works out its azimuth about the lamp's axis, and darkens
    the wedges the bars cover. It also darkens the cones above and below that the fount and the cap cover.
  - This is exact for straight bars around a point flame, because the bars are fixed to the lamp.
  - Cost: a few ALU ops per light per pixel. It also gives the flame's own flicker a structure the room can see.
- **Real fix B: shadow-casting models** [P, renderer work, large].
  - Ray-traced: a BLAS for each held model, rebuilt per frame, as an instance in the TLAS beside the level mesh.
  - Or a cube shadow map rendered from the flame for each lamp light.
  - B is the general answer (any held object would shadow); A is the cheap exact one for the lamp.
  - This belongs to the render lane's step-4 renderer work (UZDXREMA ROADMAP), not to this lane.
- **Fallback, today, no renderer work** [P]:
  - The flame is a small `RandomFlickerLight` inside the globe, short radius.
  - The globe is drawn emissive, the lamp's own geometry darkens itself, and there are no shadows in the room.
  - The bars show as dark silhouettes against the bright globe. The eye reads that as "the bars are in the light's
    way" without a projected shadow.

## 5. The finding: the Lamp is not the original's lantern light

**Fact** [F: read the code and the data; nothing run]: the original's "candle" (lantern) light is a **lighting mode
of sectors**. It is not tied to the Lamp item in any way: not held, not selected, not owned.

1. **The candle mode is chosen per sector.** Sector byte +0x0a bit 1 picks it: the alternate ramp, the tint table and
   the radial falloff from the **screen centre** (C/renderer.c:9193-9198). Its parameters come from map metadata
   +0x10/+0x14/+0x16 (`init_map_lighting_from_metadata`, C/renderer.c:10300-10314).
2. **Standing in a candle sector** sets the view bias −13 (brighter); `g_player_sector_cache+2` is 0 inside and
   0xfff3 outside (C/collision_physics.c:1773-1778).
3. **The cone's jitter** (the flicker) is an LCG stepped every 7 ticks. It moves two light-centre offsets around the
   view centre (`tick_ambient_render_animation`, C/renderer.c:13411-13428). The cone's span is set from the view size
   (C/renderer.c:12097-12119).
4. **The bit is written only by the map's light records**:
   - at load, the type-0x02 records' lights-out pass (docs/REMAROTH_MEASURED.md 4);
   - `tick_cmd_45` with `apply_flag_mask_to_record_list` (C/raw_commands.c:1875-1893, 5688);
   - `cmd_light_switch` (C/raw_commands.c:4331).

   **No inventory function reads or writes any of these.**
5. **No engine code tests the Lamp by id.** The only hard-coded `query_player_inventory` callers are the weapon ammo
   count (C/weapon_combat.c:450, 1406) and the RAW 0x27 handler (C/raw_commands.c:4776-4786).
6. **The Lamp record** (item 15) has no trigger blocks at all [F: `dbase100_dump --item 15`, `item_dossier`]:
   - no OnInspect, no OnUse, no weapon, no topics;
   - render id 0 (it is never in the world);
   - **no map script tests, gives or removes it** [F: `item_dossier`, every RAW 0x27/0x29/0x2a and DBASE op
     0x02/0x03/0x11 in 44 maps and 694 actions].

   It is given once, at new game (action 3, with the Matchbox, item 1 and Adam), and never taken away.
7. **What the Lamp does have is its close-up film**: 20 frames, 102×140 [F: looked at frames 0, 5, 10, 15, 19].
   It shows a hurricane lantern **lit, with a yellow flame in the globe**.

So the original gives Adam a lit lamp he can never lose, and lights every dark ("candle") sector with a flickering
cone from the eye. Read together, the cone is Adam's lamp [R]. But no line of the original says so.

**[P] The design that follows from this, without changing the original's lighting:**
- **Lamp emission.**
  - The lamp in Adam's hand or on his belt is drawn lit (emissive globe, a small flame light; section 4's fallback).
  - In candle sectors the room is lit by the original's own cone, exactly as now (main.vp/main.fp, 2,943/2,943
    vertices exact, docs/REMAROTH_MEASURED.md 4). The lamp's own light then adds only a short-radius glow on the
    hand, the lamp and what is within about 0.5 m.
  - Outside candle sectors the original shows no lantern light. The lamp is then drawn lit but adds **no light to
    the room** (1:1: where the original shows nothing, we show nothing).
- **Phase 2, for the owner to decide**: move the cone's centre from the view centre to the lamp.
  - Project the lamp's position into each eye and use it as the cone's (jx, jy).
  - That is a change of presentation, not of the original's shading formula: the same rows, a different centre.
  - It makes the light visibly come from the lamp, and makes holding the lamp up or down matter to what the player
    sees.
  - **It breaks 1:1 with the original's frame** whenever the lamp is not at the view centre. So it waits for the
    owner's word.
- **Phase 3**: the bar shadows (section 4, fix A or B).

---

# The dossiers

Priority order (from `tools/item_dossier --order`):
1. the starting items (action 3), Adam's own record last;
2. every item a script tests in hand (RAW 0x27), by number of tests;
3. every item placed in a map;
4. the rest.

Items 1-40 of that order follow. The data half of every item is `tools/item_dossier "<DATA>" --item N`.

Render ids below 0x200 never appear in the world as items (C/inventory.c:47). Placement counts are objects whose art
resolves to the record (ascending-first search, as `give_item_by_dbase_id`). Monsters share this mechanism and are
not items one holds.

---

## 15 Lamp (start)

**Data** [F: item_dossier]
- Category 0 (general). Flags 0x00. Render id 0.
- Close-up: 20 frames at 12 fps, 102×140.
- Obtained: starting item (action 3).
- Scripts: none, anywhere.
- No trigger blocks: no comment on examine, no use, no topics. Section 5 has the full finding.

**Object** (film, frames 0/5/10/15/19)
- A cold-blast hurricane lantern: a domed cap with a chimney, a glass globe, a lit flame inside.
- Two vertical side tubes or guard bars from the fount to the cap, and a wire bail (handle) over the top.
- A round fount (oil tank) at the bottom with a burner knob (the white disc on the front), painted dark red-brown.
- Size [R]: 30 cm tall without the bail, 18 cm wide across the tubes, globe 11 cm. About 0.9 kg with oil.
- Materials: tinned or painted sheet steel, a glass globe, a steel wire bail.

**VR** [P]
- **Grip**: the bail, hand above the lamp, the lamp hanging. That is how a hurricane lantern is carried; it keeps the
  globe upright and the light below the eye.
  - Pose: **curl (206)**, fingers hooked over the wire, thumb relaxed alongside [R: the curl frame's hooked fingers
    fit a 3 mm wire; check with a render].
  - To hold it up high, the same grip lifted.
- **Second grip**: the fount's rim from below, cup (985), palm up. For holding it close to read by, or to look at it.
- **One hand.**
- **Carry**: a hook on Adam's belt or coat, at the left hip.
  - Released near the hip, it hangs there and swings with the body; it is never "dropped".
  - When the hand that holds it is needed (a two-handed item, a weapon), it goes to the hook.
  - It stays lit on the hook (section 5): the original's light never goes out.
- **The original's verbs**:
  - examine: bring it to the face, then the original's popup with its film. There is no comment (the record has none).
  - There is no use or combine to perform.
- **Light**: section 5. The globe is emissive; a `RandomFlickerLight`, warm (about 2000 K), radius 40-60 units
  (0.4-0.55 m), only on the lamp, the hand and the near surroundings.
- **Sounds** [P]: a faint creak of the bail when it swings fast; nothing else (the original has none).
- **Voxel**:
  - The globe must be a separate translucent part: KVX voxels are opaque. Make it hollow so the flame is inside it.
  - The bars and the bail are thin (3-5 mm): at 2 mm voxels they are 1-2 voxels. Model them as thin meshes or use a
    1 mm grid.
  - The flame is a separate emissive billboard or particle, not voxels.
  - The burner knob turns: a separate part. Nothing uses it; its turning is pure feel.
- **Open**:
  - section 5's phase 2 (the cone from the lamp) and phase 3 (bar shadows) are the owner's to decide;
  - may the knob turn the flame down? The original has no such state, so no light may change [R: so no].

## 16 Matchbox (start): the deep case

**Data** [F: item_dossier, map command records read with a probe of `src/roth` (raw records, objects by command id,
art read from the level pack)]
- Category 0. Flags 0x00. Render id 0x298.
- Close-up: 10 frames, 160×67, seen from above.
- **No trigger blocks**: no comment, no OnUse, no combine.
- Obtained: starting item (action 3). One more lies in STUDY1 (placed ×1). It is not stackable, so taking it gives a
  second "Matchbox" slot [R from C/inventory.c:398-414].
- **Never consumed.** No RAW 0x2a, no DBASE100 op 0x91, no OnUse result removes it anywhere [F]. The original has no
  match count.
- **Six scripts test it in hand**: RAW 0x27, select mode, f6=0x03, so carried-but-not-in-hand auto-selects it.
  Every one is the **object-click (0x08) trigger of a candle holder**. The chain does this (C/raw_commands.c cited):
  1. `0x27` (16): the matchbox must be in a hand.
  2. `0x3a` change object id to 0xFFFF: the clicked object gets a fresh unique id, so it no longer answers this trigger
     and cannot be lit twice (C/raw_commands.c:479-497).
  3. `0x12` delay 30 (20 in one) ticks: about 0.43 s at the 70 Hz clock [R: the unit is the delay effect's tick].
  4. `0x3a` change object id to 0, 0x0F or 2: the lit candle joins another trigger group, or none.
  5. `0x0d` change object texture, immediate, on the clicked object (C/raw_commands.c:4520-4551): the unlit art
     becomes the lit art.
  6. `0x1d` change lighting: a brightness ramp (+4 to +10) on the room's sector group (C/raw_commands.c:4390-4418).

  The art was read from the level pack [F: pictures to /tmp]:
  - five-candle candelabrum 0x002 → 0x034, flames;
  - tall single candlestick 0x05a → 0x05b;
  - short candlestick 0x01c → 0x05e.

  Where they are:
  - **STUDY1**: two tall candlesticks (trigger key 3), four candelabra (key 2) and two candelabra (key 16).
    - After the two tall ones a count of 2 runs. When it completes [R: cmd_count's re-pump shape,
      C/raw_commands.c:216-235], it moves sector group 11's height, flickers light group 46 (+40, wait, +30, wait,
      −30) and spawns a small glow object (art 0x012) high in the room.
  - **STUDY4**:
    - five short candles on the floor (key 1). After them a count of 5 runs, then:
      - action 452: cutscene index 52, "Mark of the Beast";
      - action 565: music;
      - a light change and a turn of the player;
      - sound nodes, a 120-tick wait;
      - **a monster spawned** (item 177, AsMonster, health 4000).

      This is the summoning: Adam lights the circle's candles and the Beast comes.
    - one candelabrum (key 16) and two candelabra (key 17).
- **What the original does with it, in one line**: with the matchbox in hand, click an unlit candle holder. It goes
  dark-art to lit-art about half a second later, and the room brightens. Nothing is used up.
- **The Lamp is not involved.** No chain tests item 15 and none touches the candle-sector bit, and the lamp is lit in
  its own film. There is nothing to light the lamp with in the original. **[P] No lamp-lighting ritual**: it would be
  invented state.

**Object** (film, 5 of 10 frames)
- A worn cardboard slide (drawer) matchbox, printed label with red, green and yellow, seen from above.
- Size [R]: 5.5 × 3.6 × 1.6 cm, about 25 g. Cardboard sleeve, cardboard drawer, brown striking strips on the two long
  narrow sides.

**VR** [P]. Each part is marked **(logic)**, meaning it fires the original's verb, or **(feel)**, meaning no game state.

1. **(feel) Box in the off hand.** wrap_half (820) round the sleeve, thumb on the drawer's end.
2. **(feel) Opening.** The drawer slides out when the off hand's thumb touch is held and the other hand comes within
   10 cm. It shows a few matches; it never runs out (the original has no count).
3. **(feel) Taking a match.** The main hand reaches into the drawer.
   - Pose: the **tip pinch** (section 3, to be authored).
   - Until it exists, the "trigger" state (index from the fist to the thumb's base) with the match laid along the
     thumb [R: to be checked on a render]. The cup does not hold a 2 mm stick.
   - **A new authored frame is needed for this to look right.**
4. **(feel) Striking.** Draw the match head along a striker strip: head within 1 cm of the strip's face, moving
   along it faster than about 0.4 m/s relative to the box, over at least 3 cm. It lights on the first or second stroke.
   - Haptic tick on contact, a buzz while dragging, a pop on lighting. Sound: a rasp and the flare.
   - Both hands' poses come from the controllers, at render rate. This needs no playsim data.
5. **(feel) The flame.**
   - A tiny `RandomFlickerLight`: radius 30 units (0.28 m), warm, short. It lights the fingers and the box; it is
     too small to change the room.
   - It burns about 20 s. Near the end it reaches the fingers: a haptic warning, then it goes out.
   - A flick of the wrist above about 6 rad/s puts it out. The spent match falls and fades: a particle, not an item
     (there is no drop, and a match is not an item).
6. **(logic) Lighting a candle.** The lit match's flame point enters a small sphere (about 4 cm) around a **wick** of
   an unlit candle holder.
   - Wicks: the candle tips of the sprite (five for 0x002, one for 0x05a/0x01c), read once from the art.
   - That counts as the click on that object with the Matchbox as the selected item: the port's
     `activate_targeted_object` path, exactly as a mouse click. The original's chain then does all of the above:
     0.43 s later the art turns lit and the room brightens.
   - In VR the 0.43 s delay reads as the wick catching: hold the flame there.
7. **(feel) Extra flames.**
   - When the chain swaps in the lit art, the VR build may add a small flame light at each wick.
   - That is light the original does not have: the original raises the sector instead. So **this one needs the
     owner's OK** (1:1 rule). Without it, the original's sector ramp is the whole effect.
8. **(logic) The pointer path stays.** Pointing at a candle and tapping, with the matchbox in a hand slot, lights it
   as the original does, from up to 2.5 m. The match ceremony is the VR way, not the only way.
   - **Open**: should flame contact be the only way in VR? The owner's call.
9. **What the game state sees**: only the original's input — item 16 in the right-hand slot, then a click on the
   candle object. Steps 1-5 and 7 change nothing in the game state.

- **Examine**: bring the box to the face, then the original's popup with the 10-frame film. No comment (none in the
  record).
- **Voxel**:
  - Sleeve and drawer are separate parts (the drawer slides) and the drawer is hollow.
  - The striking strips are a texture.
  - Matches are separate tiny meshes (2 mm stick, 3 mm head): too thin for a voxel grid coarser than 1 mm.

## 1 Shards and Seals (start)

**Data** [F]
- Category 0. Flags 0x09 (close-up hold mode, information topics). Render id 0x285, never placed.
- Close-up: 21 frames at 9 fps, 200×140, played once and held.
- Obtained: starting item (action 3).
- 11 information topics: "Discuss" ×4, "Talk generally about Seals", "More about Seals", "Fallen Angels", "Demons",
  "Reflection" ×2, "Shards and Seals". They are gated by story flags; in them Rebecca explains the fragments.
- No use, no combine, no scripts.

**Object** (film)
- Bronze or clay seal discs carved with runes, broken in pieces. The film runs from a heap of shards to six whole discs.
- Size [R]: each disc 5-6 cm across and 5 mm thick, about 30 g; the set about 200 g.

**VR** [P]
- **Grip**: a small cloth pouch tied at the neck (the original shows only the shards, so the pouch is the carrier),
  held in a fist (319) by the neck.
- **Examine**: tip the pouch into the open palm: open (1), then cup (985). The original's popup opens beside it and
  its film plays: the shards assembling is the film. The "i" topics are the original's list.
- **Carry**: a coat pocket.
- **Voxel**: the shards are flat; voxels at 2 mm are enough. The pouch is a soft mesh.
- **Open**: show the shards in the palm (six or seven small voxels), or only the pouch? [R: the film already shows
  them; in-palm shards would be a second presentation of the same thing.]

## 68 Adam Randall (start, his own record)

**Data** [F]
- Category 2, never in a hand. Flags 0x99 (weapon, information, not pickable, hold).
- Adam's record: +5 bit 0x10, latched at new game.
- Two WeaponAction blocks: **bare hands**, the fists, the weapon slot's default.
- An AsMonster block (his doppelgänger; his art is placed 16× in DOPPLE).
- Topics: "Adam's Brands", "Discuss", "Reflection" ×2.
- Close-up: 37 frames, 200×140, **live-action video of the actor**, not a 3D render.

**VR** [P]
- Adam is the player's body. His WeaponAction is the fists: `melee_grip` (fist 319) on both hands when the weapon slot
  is bare, through `RealmsHeld` (docs/REALMS_WEAPONS.md).
- **The portrait slot**: looking at one's own open palm (the brands, his topics) is the natural "examine Adam".
  - Turn the hand palm-up in front of the face for 0.8 s → the original's Adam popup, with its film and topics.
  - This is the one place where the brands on Adam's hands could be drawn later (story: "the same design that's branded
    on my palm", item 14). A cosmetic texture; it waits for the owner.
- The film shows a dark jacket: the jacket's pockets are the natural carry spots for small items [R from the film].

## 150 Wand (tested 18×)

**Data** [F]
- Category 1, **weapon** (WeaponAction). Render id 0: never in the world.
- Close-up: 10 frames, 200×47.
- Obtained: RAW 0x29 give in CHURCH1, STUDY1, VICAR1.
- Tested 18×, CHURCH1/VICAR/VICAR1, always `in-hand,no-autoselect` and often together with the Magical Torch (175).
  - The trigger is **0x1a, wall contact**: the player's own collision with a wall fires it
    (`fire_wall_object_trigger`, C/collision_physics.c:566, C/raw_commands.c:1762).
  - The chain is particles, a spawned object, toggles, a wall texture, a flag, once an open door.
  - That is **walking into a Ward with the wand in hand dispels it**. OnInspect agrees: "So this wand dispels Wards."

**Object** (film): a thin, slightly crooked twig with red knots. Size [R]: 35 cm long, 8-10 mm thick, 40 g, wood.

**VR** [P]
- **Grip**: the thick end, like a conductor's baton: lateral pinch plus a loose wrap (wrap_half 820 on middle to
  pinky). Until the pinch exists, wrap_half.
- **One hand**, in the weapon slot (it is a weapon).
- **Carry**: a long inside pocket of the coat, or the belt.
- **Verbs**:
  - **(logic)** Ward: 1:1, walk into the warded wall with the wand held.
  - **[P, owner to decide]** Let the wand's tip touching the ward wall fire that same wall's 0x1a contact. It looks
    right in VR, but it fires up to an arm's length earlier than the original, and it is a new way to reach that
    trigger.
  - Its weapon action: the weapon lane (`RealmsWeapon150`).
- **Light**: none in the original. The film shows no glow.
- **Voxel**: 1-voxel thin at 2 mm, so a 1 mm grid or a mesh. The knots are colour.

## 175 Magical Torch (tested 12×)

**Data** [F]
- Category 3 (artifacts). Not a weapon. Render id 0x01f, below 0x200: the 141 "placements" are torches drawn as
  scenery, not this item.
- Close-up: 10 frames, 200×58.
- Obtained: RAW 0x29 give in CHURCH1, VICAR1.
- Tested 12×, `in-hand,no-autoselect`, the same 0x1a ward chains as the Wand. Either item in hand dispels the Ward.
- OnInspect: a security guard's torch, "Handy for spotting Wards".

**Object** (film): a heavy black metal flashlight with red runes along the barrel and a polished reflector. Size [R]:
32 cm, head 6 cm, 0.7 kg, aluminium, glass lens.

**VR** [P]
- **Grip**: the barrel, wrap_tight (837), thumb on top where a switch would be. One hand.
- **Carry**: the belt loop, head down.
- **Verbs**: the ward as for the Wand (1:1 walk-into; the touch variant is the owner's decision).
- **Light**:
  - **The original emits nothing from it.** No code makes light from any item. A beam would be invented light
    (the 1:1 rule).
  - **[P]** Draw the lens faintly emissive and nothing more until the owner decides.
  - If he wants a beam: an `LF_SPOT` with a 12° inner and 25° outer cone, radius about 600 units (5.6 m), cast-shadow
    asked. Walls shadow it with shadow maps; level geometry does with ray tracing (section 4).
  - The runes glowing near a Ward would be a nice tell, but it is invented.
- **Voxel**: a solid cylinder; the reflector is a separate glossy part.

## 4 Statuette (serpent) (tested 8×)

**Data** [F]
- Category 3. Flags 0x48. Render id 0x22d.
- Close-up: 20 frames, 133×140.
- Placed: AELF, CAVERNS3, SALVAT ×11, STUDY1 ×11, STUDY3. RAW 0x29 gives ×14.
- Tested 8× (STUDY1 ×7, STUDY3), in hand, on a **floor use (0x19)** trigger:
  if-dialogue, then 0x27 (4), **remove the statuette**, spawn an object (the statuette standing there), a count,
  dialogue, a height change.
- So **setting statuettes on their places** opens something once all are set.
- OnInspect: gold leaf; "It looks really heavy but there's no weight there at all". Topics "Discuss"/"Reflect" by count.

**Object** (film): a cobra rearing from a coiled body on a round coiled base, red eyes, gold leaf. Size [R]: 18 cm
tall, base 10 cm. **Weightless by the story** (Adam's line).

**VR** [P]
- **Grip**: round the coils, wrap_half (820). Or the base on the palm (cup 985) to carry it level.
- **One hand.**
- **Carry**: the pack (the inventory). It is too big for a pocket.
- **Verbs**: **(logic)** set its base onto the floor spot and open the hand. That is the floor use of that spot with
  the statuette in hand. The original's chain removes the item and spawns an object at that spot [R: the standing
  statuette; the spawned art was not read].
- **Feel**: no weight. The hand does not sag, and there is no heavy haptic: the story says so.
- **Voxel**: solid; the eyes emissive red (the film's glint). 2 mm voxels.

## 113 Earplugs (worn) (tested 5×)

**Data** [F]
- Category 0. OnUse: on self → item 108 Earplugs (take them out). Render id 0.
- Close-up: 20 frames, 200×92.
- Obtained: only as the result of item 108's use on self (put them in).
- Tested 5× in RAQUIA5 (carried, count mode):
  - an examine-object dialogue;
  - an object click that gives item 20 (a Key);
  - on a sector edge (0x13), **damage and slow-down, gated by the earplugs** [R: the polarity f6=0x00 was not
    traced; "not worn" is the reading that fits "Should prevent us hearing"].

**Object** (film): two ivory conical plugs and a lidded case marked with the alchemical sign of mercury on red. Size [R]:
each plug 1.5 cm; case 4 cm across; 20 g.

**VR** [P]
- **Grip**: tip pinch on a plug (to be authored); fallback cup.
- **Verbs**:
  - **(logic) Use on self** (item 108 → 113): bring the hand to either ear (within about 8 cm of the head's side).
    That one gesture puts both in, as the one original verb does.
  - **(logic) Take out** (113 → 108): reach to the ear, close the pinch, pull away.
- **Feel** [owner to decide]: while worn, the game's audio could be muffled (a low-pass). The original's effect is the
  sector-edge test only; it is not known to muffle anything [R].
- **Voxel**: tiny, so a mesh.

## 156 Chalice (AQUA1) (tested 5×)

**Data** [F]
- Category 1, **weapon** (WeaponAction). Render id 0.
- Close-up: 24 frames, 200×125. The same film as item 127.
- Obtained: RAW 0x29 give AQUA1 ×2, LRINTH.
- Tested 5× in AQUA1:
  - on an object click: if-not-flag, 0x27 in hand, **remove 156, give 156** (taken and given back);
  - on a wall contact (0x1a) with it in hand: two wall textures change.
- Removed by action 562.

**Object** (film): a silver goblet, engraved spiral band, slim stem, round foot. Size [R]: 18 cm, cup 7 cm, 0.3 kg.

**VR** [P]
- **Grip**: the stem between the fingers, bowl over the hand: wrap_half on the stem. One hand. Carry: the pack.
- **Verbs**:
  - **(logic)** the object click = dip the chalice into the fountain (touch).
  - **(logic)** the wall contact = walking to the wall with it in hand (1:1).
  - Its weapon action: the weapon lane.
- **Feel** [P, owner]: water in the cup after the dip (the remove/give round trip suggests filling [R]). A drawn liquid
  surface, no state.
- **Voxel**: hollow cup (it is seen into); thin stem.

## 33 Staff (Florentine's Staff) (tested 4×)

**Data** [F]
- Category 1, **weapon**. Flags 0x5d. ChangeName to "Florentine's Staff" by a flag.
- Render id 0x25b, placed CAVERNS3. Close-up: 21 frames, 62×140, hold mode. Gives in CAVERNS3, CHURCH1 ×2, SALVAT, STUDY1.
- Tested 4×, all in hand on an **object click**:
  - AELF and STUDY1: dialogue, then a **map transition**;
  - CAVERNS3: light, height, floor texture, sound, flag.
- OnInspect: "Someone's walking stick?" Topics "Discuss" ×2.

**Object** (film): a long dark wood shaft with a brass headpiece, scrolled arms, and a green cabochon in a ring.
Size [R]: 150 cm, 1.2 kg.

**VR** [P]
- **Grip**: at the balance, a third down from the head, wrap_tight (837). The other hand lower for two-hand work: the
  support hand, wrap_half.
- **Carry**: the back, diagonally (a strap is invented carry, presentation only).
- **Verbs**: **(logic)** touch the gem head to the target object, which is its click. Its weapon action: the weapon lane.
- **Voxel**: the shaft as a mesh (long and thin); the head as voxels; the gem a separate glossy part.

## 223 Brain (tested 4×)

**Data** [F]
- Category 0. Render id 0x2f8, placed DOMINION ×16. Close-up: 24 frames, 200×126.
- Tested 4× in DOMINION, in hand, on a **wall use (0x18)**: remove the brain, a loop, toggles, run commands.
  The brains are fed into a machine.
- OnInspect: "A brain?"

**Object** (film): a wet, glossy human brain. Size [R]: 16 × 14 × 10 cm, 1.3 kg.

**VR** [P]
- **Grip**: cupped from below, cup (985), one hand; or both hands. Carry: the pack.
- **Verbs**: **(logic)** push it into the receptacle (touch the wall face).
- **Feel**: a wet squelch on grab, a slight wobble (the model, not physics).
- **Voxel**: solid, glossy; 2 mm.

## 6 Magnifying Glass (tested 3×)

**Data** [F]
- Category 3. Render id 0. Close-up: 20 frames, 179×140. Obtained: RAW 0x29 AQUA1 ×3, LRINTH1.
- Tested 3× in AQUA1, in hand, on **examine-wall (0x32)** triggers: the wall texture changes, then dialogue or a toggle.
  Hidden writing appears when the wall is examined with the glass in hand.
- **Combine** with item 149 (Scroll) → the Glass back + item 205 (Scroll). Effect action 555 says the scroll has
  writing and should be burned.
- OnInspect: gold, "No inherent magical qualities".

**Object** (film): a round lens in a gold rim, a black handle with a silver ferrule. Size [R]: lens 8 cm, 20 cm long,
150 g.

**VR** [P]
- **Grip**: the handle, wrap_tight. One hand. Carry: a coat pocket.
- **Verbs**:
  - **(logic)** Examine a wall through it. Hold the lens between the eye and the wall: the wall's examine target lies on
    the ray from the HMD through the lens. Dwell 0.8 s (phase-1 examine). With the glass in hand, the original's chain
    reveals the writing.
  - **(logic)** Combine: hold the glass over the scroll held in the other hand.
- **Renderer** [P]: a magnified view inside the lens (render to texture, or a second camera at narrow field of view).
  Renderer work; the fallback is a clear lens.
- **Voxel**: the lens is a separate translucent part; the rim and handle are voxels.

## 14 The Shrive (tested 3×)

**Data** [F]
- Category 3. Render id 0x287, placed SOULST3, TGATE1F. Close-up: 20 frames, 108×107. Given in 5 maps and by action 174.
- Tested 3×:
  - AELF wall use in hand: remove it, run commands, spawn, **give it back**, open two doors, flag;
  - MAS3 wall use in hand: height and wall texture;
  - AELF sector edge (carried).
- OnInspect: set with stones, "designed to screw into something". Topics: Adam's brands and the Shrive.

**Object** (film): an oval bronze frame bound with silver wire. A gold disc inside with a green gem in a diamond, red
stones at the base, and a threaded screw stem below. Size [R]: 12 × 10 × 2 cm plus a 3 cm screw; 0.5 kg.

**VR** [P]
- **Grip**: the frame's edge, thumb on the face: halfcurl (193) or wrap_half. One hand. Carry: the pack.
- **Verbs**: **(logic)** fires when its screw meets the socket (the wall use).
- **Feel**: twisting it home, about 180°. Ratchet clicks; the firing waits for the twist. **[P, owner]** The click
  then happens 0.5-1 s later than a mouse click would; there is no game consequence [R].
- **Voxel**: the wire binding is thin, so 1 mm; the gems are separate glossy parts.

## 35 Crystal (Gnarl) (tested 3×)

**Data** [F]
- Category 3. Render id 0. Close-up: 24 frames, 202×100. Given: GNARL1, STUDY1.
- Tested:
  - STUDY1 object click with it in hand and the Mask (60) carried: a **map transition** (to the Gnarl);
  - GNARL1 carried: dialogue; a STUDY1 sector edge.
- OnInspect: uncut crystal, a magical aura; "connected to the Gnarl".

**Object** (film): an elongated green crystal, faceted, with small pits. Size [R]: 10 × 3 × 3 cm, 80 g.

**VR** [P]: lateral pinch along its length (fallback cup); touch it to the target. A faint inner glow would be
invented; the film shows none. Voxel: solid, translucent green, so a separate translucent material.

## 60 Mask (Gnarl's mask) (tested 3×)

**Data** [F]
- Category 3. Flags 0x08. Render id 0x2a5. Close-up: 20 frames, 120×140.
- Given: CHURCH1, GNARL1, STUDY3, actions 542, 682.
- **OnUse on self, effect only, not consumed** (C/inventory.c:744: only a use that is not effect-only consumes): action 539. It either says "Nothing yet..."
  or runs RAW record 32002 by flag [R: the Tower trip the topics describe].
- Tested 3× carried (GNARL1, STUDY1 with the Crystal). Removed in STUDY2/STUDY3 and by action 545.
- Topics: "keys to time", the Tower.

**Object** (film): a gold Egyptian headdress mask (nemes) with a rearing cobra on the brow, eye openings, turquoise
inlay. Size [R]: 26 × 24 × 15 cm, 0.8 kg.

**VR** [P]
- **Grip**: both hands at the sides (the lappets), relaxed (154) or halfcurl. Or one hand by the cobra.
- **Carry**: the pack.
- **Verbs**:
  - **(logic) Use on self** = bring it to the face with both hands (inside about 12 cm of the HMD, facing it).
  - If the effect says "Nothing yet", the mask comes back down. If it runs the transition, the view goes through the
    eye holes into it.
- **Feel**: a brief eye-hole vignette as it is raised. Presentation; the owner decides.
- **Voxel**: a shell (hollow behind), the eye holes open.

## 97 Eternity, the Dragon Sword (tested 3×)

**Data** [F]
- Category 1, **weapon**, 3 WeaponAction blocks. Render id 0. Close-up: 20 frames, 170×140.
- Given in 8 maps and action 562.
- Tested:
  - SOULST3 object click in hand: dialogue and a map transition;
  - MAZE and TGATE1F carried: dialogue.
- OnInspect: "So now we have Eternity...". Topics.

**Object** (film): a straight double-edged sword, a gold crossguard with spikes and a green gem, a blue-grey wrapped grip
and pommel. Size [R]: 95 cm, blade 75 cm, 1.4 kg.

**VR** [P]
- **Grip**: wrap_tight on the grip. One hand, the other may join below (hand-and-a-half).
- **Carry**: a scabbard on the left hip (or the back).
- **Verbs**: **(logic)** touch the blade's point or the guard's gem to the target, which is the click. Combat is the
  weapon lane's (docs/REALMS_WEAPONS.md).
- **Voxel**: the blade as a mesh (thin); the hilt as voxels.

## 127 Chalice (ANUBIS) (tested 3×)

**Data** [F]
- Category 1, weapon. Render id 0x24f, placed ANUBIS ×2. Close-up: the same film as 156.
- Tested in ANUBIS:
  - object click carried: remove, give, delay;
  - wall contact in hand: **two doors open**.
- OnInspect jumps to action 68.

**VR**: as item 156 (the same object). The wall contact is walking into it with the chalice held (1:1).

## 131 Fan (tested 3×)

**Data** [F]
- Category 0. Render id 0x24b, placed AQUA1 ×2. Close-up: 24 frames, 200×125.
- Obtained: a give in AQUA1, or the result of item 209's OnUse. Target of item 209's OnUse. Removed by action 562.
- Tested in AQUA1:
  - **an altar** with "last item" compares. A floor use removes the last-used item (RAW 0x2a with id 0) and counts;
    an object click gives it back.
  - a wall use in hand.
- OnInspect: a paper fan, oriental; yin and yang.

**Object** (film): a folding paper fan, a yin-yang on grey paper, wooden ribs, a blue-grey end knob. Size [R]: 25 cm
ribs, 45 cm open, 60 g.

**VR** [P]
- **Grip**: the pivot end, wrap_half. Spreading it open with the thumb is feel; the film shows it opening.
- **Verbs**: **(logic)** lay it on the altar: touch the altar's floor with it and release. Take it back: grip the item
  on the altar, which is the object click.
- **Voxel**: the paper is a thin surface, so a mesh with ribs; open and closed are two models.

## 28 Shield (dragon) (tested 2×)

**Data** [F]
- Category 1, weapon. Render id 0x26d. Close-up: 20 frames, 156×126. Given STUDY1.
- Tested in ANUBIS, `in-hand,no-autoselect`:
  - a wall use: spawn, delay;
  - **a sector edge: damage, gated by the shield in hand**. The shield protects from the fire [R: the 0x27
    polarity f6=0x22 was not traced; the OnInspect's scorch and the chain's shape say so].
- OnInspect: scorched, "Quite recently too."

**Object** (film): a heater shield, red field with a gold dragon rampant. Size [R]: 70 × 50 cm, 3 kg, wood and leather,
painted.

**VR** [P]
- **Grip**: forearm through the strap, hand on the grip, wrap_tight. Off hand.
- **Carry**: the back.
- **Verbs**: **(logic)** the protection is the original's in-hand test on the sector edge; holding it is enough
  (1:1, no aiming needed). [R] Requiring it raised toward the fire would be stricter than the original, so no.
- **Voxel**: a shell 2 cm thick; the dragon is the texture (the film's relief is shallow).

## 37 Crystal (CAVERNS) (tested 2×)

**Data** [F]
- Category 0. Render id 0. **The same film as item 103** (Jewel): 20 frames, 121×140. Given CAVERNS, STUDY1.
- Tested in CAVERNS: a **list** test with item 249 (Crystal), then a last-item compare. Remove 37, remove 249 ×2:
  two crystals are placed together.
- OnInspect: "Some sort of key perhaps?"

**Object** (film): a red and black lacquered, faceted bud shape. Size [R]: 6 × 4 cm, 50 g.

**VR**: lateral pinch or cup; touch it to the receptacle. Voxel: solid, glossy.

## 49 Statuette (quartz) (tested 2×)

**Data** [F]
- Category 3. Render id 0x2e2. Close-up: 20 frames, 68×140. Given STUDY2 ×2, STUDY4.
- Tested in STUDY2, in hand, on a **wall use**: remove it, wall texture, toggles. It **opened the library door**
  (OnInspect). The idol holds a small book.

**Object** (film): a translucent blue quartz figure of a woman holding an open book, on a conical base. Size [R]:
16 cm, 0.4 kg.

**VR** [P]: wrap_half round the figure, or the base on the palm. **(logic)** set it into the wall niche: touch and
release. Voxel: translucent material, solid.

## 103 Jewel (garnet) (tested 2×)

**Data** [F]
- Category 0. Render id 0x233, placed CAVERNS, RAQUIA5 ×2, SALVAT. The same film as 37.
- Tested in RAQUIA5: **the gem puzzle**. A floor use removes the last-used item and counts; an object click gives it
  back. Then lighting, heights and object textures.
- OnInspect: "A Garnet stone I believe."

**VR** [P], the same for the six RAQUIA5 gems (103, 106, 114, 115, 122, 123) and the Amber (154):
- **Grip**: lateral pinch (until then, cup with the gem between thumb and index).
- **Carry**: a small pouch, or the coat pocket.
- **Verbs**: **(logic)** set the gem on its pedestal or socket: the floor use, then release. Taking it back is the
  object click.
- **Feel**: a stone-on-stone click.
- **Voxel**: 2-3 cm across, so 1 mm voxels; a glossy, translucent material per stone.

## 106 Gem (amethyst) (tested 2×)
- Data: Category 0. Render id 0x2bc, placed RAQUIA5 ×2. Close-up: 20 frames, 170×140. RAQUIA5 gem puzzle, as 103.
  OnInspect: "It's an Amethyst".
- Object (film): a dark purple domed cabochon. Size [R]: 3 cm, 15 g.
- VR: as 103.

## 114 Coloured Gem (emerald) (tested 2×)
- Data: render id 0x237, placed RAQUIA5 ×2, SALVAT. 20 frames, 174×140. Gem puzzle. "It's an Emerald."
- Object: a faceted green round stone. Size [R]: 2.5 cm.
- VR: as 103.

## 115 Coloured Gem (turquoise) (tested 2×)
- Data: render id 0x256, placed RAQUIA5 ×2. Gem puzzle. "Turquoise." (and a joke about the pronunciation).
- Object: a faceted lilac and blue round stone. Size [R]: 2.5 cm.
- VR: as 103.

## 116 Ring (tested 2×)

**Data** [F]
- Category 3. Render id 0. Close-up: 10 frames, 117×85.
- Given: RAQUIA3 ×2, RAQUIA5; and the result of 215's use (taking it off).
- **Combine with item 217 (Gem) → item 203 The Spirit Ring.** Effect action 551: a flag, RAW 32019, "the Ring is now
  complete".
- **Use on self → item 215 Ring (worn).** Effect action 592.
- Tested in RAQUIA3: a sector edge carried (dialogue), and in hand (flags, dialogue, a door).

**Object** (film): a silver band with a large flat round bezel, an engraved dial, the gem socket empty. Size [R]: band
2 cm, bezel 2.5 cm, 15 g.

**VR** [P]
- **Grip**: lateral pinch on the bezel.
- **Verbs**:
  - **(logic) Use on self**: slide it onto a finger of the other hand. Bring the ring within 3 cm of that hand's ring
    finger. While worn (item 215) it is drawn on that finger.
  - **(logic) Combine**: press the gem into the bezel (bring them together).
- **Voxel**: a mesh; a ring is too thin for voxels.

## 122 Coloured Gem (sapphire) (tested 2×)
- Data: render id 0x257, placed RAQUIA5 ×2. 20 frames, 200×91. Gem puzzle. "A Sapphire. It's a little dull though."
- Object: a flat, deep blue oval cabochon. Size [R]: 3 × 2 cm.
- VR: as 103.

## 123 Coloured Gem (topaz) (tested 2×)
- Data: render id 0x255, placed RAQUIA5 ×2. Gem puzzle. "This is a Topaz." (a false one).
- Object: a flat yellow oval. Size [R]: 3 × 2 cm.
- VR: as 103.

## 128 Feather (stained glass) (tested 2×)

**Data** [F]
- Category 3. Render id 0x23e. Close-up: 20 frames, 200×125.
- Given: CHURCH1 ×2, action 479.
- Tested in CHURCH1: a **list** test with item 101 (Feather), then a last-item compare. Remove 128, flag, spawn: the
  feathers are returned to the window.
- OnInspect: "From the stained-glass window."

**Object** (film): a long red-orange feather. Size [R]: 15 cm, 1 g.

**VR** [P]
- **Grip**: tip pinch at the quill (fallback: the trigger state).
- **Verbs**: **(logic)** touch it to the floor spot below the window (the floor use).
- **Feel**: it trails a little as it moves (vertex sway).
- **Voxel**: a flat card mesh, not voxels.

## 152 Bowl and Snow (tested 2×)

**Data** [F]
- Category 0. Render id 0x24a, placed AQUA1. Close-up: 20 frames, 167×140.
- Obtained: the result of item 153's OnUse. Target of the OnUse of 111, 133 and 204.
- AQUA1 altar (as the Fan).
- OnInspect: the powder has turned to snow.

**Object** (film): a grey stone bowl with snowflakes swirling above it. Size [R]: 16 cm across, 6 cm deep, 0.6 kg.

**VR** [P]
- **Grip**: one hand under the base, palm up (relaxed 154 turned up, or cup); two hands for care.
- **Verbs**: **(logic)** the altar placement as the Fan; combines (the other items into the bowl) by bringing them
  together.
- **Feel**: the swirling flakes as particles above it (the film shows them, so this is not invented).
- **Voxel**: a hollow bowl; the snow as particles.

## 154 Amber (tested 2×)
- Data: Category 0. Render id 0x249, placed AQUA1, RAQUIA5 ×2, SALVAT. 20 frames, 127×102. RAQUIA5 gem puzzle.
  **Combine with item 195 (Mortar and Pestle) → item 155.** "Amber. Fossil Resin."
- Object: a faceted orange piece. Size [R]: 3 cm, 10 g.
- VR: as 103. Combine: drop it into the mortar held in the other hand, i.e. bring them together.

## 155 Amber and Mortar (tested 2×)

**Data** [F]
- Category 0. Render id 0x246, placed AQUA1. Close-up: 31 frames, 133×140.
- Obtained: the result of 154's combine. Target of the OnUse of 111 and 207.
- AQUA1 altar. Removed by action 562.
- OnInspect: "it used to be burned as incense".

**Object** (film): a pale stone mortar and a heavy pestle; the film shows the pestle grinding orange powder. Size [R]:
12 cm across, pestle 12 cm; 0.8 kg.

**VR** [P]
- **Grip**: the mortar on the off palm (cup), the pestle in the main hand (wrap_tight).
- **Feel**: a grinding circle (the film shows it).
- **Verbs**: the altar placement (logic).
- **Voxel**: a hollow mortar; the pestle is separate.

## 167 Key (silver) (tested 2×)

**Data** [F]
- Category 0. Flags 0x44 (ChangeName). Render id 0x2c1, shared with item 20, so a placed one resolves to 20.
- Close-up: 20 frames, 171×140. Obtained: in the blocks of item 172 (the Journal).
- Tested:
  - CAVERNS3 wall use in hand: flag, **open door**;
  - VICAR wall use carried: open door.
- OnInspect: "Nicely worked silver key."

**Object** (film): a silver bit key with a round bow and a stepped bit. Size [R]: 8 cm, 30 g.

**VR** [P], for every key:
- **Grip**: lateral pinch on the bow (until then, the trigger state).
- **Carry**: a key ring on the belt [P]; the original lists them in the pack.
- **Verbs**:
  - **(logic)** fires when the bit enters the keyhole (touch the door's lock face).
  - **(feel)** a quarter turn of the wrist; the door's opening is the original's chain.
- **Voxel**: a mesh (thin bit).

## 171 Key (floral) (tested 2×)
- Data: render id 0. 20 frames, 264×146. Given VICAR1. Tested in VICAR1, wall use in hand, opens doors.
  OnInspect: "Key. Floral design.", used on the Observatory door.
- Object (film): a big ornate key whose bow is a ring of green leaves on a small tree. Size [R]: 14 cm, 60 g.
- VR: as 167. The leaves are a card mesh.

## 193 Key (plain) (tested 2×)
- Data: render id 0. 20 frames, 170×140. **No trigger blocks.** Given VICAR. Tested in CHURCH1, wall use in hand
  (open door), and a dialogue when absent.
- Object (film): a long plain barrel key with a pierced oval bow. Size [R]: 12 cm.
- VR: as 167.

## 194 Tinder (Burning) (tested 2×)

**Data** [F]
- Category 0. Render id 0x244, placed AQUA1. Close-up: 24 frames, 200×125, burning.
- Obtained: the result of item 134's OnUse (Tinder lit; with the Flint 132 [R]). Target of the OnUse of 208.
- AQUA1 altar. Removed by action 562.

**Object** (film): a small pyre of sticks, burning with smoke. Size [R]: 10 cm, 50 g.

**VR** [P]
- **Grip**: from below, cup.
- **Verbs**: the altar placement (logic).
- **Light**: it burns in the original's own film, so a small flame in VR is not invented as an object. But its
  **light on the room** is: radius 30-40 units, as the match (section 4), and the owner decides.
- **Feel**: crackle.
- **Voxel**: sticks as voxels; fire and smoke as particles.

## 202 Spectacles (worn) (tested 2×)

**Data** [F]
- Category 0. Render id 0. Close-up: 20 frames, 200×110.
- **Use on self → item 124 Spectacles** (take off). Obtained by 124's use on self (put on).
- Tested in RAQUIA5 carried, on a sector edge: **objects are spawned** (what the spectacles let Adam see).
- OnInspect: magic woven in, "Something to do with sight."

**Object** (film): small wire spectacles, rectangular lenses striped in rainbow colours, long wavy temples. Size [R]:
12 cm wide, 15 g.

**VR** [P]
- **Grip**: both temples, tip pinch each; or one hand by the bridge.
- **Verbs**:
  - **(logic) put on** (124 → 202): raise them to the eyes.
  - **(logic) take off**: grip at the bridge and lower.
- **Feel**: a faint rainbow fringe at the edge of view while worn. Presentation; the owner decides.
- **Voxel**: a mesh; the lenses translucent.

## 216 Spirit Ring (worn) (tested 2×)
- Data: Category 3. Render id 0. 10 frames, 153×85. **Use on self → item 203** (take off), effect action 591.
  Obtained by 203's use on self. Tested in RAQUIA5 carried, sector edges: dialogue and flags. OnInspect: complete now.
- Object (film): a silver band with a large dark purple domed stone.
- VR: worn on the off hand's ring finger. Taking it off: pinch and slide off (logic: use on self). Mesh.

## 9 Colt-45 (tested 1×)

**Data** [F]
- Category 1, weapon. Render id 0x263, shared with 239. Placed MAS3, STUDY1. Given by action 637.
- Close-up: 10 frames, 200×114.
- Tested in MAS3 on a sector edge, carried (count mode): heights, sound, spawn.
- Ammo: item 51 (magazine) and 48 (cartridges) (docs/REALMS_WEAPONS.md).

**Object** (film): an M1911-type pistol, dark finish, chequered grips. Size [R]: 22 cm, 1.1 kg.

**VR** [P]: the weapon lane (`RealmsWeapon9`; `grip_pistol` from the table, the trigger finger `grip_pistol_fire`); a
holster on the right hip. Reloading with `hold_magazine` and `hold_inserting`, if the original's reload is kept 1:1
(docs/REALMS_WEAPONS.md). Voxel: the weapon lane's.

## 18 Orb (tested 1×)

**Data** [F]
- Category 0. Render id 0x2ca, placed MAS3, SALVAT ×12. Close-up: 20 frames, 82×81. Given MAS3 ×2.
- Tested in MAS3 in hand, on a **wall use**: remove it, wall texture, then a sequence of run-commands. It goes into a
  socket.
- OnInspect: "Metallic orb. Feels... alive."

**Object** (film): a bronze sphere with a cloudy surface and a warm glow on its underside. Size [R]: 8 cm, 1 kg.

**VR** [P]
- **Grip**: cup (985), one hand.
- **Verbs**: **(logic)** set it into the socket (touch the wall face).
- **Feel**: a slow haptic pulse while held ("alive"), no state.
- **Voxel**: a sphere; the glow as an emissive patch, with no light on the room (the original has none).

---

## Still to design (items 41-281 of the priority order)

Each has its data half from `tools/item_dossier "<DATA>" --item N`.

- **Tested in hand, items 41-69 of the order**: 20, 21, 22 (Keys); 53 Perdition Shield; 84 Journal; 98 Coloured Gem;
  101 Feather; 102 Tiara; 105 Flask; 109 Haunch; 125 Coloured Key; 129 Key; 130 Bowl and Powder; 134 Tinder;
  135 Mortar; 142 Flask; 144 The Key of Tears; 153 Flask and Powder; 172 Journal; 173 Key; 185 Coin;
  212 Armlet (worn); 215 Ring (worn); 218, 219 (Keys); 220 Florentine's Journal (part 3); 236 Key; 249 Crystal;
  256 Key.
- **Placed in the world, items 70-127**:
  - creatures (AsMonster records, not held; their design is the combat lane's): 30, 31, 85, 91, 92, 93, 94, 120, 121,
    157, 177, 186, 188, 189, 190, 191, 198, 229, 233, 250, 257, 258, 259;
  - things to hold: 48 Cartridges; 51 Colt-45 magazine; 143, 170, 206 Vials; 66 Aelf's Breastplate and 104 The Dragon
    Helm (both category 2: never in a hand); 13, 39, 40, 42 Parchments; 65 Magnifying Glass (AQUA world copy);
    67 Quill & Ink; 11 Letter; 12 Staff (Creator's); 17 Book; 19 Aelf's Dagger; 38 Shotgun; 45 Helm; 46 Key;
    52 Ace of Spades; 54 Gun; 99 Pipes; 100 Armlet; 108 Earplugs; 111 Water-pipe; 124 Spectacles; 126 Gem;
    132 Flint; 133 Cylinder; 136 Pestle; 138, 139 Books; 151 Magical Torch; 176 Key; 260 Die.
- **The rest, items 128-281**: the remaining documents (letters, news cuttings, notes, maps, journals, the Scrapbook),
  the category-2 people and places, bullets (AsBullet records), and the worn and other variants.
  `item_dossier --order` lists them.

Documents are 40 records with Document blocks [F: dbase100_dump]. They share one design [P]:
- read the page in hand, the original's page image on the paper;
- turn pages with the other hand: the original's page up/down;
- fold it away into the coat.

That design should be written once, as its own section, when the first document comes up.

---

## OWNER'S DECISIONS (2026-11-13, 05:35-05:55)

**The principle (owner, verbatim):** "we need to start thinking about vr features versus vanilla. we want to keep vanilla
functionality but have our VR stuff be the goal." Every original path keeps working (the click verbs, point-and-tap); the
VR way is the designed, featured way.

1. **Lamp (item 15): the light comes FROM the lamp.** The original's player light (the flickering cone of the "candle"
   sectors, C/renderer.c:9193-9198, 13411-13428; not tied to the Lamp in the original) is emitted from the lamp in Adam's
   hand in VR, so it shines where it is held. Not 1:1 with the original's frame; chosen by the owner.
2. **Candles: ONE soft, optimised, flickering dynamic light per candle holder that GROWS as each wick is lit.** Each wick
   swaps to lit as it catches; the original's script (lit picture, the room's light step) still fires as the game says.
   The original lights all flames of a holder in one picture swap; lighting them one at a time means drawing the flames at
   the wicks ourselves (easiest once the holders are voxels).
3. **Lighting a candle: both ways.** The match flame touching the wick is the VR way (the goal); the original's
   point-and-tap with the matchbox selected keeps working (vanilla).
4. **The Magical Torch (items 151/175, CHURCH1/Vicarage): the BEAM REVEALS the wards.** It is the puzzle item that makes the
   green lemniscatic wards visible (Belial's wand, item 150, dispels them). In VR a pale magic beam comes from the lens and a
   ward shows only where the beam falls, like UV light. Vanilla stays available: holding it shows the wards as the original
   does (a setting).
