/*
** roth_portalplan.h
**
** Rooms over rooms, planned: which Realms sectors have to be moved to their
** own region of the map so that no two sectors of one region share plan area,
** which faces are cut to do it, and the exact extra geometry (moved vertices,
** buffer sectors behind every cut face, their walls) the loader must emit so
** the cut faces can be joined again by linked line portals.
**
** ENGINE-FREE ON PURPOSE. It reads roth::Map and nothing else, so the offline
** harness (the standalone FNodeBuilder check) and the level loader run the
** SAME planner and cannot disagree about what was built. All of it is fresh
** code for this project; nothing here is ported from another engine.
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
**   3. BUFFERS. This engine only lets an actor through a line portal that has
**      a back sector with empty space behind it (portal.cpp, UpdatePortal), and
**      checks an actor straddling the portal against the lines found at the
**      translated position (p_maputl.cpp, FMultiBlockLinesIterator). So each
**      cut face gets a rectangular buffer sector behind it, in its own group's
**      space, carrying the heights of the sector really on the other side. A
**      buffer must not overlap anything of its own group; a face whose buffer
**      cannot be placed is priced out and the cut is solved again.
**
**   4. THE TRANSLATION. Group 0 stays. Every other group moves along +X, clear
**      of everything placed before it, by a whole number of map units (the
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
	int bufBehindFace = -1;         // buffer index: lies in face's group, stands in for sister's sector
	int bufBehindSister = -1;       // buffer index: lies in sister's group, stands in for face's sector
	int cost = 0;                   // what the cut paid for this face (10 = clean)
};

struct Buffer
{
	int group = 0;
	int likeSector = -1;            // the Realms sector whose heights, flats and light it copies
};

// A wall of a buffer. Vertex numbers are ENGINE vertex indices: below
// map.vertices.size() the Realms vertex itself, otherwise extraBase + k.
struct BufferLine { int v1, v2, front, back; };   // back < 0: one-sided

struct ExtraVertex { double x, y; };

struct Plan
{
	bool active = false;            // false: nothing to do, or it could not be done -- see notes
	int numGroups = 1;
	double depth = 0.;              // buffer depth used
	std::vector<int> group;         // per Realms sector
	std::vector<double> offX, offY; // per group
	std::vector<Cut> cuts;
	std::vector<Buffer> buffers;
	std::vector<BufferLine> bufferLines;
	std::vector<ExtraVertex> extraVerts;    // already translated
	std::map<std::pair<int, int>, int> movedVertex;   // (group, Realms vertex) -> index into extraVerts
	std::map<int, int> cutByFace;   // face OR sister -> index into cuts
	std::vector<std::string> notes; // everything worth a line in the load report
	int ignoredSlivers = 0;         // overlap pairs below the area threshold
	int penalisedCuts = 0;          // cuts that had to pay a penalty (hazards)
	int unresolvedStacks = 0;       // overlap stacks that could not be separated
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

} // namespace detail

//==========================================================================
//
// BuildPlan
//
// extraBase: the engine vertex index the plan's extra vertices will start at.
// minArea:   overlap pairs smaller than this many square units are slivers
//            and are left alone (and counted).
//
//==========================================================================

inline Plan BuildPlan(const roth::Map &rm, const std::vector<OverlapPair> &overlaps,
	const std::vector<int> &skipSectors, int extraBase, double minArea = 64.)
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
	// A pair is a SLIVER, and is left alone, when it is tiny outright or when
	// it is a small bite out of both sectors: a niche whose outline pokes a
	// few units into the room it opens on (CHURCH1 sector 558 into 348/359)
	// is an authoring overlap, not a room over a room, and a portal cannot be
	// stood in a seam four units deep.
	std::vector<double> planArea((size_t)ns, 0.);
	for (int s = 0; s < ns; s++)
	{
		const Outline &o = outline[s];
		double a = 0.;
		for (size_t i = 0; i < o.x1.size(); i++) a += o.x1[i] * o.y2[i] - o.x2[i] * o.y1[i];
		planArea[s] = fabs(a) / 2.;
	}
	std::vector<std::vector<int>> conflict((size_t)ns);
	int realPairs = 0;
	for (const OverlapPair &po : overlaps)
	{
		if (po.a < 0 || po.b < 0 || po.a >= ns || po.b >= ns || po.a == po.b) continue;
		const double smaller = planArea[po.a] < planArea[po.b] ? planArea[po.a] : planArea[po.b];
		if (po.area < minArea || (po.area < 512. && po.area < 0.2 * smaller))
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
	std::vector<int> extraCost((size_t)nf, 0);   // raised when a buffer cannot be placed
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
		return c;
	};

	// Which overlap stacks are being separated. A stack whose cut cannot be
	// given buffers is switched off again and reported; the others still go.
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
				p.cost = baseCost(p) + extraCost[p.face];
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
	// 2 + 3. Cut, place buffers, and re-cut while a buffer cannot be placed.
	//------------------------------------------------------------------
	static const double kDepths[3] = { 128., 96., 64. };
	struct Rect { double x[4], y[4]; int group; };   // v2, v1, f1, f2 (clockwise)
	auto rectHolds = [](const Rect &r, double px, double py)
	{
		bool in = false;
		for (int i = 0, j = 3; i < 4; j = i++)
			if ((r.y[i] > py) != (r.y[j] > py))
			{
				const double xi = r.x[j] + (py - r.y[j]) * (r.x[i] - r.x[j]) / (r.y[i] - r.y[j]);
				if (xi > px) in = !in;
			}
		return in;
	};
	auto makeRect = [&](int face, double depth, int group) -> Rect
	{
		const roth::Face &f = rm.faces[face];
		const double ax = rm.vertices[f.vertex1].x, ay = rm.vertices[f.vertex1].y;
		const double bx = rm.vertices[f.vertex2].x, by = rm.vertices[f.vertex2].y;
		const double len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
		// The owning sector is on the RIGHT of v1->v2; the buffer goes LEFT.
		const double nx = -(by - ay) / len * depth, ny = (bx - ax) / len * depth;
		Rect r;
		r.x[0] = bx; r.y[0] = by;  r.x[1] = ax; r.y[1] = ay;
		r.x[2] = ax + nx; r.y[2] = ay + ny;  r.x[3] = bx + nx; r.y[3] = by + ny;
		r.group = group;
		return r;
	};

	std::vector<int> group;
	std::vector<int> cutPairs;
	std::vector<int> lastFailed;
	double depthUsed = 0.;

	// One attempt with the stacks currently switched on: cut, try to place
	// every buffer, price out a face that has no room and cut again. Succeeds
	// only if every buffer is placed AND the cut never had to go through a
	// priced-out face (a cut that wanders across the map to avoid one seam is
	// worse than leaving that one overlap alone).
	auto attempt = [&]() -> bool
	{
		applyActive();
		std::fill(extraCost.begin(), extraCost.end(), 0);
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
		for (int round = 0; round < 6; round++)
		{
			if (round > 0) solve(group);
			cutPairs.clear();
			for (size_t i = 0; i < pairs.size(); i++)
				if (group[pairs[i].sa] != group[pairs[i].sb])
				{
					if (extraCost[pairs[i].face] > 0) return false;
					cutPairs.push_back((int)i);
				}
			for (int di = 0; di < 3; di++)
			{
				const double depth = kDepths[di];
				std::vector<Rect> rects;
				std::vector<int> failed;
				for (int ci : cutPairs)
				{
					const Pair &p = pairs[ci];
					for (int side = 0; side < 2; side++)
					{
						const int face = side ? p.sister : p.face;
						const int g = group[side ? p.sb : p.sa];
						const Rect r = makeRect(face, depth, g);
						bool ok = true;
						const double ex = r.x[0] - r.x[1], ey = r.y[0] - r.y[1];   // along the face
						const double fx = r.x[2] - r.x[1], fy = r.y[2] - r.y[1];   // into the buffer
						const double len = sqrt(ex * ex + ey * ey);
						const int nu = (int)(len / 3.) + 2, nw = (int)(depth / 3.) + 2;
						for (int iu = 0; iu < nu && ok; iu++)
							for (int iw = 0; iw < nw && ok; iw++)
							{
								const double u = (iu + 0.4371) / nu, w = (iw + 0.3913) / nw;
								const double px = r.x[1] + ex * u + fx * w, py = r.y[1] + ey * u + fy * w;
								for (int s = 0; s < ns && ok; s++)
									if (group[s] == g && Holds(outline[s], px, py)) ok = false;
								for (size_t q = 0; q < rects.size() && ok; q++)
									if (rects[q].group == g && rectHolds(rects[q], px, py)) ok = false;
							}
						if (!ok) failed.push_back(face);
						rects.push_back(r);
					}
				}
				if (failed.empty()) { depthUsed = depth; return true; }
				if (di == 2)
				{
					lastFailed = failed;
					for (int face : failed)
					{
						extraCost[face] += 5000;
						const int sis = rm.faces[face].sister;
						if (sis >= 0 && sis < nf) extraCost[sis] += 5000;
					}
				}
			}
		}
		return false;
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
			say("stack left as it is (no room for a portal buffer at any cut that separates it):"
				" sectors " + members);
			for (int face : lastFailed)
				say(fmt("    no room behind face %.0f (sector %.0f)", face, rm.faces[face].sector));
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
	plan.depth = depthUsed;
	plan.numGroups = numColours;

	//------------------------------------------------------------------
	// 4. The translation: a whole number of units, clear of all that has
	// been placed (buffers and a margin included). East of everything if the
	// node builder's 16.16 range has room there, else west, else north --
	// never south, where the loader parks its door leaves and slab controls.
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
			const double pad = depthUsed + MARGIN;
			double ox = 0., oy = 0.;
			const double east = ceil((all.x1 + pad - b.x0) / 64.) * 64.;
			const double west = floor((all.x0 - pad - b.x1) / 64.) * 64.;
			const double north = ceil((all.y1 + pad - b.y0) / 64.) * 64.;
			if (b.x1 + east + depthUsed <= LIMIT) ox = east;
			else if (b.x0 + west - depthUsed >= -LIMIT) ox = west;
			else if (b.y1 + north + depthUsed <= LIMIT) oy = north;
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
	// Buffers and their walls. Far corners are shared by position within a
	// group, and a wall two buffers both ask for becomes ONE two-sided line
	// between them (two cut faces side by side in one opening).
	//------------------------------------------------------------------
	std::map<std::pair<int, std::pair<long long, long long>>, int> farVertex;   // (group, quantised pos)
	std::map<std::pair<int, int>, int> wallByEnds;   // (lo vertex, hi vertex) -> bufferLines index
	auto farIndex = [&](int g, double x, double y) -> int
	{
		const std::pair<int, std::pair<long long, long long>> key(g,
			std::make_pair((long long)floor(x * 256. + 0.5), (long long)floor(y * 256. + 0.5)));
		auto it = farVertex.find(key);
		if (it != farVertex.end()) return it->second;
		const int idx = extraBase + (int)plan.extraVerts.size();
		plan.extraVerts.push_back({ x, y });
		farVertex[key] = idx;
		return idx;
	};
	auto addWall = [&](int a, int b, int buf)
	{
		const std::pair<int, int> key(a < b ? a : b, a < b ? b : a);
		auto it = wallByEnds.find(key);
		if (it != wallByEnds.end())
		{
			BufferLine &bl = plan.bufferLines[it->second];
			if (bl.back < 0 && bl.v1 == b && bl.v2 == a) { bl.back = buf; return; }
		}
		wallByEnds[key] = (int)plan.bufferLines.size();
		plan.bufferLines.push_back({ a, b, buf, -1 });
	};
	auto addBuffer = [&](int face, int g, int likeSector) -> int
	{
		const roth::Face &f = rm.faces[face];
		const Rect r = makeRect(face, depthUsed, g);
		const int bi = (int)plan.buffers.size();
		plan.buffers.push_back({ g, likeSector });
		const int v1 = plan.VertexFor(g, f.vertex1), v2 = plan.VertexFor(g, f.vertex2);
		const int f1 = farIndex(g, r.x[2] + plan.offX[g], r.y[2] + plan.offY[g]);
		const int f2 = farIndex(g, r.x[3] + plan.offX[g], r.y[3] + plan.offY[g]);
		// clockwise round the buffer, so it is on the right of each wall
		addWall(v1, f1, bi);
		addWall(f1, f2, bi);
		addWall(f2, v2, bi);
		return bi;
	};
	for (int ci : cutPairs)
	{
		const Pair &p = pairs[ci];
		Cut c;
		c.face = p.face; c.sister = p.sister;
		c.groupFace = group[p.sa]; c.groupSister = group[p.sb];
		c.cost = baseCost(p) + extraCost[p.face];
		if (c.cost > 13) plan.penalisedCuts++;
		c.bufBehindFace = addBuffer(p.face, c.groupFace, p.sb);
		c.bufBehindSister = addBuffer(p.sister, c.groupSister, p.sa);
		plan.cutByFace[p.face] = (int)plan.cuts.size();
		plan.cutByFace[p.sister] = (int)plan.cuts.size();
		plan.cuts.push_back(c);
	}

	plan.active = !plan.cuts.empty() || numColours > 1;
	say(fmt("%.0f overlap pair(s) in %.0f stack(s) -> %.0f group(s), %.0f cut face pair(s),"
		" buffer depth %.0f", realPairs, numComp, numColours, (double)plan.cuts.size(), depthUsed));
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
