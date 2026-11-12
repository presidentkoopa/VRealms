#!/bin/sh
# Build and run the menus test. Needs only g++ (C++17). Writes into ./out (or $1).
#   tools/menus_test/build.sh
#   out/menus_test "/path/to/DATA" --oracle dump1.txt --oracle dump2.txt
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../src/roth"
OUT=${1:-"$HERE/out"}
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$OUT/menus_test" \
	"$HERE/menus_test.cpp" "$SRC/roth_menus.cpp" "$SRC/roth_gameflow.cpp" "$SRC/roth_gamecore.cpp" \
	"$SRC/roth_dbase100_interp.cpp" "$SRC/roth_inventory.cpp" "$SRC/roth_dbase100.cpp" "$SRC/roth_raw.cpp"
"$OUT/menus_test" "${ROTH_DATA:-/nonexistent}" | tail -1
