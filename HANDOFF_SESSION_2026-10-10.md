# Session handoff — 2026-10-10

**Read `LANE_START.md`'s goal block, then `ROTH_PORT_STATUS.md`.** This file
says what moved and what is waiting on a headset.

Four commits. The hands load for the first time, you can point at a thing and
press it, light switches on objects have a channel, and LNK1103 is dealt with at
the root.

---

## 1. WHAT LANDED

| commit | what |
|---|---|
| `b7a1e67f0a` | six natives, so `RS_WorldHands` can load at all |
| `00dcb9420e` | three document corrections, in place |
| `b25d5d2a95` | `ROTH_PORT_STATUS.md` — where the game port actually stands |
| `84d54c5c33` | the pick (step 4) and the object channel (step 6) |

### The order was changed, by the owner

`HANDOFF_SESSION_2026-10-09.md` §4 lists seven steps. **Step 7 was done first**,
on his instruction: get WorldHands in, then find out where the game port stands.
That was the right call for a reason worth keeping — nothing else in the plan is
checkable by him without a hand ray to check it with.

Done: **7, 4, 6**, and step 2's `*usefail` bug. Left: **1** (a measurement only
he can make), **3** (one hand ray per tic), **5** (delete the three
half-measures, and the plan says only *after* 4 is trusted).

---

## 2. THE NATIVES WERE SIX, NOT FOUR

Corrected in place at `HANDOFF_SESSION_2026-10-09.md` §5, including `HandPos`,
which that table marked present when it is absent.

**The method matters more than the number**, because "the ones found so far" is
how it was four: diff every `native` declaration in both forks (1,445 here
against 1,535 in UZDXREMA, 109 absent in all), intersect with every name the mod
calls, and do native **fields** separately — a missing field breaks compilation
identically, and four names matched before turning out to be comments.

**It was not a copy of thunks.** The thunks are five lines each; the `VR_*`
backend they call did not exist here at all and is most of the ~815 lines.

**Still absent, and a bigger job than this was:** `src/r_data/vr_rig.cpp`,
`vr_rig.h`, `vr_avatar.cpp`, `vr_avatar.h`, `vr_avatar_pose.cpp`. Anything
needing the rig or avatar subsystem — RS_Avatar, RS_VRBody — needs those first.
`HandPos` does not; its setter is in `vk_openxrdevice.cpp`, which we have, and
the rig hunks were a separate concern riding in the same upstream commit.

---

## 3. WHAT TO DO IN THE HEADSET, and what to watch for

```
roth_pick 1 ; roth_useray_debug 1 ; roth_trigger_debug 1
```

Launch with **`RothHands.cmd`** (on the desktop). Point at a door across the
room and press use; point down at a floor plate and press.

**`roth_pick` is archived and default OFF**, and inert unless a Realms level is
loaded, so it cannot affect any other game.

**EXPECT SOME TRIGGERS THAT WORK TODAY TO START REFUSING.** `roth::ActivateLine`
had exactly one caller and it passed `nullptr`, so `part` was always 0 and the
wall-band gate has never run on any channel in this port. The pick hands it a
real hit position, so the rule runs for the first time. A trigger that stops
working is that rule firing, not a regression — which is the whole reason this
is behind a default-off cvar rather than simply switched on.

**The §6 count was never made.** Nobody has counted how many STUDY1 face
bindings carry a non-zero `fireFlags` and would now be band-gated. Doing it
headless first would have made this a measured decision instead of a surprise.
It is still worth doing: `roth::SidesWithOpcode(0x18)` against each line's
`ML_TWOSIDED`.

---

## 4. DO NOT UNDO THESE, they are measured

**`TRACE_Continue` is not a style choice.** `rothmap.cpp` gives a Realms face a
sister with `ML_TWOSIDED` and no `ML_BLOCKING`, and `p_trace.cpp` never
*reports* a two-sided line the ray passes through — it is crossed, not hit. So a
stop-at-first trace cannot see a mid-band wall trigger on any interior wall, at
**any** mask value. The old 2D `P_UseTraverse` found them only because it offers
every line it crosses. If someone "simplifies" the callback away, every mid-band
`0x18` goes silent and nothing will say so.

**The band comes from `FTraceResults::Tier`**, not from re-deriving it off a hit
height. `WallPartBit` still exists for callers that have only a height, but the
pick does not use it.

**Forwarding `P_ActivateLine`'s `optpos` is the WRONG fix for the dead gate**,
and the comment at `p_spec.cpp:133-141` explains why: `P_UseTraverse` builds it
as `{start.X, start.Y, usething->Z()}` — the player's **feet**. In a doorway the
feet sit at the back floor, so a mid-texture switch would read as a lower hit
and be refused. The two comments on this subject each claim the *other* path
supplies a hit point and neither does; the pick supplying its own is the only
route.

---

## 5. TWO DOCUMENTS WERE WRONG ABOUT OBJECTS, found by reading ROTH.C

Both corrected in place at `docs/GAME_core.md` §5.2 row 1.

