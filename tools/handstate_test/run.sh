#!/bin/sh
# L2 (roth_handstate.h) and L1's engine-free rules (roth_vrrules.h), scripted hands. Engine-free.
set -e
cd "$(dirname "$0")/../.."
OUT=${TMPDIR:-/tmp}/handstate_test
mkdir -p "$OUT"
g++ -std=c++17 -O1 -Wall -Wextra -Isrc/roth tools/handstate_test/test_handstate.cpp -o "$OUT/test_handstate"
"$OUT/test_handstate"
