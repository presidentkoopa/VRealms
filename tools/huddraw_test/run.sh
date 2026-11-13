#!/bin/bash
# run.sh [script ...] -- for each script in scripts/: run it in the ORIGINAL (oracle.sh: ROTH.C headless,
# hudprobe.c as a mod), then through the port (huddraw_diff), compare every screen dump byte for byte
# and diff the HUD routine calls (HC), inventory (INV), state and no-ammo lines. Exit 0 = every screen identical.
# Everything goes under $HUDDRAW_WORK (default /tmp/huddraw_test); nothing into the repo (retail art).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
W=${HUDDRAW_WORK:-/tmp/huddraw_test}; mkdir -p "$W"
"$HERE/build.sh" "$W" >/dev/null || exit 1
SCRIPTS=${*:-$(ls "$HERE"/scripts/*.txt)}
bad=0
for s in $SCRIPTS; do
	n=$(basename "$s" .txt)
	"$HERE/oracle.sh" "$s" "$W" || { echo "$n: the original did not run"; bad=1; continue; }
	mkdir -p "$W/$n.ppm"
	"$W/huddraw_diff" "$DATA" "$W/$n.oracle.log" "$s" "$W/$n" "$W/$n.ppm" > "$W/$n.port.log" 2> "$W/$n.port.err"
	rc=$?
	F='^( state|HC |INV |NOAMMO |> )'
	grep -E "$F" "$W/$n.oracle.log" > "$W/$n.o"; grep -E "$F" "$W/$n.port.log" > "$W/$n.p"
	diff "$W/$n.o" "$W/$n.p" > "$W/$n.diff"; drc=$?
	echo "$n: $(grep '^SUMMARY' "$W/$n.port.log" | cut -c9-); trace lines $( [ $drc = 0 ] && echo "identical ($(grep -c '^HC ' "$W/$n.o") HUD calls)" || echo "DIFFER ($W/$n.diff)")"
	grep -E '^(PIX|PROBLEM|TABLE)' "$W/$n.port.log" | sed 's/^/   /'
	[ $rc = 0 ] && [ $drc = 0 ] || bad=1
done
exit $bad
