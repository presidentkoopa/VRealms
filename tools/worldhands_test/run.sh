#!/bin/sh
# tools/worldhands_test/run.sh -- chapter 2's world-by-hand rules (src/roth/roth_worldhandslogic.h), engine-free:
# release, pull, press, twist, sit, drink, the contact disc. Exit 0 = every check passed. Nothing is written
# into the repo.
set -e
cd "$(dirname "$0")/../.."
OUT=${TMPDIR:-/tmp}/worldhands_test
mkdir -p "$OUT"
g++ -std=c++17 -O1 -Wall -Wextra -Werror -Isrc/roth tools/worldhands_test/test_worldhands.cpp -o "$OUT/test_worldhands"
"$OUT/test_worldhands"
rm -rf "$OUT"
