# The Quake VR finger port: what went wrong (2026-11-12)

Written for every lane. The short version is at the end, under "The rule".

## 1. What Quake VR's grasp is

Quake VR (vittorioromeo/quakevr, `Quake/vr/vr_grasp.cpp`, about 1,500 lines, plus `vr_handrig.cpp` and its rig data)
closes a VR hand's fingers on whatever the hand holds. It is a port of GraspIt!'s "auto-grasp". The whole method has
these parts:

1. **Close from open.** Each finger closes along its own curl path, all joints together. When one segment touches
   the held thing, the joints that move that segment stop; the joints past it keep closing, so the finger wraps.
2. **Conservative advancement.** Each step closes only as far as no finger sphere can reach the surface yet, so a
   finger never jumps through thin parts.
3. **Close from the fist.** A finger that is already inside the thing when open is not left open: it closes from
   the other end, opening from a full fist to the tightest curl that is clear.
4. **Settle pass** after closing.
5. **Thumb turn search.** The thumb is tried at several base turns (opposition and swing) and the best one is
   picked by a score that rewards closure and a real wrap.
6. **Quake VR's own thumb motion.** Its thumb joints roll and sweep across the palm (rig data,
   `vr_handrig_data.inc`), not along a simple fold.
7. **Finger spheres fitted to the hand's own skin** (`make_hand_rig.py`), none on the thumb's base bone for weapons.
8. The index finger is solved too, the palm may move to sit flush, the controller's grip/trigger set how far each
   finger closes, and the solve starts warm from last frame.

## 2. What REMA took, and why only part

On 2026-11-10 (commits `4279e39`, `3f5e811`) this lane wrote `[GRASP]`, described in its own header as "a port of
Quake VR's auto-grasp". It contained **only part 1** (close from open, stop on contact), in fixed steps, with REMA's
FRIK fold table (`handcurl.txt`) as the curl path and a point-cloud contact test.

Why only part 1:
- The port was written from a **summary**, not from the source. The research note
  `docs/plans/research/vr_repos_report.md` section 1.9 describes the grasp in three lines, taken from the header
  comment of `vr_grasp.hpp`. The 1,500 lines of `vr_grasp.cpp` were never read for that port. Parts 2 to 8 are not
  in the header comment, so they were not in the port.
- The code was then **labelled as the whole thing** ("Quake VR's auto-grasp, ported"), so every later session
  believed the rule was Quake VR's and treated its failures as REMA problems to invent around.
- Nobody wrote a checklist of the source's parts before porting, so nothing showed what was missing.

## 3. What that cost: the work done on top of a partial port

Each of these was a workaround for a part that was missing, or work spent finding that out:

| When | What | What it was really missing |
|---|---|---|
| 11-12 00:37, `356293a` [GRASPPOSE], test 1112b | The thumb never closed (it starts inside the gun when open, and part 1 leaves it open). "Fixed" by starting every finger from the weapon mod's pose instead of open. The owner: fingers "worse ever since". | Part 3, close from the fist |
| 11-12 early | Finger length and thickness studies, previews at 100/115/125/135 %, `FINGERTHIN` | Partly real (the hand is thick), partly the missing thumb motion |
| 11-12 04:10 | An invented thumb rule (`rule_thumb`: search first-bone turns, fold until touching). The owner: "awful", "goes through the shotgun barrels", "a downgrade". | Parts 5 and 6 (Quake VR already has a thumb turn search and its own thumb motion) |
| 11-12 | A 40-minute agent study of the shotgun thumb; verdict "leave it until the mesh is replaced" | Parts 5 and 6 |
| 11-12 17:00 | Agent 1 restores the 11-10 rule and, reading Quake VR's source for the first time, finds parts 2 to 8 were never ported | the discovery |
| 11-12 17:20 | Agent 2 ports parts 1-5 in the preview: four fingers now wrap on all six test guns; the thumb still hangs | Part 6 |
| 11-12 17:45 | Agent 3 ports part 6: the thumb wraps on all six guns. Root cause found: FRIK's thumb fold puts the curled tip more than a palm width out in front of the palm; Quake VR's lies across it | |
| now | Agent 4 puts parts 1-8 in the engine, against a written checklist of every part (`QUAKEVR_GRASP_SPEC.md`) | |

Engine builds spent on the partial rule or its workarounds: 1110f, 1110g, 1112b (and every build since carried
GRASPPOSE). One owner headset round judged the partial rule as "Quake VR".

## 4. The rule (for every lane)

1. **Port from the source, never from a summary, a header comment or a research note.** Read the whole
   implementation before writing a line.
2. **Before porting, write a numbered checklist of every part, constant and behaviour of the source, each with its
   file:line.** For each, the port's file:line, or "not ported" with the reason. That list is the acceptance test.
3. **Never label partial work with the source's name.** Write "the contact-stop core of Quake VR's grasp, parts 2-8
   not ported", so the next session knows.
4. **When a ported system misbehaves, check the port against the source before inventing a fix.** Every workaround
   above was for something the source already solved.
