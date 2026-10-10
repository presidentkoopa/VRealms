# Session handoff — 2026-10-09

**Read `LANE_START.md`'s goal block first.** This file is the plan that serves it.

Almost nothing was built today. What happened instead: the goal got stated
properly for the first time, and a 22-agent review mapped the interaction seam
and caught three wrong assumptions — two of them mine, in committed code.

---

## 1. THE GOAL, and what it changed

> **Realms running in VR with sprite weapons and the WorldHands beam, playable
> start to finish.**

And the line that decides arguments, from the owner and *already written* in
`ROTH_GAME_PORT.md` §0 two weeks earlier: **the logic must be 1:1; presentation
and interface are free** — free to be *better*, not free to be broken. A door at
the wrong height is still a bug.

`ROTH_GAME_PORT.md` §4 already has the full build order (≈15,000 lines of rule
logic, steps 0-9). **Follow it.** The one deviation below is deliberate.

---

## 2. TWO THINGS IN COMMITTED CODE THAT ARE WRONG

Found by the review, both mine, both written last week.

**2.1 The wall-band gate is dead.** `roth::ActivateLine` has exactly one caller
— `p_spec.cpp:141` — and it passes `nullptr`. So `part` at
`roth_runtime.cpp:4330` is always 0 and `WallPartBit` never runs on *any*
channel. **The comment at `roth_runtime.cpp:4327` claiming the tracer's impact
sites supply a hit position is FALSE.** Giving the pick a real hit point turns
that rule on for the first time, and some triggers that work today will start
refusing. That must be a measured decision, not a side effect.

**2.2 A stop-at-first trace would REGRESS working triggers.** Verified:
`rothmap.cpp:1300-1310` gives a Realms face *with a sister* `ML_TWOSIDED` and
**no** `ML_BLOCKING`; `p_trace.cpp:520-523` never reports a two-sided line the
ray passes *through*. So an `0x18` face trigger on the mid band of any interior
wall is reachable today via `P_UseTraverse` (which offers every crossed line)
and **invisible to a stop-at-first trace, for every possible wallMask**. No mask
value fixes it. Use `Trace`'s `ETraceStatus` callback with `TRACE_Continue`
(`p_trace.h:96-102`); `p_trace.cpp:470-474` already does this for `SPAC_Impact`.

---

## 3. LIGHT SWITCHES DO NOT WORK, and a perfect ray will not fix them

The owner's two examples are "open doors" and "turn lights on and off". **They
are two different features and only the first exists.**

Verified at `roth_runtime.cpp:3818-3829`: opcodes **`0x08`** (left-click object)
and **`0x30`** (examine object) hit `unclassified++; continue;`, and
`EventForOpcode` returns `None` for both. No binding, no event, no dispatch.

Caveat worth measuring: a light switch authored as a **wall face** (`0x18`
whose chain contains `0x02`, the light-switch opcode) *should* work today. One
authored as an **object** will not. Which kind STUDY1 uses has not been checked.

This is step 6 below, and it is a large step.

---

## 4. THE PLAN — the seam, in seven steps

From the review. Each step says what the owner can *do* afterwards.

| # | what | proves |
|---|---|---|
| **1** | **No code.** `use_mode 1` (NOT 2), `roth_trigger_debug 1`, `roth_useray_debug 1`. Point the main hand at a door, close and level, press use. | Whether the hand branch runs at all — i.e. whether `OverrideAttackPosDir` is true in his session. At `use_mode 2` the head branch runs first and returns true even when merely blocked, so a door opening would prove nothing. Expect it to work close and level and fail on stepping back or tilting — **that failure shape is the measurement** |
| **2** | Three standalone bug fixes, each worth landing alone: the `*usefail` tail runs against the OFFHAND ray because both hand branches reassign `start`/`end`; `GetLaserBeamEndpoints` passes `mo->Sector` while the origin is the HAND | independent correctness |
| **3** | **Publish ONE hand ray per tic** and point the drawn beam at *that* | the beam becomes trustworthy. Today the beam is built from controller globals and the use ray from `AttackAngle+90` — they drift as you turn |
| **4** | **The pick.** `roth::Pick` + `roth::PickFrom(...)`, ONE trace, `TRACE_Continue` callback offering every crossed Realms line. Split `UseFlat` at its trace into `UseFlatAt(who, const FTraceResults&, want)`, eye form becomes a byte-identical wrapper. Router: wall faces → roth's face dispatch **with the hit position, calling `roth::ActivateLine` directly**; flats → `UseFlatAt`; leaves → `SwingDoor`. Band from `res.Tier`, not `WallPartBit`. Ceilings gated OUT. Gated on `g.active` + one new archived cvar, default off | **point at a door across the room and open it; point DOWN at a floor plate and fire it.** Neither is possible today — the hand rays are 2D (`DVector2`, no pitch) and 48 units long |
| **5** | Delete the three half-measures: the hook on Doom's line-use, the eye-aimed floor probe, the 2D hand branches. **After** step 4 is trusted | nothing new — stops the codebase having four answers to one question |
| **6** | **The object channel.** `0x08` and `0x30` routing, a `g.byObject` map beside `byFace`/`bySide`/`bySector`, category-1 bases, the `g.activeObject` latch, ray-vs-record prop sweep | **light switches work** |
| **7** | **Port the missing natives** so `RS_WorldHands` can load, then wire examine to the secondary button | the beam in the goal sentence becomes loadable at all |

### The deviation from `ROTH_GAME_PORT.md`

