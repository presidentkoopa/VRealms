/*
** a_dynlight.h
**
** Implements actors representing dynamic lights (hardware independent)
**
**---------------------------------------------------------------------------
**
** Copyright 2003 Timothy Stump
** Copyright 2004-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once
#include "c_cvars.h"
#include "actor.h"
#include "cycler.h"
#include "g_levellocals.h"

EXTERN_CVAR(Bool, r_dynlights)
EXTERN_CVAR(Bool, gl_lights)
EXTERN_CVAR(Float, gl_light_distance_cull)
EXTERN_CVAR(Float, gl_light_max_intensity)
EXTERN_CVAR(Int, gl_light_max_collected_subsectors)

struct side_t;
struct seg_t;

class FSerializer;
struct FSectionLine;

enum ELightType
{
	PointLight,
	PulseLight,
	FlickerLight,
	RandomFlickerLight,
	SectorLight,
	DummyLight,
	ColorPulseLight,
	ColorFlickerLight,
	RandomColorFlickerLight
};

enum
{
	LIGHT_RED = 0,
	LIGHT_GREEN = 1,
	LIGHT_BLUE = 2,
	LIGHT_INTENSITY = 3,
	LIGHT_SECONDARY_INTENSITY = 4,
};

enum LightFlag
{
	LF_SUBTRACTIVE = 1,
	LF_ADDITIVE = 2,
	LF_DONTLIGHTSELF = 4,
	LF_ATTENUATE = 8,
	LF_NOSHADOWMAP = 16,
	LF_DONTLIGHTACTORS = 32,
	LF_SPOT = 64,
	LF_DONTLIGHTOTHERS = 128,
	LF_DONTLIGHTMAP = 256,
	// [LIGHTSHADOWS] This light asks to cast shadows. Content sets it on the lights whose shadows matter -- a weapon's muzzle
	// flash, a key lamp -- through A_AttachLight's flags, +DYNAMICLIGHT.CASTSHADOW or GLDEFS "castshadow 1". The player's
	// cast-shadow setting then decides what they cost: gl_light_castshadows 0 Off (they cast none, whatever else is on),
	// 1 shadow maps, 2 ray traced (hw_shadowmap.cpp, IShadowMap::LightShadowAllowed). Every other light keeps "Light shadows"
	// (gl_light_shadowmap). LF_NOSHADOWMAP still wins: such a light is never shadowable. Read by the renderer only -- it
	// decides which lights get a shadow-map row, never anything in the playsim.
	LF_CASTSHADOW = 512
};

typedef TFlags<LightFlag> LightFlags;
DEFINE_TFLAGS_OPERATORS(LightFlags)

//==========================================================================
//
// Light definitions
//
//==========================================================================
class FLightDefaults
{
public:
	FLightDefaults(FName name, ELightType type = PointLight)
	{
		m_Name = name;
		m_type = type;
	}

	void ApplyProperties(FDynamicLight * light) const;
	FName GetName() const { return m_Name; }
	void SetParameter(double p) { m_Param = p; }
	void SetArg(int arg, int val) { m_Args[arg] = val; }
	int GetArg(int arg) { return m_Args[arg]; }
	uint8_t GetAttenuate() const { return m_attenuate; }
	void SetOffset(float* ft) { m_Pos.X = ft[0]; m_Pos.Z = ft[1]; m_Pos.Y = ft[2]; }
	void SetSubtractive(bool subtract) { if (subtract) m_lightFlags |= LF_SUBTRACTIVE; else m_lightFlags &= ~LF_SUBTRACTIVE; }
	void SetAdditive(bool add) { if (add) m_lightFlags |= LF_ADDITIVE; else m_lightFlags &= ~LF_ADDITIVE; }
	void SetDontLightSelf(bool add) { if (add) m_lightFlags |= LF_DONTLIGHTSELF; else m_lightFlags &= ~LF_DONTLIGHTSELF; }
	void SetAttenuate(bool on) { m_attenuate = on; if (on) m_lightFlags |= LF_ATTENUATE; else m_lightFlags &= ~LF_ATTENUATE; }
	void SetDontLightActors(bool on) { if (on) m_lightFlags |= LF_DONTLIGHTACTORS; else m_lightFlags &= ~LF_DONTLIGHTACTORS; }
	void SetDontLightOthers(bool on) { if (on) m_lightFlags |= LF_DONTLIGHTOTHERS; else m_lightFlags &= ~LF_DONTLIGHTOTHERS; }
	void SetDontLightMap(bool on) { if (on) m_lightFlags |= LF_DONTLIGHTMAP; else m_lightFlags &= ~LF_DONTLIGHTMAP; }
	void SetNoShadowmap(bool on) { if (on) m_lightFlags |= LF_NOSHADOWMAP; else m_lightFlags &= ~LF_NOSHADOWMAP; }
	void SetCastShadow(bool on) { if (on) m_lightFlags |= LF_CASTSHADOW; else m_lightFlags &= ~LF_CASTSHADOW; }	// [LIGHTSHADOWS] GLDEFS "castshadow"
	void SetLightDefIntensity(double i) { m_LightDefIntensity = i; }
	void SetSpot(bool spot) { if (spot) m_lightFlags |= LF_SPOT; else m_lightFlags &= ~LF_SPOT; }
	void SetSpotInnerAngle(double angle) { m_spotInnerAngle = DAngle::fromDeg(angle); }
	void SetSpotOuterAngle(double angle) { m_spotOuterAngle = DAngle::fromDeg(angle); }
	void SetSpotPitch(double pitch)
	{
		m_pitch = DAngle::fromDeg(pitch);
		m_explicitPitch = true;
	}
	void UnsetSpotPitch()
	{
		m_pitch = nullAngle;
		m_explicitPitch = false;
	}

	// [round2 B1] The pose anchor for the light this definition builds. Kept on
	// the DEFINITION, not only on the FDynamicLight, because A_AttachLight only
	// marks the actor for light recreation -- no FDynamicLight exists until the
	// next tick -- and every recreate (state change, savegame load, gl_lights
	// toggle) builds the light from here again. See FDynamicLight::PoseAnchor.
	void SetPoseAnchor(int anchor, const DVector3 &offset) { m_poseAnchor = anchor; m_poseAnchorOffset = offset; }

	void SetType(ELightType type) { m_type = type; }
	void CopyFrom(const FLightDefaults &other)
	{
		auto n = m_Name;
		// [round2 B1] The anchor belongs to the light ID the actor named, not
		// to the GLDEFS definition being copied in, so A_AttachLightDef keeps it.
		auto anchor = m_poseAnchor;
		auto anchorOffset = m_poseAnchorOffset;
		*this = other;
		m_Name = n;
		m_poseAnchor = anchor;
		m_poseAnchorOffset = anchorOffset;
	}
	void SetFlags(LightFlags lf)
	{
		m_lightFlags = lf;
		m_attenuate = !!(m_lightFlags & LF_ATTENUATE);
	}
	static void SetAttenuationForLevel(bool);

	void OrderIntensities()
	{
		if (m_Args[LIGHT_INTENSITY] > m_Args[LIGHT_SECONDARY_INTENSITY])
		{
			std::swap(m_Args[LIGHT_INTENSITY], m_Args[LIGHT_SECONDARY_INTENSITY]);
			m_swapped = true;
		}
	}

protected:
	FName m_Name = NAME_None;
	int m_Args[5] = { 0,0,0,0,0 };
	double m_Param = 0;
	DVector3 m_Pos = { 0,0,0 };
	int m_type;
	int8_t m_attenuate = -1;
	LightFlags m_lightFlags = 0;
	bool m_swapped = false;
	bool m_spot = false;
	bool m_explicitPitch = false;
	DAngle m_spotInnerAngle = DAngle::fromDeg(10.0);
	DAngle m_spotOuterAngle = DAngle::fromDeg(25.0);
	DAngle m_pitch = nullAngle;
	double m_LightDefIntensity = 1.0; // Light over/underbright multiplication for GLDEFS-defined lights
	int m_poseAnchor = 0;                     // [round2 B1] see SetPoseAnchor
	DVector3 m_poseAnchorOffset = { 0,0,0 };  // [round2 B1]

	friend FSerializer &Serialize(FSerializer &arc, const char *key, FLightDefaults &value, FLightDefaults *def);
};

FSerializer &Serialize(FSerializer &arc, const char *key, TDeletingArray<FLightDefaults *> &value, TDeletingArray<FLightDefaults *> *def);

//==========================================================================
//
// Light associations (intermediate parser data)
//
//==========================================================================

class FLightAssociation
{
public:
	//FLightAssociation();
	FLightAssociation(FName actorName, const char *frameName, FName lightName)
		: m_ActorName(actorName), m_AssocLight(lightName)
	{
		strncpy(m_FrameName, frameName, 8);
	}

	FName ActorName() { return m_ActorName; }
	const char *FrameName() { return m_FrameName; }
	FName Light() { return m_AssocLight; }
	void ReplaceLightName(FName newName) { m_AssocLight = newName; }
protected:
	char m_FrameName[8];
	FName m_ActorName, m_AssocLight;
};


//==========================================================================
//
// Light associations per actor class
//
//==========================================================================

class FInternalLightAssociation
{
public:
	FInternalLightAssociation(FLightAssociation * asso);
	int Sprite() const { return m_sprite; }
	int Frame() const { return m_frame; }
	const FLightDefaults *Light() const { return m_AssocLight; }
protected:
	int m_sprite;
	int m_frame;
	FLightDefaults * m_AssocLight;
};


struct FLightNode
{
	FDynamicLight * lightsource;
};

struct FDynamicLightTouchLists
{
	TArray<FSection*> flat_tlist;
	TArray<side_t*> wall_tlist;
};

struct FDynamicLight
{
	friend class FLightDefaults;

	inline DVector3 PosRelative(int portalgroup) const
	{
		return Pos + Level->Displacements.getOffset(Sector->PortalGroup, portalgroup);
	}

	bool ShouldLightActor(AActor *check)
	{
		return visibletoplayer && IsActive() &&
				(!((*pLightFlags) & LF_DONTLIGHTSELF) || target != check) &&
				(!((*pLightFlags) & LF_DONTLIGHTOTHERS) || target == check) &&
				(!((*pLightFlags) & LF_DONTLIGHTACTORS));
	}

	void SetOffset(const DVector3 &pos)
	{
		m_off = pos;
	}


	bool IsActive() const { return m_active; }
	float GetRadius() const { return (IsActive() ? m_currentRadius * 2.f : 0.f); }
	int GetRed() const { return pArgs[LIGHT_RED]; }
	int GetGreen() const { return pArgs[LIGHT_GREEN]; }
	int GetBlue() const { return pArgs[LIGHT_BLUE]; }
	int GetIntensity() const { return pArgs[LIGHT_INTENSITY]; }
	int GetSecondaryIntensity() const { return pArgs[LIGHT_SECONDARY_INTENSITY]; }
	double GetLightDefIntensity() const { return lightDefIntensity; }
	int GetTimer() const { return Level->LocalWorldTimer; }

	bool IsSubtractive() const { return !!((*pLightFlags) & LF_SUBTRACTIVE); }
	bool IsAdditive() const { return !!((*pLightFlags) & LF_ADDITIVE); }
	bool IsSpot() const { return !!((*pLightFlags) & LF_SPOT); }
	bool IsAttenuated() const { return !!((*pLightFlags) & LF_ATTENUATE); }
	bool DontShadowmap() const { return !!((*pLightFlags) & LF_NOSHADOWMAP); }
	bool CastShadow() const { return !!((*pLightFlags) & LF_CASTSHADOW); }	// [LIGHTSHADOWS] asks to cast shadows: see LF_CASTSHADOW
	bool DontLightSelf() const { return !!((*pLightFlags) & (LF_DONTLIGHTSELF|LF_DONTLIGHTACTORS)); }	// dontlightactors implies dontlightself.
	bool DontLightActors() const { return !!((*pLightFlags) & LF_DONTLIGHTACTORS); }
	bool DontLightOthers() const { return !!((*pLightFlags) & (LF_DONTLIGHTOTHERS)); }
	bool DontLightMap() const { return !!((*pLightFlags) & (LF_DONTLIGHTMAP)); }
	void Deactivate() { m_active = false; }
	void Activate();

	void SetActor(AActor *ac, bool isowned) { target = ac; owned = isowned; }
	double X() const { return Pos.X; }
	double Y() const { return Pos.Y; }
	double Z() const { return Pos.Z; }

	void Tick();
	void UpdateLocation();
	void AddLightNode(FSection *section, side_t *sidedef);
	void LinkLight();
	void UnlinkLight();
	void ReleaseLight();

private:
	double DistToSeg(const DVector3 &pos, vertex_t *start, vertex_t *end);
	void CollectWithinRadius(const DVector3 &pos, FSection *section, float radius);

public:
	FCycler m_cycler;
	DVector3 Pos;
	DVector3 m_off;

	// This date can either come from the owning actor or from a light definition
	// To avoid having to copy these around every tic, these are pointers to the source data.
	const DAngle *pSpotInnerAngle;
	const DAngle *pSpotOuterAngle;
	const int *pArgs;
	const LightFlags *pLightFlags;

	double specialf1;
	FDynamicLight *next, *prev;
	sector_t *Sector;
	FLevelLocals *Level;
	TObjPtr<AActor *> target;
	DAngle            Yaw, Pitch;

	float radius;			// The maximum size the light can be with its current settings.
	float m_currentRadius;	// The current light size.
	int m_tickCount;
	int m_lastUpdate;
	int mShadowmapIndex;
	unsigned int mDistanceCullViewId;
	unsigned int mModelLightGatherId;
	unsigned int mSpotCacheViewId;
	unsigned int mPosRelativeCacheViewId;
	int mPosRelativeCacheGroup;
	bool mDistanceCullResult;
	float mSpotInnerCos;
	float mSpotOuterCos;
	float mSpotDirX;
	float mSpotDirY;
	float mSpotDirZ;
	DVector3 mPosRelativeCache;
	bool m_active;
	bool visibletoplayer;
	bool shadowmapped;
	uint8_t lighttype;
	bool owned;
	bool swapped;
	bool explicitpitch;

	// [round2 B1] HELD IN A TRACKED POSE, re-posed every frame.
	//
	//   PoseAnchor        0 none (target + m_off, as always), 1 main hand,
	//                     2 off hand, 3 head -- SetVolumetricBeamAnchor's ids
	//   PoseAnchorOffset  (forward, right, up) map units in the pose's frame
	//
	// A light is posed only in the tick (Tick -> UpdateLocation), so a torch
	// held in a tracked hand stepped at 35Hz against a 90Hz+ hand. Anchored, the
	// tick poses it from the hand too (so tic relinks follow the hand, not the
	// target), and hw_entrypoint.cpp re-poses it every frame from the pose the
	// VR backend wrote that frame (R_UpdatePoseAnchoredLights).
	//
	// While anchored the pose supplies position, yaw AND pitch. m_off, the
	// bob, the floor/ceiling clamp and explicitpitch are all ignored:
	// A_AttachLight's default spotp of 0 is an explicit pitch and would
	// otherwise pin a hand torch level.
	//
	// Anchored lights are listed in FLevelLocals::PoseAnchoredLights so the
	// frame step never walks every light. PoseRegistered says this one is in
	// it; ReleaseLight takes it out before the memory returns to the free list.
	int      PoseAnchor = 0;
	DVector3 PoseAnchorOffset = { 0, 0, 0 };
	int      PoseSource = 0;          // last ETrackedPoseSource, for the log line
	bool     PoseRegistered = false;
	// Where LinkLight last built the section light lists from. The frame step
	// relinks only once the pose has moved a few units away from it.
	DVector3 LinkedPos = { 0, 0, 0 };

	void SetPoseAnchor(int anchor, const DVector3 &offset);
	bool ResolvePoseAnchor();   // false = not anchored or no pose; nothing written

	double lightDefIntensity;

	FDynamicLightTouchLists touchlists;
};

//==========================================================================
//
// [LIGHTSHADOWS] Renderer side, defined in hw_dynlightdata.cpp: whether the lights that ask to cast shadows (LF_CASTSHADOW)
// want the shadow-map pass this frame -- the cast-shadow setting on and one of them live, or one was within the last 10
// seconds; or the setting reaches all lights. RenderViewpoint asks once a frame, main view only. False at once while the
// setting is Off.
//
//==========================================================================

bool DynamicLightShadowRowsWanted(FLevelLocals *Level);
