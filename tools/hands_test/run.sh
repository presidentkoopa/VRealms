#!/bin/sh
# The VR hands' finger math, outside the engine (test_handpose.cpp).
#   tools/hands_test/run.sh
set -e
cd "$(dirname "$0")"
OUT=${HANDS_TEST_OUT:-/tmp/hands_test_out}
mkdir -p "$OUT"
g++ -std=c++17 -O1 -Wall -Wextra -I../../src/r_data test_handpose.cpp -o "$OUT/test_handpose"
"$OUT/test_handpose"
