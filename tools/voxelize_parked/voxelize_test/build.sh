#!/bin/sh
# Build the close-up voxelizer's tests (engine-free, g++ only) and run the KVX
# round trip through the engine's own parser.
#
#   tools/voxelize_test/build.sh [out-dir]          (default /tmp/voxelize_test)
#
# With ROTH_DATA set to the retail DATA folder it also voxelizes four sample
# items and round-trips their KVX. The full per-item table and the pictures:
#
#   $OUT/voxelize_test "$ROTH_DATA" --pictures /tmp/vxpics > /tmp/voxelize_table.txt
#   python3 tools/voxelize_test/contact_sheet.py /tmp/vxpics /tmp/voxel_sheet.png /tmp/voxelize_table.txt 15,16,20
#
# Everything made from the retail films stays outside the repository.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$HERE/../.."
SRC="$ROOT/src/roth"
OUT=${1:-${VOXELIZE_WORK:-/tmp/voxelize_test}}
mkdir -p "$OUT"
FLAGS="-std=c++17 -O2"
STRICT="$FLAGS -Wall -Wextra -Werror"

# The DAS reader (for the world sprite sizes) and the palette it needs are
# built as they are; they are not this test's code.
g++ $FLAGS -c -o "$OUT/roth_das.o" "$SRC/roth_das.cpp"
g++ $FLAGS -c -o "$OUT/roth_palette.o" "$SRC/roth_palette.cpp"
g++ $STRICT -o "$OUT/voxelize_test" "$HERE/voxelize_test.cpp" "$SRC/roth_voxelize.cpp" "$SRC/roth_gdv.cpp" \
	"$SRC/roth_dbase100.cpp" "$OUT/roth_das.o" "$OUT/roth_palette.o"

# The engine's KVX loader, src/common/models/voxels.cpp, compiled UNCHANGED
# against engine_stub/ (the few engine types it names).
g++ $FLAGS -c -I "$HERE/engine_stub" -I "$ROOT/src/common/utility" -o "$OUT/voxels.o" "$ROOT/src/common/models/voxels.cpp"
g++ $FLAGS -c -I "$HERE/engine_stub" -o "$OUT/engine_stub.o" "$HERE/engine_stub/engine_stub.cpp"
g++ $STRICT -I "$HERE/engine_stub" -I "$ROOT/src/common/models" -o "$OUT/kvx_roundtrip" "$HERE/kvx_roundtrip.cpp" \
	"$SRC/roth_voxelize.cpp" "$SRC/roth_gdv.cpp" "$SRC/roth_dbase100.cpp" "$OUT/voxels.o" "$OUT/engine_stub.o"
# The same with the sanitizers.
# The engine reads KVX integers unaligned (m_swap.h GetInt on x86), as it does for every KVX: not checked.
SAN="-g -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=all"
g++ $FLAGS $SAN -c -I "$HERE/engine_stub" -I "$ROOT/src/common/utility" -o "$OUT/voxels_san.o" "$ROOT/src/common/models/voxels.cpp"
g++ $FLAGS $SAN -c -I "$HERE/engine_stub" -o "$OUT/engine_stub_san.o" "$HERE/engine_stub/engine_stub.cpp"
g++ $STRICT $SAN -I "$HERE/engine_stub" -I "$ROOT/src/common/models" -o "$OUT/kvx_roundtrip_san" "$HERE/kvx_roundtrip.cpp" \
	"$SRC/roth_voxelize.cpp" "$SRC/roth_gdv.cpp" "$SRC/roth_dbase100.cpp" "$OUT/voxels_san.o" "$OUT/engine_stub_san.o"
echo "built $OUT/voxelize_test $OUT/kvx_roundtrip"

if [ -n "$ROTH_DATA" ]; then
	"$OUT/kvx_roundtrip" "$ROTH_DATA" 15,16,20,9
else
	"$OUT/kvx_roundtrip"
	"$OUT/kvx_roundtrip_san" | tail -1
fi
