//
// Realms of the Haunting's actors: objects, the two pools, the commands that
// create and change them. See roth_actors.h for the layout and the sources.
//
// Engine-free. Every rule cites the ROTH.C line it was taken from; nothing here
// is a copy of that code.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//

#include "roth_actors.h"
#include "roth_raw.h"
#include "roth_dbase100.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace roth
{

namespace
{

inline uint16_t Rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t Rd32(const uint8_t *p) { return (uint32_t)Rd16(p) | ((uint32_t)Rd16(p + 2) << 16); }

// The object's +0x04 word.
inline uint16_t ObjId(const Object &o) { return (uint16_t)(o.textureIndex | (o.textureSource << 8)); }
inline void SetObjId(Object &o, uint16_t id) { o.textureIndex = (uint8_t)id; o.textureSource = (uint8_t)(id >> 8); }

//==========================================================================
//
// The 512-entry sine table (0x72080), amplitude 0x4000, and the 64-entry
// arctangent table the original builds from it at start-up (R:11333).
//
// The sine values are floor(sin * 0x4000 + 1/64) over the first quarter and
// mirrored; tools/actors_test compares all 512 words, and the 64 arctangent
// words, with the tables in the running original.
//
//==========================================================================

struct Trig
{
	int16_t sine[512];
	uint16_t atan[64];
	Trig()
	{
		const double step = 6.283185307179586476925 / 512.0;
		for (int i = 0; i <= 128; i++) sine[i] = (int16_t)(int)(sin(i * step) * 16384.0 + 1.0 / 64.0);
		for (int i = 129; i <= 256; i++) sine[i] = sine[256 - i];
		for (int i = 257; i < 512; i++) sine[i] = (int16_t)-sine[i - 256];
		// R:11333-11341: atan[i] = (0x4000 * sine[i]) / sine[i + 0x80], unsigned.
		for (int i = 0; i < 64; i++)
			atan[i] = (uint16_t)((0x4000u * (uint32_t)(uint16_t)sine[i]) / (uint16_t)sine[i + 0x80]);
	}
};

const Trig &TheTrig() { static const Trig t; return t; }

} // namespace

// sincos_lookup (R:113): the argument is a BYTE offset into the word table,
// masked even. Cosine is the same table 0x100 bytes on.
int32_t ActorSin(uint32_t byteOffset)
{
	return TheTrig().sine[(byteOffset & 0x3feu) >> 1];
}

// atan2_bearing (R:614-644). The bearing of (x1,y1) -> (x2,y2), 512 per turn.
// The argument order is the original's register order (AX, DX, BX, CX).
uint32_t ActorBearing(int16_t x1, int16_t y2, int16_t y1, int16_t x2)
{
	uint8_t oct = 0;
	uint16_t dx = (uint16_t)((uint16_t)x2 - (uint16_t)x1);
	if ((int16_t)dx < 0) { dx = (uint16_t)(0 - dx); oct |= 1; }
	uint16_t dy = (uint16_t)((uint16_t)y2 - (uint16_t)y1);
	if ((int16_t)dy < 0) { dy = (uint16_t)(0 - dy); oct |= 2; }
	if (!((int16_t)dx > (int16_t)dy)) { uint16_t t = dx; dx = dy; dy = t; oct |= 4; }
	uint16_t ratio = 0x4000;
	if (dx != 0) ratio = (uint16_t)((0x4000u * (uint32_t)dy) / dx);

	// The closest table entry; on a tie the later one wins (R:632-638).
	const uint16_t *tbl = TheTrig().atan;
	uint16_t best = 0x4000;
	int pick = 0x40;             // the countdown value at the chosen entry
	for (int i = 0; i < 0x40; i++)
	{
		uint16_t d = (uint16_t)(tbl[i] - ratio);
		if ((int16_t)d < 0) d = (uint16_t)(0 - d);
		if (!(d > best)) { pick = 0x40 - i; best = d; }
	}
	uint16_t a = (uint8_t)(0x40 - pick);
	if (!(oct & 4)) a = (uint16_t)((uint16_t)(0 - a) + 0x80);
	if (oct & 1) a = (uint16_t)(0 - a);
	if (oct & 2) a = (uint16_t)((uint16_t)(0 - a) + 0x100);
	return a & 0x1ffu;
}

// isqrt_fixed (R:169-188) and the 16-bit binary search it builds its table
// with (R:141-158).
int32_t ActorIsqrt(uint32_t v)
{
	static uint16_t table[0x300];
	static bool built = false;
	if (!built)
	{
		uint32_t x = 0x10000000u;
		for (int i = 0; i < 0x300; i++, x += 0x100000u)
		{
			uint32_t r = x;
			const uint16_t hi = (uint16_t)((uint16_t)(x >> 16) - (((uint16_t)x < 2) ? 1 : 0));
			if ((int16_t)hi >= 0)
			{
				uint16_t lo = 0, up = 0xfffe;
				do
				{
					const uint16_t mid = (uint16_t)((uint16_t)(lo + up) >> 1);
					if ((int32_t)((uint32_t)mid * mid - x) >= 0) up = mid; else lo = mid;
				} while ((int16_t)(uint16_t)(up - lo) > 1);
				r = up;
			}
			table[i] = (uint16_t)(r + r);
		}
		built = true;
	}
	if (v == 0) return 0;
	int bit = 31;
	while (!(v & (1u << bit))) bit--;
	const uint32_t shift = (uint32_t)(31 - bit) & 0xfeu;
	const uint32_t norm = v << shift;
	return (int32_t)((uint32_t)table[(norm >> 22) - 0x100u] >> (shift >> 1));
}

//==========================================================================
//
// Creature tables
//
//==========================================================================

// DAS:367-411. The header is 0x44 bytes: +4 version (5), +8 FAT offset, +0x0c
// palette offset (must be 0 in the shared pack), +0x24 the dimension table,
// +0x28 / +0x2c the creature records and their byte size, +0x34 / +0x36 the two
// entry counts.
CreatureTables LoadCreatureTables(const uint8_t *d, size_t size)
{
	CreatureTables t;
	if (d == nullptr || size < 0x44) { t.error = "shared pack shorter than its header"; return t; }
	if (Rd16(d + 4) != 5) { t.error = "shared pack is not version 5"; return t; }
	if (Rd32(d + 0x0c) != 0) { t.error = "shared pack carries a palette; not the shared pack"; return t; }
	const uint32_t na = Rd16(d + 0x34);
	const uint32_t nb = (uint16_t)(na + Rd16(d + 0x36));
	const uint32_t fatOff = Rd32(d + 8), fatBytes = nb * 8;
	const uint32_t recOff = Rd32(d + 0x28), recBytes = Rd32(d + 0x2c);
	const uint32_t dimOff = Rd32(d + 0x24);
	if ((uint64_t)fatOff + fatBytes > size || (uint64_t)recOff + recBytes > size ||
		(uint64_t)dimOff + na * 4 > size)
	{ t.error = "shared pack tables run past the end of the file"; return t; }
	t.fat.assign(d + fatOff, d + fatOff + fatBytes);
	t.records.assign(d + recOff, d + recOff + recBytes);
	// The buffer is indexed by id * 4; the shared pack's entries start at
	// 0x800, id 0x200 (DAS:401).
	t.dims.assign(0x800 + (size_t)na * 4, 0);
	if (na != 0) memcpy(t.dims.data() + 0x800, d + dimOff, (size_t)na * 4);
	t.ok = true;
	return t;
}

// map_load.c:382: 0x800 bytes at the map pack's +0x24 offset, ids 0..0x1ff.
void LoadMapDims(CreatureTables &t, const uint8_t *d, size_t size)
{
	if (t.dims.size() < 0x800) t.dims.resize(0x800, 0);
	memset(t.dims.data(), 0, 0x800);
	if (d == nullptr || size < 0x44) return;
	const uint32_t off = Rd32(d + 0x24);
	if (off == 0 || (uint64_t)off + 0x800 > size) return;
	memcpy(t.dims.data(), d + off, 0x800);
}

int CreatureTables::CreatureIndexForId(uint16_t id) const
{
	if (id < 0x200) return -1;                               // DAS:211
	const size_t e = (size_t)(id - 0x200) * 8;
	if (e + 8 > fat.size()) return -1;
	if (fat[e + 6] != 0x24) return -1;                       // DAS:215
	return fat[e + 7];
}

uint16_t CreatureTables::CreatureWord(int creature, int off) const
{
	const size_t p = (size_t)creature * 0x68 + (size_t)off;
	if (creature < 0 || p + 2 > records.size()) return 0;
	return Rd16(records.data() + p);
}

uint16_t CreatureTables::DimHeight(uint16_t id) const
{
	const size_t p = (size_t)id * 4;
	return p + 2 <= dims.size() ? Rd16(dims.data() + p) : 0;
}

uint16_t CreatureTables::DimRadius(uint16_t id) const
{
	const size_t p = (size_t)id * 4 + 2;
	return p + 2 <= dims.size() ? Rd16(dims.data() + p) : 0;
}

//==========================================================================
//
// Begin / End
//
//==========================================================================

void ActorWorld::Begin(Map *map, const Dbase100 *db, const CreatureTables *tables, ActorHost *host)
{
	mMap = map;
	mDb = db;
	mTables = tables;
	mHost = host != nullptr ? host : &mNullHost;

	mUids.clear();
	mNextUid = 0x80000000u;
	mPictureLast.clear();
	pictureClocks.clear();
	frameClock = 0;
	if (map != nullptr)
	{
		mUids.resize(map->objects.size());
		for (size_t s = 0; s < map->objects.size(); s++)
		{
			mUids[s].resize(map->objects[s].size());
			for (size_t i = 0; i < map->objects[s].size(); i++)
			{
				mUids[s][i] = (((uint32_t)s << 16) | (uint32_t)i) + 1;
				map->objects[s][i].unk0C = 0;                // RC:3403-3431
			}
		}
	}
	// The load pass over every command record (map_load.c:1179-1193): a
	// trigger of type 0x1b / 0x30 / 0x39 stamps its bit on every object whose
	// command id is its key (RC:5051-5064; key 0 is skipped, and a disabled
	// record is NOT). At most 0xc8 objects a record.
	if (map != nullptr)
		for (const Command &c : map->commands)
		{
			const uint8_t type = c.opcode & 0x7f;
			const uint8_t bit = type == 0x1b ? 0x20 : type == 0x30 ? 0x08 : type == 0x39 ? 0x04 : 0;
			if (bit == 0 || c.key == 0) continue;
			int n = 0;
			for (auto &list : map->objects)
				for (Object &o : list)
					if (o.commandID == c.key && n < 0xc8) { o.renderType |= bit; n++; }
		}

	// 0x35 records armed at load register a damage emitter (map_load.c:1193 ->
	// RC:5511-5529). Each is pushed on the FRONT of the list (RC:5533-5545),
	// so the walk meets the last record first. One with no matching face
	// registers nothing.
	mEmitters.clear();
	if (map != nullptr)
		for (size_t i = 0; i < map->commands.size(); i++)
		{
			const Command &c = map->commands[i];
			if ((c.opcode & 0x7f) != 0x35 || !(c.modifier & 0x21)) continue;
			bool any = false;
			for (const TextureMap &tm : map->textureMaps)
				if (tm.extended && tm.faceID == c.key) { any = true; break; }
			if (!any) continue;
			mEmitters.insert(mEmitters.begin(), (int)i);
		}

	for (ActorSlot &a : actors) a = ActorSlot();
	for (FlyingSlot &f : flying) f = FlyingSlot();
	actorCount = flyingCount = 0;
	creatureRt.assign(tables != nullptr ? (size_t)tables->CreatureCount() : 0, CreatureRuntime());
	mSource = 0;
	mSourceSynthetic = false;
	chainInterrupt = 0;
}

void ActorWorld::End()
{
	mMap = nullptr;
	mUids.clear();
	actorCount = flyingCount = 0;
	for (ActorSlot &a : actors) a = ActorSlot();
	for (FlyingSlot &f : flying) f = FlyingSlot();
}

uint16_t ActorWorld::SectorOffset(int sector) const
{
	if (mMap == nullptr || sector < 0) return 0;
	return (uint16_t)(mMap->header.sectorsOffset + sector * 0x1a);
}

//==========================================================================
//
// Objects
//
//==========================================================================

Object *ActorWorld::Find(ObjectUid uid, int *sector, int *index)
{
	if (uid == 0 || mMap == nullptr) return nullptr;
	for (size_t s = 0; s < mUids.size(); s++)
		for (size_t i = 0; i < mUids[s].size(); i++)
			if (mUids[s][i] == uid)
			{
				if (sector) *sector = (int)s;
				if (index) *index = (int)i;
				return &mMap->objects[s][i];
			}
	return nullptr;
}

ObjectUid ActorWorld::UidAt(int sector, int index) const
{
	if (sector < 0 || (size_t)sector >= mUids.size()) return 0;
	if (index < 0 || (size_t)index >= mUids[sector].size()) return 0;
	return mUids[sector][index];
}

// alloc_object_record_slot (RC:3799-3833): the new record goes at the END of
// the sector's list. The arena's free-space bookkeeping is not modelled: a
// std::vector cannot run out the way a fixed buffer does.
ObjectUid ActorWorld::AppendObject(int sector, const Object &o)
{
	if (mMap == nullptr || sector < 0 || (size_t)sector >= mMap->objects.size()) return 0;
	mMap->objects[sector].push_back(o);
	const ObjectUid uid = mNextUid++;
	mUids[sector].push_back(uid);
	mHost->ListsChanged();
	return uid;
}

// remove_secondary_state_record (RC:3703-3741): the records after it move down
// one place, keeping their order.
void ActorWorld::RemoveObject(int sector, int index)
{
	mMap->objects[sector].erase(mMap->objects[sector].begin() + index);
	mUids[sector].erase(mUids[sector].begin() + index);
	mHost->ListsChanged();
}

// resolve_command_objects (RC:317-352).
std::vector<ObjectUid> ActorWorld::ResolveCommandObjects(uint16_t key, int cap)
{
	std::vector<ObjectUid> out;
	if (mMap == nullptr) return out;
	if (key == 0)
	{
		// RC:320-325. The synthetic source is not an object in any list; the
		// original would hand back an offset outside the arena. No retail chain
		// reaches a key-0 command with it set (it is only left behind by the
		// projectile path of 0x3c), so it resolves to nothing here.
		if (mSource != 0 && !mSourceSynthetic) out.push_back(mSource);
		return out;
	}
	for (size_t s = 0; s < mMap->objects.size(); s++)
		for (size_t i = 0; i < mMap->objects[s].size(); i++)
			if (mMap->objects[s][i].commandID == key)
			{
				out.push_back(mUids[s][i]);
				if ((int)out.size() >= cap) return out;      // RC:345
			}
	return out;
}

// mark_object_trigger_links (RC:456-472). The opcode byte is compared whole,
// with no 0x7f mask.
void ActorWorld::MarkTriggerLinks(Object &o, uint16_t id) const
{
	for (const Command &c : mMap->commands)
	{
		if (c.key != id) continue;
		if (c.opcode == 0x1b) o.renderType |= 0x20;
		else if (c.opcode == 0x30) o.renderType |= 0x08;
		else if (c.opcode == 0x39) o.renderType |= 0x04;
	}
}

// compute_player_object_bearing (RC:300-308): the bearing from the player to
// the point, halved to a rotation byte.
PunchAim PunchAimAt(const PlayerView &p, int16_t x, int16_t y, int32_t z)
{
	PunchAim a;
	a.bearing = ActorBearing(p.x, y, p.y, x);
	// NOT wrapped: FirePlayer's angle source is the raw difference (WC:670-671, `sub ebp,[esp+0x38]` on the
	// zero-extended angle), and spawn_entity_and_fill negates it and decrements its second byte (EA:838), so
	// 512 and 0 are different shots. angle - (angle - bearing) = the bearing exactly, as the original's own
	// attack from Adam facing the target (tools/actors_test punch replay: a wrapped offset differed there).
	a.yawOffset = (int32_t)p.angle - (int32_t)a.bearing;
	int32_t shotZ = p.z;                                     // WC:664-667, as FirePlayer
	const int16_t over = (int16_t)(p.height - 0x14);
	if (over >= 0) shotZ += over;
	const int32_t dx = (int32_t)x - p.x, dy = (int32_t)y - p.y;
	int32_t run = ActorIsqrt((uint32_t)(dx * dx + dy * dy));
	if (run < 1) run = 1;
	int32_t pitch = (int32_t)(((int64_t)(z - shotZ) * 128) / run);
	if (pitch > 126) pitch = 126;
	if (pitch < -126) pitch = -126;
	a.pitch = pitch;
	return a;
}

uint8_t ActorWorld::PlayerBearing(int16_t x, int16_t y) const
{
	return (uint8_t)((int32_t)ActorBearing(player.x, y, player.y, x) >> 1);
}

//==========================================================================
//
// Where a point is. scan_sector_edges_at_y (CP:91-170) and the three searches
// built on it (CP:906-1028).
//
//==========================================================================

bool ActorWorld::InsideSector(int sector, int16_t qx, int16_t qy) const
{
	const Sector &sec = mMap->sectors[sector];
	int16_t maxX = (int16_t)0x8000, minX = 0x7fff;
	int8_t n = (int8_t)sec.faceCount;
	int fi = sec.firstFaceIndex;
	if (fi < 0) return false;
	do
	{
		if ((size_t)fi >= mMap->faces.size()) break;
		const Face &f = mMap->faces[fi];
		if (f.vertex1 >= 0 && f.vertex2 >= 0)
		{
			const Vertex &A = mMap->vertices[f.vertex1], &B = mMap->vertices[f.vertex2];
			const int16_t da = (int16_t)(A.y - qy);
			bool crossed = false;
			int16_t cx = 0;
			if (da == 0) { cx = A.x; crossed = true; }           // CP:120
			else if (da > 0)
			{
				const int16_t db = (int16_t)(B.y - qy);
				if (db < 0)
				{
					const int16_t span = (int16_t)(da - db), num = (int16_t)(qy - B.y);
					const int16_t xs = (int16_t)(A.x - B.x);
					cx = (int16_t)((int16_t)(((int32_t)num * xs) / span) + B.x);
					crossed = true;
				}
			}
			else
			{
				const int16_t db = (int16_t)(B.y - qy);
				if (db > 0)
				{
					const int16_t span = (int16_t)(db - da), num = (int16_t)(qy - A.y);
					const int16_t xs = (int16_t)(B.x - A.x);
					cx = (int16_t)((int16_t)(((int32_t)num * xs) / span) + A.x);
					crossed = true;
				}
			}
			if (crossed)
			{
				if (cx >= maxX) maxX = cx;
				if (cx <= minX) minX = cx;
			}
		}
		fi++;
	} while (--n > 0);                                       // dec cl; jg
	if (minX > qx) return false;
	if (maxX < qx) return false;
	return true;
}

int ActorWorld::LocateSector(int16_t x, int16_t y, int16_t z, int hint) const
{
	if (mMap == nullptr) return -1;
	const int count = (int)mMap->sectors.size();
	if (hint >= 0 && hint < count)                           // CP:1013-1016
	{
		if (InsideSector(hint, x, y)) return hint;
		// search_sector_neighbors (CP:919-946): each wall with a far side.
		const Sector &sec = mMap->sectors[hint];
		int8_t n = (int8_t)sec.faceCount;
		int fi = sec.firstFaceIndex;
		if (fi >= 0) do
		{
			if ((size_t)fi >= mMap->faces.size()) break;
			const Face &f = mMap->faces[fi];
			if (f.sister >= 0)
			{
				const int nb = mMap->faces[f.sister].sector;
				if (nb >= 0 && InsideSector(nb, x, y)) return nb;
			}
			fi++;
		} while (--n > 0);
	}
	// search_sector_global (CP:953-980): the first sector that holds the point
	// in plan AND in height; failing that, the LAST one that held it in plan.
	int best = -1;
	for (int s = 0; s < count; s++)
	{
		if (!InsideSector(s, x, y)) continue;
		best = s;
		const uint16_t uz = (uint16_t)z;
		if (!(uz > (uint16_t)mMap->sectors[s].ceilingHeight) && !(uz < (uint16_t)mMap->sectors[s].floorHeight))
			return s;
	}
	return best;
}

//==========================================================================
//
// Statistics
//
//==========================================================================

// lookup_dbase100_record_by_id (R:10659-10672) then build_entity_def_record
// (DB:117-180).
ActorDef ActorWorld::BuildDef(uint32_t id) const
{
	ActorDef def;
	memset(def.b, 0, sizeof def.b);
	if (mDb == nullptr || !mDb->IsLoaded() || id < 0x200) return def;

	const uint8_t *img = mDb->Image();
	const size_t size = mDb->Size();
	const uint8_t *rec = nullptr;
	for (int i = 1; i <= mDb->ItemCount(); i++)
	{
		const DbItem *it = mDb->ItemSlot(i);
		if (it == nullptr || (size_t)it->offset + 0x18 > size) continue;
		if ((int32_t)(int16_t)Rd16(img + it->offset + 2) == (int32_t)id) { rec = img + it->offset; break; }
	}
	if (rec == nullptr) return def;

	def.b[4] = rec[2]; def.b[5] = rec[3];                    // DB:123
	const uint8_t *blk = rec + 0x14;
	const uint8_t *end = img + size;
	// A write the original makes without a bound. Inside the record it lands
	// where the original's does; past the end it is dropped (there it would
	// have gone into the caller's stack frame).
	auto put16 = [&](size_t off, uint16_t v) { if (off + 2 <= sizeof def.b) { def.b[off] = (uint8_t)v; def.b[off + 1] = (uint8_t)(v >> 8); } };
	for (;;)
	{
		if (blk + 4 > end) return def;
		const uint32_t hdr = Rd32(blk);
		const uint32_t bytes = hdr & 0x00ffffffu;
		if (bytes == 0) return def;                          // no AsMonster block
		const uint32_t words = bytes >> 2;
		if ((hdr >> 24) != 0x0a) { blk += (size_t)words << 2; continue; }
		blk += 4;
		for (int32_t i = 0; i < (int32_t)words - 1; i++, blk += 4)
		{
			if (blk + 4 > end) break;
			const uint32_t w = Rd32(blk);
			const uint32_t top = w >> 24, operand = w & 0x00ffffffu;
			const bool sec = (top & 0x80) != 0;
			const uint16_t ax = (uint16_t)operand;
			switch (top & 0x7f)
			{
			case 0x24: put16(0x0c, ax); break;
			case 0x25: put16(0x0e, ax); break;
			case 0x26: put16(0x08, ax); put16(0x0a, (uint16_t)(operand >> 16)); break;
			case 0x27:
			{
				const size_t cnt = sec ? 0x46 : 0x2c;
				const uint16_t k = def.W((int)cnt);
				put16(cnt + 2 + (size_t)k * 2, ax);
				put16(cnt, (uint16_t)(k + 1));
				break;
			}
			case 0x28: put16(sec ? 0x12 : 0x10, ax); break;
			case 0x29:
			{
				const size_t cnt = sec ? 0x22 : 0x18;
				const uint16_t k = def.W((int)cnt);
				put16(cnt + 2 + (size_t)k * 2, ax);
				put16(cnt, (uint16_t)(k + 1));
				break;
			}
			case 0x2a: put16(sec ? 0x16 : 0x14, ax); break;
			case 0x2b: def.b[sec ? 0x07 : 0x06] = (uint8_t)operand; break;
			case 0x2c: def.b[0x6a] = (uint8_t)operand; break;
			case 0x2d:
				if (operand == 2) def.b[0x69] |= 2;
				else if (operand == 3) def.b[0x69] |= 4;
				break;
			case 0x30: def.b[0x68] = (uint8_t)operand; break;
			case 0x31: put16(sec ? 0x62 : 0x60, ax); break;
			case 0x32: put16(sec ? 0x66 : 0x64, ax); break;
			default: break;
			}
		}
		def.valid = true;
		return def;
	}
}

// entity_def_cache_lookup (EA:645-692). The key compare is the node's 16-bit
// id against the whole 32-bit argument.
int ActorWorld::LookupDef(uint32_t id)
{
	int prev = -1;
	for (int cur = mDefHead; cur >= 0; prev = cur, cur = mDefNext[cur])
	{
		if ((uint32_t)mDefNodes[cur].Id() != id) continue;
		if (prev >= 0)                                       // move to the front
		{
			mDefNext[prev] = mDefNext[cur];
			mDefNext[cur] = mDefHead;
			mDefHead = cur;
		}
		return cur;
	}
	ActorDef built = BuildDef(id);
	if (!built.valid) return -1;                             // EA:673: nothing is cached
	int node;
	if (mDefCount < 10) node = mDefCount++;
	else
	{
		// Evict the tail (EA:683-690).
		int p = mDefHead, before = -1;
		while (mDefNext[p] >= 0) { before = p; p = mDefNext[p]; }
		mDefNext[before] = -1;
		node = p;
	}
	mDefNodes[node] = built;
	mDefNext[node] = mDefHead;
	mDefHead = node;
	return node;
}

// init_inventory_item_object (INV:147-190). The flag tested is the item's +4
// byte bit 0x20 and the block is trigger code 6.
BulletDesc ActorWorld::BulletFor(uint32_t item) const
{
	BulletDesc d;
	if (mDb == nullptr || !mDb->IsLoaded()) return d;
	const DbItem *it = mDb->ItemSlot((int)item);
	const uint8_t *img = mDb->Image();
	const size_t size = mDb->Size();
	if (it == nullptr || (size_t)it->offset + 0x18 > size) return d;
	const uint8_t *rec = img + it->offset;
	if (!(rec[4] & 0x20)) return d;
	const uint8_t *blk = rec + 0x14, *end = img + size;
	for (;;)
	{
		if (blk + 4 > end) return d;
		const uint32_t hdr = Rd32(blk);
		const uint32_t bytes = hdr & 0x00ffffffu;
		if (bytes == 0) return d;
		const int32_t words = (int32_t)(bytes >> 2);
		if (((int32_t)hdr >> 24) != 6) { blk += (size_t)words << 2; continue; }
		blk += 4;
		for (int32_t i = 0; i < words - 1 && blk + 4 <= end; i++, blk += 4)
		{
			const uint32_t e = Rd32(blk), v = e & 0x00ffffffu;
			switch ((e >> 24) & 0x7f)
			{
			case 0x15: d.damage = (uint16_t)v; break;
			case 0x16: d.speed = (uint8_t)v; break;
			case 0x17: d.hitAnim = (uint16_t)v; break;
			case 0x19: d.hitSound = (uint16_t)v; break;
			case 0x2e: d.ttl = (uint8_t)v; break;
			case 0x2f: d.width = (uint8_t)v; break;
			case 0x30: d.flash = (uint8_t)v; break;
			default: break;
			}
		}
		d.templateId = (int32_t)(int16_t)Rd16(rec + 2);
		return d;
	}
}

// resolve_object_template_record (RC:3612-3633).
int32_t ActorWorld::ResolveTemplate(uint32_t item, uint32_t &flags) const
{
	if (item == 0) item = (uint32_t)(int32_t)(int16_t)mHost->SelectedItem();   // RC:3614-3617
	lastItemRecord = item;
	if (item == 0 || mDb == nullptr || !mDb->IsLoaded()) return 0;
	if (item > mDb->Header().inventoryCount) return 0;
	const DbItem *it = mDb->ItemSlot((int)item);
	if (it == nullptr || (size_t)it->offset + 8 > mDb->Size()) return 0;
	const uint8_t *rec = mDb->Image() + it->offset;
	flags = Rd32(rec + 4);
	const int32_t t = (int16_t)Rd16(rec + 2);
	return t < 0x200 ? 0 : t;
}

//==========================================================================
//
// The pools
//
//==========================================================================

// spawn_entity_into_state_pool_a (EA:857-892). Only the fields the original
// writes are written: a reused slot keeps whatever the last occupant left in
// the others.
int ActorWorld::SpawnActor(int creature, ObjectUid uid)
{
	if (actorCount == 0x10) return 0;
	int slot = 0;
	while (actors[slot].object != 0) { if (++slot >= 0x10) return 0; }
	Object *obj = Find(uid);
	int sector = -1;
	Find(uid, &sector);
	if (obj == nullptr) return 0;

	ActorSlot &a = actors[slot];
	const int def = LookupDef(ObjId(*obj));
	a.creature = creature;
	if (creature >= 0 && (size_t)creature < creatureRt.size())
	{
		creatureRt[creature].def = def;
		creatureRt[creature].defId = ObjId(*obj);            // EA:880
	}
	a.bits = 0;
	a.health = 0;
	if (def >= 0)
	{
		a.bits = mDefNodes[def].ModeBits();
		a.health = (int32_t)mDefNodes[def].Health();
	}
	a.object = uid;
	a.sector = sector;                                       // EA:883: the list it is in
	a.state = 0;
	a.bullet = 0;
	obj->unk0C = (uint16_t)ActorBackref(slot);
	obj->renderType |= 1;
	actorCount++;
	return ActorBackref(slot);
}

// R:5826-5840.
bool ActorWorld::WakeOnFirstDraw(ObjectUid uid)
{
	Object *obj = Find(uid);
	if (obj == nullptr || mTables == nullptr) return false;
	if (obj->flags & 0x80) return false;                     // hidden: never reaches the rasteriser (R:8035)
	if (obj->unk0C != 0) return false;
	const int creature = mTables->CreatureIndexForId(ObjId(*obj));
	if (creature < 0) return false;
	return SpawnActor(creature, uid) != 0;
}

// spawn_object_from_das_resource (DAS:208-249).
bool ActorWorld::SpawnFromResource(ObjectUid uid, uint16_t id, int variant)
{
	if (mTables == nullptr) return false;
	const int creature = mTables->CreatureIndexForId(id);
	if (creature < 0) return false;
	const int backref = SpawnActor(creature, uid);
	if (backref == 0) return false;
	ActorSlot &a = actors[(backref - 1) / 0x22];
	Object *obj = Find(uid);
	a.settle = 0;
	if (variant == 0)                                        // emerges from below (DAS:232-236)
	{
		obj->z = (int16_t)(obj->z - 0x50);
		a.settle = 0x50;
		obj->flags |= 0x40;
	}
	a.counter = 0;
	// DAS:239: a creature with a spawn picture starts in the stagger state.
	if (mTables->CreatureWord(creature, 0x5c) > 0x200) a.state |= 0x44;
	// DAS:240-242. The original reads the sound through the statistics pointer
	// without testing it; with none it reads low memory. Here: no statistics,
	// no sound.
	const ActorDef *def = Def((size_t)creature < creatureRt.size() ? creatureRt[creature].def : -1);
	if (def != nullptr && def->SpawnSound() != 0)
		mHost->SoundAt(def->SpawnSound() - 1, (uint16_t)obj->x, (uint16_t)obj->y);
	obj->rotation = PlayerBearing(obj->x, obj->y);           // DAS:247
	mHost->ObjectChanged(uid);
	return true;
}

// destroy_dynamic_entity (EA:739-800).
void ActorWorld::DestroyObject(ObjectUid uid)
{
	int sector = -1, index = -1;
	Object *obj = Find(uid, &sector, &index);
	if (obj == nullptr) return;
	if (obj->renderType & 2)
	{
		const int slot = ((int)obj->unk0C - 1);
		if (slot >= 0)
		{
			obj->unk0C = 0;
			flying[slot / 0x1c].object = 0;
			flyingCount--;
		}
	}
	if (obj->renderType & 1)
	{
		obj->renderType -= 1;
		const int slot = ((int)obj->unk0C - 1);
		if (slot >= 0)
		{
			obj->unk0C = 0;
			actors[slot / 0x22].object = 0;
			actors[slot / 0x22].creature = -1;
			actorCount--;
		}
	}
	mHost->ObjectDestroyed(uid);
	RemoveObject(sector, index);
}

// spawn_entity_at_position (EA:808-850).
int ActorWorld::SpawnFlying(uint16_t x, uint16_t y, uint16_t z, uint16_t id, uint32_t angleSource, int32_t zVelocity)
{
	if (flyingCount >= 0x10) return -1;
	const int sector = LocateSector((int16_t)x, (int16_t)y, (int16_t)z, -1);
	if (sector < 0) return -1;
	int slot = 0;
	while (flying[slot].object != 0) { if (++slot >= 0x10) { slot = 0; break; } }   // R:421-431

	Object o = Object();
	o.x = (int16_t)x; o.y = (int16_t)y; o.z = (int16_t)z;
	SetObjId(o, id);
	o.flags = 2;
	o.renderType = 2;
	o.commandID = 0;
	o.light = 0x80;
	o.unk0C = (uint16_t)FlyingBackref(slot);
	// EA:838: the angle is the negated source with its second byte decremented.
	uint32_t ang = 0u - angleSource;
	ang = (ang & 0xffff00ffu) | ((((ang >> 8) + 0xffu) & 0xffu) << 8);
	o.rotation = (uint8_t)((int32_t)ang >> 1);

	const ObjectUid uid = AppendObject(sector, o);
	if (uid == 0) return -1;
	FlyingSlot &f = flying[slot];
	flyingCount++;
	f.sector = sector;
	f.angle = (uint16_t)ang;
	f.zVelocity = zVelocity;
	f.object = uid;
	f.speed = 8;
	f.fracX = f.fracY = 0;
	mHost->ObjectCreated(uid);
	return slot;
}

// spawn_object_projectile_at_player (WC:682-721) with the shared tail
// (WC:590-610). zOffset is the original's 0x90fcc: 0x40 for a creature's shot
// (EA:1220), 0 for a command's.
int ActorWorld::FireFrom(uint16_t item, const Object &src, int zOffset, bool playerFire)
{
	const BulletDesc d = BulletFor(item);
	if (d.templateId == 0) return -1;

	const uint32_t angleSource = 0u - (((uint32_t)src.rotation + 0x80u) * 2u);
	int32_t zvel = 0;
	if (!playerFire)
	{
		// WC:700-712: aim up or down at the player when the height difference
		// is more than 0x10.
		int16_t v = (int16_t)((uint16_t)player.z + player.height - 0x20 - (uint16_t)src.z);
		const int32_t vd = (int16_t)(uint16_t)((int32_t)v - zOffset);
		if (vd > 0x10 || vd <= -0x10)
		{
			const int32_t dx = (int32_t)src.x - player.x, dy = (int32_t)src.y - player.y;
			const int32_t r = ActorIsqrt((uint32_t)(dx * dx + dy * dy));
			// The original divides by r unguarded; a shot fired from the
			// player's own position would fault there.
			zvel = (int32_t)((uint32_t)vd << 7) / (r == 0 ? 1 : r);
		}
	}
	const int slot = SpawnFlying((uint16_t)src.x, (uint16_t)src.y, (uint16_t)((uint16_t)src.z + zOffset),
		(uint16_t)d.templateId, angleSource, zvel);
	if (slot < 0) return -1;
	FlyingSlot &f = flying[slot];
	if (Object *obj = Find(f.object)) obj->light = 0;        // WC:600
	f.mode |= 1;
	f.owner = src.unk0C;                                     // WC:686: the shooter's back-reference
	f.damageType = item;
	f.damage = d.damage;
	f.width = d.width;
	f.speed = d.speed;
	f.ttl = d.ttl;
	return slot;
}

int ActorWorld::FirePlayer(uint16_t item, int32_t yawOffset)
{
	const BulletDesc d = BulletFor(item);
	if (d.templateId == 0) return -1;
	uint16_t z = (uint16_t)player.z;                         // WC:664-667
	const int16_t over = (int16_t)(player.height - 0x14);
	if (over >= 0) z = (uint16_t)(z + over);
	const uint32_t angleSource = (uint32_t)player.angle - (uint32_t)yawOffset;
	const int32_t zvel = player.pitch != 0 ? player.pitch : player.pitchApplied;   // WC:672-673
	const int slot = SpawnFlying((uint16_t)player.x, (uint16_t)player.y, z, (uint16_t)d.templateId, angleSource, zvel);
	if (slot < 0) return -1;
	FlyingSlot &f = flying[slot];
	if (Object *obj = Find(f.object)) obj->light = 0;
	f.mode |= 1;
	f.owner = 0;                                             // WC:653
	f.damageType = item;
	f.damage = d.damage;
	f.width = d.width;
	f.speed = d.speed;
	f.ttl = d.ttl;
	return slot;
}

//==========================================================================
//
// 0x16 -- cmd_spawn_object (RC:622-667)
//
// Despite the name it toggles the HIDDEN bit of every object carrying the key.
// An object that already has an actor and is visible is not hidden: its actor
// is marked "died this tick" instead, unless it is already a corpse.
//
//   +0x06 bit 0x02  force visible     bit 0x04 (without 0x02)  force hidden
//         bit 0x20  the record is NOT disabled afterwards
//
//==========================================================================

int ActorWorld::CmdSpawnObject(Command &rec)
{
	const std::vector<ObjectUid> list = ResolveCommandObjects(rec.key);
	if (list.empty()) return ACTCMD_NOTHING;
	const uint8_t flags = rec.Byte(6);
	if (mSource == 0) { mSource = list[0]; mSourceSynthetic = false; }   // RC:630

	for (ObjectUid uid : list)
	{
		Object *o = Find(uid);
		if (o == nullptr) continue;
		if (o->renderType & 1)
		{
			bool toggle;
			if (flags & 0x06) toggle = (flags & 0x02) ? true : (o->flags & 0x80) != 0;
			else toggle = (o->flags & 0x80) != 0;
			if (!toggle && o->unk0C != 0)
			{
				ActorSlot &a = actors[(o->unk0C - 1) / 0x22];
				if (!(a.state & 0x20)) a.bits |= 1;          // RC:649
				continue;
			}
		}
		o->flags ^= 0x80;
		if (flags & 0x06)
		{
			o->flags |= 0x80;
			if (!(flags & 0x04)) o->flags &= 0x7f;
		}
		mHost->ObjectChanged(uid);
	}
	if (!(rec.Byte(6) & 0x20)) { rec.modifier |= 0x08; rec.disabled = true; }
	return ACTCMD_ACTED;
}

//==========================================================================
//
// 0x3a -- cmd_change_object_id (RC:479-496)
//
// Gives the command source object a new command id: the record's +0x06 WORD,
// or a freshly minted 0xc1nn when that is 0xffff. The object's trigger-link
// bits are rebuilt for the new id.
//
//==========================================================================

int ActorWorld::CmdChangeObjectId(Command &rec)
{
	lastMintedId = 0;
	if (mSource == 0) return ACTCMD_NOTHING;
	uint16_t id = rec.Word(6);
	if (id == 0xffff)
	{
		uint32_t counter = nextObjectId + 1u;
		counter = (counter & 0xffff00ffu) | 0xc100u;
		nextObjectId = counter;
		lastMintedId = counter;
		id = (uint16_t)counter;
	}
	// RC:491: the source pointer is used as it stands. The synthetic record at
	// the player's feet has the same fields and no visible effect.
	Object *o = mSourceSynthetic ? nullptr : Find(mSource);
	if (o != nullptr)
	{
		o->renderType &= 0xd3;
		o->commandID = id;
		if (id != 0) MarkTriggerLinks(*o, id);
	}
	return ACTCMD_ACTED;
}

//==========================================================================
//
// 0x3c -- cmd_spawn_object_adv (RC:4108-4219)
//
//   +0x08 key       the source: the first object with this command id, or
//                   0xffff for the player's own position
//   +0x0a item      a DBASE100 item; its +2 word is the art the object becomes
//                   and its flags pick the path
//   +0x0c           the command id the object is given
//   +0x0e           a sound for the chain to play at the object
//   +0x06 bit 0x02  re-use the source object instead of making a new one
//         bit 0x04  a failure does not stop the chain
//         bit 0x10  disable the record afterwards
//         bit 0x40  (bullet path) turn the shot toward the player
//
// Item flag 0x20 (has a bullet block): fire it from the source and make no
// object. Item flag 0x80: the new object is a creature and comes alive at once.
//
//==========================================================================

int ActorWorld::CmdSpawnObjectAdv(Command &rec)
{
	Object synthetic = Object();
	Object *src = nullptr;
	ObjectUid srcUid = 0;
	bool isSynthetic = false;

	if (rec.key == 0xffff)                                   // RC:4114-4122
	{
		synthetic.x = player.x; synthetic.y = player.y; synthetic.z = player.z;
		synthetic.flags = 0;
		SetObjId(synthetic, 0x200);
		rec.fireFlags = (uint8_t)(rec.Byte(6) & 0xfd);
		rec.SetByte(6, rec.fireFlags);
		src = &synthetic;
		isSynthetic = true;
	}
	else
	{
		const std::vector<ObjectUid> list = ResolveCommandObjects(rec.key);
		if (!list.empty()) { srcUid = list[0]; src = Find(srcUid); }
	}

	uint32_t tflags = 0;
	const int32_t tmpl = src != nullptr ? ResolveTemplate(rec.Word(0x0a), tflags) : 0;
	if (src == nullptr || tmpl == 0)
	{
		if (!(rec.Byte(6) & 0x04)) chainInterrupt = 2;       // RC:4217
		return ACTCMD_NOTHING;
	}
	const uint8_t flags = rec.Byte(6);
	const uint16_t sound = rec.Word(0x0e), newId = rec.Word(0x0c);

	if (tflags & 0x20)                                       // the bullet path (RC:4133-4158)
	{
		mSource = srcUid; mSourceSynthetic = isSynthetic;
		if (sound != 0) mHost->ChainSound(sound, src->x, src->y);
		Object desc = *src;
		desc.unk0C = 0xffff;
		const uint16_t item = rec.Word(0x0a);
		if (flags & 0x40)
		{
			desc.rotation = PlayerBearing(desc.x, desc.y);
			if (item != 0) FireFrom(item, desc, 0, false);
		}
		else if (item != 0) FireFrom(item, desc, 0, true);
		if (flags & 0x10) { rec.modifier |= 0x08; rec.disabled = true; }
		return ACTCMD_ACTED;
	}

	ObjectUid uid;
	Object *obj;
	if (flags & 0x02)                                        // re-use the source (RC:4161-4175)
	{
		uid = srcUid; obj = src;
		if (!(obj->flags & 0x80) && ObjId(*obj) == (uint16_t)tmpl) goto no_object;
		if (obj->renderType & 2) goto no_object;
		if (obj->renderType & 1)
		{
			// Only the slot's object and the count are cleared here; the slot
			// keeps its creature record (RC:4168-4171).
			const int slot = (int)obj->unk0C - 1;
			if (slot >= 0) { actors[slot / 0x22].object = 0; actorCount--; }
			obj->renderType &= 0xfe;
			obj->unk0C = 0;
		}
	}
	else                                                     // a new object (RC:4176-4196)
	{
		const int sector = LocateSector(src->x, src->y, src->z, -1);
		if (sector < 0) goto no_object;
		const Object copy = *src;
		uid = AppendObject(sector, copy);
		if (uid == 0) goto no_object;
		obj = Find(uid);
		mSource = uid; mSourceSynthetic = false;
		mHost->ObjectCreated(uid);
	}

	// The shared tail (RC:4199-4212).
	obj->renderType &= 0xd2;
	if (sound != 0) mHost->ChainSound(sound, obj->x, obj->y);
	if (newId != 0) MarkTriggerLinks(*obj, newId);
	SetObjId(*obj, (uint16_t)tmpl);
	obj->commandID = newId;
	obj->flags &= 0x7f;
	obj->unk0C = 0;
	mHost->ObjectChanged(uid);
	if (tflags & 0x80) SpawnFromResource(uid, (uint16_t)tmpl, 8);
	if (flags & 0x10) { rec.modifier |= 0x08; rec.disabled = true; }
	return ACTCMD_ACTED;

no_object:
	if (!(rec.Byte(6) & 0x04)) chainInterrupt = 2;
	return ACTCMD_NOTHING;
}

//==========================================================================
//
// The dump tools/actors_test compares with the probe's, line for line.
//
//==========================================================================

std::string ActorWorld::DumpTick() const
{
	std::string all = Dump(), out, aob;
	size_t p = 0;
	while (p < all.size())
	{
		const size_t e = all.find('\n', p);
		if (all.compare(p, 3, "OBJ")) out.append(all, p, e - p + 1);
		p = e + 1;
	}
	char line[200];
	auto add = [&](ObjectUid uid)
	{
		for (size_t s = 0; s < mUids.size(); s++)
			for (size_t i = 0; i < mUids[s].size(); i++)
				if (mUids[s][i] == uid)
				{
					const Object &o = mMap->objects[s][i];
					snprintf(line, sizeof line, "AOB %d.%d x=%d y=%d id=%04x rot=%02x f7=%02x lt=%02x f9=%02x z=%d c=%04x cmd=%04x\n",
						(int)s, (int)i, o.x, o.y, ObjId(o), o.rotation, o.flags, o.light, o.renderType, o.z, o.unk0C, o.commandID);
					aob += line;
					return;
				}
	};
	for (const ActorSlot &a : actors) if (a.object) add(a.object);
	for (const FlyingSlot &f : flying) if (f.object) add(f.object);
	return out + aob;
}

std::string ActorWorld::Dump() const
{
	std::string out;
	char line[400];
	if (mMap == nullptr) return out;
	auto ref = [&](ObjectUid uid, char *buf)
	{
		strcpy(buf, "-");
		if (uid == 0) return;
		for (size_t s = 0; s < mUids.size(); s++)
			for (size_t i = 0; i < mUids[s].size(); i++)
				if (mUids[s][i] == uid) { snprintf(buf, 32, "%d.%d", (int)s, (int)i); return; }
		strcpy(buf, "?");
	};
	char b[32];
	ref(mSourceSynthetic ? 0 : mSource, b);
	snprintf(line, sizeof line, "GLB hp=%d maxhp=%d arm=%d flash=%d rngd=%04x rnga=%08x rngs=%08x,%08x sched=%d src=%s intr=%02x nextid=%08x na=%u np=%u\n",
		mHost->Health(), mHost->MaxHealth(), mHost->Armour(), mHost->PainFlash(), rngDamage, rngWander, rngAnim, rngAnimSeed, animScheduler,
		mSourceSynthetic ? "drop+0" : b, chainInterrupt, nextObjectId, (unsigned)actorCount, (unsigned)flyingCount);
	out += line;
	for (size_t s = 0; s < mMap->objects.size(); s++)
		for (size_t i = 0; i < mMap->objects[s].size(); i++)
		{
			const Object &o = mMap->objects[s][i];
			snprintf(line, sizeof line, "OBJ %d.%d x=%d y=%d id=%04x rot=%02x f7=%02x lt=%02x f9=%02x z=%d c=%04x cmd=%04x\n",
				(int)s, (int)i, o.x, o.y, ObjId(o), o.rotation, o.flags, o.light, o.renderType, o.z, o.unk0C, o.commandID);
			out += line;
		}
	for (int i = 0; i < 16; i++)
	{
		const ActorSlot &a = actors[i];
		if (a.creature < 0 && a.object == 0) continue;
		ref(a.object, b);
		int defId = 0;
		if (a.creature >= 0 && (size_t)a.creature < creatureRt.size())
			if (const ActorDef *d = Def(creatureRt[a.creature].def)) defId = d->Id();
		snprintf(line, sizeof line, "ACT %d mon=%d def=%04x obj=%s st=%02x f9=%02x fa=%02x fb=%02x hp=%d lo=%04x,%04x,%04x sec=%04x vel=%04x ctr=%04x proj=%04x dly=%02x acc=%02x atk=%02x pain=%02x\n",
			i, a.creature, defId, b, a.state, a.bits, a.move, a.settle, a.health, a.fracX, a.fracY, a.fracZ,
			SectorOffset(a.sector), a.fallSpeed, a.counter, a.bullet, a.fireDelay, a.stepAccum, a.attackCount, a.pain);
		out += line;
	}
	for (int i = 0; i < 16; i++)
	{
		const FlyingSlot &f = flying[i];
		if (f.object == 0) continue;
		ref(f.object, b);
		snprintf(line, sizeof line, "PRJ %d obj=%s ang=%04x sec=%04x mode=%02x w=%02x fx=%04x fy=%04x fz=%04x zv=%d type=%04x dmg=%04x spd=%02x ttl=%02x own=%04x\n",
			i, b, f.angle, SectorOffset(f.sector), f.mode, f.width, f.fracX, f.fracY, f.fracZ, f.zVelocity, f.damageType, f.damage, f.speed, f.ttl, f.owner);
		out += line;
	}
	return out;
}

} // namespace roth
