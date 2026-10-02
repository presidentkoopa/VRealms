# The use key problem, and what it is really about

**2026-10-02. Written for the co-designer.** The short version: doors not opening
was not a door bug. Realms' whole trigger layer is unreachable from play, for
three independent reasons, and the third is that our own authoritative spec says
the implementation is wired to the wrong events — in a section that names the
file and the lines it corrects.

> ## RESOLVED THE SAME EVENING, AND BY NONE OF THE THREE
>
> **The use key opens doors as of commit "A door leaf ate the use ray".** The
> cause was a fourth thing this page does not mention: the door panel itself is
> one-sided blocking geometry with no line special, so it swallowed the use ray
> and nothing behind it could be reached either. §2.4 below.
>
> **Line 1669 is the answer to §4's own open question.** This page records that
> the ray "DOES reach a line — 1669, which carries no Realms chain" and treats
> that as a puzzle. 1669 is not a trigger line at all: it is door leaf poly
> tag 1. The ray was meeting the door.
>
> **Two of the three reasons are overstated, and acting on §2.2 as written would
> have introduced a bug.** Both are corrected in place below. The parts that
> hold up: §2.1 was real and is fixed, §3.1 and §3.3 are the lessons worth
> keeping, and §3.5 was right that this blocked the game layer.

---

## 1. What was observed

Two statements, both true at the same time, for about a day:

- **"Doors open. Thirty of thirty, nothing refused."** Recorded in the tree on
  2026-10-01, from the `roth_door` console command.
- **"Cannot open doors so I cannot see more of the map."** The owner, in the
  headset, 2026-10-02.

Neither was wrong. They exercised different code. `roth_door` calls the
chain-firing code directly and never goes near the engine's input or dispatch.

## 2. Three separate breaks, found in this order

### 2.1 The engine never offered us the line

Realms' level logic is a list of TRIGGER records, each bound to geometry and
waiting for an event. STUDY1 binds 31 face-keyed and 12 sector-keyed triggers,
and the interpreter executes 62.7% of the opcodes the retail maps use.

The port hooks this into GZDoom at the top of `P_ActivateLine`, with a comment:

> Realms of the Haunting's triggers are not Doom line specials, so they must be
> tested BEFORE the engine decides this line is inert.

Right idea, wrong layer. `P_ActivateLine` is the engine's **handler**. It is not
how the engine **decides** to activate a line. That decision is made at a dozen
dispatch sites, and every one asks the same question first:

```c
// P_UseTraverse, the use key                              p_map.cpp:6554
if (in->d.line->special == 0 || !(in->d.line->activation & (SPAC_Use | ...)))
        goto blocked;          // never calls P_ActivateLine

// crossing a line                                         p_map.cpp:2792
if (side != oldside && ld->special && !(thing->flags6 & MF6_NOTRIGGER))
        P_ActivateLine(...);   // only if ld->special is non-zero
```

A Realms line has no Doom special, so it was never offered and the hook never
ran — for use, walk-over, shoot and bump alike.

**Fixed.** Lines carrying a Realms chain are given a non-zero `special` and the
activation bits at load, so the engine offers them through its own paths and the
existing hook claims them. 74 lines marked in STUDY1, confirmed surviving to
play time. The special's value is never executed: `ActivateLine` returns true for
any line it owns, so `P_TestActivateLine` never reads it.

### 2.2 Sector triggers have no dispatch at all

`FireSectorTriggers(sector_t*, AActor*)` is defined in `roth_runtime.cpp` and
declared in `roth_runtime.h`. **It has zero callers anywhere in the tree.**

Nothing in the playsim calls it, so all 12 sector-keyed triggers in STUDY1 have
never fired, in any build, ever. The handler was written and the dispatch was
never wired.

> **CORRECTED 2026-10-02 evening. The fact is right; the conclusion drawn from
> it was wrong, and the next step it implied would have introduced a bug.**
>
> This page went on to recommend "wire a caller for `FireSectorTriggers`", and
> the session handoff repeated it as an exact next step. **Do not.** Wiring it
> to a sector-entry event would fire these triggers on walking through a
> doorway, which the original does not do.
>
> `0x19` and `0x31` are not enter-sector triggers. `GAME_core.md` §5.2 rows 6
> and 12 give them as **left-click floor / platform top** and **right-click
> floor / ceiling / platform** — clicks on a FLOOR, matched by the sector's
> command id. They are sector-KEYED, which is what the marker byte says, and
> that is a different thing from being sector-TRIGGERED.
>
> `FireSectorTriggers` having no caller is therefore **deliberate**, and the
> comment on it in `roth_runtime.h` said so, in detail, with the trace —
> agreeing with the spec. This page read "zero callers" as "unfinished work"
> without reading what the function's own header said about why.
>
> What is genuinely missing is a **floor-click dispatch**: a use or examine ray
> that lands on a flat rather than a wall. That is the caller those marks want.
> The header comment now carries the §5.2 citation so the code itself pushes
> back on the next person who tries to fix this by sector entry.
>
> **The lesson is the mirror of §3.4.** That section's complaint is that a spec
> correction sat unapplied while the code carried a confident comment arguing
> the opposite. Here the code comment was the one that matched the spec, and
> this page was the confident document arguing the opposite. "A handler with no
> caller" and "a handler whose absent caller is the documented design" look
> identical from the outside too.

