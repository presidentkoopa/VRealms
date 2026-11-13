# WORLD_DESIGN — VR design dossiers for the things placed in Realms' levels

2026-11-13. Owner's authorization for VR interaction design, verbatim: "innovative VR ideas for a 30 year old
game never meant to be in true 3d"; "take our time and get it right"; "take some time to think about each one too
... ask where adam would hold this and the kind of pose it would need"; for this work: "even things in the world.
pickup sprites? remember those candelabras i mentioned before? doorknobs, right?".

The INVENTORY items (what is carried, the panel, the hands in the panel) are `docs/ITEM_DESIGN.md`, another lane.
This file covers everything PLACED in the levels: doors and their handles, candles and other lights, switches and
levers, pickups as they lie, books and shelves, furniture and meshes, and the rest of the scripted objects.

## How to read this file

Every statement carries one of four marks:

| mark | means |
|---|---|
| **[V: how]** | verified, and how: a census count (`tools/world_census`), a chain dump, a line of ROTH.C that was read, or a measurement in `docs/REMAROTH_MEASURED.md` |
| **[R]** | reasoning only, not seen in the engine or the original |
| **[P]** | a design proposal for VR interaction: how the hand gets to the original's verb. It changes no game state of its own |
| **[ADD]** | a pure VR addition: something the original does not have at all. Cosmetic, separable (its own switch, off unless the owner turns it on), and it never touches game state |

Nothing here has been built or seen in a headset. Every proposal ends in a verb of the original, named with its
record ids, so the result can be checked against the original by the differential tests later.

---

## 0. The rules, the picker, the scale, the hands, the lights

### 0.1 Game logic stays the original's

Every interaction below ends in one of the original's verbs, with the item in hand as the scripts test it:

| verb | the original's code | what it fires |
|---|---|---|
| **use** | `activate_targeted_object` (input.c:1026-1100) | object: the click directory (header +0x08: base 8 runs the chain, base 2 is a light switch), else a plain pickup (`give_item_by_dbase_id`); open door leaf: `toggle_door_open_state`; wall face: the 0x18 records; floor or platform top: the 0x19 records |
| **examine** | `examine_object_under_cursor` → `dispatch_entry_command_trigger_b` | object 0x30, wall 0x32, floor/ceiling 0x31; else the item's DBASE100 OnInspect |
| **attack hits** | `collide_ray_walls_recursive` → 0x1a; entity hit → 0x1b | the weapon's hit on a wall face or an object |
| **item in hand** | RAW 0x27 `cmd_if_not_item` (raw_commands.c:4760) with `query_player_inventory` | select mode passes for the item in the RIGHT or the LEFT hand slot; carried but not in a hand: auto-selected into the right hand, the chain is interrupted, and that click does nothing (docs/GAME_inventory.md 4) |

[V: input.c and raw_commands.c read for this file; the category directories checked against all 44 maps — the
click directory holds exactly the 0x08 and 0x02 records (377 + 31), the examine-object one only 0x30 (191), and so
on: `world_census` prints this "CATEGORY CHECK".]

A VR gesture may add feel (turn the handle, touch each wick). The **result** must be the original's chain and state.
Anything the original never does is marked [ADD] and stays off the game state.

### 0.2 The picker (phase 1, `src/roth/roth_vrpick.cpp`) and what this file adds on top

Phase 1 already maps the hands onto the original's cursor and verbs [V: read the file]:

| hand | verb | gate |
|---|---|---|
| point (the index straight, off the trigger; the grip may be curled), ray from the index tip | classify (the cursor) | the original's `classify_cursor_target_object` |
| the trigger, pointing at it (the owner's scheme, 2026-11-13; the 0.8 s dwell is removed) | examine | cursor type 3 or 5 with something to examine |
| the grip squeezed while pointing at it (the index tap is removed) | use, far | cursor type 3 within the use range |
| fingertip within 2.5 cm of a usable thing (re-arms at 6 cm) | use, near | the same |
| grip closing 0.35 → 0.7 with the palm within 15 cm of an object, or a usable surface 20 cm along the hand | use, near | the same |

This file adds **per-kind contact points** (where on the thing the hand goes: the painted door handle, a candle's
wick, a switch plate), **per-kind gestures** on top of those three (turn a handle, strike a match, slap a switch),
and **per-kind feedback**. The verbs and gates stay phase 1's.

Two things phase 1 does not do yet, which several kinds below need [V: read roth_vrpick.cpp and roth_runtime.cpp]:

1. **The depth rule.** The original measures reach as the horizontal distance from the player's x,y to the picked
   point **along the view yaw** (planar, not along the ray, height ignored; MEASURED 13). `roth_vrpick.cpp:381`
   `DepthOf` uses the straight 3D distance from the eye, and the use range is still the provisional 600
   (`rothmap.cpp`, `ROTH_USE_RANGE_PROVISIONAL`). Measured: use **z < 268**, "too far" cursor 268 ≤ z < 968,
   examine **z < 2467** world units. [P] In VR the "view yaw" of a hand ray is the ray's own horizontal direction,
   so z = the horizontal distance from the player's centre to the point. Measured along the HEAD yaw, a hand
   pointing sideways would have unlimited reach.
2. **The surface box of a trigger.** 161 of the 240 wall-use (0x18) records, 21 of the 48 floor-use (0x19) and 219 of
   the 259 wall-examine (0x32) records carry a box at +0x0c…+0x12 [V: count over all maps, `/tmp` probe]. The
   original compares it with the pick record's words +4/+6, which the renderer fills with the **hit texel**, so
   *where on the surface* the cursor is decides whether the record fires (MEASURED 13). The port does not test the
   box (`roth_runtime.cpp:471`, :757). A VR hand gives that point naturally: the fingertip or palm contact on the
   face, converted to the texel coordinates the original's renderer would have written (`renderer.c:4489-4494`,
   walls; `:13001-13002`, floors). This decides, for example, whether a door's own record fires when the hand is on
   the handle.

### 0.3 The scale: units to metres

The headset's metre is set from the original's own eye (rothmap.cpp, "THE HEADSET'S METRE"): the original's eye is
144 units above the feet, and the world gets the original's vertical stretch FY/FX = 1.1462. With the player's real
eye height E metres [R, from that code]:

- **height** above a floor: `h_m = units × E / 144`
- **horizontal** distance: `d_m = units × E / (144 × 1.1462) = units × E / 165`

Every metre figure below is for **E = 1.65 m**, so 1 m is about 87 units up and 100 units across. For another
player, scale by E/1.65.

| thing | units | metres (E = 1.65) |
|---|---|---|
| use reach (measured) | z < 268 | 2.7 m |
| "too far" cursor edge | 968 | 9.7 m |
| examine reach | 2467 | 24.7 m |
| player radius | 28 | 0.28 m |
| a common door: width × thickness × height | 128 × 12 × 218 | 1.28 × 0.12 × 2.50 m |
| the painted door handle, above the door's floor | ≈ 94 | 1.08 m |
| a light switch plate (side), above the floor | 19, at 102–120 | 0.19 m, at 1.17–1.38 m |
| the five-candle floor candelabra (height) | 164 | 1.88 m |
| a single floor candlestick (STUDY4) | 53 | 0.61 m |

Reaching: the use reach is 2.7 m, so most "near" interactions are in arm's reach only if the player walks up to
the thing. The picker's far use (the grip while pointing) covers the rest of the original's reach. Nothing in VR may reach beyond
the original's 268.

