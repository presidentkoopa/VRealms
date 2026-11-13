# The engine's FBX model loader (2026-11-13)

The owner's rule: **use the FBX hands.** The engine reads `hand_final.fbx` itself at run time. Nothing is
converted to IQM, MD3, OBJ or anything else, offline or at build time. No existing IQM hand is used. The
left hand is not made from the right hand: it is the file's own left hand (mesh `Hand_Low1`, its skeleton
under `GLOBAL_MAIN_CONTROL_R1`, mirrored by that control's -1 X scale) with its own take curves. Every
frame of the take is in the model.

Source: ufbx (MIT / public domain), commit `5955c5c0b042ac2dc6f32955ab206aaec0620cfc`, vendored unchanged
in `libraries/ufbx/` (`ufbx.c`, `ufbx.h`, `LICENSE`).

Marking: **[V: how]** = verified, with the check that shows it. **[R]** = reasoning only, not seen. Nothing
in this file has run in the engine: it is not built on the owner's PC yet.

Order of work, said plainly: the checklist below was written while the code was written, after a probe of
the file with ufbx (`tools/fbxload_test` grew out of that probe). It was not written before the first line.

## How it fits

```
MODELDEF  Model 0 "hand_final.fbx:Hand_Low1"
  -> model.cpp FindModel: "<file>.fbx:<mesh>" -> lump hand_final.fbx, mesh Hand_Low1   (W1)
  -> FFBXModel (model_fbx.h, models_fbx.cpp) : IQMModel                              (E1-E9)
       -> FBXSkinned::Load (fbx_skinned.cpp, engine-free) -> ufbx                    (C1-C20)
  -> drawn, posed and queried by IQMModel's own code (CalculateBones, FindJoint, GetJointPose, ...)
```

`FFBXModel` fills exactly the members `IQMModel::Load` fills from an `.iqm`, in the same conventions (file
space Z up, Y/Z swapped on the vertices at load, `TRSData[frame * joints + joint]`), so nothing downstream
can tell the difference. `fbx_skinned.cpp` has no engine code in it; `tools/fbxload_test` compiles and runs
that same file against the FBX.

## Checklist (the acceptance test)

ufbx lines are `libraries/ufbx/ufbx.h` (h) and `ufbx.c` (c). Ours: `src/common/models/fbx_skinned.cpp` (fs),
`models_fbx.cpp` (mf), `model.cpp` (mc), `model_iqm.h` (mi), `src/r_data/vr_handpose.cpp` (vh).

### C: the reader (fbx_skinned.cpp)

