# Billboards

A billboard is a quad placed in the world with its own orientation, drawn as
real depth-tested geometry. It is occluded by walls, sorted against sprites,
and can be shot at, touched, or pointed at.

It exists so an in-world interface can be built natively: hundreds of panels
without an actor apiece, and a pointer that can ask what it is aimed at.

Nothing here is a HUD overlay. A billboard is in the world even when it is
welded to your view.

## The shape of one

| Field | Meaning |
| --- | --- |
| `pos` | world position — or an offset from the viewer, if view-locked |
| `width`, `height` | **full** extent in map units, not half |
| `yaw` | which way the face points |
| `tilt` | 0 is vertical; positive leans the top toward the viewer |
| `facing` | how yaw is decided — see below |
| `payload` | what it draws |
| `data` | payload-specific packed int |
| `color` | tint |
| `alpha` | 0 invisible, 1 opaque |
| `flags` | see below |
| `lifetime` | seconds; ignored when persistent or attached |

Extent is **full, not half**. A billboard 48 wide measures 48 edge to edge.
This is written down because the previous engine's single `size` was ambiguous,
and for a hinge it is the difference between wings that meet a centre panel
exactly and wings that overlap it or leave a seam.

## Facing is a mode, not the definition

```
0  BBF_FIXED       use my own yaw and tilt verbatim
1  BBF_CAMERAYAW   turn to the viewer, keep my tilt
2  BBF_CAMERA      turn to the viewer including tilt
```

A quad that *always* turns to the camera cannot be hinged to another at a fixed
angle: once both turn independently the angle between them stops meaning
anything, and a hinged assembly collapses into parallel planes. So a triptych
is one `CAMERAYAW` root with `FIXED` children hinged off it.

**Hinge solving stays in script.** The engine consumes the yaw and tilt it is
handed and never computes a hinge itself.

## Flags

```
1   persistent    lives until removed; lifetime ignored
2   attached      rides an actor, dies with it
4   no depth      draws over world geometry instead of being occluded
8   view-locked   pos becomes an offset from the viewer: X ahead, Y right, Z up
16  follow angle  attached only: yaw is measured from the actor's facing
32  no hit        drawn, but invisible to aim, touch and sweep
64  void          BB_SEAM only: the opening is a hole, not a lit panel
128 hand (main)   pos becomes an offset in the WEAPON hand's frame
256 hand (off)    the same, in the off hand's frame
```

`4` is what a HUD-locked panel needs — welded to the view, it would otherwise
be sliced in half whenever you back into a wall.

`8` has to be native. Script runs at tic rate and the view does not, so a
head-locked panel repositioned from script lags and snaps against head
movement. Resolved in the renderer instead, it stays welded.

`16` is the difference between an object that *has* a front and one that merely
travels with an actor. Four billboards at yaw 0/90/180/270 on one actor make a
box; without this flag the box shears into a plane as the actor turns. A floor
marker under a monster wants the flag off.

`32` exists because a panel is usually not one quad. Compose a card out of
payloads and it is forty billboards — every glyph of every label, a bar's track
and its fill — and the queries return the *nearest* hit, so the panel's own
face is permanently masked by the text written on it. A pointer aimed at a row
comes back holding the handle of a letter, which no caller can map to anything.
Flag the decoration and the one quad that means something is the one that
answers. Anything a player can point at, touch or shoot leaves it off.

`128` and `256` are view-lock one joint further out: the offset is measured in
the hand's own frame -- X along the aim, Y across it, Z up from it -- and the
hand's angles bias the billboard's. A readout on the gun. They have to be
native for view-lock's reason and more so: a hand swings several times faster
than a head, so a panel placed from script at 35Hz swims on every movement.
Set one or the other, never both.

## Payloads

```
0  BB_PANEL     rounded-rect backing
1  BB_TEXTURE   any TextureID on the quad; data = TextureID.GetIndex()
2  BB_DIGITS    a row of digits; data = value | palette
3  BB_GLYPH     a single glyph; data = id | palette
4  BB_RING      progress ring
5  BB_BAR       progress bar
6  BB_TEXT      an arbitrary string; reads the billboard's own text, ignores data
7  BB_SEGMENT   that string as a 16-segment display, drawn procedurally -- no atlas
8  BB_SEGLCD    BB_SEGMENT inverted: a lit plate with the digits punched out
9  BB_SEAM      a glowing slit; ResizeBillboard opens it; void flag = a bright-rimmed hole
10 BB_WG13      GITD's kill badge -- plate and digits transcribed in one pass
11 BB_SDFPANEL  BB_PANEL solved as a distance field instead of a sampled texture
```

