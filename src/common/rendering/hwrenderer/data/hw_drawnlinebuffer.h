/*
** hw_drawnlinebuffer.h
**
** [DRAWNLINES] The GPU half of drawn glowing lines.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Why this exists: the engine's beam lines (SetBeam, main.fp's BeamLightAt and
** BeamAirGlow) are lit PER PIXEL -- every fragment of every surface runs the
** closest-approach solve for every live line. That is what lets a beam light
** the walls it passes, and it is why beams cap at 128 and cost pixels x lines.
**
** A drawn line keeps the look and drops the per-pixel cost: the vertex shader
** builds a box around the line, and the fragment shader runs the same air-glow
** maths BeamAirGlow does, for this one line, only for the pixels of that box.
** Many lines cost what their pixels cost. See drawnlines.fp for exactly where
** the drawn look cannot match the per-pixel one.
**
** Two callers share it: SetDrawnLine (FLevelLocals::DrawnLine, thousands of
** lines) and, when r_beams_drawn is on, the ordinary beam slots, routed here for
** an A/B comparison in the headset.
**
** Records are rebuilt and re-sent every scene (SyncDrawnLines, hw_drawinfo.cpp):
** lines are interpolated between tics and may start at a tracked hand, so their
** endpoints move every frame anyway. There is no ring and no sync rule.
**
** Vulkan only, exactly like GpuParticleBuffer: created beside it in
** VulkanRenderDevice, bound at set 1 binding 6, and never created on GL or GLES,
** which also never load the effect shader.
**
*/

#pragma once

#include <cstdint>
#include "hwrenderer/data/buffers.h"

// Seven vec4s, 112 bytes, std430 with no padding, SHADER space (y up). Must match
// the DrawnLine struct in vk_shader.cpp's prolog and drawnlines.vp.
//
//   a  xyz start                       w core thickness (SetBeam's thick)
//   b  xyz end                         w softness (SetBeam's soft)
//   c  rgb colour 0..1                 w intensity
//   d  x air glow  y halo strength     z taper  w impact flare
//   e  x scroll speed  y scroll depth  z timer seconds (main.fp's `timer` for
//      a material at speed 1)          w depth bias in map units (< 0 automatic)
//   f  rgb end colour 0..1             w swell (halo reach at the end over the
//                                        start; 1 none)
//   g  x turbulence strength  y noise scale (cells per map unit)  z rise speed
//      (cells per second)              w look flags (LOOK_GRADIENT | LOOK_TURBULENCE)
//
// [F1] f and g are two OPT-IN looks ("Engine docs/FLAME_ENGINE_PLAN.md" F1): a
// colour gradient with a widening halo, and licking turbulence. Look flags 0 --
// every beam r_beams_drawn routes, and every SetDrawnLine line that never asked
// (SetDrawnLineGradient / SetDrawnLineTurbulence) -- make drawnlines.vp/.fp skip f
// and g and run exactly the code they ran before the record grew.
struct DrawnLineRecord
{
	float a[4];
	float b[4];
	float c[4];
	float d[4];
	float e[4];
	float f[4];
	float g[4];
};

class DrawnLineBuffer
{
public:
	static const unsigned RECORD_BYTES = 112;

	// [F1] Bits of a record's look flags (g.w, written as an exact small float).
	// drawnlines.vp/.fp test the same bits.
	static const unsigned LOOK_GRADIENT = 1;
	static const unsigned LOOK_TURBULENCE = 2;

	// A box around the line: six faces, two triangles each. drawnlines.vp keeps
	// only the faces turned away from the eye, which cover the box's footprint
	// exactly once, including when the eye is inside it.
	static const unsigned VERTICES_PER_RECORD = 36;

	// Room for every beam slot on top of DrawnLineCapacity(), so r_beams_drawn
	// can never be starved by a full SetDrawnLine array. Must equal
	// FLevelLocals::MAX_BEAMS (asserted in hw_drawinfo.cpp).
	static const unsigned ROUTED_BEAM_RESERVE = 128;

	// records 0 = DrawnLineCapacity() + ROUTED_BEAM_RESERVE.
	explicit DrawnLineBuffer(unsigned records = 0);
	~DrawnLineBuffer();

	// Copies `count` records (clamped to capacity) to the GPU and makes them the
	// live set for the next draw. `routed` is how many of them are beam slots,
	// for diagnostics only.
	void Upload(const DrawnLineRecord *records, unsigned count, unsigned routed);

	IDataBuffer *GetBuffer() const { return mBuffer; }
	IVertexBuffer *GetVertexBuffer() const { return mBoxes; }
	unsigned GetCapacity() const { return mCapacity; }
	unsigned GetLiveCount() const { return mLiveCount; }
	unsigned GetRoutedCount() const { return mRoutedCount; }
	int GetVertexCount() const { return (int)(mLiveCount * VERTICES_PER_RECORD); }

	// Set by VkShaderManager once the drawnlines effect has compiled for every
	// pass. A pipeline built for an effect whose program is missing dereferences
	// null (VkRenderPassSetup::CreatePipeline), so the draw -- and r_beams_drawn's
	// routing -- is gated on this, as the particles are.
	bool ShaderReady = false;
	bool ShaderFailed = false;

	bool IsDrawable() const { return ShaderReady && !ShaderFailed && mBuffer != nullptr && mBoxes != nullptr; }

	// "ready", "FAILED (shader)", "not ready", "off (r_drawnlines 0)"
	const char *StateName() const;

	// Diagnostics for r_beams_debug.
	void CountDraw() { mDrawsSinceReport++; }
	unsigned TakeDrawCount() { unsigned d = mDrawsSinceReport; mDrawsSinceReport = 0; return d; }

private:
	IDataBuffer *mBuffer = nullptr;
	IVertexBuffer *mBoxes = nullptr;
	unsigned mCapacity = 0;
	unsigned mLiveCount = 0;
	unsigned mRoutedCount = 0;
	unsigned mDrawsSinceReport = 0;
	bool mWarnedClamp = false;
};

// True when r_beams_drawn is on AND the drawn path can actually draw (Vulkan, the
// shader compiled, r_drawnlines on). The beam upload in HWDrawInfo::StartScene
// and SyncDrawnLines both ask this, so a beam is never drawn twice or not at all.
bool BeamsRouteToDrawnLines();

// r_beams_drawn's change line.
void DrawnLinesLogToggle(bool on);
