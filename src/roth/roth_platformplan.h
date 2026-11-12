/*
** roth_platformplan.h
**
** Moving sectors, planned: which of a map's RAW opcode 0x09 "move sector"
** records can be shown as a solid platform ACTOR that slides over a static
** map, and exactly what boxes that actor is made of.
**
** WHY A PLAN AND NOT JUST CODE IN THE LOADER. The original moves a sector by
** adding a delta to its four vertices every frame (tick_moving_sector,
** raw_commands.c:5869), and the neighbouring sectors stretch to follow because
** they share those vertices. A Doom BSP cannot do that. What it CAN do is keep
** the map still and slide an actor -- provided that taking the moving sector's
** own features out of the map leaves nothing behind that should have moved.
** That is true exactly when the four neighbours are level with each other: then
** "the sector slides and its neighbours stretch" and "a slab / a block slides
** along a level channel" are the same picture. Whether a record satisfies that
** is a property of the map data, so it is decided here, once, and the loader,
** the runtime and the offline survey (tools/platforms) all read this answer.
**
** ENGINE-FREE ON PURPOSE, like roth_portalplan.h: it reads roth::Map and
** nothing else, so tools/platforms/platsurvey.cpp prints what the loader will
** build without starting the engine. All of it is fresh code for this project.
**
** The classes, measured over the 44 retail maps (32 records, 5 maps, every one
** an axis-aligned four-face rectangle):
**
**   SLAB        the sector is level with its neighbours and carries a
**               mid-platform: the slab is what visibly moves.            12
**   FLOORBLOCK  the sector's floor stands above its neighbours': a plate, a
**               pillar or a barrier wall slides along the floor.          3
**   FLOOR+CEIL  both: a block on the floor and a block hanging from the
**               ceiling, with the walkable gap between (SALVAT's tiles). 13
**   unsupported the sector has one-sided walls of its own (a pocket in solid
**               wall: moving it moves the WALL, which is not a platform).  4
**
**---------------------------------------------------------------------------
** SPDX-License-Identifier: GPL-3.0-or-later
**---------------------------------------------------------------------------
*/
#pragma once

#include <math.h>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>
#include "roth_raw.h"

namespace rothplat
{

enum PieceKind
{
	PIECE_SLAB = 0,        // the mid-platform: underside .. top
	PIECE_FLOORBLOCK = 1,  // neighbours' floor .. this sector's floor
	PIECE_CEILBLOCK = 2,   // this sector's ceiling .. neighbours' ceiling
};

struct Piece
{
	int kind = PIECE_SLAB;
	int z0 = 0, z1 = 0;    // vertical extent at load; Realms heights are world units
};

struct Plan
{
	int sector = -1;                 // the Realms sector that moves
	std::vector<int> records;        // every 0x09 record naming it, 1-based
	bool supported = false;
	std::string reason;              // why not, or the class when supported

	// The footprint at load, in OUR coordinates (the reader's, wrap shift applied).
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

	// What the four neighbours agree on, and one of them to borrow flats from
	// for the part of the static map the moving piece used to cover.
	int nbFloor = 0, nbCeil = 0, nbSector = -1;
	bool lowerFloor = false;         // the static sector's floor drops to nbFloor
	bool raiseCeil = false;          // the static sector's ceiling rises to nbCeil

	std::vector<Piece> pieces;

