/*
** roth_portalplan.h
**
** Rooms over rooms, planned: which Realms sectors have to be moved to their
** own region of the map so that no two sectors of one region share plan area,
** which faces are cut to do it, and the moved copies of the vertices the
** loader must emit so the cut faces can be joined again by linked line
** portals.
**
** ENGINE-FREE ON PURPOSE. It reads roth::Map and nothing else, so the offline
** harness (the standalone FNodeBuilder check) and the level loader run the
** SAME planner and cannot disagree about what was built. All of it is fresh
** code for this project; nothing in THIS FILE is ported from another engine.
** (What lets a cut face be a portal with nothing behind it is in the engine:
** a linked portal on a one-sided line, adapted from the Eternity Engine --
** see line_t::getPortalBeyondSector in r_defs.h.)
**
** What it decides, in order:
**
**   1. LAYERS. Every pair of sectors that overlaps in plan must end up in
**      different regions ("groups"). The overlap graph is 2-coloured where it
**      is bipartite (lower room = 0), greedily coloured by floor height where
**      it is not (three storeys).
**
**   2. THE CUT. The coloured sectors are seeds; every other sector is free and
**      goes wherever it makes the cut cheapest. For each layer above the
**      ground that is a minimum s-t cut of the sector adjacency graph (Dinic),
**      with a face's cost = one portal pair, plus penalties that keep the cut
**      off doors, door neighbours, mid-platform sectors, trigger faces and
**      very short faces. The sink side is taken as small as the minimum cut
**      allows, so the unmoved group stays as large as possible.
**
**      A CUT FACE IS A ONE-SIDED LINE. Each half of a cut pair is emitted as a
**      line with a front side only; the linked portal on it stands in for the
**      sector that used to be behind it. There are NO buffer sectors: the
**      engine takes the opening, the floor to stand on and the wall's upper
**      and lower pieces from the sector really on the far side, live -- so a
**      floor or a ceiling that moves next to a seam simply works, and a cut
**      may be made where there is no room at all behind the face (the usual
**      case: the room of the other storey is exactly there).
**
**   3. DOOR LEAVES. A swinging door is a polyobject, and a polyobject exists
**      in ONE region of the map: the part of a leaf that swings through a
**      portal line is neither drawn nor solid on the far side. So a face a
**      leaf can reach is not cut (refuseDoorReach), and a stack that cannot
**      be separated without such a face is left as it was and reported.
**
**   4. THE TRANSLATION. Group 0 stays. Every other group moves clear of
**      everything placed before it, by a whole number of map units (the
**      engine compares portal displacements for exact equality).
**
**---------------------------------------------------------------------------
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/
#pragma once

#include <math.h>
#include <stdint.h>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "roth/roth_raw.h"

namespace rothplan
{

struct OverlapPair { int a, b; double area; };

struct Cut
{
	int face = -1, sister = -1;     // the loader's pending pair: face is the primary
	int groupFace = 0, groupSister = 0;
	int cost = 0;                   // what the cut paid for this face (10 = clean)
	int doorSector = -1;            // a door leaf of this sector can reach the face (only if allowed)
};

struct ExtraVertex { double x, y; };

struct Plan
{
	bool active = false;            // false: nothing to do, or it could not be done -- see notes
	int numGroups = 1;
	std::vector<int> group;         // per Realms sector
	std::vector<double> offX, offY; // per group
	std::vector<Cut> cuts;
	std::vector<ExtraVertex> extraVerts;    // moved copies of vertices, already translated
	std::map<std::pair<int, int>, int> movedVertex;   // (group, Realms vertex) -> index into extraVerts
	std::map<int, int> cutByFace;   // face OR sister -> index into cuts
	std::vector<std::string> notes; // everything worth a line in the load report
	int ignoredSlivers = 0;         // overlap pairs below the area threshold
	int penalisedCuts = 0;          // cuts that had to pay a penalty (hazards)
	int unresolvedStacks = 0;       // overlap stacks that could not be separated
	int doorReachCuts = 0;          // cuts a door leaf can reach (only when that is allowed)
	int extraBase = 0;              // first engine vertex index of extraVerts

	// The engine vertex a face of a sector in `g` must use for Realms vertex `v`.
	int VertexFor(int g, int v) const
	{
		if (g <= 0) return v;
		auto it = movedVertex.find(std::make_pair(g, v));
		return it == movedVertex.end() ? v : extraBase + it->second;
	}
	int GroupOf(int sector) const
	{
		return (sector >= 0 && sector < (int)group.size()) ? group[sector] : 0;
	}
};

namespace detail
{

struct Outline
{
	std::vector<double> x1, y1, x2, y2;
	double minx = 0., miny = 0., maxx = 0., maxy = 0.;
	bool usable = false;
};

inline bool Holds(const Outline &o, double x, double y)
{
	if (!o.usable || x < o.minx || x > o.maxx || y < o.miny || y > o.maxy) return false;
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

// Dinic's maximum flow on a small undirected graph.
struct Flow
{
	struct E { int to; long long cap; };
	std::vector<E> e;
	std::vector<std::vector<int>> adj;
	std::vector<int> level, it;
	explicit Flow(int n) : adj((size_t)n), level((size_t)n), it((size_t)n) {}
	void Add(int a, int b, long long c, bool undirected)
	{
		adj[a].push_back((int)e.size()); e.push_back({ b, c });
		adj[b].push_back((int)e.size()); e.push_back({ a, undirected ? c : 0 });
	}
	bool Bfs(int s, int t)
	{
		std::fill(level.begin(), level.end(), -1);
		std::vector<int> q; q.push_back(s); level[s] = 0;
		for (size_t h = 0; h < q.size(); h++)
			for (int id : adj[q[h]])
				if (e[id].cap > 0 && level[e[id].to] < 0)
				{
					level[e[id].to] = level[q[h]] + 1;
					q.push_back(e[id].to);
				}
		return level[t] >= 0;
	}
	long long Dfs(int v, int t, long long f)
	{
		if (v == t) return f;
		for (int &i = it[v]; i < (int)adj[v].size(); i++)
		{
			const int id = adj[v][i];
			if (e[id].cap > 0 && level[e[id].to] == level[v] + 1)
			{
				const long long d = Dfs(e[id].to, t, f < e[id].cap ? f : e[id].cap);
				if (d > 0) { e[id].cap -= d; e[id ^ 1].cap += d; return d; }
			}
		}
		return 0;
	}
	long long Run(int s, int t)
	{
		long long total = 0;
		while (Bfs(s, t))
		{
			std::fill(it.begin(), it.end(), 0);
			long long f;
			while ((f = Dfs(s, t, (long long)4e18)) > 0) total += f;
		}
		return total;
	}
	// Nodes that can still reach t in the residual graph: the smallest sink side.
	void SinkSide(int t, std::vector<char> &out)
	{
		out.assign(adj.size(), 0);
		std::vector<int> q; q.push_back(t); out[t] = 1;
		for (size_t h = 0; h < q.size(); h++)
			for (int id : adj[q[h]])
			{
				// residual capacity INTO q[h] from e[id].to is on the reverse edge
				const int from = e[id].to;
				if (!out[from] && e[id ^ 1].cap > 0) { out[from] = 1; q.push_back(from); }
			}
	}
};

struct Pair { int face, sister, sa, sb; int cost; };

// The same test rothmap.cpp's leafBuildable makes: a door sector that gets a
// swinging polyobject leaf. Keep the two in step.
inline bool LeafBuildable(const roth::Map &rm, const roth::Sector &rs)
{
	if (!rs.IsDoorCapable() || rs.faceCount != 4 || rs.hingeFace < 0) return false;
	if (rs.firstFaceIndex < 0 || rs.firstFaceIndex + 3 >= (int)rm.faces.size()) return false;
	const int hingeRel = rs.hingeFace - rs.firstFaceIndex;
	if (hingeRel < 0 || hingeRel > 3) return false;
	for (int j = 0; j < 4; j++)
	{
		const roth::Face &a = rm.faces[rs.firstFaceIndex + j];
		const roth::Face &b = rm.faces[rs.firstFaceIndex + ((j + 1) & 3)];
		if (a.vertex1 < 0 || a.vertex1 >= (int)rm.vertices.size()) return false;
		if (a.vertex2 < 0 || a.vertex2 >= (int)rm.vertices.size()) return false;
		if (a.vertex2 != b.vertex1) return false;
	}
	return true;
}

inline double PointSegment(double px, double py, double ax, double ay, double bx, double by)
{
	const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
	double t = l2 > 0. ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.;
	t = t < 0. ? 0. : t > 1. ? 1. : t;
	const double qx = ax + dx * t - px, qy = ay + dy * t - py;
	return sqrt(qx * qx + qy * qy);
}

// Does the leaf of door sector `ds`, at any angle of its quarter turn about
// (hx,hy), properly cross face `f`? Sampled every 1.5 degrees; "properly"
// means an edge of the turned quad crosses the face at an interior point of
// both, or an end of the face lies strictly inside the quad.
inline bool LeafCrosses(const roth::Map &rm, const roth::Sector &ds, double hx, double hy,
	const roth::Face &f)
{
	const double ax = rm.vertices[f.vertex1].x, ay = rm.vertices[f.vertex1].y;
	const double bx = rm.vertices[f.vertex2].x, by = rm.vertices[f.vertex2].y;
	double qx[4], qy[4];
	for (int k = 0; k < 4; k++)
	{
		const roth::Face &c = rm.faces[ds.firstFaceIndex + k];
		qx[k] = rm.vertices[c.vertex1].x - hx;
		qy[k] = rm.vertices[c.vertex1].y - hy;
	}
	const double EPS = 1e-6;
	auto side = [](double px, double py, double qx_, double qy_, double rx, double ry)
	{
		return (qx_ - px) * (ry - py) - (qy_ - py) * (rx - px);
	};
	for (int step = 0; step <= 60; step++)
	{
		const double th = (step * 1.5) * 3.14159265358979323846 / 180.;
		const double cs = cos(th), sn = sin(th);
		double px[4], py[4];
		for (int k = 0; k < 4; k++)
		{
			px[k] = hx + qx[k] * cs - qy[k] * sn;
			py[k] = hy + qx[k] * sn + qy[k] * cs;
		}
		for (int k = 0; k < 4; k++)
		{
			const int n = (k + 1) & 3;
			const double d1 = side(ax, ay, bx, by, px[k], py[k]);
			const double d2 = side(ax, ay, bx, by, px[n], py[n]);
			const double d3 = side(px[k], py[k], px[n], py[n], ax, ay);
			const double d4 = side(px[k], py[k], px[n], py[n], bx, by);
			if (((d1 > EPS && d2 < -EPS) || (d1 < -EPS && d2 > EPS))
				&& ((d3 > EPS && d4 < -EPS) || (d3 < -EPS && d4 > EPS))) return true;
		}
		const double ex[2] = { ax, bx }, ey[2] = { ay, by };
		for (int e = 0; e < 2; e++)
		{
			int pos = 0, neg = 0;
			for (int k = 0; k < 4; k++)
			{
				const double dd = side(px[k], py[k], px[(k + 1) & 3], py[(k + 1) & 3], ex[e], ey[e]);
				if (dd > 0.01) pos++; else if (dd < -0.01) neg++;
			}
			if (pos == 4 || neg == 4) return true;
		}
	}
	return false;
}

// DOOR REACH. A leaf turns about its hinge (the first vertex of the door
// sector's hinge face, as the loader builds it), so it can be anywhere in the
// disc whose radius is the farthest corner of the door sector. A face (between
// sectors sa and sb) that enters that disc, at a height the door stands in, is
// one the leaf can cross; that deliberately takes in the door sector's own
// four faces. Returns the door sector, or -1.
inline int DoorReaching(const roth::Map &rm, int face, int sa, int sb)
{
	const int ns = (int)rm.sectors.size(), nv = (int)rm.vertices.size();
	if (face < 0 || face >= (int)rm.faces.size() || sa < 0 || sb < 0 || sa >= ns || sb >= ns) return -1;
	const roth::Face &f = rm.faces[face];
	if (f.vertex1 < 0 || f.vertex2 < 0 || f.vertex1 >= nv || f.vertex2 >= nv) return -1;
	const roth::Sector &A = rm.sectors[sa], &B = rm.sectors[sb];
	// the opening the pair shares
	const int lo = A.floorHeight > B.floorHeight ? A.floorHeight : B.floorHeight;
	const int hi = A.ceilingHeight < B.ceilingHeight ? A.ceilingHeight : B.ceilingHeight;
	for (int d = 0; d < ns; d++)
	{
		const roth::Sector &ds = rm.sectors[d];
		if (!LeafBuildable(rm, ds)) continue;
		if (sa != d && sb != d && (hi <= ds.floorHeight || lo >= ds.ceilingHeight)) continue;
		const roth::Face &hf = rm.faces[ds.hingeFace];
		const double hx = rm.vertices[hf.vertex1].x, hy = rm.vertices[hf.vertex1].y;
		double reach = 0.;
		for (int k = 0; k < 4; k++)
		{
			const roth::Face &c = rm.faces[ds.firstFaceIndex + k];
			const double r = sqrt((rm.vertices[c.vertex1].x - hx) * (rm.vertices[c.vertex1].x - hx)
				+ (rm.vertices[c.vertex1].y - hy) * (rm.vertices[c.vertex1].y - hy));
			if (r > reach) reach = r;
		}
		if (PointSegment(hx, hy, rm.vertices[f.vertex1].x, rm.vertices[f.vertex1].y,
			rm.vertices[f.vertex2].x, rm.vertices[f.vertex2].y) > reach + 1.) continue;
		// The disc is only the quick test. What decides is the real sweep: the
		// leaf is the door sector's quad, turned about the hinge through the
		// quarter turn the port gives it (roth_runtime.cpp SwingDoor:
		// EV_OpenPolyDoor, +90 degrees, about vertex1 of the hinge face). A
		// face is reached only if some position of the leaf properly crosses
		// it; lying along it, or sweeping past in front of it, is not crossing
		// it. CHURCH1 is the map that needs the difference: the niche of a
		// wall cupboard (sector 558) pokes four units through the wall into
		// the nave (sectors 348/359), and the only clean way to separate them
		// is to set the two nave sectors aside. One of the three faces that
		// takes (1779) ends at the hinge of the door of sector 586, inside
		// its disc -- but that leaf turns away from it, into the next room.
		// IF THE PORT EVER SWINGS A LEAF THE OTHER WAY (the original picks the
		// sign per use, doors.c:527-533), this must sweep both quarters.
		if (LeafCrosses(rm, ds, hx, hy, f)) return d;
	}
	return -1;
}

} // namespace detail

//==========================================================================
//
// BuildPlan
//
// extraBase: the engine vertex index the plan's extra vertices will start at.
// minArea:   overlap pairs smaller than this many square units are left alone
//            (and counted). 0, the default and what the loader uses: none are.
// refuseDoorReach: true -- a face a door leaf can swing across is never cut,
//            and a stack that needs one is left as it is. false -- such a
//            face only pays the door penalties, and a cut that lands on one
//            is reported (Cut::doorSector); the leaf will end at the seam.
//
//==========================================================================

inline Plan BuildPlan(const roth::Map &rm, const std::vector<OverlapPair> &overlaps,
	const std::vector<int> &skipSectors, int extraBase, double minArea = 0.,
	bool refuseDoorReach = true)
{
	using namespace detail;
	Plan plan;
	plan.extraBase = extraBase;
	const int ns = (int)rm.sectors.size();
	const int nf = (int)rm.faces.size();
	const int nv = (int)rm.vertices.size();
	plan.group.assign((size_t)ns, 0);
	plan.offX.assign(1, 0.); plan.offY.assign(1, 0.);

	auto skipped = [&](int s) { for (int x : skipSectors) if (x == s) return true; return false; };
	auto say = [&](const std::string &s) { plan.notes.push_back(s); };
	auto fmt = [](const char *f, double a = 0, double b = 0, double c = 0, double d = 0, double e = 0, double g = 0)
	{
		char buf[320];
		snprintf(buf, sizeof(buf), f, a, b, c, d, e, g);
		return std::string(buf);
	};

	//------------------------------------------------------------------
	// Outlines.
	//------------------------------------------------------------------
	std::vector<Outline> outline((size_t)ns);
	for (int s = 0; s < ns; s++)
	{
		const roth::Sector &rs = rm.sectors[s];
		if (skipped(s) || rs.faceCount < 3 || rs.firstFaceIndex < 0
			|| rs.firstFaceIndex + rs.faceCount > nf) continue;
		Outline &o = outline[s];
		bool first = true, bad = false;
		for (int k = 0; k < rs.faceCount; k++)
		{
			const roth::Face &f = rm.faces[rs.firstFaceIndex + k];
			if (f.vertex1 < 0 || f.vertex2 < 0 || f.vertex1 >= nv || f.vertex2 >= nv) { bad = true; break; }
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

	//------------------------------------------------------------------
	// 1. Layers.
	//------------------------------------------------------------------
	// EVERY overlap is separated, however small. An earlier version left a
	// "sliver" alone (a pair sharing a few units, or a small bite out of both
	// sectors) on the grounds that a portal could not stand in a seam a few
	// units deep. That was true of the buffer-room design; a cut face is now a
	// one-sided line with nothing built behind it, so depth does not come into
	// it. And the slivers were not harmless: the node builder, forced to split
	// wherever two sectors share plan, cost CHURCH1 sectors 348, 359 and 558
	// and SALVAT sector 402 between 168 and 2,820 square units of floor and
	// ceiling each for overlaps of 16 to 96 (tools/portalplan, nbh2). The
	// original draws both sectors whole, because it never asks where a sector
	// lies in plan (ROTH.C renderer.c:7113 walk_visible_sectors, :7319
	// build_sector_render_tree_recursive). minArea stays as a parameter for
	// the survey tool; the loader passes 0.
	std::vector<std::vector<int>> conflict((size_t)ns);
	int realPairs = 0;
	for (const OverlapPair &po : overlaps)
	{
		if (po.a < 0 || po.b < 0 || po.a >= ns || po.b >= ns || po.a == po.b) continue;
		if (po.area < minArea)
		{
			plan.ignoredSlivers++;
			say(fmt("sliver left alone: sectors %.0f and %.0f share ~%.0f sq units",
				po.a, po.b, po.area));
			continue;
		}
		conflict[po.a].push_back(po.b);
		conflict[po.b].push_back(po.a);
		realPairs++;
	}
	if (realPairs == 0)
	{
		if (plan.ignoredSlivers > 0) say("only sliver overlaps: nothing moved");
		return plan;
	}

	std::vector<int> colour((size_t)ns, -1), comp((size_t)ns, -1);
	std::vector<char> compBipartite;
	int numComp = 0;
	for (int s0 = 0; s0 < ns; s0++)
	{
		if (conflict[s0].empty() || comp[s0] >= 0) continue;
		std::vector<int> members; members.push_back(s0);
		comp[s0] = numComp; colour[s0] = 0;
		bool bip = true;
		for (size_t h = 0; h < members.size(); h++)
			for (int t : conflict[members[h]])
			{
				if (comp[t] < 0)
				{
					comp[t] = numComp; colour[t] = 1 - colour[members[h]];
					members.push_back(t);
				}
				else if (colour[t] == colour[members[h]]) bip = false;
			}
		if (bip)
		{
			// The lower side is colour 0.
			double z[2] = { 0., 0. }; int n[2] = { 0, 0 };
			for (int m : members) { z[colour[m]] += rm.sectors[m].floorHeight; n[colour[m]]++; }
			if (n[0] && n[1] && z[0] / n[0] > z[1] / n[1])
				for (int m : members) colour[m] = 1 - colour[m];
		}
		else
		{
			// Three or more storeys: greedy, lowest floor first.
			for (int m : members) colour[m] = -1;
			std::vector<int> order = members;
			for (size_t i = 1; i < order.size(); i++)
				for (size_t j = i; j > 0 && rm.sectors[order[j]].floorHeight
					< rm.sectors[order[j - 1]].floorHeight; j--)
					std::swap(order[j], order[j - 1]);
			for (int m : order)
			{
				int c = 0;
				for (bool again = true; again; )
				{
					again = false;
					for (int t : conflict[m]) if (colour[t] == c) { c++; again = true; }
				}
				colour[m] = c;
			}
		}
		compBipartite.push_back(bip ? 1 : 0);
		numComp++;
	}

	//------------------------------------------------------------------
	// The adjacency graph, from the SAME pairing the loader's line loop does
	// (first unconsumed face takes its sister), so a cut names a loader line.
	//------------------------------------------------------------------
	std::vector<Pair> pairs;
	{
		std::vector<char> consumed((size_t)nf, 0);
		for (int i = 0; i < nf; i++)
		{
			if (consumed[i]) continue;
			const roth::Face &f = rm.faces[i];
			if (!skipSectors.empty() && (skipped(f.sector)
				|| (f.sister >= 0 && f.sister < nf && skipped(rm.faces[f.sister].sector))))
			{
				consumed[i] = 1;
				if (f.sister >= 0 && f.sister < nf) consumed[f.sister] = 1;
				continue;
			}
			if (f.sector < 0) continue;
			const int s = f.sister;
			if (s >= 0 && s < nf && !consumed[s] && rm.faces[s].sector >= 0)
			{
				consumed[s] = 1;
				if (f.sector < ns && rm.faces[s].sector < ns && f.sector != rm.faces[s].sector)
					pairs.push_back({ i, s, f.sector, rm.faces[s].sector, 10 });
			}
			consumed[i] = 1;
		}
	}

	// Penalties. A clean face costs 10, so "one more portal" always loses to
	// "no hazard" until the hazard would cost a great many portals.
	std::vector<char> doorish((size_t)ns, 0), nearDoor((size_t)ns, 0);
	for (int s = 0; s < ns; s++) doorish[s] = rm.sectors[s].IsDoorCapable() ? 1 : 0;
	for (const Pair &p : pairs)
	{
		if (doorish[p.sa]) nearDoor[p.sb] = 1;
		if (doorish[p.sb]) nearDoor[p.sa] = 1;
	}
	std::vector<char> triggerFace((size_t)nf, 0);
	for (const roth::Command &c : rm.commands)
		for (int fi : c.faces) if (fi >= 0 && fi < nf) triggerFace[fi] = 1;

	// Door reach, per face of every pair (detail::DoorReaching).
	std::vector<int> doorReach((size_t)nf, -1);  // face -> a door sector whose leaf reaches it
	for (const Pair &p : pairs)
	{
		if (skipped(p.sa) || skipped(p.sb)) continue;
		const int d = DoorReaching(rm, p.face, p.sa, p.sb);
		if (d >= 0 && !skipped(d)) doorReach[p.face] = doorReach[p.sister] = d;
	}
	static const int kRefused = 100000;          // the price of a face that must not be cut
	auto baseCost = [&](const Pair &p) -> int
	{
		const roth::Sector &A = rm.sectors[p.sa], &B = rm.sectors[p.sb];
		const roth::Face &f = rm.faces[p.face];
		int c = 10;
		if (doorish[p.sa] || doorish[p.sb]) c += 2000;       // a leaf swings here
		else if (nearDoor[p.sa] && nearDoor[p.sb]) c += 300; // between two doorways
		else if (nearDoor[p.sa] || nearDoor[p.sb]) c += 60;  // a leaf may swing across
		if (A.platformIndex >= 0 || B.platformIndex >= 0) c += 60;   // a slab would end at the seam
		if (triggerFace[p.face] || triggerFace[p.sister]) c += 100;
		if (f.vertex1 >= 0 && f.vertex2 >= 0 && f.vertex1 < nv && f.vertex2 < nv)
		{
			const double dx = double(rm.vertices[f.vertex2].x) - rm.vertices[f.vertex1].x;
			const double dy = double(rm.vertices[f.vertex2].y) - rm.vertices[f.vertex1].y;
			if (dx * dx + dy * dy < 16. * 16.) c += 40;        // a portal a few units wide
		}
		if (A.floorHeight != B.floorHeight || A.ceilingHeight != B.ceilingHeight) c += 3;
		if (A.commandID != 0 || B.commandID != 0) c += 8;      // logic may move or relight it
		if (refuseDoorReach && doorReach[p.face] >= 0) c += kRefused;
		return c;
	};

	// Which overlap stacks are being separated. A stack whose cut would have
	// to cross a door leaf's swing is switched off again and reported; the
	// others still go.
	std::vector<char> compOn((size_t)numComp, 0);
	const std::vector<int> fullColour = colour;
	int numColours = 0;
	auto applyActive = [&]()
	{
		numColours = 1;
		for (int s = 0; s < ns; s++)
		{
			colour[s] = (comp[s] >= 0 && compOn[comp[s]]) ? fullColour[s] : -1;
			if (colour[s] + 1 > numColours) numColours = colour[s] + 1;
		}
	};

	// One full assignment for the current colouring; returns the total cost.
	auto solve = [&](std::vector<int> &outGroup) -> long long
	{
		outGroup.assign((size_t)ns, 0);
		long long total = 0;
		for (int c = numColours - 1; c >= 1; c--)
		{
			const int S = ns, T = ns + 1;
			Flow flow(ns + 2);
			const long long INF = (long long)1e15;
			bool anySink = false, anySource = false;
			for (int s = 0; s < ns; s++)
			{
				if (outGroup[s] != 0) continue;       // already taken by a higher layer
				if (colour[s] == c) { flow.Add(s, T, INF, false); anySink = true; }
				else if (colour[s] >= 0) { flow.Add(S, s, INF, false); anySource = true; }
			}
			if (!anySink || !anySource) continue;
			for (Pair &p : pairs)
			{
				if (outGroup[p.sa] != 0 || outGroup[p.sb] != 0) continue;
				p.cost = baseCost(p);
				flow.Add(p.sa, p.sb, p.cost, true);
			}
			total += flow.Run(S, T);
			std::vector<char> sink;
			flow.SinkSide(T, sink);
			for (int s = 0; s < ns; s++)
				if (outGroup[s] == 0 && sink[s]) outGroup[s] = c;
		}
		return total;
	};

	//------------------------------------------------------------------
	// 2 + 3. Cut; refuse a cut a door leaf can reach.
	//------------------------------------------------------------------
	std::vector<int> group;
	std::vector<int> cutPairs;
	std::vector<int> lastFailed;

	// One attempt with the stacks currently switched on. Succeeds unless the
	// cheapest cut still goes through a refused face (a cut that wanders
	// across the map to avoid one seam is worse than leaving that one overlap
	// alone -- and the price of a refused face is what stops it wandering).
	auto attempt = [&]() -> bool
	{
		applyActive();
		long long best = solve(group);
		// Which of two unrelated stacks is called "upper" changes the cost.
		for (int pass = 0; pass < 2 && numColours == 2; pass++)
			for (int k = 0; k < numComp; k++)
			{
				if (!compOn[k] || !compBipartite[k]) continue;
				for (int s = 0; s < ns; s++) if (comp[s] == k) colour[s] = 1 - colour[s];
				std::vector<int> trial;
				const long long cst = solve(trial);
				if (cst < best) { best = cst; group = trial; }
				else for (int s = 0; s < ns; s++) if (comp[s] == k) colour[s] = 1 - colour[s];
			}
		cutPairs.clear();
		lastFailed.clear();
		for (size_t i = 0; i < pairs.size(); i++)
			if (group[pairs[i].sa] != group[pairs[i].sb])
			{
				if (refuseDoorReach && doorReach[pairs[i].face] >= 0) lastFailed.push_back(pairs[i].face);
				cutPairs.push_back((int)i);
			}
		return lastFailed.empty();
	};

	// Biggest stacks first; each is kept only if the whole plan still works
	// with it switched on.
	{
		std::vector<double> compArea((size_t)numComp, 0.);
		for (const OverlapPair &po : overlaps)
			if (po.a >= 0 && po.a < ns && comp[po.a] >= 0) compArea[comp[po.a]] += po.area;
		std::vector<int> order((size_t)numComp);
		for (int k = 0; k < numComp; k++) order[k] = k;
		for (int i = 1; i < numComp; i++)
			for (int j = i; j > 0 && compArea[order[j]] > compArea[order[j - 1]]; j--)
				std::swap(order[j], order[j - 1]);
		int kept = 0;
		for (int k : order)
		{
			compOn[k] = 1;
			if (attempt()) { kept++; continue; }
			compOn[k] = 0;
			std::string members;
			for (int s = 0; s < ns; s++)
				if (comp[s] == k) members += (members.empty() ? "" : ",") + std::to_string(s);
			say("stack left as it is (it cannot be separated without cutting a face a door"
				" leaf swings across): sectors " + members);
			for (int face : lastFailed)
				say(fmt("    face %.0f (sector %.0f) is within reach of the door leaf of sector %.0f",
					face, rm.faces[face].sector, doorReach[face]));
			plan.unresolvedStacks++;
		}
		if (kept == 0 || !attempt())
		{
			say("nothing could be separated: map left as it is");
			plan.group.assign((size_t)ns, 0);
			return plan;
		}
	}

	// The biggest group is the one that stays where it is.
	{
		std::vector<int> count((size_t)numColours, 0), order((size_t)numColours), rank((size_t)numColours);
		for (int s = 0; s < ns; s++) if (outline[s].usable) count[group[s]]++;
		for (int g = 0; g < numColours; g++) order[g] = g;
		for (int i = 1; i < numColours; i++)
			for (int j = i; j > 0 && count[order[j]] > count[order[j - 1]]; j--)
				std::swap(order[j], order[j - 1]);
		for (int i = 0; i < numColours; i++) rank[order[i]] = i;
		for (int s = 0; s < ns; s++) group[s] = rank[group[s]];
	}
	plan.group = group;
	plan.numGroups = numColours;

	//------------------------------------------------------------------
	// 4. The translation: a whole number of flat periods (GRID below), clear
	// of all that has been placed (and a margin). East of everything if the node builder's
	// 16.16 range has room there, else west, else north -- never south, where
	// the loader parks its door leaves and slab controls.
	//------------------------------------------------------------------
	plan.offX.assign((size_t)numColours, 0.);
	plan.offY.assign((size_t)numColours, 0.);
	{
		struct Box { double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18; };
		std::vector<Box> gb((size_t)numColours);
		for (int s = 0; s < ns; s++)
		{
			if (!outline[s].usable) continue;
			Box &b = gb[group[s]];
			if (outline[s].minx < b.x0) b.x0 = outline[s].minx;
			if (outline[s].maxx > b.x1) b.x1 = outline[s].maxx;
			if (outline[s].miny < b.y0) b.y0 = outline[s].miny;
			if (outline[s].maxy > b.y1) b.y1 = outline[s].maxy;
		}
		const double MARGIN = 1024., LIMIT = 30000.;
		Box all = gb[0];
		bool fits = all.x1 >= all.x0;
		for (int g = 1; g < numColours && fits; g++)
		{
			const Box &b = gb[g];
			if (b.x1 < b.x0) continue;
			const double pad = MARGIN;
			double ox = 0., oy = 0.;
			// A WHOLE NUMBER OF FLAT PERIODS (GRID). A Realms flat is anchored
			// to the world, not to its sector: col = -x / 2^s + shift (mod W),
			// s = 0..3 (roth_surface.h FlatTexel; ROTH.C renderer.c:3375-3406).
			// A moved sector keeps its picture where it was only if the move
			// is a multiple of W * 2^s for every flat in it; 2048 is that for
			// every power-of-two width up to 256 at every scale. It was 64,
			// which slid every flat wider than 64 units (STUDY1's 32-texel
			// ceiling tile at s = 2 is 128 units; 5504 is 43 of those, by
			// luck, but 8768 in STUDY2 is 68.5).
			const double GRID = 2048.;
			const double east = ceil((all.x1 + pad - b.x0) / GRID) * GRID;
			const double west = floor((all.x0 - pad - b.x1) / GRID) * GRID;
			const double north = ceil((all.y1 + pad - b.y0) / GRID) * GRID;
			if (b.x1 + east <= LIMIT) ox = east;
			else if (b.x0 + west >= -LIMIT) ox = west;
			else if (b.y1 + north <= LIMIT) oy = north;
			else { fits = false; break; }
			plan.offX[g] = ox; plan.offY[g] = oy;
			if (b.x0 + ox < all.x0) all.x0 = b.x0 + ox;
			if (b.x1 + ox > all.x1) all.x1 = b.x1 + ox;
			if (b.y0 + oy < all.y0) all.y0 = b.y0 + oy;
			if (b.y1 + oy > all.y1) all.y1 = b.y1 + oy;
		}
		if (!fits)
		{
			say("no room inside the node builder's coordinate range to set a storey aside:"
				" map left as it is");
			plan.group.assign((size_t)ns, 0);
			plan.offX.assign(1, 0.); plan.offY.assign(1, 0.);
			plan.numGroups = 1;
			return plan;
		}
	}

	//------------------------------------------------------------------
	// Moved vertices: every vertex a moved sector's faces use.
	//------------------------------------------------------------------
	for (int s = 0; s < ns; s++)
	{
		const int g = group[s];
		if (g <= 0) continue;
		const roth::Sector &rs = rm.sectors[s];
		if (rs.firstFaceIndex < 0 || rs.firstFaceIndex + rs.faceCount > nf) continue;
		for (int k = 0; k < rs.faceCount; k++)
		{
			const roth::Face &f = rm.faces[rs.firstFaceIndex + k];
			const int vs[2] = { f.vertex1, f.vertex2 };
			for (int vi : vs)
			{
				if (vi < 0 || vi >= nv) continue;
				const std::pair<int, int> key(g, vi);
				if (plan.movedVertex.count(key)) continue;
				plan.movedVertex[key] = (int)plan.extraVerts.size();
				plan.extraVerts.push_back({ double(rm.vertices[vi].x) + plan.offX[g],
					double(rm.vertices[vi].y) + plan.offY[g] });
			}
		}
	}

	//------------------------------------------------------------------
	// The cuts.
	//------------------------------------------------------------------
	for (int ci : cutPairs)
	{
		const Pair &p = pairs[ci];
		Cut c;
		c.face = p.face; c.sister = p.sister;
		c.groupFace = group[p.sa]; c.groupSister = group[p.sb];
		c.cost = baseCost(p);
		c.doorSector = doorReach[p.face];
		if (c.cost > 13) plan.penalisedCuts++;
		if (c.doorSector >= 0)
		{
			plan.doorReachCuts++;
			say(fmt("cut face %.0f|%.0f is within reach of the door leaf of sector %.0f:"
				" the leaf will end at the seam", p.face, p.sister, c.doorSector));
		}
		plan.cutByFace[p.face] = (int)plan.cuts.size();
		plan.cutByFace[p.sister] = (int)plan.cuts.size();
		plan.cuts.push_back(c);
	}

	plan.active = !plan.cuts.empty() || numColours > 1;
	say(fmt("%.0f overlap pair(s) in %.0f stack(s) -> %.0f group(s), %.0f cut face pair(s)",
		realPairs, numComp, numColours, (double)plan.cuts.size()));
	return plan;
}

//==========================================================================
//
// SectorAt
//
// The Realms sector a Realms coordinate lies in. Where more than one outline
// holds the point -- which is the whole subject of this file -- the one whose
// floor..ceiling span holds z wins, then the one whose floor is nearest below.
// -1 if no outline holds it.
//
//==========================================================================

inline int SectorAt(const roth::Map &rm, double x, double y, double z)
{
	int best = -1;
	double bestScore = 1e18;
	const int nf = (int)rm.faces.size(), nv = (int)rm.vertices.size();
	for (int s = 0; s < (int)rm.sectors.size(); s++)
	{
		const roth::Sector &rs = rm.sectors[s];
		if (rs.faceCount < 3 || rs.firstFaceIndex < 0 || rs.firstFaceIndex + rs.faceCount > nf) continue;
		bool in = false, bad = false;
		for (int k = 0; k < rs.faceCount && !bad; k++)
		{
			const roth::Face &f = rm.faces[rs.firstFaceIndex + k];
			if (f.vertex1 < 0 || f.vertex2 < 0 || f.vertex1 >= nv || f.vertex2 >= nv) { bad = true; break; }
			const double ax = rm.vertices[f.vertex1].x, ay = rm.vertices[f.vertex1].y;
			const double bx = rm.vertices[f.vertex2].x, by = rm.vertices[f.vertex2].y;
			if ((ay > y) != (by > y))
			{
				const double xi = ax + (y - ay) * (bx - ax) / (by - ay);
				if (xi > x) in = !in;
			}
		}
		if (bad || !in) continue;
		double score;
		if (z >= rs.floorHeight && z <= rs.ceilingHeight) score = z - rs.floorHeight;
		else if (z > rs.ceilingHeight) score = 1e6 + (z - rs.ceilingHeight);
		else score = 2e6 + (rs.floorHeight - z);
		if (score < bestScore) { bestScore = score; best = s; }
	}
	return best;
}

} // namespace rothplan
