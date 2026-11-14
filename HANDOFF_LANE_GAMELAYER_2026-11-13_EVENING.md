# Game layer, 2026-11-13 evening: where it stands

Branch `lane/gamelayer`, GitHub presidentkoopa/VRealms. Start here, then the docs it points to.
Marks: **[V]** verified (how stated), **[R]** reasoning only.

## 1. Built and verified on the owner's PC today (engine logs + flat captures, hidden desktop)

| What | Evidence | Commit |
|---|---|---|
| Native FBX hands, authored poses, capacitive touch | `docs/images/hands/engine_poses_flat.png` | 0bd9acc0 |
| Held-object and weapon-grip poses (UZDXREMA VAP:476-656), owner's fbx_poses.json frames | `engine_holds_flat_a933c698.png` | a933c698 |
| Missing frames COPIED from the other hand; pinches (Blender 694 / 312); item in the RIGHT hand | `engine_holds2_flat_13de3c47.png` | 13de3c47 |
| Use the held item by touching (the original's click with the right-hand item; left hand = nothing selected) | `engine_useitem_flat_ad6d1ec5.png`; gameproof H 6156/6156 vs the running original | 471eb9d7..4083a3ed |
| Inventory by hand (laser + fingertip), the original's own click code | `engine_invhand2_flat_a80512e5.png`; invdraw_test 1422 points identical | 01f9e474..02b1ad96 |
| **The owner's button scheme**: trigger = examine (primary), grip = use (secondary), world + inventory; dwell removed | logs of all three cfgs; inventory trigger fixed in dc4b8746 | 983ef7b8, 47adae3b |

Regression suite on all of it: gameproof (modes 0 1 2 4 7), dialogue, cmdcore (STUDY1, CHURCH1), actors oracle
(STUDY1), gameflow 80/0, menus 48/0, wallrule 0 differ, vrpick 786,432/0, hands, fbxload, invhand 117/0 [V, run here].
Not seen: anything in the headset.

## 2. Owner's decisions today (do not relitigate)

- Hands: FBX only, two separate hands, the animation's own frames. Copy a frame from the other hand only where a hand
  lacks it. Pinches exist (694, 312). Don't ask the owner which frame: walk the take and look
  (`tools/fbxload_test/walk_sheet.py`).
- The original's hands: item in the RIGHT hand. The weapon was left where it is (main hand) by the owner's
  instruction; in the original it is the LEFT hand. Not moved.
- Buttons: trigger = examine, grip = use (pointing = at range, in reach = grab). A weapon hand's trigger fires.
  The 0.8 s dwell is dropped.
- Candles: light the wicks in any order; one growing flickering light per candelabrum; darkness that each candle
  pushes back (RS_Radiance's darkness, already in this engine). Design: `docs/WORLD_DESIGN.md` 2.3.
- Drawers/cabinets that open by hand: yes, in the original's style, where the original opens them.
- Item 3D models (meshes from the close-up films, 4x upscale): parked by the owner ("sprites will work for now").
  Scripts in `/home/claude/wt_voxel/tools/closeup_mesh/` (uncommitted worktree); the six test meshes were sent to him.
- Chapter-by-chapter design discussion next, with the research below in hand.

## 3. Research done today (in the repo)

- `docs/CHAPTERS.md` + `tools/chapter_census`: the original's 20 chapters, per-map gates, items, puzzle kinds, and
  everything timing- or order-sensitive VR must not change (delays, XOR bit puzzles, last-item tests, counters, timers,
  random draws, the 8-deep action queue).
- `docs/OPENABLES.md` + `tools/openables`: 9 things open on screen (8 hinged furniture leaves + the STUDY1
  sarcophagus lid); no drawer opens anywhere; the STUDY1 roll-top desk and the CHURCH1 desk NEVER open [V].
- `tools/wickspots` (+ OPENABLES 6): flame positions measured from unlit vs lit candle art; the lit art also recolours
  candle tops and cup rims (per-flame compositing must carry them).
- `docs/VR_INTERACTION_RESEARCH.md`: how VR adventure/horror games and VR ports handle each interaction, top 15.

## 4. Questions for the owner (1-4 answered the same evening; 5-6 open)

1. ANSWERED. The trigger examines on either hand and never sends attack unless that hand holds a drawn weapon; a
   weapon hand's trigger keeps firing; a grip on nothing is the original's secondary click (the attack, from
   E/game_core.c:651-731). The owner: "we will probably use offhand for a lot of environmental interaction, but we
   need to design with left and right hand support in mind." Done in `roth_handclick.h` (oracle test
   `tools/handclick_test`, 140 cases, 0 differ) and `roth_vrpick.cpp` VRPickTick (the fire button held back with
   no weapon in the main hand).
2. ANSWERED: pause like the original. While the inventory or the examine popup is open the world stops (monsters,
   the player's weapon, the playsim), as the original's mode 3 (E/game_core.c:862-921); the hands still work the
   panel. Done in `roth_runtime.cpp` TickLevelLogic (checklist there); log lines "the inventory pauses the world" /
   "the world resumes ... (0 ticked while paused)".
3. ANSWERED: match the original. A click during a playing dialogue line skips the line, for mouse, keys and both
   hands (E/input.c:1271-1279, :1319-1331; dialogue_ui.c:552-553). Done in `roth_invmouse.h` (items 3, 8, 12, 15,
   24); oracle test `tools/invdraw_test/scripts/busy.txt`.
4. ANSWERED (owner): unlit wicks catch by themselves 15 s after the first wick.
5. Opening the never-opening desks (STUDY1, CHURCH1) would be new content, not a VR presentation of the original.
6. VR_INTERACTION_RESEARCH "Phase B" ideas (sockets, drawers, books, torch beam, lamp light...) need the owner's go.

## 4b. Chapter 1 answers (docs/VR_CHAPTER01_SHADOWS.md section 6)

Strike a real match (off hand, 20 s); the Lamp on a hip hook, grabbed when needed; a physical attempt plays the
original's examine line, as a pattern; the Book opens in both hands, the key drops into the palm.

## 5. Next

- The darkness pass over `roth_palette_shading 1`: captures lit and dark before any tuning.
- Candles: per-wick compositing (with the rim texels), the growing light, the darkness hookup.
- Chapter 1 (the house, STUDY1: 85 gated steps, 24 items) design discussion.

## Late evening: chapter 1 second pass (owner asked before chapter 2)
- `docs/VR_CHAPTER01_SHADOWS.md` sections 8 (what the first pass missed: movement, the Colt, lines, HUD, death,
  saving, hidden markers) and 9 (changes made for this chapter); 8.7 holds the owner's answers.
- `docs/VR_CHANGES_LEDGER.md`: every change with its decision, commits, evidence and the chapters it reaches. Every
  new chapter dossier gets a "Changes made for this chapter" section pointing here.
- Owner: the original's movement ported 1:1 NOW ("we move too slow"); smooth + teleport setting; snap 45; a real crouch
  drives the original's crouch; guns aim along the gun; real-parts reload LATER (owner will find meshes); an off-hand
  mount for subtitles and health (+ a low-health cue); world-locked movies; a quicksave button; an item brought to the
  eyes = examine; REMASelacoVR's on-gun ammo display LATER.
- Agents running: movement 1:1 (`/home/claude/wt_move`), off-hand mount + aim along the gun + quicksave
  (`/home/claude/wt_mount`), plus surface boxes, gestures and held objects. Still to start after held objects:
  world-locked movies, item to the eyes. Chapter 2 dossier (`/home/claude/wt_ch02`) resumes after these land.
