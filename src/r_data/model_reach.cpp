/*
** model_reach.cpp
**
** RS FORK -- DRAW-TIME JOINT POSES AND REACH CHAINS. See model_reach.h for what the
** two capabilities are and the contract they keep; model_reach_math.h for the math
** (VR_BODY_IK_RETURN_PLAN.md section 3g, reference solver v3, and the 4b additions).
**
** This file is the state and the draw path:
**
**   - A render-side table, one entry per actor that carries a pose or a chain. It is
**     NOT on AActor and NOT on DActorModelData, so nothing about it is ever saved and
**     no actor gains model data just to hold it (review section 3). GC-safe: a marker function
**     marks every actor pointer in it each collection, GC::Mark nulls a destroyed
**     actor's pointer before the sweep can free it, and every assignment goes through
**     GC::WriteBarrier -- the pattern the engine's other native roots use.
**
**   - The apply: RenderModelFrame hands over a finished bone palette. Local edits are
**     composed on it (FJointPoseWork) and only the joints under an edit are rewritten.
**
**   - The reach solve, once per displayed frame per chain (DFrameBuffer::FrameCount): the
**     second eye, a mirror or a portal redraw of the same actor that frame re-applies the
**     cached local edits to its own palette.
**
**   - CLEARANCE (4b idea 6): a chain with <tuning>_clear_radius swings its elbow round its exact
**     circle out of the solid regions a read-only level query finds around it
**     (level_solid_query.h).
**
**   - TARGET JOINT AIM (4b idea 1): a chain may turn a joint of the model it REACHES along its
**     solved end bone (the RS hand's wrist stub along the forearm). The two models are drawn in
**     no fixed order, so the solve is a pure function (ReachSolveChain) of the pose the chain's
**     model was last drawn in, both models' drawn matrices, the tuning, the previous FRAME's
**     smoothing and the level: whichever draw needs the frame's solve first computes it, and the
**     chain's own draw computes it again only when its fresh pose or matrices differ. The aim is
**     frozen for the frame, so both eyes turn the joint identically.
**
** SPACES, because mixing them is what cost the 09-04 attempt its night:
**   joint space   -- the model's IQM file space: where baseframe and the joints' global
**                    transforms live and where the solve runs. Finv = swapYZ *
**                    objectToWorld^-1 maps the world into it: the exact inverse of the
**                    matrix this draw uses, never a rebuilt un-yaw (review guard 1).
**   model space   -- the renderer's: file (x, z, y), every IQM/MD3/MDL vertex as loaded.
**                    What ModelPointToWorld and FollowActorOfsInModel take, and what
**                    every vector a script hands the natives below is in.
**   world         -- GL layout (x, up, y), what ObjectToWorldMatrix produces.
**   map           -- Doom (x, y, z up), what the level query speaks.
**
**---------------------------------------------------------------------------
*/

#include <cmath>
#include "model_reach.h"
#include "model_reach_math.h"
#include "level_solid_query.h"
#include "actor.h"
#include "actorinlines.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_game.h"
#include "models.h"
#include "model.h"
#include "c_cvars.h"
#include "i_time.h"
#include "v_video.h"
#include "v_text.h"
#include "printf.h"
#include "vm.h"

using namespace ModelReach;

int  ModelDrawPose_RegisteredCount = 0;
bool ModelDrawPose_TestOn = false;

// ---- the test channel and the trace ----------------------------------------------
//
// r_jointpose_test names a joint; every world model drawn that has it turns that joint
// by r_jointpose_test_deg about its own local axis r_jointpose_test_axis (0 X, 1 Y,
// 2 Z). Renderer-read, so it answers with the menu or console open. It exists for the
// plan's steps 1a/1b: prove the pose layer on a non-decoupled model and on a decoupled
// one (an RS hand finger) with no mod change, and confirm script readback of that joint
// does not move. Empty, the default, is off. Not archived: it never reaches an ini.
CUSTOM_CVAR(String, r_jointpose_test, "", 0)
{
	const char *s = self;
	ModelDrawPose_TestOn = (s != nullptr && s[0] != 0);
}
CVAR(Float, r_jointpose_test_deg, 0.f, 0)
CVAR(Int, r_jointpose_test_axis, 0, 0)

// r_reachchain_debug: once a second per chain, what the solve was fed and what it
// drew -- or why it did not solve -- and, per aimed target joint, what it turned. The
// fastest ground truth for step 3, the way vr_place_debug is for the sliders. Not archived.
CVAR(Bool, r_reachchain_debug, false, 0)

namespace
{

constexpr int REACH_CHAINS = 4;
constexpr int JOINT_POSES  = 64;
constexpr int POSE_CACHES  = 4;

// Clearance: how many level regions one solve may use, and how deep a face slab reaches behind
// its face (map units; level_solid_query.h).
constexpr unsigned REACH_REGIONS    = 128;
constexpr double   REACH_SLAB_DEPTH = 16.0;

// A smoothing history older than this is not continued (a chain not drawn for a while).
constexpr uint64_t REACH_HISTORY_MS = 250;

// Rotation modes (mirror BoneOverride's 1/2), plus hide.
enum
{
	JDP_CLEAR    = 0,
	JDP_MULTIPLY = 1,	// local rotation = drawn local rotation * q
	JDP_REPLACE  = 2,	// local rotation = q
	JDP_HIDE     = 3,	// local scale 0: the joint and everything under it collapse (review item 17)
};

// Translation modes (mirror BoneOverride's translation 1/2).
enum
{
	JDO_CLEAR   = 0,
	JDO_ADD     = 1,	// local translation = drawn local translation + offset
	JDO_REPLACE = 2,	// local translation = offset
};

// ONE ENTRY PER JOINT, CARRYING A ROTATION AND A TRANSLATION. Stage one only fills
// rotations, but a bend about a point that is not the joint's own origin (a wrist stub
// turning about the palm) needs both, so the shape is not rotation-only.
struct FJointDrawPose
{
	FName       joint = NAME_None;
	int         modelIndex = 0;
	int         rotMode = JDP_CLEAR;
	FQuaternion rotation = FQuaternion(0.f, 0.f, 0.f, 1.f);
	int         ofsMode = JDO_CLEAR;
	FVector3    offset = FVector3(0.f, 0.f, 0.f);	// in the joint's PARENT's local units, like a bone translation
};

struct FReachChain
{
	bool  used = false;
	FName root = NAME_None, mid = NAME_None, end = NAME_None;
	FName tuning = NAME_None;
	int   modelIndex = 0;

	// The rig's own directions, as handed in (model space). Pole = outward * _pole_out
	// + down * _pole_down + back * _pole_back; the arm's side is the sign of outward.
	FVector3 outward = FVector3(0, 0, 0), down = FVector3(0, 0, 0), back = FVector3(0, 0, 0);
	FVector3 twistRef = FVector3(0, 0, 0);	// end bone's reference roll (index side) at the model's animated pose

	TObjPtr<AActor*> target = MakeObjPtr<AActor*>(nullptr);
	FVector3 point = FVector3(0, 0, 0);			// on the target's model, target model space and units
	FVector3 fingerDir = FVector3(0, 0, 0);		// target model space: from point toward the fingers. Zero = no swivel alignment
	FVector3 targetTwistRef = FVector3(0, 0, 0);	// target model space: the target's index side. Zero = no twist
	FName    pointCVar = NAME_None;				// <pointCVar>_ofs_x/_y/_z added to point, target MODEL units
	FName    follow = NAME_None;

