#!/bin/bash
# oracle.sh <script> <outdir> -- run one script through the ORIGINAL (ROTH.C headless, STUDY1, with
# invprobe.c loaded as a mod). Writes <outdir>/<name>.oracle.log and <outdir>/<name>/<dump>.pan.
# Holds /tmp/oracle_shot.lock around the run (the host's /roth_fb shm is machine-wide).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OB=${ORACLE_BUILD:-/home/claude/oracle_build}
S=$(realpath "$1"); W=$(realpath -m "$2"); mkdir -p "$W"
n=$(basename "$S" .txt)
$OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -Wall -I$OB/rothc/sdk/include -o "$W/plugin.so" "$HERE/invprobe.c" || exit 1
[ -s "$W/syms.txt" ] || nm $OB/rothc/roth_c/roth > "$W/syms.txt"
G="$W/game"
if [ ! -d "$G" ]; then
	[ -d $OB/games/STUDY1 ] || $OB/oracle_shot.sh STUDY1 0 0 0 "$W/warm.png" >/dev/null 2>&1
	cp -a $OB/games/STUDY1 "$G"
fi
mkdir -p "$G/mods/invprobe"; cp -f "$W/plugin.so" "$G/mods/invprobe/plugin.so"
rm -rf "$W/$n"; mkdir -p "$W/$n"
exec 9>/tmp/oracle_shot.lock; flock 9
rm -f "$W/$n.oracle.log"
( cd "$G" && INVPROBE_AT=${INVPROBE_AT:-200} INVPROBE_SCRIPT="$S" INVPROBE_OUT="$W/$n.oracle.log" INVPROBE_SYMS="$W/syms.txt" INVPROBE_DIR="$W/$n" \
	ORACLE_PAINT=0 ORACLE_OUT="$W/$n.bin" ORACLE_FRAMES=100000 timeout 180 $OB/rothc/roth_c/roth --game-dir . --headless > "$W/$n.engine.log" 2>&1 )
flock -u 9; exec 9>&-
rm -f "$W/$n.bin"
[ -s "$W/$n.oracle.log" ] || { echo "$n: the original wrote nothing (see $W/$n.engine.log)" >&2; exit 1; }
