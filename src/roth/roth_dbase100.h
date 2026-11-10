#pragma once
//
// Reader for Realms of the Haunting's DBASE100.DAT -- the game database.
//
// Engine-free, like roth_raw.h and roth_das.h: this needs nothing but the C++
// standard library, so it is built and tested on its own
// (tools/dbase100_test). The one place it meets the install is
// LoadDbase100FromInstall at the bottom, which only uses roth::Install and
// roth::Log -- both engine-free as well.
//
// WHAT IS IN THE FILE. Four tables, addressed from one 0x34-byte header:
//
//   inventory  -- one variable-length record per item (281 retail), each
//                 carrying trigger blocks of interpreter commands
//   action     -- the "dialogue" records the interpreter runs (694 retail):
//                 RAW 0x2b, the boot/quit/new-game/death hooks, op 0x1d jumps
//   cutscene   -- 0x14-byte entries naming each GDV and its subtitle stream
//   interface  -- u32 DBASE400 offsets for the game's system messages
//
// WHERE THE LAYOUT COMES FROM. docs/GAME_dialogue.md §2.1 and
// docs/GAME_inventory.md §2 (ROTH.C, E/dbase100.c and E/inventory.c),
// docs/NATIVE_rotheditor_rules.md §A4 (the editor's parsers/dbase_100.gd), and
// HANDOFF_REMAROTH.md §14 for the 1-based table bias. Field-by-field
// citations sit on the structs below. Anything those sources do not settle is
// marked UNVERIFIED where it is used -- and is something the dump tool can
// settle against the retail file (see HANDOFF_LANE_GAMELAYER_*.md).
//
// THE IMAGE IS MUTABLE. ROTH.C loads the whole file into memory once and
// writes into it at run time: inventory record +7 bit 0x80 ("examined",
// E/file_config.c:962-963), cutscene entry +0x13 (gallery index,
// E/gdv_cutscene.c:2772-2775), and at startup it rewrites the operands of the
// sound opcodes in place (resolve_dbase100_sound_ids, audio.c:1893-1924 --
// NOT done here; it needs the FX bank). So Dbase100 owns a private copy of the
// bytes and the parsed structs hold OFFSETS into it, never copies of mutable
// bytes.
//
// NOTHING FAILS SILENTLY. A load that cannot make sense of the header or a
// table fails with Error(). Anything wrong inside a single record is counted in
// Anomalies() under a fixed name, the record is marked bad, and the load goes
// on -- one broken item must not take the other 280 with it. Load also keeps
// a list of informational cross-checks (Notes()) for the questions the docs
// leave open.
//
// Everything is little-endian.
//

#include <stdint.h>
#include <string>
#include <vector>
#include <map>

namespace roth
{

class Install;
class Log;

//==========================================================================
//
// The command word, shared by action records and item trigger blocks.
//
// E/dbase100.c:6-7, 630-632:
//   opcode  = (w >> 24) & 0x7f
//   If-NOT  = bit 31
//   operand = w & 0xffffff
//
// So 0x91 is opcode 0x11 with If-NOT (remove item), 0xB4 is 0x34 with If-NOT
// (the OnUse "result" word). The interpreter skips a word whose operand is 0
// (E/dbase100.c:633) -- that is interpreter behaviour, so the reader keeps
// every word and leaves the skip to whoever executes them.
//
//==========================================================================

struct DbCommand
{
	uint32_t raw = 0;
	uint8_t opcode = 0;     // 7 bits
	bool ifNot = false;
	uint32_t operand = 0;   // 24 bits