That plan says Phase 1 is Realms played **flat**, VR second. Sound engineering.
But the owner cannot verify anything today — a week was spent asserting doors
worked, verified through a code path he would never touch. Pulling the beam
forward is small (an input path, not a system) and makes him able to check
steps 1-9 himself. **The rest of the order stands as written.**

---

## 5. THE NATIVES PORT (step 7) — measured

`RS_WorldHands` **cannot load on REMAROTH**. Its ZScript calls engine functions
this fork does not have; ZScript compiles at startup, so it is a fatal script
error and the session refuses to start with a cause that looks unrelated.

| native | REMAROTH | UZDXREMA | called from |
|---|---|---|---|
| `HandPos` | ✅ | ✅ | — |
| `HandVelAtPoint` | ❌ | ✅ | `rs_held.zs:1270`, `rs_telemetry.zs:73` |
| `HandPeakAgeMs` | ❌ | ✅ | `rs_throw.zs:217`, `rs_telemetry.zs:74` |
| `GetRawStickTurn` | ❌ | ✅ | `rs_ovaledit.zs:301` |
| `SetBeamStyleScroll` | ❌ | ✅ | `rs_grabviz.zs:560` — **the beam itself** |

All four exist in UZDXREMA, so this is a copy, not an invention. The grip
contract is **already fully exported** in REMAROTH — `GrabClaimMain`
(`actor.zs:681`), `GripContextMain` (`:703`), `GripHeldMain` (`:713`),
`GripClaimMain` (`:758`), `GripSubjectMain` (`:760`), `MainHandRoll` (`:618`),
`OffhandRoll` (`:645`). Four natives, not a merge.

**Unchecked:** whether four is the whole list. Those are the ones found so far.

---

## 6. MUST CHECK BEFORE WRITING CODE

- **Do you want the wall-band gate on?** It is dead today (§2.1). Count how many
  STUDY1 face bindings carry a non-zero `fireFlags` and would be band-gated,
  then decide. Do not let it switch on as a side effect.
- **How many Realms face triggers sit on two-sided lines?** This sizes §2.2's
  regression risk. Headless count: `roth::SidesWithOpcode(0x18)` cross-referenced
  against each line's `ML_TWOSIDED`. Cheap, no headset.
- **What does `Trace()` return aimed at a door leaf at chest height?** A leaf
  line is one-sided and its frontsector is the parked void cell, so `LineCheck`
  may relocate `CurSector` into the void sector and classify against that.

---

## 7. DO NOT

- **Do not add a `use_mode` value.** Both forks define `GAMENAME "DoomXR"`,
  there is exactly **one** `doomxr.ini`, and `use_mode` is
  `CVAR_ARCHIVE | CVAR_GLOBALCONFIG` in both (REMAROTH `p_map.cpp:74`, UZDXREMA
  `p_map.cpp:79`). Setting 3 here makes the hands build read 3 next launch,
  where the head ray silently vanishes. **Gate on `g.active`** — never true in
  UZDXREMA.
- **Do not set `vr_laser_sight` / `vr_laser_beam` / `vr_laser_show_melee`.** All
  archived and global. They persist into the hands build, where `RS_GrabViz`
  draws its own beams — two visibly disagreeing beams per hand, in the build
  where the owner's tuning lives.
- **Do not touch `rs_grab_m_*` / `rs_grab_o_*` or repoint `RS_Reach.Centre`.**
  That is baked tuning (`rs_grab_m_ofs_z = -34.0`, annotated baked 2026-09-15),
  and MODELDEF hands the same cvar names to the engine as `PlacementCVars`, so
  they also decide where the hand is **drawn**. Two writers on one transform.
- **Do not treat the drawn laser as the regression test.** Verified
  `hw_weapon.cpp:692-702`: in VR `GetLaserBeamAttackDirection` returns the raw
  controller direction — no `+90`, pitch unnegated. The `+90`/`-AttackPitch`
  lines at `:706-707` never execute in a headset. **The owner's own WorldHands
  beam is fine** — `RS_GrabViz` and `RS_Cone` both build on `RS_Basis.Fwd` and
  agree with each other by design. It is the two *engine* features that
  disagree.

---

## 8. Also settled today

- **The install we read was not the whole game.** `_oracle` is the 1996
  hard-disk install; the CD content is separate. Point `-rothpath` at the retail
  `DATA` directory — it has `DBASE300` (music **and the item close-up GDVs**),
  `DBASE500` (speech), all **164** `.GDV` movies and 51 maps against 50. Leave
  `_oracle` alone; it is the oracle rig's working directory. `ROTH_BETTER.md`
  §18.
- **`ROTH_BETTER.md` gained §13-§19**: the 1:1 line, the hand beam, the
  trenchcoat inventory, voxelising the sprite content (with the examine-GDV
  turntable lead), the mod shelf, the install correction, and IK doorknobs as
  the end state the beam stands in for.
- **The door-height change** (committed last week, `TEXTURE_MAP_OVERRIDE` for
  the 43 of 167 leaves that carry one) **built cleanly but is still unverified**
  — the capture grabbed a stale frame. See §9.

---

## 9. The capture rig cannot take pictures while the owner is at the keyboard

`i_pauseinbackground=false` (set last week to stop runs stalling) means the game
keeps **ticking** unfocused but does not keep **presenting**, so `M_ScreenShot`
grabs whatever was last in the buffer — a startup screen. Both files come out
byte-identical and look like a broken capture.

Either the window keeps focus for the whole run, or use `rothdiff_dump` (the
identity buffer), which is computed from the renderer's data rather than grabbed
and cares about neither focus nor lighting.