### 2.3 The trigger classification is wired to the wrong events

This is the one that matters most, and it was already written down.

`docs/GAME_core.md` §5.2 gives the 15 trigger categories, each with the code that
fires it, and states that it **"corrects R/ROTH_COMMANDS.md and
R/src/roth/roth_runtime.cpp:54-55"**. The correction has never been applied:

| opcode | `roth_runtime.cpp` treats it as | `GAME_core.md` §5.2 says it is |
|---|---|---|
| `0x18` | face trigger | left-click wall face ✔ |
| `0x19` | **sector** trigger | left-click **floor / platform top** |
| `0x31` | **sector** trigger | right-click **floor / ceiling / platform** |
| `0x1a` | face trigger (a "BUMP") | **attack or projectile hits** a wall face — "the player's POINT sweep never fires it" |
| `0x32` | face trigger | **right-click (examine)** wall face |
| `0x13` | **not classified at all** | **enter / leave sector** |

So:

- The real enter-sector trigger is `0x13`, and the loader never binds it. Those
  triggers are attached to nothing.
- The two opcodes bound "by sector" are floor **clicks**, pointed at the wrong
  kind of geometry.
- Left-click (use) and right-click (examine) are conflated into one bucket, so
  examining a thing and using it are the same event.
- `0x1a` fires on activation, when the original only fires it on a weapon hit.

The runtime carries a long comment reaching the opposite conclusion — it calls
`0x1a` "a BUMP" and dismisses `0x13` as "the water/lava machine ... not one of
these". Both readings trace the same original functions. `GAME_core.md` is the
authoritative spec for this port and names the lines it corrects; the observed
behaviour (no door opens, no sector trigger has ever fired) is consistent with
the spec being right and the comment wrong.

**Not fixed.** It is a data-classification change plus a new binding for `0x13`,
and it should be done against §5.2 directly rather than by patching outward from
the symptom.

> **NARROWED 2026-10-02 evening. Three of the six rows above are real; the
> table overstates the rest, and none of it was why the use key failed.**
>
> The table is built from a comparison this page did not make: it sets "what
> `roth_runtime.cpp` treats it as" against §5.2's EVENT column, but the code's
> classifiers sort triggers by **which geometry carries the key**, not by which
> event fires them. On key space the code and the spec already agree, and
> §5.2's own "Load marker" column is the proof: `0x18`/`0x1a`/`0x32` mark the
> texmap (face-keyed), `0x19`/`0x31` mark the sector's `+0x16` (sector-keyed).
> That is exactly what `IsFaceTrigger` and `IsSectorTrigger` say, and the long
> comment above them derives it from the same markers. So the `0x19` and `0x31`
> rows are not a disagreement, and "the two opcodes bound by sector are pointed
> at the wrong kind of geometry" is not true — they are keyed by the right
> geometry and merely have no dispatch, which is §2.2.
>
> **What survives, and is genuinely wrong:**
>
> - **`0x13` is not classified or bound at all.** The real enter/leave-sector
>   trigger is attached to nothing. This is the biggest of the three and needs a
>   sector-transition event. The runtime's comment dismisses `0x13` as "the
>   water/lava machine ... not one of these", which §5.2 row 4 contradicts.
> - **`0x1a` fires on a use.** §5.2 row 7 makes it attack-or-projectile-hits and
>   says "the player's POINT sweep never fires it", so it should not be in the
>   use bucket.
> - **`0x32` is conflated with `0x18`.** Examine and use are one event, so
>   examining a thing and using it are indistinguishable.
>
> **Partly addressed already.** `SPAC_Cross`, `SPAC_AnyCross` and `SPAC_Push` are
> no longer asked for on trigger lines: all three face-keyed opcodes are a click
> or a hit in §5.2, so none of them is a walk-over or a shove, and face triggers
> had been firing on brushing past their wall. `P_ActivateLine` now passes
> `activationType` through, so the per-event split has somewhere to hook. What
> remains is one bucket answering either a use or a hit, which still over-fires
> `0x1a` on use — recorded in the code at `ROTH_LINE_ACTIVATION`, not hidden.
>
> **And it was not the door bug.** This page calls §2.3 "the one that matters
> most" and §4 calls it "the likely reason" a door will not open. It was not:
> doors open by `toggle_door_open_state` on the panel, which is not a trigger at
> all (§2.4). The classification work is still worth doing for dialogue, items
> and scripted events — §3.5 is right about that — but it was never between the
> owner and a door.

