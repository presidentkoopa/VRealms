# GAME LAYER: Inventory and Examine (close-up), spec for the REMAROTH port

Source root abbreviations: `E/` = `/mnt/user-data/uploads/VRealms/tools/ROTH.C/roth_c/src/engine/`; `RE/` = `/mnt/user-data/uploads/VRealms/tools/roth-editor/src/resources/`; `BTI/` = `/mnt/user-data/uploads/VRealms/tools/beyond-the-ire/file_documentation/`; `RR` = scratchpad `rotheditor_rules.md`. Every `0x....` in parentheses is the original function VA. [inferred] = derived from code but not stated or tested; [open] = cannot be resolved from staged sources.

---

## 1. What the player experiences

1. Adam left-clicks a nearby pickable sprite, meaning one within squared distance 0x258 (`E/input.c:1031`). The sprite leaves the world (`E/input.c:1084`), flies toward the camera for 0x1c ticks (`E/inventory.c:106-139`), and lands in the pack (`E/inventory.c:1077`, `give_item`). A weapon is equipped straight away (`E/inventory.c:418-428`). Any other item becomes the selected right-hand item (`E/inventory.c:433-434`), and its icon slides into the bottom-right corner of the view (`E/inventory.c:1498-1521`).
2. The inventory is a panel with five category tabs (`E/inventory.c:1526-1555`), a scrolling 10-cell grid (`E/inventory.c:1951-1979`), two hand slots (left = weapon, right = selected item; `E/inventory.c:1213-1247`, `E/inventory.c:1339-1340`), an "Adam" portrait slot showing his name (`E/inventory.c:1905-1927`), and a name line with a count, e.g. "name (n)" (`E/inventory.c:1026-1072`).
3. Clicking a cell with nothing held opens the close-up (`E/dialogue_ui.c:1770, 1791-1796`). Picking a cell up with the other button attaches it to the cursor (`E/dialogue_ui.c:1767-1788`). Then:
   - clicking the same cell again uses the item on Adam (`E/dialogue_ui.c:1745-1749`);
   - clicking another cell with the primary button combines the two (`:1754-1756`);
   - clicking another cell with the secondary button swaps their order (`:1761`);
   - clicking a hand slot equips or selects the item (`:1695-1701`, `:1723-1729`).
   Each use or combine prints a timed message such as "used X" or "can't combine X and Y" (`E/inventory.c:716-758`, `:826-860`).
4. Examine (the close-up popup) plays the item's close-up GDV from DBASE300 in a framed window. The window shows the item name, the small icon, and a toolbar: a magnifier that replays Adam's comment, an "i" that lists information topics, and page arrows for documents (`E/dialogue_ui.c:1299-1541`). The first time an item is examined, Adam speaks the OnInspect comment and its subtitle appears as a caption (`E/file_config.c:962-967`).
5. Right-clicking a pickable item in the world plays the same OnInspect comment without opening any popup (`E/input.c:995-1012`).

---

## 2. Data

### 2.1 DBASE100 header (relevant fields)
`BTI/DBASE100.md:14-27` and `RE/parsers/dbase_100.gd:4-17`.
- +0x10 inventory count. The engine bounds-checks item indices against `[base+0x10]` (`E/inventory.c:52, 384`).
- +0x14 inventory table offset.
- +0x18 action count. `eval_dialogue_record_by_id` checks against `[base+0x18]` (`E/dbase100.c:549`).
- +0x28/+0x2C interface count and table. `resolve_dbase100_text` reads these (`E/dbase100.c:412-414`).

### 2.2 Inventory table
- `g_dbase100_inventory_table` 0x81e20 and `g_dbase100_base` 0x81e1c (`E/g_names.h:285-286`).
- The record for index *i* is at `base + table[i]`. A table entry of 0 means no record, and index ≤ 0 means none (`E/renderer.c:1362-1373`).
- **The inventory item id stored in a slot is this table index (1-based), not the render id** (`E/inventory.c:343-363`, `ebx++` before `give_item`).
- That the in-memory table is the file's +0x14 section is [inferred].

### 2.3 Inventory record (variable length)
Layout from `BTI/DBASE100.md:54-66` and `RE/parsers/dbase_100.gd:31-40`. Bit meanings come from ROTH.C usage.