**All twelve draw.** The first six are not shaders and never needed to be: a
payload emits as many textured quads as its shape wants -- a bar is a track
and a fill, a number is a row of glyphs -- and the quad-building work was
already done. Offsets are in half-extents of the parent, so 1.0 is its edge
and a payload never needs to know where in the world it sits. Every sub-quad
takes depth from the billboard's centre rather than its own, so one panel
sorts as one object and a fill cannot land behind its own track.

`BB_SEGMENT`/`BB_SEGLCD`/`BB_SEAM` and `BB_WG13` are procedural rather than
atlas-driven, and `BB_SDFPANEL` is `BB_PANEL`'s rounded rect solved per pixel
instead of sampled — the reason it exists is that only a field can take
`SetBillboardGlow`, since a halo needs to read past an edge a sampled plate
doesn't have. `FORK_CHANGES.md` §1 and §28 cover the reasoning behind each;
this file lists what they are.

The texture route is not a stopgap: point `data` at a canvas texture and paint
it from ZScript, and the billboard is whatever you painted.

## Script API

```
void AddBillboard          (Vector3 pos, double w, double h, double yaw, double tilt,
                            int facing, int payload, int data, color col,
                            int flags = 0, double lifetime = 0)
int  AddBillboardPersistent (... same ...)            returns a handle
int  AttachBillboard        (Actor mo, Vector3 offset, double w, double h,
                            double yaw, double tilt, int facing, int payload,
                            int data, color col, int flags = 0)   returns a handle

void MoveBillboard      (int id, Vector3 pos)
void OrientBillboard    (int id, double yaw, double tilt, int facing)
void ResizeBillboard    (int id, double w, double h)
void UpdateBillboard    (int id, int data, color col)
void SetBillboardAlpha  (int id, double alpha)
void RemoveBillboard    (int id)

int, Vector2         AimBillboard   (Vector3 start, Vector3 dir, double maxDist = 0)
int, Vector2, double TouchBillboard (Vector3 point, double maxRange = 0)
int, Vector2, double SweepBillboard (Vector3 from, Vector3 to, double radius = 0)
```

`AddBillboard` issues no handle. It is the cheapest form — use it for anything
you will never address again.

## Groups

A panel is not a quad. It is forty of them — a shell, a face, every rule, every
glyph of every label — and a group is how you treat that as one object.

```
int  AddBillboardGroup       (Vector3 origin)          returns a handle
void SetBillboardGroup       (int id, int gid)         gid 0 takes it out of one
void SetBillboardGroupScale  (int gid, double scale)   snap; also cancels an animation
void AnimateBillboardGroup   (int gid, double from, double to, int tics)
void SetBillboardGroupOrigin (int gid, Vector3 origin)
void RemoveBillboardGroup    (int gid)
```

Three calls and then nothing:

```
int gid = level.AddBillboardGroup((AHEAD, 0, UP));   // the pivot
... build the panel, level.SetBillboardGroup(id, gid) on each element
level.AnimateBillboardGroup(gid, 0.0, 1.0, 10);      // grow, once
```

**Why this is not script's job.** `ResizeBillboard` and `MoveBillboard` both
exist, so script *could* do it — but scaling a composed panel means scaling each
member's size *and* its offset from the pivot, which is eighty setter calls per
step, each an O(n) scan of the billboard array. Worse, it would **step**: script
runs at 35Hz and the renderer does not. A UI element scaling in twelve visible
jumps a foot from someone's face is worse than not animating it at all. A group
resolves in `DispatchBillboards` from a start tic and a duration, so the motion
is frame-rate smooth and script writes one number and walks away — the same
argument `BBFL_VIEWLOCKED` makes for position.

