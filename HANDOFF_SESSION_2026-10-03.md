# Session handoff — 2026-10-03, overnight lane

Picked up from `HANDOFF_SESSION_2026-10-02_evening.md` and ran from about 17:30
to 05:45. **17 commits, all pushed** — this lane pushed on the owner's explicit
instruction, which overrides `LANE_START.md` §6's owner-only rule. Tree clean
apart from one deliberate uncommitted change, §2.1.

Read `LANE_START.md` first; it is updated. This file is what changed, what is in
flight, and what cost the most.

---

## 1. What is done, and measured

**The use key opens doors.** That was the headline problem and it is fixed.

| | |
|---|---|
| The door bug | **Fixed, and it was none of the three reasons the last handoff gave.** A door leaf is one-sided `ML_BLOCKING` geometry with no line special, so it *swallowed the use ray* and shadowed every trigger line behind it. Line 1669, recorded last lane as "carries no Realms chain", is the door itself |
| `0x18` left-click wall | **Fires — 34, the first observation ever.** It had never been seen: every door pose hits a *leaf*, which short-circuits before the trigger path |
| `0x13` enter/leave sector | **Fires. 23 records in STUDY1 that were bound to NOTHING** — the opcode matched neither classifier and fell through the binding loop in silence. "12 sector-keyed" in every handoff was really 35 |
| `0x19` floor click | **Fires, 12 of 12** |
| `0x1a` weapon impact | **Fires, 16 of 16 landed shots** |
| The `0x13` link variants | plain / water-Z / linked-platform, from the record's `+0x07` |
| A line fires only the side touched | it used to fire both sidedefs and discard the `side` argument |

**Entering STUDY1 sector 174 warps the map to STUDY3** — the first Realms
scripted map transition this port has run from play.

Everything above is in `docs/REMAROTH_MEASURED.md` §11 with the ROTH.C
citations. **Read that before touching the trigger layer.**

---

## 2. What is in flight

### 2.1 The door leaf's HEIGHT — committed but NEVER RUN. Run it first.

`src/maploader/rothmap.cpp` applies `TEXTURE_MAP_OVERRIDE` (the door sector's
`+0x0c`) as the leaf's height for the 43 of 167 leaves that carry one.

**It compiles and has never been executed.** The link was still running when the
lane ended and was stopped; there was no working `doomxr.exe` at that point.
It is committed rather than left loose only so it does not sit unexplained in
someone's working tree — treat it as a proposal, not a result.

**So: build, run, and look at a door before believing anything about it.**
`captures/doorskin2.cfg` captures door01 from 200 units, which is the distance
that makes the fault legible (at 48 the leaf fills the frame and the picture is
uniformly black).

**It is also an experiment with a binary outcome.** Polyobject walls render with
front == back == the sector the leaf *occupies* (`hw_bsp.cpp:707`), which may
mean the leaf's own sector planes never reach the renderer at all.

- **the 43 overridden doors visibly shorten** → the planes do drive the render,
  and the remaining 116 need the fit-to-size default (below).
- **nothing changes** → they do not, and the whole approach to the door's
  appearance has to move to the wall-processing path instead.

### 2.2 The door leaf renders BLACK when closed — cause not found

Reproduced from 200 units: the wall draws its stonework and carved arch, the
doorway is a **pure black rectangle** — the clear colour, not a dark surface.

**Eliminated, each by measurement. Do not re-chase these:**

| candidate | ruled out by |
|---|---|
| no art | all 120 leaf sides carry real named art (`ROTH_DEMO_T00100` ×23 …) |
| the colour key | counter reads 0 |
| sector light | every leaf inherits **lightlevel 160** |
| `R_RothShade` | its light-0 case returns *unshaded raw texels* — **bright**. It cannot output black |
| polyobject spawn | 30 built, 30 spawn + 30 anchor, no errors |
| placement | **0 of 30** in a `SSECF_POLYORG` subsector |
| the leaf being absent | the use ray hits it and swings it |
| z-fighting when closed | open and closed are **identical** black |

So the slab is present, collidable, correctly skinned, correctly lit, correctly
placed — and does not draw. What is left is the wall-processing path:
polyobject segs take the **two-sided** route with front == back, so only
`DoMidTexture` can draw a leaf. Hexen poly doors use that same route and render,
so the mechanism works and something about ours differs.

### 2.3 The fit-to-size default — two candidates measured and killed

`TEXTURE_MAP_OVERRIDE` is **0 for 116 of 167** leaves, which `roth_raw.h` calls
fit-to-size. So it is a per-door override of a default, and the default is
unknown.

- **The two-sided opening is NOT it.** `doorgeom` now measures it: **0 of 330**
  broad faces have an opening shorter than their door sector. Fitting to the
  opening gives exactly the full-height slab that is wrong.
- **Next, unmeasured:** fit-to-size most likely means the **texture's own
  natural height**. The override magnitudes fit — −100, −55, −50 are what 50-,
  27- and 25-texel doors come to at two world units per texel, and a door
  picture is not a tiling texture. Measure the door skins' pixel heights against
  the override values where a leaf has both; if a −100 door carries a 50-texel
  skin, the rule is settled for both cases at once.

### 2.4 Still open in the trigger layer

