/*
** hw_dynlightdata.cpp
**
** dynamic light application
**
**---------------------------------------------------------------------------
**
** Copyright 2002-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "actorinlines.h"
#include "a_dynlight.h"
#include "hw_dynlightdata.h"
#include"hw_cvars.h"
#include "v_video.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hw_shadowmap.h"	// [LIGHTSHADOWS] IShadowMap::CastShadowsOn, CastShadowsReachAllLights
#include "i_time.h"			// [LIGHTSHADOWS] I_msTime

// If we want to share the array to avoid constant allocations it needs to be thread local unless it'd be littered with expensive synchronization.
thread_local FDynLightData lightdata;
unsigned int gl_dynlight_viewid = 1;

//==========================================================================
//
// Light related CVARs
//
//==========================================================================

// These shouldn't be called 'gl...' anymore...
CVAR (Bool, gl_light_sprites, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR (Bool, gl_light_particles, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR (Bool, gl_light_weapons, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR (Bool, gl_light_distance_cull_cache, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR (Bool, gl_light_model_dedupe_cache, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR (Bool, gl_light_spot_cache, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR (Bool, gl_light_pos_relative_cache, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);


//==========================================================================
//
// Sets up the parameters to render one dynamic light onto one plane
//
//==========================================================================
bool GetLight(FDynLightData& dld, int group, Plane & p, FDynamicLight * light, bool checkside)
{
	DVector3 pos = gl_GetLightPosRelative(light, group);
	float radius = (light->GetRadius());

	auto dist = fabs(p.DistToPoint((float)pos.X, (float)pos.Z, (float)pos.Y));

	if (radius <= 0.f) return false;
	if (dist > radius) return false;
	if (checkside && p.PointOnSide((float)pos.X, (float)pos.Z, (float)pos.Y))
	{
		return false;
	}

	AddLightToList(dld, group, light, false);
	return true;
}

//==========================================================================
//
// Add one dynamic light to the light data list
//
//==========================================================================
static inline void GetSpotlightShaderParams(FDynamicLight *light, float &spotInnerAngle, float &spotOuterAngle, float &spotDirX, float &spotDirY, float &spotDirZ)
{
	if (!gl_light_spot_cache)
	{
		spotInnerAngle = (float)light->pSpotInnerAngle->Cos();
		spotOuterAngle = (float)light->pSpotOuterAngle->Cos();

		DAngle negPitch = -light->Pitch;
		DAngle Angle = light->Yaw;
		double xzLen = negPitch.Cos();
		spotDirX = float(-Angle.Cos() * xzLen);
		spotDirY = float(-negPitch.Sin());
		spotDirZ = float(-Angle.Sin() * xzLen);
		return;
	}

	if (light->mSpotCacheViewId != gl_dynlight_viewid)
	{
		light->mSpotInnerCos = (float)light->pSpotInnerAngle->Cos();
		light->mSpotOuterCos = (float)light->pSpotOuterAngle->Cos();

		DAngle negPitch = -light->Pitch;
		DAngle Angle = light->Yaw;
		double xzLen = negPitch.Cos();
		light->mSpotDirX = float(-Angle.Cos() * xzLen);
		light->mSpotDirY = float(-negPitch.Sin());
		light->mSpotDirZ = float(-Angle.Sin() * xzLen);
		light->mSpotCacheViewId = gl_dynlight_viewid;
	}

	spotInnerAngle = light->mSpotInnerCos;
	spotOuterAngle = light->mSpotOuterCos;
	spotDirX = light->mSpotDirX;
	spotDirY = light->mSpotDirY;
	spotDirZ = light->mSpotDirZ;
}

void AddLightToList(FDynLightData &dld, int group, FDynamicLight * light, bool forceAttenuate)
{
	int i = 0;

	DVector3 pos = gl_GetLightPosRelative(light, group);
	float radius = light->GetRadius();

	float cs;
	if (light->IsAdditive())
	{
		cs = 0.2f;
		i = 2;
	}
	else
	{
		cs = 1.0f;
	}

	if (light->target && (light->target->renderflags2 & RF2_LIGHTMULTALPHA))
		cs *= (float)light->target->Alpha;

	// Multiply intensity from GLDEFS
	cs *= (float)light->GetLightDefIntensity();

	float r = light->GetRed() / 255.0f * cs;
	float g = light->GetGreen() / 255.0f * cs;
	float b = light->GetBlue() / 255.0f * cs;

	if (light->IsSubtractive())
	{
		DVector3 v(r, g, b);
		float length = (float)v.Length();

		r = length - r;
		g = length - g;
		b = length - b;
		i = 1;
	}

	float shadowIndex;
	if (screen->mShadowMap.Enabled()) // note: with shadowmaps switched off, we cannot rely on properly set indices anymore.
	{
		shadowIndex = light->mShadowmapIndex + 1.0f;
	}
	else shadowIndex = 1025.f;
	// Store attenuate flag in the sign bit of the float.
	if (light->IsAttenuated() || forceAttenuate) shadowIndex = -shadowIndex;

	float lightType = 0.0f;
	float spotInnerAngle = 0.0f;
	float spotOuterAngle = 0.0f;
	float spotDirX = 0.0f;
	float spotDirY = 0.0f;
	float spotDirZ = 0.0f;
	if (light->IsSpot())
	{
		lightType = 1.0f;
		GetSpotlightShaderParams(light, spotInnerAngle, spotOuterAngle, spotDirX, spotDirY, spotDirZ);
	}

	float *data = &dld.arrays[i][dld.arrays[i].Reserve(16)];
	data[0] = float(pos.X);
	data[1] = float(pos.Z);
	data[2] = float(pos.Y);
	data[3] = radius;
	data[4] = r;
	data[5] = g;
	data[6] = b;
	data[7] = shadowIndex;
	data[8] = spotDirX;
	data[9] = spotDirY;
	data[10] = spotDirZ;
	data[11] = lightType;
	data[12] = spotInnerAngle;
	data[13] = spotOuterAngle;
	data[14] = 0.0f; // unused
	data[15] = 0.0f; // unused
}

//==========================================================================
//
// [LIGHTSHADOWS] Whether the lights that ask to cast shadows (LF_CASTSHADOW) want the shadow-map pass this frame. Declared
// in a_dynlight.h; RenderViewpoint runs the pass when this or "Light shadows" says so, for a level with dynamic lights.
// The shadow index above then reads the row CollectLights gave -- or 1025, none, as before.
//
// With the cast-shadow setting on: while one of them is live (active, and its flood found a wall that can shadow it), and
// for 10 SECONDS after. Between shots a flash is gone for a few tics, and each time the pass stops RenderViewpoint takes the
// level's AABB tree away from the shadow map; handing it back re-uploads the whole tree -- a hitch at every shot otherwise
// (the effect lights' pass lingers for the same reason). A lingering pass has only empty rows, and every texel early-outs.
// Reaching all lights, it wants the pass whenever the level has dynamic lights, as "Light shadows" does. Off: false at
// once, and the linger is forgotten.
//
// Main view only, once a frame. Reads light state the renderer already reads; writes only its own clock.
//
//==========================================================================

static uint64_t CastShadowWantedMs = 0;

bool DynamicLightShadowRowsWanted(FLevelLocals *Level)
{
	if (Level == nullptr || !IShadowMap::CastShadowsOn())
	{
		CastShadowWantedMs = 0;
		return false;
	}
	if (IShadowMap::CastShadowsReachAllLights())
		return true;

	bool wanted = false;
	for (auto light = Level->lights; light; light = light->next)
	{
		if (light->IsActive() && light->shadowmapped && light->CastShadow())
		{
			wanted = true;
			break;
		}
	}

	static const uint64_t kLingerMs = 10000;
	const uint64_t nowMs = I_msTime();
	if (wanted)
		CastShadowWantedMs = nowMs;
	return wanted || (CastShadowWantedMs != 0 && nowMs - CastShadowWantedMs < kLingerMs);
}
