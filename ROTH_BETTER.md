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
- **Vertically it does not move at all.** Fixed start row, wrapped on the block height.
  Look up or down and the sky stays exactly where it was.

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

**Status:** the divergence is live (GZDoom sky in place of the band). Fog, weather and a
true skybox are ideas, not built.

---

## How to add to this

One heading per idea. Say what Realms does, what we do, why theirs might be better, and what it would
cost. Mark the status honestly — an idea labelled `verified` that nobody actually read is worse than
no entry. If an idea gets built or rejected, say so in place rather than deleting it; the reasoning is
the valuable part.