	// The opcode byte as scripts and docs write it, If-NOT bit included.
	uint8_t FullOpcode() const { return (uint8_t)(raw >> 24); }
};

inline DbCommand DecodeDbCommand(uint32_t w)
{
	DbCommand c;
	c.raw = w;
	c.opcode = (uint8_t)((w >> 24) & 0x7f);
	c.ifNot = (w & 0x80000000u) != 0;
	c.operand = w & 0xffffffu;
	return c;
}

//==========================================================================
//
// Inventory record flags.
//
// docs/GAME_inventory.md §2.3, each bit with the ROTH.C site that reads it.
// The editor calls +4 "closeup_type"; ROTH.C uses it as a flags byte.
//
//==========================================================================

enum DbItemFlags : uint8_t
{
	DBI_CLOSEUP_HOLD    = 0x01, // close-up param_2: the modal hold/loop mode (inventory.c:1834)
	DBI_HAS_DOCUMENT    = 0x02, // has a Document (trigger 7) block (dialogue_ui.c:490)
	DBI_HAS_CHANGE_NAME = 0x04, // has a ChangeName (trigger 2) block (inventory.c:76)
	DBI_HAS_INFORMATION = 0x08, // has OnInformation (trigger 1) blocks (dialogue_ui.c:77)
	DBI_WEAPON          = 0x10, // WeaponAction / is a weapon (inventory.c:392,418)
	DBI_HAS_BULLET      = 0x20, // has an AsBullet block (inventory.c:156) -- name [inferred] in the spec
	DBI_HAS_INSPECT     = 0x40, // has OnInspect (trigger 4) (renderer.c:1485-1487)
	DBI_NOT_PICKABLE    = 0x80, // inventory.c:59
};

// The +5 "item_type" byte. The LOW NIBBLE is the category / inventory tab.
enum DbItemType : uint8_t
{
	DBT_CATEGORY_MASK   = 0x0F, // inventory.c:60-62,1332
	DBT_ADAM            = 0x10, // Adam / always-has-topics (dialogue_ui.c:76, inventory.c:2216)
	DBT_STACKABLE       = 0x20, // inventory.c:398
	DBT_HAS_ON_USE      = 0x40, // has OnUse / combinable (inventory.c:557)
};

// Category 1 is a weapon (weapon_combat.c:1396); category 2 "characters and
// important info" is never pickable, never duplicated, never put in a hand.
static const uint8_t DB_CATEGORY_WEAPON = 1;
static const uint8_t DB_CATEGORY_INFO = 2;

// Runtime byte +7.
static const uint8_t DBI_RUNTIME_EXAMINED = 0x80;   // file_config.c:962-963

// Trigger block codes (docs/GAME_inventory.md §2.3, from BTI/DBASE100.md:76-86).
enum DbTrigger : uint8_t
{
	DBTRIG_INFORMATION   = 0x01,
	DBTRIG_CHANGE_NAME   = 0x02,
	DBTRIG_INSPECT       = 0x04,
	DBTRIG_WEAPON_ACTION = 0x05,
	DBTRIG_AS_BULLET     = 0x06,
	DBTRIG_DOCUMENT      = 0x07,
	DBTRIG_AS_MONSTER    = 0x0A,
	DBTRIG_ON_USE        = 0x0B,
};

bool IsKnownDbTrigger(uint8_t code);
const char *DbTriggerName(uint8_t code);

//==========================================================================
//
// Header, 0x34 bytes (docs/NATIVE_rotheditor_rules.md §A4; the fields ROTH.C
// reads are cited in docs/GAME_dialogue.md §2.1).
//
//==========================================================================

struct DbHeader
{
	uint32_t fileSize = 0;          // +0x08, as the file claims; compared against the real size
	uint32_t unk0C = 0;             // +0x0C, meaning unknown
	uint32_t inventoryCount = 0;    // +0x10 (dbase100.c:193; bounds item ids, inventory.c:52,384)
	uint32_t inventoryTable = 0;    // +0x14, file offset of u32[inventoryCount]
	uint32_t actionCount = 0;       // +0x18 (dbase100.c:549)
	uint32_t actionTable = 0;       // +0x1C, file offset of u32[actionCount]
	uint32_t cutsceneCount = 0;     // +0x20 (dbase100.c:935)
	uint32_t cutsceneTable = 0;     // +0x24, inline entries, stride 0x14 (dbase100.c:937)
	uint32_t interfaceCount = 0;    // +0x28 (dbase100.c:412-414)
	uint32_t interfaceTable = 0;    // +0x2C, file offset of u32[interfaceCount]
	uint32_t field30 = 0;           // +0x30: op 0x07 is ignored unless this is 2 (dbase100.c:936)
};

static const size_t DB_HEADER_SIZE = 0x34;
static const size_t DB_ITEM_FIXED_SIZE = 0x14;
static const size_t DB_CUTSCENE_STRIDE = 0x14;

// One trigger block inside an inventory record. Header word
// {size:24 (bytes, header included), trigger:8}; the body is u32 command
// words (inventory.c:78-95; dbase_100.gd:164-205).
struct DbTriggerBlock
{
	uint32_t offset = 0;        // file offset of the block header
	uint32_t size = 0;          // bytes, header included
	uint8_t trigger = 0;
	uint32_t firstCommand = 0;  // file offset of the first command word
	int commandCount = 0;       // (size - 4) / 4
};

// One inventory record (docs/GAME_inventory.md §2.3).
struct DbItem
{
	int id = 0;                 // 1-based table index; THIS is what a slot stores
	uint32_t offset = 0;        // file offset of the record
	bool ok = false;            // false: the record is absent or malformed (see Anomalies)

