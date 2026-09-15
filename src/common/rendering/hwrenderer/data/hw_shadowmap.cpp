/*
** hw_shadowmap.cpp
**
** 1D dynamic shadow maps (API independent part)
**
**---------------------------------------------------------------------------
**
** Copyright 2017 Magnus Norddahl
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "hw_shadowmap.h"
#include "hw_cvars.h"
#include "hw_dynlightdata.h"
#include "buffers.h"
#include "shaderuniforms.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"

/*
	The 1D shadow maps are stored in a 1024x1024 texture as float depth values (R32F).

	Each line in the texture is assigned to a single light. For example, to grab depth values for light 20
	the fragment shader (main.fp) needs to sample from row 20. That is, the V texture coordinate needs
	to be 20.5/1024.

	The texel row for each light is split into four parts. One for each direction, like a cube texture,
	but then only in 2D where this reduces itself to a square. When main.fp samples from the shadow map
	it first decides in which direction the fragment is (relative to the light), like cubemap sampling does
	for 3D, but once again just for the 2D case.

	Texels 0-255 is Y positive, 256-511 is X positive, 512-767 is Y negative and 768-1023 is X negative.

	Generating the shadow map itself is done by FShadowMap::Update(). The shadow map texture's FBO is
	bound and then a screen quad is drawn to make a fragment shader cover all texels. For each fragment
	it shoots a ray and collects the distance to what it hit.

	The shadowmap.fp shader knows which light and texel it is processing by mapping gl_FragCoord.y back
	to the light index, and it knows which direction to ray trace by looking at gl_FragCoord.x. For
	example, if gl_FragCoord.y is 20.5, then it knows its processing light 20, and if gl_FragCoord.x is
	127.5, then it knows we are shooting straight ahead for the Y positive direction.

	Ray testing is done by uploading two GPU storage buffers - one holding AABB tree nodes, and one with
	the line segments at the leaf nodes of the tree. The fragment shader then performs a test same way
	as on the CPU, except everything uses indexes as pointers are not allowed in GLSL.
*/

cycle_t IShadowMap::UpdateCycles;
int IShadowMap::LightsProcessed;
int IShadowMap::LightsShadowmapped;
int IShadowMap::LightsCastShadow;		// [LIGHTSHADOWS]
uint64_t IShadowMap::UpdateSerial;		// [LIGHTSHADOWS]
bool IShadowMap::RaytracedThisSession;	// [LIGHTSHADOWS]

