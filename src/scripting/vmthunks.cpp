/*
** vmthunks.cpp
**
** VM thunks for internal functions.
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Important note about this file:
** Since everything in here is supposed to be called from JIT-compiled VM code,
** it needs to be very careful about calling conventions. As a result none of
** the integer sized struct types may be used as function arguments, because
** current C++ calling conventions require them to be passed by reference.
** The JIT code, however will pass them by value so any direct native function
** taking such an argument needs to receive it as a naked int.
*/

#include "hw_vrmodes.h"
#include <time.h>
#include "vm.h"
#include "r_defs.h"
#include "g_levellocals.h"
#include "gamedata/g_mapinfo.h"
#include "s_sound.h"
#include "p_local.h"
#include "v_font.h"
#include "gstrings.h"
#include "a_keys.h"
#include "sbar.h"
#include "doomstat.h"
#include "p_acs.h"
#include "r_data/models.h"
#include "matrix.h"
#include "a_pickups.h"
#include "a_specialspot.h"
#include "actorptrselect.h"
#include "a_weapons.h"
#include "d_player.h"
#include "r_utility.h"		// [BB] r_viewpoint, for the view-locked yaw bias in the billboard queries
#include "p_setup.h"
#include "am_map.h"
#include "v_video.h"
#include "gi.h"
#include "utf8.h"
#include "fontinternals.h"
#include "intermission/intermission.h"
#include "menu.h"
#include "c_cvars.h"
#include "c_bind.h"
#include "c_dispatch.h"
#include "s_music.h"
#include "texturemanager.h"
#include "v_draw.h"
#include "types.h"		// [BB] PType and the type singletons, for the field reflection natives below
#include "i_specialpaths.h"	// [BB] M_GetConfigPath, for JSON profile natives below
// serializer_rapidjson.h MUST come before any rapidjson header. It defines
// RAPIDJSON_48BITPOINTER_OPTIMIZATION 0 and the CXX11 feature macros, and
// RAPIDJSON_48BITPOINTER_OPTIMIZATION changes sizeof(GenericValue). Including
// rapidjson directly here gave this translation unit a different ValueType
// size to serializer.cpp's, an ODR violation that tripped
// "stack_.GetSize() == sizeof(ValueType)" in GenericDocument::ParseStream on
// every level load.
#include "serializer_rapidjson.h"
#include "rapidjson/stringbuffer.h"
#include <fstream>
#include <sstream>
#include <cmath>	// [BLOOMOVERRIDE] std::isfinite in SetBloomOverride
#include <initializer_list>	// [SMOKEVOLUME] EffectArgsFinite
#include "volumedefs.h"			// [EMISSIVEVOLUMES] EmissiveVolumeDefinitionHandle
#include "hw_emissivevolumes.h"	// [EMISSIVEVOLUMES] EmissiveVolumeDefinitionEnabled
#include "d_net.h"

extern int paused;
extern bool pauseext;

DVector2 AM_GetPosition();
int Net_GetLatency(int *ld, int *ad);
void PrintPickupMessage(bool localview, const FString &str);


void SetCameraToTexture(AActor *viewpoint, const FString &texturename, double fov);

DEFINE_ACTION_FUNCTION_NATIVE(_TexMan, SetCameraToTexture, SetCameraToTexture)
{
	PARAM_PROLOGUE;
	PARAM_OBJECT(viewpoint, AActor);
	PARAM_STRING(texturename); // [ZZ] there is no point in having this as FTextureID because it's easier to refer to a cameratexture by name and it isn't executed too often to cache it.
	PARAM_FLOAT(fov);
	SetCameraToTexture(viewpoint, texturename, fov);
	return 0;
}

static void SetCameraTextureAspectRatio(const FString &texturename, double aspectScale, bool useTextureRatio)
{
	FTextureID textureid = TexMan.CheckForTexture(texturename.GetChars(), ETextureType::Wall, FTextureManager::TEXMAN_Overridable);
	if (textureid.isValid())
	{
		// Only proceed if the texture actually has a canvas.
		auto tex = TexMan.GetGameTexture(textureid);
		if (tex && tex->isHardwareCanvas())
		{
			static_cast<FCanvasTexture *>(tex->GetTexture())->SetAspectRatio(aspectScale, useTextureRatio);
		}
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(_TexMan, SetCameraTextureAspectRatio, SetCameraTextureAspectRatio)
{
	PARAM_PROLOGUE;
	PARAM_STRING(texturename);
	PARAM_FLOAT(aspect);
	PARAM_BOOL(useTextureRatio);
	SetCameraTextureAspectRatio(texturename, aspect, useTextureRatio);
	return 0;
}

static void SetCanvasTextureTranslucent(const FString& texturename, bool translucent)
{
	FTextureID textureid = TexMan.CheckForTexture(texturename.GetChars(), ETextureType::Wall, FTextureManager::TEXMAN_Overridable);
	if (textureid.isValid())
	{
		auto tex = TexMan.GetGameTexture(textureid);
		if (tex && tex->isHardwareCanvas())
		{
			static_cast<FCanvasTexture*>(tex->GetTexture())->bTranslucentCanvas = translucent;
		}
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(_TexMan, SetCanvasTextureTranslucent, SetCanvasTextureTranslucent)
{
	PARAM_PROLOGUE;
	PARAM_STRING(texturename);
	PARAM_BOOL(translucent);
	SetCanvasTextureTranslucent(texturename, translucent);
	return 0;
}

//=====================================================================================
//
// sector_t exports
//
//=====================================================================================

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindLowestFloorSurrounding, FindLowestFloorSurrounding)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	vertex_t *v;
	double h = FindLowestFloorSurrounding(self, &v);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(v);
	return numret;
}


DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindHighestFloorSurrounding, FindHighestFloorSurrounding)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	vertex_t *v;
	double h = FindHighestFloorSurrounding(self, &v);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(v);
	return numret;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindNextHighestFloor, FindNextHighestFloor)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	vertex_t *v;
	double h = FindNextHighestFloor(self, &v);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(v);
	return numret;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindNextLowestFloor, FindNextLowestFloor)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	vertex_t *v;
	double h = FindNextLowestFloor(self, &v);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(v);
	return numret;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindNextLowestCeiling, FindNextLowestCeiling)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	vertex_t *v;
	double h = FindNextLowestCeiling(self, &v);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(v);
	return numret;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindNextHighestCeiling, FindNextHighestCeiling)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	vertex_t *v;
	double h = FindNextHighestCeiling(self, &v);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(v);
	return numret;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindLowestCeilingSurrounding, FindLowestCeilingSurrounding)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	vertex_t *v;
	double h = FindLowestCeilingSurrounding(self, &v);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(v);
	return numret;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindHighestCeilingSurrounding, FindHighestCeilingSurrounding)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	vertex_t *v;
	double h = FindHighestCeilingSurrounding(self, &v);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(v);
	return numret;
}


DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindMinSurroundingLight, FindMinSurroundingLight)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(min);
	auto h = FindMinSurroundingLight(self, min);
	ACTION_RETURN_INT(h);
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindHighestFloorPoint, FindHighestFloorPoint)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	vertex_t *v;
	double h = FindHighestFloorPoint(self, &v);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(v);
	return numret;
}
DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindLowestCeilingPoint, FindLowestCeilingPoint)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	vertex_t *v;
	double h = FindLowestCeilingPoint(self, &v);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(v);
	return numret;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, HighestCeilingAt, HighestCeilingAt)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	sector_t *s;
	double h = HighestCeilingAt(self, x, y, &s);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(s);
	return numret;
}
DEFINE_ACTION_FUNCTION_NATIVE(_Sector, LowestFloorAt, LowestFloorAt)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	sector_t *s;
	double h = LowestFloorAt(self, x, y, &s);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetPointer(s);
	return numret;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, NextHighestCeilingAt, NextHighestCeilingAt)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(bottomz);
	PARAM_FLOAT(topz);
	PARAM_INT(flags);
	sector_t *resultsec;
	F3DFloor *resultff;
	double resultheight = NextHighestCeilingAt(self, x, y, bottomz, topz, flags, &resultsec, &resultff);

	if (numret > 2) ret[2].SetPointer(resultff);
	if (numret > 1) ret[1].SetPointer(resultsec);
	if (numret > 0) ret[0].SetFloat(resultheight);
	return numret;
}
DEFINE_ACTION_FUNCTION_NATIVE(_Sector, NextLowestFloorAt, NextLowestFloorAt)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);
	PARAM_INT(flags);
	PARAM_FLOAT(steph);
	sector_t *resultsec;
	F3DFloor *resultff;
	double resultheight = NextLowestFloorAt(self, x, y, z, flags, steph, &resultsec, &resultff);

	if (numret > 2) ret[2].SetPointer(resultff);
	if (numret > 1) ret[1].SetPointer(resultsec);
	if (numret > 0) ret[0].SetFloat(resultheight);
	return numret;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetFriction, GetFriction)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(plane);
	double mf;
	double h = self->GetFriction(plane, &mf);
	if (numret > 0) ret[0].SetFloat(h);
	if (numret > 1) ret[1].SetFloat(mf);
	return numret;
}


static void GetPortalDisplacement(sector_t *sec, int plane, DVector2 *result)
{
	*result = sec->GetPortalDisplacement(plane);
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetPortalDisplacement, GetPortalDisplacement)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(pos);
	ACTION_RETURN_VEC2(self->GetPortalDisplacement(pos));
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindShortestTextureAround, FindShortestTextureAround)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	ACTION_RETURN_FLOAT(FindShortestTextureAround(self));
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindShortestUpperAround, FindShortestUpperAround)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	ACTION_RETURN_FLOAT(FindShortestUpperAround(self));
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindModelFloorSector, FindModelFloorSector)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_FLOAT(fdh);
	auto h = FindModelFloorSector(self, fdh);
	ACTION_RETURN_POINTER(h);
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, FindModelCeilingSector, FindModelCeilingSector)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_FLOAT(fdh);
	auto h = FindModelCeilingSector(self, fdh);
	ACTION_RETURN_POINTER(h);
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetColor, SetColor)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_COLOR(color);
	PARAM_INT(desat);
	SetColor(self, color, desat);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetFade, SetFade)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_COLOR(fade);
	SetFade(self, fade);
	return 0;
}

static void SetSpecialColor(sector_t *self, int num, int color)
{
	if (num >= 0 && num < 5)
	{
		self->SetSpecialColor(num, color);
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetSpecialColor, SetSpecialColor)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(num);
	PARAM_COLOR(color);
	SetSpecialColor(self, num, color);
	return 0;
}

static void SetAdditiveColor(sector_t *self, int pos, int color)
{
	if (pos >= 0 && pos < 5)
	{
		self->SetAdditiveColor(pos, color);
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetAdditiveColor, SetAdditiveColor)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(pos);
	PARAM_COLOR(color);
	SetAdditiveColor(self, pos, color);
	return 0;
}

static void SetColorization(sector_t* self, int pos, int cname)
{
	if (pos >= 0 && pos < 2)
	{
		self->SetTextureFx(pos, TexMan.GetTextureManipulation(ENamedName(cname)));
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetColorization, SetColorization)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(pos);
	PARAM_INT(color);
	SetColorization(self, pos, color);
	return 0;
}


static void SetFogDensity(sector_t *self, int dens)
{
	self->Colormap.FogDensity = dens;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetFogDensity, SetFogDensity)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(dens);
	self->Colormap.FogDensity = dens;
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, PlaneMoving, PlaneMoving)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(pos);
	ACTION_RETURN_BOOL(PlaneMoving(self, pos));
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetFloorLight, GetFloorLight)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	ACTION_RETURN_INT(self->GetFloorLight());
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetCeilingLight, GetCeilingLight)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	ACTION_RETURN_INT(self->GetCeilingLight());
}

static sector_t *GetHeightSec(sector_t *self)
{
	return self->GetHeightSec();
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetHeightSec, GetHeightSec)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	ACTION_RETURN_POINTER(self->GetHeightSec());
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetSpecial, GetSpecial)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_POINTER(spec, secspecial_t);
	GetSpecial(self, spec);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetSpecial, SetSpecial)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_POINTER(spec, secspecial_t);
	SetSpecial(self, spec);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, TransferSpecial, TransferSpecial)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_POINTER(spec, sector_t);
	TransferSpecial(self, spec);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetTerrain, GetTerrain)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(pos);
	ACTION_RETURN_INT(GetTerrain(self, pos));
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetFloorTerrain, GetFloorTerrain_S)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(pos);
	ACTION_RETURN_POINTER(GetFloorTerrain_S(self, pos));
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, CheckPortalPlane, CheckPortalPlane)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(plane);
	self->CheckPortalPlane(plane);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, RemoveForceField, RemoveForceField)
 {
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	RemoveForceField(self);
	return 0;
 }

  DEFINE_ACTION_FUNCTION_NATIVE(_Sector, AdjustFloorClip, AdjustFloorClip)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 AdjustFloorClip(self);
	 return 0;
 }

int WorldPaused(bool checkLag)
{
	if (paused || (checkLag && Net_IsWaiting()))
		return true;

	if (netgame || gamestate != GS_LEVEL)
		return false;

	return pauseext || menuactive == MENU_On || ConsoleState == c_down || ConsoleState == c_falling;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, WorldPaused, WorldPaused)
{
	PARAM_PROLOGUE;
	PARAM_BOOL(checkLag);
	ACTION_RETURN_BOOL(WorldPaused(checkLag));
}

static sector_t *PointInSectorXY(FLevelLocals *self, double x, double y)
{
	return self->PointInSector(x ,y);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, PointInSector, PointInSectorXY)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	ACTION_RETURN_POINTER(PointInSectorXY(self, x, y));
}

static void SetXOffset(sector_t *self, int pos, double o)
{
	self->SetXOffset(pos, o);
}

DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetXOffset, SetXOffset)
{
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->SetXOffset(pos, o);
	 return 0;
 }

 // [BB] Flat-edge glow -- the plane's own surface, glowing inward from its
 // edges. Separate from SetGlowColor above, which only ever reaches the
 // wall, never the flat itself.
 static void SetFlatGlowColor(sector_t *self, int pos, int o)
 {
	 self->SetFlatGlowColor(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetFlatGlowColor, SetFlatGlowColor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_COLOR(o);
	 self->SetFlatGlowColor(pos, o);
	 return 0;
 }

 // [BB] The colour the flat glow fades toward -- see FlatGlowColorFar. Alpha
 // 0 leaves the glow the single flat colour it has always been.
 static void SetFlatGlowColorFar(sector_t *self, int pos, int o)
 {
	 self->SetFlatGlowColorFar(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetFlatGlowColorFar, SetFlatGlowColorFar)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_COLOR(o);
	 self->SetFlatGlowColorFar(pos, o);
	 return 0;
 }

 static void SetFlatGlowHeight(sector_t *self, int pos, double o)
 {
	 self->SetFlatGlowHeight(pos, float(o));
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetFlatGlowHeight, SetFlatGlowHeight)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->SetFlatGlowHeight(pos, float(o));
	 return 0;
 }

 static void SetFlatGlowFalloff(sector_t *self, int pos, int o)
 {
	 self->SetFlatGlowFalloff(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetFlatGlowFalloff, SetFlatGlowFalloff)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_INT(o);
	 self->SetFlatGlowFalloff(pos, o);
	 return 0;
 }

 static void SetFlatGlowIntensity(sector_t *self, int pos, double o)
 {
	 self->SetFlatGlowIntensity(pos, float(o));
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetFlatGlowIntensity, SetFlatGlowIntensity)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->SetFlatGlowIntensity(pos, float(o));
	 return 0;
 }

 static int GetFlatGlowColor(sector_t *self, int pos)
 {
	 return self->GetFlatGlowColor(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetFlatGlowColor, GetFlatGlowColor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_INT(self->GetFlatGlowColor(pos));
 }

 static double GetFlatGlowHeight(sector_t *self, int pos)
 {
	 return self->GetFlatGlowHeight(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetFlatGlowHeight, GetFlatGlowHeight)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(self->GetFlatGlowHeight(pos));
 }

 static void AddXOffset(sector_t *self, int pos, double o)
 {
	 self->AddXOffset(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, AddXOffset, AddXOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->AddXOffset(pos, o);
	 return 0;
 }

 static double GetXOffset(sector_t *self, int pos)
 {
	 return self->GetXOffset(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetXOffset, GetXOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(self->GetXOffset(pos));
 }

 static void SetYOffset(sector_t *self, int pos, double o)
 {
	 self->SetYOffset(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetYOffset, SetYOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->SetYOffset(pos, o);
	 return 0;
 }

 static void AddYOffset(sector_t *self, int pos, double o)
 {
	 self->AddYOffset(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, AddYOffset, AddYOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->AddYOffset(pos, o);
	 return 0;
 }

 static double GetYOffset(sector_t *self, int pos)
 {
	 return self->GetYOffset(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetYOffset, GetYOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_BOOL(addbase);
	 ACTION_RETURN_FLOAT(self->GetYOffset(pos, addbase));
 }

 static void SetXScale(sector_t *self, int pos, double o)
 {
	 self->SetXScale(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetXScale, SetXScale)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->SetXScale(pos, o);
	 return 0;
 }

 static double GetXScale(sector_t *self, int pos)
 {
	 return self->GetXScale(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetXScale, GetXScale)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(self->GetXScale(pos));
 }

 static void SetYScale(sector_t *self, int pos, double o)
 {
	 self->SetYScale(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetYScale, SetYScale)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->SetYScale(pos, o);
	 return 0;
 }

 static double GetYScale(sector_t *self, int pos)
 {
	 return self->GetYScale(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetYScale, GetYScale)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(self->GetYScale(pos));
 }

 static void SetAngle(sector_t *self, int pos, double o)
 {
	 self->SetAngle(pos, DAngle::fromDeg(o));
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetAngle, SetAngle)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_ANGLE(o);
	 self->SetAngle(pos, o);
	 return 0;
 }

 static double GetAngle(sector_t *self, int pos, bool addbase)
 {
	 return self->GetAngle(pos, addbase).Degrees();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetAngle, GetAngle)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_BOOL(addbase);
	 ACTION_RETURN_FLOAT(self->GetAngle(pos, addbase).Degrees());
 }

 static void SetBase(sector_t *self, int pos, double o, double a)
 {
	 self->SetBase(pos, o, DAngle::fromDeg(a));
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetBase, SetBase)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 PARAM_ANGLE(a);
	 self->SetBase(pos, o, a);
	 return 0;
 }

 static void SetAlpha(sector_t *self, int pos, double o)
 {
	 self->SetAlpha(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetAlpha, SetAlpha)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->SetAlpha(pos, o);
	 return 0;
 }

 static double GetAlpha(sector_t *self, int pos)
 {
	 return self->GetAlpha(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetAlpha, GetAlpha)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(self->GetAlpha(pos));
 }

 static int GetFlags(sector_t *self, int pos)
 {
	 return self->GetFlags(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetFlags, GetFlags)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_INT(self->GetFlags(pos));
 }

 static int GetVisFlags(sector_t *self, int pos)
 {
	 return self->GetVisFlags(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetVisFlags, GetVisFlags)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_INT(self->GetVisFlags(pos));
 }

 static void ChangeFlags(sector_t *self, int pos, int a, int o)
 {
	 self->ChangeFlags(pos, a, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, ChangeFlags, ChangeFlags)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_INT(a);
	 PARAM_INT(o);
	 self->ChangeFlags(pos, a, o);
	 return 0;
 }

 static void SetPlaneLight(sector_t *self, int pos, int o)
 {
	 self->SetPlaneLight(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetPlaneLight, SetPlaneLight)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_INT(o);
	 self->SetPlaneLight(pos, o);
	 return 0;
 }

 static int GetPlaneLight(sector_t *self, int pos)
 {
	 return self->GetPlaneLight(pos);
 }

  DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetPlaneLight, GetPlaneLight)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_INT(self->GetPlaneLight(pos));
 }

  static void SetTexture(sector_t *self, int pos, int o, bool adj)
  {
	  self->SetTexture(pos, FSetTextureID(o), adj);
  }

  DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetTexture, SetTexture)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_INT(o);
	 PARAM_BOOL(adj);
	 self->SetTexture(pos, FSetTextureID(o), adj);
	 return 0;
 }

  static int GetSectorTexture(sector_t *self, int pos)
  {
	  return self->GetTexture(pos).GetIndex();
  }

  DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetTexture, GetSectorTexture)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_INT(self->GetTexture(pos).GetIndex());
 }

  static void SetPlaneTexZ(sector_t *self, int pos, double o, bool)
  {
	  self->SetPlaneTexZ(pos, o, true);	// not setting 'dirty' here is a guaranteed cause for problems.
  }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetPlaneTexZ, SetPlaneTexZ)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 PARAM_BOOL(dirty);
	 self->SetPlaneTexZ(pos, o, true);	// not setting 'dirty' here is a guaranteed cause for problems.
	 return 0;
 }

 static double GetPlaneTexZ(sector_t *self, int pos)
 {
	 return self->GetPlaneTexZ(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetPlaneTexZ, GetPlaneTexZ)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(self->GetPlaneTexZ(pos));
 }

 static void SetLightLevel(sector_t *self, int o)
 {
	 self->SetLightLevel(o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetLightLevel, SetLightLevel)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(o);
	 self->SetLightLevel(o);
	 return 0;
 }

 static void ChangeLightLevel(sector_t *self, int o)
 {
	 self->ChangeLightLevel(o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, ChangeLightLevel, ChangeLightLevel)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(o);
	 self->ChangeLightLevel(o);
	 return 0;
 }

 static int GetLightLevel(sector_t *self)
 {
	 return self->GetLightLevel();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetLightLevel, GetLightLevel)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 ACTION_RETURN_INT(self->GetLightLevel());
 }

 static void SetPlaneReflectivity(sector_t* self, int pos, double val)
 {
	 if (pos < 0 || pos > 1) ThrowAbortException(X_ARRAY_OUT_OF_BOUNDS, "pos must be either 0 or 1");
	 self->SetPlaneReflectivity(pos, val);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetPlaneReflectivity, SetPlaneReflectivity)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(val)
	 SetPlaneReflectivity(self, pos, val);
	 return 0;
 }

 static double GetPlaneReflectivity(sector_t* self, int pos)
 {
	 if (pos < 0 || pos > 1) ThrowAbortException(X_ARRAY_OUT_OF_BOUNDS, "pos must be either 0 or 1");
	 return self->GetPlaneReflectivity(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetPlaneReflectivity, GetPlaneReflectivity)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(GetPlaneReflectivity(self, pos));
 }

 static int PortalBlocksView(sector_t *self, int pos)
 {
	 return self->PortalBlocksView(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, PortalBlocksView, PortalBlocksView)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_BOOL(self->PortalBlocksView(pos));
 }

 static int PortalBlocksSight(sector_t *self, int pos)
 {
	 return self->PortalBlocksSight(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, PortalBlocksSight, PortalBlocksSight)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_BOOL(self->PortalBlocksSight(pos));
 }

 static int PortalBlocksMovement(sector_t *self, int pos)
 {
	 return self->PortalBlocksMovement(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, PortalBlocksMovement, PortalBlocksMovement)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_BOOL(self->PortalBlocksMovement(pos));
 }

 static int PortalBlocksSound(sector_t *self, int pos)
 {
	 return self->PortalBlocksSound(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, PortalBlocksSound, PortalBlocksSound)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_BOOL(self->PortalBlocksSound(pos));
 }

 static int PortalIsLinked(sector_t *self, int pos)
 {
	 return self->PortalIsLinked(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, PortalIsLinked, PortalIsLinked)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_BOOL(self->PortalIsLinked(pos));
 }

 static void ClearPortal(sector_t *self, int pos)
 {
	 self->ClearPortal(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, ClearPortal, ClearPortal)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 self->ClearPortal(pos);
	 return 0;
 }

 static double GetPortalPlaneZ(sector_t *self, int pos)
 {
	 return self->GetPortalPlaneZ(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetPortalPlaneZ, GetPortalPlaneZ)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(self->GetPortalPlaneZ(pos));
 }

 static int GetPortalType(sector_t *self, int pos)
 {
	 return self->GetPortalType(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetPortalType, GetPortalType)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_INT(self->GetPortalType(pos));
 }

 static int GetOppositePortalGroup(sector_t *self, int pos)
 {
	 return self->GetOppositePortalGroup(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetOppositePortalGroup, GetOppositePortalGroup)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_INT(self->GetOppositePortalGroup(pos));
 }

 static double CenterFloor(sector_t *self)
 {
	 return self->CenterFloor();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, CenterFloor, CenterFloor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 ACTION_RETURN_FLOAT(self->CenterFloor());
 }

 static double CenterCeiling(sector_t *self)
 {
	 return self->CenterCeiling();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, CenterCeiling, CenterCeiling)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 ACTION_RETURN_FLOAT(self->CenterCeiling());
 }

 static int SectorIndex(sector_t *self)
 {
	 return self->Index();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, Index, SectorIndex)
 {
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	ACTION_RETURN_INT(SectorIndex(self));
 }

 static void SetEnvironmentID(sector_t *self, int envnum)
 {
	 self->Level->Zones[self->ZoneNumber].Environment = S_FindEnvironment(envnum);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetEnvironmentID, SetEnvironmentID)
 {
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_INT(envnum);
	SetEnvironmentID(self, envnum);
	return 0;
 }

 static void SetEnvironment(sector_t *self, const FString &env)
 {
	 self->Level->Zones[self->ZoneNumber].Environment = S_FindEnvironment(env.GetChars());
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetEnvironment, SetEnvironment)
 {
	PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	PARAM_STRING(env);
	SetEnvironment(self, env);
	return 0;
 }

 static double GetGlowHeight(sector_t *self, int pos)
 {
	 return self->GetGlowHeight(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetGlowHeight, GetGlowHeight)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(self->GetGlowHeight(pos));
 }

 static double GetGlowColor(sector_t *self, int pos)
 {
	 return self->GetGlowColor(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetGlowColor, GetGlowColor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_INT(self->GetGlowColor(pos));
 }

 static void SetGlowHeight(sector_t *self, int pos, double o)
 {
	 self->SetGlowHeight(pos, float(o));
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetGlowHeight, SetGlowHeight)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->SetGlowHeight(pos, float(o));
	 return 0;
 }

 static void SetGlowColor(sector_t *self, int pos, int o)
 {
	 self->SetGlowColor(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetGlowColor, SetGlowColor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_COLOR(o);
	 self->SetGlowColor(pos, o);
	 return 0;
 }

 // [BB] The colour the wall glow fades toward -- see GlowColorFar. Alpha 0
 // leaves the glow the single flat colour it has always been.
 static void SetGlowColorFar(sector_t *self, int pos, int o)
 {
	 self->SetGlowColorFar(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetGlowColorFar, SetGlowColorFar)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_COLOR(o);
	 self->SetGlowColorFar(pos, o);
	 return 0;
 }

 // [BB] Falloff shape and intensity for wall glow, matching what flat-edge
 // glow already exposes -- wf/wc now carry the same spec as fg/cg.
 static void SetGlowFalloff(sector_t *self, int pos, int o)
 {
	 self->SetGlowFalloff(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetGlowFalloff, SetGlowFalloff)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_INT(o);
	 self->SetGlowFalloff(pos, o);
	 return 0;
 }

 static void SetGlowIntensity(sector_t *self, int pos, double o)
 {
	 self->SetGlowIntensity(pos, float(o));
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetGlowIntensity, SetGlowIntensity)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(o);
	 self->SetGlowIntensity(pos, float(o));
	 return 0;
 }

 static int GetGlowFalloff(sector_t *self, int pos)
 {
	 return self->GetGlowFalloff(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetGlowFalloff, GetGlowFalloff)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_INT(self->GetGlowFalloff(pos));
 }

 static double GetGlowIntensity(sector_t *self, int pos)
 {
	 return self->GetGlowIntensity(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetGlowIntensity, GetGlowIntensity)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(self->GetGlowIntensity(pos));
 }

 static F3DFloor* Get3DFloor(sector_t *self, unsigned int index)
 {
 	 if (index >= self->e->XFloor.ffloors.Size())
 	 	return nullptr;
	 return self->e->XFloor.ffloors[index];
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, Get3DFloor, Get3DFloor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(index);
	 ACTION_RETURN_POINTER(Get3DFloor(self,index));
 }

 static int Get3DFloorCount(sector_t *self)
 {
	 return self->e->XFloor.ffloors.Size();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, Get3DFloorCount, Get3DFloorCount)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 ACTION_RETURN_INT(self->e->XFloor.ffloors.Size());
 }

 static sector_t* GetAttached(sector_t *self, unsigned int index)
 {
 	 if (index >= self->e->XFloor.attached.Size())
 	 	return nullptr;
	 return self->e->XFloor.attached[index];
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetAttached, GetAttached)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(index);
	 ACTION_RETURN_POINTER(GetAttached(self,index));
 }

 static int GetAttachedCount(sector_t *self)
 {
	 return self->e->XFloor.attached.Size();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetAttachedCount, GetAttachedCount)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 ACTION_RETURN_INT(self->e->XFloor.attached.Size());
 }

 static int CountSectorTags(const sector_t *self)
 {
	 return level.tagManager.CountSectorTags(self);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, CountTags, CountSectorTags)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 ACTION_RETURN_INT(level.tagManager.CountSectorTags(self));
 }

 static int GetSectorTag(const sector_t *self, int index)
 {
	 return level.tagManager.GetSectorTag(self, index);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Sector, GetTag, GetSectorTag)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(sector_t);
	 PARAM_INT(index);
	 ACTION_RETURN_INT(level.tagManager.GetSectorTag(self, index));
 }

 static int Get3DFloorTexture(F3DFloor *self, int pos)
 {
 	 if ( pos )
 		 return self->bottom.texture->GetIndex();
 	 return self->top.texture->GetIndex();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_F3DFloor, GetTexture, Get3DFloorTexture)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(F3DFloor);
	 PARAM_INT(pos);
	 if ( pos )
		 ACTION_RETURN_INT(self->bottom.texture->GetIndex());
	 ACTION_RETURN_INT(self->top.texture->GetIndex());
 }

 //===========================================================================
 //
 //  line_t exports
 //
 //===========================================================================

 static int isLinePortal(line_t *self)
 {
	 return self->isLinePortal();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Line, isLinePortal, isLinePortal)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 ACTION_RETURN_BOOL(self->isLinePortal());
 }

 static int isVisualPortal(line_t *self)
 {
	 return self->isVisualPortal();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Line, isVisualPortal, isVisualPortal)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 ACTION_RETURN_BOOL(self->isVisualPortal());
 }

 static line_t *getPortalDestination(line_t *self)
 {
	 return self->getPortalDestination();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Line, getPortalDestination, getPortalDestination)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 ACTION_RETURN_POINTER(self->getPortalDestination());
 }

 static int getPortalAlignment(line_t *self)
 {
	 return self->getPortalAlignment();
 }

 DEFINE_ACTION_FUNCTION(_Line, getPortalFlags)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 ACTION_RETURN_INT(self->getPortalFlags());
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Line, getPortalAlignment, getPortalAlignment)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 ACTION_RETURN_INT(self->getPortalAlignment());
 }

 DEFINE_ACTION_FUNCTION(_Line, getPortalType)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 ACTION_RETURN_INT(self->getPortalType());
 }

 DEFINE_ACTION_FUNCTION(_Line, getPortalDisplacement)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 ACTION_RETURN_VEC2(self->getPortalDisplacement());
 }

 DEFINE_ACTION_FUNCTION(_Line, getPortalAngleDiff)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 ACTION_RETURN_FLOAT(self->getPortalAngleDiff().Degrees());
 }

 static int LineIndex(line_t *self)
 {
	 return self->Index();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Line, Index, LineIndex)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 ACTION_RETURN_INT(LineIndex(self));
 }

 static int CountLineIDs(const line_t *self)
 {
	 return level.tagManager.CountLineIDs(self);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Line, CountIDs, CountLineIDs)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 ACTION_RETURN_INT(level.tagManager.CountLineIDs(self));
 }

 static int GetLineID(const line_t *self, int index)
 {
	 return level.tagManager.GetLineID(self, index);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Line, GetID, GetLineID)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(line_t);
	 PARAM_INT(index);
	 ACTION_RETURN_INT(level.tagManager.GetLineID(self, index));
 }

 //===========================================================================
 //
 // side_t exports
 //
 //===========================================================================

 // [BB] A wall's own ceiling glow and floor glow -- same names, same
 // Sector.floor/Sector.ceiling position argument as Sector.SetGlowColor,
 // just called on the wall instead of the room. Unset (alpha 0) means the
 // wall keeps showing its sector's glow, same as before; these only change
 // anything once something actually calls the setter.
 static void SetSideGlowColor(side_t *self, int pos, int o)
 {
	 self->SetGlowColor(pos, o);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, SetGlowColor, SetSideGlowColor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(pos);
	 PARAM_COLOR(o);
	 self->SetGlowColor(pos, o);
	 return 0;
 }

 static void SetSideGlowHeight(side_t *self, int pos, double height)
 {
	 self->SetGlowHeight(pos, (float)height);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, SetGlowHeight, SetSideGlowHeight)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(pos);
	 PARAM_FLOAT(height);
	 self->SetGlowHeight(pos, (float)height);
	 return 0;
 }

 static int GetSideGlowColor(side_t *self, int pos)
 {
	 return self->GetGlowColor(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, GetGlowColor, GetSideGlowColor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_INT(self->GetGlowColor(pos));
 }

 static double GetSideGlowHeight(side_t *self, int pos)
 {
	 return self->GetGlowHeight(pos);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, GetGlowHeight, GetSideGlowHeight)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(pos);
	 ACTION_RETURN_FLOAT(self->GetGlowHeight(pos));
 }

 static int GetSideTexture(side_t *self, int which)
 {
	return self->GetTexture(which).GetIndex();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, GetTexture, GetSideTexture)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 ACTION_RETURN_INT(self->GetTexture(which).GetIndex());
 }

 static void SetSideTexture(side_t *self, int which, int tex)
 {
	 self->SetTexture(which, FSetTextureID(tex));
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, SetTexture, SetSideTexture)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 PARAM_INT(tex);
	 self->SetTexture(which, FSetTextureID(tex));
	 return 0;
 }

 static void SetTextureXOffset(side_t *self, int which, double ofs)
 {
	 self->SetTextureXOffset(which, ofs);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, SetTextureXOffset, SetTextureXOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 PARAM_FLOAT(ofs);
	 self->SetTextureXOffset(which, ofs);
	 return 0;
 }

 static void AddTextureXOffset(side_t *self, int which, double ofs)
 {
	 self->AddTextureXOffset(which, ofs);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, AddTextureXOffset, AddTextureXOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 PARAM_FLOAT(ofs);
	 self->AddTextureXOffset(which, ofs);
	 return 0;
 }

 static double GetTextureXOffset(side_t *self, int which)
 {
	 return self->GetTextureXOffset(which);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, GetTextureXOffset, GetTextureXOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 ACTION_RETURN_FLOAT(self->GetTextureXOffset(which));
 }

 static void SetTextureYOffset(side_t *self, int which, double ofs)
 {
	 self->SetTextureYOffset(which, ofs);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, SetTextureYOffset, SetTextureYOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 PARAM_FLOAT(ofs);
	 self->SetTextureYOffset(which, ofs);
	 return 0;
 }

 static void AddTextureYOffset(side_t *self, int which, double ofs)
 {
	 self->AddTextureYOffset(which, ofs);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, AddTextureYOffset, AddTextureYOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 PARAM_FLOAT(ofs);
	 self->AddTextureYOffset(which, ofs);
	 return 0;
 }

 static double GetTextureYOffset(side_t *self, int which)
 {
	 return self->GetTextureYOffset(which);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, GetTextureYOffset, GetTextureYOffset)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 ACTION_RETURN_FLOAT(self->GetTextureYOffset(which));
 }

 static void SetTextureXScale(side_t *self, int which, double ofs)
 {
	 self->SetTextureXScale(which, ofs);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, SetTextureXScale, SetTextureXScale)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 PARAM_FLOAT(ofs);
	 self->SetTextureXScale(which, ofs);
	 return 0;
 }

 static void MultiplyTextureXScale(side_t *self, int which, double ofs)
 {
	 self->MultiplyTextureXScale(which, ofs);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, MultiplyTextureXScale, MultiplyTextureXScale)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 PARAM_FLOAT(ofs);
	 self->MultiplyTextureXScale(which, ofs);
	 return 0;
 }

 static double GetTextureXScale(side_t *self, int which)
 {
	 return self->GetTextureXScale(which);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, GetTextureXScale, GetTextureXScale)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 ACTION_RETURN_FLOAT(self->GetTextureXScale(which));
 }

 static void SetTextureYScale(side_t *self, int which, double ofs)
 {
	 self->SetTextureYScale(which, ofs);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, SetTextureYScale, SetTextureYScale)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 PARAM_FLOAT(ofs);
	 self->SetTextureYScale(which, ofs);
	 return 0;
 }

 static void MultiplyTextureYScale(side_t *self, int which, double ofs)
 {
	 self->MultiplyTextureYScale(which, ofs);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, MultiplyTextureYScale, MultiplyTextureYScale)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 PARAM_FLOAT(ofs);
	 self->MultiplyTextureYScale(which, ofs);
	 return 0;
 }

 static double GetTextureYScale(side_t *self, int which)
 {
	 return self->GetTextureYScale(which);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, GetTextureYScale, GetTextureYScale)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(which);
	 ACTION_RETURN_FLOAT(self->GetTextureYScale(which));
 }

 static vertex_t *GetSideV1(side_t *self)
 {
	 return self->V1();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, V1, GetSideV1)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 ACTION_RETURN_POINTER(self->V1());
 }

 static vertex_t *GetSideV2(side_t *self)
 {
	 return self->V2();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, V2, GetSideV2)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 ACTION_RETURN_POINTER(self->V2());
 }

 static int GetTextureFlags(side_t* self, int tier)
 {
	 return self->GetTextureFlags(tier);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, GetTextureFlags, GetTextureFlags)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(tier);
	 ACTION_RETURN_INT(self->GetTextureFlags(tier));
}

 static void ChangeTextureFlags(side_t* self, int tier, int And, int Or)
 {
	 self->ChangeTextureFlags(tier, And, Or);
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, ChangeTextureFlags, ChangeTextureFlags)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(tier);
	 PARAM_INT(a);
	 PARAM_INT(o);
	 ChangeTextureFlags(self, tier, a, o);
	 return 0;
 }

 static void SetSideSpecialColor(side_t *self, int tier, int position, int color, int useown)
 {
	 if (tier >= 0 && tier < 3 && position >= 0 && position < 2)
	 {
		 self->SetSpecialColor(tier, position, color, useown);
	 }
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, SetSpecialColor, SetSideSpecialColor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(tier);
	 PARAM_INT(position);
	 PARAM_COLOR(color);
	 PARAM_BOOL(useown)
	 SetSideSpecialColor(self, tier, position, color, useown);
	 return 0;
 }

 static int GetSideAdditiveColor(side_t *self, int tier)
 {
	 if (tier >= 0 && tier < 3)
	 {
		 return self->GetAdditiveColor(tier, self->sector);
	 }
	 return 0;
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, GetAdditiveColor, GetSideAdditiveColor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(tier);
	 ACTION_RETURN_INT(GetSideAdditiveColor(self, tier));
	 return 0;
 }

 static void SetSideAdditiveColor(side_t *self, int tier, int color)
 {
	 if (tier >= 0 && tier < 3)
	 {
		 self->SetAdditiveColor(tier, color);
	 }
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, SetAdditiveColor, SetSideAdditiveColor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(tier);
	 PARAM_COLOR(color);
	 SetSideAdditiveColor(self, tier, color);
	 return 0;
 }

 static void EnableSideAdditiveColor(side_t *self, int tier, bool enable)
 {
	 if (tier >= 0 && tier < 3)
	 {
		 self->EnableAdditiveColor(tier, enable);
	 }
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, EnableAdditiveColor, EnableSideAdditiveColor)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(tier);
	 PARAM_BOOL(enable);
	 EnableSideAdditiveColor(self, tier, enable);
	 return 0;
 }

 static void SetWallColorization(side_t* self, int pos, int cname)
 {
	 if (pos >= 0 && pos < 3)
	 {
		 self->SetTextureFx(pos, TexMan.GetTextureManipulation(ENamedName(cname)));
	 }
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, SetColorization, SetWallColorization)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 PARAM_INT(pos);
	 PARAM_INT(color);
	 SetWallColorization(self, pos, color);
	 return 0;
 }



 static int SideIndex(side_t *self)
 {
	 return self->Index();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Side, Index, SideIndex)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(side_t);
	 ACTION_RETURN_INT(SideIndex(self));
 }

 //=====================================================================================