	// ---- the target joint aim (idea 1): a joint of the TARGET's model turned along this chain ----
	FName    aimJoint = NAME_None;				// on the target's model; None = no aim
	int      aimModelIndex = 0;					// which of the target's models
	FVector3 aimPivot = FVector3(0, 0, 0);		// target model space, at rest
	FVector3 aimAxis = FVector3(0, 0, 0);		// target model space, at rest; zero = minus fingerDir
	float    aimMaxDeg = 70.f;					// <tuning>_aim_max, when it exists, wins live
	bool     aimKeepChildren = true;			// the joint's direct children keep their drawn place

	// ---- render-side state (never saved) ----
	// The pose this chain's model was last drawn in (joint space): what lets the TARGET's draw
	// solve the chain when it is drawn first.
	bool            poseValid = false;
	int             poseGeneration = -1;
	FReachChainPose pose;

	// This frame's solve, and what it was solved from.
	bool            solveValid = false;
	uint64_t        solveFrame = 0;
	uint64_t        solveMs = 0;
	int             solveGeneration = -1;
	FReachChainPose solvePose;
	VSMatrix        solveO2W, solveTargetO2W;
	FReachSolveOut  solve;
	int             solveRegions = 0;
	bool            solveRegionsCapped = false;
	float           solveClearRadius = 0.f;

	// The previous frame's smoothed values: the time smoothing only.
	bool     histValid = false;
	float    histPhi = 0.f, histTwist = 0.f, histClearOfs = 0.f;
	uint64_t histMs = 0;

	// The aim, frozen for the frame it was first computed in.
	bool      aimFrameValid = false;
	uint64_t  aimFrame = 0;
	FModel   *aimFrozenModel = nullptr;
	int       aimFrozenJoints = -1;
	int       aimFrozenGeneration = -1;
	int       aimFrozenReason = 0;
	FReachAim aim;
	FModel   *aimResolvedModel = nullptr;
	int       aimResolvedJoints = -1;
	int       aimResolvedGeneration = -1;
	int       aimJointIndex = -1;

	// ---- joint resolution, per drawn model ----
	FModel     *resolvedModel = nullptr;
	int         resolvedJoints = -1;
	int         resolvedGeneration = -1;
	bool        valid = false;
	int         jRoot = -1, jMid = -1, jEnd = -1, jFollow = -1;
	TArray<int> upperSet;	// root and every joint under it that is not under mid, ascending
	TArray<int> lowerSet;	// mid and every joint under it that is not under end, ascending

	// ---- trace throttle ----
	uint64_t traceMs = 0;
	int      traceReason = -1;
	uint64_t aimTraceMs = 0;
	int      aimTraceReason = -1;

	void ResetHistory()
	{
		histValid = false;
		histPhi = histTwist = histClearOfs = 0.f;
		histMs = 0;
		solveValid = false;
	}
};

// The local edits one model index's solve produced, reused for the rest of that frame.
struct FPoseCache
{
	bool     valid = false;
	uint64_t frame = 0;
	FModel  *model = nullptr;
	int      modelIndex = -1;
	int      joints = -1;
	int      generation = -1;
	TArray<FJointEdit> edits;
};

struct FDrawPoseEntry
{
	TObjPtr<AActor*> actor = MakeObjPtr<AActor*>(nullptr);
	TArray<FJointDrawPose> poses;
	FReachChain chains[REACH_CHAINS];
	int generation = 0;		// bumped by every setter that changes anything

	FModel     *poseModel = nullptr;
	int         poseModelJoints = -1;
	int         poseGeneration = -1;
	TArray<int> poseJoints;	// parallel to poses; -1 = not on this model

	FPoseCache caches[POSE_CACHES];

