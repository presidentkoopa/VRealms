#!/bin/bash
# run.sh [script ...] -- for each script in scripts/: run it on the ORIGINAL (ROTH.C, headless, with
# dlgprobe.c loaded as a mod), run it on the port (dialogue_diff), and diff. Exit 0 = all identical.
# Everything is written under $DLG_WORK (default /tmp/dialogue_test); nothing into the repo
# (the logs hold retail text). Holds /tmp/oracle_shot.lock around each run of the original.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OB=${ORACLE_BUILD:-/home/claude/oracle_build}
DATA="${ROTH_DATA:-/mnt/user-data/uploads/Realms of the Haunting/DATA}"
W=${DLG_WORK:-/tmp/dialogue_test}; mkdir -p "$W"
"$HERE/build.sh" "$W" >/dev/null || exit 1
$OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -I$OB/rothc/sdk/include -o "$W/plugin.so" "$HERE/dlgprobe.c" || exit 1
nm $OB/rothc/roth_c/roth > "$W/syms.txt"
G="$W/game"
if [ ! -d "$G" ]; then
	[ -d $OB/games/STUDY1 ] || $OB/oracle_shot.sh STUDY1 0 0 0 "$W/warm.png" >/dev/null 2>&1
	cp -a $OB/games/STUDY1 "$G"
fi
mkdir -p "$G/mods/dlgprobe"; cp -f "$W/plugin.so" "$G/mods/dlgprobe/plugin.so"
SCRIPTS=${*:-$(ls "$HERE"/scripts/*.txt)}
bad=0
for s in $SCRIPTS; do
	n=$(basename "$s" .txt)
	exec 9>/tmp/oracle_shot.lock; flock 9
	rm -f "$W/$n.oracle.log"
	( cd "$G" && DLGPROBE_AT=200 DLGPROBE_SCRIPT="$s" DLGPROBE_OUT="$W/$n.oracle.log" DLGPROBE_SYMS="$W/syms.txt" \
		ORACLE_PAINT=0 ORACLE_OUT="$W/$n.bin" ORACLE_FRAMES=100000 timeout 180 $OB/rothc/roth_c/roth --game-dir . --headless > "$W/$n.engine.log" 2>&1 )
	flock -u 9; exec 9>&-
	rm -f "$W/$n.bin"
	[ -s "$W/$n.oracle.log" ] || { echo "$n: the original wrote nothing (see $W/$n.engine.log)"; bad=1; continue; }
	"$W/dialogue_diff" "$DATA" "$W/$n.oracle.log" "$s" > "$W/$n.port.log" 2> "$W/$n.port.err" || { echo "$n: port failed"; bad=1; continue; }
	F='^C rng_next_index_for_count|^ENV|^# '
	grep -Ev "$F" "$W/$n.oracle.log" > "$W/$n.o"; grep -Ev "$F" "$W/$n.port.log" > "$W/$n.p"
	if diff "$W/$n.o" "$W/$n.p" > "$W/$n.diff"; then
		echo "$n: IDENTICAL ($(grep -c '^> ' "$W/$n.o") commands, $(grep -c '^S ' "$W/$n.o") state blocks, $(wc -l < "$W/$n.o") lines)"
	else echo "$n: DIFFERS -- $W/$n.diff"; bad=1; fi
done
exit $bad
