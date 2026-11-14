# VR design dossier, chapter 2 "Signs and Portents" (the Study, the cellar stair, MAS3, MAUSO1EA, MAS4, MAS6, MAS7, MAUSO1EB, AELF)

2026-11-13. Second chapter dossier, written to the chapter 1 template (`docs/VR_CHAPTER01_SHADOWS.md`). Design only:
nothing here is built for chapter 2, and nothing here was seen in the headset or in the running original.

**Marks** (as chapter 1). **[V: how]** verified, and how (a tool run on the retail install, a record or ROTH.C line
read). **[R]** reasoning only, not seen. **[P]** a design proposal. Every proposal ends in exactly one of the original's
clicks (examine, use, attack, the inventory's own clicks) and adds no outcome, item or text.
**Tags.** **A** = drives only the original's screens and clicks through paths that exist; **B** = a VR presentation or
a new gesture layered on top, which waits for the owner's go. **Effort** S = days, M = one to two weeks, L = more [R].

**Owner decisions applied here as patterns** (VR_CHANGES_LEDGER.md, VR_CHAPTER01_SHADOWS.md 6 and 8.7; not asked
again): trigger = examine, grip = use; pointing at range (< 268 planar units), touch / grab in reach; physical melee
only, with the original's damage (a grip on nothing does not attack); two light raps = examine; a physical attempt plays
the original's examine line; candles wick by wick in any order with the struck match, 15 s catch, one growing light;
tall wicks: point + grip; the Lamp on the hip hook, its light only in lantern sectors; the item shown in the item hand;
an item brought to the eyes plays its examine; world-locked movies; the forced turn snaps at black; ward tingle;
the off-wrist mount (subtitles, health, low-health cue); guns aim along the gun; real-parts reload LATER; left-hand
support LATER; no darkness pass unless the owner says; mirrors keep the original's glass; quicksave / quickload.

---

## 0. How this was made, and the scale

**Arrival.** Card 2 (`STUDY1 #299`) runs act178, which calls `#449`: a warp to STUDY1 @34, in place, and a forced
turn. A warp with a key arrives at the first sector, in file order, whose command id is the key (`FindArrivalSector`,
roth_gameflow.cpp; map_load.c:548-556). For id 34 that is **s252**, the Study [V: tool]. A warp with key 0 into
another map keeps the player's x, y and z (ChangeMap relocates only when the key is not 0: roth_gameflow.h `WarpKind`,
map_load.c:625-663) [V: read]. Every chapter 2 edge but the pit (@13) and the portal (@29) is key 0.

**Tool changes (this commit).**
- `tools/openables -area` takes `-at ID` (a keyed warp's arrival, from the paused draft f759f420), `-xy X,Y,Z` (a
  key-0 arrival: the sector holding that point) and `-setfloor ID:H` / `-setceil ID:H` (by hand, a 0x07 mover the walk
  does not apply, printed in the output).
- `tools/chapter_census -mapall NAME`: `-map` plus every other unit of the map (examines, ambient, transitions).
  `chapter_census -all` output is unchanged [V: diffed against `chapters_retail.txt`, 0 lines differ].

```
sh tools/openables/build.sh; sh tools/chapter_census/build.sh /tmp/cc
H=1,4,9,11,15,16,17,18,21,24,25,26,27,28,29,36,48,68,70,117,118,119,273,13,22      # the items in reach by then
/tmp/openables "$ROTH_DATA" -area STUDY1 -at 34 -have 1,15,16,68,21,9,17,70          # the Study, the stair down
/tmp/openables "$ROTH_DATA" -area MAS3 -xy -1250,-2528,0 -have $H -setceil 1:-1000   # MAS3, Orb door opened by hand
/tmp/openables "$ROTH_DATA" -area MAUSO1EA -xy 1598,2360,-1460 -have $H              # from MAS3 #15 (and 5 more arrivals)
/tmp/openables "$ROTH_DATA" -area MAS4 -xy -2476,3980,-1460 -have $H                 # from MAUSO1EA #53 (s189)
/tmp/openables "$ROTH_DATA" -area MAS6 -xy -1536,4992,-1380 -have $H                 # from MAUSO1EA #55 (s401)
/tmp/openables "$ROTH_DATA" -area MAS7 -at 13 -have $H                               # the pit; also -xy -1280,8576,-1500
/tmp/openables "$ROTH_DATA" -area MAUSO1EB -xy 2928,6272,-1780 -have $H              # from MAUSO1EA #65 (s393)
/tmp/openables "$ROTH_DATA" -area AELF -xy 1504,9888,-1680 -have $H,12,14            # from MAUSO1EA #61 (s103)
/tmp/cc/chapter_census "$ROTH_DATA" -mapall MAS3                                     # every unit, per map
```

[V: run this session.] Coverage: every unit `chapter_census -mapall` lists for the seven mausoleum maps was matched
against the records the walks print. The ones no walk reaches are records keyed to ids nothing carries (not-placed
objects, sector ids with no sector: the census prints no `(sec N)` for them, chapter_census.cpp:507-508), three AELF
timers and two disabled texture animations; all are in the appendix (section 10). STUDY1's 225 units were filtered to
the Study, the passage and the two chapter-1-area records chapter 2 switches on (75 records).

Also used: `world_census -chain` (raw chains), `dbase100_dump --action N` (lines, choices, cutscene numbers: op 0x07
plays entry operand − 1, dbase100_dump.cpp:353), `item_dossier --item N`, `openables -map` and `-sector`.

**The scale** (as chapter 1, rothmap.cpp:587-608; for an eye at E = 1.65 m) [V: read; R: not judged in the headset]:
**87.3 units = 1 m up, 100 units = 1 m across**; the use reach (268 planar units) is 2.7 m.

**Contact sheets:** none were made for this chapter (retail art would stay in /tmp in any case). Picture names below
are the tools' resolved names; what a picture shows is [R] unless an entry says it was looked at.

---

## 1. What chapter 2 is

**Where.** From the warp into the Study (s252) to card 3. Card 3 is STUDY1 `#75` / `#354`: a use on the bookcase door
from the stair side, with flag 82 (meeting Aelf, AELF `#2`) set and flag 55 clear [V: chains]. The area [V: walks]:

| area | map, sectors | floor (units) | lantern sectors walked | what is there |
|---|---|---|---|---|
| The Study | STUDY1 R0, 40 sectors (s252, s213-s227, s316-s320, s361, s362, s385, s401-s403, s413-s427) | 400; the dais 440; tables 480 | **0 of 40** (lit room) | statuettes, the sarcophagus and its plates, masks, Shield, Sword, letters, clock, gramophone, candelabra, the roll-top desk |
| The passage behind the bookcase and the stair down | STUDY1 R1, 28 sectors (s454 ... s446) | 400 → −400 | 28 of 28 | the first creature, the warp to MAS3 |
| The cellars | MAS3 (209 sectors with the Orb door opened by hand) | 0 → −1640 | 203 of 209 | creatures, a spare Colt, the Orb and its door, Parchment, Map, Shotgun, the Shrive recess |
| The mausoleum (hub) | MAUSO1EA (369 + 14 sectors) | −1300 → −1820 | 184 of 379 | levers, the lock lever, the walkway, the words wall, the pit trap, exits to every other map |
| The store rooms | MAS4 (218) | −1380 → −1500 | 216 of 216 | a lever door, Key i22 |
| The gauntlet | MAS6 (44 from the arrival) | −1380 → −1500 | 26 of 44 | three hand plates, a lever, the words, a portal |
| The throne room and the pit | MAS7 (43 pit + 158) | −1180 → −2280 | 183 of 201 | the throne (BRANDED), a chain wall |
| The winged-statue temple | MAUSO1EB (255) | −1200 → −2220 | 69 of 255 | the altar: the Staff and the Shrive |
| Aelf's crypt | AELF (372) | −1560 → −1680 | 231 of 372 | Florentine's trap, the Shrive door, Aelf |

[V: `openables -area` runs above; lantern = sector byte +0x0a bit 1, as chapter 1.] The Study itself is the first lit
room the player keeps returning to; everything below it is the Lamp's.

**The critical path** [V: chains and walks; R: the play order, which follows CHAPTERS.md's walkthrough reading]:
1. The Study: take what is there (statuettes, Shield, Sword, masks, letters: none is tested on the way down) → the
   bookcase doors (`#353` / `#420`, free) → the passage: a creature appears (`#362`) → the stair falls to −40 →
   `#404`: MAS3, position kept.
2. MAS3: walk-in creatures; the Orb (`#1`) into the Orb door's aperture (`#4`, the Orb in hand) → beyond: Parchment
   (`#10`), Map of the Mausoleum (`#111`), Shotgun (`#107`) → `#15`: MAUSO1EA.
3. MAUSO1EA → `#53` MAS4: a lever (`#3`) opens the store room; Key i22 (`#7`) → back → MAUSO1EA's lock lever `#76`
   with i22 **in hand** opens s193 (flag 428) → the walkway (three Creator warnings `#40`, `#44`, `#46`; the third
   hurts unless flag 62) → `#65`: MAUSO1EB.
4. Flag 62 comes from MAS7's throne (`#7` / `#33` / `#14`: sitting down plays BRANDED). MAS7 is reached by MAUSO1EA
   `#57` or by falling through the pit (`#20`). Flag 62 skips the walkway's damage and opens Florentine's dialogue.
5. MAUSO1EB: the altar (`#4`, `#16`, `#21` → act174): "Take the artefact" gives **the Shrive i14 and the Staff i12**
   (SHRIVEC), and two winged statues come alive (`#11`).
6. MAUSO1EA `#61` → AELF: walking in with the Shrive **carried** springs Florentine's trap (`#3`: his dialogue if flag
   62, wards, three creatures to kill, `#9`) → the Shrive **in hand** opens Aelf's door (`#80`: taken, the door
   pulses, given back, flag 57) → Aelf (`#2`: ALEF01, information record i69, **flag 82**, 14 creatures).
7. Back up: AELF `#17` → MAUSO1EA (two creatures once flag 82 is set, `#124`) → MAS3 `#13` → the STUDY1 stair → the
   bookcase door from the stair side: **card 3** (`#75` / `#354`: REBECCA, CHPTR3, flag 1, i2 and i79, warp @41, turn).

Optional or side: MAS6 (hand plates, lever, the words "Chullum ashdar in derias?", the portal back), the words wall in
MAUSO1EA (`#5`, after MAS6's lever), the four Study candelabra, the sarcophagus plates (the lid cannot open in this
chapter, entry 5), vials, ammunition.

**The end condition** [V: chains]: STUDY1 `#75` / `#354` with flag 82 set and flag 55 clear → act177 (flag 55,
cutscene REBECCA, cutscene CHPTR3, UI latch 4, flag 1, give i2, give i79), act578, warp STUDY1 @41 (in place), forced
turn (`#415`), show obj 36, and eight 0x17 toggles (`#202`, `#395`, `#397`, `#14`, `#453`, `#452`, `#456`, `#459`):
the chapter 3 house. Without flag 82 the same use runs the else branch `#421`: the bookcase opens.

**Corrections to CHAPTERS.md chapter 2** [V: walks and chains; written into CHAPTERS.md by this commit]:
- **The first creatures are in STUDY1**: `#362` (entering s454 behind the bookcase: obj 52, template i91, "Looks like
  we've got company.") and `#265` (the landing s25, chapter 1's area, switched on by card 2's chain `#497`: obj 58,
  template i91).