	bool HasContent() const
	{
		if (poses.Size() > 0) return true;
		for (const auto &c : chains) if (c.used) return true;
		return false;
	}
};

TArray<FDrawPoseEntry> Entries;
bool MarkerAdded = false;

void MarkDrawPoseTable()
{
	for (auto &e : Entries)
	{
		GC::Mark(e.actor);
		for (auto &c : e.chains) GC::Mark(c.target);
	}
}

// Found by identity against an actor the caller knows is live (the one being drawn,
// or self in a native), so the stored pointer is compared, never dereferenced.
int FindEntryIndex(const AActor *a)
{
	if (a == nullptr) return -1;
	for (unsigned i = 0; i < Entries.Size(); i++)
		if (Entries[i].actor.ForceGet() == a) return (int)i;
	return -1;
}

void UpdateRegisteredCount()
{
	ModelDrawPose_RegisteredCount = (int)Entries.Size();
}

void CompactEntries()
{
	for (int i = (int)Entries.Size() - 1; i >= 0; i--)
		if (Entries[i].actor.Get() == nullptr || !Entries[i].HasContent()) Entries.Delete(i);
	UpdateRegisteredCount();
}

FDrawPoseEntry *EntryFor(AActor *a, bool create)
{
	const int i = FindEntryIndex(a);
	if (i >= 0) return &Entries[i];
	if (!create || a == nullptr) return nullptr;
	if (!MarkerAdded)
	{
		GC::AddMarkerFunc(MarkDrawPoseTable);
		MarkerAdded = true;
	}
	CompactEntries();
	Entries.Push(FDrawPoseEntry());
	FDrawPoseEntry &e = Entries.Last();
	GC::WriteBarrier(a);
	e.actor = a;
	UpdateRegisteredCount();
	return &e;
}

void DropIfEmpty(AActor *a)
{
	const int i = FindEntryIndex(a);
	if (i >= 0 && !Entries[i].HasContent())
	{
		Entries.Delete(i);
		UpdateRegisteredCount();
	}
}

const char *ActorName(const AActor *a)
{
	return a ? a->GetClass()->TypeName.GetChars() : "(none)";
}

// PLACEMENT-STYLE CVARS ARE `user` CVARS: read through GetCVar(consoleplayer, ...) or the
// value is 0 -- the trap models.cpp's GetPlacementCVar documents. An absent cvar keeps the
// default rather than reading as zero. The literal name "None" is no prefix (the
// PlacementPrefix = 'None' trap in ObjectToWorldMatrix).
bool PrefixSet(FName prefix)
{
	return prefix != NAME_None && stricmp(prefix.GetChars(), "None") != 0;
}

float TuningCVar(FName prefix, const char *suffix, float def)
{
	if (!PrefixSet(prefix)) return def;
	char nm[192];
	snprintf(nm, sizeof(nm), "%s%s", prefix.GetChars(), suffix);
	FBaseCVar *cv = GetCVar(consoleplayer, nm);
	if (cv == nullptr) return def;
	const float v = (float)cv->GetGenericRep(CVAR_Float).Float;
	return std::isfinite(v) ? v : def;
}

inline FVector3 ToJoint(const FVector3 &m) { return SwapYZ(m); }
inline bool     NonZero(const FVector3 &v) { return ReachNonZero(v); }

bool SameMatrix(const VSMatrix &a, const VSMatrix &b)
{
	return memcmp(a.get(), b.get(), sizeof(FLOATTYPE) * 16) == 0;
}

// Joint parents for a model, checked against the IQM loader's rule (a parent always
// precedes its child), which FJointPoseWork relies on.
bool ReadParents(FModel *model, int n, TArray<int> &parents)
{
	parents.Resize(n);
	for (int i = 0; i < n; i++)
	{
		const int p = model->GetJointParent(i);
		if (p >= i) return false;
		parents[i] = p < 0 ? -1 : p;
	}
	return true;
}

// ---- the render window ------------------------------------------------------------------

struct FScopeState
{
	bool          open = false;
	const AActor *actor = nullptr;
	VSMatrix      objectToWorld;
	double        ticFrac = 0.;
};

thread_local FScopeState Scope;

// ---- piece A: joint draw poses --------------------------------------------------------------

void ResolvePoses(FDrawPoseEntry &e, FModel *model, const AActor *actor)
{
	const int n = model->NumJoints();
	if (e.poseModel == model && e.poseModelJoints == n && e.poseGeneration == e.generation
		&& e.poseJoints.Size() == e.poses.Size()) return;
	e.poseModel = model;
	e.poseModelJoints = n;
	e.poseGeneration = e.generation;
	e.poseJoints.Resize(e.poses.Size());
	for (unsigned k = 0; k < e.poses.Size(); k++)
	{
		const int j = model->FindJoint(e.poses[k].joint);
		e.poseJoints[k] = (j >= 0 && j < n) ? j : -1;
		if (e.poseJoints[k] < 0)
			Printf(TEXTCOLOR_YELLOW "[JOINTPOSE] %s: joint '%s' is not on %s -- that pose is ignored on this model\n",
				ActorName(actor), e.poses[k].joint.GetChars(), model->mFileName.GetChars());
	}
}

void ApplyPoses(FDrawPoseEntry &e, FJointPoseWork &w, int modelIndex)
{
	for (unsigned k = 0; k < e.poses.Size(); k++)
	{
		const FJointDrawPose &p = e.poses[k];
		const int j = e.poseJoints[k];
		if (p.modelIndex != modelIndex || j < 0) continue;
		TRS t;
		if (!w.LocalCur(j, t)) continue;
		switch (p.rotMode)
		{
		case JDP_MULTIPLY: t.rotation = (t.rotation * p.rotation).Unit(); break;
		case JDP_REPLACE:  t.rotation = p.rotation; break;
		case JDP_HIDE:     t.scaling = FVector3(0.f, 0.f, 0.f); break;
		default: break;
		}
		switch (p.ofsMode)
		{
		case JDO_ADD:     t.translation = t.translation + p.offset; break;
		case JDO_REPLACE: t.translation = p.offset; break;
		default: break;
		}
		w.SetLocal(j, t);
	}
}

void ApplyTestChannel(FJointPoseWork &w, FModel *model)
{
	const char *name = r_jointpose_test;
	if (name == nullptr || name[0] == 0) return;
	FName fn(name, true);
	if (fn == NAME_None) return;
	const int j = model->FindJoint(fn);
	if (j < 0 || j >= w.Count()) return;
	const int axis = clamp((int)r_jointpose_test_axis, 0, 2);
	const FVector3 ax(axis == 0 ? 1.f : 0.f, axis == 1 ? 1.f : 0.f, axis == 2 ? 1.f : 0.f);
	TRS t;
	if (!w.LocalCur(j, t)) return;
	t.rotation = (t.rotation * AxisAngleRad(ax, (float)r_jointpose_test_deg * kDegToRad)).Unit();
	w.SetLocal(j, t);
}

// ---- piece B: reach chains -------------------------------------------------------------------

enum EReachOutcome
{
	REACH_SOLVED = 0,
	REACH_UNRESOLVED,
	REACH_NOTARGET,
	REACH_TARGETNOMODEL,
	REACH_SINGULAR,
	REACH_DEGENERATE,
	REACH_NOLOCAL,
	REACH_COUNT
};

const char *const ReachOutcomeText[REACH_COUNT] =
{
	"solved",
	"its joints are not usable on this model (see the [REACH] resolve line)",
	"no target actor",
	"the target has no model frame to reach",
	"this model's drawn matrix is singular",
	"degenerate geometry (zero-length bone or target on the root joint)",
	"a joint's drawn local transform is undefined",
};

bool ResolveChain(FDrawPoseEntry &e, int ci, FModel *model, const AActor *actor, const TArray<int> &parents)
{
	FReachChain &c = e.chains[ci];
	const int n = model->NumJoints();
	if (c.resolvedModel == model && c.resolvedJoints == n && c.resolvedGeneration == e.generation) return c.valid;

	c.resolvedModel = model;
	c.resolvedJoints = n;
	c.resolvedGeneration = e.generation;
	c.valid = false;
	c.jFollow = -1;
	c.upperSet.Clear();
	c.lowerSet.Clear();
	c.ResetHistory();
	c.poseValid = false;

	const char *file = model->mFileName.GetChars();
	c.jRoot = model->FindJoint(c.root);
	c.jMid  = model->FindJoint(c.mid);
	c.jEnd  = model->FindJoint(c.end);
	auto inRange = [n](int j) { return j >= 0 && j < n; };

	// ALL OR NOTHING for the three chain joints (review guard 16), and in order.
	if (!inRange(c.jRoot) || !inRange(c.jMid) || !inRange(c.jEnd))
	{
		Printf(TEXTCOLOR_YELLOW "[REACH] %s chain %d: root '%s' %s, mid '%s' %s, end '%s' %s on %s -- the chain will not solve on this model\n",
			ActorName(actor), ci,
			c.root.GetChars(), inRange(c.jRoot) ? "found" : "MISSING",
			c.mid.GetChars(), inRange(c.jMid) ? "found" : "MISSING",
			c.end.GetChars(), inRange(c.jEnd) ? "found" : "MISSING", file);
		return false;
	}
	if (!IsUnderJoint(parents.Data(), c.jMid, c.jRoot) || !IsUnderJoint(parents.Data(), c.jEnd, c.jMid))
	{
		Printf(TEXTCOLOR_YELLOW "[REACH] %s chain %d: on %s, '%s' must hang under '%s' and '%s' under '%s' -- the chain will not solve\n",
			ActorName(actor), ci, file, c.mid.GetChars(), c.root.GetChars(), c.end.GetChars(), c.mid.GetChars());
		return false;
	}

	// The two bones' segments: every joint that rides each bone (section 3g joint_transforms).
	{
		static thread_local std::vector<int> upper, lower;
		ReachSegmentSets(parents.Data(), n, c.jRoot, c.jMid, c.jEnd, upper, lower);
		for (int j : upper) c.upperSet.Push(j);
		for (int j : lower) c.lowerSet.Push(j);
	}
	c.valid = true;

	if (c.follow != NAME_None)
	{
		const int j = model->FindJoint(c.follow);
		if (inRange(j) && IsUnderJoint(parents.Data(), c.jRoot, j)) c.jFollow = j;
		else Printf(TEXTCOLOR_YELLOW "[REACH] %s chain %d: follow joint '%s' is not an ancestor of '%s' on %s -- no follow\n",
			ActorName(actor), ci, c.follow.GetChars(), c.root.GetChars(), file);
	}
	Printf("[REACH] %s chain %d resolved on %s: root %d mid %d end %d, %u joints ride the root bone and %u the mid bone, follow %s\n",
		ActorName(actor), ci, file, c.jRoot, c.jMid, c.jEnd, c.upperSet.Size(), c.lowerSet.Size(), c.jFollow >= 0 ? "on" : "none");
	return true;
}

// An actor's model matrix exactly as its own draw builds it. The position the sprite pass hands
// RenderModel, as ModelFollowFrame builds a followed parent's (models.cpp); a model riding a
// controller or another model never reads it (ObjectToWorldMatrix skips the world translate).
bool TargetMatrix(AActor *t, double ticFrac, VSMatrix &out)
{
	FSpriteModelFrame *smf = FindModelFrame(t, t->sprite, t->frame, false);
	if (smf == nullptr) return false;
	DVector3 pos = t->InterpolatedPosition(ticFrac)
		+ DVector3(t->WorldOffset.X, t->WorldOffset.Y, t->WorldOffset.Z);
	if ((t->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE) pos.Z -= t->Floorclip;
	out = smf->ObjectToWorldMatrix(t, (float)pos.X, (float)pos.Y, (float)pos.Z, ticFrac);
	return true;
}

struct FReachTrace
{
	FReachSolveOut out;
	FVector3 shoulder, target;
	int      regions = 0;
	bool     capped = false;
	float    clearRadius = 0.f;
	bool     reused = false;
};

// A solve from an earlier frame becomes the smoothing history of this one.
void BeginChainFrame(FReachChain &c, uint64_t frame)
{
	if (c.solveValid && c.solveFrame != frame)
	{
		c.histValid = true;
		c.histPhi = c.solve.phiAligned;
		c.histTwist = c.solve.twistDeg;
		c.histClearOfs = c.solve.clearOfs;
		c.histMs = c.solveMs;
		c.solveValid = false;
	}
}

// The clearance regions around a chain, in its joint space (idea 6): a read-only level query in map
// units around everything the chain can reach this frame, each plane carried through the chain's own
// drawn matrix. Returns the regions' count; `capped` when the query cut its answer short.
int GatherClearance(const AActor *actor, const FReachChainPose &pose, const VSMatrix &o2w, float radiusJ, float stretchMax,
	std::vector<FReachSolidRegion> &regions, bool &capped)
{
	regions.clear();
	capped = false;
	if (actor == nullptr || actor->Level == nullptr || !(radiusJ > 0.f)) return 0;

	// Everything the chain can reach, joint units: both bones at the stretch cap, the radius, and how far a
	// follow can carry the shoulder. The world scale is the largest of the matrix's three axes.
	const float cap = Clampf(std::isfinite(stretchMax) ? stretchMax : 1.f, 1.f, 2.5f);
	const FVector3 centreJ = pose.hasFollow ? pose.followPos : pose.shoulder;
	const float reachJ = ((float)(pose.elbow - pose.shoulder).Length() + (float)(pose.wrist - pose.elbow).Length()) * cap
		+ radiusJ + (pose.hasFollow ? (float)(pose.shoulder - pose.followPos).Length() : 0.f);
	const FLOATTYPE *d = o2w.get();
	const double scale = std::max({ sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]),
		sqrt(double(d[4]) * d[4] + double(d[5]) * d[5] + double(d[6]) * d[6]),
		sqrt(double(d[8]) * d[8] + double(d[9]) * d[9] + double(d[10]) * d[10]) });
	const FVector3 cw = MatPoint(o2w, SwapYZ(centreJ));
	if (!Finite3(cw) || !(scale > 0.) || !std::isfinite(scale)) return 0;

