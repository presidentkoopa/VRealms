#!/bin/bash
# run.sh [script ...] -- for each script in scripts/: run it in the ORIGINAL (oracle.sh: ROTH.C headless,
# invprobe.c as a mod), then through the port (invdraw_diff), compare every panel dump byte for byte
# and diff the drawing state / label / hotspot lines. Exit 0 = every picture identical.
# Everything goes under $INVDRAW_WORK (default /tmp/invdraw_test); nothing into the repo (retail art).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
W=${INVDRAW_WORK:-/tmp/invdraw_test}; mkdir -p "$W"
"$HERE/build.sh" "$W" >/dev/null || exit 1
SCRIPTS=${*:-$(ls "$HERE"/scripts/*.txt)}
bad=0
for s in $SCRIPTS; do
	n=$(basename "$s" .txt)
	"$HERE/oracle.sh" "$s" "$W" || { echo "$n: the original did not run"; bad=1; continue; }
	mkdir -p "$W/$n.ppm"
	"$W/invdraw_diff" "$DATA" "$W/$n.oracle.log" "$s" "$W/$n" "$W/$n.ppm" > "$W/$n.port.log" 2> "$W/$n.port.err"
	rc=$?
	F='^( state| hands .*entries| label=| hot0| hot1|> |H )'
	grep -E "$F" "$W/$n.oracle.log" > "$W/$n.o"; grep -E "$F" "$W/$n.port.log" > "$W/$n.p"
	diff "$W/$n.o" "$W/$n.p" > "$W/$n.diff"; drc=$?
	echo "$n: $(grep '^SUMMARY' "$W/$n.port.log" | cut -c9-); state lines $( [ $drc = 0 ] && echo identical || echo "DIFFER ($W/$n.diff)"); hit-test points $(grep -c '^H ' "$W/$n.o")"
	grep '^PIX' "$W/$n.port.log" | sed 's/^/   /'
	[ $rc = 0 ] && [ $drc = 0 ] || bad=1
done
exit $bad