//
 // vertex_t exports
//
//=====================================================================================

 static int VertexIndex(vertex_t *self)
 {
	 return self->Index();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(_Vertex, Index, VertexIndex)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(vertex_t);
	 ACTION_RETURN_INT(VertexIndex(self));
 }

 //=====================================================================================
//
// TexMan exports
//
//=====================================================================================

 static int IsJumpingAllowed(FLevelLocals *self)
 {
	 return self->IsJumpingAllowed();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, IsJumpingAllowed, IsJumpingAllowed)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	 ACTION_RETURN_BOOL(self->IsJumpingAllowed());
 }

 //==========================================================================
 //
 //
 //==========================================================================

 static int IsCrouchingAllowed(FLevelLocals *self)
 {
	 return self->IsCrouchingAllowed();
 }


 DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, IsCrouchingAllowed, IsCrouchingAllowed)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	 ACTION_RETURN_BOOL(self->IsCrouchingAllowed());
 }

 //==========================================================================
 //
 //
 //==========================================================================

 static int IsFreelookAllowed(FLevelLocals *self)
 {
	 return self->IsFreelookAllowed();
 }

 DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, IsFreelookAllowed, IsFreelookAllowed)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	 ACTION_RETURN_BOOL(self->IsFreelookAllowed());
 }

 //==========================================================================
//
// ZScript counterpart to ACS ChangeSky, uses TextureIDs
//
//==========================================================================
 DEFINE_ACTION_FUNCTION(FLevelLocals, ChangeSky)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	 PARAM_INT(sky1);
	 PARAM_INT(sky2);
	 self->skytexture1 = FSetTextureID(sky1);
	 self->skytexture2 = FSetTextureID(sky2);
	 InitSkyMap(self);
	 return 0;
 }

 DEFINE_ACTION_FUNCTION(FLevelLocals, ChangeSkyMist)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	 PARAM_INT(skymist);
	 PARAM_BOOL(usemist);
	 PARAM_FLOAT(skymistyscale);
	 self->skymisttexture = FSetTextureID(skymist);
	 if (usemist)
	 {
		 self->flags3 |= LEVEL3_SKYMIST;
	 }
	 else
	 {
		 self->flags3 &= ~LEVEL3_SKYMIST;
	 }
	 self->skymistyscale = clamp(skymistyscale, 0.002, 544.0);
	 InitSkyMap(self);
	 return 0;
 }

 DEFINE_ACTION_FUNCTION(FLevelLocals, SetSkyFog)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	 PARAM_INT(fogdensity);
	 self->skyfog = fogdensity;
	 InitSkyMap(self);
	 return 0;
 }

 DEFINE_ACTION_FUNCTION(FLevelLocals, SetThickFog)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	 PARAM_FLOAT(distance);
	 PARAM_FLOAT(multiplier);
	 self->thickfogdistance = distance;
	 if (multiplier > 0.0) self->thickfogmultiplier = multiplier;
	 return 0;
 }

 DEFINE_ACTION_FUNCTION(FLevelLocals, StartIntermission)
 {
	 PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	 PARAM_NAME(seq);
	 PARAM_INT(state);
	 G_StartSlideshow(self, seq, state);
	 return 0;
 }


 // This is needed to convert the strings to char pointers.
 static void ReplaceTextures(FLevelLocals *self, const FString &from, const FString &to, int flags)
 {
	 self->ReplaceTextures(from.GetChars(), to.GetChars(), flags);
 }

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ReplaceTextures, ReplaceTextures)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_STRING(from);
	PARAM_STRING(to);
	PARAM_INT(flags);
	self->ReplaceTextures(from.GetChars(), to.GetChars(), flags);
	return 0;
}

//=====================================================================================
//
// secplane_t exports
//
//=====================================================================================

static int isSlope(secplane_t *self)
{
	return !self->normal.XY().isZero();
}

DEFINE_ACTION_FUNCTION_NATIVE(_Secplane, isSlope, isSlope)
{
	PARAM_SELF_STRUCT_PROLOGUE(secplane_t);
	ACTION_RETURN_BOOL(!self->normal.XY().isZero());
}

static int PointOnSide(const secplane_t *self, double x, double y, double z)
{
	return self->PointOnSide(DVector3(x, y, z));
}

DEFINE_ACTION_FUNCTION_NATIVE(_Secplane, PointOnSide, PointOnSide)
{
	PARAM_SELF_STRUCT_PROLOGUE(secplane_t);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);
	ACTION_RETURN_INT(self->PointOnSide(DVector3(x, y, z)));
}

static double ZatPoint(const secplane_t *self, double x, double y)
{
	return self->ZatPoint(x, y);
}

DEFINE_ACTION_FUNCTION_NATIVE(_Secplane, ZatPoint, ZatPoint)
{
	PARAM_SELF_STRUCT_PROLOGUE(secplane_t);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	ACTION_RETURN_FLOAT(self->ZatPoint(x, y));
}

static double ZatPointDist(const secplane_t *self, double x, double y, double d)
{
	return (d + self->normal.X*x + self->normal.Y*y) * self->negiC;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Secplane, ZatPointDist, ZatPointDist)
{
	PARAM_SELF_STRUCT_PROLOGUE(secplane_t);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(d);
	ACTION_RETURN_FLOAT(ZatPointDist(self, x, y, d));
}

static int isPlaneEqual(const secplane_t *self, const secplane_t *other)
{
	return *self == *other;
}

DEFINE_ACTION_FUNCTION_NATIVE(_Secplane, isEqual, isPlaneEqual)
{
	PARAM_SELF_STRUCT_PROLOGUE(secplane_t);
	PARAM_POINTER(other, secplane_t);
	ACTION_RETURN_BOOL(*self == *other);
}

static void ChangeHeight(secplane_t *self, double hdiff)
{
	self->ChangeHeight(hdiff);
}

DEFINE_ACTION_FUNCTION_NATIVE(_Secplane, ChangeHeight, ChangeHeight)
{
	PARAM_SELF_STRUCT_PROLOGUE(secplane_t);
	PARAM_FLOAT(hdiff);
	self->ChangeHeight(hdiff);
	return 0;
}

static double GetChangedHeight(const secplane_t *self, double hdiff)
{
	return self->GetChangedHeight(hdiff);
}

DEFINE_ACTION_FUNCTION_NATIVE(_Secplane, GetChangedHeight, GetChangedHeight)
{
	PARAM_SELF_STRUCT_PROLOGUE(secplane_t);
	PARAM_FLOAT(hdiff);
	ACTION_RETURN_FLOAT(self->GetChangedHeight(hdiff));
}

static double HeightDiff(const secplane_t *self, double oldd, double newd)
{
	if (newd != 1e37)
	{
		return self->HeightDiff(oldd);
	}
	else
	{
		return self->HeightDiff(oldd, newd);
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(_Secplane, HeightDiff, HeightDiff)
{
	PARAM_SELF_STRUCT_PROLOGUE(secplane_t);
	PARAM_FLOAT(oldd);
	PARAM_FLOAT(newd);
	ACTION_RETURN_FLOAT(HeightDiff(self, oldd, newd));
}

static double PointToDist(const secplane_t *self, double x, double y, double z)
{
	return self->PointToDist(DVector2(x, y), z);
}


DEFINE_ACTION_FUNCTION_NATIVE(_Secplane, PointToDist, PointToDist)
{
	PARAM_SELF_STRUCT_PROLOGUE(secplane_t);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);
	ACTION_RETURN_FLOAT(self->PointToDist(DVector2(x, y), z));
}

//=====================================================================================
//
// WeaponSlots exports
//
//=====================================================================================

static int LocateWeapon(FWeaponSlots *self, PClassActor *weap, int *pslot, int *pindex)
{
	return self->LocateWeapon(weap, pslot, pindex);
}

DEFINE_ACTION_FUNCTION_NATIVE(FWeaponSlots, LocateWeapon, LocateWeapon)
{
	PARAM_SELF_STRUCT_PROLOGUE(FWeaponSlots);
	PARAM_CLASS(weap, AActor);
	int slot = 0, index = 0;
	bool retv = self->LocateWeapon(weap, &slot, &index);
	if (numret >= 1) ret[0].SetInt(retv);
	if (numret >= 2) ret[1].SetInt(slot);
	if (numret >= 3) ret[2].SetInt(index);
	return min(numret, 3);
}

static PClassActor *GetWeapon(FWeaponSlots *self, int slot, int index)
{
	return self->GetWeapon(slot, index);
}

DEFINE_ACTION_FUNCTION_NATIVE(FWeaponSlots, GetWeapon, GetWeapon)
{
	PARAM_SELF_STRUCT_PROLOGUE(FWeaponSlots);
	PARAM_INT(slot);
	PARAM_INT(index);
	ACTION_RETURN_POINTER(self->GetWeapon(slot, index));
	return 1;
}

static int SlotSize(FWeaponSlots *self, int slot)
{
	return self->SlotSize(slot);
}

DEFINE_ACTION_FUNCTION_NATIVE(FWeaponSlots, SlotSize, SlotSize)
{
	PARAM_SELF_STRUCT_PROLOGUE(FWeaponSlots);
	PARAM_INT(slot);
	ACTION_RETURN_INT(self->SlotSize(slot));
	return 1;
}

DEFINE_ACTION_FUNCTION_NATIVE(FWeaponSlots, SetupWeaponSlots, FWeaponSlots::SetupWeaponSlots)
{
	PARAM_PROLOGUE;
	PARAM_OBJECT(pawn, AActor);
	FWeaponSlots::SetupWeaponSlots(pawn);
	return 0;
}

//=====================================================================================
//
// SpotState exports
//
//=====================================================================================


static void AddSpot(DSpotState *state, AActor *spot)
{
	state->AddSpot(spot);
}

DEFINE_ACTION_FUNCTION_NATIVE(DSpotState, AddSpot, AddSpot)
{
	PARAM_SELF_PROLOGUE(DSpotState);
	PARAM_OBJECT(spot, AActor);
	self->AddSpot(spot);
	return 0;
}

static void RemoveSpot(DSpotState *state, AActor *spot)
{
	state->RemoveSpot(spot);
}

DEFINE_ACTION_FUNCTION_NATIVE(DSpotState, RemoveSpot, RemoveSpot)
{
	PARAM_SELF_PROLOGUE(DSpotState);
	PARAM_OBJECT(spot, AActor);
	self->RemoveSpot(spot);
	return 0;
}

static AActor *GetNextInList(DSpotState *self, PClassActor *type, int skipcounter)
{
	return self->GetNextInList(type, skipcounter);
}

DEFINE_ACTION_FUNCTION_NATIVE(DSpotState, GetNextInList, GetNextInList)
{
	PARAM_SELF_PROLOGUE(DSpotState);
	PARAM_CLASS(type, AActor);
	PARAM_INT(skipcounter);
	ACTION_RETURN_OBJECT(self->GetNextInList(type, skipcounter));
}

static AActor *GetSpotWithMinMaxDistance(DSpotState *self, PClassActor *type, double x, double y, double mindist, double maxdist)
{
	return self->GetSpotWithMinMaxDistance(type, x, y, mindist, maxdist);
}

DEFINE_ACTION_FUNCTION_NATIVE(DSpotState, GetSpotWithMinMaxDistance, GetSpotWithMinMaxDistance)
{
	PARAM_SELF_PROLOGUE(DSpotState);
	PARAM_CLASS(type, AActor);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(mindist);
	PARAM_FLOAT(maxdist);
	ACTION_RETURN_OBJECT(self->GetSpotWithMinMaxDistance(type, x, y, mindist, maxdist));
}

static AActor *GetRandomSpot(DSpotState *self, PClassActor *type, bool onlyonce)
{
	return self->GetRandomSpot(type, onlyonce);
}

DEFINE_ACTION_FUNCTION_NATIVE(DSpotState, GetRandomSpot, GetRandomSpot)
{
	PARAM_SELF_PROLOGUE(DSpotState);
	PARAM_CLASS(type, AActor);
	PARAM_BOOL(onlyonce);
	ACTION_RETURN_POINTER(self->GetRandomSpot(type, onlyonce));
}

//=====================================================================================
//
// Statusbar exports
//
//=====================================================================================

static void UpdateScreenGeometry(DBaseStatusBar *)
{
	setsizeneeded = true;
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, UpdateScreenGeometry, UpdateScreenGeometry)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	setsizeneeded = true;
	return 0;
}

static void SBar_Tick(DBaseStatusBar *self)
{
	self->Tick();
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, Tick, SBar_Tick)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	self->Tick();
	return 0;
}

static void SBar_AttachMessage(DBaseStatusBar *self, DHUDMessageBase *msg, unsigned id, int layer)
{
	self->AttachMessage(msg, id, layer);
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, AttachMessage, SBar_AttachMessage)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	PARAM_OBJECT(msg, DHUDMessageBase);
	PARAM_UINT(id);
	PARAM_INT(layer);
	self->AttachMessage(msg, id, layer);
	return 0;
}

static void SBar_DetachMessage(DBaseStatusBar *self, DHUDMessageBase *msg)
{
	self->DetachMessage(msg);
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, DetachMessage, SBar_DetachMessage)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	PARAM_OBJECT(msg, DHUDMessageBase);
	ACTION_RETURN_OBJECT(self->DetachMessage(msg));
}

static void SBar_DetachMessageID(DBaseStatusBar *self, unsigned id)
{
	self->DetachMessage(id);
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, DetachMessageID, SBar_DetachMessageID)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	PARAM_INT(id);
	ACTION_RETURN_OBJECT(self->DetachMessage(id));
}

static void SBar_DetachAllMessages(DBaseStatusBar *self)
{
	self->DetachAllMessages();
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, DetachAllMessages, SBar_DetachAllMessages)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	self->DetachAllMessages();
	return 0;
}

static void SetMugshotState(DBaseStatusBar *self, const FString &statename, bool wait, bool reset)
{
	self->mugshot.SetState(statename.GetChars(), wait, reset);
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, SetMugshotState, SetMugshotState)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	PARAM_STRING(statename);
	PARAM_BOOL(wait);
	PARAM_BOOL(reset);
	self->mugshot.SetState(statename.GetChars(), wait, reset);
	return 0;
}

static void SBar_ScreenSizeChanged(DBaseStatusBar *self)
{
	self->ScreenSizeChanged();
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, ScreenSizeChanged, SBar_ScreenSizeChanged)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	self->ScreenSizeChanged();
	return 0;
}

static int GetTopOfStatusbar(DBaseStatusBar *self)
{
	return self->GetTopOfStatusbar();
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, GetTopOfStatusbar, GetTopOfStatusbar)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	ACTION_RETURN_INT(self->GetTopOfStatusbar());
}

static void GetGlobalACSString(int index, FString *result)
{
	*result = primaryLevel->Behaviors.LookupString(ACS_GlobalVars[index]);
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, GetGlobalACSString, GetGlobalACSString)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	FString res;
	GetGlobalACSString(index, &res);
	ACTION_RETURN_STRING(res);
}

static void GetGlobalACSArrayString(int arrayno, int index, FString *result)
{
	*result = primaryLevel->Behaviors.LookupString(ACS_GlobalVars[index]);
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, GetGlobalACSArrayString, GetGlobalACSArrayString)
{
	PARAM_PROLOGUE;
	PARAM_INT(arrayno);
	PARAM_INT(index);
	FString res;
	GetGlobalACSArrayString(arrayno, index, &res);
	ACTION_RETURN_STRING(res);
}

static int GetGlobalACSValue(int index)
{
	return (ACS_GlobalVars[index]);
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, GetGlobalACSValue, GetGlobalACSValue)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	ACTION_RETURN_INT(ACS_GlobalVars[index]);
}

static int GetGlobalACSArrayValue(int arrayno, int index)
{
	return (ACS_GlobalArrays[arrayno][index]);
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, GetGlobalACSArrayValue, GetGlobalACSArrayValue)
{
	PARAM_PROLOGUE;
	PARAM_INT(arrayno);
	PARAM_INT(index);
	ACTION_RETURN_INT(ACS_GlobalArrays[arrayno][index]);
}

static void ReceivedWeapon(DBaseStatusBar *self)
{
	self->mugshot.Grin();
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, ReceivedWeapon, ReceivedWeapon)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	ReceivedWeapon(self);
	return 0;
}

static int GetMugshot(DBaseStatusBar *self, int accuracy, int stateflags, const FString &def_face)
{
	auto tex = self->mugshot.GetFace(self->CPlayer, def_face.GetChars(), accuracy, (FMugShot::StateFlags)stateflags);
	return (tex ? tex->GetID().GetIndex() : -1);
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, GetMugshot, GetMugshot)
{
	PARAM_SELF_PROLOGUE(DBaseStatusBar);
	PARAM_INT(accuracy);
	PARAM_INT(stateflags);
	PARAM_STRING(def_face);
	ACTION_RETURN_INT(GetMugshot(self, accuracy, stateflags, def_face));
}

DEFINE_ACTION_FUNCTION_NATIVE(DBaseStatusBar, GetInventoryIcon, GetInventoryIcon)
{
	PARAM_PROLOGUE;
	PARAM_OBJECT(item, AActor);
	PARAM_INT(flags);
	int applyscale;
	FTextureID icon = FSetTextureID(GetInventoryIcon(item, flags, &applyscale));
	if (numret >= 1) ret[0].SetInt(icon.GetIndex());
	if (numret >= 2) ret[1].SetInt(applyscale);
	return min(numret, 2);
}

//=====================================================================================
//
//
//
//=====================================================================================

DSpotState *GetSpotState(FLevelLocals *self, int create)
{
	if (create && self->SpotState == nullptr) self->SpotState = Create<DSpotState>();
	GC::WriteBarrier(self->SpotState);
	return self->SpotState;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetSpotState, GetSpotState)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(create);
	ACTION_RETURN_POINTER(GetSpotState(self, create));
}


//---------------------------------------------------------------------------
//
// Format the map name, include the map label if wanted
//
//---------------------------------------------------------------------------

EXTERN_CVAR(Int, am_showmaplabel)
EXTERN_CVAR(Bool, am_showlevelname)

void FormatMapName(FLevelLocals *self, int cr, FString *result)
{
	char mapnamecolor[3] = { '\34', char(cr + 'A'), 0 };

	cluster_info_t *cluster = FindClusterInfo(self->cluster);
	bool ishub = (cluster != nullptr && (cluster->flags & CLUSTER_HUB));

	*result = "";
	// If a label is specified, use it uncontitionally here.
	if (self->info->MapLabel.IsNotEmpty())
	{
		if (self->info->MapLabel.Compare("*"))
			*result << self->info->MapLabel;
	}
	else if (am_showmaplabel == 1 || (am_showmaplabel == 2 && !ishub))
	{
		*result << self->MapName;
	}

	if (am_showlevelname)
	{
		if (!result->IsEmpty())
			*result << ": ";
		*result << mapnamecolor << self->LevelName;
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, FormatMapName, FormatMapName)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(cr);
	FString rets;
	FormatMapName(self, cr, &rets);
	ACTION_RETURN_STRING(rets);
}

static void GetAutomapPosition(FLevelLocals *self, DVector2 *pos)
{
 	*pos = self->automap->GetPosition();
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetAutomapPosition, GetAutomapPosition)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	DVector2 result;
	GetAutomapPosition(self, &result);
	ACTION_RETURN_VEC2(result);
}

static int ZGetUDMFInt(FLevelLocals *self, int type, int index, int key)
{
	return GetUDMFInt(self,type, index, ENamedName(key));
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetUDMFInt, ZGetUDMFInt)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(type);
	PARAM_INT(index);
	PARAM_NAME(key);
	ACTION_RETURN_INT(GetUDMFInt(self, type, index, key));
}

static double ZGetUDMFFloat(FLevelLocals *self, int type, int index, int key)
{
	return GetUDMFFloat(self, type, index, ENamedName(key));
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetUDMFFloat, ZGetUDMFFloat)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(type);
	PARAM_INT(index);
	PARAM_NAME(key);
	ACTION_RETURN_FLOAT(GetUDMFFloat(self, type, index, key));
}

static void ZGetUDMFString(FLevelLocals *self, int type, int index, int key, FString *result)
{
	*result = GetUDMFString(self, type, index, ENamedName(key));
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetUDMFString, ZGetUDMFString)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(type);
	PARAM_INT(index);
	PARAM_NAME(key);
	ACTION_RETURN_STRING(GetUDMFString(self, type, index, key));
}

DEFINE_ACTION_FUNCTION(FLevelLocals, PlayerNum)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_POINTER(player, player_t);
	ACTION_RETURN_INT(self->PlayerNum(player));
}

DEFINE_ACTION_FUNCTION(FLevelLocals, GetChecksum)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	char md5string[33];

	for (int j = 0; j < 16; ++j)
	{
		snprintf(md5string + j * 2, 3, "%02x", self->md5[j]);
	}

	ACTION_RETURN_STRING((const char*)md5string);
}

static void Vec2Offset(FLevelLocals *Level, double x, double y, double dx, double dy, bool absolute, DVector2 *result)
{
	if (absolute)
	{
		*result = (DVector2(x + dx, y + dy));
	}
	else
	{
		*result = Level->GetPortalOffsetPosition(x, y, dx, dy);
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, Vec2Offset, Vec2Offset)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(dx);
	PARAM_FLOAT(dy);
	PARAM_BOOL(absolute);
	DVector2 result;
	Vec2Offset(self, x, y, dx, dy, absolute, &result);
	ACTION_RETURN_VEC2(result);
}

static void Vec2OffsetZ(FLevelLocals *Level, double x, double y, double dx, double dy, double atz, bool absolute, DVector3 *result)
{
	if (absolute)
	{
		*result = (DVector3(x + dx, y + dy, atz));
	}
	else
	{
		DVector2 v = Level->GetPortalOffsetPosition(x, y, dx, dy);
		*result = (DVector3(v, atz));
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, Vec2OffsetZ, Vec2OffsetZ)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(dx);
	PARAM_FLOAT(dy);
	PARAM_FLOAT(atz);
	PARAM_BOOL(absolute);
	DVector3 result;
	Vec2OffsetZ(self, x, y, dx, dy, atz, absolute, &result);
	ACTION_RETURN_VEC3(result);
}

static void Vec3Offset(FLevelLocals *Level, double x, double y, double z, double dx, double dy, double dz, bool absolute, DVector3 *result)
{
	if (absolute)
	{
		*result = (DVector3(x + dx, y + dy, z + dz));
	}
	else
	{
		DVector2 v = Level->GetPortalOffsetPosition(x, y, dx, dy);
		*result = (DVector3(v, z + dz));
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, Vec3Offset, Vec3Offset)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);
	PARAM_FLOAT(dx);
	PARAM_FLOAT(dy);
	PARAM_FLOAT(dz);
	PARAM_BOOL(absolute);
	DVector3 result;
	Vec3Offset(self, x, y, z, dx, dy, dz, absolute, &result);
	ACTION_RETURN_VEC3(result);
}

int IsPointInMap(FLevelLocals *Level, double x, double y, double z);

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, IsPointInLevel, IsPointInMap)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);
	ACTION_RETURN_BOOL(IsPointInMap(self, x, y, z));
}

template <typename T>
inline T VecDiff(FLevelLocals *Level, const T& v1, const T& v2)
{
	T result = v2 - v1;

	if (Level->subsectors.Size() > 0)
	{
		const sector_t * sec1 = Level->PointInSector(v1);
		const sector_t * sec2 = Level->PointInSector(v2);

		if (nullptr != sec1 && nullptr != sec2)
		{
			result += Level->Displacements.getOffset(sec2->PortalGroup, sec1->PortalGroup);
		}
	}

	return result;
}

void Vec2Diff(FLevelLocals *Level, double x1, double y1, double x2, double y2, DVector2 *result)
{
	*result = VecDiff(Level, DVector2(x1, y1), DVector2(x2, y2));
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, Vec2Diff, Vec2Diff)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x1);
	PARAM_FLOAT(y1);
	PARAM_FLOAT(x2);
	PARAM_FLOAT(y2);
	ACTION_RETURN_VEC2(VecDiff(self, DVector2(x1, y1), DVector2(x2, y2)));
}

void Vec3Diff(FLevelLocals *Level, double x1, double y1, double z1, double x2, double y2, double z2, DVector3 *result)
{
	*result = VecDiff(Level, DVector3(x1, y1, z1), DVector3(x2, y2, z2));
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, Vec3Diff, Vec3Diff)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x1);
	PARAM_FLOAT(y1);
	PARAM_FLOAT(z1);
	PARAM_FLOAT(x2);
	PARAM_FLOAT(y2);
	PARAM_FLOAT(z2);
	ACTION_RETURN_VEC3(VecDiff(self, DVector3(x1, y1, z1), DVector3(x2, y2, z2)));
}

//==========================================================================
//
// [BB] Billboards -- world-anchored oriented quads.
//
// The API here is deliberately the one the ZScript panel layer already
// exposes, so swapping a script-side panel over to the native primitive
// does not touch a single caller. Orientation arrives already solved:
// hinge geometry is a script concern, and the engine only consumes the
// yaw/tilt it is handed.
//
//==========================================================================

// The renderer's two comfort dials. A query has to apply BOTH or the pointer
// stops matching the picture: bb_scale changes how big a panel is DRAWN, so a
// hit test using the unscaled extent leaves the new edges dead, and
// bb_tiltbias changes how far it leans. Both were missing here until
// 2026-08-08 -- a player who enlarged panels to read them got a panel with a
// border that ignored the pointer, which reads as flaky tracking rather than
// as a cvar doing exactly what it said.
#include "rendering/hwrenderer/scene/hw_sdffont.h"

EXTERN_CVAR(String, bb_sdffont)
EXTERN_CVAR(Float, bb_scale)
EXTERN_CVAR(Float, bb_tiltbias)

// [BB] Where a billboard really is right now. View-locked ones have no fixed
// world position -- theirs is an offset from the viewer, resolved by the
// renderer each frame -- so queries have to use what was last drawn or the
// pointer disagrees with what the player sees. Everything else just uses pos.
// Before a billboard's first frame drawPos is still zero, so fall back.
static inline DVector3 BillboardWorldPos(FLevelLocals *self, const FBillboard &bb)
{
	if ((bb.flags & BBFL_VIEWLOCKED) && !bb.drawPos.isZero()) return bb.drawPos;

	// [BB] A group scales its members ABOUT ITS ORIGIN, and the renderer moves
	// the centre as well as the extent (hw_drawinfo.cpp, "lpos = gorigin +
	// (lpos - gorigin) * gscale"). BillboardQueryScale already mirrors the
	// extent half of that; this is the other half. Without it a grouped
	// billboard is hit-tested at full-size offsets with scaled extent, so its
	// clickable region sits (pos - origin) * (1 - gscale) away from the picture
	// -- and a wheel that opens with a grow animation is wrong for the whole
	// animation, which is exactly when the player is already pointing at it.
	//
	// View-locked members are excluded on purpose: the renderer scales those in
	// view-local space before resolving them against the viewpoint, and the
	// drawPos branch above already carries that result.
	if (bb.group)
	{
		DVector3 gorigin(0, 0, 0);
		const double gscale = self->BillboardGroupScale(bb.group, 1.0, &gorigin);

		// Attached billboards keep their offset in attachOffset and have their
		// pos rewritten unscaled every tic (p_tick.cpp), so the offset is what
		// gets scaled for them -- matching the renderer's lattach.
		if ((bb.flags & BBFL_ATTACHED) && bb.attachedTo != nullptr)
		{
			return bb.attachedTo->Pos() + (gorigin + (bb.attachOffset - gorigin) * gscale);
		}

		return gorigin + (bb.pos - gorigin) * gscale;
	}

	return bb.pos;
}

static FBillboard *FindBillboardByID(FLevelLocals *self, int id)
{
	if (id <= 0) return nullptr;
	for (auto &b : self->Billboards)
	{
		if (b.id == id) return &b;
	}
	return nullptr;
}

static void FillBillboard(FLevelLocals *self, FBillboard &bb, const DVector3 &pos, double w, double h,
	double yaw, double tilt, int facing, int payload, int data, int color, int flags, double lifetime,
	const FString &text)
{
	bb.pos = pos;
	bb.width = w;
	bb.height = h;
	bb.yaw = yaw;
	bb.tilt = tilt;
	bb.facing = facing;
	bb.payload = payload;
	bb.data = data;
	bb.text = text;

	// [BB] BB_TEXT's halo rides in `data`, which that payload does not
	// otherwise read -- byte 0 is reach, byte 1 is strength, both 0..255
	// mapped to 0..1. Use LevelLocals.BBGlow() to build it.
	//
	// It is packed rather than passed because two more arguments would put
	// AddBillboardPersistent at sixteen, and the ZScript compiler CRASHES
	// while compiling a call to a native with that many -- no error, no
	// dialog, just a silent exit during LoadActors. Everything below the
	// cliff works, so the cliff is simply not approached.
	// EVERY procedural payload, not just BB_TEXT. This read BB_TEXT alone at
	// first, which meant BB_SEGMENT, BB_SEGLCD and BB_SEAM accepted a
	// BBGlow() and threw it away -- their shaders got reach 0 and drew no halo
	// at all. It was invisible for a while because bloom is on by default, so
	// bright segments still bled and looked like they were glowing. They were
	// not; the bloom was doing all of it and none of it was controllable.
	if (data != 0 && (payload == BB_TEXT || payload == BB_SEGMENT
		|| payload == BB_SEGLCD || payload == BB_SEAM))
	{
		bb.glowRadius = ((data >> 0) & 0xff) / 255.0;
		bb.glowStrength = ((data >> 8) & 0xff) / 255.0;
	}
	bb.color = (PalEntry)color;
	bb.flags = flags;
	bb.lifetime = lifetime;
	bb.spawntic = self->maptime;
}

// Transient: no handle issued, expires by lifetime. Cheapest form -- use it
// for anything you will never need to address again.
static void AddBillboard(FLevelLocals *self, double x, double y, double z, double w, double h,
	double yaw, double tilt, int facing, int payload, int data, int color, int flags, double lifetime,
	const FString &text)
{
	FBillboard bb;
	FillBillboard(self, bb, DVector3(x, y, z), w, h, yaw, tilt, facing, payload, data, color,
		flags & ~(BBFL_PERSISTENT | BBFL_ATTACHED), lifetime, text);
	self->Billboards.Push(bb);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, AddBillboard, AddBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	PARAM_FLOAT(w); PARAM_FLOAT(h);
	PARAM_FLOAT(yaw); PARAM_FLOAT(tilt);
	PARAM_INT(facing);
	PARAM_INT(payload);
	PARAM_INT(data);
	PARAM_COLOR(color);
	PARAM_INT(flags);
	PARAM_FLOAT(lifetime);
	PARAM_STRING(text);
	AddBillboard(self, x, y, z, w, h, yaw, tilt, facing, payload, data, color, flags, lifetime, text);
	return 0;
}

// Persistent: lives until RemoveBillboard(). Returns the handle.
static int AddBillboardPersistent(FLevelLocals *self, double x, double y, double z, double w, double h,
	double yaw, double tilt, int facing, int payload, int data, int color, int flags, double lifetime,
	const FString &text)
{
	FBillboard bb;
	bb.id = self->NextBillboardID++;
	FillBillboard(self, bb, DVector3(x, y, z), w, h, yaw, tilt, facing, payload, data, color,
		(flags | BBFL_PERSISTENT) & ~BBFL_ATTACHED, lifetime, text);
	self->Billboards.Push(bb);
	return bb.id;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, AddBillboardPersistent, AddBillboardPersistent)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	PARAM_FLOAT(w); PARAM_FLOAT(h);
	PARAM_FLOAT(yaw); PARAM_FLOAT(tilt);
	PARAM_INT(facing);
	PARAM_INT(payload);
	PARAM_INT(data);
	PARAM_COLOR(color);
	PARAM_INT(flags);
	PARAM_FLOAT(lifetime);
	PARAM_STRING(text);
	ACTION_RETURN_INT(AddBillboardPersistent(self, x, y, z, w, h, yaw, tilt, facing, payload, data, color, flags, lifetime, text));
}

// Attached: follows an actor at a fixed offset and dies with it. Returns
// the handle. Lifetime is ignored -- the actor decides when this ends.
static int AttachBillboard(FLevelLocals *self, AActor *mo, double ox, double oy, double oz,
	double w, double h, double yaw, double tilt, int facing, int payload, int data, int color, int flags,
	const FString &text)
{
	if (mo == nullptr) return 0;
	FBillboard bb;
	bb.id = self->NextBillboardID++;
	FillBillboard(self, bb, mo->Pos() + DVector3(ox, oy, oz), w, h, yaw, tilt, facing, payload, data, color,
		(flags | BBFL_ATTACHED) & ~BBFL_PERSISTENT, 0.0, text);
	bb.attachedTo = mo;
	bb.attachOffset = DVector3(ox, oy, oz);
	self->Billboards.Push(bb);
	return bb.id;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, AttachBillboard, AttachBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(mo, AActor);
	PARAM_FLOAT(ox); PARAM_FLOAT(oy); PARAM_FLOAT(oz);
	PARAM_FLOAT(w); PARAM_FLOAT(h);
	PARAM_FLOAT(yaw); PARAM_FLOAT(tilt);
	PARAM_INT(facing);
	PARAM_INT(payload);
	PARAM_INT(data);
	PARAM_COLOR(color);
	PARAM_INT(flags);
	PARAM_STRING(text);
	ACTION_RETURN_INT(AttachBillboard(self, mo, ox, oy, oz, w, h, yaw, tilt, facing, payload, data, color, flags, text));
}

static void UpdateBillboard(FLevelLocals *self, int id, int data, int color)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->data = data;
	bb->color = (PalEntry)color;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, UpdateBillboard, UpdateBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_INT(data);
	PARAM_COLOR(color);
	UpdateBillboard(self, id, data, color);
	return 0;
}

