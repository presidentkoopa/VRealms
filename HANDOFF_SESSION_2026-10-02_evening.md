# Session handoff — 2026-10-02, evening lane

**Second lane of 2026-10-02.** `HANDOFF_SESSION_2026-10-02.md` is the morning
lane's; this one picked up from `LANE_START.md` and ran from about 11:00.
12 commits, 35 unpushed in total. Tree clean apart from `doomxr-log.txt`.

Read `LANE_START.md` first — it is updated and still the entry point. This file
is what changed, what is in flight, and what cost the most to learn.

---

## 1. What is done, and measured

| | |
|---|---|
| Capture camera angle convention | **Fixed and proven.** `RothAngleToDoom` used the OBJECT rule for the CAMERA. Pose C now shows the long hall in both engines. |
| Prop drift | **Fixed.** Props were placed correctly and then rose out of the floor; `+NOINTERACTION` on `LoaderProp`. 35 of 75 props moved between two captures in one run; now 0. |
| Directional view order + `ANGLE_SENSE` | **Measured correct.** Diagonal wins 8 of 8 on an orbit of `ROTH_DEMO_O04358`. |
| Per-view lateral anchor | **Needs no code** — inert in every retail map, like the tint ramp. |
| VR world scale | **Fixed.** 93.5 units/metre, derived as 34 × 154/56. The owner confirms scale and door-sigil height are now right. |
| The oracle rig | **Unattended.** Any of the 44 maps, no movies, no CD prompt, no menu, no quicksave. |
| MAPINFO | All 44 maps declared. "Most maps are not in MAPINFO" is no longer a trap. |

Flat-screen control, fully neutralised: centre pillar **44.2 against the
original's 44.2**, far wall 14.3 against 13.9, right-hand wall 19.6 against 1.7.

---

## 2. What is in flight

### 2.1 Doors — THE BIG ONE. Read `docs/TRIGGERS_the_use_key_problem.md`.

The use key does not open doors, and chasing it found **three independent
breaks** in the trigger layer. Only the first is fixed.

1. **The engine never offered us the line.** The hook is at `P_ActivateLine`,
   the HANDLER; the engine decides at the dispatch sites, which all gate on
   `line->special != 0`. **Fixed** — lines carrying a chain get a non-zero
   special and the activation bits at load. 74 marked in STUDY1, verified still
   marked at play time.
2. **`FireSectorTriggers` has ZERO CALLERS.** Defined, declared, never called by
   anything. All 12 sector-keyed triggers in STUDY1 have never fired in any
   build. **Not fixed.**
3. **The classification is wired to the wrong events, and `docs/GAME_core.md`
   §5.2 already says so** — it states it "corrects R/ROTH_COMMANDS.md and
   R/src/roth/roth_runtime.cpp:54-55", and the correction was never applied.
   `0x13` (the real enter/leave sector) is not classified at all; `0x19`/`0x31`
   are floor CLICKS not sector entry; `0x1a` is attack-hits-wall not a bump;
   `0x32` is right-click examine, lumped in with `0x18`. **Not fixed.**

**THE EXACT NEXT STEP:** do (3) against `GAME_core.md` §5.2 directly. Rewrite
`IsFaceTrigger`/`IsSectorTrigger` (`roth_runtime.cpp:310-311`) from that table,
add a binding for `0x13`, and separate use from examine. Then wire a caller for
`FireSectorTriggers`. Do NOT patch outward from the symptom; the table is
authoritative and names the lines.

Measured at the door poses, so the next lane need not redo it: 74 marked lines
present, 2 within `USERANGE` of the player, nearest 40 units, and the use ray
DOES reach a line — 1669, which carries no Realms chain. The ray works.

### 2.2 Furniture angles — narrowed to one candidate, oracle already captured

