# ROTH_CONVERSION_PLAN.md
### Realms of the Haunting → REMAROTH: what needs converting, and how feasible it actually is

*Written 2026-09-25, at the point REMAROTH was forked from REMA (`UZDXREMA` @ `216ed49fcb`) to do
this work in isolation. Source-format claims below come from direct research against the local
`roth-editor` clone (see VRealms' `docs/05_export_pipeline_spec.md` for full detail and file:line
references). GZDoom/UDMF-side claims come from general Doom-engine architecture knowledge, **not**
yet verified against REMAROTH's actual current source — check before treating them as fact, same
caution this fork's own `VR_INTERACTION_PLAN.md`/`CAPABILITY_MANIFEST.md` need for "state of the
tree" claims.*

## Ground rule, unchanged from VRealms

Nothing original gets bundled or redistributed — no assets, no derived data, no reconstructed
source. Everything here runs locally against the player's own legit ROTH install. `ROTH.C` is a
reference for behavior, never shipped code.

## Why this is more tractable than the Unity path was

The old plan (Unity + HDRP) needed us to invent 2.5D-sector-to-arbitrary-3D-mesh extrusion from
scratch — genuinely risky, unproven logic. GZDoom doesn't have that problem, because Doom's native
map model is close to the same paradigm ROTH already uses, not a different one:

- ROTH's two-sided "sister" faces split into **lower/mid/upper wall quads** based on the height
  difference with the neighboring sector. GZDoom sidedefs have **top/mid/bottom textures** for
  exactly this case. Same mechanic, not an analogy.
- ROTH sectors store no explicit polygon — it's reconstructed from face endpoints via convex hull,
  which was flagged as a real risk for concave rooms in the original research. GZDoom sectors are
  defined implicitly by whatever closed loop of linedefs bounds them, and GZDoom's own node builder
  handles arbitrary (including concave) polygons natively — BSP exists specifically for this. **We
  don't need convex-hull reconstruction on this target at all** — emit each ROTH face as a linedef
  and let the engine's own node builder do what it already does for every other map.
- ROTH textures are 8-bit palette-indexed. Doom's whole art pipeline is natively palette-based.
  Preserving the "chunky 90s look" (explicit art-direction call from the original research) is
  *more* natural here than it would have been forcing a modern PBR/linear-color pipeline to fake it.

## What needs converting, piece by piece

### 1. Level geometry — feasible, high confidence
ROTH sector fields (floor/ceiling height, texture index + fit/shift, lighting) map onto GZDoom
UDMF sector fields close to 1:1. ROTH faces map onto linedefs+sidedefs as described above.
**Unresolved and important:** the ROTH-world-unit → Doom-map-unit scale factor. Nail this down
against real Study geometry before anything else — every VR proportion (reach, ceiling height,
door width) depends on it.
**Needs a spike:** ROTH's mid-platforms (a second floating floor/ceiling pair inside a sector's
footprint — elevated walkways) have no single obvious GZDoom equivalent. Closest candidate is
GZDoom's 3D floors. Prototype on one real example before assuming it generalizes.

### 2. Textures — feasible, arguably easier than the Unity path
roth-editor already fully decodes `.DAS` to real pixel + palette data (`Utility.convert_palette_image`).
PNG (or Doom-native patch) export from that is described in the original research as "missing
plumbing, not a hard problem." Point-filtering/no-compression requirements still apply but are
simpler to guarantee by default in a palette-native renderer than in a modern PBR one.

