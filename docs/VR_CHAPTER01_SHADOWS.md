# VR design dossier, chapter 1 "Shadows" (STUDY1, the front of the house)

2026-11-13. First chapter dossier for the design discussion with the owner. Design only: nothing here is built, and
nothing here was seen in the headset or in the running original.

**Marks.** **[V: how]** verified, and how (a tool run on the retail install, a record or ROTH.C line read, a picture
looked at). **[R]** reasoning only, not seen. **[P]** a design proposal. Every proposal ends in exactly one of the
original's clicks (examine, use, attack, the inventory's own clicks) and adds no outcome, item or text.
**Tags.** **A** = drives only the original's screens and clicks through paths that exist (laser, trigger, grip, the
built grab and touch); **B** = a VR presentation or a new gesture layered on top, which waits for the owner's go.
**Effort** S = days, M = one to two weeks, L = more [R: estimates].

Built and verified on the owner's PC before this file (HANDOFF_LANE_GAMELAYER_2026-11-13_EVENING.md 1, not re-checked
here): FBX hands with authored poses; trigger = examine, grip = use (pointing at range < 268 planar units, in reach =
grab); grip on nothing = the original's attack; the held item is the original's right-hand item, shown in the right
hand, used by touching or grabbing the target with that hand; the inventory is the original's panel and pauses the
world; a click during a line skips it; the darkness pass (RS_Radiance) is in the engine; per-wick candles with one
growing light per candelabrum are being built.

---

## 0. How this was made, and the scale

**The area.** The census tools do not model space (CHAPTERS.md 7), so this file adds one mode to `tools/openables`:

```
sh tools/openables/build.sh
/tmp/openables "$ROTH_DATA" -area STUDY1 -stop 29 -have 1,15,16,68 [-poly /tmp/plan.txt] [-dump /tmp/art]
python3 -I tools/openables/plan.py /tmp/plan.txt /tmp/plan.png [x0,x1,y0,y1] [scale]     # plan view, /tmp only
```

It walks from the map's start the way the player's body can (the original's wall-flag rule for the player's sweep,
`collide_point_walls_recursive` collision_physics.c:705-735 with the mask 0x81 set at :1706; MaxClimb 65, MinFit 96 from
the map metadata), crosses a door only where a live use record opens it from that side (with the items in reach),
stops at the sector with command id 29 (the Study: card 2, STUDY1 #299), and lists every scripted record on what it
reaches, plus every sector one or two portals away (desk tops, niches, window bays: the original clicks things up to
268 units off). The rules and their approximations are the comment above `AreaMode` in `tools/openables/openables.cpp`.
Flags are not evaluated and 0x17 enables are not followed; both are checked by hand below.
[V: run this session: 114 sectors walked (112 room sectors and the 2 door sectors s30, s44), 34 more touchable; the
walk falls into 2 rooms joined only by those two doors.]

Also used: `tools/chapter_census -map STUDY1` (the gated units), `tools/world_census` (`-doors`, `-switches`, `-chain`,
`-pic`), `tools/openables -map STUDY1` (full chains, boxes), `tools/wickspots` (wick points), `tools/propcheck -mesh`
(mesh sizes), `tools/item_dossier` and `tools/dbase100_test/dbase100_dump` (items and action lines),
`tools/interact_census` (STUDY1: 274 objects, 223 lantern sectors in the file).

**The scale** [V: rothmap.cpp:587-608 read; R: not judged in the headset]. The headset's metre is set from the
original's eye: 144 units above the feet, the world stretched vertically by 1.1462. With the player's eye at E metres:
height `h_m = units × E / 144`, horizontal `d_m = units × E / 165`. Every metre below is for **E = 1.65 m**:
**87.3 units = 1 m up, 100 units = 1 m across**. The use reach (268 planar units) is **2.7 m**; examine reaches 24.7 m.

**Contact sheets** (retail art, /tmp only, never committed): `/tmp/ch01_sheet1.png`, `/tmp/ch01_sheet2.png`, one
cell per entry, labelled with the entry numbers below. Plan views: `/tmp/ch01work/plan_*.png`.

---

## 1. What chapter 1 is

**Where.** STUDY1 only. Chapter 1 ends when Adam walks into the Study (sector id 29, `#299` card 2) [V: CHAPTERS.md
1.1; chain]. The area reachable before that is the front of the house [V: `-area`]:

| area | sectors (main) | floor | what is there |
|---|---|---|---|
| Entrance hall (start) | s267 (start), s484, s279, s482-488, s59-s71, s287/s288/s412 (table) | 0 | front door, coatstand, armour, wards, the dead switch, paintings, a mirror |
| East passage | s262-s265, s489, s490, s49, s50, s76-s78 | 0 | empty candelabra, chairs, windows |
| West corridor | s337, s338, s72 | 0 | wards, two stuck doors |
| Lounge | light group 10: s34-s46, s54, s55, s192, s491-s499; desk s305/s321/s322; typewriter s306, s313-s316 | 0 | typewriter, Colt, scrapbook, the one live switch, sofa |
| South corridor and east room | s26, s27, s29, s31, s32, s255-s258, s500-s506 | 0 | wards, rat, boot prints, the open window, a locked door |
| Stairs | s8 .. s1, s19 .. s17 | 0 → 400 | (the window walk-in) |
| Landing | s9, s20, s25, s28, s209-s211, s404, s405 | 400 | the candelabra pair, the Study door |
| Candle corridor | s368, s56 / s57 / s58 (the painting niche) | 400 (niche 500) | the two tall candles, the painting, the key |
| Upper stair and gallery | s10 .. s15, s16 (mirror niche) | 440 → 640 | armour, a chair, a mirror |

**The critical path** [V: area walk + chains; R: that this is the order a player meets it]: start in the hall → the
east passage → the lounge (its walk-in starts the typewriter) → a lounge door, `#13`, the only way to the south
corridor [V: the walk splits into two rooms joined only by s30 and s44] → the stairs (the casement windows swing open,
`#374`) → the landing (the Study door says "Locked.", `#11`) → the candle corridor: light the two tall candles with the
Matchbox (`#4`) → the painting rolls up, the niche glows, a shot leaves it → take the brass key (`#89`) → the Study
door opens with the key in hand (`#11`) → walk in: card 2 (`#299`).

**The dark.** 88 of the 112 walked room sectors carry the lantern-cone ("candle") bit in the file: the whole hall, the
corridors, the stairs, the landing and the candle corridor. The lounge (group 10) is the only room lit without it.
No load-time light switch touches the area. [V: `-area` LIGHT section; sector byte +0x0a bit 1 as
REMAROTH_MEASURED "Candle (lantern) sectors" reads it.] Chapter 1 is the Lamp's chapter: the hall's dead switch even
has Adam say "Good job I've got the lantern." (entry 11).

**Corrections to CHAPTERS.md chapter 1** [V: `-area` with `-stop 29`, and again with the key and no stop]:
- The letters #34, #38, #53, #57 (i119, i11, i118, i117) lie on the Study's tables (s413, s320, s414, s415): chapter 2.
- The "landing candelabra STUDY1 #18" (obj id 2, s217/s252) stand in the Study: chapter 2. The landing pair is `#212`
  (obj id 16, s210/s211).
- The Ace of Spades (s279) is hidden at load and shown only by `#427` in `#357` (enter sector id 32 at s430: act180,
  cutscene 14, i180, a warp in place), far from the chapter 1 area: not chapter 1.
- The House Map i43 (`#195`, s22) is not reached in chapter 1.
- The serpent statuettes i4, the three masks, the Shield and the Sword are in the Study: chapter 2. The sarcophagus
  counting 7 is in the Study too (section 2.7 still sketches it, as asked).
- The Note i36 (`#29`) is chapter 1: the lounge typewriter (entry 20).

---

## 2. Chapter-wide ideas

### 2.1 The two hands in this chapter [P]

- **Item hand** (right, as built): shows the original's selected item. At new game the selected item should be the
  Matchbox: action 3 gives i68, i1, i15, i16 in that order and `give_item` selects each non-weapon, non-category-2
  item into the right hand, so the last one, i16, stays [R: inventory.c:416-434 as GAME_inventory.md 4 restates it;
  not seen].
- **Off hand** (left): the environment: handles, switches, the page, the scrapbook, the wards. Every off-hand touch
  fires the original's click with whatever the selected item is, exactly as a mouse click would.
- **Weapon**: Adam's fists until the Colt (entry 22); the owner kept the weapon in the main hand (handoff 2).
- Left-handed players: everything below names "item hand" and "off hand", never left and right. Whether a setting
  mirrors which physical hand carries the original's right-hand item is question Q11.

### 2.2 The Lamp in the dark [P, B]

- The Lamp (i15) is the one item no script tests, gives or removes after new game [V: ITEM_DESIGN 5, item_dossier].
  So carrying it is presentation, and it can be carried **without being the selected item**: the matchbox stays
  selected while the lamp hangs from the off hand or a hip hook. Holding it never changes a click.
- Owner's decision (WORLD_DESIGN, OWNER'S DECISIONS 1): the lantern light comes from the lamp in Adam's hand. In this
  chapter that light is the main light: 88 of 112 sectors are lantern sectors.
