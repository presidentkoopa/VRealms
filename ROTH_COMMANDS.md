# Realms of the Haunting — the level logic system
### What fires, what runs, and what every opcode means
*Decoded from ROTH.C (the original engine reconstruction), 2026-09-25.*

---

## The one thing to understand first

Realms levels carry **small programs**. Not "this door is a door" — actual lists of
instructions that run one after another.

There are **two different kinds of record**, and they share a format, which is the
single easiest thing to get wrong:

| | **Triggers** | **Instructions** |
|---|---|---|
| What they are | things the game *watches for* | things the game *does* |
| When they run | registered once at level load | executed in sequence |
| Example | "player walked into sector 12" | "open door 7" |
| The field at `+4` | **the chain's START** | **the NEXT instruction** |

That last row matters. The same two bytes mean different things depending on which
kind of record you are looking at. Read it wrong and every trigger in the game points
at the wrong place.

Across the whole game: **5,531 records, 1,937 chains**. The Study alone has 661
records and 224 trigger entry points.

---

## Record layout

```
+0x00  u16  size in bytes
+0x02  u8   modifier / state flags
+0x03  u8   opcode  ("base")
+0x04  u16  1-based index -- NEXT instruction, or a trigger's chain START
+0x06  u8   direction mask and fire flags   (triggers)
+0x07  u8   sub-flags                        (triggers)
+0x08  u16  key: an ID, looked up per-opcode (0 = "whatever the player just used")
+0x0a  u16  sound effect or auxiliary value
+0x0c..0x12  s16 x0, x1, z0, z1 -- optional bounding box (0 at +0x0c = no box)
```

Arguments are 2-byte values filling `size - 6` bytes. Observed counts run from 1 to 8;
most take 2 or 3, though some take 7.

### Fire flags (`+0x06`) on triggers
- `0x10` — **one-shot**: after firing, the record marks itself spent
- `0x20` / `0x40` — latch as the "current link" sector
- low bits — an **eight-way approach mask**, so a trigger can fire only when entered
  from particular directions

### Modifier byte (`+0x02`)
- `0x08` — **disabled / skip**. Checked by every scanner and the execution loop.
- `0x01` — armed for a tick re-run
- `0x02` — use the alternate value at `+0x0c`
- `0x04` — use connected-flood collection
- **`0x80` is never tested at runtime.** It is authoring metadata from the level
  editor, not game state. Do not build behaviour on it.
  **Correction:** the "a third of all records carry it" claim is about THIS
  modifier byte. On the OPCODE byte it is measured at **0 of 5,531 records across
  all 44 maps**, so the `& 0x7f` mask the executor applies is a no-op in practice.

---

## Triggers — what the game watches for

| Opcode | Fires when |
|---|---|
| `0x08` | the player **clicks / uses an object**. Also flips the object's state word afterwards, which is how switches and levers toggle. |
| `0x18` | the player **enters a sector** (channel A) |
| `0x32` | the player **enters a sector** (channel B) |
| `0x19` | the player **uses a wall** |
| `0x31` | the player uses a wall, **direction-sensitive** |
| `0x1a` | the player **bumps into a wall** |
| `0x1b` | the player **touches / activates an object** |
| `0x13` | the player **enters or leaves water or lava**. Fires on both edges. |
| `0x3d` | a **timer**, started at level load, firing when it expires |
| `0x25` | inert — does nothing in either table |
| `0x30` | sets a marker on matching objects. **What reads that marker is unresolved.** |

---

## Instructions — what actually happens

### World and geometry
| Opcode | Does |
|---|---|
| `0x2f` | open a door |
| `0x09` | move a sector |
| `0x07` | change height |
| `0x23` | change an object's height |
| `0x03` | modify a sector |
| `0x24` | rotate an object |
| `0x16` / `0x3c` | spawn an object (and an extended form) |
| `0x3a` | change an object's id |
| `0x3b` | **map transition** — move to another level |

