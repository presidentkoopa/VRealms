/*
** hw_postprocessshader.cpp
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

#include "vm.h"
#include "hwrenderer/postprocessing/hw_postprocessshader.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "v_video.h"

// [PPPROJECT] What the projection natives returned this frame, and for which world point. Kept for one frame
// (screen->FrameTime), at most 256 of each. A SetUniform1f/3f call whose value is exactly one of these results marks
// the uniform for per-eye projection. Exact equality: the value reaches the native unchanged from the result.
struct PPNotedPoint { double Result[3]; double World[3]; };
struct PPNotedScale { double Result; double Depth; double Size; };
static TArray<PPNotedPoint> NotedPoints;
static TArray<PPNotedScale> NotedScales;
static uint64_t NotedFrame = ~(uint64_t)0;

static bool NoteFrameCurrent()
{
	return screen != nullptr && screen->FrameTime == NotedFrame;
}

static void BeginNoteFrame()
{
	if (screen != nullptr && screen->FrameTime != NotedFrame)
	{
		NotedPoints.Clear();
		NotedScales.Clear();
		NotedFrame = screen->FrameTime;
	}
}

void PP_NoteWorldToView(double u, double v, double clipZ, double worldX, double worldY, double worldZ)
{
	BeginNoteFrame();
	if (NotedPoints.Size() < 256)
		NotedPoints.Push({ { u, v, clipZ }, { worldX, worldY, worldZ } });
}

void PP_NoteDepthToViewScale(double result, double depth, double size)
{
	BeginNoteFrame();
	// A 0 result (nearer than znear) is not noted: 0 is too common a uniform value to recognise.
	if (result != 0.0 && NotedScales.Size() < 256)
		NotedScales.Push({ result, depth, size });
}

static void TagUniform(PostProcessUniformValue &value, int components)
{
	value.Projection = 0;
	if (!NoteFrameCurrent())
		return;
	if (components == 3)
	{
		for (auto &p : NotedPoints)
		{
			if (p.Result[0] == value.Values[0] && p.Result[1] == value.Values[1] && p.Result[2] == value.Values[2])
			{
				value.Projection = 1;
				memcpy(value.World, p.World, sizeof(value.World));
				return;
			}
		}
	}
	else if (components == 1 && value.Values[0] != 0.0)
	{
		// A scale is tied to the world point whose noted depth it was computed from.
		for (auto &s : NotedScales)
		{
			if (s.Result != value.Values[0])
				continue;
			for (auto &p : NotedPoints)
			{
				if (p.Result[2] == s.Depth)
				{
					value.Projection = 2;
					memcpy(value.World, p.World, sizeof(value.World));
					value.Size = s.Size;
					return;
				}
			}
		}
	}
}

static void ShaderSetEnabled(const FString &shaderName, bool value)
{
	for (unsigned int i = 0; i < PostProcessShaders.Size(); i++)
	{
		PostProcessShader &shader = PostProcessShaders[i];
		if (shader.Name == shaderName)
			shader.Enabled = value;
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(_PPShader, SetEnabled, ShaderSetEnabled)
{
	PARAM_PROLOGUE;
	PARAM_STRING(shaderName);
	PARAM_BOOL(value);
	ShaderSetEnabled(shaderName, value);

	return 0;
}

static void ShaderSetUniform1f(const FString &shaderName, const FString &uniformName, double value)
{
	for (unsigned int i = 0; i < PostProcessShaders.Size(); i++)
	{
		PostProcessShader &shader = PostProcessShaders[i];
		if (shader.Name == shaderName)
		{
			double *vec4 = shader.Uniforms[uniformName].Values;
			vec4[0] = value;
			vec4[1] = 0.0;
			vec4[2] = 0.0;
			vec4[3] = 1.0;
			TagUniform(shader.Uniforms[uniformName], 1);	// [PPPROJECT]
		}
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(_PPShader, SetUniform1f, ShaderSetUniform1f)
{
	PARAM_PROLOGUE;
	PARAM_STRING(shaderName);
	PARAM_STRING(uniformName);
	PARAM_FLOAT(value);
	ShaderSetUniform1f(shaderName, uniformName, value);
	return 0;
}

DEFINE_ACTION_FUNCTION(_PPShader, SetUniform2f)
{
	PARAM_PROLOGUE;
	PARAM_STRING(shaderName);
	PARAM_STRING(uniformName);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);

	for (unsigned int i = 0; i < PostProcessShaders.Size(); i++)
	{
		PostProcessShader &shader = PostProcessShaders[i];
		if (shader.Name == shaderName)
		{
			double *vec4 = shader.Uniforms[uniformName].Values;
			vec4[0] = x;
			vec4[1] = y;
			vec4[2] = 0.0;
			vec4[3] = 1.0;
			shader.Uniforms[uniformName].Projection = 0;	// [PPPROJECT]
		}
	}
	return 0;
}

DEFINE_ACTION_FUNCTION(_PPShader, SetUniform3f)
{
	PARAM_PROLOGUE;
	PARAM_STRING(shaderName);
	PARAM_STRING(uniformName);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);

	for (unsigned int i = 0; i < PostProcessShaders.Size(); i++)
	{
		PostProcessShader &shader = PostProcessShaders[i];
		if (shader.Name == shaderName)
		{
			double *vec4 = shader.Uniforms[uniformName].Values;
			vec4[0] = x;
			vec4[1] = y;
			vec4[2] = z;
			vec4[3] = 1.0;
			TagUniform(shader.Uniforms[uniformName], 3);	// [PPPROJECT]
		}
	}
	return 0;
}

DEFINE_ACTION_FUNCTION(_PPShader, SetUniform4f)
{
	PARAM_PROLOGUE;
	PARAM_STRING(shaderName);
	PARAM_STRING(uniformName);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);
	PARAM_FLOAT(w);

	for (unsigned int i = 0; i < PostProcessShaders.Size(); i++)
	{
		PostProcessShader &shader = PostProcessShaders[i];
		if (shader.Name == shaderName)
		{
			double *vec4 = shader.Uniforms[uniformName].Values;
			vec4[0] = x;
			vec4[1] = y;
			vec4[2] = z;
			vec4[3] = w;
			shader.Uniforms[uniformName].Projection = 0;	// [PPPROJECT]
		}
	}
	return 0;
}

DEFINE_ACTION_FUNCTION(_PPShader, SetUniform1i)
{
	PARAM_PROLOGUE;
	PARAM_STRING(shaderName);
	PARAM_STRING(uniformName);
	PARAM_INT(value);

	for (unsigned int i = 0; i < PostProcessShaders.Size(); i++)
	{
		PostProcessShader &shader = PostProcessShaders[i];
		if (shader.Name == shaderName)
		{
			double *vec4 = shader.Uniforms[uniformName].Values;
			vec4[0] = (double)value;
			shader.Uniforms[uniformName].Projection = 0;	// [PPPROJECT]
			vec4[1] = 0.0;
			vec4[2] = 0.0;
			vec4[3] = 1.0;
		}
	}
	return 0;
}
