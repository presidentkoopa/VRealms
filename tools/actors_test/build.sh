#!/bin/sh
# Build the actor rules' host test. Engine-free: g++ and the src/roth readers.
#   tools/actors_test/build.sh [outdir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../src/roth"
OUT=${1:-"$HERE/out"}
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I"$SRC" \
	-o "$OUT/actors_test" "$HERE/actors_test.cpp" "$SRC"/roth_actors.cpp "$SRC"/roth_actor_*.cpp \
	"$SRC/roth_raw.cpp" "$SRC/roth_dbase100.cpp"
