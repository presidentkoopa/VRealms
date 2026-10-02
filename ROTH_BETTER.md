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

## How to add to this

One heading per idea. Say what Realms does, what we do, why theirs might be better, and what it would
cost. Mark the status honestly — an idea labelled `verified` that nobody actually read is worse than
no entry. If an idea gets built or rejected, say so in place rather than deleting it; the reasoning is
the valuable part.