| # | Part | ufbx | Ours | Status |
|---|---|---|---|---|
| C1 | Load from memory (the lump's bytes) | `ufbx_load_memory` h:5291 | fs:197 | done |
| C2 | Embedded blobs not kept (19 MB of PNGs; textures come from the engine's own files) | `ignore_embedded` h:4700 | fs:192 | done, deliberate |
| C3 | Right-handed Z up, the IQM file convention; units untouched (size is MODELDEF Scale's) | `target_axes` h:4841, `ufbx_axes_right_handed_z_up` h:5264, `space_conversion` h:4819 (TRANSFORM_ROOT) | fs:194-195 | done [V: T3, compare.py root positions in the file's Y-up] |
| C4 | Pick the skinned mesh by its node's name, case-insensitive; refuse an unnamed pick when there are several | `ufbx_mesh.instances`, `skin_deformers` | fs:207-237 | done [V: T1] |
| C5 | Material and its base-colour texture file name | `ufbx_material_map base_color` h:2546, `ufbx_texture.relative_filename` h:2911 | fs:240-251 | done |
| C6 | Skeleton = the nodes the skin's clusters bind to | `ufbx_skin_cluster.bone_node` h:2022 | fs:253-261 | done [V: T1, 22 joints] |
| C7 | Joints ordered parents first; parent = nearest ancestor that is a joint | scene tree, `ufbx_node.children` | fs:262-285 | done [V: T1] |
| C8 | Rest pose = the scene as stored | `ufbx_node.node_to_world` h:931 | fs:289-301 | done |
| C9 | The bind is one transform for every joint at rest (so a rest-based palette skins as the FBX does) | `ufbx_skin_cluster.geometry_to_bone` h:2025 | fs:303-317 | done [V: 1.1e-15; T4 every weight 6.7e-6 mm = float rounding] |
| C10 | Rest mesh = the file's own skin at rest, all weights, normalised | `ufbx_skin_deformer.vertices/weights` h:1982 | fs:320-361 | done [V: T4] |
| C11 | Root joint's local = its whole model-space transform (the controls above it: arm motion, the left hand's mirror) | `node_to_world` | fs:158-182 | done [V: T3] |
| C12 | Child local = parentWorld⁻¹ · world, decomposed (keeps any inherit mode ufbx resolved; Maya's segment scale compensate is `IGNORE_PARENT_SCALE` h:814, resolved at c:23026) | `ufbx_matrix_to_transform` h:5594, c:31862 (a negative determinant becomes one negative scale axis) | fs:172-180 | done [V: T3 4e-15] |
| C13 | Triangles in the file's polygon order, reversed only if inside out at rest (signed volume) | `ufbx_triangulate_face` h:5657 | fs:363-387 | done; the file's order already faces out on both hands [V: T7] |
| C14 | Normals: the file's, through the rest skin's inverse transpose, if they face the surface (mean cosine ≥ 0.95); otherwise the surface's own (area-weighted per control point) | `ufbx_matrix_for_normals` h:5586, c:31792 | fs:389-424 | done; **the file's normals fail it on both hands** (below) [V: T7] |
| C15 | 4 largest weights per vertex, renormalised, bytes summing to 255 | | fs:426-453 | done; **loss** (below) [V: T4] |
| C16 | One vertex per polygon corner; UV v flipped to texture rows (as IQM) | `vertex_uv` | fs:455-472 | done [V: render.py textured right hand] |
| C17 | Winding: (b−a)×(c−a) against the outward normal in file space, as the engine's IQMs | | fs:474-482 | done [V: T7 3330/3330; R: that this is the engine's IQM convention comes from the old converter's measurement on three Blender IQMs, `git show 864c0020:tools/fbx_hands/fbx2iqm.py` ~line 582] |
| C18 | Every take (anim stack), in file order | `ufbx_anim_stack` h:3090 | fs:484-545 | done [V: T2] |
| C19 | Frame span = the keys on the skeleton's curves and on every node above it; frames at the file's rate | `settings.frames_per_second` h:3915, `ufbx_anim_curve.keyframes` | fs:486-522 | done [V: T2, 1,279 frames] |
| C20 | Each frame evaluated by ufbx | `ufbx_evaluate_scene` h:5496, c:31186 | fs:524-537 | done [V: T3, compare.py] |

### E: the model (models_fbx.cpp, IQMModel underneath)

| # | Part | IQM source it mirrors | Ours | Status |
|---|---|---|---|---|
| E1 | Joints: names, parents, rest local TRS, children, roots; Position/Rotation/Scaling from the rest world matrix (IQM's per-axis accumulation is not exact under a mirrored parent) | models_iqm.cpp:127-177 | mf:46-69 | done [R: the world-matrix choice; V: T3 for the TRS] |
| E2 | baseframe / inversebaseframe | models_iqm.cpp:201-226 | mf:71-95 | verbatim |
| E3 | TRSData, every frame of every take | models_iqm.cpp:228-257 | mf:97-107 | done |
| E4 | One animation per take, by name (`FindFrame("Take 001:500")`, `FindFirstFrame("Take 001")`) | models_iqm.cpp:188-199, 488-526 | mf:109-120 | done [R: not called in the engine yet] |
| E5 | One surface; its skin = the material's own texture beside the model; MODELDEF Skin overrides | models_iqm.cpp:108, 528-584 | mf:122-133 | done |
| E6 | Triangles | models_iqm.cpp:111-117 | mf:135-138 | done |
| E7 | Vertices stored (x, z, y) with normals, as LoadPosition/LoadNormal | models_iqm.cpp:352-407 | mf:140-152 | done |
| E8 | Two load log lines: what was read, what could not be carried | | mf:154-176 | done |
| E9 | Geometry rebuilt from Load's vertices, not by re-reading the file | models_iqm.cpp:298-313 | mf:179-193; mi: `LoadGeometry()` made virtual, members protected | done |

### W: wiring

| # | Part | Where | Status |
|---|---|---|---|
| W1 | `"<file>.fbx:<mesh>"` in a model name: the file is the lump, the mesh after the colon; only names ending `.fbx` before the colon are split | mc:149-167, 191-194 | done [V: g++ -fsyntax-only] |
| W2 | ufbx as a static library; linked into the engine; include path | `libraries/ufbx/CMakeLists.txt`, `CMakeLists.txt` (add_subdirectory), `src/CMakeLists.txt` (PROJECT_LIBRARIES, sources, include) | done [R: not configured or built; no MSVC here] |
| W3 | MODELDEF: RealmsHandRight = `hand_final.fbx:Hand_Low`, RealmsHandLeft = `hand_final.fbx:Hand_Low1`, Scale 0.11094212 | `vrealms_iwad/MODELDEF.txt` | done |
| W4 | The hand actors show the model sprites RHDR / RHDL | `wadsrc/static/zscript/actors/realmshands.zs` | done |
| W5 | vr_handpose: an FBX take has no pose map, so the fingers are not posed; logged "pose targets not mapped yet" | vh:265, 295-303, 345-348 | done |
| W6 | The owner's FBX in the game package | `vrealms_iwad/models/hands/hand_final.fbx` (24,098,864 bytes, md5 dd7cf882…, from his dxr repo `models/hands/`) | done |

### Not carried (each one said, none hidden)

- **The 5th skin weight.** `FModelVertex` has 4 bone slots (`i_modelvertexbuffer.h`). 174 control points
  of Hand_Low and 170 of Hand_Low1 have 5; the largest weight cut is 0.0442. With byte weights on top, the
  drawn skin is up to **1.96 mm** (right) / **1.99 mm** (left) from ufbx's full skinning, in the fist
  (frame 500); 0.27 mm with the wrist bent (frame 160); 0 at frame 0. [V: T4]
- **The file's normals.** Each hand's stored normals are its surface normals under one rigid rotation (best
  fit mean cosine 0.9988; Hand_Low's is 46.8° about the file's up axis) and face the surface by only 0.72
  (Hand_Low) and 0.20 (Hand_Low1). The loader uses the surface's own normals instead (C14). [V: a
  least-squares fit in the probe; T7]
- **Tangents, binormals, edge smoothing.** The engine's vertex has no tangent; the file's normals are by
  control point (smooth), and so are the surface normals used.
- **Embedded textures** (C2). The normal and roughness maps exist only inside the FBX; the engine draws the
  base colour only (cosmetic, waits for the owner).
- Blend shapes: the file has none; a file that had them would load without them, and the log says so (E8).
- More than one skin on a mesh, a mesh on more than one node: refused with a message.

## Acceptance: `tools/fbxload_test/run.sh`

Engine-free: `fbx_skinned.cpp` + ufbx + `vr_handpose_math.h`, against `vrealms_iwad/models/hands/hand_final.fbx`.
Then `poses_ref.py` (tools/fbx_hands/poses.py's own FBX reader, no ufbx) and `compare.py`.
`FBXLOAD_RENDER=1` adds software renders (`render.py`) of the loader's meshes, rest and posed.

Results, 2026-11-13 [V: run.sh, 0 failures]:

| Check | Right (Hand_Low) | Left (Hand_Low1) |
|---|---|---|
| T1 joints, FBX names, parents first, own skeleton | 22, under GLOBAL_MAIN_CONTROL_R | 22, under GLOBAL_MAIN_CONTROL_R1 |
| T1 no mesh named | refused, lists Hand_Low Hand_Low1 | |
| T2 take | Take 001, 1,279 frames (0-1278) at 60 fps, keys 0-21.3 s | same |
| T3 engine composition vs ufbx world, rest and all 1,279 frames | 6.7e-16 / 4.0e-15 file units | 5.6e-16 / 3.6e-15 |
| T4 every weight through the engine's palette vs ufbx | 6.7e-6 mm (float vertices) | 6.7e-6 mm |
| T4 the engine's 4 byte weights vs ufbx, worst | 1.964 mm (frame 500) | 1.985 mm (frame 500) |
| T5 wrist to middle fingertip (mesh) | 1.712605 file units -> Scale 0.11094212 = 0.19 m | same |
| T6 handedness (fist side; thumb side) | RIGHT (+0.598; +0.782) | LEFT (-0.396; -0.782) |
| T6 rest determinant | +1 | -1 (the mirror kept) |
| T7 faces/normals agree, rest and frame 160 | 3330/3330, 3330/3330 | 3330/3330, 3330/3330 |
| T8 vr_handpose ModelSeat on its own side | palm toward the curl (+0.665), det -1 | (+0.472), det -1 |
| compare.py: 21 joints below the root, 11 frames | 0.0000° | 0.0000° |
| compare.py: Root_joint at control-keyed frames 0, 845, 1278 | 0.00000 units, 0.0000° | same |

Between control keys Root_joint differs from poses.py by up to 0.087 units: poses.py reads the controls'
sparse cubic keys linearly; ufbx evaluates the cubic. The 21 joints below it are baked every frame and agree
exactly.

**Frame count.** The owner's figure is 1,278 frames (the 440 joint curves: 1,278 keys each, 0-21.2833 s).
The three controls above each skeleton (`GLOBAL_MAIN_CONTROL_R[1]`, `GLOBAL_cntrl`) are keyed to 21.3 s,
one frame further, and they move the hand, so the model has **1,279** frames: in frame 1278 the joints hold
frame 1277's values and the controls are at their last key. [V: probe of every curve's key range]

**Frame 0 = rest.** The scene as stored and the take at 0 s agree to 2.5e-8 in every joint's skin transform
[V: probe], so frame 0 draws the rest pose: every finger joint at its bind rotation, the controls where the
take starts them.

## Seen, outside the engine

`render.py` (a software rasteriser, not the engine's renderer) on the loader's own output:
- Hand_Low at rest: a right hand, textured correctly (nails on the nails, palm lines on the palm).
- Hand_Low1 at rest: a left hand, faces outward; **with lambert1_Base_Color.png on its own UVs the texture is
  wrong** (a nail on the palm, seams across the palm). Its UVs are Hand_Low's mirrored (u -> 1 - u, to 0.0028)
  and the FBX gives it no texture (blinn1, plain 0.5 grey, diffuse 0.8).
- Frames 500 (both fists), 1100 (right finger gun), 845 (left half curl) look like the take.

## Open questions for the owner

1. **The pose targets.** vr_handpose drove the fingers toward five baked frames of the old converter's
   files (open, point, fist, grip_tu, ready_td = frames 0 1 3 7 8). The take has 1,279 recorded frames and no
   such map. Which take frames (per hand: its own curves) are open, point, fist, grip with the thumb up, and
   ready? The take has no one-finger point and no thumbs-up (docs/FBX_HANDS_REPORT.md). Until then the
   fingers are not posed and the log says "pose targets not mapped yet".
2. **The left hand's look.** The FBX's left hand has no texture and mirrored UVs. Draw it with the skin
   texture as is (wrong islands, now), plain grey like the FBX, or something else?
3. **The take's frame count**: 1,279 (what moves the hand) rather than 1,278 (the joints' own keys). Fine?
4. **The file's normals** are stale; the surface's own are drawn. Fine?
5. **The 5th weight** (up to 2 mm in the fist) is the engine vertex format's limit. Fine, or widen the format?

## The run on the owner's PC (not done)

1. Pull, build (MSVC; the new library is `libraries/ufbx`), `python tools/rothdiff/packiwad.py`.
2. Run with `-Exec tools/claude_bridge/cfg/hands_flat.cfg` through the bridge's `boottest`.
3. The log must show, per hand, `FBX model models/hands/hand_final.fbx:Hand_Low -- mesh Hand_Low, 22 joints,
   6690 vertices, 3330 triangles, ...; 'Take 001' frames 0-1278 (1279 at 60 fps ...)` and the rest line
   (`skin det +1` / `-1 (mirrored)`, `normals from the surface`), then `VR hands: ... fingers NOT posed: pose
   targets not mapped yet ..., seat measured`.
4. The captures `hands_fbx_open_t0/t90/t180.png` and `hands_fbx_fist_t0.png` (the fist must equal the open
   shot: fingers not posed) against `tools/fbxload_test` renders.
