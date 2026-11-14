// tools/handclick_test: src/roth/roth_handclick.h (SecondaryClick, what the original's secondary click does on the
// cursor's type -- the click a landed fist makes, roth_gestures.cpp) against the ORIGINAL's own run_gameplay_frame,
// case by case (clickprobe.c's log). The oracle's observation -> a verb: trigger_weapon_fire with the aim flag set =
// ATTACK_AIMED, the use's pick (render_world_view) = USE, neither = NOTHING.
//
// EXCLUDED, NOT COMPARED: the original's STEERING attack (type 1, the primary held 0x0c ticks or more, a secondary
// edge -> trigger_weapon_fire(0, 0) with no aim, E/game_core.c:700-727). Not ported, by the owner's decision of
// 2026-11-13 ("physical only: the grip on empty air no longer punches"; roth_handclick.h checklist 4): the hands
// attack only where a fist lands. Those cases are counted and printed apart. Any other attack without the aim flag
// is a difference.
#include "roth_handclick.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "test_handclick <clickprobe.log>\n"); return 2; }
	FILE *f = fopen(argv[1], "r");
	if (!f) { fprintf(stderr, "no %s\n", argv[1]); return 2; }
	char line[256];
	int n = 0, bad = 0, excluded = 0, seen[3] = {}, end = 0;
	while (fgets(line, sizeof line, f))
	{
		if (!strncmp(line, "END", 3)) { end = 1; continue; }
		int type, held, clock, fts, fire, aimed, use, after;
		if (sscanf(line, "C %d %d %d %d fire=%d aimed=%d use=%d type_after=%d", &type, &held, &clock, &fts, &fire, &aimed, &use, &after) != 8) continue;
		if (fire && !aimed && type == 1 && held)
		{
			excluded++;   // the steering attack: not ported (see the header)
			continue;
		}
		const int orig = fire ? (aimed ? (int)roth::SECONDARY_ATTACK_AIMED : -1)
			: use ? (int)roth::SECONDARY_USE : (int)roth::SECONDARY_NOTHING;
		const int port = (int)roth::SecondaryClick(type);
		n++;
		if (orig >= 0) seen[orig]++;
		if (orig != port)
		{
			bad++;
			printf("DIFFER type %d primary held %d clock %d+%d: original %s, port %s\n", type, held, clock, fts,
				orig >= 0 ? roth::SecondaryVerbName((roth::SecondaryVerb)orig) : "an unaimed attack outside steering",
				roth::SecondaryVerbName((roth::SecondaryVerb)port));
		}
	}
	fclose(f);
	printf("secondary click: %d cases compared, %d differ (original: %d nothing, %d use, %d aimed attack); %d steering-attack"
		" cases excluded (not ported: physical only)%s\n", n, bad, seen[0], seen[1], seen[2], excluded, end ? "" : "; THE PROBE DID NOT FINISH");
	return (bad == 0 && n > 0 && end) ? 0 : 1;
}