	// Collision. A platform actor is an upright box with a SQUARE footprint
	// (radius), so a rectangle that is not square is covered by a row of equal
	// squares along its long side, overlapping, moved as one rigid group.
	double radius = 0;
	std::vector<std::pair<double, double>> centres;
};

// The four vertices cmd_move_sector takes: vertex1 of the sector's first four
// face records (raw_commands.c:4609-4616 reads the vertex slot at face +0, the
// faces being 0x0c apart). Returns false when the sector has fewer than four.
inline bool MovedVertices(const roth::Map &m, int sector, int out[4])
{
	if (sector < 0 || (size_t)sector >= m.sectors.size()) return false;
	const roth::Sector &s = m.sectors[sector];
	if (s.faceCount < 4 || s.firstFaceIndex < 0) return false;
	for (int k = 0; k < 4; k++)
	{
		const size_t fi = (size_t)(s.firstFaceIndex + k);
		if (fi >= m.faces.size()) return false;
		const int v = m.faces[fi].vertex1;
		if (v < 0 || (size_t)v >= m.vertices.size()) return false;
		out[k] = v;
	}
	return true;
}

// find_geometry_record (raw_commands.c:50): the FIRST sector whose id matches.
inline int SectorForKey(const roth::Map &m, uint16_t key)
{
	for (size_t i = 0; i < m.sectors.size(); i++)
		if (m.sectors[i].commandID == key) return (int)i;
	return -1;
}

inline void TileFootprint(Plan &p)
{
	const double w = double(p.x1 - p.x0), h = double(p.y1 - p.y0);
	const double shortSide = w < h ? w : h, longSide = w < h ? h : w;
	p.radius = shortSide * 0.5;
	p.centres.clear();
	if (shortSide <= 0) return;
	// ceil(aspect) squares cover the rectangle exactly: the first and last sit
	// flush with the ends and the rest are spread evenly between.
	int n = (int)ceil(longSide / shortSide - 1e-9);
	if (n < 1) n = 1;
	for (int i = 0; i < n; i++)
	{
		const double t = n == 1 ? 0.5 * longSide
			: p.radius + (longSide - shortSide) * double(i) / double(n - 1);
		if (w < h) p.centres.push_back({ p.x0 + p.radius, p.y0 + t });
		else       p.centres.push_back({ p.x0 + t, p.y0 + p.radius });
	}
}

inline std::vector<Plan> PlanMovingSectors(const roth::Map &m)
{
	std::vector<Plan> plans;
	for (size_t ci = 0; ci < m.commands.size(); ci++)
	{
		const roth::Command &c = m.commands[ci];
		if (c.isTrigger || (c.opcode & 0x7f) != 0x09) continue;
		const int si = SectorForKey(m, c.key);
		if (si < 0) continue;

		Plan *have = nullptr;
		for (Plan &p : plans) if (p.sector == si) have = &p;
		if (have != nullptr) { have->records.push_back((int)ci + 1); continue; }

		Plan p;
		p.sector = si;
		p.records.push_back((int)ci + 1);
		const roth::Sector &s = m.sectors[si];

		int mv[4];
		if (s.faceCount != 4 || !MovedVertices(m, si, mv))
		{
			p.reason = "not a four-face sector";
			plans.push_back(p);
			continue;
		}

		// An axis-aligned rectangle, and no wall of its own.
		bool rect = true, oneSided = false, level = true, first = true;
		p.x0 = p.y0 = INT32_MAX; p.x1 = p.y1 = INT32_MIN;
		for (int k = 0; k < 4; k++)
		{
			const roth::Face &f = m.faces[s.firstFaceIndex + k];
			if (f.vertex1 < 0 || f.vertex2 < 0) { rect = false; break; }
			const roth::Vertex &a = m.vertices[f.vertex1], &b = m.vertices[f.vertex2];
			if (a.x != b.x && a.y != b.y) rect = false;
			if (a.x < p.x0) p.x0 = a.x;
			if (a.x > p.x1) p.x1 = a.x;
			if (a.y < p.y0) p.y0 = a.y;
			if (a.y > p.y1) p.y1 = a.y;

			if (f.sister < 0 || (size_t)f.sister >= m.faces.size()
				|| m.faces[f.sister].sector < 0) { oneSided = true; continue; }
			const roth::Sector &t = m.sectors[m.faces[f.sister].sector];
			if (first)
			{
				p.nbFloor = t.floorHeight; p.nbCeil = t.ceilingHeight;
				p.nbSector = m.faces[f.sister].sector;
				first = false;
			}
			else if (t.floorHeight != p.nbFloor || t.ceilingHeight != p.nbCeil) level = false;
		}

		if (!rect || p.x1 <= p.x0 || p.y1 <= p.y0)
			p.reason = "not an axis-aligned rectangle";
		else if (oneSided)
			p.reason = "pocket: the sector has one-sided walls, so moving it moves a wall";
		else if (!level)
			p.reason = "the four neighbours are not level with each other";
		else if (s.floorHeight < p.nbFloor || s.ceilingHeight > p.nbCeil)
			p.reason = "a moving pit or alcove: nothing solid to make an actor of";
		else
		{
			if (s.floorHeight > p.nbFloor)
			{
				Piece b; b.kind = PIECE_FLOORBLOCK; b.z0 = p.nbFloor; b.z1 = s.floorHeight;
				p.pieces.push_back(b);
				p.lowerFloor = true;
			}
			if (s.platformIndex >= 0 && (size_t)s.platformIndex < m.platforms.size())
			{
				const roth::MidPlatform &mp = m.platforms[s.platformIndex];
				if (mp.topZ > mp.undersideZ)
				{
					Piece b; b.kind = PIECE_SLAB; b.z0 = mp.undersideZ; b.z1 = mp.topZ;
					p.pieces.push_back(b);
				}
			}
			if (s.ceilingHeight < p.nbCeil)
			{
				Piece b; b.kind = PIECE_CEILBLOCK; b.z0 = s.ceilingHeight; b.z1 = p.nbCeil;
				p.pieces.push_back(b);
				p.raiseCeil = true;
			}

			if (p.pieces.empty())
				p.reason = "nothing about the sector differs from its neighbours";
			else
			{
				p.supported = true;
				p.reason = p.lowerFloor && p.raiseCeil ? "floor block + ceiling block"
					: p.lowerFloor ? "floor block" : p.raiseCeil ? "ceiling block" : "slab";
				TileFootprint(p);
			}
		}
		plans.push_back(p);
	}
	return plans;
}

} // namespace rothplat
