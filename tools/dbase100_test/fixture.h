#pragma once
//
// Builds synthetic DBASE100.DAT images byte by byte, from the layout written up
// in docs/GAME_dialogue.md §2.1, docs/GAME_inventory.md §2 and
// docs/NATIVE_rotheditor_rules.md §A4. Independent of the reader: nothing here
// includes roth_dbase100.h, so a misreading in the reader cannot be copied into
// the fixtures by sharing code.
//
// Layout produced:
//   0x00  header (0x34)
//         inventory table   u32[n]
//         action table      u32[n]
//         cutscene entries  0x14 each
//         interface table   u32[n]
//         inventory records
//         action records
//

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

namespace fx
{

inline void Put16(std::vector<uint8_t> &b, size_t at, uint16_t v)
{
	b[at] = (uint8_t)v; b[at + 1] = (uint8_t)(v >> 8);
}
inline void Put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
	for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(v >> (8 * i));
}
inline void Add16(std::vector<uint8_t> &b, uint16_t v) { b.resize(b.size() + 2); Put16(b, b.size() - 2, v); }
inline void Add32(std::vector<uint8_t> &b, uint32_t v) { b.resize(b.size() + 4); Put32(b, b.size() - 4, v); }

// A command word: {operand:24, opcode:7, If-NOT:1}.
inline uint32_t W(uint8_t opcode, uint32_t operand, bool ifNot = false)
{
	return (operand & 0xffffffu) | ((uint32_t)(opcode & 0x7f) << 24) | (ifNot ? 0x80000000u : 0);
}

struct Block
{
	uint8_t trigger;
	std::vector<uint32_t> words;
};

struct ItemSpec
{
	bool absent = false;            // table entry 0
	uint16_t renderId = 0;
	uint8_t flags = 0, itemType = 0, unk06 = 0, runtime = 0;
	uint32_t closeup = 0, icon = 0, name = 0;
	std::vector<Block> blocks;
	// Length policy for +0: 0 = span through the end marker, 1 = span before
	// it, 2 = use `lengthOverride`.
	int lengthMode = 0;
	uint16_t lengthOverride = 0;
	// Raw bytes appended instead of the end marker when non-empty (for
	// malformed-record tests); `noTerminator` drops the marker entirely.
	bool noTerminator = false;
	uint32_t terminatorWord = 0;
};

struct ActionSpec
{
	bool absent = false;
	std::vector<uint32_t> words;
	uint16_t unk02 = 0;
	bool overrideLength = false;
	uint16_t length = 0;
};

struct CutsceneSpec
{
	std::string name;               // up to 8 bytes
	uint16_t unk08 = 0;
	int16_t subtitleSize = 0;
	uint32_t titleText = 0;
	uint32_t subtitleText = 0;      // 24 bits
	uint8_t gallery = 0;
};

struct Spec
{
	uint32_t unk0C = 0;
	uint32_t field30 = 2;
	std::vector<ItemSpec> items;
	std::vector<ActionSpec> actions;
	std::vector<CutsceneSpec> cutscenes;
	std::vector<uint32_t> interface;
	bool wrongFileSize = false;
};

// Where things landed, so tests can poke at exact offsets.
struct Layout
{
	uint32_t inventoryTable = 0, actionTable = 0, cutsceneTable = 0, interfaceTable = 0;
	std::vector<uint32_t> itemOffsets, actionOffsets;
};

inline std::vector<uint8_t> Build(const Spec &s, Layout *outLayout = nullptr)
{
	std::vector<uint8_t> b(0x34, 0);
	memcpy(b.data(), "DBASE100", 8);
	Layout L;

	L.inventoryTable = (uint32_t)b.size();
	for (size_t i = 0; i < s.items.size(); i++) Add32(b, 0);
	L.actionTable = (uint32_t)b.size();
	for (size_t i = 0; i < s.actions.size(); i++) Add32(b, 0);
	L.cutsceneTable = (uint32_t)b.size();
	for (const auto &c : s.cutscenes)
	{
		size_t at = b.size();
		b.resize(at + 0x14, 0);
		memcpy(b.data() + at, c.name.data(), c.name.size() < 8 ? c.name.size() : 8);
		Put16(b, at + 0x08, c.unk08);
		Put16(b, at + 0x0A, (uint16_t)c.subtitleSize);
		Put32(b, at + 0x0C, c.titleText);
		Put32(b, at + 0x10, (c.subtitleText & 0xffffffu) | ((uint32_t)c.gallery << 24));
	}
	L.interfaceTable = (uint32_t)b.size();
	for (uint32_t v : s.interface) Add32(b, v);

	for (size_t i = 0; i < s.items.size(); i++)
	{
		const ItemSpec &it = s.items[i];
		if (it.absent) { L.itemOffsets.push_back(0); continue; }
		const uint32_t off = (uint32_t)b.size();
		L.itemOffsets.push_back(off);
		Put32(b, L.inventoryTable + i * 4, off);
		b.resize(off + 0x14, 0);
		Put16(b, off + 0x02, it.renderId);
		b[off + 4] = it.flags; b[off + 5] = it.itemType;
		b[off + 6] = it.unk06; b[off + 7] = it.runtime;
		Put32(b, off + 0x08, it.closeup);
		Put32(b, off + 0x0C, it.icon);
		Put32(b, off + 0x10, it.name);
		for (const auto &blk : it.blocks)
		{
			const uint32_t size = 4 + 4 * (uint32_t)blk.words.size();
			Add32(b, (size & 0xffffffu) | ((uint32_t)blk.trigger << 24));
			for (uint32_t w : blk.words) Add32(b, w);
		}
		const uint32_t beforeMarker = (uint32_t)b.size() - off;
		if (!it.noTerminator) Add32(b, it.terminatorWord);
		const uint32_t through = (uint32_t)b.size() - off;
		uint16_t len = (uint16_t)through;
		if (it.lengthMode == 1) len = (uint16_t)beforeMarker;
		if (it.lengthMode == 2) len = it.lengthOverride;
		Put16(b, off, len);
	}

	for (size_t i = 0; i < s.actions.size(); i++)
	{
		const ActionSpec &a = s.actions[i];
		if (a.absent) { L.actionOffsets.push_back(0); continue; }
		const uint32_t off = (uint32_t)b.size();
		L.actionOffsets.push_back(off);
		Put32(b, L.actionTable + i * 4, off);
		const uint16_t len = a.overrideLength ? a.length : (uint16_t)(4 + 4 * a.words.size());
		Add16(b, len);
		Add16(b, a.unk02);
		for (uint32_t w : a.words) Add32(b, w);
	}

	Put32(b, 0x08, (uint32_t)b.size() + (s.wrongFileSize ? 1 : 0));
	Put32(b, 0x0C, s.unk0C);
	Put32(b, 0x10, (uint32_t)s.items.size());
	Put32(b, 0x14, s.items.empty() ? 0 : L.inventoryTable);
	Put32(b, 0x18, (uint32_t)s.actions.size());
	Put32(b, 0x1C, s.actions.empty() ? 0 : L.actionTable);
	Put32(b, 0x20, (uint32_t)s.cutscenes.size());
	Put32(b, 0x24, s.cutscenes.empty() ? 0 : L.cutsceneTable);
	Put32(b, 0x28, (uint32_t)s.interface.size());
	Put32(b, 0x2C, s.interface.empty() ? 0 : L.interfaceTable);
	Put32(b, 0x30, s.field30);

	if (outLayout) *outLayout = L;
	return b;
}

} // namespace fx
