#!/bin/sh
# Build and run the offline checks for the portal plan (roth_portals). Nothing here needs the
# engine to be configured or built: the planner and the Realms reader are engine-free, and the
# node builder is this tree's own, compiled by itself.
#
#   tools/portalplan/build.sh OUTDIR            build psurvey, nbh2, pchain, seamtest into OUTDIR
#   OUTDIR/psurvey [-d] [doorcuts] [reach=F,F] MAP.RAW...   the plan of each map with overlap
#   OUTDIR/nbh2 MAP.RAW full [portals] [doorcuts]           this tree's node builder on the result
#   OUTDIR/pchain MAP.RAW...                                longest chain of portals on one sight line
#   OUTDIR/seamtest                                         the engine's seam geometry, case by case
cd "$(dirname "$0")/../.." || exit 2
OUT=${1:-/tmp/portalplan}
mkdir -p "$OUT/nb" || exit 2
INC=$(find src -type d -not -path '*msinttypes*' -not -path '*/win32*' \
	-not -path '*thirdparty/rapidjson*' | sed 's/^/-I/' | tr '\n' ' ')
INC="$INC -Isrc/common/thirdparty -Ilibraries/ZMusic/include -Ilibraries/ZVulkan/include"
INC="$INC -Ilibraries/ZWidget/include -Ilibraries/miniz -Ilibraries/bzip2 -Ilibraries/lzma/C"
INC="$INC -Ilibraries/asmjit -Ilibraries/webp/include -Ilibraries/abseil -Itools/portalplan"
DEF="-D__unix__ -Dstricmp=strcasecmp -Dstrnicmp=strncasecmp -D__forceinline=inline"
CXX="g++ -std=c++20 -O1 -w -fpermissive $DEF $INC"
set -e
$CXX -c src/roth/roth_raw.cpp -o "$OUT/roth_raw.o"
for n in nodebuild nodebuild_classify_nosse2 nodebuild_events nodebuild_extract nodebuild_gl nodebuild_utility; do
	$CXX -c src/utility/nodebuilder/$n.cpp -o "$OUT/nb/$n.o"
done
$CXX tools/portalplan/psurvey.cpp "$OUT/roth_raw.o" -o "$OUT/psurvey"
$CXX tools/portalplan/pchain.cpp "$OUT/roth_raw.o" -o "$OUT/pchain"
$CXX tools/portalplan/nbh2.cpp "$OUT"/nb/*.o "$OUT/roth_raw.o" -o "$OUT/nbh2"
python3 tools/portalplan/seamtest.py src/playsim/p_maputl.cpp > "$OUT/seamtest.cpp"
$CXX "$OUT/seamtest.cpp" -o "$OUT/seamtest"
"$OUT/seamtest"
