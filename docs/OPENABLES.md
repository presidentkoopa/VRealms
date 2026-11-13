# OPENABLES: drawers, cupboards, chests, wardrobes and everything else that opens

2026-11-13. Research only, no engine change. Companion to `docs/WORLD_DESIGN.md` (doors are its section 1, candles
its section 2; this file covers the furniture and containers). Part B (where the wicks sit on the candle pictures)
is at the end.

| mark | means |
|---|---|
| **[V: how]** | verified, and how: a record dump from `tools/openables`, a geometry dump (`openables -sector`), a ROTH.C line read, a picture looked at on a contact sheet |
| **[R]** | reasoning only, not seen. Nothing in this file was seen running in the original: "opens on screen" means the data asks the original to move a door leaf, a sector or an object; no oracle frame was taken |

## 0. How the census was made, and how to repeat it

`tools/openables/` (new) prints every scripted record of every map with what it targets and its whole chain:

```
sh tools/openables/build.sh                                  # writes /tmp/openables
/tmp/openables "$ROTH_DATA"                                  # every trigger record, full chains (about 10,000 lines)
/tmp/openables "$ROTH_DATA" -summary                         # one entry per target: pictures, examine line, use chains
/tmp/openables "$ROTH_DATA" -grep drawer,wardrobe,chest      # records whose target says or holds one of the words
/tmp/openables "$ROTH_DATA" -sector CHURCH1 373 374 375      # a sector's box, heights, faces, floor picture, objects
/tmp/openables "$ROTH_DATA" -summary -dump /tmp/wd_art       # every target picture and swapped-in picture (retail art: /tmp only)
```

Targets are the category directories (`world_census` reads the same ones): object click 0x08 (objects whose id
word +0x0e equals the key), wall use 0x18 (faces by the faceID two-hop), floor use 0x19 (sectors by commandID),
and the examine records 0x30 / 0x32 / 0x31. An examine record's word +0x0a is the DBASE100 action it says
(`examine_object_under_cursor` -> `examine_world_object` -> `eval_dialogue_record_by_id(word[rec+0xa])`,
input.c:964, :979-990) [V: read]. A use trigger's +0x0a is its sound + 1 [R: from ROTH_COMMANDS.md, not re-read].

How the openables were found [V: tool output, then pictures looked at]:

1. Every examine and use line was searched for furniture words (drawer, chest, wardrobe, closet, cupboard,
   cabinet, cooker, refrigerator, desk, collection box, sarcophagus, lid, "in here", "nothing in", ...).
2. Every door of `world_census -doors` narrower than 100 or lower than 150 was looked at with `openables -sector`
   (what lies behind it, at what height, holding what) and its pictures were looked at.
3. Every use chain that swaps an object picture (0x0d), a wall picture (0x34/0x0c), a floor picture (0x0a), moves
   a sector (0x07/0x09/0x03), shows or hides objects (0x16/0x3c) or gives an item (0x29) was read against its
   target's pictures, to find containers that open without saying so.
4. Meshes were drawn with `tools/propcheck/proprender` (the chest, the roll-top desk) and measured with
   `propcheck -mesh`.

The opcode handlers named below, ROTH.C (`/home/claude/oracle_build/rothc/roth_c/src/engine/`) [V: read]:

| op | handler | file:line | what it does here |
|---|---|---|---|
| 0x2f | `cmd_open_door` | raw_commands.c:4053 | key 0 = the face just used, else the face by faceID; hands words +0x0a, +0x0c, +0x0e/+0x10 to `register_door_swing`; a leaf swings |
| 0x0d | `cmd_change_object_texture` | raw_commands.c:4520 | key 0: the clicked object's art word := word +0x0c |
| 0x0a | `cmd_change_floor_texture` | raw_commands.c:4356 | a sector's floor picture changes (the item on a shelf disappears) |
| 0x16 | `cmd_spawn_object` | raw_commands.c:622 | per object of the key: toggle the hidden bit (flags & 0x80), or wake its actor |
| 0x3c | `cmd_spawn_object_adv` | raw_commands.c:4108 | spawn or un-hide an object |
| 0x07 | `cmd_change_height` | raw_commands.c:4483 | a sector's floor or ceiling moves between two limits |
| 0x27 | `cmd_if_not_item` | raw_commands.c:4760 | the item test (in hand) |
| 0x29 / 0x2a | `cmd_give_item` / `cmd_remove_item` | raw_commands.c:4799 / :4918 | give / take an item |
| 0x2b | `run_command_dbase100_record` | raw_commands.c:4698 | say a DBASE100 action |
| 0x36 | `cmd_dbase100_if_next_fails` | raw_commands.c:514 | the line said when the next step fails ("Locked.") |
| 0x12 | `cmd_delay_timer` | raw_commands.c:4420 | wait word +0x06 ticks |
| 0x15 | `cmd_count` | raw_commands.c:216 | count to a number |
| 0x17 | `cmd_toggle_command` | raw_commands.c:541 | enable / disable another record |
| 0x26 | `cmd_set_flag` | raw_commands.c:592 | set a DBASE100 flag |
| 0x10 | `cmd_activate_sfx_node` | raw_commands.c:4855 | a sound node |
| use | `activate_targeted_object` | input.c:1026 | the use verb (objects, door leaves); walls/floors through `run_leftclick_object_trigger`, raw_commands.c:2920 |