### Light
| Opcode | Does |
|---|---|
| `0x1d` | change lighting |
| `0x11` | flash lights |
| `0x02` | light switch |

### Textures and animation
| Opcode | Does |
|---|---|
| `0x34` / `0x0c` | change a wall texture (and an extended form) |
| `0x0a` | change a floor texture |
| `0x0d` / `0x20` | change / cycle an object texture |
| `0x0e` / `0x0f` | scroll a sector or wall texture |
| `0x1c` | cycle a texture |
| `0x21` / `0x14` | animate / sync a group of wall textures |
| `0x2e` | smash a wall texture (breakage) |

### Control flow and state
| Opcode | Does |
|---|---|
| `0x26` | set a flag |
| `0x28` | branch if a flag is not set |
| `0x27` | branch if the player lacks an item |
| `0x38` | jump if the next step fails |
| `0x36` | branch on a dialogue record |
| `0x12` | delay / timer |
| `0x15`, `0x1e`, `0x22`, `0x1f` | counters and loops |
| `0x17` | toggle another command on or off |
| `0x40` | run a command by index |

### Player, items, effects
| Opcode | Does |
|---|---|
| `0x29` / `0x2a` | give / remove an item |
| `0x33` | apply damage |
| `0x35` | make a wall deal damage |
| `0x3f` | rotate the player |
| `0x41` | slow the player down |
| `0x42` | set an inventory filter |
| `0x2d` | particle effect |
| `0x10` | trigger a sound node |
| `0x2b` | run a dialogue record |

---

## What the key at `+0x08` actually names

**It is an ID that gets searched for, never an array index.** Three scanners do the
searching, all of them walking a section of the loaded map and comparing one field:

| Scanner | Walks | Stride | Matches | Returns |
|---|---|---|---|---|
| `find_geometry_record` | sectors | `0x1a` | sector `+0x14` — the field we call `floorTriggerID` | a byte offset |
| `find_raw_state_record` | texture maps | 10 or 14 | texmap `+0x0c` — `faceID`, only on extended records | a byte offset |
| `find_face_record` | faces | `0x0c` | face `+0x04` — `textureMapOffset` | a byte offset |

Two of those names are misleading and worth restating in our terms:

- **`find_raw_state_record` searches texture-map records**, not some separate state
  table. Its stride is 10 bytes, 14 when `byte[+1] & 0x80` — which is exactly our
  `TextureMap.extended`, the high bit of `fitWord`. Its match field is our `faceID`.
- **`find_face_record` is a reverse lookup**: given a texture-map record's offset, find
  the face that uses it. It is how the engine gets from a named wall group back to real
  geometry.

So naming a wall is a **two-hop** operation: `key` → the texture-map record carrying that
`faceID` → the face pointing at that record. Several walls can share one `faceID`, which
is how one command moves a whole group of them (`gather_faces_by_id` collects every
match, capped at 200).

**`floorTriggerID` is misnamed in our reader.** It is the sector's command ID — the handle
commands use to address it. The door sentinels are reserved values in that ID space.

### key = 0 means "the thing the player just used"

Not inert. `0` routes to the live interaction globals: `g_active_object` (a texture-map
record) and `g_active_object_secondary` (a face). This is what lets one command record
serve many doors — the trigger that fired supplies the target.

`gather_faces_by_id(0)` also adds the **sister face**, but only when the sister's
texture-map record carries the same `faceID`. Using either side of a shared wall affects
both.

**`65535` never appears in the key field** — not once across all 44 maps. It appears as an
*argument* (notably `cmd_spawn_object_adv`, where it means "spawn at the player").

### Which space each opcode uses

Measured across all 44 retail maps. "Resolves" = the key matches a real sector ID or
`faceID` in that same map.

**Geometry** — these resolve almost perfectly, so the key genuinely names map geometry:

