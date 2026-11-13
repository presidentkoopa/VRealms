# The owner's FBX VR hands: capability report (2026-11-12)

Source: `/home/claude/dxr/models/hands/hand_final.fbx` (24,098,864 bytes) and `textures/` beside it.
Only the FBX is used. No IQM hand was opened, converted or consulted.

Tools written for this report (standard-library Python 3; PIL only for the PNG check):

- `tools/fbx_hands/fbxread.py`: a binary FBX 7.x node reader (zlib arrays, 64-bit records from 7500).
- `tools/fbx_hands/analyze.py <fbx> [global models meshes materials skin pose anim]`: a structural dump.
- `tools/fbx_hands/poses.py <fbx> [--holds --timeline --frames a,b --keys --palm]`: samples the baked joint curves
  frame by frame with the FBX SDK transform order (T·Roff·Rp·Rpre·R·Rpost⁻¹·Rp⁻¹·S…). It measures the
  fingers in the hand's own Root_joint frame, so the arm and root motion are removed. `--holds` takes about 3
  minutes.

Marking: **[V: how]** means verified, with the command, parse or file:line that shows it. **[R]** means
reasoning only, not seen. Nothing here was run in an engine or a headset. No claim below is "works in VR".

---

## Summary

- **Contents.** The file is one Maya 2019 scene with **two copies of one right-hand rig**: 2 meshes, 2 × 22
  joints, 2 skins, 44 rig controls. The second copy is mirrored by a -1 X scale on its top control, which
  makes it a left hand. It has **one take, `Take 001`**: 1,278 baked frames at 60 fps, 21.3 s long. It has
  **no named clips** and **no blend shapes**.
- **The take is a performance, not a clip set.** The arms move: open hands, a wrist and knuckle bend, fists
  that punch, then the right hand makes a two-finger "finger gun" and recoils 8 times while the left hand
  half-closes beside it. Every useful pose has to be cut out of this timeline by frame number.
- **Fixes the take needs before use.**
  - **Scale.** The file says centimetres, but the hand is 1.67 units from wrist to middle fingertip, so it
    must be rescaled.
  - **Root and wrist motion** must be stripped.
  - **Skin weights.** The skin has up to **5 influences** per vertex. Both engines' vertex format takes 4.
- **Neither engine can read FBX today.** In VRealms and UZDXREMA, `src/` and `libraries/` contain no ufbx,
  no assimp and no FBX loader. UZDXREMA reads the FBX hands only offline: Blender dumps pose matrices, and
  `tools/avatar/mkfbxposes.py` writes `wadsrc/static/vrhandpose.txt`.
- **UZDXREMA's finger driving.**
  - The trigger curls the index. The grip curls the other three fingers. The thumb gets a fixed 0.6 on
    thumb touch.
  - Every joint target is smoothed by `0.75^(dt·90)`. That takes 89 ms to reach 90 %.
  - A free hand (nothing held) bends about one fold axis per joint from FRIK's table, not with the FBX
    rotations.
  - `vr_grasp.cpp` in this checkout is still only part 1 of Quake VR's grasp. Parts 2-8 are absent, and so
    is `QUAKEVR_GRASP_SPEC.md`.
- **Recommendation [R].**
  - Convert at build time from this FBX to a new file the engine already skins: IQM. Write a reader-checked
    verify step, as `tools/fbx2iqm` does.
  - Drive each finger every frame by blending between pose endpoints sampled from the take, with the analog
    trigger and grip values, and little or no smoothing. Do not play timed clips.

---

## Verified facts (each with how)

