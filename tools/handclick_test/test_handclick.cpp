// tools/handclick_test: src/roth/roth_handclick.h (SecondaryClick, what the original's secondary
// click does on the cursor's type) against the ORIGINAL's own run_gameplay_frame, case by case
// (clickprobe.c's log). The oracle's observation -> a verb: trigger_weapon_fire with the aim flag set
// = ATTACK_AIMED, without = ATTACK; the use's pick (render_world_view) = USE; neither = NOTHING.
// The steer clock the port is given is the original's after this frame's ticks (:703-708).
#include "roth_handclick.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "test_handclick <clickprobe.log>\n"); return 2; }
	FILE *f = fopen(argv[1], "r");
	if (!f) { fprintf(stderr, "no %s\n", argv[1]); return 2; }
	char line[256];
	int n = 0, bad = 0, seen[4] = {}, end = 0;
	while (fgets(line, sizeof line, f))
	{
		if (!strncmp(line, "END", 3)) { end = 1; continue; }
		int type, held, clock, fts, fire, aimed, use, after;
		if (sscanf(line, "C %d %d %d %d fire=%d aimed=%d use=%d type_after=%d", &type, &held, &clock, &fts, &fire, &aimed, &use, &after) != 8) continue;
		const roth::SecondaryVerb orig = fire ? (aimed ? roth::SECONDARY_ATTACK_AIMED : roth::SECONDARY_ATTACK)
			: use ? roth::SECONDARY_USE : roth::SECONDARY_NOTHING;
		const roth::SecondaryVerb port = roth::SecondaryClick(type, held != 0, clock + fts);
		n++;
		seen[orig]++;
		if (orig != port)
		{
			bad++;
			printf("DIFFER type %d primary held %d clock %d+%d: original %s, port %s\n", type, held, clock, fts,
				roth::SecondaryVerbName(orig), roth::SecondaryVerbName(port));
		}
	}
	fclose(f);
	printf("secondary click: %d cases, %d differ (original: %d nothing, %d use, %d aimed attack, %d steering attack)%s\n",
		n, bad, seen[0], seen[1], seen[2], seen[3], end ? "" : "; THE PROBE DID NOT FINISH");
	return (bad == 0 && n > 0 && end) ? 0 : 1;
}