1. **"0x08 / 0x02" are two BASES of one category, not two opcodes.**
   `run_leftclick_object_trigger` (E/raw_commands.c:2920) branches on the base
   byte at `+0x03`: base 2 is a light switch, base 8 runs the chain, anything
   else acts and does nothing. The reader already stores that byte as
   `Command::opcode`.
2. **`word[obj + 4] ^= 1` is not a state flag.** `+0x04` is the texture pair —
   index in the low byte, source in the high one — so flipping bit 0 swaps the
   object to its **neighbouring sprite**. That is how a lever shows which way it
   is thrown: the two positions are two pieces of art. Implemented the way
   opcode `0x0d` already repaints a prop, so both land on one convention.

Keying, for the record: a record's key at `+0x08` against the object's own id at
`+0x0e` (`Object::commandID`, which `roth_raw.h` already named as exactly that),
skipping records whose `+0x02` carries bit 3. A **third key space** beside faces
and sectors, which is why these fell through the binding loop entirely.

---

## 6. THE SWITCH MAKES NO SOUND, deliberately

Realms' SFX are step 3 of the game port and **no sound path exists in this tree
at all**. An engine sound there would be the wrong noise in the right place,
which is harder to notice than silence. `g.unhandledSfx` records what was asked
for. Do not substitute a Doom sound to make a demo feel finished.

---

## 7. LNK1103 IS FIXED AT THE ROOT

`CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO` carried `/INCREMENTAL`, and incremental
LTCG was writing a `doomxr.iobj` that corrupted on every change to a large file
— **three failures in one session**. Now `/INCREMENTAL:NO`.

`LANE_START.md` §6 says not to delete `doomxr.iobj` unless LNK1103 has actually
appeared. Worth knowing when it does: **deleting the named `.obj` is not
enough** — the corruption is in the incremental database, `doomxr.iobj` (1.5 GB)
plus `doomxr.ipdb`, at `build-dxr/src/zdoom.dir/RelWithDebInfo/`. A search from
`build-dxr` at depth 3 misses them; they sit four levels down. Links are slower
now and that is the trade.

---

## 8. OTHER THINGS LEARNED THE HARD WAY

- **`git apply` fails a patch ATOMICALLY and says "applied cleanly" first.** The
  `HandPos` commit also touches five files that do not exist here; git listed
  the files it could apply, then rolled everything back. It looked like success.
  Check the symbol actually arrived, not the tool's output.
- **`vk_openxrdevice.cpp` is LF here and CRLF upstream.** A naive diff reports
  15,458 changed lines where the real count is 624. Do not port that file by
  eyeballing a diff.
- **An upstream commit's position in a struct may be wrong HERE.** Upstream
  appends `mBeamScroll` after `mSweepPassedColor`, because `mLinearizeDepth` and
  `mGpuParticleParams2` did not exist there yet. Here they do and they come
  first. The GLSL blocks agree with `HWViewpointUniforms` **by offset**, and a
  layout mismatch does **not** fail shader compilation — it reads the wrong
  bytes. Verified visually afterwards (lighting and the beam are correct), but
  it would have been silent corruption.
- **`roth_<MAP>.log` could not be trusted as written. FIXED 2026-10-10
  (`b9115b02ae`); the diagnosis in this bullet was wrong.** The symptom was
  right — 1 ms and "nothing counted" for a real STUDY1 load — but nothing loads
  the map twice. `P_IsRothMap` opened the report, and `P_OpenMapData` calls it
  for the real load *and* for `P_CheckMapData`'s existence probes: menus
  validating episodes, MAPINFO redirects, the secret-exit lookup. `Log::Begin`
  opens with `"w"`, so every probe truncated that map's report. One run left a
  0 ms stub for **all 44 maps**. `justcheck` cannot separate load from probe —
  `P_SetupLevel` passes it as `true` too, because all it means is "do not
  require the map lumps". The report now opens in `MapLoader::LoadRothMap`, the
  first point reached only on a genuine load. Verified: a STUDY1 load writes one
  complete 13,670-byte report and no stub for any other map; sitting at the menu
  writes none at all.
- **ZDL's `iwad=` field overrides anything in its extra-args box.** `Roth.zdl`
  and a first attempt at `RothHands.zdl` both loaded `doom2.wad`, which gives
  Doom's menu, Doom's difficulty screen, and a hang on New Game because STUDY1
  is only in `vrealms_iwad`'s MAPINFO. Either register `vrealms.pk3` as an IWAD
  in ZDL or use `RothHands.cmd`.

---

## 9. THE EXACT NEXT STEP

**Point at a door and press it.** Everything above is unverified in a headset,
and two specific things need eyes: whether the ray goes where the beam points,
and which triggers the band gate now refuses.

After that, in order: the §6 headless counts, then step 3 (one hand ray per
tic), then step 5 (delete the half-measures) — and step 5 only once step 4 is
trusted, which is what the testing is for.

**Still undecided, and it is the owner's:** `ROTH_PORT_STATUS.md`'s one
question. Build the Realms core that `ROTH_GAME_PORT.md` §2 asks for and move
step 1's work into it, or keep growing `roth_runtime.cpp` against engine
structures. The game port cannot get much further without an answer, because
step 1 is the thing that would have to move.