// [BB] Retext a live BB_TEXT billboard. Separate from UpdateBillboard for the
// same reason SetBillboardAlpha is: a readout that changes its string every
// tic should not have to restate its colour to do it, and a caller that only
// wanted new text would otherwise have to remember what colour it set.
static void SetBillboardText(FLevelLocals *self, int id, const FString &text)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->text = text;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBillboardText, SetBillboardText)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_STRING(text);
	SetBillboardText(self, id, text);
	return 0;
}

// [BB] Retune a live billboard's halo. Its own call for the same reason the
// text and alpha setters are: a readout that pulses its glow every tic should
// not have to restate its string to do it.
static void SetBillboardGlow(FLevelLocals *self, int id, double radius, double strength)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->glowRadius = radius;
	bb->glowStrength = strength;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBillboardGlow, SetBillboardGlow)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_FLOAT(radius);
	PARAM_FLOAT(strength);
	SetBillboardGlow(self, id, radius, strength);
	return 0;
}

// [BB] Which typeface this billboard draws in. A SETTER and not an argument
// for the same reason the gradient is one: AddBillboardPersistent already
// takes fourteen and the ZScript compiler crashes, silently, on a native call
// with sixteen. Slot 0 is the default face, 1..BillboardFontCount() index the
// rolled roster. Out of range is not an error -- it draws in the default face,
// because the wrong typeface is a far better failure than invisible text.
static void SetBillboardFont(FLevelLocals *self, int id, int slot)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->font = slot;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBillboardFont, SetBillboardFont)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_INT(slot);
	SetBillboardFont(self, id, slot);
	return 0;
}

// [BB] Shuffle the roster. Called at game start so a run does not look like
// the last one; safe to call whenever a fresh look is wanted. No atlas is
// reloaded -- this reorders names, and FSDFFont::Get's cache is untouched.
static void RollBillboardFonts(FLevelLocals *self)
{
	FSDFFontRoster::Roll();
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, RollBillboardFonts, RollBillboardFonts)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	RollBillboardFonts(self);
	return 0;
}

// [BB] How many rolled faces exist, NOT counting slot 0. 0 means only the
// default face shipped, and every slot will resolve to it -- which is a
// legitimate load, not a broken one, so callers should treat it as "do not
// bother varying the typeface" rather than as an error.
static int BillboardFontCount(FLevelLocals *self)
{
	return FSDFFontRoster::Count();
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, BillboardFontCount, BillboardFontCount)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ACTION_RETURN_INT(BillboardFontCount(self));
}

// [BB] Which face is in this slot right now, for diagnostics and for a font
// preview. The answer changes every roll, which is exactly why something has
// to be able to ask.
static void BillboardFontName(FLevelLocals *self, int slot, FString *result)
{
	*result = FSDFFontRoster::SlotName(slot);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, BillboardFontName, BillboardFontName)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot);
	FString result;
	BillboardFontName(self, slot, &result);
	ACTION_RETURN_STRING(result);
}

// [BB] The gradient's far end. Its own setter rather than an argument, because
// AddBillboardPersistent is already at fourteen and the ZScript compiler
// CRASHES compiling a call to a native with sixteen -- silently, part way
// through LoadActors. Alpha 0 turns the gradient off again.
// [BB] Drive a payload's reveal, 0..1. Its own setter because this is meant to
// be called every tic on a live billboard, which is exactly the shape
// SetBillboardAlpha already has.
static void SetBillboardProgress(FLevelLocals *self, int id, double t)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->progress = clamp(t, 0.0, 1.0);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBillboardProgress, SetBillboardProgress)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_FLOAT(t);
	SetBillboardProgress(self, id, t);
	return 0;
}

static void SetBillboardGradient(FLevelLocals *self, int id, int color2)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->color2 = (PalEntry)color2;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBillboardGradient, SetBillboardGradient)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_COLOR(color2);
	SetBillboardGradient(self, id, color2);
	return 0;
}

//==========================================================================
//
// [BB] How wide a BB_TEXT string will be, in map units, at a given height.
//
// Asked for by the card compositor, which was approximating from character
// count. That approximation is only ever right for a monospace atlas, and it
// is silently wrong the moment anyone swaps in a proportional font -- the kind
// of bug that shows up as "labels overflow on one font" long after the change.
//
// This mirrors EmitBillboardSDFText's own layout maths rather than
// re-deriving it: total advance, scaled so the EM BOX matches the requested
// height. If the two ever disagree, this is wrong and the renderer is right.
//
// Returns 0 when no atlas is loaded, which a caller should read as "fall back
// to your own estimate" rather than as a zero-width string.
//
// MULTI-LINE, since 2026-08-09: `height` is the height of ONE LINE, and the
// width reported is the WIDEST line. A two-line string is therefore as wide as
// its longer half, not as wide as both halves end to end. Callers that need
// the other axis want MeasureBillboardTextBlock() below -- a panel sized from
// this alone would be one line tall no matter how many lines it held.
//
//==========================================================================

// `fontSlot` MUST match the slot the billboard will draw in. Different faces
// have different advances, so measuring in one and drawing in another is a
// layout that is quietly wrong by however much the two disagree -- and since
// the roster is reshuffled every game, it would be wrong by a DIFFERENT amount
// each run, which is about the worst way for a bug like this to present.
static double MeasureBillboardText(FLevelLocals *self, const FString &text, double height, int fontSlot)
{
	if (text.IsEmpty() || height <= 0.0) return 0.0;

	FSDFFont *font = FSDFFontRoster::Slot(fontSlot);
	if (font == nullptr) return 0.0;

	const double cell = font->Cell();
	if (cell <= 0.0) return 0.0;
	const double emBox = max(cell - 2.0 * font->Spread(), 1.0);

	return SDFMeasureText(font, text.GetChars()).widest * (height / emBox);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, MeasureBillboardText, MeasureBillboardText)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_STRING(text);
	PARAM_FLOAT(height);
	PARAM_INT(fontSlot);
	ACTION_RETURN_FLOAT(MeasureBillboardText(self, text, height, fontSlot));
}

//==========================================================================
//
// [BB] The whole block: width AND height, for text that carries newlines.
//
// Returned together, in one call, on purpose. The line pitch is the renderer's
// business and script has no way to derive it -- exposing width here and making
// callers multiply by a constant they had to be told would put a copy of the
// engine's layout rule in every mod that draws a table, and those copies would
// go stale the first time the pitch was tuned.
//
// x is the widest line, y is the total block height, both in map units, both
// for the given PER-LINE height. Single-line text returns exactly (width,
// height), so a caller can use this unconditionally.
//
// (0, 0) when no atlas is loaded, same contract as MeasureBillboardText.
//
//==========================================================================

static void MeasureBillboardTextBlock(FLevelLocals *self, const FString &text, double height, int fontSlot, DVector2 *result)
{
	*result = DVector2(0, 0);
	if (text.IsEmpty() || height <= 0.0) return;

	FSDFFont *font = FSDFFontRoster::Slot(fontSlot);
	if (font == nullptr) return;

	const double cell = font->Cell();
	if (cell <= 0.0) return;
	const double emBox = max(cell - 2.0 * font->Spread(), 1.0);

	const FSDFTextMetrics m = SDFMeasureText(font, text.GetChars());
	*result = DVector2(m.widest * (height / emBox), m.blockEm * height);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, MeasureBillboardTextBlock, MeasureBillboardTextBlock)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_STRING(text);
	PARAM_FLOAT(height);
	PARAM_INT(fontSlot);
	DVector2 result;
	MeasureBillboardTextBlock(self, text, height, fontSlot, &result);
	ACTION_RETURN_VEC2(result);
}

static void MoveBillboard(FLevelLocals *self, int id, double x, double y, double z)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->pos = DVector3(x, y, z);
	// An explicit move on an attached billboard retargets the offset rather
	// than the position, or the next tic would simply undo it.
	if ((bb->flags & BBFL_ATTACHED) && bb->attachedTo != nullptr)
	{
		bb->attachOffset = bb->pos - bb->attachedTo->Pos();
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, MoveBillboard, MoveBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	MoveBillboard(self, id, x, y, z);
	return 0;
}

// Reorienting is separate from moving because hinged assemblies re-solve
// orientation far more often than they change position.
static void OrientBillboard(FLevelLocals *self, int id, double yaw, double tilt, int facing)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->yaw = yaw;
	bb->tilt = tilt;
	bb->facing = facing;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, OrientBillboard, OrientBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_FLOAT(yaw); PARAM_FLOAT(tilt);
	PARAM_INT(facing);
	OrientBillboard(self, id, yaw, tilt, facing);
	return 0;
}

// [BB] The third angle, on its own.
//
// NOT a fourth argument to OrientBillboard: that call is made every tic by
// everything that orients anything, and widening it would have meant editing
// every existing call site to pass a value almost all of them do not care
// about. Roll is also changed on a completely different schedule -- a card
// tumbles once on arrival and then holds at zero forever -- so paying for it
// in the per-tic call would be backwards.
static void RollBillboard(FLevelLocals *self, int id, double roll)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->roll = roll;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, RollBillboard, RollBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_FLOAT(roll);
	RollBillboard(self, id, roll);
	return 0;
}

static void ResizeBillboard(FLevelLocals *self, int id, double w, double h)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->width = w;
	bb->height = h;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ResizeBillboard, ResizeBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_FLOAT(w); PARAM_FLOAT(h);
	ResizeBillboard(self, id, w, h);
	return 0;
}

// Fading is separate from UpdateBillboard because a fade runs every tic
// while data and colour change rarely, and a spawn/despawn on radius reads
// better with a short fade than with a pop.
static void SetBillboardAlpha(FLevelLocals *self, int id, double alpha)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->alpha = clamp(alpha, 0.0, 1.0);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBillboardAlpha, SetBillboardAlpha)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_FLOAT(alpha);
	SetBillboardAlpha(self, id, alpha);
	return 0;
}

static void RemoveBillboard(FLevelLocals *self, int id)
{
	if (id <= 0) return;
	for (unsigned i = 0; i < self->Billboards.Size(); i++)
	{
		if (self->Billboards[i].id == id)
		{
			self->Billboards.Delete(i);
			return;
		}
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, RemoveBillboard, RemoveBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	RemoveBillboard(self, id);
	return 0;
}

//==========================================================================
//
// [BB] BILLBOARD GROUPS -- one transform over many quads. See
// FBillboardGroup in g_levellocals.h for why this is not script's job.
//
// Usage is three calls and then nothing:
//
//     int gid = level.AddBillboardGroup((AHEAD, 0, UP));   // the pivot
//     ... build the panel, level.SetBillboardGroup(id, gid) on each element
//     level.AnimateBillboardGroup(gid, 0.0, 1.0, 10);      // grow, once
//
// The animation then runs in the renderer at frame rate. Script does not
// tick it, does not poll it, and does not need to know it finished.
//
//==========================================================================

static int AddBillboardGroup(FLevelLocals *self, double ox, double oy, double oz)
{
	FBillboardGroup g;
	g.id = self->NextBillboardGroupID++;
	g.origin = DVector3(ox, oy, oz);
	self->BillboardGroups.Push(g);
	return g.id;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, AddBillboardGroup, AddBillboardGroup)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(ox); PARAM_FLOAT(oy); PARAM_FLOAT(oz);
	ACTION_RETURN_INT(AddBillboardGroup(self, ox, oy, oz));
}

// Join a billboard to a group, or pass gid 0 to take it out of one. Deliberately
// a setter rather than a parameter on the six different Add functions: those
// are already at the argument count that crashes the ZScript compiler (see the
// warning above AddBillboard's declaration in doombase.zs).
static void SetBillboardGroup(FLevelLocals *self, int id, int gid)
{
	FBillboard *bb = FindBillboardByID(self, id);
	if (bb == nullptr) return;
	bb->group = gid;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBillboardGroup, SetBillboardGroup)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(id);
	PARAM_INT(gid);
	SetBillboardGroup(self, id, gid);
	return 0;
}

// Snap to a scale with no animation. Also the way to cancel one mid-flight.
static void SetBillboardGroupScale(FLevelLocals *self, int gid, double scale)
{
	FBillboardGroup *g = self->FindBillboardGroupByID(gid);
	if (g == nullptr) return;
	g->from = g->to = scale > 0.0 ? scale : 0.0;
	g->durTics = 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBillboardGroupScale, SetBillboardGroupScale)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(gid);
	PARAM_FLOAT(scale);
	SetBillboardGroupScale(self, gid, scale);
	return 0;
}

// The one that matters: declare the whole animation and walk away.
//
// tics <= 0 is treated as a snap to `to` rather than as an error, so a caller
// driving duration off a cvar cannot accidentally create a division by zero
// or an animation that never resolves.
static void AnimateBillboardGroup(FLevelLocals *self, int gid, double from, double to, int tics)
{
	FBillboardGroup *g = self->FindBillboardGroupByID(gid);
	if (g == nullptr) return;
	g->from     = from > 0.0 ? from : 0.0;
	g->to       = to   > 0.0 ? to   : 0.0;
	g->startTic = self->maptime;
	g->durTics  = tics > 0 ? tics : 0;
	if (g->durTics == 0) g->from = g->to;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, AnimateBillboardGroup, AnimateBillboardGroup)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(gid);
	PARAM_FLOAT(from);
	PARAM_FLOAT(to);
	PARAM_INT(tics);
	AnimateBillboardGroup(self, gid, from, to, tics);
	return 0;
}

// Move the pivot. A panel that is repositioned as a whole wants its origin to
// follow, or the next animation will scale it toward wherever it used to be.
static void SetBillboardGroupOrigin(FLevelLocals *self, int gid, double ox, double oy, double oz)
{
	FBillboardGroup *g = self->FindBillboardGroupByID(gid);
	if (g == nullptr) return;
	g->origin = DVector3(ox, oy, oz);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBillboardGroupOrigin, SetBillboardGroupOrigin)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(gid);
	PARAM_FLOAT(ox); PARAM_FLOAT(oy); PARAM_FLOAT(oz);
	SetBillboardGroupOrigin(self, gid, ox, oy, oz);
	return 0;
}

// Drops the group and releases every member back to an untransformed state.
//
// The release matters. A group is found by a linear scan that returns nullptr
// for an unknown id, and BillboardGroupScale answers 1.0 in that case -- so an
// orphaned member would silently SNAP back to full size rather than
// disappearing. Clearing the field makes that explicit instead of incidental,
// and means a stale group id can never be reused against live billboards.
static void RemoveBillboardGroup(FLevelLocals *self, int gid)
{
	if (gid <= 0) return;
	for (auto &bb : self->Billboards)
	{
		if (bb.group == gid) bb.group = 0;
	}
	for (unsigned i = 0; i < self->BillboardGroups.Size(); i++)
	{
		if (self->BillboardGroups[i].id == gid)
		{
			self->BillboardGroups.Delete(i);
			return;
		}
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, RemoveBillboardGroup, RemoveBillboardGroup)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(gid);
	RemoveBillboardGroup(self, gid);
	return 0;
}

// [BB] Volumetric beam -- a cone of light visible in the air itself, not just
// on the surfaces it lands on. Published each tic by whatever owns the
// flashlight; the renderer resolves it into view space per eye, so stereo and
// portals come out right without script knowing either exists.
//
// inner/outer are half-angles in degrees: full brightness inside inner,
// faded to nothing by outer. falloff shapes the fade along the beam's length
// -- 1 is linear, higher concentrates the light near the lens.
static void SetVolumetricBeam(FLevelLocals *self, double px, double py, double pz,
	double dx, double dy, double dz, int color,
	double inner, double outer, double length, double density, double falloff,
	double dust, double dustScale, double dustDrift, int slot)
{
	// Slot 0 is the DEFAULT for anything that never heard of slots (see the
	// declaration's `slot = 0`). An OUT-OF-RANGE slot is refused, not redirected.
	//
	// It used to be remapped to 0, which quietly took over slot 0 -- the
	// flashlight, the exact clash slots exist to prevent -- and the caller's
	// own ClearVolumetricBeam(badSlot) was then a no-op, so its cone could
	// never be turned off except by killing the torch too. Logged once per
	// distinct bad value, so a caller doing this every tic does not flood.
	if (slot < 0 || slot >= FLevelLocals::MAX_VOL_BEAMS)
	{
		static bool loggedBadSlot = false;
		static int lastBadSlot = 0;
		if (!loggedBadSlot || slot != lastBadSlot)
		{
			loggedBadSlot = true;
			lastBadSlot = slot;
			Printf("SetVolumetricBeam: slot %d is outside 0..%d; the beam is ignored\n",
				slot, FLevelLocals::MAX_VOL_BEAMS - 1);
		}
		return;
	}

	// VALIDATE THE CONE. GLSL smoothstep(e0, e1, x) is undefined when e0 >= e1,
	// and both the air pass (smoothstep(cosOuter, cosInner, ...)) and the fog
	// glow feed it the two angles directly: equal angles divide by zero (NaN on
	// some drivers), inner above outer reverses the ramp. Length 0 divides by
	// zero in the axial fade. The outer angle is also capped below 90: the beam
	// shader relies on the forward cone being convex (volumetricbeam.fp). The
	// !(x >= min) form also catches NaN. A clamp is logged when the set of
	// clamped inputs for a slot changes, not every tic.
	int clampMask = 0;
	if (!(outer >= 0.1)) { outer = 0.1; clampMask |= 1; }
	if (outer > 89.0) { outer = 89.0; clampMask |= 2; }
	if (!(inner <= outer - 0.1)) { inner = outer - 0.1; clampMask |= 4; }
	if (inner < 0.0) { inner = 0.0; clampMask |= 4; }
	if (!(length >= 1.0)) { length = 1.0; clampMask |= 8; }
	if (!(falloff >= 0.01)) { falloff = 0.01; clampMask |= 16; }
	{
		static int lastClampMask[FLevelLocals::MAX_VOL_BEAMS] = {};
		if (clampMask != lastClampMask[slot])
		{
			lastClampMask[slot] = clampMask;
			if (clampMask != 0)
			{
				Printf("SetVolumetricBeam slot %d: clamped%s%s%s%s%s -> inner %.2f outer %.2f length %.1f falloff %.2f\n",
					slot,
					(clampMask & 1) ? " outer<0.1" : "",
					(clampMask & 2) ? " outer>89" : "",
					(clampMask & 4) ? " inner-not-below-outer" : "",
					(clampMask & 8) ? " length<1" : "",
					(clampMask & 16) ? " falloff<0.01" : "",
					inner, outer, length, falloff);
			}
		}
	}

	// A slot being CLAIMED (it was not live) forgets any anchor it had.
	// Slots are reused: without this, a caller taking over a slot another
	// mod had anchored to a hand would get a cone stuck to a controller it
	// never asked about. An anchor is therefore set AFTER SetVolumetricBeam.
	if (!self->VolBeamActive[slot])
	{
		self->VolBeamAnchor[slot] = 0;
		self->VolBeamAnchorOffset[slot] = DVector3(0, 0, 0);
	}

	self->VolBeamActive[slot] = true;
	self->VolBeamDust[slot] = dust;
	self->VolBeamDustScale[slot] = dustScale;
	self->VolBeamDustDrift[slot] = dustDrift;
	self->VolBeamPos[slot] = DVector3(px, py, pz);
	DVector3 d(dx, dy, dz);
	double len = d.Length();
	self->VolBeamDir[slot] = (len > 0.0) ? d / len : DVector3(1, 0, 0);
	self->VolBeamColor[slot] = (PalEntry)color;
	self->VolBeamInner[slot] = inner;
	self->VolBeamOuter[slot] = outer;
	self->VolBeamLength[slot] = length;
	self->VolBeamDensity[slot] = density;
	self->VolBeamFalloff[slot] = falloff;
}

// NATIVE0, NOT NATIVE -- and this is not a style choice.
//
// The plain _NATIVE macro also registers a DIRECT-CALL pointer, which the JIT
// uses to emit a straight call to the C++ function. asmjit caps that at 16
// arguments (kFuncArgCount in libraries/asmjit/asmjit/base/func.h). This one
// needs seventeen: self, two Vector3s at three floats each, the colour, five
// doubles, three dust doubles and the slot. Over the cap the JIT blows up while
// compiling at load -- the process dies with no window, no dialog, no script
// error and nothing written to any log.
//
// It sat at exactly sixteen until `slot` was added, which is why this only
// started when beams gained slots, and why it broke every caller at once rather
// than only the one that passed the new argument: the JIT builds the direct call
// from the FUNCTION signature, not from the call site, so leaving `slot` to its
// default did not help anybody.
//
// _NATIVE0 registers the same function with no direct-call pointer. The VM
// calling convention has no such limit. If you add a parameter to any native,
// count the VM arguments first -- a Vector3 is three.
DEFINE_ACTION_FUNCTION_NATIVE0(FLevelLocals, SetVolumetricBeam, SetVolumetricBeam)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(dx); PARAM_FLOAT(dy); PARAM_FLOAT(dz);
	PARAM_COLOR(color);
	PARAM_FLOAT(inner);
	PARAM_FLOAT(outer);
	PARAM_FLOAT(length);
	PARAM_FLOAT(density);
	PARAM_FLOAT(falloff);
	PARAM_FLOAT(dust);
	PARAM_FLOAT(dustScale);
	PARAM_FLOAT(dustDrift);
	PARAM_INT(slot);
	SetVolumetricBeam(self, px, py, pz, dx, dy, dz, color, inner, outer, length, density, falloff, dust, dustScale, dustDrift, slot);
	return 0;
}

// Clear ONE slot, or every slot with -1. Defaulting to slot 0 rather than to
// all of them is deliberate: a caller that never heard of slots is turning off
// the beam it turned on, not everyone else's -- which was the whole failure the
// slots exist to fix.
static void ClearVolumetricBeam(FLevelLocals *self, int slot)
{
	// Clearing releases the slot, anchor included (see SetVolumetricBeam).
	if (slot < 0)
	{
		for (int i = 0; i < FLevelLocals::MAX_VOL_BEAMS; i++)
		{
			self->VolBeamActive[i] = false;
			self->VolBeamAnchor[i] = 0;
		}
		return;
	}
	if (slot < FLevelLocals::MAX_VOL_BEAMS)
	{
		self->VolBeamActive[slot] = false;
		self->VolBeamAnchor[slot] = 0;
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearVolumetricBeam, ClearVolumetricBeam)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot);
	ClearVolumetricBeam(self, slot);
	return 0;
}

// RS FORK -- anchor a volumetric beam to a tracked pose, resolved at DRAW rate.
// See FLevelLocals::VolBeamAnchor. The cone equivalent of SetBeamAnchor, and it
// moves the DIRECTION as well as the origin: a cone has no far end in the world
// to hold still, and a torch that swings at 35Hz behind a 90Hz hand judders.
//
// mode 0 none (the script's pos/dir), 1 main hand, 2 off hand, 3 head.
// offset is (forward, right, up) in map units in that pose's yaw/pitch frame.
// Out-of-range modes read as 0. Call AFTER SetVolumetricBeam: claiming a slot
// that was not live resets its anchor.
static void SetVolumetricBeamAnchor(FLevelLocals *self, int slot, int mode, double ox, double oy, double oz)
{
	if (slot < 0 || slot >= FLevelLocals::MAX_VOL_BEAMS)
	{
		static bool loggedBadSlot = false;
		static int lastBadSlot = 0;
		if (!loggedBadSlot || slot != lastBadSlot)
		{
			loggedBadSlot = true;
			lastBadSlot = slot;
			Printf("SetVolumetricBeamAnchor: slot %d is outside 0..%d; ignored\n",
				slot, FLevelLocals::MAX_VOL_BEAMS - 1);
		}
		return;
	}
	if (mode < 0 || mode > 3) mode = 0;

	// An anchor on a slot that is not live will be wiped when SetVolumetricBeam
	// claims it -- almost certainly a call-order mistake. Say so once per slot
	// until the slot is anchored while live.
	static bool warnedNotLive[FLevelLocals::MAX_VOL_BEAMS] = {};
	if (!self->VolBeamActive[slot] && mode != 0)
	{
		if (!warnedNotLive[slot])
		{
			warnedNotLive[slot] = true;
			Printf("SetVolumetricBeamAnchor: slot %d is not live; SetVolumetricBeam resets the anchor when it claims the slot -- call the anchor after it\n", slot);
		}
	}
	else
	{
		warnedNotLive[slot] = false;
	}

	self->VolBeamAnchor[slot] = mode;
	self->VolBeamAnchorOffset[slot] = DVector3(ox, oy, oz);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetVolumetricBeamAnchor, SetVolumetricBeamAnchor)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot);
	PARAM_INT(mode);
	PARAM_FLOAT(ox);
	PARAM_FLOAT(oy);
	PARAM_FLOAT(oz);
	SetVolumetricBeamAnchor(self, slot, mode, ox, oy, oz);
	return 0;
}

//==========================================================================
//
// [HEATREFRACTION] HEAT SOURCES -- see FLevelLocals::HeatSource (g_levellocals.h)
// and "Engine docs/FLAME_ENGINE_PLAN.md" F2 (heat shimmer).
//
// Render-only slots, drawn only while r_heatrefraction is on. Every native here
// is a setter; none returns render state, so nothing in gameplay can branch on
// how the air looks. No RNG: the shimmer is noise over world position and level
// time, computed on the GPU.
//
//==========================================================================

// Where an anchored hand is right now, for the anchor's recorded base. The same
// player rule as the renderer's (hw_drawinfo.cpp, ResolveLineAnchor): that player
// number when they are in the game, else the console player. Read-only.
static bool HeatSourceHandPos(FLevelLocals *self, int mode, int playerNum, DVector3 &pos)
{
	if (mode != 1 && mode != 2) return false;
	const player_t *p = (playerNum >= 0 && playerNum < MAXPLAYERS && self->PlayerInGame(playerNum))
		? &players[playerNum] : self->GetConsolePlayer();
	if (p == nullptr || p->mo == nullptr) return false;
	pos = (mode == 2) ? p->mo->OffhandPos : p->mo->AttackPos;
	return true;
}

// Logged once per distinct bad value, so a caller doing this every tic does not flood.
static void LogHeatSourceBadSlot(const char *who, int slot)
{
	static bool logged = false;
	static int lastSlot = 0;
	if (!logged || slot != lastSlot)
	{
		logged = true;
		lastSlot = slot;
		Printf("%s: slot %d is outside 0..%d; ignored\n", who, slot, FLevelLocals::MAX_HEAT_SOURCES - 1);
	}
}

// Re-bases a live anchor every time the source is set, so the hand's movement is
// measured from the pose the script placed the source by.
static void RebaseHeatSourceAnchor(FLevelLocals *self, int slot)
{
	const FLevelLocals::HeatSource &h = self->HeatSources[slot];
	if (h.Anchor == 0) return;
	DVector3 hand(0., 0., 0.);
	const bool read = HeatSourceHandPos(self, h.Anchor, h.AnchorPlayer, hand);
	// Unreadable (no pawn yet): recorded as invalid, and the source stays put.
	self->SetHeatSourceAnchorBase(slot, hand, read);
}

// 14 VM arguments (self, slot, two Vector3s, six numbers, life): under the JIT's
// direct-call cap of 16, so the plain _NATIVE macro is safe (see SetVolumetricBeam).
static void SetHeatSource(FLevelLocals *self, int slot, double sx, double sy, double sz,
	double ex, double ey, double ez, double radiusStart, double radiusEnd,
	double strength, double noiseScale, double rise, int life)
{
	if (slot < 0 || slot >= FLevelLocals::MAX_HEAT_SOURCES)
	{
		LogHeatSourceBadSlot("SetHeatSource", slot);
		return;
	}
	self->SetHeatSource(slot, DVector3(sx, sy, sz), DVector3(ex, ey, ez), radiusStart, radiusEnd,
		strength, noiseScale, rise, life);
	RebaseHeatSourceAnchor(self, slot);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetHeatSource, SetHeatSource)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot);
	PARAM_FLOAT(sx); PARAM_FLOAT(sy); PARAM_FLOAT(sz);
	PARAM_FLOAT(ex); PARAM_FLOAT(ey); PARAM_FLOAT(ez);
	PARAM_FLOAT(radiusStart);
	PARAM_FLOAT(radiusEnd);
	PARAM_FLOAT(strength);
	PARAM_FLOAT(noiseScale);
	PARAM_FLOAT(rise);
	PARAM_INT(life);
	SetHeatSource(self, slot, sx, sy, sz, ex, ey, ez, radiusStart, radiusEnd, strength, noiseScale, rise, life);
	return 0;
}

// mode 0 world, 1 main hand, 2 off hand (out of range reads as 0). owner is whose
// hand: a player's pawn; without one, the console player's. Call AFTER
// SetHeatSource when claiming a slot: a slot that was not live forgets its anchor
// when SetHeatSource takes it.
static void SetHeatSourceAnchor(FLevelLocals *self, int slot, int mode, AActor *owner)
{
	if (slot < 0 || slot >= FLevelLocals::MAX_HEAT_SOURCES)
	{
		LogHeatSourceBadSlot("SetHeatSourceAnchor", slot);
		return;
	}
	static bool warnedNotLive[FLevelLocals::MAX_HEAT_SOURCES] = {};
	if (!self->HeatSourceLive(slot) && mode != 0)
	{
		if (!warnedNotLive[slot])
		{
			warnedNotLive[slot] = true;
			Printf("SetHeatSourceAnchor: slot %d is not live; SetHeatSource resets the anchor when it claims the slot -- call the anchor after it\n", slot);
		}
	}
	else
	{
		warnedNotLive[slot] = false;
	}
	const int playerNum = (owner != nullptr && owner->player != nullptr) ? int(owner->player - players) : -1;
	self->SetHeatSourceAnchor(slot, mode, playerNum);
	RebaseHeatSourceAnchor(self, slot);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetHeatSourceAnchor, SetHeatSourceAnchor)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot);
	PARAM_INT(mode);
	PARAM_OBJECT(owner, AActor);
	SetHeatSourceAnchor(self, slot, mode, owner);
	return 0;
}

// One slot, or every slot with -1. Forgets the slot, anchor included.
static void ClearHeatSource(FLevelLocals *self, int slot)
{
	self->ClearHeatSource(slot);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearHeatSource, ClearHeatSource)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot);
	ClearHeatSource(self, slot);
	return 0;
}

// A fixed engine number, the same on every machine, so gameplay code may size a
// ring of slots by it.
static int HeatSourceCapacity(FLevelLocals *self)
{
	return FLevelLocals::MAX_HEAT_SOURCES;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, HeatSourceCapacity, HeatSourceCapacity)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ACTION_RETURN_INT(HeatSourceCapacity(self));
}

//==========================================================================
//
// [SMOKEVOLUME] + [EFFECTQUEUES] THE SMOKE VOLUME'S API AND THE ONE BLAST CALL --
// EmitSmoke, CarveSmoke, SetSmokeLook, SetSmokeWind, ClearSmoke, PushEffectImpulse.
// See FEffectTicQueue and FLevelLocals::SmokeLook (g_levellocals.h), and
// "Engine docs/SMOKE_VOLUME_PLAN.md" 13b, SH3 and SH4.
//
// ONE-WAY. Every native here only queues an event on the level's per-tic queues or
// sets a render-only look. None returns anything, so no gameplay code can branch on
// smoke, and nothing is serialized. Safe from play code on every machine in a
// netgame: each machine queues the same event, and its renderer draws it -- or
// drops it, when its smoke is off or its quality lower -- without touching the
// playsim. No RNG anywhere: the smoke's swirl is noise over world position and time
// on the GPU.
//
// A call with a non-finite number is ignored (logged once per native per session).
// A queue full for this tic drops the event (logged once per map).
//
//==========================================================================

static bool EffectArgsFinite(const char *who, bool &logged, std::initializer_list<double> values)
{
	for (double v : values)
	{
		if (!std::isfinite(v))
		{
			if (!logged)
			{
				logged = true;
				Printf("%s: ignored a call with a non-finite argument (logged once)\n", who);
			}
			return false;
		}
	}
	return true;
}

static void LogEffectQueueFull(bool &fullLogged, const char *who, int capacity)
{
	if (fullLogged) return;
	fullLogged = true;
	Printf("%s: more than %d this tic -- the rest of this tic's are dropped (logged once per map)\n", who, capacity);
}

static DVector3 ClampEffectVector(double x, double y, double z, double limit)
{
	return DVector3(clamp(x, -limit, limit), clamp(y, -limit, limit), clamp(z, -limit, limit));
}

// A segment longer than maxLength keeps its start and direction.
static DVector3 LimitEffectSegmentEnd(const DVector3 &start, const DVector3 &end, double maxLength)
{
	const DVector3 d = end - start;
	const double length = d.Length();
	return length > maxLength ? start + d * (maxLength / length) : end;
}

// 14 VM arguments: under the JIT's direct-call cap of 16 (see SetHeatSource).
static void EmitSmoke(FLevelLocals *self, double px, double py, double pz, double radius, double amount, double heat,
	double vx, double vy, double vz, double ex, double ey, double ez, double soot)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("EmitSmoke", badLogged, { px, py, pz, radius, amount, heat, vx, vy, vz, ex, ey, ez, soot }))
		return;

	const double amountC = clamp(amount, 0., 16.);
	const double heatC = clamp(heat, 0., 16.);
	const DVector3 vel = ClampEffectVector(vx, vy, vz, 4096.);
	// Nothing to add: not queued, so it asks for no volume either.
	if (amountC <= 0. && heatC <= 0. && vel.LengthSquared() <= 0.)
		return;

	FSmokeEmitEvent *e = self->SmokeEmits.Push();
	if (e == nullptr)
	{
		LogEffectQueueFull(self->SmokeEmits.FullLogged, "EmitSmoke", FLevelLocals::MAX_SMOKE_EMITS_PER_TIC);
		return;
	}
	e->Pos = DVector3(px, py, pz);
	// posEnd (0,0,0) is "not given" -- the ZScript default.
	e->Capsule = !(ex == 0. && ey == 0. && ez == 0.);
	e->End = e->Capsule ? LimitEffectSegmentEnd(e->Pos, DVector3(ex, ey, ez), 4096.) : e->Pos;
	e->Vel = vel;
	// 256 at most: the GPU tests every cell it reaches for a wall in between
	// (smoke_inject.comp, Reachable), which a bigger ball would make costly. Bigger
	// clouds are several emits.
	e->Radius = clamp(radius, 1., 256.);
	e->Amount = amountC;
	e->Heat = heatC;
	// [13e] The share of the puff that is soot, 0..1 (0, the default, is every caller before 13e).
	e->Soot = clamp(soot, 0., 1.);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, EmitSmoke, EmitSmoke)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(radius);
	PARAM_FLOAT(amount);
	PARAM_FLOAT(heat);
	PARAM_FLOAT(vx); PARAM_FLOAT(vy); PARAM_FLOAT(vz);
	PARAM_FLOAT(ex); PARAM_FLOAT(ey); PARAM_FLOAT(ez);
	PARAM_FLOAT(soot);
	EmitSmoke(self, px, py, pz, radius, amount, heat, vx, vy, vz, ex, ey, ez, soot);
	return 0;
}

//==========================================================================
//
// [EFFECTLIGHTS] SpawnEffectLight -- a short light that belongs to nothing ("Engine docs/EFFECT_LIGHTS_CORE_IMPL_NOTES.md";
// FEffectLightEvent, g_levellocals.h; hw_effectlights.cpp). ONE-WAY, like EmitSmoke: it only queues an event on the level's
// per-tic queue and returns nothing; nothing reads it back and nothing is saved, so gameplay cannot branch on it, and every
// machine queues the same light. No RNG: a landed light's varied hold is a local hash of its tic, its place in that tic's
// queue and where it began (EffectLightCore::SeedFor). A call with a non-finite number, a radius under 1 or no intensity is
// ignored.
//
//==========================================================================

static void SpawnEffectLight(FLevelLocals *self, double px, double py, double pz, int color, double radius, double intensity,
	double life, double vx, double vy, double vz, double ex, double ey, double ez, double fade, int flags, double gravity,
	double drag, double land, double hold, double tailLag, double tailBrightness)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("SpawnEffectLight", badLogged, { px, py, pz, radius, intensity, life, vx, vy, vz, ex, ey, ez, fade,
		gravity, drag, land, hold, tailLag, tailBrightness }))
		return;
	// Nothing to light: not queued.
	if (!(radius >= 1.) || !(intensity > 0.))
		return;

	FEffectLightEvent *e = self->EffectLightSpawns.Push();
	if (e == nullptr)
	{
		LogEffectQueueFull(self->EffectLightSpawns.FullLogged, "SpawnEffectLight", FLevelLocals::MAX_EFFECT_LIGHTS_PER_TIC);
		return;
	}
	e->Pos = DVector3(px, py, pz);
	// posEnd (0,0,0) is "not given" -- the ZScript default, as EmitSmoke's: a point.
	const bool line = !(ex == 0. && ey == 0. && ez == 0.);
	e->End = line ? LimitEffectSegmentEnd(e->Pos, DVector3(ex, ey, ez), 8192.) : e->Pos;
	e->Vel = ClampEffectVector(vx, vy, vz, 65536.);	// a round flies about 30,000 map units a second
	e->Radius = clamp(radius, 1., 1024.);
	e->Intensity = clamp(intensity, 0., 16.);
	e->Life = clamp(life, 0.001, 60.);
	e->Fade = clamp(fade, 0., 16.);
	e->Gravity = clamp(gravity, -65536., 65536.);
	e->Drag = clamp(drag, 0., 64.);
	e->Land = clamp(land, 0., 60.);
	e->Hold = clamp(hold, 0., 60.);
	e->TailLag = clamp(tailLag, 0., 1.);
	e->TailBrightness = clamp(tailBrightness, 0., 1.);
	e->Color = (uint32_t)color;
	e->Flags = flags;
}

