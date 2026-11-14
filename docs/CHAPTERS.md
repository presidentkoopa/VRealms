# CHAPTERS: the original's chapter structure, per-chapter puzzles, items and timing (2026-11-13)

The backbone for the chapter-by-chapter VR design discussion. Everything with a record number is read from the
retail install by `tools/chapter_census/` (engine-free, through the `src/roth` readers), so every table can be
regenerated:

**Per-chapter VR design and the changes made for each:** chapter 1 is `docs/VR_CHAPTER01_SHADOWS.md` (section 9
lists its changes), chapter 2 `docs/VR_CHAPTER02_SIGNS_AND_PORTENTS.md` (section 9); every change, with its commits, evidence and the chapters it reaches, is in
`docs/VR_CHANGES_LEDGER.md`. Each new chapter dossier gets the same "Changes made for this chapter" section.

```
sh tools/chapter_census/build.sh /tmp/cc
/tmp/cc/chapter_census "$ROTH_DATA" -all > tools/chapter_census/chapters_retail.txt   # the checked-in output
/tmp/cc/chapter_census "$ROTH_DATA" -map CHURCH1                                         # one map's chains
```

Modes: `-chapters -warps -digest -gates -items -chitems -chgates -kinds -timing -maps -map NAME`, plus the
over-approximate `-path N` / `-chpaths` (not in `-all`). The file header of `chapter_census.cpp` states every rule
the tool applies, with its ROTH.C site.

**Evidence marks.** [V: how] = read from the install by the census (or by a named probe in this session), or read in
ROTH.C at the cited line. Nothing in this document was run in the original or in our engine: "the chain does X" means
"the records say X, under the ROTH.C semantics cited", not "seen happening". [R] = reasoning only, not seen.
[R: walkthrough] = only an online walkthrough says it (Nick Brown's on JustAdventure, pages 1 and 2; Lu Richardson's
on GameBoomers; checked 2026-11-13).

**Words used.** A *unit* is one entry record (a trigger the original registers: click object, use/examine wall or
floor, enter sector, attack hits, walk into, timer) plus the chain it walks. `MAP #n` is the 1-based record number.
`iN` is DBASE100 item N, `actN` DBASE100 action N, `flag N` the DBASE100 flag bitmap. "In hand" = RAW 0x27 with
f6&2, the item must be the one selected (`cmd_if_not_item`, raw_commands.c:4760); "carried" = anywhere in the
inventory.

---

## 0. Findings in one place

