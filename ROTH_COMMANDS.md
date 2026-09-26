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
+0x08  u16  key: which sector / face / object this watches  (0 = inert)
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
- **`0x80` is never tested at runtime.** A third of all records carry it. It is
  authoring metadata from the level editor, not game state. Do not build behaviour
  on it.

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
- **Argument meanings.** Knowing an instruction is "open door" does not say *which*
  door. Arguments index into geometry and object tables, and those index spaces have
  not been verified.
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