// _NATIVE0, NOT _NATIVE: 22 VM arguments (self, three Vector3s at three floats each, and twelve more) are past asmjit's
// 16-argument direct-call cap -- see the note on SetVolumetricBeam. The VM calling convention has no such limit.
DEFINE_ACTION_FUNCTION_NATIVE0(FLevelLocals, SpawnEffectLight, SpawnEffectLight)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_COLOR(color);
	PARAM_FLOAT(radius);
	PARAM_FLOAT(intensity);
	PARAM_FLOAT(life);
	PARAM_FLOAT(vx); PARAM_FLOAT(vy); PARAM_FLOAT(vz);
	PARAM_FLOAT(ex); PARAM_FLOAT(ey); PARAM_FLOAT(ez);
	PARAM_FLOAT(fade);
	PARAM_INT(flags);
	PARAM_FLOAT(gravity); PARAM_FLOAT(drag);
	PARAM_FLOAT(land); PARAM_FLOAT(hold);
	PARAM_FLOAT(tailLag); PARAM_FLOAT(tailBrightness);
	SpawnEffectLight(self, px, py, pz, color, radius, intensity, life, vx, vy, vz, ex, ey, ez, fade, flags, gravity, drag,
		land, hold, tailLag, tailBrightness);
	return 0;
}

//==========================================================================
//
// [EMISSIVEVOLUMES] EmissiveVolumeDefinition, EmissiveVolumeEnabled, SpawnEmissiveVolume -- short-lived glowing gas volumes
// ("Engine docs/EMISSIVE_VOLUMES_15_IMPL_NOTES.md"; FEmissiveVolumeEvent, g_levellocals.h; hw_emissivevolumes.cpp).
//
// ONE-WAY, like SpawnEffectLight: SpawnEmissiveVolume only queues an event on the level's per-tic queue and returns nothing;
// nothing reads it back and nothing is saved, so every machine queues the same volume. No RNG: a volume's variety is a local
// hash of its seed, or of its tic, its place in that tic's queue and where it began (EmissiveVolumeCore::SeedFor). A followed
// pose names its player; the console player is never implied. A call with a non-finite number, handle 0, or no scale or
// brightness is ignored.
//
// EmissiveVolumeDefinition returns a hash of the name, the same on every machine. EmissiveVolumeEnabled is a PRESENTATION
// query for this machine only (which look to draw); both are static and clearscope, so a menu or HUD can ask too.
//
//==========================================================================

static int EmissiveVolumeDefinition(int name)
{
	return EmissiveVolumeDefinitionHandle(FName(ENamedName(name)).GetChars());
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, EmissiveVolumeDefinition, EmissiveVolumeDefinition)
{
	PARAM_PROLOGUE;
	PARAM_NAME(name);
	ACTION_RETURN_INT(EmissiveVolumeDefinition(name.GetIndex()));
}

static int EmissiveVolumeEnabled(int def)
{
	return EmissiveVolumeDefinitionEnabled(def) ? 1 : 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, EmissiveVolumeEnabled, EmissiveVolumeEnabled)
{
	PARAM_PROLOGUE;
	PARAM_INT(def);
	ACTION_RETURN_BOOL(EmissiveVolumeEnabled(def));
}

static void SpawnEmissiveVolume(FLevelLocals *self, int def, double px, double py, double pz, double dx, double dy, double dz,
	double scale, double brightness, double lifeScale, int tint, double vx, double vy, double vz, int seed, int follow,
	int followPlayer, double followShare, double lightScale)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("SpawnEmissiveVolume", badLogged, { px, py, pz, dx, dy, dz, scale, brightness, lifeScale, vx, vy, vz,
		followShare, lightScale }))
		return;
	// Nothing to draw: not queued.
	if (def == 0 || !(scale > 0.) || !(brightness > 0.))
		return;

	FEmissiveVolumeEvent *e = self->EmissiveVolumeSpawns.Push();
	if (e == nullptr)
	{
		LogEffectQueueFull(self->EmissiveVolumeSpawns.FullLogged, "SpawnEmissiveVolume", FLevelLocals::MAX_EMISSIVE_VOLUMES_PER_TIC);
		return;
	}
	e->Definition = def;
	e->Pos = DVector3(px, py, pz);
	const DVector3 dir(dx, dy, dz);
	const double length = dir.Length();
	e->Dir = length > 1e-9 ? dir / length : DVector3(0., 0., 1.);	// no direction: straight up
	e->Vel = ClampEffectVector(vx, vy, vz, 65536.);
	e->Scale = clamp(scale, 0.05, 16.);
	e->Brightness = clamp(brightness, 0., 16.);
	e->LifeScale = clamp(lifeScale, 0.05, 16.);
	e->FollowShare = clamp(followShare, 0., 1.);
	e->LightScale = clamp(lightScale, 0., 16.);
	e->Tint = (uint32_t)tint;
	e->Seed = seed;
	e->Follow = 0;
	e->FollowPlayer = -1;
	e->FollowValid = false;
	// The followed pose as it is NOW, for the renderer to measure the hand's motion from. Only a named player in the game.
	if (follow >= 1 && follow <= 3 && followPlayer >= 0 && followPlayer < MAXPLAYERS && self->PlayerInGame(followPlayer))
	{
		DVector3 posed;
		DAngle yaw, pitch;
		const int source = ResolveTrackedPose(self, follow, DVector3(0., 0., 0.), posed, yaw, pitch, &players[followPlayer]);
		if (source != TPOSE_NONE && source != TPOSE_NOPLAYER)
		{
			e->Follow = follow;
			e->FollowPlayer = followPlayer;
			e->FollowValid = true;
			e->FollowPos = posed;
			e->FollowYaw = yaw.Radians();
			e->FollowPitch = pitch.Radians();
		}
	}
}

// _NATIVE0, NOT _NATIVE: 20 VM arguments (self, three Vector3s at three floats each, and ten more) are past asmjit's 16-argument
// direct-call cap -- see SpawnEffectLight.
DEFINE_ACTION_FUNCTION_NATIVE0(FLevelLocals, SpawnEmissiveVolume, SpawnEmissiveVolume)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(def);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(dx); PARAM_FLOAT(dy); PARAM_FLOAT(dz);
	PARAM_FLOAT(scale);
	PARAM_FLOAT(brightness);
	PARAM_FLOAT(lifeScale);
	PARAM_COLOR(tint);
	PARAM_FLOAT(vx); PARAM_FLOAT(vy); PARAM_FLOAT(vz);
	PARAM_INT(seed);
	PARAM_INT(follow);
	PARAM_INT(followPlayer);
	PARAM_FLOAT(followShare);
	PARAM_FLOAT(lightScale);
	SpawnEmissiveVolume(self, def, px, py, pz, dx, dy, dz, scale, brightness, lifeScale, tint, vx, vy, vz, seed, follow, followPlayer,
		followShare, lightScale);
	return 0;
}

static void CarveSmoke(FLevelLocals *self, double sx, double sy, double sz, double ex, double ey, double ez,
	double radius, double amount)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("CarveSmoke", badLogged, { sx, sy, sz, ex, ey, ez, radius, amount }))
		return;

	const double amountC = clamp(amount, 0., 1.);
	if (amountC <= 0.)
		return;

	FSmokeCarveEvent *c = self->SmokeCarves.Push();
	if (c == nullptr)
	{
		LogEffectQueueFull(self->SmokeCarves.FullLogged, "CarveSmoke", FLevelLocals::MAX_SMOKE_CARVES_PER_TIC);
		return;
	}
	c->Start = DVector3(sx, sy, sz);
	c->End = LimitEffectSegmentEnd(c->Start, DVector3(ex, ey, ez), 8192.);
	c->Radius = clamp(radius, 1., 256.);
	c->Amount = amountC;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, CarveSmoke, CarveSmoke)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(sx); PARAM_FLOAT(sy); PARAM_FLOAT(sz);
	PARAM_FLOAT(ex); PARAM_FLOAT(ey); PARAM_FLOAT(ez);
	PARAM_FLOAT(radius);
	PARAM_FLOAT(amount);
	CarveSmoke(self, sx, sy, sz, ex, ey, ez, radius, amount);
	return 0;
}

// One look for the level's smoke. Clamps as FLevelLocals::SmokeLookSettings documents;
// the tint's alpha is ignored.
static void SetSmokeLook(FLevelLocals *self, int tint, double absorption, double scatter, double ambient,
	double dissipation, double buoyancy)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("SetSmokeLook", badLogged, { absorption, scatter, ambient, dissipation, buoyancy }))
		return;

	FLevelLocals::SmokeLookSettings &look = self->SmokeLook;
	look.Tint = PalEntry(0xff000000u | ((uint32_t)tint & 0xffffffu));
	look.Absorption = clamp(absorption, 0., 16.);
	look.Scatter = clamp(scatter, 0., 1.);
	look.Ambient = clamp(ambient, 0., 4.);
	look.Dissipation = clamp(dissipation, 0., 10.);
	look.Buoyancy = clamp(buoyancy, -4., 4.);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSmokeLook, SetSmokeLook)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_COLOR(tint);
	PARAM_FLOAT(absorption);
	PARAM_FLOAT(scatter);
	PARAM_FLOAT(ambient);
	PARAM_FLOAT(dissipation);
	PARAM_FLOAT(buoyancy);
	SetSmokeLook(self, tint, absorption, scatter, ambient, dissipation, buoyancy);
	return 0;
}

static void SetSmokeWind(FLevelLocals *self, double wx, double wy, double wz)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("SetSmokeWind", badLogged, { wx, wy, wz }))
		return;
	self->SmokeWind = ClampEffectVector(wx, wy, wz, 1024.);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSmokeWind, SetSmokeWind)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(wx); PARAM_FLOAT(wy); PARAM_FLOAT(wz);
	SetSmokeWind(self, wx, wy, wz);
	return 0;
}

// Empties the volume; the look and the wind stay.
static void ClearSmoke(FLevelLocals *self)
{
	self->SmokeClearSerial++;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearSmoke, ClearSmoke)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearSmoke(self);
	return 0;
}

// SH3: one blast, one call. The smoke volume reads it now; debris (#9) reads the same
// queue later.
static void PushEffectImpulse(FLevelLocals *self, double px, double py, double pz, double radius, double strength)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("PushEffectImpulse", badLogged, { px, py, pz, radius, strength }))
		return;

	const double strengthC = clamp(strength, -4096., 4096.);
	if (strengthC == 0.)
		return;

	FEffectImpulseEvent *i = self->EffectImpulses.Push();
	if (i == nullptr)
	{
		LogEffectQueueFull(self->EffectImpulses.FullLogged, "PushEffectImpulse", FLevelLocals::MAX_EFFECT_IMPULSES_PER_TIC);
		return;
	}
	i->Pos = DVector3(px, py, pz);
	i->Radius = clamp(radius, 1., 4096.);
	i->Strength = strengthC;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, PushEffectImpulse, PushEffectImpulse)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(radius);
	PARAM_FLOAT(strength);
	PushEffectImpulse(self, px, py, pz, radius, strength);
	return 0;
}

//==========================================================================
//
// [SENSORYIMPULSES] ExposureImpulse and HearingImpulse -- a flash that can overwhelm the eye and a blast that can overwhelm the
// ear ("Engine docs/SENSORY_IMPULSES_PLAN.md" 2a, 3a, 6 and 7; FExposureImpulseEvent and FHearingImpulseEvent, g_levellocals.h).
// PushEffectImpulse's shape. ONE-WAY: each only queues an event on the level's per-tic queue (64 a tic each) and returns
// nothing; nothing reads it back into the game and nothing is serialized, so every machine queues the same event and gameplay
// cannot branch on it. No RNG. Each machine then weighs a flash by its own view (hw_exposureimpulse.cpp) and a blast by its
// own listener (s_hearingimpulse.cpp). A call with a non-finite number or no strength is ignored (logged once per native per
// session); a full queue drops the event (logged once per map). Engine callers (explosion code) call the static functions.
//
//==========================================================================

static void ExposureImpulse(FLevelLocals *self, double px, double py, double pz, double strength, double reach, double recovery, int tint)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("ExposureImpulse", badLogged, { px, py, pz, strength, reach, recovery }))
		return;

	const double strengthC = clamp(strength, 0., 16.);
	if (strengthC <= 0.)
		return;

	FExposureImpulseEvent *e = self->ExposureImpulses.Push();
	if (e == nullptr)
	{
		LogEffectQueueFull(self->ExposureImpulses.FullLogged, "ExposureImpulse", FLevelLocals::MAX_EXPOSURE_IMPULSES_PER_TIC);
		return;
	}
	e->Pos = DVector3(px, py, pz);
	e->Strength = strengthC;
	e->Reach = clamp(reach, 16., 8192.);
	e->Recovery = clamp(recovery, 0.25, 4.);
	e->Tint = PalEntry((uint32_t)tint);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ExposureImpulse, ExposureImpulse)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(strength);
	PARAM_FLOAT(reach);
	PARAM_FLOAT(recovery);
	PARAM_COLOR(tint);
	ExposureImpulse(self, px, py, pz, strength, reach, recovery, tint);
	return 0;
}

static void HearingImpulse(FLevelLocals *self, double px, double py, double pz, double strength, double reach, double recovery)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("HearingImpulse", badLogged, { px, py, pz, strength, reach, recovery }))
		return;

	const double strengthC = clamp(strength, 0., 16.);
	if (strengthC <= 0.)
		return;

	FHearingImpulseEvent *e = self->HearingImpulses.Push();
	if (e == nullptr)
	{
		LogEffectQueueFull(self->HearingImpulses.FullLogged, "HearingImpulse", FLevelLocals::MAX_HEARING_IMPULSES_PER_TIC);
		return;
	}
	e->Pos = DVector3(px, py, pz);
	e->Strength = strengthC;
	e->Reach = clamp(reach, 16., 8192.);
	e->Recovery = clamp(recovery, 0.25, 4.);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, HearingImpulse, HearingImpulse)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(strength);
	PARAM_FLOAT(reach);
	PARAM_FLOAT(recovery);
	HearingImpulse(self, px, py, pz, strength, reach, recovery);
	return 0;
}

//==========================================================================
//
// [SURFACEDAMAGE] PaintSurfaceDamage -- lasting damage pressed into the wall or flat at pos ("Engine docs/
// SURFACE_DAMAGE_17_IMPL_NOTES.md"). ONE-WAY, like the smoke natives above: it only queues an event
// (FLevelLocals::SurfaceDamagePaints, 128 a tic); the renderer finds the surface, hands out tiles and stamps the
// brush (hw_surfacedamage.cpp). Nothing is returned, nothing is serialized, no RNG: every machine queues the same
// paint, and each draws it -- or does not, when its damage is off.
//
//==========================================================================

static void PaintSurfaceDamage(FLevelLocals *self, double px, double py, double pz, double nx, double ny, double nz, int brush,
	double radius, double depth, double soot, double heat, double wet, double ax, double ay, double az)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("PaintSurfaceDamage", badLogged, { px, py, pz, nx, ny, nz, radius, depth, soot, heat, wet, ax, ay, az }))
		return;

	const double depthC = clamp(depth, 0., 1.);
	const double sootC = clamp(soot, 0., 1.);
	const double heatC = clamp(heat, 0., 1.);
	const double wetC = clamp(wet, 0., 1.);
	// Nothing to press in: not queued, so it asks for no atlas either.
	if (depthC <= 0. && sootC <= 0. && heatC <= 0. && wetC <= 0.)
		return;
	// The normal says which way the surface faces; without one there is no surface to find.
	const DVector3 normal(nx, ny, nz);
	const double normalLength = normal.Length();
	if (normalLength < 1e-6)
		return;

	FSurfaceDamagePaintEvent *e = self->SurfaceDamagePaints.Push();
	if (e == nullptr)
	{
		LogEffectQueueFull(self->SurfaceDamagePaints.FullLogged, "PaintSurfaceDamage", FLevelLocals::MAX_SURFACE_DAMAGE_PAINTS_PER_TIC);
		return;
	}
	e->Pos = DVector3(px, py, pz);
	e->Normal = normal / normalLength;
	// axis (0,0,0) -- the ZScript default -- is "unrotated".
	const DVector3 axis = ClampEffectVector(ax, ay, az, 65536.);
	const double axisLength = axis.Length();
	e->Axis = axisLength > 1e-6 ? axis / axisLength : DVector3(0., 0., 0.);
	e->Brush = brush;
	// hw_surfacedamageframe.h's SURFACE_DAMAGE_RADIUS_MIN / _MAX.
	e->Radius = clamp(radius, 0.5, 64.);
	e->Depth = depthC;
	e->Soot = sootC;
	e->Heat = heatC;
	e->Wet = wetC;
}

// _NATIVE0, NOT _NATIVE: 16 VM arguments (self, two Vector3s, the brush, five numbers, a Vector3) sit exactly at asmjit's
// 16-argument direct-call cap -- see the note on SetVolumetricBeam. The VM calling convention has no such limit.
DEFINE_ACTION_FUNCTION_NATIVE0(FLevelLocals, PaintSurfaceDamage, PaintSurfaceDamage)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(nx); PARAM_FLOAT(ny); PARAM_FLOAT(nz);
	PARAM_NAME(brush);
	PARAM_FLOAT(radius);
	PARAM_FLOAT(depth);
	PARAM_FLOAT(soot);
	PARAM_FLOAT(heat);
	PARAM_FLOAT(wet);
	PARAM_FLOAT(ax); PARAM_FLOAT(ay); PARAM_FLOAT(az);
	PaintSurfaceDamage(self, px, py, pz, nx, ny, nz, brush.GetIndex(), radius, depth, soot, heat, wet, ax, ay, az);
	return 0;
}

// [RS fork] DIAGNOSTICS FOR THE LEVEL VISUAL STATE -- the sweep, glow, fog and
// darkness setters and the render gates that read them. Off by default and not
// archived. When on, each setter prints when a value CHANGES (never per tic)
// and the renderer prints when a gate flips, so one test run shows whether a
// push reached the engine. Read in hw_drawinfo.cpp, hw_weapon.cpp and events.cpp.
CVAR(Bool, r_visualstate_log, false, 0)

// [RS fork] Sweep and glow-wave shape ids are 0..9 (see SweepShapeDist in
// main.fp). Anything else is treated as 0, OFF. Unchecked, a bad id drew four
// different things: a sphere for the band light, a ring for the fog bow, wave
// and fill, and nothing in the air lattice. Reported once per distinct bad id
// -- not gated, since a bad id is a caller bug worth seeing.
static void ReportBadShapeId(const char *setter, int id)
{
	static const char *lastSetter = nullptr;
	static int lastId = 0;   // 0 is valid, so it is never reported
	if (setter == lastSetter && id == lastId) return;
	lastSetter = setter;
	lastId = id;
	Printf("%s: shape %d is outside 0..9, treated as 0 (off)\n", setter, id);
}

// [BB] Sweep -- up to eight thin bands of light travelling through the world,
// each tested per pixel against world position on every surface, so they wrap
// across floor, wall and ceiling as continuous unbroken lines.
//
//   mode 0 off
//        1 cylinder from origin -- rings expanding outward across a room
//        2 plane along X        -- bars sweeping east/west down a corridor
//                                  (both sides of the origin)
//        3 plane along Y        -- the same, north/south
//        4 sphere from origin   -- shells, so a band rises as it expands
//        5 rise                 -- one sheet climbing along Z (signed)
//        6 +X  7 +Y  8 -X  9 -Y -- one signed front crossing the level
// [round2 SW-21] 5-9 were missing from this list.
//
// Set the origin and count once, then each band's own position and colour.
// Script drives the radii each tic: grow them for a ping, oscillate for a
// sweep, stagger them for a train chasing itself.
static void SetSweepOrigin(FLevelLocals *self, int mode, double x, double y, double z, int count)
{
	// [RS fork] Shape ids 0..9; anything else is off. See ReportBadShapeId.
	if (mode < 0 || mode > 9) { ReportBadShapeId("SetSweepOrigin", mode); mode = 0; }
	self->SweepMode = mode;
	self->SweepOrigin = DVector3(x, y, z);
	self->SweepCount = clamp(count, 0, FLevelLocals::MAX_SWEEP_BANDS);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepOrigin, SetSweepOrigin)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(mode);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	PARAM_INT(count);
	SetSweepOrigin(self, mode, x, y, z, count);
	return 0;
}

static void SetSweepBand(FLevelLocals *self, int index, double radius,
	double thickness, double softness, int color, double intensity)
{
	if (index < 0 || index >= FLevelLocals::MAX_SWEEP_BANDS) return;
	self->SweepRadius[index] = radius;
	self->SweepThickness[index] = thickness;
	self->SweepSoftness[index] = softness;
	self->SweepColor[index] = (PalEntry)color;
	self->SweepIntensity[index] = intensity;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepBand, SetSweepBand)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_FLOAT(radius);
	PARAM_FLOAT(thickness);
	PARAM_FLOAT(softness);
	PARAM_COLOR(color);
	PARAM_FLOAT(intensity);
	SetSweepBand(self, index, radius, thickness, softness, color, intensity);
	return 0;
}

// [round2 A6] Sweep band draw and fill values are 0..4. Out of range is
// treated as 0 and reported once per distinct setter/value, exactly like
// ReportBadShapeId above. Unchecked, a draw of 16+ spilled into the packed fill
// bits and a negative fill made the air lattice draw a band the surfaces did not.
static void ReportBadSweepValue(const char *setter, int value)
{
	static const char *lastSetter = nullptr;
	static int lastValue = 0;   // 0 is valid, so it is never reported
	if (setter == lastSetter && value == lastValue) return;
	lastSetter = setter;
	lastValue = value;
	Printf("%s: %d is outside 0..4, treated as 0\n", setter, value);
}

// What a band does to the pixels it covers:
//   0 default (add), 1 add (the original), 2 lift -- multiply up, which is a
//   reveal -- 3 crush, multiply down (and occludes in the air lattice),
//   4 recolour -- blends the glow toward the band's colour.
// Uploaded packed as drawmode + 16*fill (0-4) + 256*passed (0/1); see
// FRenderState::SetSweepBandDraw. [round2 SW-21]
static void SetSweepBandDraw(FLevelLocals *self, int index, int drawmode)
{
	if (index < 0 || index >= FLevelLocals::MAX_SWEEP_BANDS) return;
	if (drawmode < 0 || drawmode > 4) { ReportBadSweepValue("SetSweepBandDraw", drawmode); drawmode = 0; }
	self->SweepBandDraw[index] = drawmode;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepBandDraw, SetSweepBandDraw)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_INT(drawmode);
	SetSweepBandDraw(self, index, drawmode);
	return 0;
}

// Set ONLY the band count. SetSweepOrigin also seeds all eight per-band
// origins, so it cannot be used to correct the count after those origins have
// been set -- it would erase them. Hence a setter that touches nothing else.
static void SetSweepCount(FLevelLocals *self, int count)
{
	self->SweepCount = clamp(count, 0, FLevelLocals::MAX_SWEEP_BANDS);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepCount, SetSweepCount)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(count);
	SetSweepCount(self, count);
	return 0;
}

// Give one band its own origin and shape. Mode 0 hands it back to the shared
// origin. Called after SetSweepOrigin, which seeds all eight.
static void SetSweepBandAt(FLevelLocals *self, int index, double x, double y, double z, int mode)
{
	if (index < 0 || index >= FLevelLocals::MAX_SWEEP_BANDS) return;
	// [RS fork] Shape ids 0..9; anything else is 0, which hands the band back
	// to the shared origin rather than inventing a shape. See ReportBadShapeId.
	if (mode < 0 || mode > 9) { ReportBadShapeId("SetSweepBandAt", mode); mode = 0; }
	self->SweepBandOrigin[index] = DVector3(x, y, z);
	self->SweepBandMode[index] = mode;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepBandAt, SetSweepBandAt)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	PARAM_INT(mode);
	SetSweepBandAt(self, index, x, y, z, mode);
	return 0;
}

static void SetSweepTrail(FLevelLocals *self, double trail)
{
	self->SweepTrail = trail;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepTrail, SetSweepTrail)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(trail);
	SetSweepTrail(self, trail);
	return 0;
}

// [BB] GLOW WAVE. Peaks and valleys along a glow, per pixel.
//
// Reach, brightness and the two-colour boundary can each take the wave at
// their own depth. Reach is the one that cannot be faked any other way: it
// moves the EDGE of the band rather than how bright the band is.
//
// Wavelength 0 switches the whole thing off and every glow goes back to the
// arithmetic it did before, byte for byte.
// THE ORIGIN IS SET SEPARATELY, AND ON PURPOSE.
//
// Everything else here is a number read straight off a slider, so it can be
// pushed from ANY scope -- including a menu's own ticker, which keeps running
// while the playsim is paused. That is what lets these sliders move the
// picture while you are still looking at the page.
//
// The origin is its own call: resolving "follows you" or "the nearest live
// monster" means reading the playsim, so callers make it from the world tic,
// and it simply keeps its last value while the game is paused -- which is
// correct, because nothing in the world is moving either. [round2 X1] It is
// DECLARED clearscope all the same: the resolving happens in the caller, and
// this setter only stores the point.
static void SetGlowWave(FLevelLocals *self, double wavelength, double speed,
	double sharpness, int shape)
{
	// [RS fork] Shape ids 0..9 (0 still means the ring, as before). Anything
	// else switches the wave off -- wavelength 0 is its off switch.
	if (shape < 0 || shape > 9) { ReportBadShapeId("SetGlowWave", shape); wavelength = 0; shape = 1; }
	self->GlowWaveLength = wavelength;
	self->GlowWaveSpeed = speed;
	self->GlowWaveSharp = sharpness;
	self->GlowWaveShape = shape > 0 ? shape : 1;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetGlowWave, SetGlowWave)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(wavelength);
	PARAM_FLOAT(speed);
	PARAM_FLOAT(sharpness);
	PARAM_INT(shape);
	SetGlowWave(self, wavelength, speed, sharpness, shape);
	return 0;
}

static void SetGlowWaveOrigin(FLevelLocals *self, double x, double y, double z)
{
	self->GlowWaveOrigin = DVector3(x, y, z);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetGlowWaveOrigin, SetGlowWaveOrigin)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	SetGlowWaveOrigin(self, x, y, z);
	return 0;
}

// How far each term swings. Detune adds a second sine at a wavelength that
// does not divide the first, so the room never quite repeats.
static void SetGlowWaveDepth(FLevelLocals *self, double reach, double bright,
	double colour, double detune, double seed)
{
	self->GlowWaveReach = reach;
	self->GlowWaveBright = bright;
	self->GlowWaveColour = colour;
	self->GlowWaveDetune = detune;
	self->GlowWaveSeed = seed;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetGlowWaveDepth, SetGlowWaveDepth)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(reach); PARAM_FLOAT(bright);
	PARAM_FLOAT(colour); PARAM_FLOAT(detune); PARAM_FLOAT(seed);
	SetGlowWaveDepth(self, reach, bright, colour, detune, seed);
	return 0;
}

// One phase per channel, in radians. Offsetting them is what makes a single
// wave CLIMB a room -- floor crests, then the lower wall, then the upper
// wall, then the ceiling -- rather than every surface pulsing as one.
static void SetGlowWavePhase(FLevelLocals *self, double wallTop, double wallBottom,
	double floorPhase, double ceilPhase)
{
	self->GlowWavePhase[0] = wallTop;
	self->GlowWavePhase[1] = wallBottom;
	self->GlowWavePhase[2] = floorPhase;
	self->GlowWavePhase[3] = ceilPhase;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetGlowWavePhase, SetGlowWavePhase)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(wallTop); PARAM_FLOAT(wallBottom);
	PARAM_FLOAT(floorPhase); PARAM_FLOAT(ceilPhase);
	SetGlowWavePhase(self, wallTop, wallBottom, floorPhase, ceilPhase);
	return 0;
}

static void ClearGlowWave(FLevelLocals *self)
{
	self->GlowWaveLength = 0;
	self->GlowWaveReach = 0;
	self->GlowWaveBright = 0;
	self->GlowWaveColour = 0;
	self->GlowWaveDetune = 0;
	self->GlowWaveSeed = 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearGlowWave, ClearGlowWave)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearGlowWave(self);
	return 0;
}

// [BB] DARKNESS AS A SHADER TERM.
//
// Darkening a room by scaling its sector colour makes the sector the unit of
// lighting: one multiplier, one room, wall to wall. That was correct when a
// sector's light level was the only lever there was, and it stopped being
// correct the moment a band of light could be measured per pixel.
//
// These carry the SAME four curves, unchanged -- subtract, compress, cap and
// deepen, with pre-gain before and min-light and post-gain after -- and hand
// them to the fragment shader to evaluate against the FRAGMENT's light
// instead of the sector's. The arithmetic is identical. What changes is how
// often it is asked, and that it can then take terms a sector cannot have:
// distance from the eye, and height.
//
// mode 0 switches the whole thing off and every fragment keeps the light it
// already had.
static void SetDarkness(FLevelLocals *self, int mode, double adjust,
	double minLight, double preGain, double postGain)
{
	self->DarkMode = mode;
	self->DarkAdjust = adjust;
	self->DarkMinLight = minLight;
	self->DarkPreGain = preGain;
	self->DarkPostGain = postGain;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDarkness, SetDarkness)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(mode);
	PARAM_FLOAT(adjust); PARAM_FLOAT(minLight);
	PARAM_FLOAT(preGain); PARAM_FLOAT(postGain);
	SetDarkness(self, mode, adjust, minLight, preGain, postGain);
	return 0;
}

// The two terms that only exist per pixel, and the reason the move is worth
// making at all.
//
// DISTANCE is the big one: darkness deepening with range is what makes a dark
// room feel like it has depth rather than like the brightness slider went
// down. A sector has no way to express it -- every pixel in the room is the
// same distance as far as a sector multiplier is concerned.
//
// HEIGHT pools the dark at floor level, or raises it as a tide. Same idea in
// the other axis, and free once the term is per fragment.
static void SetDarknessSpace(FLevelLocals *self, double distDepth, double distRange,
	double heightDepth, double heightRef, double heightRange)
{
	self->DarkDistDepth = distDepth;
	self->DarkDistRange = distRange;
	self->DarkHeightDepth = heightDepth;
	self->DarkHeightRef = heightRef;
	self->DarkHeightRange = heightRange;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDarknessSpace, SetDarknessSpace)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(distDepth); PARAM_FLOAT(distRange);
	PARAM_FLOAT(heightDepth); PARAM_FLOAT(heightRef); PARAM_FLOAT(heightRange);
	SetDarknessSpace(self, distDepth, distRange, heightDepth, heightRef, heightRange);
	return 0;
}

// [BB] HOW MUCH OF THE DARKNESS ACTORS ARE SPARED.
//
// The darkness pass takes the scene down as a whole, monsters included, so a
// room dark enough to be worth lighting is a room you cannot see anything
// coming in. 0 is the old behaviour and stays the default.
static void SetDarknessActors(FLevelLocals *self, double exempt)
{
	self->DarkActorExempt = clamp(exempt, 0.0, 1.0);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDarknessActors, SetDarknessActors)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(exempt);
	SetDarknessActors(self, exempt);
	return 0;
}

// [BB] Fog by room type -- see FLevelLocals::FogIndoorScale. A sky ceiling is
// outdoors, anything else is indoors, which is the marker every Doom map
// already carries.
static void SetFogZones(FLevelLocals *self, double indoor, double outdoor)
{
	self->FogIndoorScale = max(indoor, 0.0);
	self->FogOutdoorScale = max(outdoor, 0.0);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogZones, SetFogZones)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(indoor); PARAM_FLOAT(outdoor);
	SetFogZones(self, indoor, outdoor);
	return 0;
}

// [BB] FOG WITH A TOP.
//
// Sector fog tints surfaces by distance and simulates nothing in the air, so
// it can never have a ceiling, a thickness you stand in, or a bright patch
// where a torch sweeps it. This is a horizontal slab of participating medium
// with a world-space top, solved analytically in the fragment shader.
//
// Density 0 switches it off entirely.
static void SetFogSlab(FLevelLocals *self, double topZ, double density,
	double softness, double scatter, int color)
{
	self->FogSlabActive = density > 0.0;
	self->FogSlabTop = topZ;
	self->FogSlabDensity = density;
	self->FogSlabSoft = softness;
	self->FogSlabScatter = scatter;
	self->FogSlabColor = (PalEntry)color;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogSlab, SetFogSlab)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(topZ); PARAM_FLOAT(density);
	PARAM_FLOAT(softness); PARAM_FLOAT(scatter);
	PARAM_COLOR(color);
	SetFogSlab(self, topZ, density, softness, scatter, color);
	return 0;
}

// The trail you kick up walking through it. ONE point that lags behind the
// player rather than a history buffer -- a trail that settles IS a point that
// follows you slowly, and the spring lives in script where it can be tuned.
static void SetFogWake(FLevelLocals *self, double x, double y, double z,
	double radius, double strength)
{
	self->FogSlabWakePos = DVector3(x, y, z);
	self->FogSlabWakeRadius = radius;
	self->FogSlabWakeStrength = strength;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogWake, SetFogWake)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	PARAM_FLOAT(radius); PARAM_FLOAT(strength);
	SetFogWake(self, x, y, z, radius, strength);
	return 0;
}

// [RS fork] THE WAKE'S SHAPE WITHOUT ITS POSITION.
//
// SetFogWake is play scope because the position comes from the playsim, and
// radius and strength rode along with it -- so the wake's look sliders only
// moved while the game ran and did nothing with a menu open. These three are
// look settings, so they get a clearscope setter a UiTick can push.
// SetFogWakePos is the position half, so a caller that splits them does not
// write radius and strength from two scopes. SetFogWake is unchanged.
static void SetFogWakeShape(FLevelLocals *self, double radius, double strength,
	double stretch)
{
	if (r_visualstate_log && (radius != self->FogSlabWakeRadius
		|| strength != self->FogSlabWakeStrength || stretch != self->FogWakeStretch))
		Printf("SetFogWakeShape: radius %.1f strength %.2f stretch %.2f\n", radius, strength, stretch);
	self->FogSlabWakeRadius = radius;
	self->FogSlabWakeStrength = strength;
	self->FogWakeStretch = stretch;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogWakeShape, SetFogWakeShape)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(radius); PARAM_FLOAT(strength); PARAM_FLOAT(stretch);
	SetFogWakeShape(self, radius, strength, stretch);
	return 0;
}

static void SetFogWakePos(FLevelLocals *self, double x, double y, double z)
{
	self->FogSlabWakePos = DVector3(x, y, z);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogWakePos, SetFogWakePos)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	SetFogWakePos(self, x, y, z);
	return 0;
}

// The slab's surface, animated. A flat top reads as a sheet once you can
// see it clearly; two waves at an angle to each other interfere, and
// interference is what looks like a surface rolling rather than a pattern
// scrolling. Amplitude 0 leaves it perfectly flat.
static void SetFogSurface(FLevelLocals *self, double amp, double wavelength,
	double speed, double cross)
{
	self->FogSurfAmp = amp;
	self->FogSurfLen = wavelength;
	self->FogSurfSpeed = speed;
	self->FogSurfCross = cross;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogSurface, SetFogSurface)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(amp); PARAM_FLOAT(wavelength);
	PARAM_FLOAT(speed); PARAM_FLOAT(cross);
	SetFogSurface(self, amp, wavelength, speed, cross);
	return 0;
}

// The layer's BOTTOM. Far below any map is a half-space -- fog on the floor,
// which is all this could do before. Raise it and the slab becomes a layer:
// ceiling fog, a band floating at chest height, or a drain by walking the
// bottom up toward the top.
static void SetFogBottom(FLevelLocals *self, double botZ, double period, double roll)
{
	self->FogSlabBottom = botZ;
	self->FogSlabPeriod = period;
	self->FogSlabRoll = roll;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogBottom, SetFogBottom)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(botZ); PARAM_FLOAT(period); PARAM_FLOAT(roll);
	SetFogBottom(self, botZ, period, roll);
	return 0;
}

// A tornado. Doom Z is the shader Y, same swizzle as everything else here.
//
// Density 0 switches it off. Radius flares from base to top on a curve rather
// than a straight taper, because the pinch near the ground is most of the
// silhouette; swirl is what reads as rotation; lean is what stops it being a
// traffic cone.
static void SetTornado(FLevelLocals *self, double x, double y,
	double baseZ, double topZ, double radBase, double radTop, double density)
{
	self->TornadoPos = DVector2(x, y);
	self->TornadoBase = baseZ;
	self->TornadoTop = topZ;
	self->TornadoRadBase = radBase;
	self->TornadoRadTop = radTop;
	self->TornadoDensity = density;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetTornado, SetTornado)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x); PARAM_FLOAT(y);
	PARAM_FLOAT(baseZ); PARAM_FLOAT(topZ);
	PARAM_FLOAT(radBase); PARAM_FLOAT(radTop); PARAM_FLOAT(density);
	SetTornado(self, x, y, baseZ, topZ, radBase, radTop, density);
	return 0;
}

