/*
** hw_postprocessshader.h
**
** Postprocessing framework
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "zstring.h"
#include "tarray.h"

enum class PixelFormat
{
	Rgba8,
	Rgba16f,
	R32f,
	Rg16f,
	Rgba16_snorm,
	Rgba32f		// [SMOKE_TEMPORAL] RGBA, a 32-bit float a channel: a target whose alpha carries more than a 16-bit float holds (PPSmokeVolume's temporal march)
};

enum class PostProcessUniformType
{
	Undefined,
	Int,
	Float,
	Vec2,
	Vec3,
	Vec4
};

struct PostProcessUniformValue
{
	PostProcessUniformType Type = PostProcessUniformType::Undefined;
	double Values[4] = { 0.0, 0.0, 0.0, 0.0 };

	// [PPPROJECT] Re-projected for each eye when the shader runs (PPCustomShaderInstance::SetUniforms):
	//   0 = none; 1 = a world point as WorldToView gives it (vec3: u, v, clip z);
	//   2 = a world size as DepthToViewScale gives it (float).
	// Set by the SetUniform natives when the value is one a projection native just returned (hw_postprocessshader.cpp).
	int Projection = 0;
	double World[3] = { 0.0, 0.0, 0.0 };	// game axes
	double Size = 0.0;
};

struct PostProcessShader
{
	FString Target;
	FString ShaderLumpName;
	int ShaderVersion = 0;

	FString Name;
	bool Enabled = false;

	TMap<FString, PostProcessUniformValue> Uniforms;
	TMap<FString, FString> Textures;
};

extern TArray<PostProcessShader> PostProcessShaders;

// [PPPROJECT] Per-eye world-to-screen for custom post-process shaders.
// - A projection native notes what it returned (this frame, per world point). A SetUniform1f/3f call with exactly
//   that value marks the uniform for per-eye projection.
// - PP_ProjectWorldForView projects with the first published eye view, which is what such a native should return.
//   False before any view is published.
void PP_NoteWorldToView(double u, double v, double clipZ, double worldX, double worldY, double worldZ);
void PP_NoteDepthToViewScale(double result, double depth, double size);
bool PP_ProjectWorldForView(double worldX, double worldY, double worldZ, double &u, double &v, double &clipZ);
bool PP_GetViewFocalY(double &focalY);