- Carry: bail in the off hand, `fbx_curl` (206, in roth_handhold.h); hip hook when the off hand needs to grab. While
  the off hand holds the bail its grip is the bail: it can still examine (trigger), and uses go through the item hand
  (Q2).
- The hall's dead switch (entry 11) is where the original itself points at the lamp: after it, the switch's examine
  line becomes "Good job I've got the lantern."

### 2.3 The matchbox and the candles [P, B]

The chapter has four candles that light, one record each pair: `#4` (two tall candles, gate) and `#212` (two
five-candle candelabra, optional). Six more are empty holders that never light (entry 15).

- **The loop**: the off hand slides the drawer and pinches a match (`fbx_pinch`, 694), strikes it along the
  striker of the box in the item hand, carries the flame to a wick. **Commit**: the flame tip enters the wick sphere
  (4 cm, wick points from `tools/wickspots`) → one use on that candle object, with i16 selected. The original's chain
  does the rest (its delay, art swap, light step). (ITEM_DESIGN 16 and WORLD_DESIGN 2.3; which hand holds the match
  is Q1.)
- **Vanilla stays**: touching the candle with the item hand holding the matchbox (built, verified:
  `engine_useitem_flat_ad6d1ec5.png`), or pointing and gripping at range.
- **One event per candle object.** A five-candle holder takes one use; its other wicks catch as presentation only,
  and the untouched ones 15 s after the first (owner). The chain re-keys the object at once (0x3a → 0xffff), so
  nothing else can fire there [V: chains].
- **Timing the original imposes** [V: chains; flow pre-pass, GAME_core.md 5.1]: while a candle record's delay is
  armed (30 tics for `#4`, 20 for `#212`, about 0.43 s and 0.29 s), a click on the record's other object is ignored.
  The two tall candles stand 2.3 m apart, so one match cannot meet that window by accident; the VR layer still sends
  exactly one use per wick entry and never retries one the original ignored.
- **Never light what the original cannot**: the six empty holders (entry 15) and every candle drawn lit get no flame
  from a match. The wick spheres exist only on the scripted unlit kinds.
- **Light**: one growing light per holder (owner), on top of the original's sector ramps (groups 14 and 22, entries
  31 and 33). The darkness pass makes each catch visible; the original's ramp stays the game's light.

### 2.4 Doors: one feel for the whole game [P, B]

Chapter 1 has 16 door sectors and the front door face: 2 open freely (the lounge doors), 1 with a key (the Study),
2 casement leaves, and 11 that answer "Stuck fast." / "Locked." [V: `-area`, world_census `-doors`]. The handle
design is WORLD_DESIGN 1.3: off hand on the painted lever (`fbx_wrap_half`, 820), press down 30° or push/pull 4 cm =
one use at the handle's texel; the door swings by the original's tick. This chapter sets the feel players keep:
- **opens**: a click haptic, a soft rumble while the leaf swings;
- **"Stuck fast."**: a heavy shudder, no give;
- **"Locked."**: a latch click-stop (short, sharp), no give.
The lines are the original's; only the haptics differ.

### 2.5 Reading documents [P, A/B]

Chapter 1 gives three documents: the Book (i17), the Scrapbook (i27, one page) and the Note (i36, one page)
[V: item_dossier]. **A**: the original's close-up popup with its page view, placed squared to the eyes at reading
distance (VR_INTERACTION_RESEARCH 2.2), page turns swap in place (2.3). **B**: the item held up to the face opens it;
the book opens in both hands (entry 7).

### 2.6 "A physical attempt plays the examine" [P, B]

