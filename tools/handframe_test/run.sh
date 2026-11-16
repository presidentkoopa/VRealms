#!/bin/sh
# The hand frame, outside the engine (test_handframe.cpp says what it proves).
#   tools/handframe_test/run.sh
# Compiles the engine's own src/r_data/vr_posemath.h and vr_handpose_math.h and VSMatrix
# (src/common/utility/matrix.cpp); shim/math/cmath.h stands libm in for the engine's reliable math.
set -e
cd "$(dirname "$0")"
OUT=${HANDFRAME_TEST_OUT:-/tmp/handframe_test_out}
mkdir -p "$OUT"
S=../../src
g++ -std=c++17 -O1 -Wall -Wno-unused-function -Ishim -I$S/r_data -I$S -I$S/utility -I$S/common/utility -I$S/common/engine \
	test_handframe.cpp $S/common/utility/matrix.cpp -o "$OUT/test_handframe"
"$OUT/test_handframe"
