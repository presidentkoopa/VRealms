# HANDOFF: REMAROTH game layer, 2026-11-12

Read this first. It replaces `HANDOFF_LANE_GAMELAYER_2026-11-11.md`. Older handoffs and rendering notes are history; where they disagree with this file or with ROTH.C, they are wrong.

Branch: `lane/gamelayer` on `presidentkoopa/VRealms`. Owner's local copy: `E:\DOOMWork\REMAROTH`. The owner's build is kept at the branch tip through the bridge.

## 0. Evening update (2026-11-12, after the pause) -- read this first

**Verified on the owner's PC (bridge runs n2-p1, w1-w2):**
- `rothdiff` holds its end-of-batch quit while `roth_at` commands are pending (`FinishBatch`, roth_diff.cpp). `rothdiff_shot ... <pitch> - <settle>`: `-` keeps the default storey so a test can set a short settle.
- **Saving from a Realms map crashed**: the map is read from ROTH.RES, `lumpnum` -1, null "Map WAD" name -> strlen(null) (g_game.cpp PutSaveWads). Fixed.
- **Quicksave crashed**: `FBillboard::attachedTo` was uninitialised (TObjPtr has no constructor) and the save writes it for every billboard. Fixed in g_levellocals.h. The serializer now names a dead object pointer's field and owner (serializer.cpp) instead of crashing in RecurseWriteFields. GC also marks attached billboards' actors (g_level.cpp).
- **Loading** a quicksave works (map restored, props found again). Interface billboards are `BBFL_NOSAVE` and are not written into saves; before that a load brought back a dead "Save a game" panel.
- **Weapons merged** (commit 1664e8c1). All 15: 0 weapon, 0 inventory, 0 tick differences. The earlier differences were the probe: SIGALRM re-entered its log writes (actprobe.c now blocks it). Suite on the merged tree: gameproof 694/694 (0 1 2 4 7), dialogue identical, cmdcore STUDY1/CHURCH1/CAVERNS identical, gameflow 80/80, menus 38/38. On the PC: ZScript compiles, `RealmsWeapon9` raises and fires on the trigger.
- **JFO font** (JDUI by Zedas, Blackmore1014 edit; the owner's JDUI_Ultimate.zip) is the Realms text atlas: `vrealms_iwad/sdffonts/sdfmono.*`, made by `tools/sdffont/mksdf_bitmap.py`. Capitals only. Loaded, 0 missing glyphs.
- Subtitle line re-wrapped to 170 font px and drawn 1.6x.

**Every UI picture is now the original's, drawn by its own routines from the install, pixel-tested against ROTH.C and seen on the owner's PC:** inventory (e3268c1a; 2,142,720 px 0 differ), menus/choices/subtitle/timed message (01b5a239; 21,732,676 px 0), death wait as the original, no box (7acff5ed), HUD (6b4b4275; 119,808,000 px 0; flat placement by the renderer's own projection terms, 5c1e211e), examine popup (a1abc8c7; 2,041,170 px 0). Shared primitives: roth_uidraw. Tests: tools/{invdraw,menudraw,huddraw,popdraw}_test.
**Open (1:1 flat):** in flat the original draws ALL its panels on its screen and is driven by a mouse pointer. Ours still places the inventory, menus and choices in the world (picked by the view's centre) -- a VR presentation. Flat should put them at the original's screen positions (as AddHeadPicture now does for the HUD/subtitle) with the original's pointer; that also makes the HUD corner peek reachable. Not started. The inventory, the menus (options, save slots), the dialogue choice box and the subtitle backing are our own billboard layouts, not the original's. New CLAUDE.md rule: draw them from the install's own pictures, placed by the original's drawing code, tested pixel for pixel. An agent is doing the inventory in worktree `/home/claude/wt_invdraw` (engine-free `roth_invdraw` into an 8-bit buffer, pixel differential against ROTH.C, one textured billboard in the engine). Then the same for: the menus (`menu_hud_ui.c`, show_message_box), the dialogue panel (`dialogue_ui.c`), and the HUD bars at the top (not drawn at all now).

**Seen but not resolved (reasoning only):**
- Weapon 9 beside the original's frames: the ghoul's pose differs by about a frame. Its logic state matches at that tick (engine dump tick 142 vs original tick 140: st 19, hp 500, same position), so this looks like capture timing; not proven.
- The original's firing frame shows a brighter room, but its camera had moved (knockback) and ours is pinned by the test. Not a fair comparison; not investigated further.
- CORRECTION: quicksave/quickload ARE bound in game: FlowResponder maps F9/F10 from the original's keymap (0x7093d, input.c:545-559). An earlier note said otherwise without checking the code.
- Stock Doom menu graphics (M_DOOM...) give menudef script errors at startup; harmless, not ours.

**FBX hands:** report at `docs/FBX_HANDS_REPORT.md` (six questions for the owner at its end). Reader: `tools/fbx_hands/`.
- **Owner's answers (2026-11-12, then hands PARKED by the owner):** hand length 0.19 m wrist to middle fingertip; the left hand uses the FBX's own left-hand poses (not a mirror); the skin texture `lambert1_Base_Color.png`, with a planned player choice of skin tones (natural and unusual: blue, red, pink).
- **Gestures (checked, not yet decided):** the FBX has no point/pinch/thumbs-up; its one take is the 1,278 frames RS_Hands calls its source animation (`E:\DOOMWork\_old\RS_Hands`, frames 11-1288). RS_Hands made its 11 baked poses (open, point, trigger, fist, pinch, thumbout, gripfire, grip_tu, ready_td/tu, fire_tu) from five per-finger curl angles each (its README table; pinch solved geometrically) and appended 9 hold poses (iqm_append_pose.py); see `docs/images/poses/wired_poses_sheet.png` there. Proposed and not answered: rebuild those 23 on the FBX rig from that recipe (not from the IQM's data). Still unasked: engine path (build-time FBX->IQM vs ufbx), wrist animation.

**Tools added:** `tools/crashscan/stackscan.py` (minidump stack -> return addresses; `llvm-symbolizer --obj=doomxr.exe` with the exe and pdb staged from the PC gives the lines). Crash reports land in `build-dxr/RelWithDebInfo/crash_<time>/`.

## 1. How this lane works now (keep it this way)

### The two engines
- **The original (ROTH.C) runs headless:**
  - in the cloud workspace: `/home/claude/oracle_build/oracle_shot.sh MAP x y angle512 out.png`, about 4 s a frame;
  - on the owner's PC: bridge action `oracle`, from `E:\DOOMWork\_oracle`.
  - Mods for the original (probes) call its own functions and dump its state.
- **Ours** builds and runs on the owner's PC through the REMAROTH bridge (`tools/claude_bridge/CLAUDE_BRIDGE_REMAROTH.ps1`, installed at `E:\DOOMWork\tools\claude_bridge_remaroth\`).
  - Runs happen on a hidden Windows desktop, so nothing appears on the owner's screen.
  - `-Map NONE` launches with no map. `-Minimized` and `-Foreground` keep the old window modes.
  - `rothdiff_shot x y angle512 file [pitch] [z] [settle tics]` takes screenshots.
  - `roth_at <tic> <command>` runs any console command at a given level tic. Use it in test configs: a `+exec` config runs before the map exists.
  - Fatal errors are now written to `doomxr-log.txt` and flushed.

### The rule
Game logic is ported 1:1 from ROTH.C and proven by a **differential test**: the same inputs go through the original (a probe mod) and through our engine-free port, and the traces are diffed. "It compiles" or "it reads right" is not done. Presentation may be better than the original but must show the same things. Every visual fix is judged by a side-by-side against the original's frame.

### The regression suite
Every agent runs these before handing back:

| Tool | What it checks | State at 6e622c94 |
|---|---|---|
| `tools/gameproof/run.sh 0` (and modes 1 2 4 7) | DBASE100 interpreter, inventory, RAW item commands vs the original | 694/694 actions, all sections 0 mismatches |
| `tools/dialogue_test/run.sh` | Dialogue window, choice menus, inventory screen, icons vs the original | 6/6 IDENTICAL |
| `tools/cmdcore_test/run_diff.sh <maps>` | RAW counters, loops, texture commands, damage, slow, rotation, particles | 44/44 maps IDENTICAL |
| `tools/actors_test/oracle/diffcmds.sh MAP` plus tick replays | Actors: 0x16/0x3c/0x3a, creature AI, damage | 0 command diffs (one known duplicated dump in the original's STUDY1 log) |
| `tools/gameflow_test/build.sh` (with `ROTH_DATA` set) | New game, travel, saves | 80/80 |
| `tools/menus_test/build.sh` | Menus | 38/38 |
| `tools/gdv_test/` | GDV movies vs the original's decoder | 7882/7882 records, every audio sample |
| `tools/wallrule/` | Wall texture rule | 0 of 38,034 pieces differ |

### The porting rule (from UZDXREMA's Quake VR postmortem, for every source we port: ROTH.C, Eternity, Quake VR, RSQ DistanceGrab, 3D Platform)
1. Port from the source, never from a summary, a header comment, a research note or an old handoff. Read the whole implementation first.
2. Before porting, write a numbered checklist of every part, constant and behaviour of the source, each with its file:line, and for each the port's file:line or "not ported" with the reason. That list is the acceptance test.
3. Never label partial work with the source's name. Say which parts are not ported.
4. When a ported system misbehaves, check the port against the source before inventing a fix.

For ROTH.C this lane adds a fifth rule: prove the port with a differential test against the running original.

### Traps that cost hours today
- **Never use `git stash`.** All worktrees share one stash list, and two agents popped each other's work.
- **The anonymous namespace.** `roth_runtime.cpp` opens one near the top. A function declared inside it and defined outside it type-checks but fails to link with MSVC (LNK2019). This happened three times today. A failed link leaves the owner without a working exe.
- **`FTextureID tex;` is uninitialised.** That one line crashed every build of the wall work. Write `FTextureID tex = FNullTextureID();`.
- **Archived settings.** Never write them from the engine: the owner's `doomxr.ini` is shared with the hands build. The loader was writing `fov` and `r_skyband`; fixed.
- **Test config settings.** The capture ini had `roth_lighting=false` and `roth_palette_shading=false` saved in it. Test configs now set both to 1 and `roth_movies 0`.
- **Movies.** The boot's logo and INTRO are skipped by default (`roth_intro_movies 0`) until a release build, at the owner's request. Test runs use `roth_movies 0`. INTRO is 8 minutes long: never let a test sit through it.

## 2. Accomplished this session (all merged, built on the owner's PC)

### Proven identical to the original
- **DBASE100 interpreter, flag bitmap, inventory, RAW 0x27/0x29/0x2a/0x2b/0x42** (`roth_dbase100_interp`, `roth_inventory`, `roth_itemcmds`, `tools/gameproof`). Before the fix, 0 of 694 actions matched: `g_choice_selected_index` started at 0, and the original starts it at -1.
- **Dialogue and inventory screen** (`roth_dialogue`, `roth_invscreen`, `roth_icons`, `roth_interface` panels).
- **RAW command core** (`roth_cmdcore`): 0x15/0x1e/0x1f/0x20/0x22/0x2e/0x1c/0x14/0x21/0x33/0x35/0x3f/0x41/0x2d. Also 4 fixes to opcodes that were already marked done.
- **Game flow** (`roth_gameflow`): boot, new game, map travel and arrival, per-map memory, saves (engine savegames carrying the Realms state; an original SAVE0.SAV imports with 0 differences), death, end of game.
- **Menus** (`roth_menus`): main menu, in-game options, 9 slots, F9/F10, death box. The texts are the original's.
- **Actors** (`roth_actors*`): object store, 0x16/0x3a/0x3c, creature AI tick, player damage and fall, the original's wake rule (its renderer walk, once per tic), creature art by state, view and frame. Weapon records are read and equip/unequip work; firing is not done.
- **Sound** (`roth_sfx`, `roth_sound`): effect bank, 104 sound nodes per map, chain/door/switch sounds, music (HMP via ZMusic), speech (DBASE500).
- **GDV movies** (`roth_gdv`, `roth_movie`): every coder, DAC rules, DPCM audio and subtitles, identical to the original's decoder.

### Rendering fixes, each compared with the original's frames
- **Holes:** linked portals adapted from Eternity (with credit), on by default, plus the original's load-time 0x07 height pass. Sectors short of floor or ceiling over 44 maps: 54 → 0.
- **Doors:** textured, at their own height, hinge and swing from the data.
- **Walls:** ROTH.C's texture rule.
- **Props and trees:** fixed.
- **Lighting:** the original's model, including a view-angle term nobody had found before.
- **Mirrors:** the reflection works.

### Seen working in the engine on the owner's PC
- Launching with no map: boot, then (now) the main menu.
- Sound and music play.
- Dialogue 343 and its four choices.
- Inventory selection.
- Mirror smash.
- Map travel by chain 414.
- The ghoul's whole fight, matching the original's sequence.
- No wake at the start position.
- The main menu panel draws in the world.

## 3. Where it stopped

- **Owner's PC:** built and working at `6e622c94` (exit 0); the owner's exe is good.
- **Panel test, last run:** `tools/claude_bridge/cfg/ui_ingame.cfg` with `roth_at`.
  - The **inventory panel renders correctly**: tabs, the original's item icons, the held item highlighted, hand boxes, scroll arrows (`logs\ui_inventory.png`).
  - The run then **quit after the first screenshot**: `rothdiff_shot` quits the game as soon as its own capture queue is empty ("rothdiff: all captures done; quitting"), so later `roth_at` steps never ran.
  - **First fix next session:** make rothdiff not quit while `roth_at` entries are pending (or add `roth_at <tic> quit` as the explicit end). Then re-run the config to see the dialogue line, choices, options and save-slot panels, and to see save/quicksave work.
- **Panels:**
  - The main menu panel draws, but its red text is small and soft at test resolution.
  - The dialogue line draws as one long thin strip across the room, which is not readable. It needs layout work: wrap to the panel width, at a readable size and distance.
  - The options and save-slot panels and the choice menu have not been seen yet (see the early quit above).
- **Weapons: done by the agent, NOT merged.** The patch is `patches/actors3_weapons_UNMERGED.patch` (a `git diff` against `5924e859`; it applied cleanly there).
  - **Logic, proven against the original:** the fire state machine (WC:747-912, 773-910), bursts, ammo use and reload, recharging weapons, out-of-ammo, "equip the first usable weapon", unequip to Adam's item (0x81054). All 15 weapons, 25 pulls each at the STUDY1 ghoul: 107,780 weapon states with 0 differences, and 15 x 456 ticks of actors, bullets, damage and kills with 0 differences.
  - **Presentation, the owner's way:** world actors `RealmsHeld` / `RealmsWeapon<item>` (`realmsheld.zs`). They publish state, frame, progress, charge and event tics. Two mesh mappings are documented in `docs/REALMS_WEAPONS.md` with an example MODELDEF. VR: `FollowHandMode = 1`, the main hand. Fallback: the original's DBASE200 picture on a card.
  - **Why it is not merged:** its ZScript has never been compiled, and a ZScript error stops the engine at startup.
  - **To land it:**
    1. Apply it, build through the bridge, and read `doomxr-log.txt` for ZScript errors.
    2. Run `tools/claude_bridge/cfg/actors_weapon9.cfg` and `actors_weapon12.cfg`, and compare with the original's frames. Those frames are in the cloud workspace (`/home/claude/evidence/actors3/`), which may be gone; re-render them with `oracle_shot.sh` if so.
    3. Re-run the whole regression suite.
  - **Known risk:** the VR card's orientation in the hand frame is assumed. If the card shows edge-on, that is where to fix it.
  - **Not ported:** `build_weapon_billboard_record`. It is the player's own body as a world object (seen in mirrors), not the held-item view.
- **Parked by the owner (cosmetic, later):**
  - outdoor trees too dark;
  - Adam not in the mirror;
  - the pentagram door at the end of the chandelier hall (black in ours);
  - the dialogue line's glyph font;
  - movie fade in/out;
  - the challex-style "modern" lighting layer. Its seam is `R_RothShade` in `main.fp`, one entry point.
- **Census:** unchanged at 52.4% on purpose. It flips after the owner has seen things work. Candidates:
  - 0x27 0x29 0x2a 0x42 (proven);
  - 0x15 0x1e 0x1f 0x20 0x22 0x2e 0x33 0x35 0x3f 0x41 0x2d 0x1c;
  - 0x16 0x3a 0x3c;
  - 0x2b once dialogue is seen in the headset;
  - DBASE100 op 0x07 (movies).

## 4. Plan forward, in order

1. **Read the queued UI test and fix what the shots show.** The dialogue line layout comes first; it is gameplay, because you cannot read the game otherwise. Make every panel readable in VR and flat.
2. **Land weapons:** `patches/actors3_weapons_UNMERGED.patch`, as described in section 3.
3. **Hands and interaction (owner, 2026-11-12):**
   - **Use the owner's highly animated FBX hands for now.** They are in the private repo `presidentkoopa/DXR` (archived) under `models/hands`.
     - **TAKE THE FBX HANDS. DO NOT TAKE THE EXISTING IQM HANDS.** If the engine needs another format, re-export from the FBX; never use the existing IQMs.
   - **One agent on the FBX hands first, to report:**
     - their rig (bones, naming, finger chains, scale, up axis);
     - every animation clip and what it is for, plus clip lengths and frame rates;
     - blend shapes, if any;
     - how to get the most responsiveness out of them in VR: which clips to drive from controller input (grip, trigger, thumb touch, finger curl from capacitive sensors), what to blend versus pose procedurally, and what latency the pipeline adds.
   - **Hand posing on meshes: Quake VR's grasp.** Read `docs/QUAKEVR_POSTMORTEM_from_UZDXREMA_2026-11-12.md` first.
     - **Correction to an earlier version of this handoff.** It said "UZDXREMA already ported Quake VR's auto-grasp". That is wrong. The `vr_grasp.{h,cpp}` this session read at UZDXREMA `39e4dad` contains **only part 1 of 8** (close from open, stop on contact). It was written from a header-comment summary and labelled as the whole thing.
     - **Parts 2–8 are being ported in UZDXREMA now**, against a written checklist (`QUAKEVR_GRASP_SPEC.md` there): conservative advancement, close from the fist, settle, thumb turn search, Quake VR's own thumb motion, finger spheres fitted to the skin, index/palm/grip/trigger and the warm start.
     - **For our FBX hands:** take the grasp from UZDXREMA only once its checklist shows all 8 parts ported and seen working in the headset. Otherwise port from Quake VR's source itself (vittorioromeo/quakevr: `Quake/vr/vr_grasp.cpp`, `vr_handrig.cpp`, the rig data `vr_handrig_data.inc`, `make_hand_rig.py`), reading the whole implementation first.
     - **Supporting UZDXREMA files:**
       - `src/r_data/vr_avatar_pose.cpp`: hand pose, `[FBXGRIP]` thumb and support-hand rules;
       - `wadsrc/static/vrhandpose.txt`, `VRHANDPOSE` lumps;
       - `tools/avatar/mkfbxposes.py`, which writes hand poses from the FBX hands.
       - Read `HANDOFF_AVATAR_2026-11-11.md`, `AVATAR_IK_WORKLIST.md` and any newer avatar handoff there.
     - Finger spheres must be fitted to the **FBX hands' own skin** (Quake VR's `make_hand_rig.py` method), not to another hand's.
     - The owner wants this for our FBX hands. Bring it into REMAROTH, or do the engine move in step 5 first and use it in place. Decide with the owner.
   - **FBX in the engine.** Neither engine reads FBX today. Both load IQM, MD3, OBJ and the like; UZDXREMA converts FBX hand poses offline with `mkfbxposes.py`. Making the engine read FBX is not unreasonable. The well-trodden way is to embed **ufbx** (single-file C library, MIT licence) as a new model loader beside the IQM one: skeleton, skin weights, animation curves sampled into the engine's bone-animation path. The alternative is an offline FBX→IQM conversion of the FBX hands, which still keeps the FBX as the source. Recommend: the ufbx loader. Then the owner's FBX files load directly and re-exports are not needed.
   - **Interaction: a quick port of the owner's RSQ DistanceGrab** (`presidentkoopa/RSQ_UBQSeries`, `DistanceGrab/`): policy, mass, lock-on, flight, hold, throw, door, feel.
     - UZDXREMA's avatar handoff decided (2026-11-12) to rebuild it as one system on the engine, without its QZD workarounds (`handsync.zs`, KEYCONF netevents, cvars read in play). Follow that decision.
     - For Realms it must go through the game layer. A grab of a Realms item is the original's pickup (`roth_interface` pickup path, `roth_inventory`). Using an item on the world is the original's use/examine path. Doors open through the original's door logic. The hands are a new way to drive the same proven rules, never a second set of rules.
4. **Weapons as meshes:** once the hands exist, the weapon actor from step 2 attaches to the posed hand, and the grasp code closes the fingers on the weapon mesh, as UZDXREMA does for guns.
5. **Move the Realms code onto UZDXREMA** (`main`): map loader, `src/roth/*`, portals, rendering changes. That brings the body IK, holsters, rig tools (`tools/linux_rig`, hand replay `p_vrreplay`) and netplay with it. Then rig-driven VR testing of hands and controllers becomes possible: today it is not, because REMAROTH's `usercmd_t` has no hand fields. The tools are copied in `tools/rema_rig`; the engine side is not.
6. Parked cosmetics, when the owner says so.

## 5. Where things are

- **Reports for the owner:** `C:\Users\Command\Desktop\VRealms reports\`, files 00–09; comparison pictures are in `compare\`.
- **Cloud workspace** (gone when the session ends; everything that matters is in the repo):
  - `/home/claude/oracle_build`: the original, headless;
  - `/home/claude/oracle_frames`: reference frames and README;
  - `/home/claude/evidence`: logs and patches;
  - `/home/claude/wt_*`: agent worktrees.
- **Retail files** staged in the workspace (not in the repo): DATA (DBASE100–500, M/, DATA/DATA), ROTH/ (CONFIG.INI, MIDI, DATA), and 8 GDV movies.