| # | Fact | How |
|---|---|---|
| 1 | Binary FBX 7500, written by FBX SDK 2019.2 from Maya 2019 (`animation_4.mb`) on 2020-06-24 | `analyze.py … global` (header, `Creator`, SceneInfo) |
| 2 | GlobalSettings: UpAxis 1 (+Y), FrontAxis 2 (+Z), CoordAxis 0 (+X), UnitScaleFactor 1.0 (and Original 1.0), TimeMode 3, CustomFrameRate -1 | `analyze.py … global` |
| 3 | 60 fps: every key time is spaced 769,769,300 ticks = 1/60 s (46,186,158,000 ticks per second) | `analyze.py … anim`: spacing histogram `{769769300: 1278}` |
| 4 | One AnimationStack `Take 001` with one layer `BaseLayer`, LocalStop 21.667 s, keys from 0 to 21.300 s | `analyze.py … anim`; `Takes/Take` LocalTime |
| 5 | Joint curves are baked (1,278 keys, frames 0-1277). The 44 controls keep the animator's sparse keys (67-112 each; 169 distinct key frames) | `analyze.py … anim` key histogram; `poses.py --keys` |
| 6 | Two meshes `Hand_Low` and `Hand_Low1`: each 1,667 control points and 1,680 polygons (1,650 quads + 30 tris), 3,330 triangles, UV set `map1` with 3,874 UVs (IndexToDirect, by polygon vertex), normals by vertex, tangents and binormals by polygon vertex, smoothing by edge | `analyze.py … meshes` |
| 7 | The two meshes have identical vertex positions. Every polygon of `Hand_Low1` is the same polygon with its order reversed (1,680/1,680) | inline check after `analyze.py` (Vertices equal; PolygonVertexIndex reversed) |
| 8 | Skeleton A sits under `GLOBAL_MAIN_CONTROL_R` (Lcl S = 1,1,1). Skeleton B sits under `GLOBAL_MAIN_CONTROL_R1` (Lcl S = **-1**,1,1). Each has 22 LimbNodes with the same names, rest values and bind matrices | `analyze.py … models pose` |
| 9 | Both skins are bound to an unmirrored skeleton at the origin: each cluster's TransformLink equals its BindPose matrix to 3e-16, and the bind translations are identical for A and B | inline check; `analyze.py … pose skin` |
| 10 | Skeleton A is a **right** hand. Bind space has fingers along +Y, the thumb at +X (x 0.28-0.41), the thumb in front of the fingers (+Z), and the curled fingertips moving to +Z in the fist. A right hand seen palm-on has its thumb on the viewer's right | `analyze.py … pose`; `poses.py --palm`; the handedness rule is reasoning on those numbers |
| 11 | The hand is **1.6733 file units** from Root_joint to the middle fingertip. The mesh bbox is x[-0.325, 0.474] y[-0.001, 1.757] z[-0.309, 0.173]. Taken at UnitScaleFactor 1 (cm), that is a 1.7 cm hand | `poses.py` header line; `analyze.py … meshes` |
| 12 | Every LimbNode has Lcl Rotation ≈ 0 at rest, with its orientation in PreRotation (Maya joint orient). Root has InheritType 1, the others 2. All joint scales are 1 for the whole take | `analyze.py … models`; per-channel ranges script |
| 13 | Joint translations are constant (|Δ| < 0.008 on INDEX_BASE and THUMB_TOP only). Only rotations animate the fingers. UP_TOP (tip) joints never rotate | per-channel min/max dump of every LimbNode curve |
| 14 | Root_joint never moves or rotates. Root motion is on `GLOBAL_MAIN_CONTROL_*` (T.z up to 2.825, R up to ±86°) and `GLOBAL_cntrl` (T up to ±1.3, R up to ±177°). HANDPALM_joint (the wrist) rotates up to 55-59° | per-channel ranges |
| 15 | Skin: Linear skinning, 22 clusters per mesh, every vertex weighted, weight sums 0.998-1.000. Influences per vertex on `Hand_Low`: 1:579, 2:271, 3:345, 4:298, **5:174**. The 5th-largest weight is at most 0.0442 (mean 0.0054) | `analyze.py … skin`; inline 5th-weight check |
| 16 | No blend shapes: no BlendShape, BlendShapeChannel or Shape objects | object counts; inline check |
| 17 | Materials. `Hand_Low` uses `aiStandardSurface1`, with textures on baseColor (`lambert1_Base_Color.png`), normalCamera (`lambert1_Normal_OpenGL.png`) and specularRoughness (`lambert1_Roughness.png`). `Hand_Low1` uses `blinn1`, which has **no texture** | `analyze.py … materials` |
| 18 | All three textures are **embedded** in the FBX: base colour 2048² RGB 8-bit (4.6 MB), roughness 2048² grey 8-bit, normal 2048² RGB **16-bit** (13.3 MB). The 4th Video node is empty | `analyze.py … materials` (PNG IHDR parse) |
| 19 | `textures/lambert1_Base_Color.png` (2048² RGB) has **pixel-identical** content to the embedded base colour. Its bytes differ, which only shows it was re-encoded | PIL ImageChops difference: bbox None |
| 20 | `textures/lambert1_Base_Color_4.png` (2048² RGBA) is **not referenced by the FBX**. It is a blue vertical gradient with an alpha mask shaped like the UV islands (alpha 0 on 1.07 M pixels), not a skin | PIL compare plus a rendered thumbnail, viewed |
| 21 | UZDXREMA's `mkfbxposes.py` "frame N" is FBX frame **N-1**. Its pose angles match my sample of FBX frame N-1 to 0.1° (fist index 71.9/88.4/93.9°, ring 70.1/85.7/108.3°; wrap_tight; fingergun). Its hand A = `_R`, hand B = `_R1` | `poses.py` angles vs the `// deg` column of `uzdxrema/wadsrc/static/vrhandpose.txt` |