**The origin is in the members' own space** — an offset from the viewer for
`BBFL_VIEWLOCKED`, from the actor for `BBFL_ATTACHED`, a world point otherwise.
A group whose members do not all share a space draws as nonsense; there is no
cheap way to detect that and no attempt is made.

**The curve is the engine's, not a parameter.** Growth eases out with a slight
overshoot and settles — a panel that arrives at exactly its final size and stops
reads as a texture being swapped in, where a few percent past and back reads as
an object arriving. A collapse eases *in*, accelerating away, because a thing
leaving should not linger and certainly should not bounce.

Scale 0 draws nothing, so a group is also how you hide a panel without
destroying it and re-issuing every handle. `RemoveBillboardGroup` releases its
members rather than orphaning them: an unknown gid resolves to scale 1.0, so a
member left pointing at a dead group would silently **snap to full size** rather
than disappear.

Groups serialize with the level, and an animation saved mid-flight resumes where
it was rather than restarting.

## View-locked orientation

`BBFL_VIEWLOCKED` resolves **both** position and orientation against the
viewpoint. The orientation half was missing until `fdceb0abcf` (see the history
note below), and its absence is worth describing, because the symptoms point
nowhere near the cause.

Position was view-relative while yaw stayed in world space, so a head-locked
panel followed the viewer around the room while permanently facing world-east.
You could walk around your own HUD. Off-axis it foreshortened until it was a
third of its authored width; from behind you were reading its back, which
renders as **every glyph mirrored** — and that sends you hunting through
`bb_flipu` and the `right`-vector derivation, neither of which is wrong.

It is a **yaw bias**, not a camera-facing mode, and the difference matters.
`BBF_CAMERAYAW` makes each quad yaw about its own position, which bows a
composed panel into a cylinder and collapses hinged assemblies. Adding the view
yaw to each billboard's *stored* yaw keeps every element's angle relative to
every other, so a flat panel stays flat and the assembly turns with the head as
one rigid object.

The bias is `view yaw + 180`, because yaw is *which way the face points* and a
view-locked panel is parked ahead of the eye along the view vector — biasing by
the view yaw alone aims its face the same way the viewer is looking, i.e.
straight away from them. Applied in the renderer and in all three of
`AimBillboard` / `TouchBillboard` / `SweepBillboard`, so the clickable region
cannot drift away from the picture.

## Hand-locked position and orientation

`BBFL_HANDMAIN` / `BBFL_HANDOFF` are the same idea as view-lock, applied to a
hand, and they resolve **both** halves for the same reason it does.

The frame comes from the pawn's **published hand pose** -- `AttackPos`,
`AttackAngle`, `AttackPitch` and `MainHandRoll`, or the `Offhand*` set -- which
the VR backend rewrites every rendered frame from `VRMode::GetWeaponTransform`.
It is deliberately not re-derived: those fields exist for exactly this, they
are already world-space map units, and a replaying VR demo restores them, so a
demo shows the readout where it was. The note on `MDL_FOLLOWMAINHAND` in
`models.cpp` records what re-deriving that basis by hand cost the last two
times someone tried.

Roll comes from `MainHandRoll` and **not** from `AttackRoll`: the playsim zeroes
`AttackRoll` every tic to keep peers deterministic, which is the whole reason
`MainHandRoll` exists.

The yaw bias is `hand yaw + 180`, view-lock's `+180` for view-lock's reason --
yaw is which way the face points, and a card sitting on the gun is read from
behind the gun. Biased by the aim alone it faces down-range and shows the
player its back.

Pitch and roll are added **only for `BBF_FIXED`**. A card that asked to face the
camera has already said it wants to be readable rather than welded, and canting
it with the wrist afterwards would undo the request. So `BBF_FIXED` is a decal
on the weapon and `BBF_CAMERAYAW` is a tag that rides it and keeps facing you.

The pitch bias is **minus** the hand's pitch. Tilt is measured on the face and
the face looks back down the aim, so a gun raised 30 degrees points its card 30
degrees down at the player; adding it instead tips the card the wrong way and
by double the error. The same relation falls out of `BillboardHandAxes`, whose
`levelUp` equals `BillboardBasis`'s `up` only at `tilt = -pitch` -- and the two
**must** agree, or the card's face stops matching the plane its offset was
measured in.