	uint16_t length = 0;        // +0x00
	uint16_t renderId = 0;      // +0x02  textureIndex | textureSource << 8 (0x200+ = ADEMO)
	uint8_t flags = 0;          // +0x04  DbItemFlags
	uint8_t itemType = 0;       // +0x05  DbItemType
	uint8_t unk06 = 0;          // +0x06  meaning unknown
	uint8_t runtimeAtLoad = 0;  // +0x07  runtime byte as shipped; read the live one via Dbase100::ItemRuntime
	uint32_t closeup = 0;       // +0x08  DBASE300 offset / 8; 0 = none (inventory.c:1827-1836)
	uint32_t icon = 0;          // +0x0C  DBASE200 offset / 8
	uint32_t name = 0;          // +0x10  DBASE400 offset

	std::vector<DbTriggerBlock> blocks;
	bool terminated = false;    // the size-0 end marker was found
	uint32_t endOffset = 0;     // one past the end marker (or past the last good block)

	uint8_t Category() const { return itemType & DBT_CATEGORY_MASK; }
	bool IsWeapon() const { return (flags & DBI_WEAPON) != 0; }
	bool IsStackable() const { return (itemType & DBT_STACKABLE) != 0; }

	// First block with this trigger code, or null. (Information may repeat.)
	const DbTriggerBlock *FindBlock(uint8_t trigger) const;
};

// One action ("dialogue") record. The first dword's low 16 bits are the byte
// length; command count = len/4 - 1; commands start at record+4
// (dbase100.c:433-436). The high 16 bits are unknown (dbase_100.gd:224-265).
struct DbAction
{
	int id = 0;                 // 1-based
	uint32_t offset = 0;
	bool ok = false;
	uint16_t length = 0;
	uint16_t unk02 = 0;
	uint32_t firstCommand = 0;
	int commandCount = 0;
};

// One cutscene entry, 0x14 bytes (docs/GAME_dialogue.md §2.1).
struct DbCutscene
{
	int index = 0;              // 0-based; op 0x07 plays `operand - 1`
	uint32_t offset = 0;
	std::string name;           // +0x00, 8 bytes, NUL-padded: the GDV file stem
	uint16_t unk08 = 0;         // +0x08, meaning unknown
	int16_t subtitleSize = 0;   // +0x0A, byte size of the subtitle stream (gdv_cutscene.c:2777-2778)
	uint32_t titleText = 0;     // +0x0C, DBASE400 offset of the gallery title (gdv_cutscene.c:3105-3107)
	uint32_t subtitleText = 0;  // +0x10, 24-bit DBASE400 offset of the subtitle stream (gdv_cutscene.c:2782)
	uint8_t galleryAtLoad = 0;  // +0x13, written at run time (gdv_cutscene.c:2772-2775)
};

//==========================================================================
//
// The database.
//
// IDS. Inventory and action ids are 1-BASED: ROTH.C biases its in-memory table
// pointers by -4 (game_core.c:412-414, HANDOFF_REMAROTH.md §14), so
// record(id) = base + table[id - 1], with `id <= 0` and `offset == 0` meaning
// none. Ids run 1..count here. Whether ROTH.C's upper bound check is `> count`
// or `>= count` is UNVERIFIED (the docs cite the compare, not its direction);
// the retail key ranges (items 3-279 of 281, dialogue 23-693 of 694) never
// reach the edge, so the difference cannot show in shipped scripts.
//
// Cutscenes are 0-BASED (op 0x07 plays `operand - 1`). Interface entries are
// exposed 0-based and RAW: whether resolve_dbase100_text biases its index is
// UNVERIFIED, so no biased accessor is offered.
//
//==========================================================================

class Dbase100
{
public:
	// Takes a private copy (the image is mutable at run time, see the top of
	// this file). False on a structural failure, with Error() set.
	bool Load(const uint8_t *data, size_t size);
	bool Load(std::vector<uint8_t> &&bytes);

	bool IsLoaded() const { return mLoaded; }
	const std::string &Error() const { return mError; }
	const DbHeader &Header() const { return mHeader; }
	size_t Size() const { return mImage.size(); }

