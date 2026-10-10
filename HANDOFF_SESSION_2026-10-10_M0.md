# HANDOFF 2026-10-10 — M0 done, M1 is waiting on the headset

Follow `ROADMAP.md`. This session did **M0 only**. M1 is the owner's and needs
nothing built.

---

## 0. The one number that changed, and it was not the port

The port did not move this session. **The measurement of it was wrong and is now
right.**

| | records running | of 3,588 |
|---|---|---|
| census as it reported yesterday | 2,250 | **62.7%** |
| census after the fix (`f465448f1f`) | 1,879 | **52.4%** |

`opcodecensus` listed `0x2b` in `kImplemented`, reasoning that
`RunCommandWithChain` dispatches it "so the player gets it". The player does not
get it. `RunDbase100Record` is:

```c
g.unhandledOps[0x2B]++;
return CMD_NOTHING;          // eval_dialogue_record_by_id -> 0
```

and its own comment says *"Counted, so it does not look like a success."* A
`case` in the switch is not the work. The table mirrors `IsImplemented()`, and
`IsImplemented()` does **not** list `0x2b`.

It cost three things:

1. The headline read 62.7% instead of 52.4% — 371 dialogue records counted as
   running.
2. `0x2b` vanished from "the holes, biggest first", hiding **the single largest
   gap in the game** (371 records, all 44 maps).
3. **M5's acceptance test was already passing.** "Done when 0x2b no longer
   appears in the census" was true before a line of dialogue was written.

`ROADMAP.md` §1 was right all along: 1,879 of 3,588, 52.4%. The tool now agrees
with it exactly. **Census before = census after = 52.4%**, because no dispatcher
change was made.

> Re-read `ROADMAP.md` §4 step 3 before touching the dispatcher. This table
> drifts silently and it is the to-do list every later milestone is scored
> against.

---

## 1. M0, step by step

### Step 1 — the tree links again ✅

`LNK1103: debugging information corrupt` on `roth_runtime.obj` was incremental
LTCG, exactly as the roadmap guessed. The log showed it one line above the
error: *"7986 of 123921 functions were compiled, the rest were copied from
previous compilation."*

```
cmake -S E:\DOOMWork\REMAROTH -B E:\DOOMWork\REMAROTH\build-dxr -DFORCE_NO_LTO=ON
cmake --build E:\DOOMWork\REMAROTH\build-dxr --config RelWithDebInfo --parallel
```

`cmake` is not on PATH. It ships with VS:
`C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`

Verified at the instrument, not just in the cache: `FORCE_NO_LTO:BOOL=ON` **and
zero** `WholeProgramOptimization` entries in the generated `zdoom.vcxproj`. Zero
`LNK` errors. The `doomxr.iobj`/`doomxr.ipdb` the roadmap offers to delete were
never there, so that fallback was not needed.

**This produced the first `build-dxr\RelWithDebInfo\doomxr.exe` that has ever
existed in this tree** (19,286,016 bytes). The only other exe was a Debug build
from Sep 28. That absence was the symptom of the broken link, not a second
problem.

### Step 2 — the band-gate count ✅ committed (`468b6a17f2`)

It prints. MEASURED on STUDY1:

```
wall-band gate   74 binding(s) carry a non-zero +0x06 and CAN be refused
                 once a hit position is supplied; 23 answer on any band
                 of the 74, 65 sit on a two-sided line
two-sided sides  78 of 86 wired sides
```

**74 is the number M1 should expect.** Up to 74 STUDY1 triggers can start
refusing now the gate is live. 65 of them sit on two-sided lines — the ones a
stop-at-first trace loses silently, which is why the pick uses `TRACE_Continue`.
If anyone replaces that callback with a plain trace, 65 is what goes dark.

### Step 3 — confirmed by timestamp, not exit code ✅

### Step 4 — the per-map report ✅ (`b9115b02ae`)

**The old diagnosis was wrong and is corrected in
`HANDOFF_SESSION_2026-10-10.md` §8.** Nothing loads the map twice.

`P_IsRothMap` opened the report. `P_OpenMapData` calls it for the real load
**and** for every `P_CheckMapData` existence probe — menus validating episodes,
MAPINFO redirects, the secret-exit lookup. `Log::Begin` opens with `"w"`, so
each probe truncated that map's report.

`justcheck` **cannot** tell load from probe: `P_SetupLevel` passes it as `true`
as well, because all it means is *"do not require the map lumps"*. Gating on it
would have silenced the report on every real load. The report now opens in
`MapLoader::LoadRothMap` — the first point reached only on a genuine load.

| | before | after |
|---|---|---|
| a STUDY1 load | 13 KB report, destroyed by any later probe | one complete 13,670-byte report |
| the other 43 maps | a 0 ms "(nothing counted)" stub each | **no file at all** |
| sitting at the menu | 44 stub reports | **none** |

---

## 2. Launch, for whoever is next

Logs are **not** in the repo root. The root `doomxr-log.txt` is stale (Oct 2).
The live ones are beside the exe:

```
E:\DOOMWork\REMAROTH\build-dxr\RelWithDebInfo\doomxr-log.txt
E:\DOOMWork\REMAROTH\build-dxr\RelWithDebInfo\roth_STUDY1.log
```

**`RothHands.cmd` is on the owner's Desktop**, not in the tree — `ROADMAP.md`
M1 names it without saying so. Don't go looking for it in the repo.

