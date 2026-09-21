#pragma once

// ============================================================================
// GUN FIT MODE -- the engine half. Stage 1: where each MODELDEF block came
// from, and reading, setting and restoring a class's Offset live.
//
// Build plan: the owner's "Gun Fit Mode -- Build Plan" doc. The player fixes
// where a gun sits in the hand from inside the headset; the engine owns every
// piece of maths and every file write, so script never guesses an axis order
// and never touches the disk.
//
// SAFETY RULE, the owner's words: "slow and steady, can't get this wrong". The
// game never writes a weapon set's real MODELDEF. Stage 1 writes nothing to
// disk at all -- it only remembers where blocks came from and changes Offset
// in memory.
// ============================================================================

#include "tarray.h"
#include "zstring.h"
#include "vectors.h"

class PClass;

// ONE MODEL BLOCK, AS IT WAS READ. Kept per block, not per frame: one block
// makes many frames, and it is the block's text a later stage rewrites.
struct ModelDefBlockSource
{
	const PClass *cls = nullptr;
	int      lump = -1;
	FString  lumpName;           // container + path, e.g. RS_VR_Weapons_HacX.pk3:MODELDEF.txt
	int      blockStart = -1;    // byte range of the whole block, `Model` to its closing `}`
	int      blockEnd = -1;
	int      offsetStart = -1;   // byte range of its Offset line, -1 if it has none
	int      offsetEnd = -1;
	int      zoffsetStart = -1;  // byte range of its ZOffset line, -1 if it has none
	int      zoffsetEnd = -1;
	FVector3 offset = { 0.f, 0.f, 0.f };  // the Offset the block ended up with
	uint32_t crc = 0;            // CRC32 of the block text, blockStart..blockEnd
	bool     inherited = false;  // got its frames from `inherits` -- fit mode refuses these
};

extern TArray<ModelDefBlockSource> ModelDefBlockSources;

// GetOffset's status, also what SetOffset returns (negated) when it refuses.
enum
{
	MODELFIT_OK        =  1,
	MODELFIT_NOMODEL   =  0,
	MODELFIT_DISAGREE  = -1,   // its frames carry different Offsets (several blocks, or Offset after a frame line)
	MODELFIT_INHERITED = -2,   // its frames come from `inherits`
};

// Called by the MODELDEF parser.
void ModelFit_ClearSources();
void ModelFit_RecordBlock(const ModelDefBlockSource &src);

// The class's current Offset and a MODELFIT_ status.
int  ModelFit_GetOffset(const PClass *cls, FVector3 &out);
// Writes Offset into every frame of the class and its base frame. Returns how
// many frames changed, or the (<= 0) status when it refuses. The first time a
// class is touched, its original Offset is kept for RestoreOffset.
int  ModelFit_SetOffset(const PClass *cls, const FVector3 &ofs);
void ModelFit_RestoreOffset(const PClass *cls);