### 0.4 The hands and the poses

[V: from the hands handoff and docs/FBX_LOADER.md; the engine poses are shown in
`docs/images/hands/engine_poses_flat.png`, built on the owner's PC.] The engine poses the owner's FBX hands
(`hand_final.fbx`) from authored frames of its one take, frame = Blender frame − 1:

| engine state | frames (right / left) |
|---|---|
| open | 0 |
| fist (and the trigger's index) | 318 |
| point | index R 808 / L 0, the rest 318 |
| thumb up | thumb R 990 / L 205, the rest 318 |
| finger gun | R 808 (middle 318) / L index 0, thumb 205, the rest 318 |

The take holds more authored hand shapes. These are named in UZDXREMA's `tools/avatar/fbx_poses.json` (Blender
numbering; the json's "A"/"B" tag says which hand carries each). They were **not** looked at for this file [R]:

| name in fbx_poses.json | Blender frame | what it is for, per the json |
|---|---|---|
| fbx_relaxed | 154 | resting hand |
| fbx_halfcurl / fbx_curl | 193 / 206 | partly closed |
| fbx_crossed | 304 | fingers crossed |
| fbx_twofinger_point | 762 (A) | index and middle out |
| fbx_wrap_half | 820 (B) | "a hand round a magazine body" |
| fbx_wrap_tight | 837 (B) | "a hand closed on a slide", "wrapped round a pump" |
| fbx_thumb_out | 944 (B) | thumb out |
| fbx_cup | 985 (B) | "index and middle folded at the middle joint, thumb in": the nearest thing to a pinch |

**CORRECTED 2026-11-13 (owner): the FBX HAS pinches.** The earlier claim here ("no pinch") came from the pose table,
not from looking at the animation, and was wrong. Walking both takes frame by frame (`tools/fbxload_test/walk_sheet.py`,
`docs/images/hands/take_walk_*.png`, `pinch_frames_*.png`) shows a thumb-to-index "O" pinch in BOTH hands at Blender 694
(take 693), a tighter one at Blender 312 (take 311), and the left hand's Blender 985-1265 "cup" is all fingertips
bunched onto the thumb. `fbx_pinch` (694) and `fbx_pinch_tight` (312) are in the engine (13de3c47) and used by every
small item (keys, gems, ring, feather, crystals, earplugs, spectacles). Where one hand's take lacks a frame the other
has, the frame's joint rotations are COPIED from the other hand (owner, 2026-11-13; both skeletons share their rest
locals, measured), so both hands have all 52 poses.

The wrist always belongs to the controller (handoff, open question "May the wrist ever be animated?"). So a "turn"
gesture below is the player's own wrist roll, read from the controller, never an animation.

### 0.5 Light: what the original does, what this engine can do

**The original has no dynamic lights** [V: ROTH_LIGHTING.md; MEASURED 154-175]. It lights with:

1. **Sector light ramps.** A sector's light byte moves its distance-shade curve. Scripts fade it (0x1d "change
   lighting"); every candelabra in the original lights its room this way (section 2).
2. **Light switches** (base 0x02 click records, section 3) ramp a connected group of sectors by a signed step, and
   at load every armed one sets the group **lights-out** (sector +0x0a bit 1). STUDY1 runs three at load and darkens
   139 sectors (MEASURED, "Lights out at load").
3. **The lantern cone.** In a lights-out sector the shade row comes from a radial falloff around the **screen
   centre**, jittered by a flame flicker every 7 ticks of the 70 Hz clock (MEASURED 154-170; the port does this in
   `main.vp`, verified on 2,943 of 2,943 logged vertices). The player carries a light that follows his view. Nothing
   found ties it to an item [R: no item test seen in that rule].

**What the engine can do** [V: read the sources named]:

- **Dynamic lights** (GZDoom `FDynamicLight`): point and spot lights on actors, lighting walls, flats and models.
- **Shadows: 1D shadow maps only** (`hw_shadowmap.cpp`, header comment). One 1024-texel row per light, at most
  1024 lights, traced against the level's **lines** in 2D. Floors, ceilings, 3D floors, sprites and models cast no
  shadow. `gl_light_shadowmap` defaults to off.
- **Effect lights** (`hw_effectlights.h`, from UZDXREMA): short, render-only lights that belong to no actor. They are
  evaluated at frame rate, binned into a grid around the eye (32-unit bins at the default quality, 3072 × 3072 × 768
  units), and take shadow-map rows after the dynamic lights. `AddFrameLight` hands over up to 256 lights a frame from
  a renderer-side system. A crowded bin (more than `r_effectlights_perbin`, default 16) keeps its best 15 and
  **merges the rest into one residual glow**: position weighted by their light at the bin centre, radius the
  largest, colour the sum measured there (`hw_effectlightcore.h:505-525`). Presentation only: nothing reads back,
  nothing is saved, netplay-safe by construction.
- [R] Whether a dynamic or effect light shows on top of the original's palette shading (`roth_palette_shading 1`)
  was not checked. It must be looked at before anything below that adds light is built.

**The candle merge** (the owner's idea: "several small dynamic lights merging into bigger, cheaper ones as more
flames catch") [ADD] — how to do it here:

- A **cluster** is the set of flames that light together in the original: one candelabra object (five flames, one art
  swap), or every object sharing one click id (STUDY1's pair of tall candles, id 3; STUDY4's five floor candles,
  id 1). The census gives the clusters (`-kind` lists each placement's id).
- While a cluster has one flame lit: one `AddFrameLight` point at the flame, warm colour, radius r₀ (proposal: 48
  units ≈ 0.48 m, to be tuned in the headset).
- With n flames lit: **one** light. Position = the flames' intensity-weighted centroid. Colour = the sum of the
  flames' colours, clamped at the 64-per-channel ceiling of `FrameLight`. Radius = r₀·√n, so the lit area grows
  with the summed power once the colour saturates. Flicker: each flame's own phase while separate; after the merge,
  the mean of their phases, so the flicker smooths out as the flame grows, as a real cluster's does.
- Cost: one light and at most one shadow-map row per cluster instead of n. The engine's bin merge only acts past 16
  lights in a 32-unit bin, which five candles never reach. So this cluster merge is a step **before** `AddFrameLight`,
  inside the VR light system, and the bin merge stays as the safety net.
- It never changes a sector's light. The original's 0x1d ramp still runs exactly as before, and that ramp **is** the
  game's light.

### 0.6 The census

`tools/world_census/` (new with this file) makes every count here reproducible:

```
sh tools/world_census/build.sh                      # writes /tmp/world_census
/tmp/world_census "<ROTH DATA>"                     # all tables (tools/world_census/census_retail.txt)
/tmp/world_census "<ROTH DATA>" -kind "DEMO[4098]"  # every placement of one kind, with its records
/tmp/world_census "<ROTH DATA>" -chain STUDY1 18    # one record and the chain it walks
/tmp/world_census "<ROTH DATA>" -dump /tmp/wc/art   # each kind's picture (retail art: /tmp only)
python3 -I tools/world_census/sheet.py <census.txt> /tmp/wc/art /tmp/wc/sheets   # contact sheets
```

An object **kind** is the art it wears, `PACK[index]`. The same index in two map packs is two different pictures,
so the pack is part of the key. Totals [V: census]:

- 5,324 objects in 355 kinds, 44 maps.
- Object scripts by placement: click chain 530 (269 of them give an item); light switch 39; examine 792; attack-hit
  162 (mostly creatures); walk-into 52; **plain pickup 48**.
- Wall records: use 0x18 240 (156 on door faces, 84 elsewhere); examine 0x32 259; attack-hits 0x1a 72.
- Doors: 167.

Kinds were identified **by looking at their first picture** on contact sheets made with this tool, and the meshes
with `tools/propcheck/proprender`. [V: looked at, /tmp/wc/sheets/*.png; a name like "candelabra" is my reading of
the picture.]

---

## 1. Doors, and their handles

### 1.1 What they are [V: census -doors, tools/doorcheck, ROTH.C doors.c:474-577 via doorcheck's comments]

**167 doors** in 17 maps. STUDY1 has 30, CHURCH1 20, STUDY2 18, STUDY4 15, CAVERNS3 and RAQUIA5 12 each; the rest
have 2–9.

- A closed door is no object at all. The player sees, and clicks, the **room face's** transparent mid picture. An
  open door is a box leaf of four quads that swings about its hinge (`spawn_door_instance`); while it exists, the
  picker meets it as a **door leaf** (pick type 6).
- Sizes (width × thickness × height, units): 74 are 128 × 12 × 218. Overall, 133 are 100–132 wide, 19 narrower
  (hatches, cupboards, gates at 42–96) and 15 wider (188–192: double-width gates and the big 400-tall doors).
  Heights run 60–400; 92 are 218.
- **Double doors**: 26 leaves carry sector id 0xFFFE, the second panel, which opens with its neighbour
  (`resolve_door_neighbor_sector`).
- **Swing, from the hinge marker** (doors.c:523-531):
  - 0xFFFD, 74 doors: always swings counter-clockwise, into the room across c1;
  - 0xFFFE, 86 doors: always clockwise, into the room across c3;
  - 0xFFFF, 7 doors: swings **away from whoever opens it**, a push door from both sides.

  So 160 doors open toward one fixed room: from that side the leaf comes **toward** the player (pull), and from the
  other it goes away (push). The leaf turns 90 degrees (angle byte 0x40) about the pivot at its hinge edge (c0).
- **The handle is painted on the door's picture, and it is always at the latch edge.** The five common door
  pictures are DEMO[100], DEMO[218], DEMO1[199] (handle plate on the right) and DEMO[90], DEMO[168] (on the left).
  They cover 142 door sides. Counting u from the room face's vertex1 to vertex2 and reversed by FLIPX, the painted
  handle lands at the door's **latch** edge (c2) on **142 of 142** sides and at the hinge on none. [V: census
  `-doors` picture/flip/latch columns, checked by script.] [R: the u direction is my reading of the convention. The
  100% agreement is the evidence for it.]
- **Where the handle is on the picture** [V: measured on the decoded art]: a brass lever plate centred at u = 0.88
  (0.11 on the left-handed pictures), **43% of the picture's height up from its bottom**, 12 texels wide. On a
  128 × 218 door: centre 15 units in from the latch edge and ≈ 94 units up. That is about 1.08 m, a real door
  handle's height. The plate is about 24 units wide and a few texels tall: a **lever handle**, not a round knob.
- 192 door sides wear 55 other pictures (gates, panelled doors, arched doors, a bookshelf, a clock). Each needs its
  handle point read off its picture once. Where a picture has no handle (gates, the bookshelf), the contact zone is
  the latch half of the face.

### 1.2 What the original does when a door is used [V: chain dumps; DBASE100 lines decoded with a /tmp probe]

A door opens through a **wall-use record (0x18) on its room face**, whose chain runs 0x2f `cmd_open_door` with key 0
("the face just used"), or through a 0x2f elsewhere keyed to the door's face.

| class | doors | what a use does | example |
|---|---|---|---|
| FREE | 110 | a live 0x18 opens it; many then say DBASE100 527, "Open. Good." | STUDY1 #13: 0x2f (extent 600, sound 139) → 0x2b 527 |
| KEY | 15 | the chain first tests an item **in hand**; without it, the latched line is spoken | STUDY1 #11: 0x36 latch 316 ("Locked.") → 0x27 item 21 (Key) → 0x26 flag 374 → 0x2f |
| LATER | 38 | its opener is disabled until some event re-enables it (0x17), or another chain opens it; meanwhile a use often answers 531, "Stuck fast." / "Won't budge." | STUDY1 #202 (disabled, mod 0x88) and #442 → 0x2b 531 |
| NEVER | 4 | nothing found that opens it (two face a void: fake doors) | GRAVE 263: only "Stuck fast." |

- What the 15 KEY doors test in hand: a "Key" item on 12 (items 20, 21, 129 (three leaves in CHURCH1), 167, 171,
  173, 193, 219, 236, 256); "Coloured Key" (125) and "Flask" (105, with flag 139) on two RAQUIA5 doors; "The
  Shrive" (14) in AELF. Two more doors test keys on chains that start disabled (CHURCH1 491, VICAR 6: class LATER).
  The refusal lines are 316 and 438, both "Locked."
- An **open** leaf is pick type 6: a use toggles it (`toggle_door_open_state`, input.c:1086-1098). A door with a
  sound also gets a 1000-tick open timer (ROTH_COMMANDS.md, `cmd_open_door`) [R: from that document, not measured
  here].
- 42 doors also carry a wall-examine (0x32) record.
- **Correction found on the way.** `ROTH_COMMANDS.md`'s example "A locked door. Check for the key ... item 16" is not
  a door. Item 16 is the **Matchbox**, and that chain is STUDY1 #18, the **candelabra** (section 2).
  `docs/GAME_inventory.md` acceptance test 4 ("Study key ... without item 16") inherits the mistake: the Study's
  locked door is sector 24, and it tests item 21 (Key).

### 1.3 VR: where Adam's hand goes, and how a door opens [P]

**Where.** On the closed door: the painted handle, on whichever side Adam stands, which is always at the latch edge
(1.1). On an open leaf: the same point on the leaf's broad face, which carries the same picture.

**Pose.** The hand closes round a horizontal lever: `fbx_wrap_half` (Blender 820) as the target of the grip's close.
The engine's fist (318) is the fallback until wrap_half is wired.

**Gesture.**
1. The open hand comes within reach of the handle point. The handle zone is the plate plus a margin: proposal, 8 cm
   (about 8 units). That is tighter than phase 1's 15 cm palm radius, so the hand must find the handle, not the door.
   A light haptic tick says "a handle is here".
2. The grip closes on it. The hand is now "on the handle": its pose holds `wrap_half` while the grip stays closed.
3. **Turn or push.** Either the wrist rolls down by 30 degrees or more (a lever turned), or the hand moves 4 cm or more
   along the door's normal, toward the swing (push, or pull from the swing side). Either one fires **one use** on the
   room face, at the handle's texel (0.2, the box). This is the original's click: the chain decides everything that
   follows.
4. Release before turning: nothing happens. That makes it safe to rest a hand on a door.

**The result is the original's.** The door swings by the original's tick, with the original's sound (the 0x2f
record's +0x0c). The hand does not drag the leaf. The leaf's motion is geometry: game state, collision, and later
netplay. The hand model stays with the controller. Proposal: a short haptic "click" when the use fires and a soft
rumble while the leaf swings.

**KEY doors.** The key is an inventory item. In VR it is "in hand" when it sits in that hand's slot (the other lane's
inventory design). Adam holds the key and touches it to the handle plate. The keyhole is not painted separately, so
the plate is the lock. Turning the wrist with the key there fires the use. The chain's 0x27 then passes or refuses
exactly as in the original:

- **refused**: "Locked." The VR feedback is two short dull haptic pulses and no swing (a rattle);
- **carried but not in a hand**: the original auto-selects it into the right hand and does nothing on that click.
  The VR layer shows exactly that (the key appears in the right hand) and does not fire again by itself.

**LATER and NEVER doors.** The use runs, the original's line plays ("Stuck fast." / "Won't budge."), and the haptic
is the same rattle.

**Pull doors.** From the side the leaf swings into, the leaf comes toward Adam. [R] Whether the original's leaf
pushes or blocks the player as it swings was not checked. If it shoves the player, that is the original's and stays.
Proposal: a stronger haptic on the pulling hand as the leaf starts, so the swing toward him does not surprise him.

**Far use.** Pointing at the door and tapping the index stays (phase 1), at the original's 268.

### 1.4 What it needs from the engine

- **New:** per-door handle points. These come from the loader's door data, which already knows c0..c3 and the
  room faces, plus a small table of handle (u, v) per door picture (five pictures cover 142 sides; the 55 others are
  read off once).
- **New:** a "handle" contact zone in the picker, the turn/push detector, and haptic patterns.
- **New:** the use fired at a **point** on the face, with that point's texel coordinates, and the record box test
  (0.2).
- **New:** a result back from the use: did a door swing start, did the chain refuse? This drives the haptic. It is
  read from the runtime after the chain, never predicted.
- **Existing:** door leaves (`Pick::DoorLeaf`, `RegisterDoorLeafSide`), the 0x18 dispatch, the door swing tick.
- **Not built in the port yet:** the 0x2f door sounds' auto-close timer [R: check `roth_runtime.cpp` door tick].

### 1.5 Open questions

1. When the owner pulls a door toward himself from the swing side, should the hand stay on the handle for the first
   few degrees (the hand model leaving the controller by a few cm), or let go at once? Proposal: let go at once.
   Comfort first.
2. Do the 55 rarer door pictures all have a painted handle at the latch? (Read them off when building the table.)
3. Does the leaf in the original push the player or stop against him? (Measure in the oracle before tuning haptics.)

---

## 2. Light sources: candles, candelabras, lamps, torches, fires

### 2.1 What they are [V: kinds looked at on the contact sheets; counts from the census]

| group | kinds (art) | placed | maps | scripts |
|---|---|---|---|---|
| **unlit candles, lit by a click** | DEMO[4098] five-candle floor candelabra; DEMO[4186] tall one-candle stand; DEMO[4124] short candlestick; DEMO1[4185] small candle | **34** | 7 | click 34, examine 21 |
| lit candles (drawn burning) | DEMO[4148] lit five-candle candelabra; DEMO1[4103], DEMO2[4099] lit candelabras; DEMO1[4104], DEMO4[4215], DEMO4[4258], DEMO[4135] candles | 111 | 7 | examine 12 |
| unlit candles, decoration only | DEMO[4117] three-candle candlestick; DEMO1[4131]; DEMO4[4232] | 15 | 4 | none |
| empty candelabra (stubs) | DEMO[4189] | 86 | 5 | examine 86: DBASE100 123, "Candle. Tallow I believe." |
| torches, braziers, flames | 16 kinds: DEMO4[4272] wall torch (148), DEMO1[4127] (136), DEMO[4130] (81), DEMO2[4098], DEMO3[4098], DEMO4[4212], DEMO1[4187], DEMO4[4257], DEMO4[4198], DEMO4[4201], DEMO4[4255], DEMO3[4132], DEMO4[4200], DEMO2[4100], DEMO4[4202], DEMO4[4205] | ≈565 | 25 | examine ≈190; DEMO4[4201] has a click chain (GRAVE's brazier: DOOR, items 0/84/172/220) |
| lamps, lanterns, chandelier | DEMO[4112], DEMO4[4112], DEMO1[4182] hanging globes; DEMO1[4119] hanging lantern; DEMO1[4102], DEMO1[4122], DEMO[4118] lamp posts; DEMO4[4262], DEMO4[4263], DEMO1[4188] ceiling shades; DEMO1[4145] floor lamp; DEMO1[4147] desk lamp; DEMO[4115] chandelier (mesh) | 147 | 13 | examine 2 |
| open fires | DEMO[4131] and DEMO1[4129] (meshes); DEMO3[4103]; DEMO4[4199] pit; DEMO1[4196] (a burning book trap); ADEMO[279] (hidden) | 27 | 6 | click 2 |

[R] The group boundaries are my reading of the pictures. Two torch-like kinds carry the render id of item 175
"Magical Torch" (DEMO1[4127], DEMO[4127]). They are not pickups: the art word is below 0x200, so
`give_item_by_dbase_id` refuses it (inventory.c:351), and they have no click record.

### 2.2 What the original does: candles ARE lit by the player [V: chain dumps]

All 34 unlit candles are lit by a **use**. The chain is the same everywhere, with variations:

```
STUDY1 #18 (0x08, key 2: the two candelabras in sector 217, and two in 252)
  0x27 item 16 (Matchbox) in hand, flags 0x03      <- 16 of the 34 test the matchbox
  0x3a change object id -> 0xffff                   (it can no longer be clicked)
  0x12 delay 30 ticks                               (≈ 0.43 s at the 70 Hz heartbeat [R: if the delay counts heartbeat ticks])
  0x3a change object id -> 15                       (it now examines as DEMO[4189]'s record: "Candle. Tallow I believe.")
  0x0d change object texture -> 0x0034 = DEMO[4148] (the LIT candelabra, all five flames at once)
  0x1d change lighting, sector group 3, +6           (the room brightens: the original's "light")
  trigger's sound: 0x0103 - 1                       (presumably the match strike)
```

| where | kind | records | item tested | what else |
|---|---|---|---|---|
| STUDY1 (6) | DEMO[4098] | #18, #212 | Matchbox (16) | lights groups 3 and 22 |
| STUDY1 (2) | DEMO[4186] | #4 | Matchbox | **0x15 count to 2**: when both are lit, record 9 (0x07, key 11) moves the ceiling of the sector with id 11 (the corridor painting rolls up: the note in roth_raw.h), group 46's light rises and falls three times, and 0x3c spawns object 64 |
| STUDY4 (3) | DEMO[4098] | #59, #67 | Matchbox | lights group 10 |
| STUDY4 (5) | DEMO[4124] on the floor | #29 | Matchbox | **0x15 count to 5**: the ritual. After the fifth: DBASE100 records 452 and 565, group 9's light drops (−10), the player is turned (0x3f), two sound nodes, a delay, then 0x16 wakes object 6 |
| STUDY2 (4), STUDY3 (6), CAVERNS (4), TEMPLE1 (3) | DEMO[4098], DEMO[4124] | #153, #183, #124, #67, #73 | none | light change (TEMPLE1's: art only, no light) |
| VICAR1 (1) | DEMO1[4185] | — | — | LIGHT, OBJTEX |

- **The original lights every flame of a candelabra at once**: one click, one art swap after the delay.
- **Torches, lamps and fires are never lit or put out by the player.** None of their kinds has a click record; only
  GRAVE's brazier DEMO4[4201] and two fires have one, and those are puzzles (items, a door).
- The Lamp (item 15) is an inventory item with render id 0: it is never placed in a level [V: names probe]. No
  record tests it [V: itemrefs, no 0x27 on 15].

### 2.3 VR: lighting a candle [P]

**Where Adam's hands go (CORRECTED 2026-11-13).** The original's hands decide it: the item is in the RIGHT hand
(`g_selected_item_primary`), the weapon in the LEFT (`g_selected_item_secondary`, docs/GAME_inventory.md). So the
matchbox is in the right hand (pose `fbx_wrap_half`, copied onto the right hand), and the use comes from that hand:
touching the candle with it, or a grip while pointing at it (the owner's trigger/grip scheme). Verified on the owner's
PC (`docs/images/hands/engine_useitem_flat_ad6d1ec5.png`): the left hand's grip auto-selects the matchbox into the
right hand and does nothing else, as the original; the right hand's grip lights the candelabrum.

- **Striking.** The main hand brushes across the box's side, the striker: a stroke of 4 cm or more along the box's
  long axis, at a speed of 0.3 m/s or more. A match is then lit **in the main hand** for 20 s [ADD for the match
  object itself: the original has no match in hand]. Pose of the main hand while it holds the match: `fbx_cup`
  until a pinch exists. The flame is a small particle and an `AddFrameLight` point [ADD].
- **The wick.** The flame tip (main hand) comes within 3 cm of a wick → **one use** on that candle object. That is
  the original's click: the chain tests item 16 in hand (the matchbox in the off hand passes), re-keys the object,
  waits, swaps the art, ramps the room.

  Where the candles are: the five-candle candelabra's wicks are at the top of a 164-unit (1.88 m) picture, reached
  overhead. The STUDY4 floor candles' wicks are 53 units (0.61 m) up: Adam kneels in the circle, as the scene wants.
- **Without the match.** If the matchbox is carried but not in a hand, the touch does what the original's click
  does: the box is auto-selected into the RIGHT hand and nothing else happens. The VR layer shows it arriving in
  the right hand. A second touch then lights.
- **The candles that need no matchbox** (STUDY2, STUDY3, CAVERNS, TEMPLE1): any fingertip or palm touch on the wick
  zone fires the use (phase 1's near use), and the original's chain lights them. [R] Whether the original meant
  these to need a light was not asked of the data. The original does not test one, so neither does VR.

**"Light each candle one at a time, in any order" (the owner's idea, 2026-11-13).** The original lights the five
flames of one candelabra with **one** use and **one** art swap. Per-flame lighting is presentation; the game state is
the original's one click:

- The **first** wick touched fires the original's use at once (the chain's re-key, its delay, its art swap, its +6 on
  the room's light group, any 0x15 count such as STUDY4's five-candle ritual).
- Each wick then shows lit only when the player lights it, in any order: per-flame compositing from the original's own
  lit picture. The flame positions are measured, not hand-marked: `tools/wickspots` diffs each unlit picture against
  its lit partner (`docs/OPENABLES.md` section 6; five-candle DEMO[4098]->DEMO[4148] wicks at
  (-23,141) (-13,154) (0,159) (+13,154) (+23,141) units).
- **Catch (measured):** the lit picture also recolours each candle's top under the flame and the cup and drip-pan
  rims, plus a few stem texels. Per-flame compositing must carry those texels with their flame (assign by column) or
  the candelabrum shows orange rims over dark wicks.
- Presentation may be better but must end up showing the same thing as the original, so wicks left unlit catch by
  themselves after a few seconds [owner to confirm the delay].
- **One light per candelabrum** (owner's decision): one soft, flickering dynamic light that grows with each lit wick,
  and stays (the original never puts a candle out).

**Darkness (owner, 2026-11-13: "each one adding a little more light to the sector"; see RS_Radiance's Darkness).**
This engine already carries RS_Radiance's darkness pass (`Level.SetDarkness` / `SetDarknessSpace`, `DarknessAt` in
main.fp). Measured in the shader: the darkness multiplies the sector's own light BEFORE dynamic lights are added
(main.fp ~3986 vs ProcessMaterialLight), so a candle's light is never darkened. Plan [P]:
- the curve takes the original's sector light as its input, so the original's lighting design stays the shape; plus a
  distance falloff like Radiance's Lantern preset (full dark at ~576 units);
- the first wick's original +6 on the light group lifts the room through the curve; each lit wick adds to the
  candelabrum's light, so the room brightens candle by candle;
- the held Lamp's light follows the hand (owner's decision);
- monsters' "actor spare" (Radiance default 0.4) is a tuning number for the headset;
- CONSTRAINT: Radiance keeps its settings in saved cvars; the owner's doomxr.ini is shared with the hands build, so
  VRealms sets the darkness from game code per map / room, never through cvars;
- FIRST CHECK: the darkness pass over `roth_palette_shading 1`, captured lit and dark, before any tuning.

**Feedback.** The match strike: a haptic scratch (three quick pulses) and the original's chain sound, which the
trigger record already plays (+0x0a − 1 = 258). The catch: a soft pulse when the use fires. The light: the room's
0x1d ramp, the original's.

**The candle-merge lights [ADD]** (0.5): each lit flame is a small `AddFrameLight` point; a candelabra's flames merge
into one light as they catch; and the cluster's light stays until the map ends (the original never puts a candle
out). Off by default, behind its own switch, and only after the palette-shading question in 0.5 is answered. The
111 candles already drawn lit could get the same lights [ADD]. That is the cosmetic polish the owner decides on;
nothing here turns it on.

### 2.4 VR: torches, lamps, fires, and the lantern [P / ADD]

- **Nothing to light.** The original never lets the player light, douse or take a torch, lamp or fire. They stay
  examinable (point and pull the trigger) and otherwise untouched.
- **Hand near a flame [ADD]**: warmth as a faint haptic hum within 10 cm of a torch or fire flame. No damage: the
  original has none for that, except where a record says so (the CHURCH1 burning book DEMO1[4196], 0x33 HURT, is the
  original's own).
- **The lantern cone from the hand [P, an adaptation of the original's own lighting]**: in lights-out sectors the
  original shades by a flickering cone around the **screen centre** (0.5). In VR the screen centre is where the head
  points. The cone can instead follow **the hand that holds a light**: the matchbox with a lit match, or a later
  hand-held lamp. The same falloff, jitter and constants are used, measured about the hand's ray instead of the view
  axis; with no light in a hand, it stays on the head, exactly the original. Cost: no light at all, a change of axis
  in `main.vp`'s lantern term. It is presentation: the original ties the cone to no item, so the cone never goes
  dark because a hand is empty. [R] Showing the original's cone from a different origin is an adaptation the owner
  must approve, since "where the original shows nothing, we show nothing" is not at stake but "1:1 first" is.

### 2.5 Needs

- **New:** wick points per candle kind (from the picture: the candle tops; the five on DEMO[4098], one on DEMO[4186]
  and DEMO[4124]).
- **New:** match strike detection, the match object and its 20 s life [ADD]; per-flame presentation during the
  delay [ADD]; the cluster-merge light system on `AddFrameLight` [ADD].
- **New:** `main.vp` lantern axis from a hand [P].
- **Missing authored pose:** a pinch (thumb and index), for the match and the wick. `fbx_cup` stands in.
- **Existing:** 0x27 select test, 0x3a, 0x12, 0x0d, 0x1d, 0x15 in the runtime (ROTH_COMMANDS.md tick table:
  0x0d and 0x1d done).

### 2.6 Open questions

1. Per-flame lighting during the delay [ADD]: does the owner want it, given that the original shows all five at
   once after 0.43 s?
2. The lantern cone from the hand: yes or no?
3. Effect lights over palette shading: must be checked in the engine before any [ADD] light is built.

---

## 3. Switches and levers

### 3.1 What they are [V: census -switches; pictures looked at]

**Light switches are objects, not walls.** Each is a brass plate with a toggle, a fixed-angle picture (renderType
0x80) on a wall, 19 × 19 units, 102–120 units up (1.17–1.38 m). **39 placements, 31 records, 9 maps** (CAVERNS3,
CHURCH1, STUDY1–4, TEMPLE1, VICAR, VICAR1). Kinds: DEMO[4185] (12, Study), DEMO[4184] (1, its other position),
DEMO1[4181] and DEMO1[4180] (Church, Vicarage), DEMO4[4261] (Caverns).

- A click record with **base 0x02** (`cmd_light_switch`, raw_commands.c:4331): it ramps the connected group named
  at +0x0a by the step byte at +0x07 (read as −6 to −20; 30 to 50 in CAVERNS3), negated unless the direction bit
  (+0x06 & 8) is set, flips its direction and toggle bits, plays
  word +0x0e − 1 (115, the click), and **XORs the object's art word with 1**. So the plate shows its neighbouring
  picture: the toggle drawn the other way (4185 ↔ 4184) (raw_commands.c:2954-2959).
- Two DEMO[4185] plates in STUDY1 (id 13, #585) are base-8 chains that set flag 421 instead. They look the same.
- **The original's weapon cannot work them**: no attack-hit (0x1b) record is keyed to any switch [V: census, hit 0
  on every switch kind].

**Levers and wall mechanisms (objects)** [V: chain dumps]:

| kind | where | what a use does |
|---|---|---|
| DEMO4[4280] lever in a wall box (6) | MAS4, MAUSO1EA | MAS4 #3: opens door 2 (0x2f, extent 1100), a sound node, art swapped to the thrown lever (0x0d) |
| DEMO[4178] lever (1) | TEMPLE1 | #9: art swap, opens doors 7 and 2 |
| DEMO4[4246] lever with a lock (1) | MAUSO1EA | tests item 22 (Key); refuses "Won't budge. / Locked." (264) |
| DEMO1[4133] (3) | RAQUIA5 | art swap, flag, sound |
| DEMO3[4120]–[4123] arrow buttons (13), DEMO3[4116] and DEMO4[4288] hand plates (9), DEMO3[4128] (11), DEMO3[4115], DEMO3[4126], DEMO3[4130] | SALVAT, MAS6 | the Salvation puzzles: geometry moves (MOVE, OBJMOVE), lights, wall textures |

**Wall triggers that are not doors**: 84 of the 240 0x18 records. Their pictures (`world_census -faces`, contact
sheet `walls_*.png`) include the STUDY3 hand plate DEMO[684], SALVAT's buttons DEMO3[186] (13) and DEMO3[621] (8),
sockets that take an item (MAS3/MAUSO1EA DEMO4[2353] and [2366] take the Orb, item 18; DOMINION DEMO3[180] takes the
Brain, 223; AQUA1 DEMO1[634] tests 6 and 131; STUDY2 DEMO[750] tests 49), DOMINION's vine panels DEMO3[182]/[183],
CAVERNS3's green symbol panels DEMO4[2335]–[2346] (12, one record each), and the grandfather clock DEMO[726]
(a door, flag-gated).

**Attack-hits walls (0x1a), 72 records**: mostly **glass** — windows DEMO[716], [718], [742], [738], [744], [746],
DEMO1[876]/[882], whose chains swap to a broken picture, burst particles and hurt (0x33) — plus DOPPLE/SALVAT's
DEMO3[145] (20) and the CHURCH1/VICAR doors that a hit opens. [V: census face table]

### 3.2 VR: a light switch [P]

- **Where.** The plate's toggle, at 1.2–1.4 m on the wall.
- **Pose.** Point (the engine's point pose, index R 808). The index tip presses the toggle: phase 1's fingertip press,
  within 2.5 cm, fires one use. The original's chain does the rest: ramp, sound, art flip.
- **Slap or punch (the owner's "melee hits on switches").** An open or closed hand that strikes the plate at 1.2 m/s
  or more (controller speed along the wall's normal) fires **one use**, not an attack. The original's attack on a
  switch does nothing (no 0x1b record), so a strike that fired an attack would do nothing. Mapping the strike to
  **use** is an input choice, and the result is the original's switch. It works only where a use would work (cursor
  type 3, within the reach). Re-arms when the hand has moved 10 cm away.
- **With a weapon in that hand.** The weapon's swing stays the original's attack, and the attack paths (0x1a/0x1b)
  are what it can do. Proposal: a weapon strike on a switch does **not** count as use. A sword that flicks lights on
  and off would be a new mechanic. Open question for the owner.
- **Feedback.** The original's switch sound, a crisp haptic tick, and the plate's picture flips. A 3D toggle that
  visibly moves would be [ADD] cosmetic polish, for the owner to ask for; the art swap is the original's feedback.

### 3.3 VR: levers [P]

- **Where.** The lever's handle, at the top of the picture's lever.
- **Pose.** `fbx_wrap_tight` (Blender 837) closed on the handle, the fist (318) as fallback.
- **Gesture.** Grip on the handle, then pull down: 8 cm or more of hand travel along the picture's lever direction
  (down in all these pictures) fires **one use**. The art swap to the thrown picture is the original's.
- **Locked lever** (DEMO4[4246], item 22): the key in that hand and a turn, as for a door; the refusal line and the
  rattle haptic as for a locked door.
- **Feedback.** The original's sound (the trigger's +0x0a), a heavy haptic thunk at the fire, and the original's door
  or mechanism moving.

### 3.4 VR: buttons, plates and sockets [P]

- Buttons and hand plates (SALVAT, STUDY3's hand plate, MAS6): **press with the palm**, open pose (frame 0), the palm's
  centre within 3 cm of the plate's centre and pushed 2 cm in. One use. For the STUDY3 hand plate this is the
  gesture the picture asks for.
- Sockets that take an item: the item is in hand (0x27). Adam holds it and **places it into the socket**: the held
  item within 5 cm of the socket's centre and the grip released fires the use, and the chain takes the item (0x2a)
  as the original does. Pose: whatever pose holds that item (ITEM_DESIGN.md). If the chain refuses, the item stays
  in the hand: the original never drops anything into the world.
- CAVERNS3's symbol panels and DOMINION's vine panels: fingertip touch, as a switch.

### 3.5 VR: glass [P]

The original breaks windows with the weapon (0x1a). In VR the swing of a held melee weapon or a fired shot does it,
through the original's attack path. A bare-hand punch on glass stays nothing, as the original has no bare-hand
attack. [R: whether Adam's fists are a weapon in the original was not checked; see GAME_combat.md before building.]

### 3.6 Needs

- **New:** hand velocity (OpenXR gives linear velocity per controller; or the difference of two frames) and a strike
  detector against object boxes and wall faces; contact points per switch and lever kind; pull detector; socket
  placement.
- **Existing:** the base-2 click and the art XOR (`roth::ActivateObject`, ROTH_COMMANDS tick 0x02 "done").

### 3.7 Open questions

1. May a held weapon's strike count as use on a switch, or only an empty hand?
2. Strike speed and re-arm distance: tune in the headset.

---

## 4. Pickups as they lie in the world

### 4.1 What they are [V: census; give_item_by_dbase_id read, inventory.c:340-368]

Two kinds of taking exist in the original:

1. **Plain pickup: 48 placements.** A use reaches `give_item_by_dbase_id` only when the object has no live click
   record, none of renderType 0x01/0x02/0x10, and an art word **≥ 0x200**. Only ADEMO art (sources 2 and 3) can be
   picked up without a script; `give_item_by_dbase_id` refuses any lower word (inventory.c:351). Examples: the
   Matchbox (ADEMO[152], hidden at load until a script shows it, in STUDY1 sector 271 at 120 units up), Statuettes (11), Orbs (12),
   gems, coins, the Ace of Spades.
2. **Taken by its click chain: 269 placements** whose chain gives an item (0x29): Colt-45 magazines (45), cartridges
   (42), vials, the Brain (16), keys, parchments, the six GRAVE newspapers, and the AQUA1 alchemy set. Most of the
   game's pickups are this kind. [R: "gives an item" includes chains that give for another reason; it is an upper
   bound on pickups.]

**Where they lie** (z above their own floor, the 317 placements of both kinds):

| on the floor (0) | 1–39 | 40–99 (low tables, shelves) | 100–159 (desks) | 160–239 (high shelves) | 240 and up |
|---|---|---|---|---|---|
| **219** | 3 | 42 | 15 | 19 | 19 |

So 69% lie on the floor. They are billboards: camera-facing pictures, mostly 10–50 units across (0.1–0.5 m), drawn
standing up on the spot where they lie. Some are fixed-angle quads laid on a surface (the Matchbox, rt 0x80).

### 4.2 VR: taking one [P]

- **Within arm's reach: grab.** The open hand comes to it, and the grip closes with the palm within the item's box +
  5 cm → **one use** (phase 1's grip squeeze). The original's pickup or chain then runs. The original flies a
  picked-up item into the inventory (`destroy_dynamic_entity` to the pickup-fly scratch). In VR the item flies the
  few cm into the closing hand, then into the inventory as the original's fly does. The same thing happens, from
  where Adam's hand is.
- **On the floor.** Standing players crouch or bend: real floor height is the game's floor (the eye anchor puts the
  feet on it). Seated players: the far grab below. No auto-crouch, no floor offset.
- **Beyond arm's reach, within the original's reach: a DistanceGrab-like pull.** Point at it (the ray, as phase 1),
  close the grip and flick the hand back (≥ 0.8 m/s toward the body) → one use. This is the original's own reach:
  the original picks up by clicking anything within z < 268 (2.7 m), so a pull from up to 2.7 m is 1:1 with it, and
  nothing beyond 268 may be pulled. The flight to the hand is presentation of the original's pickup fly.
- **Pose.** Small things (keys, coins, gems, the matchbox, vials, magazines, parchment): `fbx_cup` until a pinch
  exists. Larger things (statuette, helm, brain, staff, guns): `fbx_wrap_half` while the item flies in; once it is in
  the inventory, the hand is empty again, and what it holds next is ITEM_DESIGN.md's.
- **Shown as what it is.** A billboard in a 3D hand is wrong: it turns to face the eye. As it lies, a billboard on a
  table is the original's picture and stays one. When the voxelizer lands (another lane: `tools/voxelize_test` in
  the wt_voxel worktree, not on the branch), the item could lie as its voxel, made from the same picture [ADD:
  presentation of the original's own picture in 3D; the owner decides].
- **Refusals** are the original's: a full pack (count 256) makes 0x29 fail and interrupt (GAME_inventory.md test 6);
  the item stays where it lies.
- **Feedback.** A soft haptic on contact, a firm one when the use fires, and the original's pickup sound if the
  chain has one.

### 4.3 Needs

- **New:** the pull gesture (flick detection), the fly-to-hand path (the original's fly retargeted), the
  horizontal-distance depth rule (0.2) for the pull's reach.
- **Missing authored pose:** pinch.
- **Existing:** phase 1's grip squeeze; `give_item_by_dbase_id` in the port's use path.

### 4.4 Open questions

1. The voxel as it lies on a table, or only once in the hand?
2. Seated play: is the pull enough for floor items, or does the owner want an explicit "reach to the floor" assist?

---

## 5. Books, shelves and things to read

### 5.1 What they are [V: pictures looked at; census]

- **Bookshelves are wall pictures**: DEMO[254] (the Study bookshelf, a wall-use record that opens a door: the secret
  bookcase door) and CHURCH1's DEMO1[596]/[597] (examine only). There are no separate book objects on shelves.
- **Books and papers lying about are pickups (section 4)**: the Book in STUDY1 (ADEMO[230], item 17, on a table at
  74), CHURCH1's Books ADEMO[81]/[82] (items 139, 138, at 186), the Parchments in STUDY2 (ADEMO[94]/[97]/[98], items
  40/39/42) and CAVERNS3 (ADEMO[150], 13), the Letter in STUDY2 (ADEMO[151], 11), and the six newspaper pages in
  GRAVE (DEMO4[4300]–[4305], click chains that give). Reading them is the inventory's document pages (40 items carry
  them: ITEM_DESIGN.md).
- **Notices on walls**: the CHURCH1 notice board DEMO1[139] and others, examine only (0x32).
- **A trap**: CHURCH1's Book DEMO1[4197] with the burning DEMO1[4196] beside it. Its click chain hurts (0x33),
  moves an object and bursts particles.

### 5.2 VR [P]

- **Bookshelf door (DEMO[254])**: the shelf has no handle. The contact zone is the latch half of the face. Gesture:
  palm flat on the books and a push of 4 cm or more, or a grip on the shelf's edge and a pull. One use; the original
  swings it.
- **Notices, inscriptions, stained glass, shelves**: point and pull the trigger = examine. The original's line plays.
- **Books and papers lying about**: taken as pickups (section 4). Reading them is the inventory's (ITEM_DESIGN.md).

### 5.3 Open question

Should a bookshelf-door face also respond to a book "pulled" (a grip on a specific book's spine)? It would be [ADD]:
the original's record has one box, the whole face or a part of it. If the box is a part (0.2), that part is where
the hand must be, and that may already be the "right book".

---

## 6. Furniture and meshes worth touching

### 6.1 What they are [V: rendered with proprender, /tmp/wc/sheets/meshes.png]

33 mesh kinds. Trees (DEMO1[4115], [4099], [4101], DEMO4[4128], ...; ~550 placements outdoors), and in the Study and
elsewhere:

| kind | what | placed | scripts |
|---|---|---|---|
| DEMO[4109] | high-backed chair | 35 | examine 33 |
| DEMO[4108] | throne-like armchair | 15 | examine 15 |
| DEMO[4111] | table | 9 | examine 2 |
| DEMO[4106] | sofa | 2 | examine 2 |
| DEMO[4113] | desk | 1 | examine 1 |
| DEMO[4152] | a chest in STUDY1 sector 109: five meshes, four hidden | 5 | click #88: the five are shown in turn 30 ticks apart (0x16), a sound node, then **door 2 opens elsewhere** and Adam says 94 ("What was that? Sounds like you just opened something.") |
| DEMO[4150] | crate | 24 | click 24, STUDY3: 656, "This isn't gonna move in a hurry. / Won't budge." |
| DEMO[4115] | chandelier | 3 | none |
| DEMO1[4118], DEMO2[4139] | waterfalls | 12 | the chalice is filled there (items 156, 127) |
| DEMO1[4100], DEMO2[4097], DEMO[4116] | green domes: portals | 4 | each tests an item (97 the Dragon Sword, 60 the Mask, 35 the Crystal), then a map transition (0x3b) |
| DEMO[4131], DEMO1[4129] | fires | 4 | examine |

### 6.2 VR [P]

- **Chairs, tables, desks, sofa**: examine by the trigger; no use in the original. The hand rests on them, and they
  **stop the hand**: the hand model stops at the mesh's surface instead of passing through [ADD: presentation;
  nothing moves]. Proposal: off until the owner sees it, since a stopped hand that drifts from the controller can be
  uncomfortable.
- **The chest (DEMO[4152])**: the original's click plays a five-step mesh sequence. Gesture: both hands under the lid's
  front edge, or one hand's grip on it, then lift 6 cm → one use. Pose `fbx_wrap_half`. The sequence and the door
  elsewhere are the original's.
- **Crates (STUDY3)**: palm push (open pose, 4 cm along the crate's face) → one use → "Won't budge.". The original
  never moves them, so neither does VR.
- **Waterfalls**: the chalice held in a hand and put into the falling water (the held item within the mesh's box)
  → one use. The chain's item test and its swap of the chalice are the original's.
- **Portals (green domes)**: walking into them is the original's map transition through its click chain, so in VR the
  hand **touches** the dome (palm) → one use, with the item test as the original.

### 6.3 Needs

- **New:** mesh boxes for contact (the mesh extents are known: propcheck prints them), the lift gesture, item-in-volume
  for the waterfalls.
- **Existing:** meshes spawned as models (roth_objects.cpp), the click and examine channels.

---

## 7. Everything else scripted, by count

Object kinds with any script, other than sections 1–6, most placements first [V: census; names are my reading of
the pictures]. "exam" means point-and-trigger is the whole interaction in VR, unless said otherwise.

| kind | what (looked at) | placed | scripts | VR [P] |
|---|---|---|---|---|
| DEMO4[4272], DEMO[4130] | wall torches | 229 | examine 102 | the trigger (section 2) |
| DEMO1[4143] | glowing crystal (AQUA1, RAQUIA5) | 69 | **walk-into 52** | walking into it is the original's 0x39; the hand passing through it is nothing [R: 0x39 is player-body contact] |
| DEMO[4105] | suit of armour | 47 | examine 48, click 3 (TALK) | the trigger; the 3 that speak: the grip in reach = use |
| DEMO1[4233], DEMO1[4232] | breastplate / armour on a stand (CHURCH1, VICAR) | 31 | click, examine, **attack-hit** 23 (SPAWN) | use by palm; hits by weapon (the original's 0x1b) |
| DEMO4[4198], DEMO4[4257], DEMO4[4126], DEMO4[4277], DEMO4[4278], DEMO4[4226] | tall braziers, statues, shields, crossed swords, chains, bones (AELF, MAS*, MAUSO*) | ~150 | examine | the trigger |
| DEMO[4175], DEMO[4176] | window bars, STUDY1 | 35 | click 35: 34, "Stuck fast. Can't see how though." | grip a bar, pull → use; the line plays |
| ADEMO[114], ADEMO[115] | long blades on the wall, as drawn (STUDY1, MAS4) | 23 | click 18, TALK | grip the hilt → use; the original speaks, nothing is taken |
| DEMO[4207], DEMO[4208] | pentagram plaques (STUDY1) | 9 | click: 530, "This one's stuck fast." | fingertip or palm → use |
| DEMO[4173], DEMO[4174] | round wall dials (STUDY1) | 7 | click: 35, "Purely decorative." | grip and turn (wrist roll) → use |
| DEMO[4201], DEMO[4202] | picture frames (STUDY1) | 20 | examine | the trigger |
| creatures (ADEMO[269], [225], [213], [223], [207], [266], ...) | monsters, NPCs | ~200 | attack-hit and examine | combat and dialogue lanes, not this file |
| ADEMO[43] | an invisible marker (no picture) | 64 | hit 2, examine 1 | nothing to touch [R] |

The full list is `world_census -objects` (237 kinds with any script).

---

## 8. What the engine needs, all kinds together

| need | for | new or existing |
|---|---|---|
| use and examine fired at a **point** on a face or object, with the original's texel coordinates, and the 0x18/0x19/0x32 **box** test | doors, switches, plates, bookshelf | new (the port skips the box: roth_runtime.cpp:471) |
| the planar horizontal **depth rule** and the measured use range 268 | every far and near use | new (DepthOf is 3D; the range is the provisional 600) |
| contact points per kind: door handles (u, v per door picture), wicks, switch toggles, lever handles, plate centres | sections 1–4 | new data; the door table comes from the loader's door data + 5 pictures for 142 sides |
| gesture detectors: turn (wrist roll), push/pull along a normal, strike (hand speed), lift, flick-pull, place-in-volume | sections 1–6 | new |
| hand linear velocity | strikes, flicks | new (OpenXR provides it) |
| the result of a use: door swing started, chain refused, item given | haptics | new (read back from the runtime after the chain) |
| authored hand poses: wrap_half, wrap_tight, cup wired as targets; a **pinch** frame | all | wiring new; pinch missing from the FBX |
| haptic patterns (tick, click, thunk, rattle, scratch) | all | `VR_ScriptHaptic` exists |
| pickup fly retargeted to the hand | section 4 | new presentation of the original's fly |
| [ADD] effect-light candles with the cluster merge, the match flame, flame warmth | section 2 | `AddFrameLight` exists; the cluster step is new |
| [P] the lantern cone's axis from a hand | section 2 | a change in `main.vp` |

## 9. Open questions for the owner, in one place

1. Doors: let go of the handle at once when the leaf starts to swing (proposed), or let the hand ride it a few degrees?
2. Candles: per-flame lighting inside the original's 0.43 s delay [ADD], yes or no?
3. The lantern cone following the hand that holds a light, yes or no?
4. May a held weapon's strike work a light switch, or only an empty hand?
5. Pickups lying as their voxel [ADD], or only once in hand?
6. Seated play and floor items: is the pull (within the original's 2.7 m) enough?
7. Furniture that stops the hand [ADD]: wanted?
8. A pinch pose for the FBX: can the owner add one (match, key, coin, wick)?

## 10. Corrections to other documents found while doing this

- `ROTH_COMMANDS.md`, the example "A locked door. Check for the key ... item 16": it is STUDY1 #18, the **candelabra**
  lit with the Matchbox (item 16). [V: chain dump; item 16 = "Matchbox"]
- `docs/GAME_inventory.md` acceptance test 4 ("Study key ... click the locked door without item 16"): the Study's
  locked door is sector 24, opened by #11, which tests **item 21 (Key)** and refuses with 316 "Locked.". The test
  should name item 21. [V: census -doors, chain dump]
- `src/roth/roth_runtime.cpp:471` describes the 0x18/0x19 box as "the player's x/z"; MEASURED 13 found the renderer
  writes the **hit texel** there. The VR contact point should be converted to texel coordinates, not the player's
  position.

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