Where the original has only an examine line, a natural attempt plays that line and nothing else: hand into the sofa
crease ("Nothing shoved down the upholstery.", entry 14), fingers on the typewriter keys ("Some of the keys as well.",
entry 21), a hand in the fly cloud ("God what a stench!", entry 5), the palm searching a door for its lock ("Where's
the lock on this damn thing?", entry 10). **Commit**: the original's examine click on that target, once per attempt
(re-armed when the hand leaves 10 cm). No new sound; the line is the original's. A knuckle rap on solid things is
proposed the same way (Q3).

### 2.7 The statuettes and the sarcophagus counting 7 (chapter 2's room, sketched as asked) [P, B]

Not chapter 1 [V: section 1]. For the record, the shape the design should take when chapter 2 is written: each of the
seven floor plates (`#119`, `#124`-`#129`) is one use with the Statuette in hand: it takes the statuette, shows it on
the plate, and counts to 7; the seventh runs "It's... it's opening!" and lowers s350 (the lid) [V: OPENABLES 3]. Taking
one back (`#121`, `#136`-`#141`) steps the count down [V: CHAPTERS 5.5]. VR: set the statuette on the plate and let
go = one use (socket, VR_INTERACTION_RESEARCH 3.1); lift it back = one use. One placement = one event, never two
(CHAPTERS 5.4). Seven glows can accumulate as the original shows each statuette on its plate.

### 2.8 Breaking glass [P, A/B]

Two mirrors, five windows and the two casement leaves in the area break on the original's attack (0x1a records) [V]. Adam's fists, the Colt, or
later weapons. The original hurts Adam for it: the mirror's chain applies damage to the player (0x33), a broken
window becomes a hazard face (0x35) [V: chains; ROTH_COMMANDS.md opcode table]. VR adds only the feel (a strike
detector, haptics). One swing is one attack: a fist passing through the plane must not hit twice
(cmd_smash_face_texture toggles the broken bit [R: raw_commands.c:361 as openables' header restates it], so a second
hit could mend the glass).

### 2.9 Movies and the two cards [P, A]

Card 1 (new game) and card 2 (the Study) play on a world-locked screen ahead, head free, world held still; decode off
the sim path (VR_INTERACTION_RESEARCH 7.1, 7.4). The original's warp and forced turn after card 2 happen during black
(Q7).

---

## 3. The entries

★ = on the critical path. Each entry: **What**, **Original**, **VR** (gesture and hand / pose / feedback / COMMIT /
fallback), **Tag**, **Q**. Record numbers are STUDY1's. Sizes are the art's as drawn (sprites) or the geometry's.

### A. The opening

**1. ★ New game: card 1 "Shadows" and the starting kit**
- What: no object. DBASE100 action 3 [V: dbase100_dump].
- Original: cutscene 151 (CHPTR1), music, give i68 Adam, i1 Shards and Seals, i15 Lamp, i16 Matchbox. Adam starts in
  s267 in the hall [V: metadata]. The Matchbox ends selected [R: 2.1].
- VR: the card on the world-locked screen (2.9). After it, the kit is where the original puts it: the Matchbox in the
  item hand (`fbx_wrap_half` round the sleeve, as built); the Lamp lit on the hip hook or in the off hand (2.2, Q2);
  Shards and Seals and Adam stay in the inventory (ITEM_DESIGN 1 and 68). COMMIT: none, the original runs it.
  Fallback: none needed.
- Tag: A (screen), B (lamp carry). Effort S.
- Q: Q2.

### B. The entrance hall (start)

**2. Front door**
- What: face f1386 on s279, DEMO[463] double door, 1.76 m wide [V: face length]. Painted handles at the middle.
- Original: use `#167` (live, modifier 00): delay 4 tics → "Locked." (act316); trigger sound 144. Use `#660` on the
  same face is off at load: the game's ending (enabled by `#571`, the Monster, chapter 20) [V: chains].
- VR: off hand on either painted handle (`fbx_wrap_half`), press down or pull. COMMIT: one use on f1386 at the handle
  texel. Feedback: the latch click-stop (2.4) and the line. Fallback: point + grip. The same gesture is the ending in
  chapter 20, so it must stay a plain use.
- Tag: B (handle system, 2.4). Effort S once the door handles exist.

**3. Coatstand**
- What: DEMO[4102] in s279, 0.55 × 2.00 m [V: art 55 × 175].
- Original: examine `#341` "Coatstand." No use record [V].
- VR: palm on it + trigger, or point + trigger. A grab sends the use and nothing happens (no record, map-pack art is
  never a pickup) [V: give_item_by_dbase_id rule]. The lamp is not hung on it: ROTH has no drop (GAME_inventory 4).
- Tag: A. Effort S.

**4. Suits of armour (4 in the area)**
- What: DEMO[4105], obj id 46: s487 and s59 (hall), s15 ×2 (gallery); 0.75 × 1.95 m [V].
- Original: examine `#313` "Old suit of armour. All the pieces are wired together. ..." [V].
- VR: palm + trigger (A); a knuckle rap plays the same examine (2.6, Q3). COMMIT: examine on obj id 46. Feedback: a
  dull rap haptic; no new sound.
- Tag: A / B (rap). Effort S.

**5. The armour with the flies**
- What: DEMO[4141] in s268, 0.75 × 2.28 m, with four ADEMO[198] sprites around it (z 100-260, no records) [V: looked
  at: a suit with green streaks over the helm].
- Original: examine `#337` "God what a stench! / ... flies are everywhere." [V].
- VR: a hand pushed into the cloud (within 30 cm of the sprites) plays the examine (2.6); a faint buzzing haptic while
  the hand is inside. COMMIT: examine on obj id 51, once per entry. Fallback: point + trigger.
- Tag: B. Effort S.

**6. The hall table**
- What: sector id 21 (s287, s288, s412), 0.74 × 0.66 m; objects lie on it at z 74 = 0.85 m [V: object z]; whether
  the top is a mid-platform was not read [R].
- Original: examine-floor `#245` → act12 "Stylised table." (its count gates on i9, i17, i19 say nothing more) [V].
- VR: palm flat on the top + trigger (A, if NearAct reaches a floor/platform target [R: not checked]); a rap (Q3).
- Tag: A. Effort S.

**7. The Book (and the silver key inside)**
- What: ADEMO[230] on the hall table (s412, 0.85 m), drawn 0.39 × 0.13 m [V].
- Original: click `#205` → give i17 "Book" (sound 275). i17 is a document (category 4), close-up 20 frames. Its
  OnInspect says "Novel. Look Homeward Angel." / "Is there anything hidden in the pages?" / "Silver key." and gives
  **i20 Key (silver)** [V: item_dossier]. OnInspect runs on the **first** close-up only (rec+7 bit 0x80, file_config.c
  962-967 as GAME_inventory 5 restates) [V: doc; R: not seen]. Order-sensitive: the key comes once, on the first look.
  i20 is used in RAQUIA5 (chapter 10).
- VR: grab the book with either hand (`fbx_wrap_half` across the spine). COMMIT 1: use on obj id 57 (built grab). The
  book flies the last centimetres into the hand (the original's pickup fly, retargeted) and becomes the item hand's
  item. Bring it up to the face, or pull the trigger on it: COMMIT 2 = the original's examine of the held item → its
  popup. **B**: the book opens in both hands while the original's film plays on a panel beside it; when the original's
  OnInspect gives i20, the silver key slides out of the pages into the off-hand palm (the give's fly, retargeted) and
  then becomes the selected item as the original selects it [R: give_item selects it]. A second look shows the book
  and gives nothing, as the original.
- Tag: A (grab, popup), B (book in hands, key into the palm). Effort M.
- Q: Q8.

**8. Colt-45 magazines (2 in the area)**
- What: ADEMO[141], obj id 1000: on the hall table (s411, 0.85 m) and on the lounge side table (s41, top 0.69 m);
  drawn 0.39 × 0.15 m [V].
- Original: click `#410` → give the clicked object's item (Colt-45 magazine), sound 273 [V].
- VR: grab with either hand, `fbx_wrap_half` (UZDXREMA's hold_magazine). COMMIT: use on obj id 1000 (built grab).
  Fly to the hand, then the inventory. Fallback: point + grip.
- Tag: A (grab), B (fly to hand). Effort S.

**9. The wards: glowing pentagrams and the green sigil (10 in the area)**
- What: DEMO[4207] green pentagram, 0.48 m across, 1.15-1.74 m up (s337, s72, s26, s501 ×3); DEMO[4208] tan
  pentagram, 0.55 m, 1.15-1.90 m (s482, s499, s495); ADEMO[232] green sigil, 0.54 m, 1.37-1.87 m (s486). All
  fixed-angle quads on walls (renderType 0x80) [V].
- Original: click `#258` (pentagrams, obj id 36) / `#498` (sigil, id 59) → one of "This one's stuck fast. / Stuck fast.
  / Damn. stuck fast. / Won't budge." (act530), sound 97. Examine `#371` / `#495` "This symbol's glowing. / I believe
  it's designed to keep us in, or keep something out." [V].
- VR: off hand grips the rim and pries or twists. COMMIT: one use per grip close on the plaque. Feedback: a stiff
  resisting buzz, no movement. Palm flat on it + trigger = examine. Optional: a faint tingle haptic when a palm comes
  within 5 cm (presentation of "glowing"; Q9).
- Tag: B. Effort S.

**10. The warded double door**
- What: door sectors s462 and s66 (DEMO[96], each leaf 0.86 × 2.50 m), from s482 [V].
- Original: use `#460` / `#462` (box y 0..108) → "Stuck fast. / Won't budge." (act531), sound 144. Examine `#454` /
  `#455` "Where's the lock on this damn thing?". The openers `#456` / `#459` and the lines `#452` / `#453` ("Wasn't
  this door warded earlier?") are off until card 3 enables them [V: chains; `#75`].
- VR: the handle (2.4) → the shudder and the line. The palm sweeping the leaf's latch edge, searching for a lock,
  plays the examine (2.6). COMMIT: use / examine on the face.
- Tag: B. Effort S (with 2.4). Measure: the handle on DEMO[96].

**11. The dead light switch (hall)**
- What: the wall panel DEMO[505] on face f2350 (s486, beside door s308). The records carry a surface box x 22..29,
  y 131..139 texels: the brass switch on the panel [V: box words; picture looked at]; about 1.2-1.4 m up [R: from the
  face height and the picture's rows].
- Original: use `#385` (box) → act222: one of "Doesn't appear to be working." / "Damn. Power's off in here." and
  **flag 257**; sound 115. Examine `#372` (same box) → act415: "Light switch." until flag 257, then "Good job I've got
  the lantern." (alone) [V: dbase100_dump]. Order-sensitive: the examine line changes after the first use.
- VR: flick the switch with the off hand's index (point pose; the fingertip press), or a slap with the open hand
  (WORLD_DESIGN 3.2). COMMIT: one use at the fingertip's texel, only inside the box. Feedback: a crisp click haptic and
  the record's sound; nothing lights (the original's). Then the natural move is to lift the lamp (2.2).
- Needs the record box test (not ported in the flat port either: roth_runtime.cpp, items 22-23 of its checklist).
- Tag: B. Effort S + the box test.

**12. The hall paintings (5 faces, 4 records)**
- What: pictures in wall niches, each 1.00 m wide, 1.42-2.57 m up (s60 DEMO[14] the sorcerer, s61 DEMO[406] the
  robed man, s62/s63 DEMO[405] the eyes, s254 DEMO[13] Elias Camber) [V].
- Original: examine `#307` "Sorcerer? / Is that a mask behind him?"; `#308` "Cult style robes. ..."; `#309` "Those
  eyes... almost look real. ..."; `#316` act124 "Hmm. Elias Camber if I'm not mistaken. / He's holding a mask." the
  first time (flag 38), "There he is again..." after [V]. Order-sensitive: `#316`'s line depends on flags 38 and 40.
- VR: point + trigger (at 1.4-2.6 m they are above reach for most); lamp held up lights them (2.2).
- Tag: A. Effort S.

**13. The hall mirror**
- What: niche s73, DEMO[752] (a grey streaked glass), 1.00 × 1.15 m, 1.42-2.57 m up [V].
- Original: examine `#240` → act414: before it breaks, act15 picks one of "Mirror, mirror on the wall..." / "Hmm.
  Looking good." / "Grief. I could really do with some sleep." / "Hell. This's really taking it's toll."; after
  (flag 210), act16 "Smashed." / "Cracked." [V]. Attack-hit `#386` (sound 108): 0x2e smashes face 40, flag 210,
  particles, 0x33 damage to the player [V: chain; opcode table]. Order-sensitive: the lines change after the smash.
- VR: look in + trigger = examine. Punch it (fist) or shoot it: the original's attack → `#386` if the hit reaches the
  face [R: the fists' reach against walls not checked, GAME_combat.md]. Feedback: a sharp jolt in the punching hand,
  the record's sound, the original's damage. A live reflection would be new presentation (Q5).
- Tag: A (attack, examine), B (strike detector). Effort S.

**14. Chairs (6) and the sofa**
- What: chairs DEMO[4109] (mesh 0.50 × 0.82 × 1.72 m: s263, s265, s54, s495, s9, s15); sofa DEMO[4106] (mesh
  1.76 × 0.80 × 1.05 m, s495) [V: propcheck -mesh].
- Original: examine `#244` "Plain old chair. / Guess so."; sofa `#314` "Chair. / Nothing shoved down the upholstery."
  No use [V].
- VR: the sofa: the hand pushed down into the seat-back crease (a volume along the mesh's back edge) plays `#314`
  (2.6). Chairs: palm on the back + trigger. COMMIT: examine on obj id 17 / 47.
- Tag: A (chairs), B (sofa crease). Effort S.

### C. The east passage

**15. The empty candelabra (6)**
- What: DEMO[4189], obj id 15, 0.55 × 1.72 m: s489, s490, s54, s42, s45, s46 [V].
- Original: examine `#186` "Candle. / Tallow I believe." No click record keyed to id 15 [V: `-area`]: a use with the
  matchbox does nothing.
- VR: no wick spheres here (2.3). A match held to them does nothing, as the original. Trigger examines.
- Tag: A. Effort S (the rule, not a feature).

**16. Windows onto the grounds (5 faces)**
- What: DEMO[738] on f162 (s35), f222 (s49), f226 (s50), each 2.56 m wide; DEMO[716] on f128 (s29), 1.20 m; DEMO[742]
  on f85 (s20, the landing), 1.28 m [V: faces].
- Original: attack-hit `#181` (sound 108): 0x2e smashes the face hit, 0x35 makes it a hazard face (damage to a player
  near it), three particle bursts. Not one-shot [V]. No examine.
- VR: punch / shoot / later swing (2.8). COMMIT: the original's attack. Feedback: glass burst haptic; when the
  original's hazard hurts Adam beside broken glass, a sharp tick on the nearer hand.
- Tag: A (attack), B (strike). Effort S.

### D. The lounge

**17. ★ The typewriter starts by itself (walk-in)**
- What: entering sector id 5 (s42, s43, s54, s55: the middle of the lounge), once [V].
- Original: `#112`: (0x0c, disabled, skipped) → sound node 25 → 0x03 on sector id 23 (s306, s315: the typewriter's
  page faces) ramping the transparent mid size from 110 to 0, step 1 → **delay 800 tics (11.4 s)** → sound node 25 →
  enable `#29` (the page can be taken) [V: chain; R: that the ramp shows the typed page rising, from roth_raw.h's
  reading of 0x03]. Order/timing-sensitive: the Note cannot be taken before the delay ends.
- VR: nothing to do. The sound nodes are positioned sounds already; the page rises as the original draws it. COMMIT:
  the player's position crossing into the sector (real walking counts: the playsim position is what moves).
- Tag: A. Effort S.

**18. ★ The casement windows swing open (walk-in)**
- What: entering sector id 4: s492 (lounge) or s1/s19 (stairs, unavoidable), once [V].
- Original: `#374`: 0x2f on face id 19 → the casement leaves of the east room's window bay (s298: leaves s299/s300) and
  a second pair outside the area (s302) open; "extent" word 0 [V: chain; R: what extent 0 does].
- VR: nothing to do; the leaves swing where they are, with the original's sound. Pairs with entry 29's line.
- Tag: A. Effort S.

**19. ★ The lounge doors (the first doors that open)**
- What: door s30 (between s491 and s31) and door s44 (between s494 and s26); 1.28 × 2.50 m, lever at about 1.08 m.
  From the lounge the faces show DEMO[168] (handle on the left), from the corridor DEMO[100] (right) [V]. Both swing
  into the lounge (marker 0xfffe → the room across c3), so from the lounge the leaf comes **toward** Adam [V: marker;
  R: reading, WORLD_DESIGN 1.1].
- Original: use `#13` (face id 4, shared by many doors): 0x2f (sound 140) → "Open. Good." (act527); trigger sound 132.
  An open leaf is pick type 6: a use toggles it [V]. `#13` also sits on the archway faces f2392/f2398 (s493/s494,
  DEMO[170]), which are no door [V; R: a use there does nothing].
- VR: off hand on the lever (`fbx_wrap_half`), press down 30° or pull 4 cm. COMMIT: one use at the handle texel. The
  hand lets go as the leaf starts toward Adam (WORLD_DESIGN 1.5 Q1); a stronger haptic on the pulling hand.
  Fallback: point + grip.
- Tag: B. Effort M (the door system, used by every door in the game).

**20. The typed page in the typewriter → the Note**
- What: face id 21 (f1552, f1558: DEMO[722], a typed sheet) on the typewriter top (s313/s315, 1.26 m up), 0.52 m
  face [V].
- Original: use `#29` (off until `#112`'s delay ends, one-shot, sound 109): 0x34 removes the page picture, give i36
  "Note" (document, one page; OnInspect "Typed."; close-up 51 frames, hold mode) [V].
- VR: the off hand pinches the page's top edge (`fbx_pinch_tight`, 312) and draws it up out of the carriage 6 cm.
  COMMIT: one use on face 21 at the pinch. The page lifts with the pinch for those 6 cm, then flies to the hand as the
  original's give (the sheet picture is already gone by the chain). Before `#29` is live the pull sends the use and the
  original does nothing; the page stays (presentation follows the result, never predicts it). Reading: 2.5.
- Tag: B. Effort M. Measure: the page's top edge on face 21 and its height under the 0x03 ramp.

**21. The typewriter (examine)**
- What: the typewriter faces (DEMO[533]/[534]/[170]/[178] around s306, s313-s316) and its top s306 (DEMO[535]) [V].
- Original: examine-wall `#90` and examine-floor `#286`: "Ink ribbon's missing. / Some of the keys as well." [V].
- VR: fingertips pressing the keys play the examine (2.6); a key-press haptic only (no typing sound: the original has
  none here). COMMIT: examine on the touched face or top.
- Tag: B. Effort S.

**22. The Colt-45**
- What: ADEMO[99] on the desk (s305, top 1.03 m), drawn 0.32 × 0.14 m [V].
- Original: click `#416` → give the clicked item: i9 Colt-45 (weapon; OnInspect "Colt .45. Semi-automatic handgun." /
  "Know your guns don't you?"); sound 275. A weapon is equipped at once (inventory.c:418-428) [V: dossier; GAME_inventory].
- VR: the weapon hand closes on the grip (`grip_pistol`). COMMIT: use on obj id 1002 (built grab). The gun stays in
  that hand (RealmsHeld, owner's placement). Fallback: point + grip.
- Tag: A (grab), B (into the hand). Effort S.

**23. The scrapbook on the desk**
- What: drawn on the desk-top floor picture DEMO[214] of s322 (sector id 6): the blue book and papers at its lower
  right; desk 1.28 × 0.88 m at 1.03 m. The record's box x 32..63, y 82..119 texels covers the drawn book [V: box;
  picture looked at].
- Original: use-floor `#51` (box, one-shot, sound 109): floor picture → DEMO[586] (the desk without it), give i27
  Scrapbook (document, one page; OnInspect "There's a newscutting in this scrapbook." ...) [V].
- VR: palm on the drawn book (inside the box) and grip, or slide it toward the desk's edge 5 cm. COMMIT: one use on the
  floor at the palm's texel. The picture swaps (the book leaves the desk) and the scrapbook is in the hand.
- Needs the box test and the floor texel at the hand. Tag: B. Effort S + the box test.

**24. The lounge light switch (2 plates)**
- What: DEMO[4185], obj id 13, 0.19 m plates: s55 at 1.17-1.39 m and s495 at 1.26-1.48 m [V].
- Original: click `#585` (sound 115): 0x40 runs `#592`, a light switch on group 10 (flood: the lounge, 32 sectors),
  step −15; sets **flag 421** [V: chain; `-area` LIGHT]. Flag 421 is tested at the end of the game (`#564`) [V]. The
  plate flips to its neighbouring picture through the exec tail [R]; whether this switch also sets the lantern bit on
  the lounge, and the direction of the first flick, were not traced [R: cmd_light_switch raw_commands.c:4331].
  Examine `#173` "Light switch.".
- VR: flick with the index or slap (WORLD_DESIGN 3.2, Q from WORLD_DESIGN 3.7). COMMIT: one use on obj id 13. The room
  dims by the original's ramp; if it goes to the lantern mode, the lamp's light takes over (2.2).
- Tag: B. Effort S.

**25. Vases (2)**
- What: DEMO[4101], 0.20 × 0.38 m: on the lounge side table (s41, 0.69 m) and the landing pedestal (s28, 0.69 m) [V].
- Original: examine `#549` "Ceramic Vase." No use [V].
- VR: palm + trigger. A grab closes the hand on it and nothing moves (the original moves nothing).
- Tag: A. Effort S.

**26. Doors that do not open in chapter 1 (9 door sectors)**
- What [V: `-area`, world_census `-doors`]: s336 (from s337), s112 (from s72), s0 (from s26), s48 (from s499), s47
  (from s495), s105 and s121 (from s501): DEMO[100]/DEMO[168]; s308 (from s486, DEMO[90]); s21 (from s501, DEMO[100]).
- Original: s336, s112, s0, s48, s47, s105, s121: use `#442` (box x 0..63, y 0..109) → "Stuck fast. / Won't budge."
  (act531, sound 144); examine `#395` "Where's the lock on this damn thing?" and `#397` "Wasn't this door warded
  earlier?" are both live on those faces: the first live one in directory order plays (roth_runtime.cpp checklist 23)
  [V; R: which is first]. Their opener `#202` is off until card 3. s308: `#484` "Stuck fast." (sound 142); opener `#481`
  off (flag 318, later). s21: `#408` "Locked." (act316); opener `#86` off; the chest `#88` in a later room opens it
  remotely [V].
- VR: the handle (2.4): "Stuck fast." = shudder, "Locked." = latch click-stop. COMMIT: one use per press.
- Tag: B. Effort S (with 2.4).

### E. The south corridor and the east room

**27. The headless rat**
- What: ADEMO[284] on the floor of s257, 0.41 × 0.27 m; not a pickup [V: `-area`].
- Original: examine `#287` "Rat. No head." [V].
- VR: crouch and touch + trigger, or point + trigger. A grab does nothing.
- Tag: A. Effort S.

**28. The boot prints**
- What: the floor of sector id 26 (s31, s257), pictures DEMO[127]/[125] [V].
- Original: examine-floor `#241` "Boots. Bet he shops at oversize. / Must be one big guy." [V].
- VR: point + trigger; or the open hand laid in the print (2.6: a physical measure plays the line).
- Tag: A / B. Effort S.

**29. The open window**
- What: face f1257 between s258 and the window bay s259 (DEMO[716], 1.28 m wide, sill 1.03 m) [V].
- Original: examine `#373` "Looks like someone's left the window open. / Recently too going by the water on the
  floor."; use `#200` (a door on this face) is off at load [V].
- VR: palm on the wet sill + trigger; a cold-wet haptic is not proposed (no state, no need).
- Tag: A. Effort S.

**30. The casement leaves in the bay**
- What: door sectors s299/s300 off s298, beyond the bay s259: two leaves DEMO[748] (wooden frame, glass panes),
  0.64 × 1.90 m each from the 1.03 m sill [V].
- Original: use `#78` (face id 19, sound 149): 0x2f on the face used, free; attack-hit `#72` and `#181`: glass smash
  as entry 16 [V]. `#374` already opened them (entry 18).
- VR: grab a leaf's frame and push/pull = one use. Reach [R]: the leaves are two portals past the walk; a touch must
  pass the inner window (`#200` off), so pointing (the original's pick skips transparent texels) is likely the only way
  in chapter 1 (Q14).
- Tag: A (point), B (grab). Effort S.

### F. The stairs

No records on the stairs other than `#374` (entry 18), which fires on s1/s19 if it has not fired in the lounge [V].

### G. The landing

**31. The candelabra pair on the landing**
- What: DEMO[4098], obj id 16, in s211 and s210 (both on the landing at floor 400); 0.55 × 1.88 m; five wicks at
  (−23, 141), (−13, 154), (0, 159), (13, 154), (23, 141) units = 1.62-1.82 m up, ±0.23 m [V: wickspots].
- Original: click `#212` (sound 258): i16 in hand (f6 03: carried but not selected → auto-select and nothing else) →
  re-key −1 → delay 20 → re-key 15 → art DEMO[4148] (all five lit) → light group 22 +4 (flood: the landing and the
  stairs, 16 sectors). Examine `#73` "Candle. / Tallow I believe." Optional. Timing: a click on the other candelabrum
  within 20 tics is ignored [V: chain; flow rule].
- VR: the match loop (2.3): first wick = the use; the other wicks by touch or 15 s later; one growing light.
  Fallback: the matchbox hand touches the candelabrum (built), or point + grip.
- Tag: B. Effort M (in progress: per-wick compositing, the growing light).

The Study door is met here first (entry 36: "Locked.").

### H. The candle corridor

**32. ★ The painting between the candles**
- What: the upper piece of face f255 (DEMO[512]) at the corridor's west end, between s56 and s57: 1.00 m wide,
  1.15-2.29 m above the corridor floor. The picture shows a sarcophagus **flanked by two tall lit candlesticks** [V:
  looked at] — the clue. The niche behind it (s58, 0.64 × 1.00 m, floor 1.15 m up) is closed: s57's ceiling starts at
  500 by the load-time 0x07 pass on record 9 [V: roth_raw.h ApplyLoadTimeHeights note; heights].
- Original: examine `#296` "Painting of a Sarcophagus." [V].
- VR: point + trigger, or palm on the canvas + trigger. With both candles lit (entry 33) their light falls on it.
- Tag: A. Effort S.

**33. ★ The two tall candles**
- What: DEMO[4186], obj id 3, in s368 at (−208, 440) and (−208, 672): flanking the painting, 2.3 m apart, 0.5 m out
  from the wall; 0.55 × 1.87 m; one wick at (0, 163) units = 1.87 m up [V: positions; wickspots].
- Original: click `#4` (sound 258): i16 in hand (f6 03) → re-key −1 → delay 30 → re-key 0 → art DEMO[4187] (lit) →
  light group 14 +5 (flood: s368, s404, s405, s369, s370) and +5 (s368) → **count to 2** (`#8`). The first candle's
  chain stops at the count; the second's continues into entry 34 [V: chain]. Each candle is one-shot (re-keyed to 0).
  Timing: the second candle within 30 tics of the first is ignored (armed delay).
- VR: the match loop (2.3). The wick is overhead for most players (1.87 m): reach up with the match. Seated or short
  players: the matchbox hand's touch on the candle's stem (built), or point + grip from the floor. COMMIT: one use per
  candle at the flame's first entry into the wick sphere. Feedback: a soft catch pulse in the match hand, the record's
  sound, the flame swap, the corridor brightening by the original's +5 +5 and the candle's own growing light.
- Tag: B. Effort M (shared with 2.3).
- Q: Q13.

**34. ★ The painting rolls up, the niche glows, a shot (scripted event)**
- What: the tail of `#4` after the second candle [V: chain].
- Original: record 9: 0x07 raises s57's ceiling from 500 to 600, step 1 (the painting rolls up into the wall top and
  opens the niche s58) → light group 46 (s58) +40 → delay 310 tics (4.4 s) → +30 → delay 70 (1.0 s) → −30 → `#429`:
  0x3c on obj 64 (a hidden DEMO[4114] glow in the niche) with template 34, DBASE100 item "Bullet" (an AsBullet record):
  the projectile path, `spawn_player_projectile_flagged` from obj 64's position (raw_commands.c:4108-4160) [V: record
  words, item dossier, handler read; R: the shot's direction and what it hits].
- VR: nothing to do: the roll-up is the original's geometry, the glow the original's ramp (made visible by the
  darkness pass), the shot the original's projectile. A hand in the niche at that moment is not hit; only the
  original's player body collides [R]. Optional: a small dynamic light following the niche's ramp (Q10).
- Tag: A. Effort S.

**35. ★ The brass key**
- What: ADEMO[131], obj id 14, on the niche floor (s58 at 500: 1.15 m above the corridor), drawn 0.30 × 0.15 m;
  reachable only after entry 34 opens the niche [V].
- Original: click `#89` (one-shot, sound 70) → give i21 "Key" (ChangeName block: its label changes on a condition;
  close-up 20 frames). Examine `#298` act111 "Brass key. No markings." (and, with Rebecca, "The lack of marks on the
  Ward ...") [V].
- VR: reach into the niche (0.3-0.6 m in, at chest height) and pinch the key (`fbx_pinch`; a lateral pinch would fit
  better, P1). COMMIT: use on obj id 14 (built grab). It flies the last centimetres into the pinch and becomes the item
  hand's item [R: give selects it]. Fallback: point + grip from the corridor.
- Tag: A (grab), B (pinch into the hand). Effort S.

### I. Back at the Study door

**36. ★ The Study door**
- What: door sector s24 between the landing (s25) and the Study (s266); face f104, DEMO[100] (lever plate at u 0.88,
  43% up: about 1.09 m); 1.28 × 2.52 m; swings into the Study, away from Adam (marker 0xfffd → the room across c1)
  [V: door row; R: the swing reading].
- Original: use `#11` (sound 135): delay 2 tics → latch "Locked." (act316) → 0x27 i21 **in hand** (f6 13) → flag 374
  → 0x2f (sound 139). Without the key: "Locked.". With the key carried but not selected: the original selects it into
  the right hand, interrupts, and does nothing; the next use opens [V: chain; cmd_if_not_item rule]. Timing: a second
  use inside the 2-tic delay is ignored.
- VR: key in the item hand (pinch), its bit to the lever plate, wrist turned 30° (WORLD_DESIGN 1.3 KEY doors). COMMIT:
  one use on f104 at the plate. Refused: the latch click-stop and "Locked."; carried-not-selected: the key appears in
  the item hand (the original's auto-select shown) and nothing else; opened: the door swings away by the original's
  tick. Without the key: the off hand on the lever gives "Locked.". Fallback: point + grip with the key selected.
- Tag: B. Effort M (with 2.4).

### J. The threshold

**37. ★ Card 2 (walking into the Study)**
- What: sector id 29 (s266), behind the Study door [V].
- Original: enter `#299` (one-shot): act178: UI latch 4, flag 422, cutscene 5 (RANDALL), cutscene 162 (CHPTR2), give i70
  "Your father" (category 2: never in a hand), RAW object 32010 → `#449`: warp STUDY1 @34 (in place) and 0x3f turns
  the player; then act633 (music), enable `#265` [V: chains]. DBASE100 actions hit during the movies are re-queued
  (CHAPTERS 5.10). This ends chapter 1.
- VR: the movies on the world-locked screen; the warp and the turn during black (Q7). COMMIT: the body crossing into
  s266.
- Tag: A. Effort S.

### K. The upper stair and gallery (optional dead end)

**38. The gallery mirror**
- What: niche s16 at the top of the upper stair (s15 at floor 640), DEMO[752], 1.00 × 1.15 m, 1.42-2.57 m above s15
  [V]. The gallery also holds two suits (entry 4) and a chair (entry 14).
- Original: attack-hit `#346`: smash face 20, flag 50, particles, 0x33 damage; examine `#310` → act413 (as entry 13 on
  flag 50) [V].
- VR: as entry 13.
- Tag: A / B. Effort S (shared).

### Not interactable in chapter 1 (present in the area, asleep) [V: `-area`, chains]

| what | where | why not in chapter 1 |
|---|---|---|
| the Monster (ADEMO[271], `#571` attack-hit → enables `#660`) | s267, hidden | shown by `#566` in `#564` (flag 373: the end) |
| Key i173 (ADEMO[130]) | s486, hidden | shown by `#474` in `#472` (flag 318) |
| Ace of Spades i52 | s279, hidden | shown by `#427` in `#357` (sector id 32, s430: not in the area) |
| developer objects `#576`, `#606` | s489, s261 | hidden, never shown (CHAPTERS finding 8) |
| enter `#564` (sector id 31), `#584` (s26), `#595` (s209), `#472`/`#486`, `#14` (off), `#265` (off) | hall, corridor, landing | gated on flags 373 / 92 / 318, or off until later cards |
| chandeliers DEMO[4115] (s267, s192), a stand DEMO[4163] (s194), a table DEMO[4111] (s495), two niche pictures DEMO[408] (s369, s370) | | no records: where the original shows nothing, we show nothing |

---

## 4. Ranked: the 10 most fun or impactful VR interactions in chapter 1 [P, R: ranking]

1. **Light the two tall candles with a struck match, and watch the painting roll up** (33, 34, with 32 as the clue):
   the chapter's one puzzle, solved with the hands; the corridor and the niche brighten in the dark; a shot leaves the
   niche.
2. **Pinch the brass key from the niche and turn it in the Study door** (35, 36, then 37): the first refused door
   becomes the first key-turn; the card follows.
3. **The lamp in the off hand through a dark house** (2.2): 88 of 112 sectors are lantern sectors; the original itself
   says "Good job I've got the lantern." (11).
4. **The typewriter that types by itself, and the page pulled from the carriage** (17, 20).
5. **The book that drops a silver key into the palm** (7), on the original's first look.
6. **The mirrors: look, then punch or shoot them; the glass bites back** (13, 38; windows 16).
7. **The first door pulled open by its lever, and the house full of doors that will not move** (19, 26, 10, 2): the
   feel every later door inherits (2.4).
8. **The Colt picked up by its grip from the desk, the magazines from the tables** (22, 8).
9. **Prying at a glowing pentagram that will not come off** (9).
10. **Flicking the dead hall switch, then dimming the lounge with the live one** (11, 24).

Honourable mentions: the hand pushed into the sofa crease (14), the scrapbook slid off the desk (23), the five-wick
candelabra (31), the casements banging open (18).

---

## 5. What each needs that is not built yet

**Engine** [R: from reading the sources named; nothing built]:

| # | need | for entries | state |
|---|---|---|---|
| N1 | the record surface box (+0x0c..+0x12) tested against the hit texel | 11, 23, 10, 26, 36 | not ported in the flat port either (roth_runtime.cpp checklist items 22-23): a 1:1 gap to fix in flat first |
| N2 | use and examine fired at a contact point (hand → face texel, floor texel, object) | 11, 20, 23, all doors | new |
| N3 | the chain's result read back (acted / refused / ignored under an armed delay) for haptics | doors, candles, 20 | new (WORLD_DESIGN 1.4) |
| N4 | door handles: handle zones per picture, press/turn/push/pull detector, swing haptics | 2, 10, 19, 26, 30, 36 | new (WORLD_DESIGN 1.4) |
| N5 | the match: drawer, pinch, strike detector, the 20 s flame light, wick spheres, first-wick commit | 31, 33 | per-wick lighting and the growing light in progress; the match itself new |
| N6 | the lamp: hip hook, the light from the lamp in the hand (owner's decision), its lantern cone | 2.2, all | new |
| N7 | strike detector (hand speed into a surface) mapped to attack (glass) or use (switch slap) | 13, 16, 24, 38 | new |
| N8 | pinch-and-pull and slide detectors | 20, 23, 9 | new |
| N9 | the pickup fly retargeted to the hand | 7, 8, 22, 35 | new presentation of the original's fly |
| N10 | the book held open for its close-up; a give's fly into the palm | 7 | new (Q8) |
| N11 | "attempt" volumes that play an examine (sofa crease, fly cloud, typewriter keys, door latch edge, knuckle rap) | 4, 5, 10, 14, 21, 28 | new (Q3, Q4) |
| N12 | movies on a world-locked screen; the forced turn (0x3f) during black | 1, 37 | new |
| N13 | walk-in by real walking (roomscale moves the playsim position) | 17, 18, 37 | [R] check that the eye anchor moves the player's sector |

**Art measurements** (read once from the install, as wickspots does): M1 the handle on DEMO[96], DEMO[463] and a frame
grip on DEMO[748] (DEMO[90]/[100]/[168] are known, WORLD_DESIGN 1.1); M2 the switch on DEMO[505] in world height (the
`#385` box gives the texels); M3 the scrapbook on DEMO[214] in desk-top coordinates (the `#51` box); M4 the page's top
edge on face 21 under `#112`'s ramp; M5 the bow and bit of ADEMO[131] for the pinch and the lock; M6 the hall table
top (z 74 from the objects; whether it is a mid-platform); M7 the pentagram rims (DEMO[4207], DEMO[4208], ADEMO[232]);
M8 the sofa crease volume from the mesh. Wick points are done (OPENABLES 6).

**Hand poses** (from the owner's FBX take; walk it with `tools/fbxload_test/walk_sheet.py`, don't ask which frame):
P1 a lateral key pinch (thumb pad on the side of the curled index) for the brass key and the silver key, `fbx_pinch`
(694) as fallback; P2 `fbx_pinch_tight` (312) checked for a paper edge; P3 a book held open across both palms
(`fbx_relaxed` 154 or `fbx_cup` 985, to be checked); P4 a knuckle rap (`fbx_halfcurl` 193) wired as a state without
the grip; P5 `fbx_wrap_half` wired as the door-lever target; P6 `fbx_curl` (206) for the lamp bail (already in
roth_handhold.h); P7 `fbx_pinch` for the match.

---

## 6. Open questions for the owner

**Owner's answers (2026-11-13 evening):**
- **Q1 / the match: strike a real match.** Strike on the box's side; a lit match in the off hand for 20 s; its flame
  lights the wicks. The matchbox stays the right-hand item, so the use still carries item 16.
- **Q2 / the Lamp: on a hip hook, grabbed when needed.** It lights the area around the player from the hip; grab it to
  hold it up.
- **Q4 / "a physical attempt plays the examine line": YES, as a pattern** everywhere the original has an examine line
  for something that can be physically tried. No new outcome, only the original's line, plus a haptic.
- **Q8 / the Book: YES.** Opens in both hands with its close-up film beside it; the silver key drops into the palm at
  the original's first-look give, then goes to the inventory as in the original.

- **Q3 / knock to examine: YES.** Two light raps of an open, empty hand play the original's examine line (plus a light haptic).
- **Q5 / mirrors: the lead's call (owner: "a live reflection would look cool but only show hands; the original would
  show the player sprite").** Decision: keep the original's painted, breakable glass for now (1:1). Revisit a
  reflection only if a full body (RS_Avatar) is ever in the game.
- **Q6 / punching: physical melee, from RSQ Melee's idea** (github.com/presidentkoopa/RSQ_UBQSeries, Melee/) without
  its combos, uppercut, slap or headlock. A fist swung into a monster at speed fires the original's attack where the
  fist lands, with the ORIGINAL's damage (speed only decides whether it counts); a punch into a switch or door is the
  original's use. **Physical only: the grip on empty air no longer punches** (it was built as the original's
  secondary-on-nothing in 8917f41d; to be replaced).
- **Q7 / the forced turn: snap during the black frames.**
- **Q9 / ward tingle: YES**, haptic only.
- **Q10 / the niche flash: YES**, a real dynamic light pulsing with the original's three light pulses.
- **Q11 / left-handed: dedicated left-hand support LATER** (owner). Until then the item is the right-side hand.
- **Q12 / a picked-up item shows in the item hand: YES**, when the original selects it into the right hand.
- **Q13 / tall wicks: point the lit match at a wick in reach + grip lights it** (seated and shorter players).
- **Q14 / far things (the casements): the pointer is fine.**
- **Darkness for chapter 1: NONE for now** (owner, after the darkness captures): the original's lighting. The pass stays
  available (`roth_darktest`).
- **Candle light strength: judge in the headset** (current: 0.30 x (1, 0.56, 0.22) per wick, radius 56 x sqrt(wicks)).


- **Q1** The match: keep the matchbox in the item hand (as built) and strike and hold the match with the off hand?
- **Q2** The Lamp: start it on a hip hook or in the off hand, and does the off hand holding it give up its grip-use?
- **Q3** Knock-to-examine (two light raps of an empty, relaxed hand) on armour, doors and tables: yes or no?
- **Q4** "A physical attempt plays the examine line" (sofa crease, typewriter keys, fly cloud, door latch edge): approve
  as a pattern?
- **Q5** Mirrors: keep the original's painted glass, or a live reflection (invented presentation)?
- **Q6** A punch: fire the original's attack when the fist arrives at a surface, or at the grip press as built?
- **Q7** The forced turn after card 2 (0x3f): a snap during the black frames, OK?
- **Q8** The Book: open in both hands with its film on a panel beside, and the silver key into the palm on the
  original's first-look give: yes or no?
- **Q9** A faint tingle haptic as a palm nears a glowing ward (haptic only): yes or no?
- **Q10** The niche's flash (light group 46): also a small dynamic light, like the candles' growing light?
- **Q11** Left-handed play: a setting that mirrors which physical hand carries the original's right-hand item?
- **Q12** After a pickup the selected item stays visible in the item hand (book, key, note), not only in the panel?
- **Q13** Seated players and the 1.87 m tall-candle wicks: is the matchbox touch on the stem or point + grip enough?
- **Q14** The casement leaves beyond the inner window: pointer only in chapter 1, OK?

---

## 7. Verified versus reasoning only

**Verified** (how):
- The chapter 1 area, its 114 walked and 34 touchable sectors, its two rooms joined only by doors s30/s44, and that the
  Study (s266), the House Map, the Ace of Spades, the letters, the serpents, the masks, the Shield and the Sword are
  outside it: `openables -area` (this commit) on the retail install, plus the same walk with the key and no stop.
- Every record, chain, box, delay, count, flag, item and line quoted: `openables` (full and `-area`), `world_census
  -chain`, `chapter_census -map STUDY1`, `dbase100_dump --action`, `item_dossier` on the retail install.
- The lantern sectors (88 of 112 walked) and the light groups the records ramp: `openables -area` LIGHT section
  (sector byte +0x0a bit 1; the floods as roth_runtime.cpp CollectGeometryGroup restates them).
- Sizes: art sizes from the packs, mesh extents from `propcheck -mesh`, face lengths and heights from the map; wick
  points from `tools/wickspots`. Metres from the scale formula read in rothmap.cpp:587-608.
- The pictures named (the painting with its two candlesticks, the scrapbook on DEMO[214], the casement DEMO[748], the
  armour with flies, the switch panel, the glow DEMO[4114]): looked at on the contact sheets.
- `#429`'s template is DBASE100 item 34 "Bullet" (AsBullet) and cmd_spawn_object_adv takes the projectile path for
  such a template: record words, item dump, raw_commands.c:4108-4160 read.

**Reasoning only, not seen:**
- That the Matchbox is the selected item after new game (the give order and the select rule, not run).
- The player's walk itself: the tool's rules are approximations (no step-up timing, no platforms, flags not
  evaluated); the critical-path order is a reading of the map and the chains, not a play-through.
- What the shot from the niche does, where it flies; what `#374`'s "extent 0" means; what the 0x03 ramp shows on the
  typewriter; whether the lounge switch sets the lantern bit; which of `#395`/`#397` plays; whether the fists' attack
  reaches a wall; whether NearAct reaches a floor/platform target; whether the casements can be clicked through the
  inner window.
- Metres assume E = 1.65 m and the eye-anchor scale, not judged in the headset.
- Every VR proposal, haptic pattern, ranking and effort estimate.
- Nothing in this file was checked against the running original or seen in our engine.

---

## 8. Second pass (2026-11-13, owner: "take another pass at ch1 if we missed anything")

The first pass walked every record in the area (all 72 trigger records of `openables -area` appear in section 3) [V:
the record list diffed against the entries]. What it did not cover is everything that is not a record: how the player
moves, the gun, the lines, the HUD, saving and dying, and a few objects that are present but never shown.

### 8.1 Moving through the chapter (no record; every chapter inherits it)
- **The original:** walk, strafe, turn, look up/down, jump (A), crouch (Z) [V: GAME_core.md 1, E/input.c:124-135].
  Step-up is `g_max_climb` (65 units; 16 crouched); crouch lowers the eye to 0x30 by 6 a tick, forced under 0x70 of
  headroom; the jump is a 19-phase table [V: GAME_core.md 2.4-2.5, E/player.c].
- **Ours:** `src/roth` has no step, jump or crouch code [V: grep]; the player moves by the base engine's rules
  [R: not compared tick by tick]. GAME_core.md's verdict is to port movement 1:1; that is still open, and it is a
  flat-port item, not a VR one.
- **What chapter 1 needs:** the main stair (s8..s1, s19..s17, 0 → 400), the upper stair (440 → 640), the painting
  niche at 500 above the corridor's 400 (the key is reached from below), the rat on the floor (entry 27, "crouch and
  touch"). Nothing in chapter 1 needs a jump [R: no ledge or gap on the critical path in the area walk].
- **VR questions:** smooth or teleport, how turning works (snap / smooth, and what angle), and whether real crouching
  drives the original's crouch (the eye at 0x30, the 16-unit step limit) or only the head. → questions L1-L3.

### 8.2 The Colt-45 (entry 22 gives it; nothing in chapter 1 to shoot)
- **The original:** the weapon logic is ported and diffed tick by tick: trigger, shot cycle, ammo, reload, recharge,
  damage [V: REALMS_WEAPONS.md, `tools/actors_test` replay mode `weapon`]. A magazine reload is automatic: an empty
  clip with a magazine item in the inventory removes one and refills (`remove_item`, WC:849); the reload picture plays
  instead of a shot [V: GAME_combat.md 2.3]. Out of magazines: message 6 and an automatic switch [V: same].
- **In chapter 1 there is no monster awake** [V: the only monster in the area, ADEMO[271], is hidden until the end
  (`#564`/`#571`)]. The Colt's only targets are the two mirrors, the five windows and the casement leaves (entry 13,
  16, 38; their 0x1a attack records) [V].
- **VR, not yet decided:** the shot leaves from the gun's muzzle along the gun (the hand aims), not from the eye; the
  original's screen cursor is not used for aiming [P]. The reload: the original's automatic swap (1:1) or a
  physical magazine (eject, take one from the belt, insert)? A physical reload is a VR addition; it must still consume
  exactly one magazine item where the original does. → questions W1-W3.
- The fists until the Colt: physical melee (owner's Q6 answer; the gestures agent is building it).

### 8.3 Adam's lines and the subtitles
- **The original:** a line plays DPCM speech with a subtitle panel under the 3D view; movies have timed subtitles;
  a Subtitles toggle is in the options [V: GAME_dialogue.md 1, 3.6; GAME_audio_save_menu.md 4]. A click skips a
  playing line (owner, built in d34fb269).
- **Most entries in section 3 end in one of Adam's lines**, and the chapter has two movie cards (the opening and
  `#299`) [V: section 3].
- **VR, not decided:** where the subtitle panel sits. Head-locked low in view, floating under where Adam looks, or
  on the item hand's wrist? The original's own font and panel art are drawn either way (1:1 rule: its panel, our
  placement). → question S1.

### 8.4 HUD, health, death, saving
- **The original's HUD:** a health bar (a "low" picture under half), a weapon/ammo panel, the held item on the cursor,
  a portrait corner [V: GAME_audio_save_menu.md 4]. The in-game menu (Resume / Load / Save / Settings / gallery) opens
  from the inventory screen; nine save slots and a quicksave [V: same].
- **Chapter 1 can hurt:** a smashed mirror applies damage to the player (0x33), a broken window becomes a hazard face
  (0x35) (section 2.8, entries 13, 38) [V: chains]; nothing else in the area deals damage [R].
- **Death:** a death script, a short wait (Esc skips, a key quick-loads), then the start map and the main menu [V:
  GAME_audio_save_menu.md 4]. 1:1; nothing to design except where the wait's text shows (same answer as S1).
- **VR, not decided:** where the health bar and the ammo panel live (a wrist watch on the off hand, a glance-down
  belt panel, or the original's strip head-locked); a quicksave button. → questions H1-H2.

### 8.5 Present in the area and never shown in chapter 1 (adds to the "asleep" table)
| what | where | why |
|---|---|---|
| ADEMO[43], invisible spawn markers | several sectors | shown only by `#15`, `#207`, `#406`, `#598`, `#599`, `#600` (0x3c); none of these fires in chapter 1 [V: chains] |
| ADEMO[0], developer object id 78 | `#576` | never reachable [V: CHAPTERS finding 8] |
| ADEMO[271], the end creature id 77 | s267 | `#564` / `#571`, the end of the game [V] |

### 8.6 Smaller things the first pass left implicit
- **Looking at a held item.** The trigger on an inventory item examines it (built). Bringing a held item up to the
  eyes could play the same examine [P] → question X1.
- **Sounds:** every sound named in section 3 is the original's, positional as the original pans and fades it
  [V: GAME_audio_save_menu.md 2]; nothing new to design.
- **The lounge's lit room vs the dark rest:** the Lamp's light from the hip (Q2) is what makes 88 of 112 sectors
  readable; with no darkness pass in chapter 1 (owner) the original's sector light stands [V: owner's answer].

---

## 9. Changes made for this chapter

Every change that touches chapter 1, with its decision, commits and evidence, is in
[`docs/VR_CHANGES_LEDGER.md`](VR_CHANGES_LEDGER.md) (the ledger is per change; each row lists the chapters it
reaches). Summary for chapter 1:

| change | state | evidence |
|---|---|---|
| FBX hands, authored poses, holds (copied frames, pinches), item in the right hand | built | engine captures 967e825f |
| trigger = examine, grip = use; pointing at range, touch/grab in reach; no dwell | built | log + captures dd507632 |
| use with the held item (the right-hand item on the world) | built | captures dd507632 |
| inventory by hand (laser, fingertip), pausing the world; a click skips a line | built | captures 75aef0fa, b987b551; log (0 actor ticks paused) |
| 0x0d key-0 fix (a candelabrum's art swap) | built, 1:1 fix | log + captures e20464e7 |
| candles wick by wick, the 15 s catch, one growing light | built | captures acb11adb |
| darkness pass (`roth_darktest`), NOT used in chapter 1 | built, off | captures aa3c465f |
| record surface boxes (N1) | agent running | — |
| physical melee, grip-on-air attack removed, knock, attempts, ward tingle, tall wicks | agent running | — |
| real match, lamp on a hip hook, item shown in hand, the Book, niche flash, forced turn snap | agent running | — |
| movement, Colt aim/reload, subtitles, HUD placement | open (8.1-8.4) | — |
