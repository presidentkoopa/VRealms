/*
** models.cpp
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

#include "filesystem.h"
#include "cmdlib.h"
#include "sc_man.h"
#include <mutex>          // RS fork -- KeepVoxelWithoutSprite's log-once (r_voxels_mode)
#include <vector>
#include "m_crc32.h"
#include "c_console.h"
#include "g_game.h"
#include "doomstat.h"
#include "g_level.h"
#include "r_state.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "r_utility.h"
#include "models.h"
#include "model_kvx.h"
#include "i_time.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "actor.h"
#include "actorinlines.h"
#include "v_video.h"
#include "hw_bonebuffer.h"
#include "hw_vrmodes.h"
#include "model_reach.h"  // RS fork -- draw-time joint poses and reach chains (RenderModel, RenderModelFrame)
#include "model_handdrive.h" // RS fork -- the hand drive (HandDrive_OwnerStep, HandDrive_PoseForFollower)
#include "model_jointfollow.h" // RS fork -- piece E: a child riding a joint as drawn (JointFollowCarry)
#include "c_dispatch.h"   // RS fork -- the modelsurfaces CCMD at the end of this file
#include "v_text.h"       // RS fork -- TEXTCOLOR_* for the same


#ifdef _MSC_VER
#pragma warning(disable:4244) // warning C4244: conversion from 'double' to 'float', possible loss of data
#endif

CVAR(Bool, gl_interpolate_model_frames, true, CVAR_ARCHIVE)

// RS FORK -- HUD BONE ANCHORING.
//
// Where a psprite's model resolves each of its requested bones, so a later
// layer can be drawn there. Keyed by (layer id, bone name) and stamped with a
// frame counter: a target that stops being drawn must not leave an attachment
// frozen at its last known position, so a stale stamp reads as "no anchor" and
// the layer falls back to its own placement.
// mat is what an anchored layer is drawn with. offset is the same bone
// expressed as a displacement from the weapon's ORIGIN, in the model's own
// axes, already multiplied by the scale the model is being drawn at -- which is
// the form script can use.
//
// Script cannot see any of this otherwise, and the cost of that has been high:
// every grab point in the mods is a hand-guessed distance ("the pump is about
// sixteen units forward"), which is wrong the moment a weapon is rescaled or
// repositioned, and wrong in a way that presents as the grab silently never
// firing. The engine has always known exactly where the pump is.
struct HudAnchorEntry { VSMatrix mat; DVector3 offset; uint64_t frame; };
static TMap<uint64_t, HudAnchorEntry> g_hudAnchors;
static uint64_t g_hudAnchorFrame = 0;

// The transform of the model currently being drawn, so a bone matrix -- which
// is model-local -- can be combined into something the anchored layer can use
// directly.
static VSMatrix g_hudAnchorSource;

static inline uint64_t HudAnchorKey(int layer, FName bone)
{
	return (uint64_t(uint32_t(layer)) << 32) | uint32_t(bone.GetIndex());
}

void HudAnchor_BeginFrame()
{
	g_hudAnchorFrame++;

	// The shared table above guards staleness with the frame stamp, but the
	// copies written onto the psprites themselves have no such guard, and a
	// latched AnchorBoneLive is worse than none: a grab test would keep firing at
	// a weapon that is no longer drawn, at wherever it was last seen. Cleared
	// here, so only a bone actually published this frame reads as live.
	player_t *player = &players[consoleplayer];
	for (DPSprite *q = player->psprites; q != nullptr; q = q->GetNext())
	{
		q->AnchorBoneLive = false;
	}
}

bool HudAnchor_Get(int layer, FName bone, VSMatrix &out)
{
	auto *e = g_hudAnchors.CheckKey(HudAnchorKey(layer, bone));
	if (!e || e->frame != g_hudAnchorFrame) return false;
	out = e->mat;
	return true;
}

// Where a bone sits relative to the weapon's origin, in the model's own axes
// and in map units. Zero if that bone was not drawn this frame -- a stale
// answer is worse than no answer, because a grab test cannot tell the two
// apart and would keep firing at a weapon that is no longer on screen.
//
// The requests drive what gets published: a bone nobody has asked for is never
// stored, so a mod must anchor something to a bone (or ask for it) before this
// returns anything for it.
bool HudAnchor_GetOffset(int layer, FName bone, DVector3 &out)
{
	auto *e = g_hudAnchors.CheckKey(HudAnchorKey(layer, bone));
	if (!e || e->frame != g_hudAnchorFrame) { out = DVector3(0, 0, 0); return false; }
	out = e->offset;
	return true;
}

// Publish whichever bones another layer has asked this one for. Driven by the
// requests rather than storing every bone, because a rigged weapon has dozens
// and almost none of them are ever anchored to.
static void HudAnchor_Store(const DPSprite *psp, FModel *mdl, const TArray<VSMatrix> &bones)
{
	if (!psp || !psp->Owner || !mdl) return;

	for (DPSprite *q = psp->Owner->psprites; q != nullptr; q = q->GetNext())
	{
		if (q->AnchorLayer != psp->GetID() || q->AnchorBone == NAME_None) continue;

		int j = mdl->FindJoint(q->AnchorBone);
		if (j < 0 || (unsigned)j >= bones.Size()) continue;

		HudAnchorEntry e;
		e.mat = g_hudAnchorSource;
		e.mat.multMatrix(bones[j]);
		e.frame = g_hudAnchorFrame;

		// The bone's own translation is its position in MODEL space, and the
		// model origin is where the weapon sits, so that translation is already
		// the offset script wants -- it only needs the scale the model is drawn
		// at, which is the length of a basis column of the model's transform.
		//
		// Taken from the matrix rather than recomputed from the cvars and the
		// MODELDEF: several things multiply into that scale and reading it back
		// off the result cannot drift out of step with them.
		{
			// THE BIND POSE MATTERS. bones[j] is a SKINNING matrix: it maps
			// bind-pose space to posed space, so reading its translation column
			// gives where the MODEL ORIGIN lands, not where the bone is. At
			// rest every skinning matrix is identity, so every bone reported
			// the same point -- the model origin -- and a magazine anchored to
			// the magwell spawned wherever the model origin happened to sit,
			// which on a pistol mesh is typically at the trigger.
			//
			// The bone's real position is that matrix applied to the joint's
			// BIND position, which is what GetJointPosition returns (an
			// absolute, parent-accumulated position, see models_iqm.cpp).
			const FVector3 bindPos = mdl->GetJointPosition(j);
			const FLOATTYPE bp[4] = { (FLOATTYPE)bindPos.X, (FLOATTYPE)bindPos.Y, (FLOATTYPE)bindPos.Z, (FLOATTYPE)1.0 };

			// Copy: bones is a const reference and multMatrixPoint is non-const.
			VSMatrix boneMat = bones[j];
			FLOATTYPE posed[4];
			boneMat.multMatrixPoint(bp, posed);

			const FLOATTYPE *sm = g_hudAnchorSource.get();
			const double sc = sqrt(sm[0]*sm[0] + sm[1]*sm[1] + sm[2]*sm[2]);
			e.offset = DVector3(posed[0] * sc, posed[1] * sc, posed[2] * sc);

			// Straight onto the psprite that asked. Script reads it from there,
			// so nothing on the game side ever touches this table.
			q->AnchorBonePos = e.offset;
			q->AnchorBoneLive = true;

			// The same bone as a world point, in the frame AttackPos and
			// OffhandPos are taken from. e.mat carries the weapon's full
			// object-to-world transform combined with the bone, and it is
			// applied to the joint's BIND position for the reason above -- not
			// read off the translation column, which would give the model
			// origin. The Y/Z swap is the usual convention: the matrix is
			// Y-up, the playsim is Z-up.
			FLOATTYPE world[4];
			e.mat.multMatrixPoint(bp, world);
			q->AnchorBoneWorld = DVector3(world[0], world[2], world[1]);

			// AND THE SAME CORRECTION ON THE MATRIX ITSELF.
			//
			// e.mat is what actually PLACES an anchored layer -- HudAnchor_Get
			// hands it straight to the renderer, which orthonormalises the
			// basis (stripping scale) and keeps the translation as-is. Its
			// translation column has the identical skinning-matrix problem
			// described above: at rest it is the MODEL ORIGIN, the same point
			// for every bone. So a hand anchored to a pistol's grip bone and a
			// hand anchored to its slide both landed at the model's origin --
			// which is nowhere near either of them, and reads in the headset
			// as hands floating off the gun entirely.
			//
			// Fixing e.offset/AnchorBoneWorld earlier only fixed what SCRIPT
			// reads. This fixes what the RENDERER draws, which is a separate
			// consumer of the same wrong number -- and it fixes every anchored
			// hand on every weapon, not just the one it was found on, since
			// nothing about it is specific to any single mesh.
			//
			// Rotation is deliberately untouched: only the translation was
			// ever wrong, and orientation does not depend on which point of
			// the bone is used.
			{
				FLOATTYPE fixed[16];
				memcpy(fixed, e.mat.get(), sizeof(fixed));
				fixed[12] = world[0];
				fixed[13] = world[1];
				fixed[14] = world[2];
				e.mat.loadMatrix(fixed);
			}

			// The same matrix's ROTATION, decomposed to the playsim's
			// yaw/pitch/roll. Read here rather than in script because the
			// matrix is right here and ZScript's Quat cannot rotate a vector,
			// so script has no honest way to derive this itself.
			//
			// Basis columns, with the same Y-up -> Z-up swap the translation
			// above uses: the matrix's X axis is forward, its Z axis is the
			// playsim's Y, and its Y axis is the playsim's Z. Scale is divided
			// out first -- the model is drawn scaled and a scaled basis would
			// give wrong angles.
			{
				// The rotation still comes from the matrix's basis columns --
				// only the TRANSLATION needed the bind-pose correction above,
				// since orientation does not depend on which point is used.
				const FLOATTYPE *wm = e.mat.get();

				DVector3 fwd  (wm[0], wm[2], wm[1]);
				DVector3 side (wm[8], wm[10], wm[9]);
				DVector3 up   (wm[4], wm[6], wm[5]);

				const double flen = fwd.Length();
				const double slen = side.Length();
				const double ulen = up.Length();
				if (flen > 0) fwd /= flen;
				if (slen > 0) side /= slen;
				if (ulen > 0) up /= ulen;

				const double yaw   = atan2(fwd.Y, fwd.X);
				const double pitch = asin(clamp(-fwd.Z, -1.0, 1.0));
				const double roll  = atan2(side.Z, up.Z);

				q->AnchorBoneAngles = DVector3(
					yaw   * (180.0 / M_PI),
					pitch * (180.0 / M_PI),
					roll  * (180.0 / M_PI));
			}
		}
		g_hudAnchors.Insert(HudAnchorKey(psp->GetID(), q->AnchorBone), e);
	}
}

// RS FORK -- pose pipeline diagnostics. Traces an explicitly addressed model
// frame from the psprite through to the bone calculation, which is otherwise
// invisible: a pose that never applies looks exactly like a pose that was
// never set. Prints only on change, so a held pose reports once.
CVAR(Bool, vr_pose_debug, false, 0)

// RS FORK -- watch per-surface overrides arrive at the renderer.
//
// Everything script-side about surface driving can be printed from ZScript,
// but whether the override actually REACHES the draw cannot -- and "the part
// did not move" looks the same whether the write never landed, the model index
// was wrong, or the frame was out of range. This prints what the renderer is
// actually about to do with each driven surface. On change only, or it is
// several hundred lines a second.
CVAR(Bool, vr_surf_debug, false, 0)
// RS FORK -- MODEL DIAGNOSTICS.
//
// Every check here cost a headset session to find by eye, and every one of them
// presents as something misleading: a model with no frames renders as missing
// textures, a packed alpha channel renders as a half-transparent gun. None of
// them look like what they are, which is exactly why they are worth reporting.
//
// Checked as a model is first drawn, and reported once each.
CVAR(Bool, vr_validate, false, 0)
CVAR(Bool, vr_spatialreport, false, 0)

static TMap<uint64_t, bool> g_validateSeen;

static bool ValidateOnce(const void *key, int slot)
{
	uint64_t k = (uint64_t)(intptr_t)key * 16 + slot;
	if (g_validateSeen.CheckKey(k)) return false;
	g_validateSeen.Insert(k, true);
	return true;
}

static void ValidateHudModel(const FSpriteModelFrame *smf, FModel *mdl, const DPSprite *psp, unsigned smf_flags)
{
	if (!vr_validate || !mdl || !smf) return;

	const char *who = (psp && psp->Caller != nullptr)
		? psp->Caller->GetClass()->TypeName.GetChars()
		: "unknown";

	// A bone-weighted mesh with no pose has nothing to evaluate its weights
	// against, so most of it collapses. It reads as missing geometry or missing
	// textures, never as an animation problem.
	if (mdl->NumJoints() > 0 && mdl->NumFrames() == 0 && ValidateOnce(smf, 0))
	{
		Printf(TEXTCOLOR_ORANGE "[MODEL] %s: %d bones but ZERO frames. A skinned model with no pose collapses; "
			"it looks like missing geometry or missing textures. Export a bind pose.\n",
			who, mdl->NumJoints());
	}

	// Packed PBR maps carry roughness or gloss in alpha, not opacity. The model
	// is alpha-tested against that channel and most of it is discarded.
	if (!(smf_flags & MDL_IGNORESKINALPHA) && ValidateOnce(smf, 1))
	{
		FGameTexture *tex = nullptr;
		if (smf->skinIDs.Size() > 0 && smf->skinIDs[0].isValid())
			tex = TexMan.GetGameTexture(smf->skinIDs[0], true);
		else if (smf->surfaceskinIDs.Size() > 0 && smf->surfaceskinIDs[0].isValid())
			tex = TexMan.GetGameTexture(smf->surfaceskinIDs[0], true);

		if (tex && tex->GetTranslucency())
		{
			Printf(TEXTCOLOR_ORANGE "[MODEL] %s: skin has an alpha channel and IgnoreSkinAlpha is not set. "
				"If that alpha is packed data rather than opacity, most of the model is alpha-tested away "
				"and reads as half transparent.\n", who);
		}
	}
}

EXTERN_CVAR(Bool, r_drawvoxels)

// [BB] BODY-AXIS CORRECTION FOR HELD VOXELS.
//
// A voxel pack's AngleOffset corrects which way the mesh FACES. That is not
// the same thing as putting its long axis on +X, which is what the pitch and
// roll rotations assume. When the two disagree by a quarter turn, a wrist roll
// comes out as a fore/aft tilt -- the mesh is being rolled about an axis that
// runs across it rather than along it.
//
// Which way a given pack is off is not knowable from the data; it depends on
// how its author authored the voxels. So this is a dial, not a constant. It
// wraps the pitch/roll pair only, leaving yaw and the mesh's resting facing
// alone, and applies ONLY to actors with VoxelOverride set -- scenery voxels
// standing on a floor never see it.
// Negative = derive it from the pack's own angleoffset, which is right for
// every pack examined so far. 0 and up override with a literal quarter turn.
//
// RENAMED from vr_voxel_bodyyaw, 2026-08-29, and the rename IS the fix. That
// cvar shipped once with a default of 0, which archived a 0 into the config of
// anyone who ran that build. Changing the default afterwards did nothing for
// them -- an archived value always beats a new default -- so the correction sat
// switched off and read as a broken feature across three separate test runs,
// with the log faithfully reporting bodyyaw=0.0 every time. A new name has
// nothing archived against it, so the default finally applies.
//
// Generalises: changing the default of a CVAR_ARCHIVE cvar only ever affects
// someone who has never run a build that wrote one.
CVAR(Float, vr_voxel_rollaxis, -1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Held-voxel orientation trace. On by default while this is being worked out;
// it only ever prints for an actor that is actually in a hand, and only once a
// second, so it is quiet unless something is held.
CVAR(Bool, vr_voxel_debug, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Int, vr_control_scheme)
EXTERN_CVAR(Float, vr_weaponScale)
EXTERN_CVAR(Float, vr_3dweaponOffsetX);
EXTERN_CVAR(Float, vr_3dweaponOffsetY);
EXTERN_CVAR(Float, vr_3dweaponOffsetZ);
// Defined here rather than externed: the only consumer is the placement trace
// below, and an earlier diagnostic that owned it elsewhere was removed.
CVAR(Bool, vr_place_debug, false, 0)
EXTERN_CVAR(Float, vr_hand_ofs_x);
EXTERN_CVAR(Float, vr_hand_ofs_y);
EXTERN_CVAR(Float, vr_hand_ofs_z);
EXTERN_CVAR(Float, vr_hand_yaw);
EXTERN_CVAR(Float, vr_hand_pitch);
EXTERN_CVAR(Float, vr_hand_roll);
EXTERN_CVAR(Float, vr_offhand_ofs_x);
EXTERN_CVAR(Float, vr_offhand_ofs_y);
EXTERN_CVAR(Float, vr_offhand_ofs_z);
EXTERN_CVAR(Float, vr_offhand_yaw);
EXTERN_CVAR(Float, vr_offhand_pitch);
EXTERN_CVAR(Float, vr_offhand_roll);

extern TDeletingArray<FVoxel *> Voxels;
extern TDeletingArray<FVoxelDef *> VoxelDefs;

// [RS FORK] Upstream added the 'ticFrac' parameter (the animation clock now comes
// from AActor::GetModelTimer() + ticFrac instead of Level->totaltime + I_GetTimeFrac()).
// The fork's trailing 'psp' parameter is kept, defaulted so upstream's psp-less
// callers (RenderModel here, AActor::CalcBones in p_mobj.cpp) still compile.
void RenderFrameModels(FModelRenderer* renderer, FLevelLocals* Level, const FSpriteModelFrame *smf, const FState* curState, int curTics, double ticFrac, FTranslationID translation, AActor* actor, const DPSprite* psp = nullptr);

// RS FORK -- NEAR-EYE FADE (VR_BODY_IK_RETURN_PLAN.md 4b idea 5; FModelRenderer::SetEyeFade).
//
// A world model whose placement prefix carries <prefix>_eyefade_far (and, if it wants,
// _eyefade_near, default 0) dissolves in a dither where it is closer to the eye than far,
// and is gone by near: an arm reaching past the face, a gun brought up to it, a torso
// looked down through. The prefix is the one the model is placed by (AActor::
// PlacementPrefix, else MODELDEF PlacementCVars), so the fade is tuned live on the same
// page as the model's seat. Read here, beside the placement code and never inside it.
// Absent, or far <= near: false, and the draw is exactly what it was.
static bool GetPlacementCVar(const char *name, float &out);
static bool ModelEyeFadeRange(const FSpriteModelFrame *smf, const AActor *actor, float &nearDist, float &farDist)
{
	FName prefix = smf->placementCVars;
	if (actor->PlacementPrefix != NAME_None && stricmp(actor->PlacementPrefix.GetChars(), "None") != 0)
		prefix = actor->PlacementPrefix;
	if (prefix == NAME_None) return false;
	char nm[192];
	float f = 0.f, n = 0.f;
	snprintf(nm, sizeof(nm), "%s_eyefade_far", prefix.GetChars());
	if (!GetPlacementCVar(nm, f) || !(f > 0.f)) return false;
	snprintf(nm, sizeof(nm), "%s_eyefade_near", prefix.GetChars());
	if (!GetPlacementCVar(nm, n) || !(n > 0.f)) n = 0.f;
	if (!(f > n)) return false;
	nearDist = n;
	farDist = f;
	return true;
}

void RenderModel(FModelRenderer *renderer, float x, float y, float z, FSpriteModelFrame *smf, AActor *actor, double ticFrac)
{
	// RS FORK -- THE RENDER WINDOW (model_reach.h, FModelRenderWindow), around this whole draw and its ObjectToWorldMatrix
	// below: the one place a FollowActorJoint child reads the joint it rides as its parent drew it (Engine docs/
	// MODEL_JOINT_DRIVE_PLAN.md section 11 condition 8). Script asking the same matrix is outside it. One int.
	FModelRenderWindow renderWindow;

	int smf_flags = smf->getFlags(actor->modelData);
	FTranslationID translation = NO_TRANSLATION;
	if (!(smf_flags & MDL_IGNORETRANSLATION))
		translation = actor->Translation;

	VSMatrix objectToWorldMatrix = smf->ObjectToWorldMatrix(actor, x, y, z, ticFrac);

	// RS FORK -- DRAW-TIME JOINT POSES AND REACH CHAINS (model_reach.h). Opens the
	// render-only window in which RenderModelFrame may pose THIS actor's bones, and hands
	// it the exact matrix this draw uses. Inert -- one int and one bool test -- unless a
	// pose or a chain is registered or r_jointpose_test is set. Closes on return.
	FModelDrawPoseScope drawPoseScope(actor, objectToWorldMatrix, ticFrac);

	const DVector2 scale = actor->InterpolatedScale(ticFrac);
	float scaleFactorX = scale.X * smf->xscale;
	float scaleFactorY = scale.X * smf->yscale;
	float scaleFactorZ = scale.Y * smf->zscale;
	float orientation = scaleFactorX * scaleFactorY * scaleFactorZ;

	// RS FORK -- the near-eye fade (ModelEyeFadeRange above). Not set: no call at all.
	float eyeFadeNear = 0.f, eyeFadeFar = 0.f;
	const bool eyeFade = ModelEyeFadeRange(smf, actor, eyeFadeNear, eyeFadeFar);

	renderer->BeginDrawModel(actor->RenderStyle, smf_flags, objectToWorldMatrix, orientation < 0);
	if (eyeFade) renderer->SetEyeFade(eyeFadeNear, eyeFadeFar);
	RenderFrameModels(renderer, actor->Level, smf, actor->state, actor->tics, ticFrac, translation, actor);
	if (eyeFade) renderer->SetEyeFade(0.f, 0.f);
	renderer->EndDrawModel(actor->RenderStyle, smf_flags);
}

// VR_WORLDACTOROFFSET -- where a world actor's MODEL actually is, in the world.
//
// THE GAP THIS FILLS.
//
// TransformByNamedBone answers "where is this bone" in MODEL space. It applies
// the bone matrix and stops. It never sees the object-to-world matrix -- the
// actor's position, the MODELDEF scale and offsets and angle corrections, or,
// for a followed model, the entire controller transform loaded by
// GetWeaponTransform. So script could ask where MARKER_grip was and get an
// answer in a space with no relation to the room.
//
// That is why every attempt at seating a world model has come down to a human
// finding an offset by eye on a slider. There was no way to ask.
//
// With this, seating is arithmetic and not taste:
//
//     grip = gun.ModelPointToWorld(gun.TransformByNamedBone('MARKER_grip', ...))
//     palm = hand.ModelPointToWorld(hand.TransformByNamedBone('HANDPALM_joint', ...))
//     gun.SetOrigin(gun.Pos + (palm - grip), false)
//
// and the firing line is the returned forward axis -- the direction the barrel
// is actually drawn pointing, not a reconstruction from Euler angles.
//
// Returns position, forward, up. Forward and up are unit vectors in world space,
// taken from the matrix's own basis, so they carry every correction the model
// received including ones nothing in script knows about.
static void ModelWorldTransform(AActor *self, double mx, double my, double mz,
	DVector3 &posOut, DVector3 &fwdOut, DVector3 &upOut)
{
	posOut = DVector3(0, 0, 0);
	fwdOut = DVector3(1, 0, 0);
	upOut  = DVector3(0, 0, 1);
	if (self == nullptr) return;

	// The frame the renderer would pick for this actor right now. Decoupled
	// actors resolve through BaseSpriteModelFrames, which is why an actor
	// without BaseFrame answers nothing here -- the same reason it draws nothing.
	FSpriteModelFrame *smf = FindModelFrame(self, self->sprite, self->frame, false);
	if (smf == nullptr) return;

	const double ticFrac = I_GetTimeFrac();
	VSMatrix m = smf->ObjectToWorldMatrix(self,
		(float)self->X(), (float)self->Y(), (float)self->Z(), ticFrac);

	// Column-major, the way VSMatrix stores it: [0..2] is axis X, [4..6] axis Y,
	// [8..10] axis Z, [12..14] the translation.
	const FLOATTYPE *v = m.get();
	auto xf = [&](double a, double b, double c) {
		return DVector3(
			v[0]*a + v[4]*b + v[8]*c  + v[12],
			v[1]*a + v[5]*b + v[9]*c  + v[13],
			v[2]*a + v[6]*b + v[10]*c + v[14]);
	};
	// BACK TO MAP ORDER. The matrix is the renderer's, so what comes out of it
	// is in the renderer's GL order -- (x, HEIGHT, y) -- while a map position
	// is (x, y, z). Swapped here, exactly as AActor::GetBonePosition swaps its
	// own result (p_mobj.cpp).
	//
	// This shipped without the swap. Every answer sat |y - z| away from the
	// model it described -- hundreds of units on a real map -- and every
	// caller had a distance guard that quietly threw the answer away and fell
	// back to something else. So it read as "roughly works" everywhere and was
	// right nowhere: grab points, a magazine well and reach markers all landed
	// out in the map, and the palm and holster positions built on it were never
	// once used.
	auto toMap = [](const DVector3 &g) { return DVector3(g.X, g.Z, g.Y); };
	posOut = toMap(xf(mx, my, mz));

	// Axes as differences from the transformed origin, so translation cancels
	// and any scale baked into the matrix normalises away.
	const DVector3 org = xf(0, 0, 0);
	DVector3 fx = toMap(xf(1, 0, 0) - org);
	DVector3 fy = toMap(xf(0, 1, 0) - org);
	if (fx.Length() > 1e-9) fwdOut = fx / fx.Length();
	if (fy.Length() > 1e-9) upOut  = fy / fy.Length();
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, ModelPointToWorld, ModelWorldTransform)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_FLOAT(mx);
	PARAM_FLOAT(my);
	PARAM_FLOAT(mz);
	DVector3 pos, fwd, up;
	ModelWorldTransform(self, mx, my, mz, pos, fwd, up);
	if (numret > 2) ret[2].SetVector(up);
	if (numret > 1) ret[1].SetVector(fwd);
	if (numret > 0) ret[0].SetVector(pos);
	return numret;
}

// RS FORK -- WHERE A CHILD OF THIS ACTOR IS DRAWN (AActor::FollowActor).
//
// A child seated in this actor's frame is drawn in the frame ObjectToWorldMatrix
// hands back through followFrameOut -- origin and rotation, WITHOUT this actor's
// scale. ModelPointToWorld answers through the full matrix, scale included, so
// the two disagree whenever the model is scaled -- and flatly contradict each
// other when a MODELDEF Scale is negative, because a mirror flips an axis. A
// script that works out a child's seat from ModelPointToWorld then puts it on
// the wrong side of the parent. Found on a gun drawn at Scale -0.82: every grab
// sphere sat off the gun, correctly spaced, on the far side.
//
// So this answers from THE SAME FRAME the renderer seats the child in. Input is
// a seat as FollowActorOfs takes one (X forward, Y left, Z up, frame units).
// Returns the world point and the three axis vectors UNNORMALISED -- each is one
// frame unit along that axis, in map units -- so a caller gets the frame's units
// along with its directions and never has to guess them.
static void ModelFollowFrameTransform(AActor *self, double sx, double sy, double sz,
	DVector3 &posOut, DVector3 &axXOut, DVector3 &axYOut, DVector3 &axZOut)
{
	posOut = DVector3(0, 0, 0);
	axXOut = DVector3(1, 0, 0);
	axYOut = DVector3(0, 1, 0);
	axZOut = DVector3(0, 0, 1);
	if (self == nullptr) return;

	FSpriteModelFrame *smf = FindModelFrame(self, self->sprite, self->frame, false);
	if (smf == nullptr) return;

	VSMatrix frame;
	smf->ObjectToWorldMatrix(self, (float)self->X(), (float)self->Y(), (float)self->Z(), I_GetTimeFrac(), &frame);

	const FLOATTYPE *v = frame.get();
	// A seat goes into the frame as (x, z, y), exactly as ModelFollowFrame
	// translates one, and comes back out of the renderer's order into the map's.
	auto xf = [&](double fwd, double left, double up) {
		const double gx = fwd, gy = up, gz = left;
		return DVector3(
			v[0]*gx + v[4]*gy + v[8]*gz  + v[12],
			v[1]*gx + v[5]*gy + v[9]*gz  + v[13],
			v[2]*gx + v[6]*gy + v[10]*gz + v[14]);
	};
	auto toMap = [](const DVector3 &g) { return DVector3(g.X, g.Z, g.Y); };

	const DVector3 org = xf(0, 0, 0);
	posOut = toMap(xf(sx, sy, sz));
	axXOut = toMap(xf(1, 0, 0) - org);
	axYOut = toMap(xf(0, 1, 0) - org);
	axZOut = toMap(xf(0, 0, 1) - org);
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, ModelFollowFrameToWorld, ModelFollowFrameTransform)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_FLOAT(sx);
	PARAM_FLOAT(sy);
	PARAM_FLOAT(sz);
	DVector3 pos, ax, ay, az;
	ModelFollowFrameTransform(self, sx, sy, sz, pos, ax, ay, az);
	if (numret > 3) ret[3].SetVector(az);
	if (numret > 2) ret[2].SetVector(ay);
	if (numret > 1) ret[1].SetVector(ax);
	if (numret > 0) ret[0].SetVector(pos);
	return numret;
}

// RS FORK -- IS THIS ACTOR SOLID, OR A BILLBOARD?
//
// Everything that draws an actor in a frame of its own -- a controller's
// (FollowHandMode), the body's, another model's -- works on MODELS only. A
// sprite has no frame to be placed in: it is drawn at the actor's position,
// facing you, whatever those fields say. So a caller that is about to put
// something in a hand has to know which it is holding, or it builds for a
// model and gets a billboard -- found as caught barrels "disappearing": the
// hold scaled a sprite up a hundredfold for a frame it was never drawn in.
//
// HasModelFrame: drawn as a model or voxel by the ordinary lookup, without any
// per-actor voxel override -- its own MODELDEF, or a voxel r_drawvoxels would
// draw anyway. HasVoxelFrame: a voxel exists for its current sprite frame,
// whether or not anything is drawing it -- the question to ask BEFORE setting
// VoxelOverride, since a pack is optional and the override finds nothing
// without one.
DEFINE_ACTION_FUNCTION(AActor, HasModelFrame)
{
	PARAM_SELF_PROLOGUE(AActor);
	const bool dropped = !!(self->flags & MF_DROPPED);
	FSpriteModelFrame *smf = FindModelFrame(
		(self->modelData != nullptr && self->modelData->modelDef != nullptr) ? self->modelData->modelDef : self->GetClass(),
		(self->flags9 & MF9_DECOUPLEDANIMATIONS), self->sprite, self->frame, dropped);
	ACTION_RETURN_BOOL(smf != nullptr);
}

DEFINE_ACTION_FUNCTION(AActor, HasVoxelFrame)
{
	PARAM_SELF_PROLOGUE(AActor);
	if (FindVoxelFrame(self->sprite, self->frame, !!(self->flags & MF_DROPPED)) == nullptr) ACTION_RETURN_BOOL(false);

	// RS FORK -- r_voxels_mode 2 ("none") ANSWERS WHAT THE RENDERER WILL DO.
	//
	// Required, not tidy-up. RS_Held decides solidInHand from
	// HasModelFrame() || (VoxelHeld && HasVoxelFrame()). In mode 2 the override
	// is refused (FindModelFrame below), so a yes here puts the hand frame on a
	// billboard -- the caught-barrels-disappearing bug described above.
	// HasModelFrame needs nothing: it already goes through the gated lookup.
	// A frame that exists only as a voxel still says yes, because the renderer
	// keeps that voxel in every mode (KeepVoxelWithoutSprite).
	ACTION_RETURN_BOOL(VoxelsEffectiveMode() != 2 || !SpriteFrameHasTexture(self->sprite, self->frame));
}

// PLACEMENT CVARS ARE `user` CVARS, AND FindCVar CANNOT READ THOSE.
//
// FindCVar hands back the raw FBaseCVar. For a CVAR_USERINFO cvar that object
// is not where the value lives: c_cvars.cpp's own GetCVar exists to say so and
// redirects through callbacks->GetUserCVar(playernum, name) for precisely this
// case. Read the raw one and you get 0.
//
// Which is the worst answer available, because 0 is a legal-looking number.
// Every guard downstream is `if (v > 0)`, so a scale silently stays 1 and an
// offset silently stays 0 -- the model draws exactly as if every slider were
// centred, moving a slider does nothing, and nothing is logged to say the value
// never arrived. It is why the drawn reach volume came out a fixed sphere
// rather than the oval its three separate semi-axes describe, and why the
// offsets could not move it.
//
// Returns false only when the cvar genuinely does not exist, so a caller keeps
// its own default instead of being handed a zero.
static bool GetPlacementCVar(const char *name, float &out)
{
	FBaseCVar *cv = GetCVar(consoleplayer, name);
	if (cv == nullptr) return false;
	out = (float)cv->GetGenericRep(CVAR_Float).Float;
	return true;
}

// ---- SURFACE SLOT POSES: the one place a part's live transform is worked out
//
// What a surface slot does to its part this frame, in its model's own space.
// Read by the part's own draw (the per-surface loop in RenderFrameModel) and by
// anything riding that part (ModelFollowFrame). One set of functions, so the
// two cannot drift.
//
// PURE. These read the slot table and never write it. Arming a drive's anchor
// on the first drawn frame, re-anchoring at the clamp, publishing the drawn
// value and the debug trace all belong to the OWNER's draw and stay there -- so
// what a follower computes cannot depend on which of the two is drawn first.

// THE HAND DRIVE ITSELF -- where a hand is along a part's travel axis, a driven
// part's pose, the follower's value, the hinge and two-stage solve, and the
// owner's arm / solve / publish -- is r_data/model_handdrive.h/.cpp (HandDrive_*),
// shared with every other kind of part a hand drives, so a surface and a joint can
// never disagree. What stays here is the surface side: the script-set transform
// below, the stage trace, and a follower's slot lookup.

// A script-set part transform, INTERPOLATED TO THE DRAWN INSTANT exactly as the
// frame position is. The transform path shipped without this while the frame
// path had it, so a part driven by both moved in two time bases at once -- the
// pose gliding, the placement stepping at 35 Hz.
//
// NLERP, SHORTEST ARC. Cheap, and correct for the small per-tic deltas a hand-
// driven part actually produces -- slerp buys accuracy only across wide arcs
// that cannot happen in one 35th of a second. The dot-sign flip is not
// optional: without it a quaternion and its negation, which are the same
// rotation, interpolate the long way round and the part spins most of a full
// turn inside one tic.
static void SurfaceSetPose(const FVector3 *ovOfs, const FVector4 *ovRot, const FVector3 *ovOfsPrev, const FVector4 *ovRotPrev,
	int s, double ticFrac, FVector3 &offset, FVector4 &rotation)
{
	float f = (float)ticFrac;
	if (f < 0.f) f = 0.f;
	if (f > 1.f) f = 1.f;

	const FVector3 curOfs = ovOfs[s];
	const FVector3 prvOfs = ovOfsPrev ? ovOfsPrev[s] : curOfs;
	offset = prvOfs + (curOfs - prvOfs) * f;

	const FVector4 qc = ovRot[s];
	const FVector4 cur = (qc.X == 0.f && qc.Y == 0.f && qc.Z == 0.f && qc.W == 0.f)
		? FVector4(0.f, 0.f, 0.f, 1.f) : qc;
	const FVector4 qp = ovRotPrev ? ovRotPrev[s] : cur;
	FVector4 prv = (qp.X == 0.f && qp.Y == 0.f && qp.Z == 0.f && qp.W == 0.f)
		? FVector4(0.f, 0.f, 0.f, 1.f) : qp;

	float dot = prv.X * cur.X + prv.Y * cur.Y + prv.Z * cur.Z + prv.W * cur.W;
	if (dot < 0.f) prv = FVector4(-prv.X, -prv.Y, -prv.Z, -prv.W);

	FVector4 blend(
		prv.X + (cur.X - prv.X) * f,
		prv.Y + (cur.Y - prv.Y) * f,
		prv.Z + (cur.Z - prv.Z) * f,
		prv.W + (cur.W - prv.W) * f);

	const float len = (float)g_sqrt(blend.X * blend.X + blend.Y * blend.Y
		+ blend.Z * blend.Z + blend.W * blend.W);
	rotation = (len > 0.0001f)
		? FVector4(blend.X / len, blend.Y / len, blend.Z / len, blend.W / len)
		: FVector4(0.f, 0.f, 0.f, 1.f);
}

// DIAGNOSTIC for staged drives: renderer-local, and always on, because it
// prints only when one arms or its hand changes stage, which is a few lines per
// grab. One entry per (model data, slot) being driven in stages. Entries are
// recycled in turn, so a stale one costs at most a mislabelled line.
struct FSurfaceStageTrace
{
	const DActorModelData *md = nullptr;
	int      slot       = -1;
	int      logged     = 0;     // stage last reported as the one the hand is working
	int      pending    = 0;     // stage crossed into, not yet reported
	float    crossPrev  = 0.f;   // the crossing frame: value drawn the frame before
	float    crossV     = 0.f;   // ... value drawn on it
	float    crossCarry = 0.f;   // ... share of its hand motion given to the new stage
	uint64_t armMs      = 0;
};

static FSurfaceStageTrace *SurfaceStageTrace(const DActorModelData *md, int slot, bool create)
{
	static FSurfaceStageTrace table[32];
	static int next = 0;
	for (auto &t : table)
		if (t.md == md && t.slot == slot) return &t;
	if (!create) return nullptr;
	FSurfaceStageTrace &t = table[next];
	next = (next + 1) % 32;
	t = FSurfaceStageTrace();
	t.md = md;
	t.slot = slot;
	return &t;
}

// The pose a follower rides for slot s this frame -- the owner's branches, in
// the owner's order: a live drive when there is a hand pose, else a set
// transform. False when the slot moves nothing (empty, or a frame-only part),
// and the follower rides the whole model instead.
static bool SurfaceSlotPoseForFollower(const DActorModelData *md, int s, const VSMatrix &modelToWorld, double ticFrac,
	FVector3 &offset, FVector4 &rotation)
{
	if (md == nullptr || s < 0 || s >= DActorModelData::RS_SURF_SLOTS) return false;
	if (md->SurfOvModel[s] < 0 || md->SurfOvSurface[s] < 0) return false;

	if (md->SurfDrive[s].on)
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		VSMatrix handMat;
		const int dhand = (md->SurfDrive[s].hand == 1) ? VR_OFFHAND : VR_MAINHAND;
		if (vrmode && vrmode->IsVR() && vrmode->GetHandTransform(VR_ControllerForHand(dhand), &handMat))
		{
			// A hinge or two-stage drive rides its own solve; a plain drive
			// exactly as it always has (HandDrive_PoseForFollower).
			HandDrive_PoseForFollower(md->SurfDrive[s], handMat, modelToWorld, offset, rotation);
			return true;
		}
	}

	if (!md->SurfOvHasXf[s]) return false;
	SurfaceSetPose(md->SurfOvOfs, md->SurfOvRot, md->SurfOvOfsPrev, md->SurfOvRotPrev, s, ticFrac, offset, rotation);
	return true;
}

// AActor::FollowActorOfsInModel -- a child's seat given as a point ON THE
// PARENT'S MESH, carried into the follow frame.
//
// The follow frame (ModelFollowFrame, below) leaves out the parent's scale, any
// mirror a negative MODELDEF Scale puts in, and its MODELDEF base orientation --
// on purpose, so a big holster does not stretch the gun in it. A point read off
// the mesh (a weapon card's grab point, a bone, a marker) lives in the space the
// vertices do, which has all three; used as a frame seat it lands the wrong size,
// turned and on the wrong side. Found on a rifle drawn at Scale -0.82.
//
// So the renderer converts it, from the two matrices it already has:
//
//     seat = frame^-1 * parentMat * P
//
// frame is the follow frame exactly as ObjectToWorldMatrix handed it back, BEFORE
// a slot's carry. The child is then drawn at frame * seat = parentMat * P, the
// point on the mesh; with FollowActorSlot, at (parentMat * part * parentMat^-1) *
// frame * seat = parentMat * part * parentMat^-1 * parentMat * P
// = parentMat * part * P, so the
// point rides the moving part. ONLY THE POSITION: the child keeps the follow
// frame's orientation, so its Angles and wrist sliders turn as they always did.
//
// P is in the space ModelPointToWorld takes: the renderer's model space, y up, as
// the MD3 loader stores every vertex (x, z, y). The result is in the order
// FollowActorOfs is (X forward, Y left, Z up, frame units), so the seat cvars add
// to it unchanged.
//
// DOUBLE PRECISION, TRANSLATIONS CANCELLED FIRST. Both matrices carry a world
// position thousands of units out; cancelling that inside a float inverse costs a
// visible fraction of a unit. So parentMat * P is taken relative to the frame's
// origin and only the frame's 3x3 is inverted (Cramer's rule). False when that
// 3x3 is degenerate, and the caller keeps FollowActorOfs as a frame seat.
static bool FollowSeatFromModelPoint(AActor *child, AActor *parent, const VSMatrix &frame, const VSMatrix &parentMat, DVector3 &seatOut)
{
	const FLOATTYPE *f = frame.get();
	const FLOATTYPE *m = parentMat.get();
	const DVector3 P = child->FollowActorOfs;

	// parentMat * P, relative to the frame's origin. Column-major, as everywhere
	// in this file.
	const DVector3 w(
		double(m[0])*P.X + double(m[4])*P.Y + double(m[8]) *P.Z + (double(m[12]) - double(f[12])),
		double(m[1])*P.X + double(m[5])*P.Y + double(m[9]) *P.Z + (double(m[13]) - double(f[13])),
		double(m[2])*P.X + double(m[6])*P.Y + double(m[10])*P.Z + (double(m[14]) - double(f[14])));

	// Solve frame3x3 * g = w on the frame's three columns.
	const DVector3 c0(f[0], f[1], f[2]);
	const DVector3 c1(f[4], f[5], f[6]);
	const DVector3 c2(f[8], f[9], f[10]);
	const double det = c0 | (c1 ^ c2);
	const bool ok = fabs(det) > 1e-12;

	DVector3 seat = P;
	double scaleRel = 0.0;
	if (ok)
	{
		const double gx = (w  | (c1 ^ c2)) / det;
		const double gy = (c0 | (w  ^ c2)) / det;
		const double gz = (c0 | (c1 ^ w )) / det;
		// Renderer order back into a seat's: (x, z, y), as the translate reads it.
		seat = DVector3(gx, gz, gy);
		seatOut = seat;

		// The parent's model-to-frame scale, signed: negative is a mirror. The one
		// number that says the MODELDEF Scale was taken into account.
		const DVector3 m0(m[0], m[1], m[2]);
		const DVector3 m1(m[4], m[5], m[6]);
		const DVector3 m2(m[8], m[9], m[10]);
		const double rel = (m0 | (m1 ^ m2)) / det;
		scaleRel = rel < 0.0 ? -cbrt(-rel) : cbrt(rel);
	}

	// PROOF IN ONE HEADSET TEST. This is the render path, once per eye, so never a
	// line per draw: one the first time a child resolves a model-space seat, then
	// again only when its parent, slot, point or resolved seat changes -- at most
	// twice a second per child, and sixteen lines a second in all, so a slider drag
	// or a field of grab markers cannot flood the log. thread_local like
	// followDepth: playsim bone queries reach this too.
	struct SeatTrace { const AActor *child; const AActor *parent; int slot; double p[3], seat[3]; uint64_t seenMs, printMs; bool printed; };
	static thread_local SeatTrace traces[32];
	static thread_local uint64_t windowMs = 0;
	static thread_local int windowLines = 0;

	const uint64_t now = I_msTime();
	SeatTrace *t = nullptr;
	for (auto &e : traces) if (e.child == child) { t = &e; break; }
	if (t == nullptr)
	{
		t = &traces[0];
		for (auto &e : traces) if (e.seenMs < t->seenMs) t = &e;

		// EVICTION IS NOT A FIRST SIGHTING. If even the least recently seen entry
		// was drawn within the last second, every slot is live and the table is
		// churning -- more children than it holds. Treating each re-entry as new
		// would print a line per eviction, every frame, held back only by the
		// global cap. So a churned-in child enters QUIET, its current numbers taken
		// as already reported, and still waits out its own throttle; only an empty
		// slot or one stale for a second gives a real first line. The cost, and only
		// past 32 live children: a new child's first line can go unprinted.
		const bool churning = t->child != nullptr && now - t->seenMs < 1000;
		memset(t, 0, sizeof(*t));
		t->child = child;
		if (churning)
		{
			t->printed = true;
			t->printMs = now;
			t->parent = parent;
			t->slot = child->FollowActorSlot;
			t->p[0] = P.X; t->p[1] = P.Y; t->p[2] = P.Z;
			t->seat[0] = seat.X; t->seat[1] = seat.Y; t->seat[2] = seat.Z;
		}
	}
	t->seenMs = now;

	auto differs = [](const double *a, const DVector3 &b) {
		return fabs(a[0] - b.X) > 0.01 || fabs(a[1] - b.Y) > 0.01 || fabs(a[2] - b.Z) > 0.01;
	};
	const bool changed = !t->printed || t->parent != parent || t->slot != child->FollowActorSlot
		|| differs(t->p, P) || differs(t->seat, seat);
	if (changed && (!t->printed || now - t->printMs >= 500))
	{
		if (now - windowMs >= 1000) { windowMs = now; windowLines = 0; }
		if (windowLines < 16)
		{
			windowLines++;
			t->printed = true;
			t->printMs = now;
			t->parent = parent;
			t->slot = child->FollowActorSlot;
			t->p[0] = P.X; t->p[1] = P.Y; t->p[2] = P.Z;
			t->seat[0] = seat.X; t->seat[1] = seat.Y; t->seat[2] = seat.Z;

			// Where the point is on the parent's mesh with its part at rest, map
			// order, AS DRAWN: parentMat here is built from the parent's
			// INTERPOLATED position plus WorldOffset (less Floorclip for a facing
			// sprite), which is what the renderer uses. ModelPointToWorld(P) builds
			// from raw X/Y/Z, so the two can differ by up to a tic of the parent's
			// motion and by its WorldOffset -- labelled so that is not read as a
			// mismatch.
			const DVector3 rest(w.X + f[12], w.Z + f[14], w.Y + f[13]);
			Printf("[FOLLOWSEAT] %s on %s slot %d: seat resolved in PARENT MODEL space  P=(%.3f %.3f %.3f)  model->frame scale %.4f  ->  frame seat (fwd left up)=(%.3f %.3f %.3f)  mesh point at rest, drawn/interpolated=(%.2f %.2f %.2f)%s\n",
				child->GetClass()->TypeName.GetChars(), parent->GetClass()->TypeName.GetChars(), child->FollowActorSlot,
				P.X, P.Y, P.Z, scaleRel, seat.X, seat.Y, seat.Z, rest.X, rest.Y, rest.Z,
				ok ? "" : "  FRAME DEGENERATE -- used as a follow-frame seat instead");
		}
	}
	return ok;
}

// AActor::FollowActorJoint -- A JOINT OF THE PARENT'S MODEL, AS DRAWN (Engine docs/MODEL_JOINT_DRIVE_PLAN.md piece E).
//
// Set when it names a joint. 'None' from ZScript is no joint, as PlacementPrefix's 'None' is no prefix
// (ObjectToWorldMatrix): unset, a child takes the FollowActorSlot path exactly as before.
static bool JointFollowSet(const AActor *child)
{
	return child->FollowActorJoint != NAME_None && stricmp(child->FollowActorJoint.GetChars(), "None") != 0;
}

// [FOLLOWJOINT] -- WHAT A JOINT FOLLOWER RIDES, SAID ONCE. The render path, once per eye, so never a line per draw: one
// when a child's outcome, parent, joint or model index changes, at most twice a second per child and sixteen lines a
// second in all. "Not drawn yet" waits half a second first: every new follower spends its first frame there, before its
// parent's draw has published the joint it asked for. More than 32 live children churn the table, and a churned-in child
// enters quietly, as FollowSeatFromModelPoint's trace does. thread_local like followDepth.
static void JointFollowTrace(const AActor *child, const AActor *parent, int outcome, bool shared)
{
	struct JointTrace { const AActor *child; const AActor *parent; int joint, model, outcome, said; uint64_t seenMs, sinceMs, printMs; };
	static thread_local JointTrace traces[32];
	static thread_local uint64_t windowMs = 0;
	static thread_local int windowLines = 0;

	const uint64_t now = I_msTime();
	const int joint = child->FollowActorJoint.GetIndex();
	const int model = child->FollowActorJointModel;
	JointTrace *t = nullptr;
	for (auto &e : traces) if (e.child == child) { t = &e; break; }
	if (t == nullptr)
	{
		t = &traces[0];
		for (auto &e : traces) if (e.seenMs < t->seenMs) t = &e;
		const bool churning = t->child != nullptr && now - t->seenMs < 1000;
		memset(t, 0, sizeof(*t));
		t->child = child;
		t->parent = parent;
		t->joint = joint;
		t->model = model;
		t->outcome = outcome;
		t->sinceMs = now;
		t->said = churning ? outcome : -1;
		if (churning) t->printMs = now;
	}
	t->seenMs = now;
	if (t->outcome != outcome || t->parent != parent || t->joint != joint || t->model != model)
	{
		t->parent = parent;
		t->joint = joint;
		t->model = model;
		t->outcome = outcome;
		t->sinceMs = now;
		t->said = -1;
	}
	if (t->said == outcome) return;
	if (outcome == JFO_NOTDRAWN && now - t->sinceMs < 500) return;
	if (t->printMs != 0 && now - t->printMs < 500) return;
	if (now - windowMs >= 1000) { windowMs = now; windowLines = 0; }
	if (windowLines >= 16) return;
	windowLines++;
	t->said = outcome;
	t->printMs = now;

	const char *what = "";
	switch (outcome)
	{
	case JFO_RIDING:    what = "riding it as drawn"; break;
	case JFO_NOTDRAWN:  what = "the parent has not been drawn with bones at that model index since this was asked -- riding the whole model"; break;
	case JFO_NOJOINT:   what = "no such joint on the model the parent drew (see its line) -- riding the whole model"; break;
	case JFO_COLLAPSED: what = "the joint is drawn collapsed (hidden) or flat -- riding the whole model"; break;
	case JFO_FULL:      what = "the joint followers' table is full -- riding the whole model"; break;
	default: break;
	}
	Printf("[FOLLOWJOINT] %s on %s: joint '%s' of model %d%s: %s\n", child->GetClass()->TypeName.GetChars(),
		parent->GetClass()->TypeName.GetChars(), child->FollowActorJoint.GetChars(), model,
		shared ? " (the parent draws every model with one palette)" : "", what);
}

// The motion a child rides for its joint this frame: READ FROM WHAT THE PARENT'S DRAW PUBLISHED (model_reach.cpp,
// ModelJointFollow_Read; model_jointfollow.h), never solved here -- the Body IK lane's condition 8. Rigid, in the
// parent's model space, the identity at the bind pose. False when there is nothing to ride this frame, and the child
// rides the whole model; the reason is said once. Outside RenderModel always false and silent: a script query of a
// joint follower sees the whole model's frame, never the joint's (and FollowActorSlot is not consulted either).
static bool JointFollowMotion(AActor *child, AActor *parent, FSpriteModelFrame *psmf, double motion[16])
{
	// +DECOUPLEDANIMATIONS or MODELSAREATTACHMENTS: every model of the parent is skinned with the first one's bones
	// (RenderModelFrame's evaluatedSingle), so that one palette is the joint's, whatever model index is named.
	const bool shared = !!(parent->flags9 & MF9_DECOUPLEDANIMATIONS) || !!(psmf->getFlags(parent->modelData) & MDL_MODELSAREATTACHMENTS);
	const int outcome = ModelJointFollow_Read(parent, child->FollowActorJointModel, shared, child->FollowActorJoint, motion);
	if (outcome == JFO_NOTRENDER) return false;
	JointFollowTrace(child, parent, outcome, shared);
	return outcome == JFO_RIDING;
}

// AActor::FollowActor -- the frame a child model rides.
//
// Built by the PARENT'S OWN ObjectToWorldMatrix, never reconstructed here: that
// function hands back, beside its full matrix, the parent's drawn origin with
// the parent's own and placement rotation and its path units, and no scale or
// MODELDEF base orientation. Rebuilding that basis by hand is what cost this
// tree two attempts at the holster centring.
//
// Then, if FollowActorSlot names one, carried by that slot's live motion: the
// part's own transform conjugated into the world (into model space, the part's
// motion, back out), so at rest it is the identity and a child seated against
// slot -1 sits exactly the same against a slot. Then the child's seat.
//
// False when there is nothing to follow this frame -- no parent, no model, or a
// follow loop -- and the child draws as though the field were unset.
static thread_local int followDepth = 0;

static bool ModelFollowFrame(AActor *child, double ticFrac, VSMatrix &out)
{
	AActor *parent = child->FollowActor.Get();
	if (parent == nullptr || parent == child) return false;

	// A chain is fine -- a hand on a magazine in a gun in a holster. A loop is
	// not, and this is what ends one.
	if (followDepth >= 4) return false;

	FSpriteModelFrame *psmf = FindModelFrame(parent, parent->sprite, parent->frame, false);
	if (psmf == nullptr) return false;

	// The position the sprite pass hands RenderModel for this actor
	// (hw_sprites.cpp), less portal displacement. A parent riding the body or a
	// controller never reads it.
	DVector3 ppos = parent->InterpolatedPosition(ticFrac)
		+ DVector3(parent->WorldOffset.X, parent->WorldOffset.Y, parent->WorldOffset.Z);
	const uint32_t spritetype = (parent->renderflags & RF_SPRITETYPEMASK);
	if (spritetype == RF_FACESPRITE) ppos.Z -= parent->Floorclip;

	VSMatrix frame;
	followDepth++;
	VSMatrix parentMat = psmf->ObjectToWorldMatrix(parent, (float)ppos.X, (float)ppos.Y, (float)ppos.Z, ticFrac, &frame);
	followDepth--;

	// AActor::FollowActorOfsInModel -- resolved HERE, against the frame exactly as
	// ObjectToWorldMatrix handed it back and BEFORE a slot's carry below folds the
	// part's motion into it. That order is what makes the point ride the part.
	DVector3 modelSeat;
	const bool seatInModel = child->FollowActorOfsInModel
		&& FollowSeatFromModelPoint(child, parent, frame, parentMat, modelSeat);

	FVector3 partOfs;
	FVector4 partRot;
	if (JointFollowSet(child))
	{
		// AActor::FollowActorJoint -- in FollowActorSlot's place, at the same point in the order: after a model-space seat
		// is resolved against the un-carried frame, so a point on the mesh rides the joint as a slot's rides its part.
		// Nothing to ride this frame (or a script query): the frame stays the whole model's.
		double jointMotion[16];
		if (JointFollowMotion(child, parent, psmf, jointMotion))
			JointFollowCarry(parentMat, jointMotion, frame);
	}
	else if (child->FollowActorSlot >= 0
		&& SurfaceSlotPoseForFollower(parent->modelData.ForceGet(), child->FollowActorSlot, parentMat, ticFrac, partOfs, partRot))
	{
		// Built exactly as models_md3.cpp builds a surface's transform.
		VSMatrix local;
		local.loadIdentity();
		local.translate(partOfs.X, partOfs.Y, partOfs.Z);
		if (partRot.X != 0.f || partRot.Y != 0.f || partRot.Z != 0.f || partRot.W != 1.f)
			local.multQuaternion(partRot);

		VSMatrix toModel;
		parentMat.inverseMatrix(toModel);

		VSMatrix carried = parentMat;
		carried.multMatrix(local);
		carried.multMatrix(toModel);
		carried.multMatrix(frame);
		frame = carried;
	}

	// The child's seat, Doom-local into the renderer's axes the same way the
	// world translate below writes a position: (x, z, y). With
	// FollowActorOfsInModel it is the mesh point already carried into this frame.
	DVector3 seat = seatInModel ? modelSeat : child->FollowActorOfs;

	// AActor::FollowActorOfsCVar -- the tunable half of that seat, read HERE so
	// it answers with the playsim frozen behind a menu. Added in the frame's own
	// units and NOT divided by the child's scale: the seat belongs to the parent
	// frame, so resizing the child must not move it, and one cvar set can seat
	// many children of different sizes.
	auto addSeatCVars = [&seat](FName prefix)
	{
		if (prefix == NAME_None) return;
		const char *pre = prefix.GetChars();
		char nm[160];
		float v;
		snprintf(nm, sizeof(nm), "%s_ofs_x", pre); if (GetPlacementCVar(nm, v)) seat.X += v;
		snprintf(nm, sizeof(nm), "%s_ofs_y", pre); if (GetPlacementCVar(nm, v)) seat.Y += v;
		snprintf(nm, sizeof(nm), "%s_ofs_z", pre); if (GetPlacementCVar(nm, v)) seat.Z += v;
	};
	addSeatCVars(child->FollowActorOfsCVar);
	addSeatCVars(child->FollowActorOfsCVar2);

	frame.translate((float)seat.X, (float)seat.Z, (float)seat.Y);

	out = frame;
	return true;
}

VSMatrix FSpriteModelFrame::ObjectToWorldMatrix(AActor * actor, float x, float y, float z, double ticFrac, VSMatrix *followFrameOut)
{
	int smf_flags = getFlags(actor->modelData);

	// [BB] A VOXEL ASKED FOR BY HAND TURNS WITH THE HAND.
	//
	// An actor with VoxelOverride set has been switched to its voxel for a
	// reason -- something is holding it, and a held thing has to answer the
	// wrist. But pitch and roll are opt-in per model definition, and no voxel
	// pack in the wild sets them: they were authored for scenery standing on a
	// floor, where the only meaningful rotation is yaw. The pack this was
	// written against declares AngleOffset on all 74 entries and
	// UseActorPitch/UseActorRoll on none of them, which is typical.
	//
	// Forcing both here rather than asking authors to re-tag their packs, and
	// doing it on a LOCAL copy of the flags rather than on the shared
	// FSpriteModelFrame, so nothing leaks to the same voxel drawn elsewhere in
	// the level. Costs one OR on actors that have the field set and nothing at
	// all on those that do not.
	// MDL_VOXELBODYAXIS rides along so the matrix overload below -- which is
	// handed flags and no actor -- knows this one is held.
	//
	// RS FORK -- r_voxels_mode 2 refuses VoxelOverride (FindModelFrame), so an
	// actor carrying the flag can reach here drawn as its MODELDEF model rather
	// than its voxel -- RS_Pull sets the flag on every grab regardless of mode.
	// The held-voxel treatment (this line, the trace below, the mid-height
	// pivot further down) belongs to the voxel, so it stays off a model the
	// mode put back. Modes 0 and 1 are unchanged: the right side is true.
	const bool voxelOverrideDrawn = actor->VoxelOverride && (isVoxel || VoxelsEffectiveMode() != 2);
	if (voxelOverrideDrawn) smf_flags |= MDL_USEACTORPITCH | MDL_USEACTORROLL | MDL_VOXELBODYAXIS;

	// The same opt-in without the voxel, for an actor wearing a model it does
	// not own -- a holstered weapon above all. See the field note in actor.h.
	// No MDL_VOXELBODYAXIS here: the body-axis correction undoes a VOXEL pack's
	// angleoffset, and a borrowed MODELDEF's offsets are already the ones its
	// own weapon is drawn with.
	if (actor->ForceModelAngles) smf_flags |= MDL_USEACTORPITCH | MDL_USEACTORROLL;

	// [BB] HELD-VOXEL DIAGNOSTIC.
	//
	// Which quarter turn a pack is off by is not something anyone should have
	// to find by feel in a headset, and it is not guessable from the pack
	// either -- it depends on how its author laid the voxels out. But it IS
	// derivable from the three model offsets against the three actor angles,
	// and both of those are right here.
	//
	// Throttled to once a second per actor rather than once per draw: this runs
	// on the render path, which is called per eye, so an unthrottled Printf
	// would be two lines a frame and would itself cost frametime.
	if (voxelOverrideDrawn && vr_voxel_debug)	// RS fork -- r_voxels_mode, see above
	{
		static const AActor *lastActor = nullptr;
		static int lastTic = -1000;
		if (actor != lastActor || (gametic - lastTic) > TICRATE)
		{
			lastActor = actor;
			lastTic = gametic;
			Printf("[RSVOX] %s  modeloffsets angle=%.1f pitch=%.1f roll=%.1f  |  actor yaw=%.1f pitch=%.1f roll=%.1f  |  bodyyaw=%.1f  usepitch=%d useroll=%d pivotz=%.1f\n",
				actor->GetClass()->TypeName.GetChars(),
				angleoffset, pitchoffset, rolloffset,
				actor->Angles.Yaw.Degrees(), actor->Angles.Pitch.Degrees(), actor->Angles.Roll.Degrees(),
				// The DERIVED body yaw, not the raw cvar. The cvar is a
				// sentinel when negative and printing it raw said "bodyyaw=-1"
				// while the renderer was using 90 -- a trace that reports the
				// input rather than the decision is worse than none.
				(vr_voxel_rollaxis < 0.f) ? angleoffset : (float)vr_voxel_rollaxis,
				!!(smf_flags & MDL_USEACTORPITCH), !!(smf_flags & MDL_USEACTORROLL),
				// The PIVOT ACTUALLY USED, in map units. This used to print
				// MDL_USEROTATIONCENTER, which is a MODELDEF flag no voxel pack
				// sets -- so it read 0 on every line while the per-actor pivot
				// added alongside it was working fine. It described the old
				// mechanism, not the live one.
				float(actor->Height * 0.5));
		}
	}

	// Setup transformation.
	DRotator angles;

	if (actor->renderflags & RF_INTERPOLATEANGLES) // [Nash] use interpolated angles
		angles = actor->InterpolatedAngles(ticFrac);
	else
		angles = actor->Angles;

	float angle = angles.Yaw.Degrees();
	float pitch = 0;
	float roll = 0;

	// [BB] Workaround for the missing pitch information.
	if ((smf_flags & MDL_PITCHFROMMOMENTUM))
	{
		const double x = actor->Vel.X;
		const double y = actor->Vel.Y;
		const double z = actor->Vel.Z;

		if (actor->Vel.LengthSquared() > EQUAL_EPSILON)
		{
			// [BB] Calculate the pitch using spherical coordinates.
			if (z || x || y) pitch = float(atan(z / sqrt(x*x + y*y)) / M_PI * 180);

			// Correcting pitch if model is moving backwards
			if (fabs(x) > EQUAL_EPSILON || fabs(y) > EQUAL_EPSILON)
			{
				if ((x * cos(angle * M_PI / 180) + y * sin(angle * M_PI / 180)) / sqrt(x * x + y * y) < 0) pitch *= -1;
			}
			else pitch = fabs(pitch);
		}
	}

	// Added MDL_USEACTORPITCH and MDL_USEACTORROLL flags processing.
	// If both flags MDL_USEACTORPITCH and MDL_PITCHFROMMOMENTUM are set, the pitch sums up the actor pitch and the velocity vector pitch.
	if (smf_flags & MDL_USEACTORPITCH)
	{
		double d = angles.Pitch.Degrees();
		if (smf_flags & MDL_BADROTATION) pitch += d;
		else pitch -= d;
	}
	if (smf_flags & MDL_USEACTORROLL) roll += angles.Roll.Degrees();

	// [Nash] take SpriteRotation into account
	angle += actor->SpriteRotation.Degrees();

	double tic = actor->GetModelTimer();

	if (!WorldPaused(true) && !actor->isFrozen())
	{
		tic += ticFrac;
	}

	// TURN IT ABOUT ITS MIDDLE, NOT ITS FEET.
	//
	// An actor's origin sits on the floor between its feet, and every rotation
	// below is applied about that origin. For scenery standing in a room that is
	// exactly right -- a barrel turns on the spot. For a barrel in your hand it
	// is not: the thing you are holding swings through an arc the length of its
	// own height, which reads as the object pivoting about a point somewhere
	// below it rather than turning where you are holding it.
	//
	// Half the height is the honest approximation. The real answer is where the
	// hand actually gripped it, which nothing here knows; the midpoint is right
	// for the upright cylinders this mostly picks up and wrong by less than half
	// a height for everything else.
	// RS fork -- voxelOverrideDrawn, not the raw flag: r_voxels_mode 2 (see above).
	const float bodyPivotZ = voxelOverrideDrawn ? float(actor->Height * 0.5) : 0.f;

	// AActor::FollowActor -- inside another model's drawn frame. One pointer
	// test for every actor that never sets it. See ModelFollowFrame.
	VSMatrix followFrame;
	const bool following = ModelFollowFrame(actor, ticFrac, followFrame);

	// AActor::ScaleCVar -- the size a setting says, read here so it answers with
	// the playsim frozen. Zero, or a cvar that does not exist, leaves the actor's
	// own scale alone: a slider at "off" hands the size back to its owner rather
	// than shrinking the model to nothing.
	DVector2 drawScale = actor->InterpolatedScale(ticFrac);
	if (actor->ScaleCVar != NAME_None)
	{
		float sv = 0.f;
		if (GetPlacementCVar(actor->ScaleCVar.GetChars(), sv) && sv > 0.f)
			drawScale = DVector2(sv * actor->ScaleCVarUnit, sv * actor->ScaleCVarUnit);
	}

	// AActor::FollowBodyYawInterp -- the body-frame heading as drawn: the value as written
	// unless the actor opts in to draw-rate interpolation (DrawFollowBodyYaw, actor.h).
	// AActor::FollowHandRot is always drawn interpolated (DrawFollowHandRot, actor.h).
	return ObjectToWorldMatrix(actor->Level, DVector3(x, y, z), DRotator(DAngle::fromDeg(pitch), DAngle::fromDeg(angle), DAngle::fromDeg(roll)), drawScale, smf_flags, tic, bodyPivotZ, actor->FollowBodyMode, actor->FollowBodyOfs, actor->DrawFollowBodyYaw(ticFrac), actor->FollowHandMode, actor->FollowHandOfs, actor->PlacementPrefix,
		following ? &followFrame : nullptr, followFrameOut, actor->ScaleAxes, actor->DrawFollowHandRot(ticFrac));
}

VSMatrix FSpriteModelFrame::ObjectToWorldMatrix(FLevelLocals *Level, DVector3 translation, DRotator rotation, DVector2 scaling, unsigned int flags, double tic, float bodyPivotZ, int followBodyMode, DVector3 followBodyOfs, double followBodyYaw, int followHandMode, DVector3 followHandOfs, FName placementPrefix, const VSMatrix *followFrameIn, VSMatrix *followFrameOut, DVector3 scaleAxes, DVector3 followHandRot)
{
	double rotateOffset = 0;

	if (flags & MDL_ROTATING)
	{
		if (rotationSpeed > 0.0000000001 || rotationSpeed < -0.0000000001)
		{
			double turns = (tic) / (200.0 / rotationSpeed);
			turns -= floor(turns);
			rotateOffset = turns * 360.0;
		}
		else
		{
			rotateOffset = 0.0;
		}
	}

	// y scale for a sprite means height, i.e. z in the world!
	float scaleFactorX = scaling.X * xscale;
	float scaleFactorY = scaling.X * yscale;
	float scaleFactorZ = scaling.Y * zscale;

	VSMatrix objectToWorldMatrix;
	objectToWorldMatrix.loadIdentity();

	// MDL_FOLLOWMAINHAND / MDL_FOLLOWOFFHAND -- see the flag comment in models.h.
	//
	// Deliberately GetWeaponTransform and not a reconstruction of it. Two prior
	// attempts to rebuild this engine's rotation basis by hand each passed their
	// own self-consistency check and each still landed every prop 4.55 units off,
	// because neither accounted for RenderModel negating pitch before rotating.
	// Replaying the engine's own transform is the only approach in this tree's
	// history that ever worked, so this calls the exact function the working HUD
	// path calls and takes the matrix whole -- no decomposition, no Euler round
	// trip, nothing to get the axis order wrong in.
	// AActor::FollowBodyMode -- the same idea one step out from the hand: the
	// player's own frame, read at draw rate, with the actor's seat inside it.
	// Taken WHOLE from GetHmdTransform rather than rebuilt, for the reason the
	// hand path gives below -- re-deriving this basis by hand is what cost the
	// two earlier attempts, and the body frame is built the same way the hand
	// one is precisely so the two agree.
	//
	// The seat is applied in the body's frame BEFORE any of the model's own
	// offsets, so MODELDEF Offset and the placement sliders keep meaning what
	// they mean everywhere else: adjustments relative to where the thing sits.
	// followedBody and followedHand are SEPARATE because they want opposite
	// things from the actor's rotation, and collapsing them into one flag is
	// what drew every holstered weapon barrel-forward instead of barrel-down.
	bool followedBody = false;
	bool followedHand = false;

	// AActor::FollowActor, handed down as a finished frame (ModelFollowFrame):
	// another model's drawn frame with this actor's seat already in it. Loaded
	// whole, like the body and hand frames below, and it outranks both -- the
	// parent already rides whichever of those it rides. This actor's own
	// rotation then applies RELATIVE to it, and the world translate is skipped.
	bool followedActor = false;
	if (followFrameIn != nullptr)
	{
		objectToWorldMatrix = *followFrameIn;
		followedActor = true;
	}

	if (!followedActor && followBodyMode > 0)
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		float bodyYaw = 0.f;
		if (vrmode != nullptr && vrmode->IsVR() &&
			vrmode->GetHmdTransform(&objectToWorldMatrix, followBodyOfs, &bodyYaw,
				followBodyMode == 2 ? followBodyYaw : NAN))
		{
			// The actor's Angles are WORLD angles -- a caller writes body yaw
			// plus its own offset, because the same numbers have to drive the
			// no-headset path where SetOrigin places the actor for real. The
			// frame has already applied that heading, so subtract it here or it
			// counts twice and the thing swings out as you turn.
			//
			// PITCH AND ROLL PASS THROUGH UNTOUCHED. They are frame-independent,
			// and they are the whole reason a holstered gun hangs barrel-down.
			rotation.Yaw -= DAngle::fromDeg(bodyYaw);
			followedBody = true;
		}
		else
		{
			// Not in VR, or no pose this frame. Fall back to ordinary world
			// placement rather than drawing everything at the origin.
			objectToWorldMatrix.loadIdentity();
		}
	}

	// WHICH HAND, and the ACTOR gets the last word.
	//
	// The MODELDEF flag is per CLASS and says which controller this kind of
	// thing normally rides. AActor::FollowHandMode is per INSTANCE and says
	// which one it is riding right now, because that is a property of the
	// moment rather than of the class -- an off hand reaching for a slide has to
	// be drawn in the MAIN hand's frame, since that is the frame the gun and its
	// slide live in, while the player's real off hand stays somewhere it will
	// not knock against the other controller.
	//
	// 0 defers to the MODELDEF, so everything written before this existed keeps
	// exactly the behaviour it had. See AActor::FollowHandMode.
	int followHand = (flags & MDL_FOLLOWMAINHAND) ? VR_MAINHAND
		: ((flags & MDL_FOLLOWOFFHAND) ? VR_OFFHAND : -1);
	if (followHandMode == 1)      followHand = VR_MAINHAND;
	else if (followHandMode == 2) followHand = VR_OFFHAND;
	// [FIT] THE OFF HAND GETS THE MIRROR IMAGE OF A GUN'S SIDEWAYS OFFSET.
	//
	// Gun fit mode fits a gun once, in one hand, and the other hand is meant to
	// get the mirror image rather than being fitted again. A model that lets the
	// engine mirror it in the off hand gets that for free -- the whole hand frame
	// is mirrored, Offset included. A model marked NOAUTOREVERSE (every HacX gun)
	// is NOT mirrored, so its Offset was not either: the owner nudged the Cryogun
	// +x and it moved right in BOTH hands, which would have put a fitted grip on
	// the wrong side of the left hand. Only MODELDEF Offset's x is mirrored --
	// the placement sliders keep exactly the behaviour they had. Every gun's
	// sideways Offset was 0 when this went in, so nothing moved on the day.
	bool fitMirrorX = false;
	if (!followedActor && !followedBody && followHand >= 0)
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		bool handMirrored = false;	// AActor::FollowHandRot, below
		if (vrmode != nullptr && vrmode->IsVR() &&
			vrmode->GetWeaponTransform(&objectToWorldMatrix, followHand, !(flags & MDL_NOAUTOREVERSE), &handMirrored))
		{
			fitMirrorX = (followHand == VR_OFFHAND && !handMirrored);
			// THE MODEL-UNIT CONVERSION, WHICH THIS BRANCH WAS MISSING.
			//
			// GetWeaponTransform hands back a frame scaled by vr_vunits_per_meter
			// -- 34 map units to the metre -- and NOT in model units. The HUD path
			// converts immediately after calling it (RenderHUDModel, `float scale
			// = 0.01f`); this one never did. So the two branches disagreed by
			// exactly 100 for the same mesh at the same MODELDEF Scale.
			//
			// That is the whole "100x" class of bug in this fork, and it is not
			// occasional: EVERY world actor put on a controller takes this path --
			// a gun prop, a held magazine, a wireframe reticle -- so every one of
			// them came out a hundred times too large. A model at 100x fills the
			// room and one at 1/100 is a speck, and both read as "it did not
			// appear", which is indistinguishable from the actor never spawning.
			// Four separate scale values were dialled around this before anyone
			// compared the two branches.
			//
			// THE 0.01 ONLY. The HUD path also translates (0, 5, 30) afterwards,
			// which is where a VIEW model sits in front of an eye. A world actor
			// following a controller wants no such seat -- it places itself from
			// its own offsets further down.
			//
			// EVERY MODELDEF Scale ON THIS PATH MOVES BY 100 WITH THIS. They were
			// all tuned against the missing conversion; see the MODELDEF blocks in
			// RS_TestPistol, RS_WorldHands and RS_VRBody, corrected in the same
			// change.
			const float followHandUnitScale = 0.01f;
			objectToWorldMatrix.scale(followHandUnitScale, followHandUnitScale, followHandUnitScale);

			// AActor::FollowHandRot -- a turn of the hand's own frame, about the hand.
			//
			// Written directly after the frame is fetched, so it lands on the vertex
			// LAST (VSMatrix composes on the right; see step 5): after the model's
			// whole seat -- MODELDEF Offset, FollowHandOfs, the placement sliders, the
			// base orientation, the pivot -- and about the controller's origin. A gun
			// swings about the grip, and followOut (the frame FollowActor riders use)
			// is copied later from this same matrix, so they turn with it.
			//
			// THE AXES ARE THE HAND'S. GetHandTransform's frame is X right, Y up, Z
			// back (forward is -Z). Yaw is about Y, + turns left like Angle; pitch is
			// about X, + tips the muzzle down like Pitch; roll is about Z, + tips the
			// top to the right seen from behind. (The placement sliders, applied in
			// these same axes in step 5, keep their own labels; this does not copy
			// them.)
			//
			// THE MIRROR. On controller 0 with auto reverse GetWeaponTransform has
			// scaled X by -1, and a turn written inside that mirror is drawn as its
			// reflection: the turn about X keeps its sense, the turns about Y and Z
			// reverse. Negating those two conjugates the turn by the mirror, so it is
			// drawn as it would be in the unmirrored hand and the same numbers point
			// the model the same way in either hand. The sliders are left mirrored:
			// they are a seat the player tunes per hand.
			if (followHandRot.X != 0.0 || followHandRot.Y != 0.0 || followHandRot.Z != 0.0)
			{
				const float mirror = handMirrored ? -1.f : 1.f;
				objectToWorldMatrix.rotate((float)followHandRot.X * mirror, 0, 1, 0);
				objectToWorldMatrix.rotate(-(float)followHandRot.Y, 1, 0, 0);
				objectToWorldMatrix.rotate(-(float)followHandRot.Z * mirror, 0, 0, 1);
			}

			followedHand = true;
		}
		else
		{
			// Not in VR, or the pose is unavailable this frame. Fall back to the
			// ordinary world placement rather than drawing at the origin.
			objectToWorldMatrix.loadIdentity();
		}
	}

	if (followedBody)
	{
		// Placed in the body frame above; its own rotation still applies, and it
		// must NOT be translated into world space again.
	}
	else if (followedHand)
	{
		// The controller supplies orientation, so the actor's own Angles must not
		// be applied on top. Zeroing them here rather than branching around the
		// rotation block below leaves that block's structure untouched -- a
		// rotate() of zero degrees is a no-op -- so MDL_ROTATING and the rotation
		// -centre paths keep behaving exactly as they always have.
		rotation.Yaw = rotation.Pitch = rotation.Roll = DAngle::fromDeg(0.);
	}
	else if (followedActor)
	{
		// In the parent's frame already, seat included. Nothing to translate,
		// and this actor's Angles are relative to that frame.
	}
	else
	{
		// Model space => World space
		objectToWorldMatrix.translate(translation.X, translation.Z, translation.Y);
	}

	// consider the pixel stretching. For non-voxels this must be factored out here
	float stretch = 1.f;

	// [MK] distortions might happen depending on when the pixel stretch is compensated for
	// so we make the "undistorted" behavior opt-in
	if ((flags & MDL_CORRECTPIXELSTRETCH) && modelIDs.Size() > 0)
	{
		stretch = (modelIDs[0] >= 0 ? Models[modelIDs[0]]->getAspectFactor(Level->info->pixelstretch) : 1.f) / Level->info->pixelstretch;
		objectToWorldMatrix.scale(1, stretch, 1);
	}

	// Zero for everything that is not a held voxel, so the common path is one
	// compare and the rotate calls below fold away.
	// NEGATIVE MEANS DERIVE IT, and that is the default.
	//
	// Step 5 below spins the mesh by -angleoffset before any of this runs. A
	// pack that declares 90 therefore leaves the mesh's long axis on Z while
	// roll still turns about X -- a quarter turn out, which is a wrist roll
	// coming out as a fore/aft tilt. Undoing exactly the offset the pack
	// declared puts the body axes back where pitch and roll expect them, so the
	// right number is not a matter of taste and nobody should have to find it
	// by feel. Confirmed against the barrel: angleoffset=90, tilt on roll.
	//
	// The override stays because a pack whose voxels are authored nose-up
	// rather than nose-along could need something else, and there is no way to
	// tell that from the data either.
	float voxBodyYaw = 0.f;
	if (flags & MDL_VOXELBODYAXIS)
		// NEGATED, confirmed in a headset 2026-08-29: at +angleoffset the barrel
		// rolled the right way about the right axis and in the WRONG DIRECTION.
		//
		// Which is the expected result of getting the wrap backwards. The pair
		// below rotates the mesh into a frame, applies pitch and roll, and
		// rotates back out; sending +angleoffset winds it the same way step 5
		// already wound the mesh instead of unwinding it, so the body axes land
		// a quarter turn past where they started rather than back at it.
		voxBodyYaw = (vr_voxel_rollaxis < 0.f) ? -angleoffset : (float)vr_voxel_rollaxis;

	bool rotating_xzy = (flags & MDL_ROTATING) && (flags & MDL_FIXROTATING);
	bool rotating_xyz = (flags & MDL_ROTATING) && !(flags & MDL_FIXROTATING);

	// Applying model transformations:
	// 1) Applying actor angle, pitch and roll to the model
	if (flags & MDL_USEROTATIONCENTER)
	{
		objectToWorldMatrix.translate(rotationCenterX, rotationCenterZ/stretch, rotationCenterY);

		objectToWorldMatrix.rotate(-rotation.Yaw.Degrees(), 0, 1, 0);
		if (voxBodyYaw != 0.f) objectToWorldMatrix.rotate(-voxBodyYaw, 0, 1, 0);
		objectToWorldMatrix.rotate(rotation.Pitch.Degrees(), 0, 0, 1);
		objectToWorldMatrix.rotate(-rotation.Roll.Degrees(), 1, 0, 0);
		if (voxBodyYaw != 0.f) objectToWorldMatrix.rotate(voxBodyYaw, 0, 1, 0);

		// 2) Applying Doomsday like rotation of the weapon pickup models
		// The rotation angle is based on the elapsed time.
		if(rotating_xzy)
		{
			objectToWorldMatrix.rotate(rotateOffset, xrotate, yrotate, zrotate);
		}

		objectToWorldMatrix.translate(-rotationCenterX, -rotationCenterZ/stretch, -rotationCenterY);

		if(rotating_xyz)
		{
			objectToWorldMatrix.translate(rotationCenterX, rotationCenterY/stretch, rotationCenterZ);
			objectToWorldMatrix.rotate(rotateOffset, xrotate, yrotate, zrotate);
			objectToWorldMatrix.translate(-rotationCenterX, -rotationCenterY/stretch, -rotationCenterZ);
		}
	}
	else
	{
		// Same shape as the USEROTATIONCENTER branch above -- lift the pivot to
		// the origin, turn, put it back -- but the height comes from the ACTOR
		// rather than from a MODELDEF, because no voxel pack declares one and a
		// held object needs one regardless. Zero for everything else, and the
		// two translates fold away.
		if (bodyPivotZ != 0.f) objectToWorldMatrix.translate(0, bodyPivotZ / stretch, 0);

		objectToWorldMatrix.rotate(-rotation.Yaw.Degrees(), 0, 1, 0);
		if (voxBodyYaw != 0.f) objectToWorldMatrix.rotate(-voxBodyYaw, 0, 1, 0);
		objectToWorldMatrix.rotate(rotation.Pitch.Degrees(), 0, 0, 1);
		objectToWorldMatrix.rotate(-rotation.Roll.Degrees(), 1, 0, 0);
		if (voxBodyYaw != 0.f) objectToWorldMatrix.rotate(voxBodyYaw, 0, 1, 0);

		if (bodyPivotZ != 0.f) objectToWorldMatrix.translate(0, -bodyPivotZ / stretch, 0);

		// 2) Applying Doomsday like rotation of the weapon pickup models
		// The rotation angle is based on the elapsed time.
		if(rotating_xzy)
		{
			objectToWorldMatrix.translate(rotationCenterX, rotationCenterZ/stretch, rotationCenterY);
			objectToWorldMatrix.rotate(rotateOffset, xrotate, yrotate, zrotate);
			objectToWorldMatrix.translate(-rotationCenterX, -rotationCenterZ/stretch, -rotationCenterY);
		}
		else if(rotating_xyz)
		{
			objectToWorldMatrix.translate(rotationCenterX, rotationCenterY/stretch, rotationCenterZ);
			objectToWorldMatrix.rotate(rotateOffset, xrotate, yrotate, zrotate);
			objectToWorldMatrix.translate(-rotationCenterX, -rotationCenterY/stretch, -rotationCenterZ);
		}
	}

	// PlacementCVars on the WORLD path.
	//
	// This used to exist only in RenderHUDModel, which made the feature exactly
	// backwards: a physically held gun IS a world actor, so the one case that
	// most needs live tuning was the one case the sliders could not reach, and
	// moving them did nothing at all with nothing in the log to say why.
	// Commit 026d2a8a80 fixed that and the wholesale revert took it back out.
	//
	// Summed into the SAME translate and rotate calls as the MODELDEF values,
	// never applied afterwards. Rotations do not commute: a yaw applied after the
	// model's own pitch and roll turns about an already-rotated axis and is NOT
	// the same number added to angleoffset. Because they fold in here, a value
	// found by eye transfers into the MODELDEF verbatim and the slider returns to
	// zero with nothing moving.
	float wPlaceOfs[3] = { 0.0f, 0.0f, 0.0f };
	float wPlaceRot[3] = { 0.0f, 0.0f, 0.0f };
	float wPlaceScale = 1.0f;
	// PER-AXIS scale, on top of the uniform one. Needed by anything whose three
	// dimensions are genuinely different numbers -- a drawn collision box, most
	// obviously, whose whole value is being the same three numbers the solver
	// was handed. Axes are stated in ACTOR terms, matching _ofs_x/_y/_z:
	// x = forward, y = sideways, z = up.
	float wPlaceAxis[3] = { 1.0f, 1.0f, 1.0f };
	// WHOSE SLIDERS. The MODELDEF names a prefix for the CLASS; an actor may
	// name a different one for the MOMENT -- see AActor::PlacementPrefix. The
	// actor wins when it has one, which is what lets a hand pinned to a slide be
	// tuned on the pistol's own page, live, while the menu is open.
	//
	// Read here rather than anywhere in script on purpose: this runs on the
	// RENDER path, every frame it draws, and the render path does not stop for a
	// menu. That is the entire reason this is the only placement channel whose
	// sliders move a model while you are looking at them.
	// A PREFIX THAT NAMES NOTHING FALLS BACK. IT DOES NOT BLANK THE MODEL OUT.
	//
	// placementPrefix is set from script, and script has exactly one way to say
	// "go back to normal": clear the field. In ZScript the obvious spelling of
	// that is `a.PlacementPrefix = 'None'` -- which does NOT produce NAME_None,
	// it produces the literal name "None". The renderer then looked up
	// None_ofs_x, None_yaw and so on, found nothing, and every placement value
	// silently read as zero.
	//
	// The result: one grab of the pistol's slide permanently killed the world
	// hand's own placement sliders, because the hand never got its prefix back.
	// Nothing in the log, nothing at load -- the sliders simply stopped moving
	// the hand.
	//
	// So "None" is treated as no prefix, and the MODELDEF's own is used. Costs
	// one comparison and removes a whole class of silent failure: a mod that
	// sets a prefix it later mis-spells gets its own sliders back rather than
	// a model that cannot be placed at all.
	FName activePrefix = placementCVars;
	if (placementPrefix != NAME_None && stricmp(placementPrefix.GetChars(), "None") != 0)
		activePrefix = placementPrefix;
	if (activePrefix != NAME_None)
	{
		static const char *sufOfs[3] = { "_ofs_x", "_ofs_y", "_ofs_z" };
		static const char *sufRot[3] = { "_yaw", "_pitch", "_roll" };
		FString nm;
		for (int i = 0; i < 3; ++i)
		{
			nm.Format("%s%s", activePrefix.GetChars(), sufOfs[i]);
			GetPlacementCVar(nm.GetChars(), wPlaceOfs[i]);
			nm.Format("%s%s", activePrefix.GetChars(), sufRot[i]);
			GetPlacementCVar(nm.GetChars(), wPlaceRot[i]);
		}
		if (vr_place_debug)
		{
			// WHAT THE RENDERER READ, per prefix, on change only.
			//
			// Five readings of the MODELDEF, the cvar declarations and the menu
			// wiring all said the hands were wired correctly, and all five were
			// right. Reading the source answers "is it wired". Only the renderer
			// can answer "is this the entry I am drawing from, and are these the
			// numbers I am using" -- and those kept being mistaken for each other.
			static FName lastPrefix = NAME_None;
			static int   lastKey = -0x7fffffff;
			const int key = int(wPlaceOfs[0] * 1000) ^ int(wPlaceOfs[1] * 977)
			              ^ int(wPlaceOfs[2] * 953) ^ int(wPlaceRot[0] * 31);
			if (activePrefix != lastPrefix || key != lastKey)
			{
				lastPrefix = activePrefix; lastKey = key;
				Printf("[VRPLACE] %s  ofs=(%.3f %.3f %.3f)  rot=(%.0f %.0f %.0f)  xscale=%.4f  applied=(%.3f %.3f %.3f)\n",
					activePrefix.GetChars(),
					wPlaceOfs[0], wPlaceOfs[1], wPlaceOfs[2],
					wPlaceRot[0], wPlaceRot[1], wPlaceRot[2],
					xscale,
					xscale != 0.f ? wPlaceOfs[0] / xscale : 0.f,
					zscale != 0.f ? wPlaceOfs[2] / zscale : 0.f,
					yscale != 0.f ? wPlaceOfs[1] / yscale : 0.f);
			}
		}

		// Defaults to 1, NOT the 0 an absent cvar reads as -- a missing slider
		// must leave the model alone, not collapse it to a point.
		nm.Format("%s_scale", activePrefix.GetChars());
		{
			float sc = 0.0f;
			if (GetPlacementCVar(nm.GetChars(), sc) && sc > 0.0f) wPlaceScale = sc;
		}
		static const char *sufAxis[3] = { "_scale_x", "_scale_y", "_scale_z" };
		for (int i = 0; i < 3; ++i)
		{
			nm.Format("%s%s", activePrefix.GetChars(), sufAxis[i]);
			{
				float sc = 0.0f;
				if (GetPlacementCVar(nm.GetChars(), sc) && sc > 0.0f) wPlaceAxis[i] = sc;
			}
		}
	}

	// THE FRAME A FOLLOWER RIDES (AActor::FollowActor), if one asked: this
	// model's frame and own rotation so far, plus its placement rotation --
	// written in the same order and sense as step 5 below. Taken BEFORE the
	// scale and without the MODELDEF base orientation, on purpose: see
	// ModelFollowFrame. Its origin is filled in at the very end, once the whole
	// matrix says where this model is actually drawn.
	VSMatrix followOut;
	if (followFrameOut != nullptr)
	{
		followOut = objectToWorldMatrix;
		followOut.rotate(-wPlaceRot[0], 0, 1, 0);
		followOut.rotate(wPlaceRot[1],  0, 0, 1);
		followOut.rotate(-wPlaceRot[2], 1, 0, 0);
	}

	// 3) Scaling model.
	// AActor::ScaleAxes rides with the placement set's own per-axis scale: same
	// axes, same meaning, one from a slider and one from the actor. Zero or less
	// on an axis means "leave it", so an unset actor multiplies by one.
	const float axX = scaleAxes.X > 0.0 ? (float)scaleAxes.X : 1.f;
	const float axY = scaleAxes.Y > 0.0 ? (float)scaleAxes.Y : 1.f;
	const float axZ = scaleAxes.Z > 0.0 ? (float)scaleAxes.Z : 1.f;
	objectToWorldMatrix.scale(scaleFactorX * wPlaceScale * wPlaceAxis[0] * axX,
		scaleFactorZ * wPlaceScale * wPlaceAxis[2] * axZ,
		scaleFactorY * wPlaceScale * wPlaceAxis[1] * axY);

	// 4) Aplying model offsets (model offsets do not depend on model scalings).
	//
	// AActor::FollowHandOfs RIDES ALONG WITH THE OTHER TWO, and that placement is
	// the whole point of it. MODELDEF's own Offset and the placement cvars are
	// already summed here; adding the per-actor seat to the same sum means it
	// carries the same units, the same axis order and the same division by scale
	// as the sliders a human tunes by hand. There is no second convention to
	// learn and no conversion to get wrong -- whatever number moves a slider one
	// unit moves this field one unit.
	//
	// Zero by default, so every actor that never sets it lands on exactly the
	// arithmetic that was here before.
	objectToWorldMatrix.translate(((fitMirrorX ? -xoffset : xoffset) + wPlaceOfs[0] + (float)followHandOfs.X) / xscale,
		(zoffset + wPlaceOfs[2] + (float)followHandOfs.Z) / (zscale*stretch),
		(yoffset + wPlaceOfs[1] + (float)followHandOfs.Y) / yscale);

	// 5) Applying model rotations.
	// THE MESH'S BASE ORIENTATION AND THE MOD'S SLIDERS ARE SEPARATE ROTATIONS.
	//
	// They used to be SUMMED into these same three. That works until a mesh needs
	// a base pitch of 90 to sit right -- and then the mod's sliders start life
	// AT THE EULER SINGULARITY, where the yaw and roll axes point the same way
	// and moving either one does the same thing. Two of the three sliders become
	// one, and no amount of tuning gets the third axis back.
	//
	// Applied in sequence instead, the base puts the mesh where the artist meant
	// and the placement rotations then act in that already-oriented frame,
	// starting from zero. All three stay independent wherever the base happens
	// to be.
	//
	// This DOES change the meaning of an existing non-zero placement rotation
	// paired with a non-zero MODELDEF offset -- summing and sequencing agree only
	// while one of them is zero. Nothing shipped in this tree had both until now,
	// and the previous behaviour made the sliders unusable on exactly the models
	// that needed them most.
	// ORDER IS REVERSED FROM WHAT IT LOOKS LIKE. VSMatrix::multMatrix computes
	// res = mMatrix * aMatrix (matrix.cpp:92), so rotate() composes on the RIGHT
	// and the LAST call written is the FIRST one applied to a vertex.
	//
	// That is why these two blocks are in this order and not the other one. The
	// intent -- stated at length above and, until 2026-09-08, not what the code
	// did -- is that the MODELDEF base orientation puts the mesh where the artist
	// meant, and the mod's placement sliders then act in that already-oriented
	// frame. To get that on the vertex, the placement rotations must be written
	// FIRST so they are applied LAST.
	//
	// Written the other way round, the sliders turned the RAW mesh and the base's
	// 90 degree pitch and -90 roll were then layered on top of all three. On a
	// model with a large base orientation -- every hand in this fork -- that maps
	// three separate slider axes through one shared rotation, and they stop
	// reading as three separate controls. Reported from a headset as "roll yaw
	// pitch all do the same thing".
	objectToWorldMatrix.rotate(-wPlaceRot[0], 0, 1, 0);
	objectToWorldMatrix.rotate(wPlaceRot[1],  0, 0, 1);
	objectToWorldMatrix.rotate(-wPlaceRot[2], 1, 0, 0);

	objectToWorldMatrix.rotate(-angleoffset, 0, 1, 0);
	objectToWorldMatrix.rotate(pitchoffset,  0, 0, 1);
	objectToWorldMatrix.rotate(-rolloffset,  1, 0, 0);

	// 6) The pivot: the point the model turns about, in its own space.
	//
	// VSMatrix post-multiplies, so the LAST operation written is the FIRST one
	// applied to the vertex. Written here, after step 5, it therefore lands on
	// the vertex BEFORE the rotations -- which is the whole feature. Move these
	// three lines above the rotations and they silently become another Offset:
	// still compiling, still looking plausible, and no longer doing anything.
	//
	// Same axis order and the same division by scale as step 4, so a pivot and
	// an offset are stated in the same units and can be read against each other.
	//
	// The compare is not an optimisation. The common case is a model with no
	// pivot at all, and three float compares are cheaper than a matrix multiply
	// on every drawn model in the level.
	if (pivotx != 0.f || pivoty != 0.f || pivotz != 0.f)
	{
		objectToWorldMatrix.translate(-pivotx / xscale,
			-pivotz / (zscale * stretch),
			-pivoty / yscale);
	}

	if (!(flags & MDL_CORRECTPIXELSTRETCH) && !(flags & MDL_NOPIXELSTRETCH) && modelIDs.Size() > 0)	// NoPixelStretch (GZSelaco 1e7fd30b59)
	{
		stretch = (modelIDs[0] >= 0 ? Models[modelIDs[0]]->getAspectFactor(Level->info->pixelstretch) : 1.f) / Level->info->pixelstretch;
		objectToWorldMatrix.scale(1, stretch, 1);
	}

	if (followFrameOut != nullptr)
	{
		// At the model's DRAWN origin: where the whole matrix puts (0,0,0) --
		// the same point ModelPointToWorld(0,0,0) answers.
		FLOATTYPE m[16];
		followOut.copy(m);
		const FLOATTYPE *full = objectToWorldMatrix.get();
		m[12] = full[12];
		m[13] = full[13];
		m[14] = full[14];
		followOut.loadMatrix(m);
		*followFrameOut = followOut;
	}

	return objectToWorldMatrix;
}

void RenderHUDModel(FModelRenderer *renderer, DPSprite *psp, FVector3 translation, FVector3 rotation, FVector3 rotation_pivot, FSpriteModelFrame *smf, double ticFrac)
{
	AActor * playermo = players[consoleplayer].camera;

	int smf_flags = smf->getFlags(psp->Caller->modelData);

	// [BB] No model found for this sprite, so we can't render anything.
	if (smf == nullptr)
		return;

	// The model position and orientation has to be drawn independently from the position of the player,
	// but we need to position it correctly in the world for light to work properly.
	VSMatrix objectToWorldMatrix = renderer->GetViewToWorldMatrix();

	// [BB] Which controller this psprite rides on.
	//
	// The caller test comes first and is the original one: both hands' muzzle
	// flashes share PSP_FLASH, so a flash layer's ID says nothing about its
	// side and only the caller identifies it.
	//
	// The ID test is the addition. Hand selection used to be derived purely
	// from the player's weapon slots, so any psprite whose caller was not
	// literally player->OffhandWeapon -- i.e. anything that is not a weapon,
	// such as a hand model -- silently fell through to the mainhand pose, with
	// no way to say otherwise from script. Layers at or above
	// PSP_OFFHANDWEAPON now name the offhand explicitly. That range includes
	// PSP_OFFHANDWEAPON itself, so this also keeps the offhand weapon on the
	// correct hand during the frames where its slot is momentarily null.
	int hand = (psp->GetCaller() == playermo->player->OffhandWeapon
		|| psp->GetID() >= PSP_OFFHANDWEAPON) ? 1 : 0;
	auto vrmode = VRMode::GetVRModeCached(true);

	// MDL_NOAUTOREVERSE: the model supplies its own left and right variants, so
	// the non-dominant-hand mirror would flip an already-correct mesh.
	// The HUD-model unit conversion, remembered rather than only applied.
	//
	// This 0.01 is not part of positioning the model at the controller -- it is
	// the conversion from the model's own units into the units the rest of this
	// function works in, and EVERY hud model needs it, anchored or not. The
	// anchoring block below replaces the whole matrix (loadMatrix), which
	// silently discarded it and drew anchored models at 100x size.
	//
	// It went unnoticed on weapons because a weapon cancels it: a weapon mesh
	// carrying MODELDEF Scale 100 gives 0.01 * 100 = 1. VR hands carry Scale 1.0, so
	// they have nothing to cancel with and take the full factor of 100 -- which
	// is precisely the "absolutely massive" hands, and why only the ANCHORED
	// ones were affected while a hand holding a magazine (deliberately
	// unanchored by the hands mod) stayed correct.
	// RS FORK -- MOD-OWNED PLACEMENT, read live from CVARs the MOD declares.
	//
	// Looked up by name every frame rather than resolved once at parse time,
	// because MODELDEF is parsed before a mod's CVARINFO is guaranteed to have
	// run, and a cached null would be permanent. Six hash lookups for a handful
	// of drawn models is not worth optimising away.
	float placeOfs[3] = { 0.0f, 0.0f, 0.0f };
	float placeRot[3] = { 0.0f, 0.0f, 0.0f };
	float placeScale = 1.0f;
	// Per-axis, same as the world path -- kept in step so a prefix behaves the
	// same whichever path draws it. A model tuned on one and moved to the other
	// silently losing an axis is the kind of asymmetry that costs a session.
	float placeAxis[3] = { 1.0f, 1.0f, 1.0f };
	if (smf->placementCVars != NAME_None)
	{
		static const char *sufOfs[3] = { "_ofs_x", "_ofs_y", "_ofs_z" };
		static const char *sufRot[3] = { "_yaw", "_pitch", "_roll" };
		FString nm;
		for (int i = 0; i < 3; ++i)
		{
			nm.Format("%s%s", smf->placementCVars.GetChars(), sufOfs[i]);
			GetPlacementCVar(nm.GetChars(), placeOfs[i]);
			nm.Format("%s%s", smf->placementCVars.GetChars(), sufRot[i]);
			GetPlacementCVar(nm.GetChars(), placeRot[i]);
		}

		// Scale defaults to 1, NOT to the 0 an absent CVAR would read as -- a
		// missing or zeroed slider must leave the model alone, not collapse it
		// to a point.
		nm.Format("%s_scale", smf->placementCVars.GetChars());
		{
			float sv = 0.0f;
			if (GetPlacementCVar(nm.GetChars(), sv) && sv > 0.0f) placeScale = sv;
		}
		static const char *sufAxis[3] = { "_scale_x", "_scale_y", "_scale_z" };
		for (int i = 0; i < 3; ++i)
		{
			nm.Format("%s%s", smf->placementCVars.GetChars(), sufAxis[i]);
			{
				float v = 0.0f;
				if (GetPlacementCVar(nm.GetChars(), v) && v > 0.0f) placeAxis[i] = v;
			}
		}
	}

	float hudUnitScale = 1.0f;
	const bool followBody = !!(smf_flags & MDL_FOLLOWBODY);
	// THE SEAT, WITH THE LIVE SLIDERS FOLDED IN.
	//
	// placeOfs is read from the mod's own CVARs every frame the model is drawn,
	// so folding it into the body seat here is what makes a slider move a worn
	// thing WHILE THE MENU IS OPEN -- exactly what UseHandOffsets does for a
	// hand, and the reason RS_Hands' sliders have always moved live.
	//
	// Script cannot do this. A mod writes BodyOfs from its playsim tic, and the
	// playsim does not tick while a menu is up, so a script-driven seat is frozen
	// for as long as you are looking at the slider you are dragging.
	//
	// Added in the BODY's axes (X forward, Y right, Z up) because that is what
	// the seat is, and zeroed afterwards so the model-space application further
	// down does not apply it a second time.
	DVector3 bodySeat = psp->BodyOfs;
	if (followBody)
	{
		bodySeat.X += placeOfs[0];
		bodySeat.Y += placeOfs[1];
		bodySeat.Z += placeOfs[2];
		placeOfs[0] = placeOfs[1] = placeOfs[2] = 0.0f;
	}

	if (followBody && vrmode->GetHmdTransform(&objectToWorldMatrix, bodySeat, nullptr, psp->BodyYaw))
	{
		// GetHmdTransform already returns a MAP-UNIT frame and has applied the
		// seat itself, so this takes NEITHER the 0.01 nor the translate(0,5,30)
		// the controller branch below needs. Those two exist to convert and
		// place a hand-frame model; a body-frame one arrives already converted
		// and already placed.
		hudUnitScale = 1.0f;
	}
	else if (!followBody && vrmode->GetWeaponTransform(&objectToWorldMatrix, hand, !(smf_flags & MDL_NOAUTOREVERSE)))
	{
		float scale = 0.01f;
		objectToWorldMatrix.scale(scale, scale, scale);
		objectToWorldMatrix.translate(0, 5, 30);
		hudUnitScale = scale;
	}
	else if (vrmode->IsVR())
	{
		DVector3 pos = playermo->Pos();
		objectToWorldMatrix.translate(pos.X, pos.Z + 40, pos.Y);
		objectToWorldMatrix.rotate(-playermo->Angles.Yaw.Degrees() - 90, 0, 1, 0);
	}

	float fovscale = 1.0f;
	if (smf->viewModelFOV <= 0.0f)
	{
		if (smf->viewModelFOV < 0.0f)
			fovscale = 1.0f / fabs(smf->viewModelFOV);

		// [Nash] Optional scale weapon FOV
		if (smf_flags & MDL_SCALEWEAPONFOV)
		{
			float newScale = tan(players[consoleplayer].DesiredFOV * (0.5f * M_PI / 180.f));
			newScale = 1.f + (newScale - 1.f) * cl_scaleweaponfov;
			fovscale *= newScale;
		}
	}
	else if (players[consoleplayer].DesiredFOV != smf->viewModelFOV)
	{
		fovscale = tan(players[consoleplayer].DesiredFOV * (0.5f * M_PI / 180.f)) / tan(smf->viewModelFOV * (0.5f * M_PI / 180.f));
	}

	// [BB] The psprite's own scale reaches the MODEL path, not just the sprite
	// one.
	//
	// psp->scale was read only by the 2D weapon-sprite code, so a mod that
	// shrank a psprite saw nothing happen to a weapon drawn as a model -- and
	// there was no other way to resize one from script at all.
	//
	// It defaults to (0,0) and the sprite path already treats that as "unset"
	// (hw_weapon.cpp tests scale.isZero()), so this is a free channel: zero
	// leaves every existing model exactly as it was, and no content that does
	// not opt in can be affected.
	//
	// X alone, applied uniformly. A model scaled unevenly on two axes shears
	// rather than resizes, and "half size" is one number in every caller's head.
	float pspScale = 1.0f;
	if (!psp->scale.isZero()) pspScale = (float)psp->scale.X;

	// Scaling model (y scale for a sprite means height, i.e. z in the world!).
	objectToWorldMatrix.scale(smf->xscale * pspScale, smf->zscale * pspScale, (smf->yscale / fovscale) * pspScale);

	// Aplying model offsets (model offsets do not depend on model scalings).
	//
	// MDL_USEHANDOFFSETS adds the live vr_hand_ofs_* CVARs into the same
	// translate rather than a second one. Summing inside the single call is what
	// makes a CVAR value and a MODELDEF value genuinely interchangeable, so a
	// number found on a slider can be folded into MODELDEF and the slider zeroed
	// with nothing moving.
	// RS FORK -- HUD BONE ANCHORING, applied.
	//
	// Everything above positioned this model at a controller. If it is anchored
	// to a bone instead, that work is discarded here and the bone's transform
	// becomes the base. Deliberately placed AFTER the controller maths rather
	// than replacing it: the offsets, rotations and scale below then apply
	// relative to the bone, so MODELDEF still fine-tunes the fit exactly as it
	// does for an unanchored model, and one code path serves both.
	bool isAnchored = false;
	if (psp->AnchorLayer >= 0 && psp->AnchorBone != NAME_None)
	{
		VSMatrix anchored;
		if (HudAnchor_Get(psp->AnchorLayer, psp->AnchorBone, anchored))
		{
			// Position and orientation only -- NEVER scale.
			//
			// A bone matrix carries the entire chain that produced it, and that
			// chain includes whatever scale the target model was authored at.
			// Adopting it wholesale multiplies THIS model by the other one's
			// size, which is never what anchoring means: a magazine placed in a
			// hand should be magazine-sized, not hand-times-magazine sized.
			//
			// It bites hard because the numbers involved are not small. The hand
			// rig carries a 0.01 at its root joint, so a magazine anchored to a
			// knuckle came out a hundredth of its size -- far past what any
			// scale slider could climb back out of, and looking for all the
			// world like a model exported wrong.
			//
			// So the basis is orthonormalised: each of the three axes is scaled
			// back to unit length, which strips scale while leaving rotation and
			// translation exactly as they were.
			FLOATTYPE m[16];
			memcpy(m, anchored.get(), sizeof(m));
			for (int c = 0; c < 3; ++c)
			{
				FLOATTYPE *col = &m[c * 4];
				FLOATTYPE len = (FLOATTYPE)sqrt(col[0]*col[0] + col[1]*col[1] + col[2]*col[2]);
				if (len > (FLOATTYPE)1e-8)
				{
					col[0] /= len; col[1] /= len; col[2] /= len;
				}
			}
			objectToWorldMatrix.loadMatrix(m);

			// AND PUT THIS MODEL'S OWN SCALE BACK.
			//
			// loadMatrix REPLACES the matrix outright, which throws away the
			// MODELDEF scale applied further up (the `objectToWorldMatrix.scale`
			// on smf->xscale/zscale/yscale). Anchoring is only supposed to
			// discard the target-relative POSITIONING done above it -- not the
			// model's own size.
			//
			// It is not only magnitude. The VR hands are ONE mesh mirrored by a
			// negative X scale (RS_HandIdleMain carries `Scale -1.0 1.0 1.0`),
			// so dropping this silently un-mirrors the main hand: an anchored
			// hand came out the wrong way round as well as the wrong size.
			//
			// Deliberately the identical expression used above, not a
			// recalculation -- the two must not be able to drift apart.
			objectToWorldMatrix.scale(smf->xscale * pspScale, smf->zscale * pspScale, (smf->yscale / fovscale) * pspScale);

			// ...and the hud-model unit conversion the controller branch
			// applied, which loadMatrix above also threw away. See the note
			// where hudUnitScale is set: without this an anchored model is
			// drawn 100x too large.
			objectToWorldMatrix.scale(hudUnitScale, hudUnitScale, hudUnitScale);

			isAnchored = true;
		}
	}

	const bool useHandOfs = !!(smf_flags & MDL_USEHANDOFFSETS);
	// Which set of sliders this model listens to. The two hands are one mesh
	// mirrored by a negative X scale, so a shared value pushes them in opposite
	// directions and no single number can place both -- they each need their own.
	// hand was resolved above from the psprite, so it is already known here.
	const bool isOffhand = (hand == 1);
	const float handOfsX = useHandOfs ? (float)(isOffhand ? vr_offhand_ofs_x : vr_hand_ofs_x) : 0.0f;
	const float handOfsY = useHandOfs ? (float)(isOffhand ? vr_offhand_ofs_y : vr_hand_ofs_y) : 0.0f;
	const float handOfsZ = useHandOfs ? (float)(isOffhand ? vr_offhand_ofs_z : vr_hand_ofs_z) : 0.0f;

	// Seat offset. Only meaningful for an anchored layer -- for anything else
	// the bone frame it is expressed in does not exist -- and summed in here
	// rather than applied separately, for the same non-commuting reason the
	// rotation block below spells out.
	const float seatX = isAnchored ? (float)psp->AnchorOfs.X : 0.0f;
	const float seatY = isAnchored ? (float)psp->AnchorOfs.Y : 0.0f;
	const float seatZ = isAnchored ? (float)psp->AnchorOfs.Z : 0.0f;

	objectToWorldMatrix.translate((smf->xoffset + handOfsX + placeOfs[0] + seatX) / smf->xscale,
		(smf->zoffset + handOfsZ + placeOfs[2] + seatZ) / smf->zscale,
		(smf->yoffset + handOfsY + placeOfs[1] + seatY) / smf->yscale);

	// Everything in this block places the model relative to the PLAYER: the
	// global weapon offset, the bob, the aim rotation, the viewmodel axis fix.
	//
	// An anchored model must skip all of it. The bone matrix it started from was
	// captured after its target had already been through these same steps, so
	// applying them again would add the weapon's position and rotation a second
	// time and throw the attachment well clear of the bone it is meant to sit on.
	// What survives below is only the model's OWN offsets, rotations and scale,
	// which is exactly what should still fine-tune the fit.
	if (!isAnchored)
	{
		// Applying player custom offsets
		objectToWorldMatrix.translate(-vr_3dweaponOffsetX, vr_3dweaponOffsetY, vr_3dweaponOffsetZ);

		// [BB] Weapon bob, very similar to the normal Doom weapon bob.
		objectToWorldMatrix.translate(rotation_pivot.X, rotation_pivot.Y, rotation_pivot.Z);

		objectToWorldMatrix.rotate(rotation.X, 0, 1, 0);
		objectToWorldMatrix.rotate(rotation.Y, 1, 0, 0);
		objectToWorldMatrix.rotate(rotation.Z, 0, 0, 1);

		objectToWorldMatrix.translate(-rotation_pivot.X, -rotation_pivot.Y, -rotation_pivot.Z);

		objectToWorldMatrix.translate(translation.X, translation.Y, translation.Z);

		// [BB] For some reason the jDoom models need to be rotated.
		objectToWorldMatrix.rotate(90.f, 0, 1, 0);
	}

	// Applying angleoffset, pitchoffset, rolloffset.
	const float handYaw   = useHandOfs ? (float)(isOffhand ? vr_offhand_yaw   : vr_hand_yaw)   : 0.0f;
	const float handPitch = useHandOfs ? (float)(isOffhand ? vr_offhand_pitch : vr_hand_pitch) : 0.0f;
	const float handRoll  = useHandOfs ? (float)(isOffhand ? vr_offhand_roll  : vr_hand_roll)  : 0.0f;

	// Summed into the same three calls: rotations do not commute, so a seat
	// angle only equals a MODELDEF value if it is added here rather than
	// applied afterwards. These are solved against a bone's own orientation,
	// so they belong in the same frame the MODELDEF offsets establish.
	const float seatYaw   = isAnchored ? (float)psp->AnchorAngles.X : 0.0f;
	const float seatPitch = isAnchored ? (float)psp->AnchorAngles.Y : 0.0f;
	const float seatRoll  = isAnchored ? (float)psp->AnchorAngles.Z : 0.0f;

	// THE MESH'S BASE ORIENTATION AND THE MOD'S SLIDERS ARE SEPARATE ROTATIONS.
	//
	// They used to be SUMMED into these same three. That works until a mesh needs
	// a base pitch of 90 to sit right -- and then the mod's sliders start life
	// AT THE EULER SINGULARITY, where the yaw and roll axes point the same way
	// and moving either one does the same thing. Two of the three sliders become
	// one, and no amount of tuning gets the third axis back.
	//
	// Applied in sequence instead, the base puts the mesh where the artist meant
	// and the placement rotations then act in that already-oriented frame,
	// starting from zero. All three stay independent wherever the base happens
	// to be.
	//
	// This DOES change the meaning of an existing non-zero placement rotation
	// paired with a non-zero MODELDEF offset -- summing and sequencing agree only
	// while one of them is zero. Nothing shipped in this tree had both until now,
	// and the previous behaviour made the sliders unusable on exactly the models
	// that needed them most.
	objectToWorldMatrix.rotate(-(smf->angleoffset + seatYaw),  0, 1, 0);
	objectToWorldMatrix.rotate(smf->pitchoffset + seatPitch,   0, 0, 1);
	objectToWorldMatrix.rotate(-(smf->rolloffset + seatRoll),  1, 0, 0);

	objectToWorldMatrix.rotate(-placeRot[0], 0, 1, 0);
	objectToWorldMatrix.rotate(placeRot[1],  0, 0, 1);
	objectToWorldMatrix.rotate(-placeRot[2], 1, 0, 0);

	// The pivot, on the HUD path too -- see the field note in model.h.
	//
	// Present on BOTH paths deliberately. A model tuned on one and moved to the
	// other silently losing a correction is the worst kind of asymmetry, and a
	// pivot is the worst one to lose: being wrong shows up as a wobble while
	// turning rather than as a static misplacement anyone would spot at once.
	//
	// No division by scale here, matching the offset call on this path above.
	if (smf->pivotx != 0.f || smf->pivoty != 0.f || smf->pivotz != 0.f)
	{
		objectToWorldMatrix.translate(-smf->pivotx, -smf->pivotz, -smf->pivoty);
	}

	// THE HAND SLIDERS ARE APPLIED HERE, IN THE MODEL'S OWN ORIENTED FRAME,
	// and NOT summed into the three calls above. They used to be summed, on
	// the reasoning that a slider value should mean the same thing as the
	// MODELDEF keyword of the same name. That equivalence is real, but it is
	// what broke the sliders outright on any model carrying a 90 degree
	// PitchOffset -- which the VR hands do.
	//
	// These rotations are intrinsic: each turns about the axis the previous
	// ones left behind. Summing put the model's baked pitch BETWEEN the yaw
	// and the roll, and a 90 degree turn about Z maps the X axis onto Y -- so
	// by the time roll was applied, its axis had been rotated onto the exact
	// axis yaw had already used. Two sliders, one axis: moving either one
	// rolled the hand, and nothing at all turned it. Textbook gimbal lock,
	// and not a fault in either slider.
	//
	// Applied after the model is oriented, the three are orthogonal again in
	// the frame the player actually sees: roll turns the hand about its own
	// long axis -- a cylinder roll, which is what the word means -- and yaw
	// turns it about an axis genuinely across that, because the baked pitch is
	// now behind all three rather than in the middle of them.
	//
	// THE EQUIVALENCE IS NOT LOST, it moved. HandAngleOffset/HandPitchOffset/
	// HandRollOffset are summed in right here, in this frame and this order,
	// so a value dialled in on a slider can be written into the matching
	// MODELDEF keyword and mean EXACTLY the same thing. That is what makes a
	// tuning pass permanent instead of something every user has to redo --
	// dial it in live, then bake it, and the slider goes back to zero having
	// changed nothing.
	//
	// What is NOT interchangeable is these three against angleoffset/
	// pitchoffset/rolloffset, and that is the entire reason they are separate
	// keywords rather than more of the same: those orient the model, these
	// orient the hand holding it, and folding one into the other is what put
	// a baked 90 degree pitch between the yaw and the roll in the first place.
	objectToWorldMatrix.rotate(-(smf->handangleoffset + handYaw), 0, 1, 0);
	objectToWorldMatrix.rotate(smf->handpitchoffset + handPitch, 0, 0, 1);
	objectToWorldMatrix.rotate(-(smf->handrolloffset + handRoll), 1, 0, 0);

	//Scale weapon
	// placeScale is the mod's own live slider, multiplied onto the global one so
	// a per-weapon size can be found without disturbing every other weapon.
	objectToWorldMatrix.scale(vr_weaponScale * placeScale * placeAxis[0],
		vr_weaponScale * placeScale * placeAxis[2],
		vr_weaponScale * placeScale * placeAxis[1]);

	float orientation = smf->xscale * smf->yscale * smf->zscale;

	// Where this layer actually ended up, in numbers.
	//
	// "Tiny and far away" and "correctly sized but mispositioned" look identical
	// through a headset and are entirely different bugs. The transform says
	// which: position is the last column, scale is the length of the first.
	// Resolved defensively: a frame's model id is -1 when it has no model, and
	// Models[-1] is an out-of-bounds read -- a silent crash at the first weapon
	// draw, which is to say the instant a map starts.
	FModel *validateModel = nullptr;
	if (smf->modelIDs.Size() > 0)
	{
		const int vid = smf->modelIDs[0];
		if (vid >= 0 && vid < Models.SSize()) validateModel = Models[vid];
	}
	ValidateHudModel(smf, validateModel, psp, smf_flags);

	if (vr_spatialreport && psp)
	{
		// PER LAYER, not one shared timer.
		//
		// This used to be a single `static uint64_t lastReport`, which made the
		// report structurally unable to say anything about most of the scene:
		// psprites are drawn in ascending layer order, so the WEAPON (layer 1)
		// consumed the once-a-second slot every time and no other layer was
		// ever printed. Hands (900000/1900000) never appeared at all, and their
		// absence read as "not being drawn" when it only meant "never got the
		// slot" -- the exact wrong conclusion to hand someone debugging a
		// missing model.
		static TMap<int, uint64_t> lastReportByLayer;
		const int reportLayer = psp->GetID();
		uint64_t *slot = lastReportByLayer.CheckKey(reportLayer);
		const uint64_t last = slot ? *slot : 0;
		if (screen && (screen->FrameTime - last) > 1000)
		{
			lastReportByLayer.Insert(reportLayer, screen->FrameTime);
			const FLOATTYPE *m = objectToWorldMatrix.get();
			float sx = (float)sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
			Printf("[SPATIAL] layer %-8d %-22s pos(%.1f %.1f %.1f) scale %.3f frame %d %s\n",
				psp->GetID(),
				(psp->Caller != nullptr) ? psp->Caller->GetClass()->TypeName.GetChars() : "unknown",
				(float)m[12], (float)m[13], (float)m[14], sx,
				psp->ModelFrame,
				isAnchored ? "ANCHORED" : "");
		}
	}

	// Kept for HudAnchor_Store: a bone matrix is model-local, so publishing a
	// usable anchor needs the transform that puts this model in the world.
	g_hudAnchorSource = objectToWorldMatrix;

	renderer->BeginDrawHUDModel(playermo->RenderStyle, objectToWorldMatrix, orientation < 0, smf_flags);
	auto trans = psp->GetTranslation();
	if ((psp->Flags & PSPF_PLAYERTRANSLATED)) trans = psp->Owner->mo->Translation;

	RenderFrameModels(renderer, playermo->Level, smf, psp->GetState(), psp->GetTics(), ticFrac, trans, psp->Caller, psp);
	renderer->EndDrawHUDModel(playermo->RenderStyle, smf_flags);
}

double getCurrentFrame(const ModelAnim &anim, double tic, bool *looped)
{
	if(anim.framerate <= 0) return anim.startFrame;

	double frame = ((tic - anim.startTic) / GameTicRate) * anim.framerate; // position in frames

	double duration = double(anim.lastFrame) - anim.startFrame;

	if((anim.flags & MODELANIM_LOOP) && frame >= duration)
	{
		if(looped) *looped = true;
		frame = frame - duration;
		return fmod(frame, anim.lastFrame - anim.loopFrame) + anim.loopFrame;
	}
	else
	{
		return min(frame, duration) + anim.startFrame;
	}
}

void calcFrame(const ModelAnim &anim, double tic, ModelAnimFrameInterp &inter)
{
	bool looped = false;

	double frame = getCurrentFrame(anim, tic, &looped);

	inter.frame1 = int(floor(frame));

	inter.inter = frame - inter.frame1;

	inter.frame2 = int(ceil(frame));

	int startFrame = (looped ? anim.loopFrame : anim.startFrame);

	if(inter.frame1 < startFrame) inter.frame1 = anim.lastFrame;
	if(inter.frame2 > anim.lastFrame) inter.frame2 = startFrame;
}

void calcFrames(const ModelAnim &curAnim, double tic, ModelAnimFrameInterp &to, float &inter)
{
	if(curAnim.startTic > tic)
	{
		inter = (tic - (curAnim.startTic - curAnim.switchOffset)) / curAnim.switchOffset;

		calcFrame(curAnim, curAnim.startTic, to);
	}
	else
	{
		inter = -1.0f;
		calcFrame(curAnim, tic, to);
	}
}

CalcModelFrameInfo CalcModelFrame(FLevelLocals *Level, const FSpriteModelFrame *smf, const FState *curState, const int curTics, DActorModelData* data, AActor* actor, bool is_decoupled, double tic, double ticFrac, const DPSprite* psp)
{
	// [BB] Frame interpolation: Find the FSpriteModelFrame smfNext which follows after smf in the animation
	// and the scalar value inter ( element of [0,1) ), both necessary to determine the interpolated frame.

	int smf_flags = smf->getFlags(data);

	const FSpriteModelFrame * smfNext = nullptr;
	float inter = 0.;

	ModelAnimFrameInterp decoupled_frame;
	ModelAnimFrame * decoupled_frame_prev = nullptr;

	// if prev_frame == -1: interpolate(main_frame, next_frame, inter), else: interpolate(interpolate(main_prev_frame, main_frame, inter_main), interpolate(next_prev_frame, next_frame, inter_next), inter)
	// 4-way interpolation is needed to interpolate animation switches between animations that aren't 35hz

	if(is_decoupled)
	{
		smfNext = smf = &BaseSpriteModelFrames[(data != nullptr && data->modelDef != nullptr) ? data->modelDef : actor->GetClass()];
		if(data && !(data->anims.curAnim.flags & MODELANIM_NONE))
		{
			calcFrames(data->anims.curAnim, tic, decoupled_frame, inter);
			decoupled_frame_prev = &data->anims.prevAnim;
		}
	}
	else if (gl_interpolate_model_frames && !(smf_flags & MDL_NOINTERPOLATION))
	{
		FState *nextState = curState->GetNextState();
		if (curState != nextState && nextState)
		{
			// [BB] To interpolate at more than 35 fps we take tic fractions into account.
			float ticFraction = 0.;
			// [BB] In case the tic counter is frozen we have to leave ticFraction at zero.
			if (!WorldPaused(true) && !Level->isFrozen())
			{
				ticFraction = ticFrac;
			}

			inter = static_cast<double>(curState->Tics - curTics + ticFraction) / static_cast<double>(curState->Tics);

			// [BB] For some actors (e.g. ZPoisonShroom) spr->actor->tics can be bigger than curState->Tics.
			// In this case inter is negative and we need to set it to zero.
			if (curState->Tics < curTics)
				inter = 0.;
			else
			{
				// [BB] Workaround for actors that use the same frame twice in a row.
				// Most of the standard Doom monsters do this in their see state.
				if ((smf_flags & MDL_INTERPOLATEDOUBLEDFRAMES))
				{
					const FState *prevState = curState - 1;
					if ((curState->sprite == prevState->sprite) && (curState->Frame == prevState->Frame))
					{
						inter /= 2.;
						inter += 0.5;
					}
					if (nextState && ((curState->sprite == nextState->sprite) && (curState->Frame == nextState->Frame)))
					{
						inter /= 2.;
						nextState = nextState->GetNextState();
					}
				}
				if (nextState && inter != 0.0)
					smfNext = FindModelFrame(actor, nextState->sprite, nextState->Frame, false);
			}
		}
	}

	// RS FORK -- EXPLICIT INTERPOLATION.
	// Everything above derives 'inter' from state tics and only tweens across a
	// state transition. That is useless when one of OUR model animations is
	// being played across THEIR state timings -- the blend would restart on
	// every state change and sit at zero in between, so the model would snap
	// from pose to pose. When the psprite supplies a lerp we take it verbatim.
	//
	// smfNext must be non-null or RenderModelFrame's nextFrame test fails and
	// 'inter' is discarded. Same definition, different frame number, so smf
	// itself is the correct "next" here.
	//
	// Deliberately placed AFTER the gl_interpolate_model_frames /
	// MDL_NOINTERPOLATION branch so an explicit blend is not silently dropped
	// when a user turns that CVar off.
	if (psp && psp->ModelFrameLerp >= 0.f)
	{
		float f = psp->ModelFrameLerp;
		if (f > 1.f) f = 1.f;
		inter   = f;
		smfNext = smf;
	}
	// RS FORK -- the same, for a WORLD ACTOR. A world-actor hand has no psprite
	// to carry the blend, and without this every pose change is a single-tic
	// jump between rigged shapes, which reads as the hand teleporting.
	else if (actor && actor->ModelFrameLerp >= 0.f)
	{
		float f = actor->ModelFrameLerp;
		if (f > 1.f) f = 1.f;
		inter   = f;
		smfNext = smf;
	}

	// RS FORK -- NATIVE STATE REMAP interpolation (FORK_CHANGES.md, "Native
	// state remap"). When the weapon carries a state->frame table, the
	// psprite's own state IS the animation clock, and intra-state progress
	// comes straight from its tic countdown plus the render fraction --
	// true display-rate smoothness, computed where the display rate lives.
	// Runs after every other inter derivation so the table, when present,
	// is authoritative; unmapped states simply keep whatever resolved above.
	if (psp && data && data->stateRemap.CountUsed() > 0 && curState != nullptr)
	{
		if (data->stateRemap.CheckKey(intptr_t(curState)) && curState->Tics > 0)
		{
			// [RS FORK] Upstream 5.0.0 hands the render fraction down as 'ticFrac'
			// and folded the console/menu/freeze test into WorldPaused(), which
			// already returns false for MENU_OnNoPause -- so the fork's original
			// "keep animating under a non-pausing menu" semantics are preserved.
			float ticFraction = 0.f;
			if (!WorldPaused(true) && !Level->isFrozen())
			{
				ticFraction = ticFrac;
			}
			float f = (float(curState->Tics - curTics) + ticFraction) / float(curState->Tics);
			if (f < 0.f) f = 0.f;
			if (f > 1.f) f = 1.f;
			inter   = f;
			smfNext = smf;
		}
	}

	unsigned modelsamount = smf->modelsAmount;
	//[SM] - if we added any models for the frame to also render, then we also need to update modelsAmount for this smf
	if (data != nullptr)
	{
		if (data->models.Size() > modelsamount)
			modelsamount = data->models.Size();
	}

	return
	{
		actor,          // RS fork -- so the overrides pass can read ModelFrame
		smf_flags,
		smfNext,
		inter,
		is_decoupled,
		decoupled_frame,
		decoupled_frame_prev,
		modelsamount
	};
}

bool CalcModelOverrides(int i, const FSpriteModelFrame *smf, DActorModelData* data, const CalcModelFrameInfo &info, ModelDrawInfo &out, bool is_decoupled, const DPSprite* psp)
{
	//reset drawinfo
	out.modelid = -1;
	out.animationid = -1;
	out.modelframe = -1;
	out.modelframenext = -1;
	out.modelframe_explicit = false;
	out.skinid.SetNull();
	out.surfaceskinids.Clear();

	// [RS] A part dropped from the draw entirely -- which is what "the magazine
	// is out of the gun" actually is. The alternative a MODELDEF has to reach
	// for is pointing the part at a junk frame index and relying on an
	// out-of-range frame drawing nothing; a real switch costs nothing and does
	// not depend on that.
	//
	// CAVEAT FOR RIGGED MODELS: RenderModelFrame threads boneStartingPosition
	// and evaluatedSingle across iterations, so skipping a part of an IQM whose
	// bones are evaluated once for the whole stack can leave later parts reading
	// a bone offset that was never written. MD3 has no bones and is the case
	// this exists for; hiding a part of a skinned model is untested.
	if (psp && i >= 0 && i < DPSprite::RS_MODEL_PARTS && psp->ModelPartHidden[i])
	{
		return false;
	}

	if (data)
	{
		//modelID
		if (data->models.SSize() > i && data->models[i].modelID >= 0)
		{
			out.modelid = data->models[i].modelID;
		}
		else if(data->models.SSize() > i && data->models[i].modelID == -2)
		{
			return false;
		}
		else if(smf->modelsAmount > i)
		{
			out.modelid = smf->modelIDs[i];
		}

		//animationID
		if (data->animationIDs.SSize() > i && data->animationIDs[i] >= 0)
		{
			out.animationid = data->animationIDs[i];
		}
		else if(smf->modelsAmount > i)
		{
			out.animationid = smf->animationIDs[i];
		}
		if(!is_decoupled)
		{
			//modelFrame
			if (data->modelFrameGenerators.SSize() > i
				&& (unsigned)data->modelFrameGenerators[i] < info.modelsamount
				&& smf->modelframes[data->modelFrameGenerators[i]] >= 0
				) {
				out.modelframe = smf->modelframes[data->modelFrameGenerators[i]];

				if (info.smfNext)
				{
					if(info.smfNext->modelframes[data->modelFrameGenerators[i]] >= 0)
					{
						out.modelframenext = info.smfNext->modelframes[data->modelFrameGenerators[i]];
					}
					else
					{
						out.modelframenext = info.smfNext->modelframes[i];
					}
				}
			}
			else if(smf->modelsAmount > i)
			{
				out.modelframe = smf->modelframes[i];
				if (info.smfNext) out.modelframenext = info.smfNext->modelframes[i];
			}
		}

		// [RS] PER-PART FRAME ADDRESSING, and it OVERRIDES everything above for
		// its own index only. A caller that sets both gets per-part where it
		// asked for it and the scalar everywhere else, so anything setting only
		// the scalar is untouched.
		//
		// ModelFrame is ONE number applied to EVERY sub-model in the stack --
		// right for a donor mesh playing a single animation across its parts,
		// wrong for a gun. A weapon is already SEPARATE models in MODELDEF, each
		// with its own FrameIndex rows, but one shared frame number means the
		// only way to show "slide back AND magazine out" is a baked frame for
		// that exact combination: the cross product of every part's every
		// position, hand-authored.
		//
		// Out of range is deliberately not clamped. FMD3Model::RenderFrame
		// rejects an impossible frame and draws nothing, which is a visible
		// failure rather than a silent wrong pose.
		if (psp && i >= 0 && i < DPSprite::RS_MODEL_PARTS && psp->ModelFramePart[i] >= 0)
		{
			out.modelframe     = psp->ModelFramePart[i];
			out.modelframenext = (psp->ModelFrameNextPart[i] >= 0)
				? psp->ModelFrameNextPart[i] : psp->ModelFramePart[i];
			out.modelframe_explicit = true;
		}

		//skinID
		if (data->skinIDs.SSize() > i && data->skinIDs[i].isValid())
		{
			out.skinid = data->skinIDs[i];
		}
		else if(smf->modelsAmount > i)
		{
			out.skinid = smf->skinIDs[i];
		}

		//surfaceSkinIDs
		if(data->models.SSize() > i && data->models[i].surfaceSkinIDs.SSize() > 0)
		{
			unsigned sz1 = smf->surfaceskinIDs.Size();
			unsigned sz2 = data->models[i].surfaceSkinIDs.Size();
			unsigned start = i * MD3_MAX_SURFACES;

			out.surfaceskinids = data->models[i].surfaceSkinIDs;
			out.surfaceskinids.Resize(MD3_MAX_SURFACES);

			for (unsigned surface = 0; surface < MD3_MAX_SURFACES; surface++)
			{
				if (sz2 > surface && (data->models[i].surfaceSkinIDs[surface].isValid()))
				{
					continue;
				}
				if((surface + start) < sz1)
				{
					out.surfaceskinids[surface] = smf->surfaceskinIDs[surface + start];
				}
				else
				{
					out.surfaceskinids[surface].SetNull();
				}
			}
		}
	}
	else
	{
		out.modelid = smf->modelIDs[i];
		out.animationid = smf->animationIDs[i];
		out.modelframe = smf->modelframes[i];
		if (info.smfNext) out.modelframenext = info.smfNext->modelframes[i];
		out.skinid = smf->skinIDs[i];
	}

	// RS FORK -- DIRECT MODEL FRAME ADDRESSING.
	// Every branch above resolved the frame through the sprite: a letter index
	// capped at MAX_SPRITE_FRAMES (29) fed through MODELDEF's FrameIndex table.
	// Our weapon meshes run past 70 frames, so most of their animation had no
	// letter to name it with and could not be reached at all.
	//
	// The psprite's ModelFrame replaces the NUMBER only. Everything else the
	// smf carries -- scale, offsets, angle/pitch/roll, skins, flags -- still
	// comes from the sprite lookup, which is why FindModelFrame must still
	// resolve upstream of here.
	//
	// Applied to every model index. A donor with several models is showing
	// frames of one animation, so they advance together; per-index divergence
	// would need an array and no donor needs it yet.
	//
	// Out of range is not clamped on purpose: FMD3Model::RenderFrame rejects
	// (unsigned)frameno >= Frames.Size() and draws nothing, which is a visible
	// failure. Silently clamping to the last frame would hide the bug.
	if (psp && psp->ModelFrame >= 0)
	{
		out.modelframe     = psp->ModelFrame;
		out.modelframenext = (psp->ModelFrameNext >= 0) ? psp->ModelFrameNext : psp->ModelFrame;
		out.modelframe_explicit = true;
	}
	// RS FORK -- the same, for a WORLD ACTOR. Checked second so a psprite still
	// wins on the HUD path; the two never both apply to one draw.
	else if (info.actor && info.actor->ModelFrame >= 0)
	{
		out.modelframe     = info.actor->ModelFrame;
		out.modelframenext = (info.actor->ModelFrameNext >= 0)
			? info.actor->ModelFrameNext : info.actor->ModelFrame;
		out.modelframe_explicit = true;
	}

	if (vr_pose_debug && psp)
	{
		// Separate slots per hand so the two do not mask each other's changes.
		const bool offhand = (psp->GetID() >= PSP_OFFHANDWEAPON);
		static int lastMain = -0x7fffffff, lastOff = -0x7fffffff;
		int &last = offhand ? lastOff : lastMain;
		int key = (psp->ModelFrame * 4) + (out.modelframe_explicit ? 1 : 0) + (out.modelframe << 12);
		if (key != last)
		{
			last = key;
			Printf("[POSE/in ] %s layer=%d psp.ModelFrame=%d -> drawinfo.modelframe=%d next=%d explicit=%s\n",
				offhand ? "OFF " : "MAIN", psp->GetID(), psp->ModelFrame,
				out.modelframe, out.modelframenext,
				out.modelframe_explicit ? "yes" : "NO");
		}
	}

	// RS FORK -- NATIVE STATE REMAP frame resolution (FORK_CHANGES.md,
	// "Native state remap"). The table maps the psprite's CURRENT state
	// directly to mesh frames, registered once at bind time from ZScript.
	// Placed last on purpose: when a table exists it beats both the sprite
	// lookup and the legacy per-tick ModelFrame fields (which can linger,
	// serialized, from older builds). Unmapped states fall through to
	// whatever resolved above -- the pinned anchor's rest pose -- so a
	// state the walk couldn't see reads as a pause, never as garbage.
	if (psp && data && data->stateRemap.CountUsed() > 0)
	{
		FState *st = psp->GetState();
		if (st != nullptr)
		{
			int64_t *v = data->stateRemap.CheckKey(intptr_t(st));
			if (v != nullptr)
			{
				out.modelframe     = int(uint32_t(*v >> 32));
				out.modelframenext = int(uint32_t(*v & 0xffffffff));
				out.modelframe_explicit = true;
			}
		}
	}

	return (out.modelid >= 0 && out.modelid < Models.SSize());
}


const TArray<VSMatrix> * ProcessModelFrame(FModel * animation, bool nextFrame, int i, const FSpriteModelFrame *smf, DActorModelData* modelData, const CalcModelFrameInfo &frameinfo, ModelDrawInfo &drawinfo, bool is_decoupled, double tic, BoneInfo *out)
{
	const TArray<TRS>* animationData = nullptr;

	if (drawinfo.animationid >= 0)
	{
		animation = Models[drawinfo.animationid];
		animationData = animation->AttachAnimationData();
	}

	const TArray<VSMatrix> *boneData = nullptr;

	if (vr_pose_debug && is_decoupled)
	{
		static int last = -0x7fffffff;
		const int branch = (frameinfo.decoupled_frame.frame1 >= 0) ? 0
		                 : (drawinfo.modelframe_explicit ? 1 : 2);
		int key = branch + (drawinfo.modelframe << 4);
		if (key != last)
		{
			last = key;
			static const char *names[3] = { "ANIM (SetAnimation wins)", "STATIC (our pose)", "REST (pose discarded)" };
			Printf("[POSE/out] decoupled -> %s  frame=%d next=%d  decoupled_frame1=%d\n",
				names[branch], drawinfo.modelframe, drawinfo.modelframenext,
				frameinfo.decoupled_frame.frame1);
		}
	}

	if(is_decoupled)
	{
		if(frameinfo.decoupled_frame.frame1 >= 0)
		{
			boneData = animation->CalculateBones(
				frameinfo.decoupled_frame_prev ? *frameinfo.decoupled_frame_prev : nullptr,
				frameinfo.decoupled_frame,
				frameinfo.inter,
				animationData,
				(modelData && modelData->modelBoneOverrides.SSize() > i)
				? &modelData->modelBoneOverrides[i]
				: nullptr,
				out,
				tic);
		}
		else if(drawinfo.modelframe_explicit)
		{
			// RS FORK -- STATIC POSE ON THE DECOUPLED PATH.
			//
			// A +DECOUPLEDANIMATIONS model has only two states here: playing an
			// animation (above), or the rest pose (below). Neither consults
			// drawinfo.modelframe, which is read only on the non-decoupled
			// branch -- so a decoupled model could not be pinned to a single
			// authored frame at all.
			//
			// That is exactly what hand posing needs. The poses are baked as
			// frames of one clip and ZScript picks one per tic from controller
			// input; there is no animation to play, just a pose to hold. Doing
			// it through SetAnimation instead would mean running a clip at zero
			// framerate to keep it still, i.e. fighting the animation clock to
			// get a static result.
			//
			// Same construction as the non-decoupled branch below, so an
			// explicitly addressed frame means the same thing on both paths.
			// Bone overrides still compose on top, so a posed hand can still be
			// adjusted procedurally afterwards.
			boneData = animation->CalculateBones(
				nullptr,
				{
					nextFrame ? frameinfo.inter : -1.0f,
					drawinfo.modelframe,
					drawinfo.modelframenext
				},
				-1.0f,
				animationData,
				(modelData && modelData->modelBoneOverrides.SSize() > i)
				? &modelData->modelBoneOverrides[i]
				: nullptr,
				out,
				tic);
		}
		else
		{
			boneData = animation->CalculateBonesOnlyOffsets(
				(modelData && modelData->modelBoneOverrides.SSize() > i)
				? &modelData->modelBoneOverrides[i]
				: nullptr,
				out,
				tic);
		}
	}
	else
	{
		boneData = animation->CalculateBones(
			nullptr,
			{
				nextFrame ? frameinfo.inter : -1.0f,
				drawinfo.modelframe,
				drawinfo.modelframenext
			},
			-1.0f,
			animationData,
			(modelData && modelData->modelBoneOverrides.SSize() > i)
			? &modelData->modelBoneOverrides[i]
			: nullptr,
			out,
			tic);
	}

	return boneData;
}

static inline void RenderModelFrame(FModelRenderer *renderer, int i, const FSpriteModelFrame *smf, DActorModelData* modelData, const CalcModelFrameInfo &frameinfo, ModelDrawInfo &drawinfo, bool is_decoupled, double tic, double ticFrac, FTranslationID translation, int &boneStartingPosition, bool &evaluatedSingle, const DPSprite *psp = nullptr)
{
	FModel * mdl = Models[drawinfo.modelid];
	auto tex = drawinfo.skinid.isValid() ? TexMan.GetGameTexture(drawinfo.skinid, true) : nullptr;
	mdl->BuildVertexBuffer(renderer);

	auto ssidp = drawinfo.surfaceskinids.Size() > 0
		? drawinfo.surfaceskinids.Data()
		: (((i * MD3_MAX_SURFACES) < smf->surfaceskinIDs.SSize()) ? &smf->surfaceskinIDs[i * MD3_MAX_SURFACES] : nullptr);

	bool nextFrame = frameinfo.smfNext && drawinfo.modelframe != drawinfo.modelframenext;

	// RS FORK -- a posed palette's own upload, for a model that otherwise uploads its
	// bones inside RenderFrame (neither decoupled nor attachments). -1 keeps that path.
	int posedBoneStart = -1;

	// [Jay] while per-model animations aren't done, DECOUPLEDANIMATIONS does the same as MODELSAREATTACHMENTS
	if(!evaluatedSingle)
	{  // [Jay] TODO per-model decoupled animations
		const TArray<VSMatrix> *boneData = ProcessModelFrame(mdl, nextFrame, i, smf, modelData, frameinfo, drawinfo, is_decoupled, tic, nullptr);

		if(frameinfo.smf_flags & MDL_MODELSAREATTACHMENTS || is_decoupled)
		{
			if(!boneData && is_decoupled)
			{
				boneData = mdl->CalculateBonesOnlyOffsets((modelData && modelData->modelBoneOverrides.SSize() > i)? &modelData->modelBoneOverrides[i] : nullptr, nullptr, tic);
			}

			// RS FORK -- DRAW-TIME JOINT POSES AND REACH CHAINS (model_reach.h), on the
			// finished palette: every decoupled branch of ProcessModelFrame and the
			// OnlyOffsets fallback above arrive here, stock overrides already in. Never
			// on the base-pose fallback below, which is not a palette. Hands boneData
			// back untouched unless RenderModel opened a pose window for this actor.
			if (boneData) boneData = ModelDrawPose_Apply(frameinfo.actor, mdl, i, *boneData);

			// [RS FORK] Upstream's new CalculateBonesOnlyOffsets path only covers
			// the decoupled case. Keep the fork's base-pose fallback so a
			// MDL_MODELSAREATTACHMENTS model that is NOT decoupled still gets a
			// bone set uploaded instead of rendering unskinned.
			if(!boneData)
			{
				boneData = mdl->GetBasePose();
			}

			boneStartingPosition = boneData ? screen->mBones->UploadBones(*boneData) : -1;
			evaluatedSingle = true;

		}
		else if (boneData)
		{
			// RS FORK -- the same, for a model that uploads its own bones inside RenderFrame.
			// A posed palette gets its own upload for THIS draw only: boneStartingPosition is
			// shared with the next model index and stays as it was.
			const TArray<VSMatrix> *posed = ModelDrawPose_Apply(frameinfo.actor, mdl, i, *boneData);
			if (posed != boneData) posedBoneStart = screen->mBones->UploadBones(*posed);
		}

		// Publish this model's bones for anything anchored to this layer.
		//
		// Outside the branch above on purpose. That branch only runs for
		// attachment sets and decoupled models, and anchoring has no reason to
		// require either -- a plain weapon model resolves perfectly good bones and
		// something should be able to hang off them. Keeping the store inside it
		// meant a non-decoupled weapon silently published nothing, so anchoring to
		// it did nothing and looked like a script bug.
		if (psp && boneData) HudAnchor_Store(psp, mdl, *boneData);
	}

	// RS FORK -- PER-SURFACE FRAME ADDRESSING (p_pspr.h, model.h).
	//
	// Collect THIS model's surface overrides out of the psprite's flat slot
	// table and hand them down. Built on the stack per draw: sixteen entries,
	// and the common case -- no weapon driving any surface -- is one test in
	// AnySurfaceOverride() and no work at all.
	//
	// Filtered to model index i HERE rather than inside the model, because a
	// model has no way to know its own place in the MODELDEF stack.
	FModelSurfaceOverride surfItems[DPSprite::RS_SURF_SLOTS];
	FModelSurfaceOverrideList surfList;
	// EITHER A PSPRITE OR A WORLD ACTOR OWNS THESE.
	//
	// A weapon on a psprite hangs its overrides there; a weapon held in WORLD
	// space has no psprite at all, so DActorModelData carries the identical
	// table under the identical names. Same feature, two homes, one reader --
	// a second code path here is how the two would drift the first time either
	// gained anything.
	//
	// The psprite wins when there is one: a HUD weapon that also has model data
	// is still a HUD weapon.
	const int   *ovModel = nullptr, *ovSurface = nullptr, *ovFrame = nullptr, *ovNext = nullptr;
	const float *ovLerp = nullptr, *ovPos = nullptr, *ovPosPrev = nullptr;
	const bool  *ovHidden = nullptr;
	const int   *ovBlendTex = nullptr;	// RS fork -- actor model data only; a HUD sprite has none
	const float *ovBlend    = nullptr;
	// RS fork -- the live-transform half of the same table. See
	// FModelSurfaceOverride in model.h.
	const bool     *ovHasXf  = nullptr;
	const FVector3 *ovOfs    = nullptr;
	const FVector4 *ovRot    = nullptr;
	const FVector3 *ovOfsPrev = nullptr;
	const FVector4 *ovRotPrev = nullptr;
	// E2 -- the draw-rate hand drive. Mutable, unlike everything else here: the
	// renderer captures the anchor and publishes the drawn value back, so
	// script reads the number that was actually drawn rather than a second
	// estimate of it.
	DActorModelData *driveData = nullptr;
	if (psp && psp->AnySurfaceOverride())
	{
		ovModel = psp->SurfOvModel; ovSurface = psp->SurfOvSurface;
		ovFrame = psp->SurfOvFrame; ovNext    = psp->SurfOvNext;
		ovLerp  = psp->SurfOvLerp;  ovHidden  = psp->SurfOvHidden;
		ovPos   = psp->SurfOvPos;   ovPosPrev = psp->SurfOvPosPrev;
		ovHasXf = psp->SurfOvHasXf; ovOfs     = psp->SurfOvOfs;   ovRot = psp->SurfOvRot;
		ovOfsPrev = psp->SurfOvOfsPrev; ovRotPrev = psp->SurfOvRotPrev;
	}
	else if (!psp && modelData && modelData->AnySurfaceOverride())
	{
		ovModel = modelData->SurfOvModel; ovSurface = modelData->SurfOvSurface;
		ovFrame = modelData->SurfOvFrame; ovNext    = modelData->SurfOvNext;
		ovLerp  = modelData->SurfOvLerp;  ovHidden  = modelData->SurfOvHidden;
		ovBlendTex = modelData->SurfOvBlendTex; ovBlend = modelData->SurfOvBlend;
		ovPos   = modelData->SurfOvPos;   ovPosPrev = modelData->SurfOvPosPrev;
		ovHasXf = modelData->SurfOvHasXf; ovOfs     = modelData->SurfOvOfs;   ovRot = modelData->SurfOvRot;
		ovOfsPrev = modelData->SurfOvOfsPrev; ovRotPrev = modelData->SurfOvRotPrev;
		driveData = modelData;
	}
	if (ovModel)
{
		int n = 0;
		for (int s = 0; s < DPSprite::RS_SURF_SLOTS; s++)
		{
			if (ovModel[s] != i || ovSurface[s] < 0) continue;
			FModelSurfaceOverride& o = surfItems[n++];
			o.surface   = ovSurface[s];
			o.frame     = ovFrame[s];
			o.frameNext = ovNext[s];
			o.lerp      = ovLerp[s];
			o.hidden    = ovHidden[s];
			if (ovBlendTex && ovBlend && ovBlend[s] > 0.f && ovBlendTex[s] > 0)
			{
				o.blendSkin   = FSetTextureID(ovBlendTex[s]);
				o.blendAmount = ovBlend[s] > 1.f ? 1.f : ovBlend[s];
			}

			// The live transform, if this slot has one. An all-zero
			// quaternion means "never written" and is read as identity, so a
			// caller that only ever translates never has to supply one.
			// ---- E2: DRAW-RATE HAND DRIVE -------------------------------
			//
			// A driven slot ignores whatever script last wrote and computes its
			// own position from the LIVE controller pose, on the frame being
			// drawn. That is the whole point: the part and the hand come from
			// one pose at one instant, so they cannot separate. A tic-driven
			// part is smooth and still a tic behind, which is exactly the
			// difference this exists to remove and exactly what a slow test
			// cannot show you.
			bool driven = false;
			if (driveData && driveData->SurfDrive[s].on)
			{
				VSMatrix modelToWorld;
				auto vrmode = VRMode::GetVRModeCached(true);
				if (vrmode && vrmode->IsVR() && renderer->GetModelToWorldMatrix(&modelToWorld))
				{
					FHandDrive &drive = driveData->SurfDrive[s];
					VSMatrix handMat;
					const int dhand = (drive.hand == 1) ? VR_OFFHAND : VR_MAINHAND;

					// GetHandTransform, NOT GetWeaponTransform. The latter
					// applies a conditional X mirror (hw_vrmodes.cpp, gated on
					// the held weapon's auto-reverse flag), and a formula that
					// projects a hand onto a signed axis reads that mirror as a
					// sign flip -- on one hand only, for some weapons. Silent at
					// rest and wrong at an angle.
					//
					// THE DRIVE ITSELF -- arm, solve, publish the drawn value, pose
					// the part -- is HandDrive_OwnerStep (model_handdrive.cpp), the
					// same function a joint drive uses. A hinge or two-stage drive
					// takes its own branch inside it, and a plain drive reads exactly
					// as it did. What stays here is the surface's own diagnostics,
					// read off `step`.
					if (vrmode->GetHandTransform(VR_ControllerForHand(dhand), &handMat))
					{
						FHandDriveStep step;
						HandDrive_OwnerStep(drive, handMat, modelToWorld, o.offset, o.rotation, step);
						o.hasTransform = true;
						driven = true;

						if (step.staged)
						{
							// DIAGNOSTIC: the renderer has picked the drive up.
							// Once per arm, and never more than four a second.
							if (step.armedNow)
							{
								FSurfaceStageTrace *t = SurfaceStageTrace(driveData, s, true);
								t->logged  = step.wasStage2 ? 2 : 1;
								t->pending = 0;
								const uint64_t nowMs = I_msTime();
								if (nowMs - t->armMs >= 250)
								{
									t->armMs = nowMs;
									const bool hinge1 = drive.hinge;
									const int kind2 = drive.stage2Kind;
									char st2[96] = "none";
									if (kind2 != HANDDRIVE_None)
										snprintf(st2, sizeof(st2), "%s %.2f%s, split %.3f",
											kind2 == HANDDRIVE_Hinge ? "hinge" : "slide", drive.stage2Amount,
											kind2 == HANDDRIVE_Hinge ? " deg" : " units", drive.split);
									Printf("[DRIVESTAGE] model %d surf %d slot %d: renderer armed at v=%.3f, hand %d working stage %d -- stage 1 %s %.2f%s, stage 2 %s\n",
										i, o.surface, s, step.prevValue, drive.hand, t->logged,
										hinge1 ? "hinge" : "slide", hinge1 ? drive.turnDeg : drive.dist,
										hinge1 ? " deg" : " units", st2);
								}
							}

							// DIAGNOSTIC: the stage handoff, seen in the renderer, once
							// per crossing. The frame the hand changes stage is kept,
							// and reported once the drawn value has left the split by 2%
							// of the whole travel in the new stage. So a hand resting on
							// the corner cannot flood the log by dithering across it,
							// and a crossing backed straight out of is not reported.
							if (drive.stage2Kind != HANDDRIVE_None)
							{
								const bool crossed = (step.st.inStage2 != step.wasStage2);
								FSurfaceStageTrace *t = SurfaceStageTrace(driveData, s, crossed);
								if (t && crossed)
								{
									t->pending    = step.st.inStage2 ? 2 : 1;
									t->crossPrev  = step.prevValue;
									t->crossV     = step.value;
									t->crossCarry = step.carry;
								}
								if (t && t->pending != 0)
								{
									const float split = drive.split;
									if (t->pending == t->logged) t->pending = 0;
									else if (fabs(step.value - split) >= 0.02f)
									{
										Printf("[DRIVESTAGE] model %d surf %d slot %d: hand moved from stage %d to stage %d, now v=%.3f (split %.3f). Crossing frame drew %.3f -> %.3f and carried %.0f%% of that frame's hand motion into stage %d\n",
											i, o.surface, s, t->logged, t->pending, step.value, split,
											t->crossPrev, t->crossV, t->crossCarry * 100.f, t->pending);
										t->logged  = t->pending;
										t->pending = 0;
									}
								}
							}

							if (vr_surf_debug)
							{
								static int lastSV[16 * 64];
								static bool svInit = false;
								if (!svInit) { for (int k = 0; k < 16*64; k++) lastSV[k] = -0x7fffffff; svInit = true; }
								int vslot = (i * 64 + o.surface) & (16*64 - 1);
								int vkey = (int)(step.value * 200.f);
								if (vkey != lastSV[vslot])
								{
									lastSV[vslot] = vkey;
									Printf("[DRIVE] model %d surf %d hand %d  v=%.3f  stage %d  anchor=(%.3f %.3f) base=(%.3f %.3f)  handModel=(%.2f %.2f %.2f)\n",
										i, o.surface, drive.hand, step.value, step.st.inStage2 ? 2 : 1,
										step.st.anchor[0], step.st.anchor[1], step.st.base[0], step.st.base[1],
										step.handModel.X, step.handModel.Y, step.handModel.Z);
								}
							}
						}
						else if (vr_surf_debug)
						{
							const float dist = (drive.dist != 0.f) ? drive.dist : 1.f;
							static int lastV[16 * 64];
							static bool vInit = false;
							if (!vInit) { for (int k = 0; k < 16*64; k++) lastV[k] = -0x7fffffff; vInit = true; }
							int vslot = (i * 64 + o.surface) & (16*64 - 1);
							int vkey = (int)(step.value * 200.f);
							if (vkey != lastV[vslot])
							{
								lastV[vslot] = vkey;
								Printf("[DRIVE] model %d surf %d hand %d  v=%.3f  proj=%.3f anchor=%.3f dist=%.3f  handModel=(%.2f %.2f %.2f)\n",
									i, o.surface, dhand, step.value, step.proj, drive.anchor,
									dist, step.handModel.X, step.handModel.Y, step.handModel.Z);
							}
						}
					}
				}
			}

			o.hasTransform = driven || (ovHasXf && ovHasXf[s]);
			// A script-set transform, interpolated to the drawn instant by the
			// shared pure function -- see SurfaceSetPose.
			if (o.hasTransform && !driven)
				SurfaceSetPose(ovOfs, ovRot, ovOfsPrev, ovRotPrev, s, ticFrac, o.offset, o.rotation);

			// RS FORK -- DISPLAY-RATE PART MOTION (p_pspr.h, SurfOvPos).
			//
			// Script runs at 35 Hz and a headset draws at three times that, so
			// a part driven by the frame/lerp triple above is handed the same
			// values two or three draws running and steps visibly. Resolving
			// the smoothed position here instead, and deriving the triple from
			// it, means what the renderer gets changes on every DRAWN frame
			// rather than on every tic.
			//
			// UNITS: SurfOvPos is a FRACTIONAL FRAME INDEX -- 3.5 is halfway
			// between mesh frame 3 and mesh frame 4. Not map units, not a
			// 0..1 fraction of the travel.
			//
			// Negative is the opt-out, and a deliberately pinned pose takes
			// that path: a slide held at locked-back wants to be exactly
			// there, not eased toward it.
			if (vr_surf_debug)
			{
				// ONE SLOT PER (model, surface), NOT ONE SHARED SLOT.
				//
				// A single static here produced 11,572 lines from a session
				// with about ten real events: two surfaces are driven at once,
				// they alternate every draw, and each one's key always differs
				// from the other's -- so "has it changed" was true on every
				// single frame forever. The flood was the watcher, not the
				// thing being watched.
				//
				// THE LERP IS DELIBERATELY OUT OF THE KEY. With smoothing on it
				// changes every drawn frame by design, so including it puts the
				// flood straight back. The frame pair and the hidden flag are
				// what a human is actually looking for; the sub-frame position
				// is printed but not keyed on.
				static int lastKey[16 * 64];
				static bool keyInit = false;
				if (!keyInit) { for (int k = 0; k < 16 * 64; k++) lastKey[k] = -0x7fffffff; keyInit = true; }

				int slot = (i * 64 + o.surface) & (16 * 64 - 1);
				int key = (o.frame * 131) ^ (o.frameNext * 17)
				        ^ (o.hidden ? 0x40000000 : 0);
				if (key != lastKey[slot])
				{
					lastKey[slot] = key;
					Printf("[SURF] model %d surface %d -> frame %d..%d lerp %.3f%s  (pos %.3f prev %.3f ticFrac %.3f)\n",
						i, o.surface, o.frame, o.frameNext, o.lerp,
						o.hidden ? "  HIDDEN" : "",
						ovPos[s], ovPosPrev[s], ticFrac);
				}
			}

			if (ovPos[s] >= 0.f)
			{
				float prev = ovPosPrev[s];

				// A slot that has only just become active has no previous
				// position. Interpolating from the -1 sentinel would fling the
				// part in from before the start of the mesh on its first drawn
				// frame; starting still is the honest answer.
				if (prev < 0.f) prev = ovPos[s];

				float f = (float)ticFrac;
				if (f < 0.f) f = 0.f;
				if (f > 1.f) f = 1.f;

				float p = prev + (ovPos[s] - prev) * f;
				if (p < 0.f) p = 0.f;

				// RS FORK -- CLAMP TO THE MESH, AND SAY SO.
				//
				// A frame past the end of the mesh used to be accepted in
				// silence: o.frameNext ran off the end of the frame list and
				// what got drawn was whatever the surface renderer made of an
				// out-of-range index. Nothing said a number was wrong, so an
				// off-by-one in a part map looked like a part that "just
				// doesn't move right" -- and the parts most likely to carry a
				// bad number are exactly the deliberately-unusual ones (a
				// jam-stuck frame, a lock-back hold, a precondition-gated
				// pose) that nobody has a correct reference for yet.
				//
				// NumFrames() is -1 for formats that do not know their own
				// count, and the clamp is skipped for those rather than
				// guessed at.
				const int frameCount = mdl ? mdl->NumFrames() : -1;
				if (frameCount > 0)
				{
					const float maxPos = (float)(frameCount - 1);
					if (p > maxPos)
					{
						if (vr_surf_debug)
						{
							// Throttled per (model, surface) for the same
							// reason the trace above is: a part parked on a
							// bad frame is wrong on every drawn frame, and an
							// unthrottled complaint about it buries the log
							// it is trying to be visible in.
							static int lastBad[16 * 64];
							static bool badInit = false;
							if (!badInit) { for (int k = 0; k < 16 * 64; k++) lastBad[k] = -0x7fffffff; badInit = true; }

							int bslot = (i * 64 + o.surface) & (16 * 64 - 1);
							int bkey = (int)(p * 16.f);
							if (bkey != lastBad[bslot])
							{
								lastBad[bslot] = bkey;
								Printf(TEXTCOLOR_YELLOW "[SURF] model %d surface %d driven to frame %.3f, but this mesh only has %d frames (0..%d) -- clamped\n",
									i, o.surface, p, frameCount, frameCount - 1);
							}
						}
						p = maxPos;
					}
				}

				int lo = (int)p;
				o.frame     = lo;

				// frameNext must stay inside the mesh too. At the very last
				// frame there is nothing to blend toward, so it blends with
				// itself -- which is what "hold exactly here" means, and is
				// the same answer the pinned-pose path gives.
				o.frameNext = (frameCount > 0 && lo + 1 > frameCount - 1) ? lo : lo + 1;
				o.lerp      = p - (float)lo;
			}
		}
		if (n)
		{
			surfList.items = surfItems;
			surfList.count = n;
		}
	}

	mdl->RenderFrame(renderer, tex, drawinfo.modelframe, nextFrame ? drawinfo.modelframenext : drawinfo.modelframe, nextFrame ? frameinfo.inter : -1.f, translation, ssidp, posedBoneStart >= 0 ? posedBoneStart : boneStartingPosition, surfList.items ? &surfList : nullptr);
}

void RenderFrameModels(FModelRenderer *renderer, FLevelLocals *Level, const FSpriteModelFrame *smf, const FState *curState, int curTics, double ticFrac, FTranslationID translation, AActor* actor, const DPSprite* psp)
{
	double tic = actor->GetModelTimer();
	if (!WorldPaused(true) && !actor->isFrozen())
	{
		tic += ticFrac;
	}

	bool is_decoupled = (actor->flags9 & MF9_DECOUPLEDANIMATIONS);

	DActorModelData* modelData = actor ? actor->modelData.ForceGet() : nullptr;

	CalcModelFrameInfo frameinfo = CalcModelFrame(Level, smf, curState, curTics, modelData, actor, is_decoupled, tic, ticFrac, psp);
	ModelDrawInfo drawinfo;

	int boneStartingPosition = -1;
	bool evaluatedSingle = false;

	// [RS] Per-part interpolation. frameinfo.inter is one number for the whole
	// stack, so a part driven to its own frame pair also needs its own blend
	// position -- otherwise the slide racks at whatever rate the shared
	// animation happens to be at.
	//
	// baseInter is captured because the loop below overwrites frameinfo.inter
	// per part and every part that does NOT set one must get the shared value
	// back rather than the previous part's. AnyModelPartActive() keeps the whole
	// thing out of the way when nothing is using it, which is every existing
	// caller.
	const float baseInter = frameinfo.inter;
	const bool  anyPart   = (psp && psp->AnyModelPartActive());

	for (unsigned i = 0; i < frameinfo.modelsamount; i++)
	{
		if (anyPart)
		{
			frameinfo.inter = baseInter;
			if (i < (unsigned)DPSprite::RS_MODEL_PARTS && psp->ModelFrameLerpPart[i] >= 0.f)
			{
				float f = psp->ModelFrameLerpPart[i];
				if (f > 1.f) f = 1.f;
				frameinfo.inter = f;
			}
		}

		if (CalcModelOverrides(i, smf, modelData, frameinfo, drawinfo, is_decoupled, psp))
		{
			RenderModelFrame(renderer, i, smf, modelData, frameinfo, drawinfo, is_decoupled, tic, ticFrac, translation, boneStartingPosition, evaluatedSingle, psp);
		}
	}
}


static TArray<int> SpriteModelHash;
//TArray<FStateModelFrame> StateModelFrames;

//===========================================================================
//
// InitModels
//
//===========================================================================

void ParseModelDefLump(int Lump);
#include "model_fit.h"
#include "m_crc32.h"

// RS FORK -- the whole line a token sits on, [start, end) byte offsets into the
// script text: back to the character after the previous newline, forward to the
// next newline (not included). For MODELDEF fit files, which replace one line.
static void ModelFit_LineSpan(const FString &text, int tokenStart, int &lineStart, int &lineEnd)
{
	const char *t = text.GetChars();
	const int len = (int)text.Len();
	lineStart = tokenStart;
	while (lineStart > 0 && t[lineStart - 1] != '\n') lineStart--;
	lineEnd = tokenStart;
	while (lineEnd < len && t[lineEnd] != '\n') lineEnd++;
	if (lineEnd > lineStart && t[lineEnd - 1] == '\r') lineEnd--;
}

void InitModels()
{
	Models.DeleteAndClear();
	SpriteModelFrames.Clear();
	SpriteModelHash.Clear();
	ModelFit_ClearSources();	// RS fork -- gun fit mode's record of where each block came from

	// First, create models for each voxel
	for (unsigned i = 0; i < Voxels.Size(); i++)
	{
		FVoxelModel *md = new FVoxelModel(Voxels[i], false);
		Voxels[i]->VoxelIndex = Models.Push(md);
	}
	// now create GL model frames for the voxeldefs
	for (unsigned i = 0; i < VoxelDefs.Size(); i++)
	{
		FVoxelModel *md = (FVoxelModel*)Models[VoxelDefs[i]->Voxel->VoxelIndex];
		FSpriteModelFrame smf;
		memset((void*)&smf, 0, sizeof(smf));
		smf.isVoxel = true;
		smf.modelsAmount = 1;
		smf.modelframes.Alloc(1);
		smf.modelframes[0] = -1;
		smf.modelIDs.Alloc(1);
		smf.modelIDs[0] = VoxelDefs[i]->Voxel->VoxelIndex;
		smf.skinIDs.Alloc(1);
		smf.skinIDs[0] = md->GetPaletteTexture();
		smf.animationIDs.Alloc(1);
		smf.animationIDs[0] = -1;
		smf.xscale = smf.yscale = smf.zscale = VoxelDefs[i]->Scale;
		smf.angleoffset = VoxelDefs[i]->AngleOffset.Degrees();
		smf.xoffset = VoxelDefs[i]->xoffset;
		smf.yoffset = VoxelDefs[i]->yoffset;
		smf.zoffset = VoxelDefs[i]->zoffset;
		// this helps catching uninitialized data.
		assert(VoxelDefs[i]->PitchFromMomentum == true || VoxelDefs[i]->PitchFromMomentum == false);
		if (VoxelDefs[i]->PitchFromMomentum) smf.flags |= MDL_PITCHFROMMOMENTUM;
		if (VoxelDefs[i]->UseActorPitch) smf.flags |= MDL_USEACTORPITCH;
		if (VoxelDefs[i]->UseActorRoll) smf.flags |= MDL_USEACTORROLL;
		if (VoxelDefs[i]->PlacedSpin != 0)
		{
			smf.yrotate = 1.f;
			smf.rotationSpeed = VoxelDefs[i]->PlacedSpin / 55.55f;
			smf.flags |= MDL_ROTATING;
		}
		VoxelDefs[i]->VoxeldefIndex = SpriteModelFrames.Push(smf);
		if (VoxelDefs[i]->PlacedSpin != VoxelDefs[i]->DroppedSpin)
		{
			if (VoxelDefs[i]->DroppedSpin != 0)
			{
				smf.yrotate = 1.f;
				smf.rotationSpeed = VoxelDefs[i]->DroppedSpin / 55.55f;
				smf.flags |= MDL_ROTATING;
			}
			else
			{
				smf.yrotate = 0;
				smf.rotationSpeed = 0;
				smf.flags &= ~MDL_ROTATING;
			}
			SpriteModelFrames.Push(smf);
		}
	}

	int Lump;
	int lastLump = 0;
	while ((Lump = fileSystem.FindLump("MODELDEF", &lastLump)) != -1)
	{
		ParseModelDefLump(Lump);
	}

	// create a hash table for quick access
	SpriteModelHash.Resize(SpriteModelFrames.Size ());
	memset(SpriteModelHash.Data(), 0xff, SpriteModelFrames.Size () * sizeof(int));

	for (unsigned int i = 0; i < SpriteModelFrames.Size (); i++)
	{
		int j = ModelFrameHash(&SpriteModelFrames[i]) % SpriteModelFrames.Size ();

		SpriteModelFrames[i].hashnext = SpriteModelHash[j];
		SpriteModelHash[j]=i;
	}
}

void ParseModelDefLump(int Lump)
{
	FScanner sc(Lump);
	while (sc.GetString())
	{
		if (sc.Compare("model"))
		{
			int index, surface;
			FString path = "";
			sc.MustGetString();

			FSpriteModelFrame smf;
			memset((void*)&smf, 0, sizeof(smf));
			smf.xscale=smf.yscale=smf.zscale=1.f;

			// RS FORK -- GUN FIT MODE REMEMBERS WHERE THIS BLOCK CAME FROM (model_fit.h).
			// The `Model` keyword has just been read, so it ends at the scanner.
			ModelDefBlockSource fitSrc;
			fitSrc.lump = Lump;
			fitSrc.lumpName = fileSystem.GetFileFullPath(Lump).c_str();
			fitSrc.blockStart = sc.BytePos() - (int)strlen("model");

			auto type = PClass::FindClass(sc.String);
			if (!type || type->Defaults == nullptr)
			{
				sc.ScriptError("MODELDEF: Unknown actor type '%s'\n", sc.String);
			}
			smf.type = type;
			fitSrc.cls = type;
			unsigned int preParseFrames = SpriteModelFrames.Size();	// frames earlier blocks defined: what 'inherits' copies from
			FScanner::SavedPos scPos = sc.SavePos();
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("model"))
				{
					sc.MustGetNumber();
					index = sc.Number;
					if (index < 0)
					{
						sc.ScriptError("Model index must be 0 or greater in %s", type->TypeName.GetChars());
					}
					smf.modelsAmount = index + 1;
				}
			}
			//Make sure modelsAmount is at least equal to MIN_MODELS(4) to ensure compatibility with old mods
			if (smf.modelsAmount < MIN_MODELS)
			{
				smf.modelsAmount = MIN_MODELS;
			}

			const auto initArray = [](auto& array, const unsigned count, const auto value)
			{
				array.Alloc(count);
				std::fill(array.begin(), array.end(), value);
			};

			initArray(smf.modelIDs, smf.modelsAmount, -1);
			initArray(smf.skinIDs, smf.modelsAmount, FNullTextureID());
			initArray(smf.surfaceskinIDs, smf.modelsAmount * MD3_MAX_SURFACES, FNullTextureID());
			initArray(smf.animationIDs, smf.modelsAmount, -1);
			initArray(smf.modelframes, smf.modelsAmount, 0);

			sc.RestorePos(scPos);
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("path"))
				{
					sc.MustGetString();
					FixPathSeperator(sc.String);
					path = sc.String;
					if (path[(int)path.Len()-1]!='/') path+='/';
				}
				else if (sc.Compare("model"))
				{
					sc.MustGetNumber();
					index = sc.Number;
					if (index < 0)
					{
						sc.ScriptError("Model index must be 0 or greater in %s", type->TypeName.GetChars());
					}
					else if (index >= smf.modelsAmount)
					{
						sc.ScriptError("Too many models in %s", type->TypeName.GetChars());
					}
					sc.MustGetString();
					FixPathSeperator(sc.String);
					smf.modelIDs[index] = FindModel(path.GetChars(), sc.String);
					if (smf.modelIDs[index] == -1)
					{
						Printf("%s: model not found in %s\n", sc.String, path.GetChars());
					}
				}
				else if (sc.Compare("animation"))
				{
					sc.MustGetNumber();
					index = sc.Number;
					if (index < 0)
					{
						sc.ScriptError("Animation index must be 0 or greater in %s", type->TypeName.GetChars());
					}
					else if (index >= smf.modelsAmount)
					{
						sc.ScriptError("Too many models in %s", type->TypeName.GetChars());
					}
					sc.MustGetString();
					FixPathSeperator(sc.String);
					smf.animationIDs[index] = FindModel(path.GetChars(), sc.String);
					if (smf.animationIDs[index] == -1)
					{
						Printf("%s: animation model not found in %s\n", sc.String, path.GetChars());
					}
				}
				else if (sc.Compare("scale"))
				{
					sc.MustGetFloat();
					smf.xscale = sc.Float;
					sc.MustGetFloat();
					smf.yscale = sc.Float;
					sc.MustGetFloat();
					smf.zscale = sc.Float;
				}
				else if (sc.Compare("inherits"))
				{
					// inherits <class>: copies every frame another class defined in an earlier MODELDEF block, then lets
					// this block's models, skins, placement and flags override them (GZSelaco e1e266c2c5, from
					// ShinyMetagross #1487). Parse-time data only; the struct copy carries every field, placementCVars too.
					fitSrc.inherited = true;	// RS fork -- fit mode refuses a class built this way
					sc.MustGetString();
					auto type2 = PClass::FindClass(sc.String);
					if (!type2 || type2->Defaults == nullptr)
					{
						sc.ScriptError("MODELDEF: Unknown actor type '%s'\n", sc.String);
					}
					for (unsigned int i = 0; i < preParseFrames; i++)
					{
						if (SpriteModelFrames[i].type != type2) continue;
						FSpriteModelFrame frame = SpriteModelFrames[i];
						frame.type = type;
						unsigned int n = SpriteModelFrames.Push(frame);
						FSpriteModelFrame &dst = SpriteModelFrames[n];
						for (unsigned int j = 0; j < smf.modelsAmount && j < dst.modelIDs.Size() && j < dst.skinIDs.Size(); j++)
						{
							if (smf.modelIDs[j] != -1) dst.modelIDs[j] = smf.modelIDs[j];
							if (smf.skinIDs[j].isValid()) dst.skinIDs[j] = smf.skinIDs[j];
						}
						for (unsigned int j = 0; j < smf.surfaceskinIDs.Size() && j < dst.surfaceskinIDs.Size(); j++)
						{
							if (smf.surfaceskinIDs[j].isValid()) dst.surfaceskinIDs[j] = smf.surfaceskinIDs[j];
						}
						if (smf.xscale != 1.f) dst.xscale = smf.xscale;
						if (smf.yscale != 1.f) dst.yscale = smf.yscale;
						if (smf.zscale != 1.f) dst.zscale = smf.zscale;
						if (smf.xoffset != 0.f) dst.xoffset = smf.xoffset;
						if (smf.yoffset != 0.f) dst.yoffset = smf.yoffset;
						if (smf.zoffset != 0.f) dst.zoffset = smf.zoffset;
						if (smf.angleoffset != 0.f) dst.angleoffset = smf.angleoffset;
						if (smf.pitchoffset != 0.f) dst.pitchoffset = smf.pitchoffset;
						if (smf.rolloffset != 0.f) dst.rolloffset = smf.rolloffset;
						if (smf.rotationSpeed != 0.f) dst.rotationSpeed = smf.rotationSpeed;
						if (smf.xrotate != 0.f) dst.xrotate = smf.xrotate;
						if (smf.yrotate != 0.f) dst.yrotate = smf.yrotate;
						if (smf.zrotate != 0.f) dst.zrotate = smf.zrotate;
						// GZSelaco writes all three rotation-centre overrides into rotationCenterX. Kept as-is so
						// Selaco's MODELDEF data lands where it was authored against.
						if (smf.rotationCenterX != 0.f) dst.rotationCenterX = smf.rotationCenterX;
						if (smf.rotationCenterY != 0.f) dst.rotationCenterX = smf.rotationCenterY;
						if (smf.rotationCenterZ != 0.f) dst.rotationCenterX = smf.rotationCenterZ;
						dst.flags |= smf.flags;
					}
					GetDefaultByType(type)->hasmodel = true;
				}
				// [BB] Added zoffset reading.
				// Now it must be considered deprecated.
				else if (sc.Compare("zoffset"))
				{
					const int tok = sc.BytePos() - sc.StringLen;
					sc.MustGetFloat();
					smf.zoffset=sc.Float;
					ModelFit_LineSpan(sc.ScriptText(), tok, fitSrc.zoffsetStart, fitSrc.zoffsetEnd);
				}
				// [BB] PivotOffset -- the point the model TURNS ABOUT, in its own
				// space. Same three axes and the same units as Offset below, and
				// deliberately spelled to sit next to it, because the two are
				// constantly confused: Offset moves the model, PivotOffset moves
				// what it rotates around. See the field note in model.h.
				else if (sc.Compare("pivotoffset"))
				{
					sc.MustGetFloat();
					smf.pivotx = sc.Float;
					sc.MustGetFloat();
					smf.pivoty = sc.Float;
					sc.MustGetFloat();
					smf.pivotz = sc.Float;
				}
				// Offset reading.
				else if (sc.Compare("offset"))
				{
					const int tok = sc.BytePos() - sc.StringLen;
					sc.MustGetFloat();
					smf.xoffset = sc.Float;
					sc.MustGetFloat();
					smf.yoffset = sc.Float;
					sc.MustGetFloat();
					smf.zoffset = sc.Float;
					ModelFit_LineSpan(sc.ScriptText(), tok, fitSrc.offsetStart, fitSrc.offsetEnd);
				}
				// angleoffset, pitchoffset and rolloffset reading.
				else if (sc.Compare("angleoffset"))
				{
					sc.MustGetFloat();
					smf.angleoffset = sc.Float;
				}
				else if (sc.Compare("placementcvars"))
				{
					// One prefix, six CVARs by convention. Declaring them is the
					// mod's job -- a missing CVAR reads as zero, so a half-finished
					// set degrades to the MODELDEF values instead of failing.
					sc.MustGetString();
					smf.placementCVars = sc.String;
				}
				else if (sc.Compare("pitchoffset"))
				{
					sc.MustGetFloat();
					smf.pitchoffset = sc.Float;
				}
				else if (sc.Compare("rolloffset"))
				{
					sc.MustGetFloat();
					smf.rolloffset = sc.Float;
				}
				// RS FORK -- the bakeable twin of the vr_hand_* sliders. Same
				// frame, same order, same sign, so a tuned slider value can be
				// written here verbatim and mean exactly what it did live.
				// See FSpriteModelFrame in model.h for why these cannot simply
				// be folded into angleoffset/pitchoffset/rolloffset.
				else if (sc.Compare("handangleoffset"))
				{
					sc.MustGetFloat();
					smf.handangleoffset = sc.Float;
				}
				else if (sc.Compare("handpitchoffset"))
				{
					sc.MustGetFloat();
					smf.handpitchoffset = sc.Float;
				}
				else if (sc.Compare("handrolloffset"))
				{
					sc.MustGetFloat();
					smf.handrolloffset = sc.Float;
				}
				// [BB] Added model flags reading.
				else if (sc.Compare("ignoretranslation"))
				{
					smf.flags |= MDL_IGNORETRANSLATION;
				}
				else if (sc.Compare("followmainhand"))
				{
					smf.flags |= MDL_FOLLOWMAINHAND;
				}
				else if (sc.Compare("followoffhand"))
				{
					smf.flags |= MDL_FOLLOWOFFHAND;
				}
				else if (sc.Compare("pitchfrommomentum"))
				{
					smf.flags |= MDL_PITCHFROMMOMENTUM;
				}
				else if (sc.Compare("inheritactorpitch"))
				{
					smf.flags |= MDL_USEACTORPITCH | MDL_BADROTATION;
				}
				else if (sc.Compare("inheritactorroll"))
				{
					smf.flags |= MDL_USEACTORROLL;
				}
				else if (sc.Compare("useactorpitch"))
				{
					smf.flags |= MDL_USEACTORPITCH;
				}
				else if (sc.Compare("useactorroll"))
				{
					smf.flags |= MDL_USEACTORROLL;
				}
				else if (sc.Compare("noperpixellighting"))
				{
					smf.flags |= MDL_NOPERPIXELLIGHTING;
				}
				else if (sc.Compare("scaleweaponfov"))
				{
					smf.flags |= MDL_SCALEWEAPONFOV;
				}
				else if (sc.Compare("modelsareattachments"))
				{
					smf.flags |= MDL_MODELSAREATTACHMENTS;
				}
				else if (sc.Compare("viewmodelfov"))
				{
					sc.MustGetFloat();
					smf.viewModelFOV = sc.Float;
					if (smf.viewModelFOV > 0.0f)
						smf.viewModelFOV = min<float>(smf.viewModelFOV, 175.0f);
				}
				else if (sc.Compare("rotating"))
				{
					smf.flags |= MDL_ROTATING;
					smf.xrotate = 0.;
					smf.yrotate = 1.;
					smf.zrotate = 0.;
					smf.rotationCenterX = 0.;
					smf.rotationCenterY = 0.;
					smf.rotationCenterZ = 0.;
					smf.rotationSpeed = 1.;
				}
				else if (sc.Compare("fix-rotating"))
				{
					smf.flags |= MDL_FIXROTATING;
				}
				else if (sc.Compare("rotation-speed"))
				{
					sc.MustGetFloat();
					smf.rotationSpeed = sc.Float;
				}
				else if (sc.Compare("rotation-vector"))
				{
					sc.MustGetFloat();
					smf.xrotate = sc.Float;
					sc.MustGetFloat();
					smf.yrotate = sc.Float;
					sc.MustGetFloat();
					smf.zrotate = sc.Float;
				}
				else if (sc.Compare("rotation-center"))
				{
					sc.MustGetFloat();
					smf.rotationCenterX = sc.Float;
					sc.MustGetFloat();
					smf.rotationCenterY = sc.Float;
					sc.MustGetFloat();
					smf.rotationCenterZ = sc.Float;
				}
				else if (sc.Compare("interpolatedoubledframes"))
				{
					smf.flags |= MDL_INTERPOLATEDOUBLEDFRAMES;
				}
				else if (sc.Compare("nointerpolation"))
				{
					smf.flags |= MDL_NOINTERPOLATION;
				}
				else if (sc.Compare("skin"))
				{
					sc.MustGetNumber();
					index=sc.Number;
					if (index<0 || index>= smf.modelsAmount)
					{
						sc.ScriptError("Too many models in %s", type->TypeName.GetChars());
					}
					sc.MustGetString();
					FixPathSeperator(sc.String);
					if (sc.Compare(""))
					{
						smf.skinIDs[index]=FNullTextureID();
					}
					else
					{
						smf.skinIDs[index] = LoadSkin(path.GetChars(), sc.String);
						if (!smf.skinIDs[index].isValid())
						{
							Printf("Skin '%s' not found in '%s'\n",
								sc.String, type->TypeName.GetChars());
						}
					}
				}
				else if (sc.Compare("surfaceskin"))
				{
					sc.MustGetNumber();
					index = sc.Number;
					sc.MustGetNumber();
					surface = sc.Number;

					if (index<0 || index >= smf.modelsAmount)
					{
						sc.ScriptError("Too many models in %s", type->TypeName.GetChars());
					}

					if (surface<0 || surface >= MD3_MAX_SURFACES)
					{
						sc.ScriptError("Invalid MD3 Surface %d in %s", MD3_MAX_SURFACES, type->TypeName.GetChars());
					}

					sc.MustGetString();
					FixPathSeperator(sc.String);
					int ssIndex = surface + index * MD3_MAX_SURFACES;
					if (sc.Compare(""))
					{
						smf.surfaceskinIDs[ssIndex] = FNullTextureID();
					}
					else
					{
						smf.surfaceskinIDs[ssIndex] = LoadSkin(path.GetChars(), sc.String);
						if (!smf.surfaceskinIDs[ssIndex].isValid())
						{
							Printf("Surface Skin '%s' not found in '%s'\n",
								sc.String, type->TypeName.GetChars());
						}
					}
				}
				else if (sc.Compare("baseframe"))
				{
					FSpriteModelFrame *smfp = &BaseSpriteModelFrames.Insert(type, smf);
					for(int modelID : smf.modelIDs)
					{
						if(modelID >= 0)
							Models[modelID]->baseFrame = smfp;
					}
					GetDefaultByType(type)->hasmodel = true;
				}
				else if (sc.Compare("frameindex") || sc.Compare("frame"))
				{
					bool isframe=!!sc.Compare("frame");

					sc.MustGetString();
					smf.sprite = -1;
					for (int i = 0; i < (int)sprites.Size (); ++i)
					{
						if (strnicmp (sprites[i].name, sc.String, 4) == 0)
						{
							if (sprites[i].numframes==0)
							{
								//sc.ScriptError("Sprite %s has no frames", sc.String);
							}
							smf.sprite = i;
							break;
						}
					}
					if (smf.sprite==-1)
					{
						sc.ScriptError("Unknown sprite %s in model definition for %s", sc.String, type->TypeName.GetChars());
					}

					sc.MustGetString();
					FString framechars = sc.String;

					sc.MustGetNumber();
					index=sc.Number;
					if (index<0 || index>= smf.modelsAmount)
					{
						sc.ScriptError("Too many models in %s", type->TypeName.GetChars());
					}
					if (isframe)
					{
						sc.MustGetString();
						if (smf.modelIDs[index] >= 0)
						{
							FModel *model = Models[smf.modelIDs[index]];
							if (smf.animationIDs[index] >= 0)
							{
								model = Models[smf.animationIDs[index]];
							}
							smf.modelframes[index] = model->FindFrame(sc.String);
							if (smf.modelframes[index]==-1) sc.ScriptError("Unknown frame '%s' in %s", sc.String, type->TypeName.GetChars());
						}
						else smf.modelframes[index] = -1;
					}
					else
					{
						sc.MustGetNumber();
						smf.modelframes[index] = sc.Number;
					}

					for (int i = 0; i < static_cast<int>(framechars.Len()) && framechars[i] > 0; i++)
					{
						char map[29]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
						int c = toupper(framechars[i])-'A';

						if (c<0 || c>=29)
						{
							sc.ScriptError("Invalid frame character %c found", c+'A');
						}
						if (map[c]) continue;
						smf.frame=c;
						SpriteModelFrames.Push(smf);
						GetDefaultByType(type)->hasmodel = true;
						map[c]=1;
					}
				}
				else if (sc.Compare("dontcullbackfaces"))
				{
					smf.flags |= MDL_DONTCULLBACKFACES;
				}
				else if (sc.Compare("userotationcenter"))
				{
					smf.flags |= MDL_USEROTATIONCENTER;
					smf.rotationCenterX = 0.;
					smf.rotationCenterY = 0.;
					smf.rotationCenterZ = 0.;
				}
				else if (sc.Compare("correctpixelstretch"))
				{
					smf.flags |= MDL_CORRECTPIXELSTRETCH;
				}
				else if (sc.Compare("forcecullbackfaces"))
				{
					smf.flags |= MDL_FORCECULLBACKFACES;
				}
				else if (sc.Compare("followbody"))
				{
					smf.flags |= MDL_FOLLOWBODY;
				}
				else if (sc.Compare("usehandoffsets"))
				{
					smf.flags |= MDL_USEHANDOFFSETS;
				}
				else if (sc.Compare("ignoreskinalpha"))
				{
					smf.flags |= MDL_IGNORESKINALPHA;
				}
				else if (sc.Compare("noautoreverse"))
				{
					smf.flags |= MDL_NOAUTOREVERSE;
				}
				else if (sc.Compare("nopixelstretch"))
				{
					// No pixel-stretch scale, so the model can pitch and roll undistorted (GZSelaco 1e7fd30b59).
					smf.flags |= MDL_NOPIXELSTRETCH;
				}
				else
				{
					sc.ScriptMessage("Unrecognized string \"%s\"", sc.String);
				}
			}

			// RS FORK -- THE BLOCK IS CLOSED: the `}` was just consumed. Record its
			// span, the Offset it ended with, and a CRC of its exact text, so a fit
			// file can tell later whether the block it was made against has changed.
			fitSrc.blockEnd = sc.BytePos();
			fitSrc.offset = { smf.xoffset, smf.yoffset, smf.zoffset };
			{
				const FString &txt = sc.ScriptText();
				const int a = fitSrc.blockStart, b = fitSrc.blockEnd;
				if (a >= 0 && b > a && b <= (int)txt.Len())
					fitSrc.crc = CalcCRC32(std::string_view(txt.GetChars() + a, (size_t)(b - a)));
			}
			ModelFit_RecordBlock(fitSrc);
		}
		else if (sc.Compare("#include"))
		{
			sc.MustGetString();
			// This is not using sc.Open because it can print a more useful error message when done here
			int includelump = fileSystem.CheckNumForFullName(sc.String, true);
			if (includelump == -1)
			{
				if (strcmp(sc.String, "sentinel.modl") != 0) // Gene Tech mod has a broken #include statement
					sc.ScriptError("Lump '%s' not found", sc.String);
			}
			else
			{
				ParseModelDefLump(includelump);
			}
		}
	}
}

//===========================================================================
//
// FindModelFrame
//
//===========================================================================

//===========================================================================
//
// [BB] FindVoxelFrame
//
// The voxel half of the lookup, lifted out of FindModelFrameRaw so the
// per-actor override below can reach it without duplicating the walk or the
// dropped-spin rule. Deliberately does NOT consult r_drawvoxels: the two
// callers disagree about that on purpose -- the ordinary path is gated by the
// cvar, the per-actor override is not.
//
// Voxels are keyed on the SPRITE FRAME, not on a class, which is the whole
// reason a per-actor opt-in has to live outside this function.
//
//===========================================================================

FSpriteModelFrame * FindVoxelFrame(int sprite, int frame, bool dropped)
{
	if (sprite < 0 || sprite >= (int)sprites.Size()) return nullptr;

	spritedef_t *sprdef = &sprites[sprite];
	if (frame >= sprdef->numframes) return nullptr;

	spriteframe_t *sprframe = &SpriteFrames[sprdef->spriteframes + frame];
	if (sprframe->Voxel == nullptr) return nullptr;

	int index = sprframe->Voxel->VoxeldefIndex;
	if (dropped && sprframe->Voxel->DroppedSpin != sprframe->Voxel->PlacedSpin) index++;
	return &SpriteModelFrames[index];
}

//===========================================================================
//
// RS FORK -- r_voxels_mode: WHICH ACTORS THE LOOKUP HANDS A VOXEL TO
//
// The cvars live beside r_drawvoxels in r_utility.cpp. Values:
//  -1  auto (the default): 1 when r_voxelpack_loaded, else 0
//   0  all -- every actor with a voxel, the stock behaviour
//   1  only actors carrying VoxelOverride (held, grabbed, gravity-grabbed in
//      flight); everything else draws its sprite
//   2  none, not even those
//
// Auto is a sentinel inside the one cvar rather than a second archived "has
// the user touched this" flag. A flag has to be kept in step with the value it
// describes, and a menu reset would have to clear both; here "never set" and
// "reset to default" are the same value. Without a pack auto resolves to 0,
// so a build without one behaves exactly as before. r_voxelpack_loaded is set
// once at startup (R_DetectVoxelPack, sprites.cpp).
//
// Render-only: both cvars are client-side, and nothing here writes playsim
// state or draws a random number.
//
//===========================================================================

EXTERN_CVAR(Int, r_voxels_mode)
EXTERN_CVAR(Bool, r_voxelpack_loaded)

int VoxelsEffectiveMode()
{
	const int mode = r_voxels_mode;
	if (mode >= 0) return mode;
	return r_voxelpack_loaded ? 1 : 0;
}

//===========================================================================
//
// RS FORK -- SpriteFrameHasTexture: IS THERE A SPRITE TO FALL BACK TO?
//
// r_voxels_mode and r_voxeldistance both work by refusing a voxel, and a
// refused voxel draws the sprite. That is only safe when a sprite exists. A
// frame that ships only as a voxel lump has no texture -- R_InitSpriteDefs
// starts every rotation at 0xFF -- and HWSprite::Process returns before
// drawing anything, so the actor would vanish. Stock GZDoom does exactly that
// with r_drawvoxels 0.
//
// Tests what that early return tests: a valid, non-null texture. Rotation 0
// answers for the frame because R_InstallSprite fills every rotation from it
// or fails the load.
//
//===========================================================================

bool SpriteFrameHasTexture(int sprite, int frame)
{
	if (sprite < 0 || sprite >= (int)sprites.Size()) return false;

	const spritedef_t *sprdef = &sprites[sprite];
	if (frame < 0 || frame >= sprdef->numframes) return false;

	const FTextureID tid = SpriteFrames[sprdef->spriteframes + frame].Texture[0];
	if (!tid.isValid()) return false;

	auto tex = TexMan.GetGameTexture(tid, false);
	return tex != nullptr && tex->isValid();
}

// The guard itself, for every place that refuses a voxel (r_voxels_mode here,
// r_voxeldistance in hw_sprites.cpp): true when the voxel has to be kept
// because there is no sprite behind it. Logs once per sprite name. The lookup
// runs on the BSP worker threads, so the log-once takes a lock; it is only
// reached by voxel-only frames something refused, which is rare.
bool KeepVoxelWithoutSprite(int sprite, int frame)
{
	if (sprite < 0 || sprite >= (int)sprites.Size()) return false;
	if (SpriteFrameHasTexture(sprite, frame)) return false;

	static std::mutex logLock;
	static std::vector<uint8_t> logged;
	std::lock_guard<std::mutex> guard(logLock);
	if ((size_t)sprite >= logged.size()) logged.resize((size_t)sprite + 1, 0);
	if (!logged[sprite])
	{
		logged[sprite] = 1;
		char sprname[5] = { 0 };
		memcpy(sprname, sprites[sprite].name, 4);
		Printf("r_voxels_mode: %s kept as voxel, no sprite\n", sprname);
	}
	return true;
}

FSpriteModelFrame * FindModelFrameRaw(const AActor * actorDefaults, const PClass * ti, int sprite, int frame, bool dropped)
{
	if(actorDefaults->hasmodel)
	{
		FSpriteModelFrame smf;

		memset((void*)&smf, 0, sizeof(smf));
		smf.type = ti;
		smf.sprite = sprite;
		smf.frame = frame;

		int hash = SpriteModelHash[ModelFrameHash(&smf) % SpriteModelFrames.Size()];

		while (hash>=0)
		{
			FSpriteModelFrame * smff = &SpriteModelFrames[hash];
			if (smff->type == ti && smff->sprite == sprite && smff->frame == frame) return smff;
			hash = smff->hashnext;
		}
	}

	// Check for voxel replacements
	//
	// RS FORK -- r_voxels_mode: the ordinary path hands a voxel out only in
	// mode 0 ("all"), and only with r_drawvoxels on, as before. Anything else
	// draws the sprite, unless there is no sprite to draw, in which case the
	// voxel is kept rather than leaving the actor invisible
	// (KeepVoxelWithoutSprite). That guard also covers r_drawvoxels 0.
	FSpriteModelFrame *vox = FindVoxelFrame(sprite, frame, dropped);
	if (vox != nullptr)
	{
		if (r_drawvoxels && VoxelsEffectiveMode() == 0) return vox;
		if (KeepVoxelWithoutSprite(sprite, frame)) return vox;
	}

	return nullptr;
}

//===========================================================================
//
// RS FORK -- FindModelDefFrame: the MODELDEF block for (class, sprite, frame), and nothing else.
//
// FindModelFrameRaw's own hash walk, without the voxel fallback it runs after a miss. That fallback reads render cvars
// (r_drawvoxels, r_voxels_mode) and can log, so two machines in one game may get different answers from it -- right
// for drawing, wrong for a game-state question such as "which model file is model index 1 of this actor", whose
// answer can decide what a script does. The model queries in p_actionfunctions.cpp (ResolveModelForSurface's last
// fallback and the class model queries) ask this instead. FindModelFrameRaw itself is unchanged.
//
//===========================================================================

FSpriteModelFrame * FindModelDefFrame(const PClass * ti, int sprite, int frame)
{
	if (ti == nullptr || SpriteModelFrames.Size() == 0) return nullptr;
	auto def = GetDefaultByType(ti);
	if (def == nullptr || !def->hasmodel) return nullptr;

	FSpriteModelFrame smf;

	memset((void*)&smf, 0, sizeof(smf));
	smf.type = ti;
	smf.sprite = sprite;
	smf.frame = frame;

	int hash = SpriteModelHash[ModelFrameHash(&smf) % SpriteModelFrames.Size()];

	while (hash >= 0)
	{
		FSpriteModelFrame * smff = &SpriteModelFrames[hash];
		if (smff->type == ti && smff->sprite == sprite && smff->frame == frame) return smff;
		hash = smff->hashnext;
	}
	return nullptr;
}

FSpriteModelFrame * FindModelFrame(const PClass * ti, int sprite, int frame, bool dropped)
{
	auto def = GetDefaultByType(ti);

	if (def->hasmodel)
	{
		if(def->flags9 & MF9_DECOUPLEDANIMATIONS)
		{
			FSpriteModelFrame * smf = BaseSpriteModelFrames.CheckKey(ti);
			if(smf) return smf;
		}
	}

	return FindModelFrameRaw(def, ti, sprite, frame, dropped);
}

FSpriteModelFrame * FindModelFrame(const PClass * ti, bool is_decoupled, int sprite, int frame, bool dropped)
{
	if(!ti) return nullptr;

	if(is_decoupled)
	{
		return BaseSpriteModelFrames.CheckKey(ti);
	}
	else
	{
		return FindModelFrameRaw(GetDefaultByType(ti), ti, sprite, frame, dropped);
	}
}

FSpriteModelFrame * FindModelFrame(AActor * thing, int sprite, int frame, bool dropped)
{
	if(!thing) return nullptr;

	// [BB] PER-ACTOR VOXEL OVERRIDE.
	//
	// Voxels are otherwise all-or-nothing: the lookup keys on a sprite frame
	// and is gated by one global cvar, so loading a voxel pack turns EVERY
	// actor that has one into a voxel, everywhere, with no way to ask for it
	// on a single object. VoxelOverride is that way.
	//
	// This is the hook for "a thing you are physically holding becomes a real
	// 3D object". A billboard cannot be turned over in your hand -- it always
	// faces you -- so a grabbed item wants to be a voxel for exactly as long
	// as it is held, and a sprite again the moment it is dropped. Set the
	// field on grab, clear it on release.
	//
	// TWO DELIBERATE DIFFERENCES from the ordinary path, both of which are the
	// point of the feature rather than oversights:
	//
	//   IT IGNORES r_drawvoxels. That cvar means "draw voxels for everything",
	//   and the case this exists for is a pack loaded with it switched OFF.
	//   Gating the override on it would make the feature unreachable in the
	//   exact configuration it was built for.
	//
	//   IT OUTRANKS A MODEL. Ordinarily a model wins and the voxel is only a
	//   fallback (see FindModelFrameRaw). Here the caller has explicitly asked
	//   for the voxel on this one actor, so it takes precedence -- otherwise
	//   anything carrying a MODELDEF could never be overridden, which includes
	//   most of what a mod would want to pick up.
	//
	// Falls through when the actor has no voxel for its current frame, so
	// setting the field on something without one costs a null check and
	// changes nothing.
	if (thing->VoxelOverride)
	{
		FSpriteModelFrame *vox = FindVoxelFrame(sprite, frame, dropped);

		// RS FORK -- r_voxels_mode 2 ("none") REFUSES THE OVERRIDE TOO.
		//
		// Mode 1 is exactly this path, so it needs nothing. Refused here rather
		// than by asking mods not to set the flag, so no mod has to know the
		// mode: RS_Pull sets VoxelOverride on every grab. A voxel with no
		// sprite behind it is kept even in mode 2 (KeepVoxelWithoutSprite).
		const bool refusedByMode = vox != nullptr && VoxelsEffectiveMode() == 2 && !KeepVoxelWithoutSprite(sprite, frame);

		// [BB] REPORT THE MISS, NOT JUST THE HIT.
		//
		// The first cut of this trace lived in ObjectToWorldMatrix, which only
		// ever runs on something that has ALREADY resolved to a voxel -- so the
		// one outcome worth knowing about, "asked for a voxel and there is not
		// one", printed nothing at all and read exactly like the trace being
		// broken. This is the decision itself: what was asked for, by which
		// sprite and frame, and whether the pack answered.
		if (vr_voxel_debug)
		{
			static const AActor *lastActor = nullptr;
			static int lastTic = -1000;
			if (thing != lastActor || (gametic - lastTic) > TICRATE)
			{
				lastActor = thing;
				lastTic = gametic;
				char sprname[5] = { 0 };
				if (sprite >= 0 && sprite < (int)sprites.Size())
					memcpy(sprname, sprites[sprite].name, 4);
				Printf("[RSVOX] %s  sprite=%s frame=%d dropped=%d  ->  %s\n",
					thing->GetClass()->TypeName.GetChars(),
					sprname, frame, (int)dropped,
					vox ? (refusedByMode ? "VOXEL FOUND, refused by r_voxels_mode 2" : "VOXEL FOUND") : "no voxel for this frame");
			}
		}

		if (vox != nullptr && !refusedByMode) return vox;	// RS fork -- r_voxels_mode 2
	}

	return FindModelFrame((thing->modelData != nullptr && thing->modelData->modelDef != nullptr) ? thing->modelData->modelDef : thing->GetClass(), (thing->flags9 & MF9_DECOUPLEDANIMATIONS), sprite, frame, dropped);
}

//===========================================================================
//
// IsHUDModelForPlayerAvailable
//
//===========================================================================

bool IsHUDModelForPlayerAvailable (player_t * player)
{
	if (player == nullptr || player->psprites == nullptr)
		return false;

	// [MK] check that at least one psprite uses models
	for (DPSprite *psp = player->psprites; psp != nullptr && psp->GetID() < PSP_TARGETCENTER; psp = psp->GetNext())
	{
		if ( FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false) != nullptr ) return true;
	}
	return false;
}


unsigned int FSpriteModelFrame::getFlags(class DActorModelData * defs) const
{
	return (defs && defs->flags & MODELDATA_OVERRIDE_FLAGS)? (flags | defs->overrideFlagsSet) & ~(defs->overrideFlagsClear) : flags;
}

//===========================================================================
//
// RS FORK -- what surfaces does a model have, and what are they called.
//
// Per-surface frame addressing (p_pspr.h) is driven by SURFACE INDEX, because
// a third of the donor library names its surfaces `Cube`, `Untitled` or
// `pCylinder10` and several models repeat a name. An index is unambiguous;
// a name is not. But an index is also unguessable, so this prints the pairing
// and a part map can be written from it.
//
// Every LOADED model rather than just the one in your hands: the answer does
// not change with what is equipped, models are loaded on demand so the list is
// already scoped to what this session has actually touched, and reaching the
// held weapon's FSpriteModelFrame from here would duplicate the render path's
// own lookup for no gain.
//
//===========================================================================

CCMD(modelsurfaces)
{
	const char* filter = (argv.argc() > 1) ? argv[1] : nullptr;
	int shown = 0;

	for (unsigned m = 0; m < Models.Size(); m++)
	{
		FModel* mdl = Models[m];
		if (!mdl) continue;

		int ns = mdl->GetSurfaceCount();
		if (ns <= 0) continue;   // formats with no surface concept

		const char* fn = mdl->mFileName.GetChars();
		if (filter && !strstr(fn, filter)) continue;

		shown++;
		Printf(TEXTCOLOR_GOLD "%s" TEXTCOLOR_NORMAL "  (%d surfaces)\n", fn, ns);
		for (int s = 0; s < ns; s++)
		{
			FName sn = mdl->GetSurfaceName(s);
			Printf("    surface %2d  %s\n", s, sn == NAME_None ? "(unnamed)" : sn.GetChars());
		}
	}

	if (!shown)
	{
		if (filter) Printf("no loaded model matches \"%s\"\n", filter);
		else        Printf("no surfaced models loaded yet -- models load on demand, so equip the weapon first\n");
	}
}
