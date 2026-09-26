# ROTH_NATIVE_PLAN.md
### Make REMAROTH read Realms of the Haunting's files natively — 2026-09-25

**Decision:** the engine loads ROTH's own `.RAW` maps and `.DAS` art straight from the player's
install and follows ROTH's rules. No Python converter, no `.pk3`, no Doom data in between.

Full evidence, with file:line references, is in the three companion files:
- `NATIVE_engine_scope.md` — where REMAROTH loads and draws its world, and where ROTH hooks in
- `NATIVE_rothc_rules.md` — the original engine's exact rules, read from ROTH.C (ground truth)
- `NATIVE_rotheditor_rules.md` — every ROTH file format roth-editor reads, and how it draws them

Where ROTH.C and roth-editor disagree, **ROTH.C wins** (it is the original engine; roth-editor
approximates in several places, listed at the end of its report).

---

## Corrections to ROTH_CONVERSION_PLAN.md (from ROTH.C)

1. **The player is 144 units tall, not 72.** The original engine doubles `playerHeight`,
   `maxClimb` and `minFit` at load (player 144 tall, radius 28, step 65, min gap 96). The converter
   sized the player at 72, so the world looked twice too big — very likely the "doorways three
   times a person's height" problem. Do not rescale the world; size the player correctly.
2. **Floors are world-anchored, not stretched per sector.** The per-sector builder the plan found
   (`build_floorceil_vertex_records`, the 0x1000 flag) draws 3D-object faces, not world floors.
   World floors tile across the world like Doom flats. roth-editor agrees.
3. **`textureFit` nibbles do not apply to world walls or floors.** They belong to 3D-object faces.
   Floors use a 2-bit scale in the sector flags byte: `2^s` world units per texel
   (ceiling bits 2-3, floor bits 4-5). Walls use the stored-extent path.
4. The extended face texture record's stored extent is **12 bits**, not 15.

## What already fits the engine without renderer changes

- Wall stored extent → `side->TexelLength`, set after `FinishLoadingLineDef`.
- Wall 2-units-per-texel, shifts, flips → per-part scale and offset on `side_t`.
- Floors: world-anchored per-sector scale/offset/flip → existing sector plane transform.
- Sprites: `Scale 2`; hanging sprites via texture offsets; wall-mounted via `RF_WALLSPRITE`;
  art assigned by texture ID (`picnum`) with no sprite-name lumps.
- Node builder and blockmap already run automatically when a map has none.

## What needs new engine code

| # | Piece | Size | Notes |
|---|---|---|---|
| 1 | **Native map loader** | Medium | Third format branch next to UDMF/binary. Reuses the dead Build-map hook (`P_IsBuildMap`, p_openmap.cpp:37). Modelled on the UDMF loader. Reads `ROTH.RES` to pair each map with its DAS pack. |
| 2 | **DAS texture source** | Medium | Register 8-bit palette images as engine textures in memory, rotating wall art back once at load. Palette-colour surfaces as generated 1-colour textures. |
| 3 | **Objects** | Medium | Sprites, hanging, fixed-angle, 3D mesh props with per-face textures, collision sizes from the DAS table. |
| 4 | **Mid-platforms** | Medium | As 3D floors created directly by the loader (one per sector that has one). |
| 5 | **Directional sprites / monsters** | Small-medium | ROTH's own frame-selection formula (in `NATIVE_rothc_rules.md` §5). |
| 6 | **Doors** | Medium | Door sectors are marked by `+0x14` = 0xFFFD/E/F; logic in ROTH.C `doors.c`. |
| 7 | **Lighting** | Medium | Sector light, candle sectors, lightning. roth-editor has none; ROTH.C has the rules. |
| 8 | **Commands / triggers** | Large | Port `raw_commands.c`. The biggest remaining job. |
| 9 | **Stop needing doom2.wad** | Small | A minimal base game definition so the engine boots without a Doom IWAD. |

Items 1-3 together replace the Python pipeline for geometry, textures and props.

## Open questions to settle by A/B against DOSBox

- Floor texture axis signs (ROTH maps world X to image rows).
- 256x256 opaque floor textures may draw at twice the density.
- Static value of the IMAGE_FIT repeat byte for world faces (presumably 0).