// ===========================================================================
// [BB] TEXTURE INSIDE THE GLOW.
//
// The glow wave varies a lane's EDGE, which is the right answer while the
// edge is on screen and no answer at all once reach saturates -- a maxed lane
// is a solid card of colour with a wave moving a boundary nobody can see.
// Everything here happens WITHIN the lit area instead, so a saturated lane
// still has somewhere to go.
//
// Five terms, all multipliers on the glow's finished contribution so none of
// them can move a band's shape: noise for material, flow for current along
// the surface, cells for veins, the disturbance array so gunfire crosses the
// walls, and one state level so the room can look alarmed.
static void SetGlowTexture(FLevelLocals *self, double noise, double scale,
	double drift, double contrast)
{
	self->GlowTexNoise = noise;
	self->GlowTexScale = scale;
	self->GlowTexDrift = drift;
	self->GlowTexContrast = contrast;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetGlowTexture, SetGlowTexture)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(noise); PARAM_FLOAT(scale);
	PARAM_FLOAT(drift); PARAM_FLOAT(contrast);
	SetGlowTexture(self, noise, scale, drift, contrast);
	return 0;
}

static void SetGlowFlow(FLevelLocals *self, double amount, double spacing,
	double speed, double sharp)
{
	self->GlowFlow = amount;
	self->GlowFlowSpacing = spacing;
	self->GlowFlowSpeed = speed;
	self->GlowFlowSharp = sharp;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetGlowFlow, SetGlowFlow)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(amount); PARAM_FLOAT(spacing);
	PARAM_FLOAT(speed); PARAM_FLOAT(sharp);
	SetGlowFlow(self, amount, spacing, speed, sharp);
	return 0;
}

static void SetGlowCells(FLevelLocals *self, double amount, double scale,
	double speed, double width)
{
	self->GlowCell = amount;
	self->GlowCellScale = scale;
	self->GlowCellSpeed = speed;
	self->GlowCellWidth = width;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetGlowCells, SetGlowCells)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(amount); PARAM_FLOAT(scale);
	PARAM_FLOAT(speed); PARAM_FLOAT(width);
	SetGlowCells(self, amount, scale, speed, width);
	return 0;
}

// React is the disturbance array reaching the walls; pulse and level are the
// room's own alarm. Kept together because both are "the glow responding to
// something" rather than "the glow having a texture".
//
// [RS fork] `rate` is back (it was in 07b540ba1b and went out only because
// 79c31bffc8 reset the whole tree to the 5.0.0-dxr release). It multiplies the
// beat rate the level implies, so depth and speed are separable -- without it a
// bright alarm is always a fast one. Defaulted to 1.0 in ZScript, which is the
// old rate exactly, so three-argument callers are untouched.
static void SetGlowReact(FLevelLocals *self, double react, double pulse,
	double level, double rate)
{
	self->GlowReact = react;
	self->GlowPulse = pulse;
	self->GlowPulseLevel = level;
	if (r_visualstate_log && rate != self->GlowPulseRate)
		Printf("SetGlowReact: pulse rate x%.2f (level %.2f beats at %.2f Hz)\n", rate, level,
			(1.0 + 6.0 * clamp(level, 0.0, 1.0)) * max(rate, 0.0) * 0.35);
	self->GlowPulseRate = rate;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetGlowReact, SetGlowReact)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(react); PARAM_FLOAT(pulse); PARAM_FLOAT(level); PARAM_FLOAT(rate);
	SetGlowReact(self, react, pulse, level, rate);
	return 0;
}

// [BB] THE HEATMAP -- where the fighting happened.
//
// Deliberately NOT the disturbance array. A disturbance is a handful of
// short-lived events and belongs in uniforms; a heatmap is hundreds of
// permanent deposits that have to be SUMMED, and eight slots cannot express
// that any more than eighty could. A sum wants a bucket, not a list, so this
// is a coarse grid over the map's own extent -- and the thousandth death costs
// exactly what the first one cost.
//
// Stamped on the CPU because deaths are rare. A few a second at worst, and a
// stamp is a couple of thousand float adds, which is nothing next to doing it
// per pixel per frame forever.
// ===========================================================================

// The map's own bounding box, from the blockmap, which is the one structure
// that already knows it. Falls back to something harmless on a level with no
// blockmap rather than dividing by zero.
static void HeatBounds(FLevelLocals *self, double &ox, double &oy,
	double &w, double &h)
{
	ox = self->blockmap.bmaporgx;
	oy = self->blockmap.bmaporgy;
	w = std::max((double)(self->blockmap.bmapwidth * FBlockmap::MAPBLOCKUNITS), 1.0);
	h = std::max((double)(self->blockmap.bmapheight * FBlockmap::MAPBLOCKUNITS), 1.0);
}

static void HeatEnsure(FLevelLocals *self)
{
	if (self->HeatIntensity.Size() == (unsigned)(FLevelLocals::HEAT_RES * FLevelLocals::HEAT_RES))
		return;
	self->HeatIntensity.Resize(FLevelLocals::HEAT_RES * FLevelLocals::HEAT_RES);
	self->HeatHeight.Resize(FLevelLocals::HEAT_RES * FLevelLocals::HEAT_RES);
	memset(&self->HeatIntensity[0], 0, self->HeatIntensity.Size() * sizeof(float));
	memset(&self->HeatHeight[0], 0, self->HeatHeight.Size() * sizeof(float));
	self->HeatEverUsed = true;
	self->HeatDirty = true;
}

// One deposit. Radius is in WORLD units, not cells, so a slider means the same
// thing on a cramped map as on an open one -- which is the whole reason the
// grid resolution is not exposed.
static void HeatmapAdd(FLevelLocals *self, double x, double y, double z,
	double radius, double amount)
{
	if (amount <= 0.0) return;
	HeatEnsure(self);

	double ox, oy, mw, mh;
	HeatBounds(self, ox, oy, mw, mh);

	const int R = FLevelLocals::HEAT_RES;
	double cellW = mw / R, cellH = mh / R;

	double cx = (x - ox) / cellW;
	double cy = (y - oy) / cellH;

	// Radius converted per axis, because a map's bounding box is rarely square
	// and one cell is not the same size in both directions.
	int spanX = std::max(int(ceil(radius / cellW)), 1);
	int spanY = std::max(int(ceil(radius / cellH)), 1);

	int i0 = clamp(int(cx) - spanX, 0, R - 1), i1 = clamp(int(cx) + spanX, 0, R - 1);
	int j0 = clamp(int(cy) - spanY, 0, R - 1), j1 = clamp(int(cy) + spanY, 0, R - 1);

	for (int j = j0; j <= j1; j++)
	{
		for (int i = i0; i <= i1; i++)
		{
			// Measured back in world units so the falloff is round on screen
			// rather than round in cell space, which would be an ellipse.
			double dx = (i + 0.5 - cx) * cellW;
			double dy = (j + 0.5 - cy) * cellH;
			double d = sqrt(dx * dx + dy * dy);
			if (d > radius) continue;

			// Smooth, not flat. A flat disc accumulates into visible tiles
			// with hard edges the moment two deposits overlap.
			double f = 1.0 - d / radius;
			f = f * f * (3.0 - 2.0 * f);

			int idx = j * R + i;
			float before = self->HeatIntensity[idx];
			self->HeatIntensity[idx] = float(before + amount * f);

			// The height follows whichever deposit is contributing most, so a
			// cell that has only ever seen one storey reports that storey. Two
			// storeys stacked exactly will fight, and that is a fair trade for
			// not storing a list per cell.
			if (amount * f > before) self->HeatHeight[idx] = float(z);
		}
	}
	self->HeatDirty = true;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, HeatmapAdd, HeatmapAdd)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	PARAM_FLOAT(radius); PARAM_FLOAT(amount);
	HeatmapAdd(self, x, y, z, radius, amount);
	return 0;
}

static void HeatmapClear(FLevelLocals *self)
{
	if (self->HeatIntensity.Size() == 0) return;
	memset(&self->HeatIntensity[0], 0, self->HeatIntensity.Size() * sizeof(float));
	memset(&self->HeatHeight[0], 0, self->HeatHeight.Size() * sizeof(float));
	self->HeatDirty = true;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, HeatmapClear, HeatmapClear)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	HeatmapClear(self);
	return 0;
}

// How it is drawn. Scale 0 switches it off without discarding what has been
// accumulated, so a player can toggle it on to see the shape of a fight they
// have already had.
// What survives a colour drain. The drain itself is the sector desaturation
// that has always been there; this only decides which colours it is allowed to
// take. Threshold 0 restores the old all-or-nothing behaviour exactly.
static void SetDesatKeep(FLevelLocals *self, double threshold, double soft, int hue)
{
	self->DesatKeep = threshold;
	self->DesatKeepSoft = soft;
	self->DesatKeepHue = hue;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDesatKeep, SetDesatKeep)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(threshold); PARAM_FLOAT(soft); PARAM_INT(hue);
	SetDesatKeep(self, threshold, soft, hue);
	return 0;
}

// [BB] The DRAIN, as one number for the frame.
//
// SetDesatKeep decides what survives desaturation; this is how much
// desaturation there is to survive. Before it, the only way to grey a map from
// script was to walk every sector and rewrite its colormap byte -- the same
// per-sector mutation SetDarkness exists to spare a mod, with the same costs:
// it fights anything else that touches sector colour, and it has to be undone
// by hand rather than simply stopped.
//
// Clamped here rather than in the shader because a caller passing 2.0 means
// "as grey as possible" and should get it, not a wrapped value.
static void SetDesatGlobal(FLevelLocals *self, double amount)
{
	self->DesatGlobal = clamp(amount, 0.0, 1.0);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDesatGlobal, SetDesatGlobal)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(amount);
	SetDesatGlobal(self, amount);
	return 0;
}

static void SetHeatmap(FLevelLocals *self, double scale, int lowCol,
	int highCol, double ceiling, double decay, double tolerance)
{
	self->HeatScale = scale;
	self->HeatColorLow = lowCol;
	self->HeatColorHigh = highCol;
	self->HeatCeiling = std::max(ceiling, 0.01);
	self->HeatDecay = decay;
	self->HeatTolerance = tolerance;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetHeatmap, SetHeatmap)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(scale); PARAM_COLOR(lowCol); PARAM_COLOR(highCol);
	PARAM_FLOAT(ceiling); PARAM_FLOAT(decay); PARAM_FLOAT(tolerance);
	SetHeatmap(self, scale, lowCol, highCol, ceiling, decay, tolerance);
	return 0;
}

// Reading it back, so script can ask "how bad was it here" -- which is what
// makes this a design tool and not only a picture. A spawn director can weight
// against cells that have already seen a lot.
static double HeatmapAt(FLevelLocals *self, double x, double y)
{
	if (self->HeatIntensity.Size() == 0) return 0.0;

	double ox, oy, mw, mh;
	HeatBounds(self, ox, oy, mw, mh);
	const int R = FLevelLocals::HEAT_RES;

	int i = clamp(int((x - ox) / (mw / R)), 0, R - 1);
	int j = clamp(int((y - oy) / (mh / R)), 0, R - 1);
	return self->HeatIntensity[j * R + i];
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, HeatmapAt, HeatmapAt)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x); PARAM_FLOAT(y);
	ACTION_RETURN_FLOAT(HeatmapAt(self, x, y));
}

// ===========================================================================
// [BB] SHAPES -- signed distance fields drawn onto surfaces.
//
// A ring buffer like the disturbances, and for the same reason: a shape is
// mostly a short-lived event, and refusing the seventeenth one makes the most
// interesting moment of a firefight silently do nothing. The oldest slot is
// always the right one to lose.
//
// A shape with life 0 never expires and holds its slot until cleared, which is
// what a permanent marker wants. Those are placed at the FRONT of the array by
// convention -- nothing enforces it, but a script that mixes permanent and
// transient marks in one buffer will eventually recycle a permanent one, and
// the fix for that is two arrays rather than a rule nobody remembers.
// ===========================================================================
static int ShapeSlot(FLevelLocals *self)
{
	double now = self->maptime / (double)TICRATE;
	int slot = -1;
	double oldest = 1e30;

	for (int i = 0; i < FLevelLocals::MAX_SHAPES; i++)
	{
		if (self->ShapeSize[i] <= 0.0 || self->ShapeKind[i] <= 0)
			return i;
		if (self->ShapeLife[i] > 0.0 &&
			now - self->ShapeBirth[i] > self->ShapeLife[i])
			return i;
		// Permanent shapes are never the oldest candidate -- they cannot age
		// out, so treating them as stale would recycle the one thing that was
		// explicitly asked to stay.
		if (self->ShapeLife[i] > 0.0 && self->ShapeBirth[i] < oldest)
		{
			oldest = self->ShapeBirth[i];
			slot = i;
		}
	}
	return (slot >= 0) ? slot : 0;
}

static int AddShape(FLevelLocals *self, int kind, int orient,
	double x, double y, double z, double size, double angle, double thick,
	int color, double intensity, double life)
{
	if (kind <= 0 || size <= 0.0) return -1;

	int i = ShapeSlot(self);
	self->ShapePos[i] = DVector3(x, y, z);
	self->ShapeSize[i] = size;
	self->ShapeKind[i] = clamp(kind, 0, 7);
	self->ShapeOrient[i] = clamp(orient, 0, 3);   // 3 = standing; see StandingShapesAt() in main.fp
	self->ShapeAngle[i] = angle;
	self->ShapeThick[i] = thick;
	self->ShapeColor[i] = color;
	self->ShapeIntensity[i] = intensity;
	self->ShapeLife[i] = life;
	self->ShapeBirth[i] = self->maptime / (double)TICRATE;
	self->ShapeSeam[i] = 0;
	self->ShapeSeamRate[i] = 0;
	self->ShapeGrow[i] = 0;
	self->ShapeRepeat[i] = 0;
	self->ShapePitch[i] = 0;
	self->ShapeRoll[i] = 0;
	self->ShapeYawRate[i] = 0;
	self->ShapePitchRate[i] = 0;
	self->ShapeRollRate[i] = 0;
	self->ShapeParent[i] = -1;    // explicit: int arrays zero-init, and 0 is a real slot
	self->ShapeLocalPos[i] = DVector3(0, 0, 0);
	self->ShapeLocalYaw[i] = 0;
	self->ShapeLocalPitch[i] = 0;
	self->ShapeLocalRoll[i] = 0;
	return i;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, AddShape, AddShape)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(kind); PARAM_INT(orient);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	PARAM_FLOAT(size); PARAM_FLOAT(angle); PARAM_FLOAT(thick);
	PARAM_COLOR(color); PARAM_FLOAT(intensity); PARAM_FLOAT(life);
	ACTION_RETURN_INT(AddShape(self, kind, orient, x, y, z, size, angle,
		thick, color, intensity, life));
}

// The seam, and growth. Separate from AddShape because they are the ANIMATED
// half and a caller usually wants to place a shape and only sometimes wants it
// to move -- and because both are resolved per frame from a rate rather than
// stepped per tic from script.
static void SetShapeMotion(FLevelLocals *self, int slot, double seam,
	double seamRate, double grow)
{
	if (slot < 0 || slot >= FLevelLocals::MAX_SHAPES) return;
	self->ShapeSeam[slot] = seam;
	self->ShapeSeamRate[slot] = seamRate;
	self->ShapeGrow[slot] = grow;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetShapeMotion, SetShapeMotion)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot); PARAM_FLOAT(seam); PARAM_FLOAT(seamRate); PARAM_FLOAT(grow);
	SetShapeMotion(self, slot, seam, seamRate, grow);
	return 0;
}

// Moving one that already exists, so a shape can follow an actor without
// being re-added every tic and losing its age with it.
// ONE SLOT, MANY COPIES. The coordinate is folded rather than the shape being
// drawn N times, so eight copies and eight hundred cost the same.
//
// This does not replace the slots and is not meant to: every copy in a
// formation is necessarily identical -- same age, same colour, same fade --
// and a kill mark needs its own clock. Slots are for distinct events; this is
// for many of the same thing at once.
//
// The anchor is the slot's own position, so a formation follows an actor
// exactly as a single shape does. Dynamic and repeated are not opposites.
//
//   mode 1 RADIAL  count copies around a circle of radius space, spinning
//   mode 2 GRID    tiled every space units, out to count, drifting
static void SetShapeRepeat(FLevelLocals *self, int slot, int mode,
	double count, double space, double spin)
{
	if (slot < 0 || slot >= FLevelLocals::MAX_SHAPES) return;
	self->ShapeRepeat[slot] = clamp(mode, 0, 2);
	self->ShapeRepCount[slot] = count;
	self->ShapeRepSpace[slot] = space;
	self->ShapeRepSpin[slot] = spin;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetShapeRepeat, SetShapeRepeat)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot); PARAM_INT(mode);
	PARAM_FLOAT(count); PARAM_FLOAT(space); PARAM_FLOAT(spin);
	SetShapeRepeat(self, slot, mode, count, space, spin);
	return 0;
}

static void MoveShape(FLevelLocals *self, int slot, double x, double y, double z)
{
	if (slot < 0 || slot >= FLevelLocals::MAX_SHAPES) return;
	self->ShapePos[slot] = DVector3(x, y, z);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, MoveShape, MoveShape)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot); PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	MoveShape(self, slot, x, y, z);
	return 0;
}

// [BB] Pitch, roll, and a rate for yaw/pitch/roll alike -- standing shapes
// only (orient 3; see StandingShapesAt() in main.fp). Base yaw stays on
// AddShape's own angle parameter, unchanged, so this does not touch the
// decal orientations (0-2) at all.
//
// RESOLVED THE SAME WAY GROW AND SEAMRATE ALREADY ARE: base plus rate times
// age, once per frame, natively -- not stepped per tic from script. A
// caller that wants a panel spinning in place sets a rate once and is done;
// nothing has to poll it.
static void SetShapeOrient(FLevelLocals *self, int slot, double pitch,
	double roll, double yawRate, double pitchRate, double rollRate)
{
	if (slot < 0 || slot >= FLevelLocals::MAX_SHAPES) return;
	self->ShapePitch[slot] = pitch;
	self->ShapeRoll[slot] = roll;
	self->ShapeYawRate[slot] = yawRate;
	self->ShapePitchRate[slot] = pitchRate;
	self->ShapeRollRate[slot] = rollRate;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetShapeOrient, SetShapeOrient)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot); PARAM_FLOAT(pitch); PARAM_FLOAT(roll);
	PARAM_FLOAT(yawRate); PARAM_FLOAT(pitchRate); PARAM_FLOAT(rollRate);
	SetShapeOrient(self, slot, pitch, roll, yawRate, pitchRate, rollRate);
	return 0;
}

// [BB] LINKING -- one shape's world transform composes with its parent's.
// See the long comment on ShapeParent in g_levellocals.h for the two rules
// this depends on the caller keeping: parent index < child index (so one
// forward pass resolves parents before children read them), and local
// yaw/pitch/roll is Euler ADDITION onto the parent's resolved orientation,
// exact for a pure-yaw chain and an approximation once pitch and roll are
// combined at the same joint.
//
// parentSlot -1 clears the link -- the shape goes back to its own authored
// position and orientation, resolved with no parent at all.
static void LinkShape(FLevelLocals *self, int slot, int parentSlot,
	double lx, double ly, double lz, double lyaw, double lpitch, double lroll)
{
	if (slot < 0 || slot >= FLevelLocals::MAX_SHAPES) return;
	if (parentSlot < -1 || parentSlot >= FLevelLocals::MAX_SHAPES) return;
	self->ShapeParent[slot] = parentSlot;
	self->ShapeLocalPos[slot] = DVector3(lx, ly, lz);
	self->ShapeLocalYaw[slot] = lyaw;
	self->ShapeLocalPitch[slot] = lpitch;
	self->ShapeLocalRoll[slot] = lroll;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, LinkShape, LinkShape)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot); PARAM_INT(parentSlot);
	PARAM_FLOAT(lx); PARAM_FLOAT(ly); PARAM_FLOAT(lz);
	PARAM_FLOAT(lyaw); PARAM_FLOAT(lpitch); PARAM_FLOAT(lroll);
	LinkShape(self, slot, parentSlot, lx, ly, lz, lyaw, lpitch, lroll);
	return 0;
}

static void RemoveShape(FLevelLocals *self, int slot)
{
	if (slot < 0 || slot >= FLevelLocals::MAX_SHAPES) return;
	self->ShapeSize[slot] = 0;
	self->ShapeKind[slot] = 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, RemoveShape, RemoveShape)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(slot);
	RemoveShape(self, slot);
	return 0;
}

static void ClearShapes(FLevelLocals *self)
{
	for (int i = 0; i < FLevelLocals::MAX_SHAPES; i++)
	{
		self->ShapeSize[i] = 0;
		self->ShapeKind[i] = 0;
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearShapes, ClearShapes)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearShapes(self);
	return 0;
}

// [BB] The box the sweep's air lattice is allowed to exist inside.
//
// The lattice is built from an INFINITE plane -- "perpendicular to X at o.x"
// is a plane that exists at every Y and Z on the map -- so a window pointing
// anywhere near one showed the grid standing in a room the sweep had never
// entered. There was a radius, but the plane itself had no extent, so this
// was never a leak to patch: the primitive had no concept of a room at all.
//
// WHY SCRIPT PUBLISHES THIS RATHER THAN THE RENDERER DERIVING IT: "which
// sectors are one room" is a judgement, not a fact. A Doom room is usually
// several sectors -- steps, light panels, door tracks, alcoves -- and where a
// room stops (a window? a doorway? a rise in the floor?) has no single right
// answer. The renderer has no business guessing, and a mod that wants a
// different answer should be able to give one.
//
// soft <= 0 disables the bound entirely. That is what every map that never
// calls this gets, and it is why this needed no separate enable flag.
static void SetSweepRoom(FLevelLocals *self, double minx, double miny,
	double minz, double maxx, double maxy, double maxz, double soft)
{
	// [round2 A6] Component-wise min/max. One swapped corner made the box
	// inside-out, every point "outside" it, and the air lattice faded out
	// everywhere with nothing to say why.
	self->SweepRoomMin = DVector3(min(minx, maxx), min(miny, maxy), min(minz, maxz));
	self->SweepRoomMax = DVector3(max(minx, maxx), max(miny, maxy), max(minz, maxz));
	self->SweepRoomSoft = soft;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepRoom, SetSweepRoom)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(minx); PARAM_FLOAT(miny); PARAM_FLOAT(minz);
	PARAM_FLOAT(maxx); PARAM_FLOAT(maxy); PARAM_FLOAT(maxz);
	PARAM_FLOAT(soft);
	SetSweepRoom(self, minx, miny, minz, maxx, maxy, maxz, soft);
	return 0;
}

// How they are drawn, shared by all sixteen.
static void SetShapeLook(FLevelLocals *self, double soft, double heightFade,
	double reach, int under)
{
	self->ShapeSoft = soft;
	self->ShapeHeightFade = heightFade;
	self->ShapeReach = reach;
	self->ShapeUnder = under;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetShapeLook, SetShapeLook)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(soft); PARAM_FLOAT(heightFade); PARAM_FLOAT(reach);
	PARAM_COLOR(under);
	SetShapeLook(self, soft, heightFade, reach, under);
	return 0;
}

// [BB] ONE DISTURBANCE PRIMITIVE, FIVE EFFECTS.
//
// A wake, a ripple, an ignition, fog draining from a point and a monster
// shouldering mist aside are the same function with different signs and a
// different answer to "does the radius grow with age". So there is one native
// and one ring buffer, and every reactive fog idea after this is a script call
// with no engine change at all.
//
// The ring recycles the OLDEST slot rather than refusing a ninth. A refusal
// makes the ninth gunshot in a firefight silently do nothing, which is the one
// moment the effect exists for; dropping the oldest costs a disturbance that
// has already mostly faded.
static void FogDisturb(FLevelLocals *self, double x, double y, double z,
	double radius, double strength, double speed, double life, int mode)
{
	if (strength <= 0.0 || life <= 0.0) return;

	int slot = -1;
	double now = self->maptime / (double)TICRATE;
	double oldest = 1e30;

	for (int i = 0; i < FLevelLocals::MAX_FOG_DISTURB; i++)
	{
		// A free slot first: one that never held anything, or one whose life
		// has run out.
		if (self->FogDisturbLife[i] <= 0.0 ||
			now - self->FogDisturbBirth[i] > self->FogDisturbLife[i])
		{
			slot = i;
			break;
		}
		if (self->FogDisturbBirth[i] < oldest)
		{
			oldest = self->FogDisturbBirth[i];
			slot = i;
		}
	}
	if (slot < 0) slot = 0;

	self->FogDisturbPos[slot] = DVector3(x, y, z);
	self->FogDisturbRadius[slot] = radius;
	self->FogDisturbStrength[slot] = strength;
	self->FogDisturbSpeed[slot] = speed;
	self->FogDisturbLife[slot] = life;
	self->FogDisturbBirth[slot] = now;
	self->FogDisturbMode[slot] = clamp(mode, 0, 3);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, FogDisturb, FogDisturb)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	PARAM_FLOAT(radius); PARAM_FLOAT(strength); PARAM_FLOAT(speed);
	PARAM_FLOAT(life); PARAM_INT(mode);
	FogDisturb(self, x, y, z, radius, strength, speed, life, mode);
	return 0;
}

// Clearing them all at once, for a map change or a script that wants a clean
// slate. Setting life to 0 is what marks a slot free, so nothing else needs
// touching.
static void ClearFogDisturb(FLevelLocals *self)
{
	for (int i = 0; i < FLevelLocals::MAX_FOG_DISTURB; i++)
		self->FogDisturbLife[i] = 0;
	self->FogDisturbNext = 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearFogDisturb, ClearFogDisturb)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearFogDisturb(self);
	return 0;
}

// Density stops being one number for the whole map. Depth 0 restores exactly
// the old uniform behaviour.
static void SetFogNoise(FLevelLocals *self, double scale, double depth,
	double driftX, double driftY)
{
	self->FogNoiseScale = scale;
	self->FogNoiseDepth = depth;
	self->FogNoiseDrift = DVector2(driftX, driftY);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogNoise, SetFogNoise)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(scale); PARAM_FLOAT(depth);
	PARAM_FLOAT(driftX); PARAM_FLOAT(driftY);
	SetFogNoise(self, scale, depth, driftX, driftY);
	return 0;
}

// Tendrils: the tornado's maths at small scale, one per cell of a lattice, so
// the count is free and only the spacing matters. Density 0 is off.
static void SetFogTendrils(FLevelLocals *self, double spacing, double radius,
	double height, double density, double rise, double spread, double lean,
	double taper)
{
	self->FogTendrilSpacing = spacing;
	self->FogTendrilRadius = radius;
	self->FogTendrilHeight = height;
	self->FogTendrilDensity = density;
	self->FogTendrilRise = rise;
	self->FogTendrilSpread = spread;
	self->FogTendrilLean = lean;
	self->FogTendrilTaper = taper;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogTendrils, SetFogTendrils)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(spacing); PARAM_FLOAT(radius); PARAM_FLOAT(height);
	PARAM_FLOAT(density); PARAM_FLOAT(rise); PARAM_FLOAT(spread);
	PARAM_FLOAT(lean); PARAM_FLOAT(taper);
	SetFogTendrils(self, spacing, radius, height, density, rise, spread, lean, taper);
	return 0;
}

// The wake, stretched along the way you are going. A disc is a hole you carry
// around; an ellipse is a corridor you carve and leave behind.
static void SetFogWakeMotion(FLevelLocals *self, double velX, double velY,
	double stretch)
{
	self->FogWakeVel = DVector2(velX, velY);
	self->FogWakeStretch = stretch;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogWakeMotion, SetFogWakeMotion)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(velX); PARAM_FLOAT(velY); PARAM_FLOAT(stretch);
	SetFogWakeMotion(self, velX, velY, stretch);
	return 0;
}

// [BB] Which reference each fog edge follows, and how gently.
//
// A slab with one world Z is flat across the whole map. What "fog on the
// floor" means is a constant height ABOVE THE GROUND, and those are different
// questions -- the second one climbs a staircase.
//
// One signed number per edge, sign picking the plane: 0 absolute, positive
// follows the floor, negative follows the ceiling. The magnitude is the
// gentleness, and it is the part worth having -- at 0.3 the surface rises
// three units for every ten the floor does, so a staircase reads as a slope
// rather than as a flight of steps.
//
// Top follows floor is floor fog. Bottom follows ceiling is ceiling fog. Both
// following the floor is a chest-high band that walks upstairs with you.
//
// [RS fork] The edge's value is now a height ABOVE that floor (below that
// ceiling): the shader adds the full plane height and the magnitude blends the
// eye's floor toward each fragment's floor. It used to scale absolute floor Z,
// so ground mist sank under any raised floor. See FogSlabAt in main.fp.
static void SetFogFollow(FLevelLocals *self, double top, double bottom)
{
	self->FogFollowTop = clamp(top, -1.0, 1.0);
	self->FogFollowBottom = clamp(bottom, -1.0, 1.0);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogFollow, SetFogFollow)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(top); PARAM_FLOAT(bottom);
	SetFogFollow(self, top, bottom);
	return 0;
}

// A sweep band pushing mist ahead of itself. Strength 0 and the sweep passes
// through the fog without touching it, as it always did.
static void SetFogBow(FLevelLocals *self, double strength, double width,
	double thin)
{
	self->FogBowStrength = strength;
	self->FogBowWidth = width;
	self->FogBowThin = thin;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogBow, SetFogBow)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(strength); PARAM_FLOAT(width); PARAM_FLOAT(thin);
	SetFogBow(self, strength, width, thin);
	return 0;
}

// A second colour across the layer's own thickness -- cold at the floor, warm
// at the top. Mix 0 keeps the single colour.
static void SetFogGradient(FLevelLocals *self, int color, double mix)
{
	self->FogColor2 = color;
	self->FogColor2Mix = mix;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogGradient, SetFogGradient)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_COLOR(color); PARAM_FLOAT(mix);
	SetFogGradient(self, color, mix);
	return 0;
}

// [round2 B3] A TRANSIENT FOG GRADIENT THAT WINS OVER THE STANDING ONE.
//
// SetFogGradient is one shared slot, and a fog mod re-pushes it every tic, so a
// passing caller's tint (a sweep colouring the mist) was overwritten before it
// reached a frame. Same fix as SetFogSlabOverride: a second slot the renderer
// prefers while it is set. FogColor2/FogColor2Mix are never touched, so
// ClearFogGradientOverride hands the view straight back to them. Mix 0 is a
// valid override (one colour while it lasts); only Clear ends it. One
// override, not a stack. Look-only, so both are clearscope.
static void ClearFogGradientOverride(FLevelLocals *self)
{
	if (r_visualstate_log && self->FogColor2OverrideActive)
		Printf("ClearFogGradientOverride: the standing fog gradient is active again\n");
	self->FogColor2OverrideActive = false;
}

static void SetFogGradientOverride(FLevelLocals *self, int color, double mix)
{
	if (r_visualstate_log && !self->FogColor2OverrideActive)
		Printf("SetFogGradientOverride: the OVERRIDE fog gradient is active (mix %.2f), standing gradient held underneath\n", mix);
	self->FogColor2OverrideActive = true;
	self->FogColor2OverrideColor = (PalEntry)color;
	self->FogColor2OverrideMix = mix;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogGradientOverride, SetFogGradientOverride)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_COLOR(color); PARAM_FLOAT(mix);
	SetFogGradientOverride(self, color, mix);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearFogGradientOverride, ClearFogGradientOverride)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearFogGradientOverride(self);
	return 0;
}

// [RS fork] THE COLOUR AN IGNITE DISTURBANCE BURNS (FogDisturb mode 2).
//
// Ignite was lit with the gradient colour above -- a colour picked for the top
// of the layer, which most presets leave black, so an explosion added black
// light and there was no flash. This gives the flash its own colour. Until it
// is set, or after ClearFogIgniteColor, ignite follows the gradient colour
// exactly as before. Look-only, so clearscope.
static void SetFogIgniteColor(FLevelLocals *self, int color)
{
	const PalEntry c = (PalEntry)color;
	if (r_visualstate_log && (!self->FogIgniteColorSet
		|| (self->FogIgniteColor.d & 0xffffff) != (c.d & 0xffffff)))
		Printf("SetFogIgniteColor: %02x%02x%02x\n", c.r, c.g, c.b);
	self->FogIgniteColor = c;
	self->FogIgniteColorSet = true;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogIgniteColor, SetFogIgniteColor)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_COLOR(color);
	SetFogIgniteColor(self, color);
	return 0;
}

static void ClearFogIgniteColor(FLevelLocals *self)
{
	if (r_visualstate_log && self->FogIgniteColorSet)
		Printf("ClearFogIgniteColor: ignite follows the gradient colour again\n");
	self->FogIgniteColorSet = false;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearFogIgniteColor, ClearFogIgniteColor)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearFogIgniteColor(self);
	return 0;
}

static void SetTornadoLook(FLevelLocals *self, int color, double scatter)
{
	self->TornadoColor = color;
	self->TornadoScatter = scatter;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetTornadoLook, SetTornadoLook)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_COLOR(color); PARAM_FLOAT(scatter);
	SetTornadoLook(self, color, scatter);
	return 0;
}

static void SetTornadoMotion(FLevelLocals *self, double swirl, double spin,
	double twist, double lean, double leanPeriod)
{
	self->TornadoSwirl = swirl;
	self->TornadoSpin = spin;
	self->TornadoTwist = twist;
	self->TornadoLean = lean;
	self->TornadoLeanPeriod = leanPeriod;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetTornadoMotion, SetTornadoMotion)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(swirl); PARAM_FLOAT(spin); PARAM_FLOAT(twist);
	PARAM_FLOAT(lean); PARAM_FLOAT(leanPeriod);
	SetTornadoMotion(self, swirl, spin, twist, lean, leanPeriod);
	return 0;
}

// How much of the surface behind it the mist takes on. This is what makes the
// slab read as a substance rather than a coloured filter over the scene.
static void SetFogPickup(FLevelLocals *self, double amount)
{
	self->FogSlabPickup = amount;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogPickup, SetFogPickup)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(amount);
	SetFogPickup(self, amount);
	return 0;
}

// [BB] THE PATTERN INSIDE A BAND.
//
// Style is frame-global; only the MODE is per band. Putting the style per band
// would mean another vec4[8] in StreamData, and that buffer's size divides
// 64KB into MAX_STREAM_DATA draws -- so it would cost draw batching in every
// frame of the game to let band 3 have a different line width from band 4.
//
// Spacing 0 in an axis means no lines in that axis, which is what collapses
// grid, slats and a single tripwire into one mode.
static void SetSweepFill(FLevelLocals *self, double spacingU, double spacingV,
	double width, double soft, int color, double gap)
{
	self->SweepFillSpacingU = spacingU;
	self->SweepFillSpacingV = spacingV;
	self->SweepFillWidth = width;
	self->SweepFillSoft = soft;
	self->SweepFillColor = (PalEntry)color;
	self->SweepFillGap = gap;
}

// How strongly the lattice is drawn IN THE AIR inside the band, rather than
// only on the surfaces it lands on. Everything else about it -- colour,
// density, width, softness, rotation, drift, flicker, jitter, major lines --
// is shared with the painted version, so one set of controls drives both and
// the two cannot drift apart.
static void SetSweepFillAir(FLevelLocals *self, double amount)
{
	self->SweepFillAir = amount;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepFillAir, SetSweepFillAir)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(amount);
	SetSweepFillAir(self, amount);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepFill, SetSweepFill)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(spacingU); PARAM_FLOAT(spacingV);
	PARAM_FLOAT(width); PARAM_FLOAT(soft);
	PARAM_COLOR(color); PARAM_FLOAT(gap);
	SetSweepFill(self, spacingU, spacingV, width, soft, color, gap);
	return 0;
}

static void SetSweepFillMotion(FLevelLocals *self, double rotate, double drift,
	double major, double majorBoost, double jitter, double flicker,
	double grad, int gradAxis)
{
	self->SweepFillRotate = rotate;
	self->SweepFillDrift = drift;
	self->SweepFillMajor = major;
	self->SweepFillMajorBoost = majorBoost;
	self->SweepFillJitter = jitter;
	self->SweepFillFlicker = flicker;
	self->SweepFillGrad = grad;
	self->SweepFillGradAxis = gradAxis;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepFillMotion, SetSweepFillMotion)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(rotate); PARAM_FLOAT(drift);
	PARAM_FLOAT(major); PARAM_FLOAT(majorBoost);
	PARAM_FLOAT(jitter); PARAM_FLOAT(flicker);
	PARAM_FLOAT(grad); PARAM_INT(gradAxis);
	SetSweepFillMotion(self, rotate, drift, major, majorBoost, jitter, flicker, grad, gradAxis);
	return 0;
}

