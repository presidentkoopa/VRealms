// ============================================================================
// GUN FIT MODE -- engine half, stage 1. See model_fit.h.
// ============================================================================

#include "model_fit.h"
#include "model.h"
#include "c_dispatch.h"
#include "printf.h"
#include "dobject.h"
#include "info.h"
#include "actor.h"
#include "vm.h"

TArray<ModelDefBlockSource> ModelDefBlockSources;

// A CLASS'S OFFSET AS IT WAS BEFORE THIS SESSION FIRST TOUCHED IT. RestoreOffset
// puts this back; Cancel in fit mode uses it. Cleared with the model list, so a
// restart or a reload of models starts clean.
static TMap<const PClass *, FVector3> ModelFitOriginals;

void ModelFit_ClearSources()
{
	ModelDefBlockSources.Clear();
	ModelFitOriginals.Clear();
}

void ModelFit_RecordBlock(const ModelDefBlockSource &src)
{
	ModelDefBlockSources.Push(src);
}

static bool AnyInherited(const PClass *cls)
{
	for (auto &b : ModelDefBlockSources)
		if (b.cls == cls && b.inherited) return true;
	return false;
}

//===========================================================================
//
// GetOffset
//
// Every frame of the class must agree. Offset is copied into a frame at the
// moment its frame line is read, so a block that states Offset AFTER some of
// its frame lines, or a class split over several blocks with different
// Offsets, leaves frames that disagree. There is no single number to fit in
// that case, so it is reported rather than averaged or guessed.
//
//===========================================================================

int ModelFit_GetOffset(const PClass *cls, FVector3 &out)
{
	out = { 0.f, 0.f, 0.f };
	if (cls == nullptr) return MODELFIT_NOMODEL;
	if (AnyInherited(cls)) return MODELFIT_INHERITED;

	bool have = false;
	FVector3 first = { 0.f, 0.f, 0.f };
	auto check = [&](const FSpriteModelFrame &f) -> bool
	{
		FVector3 v = { f.xoffset, f.yoffset, f.zoffset };
		if (!have) { first = v; have = true; return true; }
		return v.X == first.X && v.Y == first.Y && v.Z == first.Z;
	};

	for (auto &f : SpriteModelFrames)
	{
		if (f.type != cls || f.isVoxel) continue;
		if (!check(f)) return MODELFIT_DISAGREE;
	}
	if (auto base = BaseSpriteModelFrames.CheckKey(cls))
	{
		if (!check(*base)) return MODELFIT_DISAGREE;
	}
	if (!have) return MODELFIT_NOMODEL;
	out = first;
	return MODELFIT_OK;
}

int ModelFit_SetOffset(const PClass *cls, const FVector3 &ofs)
{
	FVector3 cur;
	int status = ModelFit_GetOffset(cls, cur);
	if (status != MODELFIT_OK) return status <= 0 ? status : 0;

	if (ModelFitOriginals.CheckKey(cls) == nullptr)
		ModelFitOriginals.Insert(cls, cur);

	int changed = 0;
	for (auto &f : SpriteModelFrames)
	{
		if (f.type != cls || f.isVoxel) continue;
		f.xoffset = ofs.X; f.yoffset = ofs.Y; f.zoffset = ofs.Z;
		changed++;
	}
	if (auto base = BaseSpriteModelFrames.CheckKey(cls))
	{
		base->xoffset = ofs.X; base->yoffset = ofs.Y; base->zoffset = ofs.Z;
		changed++;
	}
	return changed;
}

void ModelFit_RestoreOffset(const PClass *cls)
{
	auto orig = ModelFitOriginals.CheckKey(cls);
	if (orig == nullptr) return;
	const FVector3 o = *orig;
	for (auto &f : SpriteModelFrames)
	{
		if (f.type != cls || f.isVoxel) continue;
		f.xoffset = o.X; f.yoffset = o.Y; f.zoffset = o.Z;
	}
	if (auto base = BaseSpriteModelFrames.CheckKey(cls))
	{
		base->xoffset = o.X; base->yoffset = o.Y; base->zoffset = o.Z;
	}
	ModelFitOriginals.Remove(cls);
}

//===========================================================================
//
// Script natives: struct ModelDef (wadsrc/static/zscript/engine/modeldef.zs)
//
//===========================================================================

