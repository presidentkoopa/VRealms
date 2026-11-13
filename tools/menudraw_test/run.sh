#!/bin/bash
# run.sh [script ...] -- for each script in scripts/: run it in the ORIGINAL (oracle.sh: ROTH.C headless,
# menuprobe.c as a mod), then through the port (menudraw_diff), compare every screen dump byte for byte
# and diff the hit-test lines. Exit 0 = every picture identical and every hit the same.
# Everything goes under $MENUDRAW_WORK (default /tmp/menudraw_test); nothing into the repo (retail art).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
W=${MENUDRAW_WORK:-/tmp/menudraw_test}; mkdir -p "$W"
"$HERE/build.sh" "$W" >/dev/null || exit 1
SCRIPTS=${*:-$(ls "$HERE"/scripts/*.txt)}
bad=0
for s in $SCRIPTS; do
	n=$(basename "$s" .txt)
	"$HERE/oracle.sh" "$s" "$W" 2>/dev/null || { echo "$n: the original did not run"; bad=1; continue; }
	mkdir -p "$W/$n.ppm"
	"$W/menudraw_diff" "$DATA" "$W/$n.oracle.log" "$W/$n" "$W/$n.ppm" > "$W/$n.port.log" 2> "$W/$n.port.err"
	rc=$?
	F='^(HG|H |CHG|CH |O |C )'
	grep -E "$F" "$W/$n.oracle.log" > "$W/$n.o"; grep -E "$F" "$W/$n.port.log" > "$W/$n.p"
	diff "$W/$n.o" "$W/$n.p" > "$W/$n.diff"; drc=$?
	echo "$n: $(grep '^SUMMARY' "$W/$n.port.log" | cut -c9-); hit/box lines $( [ $drc = 0 ] && echo identical || echo "DIFFER ($W/$n.diff)"); hit-test points $(grep -c '^H \|^CH ' "$W/$n.o")"
	grep '^PIX\|^PROBLEM' "$W/$n.port.log" | sed 's/^/   /'
	[ $rc = 0 ] && [ $drc = 0 ] || bad=1
done
exit $bad