1. **The original names its own 20 chapters.** CHPTR1..CHPTR20.GDV are title cards in the DBASE100 cutscene table
   (entries 147-168) and each carries its gallery title in DBASE400 ("Chapter 1 - Shadows" ... "Chapter 20 - Devil
   take..."). Each card is played from one place in the level scripts, except card 3 (two walls of one room), card 16
   (GRAVE #19 and MAZE #16) and card 15 (GRAVE #23, plus two dubious records, finding 9). [V: `-chapters`]
2. **The CD split is in the install.** `DATA/FILELIST.TXT` has one `DISK1={...}` block whose GDV names fall in four
   alphabetical runs: run 1 holds chptr1-4, run 2 chptr5-11, run 3 chptr12-16, run 4 chptr17-20. The engine reads
   that file with a `DISK1 DISK2 DISK3 DISK4` template (`load_disk_path_config`, file_config.c:678-712). [V: file
   read; ROTH.C read] That the four runs are the four retail CDs is [R] (this install merged them into DISK1).
3. **New game starts in STUDY1** (the first `maps` entry of ROTH.RES; action 3 plays CHPTR1 and gives i68 Adam,
   i1 Shards and Seals, i15 Lamp, i16 Matchbox) and **the game ends at STUDY1 #660** (a use-wall record enabled by
   killing the house monster, STUDY1 #571, that runs act614: EPILOGUE, ECREDITS, a picture, health to -32766).
   [V: `-chapters`; roth_gameflow.h header for the boot/end rules]
4. **The level scripts are a hub.** The four house maps STUDY1-4 are entered and left again in at least eight
   chapters; the chapter cards are played in STUDY1 (cards 2, 3), STUDY2 (5, 9), GNARL1, CAVERNS, CAVERNS3, ANUBIS
   (8, 12), TEMPLE1, RAQUIA5, CHURCH1, AQUA1, GRAVE (15, 16), MAZE, DOPPLE, SOULST3 and SOULST2 (19, 20). [V]
   STUDY2/3/4 are **other wings, not the same rooms at another time**: they share 19, 38 and 25 vertex positions
   with STUDY1's 1,076 (STUDY1's own sarcophagus-room sectors 221/361/362 appear in no other map). [V: probe
   /tmp/chx/explore7, explore8, this session]
5. **Gates are mostly items in hand, and many are sequences.** 716 of the 1,912 units have a gate or a progress
   effect; 95 of those hold a counter or a step (section 4). Five puzzles are **bit masks**: each correct placement
   XORs one bit into a Count and the reward runs when the mask is full (AQUA1 four plaques, mask 15; RAQUIA5 seven
   jewels, mask 127; SALVAT eight dials, mask 255; ANUBIS clock, two bits). Taking an item back XORs it out again.
   [V: census; the key-0 0x1e rule read in raw_commands.c:4001-4007 and :156-201]
6. **The Mask (i60) is a travel item.** Using it from the inventory runs the click chain of a script id (32002)
   in whatever map Adam is in: act539 → RAW 0x36. Seven maps answer it (STUDY2, STUDY3, TEMPLE1, CHURCH1, AQUA2,
   RAQUIA4, MAZE), each with its own gate; it is how a Mantel Stone is used. [V: `-gates`, `-digest`]
7. **Examine records run a DBASE100 action** (their word +0x0a, `examine_world_object`, input.c:949-965) before
   their chain. Most only speak, but some set flags or give information records (e.g. CHURCH1 #422 gives i174).
   The census follows them. [V: ROTH.C read; census]
8. **Nine developer objects.** Nine click entries sit on objects that are hidden at load and that no 0x16/0x3c in
   their map ever shows: STUDY1 #606 gives every map, the Dragon Sword, the Wand, crystals and more; TGATE1F #73
   gives the four ch16 prerequisites; STUDY1 #576 and STUDY2 #265 set the ending flag 373; CHURCH1 #423, MAZE #4,
   CAVERNS3 #47, DOMINION #59, DOPPLE #1. They are excluded from "where found". [V: object flags 0x80 and the
   absence of 0x16/0x3c keyed to them] That a hidden object cannot be clicked is [R].
9. **Loose ends in the data** [V]: MAUSO1EA #59 warps to MAS5, which is not in the install; act691 plays CHPTR9 and
   nothing runs it; RAQUIA2 #187 and RAQUIA3 #58 play CHPTR15 and give i97 from ids no object carries and no action
   calls; CHURCH1 #34 tests key i256 and nothing in the data gives i256; i125 Coloured Key (RAQUIA5 #137) and i105/
   i142 Flask have no producer the census can see (they may come from item blocks it does not read, [R]).

---

## 1. The chapter structure as the original defines it

### 1.1 The cards [V: `-chapters`]

| ch | gallery title (DBASE400) | card action | card entry: record, what fires it, its gate | maps first played [R: walkthrough + map graph] |
|---|---|---|---|---|
| 1 | Shadows | act3 | new game (menu code 10, game_core.c:821-845) | STUDY1 |
| 2 | Signs and Portents | act178 (+RANDALL) | STUDY1 #299 enter sector id 29, once; warps in place @34 | MAS3, MAUSO1EA, MAS4, MAS6, MAS7, MAUSO1EB, AELF |
| 3 | Keeper of Time | act177 (+REBECCA) | STUDY1 #75 / #354 use wall, needs flag 82 (set by AELF #2, meeting Aelf) | GNARL1 (via STUDY1 #267, i35 Crystal in hand) |
| 4 | Tales of the Tower | act516 (+GNARLI) | GNARL1 #3 click, needs i60 Mask carried; takes i35; warp STUDY1@16 | STUDY3, TOWER1, STUDY2 |
| 5 | The Offering | act627 | STUDY2 #118 = use i60 Mask (two masks taken, i200 given); warp TOWER1@9 | CAVERNS2, CAVERNS |
| 6 | Journal of Evil | act579 (+EFFIGYC) | CAVERNS #48 click the guardian (off until CAVERNS #26), one of i37/i249 in hand, last given = i37 | OPTEMP1, CAVERNS3 |
| 7 | Beyond the Gate | act623 | CAVERNS3 #44 enter sector (off until CAVERNS3 #35: i33 Staff in hand); warp STUDY1@13 | ANUBIS |
| 8 | Mark of the Beast | act532 (Belial) | ANUBIS #70 enter sector, once, needs flag 93 (ANUBIS #56, Aelf's Dagger); the card is in one dialogue branch | STUDY4 |
| 9 | Seven be Bound | act442 (act691 unused) | STUDY2 #74 enter sector, once | TEMPLE1 |
| 10 | The Key of Tears | act181 (+GAUL02) | TEMPLE1 #18 enter sector, once | TGATE1F, TGATE1H, TGATE1I, RAQUIA4, RAQUIA1, RAQUIA2, RAQUIA3, RAQUIA5 |
| 11 | Captive Angel | act593 (+RATPFA) | RAQUIA5 #377 = use i116 Ring or i203 Spirit Ring, needs flag 330; gives i144 Key of Tears | (return trip only) |
| 12 | St. Michael's | act583 (Hawk) | ANUBIS #89 enter sector, once, needs i144 carried; takes it | CHURCH1, VICAR, VICAR1 |
| 13 | Dragon Sword | act495 (+ALEF04) | CHURCH1 #309 click The Dragon Helm; gives i104, flag 242 | AQUA2, AQUA1, LRINTH, LRINTH1 |
| 14 | Vengeful Souls | act562 (+RITUAL) | AQUA1 #22 click, i156 Chalice in hand, then the else branch; gives i97 Eternity | (TGATE1F again) |
| 15 | Where the dead Lie | act589 (+BELSB5/6) | GRAVE #23 enter sector, after 420 + 1040 tics of movie delay | GRAVE |
| 16 | The Gate | act625 | GRAVE #19 enter sector (warp MAZE@0) or MAZE #16 Mask use (card only if i97 carried); once by flag 388 | MAZE, TGATE1G, ABAGATE2, DOMINION, DOPPLE |
| 17 | Father of Lies | act608 (+HAWK04) | DOPPLE #16 enter sector; warp ABAGATE2@7 | SALVAT, SOULST3, ELOHIM1 |
| 18 | Threads | act626 | SOULST3 #17 click, i97 in hand; warp SOULST2@6 | SOULST2 |
| 19 | The Gathering | act613 | SOULST2 #17 enter sector: TUNNWARP, CHPTR19, TALEWELA, then a three-way dialogue | - |
| 20 | Devil take... | act613 | the same action, branch FULLKEEP: CHPTR20, flag 373, rawobj 32027 → warp STUDY2@13 | (STUDY2 courtyard, STUDY1) |
| end | The Whole Story (EPILOGUE) | act614 | STUDY1 #660 use wall, off until STUDY1 #571 (attack hits the Monster) | |

Notes:
- Cards 19 and 20 are one action with one choice menu; CHPTR20 plays in the branch that also sets flag 373, which
  STUDY2 #262 and STUDY1 #564/#584/#630 test to collapse the house. [V]
- Card 5 plays as the Mask is used at STUDY2's Mantel Stone. The walkthrough puts the first Tower visit and Raphael
  (STUDY3 #98, the Mask used at STUDY3's stone: RAPHAEL movies and the information records Raphael, Raquia, Arqua,
  The Tower) inside chapter 4, which agrees: chapter 4 is STUDY3's stone → TOWER1 → STUDY2's courtyard, and its card
  5 fires on the way back into the Tower from STUDY2. [V: records; R: the order]
- Card 15 has two more players (RAQUIA2 #187, RAQUIA3 #58) on ids nothing carries (finding 9).
- The walkthrough calls chapter 2 "Sign and Portents" and chapter 8 "The Mark of the Beast"; the data says "Signs and
  Portents" and "Mark of the Beast". [V]

### 1.2 Transitions (RAW 0x3b) [V: `-warps`]

127 transition records in 44 maps (190 unit-to-warp pairs, because some chains are reached from more than one entry). Arrival is the first sector whose id equals the key (0 = keep position,
`FindArrivalSector`, roth_gameflow.cpp). The unique edges:

```
STUDY1   -> AELF ANUBIS CAVERNS3 GNARL1 MAS3 STUDY1 STUDY2 STUDY3 STUDY4
STUDY2   -> GRAVE STUDY1 STUDY2 STUDY4 TEMPLE1 TOWER1        STUDY3 -> CAVERNS2 STUDY1 TOWER1
STUDY4   -> STUDY1 STUDY2                                     GNARL1 -> STUDY1
MAUSO1EA -> AELF MAS3 MAS4 MAS5(missing) MAS6 MAS7 MAUSO1EB   MAS3 -> MAUSO1EA STUDY1
MAS4     -> MAS6 MAUSO1EA   MAS6 -> MAS4 MAS7 MAUSO1EA   MAS7 -> MAUSO1EA   MAUSO1EB -> MAUSO1EA (+ in place)
AELF     -> MAUSO1EA STUDY1                                   TOWER1 -> STUDY2 STUDY3
CAVERNS  -> CAVERNS2 OPTEMP1 (+ in place)   CAVERNS2 -> CAVERNS STUDY3   OPTEMP1 -> CAVERNS CAVERNS3
CAVERNS3 -> OPTEMP1 STUDY1                                    ANUBIS -> ANUBIS CHURCH1 STUDY1
TEMPLE1  -> STUDY2 TEMPLE1 TGATE1F
TGATE1F  -> ABAGATE2 GRAVE TEMPLE1 TGATE1G TGATE1H   TGATE1G -> MAZE TGATE1F
TGATE1H  -> AQUA2 CHURCH1 TGATE1F TGATE1I            TGATE1I -> RAQUIA4 TGATE1H
RAQUIA4  -> RAQUIA1 TGATE1I   RAQUIA1 -> RAQUIA2 RAQUIA4   RAQUIA2 -> RAQUIA1 RAQUIA3   RAQUIA3 -> RAQUIA2 RAQUIA5
RAQUIA5  -> RAQUIA3 (+ in place)
CHURCH1  -> TGATE1H VICAR VICAR1   VICAR -> CHURCH1   VICAR1 -> CHURCH1
AQUA2    -> AQUA1 TGATE1H   AQUA1 -> AQUA2 LRINTH LRINTH1   LRINTH -> AQUA1   LRINTH1 -> AQUA1
GRAVE    -> MAZE STUDY2   MAZE -> GRAVE TGATE1G
ABAGATE2 -> DOMINION SALVAT SOULST3   DOMINION -> DOPPLE   DOPPLE -> ABAGATE2   SALVAT -> ABAGATE2
SOULST3  -> ELOHIM1 SOULST2 (+ in place)   ELOHIM1 -> CHURCH1 SOULST3   SOULST2 -> STUDY2
```

The gate **as it stands at each 0x3b record** (what the chain tested before reaching it; "!" = only while the flag is
clear or the item absent). A test that comes after the 0x3b in the same chain does not gate the move: ANUBIS #64's
flag 92, CAVERNS #17/#19's flag 1 and TGATE1F #17's flag 331 all sit after their warp. [V: `-warps`]

| record | to | gate at the record | produced by |
|---|---|---|---|
| STUDY1 #267 | GNARL1 | i35 Crystal in hand, and !i60 (only before Gnarl hands over the masks) | STUDY1 #378 |
| STUDY1 #424 / #445 | CAVERNS3 / AELF | i33 Staff in hand (the green wall sigils) | CAVERNS3 #9 |
| AELF #16 | STUDY1 | i33 Staff in hand | CAVERNS3 #9 |
| GNARL1 #11 | STUDY1 | i35 carried | GNARL1 #33, STUDY1 #378 |
| STUDY1 #75 / #354 | STUDY1 @41 (card 3) | flag 82, !flag 55 | AELF #2 (Aelf) |
| ANUBIS #89 | ANUBIS @13 (card 12) | i144 Key of Tears carried | RAQUIA5 #313, #377 |
| AQUA1 #79 | AQUA2 | flag 311 (the ritual) | AQUA1 #22 |
| SOULST3 #17 | SOULST2 (card 18) | i97 Eternity in hand | AQUA1 #22 |
| TGATE1F #44 | GRAVE (Belial's prison) | i97 carried; then takes i14, i97 and the travel mask | AQUA1 #22 |
| TGATE1F #48 | ABAGATE2 | flag 398 = TGATE1F #13 with i14, i97, i104, i137 all carried | TGATE1F #13 |
| TGATE1H #5 | AQUA2 | flag 242 (the Dragon Helm), !flag 311 | CHURCH1 #309 |
| TGATE1H #21 | CHURCH1 | flag 96 (Hawk, card 12) | ANUBIS #89 |
| TGATE1I #12 | RAQUIA4 | !flag 331 (open until Raquia is done) | - |

The Mask travel records (finding 6): STUDY2 #118 and STUDY3 #98 (the Mask only), TEMPLE1 #24 (flag 431, from
TEMPLE1 #51, the Green Crystals), CHURCH1 #396 (flag 242), AQUA2 #17 (flag 311), RAQUIA4 #7 (flag 331), MAZE #16
(the Mask only; i97 is tested after the warp and gates card 16). Entries off at load until a 0x17 enables them:
ANUBIS #93, AQUA1 #98, AQUA2 #15, CAVERNS3 #44, DOMINION #54, MAS6 #54, SALVAT #199, TGATE1F #55.

---

## 2. Per chapter: maps, the gates on the critical path, optional content

How to read: **Path** is the order the walkthrough plays it [R: walkthrough], with each step's records [V: census].
**Gates** is the census's own list for the chapter's maps (`-chgates`; kinds as in section 4); the full markdown
table for every chapter is in `chapters_retail.txt`, section CHAPTER GATES, and every unit with its whole chain is
in the MAP sections. A gate the path does not name may be optional or may be the walkthrough skipping a step; the
census cannot tell, because it does not model space (which rooms a door closes off).

### Chapter 1, Shadows (STUDY1, the front of the house)
**CORRECTED 2026-11-13 by the chapter 1 area walk** (`openables -area STUDY1 -stop 29 -have 1,15,16,68`, details in
`docs/VR_CHAPTER01_SHADOWS.md` section 1). The first version of this section placed several Study things in chapter 1.
- **Path [V: area walk]:** take the Colt (#416) and magazines (#410); the Book i17 (#205; first look gives the silver
  key i20); light the tall candles #4 (obj id 3, two objects: a count to 2, then the corridor painting rolls up and the
  niche fires a shot) and the landing candelabra **#212** (obj id 16, s210/s211) with the Matchbox **in hand**; take
  the brass Key i21 (#89, the niche); the Study door STUDY1 #11 (i21 in hand, refusal act316 latched first, flag 374,
  door). Entering the Study: card 2 (#299).
- **Optional:** the Note i36 (#29, the lounge typewriter), the Scrapbook i27 (#51).
- **Moved to chapter 2 (they are in the Study):** the letters #34/#38/#53/#57, the candelabra #18 (obj id 2, s217/s252),
  the serpent statuettes i4, the three masks, the Shield, the Sword, the sarcophagus count.
- **Not chapter 1:** the Ace of Spades (hidden at load, shown only by #427 in #357, far from this area); the House Map
  i43 (#195, s22, not reached).

### Chapter 2, Signs and Portents (STUDY1 study and cellars, MAS3, MAUSO1EA, MAS4, MAS6, MAS7, MAUSO1EB, AELF)
- **Path [R: walkthrough]:** Study: four gold serpents i4 (STUDY1 #23, #58, #74, #540), Shield i28 (#320), Sword
  i29 (#332), three masks i24-26 (#301, #351, #171); the bookcase door; the molten river and pentagram room (MAS3,
  entered by STUDY1 #404); the Orb i18 (MAS3 #1/#24) **placed** in the Orb door MAS3 #4 (taken, door sequence over
  760 tics); store rooms (MAS4): Key i22 (MAS4 #7) → MAUSO1EA #76 (in hand: a height change, flag 428); the
  gauntlet MAS6: hand panels #5/#9/#10 start off and are switched on by #20; four kills counted by MAS6 #48; a timed
  lever MAS6 #29 (timer, random 120-tic delay); the throne room MAS7 (BRANDED); the winged-statue temple MAUSO1EB:
  Staff i12 and the Shrive i14 (SHRIVEA-D, #4/#16/#21); Aelf at the altar AELF #2 (sets flag 82), AELF #3 (i14
  carried + flag 62 from MAS7: the Florentine dialogue, FLOTRAP1-6), AELF #80 (the double door: i14 in hand, taken and
  given back, flag 57). Back in the Study through MAS3 #113 (i14 in hand at the recess). Card 3 at the sarcophagus
  wall (STUDY1 #75 / #354, flag 82): Rebecca joins (flag 1).
- **Gates [V]:** MAS3 #4 (place i18), MAS3 #113 (i14), MAUSO1EA #76 (i22), AELF #3 (carry i14), AELF #80 (i14),
  MAS6 #48 (kill counter), AELF #9/#29/#41/#42 (kill counters 3 and 14), 4 timers in AELF/MAS6.
- **Optional:** Parchment i13 (MAS3 #10), Map of the Mausoleum i273 (MAS3 #111), vials.
- **CORRECTED 2026-11-13 by the chapter 2 area walks** (`openables -area` from every arrival: `-at` for keyed warps,
  `-xy` for key-0 warps, which keep the player's position; details and commands in
  `docs/VR_CHAPTER02_SIGNS_AND_PORTENTS.md` sections 0-1):
  - **The first creatures are in STUDY1**: `#362` (entering s454 behind the bookcase: obj 52, template i91) and `#265`
    (the landing s25, switched on by card 2's chain `#497`: obj 58, template i91).
  - **MAS3 also holds a Shotgun i38** (`#107`), cartridges (`#47`; `#60` also spawns a creature) and a spare Colt
    (`#83` moves the niche s236/s237 when Adam already carries the Colt, then a cave-in, `#86`). The Orb door is a
    **sector** (s30's ceiling −1200 → −1000, `#68`); the Parchment and the Map lie beyond it.
  - **MAS6: the hand panels are `#42`, `#8`, `#41`** (three DEMO4[4288] plates, a count to 3, which shows the lever
    `#20`). `#5`/`#9`/`#10` are the inscription ("Chullum ashdar in derias?"), switched on by `#20` (flag 116); they
    switch on the portal `#54` (MAUSO1EA @29, a forced turn). The words are spoken at MAUSO1EA `#5` (act235, a choice,
    only with flag 116).
  - **Key i22 opens the walkway**: MAUSO1EA `#76` (i22 in hand) raises s193's ceiling; beyond it the walkway's three
    Creator movies `#40`/`#44`/`#46` (WALKWAY1/1A/2/3); the third hurts (`#95`) unless flag 62 (MAS7's throne) is set.
  - **MAS7 is entered by MAUSO1EA `#57` (@0) or by the pit** `#20`/`#27`/`#136` (the floor falls 820 units, damage,
    warp @13). MAS6 `#27`'s @11 is never used: no MAS6 sector carries id 11.
  - **MAUSO1EB**: act174 at the altar (`#4`, `#16`, `#21`) offers "Take the artefact." / "Ignore the voice." /
    "Leave."; Take gives **i14 and i12** (SHRIVEC) and two winged statues come alive (`#11`, template i30).
  - **AELF `#3` tests the Shrive carried** (flags 01); flag 62 gates only Florentine's movies and choices. The ward
    trap (wards, three creatures, `#9` count 3) runs either way.
  - **MAS3 `#113` is not the way back**: the walk returns by the stair `#13`; `#113` moves sectors id 20 (where they
    lead is not established).
  - **The sarcophagus cannot open in chapter 2**: the count starts at 1 (obj 24 on `#128`'s spot) and only six
    statuettes exist by card 3 (STUDY1 `#23`, `#58`, `#74`, `#540`, the one on its plate, AELF `#102`); the seventh is
    CAVERNS3 `#7` or STUDY3 `#11`.
  - **Dead records** (keyed to ids no sector carries): MAS3 `#17`, MAS4 `#16`, MAS6 `#25`, `#27`, `#59`, MAUSO1EA `#24`,
    `#25`, `#59`.
  - **End condition:** STUDY1 `#75`/`#354` from the passage with flag 82 set and flag 55 clear (else the bookcase
    opens, `#421`).

### Chapter 3, Keeper of Time (STUDY1 upper rooms, GNARL1)
- **Path:** Helm i45 (#355, GAUL01 on inspection [R]); the fire room gives the green Crystal i35 (#378: shown, taken,
  given); the green hemisphere STUDY1 #267 (i35 in hand → warp GNARL1, GNARLA-G, a dialogue whose choices set flags
  29/30/64/75/76/278); GNARL1: two masks i60 (#16), Crystal back (#33), GNARL1 #11 (i35 carried, the test dialogue);
  GNARL1 #3 (i60 carried) plays card 4 and returns to STUDY1@16.
- **Gates [V]:** STUDY1 #267, GNARL1 #3/#11; GNARL1 #18 (script id 32001 called from Gnarl's dialogue act189).

### Chapter 4, Tales of the Tower (STUDY3, TOWER1, STUDY2)
- **Path:** use the Mask at STUDY3's stone: STUDY3 #98 (Raphael, RAPHAEL/H-O, information records i74 i82 i86 i87
  i90 i174, gives i200); TOWER1 (way stones; TOWER1 #14 → STUDY3, #11 → STUDY2@9); STUDY2 courtyard: Key i236
  (#126, once) → STUDY2 #10 (in hand, flag 14) → Rune guide i95 (#47, which also starts a sequence of spawns and two
  timed lines over 720 tics). Using the Mask at STUDY2's stone: STUDY2 #118, card 5.
- **Gates [V]:** STUDY3 #98, STUDY2 #10, STUDY2 #118; STUDY3 #4 and #42 belong to chapter 5 (below).

### Chapter 5, The Offering (STUDY3, CAVERNS2, CAVERNS)
- **Path:** back through TOWER1 #14 to STUDY3; the floating-key chapel: STUDY3 #4 (serpent i4 **placed** on the floor
  plate: taken, a height change) and STUDY3 #11 (the serpent given back), STUDY3 #21 (Key i173, needs flag 326, set
  on entering the room: #116/#136/#162/#164) → STUDY3 #42 (i173 in hand); the axe demons [R: walkthrough, by shooting a
  wall device; not identified in the records]; CAVERNS2 (HAWK01, i89 The Voice in the Tunnels) → CAVERNS: Jewel/blue
  crystal i37 (#6), red crystals i249 (#76, three objects), the guardian: CAVERNS #26 (dialogue EFFIGYA2/A3/B,
  enables #48), CAVERNS #48 (list i249/i37 in hand, last given = i37: takes i37 and i249 ×4, card 6).
- **Gates [V]:** STUDY3 #4, #21, #42; CAVERNS #26, #48, #65; kill counters CAVERNS #37/#45 (toggle flags 285/286).

### Chapter 6, Journal of Evil (OPTEMP1, CAVERNS3)
- **Path:** OPTEMP1 (walk-in events); CAVERNS3: Breastplate i66 (#37), Magnifying Glass i65 (#11), Sketches i168/i248,
  Staff i33 (#9, under the bed), Quill & Ink i67 (#40), Journal i84/i172 (#25) whose inspection gives key i167
  [V: `-gates`, "inspect i172"], wardrobe CAVERNS3 #57 (i167 in hand, flag 427) → the last serpent i4 (#7), Starchart
  i235, Map of Raquia Maze i275 (#131); the green wall outline CAVERNS3 #35 (i33 in hand: light, height, flag 94,
  enables #44) → CAVERNS3 #44 (card 7, warp STUDY1@13).
- **Gates [V]:** CAVERNS3 #35, #44, #57.

### Chapter 7, Beyond the Gate (STUDY1 sarcophagus, ANUBIS)
- **Path:** the sarcophagus plates STUDY1 #119, #124-#129 (seven use-floor records on sector id 13): each takes one i4
  **in hand**, shows it, and runs Count #156 from 1 toward 7; at 7 the sarcophagus opens (a height change, act23).
  One plate (#128) starts disabled with a serpent already on it (object 24, #138), which is why the count starts at
  1. Clicking a placed serpent (#121, #136-#141) gives it back, steps the count down and re-enables its plate. Then
  STUDY1 #188 → ANUBIS. The clock: ANUBIS #2/#3 (use-floor, each XORs one bit into a count; together they enable the
  button #34, which is off at load), the Dragon Shield in hand [R: walkthrough; the records name no shield test];
  Chalice i127 (#103), fountains ANUBIS #14 (carried: take and give back = fill), the fish doors ANUBIS #68/#69 (an
  **attack-hits-wall** record with i127 in hand: the chalice is thrown); Hawk ANUBIS #60/#151 (dialogue, i77, i213);
  Aelf's Dagger ANUBIS #56 (dialogue, flag 93) → ANUBIS #70 (Belial, card 8 in one branch, flag 92).
- **Gates [V]:** STUDY1 plate records; ANUBIS #2, #3, #34, #107, #14, #68, #69, #56, #70, #64.

### Chapter 8, Mark of the Beast (STUDY1 front hall, STUDY4)
- **Path:** ANUBIS #64 (flag 92) back to STUDY1; STUDY1 #472/#486/#595 (flag 92/318 spawn the front-hall events);
  STUDY4: the pentagram candles STUDY4 #29 (five candle objects, i16 in hand, a count to 5, then BEAST, a turn of the
  player, spawns), #59, #67; the quartz statuette i49 (STUDY4 #30) used on doors: STUDY2 #90 (taken, flag 61) and
  STUDY2 #232; it is given back by STUDY2 #100 and #242 (the walkthrough's "keep the statuette").
- **Gates [V]:** STUDY4 #29, #59, #67; STUDY2 #90, #232.

### Chapter 9, Seven be Bound (STUDY2, TEMPLE1)
- **Path:** card 9 on entering STUDY2's library (#74); parchments i39/i40/i42 (#23, #46, #85), Complete Map of the
  Tower i279 (#259); the statuette door #232; the cellar maze and STUDY2 #110 → TEMPLE1; TEMPLE1 #18 (GAUL02, card 10).
- **Gates [V]:** STUDY2 #74, #232, TEMPLE1 #18.

### Chapter 10, The Key of Tears (TEMPLE1, TGATE1F/H/I, RAQUIA4, RAQUIA1-3, RAQUIA5)
- **Path:** Green Crystals i137 (TEMPLE1 #51); the Mask at TEMPLE1 (#24, flag 431) → TGATE1F; TGATE1H (#31, the
  instructions dialogue INSTRA-I), TGATE1I #12 (open while flag 331 is clear) → RAQUIA4; RAQUIA1 #6 (the spirit, RAYSEL1A-G); RAQUIA2:
  Pipes i99 (#174), Earplugs i108 (#169), Haunch i109 (#170); RAQUIA3: Ring i116 (#8), the beast fed RAQUIA3 #31
  (i109 placed), the gate RAQUIA3 #6/#17 (i116 or the worn ring in hand); RAQUIA5, the bell tower and seven doors:
  Pipes used from the inventory RAQUIA5 #262 (act549, flag 385), Earplugs worn i113 at the siren (#84, #140 → Key
  i20) → RAQUIA5 #292 (i20), the spectacles worn i202 (#430), the flask doors #232 (i105) and #247 (i142), the
  Coloured Key door #137 (i125), the coloured-gem pedestal #238/#283 (i98 placed), the seven jewels: RAQUIA5 #152,
  #163, #172, #181, #190, #199, #208 (one of i103 i106 i114 i115 i122 i123 i154 in hand; each XORs bit 1..64 into a
  count that must reach 127; the matching #161...#215 take a jewel back and XOR it out), the ring and gem combined
  (i116/i215 + i217 → i203, recipe) and used RAQUIA5 #311; the Key of Tears i144 from RAQUIA5 #313/#377 (#377 is
  card 11).
- **Gates [V]:** 145 units; 4 locked doors, 10 placements, 8 use-on-object, 3 inventory uses, 8 carry gates, 19
  counter units.

### Chapter 11, Captive Angel (return: RAQUIA5 → ... → STUDY1 → ANUBIS)
- **Path:** RAQUIA5 #321 (i216 worn, flags 10/331), RAQUIA4 #7 (the Mask, flag 331) → TGATE1I, TGATE1F #17 to TEMPLE1,
  back to the Study; ANUBIS #89
  (i144 carried, taken; Hawk, card 12). No new map. [V: records; R: route]

### Chapter 12, St. Michael's (ANUBIS #170 → CHURCH1, VICAR, VICAR1)
- **Path:** the Wand i150 and Magical Torch i175 (VICAR1 #11, CHURCH1 #11/#13); wards: **attack-hits-wall** records
  with the Wand in hand (and the Torch in hand for the hidden ones): CHURCH1 #58, #89, #90, #97, #106, #112, #123,
  #156-#158, VICAR #20, #27, #33, #39, #51, #61, #70, VICAR1 #19 (each sets flag 158 and enables a door or a record);
  keys: i129 (CHURCH1 #42 → #40), i193 (VICAR #100 → CHURCH1 #70), i218 (CHURCH1 #28 → VICAR #49), i219 (VICAR #2 →
  #45), i171 (VICAR1 #27 → VICAR1 #63/#67), i167 (the journal's key) → VICAR #1; the Coin i185 into the collection box
  CHURCH1 #77 (placed); the Tiara i102 on the statuette CHURCH1 #67 and a feather i101/i128 on the base CHURCH1 #256
  (list, last given = i128); the Dragon Helm CHURCH1 #309 (card 13).
- **Gates [V]:** 88 units: 10 locked doors, 17 use-on-object, 19 doors enabled later.

### Chapter 13, Dragon Sword (CHURCH1 #396 Mask → TGATE1H #5 → AQUA2 → AQUA1, LRINTH, LRINTH1)
- **Path:** Chalice i156 (LRINTH #3 / AQUA1 #4), Magnifying Glass i6 (LRINTH1 #4), the force-field doors: AQUA1 #13,
  #21, #28, #141-#146 (**attack-hits-wall** with the Chalice in hand = thrown); the panels read with the Magnifying
  Glass in hand (AQUA1 #78, #94, #155 → enable #160, #165); the recipe chain in the inventory (Cylinder+Flint,
  Mortar+Pestle, Bowl+Snow, Tinder, Amber, the Water-pipe: 24 recipes in all, section 3); the four fountain plaques
  AQUA1 #7, #11, #48, #56 (one of eight items in hand; each XORs bit 1/2/4/8, mask 15) and their take-back records #1,
  #9, #65, #128 [R: that these are the take-backs; they give the clicked object and XOR the same bit]; AQUA1 #22 (Chalice in hand, then act562: RITUAL, card 14, gives Eternity i97, takes the recipe items).
- **Gates [V]:** 52 units; 16 use-on-object, 4 placements, 3 inventory uses.

### Chapter 14, Vengeful Souls (AQUA1 #79 → AQUA2 → TGATE1H → TGATE1F → GRAVE cell)
- **Path:** TGATE1F #44 (i97 carried): takes the Shrive, the Sword and the travel mask, BELSB1/3/4, BELEXTRA, warp
  GRAVE@5 (Belial's prison). No new puzzle records. [V: records; R: route]

### Chapter 15, Where the dead Lie (GRAVE)
- **Path:** GRAVE #23 (card 15 after 1,460 tics of movie delays); the cell's journal part 3 i220 and news cuttings
  (#40, #44, #52-#56); the three flames GRAVE #96, #98, #100 (one of i84/i172/i220 in hand, each placed, a count to 3,
  then the door #90); GRAVE #19 → MAZE (card 16).
- **Gates [V]:** GRAVE #96/#98/#100, #23, #19, doors #1/#90 enabled later.

### Chapter 16, The Gate (MAZE, TGATE1G, TGATE1F, ABAGATE2, DOMINION, DOPPLE)
- **Path:** MAZE #16 (Mask use, i97 carried, card 16 if not yet played); TGATE1F #13 (carry i14, i97, i104, i137 →
  flag 398) → #48 → ABAGATE2; DOMINION: the brain machine DOMINION #47, #70, #71, #72 (i223 in hand, placed; two
  counts to 5 and 4; #72 opens the way and enables #54), the walls #11/#15/#20/#25 (flag 212); DOPPLE: the 16 mirrors
  DOPPLE #23 ... #68 (**attack-hits-wall**, each once, a count to 16 that enables #7), the Abaddon dialogue #18;
  DOPPLE #16 (card 17).
- **Gates [V]:** 48 units, 17 of them attack/kill.

### Chapter 17, Father of Lies (ABAGATE2, SALVAT, SOULST3, ELOHIM1)
- **Path:** SALVAT's octagon: 23 walk-in events, attack-hits-wall switches (#233, #275, #286, #297), the eight dials
  SALVAT #129, #131, #132, #136, #140, #141, #142, #143 (each steps its own count 7→3 and on completion XORs one bit of
  255), the Perdition Shield i53 (#584), Abaddon (#399); ABAGATE2 #26 → SOULST3: Belial (#19, REFLECTA-C; takes i14 and
  i97 in one branch), SOULST3 #17 (i97 in hand, card 18).
- **Gates [V]:** 58 units; 14 switches, 8 attack.

### Chapters 18-20, Threads, The Gathering, Devil take... (SOULST2, STUDY2, STUDY1)
- **Path:** SOULST2 (the cave route, vials); SOULST2 #17 (act613: CHPTR19, TALEWELA, the choice; CHPTR20 in the
  FULLKEEP branch, flag 373, rawobj32027 → warp STUDY2@13); the house collapses (STUDY2 #262, STUDY1 #564/#584/#630
  test flag 373); the Monster STUDY1 #571 (attack hits object: enables #660) → STUDY1 #660 (act614, the end).
- **Gates [V]:** SOULST2 #17; STUDY1 #571, #660.

---

## 3. Items per chapter [V: `-chitems`, `-items`]

Items are grouped under the chapter whose maps first hold them (the -kinds table's chapter→map list, which is [R]).
Information records (category 2: characters, topics) are counted apart. D = dossier in `ITEM_DESIGN.md` (1-40).

| ch | items | info | progress items found there (where → where used) |
|---|---|---|---|
| 1 | 24 | 6 | D i1, D i15 Lamp, D i16 Matchbox (start; STUDY1 #4/#18/#212, STUDY4 #29/#59/#67); i21 Key (#89 → #11); D i4 serpents (#23 #58 #74 #540 → plates #119...); i24-26 masks; D i28 Shield; i29 Sword; D i35 Crystal (#378 → #267, GNARL1 #11); i45 Helm; i173 Key (pickup → STUDY3 #42) |
| 2 | 8 | 1 | D i14 Shrive (MAUSO1EB → AELF #80, MAS3 #113, TGATE1F #13); D i18 Orb (MAS3 → MAS3 #4); i22 Key (MAS4 #7 → MAUSO1EA #76); i12 Staff |
| 3 | 1 | 0 | D i60 Mask (GNARL1 #16 → GNARL1 #3; the travel item, 7 maps) |
| 4 | 10 | 7 | i236 Key (STUDY2 #126 → #10); D i49 Statuette (STUDY4 #30 → STUDY2 #90/#232); i95 Rune guide; parchments |
| 5 | 4 | 1 | D i37 Crystal and i249 Crystal (CAVERNS → #48); i185 Coin (→ CHURCH1 #77) |
| 6 | 8 | 2 | D i33 Staff (CAVERNS3 #9 → STUDY1 #424/#445, AELF #16, CAVERNS3 #35); i84/i172/i220 journals (→ GRAVE flames); i65 Magnifying Glass |
| 7 | 3 | 4 | D i127 Chalice (ANUBIS #103 → #14, #68, #69); i19 Aelf's Dagger (#56) |
| 8 | 0 | 0 | (i49 counted under ch4: STUDY2 is listed there) |
| 9 | 1 | 0 | i137 Green Crystals (TEMPLE1 #51 → TGATE1F #13) |
| 10 | 19 | 1 | D i116 Ring; D i106 D i114 D i115 D i122 D i123 D i154 D i103 jewels (→ RAQUIA5 star); i98 Coloured Gem; i20 Key; i99 Pipes; i108→D i113 Earplugs; i124→D i202 Spectacles; i109 Haunch; i217 Gem; i211 Armlet; i144 Key of Tears |
| 11 | 0 | 0 | - |
| 12 | 13 | 0 | D i150 Wand, D i175 Magical Torch; keys i129, D i171, D i193, i218, i219; i101, D i128 Feathers; i102 Tiara; books, notebook |
| 13 | 10 | 1 | D i156 Chalice; D i6 Magnifying Glass; D i131 Fan; D i152 Bowl and Snow; D i155 Amber and Mortar; D i194 Tinder (Burning); i111 Water-pipe; i204 Cylinder and Flint; i149 Scroll |
| 14 | 0 | 0 | - (D i97 Eternity is given by AQUA1 #22 at the end of ch13) |
| 15 | 8 | 0 | news cuttings; Map of the Graveyard Maze i276 |
| 16 | 2 | 0 | D i223 Brain (DOMINION #8 → #47 #70 #71 #72); i225 Power of Dominion |
| 17 | 1 | 0 | i53 Perdition Shield |
| 18-20 | 0 | 0 | - |

Dossier items not first found in a chapter map: D i68 Adam (start), D i9 Colt-45 (STUDY1 #416 by click), D i167
Key (from the journal's inspection), D i216 Spirit Ring (worn) (recipe), D i97 Eternity (AQUA1 #22). Recipes: 24
OnUse records with a target item (combinations) and 18 on the item itself (wear, read, light) [V: `-items`
RECIPES].

---

## 4. Puzzle kinds [V: `-kinds`]

One kind per unit, by the first rule that matches (`Classify()` in chapter_census.cpp). "P" = has a gate or a
progress effect. The third column counts P units whose chain also holds a counter or step (a sequence).

| kind | P units | all units | with counter |
|---|---|---|---|
| locked door + key (item tested, door opened) | 21 | 21 | 0 |
| use item on object (item in hand, kept) | 44 | 44 | 11 |
| place / hand over item (item in hand, taken) | 34 | 34 | 25 |
| use item from the inventory (OnUse → RAW chain) | 6 | 6 | 0 |
| use item from the inventory: travel (the Mask) | 5 | 5 | 0 |
| carry gate (item anywhere in the inventory) | 14 | 14 | 0 |
| light with a fire item (Matchbox in hand) | 6 | 6 | 2 |
| switch / lever (click or use, no item) | 37 | 144 | 15 |
| light switch (base 0x02) | 0 | 31 | 0 |
| flag-gated use | 71 | 71 | 0 |
| flag-gated event (walk-in or timer) | 42 | 42 | 0 |
| dialogue (choices) | 11 | 11 | 0 |
| attack / kill (attack hits wall or object) | 40 | 40 | 25 |
| cutscene | 14 | 14 | 0 |
| chapter card | 22 | 22 | 0 |
| gated map transition (a flag or item tested before the 0x3b) | 10 | 10 | 0 |
| map transition (ungated) | 6 | 109 | 0 |
| take item (scripted pickup) | 194 | 194 | 8 |
| walk-in event | 76 | 150 | 3 |
| timer | 11 | 15 | 6 |
| door enabled later | 34 | 34 | 0 |
| door, free | 0 | 70 | 0 |
| flag set | 14 | 14 | 0 |
| examine (comment), comment, ambient | 4 | 802 | 0 |
| unreachable (hidden developer object) | 0 | 9 | 0 |
| **all** | **716** | **1,912** | **95** |

Plus, outside the maps: 24 item combinations (DBASE100 OnUse with a target).

Per chapter (P units; the house maps STUDY1-4 are counted under chapters 1 and 4 and 8, so chapters 2, 3, 7, 8, 9,
11, 19 and 20 are under-counted by the house puzzles they use):

| ch | P | main kinds |
|---|---|---|
| 1 (STUDY1, all its chapters) | 85 | 33 scripted takes, 7 place (serpents), 3 light, 3 attack, 1 key door, 9 doors enabled later, 3 cards, 3 gated transitions |
| 2 | 60 | 27 takes, 7 cutscenes, 5 switches, 5 attack, 4 timers, 1 key door, 2 use, 1 place, 1 carry |
| 3 | 5 | card, gated transition, switch, 2 takes |
| 4 (STUDY2, STUDY3, TOWER1) | 67 | 2 key doors, 3 place, Mask travel, 17 takes, 17 flag-gated uses (Rebecca's comments that set flags), 9 walk-in events |
| 5 | 23 | 2 dialogue, 3 attack, card (offering) |
| 6 | 22 | key door, use (Staff), 12 takes |
| 7 | 18 | 2 key doors (chalice thrown), carry, 4 switches (clock), 3 dialogue |
| 8 (STUDY4) | 9 | 3 light |
| 9 | 6 | Mask travel, carry |
| 10 | 145 | 4 key doors, 8 use, 10 place, 3 inventory use, 8 carry, 6 switches, 4 dialogue, 28 takes, 19 sequences |
| 12 | 88 | 10 key doors, 17 use (wards), 2 place, 19 doors enabled later |
| 13 | 52 | 16 use (chalice, glass), 4 place (plaques), 3 inventory use, 8 sequences |
| 15 | 22 | 3 place (flames) |
| 16 | 48 | 17 attack (mirrors), 4 place (brains), 16 flag-gated uses |
| 17 | 58 | 14 switches (dials), 8 attack, 23 walk-in events |
| 18 | 8 | card |

The VR categories the owner asked for, mapped to these kinds: locked door + key = "locked door + key"; use item on
object = "use item on object" + "place"; combine items = the 24 recipes; give to NPC = within "place / hand over"
(CAVERNS #48 the guardian, RAQUIA3 #31 the beast, CHURCH1 #77 the collection box; which of the 34 are characters is
[R], the data does not mark NPCs); dialogue gate = "dialogue" + the flag-gated kinds downstream of a choice; switch
sequence = "switch / lever" with a counter (15) + the bit-mask placements; light (candles) = "light (fire item)";
read document = documents are never tested by a gate except the journals at GRAVE's flames and the Journal whose
inspection gives key i167 [V: `-gates` "inspect i172"]; cutscene trigger = "cutscene" + "chapter card".

---

## 5. Where the original depends on timing or order [V: `-timing`; semantics read in ROTH.C]

Tally over P units: 102 with a delay, 83 with a count, 20 with a step, 11 timers, 22 with a random delay or choice,
24 comparing "the last item given", 16 list tests, 182 one-shot, 256 toggling other records.

1. **A delay stops the chain and resumes it later** (0x12, `cmd_delay_timer` raw_commands.c:4420; `tick_delay_timer`
   :5328). While a delay is armed, the trigger **does not fire again** (the flow pre-pass, GAME_core.md 5.1). Puzzle
   chains with delays: the candles (STUDY1 #4: 410 tics; STUDY4 #29: 150), the Orb door MAS3 #4 (760), the clock
   ANUBIS #2/#3/#34/#107 (20-660), the plaques AQUA1 (180) and jewels RAQUIA5 (240 each), the Rune guide STUDY2 #47
   (720), card 15 GRAVE #23 (1,460), the end STUDY1 #660 (200). VR must not let a second hand or a re-grab re-fire
   these: the original ignores it.
2. **Bit masks** (0x1e with key 0 sets the step mode and delta for the next Count, :4001-4007, :156-201): AQUA1 (4
   plaques, XOR 1/2/4/8, target 15), RAQUIA5 (7 jewels, XOR 1..64, target 127), SALVAT (8 dials, XOR 16/1/2/32/64/4/
   128/8, target 255), ANUBIS clock (XOR 2 and 1). Placing XORs a bit in, taking the item back XORs it out; the reward
   runs only when the mask is exactly full. A VR "drop near the plaque" must produce exactly one place and one take,
   never a double event.
3. **"The last item given" and lists** (0x27 f6&4 and f6&8): AQUA1 plaques, RAQUIA5 jewels, CAVERNS #48 (the
   offering), CHURCH1 #256 (the feather), GRAVE flames. The chain removes "the item used" and then compares which one
   it was. The order inside the chain (take, then compare) is the original's; the hand must hand over exactly the
   selected item.
4. **Counters that count events** (0x15/0x1f/0x22 in the chain, cmd_count :216): the serpent plates (1→7), the
   kill counts (AELF 3 and 14, MAS6 4, CAVERNS 3, RAQUIA5 6, DOPPLE 16 mirrors), DOMINION brains (5 and 4), GRAVE
   flames (3), STUDY4 candles (5), STUDY1 #4 candles (2), CHURCH1 #82 (4). Each counted record is one-shot or
   disabled after use: one VR action must be one event.
5. **Steps across records** (0x1e with a target): the serpent take-backs (STUDY1 #121, #136-141 step Count #156),
   SALVAT dials (each its own count 7→3), RAQUIA5 #17 (three switches to 4), MAS6 #48.
6. **Timers** (0x3d, started at load): TGATE1F/G/H/I and TOWER1 portal timers with random delays; MAS6 #29 (the timed
   lever: 120 tics, random); CAVERNS #8; RAQUIA5 #414 (off until enabled; counts kills); AELF #31/#47/#49.
7. **Random** (0x12 with modifier 0x02, DBASE100 0x0b, 0x3f flag 2): portal timers, random comments (act674/672).
   They draw from the command RNG (0x71f48); any extra or missing draw changes later results. [R: consequence]
8. **One-shot and order of firing**: 182 P units carry fire flag 0x10 (spent after one run); 0x17 enables and
   disables records so the same face does different things in sequence (the study wall STUDY1 #75/#354: the doors
   in the else branch until flag 82, then the card; the plates; the house collapse). Card 9 (`if !flag226`) and 16
   (`if !flag388`) guard against a second play.
9. **"In hand" versus "carried"**: 0x27 f6&2 needs the item selected; f6&0x20 forbids the auto-select. A VR hand
   holding the item must select it first, exactly as the original's click with the item on the cursor.
10. **Queues**: DBASE100 actions hit while a dialogue or movie is running are re-queued (8 entries,
    dbase100.c:437-446); a chain that plays a GDV mid-way continues after it. Cards 2-20 are all inside such chains.

---

## 6. The desks that do not open (owner's question, 2026-11-13)

**STUDY1 roll-top desk** (object id 1, art DEMO[4113], a mesh, sector 361, the sarcophagus room):
- The only record keyed to object id 1 is STUDY1 #288, an examine-object entry with **no chain** (start 0) whose
  word +0x0a runs act100: two lines and nothing else (op 0x05, 0x01 flag 1, 0x05). [V: world_census -chain STUDY1 288;
  DBASE100 dump of act100, this session]
- The other STUDY1 records with key 1 are other id spaces (sector id 1, face id 1, flag 1, sound node 1). No 0x0d,
  0x20, 0x16, 0x3c, 0x23, 0x24, 0x22 names object 1. [V: probe explore9]
- 0x3a re-keys only the object that started the chain (`cmd_change_object_id`, raw_commands.c:478-495); the desk
  starts no chain. None of STUDY1's 11 0x3a records gives the new id 1, and a minted id (0xffff) is 0xc1xx, never 1.
  [V: probe; ROTH.C read]
- No 0x17 targets #288, no 0x40 or DBASE100 0x23 runs it; no DBASE100 action runs object 1's click chain (0x36 with
  operand 1: none), and nothing else runs act100 (no RAW 0x2b, no examine, no 0x1d jump). [V: probes explore6,
  explore10, explore11, explore13]
- The art DEMO[4113] is placed once in the 44 maps; no map other than STUDY1 has an object within 250 units of the
  desk or shares the room's sectors. [V: world_census -kind; explore6, explore7]
- **Answer: the desk never opens in the original. [V]**

**CHURCH1 desk** (sectors 515/516/517, sector id 14, top at 72; 20 faces carrying face id 24, whose lower pieces
wear textures 730-733, the drawers):
- Records on face id 24: two examine-wall entries, #167 (act294: two lines) and #422 (act57: gives information
  record i174 "Tower Gates" once, flag 335, and lines). On sector id 14: one examine-floor entry, #168 (act294). All
  three have no chain. [V: world_census -chain, explore9, explore12]
- No use-wall (0x18), use-floor (0x19), texture change (0x34/0x0c/0x1c), door (0x2f) or 0x17 names face 24, sector 14
  or those records; nothing else runs act294. [V]
- **Answer: the CHURCH1 desk never opens either. [V]** Its #422 is the one place in the game a desk examine gives
  something (an information topic, not an item).

**Walkthroughs:** none of those read (JustAdventure pages 1-2, GameBoomers, cyberly.org, and a hint file on a PC
PowerPlay cover disc, discmaster.textfiles.com) mentions
opening a desk, drawers or a canopy; they mention items "on the writing table" and "on the chest of drawers" (lying
on top). JustAdventure ch. 12 says the bronze key from the rectory fridge was "never used" [R: walkthrough]; in the
data every key item that is found has a test somewhere (section 3), none at a desk. [V]

**[R]** The owner's memory may come from containers that do open (OPENABLES.md in `wt_drawers` lists them), or from
another game.

---

## 7. Verified versus reasoning only

**Verified [V]** (read from the install by `tools/chapter_census` and the probes named, or read in ROTH.C at the
cited line; nothing run):
- the 20 cards, their gallery titles, their actions and every record that plays them; the end action and record;
- the FILELIST runs and the 4-disk template in ROTH.C;
- the 127 transitions, the gate at each 0x3b record and its producers; the missing MAS5;
- the 1,912 units, their chains, the kinds and counts in section 4, the timing tally in section 5;
- the bit-mask puzzles' constants (XOR deltas and targets), the counter targets, the delays in tics;
- items: where placed, given, tested, removed, combined; the 9 hidden developer objects;
- the STUDY1 and CHURCH1 desks have no opening record.

**Reasoning only [R]:**
- which maps belong to which chapter (`kChapterMaps` in the tool) and the play order inside a chapter: the
  walkthrough plus the map graph; the cards themselves are [V];
- that the four FILELIST runs are the four CDs;
- the meaning of many records in words (which candle, which door, "thrown", "the beast"), and which placements are
  characters;
- that hidden objects cannot be clicked (the developer-object conclusion);
- the census's static walk: delays are walked through, an else branch is walked once, a count's tail is listed as
  if it ran; the "critical path" is a dependency reading, not a play-through. Space (which room a door closes) is not
  modelled, so a gate's necessity is not proven;
- nothing here was checked against the running original; the next step that would verify a chapter is an oracle
  probe that plays its gates in order and dumps the flags and inventory after each.
