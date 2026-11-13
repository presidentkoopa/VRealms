#!/bin/sh
# Build the item dossier generator (engine-free, g++ only). Writes into $1 (default /tmp/idw), never beside the sources.
#   tools/item_dossier/build.sh [out-dir]
#   <out>/item_dossier "<ROTH DATA folder>" [--item N | --order]
set -e
HERE=$(cd "$(dirname "$0")" && pwd); SRC="$HERE/../../src/roth"; OUT=${1:-/tmp/idw}
mkdir -p "$OUT"
g++ -std=c++17 -Wall -Wextra -Werror -O1 -I "$SRC" -o "$OUT/item_dossier" "$HERE/item_dossier.cpp" \
	"$SRC/roth_raw.cpp" "$SRC/roth_install.cpp" "$SRC/roth_dbase100.cpp" "$SRC/roth_dbase100_install.cpp" "$SRC/roth_log.cpp"
echo "built $OUT/item_dossier"
