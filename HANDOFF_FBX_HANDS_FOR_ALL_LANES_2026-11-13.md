# FBX hands: how they are built, and how another lane takes them without doing it twice

Written 2026-11-13 for every lane that touches the VR hands, in REMAROTH (VRealms) or in UZDXREMA (REMA).
The work below is on VRealms `lane/gamelayer` (github.com/presidentkoopa/VRealms) and on the owner's PC copy
`E:\DOOMWork\REMAROTH`. **Take it from there; do not rebuild it.**

Marking: **[V]** verified (how stated), **[R]** reasoning only, not seen.

---

## 0. The owner's rules for hands (do not relitigate)

1. **USE THE FBX HANDS.** The engine reads the owner's `hand_final.fbx` itself, at run time. No conversion to IQM, MD3,
   OBJ or anything else, offline or at build time. No existing IQM hands. (He said it three times; it was ignored twice.)
2. **Two separate hands.** `Hand_Low` (right, skeleton under `GLOBAL_MAIN_CONTROL_R`) and `Hand_Low1` (left, skeleton
   under `GLOBAL_MAIN_CONTROL_R1`) are separate meshes with separate skeletons, skins and animation curves. **Never
   clone or mirror one hand from the other.** The `-1` X scale on `GLOBAL_MAIN_CONTROL_R1` is just how Maya makes it a
   left hand; keep that transform, that is all.
3. **Nothing from RS_Hands.** The owner sent RS_Hands' *images* (its pose sheet) only. RS_Hands itself was a degraded
   derivative (one `hand_left.iqm`, mirrored with `Scale -1`). Its frame map, its pose recipe
   (`tools_export_poses.py`), its thumb rules: none of it. All removed from VRealms in 2e58b3e4.
4. **The hands have poses.** Fingers are driven by **authored frames of each hand's own animation**, not by bending
   joints with a formula. In-betweens are a slerp between two authored frames.
5. **The palm does not cup** when any finger is straight (the owner saw a pointing index swing 4 cm in the headset).
6. Don't ask the owner which frame is "open" or "fist": the frames come from his file and his table, and are chosen by
   measuring (section 4).

---

## 1. What exists, where (VRealms `lane/gamelayer`)

| Commit | What |
|---|---|
| 69e2e065 | **Native FBX loader** (ufbx), both hands, every frame of the take |
| 2e58b3e4 | RS_Hands pose layer removed |
| 212d6dcd | UZDXREMA's curl rule on the FBX hands (superseded by 0bd9acc0, kept for the smoothing port) |
| b5e2257f | Left hand's skin (`mirrorskin`), the FBX's embedded normal + roughness maps, palm cup |
| b60ded40 | Palm cup off |
| 0bd9acc0 | **Authored poses drive the fingers** (current) |
| 15c9da27, 10e884f6, 79dc269c | The VR hand picker (Realms-specific interaction; section 7) |

**Files (engine-generic: these are what another engine takes):**
- `libraries/ufbx/` — ufbx vendored unchanged (MIT, commit 5955c5c0), with its own `CMakeLists.txt`; top-level
  `CMakeLists.txt` `add_subdirectory`, and `src/CMakeLists.txt` links it and adds the include.
- `src/common/models/fbx_skinned.{h,cpp}` — **engine-free** reader: one skinned mesh + the skeleton its skin binds to
  (the FBX's own joint names, parents first) + every frame of every take, output in IQM file conventions (Z-up, file
  units). Root joints keep their whole world transform (that is where the left hand's mirror lives).
- `src/common/models/model_fbx.h`, `models_fbx.cpp` — `FFBXModel`, an `IQMModel` underneath, so the engine's existing
  bone path draws it unchanged: FrameIndex, `FindJoint`/`GetJointPose`, take lookup by name (`"Take 001:500"`).
  `model_iqm.h`: members private → protected and `LoadGeometry()` virtual; nothing else changed for IQM.
- `src/common/models/fbx_image.h` — decodes the FBX's embedded images (engine's stb_image; 16-bit rounded, not truncated).
- `src/common/models/model.cpp` `FindModel` — the **selector**: `"<file>.fbx:<mesh>[?mirrorskin=<mesh>]"`. Only names
  ending in `.fbx` before the colon are split. Two skinned meshes and no mesh named → refused, the error lists them.
