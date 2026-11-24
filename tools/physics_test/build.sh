#!/bin/bash
# The engine-free test of roth_physics_core (docs/VR_PHYSICS.md): Jolt built once into $OUT/libjolt.a with the same
# defines as libraries/JoltPhysics/CMakeLists.txt, then the core and the test.
#   tools/physics_test/build.sh [out dir]   (default /tmp/physics_test)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-/tmp/physics_test}"
mkdir -p "$OUT/obj"
FLAGS="-std=c++17 -O2 -fno-fast-math -ffp-contract=off -DJPH_NO_FORCE_INLINE -I$ROOT/libraries/JoltPhysics"
if [ ! -f "$OUT/libjolt.a" ]; then
	echo "building Jolt (once)..."
	find "$ROOT/libraries/JoltPhysics/Jolt" -name '*.cpp' | xargs -P "$(nproc)" -I{} sh -c 'f="{}"; o="'"$OUT"'/obj/$(echo "$f" | md5sum | cut -c1-16).o"; g++ '"$FLAGS"' -w -c "$f" -o "$o"'
	ar rcs "$OUT/libjolt.a" "$OUT"/obj/*.o
fi
g++ $FLAGS -Wall -I"$ROOT/src/roth" -o "$OUT/physics_test" "$ROOT/tools/physics_test/physics_test.cpp" "$ROOT/src/roth/roth_physics_core.cpp" "$OUT/libjolt.a" -lpthread
"$OUT/physics_test"