// Per band: 0 none, 1 grid, 2 dots (where the axes cross), 3 solid slab,
// 4 pickets (bars along U only, spacing snapped to the published room; on the
// surfaces as well as in the air since [round2 C1]).
static void SetSweepBandFill(FLevelLocals *self, int index, int fill)
{
	if (index < 0 || index >= FLevelLocals::MAX_SWEEP_BANDS) return;
	// [round2 A6] 0..4 only, see ReportBadSweepValue. A negative fill made the
	// air lattice draw a band the surfaces did not; 16+ would collide with the
	// packed passed bit.
	if (fill < 0 || fill > 4) { ReportBadSweepValue("SetSweepBandFill", fill); fill = 0; }
	self->SweepBandFill[index] = fill;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepBandFill, SetSweepBandFill)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index); PARAM_INT(fill);
	SetSweepBandFill(self, index, fill);
	return 0;
}

// [round2 B2] THE PASSED REGION. Grade everything BEHIND a band's front --
// per pixel, so the look follows the line exactly instead of flipping a whole
// sector when its centre crosses. See FLevelLocals::SweepBandPassed and
// SweepPassedAt in main.fp. Uploaded as +256 in the packed band word.
// Look-only render state, so both setters are clearscope.
//   on: 0 off, 1 grade this band's passed side
static void SetSweepBandPassed(FLevelLocals *self, int index, int on)
{
	if (index < 0 || index >= FLevelLocals::MAX_SWEEP_BANDS) return;
	self->SweepBandPassed[index] = on != 0 ? 1 : 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepBandPassed, SetSweepBandPassed)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index); PARAM_INT(on);
	SetSweepBandPassed(self, index, on);
	return 0;
}

// [round2 B2] What the passed side looks like, shared by every band with the
// bit. tintMix multiplies the light toward the tint, darken takes that much
// light away, desat drains colour toward grey (all 0..1). soft is the map
// units the look fades in over behind the front (at least 1).
static void SetSweepPassedLook(FLevelLocals *self, int tint, double tintMix,
	double darken, double desat, double soft)
{
	self->SweepPassedTint = (PalEntry)tint;
	self->SweepPassedTintMix = clamp(tintMix, 0., 1.);
	self->SweepPassedDarken = clamp(darken, 0., 1.);
	self->SweepPassedDesat = clamp(desat, 0., 1.);
	self->SweepPassedSoft = max(soft, 1.);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetSweepPassedLook, SetSweepPassedLook)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_COLOR(tint);
	PARAM_FLOAT(tintMix); PARAM_FLOAT(darken);
	PARAM_FLOAT(desat); PARAM_FLOAT(soft);
	SetSweepPassedLook(self, tint, tintMix, darken, desat, soft);
	return 0;
}

// [BB] REAL BEAMS. A segment lit per pixel by distance from it -- continuous
// at any length, wrapping every surface, and lighting what it passes, because
// the shader tests surfaces against the segment itself rather than against a
// sprite standing in for one.
//
// Set the count, then each beam. Beams persist until changed or cleared, so a
// tripwire grid is set once and a weapon beam is re-set as it moves.
// [STAMP] Script's way in to FLevelLocals::SpawnSurfaceStamp. The policy --
// slot choice, eviction, ageing -- lives on the level, not here, because the
// `stamp` CCMD and native gameplay code publish these too.
static void SpawnSurfaceStamp(FLevelLocals *self, int shape, double x, double y, double z,
	double radius, int color, int life, double ax, double ay, double az,
	int tex, double texStrength)
{
	self->SpawnSurfaceStamp(shape, DVector3(x, y, z), radius, (PalEntry)color, life,
		DVector3(ax, ay, az), tex, texStrength);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SpawnSurfaceStamp, SpawnSurfaceStamp)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(shape);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	PARAM_FLOAT(radius);
	PARAM_COLOR(color);
	PARAM_INT(life);
	PARAM_FLOAT(ax); PARAM_FLOAT(ay); PARAM_FLOAT(az);
	PARAM_INT(tex); PARAM_FLOAT(texStrength);
	SpawnSurfaceStamp(self, shape, x, y, z, radius, color, life, ax, ay, az, tex, texStrength);
	return 0;
}

static void ClearSurfaceStamps(FLevelLocals *self)
{
	self->ClearSurfaceStamps();
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearSurfaceStamps, ClearSurfaceStamps)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearSurfaceStamps(self);
	return 0;
}

// [GPUPARTICLES] Script's way in to FLevelLocals::SpawnGpuParticles. Policy --
// jitter, eviction, space conversion -- lives on the level so native code can
// publish particles too.
static void SpawnGpuParticles(FLevelLocals *self, double px, double py, double pz,
	double dx, double dy, double dz, int count, double spread, double speed, double speedJitter,
	int color, double intensity, double life, double lifeJitter,
	double sizeStart, double sizeEnd, double gravity, double drag,
	int orient, double stretch, int seed)
{
	const bool firstThisLevel = self->GpuParticleWritten == 0;
	self->SpawnGpuParticles(DVector3(px, py, pz), DVector3(dx, dy, dz), count,
		spread, speed, speedJitter, (PalEntry)color, intensity, life, lifeJitter,
		sizeStart, sizeEnd, gravity, drag, orient, stretch, seed);

	// Diagnostic: one line per level, the first time its ring is written.
	if (firstThisLevel && self->GpuParticleWritten > 0)
	{
		Printf("GpuParticles: first write this level -- serial %llu, ring %u records, burst of %d\n",
			(unsigned long long)self->GpuParticleSerial, self->GpuParticles.Size(), count);
	}
}

// _NATIVE0, NOT _NATIVE. This is 22 VM arguments (self, two Vector3s at three
// floats each, fifteen more). The plain macro registers a direct-call pointer
// the JIT builds from the function signature, asmjit caps that at 16, and past
// the cap the process dies at load with nothing in any log -- see the note on
// SetVolumetricBeam. The VM calling convention has no such limit.
DEFINE_ACTION_FUNCTION_NATIVE0(FLevelLocals, SpawnGpuParticles, SpawnGpuParticles)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(dx); PARAM_FLOAT(dy); PARAM_FLOAT(dz);
	PARAM_INT(count);
	PARAM_FLOAT(spread); PARAM_FLOAT(speed); PARAM_FLOAT(speedJitter);
	PARAM_COLOR(color);
	PARAM_FLOAT(intensity); PARAM_FLOAT(life); PARAM_FLOAT(lifeJitter);
	PARAM_FLOAT(sizeStart); PARAM_FLOAT(sizeEnd);
	PARAM_FLOAT(gravity); PARAM_FLOAT(drag);
	PARAM_INT(orient); PARAM_FLOAT(stretch); PARAM_INT(seed);
	SpawnGpuParticles(self, px, py, pz, dx, dy, dz, count, spread, speed, speedJitter,
		color, intensity, life, lifeJitter, sizeStart, sizeEnd, gravity, drag,
		orient, stretch, seed);
	return 0;
}

static void ClearGpuParticles(FLevelLocals *self)
{
	self->ClearGpuParticles();
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearGpuParticles, ClearGpuParticles)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearGpuParticles(self);
	return 0;
}

// [PARTICLEDEFS] LevelLocals.ParticleDefinition(name): the HANDLE for a particle
// definition name (gamedata/particledefs.h), for script to cache and pass to
// SpawnParticles. NETPLAY: worked out from the name's text alone -- the same number
// on every machine, never 0 or negative -- so it says nothing about whether this
// machine's PARTICLEDEFS loaded that definition, and nothing in the simulation
// can come to depend on a particle through it.
static int ParticleDefinition(FLevelLocals *self, int name)
{
	return ParticleDefinitionHandle(FName(ENamedName(name)).GetChars());
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ParticleDefinition, ParticleDefinition)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_NAME(name);
	ACTION_RETURN_INT(ParticleDefinition(self, name.GetIndex()));
}

// [PARTICLEDEFS] Script's way in to FLevelLocals::SpawnParticles. Returns nothing;
// a handle with no definition on this machine only ever costs this machine pixels.
// [DEBRISPOOL] A debris definition's burst (`restitution`) goes to the renderer's debris pool
// from there ("Engine docs/DEBRIS_9_IMPL_NOTES.md") -- the hook is the definition, not a native.
static void SpawnParticles(FLevelLocals *self, int definition, double px, double py, double pz,
	double dx, double dy, double dz, int count, double spread, double speed, double speedJitter,
	double life, double lifeJitter, int tint, double intensity, double sizeScale, int seed,
	int shape, double spx, double spy, double spz, double snx, double sny, double snz, double floorZ)
{
	self->SpawnParticles(definition, DVector3(px, py, pz), DVector3(dx, dy, dz), count,
		spread, speed, speedJitter, life, lifeJitter, (PalEntry)tint, intensity, sizeScale, seed,
		shape, DVector3(spx, spy, spz), DVector3(snx, sny, snz), floorZ);
}

// _NATIVE0, like SpawnGpuParticles: 26 VM arguments (self, handle, four Vector3s at
// three floats each, and twelve more) are past asmjit's 16-argument direct-call cap,
// which kills the process at load with nothing in any log.
DEFINE_ACTION_FUNCTION_NATIVE0(FLevelLocals, SpawnParticles, SpawnParticles)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(definition);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(dx); PARAM_FLOAT(dy); PARAM_FLOAT(dz);
	PARAM_INT(count);
	PARAM_FLOAT(spread); PARAM_FLOAT(speed); PARAM_FLOAT(speedJitter);
	PARAM_FLOAT(life); PARAM_FLOAT(lifeJitter);
	PARAM_COLOR(tint);
	PARAM_FLOAT(intensity); PARAM_FLOAT(sizeScale);
	PARAM_INT(seed); PARAM_INT(shape);
	PARAM_FLOAT(spx); PARAM_FLOAT(spy); PARAM_FLOAT(spz);
	PARAM_FLOAT(snx); PARAM_FLOAT(sny); PARAM_FLOAT(snz);
	PARAM_FLOAT(floorZ);
	SpawnParticles(self, definition, px, py, pz, dx, dy, dz, count, spread, speed, speedJitter,
		life, lifeJitter, tint, intensity, sizeScale, seed, shape, spx, spy, spz, snx, sny, snz, floorZ);
	return 0;
}

static void SetBeam(FLevelLocals *self, int index,
	double ax, double ay, double az, double bx, double by, double bz,
	double thick, double soft, int color, double intensity)
{
	if (index < 0 || index >= FLevelLocals::MAX_BEAMS) return;
	self->BeamStart[index] = DVector3(ax, ay, az);
	self->BeamEnd[index] = DVector3(bx, by, bz);
	self->BeamThick[index] = thick;
	self->BeamSoft[index] = soft;
	self->BeamColor[index] = (PalEntry)color;
	self->BeamIntensity[index] = intensity;
}

// RS FORK -- anchor a beam's origin to a hand, resolved at draw rate.
// See FLevelLocals::BeamAnchor. 0 clears it back to the stored world point.
static void SetBeamAnchor(FLevelLocals *self, int index, int mode)
{
	if (index < 0 || index >= FLevelLocals::MAX_BEAMS) return;
	self->BeamAnchor[index] = (mode < 0 || mode > 2) ? 0 : mode;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBeamAnchor, SetBeamAnchor)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_INT(mode);
	SetBeamAnchor(self, index, mode);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBeam, SetBeam)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_FLOAT(ax); PARAM_FLOAT(ay); PARAM_FLOAT(az);
	PARAM_FLOAT(bx); PARAM_FLOAT(by); PARAM_FLOAT(bz);
	PARAM_FLOAT(thick); PARAM_FLOAT(soft);
	PARAM_COLOR(color); PARAM_FLOAT(intensity);
	SetBeam(self, index, ax, ay, az, bx, by, bz, thick, soft, color, intensity);
	return 0;
}

static void SetBeamCount(FLevelLocals *self, int count, double glow, double fogScatter)
{
	self->BeamCount = clamp(count, 0, FLevelLocals::MAX_BEAMS);

	// [BEAMLINES] ClaimBeam never hands out a slot below the count, but nothing
	// stops the count growing over one later -- then two callers write the same
	// line. Say so once per map, naming the slot, rather than refuse the count.
	if (!self->BeamCountOverClaimLogged)
	{
		for (int i = 0; i < self->BeamCount; i++)
		{
			if (!self->BeamClaimed[i]) continue;
			self->BeamCountOverClaimLogged = true;
			Printf("Beams: SetBeamCount(%d) now covers claimed slot %d -- a SetBeam caller and the claim's owner "
				"will both write it. Further overlaps on this map are silent.\n", self->BeamCount, i);
			break;
		}
	}

	self->BeamGlow = glow;
	self->BeamFogScatter = fogScatter;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBeamCount, SetBeamCount)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(count); PARAM_FLOAT(glow); PARAM_FLOAT(fogScatter);
	SetBeamCount(self, count, glow, fogScatter);
	return 0;
}

// The beam seen IN THE AIR, and what travels along it.
//
// airGlow 0 means a beam only lights what it touches -- a spotlight, not a
// laser. Everything else here rides the position along the segment, which the
// closest-approach solve already produces, so none of it costs a second pass.
static void SetBeamLook(FLevelLocals *self, double airGlow, double scrollSpeed,
	double scrollDepth, double taper, double flare)
{
	self->BeamAirGlow = airGlow;
	self->BeamScrollSpeed = scrollSpeed;
	self->BeamScrollDepth = scrollDepth;
	self->BeamTaper = taper;
	self->BeamFlare = flare;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBeamLook, SetBeamLook)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(airGlow); PARAM_FLOAT(scrollSpeed);
	PARAM_FLOAT(scrollDepth); PARAM_FLOAT(taper); PARAM_FLOAT(flare);
	SetBeamLook(self, airGlow, scrollSpeed, scrollDepth, taper, flare);
	return 0;
}

static void ClearBeams(FLevelLocals *self)
{
	self->BeamCount = 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearBeams, ClearBeams)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearBeams(self);
	return 0;
}

// [BEAMLINES] A line of your own, independent of BeamCount, and a look per
// line. The policy lives on FLevelLocals (ClaimBeam and neighbours) so native
// code can claim a line too; these add only the script entry and the log.
// See "Engine docs/BEAM_LINES_PLAN.md".
static int ClaimBeam(FLevelLocals *self, AActor *owner)
{
	const int slot = self->ClaimBeam();
	if (slot >= 0)
	{
		self->BeamClaimOwner[slot] = owner;
		self->BeamClaimOwned[slot] = owner != nullptr;
	}
	if (slot < 0 && !self->BeamClaimFullLogged)
	{
		self->BeamClaimFullLogged = true;
		int claimed = 0;
		for (int i = 0; i < FLevelLocals::MAX_BEAMS; i++)
			if (self->BeamClaimed[i]) claimed++;
		Printf("Beams: ClaimBeam found no free slot -- %d claimed, BeamCount %d, %d slots in all. "
			"It returned -1; further refusals on this map are silent.\n",
			claimed, self->BeamCount, FLevelLocals::MAX_BEAMS);
	}
	return slot;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClaimBeam, ClaimBeam)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(owner, AActor);
	ACTION_RETURN_INT(ClaimBeam(self, owner));
}

static void ReleaseBeam(FLevelLocals *self, int index)
{
	self->ReleaseBeam(index);
	if (index >= 0 && index < FLevelLocals::MAX_BEAMS && !self->BeamClaimed[index])
		self->BeamClaimOwner[index] = nullptr;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ReleaseBeam, ReleaseBeam)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	ReleaseBeam(self, index);
	return 0;
}

static int IsBeamClaimed(FLevelLocals *self, int index)
{
	return self->IsBeamClaimed(index);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, IsBeamClaimed, IsBeamClaimed)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	ACTION_RETURN_BOOL(IsBeamClaimed(self, index));
}

static void SetBeamStyle(FLevelLocals *self, int index, double airGlow, double halo, double taper, double flare)
{
	self->SetBeamStyle(index, airGlow, halo, taper, flare);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBeamStyle, SetBeamStyle)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_FLOAT(airGlow); PARAM_FLOAT(halo); PARAM_FLOAT(taper); PARAM_FLOAT(flare);
	SetBeamStyle(self, index, airGlow, halo, taper, flare);
	return 0;
}

static void ClearBeamStyle(FLevelLocals *self, int index)
{
	self->ClearBeamStyle(index);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearBeamStyle, ClearBeamStyle)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	ClearBeamStyle(self, index);
	return 0;
}

// [DRAWNLINES] Many glowing lines -- see FLevelLocals::DrawnLine. The storage
// and its rules live on the level; these are the script entry and two log lines.
extern int DrawnLineCapacity();   // hw_cvars.cpp, a fixed engine number

static void SetDrawnLine(FLevelLocals *self, int index,
	double ax, double ay, double az, double bx, double by, double bz,
	double thick, double soft, int color, double intensity)
{
	const bool first = self->DrawnLines.Size() == 0;
	self->SetDrawnLine(index, DVector3(ax, ay, az), DVector3(bx, by, bz), (PalEntry)color, intensity, thick, soft);

	// Diagnostics: the first write of the run, and the first out-of-range index.
	if (first && self->DrawnLines.Size() > 0)
		Printf("DrawnLines: first SetDrawnLine this run -- %u slots on the level\n", self->DrawnLines.Size());
	if (index < 0 || index >= DrawnLineCapacity())
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			Printf("DrawnLines: SetDrawnLine index %d is outside 0..%d and was ignored; further bad indices are silent\n", index, DrawnLineCapacity() - 1);
		}
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDrawnLine, SetDrawnLine)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_FLOAT(ax); PARAM_FLOAT(ay); PARAM_FLOAT(az);
	PARAM_FLOAT(bx); PARAM_FLOAT(by); PARAM_FLOAT(bz);
	PARAM_FLOAT(thick); PARAM_FLOAT(soft);
	PARAM_COLOR(color); PARAM_FLOAT(intensity);
	SetDrawnLine(self, index, ax, ay, az, bx, by, bz, thick, soft, color, intensity);
	return 0;
}

static void SetDrawnLineLook(FLevelLocals *self, int index, double airGlow, double halo,
	double taper, double flare, double scrollSpeed, double scrollDepth)
{
	self->SetDrawnLineLook(index, airGlow, halo, taper, flare, scrollSpeed, scrollDepth);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDrawnLineLook, SetDrawnLineLook)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_FLOAT(airGlow); PARAM_FLOAT(halo); PARAM_FLOAT(taper); PARAM_FLOAT(flare);
	PARAM_FLOAT(scrollSpeed); PARAM_FLOAT(scrollDepth);
	SetDrawnLineLook(self, index, airGlow, halo, taper, flare, scrollSpeed, scrollDepth);
	return 0;
}

// [F1] The two opt-in looks for a drawn line -- see FLevelLocals::DrawnLine.
// Setters only: no call hands a line's look back, so nothing in play can branch
// on it. Presentation only, no RNG; the noise is computed in drawnlines.fp.
static void SetDrawnLineGradient(FLevelLocals *self, int index, int colorEnd, double swell)
{
	self->SetDrawnLineGradient(index, (PalEntry)colorEnd, swell);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDrawnLineGradient, SetDrawnLineGradient)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_COLOR(colorEnd);
	PARAM_FLOAT(swell);
	SetDrawnLineGradient(self, index, colorEnd, swell);
	return 0;
}

static void SetDrawnLineTurbulence(FLevelLocals *self, int index, double strength, double scale, double speed)
{
	self->SetDrawnLineTurbulence(index, strength, scale, speed);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDrawnLineTurbulence, SetDrawnLineTurbulence)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_FLOAT(strength); PARAM_FLOAT(scale); PARAM_FLOAT(speed);
	SetDrawnLineTurbulence(self, index, strength, scale, speed);
	return 0;
}

static void SetDrawnLineAnchor(FLevelLocals *self, int index, int mode, AActor *owner)
{
	const int playerNum = (owner != nullptr && owner->player != nullptr) ? int(owner->player - players) : -1;
	self->SetDrawnLineAnchor(index, mode, playerNum);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDrawnLineAnchor, SetDrawnLineAnchor)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_INT(mode);
	PARAM_OBJECT(owner, AActor);
	SetDrawnLineAnchor(self, index, mode, owner);
	return 0;
}

// [EFFECTLIGHTS] The light a drawn line throws -- see FLevelLocals::DrawnLine's Light* members. A setter only: nothing hands
// it back. A call with a non-finite number is ignored. 6 VM arguments: under the JIT's direct-call cap of 16.
static void SetDrawnLineLight(FLevelLocals *self, int index, double radius, double intensity, int flags, double endBrightness)
{
	static bool badLogged = false;
	if (!EffectArgsFinite("SetDrawnLineLight", badLogged, { radius, intensity, endBrightness }))
		return;
	self->SetDrawnLineLight(index, radius, intensity, flags, endBrightness);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDrawnLineLight, SetDrawnLineLight)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	PARAM_FLOAT(radius);
	PARAM_FLOAT(intensity);
	PARAM_INT(flags);
	PARAM_FLOAT(endBrightness);
	SetDrawnLineLight(self, index, radius, intensity, flags, endBrightness);
	return 0;
}

static void ClearDrawnLine(FLevelLocals *self, int index)
{
	self->ClearDrawnLine(index);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearDrawnLine, ClearDrawnLine)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(index);
	ClearDrawnLine(self, index);
	return 0;
}

static void ClearDrawnLines(FLevelLocals *self)
{
	self->ClearDrawnLines();
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearDrawnLines, ClearDrawnLines)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearDrawnLines(self);
	return 0;
}

// Named Get... because the global DrawnLineCapacity() would make the function
// pointer this macro takes ambiguous.
static int GetDrawnLineCapacity(FLevelLocals *self)
{
	return DrawnLineCapacity();
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, DrawnLineCapacity, GetDrawnLineCapacity)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ACTION_RETURN_INT(GetDrawnLineCapacity(self));
}

static void ClearFogSlab(FLevelLocals *self)
{
	self->FogSlabActive = false;
	self->FogSlabDensity = 0;
	self->FogSlabWakeStrength = 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearFogSlab, ClearFogSlab)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearFogSlab(self);
	return 0;
}

// [RS fork] A TRANSIENT FOG SLAB THAT WINS OVER THE STANDING ONE.
//
// SetFogSlab is one shared slot, so a temporary mist (the weapon wheel's, while
// it is open) and a standing fog (RS_Fog, re-pushed every tic) replaced each
// other, and the temporary caller's ClearFogSlab wiped the standing fog. The
// override is a second slot the renderer prefers while it is set; the standing
// FogSlab* values are never touched, so ClearFogSlabOverride hands the view
// straight back to them. Same parameters as SetFogSlab plus its own bottom, all
// absolute world Z (the override ignores SetFogFollow, SetFogBottom's stack and
// SetFogSurface). Density <= 0 clears it, as density 0 switches SetFogSlab off.
//
// One override, not a stack: two transient callers at once still share it.
static void ClearFogSlabOverride(FLevelLocals *self)
{
	if (r_visualstate_log && self->FogSlabOverrideActive)
		Printf("ClearFogSlabOverride: the standing fog slab is active again (%s)\n",
			(self->FogSlabActive && self->FogSlabDensity > 0.0) ? "set" : "off, so no slab");
	self->FogSlabOverrideActive = false;
	self->FogSlabOverrideDensity = 0;
}

static void SetFogSlabOverride(FLevelLocals *self, double topZ, double density,
	double softness, double scatter, int color, double bottomZ)
{
	if (density <= 0.0)
	{
		ClearFogSlabOverride(self);
		return;
	}
	if (r_visualstate_log && !self->FogSlabOverrideActive)
		Printf("SetFogSlabOverride: the OVERRIDE fog slab is active (top %.1f density %.2f bottom %.1f), standing slab %s underneath\n",
			topZ, density, bottomZ,
			(self->FogSlabActive && self->FogSlabDensity > 0.0) ? "held" : "off");
	self->FogSlabOverrideActive = true;
	self->FogSlabOverrideTop = topZ;
	self->FogSlabOverrideDensity = density;
	self->FogSlabOverrideSoft = softness;
	self->FogSlabOverrideScatter = scatter;
	self->FogSlabOverrideColor = (PalEntry)color;
	self->FogSlabOverrideBottom = bottomZ;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetFogSlabOverride, SetFogSlabOverride)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(topZ); PARAM_FLOAT(density);
	PARAM_FLOAT(softness); PARAM_FLOAT(scatter);
	PARAM_COLOR(color);
	PARAM_FLOAT(bottomZ);
	SetFogSlabOverride(self, topZ, density, softness, scatter, color, bottomZ);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearFogSlabOverride, ClearFogSlabOverride)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearFogSlabOverride(self);
	return 0;
}

// [BLOOMOVERRIDE] A TRANSIENT BLOOM LOOK OVER THE gl_bloom_* SETTINGS
// (FLevelLocals::BloomOverride*, g_levellocals.h; E1 in
// "Engine docs/REVIEW_BLOOM_PLAN.md").
//
// Setters only. Nothing hands the values, the renderer's eased state or its clock
// back to script, so nothing in play can act on them. The renderer copies the slot
// every frame (HWDrawInfo::ProcessScene) and does the blending, the fade and the
// pulse itself (PPBloom, hw_postprocess.cpp).
//
// SAFETY. The gl_bloom_* CUSTOM_CVAR clamps never see these values, and the bloom
// maths does not survive a bad one: spread 0 makes every blur weight 0/0, and a NaN
// or inf anywhere reaches the additive combine and writes NaN over the scene. So a
// call with any non-finite argument is ignored (the slot keeps what it had), and
// every value is clamped to the matching cvar's range, with an upper bound on
// spread as well (a float inf there empties the weights just the same).
//
// QUIET. No per-call logging: a caller may push every tic. With r_visualstate_log on
// it logs when the override starts, when it is cleared, and the first ignored call.
// Clearing when nothing is set is a no-op.
//
// 12 VM arguments (self and eleven numbers): under the JIT's direct-call cap of 16,
// so the plain _NATIVE macro is safe (see SetHeatSource).
static void ClearBloomOverride(FLevelLocals *self)
{
	if (!self->BloomOverrideActive)
		return;
	if (r_visualstate_log)
		Printf("ClearBloomOverride: bloom follows the gl_bloom_* settings again (the renderer eases back over %.2f s)\n",
			self->BloomOverrideFade);
	self->BloomOverrideActive = false;
}

static void SetBloomOverride(FLevelLocals *self, double spread, double threshold, double knee,
	double tintR, double tintG, double tintB, double mix, double intensity, double fade,
	double pulse, double pulseRate)
{
	const double args[] = { spread, threshold, knee, tintR, tintG, tintB, mix, intensity, fade, pulse, pulseRate };
	for (double v : args)
	{
		if (!std::isfinite(v))
		{
			static bool warned = false;
			if (r_visualstate_log && !warned)
			{
				Printf("SetBloomOverride: ignored a call with a non-finite argument (spread %g threshold %g knee %g tint %g %g %g mix %g intensity %g fade %g pulse %g rate %g); the override keeps its last values. Logged once.\n",
					spread, threshold, knee, tintR, tintG, tintB, mix, intensity, fade, pulse, pulseRate);
				warned = true;
			}
			return;
		}
	}

	const bool wasActive = self->BloomOverrideActive;
	self->BloomOverrideActive = true;
	self->BloomOverrideSpread = clamp(spread, 0.1, 100.0);
	self->BloomOverrideThreshold = clamp(threshold, 0.05, 4.0);
	self->BloomOverrideKnee = clamp(knee, 0.0, 8.0);
	self->BloomOverrideTintR = clamp(tintR, 0.0, 16.0);
	self->BloomOverrideTintG = clamp(tintG, 0.0, 16.0);
	self->BloomOverrideTintB = clamp(tintB, 0.0, 16.0);
	self->BloomOverrideMix = clamp(mix, 0.0, 1.0);
	self->BloomOverrideIntensity = clamp(intensity, 0.0, 16.0);
	self->BloomOverrideFade = clamp(fade, 0.0, 10.0);
	self->BloomOverridePulse = clamp(pulse, 0.0, 1.0);
	self->BloomOverridePulseRate = clamp(pulseRate, 0.0, 20.0);

	if (r_visualstate_log && !wasActive)
		Printf("SetBloomOverride: the bloom override is active (spread %.2f threshold %.2f knee %.2f tint %.2f %.2f %.2f mix %.2f intensity %.2f fade %.2f pulse %.2f rate %.2f)\n",
			self->BloomOverrideSpread, self->BloomOverrideThreshold, self->BloomOverrideKnee,
			self->BloomOverrideTintR, self->BloomOverrideTintG, self->BloomOverrideTintB,
			self->BloomOverrideMix, self->BloomOverrideIntensity, self->BloomOverrideFade,
			self->BloomOverridePulse, self->BloomOverridePulseRate);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetBloomOverride, SetBloomOverride)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(spread); PARAM_FLOAT(threshold); PARAM_FLOAT(knee);
	PARAM_FLOAT(tintR); PARAM_FLOAT(tintG); PARAM_FLOAT(tintB);
	PARAM_FLOAT(mix);
	PARAM_FLOAT(intensity);
	PARAM_FLOAT(fade);
	PARAM_FLOAT(pulse);
	PARAM_FLOAT(pulseRate);
	SetBloomOverride(self, spread, threshold, knee, tintR, tintG, tintB, mix, intensity, fade, pulse, pulseRate);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearBloomOverride, ClearBloomOverride)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearBloomOverride(self);
	return 0;
}

static void ClearDarkness(FLevelLocals *self)
{
	self->DarkMode = 0;
	self->DarkDistDepth = 0;
	self->DarkHeightDepth = 0;
	self->DarkHeightFollow = 0;   // [round2 B4] back to the absolute reference
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearDarkness, ClearDarkness)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearDarkness(self);
	return 0;
}

// [round2 B4] WHERE THE DARKNESS HEIGHT REFERENCE COMES FROM.
//   mode 0  SetDarknessSpace's heightRef as written (the old behaviour)
//   mode 1  the viewer's feet at draw rate -- the camera's interpolated Z --
//           plus offset, so the pool edge rides lifts and stairs smoothly
//           instead of stepping with a 35Hz WorldTick write
// Anything else is 0. Look-only, so clearscope.
static void SetDarknessHeightFollow(FLevelLocals *self, int mode, double offset)
{
	self->DarkHeightFollow = (mode == 1) ? 1 : 0;
	self->DarkHeightOffset = offset;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetDarknessHeightFollow, SetDarknessHeightFollow)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(mode); PARAM_FLOAT(offset);
	SetDarknessHeightFollow(self, mode, offset);
	return 0;
}

static void ClearSweep(FLevelLocals *self)
{
	self->SweepMode = 0;
	self->SweepCount = 0;
	self->SweepTrail = 0;
	for (int i = 0; i < FLevelLocals::MAX_SWEEP_BANDS; i++) { self->SweepBandMode[i] = 0; self->SweepBandDraw[i] = 0; }

	// [round2 A5] Fill, air and the room box too. hw_drawinfo.cpp re-applies
	// any band whose SweepBandFill is above 0, so a cleared sweep used to hand
	// the next caller a grid or a solid slab it never asked for. This is the
	// same set ClearLevelData (p_setup.cpp) resets, so the two clears agree.
	// [round2 B2] The passed bits too; the passed look settings stay, as
	// ClearLevelData leaves look settings alone.
	for (int i = 0; i < FLevelLocals::MAX_SWEEP_BANDS; i++) { self->SweepBandFill[i] = 0; self->SweepBandPassed[i] = 0; }
	self->SweepFillAir = 0;
	self->SweepRoomSoft = 0;
	self->SweepRoomMin = self->SweepRoomMax = DVector3(0, 0, 0);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ClearSweep, ClearSweep)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ClearSweep(self);
	return 0;
}

// [BB] Can this billboard answer a query at all?
//
// Transients carry no handle, so a hit could not be reported even if it
// happened. BBFL_NOHIT is decoration the caller has declared unclickable --
// the letters painted on a panel, a bar's track -- and without it the nearest
// hit is always the text rather than the panel it is written on.
static inline bool BillboardHittable(FLevelLocals *self, const FBillboard &bb)
{
	if (bb.id == 0 || (bb.flags & BBFL_NOHIT) != 0) return false;

	// [BB] A group collapsed to zero is the documented way to hide a panel, and
	// the renderer drops those before they are ever submitted ("if (gscale <=
	// 0.0) continue"). The queries had no equivalent, and BillboardBasis floors
	// the scale at 0.01 rather than zero -- so a hidden panel kept a tiny,
	// invisible hit box sitting at its full-size offset, and could win a hit
	// against the visible panel in front of it.
	if (bb.group && self->BillboardGroupScale(bb.group, 1.0) <= 0.0) return false;

	return true;
}

// [BB] The extent multiplier a query must use, matching the renderer.
//
// bb_scale is a comfort dial that changes what is DRAWN, so a query ignoring
// it puts the clickable region somewhere other than the picture -- that was a
// real defect once and the comment on BillboardBasis records it. A group
// scale is the same hazard with a second cause: a panel caught mid-grow draws
// at 60% and would answer at 100%, so its edges would be live in empty air.
//
// ticFrac 1.0 rather than the renderer's fraction, because a query runs in
// the playsim where the tic is over. Half a tic of disagreement during an
// animation is not worth a viewpoint dependency here, and every group this
// engine has is settled at 1.0 by the time anything is meant to be aimed at.
static inline double BillboardQueryScale(FLevelLocals *self, const FBillboard &bb)
{
	if (!bb.group) return bb_scale;
	return bb_scale * self->BillboardGroupScale(bb.group, 1.0);
}

// The orientation half of the same obligation. A view-locked billboard's yaw
// is relative to the view, so a query that solved it in world space would put
// the clickable face at a different angle from the drawn one -- the panel
// would turn with your head and its hit region would not.
static inline double BillboardQueryYawBias(const FBillboard &bb)
{
	if (!(bb.flags & BBFL_VIEWLOCKED)) return 0.0;
	return r_viewpoint.Angles.Yaw.Degrees() + 180.0;	// see the renderer's note
}

// [BB] Ray versus billboard. Returns the id of the nearest billboard the ray
// crosses and where on its face it landed, as 0..1 across and down -- so a
// caller gets back the same UV the shader sees and can decide what was
// clicked without knowing anything about world geometry.
//
// Camera-facing billboards are tested against the orientation they would be
// drawn with, using the supplied ray origin as the eye. A pointer and a
// viewpoint are not the same thing in VR, but they are the same thing often
// enough that testing against the drawn orientation is what a player expects.
//
// Returns 0 and (0,0) on a miss.
static int AimBillboard(FLevelLocals *self, double sx, double sy, double sz,
	double dx, double dy, double dz, double maxDist, DVector2 *uvOut)
{
	DVector3 start(sx, sy, sz);
	DVector3 dir(dx, dy, dz);

	double dlen = dir.Length();
	if (dlen <= 0.0) { if (uvOut) *uvOut = DVector2(0, 0); return 0; }
	dir /= dlen;

	int bestID = 0;
	double bestT = maxDist > 0.0 ? maxDist : FLT_MAX;
	DVector2 bestUV(0, 0);

	for (auto &bb : self->Billboards)
	{
		if (!BillboardHittable(self, bb)) continue;

		const DVector3 bpos = BillboardWorldPos(self, bb);

		// Same solver, same cvars, as the renderer. See BillboardBasis.
		DVector3 right, up, normal;
		double halfw, halfh;
		BillboardBasis(bb, bpos, start, bb_tiltbias, BillboardQueryScale(self, bb), right, up, normal, halfw, halfh, BillboardQueryYawBias(bb));

		double denom = normal | dir;
		if (fabs(denom) < EQUAL_EPSILON) continue;	// parallel to the face

		double t = (normal | (bpos - start)) / denom;
		if (t <= 0.0 || t >= bestT) continue;		// behind the ray, or farther than a hit we already have

		DVector3 rel = (start + dir * t) - bpos;

		double across = rel | right;
		double down = rel | up;
		if (fabs(across) > halfw || fabs(down) > halfh) continue;	// outside the quad

		bestT = t;
		bestID = bb.id;
		// Match the drawn UV: u runs left to right, v runs top to bottom.
		bestUV = DVector2((across + halfw) / (halfw * 2.0), (halfh - down) / (halfh * 2.0));
	}

	if (uvOut) *uvOut = bestUV;
	return bestID;
}

// [BB] Point versus billboard -- the touch case. Same geometry as the aim
// ray, but tested from a position rather than along a direction, so a tracked
// fingertip or controller tip can drive a panel directly.
//
// Returns the nearest billboard whose face the point sits within maxRange of
// AND within the bounds of, along with the same 0..1 UV the aim ray reports
// and the distance to the surface. Distance is the useful part: hover can
// track it as a hand approaches and the press can fire on contact, which is
// what makes touch feel like touch rather than a switch.
//
// Camera-facing billboards resolve their orientation against the touching
// point, matching how AimBillboard treats its ray origin.
//
// Returns 0 on a miss.
static int TouchBillboard(FLevelLocals *self, double px, double py, double pz,
	double maxRange, DVector2 *uvOut, double *distOut)
{
	DVector3 p(px, py, pz);

	int bestID = 0;
	double bestDist = maxRange > 0.0 ? maxRange : FLT_MAX;
	DVector2 bestUV(0, 0);

	for (auto &bb : self->Billboards)
	{
		if (!BillboardHittable(self, bb)) continue;

		const DVector3 bpos = BillboardWorldPos(self, bb);

		DVector3 right, up, normal;
		double halfw, halfh;
		BillboardBasis(bb, bpos, p, bb_tiltbias, BillboardQueryScale(self, bb), right, up, normal, halfw, halfh, BillboardQueryYawBias(bb));

		DVector3 rel = p - bpos;

		// Distance to the plane, unsigned: a finger behind a panel is still
		// touching it as far as the player is concerned.
		double dist = fabs(normal | rel);
		if (dist >= bestDist) continue;

		double across = rel | right;
		double down = rel | up;
		if (fabs(across) > halfw || fabs(down) > halfh) continue;

		bestDist = dist;
		bestID = bb.id;
		bestUV = DVector2((across + halfw) / (halfw * 2.0), (halfh - down) / (halfh * 2.0));
	}

	if (uvOut) *uvOut = bestUV;
	if (distOut) *distOut = (bestID != 0) ? bestDist : 0.0;
	return bestID;
}