	static thread_local TArray<FLevelSolidRegion> world;
	capped = !LevelSolidQuery(actor->Level, DVector3(cw.X, cw.Z, cw.Y), double(reachJ) * scale, REACH_SLAB_DEPTH, world, REACH_REGIONS);

	regions.reserve(world.Size());
	for (const FLevelSolidRegion &wr : world)
	{
		FReachSolidRegion jr;
		bool ok = wr.planes > 0;
		for (int i = 0; i < wr.planes && i < kReachRegionPlanes && ok; i++)
		{
			// map (x, y, z) -> world (x, z, y): the same swap on the normal keeps n . p.
			ok = ReachPlaneToJoint(o2w, wr.n[i].X, wr.n[i].Z, wr.n[i].Y, wr.c[i], jr.n[i], jr.c[i]);
			jr.planes = i + 1;
		}
		if (ok) regions.push_back(jr);
	}
	return (int)regions.size();
}

// The chain's solve for `frame`, from `pose` and the two drawn matrices; stored on the chain. The
// caller owns the history: a failure here leaves it as it was.
int ComputeChainSolve(FDrawPoseEntry &e, int ci, const AActor *actor, const FReachChainPose &pose, const VSMatrix &o2w,
	const VSMatrix &handM, uint64_t frame, FReachTrace &tr)
{
	FReachChain &c = e.chains[ci];

	// Finv = swapYZ * objectToWorld^-1, the exact inverse of the chain model's drawn matrix (guard 1),
	// so its own fit scale, placement sliders and pixel stretch are all undone with it. A singular
	// matrix holds the drawn pose (guard 5).
	VSMatrix o2wc = o2w, armInv;
	if (!o2wc.inverseMatrix(armInv)) return REACH_SINGULAR;
	VSMatrix Finv;
	Finv.loadMatrix(kSwapYZ);
	Finv.multMatrix(armInv);

	// ---- tuning, read by the renderer every solve (absent cvar = section 3g / 4b default) ----
	const FName tn = c.tuning;
	FReachFrameIn f;
	f.t.stretchMax    = TuningCVar(tn, "_stretch_max",     f.t.stretchMax);
	f.t.softStart     = TuningCVar(tn, "_soft_start",      f.t.softStart);
	f.t.align         = TuningCVar(tn, "_align",           f.t.align);
	f.t.alignMax      = TuningCVar(tn, "_align_max",       f.t.alignMax);
	f.t.fadeLo        = TuningCVar(tn, "_align_fade_lo",   f.t.fadeLo);
	f.t.fadeSpan      = TuningCVar(tn, "_align_fade_span", f.t.fadeSpan);
	f.t.confLo        = TuningCVar(tn, "_align_conf_lo",   f.t.confLo);
	f.t.confSpan      = TuningCVar(tn, "_align_conf_span", f.t.confSpan);
	f.t.twist         = TuningCVar(tn, "_twist",           f.t.twist);
	f.t.twistTaper    = TuningCVar(tn, "_twist_taper",     f.t.twistTaper);
	f.t.twistConfLo   = TuningCVar(tn, "_twist_conf_lo",   f.t.twistConfLo);
	f.t.twistConfSpan = TuningCVar(tn, "_twist_conf_span", f.t.twistConfSpan);
	f.t.twistOfs      = TuningCVar(tn, "_twist_ofs",       f.t.twistOfs);
	f.poleOut    = TuningCVar(tn, "_pole_out",    f.poleOut);
	f.poleDown   = TuningCVar(tn, "_pole_down",   f.poleDown);
	f.poleBack   = TuningCVar(tn, "_pole_back",   f.poleBack);
	f.swivelRate = TuningCVar(tn, "_swivel_rate", 0.0f);
	f.twistRate  = TuningCVar(tn, "_twist_rate",  0.0f);
	f.follow     = TuningCVar(tn, "_follow",      f.follow);
	f.followMax  = TuningCVar(tn, "_follow_max",  f.followMax);
	// Clearance (idea 6). _clear_radius is in this model's own units, so it scales with the arm's fit.
	const float clearRadius = TuningCVar(tn, "_clear_radius", 0.0f);
	f.clearRate  = TuningCVar(tn, "_clear_rate",  0.0f);

	// ---- the target, as drawn: the point in the target's model units ----
	FVector3 pointM = c.point;
	if (PrefixSet(c.pointCVar))
	{
		pointM.X += TuningCVar(c.pointCVar, "_ofs_x", 0.f);
		pointM.Y += TuningCVar(c.pointCVar, "_ofs_y", 0.f);
		pointM.Z += TuningCVar(c.pointCVar, "_ofs_z", 0.f);
	}
	f.target = MatPoint(Finv, MatPoint(handM, pointM));
	if (!Finite3(f.target)) return REACH_DEGENERATE;
	tr.target = f.target;
	tr.shoulder = pose.shoulder;
	// Directions go through the same two 3x3s and are then made unit, so neither model's
	// scale or mirror changes what they mean.
	f.fingerDir = NonZero(c.fingerDir) ? UnitOr(MatDir(Finv, MatDir(handM, c.fingerDir)), FVector3(0, 0, 0)) : FVector3(0, 0, 0);
	f.targetTwistRef = NonZero(c.targetTwistRef) ? UnitOr(MatDir(Finv, MatDir(handM, c.targetTwistRef)), FVector3(0, 0, 0)) : FVector3(0, 0, 0);
	f.outward  = UnitOr(ToJoint(c.outward), FVector3(0, 0, 0));
	f.down     = UnitOr(ToJoint(c.down),    FVector3(0, 0, 0));
	f.back     = UnitOr(ToJoint(c.back),    FVector3(0, 0, 0));
	f.twistRef = ToJoint(c.twistRef);

	// ---- the clock: the previous frame's values, for the time smoothing only ----
	const uint64_t nowMs = screen != nullptr ? screen->FrameTime : 0;
	f.histValid = c.histValid && nowMs >= c.histMs && nowMs - c.histMs <= REACH_HISTORY_MS;
	f.dt = f.histValid ? float(nowMs - c.histMs) / 1000.f : 0.f;
	f.histPhi = c.histPhi;
	f.histTwist = c.histTwist;
	f.histClearOfs = c.histClearOfs;

	// ---- clearance regions ----
	static thread_local std::vector<FReachSolidRegion> regions;
	regions.clear();
	bool capped = false;
	if (clearRadius > 0.f)
	{
		GatherClearance(actor, pose, o2w, clearRadius, f.t.stretchMax, regions, capped);
		f.clear.regions = regions.data();
		f.clear.count = (int)regions.size();
		f.clear.radius = clearRadius;
		f.clear.maxDeg = TuningCVar(tn, "_clear_max", 100.0f);
	}
	tr.regions = (int)regions.size();
	tr.capped = capped;
	tr.clearRadius = clearRadius;

	if (!ReachSolveChain(pose, f, tr.out)) return REACH_DEGENERATE;

	c.solveValid = true;
	c.solveFrame = frame;
	c.solveMs = nowMs;
	c.solveGeneration = e.generation;
	c.solvePose = pose;
	c.solveO2W = o2w;
	c.solveTargetO2W = handM;
	c.solve = tr.out;
	c.solveRegions = tr.regions;
	c.solveRegionsCapped = capped;
	c.solveClearRadius = clearRadius;
	return REACH_SOLVED;
}

