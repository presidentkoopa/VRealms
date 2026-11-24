# Handoff: lane/gamelayer, 2026-11-24 (VR physics milestone 1, 3D items)

Read CLAUDE.md first (its owner's rules), then `docs/VR_PHYSICS.md`, `docs/VR_INTERACTION.md`, `docs/IMG3D_PIPELINE.md`.
Repo `presidentkoopa/VRealms`, branch `lane/gamelayer`, tip `3fbc4a80` when written. The owner's PC copy:
`E:\DOOMWork\REMAROTH` (built and in step with GitHub at this tip).

## 1. Where the owner stands (his words, 2026-11-23/24)

- "our 1:1 rebuild has been satisfied, and now we are in the fun zone"; "realms of the haunting reimagined to take
  advantage of full 6dof for a shooter / adventure game hybrid"; "object / world / hand / player interaction is the
  single most important thing for this project"; "you're welcome to do anything you want in order to achieve this goal".
- The original's game logic still runs underneath (pickups, keys, chains, reach): VR changes how the hands do it.
- He wants milestones reported, not step by step. Rig checks (the bridge, builds, runs) cost his account: do them at
  milestones, or ask first.
- He is not technical: design from the game's internals, explain results in plain words, don't quiz him.
- Asset generation (3D models) is last; the systems do not depend on it. He will green-light a generation run when
  you say what it is for.
- He encourages searching GitHub widely for references, beyond the links he sends.
- Hand finger posing: later ("we can pose later").
- Never comment on his hours, schedule or life. Never use screen control on his PC.

## 2. What was built since the last handoff (newest first)

### VR physics, milestone 1 (seen on the PC): docs/VR_PHYSICS.md
- **Jolt Physics v5.3.0** (MIT) vendored in `libraries/JoltPhysics` (`Jolt/` + LICENSE, our own CMakeLists: single
  precision, no fast math, `JPH_NO_FORCE_INLINE` public). Linked into doomxr via `PROJECT_LIBRARIES`.
- `src/roth/roth_physics_core.{h,cpp}`: engine-free wrapper (metres, map axes). Static level triangles by key, loose
  item bodies (convex hull), hand bodies driven by velocity to the tracked pose (after Godot XR Tools' collision hand
  and MinimalHandPhysics, both MIT), held item = part of the hand's compound, contact hit speeds, `ModelAngles` (GZDoom
  model yaw/pitch/roll from a quaternion), ghost layer, ray and shape queries. Test: `tools/physics_test/build.sh`
  (24/24; builds Jolt with g++ once into /tmp/physics_test).
- `src/roth/roth_physics.{h,cpp}`: the engine side. Every sector's flats (GL subsectors, fanned) and solid wall parts
  (one-sided walls, steps, lintels, closed doors) as static triangles, rebuilt when a sector's planes move. Hands:
  drawn where they stop (`VRHandPose_SetBlocked` in `src/r_data/vr_handpose.{h,cpp}`, a world correction in front of
  the tracked frame), only when touching something; a buzz on hits over 0.35 m/s; never teleported through the level
  (unless 1.5 m off); a reach under 2.5 m/s passes into loose items (only a swipe knocks them); stuck 25 cm off for
  half a second: slips through (ghost) until it fits again. Loose items: let go anywhere but over the shoulder (behind
  the face, above the chest) = dropped/thrown with the hand's 3-tic velocity plus the wrist's spin; a grace of 9 steps
  so the leaving palm does not knock it; drawn by `Img3dLooseShow` (KIND_LOOSE: the world mesh with
  `MDL_USEACTORPITCH | MDL_USEACTORROLL`); back in the pack at 10 m, out of the level, used, selected from the
  inventory, or the level's end; 16 at most. A free palm squeezing within 8 cm takes one where it touched it (the
  held model's seat, `Img3dHeldRel`).
- The game's side (`roth_vrpick.cpp`): `Stow()` first asks `PhysicsLetGo`; a drop is put away by the same original
  calls as a stow (`PickUpToCursor` + `DropHeld`: carried, not selected). `TakeLoose()` in `GripUse` selects the item
  by the original's calls (`PickUpToCursor` + `HeldToRightHand`). One item in the hands at a time (the original selects
  one): a second hand reaching for a loose item while the other holds one is refused.
- Hooks: `PhysicsTick` before `VRPickTick` in `roth_runtime.cpp` (world false while a menu or the inventory holds the
  world); `PhysicsEndLevel` in `VRPickEndLevel`. Cvars `roth_physics` (default 1), `roth_physics_debug`; ccmd
  `roth_physics_status`; `roth_xrpose handat|stand ... loose` targets the first loose item.
- PC evidence: `log-physics_flat.txt` + `logs/phys6_*.png` (wall stop at 945 vs the face at 948; the Helm dropped,
  resting, taken, thrown 3.9 m/s, landed, stowed); `log-interact_flat.txt` (P1 regression passes).

### The pickup flies as the item's 3D model; the stow over the shoulder (3b)
- `roth_img3d` THE FLIGHTS (`Img3dFlyShow`, two flights), `roth_interface.cpp` TickFly/TickStow: the original's 0x1c
  ticks and fractions, from the size and facing it lay at to its size in the hand; items without a model fly their
  picture, with the inventory icon as the last fallback (`HeldIconTexture`). The hand shows the item only once it
  lands (`roth_held.cpp`, `InterfaceFlyingTo`). Stow target: over the hand's shoulder, 0.35 s.