### 3. Objects — mixed, but bounded
Billboard sprites are a first-class Doom primitive (sprites are billboards by default) — an
arguably better native fit than Unity ever offered. True 3D objects (ROTH's `OBJECT_DATA` bit —
real vertex/face lists with per-face textures) need export into whatever format REMAROTH's
existing MODELDEF pipeline consumes — a real but bounded task, not a new problem (this fork
already has an active model pipeline; see `RS_WorldHands`' `MODELDEF.txt`). Index resolution
(`textureSource` indirection into the level's own DAS vs. the shared ademo pack) is fully mapped
already in the original research, purely mechanical to port.

### 4. Triggers / commands — the hardest piece, same difficulty on any engine
ROTH's command system is a linked list of opcodes across 15 trigger categories. roth-editor
exposes the *structure* but not the *meaning* — full opcode semantics live only in the
`beyond-the-ire` decomp docs, not yet read closely. This has to become real ZScript interpretation
logic, ported from `ROTH.C`'s actual behavior, regardless of which engine we'd picked. Not made
easier or harder by the GZDoom decision — just still ahead of us.

### 5. FMV / movies — medium
`ROTH.C` already has a working GDV decoder (verified, per the VRealms overview docs). Porting that
decode logic into a ZScript/engine-level playback path is the task. Worth checking what native
video/cutscene hooks this fork already has before assuming a from-scratch build.

### 6. Inventory, interactables, Rebecca hardpoints — downstream, correctly deprioritized
Pointless to build before geometry is proven, as already decided. Good news for when we get there:
`RS_HardPoints`, `RS_WorldHands`, `RS_VR_Reload`/`RS_PSReload` already exist as real, substantial
systems in this ecosystem — likely direct reuse or close analogs for what VRealms' design docs
(player IK/holsters, Rebecca hardpoints, shelf inventory) called for. Not yet verified by reading
their actual code — that's the next research task, only once geometry is proven.

## Recommended sequencing

1. Nail the scale factor against real Study data.
2. Convert the Study's geometry + textures alone, emitting something independently inspectable
   (real UDMF text, opened in Ultimate Doom Builder) — prove the translation is correct before any
   native in-engine RAW/DAS loader is written, so translation bugs and rendering bugs are never
   debugged at the same time.
3. Only after that's proven: trigger/opcode semantics, then objects, then movies.
4. Loop in `RS_HardPoints`/`RS_WorldHands`/`RS_VR_Reload` for interactables/inventory once there's
   a real room to hang them in.
5. Backport anything broadly useful from REMAROTH to REMA selectively, via the `rema` remote, once
   proven — never an obligation, never automatic.

## What's actually resolved vs. still open

**Resolved by the engine switch:** the concave-sector risk (GZDoom's node builder handles it
natively — no convex-hull reconstruction needed at all).

**Still open, unaffected by the engine switch:** mid-platform representation, trigger opcode
semantics, exact extent of RS_* reuse.

---

## FINDINGS — first extraction run, 2026-09-25

Parser built and run successfully against real game data. `roth_pipeline/parse_raw.py` is a Python
port of roth-editor's `raw.gd`+`parser.gd`, **not** the headless-Godot approach the original spec
proposed — Godot isn't installed on this machine and the format turned out to be a simple sequential
typed-field read, so a dependency-free Python port was the cheaper path. Python 3.12 is present.
All four STUDY maps parse; JSON in `roth_pipeline/out/`.

### Scale factor — effectively ANSWERED, and favourably
`mapMetadataSection` carries it directly, identical across all four maps:
`playerHeight=72`, `maxClimb=32`, `minFit=48`, `moveSpeed=5`.
A 72-unit player at ~1 unit ≈ 1 inch is a **6-foot person** — i.e. ROTH is authored at roughly the
same unit convention Doom uses, but with a realistically-proportioned player instead of Doom's
56-unit one. **ROTH→Doom map units is plausibly near 1:1**, and ROTH's native scale is *better* for
VR than Doom's own. Still to confirm against REMAROTH's actual VR units-per-metre calibration
(see the `216ed49fcb` "one model unit is one map unit everywhere" work) before locking it in.

### Geometry is structurally UDMF-ready — verified, not assumed
Checked every sector's face list for loop closure (each vertex used an even number of times):
**1,648 / 1,648 sectors across all four maps form closed loops. Zero open, zero empty.**
This both proves the parse is correct (misalignment would shatter these loops) and confirms the
sector→linedef-loop mapping Doom requires works directly on this data.

### Correction: the spec's "one DAS per level" claim is WRONG — and map→pack is SOLVED
There is no `STUDY1.DAS`. The install ships only **six** texture packs total — `ADEMO.DAS`,
`DEMO.DAS`, `DEMO1-4.DAS` — for **44** `.RAW` maps.

**Resolution mechanism found: `ROTH.RES`**, a plain-text manifest in the game root. Format:
```
version="Roth Version F1.4"
snd=data\fxscript.sfx
das2=m\ademo            <- global shared sprite pack for ALL maps
maps {
m\study1 m\demo         <- <map> <daspack> pairs
m\study2 m\demo
...
}
```
**All four STUDY maps use `DEMO.DAS`.** `ADEMO.DAS` is the global `das2` pack. The manifest lists
45 maps, 6 of which have no `.RAW` on disk (`tower1`, `tgate1f-i`, `temple1`, `vicar`, `vicar1`) —
cut content, worth a look later. Parsed by `roth.gd:118-150` if a reference is needed.

### Texture extraction WORKS — `roth_pipeline/extract_das.py`
`DEMO.DAS`: 4382 FAT entries, of which **744 are plain images** (the wall/floor/ceiling set) —
93 at 256x256 plus the usual 32/64/128 squares — with an **embedded palette**. Remainder: 3549
empty, 57 animated, 16 3D-objects, 12 image-packs, 4 directional refs.
`ADEMO.DAS`: 778 entries, 181 plain / 329 animated / 27 monster refs, and it uses the **default
palette** (`palette_offset == 0`) — the `DEFAULT_RAW_PALETTE` fallback from `das.gd:67` is **not
yet implemented** in our extractor. Needed before sprites/monsters can be pulled.

Output format: **indexed-colour PNG (type 3 + PLTE)**, so the original palette survives
byte-for-byte — no upscaling, no resampling, and GZDoom ingests it directly. Index 0 is written
transparent unless `PALETTE_ZERO_OPAQUE` is set (that inference should be double-checked against
real sprite edges).

**Verified visually**, not just structurally: extracted images are coherent ROTH artwork with
correct colours and no row-stride skew. Independent cross-check — the DAS header reports
`sky_index = 0`, and image index 0 decodes to a cloud sky.

### Correction: `STUDY1-4` are maps, not "the Study room"
507 / 629 / 276 / 236 sectors respectively, spanning ~10,700 units ≈ **270 metres** across. Each
file is a wing of the mansion, not a room. The Study *as a room* is a subset of sectors inside one
of them — identifying that subset is a prerequisite for the "self-contained room" PoC.
`roth_pipeline/render_floorplan.py` renders labelled top-down SVG floorplans for exactly this.

### UDMF generation WORKS — `roth_pipeline/build_map.py`
Produces a GZDoom-loadable PK3 (`maps/<NAME>.wad` holding a UDMF `TEXTMAP`, plus `TEXTURES.txt`
and the PNGs). STUDY1 output: 1076 vertices, **1608 linedefs**, 2454 sidedefs, 507 sectors,
411 KB TEXTMAP, 4.7 MB PK3.

**The critical structural translation — sister-face merging.** ROTH stores a two-sided wall as
TWO faces pointing at each other via `sisterFaceIndex`, one owned by each neighbouring sector.
Doom stores the same wall as ONE linedef with two sidedefs. So sister pairs must be **merged**;
emitting both would leave duplicate overlapping linedefs and wreck node building. Arithmetic
confirms it: 2454 faces − 846 sister pairs = 1608 linedefs, with 2454 sidedefs (one per original
face). Nodes are deliberately not precomputed — GZDoom builds them on load.

**Texture index space verified.** ROTH texture indices index the DAS FAT **directly** — of 297
distinct indices STUDY1 references, 288 hit an extracted plain image. The 9 misses are
animated/image-pack entries we don't decode yet. No offset or remapping needed.

**Sentinels and sky — context matters.** Per `face.gd:150-180`:
- `index == sky_index` renders **transparent**, and its Doom equivalent *depends on the surface*:
  on a wall upper/lower that's the ordinary "no height step against my neighbour" case → `-`;
  on a ceiling it means open-to-sky → `F_SKY1`. Getting this wrong one way puts 1577 bogus sky
  surfaces in an indoor mansion; the correct split yields **29** sky ceilings and 3143 empty wall
  slots, which matches a real interior level.
- `index == 65535` → `palette[255]`; `index >= 32768` → `palette[index - 32768]`. These are flat
  colours, generated as solid 64x64 indexed PNGs (`PAL###`). STUDY1 uses only 7 distinct ones.
- Bonus semantic spotted, not yet handled: `sector.floorTriggerID == 65534` marks a sector as
  transparent.

### Structural validation of the generated map
Zero zero-length lines, zero over-duplicated lines, zero out-of-range references. **All 507
sectors wind identically** (unanimous), so ROTH uses one consistent convention and front/back
sidedef assignment is either already right or needs exactly one global flip — determinable in a
single test run.

### Known gaps before it will *look* right (as opposed to *load*)
- **Texture alignment is not implemented at all.** ROTH's `textureFit`, per-axis shift bytes and
  the procedural UV math (`face.gd:130-358`, `sector.gd:438-644`) are ignored; walls will be
  misaligned until ported.
- **Objects are not placed.** Only the player start is emitted; STUDY1's 274 objects are absent.
- **Mid-platforms ignored** (16 in STUDY1) — needs GZDoom 3D floors.
- **Light levels** are a raw byte copy, unverified against ROTH's actual lighting model.
- **Winding** unconfirmed until first render.
- **ADEMO.DAS default-palette fallback** still unimplemented, so sprites/monsters can't be pulled.

---

## TEXTURE PLACEMENT — what the two investigations actually settled

Two independent reads were commissioned: one of `roth-editor` (a Godot reimplementation) and one of
`ROTH.C` (a reconstruction of the original 1996 engine). **They contradicted each other twice, and
in both cases the original engine won — roth-editor is approximating.** Do not treat roth-editor as
ground truth for texturing.

### CONFIRMED IN-GAME: wall textures are stored rotated 90 degrees
ROTH's along-wall coordinate indexes texture **rows**, not columns (`renderer.c:4732-4736` — the
per-column value is multiplied by the row stride). Transposing every wall texture produced a large,
immediately visible improvement when A/B'd against the original running in DOSBox. **Flats are not
rotated.** This is now the default (`ROTATE_WALLS`, `png_transpose()`); `--no-rotate-walls` exists
only to re-run the comparison.

### CONFIRMED: two world units per texture pixel
Both investigations agree, and the arithmetic checks out: a wall of length L with a texture W pixels
wide tiles `L / (2W)` times, which Doom reproduces exactly at scale 0.5. Applies to walls and flats
alike. **Do not try to "fix" an apparent wall/floor size mismatch by making these differ** — that
was tried and made things worse. Apparent differences in a screenshot are dominated by perspective.

### CONFIRMED: `textureFit` is TWO NIBBLES, not four ratios — and we still ignore it
High nibble = horizontal repeats `(1+h)`, low nibble = vertical repeats `(1+l)`, integers 1..16 per
axis, one shared byte for walls and flats (`renderer.c:3700-3706` flats, `:3345-3359` walls). A byte
of 0 means no scaling *and* disables wrapping. Crucially these are **repeats across the whole
surface**, not a per-unit rate — a long wall gets the texture *stretched*, not tiled more.

roth-editor reads only bits 2-5 and models four fixed ratios, so it mis-renders every sector whose
fit byte sets bit 6 — about 600 across the retail maps. Real data: 27 distinct byte values, high
nibble 0-6, low nibble 0-14. **Not yet implemented in our pipeline. This is the largest known gap.**

Caveat from the second read: the nibbles apply only on the *computed-extent* path; on the
*stored-extent* path the nibble byte is never consulted.

### CONFIRMED: walls carry a stored texture extent that can defy geometry
ROTH stores a 15-bit horizontal extent per face (`unk0x00` plus the low 7 bits of `type`). The
original engine has a dedicated stored-extents path (`renderer.c:13334-13341`), and roth-editor
exposes the value as a hand-editable field independent of geometry — so retail maps can and do
carry values that deliberately stretch or squash a wall's texture.

Doom always derives wall tiling from the measured wall length, so this must be converted:
`scalex = stored / (2 * wall_length)`. Note the texture width cancels, and when the stored value
equals the true length this reduces to 0.5 — our current default. **So this only deviates where an
author actually authored a deviation.** Helper written (`wall_u_repeats()`), not yet wired in.

### UNRESOLVED: are floors world-anchored or fitted per sector?
The single most consequential open question. The renderer's normal floor builder
(`build_floorceil_vertex_records`, `renderer.c:3726-3739`) assigns texcoords from four
dimension-derived constants by **vertex-ring parity**, never touching world position — which means
one stretched copy per sector polygon, and adjacent sectors NOT lining up. That is the opposite of
how Doom flats work.

But a third builder (`variant1000`, `~3766-3791`, gated on `flags & 0x1000`) reads authored
per-vertex texcoords, and no writer of that bit was found inside `renderer.c`. If map loading
precomputes per-vertex UVs from world coordinates, world-anchoring arrives by that route after all.
**Investigation of the map-loading code is outstanding.** We currently implement world-anchored
(Doom's native behaviour); if that is wrong, every floor and ceiling in all 44 maps is wrong, and
Doom flats cannot express the alternative at all — it would need new engine code.

### Object counts (interactable density, for PoC planning)
STUDY1: 274 objects / 661 commands / 224 command entry points.
STUDY2: 294 / 273 / 100.  STUDY3: 71 / 176 / 66.  STUDY4: 157 / 99 / 45.

---

## WHOLE-GAME SURVEY — `roth_pipeline/survey_all.py`

Run it after *any* pipeline change; it is the regression harness. It batch-parses all 44 maps and
counts everything unhandled rather than dropping it silently, so high coverage on one map can't
mask zero coverage on another.

### Result: the parser generalises across the entire game
```
parsed OK: 44/44   failed: 0
UNCLOSED sector loops across ALL maps: 0
totals: 16,906 sectors | 82,210 faces | 5,324 objects | 5,531 commands | 2,299 mid-platforms
```
16,906 sectors, every one a closed loop. The whole game — not just the mansion — is structurally
convertible to Doom sector/linedef form. This retires the project's single largest technical risk.

Also: `TOWER1`, `TGATE1F-I`, `TEMPLE1`, `VICAR`, `VICAR1` are **not** missing or cut (an earlier
`find` was simply truncated at 40 results). All 44 `.RAW` files present and parsing.

### Variance that a Study-only pipeline would have gotten wrong

**Scale is NOT universal.** `playerHeight` is 72 on 41 maps but **64** on three: `ABAGATE2`,
`AQUA1`, `DOPPLE`. Hardcoding 72 would leave those three subtly mis-scaled and near-impossible to
diagnose later. Scale must be read per-map from `mapMetadataSection`, never assumed.

**Mid-platforms are core, not an edge case.** 2,299 game-wide. `TOWER1` has 338, `TGATE1F` 251,
`TGATE1H` 211, `TGATE1G` 121. GZDoom 3D-floor work is required, not optional.

**Flat-colour share swings enormously** — 9% (`LRINTH1`), 11% (`LRINTH`) up to **55%**
(`TGATE1G`), 52% (`TOWER1`), 51% (`TGATE1H`). The tower/gate realms are predominantly solid
colour rather than textured, so the `PAL###` generation path is load-bearing for those maps.

**Extents range 2,688 (`MAS6`) to 64,704 (`RAQUIA2`).** RAQUIA2 is close enough to the classic
16-bit map-coordinate limit (±32767) to warrant a check; UDMF/GZDoom use floats so this is
probably fine, but confirm rather than assume.

**Extended texture mappings** (`type >= 0x80`, carrying extra shift metadata that alignment
depends on) range 6 to 544 per map — `CHURCH1` 544, `RAQUIA2` 423, `STUDY2` 412, `STUDY1` 383.

### Architectural consequence: namespace textures per pack
Pack usage: `DEMO1` x15, `DEMO4` x11, `DEMO2` x7, `DEMO` x7, `DEMO3` x4. `TEX0001` from `DEMO` and
from `DEMO3` are **different images**, so texture names must encode their source pack before more
than one map is built. (Differing palettes between packs are already handled for free — each PNG
carries its own PLTE.)