| Opcode | Records | Resolves | key=0 | Unresolved |
|---|---|---|---|---|
| `0x18` enter sector A | 240 | 239 | 0 | 1 |
| `0x32` enter sector B | 259 | 258 | 0 | 1 |
| `0x13` water/lava | 443 | 428 | 0 | 15 |
| `0x19` use wall | 48 | 48 | 0 | 0 |
| `0x1a` bump wall | 72 | 72 | 0 | 0 |
| `0x31` use wall, directional | 125 | 123 | 1 | 1 |
| `0x2f` **open door** | 116 | 61 | 55 | **0** |
| `0x34` change wall texture | 103 | 98 | 5 | 0 |
| `0x07` change height | 126 | 125 | 0 | 1 |
| `0x1d` change lighting | 60 | 60 | 0 | 0 |
| `0x09` move sector | 32 | 32 | 0 | 0 |
| `0x0a` change floor texture | 56 | 56 | 0 | 0 |

`0x2f` having **zero** unresolved keys across the whole game is the strongest single
confirmation that the two-hop rule is right.

**Their own numbering** — these do not index geometry at all, and their ranges match the
tables they belong to:

| Opcodes | Key is | Observed | Bound |
|---|---|---|---|
| `0x26` set flag, `0x28` if-not-flag | DBASE100 record id | 1–431 | 433 records in `DBASE100.DAT` |
| `0x27` lacks item, `0x29` give, `0x2a` remove | DBASE100 inventory id | 3–279 | 281 |
| `0x2b` run dialogue, `0x36` branch on dialogue | DBASE100 dialogue id | 23–693 | 694 |
| `0x17` toggle, `0x38` jump-if-fails | **1-based command index** | 1–660 | 661 records in the largest map |
| `0x40` run by index | **1-based command index, but read from `+0x06`, NOT the key** | | see below |

Every one lands just inside its table's size. `0x17` resolving through
`resolve_command_by_index`, and `0x38` storing `word[rec+8]` straight into
`g_command_next_active`, both confirm the command-index reading from the code side.

**No key at all** — `0x33` damage (44 records, every key `0`: damage always hits the
player) and `0x3d` timer (15 records, every key `0`: a timer has nothing to point at).

**Still open:** `0x2d` particle effect (76 records, 24 distinct keys, 102–791 — fits no
table we have identified) and `0x0e`/`0x0f` texture scroll (values up to 16898, so `+8`
is probably not a key for these at all).

---

## `cmd_open_door` — and why our doors cannot be Doom doors

`cmd_open_door` resolves its key to a **face**, then calls `register_door_swing`:

| Field | Becomes |
|---|---|
| `+0x08` key | the wall, via the two-hop lookup |
| `+0x0a` × `+0x07` | swing extent — the two are multiplied when `+0x07` is nonzero |
| `+0x0c` | sound id **plus one**; `0` means silent, and a door with sound also gets a 1000-tick open timer |
| `+0x0e`, `+0x10` | **target vector x, y** — the point the wall moves to |

The engine's own dev shortcut, `dev_open_nearest_door`, walks the current sector's walls
and hands `spawn_door_instance` a **wall**, with extent 600 and no sound. Two independent
paths agreeing that the first argument is a wall, not a sector.

**This is a hinged wall swinging to an explicit destination point** — the door record
stores a target vector and the wall's geometry is moved toward it. It is not a floor or
ceiling mover. Modelling Realms doors as Doom door sectors will be wrong for every door
in the game; they need to move wall geometry.

### The door sentinels are three different things

`dev_open_nearest_door` will only make a door of a wall whose **far** sector has
`floorTriggerID >= 0xFFFD`, and `spawn_door_instance` then splits on the value:

| Value | Meaning |
|---|---|
| `0xFFFD` | door, **secondary** pool — at most 6 |
| `0xFFFE` | door-capable, and the two-sided wall is not drawn |
| `0xFFFF` | door, **primary** pool — at most 6, or 5 when a door and its neighbour spawn as a pair |

Our reader's `IsDoor()` tested only `0xFFFD` and `0xFFFF`, so it misses `0xFFFE` and
flattens a distinction the original keeps. Doors also spawn **in pairs** when
`resolve_door_neighbor_sector` finds a neighbour, which is how double doors work.

