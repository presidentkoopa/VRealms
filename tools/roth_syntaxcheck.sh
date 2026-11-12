#!/bin/sh
# Syntax-check src/roth/*.cpp against the engine's own headers with g++,
# without configuring or building the engine. It catches a typo or a wrong
# engine API call in minutes on a machine that cannot build doomxr.
#
#   tools/roth_syntaxcheck.sh              # every file in src/roth
#   tools/roth_syntaxcheck.sh roth_runtime # just one
#
# This is NOT a build: nothing is linked, warnings are off, and MSVC may
# still disagree. Exit code 0 means every file parsed and type-checked.
cd "$(dirname "$0")/.." || exit 2
INC=$(find src -type d -not -path '*msinttypes*' -not -path '*/win32*' \
	-not -path '*thirdparty/rapidjson*' | sed 's/^/-I/' | tr '\n' ' ')
INC="$INC -Isrc/common/thirdparty -Ilibraries/ZMusic/include -Ilibraries/ZVulkan/include"
INC="$INC -Ilibraries/ZWidget/include -Ilibraries/miniz -Ilibraries/bzip2 -Ilibraries/lzma/C"
INC="$INC -Ilibraries/asmjit -Ilibraries/webp/include -Ilibraries/abseil"
DEF="-D__unix__ -Dstricmp=strcasecmp -Dstrnicmp=strncasecmp -D__forceinline=inline"
if [ $# -gt 0 ]; then FILES=""; for n in "$@"; do FILES="$FILES src/roth/$n.cpp"; done
else FILES=$(ls src/roth/*.cpp); fi
bad=0
for f in $FILES; do
	out=$(g++ -std=c++20 -fsyntax-only -w -fpermissive $DEF $INC "$f" 2>&1 | grep -E "error|fatal" | head -5)
	if [ -n "$out" ]; then bad=1; echo "FAIL $f"; echo "$out" | sed 's/^/     /'; else echo "ok   $f"; fi
done
exit $bad