	int ItemCount() const { return (int)mItems.size(); }
	// 1-based. Null when out of range, the table entry is 0, or the record is bad.
	const DbItem *Item(int id) const;
	// As Item(), but also returns records that failed to parse (ok == false),
	// for diagnostics.
	const DbItem *ItemSlot(int id) const;

	int ActionCount() const { return (int)mActions.size(); }
	const DbAction *Action(int id) const;   // 1-based, null when none

	int CutsceneCount() const { return (int)mCutscenes.size(); }
	const DbCutscene *Cutscene(int index) const;   // 0-based

	int InterfaceCount() const { return (int)mInterface.size(); }
	// 0-based raw entry: a DBASE400 offset. False when out of range.
	bool InterfaceEntry(int index, uint32_t &out) const;

	// Command words. `index` counts from the first command.
	DbCommand ActionCommand(const DbAction &a, int index) const;
	DbCommand BlockCommand(const DbTriggerBlock &b, int index) const;
	std::vector<DbCommand> Commands(const DbAction &a) const;
	std::vector<DbCommand> Commands(const DbTriggerBlock &b) const;

	// The first item, by ascending id, whose +2 render id equals `renderId`;
	// 0 when none. Ascending-first is UNVERIFIED as ROTH.C's search order --
	// it only matters if two records share a render id, which
	// DuplicateRenderIds() reports.
	int FindItemByRenderId(uint16_t renderId) const;
	int DuplicateRenderIds() const { return mDuplicateRenderIds; }

	// The live runtime byte +7 of an item (bit 0x80 = examined), and setter.
	uint8_t ItemRuntime(int id) const;
	void SetItemRuntime(int id, uint8_t value);

	// The live gallery byte +0x13 of a cutscene, and setter.
	uint8_t CutsceneGallery(int index) const;
	void SetCutsceneGallery(int index, uint8_t value);

	// The flag bitmap ROTH.C allocates at game start: one bit per record,
	// ((count + 0x20) & ~0x1F) >> 3 bytes (docs/GAME_core.md §6). WHICH count
	// it uses is UNVERIFIED -- the docs say "433 records", which is neither the
	// inventory (281) nor the action (694) count. The dump tool prints every
	// header field that equals 433 to settle it.
	static int FlagBitmapBytes(uint32_t count) { return (int)(((count + 0x20u) & ~0x1Fu) >> 3); }

	// Every problem met inside a record, by fixed name, with a count. Empty on
	// a clean file.
	const std::map<std::string, int> &Anomalies() const { return mAnomalies; }
	int AnomalyTotal() const;
	// Informational cross-checks for open questions, one line each.
	const std::vector<std::string> &Notes() const { return mNotes; }

	// Raw access for consumers that work by offset, as ROTH.C does.
	const uint8_t *Image() const { return mImage.data(); }
	uint8_t *MutableImage() { return mImage.data(); }
	uint32_t U32At(uint32_t offset) const;   // 0 when out of range

private:
	bool Parse();
	void ParseItem(DbItem &it, uint32_t offset);
	void ParseAction(DbAction &a, uint32_t offset);
	void CrossCheck();
	void Anomaly(const char *what, int n = 1) { mAnomalies[what] += n; }
	bool InRange(uint32_t offset, uint32_t len) const;

	std::vector<uint8_t> mImage;
	bool mLoaded = false;
	std::string mError;
	DbHeader mHeader;
	std::vector<DbItem> mItems;          // [id - 1]
	std::vector<DbAction> mActions;      // [id - 1]
	std::vector<DbCutscene> mCutscenes;  // [index]
	std::vector<uint32_t> mInterface;    // [index]
	int mDuplicateRenderIds = 0;
	std::map<std::string, int> mAnomalies;
	std::vector<std::string> mNotes;
};

//==========================================================================
//
// The install adapter. Finds DBASE100.DAT beside ROTH.RES (the retail DATA
// directory, ROTH_BETTER.md §18), reads it with Install::ReadWholeFile, loads
// it, and reports what it read to `log` when given one: the counts, every
// anomaly through Log::Count, and a Warn for a failure. Returns false on any
// failure.
//
//==========================================================================

bool LoadDbase100FromInstall(const Install &install, Dbase100 &db, Log *log);

// The database the engine is using, if any. Empty until something loads it.
Dbase100 &TheDbase100();

} // namespace roth
