#!/bin/bash
# run_diff.sh -- THE HELD OBJECTS LEAVE THE ORIGINAL'S GAME STATE AS THE FLAT CLICKS DO, and follow it.
#
# What the held objects ask of the game is decided by src/roth/roth_heldlogic.cpp (the strike, the match's 20 s) and
# src/roth/roth_wicks.cpp (which flame visit is a candle's use): tools/held_test turns sessions -- a match struck,
# its flame carried to the candelabrum's wicks and the two tall candles, struck late, lingering, burnt out and struck
# again -- into the uses they ask for, and adds the Book: taken, looked at twice (the first look's OnInspect gives the
# silver key, i20). Each session is played into the ORIGINAL (ROTH.C, headless, heldprobe.c) on STUDY1 with the
# Matchbox in the right hand, and so is the reference: the flat clicks. Past every chain (the tall candles' count,
# the niche's three light changes, the shot) every byte of the command table, the geometry, the objects, the flag
# bitmap and the chain globals that changed since the snapshot must equal the reference's.
# Then held_test follow runs the niche's light follower and the Book's palm over the ORIGINAL's own frame-by-frame
# light byte (s58) and inventory counts, in every run.
# Needs the oracle build (/home/claude/oracle_build) and the retail DATA. Writes only under $W (default /tmp/heldw).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=${W:-/tmp/heldw}; export W
mkdir -p "$W/res"
sh "$HERE/build.sh" "$W/held_test" || exit 2
"$W/held_test" scripts "$W/res" || exit 2
# The state after the "D end" line (candles_test's comparison, without the frame-stamped watch lines).
state() { sed -n '/^D end/,$p' "$1" > "$2"; }
oracle() { "$HERE/run_oracle.sh" STUDY1 "$1" "$2" && state "$2" "$3"; }
# The oracle is not always the same run to run (candles_test/run_diff.sh, 2026-11-13): the reference is taken twice
# and must agree, and a scenario that differs is run again, up to three runs; every rerun is printed.
for r in 1 2 3; do
  oracle "$W/res/ref.script" "$W/res/ref.out" "$W/res/ref.f" && oracle "$W/res/ref.script" "$W/res/ref2.out" "$W/res/ref2.f" \
    && cmp -s "$W/res/ref.f" "$W/res/ref2.f" && break
  echo "reference: the two runs disagree or one failed (attempt $r)"; [ $r = 3 ] && exit 3
done
echo "reference (flat clicks): $(grep -c '^R click' "$W/res/ref.out") click(s), $(grep -c '^R inspect' "$W/res/ref.out") look(s),"\
  "$(grep -c '^[CGOF] ' "$W/res/ref.f") changed bytes, two runs identical"
bad=0
"$W/held_test" follow "$W/res/ref.out" || bad=1
# The port's 0x1d ramp (roth_lightramp.h, the runtime's lines) against the original's own s58 bytes.
"$W/held_test" ramp "${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}" "$W/res/ref.out" || bad=1
for s in "$W"/res/*.script; do
  n=$(basename "$s" .script); [ "$n" = ref ] && continue
  case "$n" in follow_*)
    # The palm's own case: followed, not compared (it gives the key through the original's give_item).
    "$HERE/run_oracle.sh" STUDY1 "$s" "$W/res/$n.out" && "$W/held_test" follow "$W/res/$n.out" || bad=1
    continue;;
  esac
  ok=0
  for r in 1 2 3; do
    if oracle "$s" "$W/res/$n.out" "$W/res/$n.f" && cmp -s "$W/res/ref.f" "$W/res/$n.f"; then ok=1; break; fi
    echo "$n: run $r $( [ -s "$W/res/$n.f" ] && echo "differs in $(diff "$W/res/ref.f" "$W/res/$n.f" | grep -c '^[<>]') lines" || echo "failed"), running again"
  done
  if [ $ok = 1 ]; then echo "$n: IDENTICAL to the flat clicks ($(grep -c '^R click' "$W/res/$n.out") click(s), $(wc -l < "$W/res/$n.f") lines)"
  else echo "$n: DIFFERS in three runs"; bad=1; fi
  "$W/held_test" follow "$W/res/$n.out" || bad=1
done
exit $bad
