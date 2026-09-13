/*
** r_utility.h
**
** Rendering main loop and setup/utility functions
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 id Software
** Copyright 1994-1996 Raven Software
** Copyright 1999-2016 Marisa Heit
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#ifndef __R_UTIL_H
#define __R_UTIL_H

#include "r_state.h"
#include "vectors.h"

class FSerializer;
struct FViewWindow;
//
// Stuff from r_main.h that's needed outside the rendering code.

// Number of diminishing brightness levels.
// There a 0-31, i.e. 32 LUT in the COLORMAP lump.
#define NUMCOLORMAPS			32

struct FLevelLocals;

struct FRenderViewpoint
{
	FRenderViewpoint();

	player_t		*player;		// For which player is this viewpoint being renderered? (can be null for camera textures)
	DVector3		Pos;			// Camera position
	DVector3		CenterEyePos;	// Camera position without view shift
	DVector3		ActorPos;		// Camera actor's position
	DRotator		Angles;			// Camera angles
	FRotator		HWAngles;		// Actual rotation angles for the hardware renderer
	DVector2		ViewVector;		// HWR only: direction the camera is facing.
	DVector3		ViewVector3D;	// 3D direction the camera is facing.
	DVector3        OffPos;         // Viewpoint position to use for Ortho and OoB calculations
	AActor			*ViewActor;		// either the same as camera or nullptr
	FLevelLocals	*ViewLevel;		// The level this viewpoint is on.

	DVector3		Path[2];		// View path for portal calculations
	double			Cos;			// cos(Angles.Yaw)
	double			Sin;			// sin(Angles.Yaw)
	double			TanCos;			// FocalTangent * cos(Angles.Yaw)
	double			TanSin;			// FocalTangent * sin(Angles.Yaw)
	double			PitchCos;		// cos(Angles.Pitch)
	double			PitchSin;		// sin(Angles.Pitch)
	double			floordistfact;	// used for isometric sprites Y-billboarding compensation in hw_sprites.cpp
	double			cotfloor;		// used for isometric sprites Y-billboarding compensation in hw_sprites.cpp
	angle_t		FrustAngle; 	// FrustumAngle() result

	AActor			*camera;		// camera actor
	sector_t		*sector;		// [RH] keep track of sector viewing from
	double			ScreenProj;	// Screen projection factor for orthographic projection
	double			ScreenProjX;	// Same for X-axis (screenspace)

	double			TicFrac;		// fraction of tic for interpolation
	uint32_t		FrameTime;		// current frame's time in tics.

	int				extralight;		// extralight to be added to this viewpoint
	bool			showviewer;		// show the camera actor?
	bool			bForceNoViewer; // Never show the camera Actor.
	bool			bDoOob;
	bool			bDoOrtho;
	void SetViewAngle(const FViewWindow& viewWindow);
	bool IsAllowedOoB();				// Checks if camera actor exists, has viewpos, and viewpos has VPSF_ALLOWOUTOFBOUNDS flag set
	bool IsOrtho();					// Checks if camera actor exists, has viewpos, and viewpos has VPSF_ORTHOGRAPHIC flag set
	DAngle GetFieldOfView() const;
	void SetFieldOfView(DAngle);

private:
	DAngle			FieldOfView;	// current field of view

};

extern FRenderViewpoint r_viewpoint;

//-----------------------------------
struct FViewWindow
{
	double FocalTangent = 0.0;
	int centerx = 0;
	int centerxwide = 0;
	int centery = 0;
	float WidescreenRatio = 0.0f;
};

extern FViewWindow r_viewwindow;

//-----------------------------------


extern int				setblocks;
extern bool				r_NoInterpolate;
extern int				validcount;
extern int				dl_validcount;			// For use with FSection. validcount is in use by the renderer and any quick section exclusion needs another variable.

extern angle_t			LocalViewAngle;			// [RH] Added to consoleplayer's angle
extern int				LocalViewPitch;			// [RH] Used directly instead of consoleplayer's pitch
extern bool				LocalKeyboardTurner;	// [RH] The local player used the keyboard to turn, so interpolate

extern unsigned int		R_OldBlend;

const double			r_Yaspect = 200.0;		// Why did I make this a variable? It's never set anywhere.


//==========================================================================
//
// R_PointOnSide
//
// Traverse BSP (sub) tree, check point against partition plane.
// Returns side 0 (front/on) or 1 (back).
//
// [RH] inlined, stripped down, and made more precise
//
//==========================================================================

inline constexpr int R_PointOnSide (fixed_t x, fixed_t y, const node_t *node)
{
	return DMulScale (y-node->y, node->dx, node->x-x, node->dy, 32) > 0;
}
inline int R_PointOnSide(double x, double y, const node_t *node)
{
	return DMulScale(FLOAT2FIXED(y) - node->y, node->dx, node->x - FLOAT2FIXED(x), node->dy, 32) > 0;
}
inline int R_PointOnSide(const DVector2 &pos, const node_t *node)
{
	return DMulScale(FLOAT2FIXED(pos.Y) - node->y, node->dx, node->x - FLOAT2FIXED(pos.X), node->dy, 32) > 0;
}

// Used for interpolation waypoints.
struct DVector3a
{
	DVector3 pos;
	DAngle angle;
};

void R_ResetViewInterpolation ();
void R_RebuildViewInterpolation(player_t *player);
bool R_GetViewInterpolationStatus();
void R_ClearInterpolationPath();
void R_AddInterpolationPoint(const DVector3a &vec);
void R_SetViewSize (int blocks);
void R_SetFOV (FRenderViewpoint &viewpoint, DAngle fov);
void R_SetupFrame(FRenderViewpoint& viewPoint, const FViewWindow& viewWindow, AActor* const camera);
void R_SetViewAngle (FRenderViewpoint &viewpoint, const FViewWindow &viewwindow);

// Called by startup code.
void R_Init (void);
void R_ExecuteSetViewSize (FRenderViewpoint &viewpoint, FViewWindow &viewwindow);

// Called by M_Responder.
void R_SetViewSize (int blocks);
void R_SetWindow (FRenderViewpoint &viewpoint, FViewWindow &viewwindow, int windowSize, int fullWidth, int fullHeight, int stHeight, bool renderingToCanvas = false);

double R_GetGlobVis(const FViewWindow &viewwindow, double vis);
double R_ClampVisibility(double vis);

extern void R_FreePastViewers ();
extern void R_ClearPastViewer (AActor *actor);

bool R_ShouldDrawSpriteShadow(AActor *thing);

int WorldPaused(bool checkLag);

//==========================================================================
//
// [round2 B1] WHERE A TRACKED POSE IS RIGHT NOW -- one resolver for everything
// that can be held in a hand or worn on the head.
//
// Volumetric beam cones (hw_drawinfo.cpp, ResolveVolBeamPose) and dynamic
// lights (FDynamicLight::ResolvePoseAnchor) both call this, so a torch's cone
// and the light it throws on the wall can never disagree about where the hand
// is, including the yaw-90 and negated-pitch conventions described at the
// definition. Lives here rather than in a renderer header because the light
// tick (playsim side) needs it too, and r_utility is linked into both.
//
//   anchor  0 none, 1 main hand, 2 off hand, 3 head
//   offset  (forward, right, up) in map units in the pose's own yaw/pitch frame
//   who     whose pose; nullptr = the console player (what beams use)
//
// On success pos/yaw/pitch are written as plain Doom values -- yaw is world
// yaw, pitch is positive DOWN, the convention Actor.Angles and FDynamicLight's
// Yaw/Pitch use. On TPOSE_NONE and TPOSE_NOPLAYER nothing is written.
//
// READ-ONLY: no playsim state is written and no random number is drawn.
//
//==========================================================================

enum ETrackedPoseSource
{
	TPOSE_NONE,         // not anchored
	TPOSE_MAINHAND,     // AttackPos
	TPOSE_OFFHAND,      // OffhandPos
	TPOSE_HMD,          // HmdPos
	TPOSE_VIEW,         // head anchor, no headset pose written: the view
	TPOSE_NOPLAYER,     // anchored, but no player (or no pawn) to read
	TPOSE_COUNT
};

class player_t;

int ResolveTrackedPose(const FLevelLocals *Level, int anchor, const DVector3 &offset,
	DVector3 &pos, DAngle &yaw, DAngle &pitch, const player_t *who = nullptr);

// One line of text per ETrackedPoseSource, for the logs that say where a held
// thing is really reading its pose from.
const char *TrackedPoseSourceName(int source);

// The unit vector a pose looks along. Doom pitch is positive down, hence -sin.
inline DVector3 TrackedPoseForward(DAngle yaw, DAngle pitch)
{
	const double cp = pitch.Cos(), sp = pitch.Sin();
	return DVector3(cp * yaw.Cos(), cp * yaw.Sin(), -sp);
}

#endif