Hand-locked billboards are never distance-culled, for view-lock's reason: they
are welded to the gun in front of the eye, so distance to them is meaningless
and losing one to a budget reads as the UI vanishing.

## Pointing at one

Both queries answer the same question: **which billboard, and where on its
face**, as a 0..1 UV — the same UV the shader sees. Both return 0 on a miss,
and neither can hit a transient, since a transient has no handle to report.

```
int hit; Vector2 uv;
[hit, uv] = level.AimBillboard(eyePos, aimDir);

int t; Vector2 tuv; double dist;
[t, tuv, dist] = level.TouchBillboard(handPos, 8);
```

Because a trigger, a tracked fingertip and the Use key all resolve to the same
answer, a panel handles a press without knowing which one it came from. Use is
the fallback when there is no tracking, and it costs nothing extra.

`TouchBillboard` also returns distance to the surface, which is what makes
touch feel like touch: drive a hover highlight as the hand approaches, fire the
press on contact. Two things worth handling in script — a hover band, or
buttons light up whenever a hand drifts near; and debouncing, or a hand resting
on a panel fires every tic.

`SweepBillboard` is the same test taken along a path — where the hand was, to
where it is — and it exists because script only gets to ask 35 times a second.
A panel's touch slab is a few map units thick and a deliberate jab moves a
controller several units per tic, so the hand can be in front of the panel on
one tic and behind it on the next without ever being inside it: the gentle
touch works and the hard one does nothing. Sweeping the path closes that, and
hands back "the hand arrived this tic" instead of making the caller infer an
edge from two samples. `radius` inflates the face into a slab and pads its
edges, which is what a fingertip is; the returned fraction is where along the
segment contact happened.

Debounce, cooldown, hysteresis and which hand acted are deliberately *not* in
the engine. How many presses a held hand is worth is a design question, not a
geometric one, and it belongs to whoever owns the panel.

## Lifetime

Three kinds, checked once per game tic in `FLevelLocals::TickBillboards`:

- **transient** — expires after `lifetime` seconds
- **persistent** — lives until `RemoveBillboard`
- **attached** — follows an actor, dies with it, ignores lifetime

Attachment holds a `TObjPtr`, which does not dangle across a GC sweep and does
not itself keep the actor alive. "Dies with its actor" therefore means exactly
that, and never the reverse: once the pointer resolves null the billboard is
dropped, and the actor is never revived to keep it company.

Attached billboards read their actor's **interpolated** position when drawn, so
they track at render rate rather than stepping at 35Hz.

## Save and load

All billboards travel, transients included. `maptime` is already saved, so a
transient's `spawntic` stays meaningful and it resumes its remaining lifetime
rather than restarting it. `attachedTo` rides the object table, so a pointer
whose actor did not survive loads as null and the next tic drops that billboard
exactly as it would have live.

`drawPos` is deliberately not saved — the first frame after a load rewrites it.

Groups travel with their members. Dropping them would not error — an unknown gid
resolves to scale 1.0 — so every grouped billboard would silently come back full
size at its unscaled offset, which on a panel saved mid-animation is a pile of
quads in the wrong places.

## Cvars

| Cvar | Default | What |
| --- | --- | --- |
| `bb_scale` | 1.0 | scales every billboard as drawn |
| `bb_tiltbias` | 0 | degrees added to every tilt |
| `bb_flipu` | false | flips which way a face runs |
| `rs_bb_maxpanels` | 0 | most drawn at once; 0 unlimited |
| `rs_bb_cullradius` | 0 | distance limit; 0 unlimited |

All are live — next frame, no reload.

Size and tilt are sliders because neither has a correct value: comfortable card
size and the tilt that makes a panel look upright depend on the person and the
headset.

`bb_flipu` is different in kind. It is not a preference but the settlement of a
handedness question, and the bug it fixes stays invisible until something with
text on it renders backwards. This project has been caught by exactly that
before. With the basis corrected on 2026-08-08 its default — off — is the right
value, and it should stay there.

```
face  F = ( cos y,  sin y, 0)
right R = (-sin y,  cos y, 0)
up    U tilts toward -F, so positive tilt leans the top toward the viewer
```

