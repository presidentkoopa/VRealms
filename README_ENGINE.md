![Doom XR Edition](https://github.com/iAmErmac/DoomXR/blob/doomxr/branding/banner.png)

# UZDXREMA — a rendering fork of DoomXR

[![Continuous Integration][badge_git]][status_git]
[![Engine Translation status][badge_trans]][status_trans]
[![Game Translation status][badge_trans_games]][status_trans_games]

A fork of [DoomXR](https://github.com/iAmErmac/DoomXR) that adds a **lighting
and rendering feature set** on top of it. Everything DoomXR does — VR, dual
tracked hands, OpenVR input, the mod compatibility below — is unchanged and
inherited. What is new here is what the renderer can draw.

The name is the lineage, in order:
**UZ**Doom · **DXR** for [iAmErmac](https://github.com/iAmErmac/DoomXR)'s DoomXR ·
**EMA** for [emawind](https://github.com/emawind84/QuestZDoom)'s QuestZDoom.
Two thirds of it is theirs, and that is about the right ratio.

The full chain runs one step further back: [Team Beef](https://github.com/Team-Beef-Studios)
built the original Quest VR port, emawind forked it as QuestZDoom and carried
it forward unofficially, iAmErmac brought that VR work into a UZDoom base as
DoomXR, and this fork is what came after, on top of all three.

The capability claims in this document and in `FORK_CHANGES.md` have been
checked against source, and so has that chain — GitHub confirms
`emawind84/QuestZDoom` forks `Team-Beef-Studios/QuestZDoom`. What each of them
actually went through along the way — the hurdles, the decisions made and why
— is still attribution, not a verified history this repo can tell.

Branch: `main`. Engine base: **UZDoom 5.0.0-rc.2**.

Three documents, for three questions:

- **[`CHANGES.md`](CHANGES.md)** — *where* the changes are. Every one of the 400
  files this fork touches, what owns it, and what it does there.
- **[`FORK_CHANGES.md`](FORK_CHANGES.md)** — *why*, in full. The engineering
  write-up, with file references and the reasoning behind each decision.
- This page — *what*, for someone deciding whether they want any of it.

Note that this repository is the **engine** half. Nearly every feature here is a
capability plus a ZScript native to reach it; the content that calls those
natives lives in the mods, not here. `CHANGES.md` covers that boundary.

---

## Why this fork exists

It was built to support a lighting mod whose entire premise is darkness, and
that premise kept running into the same wall: **the sector is the smallest
thing stock Doom lighting can talk about.**

A sector has one light level and one colour. So a room is uniformly dim, a
glow is a flat wash on a surface, an "expanding ring of light" lights whole
rooms in sequence, and fog fills a space floor to ceiling with no shape at all.
Every effect ends up chunky at exactly the scale you most want it smooth.

Almost everything below is the same move applied to a different problem:
**stop placing objects to represent an effect, and give the fragment shader the
maths instead.** A sweep band asks each pixel its distance from a point. A beam
asks its distance from a segment. Height fog asks how much of the view ray lay
below a ceiling. Darkness asks each pixel about its own light rather than its
room's.

The result is a set of features that are cheap — they ride a shader that was
already running, costing arithmetic rather than draw calls — and that behave
correctly at any scale, because nothing is being approximated by geometry.

The features are general. None of them know anything about the mod they were
built for.

---

## What this fork adds

| | |
| --- | --- |
| **Billboards** | Oriented world quads with hit testing — an in-world UI primitive. Real depth-tested geometry, occluded by walls, pointable and touchable. Twelve payload types including panels, digits, segment displays and a glowing seam that opens. |
| **Surface glow** | Floors and ceilings glow on their **own face**, not just spilling onto a nearby wall. Wall glow gains falloff curves and intensity. Each glow carries **two colours** and ramps between them, so a corner can be a continuous gradient instead of a hard edge. |
| **Sweep** | World-space bands of light that wrap floor, wall and ceiling as one unbroken line. Eight at once, five shapes, each with its own origin, speed and colour, and four things a band can do to what it crosses: add, lift, crush, re-colour. |
| **Sweep band fill** | A band can carry a **lattice**, a field of dots or a solid slab instead of a wash, and the lattice can stand **in the air** inside the band rather than being painted on what it lands on — a wall of lasers filling a corridor. It is a pattern rather than a set of objects, so four lines and four hundred cost the same. |
| **Glow wave** | A glow's **edge** varies along a surface, not just up it. Peaks and valleys, with a phase per surface so one wave climbs a room. |
| **Per-fragment darkness** | The darkness curve leaves the sector. Same four curves, asked per pixel — which makes **distance falloff** and **height pooling** possible, neither of which a per-sector multiplier can express. |
| **Fog slab** | Fog with a **top**, and a bottom. A layer of mist you stand in and look down at, solved analytically rather than raymarched — so it is also ceiling fog, a band at chest height, or fog draining and filling, depending only on where the two edges sit. Lit by the flashlight, tinted by what is behind it, and repeatable up the room as a rolling stack for the price of one layer. |
| **Reactive fog** | Density that **banks** instead of being one number for the map; rings off a muzzle, bursts on a death, ignition that lights mist in clear air, monsters shouldering it aside — all one primitive with eight slots, so anything built on it afterwards needs no engine change. Plus wisps rising off the surface as a lattice, hundreds for the price of one. |
| **Tornado** | The same mist gathered around a **vertical axis**. A funnel you can walk into and stand inside, hollow in the centre so you can see out, independent of the floor layer. |
| **Heatmap** | A grid over the map that accumulates where fighting happened and paints it on the floor, readable back from script so a spawn director can weight against ground already fought over. |
| **Selective desaturation** | A colour drain weighted by each colour's **own** saturation, so a monochrome world can still have blood in it — with nothing tagged, because the rule is about the colour and not the thing wearing it. |
| **Beams** | Real segment lasers. Continuous at any length, visible hanging in the air, correctly occluded, lighting the surfaces they pass — and **no dynamic lights, sprites or quads** anywhere in it. |
| **Volumetric beam** | A raymarched light cone with world-space dust, for the flashlight. |
| **Bloom** | Threshold, soft knee, anamorphic streak, tint, chromatic fringing. Upstream's threshold was hardcoded at 1.0, meaning only already-blown-out pixels could bloom. |
| **Non-pausing menus** | A settings page can let the world run behind it, so a lighting page can actually preview what it is adjusting. |
| **VR weapon wheel** | A wheel per hand, worked by the hand it belongs to. |
| **HUD stereo gating** | **Bug fix** — flat desktop sessions were losing the entire 2D layer: status bar, view border, and every mod's `RenderOverlay` at once. |

### Two rules the additions follow

**Frame-global values go in the viewpoint buffer, never `StreamData`.**
`StreamData` is the per-draw block, and its size divides a fixed 64KB buffer
into `MAX_STREAM_DATA` draws. Growing it costs draw batching in every frame of
the game, forever. Several features here would have been easier with a
`vec4[8]` of per-band data and deliberately are not.

**Spare bits get reused before anything grows.** The sweep's draw mode lives in
a component that began as a bare on/off flag hardcoded to `1.0`; the band fill
mode now shares it. `uFlatGlowIsCeiling` was a padding int. `MAX_STREAM_DATA`
has not moved.

---

## What it adds beyond the renderer

The table above is what the renderer can draw. This is the rest — the VR input
surface, and the reach script was given so a mod can use any of it.

| | |
| --- | --- |
| **Shapes** | Signed distance fields painted onto surfaces. A flat emissive glyph — disc, ring, square, outline, cross, hexagon, triangle — projected onto whatever surface passes through it. 128 slots, seven primitives, seven natives, its own shader function. The largest system in the renderer, and it had no entry anywhere until the fork was audited against itself. |
| **Laser sight** | The fork's own: beam, dot, glow, per-hand colours, a trace behind it, a target lock and a headshot reaction. Around fifty `vr_laser_*` cvars and a `toggle_laser_sight` CCMD. |
| **The laser as a borrowed cursor** | An in-world menu made of billboards needs a pointer, and the engine already draws a very good one. Three natives let a script borrow the real laser instead of building its own — an override that never writes a cvar, so the player's settings survive it. |
| **Haptics reach ZScript** | `Level.VRHaptic(hand, intensity, durationMs)`. Complete per-hand OpenXR haptics were already there — bound for the Touch, Index, Vive and simple profiles — and script could reach none of it. A menu could draw itself and be pointed at, and could not make your hand feel anything. |
| **Script-side VR input suppression** | A script can say *the stick is mine right now*. Snap turn and stick movement are decided deep in the VR input path, long before any script sees a button; the native wheel suppressed both and nothing else could. Driving a menu with the thumbstick used to spin and walk you while you were choosing. |
| **`MainHandRoll`** | The main hand's true wrist roll, for anything drawn *on* the held weapon. `AttackRoll` cannot carry it — the VR backends write the real value and `UpdateCanonicalMainHandPose` zeroes it on the next tic, before `WorldTick` runs, so script never saw anything but 0. |
| **Direct model frame addressing** | A HUD weapon model gets its frame through the *sprite*, and that channel is one character wide: `MAX_SPRITE_FRAMES` is **29**, inherited from Doom's 8-character lump names. The weapon models this fork ships run to 75 frames. Everything past the 29th was not awkward but *unaddressable* — there was no letter left to name it with. |
| **Native state remap** | ModelSwapper's animation engine, moved into the engine where it should have started. Frame addressing made the *script* the animation clock, with everything that entails: event ordering, tick timing, and silent failure when any link in the chain broke. This makes the psprite's own current state the clock, natively. |
| **psprite scale reaches models** | `psp->scale` was read only by the 2D weapon-sprite path. A mod that shrank a psprite saw nothing happen to a weapon drawn as a **model**, and there was no other way to resize one from script at all — model scale came from `MODELDEF` and the sprite frame. |
| **Texture inside the glow** | The glow wave varies a lane's **edge**, which is the right answer while the edge is on screen and no answer at all once coverage saturates and the wall is a solid card of colour. Five terms that act **inside** the lit area instead: the wave owns shape, these own substance. |
| **Field reflection** | Read another mod's data without linking against it — `HasField`, `GetFieldInt/Bool/Float/String/Name/Object`, `FieldCount`, `FieldAt`. Interoperate with something you do not control and cannot compile against. |

---

## Compatibility

**Content built for this fork will not run on stock GZDoom.** The additions are
engine features, not ZScript libraries — a mod calling `Level.SetBeam` or
`Sector.SetFlatGlowColor` fails to compile elsewhere.

The reverse is fine: anything that ran on DoomXR runs here.

**GLES does not implement** surface glow, glow waves, per-fragment darkness,
the fog slab, band fill or beams. The Vulkan and OpenGL paths both do.

---

## Everything below is inherited from DoomXR

DoomXR is a VR port based on QuestZDoom and UZDoom. Built and tested on HP
Reverb and Oculus Quest via Virtual Desktop; other setups should work as long
as they are OpenVR-compatible.

It exposes OpenVR controller input for definition — you will need to define the
controls. Both hands are tracked for weapons.

### Controller Info
#### Index Controllers
To get the most out of your Index Controllers, choose the Community Binding
"Index Controller Bindings" by gameflorist in SteamVR. It makes the maximum
buttons available for binding in DoomXR.

### Mods

Optional mods tested with DoomXR:

- https://github.com/hh79/gz3doom/files/4378108/HDVRweapons.zip HD weapon pack made by ajantaju
- https://github.com/ajantaju/br_vr weapon pack made for Brutal Doom
- https://github.com/dxt121730/BD64Weapons weapon pack for Brutal Doom 64 by dxt121730
- https://github.com/mmaulwurff/laser-sight/releases laser sight mod, you need this for aiming
- https://github.com/iAmErmac/Virtual-Tactical-Vest adds a virtual vest with weapon slots by iAmErmac
- https://www.moddb.com/downloads/doom-neural-upscale-2x texture upscale mod
- https://forum.zdoom.org/download/file.php?id=30459&sid=df63736751c12c3ebb76230d1dc86543 blood color fixer mod

---

## Building

`auto-setup-windows-vr.cmd` locates Visual Studio's bundled CMake via
`vswhere` — CMake is generally not on PATH. Build output lands in
`build-dxr/RelWithDebInfo/`.

**Python 3 is required to configure.** UZDoom 5.0.0 compiles
`libraries/Translation/*.po` into the language lumps at build time, so
`find_package(Python3 3.6 REQUIRED)` will fail the configure step without it.
If it is not on PATH, vcpkg downloads one and cmake can be pointed at it:

```
-DPython3_EXECUTABLE=build-dxr/vcpkg/downloads/tools/python/python-3.14.2-x64-1/python.exe
```

For the general UZDoom build process, see UZDoom's
[wiki][gh_wiki]:
[Linux][gh_linux] ·
[MacOS][gh_apple] ·
[Windows][gh_windows]

---

Copyright (c) 1998-2025 ZDoom + GZDoom + UZDoom teams, and contributors

Doom Source (c) 1997 id Software, Raven Software, and contributors

Please see license files for individual contributor licenses.

### Source code licensed under the GPL v3
##### https://www.gnu.org/licenses/quick-guide-gplv3.en.html

**Copyrights:**
* Copyright 1993-1996 id Software
* Copyright 1999-2016 Marisa Heit
* Copyright 2002-2016 Christoph Oelckers
* Copyright 2017-2025 GZDoom Maintainers and Contributors
* Copyright 2025-2026 UZDoom Maintainers and Contributors

See the [CONTRIBUTORS](CONTRIBUTORS) file for a full list of upstream code contributors.

The **UZDoom Icon** was designed by **Carlos "Cardboard Marty" Sanchez**, copyrighted to the UZDoom Team, and licensed under **Creative Commons BY-SA 4.0**.

## Resources
- [Home Page][home]
- [Forum][forum]
- [Wiki][wiki]
- [Discord Server][community]
- [Engine Translation][status_trans]
- [Game Translation][status_trans_games]

## Credits

This fork stands on a long chain, and none of the work below is mine.

The two tables above are the part I added. They sit on top of a VR engine that
was already finished, already fast, and already correct — which is the harder
half, and not mine at all.

### iAmErmac — [DoomXR](https://github.com/iAmErmac/DoomXR)

The port this forks directly, and a great deal more of what you are running
than the tables above imply. The VR shell is his: dual tracked hands, the
OpenXR path, the weapon wheel, the portable HUD, the controller menus.

So is nearly all of the performance work — and none of it had been written down
anywhere until this fork was audited against itself. The wall and flat surface
**light budget** and its culling, decorative sprite culling, the dynlight
portal, spotlight and model-dedupe caches, **BSP multithreading** and its
refined wall dispatch, threaded texture upload, and the performance tweak menu
that exposes them. Around thirty cvars under `gl_light_*`, `gl_bsp_*` and
`gl_texture_thread_*` are his, and they are the reason this runs at framerate
in a headset at all.

And the multiplayer layer on top: the host and join menus, name tags, visible
IP address, laser sight drawn for other players, and the netplay fixes
underneath all of it.

### Emanuele Disco — [QuestZDoom](https://github.com/emawind84/QuestZDoom)

QuestZDoom itself started as Team Beef's original Quest VR port (see below);
this is emawind's unofficial fork of it, carried forward and maintained on
his own. Over 1,300 commits in this tree's history, and the reason VR here
feels like a game instead of a demo.

The long, unglamorous campaign against **yaw desync** is his. Teleporting. The
ironlich whirlwind. The strife dagger. Every Hexen and Heretic weapon whose
action turned the player and left the headset pointing somewhere else. Each one
found separately and fixed separately, and none of them the kind of thing
anyone thanks you for.

Also the VR movement model — `vr_move_speed`, `vr_run_multiplier` and
`vr_walk_multiplier` scaling against `normforwardmove` rather than replacing it
— instant 180° turns, weapon bob restored on flat screen, the `PlayInVR`
property mods branch on, dual wielding, the integrated mod loader, and the
Android and CI maintenance that kept the whole thing compiling across years of
upstream merges.

### And upstream of both

- [ZDoom + GZDoom + UZDoom teams](https://zdoom.org/) — the engine this is based upon
- [Emile Belanger](http://www.beloko.com/) — the developer behind the android port
- [Team Beef](https://github.com/Team-Beef-Studios) — built the original VR port for the Oculus Quest device; QuestZDoom above is a fork of their work, not a from-scratch project

Special thanks to Coraline of the EDGE team for allowing use of her
[README.md](https://github.com/3dfxdev/EDGE/blob/master/README.md) as a
template for the original.

[gzdoom]: https://github.com/ZDoom/gzdoom/
[zdoom]: https://github.com/rheit/zdoom/

[repo]: https://github.com/UZDoom/UZDoom/
[home]: https://zdoom.org/
[wiki]: https://zdoom.org/wiki/
[forum]: https://forum.zdoom.org/
[community]: https://dsc.gg/zdoom

[gh_wiki]: https://github.com/UZDoom/UZDoom/wiki/Compilation
[gh_linux]: https://github.com/UZDoom/UZDoom/wiki/Compilation#linux
[gh_windows]: https://github.com/UZDoom/UZDoom/wiki/Compilation#windows
[gh_apple]: https://github.com/UZDoom/UZDoom/wiki/Compilation#macos

[status_git]: https://github.com/UZDoom/UZDoom/actions/workflows/continuous_integration.yml
[badge_git]: https://github.com/UZDoom/UZDoom/actions/workflows/continuous_integration.yml/badge.svg

[badge_trans]: https://hosted.weblate.org/widget/uzdoom/svg-badge.svg
[status_trans]: https://hosted.weblate.org/engage/uzdoom/

[badge_trans_games]: https://hosted.weblate.org/widget/doom-engine-games/svg-badge.svg
[status_trans_games]: https://hosted.weblate.org/engage/doom-engine-games/