---

## The FBX's contents (tables)

### Skeleton (identical for A and B; bind translations in file units, bind space at origin)

Parent chain: `GLOBAL_MAIN_CONTROL_R[1]` (NurbsCurve) → `GLOBAL_cntrl` (NurbsCurve) → `Root_joint` → `HANDPALM_joint`
→ five chains. **[V: `analyze.py … models skin`]**

| Joint | Parent | Bind t (x, y, z) | PreRotation (deg) | Skinned verts |
|---|---|---|---|---|
| Root_joint (wrist) | GLOBAL_cntrl | 0.055, 0.049, 0.000 | 0, 0, 88.68 | 409 |
| HANDPALM_joint | Root_joint | 0.067, 0.565, -0.061 | 0, 0, -15.52 | 767 |
| THUMB_BASE / MID / TOP / UP_TOP | HANDPALM → chain | 0.278,0.748,-0.031 / 0.376,0.873,0.021 / 0.411,1.087,0.078 / 0.397,1.269,0.105 | z -19.5 / 26.0 / (180, 0, 15.1) / (-180, 0, 21.6) | 306 / 364 / 277 / 159 |
| INDEX_BASE / MID / TOP / UP_TOP | HANDPALM → chain | 0.263,1.098,-0.195 / 0.264,1.356,-0.187 / 0.270,1.520,-0.140 / 0.274,1.640,-0.097 | z 10.7 / 4.3 / 0.1 / -88.3 | 223 / 137 / 126 / 15 |
| MIDDLE_F_BASE / MID / TOP / UP_TOP | HANDPALM → chain | 0.082,1.098,-0.218 / 0.077,1.377,-0.195 / 0.077,1.593,-0.153 / 0.081,1.719,-0.105 | (180, 0, 18.0) / 1.2 / (180, 0, 1.2) / -15.7 | 240 / 118 / 119 / 5 |
| RING_BASE / MID / TOP / UP_TOP | HANDPALM → chain | -0.099,1.084,-0.190 / -0.112,1.328,-0.190 / -0.116,1.510,-0.125 / -0.116,1.623,-0.082 | (180, 0, 20.8) / 2.5 / 1.5 / (-180, 0, 16.8) | 256 / 101 / 113 / 2 |
| PINK_BASE / MID / TOP / UP_TOP | HANDPALM → chain | -0.243,1.057,-0.142 / -0.267,1.249,-0.117 / -0.271,1.386,-0.068 / -0.269,1.479,-0.033 | (180, 0, 25.9) / 8.5 / -2.5 / (-180, 0, 19.9) | 238 / 101 / 129 / 13 |

- Each finger and the thumb has 4 joints: 3 that rotate, plus a tip (UP_TOP) that never rotates but carries
  skin weight. The total is 22 joints per hand. **[V]**
- Some joints have a 180° PreRotation about X, so their local axes are flipped. The fold is mostly about
  local Y: for example, MIDDLE_F_MID on hand A animates Y only, from -8.6° to 99.0°. **[V: channel ranges]**
- Non-skinned rig objects: 22 NurbsCurve controls and 5 Nulls (`ROTATE_CNTRL`, `pivot` ×4) per hand. **[V]**

### The one take, segmented

Frames are 0-based FBX frames at 60 fps. `ext` is the knuckle-to-tip distance over its rest value
(1 = straight, about 0.25 = fully curled). `t2i` is the thumb tip to the index MID joint, in hand lengths.
All values come from **[V: `poses.py --holds --timeline --frames`]**. The pose names in the last column are
**[R]** interpretation of those numbers.