**This block said `R = (sin y, -cos y, 0)` until 2026-08-08, and that is the
viewer's LEFT.** So did the code, in three separate copies, which is why every
billboard drew mirrored and `BB_DIGITS` laid `120` out as `021`. The derivation
is in `FORK_CHANGES.md` and in the comment at `hw_sprites.cpp`.

The renderer, both queries and the sweep have to agree, or the pointer lands
somewhere other than where the panel is — so they no longer each carry a copy.
All four call `BillboardBasis` in `g_levellocals.h`, which also applies
`bb_scale` and `bb_tiltbias`. Those two were missing from the queries: raising
`bb_scale` to make a panel readable used to grow the picture and leave the new
edges dead.

Under a budget the nearest billboards win — the far ones are unreadable anyway,
so they are both the cheapest to drop and the least missed. View-locked
billboards are never culled: welded to the eye, their distance is meaningless,
and losing one to a budget would read as the interface vanishing.

## A note on where the group system landed in the history

**Groups and the `BBFL_VIEWLOCKED` orientation fix are inside commit
`fdceb0abcf`, whose message says only "A uniform block is matched by offset,
not by name".** That message is true about part of the commit and says nothing
about the rest of it.

Two lanes were working in this repo at once, and a `git add -A` from the
lighting lane staged the billboard lane's uncommitted work along with its own.
The commit therefore contains a Vulkan uniform-offset fix *and* a new engine
feature *and* a VR-comfort bug fix, under a message describing the first only.

It is recorded here rather than corrected in place, because the commit is
pushed and other lanes may have pulled it; rewriting shared history to tidy a
message costs more than the message is worth. Anyone bisecting billboard
behaviour should know it is that commit and not a later one.

**The process lesson, which is the useful part:** in a repo another lane is
editing, stage explicitly. `git add -A` is a claim that everything in the tree
is yours, and in a shared checkout that claim is false.

## Where it lives

| File | What |
| --- | --- |
| `src/g_levellocals.h` | `FBillboard`, `FBillboardGroup`, `BillboardBasis`, `BillboardGroupScale`, the enums, storage on the level |
| `src/p_tick.cpp` | `TickBillboards` — attachment, expiry; `bb_clear` |
| `src/scripting/vmthunks.cpp` | every native, including aim/touch and the group setters |
| `wadsrc/static/zscript/doombase.zs` | the ZScript declarations |
| `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` | `DispatchBillboards`, culling, the group transform |
| `src/rendering/hwrenderer/scene/hw_sprites.cpp` | `ProcessBillboard`, the quad |
| `src/p_saveg.cpp` | serialization, billboards and groups |

Two things worth knowing before you change any of it. The orientation solver is
`BillboardBasis` and there is **one** copy on purpose — the renderer and all
three queries call it with the same cvars, because when it was three hand-copied
pairs of lines all three were wrong at once and a pointer that lands somewhere
other than where the panel draws is invisible until someone notices. And there
are **no getters**: script cannot read a billboard's size, position or alpha
back, so a caller must mirror anything it sets, and `flags` and `payload` cannot
be changed after creation at all.

## Not done

**SDF payloads -- done, and then some.** `BB_TEXT` reads a real signed-distance
atlas built offline from any TTF (`tools/sdffont/mksdf.ps1`), so it stays sharp
at any magnification and its glow falls out of the field rather than out of a
blur pass. `BB_SEGMENT` and `BB_SEGLCD` build a sixteen-segment display from
arithmetic with no atlas at all, and `BB_SEAM` is a procedural slit. All four
are genuine distance fields; all four share one halo.

Still true from the original note: building whole OBJECTS out of shapes rather
than out of geometry is a different feature and nothing is blocked on it.

What remains not-SDF is `BB_PANEL`, `BB_RING`, `BB_BAR`, `BB_DIGITS` and
`BB_GLYPH` -- the first three sample small textures, the last two are the
bitmap-font path `BB_TEXT` supersedes. They work; they are just not fields.

**Collision.** Billboards are render-only. You fall through one. Doom's
collision model has no place for an arbitrary oriented quad, so this is a real
feature rather than a flag. Until then: 3D floors for anything walkable, or an
invisible solid actor with a billboard attached for looks — bearing in mind
actor collision is a cylinder, so a thin plank gets a chunky invisible box.