- **The face direction mask is implemented but has nothing to bite on**, and the
  *use* path cannot drive it: `P_UseTraverse` gives no hit height (its `optpos`
  is the player's **feet**), so `p_spec.cpp` deliberately passes `nullptr`.
  Driving it properly needs one aimed pick — the same thing the floor probe
  wanted, and the shape the original actually uses.
- **The authored bounding box** on both click channels: unimplemented.
- **Untestable in STUDY1** (the data simply is not there): the leave refire
  (`+0x06 & 0x40`), the linked-platform variant, platform-top `0x19`, and the
  facing mask's *blocking* half.
- **No examine input exists**, so `0x32`/`0x31` are bound and unreachable.
  STUDY1 has one `0x32` and zero `0x31`, so this map cannot test it anyway.

---

## 3. What cost the most, and must not cost it again

### 3.1 Two rig faults were corrupting results all session

**`i_pauseinbackground` defaults to true**, and `capture.ini` inherited it. The
game **pauses the moment its window loses focus** — every time the owner clicks
back to their own work — so the queue stops draining and the run looks hung.
Hours went into "frozen game" diagnosis that was this. Now `false` in
`capture.ini`; set it in any new capture config.

**A launch steals keyboard focus.** One capture came back showing GZDoom's
`SAY:` prompt with the owner's typing in it — their keystrokes were going into
the game. Warn before a run, and do not run while they are mid-sentence.

The WAITING diagnostic in `RothDiff_RunPending` **cannot report either**: it only
prints when the ticker calls it. Silence from it means the ticker is stopped,
which is a different fault from the queue being ineligible.

### 3.2 Always run with `-config`

`doomxr.exe -config tools/rothdiff/captures/capture.ini …`. Without it a run
reads *and writes* the owner's `doomxr.ini` — the whole §3.5 archived-cvar leak.

`capture.ini` is **a copy of the owner's ini with `vr_mode` forced to 0, on
purpose**. Two attempts at writing a minimal one cost the owner a disrupted
session each: the first inherited `vr_mode=15` and **took over the headset while
they were wearing it**; the second defaulted to **fullscreen at 3840×2160 on
OpenGL** and never loaded the map. Do not re-derive those settings.

`vr_mode` is `CVAR_GLOBALCONFIG`, so **checking it once early says nothing about
later runs.** The owner changed it mid-session and the next launch went to VR.

### 3.3 Instrument traps

- **`+exec` runs BEFORE the deferred `map`.** A console command in a cfg that
  inspects the level finds none — `roth_trigger_list` printed "no Realms level
  loaded". Anything needing the built level belongs in the **load report**, or
  must be deferred like the capture queue.
- **`roth_lighting 0` and `roth_shade_debug` cancel each other.** The debug mode
  is packed into `uGlobVis` only inside `if (Level->RothLighting)`
  (`hw_drawinfo.cpp:1831`). Three settings in a row produced byte-identical
  frames before this was spotted — the cvar was *inert*, not ineffective.
- **A lit screenshot cannot answer "is there a texture here."** Realms is dark
  enough that a door at 48 units photographs as uniformly black and reads as a
  broken capture. 200 units shows the wall beside it.
- **`vid_setsize` in a cfg hangs the run.** `pair2.cfg` carries one that
  `LANE_START.md` records as working.
- **The load report is currently returning 350-byte stubs.** Not diagnosed. Use
  `Printf` (which reaches `doomxr-log.txt` reliably) for anything that matters.
- **Do not run two builds at once.** This lane did, against §9's rule, and may
  have been causing some of its own LNK1103s.

---

## 4. Instruments added

| | |
|---|---|
| `rothdiff_sector <n…>` | stand in each sector in turn — a sector was not addressable before |
| `rothdiff_usefloor <n…>` | the same, pitched 80° down, pressing use: the `0x19` test |
| `rothdiff_shoot <side…>` / `op 1a` | stand off a wall and fire a hitscan: the `0x1a` test. `op` resolves the sides itself at drain time |
| `rothdiff_usewall <side…>` / `op 18` | the same with the use key: the `0x18` test |
| the load report | per-opcode trigger table with each event and whether anything delivers it; the sectors and sides carrying each opcode; the record-flag histogram **per record**; door leaf skins by name; leaf light |
| `roth::ReportPolyobjects` | where each leaf ended up and whether the renderer skips it |
| `doorgeom` "the vertical" | door heights, `TEXTURE_MAP_OVERRIDE`, and the opening. Every prior measurement in that tool was in PLAN, which is how a wrong door height survived |
| the rig abandons its queue on a map change | poses are authored for one map, and the level logic can change the map |

---

## 5. One thing to hold on to

**Three of this lane's findings were documents being wrong, and two of those
documents were written the previous evening.**

The last handoff's "THE EXACT NEXT STEP" named `FireSectorTriggers` and the
classification as what stood between the owner and a door. Neither was, and its
final instruction — "then wire a caller for `FireSectorTriggers`" — would have
*introduced* a bug, because `0x19`/`0x31` are floor clicks and a sector-entry
caller would fire them on walking through a doorway. The code comment that
disagreed with it was right.

A load-report line claiming the unimplemented opcodes "need the effect pool"
stayed after the pool was built, and sent this lane at work already finished.

And `dir_mask1` is not a direction mask at all — it selects a **band of the
wall**, mid/lower/upper. Built as named, it would have restricted 24 of 26
`0x18` records to one quadrant of player facing and silently broken them.

The project rule already covers this and was not being applied: *the running
original beats ROTH.C's code, which beats its comments, which beat our
documents* — **and "our documents" includes the one written three hours ago.**
The ordering is about distance from the thing itself, not age. A conclusion
drawn in a document is only ever *read*.

Where a page says "the likely reason", read an open question with a candidate
attached. Where it reports a number, trust the number.