// The chain model's own draw: the pose as drawn, this frame's solve (reused when it was already
// solved from exactly this pose and these matrices), then the write onto the palette.
int SolveChain(FDrawPoseEntry &e, int ci, FJointPoseWork &w, FModel *model, const AActor *actor,
	const TArray<int> &parents, const FScopeState &sc, uint64_t frame, FReachTrace &tr)
{
	FReachChain &c = e.chains[ci];
	if (!ResolveChain(e, ci, model, actor, parents)) return REACH_UNRESOLVED;

	AActor *tgt = c.target.Get();
	if (tgt == nullptr) { c.ResetHistory(); return REACH_NOTARGET; }

	VSMatrix handM;
	if (!TargetMatrix(tgt, sc.ticFrac, handM)) { c.ResetHistory(); return REACH_TARGETNOMODEL; }

	BeginChainFrame(c, frame);

	// ---- the pose as drawn, before anything is written ----
	FReachChainPose pose;
	const VSMatrix gMid = w.GlobalPosed(c.jMid), gEnd = w.GlobalPosed(c.jEnd);
	pose.shoulder = MatTranslation(w.GlobalPosed(c.jRoot));
	pose.elbow = MatTranslation(gMid);
	pose.wrist = MatTranslation(gEnd);
	pose.midRot = MatRotation(gMid);
	pose.endRot = MatRotation(gEnd);
	pose.midRotAnimated = MatRotation(w.GlobalOrig(c.jMid));
	if (c.jFollow >= 0)
	{
		const VSMatrix gF = w.GlobalPosed(c.jFollow);
		pose.hasFollow = true;
		pose.followPos = MatTranslation(gF);
		pose.followRot = MatRotation(gF);
	}
	c.pose = pose;
	c.poseValid = true;
	c.poseGeneration = e.generation;

	if (c.solveValid && c.solveFrame == frame && c.solveGeneration == e.generation && ReachSamePose(c.solvePose, pose)
		&& SameMatrix(c.solveO2W, sc.objectToWorld) && SameMatrix(c.solveTargetO2W, handM))
	{
		tr.out = c.solve;
		tr.shoulder = pose.shoulder;
		tr.target = c.solve.in.target;
		tr.regions = c.solveRegions;
		tr.capped = c.solveRegionsCapped;
		tr.clearRadius = c.solveClearRadius;
		tr.reused = true;
	}
	else
	{
		const int outcome = ComputeChainSolve(e, ci, actor, pose, sc.objectToWorld, handM, frame, tr);
		if (outcome != REACH_SOLVED) { c.ResetHistory(); return outcome; }
	}
	const FReachSolveOut &s = c.solve;

	// ---- write: the follow, then every joint riding either bone, parents first ----
	if (s.followApplied && c.jFollow >= 0)
		w.SetGlobal(c.jFollow, pose.followPos, (s.followSwing * pose.followRot).Unit());

	// Every joint the solve will place, with its pre-solve global pose, fetched before
	// anything is written (ReachCaptureSegments): no half-posed chain.
	static thread_local std::vector<FReachSegJoint> seg;
	if (!ReachCaptureSegments(w, c.upperSet.Data(), (int)c.upperSet.Size(), c.lowerSet.Data(), (int)c.lowerSet.Size(), c.jEnd, seg))
	{
		c.ResetHistory();
		return REACH_NOLOCAL;
	}
	// Minimal swings from the drawn bone directions; stretch slides a joint along its bone
	// by where it sits (LENGTHEN, never scale); along the mid bone the twist turns a joint
	// in proportion. The end joint takes the whole twist; everything under it rides it.
	ReachWriteSegments(w, seg, c.jEnd, s.endRot0, s.in, s.circle, s.pose, s.twistDeg);
	return REACH_SOLVED;
}

void TraceChain(FReachChain &c, int ci, const AActor *actor, int outcome, const FReachTrace &tr)
{
	const uint64_t now = I_msTime();
	if (outcome == c.traceReason && now - c.traceMs < 1000) return;
	c.traceReason = outcome;
	c.traceMs = now;
	if (outcome != REACH_SOLVED)
	{
		Printf("[REACH] %s chain %d: not solved -- %s\n", ActorName(actor), ci, ReachOutcomeText[outcome]);
		return;
	}
	const FReachCircle &cc = tr.out.circle;
	const FReachArmPose &p = tr.out.pose;
	Printf("[REACH] %s chain %d -> %s: raw %.2f soft %.2f stretch %.3f (bones x%.3f)  bend %.2f fade %.2f conf %.2f swivel %.1f (want %.1f, aligned %.1f)  "
		"twist %.1f (raw %.1f tconf %.2f)  shoulder (%.2f %.2f %.2f) elbow (%.2f %.2f %.2f) wrist (%.2f %.2f %.2f) target (%.2f %.2f %.2f) gap %.3f side %.2f  [joint space]%s\n",
		ActorName(actor), ci, ActorName(c.target.Get()), cc.raw, cc.soft, cc.stretch, cc.sc,
		tr.out.swivel.bend, tr.out.swivel.fade, tr.out.swivel.conf, tr.out.phi, tr.out.swivel.phiDeg, tr.out.phiAligned,
		tr.out.twistDeg, tr.out.twist.rawDeg, tr.out.twist.tconf,
		tr.shoulder.X, tr.shoulder.Y, tr.shoulder.Z, p.elbow.X, p.elbow.Y, p.elbow.Z,
		p.wristSolved.X, p.wristSolved.Y, p.wristSolved.Z, tr.target.X, tr.target.Y, tr.target.Z, p.gap, p.elbowSide,
		tr.reused ? "  (solved earlier this frame by the target's draw)" : "");
	if (tr.clearRadius > 0.f)
		Printf("[REACH] %s chain %d clearance: radius %.2f, %d solid regions%s, penetration %.3f -> %.3f, swivel pushed %.1f\n",
			ActorName(actor), ci, tr.clearRadius, tr.regions, tr.capped ? " (query capped)" : "", tr.out.penBefore, tr.out.penAfter, tr.out.clearOfs);
}

