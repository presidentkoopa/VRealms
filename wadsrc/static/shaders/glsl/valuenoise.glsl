/*
** valuenoise.glsl
**
** 3D value noise: a hash at each lattice corner, blended with a smoothstep so
** cells flow into each other instead of blocking. Returns 0..1. A pure function
** of its input -- the same on every machine, every frame and both eyes; no RNG,
** no state.
**
** This is the noise volumetricbeam.fp's dust uses (its hash13 / valueNoise, the
** same maths), kept here so any Vulkan scene shader can share it:
**
**     #include "shaders/glsl/valuenoise.glsl"
**
** First user: drawnlines.fp's turbulence look ([F1], "Engine docs/
** FLAME_ENGINE_PLAN.md"), which licks the edges of a drawn line.
**
** volumetricbeam.fp does NOT include this; it keeps its own copy. GL compiles
** post-process shaders through FShaderProgram::PatchShader (gl_shaderprogram.cpp),
** which does not process #include, so an #include there would stop the beam
** compiling on GL. Vulkan resolves includes for scene and post-process shaders
** alike (VkShaderManager::OnInclude). If GL's post-process path ever learns
** #include, volumetricbeam.fp can drop its copy for this one.
**
** The include guard VkShaderManager::OnInclude wraps around every include makes a
** second #include of this lump harmless.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
*/

// A 0..1 hash of a lattice point. Cheap: no texture, no integer maths.
float hash13(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

// One octave. Features are about one unit of p across: scale p to size them.
float valueNoise(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);   // smoothstep, so cells blend instead of blocking

	return mix(
		mix(mix(hash13(i + vec3(0,0,0)), hash13(i + vec3(1,0,0)), f.x),
		    mix(hash13(i + vec3(0,1,0)), hash13(i + vec3(1,1,0)), f.x), f.y),
		mix(mix(hash13(i + vec3(0,0,1)), hash13(i + vec3(1,0,1)), f.x),
		    mix(hash13(i + vec3(0,1,1)), hash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}
