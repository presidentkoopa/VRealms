# Realms of the Haunting — VR Recreation
### Project brief, 2026-09-25

## What it is

A VR recreation of *Realms of the Haunting* (Gremlin Interactive, 1996) — the gothic
horror adventure set in a haunted English manor that escalates into a conflict
between Heaven and Hell.

It is built on **REMAROTH**, a private fork of our own VR engine (GZDoom lineage).
It is **not a Doom mod** — the engine is ours, and engines in this family ship
standalone commercial games (Selaco being the obvious example) with their own
branding, menus and content.

**Legal position:** the player must own the original game. We ship no original
artwork, levels, or code. Everything is converted on the player's own machine
from files they already have.

## Why this engine rather than Unity

The project began in Unity and moved deliberately.

1. **The VR work already exists.** Hands, holsters, hardpoints, weapon handling
   and physics are already built and tested in this engine family. In Unity all
   of that was still ahead of us.
2. **The level formats are cousins.** Realms and Doom both build worlds the same
   way — flat floors and ceilings at set heights with walls between them. Unity
   wanted the levels rebuilt as free-form 3D geometry, a conversion nobody had
   done. Here, source and destination already speak nearly the same language.

The trade-off accepted: this engine's lighting is less capable than Unity's
high-end renderer, which matters because Realms' atmosphere is carried by light
and shadow. Mitigated by existing fog, darkness, bloom and glow mods.

## Where it stands

**Working:**

- A conversion pipeline that processes **all 44 maps of the game in one command**.
- The entire game's geometry reads correctly — **16,906 rooms, every one
  structurally sound**, verified automatically rather than by eye.
- Artwork extracts with the **original 1996 palette preserved exactly**. No
  upscaling, no AI enhancement — the chunky period look is intentional.
- **The Study loads and runs**, at roughly 190fps, with **259 objects placed**
  including **47 pieces of real 3D furniture** (Realms stores true geometry for
  props needing a solid silhouette, not just flat cards).
- Wall-mounted items (paintings, signs) are pinned flat rather than swivelling
  to face the player.
- Monster artwork is extractable, though not yet used.

**Not done:**

- **Doors, switches and triggers** — none of it. Realms' level logic is a chained
  instruction system that still needs decoding. This is the largest remaining
  unknown.
- **Texture alignment is partial.** Artwork appears at the right size and the
  right orientation, but per-surface stretch information is not yet applied.
- **Object elevation and orientation** are not right yet.
- **Overall world scale** is still being calibrated. Realms' architecture is
  genuinely grand — doorways three times a person's height — so separating
  "authentically oversized" from "wrongly scaled" is unresolved.
- Monsters don't turn to face you (their multi-angle artwork uses one view).
- **Only the Study has been visually checked.** The other 43 maps convert without
  errors, which is not the same as being correct.

## How the conversion works

The original game keeps its levels in `.RAW` files and its artwork in `.DAS`
files, in formats nothing else reads. Five small programs translate both into
something the engine loads.

```
   the player's own game install
                 |
      .RAW levels     .DAS artwork
                 |
       [ conversion tools ]
                 |
       a package the engine loads
```

**Everything is automatic and repeatable.** No hand-editing of levels, ever. If a
conversion needs manual cleanup, the tool is considered wrong. This is a hard
requirement, not a preference — 44 maps cannot be maintained by hand.

**Generated and hand-authored data are kept strictly separate.** Anything a person
authors — grab points, weight and mass, interaction tags, character positions —
lives apart from converted output and is merged at build time. Every placed prop
carries a stable identity back to the original game object, so authored work
survives any number of rebuilds.

## Design direction

- **Faithful geometry and artwork.** 1:1 with the original, original resolution
  and palette.
- **VR-native interaction rebuilt from scratch** rather than ported. The original's
  combat was its weakest element; that is the part most worth reimagining.
- **Long term:** props with real weight and mass — pick up a chair, move it,
  throw it. Explicitly later, but the data structure is already being laid so it
  can be attached without rework.

## Honest risk assessment

The riskiest technical question — *can the whole game's geometry be converted at
all* — is **answered and retired.** All 44 maps, all 16,906 rooms, cleanly.

The remaining unknowns are, in order of risk:

1. **Level logic** (doors, switches, scripted events). Not started, not scoped,
   and the piece most likely to hold up a playable vertical slice.
2. **Texture and object placement polish.** Known problems with known causes;
   work rather than risk.
3. **VR comfort and scale calibration.** Needs testing in a headset against the
   original, not solvable on paper.