---

## Flags, items and dialogue all live outside the map

`cmd_set_flag` and `cmd_if_not_flag` operate on a **bitmap allocated once at game start**,
one bit per DBASE100 record: `((count + 0x20) & ~0x1F) >> 3` bytes. For the retail data
that is 433 records → **56 bytes, 448 flags**.

It is **global and persistent** — savegame chunk 6 writes it out whole. Progress flags
therefore survive level changes, which they must: the game is one continuous story.

`cmd_set_flag` picks its operation from `byte[rec+6]`: bit `0x02` toggles, else bit `0x01`
clears, else it sets. It reports "acted" only when the bit actually changed.

---

## `cmd_map_transition`

The destination is an **8-byte NUL-padded ASCII map name**, stored as the two dwords at
`+0x0a` and `+0x0e`; the key at `+0x08` is the arrival point. Read straight out of the
retail maps:

```
AELF      key=13  "STUDY1"
ANUBIS    key=24  "CHURCH1"
AQUA1     key=4   "LRINTH1"
DOPPLE    key=7   "ABAGATE2"
CAVERNS   key=0   "CAVERNS2"
```

Treat it as NUL-terminated rather than fixed-width: `CHURCH1`'s exit to `VICAR` stores
`V I C A R \0 1 \0` — a stray digit left behind the terminator when the name was
shortened.

### Three corrections, all measured

- **`0x40` does not read the key.** It takes its command index from **`word[rec+6]`**
  (`raw_commands.c:3958`), not `+0x08`. Every one of the 76 retail records is 8
  bytes long, so there is no `+0x08` to read at all; `+6` is in range for 76 of 76.
  `0x17` and `0x38` DO use the key and are in range 397/400 and 26/26.
- **`0x01` (62 records), `0x37` (4), `0x2c` and `0x39` are no-ops.** Their slots in
  the dispatch table are `cmd_default_nop` (`boot.c:189-205`), so they provably do
  nothing. They were simply undocumented.
- **Most "opcodes" are not handlers.** The dispatch table holds three kinds of
  entry: immediate, verified no-op, and **REGISTRAR** -- which allocates an
  active-effect record and returns, with the visible behaviour living in a
  SEPARATE per-frame tick handler reached through a second table at `0x3088c`
  (`boot.c:215-229`). Every lighting, texture and geometry-mover opcode is a
  registrar. `cmd_change_lighting` (`raw_commands.c:4390`) does not touch a light:
  it calls `alloc_active_effect` and writes a ramp step. So "implement the
  lighting opcode" is not one function -- it is the effect pool plus that tick
  table, and it is a stage of its own.

---

## A worked example, from the real Study

Two chains, decoded:

```
click/use an object
  if the player does NOT have item 16
    change object id      65535
    delay                 30
    change object id
    change object texture
    change lighting
```

A locked door. Check for the key; if it's missing, play a refusal — swap the
appearance, pause, swap back, flicker the light. The same routine appears twice with
different targets, which is what made it recognisable before any of it was decoded.

---

## What is still open

- **Opcode `0x30`** — the marker it sets is certain, what consumes it is not.
- **Two argument keys.** `0x2d` particle effect and `0x0e`/`0x0f` texture scroll are the
  only key fields left that fit no table we have identified.
- **The 15 trigger categories.** The Study uses 11 of them and the firers are named
  (contact, use, entry), but each category slot has not been individually mapped.

## Where this came from

`E:\VRealms\tools\ROTH.C\roth_c\src\engine\raw_commands.c` — handlers and their
opcode numbers appear in the comments. The execution table is transcribed at
`platform/boot.c:189-205`; the load-time trigger-registration table at
`map_load.c:1089-1097`.

Cross-check: `beyond-the-ire/file_documentation/RAW_commands.md` independently
identifies `0x29` as "add item to inventory", matching `cmd_give_item`. That document
describes the format correctly but its opcode list is an unfinished stub.
