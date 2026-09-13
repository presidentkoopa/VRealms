/*
** defblocks.cpp
**
** [DEFBLOCKS] The general reader for block-of-keys definition lumps. See the
** header for the shape and the error policy.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "defblocks.h"
#include "sc_man.h"
#include "filesystem.h"
#include "printf.h"

const FDefBlockEntry *FDefBlock::Find(const char *key) const
{
	for (const FDefBlockEntry &entry : Entries)
	{
		if (entry.Key.CompareNoCase(key) == 0)
			return &entry;
	}
	return nullptr;
}

namespace
{
	// A word is anything that starts like an identifier and is not quoted. By its
	// text, not its token type: the scanner hands back some words as keywords.
	bool IsWord(const FScanner &sc)
	{
		if (sc.TokenType == TK_StringConst || sc.TokenType == TK_NameConst) return false;
		const char c = sc.String[0];
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
	}

	bool IsNumber(const FScanner &sc)
	{
		return sc.TokenType == TK_IntConst || sc.TokenType == TK_UIntConst || sc.TokenType == TK_FloatConst;
	}

	FString Describe(const FScanner &sc)
	{
		if (sc.End) return "the end of the lump";
		FString s;
		s.Format("'%s'", sc.String);
		return s;
	}

	void Refuse(const FDefBlock &block, int line, const FString &why)
	{
		if (block.Name.IsNotEmpty())
		{
			Printf(TEXTCOLOR_RED "%s, line %d: %s '%s' refused -- %s\n",
				block.LumpName.GetChars(), line, block.Kind.GetChars(), block.Name.GetChars(), why.GetChars());
		}
		else if (block.Kind.IsNotEmpty())
		{
			Printf(TEXTCOLOR_RED "%s, line %d: %s block refused -- %s\n",
				block.LumpName.GetChars(), line, block.Kind.GetChars(), why.GetChars());
		}
		else
		{
			Printf(TEXTCOLOR_RED "%s, line %d: refused -- %s\n",
				block.LumpName.GetChars(), line, why.GetChars());
		}
	}

	// Consume tokens until the brace depth returns to 0. Starts inside one block
	// (depth 1). Returns false if the lump ended first.
	//
	// A block missing its '}' must not swallow the blocks after it: `<word> <word> {`
	// at this block's own level can only be the NEXT block's header (no value holds
	// a '{'), so the skip stops in front of it and reports the block as ended.
	bool SkipToBlockEnd(FScanner &sc)
	{
		int depth = 1;
		FScanner::SavedPos wordPos[2];
		int words = 0;	// consecutive words just read, at most 2 kept
		for (;;)
		{
			const FScanner::SavedPos before = sc.SavePos();
			if (!sc.GetToken()) return false;
			if (sc.TokenType == '{')
			{
				if (depth == 1 && words == 2)
				{
					sc.RestorePos(wordPos[0]);
					return true;
				}
				depth++;
				words = 0;
			}
			else if (sc.TokenType == '}')
			{
				if (--depth == 0) return true;
				words = 0;
			}
			else if (IsWord(sc))
			{
				if (words == 2)
				{
					wordPos[0] = wordPos[1];
					words = 1;
				}
				wordPos[words++] = before;
			}
			else
			{
				words = 0;
			}
		}
	}

	// After a broken block header: skip to the next '{' and past its block.
	// Returns false if the lump ended first.
	bool SkipPastNextBlock(FScanner &sc)
	{
		while (sc.GetToken())
		{
			if (sc.TokenType == '{') return SkipToBlockEnd(sc);
		}
		return false;
	}

	// Unclosed: the block is missing its '}' and the scanner is left in front of the
	// next block's header, which the caller reads next (no skipping).
	enum class EParse { Ok, Error, EndOfLump, Unclosed };

	// One value item: atoms, then an optional '@' number. Leaves the token after
	// the item unread.
	EParse ParseItem(FScanner &sc, FDefBlockItem &item, FString &error, int &errorLine)
	{
		for (;;)
		{
			const FScanner::SavedPos before = sc.SavePos();
			if (!sc.GetToken())
			{
				error = "missing '}' before the end of the lump";
				errorLine = sc.Line;
				return EParse::EndOfLump;
			}

			FDefBlockAtom atom;
			if (IsNumber(sc))
			{
				atom.Kind = FDefBlockAtom::Number;
				atom.Value = sc.Float;
				atom.Text = sc.String;
			}
			else if (sc.TokenType == '-')
			{
				// Normally the scanner folds a leading '-' into the number; accept
				// it as its own token too. On error the offending token is left
				// unread, so a '}' there still closes the block for the skip.
				const FScanner::SavedPos afterMinus = sc.SavePos();
				if (!sc.GetToken() || !IsNumber(sc))
				{
					error.Format("expected a number after '-', found %s", Describe(sc).GetChars());
					errorLine = sc.Line;
					if (sc.End) return EParse::EndOfLump;
					sc.RestorePos(afterMinus);
					return EParse::Error;
				}
				atom.Kind = FDefBlockAtom::Number;
				atom.Value = -sc.Float;
				atom.Text.Format("-%s", sc.String);
			}
			else if (sc.TokenType == TK_StringConst || sc.TokenType == TK_NameConst)
			{
				atom.Kind = FDefBlockAtom::String;
				atom.Text = sc.String;
			}
			else if (IsWord(sc))
			{
				// A word followed by '=' is the NEXT key; a word followed by a word and
				// '{' is the NEXT BLOCK's header (this block is missing its '}'). Either
				// way it is not part of this value.
				FString word = sc.String;
				const FScanner::SavedPos afterWord = sc.SavePos();
				if (sc.GetToken() && (sc.TokenType == '=' || (IsWord(sc) && sc.GetToken() && sc.TokenType == '{')))
				{
					sc.RestorePos(before);
					break;
				}
				sc.RestorePos(afterWord);
				atom.Kind = FDefBlockAtom::Word;
				atom.Text = word;
			}
			else
			{
				// ',', '@', ';', '}' or anything else ends the atoms.
				sc.RestorePos(before);
				break;
			}

			if (item.Atoms.Size() == 0) item.Line = sc.Line;
			item.Atoms.Push(atom);
		}

		if (item.Atoms.Size() == 0)
		{
			const FScanner::SavedPos before = sc.SavePos();
			sc.GetToken();
			error.Format("expected a value, found %s", Describe(sc).GetChars());
			errorLine = sc.Line;
			sc.RestorePos(before);
			return EParse::Error;
		}

		const FScanner::SavedPos beforeAt = sc.SavePos();
		if (sc.GetToken() && sc.TokenType == '@')
		{
			const FScanner::SavedPos afterAt = sc.SavePos();
			if (!sc.GetToken() || !IsNumber(sc))
			{
				error.Format("expected a number after '@', found %s", Describe(sc).GetChars());
				errorLine = sc.Line;
				if (sc.End) return EParse::EndOfLump;
				sc.RestorePos(afterAt);
				return EParse::Error;
			}
			item.HasAt = true;
			item.At = sc.Float;
		}
		else
		{
			sc.RestorePos(beforeAt);
		}
		return EParse::Ok;
	}

	// The entries of one block, after its '{'. Consumes the closing '}' on success.
	EParse ParseEntries(FScanner &sc, FDefBlock &block, FString &error, int &errorLine)
	{
		for (;;)
		{
			const FScanner::SavedPos before = sc.SavePos();
			if (!sc.GetToken())
			{
				error = "missing '}' before the end of the lump";
				errorLine = sc.Line;
				return EParse::EndOfLump;
			}
			if (sc.TokenType == '}') return EParse::Ok;
			if (sc.TokenType == ';') continue;
			if (!IsWord(sc))
			{
				error.Format("expected a key, found %s", Describe(sc).GetChars());
				errorLine = sc.Line;
				sc.RestorePos(before);
				return EParse::Error;
			}

			FDefBlockEntry entry;
			entry.Key = sc.String;
			entry.Line = sc.Line;

			const FScanner::SavedPos beforeEquals = sc.SavePos();
			const bool gotToken = sc.GetToken();
			if (!gotToken || sc.TokenType != '=')
			{
				const FString found = Describe(sc);
				const int foundLine = sc.Line;
				if (gotToken && IsWord(sc) && sc.GetToken() && sc.TokenType == '{')
				{
					// `<word> <word> {` where a key belongs is the NEXT block's header:
					// this block is missing its '}'. Refuse it and leave the scanner in
					// front of that header, so the next block still loads.
					error = "missing '}' (the next block starts here)";
					errorLine = entry.Line;
					sc.RestorePos(before);
					return EParse::Unclosed;
				}
				error.Format("expected '=' after '%s', found %s", entry.Key.GetChars(), found.GetChars());
				errorLine = foundLine;
				if (!gotToken) return EParse::EndOfLump;
				sc.RestorePos(beforeEquals);
				return EParse::Error;
			}

			for (;;)
			{
				FDefBlockItem item;
				const EParse result = ParseItem(sc, item, error, errorLine);
				if (result != EParse::Ok) return result;
				entry.Items.Push(item);

				const FScanner::SavedPos beforeComma = sc.SavePos();
				if (sc.GetToken() && sc.TokenType == ',') continue;
				sc.RestorePos(beforeComma);
				break;
			}

			block.Entries.Push(entry);
		}
	}
}

void ReadDefinitionBlocks(int lump, const FDefBlockHandler &handler, FDefBlockStats &stats)
{
	FScanner sc(lump);
	const FString lumpName = sc.ScriptName;

	while (sc.GetToken())
	{
		FDefBlock block;
		block.LumpName = lumpName;
		block.Line = sc.Line;
		stats.Blocks++;

		FString error;
		int errorLine = 0;

		// Header: <kind> <name> '{'
		if (!IsWord(sc))
		{
			error.Format("expected a block such as 'particle <name> { ... }', found %s", Describe(sc).GetChars());
			Refuse(block, sc.Line, error);
			stats.Refused++;
			// A stray '{' starts a block to skip; anything else is dropped alone.
			if (sc.TokenType == '{' && !SkipToBlockEnd(sc)) break;
			continue;
		}
		block.Kind = sc.String;

		const FScanner::SavedPos beforeName = sc.SavePos();
		if (!sc.GetToken() || !(IsWord(sc) || sc.TokenType == TK_StringConst || sc.TokenType == TK_NameConst))
		{
			error.Format("expected a name after '%s', found %s", block.Kind.GetChars(), Describe(sc).GetChars());
			Refuse(block, block.Line, error);
			stats.Refused++;
			if (sc.End) break;
			sc.RestorePos(beforeName);
			// Skip to this block's '{' and past its '}'.
			if (!SkipPastNextBlock(sc)) break;
			continue;
		}
		block.Name = sc.String;

		if (!sc.GetToken() || sc.TokenType != '{')
		{
			error.Format("expected '{' after the name, found %s", Describe(sc).GetChars());
			Refuse(block, sc.Line, error);
			stats.Refused++;
			if (sc.End) break;
			if (IsWord(sc))
			{
				// `<a> <b> <c>`: the kind word may be stray and `<b> <c> {` a block.
				// Start again at the name; the kind word is consumed, so this always
				// moves forward.
				sc.RestorePos(beforeName);
				continue;
			}
			if (!SkipPastNextBlock(sc)) break;
			continue;
		}

		const EParse result = ParseEntries(sc, block, error, errorLine);
		if (result != EParse::Ok)
		{
			Refuse(block, errorLine > 0 ? errorLine : block.Line, error);
			stats.Refused++;
			if (result == EParse::Unclosed) continue;
			if (result == EParse::EndOfLump || !SkipToBlockEnd(sc)) break;
			continue;
		}

		error = "";
		errorLine = 0;
		if (handler(block, error, errorLine))
		{
			stats.Accepted++;
		}
		else
		{
			Refuse(block, errorLine > 0 ? errorLine : block.Line, error.IsNotEmpty() ? error : FString("refused by its reader"));
			stats.Refused++;
		}
	}
}
