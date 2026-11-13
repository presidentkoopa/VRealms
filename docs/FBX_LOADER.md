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
| C1 | Load from memory (the lump's bytes) | `ufbx_load_memory` h:5291 | fs:359 | done |
| C2 | Embedded blobs KEPT (2026-11-13: the material's normal and roughness maps exist only inside the FBX; C5b reads them). Measured cost: the test loads both hands in 1.29 s instead of 1.04 s | `ignore_embedded` h:4700 | fs:354 | done |
| C3 | Right-handed Z up, the IQM file convention; units untouched (size is MODELDEF Scale's) | `target_axes` h:4841, `ufbx_axes_right_handed_z_up` h:5264, `space_conversion` h:4819 (TRANSFORM_ROOT) | fs:356-357 | done [V: T3, compare.py root positions in the file's Y-up] |
| C4 | Pick the skinned mesh by its node's name, case-insensitive; refuse an unnamed pick when there are several | `ufbx_mesh.instances`, `skin_deformers` | fs:369-400; selector fs:155-181 | done [V: T1] |
| C5 | The material (the mesh's own, or the mirrorskin twin's, C21), its base-colour texture file name, and the scalars metalness/roughness | `ufbx_material.pbr` h:2546ff, `ufbx_texture.relative_filename` h:2911 | fs:402-419, 186-227 | done [V: S5] |
| C5b | Every map the material links -- base colour, normal (aiStandardSurface `Maya\|normalCamera`), roughness (`Maya\|specularRoughness`), metalness, AO -- with the property it hangs on and its embedded bytes, byte for byte | `ufbx_material.pbr.normal_map/roughness/...`, `ufbx_material.textures[].material_prop`, `ufbx_texture.content` | fs:198-227 | done [V: S5 against ufbx's own scene; maps_ref.py M1 against fbxread.py's Video content, no ufbx] |
| C6 | Skeleton = the nodes the skin's clusters bind to | `ufbx_skin_cluster.bone_node` h:2022 | fs:421-429 | done [V: T1, 22 joints] |
| C7 | Joints ordered parents first; parent = nearest ancestor that is a joint | scene tree, `ufbx_node.children` | fs:430-453 | done [V: T1] |
| C8 | Rest pose = the scene as stored | `ufbx_node.node_to_world` h:931 | fs:457-469 | done |
| C9 | The bind is one transform for every joint at rest (so a rest-based palette skins as the FBX does) | `ufbx_skin_cluster.geometry_to_bone` h:2025 | fs:471-486 | done [V: 1.1e-15; T4 every weight 6.7e-6 mm = float rounding] |
| C10 | Rest mesh = the file's own skin at rest, all weights, normalised | `ufbx_skin_deformer.vertices/weights` h:1982 | fs:488-529 | done [V: T4] |
| C11 | Root joint's local = its whole model-space transform (the controls above it: arm motion, the left hand's mirror) | `node_to_world` | fs:317-342 | done [V: T3] |
| C12 | Child local = parentWorld⁻¹ · world, decomposed (keeps any inherit mode ufbx resolved; Maya's segment scale compensate is `IGNORE_PARENT_SCALE` h:814, resolved at c:23026) | `ufbx_matrix_to_transform` h:5594, c:31862 (a negative determinant becomes one negative scale axis) | fs:331-339 | done [V: T3 4e-15] |
| C13 | Triangles in the file's polygon order, reversed only if inside out at rest (signed volume) | `ufbx_triangulate_face` h:5657 | fs:531-555 | done; the file's order already faces out on both hands [V: T7] |
| C14 | Normals: the file's, through the rest skin's inverse transpose, if they face the surface (mean cosine ≥ 0.95); otherwise the surface's own (area-weighted per control point) | `ufbx_matrix_for_normals` h:5586, c:31792 | fs:557-593 | done; **the file's normals fail it on both hands** (below) [V: T7] |
| C15 | 4 largest weights per vertex, renormalised, bytes summing to 255 | | fs:595-622 | done; **loss** (below) [V: T4] |
| C16 | One vertex per polygon corner; UV v flipped to texture rows (as IQM) | `vertex_uv` | fs:624-643 | done [V: render.py textured right hand]; under mirrorskin u is drawn as axis - u (C21) |
| C17 | Winding: (b−a)×(c−a) against the outward normal in file space, as the engine's IQMs | | fs:645-653 | done [V: T7 3330/3330; R: that this is the engine's IQM convention comes from the old converter's measurement on three Blender IQMs, `git show 864c0020:tools/fbx_hands/fbx2iqm.py` ~line 582] |
| C18 | Every take (anim stack), in file order | `ufbx_anim_stack` h:3090 | fs:655-716 | done [V: T2] |
| C19 | Frame span = the keys on the skeleton's curves and on every node above it; frames at the file's rate | `settings.frames_per_second` h:3915, `ufbx_anim_curve.keyframes` | fs:657-692 | done [V: T2, 1,279 frames] |
| C20 | Each frame evaluated by ufbx | `ufbx_evaluate_scene` h:5496, c:31186 | fs:694-707 | done [V: T3, compare.py] |
| C21 | MIRRORSKIN (owner, 2026-11-13: "the left hand reads the right hand's skin with the flip undone"), asked for by name in the MODELDEF (`Hand_Low1?mirrorskin=Hand_Low`), never guessed: the twin must have the same polygons over the same control points; each corner is matched to the twin's on the same polygon and control point; u_this + u_twin must be one constant on every corner and v equal (tolerance 1e-4 UV = 0.2 texel), else the load is REFUSED; then u is drawn as (that measured axis) - u and the twin's material is drawn | `ufbx_mesh.faces`, `vertex_indices`, `vertex_uv` | fs:229-303, 405-417, 634-637 | done [V: S1-S4; maps_ref.py M4 with fbxread.py, no ufbx: every one of the 6,690 left corners samples the right hand's UV at the matching point to 4.1e-8] |

### E: the model (models_fbx.cpp, IQMModel underneath)

| # | Part | IQM source it mirrors | Ours | Status |
|---|---|---|---|---|
| E1 | Joints: names, parents, rest local TRS, children, roots; Position/Rotation/Scaling from the rest world matrix (IQM's per-axis accumulation is not exact under a mirrored parent) | models_iqm.cpp:127-177 | mf:217-240 | done [R: the world-matrix choice; V: T3 for the TRS] |
| E2 | baseframe / inversebaseframe | models_iqm.cpp:201-226 | mf:242-266 | verbatim |
| E3 | TRSData, every frame of every take | models_iqm.cpp:228-257 | mf:268-278 | done |
| E4 | One animation per take, by name (`FindFrame("Take 001:500")`, `FindFirstFrame("Take 001")`) | models_iqm.cpp:188-199, 488-526 | mf:280-291 | done [R: not called in the engine yet] |
| E5 | One surface; its skin = the material's own texture beside the model; MODELDEF Skin overrides | models_iqm.cpp:108, 528-584 | mf:293-304 | done |
| E6 | Triangles | models_iqm.cpp:111-117 | mf:310-313 | done |
| E7 | Vertices stored (x, z, y) with normals, as LoadPosition/LoadNormal | models_iqm.cpp:352-407 | mf:315-327 | done |
| E8 | Two load log lines: what was read, what could not be carried | | mf:329-357 | done |
| E9 | Geometry rebuilt from Load's vertices, not by re-reading the file | models_iqm.cpp:298-313 | mf:363-376; mi: `LoadGeometry()` made virtual, members protected | done |
| E10 | The material's maps the FBX embeds, registered as engine textures `<lump>:<file>` from the embedded bytes (decoded on first use by fbx_image.h: the engine's stb_image, 16-bit channels rounded to 8), hung on the skin texture as the engine's material layers (`FGameTexture::SetShaderLayers`, what a GLDEFS `material` block sets) so it draws with the PBR material shader (SHADER_PBR needs normal + metallic + roughness + AO, hw_material.cpp:89); metalness = the material's scalar 0 as a constant texture, AO = none (1); a skin that already has a material (GLDEFS, materials/) keeps it | gldefs.cpp ParseMaterial, gametexture.h SetShaderLayers, hw_material.cpp FMaterial | mf:23-189, 306-308 | done [V: S5, maps_ref.py M1-M3, M5; R: not built, not seen drawn] |

### W: wiring

| # | Part | Where | Status |
|---|---|---|---|
| W1 | `"<file>.fbx:<mesh>"` in a model name: the file is the lump, the mesh after the colon; only names ending `.fbx` before the colon are split | mc:149-167, 191-194 | done [V: g++ -fsyntax-only] |
| W1b | Options after the mesh: `<mesh>?<option>,...`, only `mirrorskin=<mesh>` (fbx_skinned.h Selector); an unknown, empty or repeated option refuses the load. The whole name stays the model's cache key | fs:155-181, mc comment | done [V: S1] |
| W2 | ufbx as a static library; linked into the engine; include path | `libraries/ufbx/CMakeLists.txt`, `CMakeLists.txt` (add_subdirectory), `src/CMakeLists.txt` (PROJECT_LIBRARIES, sources, include) | done [R: not configured or built; no MSVC here] |
| W3 | MODELDEF: RealmsHandRight = `hand_final.fbx:Hand_Low`, RealmsHandLeft = `hand_final.fbx:Hand_Low1?mirrorskin=Hand_Low` (no Skin line: the material's skin and maps), Scale 0.11094212 | `vrealms_iwad/MODELDEF.txt` | done |
| W4 | The hand actors show the model sprites RHDR / RHDL | `wadsrc/static/zscript/actors/realmshands.zs` | done |
| W5 | vr_handpose: the fingers posed from each hand's own take, frame 0 (open) to 318 (fist), by UZDXREMA's curl rule (2026-11-13; the checklist is at the top of vr_handpose.cpp, the tests `tools/hands_test` and `tools/fbxload_test` test_fingers/measure_ref) | vh | done [V: tests, not yet seen in the engine] |
| W5b | The palm cup: HANDPALM_joint follows grip from frame 0 to frame 318 of its own take, smoothed as the fingers are, turned before them (owner, 2026-11-13; checklist 38 in vr_handpose.cpp) | vh, vr_handpose_math.h StepPalm/BuildPalm/ApplyPalmTurn | done [V: fbxload_test P0-P3, F5; not seen in the engine] |
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
- **Edge smoothing.** The file's normals are by control point (smooth), and so are the surface normals used.
- **Tangents.** None are needed: the engine's normal mapping builds the tangent frame per pixel from screen-space
  derivatives (main.fp `cotangent_frame`, Schueler's method), for walls and models alike, so a skinned, mirrored
  model is framed from what is drawn. [R: read in the shader]
- **What the normal and roughness maps change, and what they cannot.** In this engine a normal map bends the
  normal that DYNAMIC lights and effect lights read (material_pbr.fp ProcessMaterialLight; main.fp EffectLightsAt);
  the sector's light is not directional and does not see it. Under sector light alone the PBR shader differs from
  the default only by its Fresnel term on the ambient (kD = 1 - F, F from roughness and the view angle: a few per
  cent darker, more at grazing angles). [R: read in the shaders, not seen]
- **16 bits.** The normal map is 16-bit; the engine's textures are 8-bit RGBA, so it is drawn rounded to 8 bits
  (maps_ref.py M2: identical to libpng's decode rounded the same way).
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
- Hand_Low1 at rest: a left hand, faces outward. On its own UVs the skin lands wrong (a nail on the palm, seams
  across the palm: its UVs are Hand_Low's mirrored and blinn1 has no texture). With `?mirrorskin=Hand_Low`
  (2026-11-13) it wears the right hand's skin at the matching points: nails on the nails, palm lines on the palm
  (`rest.png`, `fingers/sheet_Hand_Low1.png`).
- `render.py --normal` (the two right-hand tiles of each row): the same views lit through the normal map, framed
  as the engine's shader frames it. Shows the wrist tendons, knuckle creases and nail beds; a software picture,
  not the engine's.
- Frames 500 (both fists), 1100 (right finger gun), 845 (left half curl) look like the take.

## Open questions for the owner

1. **The pose targets.** Answered 2026-11-13: the owner's fbx_poses.json "open" 1 and "fist" 319 (Blender), frames
   0 and 318 here, each hand from its own take; the curl rule blends them per finger (vr_handpose.cpp).
2. **The left hand's look.** Answered 2026-11-13: the right hand's skin with the flip undone (C21, mirrorskin).
   Measured: the file's flip is about u = 0.501416 (the UV islands' centre), not 0.5 -- u_left + u_right =
   1.002832 on every corner (spread 4e-8) -- so the loader un-flips about the measured axis; 1 - u would leave every
   point 0.0028 (5.8 texels) off.
3. **The take's frame count**: 1,279 (what moves the hand) rather than 1,278 (the joints' own keys). Fine?
4. **The file's normals** are stale; the surface's own are drawn. Fine?
5. **The 5th weight** (up to 2 mm in the fist) is the engine vertex format's limit. Fine, or widen the format?

## The run on the owner's PC (not done)

1. Pull, build (MSVC; the new library is `libraries/ufbx`), `python tools/rothdiff/packiwad.py`.
2. Run with `-Exec tools/claude_bridge/cfg/hands_flat.cfg` through the bridge's `boottest`.
3. The log must show, per hand, `FBX model models/hands/hand_final.fbx:Hand_Low -- mesh Hand_Low, 22 joints,
   6690 vertices, 3330 triangles, ...; 'Take 001' frames 0-1278 (1279 at 60 fps ...)` and the rest line
   (`skin det +1` / `-1 (mirrored)`, `normals from the surface`), then `VR hands: ... seat measured` and
   `VR hands: ...:Hand_Low as the right hand -- fingers posed from frames 0 (open) and 318 (fist) of its own take`
   (Hand_Low1: the left hand), and since 2026-11-13: `FBX model ...:Hand_Low1?mirrorskin=Hand_Low -- mirrorskin=Hand_Low
   ... about 1.002832`, `FBX model ...:Hand_Low -- material aiStandardSurface1 (of Hand_Low) on skin
   models/hands/lambert1_Base_Color.png, the engine's PBR material: normal lambert1_Normal_OpenGL.png (Maya|normalCamera,
   2048x2048, 3 channel(s) of 16 bits, ...)`, and `VR hands: ... palm cup: HANDPALM_joint follows grip from frame 0 to
   frame 318`.
4. The captures `hands_curl_<open|fist|point|thumbup|trigger>_t<0|90>.png` against the software renders
   `tools/fbxload_test` writes (`FBXLOAD_RENDER=1`: `fingers/sheet_Hand_Low.png`, `fingers/sheet_Hand_Low1.png`).
