#!/bin/sh
# Build and run the DBASE100 reader tests, and build the dump tool.
# Needs only g++ (C++17). Writes into ./out (or $1), never beside the sources.
#
#   tools/dbase100_test/build.sh            # tests (plain + sanitizers) and dump
#   out/dbase100_dump /path/to/DATA         # then dump the retail file
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../src/roth"
OUT=${1:-"$HERE/out"}
mkdir -p "$OUT"
FLAGS="-std=c++17 -Wall -Wextra -Werror -O1 -g"
CORE="$SRC/roth_dbase100.cpp $SRC/roth_dbase100_install.cpp $SRC/roth_install.cpp $SRC/roth_log.cpp"

g++ $FLAGS -o "$OUT/dbase100_test" "$HERE/dbase100_test.cpp" $CORE
g++ $FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all \
	-o "$OUT/dbase100_test_san" "$HERE/dbase100_test.cpp" $CORE
g++ $FLAGS -o "$OUT/dbase100_dump" "$HERE/dbase100_dump.cpp" $CORE
g++ $FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all \
	-o "$OUT/inventory_test" "$HERE/inventory_test.cpp" "$SRC/roth_inventory.cpp" $CORE

TMP=$(mktemp -d)
"$OUT/dbase100_test" "$TMP"
"$OUT/dbase100_test_san" "$TMP"
rm -rf "$TMP"
"$OUT/inventory_test"

# Exercise the dump tool on the synthetic fixture (no retail data here).
FIX=$(mktemp -d)
"$OUT/dbase100_test" --write-fixture "$FIX/DBASE100.DAT"
"$OUT/dbase100_dump" "$FIX/DBASE100.DAT" --all > "$FIX/dump.txt"
grep -q "LOADED CLEAN" "$FIX/dump.txt" && echo "dump tool: fixture LOADED CLEAN"
rm -rf "$FIX"