// ---- idea 1: target joint aim ------------------------------------------------------------------

enum EAimOutcome
{
	AIM_TURNED = 0,
	AIM_NOJOINT,
	AIM_NOSOLVE,
	AIM_NOAXIS,
	AIM_COUNT
};

const char *const AimOutcomeText[AIM_COUNT] =
{
	"turned",
	"the joint is not on this model (see the [REACH] aim resolve line)",
	"the chain has no solve this frame (its model has not been drawn yet, or it did not solve)",
	"no axis to aim (axis and fingerDir both zero) or a singular bind",
};

bool AnyAimAt(const AActor *actor, int modelIndex)
{
	for (const auto &e : Entries)
		for (const auto &c : e.chains)
			if (c.used && c.aimJoint != NAME_None && c.aimModelIndex == modelIndex && c.target.ForceGet() == actor) return true;
	return false;
}

int AimOne(FDrawPoseEntry &e, int ci, AActor *chainActor, const AActor *actor, FModel *model, FJointPoseWork &w,
	const TArray<VSMatrix> &base, const FScopeState &sc, uint64_t frame)
{
	FReachChain &c = e.chains[ci];
	const int n = model->NumJoints();

	if (c.aimResolvedModel != model || c.aimResolvedJoints != n || c.aimResolvedGeneration != e.generation)
	{
		c.aimResolvedModel = model;
		c.aimResolvedJoints = n;
		c.aimResolvedGeneration = e.generation;
		const int j = model->FindJoint(c.aimJoint);
		c.aimJointIndex = (j >= 0 && j < n) ? j : -1;
		if (c.aimJointIndex < 0)
			Printf(TEXTCOLOR_YELLOW "[REACH] %s chain %d: aim joint '%s' is not on %s (%s) -- no aim on this model\n",
				ActorName(chainActor), ci, c.aimJoint.GetChars(), model->mFileName.GetChars(), ActorName(actor));
	}
	if (c.aimJointIndex < 0) return AIM_NOJOINT;

	if (!(c.aimFrameValid && c.aimFrame == frame && c.aimFrozenModel == model && c.aimFrozenJoints == n && c.aimFrozenGeneration == e.generation))
	{
		c.aimFrameValid = true;
		c.aimFrame = frame;
		c.aimFrozenModel = model;
		c.aimFrozenJoints = n;
		c.aimFrozenGeneration = e.generation;
		c.aim = FReachAim();
		c.aimFrozenReason = AIM_TURNED;

		// This frame's solve: from the chain model's own draw if that came first, else from here, from the
		// pose it was last drawn in and its matrix as its draw builds it.
		BeginChainFrame(c, frame);
		if (!(c.solveValid && c.solveFrame == frame && c.solveGeneration == e.generation))
		{
			VSMatrix chainM;
			FReachTrace tr;
			if (!c.poseValid || c.poseGeneration != e.generation || !TargetMatrix(chainActor, sc.ticFrac, chainM)
				|| ComputeChainSolve(e, ci, chainActor, c.pose, chainM, sc.objectToWorld, frame, tr) != REACH_SOLVED)
			{
				c.aimFrozenReason = AIM_NOSOLVE;
				return AIM_NOSOLVE;
			}
		}

		const FVector3 toward = ReachAimToward(c.solveO2W, c.solve.pose.lowerDir, sc.objectToWorld);
		const FVector3 axisM = NonZero(c.aimAxis) ? c.aimAxis : -c.fingerDir;
		FVector3 axisDrawn, pivotDrawn;
		if (!NonZero(axisM) || !NonZero(toward)
			|| !ReachAimFrame(w, base[c.aimJointIndex], c.aimJointIndex, UnitOr(ToJoint(axisM), FVector3(0, 0, 0)), ToJoint(c.aimPivot), axisDrawn, pivotDrawn))
		{
			c.aimFrozenReason = AIM_NOAXIS;
			return AIM_NOAXIS;
		}
		c.aim = ReachAimRotation(axisDrawn, toward, TuningCVar(c.tuning, "_aim_max", c.aimMaxDeg), TuningCVar(c.tuning, "_aim", 1.0f));
		c.aim.pivot = pivotDrawn;
		if (!c.aim.valid) c.aimFrozenReason = AIM_NOAXIS;
	}
	if (!c.aim.valid) return c.aimFrozenReason != AIM_TURNED ? c.aimFrozenReason : AIM_NOAXIS;
	ReachApplyAim(w, c.aimJointIndex, c.aim, c.aimKeepChildren);
	return AIM_TURNED;
}

void TraceAim(FReachChain &c, int ci, const AActor *chainActor, const AActor *actor, int outcome)
{
	const uint64_t now = I_msTime();
	if (outcome == c.aimTraceReason && now - c.aimTraceMs < 1000) return;
	c.aimTraceReason = outcome;
	c.aimTraceMs = now;
	if (outcome != AIM_TURNED)
		Printf("[REACH] %s chain %d aim on %s '%s': not turned -- %s\n", ActorName(chainActor), ci, ActorName(actor), c.aimJoint.GetChars(), AimOutcomeText[outcome]);
	else
		Printf("[REACH] %s chain %d aim on %s '%s': turned %.1f of %.1f degrees (cap %.1f) about (%.2f %.2f %.2f) [its joint space]\n",
			ActorName(chainActor), ci, ActorName(actor), c.aimJoint.GetChars(), c.aim.usedDeg, c.aim.wantDeg,
			TuningCVar(c.tuning, "_aim_max", c.aimMaxDeg), c.aim.pivot.X, c.aim.pivot.Y, c.aim.pivot.Z);
}

// Every chain that aims a joint of THIS actor's model `modelIndex`.
void ApplyAims(const AActor *actor, FModel *model, int modelIndex, FJointPoseWork &w, const TArray<VSMatrix> &base,
	const FScopeState &sc, uint64_t frame)
{
	for (unsigned ei = 0; ei < Entries.Size(); ei++)
	{
		FDrawPoseEntry &e = Entries[ei];
		for (int ci = 0; ci < REACH_CHAINS; ci++)
		{
			FReachChain &c = e.chains[ci];
			if (!c.used || c.aimJoint == NAME_None || c.aimModelIndex != modelIndex || c.target.ForceGet() != actor) continue;
			AActor *chainActor = e.actor.Get();
			if (chainActor == nullptr || chainActor == actor) continue;
			const int outcome = AimOne(e, ci, chainActor, actor, model, w, base, sc, frame);
			if (r_reachchain_debug) TraceAim(c, ci, chainActor, actor, outcome);
		}
	}
}

} // namespace

// ---- the draw path ------------------------------------------------------------------------

void FModelDrawPoseScope::Open(const AActor *actor, const VSMatrix &objectToWorld, double ticFrac)
{
	prevOpen = Scope.open;
	prevActor = Scope.actor;
	prevObjectToWorld = Scope.objectToWorld;
	prevTicFrac = Scope.ticFrac;
	Scope.open = true;
	Scope.actor = actor;
	Scope.objectToWorld = objectToWorld;
	Scope.ticFrac = ticFrac;
	opened = true;
}

void FModelDrawPoseScope::Close()
{
	Scope.open = prevOpen;
	Scope.actor = prevActor;
	Scope.objectToWorld = prevObjectToWorld;
	Scope.ticFrac = prevTicFrac;
	opened = false;
}

