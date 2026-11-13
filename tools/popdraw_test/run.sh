#!/bin/bash
# run.sh [script ...] -- for each script in scripts/: run it in the ORIGINAL (oracle.sh: ROTH.C headless,
# popprobe.c as a mod), then through the port (popdraw_diff), compare every popup picture byte for byte
# and diff the popup's state, its topic records and the hit tests. Exit 0 = every picture identical,
# every state and hit line the same.
# Everything goes under $POPDRAW_WORK (default /tmp/popdraw_test); nothing into the repo (retail art).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
W=${POPDRAW_WORK:-/tmp/popdraw_test}; mkdir -p "$W"
"$HERE/build.sh" "$W" >/dev/null || exit 1
SCRIPTS=${*:-$(ls "$HERE"/scripts/*.txt)}
bad=0
# The lines both sides print: the popup's state (its info latch is the original's count, kept
# as a flag by the port), the cells, the film's counts, the topic records, the label, the hit tests,
# the keys read and the film's end.
norm() { grep -E '^(P |K | cells| film| TL | label| RH|HG|H |R examine)' "$1" | sed -E 's/latch=[-0-9*]+/latch=*/; s/( film boundaries=[0-9]+ decodes=[0-9]+).*/\1/'; }
for s in $SCRIPTS; do
	n=$(basename "$s" .txt)
	"$HERE/oracle.sh" "$s" "$W" 2>/dev/null || { echo "$n: the original did not run"; bad=1; continue; }
	mkdir -p "$W/$n.ppm"
	"$W/popdraw_diff" "$DATA" "$W/$n.oracle.log" "$s" "$W/$n" "$W/$n.ppm" > "$W/$n.port.log" 2> "$W/$n.port.err"
	rc=$?
	norm "$W/$n.oracle.log" > "$W/$n.o"; norm "$W/$n.port.log" > "$W/$n.p"
	diff "$W/$n.o" "$W/$n.p" > "$W/$n.diff"; drc=$?
	echo "$n: $(grep '^SUMMARY' "$W/$n.port.log" | cut -c9-); state lines $( [ $drc = 0 ] && echo identical || echo "DIFFER ($W/$n.diff)"); hit-test points $(grep -c '^H ' "$W/$n.o")"
	grep '^PIX\|^PROBLEM\|^RESTORED' "$W/$n.port.log" | sed 's/^/   /'
	[ $rc = 0 ] && [ $drc = 0 ] || bad=1
done
exit $bad
