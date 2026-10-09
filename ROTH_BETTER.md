# Different, or better
### Things Realms does that we could adopt, and things we should stop inheriting from Doom
*A living document. Append to it; do not tidy it away. Started 2026-09-25.*

The point of this file: reimplementing Realms inside the fork keeps turning up places where the
original's approach is **cleaner than the Doom one we inherited**, or where a Doom assumption is
costing us something we don't need to pay. Those are worth capturing even when we are not ready to
act on them, because several of them would improve the **main UZD engine** and widen what maps can
be designed for it — not just Realms.

**Status vocabulary:** `verified` = read out of ROTH.C or our own source, `measured` = counted across
the 44 retail maps, `idea` = plausible and not yet checked. Do not promote anything to `verified`
without reading the code.

---

## 1. The 35 Hz game tick

`TICRATE = 35` in `src/doomdef.h`. *(verified)*

We inherited it, and it is only load-bearing for **demo playback and netplay determinism**. This is
single-player VR: no co-op, no netplay, no demos anyone will ever replay. The user raised this
directly — "35 is zdoom world tick rate, we can move off of that since we aren't co-op or netplay in
this."

Why it matters more in VR than on a monitor: anything driven by the game tick — door swings, platform
motion, a prop you are holding — updates 35 times a second, and in a headset you are looking at it
from six inches away with your own head moving at display rate. Interpolation hides some of that but
not the input-to-motion latency on things you are physically manipulating.

**Cost:** it is not a constant swap. Doom-lineage code has speeds, gravity and durations expressed in
units-per-tic scattered through it, and a lot of it assumes integer tics. Wants a survey before anyone
commits.

**Status:** raised, not scoped. Deferred by the user ("later").

---

## 2. Intermediate floors are a first-class sector field

Realms gives **every sector** an optional single intermediate floor — a `MidPlatform` record with its
own top and underside texture, two heights, and shift/scale bits. One record, inline, pointed at from
the sector. *(verified — `roth_raw.h`, `MidPlatform`)*

There are **2,299 of them across the 44 maps, and `TOWER1` alone has 338.** *(measured)* This is not
an edge case in Realms; it is how the game builds tables, ledges, balconies and walkways.

Doing the same thing in GZDoom means a **3D floor**, which needs a **separate control sector**
somewhere else in the map, wired up by tag. That is a mapping ritual to express "this sector has a
shelf in it", and it costs a sector plus a tag per shelf. For 2,299 of them it is absurd.

**The idea:** a native per-sector intermediate floor — two heights, two flats, a scale — rendered and
collided directly, no control sector. It is strictly less general than a 3D floor (one per sector, no
stacking) which is exactly why it can be cheap.

This is the strongest candidate in this document for something that would **improve UZD mapping
generally**. Every Doom-lineage mapper who has built a table out of a control sector would use it.

**Status:** idea. Needs a look at how the fork's 3D-floor rendering path is structured before anyone
estimates it.

---

## 3. In-place polyobjects via a zero-delta anchor

Found while working out how Realms doors should work. *(verified — `maploader/polyobjects.cpp`)*

`TranslateToStartSpot` computes `delta = origin - StartSpot.pos` and shifts the whole polyobject by
it. The Hexen convention is to draw a polyobject in a void area and let this move it into place. But
**if the spawn spot and the anchor are the same point, delta is zero and nothing moves** — the
polyobject is built where it already sits.

So a swinging door does not require a void area at all. Nothing in the engine needs to change; the
convention was never a requirement.

**Why it is general:** any UZD map wanting a swinging door, a rotating platform or a moving wall can
skip the void-area layout entirely. Worth writing up for mappers regardless of what Realms does.

**Status:** verified, unused so far. Will be exercised by the Realms door work.

---

## 4. Doors are hinged slabs, and Doom has no concept of one

A Realms door is a **4-walled sector that rotates about a hinge corner** toward a stored target point.
*(verified — `doors.c`, `setup_door_swing_geometry`)* Doom doors move a ceiling down. These are not
the same thing and never will be.

Realms is right and Doom is wrong here, for our purposes: a hinged slab is what a door *is*, and in VR
the difference is enormous — you can watch a slab swing past you, reach around it, be pushed by it.
A ceiling coming down is an abstraction that reads as an abstraction the moment you are standing
inside it.

We can express this today with `PODOOR_SWING` (see 3). What is worth considering later is making the
**hinged slab a first-class thing to author**, rather than four explicit-line tags plus two
synthesised map things, because right now saying "this is a door" takes six pieces of setup.

**Status:** mapping planned via polyobjects. The authoring sugar is an idea only.

---

