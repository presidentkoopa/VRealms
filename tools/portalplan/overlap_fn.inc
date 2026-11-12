//==========================================================================
//
// PLAN OVERLAP -- rooms stacked over rooms.
//
// Realms draws by walking from the camera's sector through faces into the
// neighbour each one names (ROTH.C renderer.c:7113 walk_visible_sectors,
// "Mode 0: portal walk"; :7319 build_sector_render_tree_recursive, which takes
// the neighbour straight from the face's sister at +8). Nothing in that walk
// asks where a sector lies in plan, so two sectors may occupy the same x,y at
// different heights and the retail maps do exactly that: STUDY1's upper
// corridor (sectors 368/404/405, z 400..720) lies directly over the ground-floor
// hall (358/359/360/380, z 60..380/460).
//
// A Doom BSP gives every point of the plan to exactly ONE subsector, and so to
// one sector. Where two sectors overlap, one of them cannot have its floor and
// ceiling there at all, whatever the node builder does. MEASURED, by linking
// this tree's own FNodeBuilder standalone against the loader's line
// construction for all 44 retail maps (2026-11-12): 54 sectors come out of the
// build with less subsector area than their outline; ALL 54 are plan-overlapped,
// none of the 547 sectors thinner than 4 units is among them, and the 33 maps
// with no overlap lose nothing. That is the "black wedges in the floor and
// ceiling" of RENDERING_NEXT_STEP.md -- not the 2-unit sectors 56/57, which
// each build into one exact 4-seg subsector; leaving them out only reshuffles
// which splitters are picked and so where the holes fall.
//
// This function only FINDS the overlaps so the load report can name them. It
// does not repair anything: that needs the storeys moved apart and joined by
// line portals, or folded into 3D floors, and is not attempted here.
//
// Sampled, not exact: each sector is probed on a lattice (4 units, finer for a
// narrow one) at points that can never lie on an integer edge, with an even-odd
// test against its own outline and then against every other sector whose
// bounding box it touches. A hit is a real point inside both outlines; the area
// is hits times the lattice cell, so treat it as an estimate.
//
//==========================================================================
namespace
{

struct RothPlanOverlap
{
	int a, b;        // Realms sector indices, a < b
	double area;     // estimated shared plan area, square map units
};

struct RothOutline
{
	std::vector<double> x1, y1, x2, y2;
	double minx = 0., miny = 0., maxx = 0., maxy = 0.;
	bool usable = false;
};

bool RothOutlineHolds(const RothOutline &o, double x, double y)
{
	if (x < o.minx || x > o.maxx || y < o.miny || y > o.maxy) return false;
	bool in = false;
	for (size_t i = 0; i < o.x1.size(); i++)
	{
		if ((o.y1[i] > y) != (o.y2[i] > y))
		{
			const double xi = o.x1[i] + (y - o.y1[i]) * (o.x2[i] - o.x1[i]) / (o.y2[i] - o.y1[i]);
			if (xi > x) in = !in;
		}
	}
	return in;
}

void RothFindPlanOverlaps(const roth::Map &rm, const std::vector<int> &skipSectors,
	std::vector<RothPlanOverlap> &out)
{
	const int ns = (int)rm.sectors.size();
	std::vector<RothOutline> outline((size_t)ns);
	for (int s = 0; s < ns; s++)
	{
		bool skipped = false;
		for (int sx : skipSectors) if (sx == s) skipped = true;
		const roth::Sector &rs = rm.sectors[s];
		if (skipped || rs.faceCount < 3 || rs.firstFaceIndex < 0
			|| rs.firstFaceIndex + rs.faceCount > (int)rm.faces.size()) continue;
		RothOutline &o = outline[s];
		bool first = true, bad = false;
		for (int k = 0; k < rs.faceCount; k++)
		{
			const roth::Face &f = rm.faces[rs.firstFaceIndex + k];
			if (f.vertex1 < 0 || f.vertex2 < 0 || f.vertex1 >= (int)rm.vertices.size()
				|| f.vertex2 >= (int)rm.vertices.size()) { bad = true; break; }
			const double ax = rm.vertices[f.vertex1].x, ay = rm.vertices[f.vertex1].y;
			const double bx = rm.vertices[f.vertex2].x, by = rm.vertices[f.vertex2].y;
			o.x1.push_back(ax); o.y1.push_back(ay); o.x2.push_back(bx); o.y2.push_back(by);
			if (first) { o.minx = o.maxx = ax; o.miny = o.maxy = ay; first = false; }
			o.minx = ax < o.minx ? ax : o.minx;  o.maxx = ax > o.maxx ? ax : o.maxx;
			o.minx = bx < o.minx ? bx : o.minx;  o.maxx = bx > o.maxx ? bx : o.maxx;
			o.miny = ay < o.miny ? ay : o.miny;  o.maxy = ay > o.maxy ? ay : o.maxy;
			o.miny = by < o.miny ? by : o.miny;  o.maxy = by > o.maxy ? by : o.maxy;
		}
		o.usable = !bad && o.maxx > o.minx && o.maxy > o.miny;
	}

	// Each member of a pair probes it on its own lattice; first = the estimate
	// from the lower-numbered sector's probes, second = from the higher one's.
	std::map<std::pair<int, int>, std::pair<double, double>> found;
	std::vector<int> nearby;
	for (int s = 0; s < ns; s++)
	{
		const RothOutline &o = outline[s];
		if (!o.usable) continue;
		nearby.clear();
		for (int t = 0; t < ns; t++)
		{
			const RothOutline &p = outline[t];
			if (t == s || !p.usable) continue;
			if (p.maxx <= o.minx || p.minx >= o.maxx || p.maxy <= o.miny || p.miny >= o.maxy) continue;
			nearby.push_back(t);
		}
		if (nearby.empty()) continue;

		// 4 units, or a third of the narrow side for a sliver, never under 1.
		double step = 4.;
		const double narrow = (o.maxx - o.minx) < (o.maxy - o.miny) ? (o.maxx - o.minx) : (o.maxy - o.miny);
		if (narrow / 3. < step) step = narrow / 3.;
		if (step < 1.) step = 1.;
		// Offsets that are not multiples of anything a vertex can be, so no
		// probe sits on an edge and the even-odd test is never ambiguous.
		for (double y = o.miny + step * 0.4371; y < o.maxy; y += step)
		{
			for (double x = o.minx + step * 0.3913; x < o.maxx; x += step)
			{
				if (!RothOutlineHolds(o, x, y)) continue;
				for (int t : nearby)
				{
					if (!RothOutlineHolds(outline[t], x, y)) continue;
					std::pair<double, double> &est =
						found[std::pair<int, int>(s < t ? s : t, s < t ? t : s)];
					(s < t ? est.first : est.second) += step * step;
				}
			}
		}
	}
	for (const auto &kv : found)
	{
		RothPlanOverlap po;
		po.a = kv.first.first;
		po.b = kv.first.second;
		po.area = kv.second.first > kv.second.second ? kv.second.first : kv.second.second;
		out.push_back(po);
	}
}

} // namespace
