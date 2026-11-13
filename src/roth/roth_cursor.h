#pragma once
//
// THE ORIGINAL'S CURSOR CLASSIFIER, ENGINE-FREE: classify_cursor_target_object
// (ROTH.C E/input.c:1111-1245) as a pure function of what its callees answer.
//
// The callees are read-only matchers (the checklist in roth_runtime.cpp above
// ClassifyPick says which port function answers each). The original calls them
// lazily, on some paths only; being read-only, answering all of them up front
// gives the same result, which is what lets this be a pure function -- and what
// lets tools/vrpick_test compile it beside the ORIGINAL'S OWN TEXT of the
// function, cut out of input.c, and compare them on every combination.
//
// The depth bands are passed in. The original's are 0x258 / 0x7d0 / 0x1388 on
// its renderer's depth metric; the engine passes its provisional use range and
// that range times 2000/600 and 5000/600 (roth_runtime.h).
//

#include <stdint.h>

namespace roth
{

struct CursorFacts
{
	uint8_t recType = 0;        // the pick record's +1: 1 ceiling, 2 floor, 3 face, 4 object, 6 door, 7/8 platform
	uint8_t defFlags = 0;       // an object's def+9 (0x01 keyed, 0x02, 0x10, 0x20 the 0x1b load marker)
	bool faceImpactMarked = false; // a face's texmap +9 & 2 (a 0x1a is bound to it)
	bool impactLink = false;    // object_has_active_trigger_link
	bool clickRecord = false;   // find_unflagged_object_by_key
	bool examineMatch = false;  // dispatch_entry_command_trigger_b != 0
	bool onInspect = false;     // find_oninspect_block_by_id != 0
	bool useMatch = false;      // dispatch_entry_command_trigger != 0
	bool pickable = false;      // is_item_id_pickable
	double depth = 0;           // +0x20
	double useR = 600, farR = 2000, probeR = 5000;
};

struct CursorResult
{
	// 0x7e932. run_gameplay_frame zeroes it before the pick (game_core.c
	// L169f3), so a path of the classifier that does not write it leaves 0,
	// which no click acts on.
	uint8_t  type = 0;
	uint16_t shape = 0x240;     // the return value: set_cursor_shape's id
	uint8_t  flags = 0;         // 0x7fdac
	int      count = 0;         // 0x7fdb0
	int      outOfRange = 0;    // 0x7fdb4
};

inline CursorResult ClassifyCursorFacts(const CursorFacts &f)
{
	CursorResult c;
	const double d = f.depth;
	auto L16375 = [&]() { c.shape = c.count ? 0x250 : 0x248; return c; };
	auto L164ba = [&]() { c.type = 1; c.shape = 0x240; return c; };
	auto L164a2 = [&]() { if (c.flags & 4) { c.type = 5; c.shape = 0x258; return c; } return L164ba(); };
	auto L16475 = [&]() {
		if (c.count == 0) return L164a2();
		if (c.flags & 4) { c.type = 5; c.shape = 0x260; return c; }
		c.type = 3; c.shape = 0x278; return c;
	};
	auto L162b3 = [&]() { c.type = 5; c.flags |= 4; c.shape = 0x258; return c; };

	if (f.recType == 4)                                           // IN:1119-1133
	{
		if (f.defFlags & 1)
		{
			if (d >= f.useR) return L162b3();
			if (!f.clickRecord) return L162b3();
			c.type = 3;
			return L16375();
		}
		if ((f.defFlags & 0x20) && f.impactLink) c.flags |= 4;
	}
	else if (f.recType == 3)                                      // IN:1134-1143
	{
		if (f.faceImpactMarked && f.impactLink) c.flags |= 4;
	}

	if (d >= f.probeR) return L164a2();                           // IN:1145
	if (f.examineMatch) { c.flags |= 2; c.count++; }              // IN:1146-1151
	if (c.count == 0 && f.recType == 4 && (f.defFlags & 0x13) == 0 && f.onInspect) c.count++;   // IN:1152-1161

	if (f.recType == 6)                                           // IN:1162-1175
	{
		if (d < f.useR) { c.type = 3; return L16375(); }
		c.outOfRange = 1;
		if (d < f.farR)
		{
			if (c.count) { c.shape = 0x3a0; return c; }
			c.type = 1; c.shape = 0x398; return c;
		}
		if (c.count == 0) return L164ba();
		c.shape = 0x278; return c;
	}

	if (d < f.useR)                                               // IN:1177-1197
	{
		if (f.useMatch) { c.type = 3; return L16375(); }
	}
	else if (!(c.flags & 4) && d < f.farR)
	{
		if (f.useMatch) { c.type = 3; c.shape = c.count ? 0x3a0 : 0x398; return c; }
	}

	if (f.recType == 4 && (f.defFlags & 0x13) == 0)              // IN:1198-1222
	{
		c.type = 3;                                               // set early: sticks on bail
		if (!f.clickRecord && !f.pickable) return L16475();
		if (d < f.useR) return L16375();
		c.outOfRange = 1;
		if (c.flags & 4) return L16475();
		if (d < f.farR) { c.shape = c.count ? 0x3a0 : 0x398; return c; }
	}
	return L16475();
}

// run_gameplay_frame's click gates on a classification (E/game_core.c:604-699):
// the primary button examines a type 3 or 5 with something to examine (a type
// 5 with nothing is made type 1, L16a8e; a type 3 with nothing is made type 1,
// L16b84); the secondary uses a type 3 (made type 4, L16ab5), and
// activate_targeted_object refuses past the use range itself (IN:1031).
inline bool CursorAllowsExamine(const CursorResult &c) { return (c.type == 3 || c.type == 5) && c.count != 0; }
inline bool CursorAllowsUse(const CursorResult &c, double depth, double useR) { return c.type == 3 && depth < useR; }

} // namespace roth