- `src/r_data/vr_handpose.{h,cpp}`, `src/r_data/vr_handpose_math.h` — the hand on the controller and the fingers:
  input → pose → smoothed joint rotations → the bone palette (`VRHandPose_Apply`, called from `models.cpp`
  `RenderModelFrame`). `vr_handpose_math.h` is engine-free and is what the tests compile.
- `src/r_data/models.cpp` — `FollowHandMode` 3/4 (the model rides the controller GRIP pose, in metres), the seat
  (`VRHandPose_ModelSeat`), and the call into `VRHandPose_Apply`.
- `src/common/rendering/hwrenderer/data/hw_vrmodes.h` `VRFingerInput` + `VRMode::GetFingerInput`;
  `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp` — trigger, grip, **capacitive** trigger touch and thumb
  touch per controller (bindings for Oculus Touch, Index, Vive, WMR; search `xrThumbTouchAction`).
- `vrealms_iwad/MODELDEF.txt` — the two hands (below). `vrealms_iwad/models/hands/hand_final.fbx` (the owner's file,
  24,098,864 bytes, from his DXR repo `models/hands/`) and `lambert1_Base_Color.png`.

**Files (Realms-specific; another game writes its own):** `src/roth/roth_hands.cpp` (spawns the two hand actors in VR
only, `roth_handtest` for flat captures), `wadsrc/static/zscript/actors/realmshands.zs`.

**MODELDEF as it stands:**
```
Model RealmsHandRight { Path "models/hands"  Model 0 "hand_final.fbx:Hand_Low"
    Scale 0.11094212 0.11094212 0.11094212  NoAutoReverse  NoPixelStretch  FrameIndex RHDR A 0 0 }
Model RealmsHandLeft  { Path "models/hands"  Model 0 "hand_final.fbx:Hand_Low1?mirrorskin=Hand_Low"
    Scale 0.11094212 0.11094212 0.11094212  NoAutoReverse  NoPixelStretch  FrameIndex RHDL A 0 0 }
```
Scale = 0.19 m / 1.712605 file units (wrist `Root_joint` to the farthest middle-fingertip vertex). The grip path is in
**metres** (the grip frame × `VR_UnitsPerMeter()` at draw), so Scale is metres per file unit and the hand is 0.19 m at
any units-per-metre. 0.19 m was the owner's choice.

---

## 2. Facts about hand_final.fbx you will otherwise rediscover (all [V], `tools/fbxload_test`)

- One Maya 2019 scene, **one take** `Take 001`: joint curves have 1,278 keys (0–1277) but the controls above each
  skeleton are keyed to 1278, so the loader exposes **1,279 frames at 60 fps**; frame 1278 holds 1277's joints.
- **Frame 0 = the rest pose** (to 2.5e-8).
- **Frame numbering:** the owner's UZDXREMA table (`tools/avatar/fbx_poses.json`, written for `mkfbxposes.py`) uses
  **Blender 1-based** numbers. Take frame = Blender − 1. Checked: take 318 matches his `fbx_fist` (319) to 0.13°/0.11°;
  317 and 319 are 1.3–3.6° off.
- 22 joints per hand: `Root_joint`, `HANDPALM_joint`, and THUMB / INDEX / MIDDLE_F / RING / PINK × BASE / MID / TOP /
  UP_TOP. `UP_TOP` is a skinned tip with no role.
- **Left hand:** skin determinant −1 (the mirror), kept; faces and normals agree on both hands (3330/3330).
- **The file's normals are stale** (a rigidly rotated copy, ~47°, facing the surface by 0.72 / 0.20): the loader uses
  the surface's own normals and logs it.
