#!/bin/sh
# Build and run gunaim_test (ActorWorld::HandOriginSector on the retail maps).
#   tools/gunaim_test/build.sh [DATA dir] [MAP ...]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../../src/roth"
OUT=${GUNAIM_OUT:-/tmp/gunaim_test}   # outside the repo
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I"$SRC" \
	-o "$OUT/gunaim_test" "$HERE/gunaim_test.cpp" "$SRC"/roth_actors.cpp "$SRC"/roth_actor_*.cpp \
	"$SRC/roth_raw.cpp" "$SRC/roth_dbase100.cpp" "$SRC/roth_das.cpp" "$SRC/roth_palette.cpp"
DATA=${1:-${ROTH_DATA:-"/mnt/user-data/uploads/Realms of the Haunting/DATA"}}
[ $# -gt 0 ] && shift
"$OUT/gunaim_test" "$DATA" "$@"
