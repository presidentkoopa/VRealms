#!/bin/bash
# tools/handclick_test/run.sh -- the original's secondary click on each cursor type
# (run_gameplay_frame, E/game_core.c:515-732; the click a landed fist makes) through the ORIGINAL (clickprobe.c in the headless
# ROTH.C build, /home/claude/oracle_build) against src/roth/roth_handclick.h. Exit 0 = no difference.
# Outputs under $HANDCLICK_WORK (default /tmp/handclick_test); nothing into the repo.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OB=${ORACLE_BUILD:-/home/claude/oracle_build}
W=${HANDCLICK_WORK:-/tmp/handclick_test}; mkdir -p "$W"
$OB/bin/i386-musl-gcc -shared -fPIC -std=gnu11 -O1 -Wall -Wextra -I$OB/rothc/sdk/include -o "$W/plugin.so" "$HERE/clickprobe.c" || exit 1
g++ -std=c++17 -O1 -Wall -Wextra -Werror -I"$HERE/../../src/roth" -o "$W/test_handclick" "$HERE/test_handclick.cpp" || exit 1
[ -s "$W/syms.txt" ] || nm $OB/rothc/roth_c/roth > "$W/syms.txt"
G="$W/game"
if [ ! -d "$G" ]; then
	[ -d $OB/games/STUDY1 ] || $OB/oracle_shot.sh STUDY1 0 0 0 "$W/warm.png" >/dev/null 2>&1
	cp -a $OB/games/STUDY1 "$G"
fi
mkdir -p "$G/mods/clickprobe"; cp -f "$W/plugin.so" "$G/mods/clickprobe/plugin.so"
exec 9>/tmp/oracle_shot.lock; flock 9
rm -f "$W/clickprobe.log"
( cd "$G" && CLICKPROBE_OUT="$W/clickprobe.log" CLICKPROBE_SYMS="$W/syms.txt" ORACLE_PAINT=0 ORACLE_OUT="$W/o.bin" ORACLE_FRAMES=100000 \
	timeout 180 $OB/rothc/roth_c/roth --game-dir . --headless > "$W/engine.log" 2>&1 )
flock -u 9; exec 9>&-
rm -f "$W/o.bin"
[ -s "$W/clickprobe.log" ] || { echo "the original wrote nothing (see $W/engine.log)"; tail -5 "$W/engine.log"; exit 1; }
"$W/test_handclick" "$W/clickprobe.log"