- **5th skin weights are dropped** (the engine's vertex has 4): 174 / 170 vertices, largest 0.044, ≤ 2 mm in the fist.
- **The left hand has no texture**: `Hand_Low1`'s material `blinn1` is a plain 0.5 grey; its UVs are `Hand_Low`'s
  mirrored in u **about 1.002832** (not 0.5: the islands' centre; constant to 4e-8 on all 6,690 corners). `mirrorskin`
  draws it with `Hand_Low`'s material at `u' = 1.002832 − u`. `lambert1_Base_Color_4.png` in DXR is NOT a skin (a
  gradient mask).
- **Embedded maps** (in `aiStandardSurface1`): base colour 2048² (byte-identical to the PNG beside it), **normal map
  2048² 16-bit, OpenGL/green-up** (measured), roughness 2048² 8-bit. Attached as the engine's PBR material
  (`SetShaderLayers`; metallic 0 and AO 1 as constant textures). In this engine the normal map only shows under
  dynamic lights. [R]
- **No pinch pose exists** in the take (confirmed, and the owner's `fbx_poses.json` says so too).
- Load cost ≈ 0.3 s per hand [R]; maps add ~0.25 s; each 2048² map ~16 MB VRAM.

---

## 3. The hand on the controller [V in flat on the owner's PC; R in a headset]

- Ported from UZDXREMA (`vr_rig.cpp` HandFrameFromJoints, `DefaultHandFit`; checklist at the top of `vr_handpose.cpp`):
  the hand frame from wrist + 4 knuckles, the grip socket = knuckle centroid + palm width × UZDXREMA's fitted vector.
- **Departure:** the grip SOCKET (not the wrist) sits at the OpenXR grip origin (the palm centroid inside the handle),
  as UZDXREMA's gun-anchor path does.
- `NoPixelStretch` on the hands: the grip frame already carries the world's pixel stretch.

---

## 4. The fingers: authored poses (0bd9acc0) [V: tests; V: flat captures on the PC]

**Input:** `VRFingerInput` per controller: trigger (analog), grip (analog), triggerTouch, thumbTouch. No controller →
open hand (UZDXREMA's rule: "an open hand, not a frozen one").

**Which pose** (`PoseRule`, vr_handpose_math.h:210):

| index on the trigger (touch or any pull) | thumb touching | pose |
|---|---|---|
| yes | yes | fist |
| yes | no | thumb up |
| no | yes | point |
| no | no | finger gun |

**How far:** the index goes by the trigger when on it (a touch alone = 0.3, UZDXREMA VAP:444); every other finger, the
index when off the trigger, and the palm go by the grip. Grip 0 + trigger = "trigger only" (index curls, rest open).

**Where to:** each joint's LOCAL rotation slerps from the open frame (take 0) to that pose's authored frame, per finger
(`kPoseSources`, vr_handpose_math.h:868, take frames):

| pose | right (Hand_Low): thumb, index, middle, ring, pinky / palm | left (Hand_Low1) |
|---|---|---|
| fist | 318 318 318 318 318 / palm 318 | 318 318 318 318 318 / palm 318 |
| point | 318 **808** 318 318 318 / palm off | 318 **0** 318 318 318 / palm off |
| thumb up | **990** 318 318 318 318 / palm off | **205** 318 318 318 318 / palm off |
| finger gun | 808 808 318 808 808 / palm off | 205 0 318 318 318 / palm off |

Chosen by walking every frame of both takes (`tools/fbxload_test/walk_poses.py`: fingertip-to-palm distance,
straightness, "held" = < 0.05°/frame). Where they differ from the owner's table, and why:
- right index/finger gun 808 (his `fbx_fingergun` 803 = take 802 is mid-recoil, index bent 14.8°; 808 is the hold);
- right thumb up 990 (his 985 = take 984 is a twitch);
- left thumb up 205 (his `thumb_out` 944 = take 943 lies flat along the fingers);
- the left take has no straight-index frame except the open frames, so its point index is frame 0.
No single frame of either take is a point, thumb up, middle-closed finger gun or trigger-only hand: those are
**composed** the owner's way (each finger taken whole from one authored frame — his `fbx_poses.json` "composed" entries
do the same).

**Palm:** `HANDPALM_joint` is the parent of all five fingers; driven only in the fist (measured: the fist's palm would
carry a pointing index 47 / 33 mm and a raised thumb 24 / 21 mm).

**Smoothing:** UZDXREMA's, unchanged: `retain = 0.75^(dt·90)`, dt from `screen->FrameTime`, hitch/first frame → 1/90,
`cur = slerp(cur, target, 1 − retain)`, once per displayed frame per side (VAP:421-431, 691-696).

**Applying it:** the source's own matrix route (D = F·R·Fᵀ on the joint's global, every joint below carried —
UZDXREMA VAP:1785-1836), on the finished bone palette.

---

## 5. Tests (run them; they need no engine)

- `tools/fbxload_test/run.sh` — loader vs ufbx and vs `tools/fbx_hands/poses.py`'s own FBX reader (0.0000° on every
  finger joint), skinning, 0.19 m, handedness, faces/normals, mirrorskin (S1–S5), maps byte-for-byte (`maps_ref.py`),
  poses: every state at full input equals its authored frame/composition to 1e-5°. `FBXLOAD_RENDER=1` writes software
  renders; `FBXLOAD_DUMP_FRAMES=a,b,c` picks frames to render. Needs `UZDXREMA` (default `/home/claude/uzdxrema`).
- `tools/hands_test/run.sh` — UZDXREMA's own rate/smoothing lines cut out at build time and run beside the port:
  difference 0 at 72–144 Hz with jitter, hitches, pose switches, dropouts; the rule's cases.
- Flat capture on the owner's PC: `run -Exec tools/claude_bridge/cfg/hands_flat.cfg` (`roth_handtest <pose> <turn>`;
  keep **30 tics between a capture and the next input**, or the next pose lands in the picture).
  `docs/images/hands/engine_poses_flat.png` is that capture (open, fist, point, thumb up, finger gun, trigger).

---

## 6. How another engine (UZDXREMA / REMA) takes this — without redoing it

1. Copy `libraries/ufbx/` and the CMake lines. Copy `fbx_skinned.{h,cpp}`, `fbx_image.h`, `model_fbx.h`,
   `models_fbx.cpp`; apply the small `model_iqm.h` change (protected + virtual `LoadGeometry`) and the `FindModel`
   selector. These are engine-generic and touch nothing game-specific.
2. Copy `vr_handpose_math.h` and the parts of `vr_handpose.cpp` / `models.cpp` your hand path needs. **UZDXREMA already
   has its own hand path** (`vr_avatar_pose.cpp` UpdateCurls, `[POSEROT]`, VRHANDPOSE lumps from `mkfbxposes.py`,
   `vr_rig.cpp`): VRealms' code was *ported from* those. Decide per piece, don't run both:
   - the **loader** is new and missing in UZDXREMA — take it;
   - the **pose table** (`kPoseSources`) and **PoseRule** replace UpdateCurls' empty-hand curl targets with authored
     frames; UZDXREMA's weapon-grip / held-object branches (`gun_grip`, `hold_*` from VRHANDPOSE) were *not* ported
     here and remain UZDXREMA's;
   - the smoothing is identical to UZDXREMA's (tested), so either copy is fine.
3. MODELDEF the two hands with the selector strings above (`?mirrorskin=Hand_Low` on the left).
4. Run `tools/fbxload_test` and `tools/hands_test` in the new tree before the first build.
5. The input (`VRFingerInput`, touch bindings) exists in both engines (UZDXREMA had it first; VRealms added Index, Vive,
   WMR touch bindings — take those).
6. For netplay (UZDXREMA's goal): finger input must enter through `usercmd_t` as UZDXREMA's rules say; VRealms reads it
   directly (single-player) — that part does NOT carry over as-is.

---

## 7. Not hands, but built on them (VRealms only)

The VR hand picker (`src/roth/roth_vrpick.cpp`, `src/roth/roth_cursor.h`), the owner's buttons since 2026-11-13: trigger = examine
(the original's primary), grip = use (its secondary), in reach or pointing; dwell and index tap removed. Before that: point = examine (dwell), index tap = far use,
fingertip press / grip = near use, through a port of the original's `classify_cursor_target_object` (786,432 cases,
0 differ) and the original's measured reach (`docs/REMAROTH_MEASURED.md` section 13). It reads fingertip positions from
the posed bones (`VRHandPose_FingerPoints`). Realms-specific.

---

## 8. Open (owner's call or not yet seen)

- **Not yet judged in the headset** after the authored poses (0bd9acc0).
- New authored hand frames: a lateral pinch and a fingertip pinch (keys, matches, pages; `docs/ITEM_DESIGN.md`).
- Weapon in the posed hand, Quake VR's grasp (8 parts), DistanceGrab: later steps (see the gamelayer handoffs).
- Skin-tone choices (natural and unusual: blue, red, pink): planned by the owner, not built.

## 9. Mistakes made on the way (so nobody repeats them)

1. Converting the FBX to IQM offline (twice) instead of loading FBX — the owner's first instruction.
2. Building the left hand from the right's poses ("mirror") — threw away the left hand's own animation.
3. Taking RS_Hands' frame map, recipe and thumb rules — the owner sent only images.
4. Bending fingers by formula (FRIK angle table) when the take has authored poses.
5. Cupping the palm with grip — carried straight fingers 2–5 cm.
6. Asking the owner to pick frames — measure them instead.
7. Capture configs with < 30 tics between a shot and the next input — every picture showed the next pose.