In the dumps, a 0x2f record's "extent" and "sound" are the tool's names for its words +0x0a and +0x0c, which
`cmd_open_door` hands to `register_door_swing` unread [V]; the names (and "+0x0c is the sound + 1, 0 silent") are
ROTH_COMMANDS.md's [R: not re-derived]. A 0x07 record's "limits" are its words +0x0a/+0x0c doubled (roth_raw.h).

Chain dumps below are `openables` output. The walk lists every record on the NEXT path; it does not follow run-time
branches (a failed 0x27 ends the run in the original; a 0x15 count stops it until the count is reached) [R: the
branch behaviour is from the handlers' comments, not traced].

---

## 1. The counts

| class | pieces | what |
|---|---|---|
| **A. opens on screen: a hinged leaf swings** (0x2f on a small door sector) | **8** (8 leaves) | refrigerator, wall cupboard, wardrobe, two Vicarage wall cupboards, the cabinet in the closet, a small chest, a small cabinet |
| **B. opens on screen: a moving sector** (0x07) | **1** | the Egyptian sarcophagus in the Study (its lid sinks after the seven statuettes) |
| **C. moves on screen, does not open** (objects hidden and shown) | **1** (5 placements) | the Study chest slides 40 units and a door elsewhere opens |
| **D. use only talks, or takes an item, nothing opens** | **2** kinds (3 records) | the church collection box (takes a coin, enables a door elsewhere); the crates (24 placements, 2 records: "Won't budge.") |
| **E. examine only, no use record** | **6** pieces + 5 sarcophagus and 4 tomb groups | roll-top desk, church desk, cooker, CD rack, the closets (drawn open; A6's cabinet is set in one), "nothing down the back"; sarcophagi and tombs in 5 maps. (E4, E7, E8 are the examine records of A1, A3, C1, not more pieces) |

[V: every row is a record dump and a geometry dump below; the pieces were identified by their examine lines and by
looking at their pictures (contact sheet, section 5)]. **Really open on screen in the original's data: 9**
(A + B). **Give an item or say a line only: 2 kinds** (D). Of the 9 that open, **6 hold an item inside**, which is
taken by its own record after the opening (a separate click on the object inside, or a floor use on the shelf).

There are **no drawers that open** anywhere: the three drawer lines are all refusals ("The drawers are stuck.",
"Drawer's locked.") [V: grep over every action line referenced by a record]. No container in the game swaps its
own picture to an open one (no 0x0d / 0x34 / 0x0c on a container target) [V: every 0x0d/0x34/0x0c chain was read
against its target; the 0x0d swaps are candles, levers, a statue, the gramophone, a lamp].

Not counted, though they look like it in the data: the **papers on table tops** that a floor use takes away by
swapping the floor picture (STUDY1 #34, #38, #51, #53, #57; MAS3 #10; CHURCH1 #14) are pickups from a surface,
not containers [V: chains; sector heights 400-480]; normal room doors and gates (WORLD_DESIGN section 1); the
STUDY4 #49 second leaf (s187), a 52 x 120 low door to a 64 x 256 passage, not a cabinet [V: -sector].

---

## 2. Class A: hinged leaves (8)

A closed leaf is no object: the room face shows the leaf's picture. The use record on that face (or a record
elsewhere) runs 0x2f and the original builds a box leaf that swings 90 degrees about its hinge (WORLD_DESIGN 1.1,
`spawn_door_instance`) [V: chains and door rows; R: the swing itself not seen]. Sizes are the door rows of
`world_census -doors`: width x thickness x height in map units; "at +N" = the leaf's bottom above the room's floor.

| # | map, where | leaf sector | size | at | class | use record (chain) | item inside | pictures (outside / inside of the leaf) |
|---|---|---|---|---|---|---|---|---|
| A1 | CHURCH1, kitchen: **refrigerator** | s373 (hinge fffd) | 116 x 6 x 140 | +20 | FREE | #48: 0x2f key 0, word +0x0c = 0 (silent) | Key 218 on the shelf s375 (64 x 116, floor +40): **floor use #28** gives it and swaps the shelf's floor picture | DEMO1[288] / DEMO1[289]; shelf floor DEMO1[286] -> DEMO1[875] |
| A2 | CHURCH1: **wall cupboard** | s557 (fffe) | 42 x 2 x 60 | +120 | FREE | #25: 0x2f key 0, sound word 151 | Magical Torch (item 175), ADEMO[54] obj id 12 in s558 (42 x 18): click #13 gives it | DEMO1[781] / DEMO1[155]; wall DEMO1[688], below DEMO1[893] |
| A3 | VICAR, bedroom s206: **wardrobe** | s209 (fffe) | 70 x 6 x 256 | +0 | FREE | #90: 0x2f face 17, sound word 124 | a green ward DEMO1[4233] (obj id 12) and a hidden red ward DEMO1[4232] (id 24) in s211 (250 x 58), "touch it and it'll take your hand off" | DEMO1[781] / DEMO1[782]; front panel DEMO1[780]; examine #93 "Wardrobe. Standard fitting I suppose." |
| A4 | VICAR, room s167: **locked wall cupboard** | s168 (fffd) | 64 x 6 x 118 | +100 | KEY | #45: 0x12 5 -> 0x36 "Locked." -> 0x27 Key 219 -> 0x26 flag 426 -> 0x2f key 0 | Key 193, ADEMO[254] obj id 13 in s169 (40 x 64): click #100 gives it, flag 281 | DEMO1[757] both sides; below DEMO1[760] |
| A5 | VICAR, room s167: **warded cupboard / hatch** into the recess s122 (76 x 32) | s10 (fffe) | 76 x 12 x 118 | +100 | LATER | live #112: 0x3c shows objs 25, 26 (wards); disabled #74: flag 191 -> 0x2f face 10 | none placed in s122 except a ward DEMO1[4233] at +150 | DEMO1[757] both sides |
| A6 | CAVERNS3: **locked cabinet in the closet wall** | s615 (fffe) | 42 x 2 x 96 | +0 | KEY | #57: 0x12 2 -> 0x36 "Locked." -> 0x27 Key 167 -> 0x26 flag 427 -> 0x2f key 0 | Statuette (item 4), ADEMO[45] obj id 5 in s616 (42 x 38): click #7 gives it | DEMO4[2826] both sides (flipped on one); the closet wall above it DEMO4[2502] |
| A7 | RAQUIA5: **small chest** (a cabinet under a stone alcove) | s420 (fffd) | 64 x 6 x 120 | +0 | FREE | #128: 0x2f face 10, word +0x0a 300 (others 600), sound word 151; #42 (one record for eleven doors) closes it from inside | Vial (206), ADEMO[276] obj id 14: click #130; and ADEMO[56] (id 0) in s448 (62 x 52) | DEMO1[612] both sides; alcove DEMO1[545], inner wall DEMO1[613]; examine #125, #126, #127 "Just a small chest. Hey, there's something in here." |
| A8 | STUDY4: **small cabinet** | s180 (fffd) | 64 x 6 x 120 | +0 | FREE | #49: 0x2f key 0, silent | Vial ADEMO[276] (id 10) and two Cartridges ADEMO[146] (id 11) in s179 (58 x 64) | DEMO[573] both sides; sides DEMO[501]; no examine record |

[V: record dumps (2.1), `-sector` dumps, the census door rows, pictures looked at.] [R: what each leaf looks like
swinging; A5's recess being a cupboard (its walls are the bathroom pictures DEMO1[744]/[745]); A2's leaf showing
DEMO1[781], a 32 x 129-texel picture, on a 42 x 60 face (how it is scaled there was not checked).]

Correction found on the way [V: pictures looked at]: DEMO1[4232] and DEMO1[4233], called "breastplate / armour on
a stand" in WORLD_DESIGN section 7, are the **red and green lemniscate wards** (37 x 18 and 37 x 13 texels), which
fits their examine line "Touch it and it'll take your hand off" and the owner's decision 4 (the Magical Torch shows
the wards).

### 2.1 Chain dumps

```
USEW CHURCH1  #48 op 18 mod 80 box 0 57 0 70 sound 150            the refrigerator door
    target face id 6
      f1847 s374 len 116 floor 20 ceil 160 | across s373 ... mid=DEMO1[289]
      f2662 s540 len 116 floor 0 ceil 400 | across s373 floor 20 ceil 160 mid=DEMO1[288] up=DEMO1[283] low=DEMO1[285]
    #49   0x2f DOOR face-id 0 (the face used) extent 600 sound 0
EXW CHURCH1  #223 op 32 mod 80 box 0 0 0 70 says 279 "Look at the grime on this. Someone call bug hunters. / ..."
USEF CHURCH1  #28 op 19 mod 00 box 0 0 0 0 sound 73              the shelf inside (s375, floor +40, floor pic DEMO1[286])
    target sector id 6
    #170  0x29 GIVE item 218 "Key"
    #227  0x26 set-flag 138
    #29   0x0a floortex sector 6 -> DEMO1[875]
    #450  0x17 toggle record 226
   (examine of sector id 6: 339 "Key. Used to be a tag on it I think. / So what was it doing in the refrigerator?")

USEW CHURCH1  #25 op 18 mod 80 box 0 0 0 81 sound 149            the wall cupboard
      f2734 s553 len 42 floor 0 ceil 800 | across s557 floor 120 ceil 180 mid=DEMO1[781] up=DEMO1[688] low=DEMO1[893]
    #27   0x2f DOOR face-id 0 (the face used) extent 600 sound 151
CLICK CHURCH1  #13 op 08 mod 80 sound 275                         inside it
      obj 558.0 ADEMO[54] at (3712,1548) z 120 (floor +0)
    #318  0x29 GIVE item 175 "Magical Torch"

USEW VICAR    #90 op 18 mod 80 box 0 0 0 128 sound 135            the wardrobe
      f995 s206 len 70 floor 420 ceil 760 | across s209 ... mid=DEMO1[781]
      f1022 s211 len 70 floor 420 ceil 760 | across s209 ... mid=DEMO1[782]
    #92   0x2f DOOR face-id 17 s206 s211 extent 600 sound 124

USEW VICAR    #45 op 18 mod 80 box 0 0 0 59 sound 149             the locked wall cupboard
      f821 s167 len 64 floor 420 ceil 820 | across s168 floor 520 ceil 638 mid=DEMO1[757] up=DEMO1[261] low=DEMO1[760]
    #155  0x12 delay 5 ticks
    #57   0x36 talk 438 "Locked."
    #152  0x27 if-not-item 219 "Key" (flags 03)
    #156  0x26 set-flag 426
    #46   0x2f DOOR face-id 0 (the face used) extent 600 sound 151
CLICK VICAR    #100 op 08 mod 80 sound 274                         inside it (s169)
    #153  0x29 GIVE item 193 "Key"
    #101  0x26 set-flag 281

USEW VICAR    #112 op 18 mod 80 box 0 0 0 59                      the warded hatch, live record
    #118  0x3c SPAWN/SHOW obj 25 words 0040 0000
    #134  0x3c SPAWN/SHOW obj 26 words 0040 0000
USEW VICAR    #74 op 18 mod 88 DISABLED sound 135                 its opener, enabled later
    #95   0x26 set-flag 191
    #75   0x2f DOOR face-id 10 s122 s167 extent 600 sound 151

USEW CAVERNS3 #57 op 18 mod 80 sound 149                          the locked cabinet in the closet
      f2856 s614 len 42 floor 720 ceil 1100 | across s615 floor 720 ceil 816 mid=DEMO4[2826] up=DEMO4[2502]
    #123  0x12 delay 2 ticks
    #53   0x36 talk 438 "Locked."
    #52   0x27 if-not-item 167 "Key" (flags 03)
    #130  0x26 set-flag 427
    #58   0x2f DOOR face-id 0 (the face used) extent 600 sound 138
CLICK CAVERNS3 #7 op 08 mod 80 sound 53                           inside it (s616)
    #8    0x29 GIVE item 4 "Statuette"
    #14   0x10 sfx-node 4

USEW RAQUIA5  #128 op 18 mod 00 box 0 200 0 200 sound 149         the small chest
      f1198 s245 len 64 floor 640 ceil 1140 | across s420 ... mid=DEMO1[612]
    #129  0x2f DOOR face-id 10 s245 extent 300 sound 151
EXW RAQUIA5  #127 says 262 "Just a small chest. Hey, there's something in here. / What is it? / Just a small chest. / Empty now."
CLICK RAQUIA5  #130 op 08 mod 00 sound 275                        inside it (s448)
    #81   0x29 GIVE item 206 "Vial"
    #132  0x26 set-flag 152

USEW STUDY4   #49 op 18 mod 00 box 0 0 0 108 sound 149            the small cabinet (and the low passage door s187)
      f925 s181 len 64 floor 480 ceil 800 | across s180 ... mid=DEMO[573]
      f956 s188 len 52 floor -160 ceil 320 | across s187 ... mid=DEMO[573]
      f1019 s200 len 52 floor -160 ceil 320 | across s187 ... mid=DEMO[573]
    #51   0x2f DOOR face-id 0 (the face used) extent 600 sound 0
```

---

## 3. Class B and C: the sarcophagus and the chest (STUDY1)

**B1. The Egyptian sarcophagus** (face id 31; the lid is the LOWER piece DEMO[657] between room s221's floor (440) and
the thin sector s350's floor; behind it the transparent mid DEMO[630], cobwebs). s350 is 128 x 2; its floor moves
between 656 and 442 [V: -sector, chain]. A use on it only says "Won't open." (#304). It opens when the seventh
statuette is placed on the floor plates around it (USEF #119, #124-#129, each: take a Statuette, show it, count to 7,
then "It's... it's opening! Is that a ladder down there?" and 0x07 on s350) [V: chains]. [R] s350 starts at 656 by
the load-time rule (`ApplyLoadTimeHeights`, roth_raw.h, a 0x07 record with modifier 0x02 takes its +0x0c limit), so
the lid stands 216 tall; lowering s350's floor to 442 makes the lid sink into the floor and opens the way down.
Size: 128 wide, lid 216 high (DEMO[657] is 64 x 109 texels).

```
USEF STUDY1   #119 op 19 mod 80 box 27 61 45 80 sound 126         one of the seven statuette plates
    #545  0x36 talk 590 "Doesn't appear to be working."
    #222  0x27 if-not-item 4 "Statuette" (flags 03)
    #221  0x2a take item 4 "Statuette"
    #120  0x16 SPAWN/SHOW obj 22
    #156  0x15 key 7 args 0028 0007 0001 0007                     count to 7
    #239  0x2b talk 23 "... Something's happening to the Sarcophagus. It's... it's opening! / Is that a ladder down there?"
    #157  0x07 HEIGHT sector-id 17 [ s350 ] fl 24 mod 02 limits 442 656 step 32
USEW STUDY1   #304 op 18 mod 80 sound 275                         a direct use
    #305  0x2b talk 115 "Won't open. I can't see any hinges or anything. / I tried to open it earlier. ..."
```

**C1. The old chest** DEMO[4152], a mesh 108 x 68 x 70 (propcheck -mesh) [V], in STUDY1 sector 109. Five placements
of the same mesh, ids 10, 27, 28, 29, 30, at x = -1542, -1532, -1522, -1512, -1502 (y 2140, z 0); four start
hidden [V: world_census -kind]. A use runs #88: hide 10, show 27, wait 30, hide 27, show 28, ... show 30, wait 60,
sound node 16, open door face 2 (a locked DEMO[100] door, s501), flag 28, "What was that? Sounds like you just opened
something.", enable #86 and #408 [V: chain; 0x16 toggles the hidden bit, raw_commands.c:622-672]. So the chest
**slides 40 units in four 10-unit steps** and reveals nothing itself; it does not open. Examine (#256 on id 10, #25 on
id 30): "Old chest. Nothing in it. / Sometimes these things have hidden compartments." WORLD_DESIGN 6.1 says "the
five are shown in turn"; the x positions show that the showing is a slide [V: positions].

```
CLICK STUDY1   #88 op 08 mod 81 sound 121
    #97   0x16 SPAWN/SHOW obj 10        #98   0x16 SPAWN/SHOW obj 27        #101  0x12 delay 30 ticks
    #99   0x16 SPAWN/SHOW obj 27        #100  0x16 SPAWN/SHOW obj 28        #103  0x12 delay 30 ticks
    #102  0x16 SPAWN/SHOW obj 28        #104  0x16 SPAWN/SHOW obj 29        #105  0x12 delay 30 ticks
    #106  0x16 SPAWN/SHOW obj 29        #107  0x16 SPAWN/SHOW obj 30        #109  0x12 delay 60 ticks
    #110  0x10 sfx-node 16
    #84   0x2f DOOR face-id 2 s501 extent 600 sound 124
    #284  0x26 set-flag 28
    #285  0x2b talk 94 "What was that? / Sounds like you just opened something."
    #10   0x17 toggle record 86          #423  0x17 toggle record 408
```

---

## 4. Class D and E: the ones that do not open

| # | map, what | target | records | what happens | size / pictures |
|---|---|---|---|---|---|
| D1 | CHURCH1 **collection box** | sector id 9 (s77) + face id 26 | USEF #77; examine #173, #174 "Collection box. With a few coins in it." | 0x36 "Doesn't appear to be working." / 0x27 Coin 185 / 0x2a take it / 0x17 enable #32 (the opener of door face 22) and #381. Takes a coin, opens a door elsewhere | DEMO1[136], [140], [141] |
| D2 | STUDY3 **crates** DEMO[4150] (mesh 108 x 68 x 66) | obj ids 2, 18 (24 placements) | CLICK #171, #174 | 0x2b 656 "This isn't gonna move in a hurry. / Won't budge." | mesh |
| E1 | STUDY1 **roll-top desk** DEMO[4113] (mesh 114 x 68 x 80) | obj id 1 | examine #288 | "The drawers are stuck. / The canopy's locked too. And the lock's been melted." | mesh (rendered, section 5) |
| E2 | CHURCH1 **desk** (built of sectors, top at +72) | face id 24, sector id 14 | examine #167, #168 | "Papers and things. Nothing to give anything away. / Drawer's locked." | drawers on the lower piece DEMO1[731]; DEMO1[730], [732], [733] |
| E3 | CHURCH1 **cooker** (sectors, top at +80) | face id 20, sector id 20 | examine #165, #236 | "Old cooker. / Spotless." | DEMO1[290], [291]; DEMO1[258] |
| E4 | CHURCH1 **refrigerator, outside** (the door A1 and the wall beside it) | face ids 6, 21 | examine #223, #166 | "Look at the grime on this." | DEMO1[288], DEMO1[287] |
| E5 | CHURCH1 **CD rack** | face id 55, sector id 19 | examine #221, #222 | "There's a CD in here. / Can't see any others." | DEMO1[721], [722] |
| E6 | CAVERNS3 **closets** (drawn already open, hangers showing) | face id 2 | examine #17 | "Empty closets." | DEMO4[2502], [2507] 88 x 191; sides DEMO4[2504], [2505] |
| E7 | VICAR **wardrobe front panel** (beside the opening leaf A3) | face id 16 | examine #89 | "Wardrobe. Standard fitting I suppose. / The claw marks aren't though!" | DEMO1[780] |
| E8 | STUDY1 **chest, last position** (part of C1, counted once there) | obj id 30 | examine #25 | as C1 | DEMO[4152] |
| E9 | CHURCH1 **"Nothing down the back."** (sofa or cushions; sectors) | face ids 64-66, sector 22 | examine #264-#267 | "Nothing down the back." | DEMO1[708]-[713] [R: what it is] |
| E10 | **sarcophagi**: AELF (face 8), CAVERNS2 (face 5), MAS6 (face 3), MAS7 (face 5), MAUSO1EA (faces 1-4) | | examine #108, #2, #40, #40, #102, #103, #106, #107 | "Sarcophagus. Cold to touch." | sectors, lower pieces DEMO4[294] etc. |
| E11 | **tomb lids**: AELF s10, CAVERNS2 s5, MAS6 s7, MAS7 s11 | sector ids | examine #107, #13, #62, #39 | "Depiction of a knight. No name. / Doubt we could get the lid off." | floors |

[V: record dumps; mesh sizes from propcheck -mesh; pictures looked at.] [R: E9's identity.]

---

## 5. Contact sheets (retail art: /tmp only, never committed)

- `/tmp/wd_openables_A.png` (half size: `/tmp/wd_openables_A_half.png`): for each kind the leaf outside and inside,
  the wall round it, the floor picture before and after (fridge shelf), and what lies inside. A leaf has no "open"
  picture: open, it is the same picture on a swung box leaf.
- `/tmp/wd_openables_meshes.png`: the chest DEMO[4152] and the roll-top desk DEMO[4113] drawn by proprender
  (STUDY1, from (-1542,1960) and from both sides of (318,340)).
- `/tmp/wd_doors_sheet.png`, `/tmp/wd_sheet2.png`: the candidates looked at and rejected (gates, levers, glass
  doors, statue niches, plaques).

---

## 6. Part B: where the wicks are on the candle pictures

`tools/wickspots/` (new) recomputes everything here; its output is `tools/wickspots/wickspots_retail.txt`.

```
sh tools/wickspots/build.sh                          # writes /tmp/wickspots
/tmp/wickspots "$ROTH_DATA"                          # the table below
/tmp/wickspots "$ROTH_DATA" -sheet /tmp/wd_wick      # per pair: unlit aligned | lit | change mask, 4x (retail art)
```

**Method** [V: tool]. The unlit/lit pairs are found in the maps, not typed in: every object click chain's 0x0d with
key 0 (cmd_change_object_texture's immediate path, raw_commands.c:4520-4549) gives (clicked object's art, new art).
Both pictures are billboards drawn bottom-centre on the anchor (`proprender` drawBillboard: units per texel 2 or by
IM_HALF_SIZE; bottom = z - shift, shift = (modifier & 0x0f) * 2 + the entry's anchor y), so the unlit picture is
mapped into the lit picture's texel grid by centre and bottom. A texel is "changed" when it differs from the unlit
one in any lit frame. **Flames** are the texels that flicker between the lit picture's own frames (the lit
pictures are 4- or 9-frame animations); flicker groups whose boxes overlap are one flame (sparks). The **wick
point** is the middle of the flame's lowest row, one texel down: where flame meets wick. Texel coordinates: x right,
y down from the LIT picture's top-left. World: units from the object's anchor, x across as drawn, h up.

**Rotations: none.** Every candle kind is a Plain (unlit) or Animated (lit) entry, not a directional one, and no
placement of any of them is drawn fixed-angle (renderType & 0x80): one picture for every view angle [V: entry kinds;
placements counted by the tool, 0 fixed of 21 / 2 / 10 / 1 / 12 / 32 / 8 / 19 / 18 / 18 / 11 / 3 / 1 / 86]. The one
exception is DEMO1[4104]: 1 of its 4 placements is fixed-angle (still one picture).

### 6.1 The four scripted pairs [V: tool]

| kind | unlit picture | lit picture (frames) | unlit -> lit texel offset | flames |
|---|---|---|---|---|
| DEMO[4098] -> DEMO[4148], five-candle floor candelabra (CAVERNS, STUDY1-4: 7 records) | 55 x 164 texels, 1 u/t, shift 4 | 55 x 171 (4) | (+0, +7) | 5 |
| DEMO[4186] -> DEMO[4187], tall one-candle stand (STUDY1 #4) | 55 x 163, 1 u/t, shift 0 | 55 x 171 (4) | (+0, +8) | 1 |
| DEMO[4124] -> DEMO[4190], short candlestick (STUDY2 #153, STUDY4 #29, TEMPLE1 #73) | 18 x 53, 1 u/t, shift 4 | 18 x 59 (4) | (+0, +6) | 1 |
| DEMO1[4185] -> DEMO1[4186], small candle (VICAR1 #28) | 10 x 17, **2** u/t (20 x 34 units) | 16 x 24 (4), 2 u/t (32 x 48) | (+3, +7) | 1 |

Per flame, in LIT texels (subtract the offset for the unlit picture):

| kind | flame | box x0..x1, y0..y1 | centroid | wick point | wick, unlit texels | wick, world (x, h) |
|---|---|---|---|---|---|---|
| DEMO[4148] | 1 (left) | 2..5, 18..25 | (4.3, 22.1) | (4.5, 26) | (4.5, 19) | (-23, 141) |
| | 2 | 12..15, 5..12 | (14.3, 9.1) | (14.5, 13) | (14.5, 6) | (-13, 154) |
| | 3 (centre) | 26..29, 0..7 | (27.7, 4.1) | (27.5, 8) | (27.5, 1) | (0, 159) |
| | 4 | 38..41, 5..12 | (40.3, 9.1) | (40.5, 13) | (40.5, 6) | (+13, 154) |
| | 5 (right) | 49..52, 18..25 | (50.7, 22.1) | (50.5, 26) | (50.5, 19) | (+23, 141) |
| DEMO[4187] | 1 | 26..29, 0..7 | (27.7, 4.1) | (27.5, 8) | (27.5, 0) | (0, 163) |
| DEMO[4190] | 1 | 7..10, 0..7 | (8.7, 4.1) | (8.5, 8) | (8.5, 2) | (-0.5, 47) |
| DEMO1[4186] | 1 | 6..9, 0..7 | (7.8, 4.0) | (7.5, 8) | (4.5, 1) | (-1, 32) |

Every flame is 4 x 8 texels (17 flickering texels; 15 on DEMO1[4186]).

### 6.2 Lit kinds with no scripted partner [V: tool, flicker only, no diff]

| kind | picture | flames: wick point (lit texels) -> world (x, h) |
|---|---|---|
| DEMO1[4103] lit candelabra (CHURCH1, RAQUIA5) | 55 x 171, 1 u/t, shift 0, 4 frames | same texel boxes as DEMO[4148]: (4.5,26) (14.5,13) (27.5,8) (40.5,13) (50.5,26) -> (-23,145) (-13,158) (0,163) (+13,158) (+23,145) |
| DEMO2[4099] three-flame stand (ANUBIS) | 24 x 96, 2 u/t, 4 frames | boxes 0..5 x 9..21, 8..13 x 0..10, 18..23 x 9..21 (6 x 13, 6 x 11, 6 x 13): (2.5,22) (10.5,11) (21.5,22) -> (-19,148) (-3,170) (+19,148) |
| DEMO1[4104] candle (CHURCH1, RAQUIA5) | 16 x 47, 1 u/t | 6..9 x 0..7: (7.5,8) -> (-0.5, 39) |
| DEMO4[4215] (CAVERNS3) | 13 x 47, 2 u/t, 9 frames | 5..7 x 2..12 (3 x 11): (7.0,13) -> (+1, 68); plus a flickering glow 4..8 x 14..16 under it, not a flame |
| DEMO4[4258] (CAVERNS3) | 13 x 50, 2 u/t, 9 frames | 5..7 x 1..11: (7.0,12) -> (+1, 76); glow 4..8 x 13..15 |
| DEMO[4135] (CAVERNS, OPTEMP1) | 16 x 24, 2 u/t | 6..9 x 0..7: (7.5,8) -> (-1, 32) |

The unlit decoration kinds DEMO[4117], DEMO1[4131], DEMO4[4232] and the empty candelabra DEMO[4189] have no lit
partner in the data and no animated picture of the same width in their pack: not measured. [R] DEMO4[4224] differs
from DEMO4[4215] in 23% of texels and DEMO[4189] from DEMO[4148] in 21%: possible relatives, not partners.

### 6.3 Is per-flame compositing from the lit picture clean? No [V: tool, change masks looked at]

Between unlit and lit, more changes than the flames, in every pair (`OTHER changed texels` in the tool output):

| pair | changed texels | flicker | other | where the others are |
|---|---|---|---|---|
| DEMO[4098] -> [4148] | 258 | 85 | 168 in 20 groups | the **candle top under each flame** (wick and molten top drawn lit: 19, 17, 12, 21, 16 texels, starting on the wick row); the **cup and drip-pan rims recoloured orange** (rows 21, 28-29, 38-40, 1-11 texels each); 6 grey texels round the centre cup (rows 33-34); **6 grey texels on the stem, x 26-27, y 115-119**, unexplained |
| DEMO[4186] -> [4187] | 44 | 17 | 26 in 5 | candle top (12), cup rim (7 + 3, rows 21-23), 4 texels at row 39 |
| DEMO[4124] -> [4190] | 31 | 17 | 13 in 1 | candle top only; 2 unlit texels vanish (the only pair where any do) |
| DEMO1[4185] -> [4186] | 33 | 15 | 15 in 3 | candle top (9), cup rim (3 + 3, row 21) |

So a flame lit on its own needs its flame box **and** the candle-top patch directly under it (both assignable to
the flame by column); the orange rims are the lit picture's glow on the cups, one cup per candle on the candelabra
(assignable by column) [R: the assignment]; the stem and centre-cup texels belong to no flame. No unlit texel falls
outside the lit picture in any pair; every flickering texel is also a changed texel.

**Transparency, found on the way** [V: image_type read]: DEMO[4148], DEMO1[4186], DEMO1[4103] and DEMO2[4099] carry
image_type 0x13, whose bit 0x02 roth_das.h calls IT_ZERO_OPAQUE. Read that way they are black boxes. The tool reads
index 0 as a hole (the engine's object-art rule, roth_texture.h, whose comment says "No object image in the retail
packs sets IT_ZERO_OPAQUE"; these four do). Which rule ROTH.C's sprite path applies to them was not read [R].

### 6.4 Contact sheets (retail art: /tmp only)

- `/tmp/wd_wick_sheet_full.png`: the four pairs, each unlit (aligned) | lit frame 0 | change mask (white changed,
  amber changed and flickering), 4x; flame boxes green, centroids blue, wick points red, other changes magenta.
- `/tmp/wd_wick_sheet_tops.png`: the same, the top 50 texels only.
- `/tmp/wd_wick_litonly.png`: the lit kinds with no partner, flames and wick points marked.
- `/tmp/wd_wick/*.ppm`: the single sheets as the tool wrote them.