| Frames | Right hand (A) | Left hand (B) | Root / wrist | Reading [R] |
|---|---|---|---|---|
| 0-137 | all ext 1.00, every joint at 0° (= bind) | same | still | **open / rest** |
| 137-300 | fingers straight. BASE (knuckle) bend 10-17° at 153-183, then 50-61° at 192-218 (ext 1.01) | same shape | wrist (HANDPALM) to 57-64°; root moves up to z 2.0 | hand raise / flat-hand knuckle bend, not a finger curl |
| 292-319 | closes in about 25 frames | same | | fist transition |
| 319-685 | I/M/R/P ext 0.28/0.28/0.27/0.23, bend 259-275°; thumb ext 0.91, t2i 0.15 (across the index) | ext 0.25/0.34/0.39/0.37, thumb the same way | root z travels from -1.25 to +1.94: fingers constant, arms punching | **fist** (thumb over the index) |
| 686-698 | opens in about 12 frames | same | | |
| 699-742 | open (0°) | open | still | open |
| 743-761 | ring and pinky curl in 18 frames to ext 0.36/0.39; index and middle stay straight and together (spread 0.59-0.70) | open | | |
| 761-1277 | the same finger shape held; thumb out (t2i 0.22 → 0.36) | 810-830 half-closes to ext 0.69/0.70/0.68/0.72, thumb by the index (t2i 0.13); opens 863-908; recloses 947-959 to ext 0.68/0.66/0.70/0.88 and holds | right root jumps up at frames 985, 1006, 1028, 1055, 1080, 1106, 1127, 1150; the left root ends about 0.6 units from the right | **two-finger "finger gun" with recoil**; left = **half-curl wrap**, then **support / cup** |

Pose types that are **not in the take** (checked over the holds: no hold has only the index straight, none
has the thumb straight with all four fingers curled, and none has the thumb tip at the index tip):

- a one-finger point;
- thumbs-up;
- a pinch;
- a trigger-pull index with the other fingers open.

The fist thumb only reaches ext 0.90-0.92: it lies across the index and does not fold into the palm.
**[V: holds list]**

### What UZDXREMA's `fbx_poses.json` names, against the data

The frames below are converted to FBX N-1. **[V: `poses.py --frames`]**

| Name there | FBX frame | What the data shows |
|---|---|---|
| fbx_open | 0 | open (bind) |
| fbx_relaxed | 153 | fingers straight, knuckles 10-17°, wrist 10° |
| fbx_halfcurl | 192 | **fingers straight** (ext 1.0): knuckle bend only, wrist 57° |
| fbx_curl | 205 | **fingers straight** (ext 1.0), knuckles 50-60°, wrist 59°; not a curl |
| fbx_crossed | 303 | **mid-transition** into the fist (ring 0.83, pinky 0.63) |
| fbx_fist | 318 | fist, one frame before the hold settles (within 1°) |
| fbx_twofinger_point / fbx_fingergun (A) | 761 / 802 | the same finger shape (I/M straight, R/P curled); the thumb differs |
| fbx_wrap_half (B) | 819 | **mid-transition** (ext 0.86, moving toward 0.70) |
| fbx_wrap_tight (B) | 836 | the held half-curl (ext 0.70), **not tight** |
| fbx_thumb_out (B) | 943 | all fingers **open**; thumb at t2i 0.13, i.e. by the index |
| fbx_cup / twohand_support (B) | 984 | the relaxed curl (I 0.68, M 0.66, R 0.70, P 0.88) |

### Textures

| File | Size / format | Referenced by the FBX | Note |
|---|---|---|---|
| `textures/lambert1_Base_Color.png` | 2048², RGB 8-bit | yes (`file1`, by path `..\..\texturing\textures\…`) | same pixels as the embedded copy **[V]** |
| `textures/lambert1_Base_Color_4.png` | 2048², RGBA 8-bit | **no** | blue gradient with a UV-island alpha mask **[V]** |
| embedded `lambert1_Normal_OpenGL.png` | 2048², RGB 16-bit | yes (`file2`, normalCamera) | not on disk; extractable with `fbxread.py` **[V]** |
| embedded `lambert1_Roughness.png` | 2048², grey 8-bit | yes (`file3`) | not on disk **[V]** |

---

## UZDXREMA hand-posing code today (read-only; HEAD `39e4dad`)

**The pipeline, in order** (all **[V: file:line read]**):

