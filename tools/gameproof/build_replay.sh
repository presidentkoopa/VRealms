#!/bin/sh
# Build the port-side replayer (g++ only). Output: $1 (default /tmp/gameproof/replay).
set -e
HERE=$(cd "$(dirname "$0")" && pwd); S="$HERE/../../src/roth"; OUT=${1:-/tmp/gameproof/replay}
mkdir -p "$(dirname "$OUT")"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -g ${PROOF_SAN:+-fsanitize=address,undefined -fno-sanitize-recover=all} -o "$OUT" "$HERE/replay.cpp" \
    "$S/roth_itemcmds.cpp" "$S/roth_sfx.cpp" "$S/roth_gamecore.cpp" "$S/roth_dbase100_interp.cpp" "$S/roth_inventory.cpp" "$S/roth_dbase100.cpp"
