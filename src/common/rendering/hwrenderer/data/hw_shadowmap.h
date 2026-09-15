/*
** hw_shadowmap.h
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

#pragma once

#include "hw_aabbtree.h"
#include "stats.h"
#include <cstdint>
#include <memory>
#include <functional>

class IDataBuffer;

class IShadowMap
{
public:
	IShadowMap() { }
	virtual ~IShadowMap();

	void Reset();

	// Test if a world position is in shadow relative to the specified light and returns false if it is
	bool ShadowTest(const DVector3 &lpos, const DVector3 &pos);
	// [LIGHTSHADOWS] The same test for one light by that light's own shadow switch (LightShadowAllowed) instead of "Light
	// shadows" alone. `asksToCast` is the light's LF_CASTSHADOW. For a light that does not ask, while the cast-shadow setting does
	// not reach all lights, it is exactly the test above.
	bool ShadowTest(const DVector3 &lpos, const DVector3 &pos, bool asksToCast);

	static cycle_t UpdateCycles;
	static int LightsProcessed;
	static int LightsShadowmapped;
	// [LIGHTSHADOWS] Of LightsShadowmapped, the rows given to lights that ask to cast (CollectLights). And the map updates since
	// start-up: the counters above keep the last pass's values while the pass does not run, so the performance log samples them
	// only on a frame where this moved on.
	static int LightsCastShadow;
	static uint64_t UpdateSerial;
	// [LIGHTSHADOWS] True when this session's scene shaders ray trace light shadows: set once by the Vulkan device at start-up,
	// false on OpenGL. For the performance log and the start-up log line -- not a switch.
	static bool RaytracedThisSession;

	// [LIGHTSHADOWS] THE CAST-SHADOW SETTING (cvars in hw_shadowmap.cpp, where the ladder is described). One ladder for the
	// lights that ask to cast shadows (a_dynlight.h, LF_CASTSHADOW) beside "Light shadows", which keeps every other light.
	enum ECastShadows { CASTSHADOWS_OFF = 0, CASTSHADOWS_SHADOWMAP = 1, CASTSHADOWS_RAYTRACED = 2 };	// gl_light_castshadows
	enum EShadowLights { SHADOWLIGHTS_ASKING = 0, SHADOWLIGHTS_ALL = 1 };								// gl_light_shadowmap_lights
	static bool CastShadowsOn();				// gl_light_castshadows is shadow maps or ray traced
	static bool CastShadowsRaytraced();			// gl_light_castshadows is ray traced -- read ONCE, by the Vulkan device at start-up
	static bool CastShadowsReachAllLights();	// on, and gl_light_shadowmap_lights is all lights
	// One light's shadow switch: the rule CollectLights hands out rows by and the sprite light tests by. A light that asks to
	// cast follows the cast-shadow setting alone; any other follows "Light shadows" -- or the cast-shadow setting too while it
	// reaches all lights. With that setting Off it is gl_light_shadowmap for every light that does not ask: the rule before it.
	static bool LightShadowAllowed(bool asksToCast);

	bool PerformUpdate();
	void FinishUpdate()
	{
		// [LIGHTSHADOWS] Unclock, not Clock: this called Clock a second time, which ran the timer backwards, so `stat shadowmap`'s
		// upload time was nonsense. The performance log's shadowcpu_ms reads it.
		UpdateCycles.Unclock();
	}

	unsigned int NodesCount() const
	{
		assert(mAABBTree);
		return mAABBTree->NodesCount();
	}

	void SetAABBTree(hwrenderer::LevelAABBTree* tree)
	{
		if (mAABBTree != tree)
		{
			mAABBTree = tree;
			mNewTree = true;
		}
	}

	void SetCollectLights(std::function<void()> func)
	{
		CollectLights = std::move(func);
	}

	void SetLight(int index, float x, float y, float z, float r)
	{
		index *= 4;
		mLights[index] = x;
		mLights[index + 1] = y;
		mLights[index + 2] = z;
		mLights[index + 3] = r;
	}

	bool Enabled() const
	{
		return mAABBTree != nullptr;
	}

protected:
	// Upload the AABB-tree to the GPU
	void UploadAABBTree();
	void UploadLights();

	// Working buffer for creating the list of lights. Stored here to avoid allocating memory each frame
	TArray<float> mLights;

	// AABB-tree of the level, used for ray tests, owned by the playsim, not the renderer.
	hwrenderer::LevelAABBTree* mAABBTree = nullptr;
	bool mNewTree = false;

	IShadowMap(const IShadowMap &) = delete;
	IShadowMap &operator=(IShadowMap &) = delete;

	// OpenGL storage buffer with the list of lights in the shadow map texture
	// These buffers need to be accessed by the OpenGL backend directly so that they can be bound.
public:
	IDataBuffer *mLightList = nullptr;

	// OpenGL storage buffers for the AABB tree
	IDataBuffer *mNodesBuffer = nullptr;
	IDataBuffer *mLinesBuffer = nullptr;

	std::function<void()> CollectLights = nullptr;

};
