#!/bin/sh
# The VR hands' finger math and placement, outside the engine (see test_handpose.cpp, test_iqm.py).
#   tools/hands_test/run.sh [models dir]   (default: vrealms_iwad/models/hands, else the converter's worktree)
set -e
cd "$(dirname "$0")"
OUT=${HANDS_TEST_OUT:-/tmp/hands_test_out}
mkdir -p "$OUT"
g++ -std=c++17 -O1 -Wall -Wextra -I../../src/r_data test_handpose.cpp -o "$OUT/test_handpose"
"$OUT/test_handpose"
if [ -n "$1" ]; then M="--models $1"; else M=""; fi
python3 -I test_iqm.py $M --exe "$OUT/test_handpose" --out "$OUT"
