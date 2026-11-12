#!/bin/sh
# THE WHOLE-GAME FLOOR AND CEILING CHECK. For every retail map: build the lines exactly as the
# loader does, run this tree's own node builder, and compare each Realms sector's outline area
# with the area of the subsectors the build gave it -- once as the maps are stored (roth_portals 0)
# and once with the portal plan applied (roth_portals 1, the default). A sector that comes up
# short has a hole in its floor and its ceiling. Also reports any side not exactly covered by
# segs (a missing piece of wall) and any plan bug.
#
#   tools/portalplan/build.sh OUT        first, once
#   tools/portalplan/allmaps.sh OUT "/path/to/Realms of the Haunting/DATA/M"
#
# Prints one line per map and the two totals. Exit code 0 only if the portals-on total is zero.
OUT=${1:-/tmp/portalplan}
DIR=${2:?give the folder that holds the .RAW maps}
toff=0; ton=0; maps=0; sides=0; bugs=0
for f in "$DIR"/*.RAW; do
	name=$(basename "$f" .RAW)
	off=$("$OUT/nbh2" "$f" full)
	on=$("$OUT/nbh2" "$f" full portals)
	noff=$(printf '%s\n' "$off" | sed -n 's/^sectors whose subsector area != polygon area: \([0-9]*\) of.*/\1/p')
	non=$(printf '%s\n' "$on" | sed -n 's/^sectors whose subsector area != polygon area: \([0-9]*\) of.*/\1/p')
	son=$(printf '%s\n' "$on" | sed -n 's/^sides not exactly covered by segs: \([0-9]*\) of.*/\1/p')
	soff=$(printf '%s\n' "$off" | sed -n 's/^sides not exactly covered by segs: \([0-9]*\) of.*/\1/p')
	cuts=$(printf '%s\n' "$on" | grep -c '^PORTAL ')
	bug=$(printf '%s\n' "$on" | grep -c 'PLAN BUG\|^LOADER')
	[ -z "$non" ] && { non=999; bug=$((bug+1)); }
	[ -z "$noff" ] && noff=999
	listoff=$(printf '%s\n' "$off" | sed -n 's/^DEF \([0-9]*\) .*/\1/p' | tr '\n' ' ')
	liston=$(printf '%s\n' "$on" | sed -n 's/^DEF \([0-9]*\) .*/\1/p' | tr '\n' ' ')
	printf '%-9s portals off: %2d short%s | portals on: %2d short%s | cut pairs %2d | sides uncovered off/on %s/%s%s\n' \
		"$name" "$noff" "${listoff:+ ($listoff)}" "$non" "${liston:+ ($liston)}" "$cuts" "${soff:-?}" "${son:-?}" \
		"$([ "$bug" -gt 0 ] && echo '  PLAN BUG')"
	toff=$((toff+noff)); ton=$((ton+non)); maps=$((maps+1)); sides=$((sides+${son:-0}+${soff:-0})); bugs=$((bugs+bug))
done
echo "TOTAL over $maps maps: sectors short of floor/ceiling -- portals off $toff, portals on $ton; sides not covered $sides; plan bugs $bugs"
[ "$ton" -eq 0 ] && [ "$bugs" -eq 0 ]