DEFINE_ACTION_FUNCTION(_ModelDef, GetOffset)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(cls, AActor);
	FVector3 o;
	int status = ModelFit_GetOffset(cls, o);
	if (numret > 1) ret[1].SetInt(status);
	if (numret > 0) ret[0].SetVector(DVector3(o.X, o.Y, o.Z));
	return numret;
}

DEFINE_ACTION_FUNCTION(_ModelDef, SetOffset)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(cls, AActor);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);
	ACTION_RETURN_INT(ModelFit_SetOffset(cls, FVector3((float)x, (float)y, (float)z)));
}

DEFINE_ACTION_FUNCTION(_ModelDef, RestoreOffset)
{
	PARAM_PROLOGUE;
	PARAM_CLASS(cls, AActor);
	ModelFit_RestoreOffset(cls);
	return 0;
}

//===========================================================================
//
// Console: modelfit_nudge / modelfit_list (stage 1's test commands)
//
//===========================================================================

static const char *ModelFitStatusName(int s)
{
	switch (s)
	{
	case MODELFIT_OK:        return "ok";
	case MODELFIT_NOMODEL:   return "no model";
	case MODELFIT_DISAGREE:  return "frames disagree";
	case MODELFIT_INHERITED: return "inherited";
	default:                 return "?";
	}
}

// modelfit_nudge <class> <x> <y> <z> -- moves the class's Offset by that much,
// live, in MODELDEF units and axis order. The stage 1 gate: nudging a gun in
// the hand moves it in the expected direction on each axis, in both hands.
CCMD(modelfit_nudge)
{
	if (argv.argc() < 5)
	{
		Printf("usage: modelfit_nudge <class> <x> <y> <z>   (adds to the class's MODELDEF Offset, live)\n");
		return;
	}
	auto cls = PClass::FindActor(argv[1]);
	if (cls == nullptr) { Printf("modelfit_nudge: no actor class '%s'\n", argv[1]); return; }
	FVector3 cur;
	int status = ModelFit_GetOffset(cls, cur);
	if (status != MODELFIT_OK)
	{
		Printf("modelfit_nudge: %s refused -- %s\n", cls->TypeName.GetChars(), ModelFitStatusName(status));
		return;
	}
	FVector3 n = { cur.X + (float)atof(argv[2]), cur.Y + (float)atof(argv[3]), cur.Z + (float)atof(argv[4]) };
	int changed = ModelFit_SetOffset(cls, n);
	Printf("modelfit_nudge: %s Offset %.4f %.4f %.4f -> %.4f %.4f %.4f (%d frames)\n",
		cls->TypeName.GetChars(), cur.X, cur.Y, cur.Z, n.X, n.Y, n.Z, changed);
}

// modelfit_list [class] -- with a class, every block it was read from: the
// lump, the byte ranges, the Offset it ended with, the CRC. Without, a count.
CCMD(modelfit_list)
{
	if (argv.argc() < 2)
	{
		TMap<const PClass *, bool> seen;
		for (auto &b : ModelDefBlockSources) seen.Insert(b.cls, true);
		Printf("modelfit_list: %u MODELDEF blocks read, %u classes. Give a class name for its blocks.\n",
			ModelDefBlockSources.Size(), seen.CountUsed());
		return;
	}
	auto cls = PClass::FindActor(argv[1]);
	if (cls == nullptr) { Printf("modelfit_list: no actor class '%s'\n", argv[1]); return; }
	FVector3 cur;
	int status = ModelFit_GetOffset(cls, cur);
	Printf("%s: status %s, live Offset %.4f %.4f %.4f\n", cls->TypeName.GetChars(), ModelFitStatusName(status), cur.X, cur.Y, cur.Z);
	int n = 0;
	for (auto &b : ModelDefBlockSources)
	{
		if (b.cls != cls) continue;
		n++;
		Printf("  block in %s  bytes %d-%d  Offset line %d-%d  ZOffset line %d-%d  read Offset %.4f %.4f %.4f  crc %08x%s\n",
			b.lumpName.GetChars(), b.blockStart, b.blockEnd, b.offsetStart, b.offsetEnd, b.zoffsetStart, b.zoffsetEnd,
			b.offset.X, b.offset.Y, b.offset.Z, b.crc, b.inherited ? "  INHERITED" : "");
	}
	if (n == 0) Printf("  no MODELDEF block names this class\n");
}
