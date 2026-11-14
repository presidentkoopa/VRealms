#!/bin/bash
# run_diff.sh -- lighting a candle wick by wick leaves the ORIGINAL's game state exactly as one click does.
#
# The use that wick-by-wick lighting fires is decided by WickLighting (src/roth/roth_wicks.cpp), the code the
# engine runs: tools/wicks_test turns each scenario (an order and timing of wicks touched) into the clicks it
# asks for. Each scenario's clicks are played into the ORIGINAL (ROTH.C, headless, candleprobe.c) on STUDY1's
# five-candle candelabrum 211/0 with the Matchbox in the right hand, and so is the reference: one click. After
# 620 frames (past the 0x12 delay, the 0x0d picture, the 0x1d light ramp) every byte of the command table,
# the geometry (sector lights), the objects, the flag bitmap and the chain globals that changed since the
# snapshot is printed; every scenario's lines must equal the reference's.
# Needs the oracle build (/home/claude/oracle_build) and the retail DATA. Writes only under $W (default /tmp/candlew).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=${W:-/tmp/candlew}; export W
D=${ROTH_DATA:-"/mnt/user-data/uploads/Realms of the Haunting/DATA"}
mkdir -p "$W/res"
sh "$HERE/../wicks_test/build.sh" "$W/wicks_test" || exit 2
"$W/wicks_test" "$D" scripts "$W/res" || exit 2
# THE ORACLE IS NOT ALWAYS THE SAME RUN TO RUN: about one run in eight here crashed or showed geometry
# changes far from the candle (seen 2026-11-13). So the reference is taken twice and must agree, and a
# scenario that differs is run again, up to three runs; every rerun is printed, none is hidden.
oracle() { "$HERE/run_oracle.sh" STUDY1 "$1" "$2" && grep -v "^R click" "$2" > "$3"; }
for r in 1 2 3; do
  oracle "$W/res/ref.script" "$W/res/ref.out" "$W/res/ref.f" && oracle "$W/res/ref.script" "$W/res/ref2.out" "$W/res/ref2.f"     && cmp -s "$W/res/ref.f" "$W/res/ref2.f" && break
  echo "reference: the two runs disagree or one failed (attempt $r)"; [ $r = 3 ] && exit 3
done
echo "reference (one click): $(grep -c '^R click' "$W/res/ref.out") click(s), $(grep -c '^[CGOF] ' "$W/res/ref.f") changed bytes, two runs identical"
bad=0
for s in "$W"/res/*.script; do
  n=$(basename "$s" .script); [ "$n" = ref ] && continue
  ok=0
  for r in 1 2 3; do
    if oracle "$s" "$W/res/$n.out" "$W/res/$n.f" && cmp -s "$W/res/ref.f" "$W/res/$n.f"; then ok=1; break; fi
    echo "$n: run $r $( [ -s "$W/res/$n.f" ] && echo "differs in $(diff "$W/res/ref.f" "$W/res/$n.f" | grep -c '^[<>]') lines" || echo "failed"), running again"
  done
  if [ $ok = 1 ]; then echo "$n: IDENTICAL to one click ($(grep -c '^R click' "$W/res/$n.out") click(s), $(wc -l < "$W/res/$n.f") lines)"
  else echo "$n: DIFFERS in three runs"; bad=1; fi
done
exit $bad
