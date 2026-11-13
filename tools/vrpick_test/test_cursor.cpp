// tools/vrpick_test: src/roth/roth_cursor.h (the port of classify_cursor_target_object)
// against THE ORIGINAL'S OWN TEXT of the function (cut out of ROTH.C's input.c by run.sh,
// compiled unchanged against stub callees), on every combination of: the pick record's type,
// the object's def+9 bits, the face's 0x1a marker, each callee's answer, and a depth on each
// side of each band. Every output the function has is compared: the cursor type, the shape it
// returns, the flag byte, the examine count, the out-of-range latch.
#include "roth_cursor.h"
#include <stdio.h>

extern "C" {
extern int s_click, s_link, s_exb, s_use, s_insp, s_pick;
void orig_classify(int recType, int defFlags, int faceMarked, int depth,
	int *type, int *shape, int *flags, int *count, int *oor);
}

int main()
{
	const int types[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	const int depths[] = { 0, 300, 599, 600, 601, 1999, 2000, 2001, 4999, 5000, 5001, 9000 };
	long cases = 0, bad = 0;
	static int shapesSeen[0x400] = {};
	for (int t : types)
	for (int fl = 0; fl < 64; fl++)                         // def+9 bits 0x01 0x02 0x04 0x08 0x10 0x20
	for (int bits = 0; bits < 128; bits++)                  // marker, link, click, examine, inspect, use, pickable
	for (int d : depths)
	{
		const int marked = bits & 1;
		s_link = (bits >> 1) & 1; s_click = (bits >> 2) & 1; s_exb = (bits >> 3) & 1;
		s_insp = (bits >> 4) & 1; s_use = (bits >> 5) & 1; s_pick = (bits >> 6) & 1;
		int ot, os, of, oc, oo;
		orig_classify(t, fl, marked, d, &ot, &os, &of, &oc, &oo);

		roth::CursorFacts f;
		f.recType = (uint8_t)t; f.defFlags = (uint8_t)fl; f.faceImpactMarked = marked;
		f.impactLink = s_link; f.clickRecord = s_click; f.examineMatch = s_exb; f.onInspect = s_insp;
		f.useMatch = s_use; f.pickable = s_pick; f.depth = d;
		const roth::CursorResult c = roth::ClassifyCursorFacts(f);
		cases++;
		if (os >= 0 && os < 0x400) shapesSeen[os] = 1;
		if (c.type != ot || c.shape != os || c.flags != of || c.count != oc || c.outOfRange != oo)
		{
			if (bad < 12)
				printf("DIFF type %d def+9 0x%02x bits 0x%02x depth %d: original type %d shape 0x%x flags 0x%x count %d oor %d;"
					" port type %d shape 0x%x flags 0x%x count %d oor %d\n", t, fl, bits, d, ot, os, of, oc, oo,
					c.type, c.shape, c.flags, c.count, c.outOfRange);
			bad++;
		}
	}
	printf("classify_cursor_target_object: %ld cases, %ld differ; shapes reached:", cases, bad);
	for (int i = 0; i < 0x400; i++) if (shapesSeen[i]) printf(" 0x%x", i);
	printf("\n");
	return bad == 0 ? 0 : 1;
}
