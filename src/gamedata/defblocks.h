/*
** defblocks.h
**
** [DEFBLOCKS] The general reader for engine-read definition lumps made of named
** blocks of keys. Written for PARTICLEDEFS (stage 2b, particledefs.cpp) and meant
** to be the ONE such reader: debris keys, mesh particle keys and surface brushes
** are the next callers named in "Engine docs/REVIEW_SMOKE_DEBRIS_DAMAGE.md" (X4,
** "one definitions parser"). It knows nothing about any of them.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** THE SHAPE
**
**   <kind> <name>
**   {
**       key = item, item, ...      // one entry; ';' after it is allowed, not needed
**   }
**
**   item  :=  atom+ [ '@' number ]
**   atom  :=  number | word | "string"
**
** e.g.  size = 6 @0, 28 @0.5, 40 @1     three items, one number each, each with @
**       color = 255 245 200 @0          one item of three numbers, with @
**       texture = "RSSKA0", 6, 12, loop  four items, one atom each
**
** An entry runs until a word followed by '=' (the next key), a '}' or a ';', so
** keys need no terminator and may share a line. Words are matched by their TEXT,
** because FScanner returns some words (loop, stop, color...) as keyword tokens.
** '//' and block comments are skipped by FScanner.
**
** ERRORS NEVER ABORT. A malformed block is refused with one console line naming
** the lump, the line and the block, the reader skips to that block's closing '}',
** and the rest of the lump loads. The caller's handler refuses a well-formed block
** the same way by returning false. Nothing here throws: it avoids the FScanner
** calls that raise script errors.
**
*/

#pragma once

#include <functional>
#include "tarray.h"
#include "zstring.h"

struct FDefBlockAtom
{
	enum EKind { Number, Word, String };

	EKind Kind = Number;
	double Value = 0.0;		// Number
	FString Text;			// Word or String (without quotes); for a Number, as written
};

struct FDefBlockItem
{
	TArray<FDefBlockAtom> Atoms;
	bool HasAt = false;		// written with '@'
	double At = 0.0;
	int Line = 0;
};

struct FDefBlockEntry
{
	FString Key;			// as written; compare with CompareNoCase
	TArray<FDefBlockItem> Items;
	int Line = 0;
};

struct FDefBlock
{
	FString Kind;			// the leading word, as written
	FString Name;			// as written (a word or a quoted string)
	FString LumpName;		// "container:path", for messages
	int Line = 0;			// the line of the kind word
	TArray<FDefBlockEntry> Entries;

	// The first entry with this key (case-insensitive), or null.
	const FDefBlockEntry *Find(const char *key) const;
};

struct FDefBlockStats
{
	int Blocks = 0;			// every block started, well-formed or not
	int Accepted = 0;
	int Refused = 0;		// syntax errors plus handler refusals
};

// Called once per well-formed block. Return true to accept it. To refuse, return
// false with a reason in `error` and, if a particular line is to blame, that line
// in `errorLine` (left 0, the block's own line is named).
using FDefBlockHandler = std::function<bool(const FDefBlock &block, FString &error, int &errorLine)>;

// Reads every block of one lump, calling `handler` for each well-formed one and
// printing one line per refused one. Adds to `stats`.
void ReadDefinitionBlocks(int lump, const FDefBlockHandler &handler, FDefBlockStats &stats);
