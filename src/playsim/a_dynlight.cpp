/*
** a_dynlight.cpp
**
** Implements actors representing dynamic lights (hardware independent)
**
**---------------------------------------------------------------------------
**
** Copyright 2004-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** all functions marked with [TS] are licensed under
**
** Copyright 2003 Timothy Stump
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "c_dispatch.h"
#include "thingdef.h"
#include "r_utility.h"
#include "doomstat.h"
#include "serializer.h"
#include "g_levellocals.h"
#include "a_dynlight.h"
#include "actorinlines.h"
#include "hw_clock.h"
#include "memarena.h"
#include "d_player.h"	// [round2 B1] a pawn's player, for whose pose a light follows

EXTERN_CVAR(Bool, r_visualstate_log)	// [round2 B1] RS fork: defined in vmthunks.cpp

static FMemArena DynLightArena(sizeof(FDynamicLight) * 200);
static TArray<FDynamicLight*> FreeList;
static FCRandom randLight;

CVAR(Float, gl_light_max_intensity, 1000.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, gl_light_distance_cull, 2000.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, gl_light_max_collected_subsectors, 1000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, gl_light_flat_max_lights, 1000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, gl_light_wall_max_lights, 1000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, gl_light_flat_candidate_budget, 16, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, gl_light_wall_candidate_budget, 8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, gl_light_range_limit, 64, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

extern TArray<FLightDefaults *> StateLights;


//==========================================================================
//
//
//
//==========================================================================

static FDynamicLight *GetLight(FLevelLocals *Level)
{
	FDynamicLight *ret;
	if (FreeList.Size())
	{
		FreeList.Pop(ret);
	}
	else ret = (FDynamicLight*)DynLightArena.Alloc(sizeof(FDynamicLight));
	memset((void*)ret, 0, sizeof(*ret));
	ret = new(ret)FDynamicLight();
	ret->m_cycler.m_increment = true;
	ret->next = Level->lights;
	Level->lights = ret;
	if (ret->next) ret->next->prev = ret;
	ret->visibletoplayer = true;
	ret->mShadowmapIndex = 1024;
	ret->Level = Level;
	ret->Pos.X = -10000000;	// not a valid coordinate.
	return ret;
}


//==========================================================================
//
// Attaches a dynamic light descriptor to a dynamic light actor.
// Owned lights do not use this function.
//
//==========================================================================

void AttachLight(AActor *self)
{
	if(self->ObjectFlags & OF_EuthanizeMe) return;
	auto light = GetLight(self->Level);

	light->pSpotInnerAngle = &self->AngleVar(NAME_SpotInnerAngle);
	light->pSpotOuterAngle = &self->AngleVar(NAME_SpotOuterAngle);
	light->lightDefIntensity = 1.0;
	light->Yaw = self->Angles.Yaw;
	light->Pitch = self->Angles.Pitch;
	light->pLightFlags = (LightFlags*)&self->IntVar(NAME_lightflags);
	light->pArgs = self->args;
	light->specialf1 = DAngle::fromDeg(double(self->SpawnAngle)).Normalized360().Degrees();
	light->Sector = self->Sector;
	light->target = self;
	light->mShadowmapIndex = 1024;
	light->m_active = false;
	light->visibletoplayer = true;
	light->lighttype = (uint8_t)self->IntVar(NAME_lighttype);
	// [round2 B1] A SpotLight (or any light actor) can be held in a tracked pose
	// (Actor.SetAttachedLightAnchor). The anchor lives on the actor's fields, so
	// it comes back whenever the light is rebuilt from the actor.
	light->SetPoseAnchor(self->IntVar(NAME_PoseAnchor), *(DVector3*)self->ScriptVar(NAME_PoseAnchorOffset, nullptr));
	self->AttachedLights.Push(light);

	// Disable postponed processing of dynamic light because its setup has been completed by this function
	self->flags8 &= ~MF8_RECREATELIGHTS;
}

DEFINE_ACTION_FUNCTION_NATIVE(ADynamicLight, AttachLight, AttachLight)
{
	PARAM_SELF_PROLOGUE(AActor);
	AttachLight(self);
	return 0;
}

//==========================================================================
//
//
//
//==========================================================================

void ActivateLight(AActor *self)
{
	for (auto l : self->AttachedLights) l->Activate();
}

DEFINE_ACTION_FUNCTION_NATIVE(ADynamicLight, ActivateLight, ActivateLight)
{
	PARAM_SELF_PROLOGUE(AActor);
	ActivateLight(self);
	return 0;
}


//==========================================================================
//
//
//
//==========================================================================

void DeactivateLight(AActor *self)
{
	for (auto l : self->AttachedLights) l->Deactivate();
}

DEFINE_ACTION_FUNCTION_NATIVE(ADynamicLight, DeactivateLight, DeactivateLight)
{
	PARAM_SELF_PROLOGUE(AActor);
	DeactivateLight(self);
	return 0;
}

//==========================================================================
//
//
//
//==========================================================================

static void SetOffset(AActor *self, double x, double y, double z)
{
	for (auto l : self->AttachedLights)
	{
		l->SetOffset(DVector3(x, y, z));
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(ADynamicLight, SetOffset, SetOffset)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);
	SetOffset(self, x, y, z);
	return 0;
}

//==========================================================================
//
//
//
//==========================================================================

void FDynamicLight::ReleaseLight()
{
	// [round2 B1] Out of the anchored-light registry BEFORE the memory goes back
	// to the free list. Every free passes through here (Tick with no target,
	// DeleteAttachedLights from OnDestroy, level unlink, gl_lights off), which is
	// why the removal is here and not in UnlinkLight: LinkLight calls UnlinkLight
	// on every relink. Searched whenever the registry is non-empty, not only when
	// PoseRegistered says so, so a stale flag still cannot leave a dangling entry.
	// The registry is a handful of lights at most.
	if (Level->PoseAnchoredLights.Size() > 0)
	{
		unsigned i = Level->PoseAnchoredLights.Find(this);
		if (i < Level->PoseAnchoredLights.Size()) Level->PoseAnchoredLights.Delete(i);
	}
	PoseRegistered = false;
	PoseAnchor = 0;

	assert(prev != nullptr || this == Level->lights);
	if (prev != nullptr) prev->next = next;
	else Level->lights = next;
	if (next != nullptr) next->prev = prev;
	next = prev = nullptr;
	this->~FDynamicLight();
	FreeList.Push(this);
}


//==========================================================================
//
// [TS]
//
//==========================================================================
void FDynamicLight::Activate()
{
	m_active = true;
	m_currentRadius = float(GetIntensity());
	m_tickCount = 0;

	if (lighttype == PulseLight)
	{
		float pulseTime = float(specialf1 / TICRATE);

		m_lastUpdate = GetTimer();
		if (!swapped) m_cycler.SetParams(float(GetSecondaryIntensity()), float(GetIntensity()), pulseTime);
		else m_cycler.SetParams(float(GetIntensity()), float(GetSecondaryIntensity()), pulseTime);
		m_cycler.ShouldCycle(true);
		m_cycler.SetCycleType(CYCLE_Sin);
		m_currentRadius = float(m_cycler.GetVal());
	}
	if (m_currentRadius > gl_light_max_intensity) m_currentRadius = gl_light_max_intensity;
	if (m_currentRadius <= 0) m_currentRadius = 1;
}


//==========================================================================
//
// [TS]
//
//==========================================================================
void FDynamicLight::Tick()
{
	if (!target)
	{
		// How did we get here? :?
		UnlinkLight();
		ReleaseLight();
		return;
	}

	if (owned)
	{
		if (!target->state || !target->ShouldRenderLocally())
		{
			Deactivate();
			return;
		}
		if (target->flags & MF_UNMORPHED)
		{
			m_active = false;
			return;
		}
		visibletoplayer = target->IsVisibleToPlayer();	// cache this value for the renderer to speed up calculations.
	}

	// Don't bother if the light won't be shown
	if (!IsActive()) return;

	// I am doing this with a type field so that I can dynamically alter the type of light
	// without having to create or maintain multiple objects.
	switch(lighttype)
	{
	case PulseLight:
	{
		const int timer = GetTimer();
		float diff = (timer - m_lastUpdate) / (float)TICRATE;

		m_lastUpdate = timer;
		m_cycler.Update(diff);
		m_currentRadius = float(m_cycler.GetVal());
		break;
	}

	case FlickerLight:
	{
		int rnd = randLight(360);
		m_currentRadius = float((rnd < int(specialf1))? GetIntensity() : GetSecondaryIntensity());
		break;
	}

	case RandomFlickerLight:
	{
		int flickerRange = GetSecondaryIntensity() - GetIntensity();
		float amt = randLight() / 255.f;

		if (m_tickCount > specialf1)
		{
			m_tickCount = 0;
		}
		if (m_tickCount++ == 0 || m_currentRadius > GetSecondaryIntensity())
		{
			m_currentRadius = float(GetIntensity() + (amt * flickerRange));
		}
		break;
	}

#if 0
	// These need some more work elsewhere
	case ColorFlickerLight:
	{
		int rnd = randLight();
		float pct = specialf1/360.f;

		m_currentRadius = m_Radius[rnd >= pct * 255];
		break;
	}

	case RandomColorFlickerLight:
	{
		int flickerRange = GetSecondaryIntensity() - GetIntensity();
		float amt = randLight() / 255.f;

		m_tickCount++;

		if (m_tickCount > specialf1)
		{
			m_currentRadius = GetIntensity() + (amt * flickerRange);
			m_tickCount = 0;
		}
		break;
	}
#endif

	case SectorLight:
	{
		float intensity;
		float scale = GetIntensity() / 8.f;

		if (scale == 0.f) scale = 1.f;

		intensity = Sector? Sector->lightlevel * scale : 0;
		intensity = clamp<float>(intensity, 0.f, 255.f);

		m_currentRadius = intensity;
		break;
	}

	case PointLight:
		m_currentRadius = float(GetIntensity());
		break;
	}
	if (m_currentRadius > gl_light_max_intensity) m_currentRadius = gl_light_max_intensity;
	if (m_currentRadius <= 0) m_currentRadius = 1;
	UpdateLocation();
}




//==========================================================================
//
//
//
//==========================================================================
void FDynamicLight::UpdateLocation()
{
	double oldx= X();
	double oldy= Y();
	float oldradius = radius;

	if (IsActive())
	{
		dynlights_active_updates++;
		AActor *target = this->target;	// perform the read barrier only once.

		// [round2 B1] Held in a tracked pose: position, yaw and pitch come from
		// the hand or head, so a tic relink follows the hand rather than the
		// target. No m_off, no bob and no floor/ceiling clamp -- a held torch goes
		// where the hand goes. Not anchored, or no pose to read: as always.
		if (!ResolvePoseAnchor())
		{
			// Offset is calculated in relation to the owning actor.
			DAngle angle = target->Angles.Yaw;
			double s = angle.Sin();
			double c = angle.Cos();
			if (IsSpot())
			{
				Yaw = angle;
				if (!explicitpitch)
					Pitch = target->Angles.Pitch;
			}

			Pos = target->Vec3Offset(m_off.X * c + m_off.Y * s, m_off.X * s - m_off.Y * c, m_off.Z + target->GetBobOffset());
			Sector = target->subsector->sector;	// Get the render sector. target->Sector is the sector according to play logic.

			if (!(target->flags5 & MF5_NOINTERACTION))
			{
				// Some z-coordinate fudging to prevent the light from getting too close to the floor or ceiling planes. With proper attenuation this would render them invisible.
				// A distance of 5 is needed so that the light's effect doesn't become too small.
				// [SP] don't do this if +NOINTERACTION is set, since the object can fly right through floors and ceilings with that flag
				if (Z() < target->floorz + 5.) Pos.Z = target->floorz + 5.;
				else if (Z() > target->ceilingz - 5.) Pos.Z = target->ceilingz - 5.;
			}
		}

		// The radius being used here is always the maximum possible with the
		// current settings. This avoids constant relinking of flickering lights

		float intensity;

		if (lighttype == FlickerLight || lighttype == RandomFlickerLight || lighttype == PulseLight)
		{
			intensity = float(max(GetIntensity(), GetSecondaryIntensity()));
		}
		else
		{
			intensity = m_currentRadius;
		}
		radius = intensity * 2.0f;
		if (radius < m_currentRadius * 2) radius = m_currentRadius * 2;

		if (X() != oldx || Y() != oldy || radius != oldradius)
		{
			//Update the light lists
			dynlights_relink_calls++;
			LinkLight();
		}
	}
}

//=============================================================================
//
// Attempts to emplace the light node in the TMap
//
//=============================================================================


int FSection::Index() const
{
	return int(this - &sector->Level->sections.allSections[0]);
}

void FDynamicLight::AddLightNode(FSection *section, side_t *sidedef)
{
	if (section)
	{
		if(Level->lightlists.flat_dlist.SSize() <= section->Index())
		{
			Level->lightlists.flat_dlist.Resize(section->Index() + 1);
		}

		auto &flatLightList = Level->lightlists.flat_dlist[section->Index()];

		if (!flatLightList.CheckKey(this))
		{
			FLightNode * node = new FLightNode;
			node->lightsource = this;

			flatLightList.TryEmplace(this, node);
			touchlists.flat_tlist.SortedAddUnique(section);
			dynlights_linked_sectors++;	// [UZDXREMA] perf instrumentation
		}
	}
	else if (sidedef)
	{
		if(Level->lightlists.wall_dlist.SSize() <= sidedef->Index())
		{
			Level->lightlists.wall_dlist.Resize(sidedef->Index() + 1);
		}

		auto &wallLightList = Level->lightlists.wall_dlist[sidedef->Index()];

		if (!wallLightList.CheckKey(this))
		{
			FLightNode * node = new FLightNode;
			node->lightsource = this;

			wallLightList.TryEmplace(this, node);
			touchlists.wall_tlist.SortedAddUnique(sidedef);
			dynlights_linked_sides++;	// [UZDXREMA] perf instrumentation
		}
	}
}




//==========================================================================
//
// Gets the light's distance to a line
//
//==========================================================================

double FDynamicLight::DistToSeg(const DVector3 &pos, vertex_t *start, vertex_t *end)
{
	double u, px, py;

	double seg_dx = end->fX() - start->fX();
	double seg_dy = end->fY() - start->fY();
	double seg_length_sq = seg_dx * seg_dx + seg_dy * seg_dy;

	u = (((pos.X - start->fX()) * seg_dx) + (pos.Y - start->fY()) * seg_dy) / seg_length_sq;
	if (u < 0.) u = 0.; // clamp the test point to the line segment
	else if (u > 1.) u = 1.;

	px = start->fX() + (u * seg_dx);
	py = start->fY() + (u * seg_dy);

	px -= pos.X;
	py -= pos.Y;

	return (px*px) + (py*py);
}


//==========================================================================
//
// Collect all touched sidedefs and subsectors
// to sidedefs and sector parts.
//
//==========================================================================
struct LightLinkEntry
{
	FSection *sect;
	DVector3 pos;
};
static TArray<LightLinkEntry> collected_ss;

void FDynamicLight::CollectWithinRadius(const DVector3 &opos, FSection *section, float radius)
{
	if (!section) return;
	collected_ss.Clear();
	collected_ss.Push({ section, opos });
	section->validcount = dl_validcount;

	bool hitonesidedback = false;
	for (unsigned i = 0; i < collected_ss.Size(); i++)
	{
		if (collected_ss.Size() >= (unsigned int)gl_light_max_collected_subsectors)
			break;

		auto pos = collected_ss[i].pos;
		section = collected_ss[i].sect;

		AddLightNode(section, NULL);


		auto processSide = [&](side_t *sidedef, const vertex_t *v1, const vertex_t *v2)
		{
			auto linedef = sidedef->linedef;
			if (linedef && linedef->validcount != ::validcount)
			{
				// light is in front of the seg
				if ((pos.Y - v1->fY()) * (v2->fX() - v1->fX()) + (v1->fX() - pos.X) * (v2->fY() - v1->fY()) <= 0)
				{
					linedef->validcount = ::validcount;

					AddLightNode(NULL, sidedef);
				}
				else if (linedef->sidedef[0] == sidedef && linedef->sidedef[1] == nullptr)
				{
					hitonesidedback = true;
				}
			}
			if (linedef)
			{
				FLinePortal *port = linedef->getPortal();
				if (port && port->mType == PORTT_LINKED)
				{
					line_t *other = port->mDestination;
					if (other->validcount != ::validcount)
					{
						subsector_t *othersub = Level->PointInRenderSubsector(other->v1->fPos() + other->Delta() / 2);
						FSection *othersect = othersub->section;
						if (othersect->validcount != ::validcount)
						{
							othersect->validcount = ::validcount;
							collected_ss.Push({ othersect, PosRelative(other->frontsector->PortalGroup) });
						}
					}
				}
			}
		};

		for (auto &segment : section->segments)
		{
			// check distance from x/y to seg and if within radius add this seg and, if present the opposing subsector (lather/rinse/repeat)
			// If out of range we do not need to bother with this seg.
			if (DistToSeg(pos, segment.start, segment.end) <= radius)
			{
				auto sidedef = segment.sidedef;
				if (sidedef)
				{
					processSide(sidedef, segment.start, segment.end);
				}

				auto partner = segment.partner;
				if (partner)
				{
					FSection *sect = partner->section;
					if (sect != nullptr && sect->validcount != dl_validcount)
					{
						sect->validcount = dl_validcount;
						collected_ss.Push({ sect, pos });
					}
				}
			}
		}
		for (auto side : section->sides)
		{
			auto v1 = side->V1(), v2 = side->V2();
			if (DistToSeg(pos, v1, v2) <= radius)
			{
				processSide(side, v1, v2);
			}
		}
		sector_t *sec = section->sector;
		if (!sec->PortalBlocksSight(sector_t::ceiling))
		{
			line_t *other = section->segments[0].sidedef->linedef;
			if (sec->GetPortalPlaneZ(sector_t::ceiling) < Z() + radius)
			{
				DVector2 refpos = other->v1->fPos() + other->Delta() / 2 + sec->GetPortalDisplacement(sector_t::ceiling);
				subsector_t *othersub = Level->PointInRenderSubsector(refpos);
				FSection *othersect = othersub->section;
				if (othersect->validcount != dl_validcount)
				{
					othersect->validcount = dl_validcount;
					collected_ss.Push({ othersect, PosRelative(othersub->sector->PortalGroup) });
				}
			}
		}
		if (!sec->PortalBlocksSight(sector_t::floor))
		{
			line_t *other = section->segments[0].sidedef->linedef;
			if (sec->GetPortalPlaneZ(sector_t::floor) > Z() - radius)
			{
				DVector2 refpos = other->v1->fPos() + other->Delta() / 2 + sec->GetPortalDisplacement(sector_t::floor);
				subsector_t *othersub = Level->PointInRenderSubsector(refpos);
				FSection *othersect = othersub->section;
				if (othersect->validcount != dl_validcount)
				{
					othersect->validcount = dl_validcount;
					collected_ss.Push({ othersect, PosRelative(othersub->sector->PortalGroup) });
				}
			}
		}
	}
	dynlights_collected_subsectors += collected_ss.Size();
	shadowmapped = hitonesidedback && !DontShadowmap();
}

//==========================================================================
//
// Link the light into the world
//
//==========================================================================

void FDynamicLight::LinkLight()
{
	dynlights_link_calls++;	// [UZDXREMA] perf instrumentation
	UnlinkLight();
	LinkedPos = Pos;	// [round2 B1] where these lists were built from; see R_UpdatePoseAnchoredLights
	if (radius>0)
	{
		// passing in radius*radius allows us to do a distance check without any calls to sqrt
		FSection *sect = Level->PointInRenderSubsector(Pos)->section;

		dl_validcount++;
		::validcount++;
		CollectWithinRadius(Pos, sect, float(radius*radius));

	}
}


//==========================================================================
//
// Deletes the link lists
//
//==========================================================================
void FDynamicLight::UnlinkLight()
{
	dynlights_unlink_calls++;	// [UZDXREMA] perf instrumentation

	for(int i = 0; i < touchlists.wall_tlist.SSize(); i++)
	{
		auto sidedef = touchlists.wall_tlist[i];
		if (!sidedef) continue;

		if(Level->lightlists.wall_dlist.SSize() > sidedef->Index())
		{
			Level->lightlists.wall_dlist[sidedef->Index()].Remove(this);
			dynlights_removed_side_links++;	// [UZDXREMA] perf instrumentation
		}
	}

	for(int i = 0; i < touchlists.flat_tlist.SSize(); i++)
	{
		auto sec = touchlists.flat_tlist[i];
		if (!sec) continue;

		if(Level->lightlists.flat_dlist.SSize() > sec->Index())
		{
			Level->lightlists.flat_dlist[sec->Index()].Remove(this);
			dynlights_removed_sector_links++;	// [UZDXREMA] perf instrumentation
		}
	}

	touchlists.flat_tlist.Clear();
	touchlists.wall_tlist.Clear();

	shadowmapped = false;
}

//==========================================================================
//
//
//
//==========================================================================

void AActor::AttachLight(unsigned int count, const FLightDefaults *lightdef)
{
	if(ObjectFlags & OF_EuthanizeMe) return;

	FDynamicLight *light;

	if (count < AttachedLights.Size())
	{
		light = AttachedLights[count];
		assert(light != nullptr);
	}
	else
	{
		light = GetLight(Level);
		light->SetActor(this, true);
		AttachedLights.Push(light);
	}
	lightdef->ApplyProperties(light);
	light->UpdateLocation();
}

//==========================================================================
//
// per-state light adjustment
//
//==========================================================================

void AActor::SetDynamicLights()
{
	TArray<FInternalLightAssociation *> & LightAssociations = GetInfo()->LightAssociations;
	unsigned int count = 0;

	if (state == nullptr) return;

	for (const auto def : UserLights)
	{
		AttachLight(count++, def);
	}

	for (const auto asso : LightAssociations)
	{
		if (asso->Sprite() == sprite && (asso->Frame() == frame || asso->Frame() == -1))
		{
			AttachLight(count++, asso->Light());
		}
	}
	if (count == 0 && state->Light > 0)
	{
		for(int i= state->Light; StateLights[i] != nullptr; i++)
		{
			if (StateLights[i] != (FLightDefaults*)-1)
			{
				AttachLight(count++, StateLights[i]);
			}
		}
	}

	for(;count<AttachedLights.Size();count++)
	{
		AttachedLights[count]->Deactivate();
	}
}

//==========================================================================
//
//
//
//==========================================================================

void AActor::DeleteAttachedLights()
{
	for (auto l : AttachedLights)
	{
		l->UnlinkLight();
		l->ReleaseLight();
	}
	AttachedLights.Clear();
}

//==========================================================================
//
//
//
//==========================================================================
extern TDeletingArray<FLightDefaults *> LightDefaults;

unsigned FindUserLight(AActor *self, FName id, bool create = false)
{
	for (unsigned i = 0; i < self->UserLights.Size(); i++ )
	{
		if (self->UserLights[i]->GetName() == id) return i;
	}
	if (create)
	{
		auto li = new FLightDefaults(id);
		return self->UserLights.Push(li);
	}
	return ~0u;
}

//==========================================================================
//
//
//
//==========================================================================

int AttachLightDef(AActor *self, int _lightid, int _lightname)
{
	if(self->ObjectFlags & OF_EuthanizeMe) return 0;

	FName lightid = FName(ENamedName(_lightid));
	FName lightname = FName(ENamedName(_lightname));

	// Todo: Optimize. This may be too slow.
	auto lightdef = LightDefaults.FindEx([=](const auto &a) {
		return a->GetName() == lightname;
	});
	if (lightdef < LightDefaults.Size())
	{
		auto userlight = self->UserLights[FindUserLight(self, lightid, true)];
		userlight->CopyFrom(*LightDefaults[lightdef]);
		self->flags8 |= MF8_RECREATELIGHTS;
		self->Level->flags3 |= LEVEL3_LIGHTCREATED;
		return 1;
	}
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, A_AttachLightDef, AttachLightDef)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_NAME(lightid);
	PARAM_NAME(lightname);
	ACTION_RETURN_BOOL(AttachLightDef(self, lightid.GetIndex(), lightname.GetIndex()));
}

//==========================================================================
//
//
//
//==========================================================================

int AttachLightDirect(AActor *self, int _lightid, int type, int color, int radius1, int radius2, int flags, double ofs_x, double ofs_y, double ofs_z, double param, double spoti, double spoto, double spotp, double intensity)
{
	if(self->ObjectFlags & OF_EuthanizeMe) return 0;

	FName lightid = FName(ENamedName(_lightid));
	auto userlight = self->UserLights[FindUserLight(self, lightid, true)];
	userlight->SetType(ELightType(type));
	userlight->SetArg(LIGHT_RED, RPART(color));
	userlight->SetArg(LIGHT_GREEN, GPART(color));
	userlight->SetArg(LIGHT_BLUE, BPART(color));
	userlight->SetArg(LIGHT_INTENSITY, radius1);
	userlight->SetArg(LIGHT_SECONDARY_INTENSITY, radius2);
	userlight->SetFlags(LightFlags::FromInt(flags));
	float of[] = { float(ofs_x), float(ofs_z), float(ofs_y)};
	userlight->SetOffset(of);
	userlight->SetParameter(type == PulseLight? param*TICRATE : param*360.);
	userlight->SetSpotInnerAngle(spoti);
	userlight->SetSpotOuterAngle(spoto);
	userlight->SetLightDefIntensity(intensity);
	if (spotp >= -90. && spotp <= 90.)
	{
		userlight->SetSpotPitch(spotp);
	}
	else
	{
		userlight->UnsetSpotPitch();
	}
	self->flags8 |= MF8_RECREATELIGHTS;
	self->Level->flags3 |= LEVEL3_LIGHTCREATED;
	return 1;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, A_AttachLight, AttachLightDirect)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_NAME(lightid);
	PARAM_INT(type);
	PARAM_INT(color);
	PARAM_INT(radius1);
	PARAM_INT(radius2);
	PARAM_INT(flags);
	PARAM_FLOAT(ofs_x);
	PARAM_FLOAT(ofs_y);
	PARAM_FLOAT(ofs_z);
	PARAM_FLOAT(parami);
	PARAM_FLOAT(spoti);
	PARAM_FLOAT(spoto);
	PARAM_FLOAT(spotp);
	PARAM_FLOAT(intensity);
	ACTION_RETURN_BOOL(AttachLightDirect(self, lightid.GetIndex(), type, color, radius1, radius2, flags, ofs_x, ofs_y, ofs_z, parami, spoti, spoto, spotp, intensity));
}

//==========================================================================
//
//
//
//==========================================================================

int RemoveLight(AActor *self, int _lightid)
{
	FName lightid = FName(ENamedName(_lightid));
	auto userlight = FindUserLight(self, lightid, false);
	if (userlight < self->UserLights.Size())
	{
		delete self->UserLights[userlight];
		self->UserLights.Delete(userlight);
		self->flags8 |= MF8_RECREATELIGHTS;
		self->Level->flags3 |= LEVEL3_LIGHTCREATED;
		return 1;
	}
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, A_RemoveLight, RemoveLight)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_NAME(lightid);
	ACTION_RETURN_BOOL(RemoveLight(self, lightid.GetIndex()));
}

//==========================================================================
//
// [round2 B1] ANCHORED DYNAMIC LIGHTS. See FDynamicLight::PoseAnchor.
//
// SetPoseAnchor keeps PoseAnchoredLights in step with PoseAnchor. It is only
// called on the main thread (ApplyProperties, AttachLight, the script setter).
//
//==========================================================================

void FDynamicLight::SetPoseAnchor(int anchor, const DVector3 &offset)
{
	if (anchor < 0 || anchor > 3) anchor = 0;
	PoseAnchor = anchor;
	PoseAnchorOffset = offset;

	auto &registry = Level->PoseAnchoredLights;
	if (anchor != 0 && !PoseRegistered)
	{
		registry.Push(this);
		PoseRegistered = true;
	}
	else if (anchor == 0 && PoseRegistered)
	{
		unsigned i = registry.Find(this);
		if (i < registry.Size()) registry.Delete(i);
		PoseRegistered = false;
	}
}

// Pose this light from its anchor. Called by the tick (UpdateLocation) and,
// every frame, by hw_entrypoint.cpp's R_UpdatePoseAnchoredLights. Writes only
// this client-side light's own Pos/Yaw/Pitch/Sector -- lights are not part of
// the simulation and no gameplay code reads where one is.
bool FDynamicLight::ResolvePoseAnchor()
{
	if (PoseAnchor <= 0) return false;

	// Whose pose: the player whose pawn this light is attached to, so in a
	// network game each player's torch follows that player's hand. A light on any
	// other actor reads the local player's, as volumetric beams do.
	AActor *owner = target;	// one read barrier
	const player_t *who = (owner != nullptr && owner->player != nullptr && owner->player->mo == owner)
		? owner->player : nullptr;

	DVector3 pos;
	DAngle yaw, pitch;
	const int source = ResolveTrackedPose(Level, PoseAnchor, PoseAnchorOffset, pos, yaw, pitch, who);
	if (source != PoseSource)
	{
		PoseSource = source;
		if (r_visualstate_log)
			Printf("dynlight: light on %s (anchor %d) pose source: %s\n",
				owner != nullptr ? owner->GetClass()->TypeName.GetChars() : "(no actor)",
				PoseAnchor, TrackedPoseSourceName(source));
	}
	if (source == TPOSE_NONE || source == TPOSE_NOPLAYER) return false;

	Pos = pos;
	Yaw = yaw;
	Pitch = pitch;
	// The render sector at the hand, not the target's: PosRelative reads its
	// portal group.
	Sector = Level->PointInRenderSubsector(Pos)->sector;
	return true;
}

int SetAttachedLightAnchor(AActor *self, int _lightid, int mode, double ox, double oy, double oz)
{
	if (mode < 0 || mode > 3)
	{
		static int lastBad = 0;	// 0 is valid, so it is never reported
		if (mode != lastBad)
		{
			lastBad = mode;
			Printf("SetAttachedLightAnchor: mode %d is outside 0..3, treated as 0\n", mode);
		}
		mode = 0;
	}
	const DVector3 offset(ox, oy, oz);

	// A light ACTOR (SpotLight and the rest) has one light of its own, built by
	// ::AttachLight from the actor's fields -- so the anchor goes on the actor,
	// where savegames and light rebuilds read it back from.
	if (self->IsKindOf(NAME_DynamicLight))
	{
		self->IntVar(NAME_PoseAnchor) = mode;
		*(DVector3*)self->ScriptVar(NAME_PoseAnchorOffset, nullptr) = offset;
		for (auto l : self->AttachedLights) l->SetPoseAnchor(mode, offset);
		return 1;
	}

	// Otherwise the light id, found the way A_RemoveLight finds it. The anchor
	// goes on the definition, which every rebuild applies.
	FName lightid = FName(ENamedName(_lightid));
	unsigned index = FindUserLight(self, lightid, false);
	if (index >= self->UserLights.Size()) return 0;
	self->UserLights[index]->SetPoseAnchor(mode, offset);

	// SetDynamicLights attaches user lights first and in order, so while no
	// rebuild is pending the live light is AttachedLights[index] and can take the
	// anchor now. With a rebuild pending (A_AttachLight or A_RemoveLight this
	// tic) the slots may not line up yet; the rebuild applies the definition.
	if (!(self->flags8 & MF8_RECREATELIGHTS) && index < self->AttachedLights.Size())
		self->AttachedLights[index]->SetPoseAnchor(mode, offset);
	return 1;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, SetAttachedLightAnchor, SetAttachedLightAnchor)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_NAME(lightid);
	PARAM_INT(mode);
	PARAM_FLOAT(ox);
	PARAM_FLOAT(oy);
	PARAM_FLOAT(oz);
	ACTION_RETURN_BOOL(SetAttachedLightAnchor(self, lightid.GetIndex(), mode, ox, oy, oz));
}


//==========================================================================
//
//
//
//==========================================================================

//==========================================================================
//
// This is called before saving the game
//
//==========================================================================

void FLevelLocals::DeleteAllAttachedLights()
{
	auto it = GetThinkerIterator<AActor>();
	AActor * a;

	while ((a=it.Next()))
	{
		a->DeleteAttachedLights();
	}
}

//==========================================================================
//
//
//
//==========================================================================

void FLevelLocals::RecreateAllAttachedLights()
{
	auto it = GetThinkerIterator<AActor>();
	AActor * a;

	while ((a=it.Next()))
	{
		if (!a->IsKindOf(NAME_DynamicLight))
		{
			a->SetDynamicLights();
		}
		else if (a->AttachedLights.Size() == 0)
		{
			::AttachLight(a);
			if (!(a->flags2 & MF2_DORMANT))
			{
				::ActivateLight(a);
			}
		}
	}
}
