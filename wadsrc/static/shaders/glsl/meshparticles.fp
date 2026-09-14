/*
** meshparticles.fp
**
** [MESHPARTICLES] The fragment half of a mesh particle ("Engine docs/
** COLLISION_DEBRIS_MESH_PLAN.md" #10): the model's skin times the light the vertex
** shader worked out ONCE for the whole chunk (owner, 2026-09-14: one light value per
** chunk), plus its glow. Opaque: the pass writes depth, alpha is not used, nothing is
** discarded -- so the GPU keeps its early depth test for thousands of small chunks.
**
** Its own lump rather than main.fp: main.fp is the shared surface shader the laser look
** lives in and lights per pixel from varyings only main.vp writes; a chunk needs neither.
**
** Vulkan only (the vertex half reads storage buffers only Vulkan declares).
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
*/

layout(location = 0) in vec2 vMeshTexCoord;
// rgb the lit body colour: tint x colour ramp x the chunk's light (meshparticles.vp)  a 1
layout(location = 1) flat in vec4 vMeshBody;
// rgb the glow added on top: tint x colour ramp x emissive x intensity x fade  a 0
layout(location = 2) flat in vec4 vMeshGlow;
// The surface normal in eye space, for the G-buffer only: ambient occlusion shapes the chunk.
layout(location = 3) in vec3 vMeshEyeNormal;

layout(location = 0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location = 1) out vec4 FragFog;
layout(location = 2) out vec4 FragNormal;
#endif

void main()
{
	vec4 skinTexel = texture(tex, vMeshTexCoord);
	FragColor = vec4(skinTexel.rgb * (vMeshBody.rgb + vMeshGlow.rgb), 1.0);

#ifdef GBUFFER_PASS
	// No fog colour (as stencil.fp), and the normal the way main.fp writes a surface's.
	FragFog = vec4(0.0, 0.0, 0.0, 1.0);
	float normalLength = length(vMeshEyeNormal);
	vec3 eyeNormal = normalLength > 1e-5 ? vMeshEyeNormal / normalLength : vec3(0.0, 0.0, 1.0);
	FragNormal = vec4(eyeNormal * 0.5 + 0.5, 1.0);
#endif
}