### 2.4 The door panel ate the use ray — FOUND AND FIXED, and this was the bug

Added 2026-10-02 evening. None of §2.1-§2.3 stood between the owner and a door.
This did, and it is not in the trigger layer at all.

A Realms door is opened in the original by clicking **the door**, not a wall
beside it: `GAME_core.md` §4.5 sends a type-6 door straight to
`toggle_door_open_state` (`E/input.c:1087-1098`), with no trigger, no chain and
no command record. This port only ever implemented the other route — an `0x18`
wall-face trigger whose chain happens to contain `cmd_open_door` — and left the
panel as bare geometry.

Bare, and worse than inert:

1. A leaf's four lines are generated by the loader (`rothmap.cpp`, the leaf
   loop), so they carry no Realms face id, and `RegisterFaceSide` —  called only
   from the wall loop at `rothmap.cpp:848` — never saw them.
2. With no face they were never in `bySide`, so the marking pass never gave them
   a special or any activation bits.
3. `PO_Init` then zeroed the `Polyobj_ExplicitLine` special they were built with
   (`polyobjects.cpp:431-433`).
4. A leaf is one-sided and `ML_BLOCKING`. In `P_UseTraverse`, `special == 0`
   goes to the `blocked` path, `P_LineOpening` on a line with no backsector
   gives `range = 0`, and the traverse gives up with "can't use through a wall"
   (`p_map.cpp:6608`).

So a closed door swallowed the use **and shadowed every trigger line behind
it**. That is the whole of §4's measurement: 74 marked lines present, 2 within
`USERANGE`, and the ray stopping at line 1669 "which carries no Realms chain".
1669 is door leaf poly tag 1. The marked lines at 40 units were behind it and
were never reached.

**The ordering trap, which is why this cannot be fixed in one line.**
`SpawnPolyobj` needs `Polyobj_ExplicitLine` to find the leaf's lines
(`polyobjects.cpp:221`), so overwriting the special before `PO_Init` stops the
door spawning at all. The leaves can only be marked *after* `PO_Init`. The
marking pass is therefore `MarkTriggerLines`, idempotent and called twice — once
from `BeginLevel` and once from the map loader after `PO_Init`.

Measured over six door poses in STUDY1 (`captures/usedoor.cfg`): marked lines
74 → 194, which is 74 trigger walls plus 30 leaves of four lines each; the use
ray reports `special 9000` where it read `0`; the event arrives as `0x2`,
`SPAC_Use`; and the swing returns `SWUNG` at all six.

**Still open on the door itself:** this OPENS. `toggle_door_open_state` also
closes on the second click, which needs the door's own state word — the existing
"closing, blocking, the second use" item.

---

## 3. What this is indicative of

### 3.1 A hook at the handler is not a hook at the decision

When a port intercepts a host engine, the interception has to sit where the host
**decides**, not where it **acts**. Everything upstream of the hook is still the
host's judgement applied to data that was never the host's.

Worth asking of every other place this port intercepts GZDoom — objects,
surfaces, lighting all hook somewhere. This one was found because a human tried
to open a door. The others have not been checked.

### 3.2 A handler with no caller looks exactly like a handler that is not firing

`FireSectorTriggers` compiles, is declared in the header, and reads as finished
work. Nothing distinguishes it from a working feature except grepping for its
callers. Both of §2.1 and §2.2 are the same mistake: **the handler was treated as
the feature.**

### 3.3 The test and the feature took different paths

`roth_door` was written to test doors and it did test doors — the door
*mechanism*, which was fine. It could not have found any of this, because it
never goes through the engine.

**A test that reaches the feature by a different route than the user does cannot
tell you the user's route works**, and will report success with total
confidence. Thirty of thirty.

The instrument built today is `rothdiff_use <x> <y> <angle512>`: it places the
camera and calls `P_UseLines`, *the same function the +use button calls*, with
`roth_useray_debug` printing every line the ray meets and `roth_trigger_debug`
printing every side offered to the Realms handler. Door camera spots come from
`doorgeom.exe <ROTH folder> -cam MAP <dist>` — pass a small distance, because
`USERANGE` is 64 units and the default is a *camera* distance.