// [BB] Swept touch -- did this hand cross into a panel between one tic and the
// next, and where did it land?
//
// TouchBillboard answers "is the hand in the panel RIGHT NOW", and that
// question has a hole in it that only shows up on the most emphatic gesture a
// player can make. Script runs at 35Hz. A panel's touch slab is a few map
// units thick. A deliberate jab moves a controller several units per tic, so
// the hand can be in front of the panel on one tic and behind it on the next
// and never once be inside it: reach out gently and it works, punch it and
// nothing happens. Sampling harder is not available to script, and thickening
// the slab to cover the fastest possible hand makes a panel you cannot stand
// near without pressing.
//
// So the segment is the primitive, not the point. `from` and `to` are where
// the hand was and where it is; the answer is the FIRST billboard the path
// touches, the UV where it touched, and how far along the segment that
// happened as a 0..1 fraction. Testing the whole path also makes the enter
// event fall out for free -- a caller that wants "the hand arrived this tic"
// no longer has to infer it from two containment tests and hope it saw both.
//
// `radius` inflates the quad into a slab of that thickness and pads its edges,
// which is what a fingertip or a controller tip actually is. 0 is the exact
// plane.
//
// Deliberately NOT a debounce, a cooldown, or a hysteresis band: which hand
// did it, whether it counts twice, and what a press means are all policy, and
// policy belongs to whoever owns the panel. This reports geometry.
//
// Returns 0 on a miss, with frac 0.
static int SweepBillboard(FLevelLocals *self, double fx, double fy, double fz,
	double tx, double ty, double tz, double radius, DVector2 *uvOut, double *fracOut)
{
	DVector3 from(fx, fy, fz);
	DVector3 to(tx, ty, tz);
	DVector3 seg = to - from;

	if (radius < 0.0) radius = 0.0;

	int bestID = 0;
	double bestFrac = 1.0e30;
	DVector2 bestUV(0, 0);

	for (auto &bb : self->Billboards)
	{
		if (!BillboardHittable(self, bb)) continue;

		const DVector3 bpos = BillboardWorldPos(self, bb);

		// Resolved against where the hand ENDED, so a camera-facing panel is
		// tested against the orientation it holds now rather than one it has
		// already turned away from.
		DVector3 right, up, normal;
		double halfw, halfh;
		BillboardBasis(bb, bpos, to, bb_tiltbias, BillboardQueryScale(self, bb), right, up, normal, halfw, halfh, BillboardQueryYawBias(bb));

		// Signed distance off the face at each end of the sweep.
		double d0 = normal | (from - bpos);
		double d1 = normal | (to - bpos);

		// When did the path first come within `radius` of the plane? Already
		// there at the start is frac 0 -- a hand resting on a panel has
		// touched it, and it is the caller's business whether that repeats.
		double frac;
		if (fabs(d0) <= radius)
		{
			frac = 0.0;
		}
		else
		{
			// Aim for the near face of the slab, on the side the hand
			// started, so a sweep that passes clean through still reports
			// where it went IN rather than where it came out.
			double target = (d0 > 0.0) ? radius : -radius;
			double denom = d1 - d0;
			if (fabs(denom) < EQUAL_EPSILON) continue;	// travelling parallel to the face

			frac = (target - d0) / denom;
			if (frac < 0.0 || frac > 1.0) continue;		// never reached it this tic
		}

		if (frac >= bestFrac) continue;					// something else was touched sooner

		DVector3 rel = (from + seg * frac) - bpos;
		double across = rel | right;
		double down = rel | up;
		if (fabs(across) > halfw + radius || fabs(down) > halfh + radius) continue;

		bestFrac = frac;
		bestID = bb.id;
		// Clamped, because the radius pad lets a hit land just outside the
		// face and a caller mapping UV to a row must never see 1.02.
		bestUV = DVector2(
			clamp((across + halfw) / (halfw * 2.0), 0.0, 1.0),
			clamp((halfh - down) / (halfh * 2.0), 0.0, 1.0));
	}

	if (uvOut) *uvOut = bestUV;
	if (fracOut) *fracOut = (bestID != 0) ? bestFrac : 0.0;
	return bestID;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SweepBillboard, SweepBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(fx); PARAM_FLOAT(fy); PARAM_FLOAT(fz);
	PARAM_FLOAT(tx); PARAM_FLOAT(ty); PARAM_FLOAT(tz);
	PARAM_FLOAT(radius);
	DVector2 uv;
	double frac = 0.0;
	int id = SweepBillboard(self, fx, fy, fz, tx, ty, tz, radius, &uv, &frac);
	if (numret > 0) ret[0].SetInt(id);
	if (numret > 1) ret[1].SetVector2(uv);
	if (numret > 2) ret[2].SetFloat(frac);
	return min(numret, 3);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, TouchBillboard, TouchBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(px); PARAM_FLOAT(py); PARAM_FLOAT(pz);
	PARAM_FLOAT(maxRange);
	DVector2 uv;
	double dist = 0.0;
	int id = TouchBillboard(self, px, py, pz, maxRange, &uv, &dist);
	if (numret > 0) ret[0].SetInt(id);
	if (numret > 1) ret[1].SetVector2(uv);
	if (numret > 2) ret[2].SetFloat(dist);
	return min(numret, 3);
}

// [BB] Let a mod claim the VR sticks while its own selector is open.
//
// Snap turn and stick movement are handled deep in the VR input path, long
// before any script sees a button, so a mod with an in-world menu could not stop
// the same thumbstick from spinning and walking the player while it was being
// used to choose something. The native wheel has had this since it was written
// -- it just had no way out to ZScript.
//
// Set on open, clear on close. It is not a cvar on purpose: transient state that
// survived a crash would leave someone unable to turn with nothing to blame.
static void SuppressVRInput(FLevelLocals *self, int suppressed)
{
	VR_SetScriptInputSuppressed(suppressed != 0);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SuppressVRInput, SuppressVRInput)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_BOOL(suppressed);
	SuppressVRInput(self, suppressed);
	return 0;
}

// [BB] The raw locomotion stick, for a mod that just suppressed it above.
//
// SuppressVRInput stops the stick from walking or turning the player by
// zeroing it at the exact point g_game.cpp would otherwise have fed it into
// the ticcmd -- which also zeroes the ONE channel script had for reading
// stick deflection at all (cmd.sidemove/forwardmove). A mod that wants to
// suppress movement AND still read "which way is it pushed" (a wheel doing
// stick-select) had no way to have both.
//
// g_wheelStickForward/Side are filled unconditionally, every VR tic, right
// where g_game.cpp already calls VR_GetMove() for the locomotion stick --
// before the suppression check, so this reads the real deflection whether
// or not the caller is currently suppressing it.
extern float g_wheelStickForward, g_wheelStickSide;

static void GetRawStickMove(FLevelLocals *self, DVector2 *result)
{
	*result = DVector2(g_wheelStickForward, g_wheelStickSide);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetRawStickMove, GetRawStickMove)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	DVector2 result;
	GetRawStickMove(self, &result);
	ACTION_RETURN_VEC2(result);
}

// [BB] Let a mod turn the laser sight on for as long as its own menu is open.
//
// vr_laser_sight and vr_laser_beam are CVAR_ARCHIVE|CVAR_GLOBALCONFIG, and the
// VM refuses to let script write those outside menu code -- correctly, since a
// mod quietly rewriting someone's saved settings is exactly what that rule is
// for. But a script-side in-world menu wants the laser for the duration and
// then wants it back how it was, which is not a settings change at all.
//
// So: an override the renderer consults, layered ON TOP of the cvars without
// touching them. Nothing is written, nothing is archived, and the player's own
// preference is still theirs the moment the override is dropped.
//
// hand is -1 for both, 0 for the main hand, 1 for the off hand. A menu worn on
// one hand only wants a cursor on that hand; forcing both drew a second beam
// from the hand still holding a gun.
static void ForceVRLaser(FLevelLocals *self, int on, int hand)
{
	VR_SetScriptLaserForced(on != 0, hand);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, ForceVRLaser, ForceVRLaser)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_BOOL(on);
	PARAM_INT(hand);
	ForceVRLaser(self, on, hand);
	return 0;
}

// [BB] Terminate the laser at something only script knows about.
//
// The engine's trace sees level geometry and actors; a billboard panel is
// neither, so a laser aimed at one passes through and lands on the wall behind.
// Script has already hit-tested and knows the distance, so it publishes it
// rather than teaching the renderer about billboards.
static void SetVRLaserRange(FLevelLocals *self, double range)
{
	VR_SetScriptLaserRange(range);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SetVRLaserRange, SetVRLaserRange)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(range);
	SetVRLaserRange(self, range);
	return 0;
}

// [BB] Buzz a controller from script.
//
// The engine has full per-hand OpenXR haptics -- bound for Touch, Index, Vive
// and the simple profile -- and script had no way to reach any of it. An
// in-world menu without haptics is a picture of a menu; one tick per hover is
// the difference between reading a ring and feeling one.
//
// VR_HapticEvent, which the playsim already calls in a dozen places, is an
// empty stub here, so it was not an option to route through.
static void VRHaptic(FLevelLocals *self, int hand, double intensity, double durationMs)
{
	VR_ScriptHaptic(hand, intensity, durationMs);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, VRHaptic, VRHaptic)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(hand);
	PARAM_FLOAT(intensity);
	PARAM_FLOAT(durationMs);
	VRHaptic(self, hand, intensity, durationMs);
	return 0;
}

// ---------------------------------------------------------------------------
// JSON PROFILES -- named key/double documents, saved under the same writable
// directory doomxr.ini itself lives in, in an "rs_profiles" subfolder.
// ZScript has no file I/O and no JSON parser of its own -- everywhere else in
// this codebase that reads or writes JSON does it in C++ via rapidjson
// (already vendored for savegames; see serializer.cpp), so this is that same
// approach exposed for script's own use rather than a second, ad-hoc,
// hand-rolled parser living in ZScript string-scanning code.
//
// SHAPE: a flat map of string key -> double. No nesting, no arrays, no other
// value types -- deliberately, because a flat map needs no schema
// negotiation between script and native code. A caller with different data
// just picks different key names ("hip_left_pitch", "seat_height", whatever);
// it does not need new natives.
//
// PROTOCOL, because ZScript cannot pass a whole array or struct into a
// native call (see the model-orientation natives above for the same
// constraint): the caller builds up one profile with a sequence of
// JSONProfileSetDouble calls against an in-progress buffer, then
// JSONProfileSave flushes it to disk. Loading is the mirror: JSONProfileLoad
// parses the file into that buffer, then the caller pulls values back out
// with JSONProfileGetDouble. Exactly one profile is ever "in progress" at a
// time -- the VM is single-threaded and non-reentrant here, so one static
// buffer is enough and avoids a handle/token system for no real benefit.
//
// NAME SANITIZATION is load-bearing, not decoration: this writes to a real
// path on disk from a name a mod controls, in principle any mod, not just
// this one. A name must never be able to walk outside rs_profiles/ or
// collide with an unrelated file. Anything outside [A-Za-z0-9_-], empty, or
// over 64 characters is refused outright rather than silently truncated or
// substituted -- a caller that gets false back knows its name did not
// resolve to a file; a caller that got a silently mangled name would not.
static TMap<FString, double> JSONProfileBuffer;

static bool SanitizeProfileName(const FString &name, FString &outClean)
{
	if (name.IsEmpty() || name.Len() > 64)
		return false;

	for (int i = 0; i < name.Len(); ++i)
	{
		char c = name[i];
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		          (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok)
			return false;
	}

	outClean = name;
	return true;
}

// M_GetConfigPath(false) specifically -- for_reading=false takes that
// function's early "!for_reading || FileExists(path)" return unconditionally,
// before it ever reaches the old-config migration branch (which can prompt
// the user with a dialog). false is what makes this call side-effect-free.
static FString ProfileDirectory()
{
	FString cfg = M_GetConfigPath(false);
	ptrdiff_t slash = cfg.LastIndexOfAny("/\\");
	FString dir = (slash >= 0) ? cfg.Left((long)slash) : cfg;
	dir += "/rs_profiles";
	CreatePath(dir.GetChars());
	return dir;
}

static FString ProfilePath(const FString &cleanName)
{
	FString p = ProfileDirectory();
	p += "/";
	p += cleanName;
	p += ".json";
	return p;
}

static void JSONProfileBegin(FLevelLocals*)
{
	JSONProfileBuffer.Clear();
}

static void JSONProfileSetDouble(FLevelLocals*, const FString &key, double value)
{
	JSONProfileBuffer.Insert(key, value);
}

static double JSONProfileGetDouble(FLevelLocals*, const FString &key, double defaultValue)
{
	auto val = JSONProfileBuffer.CheckKey(key);
	return val ? *val : defaultValue;
}

static int JSONProfileSave(FLevelLocals*, const FString &name)
{
	FString clean;
	if (!SanitizeProfileName(name, clean))
		return 0;

	rapidjson::StringBuffer buffer;
	rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	TMap<FString, double>::Iterator it(JSONProfileBuffer);
	TMap<FString, double>::Pair *pair;
	while (it.NextPair(pair))
	{
		writer.Key(pair->Key.GetChars());
		writer.Double(pair->Value);
	}
	writer.EndObject();

	std::ofstream f(ProfilePath(clean).GetChars(), std::ios::binary | std::ios::trunc);
	if (!f.is_open())
		return 0;
	f.write(buffer.GetString(), (std::streamsize)buffer.GetSize());
	return f.good() ? 1 : 0;
}

static int JSONProfileLoad(FLevelLocals*, const FString &name)
{
	JSONProfileBuffer.Clear();

	FString clean;
	if (!SanitizeProfileName(name, clean))
		return 0;

	std::ifstream f(ProfilePath(clean).GetChars(), std::ios::binary);
	if (!f.is_open())
		return 0;

	std::stringstream ss;
	ss << f.rdbuf();
	std::string contents = ss.str();

	rapidjson::Document doc;
	if (doc.Parse(contents.c_str()).HasParseError() || !doc.IsObject())
		return 0;

	for (auto mit = doc.MemberBegin(); mit != doc.MemberEnd(); ++mit)
	{
		if (mit->value.IsNumber())
			JSONProfileBuffer.Insert(FString(mit->name.GetString()), mit->value.GetDouble());
	}
	return 1;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, JSONProfileBegin, JSONProfileBegin)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	JSONProfileBegin(self);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, JSONProfileSetDouble, JSONProfileSetDouble)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_STRING(key);
	PARAM_FLOAT(value);
	JSONProfileSetDouble(self, key, value);
	return 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, JSONProfileGetDouble, JSONProfileGetDouble)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_STRING(key);
	PARAM_FLOAT(defaultValue);
	ACTION_RETURN_FLOAT(JSONProfileGetDouble(self, key, defaultValue));
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, JSONProfileSave, JSONProfileSave)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_STRING(name);
	ACTION_RETURN_BOOL(JSONProfileSave(self, name) != 0);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, JSONProfileLoad, JSONProfileLoad)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_STRING(name);
	ACTION_RETURN_BOOL(JSONProfileLoad(self, name) != 0);
}

// [BB] FIELD REFLECTION -- read another mod's data without linking to it.
//
// ZScript can only reach a field through a TYPED reference, and a typed
// reference needs the class at COMPILE time -- so a mod that wants to describe
// another mod's weapon has to hard-depend on it, which for an informational
// consumer is exactly the wrong shape. A weapon-select panel that shows tier,
// rarity and affixes wants to work with DoomRL Arsenal, LegenDoom, Doomablo and
// mods not written yet, none of which will declare an interface for it and none
// of which it can afford to require.
//
// Service (service.zs) solves the cooperating case and only that case. Nothing
// already released is going to publish one.
//
// The VM knows all of this already: PClass::Fields lists every field of every
// class in every loaded mod, and PField carries its Offset, Type and Flags.
// None of it was exposed. This opens a door onto data the VM maintains anyway.
//
// READ ONLY, permanently. There is deliberately no SetField: writing into
// another mod's private state puts the corruption and the crash in different
// mods and leaves the culprit no trace. Reading cannot corrupt anything.
static PField *WR_ResolveField(DObject *o, const FString &name)
{
	if (o == nullptr) return nullptr;

	// searchparents, so a subclass reports the fields it inherits. Without it
	// every caller would need to know the other mod's class hierarchy to find
	// Weapon's own members -- which is the knowledge this exists to remove.
	PSymbol *sym = o->GetClass()->FindSymbol(FName(name.GetChars()), true);
	PField *f = dyn_cast<PField>(sym);
	if (f == nullptr) return nullptr;

	// Private: the declaring mod said no. Meta/Static are class data, not
	// instance data -- reading them at an instance offset is meaningless.
	// VARF_ReadOnly is deliberately ALLOWED: it means "may not be written",
	// and nothing here writes.
	if (f->Flags & (VARF_Private | VARF_Meta | VARF_Static)) return nullptr;
	return f;
}

// Every getter returns false rather than a value when it cannot answer, and
// leaves the out parameter untouched. False is "I could not answer", never
// "the answer is zero" -- a caller has to be able to tell an absent stat from
// one that is genuinely 0, because those render differently and conflating
// them is how a panel starts lying.
DEFINE_ACTION_FUNCTION(FLevelLocals, HasField)
{
	// SELF_STRUCT, not bare PROLOGUE. These are declared as methods on
	// LevelLocals rather than as statics, so the VM passes self as parameter
	// zero -- a bare PARAM_PROLOGUE does not consume it and every argument
	// after shifts by one, which reads the level itself as the target object.
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(o, DObject);
	PARAM_STRING(name);
	ACTION_RETURN_BOOL(WR_ResolveField(o, name) != nullptr);
}

DEFINE_ACTION_FUNCTION(FLevelLocals, GetFieldInt)
{
	// SELF_STRUCT, not bare PROLOGUE. These are declared as methods on
	// LevelLocals rather than as statics, so the VM passes self as parameter
	// zero -- a bare PARAM_PROLOGUE does not consume it and every argument
	// after shifts by one, which reads the level itself as the target object.
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(o, DObject);
	PARAM_STRING(name);
	PARAM_OUTPOINTER(out, int);

	PField *f = WR_ResolveField(o, name);
	// Type-checked, not reinterpreted. Offset is a raw byte offset, so reading
	// an int32 field through the wrong pointer type is garbage or a crash
	// rather than a wrong answer.
	if (f == nullptr || (f->Type != TypeSInt32 && f->Type != TypeUInt32))
		ACTION_RETURN_BOOL(false);

	if (out) *out = *(int *)((uint8_t *)o + f->Offset);
	ACTION_RETURN_BOOL(true);
}

// ARRAY-ELEMENT REFLECTION. GetFieldInt answers "what is field X", which is
// silently wrong for a fixed DECORATE/ZScript array field -- `int
// user_equippedDamage[N];` is ONE field whose Type is a PArray, not N
// fields, and the plain scalar path above type-checks against
// TypeSInt32/TypeUInt32 and correctly refuses it. Two real mods hit this in
// the same session: Doomablo's currentStats[totalStatsCount] and
// BorderDoom's user_equipped*[MAX_EQUIPPED_ITEMS] family -- both plain,
// safe-to-read data with no way in.
//
// Bounds-checked against the array's OWN ElementCount, not trusted from the
// caller: a caller guessing at another mod's array size and reading past
// its end would read into whatever field happens to sit next in memory,
// which is a wrong answer with no error rather than the honest "false"
// every other getter here already returns for "cannot answer".
DEFINE_ACTION_FUNCTION(FLevelLocals, GetFieldIntArray)
{
	// SELF_STRUCT, not bare PROLOGUE. These are declared as methods on
	// LevelLocals rather than as statics, so the VM passes self as parameter
	// zero -- a bare PARAM_PROLOGUE does not consume it and every argument
	// after shifts by one, which reads the level itself as the target object.
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(o, DObject);
	PARAM_STRING(name);
	PARAM_INT(index);
	PARAM_OUTPOINTER(out, int);

	PField *f = WR_ResolveField(o, name);
	if (f == nullptr) ACTION_RETURN_BOOL(false);

	// PType's own hierarchy uses isArray()/static_cast, not dyn_cast --
	// that overload set is for DObject's runtime-object hierarchy, a
	// different type system than the reflection PType hierarchy this
	// walks. Confirmed against codegen.cpp's own array-index handling
	// (e.g. FxArrayElement::Emit), which does exactly this.
	if (f->Type == nullptr || !f->Type->isArray()) ACTION_RETURN_BOOL(false);
	PArray *arr = static_cast<PArray *>(f->Type);
	if (index < 0 || (unsigned)index >= arr->ElementCount) ACTION_RETURN_BOOL(false);

	// Same type discipline as the scalar getter: the element type is
	// checked, not assumed, so an array of something else (a struct array,
	// a string array) fails clean instead of reinterpreting its bytes as
	// an int.
	if (arr->ElementType != TypeSInt32 && arr->ElementType != TypeUInt32)
		ACTION_RETURN_BOOL(false);

	uint8_t *base = (uint8_t *)o + f->Offset + (size_t)index * arr->ElementSize;
	if (out) *out = *(int *)base;
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(FLevelLocals, GetFieldFloat)
{
	// SELF_STRUCT, not bare PROLOGUE. These are declared as methods on
	// LevelLocals rather than as statics, so the VM passes self as parameter
	// zero -- a bare PARAM_PROLOGUE does not consume it and every argument
	// after shifts by one, which reads the level itself as the target object.
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(o, DObject);
	PARAM_STRING(name);
	PARAM_OUTPOINTER(out, double);

	PField *f = WR_ResolveField(o, name);
	if (f == nullptr) ACTION_RETURN_BOOL(false);

	// WIDENING is the one permitted courtesy -- every int and every float32
	// survives the trip into a double. Narrowing is not offered anywhere: a
	// double read as an int silently discards, and a sheet quietly showing 3
	// for 3.7 is worse than one showing nothing at all.
	uint8_t *base = (uint8_t *)o + f->Offset;
	if      (f->Type == TypeFloat64) { if (out) *out = *(double *)base; }
	else if (f->Type == TypeFloat32) { if (out) *out = *(float *)base; }
	else if (f->Type == TypeSInt32)  { if (out) *out = *(int32_t *)base; }
	else if (f->Type == TypeUInt32)  { if (out) *out = *(uint32_t *)base; }
	else ACTION_RETURN_BOOL(false);

	ACTION_RETURN_BOOL(true);
}

// Bools are their own getter, not folded into GetFieldInt. TypeBool derives
// from PInt but is a distinct singleton, so the integer path never catches one
// by accident -- and a caller reaching for GetFieldInt on a flag has usually
// misunderstood what it is reading. FieldAt reports "bool" so it can tell.
//
// TWO STORAGE SHAPES, and missing the second one is what makes flags look
// absent. A standalone bool field is a whole byte (PBool::GetValueInt is
// `*(bool *)addr`, types.cpp:806). A flagdef -- every +WEAPON.OFFHANDWEAPON,
// every actor flag -- is a BIT packed into a shared byte, which codegen reads
// with OP_LBIT against `1 << BitValue` (codegen.cpp:7389). BitValue is -1 for
// the first shape and the bit index for the second.
DEFINE_ACTION_FUNCTION(FLevelLocals, GetFieldBool)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(o, DObject);
	PARAM_STRING(name);
	PARAM_OUTPOINTER(out, int);

	PField *f = WR_ResolveField(o, name);
	if (f == nullptr || f->Type != TypeBool) ACTION_RETURN_BOOL(false);

	uint8_t *base = (uint8_t *)o + f->Offset;
	int v;
	if (f->BitValue < 0) v = (*(bool *)base) ? 1 : 0;
	else                 v = ((*base) & (1 << f->BitValue)) ? 1 : 0;

	if (out) *out = v;
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(FLevelLocals, GetFieldString)
{
	// SELF_STRUCT, not bare PROLOGUE. These are declared as methods on
	// LevelLocals rather than as statics, so the VM passes self as parameter
	// zero -- a bare PARAM_PROLOGUE does not consume it and every argument
	// after shifts by one, which reads the level itself as the target object.
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(o, DObject);
	PARAM_STRING(name);
	PARAM_OUTPOINTER(out, FString);

	PField *f = WR_ResolveField(o, name);
	if (f == nullptr || f->Type != TypeString) ACTION_RETURN_BOOL(false);

	if (out) *out = *(FString *)((uint8_t *)o + f->Offset);
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(FLevelLocals, GetFieldName)
{
	// SELF_STRUCT, not bare PROLOGUE. These are declared as methods on
	// LevelLocals rather than as statics, so the VM passes self as parameter
	// zero -- a bare PARAM_PROLOGUE does not consume it and every argument
	// after shifts by one, which reads the level itself as the target object.
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(o, DObject);
	PARAM_STRING(name);
	PARAM_OUTPOINTER(out, int);

	PField *f = WR_ResolveField(o, name);
	if (f == nullptr || f->Type != TypeName) ACTION_RETURN_BOOL(false);

	// A Name is an index into the name table, which is what the VM passes
	// around for one -- so it goes out as the int the script side receives.
	if (out) *out = ((FName *)((uint8_t *)o + f->Offset))->GetIndex();
	ACTION_RETURN_BOOL(true);
}

DEFINE_ACTION_FUNCTION(FLevelLocals, GetFieldObject)
{
	// SELF_STRUCT, not bare PROLOGUE. These are declared as methods on
	// LevelLocals rather than as statics, so the VM passes self as parameter
	// zero -- a bare PARAM_PROLOGUE does not consume it and every argument
	// after shifts by one, which reads the level itself as the target object.
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(o, DObject);
	PARAM_STRING(name);
	PARAM_OUTPOINTER(out, DObject *);

	PField *f = WR_ResolveField(o, name);
	if (f == nullptr || f->Type == nullptr) ACTION_RETURN_BOOL(false);
	if (f->Type->GetRegType() != REGT_POINTER) ACTION_RETURN_BOOL(false);

	if (out) *out = *(DObject **)((uint8_t *)o + f->Offset);
	ACTION_RETURN_BOOL(true);
}

// ENUMERATION -- the half that matters.
//
// The typed getters above let a caller ask a question it already knew to ask.
// These let it DISCOVER what there is to ask, which is the difference between
// a panel that supports a fixed list of mods and one that degrades usefully on
// all of them, including mods released after this fork stops being maintained.
//
// WALKS THE HIERARCHY. PClass::Fields holds only what a class DECLARES, so a
// weapon subclass that adds nothing reports zero fields and everything it
// inherited from Weapon and Actor is invisible -- which was the entire useful
// content. Collected base-first so an index means the same thing on both calls.
//
// Filtered here rather than at the point of use, so FieldCount and FieldAt
// agree: every index in 0..count-1 resolves, with no holes for a caller to
// trip over mid-loop.
static void WR_CollectFields(PClass *cls, TArray<PField *> &out)
{
	if (cls == nullptr) return;
	WR_CollectFields(cls->ParentClass, out);
	for (unsigned i = 0; i < cls->Fields.Size(); ++i)
	{
		PField *f = cls->Fields[i];
		if (f == nullptr) continue;
		if (f->Flags & (VARF_Private | VARF_Meta | VARF_Static)) continue;
		out.Push(f);
	}
}

DEFINE_ACTION_FUNCTION(FLevelLocals, FieldCount)
{
	// SELF_STRUCT, not bare PROLOGUE. These are declared as methods on
	// LevelLocals rather than as statics, so the VM passes self as parameter
	// zero -- a bare PARAM_PROLOGUE does not consume it and every argument
	// after shifts by one, which reads the level itself as the target object.
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(o, DObject);
	if (o == nullptr) ACTION_RETURN_INT(0);

	TArray<PField *> fields;
	WR_CollectFields(o->GetClass(), fields);
	ACTION_RETURN_INT((int)fields.Size());
}

DEFINE_ACTION_FUNCTION(FLevelLocals, FieldAt)
{
	// SELF_STRUCT, not bare PROLOGUE. These are declared as methods on
	// LevelLocals rather than as statics, so the VM passes self as parameter
	// zero -- a bare PARAM_PROLOGUE does not consume it and every argument
	// after shifts by one, which reads the level itself as the target object.
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(o, DObject);
	PARAM_INT(index);
	PARAM_OUTPOINTER(outName, FString);
	PARAM_OUTPOINTER(outType, FString);

	if (o == nullptr || index < 0) ACTION_RETURN_BOOL(false);

	TArray<PField *> fields;
	WR_CollectFields(o->GetClass(), fields);

	// Out of range is not an error worth a console line -- enumeration loops
	// are expected to probe.
	if ((unsigned)index >= fields.Size()) ACTION_RETURN_BOOL(false);

	PField *f = fields[index];
	if (f == nullptr) ACTION_RETURN_BOOL(false);

	if (outName) *outName = f->SymbolName.GetChars();

	// The type goes out as a plain string so a caller can pick the right
	// getter without this fork having to export the type system.
	if (outType)
	{
		const char *t = "other";
		// Bool before int: TypeBool derives from PInt, so the order matters
		// only for a reader's understanding, but reporting a flag as "int"
		// would send a caller to a getter that correctly refuses it.
		if      (f->Type == TypeBool) t = "bool";
		else if (f->Type == TypeSInt32 || f->Type == TypeUInt32) t = "int";
		else if (f->Type == TypeFloat32 || f->Type == TypeFloat64) t = "double";
		else if (f->Type == TypeString) t = "string";
		else if (f->Type == TypeName) t = "name";
		else if (f->Type != nullptr && f->Type->GetRegType() == REGT_POINTER) t = "object";
		*outType = t;
	}
	ACTION_RETURN_BOOL(true);
}

// Wrapped rather than bound straight to VR_IsScriptInputSuppressed: a direct
// native has to take the self pointer and return a VM-representable type, and a
// bare bool() satisfies neither.
static int IsVRInputSuppressed(FLevelLocals *self)
{
	return VR_IsScriptInputSuppressed() ? 1 : 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, IsVRInputSuppressed, IsVRInputSuppressed)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ACTION_RETURN_BOOL(IsVRInputSuppressed(self));
}

// GetModelOrientationHint -- what FindModelFrameRaw would resolve for a given
// (class, sprite, frame), specifically the parts a script cannot see any other
// way: whether the model's own MODELDEF Scale mirrors it (negative X), and the
// AngleOffset/PitchOffset/RollOffset baked into that same block.
//
// Written for the holster system's "why do some stored weapons face forward,
// some backward, some sideways" bug. The cause: mirroring is a per-MODEL
// authoring choice (chainsaw is -1.5 X, SMG is -1.0 X, rifle/pistol/revolver
// are positive, unmirrored) with NO correlation to which hand a weapon is
// normally held in -- so a single script-side "flip main hand 180" guess can
// only ever be right for a subset of the arsenal. Same story for PitchOffset:
// SMG bakes in +45, most weapons bake in 0. There was no way to know either
// value from script, so there was no way to compensate for real.
//
// Returns false if the class has no model bound at all (hasmodel false) or
// the (sprite, frame) pair has no FSpriteModelFrame -- e.g. a still-loading
// class, or a caller that got the frame wrong.
static int GetModelOrientationHint(FLevelLocals* self, PClass* cls, int sprite, int frame,
	bool* outMirrored, double* outAngleOffset, double* outPitchOffset, double* outRollOffset)
{
	*outMirrored = false;
	*outAngleOffset = 0.0;
	*outPitchOffset = 0.0;
	*outRollOffset = 0.0;

	if (cls == nullptr) return 0;
	auto def = GetDefaultByType(cls);
	if (def == nullptr || !def->hasmodel) return 0;

	FSpriteModelFrame* smf = FindModelFrame(cls, sprite, frame, false);
	if (smf == nullptr) return 0;

	*outMirrored = (smf->xscale < 0.0f);
	*outAngleOffset = smf->angleoffset;
	*outPitchOffset = smf->pitchoffset;
	*outRollOffset = smf->rolloffset;
	return 1;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetModelOrientationHint, GetModelOrientationHint)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_POINTER(cls, PClass);
	PARAM_INT(sprite);
	PARAM_INT(frame);
	bool mirrored; double angleoffset, pitchoffset, rolloffset;
	int found = GetModelOrientationHint(self, cls, sprite, frame, &mirrored, &angleoffset, &pitchoffset, &rolloffset);
	if (numret > 0) ret[0].SetInt(found);
	if (numret > 1) ret[1].SetInt(mirrored);
	if (numret > 2) ret[2].SetFloat(angleoffset);
	if (numret > 3) ret[3].SetFloat(pitchoffset);
	if (numret > 4) ret[4].SetFloat(rolloffset);
	return min(numret, 5);
}

// GetModelOffsetHint -- the model's baked POSITION offset (MODELDEF Offset /
// ZOffset), already divided by that model's own scale exactly the way
// RenderModel does it internally:
//   translate(xoffset/xscale, zoffset/(zscale*stretch), yoffset/yscale)
// so the three returned numbers are ready to use directly as a Doom-space
// (X, Y, Z) local offset -- no further division needed on the script side.
//
// This is centering, not orientation -- a different bug from the mirror/angle
// one GetModelOrientationHint answers. The chainsaw's Offset 0.0 14.0 0.0 is
// real displacement (14 units, not a rotation), and nothing before this could
// see it: a manual trim slider defaulting to zero applies no correction at
// all until someone finds and moves it, which is why centering kept failing
// even after the orientation fix landed.
//
// STRETCH IS 1.0 HERE, ALWAYS -- this was the second centering bug, found
// after the first (missing actor-scale) fix cut the drift from ~4.5 units to
// ~1.0 rather than to zero. RenderModel's local `stretch` variable starts at
// 1.f and is ONLY recomputed to getAspectFactor(pixelstretch)/pixelstretch
// INSIDE `if (smf_flags & MDL_CORRECTPIXELSTRETCH)`, a block that runs BEFORE
// the offset translate() -- so the translate divides by that real stretch
// value ONLY when the flag is set. When it is not (confirmed true for every
// block in both MODELDEFs here -- MDL_CORRECTPIXELSTRETCH never appears),
// `stretch` is still 1.f at the translate() call, and the SEPARATE
// `!(smf_flags & MDL_CORRECTPIXELSTRETCH)` block that computes the real
// aspect stretch runs AFTER the translate, as its own later scale() call --
// it resizes the whole mesh uniformly, it does not touch the offset value.
// Dividing by the passed-in pixelstretch here (as this function used to)
// applied a correction the renderer was never actually applying at that step.
// pixelstretch is still accepted as a parameter for API stability and in
// case a future weapon block opts into MDL_CORRECTPIXELSTRETCH, but nothing
// in it is used while that flag stays absent from the data, which is the
// only case this has ever needed to handle.
static int GetModelOffsetHint(FLevelLocals* self, PClass* cls, int sprite, int frame, double pixelstretch,
	double* outX, double* outY, double* outZ)
{
	*outX = 0.0; *outY = 0.0; *outZ = 0.0;

	if (cls == nullptr) return 0;
	auto def = GetDefaultByType(cls);
	if (def == nullptr || !def->hasmodel) return 0;

	FSpriteModelFrame* smf = FindModelFrame(cls, sprite, frame, false);
	if (smf == nullptr) return 0;
	if (smf->xscale == 0.0f || smf->yscale == 0.0f || smf->zscale == 0.0f) return 0;

	*outX = smf->xoffset / smf->xscale;
	*outY = smf->yoffset / smf->yscale;
	*outZ = smf->zoffset / smf->zscale;
	return 1;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetModelOffsetHint, GetModelOffsetHint)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_POINTER(cls, PClass);
	PARAM_INT(sprite);
	PARAM_INT(frame);
	PARAM_FLOAT(pixelstretch);
	double ox, oy, oz;
	int found = GetModelOffsetHint(self, cls, sprite, frame, pixelstretch, &ox, &oy, &oz);
	if (numret > 0) ret[0].SetInt(found);
	if (numret > 1) ret[1].SetFloat(ox);
	if (numret > 2) ret[2].SetFloat(oy);
	if (numret > 3) ret[3].SetFloat(oz);
	return min(numret, 4);
}

