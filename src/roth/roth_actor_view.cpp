//
// What the original's renderer draws, as far as the GAME needs to know it.
//
// Realms wakes a creature the first time the renderer resolves its picture
// (renderer.c:5826-5840): the moment the draw list reaches the object, not a
// line-of-sight test and not a distance. So "is it awake" is a question about
// the renderer, and the honest answer is the renderer's own visibility walk:
//
//   build_sector_render_tree_recursive (renderer.c:7319-7505): from the root
//   sector, every wall is projected to screen columns (near plane at depth
//   0x1000, clamped to +-0x3ffe); a portal whose columns overlap the current
//   window, whose far sector is open (ceiling above floor, heights overlapping,
//   a door only when the door pool holds it), is descended with the window
//   narrowed to it. A sector is visited once per path that reaches it.
//
//   build_scene_draw_list (renderer.c:8022-8105): for every visited sector,
//   every object that is not hidden and stands at depth 0x1000 or more in front
//   of the camera goes to finalize_draw_list_entry, which resolves its picture
//   (renderer.c:7713) -- and resolving a creature placeholder spawns its actor.
//   There is NO left/right test on the object itself: a creature in a visited
//   sector wakes even when it stands just off the edge of the screen.
//
// All integer, all the original's widths, so tools/actors_test can compare the
// sector set with the running original's frame by frame.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_actors.h"
#include "roth_raw.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string>

namespace roth
{

namespace
{

inline uint16_t ObjId(const Object &o) { return (uint16_t)(o.textureIndex | (o.textureSource << 8)); }

int32_t Proj(int32_t src, int32_t scale, int32_t depth, int32_t center)
{
	return (int32_t)(((int64_t)src * scale) / depth) + center;      // renderer.c:7311 proj64
}

int32_t Clamp3ffe(int32_t v)
{
	if (v < -0x3ffe) return -0x3ffe;
	if (v < 0x3ffe) return v;
	return 0x3ffe;
}

struct View
{
	const Map *map;
	ActorHost *host;
	int32_t xscale, xcenter;
	int16_t s, c;                 // g_camera_sin / g_camera_cos
	bool rotate;
	uint16_t ox, oy;              // g_view_offset_x / _y
	std::vector<int32_t> vx, vd;  // transformed vertices: view X, depth
	// per sector: the projected wall nodes, built on the first visit
	struct Node { int32_t x0, x2; bool sentinel; int neighbour; };
	std::vector<std::vector<Node>> nodes;
	std::vector<char> built;
	int depth = 0, count = 0, wallCursor = 2;
	int16_t winLeft, winRight;    // 0x852fe / 0x852fc
	std::vector<int> visits;
	std::vector<std::string> *trace = nullptr;

	// rotate_point_2d (renderer.c:1278-1292) on a point already offset.
	void Rot(int16_t x, int16_t y, int32_t &depthOut, int32_t &xOut) const
	{
		if (rotate)
		{
			depthOut = ((int32_t)y * s - (int32_t)x * c) >> 6;
			xOut = ((int32_t)x * s + (int32_t)y * c) >> 6;
		}
		else
		{
			depthOut = (int32_t)((uint32_t)(int32_t)y << 8);
			xOut = (int32_t)((uint32_t)(int32_t)x << 8);
		}
	}

	// The far sector of a wall, or -1 when the walk may not look through it
	// (renderer.c:7360-7385).
	int Through(int sector, const Face &f) const
	{
		if (f.sister < 0) return -1;
		const int nb = map->faces[f.sister].sector;
		if (nb < 0) return -1;
		const Sector &n = map->sectors[nb], &cur = map->sectors[sector];
		if (n.commandID >= 0xfffe)
		{
			if (!host->DoorInPool(nb) && n.commandID != 0xfffe) return -1;
		}
		const bool vis = n.ceilingHeight > n.floorHeight && (int32_t)n.ceilingHeight > (int32_t)cur.floorHeight
			&& (int32_t)n.floorHeight < (int32_t)cur.ceilingHeight;
		return vis ? nb : -1;
	}

