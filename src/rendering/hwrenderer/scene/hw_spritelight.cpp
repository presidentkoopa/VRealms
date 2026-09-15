/*
** hw_spritelight.cpp
**
** Light level / fog management / dynamic lights
**
**---------------------------------------------------------------------------
**
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "c_dispatch.h"
#include "a_dynlight.h"
#include "p_local.h"
#include "p_effect.h"
#include "g_level.h"
#include "g_levellocals.h"
#include "actorinlines.h"
#include "hw_dynlightdata.h"
#include "hw_clock.h"
#include "hw_shadowmap.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "models.h"
#include <cmath>	// needed for std::floor on mac

template<class T>
T smoothstep(const T edge0, const T edge1, const T x)
{
	auto t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

LightProbe* FindLightProbe(FLevelLocals* level, float x, float y, float z)
{
	LightProbe* foundprobe = nullptr;
	if (level->LightProbes.Size() > 0)
	{
#if 1
		double rcpCellSize = 1.0 / level->LPCellSize;
		int gridCenterX = (int)std::floor(x * rcpCellSize) - level->LPMinX;
		int gridCenterY = (int)std::floor(y * rcpCellSize) - level->LPMinY;
		int gridWidth = level->LPWidth;
		int gridHeight = level->LPHeight;
		float lastdist = 0.0f;
		for (int gridY = gridCenterY - 1; gridY <= gridCenterY + 1; gridY++)
		{
			for (int gridX = gridCenterX - 1; gridX <= gridCenterX + 1; gridX++)
			{
				if (gridX >= 0 && gridY >= 0 && gridX < gridWidth && gridY < gridHeight)
				{
					const LightProbeCell& cell = level->LPCells[gridX + (size_t)gridY * gridWidth];
					for (int i = 0; i < cell.NumProbes; i++)
					{
						LightProbe* probe = cell.FirstProbe + i;
						float dx = probe->X - x;
						float dy = probe->Y - y;
						float dz = probe->Z - z;
						float dist = dx * dx + dy * dy + dz * dz;
						if (!foundprobe || dist < lastdist)
						{
							foundprobe = probe;
							lastdist = dist;
						}
					}
				}
			}
		}
#else
		float lastdist = 0.0f;
		for (unsigned int i = 0; i < level->LightProbes.Size(); i++)
		{
			LightProbe *probe = &level->LightProbes[i];
			float dx = probe->X - x;
			float dy = probe->Y - y;
			float dz = probe->Z - z;
			float dist = dx * dx + dy * dy + dz * dz;
			if (i == 0 || dist < lastdist)
			{
				foundprobe = probe;
				lastdist = dist;
			}
		}
#endif
	}
	return foundprobe;
}

//==========================================================================
//
// Sets a single light value from all dynamic lights affecting the specified location
//
//==========================================================================

void HWDrawInfo::GetDynSpriteLight(AActor *self, float x, float y, float z, FSection *sec, int portalgroup, float *out)
{
	FDynamicLight *light;
	float frac, lr, lg, lb;
	float radius;

	out[0] = out[1] = out[2] = 0.f;

	LightProbe* probe = FindLightProbe(Level, x, y, z);
	if (probe)
	{
		out[0] = probe->Red;
		out[1] = probe->Green;
		out[2] = probe->Blue;
	}

	// Go through both light lists
	if (Level->lightlists.flat_dlist.SSize() > sec->Index())
	{
		TMap<FDynamicLight *, std::unique_ptr<FLightNode>>::Iterator it(Level->lightlists.flat_dlist[sec->Index()]);
		TMap<FDynamicLight *, std::unique_ptr<FLightNode>>::Pair *pair;
		while (it.NextPair(pair))
		{
			auto node = pair->Value.get();
			if (!node) continue;

			light=node->lightsource;
			if (light->ShouldLightActor(self))
			{
				float dist;
				FVector3 L;

				// This is a performance critical section of code where we cannot afford to let the compiler decide whether to inline the function or not.
				// This will do the calculations explicitly rather than calling one of AActor's utility functions.
				if (Level->Displacements.size > 0)
				{
					int fromgroup = light->Sector->PortalGroup;
					int togroup = portalgroup;
					if (fromgroup == togroup || fromgroup == 0 || togroup == 0) goto direct;

					DVector2 offset = Level->Displacements.getOffset(fromgroup, togroup);
					L = FVector3(x - (float)(light->X() + offset.X), y - (float)(light->Y() + offset.Y), z - (float)light->Z());
				}
				else
				{
				direct:
					L = FVector3(x - (float)light->X(), y - (float)light->Y(), z - (float)light->Z());
				}

				dist = (float)L.LengthSquared();
				radius = light->GetRadius();

				if (dist < radius * radius)
				{
					dist = sqrtf(dist);	// only calculate the square root if we really need it.

					frac = 1.0f - (dist / radius);

					if (light->IsSpot())
					{
						L *= -1.0f / dist;
						DAngle negPitch = -light->Pitch;
						DAngle Angle = light->Yaw;
						double xyLen = negPitch.Cos();
						double spotDirX = -Angle.Cos() * xyLen;
						double spotDirY = -Angle.Sin() * xyLen;
						double spotDirZ = -negPitch.Sin();
						double cosDir = L.X * spotDirX + L.Y * spotDirY + L.Z * spotDirZ;
						frac *= (float)smoothstep(light->pSpotOuterAngle->Cos(), light->pSpotInnerAngle->Cos(), cosDir);
					}

					// [LIGHTSHADOWS] Tested by the light's own shadow switch (IShadowMap::LightShadowAllowed): a light that asks to cast by
					// the cast-shadow setting, any other by "Light shadows" -- the rule CollectLights gives rows by.
					if (frac > 0 && (!light->shadowmapped || (light->GetRadius() > 0 && screen->mShadowMap.ShadowTest(light->Pos, { x, y, z }, light->CastShadow()))))
					{
						lr = light->GetRed() / 255.0f;
						lg = light->GetGreen() / 255.0f;
						lb = light->GetBlue() / 255.0f;

						if (light->target && (light->target->renderflags2 & RF2_LIGHTMULTALPHA))
						{
							float alpha = (float)light->target->Alpha;
							lr *= alpha;
							lg *= alpha;
							lb *= alpha;
						}

						// Get GLDEFS intensity
						lr *= light->GetLightDefIntensity();
						lg *= light->GetLightDefIntensity();
						lb *= light->GetLightDefIntensity();

						if (light->IsSubtractive())
						{
							float bright = (float)FVector3(lr, lg, lb).Length();
							FVector3 lightColor(lr, lg, lb);
							lr = (bright - lr) * -1;
							lg = (bright - lg) * -1;
							lb = (bright - lb) * -1;
						}

						out[0] += lr * frac;
						out[1] += lg * frac;
						out[2] += lb * frac;
					}
				}
			}
		}
	}
}

void HWDrawInfo::GetDynSpriteLight(AActor *thing, particle_t *particle, float *out)
{
	if (thing && !(thing->renderflags2 & RF2_NODYNAMICLIGHTING))
	{
		GetDynSpriteLight(thing, (float)thing->X(), (float)thing->Y(), (float)thing->Center(), thing->section, thing->Sector->PortalGroup, out);
	}
	else if (particle && !(particle->flags & SPF_NODYNAMICLIGHTING))
	{
		GetDynSpriteLight(NULL, (float)particle->Pos.X, (float)particle->Pos.Y, (float)particle->Pos.Z, particle->subsector->section, particle->subsector->sector->PortalGroup, out);
	}
}

// static so that we build up a reserve (memory allocations stop)
// For multithread processing each worker thread needs its own copy, though.
static thread_local TArray<FDynamicLight*> addedLightsArray;

void hw_GetDynModelLight(AActor *self, FDynLightData &modellightdata)
{
	static thread_local unsigned int modelLightGatherId = 0;
	modellightdata.Clear();

	if (self)
	{
		auto &addedLights = addedLightsArray;	// avoid going through the thread local storage for each use.
		unsigned int gatherId = 0;

		addedLights.Clear();
		if (gl_light_model_dedupe_cache)
		{
			gatherId = ++modelLightGatherId;
			if (gatherId == 0)
			{
				gatherId = ++modelLightGatherId;
			}
		}

		float x = (float)self->X();
		float y = (float)self->Y();
		float z = (float)self->Center();
		float actorradius = (float)self->RenderRadius();
		float radiusSquared = actorradius * actorradius;
		dl_validcount++;

		BSPWalkCircle(self->Level, x, y, radiusSquared, [&](subsector_t *subsector) // Iterate through all subsectors potentially touched by actor
		{
			dynlights_model_subsectors++;
			auto section = subsector->section;
			if (section->validcount == dl_validcount) return;	// already done from a previous subsector.

			if (self->Level->lightlists.flat_dlist.SSize() > subsector->section->Index())
			{
				// [UZDXREMA] Upstream 5.0 replaced FSection::lighthead with a per-section TMap of
				// FLightNode; the fork's model-light telemetry, distance culling and dedupe cache
				// are re-seated onto the new iteration.
				TMap<FDynamicLight *, std::unique_ptr<FLightNode>>::Iterator it(self->Level->lightlists.flat_dlist[subsector->section->Index()]);
				TMap<FDynamicLight *, std::unique_ptr<FLightNode>>::Pair *pair;
				while (it.NextPair(pair))
				{ // check all lights touching a subsector
					auto node = pair->Value.get();
					if (!node) continue;

					dynlights_model_candidates++;
					FDynamicLight *light = node->lightsource;
					if (!light->IsActive() || gl_IsDistanceCulled(light))
					{
						if (light->IsActive() && gl_IsDistanceCulled(light)) dynlights_distance_culled_models++;
						continue;
					}
					if (light->ShouldLightActor(self))
					{
						int group = subsector->sector->PortalGroup;
						DVector3 pos = gl_GetLightPosRelative(light, group);
						float radius = (float)(light->GetRadius() + actorradius);
						double dx = pos.X - x;
						double dy = pos.Y - y;
						double dz = pos.Z - z;
						double distSquared = dx * dx + dy * dy + dz * dz;
						if (distSquared < radius * radius) // Light and actor touches
						{
							bool alreadyAdded = false;
							if (gl_light_model_dedupe_cache)
							{
								alreadyAdded = light->mModelLightGatherId == gatherId;
							}
							else
							{
								alreadyAdded = std::find(addedLights.begin(), addedLights.end(), light) != addedLights.end();
							}

							if (!alreadyAdded) // Check if we already added this light from a different subsector
							{
								AddLightToList(modellightdata, group, light, true);
								dynlights_model_uploads++;
								if (gl_light_model_dedupe_cache)
								{
									light->mModelLightGatherId = gatherId;
								}
								else
								{
									addedLights.Push(light);
								}
							}
							else dynlights_model_duplicates++;
						}
					}
				}
			}
		});
	}
}