### 3.4 An authoritative correction that nobody applied

§2.3 is the uncomfortable one. The right answer has been sitting in
`docs/GAME_core.md` since it was written, in a section that names the file and
line numbers it corrects, and the code still carries a confident comment arguing
the opposite. Nothing flagged the contradiction because nothing reads a spec and
an implementation together.

If there is one process change worth making, it is that a spec section which
says "this corrects *file:line*" should leave a mark in that file until someone
resolves it, one way or the other. A disagreement between the spec and the code
is a finding; silently keeping both is how this survived.

### 3.5 It blocks the game layer, not the visuals

Dialogue, items, scripted events, doors, lifts and lights all arrive through a
trigger firing a chain. The interpreter is built and the chains parse; they were
never being asked to run, and half of them are bound to the wrong event.

Starting the game layer on this would have produced a long run of "the opcode
must be wrong" investigations into code that was correct and unreached.

> Still true, with one correction: **doors are not on that list.** A type-6 door
> takes no trigger and no chain (§2.4), which is exactly why it could be fixed
> without touching the trigger layer at all.

### 3.7 A document is not evidence, including this one

Added 2026-10-02 evening, after §2.2 and §2.3 were corrected.

This page was written to resolve a confusion and became a source of one. It
named the wrong cause with confidence, proposed a next step that would have
introduced a bug, and was repeated verbatim into a session handoff as "THE EXACT
NEXT STEP" — where it carried more authority than when it was written, because
by then it was a citation rather than a claim.

Nothing about its form distinguished it from a reliable page. It cites file and
line numbers throughout, every individual fact in it is checkable, and its
measurement section is accurate. The error is entirely in the joins between
them.

The project rule already covers this and it was not applied: *the running
original beats ROTH.C's code, which beats its comments, which beat our
documents* — and **our documents include the ones written yesterday.** The
ordering is not about age or tone, it is about distance from the thing itself.
`HANDOFF_REMAROTH.md` §1 says to re-derive a rule that was only read before
building on it. A conclusion drawn in a document three hours ago was only read.

Practically, for the next lane: when a page here says "the likely reason", treat
that as an open question with a candidate attached, not as a finding. When it
reports a number, trust the number.

### 3.6 Two true statements can hide a contradiction

"Thirty of thirty open" and "I cannot open a door" are not contradictory if you
never notice they are about different things. The contradiction sat in the record
for a day. What resolved it was not more evidence but asking which code each
sentence referred to.

This port has many pairs of that shape: an offline tool and a runtime path, a
console command and an input binding, a measurement rig and the renderer. They
agree right up until they do not, and the disagreement is always more
interesting than either number.

---

## 4. Status

Rewritten 2026-10-02 evening. The original table is kept below it, because what
it got wrong is the useful part of this page.

| | |
|---|---|
| §2.1 engine never offered the line | **fixed**, 74 lines marked, verified present at play time |
| §2.2 `FireSectorTriggers` has no caller | **true, and correct as it stands.** Not a defect. What is missing is a floor-click dispatch; do NOT wire sector entry |
| §2.3 trigger classification | **narrowed.** Three real errors: `0x13` unbound, `0x1a` on use, `0x32` conflated with `0x18`. The key-space rows were not a disagreement. Cross and push removed from trigger lines; `activationType` now reaches the hook |
| §2.4 the door leaf ate the use ray | **fixed.** This was the bug |
| A door opening by the use key | **works.** Six of six in STUDY1, `SWUNG` at each |
| A door CLOSING on the second use | **not done** — needs the door's state word |

Measured at six door poses (`captures/usedoor.cfg`): marked lines 74 → 194,
4 within `USERANGE` where there were 2, nearest 48 units, the ray reports
`special 9000` where it read `0`, and the swing returns `SWUNG` every time.

**What the earlier measurement meant.** This page recorded "74 marked lines
present, 2 within `USERANGE`, nearest 40 units, and the use ray reaches a line
(`1669`) that carries no Realms chain", and concluded "the ray works; it is
meeting geometry whose trigger was bound to the wrong event or not bound at
all." Every number was right and the conclusion was wrong. Line 1669 has no
Realms chain because it is **not a trigger line** — it is a door leaf, and the
ray was not getting past it to the lines at 40 units. The reading assumed the
only things the ray could meet were trigger walls, so an unexplained line became
evidence for the theory already in hand rather than a question about what that
line was.

That is the one worth carrying: **the measurement was never ambiguous, only the
interpretation.** Asking "what IS line 1669?" would have ended this in minutes,
and nothing in the rig stopped anyone asking — `roth_useray_debug` had already
printed the line number. Three sections of theory were written past a fact that
was sitting in the log.
