/*
** a_dynlightdata.cpp
**
** Light definitions for actors.
**
**---------------------------------------------------------------------------
**
** Copyright 2003 Timothy Stump
** Copyright 2005-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "r_state.h"
#include "g_levellocals.h"
#include "a_dynlight.h"
#include "serializer.h"


//==========================================================================
//
// Dehacked aliasing
//
//==========================================================================

inline PClassActor * GetRealType(PClassActor * ti)
{
	PClassActor *rep = ti->GetReplacement(nullptr, false);
	if (rep != ti && rep != NULL && rep->IsDescendantOf(NAME_DehackedPickup))
	{
		return rep;
	}
	return ti;
}

TDeletingArray<FLightDefaults *> LightDefaults;
int AttenuationIsSet = -1;

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

FSerializer &Serialize(FSerializer &arc, const char *key, FLightDefaults &value, FLightDefaults *def)
{
	if (arc.BeginObject(key))
	{
		arc("name", value.m_Name)
			.Array("args", value.m_Args, 5)
			("param", value.m_Param)
			("pos", value.m_Pos)
			("type", value.m_type)
			("attenuate", value.m_attenuate)
			("flags", value.m_lightFlags)
			("swapped", value.m_swapped)
			("spot", value.m_spot)
			("explicitpitch", value.m_explicitPitch)
			("spotinner", value.m_spotInnerAngle)
			("spotouter", value.m_spotOuterAngle)
			("pitch", value.m_pitch)
			("lightdefintensity", value.m_LightDefIntensity)
			// [round2 B1] Additive keys: an old save lacks them and loads unanchored.
			("poseanchor", value.m_poseAnchor)
			("poseanchoroffset", value.m_poseAnchorOffset)
		.EndObject();
	}
	return arc;
}

FSerializer &Serialize(FSerializer &arc, const char *key, TDeletingArray<FLightDefaults *> &value, TDeletingArray<FLightDefaults *> *def)
{
	if (arc.isWriting())
	{
		if (value.Size() == 0) return arc;	// do not save empty arrays
	}
	bool res = arc.BeginArray(key);
	if (arc.isReading())
	{
		if (!res)
		{
			value.Clear();
			return arc;
		}
		value.Resize(arc.ArraySize());
		for (auto &entry : value) entry = new FLightDefaults(NAME_None);
	}
	for (unsigned i = 0; i < value.Size(); i++)
	{
		Serialize(arc, nullptr, *value[i], nullptr);
	}
	arc.EndArray();
	return arc;
}


//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

void FLightDefaults::ApplyProperties(FDynamicLight * light) const
{
	auto oldtype = light->lighttype;

	light->m_active = true;
	light->lighttype = m_type;
	light->specialf1 = m_Param;
	light->lightDefIntensity = m_LightDefIntensity;
	light->pArgs = m_Args;
	light->pLightFlags = &m_lightFlags;
	if (m_lightFlags & LF_SPOT)
	{
		light->pSpotInnerAngle = &m_spotInnerAngle;
		light->pSpotOuterAngle = &m_spotOuterAngle;
		light->explicitpitch   = m_explicitPitch;
		light->Yaw             = light->target->Angles.Yaw;
		if (m_explicitPitch) light->Pitch = m_pitch;
		else light->Pitch = light->target->Angles.Pitch;
	}
	light->m_tickCount = 0;
	if (m_type == PulseLight)
	{
		float pulseTime = float(m_Param / TICRATE);

		light->m_lastUpdate = light->GetTimer();
		if (m_swapped) light->m_cycler.SetParams(float(m_Args[LIGHT_SECONDARY_INTENSITY]), float(m_Args[LIGHT_INTENSITY]), pulseTime, oldtype == PulseLight);
		else light->m_cycler.SetParams(float(m_Args[LIGHT_INTENSITY]), float(m_Args[LIGHT_SECONDARY_INTENSITY]), pulseTime, oldtype == PulseLight);
		light->m_cycler.ShouldCycle(true);
		light->m_cycler.SetCycleType(CYCLE_Sin);
		light->m_currentRadius = (float)light->m_cycler.GetVal();
		if (light->m_currentRadius <= 0) light->m_currentRadius = 1;
		light->swapped = m_swapped;
	}
	// [round2 B1] Every (re)build re-applies the definition's anchor; 0 takes
	// the light back out of the anchored registry if this slot held one.
	light->SetPoseAnchor(m_poseAnchor, m_poseAnchorOffset);
	light->SetOffset(m_Pos);	// this must be the last thing to do.
}

// [LIGHTLIFETIME] See the declaration. Matched on any of the four pointers, not by
// slot: while a rebuild is pending the slots need not line up with UserLights, and
// a slot reused by a non-spot definition still holds an older spot's angle pointers.
void FLightDefaults::ReleaseLightsBuiltFrom(const TArray<FDynamicLight *> &lights) const
{
	static const int inertArgs[5] = { 0,0,0,0,0 };
	static const LightFlags inertFlags = 0;
	static const DAngle inertAngle = nullAngle;

	for (auto light : lights)
	{
		if (light == nullptr) continue;
		if (light->pArgs != m_Args && light->pLightFlags != &m_lightFlags &&
			light->pSpotInnerAngle != &m_spotInnerAngle && light->pSpotOuterAngle != &m_spotOuterAngle) continue;

		light->UnlinkLight();
		light->Deactivate();
		light->SetPoseAnchor(0, DVector3(0, 0, 0));
		light->pArgs = inertArgs;
		light->pLightFlags = &inertFlags;
		light->pSpotInnerAngle = &inertAngle;
		light->pSpotOuterAngle = &inertAngle;
	}
}

void FLightDefaults::SetAttenuationForLevel(bool yes)
{
	if (AttenuationIsSet != int(yes))
	{
		for (auto ldef : LightDefaults)
		{
			if (ldef->m_attenuate == -1)
			{
				if (yes)  ldef->m_lightFlags |= LF_ATTENUATE; else ldef->m_lightFlags &= ~LF_ATTENUATE;
			}
		}
		AttenuationIsSet = yes;
	}
}

//==========================================================================
//
// light definition file parser
//
//==========================================================================


extern int ScriptDepth;

void AddLightDefaults(FLightDefaults *defaults, double attnFactor)
{
   FLightDefaults *temp;
   unsigned int i;

   // remove duplicates
   for (i = 0; i < LightDefaults.Size(); i++)
   {
	  temp = LightDefaults[i];
	  if (temp->GetName() == defaults->GetName())
	  {
		 delete temp;
		 LightDefaults.Delete(i);
		 break;
	  }
   }
   if (defaults->GetAttenuate())
   {
	   defaults->SetArg(LIGHT_INTENSITY, int(defaults->GetArg(LIGHT_INTENSITY) * attnFactor));
	   defaults->SetArg(LIGHT_SECONDARY_INTENSITY, int(defaults->GetArg(LIGHT_SECONDARY_INTENSITY) * attnFactor));
   }

   LightDefaults.Push(defaults);
}

//==========================================================================
//
//
//
//==========================================================================

FInternalLightAssociation::FInternalLightAssociation(FLightAssociation * asso)
{

	m_AssocLight=NULL;
	for(unsigned int i=0;i<LightDefaults.Size();i++)
	{
		if (LightDefaults[i]->GetName() == asso->Light())
		{
			m_AssocLight = LightDefaults[i];
			break;
		}
	}

	m_sprite=-1;
	m_frame = -1;
	for (unsigned i = 0; i < sprites.Size (); ++i)
	{
		if (strncmp (sprites[i].name, asso->FrameName(), 4) == 0)
		{
			m_sprite = (int)i;
			break;
		}
	}

	// Only handle lights for full frames.
	// I won't bother with special lights for single rotations
	// because there is no decent use for them!
	if (strlen(asso->FrameName())==5 || asso->FrameName()[5]=='0')
	{
		m_frame = toupper(asso->FrameName()[4])-'A';
	}
}

//==========================================================================
//
// State lights
//
//==========================================================================
static TArray<FName> ParsedStateLights;
TArray<FLightDefaults *> StateLights;

//==========================================================================
//
//
//
//==========================================================================

void InitializeActorLights(TArray<FLightAssociation> &LightAssociations)
{
	for(unsigned int i=0;i<LightAssociations.Size();i++)
	{
		PClassActor * ti = PClass::FindActor(LightAssociations[i].ActorName());
		if (ti)
		{
			ti = GetRealType(ti);
			// put this in the class data arena so that we do not have to worry about deleting it ourselves.
			void *mem = ClassDataAllocator.Alloc(sizeof(FInternalLightAssociation));
			FInternalLightAssociation * iasso = new(mem) FInternalLightAssociation(&LightAssociations[i]);
			if (iasso->Light() != nullptr)
				ti->ActorInfo()->LightAssociations.Push(iasso);
		}
	}

	StateLights.Resize(ParsedStateLights.Size()+1);
	for(unsigned i=0; i<ParsedStateLights.Size();i++)
	{
		if (ParsedStateLights[i] != NAME_None)
		{
			StateLights[i] = (FLightDefaults*)-1;	// something invalid that's not NULL.
			for(unsigned int j=0;j<LightDefaults.Size();j++)
			{
				if (LightDefaults[j]->GetName() == ParsedStateLights[i])
				{
					StateLights[i] = LightDefaults[j];
					break;
				}
			}
		}
		else StateLights[i] = NULL;
	}
	StateLights[StateLights.Size()-1] = NULL;	// terminator
	ParsedStateLights.Clear();
	ParsedStateLights.ShrinkToFit();
}

//==========================================================================
//
//
//
//==========================================================================

void AddStateLight(FState *State, const char *lname)
{
	if (State->Light == 0)
	{
		ParsedStateLights.Push(NAME_None);
		State->Light = ParsedStateLights.Push(FName(lname));
	}
	else
	{
		ParsedStateLights.Push(FName(lname));
	}
}