## 5. Walls can be addressed as a group, by ID

Realms names a wall by putting a `faceID` on its texture-map record, and **several walls can share
one ID** — so one command repaints, animates or breaks the whole group. `gather_faces_by_id` collects
every match. *(verified — `raw_commands.c`; see `ROTH_COMMANDS.md`)*

Doom tags *sectors* well and *lines* poorly. There is no equivalent of "all the walls that are this
one surface", so a mapper repeats the special on every linedef and hopes they caught them all.

**Related and separate:** Realms puts the texture assignment in a **shared record** the faces point
at, rather than on each face. Change the record and every face using it changes. That indirection is
free grouping, and Doom's per-sidedef texture fields cannot express it.

**Status:** verified for Realms. Whether UZD wants a line-group concept is an open design question,
not a proposal.

---

## 6. Progress flags are one global persistent bitmap

`((recordCount + 0x20) & ~0x1F) >> 3` bytes — **448 flags for the retail data** — allocated once at
game start, one bit per DBASE100 record, written whole into savegame chunk 6. *(verified —
`game_core.c`, `savegame.c`; count read from `DBASE100.DAT`)*

Flat, cheap, and **survives level changes**, which it must: Realms is one continuous story, not a
series of disconnected arenas. Doom's map-local state plus ACS world variables covers this, but only
because someone bolted it on; the shape Realms uses is simpler and is the shape a story game actually
wants.

**Status:** verified. We need something like it for Realms regardless. Whether it should be an engine
facility or ZScript is undecided.

---

## 7. Level logic is data, not a compiled script

Realms levels carry small programs — lists of records with an opcode, a key and arguments, chained by
index. 5,531 records and 1,937 chains across the game; the Study alone has 661 records. *(measured)*
No compiler, no separate lump, no build step. The level file *is* the script.

ACS is more powerful and it is also a toolchain. For the kind of thing Realms actually does — open
this, give that, check a flag, branch — a data-driven table is easier to generate, easier to diff and
easier for a tool to edit live.

Not a proposal to replace ACS. Worth noting because **we have to interpret this table anyway**, and
once we have an interpreter, a data-driven logic layer exists in the engine whether we planned one or
not. Worth deciding deliberately what it becomes.

**Status:** decoded, interpreter not built.

---

## 8. Small things worth remembering

- **Per-sector flat scale in two bits.** `2^s` world units per texel, bits 4-5 floor and 2-3 ceiling.
  *(verified)* Doom has no per-sector flat scale at all without UDMF properties.
- **The stored wall extent is authoritative** and can disagree with the measured geometry.
  *(verified)* Realms trusts the authored number over the maths. Doom always recomputes. Theirs is
  more forgiving of imprecise geometry — a useful instinct.
- **The player is sized at load, not authored.** The map records 72 and the engine doubles it to 144.
  *(verified)* Cost us real time when we read the stored number and concluded the world was twice too
  big. A reminder that a stored value is not always the effective one.
- **`playerHeight` is not universal** — 72 on 41 maps, **64** on `ABAGATE2`, `AQUA1` and `DOPPLE`.
  *(measured)* Per-map, never hardcoded.

---

## 9. The sky is a 2D band, and in VR that is not good enough

**Realms has no skybox.** `render_parallax_sky_columns` (`renderer.c:5389`) blits a
single 2D image in vertical columns straight into the framebuffer. *(verified)*

- The image is the map's own, from metadata `+0x18`, and it **animates** -- if the
  block's flags carry `0x100` it advances a frame every draw. TOWER1's sky is an
  animated entry, so the original genuinely has moving cloud.
- The parallax is one line: the source column is offset by `view_angle * 2`. Turning
  scrolls the band horizontally. That is the whole effect.
- **Vertically it SHEARS 1:1 WITH PITCH.** *(Corrected 2026-10-01. This entry
  said "it does not move at all", which is wrong, and the plan below was built
  on it.)* The start row is
  `[0x909f6] = viewport_top_margin - (pitch * vertical_scale >> 7)`
  — `render_world.c:316-319` reads `[0x90a6c]`, which `render_world.c:66-70`
  sets from the clamped view pitch at `0x89ee8` (`player.c:90-95`). Look up and
  the band slides with everything else, which is consistent: Realms' look
  up/down is a shear of the whole finished picture.
- **At the bottom it CLAMPS, not wraps** — the last source row is smeared
  downward, and there is no modulo anywhere in the vertical path
  (`renderer.c:5560-5579`, `:5491-5519`). This entry previously said "wrapped on
  the block height".
- Found by two independent readers on 2026-10-01, the second one specifically
  trying to refute the first. Neither could break either correction.