| Off | Size | Field | Engine meaning (ROTH.C) |
|---|---|---|---|
| +0x00 | u16 | length | |
| +0x02 | u16 | object render id | Bits 0-8: FAT-3 index; 0x200 = ADEMO (`BTI/DBASE100.md:57`). Matched against the world object's id word (`E/inventory.c:56-57`). Ids < 0x200 are never pickable (`:48`). |
| +0x04 | u8 | "closeup_type": really a **flags byte** | 0x01 → passed as close-up `param_2`, i.e. the modal hold/loop mode (`E/inventory.c:1834`, `E/file_config.c:930,976`); the editor notes 0x00 = loop, 0x09 = play once (`RE/parsers/dbase_100.gd:34`).<br>0x02 = has Document (trigger 7) block (`E/dialogue_ui.c:490`).<br>0x04 = has ChangeName (trigger 2) block (`E/inventory.c:76`).<br>0x08 = has OnInformation (trigger 1) blocks (`E/dialogue_ui.c:77, 1068`).<br>0x10 = WeaponAction / weapon (`E/inventory.c:392,418`).<br>0x20 = has AsBullet block, copied by `init_inventory_item_object` (`E/inventory.c:156`) [inferred name].<br>0x40 = has OnInspect (trigger 4) block (`E/renderer.c:1485-1487`), also shows the magnifier (`E/dialogue_ui.c:1504`).<br>0x80 = not pickable (`E/inventory.c:59`). |
| +0x05 | u8 | item_type | Low nibble = category / tab (`E/inventory.c:60-62,1332`). Category 2 ("characters and important info", `BTI/DBASE100.md:101`) is never pickable, never duplicated, and never put in a hand.<br>0x10 = Adam / always-has-topics (`E/dialogue_ui.c:76`, `E/inventory.c:2216`).<br>0x20 = stackable (`E/inventory.c:398`).<br>0x40 = has OnUse / combinable (`E/inventory.c:557`). |
| +0x06 | u8 | unk | |
| +0x07 | u8 | runtime | Bit 0x80 = "already examined", set on the first close-up (`E/file_config.c:962-963`). It also picks the magnifier sprite (`E/dialogue_ui.c:1506`). |
| +0x08 | u32 | close-up | DBASE300 offset/8. The engine seeks to `(v<<3)+4` (skipping the record's size word) and treats the data as a GDV (`E/inventory.c:1832-1834`, `E/file_config.c:941-960`). |
| +0x0C | u32 | icon | DBASE200 offset/8 (`E/inventory.c:977,1476-1486`). |
| +0x10 | u32 | name | DBASE400 offset (`E/inventory.c:704,1037`). |
| +0x14 | … | trigger blocks | Each starts with a u32 header `{size:24 (bytes, including header), trigger:8}`. The list ends at size 0 (`E/inventory.c:78-95`, `RE/parsers/dbase_100.gd:164-205`). The block body is u32 commands `{operand:24, opcode:7, IfNot:1}` (`E/dbase100.c:630-632`). |

Trigger codes (`BTI/DBASE100.md:76-86`):
- 1 OnInformation: topic list.
- 2 ChangeName.
- 4 OnInspect.
- 5 WeaponAction.
- 6 AsBullet.
- 7 Document: page list, using opcode 0x00 operands.
- 0x0A AsMonster.
- 0x0B OnUse: `0x34` target, `0xB4` result, `0x35` effect action id (`E/inventory.c:579-584`).

### 2.4 Related files
- **DBASE200 icon** (`RE/parsers/dbase_200.gd:11-15`): u32 size, then u32 type (3), u16 w, u16 h, then 0xF0-RLE data. `update_selected_item_icon` reads the size and payload and takes w/h from payload+4/+6 (`E/inventory.c:1480-1486`).
- **DBASE300 close-up**: a GDV (sig 0x29111994, `RR` A6). w/h are read at GDV+0x14 (`E/file_config.c:944-946`).
- **Document pages** are DBASE300 resources loaded by `load_das_cache_resource` on the DBASE300 handle (`E/dialogue_ui.c:1843-1862`) [inferred to be DBASE300 offsets/8, per `RE/opcodes.gd` op 0].
- **DBASE400 text**: 8-byte header {u32 voice clip (DBASE500 offset/8), u16 len, u8 color}, then the text (`E/dbase100.c:382-392`, `RE/parsers/dbase_400.gd:4-9`). A colour of 0 defaults to 0x20 (`E/dbase100.c:388`).
- **DBASE500 speech** is primed only when read with `flag≠0` (`E/dbase100.c:393-395`).
- **System messages** use DBASE100 interface indices through `resolve_dbase100_text`: 0x2a "can't use", 0x2d "used", 0x29 "can't combine", 0x28 "combined" (`E/inventory.c:718,737,828,847`). A missing entry falls back to "Missing Text" (`E/dbase100.c:420`).

### 2.5 World object → inventory record
- The picked hit-record has type at +1, def pointer at +0xE, and dist² at +0x20 (`E/input.c:895-896`).
- For type 4, `u16 def[+4]` is the id matched against record +2 (`E/input.c:1077,1211`).
- def matches the 16-byte RAW object: posX +0, posY +2, textureIndex +4, textureSource +5, flags +7, posZ +0xA (`RR:28`). `give_item` reads exactly those offsets (`E/inventory.c:445-454`).
- So **id = textureIndex | textureSource<<8**. Only source ≥ 2 (ADEMO) objects can be items (id ≥ 0x200) [inferred].
- `def[+9]` flag bits 0x01 keyed, 0x02, 0x10, 0x20 gate interaction (`E/input.c:1036-1057,1121-1132`). RAW calls byte +9 "renderType"; whether the runtime rewrites it is [open].

---

## 3. Inventory state

| Global | VA | Meaning |
|---|---|---|
| `g_inventory_slots` | 0x80c30 | 256 × {u16 item_id, u16 quantity} (`E/inventory.c:12-13`, `E/g_names.h:260`). id 0 = empty. **Bit 15 of id = hidden by filter** (`E/inventory.c:1266,1282`). |
| `g_inventory_count` | 0x80c2c | Used-slot count. Maximum 0x100, and `give_item` fails when it is reached (`E/inventory.c:381`). |
| `g_selected_item_secondary` | 0x81038 | Slot pointer of the **left hand**, the equipped weapon (`E/weapon_combat.c:432`, `E/inventory.c:1339`). |
| `g_selected_item_primary` | 0x81044 | Slot pointer of the **right hand**, the selected usable item (`E/inventory.c:433,1340`). |
| `+0x8/+0xc/+0x10` | 0x8104c.. | Adam record: slot, record, id (`E/inventory.c:2226-2228`). |
| cursor-entry table | 0x7fef4 | 12-byte entries {icon handle, slot pointer, id} built per tab (`E/inventory.c:1314-1386`); count at 0x80af4 (`E/g_names.h:247-248`). |
| `g_current_cursor_entry` | 0x7fef0 | The entry held on the cursor. |
| `g_left/right_hand_item` | 0x81030/0x8103c | Cursor entries of the hands. `g_displayed_item_left/right` 0x81034/0x81040. |
| per-tab cursor | 0x80afc / 0x80b10 | Cursor positions and scroll offsets; the active list is at 0x80b38 (`E/savegame.c:1400-1406`). |
| pickup lock | 0x7fd84.. | bit0 = armed (`E/renderer.c:9880-9887`). |
| `g_last_item_record` | 0x89fa8 | Last item given or matched (`E/raw_commands.c:4827`). |
| `g_item_autoselected_flag` | 0x89f60 | Set by `query_player_inventory` / `find_or_autoselect` (`E/inventory.c:501,1435`). |
| `g_object_select_easy_flag` | 0x81e34 | User toggle. When set, the game does **not** auto-select items for scripts (`E/inventory.c:478,493`, `E/input.c:523-527`). |

- **Ordering.** Slots keep insertion order. `find_free_inventory_slot` returns the first empty slot (`E/renderer.c:10030-10041`). `swap_inventory_entries` physically swaps slot words to reorder (`E/inventory.c:667-674`).
- **Stacking.** A stackable (+5&0x20) item increments an existing slot's quantity (`E/inventory.c:398-403`). There is no cap [inferred from absence].
- **Weapons.** A weapon slot's quantity is the loaded rounds, taken from WeaponAction attrs+4, `<<8` if infinite (`E/inventory.c:392-396`). Firing removes one ammo item and adds attrs+4 to the weapon slot (`E/weapon_combat.c:849-853`). The ammo display is the count of ammo-id items (`E/weapon_combat.c:448-452`, `E/inventory.c:264-267,438-442`).
- **Persistence** (savegame):
  - chunk 7 = the 0x400-byte slot array raw, including hidden bits. On load the count is recomputed and the weapon list rebuilt (`E/savegame.c:1410-1417,1593-1594`).
  - chunk 2 = the hand slot pointers as base-relative offsets (`E/savegame.c:152-157,186-192`).
  - chunks 4/5 = cursor positions and scroll offsets (`E/savegame.c:1400-1405`).
  - chunk 6 = the DBASE100 flag bitmap (`E/savegame.c:1406-1408`).
  - Whether record +7 bit 0x80 ("examined") persists is [open]: no chunk writes the DBASE100 image.

---

## 4. Operations as rules

**Pick up**: `activate_targeted_object` (0x164c9), `E/input.c:1026-1101`.
1. Refuse if dist² ≥ 0x258.
2. For type 4:
   - If def+9 & 0x12, refuse.
   - Keyed (&1): run `find_unflagged_object_by_key` then `run_leftclick_object_trigger`. There is no pickup.
   - If the pickup lock is armed, or &0x10, refuse.
   - If `run_leftclick_object_trigger(def)` ≠ 0, the object has a use chain: `fire_object_use_trigger`, and there is no pickup (`:1059-1069`).
   - Otherwise call `give_item_by_dbase_id(def[+4], def)`. On success, `destroy_dynamic_entity(def, 0x7114c)` and cursor 0x268. On failure, cursor 0x240 (`:1077-1085`).
3. Type 6 toggles a door. Other types fire the use trigger.
4. The cursor preview uses the same tests (`classify_cursor_target_object`, `E/input.c:1199-1221`).

**Give**: `give_item(index, ctx)` (0x1cedc), `E/inventory.c:372-462`.
1. Fail if the pack is full or the index is out of range. Index 0 means resolve through `ctx[+4]` (`:381-386`).
2. Category-2 item already held: return without adding (`:407-409`).
3. Otherwise stack or use a new slot (`:398-414`).
4. Weapon: `activate_weapon_item(slot,id)` into the left hand, and bump the HUD counter. Category 2: no select. Otherwise: select into the right hand plus refresh the icon (`:416-434`).
5. If the index is the active ammo id, increment the ammo display.
6. With a ctx, arm the fly animation from ctx x/y/z+0x18. Id = the record id if ≥ 0x200, else `ctx[+4]`, or 0 if ctx+7&1 (`:444-456`).
7. For weapons, `rebuild_weapon_inventory_list`.
8. Return the index.

**Remove**: `remove_item(id)` (0x1d077), `E/inventory.c:283-320`.
1. Check order: right hand, left hand, then the slot scan.
2. `remove_inventory_item` (`:246-277`):
   - left hand → unequip `activate_weapon_item(0,0)`;
   - right hand → clear the selection;
   - active ammo → decrement the display;
   - stackable with quantity > 1 → decrement quantity; otherwise clear the slot and decrement the count.
3. Returns -1 if something was removed, else 0.
4. `consume_held_item(slot)` removes a given slot (`:325-338`).

**Query (the has-item test)**: `query_player_inventory(id, flags)` (0x1ccf7), `E/inventory.c:472-531`.
- Count mode (flags&1 = 0): number of matching slots. If exactly one stackable slot matches, return its quantity.
- Select mode (flags&1):
  - item in the right hand → 1, and reset the slide timer;
  - item in the left hand → 1;
  - otherwise, unless the easy flag is set or flags&2, auto-select the first match into the right hand, set `g_item_autoselected_flag=1`, and **return 0**.
- The hidden bit makes the item invisible to every query, because the full u16 is compared (`:498,512`) [inferred consequence].
- The multi-item version is `find_or_autoselect_inventory_item` (0x1cb6c). It picks a **random** match through `rng_next_index_for_count` (`E/inventory.c:1394-1452`).

**Use on world.** No item-vs-object hit test exists. The right-hand item is used implicitly: clicking an object runs its RAW chain, and RAW opcode 0x27 tests the item with select mode = `rec[6]&2`, adding 2 if `rec[6]&0x20` (`E/raw_commands.c:4785-4788`).
- If the item is carried but not in hand, it gets auto-selected, the chain is interrupted (`g_command_chain_interrupt=1`), and the click does nothing (`E/raw_commands.c:4747-4752`). A second click then succeeds [inferred].

**Use on self**: `use_item_on_self` (0x1b141), `E/inventory.c:691-776`.
1. `resolve_item_use_action(rec, target=0)` finds an OnUse block whose 0x34 target equals 0 (`:551-594`).
2. No block: message 0x2a, stop.
3. Otherwise, message 0x2d.
4. Unless the block is effect-only (-1): consume the held item, then give the low16 result, then the high16 result.
5. Clear the held cursor.
6. If a 0x35 effect exists, run `eval_dialogue_record_by_id(effect)` (the action table) and `finish_dialogue_record_eval`.
- **Quirk:** an effect-only item is **not consumed** here. Its effect chain must remove it itself [inferred].

**Combine**: `combine_held_item_with_target(idx)` (0x1b26d), `E/inventory.c:787-928`.
1. Try held×target when held+5&0x40, else target×held (`:806-809`).
2. No recipe: message 0x29 ("%s %s", held then target names).
3. Otherwise message 0x28, then the 5-way dispatch (`:864-892`). An ingredient whose id equals a result is kept:
   - second==held → consume target, give first, keep held on cursor;
   - first==held → consume target, give second, keep held;
   - second==target / first==target → consume held, give the other;
   - neither → consume both, give both.
   - With a single result, consume both and give it.
4. Refresh, then run the effect.

**Examine**: see §5. **Drop**: none. No code path returns an item to the world. Only `destroy_dynamic_entity` on pickup, and `cmd_give_item`'s drop position is the *fly-in* origin (`E/raw_commands.c:4806-4822`) [inferred: ROTH has no drop]. **Give to NPC**: none as an inventory verb. NPC interactions are RAW/DBASE chains that test with 0x27 / op 0x02 and remove with 0x2a / op 0x91 [inferred].

**Equip and hands**
- Left hand (weapon): `activate_weapon_item(slot,id)` sets 0x81038, parses attrs, and sets the ammo model (`E/weapon_combat.c:426-455`).
- Right hand: set `g_selected_item_primary` and call `update_selected_item_icon`.
- Routes into the hands:
  - inventory Enter: list 1 → equip, other lists → select, list 2 → nothing (`E/inventory.c:1710-1720`);
  - `commit_held_cursor_item` when the panel closes with an item held (`E/inventory.c:2178-2196`, triggered by action 0x26 = click outside the panel, `E/dialogue_ui.c:576-581,1651-1657`);
  - hand-slot clicks (`E/dialogue_ui.c:1683-1737`).
- Picking a hand's cell onto the cursor empties that hand (`E/dialogue_ui.c:1773-1786`).
- A thrown/consumable weapon (end-of-shot condition) consumes the left-hand slot and reverts to fists (`E/player.c:524-528`).

**Filter** (RAW 0x42 "take inventory"): `set_inventory_list_filter` (`E/inventory.c:1255-1288`, `E/raw_commands.c:4940-4947`).
- mode 0 sets bit 15 on every non-category-2 slot, clears both hands, and resets the weapon HUD.
- mode 1 clears the bit.
- **Quirk:** `rebuild_weapon_inventory_list` masks bit 15 away, so hidden weapons stay in the weapon list (`E/weapon_combat.c:501`).

**Lifecycle**
- New game: `reset_inventory` → run action 3 (the starting items) → `restore_active_held_item` (finds the record with +5&0x10, i.e. Adam) → `activate_weapon_item(0,0)` (`E/game_core.c:823-833`, `E/dbase100.c:236`).
- Death restart: reset, then action 1 (`E/game_core.c:760-766`).

**Tab membership**
- An entry appears in tab *t* if its category equals `tab_map[t]` (`E/inventory.c:1333`). The table is at 0x7123c (5 bytes); its values are [open].
- A category-2 entry is listed only if `node_has_available_choice` (`E/inventory.c:1335-1336`, `E/dialogue_ui.c:74-97`).
- Hidden or negative ids are skipped (`E/inventory.c:1328`).

**Label**
- The name is record+0x10. If a ChangeName block exists and evaluates true, its first command's operand is used instead (`E/inventory.c:73-97,1031-1072`).
- The quantity is appended when non-zero. For infinite-ammo weapons the quantity shown is `qty>>8` (`E/inventory.c:1581-1584`).

---

## 5. Examine / close-up

**Entry.** `g_inventory_inspect_request` 0x80b3c is set in one of three ways:
- a primary click on a cell with nothing held (`E/dialogue_ui.c:1770,1791-1796`);
- the Space key over a cell (`E/inventory.c:1792-1797`);
- a primary click on a hand slot (`E/dialogue_ui.c:1686-1690,1714-1718`).

`update_inventory_screen` then calls `load_dbase300_resource_at_offset((rec+8)<<3 + 4, rec+4 & 1, rec, entry)`. It does nothing if the close-up offset is 0 (`E/inventory.c:1827-1836`).

**Modal** (`E/file_config.c:900-1144`):
1. Open DBASE300, with a CD-retry prompt (`:913-920`).
2. Read the GDV w/h.
3. `render_inspect_popup_window(w,h,rec,entry)` lays out the frame (`E/dialogue_ui.c:1299-1541`):
   - the icon (**entry's DBASE200 icon**) at the top-left (`:1471-1487`);
   - the name (`:1489-1494`);
   - the magnifier if rec+4&0x40, sprite 0x98 if already examined, else 0xa0 (`:1504-1519`);
   - the "i" button if any OnInformation topic is available (`:1522-1531`);
   - page arrows if a Document block exists (`:1335-1347`).
4. Open the GDV. **On first view** (rec+7 bit 0x80 clear), set the bit and run the OnInspect block (`scan_tag4_chunk` + eval) (`E/file_config.c:962-967`).
5. Decode the GDV. The frame callback polls Esc/I (exit), Space (state 1→2, switching to document mode), and Enter (interrupt the voice) (`E/gdv_cutscene.c:1574-1590`).
6. If `rec+4&1` or pages exist, and the decode did not return 0x100, enter the blocking loop (`:989-1122`). The loop handles:
   - page turns (`load_inspect_document_page`);
   - image scroll in steps of 8 (arrows, or a mouse drag via action 4);
   - topic navigation (Up/Down when the choice panel is open);
   - Enter = accept a topic, or exit;
   - exit on Esc/I/Space when the voice is not interruptible, or when the panel closes (`:1071-1121`).
7. Free everything and clear `g_inspect_popup_active` (`:1130-1142`).

- The exact meaning of flags bit0 versus the GDV loop flag 8 (`:929-930`) is [inferred]: bit0 clear loops the animation until dismissed, bit0 set plays once and then holds.

**Caption and speech.** OnInspect is ordinary DBASE100 commands, typically op 0x05 with a DBASE400 offset (`BTI/DBASE100_inventory_examples.md:375-376`). Op 0x05 reads the text with flag 1, which primes the DBASE500 voice, and opens the subtitle window `dbase100_open_dialogue_window_alt`, setting busy (`E/dbase100.c:852-864`). The caption under the popup is that dialogue window, not a field of the item.

**Toolbar actions** (`dispatch_dialogue_ui_action`, `E/dialogue_ui.c:1556-1808`; hit codes from `E/dialogue_ui.c:609-659`):

| Code | Button | Effect |
|---|---|---|
| 2 | magnifier | Re-evaluates the OnInspect block, i.e. replays the comment (`:1569-1579`). |
| 3 | "i" | Toggles the topic panel (`:1580-1593`). |
| 5/6 | page up/down | Turn a document page (`:1604-1627`). |
| 7..0x16 | topic line | Selects that topic → `activate_selected_choice_record` evals the OnInformation block with mode 0 (`:1628-1630`, `E/dialogue_ui.c:191-203`). |
| 4 | drag | Scrolls the image. |

- Topic text is the first command operand of each type-1 block whose condition passes in scan mode 2 (`E/dialogue_ui.c:1059-1090`).

**World examine.** There is no popup (`examine_object_under_cursor`, `E/input.c:979-1015`).
- If the object has a RAW command trigger, `examine_world_object` runs its DBASE100 action `rec+0xa` plus the interact trigger (`:953-970`).
- Otherwise, for a type-4 object with id ≥ 0x200 whose record has OnInspect, the game evaluates that block with flag 1 (`:995-1012`, `E/dialogue_ui.c:518-539`).

---

## 6. Opcodes that touch inventory or examine

**RAW map commands** (`E/raw_commands.c`):
- 0x27 if-not-item, with list/compare/select modes (`:4760-4789`).
- 0x29 give, which sets the fly-in origin from the active object's edge (`:4799-4831`). It fails, and sets interrupt=2, when the pack is full.
- 0x2a remove. id 0 means the last item (`:4918-4931`).
- 0x42 filter (`:4940-4947`).
- Keys are DBASE100 inventory ids 3–279 (`/mnt/user-data/uploads/REMAROTH/ROTH_COMMANDS.md:216`).

**DBASE100 interpreter** (`E/dbase100.c:607-953`):

| Op | Effect |
|---|---|
| 0x11 | give |
| 0x91 | remove (IfNot bit set) (`:767-772`) |
| 0x02 / 0x82 | count > / ≤ threshold gate (count mode) (`:810-818`) |
| 0x03 / 0x83 | count == gate (`:820-832`) |
| 0x05 | spoken line with subtitle (`:852-864`) |
| 0x08 / 0x09 / 0x0A | choice menu |
| 0x0E | full-screen image (requeued while the inspect popup is active, `:909-919`) |
| 0x07 | GDV (same requeue, `:921-942`) |
| 0x2D | sub-code 7 closes the inventory (`:887-897`) |

**Parsed outside the interpreter:**
- 0x00 document page (`E/dialogue_ui.c:488-511`)
- 0x0F topic
- 0x10 ChangeName (`E/inventory.c:73-97`)
- 0x34 / 0xB4 / 0x35 OnUse (`E/inventory.c:551-594`)
- 0x13–0x33 weapon and bullet attrs

---

## 7. Rules (port 1:1) versus presentation (replace)

**Rules: pure logic, port verbatim**
- `is_item_id_pickable`
- `give_item_by_dbase_id`
- `give_item` (minus the fly-anim call; keep the lock timing)
- `remove_inventory_item`, `remove_item`, `consume_held_item`
- `query_player_inventory`, `find_or_autoselect_inventory_item` (use the game RNG)
- `resolve_item_use_action`, `resolve_record_conditional_op2`
- `use_item_on_self` and `combine_held_item_with_target` (logic; the messages go to the engine's text system)
- `swap_inventory_entries` (slot swap and hand re-point)
- `set_inventory_list_filter`
- `init_inventory_item_object`
- `get_item_tab_index`
- `build_inventory_entry_list` (the list/filter part, not icon loading)
- `commit_held_cursor_item`, `restore_active_held_item` (minus the DAS load)
- `reset_inventory`, `find_free_inventory_slot`, `stack_onto_inventory_slot`
- `find_oninspect_block_by_id`, `scan_tag4_chunk`, `copy_record_block_op7`
- `node_has_available_choice`, `build_available_choice_menu` (the selection part)
- `activate_selected_choice_record`
- `examine_object_under_cursor`, `activate_targeted_object`, `classify_cursor_target_object` (the gates; the cursor shape ids become engine affordances)
- the first-examine rule (`E/file_config.c:962-967`)
- the item handlers in `cmd_if_not_item`, `cmd_give_item`, `cmd_remove_item`, `cmd_set_inventory_filter`
- DBASE100 ops 0x02 / 0x03 / 0x11
- save chunks 2 and 7
- the pickup-lock timer (0x1c ticks, `E/inventory.c:121,134-137`) as a rule gate

**Presentation: replace**
- `render_inventory_panel`, `render_inventory_grid`, `draw_inventory_tabs`
- `draw_item_icon_*`, `draw_equipped_item_*`, `draw_panel_slot_tile`
- `blit_item_icon`, `encode_item_icon_to_spans`
- `update_selected_item_icon`, `draw_held_item_icon`
- `refresh_inventory_*`, `redraw_inventory_cursor_cell`, `close_inventory_panel` (except the move/flag resets)
- `update_inventory_screen`'s key mapping (keep the semantic actions)
- `hit_test_dialogue_ui_action`
- the `dispatch_dialogue_ui_action` layout parts (keep the verb semantics of each action)
- `render_inspect_popup_window`, `load_inspect_document_page` drawing, `draw_das_panel_slide_reveal`
- the GDV blit and the inspect frame callback's drawing
- `tick_item_pickup_lock`'s screen interpolation
- the 10-cell / 5-row scroll arithmetic (`E/inventory.c:1751-1791`)

---

## 8. Dependencies

- **DBASE100 interpreter and flags**: `execute_dbase100_chain`, bitmap flags (`E/dbase100.c:607`).
- **Action table**: `eval_dialogue_record_by_id` (`E/dbase100.c:545`).
- **Text**: DBASE400 through `read_next_dialogue_line` and `resolve_dbase100_text`; timed messages through `queue_timed_message_color`.
- **Speech**: DBASE500 through `prime_voice_clip` (`E/dbase100.c:395`); dialogue busy/queue gates (`E/dbase100.c:424-447`).
- **Video**: GDV decoder (`E/file_config.c:960-981`).
- **Weapons**: `activate_weapon_item`, `apply_weapon_action_attributes`, `rebuild_weapon_inventory_list`, the ammo globals.
- **RAW commands and triggers**: `run_leftclick_object_trigger`, `fire_object_use_trigger`, `dispatch_entry_command_trigger(_b)`.
- **Entities**: `destroy_dynamic_entity`.
- **RNG**: `rng_next_index_for_count`.
- **Savegame.**
- **Game-state**: movement lock `g_player_movement_enabled`=3 while the inventory is open (`E/inventory.c:1865`), restored to 1 (`:1674`).

---

## 9. Size, traps, open questions

**Size.** Rules code is about 900 lines of C: inventory.c sections A+B ≈ 560, input pickers ≈ 250, raw handlers ≈ 90, inspect-rule parts ≈ 60. An idiomatic port is ≈ 600–800 lines [inferred].

**Traps to preserve**
1. Slot id = table index; the world id is the render word. Ids < 0x200 are never items.
2. The select-mode query returns 0 and auto-selects. With the easy flag set, it does not.
3. The hidden bit 15 hides items from queries, the list, and stacking. `stack_onto` compares sign-extended values, so picking up a duplicate while it is hidden creates a **second slot** [inferred].
4. An effect-only OnUse does not consume the item.
5. Category-2 items are never duplicated or selected.
6. The post-combine held re-find loop is dead (`E/inventory.c:897-909`), so the cursor keeps a stale entry pointer until the list is rebuilt.
7. The equipped path of `query_player_inventory` does not reset the slide timer (`E/inventory.c:490-492`).
8. `activate_weapon_item` reads `[slot+2]` (the quantity) as "item id" (`E/weapon_combat.c:441-442`) [open: intended?].
9. Pressing the key for the tab that is already active does nothing (`E/inventory.c:1745`).
10. On the first examine, the flag is set **before** the speech runs.
11. Op 0x05/0x07/0x0E requeue while the popup is up.
12. The pickup lock blocks further pickups for 0x1c ticks.
13. `give_item` returns 0 when the pack is full. The object then stays in the world (`E/input.c:1078`).

**Open questions**
- The 5 tab category bytes at 0x7123c.
- The label format string at heap+0x81c.
- The meaning of def+9 flags 0x02 / 0x10 / 0x20.
- Whether record+7 bit 0x80 survives a save or load.
- Exactly when the flags bit0 GDV mode loops.
- The 0x75ded / 0x75e0b message texts for easy-select.
- The icon source for the Adam portrait (record +0xC of the +5&0x10 record, `E/inventory.c:2218-2222`) [inferred].

---

## 10. Acceptance tests (run REMAROTH and ROTH.C side by side, same save)

1. **Pickup gate.** Stand 25 units (d² = 625 > 0x258) from an ADEMO item sprite and click: the object stays and the cursor is 0x240. Step closer and click: the object disappears, `g_inventory_count` goes up by 1, and the slot id equals the DBASE100 table index whose record+2 equals `textureIndex|source<<8`.
2. **Staff.** Pick up the Creator's Staff (index 11, render id 0x294, `BTI/DBASE100_inventory_examples.md:349-371`). Expect:
   - it is equipped in the left hand (`g_selected_item_secondary` = its slot);
   - the weapon list is rebuilt;
   - slot quantity = SetAmmoCap 6 <<8 when the recharge attr marks infinite ammo, else 6 (op 0x20 = 0x8002, `:381-382`).
   Right-click a Staff lying in the world: Adam speaks DBASE400 0xF38C, and its subtitle shows (`:375-376`). No popup.
3. **Non-pickable.** The Dodger record (+4 = 0x80, `BTI/DBASE100_inventory_examples.md:293`): `is_item_id_pickable` returns 0.
4. **Study key.** In STUDY1, click the locked door without item 16: the refusal chain runs (`/mnt/user-data/uploads/REMAROTH/ROTH_COMMANDS.md:303-316`).
   - With item 16 carried but not in hand (easy flag 0): the first click auto-selects it into the right hand and nothing else happens. The second click opens the door.
   - With the easy flag set: every click refuses until the key is selected manually.
5. **Stack.** Give a stackable item twice (DBASE op 0x11 ×2): one slot, quantity 2, label "name (2)". Then `remove_item` once: quantity 1, and the slot is kept.
6. **Full pack.** With count = 256, RAW 0x29 returns 0 and sets `g_command_chain_interrupt`=2.
7. **Combine / use.** Pick any item with +5&0x40 and an OnUse block with 0x34 = X and 0xB4 = R. Hold it and primary-click X: message 0x28 with both names; both are removed and R appears. Primary-click an item that is not in the recipe: message 0x29, and nothing changes. Use an effect-only item on itself: message 0x2d, the effect action runs, and the item remains unless the effect removes it.
8. **First examine.** Open the close-up of an item with +4&0x40 in a fresh game:
   - the OnInspect speech plays once, and record+7 bit 0x80 becomes set;
   - reopening does not replay it;
   - the magnifier button replays it;
   - the "i" button lists only topics whose conditions pass.
   Compare the caption text and voice clip with DBASE400/500.
9. **Filter.** RAW 0x42 mode 0: the list shows only category-2 entries, both hands are empty, and `query_player_inventory(heldId,0)` = 0. Mode 1: everything is restored, with the order unchanged.
10. **Save round-trip.** Save with items in both hands, reload: slot array byte-identical, both hands restored, count recomputed.
11. **New game.** The inventory equals the result of action chain 3. The Adam record is latched (0x81050 ≠ 0). Fists are active.
