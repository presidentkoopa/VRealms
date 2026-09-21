/*
** model.h
**
** General model handling code
**
**---------------------------------------------------------------------------
**
** Copyright 2013-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include <stdint.h>
#include "textureid.h"
#include "i_modelvertexbuffer.h"
#include "matrix.h"
#include "palettecontainer.h"
#include "TRS.h"
#include "tarray.h"
#include "name.h"
#include "fs_files.h"

#include "bonecomponents.h"

class DBoneComponents;
class FModelRenderer;
class FGameTexture;
class IModelVertexBuffer;
class FModel;
class PClass;
class AActor;
struct FSpriteModelFrame;
struct FLevelLocals;

FTextureID LoadSkin(const char* path, const char* fn);
void FlushModels();


extern TDeletingArray<FModel*> Models;
extern TArray<FSpriteModelFrame> SpriteModelFrames;
extern TMap<const PClass*, FSpriteModelFrame> BaseSpriteModelFrames;

#define MD3_MAX_SURFACES	64	// [XR] was 32; the Slayer body ships 64 mesh parts and every one must be addressable by SurfaceSkin
#define MIN_MODELS	4

struct FSpriteModelFrame
{
	uint8_t modelsAmount = 0;
	TArray<int> modelIDs;
	TArray<FTextureID> skinIDs;
	TArray<FTextureID> surfaceskinIDs;
	TArray<int> modelframes;
	TArray<int> animationIDs;
	float xscale, yscale, zscale;
	// [BB] Added zoffset, rotation parameters and flags.
	// Added xoffset, yoffset
	float xoffset, yoffset, zoffset;

	// [BB] THE POINT THE MODEL TURNS ABOUT, IN ITS OWN SPACE.
	//
	// Offset above cannot express this, and the difference is not a nicety.
	// Offset is applied AFTER the rotations, which makes it a rigid displacement
	// in the parent frame: it moves the model without moving the point the model
	// SPINS about. A mesh whose own origin is not where it ought to turn from
	// therefore ORBITS that origin instead of rotating in place, and no value of
	// Offset shrinks that orbit -- it only moves the whole circle somewhere else.
	//
	// This is subtracted BEFORE the rotations instead, which is the ordinary
	// v' = R * (v - p). It is the only way to say "turn about HERE" for a mesh
	// that was not authored centred on the point it should turn from.
	//
	// Zero by default, so every existing model is untouched. MODELDEF keyword is
	// PivotOffset, deliberately spelled to sit next to Offset because the two get
	// confused constantly: Offset moves the model, PivotOffset moves what it
	// rotates around.
	float pivotx = 0.f, pivoty = 0.f, pivotz = 0.f;
	float xrotate, yrotate, zrotate;
	float rotationCenterX, rotationCenterY, rotationCenterZ;
	float rotationSpeed;
	float viewModelFOV;
private:
	unsigned int flags;
public:
	const void* type;	// used for hashing, must point to something usable as identifier for the model's owner.
	short sprite;
	short frame;
	int hashnext;
	float angleoffset;

	// RS FORK -- MOD-OWNED PLACEMENT.
	//
	// Names a CVAR prefix. The six placement values are then read live from
	// <prefix>_ofs_x/_ofs_y/_ofs_z and <prefix>_yaw/_pitch/_roll.
	//
	// The point is WHERE those CVARs live: in the mod's own CVARINFO, with the
	// mod's own MENUDEF page, so adding a tunable weapon needs nothing in the
	// engine and does not put a per-weapon slider in the engine's option tree.
	FName placementCVars = NAME_None;
	// added pithoffset, rolloffset.
	float pitchoffset, rolloffset; // I don't want to bother with type transformations, so I made this variables float.

	// RS FORK -- HAND-FRAME ORIENTATION, the bakeable twin of vr_hand_*.
	//
	// angleoffset/pitchoffset/rolloffset above are applied as one intrinsic
	// triple that orients the model itself. These three are applied AFTER
	// that, in the frame the oriented model leaves behind, and are summed
	// with the live vr_hand_yaw/_pitch/_roll sliders -- the same position,
	// the same order, the same sign.
	//
	// That is the whole point of them existing separately. A value dialled in
	// on a slider can be written into the matching keyword here and mean
	// EXACTLY the same thing, which is what makes a tuning pass permanent.
	// Summing the sliders into pitchoffset instead cannot do that: a model
	// carrying a 90 degree pitchoffset puts that rotation between the yaw and
	// the roll, and a 90 degree turn about Z lands the roll axis on top of the
	// yaw axis -- so both sliders drive one rotation and neither drives the
	// other. Applied here, past the baked pitch, the three stay orthogonal.
	float handangleoffset = 0.f, handpitchoffset = 0.f, handrolloffset = 0.f;
	bool isVoxel;
	unsigned int getFlags(class DActorModelData * defs) const;

	// RS FORK -- read straight off the MODELDEF flags, with no per-actor
	// override applied. The draw path needs this before it has resolved an
	// actor's model data, and no actor overrides it anyway.
	bool ignoresSkinAlpha() const { return !!(flags & (1 << 18)); }
	friend void InitModels();
	friend void ParseModelDefLump(int Lump);

	// followFrameOut, when given, receives the frame a model following THIS one
	// rides (AActor::FollowActor) -- see ModelFollowFrame in models.cpp.
	VSMatrix ObjectToWorldMatrix(AActor * actor, float x, float y, float z, double ticFrac, VSMatrix *followFrameOut = nullptr);
	// bodyPivotZ: height above the actor's origin to turn about, in map units.
	// Zero keeps the historical behaviour of turning about the origin itself --
	// which for anything standing on a floor is the point between its feet, so a
	// held object swings through an arc instead of turning in place. Only the
	// held-voxel path passes anything else.
	// followBodyMode/followBodyOfs come from the ACTOR, not from MODELDEF, so
	// they arrive as arguments rather than as flags -- see AActor::FollowBodyMode.
	// Defaulted off: every caller written before this existed is unaffected.
	// followHandMode/followHandOfs are the hand-frame twins of the two above and
	// arrive the same way, for the same reason -- see AActor::FollowHandMode.
	VSMatrix ObjectToWorldMatrix(FLevelLocals *Level, DVector3 translation, DRotator rotation, DVector2 scaling, unsigned int flags, double tic, float bodyPivotZ = 0.f, int followBodyMode = 0, DVector3 followBodyOfs = DVector3(0, 0, 0), double followBodyYaw = 0.0, int followHandMode = 0, DVector3 followHandOfs = DVector3(0, 0, 0), FName placementPrefix = NAME_None, const VSMatrix *followFrameIn = nullptr, VSMatrix *followFrameOut = nullptr, DVector3 scaleAxes = DVector3(0, 0, 0), DVector3 followHandRot = DVector3(0, 0, 0));
};


enum ModelRendererType
{
	GLModelRendererType,
	SWModelRendererType,
	PolyModelRendererType,
	NumModelRendererTypes
};

enum EFrameError
{
	FErr_NotFound = -1,
	FErr_Voxel = -2,
	FErr_Singleframe = -3
};

// RS FORK -- PER-SURFACE FRAME ADDRESSING.
//
// A model's frame number applies to the WHOLE FILE, which is one number for
// every surface in it. That is what stops a gun's slide from moving while its
// body stays put -- and MD3 has stored the data to do it all along: every
// surface owns its own vertex block per frame (models_md3.cpp, the draw loop
// already computes `surf->vindex + frameno * surf->numVertices` per surface).
// The frame was simply never allowed to vary between them.
//
// This is the MD3 answer to a skeleton, and for a fork whose weapons are all
// MD3 -- and whose Quest target will never have the bone API -- it is the only
// articulation available. It is the same split-mesh technique Quake 3 used for
// head/torso/legs, driven from script instead of from an animation.
//
// ADDRESSED BY INDEX, NOT NAME, and that is a decision the asset library
// forced rather than a preference. Across the donor meshes here, a third of
// the surfaces are named `Cube`, `Untitled`, `pCylinder10`, `basic boot` or
// `python.004` -- exporter defaults with no meaning -- and several models
// repeat a name (`Sights` twice, `Runko` three times). Index always works;
// GetSurfaceName below exists so the meaningful names can still be read off
// when authoring the map that says which index is the slide.
struct FModelSurfaceOverride
{
	int   surface   = -1;     // which surface index this applies to
	int   frame     = -1;     // <0 leaves the caller's frame alone
	int   frameNext = -1;     // <0 means "same as frame", i.e. no blend
	float lerp      = -1.f;   // <0 keeps the caller's interpolation
	bool  hidden    = false;  // skip this surface entirely

	// RS FORK -- A LIVE TRANSFORM ON TOP OF THE FRAME.
	//
	// The fields above select a POSE: a complete, baked snapshot of every
	// vertex. That is all an MD3 frame is, and it is the root of an entire
	// class of bug rather than one bug -- a part driven by frame selection
	// can only ever be where the author baked it, so a live hand position and
	// a baked frame position can never exactly agree, and every handoff
	// between hand-driven and frame-driven motion has a seam in it. Blending
	// between adjacent frames narrows the seam; it cannot remove it, because
	// it is still interpolating two authored snapshots rather than computing
	// a position.
	//
	// Every part this is used for -- a slide, a magazine, a hammer, a
	// cylinder, a trigger -- is RIGID. Its shape never changes as it moves,
	// only its position. So the missing capability was never "arbitrary
	// vertex control"; it is "a rigid surface needs a live transform on top
	// of its current frame", which is what a single-bone rig would give it
	// if MD3 had bones.
	//
	// hasTransform is separate from the values so an identity offset and "no
	// offset" stay distinguishable -- the renderer skips the matrix work
	// entirely for surfaces that never asked, which is nearly all of them.
	bool     hasTransform = false;
	FVector3 offset       = { 0.f, 0.f, 0.f };   // model-space translation
	FVector4 rotation     = { 0.f, 0.f, 0.f, 1.f }; // quaternion, xyzw, identity = no rotation

	// RS FORK -- SHOW THIS SURFACE PARTWAY TOWARD A SECOND TEXTURE.
	//
	// A surface could be hidden, re-skinned, moved or re-framed, but never
	// shown BETWEEN two looks. The only fade available was a second copy of the
	// whole actor at an alpha -- which for a skinned body brought its own arms
	// with no reach chain, hanging at its sides over the real ones.
	//
	// The surface is drawn once as normal, then again on the identical geometry
	// with blendSkin at blendAmount. Same vertices, same bones, same depth: the
	// second pass lands exactly on the first. 0 is inert and costs nothing.
	// Armour going red at low health, a gun wearing toward a worn texture, heat,
	// scorch -- any state a surface reports through its look.
	FTextureID blendSkin;
	float      blendAmount = 0.f;
};

// Passed as one pointer so adding this to the RenderFrame virtual costs every
// model format a single defaulted parameter it can ignore.
struct FModelSurfaceOverrideList
{
	const FModelSurfaceOverride* items = nullptr;
	int count = 0;

	// ONE OVERRIDE PER SURFACE WINS, AND IT IS THE LOWEST SLOT INDEX.
	//
	// This is the composition rule for the whole per-surface system, stated
	// here because this function IS the rule and everything else only obeys it.
	//
	// Find returns the FIRST entry matching a surface, and the caller fills
	// this list by walking the slot table in ascending order -- so if two slots
	// are pointed at the same (model, surface), the lower-numbered slot is
	// drawn and the higher one is silently ignored. It is NOT "last writer
	// wins", and it is NOT nondeterministic; both beliefs have come up, and
	// both would send someone hunting a race that does not exist.
	//
	// THERE IS NO ADDITIVE COMPOSITION. Two writers cannot both contribute to
	// one surface's pose -- there is no base-plus-delta channel anywhere in
	// this path. Anything wanting "recoil on top of what the hand is doing", or
	// a revolver cylinder that both index-steps and swings out, has to compose
	// the values ITSELF and write the single combined result through one slot.
	// If a design ever genuinely needs two independent simultaneous writers,
	// that is a real engine change to this struct and this function -- not
	// something to approximate with a second slot.
	const FModelSurfaceOverride* Find(int surface) const
	{
		if (!items) return nullptr;
		for (int i = 0; i < count; i++)
			if (items[i].surface == surface) return &items[i];
		return nullptr;
	}
};

class FModel
{
public:
	enum LoadState
	{
		NONE = 0,
		LOADING = 1,
		READY = 2
	};

	FModel();
	virtual ~FModel();

	virtual bool Load(const char * fn, int lumpnum, const char * buffer, int length) = 0;

	virtual int FindFrame(const char * name, bool nodefault = false) = 0;

	virtual int NumJoints() { return 0; }
	virtual int FindJoint(FName name) { return -1; }

	virtual int GetJointParent(int joint) { return -1; }
	virtual FName GetJointName(int joint) { return NAME_None; }
	virtual FQuaternion GetJointRotation(int joint) { return FQuaternion(0.0f,0.0f,0.0f,1.0f); }
	virtual FVector3 GetJointPosition(int joint) { return FVector3(0.0f,0.0f,0.0f); }
	virtual TRS GetJointBaseTRS(int joint) { return {}; }
	virtual TRS GetJointPose(int joint, int frame) { return {}; }
	virtual int NumFrames() { return -1; }

	virtual void GetJointChildren(int joint, TArray<int> &out) {}

	virtual void GetRootJoints(TArray<int> &out) {}

	// [RL0] these are used for decoupled iqm animations
	virtual int FindFirstFrame(FName name) { return FErr_NotFound; }
	virtual int FindLastFrame(FName name) { return FErr_NotFound; }
	virtual double FindFramerate(FName name) { return FErr_NotFound; }

	// RS fork -- the trailing surfov is the per-surface frame override list
	// (see FModelSurfaceOverride above). Defaulted, so every existing caller
	// and every format that does not implement it are untouched; only MD3
	// honours it today.
	virtual void RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frame, int frame2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, int boneStartPosition, const FModelSurfaceOverrideList* surfov = nullptr) = 0;

	// RS fork -- how many surfaces this model has, and what they are called.
	// Not used by rendering: this is how a human finds out that surface 2 of
	// the Beretta is the slide, so a part map can be written against indices.
	// Default 0/None for formats with no surface concept.
	virtual int   GetSurfaceCount() { return 0; }
	virtual FName GetSurfaceName(int surface) { return NAME_None; }
	virtual void BuildVertexBuffer(FModelRenderer *renderer) = 0;
	virtual void AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids) = 0;
	virtual float getAspectFactor(float vscale) { return 1.f; }
	virtual const TArray<TRS>* AttachAnimationData() { return nullptr; };

	virtual ModelAnimFrame PrecalculateFrame(const ModelAnimFrame &from, const ModelAnimFrameInterp &to, float inter, const TArray<TRS>* animationData) { return nullptr; };

	virtual const TArray<VSMatrix>* CalculateBones(const ModelAnimFrame &from, const ModelAnimFrameInterp &to, float inter, const TArray<TRS>* animationData, TArray<BoneOverride> *in, BoneInfo *out, double time) { return nullptr; };
	virtual const TArray<VSMatrix>* CalculateBonesOnlyOffsets(TArray<BoneOverride> *in, BoneInfo *out, double time) { return nullptr; };

	virtual const TArray<VSMatrix>* GetBasePose() {return nullptr;}

	// Largest |X|/|Y|/|Z| across the model's own raw local-space vertices,
	// tracked independently per axis (not necessarily from the same vertex --
	// a conservative bounding proxy, not a tight AABB). Unscaled by the
	// MODELDEF's own Scale block; the caller (GetModelBoundsHint) applies
	// that the same way GetModelWorldOffset applies actor scale to an
	// offset -- this just answers the one thing script has no other way to
	// see: how big the raw mesh actually is. False/zero for any format that
	// does not override this; a holster falling back to a flat guess for an
	// unmeasured model is no worse off than it is today.
	virtual bool GetLocalExtent(float *outMaxAbsX, float *outMaxAbsY, float *outMaxAbsZ) { return false; }

	void SetVertexBuffer(int type, IModelVertexBuffer *buffer) { mVBuf[type] = buffer; }
	IModelVertexBuffer *GetVertexBuffer(int type) const { return mVBuf[type]; }
	void DestroyVertexBuffer();
	LoadState GetLoadState() const { return loadState; }
	void SetLoadState(LoadState state) { loadState = state; }
	virtual void LoadGeometry(FileSys::FileData* lumpData);
	int GetLumpNum() const { return mLumpNum; }

	bool hasSurfaces = false;

	FString mFileName;
	std::pair<FString, FString> mFilePath;

	FSpriteModelFrame *baseFrame;
private:
	IModelVertexBuffer *mVBuf[NumModelRendererTypes];
	LoadState loadState = NONE;
protected:
	int mLumpNum = -1;
};

int ModelFrameHash(FSpriteModelFrame* smf);
unsigned FindModel(const char* path, const char* modelfile, bool silent = false);
