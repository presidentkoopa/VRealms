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
player's five measurements, **only height had ever been ported.** Now four:

| | the original | this port |
|---|---|---|
| height | 144, doubled from meta[0x0a] | ✅ 154 / eye 144 |
| **max step-up** | `2*meta[0x0c]+1` = 65 | ✅ **65** (was 24) |
| **radius** | `0x1c` = 28, hardcoded | ✅ **28** (was 16) |
| **use range** | pick depth < 600, units **[open]** | ⚠️ **600 PROVISIONAL** (was 64) |
| **min fit** (headroom) | `2*meta[0x0e]` = 96 | ❌ `MinFit()` parsed, **used by nothing** |

**The radius is `0x1c`, not the `0x70` that `GAME_core.md` §3.3 calls "radius".
Read ROTH.C before this one.** `move_player_with_collision` builds every
*other* entity's block from a single `r` (`E/collision_physics.c:1592-1600`):

```
box_max = r    box_min = -r    radius = 4r    radius_sq = 64r²    +4 = 65r²
```

and the player's hardcoded block (`:1687-1692`) satisfies **all five** at
`r = 28`: `0x1c`, `0x70 = 4·28`, `0xc400 = 64·28²`, `0xc710 = 65·28²`. `0x70`
is the derived `4r`. Taking the doc at its word would have made him **four
times too wide**.

Applied through `UnlinkFromWorld`/`LinkToWorld` — what `A_SetSize` does, because
radius decides which blockmap cells an actor is filed under and a bare
assignment leaves the world holding the old extent. `ForcedPlayerHeight` was
assigning in place and now goes through the same relink.

**Use range was the quiet one.** Doom's 64 is **0.68 m** at 93.5 units/metre,
for a player 1.65 m tall. `UseFlat` *and* the hand pick both read it, and the
pick's own comment says the beam "crosses rooms" — it could not. **M1's first
check, "point at a door across the room", would have failed and looked like the
pick was broken.** 600 is the original's band number read as world units; it is
**not** the calibration `GAME_core.md` §4.4 asks for, and the load report labels
it PROVISIONAL. §5.2 step 8 is the measurement that replaces it.

**min fit — done (`68610d094a`), and it needed no engine change after all.**
What the original tests to let the player through an opening is a SPAN
THRESHOLD loaded from `g_min_fit` (`E/collision_physics.c:1693`), and every
passage check is the same shape — `if (ceiling - floor) < span, blocked`
(`:1048`, portals at `:604-633`). It is `2*meta[0x0e]` = **96**, while the eye
sits at 144: a Realms player walks through a 96-unit gap **standing up**.

The loader had been using `ph + 10` as the collision height. **That was never a
Realms measurement** — it is the eye height plus a guess — and it made every
opening between 96 and 154 impassable that the original walks straight through.
Collision height is now `MinFit()`; the eye stays 144.

The original also *squashes* rather than stopping: `g_player_height` is dynamic,
clamped to `(ceiling - pos)` and eased back to its resting target at 4 a tic
(`E/player.c:313`, `:318-326`) — a hard landing squashes it toward `0x30`.
GZDoom's `viewz` clamp in `P_CalcHeight` gives the same duck for free once the
collision height is the span, so **the original's auto-crouch is reproduced
without new engine code.**

> **THE TRAP THIS NEARLY SET.** The VR units-per-metre was derived from
> `ForcedPlayerHeight`, which was the same number until this change. Left alone
> it would have rescaled the headset's entire world from **93.5 to 58.3** units
> a metre as a side effect of a collision fix. It now reads a named
> `standingHeight`. Verified unchanged at 93.5 in the load report.

### Crouch

**Natural crouch already exists and was switched off.** The OpenXR device sets
`player->crouchfactor = hmdHeight / defaultViewHeight` from the real headset
height — but only when `vr_crouch_use_button` is false, and
`CVAR(Bool, vr_crouch_use_button, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)`
defaults to **true**, so it waits for the button instead. Added
`+vr_crouch_use_button false` to the owner's `RothHands.cmd`.

**Still not ported:** the crouched step-up limit. The original drops
`g_max_climb` to `0x10` = 16 while crouched (`E/collision_physics.c:1698-1701`,
`GAME_core.md` §5); `ForcedPlayerMaxStepHeight` is a constant 65 regardless of
crouch state. Needs the crouch state at move time, so it belongs in the movement
rules rather than the loader.

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
| `44d80c54aa` | Give the player his own width and his own reach |

Branch `workingbuild`. Working tree clean. `build-dxr` is now configured with
`FORCE_NO_LTO=ON` and stays that way until someone reconfigures it.
