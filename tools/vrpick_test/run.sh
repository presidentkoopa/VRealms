#!/bin/sh
# tools/vrpick_test/run.sh -- the VR picker's engine-free check.
#
# src/roth/roth_cursor.h (classify_cursor_target_object, ported) against the
# original's own text of that function, cut out of ROTH.C's input.c HERE, at
# build time, so the comparison is with the source and not with a copy of it.
#
#   ROTHC_SRC  ROTH.C's engine sources (default /home/claude/oracle_build/rothc/roth_c/src/engine)
set -e
cd "$(dirname "$0")/../.."
SRC=${ROTHC_SRC:-/home/claude/oracle_build/rothc/roth_c/src/engine}
OUT=${TMPDIR:-/tmp}/vrpick_test
mkdir -p "$OUT"
[ -f "$SRC/input.c" ] || { echo "no ROTH.C input.c under $SRC (set ROTHC_SRC)"; exit 2; }
{
	echo '#include "orig_prelude.h"'
	echo "/* cut from $SRC/input.c by tools/vrpick_test/run.sh */"
	awk '/^int32_t classify_cursor_target_object\(const regs_t \*in\)/{f=1} f{print} f&&/^}/{exit}' "$SRC/input.c"
} > "$OUT/orig_classify.c"
lines=$(wc -l < "$OUT/orig_classify.c")
[ "$lines" -gt 100 ] || { echo "could not cut the function out of input.c ($lines lines)"; exit 2; }
gcc -std=c11 -O1 -w -Itools/vrpick_test -c "$OUT/orig_classify.c" -o "$OUT/orig_classify.o"
gcc -std=c11 -O1 -w -Itools/vrpick_test -c tools/vrpick_test/orig_env.c -o "$OUT/orig_env.o"
g++ -std=c++17 -O1 -Wall -Isrc/roth tools/vrpick_test/test_cursor.cpp "$OUT/orig_classify.o" "$OUT/orig_env.o" -o "$OUT/test_cursor"
"$OUT/test_cursor"
