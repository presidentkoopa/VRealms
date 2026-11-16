#!/bin/sh
# The item models' reader, engine-free (src/roth/roth_img3d_core.cpp + the engine's picture decoder,
# src/common/models/fbx_image.h), on procedural boxes only (tools/img3d/make_placeholder.py), and tools/img3d/post.py
# on the same boxes (tools/img3d_test/post_test.py; numpy + Pillow). No retail art.
#   tools/img3d_test/build.sh [out-dir]          (default /tmp/img3d_test)
# Also builds the pipeline's extractor (tools/img3d/img3d_extract.cpp) to prove it compiles and links with g++;
# with ROTH_DATA set it runs it on two items into the out-dir (outside the repository).
set -e
HERE=$(cd "$(dirname "$0")" && pwd); ROOT="$HERE/../.."; SRC="$ROOT/src/roth"; OUT=${1:-/tmp/img3d_test}
mkdir -p "$OUT"
SAN="-g -fsanitize=address,undefined -fno-sanitize-recover=all"
g++ -std=c++17 -O1 -Wall -Wextra -Werror $SAN -I "$SRC" -I "$ROOT/src/common/models" -I "$ROOT/src/common/thirdparty" \
	-I "$ROOT/src/common/thirdparty/stb" -o "$OUT/img3d_test" "$HERE/img3d_test.cpp" "$SRC/roth_img3d_core.cpp"
python3 -I "$ROOT/tools/img3d/make_placeholder.py" "$OUT/boxes" > /dev/null
python3 -I "$ROOT/tools/img3d/make_placeholder.py" "$OUT/boxes_node" --trs > /dev/null
python3 -I "$HERE/post_test.py" "$OUT/boxes" "$OUT/post" | tail -1
"$OUT/img3d_test" "$OUT/boxes" "$OUT/boxes_node" "$OUT/post/out"
V="$ROOT/tools/voxelize_parked"
g++ -std=c++17 -O1 -Wall -Wextra -I "$SRC" -I "$V" -o "$OUT/img3d_extract" "$ROOT/tools/img3d/img3d_extract.cpp" \
	"$SRC/roth_install.cpp" "$SRC/roth_dbase100.cpp" "$SRC/roth_icons.cpp" "$SRC/roth_das.cpp" "$SRC/roth_palette.cpp" \
	"$SRC/roth_gdv.cpp" "$SRC/roth_log.cpp" "$V/roth_voxelize.cpp" 2>&1 | grep -E "error|img3d_extract.cpp.*warning" || true
test -x "$OUT/img3d_extract" && echo "built $OUT/img3d_extract"
if [ -n "$ROTH_DATA" ]; then "$OUT/img3d_extract" "$ROTH_DATA" "$OUT/extract" 16 21; fi
# The boxes committed for the bridge's flat test (tools/claude_bridge/cfg/img3d_flat.cfg) are exactly what the
# generator writes now.
python3 -I "$ROOT/tools/img3d/make_placeholder.py" "$OUT/boxes_again" > /dev/null
for f in manifest.json 16_placeholder.glb 17_placeholder.glb 21_placeholder.glb; do
	cmp "$ROOT/tools/img3d/placeholder/$f" "$OUT/boxes_again/$f" || { echo "FAIL tools/img3d/placeholder/$f differs from make_placeholder.py's output"; exit 1; }
done
echo "tools/img3d/placeholder: identical to make_placeholder.py's output"