// GetModelWorldOffset -- what GetModelOffsetHint SHOULD have been from the
// start: instead of handing script a local offset and trusting script to
// re-derive the engine's own rotation composition (which is how the holster
// system's centering bug happened -- two independent hand-derivations of the
// yaw/pitch basis, each internally self-consistent, each wrong against the
// real renderer, because neither accounted for RenderModel silently
// NEGATING pitch before rotating: "pitch -= angles.Pitch.Degrees()" when
// MDL_USEACTORPITCH is set without MDL_BADROTATION), this builds the SAME
// VSMatrix with the SAME rotate() calls RenderModel itself makes and
// transforms the offset through it directly. No trig reconstruction, no
// axis-convention guessing -- it IS the engine's own transform.
//
// Assumes MDL_USEACTORPITCH + MDL_USEACTORROLL set, MDL_BADROTATION and
// MDL_USEROTATIONCENTER NOT set -- true for every block in this MODELDEF
// today (Add-ModelDefPitchFlags.ps1 only ever adds the first two). A weapon
// added later with different flags would need this extended; nothing here
// silently mishandles that case, it just was not built for data that does
// not currently exist.
//
// actorAngle/Pitch/Roll are passed in rather than read from an AActor,
// because the caller (a holster prop) wants the WOULD-BE offset for angles
// it has computed but not necessarily assigned to anything yet. Same for
// actorScaleX/Y.
//
// THE SCALE MATTERS, and getting that wrong is its own separate bug. In
// RenderModel the calls are, in this order:
//     objectToWorldMatrix.scale(scaleFactorX, scaleFactorZ, scaleFactorY);   // step 3
//     objectToWorldMatrix.translate(xoffset/xscale, zoffset/(zscale*stretch), yoffset/yscale); // step 4
// and because a later matrix call applies to the raw vertex FIRST, the offset
// translate happens BEFORE the scale -- so the offset is multiplied by it. The
// engine's comment on step 4 ("model offsets do not depend on model scalings")
// is only half the story: smf->xscale cancels against scaleFactorX, but the
// ACTOR's own Scale does not cancel and survives into the final displacement:
//     world displacement = (actorScaleX * xoffset,
//                           actorScaleX * yoffset,
//                           actorScaleY * zoffset / stretch)   [pre-rotation]
// Ignoring that made this function over-correct by 1/actorScale -- at a holster
// prop's 0.18 scale it subtracted roughly four times too much and pushed the
// shrunken model clean outside the marker sphere it was supposed to sit in.
static int GetModelWorldOffset(FLevelLocals* self, PClass* cls, int sprite, int frame, double pixelstretch,
	double actorAngle, double actorPitch, double actorRoll,
	double actorScaleX, double actorScaleY,
	double* outDX, double* outDY, double* outDZ)
{
	*outDX = 0.0; *outDY = 0.0; *outDZ = 0.0;

	if (cls == nullptr) return 0;
	auto def = GetDefaultByType(cls);
	if (def == nullptr || !def->hasmodel) return 0;

	FSpriteModelFrame* smf = FindModelFrame(cls, sprite, frame, false);
	if (smf == nullptr) return 0;

	// The MODELDEF scales cancel out of the product above, so unlike
	// GetModelOffsetHint this never divides by them and cannot trip over a
	// zero or negative scale. A mirrored model (negative xscale) likewise
	// needs no special case: the sign cancels for exactly the same reason.
	//
	// NO stretch division -- see the long comment on GetModelOffsetHint's
	// declaration. This used to divide localZ by the passed-in pixelstretch
	// (~1.2), a correction RenderModel only actually applies at this step
	// under MDL_CORRECTPIXELSTRETCH, which is absent from every block in both
	// MODELDEFs here. That extra division was the second centering bug: it
	// cut real drift from ~4.5 units to ~1.0 (an improvement, since it landed
	// on top of a genuine first bug -- the missing actor-scale factor -- but
	// not the whole fix). pixelstretch is kept as a parameter for the same
	// future-flag reason GetModelOffsetHint keeps it, unused while that flag
	// stays absent from the data.
	const double localX = actorScaleX * smf->xoffset;
	const double localY = actorScaleX * smf->yoffset;
	const double localZ = actorScaleY * smf->zoffset;

	// RenderModel's own rotation sequence, replicated exactly:
	//   rotate(-angle, 0,1,0); rotate(pitch, 0,0,1); rotate(-roll, 1,0,0);
	// with pitch pre-negated (the MDL_USEACTORPITCH, !MDL_BADROTATION branch)
	// and roll taken as-is (MDL_USEACTORROLL). No translate() calls at all --
	// this matrix represents pure rotation, so multMatrixPoint on a w=0
	// vector transforms a DIRECTION, never picking up a spurious position.
	const float engineePitch = (float)(-actorPitch);
	const float engineRoll = (float)actorRoll;
	const float engineAngle = (float)actorAngle;

	VSMatrix m;
	m.loadIdentity();
	m.rotate(-engineAngle, 0, 1, 0);
	m.rotate(engineePitch, 0, 0, 1);
	m.rotate(-engineRoll, 1, 0, 0);

	// Doom-space (X,Y,Z-up) -> matrix space (X, Z-as-matrixY, Y-as-matrixZ),
	// the SAME swap the engine's own offset translate() and position
	// translate() both use ("y scale for a sprite means height, i.e. z in
	// the world" -- r_data/models.cpp).
	FLOATTYPE point[4] = { (FLOATTYPE)localX, (FLOATTYPE)localZ, (FLOATTYPE)localY, 0.0f };
	FLOATTYPE result[4] = { 0, 0, 0, 0 };
	m.multMatrixPoint(point, result);

	// matrix space back to Doom space.
	*outDX = result[0];
	*outDY = result[2];
	*outDZ = result[1];
	return 1;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetModelWorldOffset, GetModelWorldOffset)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_POINTER(cls, PClass);
	PARAM_INT(sprite);
	PARAM_INT(frame);
	PARAM_FLOAT(pixelstretch);
	PARAM_FLOAT(actorAngle);
	PARAM_FLOAT(actorPitch);
	PARAM_FLOAT(actorRoll);
	PARAM_FLOAT(actorScaleX);
	PARAM_FLOAT(actorScaleY);
	double dx, dy, dz;
	int found = GetModelWorldOffset(self, cls, sprite, frame, pixelstretch, actorAngle, actorPitch, actorRoll, actorScaleX, actorScaleY, &dx, &dy, &dz);
	if (numret > 0) ret[0].SetInt(found);
	if (numret > 1) ret[1].SetFloat(dx);
	if (numret > 2) ret[2].SetFloat(dy);
	if (numret > 3) ret[3].SetFloat(dz);
	return min(numret, 4);
}

// GetModelBoundsHint -- the one measurement the holster system still had to
// guess at: how physically big a model actually is. GetModelOrientationHint
// and GetModelWorldOffset answer "which way" and "how far off-center"; this
// answers "how big", so a holster can solve scale = targetRadius /
// measuredRadius per weapon instead of applying one flat multiplier to every
// model regardless of its real size (today's behaviour -- a BFG and a pistol
// get the identical number and do not read as "the same size" in the ring).
//
// Returns a WORLD-space radius at actor Scale (1,1): FModel::GetLocalExtent
// hands back the model's raw per-axis extent in its own local units,
// unscaled by anything; this multiplies each axis by that FRAME's own
// MODELDEF Scale (smf->xscale/yscale/zscale) before combining them, the same
// "bake the model's own baked scale in, leave only actor scale for the
// caller" split GetModelWorldOffset already uses for position. A caller that
// wants the real actor-space radius just multiplies by its own Scale, same
// as it already does for the offset natives.
//
// Only as precise as GetLocalExtent's own contract: max |X|/|Y|/|Z|
// independently, not necessarily from one vertex, then combined as if they
// were -- a conservative (slightly oversized, never undersized) proxy for a
// tight bounding sphere. Good enough to solve "fit inside this radius"
// without needing exact mesh geometry on the script side, and erring toward
// too small on screen rather than clipping outside the marker.
//
// found=0 whenever the earlier three natives would also fail to resolve a
// (class, sprite, frame) triple, PLUS whenever the resolved model's format
// has no GetLocalExtent override (returns false by FModel's own default) --
// currently true for every format except FOBJModel, which the SAME real
// vertex data used for GetLocalExtent's silhouette check already lives on.
static int GetModelBoundsHint(FLevelLocals* self, PClass* cls, int sprite, int frame, double* outRadius)
{
	*outRadius = 0.0;

	if (cls == nullptr) return 0;
	auto def = GetDefaultByType(cls);
	if (def == nullptr || !def->hasmodel) return 0;

	FSpriteModelFrame* smf = FindModelFrame(cls, sprite, frame, false);
	if (smf == nullptr) return 0;
	if (smf->modelsAmount == 0 || smf->modelIDs[0] < 0) return 0;

	FModel* model = Models[smf->modelIDs[0]];
	if (model == nullptr) return 0;

	float ex, ey, ez;
	if (!model->GetLocalExtent(&ex, &ey, &ez)) return 0;

	const double sx = (double)ex * smf->xscale;
	const double sy = (double)ey * smf->yscale;
	const double sz = (double)ez * smf->zscale;
	*outRadius = sqrt(sx * sx + sy * sy + sz * sz);
	return 1;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetModelBoundsHint, GetModelBoundsHint)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_POINTER(cls, PClass);
	PARAM_INT(sprite);
	PARAM_INT(frame);
	double radius;
	int found = GetModelBoundsHint(self, cls, sprite, frame, &radius);
	if (numret > 0) ret[0].SetInt(found);
	if (numret > 1) ret[1].SetFloat(radius);
	return min(numret, 2);
}

// GetActorModelClass -- which class's MODELDEF an ACTOR INSTANCE actually
// resolves against right now, as opposed to which class its own type is.
// Mirrors FindModelFrame(AActor*)'s own fallback exactly (r_data/models.cpp,
// line ~2163): modelData->modelDef if a per-instance override has been set,
// else the actor's own GetClass(). A_ChangeModel sets modelData->modelDef as
// a side effect on the INSTANCE it is called on (see actor.zs's own
// `hasmodel` comment, which documents exactly this: "A_ChangeModel sets it
// on the instance as a side effect"), and it persists there regardless of
// whether that actor is currently being rendered.
//
// Built so a holster/prop system can show the CORRECT model for a weapon
// some OTHER mod has model-swapped onto a per-instance basis. ModelSwapper
// is the motivating case: it points a flat-sprite weapon's psprite at a
// donor class's model via A_ChangeModel called on the live weapon instance,
// and never registers a MODELDEF entry under that weapon's OWN class name
// at all -- a caller doing its own class-name-keyed lookup (what every
// GetModel*Hint above already does, and what RS_Holsters' holster prop used
// to do directly) finds nothing for exactly that weapon, because the model
// only ever lived on the instance, never on the class.
static PClass* GetActorModelClass(FLevelLocals* self, AActor* act)
{
	if (act == nullptr) return nullptr;
	if (act->modelData != nullptr && act->modelData->modelDef != nullptr)
		return act->modelData->modelDef;
	return act->GetClass();
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, GetActorModelClass, GetActorModelClass)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_OBJECT(act, AActor);
	ACTION_RETURN_POINTER(GetActorModelClass(self, act));
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, AimBillboard, AimBillboard)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(sx); PARAM_FLOAT(sy); PARAM_FLOAT(sz);
	PARAM_FLOAT(dx); PARAM_FLOAT(dy); PARAM_FLOAT(dz);
	PARAM_FLOAT(maxDist);
	DVector2 uv;
	int id = AimBillboard(self, sx, sy, sz, dx, dy, dz, maxDist, &uv);
	if (numret > 0) ret[0].SetInt(id);
	if (numret > 1) ret[1].SetVector2(uv);
	return min(numret, 2);
}

DEFINE_ACTION_FUNCTION(FLevelLocals, GetDisplacement)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_INT(pg1);
	PARAM_INT(pg2);

	DVector2 ofs(0, 0);
	if (pg1 != pg2)
	{
		unsigned i = pg1 + self->Displacements.size * pg2;
		if (i < self->Displacements.data.Size())
			ofs = self->Displacements.data[i].pos;
	}

	ACTION_RETURN_VEC2(ofs);
}

DEFINE_ACTION_FUNCTION(FLevelLocals, GetPortalGroupCount)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ACTION_RETURN_INT(self->Displacements.size);
}

void SphericalCoords(FLevelLocals *self, double vpX, double vpY, double vpZ, double tX, double tY, double tZ, double viewYaw, double viewPitch, int absolute, DVector3 *result)
{

	DVector3 viewpoint(vpX, vpY, vpZ);
	DVector3 target(tX, tY, tZ);
	auto vecTo = absolute ? target - viewpoint : VecDiff(self, viewpoint, target);

	*result = (DVector3(
								deltaangle(vecTo.Angle(), DAngle::fromDeg(viewYaw)).Degrees(),
								deltaangle(vecTo.Pitch(), DAngle::fromDeg(viewPitch)).Degrees(),
								vecTo.Length()
								));

}
DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, SphericalCoords, SphericalCoords)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_FLOAT(viewpointX);
	PARAM_FLOAT(viewpointY);
	PARAM_FLOAT(viewpointZ);
	PARAM_FLOAT(targetX);
	PARAM_FLOAT(targetY);
	PARAM_FLOAT(targetZ);
	PARAM_FLOAT(viewYaw);
	PARAM_FLOAT(viewPitch);
	PARAM_BOOL(absolute);
	DVector3 result;
	SphericalCoords(self, viewpointX, viewpointY, viewpointZ, targetX, targetY, targetZ, viewYaw, viewPitch, absolute, &result);
	ACTION_RETURN_VEC3(result);
}

static void LookupString(FLevelLocals *level, uint32_t index, FString *res)
{
	*res = level->Behaviors.LookupString(index);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, LookupString, LookupString)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_UINT(index);
	FString res;
	LookupString(self, index, &res);
	ACTION_RETURN_STRING(res);
}

static int isFrozen(FLevelLocals *self)
{
	return self->isFrozen();
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, isFrozen, isFrozen)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	ACTION_RETURN_INT(isFrozen(self));
}

void setFrozen(FLevelLocals *self, int on)
{
	self->frozenstate = (self->frozenstate & ~1) | !!on;
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, setFrozen, setFrozen)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_BOOL(on);
	setFrozen(self, on);
	return 0;
}

static DThinker* CreateThinker(FLevelLocals* self, PClass* type, int statnum)
{
	if (type->IsDescendantOf(NAME_Actor))
	{
		ThrowAbortException(X_OTHER, "Actors cannot be created from this function");
		return nullptr;
	}
	else if (type->IsDescendantOf(NAME_VisualThinker))
	{
		ThrowAbortException(X_OTHER, "VisualThinkers cannot be created from this function");
		return nullptr;
	}

	return self->CreateThinker(type, statnum);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, CreateThinker, CreateThinker)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_POINTER_NOT_NULL(type, PClass);
	PARAM_INT(statnum);

	ACTION_RETURN_OBJECT(CreateThinker(self, type, statnum));
}

static DThinker* CreateClientSideThinker(FLevelLocals* self, PClass* type, int statnum)
{
	if (type->IsDescendantOf(NAME_Actor))
	{
		ThrowAbortException(X_OTHER, "Actors cannot be created from this function");
		return nullptr;
	}
	else if (type->IsDescendantOf(NAME_VisualThinker))
	{
		ThrowAbortException(X_OTHER, "VisualThinkers cannot be created from this function");
		return nullptr;
	}

	return self->CreateClientSideThinker(type, statnum);
}

DEFINE_ACTION_FUNCTION_NATIVE(FLevelLocals, CreateClientSideThinker, CreateClientSideThinker)
{
	PARAM_SELF_STRUCT_PROLOGUE(FLevelLocals);
	PARAM_POINTER_NOT_NULL(type, PClass);
	PARAM_INT(statnum);

	ACTION_RETURN_OBJECT(CreateClientSideThinker(self, type, statnum));
}

//=====================================================================================
//
//
//
//=====================================================================================

DEFINE_ACTION_FUNCTION_NATIVE(_AltHUD, GetLatency, Net_GetLatency)
{
	PARAM_PROLOGUE;
	int ld, ad;
	int severity = Net_GetLatency(&ld, &ad);
	if (numret > 0) ret[0].SetInt(severity);
	if (numret > 1) ret[1].SetInt(ld);
	if (numret > 2) ret[2].SetInt(ad);
	return numret;
}

DEFINE_ACTION_FUNCTION(_CVar, GetCVar)
{
	PARAM_PROLOGUE;
	PARAM_NAME(name);
	PARAM_POINTER(plyr, player_t);
	ACTION_RETURN_POINTER(GetCVar(plyr ? int(plyr - players) : -1, name.GetChars()));
}


DEFINE_ACTION_FUNCTION(DObject, S_ChangeMusic)
{
	PARAM_PROLOGUE;
	PARAM_STRING(music);
	PARAM_INT(order);
	PARAM_BOOL(looping);
	PARAM_BOOL(force);
	ACTION_RETURN_BOOL(S_ChangeMusic(music.GetChars(), order, looping, force));
}


DEFINE_ACTION_FUNCTION(_Screen, GetViewWindow)
{
	PARAM_PROLOGUE;
	if (numret > 0) ret[0].SetInt(viewwindowx);
	if (numret > 1) ret[1].SetInt(viewwindowy);
	if (numret > 2) ret[2].SetInt(viewwidth);
	if (numret > 3) ret[3].SetInt(viewheight);
	return min(numret, 4);
}

DEFINE_ACTION_FUNCTION(_Console, MidPrint)
{
	PARAM_PROLOGUE;
	PARAM_POINTER(fnt, FFont);
	PARAM_STRING(text);
	PARAM_BOOL(bold);

	const char* txt = text[0] == '$' ? GStrings.GetString(&text[1]) : text.GetChars();
	C_MidPrint(fnt, txt, bold);
	return 0;
}

//==========================================================================
//
//
//
//==========================================================================

static int isValid( level_info_t *info )
{
	return info->isValid();
}

DEFINE_ACTION_FUNCTION_NATIVE(_LevelInfo, isValid, isValid)
{
	PARAM_SELF_STRUCT_PROLOGUE(level_info_t);
	ACTION_RETURN_BOOL(isValid(self));
}

static void LookupLevelName( level_info_t *info, FString *result )
{
	*result = info->LookupLevelName();
}

DEFINE_ACTION_FUNCTION_NATIVE(_LevelInfo, LookupLevelName, LookupLevelName)
{
	PARAM_SELF_STRUCT_PROLOGUE(level_info_t);
	FString rets;
	LookupLevelName(self,&rets);
	ACTION_RETURN_STRING(rets);
}

static int GetLevelInfoCount()
{
	return wadlevelinfos.Size();
}

DEFINE_ACTION_FUNCTION_NATIVE(_LevelInfo, GetLevelInfoCount, GetLevelInfoCount)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_INT(GetLevelInfoCount());
}

static level_info_t* GetLevelInfo( unsigned int index )
{
	if ( index >= wadlevelinfos.Size() )
		return nullptr;
	return &wadlevelinfos[index];
}

DEFINE_ACTION_FUNCTION_NATIVE(_LevelInfo, GetLevelInfo, GetLevelInfo)
{
	PARAM_PROLOGUE;
	PARAM_INT(index);
	ACTION_RETURN_POINTER(GetLevelInfo(index));
}

static level_info_t* ZFindLevelInfo( const FString &mapname )
{
	return FindLevelInfo(mapname.GetChars());
}

DEFINE_ACTION_FUNCTION_NATIVE(_LevelInfo, FindLevelInfo, ZFindLevelInfo)
{
	PARAM_PROLOGUE;
	PARAM_STRING(mapname);
	ACTION_RETURN_POINTER(ZFindLevelInfo(mapname));
}

DEFINE_ACTION_FUNCTION_NATIVE(_LevelInfo, FindLevelByNum, FindLevelByNum)
{
	PARAM_PROLOGUE;
	PARAM_INT(num);
	ACTION_RETURN_POINTER(FindLevelByNum(num));
}

static int MapExists( const FString &mapname )
{
	return P_CheckMapData(mapname.GetChars());
}

DEFINE_ACTION_FUNCTION_NATIVE(_LevelInfo, MapExists, MapExists)
{
	PARAM_PROLOGUE;
	PARAM_STRING(mapname);
	ACTION_RETURN_BOOL(MapExists(mapname));
}

DEFINE_ACTION_FUNCTION(_LevelInfo, MapChecksum)
{
	PARAM_PROLOGUE;
	PARAM_STRING(mapname);
	char md5string[33] = "";
	MapData *map = P_OpenMapData(mapname.GetChars(), true);
	if (map != nullptr)
	{
		uint8_t cksum[16];
		map->GetChecksum(cksum);
		for (int j = 0; j < 16; ++j)
		{
			snprintf(md5string + j * 2, 3, "%02x", cksum[j]);
		}
		delete map;
	}
	ACTION_RETURN_STRING((const char *)md5string);
}

//==========================================================================
//
//
//
//==========================================================================
DEFINE_FIELD_X(LevelInfo, level_info_t, levelnum)
DEFINE_FIELD_X(LevelInfo, level_info_t, MapName)
DEFINE_FIELD_X(LevelInfo, level_info_t, NextMap)
DEFINE_FIELD_X(LevelInfo, level_info_t, NextSecretMap)
DEFINE_FIELD_X(LevelInfo, level_info_t, SkyPic1)
DEFINE_FIELD_X(LevelInfo, level_info_t, SkyPic2)
DEFINE_FIELD_X(LevelInfo, level_info_t, SkyMistPic)
DEFINE_FIELD_X(LevelInfo, level_info_t, F1Pic)
DEFINE_FIELD_X(LevelInfo, level_info_t, cluster)
DEFINE_FIELD_X(LevelInfo, level_info_t, partime)
DEFINE_FIELD_X(LevelInfo, level_info_t, sucktime)
DEFINE_FIELD_X(LevelInfo, level_info_t, flags)
DEFINE_FIELD_X(LevelInfo, level_info_t, flags2)
DEFINE_FIELD_X(LevelInfo, level_info_t, flags3)
DEFINE_FIELD_X(LevelInfo, level_info_t, Music)
DEFINE_FIELD_X(LevelInfo, level_info_t, LightningSound)
DEFINE_FIELD_X(LevelInfo, level_info_t, LevelName)
DEFINE_FIELD_X(LevelInfo, level_info_t, AuthorName)
DEFINE_FIELD_X(LevelInfo, level_info_t, Description)	// MAPINFO data for scripts (GZSelaco)
DEFINE_FIELD_X(LevelInfo, level_info_t, levelgroup)
DEFINE_FIELD_X(LevelInfo, level_info_t, areaNum)
DEFINE_FIELD_X(LevelInfo, level_info_t, invasiontier)
DEFINE_FIELD_X(LevelInfo, level_info_t, tilt)
DEFINE_FIELD_X(LevelInfo, level_info_t, tiltAngle)
DEFINE_FIELD_X(LevelInfo, level_info_t, MapLabel)
DEFINE_FIELD_X(LevelInfo, level_info_t, musicorder)
DEFINE_FIELD_X(LevelInfo, level_info_t, skyspeed1)
DEFINE_FIELD_X(LevelInfo, level_info_t, skyspeed2)
DEFINE_FIELD_X(LevelInfo, level_info_t, skymistspeed)
DEFINE_FIELD_X(LevelInfo, level_info_t, skymistyscale)
DEFINE_FIELD_X(LevelInfo, level_info_t, cdtrack)
DEFINE_FIELD_X(LevelInfo, level_info_t, gravity)
DEFINE_FIELD_X(LevelInfo, level_info_t, aircontrol)
DEFINE_FIELD_X(LevelInfo, level_info_t, airsupply)
DEFINE_FIELD_X(LevelInfo, level_info_t, compatflags)
DEFINE_FIELD_X(LevelInfo, level_info_t, compatflags2)
DEFINE_FIELD_X(LevelInfo, level_info_t, deathsequence)
DEFINE_FIELD_X(LevelInfo, level_info_t, fogdensity)
DEFINE_FIELD_X(LevelInfo, level_info_t, outsidefogdensity)
DEFINE_FIELD_X(LevelInfo, level_info_t, skyfog)
DEFINE_FIELD_X(LevelInfo, level_info_t, thickfogdistance)
DEFINE_FIELD_X(LevelInfo, level_info_t, thickfogmultiplier)
DEFINE_FIELD_X(LevelInfo, level_info_t, pixelstretch)
DEFINE_FIELD_X(LevelInfo, level_info_t, RedirectType)
DEFINE_FIELD_X(LevelInfo, level_info_t, RedirectMapName)
DEFINE_FIELD_X(LevelInfo, level_info_t, teamdamage)

DEFINE_GLOBAL_NAMED(currentVMLevel, level)
DEFINE_FIELD(FLevelLocals, sectors)
DEFINE_FIELD(FLevelLocals, lines)
DEFINE_FIELD(FLevelLocals, sides)
DEFINE_FIELD(FLevelLocals, vertexes)
DEFINE_FIELD(FLevelLocals, linePortals)
DEFINE_FIELD(FLevelLocals, sectorPortals)
DEFINE_FIELD(FLevelLocals, time)
DEFINE_FIELD(FLevelLocals, maptime)
DEFINE_FIELD(FLevelLocals, totaltime)
DEFINE_FIELD(FLevelLocals, starttime)
DEFINE_FIELD(FLevelLocals, partime)
DEFINE_FIELD(FLevelLocals, sucktime)
DEFINE_FIELD(FLevelLocals, cluster)
DEFINE_FIELD(FLevelLocals, clusterflags)
DEFINE_FIELD(FLevelLocals, levelnum)
DEFINE_FIELD(FLevelLocals, LevelName)
DEFINE_FIELD(FLevelLocals, MapName)
DEFINE_FIELD(FLevelLocals, NextMap)
DEFINE_FIELD(FLevelLocals, NextSecretMap)
DEFINE_FIELD(FLevelLocals, F1Pic)
DEFINE_FIELD(FLevelLocals, AuthorName)
DEFINE_FIELD(FLevelLocals, levelgroup)	// MAPINFO data for scripts (GZSelaco)
DEFINE_FIELD(FLevelLocals, areaNum)
DEFINE_FIELD(FLevelLocals, invasiontier)
DEFINE_FIELD(FLevelLocals, tilt)
DEFINE_FIELD(FLevelLocals, tiltAngle)
DEFINE_FIELD(FLevelLocals, maptype)
DEFINE_FIELD(FLevelLocals, LightningSound)
DEFINE_FIELD(FLevelLocals, Music)
DEFINE_FIELD(FLevelLocals, musicorder)
DEFINE_FIELD(FLevelLocals, skytexture1)
DEFINE_FIELD(FLevelLocals, skytexture2)
DEFINE_FIELD(FLevelLocals, skymisttexture)
DEFINE_FIELD(FLevelLocals, skyspeed1)
DEFINE_FIELD(FLevelLocals, skyspeed2)
DEFINE_FIELD(FLevelLocals, skymistspeed)
DEFINE_FIELD(FLevelLocals, skymistyscale)
DEFINE_FIELD(FLevelLocals, total_secrets)
DEFINE_FIELD(FLevelLocals, found_secrets)
DEFINE_FIELD(FLevelLocals, total_items)
DEFINE_FIELD(FLevelLocals, found_items)
DEFINE_FIELD(FLevelLocals, total_monsters)
DEFINE_FIELD(FLevelLocals, killed_monsters)
DEFINE_FIELD(FLevelLocals, gravity)
DEFINE_FIELD(FLevelLocals, aircontrol)
DEFINE_FIELD(FLevelLocals, airfriction)
DEFINE_FIELD(FLevelLocals, airsupply)
DEFINE_FIELD(FLevelLocals, teamdamage)
DEFINE_FIELD(FLevelLocals, fogdensity)
DEFINE_FIELD(FLevelLocals, outsidefogdensity)
DEFINE_FIELD(FLevelLocals, skyfog)
DEFINE_FIELD(FLevelLocals, thickfogdistance)
DEFINE_FIELD(FLevelLocals, thickfogmultiplier)
DEFINE_FIELD(FLevelLocals, pixelstretch)
DEFINE_FIELD(FLevelLocals, MusicVolume)
DEFINE_FIELD(FLevelLocals, deathsequence)
DEFINE_FIELD_BIT(FLevelLocals, frozenstate, frozen, 1)	// still needed for backwards compatibility.
DEFINE_FIELD_NAMED(FLevelLocals, i_compatflags, compatflags)
DEFINE_FIELD_NAMED(FLevelLocals, i_compatflags2, compatflags2)
DEFINE_FIELD(FLevelLocals, info);

DEFINE_FIELD_BIT(FLevelLocals, flags, noinventorybar, LEVEL_NOINVENTORYBAR)
DEFINE_FIELD_BIT(FLevelLocals, flags, monsterstelefrag, LEVEL_MONSTERSTELEFRAG)
DEFINE_FIELD_BIT(FLevelLocals, flags, actownspecial, LEVEL_ACTOWNSPECIAL)
DEFINE_FIELD_BIT(FLevelLocals, flags, sndseqtotalctrl, LEVEL_SNDSEQTOTALCTRL)
DEFINE_FIELD_BIT(FLevelLocals, flags, useplayerstartz, LEVEL_USEPLAYERSTARTZ)
DEFINE_FIELD_BIT(FLevelLocals, flags2, allmap, LEVEL2_ALLMAP)
DEFINE_FIELD_BIT(FLevelLocals, flags2, missilesactivateimpact, LEVEL2_MISSILESACTIVATEIMPACT)
DEFINE_FIELD_BIT(FLevelLocals, flags2, monsterfallingdamage, LEVEL2_MONSTERFALLINGDAMAGE)
DEFINE_FIELD_BIT(FLevelLocals, flags2, checkswitchrange, LEVEL2_CHECKSWITCHRANGE)
DEFINE_FIELD_BIT(FLevelLocals, flags2, polygrind, LEVEL2_POLYGRIND)
DEFINE_FIELD_BIT(FLevelLocals, flags2, allowrespawn, LEVEL2_ALLOWRESPAWN)
DEFINE_FIELD_BIT(FLevelLocals, flags2, nomonsters, LEVEL2_NOMONSTERS)
DEFINE_FIELD_BIT(FLevelLocals, flags2, infinite_flight, LEVEL2_INFINITE_FLIGHT)
DEFINE_FIELD_BIT(FLevelLocals, flags2, no_dlg_freeze, LEVEL2_CONV_SINGLE_UNFREEZE)
DEFINE_FIELD_BIT(FLevelLocals, flags2, keepfullinventory, LEVEL2_KEEPFULLINVENTORY)
DEFINE_FIELD_BIT(FLevelLocals, flags3, removeitems, LEVEL3_REMOVEITEMS)
DEFINE_FIELD_BIT(FLevelLocals, flags3, rainymap, LEVEL3_RAINYMAP)	// MAPINFO hints for scripts (GZSelaco)
DEFINE_FIELD_BIT(FLevelLocals, flags3, windymap, LEVEL3_WINDYMAP)
DEFINE_FIELD_BIT(FLevelLocals, flags3, saferoom, LEVEL3_SAFEROOM)

DEFINE_FIELD_X(Sector, sector_t, floorplane)
DEFINE_FIELD_X(Sector, sector_t, ceilingplane)
DEFINE_FIELD_X(Sector, sector_t, Colormap)
DEFINE_FIELD_X(Sector, sector_t, SpecialColors)
DEFINE_FIELD_X(Sector, sector_t, AdditiveColors)
DEFINE_FIELD_X(Sector, sector_t, SoundTarget)
DEFINE_FIELD_X(Sector, sector_t, special)
DEFINE_FIELD_X(Sector, sector_t, lightlevel)
DEFINE_FIELD_X(Sector, sector_t, seqType)
DEFINE_FIELD_NAMED_X(Sector, sector_t, skytransfer, sky)
DEFINE_FIELD_X(Sector, sector_t, SeqName)
DEFINE_FIELD_X(Sector, sector_t, centerspot)
DEFINE_FIELD_X(Sector, sector_t, validcount)
DEFINE_FIELD_X(Sector, sector_t, thinglist)
DEFINE_FIELD_X(Sector, sector_t, friction)
DEFINE_FIELD_X(Sector, sector_t, movefactor)
DEFINE_FIELD_X(Sector, sector_t, terrainnum)
DEFINE_FIELD_X(Sector, sector_t, floordata)
DEFINE_FIELD_X(Sector, sector_t, ceilingdata)
DEFINE_FIELD_X(Sector, sector_t, lightingdata)
DEFINE_FIELD_X(Sector, sector_t, Level)
DEFINE_FIELD_X(Sector, sector_t, interpolations)
DEFINE_FIELD_X(Sector, sector_t, soundtraversed)
DEFINE_FIELD_X(Sector, sector_t, stairlock)
DEFINE_FIELD_X(Sector, sector_t, prevsec)
DEFINE_FIELD_X(Sector, sector_t, nextsec)
DEFINE_FIELD_UNSIZED(Sector, sector_t, Lines)
DEFINE_FIELD_X(Sector, sector_t, heightsec)
DEFINE_FIELD_X(Sector, sector_t, bottommap)
DEFINE_FIELD_X(Sector, sector_t, midmap)
DEFINE_FIELD_X(Sector, sector_t, topmap)
DEFINE_FIELD_X(Sector, sector_t, touching_thinglist)
DEFINE_FIELD_X(Sector, sector_t, sectorportal_thinglist)
DEFINE_FIELD_X(Sector, sector_t, gravity)
DEFINE_FIELD_X(Sector, sector_t, damagetype)
DEFINE_FIELD_X(Sector, sector_t, damageamount)
DEFINE_FIELD_X(Sector, sector_t, damageinterval)
DEFINE_FIELD_X(Sector, sector_t, leakydamage)
DEFINE_FIELD_X(Sector, sector_t, ZoneNumber)
DEFINE_FIELD_X(Sector, sector_t, healthceiling)
DEFINE_FIELD_X(Sector, sector_t, healthfloor)
DEFINE_FIELD_X(Sector, sector_t, healthceilinggroup)
DEFINE_FIELD_X(Sector, sector_t, healthfloorgroup)
DEFINE_FIELD_X(Sector, sector_t, MoreFlags)
DEFINE_FIELD_X(Sector, sector_t, Flags)
DEFINE_FIELD_X(Sector, sector_t, SecActTarget)
DEFINE_FIELD_X(Sector, sector_t, Portals)
DEFINE_FIELD_X(Sector, sector_t, PortalGroup)
DEFINE_FIELD_X(Sector, sector_t, sectornum)

DEFINE_FIELD_X(Line, line_t, v1)
DEFINE_FIELD_X(Line, line_t, v2)
DEFINE_FIELD_X(Line, line_t, delta)
DEFINE_FIELD_X(Line, line_t, flags)
DEFINE_FIELD_X(Line, line_t, flags2)
DEFINE_FIELD_X(Line, line_t, blockBits)	// [BLOCKBITS] GZSelaco 14d9255578: line_t::blockBits (r_defs.h), ZScript Line.blockBits (mapdata.zs)
DEFINE_FIELD_X(Line, line_t, activation)
DEFINE_FIELD_X(Line, line_t, special)
DEFINE_FIELD_X(Line, line_t, args)
DEFINE_FIELD_X(Line, line_t, alpha)
DEFINE_FIELD_X(Line, line_t, sidedef)
DEFINE_FIELD_X(Line, line_t, bbox)
DEFINE_FIELD_X(Line, line_t, frontsector)
DEFINE_FIELD_X(Line, line_t, backsector)
DEFINE_FIELD_X(Line, line_t, validcount)
DEFINE_FIELD_X(Line, line_t, locknumber)
DEFINE_FIELD_X(Line, line_t, portalindex)
DEFINE_FIELD_X(Line, line_t, portaltransferred)
DEFINE_FIELD_X(Line, line_t, health)
DEFINE_FIELD_X(Line, line_t, healthgroup)

DEFINE_FIELD_X(Side, side_t, sector)
DEFINE_FIELD_X(Side, side_t, linedef)
DEFINE_FIELD_X(Side, side_t, Light)
DEFINE_FIELD_X(Side, side_t, Flags)

DEFINE_FIELD_X(Secplane, secplane_t, normal)
DEFINE_FIELD_X(Secplane, secplane_t, D)
DEFINE_FIELD_X(Secplane, secplane_t, negiC)

DEFINE_FIELD_NAMED_X(F3DFloor, F3DFloor, bottom.plane, bottom);
DEFINE_FIELD_NAMED_X(F3DFloor, F3DFloor, top.plane, top);
DEFINE_FIELD_X(F3DFloor, F3DFloor, flags);
DEFINE_FIELD_X(F3DFloor, F3DFloor, master);
DEFINE_FIELD_X(F3DFloor, F3DFloor, model);
DEFINE_FIELD_X(F3DFloor, F3DFloor, target);
DEFINE_FIELD_X(F3DFloor, F3DFloor, alpha);

DEFINE_FIELD_X(Vertex, vertex_t, p)

DEFINE_FIELD(DBaseStatusBar, Centering);
DEFINE_FIELD(DBaseStatusBar, FixedOrigin);
DEFINE_FIELD(DBaseStatusBar, CrosshairSize);
DEFINE_FIELD(DBaseStatusBar, Displacement);
DEFINE_FIELD(DBaseStatusBar, CPlayer);
DEFINE_FIELD(DBaseStatusBar, ShowLog);
DEFINE_FIELD(DBaseStatusBar, artiflashTick);
DEFINE_FIELD(DBaseStatusBar, itemflashFade);
DEFINE_FIELD(DBaseStatusBar, ScoreboardFont);
DEFINE_FIELD(DBaseStatusBar, BigScoreboardFont);


DEFINE_GLOBAL(StatusBar);
