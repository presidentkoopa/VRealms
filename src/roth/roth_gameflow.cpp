//
// Realms game flow and persistence. See roth_gameflow.h.
//
// Engine-free. Every rule cites ROTH.C (roth_c/src/engine) as file.c:LINE.
//

#include "roth_gameflow.h"
#include "roth_raw.h"

#include <string.h>

namespace roth
{

//==========================================================================
//
// The byte stream
//
//==========================================================================

void SaveWriter::EndChunk(size_t mark)
{
	if (mark < 4 || mark > mBuf.size()) return;
	const uint32_t size = (uint32_t)(mBuf.size() - mark);
	mBuf[mark - 4] = (uint8_t)size;
	mBuf[mark - 3] = (uint8_t)(size >> 8);
	mBuf[mark - 2] = (uint8_t)(size >> 16);
	mBuf[mark - 1] = (uint8_t)(size >> 24);
}

uint8_t SaveReader::U8()
{
	if (mPos >= mN) { mOk = false; return 0; }
	return mP[mPos++];
}

bool SaveReader::Bytes(uint8_t *dst, size_t n)
{
	if (n > Remaining()) { mOk = false; mPos = mN; if (dst != nullptr && n != 0) memset(dst, 0, n); return false; }
	if (dst != nullptr && n != 0) memcpy(dst, mP + mPos, n);
	mPos += n;
	return true;
}

std::string SaveReader::Str()
{
	const uint32_t n = U32();
	if (!mOk || n > Remaining()) { mOk = false; mPos = mN; return std::string(); }
	std::string s((const char *)(mP + mPos), (size_t)n);
	mPos += n;
	return s;
}

std::vector<uint8_t> SaveReader::Blob()
{
	const uint32_t n = U32();
	if (!mOk || n > Remaining()) { mOk = false; mPos = mN; return std::vector<uint8_t>(); }
	std::vector<uint8_t> b(mP + mPos, mP + mPos + n);
	mPos += n;
	return b;
}

bool SaveReader::NextChunk(uint16_t &id, SaveReader &payload)
{
	if (!mOk || Remaining() < 6) return false;
	id = U16();
	const uint32_t size = U32();
	if (size > Remaining()) { mOk = false; mPos = mN; return false; }
	payload = SaveReader(mP + mPos, (size_t)size);
	mPos += size;
	return true;
}

//==========================================================================
//
// The player record
//
//==========================================================================

static void Put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void Put32(uint8_t *p, uint32_t v) { Put16(p, (uint16_t)v); Put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t Get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t Get32(const uint8_t *p) { return (uint32_t)Get16(p) | ((uint32_t)Get16(p + 2) << 16); }

void PlayerRecord::Encode(uint8_t out[BYTES]) const
{
	// write_player_state_chunk, savegame.c:139-166.
	memset(out, 0, BYTES);
	Put32(out + 0x00, (uint32_t)x);
	Put32(out + 0x04, (uint32_t)z);
	Put32(out + 0x08, (uint32_t)y);
	Put16(out + 0x0c, angle);
	Put16(out + 0x0e, sector);
	Put32(out + 0x10, secondary);
	Put32(out + 0x14, primary);
	Put32(out + 0x18, (uint32_t)health);
	Put32(out + 0x1c, (uint32_t)viewPitch);
	Put32(out + 0x20, (uint32_t)viewPitchApplied);
	Put32(out + 0x24, (uint32_t)valueReduction);
	Put16(out + 0x28, eyeHeight);
	Put32(out + 0x2c, dword8c114);
}

bool PlayerRecord::Decode(const uint8_t *in, size_t size)
{
	if (in == nullptr || size < (size_t)BYTES) return false;
	x = (int32_t)Get32(in + 0x00);
	z = (int32_t)Get32(in + 0x04);
	y = (int32_t)Get32(in + 0x08);
	angle = Get16(in + 0x0c);
	sector = Get16(in + 0x0e);
	secondary = Get32(in + 0x10);
	primary = Get32(in + 0x14);
	health = (int32_t)Get32(in + 0x18);
	viewPitch = (int32_t)Get32(in + 0x1c);
	viewPitchApplied = (int32_t)Get32(in + 0x20);
	valueReduction = (int32_t)Get32(in + 0x24);
	eyeHeight = Get16(in + 0x28);
	dword8c114 = Get32(in + 0x2c);
	return true;
}

//==========================================================================
//
// ROTH.RES's map list
//
//==========================================================================

static std::string BaseUpper(const std::string &path)
{
	size_t start = 0;
	for (size_t i = 0; i < path.size(); i++)
		if (path[i] == '\\' || path[i] == '/' || path[i] == ':') start = i + 1;
	std::string out;
	for (size_t i = start; i < path.size(); i++)
	{
		char c = path[i];
		if (c == '.') break;
		if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
		out += c;
	}
	return out;
}

std::vector<MapListEntry> ParseMapList(const uint8_t *res, size_t size)
{
	std::vector<MapListEntry> out;
	if (res == nullptr) return out;

	// Split into lines; find "maps {"; take "<map> <pack>" pairs to the "}".
	bool inList = false;
	size_t i = 0;
	while (i < size)
	{
		std::string line;
		while (i < size && res[i] != '\n' && res[i] != '\r' && res[i] != 0) line += (char)res[i++];
		while (i < size && (res[i] == '\n' || res[i] == '\r' || res[i] == 0)) i++;

		std::vector<std::string> tok;
		std::string cur;
		for (char c : line)
		{
			if (c == ' ' || c == '\t') { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } }
			else cur += c;
		}
		if (!cur.empty()) tok.push_back(cur);
		if (tok.empty()) continue;

		if (!inList)
		{
			std::string head = tok[0];
			for (char &c : head) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
			if (head == "maps" && tok.size() >= 2 && tok[1] == "{") inList = true;
			else if (head == "maps{") inList = true;
			continue;
		}
		if (tok[0][0] == '}') break;
		if (tok[0][0] == ';') continue;
		MapListEntry e;
		e.map = BaseUpper(tok[0]);
		if (tok.size() >= 2) e.pack = BaseUpper(tok[1]);
		if (!e.map.empty()) out.push_back(e);
	}
	return out;
}

//==========================================================================
//
// RAW 0x3b
//
//==========================================================================

bool WarpNamesMatch(const char *requested, const char *currentBase)
{
	// map_load.c:596-610. `esi` walks the request, `edi` the loaded map's
	// file name from its last path segment; the loop ends at the name's '.'
	// or after 8 characters. An empty request never enters the loop and
	// leaves `same` set.
	if (requested == nullptr || requested[0] == 0) return true;
	if (currentBase == nullptr) currentBase = "";
	const char *cur = currentBase;
	for (const char *p = currentBase; *p; p++)
		if (*p == '\\' || *p == '/') cur = p + 1;        // :586-592, the last segment

	int ecx = 8;
	const uint8_t *esi = (const uint8_t *)requested;
	const uint8_t *edi = (const uint8_t *)cur;
	uint8_t al = 0;
	for (;;)
	{
		al = *esi;
		// The original's loaded name always carries ".RAW"; a bare base name
		// here ends where that '.' would be.
		const uint8_t ah = (*edi == 0) ? (uint8_t)0x2e : *edi;
		if (ah == 0x2e) break;                                   // :603
		if ((uint8_t)(al & 0xdf) != (uint8_t)(ah & 0xdf)) return false;   // :604
		esi++; edi++;
		if (--ecx <= 0) { al = 0; break; }                       // :606
	}
	return al == 0;                                              // :608
}

WarpKind ClassifyWarp(const WarpRequest &req, const char *currentBase)
{
	// game_core.c:938-939: the warp runs only when name[0] + key != 0.
	if ((int)(uint8_t)req.name[0] + (int)req.key == 0) return WarpKind::Nothing;
	if (WarpNamesMatch(req.name, currentBase))
		return req.key != 0 ? WarpKind::RelocateInPlace : WarpKind::SameMapNoMove;   // map_load.c:612-623
	return WarpKind::ChangeMap;
}

std::string WarpTargetName(const WarpRequest &req)
{
	// map_load.c:632-641: copy until a byte <= 0x20, at most 8.
	std::string out;
	for (int i = 0; i < 8; i++)
	{
		const uint8_t c = (uint8_t)req.name[i];
		if (c <= 0x20) break;
		out += (char)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
	}
	return out;
}

bool FindArrivalSector(const Map &map, uint16_t key, int skipSector, Arrival &out)
{
	for (size_t i = 0; i < map.sectors.size(); i++)              // map_load.c:548-556
	{
		if ((int)i == skipSector) continue;                      // :549
		const Sector &s = map.sectors[i];
		if (s.commandID != key) continue;                        // :550

		// ml_relocate_found, map_load.c:516-539.
		if (s.firstFaceIndex < 0 || s.faceCount == 0) return false;
		int minx = 0x7fff, miny = 0x7fff, maxx = -0x8000, maxy = -0x8000;
		for (int j = 0; j < (int)s.faceCount; j++)
		{
			const int fi = s.firstFaceIndex + j;
			if (fi < 0 || fi >= (int)map.faces.size()) return false;
			const int vi = map.faces[(size_t)fi].vertex1;
			if (vi < 0 || vi >= (int)map.vertices.size()) return false;
			const Vertex &v = map.vertices[(size_t)vi];
			if (!(v.x > minx)) minx = v.x;
			if (!(v.x < maxx)) maxx = v.x;
			if (!(v.y > miny)) miny = v.y;
			if (!(v.y < maxy)) maxy = v.y;
		}
		out.sector = (int)i;
		out.x = (minx + maxx) >> 1;                              // :536
		out.y = (miny + maxy) >> 1;                              // :537
		out.floor = s.floorHeight;
		return true;
	}
	return false;
}

//==========================================================================
//
// One map's logic state
//
//==========================================================================

static const uint32_t MAPSTATE_MAGIC = 0x534d4c52;   // "RLMS"
static const uint16_t MAPSTATE_VERSION = 1;

void MapLogicState::Save(SaveWriter &w) const
{
	w.U32(MAPSTATE_MAGIC);
	w.U16(MAPSTATE_VERSION);
	w.Str(mapName);

	w.U32((uint32_t)commands.size());
	for (const SavedCommand &c : commands)
	{
		w.U8(c.modifier);
		w.U8(c.disabled);
		w.U16((uint16_t)c.args.size());
		for (uint16_t a : c.args) w.U16(a);
	}
	w.U32((uint32_t)delays.size());
	for (const SavedDelay &d : delays) { w.I32(d.remaining); w.U16(d.record); w.U32(d.context); }
	w.U32((uint32_t)deferred.size());
	for (const SavedDeferred &d : deferred) { w.U16(d.chain); w.U32(d.context); }
	w.U32((uint32_t)effects.size());
	for (const SavedEffect &e : effects)
	{
		w.U8(e.tick); w.U16(e.record); w.U16(e.payload);
		w.U32((uint32_t)e.sectors.size());
		for (int32_t s : e.sectors) w.I32(s);
		w.U8(e.finished);
		w.Blob(e.baseLight);
		w.U16(e.hold); w.U8(e.holding); w.U8(e.flags5);
		w.I32(e.sfxNode); w.U8(e.sfxStarted);
		w.U32((uint32_t)e.objects.size());
		for (int32_t o : e.objects) w.I32(o);
		for (int i = 0; i < 4; i++) w.I32(e.moveVerts[i]);
	}
	w.U16(pendingRecord);
	w.I32(activeFace); w.I32(activeSector);
	w.I32(activeObjectSector); w.I32(activeObjectIndex);
	w.U32((uint32_t)movers.size());
	for (int32_t m : movers) w.I32(m);
	w.U32((uint32_t)lastSectorByPlayer.size());
	for (int32_t m : lastSectorByPlayer) w.I32(m);
	w.U8(dialogueLatch);
}

// A count read from a stream is believed only as far as the bytes left can
// hold that many entries of at least `each` bytes.
static bool CountFits(SaveReader &r, uint32_t n, size_t each)
{
	return r.Ok() && (uint64_t)n * each <= r.Remaining();
}

bool MapLogicState::Load(SaveReader &r)
{
	MapLogicState t;
	if (r.U32() != MAPSTATE_MAGIC) return false;
	if (r.U16() != MAPSTATE_VERSION) return false;
	t.mapName = r.Str();

	uint32_t n = r.U32();
	if (!CountFits(r, n, 4)) return false;
	t.commands.resize(n);
	for (SavedCommand &c : t.commands)
	{
		c.modifier = r.U8();
		c.disabled = r.U8();
		const uint16_t an = r.U16();
		if (!CountFits(r, an, 2)) return false;
		c.args.resize(an);
		for (uint16_t &a : c.args) a = r.U16();
	}
	n = r.U32();
	if (!CountFits(r, n, 10)) return false;
	t.delays.resize(n);
	for (SavedDelay &d : t.delays) { d.remaining = r.I32(); d.record = r.U16(); d.context = r.U32(); }
	n = r.U32();
	if (!CountFits(r, n, 6)) return false;
	t.deferred.resize(n);
	for (SavedDeferred &d : t.deferred) { d.chain = r.U16(); d.context = r.U32(); }
	n = r.U32();
	if (!CountFits(r, n, 43)) return false;
	t.effects.resize(n);
	for (SavedEffect &e : t.effects)
	{
		e.tick = r.U8(); e.record = r.U16(); e.payload = r.U16();
		uint32_t m = r.U32();
		if (!CountFits(r, m, 4)) return false;
		e.sectors.resize(m);
		for (int32_t &s : e.sectors) s = r.I32();
		e.finished = r.U8();
		e.baseLight = r.Blob();
		e.hold = r.U16(); e.holding = r.U8(); e.flags5 = r.U8();
		e.sfxNode = r.I32(); e.sfxStarted = r.U8();
		m = r.U32();
		if (!CountFits(r, m, 4)) return false;
		e.objects.resize(m);
		for (int32_t &o : e.objects) o = r.I32();
		for (int i = 0; i < 4; i++) e.moveVerts[i] = r.I32();
	}
	t.pendingRecord = r.U16();
	t.activeFace = r.I32(); t.activeSector = r.I32();
	t.activeObjectSector = r.I32(); t.activeObjectIndex = r.I32();
	n = r.U32();
	if (!CountFits(r, n, 4)) return false;
	t.movers.resize(n);
	for (int32_t &m : t.movers) m = r.I32();
	n = r.U32();
	if (!CountFits(r, n, 4)) return false;
	t.lastSectorByPlayer.resize(n);
	for (int32_t &m : t.lastSectorByPlayer) m = r.I32();
	t.dialogueLatch = r.U8();
	if (!r.Ok()) return false;
	*this = t;
	return true;
}

bool MapLogicState::operator==(const MapLogicState &o) const
{
	if (mapName != o.mapName || commands.size() != o.commands.size()) return false;
	for (size_t i = 0; i < commands.size(); i++)
	{
		const SavedCommand &a = commands[i], &b = o.commands[i];
		if (a.modifier != b.modifier || a.disabled != b.disabled || a.args != b.args) return false;
	}
	if (delays.size() != o.delays.size() || deferred.size() != o.deferred.size()
		|| effects.size() != o.effects.size()) return false;
	for (size_t i = 0; i < delays.size(); i++)
		if (delays[i].remaining != o.delays[i].remaining || delays[i].record != o.delays[i].record
			|| delays[i].context != o.delays[i].context) return false;
	for (size_t i = 0; i < deferred.size(); i++)
		if (deferred[i].chain != o.deferred[i].chain || deferred[i].context != o.deferred[i].context) return false;
	for (size_t i = 0; i < effects.size(); i++)
	{
		const SavedEffect &a = effects[i], &b = o.effects[i];
		if (a.tick != b.tick || a.record != b.record || a.payload != b.payload || a.sectors != b.sectors
			|| a.finished != b.finished || a.baseLight != b.baseLight || a.hold != b.hold
			|| a.holding != b.holding || a.flags5 != b.flags5 || a.objects != b.objects
			|| a.sfxNode != b.sfxNode || a.sfxStarted != b.sfxStarted) return false;
		for (int k = 0; k < 4; k++) if (a.moveVerts[k] != b.moveVerts[k]) return false;
	}
	return pendingRecord == o.pendingRecord && activeFace == o.activeFace && activeSector == o.activeSector
		&& activeObjectSector == o.activeObjectSector && activeObjectIndex == o.activeObjectIndex
		&& movers == o.movers && lastSectorByPlayer == o.lastSectorByPlayer && dialogueLatch == o.dialogueLatch;
}

//==========================================================================
//
// An original .SAV
//
//==========================================================================

OriginalSave ParseOriginalSave(const uint8_t *data, size_t size)
{
	OriginalSave s;
	if (data == nullptr) return s;
	size_t pos = 0;
	std::string pendingName;
	while (pos + 4 <= size)                                      // savegame.c:1380-1384
	{
		const uint16_t id = Get16(data + pos);
		const uint16_t len = Get16(data + pos + 2);
		pos += 4;
		s.order.push_back(id);
		if (id == 1)
		{
			// The version marker is a HEADER ONLY: its size word is 3 and no
			// payload follows (savegame.c:1565, 1386-1389).
			if (len < 3) return s;
			continue;
		}
		if (id == 0xc) { s.ok = true; break; }                   // :1427-1430
		if (pos + len > size) return s;
		const uint8_t *p = data + pos;
		switch (id)
		{
		case 2: s.hasPlayer = s.player.Decode(p, len); break;    // :1390-1393
		case 3:                                                  // :1394-1396
		{
			std::string n;
			for (size_t i = 0; i < len && p[i] != 0; i++) n += (char)p[i];
			s.mapName = BaseUpper(n);
			break;
		}
		case 4: s.cursorPos.assign(p, p + len); break;
		case 5: s.cursorScroll.assign(p, p + len); break;
		case 6: s.flags.assign(p, p + len); break;
		case 7: s.inventory.assign(p, p + len); break;
		case 8:                                                  // :1419-1421
			pendingName.clear();
			for (size_t i = 0; i < len && p[i] != 0; i++) pendingName += (char)p[i];
			break;
		case 9:                                                  // :1422-1424
		{
			OriginalSave::MapState m;
			m.file = pendingName;
			m.bytes.assign(p, p + len);
			s.maps.push_back(m);
			pendingName.clear();
			break;
		}
		case 0xa:
			for (size_t i = 0; i < len && p[i] != 0; i++) s.slotName += (char)p[i];
			break;
		case 0xb: s.hasThumbnail = true; break;
		case 0xd: s.gallery.assign(p, p + len); break;           // :1431-1448
		case 0xe: if (len >= 4) s.musicChunk = Get32(p); break;  // :1449-1460
		default: break;                                          // :1461
		}
		pos += len;
	}
	return s;
}

OriginalMapState MeasureOriginalMapState(const uint8_t *d, size_t n)
{
	OriginalMapState o;
	if (d == nullptr || n < 4) return o;
	size_t pos = 0;
	const uint16_t w0 = Get16(d), w1 = Get16(d + 2);             // map_load.c:821-824
	pos = 4;
	if (w0 == 0xffff)
	{
		o.geometryIsDelta = true;
		o.geometryBytes = w1;
		if (pos + w1 > n) return o;
		// 0 = end, 0x01-0x7f = that many literal bytes, 0x80-0xff = skip
		// (code - 0x80) bytes (renderer.c:250-266).
		size_t q = pos;
		const size_t end = pos + w1;
		while (q < end)
		{
			const uint8_t c = d[q++];
			if (c == 0) break;
			if (c < 0x80) { o.geometryChanged += c; q += c; }
		}
		pos += w1;
	}
	else
	{
		if (w0 < 4 || (size_t)w0 > n) return o;
		o.geometryBytes = w0;
		pos = w0;
	}
	if (pos + 2 > n) return o;                                   // :833-838, the object buffer
	{
		const uint16_t len = Get16(d + pos);
		const size_t total = 2 + (size_t)(((uint32_t)len - 1) & ~1u);
		o.objectBytes = (uint32_t)total;
		pos += total;
	}
	if (pos + 4 > n) return o;                                   // :841-870, the command patch
	o.commandPatchBytes = Get32(d + pos); pos += 4 + (size_t)o.commandPatchBytes;
	if (pos + 4 > n) return o;                                   // :872-879, the effect list
	o.effectBytes = Get32(d + pos); pos += 4 + (size_t)o.effectBytes;
	if (pos + 4 > n) return o;                                   // :880-888: this length counts itself
	o.entityBytes = Get32(d + pos); pos += (o.entityBytes >= 4 ? (size_t)o.entityBytes : 4);
	if (pos + 4 > n) return o;                                   // :889-896, the sound nodes
	o.soundNodeBytes = Get32(d + pos); pos += 4 + (size_t)o.soundNodeBytes;
	if (pos + 4 > n) return o;
	o.ok = Get32(d + pos) == 0x45584954;                         // :897-901, "EXIT"
	return o;
}

//==========================================================================
//
// The flow
//
//==========================================================================

void GameFlow::Attach(GameCore *core, Dbase100 *db, FlowHost *host)
{
	mCore = core;
	mDb = (db != nullptr && db->IsLoaded()) ? db : nullptr;
	mHost = host;
}

void GameFlow::ClearCursors()
{
	memset(mCursorPos, 0, sizeof(mCursorPos));
	memset(mCursorScroll, 0, sizeof(mCursorScroll));
}

void GameFlow::Boot()
{
	if (!IsAttached()) return;
	mSeenCount = 1;                                              // game_core.c:394
	mOverlayActive = false;
	mScreenHeld = false;
	mSavedMode = MODE_OFF;
	mMode = MODE_OFF;
	mCore->RunAction(GameCore::ACTION_BOOT);                     // game_core.c:757-758
	mMode = MODE_BETWEEN;                                        // :761
	mCore->Items().Reset();                                      // :763
	mCore->Interp().FinishDialogueRecordEval();                  // :764
	mCore->Items().RestoreActiveHeldItem(*mDb, mCore->ItemSink()); // :765
	mMaxHealth = DEFAULT_MAX_HEALTH;                             // :766-767 (no Adam record yet: the cap is unset)
	mCore->Interp().State().g_value_reduction_factor = 0;        // :769
	mHealth = mMaxHealth;                                        // :770
	mNoDeathWait = false;                                        // :776
	mBooted = true;
}

void GameFlow::NewGame()
{
	if (!IsAttached()) return;
	mNoDeathWait = false;                                        // game_core.c:776
	mMode = MODE_BETWEEN;                                        // :777
	ClearCursors();                                              // :822
	// reset_inventory, action 3 and its finish, restore_active_held_item,
	// activate_weapon_item(0, 0): :823-827, 832.
	mCore->NewGame();
	if (mMaxHealth == 0) mMaxHealth = DEFAULT_MAX_HEALTH;        // :828
	mHealth = mMaxHealth;                                        // :829-830
	mCore->Interp().State().g_value_reduction_factor = 0;        // :831
	mDeathUnits = 0;
	mMode = MODE_PLAY;                                           // :845
}

void GameFlow::ResetForMenu()
{
	if (!IsAttached()) return;
	// free_dbase100_data + init_game_databases (game_core.c:450, 459): the
	// image comes off the disk again, so everything the game wrote into it is
	// gone -- each movie's gallery byte and each item's "examined" bit -- and
	// the flag bitmap is allocated zeroed (game_core.c:394, 420-432).
	for (int i = 0; i < mDb->CutsceneCount(); i++) mDb->SetCutsceneGallery(i, 0);
	for (int id = 1; id <= mDb->ItemCount(); id++)
	{
		const uint8_t rt = mDb->ItemRuntime(id);
		if (rt & 0x80) mDb->SetItemRuntime(id, (uint8_t)(rt & 0x7f));    // file_config.c:962-967 sets it
	}
	mSeenCount = 1;
	std::vector<uint8_t> &bm = mCore->Interp().State().g_dbase100_record_bitmap;
	for (uint8_t &b : bm) b = 0;
	mOverlayActive = false;
	mScreenHeld = false;
	mNoDeathWait = false;                                        // :776
	mDeathUnits = 0;
	mMode = MODE_BETWEEN;                                        // :777
}

void GameFlow::AdvanceHealth(uint32_t amount, uint32_t max)
{
	uint32_t v = (uint32_t)mHealth + amount;                     // renderer.c:9821
	if (v >= max) v = max;                                       // :9822
	mHealth = (int32_t)v;
}

void GameFlow::StampGallery(int index)
{
	// gdv_cutscene.c:2771-2776. The two gates in front of it -- g@0x71384,
	// which is 1 in the image and never written, and "DBASE400 is open" --
	// hold in the retail game (measured: tools/gameflow_test, the oracle's
	// EVENT lines).
	if (mDb->CutsceneGallery(index) == 0)
	{
		mDb->SetCutsceneGallery(index, (uint8_t)mSeenCount);
		mSeenCount++;
	}
}

uint32_t GameFlow::PlayRecordGdvCutscene(int index)
{
	if (!IsAttached()) return 0;
	const DbCutscene *c = mDb->Cutscene(index);
	if (c == nullptr || c->name.empty()) return 0;               // gdv_cutscene.c:2762-2763

	StampGallery(index);

	// :2801-2804: the mode is saved only when no dialogue is up and this is
	// the first movie of the run (a second one finds the screen already held).
	const bool busy = mCore->Interp().State().g_dialogue_busy_flag != 0;
	if (!busy && !mScreenHeld) mSavedMode = mMode;
	mScreenHeld = true;                                          // :2831-2834
	mMode = MODE_DIALOGUE;                                       // :2839

	mMovieLog.push_back(c->name);
	FlowHost &host = mHost != nullptr ? *mHost : mNullHost;
	// :2777-2795: the subtitle stream is loaded when the entry has one.
	host.PlayMovie(c->name.c_str(), c->subtitleText & 0xffffffu, c->subtitleSize);   // :2840

	mOverlayActive = true;                                       // gdv_cutscene.c:2731
	return 1;                                                    // :2847
}

uint32_t GameFlow::ShowFullscreenImage(uint32_t id)
{
	if (!IsAttached() || id == 0) return 0;                      // menu_hud_ui.c:1178-1179
	const bool busy = mCore->Interp().State().g_dialogue_busy_flag != 0;
	if (!busy && !mScreenHeld) mSavedMode = mMode;               // :1191-1193
	mScreenHeld = true;                                          // :1209-1212
	mMode = MODE_DIALOGUE;                                       // :1215
	FlowHost &host = mHost != nullptr ? *mHost : mNullHost;
	host.ShowImage(id);                                          // :1217-1230
	return 1;
}

void GameFlow::ExitCutsceneOverlayMode()
{
	if (!mOverlayActive) return;                                 // gdv_cutscene.c:2436
	mOverlayActive = false;                                      // :2440
	mMode = mSavedMode;                                          // :2474-2475
	mScreenHeld = false;                                         // :2476-2479
	FlowHost &host = mHost != nullptr ? *mHost : mNullHost;
	host.MovieOverlayEnded();
}

GameFlow::DeathStep GameFlow::BeginDeath()
{
	if (!IsAttached()) return DeathStep::Restart;
	mMode = MODE_DEAD;                                           // game_core.c:961
	mCore->UnequipWeapon();                                      // :962
	mCore->RunAction(GameCore::ACTION_DEATH);                    // :967-968
	mMode = MODE_DEAD;                                           // :972
	mDeathUnits = 0;                                             // :966, ecx = 0
	if (mNoDeathWait) return DeathStep::Restart;                 // :973
	return DeathStep::Waiting;
}

GameFlow::DeathStep GameFlow::TickDeath(int frameScale, uint8_t scancode)
{
	if (mDeathUnits >= DEATH_WAIT_UNITS) return DeathStep::Restart;
	mDeathUnits += frameScale;                                   // game_core.c:975
	if (scancode == 1) mDeathUnits = DEATH_WAIT_UNITS;           // :982-983
	else if (scancode == 0x39)                                   // :984
	{
		if (mLastSlot != -1) return DeathStep::QuickLoad;        // :986-988
		mDeathUnits = DEATH_WAIT_UNITS;                          // :998
	}
	return mDeathUnits < DEATH_WAIT_UNITS ? DeathStep::Waiting : DeathStep::Restart;   // :1002
}

//==========================================================================
//
// Saving
//
// The global chunks keep the original's ids and payloads (savegame.c:1565-1627)
// so that one table describes both formats and ImportOriginal is the same
// code path as LoadGlobals. The framing differs: the original's chunk header
// is { u16 id, u16 size }, this one's is { u16 id, u32 size }.
//
//==========================================================================

static const uint16_t SAVE_VERSION = 1;

void GameFlow::FillPlayerRecord(PlayerRecord &p) const
{
	if (!IsAttached()) return;
	uint32_t left = 0, right = 0;
	mCore->Items().SaveHands(left, right);
	p.secondary = left;                                          // the weapon hand, savegame.c:152-154
	p.primary = right;                                           // the selected item, :155-157
	p.health = mHealth;                                          // :159
	p.valueReduction = mCore->Interp().State().g_value_reduction_factor;   // :143
}

void GameFlow::SaveGlobals(SaveWriter &w, const PlayerRecord &player, const std::string &mapName) const
{
	if (!IsAttached()) return;
	size_t m;

	m = w.BeginChunk(1); w.U16(SAVE_VERSION); w.EndChunk(m);     // savegame.c:1565

	m = w.BeginChunk(2);                                         // :1578-1579
	{
		PlayerRecord p = player;
		FillPlayerRecord(p);
		uint8_t rec[PlayerRecord::BYTES];
		p.Encode(rec);
		w.Bytes(rec, sizeof(rec));
	}
	w.EndChunk(m);

	m = w.BeginChunk(3);                                         // :1581-1584, 0xe bytes
	{
		uint8_t name[0xe] = {};
		for (size_t i = 0; i < mapName.size() && i < sizeof(name) - 1; i++) name[i] = (uint8_t)mapName[i];
		w.Bytes(name, sizeof(name));
	}
	w.EndChunk(m);

	m = w.BeginChunk(0xe); w.U32(mMusicChunk); w.EndChunk(m);    // :1586-1587

	m = w.BeginChunk(7);                                         // :1589-1590
	{
		const std::vector<uint8_t> inv = mCore->Items().Serialize();
		w.Bytes(inv.data(), inv.size());
	}
	w.EndChunk(m);

	m = w.BeginChunk(4); w.Bytes(mCursorPos, sizeof(mCursorPos)); w.EndChunk(m);         // :1592-1593
	m = w.BeginChunk(5); w.Bytes(mCursorScroll, sizeof(mCursorScroll)); w.EndChunk(m);   // :1595-1596

	m = w.BeginChunk(6);                                         // :1598-1600
	{
		const std::vector<uint8_t> &bm = mCore->Interp().State().g_dbase100_record_bitmap;
		w.Bytes(bm.data(), bm.size());
	}
	w.EndChunk(m);

	m = w.BeginChunk(0xd);                                       // :1602-1613
	for (int i = 0; i < mDb->CutsceneCount(); i++) w.U8(mDb->CutsceneGallery(i));
	w.EndChunk(m);

	m = w.BeginChunk(0xc); w.EndChunk(m);                        // :1616, the end marker
}

bool GameFlow::ImportOriginal(const OriginalSave &sav)
{
	if (!IsAttached() || !sav.ok || !sav.hasPlayer) return false;
	std::vector<uint8_t> &bm = mCore->Interp().State().g_dbase100_record_bitmap;
	if (sav.inventory.size() != (size_t)Inventory::SAVE_BYTES) return false;
	if (sav.flags.size() != bm.size()) return false;
	if (sav.gallery.size() != (size_t)mDb->CutsceneCount()) return false;

	// Chunk 2, read_player_state_chunk (savegame.c:168-195).
	mCore->Interp().State().g_value_reduction_factor = sav.player.valueReduction;
	mHealth = sav.player.health;

	// Chunk 7 (savegame.c:1408-1418): the slots, the recount, the weapon list.
	mCore->Items().Deserialize(sav.inventory.data(), sav.inventory.size(), mCore->ItemSink());
	mCore->Items().LoadHands(sav.player.secondary, sav.player.primary);

	if (sav.cursorPos.size() == sizeof(mCursorPos)) memcpy(mCursorPos, sav.cursorPos.data(), sizeof(mCursorPos));
	if (sav.cursorScroll.size() == sizeof(mCursorScroll)) memcpy(mCursorScroll, sav.cursorScroll.data(), sizeof(mCursorScroll));

	bm = sav.flags;                                              // chunk 6, :1405-1407

	mSeenCount = 1;                                              // chunk 0xd, :1431-1448
	for (int i = 0; i < mDb->CutsceneCount(); i++)
	{
		const uint8_t v = sav.gallery[(size_t)i];
		if (mSeenCount <= v) mSeenCount = (uint32_t)v + 1;
		mDb->SetCutsceneGallery(i, v);
	}

	mMusicChunk = sav.musicChunk;                                // chunk 0xe, :1449-1460

	// The tail (savegame.c:1474-1498, then game_core.c:842): the weapon in
	// the weapon hand is re-activated and the Adam latch refreshed. Loading
	// stops every voice first (:1367); nothing of a conversation survives.
	mCore->Interp().ResetDialogueState();
	mCore->ReactivateWeapon();
	mCore->Items().RestoreActiveHeldItem(*mDb, mCore->ItemSink());
	mOverlayActive = false;
	mScreenHeld = false;
	mNoDeathWait = false;
	mDeathUnits = 0;
	mMode = MODE_PLAY;
	mBooted = true;     // a loaded game is a running one, booted or not in this process
	return true;
}

bool GameFlow::LoadGlobals(SaveReader &r, PlayerRecord *playerOut, std::string *mapNameOut)
{
	if (!IsAttached()) return false;
	OriginalSave s;
	uint16_t id = 0;
	SaveReader c(nullptr, 0);
	bool versionOk = false;
	while (r.NextChunk(id, c))
	{
		const size_t n = c.Remaining();
		std::vector<uint8_t> b(n);
		c.Bytes(b.data(), n);
		switch (id)
		{
		case 1: versionOk = n >= 2 && Get16(b.data()) == SAVE_VERSION; break;
		case 2: s.hasPlayer = s.player.Decode(b.data(), n); break;
		case 3:
			for (size_t i = 0; i < n && b[i] != 0; i++) s.mapName += (char)b[i];
			break;
		case 4: s.cursorPos = b; break;
		case 5: s.cursorScroll = b; break;
		case 6: s.flags = b; break;
		case 7: s.inventory = b; break;
		case 0xd: s.gallery = b; break;
		case 0xe: if (n >= 4) s.musicChunk = Get32(b.data()); break;
		case 0xc: s.ok = true; break;
		default: break;     // a chunk from a later version: skipped, as the original skips unknown ids
		}
		if (s.ok) break;
	}
	if (!versionOk) return false;
	if (!ImportOriginal(s)) return false;
	if (playerOut != nullptr) *playerOut = s.player;
	if (mapNameOut != nullptr) *mapNameOut = s.mapName;
	return true;
}

} // namespace roth