	void Build(int sector)
	{
		const Sector &sec = map->sectors[sector];
		const int n = (int8_t)sec.faceCount > 0 ? sec.faceCount : 1;   // the do/while runs at least once
		std::vector<Node> &out = nodes[sector];
		out.assign((size_t)n, Node());
		std::vector<int32_t> X(n + 1), D(n + 1);
		for (int k = 0; k < n; k++)
		{
			const Face &f = map->faces[sec.firstFaceIndex + k];
			X[k] = vx[f.vertex1]; D[k] = vd[f.vertex1];
			if (trace && depth == 1)
			{
				char b[160];
				snprintf(b, sizeof b, "vtx %d,%d -> X=%d D=%d", map->vertices[f.vertex1].x, map->vertices[f.vertex1].y, X[k], D[k]);
				trace->push_back(b);
			}
			out[k].neighbour = Through(sector, f);
		}
		X[n] = X[0]; D[n] = D[0];                                   // the closing node
		for (int k = 0; k < n; k++)
		{
			Node &nd = out[k];
			nd.sentinel = false;
			if (D[k] < 0x1000)
			{
				if (D[k + 1] < 0x1000) { nd.sentinel = true; continue; }
				const int32_t v = (int32_t)(((int64_t)(0x1000 - D[k]) * (X[k + 1] - X[k])) / (D[k + 1] - D[k])) + X[k];
				nd.x0 = Clamp3ffe(Proj(v, xscale, 0x1000, xcenter));
			}
			else nd.x0 = Clamp3ffe(Proj(X[k], xscale, D[k], xcenter));
			if (D[k + 1] < 0x1000)
			{
				const int32_t v = (int32_t)(((int64_t)(0x1000 - D[k + 1]) * (X[k] - X[k + 1])) / (D[k] - D[k + 1])) + X[k + 1];
				nd.x2 = Clamp3ffe(Proj(v, xscale, 0x1000, xcenter));
			}
			else nd.x2 = Clamp3ffe(Proj(X[k + 1], xscale, D[k + 1], xcenter));
		}
	}

