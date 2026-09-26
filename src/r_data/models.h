/*
** models.h
**
** General model handling code
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#ifndef __GL_MODELS_H_
#define __GL_MODELS_H_

#include "tarray.h"
#include "matrix.h"
#include "m_bbox.h"
#include "r_defs.h"
#include "g_levellocals.h"
#include "voxels.h"
#include "i_modelvertexbuffer.h"
#include "model.h"

class FModelRenderer;

struct FSpriteModelFrame;
class IModelVertexBuffer;
struct FLevelLocals;

//
// [BB] Model rendering flags.
//
enum
{
	// [BB] Color translations for the model skin are ignored. This is
	// useful if the skin texture is not using the game palette.
	MDL_IGNORETRANSLATION			= 1<<0,
	MDL_PITCHFROMMOMENTUM			= 1<<1,
	MDL_ROTATING					= 1<<2,
	MDL_INTERPOLATEDOUBLEDFRAMES	= 1<<3,
	MDL_NOINTERPOLATION				= 1<<4,
	MDL_USEACTORPITCH				= 1<<5,
	MDL_USEACTORROLL				= 1<<6,
	MDL_BADROTATION					= 1<<7,
	MDL_DONTCULLBACKFACES			= 1<<8,
	MDL_USEROTATIONCENTER			= 1<<9,
	MDL_NOPERPIXELLIGHTING			= 1<<10,	// forces a model to not use per-pixel lighting. useful for voxel-converted-to-model objects.
	MDL_SCALEWEAPONFOV				= 1<<11,	// scale weapon view model with higher user FOVs
	MDL_MODELSAREATTACHMENTS		= 1<<12,	// any model index after 0 is treated as an attachment, and therefore will use the bone results of index 0
	MDL_CORRECTPIXELSTRETCH			= 1<<13,	// ensure model does not distort with pixel stretch when pitch/roll is applied
	MDL_FORCECULLBACKFACES			= 1<<14,
	MDL_FIXROTATING					= 1<<15,
	MDL_NOAUTOREVERSE				= 1<<16,	// model ships explicit left/right variants, so never apply the VR non-dominant-hand mirror
	MDL_USEHANDOFFSETS				= 1<<17,	// apply the live vr_hand_* placement CVARs on top of this model's own MODELDEF offsets
	MDL_IGNORESKINALPHA				= 1<<18,	// skin alpha carries data, not opacity -- do not alpha-test against it

	// A world actor's model normally gets its transform from where the actor was
	// last PUT -- position and Angles, written by script in the playsim tic. That
	// is 35Hz, it is interpolated between tics, and it stops entirely while a menu
	// is open. There is no controller anywhere in that path: ObjectToWorldMatrix
	// contains zero references to AttackPos, weaponangles or GetWeaponTransform.
	// So a world model does not track a hand badly -- it has never been wired to
	// one at all, which is why moving a gun off a psprite and into the world lost
	// tracking outright rather than degrading it.
	//
	// With one of these set, the model's transform comes from the SAME call the
	// HUD psprite path has always used -- VRMode::GetWeaponTransform, read fresh
	// at DRAW time. Not a reimplementation of it: the same function, the same
	// matrix, the same clock. The model's own MODELDEF scale, offset and angle
	// offsets still apply on top, exactly as they do for any other world model.
	MDL_FOLLOWMAINHAND				= 1<<19,	// world model rides the main hand's controller transform, at draw rate
	MDL_FOLLOWOFFHAND				= 1<<20,	// world model rides the off hand's controller transform, at draw rate
	MDL_VOXELBODYAXIS				= 1<<21,	// held voxel: wrap pitch/roll in vr_voxel_bodyyaw. Set at runtime, never from MODELDEF

	// PSPRITE worn on the BODY rather than on a controller.
	//
	// The two flags above and MDL_USEHANDOFFSETS anchor to a hand, and that is
	// every anchor a psprite had. A torso, a holster or a boot belongs on none
	// of them.
	//
	// IT HAS TO BE A PSPRITE AT ALL because a world model is drawn in a
	// DIFFERENT PASS from the weapons and the hands, so the two cannot occlude
	// each other: a hand and a gun pass straight through a world-model torso and
	// nothing can be done about it from content. One pass is the only way
	// anything worn looks solid against anything held.
	//
	// The seat and the heading come from the PSPRITE (DPSprite::BodyOfs and
	// BodyYaw), not from MODELDEF, because a dozen worn things share one MODELDEF
	// entry and each needs its own place on the body. Heading is supplied by the
	// caller for the reason AActor::FollowBodyYaw exists: the renderer's own
	// heading is not visible to script and is not the same number as HmdYaw.
	MDL_FOLLOWBODY					= 1<<22,	// psprite rides the body frame at draw rate; seat from DPSprite::BodyOfs
	MDL_NOPIXELSTRETCH				= 1<<23,	// MODELDEF NoPixelStretch: no pixel-stretch scale, so the model pitches and rolls undistorted (GZSelaco 1e7fd30b59, which used 1<<16)
};

FSpriteModelFrame * FindModelFrame(AActor * thing, int sprite, int frame, bool dropped);
FSpriteModelFrame * FindModelFrame(const PClass * ti, bool is_decoupled, int sprite, int frame, bool dropped);
FSpriteModelFrame * FindModelFrame(const PClass * ti, int sprite, int frame, bool dropped);
// RS FORK -- the MODELDEF block for (class, sprite, frame) and nothing else: FindModelFrameRaw's hash walk without its
// voxel fallback, so no render cvar and no voxel changes the answer. For game-state questions that every machine in a
// game must answer alike -- p_actionfunctions.cpp's model queries (models.cpp says more).
FSpriteModelFrame * FindModelDefFrame(const PClass * ti, int sprite, int frame);
FSpriteModelFrame * FindVoxelFrame(int sprite, int frame, bool dropped);

// Binding a model to (class, sprite, frame) without a MODELDEF lump, for models
// that are built while a level loads. See the comment on the definitions in
// models.cpp -- these are level-load-only calls, because they reallocate and
// rehash the table the renderer takes pointers into.
void AddSpriteModelFrame(const FSpriteModelFrame &smf);
void ClearAddedSpriteModelFrames();

// RS FORK -- r_voxels_mode / r_voxeldistance helpers, defined beside
// FindVoxelFrame in models.cpp (see the notes there).
//   VoxelsEffectiveMode     r_voxels_mode with auto (-1) resolved against
//                           r_voxelpack_loaded: 0 all, 1 VoxelOverride only, 2 none
//   SpriteFrameHasTexture   the frame has a real sprite to draw if its voxel is refused
//   KeepVoxelWithoutSprite  a refused voxel must be kept because there is no sprite
//                           (logs once per sprite name)
int VoxelsEffectiveMode();
bool SpriteFrameHasTexture(int sprite, int frame);
bool KeepVoxelWithoutSprite(int sprite, int frame);
//FSpriteModelFrame * FindModelFrameRaw(const AActor * actorDefaults, const PClass * ti, int sprite, int frame, bool dropped);

bool IsHUDModelForPlayerAvailable(player_t * player);

// Check if circle potentially intersects with node AABB
inline bool CheckBBoxCircle(float *bbox, float x, float y, float radiusSquared)
{
	float centerX = (bbox[BOXRIGHT] + bbox[BOXLEFT]) * 0.5f;
	float centerY = (bbox[BOXBOTTOM] + bbox[BOXTOP]) * 0.5f;
	float extentX = (bbox[BOXRIGHT] - bbox[BOXLEFT]) * 0.5f;
	float extentY = (bbox[BOXBOTTOM] - bbox[BOXTOP]) * 0.5f;
	float aabbRadiusSquared = extentX * extentX + extentY * extentY;
	x -= centerX;
	y -= centerY;
	float dist = x * x + y * y;
	return dist <= radiusSquared + aabbRadiusSquared;
}

// Helper function for BSPWalkCircle
template<typename Callback>
void BSPNodeWalkCircle(void *node, float x, float y, float radiusSquared, const Callback &callback)
{
	while (!((size_t)node & 1))
	{
		node_t *bsp = (node_t *)node;

		if (CheckBBoxCircle(bsp->bbox[0], x, y, radiusSquared))
			BSPNodeWalkCircle(bsp->children[0], x, y, radiusSquared, callback);

		if (!CheckBBoxCircle(bsp->bbox[1], x, y, radiusSquared))
			return;

		node = bsp->children[1];
	}

	subsector_t *sub = (subsector_t *)((uint8_t *)node - 1);
	callback(sub);
}

// Search BSP for subsectors within the given radius and call callback(subsector) for each found
template<typename Callback>
void BSPWalkCircle(FLevelLocals *Level, float x, float y, float radiusSquared, const Callback &callback)
{
	if (Level->nodes.Size() == 0)
		callback(&Level->subsectors[0]);
	else
		BSPNodeWalkCircle(Level->HeadNode(), x, y, radiusSquared, callback);
}

void RenderModel(FModelRenderer* renderer, float x, float y, float z, FSpriteModelFrame* smf, AActor* actor, double ticFrac);
void RenderHUDModel(FModelRenderer* renderer, DPSprite* psp, FVector3 translation, FVector3 rotation, FVector3 rotation_pivot, FSpriteModelFrame *smf, double ticFrac);

struct CalcModelFrameInfo
{
	// RS fork -- carried so CalcModelOverrides can honour an actor's explicit
	// ModelFrame the same way it already honours a psprite's. It receives the
	// psprite but never received the actor.
	const AActor * actor;
	int smf_flags;
	const FSpriteModelFrame * smfNext;
	float inter;
	bool is_decoupled;
	ModelAnimFrameInterp decoupled_frame;
	ModelAnimFrame * decoupled_frame_prev;
	unsigned modelsamount;
};

struct ModelDrawInfo
{
	TArray<FTextureID> surfaceskinids;
	int modelid;
	int animationid;
	int modelframe;
	int modelframenext;
	FTextureID skinid;

	// RS FORK -- set when the frame above was chosen deliberately from ZScript
	// (the psprite's ModelFrame, or the native state remap) rather than falling
	// out of the sprite table. Only a deliberate frame is allowed to override a
	// decoupled model's rest pose -- see ProcessModelFrame.
	bool modelframe_explicit = false;
};

class DActorModelData;

// Union of the fork's and upstream's signatures. Upstream added ticFrac to
// CalcModelFrame and BoneInfo* to ProcessModelFrame; the fork threads a
// const DPSprite* psp through CalcModelFrame/CalcModelOverrides so psprite
// model-frame addressing (ModelFrame / ModelFrameNext / ModelFrameLerp) and
// the native state remap can reach the model path. Both are kept.
// Default arguments live ONLY here - the definitions in models.cpp carry none.
// RS FORK -- HUD bone anchoring. Ticks the frame stamp that decides whether a
// stored anchor is still current; call once per psprite render pass.
void HudAnchor_BeginFrame();
bool HudAnchor_Get(int layer, FName bone, VSMatrix &out);

CalcModelFrameInfo CalcModelFrame(FLevelLocals *Level, const FSpriteModelFrame *smf, const FState *curState, const int curTics, DActorModelData* modelData, AActor* actor, bool is_decoupled, double tic, double ticFrac, const DPSprite* psp = nullptr);

// returns true if the model isn't removed
bool CalcModelOverrides(int modelindex, const FSpriteModelFrame *smf, DActorModelData* modelData, const CalcModelFrameInfo &frameinfo, ModelDrawInfo &drawinfo, bool is_decoupled, const DPSprite* psp = nullptr);

const TArray<VSMatrix> * ProcessModelFrame(FModel * animation, bool nextFrame, int i, const FSpriteModelFrame *smf, DActorModelData* modelData, const CalcModelFrameInfo &frameinfo, ModelDrawInfo &drawinfo, bool is_decoupled, double tic, BoneInfo *out);

EXTERN_CVAR(Float, cl_scaleweaponfov)

#endif
