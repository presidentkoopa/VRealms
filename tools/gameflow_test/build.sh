#!/bin/sh
# Build and run the game-flow test. Needs only g++ (C++17). Writes into ./out
# (or $1), never beside the sources.
#
#   tools/gameflow_test/build.sh                       # build, run on the retail data if ROTH_DATA is set
#   out/gameflow_test "/path/to/DATA" --oracle dump.txt --sav SAVE0.SAV
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../src/roth"
OUT=${1:-"$HERE/out"}
mkdir -p "$OUT"
FLAGS="-std=c++17 -Wall -Wextra -Werror -O1 -g"
CORE="$SRC/roth_gameflow.cpp $SRC/roth_gamecore.cpp $SRC/roth_dbase100_interp.cpp $SRC/roth_inventory.cpp $SRC/roth_dbase100.cpp $SRC/roth_raw.cpp"
g++ $FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -o "$OUT/gameflow_test" "$HERE/gameflow_test.cpp" $CORE
"$OUT/gameflow_test" "${ROTH_DATA:-/nonexistent}" | tail -1
