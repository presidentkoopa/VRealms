# ROTH game layer: weapons, combat, monsters, projectiles, player health (porting spec)

**File abbreviations** (all under `roth_c/src/engine/` unless noted): WC=weapon_combat.c, EA=entity_ai.c, R=renderer.c, CP=collision_physics.c, PL=player.c, GC=game_core.c, DB=dbase100.c, INV=inventory.c, RC=raw_commands.c, DAS=das_assets.c, SG=savegame.c, IN=input.c. Editor files: `das.gd`, `dbase_200.gd`, `opcodes.gd` (roth-editor/src/resources/…). **Tick** = one count of the 70 Hz timer (GC:374, GC:327-341). Every per-frame system loops `g_frame_time_scale` (0x85324) times, or multiplies by it.

---

## 1. What the player experiences

Adam carries one weapon in his left hand and one selected item in his right. The first-person weapon is a DBASE200 animation that bobs while idle. Firing plays the animation, and on a set frame it launches a projectile. There is no hitscan: melee weapons fire a short-lived bullet as well. Monsters are placed in the map as art. The first time one is drawn it comes alive: it turns toward Adam and walks forward, and when it reaches him or rolls a random ranged attack, it plays an attack animation and fires its own bullet. A hit makes the monster flinch for 24 ticks. At 0 HP it plays a normal death, or a "critical" death if a single hit did at least its full HP. After about 257 ticks the corpse becomes a static prop. When Adam's health reaches 0, DBASE100 record 4 runs, a death loop of up to 300 ticks plays, and the game returns to the menu.

---

## 2. Weapons

### 2.1 Where weapon data lives
- A weapon is a DBASE100 inventory record whose category nibble `(rec[+4]>>8)&0xF == 1` (WC:1396, IN:99). The flag `rec[+4]&0x10` means the record has a WeaponAction block (INV:386, WC:351). `rec[+4]&0x20` means it has an AsBullet block (INV:153).
- The WeaponAction block (trigger code 5, `opcodes.gd` triggers 5) is parsed by `apply_weapon_action_attributes` (WC:338-417) into 0x50-byte **slots**. The equipped weapon gets up to 4 slots at `g_active_weapon_attrs` 0x811b4, one per WeaponAction block (WC:433), and the slot count is stored at 0x812f4 (WC:434). Opcode → field map (names from `opcodes.gd:21-72`):