### 3D items: glass, glow, cut gems; models of several parts; real sizes
- `tools/img3d/film_build.py`: models from the item's own close-up film (DBASE300, 20 frames a full turn): a visual
  hull carved from the outlines, parts sorted by pixel colour (glass, liquid, cork), liquids filled flat to their level,
  gems flattened to facets, a tumbling crystal as a measured prism, a still outline as an n-sided cut gem. Items 35, 37,
  103, 143, 206, 249. `glbio.write_glb_parts`.
- Engine: `roth_img3d_core` reads each primitive's material as a part (colour, BLEND, emissive); the wearer draws the
  first plain part, companion actors (`PartsTick`, special1 0x1A3D) draw the rest with their own look (translucent,
  fullbright). `img3d_test` 100/0.
- `items.json` sizes by judgement for small items; `post.py --resize`. The 28 chapter 1-2 models are on the owner's
  `D:\VRealms_img3d\items` (nothing derived from retail art is in the repo).

## 3. How to test

- Engine-free: `tools/physics_test/build.sh`, `tools/img3d_test/build.sh`, and the suites CLAUDE.md lists.
- Syntax: `tools/roth_syntaxcheck.sh <file>` (now with the Jolt include and define).
- The owner's PC through the REMAROTH bridge (`E:\DOOMWork\tools\claude_bridge_remaroth`, requests `.req` one argument
  per line; names must sort after every existing response, the last used is `zzzzl19`): `git pull --ff-only origin
  lane/gamelayer`, `build`, then `run -Map STUDY1 -Exec tools/claude_bridge/cfg/<cfg> -TimeoutSec 300`. Configs:
  `physics_flat.cfg`, `interact_flat.cfg`, `fly_flat.cfg`, `img3d_parts_flat.cfg`. Logs land beside the exe
  (`build-dxr\RelWithDebInfo\log-<name>.txt`), captures in the bridge's `logs\`.
- Launch for the owner: desktop "PLAY VREALMS VR.bat". Models: "VRealms 3D - 3 MAKE CHAPTER 1-2 ITEMS.bat" (D:).

## 4. Traps learned today

- **Scale:** without a headset the VR scale falls back to 34 map units a metre; the owner's headset is 164.3. Test
  configs set `roth_vr_units_per_metre 164.3` (not archived) so flat runs match the headset.
- **`roth_xrpose handat` jumps the hand in one tic.** With solid hands, approach in steps (`handat ... palm 0 0 40`,
  32, 24 ...) or the hand meets geometry or shoves the item. Its palm offset reads the DRAWN hand, so a blocked hand
  skews it.
- A change to `vr_handpose.h` rebuilds most of the engine on the PC (12 minutes).
- Jolt meshes collide on their front only: the level's triangles are added with both windings.
- Jolt moves a body's centre of mass: the hand drive aims the COM at its target (or a turning hand drifts).

## 5. Next (milestone 2 onwards), in order

1. **Things worked by hand** -- doors, drawers, levers, wheels, keys turned in locks: constrained grabbables moved
   along their track by the hand, firing the original's use at the original's threshold. The gestures already exist
   as rules (`roth_worldhands.h`: THE PULL, THE TWIST, THE PRESS, THE HOUR HAND ...): make them physical (the lever
   swings with the hand, the key turns in the hole) and generalise to every door and lever. Reference to port from:
   **VRExpansionPlugin** (MIT, https://github.com/mordentral/VRExpansionPlugin,
   `Source/VRExpansionPlugin/Private/Interactibles/VRLeverComponent.cpp`, `VRDialComponent.cpp`,
   `VRSliderComponent.cpp`, `VRButtonComponent.cpp`, `VRMountComponent.cpp`; 3,700 lines). CLAUDE.md's porting rule:
   read the whole implementation, write the numbered checklist (source file:line, port file:line) before porting.
   UltimateXR (MIT, https://github.com/VRMADA/ultimatexr-unity) for grab points, snap anchors, two-hand.
2. **Sockets:** an item brought to its lock, slot or keyhole = the original's use with that item.
3. **Two hands:** two items held at once (select the item of the hand that makes a use at that use); two hands on
   one object.
4. **The level's own lying items as bodies** (knocked over, pushed), kept in step with the original's objects.
5. **Cloth:** the trenchcoat and curtains, XPBD after Matthias Mueller's Ten Minute Physics cloth (the owner linked
   yesongO/cloth-vr-simulation, a WebXR port of it).
6. Later: hand poses per hold kind (one closed pose for palm and fist holds), long weapons' grips and real sizes, the
   shield on the forearm.

## 6. Open items

- **"main" branch:** an earlier session left open whether VRealms should have a `main`. It has none; work is on
  `lane/gamelayer`, and `workingbuild` (the default) is an old ancestor. Unanswered: create `main` from the lane tip,
  or fast-forward `workingbuild`. Ask him once, in plain words; don't create branches without his yes.
- The statuettes on STUDY1's shelf are drawn 1.26 m tall (log: "item 4: its model, 1.26 m (its picture's size)"):
  an item without a table size takes its picture's size lying in the world. Not looked into.
- Weak generated models: the Orb (smeared pattern), Mask 60 (paper-thin), Sword 29, the Shards (one plate). Long
  weapons and the Shield keep the 10 cm picture size until their grips are done.
- The vials' glass at 0.30 opacity may be too faint in the headset (tuning).
- Two STUDY1 faces (1361, 1527) classify differently from the original in the L1 differential (class only).
- Nothing of today has been tried in the headset by the owner yet.
