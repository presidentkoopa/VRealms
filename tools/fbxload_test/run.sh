#!/bin/sh
# The engine's FBX loader core (src/common/models/fbx_skinned.cpp) with ufbx, outside the engine, against
# the owner's hand_final.fbx. docs/FBX_LOADER.md "Acceptance" lists what each check is.
#   tools/fbxload_test/run.sh [path/to/hand_final.fbx]     (default: vrealms_iwad/models/hands/hand_final.fbx)
#   FBXLOAD_RENDER=1 also writes software renders of the loader's meshes (render.py) into the output dir.
set -e
cd "$(dirname "$0")/../.."
FBX=${1:-vrealms_iwad/models/hands/hand_final.fbx}
OUT=${FBXLOAD_TEST_OUT:-/tmp/fbxload_test_out}
mkdir -p "$OUT/dump"
gcc -std=c99 -O2 -c libraries/ufbx/ufbx.c -o "$OUT/ufbx.o"
g++ -std=c++17 -O2 -Wall -Wextra -Isrc/common/models -Isrc/r_data -Ilibraries/ufbx \
	tools/fbxload_test/test_fbxload.cpp src/common/models/fbx_skinned.cpp "$OUT/ufbx.o" -o "$OUT/test_fbxload" -lm
FRAMES=0,160,205,318,500,761,845,984,1100,1277,1278
"$OUT/test_fbxload" "$FBX" --dump "$OUT/dump" --angles $FRAMES > "$OUT/test.txt" || { cat "$OUT/test.txt"; exit 1; }
grep -v '^ANGLES' "$OUT/test.txt"
grep '^ANGLES' "$OUT/test.txt" > "$OUT/ours.txt"
echo
echo "== the loader's joint rotations against tools/fbx_hands/poses.py (its own FBX reader, no ufbx)"
python3 -I tools/fbxload_test/poses_ref.py "$FBX" $FRAMES > "$OUT/ref.txt"
python3 -I tools/fbxload_test/compare.py "$OUT/ours.txt" "$OUT/ref.txt"
if [ -n "$FBXLOAD_RENDER" ]; then
	TEX=vrealms_iwad/models/hands/lambert1_Base_Color.png
	python3 -I tools/fbxload_test/render.py "$OUT/dump" "$TEX" "$OUT/rest.png" Hand_Low_rest Hand_Low1_rest
	python3 -I tools/fbxload_test/render.py "$OUT/dump" "$TEX" "$OUT/posed.png" Hand_Low_500 Hand_Low1_500 Hand_Low_1100 Hand_Low1_845
fi
