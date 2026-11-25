# VRealms / REMAROTH: read before anything

Realms of the Haunting (1996), ported into a GZDoom-derived VR engine from the owner's retail install.
Start a session by reading the newest `HANDOFF_*.md` at the repo root (today `HANDOFF_LANE_GAMELAYER_2026-11-24_M2.md`).

## Owner's rules
- Never comment on the owner's hours, schedule, job or life. Never tell him to rest.
- No shortcuts, workarounds, or switches that hide unfinished work. ROTH.C proves it can be done properly.
- Cosmetic polish waits until the owner asks for it. Gameplay and logic come first.
- **VR design is open (owner, 2026-11-23): "we are clear to engage VR Realms of the Haunting now, we want to make a terrific VR experience for a game which has such an unusual interface."** And: "VR-first designs in mind now, we want to take Realms of the Haunting to the next level ... it is our engine." VR may change how you reach, handle and see things (docs/VR_INTERACTION.md). The original's game logic still runs underneath and stays 1:1: chains, pickups, keys, reach limits, what a use or examine does. Until 2026-11-23 the rule was "1:1 with the original first, no VR adaptations" (2026-11-12: an invented death box and a re-wrapped subtitle); the flat game itself stays 1:1.
- **The fun zone (owner, 2026-11-24): "our 1:1 rebuild has been satisfied, and now we are in the fun zone"; "realms of the haunting reimagined to take advantage of full 6dof for a shooter / adventure game hybrid"; "object / world / hand / player interaction is the single most important thing for this project"; "you're welcome to do anything you want in order to achieve this goal".** Plan: **docs/VR_GAME_PLAN.md** (the whole game, milestones in order; every session picks its work from it and updates its status); docs/VR_PHYSICS.md is the physics part. Report at milestones, not step by step; rig checks (the bridge) only at milestones, or ask first. Asset generation comes last: the systems do not depend on it. The original's game logic still runs underneath (what can be picked up, keys, chains).
- **Existing solutions first (owner, 2026-11-24): "can we say that we want to look for existing solutions before guessing at desiging our own? the physics we found, the interaction we built, and i complained about the interavtion".** Before designing any VR system (movement, pointers, grabbing, menus, comfort, physics), find a proven open-source implementation (a licence we can ship: MIT, BSD, Apache, zlib, GPL-compatible) or a shipped game's known solution, name it, and port it under the porting rule below. Design our own only when nothing fits, and write down why. On hand: Jolt Physics (in the repo), Godot XR Tools (`/home/claude/godotvr/godot-xr-tools`, MIT: pointer, pickup, movement providers, player body, vignette), VRExpansionPlugin (MIT, the M2 tracks), UltimateXR (MIT).
- **VR first (owner, 2026-11-24): "if we are going vr first then we should disable ... the use key so i have to use the gestures and world".** In VR the hands are the only way to use the world; the flat game keeps the original's keys.
- **The game is read from the native install. Never design anything the original already has.** Its screens, panels, menus, fonts and pictures are in the install; draw them from there, placed by the original's own drawing code (ROTH.C), and check them pixel for pixel against the original's frame. A layout of our own (boxes, text tabs, colours) is invented work. (2026-11-12: the inventory, menus and dialogue box were drawn as our own layouts.)
- The owner's `doomxr.ini` is shared with his hands build: never set or write archived settings from the engine or from tests.
- Never use screen control (computer use) on the owner's PC. Test through the REMAROTH bridge's hidden desktop.
- Never use `git stash`: all worktrees share one stash list.
- Branch `lane/gamelayer` (the working branch). `main` (owner, 2026-11-24) is moved up to it at each milestone. Do not create other branches unless the owner asks.

## Never claim what you have not checked
1. **Look before you assume or build.** List what already exists before deciding: the owner's folders (`E:\DOOMWork`, the Steam install), this repo's docs and tools, the owner's other repos. Hours were lost on 2026-11-12 because `E:\DOOMWork\_oracle`, the original engine already running, went unnoticed.
2. **"Done", "works" and "fixed" require named evidence:**
   - a differential-test count against the running original;
   - a screenshot of ours beside the original's frame at the same pose;
   - a log line from a run on the owner's PC.
   
   Anything else is written "reasoning only, not seen". A log line saying something loaded is not evidence that it draws or plays correctly.
3. **Every report separates "verified (how)" from "reasoning only".** Agents' reports must do the same, and the lead does not pass an agent's claim on as fact without that split.

## Porting rule (every source: ROTH.C, Eternity, Quake VR, RSQ DistanceGrab, 3D Platform, ...)
See `docs/QUAKEVR_POSTMORTEM_from_UZDXREMA_2026-11-12.md`.
1. Port from the source, never from a summary, a header comment, a research note or a handoff. Read the whole implementation first.
2. Before porting, write a numbered checklist of every part, constant and behaviour, with the source's file:line and the port's file:line (or "not ported" and why). That checklist is the acceptance test.
3. Never label partial work with the source's name.
4. When a port misbehaves, compare it with the source before inventing a fix.
5. For ROTH.C: prove it with a differential test. Same inputs through the original (a probe mod in the headless build) and through our engine-free port; diff the traces. ROTH.C is the specification; game logic is 1:1, and presentation may be better but must show the same things.

## Regression suite: run before handing back any change it touches
- `tools/gameproof/run.sh 0` (modes 0 1 2 4 7)
- `tools/dialogue_test/run.sh`
- `tools/cmdcore_test/run_diff.sh <maps>`
- `tools/actors_test/oracle/diffcmds.sh MAP` plus tick replays
- `tools/gameflow_test/build.sh` and `tools/menus_test/build.sh` (with `ROTH_DATA` set)
- `tools/gdv_test/`
- `tools/wallrule/`

A visual change gets `rothdiff_shot` captures on the owner's PC beside `oracle_shot.sh` frames of the original.

## Build traps
- **`src/roth/roth_runtime.cpp` opens an anonymous namespace.** A function declared inside it must be defined inside it. Otherwise it type-checks but MSVC fails to link, and the owner is left without an exe. This happened three times on 2026-11-12.
- **`FTextureID x;` is uninitialised.** Write `FTextureID x = FNullTextureID();`.
- **A failed build on the owner's PC is fixed before anything else.**
- **Test configs run before the map exists.** Use `roth_at <tic> <command>` and set `roth_movies 0`, `roth_lighting 1` and `roth_palette_shading 1`.
- **Commits end with the attribution lines the session gives.**
