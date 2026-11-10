//
// Realms of the Haunting DBASE100.DAT reader. Engine-free; see roth_dbase100.h.
//
// Only the C++ standard library is used here. The install adapter
// (LoadDbase100FromInstall) lives in roth_dbase100_install.cpp so this file
// stays standalone.
//

#include "roth_dbase100.h"

#include <stdio.h>
#include <string.h>

namespace roth
{

namespace
{

inline uint16_t RdU16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t RdU32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
		| ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

std::string Fmt(const char *fmt, long a = 0, long b = 0, long c = 0, long d = 0)
{
	char buf[256];
	snprintf(buf, sizeof(buf), fmt, a, b, c, d);
	return buf;
}

} // namespace

//==========================================================================
//
// Trigger codes
//
//==========================================================================

bool IsKnownDbTrigger(uint8_t code)
{
	switch (code)
	{
	case DBTRIG_INFORMATION: case DBTRIG_CHANGE_NAME: case DBTRIG_INSPECT:
	case DBTRIG_WEAPON_ACTION: case DBTRIG_AS_BULLET: case DBTRIG_DOCUMENT:
	case DBTRIG_AS_MONSTER: case DBTRIG_ON_USE:
		return true;
	default:
		return false;
	}
}

const char *DbTriggerName(uint8_t code)
{
	switch (code)
	{
	case DBTRIG_INFORMATION:   return "OnInformation";
	case DBTRIG_CHANGE_NAME:   return "ChangeName";
	case DBTRIG_INSPECT:       return "OnInspect";
	case DBTRIG_WEAPON_ACTION: return "WeaponAction";
	case DBTRIG_AS_BULLET:     return "AsBullet";
	case DBTRIG_DOCUMENT:      return "Document";
	case DBTRIG_AS_MONSTER:    return "AsMonster";
	case DBTRIG_ON_USE:        return "OnUse";
	default:                   return "unknown";
	}
}

const DbTriggerBlock *DbItem::FindBlock(uint8_t trigger) const
{
	for (const auto &b : blocks)
		if (b.trigger == trigger) return &b;
	return nullptr;
}

//==========================================================================
//
// Loading
//
//==========================================================================

bool Dbase100::Load(const uint8_t *data, size_t size)
{
	std::vector<uint8_t> copy;
	if (data != nullptr && size > 0) copy.assign(data, data + size);
	return Load(std::move(copy));
}

bool Dbase100::Load(std::vector<uint8_t> &&bytes)
{
	mImage = std::move(bytes);
	mLoaded = false;
	mError.clear();
	mHeader = DbHeader();
	mItems.clear();
	mActions.clear();
	mCutscenes.clear();
	mInterface.clear();
	mDuplicateRenderIds = 0;
	mAnomalies.clear();
	mNotes.clear();

	mLoaded = Parse();
	if (!mLoaded)
	{
		// A failed load serves nothing: no half-read tables.
		mItems.clear();
		mActions.clear();
		mCutscenes.clear();
		mInterface.clear();
	}
	return mLoaded;
}

bool Dbase100::InRange(uint32_t offset, uint32_t len) const
{
	return (uint64_t)offset + (uint64_t)len <= (uint64_t)mImage.size();
}

uint32_t Dbase100::U32At(uint32_t offset) const
{
	if (!InRange(offset, 4)) return 0;
	return RdU32(mImage.data() + offset);
}

bool Dbase100::Parse()
{
	const size_t size = mImage.size();
	if (size < DB_HEADER_SIZE)
	{
		mError = Fmt("file is %ld bytes, smaller than the 0x34-byte DBASE100 header", (long)size);
		return false;
	}
	// The editor's parser checks this signature (dbase_100.gd:4-17). Whether
	// ROTH.C does is UNVERIFIED; refusing a file without it is this reader's
	// choice, made because everything after it would be garbage.
	if (memcmp(mImage.data(), "DBASE100", 8) != 0)
	{
		mError = "no \"DBASE100\" signature at offset 0";
		return false;
	}

	const uint8_t *d = mImage.data();
	DbHeader &h = mHeader;
	h.fileSize       = RdU32(d + 0x08);
	h.unk0C          = RdU32(d + 0x0C);
	h.inventoryCount = RdU32(d + 0x10);
	h.inventoryTable = RdU32(d + 0x14);
	h.actionCount    = RdU32(d + 0x18);
	h.actionTable    = RdU32(d + 0x1C);
	h.cutsceneCount  = RdU32(d + 0x20);
	h.cutsceneTable  = RdU32(d + 0x24);
	h.interfaceCount = RdU32(d + 0x28);
	h.interfaceTable = RdU32(d + 0x2C);
	h.field30        = RdU32(d + 0x30);

	if (h.fileSize != size)
		Anomaly("header file size differs from the real file size");

	// A table that does not fit is structural: every id into it would be
	// wrong, so the load fails rather than serve a partial table.
	struct TableCheck { const char *name; uint32_t count, offset, stride; };
	const TableCheck tables[] = {
		{ "inventory", h.inventoryCount, h.inventoryTable, 4 },
		{ "action",    h.actionCount,    h.actionTable,    4 },
		{ "cutscene",  h.cutsceneCount,  h.cutsceneTable,  (uint32_t)DB_CUTSCENE_STRIDE },
		{ "interface", h.interfaceCount, h.interfaceTable, 4 },
	};
	for (const auto &t : tables)
	{
		if (t.count == 0) continue;
		const uint64_t end = (uint64_t)t.offset + (uint64_t)t.count * t.stride;
		if (end > size || t.offset < DB_HEADER_SIZE)
		{
			char buf[200];
			snprintf(buf, sizeof(buf),
				"%s table (count %u at 0x%x) lies outside the %u-byte file",
				t.name, (unsigned)t.count, (unsigned)t.offset, (unsigned)size);
			mError = buf;
			return false;
		}
	}

	// ---- inventory ---------------------------------------------------------
	mItems.resize(h.inventoryCount);
	for (uint32_t i = 0; i < h.inventoryCount; i++)
	{
		DbItem &it = mItems[i];
		it.id = (int)i + 1;
		it.offset = RdU32(d + h.inventoryTable + i * 4);
		if (it.offset == 0) continue;            // "no record" -- legitimate
		ParseItem(it, it.offset);
	}

	// ---- actions -------------------------------------------------------------
	mActions.resize(h.actionCount);
	for (uint32_t i = 0; i < h.actionCount; i++)
	{
		DbAction &a = mActions[i];
		a.id = (int)i + 1;
		a.offset = RdU32(d + h.actionTable + i * 4);
		if (a.offset == 0) continue;
		ParseAction(a, a.offset);
	}

	// ---- cutscenes -----------------------------------------------------------
	mCutscenes.resize(h.cutsceneCount);
	for (uint32_t i = 0; i < h.cutsceneCount; i++)
	{
		DbCutscene &c = mCutscenes[i];
		const uint32_t off = h.cutsceneTable + i * (uint32_t)DB_CUTSCENE_STRIDE;
		const uint8_t *p = d + off;
		c.index = (int)i;
		c.offset = off;
		size_t n = 0;
		while (n < 8 && p[n] != 0) n++;
		c.name.assign((const char *)p, n);
		for (size_t k = 0; k < n; k++)
		{
			if (p[k] < 0x20 || p[k] > 0x7e)
			{
				Anomaly("cutscene name holds a non-printable byte");
				break;
			}
		}
		c.unk08 = RdU16(p + 0x08);
		c.subtitleSize = (int16_t)RdU16(p + 0x0A);
		c.titleText = RdU32(p + 0x0C);
		c.subtitleText = RdU32(p + 0x10) & 0xffffffu;
		c.galleryAtLoad = p[0x13];
	}

	// ---- interface -----------------------------------------------------------
	mInterface.resize(h.interfaceCount);
	for (uint32_t i = 0; i < h.interfaceCount; i++)
		mInterface[i] = RdU32(d + h.interfaceTable + i * 4);

	CrossCheck();
	return true;
}

//==========================================================================
//
// One inventory record: the 0x14-byte fixed part, then trigger blocks until a
// header whose size is 0 (inventory.c:78-95).
//
//==========================================================================

void Dbase100::ParseItem(DbItem &it, uint32_t off)
{
	if (!InRange(off, (uint32_t)DB_ITEM_FIXED_SIZE))
	{
		Anomaly("item record lies outside the file");
		return;
	}
	const uint8_t *p = mImage.data() + off;
	it.length        = RdU16(p + 0x00);
	it.renderId      = RdU16(p + 0x02);
	it.flags         = p[0x04];
	it.itemType      = p[0x05];
	it.unk06         = p[0x06];
	it.runtimeAtLoad = p[0x07];
	it.closeup       = RdU32(p + 0x08);
	it.icon          = RdU32(p + 0x0C);
	it.name          = RdU32(p + 0x10);

	uint32_t q = off + (uint32_t)DB_ITEM_FIXED_SIZE;
	for (;;)
	{
		if (!InRange(q, 4))
		{
			Anomaly("item trigger list reaches the end of the file with no end marker");
			break;
		}
		const uint32_t w = RdU32(mImage.data() + q);
		const uint32_t bsize = w & 0xffffffu;
		const uint8_t trig = (uint8_t)(w >> 24);
		if (bsize == 0)
		{
			it.terminated = true;
			it.endOffset = q + 4;
			if (trig != 0)
				Anomaly("item end marker carries a non-zero trigger byte");
			break;
		}
		if (bsize < 4 || (bsize & 3) != 0)
		{
			Anomaly("item trigger block size is not a whole number of words");
			break;
		}
		if (!InRange(q, bsize))
		{
			Anomaly("item trigger block runs past the end of the file");
			break;
		}
		DbTriggerBlock b;
		b.offset = q;
		b.size = bsize;
		b.trigger = trig;
		b.firstCommand = q + 4;
		b.commandCount = (int)((bsize - 4) / 4);
		if (!IsKnownDbTrigger(trig))
			Anomaly("item trigger block with an unknown trigger code");
		it.blocks.push_back(b);
		q += bsize;
	}
	if (!it.terminated) it.endOffset = q;
	// Without its end marker the walk ROTH.C does would run on into whatever
	// follows, so the record is not served. Its blocks stay for the dump.
	it.ok = it.terminated;
}

//==========================================================================
//
// One action record. The first dword's low 16 bits are the byte length;
// command count = len/4 - 1; commands at +4 (dbase100.c:433-436).
//
//==========================================================================

void Dbase100::ParseAction(DbAction &a, uint32_t off)
{
	if (!InRange(off, 4))
	{
		Anomaly("action record lies outside the file");
		return;
	}
	const uint32_t w = RdU32(mImage.data() + off);
	a.length = (uint16_t)(w & 0xffff);
	a.unk02 = (uint16_t)(w >> 16);
	if (a.length < 4)
	{
		// len/4 - 1 would be -1: ROTH.C runs no commands. Not served.
		Anomaly("action record shorter than its own length word");
		return;
	}
	if (a.length & 3)
		Anomaly("action record length is not a whole number of words");
	a.firstCommand = off + 4;
	a.commandCount = (int)(a.length / 4) - 1;
	if (!InRange(a.firstCommand, (uint32_t)a.commandCount * 4))
	{
		Anomaly("action record runs past the end of the file");
		a.commandCount = 0;
		return;
	}
	a.ok = true;
}

//==========================================================================
//
// Cross-checks: not errors, but the numbers that settle open questions in
// the docs. Each becomes one line in Notes().
//
//==========================================================================

void Dbase100::CrossCheck()
{
	const DbHeader &h = mHeader;

	int emptyItems = 0, badItems = 0, lowIds = 0;
	int lenThroughMarker = 0, lenBeforeMarker = 0, lenOther = 0;
	std::map<uint32_t, int> itemOffsets;
	std::map<uint16_t, int> renderIds;
	for (const auto &it : mItems)
	{
		if (it.offset == 0) { emptyItems++; continue; }
		itemOffsets[it.offset]++;
		if (!it.ok) { badItems++; continue; }
		renderIds[it.renderId]++;
		if (it.renderId < 0x200) lowIds++;
		const uint32_t span = it.endOffset - it.offset;
		if (it.length == span) lenThroughMarker++;
		else if (it.length + 4u == span) lenBeforeMarker++;
		else lenOther++;
	}
	int sharedItems = 0;
	for (auto &kv : itemOffsets) if (kv.second > 1) sharedItems += kv.second;
	mDuplicateRenderIds = 0;
	for (auto &kv : renderIds) if (kv.second > 1) mDuplicateRenderIds += kv.second;

	mNotes.push_back(Fmt("inventory: %ld entries, %ld empty, %ld malformed, %ld sharing a record offset",
		(long)mItems.size(), emptyItems, badItems, sharedItems));
	mNotes.push_back(Fmt("inventory: %ld records carry a render id below 0x200 (never pickable), %ld share a render id",
		lowIds, mDuplicateRenderIds));
	// Whether the +0 length counts the end marker is not stated anywhere.
	mNotes.push_back(Fmt("inventory +0 length: %ld equal the span through the end marker, %ld the span before it, %ld neither",
		lenThroughMarker, lenBeforeMarker, lenOther));

	// Flag bits that claim "has a block of trigger T" (GAME_inventory.md §2.3,
	// some of them [inferred] there) against the blocks actually present.
	struct Pair { const char *what; bool typeByte; uint8_t bit; uint8_t trigger; };
	const Pair pairs[] = {
		{ "+4&0x02 Document",       false, DBI_HAS_DOCUMENT,    DBTRIG_DOCUMENT },
		{ "+4&0x04 ChangeName",     false, DBI_HAS_CHANGE_NAME, DBTRIG_CHANGE_NAME },
		{ "+4&0x08 OnInformation",  false, DBI_HAS_INFORMATION, DBTRIG_INFORMATION },
		{ "+4&0x10 WeaponAction",   false, DBI_WEAPON,          DBTRIG_WEAPON_ACTION },
		{ "+4&0x20 AsBullet",       false, DBI_HAS_BULLET,      DBTRIG_AS_BULLET },
		{ "+4&0x40 OnInspect",      false, DBI_HAS_INSPECT,     DBTRIG_INSPECT },
		{ "+5&0x40 OnUse",          true,  DBT_HAS_ON_USE,      DBTRIG_ON_USE },
	};
	for (const auto &pr : pairs)
	{
		int both = 0, bitOnly = 0, blockOnly = 0;
		for (const auto &it : mItems)
		{
			if (!it.ok) continue;
			const bool bit = ((pr.typeByte ? it.itemType : it.flags) & pr.bit) != 0;
			const bool blk = it.FindBlock(pr.trigger) != nullptr;
			if (bit && blk) both++;
			else if (bit) bitOnly++;
			else if (blk) blockOnly++;
		}
		char buf[200];
		snprintf(buf, sizeof(buf), "flag %-22s both %d, bit without block %d, block without bit %d",
			pr.what, both, bitOnly, blockOnly);
		mNotes.push_back(buf);
	}

	int emptyActions = 0, badActions = 0;
	std::map<uint32_t, int> actionOffsets;
	for (const auto &a : mActions)
	{
		if (a.offset == 0) { emptyActions++; continue; }
		actionOffsets[a.offset]++;
		if (!a.ok) badActions++;
	}
	int sharedActions = 0;
	for (auto &kv : actionOffsets) if (kv.second > 1) sharedActions += kv.second;
	mNotes.push_back(Fmt("actions: %ld entries, %ld empty, %ld malformed, %ld sharing a record offset",
		(long)mActions.size(), emptyActions, badActions, sharedActions));

	mNotes.push_back(Fmt("header +0x30 = %ld (op 0x07 plays cutscenes only when this is 2)", (long)h.field30));

	// The flag bitmap: "433 records" per the docs, from an unnamed count.
	const struct { const char *name; uint32_t v; } fields[] = {
		{ "+0x0C", h.unk0C }, { "+0x10 inventory", h.inventoryCount },
		{ "+0x18 action", h.actionCount }, { "+0x20 cutscene", h.cutsceneCount },
		{ "+0x28 interface", h.interfaceCount }, { "+0x30", h.field30 },
	};
	std::string match;
	for (const auto &f : fields)
	{
		if (f.v == 433) { if (!match.empty()) match += ", "; match += f.name; }
	}
	mNotes.push_back("flag bitmap: header fields equal to the documented 433: "
		+ (match.empty() ? std::string("none") : match));
}

//==========================================================================
//
// Accessors
//
//==========================================================================

const DbItem *Dbase100::ItemSlot(int id) const
{
	if (id <= 0 || id > (int)mItems.size()) return nullptr;
	const DbItem &it = mItems[(size_t)id - 1];
	if (it.offset == 0) return nullptr;
	return &it;
}

const DbItem *Dbase100::Item(int id) const
{
	const DbItem *it = ItemSlot(id);
	return (it != nullptr && it->ok) ? it : nullptr;
}

const DbAction *Dbase100::Action(int id) const
{
	if (id <= 0 || id > (int)mActions.size()) return nullptr;
	const DbAction &a = mActions[(size_t)id - 1];
	return a.ok ? &a : nullptr;
}

const DbCutscene *Dbase100::Cutscene(int index) const
{
	if (index < 0 || index >= (int)mCutscenes.size()) return nullptr;
	return &mCutscenes[(size_t)index];
}

bool Dbase100::InterfaceEntry(int index, uint32_t &out) const
{
	if (index < 0 || index >= (int)mInterface.size()) return false;
	out = mInterface[(size_t)index];
	return true;
}

DbCommand Dbase100::ActionCommand(const DbAction &a, int index) const
{
	if (index < 0 || index >= a.commandCount) return DbCommand();
	return DecodeDbCommand(U32At(a.firstCommand + (uint32_t)index * 4));
}

DbCommand Dbase100::BlockCommand(const DbTriggerBlock &b, int index) const
{
	if (index < 0 || index >= b.commandCount) return DbCommand();
	return DecodeDbCommand(U32At(b.firstCommand + (uint32_t)index * 4));
}

std::vector<DbCommand> Dbase100::Commands(const DbAction &a) const
{
	std::vector<DbCommand> out;
	out.reserve((size_t)(a.commandCount > 0 ? a.commandCount : 0));
	for (int i = 0; i < a.commandCount; i++) out.push_back(ActionCommand(a, i));
	return out;
}

std::vector<DbCommand> Dbase100::Commands(const DbTriggerBlock &b) const
{
	std::vector<DbCommand> out;
	out.reserve((size_t)(b.commandCount > 0 ? b.commandCount : 0));
	for (int i = 0; i < b.commandCount; i++) out.push_back(BlockCommand(b, i));
	return out;
}

int Dbase100::FindItemByRenderId(uint16_t renderId) const
{
	for (const auto &it : mItems)
		if (it.ok && it.renderId == renderId) return it.id;
	return 0;
}

uint8_t Dbase100::ItemRuntime(int id) const
{
	const DbItem *it = Item(id);
	return it ? mImage[it->offset + 7] : 0;
}

void Dbase100::SetItemRuntime(int id, uint8_t value)
{
	const DbItem *it = Item(id);
	if (it) mImage[it->offset + 7] = value;
}

uint8_t Dbase100::CutsceneGallery(int index) const
{
	const DbCutscene *c = Cutscene(index);
	return c ? mImage[c->offset + 0x13] : 0;
}

void Dbase100::SetCutsceneGallery(int index, uint8_t value)
{
	const DbCutscene *c = Cutscene(index);
	if (c) mImage[c->offset + 0x13] = value;
}

int Dbase100::AnomalyTotal() const
{
	int n = 0;
	for (auto &kv : mAnomalies) n += kv.second;
	return n;
}

} // namespace roth