CVAR(Bool, gl_light_shadowmap, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

//==========================================================================
//
// [LIGHTSHADOWS] THE CAST-SHADOW SETTING ("Engine docs/EFFECT_LIGHTS_LE_IMPL_NOTES.md"; plan LIGHTS_20_21_22_PLAN.md
// section 5 and the owner's answer 2). One ladder for the lights that ask to cast shadows (LF_CASTSHADOW, a_dynlight.h:
// a mod's muzzle flashes, or any light content marks), beside "Light shadows" above, which keeps every other light exactly
// as it always was.
//
//   gl_light_castshadows       0 Off (the default): lights that ask cast no shadow, whatever else is on.
//                              1 Shadow maps: they take the FIRST shadow-map rows (CollectLights, hw_entrypoint.cpp), with
//                                or without "Light shadows"; walls -- one-sided lines -- cast. The map pass runs once a
//                                frame, both eyes sharing it, while one of them is live and for 10 seconds after
//                                (DynamicLightShadowRowsWanted, hw_dynlightdata.cpp).
//                              2 Ray traced: as 1, and the scene shaders ray trace light shadows from the NEXT START on.
//                                The Vulkan device reads it once, as it reads vk_raytrace (VulkanRenderDevice::
//                                RaytracingEnabled). While ray traced, every light with a row is traced -- lamps under
//                                "Light shadows" too: floors, ledges and 3D objects cast, and the level is where it was at
//                                map start (doors, lifts). Without ray queries, or on OpenGL, it stays shadow maps.
//   gl_light_shadowmap_lights  what that setting reaches: 0 the lights that ask (the default); 1 all lights -- every other
//                              light the map can shadow casts by it too, on top of "Light shadows", never less.
//
// Both are read by the renderer every frame (the ray-traced step aside). They are client settings: they change which lights
// get a row, never the playsim, so each machine in a network game chooses its own.
//
//==========================================================================

CUSTOM_CVARD(Int, gl_light_castshadows, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "shadows for lights that ask to cast them: 0 off, 1 shadow maps, 2 ray traced (from the next start)")
{
	if (self < IShadowMap::CASTSHADOWS_OFF || self > IShadowMap::CASTSHADOWS_RAYTRACED) self = IShadowMap::CASTSHADOWS_OFF;
}

CUSTOM_CVARD(Int, gl_light_shadowmap_lights, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "which lights gl_light_castshadows reaches: 0 lights that ask, 1 all lights")
{
	if (self < IShadowMap::SHADOWLIGHTS_ASKING || self > IShadowMap::SHADOWLIGHTS_ALL) self = IShadowMap::SHADOWLIGHTS_ASKING;
}

bool IShadowMap::CastShadowsOn()
{
	return gl_light_castshadows >= CASTSHADOWS_SHADOWMAP;
}

bool IShadowMap::CastShadowsRaytraced()
{
	return gl_light_castshadows == CASTSHADOWS_RAYTRACED;
}

bool IShadowMap::CastShadowsReachAllLights()
{
	return CastShadowsOn() && gl_light_shadowmap_lights == SHADOWLIGHTS_ALL;
}

bool IShadowMap::LightShadowAllowed(bool asksToCast)
{
	if (asksToCast)
		return CastShadowsOn();
	return gl_light_shadowmap || CastShadowsReachAllLights();
}

ADD_STAT(shadowmap)
{
	FString out;
	// [LIGHTSHADOWS] asked = the rows of lights that ask to cast shadows
	out.Format("upload=%04.2f ms  lights=%d  shadowmapped=%d  asked=%d", IShadowMap::UpdateCycles.TimeMS(), IShadowMap::LightsProcessed, IShadowMap::LightsShadowmapped, IShadowMap::LightsCastShadow);
	return out;
}

CUSTOM_CVAR(Int, gl_shadowmap_quality, 128, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // default to 128 for VR
{
	switch (self)
	{
	case 2<<6: // 128
	case 2<<7: // 256
	case 2<<8: // 512
	case 2<<9: // 1024
	case 2<<10: // 2048
	case 2<<11: // 4096
	case 2<<12: // 8192
		break;
	default:
		self = 128;
		break;
	}
}

bool IShadowMap::ShadowTest(const DVector3 &lpos, const DVector3 &pos)
{
	if (mAABBTree && gl_light_shadowmap)
		return mAABBTree->RayTest(lpos, pos) >= 1.0f;
	else
		return true;
}

// [LIGHTSHADOWS] See the header: the light's own switch in place of gl_light_shadowmap.
bool IShadowMap::ShadowTest(const DVector3 &lpos, const DVector3 &pos, bool asksToCast)
{
	if (mAABBTree && LightShadowAllowed(asksToCast))
		return mAABBTree->RayTest(lpos, pos) >= 1.0f;
	else
		return true;
}

bool IShadowMap::PerformUpdate()
{
	UpdateCycles.Reset();

	LightsProcessed = 0;
	LightsShadowmapped = 0;
	LightsCastShadow = 0;	// [LIGHTSHADOWS]

	// CollectLights will be null if the calling code decides that shadowmaps are not needed.
	if (CollectLights != nullptr)
	{
		UpdateSerial++;	// [LIGHTSHADOWS] this frame ran the pass
		UpdateCycles.Clock();
		UploadAABBTree();
		UploadLights();
		return true;
	}
	return false;
}

void IShadowMap::UploadLights()
{
	mLights.Resize(1024 * 4);
	CollectLights();

	if (mLightList == nullptr)
		mLightList = screen->CreateDataBuffer(LIGHTLIST_BINDINGPOINT, true, false);

	mLightList->SetData(sizeof(float) * mLights.Size(), &mLights[0], BufferUsageType::Stream);
}


void IShadowMap::UploadAABBTree()
{
	if (mNewTree)
	{
		mNewTree = false;

		if (!mNodesBuffer)
			mNodesBuffer = screen->CreateDataBuffer(LIGHTNODES_BINDINGPOINT, true, false);
		mNodesBuffer->SetData(mAABBTree->NodesSize(), mAABBTree->Nodes(), BufferUsageType::Static);

		if (!mLinesBuffer)
			mLinesBuffer = screen->CreateDataBuffer(LIGHTLINES_BINDINGPOINT, true, false);
		mLinesBuffer->SetData(mAABBTree->LinesSize(), mAABBTree->Lines(), BufferUsageType::Static);
	}
	else if (mAABBTree->Update())
	{
		mNodesBuffer->SetSubData(mAABBTree->DynamicNodesOffset(), mAABBTree->DynamicNodesSize(), mAABBTree->DynamicNodes());
		mLinesBuffer->SetSubData(mAABBTree->DynamicLinesOffset(), mAABBTree->DynamicLinesSize(), mAABBTree->DynamicLines());
	}
}

void IShadowMap::Reset()
{
	delete mLightList; mLightList = nullptr;
	delete mNodesBuffer; mNodesBuffer = nullptr;
	delete mLinesBuffer; mLinesBuffer = nullptr;
}

IShadowMap::~IShadowMap()
{
	Reset();
}