That was fine on a 320-pixel screen. **In a headset a band with no vertical response
and no positional parallax reads as flat wallpaper the instant you tilt your head**, and
Realms leans on it heavily: 6,208 sky surfaces across 36 of the 44 maps. *(measured)*

So this is a place to **deliberately diverge rather than reproduce**. GZDoom's own sky
already projects onto a cylinder and responds to pitch, which is better than the original
for free, and is what the loader now uses.

**Where to push it further (the user's own list, 2026-09-26):**

- **Darkness and fog.** We already have the fork's fog and the real Realms shade model
  transcribed in `ROTH_LIGHTING.md`. An outdoor sky lit and fogged to match the sector it
  is seen through would do far more for the manor's atmosphere than the flat band ever
  could -- and Realms' own model already thinks in terms of how fast dark closes in, so
  the two are a natural fit rather than a bolt-on.
- **Weather.** Rain, mist, drifting cloud. The original could not have afforded any of
  it; the reference screenshots show rain streaks painted INTO the window artwork,
  because that was the only way to get them. Real weather in front of a real sky is the
  version of that idea the 1996 team would have shipped if they could.
- **A real skybox or portal** for the genuinely outdoor maps, so leaning toward a window
  changes what you see through it. That is the whole promise of the headset and it is
  exactly what a 2D band cannot do.

**Status: PHASE 2, not now.** The immediate goal is a 1:1 Realms, so the faithful 2D
band is what gets built first -- everything above is for the later VR pass. This entry
was briefly written as a live divergence, which was drift: "it would be better in VR"
is phase 2's argument, and this file RECORDS such ideas rather than authorising them.

---

## 10. Props sunk into the floor

**What Realms does.** A standing sprite's base sits at `objZ - shift`, where
`shift = (int16)(2*(modifier & 0x0f) + u16[0x84aba])` — so a prop's base can be
as much as 30 world units BELOW its own Z, and in STUDY1 68 of the 71 props
carrying a non-zero nibble end up partly buried in the floor they stand on.
Three independent readings of `renderer.c:7756-7760` and `:6553-6555` agree this
is the only possible reading, and that both alternatives are worse: flipping the
sign floats every prop up to 30 units above its floor, anchoring the top buries
it by its entire height. *(verified)*

**What we do.** The same thing, as of `8bc39fa`. 1:1 first.

**Why it might be better not to.** The owner's earlier instinct was to leave the
shift off so props sit cleanly on their floors — *"can't imagine the gaming gods
being mad at me for fixing this"*. In a headset, at eye level and in stereo, a
table leg disappearing into the floorboards is far more noticeable than it was on
a 320-pixel screen with a shear for a camera. It may genuinely read as a bug to a
player rather than as period charm.

**Cost to flip:** one line in `roth_objects.cpp` — the `p.z +=` in the nibble
block. The shift is already computed and counted either way.

**Status: 1:1 FOR NOW.** Recorded here so the decision is not lost, not because
it is authorised. Revisit in the VR pass with the headset on.

---

## 11. The world's scale in a headset

**What Realms does.** Nothing: it has no notion of a metre. Its player is 154
units tall with the eye at 144, and on a flat screen that is a pure ratio -- it
only matters relative to the geometry, which is why making the player the right
number of UNITS tall was enough for the flat view.

**What we do.** A headset asks a question the original never had to answer: how
many world units is a real metre? `vr_vunits_per_meter` is 34, which is tuned
for Doom's 56-unit player. Left at that, a Realms player stands 4.5 m tall and
the manor is enormous from inside the headset -- the owner's words on
2026-10-02 were "I am super super small, the world seems massive".

`VR_UnitsPerMeter()` (hw_vrmodes.h) now answers it, and the Realms loader sets
a per-level override of `34 * 154/56 = 93.5`, which puts the eye at 1.54 m.
Derived from the player height rather than tuned by eye, so any map stating a
different player height gets the matching scale for free.

**Status: BUILT, 2026-10-02.** This is not a divergence from 1:1 -- the flat
screen cannot reach the code path, and the geometry is untouched. It is the
answer to a question 1:1 does not ask. The one judgement in it is that a Realms
person is the same real-world height as a Doom person; if the owner wants to be
taller or shorter in the manor, this constant is where that lives.

**NOT the cvar.** It is `CVAR_ARCHIVE | CVAR_GLOBALCONFIG`: writing it would
resize every other game this engine runs, permanently, from loading one Realms
map.

---

## 12. The candle cone follows the head, not the body

**What Realms does.** Shades a lantern sector by a cone about the VIEW axis. On
a desktop the view and the body are the same direction, so the original never
had to tell them apart.

**Why it is wrong in VR.** They are not the same in a headset. The lit pool
swings to wherever the player looks, so turning your head re-lights the room
around you. The owner's words on 2026-10-02: "motion sickness city when it is
constantly reorienting itself around where I look, as opposed to what direction
my body is facing".

**What it would cost.** A per-frame axis the shaders measure the cone from,
defaulting to the view so nothing else moves. Attempted on 2026-10-02 and
REVERTED: the sign was confirmed correct by a +/-30 degree test (+30 swings the
cone left, so `body - view` holds it still), and the uniform plumbing was
verified across all three declarations -- but a trustworthy flat-screen control
could not be obtained while `vid_fixgamma` was poisoning every capture, so it
was pulled rather than shipped unverified. Re-applying it is cheap and the sign
is known.

**Status: WANTED, NOT BUILT.** The owner asked for it directly, which makes it
the first deliberate VR divergence to be authorised rather than recorded.

---

## 13. The line between 1:1 and free — the owner's framing, 2026-10-09

Status: `decision`. Not an idea to weigh; the rule the rest of this file hangs off.

Every entry here used to carry an unspoken worry: how far are we allowed to depart from 1996? The
owner settled it. **What must be 1:1 is the LOGIC.** What is free is the **presentation and the
interface**.

| must match the original | free to diverge |
|---|---|
| which triggers fire, and when | how you reach for a thing |
| item state, what a thing does when used | how it is lit |
| dialogue branching, the story flags | whether it is a sprite or a voxel |
| save state, combat rules | what a coat does when you open it |

This cuts work both ways and it is worth saying plainly: a door being the wrong *height* is still a
**bug** — wrong-looking is wrong. "Presentation is free" means free to be *better*, not free to be
broken.

And the companion rule, which is about respect as much as method: **ROTH.C is not to be made
obsolete.** Chillex's lift is the desktop reference implementation; REMAROTH is the same logic with a
VR front end. The two coexist. It is also the only way we can ever *prove* the logic matches —
`HANDOFF_REMAROTH.md` §0 notes that editing it "costs it its authority as an oracle", and an oracle
is exactly what it is.

---

## 14. The interface is already a pointer — the hand beam

Status: `idea`, with the groundwork `verified`. Owner's call, 2026-10-09.

**What Realms does.** It is a hybrid adventure/FPS driven by a cursor over the world. Left-click
uses, right-click examines, and the engine classifies whatever is under the cursor as wall face,
floor, mid-platform, object or door and routes each differently (`GAME_core.md` §4.5;
`dispatch_entry_command_trigger`, `E/raw_commands.c:3008`).

**Why that matters for VR.** Most 1996 adventure interfaces need reinventing for a headset. This one
does not — it needs *connecting*. A controller beam with two buttons is the same interface described
in 1996 terms. Primary fire is use, secondary is examine, and that mapping is the original's own, not
an invention.

**What we have already.** `RS_WorldHands` has a tuned pointer — `RS_Cone` in `rs_distance.zs`, which
builds the ray from `AttackAngle`/`OffhandAngle` plus pitch and roll, scores candidates angle-first
then distance, and already draws a beam and a highlight. REMAROTH carries the same hand fields
(`p_vrdemo.cpp` records `AttackPos`, `AttackAngle`, `MainHandRoll` and the off-hand set), so the mod
and this engine already agree on where a hand points.

**The gap.** `RS_WorldHands` points at **actors**. Realms' interactivity is mostly on **surfaces** —
a light switch is a patch of wall with a script bound to it, a floor trigger is a sector. Only props
and pickups are actors. So the hand mod supplies the ray; the engine has to classify what the ray
hits in Realms' own terms and route it. One pick, like the original's.

**Cost.** Moderate engine work in REMAROTH, and it *deletes* three existing half-measures that each
approximate part of it: the hook on Doom's line-use, the separate floor probe, and the door-leaf
special case.

---

## 15. A coat you open, instead of a grid on a screen

Status: `idea`. Owner's, 2026-10-09.

**What Realms does.** The inventory is a grid on a flat screen. It is also central — this is an
adventure game, and you carry keys, documents and junk as much as weapons.

**The idea.** A player body (`RS_Avatar`) wearing a trenchcoat. You open the coat with your hands and
the inside is the inventory — weapons selectable from it, and by extension the rest of what you are
carrying.

**Why it fits Realms specifically.** This is not a generic VR flourish. The README already says the
original's grid "should be something you reach into" in a headset. A coat *is* that, and Realms'
inventory is large and used constantly, so the payoff is bigger here than in a shooter.

**What it needs first.** The inventory has to exist as logic before it can have a body: DBASE100, the
item records, pickup and use. §13 applies — the *contents and behaviour* must be 1:1, the way you
reach into them is free.

---

## 16. Voxelising the sprite content, and where the source data already is

Status: `idea`, with one strong `verified` lead. Owner's, 2026-10-09.

**The problem.** Flat billboards are the thing that gives a sprite game away in a headset. Stereo
vision makes cardboard obvious in a way a monitor never does, and it is worst for an object held at
arm's length.

**Scope, narrower than it first looks.** Realms is not purely sprite-based — it already has real 3D
mesh furniture through the DAS mesh path. The voxel target is the sprite content: items, some props,
characters.

**The lead that makes this tractable.** The examine close-up is **a GDV video from DBASE300**, played
in a framed window (`GAME_inventory.md` §5; `E/dialogue_ui.c:1299-1541`), and the close-up mode flags
include `0x00 = loop`. A looping close-up of an object is **a turntable** — a full rotation, at
close-up resolution, per item, already authored by the original's artists. That is far better voxel
input than the 8-view world sprites, and Realms' colour-key transparency means the frames should key
to clean silhouettes.

**Two things converge.** Those GDVs have to be decoded anyway — Examine is a real feature we owe the
player. And DBASE300 is one of the files the `_oracle` mirror does **not** contain; it lives in the
retail `DATA` directory (the CD content). See §18.

**Where to spend it, in order:**

1. **Items you pick up and hold.** Small set, biggest payoff, best source data, pairs with §15.
2. **Static props you walk around.** Auto-generated from the 8 directional views.
3. **Characters — be careful.** Realms' expressiveness lives in digitised performances. Voxelising a
   photographed actor tends to look *worse*. Flat-but-right may beat solid-but-wrong.
4. **Distant scenery — leave it.** Billboards read fine at range.

**A cheaper middle worth trying first:** cross-billboards or depth-offset sprites fix most of the
flatness for a fraction of the work. Try one object before committing to a pipeline.

**The constraint it brushes.** §0's hard rule says nothing derived from the game is "written to disk,
cached, converted or shipped". Voxelising is converting. Per §13 the owner's line is *logic* 1:1, and
the half of that rule that genuinely protects us is **shipped** — the fork is never distributed. A
baked voxel cache is therefore the owner's to choose, but it should be chosen deliberately, and the
README's "nothing is extracted or converted" would need to stop claiming otherwise.

---

## 17. Atmosphere, bodies and the few guns — the mod shelf

Status: `idea`. Owner's, 2026-10-09. Recorded so they are not lost, not because they are next.

- **`RS_Radiance`** — fog, darkness and atmosphere. Realms *is* darkness and what is in it; the
  candle cone (§12) and the lights-out pass are already faithful, and this is where the deliberate
  improvements on top would live.
- **`RS_Avatar`** — the player body, and the coat of §15.
- **`RS_WorldHands`** — the interaction layer of §14.
- **`RS_Reloading`** — Realms has only two or three guns, so this is a small job. **The staves are
  the open question**: they are not guns and will not reload like guns, so what a stave *does* in
  your hands needs deciding before anyone models one. Models for them do not exist yet.

---

## 18. The retail install we read is not the whole game

Status: `measured` 2026-10-09. Not a "better" idea — a correction, filed here because it changes
what several of the ideas above can use.

`E:\DOOMWork\_oracle` mirrors the **hard-disk install** the 1996 installer made. The installer left
the large content on the CD, so that mirror has `DBASE100`, `200` and `400` and is missing:

| | `_oracle` | retail `DATA` directory |
|---|---|---|
| `DBASE300` (music **and item close-up GDVs**) | no | yes |
| `DBASE500` (speech) | no | yes |
| `.GDV` movies | none | **164** |
| maps in `M/` | 50 | 51 |

The retail `DATA` directory is the CD root and is complete. **Point `-rothpath` at it.** Leave
`_oracle` alone: it is the oracle rig's working directory, with `rothc.exe`, its mods, savegames and
several hundred `.ppm` dumps in it, and repointing it would break the comparison rig.

This matters beyond tidiness. Speech, music, every cutscene and the item turntables of §16 are all in
the half we were not reading, and we would have built a Realms with no voices and no movies without
noticing until late.

---

## How to add to this

One heading per idea. Say what Realms does, what we do, why theirs might be better, and what it would
cost. Mark the status honestly — an idea labelled `verified` that nobody actually read is worse than
no entry. If an idea gets built or rejected, say so in place rather than deleting it; the reasoning is
the valuable part.