const TArray<VSMatrix> *ModelDrawPose_ApplyOpen(const AActor *actor, FModel *model, int modelIndex, const TArray<VSMatrix> &bones)
{
	const FScopeState &sc = Scope;
	if (!sc.open || sc.actor != actor || actor == nullptr || model == nullptr) return &bones;

	const int ei = FindEntryIndex(actor);
	const bool aimed = AnyAimAt(actor, modelIndex);
	if (ei < 0 && !aimed && !ModelDrawPose_TestOn) return &bones;

	// Only a rigged palette that matches its own skeleton: a joint-count mismatch is
	// ignored, as CalculateBonesIQM ignores mismatched overrides.
	const int n = model->NumJoints();
	const TArray<VSMatrix> *base = model->GetBasePose();
	if (n <= 0 || base == nullptr || (int)base->Size() != n || (int)bones.Size() != n) return &bones;

	static thread_local TArray<int> parents;
	if (!ReadParents(model, n, parents)) return &bones;

	static thread_local FJointPoseWork work;
	work.Begin(parents.Data(), bones.Data(), base->Data(), n);

	const uint64_t frame = screen != nullptr ? screen->FrameCount : 0;

	if (ei >= 0)
	{
		FDrawPoseEntry &e = Entries[ei];

		FPoseCache *cache = nullptr;
		for (auto &pc : e.caches) if (pc.valid && pc.modelIndex == modelIndex) { cache = &pc; break; }

		if (cache != nullptr && cache->frame == frame && cache->model == model && cache->joints == n
			&& cache->generation == e.generation)
		{
			// ONCE PER FRAME, NOT PER EYE (review item 19): the same local edits on this
			// draw's own palette -- the aims among them.
			for (const auto &ed : cache->edits)
				if (ed.joint >= 0 && ed.joint < n) work.SetLocal(ed.joint, ed.local);
		}
		else
		{
			if (e.poses.Size() > 0)
			{
				ResolvePoses(e, model, actor);
				ApplyPoses(e, work, modelIndex);
			}
			for (int ci = 0; ci < REACH_CHAINS; ci++)
			{
				FReachChain &c = e.chains[ci];
				if (!c.used || c.modelIndex != modelIndex) continue;
				FReachTrace tr;
				const int outcome = SolveChain(e, ci, work, model, actor, parents, sc, frame, tr);
				if (r_reachchain_debug) TraceChain(c, ci, actor, outcome, tr);
			}
			if (aimed) ApplyAims(actor, model, modelIndex, work, *base, sc, frame);
			if (cache == nullptr)
			{
				cache = &e.caches[0];
				for (auto &pc : e.caches) if (!pc.valid) { cache = &pc; break; }
			}
			cache->valid = true;
			cache->frame = frame;
			cache->model = model;
			cache->modelIndex = modelIndex;
			cache->joints = n;
			cache->generation = e.generation;
			cache->edits.Clear();
			for (const auto &ed : work.Edits()) cache->edits.Push(ed);
		}
	}
	else if (aimed)
	{
		// Not a registered actor itself, only reached: the aims are frozen per frame on their chains.
		ApplyAims(actor, model, modelIndex, work, *base, sc, frame);
	}

	if (ModelDrawPose_TestOn) ApplyTestChannel(work, model);

	static thread_local TArray<VSMatrix> posed;
	posed.Resize(n);
	if (!work.FinishInto(posed.Data())) return &bones;
	return &posed;
}

// ---- the ZScript setters ------------------------------------------------------------------
//
// SETTERS ONLY. There is deliberately no getter for a posed joint: script reads bones
// through CalcBones, which never sees this layer, and that is what keeps a pose built
// from this machine's controllers out of anything a playsim decision can read.
//
// Every setter is idempotent: calling it every tic with the same arguments changes
// nothing and invalidates nothing, so a rig may simply re-assert its setup each tic --
// which is also how it survives a savegame load (none of this is saved).

namespace
{

bool ChainIndexOk(int chain)
{
	return chain >= 0 && chain < REACH_CHAINS;
}

bool FiniteVec(double x, double y, double z)
{
	return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}

void Changed(FDrawPoseEntry &e)
{
	e.generation++;
}

// The entry for (joint, modelIndex), created if asked and there is room.
FJointDrawPose *PoseFor(FDrawPoseEntry &e, FName joint, int modelIndex, bool create)
{
	for (auto &p : e.poses)
		if (p.joint == joint && p.modelIndex == modelIndex) return &p;
	if (!create || e.poses.Size() >= JOINT_POSES) return nullptr;
	FJointDrawPose p;
	p.joint = joint;
	p.modelIndex = modelIndex;
	e.poses.Push(p);
	return &e.poses.Last();
}

void DropEmptyPoses(FDrawPoseEntry &e)
{
	for (int k = (int)e.poses.Size() - 1; k >= 0; k--)
		if (e.poses[k].rotMode == JDP_CLEAR && e.poses[k].ofsMode == JDO_CLEAR) e.poses.Delete(k);
}

} // namespace

DEFINE_ACTION_FUNCTION(AActor, SetModelJointDrawPose)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_NAME(joint);
	PARAM_FLOAT(qx);
	PARAM_FLOAT(qy);
	PARAM_FLOAT(qz);
	PARAM_FLOAT(qw);
	PARAM_INT(mode);
	PARAM_INT(modelIndex);

	if (joint == NAME_None || modelIndex < 0 || mode < JDP_CLEAR || mode > JDP_HIDE) ACTION_RETURN_BOOL(false);

	FQuaternion q(0, 0, 0, 1);
	if (mode == JDP_MULTIPLY || mode == JDP_REPLACE)
	{
		q = FQuaternion((float)qx, (float)qy, (float)qz, (float)qw);
		if (!std::isfinite(qx) || !std::isfinite(qy) || !std::isfinite(qz) || !std::isfinite(qw) || q.LengthSquared() < 1.e-12)
			ACTION_RETURN_BOOL(false);
		q.MakeUnit();
	}

	FDrawPoseEntry *e = EntryFor(self, mode != JDP_CLEAR);
	if (e == nullptr) ACTION_RETURN_BOOL(mode == JDP_CLEAR);
	FJointDrawPose *p = PoseFor(*e, joint, modelIndex, mode != JDP_CLEAR);
	if (p == nullptr)
	{
		DropIfEmpty(self);
		ACTION_RETURN_BOOL(mode == JDP_CLEAR);
	}
	if (p->rotMode != mode || !(p->rotation == q))
	{
		p->rotMode = mode;
		p->rotation = q;
		Changed(*e);
	}
	DropEmptyPoses(*e);
	DropIfEmpty(self);
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(AActor, SetModelJointDrawOffset)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_NAME(joint);
	PARAM_FLOAT(ox);
	PARAM_FLOAT(oy);
	PARAM_FLOAT(oz);
	PARAM_INT(mode);
	PARAM_INT(modelIndex);

	if (joint == NAME_None || modelIndex < 0 || mode < JDO_CLEAR || mode > JDO_REPLACE || !FiniteVec(ox, oy, oz))
		ACTION_RETURN_BOOL(false);

	const FVector3 o = mode == JDO_CLEAR ? FVector3(0, 0, 0) : FVector3((float)ox, (float)oy, (float)oz);
	FDrawPoseEntry *e = EntryFor(self, mode != JDO_CLEAR);
	if (e == nullptr) ACTION_RETURN_BOOL(mode == JDO_CLEAR);
	FJointDrawPose *p = PoseFor(*e, joint, modelIndex, mode != JDO_CLEAR);
	if (p == nullptr)
	{
		DropIfEmpty(self);
		ACTION_RETURN_BOOL(mode == JDO_CLEAR);
	}
	if (p->ofsMode != mode || !(p->offset == o))
	{
		p->ofsMode = mode;
		p->offset = o;
		Changed(*e);
	}
	DropEmptyPoses(*e);
	DropIfEmpty(self);
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(AActor, ClearModelJointDrawPose)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_NAME(joint);
	PARAM_INT(modelIndex);

	FDrawPoseEntry *e = EntryFor(self, false);
	if (e != nullptr)
	{
		for (int k = (int)e->poses.Size() - 1; k >= 0; k--)
		{
			if ((joint == NAME_None || e->poses[k].joint == joint) && (modelIndex < 0 || e->poses[k].modelIndex == modelIndex))
			{
				e->poses.Delete(k);
				Changed(*e);
			}
		}
		DropIfEmpty(self);
	}
	return 0;
}

