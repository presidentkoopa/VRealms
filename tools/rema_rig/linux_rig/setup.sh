#!/bin/bash
# REMA Linux rig: one command, no compiling.
#   sh tools/linux_rig/setup.sh            -> installs runtime packages, checks that the prebuilt
#                                             engine in bin/ matches THIS commit's engine source.
#   sh tools/linux_rig/setup.sh build      -> compiles instead (~90 min on 2 cores), same output.
# Output: tools/linux_rig/bin/ (doomxr + its pk3s). Then: monado_start.sh, boot.sh.
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../.." && pwd); BIN=$HERE/bin
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -q libsdl2-2.0-0 libopenal1 libvpx9 libgtk-3-0 xvfb mesa-vulkan-drivers vulkan-tools \
  libopenxr-loader1 libopenxr1-monado monado-service x11-apps imagemagick freedoom >/dev/null
# Which prebuilt engine: bin/ IS the prebuilt engine, committed in the repo (bin/SOURCE names
# the engine-source commit it was built from; CURRENT says the same). It is usable only if the
# checkout's engine code is IDENTICAL to that commit's (compared by content, so it works in a
# shallow clone, where `git log -- src` cannot be trusted).
ENGINE_PATHS="src wadsrc wadsrc_bm wadsrc_extra wadsrc_lights wadsrc_widepix libraries CMakeLists.txt cmake"
SRC=$(cat "$HERE/CURRENT")
if [ "$1" != build ]; then
  git -C "$REPO" cat-file -e "$SRC^{commit}" 2>/dev/null || git -C "$REPO" fetch -q --depth 1 origin "$SRC" 2>/dev/null \
    || git -C "$REPO" fetch -q --depth 1 origin "$(gh api repos/presidentkoopa/UZDXREMA/commits/$SRC -q .sha)"
  if ! git -C "$REPO" diff --quiet "$SRC" HEAD -- $ENGINE_PATHS; then
    echo "Engine code differs from the prebuilt rig ($SRC):"; git -C "$REPO" diff --stat "$SRC" HEAD -- $ENGINE_PATHS | tail -5
    echo "Run: bash $0 build   (then publish.sh commits the new bin/ and CURRENT)"; exit 1
  fi
fi
if [ "$1" = build ]; then
  apt-get install -y -q cmake ninja-build g++ libsdl2-dev libopenal-dev libvulkan-dev libgtk-3-dev libvpx-dev >/dev/null
  cmake -S "$REPO" -B "$HERE/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_OPENXR=ON -DOPENXR_DIR="$HERE/openxr" -DFORCE_NO_LTO=ON >/dev/null
  ninja -C "$HERE/build" -j"$(nproc)" > "$HERE/build.log" 2>&1 || { tail -20 "$HERE/build.log"; exit 1; }
  SRC=$(git -C "$REPO" rev-parse --short=8 HEAD)
  rm -rf "$BIN"; mkdir -p "$BIN"; cp "$HERE/build/doomxr" "$HERE"/build/*.pk3 "$BIN/"; echo "$SRC" > "$BIN/SOURCE"
  echo "built for $SRC. Now: bash $HERE/publish.sh   (commits bin/ and CURRENT so nobody builds it twice)"
fi
[ "$(cat "$BIN/SOURCE")" = "$SRC" ] || { echo "bin/ is for $(cat "$BIN/SOURCE"), expected $SRC: refusing."; exit 1; }
(cd "$BIN" && sha256sum -c SHA256SUMS --quiet 2>/dev/null || true)
chmod +x "$BIN/doomxr"
echo "REMA linux rig ready: engine source $SRC -> $BIN"
