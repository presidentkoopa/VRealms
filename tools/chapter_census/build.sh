#!/bin/sh
# Build the chapter census (engine-free: g++ and the src/roth readers). Writes into $1 (default /tmp/chapter_census_bin).
#   sh tools/chapter_census/build.sh [out-dir]
#   <out>/chapter_census "<ROTH DATA folder>" -all > tools/chapter_census/chapters_retail.txt
set -e
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../src/roth"; OUT=${1:-/tmp/chapter_census_bin}
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -O1 -I "$SRC" -o "$OUT/chapter_census" "$HERE/chapter_census.cpp" \
	"$SRC/roth_raw.cpp" "$SRC/roth_install.cpp" "$SRC/roth_dbase100.cpp" "$SRC/roth_dbase100_install.cpp" "$SRC/roth_log.cpp"
echo "built $OUT/chapter_census"
