#!/bin/bash
# After `setup.sh build`: record the new engine in the repo so the next session uses it instead
# of compiling. bin/ (doomxr + pk3s + SOURCE + SHA256SUMS) is committed on the current branch,
# CURRENT is set to the engine-source commit. ~42 MB per published engine; publish only when the
# engine source changed.
set -eo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../.." && pwd); BIN=$HERE/bin
SRC=$(cat "$BIN/SOURCE")
(cd "$BIN" && sha256sum doomxr *.pk3 > SHA256SUMS)
echo "$SRC" > "$HERE/CURRENT"
cd "$REPO"
git add -f tools/linux_rig/bin tools/linux_rig/CURRENT tools/linux_rig/openxr/include
git commit -q -m "linux_rig: prebuilt engine for source $SRC" -- tools/linux_rig/bin tools/linux_rig/CURRENT tools/linux_rig/openxr/include
git push origin HEAD
echo "published: bin/ for $SRC is in the repo"
