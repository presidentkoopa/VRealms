/*
** hw_viewlightbuffer.h
**
** [VIEWLIGHTS] The dynamic lights in view this scene, for effects that light
** themselves in the VERTEX shader.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: surfaces are lit per surface from the light lists the BSP
** links (FLightBuffer, fragment stage), and sprites are lit on the CPU at one
** point (HWDrawInfo::GetDynSpriteLight). Neither reaches thousands of GPU
** particles whose positions exist only in a vertex shader. This is one short,
** world-space list of the lights nearest the eye among those in view, which any
** vertex-stage effect can loop over. GPU particles (gpuparticles.vp, stage 2d)
** are the first user; mesh particles ("Engine docs/REVIEW_SMOKE_DEBRIS_DAMAGE.md"
** D4) index the same list per instance. Muzzle flashes, flame lights and
** flashlights from any mod reach it with no coupling. See "Engine docs/
** GPU_PARTICLES_STAGE2_PLAN.md" 2d.
**
** The list is chosen and filled on the CPU every scene (SyncViewLights,
** hw_drawinfo.cpp) and read only by shaders. It is presentation: nothing reads it
** back, and a light on one machine's list changes only that machine's pixels.
**
** Vulkan only, exactly like GpuParticleBuffer: created beside it in
** VulkanRenderDevice::InitializeState, null on GL and GLES. Set 1 binding 8
** (vk_descriptorset.cpp), vertex stage only.
**
*/

#pragma once

#include <cstdint>

class IDataBuffer;

// One light: four vec4s, 64 bytes, std430 with no padding, SHADER space (y up).
// Must match the ViewLight struct in gpuparticles.vp (asserted here in size; the
// shader's offsets are checked from SPIR-V, see "Engine docs/STAGE2D_IMPL_NOTES.md").
//
//   origin         xyz position                     w radius, map units (FDynamicLight::GetRadius)
//   color          rgb colour x GLDEFS intensity, 1.0 per 255 -- negative for a
//                  subtractive light                w 0
//   spotDirection  xyz the direction a spot's cone test dots the direction TO the
//                  light with (hw_dynlightdata.cpp's spot direction); 0 for a
//                  point light                      w 0
//   spotCone       x cos of the outer angle  y cos of the inner angle -- a point
//                  light has -2, -1, so its smoothstep is always 1   zw 0
struct ViewLightRecord
{
	float origin[4];
	float color[4];
	float spotDirection[4];
	float spotCone[4];
};

class ViewLightBuffer
{
public:
	static const unsigned RECORD_BYTES = 64;

	// One vec4 ahead of the records: x how many records are live, yzw 0.
	static const unsigned HEADER_BYTES = 16;

	// The most lights one scene hands the GPU (the plan's "up to 32"). The shader
	// clamps the count to its own copy of this number (gpuparticles.vp,
	// kViewLightCapacity); raise both together.
	static const unsigned CAPACITY = 32;

	ViewLightBuffer();
	~ViewLightBuffer();

	// Copies `count` records (clamped to CAPACITY) and then the count ahead of
	// them, making them the list every draw after this reads. A count of 0 is
	// written once and then skipped while it stays 0, so a scene with nothing lit
	// costs no copy at all.
	void Upload(const ViewLightRecord *records, unsigned count);

	IDataBuffer *GetBuffer() const { return mBuffer; }
	unsigned GetLiveCount() const { return mLiveCount; }

private:
	IDataBuffer *mBuffer = nullptr;
	unsigned mLiveCount = 0;
	bool mCountWritten = false;
};