DEFINE_ACTION_FUNCTION(AActor, SetModelReachChain)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_INT(chain);
	PARAM_NAME(rootJoint);
	PARAM_NAME(midJoint);
	PARAM_NAME(endJoint);
	PARAM_NAME(tuning);
	PARAM_INT(modelIndex);

	if (!ChainIndexOk(chain) || rootJoint == NAME_None || midJoint == NAME_None || endJoint == NAME_None || modelIndex < 0)
		ACTION_RETURN_BOOL(false);

	FDrawPoseEntry *e = EntryFor(self, true);
	if (e == nullptr) ACTION_RETURN_BOOL(false);
	FReachChain &c = e->chains[chain];
	if (c.used && c.root == rootJoint && c.mid == midJoint && c.end == endJoint && c.tuning == tuning && c.modelIndex == modelIndex)
		ACTION_RETURN_BOOL(true);
	c.used = true;
	c.root = rootJoint;
	c.mid = midJoint;
	c.end = endJoint;
	c.tuning = tuning;
	c.modelIndex = modelIndex;
	c.ResetHistory();
	Changed(*e);
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(AActor, SetModelReachFrame)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_INT(chain);
	PARAM_FLOAT(ox); PARAM_FLOAT(oy); PARAM_FLOAT(oz);
	PARAM_FLOAT(dx); PARAM_FLOAT(dy); PARAM_FLOAT(dz);
	PARAM_FLOAT(bx); PARAM_FLOAT(by); PARAM_FLOAT(bz);
	PARAM_FLOAT(rx); PARAM_FLOAT(ry); PARAM_FLOAT(rz);

	if (!ChainIndexOk(chain) || !FiniteVec(ox, oy, oz) || !FiniteVec(dx, dy, dz) || !FiniteVec(bx, by, bz) || !FiniteVec(rx, ry, rz))
		ACTION_RETURN_BOOL(false);
	FDrawPoseEntry *e = EntryFor(self, false);
	if (e == nullptr || !e->chains[chain].used) ACTION_RETURN_BOOL(false);	// SetModelReachChain first
	FReachChain &c = e->chains[chain];
	const FVector3 o((float)ox, (float)oy, (float)oz), d((float)dx, (float)dy, (float)dz), b((float)bx, (float)by, (float)bz), r((float)rx, (float)ry, (float)rz);
	if (c.outward == o && c.down == d && c.back == b && c.twistRef == r) ACTION_RETURN_BOOL(true);
	c.outward = o;
	c.down = d;
	c.back = b;
	c.twistRef = r;
	Changed(*e);
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(AActor, SetModelReachTarget)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_INT(chain);
	PARAM_OBJECT(reachTarget, AActor);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(fx); PARAM_FLOAT(fy); PARAM_FLOAT(fz);
	PARAM_FLOAT(tx); PARAM_FLOAT(ty); PARAM_FLOAT(tz);
	PARAM_NAME(pointCVar);

	if (!ChainIndexOk(chain) || !FiniteVec(px, py, pz) || !FiniteVec(fx, fy, fz) || !FiniteVec(tx, ty, tz))
		ACTION_RETURN_BOOL(false);
	if (reachTarget == self) ACTION_RETURN_BOOL(false);	// a model cannot reach a point on itself as drawn
	FDrawPoseEntry *e = EntryFor(self, false);
	if (e == nullptr || !e->chains[chain].used) ACTION_RETURN_BOOL(false);	// SetModelReachChain first
	FReachChain &c = e->chains[chain];
	const FVector3 p((float)px, (float)py, (float)pz), f((float)fx, (float)fy, (float)fz), t((float)tx, (float)ty, (float)tz);
	if (c.target.ForceGet() == reachTarget && c.point == p && c.fingerDir == f && c.targetTwistRef == t && c.pointCVar == pointCVar)
		ACTION_RETURN_BOOL(true);
	if (c.target.ForceGet() != reachTarget)
	{
		c.ResetHistory();
		c.aimFrameValid = false;
	}
	if (reachTarget != nullptr) GC::WriteBarrier(reachTarget);
	c.target = reachTarget;
	c.point = p;
	c.fingerDir = f;
	c.targetTwistRef = t;
	c.pointCVar = pointCVar;
	Changed(*e);
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(AActor, SetModelReachFollowJoint)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_INT(chain);
	PARAM_NAME(joint);

	if (!ChainIndexOk(chain)) ACTION_RETURN_BOOL(false);
	FDrawPoseEntry *e = EntryFor(self, false);
	if (e == nullptr || !e->chains[chain].used) ACTION_RETURN_BOOL(false);	// SetModelReachChain first
	FReachChain &c = e->chains[chain];
	const FName j = PrefixSet(joint) ? joint : FName(NAME_None);
	if (c.follow != j)
	{
		c.follow = j;
		Changed(*e);
	}
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(AActor, SetModelReachTargetJoint)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_INT(chain);
	PARAM_NAME(joint);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(maxDeg);
	PARAM_FLOAT(ax); PARAM_FLOAT(ay); PARAM_FLOAT(az);
	PARAM_BOOL(keepChildren);
	PARAM_INT(targetModelIndex);

	if (!ChainIndexOk(chain) || !FiniteVec(px, py, pz) || !FiniteVec(ax, ay, az) || !std::isfinite(maxDeg) || targetModelIndex < 0)
		ACTION_RETURN_BOOL(false);
	FDrawPoseEntry *e = EntryFor(self, false);
	if (e == nullptr || !e->chains[chain].used) ACTION_RETURN_BOOL(false);	// SetModelReachChain first
	FReachChain &c = e->chains[chain];
	const FName j = PrefixSet(joint) ? joint : FName(NAME_None);
	const FVector3 pivot((float)px, (float)py, (float)pz), axis((float)ax, (float)ay, (float)az);
	const float md = (float)clamp(maxDeg, 0., 180.);
	if (c.aimJoint == j && (j == NAME_None || (c.aimPivot == pivot && c.aimAxis == axis && c.aimMaxDeg == md
		&& c.aimKeepChildren == keepChildren && c.aimModelIndex == targetModelIndex)))
		ACTION_RETURN_BOOL(true);
	c.aimJoint = j;
	c.aimPivot = pivot;
	c.aimAxis = axis;
	c.aimMaxDeg = md;
	c.aimKeepChildren = keepChildren;
	c.aimModelIndex = targetModelIndex;
	c.aimFrameValid = false;
	Changed(*e);
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(AActor, ClearModelReachChain)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_INT(chain);

	FDrawPoseEntry *e = EntryFor(self, false);
	if (e != nullptr)
	{
		for (int ci = 0; ci < REACH_CHAINS; ci++)
		{
			if (chain >= 0 && ci != chain) continue;
			if (e->chains[ci].used)
			{
				e->chains[ci] = FReachChain();
				Changed(*e);
			}
		}
		DropIfEmpty(self);
	}
	return 0;
}
