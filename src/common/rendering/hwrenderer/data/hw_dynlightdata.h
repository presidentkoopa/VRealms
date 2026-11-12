/*
** hw_dynlightdata.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#ifndef __GLC_DYNLIGHT_H
#define __GLC_DYNLIGHT_H

#include "tarray.h"
#include "a_dynlight.h"
#include "hw_cvars.h"
#include "r_utility.h"

struct FDynLightData
{
	TArray<float> arrays[3];

	void Clear()
	{
		arrays[0].Clear();
		arrays[1].Clear();
		arrays[2].Clear();
	}

	void Combine(int *siz, int max)
	{
		siz[0] = arrays[0].Size();
		siz[1] = siz[0] + arrays[1].Size();
		siz[2] = siz[1] + arrays[2].Size();
		arrays[0].Resize(arrays[0].Size() + arrays[1].Size() + arrays[2].Size());
		memcpy(&arrays[0][siz[0]], &arrays[1][0], arrays[1].Size() * sizeof(float));
		memcpy(&arrays[0][siz[1]], &arrays[2][0], arrays[2].Size() * sizeof(float));
		siz[0]>>=2;
		siz[1]>>=2;
		siz[2]>>=2;
		if (siz[0] > max) siz[0] = max;
		if (siz[1] > max) siz[1] = max;
		if (siz[2] > max) siz[2] = max;
	}


};

extern unsigned int gl_dynlight_viewid;

inline DVector3 gl_GetLightPosRelative(FDynamicLight *light, int portalgroup)
{
	if (!gl_light_pos_relative_cache)
	{
		return light->PosRelative(portalgroup);
	}

	if (light->mPosRelativeCacheViewId == gl_dynlight_viewid && light->mPosRelativeCacheGroup == portalgroup)
	{
		return light->mPosRelativeCache;
	}

	light->mPosRelativeCache = light->PosRelative(portalgroup);
	light->mPosRelativeCacheViewId = gl_dynlight_viewid;
	light->mPosRelativeCacheGroup = portalgroup;
	return light->mPosRelativeCache;
}

template<typename CandidateT>
inline void gl_InsertBestLightCandidate(TArray<CandidateT> &candidates, const CandidateT &candidate, int limit)
{
	if (limit <= 0)
	{
		return;
	}

	candidates.Push(candidate);
	for (unsigned int i = candidates.Size() - 1; i > 0; --i)
	{
		if (candidates[i].Score >= candidates[i - 1].Score)
		{
			break;
		}

		CandidateT tmp = candidates[i - 1];
		candidates[i - 1] = candidates[i];
		candidates[i] = tmp;
	}

	if ((int)candidates.Size() > limit)
	{
		candidates.Pop();
	}
}

// [RS] Where the light is AS THE CAMERA'S PART OF THE MAP SEES IT. A light in a region joined to the
// camera's by linked portals is near the camera through the portal, however far apart the two regions
// were set on the map; measured raw, every light of a room set aside by the Realms loader (roth_portals,
// thousands of units away) was culled while the player stood at its door. The result is still one answer
// per light per view, so the cache below is unaffected.
inline DVector3 gl_LightPosForCull(FDynamicLight *light)
{
	if (r_viewpoint.sector != nullptr && light->Sector != nullptr)
		return light->PosRelative(r_viewpoint.sector->PortalGroup);
	return light->Pos;
}

inline bool gl_IsDistanceCulled(FDynamicLight *light)
{
	if (!gl_light_distance_cull_cache)
	{
		double dist3 = gl_light_distance_cull * gl_light_distance_cull;
		if (dist3 <= 0.0)
			return false;

		double dist1 = (gl_LightPosForCull(light) - r_viewpoint.Pos).LengthSquared();
		return dist1 > dist3;
	}

	if (light->mDistanceCullViewId == gl_dynlight_viewid)
		return light->mDistanceCullResult;

	double dist3 = gl_light_distance_cull * gl_light_distance_cull;
	if (dist3 <= 0.0)
	{
		light->mDistanceCullViewId = gl_dynlight_viewid;
		light->mDistanceCullResult = false;
		return false;
	}

	double dist1 = (gl_LightPosForCull(light) - r_viewpoint.Pos).LengthSquared();
	light->mDistanceCullViewId = gl_dynlight_viewid;
	light->mDistanceCullResult = dist1 > dist3;
	return light->mDistanceCullResult;
}

extern thread_local FDynLightData lightdata;


#endif
