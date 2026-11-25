#!/bin/sh
# Engine-free probes of the retail .RAW / .DAS files (plain g++ and src/roth's readers). Output: ${1:-/tmp/rothprobe}/<name>.
#   dump   <RAW...>                  every map's 0x25 records (category 8)
#   chain  <RAW> <rec>...            a chain from a record, and which records name picture 762 (edit the number)
#   who    <RAW> <rec>               the records whose +4 link is <rec> (walk a chain backwards)
#   where  <RAW> <picture>           mappings, faces, sectors and objects wearing a picture
#   sec    <RAW> <sector>...         a sector's centre, box, floor and ceiling
#   pt     <RAW> <x> <y>             the sectors holding a point (even-odd test)
#   faces  <RAW> <rec>...            a record's faces and their ends       (faces2: a sector's faces, one-sided marked)
#   tm     <RAW> <face>...           a face's mapping: mid/upper/lower pictures, flags, faceID
#   frames <DAS> <outdir> <pic>...   a picture's frames as .ppm, with its rate and kind
#   anim   <RAW> <DAS>               animated wall pictures within 1500 of the player start
#   sky    <RAW>...                  the metadata's sky picture
# findpose.py <MAP> <sector> [yShift]: oracle frames 200 and 260 from twelve spots around a moving sector, ranked by
# how much changes between them (needs the Linux oracle build; ORACLE_DIR, ROTH_DATA, ROTHPROBE_BIN).
cd "$(dirname "$0")/../.." || exit 2
OUT="${1:-/tmp/rothprobe}"; mkdir -p "$OUT"
for n in dump chain who where sec pt tm faces2 sky; do
	g++ -std=c++17 -O1 -Isrc/roth -o "$OUT/$n" tools/rothprobe/$n.cpp src/roth/roth_raw.cpp || exit 1
done
for n in frames anim; do
	g++ -std=c++17 -O1 -Isrc/roth -o "$OUT/$n" tools/rothprobe/$n.cpp src/roth/roth_raw.cpp src/roth/roth_das.cpp src/roth/roth_palette.cpp 2>&1 | grep -v warning
done
echo "built in $OUT"