**A `-rothpath` with spaces in it will silently fail.** PowerShell's
`Start-Process -ArgumentList` split
`D:\SteamLibrary\steamapps\common\Realms of the Haunting\DATA` at the first
space; the engine reported `ROTH.RES not found in
D:\SteamLibrary\steamapps\common\Realms`, then sat at the menu with
`map=(none)`. **`-rothpath E:\DOOMWork\_oracle` has no spaces and works.** That
is also what `opcodecensus` takes.

Flat verification run (loads STUDY1 in ~4 s, no headset needed):

```
doomxr.exe -iwad vrealms.pk3 -rothpath E:\DOOMWork\_oracle +map STUDY1 +vr_mode 0 +vid_fullscreen 0
```

`OpenXR unavailable: runtime probe failed. Falling back to vr_mode 0.` is normal
with no headset running, and is how a flat check stays out of the owner's way.
The exe exits `0xC0000409` even on a clean run — the exit code says nothing.

---

## 3. THE EXACT NEXT STEP

**M1. It needs the owner and a headset, and nothing needs building first.**

Launch `RothHands.cmd` from the Desktop, then at the console:

```
roth_pick 1 ; roth_useray_debug 1 ; roth_trigger_debug 1
```

- [ ] point at a door across the room, press: it opens
- [ ] point down at a floor plate, press: it fires
- [ ] a light switch: the light changes and the switch art flips
- [ ] the beam you see and the thing that reacts are the same thing
- [ ] note any trigger that worked before and now refuses — **expect up to 74**

If the beam and the pick disagree, do `HANDOFF_SESSION_2026-10-09.md` §4 step 3
(one hand ray per tic). Once the pick is trusted, step 5 deletes the three old
half-measures.

After M1: **M2, the Realms core and the DBASE100 reader.** `DBASE100.DAT` is
42,124 bytes and sits at `E:\DOOMWork\_oracle\DBASE100.DAT`. It is the keystone
— dialogue and inventory are half of everything missing and both read it.

---

## 3b. The owner could not walk up stairs — found and fixed

Reported from the headset, after M0: *"the scale is so fucking off, I can't walk
up stairs."* The scale is not the cause and the world is not wrong.

`g_max_climb` — the map's own step-up limit — **was parsed and then used by
nothing.** The player kept Doom's `MaxStepHeight` of **24** in a manor whose
stairs are cut for **65**.

```
step up   65 units (Doom's default is 24) -- map says meta[0x0c]=32, doubled +1
```

Read from the retail STUDY1: `meta[0x0c] = 32`, so `2*32+1 = 65`, which is
exactly the 65/66 pair `docs/GAME_core.md` §5.2 names as the acceptance test.
`Metadata::MaxClimb()` was also returning `2*meta[0x0c]` with the **+1
dropped** — on its own that made the tallest authored step in the game the one
step you cannot climb.

`FLevelLocals::ForcedPlayerMaxStepHeight` (`994ca3318b`) is the sibling of
`ForcedPlayerHeight`/`ForcedPlayerViewHeight`, applied in the same place at
player spawn, `0` = class default so it is inert for every non-Realms level.

### Still Doom's, and next

`ROADMAP.md` §2 says to port the parts of movement that are rules. Of the
player's four measurements, **only height was ever ported.** Now two:

| | the original | this port |
|---|---|---|
| height | 144, doubled from meta[0x0a] | ✅ 154 / eye 144 |
| **max step-up** | `2*meta[0x0c]+1` = 65 | ✅ **65** (was 24) |
| **min fit** (headroom) | `2*meta[0x0e]` = 96 | ❌ `MinFit()` parsed, **used by nothing** |
| **radius** | fixed at `0x70` (`GAME_core.md` §3.3) | ❌ never set, still Doom's **16** |

A player 2.75× Doom's height still carrying Doom's 16-unit radius will slip
through gaps the original would stop him in, and the 96-unit headroom span is
what decides whether he fits under a lintel at all — separate from his 154-unit
height. **Verify `0x70`'s units against ROTH.C before setting the radius**; the
spec line quotes `box ±0x1c` and `radius 0x70` together and they are not in the
same scale. Crouch (step-up drops to 16, `GAME_core.md` §5) needs the crouch
state and belongs with the movement rules, not the loader.

## 3c. VR_UnitsPerMeter called itself

```c
return gVRUnitsPerMeterOverride > 0.f ? gVRUnitsPerMeterOverride
                                      : (float)VR_UnitsPerMeter();   // itself
```

The fallback is supposed to be the `vr_vunits_per_meter` cvar. Every VR frame
goes through this function — IPD, stereo separation, HUD projection, head
height, positional movement — so the only reason it was survivable is that a
loaded Realms map always sets the override and takes the other branch. The
archived player preference could never be reached. Fixed in `a5087ca27c`.

**REMAROTH-only**: UZDXREMA has no `gVRUnitsPerMeterOverride` at all, so no
other lane inherits it. **Known and not changed:** nothing ever clears the
override, so 93.5 persists for the rest of the run once a Realms map has
loaded. Harmless while this tree only loads Realms maps.

## 4. Commits

| | |
|---|---|
| `468b6a17f2` | Count what the wall-band gate can refuse, at load |
| `b9115b02ae` | One load report per real load, not per map lookup |
| `f465448f1f` | The census counted dialogue as implemented; it is a stub |
| `1d4f1608fb` | The roadmap to playable, and the lane docs behind it |
| `e752b84d95` | M0 done: the tree builds, STUDY1 loads, and the census was lying |
| `994ca3318b` | Let the player climb the stairs the map built for him |
| `a5087ca27c` | VR_UnitsPerMeter called itself instead of reading the cvar |

Branch `workingbuild`. Working tree clean. `build-dxr` is now configured with
`FORCE_NO_LTO=ON` and stays that way until someone reconfigures it.
