# Item models from the item's own pictures: the local image-to-3D pipeline

2026-11-16. The owner's decision: "sprites are a no-go, we need a solution to sprites and we need it now". The held
and lying items (props later) become real 3D in VR. Voxels and hand-made meshes were rejected; the owner chose a
LOCAL open-source image-to-3D model on his PC (RTX 3080 Ti 12 GB, i9-9900KF, Windows).

**Marks.** **[V: how]** verified, and how. **[R]** reasoning only, not seen. Nothing in this file was run on the
owner's PC yet: no model was generated, nothing was drawn by the engine.

**Rule kept:** everything derived from retail art (pictures, meshes, contact sheets) is made on the owner's PC,
outside the repository, at run time. The only meshes in the repository are procedural test boxes
(`tools/img3d/placeholder/`, `make_placeholder.py`).

---

## 1. The candidates (web research, 2026-11-16)

| model | licence | shape + texture VRAM (upstream) | on 12 GB | Windows | texture |
|---|---|---|---|---|---|
| **Hunyuan3D-2 (2.0)**: DiT-v2-0 1.1B + Paint-v2-0 (turbo) 1.3B + Delight-v2-0 | Tencent Hunyuan 3D 2.0 Community | "6 GB VRAM for shape generation and 16 GB for shape and texture generation in total" [1] | yes, by stages + CPU offload: the Windows portable build says shape + texture needs "at least 8GB of VRAM" [5] | community builds and guides [5][6]; the two texture modules compile with CUDA 12.4 + VS 2022 [5][6] | baked RGB on a UV atlas, delight model removes baked lighting |
| Hunyuan3D-2mini (0.6B shape) | same | less | yes | same | same paint model; smaller, weaker shape [1] |
| Hunyuan3D-2.1: DiT-v2-1 3.0B + Paint-v2-1 (PBR) | same | "10 GB VRAM for shape generation, 21GB for texture generation and 29GB ... in total" [3] | texture only with third-party offloading (mmgp), users report OOM [4] | community | PBR (best), out of reach here |
| **TRELLIS.2-4B** (Microsoft) | MIT [7][10] | "at least 24GB", "tested only on Linux" [7] | community low-VRAM builds: "fit better into 8GB gpus" [8]; "12GB ... 512 resolution plus a tiled decoder" [9] | one-click community installer (Python 3.11, CUDA 12.8, Torch 2.8) [8] | PBR GLB from its O-Voxel decoder |