1. **Inputs.**
   - OpenXR reads trigger value, squeeze value, thumb touch and trigger touch per hand:
     `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp:3936-3949`. The bindings include
     `thumbrest/touch`, `thumbstick/touch` and `trigger/touch` (`:2700-2709`).
   - The rig copies them into `FVRRigHandState`: `src/r_data/vr_rig.cpp:629-634`. On the desktop, fire and
     altfire give 0/1 instead (`:620-623`).
   - There is no per-finger capacitive input beyond thumb and trigger touch, and no XR_EXT_hand_tracking
     (grep finds none).
2. **The curl targets.** `UpdateCurls()` in `src/r_data/vr_avatar_pose.cpp:419`. It is called once per
   rendered frame from `VRAvatar_Pose` (`:1569`).
   - Index = trigger, or 0.3 on a touch alone (`:444`).
   - Middle, ring and pinky = grip value (`:445-447`).
   - Thumb = 0.6 if thumb touch, else 0 (`:448`).
3. **A drawn weapon** (`:476-562`).
   - Middle, ring and pinky take the named pose `grip_<prop>`, then `grip_<class>`, then `gun_grip`.
   - The thumb takes it too when the pose has a full rotation for it (`[FBXGRIP]`, `:516-532`).
   - The index blends ready → fire by the analog trigger (`:539-557`).
4. **A held non-weapon object** (magazine, round, slide, support, …) looks up `hold_<subject>[_<gun>]` for all
   five fingers (`:558-650`).
5. **Smoothing.** `retain = 0.75^(dt·90)` (`:431`). It applies to curls (`:654-662`) and to the rotation
   targets as a slerp (`:691-695`).
6. **The rotation target per joint.**
   - Use the pose's `rot` quaternion if one named pose set that joint (`:684`).
   - **Otherwise rotate about the FRIK fold axis**, `QAxisAngle(cj->Axis, cj->Degrees*want)` (`:688`).
   - So an empty hand driven by the controller **does not use the FBX rotations**: its "open" and "fist"
     rows in `vrhandpose.txt` are not read on that path.
7. **Application** (`:1770-1830`). Each joint gets `D = F·R(q)·Fᵀ`, where F is the body's measured canonical
   frame for that joint (`FVRAvatarFingerJoint::Frame`, `vr_avatar.h:74-81`, written by
   `tools/avatar/measure_avatar.py`). The descendants are carried along.
8. **`[FINGERLEN]`** (`:2336-2400`). Finger bones are moved out by the per-body `VRHANDFIT` length
   (`vr_avatar.cpp:357-390`). Then the grasp runs (`:2376`). Then the skin is stretched.
9. **The grasp.** `src/r_data/vr_grasp.cpp:179-351`.
   - It runs only when the hand holds a weapon.
   - It wraps thumb, middle, ring and pinky; the index is untouched.
   - It closes from the mod's pose (`[GRASPPOSE]`) in fixed steps of 1/30 along the FRIK fold table.
   - Contact is a sphere of 0.11 × palm width against a point cloud **sampled from MD3 guns only** (`:52`).
   - A finger already touching keeps its pose.
   - That is **part 1 of Quake VR's 8**. There is no conservative advancement, close-from-fist, settle,
     thumb turn search, Quake VR thumb motion, skin-fitted spheres or warm start. `QUAKEVR_GRASP_SPEC.md`
     does not exist in this checkout. Its own header (`vr_grasp.h:6`) still calls it "a port of Quake VR's
     auto-grasp".

**Model formats it takes.**

- `VRAvatar_Pose` works on any `FModel` with joints: IQM (`models_iqm.cpp`) or Valve Source `.mdl`
  (`models_studio.cpp`). See `model.cpp:227-245` and the palette conventions in `model.h:385-404`.
- The body needs a `.avatar` file from `measure_avatar.py`. That tool reads `.mdl` or `.iqm` only
  (its header).
- The body must have `Tier == VRTIER_FULL` and `HasFingers` (`vr_avatar_pose.cpp:1778`, `vr_avatar.h:52-57`).
- The tiers describe whole bodies with arms. Nothing in them is a hands-only model. **[V header; R on
  whether a hands-only model would measure]**

**What `mkfbxposes.py` does to an FBX** (`tools/avatar/mkfbxposes.py`, 202 lines):

- It never reads the FBX itself. It reads `fbx_pose_mats.json`, produced in Blender by
  `tools/claude_bridge/scripts/hands_dump_pose_mats.py` (`bpy.ops.import_scene.fbx`, armatures `Armature` and
  `Armature.001`, frames chosen by hand).
