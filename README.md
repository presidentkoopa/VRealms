# VRealms

**Realms of the Haunting (1996), in VR.**

Not a port and not a remake. The engine opens your own retail installation and builds the
game from it at load — maps, artwork, 3D props, level logic. Nothing is extracted,
converted or cached, and nothing from the original ships with this. **You must own the
game.**

Underneath is a GZDoom fork with VR built into it rather than bolted on, so Realms
inherits room-scale tracking, real hands, and everything else already living there.

## Running it

```
doomxr.exe -iwad <any iwad> -rothpath "<path to your ROTH folder>" +map STUDY1
```

The path must be quoted — it contains spaces.

Every load writes a full report beside the executable: what was opened, what was in it,
and a counter for anything the loader did not understand. Nothing fails silently.

## Where it is

A level loads from the retail install with geometry, textures, animated and translucent
surfaces, 3D furniture, intermediate floors, and doors on their real hinges. Lighting,
level logic and interaction are in progress.

## Where it is going

- **Gestures** — reaching, grabbing, turning and opening, rather than pressing a button
  at a thing.
- **A VR-forward inventory.** The original's was a grid on a screen. In a headset it
  should be something you reach into.
- **Weapon handling** with real two-handed manipulation, holsters and hardpoints.
- **Enemies** faithful to the original first, then free to be smarter.
- **Props with weight** — furniture you can actually move, with mass behind it.
- **Modern visual effects** where they serve the atmosphere. Realms was always about
  darkness and what is in it.
- **Doom mods still work.** It is still the fork, so the wider ecosystem comes along.

## Reading the code

- `ROTH_STATE.md` — start here. What runs, how to build, what is open.
- `ROTH_LIGHTING.md` — the original's lighting model, transcribed.
- `ROTH_COMMANDS.md` — its level-logic system, decoded.
- `ROTH_BETTER.md` — things the 1996 engine does better than the one hosting it.
- `README_ENGINE.md` — the host fork, and its lineage: GZDoom → Team Beef → emawind's
  QuestZDoom → iAmErmac's DoomXR → UZDXREMA.

The rule throughout: **the original is the specification.** Where it and the host engine
disagree, the original wins and the engine changes to suit. Anything not read out of the
original's own code is marked unverified — guessing has cost more time here than reading
ever has.
