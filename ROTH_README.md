# REMAROTH — Realms of the Haunting in VR

Plain-language overview. Read this first. The deep technical detail lives in
`ROTH_CONVERSION_PLAN.md`; engine modifications are logged in `ROTH_ENGINE_CHANGES.md`.

---

## What this project is

A VR recreation of *Realms of the Haunting* (1996), built on our own engine rather than in Unity.

The player needs their own legal copy of the game. **We ship no original artwork, no original
levels, and no original code.** Everything is read from the player's own installation on their own
machine. Nothing from the 1996 game gets redistributed, ever.

## Why this engine and not Unity

This started in Unity and moved here deliberately. Two reasons:

1. **The VR work already exists.** Hands, holsters, hardpoints, physics, weapon handling — all of
   it is already built and tested in this engine family. In Unity that was all still ahead of us.
2. **The level formats are cousins.** Realms and Doom both build their worlds the same way: flat
   floors and ceilings at set heights, with walls between them. Unity wanted the levels turned into
   free-form 3D shapes, which meant inventing a conversion nobody had done. Here, the source and
   the destination already speak nearly the same language.

**This is not "a Doom mod."** The engine is ours, and engines like this ship standalone commercial
games (Selaco being the obvious example) with their own branding, menus and content, no Doom
anything present. How it eventually ships is a choice, not a constraint.

## Where REMAROTH sits

`REMAROTH` is a full clone of `UZDXREMA` ("REMA"), kept separate on purpose so heavy engine
surgery for Realms can't disturb the many other mods that share REMA. The `rema` git remote points
back at the original, so useful work can be cherry-picked back deliberately — never automatically.

## How the conversion works, in plain terms

The original game keeps its levels in `.RAW` files and its artwork in `.DAS` files, in formats
nobody else reads. Our tools translate both into something this engine understands.

```
      the player's own game install
                  |
       .RAW levels        .DAS artwork
                  |
        [ our Python tools ]
                  |
        a .pk3 the engine loads
```

Four small programs do it, all in `roth_pipeline/`, all plain Python with nothing to install:

| Tool | What it does |
|---|---|
| `parse_raw.py` | Reads a level file and writes out its rooms, walls and objects as readable data |
| `extract_das.py` | Pulls the artwork out as PNG images, original colours untouched |
| `render_floorplan.py` | Draws a top-down map of a level so a human can see its layout |
| `build_map.py` | Combines the above into a `.pk3` the engine can load |
| `survey_all.py` | Runs everything across all 44 levels and reports what worked |

### Running it

```
cd roth_pipeline
python parse_raw.py "<game>/ROTH/M/STUDY1.RAW" -o out/STUDY1.json
python extract_das.py "<game>/ROTH/M/DEMO.DAS" -o out/tex_demo
python build_map.py out/STUDY1.json --textures out/tex_demo -o out/roth_study1.pk3
```

Then load `out/roth_study1.pk3` in `build-dxr/RelWithDebInfo/doomxr.exe`.

### Checking nothing broke

```
python survey_all.py --roth "<game>/ROTH"
```

Run this after **any** change to the tools. It processes all 44 levels and reports a table. We are
reverse-engineering a 30-year-old format, so we keep finding out we were wrong — this is how a fix
for the caverns doesn't quietly break the study.

## Where things stand

**Working:**
- All 44 levels of the game read correctly. Every room in all of them is structurally sound
  (16,906 rooms, zero faults) — meaning the whole game can be converted, not just one level.
- Artwork extracts correctly with the original 1996 colours preserved exactly.
- A loadable level file is produced, and the engine accepts it.

**Not done yet:**
- **Artwork alignment.** The images are correct but not yet positioned properly on the walls, so
  expect patterns to look offset. This is the next real job.
- **Objects aren't placed.** The furniture and items are read but not yet put into the level.
- **Raised walkways** (2,299 across the game) aren't built yet.
- **Doors, switches and triggers** aren't converted — this is the largest remaining unknown and
  needs the original game's logic decoded.
- **Characters and creatures** can't be extracted yet (a second artwork format still to handle).

## Ground rules we're working to

1. **Everything is automatic and repeatable.** No hand-editing of levels, ever. If a conversion
   needs manual cleanup, the tool is wrong.
2. **Batch over all 44 levels, not one.** Problems that only appear in the water levels or the
   tower need to surface immediately, not after weeks of work tuned to one room.
3. **Count what we don't understand — never skip it silently.** A tool that quietly drops the parts
   it can't handle will report success while producing a broken level.
4. **Generated data and hand-made VR work stay in separate files.** Regenerating a level must never
   destroy hand-placed grab points, interaction tags, or character positions.
5. **Keep the 1996 look.** Original resolution, original colours. No upscaling, no AI enhancement.