- For each hand, as stored and mirrored, it:
  - detects the side from the fist's fingertip travel;
  - builds canonical bone frames (+X along the bone, +Z the fold axis, +Y palmward) on the open frame;
  - writes each finger joint's full rotation from open as `rot <pose> <R|L> <finger> <1-3> x y z w`.
- Composed poses copy whole fingers from earlier poses.
- The UP_TOP joints are dropped. FBX BASE/MID/TOP become engine joints 1-3.

Two of its claims I checked against the data:

- Its comment "the FBX controls carry a 0.01 scale" is **not in the file**: every Lcl Scaling is ±1
  **[V]**. That scale must come from Blender's unit conversion **[R]**.
- Its frame numbers are Blender frames, 1 higher than the FBX's **[V, fact 21]**.

**What driving these FBX hands with that pipeline would need [R].**

1. A skinned file the engine loads: a new IQM re-exported from `hand_final.fbx`, never an existing one.
2. An avatar description: finger joint indices and measured frames. That means either extending
   `measure_avatar.py` and the tiers to a hands-only model, or a separate hand path that applies the same
   `D = F·R(q)·Fᵀ` to the 15 finger joints.
3. Poses taken from **held** frames, not the transition frames listed above.
4. The FBX rotations used on the free-hand path too, instead of the FRIK fold axis.
5. For grips, the grasp ported whole (all 8 parts) with spheres fitted to this hand's skin.
6. VRealms has none of 1-5 today. `src/r_data` has no `vr_avatar*`, `vr_grasp*` or finger code; it has only
   rigid hand drives (`model_handdrive.*`). **[V: grep]**

---

## FBX loading options

**Today: neither engine reads FBX.**

- [V] A case-insensitive grep for `ufbx`, `assimp`, `fbx` and `Kaydara` over `src/` and `libraries/` finds
  nothing in VRealms. In UZDXREMA it finds only comments.
- [V] The loader dispatch is `FindModel` (VRealms `src/common/models/model.cpp:141-245`; UZDXREMA
  `:156-265`). It goes by file magic: MD2, MD3, MDL, IQM, UE1, OBJ, and KVX as a fallback; UZDXREMA also
  has Source `IDST`.
- [V] VRealms also has `RegisterModel` (`model.cpp:266`) for models built in memory.
- [V] Skinned rendering is the IQM path (`model_iqm.h:128-200`, `CalculateBones`, `TRSData`).
  `FModelVertex` has **4** bone slots with 8-bit weights (`src/common/rendering/i_modelvertexbuffer.h:22-30`).

**Adding ufbx at runtime [R; ufbx facts from its README].**

- ufbx is two files (`ufbx.c`, `ufbx.h`), dual MIT / public domain. It reads binary and ASCII FBX from
  version 3000 and has skinning, blend shapes, animation curve evaluation and CPU skinning.
- From memory, not checked: the API names for scene evaluation, baking and unit or axis conversion.
- Where it would plug in: a magic test for `"Kaydara FBX Binary  "` in `FindModel`, and a new
  `FFBXModel : FModel`. That class would most simply fill the same arrays `IQMModel` uses (joints, inverse
  bind, `TRSData` per frame, `FModelVertex` with 4 weights), so `CalculateBones` and the renderer stay as
  they are.
- What it must provide:
  - the skinned mesh, with the 5th weight dropped and the rest renormalised;
  - the joint hierarchy, including PreRotation and the mirrored parent;
  - bind matrices from the clusters;
  - the take sampled at 60 fps into per-joint TRS, or poses sampled by frame number;
  - the textures.

**Build time against runtime, for a fixed, versioned mod set [R].** Build time is the better choice.

- One reviewed converter writes a small file, checked by a verifier that reads the written bytes back
  (the method of `tools/fbx2iqm/fbx2iqm.py`, which exists in VRealms but needs Blender).
- The 24 MB FBX carries 19 MB of embedded PNGs and Maya rig controls, none of which the game needs.
- No third-party parser runs at load. Every client loads byte-identical data.
- What the converter has to do:
  - scale the hand: the owner picks the size; `fbx2iqm`'s `hand_*.json` asks for 0.19 m;
  - mirror for the left hand, as data, not as a negative node scale;
  - choose the frames;
  - reduce to 4 weights.
- `fbxread.py` already does the parsing without Blender. An IQM writer is **not** written.
- **A re-export is needed.**