The owner reports some furniture needs a 90° turn and some does not. Eliminated
by reading: `ANGLE_SENSE` (today's directional test would have broken), the
wall-sprite convention (GZDoom spans the quad perpendicular to Yaw, which is
what Realms means), and the shared `ObjectYaw` (all props use one path).

**What is left: meshes.** `roth_objects.cpp` memsets `FSpriteModelFrame`, so
`angleoffset` is 0 and the mesh is rotated by the actor's yaw with no correction
for the model's own forward axis. GZDoom MODELDEFs routinely carry
`AngleOffset 90` for exactly this.

**Half done:** the ORACLE side is captured — `captures/mesh4123.frame.pgm` and
`mesh4128.frame.pgm`, from `poses_mesh.csv` (`DEMO[4123]` at rot 175 and
`DEMO[4128]` at rot 192, both asymmetric enough that 90° cannot hide). Ours is
not captured yet. Capture at the same two poses, `framepng` both, compare the
orientation.

### 2.3 The candle cone following the head — built and reverted

The owner asked for it (motion sickness: the lit pool swings to wherever you
look). Built, then reverted. The SIGN is known good — a ±30° test showed +30
swings the cone left, so `body − view` holds it still — and the uniform plumbing
was verified across all three declarations. It was pulled because a trustworthy
flat-screen control could not be had while `vid_fixgamma` was poisoning every
capture (see §3.1). Recorded in `ROTH_BETTER.md` §12. Re-applying is cheap.

### 2.4 Still open from before, unchanged

The start-pose right-hand wall (19.6 against 1.7) — diagnosed as the candle cone
being interpolated across OUR quads rather than the original's spans; two fixes
tried and both reverted with numbers, see `docs/REMAROTH_MEASURED.md` §4. Trees
drawing as green streaks. Ceiling seams and misaligned walls (owner-reported,
uninvestigated). Sky column remainder. Storm glow never seen.

---

## 3. What cost the most, and must not cost it again

### 3.1 `vid_fixgamma` — a capture condition nobody had written down

`CVAR_ARCHIVE | CVAR_GLOBALCONFIG`, default 0. The owner turned the gamma down in
the headset, which wrote −1 into the config and darkened **every capture
afterwards**, in this game and every other the engine runs.

It read as a shader regression for an hour. It survived defaulting a cvar off,
gating on `IsVR`, setting it explicitly to 0, commenting a uniform read out of
both shaders, and finally a full `git checkout` of every file touched. **The
picture was still wrong with the tree at HEAD**, which is the only reason it was
eventually looked for outside the tree.

"Still broken after a full revert" is the signal. It was two builds before that
registered. `vid_blackpoint`/`vid_whitepoint` are the same hazard; all three are
now in `docs/REMAROTH_MEASURED.md` §8 and in `captures/pair2.cfg`, `pair3.cfg`,
`s2orbit.cfg`.

**It also corrected the control:** the pillar reads 44.2 against the original's
44.2 exactly. The 44.1 in the measured doc and the 42.9 quoted all day were both
taken with `vid_fixgamma` already off its default.

### 3.2 `doorgeom -cam` and `meshcheck -v` emit STALE angles

Both compute `a512 = (90 − doomDeg) × 512/360`, the pre-fix convention, and
`doorgeom.cpp:140` cites the very lines that changed. `LANE_START` said to
regenerate `doorgeom`'s output; this lane grepped for `360.0 / 512`, found
nothing, and declared it clean. The grep was too narrow and the instruction was
right. **Every pose either tool prints needs `(512 − a) % 512` until they are
rebuilt.** Rebuilding them is a small job and worth doing first.

### 3.3 LNK1103 is frequent, and the exit code lies

Five times in one session. The handoff's earlier claim that it follows
header changes is WRONG and is corrected in place — the fifth came from editing
two `.cpp` files. Budget the full LTCG pass as a normal cost.

`cmake --build` returned **0** to the shell on a build whose link had failed and
whose exe CMake had then deleted. Grep for
`fatal error|error C[0-9]{4}|LNK[0-9]{4}` and **confirm the timestamp on
`doomxr.exe`**.

### 3.4 No shell heredocs, and this lane did it twice

`\\n` inside a heredoc becomes a real newline, which splits a C string literal
and fails to compile. It is already a rule in this project. Write patch scripts
to a file and run them.

### 3.5 Archived cvars leak between a capture and the owner's session

`roth_lighting`, `roth_pattern`, `roth_palette_shading` were already known.
`vr_mode`, `vid_contrast`, `vid_brightness`, `vid_saturation` were being written
into the owner's config by the capture scripts every run — `vr_mode 0` meant
every plain launch was flat for half a day. A capture cfg sets archived cvars for
the owner, not just for itself.

---

## 4. Instruments added

| | |
|---|---|
| `tools/rothdiff/run_oracle.ps1` | the oracle, unattended, any map. **Never pass `--skip-gdv`** — it hides the .GDV files and raises the CD prompt. |
| `tools/rothdiff/build_plugin.cmd` (×2) | rebuild either oracle mod, 32-bit MSVC |
| `rothdiff_use <x> <y> <angle512>` | places the camera and calls `P_UseLines`, the same function `+use` calls |
| `roth_useray_debug`, `roth_trigger_debug` | what the use ray meets; what reaches the Realms handler |
| `tools/rothdiff/ridb.py` | what surface is at a region of an identity buffer |
| `tools/rothdiff/strips.py` | mean grey in strips: FLAT difference = the sector, one that GROWS with \|x−centre\| = the candle cone |
| `tools/rothdiff/viewmatch.py`, `dirprops.py` | directional view order |
| `tools/rothdiff/sprcheck.py` | prop rectangles against measured §6 |
| `tools/rothdiff/packiwad.py` | rebuild the stale-prone `vrealms.pk3` |
| lights-out counters | the load report now says how many sectors were darkened and with what deltas (139, −15/−15/−10 — exact match to the original) |

---

## 5. One thing to hold on to

Two of today's three biggest findings were **a handler with nothing calling it**
and **a spec correction nobody applied**. Neither is visible from the outside;
both compile, both read as finished work. The third was a capture condition that
made a correct tree look broken.

The common thread is that none of them could be seen by looking at the thing
that was wrong. They were found by asking which code a sentence referred to,
and by noticing that a symptom survived a change that should have removed it.