- **MAS3 also holds a Shotgun i38** (`#107`), cartridges (`#47`, `#60`: an ambush pickup that spawns a creature) and a
  spare Colt behind `#83` (entry 26). The Orb door opens a **sector** (s30's ceiling −1200 → −1000, `#68`); the walk
  reaches the Parchment and the Map only with it open.
- **MAS6: the hand panels are `#42`, `#8`, `#41`** (three DEMO4[4288] plates, a count to 3) → the lever `#20` (flag
  116) → the inscription `#5` / `#9` / `#10` ("Chullum ashdar in derias?") → the portal `#54` (MAUSO1EA @29, a forced
  turn). The words are spoken at MAUSO1EA `#5` (act235, a choice, only with flag 116).
- **MAS7's @11 arrival is never used**: MAS6 `#27` is keyed to sector id 11, which no MAS6 sector carries. MAS7 is
  reached by MAUSO1EA `#57` (@0) and by the pit `#20` / `#27` / `#136` (@13, a fall with damage).
- **Dead records** (keyed to an id no sector carries): MAS3 `#17`, MAS4 `#16`, MAS6 `#25`, `#27`, `#59`, MAUSO1EA
  `#24`, `#25`, `#59` (MAS5, also missing from the install).
- **MAS3 `#113` is not the way back**: the walk returns from MAS3 to STUDY1 by the stair `#13` without it. `#113` (the
  Shrive in hand) moves sectors id 20 (s225-s234) between −440 and 320; where that leads was not established [R].
- **The sarcophagus lid cannot open in chapter 2**: 6 statuettes exist by then (4 in the Study, 1 standing on its plate
  at load, AELF `#102`); the seventh is CAVERNS3 `#7` (chapter 6) or STUDY3 `#11` [V: every record that gives i4].
- **AELF `#3` tests the Shrive carried** (0x27 flags 01), not in hand; flag 62 gates only Florentine's movie and choices
  inside act173. The ward trap (wards, three creatures, doors shut) runs either way.

---

## 2. Chapter-wide ideas

### 2.1 First blood: creatures, the Colt, the fists [P, A/B]

Chapter 1 has no creature awake; chapter 2 has dozens [V: 0x3c spawns in every map, templates i91, i92, i121, i258,
i259, i30: all AsMonster records, item_dossier]. They are the combat lane's (actors, weapon replay). What this chapter
adds to VR is only that the decided rules now meet creatures for the first time: the Colt fires from the main hand along
the gun (ledger 33), the fists land physically with the original's damage (ledger 19), the off-wrist mount shows health
(ledger 34). Creature hits on the not-placed ids (MAS3 `#2`, `#31`, `#102`; MAS4 `#18`, `#20`, `#47`; MAUSO1EA `#75`,
`#80`, `#118`, `#131`; MAS6 `#34`, `#48`; AELF `#9`, `#29`, `#33`, `#41`-`#46`; MAUSO1EB `#39`) re-key the creature,
burst particles and play Adam's random line act672 [V: chains]. Nothing to design there.

### 2.2 Four new weapons in one chapter [P, B]

The Shield i28, the Sword i29, the Shotgun i38 and the Staff i12 are all weapons (category 1, WeaponAction blocks
1, 3, 1, 1) [V: item_dossier]. A weapon given is equipped at once (inventory.c:418-428, GAME_inventory). So the player
ends chapter 2 with fists, Colt, Shield, Sword, Shotgun and Staff, and the original's only way between them is the
inventory. Proposals, each a question:
- **Melee weapons swing physically** (Sword, maybe the Staff): RSQ Melee's reader (built for the fists, ledger 19)
  on the held weapon, firing the original's weapon attack (its short-lived bullet, GAME_combat.md) where the blade is,
  with the weapon's own rate and damage (Q6, Q10).
- **The Shield shows only while it is the selected weapon**, on the weapon hand (Q7): the original's later tests on
  it are in-hand (ANUBIS, `in-hand,no-autoselect`), so showing it when it is not selected would lie about the state.
- **Switching**: the inventory (1:1) and, as the VR way, body slots that run the same select click (Q8).
- The Shotgun is a Winchester M1897 pump [V: its OnInspect]; the pump is part of the real-parts reload, LATER (W2).

### 2.3 Sockets: setting an item into the world [P, B]

Chapter 2 is the chapter of placing things: the statuettes on their plates (7 records), the Orb into the door's
aperture (`#4`), the Key into the lock lever (`#76`), the Shrive into Aelf's door (`#80`) and the recess (`#113`).
Every one is a use with the item **in hand** (0x27 flags 03), and three take the item (0x2a) [V: chains]. One design
for all (VR_INTERACTION_RESEARCH 3.1; WORLD_DESIGN 3.4):
- the item hand holds the selected item (built: ledger 5, 10, 26);
- the **commit** is the original's use at the socket's point, fired when the held item reaches the socket (within 5 cm
  of it) and the grip opens (release); the point must be inside the record's surface box where it has one (ledger 18,
  built and seen on the owner's PC for walls);
- the original then does what it does: takes the item (it leaves the hand as its picture appears in the world: the
  statuette shown by 0x16, the Orb drawn into the door by 0x34), or refuses with its own line ("Doesn't appear to be
  working.", "Locked.", "Won't budge. / Locked."); on a refusal the item stays in the hand: ROTH has no drop;
- the pointer stays (point + grip with the item selected).
The one rule the original imposes: **one release = one use, never two** (CHAPTERS 5.4: each plate is one-shot and
counted).

### 2.4 The sarcophagus counting 7, thought through [P, B]

The puzzle as the records state it [V: chains, boxes, positions]:
- The Egyptian sarcophagus stands **upright** against the Study's north wall: its lid is the lower piece DEMO[657] of
  face f1064 between the dais (s221, floor 440) and s350, a 128 × 2 sector whose floor stands at 656 (load-time rule) →
  a lid **1.28 m wide and 2.47 m tall**. A use on it says "Won't open. I can't see any hinges or anything." (`#304`);
  its examine "Sarcophagus. Egyptian. / Authentic too." sets flag 36 (`#294`), which nothing reads.
- In front of it, the dais s221 / s222 / s362 (sector id 13, floor 440 = 0.46 m above the Study floor) carries **seven
  plates**: the use-floor records `#119`, `#124`-`#129`, one per plate, each with its own **surface box on the dais's
  floor picture DEMO[66]**: `#119` x 27..61 y 45..80; `#124` x 101..123 y 223..245; `#125` x 63..84 y 218..235; `#126`
  x 25..51 y 178..196; `#127` x 63..81 y 22..42; `#128` x 7..28 y 120..140 (off at load); `#129` x 101..119 y 16..35
  (texels; box words x0 x1 y0 y1, roth_surfbox.h).
- Each plate's statuette is a hidden ADEMO[45] object at its spot (0.36 m wide, 0.49 m tall as drawn), in an arc round
  the lid: `#119` → obj 22 at (120, 186); `#124` → obj 19 (−198, 356); `#125` → obj 20 (−188, 272); `#126` → obj 21
  (−120, 186); `#127` → obj 23 (196, 274); `#128` → obj 24 (0, 164); `#129` → obj 31 (208, 362). The lid spans x
  −64..64 at y 304.
- A plate's use: refusal line "Doesn't appear to be working." (0x36) unless a Statuette is **in hand**; take it, show
  its object, count `#156` toward 7 (it starts at **1**: obj 24 already stands on `#128`'s spot); the seventh says
  "The statuettes are glowing. Something's happening to the Sarcophagus. It's... it's opening! / Is that a ladder down
  there?" and sinks s350 from 656 to 442, step 32 (the lid goes down into the floor). Each plate is one-shot.
- Clicking a shown statuette (`#121`, `#136`-`#141`) gives it back, steps `#156` down by one, re-enables its plate and
  hides it again (0x16 toggles).
- **In chapter 2 the count can reach 6, not 7** (correction above). The player can set every statuette he has; the lid
  stays. Chapter 7 finishes it.

**VR** [P]:
1. **Carry**: the statuette is the item hand's item (wrap round the coils, ITEM_DESIGN 4: weightless by Adam's own line:
   no weight haptic).
2. **Set it down**: lower it onto the dais at a spot and open the hand. COMMIT: the original's floor use at the texel
   under the statuette's base. Where that texel falls inside a plate's box, that plate's record runs; elsewhere on the
   dais the original's click finds no record and nothing happens, and the statuette stays in the hand (no drop).
   Natural target: the spot where the original will show it; whether each box contains its own statuette's spot is
   measurement M2 [R: not checked].
3. **The original's answer is the feedback**: the statuette appears standing where the original shows it (its 0x16),
   the item leaves the hand; the plate's sound (126). No counter is shown: the original shows none; its own words come
   only with the seventh.
4. **Lift one back**: grab a standing statuette = the original's click on it (built grab path); it returns to the hand.
5. **Touch a plate with an empty hand**: "Doesn't appear to be working." (the 0x36 refusal), a dull haptic.
6. **The seventh** (chapter 7): the line, then the lid sinking by the original's ramp; nothing of ours.
7. **Never two**: a release that hovers across two boxes sends one use (the first box under the base); a statuette
   lifted and set down again is two of the original's events, as with a mouse.
8. Seated players and the floor: the dais is 0.46 m up, so the base is about 0.5 m below a standing hand; a crouch or a
   long reach (the original's 2.7 m) with the pointer.
Questions Q1-Q4.

### 2.5 Levers, chains, hand plates, seats [P, B]

The mausoleum is worked by hand: four wall levers DEMO4[4280] (MAS4 `#3`, MAUSO1EA `#18`, `#30`, `#101`), one lever
with a lock DEMO4[4246] (MAUSO1EA `#76`), one wall that must be used three times (MAS7 `#22`, a count to 3), two wall
mechanisms that open passages (MAUSO1EA `#41`, `#72`), three hand plates DEMO4[4288] (MAS6), a throne seat (MAS7).
WORLD_DESIGN 3.3 / 3.4 already propose the gestures: grip the lever handle and pull 8 cm (one use); press the open palm
2 cm into a hand plate (one use); the key into the lever's lock and a 30° turn (one use). The throne is new: **sit
down** (Q19). All keep the pointer.

### 2.6 The first dialogue choices [P, A/B]

Chapter 2 holds the game's first choice menus [V: dbase100_dump]: act174 at the altar ("Take the artefact." /
"Ignore the voice." / "Leave."), act235 at the words wall ("Keep silent" / "Speak words of power"), act173 Florentine
("Tell him nothing." / "Mention your father." / "Demand answers.", each shown only once, by flags 67-69). The panel and
its text are the original's (1:1 rule); only where it stands and how a hand picks are ours (Q11). Speaking the words
aloud would be new content (Q12).

### 2.7 Movies, cards and forced turns [P, A]

Movies in the chapter: CHPTR2 and RANDALL (card 2, chapter 1's end), SHRIVEA/B/C/D (the altar), WALKWAY1/1A/2/3,
BRANDED, ALEF01, FLOTRAP1-6, the warp film act455/act534 (cutscene 81), REBECCA and CHPTR3 (card 3) [V: actions; the
op 0x07 index rule]. All on the world-locked screen (ledger 35). Forced turns (0x3f): STUDY1 `#449` (card 2), `#415`
(card 3), MAS6 `#56` (the portal), MAUSO1EB `#22` / `#23` (after the altar's warps), AELF `#78` (Aelf): the built rule
(snap at the movie's black, else at once) covers them (Q29).

### 2.8 Traps and falls [P, A]

The original drops, crushes and burns Adam here [V: chains]: the pit (MAUSO1EA `#20`: the floor of s110 / s297 / s300
falls from −1780 to −2600 at 200 a tick, 0x33 damage, warp to MAS7 @13), the cave-in (MAS3 `#86`: four ceilings drop
from 320 to −400 at 64 a tick, bones appear), the walkway's punishment (MAUSO1EA `#95`, damage, without flag 62), the
temple floor (MAUSO1EB `#1`: damage on entering sector id 1, every time), Florentine's trap (AELF `#3`). VR changes
nothing in what happens; the falls are the chapter's comfort question (Q20).

### 2.9 Physical attempts in this chapter (the approved pattern) [P, B]

Where the original has a line for something a hand can try: the roll-top desk's drawers (`#288` "The drawers are
stuck.", built as an attempt zone), the sarcophagus lid (`#304` is a **use**: pushing the lid is that use, "Won't open.
I can't see any hinges or anything."), the heraldic shields (`#318` "This one's stuck fast.", `#322` "This is fastened
to the wall.": uses), the words wall (`#98` "This isn't gonna move in a hurry."), the tomb lids ("Doubt we could get the
lid off. These things are built to last.": MAS6 `#62`, MAS7 `#39`, AELF `#107`), the stone door with no lock (AELF
`#1` / `#95` / `#96` "Won't open. Can't see a lock or keyhole or anything."). Where the record is a use, the attempt
is that use; where it is an examine, it is the examine (ledger 21). Q32.

### 2.10 Documents [P, A]

Six documents [V: item_dossier]: the letters i11 ("There's something written on this.", 41 frames), i117, i118,
i119 ("No address or postage. Hmm. Scented too. Very nice.", 51 frames each), the Parchment i13 ("The symbol on this is
the same as the marks on my hands.") and the Map of the Mausoleum i273; all one page, hold mode. Built already: the item
in the item hand (ledger 26) and the item to the eyes = its examine (ledger 37), which opens the original's popup
(Q22).

### 2.11 Light [P]

The Study is lit (0 of 40 lantern sectors) and has two switches that dim it: the wall switch `#208` (light switch,
group 3, step −15: all 67 sectors of the Study) and the plate `#382` (DEMO[4184], light switch, group 27) [V:
`-area` LIGHT; R: what group 27 covers was not traced]; and four candelabra that brighten it (+6 each, group 3). Below
the Study, the mausoleum maps are 52-100 % lantern sectors with magic torches DEMO4[4272] / DEMO4[4212] everywhere
("Torch. Wonder who put that there? / There's power coming from the torch Adam. The flame's magical. It might have
been lit for years."), examine only. Chapter 1's answers stand: no darkness, the Lamp from the hip (Q27, Q28).

---

## 3. The entries

★ = on the critical path. Record numbers are per map. Sizes are as drawn (sprites) or the geometry's; metres for E =
1.65 m. Every record of the chapter is in section 10 with the entry it belongs to.

### A. The Study (STUDY1, R0)

**1. ★ Arrival: card 2's tail**
- What: `#299` (chapter 1's end) runs act178, whose 0x36 calls `#449`: warp STUDY1 @34 (s252) and 0x3f turn; then
  act633 (music) and `#497` enables `#265` (entry 23) [V: chains].
- VR: the movies on the world-locked screen; the warp and the turn during their black (ledger 35, 29). COMMIT: none.
- Tag: A. Effort S (built pieces).

**2. The Study door from inside**
- What: face f1301 (s266 → door s24), DEMO[90]. Original: use `#13` "Open. Good." (sound 140): the door back to the
  landing opens freely [V].
- VR: the door handle system (chapter 1, 2.4). Tag: B. Effort S (with N4).

**3. The Study's four candelabra**
- What: DEMO[4098] (five candles, as the landing pair), obj id 2: s252 at (−224, −224) and (−224, −32), s217 at (160,
  −224) and (160, −32); 0.55 × 1.88 m; wicks 1.62-1.82 m up [V: walk; wickspots for DEMO[4098]].
- Original: click `#18` (sound 258): Matchbox in hand (f6 03) → re-key −1 → delay 30 → re-key 15 → art DEMO[4148] →
  light group 3 step +6 (the whole Study, 67 sectors). One use per candelabrum; examine `#315` "Candle. / Tallow I
  believe." Optional [V: chain].
- VR: the built candles (ledger 16, 24, 23): the match lights any wick; the first wick is the one use; the rest catch
  in 15 s; one growing light each. Four uses brighten the Study by 4 × 6 [R: arithmetic on the step].
- Tag: B (built). Effort S (nothing new; check the four in a run).

**4. ★ The four statuettes lying in the Study**
- What: ADEMO[45] (0.36 × 0.49 m) on the floor: `#23` obj 34 s212 (−672, −232), `#58` obj 33 s214 (−560, −380),
  `#74` obj 35 s318 (280, −544), `#540` obj 56 s216 (520, 52) [V].
- Original: click → give i4 Statuette (sounds 52, sfx nodes 3, 4, 5, 7) [V]. i4: "It looks really heavy but there's no
  weight there at all" (ITEM_DESIGN 4).
- VR: crouch and grab (built grab, fly to the hand), or point + grip. Then entry 5. Feedback: no weight.
- Tag: A (grab), B (no-weight feel). Effort S.

**5. ★ The sarcophagus, its seven plates, the count** (section 2.4 has the whole design)
- What / Original: section 2.4 [V]. Records `#119`, `#124`-`#129` (plates), `#121`, `#136`-`#141` (take-backs),
  `#304` (use the lid), `#294` (examine, flag 36), `#380` (the pit below, off: chapter 7).
- VR: set down on the dais = the plate's use at the base's texel; lift back = the click; the empty-hand refusal; the
  lid's use as a push. COMMIT: one use per release / per lift.
- Tag: B (A: pointer). Effort M (N14 sockets, M2).
- Q: Q1-Q4.

**6. The three masks**
- What: fixed-angle quads on the west-wall panel of s401, bottoms 0.96-1.01 m up: DEMO[4203] obj 40 (0.44 m wide),
  DEMO[4204] obj 32 (0.59 m), DEMO[4205] obj 41 (0.37 m) [V].
- Original: `#351` → i25 Mask, `#301` → i24, `#171` (once) → i26 (sound 62). Examine `#289` "Wooden voodoo mask. /
  God. These things give me nightmares.", `#291` "Japanese.", `#290` "Theatrical Mask. / Porcelain I believe." [V].
  Later the three are counted against Gnarl's mask i60 (act440, act487-491, act540) [V: actions; R: what that means
  in chapter 3+].
- VR: lift each off the wall with the hand (built grab: the use). Held to the face = its examine (ledger 37, built).
  Wearing one (seeing through it) is new content: not proposed (Q5).
- Tag: A / B. Effort S.

**7. The Shield**
- What: DEMO[4132] obj 48, 0.74 × 1.03 m, standing against the east wall of s317 (640, −294) [V].
- Original: click `#320` (sound 275): "The dragon's widely used in heraldry though I've not seen this design before."
  then give i28 (a weapon: equipped). Examine `#317` "Dragon. This is scorched quite badly. / Quite recently too. / We
  know why now don't we?" [V].
- VR: grip the rim and lift (the use); it goes to the weapon hand's forearm as the selected weapon (Q7). Feedback: a
  heavy haptic as it comes off the wall [R: ITEM_DESIGN 28 gives 3 kg].
- Tag: B. Effort S (+ N20 if body slots).

**8. The Sword and the weapon rack**
- What: the Sword DEMO[4129] obj 44 (0.47 × 1.51 m as drawn) leaning by the rack in s362 (210, 384); the rack
  DEMO[4169] obj 43 (2.15 × 0.71 m) on the wall at z 460, twice (s361, s362); a Claymore ADEMO[114] (0.28 × 1.48 m)
  and a Scimitar ADEMO[115] (0.32 × 1.34 m) hung on it [V].
- Original: click `#332` (sound 71): "This one's come away from the rack. Despite the muck it might be useful. / Got
  some ideas about protecting me? We'll just have to see about that." then give i29 (a weapon). Examines: `#327`
  "Behind all this muck I'll bet there's a sword. Or at least there once was.", `#324` "Mint condition. / Old too."
  (the rack), `#325` "Claymore?", `#326` "Scimitar." (no use) [V].
- VR: close the weapon hand on the Sword's grip and lift: the use; it is then the held weapon, swung physically (Q6).
  The Claymore and Scimitar: a grab sends the use and nothing happens (no record): they stay on the rack; a tug on
  them plays their examine (attempt pattern).
- Tag: B. Effort M (N19 melee weapon).

**9. The letters**
- What: three drawn on the writing table's floor picture (sectors s413, s414, s415, tops at 480 = 0.92 m; DEMO[711])
  and one on the floor picture of s320 (DEMO[703], floor level) [V].
- Original: use-floor, one-shot, sound 109, each swaps its floor picture and gives a letter: `#34` (box x 16..40 y
  25..48) → i119 and flag 163 (read by nothing); `#53` (x 11..24 y 20..35) → i118; `#57` (x 15..18 y 25..27) → i117;
  `#38` (x 21..51 y 19..45) → i11 [V: chains, boxes; flag 163: no reader in the census].
- VR: pinch the drawn letter (fbx_pinch_tight) and lift 3 cm, or slide it off the edge: one use at the contact texel,
  inside the box (Q21: `#57`'s box is 4 × 3 texels). The picture swaps (the letter leaves the table) and the letter is
  in the hand. Reading: 2.10.
- Tag: B. Effort S (box test built).

**10. The table with the magazine**
- What: s384 / s385 (sector id 28, top at 480 in s384); a Colt-45 magazine ADEMO[141] obj 1000 on s385 at z 484 [V].
- Original: examine-floor `#303` "Table. Used recently. / You're right, the dust's been disturbed."; click `#410` gives
  the clicked item (a magazine) [V].
- VR: grab (built). Tag: A. Effort S.

**11. The grandfather clock**
- What: DEMO[726] on faces f1978 / f1983 between s399 and s400 (the clock case, floor 446) [V].
- Original: use `#35` (no box: x words 0): act27 ("Still doesn't make any damn sense.", only if flag 292), a 0x1f
  counter (key 12: the census reads 1 → 11 of 12), two sfx nodes, **0x2f door on face id 10** (s224), then if flag 293
  (ANUBIS `#34`, chapter 7) is clear: act27 again, delay 400, sfx; else `#250`: act28 and toggle `#251`. Examine `#251`
  "Hand's not working. But it's still ticking. Doesn't make any damn sense." [V: chain; R: what the 0x1f counter shows].
- VR: the case door by its edge (the use). Nothing more until 0x1f is read in ROTH.C (Q24).
- Tag: B. Effort S.

**12. The gramophone**
- What: face f1450 (s291 / s367, DEMO[17]) and the floor of s367 (sector id 1) [V].
- Original: use `#33` (sound 115): floor picture → DEMO[516], wall texture, obj 18 art → DEMO[4188], a count to 2,
  sfx node 1, delay 120, "What the hell's that? / Turn it off for heaven's sake." (act26, flag 12), toggles `#6` (sfx
  node 18). Examines `#248` / `#249` "This Gramophone still works. Shame about the taste in music. ..." [V].
- VR: the hand on the tone arm (or the crank) = the use (Q25). Tag: B. Effort S.

**13. The heraldic shields**
- What: faces f1439, f1443, f1787 (DEMO[79], s289 / s290 / s367) with boxes `#318` / `#306` x 115..146 y 89..126 (the
  lion) and `#322` / `#293` x 47..79 y 85..128 (the star) [V].
- Original: use `#318` "This one's stuck fast. / There are a few famous knights who have used this charge. ...", use
  `#322` "This is fastened to the wall. / The star - the charge - it's called a Mullet. ..."; examines `#306` "Hmm. Lion
  design. / Nice condition.", `#293` "Star motif. ..." [V].
- VR: grip the shield's rim and pull: the use (its line), a stiff no-give haptic; palm + trigger: the examine.
- Tag: B. Effort S.

**14. The painting of the reader**
- What: face f1082 in the niche s225 (floor 546, DEMO[181]) [V]. Original: examine `#312` "What's that book he's
  reading? / Looks like a diary. / That book's the same one we found in the observatory." [V].
- VR: point + trigger (it is 1.7 m up [R: from the niche floor]). Tag: A. Effort S.

**15. The rugs**
- What: floors of s226 (sector id 40, DEMO[39]), s401 / s403 (id 27, DEMO[32]), s402 (id 19) [V].
- Original: examine-floor `#108`, `#243`, `#242`: "This's intricate. Look at the work in this. / It's got some sort of
  device woven into it. ..." [V].
- VR: point + trigger, or two raps on the floor (knock, ledger 20). Tag: A. Effort S.

**16. Chairs, the sofa, the coalscuttle**
- What: chairs DEMO[4109] obj 17 (s253, s216, s318), the sofa DEMO[4106] obj 45 (s217), the coalscuttle DEMO[4100]
  obj 42 (s217, 0.43 × 0.83 m) [V].
- Original: `#244` "Plain old chair. / Guess so."; `#311` "Not much to search. / Could be a clue anywhere remember.";
  `#292` "Coalscuttle. Just an ordinary Coalscuttle. / Silver isn't it? Must be worth quite a bit." [V].
- VR: the sofa crease (the attempt zone, as chapter 1 entry 14, if the owner wants it for `#311`); knock; trigger.
- Tag: A / B. Effort S.

**17. The Study windows**
- What: faces f1012-f1014 of s212 (DEMO[744], DEMO[746]) [V]. Original: attack-hit `#181` (smash, hazard, particles),
  as chapter 1 entry 16 [V].
- VR: physical melee or a shot (built). Tag: A. Effort S.

**18. The Study's light switches**
- What: the wall switch on face f1302 (s266, DEMO[21]) with box x 7..17 y 130..140; the plate DEMO[4184] obj 55 in s361
  at 1.37 m [V].
- Original: use `#208` → `#209` light switch, group 3, step −15 (the Study); click `#382` light switch, group 27;
  examine `#547` "Light switch." [V].
- VR: flick with the index or slap (chapter 1 entry 24; ledger 19: a punch into a switch is its use). Tag: B. Effort S.

**19. The roll-top desk**
- What: DEMO[4113] obj 1 (mesh 114 × 68 × 80) in s361 [V].
- Original: examine `#288` act100: "The drawers are stuck." and, with Rebecca (flag 1), "The canopy's locked too. And
  the lock's been melted." Never opens (CHAPTERS 6) [V].
- VR: the drawer tug plays `#288` (built attempt zone, ledger 21). Opening it would be new content (Q23).
- Tag: B (built). Effort none.

**20. The green sigils (the Staff's gates, chapter 6)**
- What: ADEMO[235] (0.23 × 1.47 m) fixed-angle, obj 66 in s222 (−66, 350) and obj 65 in s362 (66, 350), either side of
  the sarcophagus [V].
- Original: click `#424` / `#445`: "Doesn't appear to be working." unless i33 Staff (CAVERNS3, chapter 6) is in hand;
  then delay 20, flag 71, warp CAVERNS3 @21 / AELF @8, the warp film (act534) [V].
- VR: in chapter 2 a touch gives the refusal line; the ward tingle (ledger 22) may extend to these (Q31).
- Tag: A. Effort S.

### B. The bookcase and the way down (STUDY1, R1)

**21. ★ The bookcase doors**
- What: two leaves s228 / s458 (faces f1030 / f1031 from the Study, DEMO[254]; 1.28 m each, 2.8 m tall) [V].
- Original: from the Study, use `#353` / `#420` (sound 149): 0x2f on face ids 38 and 42 (both leaves). From the
  passage, `#75` / `#354`: the else branch `#421` opens them until card 3's gate holds (entry 65) [V].
- VR: no painted handle: grip a leaf's edge and pull 4 cm (WORLD_DESIGN 1.3) = one use; both leaves swing by the
  original's tick (Q30). Tag: B. Effort S (with N4).

**22. ★ "Looks like we've got company."**
- What: entering s454 (sector id 42), once [V].
- Original: `#362`: 0x3c shows obj 52 with creature template i91, act674 (one of "Looks like we've got company. / What
  the Hell? / Oh God. / Jeez! What the Hell..?") [V]. The game's first awake creature on the critical path [V: chapter
  1 has none awake].
- VR: 2.1. The off-wrist line; the Colt drawn from wherever Q8 puts it. Tag: A. Effort S.

**23. The landing creature (chapter 1's area, after card 2)**
- What: entering s25 (sector id 39), once; `#265` is off until card 2's `#497` [V].
- Original: 0x3c obj 58, template i91 [V]. A player who walks back out of the Study meets it.
- VR: nothing new. Tag: A.

**24. ★ The stair down and the warp to MAS3**
- What: the passage s454 → s459 → the stair s441 ... s433 (400 → 0), s455-s457 (sector id 38, floor 0), s448 (−40),
  then down to s446 (−400); all lantern sectors [V].
- Original: entering any id-38 sector runs `#404`: warp MAS3, key 0 (position kept) [V].
- VR: real stairs under the original's step-up (ledger 30, seen on the PC); the Lamp from the hip. Tag: A.

### C. The cellars (MAS3)

**25. ★ Arrival, the torches, the way back up**
- What: s248 at the stair foot (the STUDY1 position kept), the stair s131-s141 (sector id 6) back up [V].
- Original: entering id 6 runs `#13`: warp STUDY1 key 0 (back to the Study stair). The torches DEMO4[4272] (`#98`,
  32 placements) say "Torch. Wonder who put that there? / There's power coming from the torch Adam. ..." [V].
- VR: nothing new (Q28 on the torches' light). Tag: A.

**26. The cave-in and the spare Colt**
- What: entering s194 / s224 (sector id 17, floor −440), once [V].
- Original: `#83`: 0x38 → `#86` if the next test fails; 0x27 Colt **carried** (flags 00) → `#85` moves s236 / s237's
  ceiling (−320 / −240): the niche holding a Colt ADEMO[99] and two magazines; then `#86`: the ceilings of s239, s243,
  s244, s245 drop from 320 to −400, step 64; sfx; bones (obj 15, four DEMO4[4226]) shown. Examines `#89` "Plenty of
  this rubble around. / The structure doesn't look too safe does it?", `#88` "Bone. Looks human / It is." [V: chain;
  R: that `#85` closes the niche when Adam already has a Colt (fl 01 read as the ceiling, as STUDY1 `#9`)].
- VR: the collapse happens round the player (Q20). Tag: A. Effort none.

**27. Creatures waking as the player walks in**
- What [V: chains]: `#49` (s193, id 11, once: obj 3), `#51` (s192, id 12, once: obj 4), `#53` (s156, id 13: obj 5, a
  count to 2, then off), `#19` (s44, id 3, once: obj 2), `#57` (s110, id 14: act674, objs 6, 7), `#25` (s102, id 16,
  once: act674, objs 11-14), `#67` (s161, id 15, once: obj 10, act674, light group 15 −40 for 140 tics then +40: the
  room goes dark while it comes), all template i91; `#5` (s159, a random 200-tic delay, sound 88: ambient).
- VR: nothing new; `#67`'s blackout is the original's ramp (no darkness pass). Tag: A.

**28. ★ The Orb**
- What: ADEMO[202] obj 1 (0.32 × 0.26 m) on a ledge in s28, 1.40 m above the floor (z +122) [V].
- Original: click `#1` (sound 127) → give i18 Orb ("Metallic orb. Feels... alive.") [V].
- VR: cup it (fbx_cup) off the ledge (the use); a slow pulse while held (ITEM_DESIGN 18: no state). Tag: A / B.
  Effort S.

**29. ★ The Orb door**
- What: s30 (128 wide, 20 thick) between s29 and s34, DEMO4[2353] / DEMO4[2366]; closed: s30's ceiling at −1200 [V:
  the walk; with `-setceil 1:-1000` it reaches 52 more sectors].
- Original: examine `#7` "There's a small aperture in the middle of this door." Use `#4` (box **x 26..41 y 40..55**:
  the aperture; sound 121, once): Orb in hand → take it → face id 2 → DEMO4[2816] (the Orb in the door) → delay 120 →
  enable `#24` → `#68`: s30's ceiling −1200 → −1000 (the door opens) and two wall textures → delay 120 → `#65` textures
  → delay 400 → `#68` again → delay 120 → `#65` [V: chain]. `#24` (off until then, once): give the Orb back, face id 2
  back to DEMO4[2366], re-enable `#4`. [R: that the second `#68` closes the door again: 0x07 toggling between its limits
  was not traced; the 400-tic window would then be the time to pass.]
- VR: hold the Orb into the aperture and open the hand (2.3): the use inside the box. The Orb appears in the door, the
  door rises by the original's tick. Taking it out: grab the Orb in the door = `#24`'s use (Q14).
- Tag: B. Effort S (N14; box built).

**30. The Parchment**
- What: drawn on the floor picture of s97 (sector id 4), beyond the Orb door [V].
- Original: use-floor `#10` (once, sound 109): floor picture → DEMO4[2753], give i13 Parchment ("The symbol on this is
  the same as the marks on my hands." / "Sketch of a... door. And a strange symbol.") [V].
- VR: pinch and lift (as entry 9). Tag: B. Effort S.

**31. The Map of the Mausoleum**
- What: ADEMO[101] obj 22 in s81 [V]. Original: click `#111` → give i273; examine `#110` "A map. But of where? / Map.
  But to where?" [V].
- VR: grab; reading as 2.10 (the original's map popup, nothing of ours: Q22). Tag: A. Effort S.

**32. The Shotgun and the ammunition**
- What: the Shotgun ADEMO[92] obj 20 (1.17 m wide as drawn) on the floor of s102; cartridges ADEMO[146] (`#47`, `#60`,
  and a plain pickup obj 1002 in s102); magazines ADEMO[141] (`#45`, 11 placements) [V].
- Original: `#107` gives the clicked item (i38 Shotgun: "Shotgun. Winchester M1897 slide action twelve gauge
  shotgun...." / "Do be careful where you point that Adam."); `#60` gives cartridges **and** shows obj 9 (template i91)
  with act674: an ambush [V].
- VR: the Shotgun picked up by its grip (weapon hand); one hand for now (left-hand support LATER), along the gun (W1),
  the original's cycle (Q9). Tag: A / B. Effort S.

**33. Vials (all the chapter's)**
- What: ADEMO[276] / ADEMO[277]: MAS3 `#109` (×3), MAS7 `#29`, MAUSO1EB `#42` (×2), AELF `#97` (×4, hidden until
  `#2`) [V].
- Original: each gives a Vial (i143 or the clicked item): "It's got some sort of liquid in it. Any ideas?" / "Yes...
  It's magical; healing I believe."; its OnUse on self runs act617 (health, remove the vial) [V: item_dossier].
- VR: drink: tip it at the mouth = the original's use on self (Q26). Tag: B. Effort S.

**34. The Shrive recess**
- What: face f1012 of s223 (DEMO4[2830], a 0.50 × 0.69 m recess at −336, beside the cave-in hall); a hidden DEMO4[4244]
  (the Shrive's picture) obj 16 sits in it [V].
- Original: use `#113` (box: none): "Doesn't appear to be working." unless the Shrive is in hand → move sectors id 20
  (s225-s234) between −440 and 320, step 2 → delay 200 → face id 5 → DEMO4[2379] (fl 0a). The Shrive is kept [V; R:
  where s225-s234 lead].
- VR: as Aelf's door (entry 61, Q15). Tag: B. Effort S.

**35. Examine-only things in MAS3**
- `#8` (floor of s41) "Just an old wooden chair. / It's seen better days."; `#9` (s44) "Colour's faded badly on the
  weave. ..."; `#99` DEMO4[4291] / DEMO4[4277] (10) "Someone's coat of arms maybe."; `#100` (s218) "Some strange symbol
  by the looks of it." [V]. VR: trigger, knock. Tag: A.

**36. Exits**
- `#15` (sector id 7, s112-s122) → MAUSO1EA key 0; `#17` (sector id 8: no sector carries it) dead [V].

### D. The mausoleum (MAUSO1EA)

**37. The mausoleum doors**
- What: door sectors s150, s153, s154 (DEMO4[2364]) and s187 (DEMO4[2426] / [2427]) [V].
- Original: use `#16` (no box) / `#37` (box x 0..145, y 0..255): free doors (sound 114). Examine `#36` on the
  inscribed door f1885 "What's that schpiel? What's all this about? / It's a passage from the Bible. Your interpretation's
  as good as mine I'm afraid." [V].
- VR: the door system (chapter 1, 2.4). Tag: B. Effort S.

**38. Levers and wall mechanisms**
- What / Original [V: chains]: DEMO4[4280] levers (0.35 × 0.54 m, at +80 or on the floor): `#18` obj 10 → s425's ceiling
  −1320 → −1100; `#30` obj 5 → s213's ceiling −1380 → −1160; `#101` obj 8 → `#100` 0x03 on sector id 28 (s37, s38,
  s246-s248), limits −1580 / 0, sfx 6 (also run by walking into s39, `#99`, once). Examines `#109` / `#110` "Lever.
  Still works." Wall uses: `#41` (f1657, sound 179) → s186's ceiling −1620 → −1300, examine `#121` "The metal's still
  good. ... Anti-corrosive."; `#72` (f163, sound 127) → s73's ceiling −1360 → −1040.
- VR: grip and pull 8 cm (2.5, Q17); the wall mechanisms: grip the metal and pull (their pictures were not looked at:
  M4). Tag: B. Effort S each (N18).

**39. ★ The locked passage and Key i22**
- What: s193 (sector id 7), closed (ceiling = floor −1780); the lock lever DEMO4[4246] obj 6 (0.49 × 0.50 m at +60) in
  s76; the face f878 of s194 [V].
- Original: click `#76` (once): "Won't budge. / Locked." (act264: the first only if i22 is not carried) unless Key i22
  is **in hand** → s193's ceiling −1780 → −1564 → flag 428 (read by nothing). Use `#15` on f878: "How's this suppose to
  open?". From the walkway side `#33` opens s193 with no key [V].
- VR: the key into the lever's lock and a 30° turn = the use (Q16); without it the refusal and a rattle. Tag: B. Effort
  S (with N4's key turn).

**40. ★ The walkway and the Creator's warnings**
- What: s207 (id 9), s206 (id 10), s209 (id 11), once each, on the way to MAUSO1EB [V].
- Original: `#40` act184: WALKWY1A if flag 62, else WALKWAY1 ("Creator - The Warning"); `#44` act185: WALKWAY2 if not
  flag 62; `#46` act186: if not flag 62, `#95` (0x33 damage) then WALKWAY3 ("Creator - Punishment") [V]. Order-
  sensitive: the throne (entry 55) first spares the player.
- VR: world-locked movies; the damage on the mount and the low-health cue (built). Tag: A.

**41. The words wall**
- What: f1595 of s346 (DEMO4[2791]), the stone block of sector id 8 (s331, s333, s346, s347) [V].
- Original: use `#5`: act235: only with flag 116 (MAS6's lever, entry 52), the choice "Keep silent" / "Speak words of
  power". Keep silent → run `#68`: show obj 2, spawn objs 14 and 18 (template i92), toggle obj 14. Speak → Adam says
  "Chullum ashdar in derias.", flag 433, run `#6`: 0x03 on sector id 8 (limit −880, sfx 3), disable `#5` and `#98`,
  delay 60, spawn objs 1 and 4 (template i91). Without flag 116: "This isn't gonna move in a hurry." (`#73`, act265);
  examine `#98` the same line [V: chains, act235; R: where the moved block leads].
- VR: the hand on the stone: the use; the choice in the original's panel (Q11). The spoken words are Adam's line, not
  the player's voice (Q12). Tag: A / B. Effort S.

**42. The pit trap**
- What: s297 (sector id 6, 0.48 × 2.2 m) and s298 (id 33), with s110 and s300 [V].
- Original: `#20` (once, sound 186) and `#136`: delay 4, disable `#20`, the floors of s110, s297, s300 fall −1780 →
  −2600 at 200 a tick, 0x33 damage, warp MAS7 @13. `#27` (id 6 again): the warp alone [V].
- VR: the original's fall, then the warp (Q20). Tag: A.

**43. Creatures in the mausoleum**
- What [V: chains]: placed ADEMO[160] (id 3, four) and ADEMO[164] (id 2, examine `#48` "One ugly mother."); a hidden
  ADEMO[43] obj 4 (examine `#39`); `#128` (s402, once: obj 22, template i91); `#124` (s94, once, **only with flag 82**:
  objs 20 and 19, template i258, act674: the way back, entry 65); `#75` / `#80` / `#118` / `#131` / `#70` / `#93` hits.
- VR: nothing new. Tag: A.

**44. Sarcophagi, the tomb, the room that changes**
- What / Original [V]: examine-wall `#102`, `#103`, `#106`, `#107` "Sarcophagus. Cold to touch."; examine-floor `#108`
  (s36, the tomb lid) "Depiction of a knight. No name."; entering s34 (id 2, once) `#8`: four wall textures, a floor
  texture, sfx 8; entering s10 (id 4) `#23`: act213 (flag 206); entering s92 (id 26) `#7`: flag 83.
- VR: palm on the stone + trigger ("Cold to touch." invites the palm); a two-hand push on the tomb lid plays `#108`
  (attempt pattern, Q32). Tag: A / B. Effort S.

**45. Examine-only things in MAUSO1EA**
- `#111` torches; `#112` DEMO4[4126] knights (25) "Knight or some soldier. Definitely Norman period though. / You aren't
  the idiot I first took you for are you?"; `#113` coats of arms; `#114` chains "The metal's still good. ... untarnished.";
  `#115` bones DEMO4[4295] / [4223] "Bone. Looks human / It is."; `#116` "Some strange symbol by the looks of it.";
  `#35` (id 7, not placed) "Old and chipped." [V]. VR: trigger, knock. Tag: A.

**46. Ammunition**: `#86` cartridges and magazines (10 placements, give the clicked item); `#88`, `#104` keyed to ids
nothing carries [V]. VR: grab. Tag: A.

**47. Exits**: `#51` (s221-s232, id 17) → MAS3; `#53` (s189) → MAS4; `#55` (s401) → MAS6; `#57` (s306) → MAS7; `#61`
(s103) → AELF; `#65` (s393) → MAUSO1EB; all key 0; `#24`, `#25`, `#59` dead (no sector carries their ids; `#59` names
MAS5, not in the install) [V].

### E. The store rooms (MAS4)

**48. The store-room door and its lever**
- What / Original [V]: use `#1` (door s128, free, sound 139); the lever DEMO4[4280] obj 1: click `#3` (sound 120): 0x2f
  face id 2 (s134, extent 1100), sfx node 1, art → DEMO4[4281] (the thrown lever); examine `#14` "Lever. Still works.".
- VR: pull the lever (Q17); the art swap is the original's feedback. Tag: B. Effort S.

**49. ★ Key i22 and the pact**
- What: ADEMO (art 2:242) obj 2 in s133, behind the lever's door [V].
- Original: click `#7` (once, sound 188): give i22 Key ("Iron Key." / "No markings."), toggle obj 5, enable `#27`, sfx
  node 2, spawn obj 4 (template i91), light group 1 +20 (77 sectors). `#27` (entering s134, once): "Your presence
  violates the pact, as it is written, as it was, so shall it be, for all enternity." (act217), delay 200, toggle objs 5
  and 3 [V: chains; the line's spelling is the original's].
- VR: pinch the key (P1 lateral pinch); the room brightens by the original's ramp. Tag: A / B. Effort S.

**50. MAS4's ammunition, creatures and examines**: `#43` magazines (9), `#45` cartridges (7); creatures `#18`, `#20`,
`#47`; examines `#11` symbol, `#15` torches, `#24` coats of arms, `#30` chains; exits `#10` → MAUSO1EA, `#16` dead [V].
Tag: A.

### F. The gauntlet (MAS6)

**51. ★ The three hand plates**
- What: DEMO4[4288] (0.41 × 0.41 m, bottoms at 0.92 m) on walls: obj 5 in s17, obj 2 in s14, obj 4 in s19 [V].
- Original: clicks `#42`, `#8`, `#41` (sound 116), each: lower one sector (s39 / s51 / s40, floor −1102 → −1500), re-key
  the plate to 0 (one-shot), count `#77` to 3; at 3: show obj 3 (the lever) and raise s38's ceiling (−1500 → −1316)
  [V: chains; R: the walk could not reach the lever until s38 opens].
- VR: the open palm pressed 2 cm into the hand print = one use (Q18). Pose: open hand (frame 0). Feedback: a press
  haptic, the original's sound and the sector moving. Tag: B. Effort S.

**52. ★ The lever, the inscription and the portal**
- What: the lever obj 3 (DEMO4, art 0:192) in s38; the hidden panel DEMO4[4290] obj 6 (0.89 × 1.40 m, 2.3 m up) and the
  wall faces f45 / f46 (DEMO4[2797]) of s9 [V].
- Original: click `#20` (once): lower s52, two wall textures, show obj 6, sfx 2, enable `#31`, **flag 116**, enable
  `#5`, `#9`, `#10`, show obj 2000. Then a use on the panel or the wall (`#5`, `#9`, `#10`): Adam reads "Chullum ashdar
  in derias?" (act416), sfx 8, enable `#54`. `#54` (entering s12, id 13): the warp film (act534), warp MAUSO1EA @29,
  0x3f turn [V].
- VR: pull the lever; palm on the inscription = the use (Adam's line on the wrist); the portal by walking in.
  Tag: B. Effort S.

**53. The rest of MAS6**: the timer `#29` (a random 120-tic delay; while flag 154 is clear, spawns and counts to 4;
then off) and the kill count `#48` (objs re-keyed, `#32` stepped); `#67` (once) act673 "What the Hell's that? / This
just gets better all the time. ..."; examines `#36`, `#38`, `#39`, `#40` sarcophagus, `#62` tomb "Doubt we could get the
lid off. ...", `#63`, `#64`, `#65` symbols; exit `#23` → MAUSO1EA; `#11`, `#13`, `#25`, `#27`, `#59` keyed to ids
nothing carries [V]. Tag: A.

### G. The pit and the throne (MAS7)

**54. The pit and the chain wall**
- What: the pit region (43 sectors, arrival @13 = s338) [V].
- Original: use `#22` (wall id 4, sound 122): count to 3 (each use counts); the third raises s286's ceiling −2280 →
  −1990, then a 300-tic delay. Examines `#26` "The metal in these chains is untarnished. / They're inhued with some
  Power. ...", `#27`; `#41` (once) act673; `#29` a vial [V; R: that s286 is the way out].
- VR: three pulls on the chains (Q17), each one use, a heavy haptic; the third opens. Tag: B. Effort S.

**55. ★ The throne (BRANDED)**
- What: the seat s162 (sector id 8, 0.16 × 0.74 m) at −1180, 40 units (0.46 m: chair height) above the step s163 in
  front, on a stepped dais 240 units above the room floor; the back on face f807 / f809 (wall id 1, DEMO4[2328]) [V].
- Original: use-floor `#7` (once) / `#33`, use-wall `#14`: act183: the first time flag 56, cutscene BRANDED, flag 62;
  after: "Hell. You won't get me sat on that again."; the chain sets flag 62 again. Examines `#30` / `#32` "Impressive.
  Very impressive. Though who'd sit here in the middle of a dungeon I've no idea." (and the "again" line after) [V].
  Flag 62 spares the walkway (entry 40) and opens Florentine's movie (entry 60).
- VR: **sit down**: standing on the step with the back to the throne, the head drops to sitting height over the seat:
  one use on the seat (Q19). The movie on the world-locked screen. Pointer + grip stays. Tag: B. Effort S (N16).

**56. The rest of MAS7**: `#15` (entering id 4: 0x3e, sound 272, "This is weird. What on earth is this all about?"
[R: what 0x3e does here]); `#20` (entering s291: a height change); examines `#28` "It's a Ward. Though to ward what I
havn't got a clue.", `#34` / `#35` "Just an old wooden chair.", `#36`-`#38`, `#39` tomb, `#40` sarcophagus, `#43`;
exit `#3` → MAUSO1EA; `#10`, `#12` keyed to ids nothing carries [V]. Tag: A.

### H. The winged-statue temple (MAUSO1EB)

**57. The temple, its statues, the burning floor**
- What [V: chains]: four winged statues DEMO4[4203] (ids 1, 4, 5, 6; examines `#3`, `#30`, `#31`, `#32` "White marble.
  There's a sense of calm around these things."); entering sector id 1 (s4) `#1`: 0x33 damage, every time; entering
  `#35` (once): spawns obj 8 (template i258), objs 3 and 7 (i91); vials `#42`; `#14` / `#12` toggle record 0 [R: a no-op
  in the data]; `#39` a creature hit; `#24`, `#26` not placed.
- VR: nothing new; the damage on the mount. Tag: A.

**58. ★ The altar: the Staff and the Shrive**
- What: the Staff ADEMO[148] (1.99 m long as drawn) lying 1.08 m up on the altar s109 (obj 2), the Shrive's picture
  DEMO4[4244] 1.47 m up (obj 2 too); the altar face f547 / f1101 (wall id 1, DEMO4[2154]) [V].
- Original: click the Staff `#21`, use the altar `#4`, or walk into s214 (id 6, `#16`, once) → act174: unless flag 190,
  SHRIVEA the first time (flag 189), then the choice:
  - "Take the artefact." → SHRIVEC, **give i14 the Shrive, give i12 the Staff**, flags 63 and 190, then `#11`: toggle
    obj 2 (the Staff and the Shrive leave the altar), warp in place @7, 0x3f turn, **two of the winged statues (objs 6
    and 5) become creatures** (template i30), UI latch;
  - "Ignore the voice." → SHRIVEB;
  - "Leave." → `#17` (warp @5, turn), SHRIVED, UI latch.
  Examine `#5` "Marble. Altar stone probably." / after flag 63 "Two intertwined serpents." [V: chains, act174].
- VR: grab the Staff = `#21`'s click: the original's movie and choice; the choice in the panel (Q13). After "Take", the
  Staff is a weapon (Q10) and the Shrive an item. The statues waking: the original's.
- Tag: A (B: grab). Effort S.

**59. Exit**: `#7` (s276, id 4) → MAUSO1EA key 0 [V].

### I. Aelf's crypt (AELF)

**60. ★ Florentine's trap**
- What: entering s467 (sector id 2), once, the arrival hall [V].
- Original: `#3`: the Shrive **carried** (flags 01) → act173 (flag 40; if flag 62: FLOTRAP1 and up to three choices,
  each once) → face id 2 → none, show the wards obj 2 (two DEMO4[4273], 0.37 m, at +100), show obj 1 (three creatures
  ADEMO[124] / [122] / [126]), disable the doors `#7` and `#59`. Each kill `#9`: re-key, count to 3; at 3: face id 2 →
  none (fl 08), toggle the wards, re-enable `#7`, toggle `#59`. The wards: click `#57` "This one's stuck fast. ...",
  examine `#111` "This symbol's glowing. / I believe it's designed to keep us in, or keep something out." [V].
- VR: the ward tingle on these wards (ledger 22; Q31); prying at them plays `#57`; Florentine's choices in the panel
  (Q11). Tag: A / B. Effort S.

**61. ★ The Shrive door**
- What: double door s441 / s532 off s439 (wall ids 1 and 6, DEMO4[2456] at s439) [V].
- Original: use `#80` (sound 147): delay-latched "Locked." (0x36) unless the Shrive is **in hand** → delay 5 → take it →
  `#90` (light group 9, −20) → toggle obj 7 → delay 30 → toggle obj 7 → `#90` → give the Shrive back → open faces 1
  and 6 → flag 57 (read by nothing). From the far side `#94` opens the same doors. `#19` "Locked." (toggled by the
  14-kill count; `#2`'s own toggle of it is a disabled step);
  examines `#1`, `#95`, `#96` "Hey! What do you know! Ingenious. / Won't open. Can't see a lock or keyhole or
  anything." [V; R: obj 7 is the Shrive shown in the door].
- VR: screw the Shrive in (Q15): it leaves the hand, the door's light dips twice, it comes back into the hand (the
  original's give, retargeted), the leaves swing. Tag: B. Effort S (N23).

**62. ★ Aelf**
- What: entering s113 / s114 (sector id 1), once: Aelf's chamber [V].
- Original: `#2`: act176 (cutscene ALEF01, give i69 Aelf, **flag 82**), act658 (music), show the vials obj 9, 0x3f
  turn, a disabled step that would toggle `#19`, enable `#72`, show objs 4, 5, 6: **14 creatures** (ADEMO[122], [124], [126]). Kills `#29`, `#41`,
  `#42` and the timers `#31`, `#47`, `#49` (spawns) step a count to 14 that toggles `#19`; `#72` (off until `#2`, once):
  obj 8, template i121 [V; R: the timers' firing].
- VR: ALEF01 world-locked; the turn snaps at its black; then the fight (2.1). Tag: A.

**63. The statuette, vials and ammunition**: `#102` the sixth Statuette (obj 10, s227); `#97` vials (shown by `#2`);
`#74` magazines, `#76` cartridges [V]. VR: grab. Tag: A.

**64. The rest of AELF**: the Staff sigil `#16` (ADEMO[235] obj 3, s214: the chapter 6 gate back to STUDY1 @13, the
Staff i33 in hand); examines `#82` torches DEMO4[4212], `#106` knights, `#107` tomb lids "Doubt we could get the lid
off.", `#108` sarcophagi, `#109` "Incense burner. / Persian judging by the design.", `#110` "Marble. Altar stone
probably."; `#51` (s502, id 5, once: 0x03 on sector id 6, delay 60, face id 4 texture); `#70` ambient; `#25` off;
exit `#17` → MAUSO1EA key 0 [V]. Tag: A.

### J. The end

**65. ★ The way back and card 3**
- What: AELF `#17` → MAUSO1EA (`#124`: two creatures, once, with flag 82) → MAS3 `#51` / `#15` arrivals → MAS3 `#13` →
  the STUDY1 stair → the bookcase door from the passage (faces f2218 / f2220 of s454, DEMO[255]) [V].
- Original: use `#75` / `#354` (sound 149): 0x38 → `#421` if the test fails; flag 82; 0x38 → `#421`; flag 55 clear;
  delay 10; act177 (flag 55, REBECCA, CHPTR3, UI latch, flag 1, i2, i79); act578; warp STUDY1 @41; 0x3f; show obj 36;
  the eight toggles [V].
- VR: the same bookcase gesture as entry 21 (the original decides between door and card); the movies world-locked;
  the turn at their black. COMMIT: one use. Tag: A / B. Effort S.

### Not interactable in chapter 2 (present, asleep) [V: walks, chains]

| what | where | why not in chapter 2 |
|---|---|---|
| the pit under the sarcophagus, `#380` "What's that smell? Sulphur." | STUDY1 s351 (id 33) | off; the lid opens in chapter 7 |
| the green sigils' warps (`#424`, `#445`, AELF `#16`) | Study, AELF | need the Staff i33 (chapter 6) |
| the walkway lines with flag 62 set | MAUSO1EA | WALKWY1A instead of WALKWAY1 |
| developer object `#606` (every map, the Dragon Sword, ...) | STUDY1 s261, hidden | never shown (CHAPTERS finding 8) |
| MAS5 (`#59`) | MAUSO1EA | not in the install |
| records keyed to ids nothing carries | see section 10 | dead in the data |

---

## 4. Ranked: the 10 most fun or impactful VR interactions in chapter 2 [P, R: ranking]

1. **The sarcophagus counting 7**: carrying the weightless gold serpents, setting each on the dais round the upright
   Egyptian lid, lifting them back (5, 4). Six of seven in this chapter: the room's question stays open until chapter 7.
2. **The Sword pulled from the rack and swung** in the chapter's first fights; the Shield off the wall (8, 7, 2.1).
3. **The Orb set into the door's aperture, the door rising** (28, 29).
4. **The altar: the Staff lifted, the Creator's choice, the winged statues coming alive** (58).
5. **Sitting on the throne: BRANDED** ("Hell. You won't get me sat on that again.") (55).
6. **The Shrive screwed into Aelf's door, then Aelf** (61, 62), after Florentine's ward trap (60).
7. **The bookcase swinging open and "Looks like we've got company."** (21, 22): the first creature, the Colt along the
   gun.
8. **MAS6's three hand prints, the lever, the words read and later spoken at the stone** (51, 52, 41).
9. **The key into the lock lever, then the walkway's three warnings** (39, 40).
10. **The Shotgun from the cellar floor** (32), and the masks lifted off the wall and held to the face (6).

Honourable mentions: the gramophone's tone arm (12), the clock case (11), the letters slid off the writing table (9),
drinking a vial (33), three pulls on the pit's chains (54).

---

## 5. What each needs that is not built yet

**Engine** [R: from reading the sources named; numbers continue chapter 1's N1-N13]:

| # | need | for entries | state |
|---|---|---|---|
| N1 | the record surface box | 5, 9, 13, 18, 29 | built (ledger 18); seen on the owner's PC for walls; **floor boxes** (the plates, the letters) not yet seen on the PC |
| N2 | use / examine at a contact point | 5, 9, 29, 34, 61 | built for the palm and fingertip on walls (ledger 18); a held item's contact as the click point: new |
| N4 | door handles, a key turn | 2, 21, 37, 39, 65 | new (chapter 1) |
| N7 | strike detector | 17 | built (ledger 19) |
| N14 | **socket release**: the held item within 5 cm of a socket and the grip opening = one use at that point; the item stays in the hand on a refusal | 5, 29, 34, 39, 61 | new |
| N15 | the pickup fly retargeted to the hand (chapter 1 N9) | 4, 6, 7, 8, 28, 49, 58 | new |
| N16 | **sit detector**: the head drops ≥ 35 cm while within 0.6 m above a seat sector, facing away from its back | 55 | new |
| N17 | palm press into a plate | 51, 52 | the press attempt zone exists (ledger 21); a press that fires a **use** is new |
| N18 | lever pull (grip + 8 cm along the lever) | 38, 48, 54 | new (WORLD_DESIGN 3.3) |
| N19 | **a melee weapon's swing**: RSQ Melee's reader on the held weapon → the original's attack of that weapon at the blade | 8 (Sword), 58 (Staff, if melee) | the fists' reader is built (ledger 19); a held weapon's: new |
| N20 | body slots for weapons (if Q8 b/c): reach to a slot, grip = the original's select | 7, 8, 32, 58 | new |
| N21 | the choice panel's placement and hand picking (Q11) | 41, 58, 60 | new (the panel itself is the original's) |
| N22 | drink: the vial tipped at the mouth = use on self | 33 | new |
| N23 | twist detector (the Shrive) | 34, 61 | new |
| N24 | two-handed weapons (Shotgun, Staff) | 32, 58 | LATER (left-hand support) |

**Art measurements:** M2 the seven plate boxes on DEMO[66] mapped to the dais (does each box hold its statuette's
spot?) through roth_surfbox's floor texel rule; M3 the Orb aperture on DEMO4[2353] / [2366] in world height; M4 the
handle and lock of DEMO4[4280] / [4246] and the wall mechanisms of `#41` / `#72` (pictures not looked at); M5 the hand
print centres on DEMO4[4288]; M6 the throne's facing (s162's back faces); M7 the masks' hanging points; M8 the Shrive
sockets (AELF wall id 6, MAS3 DEMO4[2830]); M9 the Sword's grip and blade length on DEMO[4129]; M10 the bookcase
leaves' edges (DEMO[254] / [255]).

**Hand poses** (walk the owner's take with `tools/fbxload_test/walk_sheet.py`; don't ask which frame): P8 a sword grip
(check the UZDXREMA weapon grips already copied, ledger 4); P9 `fbx_cup` (985) for the Orb; P10 `fbx_wrap_half` round
the statuette's coils; P11 the open palm (frame 0) for hand prints; P12 a two-handed shotgun hold (LATER); P13
`fbx_halfcurl` (193) to turn the Shrive; P1 (chapter 1) the lateral key pinch for Key i22.

---

## 6. Open questions for the owner (chapter 2)

**Owner's answers (2026-11-13/14; questions 1-12 asked, 13-32 NOT YET ASKED — ask them next):**
- **Q1 statuettes: lower onto the dais and release** = the original's use where the base lands (the plate's box decides); pointer kept.
- **Q2 plate help:** owner: "in flat-screen they auto assign. It never occurred to me they would be specific." No hint chosen. **Check first** (measurement M2): does the original place a statuette on whichever plate is clicked/next free (auto-assign), or does each plate want its own statuette? If the original auto-assigns, the VR release anywhere on the dais must do the same; if plates are specific, bring it back to the owner.
- **Q3 lifting a placed statuette back = the original's click on it: YES.**
- **Q4 the lid (opens in chapter 7): add nothing.**
- **Q5 masks: lift off the wall; held to the face = examine** (built: item to the eyes).
- **Q6 Sword: a physical swing** fires the original's sword attack at the blade, its own rate and damage.
- **Q7 Shield: ALWAYS on the off forearm** (not the recommendation).
- **Q8 weapon switching: both** the inventory and body slots that run the original's select click.
- **Q9 Shotgun: TWO-HANDED NOW** (off hand on the fore-end aims it); the reload/pump waits for adequate meshes (owner will find them).
- **Q10 Staff: read the original's WeaponAction, but in VR it is "a combination projectile and melee"** (owner): a swing is melee and the trigger fires its projectile, each with the original's numbers where the original has them; design after reading.
- **Q11 the choice menu: world-locked where the movie screen stood**, picked by laser or fingertip.
- **Q12 the words of passing: VOICE.** Owner: "Speaking the dialog would be awesome. Later we might entertain voice recognition to more organically unlock dialogs." Now: the player SPEAKS the words (speech recognition of the original's choice texts), the original's choice is what fires. Later: voice for dialogs generally. (New content, approved.)


Each is a concrete choice; **R** marks the recommendation and why.

- **Q1 Setting a statuette on the dais.** (a) Lower it onto the dais and open the hand: one floor use at the texel
  under its base (that plate's box decides); no plate there = nothing happens and it stays in the hand. (b) Pointer
  only. **R: (a)**, with (b) kept as vanilla: it is the original's click, made where Adam's hand is.
- **Q2 Help to find the plates.** (a) Show nothing: the plates are only boxes on the floor picture, as in the original.
  (b) A faint ghost of the statuette when a held one is within 10 cm of a plate (new presentation). **R: (a)** (1:1
  rule; "where the original shows nothing, we show nothing").
- **Q3 Lifting a placed statuette back** = the original's click on it (the count steps down). **R: yes.**
- **Q4 The lid in chapter 2.** Only six statuettes exist, so the lid cannot open. (a) Add nothing. (b) Some hint.
  **R: (a).**
- **Q5 The masks.** (a) Lift off the wall (the use); held to the face = the examine (built X1). (b) Also "wear" a mask
  (new content). **R: (a).**
- **Q6 The Sword.** (a) A physical swing (the fists' reader on the sword) fires the original's sword attack at the
  blade with its own rate and damage; the trigger does nothing. (b) The trigger plays the original's swing as in flat.
  **R: (a)**: it is your Q6 rule for fists, extended to the first melee weapon.
- **Q7 The Shield.** (a) Shown on the weapon hand's forearm only while it is the selected weapon. (b) Always on the off
  forearm. **R: (a)**: the original's later tests need it in hand; (b) would show a state the game does not have.
- **Q8 Switching weapons** (fists, Colt, Shield, Sword, Shotgun, Staff by the end of the chapter). (a) The original's
  inventory only. (b) Body slots (Colt at the hip, Sword / Shotgun / Staff over the shoulder, Shield on the back) whose
  grab runs the original's select click. (c) Both. **R: (c)**: vanilla stays, the VR way is the goal.
- **Q9 The Shotgun.** One hand now, along the gun (W1), the original's cycle and reload; the pump with the off hand
  comes with the real-parts reload (W2, LATER) and left-hand support (LATER). **R: confirm.**
- **Q10 The Staff i12.** (a) Read its WeaponAction first: a short melee bullet → swung like the Sword; a projectile →
  the trigger along the Staff. (b) Decide now without reading. **R: (a).**
- **Q11 Dialogue choices** (first in the game: the altar, the words wall, Florentine). Where the original's choice
  panel stands: (a) on the off-wrist mount with the subtitles, picked by the other hand; (b) world-locked at reading
  distance where the movie screen was, picked by laser or fingertip; (c) head-locked low. **R: (b)**: every choice in
  this chapter follows a movie, so it appears where the player is already looking, and both hands stay free.
- **Q12 Speaking the words** "Chullum ashdar in derias." (a) The original's choice only. (b) Voice recognition (new
  content). **R: (a).**
- **Q13 The altar's Staff.** (a) Grabbing it is the original's click: SHRIVEA and the choice, answered in the panel.
  (b) Lifting it answers "Take the artefact." directly. **R: (a)**: (b) adds an outcome path the original does not
  have.
- **Q14 The Orb door.** Hold the Orb into the aperture and release = `#4`; grab it out of the door = `#24`. **R: yes.**
- **Q15 The Shrive into its sockets** (Aelf's door, the MAS3 recess). (a) Touching the socket fires the use at once;
  a short screw-in plays after as presentation. (b) The 180° twist is the commit (ITEM_DESIGN 14: the click comes 0.5-1
  s later than a mouse's, no game consequence). **R: (b)**, the item's own words ("designed to screw into
  something") ask for it; pointer + grip stays.
- **Q16 The lock lever** (MAUSO1EA `#76`). The Key i22 to the lever's lock and a 30° turn = the use; without it the
  original's "Won't budge. / Locked." and a rattle. **R: yes** (the same gesture as the Study door's key).
- **Q17 Levers and the chains** (MAS4 `#3`; MAUSO1EA `#18`, `#30`, `#101`; MAS7 `#22` three times). Grip the handle and
  pull 8 cm = one use. **R: yes** (WORLD_DESIGN 3.3).
- **Q18 The hand prints** (MAS6 `#42`, `#8`, `#41`). The open palm pressed 2 cm into the print = one use. **R: yes.**
- **Q19 The throne.** (a) Sit: on the step, back to the throne, the head drops to sitting height over the seat = the
  seat's use. (b) Pointer only. **R: (a)**, pointer kept for seated players.
- **Q20 Involuntary falls** (the pit: 9.4 m at 200 units a tick; the cave-in round the player). (a) As the original.
  (b) The original plus a short fade during the drop (a VR comfort adaptation). **R: (a) now** (1:1 until the flat port
  is finished); (b) for the comfort options later.
- **Q21 The letters' boxes** (`#57`'s box is 4 × 3 texels). (a) The contact point must be inside the box. (b) Any part of
  the fingertip or palm footprint overlapping the box sends the click at an overlapping texel (a click the original
  accepts). **R: (b)**: the result is the original's; only the aim is easier.
- **Q22 Documents** (four letters, the Parchment, the Map). (a) As built: in the item hand; to the eyes = the original's
  popup. (b) One-page documents unfolded in both hands like the Book. **R: (a)**; the Map stays the original's picture.
- **Q23 The roll-top desk** (`#288`, in the Study). (a) Keep it shut: the tug plays "The drawers are stuck." (built).
  (b) Design an opening (new content). **R: (a)**.
- **Q24 The clock** (`#35`). (a) Open the case door by hand (the use), nothing else. (b) Also move the hands by hand (new
  until the 0x1f counter is read in ROTH.C). **R: (a)**, and read 0x1f.
- **Q25 The gramophone** (`#33`). The hand on the tone arm (or the crank) = the use. **R: the tone arm.**
- **Q26 Vials.** (a) Tip the vial at the mouth (≥ 60°, within 8 cm under the eyes) = the original's use on self; to the
  eyes stays the examine. (b) Inventory use only. **R: (a)**, with (b) kept.
- **Q27 Darkness in chapter 2.** (a) None: the original's lighting, the Lamp from the hip. (b) The darkness pass in the
  mausoleum maps. **R: (a)**, as chapter 1.
- **Q28 The magic torches** (about 70 in these maps). (a) The original's lighting only. (b) A small dynamic light per
  torch (cosmetic). **R: (a)** until you ask for cosmetic work.
- **Q29 Forced turns** (card 2 `#449`, card 3 `#415`, MAS6 `#56`, MAUSO1EB `#22` / `#23`, AELF `#78`). The built rule:
  snap at the movie's black, else at once. **R: confirm.**
- **Q30 The bookcase doors** (no painted handle). (a) Grip a leaf's edge and pull 4 cm = the use. (b) Pull a book (new).
  **R: (a).**
- **Q31 Ward tingle on the new wards** (AELF DEMO4[4273]; the green sigils ADEMO[235] in the Study). **R: yes** for the
  wards (they glow and are "designed to keep us in"); no for the sigils (no line says they glow).
- **Q32 Physical attempts in this chapter** (the lid's "Won't open.", the heraldic shields, the words wall, the tomb
  lids, the lockless stone door). Where the record is a use the attempt is that use; where an examine, the examine.
  **R: confirm** (your Q4 pattern).

---

## 7. Verified versus reasoning only

**Verified** (how):
- The chapter's area, its rooms, the lantern counts and every record listed: `openables -area` from each arrival (the
  commands in section 0), `chapter_census -mapall` per map, `world_census -chain`, `openables -map` and `-sector` on
  the retail install, this session.
- The arrival rules (keyed and key 0): roth_gameflow.h / .cpp read (map_load.c lines as cited there).
- Chains, boxes, flags, counts, delays, lines, choices and cutscene names: the tools above and `dbase100_dump
  --action` (choices, cutscene numbers by the operand − 1 rule).
- The count starts at 1 and reaches at most 6 in chapter 2: the census (`count15(1->7/7)`) and every record that gives
  i4 (`chapter_census -all`), plus the walks' plain pickups.
- Dead records: no sector carries their id (the census prints no sector, chapter_census.cpp:507-508).
- Flags set and never read (36, 163, 428, 57): `chapter_census -all` and the DBASE100 action dump, grepped.
- Item facts (categories, weapons, OnInspect, OnUse): `item_dossier`.

**Reasoning only, not seen:**
- The play order (the walkthrough reading), what each moving sector looks like moving, where MAS3 `#113`, MAUSO1EA
  `#6` / `#100` and MAS7 `#22` lead, whether the Orb door closes again (`#68` run twice), what the clock's 0x1f does,
  what MAS7's 0x3e does, what light group 27 covers, what the creatures are and how they fight.
- Whether each plate's box holds its statuette's spot (M2); whether the walk's approximations hold everywhere (no
  step timing, no platforms, flags not evaluated: as chapter 1).
- Sizes in metres (E = 1.65 m); "as drawn" sizes of sprites.
- Every VR proposal, haptic, ranking and effort.
- Nothing in this file was checked against the running original or seen in our engine.

---

## 8. Second pass

Not done yet. Chapter 1's second pass (movement, the gun, lines, HUD, death, saving) applies to chapter 2 as decided
(section 9); what chapter 2 adds to it is in 2.1, 2.2 and 2.8.

---

## 9. Changes made for this chapter

Every change, with its decision, commits and evidence, is in [`docs/VR_CHANGES_LEDGER.md`](VR_CHANGES_LEDGER.md).
The rows that reach chapter 2, and where:

| ledger row | change | state (ledger) | chapter 2 entries |
|---|---|---|---|
| 1-3, 6, 7 | FBX hands, poses, scale, the original's font | built | all |
| 4 | held-object and weapon grips | built | 4, 6, 7, 8, 28, 32, 49, 58 |
| 5, 10, 26 | the item in the right hand; a use carries it; shown in the hand | built | 5, 29, 34, 39, 61 (the sockets) |
| 8, 9, 11-14 | picker, buttons, inventory by hand, pause, skip a line | built | all |
| 15 | 0x0d key-0 art swap | built | 3 (`#18`) |
| 16, 23, 24 | candles wick by wick, tall wicks, the struck match | built | 3 (four candelabra) |
| 17 | darkness pass | built, not used | none (Q27) |
| 18 | surface boxes | built, seen (walls) | 5, 9, 13, 18, 29 (floor boxes not yet seen) |
| 19 | physical melee | built, seen | 17, 22 and every creature; extended by N19 (Q6) |
| 20, 21 | knock, physical attempts | built, seen | 5 (`#304`), 13, 15, 19, 41, 44, 53, 56, 61, 64 |
| 22 | ward tingle | built, seen | 60 (Q31) |
| 25 | the Lamp on the hip | built | 24-64 (the lantern maps) |
| 29 | forced turn at black | built | 1, 52, 58, 62, 65 |
| 30 | the original's movement | built, diffed | 24, 42, 55 (stairs, the pit, the throne steps) |
| 33 | guns along the gun | built | 22 and every fight; the Shotgun (32) |
| 34 | the off-wrist mount, low-health cue | built | every line; the traps (2.8) |
| 35 | world-locked movies | built | 1, 40, 55, 58, 60, 62, 65 |
| 36 | quicksave / quickload | built | all |
| 37 | item to the eyes = examine | built | 6, 9, 30, 31 (documents) |
| — | mirrors keep the glass; desks never open | decided | 19 (Q23) |

**What chapter 2 needs that is not built:** N14 socket release, N15 fly to the hand, N16 sit, N17 press-to-use, N18
lever pull, N19 melee weapon swing, N20 weapon body slots (if Q8), N21 the choice panel's placement, N22 drink, N23
twist, N4 door handles and key turn (chapter 1's, still new); M2-M10; P8-P13 (section 5).

---

## 10. Every trigger record in the chapter

Generated from `chapter_census -mapall` (kind, target, the census's chain notation) with the first line each record
says (from `openables`), and the entry it belongs to. STUDY1: the Study, the passage, and `#265` / `#449`.
**298 records.**

**STUDY1** (75 records)

```
#13   2      use-wall wall id 4 tex 218 sec 22
        door (free) | door act527{ say }
        "Open. Good."
#18   3      click-obj obj 2 art 0:2 sec 217 x4
        light (fire item) | ifi16@hand objid delay30 objid tex light
#23   4      click-obj obj 34 "Statuette" sec 212
        take item (scripted) | give4
#33   12     use-wall wall id 12 tex 17 sec 291
        switch / lever | tex [off] count15(0->1/2) delay120 act26{ +flag12 say ifflag1 say } toggle#6
        "What the hell's that? / Turn it off for heaven's sake."
#34   9      use-floor sector id 2 (sec 413) [once]
        take item (scripted) | give119 tex +flag163
#35   11     use-wall wall id 23 tex 726 sec 399
        door (free) | act27{ ifflag292 say } count1f(1->11/12) door else->#250 if!flag293 act27{ ifflag292 say } delay400 <else #...
        "Still doesn't make any damn sense."
#38   9      use-floor sector id 3 (sec 320) [once]
        take item (scripted) | tex give11
#53   9      use-floor sector id 25 (sec 414) [once]
        take item (scripted) | give118 tex
#57   9      use-floor sector id 24 (sec 415) [once]
        take item (scripted) | give117 tex
#58   4      click-obj obj 33 "Statuette" sec 214
        take item (scripted) | give4
#74   4      click-obj obj 35 "Statuette" sec 318
        take item (scripted) | give4
#75   65     use-wall wall id 43 tex 255 sec 228
        chapter card | else->#421 ifflag82 else->#421 if!flag55 delay10 act177{ ifflag82 +flag55 GDV:REBECCA GDV:CHPTR3 +flag1 giv...
#108  15     exam-floor sector id 40 (sec 226)
        examine (comment) | act17{ say ifflag1 say }
        "This's intricate. Look at the work in this. / It's got some sort of..."
#119  5      use-floor sector id 13 (sec 221) [once]
        place / hand over item | onfail:act590 ifi4@hand take4 spawn count15(1->7/7) act23{ say ifflag1 say } height
        "(refusal) Doesn't appear to be working."
#121  5      click-obj obj 22 "Statuette" sec 221
        take item (scripted) | give4 step#156 <when #156(1->7) done: act23{ say ifflag1 say } height > toggle#119 spawn
#124  5      use-floor sector id 13 (sec 221) [once]
        place / hand over item | onfail:act590 ifi4@hand take4 spawn count15(1->7/7) act23{ say ifflag1 say } height
        "(refusal) Doesn't appear to be working."
#125  5      use-floor sector id 13 (sec 221) [once]
        place / hand over item | onfail:act590 ifi4@hand take4 spawn count15(1->7/7) act23{ say ifflag1 say } height
        "(refusal) Doesn't appear to be working."
#126  5      use-floor sector id 13 (sec 221) [once]
        place / hand over item | onfail:act590 ifi4@hand take4 spawn count15(1->7/7) act23{ say ifflag1 say } height
        "(refusal) Doesn't appear to be working."
#127  5      use-floor sector id 13 (sec 221) [once]
        place / hand over item | onfail:act590 ifi4@hand take4 spawn count15(1->7/7) act23{ say ifflag1 say } height
        "(refusal) Doesn't appear to be working."
#128  5      use-floor sector id 13 (sec 221) [off] [once]
        place / hand over item | onfail:act590 ifi4@hand take4 spawn count15(1->7/7) act23{ say ifflag1 say } height
        "(refusal) Doesn't appear to be working."
#129  5      use-floor sector id 13 (sec 221) [once]
        place / hand over item | onfail:act590 ifi4@hand take4 spawn count15(1->7/7) act23{ say ifflag1 say } height
        "(refusal) Doesn't appear to be working."
#136  5      click-obj obj 31 "Statuette" sec 362
        take item (scripted) | give4 step#156 <when #156(1->7) done: act23{ say ifflag1 say } height > toggle#129 spawn
#137  5      click-obj obj 23 "Statuette" sec 362
        take item (scripted) | give4 step#156 <when #156(1->7) done: act23{ say ifflag1 say } height > toggle#127 spawn
#138  5      click-obj obj 24 "Statuette" sec 221
        take item (scripted) | give4 step#156 <when #156(1->7) done: act23{ say ifflag1 say } height > toggle#128 spawn
#139  5      click-obj obj 21 "Statuette" sec 221
        take item (scripted) | give4 step#156 <when #156(1->7) done: act23{ say ifflag1 say } height > toggle#126 spawn
#140  5      click-obj obj 20 "Statuette" sec 221
        take item (scripted) | give4 step#156 <when #156(1->7) done: act23{ say ifflag1 say } height > toggle#125 spawn
#141  5      click-obj obj 19 "Statuette" sec 222
        take item (scripted) | give4 step#156 <when #156(1->7) done: act23{ say ifflag1 say } height > toggle#124 spawn
#171  6      click-obj obj 41 art 0:109 sec 401 [once]
        take item (scripted) | give26
#181  17     hit-wall wall id 24 tex 742 sec 20
        ambient | tex damage particles particles particles
#208  18     use-wall wall id 14 tex 21 sec 266
        ambient | (entry #209)
#242  15     exam-floor sector id 19 (sec 402)
        examine (comment) | act17{ say ifflag1 say }
        "This's intricate. Look at the work in this. / It's got some sort of..."
#243  15     exam-floor sector id 27 (sec 401)
        examine (comment) | act17{ say ifflag1 say }
        "This's intricate. Look at the work in this. / It's got some sort of..."
#244  16     exam-obj obj 17 art 0:13 sec 9 x17
        examine (comment) | act24{ say ifflag1 say }
        "Plain old chair. / Guess so."
#248  12     exam-wall wall id 12 tex 17 sec 291
        examine (comment) | act25{ skip-ifflag12 say if!flag12 say ifflag1 say }
        "This Gramophone still works. Shame about the taste in music. / This..."
#249  12     exam-floor sector id 1 (sec 367)
        examine (comment) | act25{ skip-ifflag12 say if!flag12 say ifflag1 say }
        "This Gramophone still works. Shame about the taste in music. / This..."
#251  11     exam-wall wall id 23 tex 726 sec 399
        flag set | act29{ say +flag292 }
        "Hand's not working. But it's still ticking. Doesn't make any damn s..."
#265  23     enter-sector sector id 39 (sec 25) [off] [once]
        walk-in event | spawn
#288  19     exam-obj obj 1 art 0:17 sec 361
        examine (comment) | act100{ say ifflag1 say }
        "The drawers are stuck. / The canopy's locked too. And the lock's be..."
#289  6      exam-obj obj 40 art 0:107 sec 401
        examine (comment) | act101{ say ifflag1 say }
        "Wooden voodoo mask. / God. These things give me nightmares."
#290  6      exam-obj obj 41 art 0:109 sec 401
        examine (comment) | act103{ say ifflag1 say }
        "Theatrical Mask. / Porcelain I believe."
#291  6      exam-obj obj 32 art 0:108 sec 401
        examine (comment) | act102{ say }
        "Japanese."
#292  16     exam-obj obj 42 art 0:4 sec 217
        examine (comment) | act104{ say ifflag1 say }
        "Coalscuttle. Just an ordinary Coalscuttle. / Silver isn't it? Must ..."
#293  13     exam-wall wall id 1 tex 79 sec 289
        flag-gated use | act105{ skip-if!flag1 say ifflag1 skip-ifflag39 say if!flag39 say +flag39 say }
        "Star motif. / You seem to know a lot about these don't you. / Star ..."
#294  5      exam-wall wall id 31 tex 630 sec 221
        flag set | act96{ say ifflag1 say } +flag36
        "Sarcophagus. Egyptian. / Authentic too."
#299  1      enter-sector sector id 29 (sec 266) [once]
        chapter card | act178{ +flag422 GDV:RANDALL GDV:CHPTR2 give70 rawobj32010 } <callobj32010 #449: WARP:STUDY1@34 turnplayer ...
#301  6      click-obj obj 32 art 0:108 sec 401
        take item (scripted) | give24
#303  10     exam-floor sector id 28 (sec 384)
        examine (comment) | act47{ say ifflag1 say }
        "Table. Used recently. / You're right, the dust's been disturbed."
#304  5      use-wall wall id 31 tex 630 sec 221
        comment | act115{ say ifflag1 say }
        "Won't open. I can't see any hinges or anything. / I tried to open i..."
#306  13     exam-wall wall id 1 tex 79 sec 289
        examine (comment) | act116{ say ifflag1 say }
        "Hmm. Lion design. / Nice condition."
#311  16     exam-obj obj 45 art 0:10 sec 217
        examine (comment) | act120{ say ifflag1 say }
        "Not much to search. / Could be a clue anywhere remember."
#312  14     exam-wall wall id 33 tex 181 sec 225
        examine (comment) | act114{ say ifflag1 say ifflag37 say }
        "What's that book he's reading? / Looks like a diary. / That book's ..."
#315  3      exam-obj obj 2 art 0:2 sec 217 x4
        examine (comment) | act123{ say ifflag1 say }
        "Candle. / Tallow I believe."
#317  7      exam-obj obj 48 art 0:36 sec 317
        examine (comment) | act125{ say ifflag1 say skip-ifflag41 say }
        "Dragon. This is scorched quite badly. / Quite recently too. / We kn..."
#318  13     use-wall wall id 1 tex 79 sec 289
        comment | act126{ say ifflag1 say }
        "This one's stuck fast. / There are a few famous knights who have us..."
#320  7      click-obj obj 48 art 0:36 sec 317
        take item (scripted) | act128{ ifflag1 say } give28
        "The dragon's widely used in heraldry though I've not seen this desi..."
#322  13     use-wall wall id 1 tex 79 sec 289
        comment | act127{ say ifflag1 say }
        "This is fastened to the wall. / The star - the charge - it's called..."
#324  8      exam-obj obj 43 art 0:73 sec 361 x2
        examine (comment) | act129{ say ifflag1 say }
        "Mint condition. / Old too."
#325  8      exam-obj obj 49 art 2:114 sec 361
        examine (comment) | act131{ say }
        "Claymore?"
#326  8      exam-obj obj 50 art 2:115 sec 361
        examine (comment) | act132{ say }
        "Scimitar."
#327  8      exam-obj obj 44 art 0:33 sec 362
        examine (comment) | act410{ say }
        "Behind all this muck I'll bet there's a sword. Or at least there on..."
#332  8      click-obj obj 44 art 0:33 sec 362
        take item (scripted) | act133{ say ifflag1 say } give29
        "This one's come away from the rack. Despite the muck it might be us..."
#351  6      click-obj obj 40 art 0:107 sec 401
        take item (scripted) | give25
#353  21     use-wall wall id 38 tex 254 sec 214
        door (free) | door door
#354  65     use-wall wall id 39 tex 255 sec 454
        chapter card | else->#421 ifflag82 else->#421 if!flag55 delay10 act177{ ifflag82 +flag55 GDV:REBECCA GDV:CHPTR3 +flag1 giv...
#362  22     enter-sector sector id 42 (sec 454) [once]
        walk-in event | spawn act674{ random say }
        "Looks like we've got company. / What the Hell? / Oh God. / Jeez! Wh..."
#380  5      enter-sector sector id 33 (sec 351) [off] [once]
        comment | act246{ say ifflag1 say }
        "What's that smell? / Sulphur. Something's been burning."
#382  18     switch obj 55 art 0:88 sec 361
        light switch | lightswitch
#404  24     enter-sector sector id 38 (sec 448)
        map transition | WARP:MAS3@0
#410  10     click-obj obj 1000 "Colt-45 magazine" sec 41 x3
        take item (scripted) | give(clicked)
#420  21     use-wall wall id 42 tex 254 sec 214
        door (free) | door door
#424  20     click-obj obj 66 art 2:235 sec 222
        gated map transition | onfail:act590 ifi33@hand delay20 +flag71 WARP:CAVERNS3@21 act534{ GDV:TUNNWARP }
        "(refusal) Doesn't appear to be working."
#445  20     click-obj obj 65 art 2:235 sec 362
        gated map transition | onfail:act590 ifi33@hand delay20 +flag71 WARP:AELF@8 act534{ GDV:TUNNWARP }
        "(refusal) Doesn't appear to be working."
#449  1      click-obj id 32010 (not placed) <-act178(STUDY1 #300)
        map transition | WARP:STUDY1@34 turnplayer
#540  4      click-obj obj 56 "Statuette" sec 216
        take item (scripted) | give4
#547  18     exam-obj obj 55 art 0:88 sec 361
        examine (comment) | act654{ say }
        "Light switch."
```

**MAS3** (37 records)

```
#1    28     click-obj obj 1 "Orb" sec 28
        take item (scripted) | give18
#2    27     hit-obj obj 3 "Monster" sec 192
        ambient | objid
#4    29     use-wall wall id 2 tex 2353 sec 29 [once]
        place / hand over item | ifi18@hand take18 tex delay120 enable#24 <sub #68: height tex > delay120 <sub #65: tex > delay400 <sub #68:...
#5    27     enter-sector sector id 5 (sec 159)
        ambient | delay200~rnd
#7    29     exam-wall wall id 2 tex 2353 sec 29
        examine (comment) | act147{ say }
        "There's a small aperture in the middle of this door."
#8    35     exam-floor sector id 2 (sec 41)
        examine (comment) | act148{ say ifflag1 say }
        "Just an old wooden chair. / It's seen better days."
#9    35     exam-floor sector id 3 (sec 44)
        examine (comment) | act149{ say }
        "Colour's faded badly on the weave. Not surprising considering the s..."
#10   30     use-floor sector id 4 (sec 97) [once]
        take item (scripted) | tex give13
#13   25     enter-sector sector id 6 (sec 131)
        map transition | WARP:STUDY1@0
#15   36     enter-sector sector id 7 (sec 112)
        map transition | WARP:MAUSO1EA@0
#17   36     enter-sector sector id 8
        map transition | WARP:MAUSO1EA@0
#19   27     enter-sector sector id 3 (sec 44) [once]
        walk-in event | spawn
#24   29     use-wall wall id 2 tex 2353 sec 29 [off] [once]
        take item (scripted) | give18 tex enable#4
#25   27     enter-sector sector id 16 (sec 102) [once]
        walk-in event | act674{ random say } spawn spawn spawn spawn
        "Looks like we've got company. / What the Hell? / Oh God. / Jeez! Wh..."
#31   27     hit-obj id 20012 (not placed)
        ambient | spawn particles particles particles
#41   29     tex-anim [off]
        ambient | tex
#43   29     tex-anim [off]
        ambient | tex
#45   32     click-obj obj 1000 "Colt-45 magazine" sec 19 x11
        take item (scripted) | give(clicked)
#47   32     click-obj obj 1001 "Cartridges" sec 24 x2
        take item (scripted) | give(clicked)
#49   27     enter-sector sector id 11 (sec 193) [once]
        walk-in event | spawn
#51   27     enter-sector sector id 12 (sec 192) [once]
        walk-in event | spawn
#53   27     enter-sector sector id 13 (sec 156)
        walk-in event | spawn count15(0->2/2) toggle#53
#57   27     enter-sector sector id 14 (sec 110)
        walk-in event | act674{ random say } spawn spawn
        "Looks like we've got company. / What the Hell? / Oh God. / Jeez! Wh..."
#60   32     click-obj obj 8 "Cartridges" sec 24
        take item (scripted) | give(clicked) spawn act674{ random say }
        "Looks like we've got company. / What the Hell? / Oh God. / Jeez! Wh..."
#67   27     enter-sector sector id 15 (sec 161) [once]
        walk-in event | spawn act674{ random say } light delay140 light
        "Looks like we've got company. / What the Hell? / Oh God. / Jeez! Wh..."
#83   26     enter-sector sector id 17 (sec 194) [once]
        walk-in event | else->#86 if!i9 height height spawn
#88   26     exam-obj obj 15 art 0:130 sec 241 x4
        examine (comment) | act370{ say ifflag1 say }
        "Bone. Looks human / It is."
#89   26     exam-wall wall id 1 tex 2353 sec 238
        examine (comment) | act202{ say ifflag1 say }
        "Plenty of this rubble around. / The structure doesn't look too safe..."
#98   25     exam-obj obj 17 art 0:161 sec 120 x32
        examine (comment) | act667{ say ifflag1 say }
        "Torch. Wonder who put that there? / There's power coming from the t..."
#99   35     exam-obj obj 18 art 0:181 sec 13 x10
        examine (comment) | act668{ say }
        "Someone's coat of arms maybe."
#100  35     exam-floor sector id 21 (sec 218)
        examine (comment) | act669{ say }
        "Some strange symbol by the looks of it."
#102  27     hit-obj id 19 (not placed)
        comment | act672{ random ->act675{ random say } ->act676{ random say } }
#107  32     click-obj obj 20 "Shotgun" sec 102
        take item (scripted) | give(clicked)
#109  33     click-obj obj 21 "Vial" sec 43 x3
        take item (scripted) | give(clicked)
#110  31     exam-obj obj 22 art 2:101 sec 81
        examine (comment) | act629{ random say }
        "A map. But of where? / Map. But to where?"
#111  31     click-obj obj 22 art 2:101 sec 81
        take item (scripted) | give273
#113  34     use-wall wall id 6 tex 2830 sec 223
        use item on object | onfail:act590 ifi14@hand height delay200 tex
        "(refusal) Doesn't appear to be working."
```

**MAUSO1EA** (63 records)

```
#1    41     click-obj id 65529 (not placed)
        ambient | (entry #6)
#5    41     use-wall wall id 7 tex 2790 sec 331
        comment | onfail:act235 if!flag116 act265{ if!flag433 say }
        "(refusal) Keep silent / Speak words of power / Chullum ashdar in de..."
#7    44     enter-sector sector id 26 (sec 65)
        walk-in event | +flag83
#8    44     enter-sector sector id 2 (sec 29) [once]
        ambient | tex
#15   39     use-wall wall id 12 tex 2321 sec 193
        comment | act271{ say }
        "How's this suppose to open?"
#16   37     use-wall wall id 5 tex 2364 sec 151
        door (free) | door
#18   38     click-obj obj 10 art 0:184 sec 2 x2
        switch / lever | height
#20   42     enter-sector sector id 6 (sec 110) [once]
        map transition | delay4 disable#20 height damage WARP:MAS7@13
#23   44     enter-sector sector id 4 (sec 10)
        walk-in event | act213{ +flag206 }
#24   47     enter-sector sector id 16
        map transition | WARP:MAS3@0
#25   47     enter-sector sector id 5
        map transition | WARP:MAS7@13
#27   42     enter-sector sector id 6 (sec 110)
        map transition | WARP:MAS7@13
#30   38     click-obj obj 5 art 0:184 sec 191 x2
        switch / lever | height
#33   39     use-wall wall id 9 tex 2321 sec 192
        switch / lever | height
#35   45     exam-obj id 7 (not placed)
        examine (comment) | act164{ say ifflag1 say }
#36   37     exam-wall wall id 10 tex 2426 sec 404
        examine (comment) | act165{ say ifflag1 say }
        "What's that schpiel? What's all this about? / It's a passage from t..."
#37   37     use-wall wall id 10 tex 2426 sec 404
        door (free) | door
#39   43     exam-obj obj 4 "Monster" sec 285
        examine (comment) | act169{ say }
        "One ugly mother."
#40   40     enter-sector sector id 9 (sec 207) [once]
        cutscene | act184{ skip-ifflag62 GDV:WALKWY1A if!flag62 GDV:WALKWAY1 }
#41   38     use-wall wall id 6 tex 2353 sec 185
        switch / lever | height
#44   40     enter-sector sector id 10 (sec 206) [once]
        cutscene | act185{ if!flag62 GDV:WALKWAY2 }
#46   40     enter-sector sector id 11 (sec 209) [once]
        cutscene | act186{ if!flag62 rawobj32008 GDV:WALKWAY3 } <callobj32008 #95: damage >
#48   43     exam-obj obj 2 art 2:164 sec 264 x2
        examine (comment) | act663{ say }
        "One ugly mother."
#51   47     enter-sector sector id 17 (sec 221)
        map transition | WARP:MAS3@0
#53   47     enter-sector sector id 18 (sec 189)
        map transition | WARP:MAS4@0
#55   47     enter-sector sector id 19 (sec 401)
        map transition | WARP:MAS6@0
#57   47     enter-sector sector id 20 (sec 306)
        map transition | WARP:MAS7@0
#59   47     enter-sector sector id 21
        map transition | WARP:MAS5@0
#61   47     enter-sector sector id 22 (sec 103)
        map transition | WARP:AELF@0
#65   47     enter-sector sector id 23 (sec 393)
        map transition | WARP:MAUSO1EB@0
#70   43     hit-obj obj 14 "Monster" sec 264
        ambient | spawn
#72   38     use-wall wall id 8 tex 2679 sec 35
        switch / lever | height
#75   43     hit-obj obj 3 "Monster" sec 86 x4
        ambient | objid
#76   39     click-obj obj 6 art 0:150 sec 76 [once]
        use item on object | onfail:act264 ifi22@hand height +flag428
        "(refusal) Won't budge. / Locked."
#80   43     hit-obj id 4000 (not placed)
        comment | spawn particles particles particles objid act672{ random ->act675{ random say } ->act676{ random say } }
#86   46     click-obj obj 1000 "Cartridges" sec 164 x10
        take item (scripted) | give(clicked)
#88   46     click-obj id 1001 (not placed)
        take item (scripted) | give(clicked)
#93   43     hit-obj id 65533 (not placed)
        ambient | damage
#95   40     click-obj id 32008 (not placed) <-act186(MAUSO1EA #47)
        ambient | damage
#98   41     exam-wall wall id 7 tex 2790 sec 331
        examine (comment) | act481{ say }
        "This isn't gonna move in a hurry."
#99   38     enter-sector sector id 27 (sec 39) [once]
        ambient | (entry #100)
#101  38     click-obj obj 8 art 0:184 sec 259
        ambient | (entry #100)
#102  44     exam-wall wall id 1 tex 65535 sec 36
        examine (comment) | act664{ say }
        "Sarcophagus. Cold to touch."
#103  44     exam-wall wall id 2 tex 2353 sec 36
        examine (comment) | act664{ say }
        "Sarcophagus. Cold to touch."
#104  46     click-obj id 9 (not placed)
        take item (scripted) | give143
#106  44     exam-wall wall id 3 tex 65535 sec 35
        examine (comment) | act664{ say }
        "Sarcophagus. Cold to touch."
#107  44     exam-wall wall id 4 tex 2353 sec 36
        examine (comment) | act664{ say }
        "Sarcophagus. Cold to touch."
#108  44     exam-floor sector id 3 (sec 36)
        examine (comment) | act665{ say }
        "Depiction of a knight. No name."
#109  38     exam-obj obj 5 art 0:184 sec 191 x2
        examine (comment) | act666{ say }
        "Lever. Still works."
#110  38     exam-obj obj 10 art 0:184 sec 2 x2
        examine (comment) | act666{ say }
        "Lever. Still works."
#111  45     exam-obj obj 11 art 0:176 sec 144 x39
        examine (comment) | act667{ say ifflag1 say }
        "Torch. Wonder who put that there? / There's power coming from the t..."
#112  45     exam-obj obj 12 art 0:30 sec 86 x16
        examine (comment) | act306{ say ifflag1 say }
        "Knight or some soldier. Definitely Norman period though. / You aren..."
#113  45     exam-obj obj 13 art 0:181 sec 182 x5
        examine (comment) | act668{ say }
        "Someone's coat of arms maybe."
#114  45     exam-obj obj 15 art 0:182 sec 237
        examine (comment) | act639{ random say }
        "The metal's still good. That surprises me. / The metal in these cha..."
#115  45     exam-obj obj 16 art 0:126 sec 206 x5
        examine (comment) | act370{ say ifflag1 say }
        "Bone. Looks human / It is."
#116  45     exam-wall wall id 13 tex 2815 sec 191
        examine (comment) | act669{ say }
        "Some strange symbol by the looks of it."
#118  43     hit-obj id 17 (not placed)
        comment | act672{ random ->act675{ random say } ->act676{ random say } } objid
#121  38     exam-wall wall id 6 tex 2353 sec 185
        examine (comment) | act152{ say ifflag1 say }
        "The metal's still good. That surprises me. / There's a residual tra..."
#124  65     enter-sector sector id 31 (sec 94) [once]
        flag-gated event | ifflag82 spawn spawn act674{ random say }
        "Looks like we've got company. / What the Hell? / Oh God. / Jeez! Wh..."
#128  43     enter-sector sector id 32 (sec 402) [once]
        walk-in event | spawn
#131  43     hit-obj id 21 (not placed)
        comment | act672{ random ->act675{ random say } ->act676{ random say } } objid
#136  42     enter-sector sector id 33 (sec 298)
        map transition | delay4 disable#20 height damage WARP:MAS7@13
#139  41     click-obj id 65529 (not placed)
        switch / lever | spawn spawn spawn spawn
```

**MAS4** (16 records)

```
#1    48     use-wall wall id 1 tex 2364 sec 127
        door (free) | door
#3    48     click-obj obj 1 art 0:184 sec 240
        door (free) | door tex
#7    49     click-obj obj 2 art 2:242 sec 133 [once]
        take item (scripted) | give22 spawn enable#27 spawn light
#10   50     enter-sector sector id 4 (sec 251)
        map transition | WARP:MAUSO1EA@0
#11   50     exam-floor sector id 3 (sec 215)
        examine (comment) | act669{ say }
        "Some strange symbol by the looks of it."
#14   48     exam-obj obj 1 art 0:184 sec 240
        examine (comment) | act666{ say }
        "Lever. Still works."
#15   50     exam-obj obj 6 art 0:161 sec 130 x18
        examine (comment) | act667{ say ifflag1 say }
        "Torch. Wonder who put that there? / There's power coming from the t..."
#16   50     enter-sector sector id 5
        map transition | WARP:MAS6@0
#18   50     hit-obj obj 3 "Monster" sec 193 x2
        ambient | objid
#20   50     hit-obj id 2040 (not placed)
        comment | spawn act672{ random ->act675{ random say } ->act676{ random say } } particles particles particles particle...
#24   50     exam-obj obj 7 art 0:181 sec 194 x9
        examine (comment) | act668{ say }
        "Someone's coat of arms maybe."
#27   49     enter-sector sector id 2 (sec 134) [off] [once]
        walk-in event | act217{ say } delay200 spawn spawn
        "Your presence violates the pact, as it is written, as it was, so sh..."
#30   50     exam-obj obj 8 art 0:182 sec 190 x6
        examine (comment) | act639{ random say }
        "The metal's still good. That surprises me. / The metal in these cha..."
#43   50     click-obj obj 1000 "Colt-45 magazine" sec 31 x9
        take item (scripted) | give(clicked)
#45   50     click-obj obj 1001 "Cartridges" sec 31 x7
        take item (scripted) | give(clicked)
#47   50     hit-obj id 9 (not placed)
        comment | act672{ random ->act675{ random say } ->act676{ random say } }
```

**MAS6** (26 records)

```
#5    52     click-obj obj 6 art 0:194 sec 9 [off]
        switch / lever | act416{ say } enable#54
        "Chullum ashdar in derias?"
#8    51     click-obj obj 2 art 0:192 sec 14
        switch / lever | height objid count15(0->3/3) spawn height [off] light
#9    52     use-wall wall id 1 tex 2797 sec 9 [off]
        switch / lever | act416{ say } enable#54
        "Chullum ashdar in derias?"
#10   52     use-wall wall id 2 tex 2797 sec 9 [off]
        switch / lever | act416{ say } enable#54
        "Chullum ashdar in derias?"
#11   53     click-obj id 1000 (not placed)
        take item (scripted) | give(clicked)
#13   53     click-obj id 1001 (not placed)
        take item (scripted) | give(clicked)
#20   52     click-obj obj 3 art 0:192 sec 38 [once]
        switch / lever | height tex spawn disable#31 +flag116 enable#5 enable#9 enable#10 spawn
#23   53     enter-sector sector id 9 (sec 53)
        map transition | WARP:MAUSO1EA@0
#25   53     enter-sector sector id 10
        map transition | WARP:MAS4@0
#27   53     enter-sector sector id 11
        map transition | WARP:MAS7@11
#29   53     timer [once]
        timer | delay120~rnd if!flag154 spawn count15(0->4/4) [off] disable#29 ~flag154
#34   53     hit-obj id 2001 (not placed)
        comment | spawn particles particles particles objid act672{ random ->act675{ random say } ->act676{ random say } }
#36   53     exam-obj obj 7 art 0:176 sec 7 x5
        examine (comment) | act667{ say ifflag1 say }
        "Torch. Wonder who put that there? / There's power coming from the t..."
#38   53     exam-obj obj 8 art 0:181 sec 20 x2
        examine (comment) | act668{ say }
        "Someone's coat of arms maybe."
#39   53     exam-obj obj 9 art 0:182 sec 7 x2
        examine (comment) | act639{ random say }
        "The metal's still good. That surprises me. / The metal in these cha..."
#40   53     exam-wall wall id 3 tex 2399 sec 3
        examine (comment) | act664{ say }
        "Sarcophagus. Cold to touch."
#41   51     click-obj obj 4 art 0:192 sec 19
        switch / lever | height objid count15(0->3/3) spawn height [off] light
#42   51     click-obj obj 5 art 0:192 sec 17 [once]
        switch / lever | height objid count15(0->3/3) spawn height [off] light
#48   53     hit-obj id 2000 (not placed)
        attack / kill | objid step#32 <when #32(0->4) done: [off] disable#29 ~flag154 >
#54   52     enter-sector sector id 13 (sec 12) [off]
        map transition | act534{ GDV:TUNNWARP } WARP:MAUSO1EA@29 turnplayer
#59   53     enter-sector sector id 15 [once]
        walk-in event | height
#62   53     exam-floor sector id 7 (sec 3)
        examine (comment) | act155{ say ifflag1 say }
        "Depiction of a knight. No name. / Doubt we could get the lid off. T..."
#63   53     exam-floor sector id 13 (sec 12)
        examine (comment) | act669{ say }
        "Some strange symbol by the looks of it."
#64   53     exam-floor sector id 16 (sec 29)
        examine (comment) | act669{ say }
        "Some strange symbol by the looks of it."
#65   53     exam-wall wall id 4 tex 2815 sec 53
        examine (comment) | act669{ say }
        "Some strange symbol by the looks of it."
#67   53     enter-sector sector id 17 (sec 25) [once]
        comment | act673{ random say }
        "What the Hell's that? / This just gets better all the time. / Hey! ..."
```

**MAS7** (24 records)

```
#3    56     enter-sector sector id 2 (sec 0)
        map transition | WARP:MAUSO1EA@0
#7    55     use-floor sector id 8 (sec 162) [once]
        cutscene | act183{ skip-ifflag56 say if!flag56 +flag56 GDV:BRANDED +flag62 } +flag62
        "Hell. You won't get me sat on that again."
#10   56     click-obj id 1000 (not placed)
        take item (scripted) | give(clicked)
#12   56     click-obj id 1001 (not placed)
        take item (scripted) | give(clicked)
#14   55     use-wall wall id 1 tex 2328 sec 162
        cutscene | act183{ skip-ifflag56 say if!flag56 +flag56 GDV:BRANDED +flag62 } +flag62
        "Hell. You won't get me sat on that again."
#15   56     enter-sector sector id 4 (sec 7)
        comment | act677{ say }
        "This is weird. What on earth is this all about?"
#20   56     enter-sector sector id 6 (sec 291)
        walk-in event | height
#22   54     use-wall wall id 4 tex 2404 sec 276
        switch / lever | count15(0->3/3) height delay300
#26   54     exam-wall wall id 3 tex 2406 sec 250
        examine (comment) | act153{ say ifflag1 say }
        "The metal in these chains is untarnished. / They're inhued with som..."
#27   54     exam-wall wall id 4 tex 2404 sec 276
        examine (comment) | act152{ say ifflag1 say }
        "The metal's still good. That surprises me. / There's a residual tra..."
#28   56     exam-floor sector id 0 (sec 1)
        examine (comment) | act154{ say ifflag1 say }
#29   54     click-obj obj 2 "Vial" sec 337
        take item (scripted) | give143
#30   55     exam-wall wall id 1 tex 2328 sec 162
        examine (comment) | act661{ skip-if!flag56 say ifflag56 say }
        "Impressive. Very impressive. Though who'd sit here in the middle of..."
#32   55     exam-floor sector id 8 (sec 162)
        examine (comment) | act661{ skip-if!flag56 say ifflag56 say }
        "Impressive. Very impressive. Though who'd sit here in the middle of..."
#33   55     use-floor sector id 8 (sec 162)
        cutscene | act183{ skip-ifflag56 say if!flag56 +flag56 GDV:BRANDED +flag62 } +flag62
        "Hell. You won't get me sat on that again."
#34   56     exam-floor sector id 10 (sec 273)
        examine (comment) | act662{ say }
#35   56     exam-wall wall id 2 tex 2400 sec 262
        examine (comment) | act662{ say }
#36   56     exam-obj obj 3 art 0:176 sec 241 x9
        examine (comment) | act667{ say ifflag1 say }
        "Torch. Wonder who put that there? / There's power coming from the t..."
#37   56     exam-obj obj 4 art 0:181 sec 229 x3
        examine (comment) | act668{ say }
        "Someone's coat of arms maybe."
#38   56     exam-obj obj 5 art 0:182 sec 240 x2
        examine (comment) | act639{ random say }
        "The metal's still good. That surprises me. / The metal in these cha..."
#39   56     exam-floor sector id 11 (sec 10)
        examine (comment) | act155{ say ifflag1 say }
        "Depiction of a knight. No name. / Doubt we could get the lid off. T..."
#40   56     exam-wall wall id 5 tex 65535 sec 9
        examine (comment) | act664{ say }
        "Sarcophagus. Cold to touch."
#41   54     enter-sector sector id 12 (sec 250) [once]
        comment | act673{ random say }
        "What the Hell's that? / This just gets better all the time. / Hey! ..."
#43   56     exam-floor sector id 7 (sec 276)
        examine (comment) | act669{ say }
        "Some strange symbol by the looks of it."
```

**MAUSO1EB** (19 records)

```
#1    57     enter-sector sector id 1 (sec 4)
        ambient | damage
#3    57     exam-obj obj 1 art 0:107 sec 254
        examine (comment) | act161{ say }
        "White marble. There's a sense of calm around these things."
#4    58     use-wall wall id 1 tex 2154 sec 108
        map transition | act174{ if!flag190 skip-if!flag189 GDV:SHRIVEA +flag189 choice choice choice
        "Take the artefact. / Ignore the voice. / Leave."
#5    58     exam-wall wall id 1 tex 2154 sec 108
        examine (comment) | act162{ skip-if!flag63 say skip-ifflag63 say }
        "Marble. Altar stone probably. / Two intertwined serpents."
#7    59     enter-sector sector id 4 (sec 276)
        map transition | WARP:MAUSO1EA@0
#11   58     click-obj id 32006 (not placed) <-act174(MAUSO1EB #6)
        map transition | spawn WARP:MAUSO1EB@7 turnplayer spawn spawn
#12   57     click-obj id 65534 (not placed)
        switch / lever | disable#0
#14   57     enter-sector sector id 5 (sec 34)
        walk-in event | enable#0
#16   58     enter-sector sector id 6 (sec 214) [once]
        map transition | act174{ if!flag190 skip-if!flag189 GDV:SHRIVEA +flag189 choice choice choice
        "Take the artefact. / Ignore the voice. / Leave."
#17   58     click-obj id 32007 (not placed) <-act174(MAUSO1EB #6)
        map transition | WARP:MAUSO1EB@5 turnplayer
#21   58     click-obj obj 2 "Staff" sec 109 x2
        map transition | act174{ if!flag190 skip-if!flag189 GDV:SHRIVEA +flag189 choice choice choice
        "Take the artefact. / Ignore the voice. / Leave."
#24   57     click-obj id 1000 (not placed)
        take item (scripted) | give(clicked)
#26   57     click-obj id 1001 (not placed)
        take item (scripted) | give(clicked)
#30   57     exam-obj obj 4 art 0:107 sec 263
        examine (comment) | act161{ say }
        "White marble. There's a sense of calm around these things."
#31   57     exam-obj obj 5 art 0:107 sec 261
        examine (comment) | act161{ say }
        "White marble. There's a sense of calm around these things."
#32   57     exam-obj obj 6 art 0:107 sec 251
        examine (comment) | act161{ say }
        "White marble. There's a sense of calm around these things."
#35   57     enter-sector sector id 10 (sec 245) [once]
        walk-in event | spawn spawn spawn
#39   57     hit-obj id 10 (not placed)
        comment | act672{ random ->act675{ random say } ->act676{ random say } } objid
#42   57     click-obj obj 9 "Vial" sec 250 x2
        take item (scripted) | give143
```

**AELF** (38 records)

```
#1    61     exam-wall wall id 1 tex 2416 sec 439
        examine (comment) | act168{ skip-ifflag57 say if!flag57 say }
        "Hey! What do you know! Ingenious. / Won't open. Can't see a lock or..."
#2    62     enter-sector sector id 1 (sec 113) [once]
        cutscene | act176{ GDV:ALEF01 give69 +flag82 } act658{ } spawn turnplayer [off] disable#19 enable#72 spawn spawn spawn
#3    60     enter-sector sector id 2 (sec 467) [once]
        carry gate | ifi14 act173{ +flag40 ifflag62 skip-if!flag69 GDV:FLOTRAP1 choice skip-if!flag67 choice skip-if!flag68 choice
        "Tell him nothing. / Mention your father. / Demand answers."
#7    60     use-wall wall id 3 tex 2013 sec 0
        door (free) | door door
#9    60     hit-obj obj 1 "Monster" sec 467 x3
        attack / kill | objid count15(0->3/3) tex spawn enable#7 toggle#59
#16   64     click-obj obj 3 art 2:235 sec 214
        gated map transition | onfail:act590 ifi33@hand delay20 act455{ GDV:TUNNWARP } WARP:STUDY1@13
        "(refusal) Doesn't appear to be working."
#17   64     enter-sector sector id 3 (sec 458)
        map transition | WARP:MAUSO1EA@0
#19   61     use-wall wall id 1 tex 2416 sec 439
        comment | act316{ say }
        "Locked."
#25   62     enter-sector sector id 4 (sec 456) [off] [once]
        walk-in event | [off] disable#19
#29   62     hit-obj obj 4 "Monster" sec 50 x10
        attack / kill | objid count15(0->14/14) enable#19
#31   62     timer
        timer | spawn step#23 <when #23(0->14) done: enable#19 >
#33   62     hit-obj id 2001 (not placed)
        comment | spawn particles particles particles objid act672{ random ->act675{ random say } ->act676{ random say } }
#41   62     hit-obj obj 5 "Monster" sec 197 x2
        attack / kill | objid count15(0->14/14) enable#19
#42   62     hit-obj obj 6 "Monster" sec 288 x2
        attack / kill | objid count15(0->14/14) enable#19
#45   62     hit-obj id 2002 (not placed)
        comment | spawn particles particles particles objid act672{ random ->act675{ random say } ->act676{ random say } }
#46   62     hit-obj id 2003 (not placed)
        comment | spawn particles particles particles objid act672{ random ->act675{ random say } ->act676{ random say } }
#47   62     timer
        timer | spawn step#23 <when #23(0->14) done: enable#19 >
#49   62     timer
        timer | spawn step#23 <when #23(0->14) done: enable#19 >
#51   64     enter-sector sector id 5 (sec 502) [once]
        ambient | (entry #52)
#57   60     click-obj obj 2 art 0:177 sec 13 x3
        comment | act530{ random say }
        "This one's stuck fast. / Stuck fast. / Damn. stuck fast. / Won't bu..."
#59   60     use-wall wall id 5 tex 2013 sec 0
        door (free) | door door
#70   64     enter-sector sector id 7 (sec 57)
        ambient | delay200~rnd
#72   62     enter-sector sector id 2 (sec 467) [off] [once]
        walk-in event | spawn
#74   63     click-obj obj 1000 "Colt-45 magazine" sec 330 x2
        take item (scripted) | give(clicked)
#76   63     click-obj obj 1001 "Cartridges" sec 17 x3
        take item (scripted) | give(clicked)
#80   61     use-wall wall id 6 tex 2456 sec 439
        locked door + key | onfail:act316 ifi14@hand delay5 take14 <sub #90: light > spawn delay30 spawn <sub #90: (->#90) > give14 doo...
        "(refusal) Locked."
#82   64     exam-obj obj 11 art 0:116 sec 0 x46
        examine (comment) | act667{ say ifflag1 say }
        "Torch. Wonder who put that there? / There's power coming from the t..."
#94   61     use-wall wall id 7 tex 2416 sec 455
        door (free) | door door +flag57
#95   61     exam-wall wall id 6 tex 2456 sec 439
        examine (comment) | act168{ skip-ifflag57 say if!flag57 say }
        "Hey! What do you know! Ingenious. / Won't open. Can't see a lock or..."
#96   61     exam-wall wall id 7 tex 2416 sec 455
        examine (comment) | act168{ skip-ifflag57 say if!flag57 say }
        "Hey! What do you know! Ingenious. / Won't open. Can't see a lock or..."
#97   63     click-obj obj 9 "Vial" sec 17 x4
        take item (scripted) | give(clicked)
#102  63     click-obj obj 10 "Statuette" sec 227
        take item (scripted) | give4
#106  64     exam-obj obj 12 art 0:30 sec 60 x9
        examine (comment) | act306{ say ifflag1 say }
        "Knight or some soldier. Definitely Norman period though. / You aren..."
#107  64     exam-floor sector id 10 (sec 14)
        examine (comment) | act155{ say ifflag1 say }
        "Depiction of a knight. No name. / Doubt we could get the lid off. T..."
#108  64     exam-wall wall id 8 tex 65535 sec 14
        examine (comment) | act664{ say }
        "Sarcophagus. Cold to touch."
#109  64     exam-obj obj 13 art 0:103 sec 91 x2
        examine (comment) | act670{ say ifflag1 say }
        "Incense burner. / Persian judging by the design."
#110  64     exam-wall wall id 9 tex 287 sec 112
        examine (comment) | act671{ say }
        "Marble. Altar stone probably."
#111  60     exam-obj obj 2 art 0:177 sec 13 x3
        examine (comment) | act20{ say ifflag1 say }
        "This symbol's glowing. / I believe it's designed to keep us in, or ..."
```