---

## Responsiveness plan [R throughout, built on the measured numbers]

1. **Pose endpoints, not clips.** Each frame, for each finger,
   `q_joint = slerp(open_joint, closed_joint, analog)`, using the 3 rotating joints per finger. The
   transitions authored in the take would add 0.2-0.42 s if played: about 25 frames to close the fist and
   about 12 to open it **[V]**. A blend adds none.
2. **Endpoints from held frames only** (FBX 0-based):

   | Input | Fingers | Open | Closed | Middle point (optional) |
   |---|---|---|---|---|
   | grip (analog) | middle, ring, pinky | frame 0 | fist, A frame 500 | B frame 845 (wrap, ext 0.70) |
   | trigger (analog) | index | frame 0 (or 0.3 on trigger touch, as UZDXREMA does) | A frame 500 | B frame 845 index (ext 0.69) as "finger on the trigger" |
   | thumb touch (bool) | thumb | frame 0 | A frame 500 thumb (across the index) | B frame 845 thumb (by the index, t2i 0.13) |

   A per-finger composition gives the poses the take lacks:
   - **point** = index open, the rest closed;
   - **thumbs-up** = thumb open, the rest closed.

   A **pinch** is not in the data. It needs authoring or a solver.
3. **Strip everything above the fingers.**
   - Root motion: `GLOBAL_MAIN_CONTROL_*` and `GLOBAL_cntrl` move up to 2.8 units and turn up to 177°.
   - The wrist: HANDPALM_joint rotates up to 59°.
   - Use only the 15 finger joint local rotations. The controller owns the wrist.
   - Frames 137-300 (wrist and knuckle gesture), 303, 819 and 943 are unsuitable as endpoints. So are the
     recoil frames, for anything but fingers.
4. **Latency.**
   - UZDXREMA's smoothing (`retain 0.75` per 1/90 s) reaches 50 % in 27 ms, 63 % in 39 ms and 90 % in 89 ms
     **[V: arithmetic on `:431`]**.
   - For analog trigger and grip, use no smoothing, or a much shorter constant. Analog sensors are already
     continuous.
   - Smooth only the boolean touches, so a 0 → 0.6 thumb jump does not pop. 20-30 ms is suggested.
   - Sample the inputs from the same predicted frame as the hand pose. **Not checked** where UZDXREMA
     samples them relative to `xrWaitFrame`.
5. **Cost.**
   - Each hand has 22 joints, 1,667 control points and 3,330 triangles. That is trivial to skin.
   - The GPU vertex count after UV splitting is at most 6,690 polygon-vertex references and at least 3,874
     UVs **[V counts; split count R]**.
   - The UP_TOP joints never rotate. Their weights can merge into TOP exactly, for this animation **[R]**.
6. **The left hand.** Mirror the right mesh and skeleton at conversion: the bind data is identical
   **[V]**. Or take hand B's own poses: its fist and wrap differ from A's **[V]**. Give it the skin texture,
   because `blinn1` in the FBX has none **[V]**.
7. **Grips on guns.** Port Quake VR's grasp whole (8 parts, against a checklist), with spheres fitted to
   this mesh's skin. The FRIK fold table should not be the curl path: the postmortem traces the bad thumb to
   it.

---

## Open questions for the owner

1. **Hand size.** The FBX is 1.67 units from wrist to middle fingertip and says centimetres. What real
   length do you want? `fbx2iqm`'s `hand_*.json` uses 0.19 m.
2. **The left hand.** Mirror the right hand (one shape set), or use the FBX's own left-hand poses, which
   differ slightly?
3. **Texture.** `lambert1_Base_Color.png` (skin, the one the FBX references), or the blue `_4` variant with
   alpha, which the FBX does not reference? Should the embedded normal and roughness maps be extracted now,
   or wait (cosmetic)?
4. **Missing gestures.** Point, thumbs-up and pinch are not in the take. Compose them per finger from the
   existing poses, or author them in Maya and re-export?
5. **Engine path.** VRealms has no finger or avatar pipeline. Port UZDXREMA's (then fix the free-hand path
   and the grasp), or write a hands-only path? And is a build-time FBX → IQM converter (Blender-free, from
   `fbxread.py`) the route you want, rather than ufbx in the engine?
6. **Wrist motion.** HANDPALM bends up to 59° in the take. May the wrist ever be animated, or does the
   controller always own it?
