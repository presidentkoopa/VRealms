#!/bin/bash
# tools/surfbox_test/run.sh [match|texel|all] [MAP ...] -- the RAW records' surface box (src/roth/roth_surfbox.h)
# against the ORIGINAL (boxprobe.c in the headless ROTH.C build, /home/claude/oracle_build). Exit 0 = no difference.
#   match   every boxed record's target, pick type, band and texel point through dispatch_entry_command_trigger / _b
#   texel   the original's own cursor picks over faces and floors that carry a box, against WallPickTexel / FlatPickTexel
# No MAP: all 44 retail maps. Outputs under $SURFBOX_WORK (default /tmp/surfbox_test); nothing into the repo.
# Needs ROTH_DATA (default the uploaded retail DATA). SURFBOX_MUT=<n> runs the port side with mutation n (README).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=${SURFBOX_WORK:-/tmp/surfbox_test}; mkdir -p "$W"
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
WHAT=${1:-all}; shift || true
MAPS="$*"
[ -n "$MAPS" ] || MAPS="ABAGATE2 AELF ANUBIS AQUA1 AQUA2 CAVERNS CAVERNS2 CAVERNS3 CHURCH1 DOMINION DOPPLE ELOHIM1 GNARL1 GRAVE LRINTH LRINTH1 MAS3 MAS4 MAS6 MAS7 MAUSO1EA MAUSO1EB MAZE OPTEMP1 RAQUIA1 RAQUIA2 RAQUIA3 RAQUIA4 RAQUIA5 SALVAT SOULST2 SOULST3 STUDY1 STUDY2 STUDY3 STUDY4 TEMPLE1 TGATE1F TGATE1G TGATE1H TGATE1I TOWER1 VICAR VICAR1"
sh "$HERE/build.sh" "$W/surfbox_test" || exit 1
T="$W/surfbox_test"
fail=0
for M in $MAPS; do
	if [ "$WHAT" = match ] || [ "$WHAT" = all ]; then
		n=$("$T" "$DATA" "$M" match-script "$W/$M.match.txt") || { echo "$M: no match script"; fail=1; continue; }
		case "$n" in *": 0 match cases"*) echo "$M: no record with a box: nothing to match"; continue;; esac
		SB_SETTLE=3 "$HERE/oracle_run.sh" "$M" "$W/$M.match.txt" "$W/$M.match.log" || { fail=1; continue; }
		"$T" "$DATA" "$M" match-check "$W/$M.match.log" ${SURFBOX_MUT:-} > "$W/$M.match.out"; rc=$?
		tail -26 "$W/$M.match.out"; [ $rc -eq 0 ] || fail=1
	fi
	if [ "$WHAT" = texel ] || [ "$WHAT" = all ]; then
		n=$("$T" "$DATA" "$M" texel-script "$W/$M.texel.txt") || { echo "$M: no texel script"; fail=1; continue; }
		case "$n" in *": 0 poses"*) echo "$M: no pose in front of a boxed face or floor"; continue;; esac
		SB_SETTLE=20 SB_NOCHAINS=1 "$HERE/oracle_run.sh" "$M" "$W/$M.texel.txt" "$W/$M.texel.log" || { fail=1; continue; }
		"$T" "$DATA" "$M" texel-check "$W/$M.texel.log" ${SURFBOX_MUT:-} > "$W/$M.texel.out"; rc=$?
		tail -24 "$W/$M.texel.out"; [ $rc -eq 0 ] || fail=1
		rm -f "$W/$M.texel.log"          # about 1 MB of log per pose; the counts above are the result
	fi
done
exit $fail
