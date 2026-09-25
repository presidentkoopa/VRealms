# roth_pipeline

Converts *Realms of the Haunting*'s original files into levels this engine can
load. Reads the player's own game install; ships nothing.

## One command

```
python build.py --roth "D:/SteamLibrary/steamapps/common/Realms of the Haunting/ROTH" STUDY1
python build.py --roth "..." --all          # every map in the game
```

Output lands in `generated/pk3/ROTH_<MAP>.pk3`. Load that in the engine.

## Layout

```
roth_pipeline/
├── build.py          run this; it drives everything below
├── tools/            the converters — nothing here is generated
├── generated/        every output. SAFE TO DELETE; build.py rebuilds it
│   ├── maps/         one JSON per map, the parsed original data
│   ├── packs/        artwork extracted per texture pack, cached and reused
│   │   └── DEMO/
│   │       ├── textures/   PNGs + palette.json + meta.json
│   │       └── meshes/     OBJ models for 3D props + meshes.json
│   ├── floorplans/   top-down SVG per map, for finding your way around
│   └── pk3/          the loadable results
└── authored/         HAND-MADE DATA. A rebuild MUST NEVER TOUCH THIS.
```

### Why generated/ and authored/ are separate

Regenerating a map has to stay safe forever, or the project slides back into
hand-editing levels. So anything a human authors — grab points, mass and
physics properties, interaction tags, character positions — lives in
`authored/` and is merged in at build time, never written into files the
converter owns.

That works because **every placed prop carries a stable identity back to the
original game data**: each `thing` in the generated map has
`user_roth_sector` and `user_roth_object`. Author against those IDs and your
work survives any number of rebuilds, even if the converter's output changes
completely.

`generated/` can be deleted at any time with no loss. `authored/` cannot.

## The tools, individually

| Tool | Does |
|---|---|
| `parse_raw.py` | `.RAW` map → JSON (rooms, walls, objects, triggers) |
| `extract_das.py` | `.DAS` pack → indexed PNGs, original palette preserved |
| `extract_meshes.py` | 3D props (furniture) → OBJ models |
| `build_map.py` | JSON + artwork → loadable PK3 |
| `render_floorplan.py` | map JSON → labelled top-down SVG |
| `survey_all.py` | parses all 44 maps, reports a coverage table |
| `make_grid.py` | calibration texture for debugging texture alignment |

### Checking nothing broke

```
python tools/survey_all.py --roth "<...>/ROTH"
```

Run after **any** change to the tools. We are reverse-engineering a 30-year-old
format and keep discovering we were wrong, so this is how a fix for the caverns
doesn't quietly break the study.

## Useful flags on build_map.py

| Flag | Why |
|---|---|
| `--grid` | replace all artwork with a measurable grid, for alignment debugging |
| `--no-rotate-walls` | disable the 90° wall transpose (it is correct; A/B only) |
| `--flat-fit` | per-sector flat fitting; currently produces extreme scales, off by default |
| `--scale` | geometry scale; texture scale compensates automatically |

## Known gaps

- **Texture alignment** is partly implemented. `textureFit` (a per-surface
  stretch factor, 27 distinct values across the game) is still ignored, as is
  the stored per-wall texture length that can override wall geometry.
- **Doors, switches and triggers** are not converted at all. The level logic is
  a chained opcode system that still needs decoding.
- **Animated textures** use their first frame only.
- **Multi-angle sprites** use their first view only, so monsters will not turn.
- Each PK3 bundles its whole texture pack, so the full set is ~375MB. Sharing
  one pack PK3 between maps would cut that hugely; not done yet.
- **Only STUDY1 has been visually verified.** The other 43 build without errors,
  which is not the same thing.