	void Recurse(int sector, int parent)
	{
		if (count >= 0xc65) return;                                 // renderer.c:7325
		depth++;
		count++;
		visits.push_back(sector);
		if (!built[sector])
		{
			const int n = (int8_t)map->sectors[sector].faceCount > 0 ? map->sectors[sector].faceCount : 1;
			if (wallCursor >= 0x7ce0) { depth--; return; }         // renderer.c:7347
			wallCursor += 8 + (n + 1) * 0x14;
			built[sector] = 1;
			Build(sector);
		}
		const std::vector<Node> &list = nodes[sector];
		for (const Node &nd : list)
		{
			if (trace && depth == 1)
			{
				char b[200];
				snprintf(b, sizeof b, "node x0=%d x2=%d sentinel=%d nb=%d win=%d,%d", nd.x0, nd.x2, (int)nd.sentinel, nd.neighbour, winLeft, winRight);
				trace->push_back(b);
			}
			if (nd.sentinel) continue;
			const int16_t ax = (int16_t)nd.x0, dx = (int16_t)nd.x2;
			if (ax >= dx) continue;
			if (ax >= winRight) continue;
			if (dx <= winLeft) continue;
			if (nd.neighbour < 0) continue;
			if (depth > 0x3c) continue;
			const int16_t sl = winLeft, sr = winRight;
			if (ax >= winLeft) winLeft = ax;
			if (dx <= winRight) winRight = dx;
			if (winLeft < winRight && nd.neighbour != parent && nd.neighbour != sector)
				Recurse(nd.neighbour, sector);
			winLeft = sl; winRight = sr;
		}
		depth--;
	}
};

} // namespace

ActorWorld::RothViewResult ActorWorld::RenderView(const RothViewParams &p) const
{
	RothViewResult r;
	if (mMap == nullptr || p.rootSector < 0 || (size_t)p.rootSector >= mMap->sectors.size()) return r;
	View v;
	v.map = mMap;
	v.host = mHost;
	v.xscale = p.xscale; v.xcenter = p.xcenter;
	// transform_world_vertices (renderer.c:6738-6777): sincos_pair(-angle),
	// g_camera_sin = its cosine, g_camera_cos = its sine.
	const uint16_t neg = (uint16_t)(0 - p.viewAngle);
	const uint32_t idx = ((uint32_t)neg & 0x1ff) << 1;
	v.c = (int16_t)ActorSin(idx);
	v.s = (int16_t)ActorSin((idx + 0x100) & 0x3ff);
	v.rotate = (neg & 0x1ff) != 0;
	v.ox = (uint16_t)(0 - (uint16_t)p.camX);
	v.oy = (uint16_t)(0 - (uint16_t)p.camY);
	v.vx.resize(mMap->vertices.size()); v.vd.resize(mMap->vertices.size());
	for (size_t i = 0; i < mMap->vertices.size(); i++)
		v.Rot((int16_t)(uint16_t)((uint16_t)mMap->vertices[i].x + v.ox), (int16_t)(uint16_t)((uint16_t)mMap->vertices[i].y + v.oy), v.vd[i], v.vx[i]);
	v.nodes.resize(mMap->sectors.size());
	v.built.assign(mMap->sectors.size(), 0);
	v.winLeft = p.left; v.winRight = p.right;
	v.trace = p.trace;
	v.Recurse(p.rootSector, -1);
	r.visits = v.visits;

	// The object pass (renderer.c:8022-8105), once per visit.
	for (int s : v.visits)
	{
		const std::vector<Object> &list = mMap->objects[s];
		for (size_t i = 0; i < list.size(); i++)
		{
			const Object &o = list[i];
			if (o.flags & 0x80) continue;
			int32_t d = 0, x = 0;
			v.Rot((int16_t)(uint16_t)((uint16_t)o.x + v.ox), (int16_t)(uint16_t)((uint16_t)o.y + v.oy), d, x);
			if (o.flags & 1)                                        // a "type 3" object
			{
				if (d < -0x8000) continue;
				const int32_t ad = x < 0 ? -x : x;
				if ((d != 0 ? ad / d : 0) > 0x28) continue;
			}
			else if (d < 0x1000) continue;                          // behind the near plane
			r.resolved.push_back(mUids[s][i]);
			r.viewIndex.push_back((uint8_t)ViewIndex(o.rotation, p.viewAngle, d, x));
		}
	}
	return r;
}

// The near plane (depth 0x1000) across the screen, column by column, back in
// the world; each run of columns standing in one sector is a root with that
// window. This reproduces the extents walk_visible_sectors hands
// render_clipped_sector_subscene; its own clip pass (clip_sector_walls_to_view
// pass 1) is not lifted in ROTH.C, so the split is measured against the
// running original instead (tools/actors_test "viscmp").
std::vector<ActorWorld::RothViewParams> ActorWorld::ViewRoots(int16_t camX, int16_t camY, uint16_t viewAngle, int playerSector) const
{
	std::vector<RothViewParams> out;
	RothViewParams base;
	base.camX = camX; base.camY = camY; base.viewAngle = viewAngle;
	if (mMap == nullptr || playerSector < 0) return out;
	const uint16_t neg = (uint16_t)(0 - viewAngle);
	const uint32_t idx = ((uint32_t)neg & 0x1ff) << 1;
	const double C = ActorSin(idx) / 16384.0, S = ActorSin((idx + 0x100) & 0x3ff) / 16384.0;
	const bool rotate = (neg & 0x1ff) != 0;
	int prev = -2, start = base.left;
	for (int col = base.left; col <= base.right + 1; col++)
	{
		int sec = prev;
		if (col <= base.right)
		{
			const double vx = (double)(col - base.xcenter) * 0x1000 / base.xscale, d = 0x1000;
			double x, y;
			if (rotate) { x = (vx * S - d * C) / 256.0; y = (vx * C + d * S) / 256.0; }
			else { x = vx / 256.0; y = d / 256.0; }
			sec = LocateSector((int16_t)lround(camX + x), (int16_t)lround(camY + y), mMap->sectors[playerSector].floorHeight, playerSector);
			if (sec < 0) sec = playerSector;
		}
		if (col == base.left) { prev = sec; continue; }
		if (sec != prev || col == base.right + 1)
		{
			RothViewParams p = base;
			p.rootSector = prev;
			p.left = (int16_t)start;
			p.right = (int16_t)(col == base.right + 1 ? base.right : col);
			out.push_back(p);
			start = col;
			prev = sec;
		}
	}
	// Column sampling flickers where the near plane runs along a wall; the
	// original gives each sector one run. A sector met again folds into the run
	// before it, and runs of one sector join.
	{
		std::vector<RothViewParams> m;
		for (const RothViewParams &p : out)
		{
			bool seen = false;
			for (const RothViewParams &q : m) if (q.rootSector == p.rootSector) seen = true;
			if (!m.empty() && (seen || m.back().rootSector == p.rootSector)) { m.back().right = p.right; continue; }
			m.push_back(p);
		}
		out.swap(m);
	}
	// The boundary between two pieces is where the wall between their sectors
	// crosses the near plane, in the original's own integers
	// (clip_sector_walls_to_view's interpolation, renderer.c:6797+, and the
	// projection of walk_visible_sectors' extents, 7240-7250).
	if (out.size() > 1)
	{
		View v;
		v.map = mMap; v.host = mHost;
		v.c = (int16_t)ActorSin(idx); v.s = (int16_t)ActorSin((idx + 0x100) & 0x3ff); v.rotate = rotate;
		v.ox = (uint16_t)(0 - (uint16_t)camX); v.oy = (uint16_t)(0 - (uint16_t)camY);
		for (size_t k = 0; k + 1 < out.size(); k++)
		{
			const int a = out[k].rootSector, b = out[k + 1].rootSector;
			const Sector &sa = mMap->sectors[a];
			for (int fi = sa.firstFaceIndex; fi >= 0 && fi < sa.firstFaceIndex + sa.faceCount; fi++)
			{
				const Face &f = mMap->faces[fi];
				if (f.sister < 0 || mMap->faces[f.sister].sector != b) continue;
				int32_t dA, xA, dB, xB;
				const Vertex &A = mMap->vertices[f.vertex1], &B = mMap->vertices[f.vertex2];
				v.Rot((int16_t)(uint16_t)((uint16_t)A.x + v.ox), (int16_t)(uint16_t)((uint16_t)A.y + v.oy), dA, xA);
				v.Rot((int16_t)(uint16_t)((uint16_t)B.x + v.ox), (int16_t)(uint16_t)((uint16_t)B.y + v.oy), dB, xB);
				if ((dA < 0x1000) == (dB < 0x1000) || dA == dB) continue;
				const int32_t cx = (int32_t)(((int64_t)(0x1000 - dA) * (xB - xA)) / (dB - dA)) + xA;
				int32_t col = (int32_t)(((int64_t)cx * base.xscale) / 0x1000) + base.xcenter;
				if (col <= out[k].left || col >= out[k + 1].right) continue;
				out[k].right = (int16_t)col;
				out[k + 1].left = (int16_t)col;
				break;
			}
		}
	}
	return out;
}

int ActorWorld::ViewIndex(uint8_t rotation, uint16_t viewAngle, int32_t depth, int32_t viewX)
{
	// cdiv: the object's view x over its depth, <<6 (renderer.c:5855-5856);
	// zero unless the depth is positive.
	const uint32_t cdiv = depth > 0 ? (uint32_t)((int32_t)((uint32_t)viewX << 6) / depth) : 0u;
	uint32_t t = (uint32_t)rotation * 2u + 0x120u;            // renderer.c:5863
	t = (uint16_t)(t - viewAngle);                           // 16-bit subtract
	t = t - cdiv;
	return (int)((t >> 5) & 0xeu);
}

uint16_t ActorWorld::CreaturePicture(ObjectUid uid, int ci, bool clearPain)
{
	Object *obj = Find(uid);
	return obj != nullptr ? CreaturePictureOf(*obj, ci, clearPain) : 0;
}

uint16_t ActorWorld::CreaturePictureOf(const Object &o, int ci, bool clearPain)
{
	const Object *obj = &o;
	if (mTables == nullptr) return 0;
	ci &= 0xe;
	const bool slotted = (obj->renderType & 1) && obj->unk0C != 0;
	if (!slotted)
	{
		// No slot (not woken, or the pool was full): the record's walking
		// picture for this view (renderer.c:5843-5852).
		const int c = mTables->CreatureIndexForId(ObjId(*obj));
		if (c < 0) return 0;
		return mTables->CreatureWord(c, 0x14 + ci);
	}
	ActorSlot &a = actors[(obj->unk0C - 1) / 0x22];
	const int c = a.creature;
	if (c < 0) return 0;
	auto B = [&](int off) { return mTables->CreatureWord(c, ci + off); };     // [base+ci+off]
	auto S = [&](int off) { return mTables->CreatureWord(c, off); };          // [base+off]
	if (a.pain != 0)                                         // renderer.c:12706-12710
	{
		const uint16_t walking = B(0x14), v = B(0x44);
		if (v == 0 || v == walking) { if (clearPain) a.pain = 0; return walking; }
		return v;
	}
	const uint8_t bl = a.state;
	if (bl & 0xfc)
	{
		if (bl & 0x20)                                       // dying (renderer.c:12715-12719)
		{
			if (bl & 1) { if (uint16_t v = S(0x58)) return v; }
			if (uint16_t v = S(0x54)) return v;
			return B(0x14);
		}
		if (bl & 0x40)                                       // rising / spawning (12720-12724)
		{
			if (uint16_t v = (bl & 1) ? S(0x5e) : S(0x5c)) return v;
			goto dispatch_default;
		}
		if (bl & 8)                                          // attacking (12725-12729)
		{
			if (bl & 1) { if (uint16_t v = B(0x34)) return v; }
			if (uint16_t v = B(0x24)) return v;
			return B(0x14);
		}
		if (bl & 0x80)                                       // 12730-12733
		{
			if (uint16_t v = B(0x44)) return v;
			return B(0x14);
		}
		if (bl == 5)                                         // 12734-12737
		{
			if (uint16_t v = S(0x62)) return v;
			return B(0x14);
		}
	}
dispatch_default:
	// flags & 0x20000: the third byte of the slot's flag dword (+0x0a), bit 1.
	if (a.move & 2) { if (uint16_t v = B(0x04)) return v; }  // 12740-12741
	return B(0x14);
}

std::vector<ActorWorld::DrawnObject> ActorWorld::DrawnPass(int *woke)
{
	std::vector<DrawnObject> out;
	if (woke) *woke = 0;
	if (mMap == nullptr || player.sector < 0) return out;
	const uint16_t va = (uint16_t)(0 - player.angle);
	std::vector<ObjectUid> seen;
	for (const RothViewParams &p : ViewRoots(player.x, player.y, va, player.sector))
	{
		const RothViewResult r = RenderView(p);
		for (size_t k = 0; k < r.resolved.size(); k++)
		{
			const ObjectUid uid = r.resolved[k];
			bool dup = false;
			for (ObjectUid u : seen) if (u == uid) { dup = true; break; }
			if (dup) continue;
			seen.push_back(uid);
			if (WakeOnFirstDraw(uid) && woke) (*woke)++;
			Object *obj = Find(uid);
			if (obj == nullptr) continue;
			DrawnObject d;
			d.uid = uid;
			d.viewIndex = r.viewIndex[k];
			const uint16_t cp = CreaturePicture(uid, d.viewIndex, true);
			// An object's own art word as a das id: source 0..3 name the map
			// pack's 0x1000 / 0x1100 and the shared pack's 0x1200 / 0x1300
			// pictures (Pack::ResolveObjectArt), so the id is 0x1000 + the
			// word's low ten bits -- the same numbering the creature records
			// use.
			d.picture = cp != 0 ? cp : (uint16_t)(0x1000 + (ObjId(*obj) & 0x3ff));
			if (mHost != nullptr)
			{
				const ActorHost::PictureTiming t = mHost->Timing((uint16_t)(d.picture & 0x7fff));
				if (t.ok && t.rle) AnimationDrawn(uid, d.picture & 0x7fff, t.ticksPerFrame, t.frameCount);
			}
			out.push_back(d);
		}
	}
	return out;
}

void ActorWorld::StepPictureClock(PictureClock &c, uint8_t rate, int frameCount, int scale)
{
	if (frameCount <= 1) { c.frame = 0; return; }
	if (c.frame < 0)                                         // das_assets.c:1455-1463
	{
		c.frame = 0;
		if (!(rate & 0x80)) { c.timer = rate; return; }
	}
	if (rate != 0xff)                                        // das_assets.c:1465-1474
	{
		c.timer = (uint8_t)(c.timer - (uint8_t)scale);
		if (c.timer < 0x10) return;
		const uint8_t t = (uint8_t)(c.timer + (uint8_t)((rate & 0x1f) + 1));
		c.timer = (t & 0x80) ? 0 : t;
	}
	c.frame = (c.frame + 1 >= frameCount) ? 0 : c.frame + 1; // das_assets.c:1476-1479
}

void ActorWorld::StepDrawnPictures(const std::vector<DrawnObject> &drawn, int scale)
{
	if (mHost == nullptr) return;
	std::vector<uint16_t> stepped;
	for (const DrawnObject &d : drawn)
	{
		const uint16_t das = (uint16_t)(d.picture & 0x7fff);
		bool done = false;
		for (uint16_t s : stepped) if (s == das) { done = true; break; }
		if (done) continue;
		stepped.push_back(das);
		const ActorHost::PictureTiming t = mHost->Timing(das);
		if (t.ok && !t.rle) StepPictureClock(pictureClocks[das], t.rate, t.frameCount, scale);
	}
}

int ActorWorld::PictureFrame(const Object &o, uint16_t das)
{
	if (mHost == nullptr) return 0;
	das &= 0x7fff;
	const ActorHost::PictureTiming t = mHost->Timing(das);
	const int count = t.frameCount;
	if (!t.ok || count <= 1) return 0;
	if (!t.rle)
	{
		auto it = pictureClocks.find(das);
		return (it == pictureClocks.end() || it->second.frame < 0) ? 0 : it->second.frame % count;
	}
	int counter = -1;
	if (o.unk0C != 0 && (o.renderType & 1))                  // render_world.c:416-420
	{
		const ActorSlot &a = actors[(o.unk0C - 1) / 0x22];
		if (a.state & 0x6c) counter = a.counter;
	}
	else if (o.unk0C != 0 && (o.renderType & 2))             // render_world.c:421-425
	{
		const FlyingSlot &f = flying[(o.unk0C - 1) / 0x1c];
		if (f.mode & 2) counter = f.owner;
	}
	if (counter >= 0)
	{
		const int frame = t.ticksPerFrame > 1 ? counter / t.ticksPerFrame : counter;
		return frame >= count ? count - 1 : frame;           // das_assets.c:1512-1513
	}
	if (t.ticksPerFrame == 0) return 0;                      // render_world.c:402-408
	return (int)(((frameClock & 0x7fff) / t.ticksPerFrame) % (uint32_t)count);
}

int ActorWorld::WakeFromPlayerView()
{
	if (mMap == nullptr || player.sector < 0) return 0;
	// g_sprite_view_angle is the player's angle negated (measured in the
	// running original: angle 128 renders with 0xff80).
	int woke = 0;
	for (const RothViewParams &p : ViewRoots(player.x, player.y, (uint16_t)(0 - player.angle), player.sector))
	{
		const RothViewResult r = RenderView(p);
		for (ObjectUid uid : r.resolved)
			if (WakeOnFirstDraw(uid)) woke++;
	}
	return woke;
}

} // namespace roth