| op | name | attr field |
|---|---|---|
| 0x12 (18) | WeaponAnimation (DBASE200 off/8) | +0x00. Also commits +0x20 ← acc(0x22), +0x1C ← acc(0x1E), +0x3C ← latch (WC:372-378) |
| 0x13 (19) | SetWeaponBullet (item id) | +0x08. Also +0x18/+0x14 ← accs (WC:379-384) |
| 0x14 (20) | SetAmmoCap (signed) | +0x04 (WC:385-388) |
| 0x18 (24) | WeaponBulletDelay (frame #) | +0x0C, default 1 (WC:345, 389) |
| 0x19 (25) | fire SFX | +0x10 (WC:390) |
| 0x1E / 0x22 (0xA2 = "X screen offset") | signed Y / X draw offset accumulators | (WC:391, 401) |
| 0x1F (31) | WeaponReloadAnimation | +0x2C. Also +0x28/+0x24 ← accs, +0x40 ← latch (WC:392-398) |
| 0x20 (32) | WeaponAmmoRecharge | +0x30: low word = ammo item id; bit15 (+0x31&0x80) = recharge weapon, low bits = rate (WC:399, 443, 503-507) |
| 0x21 (33) | reload SFX [inferred from its use at WC:863] | +0x34 |
| 0x2C (44) | WeaponAnimationSpeed | latch (default 8) → +0x3C / +0x40 (WC:362, 402) |
| 0x2D (45) | val 5 → +0x4C\|=1 (weapon tracks the aim X); val 6 → +0x4C\|=2 (fixed draw position) | (WC:403-406, PL:595, 620) |
| 0x30 (48) | FlashScreen | +0x44 (WC:407, 751-752) |
| 0x33 (51) | WeaponIconUI | +0x48 (HUD panel index, WC:408, 1015-1025) |

- Slot +0x38 = the item's own DBASE100 index (WC:347).
- **AsBullet block** (trigger 6), parsed by `init_inventory_item_object` (INV:147-190) into a 0x0C-byte descriptor:

| sub-op | name | descriptor field | copied to projectile |
|---|---|---|---|
| 0x15 (21) | MaxDamage | +6 | rec+0x16 (WC:605) |
| 0x16 (22) | TravelSpeed | +4 | rec+0x18 (WC:607) |
| 0x17 (23) | BulletHitAnimation | +0 | impact object id (WC:195-198) |
| 0x19 (25) | impact SFX | +2 | |
| 0x2E (46) | SetBulletDuration (TTL) | +5 | rec+0x19 (WC:608) |
| 0x2F (47) | collision width, default 0x10 | +8 | rec+9 (WC:606) |
| 0x30 (48) | flash | +9 | |

### 2.2 Equip rules (hands)
- **Left hand** = the equipped weapon's inventory slot, `g_selected_item_secondary` 0x81038. **Right hand** = the selected item, `g_selected_item_primary` 0x81044 (INV:1339-1340).
- `activate_weapon_item(slot, idx)` (WC:426-459) does the following:
  - `slot==0` means fists: idx = [0x81054], Adam's own DBASE100 record (WC:430).
  - It parses up to 4 slots.
  - Recharge weapons: ammo=0, cap=0x100. Others: cap=1 and ammo = the inventory count of the ammo item (WC:443-456).
  - Finally it calls `arm_weapon_and_cache_def` (WC:319-326).
- Equipping happens:
  - on pickup (`give_item`, INV:417-427);
  - with keys 2-6, which select the Nth category-1 item (`select_weapon_by_number`, IN:86-117);
  - with key 1, which gives fists (`reset_weapon_hud`, IN:162, WC:922-936);
  - automatically on running dry (`equip_first_usable_weapon`, WC:1379-1438; this needs ammo>0 and attrs+8≠0).
- A weapon that is the right-hand item is cleared from the right hand when equipped (WC:1415-1418).

### 2.3 Fire / ammo / reload: `trigger_weapon_fire` (WC:773-909)
Entry points: `key_fire_weapon` (WC:912), and a mouse click with the aim position in (EAX,EDX) (GC:684, 722).

1. Abort if the fire lock 0x7fdd0≠0, the slot count is 0, or attrs[+0]==0 (WC:781-785).
2. If a weapon item is held and cap (attrs+4)≥1, the magazine is word `slot+2` (WC:793-803):
   - step = 1, or 0x100 for recharge weapons (8.8 fixed point).
   - If the magazine holds at least one step, subtract one step.
   - **Recharge burst:** a recharge weapon keeps draining steps, and each extra step adds 1 to `g_pending_shot_scale` 0x7fe0c (WC:803-814). Later, `fire_pending_weapon_shot` multiplies the bullet's damage word by that scale (WC:755-759).
3. If the magazine is empty (WC:825-876):
   - No ammo item: with more than one slot, pick a random alternate slot (`rng_range`, WC:835-843). Otherwise show message 5 (recharge weapon) or message 6 and call `equip_first_usable_weapon` (WC:828-834).
   - With an ammo item and inventory count > 0: `remove_item(ammoId)` (WC:849), then `slot+2 += cap` when cap>1 (WC:851-856).
     - If a reload animation exists (attrs+0x2C), play it instead of the shot. No bullet: pending def=0, rate +0x40, SFX +0x34 (WC:859-866).
     - Otherwise consume one round and fire (WC:867-876).
4. Commit (WC:884-906): set fire lock = −1, call `arm_weapon_fire` (R:10961), play SFX `attrs+0x10 − 1` (WC:896-899), and latch these globals:

| global | value |
|---|---|
| 0x7fdf4 | bullet id (attrs+8) |
| 0x7fdf8 | animation (attrs+0) |
| 0x7fdfc | rate (attrs+0x3C) |
| 0x7fe10/14 | aim position |

- With cap<1, or with no weapon item (fists), the shot is free (WC:793-796).

### 2.4 Timing and animation: `update_player_tick` (PL:445-643), once per frame
- Raise/lower state 0x7fddc (states 0/1/2/3) with offset 0x7fde0 moving ±4·fts, range 0..0x64 (PL:471-480, WC:560-571).
- Lock == −1 → lock=1, accumulator 0x7fdd8 = rate+fts, cycle end 0x7fe04=0x3E8 (PL:489-494).
- Each later frame (PL:509-514):
  - `frame = accum / rate`, then `accum += fts`.
  - rate defaults to 8 when 0 (PL:484).
- **Bullet spawn:** the first time `frame ≥ attrs+0x0C` (BulletDelay), call `fire_pending_weapon_shot` once (PL:503-506, 516-519).
- **Cycle end:** when `frame ≥ word[anim+0xC]` (the DBASE200 `num_images`; written every drawn frame, PL:619), the lock is cleared and the next shot may start (PL:520-521).
  - **Rate of fire = num_images × WeaponAnimationSpeed ticks**, plus the frame of fts slack.
- **Thrown weapons:** if the bullet id equals the weapon's own index (+0x38 == +8), the held item is consumed and the player reverts to fists (PL:523-531).
- **Multi-slot weapons** re-pick the next slot at cycle end, using the sign of attrs+4 and [0x8a0f4] [open: meaning of 0x8a0f4] (PL:535-562).
- **Viewmodel:**
  - The drawn frame is the lock value (0 when idle) (PL:618-634).
  - Frames are walked by the chain `esi += [esi+0xC]` (`draw_player_viewmodel_sprite`, WC:1082-1087), using the row-RLE sub-headers in `dbase_200.gd:17-28`.
  - The idle bob is `sincos[(phase<<2)&0x1ff]>>11` (PL:566-570). This is presentation.
- **Recharge:** `tick_weapon_hud_ammo_anim` (WC:945-982) is a game rule despite its name.
  - Every 16 accumulated ticks, each recharge weapon in the list (only items whose attrs+0x30 has bit15, WC:504) whose charge `word[slot+2]>>8` is below cap gains `max(1, 0x1000/(rate·70)) / (charges+1)` (WC:957-962).

### 2.5 Projectile launch (player)
- `fire_pending_weapon_shot` (WC:747-762) calls `spawn_projectile_from_aim(bulletId, aimX, aimY)` (WC:616-675).
- Heading = player angle minus a yaw offset. The offset comes from the aim X through the arcsine table (WC:580-585, 620-634); with key-fire it is 0.
- Z velocity = `g_view_pitch`, or `g_view_pitch_applied` if that is 0 (WC:672-673), minus the aim-Y term (WC:636-648, 593).
- Spawn height = `player_z + max(0, player_height − 0x14)` (WC:664-667). Spawn XY = the player position.
- The finalize step (WC:590-610) sets:

| field | value |
|---|---|
| rec+0x1A | 0 (no owner, so the bullet can never hit the player) |
| rec+0x14 | damage type = bullet item id |
| rec+0x16 | damage |
| rec+0x18 | speed |
| rec+0x19 | TTL |
| rec+9 | width |

- `g_light_offset` 0x853f6 (sector-light boost) is raised to attrs+0x44 and decays by fts per frame (WC:751-752, PL:459-463).

---

## 3. Player health, damage, death, healing

- **Health** is `g_player_health` 0x8a0f0; **max** is 0x7fe44.
- Max health is the HP field (+8) of Adam's AsMonster def, built from [0x81054]. The default is 0x800 = 2048 (GC:766-772, GC:825-830). New game and restart set health to max and armour to 0.
- **Armour** is `g_value_reduction_factor` 0x81e30. DBASE100 op 0x37 adds a signed16 value to it (DB:781-784). It is saved in the savegame (SG:145, 180).
- Damage entry points:

| function | used by | rule |
|---|---|---|
| `apply_reduced_damage_to_player` (WC:50-61) | monster projectiles | `d -= d·armour>>11`; applied only if the result is >0. d ≥ 0x35C6 bypasses the reduction |
| `apply_damage_to_player(d, dl, falloff)` (R:9999-10026) | scripts, hazards | d=0 → d = current health (kills). d>200 → `(d−199)<<7`. dl≠0 → `d *= fts` (per-tick hazards). Falloff: `d -= d·f>>8`. **Armour does not apply.** |
| `apply_direct_damage_to_player` (WC:29-43) | fall damage (PL:286-291) | common tail. Sets the damage-this-frame flag, pain flash `+= (d+0x20)/2` (only if the flash ≤ d+0x20; clamp 0x164), `health -= d`, clamp to 0 |

- **Monster hit on the player** (EA:986-996):
  - `base = rec+0x16`, `part = (lcg16·base)>>16`, `d = (base+part)>>1`, which ranges over [0.5, 1.0)·base.
  - If d≠0, apply it through the reduced path, and play the pain sound from Adam's def (EA:992).
- **Fall damage** = `min((fallCounter−0x16)², 0x1F40)`, applied when fallCounter>0x16 (PL:286-291).
- **Pain flash** decays by 4·fts per frame (GC:334-340). This is presentation.
- **Healing:** DBASE100 op 0x26 with operand v (int16):
  - v≤0 → health=0 and `pending_game_action+0xC=1`.
  - Otherwise `health = min(health+v, max)` via `advance_clamp_8a0f0` (DB:900-907, R:9819-9825).
- **Death** (GC:959-1010):
  - `health≤0` exits the inner loop.
  - Movement mode is set to 0x20 and the weapon is unequipped (`activate_weapon_item(0,0)`).
  - DBASE100 record **4** runs.
  - A loop of up to 0x12C ticks runs the gameplay frame. It exits early on input code 1, or on F9, which quickloads the slot at [0x7138c].
  - Then the menu opens.
  - While dead: monsters stop aiming and attacking (EA:513), the weapon tick stops (PL:465), and Adam is forced to crouch (PL:218-219).

---

## 4. Monsters

### 4.1 Coming into existence
1. **First draw (the main path).**
   - A DAS FAT entry with flag_1 `'$'` (0x24 = MONSTER|DIRECTIONAL) gets status word `(flags_2<<8)|0xFC` (DAS:885-889). flags_2 is the monster-table index.
   - When the renderer rasterises such an object and its backref `obj+0xC==0`, it calls `spawn_entity_into_state_pool_a(AH=index, ESI=obj)` (R:5826-5840).
   - If the pool is full (16 actors, EA:859-866), the object is drawn with its static walking frame and the spawn is retried on the next draw (R:5844-5851).
   - A successful spawn (EA:857-892) sets:

| field | value |
|---|---|
| slot+0 | the **shared** 0x68-byte DAS monster record `[0x85cf4] + idx·0x68` (its +0 is overwritten with the def pointer, +0x60 with the def id) |
| slot+9 | def+0x69 |
| slot+0xC (HP) | def+8 |
| slot+0x16 | owning sector |
| slot+8 | 0 (so THINK starts immediately) |
| obj+0xC | backref |
| obj+9 | \|= 1 |

   - The def is loaded from **the object's own id word `obj+4` read as a DBASE100 id** (EA:868-869).
2. **Script spawn (0x3C `cmd_spawn_object_adv`)** (RC:4108-4219):
   - Creates or unhides an object from a template.
   - If the template flag 0x80 is set, it calls `spawn_object_from_das_resource(obj, tmpl, 8)` (DAS:208-249), which spawns the actor immediately, plays the spawn SFX (def+0x64), and faces the actor at the player.
   - Variant 0 (not used here, since variant=8) sinks the object 0x50 and starts the "emerge" stagger (DAS:232-237).
   - If the template flag 0x20 is set, it fires a projectile from the source instead (RC:4128-4150).
3. **0x16 `cmd_spawn_object`** (RC:622-667): toggles the object's hidden bit `obj+7^0x80`, which spawns the monster by making it visible. For a visible live actor it sets `actor[9]|=1`, which is the died-this-frame bit, so **it kills the monster** [inferred from EA:1313-1320].

### 4.2 Per-monster data
**The DAS 104-byte record** (`das.gd:180-230`; the offsets are confirmed by R:12700-12745):

| offset | content |
|---|---|
| +0x00 | u32 (runtime: def pointer) |
| +0x04 | flying[8] |
| +0x14 | walking[8] |
| +0x24 | attack1[8] |
| +0x34 | attack2[8] |
| +0x44 | on_damage[8] |
| +0x54 | dying_normal |
| +0x56 | dead_normal |
| +0x58 | dying_crit |
| +0x5A | dead_crit |
| +0x5C | spawn |
| +0x5E | rise |
| +0x60 | u32 (runtime: def id). Its high word +0x62 = special-idle frame |
| +0x64 | u32 |

- Directions are ordered back, back_right, …; the index is `((2·rot+0x120−V−off)>>5)&0xE` (R:5864-5868).

**Frame choice** (`resolve_face_surface_id`, R:12700-12745), by state:

| state | frame |
|---|---|
| pain (slot+0x21≠0) | on_damage |
| corpse 0x20 | dying_crit if bit0, else dying_normal |
| stagger 0x40 | rise if bit0, else spawn |
| attack 8 | attack2 if secondary (bit0), else attack1 |
| 0x80 | on_damage |
| ==5 | +0x62 |
| else | flying if `slot+0xA&2`, else walking |

**Stats: the AsMonster def** (trigger 0x0A), built by `build_entity_def_record` (DB:117-180) into 0x6C bytes and cached in a 10-entry LRU (EA:645-692):

| op | name | def field | used as |
|---|---|---|---|
| 0x24 (36) | SetMonsterMoveSpeed | +0x0C | move rate (EA:1290, 588) |
| 0x25 (37) | "pathing flag" | +0x0E | **max turn per tick**, in 1/65536-turn units (EA:1291, 1307) |
| 0x26 (38) | HP | +0x08 (u32) | |
| 0x27 / 0xA7 | Immunity / Vulnerable lists | count +0x2C / +0x46, ids +0x2E / +0x48 | |
| 0x28 / 0xA8 | primary / secondary bullet | +0x10 / +0x12 | |
| 0x29 / 0xA9 | roam / damage SFX lists | count +0x18 / +0x22 | |
| 0x2A / 0xAA | attack SFX | +0x14 / +0x16 | |
| 0x2B / 0xAB | attack spawn delay | +6 / +7 | |
| 0x2C | attack-rate threshold | +0x6A | |
| 0x2D | 2 → +0x69\|=2; 3 → +0x69\|=4 | +0x69 | ranged-attack mode bits, copied to slot+9 |
| 0x30 | light flash | +0x68 | |
| 0x31 / 0xB1 | death / crit-death SFX | +0x60 / +0x62 | |
| 0x32 | spawn SFX | +0x64 | |

- The list writes are not bounds-checked (DB:151-163).

### 4.3 AI tick
`tick_dynamic_entities` (EA:1348-1379) runs fts times per frame. Each iteration does `update_dynamic_entities` (projectiles), then every pool-A slot goes through `ta_slot` (EA:1311-1341). The priority order is:

1. **`slot+9&1` (died):** `fire_entity_pending_trigger` (RC:2809-2836).
   - Runs the object's own command chain if `obj+9&0x20`.
   - Then `reset_entity_state_with_sound(slot, killDmg)` (EA:80-103) sets state=0x20 and plays the death SFX, **or the crit-death SFX with state|=1 if the killing hit ≥ max HP** (EA:91-94).
2. **0x20 corpse (`ta_corpse`, EA:1092-1139):**
   - The body settles to the floor at an accelerating step.
   - Then the counter counts to 0x101 ticks.
   - Then: if dead_normal/dead_crit >0x1200, the object becomes a static prop with id `frame−0x1000` and the slot is freed. Otherwise the object is destroyed.
3. **0x40 stagger (`ta_stagger`, EA:1143-1199):** z moves ±2 per tick while `slot+0xB` is nonzero, for up to 0x101 ticks. After that it returns to THINK (state 5 if `rec+0x62` is set).
4. **8 attack-recover (`ta_attack_recover`, EA:1204-1237):**
   - When the counter reaches the attack delay, it spawns the queued bullet with `spawn_object_projectile_at_player` (WC:682-721) and raises the light by def+0x68.
   - It keeps aiming with limit 0x2710 and applying gravity until the counter passes 0x100, then returns to THINK.
   - **Attack cycle = 257 ticks** [as coded; verify in test].
5. **Pain (slot+0x21, set to 0x18 per hit, EA:1014):** aim with limit 0x3E8 plus gravity; no movement. It can still start an attack [quirk, EA:1335].
6. **THINK (EA:1281-1308):** ambient sound roll (EA:1243-1276), then `update_actor_movement_ai` (EA:574-627), then `aim_enemy_at_player(def+0xE)` (EA:490-560) unless steering-blocked (`slot+9&8`).

**Movement and perception.** There is no line-of-sight test and no pathing. Monsters always know where the player is (`atan2` to the player, EA:499-502) [inferred: aggro = "has been drawn"].
- **Contact:** `check_entity_player_contact` (R:10689-10729) tests Chebyshev distance − (0x60, or 0x30 once `slot+9&0x80`) − DAS collision radius < 0, and |dz| ≤ 0xAA.
  - In contact: stand still and set bit 0x10 (EA:580-587).
  - Otherwise: accumulator `+= def+0xC`. For every 0x10 accumulated, step 8 units along the facing (sincos amplitude 0x4000 = 1.0, table `obj3_symbols.h:173`; `<<5`, EA:588-626).
  - Moves go through `move_entity_with_collision` (EA:216-335): step height 0x80, or 0x260 when flying.
  - `collide_entity_and_steer` (EA:105-215): a barely-moving monster nudges ±3 per tick (LCG-chosen side); otherwise it turns to the wall bearing.
- **Crush:** when head clearance fails, HP drops by 10 per tick. On underflow, HP=0x2EE0 and the monster dies, giving a crit death (EA:298-308).
- **Aim and attack** (EA:490-560). A monster turns by at most `limit` per tick. When the facing error is ≤2:
  - **Contact mode (0x10):** each aligned tick adds 1 to `slot+0x20`. At def+0x6A it resets and calls `begin_enemy_attack` (WC:248-268): primary if there is no secondary, forced secondary if `slot+0xA&2`, otherwise LCG bit0.
  - **Ranged:** requires `slot+9&6`, and the LCG low byte ≤0x0C (13/256 per tick). The same counter gate applies, then `launch_enemy_attack_animation` (WC:275-300): mode 6 means 50/50, mode 2 means primary.
  - `enemy_attack_apply` (WC:216-242) sets bullet=def+0x10/0x12, delay = def+6/7 + 1, state|=8, plays the attack SFX.
- **Monster bullet** (WC:682-721):
  - Heading = the monster facing. z = obj z + 0x40.
  - Vertical aim: `zvel = (dz<<7)/horizontal distance`, applied only when |dz| is more than about 0x10.
  - Owner = rec+0x1A, the shooter's backref, so the shooter is excluded (CP:1148-1150). **Other monsters can be hit** [inferred].
- **RNGs:** there are three separate 16/32-bit LCGs, all `x·0x5E5+0x29`:

| LCG | address | used for |
|---|---|---|
| aim/steer | 0x72730 | EA:493, 605 |
| damage | 0x7272c | EA:721-726 |
| ambient | 0x72738 | EA:1250-1254 |

---

## 5. Projectiles

- **Pool:** 16 records, stride 0x1C, at 0x90fe4 (EA:700-704). `spawn_entity_at_position` (EA:808-850) returns 0 when the pool is full.
- **Per tick** (`ud_projectile`, EA:946-1050): velocity `(sincos·speed)<<3` gives **2·speed units per tick**; z velocity is `(zvel·speed)<<10`.
- **Sub-stepping** (`sweep_move_with_collision`, CP:1556-1665): the magnitude is the speed.

| speed | sub-steps | velocity per sub-step |
|---|---|---|
| <0x0C | 1 | full |
| <0x18 | 2 | halved |
| <0x30 | 4 | quartered |
| otherwise | 8 | eighthed |

  - Each sub-step probes the collision; the first hit stops the move (CP:1624-1640). Ray mode is used (see rothc_rules §8).
  - Target box radius = width + 0x20 (CP:1602).
  - **Player hit test** (CP:43-72): Chebyshev ≤ width+0x20, and `player_z ≤ bulletZ ≤ player_z+height+0xA`. It only applies when the owner ≠ 0.
- **Outcomes:**
  - **Clean flight:** TTL (when ≠0) counts down, and at 0 the bullet is destroyed (EA:964-979).
  - **Player hit:** see §3.
  - **Actor hit** (not a corpse and not already dying):

    ```
    B = compute_projectile_hit_damage   (R:10754-10779)
      = base · mult
    mult = 0 on the first Immunity match (id or −1),
           then ×2 per Vulnerable match (0→1)
    d = (B + (lcg·B>>16)) >> 1
    ```

    Then: damage SFX, pain = 0x18, `HP −= d`. If HP≤0: `slot+0xC=d`, `slot+9=1` (EA:998-1021).
  - **Object hit on a player-owned bullet:** fires the object's command chain when `obj+9&0x20` and the object has no actor (CP:1158-1162). This makes objects shootable.
  - **Wall or object hit:** marks the shooter `slot+9|=0x80` (EA:1027-1032).
  - **Expiry:** `init_projectile_from_item` (WC:175-204). If a hit-animation id exists, the object becomes that id and the record switches to mode 2 (dying). It is destroyed after 0x100 ticks (EA:1067-1075). Otherwise it is destroyed at once.

---

## 6. Commands touching combat

**RAW commands:**

| opcode | name | effect |
|---|---|---|
| 0x33 | `cmd_apply_damage` (RC:4245-4275) | byte[rec+7] damage with radius falloff `0x100·dist²/(2r)²`, from the source object or active face. Type byte 0x8a26a means ×fts |
| 0x35 | `cmd_face_emits_damage` (RC:4282-4297) | hazard faces |
| (emitter) | `damage_player_from_emitter` (WC:72-106, via RC:3298) | per-tick emitter damage ×fts, optional one-shot latch |
| 0x16 | spawn/kill | see §4.1 |
| 0x3C | spawn / projectile trap / monster spawn | see §4.1 |
| 0x29 / 0x2A | give / remove item | give can equip a weapon (INV:417-427) |

**DBASE100 runtime ops:**

| op | effect |
|---|---|
| 0x11 | give/remove (DB:767-772) |
| 0x26 | heal or kill (DB:900-907) |
| 0x37 | armour (DB:781-784) |
| 0x23 / 0x36 | run map commands |

The item blocks 5/6/0x0A define weapons, bullets and monsters (§2.1, §4.2).

---

## 7. Rules vs presentation

| Group | Functions | Port |
|---|---|---|
| Damage math | WC:29-106, R:9999-10026, R:10754-10779, R:9819 | 1:1 |
| Weapon attrs / equip / ammo / recharge | WC:338-459, 488-516, 773-909, 945-982 (logic only), 1379-1438, IN:86-117 | 1:1 |
| Fire state machine | PL:459-562, R:10961, WC:309-326 | 1:1 (the frame count comes from the DBASE200 header) |
| Projectile spawn / move / hit | WC:117-204, 590-762, EA:808-850, 946-1085, CP:1556-1665, CP:43-72 | 1:1 (see trap 1) |
| Actor pool / AI / defs | EA:80-892, 1092-1379, WC:216-300, R:10689-10746, DB:117-180 | 1:1 |
| Spawn trigger | R:5826-5840 (the decision only), DAS:208-249 | 1:1 logic, new trigger (see trap 2) |
| Frame selection | R:12700-12745, R:5864-5868 | rule-like; feeds the engine's sprite frames |
| Viewmodel draw, bob, HUD panels, ammo glyphs, pain flash palette, no-ammo text | WC:466-480, 522-553, 984-1369, PL:564-640, GC:334-340, menu_hud_ui 296-370 | replace |

---

## 8. Dependencies, size, traps, open questions

**Dependencies:**
- DBASE100 parser (trigger blocks 5/6/0x0A)
- DAS FAT flags plus the monster table
- DAS object collision table (`[0x85c50]`, R:10717)
- sector/portal collision (CP)
- the object store and per-sector object lists (`add_secondary_state_record`, `destroy_dynamic_entity` EA:739-800)
- the command runner (object chains)
- inventory, and the 70 Hz tick

**Size:** about 2,000-2,800 lines of C++ for the logic (WC logic ≈700, EA ≈1,000, collision sweep ≈250, damage/def/contact ≈250, commands ≈300). Presentation replaced: WC ≈600 and PL ≈100.

**Traps and quirks:**
1. **Coordinate packing.** Positions are split between a 16-bit integer in the object and a 16-bit fraction in the pool record. Movement is 16.16 with carries (EA:911-917). Keep integer units; do not convert to floating point before the rules have run.
2. **First-draw spawn.** GZDoom does not draw through ROTH's rasteriser. Reproduce the spawn with a "first frame this object is potentially visible" hook: sector visibility or the engine's sprite-drawn callback, plus the pool-full retry. Record the exact trigger in a test, because monsters behind geometry are not rasterised in ROTH [open: whether the spans must be visible, or only the object queued].
3. **Shared records.** The DAS record and the def are shared by all actors of one type (EA:870-872). The def cache holds 10 entries, LRU (EA:645-692).
4. **Pool limits.** 16 actors and 16 projectiles. Spawns fail silently when the pool is full.
5. **Per-frame loops.** AI runs fts times per frame with an 8-bit counter (EA:1358-1377). In the weapon cycle, fts is added once per frame (PL:513). Tie both to a fixed 70 Hz tick.
6. **Crit death.** It depends on the size of the single killing hit, not on overkill (EA:91).
7. **0x16 kills live monsters** [inferred].
8. **The player's bullets cannot hit the player.** Monster bullets hit other monsters.
9. **`num_images` is used** as the cycle end even where the editor finds it wrong (`dbase_200.gd:90-92`) [open: those weapons' real cycle].
10. **Unbounded list appends** in the def builder (DB:151-163).
11. The fists `movsx word[2]` null read returns 0 (WC:435-442).

**Open questions:**
- the meaning of [0x8a0f4] (PL:543);
- op 0x21;
- slot+0xA bit1 (flying?);
- whether the 257-tick attack cycle is visible in play;
- the enemy collision mask (rothc_rules §8).

---

## 9. Side-by-side acceptance tests

All of these run ROTH.C and REMAROTH on the same map, the same DBASE100, and fixed LCG seeds (dump 0x72730, 0x7272c and 0x72738 at map load and inject them).

1. **Def dump.** For every DBASE100 item with an AsMonster block, dump the 0x6C def (DB:117) from ROTH.C and compare byte-for-byte with the port. Do the same for the 0x50 weapon slots (WC:338) and the 0x0C bullet descriptors (INV:147).
2. **Damage per hit.** With the LCG seeded to s, fire a weapon W at monster M. Expected damage = `(B + (((s·0x5E5+0x29)&0xFFFF)·B>>16))>>1`, where B = MaxDamage(bullet) × mult(M). Check HP after each hit (slot+0xC) for 20 hits against ROTH.C. **Hits-to-kill** = the smallest n where ΣHP drops to ≤0. Run a table of every weapon × every monster in STUDY1/CHURCH1.
3. **Immunity and vulnerability.** For a monster immune to bullet X: 0 damage, no pain (pain is only set when d≠0, EA:1009-1014). For one vulnerability: B doubles; for two: ×4.
4. **Crit death.** A single hit with d ≥ def HP triggers the crit-death SFX and dying_crit frames. Otherwise the normal ones.
5. **Rate of fire.** Hold fire with weapon W. Shot spacing in ticks must equal `num_images×rate` (±1 frame of fts). The bullet spawns at frame BulletDelay: log the tick of each `spawn_entity_at_position`.
6. **Ammo.** For a cap-6 weapon with 2 ammo items: 12 shots; then message 6 and an auto-switch. For a recharge weapon, measure charge versus time against the formula in WC:957-962.
7. **Spawn.** On a map with a known `'$'` object, the actor slot count goes from 0 to 1 on the first frame the object is drawn, never earlier. With 16 actors alive, the 17th stays static.
8. **Monster attack timing.** Place Adam in contact. Ticks from alignment to `begin_enemy_attack` must equal def+0x6A. Bullet spawn = delay+1 ticks after that. Return to THINK = 257 ticks after that.
9. **Movement.** A monster with speed S moves S/2 units per tick in open space. Log its XY over 70 ticks.
10. **Pain.** After a hit: on_damage frames, no movement, for 24 ticks.
11. **Player damage.** With armour A (after op 0x37), a monster bullet of base B takes health down by `d − (d·A>>11)`. A fall with counter c takes `min((c−22)²,8000)`. Command 0x33 at distance r from the source gives the falloff formula.
12. **Death.** Health hitting 0 runs DBASE100 record 4. The death loop lasts ≤300 ticks. Monsters stop turning.
13. **Projectile sweep.** A bullet with speed 0x30 fired at a thin wall must stop in the same sub-step (8 sub-steps). Compare its final XY and sector.
14. **Savegame.** Save mid-fight and load. The pool-A and projectile pools round-trip (SG:452-496): identical HP, state, and counters.