Sources: [1] github.com/Tencent/Hunyuan3D-2 README; [2] huggingface.co/tencent/Hunyuan3D-2/blob/main/LICENSE;
[3] huggingface.co/spaces/tencent/Hunyuan3D-2.1 README (refs/pr/15); [4] github.com/Tencent-Hunyuan/Hunyuan3D-2.1
issue 15; [5] github.com/yanwenkun/hunyuan3d-2-winportable (via awesome.ecosyste.ms); [6] codersera.com "Set Up
Hunyuan3D-2 on Windows"; [7] github.com/microsoft/TRELLIS.2 README (cdn.jsdelivr.net mirror); [8]
github.com/IgorAherne/TRELLIS.2-stableprojectorz; [9] book.st-hakky.com "trellis2 native vs legacy vram guide";
[10] huggingface.co/microsoft/TRELLIS.2-4B README; [11] tspi.at/2026/05/01/opensource3dassets.html (a local
Hunyuan3D run: "12-16 GB for single-image inference"; without the compiled texture modules "painfully - unusable -
slow"). Blocked while researching (not read): github.com/Tencent/Hunyuan3D-2/blob/main/gradio_app.py.

## 2. The choice

**Primary: Hunyuan3D-2 (2.0)**, shape `hunyuan3d-dit-v2-0`, texture `hunyuan3d-paint-v2-0-turbo` (falls back to
`hunyuan3d-paint-v2-0`) with `hunyuan3d-delight-v2-0`. Why:
1. It fits 12 GB the way upstream itself documents: shape alone is 6 GB [1]; texture under CPU offload (the gradio
   app's `--low_vram_mode` [1]); `generate.py` never holds both on the GPU (all shapes first, the shape model freed,
   then all textures). The Windows portable build runs shape + texture from 8 GB [5].
2. It gives what the engine needs in one go: a closed mesh, a UV atlas, one baked base-colour picture. The delight
   model takes out the lighting the 1996 renders baked in.
3. Windows is a beaten path: torch 2.5.1 + CUDA 12.4 [6]; the two native texture modules build with CUDA 12.4 and
   VS 2022 [5][6], and VS 2022 is the engine's own compiler on this PC.
4. 2.1's PBR texture needs 21 GB [3]: out. 2mini is the same paint model with a weaker shape: kept as a later option.

**The licence (must be read by the owner).** Hunyuan3D-2's licence "DOES NOT APPLY IN THE EUROPEAN UNION, UNITED
KINGDOM AND SOUTH KOREA"; the Territory is "the worldwide territory, excluding the territory of the European Union,
United Kingdom and South Korea" [2]. "Tencent claims no rights in Outputs You generate"; over 1 million monthly active
users needs Tencent's own licence; outputs may not be used to improve other AI models [2]. I do not know where the
owner is, so `SETUP_IMG3D.bat` prints the clause and installs nothing until he types AGREE.

**Fallback: TRELLIS.2-4B (MIT).** No territory clause and the stronger generator, but upstream wants Linux and 24 GB
[7]; on this PC only a community Windows build with low-VRAM changes runs it [8][9]. `generate.py --backend trellis2`
drives such a build through `trellis2_run.py` (README example's calls [7]; the `to_glb` arguments beyond
`decimation_target` / `texture_size` are as remembered [R], check that build's `example.py` first if it fails). Setup
is that build's own installer, then `IMG3D_TRELLIS_PYTHON` / `IMG3D_TRELLIS_CODE`. Per [9] it also needs DINOv3
fetched separately [R: whether that needs a Hugging Face access approval was not checked].

## 3. What the owner does

1. **Once:** double-click `tools\img3d\SETUP_IMG3D.bat`. It prints `[n/11]` per step (setup_img3d.py): licence; a
   Python 3.10-3.12 (`winget install -e --id Python.Python.3.10` if none); `nvidia-smi`; **nvcc 12.x** (if missing it
   stops and names the CUDA Toolkit 12.4.1 download); VS 2022 (vswhere); the venv `E:\DOOMWork\tools\img3d_env` with
   torch 2.5.1+cu124; Hunyuan3D-2 cloned into `E:\DOOMWork\tools\img3d_models\Hunyuan3D-2` (its commit written to
   `hunyuan3d2_commit.txt`), its requirements; the two texture modules compiled; spandrel + Real-ESRGAN x4plus
   weights; the Hunyuan weights (~20 GB) into `img3d_models\tencent\Hunyuan3D-2`; `img3d_bin\img3d_extract.exe`
   built with cl; the **self-test**: a red can drawn by the script (no game art) through shape and texture into
   `E:\DOOMWork\tools\img3d_work\selftest.glb`. Run it again to resume: finished steps are skipped.
2. **Then:** `tools\img3d\RUN_IMG3D.bat` (all chapter 1-2 items) or `RUN_IMG3D.bat 16 21` (some).
   Work: `E:\DOOMWork\tools\img3d_work\<item>\` (`views.png` every picture source with the chosen one framed;
   `input.png` what the model saw; `shape.glb`, `raw.glb`; a `choose.txt` with a file name, e.g. `film_004.tga`, picks
   another view next run). Models: `E:\DOOMWork\REMAROTH_local\items\<id>_<name>.glb` + `manifest.json`;
   `img3d_path.txt` is written beside every `build-dxr\*\doomxr.exe`, so the engine finds them with no setting.
   Each GLB opens in Windows' 3D Viewer.
3. In the game: the console `roth_img3d status` lists what was read; `roth_img3d reload` re-reads (a map reload
   rebinds).

## 4. The pipeline (tools/img3d)

| step | file | what |
|---|---|---|
| extract | `img3d_extract.cpp` (C++, the engine's readers), `extract.py` | per item: `icon.tga` (DBASE200 icon, STUDY1's palette, index 0 a hole), `world.tga` (the picture it lies as: render id -> `Pack::ResolveObjectArt`), `film_NNN.tga` (every close-up frame, the film's ground mask cut away, `tools/voxelize_parked` reader), `info.json`; the view: `choose.txt`, else the close-up frame with the most item pixels not touching the frame edge (any if all do) unless the icon has more, else icon, else world picture; cropped, transparent border filled with the nearest item colour, Real-ESRGAN x4 until >= 384 px, Lanczos to 768, soft alpha, centred on 1024x1024 RGBA. `fill_holes` (items.json, the Lamp): the film's mask cuts dark glass out as holes; holes the border cannot reach are made item again. |
| generate | `generate.py` | Hunyuan3D-2: shapes (50 steps, octree 380, seed 1234), floaters and degenerate faces removed, reduced to 20,000 faces, saved `shape.glb`; model freed; textures (paint + delight, CPU offload) -> `raw.glb`. `--selftest`. `--backend trellis2`. |
| post | `post.py`, `glbio.py` | one mesh (glbio's reader = the engine reader's rules); refused over the face budget or without a picture; items.json `front_yaw`; faces turned outward if the signed volume is negative; smooth vertex normals; picture opaque RGB <= 1024^2 PNG; longest side = items.json `metres` (`"size": "table"`) else 0.10 m (`"size": "picture"`: the engine sizes it); base on y = 0, centred; grip = `grip_frac` of the box + `grip_offset`; written, read back and compared; manifest updated. |
| all | `run.py`, `RUN_IMG3D.bat` | the three in order, a log `img3d_work\run_<time>.log`, `img3d_path.txt` beside the exes. |

**items.json** holds the default list (16 Matchbox, 15 Lamp, 21 Key, 17 Book, 1 Shards and Seals, 4 Statuette,
24 / 25 / 26 Mask, 9 Colt-45, 51 Colt-45 magazine [V: names from tools/item_dossier on a retail copy, 2026-11-16;
the "68" in the task is Adam Randall, the magazine is 51]) and per item the size, hold, grip and turn: ALL TUNING
NUMBERS, none seen in a headset.

## 5. The engine (VRealms)

Files: `src/roth/roth_img3d_core.{h,cpp}` (engine-free: manifest, GLB reader, the two placements),
`src/roth/roth_img3d.{h,cpp}` (folder, textures, binding, the two uses, `roth_img3d`), hooks in `roth_objects.cpp`,
`roth_actors_engine.cpp` (Dress), `roth_runtime.cpp` (repaint 0x0d / flip 0x02, `ObjectPickBox`), `roth_held.cpp`
(`ShowCard`), `model_memorymesh.{h,cpp}` (optional per-corner normals), `realmshands.zs` (`RealmsItemModel`).

- **Folder:** cvar `roth_img3d_dir` (flags 0: never archived, never in the shared ini), else `img3d_path.txt` beside
  the exe (read once). Relative paths are from the exe's folder.
- **Formats:** GLB 2.0 (every triangle primitive of the default scene, node transforms, POSITION / NORMAL /
  TEXCOORD_0, u8/u16/u32 or no indices, `extensionsRequired` refused and named; the first primitive's base colour,
  PNG or JPG, decoded by the engine's stb_image via `fbx_image.h`). Each item becomes FMemoryMeshModel meshes (the
  engine's in-memory model, `BindMemoryMesh`): one for the world, one per hand. MODELDEF/OBJ/MD3/IQM are not used:
  they read lumps from the game's archives, not a folder outside them.
- **Lying in the world:** an object whose render id names an item (`Dbase100::FindItemByRenderId`, the dossier's
  "placed in maps" rule) wears the world mesh: actor Scale = map units per metre (vertical / pixel stretch), the
  manifest's real size or the picture's longest side, base on the picture's bottom (never below the floor), front
  (+Z) along `ObjectYaw(rotation)` + `world_yaw`; a fixed-angle wall quad stands its back on the wall. Re-applied when
  the actor rules re-dress it or a command repaints it; given back when the new picture is not a modelled item.
  **The game still picks by the picture**: `ObjectPickBox` reads the kept picture sprite, scale and z (as it already
  did for the candles' per-wick pictures), so reach, touch and click are unchanged.
- **In a hand:** `roth_held`'s cards (item hand, palm, Lamp, Book; slots 0-4) become a `RealmsItemModel` actor. Holds
  `palm` / `fist`: FollowHandMode 3/4 (the controller's grip frame at draw rate, metres; X fingers, +Y back of the
  hand, thumb -Z right / +Z left), the grip point on the grip socket. `hang` and the cards placed in the world (the
  Lamp on its hook, the Book between the hands): upright at the card's point, facing the player, placed each tic.
- **Fallback:** no model, or any read error (named once in the console) -> the picture / the card, as before.
- **Limits** [R]: 3 meshes per item, each its own vertex buffer (<= 20k triangles each, 60k per item); one runtime
  sprite per binding per load (the sprite frame table's 0xffff ceiling is shared with the objects); one 1024^2 RGBA
  texture per item; opaque models are not back-face culled (`hw_models.cpp`), so winding does not matter; the normals
  are the mesh's own. Not replaced: directional and 3D-mesh objects (only plain picture objects), creatures, the drawn
  weapon (`RealmsHeld` / `RealmsWeapon<index>` keeps its own path: the Colt in hand as a weapon is unchanged, the Colt
  lying or as an item card is modelled). The three masks are placed by click records on wall objects whose render id
  is not the items' (item_dossier: placed 0): they get models in the hand, not on the wall. A height command (0x23)
  moves the actor from the record's z, not the model's base [R].

## 6. Tests

- `tools/img3d_test/build.sh` [V: run here, 2026-11-16]: the engine-free reader on procedural boxes
  (`img3d_test: 95 passed, 0 failed`, ASan/UBSan): manifest fields and 11 malformed cases, three boxes (counts,
  bounds, the PNG through the engine's own `fbx_image.h` decoder, the front's colour on +Z), a node TRS landing every
  vertex where the plain box has it, uint16 / no-index / lines-only / required-extension / short-BIN / bad-index /
  truncated / bad-magic files, the placements (proper turns, det +1, in both hands and both holds; front -> fingers;
  grip -> socket; grip yaw); `post_test.py` (post.py on an inward-wound, oversized, offset box: table size, base,
  centre, outward faces and normals, front picture on +Z, grip) and the C++ reader on post.py's output; the
  committed placeholder boxes byte-identical to `make_placeholder.py`'s output; `img3d_extract` builds and links
  with g++.
- `tools/roth_syntaxcheck.sh` (every src/roth file) and a syntax check of `model_memorymesh.cpp` [V: run here, all ok;
  not MSVC, not a link].
- The extractor and `extract.py` on a retail copy in this container's /tmp [V: 11 items, every source found;
  outputs looked at in /tmp only, never committed].
- **On the PC (the lead):** bridge `build`, then `run` with `-Exec tools/claude_bridge/cfg/img3d_flat.cfg`,
  `-TimeoutSec 240` (the committed boxes through `roth_img3d_dir ../../tools/img3d/placeholder`: the Book box on its
  table, the Matchbox box in the item hand, the Key box after `roth_give 21` / `roth_ui_select 21`, 20 cm views, the
  hands off). Read `doomxr-log.txt` for `[roth_img3d]` lines; the captures `img3d00`-`img3d05` in the bridge's logs.

## 7. A bridge verb (proposal; the bridge script was NOT edited)

`img3d` would let the lead run the generation remotely: run
`%IMG3D_ROOT%\img3d_env\Scripts\python.exe tools\img3d\run.py <items> [--steps extract,generate,post]` with only
digits, commas and `--steps <list>` accepted, the environment `RUN_IMG3D.bat` sets, output to `logs\img3d.log`.
Generation takes minutes per item, longer than a bridge request should block, so start it detached and add
`img3dstatus` (the tail of the newest `img3d_work\run_*.log`); the GLBs then reach the engine through
`img3d_path.txt` with no further step. A second verb `img3dsetup` is NOT proposed: setup asks the owner to accept a
licence.
