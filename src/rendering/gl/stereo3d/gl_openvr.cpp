//
//---------------------------------------------------------------------------
//
// Copyright(C) 2016-2017 Christopher Bruns
// Copyright(C) 2020 Simon Brown
// Copyright(C) 2020 Krzysztof Marecki
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_openvr.cpp
** Stereoscopic virtual reality mode for the HTC Vive headset
**
*/

#ifdef USE_OPENVR

#include <string>
#include <map>
#include <cmath>
#include <cinttypes> // PRIu64 - used to be pulled in transitively via m_random.h -> sfmt/SFMTObj.h -> SFMT.h -> <inttypes.h>,
                     // which UZDoom 5.0.0 removed along with the SFMT RNG. Include it directly now.
#include "p_trace.h"
#include "p_linetracedata.h"
#include "gl_load/gl_system.h"
#include "doomtype.h" // Printf
#include "d_player.h"
#include "g_game.h" // G_Add...
#include "p_local.h" // P_TryMove
#include "gl_renderer.h"
#include "v_2ddrawer.h" // crosshair
#include "models.h"
#include "hw_material.h"
#include "hw_models.h"
#include "hw_renderstate.h"
#include "g_levellocals.h" // pixelstretch
#include "g_statusbar/sbar.h"
#include "c_cvars.h"
#include "c_console.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "LSMatrix.h"
#include "m_joy.h"
#include "d_gui.h"
#include "d_event.h"
#include "i_time.h"
#include "stats.h"
#include "hwrenderer/data/flatvertices.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "texturemanager.h"
#include "hwrenderer/scene/hw_weapon.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/data/hw_vrmodes.h"

#include "gl_openvr.h"
// #include "openvr_include.h"

EXTERN_CVAR(Int, developer);
#include <QzDoom/VrCommon.h>

using namespace openvr;
using namespace OpenGLRenderer;

static float RAD2DEG(float rad)
{
	return rad * float(180. / M_PI);
}

static float DEG2RAD(float deg)
{
	return deg * float(M_PI / 180.0);
}

namespace openvr {
#include "openvr.h"
}

void I_StartupOpenVR();
double P_XYMovement(AActor* mo, DVector2 scroll);
float I_OpenVRGetYaw();
float I_OpenVRGetPitch();
float I_OpenVRGetDirectionalMove();

float length(float x, float y);
float nonLinearFilter(float in);
double normalizeAngle(double angle);

void QzDoom_setUseScreenLayer(bool use);

bool VR_UseScreenLayer();
bool VR_UseCinematicScreenLayer();
void VR_GetMove( float *joy_forward, float *joy_side, float *hmd_forward, float *hmd_side, float *up, float *yaw, float *pitch, float *roll );
void VR_SetHMDOrientation(float pitch, float yaw, float roll );
void VR_SetHMDPosition(float x, float y, float z );
extern bool menu_allow_mouse_override;
class DMenu;
extern DMenu* CurrentMenu;

#ifdef DYN_OPENVR
// Dynamically load OpenVR

#include "i_module.h"
FModule OpenVRModule{ "OpenVR" };

/** Pointer-to-function type, useful for dynamically getting OpenVR entry points. */
// Derived from global entry at the bottom of openvr_capi.h, plus a few other functions
// typedef intptr_t(*LVR_InitInternal)(vr::EVRInitError* peError, vr::EVRApplicationType eType);
// typedef void (*LVR_ShutdownInternal)();
// typedef bool (*LVR_IsHmdPresent)();
// typedef intptr_t(*LVR_GetGenericInterface)(const char* pchInterfaceVersion, vr::EVRInitError* peError);
// typedef bool (*LVR_IsRuntimeInstalled)();
// typedef const char* (*LVR_GetVRInitErrorAsSymbol)(vr::EVRInitError error);
// typedef const char* (*LVR_GetVRInitErrorAsEnglishDescription)(vr::EVRInitError error);
// typedef bool (*LVR_IsInterfaceVersionValid)(const char* version);
// typedef uint32_t(*LVR_GetInitToken)();

// #define DEFINE_ENTRY(name) static TReqProc<OpenVRModule, L##name> name{#name};
// DEFINE_ENTRY(VR_InitInternal)
// DEFINE_ENTRY(VR_ShutdownInternal)
// DEFINE_ENTRY(VR_IsHmdPresent)
// DEFINE_ENTRY(VR_GetGenericInterface)
// DEFINE_ENTRY(VR_IsRuntimeInstalled)
// DEFINE_ENTRY(VR_GetVRInitErrorAsSymbol)
// DEFINE_ENTRY(VR_GetVRInitErrorAsEnglishDescription)
// DEFINE_ENTRY(VR_IsInterfaceVersionValid)
// DEFINE_ENTRY(VR_GetInitToken)

#ifdef _WIN32
#define OPENVRLIB "openvr_api.dll"
#elif defined(__APPLE__)
#define OPENVRLIB "libopenvr_api.dylib"
#else
#define OPENVRLIB "libopenvr_api.so"
#endif

#else
// Non-dynamic loading of OpenVR

// OpenVR Global entry points
S_API intptr_t VR_InitInternal(EVRInitError* peError, EVRApplicationType eType);
S_API void VR_ShutdownInternal();
S_API bool VR_IsHmdPresent();
S_API intptr_t VR_GetGenericInterface(const char* pchInterfaceVersion, EVRInitError* peError);
S_API bool VR_IsRuntimeInstalled();
S_API const char* VR_GetVRInitErrorAsSymbol(EVRInitError error);
S_API const char* VR_GetVRInitErrorAsEnglishDescription(EVRInitError error);
S_API bool VR_IsInterfaceVersionValid(const char* version);
S_API uint32_t VR_GetInitToken();

#endif

typedef float vec_t;
typedef vec_t vec3_t[3];

#define PITCH 0
#define YAW 1
#define ROLL 2

typedef enum control_scheme {
	RIGHT_HANDED_DEFAULT = 0,  // x,y,a,b - trigger,grip,joystick btn,thumb left/right - joystick axis left/right
    LEFT_HANDED_DEFAULT = 10,  // a,b,x,y - trigger,grip,joystick btn,thumb right/left - joystick axis right/left
    LEFT_HANDED_ALT = 11       // x,y,a,b - trigger,grip,joystick btn,thumb right/left - joystick axis left/right
} control_scheme_t;

extern vec3_t hmdPosition;
extern vec3_t hmdorientation;
extern vec3_t positionDeltaThisFrame;
extern vec3_t weaponoffset;
extern vec3_t weaponangles;
extern vec3_t offhandoffset;
extern vec3_t offhandangles;

extern float playerYaw;
extern float doomYaw;
extern float previousPitch;
extern float snapTurn;
extern float remote_movementSideways;
extern float remote_movementForward;
extern float positional_movementSideways;
extern float positional_movementForward;

extern bool ready_teleport;
extern bool trigger_teleport;
extern bool resetDoomYaw;
extern bool resetPreviousHmdYaw;
extern bool resetPreviousPitch;
extern bool cinemamode;
extern float cinemamodeYaw;
extern float cinemamodePitch;

double HmdHeight;

EXTERN_CVAR(Float, fov);
EXTERN_CVAR(Int, screenblocks);
EXTERN_CVAR(Float, movebob);
EXTERN_CVAR(Bool, gl_billboard_faces_camera);
EXTERN_CVAR(Int, gl_multisample);
EXTERN_CVAR(Int, vr_desktop_view);
EXTERN_CVAR(Float, vr_vunits_per_meter);
EXTERN_CVAR(Float, vr_height_adjust)
EXTERN_CVAR(Float, vr_ipd);
EXTERN_CVAR(Float, vr_weaponScale);
EXTERN_CVAR(Float, vr_weaponRotate);
EXTERN_CVAR(Int, vr_control_scheme);
EXTERN_CVAR(Bool, vr_move_use_offhand);
EXTERN_CVAR(Int, vr_joy_mode);

EXTERN_CVAR(Int, vr_overlayscreen);
EXTERN_CVAR(Bool, vr_overlayscreen_always);
EXTERN_CVAR(Float, vr_overlayscreen_size);
EXTERN_CVAR(Float, vr_overlayscreen_dist);
EXTERN_CVAR(Float, vr_overlayscreen_vpos);
EXTERN_CVAR(Int, vr_overlayscreen_bg);
EXTERN_CVAR(Bool, vr_menu_pointer);
EXTERN_CVAR(Bool, vr_mouse_in_menu);
EXTERN_CVAR(Color, vr_menu_pointer_color);
CVAR(Float, vr_openvr_menu_pointer_pitch_bias, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_openvr_menu_pointer_tip_offset, 0.035f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

EXTERN_CVAR(Bool, vr_secondary_button_mappings);
EXTERN_CVAR(Bool, vr_teleport);
EXTERN_CVAR(Bool, vr_switch_sticks);
EXTERN_CVAR(Bool, vr_two_handed_weapons);
EXTERN_CVAR(Int, vid_refreshrate);

EXTERN_CVAR(Bool, vr_enable_haptics);
EXTERN_CVAR(Bool, vr_crouch_use_button);
EXTERN_CVAR(Float, vr_snapTurn);

EXTERN_CVAR(Float, vr_2dweaponScale)
EXTERN_CVAR(Float, vr_2dweaponOffsetX);
EXTERN_CVAR(Float, vr_2dweaponOffsetY);
EXTERN_CVAR(Float, vr_2dweaponOffsetZ);

//HUD control
EXTERN_CVAR(Float, vr_hud_scale);
EXTERN_CVAR(Float, vr_hud_stereo);
EXTERN_CVAR(Float, vr_hud_distance);
EXTERN_CVAR(Float, vr_hud_rotate);
EXTERN_CVAR(Bool, vr_hud_fixed_pitch);
EXTERN_CVAR(Bool, vr_hud_fixed_roll);

//Mounted HUD
EXTERN_CVAR(Bool, vr_hud_mount);
EXTERN_CVAR(Int, vr_hud_mount_pos);
EXTERN_CVAR(Float, vr_hud_mount_scale);
EXTERN_CVAR(Float, vr_hud_mount_xoffset);
EXTERN_CVAR(Float, vr_hud_mount_yoffset);
EXTERN_CVAR(Float, vr_hud_mount_zoffset);
EXTERN_CVAR(Bool, vr_hud_mount_pitch);
EXTERN_CVAR(Float, vr_hud_mount_yaw);
EXTERN_CVAR(Bool, vr_hud_mount_roll);

//Automap  control
EXTERN_CVAR(Bool, vr_automap_use_hud);
EXTERN_CVAR(Float, vr_automap_scale);
EXTERN_CVAR(Float, vr_automap_stereo);
EXTERN_CVAR(Float, vr_automap_distance);
EXTERN_CVAR(Float, vr_automap_rotate);
EXTERN_CVAR(Bool, vr_automap_fixed_pitch);
EXTERN_CVAR(Bool, vr_automap_fixed_roll);

//Mounted Automap
EXTERN_CVAR(Bool, vr_automap_mount);
EXTERN_CVAR(Int, vr_automap_mount_pos);
EXTERN_CVAR(Float, vr_automap_mount_scale);
EXTERN_CVAR(Float, vr_automap_mount_xoffset);
EXTERN_CVAR(Float, vr_automap_mount_yoffset);
EXTERN_CVAR(Float, vr_automap_mount_zoffset);
EXTERN_CVAR(Bool, vr_automap_mount_pitch);
EXTERN_CVAR(Float, vr_automap_mount_yaw);
EXTERN_CVAR(Bool, vr_automap_mount_roll);
EXTERN_CVAR(Int, vr_automap_border);
EXTERN_CVAR(Color, vr_automap_border_color);

const float DEAD_ZONE = 0.25f;

bool IsOpenVRPresent()
{
#ifndef USE_OPENVR
	return false;
#elif !defined DYN_OPENVR
	return true;
#else
	static bool cached_result = false;
	static bool done = false;

	if (!done)
	{
		done = true;
		FString libname = NicePath("$PROGDIR/" OPENVRLIB);
		cached_result = OpenVRModule.Load({ libname.GetChars(), OPENVRLIB });
	}
	return cached_result;
#endif
}


//bit of a hack, assume player is at "normal" height when not crouching
static float getDoomPlayerHeightWithoutCrouch(const player_t* player)
{
	static float height = 0;
	if (!vr_crouch_use_button)
	{
		return HmdHeight;
	}
	if (height == 0)
	{
		// Doom thinks this is where you are
		//height = player->viewheight;
		height = player->DefaultViewHeight();
	}

	return height;
}

static float getViewpointYaw()
{
	if (VR_UseCinematicScreenLayer())
	{
		return cinemamodeYaw;
	}

	return doomYaw;
}

static void QueueMultiplayerTeleportTarget(const player_t* player, const DVector3& target)
{
	if (player == nullptr || player->mo == nullptr)
	{
		return;
	}

	VR_QueueMultiplayerTeleportTarget(VR_MakeCanonicalMultiplayerTeleportTarget(target.X, target.Y, target.Z, true));
}

// feature toggles, for testing and debugging
static const bool doTrackHmdYaw = true;
static const bool doTrackHmdPitch = true;
static const bool doTrackHmdRoll = true;
static const bool doLateScheduledRotationTracking = true;
static const bool doStereoscopicViewpointOffset = true;
static const bool doRenderToDesktop = true; // mirroring to the desktop is very helpful for debugging
static const bool doRenderToHmd = true;
static const bool doTrackHmdVerticalPosition = true;
static const bool doTrackHmdHorizontalPosition = true;
static const bool doTrackVrControllerPosition = false; // todo:

static int axisTrackpad = -1;
static int axisJoystick = -1;
static int axisTrigger = -1;
static bool identifiedAxes = false;

LSVec3 openvr_dpos(0, 0, 0);
DAngle openvr_to_doom_angle;

VROverlayHandle_t overlayHandle;
VROverlayHandle_t overlayBeamHandle;
VROverlayHandle_t overlayCursorHandle;
Texture_t* blankTexture;
Texture_t* beamTexture;
Texture_t* cursorTexture;
bool doTrackHmdAngles = true;
bool forceDisableOverlay = false;
int prevOverlayBG = -1;
int prevOverlayBackdropMode = -1;
bool openvrMenuPointerActive = false;
bool openvrMenuPointerBeamVisible = false;
bool openvrMenuPointerLastTriggerDown = false;
bool openvrMenuSuppressTriggerUntilRelease = false;
bool openvrMenuSuppressSelectAsKey = false;
DMenu* openvrLastMenuContext = nullptr;
float openvrMenuPointerLastX = 0.0f;
float openvrMenuPointerLastY = 0.0f;
double openvrMenuWheelCooldownUntil = 0.0;
bool openvrMenuWheelNeutral = true;
HmdVector3_t openvrMenuPointerBeamStart = { 0.0f, 0.0f, 0.0f };
HmdVector3_t openvrMenuPointerBeamEnd = { 0.0f, 0.0f, 0.0f };
HmdVector3_t openvrMenuPointerHit = { 0.0f, 0.0f, 0.0f };
HmdMatrix34_t openvrOverlayAbsTransform = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 1.5f,
	0.0f, 0.0f, 1.0f, -2.5f
};
bool openvrHasLatestHmdPose = false;
HmdMatrix34_t openvrLatestHmdPose = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f, 0.0f
};
bool openvrOverlayAnchorValid = false;
int openvrOverlayAnchorMode = -1;
bool openvrOverlayWasVisible = false;
HmdMatrix34_t openvrOverlayFollowCurrentTransform = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 1.5f,
	0.0f, 0.0f, 1.0f, -2.5f
};
HmdMatrix34_t openvrOverlayFollowTargetTransform = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 1.5f,
	0.0f, 0.0f, 1.0f, -2.5f
};
double openvrOverlayFollowNextTargetTime = 0.0;
double openvrOverlayFollowLastStepTime = 0.0;
bool openvrHadPrevHmdPoseForRecenter = false;
HmdMatrix34_t openvrPrevHmdPoseForRecenter = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f, 0.0f
};
HmdMatrix34_t openvrOverlayAnchorTransform = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 1.5f,
	0.0f, 0.0f, 1.0f, -2.5f
};
float overlayBG[6][3] = {
	{0.0f, 0.0f, 0.0f},
	{0.11f, 0.0f, 0.01f},
	{0.0f, 0.11f, 0.02f},
	{0.0f, 0.02f, 0.11f},
	{0.0f, 0.11f, 0.1f},
	{0.1f, 0.1f, 0.1f}
};

static float Dot3(const HmdVector3_t& a, const HmdVector3_t& b)
{
	return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2];
}

static HmdVector3_t Sub3(const HmdVector3_t& a, const HmdVector3_t& b)
{
	return HmdVector3_t{ a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2] };
}

static HmdVector3_t Add3(const HmdVector3_t& a, const HmdVector3_t& b)
{
	return HmdVector3_t{ a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2] };
}

static HmdVector3_t Scale3(const HmdVector3_t& v, const float s)
{
	return HmdVector3_t{ v.v[0] * s, v.v[1] * s, v.v[2] * s };
}

static HmdVector3_t Cross3(const HmdVector3_t& a, const HmdVector3_t& b)
{
	return HmdVector3_t{
		a.v[1] * b.v[2] - a.v[2] * b.v[1],
		a.v[2] * b.v[0] - a.v[0] * b.v[2],
		a.v[0] * b.v[1] - a.v[1] * b.v[0]
	};
}

static HmdVector3_t Normalize3(const HmdVector3_t& v)
{
	const float len = std::sqrt(std::max(0.0f, Dot3(v, v)));
	if (len <= 1e-6f)
		return HmdVector3_t{ 0.0f, 0.0f, 1.0f };
	return Scale3(v, 1.0f / len);
}

static HmdMatrix34_t Mul34(const HmdMatrix34_t& a, const HmdMatrix34_t& b)
{
	HmdMatrix34_t out = {};
	for (int r = 0; r < 3; ++r)
	{
		for (int c = 0; c < 3; ++c)
		{
			out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
		}
		out.m[r][3] = a.m[r][0] * b.m[0][3] + a.m[r][1] * b.m[1][3] + a.m[r][2] * b.m[2][3] + a.m[r][3];
	}
	return out;
}

static HmdVector3_t RotateAroundAxis(const HmdVector3_t& v, const HmdVector3_t& axisIn, float angleRad)
{
	const HmdVector3_t axis = Normalize3(axisIn);
	const float c = cosf(angleRad);
	const float s = sinf(angleRad);
	const HmdVector3_t term1 = Scale3(v, c);
	const HmdVector3_t term2 = Scale3(Cross3(axis, v), s);
	const HmdVector3_t term3 = Scale3(axis, Dot3(axis, v) * (1.0f - c));
	return Add3(Add3(term1, term2), term3);
}

static HmdMatrix34_t BuildOverlayRelativeTransform()
{
	const float overlayDrawDistance = -2.5f - vr_overlayscreen_dist;
	return HmdMatrix34_t{
		1.3f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, vr_overlayscreen_vpos,
		0.0f, 0.0f, 1.0f, overlayDrawDistance
	};
}

static HmdMatrix34_t BuildStationaryOverlayAnchorFromHmd()
{
	const float screenDistance = 2.5f + vr_overlayscreen_dist;
	const HmdVector3_t hmdPos = { openvrLatestHmdPose.m[0][3], openvrLatestHmdPose.m[1][3], openvrLatestHmdPose.m[2][3] };
	const HmdVector3_t worldUp = Normalize3({ 0.0f, 1.0f, 0.0f });
	HmdVector3_t forward = Normalize3({ -openvrLatestHmdPose.m[0][2], -openvrLatestHmdPose.m[1][2], -openvrLatestHmdPose.m[2][2] });
	// Stationary anchor should be upright in world space: preserve yaw only.
	forward.v[1] = 0.0f;
	if (Dot3(forward, forward) < 1e-6f)
	{
		forward = Normalize3({ -openvrLatestHmdPose.m[0][0], 0.0f, -openvrLatestHmdPose.m[2][0] });
	}
	forward = Normalize3(forward);
	const HmdVector3_t normal = Scale3(forward, -1.0f);
	HmdVector3_t right = Normalize3(Cross3(worldUp, normal));
	if (Dot3(right, right) < 1e-6f)
	{
		right = Normalize3({ 1.0f, 0.0f, 0.0f });
	}
	const HmdVector3_t up = worldUp;
	right = Scale3(right, 1.3f);
	const HmdVector3_t center = Add3(Add3(hmdPos, Scale3(forward, screenDistance)), Scale3(worldUp, vr_overlayscreen_vpos));
	return HmdMatrix34_t{
		right.v[0], up.v[0], normal.v[0], center.v[0],
		right.v[1], up.v[1], normal.v[1], center.v[1],
		right.v[2], up.v[2], normal.v[2], center.v[2]
	};
}

static float YawFromPoseDeg(const HmdMatrix34_t& pose)
{
	HmdVector3_t forward = Normalize3({ -pose.m[0][2], -pose.m[1][2], -pose.m[2][2] });
	forward.v[1] = 0.0f;
	forward = Normalize3(forward);
	return RAD2DEG(atan2f(forward.v[0], forward.v[2]));
}

static float ShortestAngleDeltaDeg(float a, float b)
{
	float d = fmodf(a - b, 360.0f);
	if (d > 180.0f) d -= 360.0f;
	if (d < -180.0f) d += 360.0f;
	return d;
}

static float YawFromOverlayTransformDeg(const HmdMatrix34_t& xf)
{
	const HmdVector3_t forward = Normalize3({ -xf.m[0][2], -xf.m[1][2], -xf.m[2][2] });
	return RAD2DEG(atan2f(forward.v[0], forward.v[2]));
}

static HmdMatrix34_t OrthonormalizeOverlayTransform(const HmdMatrix34_t& in)
{
	HmdVector3_t right = { in.m[0][0], in.m[1][0], in.m[2][0] };
	HmdVector3_t up = { in.m[0][1], in.m[1][1], in.m[2][1] };
	HmdVector3_t normal = { in.m[0][2], in.m[1][2], in.m[2][2] };
	const float rightLen = std::sqrt(std::max(0.0f, Dot3(right, right)));
	normal = Normalize3(normal);
	right = Normalize3(Cross3(up, normal));
	if (Dot3(right, right) < 1e-6f)
	{
		right = Normalize3({ 1.0f, 0.0f, 0.0f });
	}
	up = Normalize3(Cross3(normal, right));
	right = Scale3(right, std::max(0.1f, rightLen));
	return HmdMatrix34_t{
		right.v[0], up.v[0], normal.v[0], in.m[0][3],
		right.v[1], up.v[1], normal.v[1], in.m[1][3],
		right.v[2], up.v[2], normal.v[2], in.m[2][3]
	};
}

static HmdMatrix34_t BuildControllerFollowOverlayAnchor(const HmdMatrix34_t& controllerPose)
{
	const float screenDistance = 2.5f + vr_overlayscreen_dist;
	const HmdVector3_t controllerPos = { controllerPose.m[0][3], controllerPose.m[1][3], controllerPose.m[2][3] };
	HmdVector3_t forward = Normalize3({ -controllerPose.m[0][2], -controllerPose.m[1][2], -controllerPose.m[2][2] });
	const HmdVector3_t controllerRight = Normalize3({ controllerPose.m[0][0], controllerPose.m[1][0], controllerPose.m[2][0] });
	// Match expected controller-follow feel by pitching the overlay downward.
	forward = Normalize3(RotateAroundAxis(forward, controllerRight, -45.0f * (float)(M_PI / 180.0)));
	const HmdVector3_t normal = Scale3(forward, -1.0f);
	HmdVector3_t upRef = Normalize3({ 0.0f, 1.0f, 0.0f });
	if (openvrHasLatestHmdPose)
	{
		upRef = Normalize3({ openvrLatestHmdPose.m[0][1], openvrLatestHmdPose.m[1][1], openvrLatestHmdPose.m[2][1] });
	}
	// Build an orthonormal frame to avoid shear/trapezoid distortion when controller rolls.
	HmdVector3_t right = Normalize3(Cross3(upRef, normal));
	if (Dot3(right, right) < 1e-6f)
	{
		right = Normalize3({ controllerPose.m[0][0], controllerPose.m[1][0], controllerPose.m[2][0] });
	}
	HmdVector3_t up = Normalize3(Cross3(normal, right));
	right = Scale3(right, 1.3f);
	const HmdVector3_t center = Add3(Add3(controllerPos, Scale3(forward, screenDistance)), Scale3(up, vr_overlayscreen_vpos));
	return HmdMatrix34_t{
		right.v[0], up.v[0], normal.v[0], center.v[0],
		right.v[1], up.v[1], normal.v[1], center.v[1],
		right.v[2], up.v[2], normal.v[2], center.v[2]
	};
}

static void PostGuiMouseEvent(EGUIEvent type, int x, int y)
{
	event_t ev = {};
	ev.type = EV_GUI_Event;
	ev.subtype = type;
	ev.data1 = x;
	ev.data2 = y;
	D_PostEvent(&ev);
}

static void PostGuiWheelEvent(EGUIEvent type, int x, int y)
{
	event_t ev = {};
	ev.type = EV_GUI_Event;
	ev.subtype = type;
	ev.data1 = x;
	ev.data2 = y;
	D_PostEvent(&ev);
}

namespace s3d
{
	static LSVec3 openvr_origin(0, 0, 0);
	static float deltaYawDegrees;

	class FControllerTexture : public FTexture
	{
	public:
		FControllerTexture(RenderModel_TextureMap_t* tex) : FTexture()
		{
			m_pTex = tex;
			Width = m_pTex->unWidth;
			Height = m_pTex->unHeight;
		}

		/*const uint8_t *GetColumn(FRenderStyle style, unsigned int column, const Span **spans_out)
		{
			return nullptr;
		}*/
		const uint8_t* GetPixels(FRenderStyle style)
		{
			return m_pTex->rubTextureMapData;
		}

		RenderModel_TextureMap_t* m_pTex;
	};

	class VRControllerModel : public FModel
	{
	public:
		enum LoadState {
			LOADSTATE_INITIAL,
			LOADSTATE_LOADING_VERTICES,
			LOADSTATE_LOADING_TEXTURE,
			LOADSTATE_LOADED,
			LOADSTATE_ERROR
		};

		VRControllerModel(const std::string& model_name, VR_IVRRenderModels_FnTable* vrRenderModels)
			: loadState(LOADSTATE_INITIAL)
			, modelName(model_name)
			, vrRenderModels(vrRenderModels)
		{
			if (!vrRenderModels) {
				loadState = LOADSTATE_ERROR;
				return;
			}
			isLoaded();
		}
		VRControllerModel() {}

		// FModel methods

		virtual bool Load(const char* fn, int lumpnum, const char* buffer, int length) override {
			return false;
		}

		// Controller models don't have frames so always return 0
		virtual int FindFrame(const char* name, bool nodefault) override {
			return 0;
		}

		virtual void RenderFrame(FModelRenderer* renderer, FGameTexture* skin, int frame, int frame2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, int boneStartPosition, const FModelSurfaceOverrideList* surfov) override
		{
			if (!isLoaded())
				return;
			FMaterial* tex = FMaterial::ValidateTexture(pFTex, false, false);
			auto vbuf = GetVertexBuffer(renderer->GetType());
			renderer->SetupFrame(this, 0, 0, 0, -1);
			renderer->SetMaterial(pFTex, CLAMP_NONE, translation);
			renderer->DrawElements(pModel->unTriangleCount * 3, 0);
		}

		virtual void BuildVertexBuffer(FModelRenderer* renderer) override
		{
			if (loadState != LOADSTATE_LOADED)
				return;

			auto vbuf = GetVertexBuffer(renderer->GetType());
			if (vbuf != NULL)
				return;

			vbuf = new FModelVertexBuffer(true, true);
			FModelVertex* vertptr = vbuf->LockVertexBuffer(pModel->unVertexCount);
			unsigned int* indxptr = vbuf->LockIndexBuffer(pModel->unTriangleCount * 3);

			for (int v = 0; v < pModel->unVertexCount; ++v)
			{
				const RenderModel_Vertex_t& vd = pModel->rVertexData[v];
				vertptr[v].x = vd.vPosition.v[0];
				vertptr[v].y = vd.vPosition.v[1];
				vertptr[v].z = vd.vPosition.v[2];
				vertptr[v].u = vd.rfTextureCoord[0];
				vertptr[v].v = vd.rfTextureCoord[1];
				vertptr[v].SetNormal(
					vd.vNormal.v[0],
					vd.vNormal.v[1],
					vd.vNormal.v[2]);
			}
			for (int i = 0; i < pModel->unTriangleCount * 3; ++i)
			{
				indxptr[i] = pModel->rIndexData[i];
			}

			vbuf->UnlockVertexBuffer();
			vbuf->UnlockIndexBuffer();
			SetVertexBuffer(renderer->GetType(), vbuf);
		}

		virtual void AddSkins(uint8_t* hitlist, const FTextureID* surfaceskinids)  override
		{

		}

		bool isLoaded()
		{
			if (loadState == LOADSTATE_ERROR)
				return false;
			if (loadState == LOADSTATE_LOADED)
				return true;
			if ((loadState == LOADSTATE_INITIAL) || (loadState == LOADSTATE_LOADING_VERTICES))
			{
				// Load vertex data first
				EVRRenderModelError eError = vrRenderModels->LoadRenderModel_Async(const_cast<char*>(modelName.c_str()), &pModel);
				if (eError == EVRRenderModelError_VRRenderModelError_Loading) {
					loadState = LOADSTATE_LOADING_VERTICES;
					return false;
				}
				else if (eError == EVRRenderModelError_VRRenderModelError_None) {
					loadState = LOADSTATE_LOADING_TEXTURE;
					vrRenderModels->LoadTexture_Async(pModel->diffuseTextureId, &pTexture);
				}
				else {
					loadState = LOADSTATE_ERROR;
					return false;
				}
			}
			// Load texture data second
			EVRRenderModelError eError = vrRenderModels->LoadTexture_Async(pModel->diffuseTextureId, &pTexture);
			if (eError == EVRRenderModelError_VRRenderModelError_Loading) {
				return false; // No change, and not done, still loading texture
			}
			if (eError == EVRRenderModelError_VRRenderModelError_None) {
				loadState = LOADSTATE_LOADED;

				auto tex = new FControllerTexture(pTexture);
				pFTex = MakeGameTexture(tex, "Controllers", ::ETextureType::Any);
				auto& renderState = *screen->RenderState();
				auto* di = HWDrawInfo::StartDrawInfo(r_viewpoint.ViewLevel, nullptr, r_viewpoint, nullptr);
				FHWModelRenderer renderer(di, renderState, -1);
				BuildVertexBuffer(&renderer);
				di->EndDrawInfo();
				return true;
			}
			loadState = LOADSTATE_ERROR;
			return false;
		}

	private:
		RenderModel_t* pModel;
		RenderModel_TextureMap_t* pTexture;
		FGameTexture* pFTex;
		LoadState loadState;
		std::string modelName;
		VR_IVRRenderModels_FnTable* vrRenderModels;

	};



	OpenVRHaptics::OpenVRHaptics(openvr::VR_IVRSystem_FnTable* vrSystem)
		: vrSystem(vrSystem)
	{
		controllerIDs[0] = openvr::k_unTrackedDeviceIndexInvalid;
		controllerIDs[1] = openvr::k_unTrackedDeviceIndexInvalid;
	}

	void OpenVRHaptics::Vibrate(float duration, int channel, float intensity)
	{
		if (vibration_channel_duration[channel] > 0.0f)
			return;

		if (vibration_channel_duration[channel] == -1.0f && duration != 0.0f)
			return;

		vibration_channel_duration[channel] = duration;
		vibration_channel_intensity[channel] = intensity;
	}

	void OpenVRHaptics::UpdateControllerId(int role, openvr::TrackedDeviceIndex_t deviceIndex)
	{
		if (role < 0 || role >= 2)
			return;
		controllerIDs[role] = deviceIndex;
	}

	using namespace std::chrono;
	void  OpenVRHaptics::ProcessHaptics()
	{
		if (!vr_enable_haptics) {
			return;
		}

		static double lastFrameTime = 0.0f;
		double timestamp = (duration_cast<milliseconds>(
			system_clock::now().time_since_epoch())).count();
		double frametime = timestamp - lastFrameTime;
		lastFrameTime = timestamp;

		for (int i = 0; i < 2; ++i) {
			if (controllerIDs[i] == openvr::k_unTrackedDeviceIndexInvalid || vrSystem == nullptr) {
				vibration_channel_duration[i] = 0.0f;
				vibration_channel_intensity[i] = 0.0f;
				continue;
			}

			if (vibration_channel_duration[i] > 0.0f ||
				vibration_channel_duration[i] == -1.0f) {

				vrSystem->TriggerHapticPulse(controllerIDs[i], 0, 3999 * vibration_channel_intensity[i]);

				if (vibration_channel_duration[i] != -1.0f) {
					vibration_channel_duration[i] -= frametime;

					if (vibration_channel_duration[i] < 0.0f) {
						vibration_channel_duration[i] = 0.0f;
						vibration_channel_intensity[i] = 0.0f;
					}
				}
			}
			else {
				vrSystem->TriggerHapticPulse(controllerIDs[i], 0, 0);
			}
		}
	}


	static std::map<std::string, VRControllerModel> controllerMeshes;

	struct Controller
	{
		bool active = false;
		TrackedDeviceIndex_t index;
		TrackedDevicePose_t pose;
		VRControllerState_t lastState;
		VRControllerModel* model = nullptr;
	};

	enum { MAX_ROLES = 2 };
	Controller controllers[MAX_ROLES];

	static HmdVector3d_t eulerAnglesFromQuat(HmdQuaternion_t quat) {
		double q0 = quat.w;
		// permute axes to make "Y" up/yaw
		double q2 = quat.x;
		double q3 = quat.y;
		double q1 = quat.z;

		// http://stackoverflow.com/questions/18433801/converting-a-3x3-matrix-to-euler-tait-bryan-angles-pitch-yaw-roll
		double roll = atan2(2 * (q0 * q1 + q2 * q3), 1 - 2 * (q1 * q1 + q2 * q2));
		double pitch = asin(2 * (q0 * q2 - q3 * q1));
		double yaw = atan2(2 * (q0 * q3 + q1 * q2), 1 - 2 * (q2 * q2 + q3 * q3));

		return HmdVector3d_t{ yaw, pitch, roll };
	}

	static HmdQuaternion_t quatFromMatrix(HmdMatrix34_t matrix) {
		HmdQuaternion_t q;
		typedef float f34[3][4];
		f34& a = matrix.m;
		// http://www.euclideanspace.com/maths/geometry/rotations/conversions/matrixToQuaternion/
		float trace = a[0][0] + a[1][1] + a[2][2]; // I removed + 1.0f; see discussion with Ethan
		if (trace > 0) {// I changed M_EPSILON to 0
			float s = 0.5f / sqrtf(trace + 1.0f);
			q.w = 0.25f / s;
			q.x = (a[2][1] - a[1][2]) * s;
			q.y = (a[0][2] - a[2][0]) * s;
			q.z = (a[1][0] - a[0][1]) * s;
		}
		else {
			if (a[0][0] > a[1][1] && a[0][0] > a[2][2]) {
				float s = 2.0f * sqrtf(1.0f + a[0][0] - a[1][1] - a[2][2]);
				q.w = (a[2][1] - a[1][2]) / s;
				q.x = 0.25f * s;
				q.y = (a[0][1] + a[1][0]) / s;
				q.z = (a[0][2] + a[2][0]) / s;
			}
			else if (a[1][1] > a[2][2]) {
				float s = 2.0f * sqrtf(1.0f + a[1][1] - a[0][0] - a[2][2]);
				q.w = (a[0][2] - a[2][0]) / s;
				q.x = (a[0][1] + a[1][0]) / s;
				q.y = 0.25f * s;
				q.z = (a[1][2] + a[2][1]) / s;
			}
			else {
				float s = 2.0f * sqrtf(1.0f + a[2][2] - a[0][0] - a[1][1]);
				q.w = (a[1][0] - a[0][1]) / s;
				q.x = (a[0][2] + a[2][0]) / s;
				q.y = (a[1][2] + a[2][1]) / s;
				q.z = 0.25f * s;
			}
		}

		return q;
	}

	static HmdVector3d_t eulerAnglesFromMatrix(HmdMatrix34_t mat) {
		return eulerAnglesFromQuat(quatFromMatrix(mat));
	}

	// rotate quat by pitch
	// https://stackoverflow.com/questions/4436764/rotating-a-quaternion-on-1-axis/34805024#34805024
	HmdQuaternion_t makeQuat(float x, float y, float z, float w) {
		HmdQuaternion_t quat = { x,y,z,w };
		return quat;
	}
	float dot(HmdQuaternion_t a)
	{
		return (((a.x * a.x) + (a.y * a.y)) + (a.z * a.z)) + (a.w * a.w);
	}
	HmdQuaternion_t normalizeQuat(HmdQuaternion_t q)
	{
		float num = dot(q);
		float inv = 1.0f / (sqrtf(num));
		return makeQuat(q.x * inv, q.y * inv, q.z * inv, q.w * inv);
	}
	HmdQuaternion_t createQuatfromAxisAngle(const float& xx, const float& yy, const float& zz, const float& a)
	{
		// Here we calculate the sin( theta / 2) once for optimization
		float factor = sinf(a / 2.0f);

		HmdQuaternion_t quat;
		// Calculate the x, y and z of the quaternion
		quat.x = xx * factor;
		quat.y = yy * factor;
		quat.z = zz * factor;

		// Calcualte the w value by cos( theta / 2 )
		quat.w = cosf(a / 2.0f);
		return normalizeQuat(quat);
	}
	// https://www.euclideanspace.com/maths/algebra/realNormedAlgebra/quaternions/code/index.htm
	static HmdQuaternion_t multiplyQuat(HmdQuaternion_t q1, HmdQuaternion_t q2) {
		HmdQuaternion_t q;
		q.x = q1.x * q2.w + q1.y * q2.z - q1.z * q2.y + q1.w * q2.x;
		q.y = -q1.x * q2.z + q1.y * q2.w + q1.z * q2.x + q1.w * q2.y;
		q.z = q1.x * q2.y - q1.y * q2.x + q1.z * q2.w + q1.w * q2.z;
		q.w = -q1.x * q2.x - q1.y * q2.y - q1.z * q2.z + q1.w * q2.w;
		return q;
	}

	static HmdVector3d_t eulerAnglesFromQuatPitchRotate(HmdQuaternion_t quat, float pitch) {
		HmdQuaternion_t qRot = createQuatfromAxisAngle(0, 0, 1, -pitch * (3.14159f / 180.0f));
		HmdQuaternion_t q = multiplyQuat(quat, qRot);
		return eulerAnglesFromQuat(q);
	}
	static HmdVector3d_t eulerAnglesFromMatrixPitchRotate(HmdMatrix34_t mat, float pitch) {
		return eulerAnglesFromQuatPitchRotate(quatFromMatrix(mat), pitch);
	}

	OpenVREyePose::OpenVREyePose(int eye, float shiftFactor, float scaleFactor)
		: VREyeInfo(0.0f, 1.f)
		, eye(eye)
		, eyeTexture(nullptr)
		, currentPose(nullptr)
	{
	}


	/* virtual */
	OpenVREyePose::~OpenVREyePose()
	{
		dispose();
	}

	static void vSMatrixFromHmdMatrix34(VSMatrix& m1, const HmdMatrix34_t& m2)
	{
		float tmp[16];
		for (int i = 0; i < 3; ++i) {
			for (int j = 0; j < 4; ++j) {
				tmp[4 * i + j] = m2.m[i][j];
			}
		}
		int i = 3;
		for (int j = 0; j < 4; ++j) {
			tmp[4 * i + j] = 0;
		}
		tmp[15] = 1;
		m1.loadMatrix(&tmp[0]);
	}


	/* virtual */
	DVector3 OpenVREyePose::GetViewShift(FRenderViewpoint& vp) const
	{

		if (currentPose == nullptr)
			return { 0, 0, 0 };
		const TrackedDevicePose_t& hmd = *currentPose;
		if (!hmd.bDeviceIsConnected)
			return { 0, 0, 0 };
		if (!hmd.bPoseIsValid)
			return { 0, 0, 0 };

		if (!doStereoscopicViewpointOffset)
			return { 0, 0, 0 };

		const HmdMatrix34_t& hmdPose = hmd.mDeviceToAbsoluteTracking;

		// Pitch and Roll are identical between OpenVR and Doom worlds.
		// But yaw can differ, depending on starting state, and controller movement.
		float doomYawDegrees = vp.HWAngles.Yaw.Degrees();
		float openVrYawDegrees = RAD2DEG(-eulerAnglesFromMatrix(hmdPose).v[0]);
		deltaYawDegrees = doomYawDegrees - openVrYawDegrees;
		while (deltaYawDegrees > 180)
			deltaYawDegrees -= 360;
		while (deltaYawDegrees < -180)
			deltaYawDegrees += 360;

		openvr_to_doom_angle = DAngle::fromDeg(-deltaYawDegrees);

		// extract rotation component from hmd transform
		LSMatrix44 openvr_X_hmd(hmdPose);
		LSMatrix44 hmdRot = openvr_X_hmd.getWithoutTranslation(); // .transpose();

		/// In these eye methods, just get local inter-eye stereoscopic shift, not full position shift ///

		// compute local eye shift
		LSMatrix44 eyeShift2;
		eyeShift2.loadIdentity();
		eyeShift2 = eyeShift2 * eyeToHeadTransform; // eye to head
		eyeShift2 = eyeShift2 * hmdRot; // head to openvr

		LSVec3 eye_EyePos = LSVec3(0, 0, 0); // eye position in eye frame
		LSVec3 hmd_EyePos = LSMatrix44(eyeToHeadTransform) * eye_EyePos;
		LSVec3 hmd_HmdPos = LSVec3(0, 0, 0); // hmd position in hmd frame
		LSVec3 openvr_EyePos = openvr_X_hmd * hmd_EyePos;
		LSVec3 openvr_HmdPos = openvr_X_hmd * hmd_HmdPos;
		LSVec3 hmd_OtherEyePos = LSMatrix44(otherEyeToHeadTransform) * eye_EyePos;
		LSVec3 openvr_OtherEyePos = openvr_X_hmd * hmd_OtherEyePos;
		LSVec3 openvr_EyeOffset = openvr_EyePos - openvr_HmdPos;

		VSMatrix doomInOpenVR = VSMatrix();
		doomInOpenVR.loadIdentity();
		// permute axes
		float permute[] = { // Convert from OpenVR to Doom axis convention, including mirror inversion
			-1,  0,  0,  0, // X-right in OpenVR -> X-left in Doom
				0,  0,  1,  0, // Z-backward in OpenVR -> Y-backward in Doom
				0,  1,  0,  0, // Y-up in OpenVR -> Z-up in Doom
				0,  0,  0,  1 };
		doomInOpenVR.multMatrix(permute);
		doomInOpenVR.scale(VR_UnitsPerMeter(), VR_UnitsPerMeter(), VR_UnitsPerMeter()); // Doom units are not meters
		double pixelstretch = level.info ? level.info->pixelstretch : 1.2;
		doomInOpenVR.scale(pixelstretch, pixelstretch, 1.0); // Doom universe is scaled by 1990s pixel aspect ratio
		doomInOpenVR.rotate(deltaYawDegrees, 0, 0, 1);

		LSVec3 doom_EyeOffset = LSMatrix44(doomInOpenVR) * openvr_EyeOffset;

		if (doTrackHmdVerticalPosition) {
			// In OpenVR, the real world floor level is at y==0
			// In Doom, the virtual player foot level is viewheight below the current viewpoint (on the Z axis)
			// We want to align those two heights here
			const player_t& player = players[consoleplayer];
			double vh = getDoomPlayerHeightWithoutCrouch(&player); // Doom thinks this is where you are
			double hh = ((openvr_X_hmd[1][3] + vr_height_adjust) * VR_UnitsPerMeter()) / pixelstretch; // HMD is actually here
			HmdHeight = hh;
			doom_EyeOffset[2] += hh - vh;
			// TODO: optionally allow player to jump and crouch by actually jumping and crouching
		}

		if (doTrackHmdHorizontalPosition) {
			if (!multiplayer)
			{
				// shift viewpoint when hmd position shifts
				static bool is_initial_origin_set = false;
				if (!is_initial_origin_set) {
					// initialize origin to first noted HMD location
					// TODO: implement recentering based on a CCMD
					openvr_origin = openvr_HmdPos;
					is_initial_origin_set = true;
				}
				openvr_dpos = openvr_HmdPos - openvr_origin;

				LSVec3 doom_dpos = LSMatrix44(doomInOpenVR) * openvr_dpos;
				doom_EyeOffset[0] += doom_dpos[0];
				doom_EyeOffset[1] += doom_dpos[1];
			}
		}

		return { doom_EyeOffset[0], doom_EyeOffset[1], doom_EyeOffset[2] };
	}

	/* virtual */
	VSMatrix OpenVREyePose::GetProjection(FLOATTYPE fov, FLOATTYPE aspectRatio, FLOATTYPE fovRatio, bool iso_ortho) const
	{
		// Ignore those arguments and get the projection from the SDK
		// VSMatrix vs1 = ShiftedEyePose::GetProjection(fov, aspectRatio, fovRatio);
		return projectionMatrix;
	}

	DAngle OpenVREyePose::GetRenderFov(DAngle fallback) const
	{
		return renderFovDegrees > 0.0f ? DAngle::fromDeg(renderFovDegrees) : fallback;
	}

	void OpenVREyePose::initialize(VR_IVRSystem_FnTable* vrsystem)
	{
		float zNear = screen->GetZNear(); // 5.0;
		float zFar = screen->GetZFar(); // 65536.0;
		HmdMatrix44_t projection = vrsystem->GetProjectionMatrix(
			EVREye(eye), zNear, zFar);
		HmdMatrix44_t proj_transpose;
		for (int i = 0; i < 4; ++i) {
			for (int j = 0; j < 4; ++j) {
				proj_transpose.m[i][j] = projection.m[j][i];
			}
		}
		projectionMatrix.loadIdentity();
		projectionMatrix.multMatrix(&proj_transpose.m[0][0]);

		const float horizontalFov = 2.0f * RAD2DEG(atanf(1.0f / projection.m[0][0]));
		const float verticalFov = 2.0f * RAD2DEG(atanf(1.0f / projection.m[1][1]));
		renderFovDegrees = std::max(horizontalFov, verticalFov);
		fov = verticalFov;

		HmdMatrix34_t eyeToHead = vrsystem->GetEyeToHeadTransform(EVREye(eye));
		vSMatrixFromHmdMatrix34(eyeToHeadTransform, eyeToHead);
		HmdMatrix34_t otherEyeToHead = vrsystem->GetEyeToHeadTransform(eye == EVREye_Eye_Left ? EVREye_Eye_Right : EVREye_Eye_Left);
		vSMatrixFromHmdMatrix34(otherEyeToHeadTransform, otherEyeToHead);

		if (eyeTexture == nullptr)
			eyeTexture = new Texture_t();
		eyeTexture->handle = nullptr; // TODO: populate this at resolve time
		eyeTexture->eType = ETextureType_TextureType_OpenGL;
		eyeTexture->eColorSpace = EColorSpace_ColorSpace_Linear;
	}

	void OpenVREyePose::dispose()
	{
		if (eyeTexture) {
			delete eyeTexture;
			eyeTexture = nullptr;
		}
	}

	bool OpenVREyePose::submitFrame(VR_IVRCompositor_FnTable* vrCompositor, VR_IVROverlay_FnTable* vrOverlay) const
	{
		if (eyeTexture == nullptr)
			return false;
		if (vrCompositor == nullptr)
			return false;

		// Copy HDR game texture to local vr LDR framebuffer, so gamma correction could work
		if (eyeTexture->handle == nullptr) {
			glGenFramebuffers(1, &framebuffer);
			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

			GLuint texture;
			glGenTextures(1, &texture);
			eyeTexture->handle = (void*)(std::ptrdiff_t)texture;
			glBindTexture(GL_TEXTURE_2D, texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, screen->mSceneViewport.width,
				screen->mSceneViewport.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
			GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
			glDrawBuffers(1, drawBuffers);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			return false;
		GLRenderer->mBuffers->BindEyeTexture(eye, 0);
		IntRect box = { 0, 0, screen->mSceneViewport.width, screen->mSceneViewport.height };
		GLRenderer->DrawPresentTexture(box, true);

		// Maybe this would help with AMD boards?
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);

		static VRTextureBounds_t tBounds = { 0, 0, 1, 1 };

		const bool renderNetWaitShell = VR_IsNetWaitShellActive();
		const bool showOverlay = renderNetWaitShell || (ConsoleState != c_up) || (!forceDisableOverlay && VR_UseScreenLayer());
		if (!showOverlay)
		{
			//clear and hide overlay when not in use
			vrOverlay->ClearOverlayTexture(overlayHandle);
			vrOverlay->HideOverlay(overlayHandle);
			if (overlayBeamHandle != 0)
			{
				vrOverlay->HideOverlay(overlayBeamHandle);
			}
			if (overlayCursorHandle != 0)
			{
				vrOverlay->HideOverlay(overlayCursorHandle);
			}
			vrCompositor->Submit(EVREye(eye), eyeTexture, &tBounds, EVRSubmitFlags_Submit_Default);
		}
		else {
			// create a solid color backdrop texture
			const bool useBlackBackdrop = renderNetWaitShell || (gamestate == GS_STARTUP || gamestate == GS_DEMOSCREEN || gamestate == GS_INTRO || gamestate == GS_TITLELEVEL);
			const int currentBackdropMode = useBlackBackdrop ? 0 : 1;
			if (prevOverlayBG != vr_overlayscreen_bg || prevOverlayBackdropMode != currentBackdropMode) {
				prevOverlayBG = vr_overlayscreen_bg;
				prevOverlayBackdropMode = currentBackdropMode;
				blankTexture = new Texture_t();
				blankTexture->handle = nullptr;
				blankTexture->eType = ETextureType_TextureType_OpenGL;
				blankTexture->eColorSpace = EColorSpace_ColorSpace_Linear;
				int tWidth = screen->mSceneViewport.width;
				int tHeight = screen->mSceneViewport.height;
				std::vector<GLubyte> emptyDataStart(screen->mSceneViewport.width * screen->mSceneViewport.height * 4, 0);
				unsigned char* emptyData = new unsigned char[3 * tWidth * tHeight * sizeof(unsigned char)];
				for (unsigned int i = 0; i < tWidth * tHeight; i++)
				{
					emptyData[i * 3] = (unsigned char)(overlayBG[vr_overlayscreen_bg][0] * 255.0f);
					emptyData[i * 3 + 1] = (unsigned char)(overlayBG[vr_overlayscreen_bg][1] * 255.0f);
					emptyData[i * 3 + 2] = (unsigned char)(overlayBG[vr_overlayscreen_bg][2] * 255.0f);
				}
				GLuint emptyTextureID;
				glGenTextures(1, &emptyTextureID);
				blankTexture->handle = (void*)(std::ptrdiff_t)emptyTextureID;
				glBindTexture(GL_TEXTURE_2D, emptyTextureID);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				if (useBlackBackdrop)
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tWidth, tHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, &emptyDataStart[0]);
				else
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tWidth, tHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, emptyData);
				glGenerateMipmap(GL_TEXTURE_2D);
				delete[] emptyData;
			}
				
			//static VRTextureBounds_t oBounds = { 0, 0.05, 0.8, 0.95 }; // screen texture crop for overlay

			// set screen texture on overly instead of compositor
			vrCompositor->Submit(EVREye(eye), blankTexture, &tBounds, EVRSubmitFlags_Submit_Default);
				vrOverlay->SetOverlayTexture(overlayHandle, eyeTexture);
				vrOverlay->SetOverlayTextureBounds(overlayHandle, &tBounds);
				vrOverlay->SetOverlayWidthInMeters(overlayHandle, (1.0f + vr_overlayscreen_size) * 0.8f);
				vrOverlay->ShowOverlay(overlayHandle);

				if (eye == 0 && vrOverlay != nullptr && overlayBeamHandle != 0 && openvrMenuPointerBeamVisible)
				{
				if (beamTexture == nullptr)
				{
					beamTexture = new Texture_t();
					beamTexture->eType = ETextureType_TextureType_OpenGL;
					beamTexture->eColorSpace = EColorSpace_ColorSpace_Linear;
					beamTexture->handle = nullptr;
				}
				if (beamTexture->handle == nullptr)
				{
					GLuint beamTextureID = 0;
					glGenTextures(1, &beamTextureID);
					beamTexture->handle = (void*)(std::ptrdiff_t)beamTextureID;
					glBindTexture(GL_TEXTURE_2D, beamTextureID);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					const unsigned char white[4] = { 255, 255, 255, 255 };
					glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
					glBindTexture(GL_TEXTURE_2D, 0);
				}

				HmdVector3_t beamVec = Sub3(openvrMenuPointerBeamEnd, openvrMenuPointerBeamStart);
				const float beamLen = std::sqrt(std::max(0.0f, Dot3(beamVec, beamVec)));
					if (beamLen > 0.01f)
					{
						const HmdVector3_t xAxis = Normalize3(beamVec);
						HmdVector3_t upHint = HmdVector3_t{ 0.0f, 1.0f, 0.0f };
						HmdVector3_t yAxis = Normalize3(Cross3(upHint, xAxis));
						if (Dot3(yAxis, yAxis) < 1e-6f)
						{
							upHint = HmdVector3_t{ 1.0f, 0.0f, 0.0f };
							yAxis = Normalize3(Cross3(upHint, xAxis));
						}
						const HmdVector3_t zAxis = Normalize3(Cross3(xAxis, yAxis));
						const HmdVector3_t beamCenter = Add3(openvrMenuPointerBeamStart, Scale3(xAxis, beamLen * 0.5f));

						HmdMatrix34_t beamTransform = {
							xAxis.v[0], yAxis.v[0], zAxis.v[0], beamCenter.v[0],
							xAxis.v[1], yAxis.v[1], zAxis.v[1], beamCenter.v[1],
							xAxis.v[2], yAxis.v[2], zAxis.v[2], beamCenter.v[2]
						};

						const int colorRaw = (int)vr_menu_pointer_color;
						const float colorR = ((colorRaw >> 16) & 0xff) / 255.0f;
						const float colorG = ((colorRaw >> 8) & 0xff) / 255.0f;
						const float colorB = (colorRaw & 0xff) / 255.0f;
						vrOverlay->SetOverlayTexture(overlayBeamHandle, beamTexture);
						vrOverlay->SetOverlayColor(overlayBeamHandle, colorR, colorG, colorB);
						vrOverlay->SetOverlayAlpha(overlayBeamHandle, 1.0f);
						vrOverlay->SetOverlayWidthInMeters(overlayBeamHandle, std::max(0.02f, beamLen));
						vrOverlay->SetOverlayTexelAspect(overlayBeamHandle, std::max(1.0f, std::max(0.02f, beamLen) / 0.005f));
						vrOverlay->SetOverlayTransformAbsolute(overlayBeamHandle, (ETrackingUniverseOrigin)openvr::ETrackingUniverseOrigin_TrackingUniverseRawAndUncalibrated, &beamTransform);
						vrOverlay->ShowOverlay(overlayBeamHandle);
					}
					else
					{
						vrOverlay->HideOverlay(overlayBeamHandle);
					}
			}
				else if (eye == 0 && overlayBeamHandle != 0)
				{
					vrOverlay->HideOverlay(overlayBeamHandle);
				}

				if (eye == 0 && vrOverlay != nullptr && overlayCursorHandle != 0 && openvrMenuPointerActive)
				{
					if (cursorTexture == nullptr)
					{
						cursorTexture = new Texture_t();
						cursorTexture->eType = ETextureType_TextureType_OpenGL;
						cursorTexture->eColorSpace = EColorSpace_ColorSpace_Linear;
						cursorTexture->handle = nullptr;
					}
					if (cursorTexture->handle == nullptr)
					{
						GLuint cursorTextureID = 0;
						glGenTextures(1, &cursorTextureID);
						cursorTexture->handle = (void*)(std::ptrdiff_t)cursorTextureID;
						glBindTexture(GL_TEXTURE_2D, cursorTextureID);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
						constexpr int cursorTexSize = 32;
						std::vector<unsigned char> cursorPixels(cursorTexSize * cursorTexSize * 4, 0);
						for (int y = 0; y < cursorTexSize; y++)
						{
							for (int x = 0; x < cursorTexSize; x++)
							{
								const float fx = ((float)x + 0.5f) / (float)cursorTexSize * 2.0f - 1.0f;
								const float fy = ((float)y + 0.5f) / (float)cursorTexSize * 2.0f - 1.0f;
								const float dist = std::sqrt(fx * fx + fy * fy);
								const bool ring = (dist > 0.72f && dist < 0.96f);
								const bool centerDot = (dist < 0.10f);
								const bool on = ring || centerDot;
								const size_t idx = (size_t)(y * cursorTexSize + x) * 4;
								cursorPixels[idx + 0] = 255;
								cursorPixels[idx + 1] = 255;
								cursorPixels[idx + 2] = 255;
								cursorPixels[idx + 3] = on ? 255 : 0;
							}
						}
						glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cursorTexSize, cursorTexSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, cursorPixels.data());
						glBindTexture(GL_TEXTURE_2D, 0);
					}

					const HmdVector3_t xAxis = Normalize3(HmdVector3_t{ openvrOverlayAbsTransform.m[0][0], openvrOverlayAbsTransform.m[1][0], openvrOverlayAbsTransform.m[2][0] });
					const HmdVector3_t yAxis = Normalize3(HmdVector3_t{ openvrOverlayAbsTransform.m[0][1], openvrOverlayAbsTransform.m[1][1], openvrOverlayAbsTransform.m[2][1] });
					const HmdVector3_t zAxis = Normalize3(HmdVector3_t{ openvrOverlayAbsTransform.m[0][2], openvrOverlayAbsTransform.m[1][2], openvrOverlayAbsTransform.m[2][2] });
					const HmdVector3_t cursorCenter = Add3(openvrMenuPointerHit, Scale3(zAxis, 0.002f));
					HmdMatrix34_t cursorTransform = {
						xAxis.v[0], yAxis.v[0], zAxis.v[0], cursorCenter.v[0],
						xAxis.v[1], yAxis.v[1], zAxis.v[1], cursorCenter.v[1],
						xAxis.v[2], yAxis.v[2], zAxis.v[2], cursorCenter.v[2]
					};

					const int colorRaw = (int)vr_menu_pointer_color;
					const float colorR = ((colorRaw >> 16) & 0xff) / 255.0f;
					const float colorG = ((colorRaw >> 8) & 0xff) / 255.0f;
					const float colorB = (colorRaw & 0xff) / 255.0f;
					vrOverlay->SetOverlayTexture(overlayCursorHandle, cursorTexture);
					vrOverlay->SetOverlayColor(overlayCursorHandle, colorR, colorG, colorB);
					vrOverlay->SetOverlayAlpha(overlayCursorHandle, 1.0f);
					vrOverlay->SetOverlayTexelAspect(overlayCursorHandle, 1.0f);
					vrOverlay->SetOverlayWidthInMeters(overlayCursorHandle, 0.02f);
					vrOverlay->SetOverlayTransformAbsolute(overlayCursorHandle, (ETrackingUniverseOrigin)openvr::ETrackingUniverseOrigin_TrackingUniverseRawAndUncalibrated, &cursorTransform);
					vrOverlay->ShowOverlay(overlayCursorHandle);
				}
				else if (eye == 0 && overlayCursorHandle != 0)
				{
					vrOverlay->HideOverlay(overlayCursorHandle);
				}
			}
		return true;
	}

	void ApplyVPUniforms(HWDrawInfo* di)
	{
		auto& renderState = *screen->RenderState();
		di->VPUniforms.CalcDependencies();
		di->vpIndex = screen->mViewpoints->SetViewpoint(renderState, &di->VPUniforms);
	}

	template<class TYPE>
	TYPE& getHUDValue(TYPE& automap, TYPE& hud)
	{
		return (automapactive && !vr_automap_use_hud) ? automap : hud;
	}

	VSMatrix OpenVREyePose::GetHUDProjection() const
	{
		VSMatrix new_projection;
		new_projection.loadIdentity();

		float stereo_separation = (vr_ipd * 0.5) * VR_UnitsPerMeter() * getHUDValue<FFloatCVarRef>(vr_automap_stereo, vr_hud_stereo) * (eye == 1 ? -1.0 : 1.0);
		new_projection.translate(stereo_separation, 0, 0);

		// doom_units from meters
		new_projection.scale(
			-VR_UnitsPerMeter(),
			VR_UnitsPerMeter(),
			-VR_UnitsPerMeter());
		double pixelstretch = level.info ? level.info->pixelstretch : 1.2;
		new_projection.scale(1.0, pixelstretch, 1.0); // Doom universe is scaled by 1990s pixel aspect ratio

		const OpenVREyePose* activeEye = this;

		// eye coordinates from hmd coordinates
		LSMatrix44 e2h(activeEye->eyeToHeadTransform);
		new_projection.multMatrix(e2h.transpose());

		// Follow HMD orientation, EXCEPT for roll angle (keep weapon upright)
		if (activeEye->currentPose) {

			if (getHUDValue<FBoolCVarRef>(vr_automap_fixed_roll, vr_hud_fixed_roll))
			{
				float openVrRollDegrees = RAD2DEG(-eulerAnglesFromMatrix(this->currentPose->mDeviceToAbsoluteTracking).v[2]);
				new_projection.rotate(-openVrRollDegrees, 0, 0, 1);
			}

			new_projection.rotate(getHUDValue<FFloatCVarRef>(vr_automap_rotate, vr_hud_rotate), 1, 0, 0);

			if (getHUDValue<FBoolCVarRef>(vr_automap_fixed_pitch, vr_hud_fixed_pitch))
			{
				float openVrPitchDegrees = RAD2DEG(-eulerAnglesFromMatrix(this->currentPose->mDeviceToAbsoluteTracking).v[1]);
				new_projection.rotate(-openVrPitchDegrees, 1, 0, 0);
			}
		}

		// hmd coordinates (meters) from ndc coordinates
		// const float weapon_distance_meters = 0.55f;
		// const float weapon_width_meters = 0.3f;
		double distance = getHUDValue<FFloatCVarRef>(vr_automap_distance, vr_hud_distance);
		new_projection.translate(0.0, 0.0, distance);
		double vr_scale = getHUDValue<FFloatCVarRef>(vr_automap_scale, vr_hud_scale);
		new_projection.scale(
			-vr_scale,
			vr_scale,
			-vr_scale);

		// ndc coordinates from pixel coordinates
		new_projection.translate(-1.0, 1.0, 0);
		new_projection.scale(2.0 / SCREENWIDTH, -2.0 / SCREENHEIGHT, -1.0);

		VSMatrix proj(this->projectionMatrix);
		proj.multMatrix(new_projection);
		new_projection = proj;

		return new_projection;
	}

	VSMatrix OpenVRMode::GetHUDProjection() const
	{
		for (int i = 0; i < mEyeCount; ++i)
		{
			if (mEyes[i] != nullptr && mEyes[i]->isActive())
			{
				return mEyes[i]->GetHUDProjection();
			}
		}
		return GetHUDSpriteProjection();
	}

	void OpenVREyePose::AdjustHud() const
	{
		// Keep the regular camera-mounted HUD path working for non-mounted mode.
		const auto vrmode = VRMode::GetVRModeCached(true);
		if (vrmode->mEyeCount == 1)
		{
			return;
		}
		if (VR_ShouldDrawMountedHud())
		{
			return;
		}
		auto* di = HWDrawInfo::StartDrawInfo(r_viewpoint.ViewLevel, nullptr, r_viewpoint, nullptr);

		di->VPUniforms.mProjectionMatrix = GetHUDProjection();
		ApplyVPUniforms(di);
		di->EndDrawInfo();
	}

	void OpenVRMode::DrawMountedHud(HWDrawInfo* di, FRenderState& state) const
	{
		if (!VR_ShouldDrawMountedHud() || di == nullptr)
		{
			return;
		}

		auto& surface = GetVRHudSurface();
		VSMatrix mountTransform;
		if (!VR_GetMountedHudTransform(mountTransform))
		{
			return;
		}

		// Use mode-specific scale to ensure quad aspect ratio is correct for HUD vs Map
		const float mountScale = automapactive ? vr_automap_mount_scale : vr_hud_mount_scale;
		const float pixelUnit = mountScale * 0.002f;
		const float baseWidth = (float)surface.GetWidth() * pixelUnit;
		const float baseHeight = (float)surface.GetHeight() * pixelUnit;
		const bool portableHud = VR_UsePortableHud();

		auto savedMatrix = state.mModelMatrix;
		state.mModelMatrix = mountTransform;
		state.EnableModelMatrix(true);
		if (portableHud)
		{
			// Portable HUD should stay visible over weapon models in VR.
			state.SetDepthFunc(DF_Always);
		}
		di->DrawHudQuad(state, surface.GetGameTexture(),
			baseWidth,
			baseHeight,
			0.f, 0.f, // Matrix already contains the user-defined z-offset
			(automapactive ? vr_automap_mount_pos : vr_hud_mount_pos) == 1,
			automapactive);
		if (automapactive && vr_automap_border > 0)
		{
			const PalEntry borderColor = (uint32_t)vr_automap_border_color;
			float borderPadPx = (float)vr_automap_border;
			if (borderPadPx < 1.0f) borderPadPx = 1.0f;
			if (borderPadPx > 5.0f) borderPadPx = 5.0f;
			borderPadPx *= 5.0f;
			const float borderPadX = pixelUnit * borderPadPx;
			const float borderPadY = pixelUnit * borderPadPx;
			di->DrawVRHudBorder(state, baseWidth + (borderPadX * 2.0f), borderPadY, borderColor, 0.f, (baseHeight * 0.5f) + (borderPadY * 0.5f));
			di->DrawVRHudBorder(state, baseWidth + (borderPadX * 2.0f), borderPadY, borderColor, 0.f, -((baseHeight * 0.5f) + (borderPadY * 0.5f)));
			di->DrawVRHudBorder(state, borderPadX, baseHeight, borderColor, -((baseWidth * 0.5f) + (borderPadX * 0.5f)), 0.f);
			di->DrawVRHudBorder(state, borderPadX, baseHeight, borderColor, (baseWidth * 0.5f) + (borderPadX * 0.5f), 0.f);
		}
		if (portableHud)
		{
			state.SetDepthFunc(DF_Less);
		}
		state.mModelMatrix = savedMatrix;
		state.EnableModelMatrix(false);
	}

	void OpenVREyePose::AdjustBlend(HWDrawInfo* di) const
	{
		bool new_di = false;
		if (di == nullptr)
		{
			di = HWDrawInfo::StartDrawInfo(r_viewpoint.ViewLevel, nullptr, r_viewpoint, nullptr);
			new_di = true;
		}

		VSMatrix& proj = di->VPUniforms.mProjectionMatrix;
		proj.loadIdentity();
		proj.translate(-1, 1, 0);
		proj.scale(2.0 / SCREENWIDTH, -2.0 / SCREENHEIGHT, -1.0);
		ApplyVPUniforms(di);

		if (new_di)
		{
			di->EndDrawInfo();
		}
	}

	const VRMode &OpenVRMode::getInstance()
	{
		static OpenVREyePose vrmi_openvr_eyes[2] = { OpenVREyePose(0, 0., 1.), OpenVREyePose(1, 0., 1.) };
		static OpenVRMode instance(vrmi_openvr_eyes);
		return instance;
	}

	OpenVRMode::OpenVRMode(OpenVREyePose eyes[2])
		: VRMode(2, 1.f, 1.f, 1.f, eyes)
		, vrSystem(nullptr)
		, hmdWasFound(false)
		, sceneWidth(0), sceneHeight(0)
		, vrCompositor(nullptr)
		, vrOverlay(nullptr)
		, vrRenderModels(nullptr)
		, vrSettings(nullptr)
		, vrToken(0)
		, haptics(nullptr)
		, crossHairDrawer(new F2DDrawer)
	{
		//eye_ptrs.Push(&leftEyeView); // initially default behavior to Mono non-stereo rendering

		leftEyeView = &eyes[0];
		rightEyeView = &eyes[1];
		mEyes[0] = &eyes[0];
		mEyes[1] = &eyes[1];

		if (!IsOpenVRPresent()) return; // failed to load openvr API dynamically

		if (!vr::VR_IsRuntimeInstalled()) return; // failed to find OpenVR implementation

		if (!vr::VR_IsHmdPresent()) return; // no VR headset is attached

		vr::EVRInitError eError;
		// Code below recapitulates the effects of C++ call vr::VR_Init()
		vr::VR_Init(&eError, vr::EVRApplicationType::VRApplication_Scene);
		if (eError != EVRInitError_VRInitError_None) {
			std::string errMsg = vr::VR_GetVRInitErrorAsEnglishDescription(eError);
			return;
		}
		if (!vr::VR_IsInterfaceVersionValid(IVRSystem_Version))
		{
			vr::VR_Shutdown();
			return;
		}
		vrToken = vr::VR_GetInitToken();
		const std::string sys_key = std::string("FnTable:") + std::string(IVRSystem_Version);
		vrSystem = (VR_IVRSystem_FnTable*)vr::VR_GetGenericInterface(sys_key.c_str(), &eError);
		if (vrSystem == nullptr)
			return;

		vrSystem->GetRecommendedRenderTargetSize(&sceneWidth, &sceneHeight);

		leftEyeView->initialize(vrSystem);
		rightEyeView->initialize(vrSystem);


		const std::string comp_key = std::string("FnTable:") + std::string(IVRCompositor_Version);
		vrCompositor = (VR_IVRCompositor_FnTable*)VR_GetGenericInterface(comp_key.c_str(), &eError);
		if (vrCompositor == nullptr)
			return;

		SetupOverlay();

		const std::string model_key = std::string("FnTable:") + std::string(IVRRenderModels_Version);
		vrRenderModels = (VR_IVRRenderModels_FnTable*)VR_GetGenericInterface(model_key.c_str(), &eError);

		const std::string settings_key = std::string("FnTable:") + std::string(IVRSettings_Version);
		vrSettings = (VR_IVRSettings_FnTable*)VR_GetGenericInterface(settings_key.c_str(), &eError);

		//eye_ptrs.Push(&rightEyeView); // NOW we render to two eyes
		hmdWasFound = true;

		crossHairDrawer->Clear();

		haptics = new OpenVRHaptics(vrSystem);
		ApplyRefreshRate();
	}

	/* virtual */
	void OpenVRMode::SetupOverlay()
	{
		vr::EVRInitError eError;

		const std::string comp_key = std::string("FnTable:") + std::string(IVROverlay_Version);
		vrOverlay = (VR_IVROverlay_FnTable*)vr::VR_GetGenericInterface(comp_key.c_str(), &eError);
		if (vrOverlay == nullptr)
			return;

		vrOverlay->CreateOverlay((char*)"doomVROverlay", (char*)"doomVROverlay", &overlayHandle);
		vrOverlay->CreateOverlay((char*)"doomVRBeamOverlay", (char*)"doomVRBeamOverlay", &overlayBeamHandle);
		vrOverlay->CreateOverlay((char*)"doomVRCursorOverlay", (char*)"doomVRCursorOverlay", &overlayCursorHandle);
		if (overlayBeamHandle != 0)
		{
			vrOverlay->SetOverlaySortOrder(overlayBeamHandle, 2);
			vrOverlay->SetOverlayAlpha(overlayBeamHandle, 1.0f);
			vrOverlay->SetOverlayWidthInMeters(overlayBeamHandle, 0.1f);
		}
		if (overlayCursorHandle != 0)
		{
			vrOverlay->SetOverlaySortOrder(overlayCursorHandle, 3);
			vrOverlay->SetOverlayAlpha(overlayCursorHandle, 1.0f);
			vrOverlay->SetOverlayWidthInMeters(overlayCursorHandle, 0.02f);
		}
	}

	void OpenVRMode::UpdateOverlaySettings() const
	{
		if (vrOverlay == nullptr || overlayHandle == 0)
			return;
		HmdMatrix34_t overlayRelTransform = BuildOverlayRelativeTransform();

		bool rightHanded = vr_control_scheme < 10;
		TrackedDeviceIndex_t mainhandOverlayIndex = controllers[rightHanded ? 1 : 0].active ? controllers[rightHanded ? 1 : 0].index : openvr::k_unTrackedDeviceIndex_Hmd;
		TrackedDeviceIndex_t offhandOverlayIndex = controllers[rightHanded ? 0 : 1].active ? controllers[rightHanded ? 0 : 1].index : openvr::k_unTrackedDeviceIndex_Hmd;

		switch (vr_overlayscreen) {
		case 1: // stationary
		{
			if (!openvrOverlayAnchorValid || openvrOverlayAnchorMode != vr_overlayscreen)
			{
				if (openvrHasLatestHmdPose)
				{
					openvrOverlayAnchorTransform = BuildStationaryOverlayAnchorFromHmd();
					openvrOverlayAnchorValid = true;
				}
				openvrOverlayAnchorMode = vr_overlayscreen;
			}
			openvrOverlayAbsTransform = openvrOverlayAnchorTransform;
			auto tracking = (ETrackingUniverseOrigin)openvr::ETrackingUniverseOrigin_TrackingUniverseStanding;
			vrOverlay->SetOverlayTransformAbsolute(overlayHandle, tracking, &openvrOverlayAbsTransform);
			break;
		}

		case 2: // stationary (follow)
		{
			const double now = I_msTimeF();
			if (!openvrOverlayAnchorValid || openvrOverlayAnchorMode != vr_overlayscreen)
			{
				if (openvrHasLatestHmdPose)
				{
					openvrOverlayFollowCurrentTransform = BuildStationaryOverlayAnchorFromHmd();
					openvrOverlayFollowTargetTransform = openvrOverlayFollowCurrentTransform;
					openvrOverlayFollowNextTargetTime = now + 1000.0;
					openvrOverlayFollowLastStepTime = now;
					openvrOverlayAnchorValid = true;
				}
				openvrOverlayAnchorMode = vr_overlayscreen;
			}
			if (openvrHasLatestHmdPose && now >= openvrOverlayFollowNextTargetTime)
			{
				const HmdMatrix34_t candidateTarget = BuildStationaryOverlayAnchorFromHmd();
				const float currentTargetYaw = YawFromOverlayTransformDeg(openvrOverlayFollowTargetTransform);
				const float candidateYaw = YawFromOverlayTransformDeg(candidateTarget);
				const float yawDelta = fabsf(ShortestAngleDeltaDeg(candidateYaw, currentTargetYaw));
				// Ignore tiny head yaw drift to prevent stationary-follow jitter.
				if (yawDelta >= 15.0f)
				{
					openvrOverlayFollowTargetTransform = candidateTarget;
				}
				openvrOverlayFollowNextTargetTime = now + 1000.0;
			}
			const float dt = (float)clamp((now - openvrOverlayFollowLastStepTime) / 1000.0, 0.0, 0.1);
			openvrOverlayFollowLastStepTime = now;
			const float step = clamp(dt * 1.1f, 0.0f, 1.0f);
			const float eased = 1.0f - powf(1.0f - step, 3.0f);
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 4; ++c)
				{
					openvrOverlayFollowCurrentTransform.m[r][c] =
						openvrOverlayFollowCurrentTransform.m[r][c] +
						(openvrOverlayFollowTargetTransform.m[r][c] - openvrOverlayFollowCurrentTransform.m[r][c]) * eased;
				}
			}
			openvrOverlayFollowCurrentTransform = OrthonormalizeOverlayTransform(openvrOverlayFollowCurrentTransform);
			openvrOverlayAbsTransform = openvrOverlayFollowCurrentTransform;
			auto tracking = (ETrackingUniverseOrigin)openvr::ETrackingUniverseOrigin_TrackingUniverseStanding;
			vrOverlay->SetOverlayTransformAbsolute(overlayHandle, tracking, &openvrOverlayAbsTransform);
			break;
		}

		case 3: // follow head movement
			if (openvrHasLatestHmdPose)
				openvrOverlayAbsTransform = Mul34(openvrLatestHmdPose, overlayRelTransform);
			vrOverlay->SetOverlayTransformTrackedDeviceRelative(overlayHandle, openvr::k_unTrackedDeviceIndex_Hmd, &overlayRelTransform);
			break;

		case 4: // follow main hand
			if (mainhandOverlayIndex == openvr::k_unTrackedDeviceIndex_Hmd)
			{
				if (openvrHasLatestHmdPose)
					openvrOverlayAbsTransform = Mul34(openvrLatestHmdPose, overlayRelTransform);
				vrOverlay->SetOverlayTransformTrackedDeviceRelative(overlayHandle, mainhandOverlayIndex, &overlayRelTransform);
			}
			else
			{
				const int role = rightHanded ? 1 : 0;
				if (controllers[role].active && controllers[role].pose.bPoseIsValid)
					openvrOverlayAbsTransform = BuildControllerFollowOverlayAnchor(controllers[role].pose.mDeviceToAbsoluteTracking);
				vrOverlay->SetOverlayTransformAbsolute(overlayHandle, (ETrackingUniverseOrigin)openvr::ETrackingUniverseOrigin_TrackingUniverseStanding, &openvrOverlayAbsTransform);
			}
			break;

		case 5: // follow off hand
			if (offhandOverlayIndex == openvr::k_unTrackedDeviceIndex_Hmd)
			{
				if (openvrHasLatestHmdPose)
					openvrOverlayAbsTransform = Mul34(openvrLatestHmdPose, overlayRelTransform);
				vrOverlay->SetOverlayTransformTrackedDeviceRelative(overlayHandle, offhandOverlayIndex, &overlayRelTransform);
			}
			else
			{
				const int role = rightHanded ? 0 : 1;
				if (controllers[role].active && controllers[role].pose.bPoseIsValid)
					openvrOverlayAbsTransform = BuildControllerFollowOverlayAnchor(controllers[role].pose.mDeviceToAbsoluteTracking);
				vrOverlay->SetOverlayTransformAbsolute(overlayHandle, (ETrackingUniverseOrigin)openvr::ETrackingUniverseOrigin_TrackingUniverseStanding, &openvrOverlayAbsTransform);
			}
			break;
		}
	}

	void OpenVRMode::ApplyRefreshRate() const
	{
		if (!hmdWasFound || vrSystem == nullptr)
		{
			if (developer > 0 && !refreshRateLoggedUnavailable)
			{
				Printf("OpenVR: refresh-rate control unavailable; HMD/runtime missing.\n");
				refreshRateLoggedUnavailable = true;
			}
			return;
		}

		if (vrSettings == nullptr)
		{
			if (developer > 0 && !refreshRateLoggedUnavailable)
			{
				Printf("OpenVR: refresh-rate control unavailable; SteamVR settings interface missing.\n");
				refreshRateLoggedUnavailable = true;
			}
			return;
		}

		auto readDisplayFrequency = [this]() -> float
		{
			ETrackedPropertyError propError = (ETrackedPropertyError)0;
			const float rate = vrSystem->GetFloatTrackedDeviceProperty(
				k_unTrackedDeviceIndex_Hmd,
				(ETrackedDeviceProperty)2002,
				&propError);
			return propError == (ETrackedPropertyError)0 ? rate : 0.0f;
		};

		const int requestedRate = std::max(0, (int)vid_refreshrate);
		if (refreshRateHasLastRequest && lastRefreshRateMenuValue == requestedRate)
			return;

		refreshRateHasLastRequest = true;
		lastRefreshRateMenuValue = requestedRate;

		const float currentRateBefore = readDisplayFrequency();
		if (currentRateBefore > 0.0f)
			lastObservedHmdRefreshRate = currentRateBefore;

		if (developer > 0 && !refreshRateLoggedControlPath)
		{
			Printf("OpenVR: using SteamVR display refresh-rate control.\n");
			refreshRateLoggedControlPath = true;
		}

		if (requestedRate <= 0)
			return;

		EVRSettingsError settingsError = (EVRSettingsError)0;
		vrSettings->SetFloat(
			const_cast<char*>(k_pch_SteamVR_Section),
			const_cast<char*>(k_pch_SteamVR_PreferredRefreshRate),
			(float)requestedRate,
			&settingsError);
		if (settingsError != (EVRSettingsError)0)
		{
			if (developer > 0)
				Printf("OpenVR: failed to request display refresh rate %d Hz.\n", requestedRate);
			return;
		}

		lastAppliedPreferredRefreshRate = (float)requestedRate;

		const float currentRateAfter = readDisplayFrequency();
		if (currentRateAfter > 0.0f)
			lastObservedHmdRefreshRate = currentRateAfter;

		if (currentRateAfter <= 0.0f)
		{
			if (developer > 0)
				Printf("OpenVR: requested display refresh rate %d Hz.\n", requestedRate);
			return;
		}

		if (developer > 0)
		{
			Printf("OpenVR: requested display refresh rate %d Hz (current=%.0f Hz).\n",
				requestedRate,
				(double)currentRateAfter);
		}
	}

	// AdjustViewports() is called from within FLGRenderer::SetOutputViewport(...)
	void OpenVRMode::AdjustViewport(DFrameBuffer* screen) const
	{
		if (screen == nullptr)
			return;
		// Draw the 3D scene into the entire framebuffer
		screen->mSceneViewport.width = sceneWidth;
		screen->mSceneViewport.height = sceneHeight;
		screen->mSceneViewport.left = 0;
		screen->mSceneViewport.top = 0;

		screen->mScreenViewport.width = sceneWidth;
		screen->mScreenViewport.height = sceneHeight;
	}

	void OpenVRMode::AdjustPlayerSprites(FRenderState &state, int hand) const
	{
		if (GetWeaponTransform(&state.mModelMatrix, hand))
		{
			// TODO scale need to be fixed
			float scale = 0.00125f * vr_weaponScale * vr_2dweaponScale;
			state.mModelMatrix.scale(scale, -scale, scale);
			state.mModelMatrix.translate(-viewwidth / 2, -viewheight * 3 / 4, 0.0f);

			float offsetFactor = 40.f;
			state.mModelMatrix.translate(vr_2dweaponOffsetX * offsetFactor, -vr_2dweaponOffsetY * offsetFactor, vr_2dweaponOffsetZ * offsetFactor);
		}
		state.EnableModelMatrix(true);
	}

	void OpenVRMode::UnAdjustPlayerSprites(FRenderState &state) const {

		state.EnableModelMatrix(false);
	}

	void OpenVRMode::AdjustCrossHair() const
	{
		// Remove effect of screenblocks setting on crosshair position
		cachedViewheight = viewheight;
		cachedViewwindowy = viewwindowy;
		viewheight = SCREENHEIGHT;
		viewwindowy = 0;
	}

	void OpenVRMode::UnAdjustCrossHair() const
	{
		viewheight = cachedViewheight;
		viewwindowy = cachedViewwindowy;
	}

	bool OpenVRMode::GetHandTransform(int hand, VSMatrix* mat) const
	{
		double pixelstretch = level.info ? level.info->pixelstretch : 1.2;
		player_t* player = r_viewpoint.camera ? r_viewpoint.camera->player : nullptr;
		if (player)
		{
			mat->loadIdentity();

			//We want to offset the weapon exactly from where we are seeing from
			mat->translate(r_viewpoint.CenterEyePos.X, r_viewpoint.CenterEyePos.Z - getDoomPlayerHeightWithoutCrouch(player), r_viewpoint.CenterEyePos.Y);

			mat->scale(VR_UnitsPerMeter(), VR_UnitsPerMeter(), -VR_UnitsPerMeter());

			if ((vr_control_scheme < 10 && hand == 1)
				|| (vr_control_scheme >= 10 && hand == 0)) {
				mat->translate(-weaponoffset[0], (hmdPosition[1] + weaponoffset[1] + vr_height_adjust) / pixelstretch, weaponoffset[2]);

				mat->scale(1, 1 / pixelstretch, 1);

                if (VR_UseCinematicScreenLayer())
                {
                    mat->rotate(-90 + r_viewpoint.Angles.Yaw.Degrees()  + (weaponangles[YAW] - hmdorientation[YAW]), 0, 1, 0);
                    mat->rotate(-weaponangles[PITCH] - r_viewpoint.Angles.Pitch.Degrees(), 1, 0, 0);
                } else {
					mat->rotate(-90 + doomYaw + (weaponangles[YAW]- hmdorientation[YAW]), 0, 1, 0);
					mat->rotate(-weaponangles[PITCH], 1, 0, 0);
				}
				mat->rotate(-weaponangles[ROLL], 0, 0, 1);
			}
			else
			{
				mat->translate(-offhandoffset[0], (hmdPosition[1] + offhandoffset[1] + vr_height_adjust) / pixelstretch, offhandoffset[2]);

				mat->scale(1, 1 / pixelstretch, 1);

                if (VR_UseCinematicScreenLayer())
                {
                    mat->rotate(-90 + r_viewpoint.Angles.Yaw.Degrees()  + (offhandangles[YAW] - hmdorientation[YAW]), 0, 1, 0);
                    mat->rotate(-offhandangles[PITCH] - r_viewpoint.Angles.Pitch.Degrees(), 1, 0, 0);
                } else {
					mat->rotate(-90 + doomYaw + (offhandangles[YAW]- hmdorientation[YAW]), 0, 1, 0);
					mat->rotate(-offhandangles[PITCH], 1, 0, 0);
				}
				mat->rotate(-offhandangles[ROLL], 0, 0, 1);
			}

			return true;

		}

		return false;
	}

	void getMainHandAngles()
	{
		bool rightHanded = vr_control_scheme < 10;
		int hand = rightHanded ? 1 : 0;
		HmdVector3d_t eulerAngles = eulerAnglesFromMatrixPitchRotate(controllers[hand].pose.mDeviceToAbsoluteTracking, vr_weaponRotate * 2);
		weaponangles[YAW] = RAD2DEG(eulerAngles.v[0]);
		weaponangles[PITCH] = -RAD2DEG(eulerAngles.v[1]);
		weaponangles[ROLL] = normalizeAngle(-RAD2DEG(eulerAngles.v[2]) + 180.);
	}

	void getOffHandAngles()
	{
		bool rightHanded = vr_control_scheme < 10;
		int hand = rightHanded ? 0 : 1;
		HmdVector3d_t eulerAngles = eulerAnglesFromMatrixPitchRotate(controllers[hand].pose.mDeviceToAbsoluteTracking, vr_weaponRotate * 2);
		offhandangles[YAW] = RAD2DEG(eulerAngles.v[0]);
		offhandangles[PITCH] = -RAD2DEG(eulerAngles.v[1]);
		offhandangles[ROLL] = normalizeAngle(-RAD2DEG(eulerAngles.v[2]) + 180.);
	}

	/* virtual */
	void OpenVRMode::Present() const {
		GLRenderer->mBuffers->BlitToEyeTexture(GLRenderer->mBuffers->CurrentEye(), false);
		// TODO: For performance, don't render to the desktop screen here
		if (doRenderToDesktop && vr_desktop_view != -1) {
			GLRenderer->mBuffers->BindOutputFB();
			GLRenderer->ClearBorders();

			// Compute screen regions to use for left and right eye views
			int leftWidth;
			if(vr_desktop_view == 1)
				leftWidth = screen->mOutputLetterbox.width;
			else if(vr_desktop_view == 2)
				leftWidth = 0;
			else
				leftWidth = screen->mOutputLetterbox.width / 2;
			int rightWidth = screen->mOutputLetterbox.width - leftWidth;
			IntRect leftHalfScreen = screen->mOutputLetterbox;
			leftHalfScreen.width = leftWidth;
			IntRect rightHalfScreen = screen->mOutputLetterbox;
			rightHalfScreen.width = rightWidth;
			rightHalfScreen.left += leftWidth;

			if (vr_desktop_view < 2) {
				GLRenderer->mBuffers->BindEyeTexture(0, 0);
				GLRenderer->DrawPresentTexture(leftHalfScreen, true);
			}
			if (vr_desktop_view != 1) {
				GLRenderer->mBuffers->BindEyeTexture(1, 0);
				GLRenderer->DrawPresentTexture(rightHalfScreen, true);
			}
		}
		if (doRenderToHmd)
		{
			leftEyeView->submitFrame(vrCompositor, vrOverlay);
			rightEyeView->submitFrame(vrCompositor, vrOverlay);
		}
	}

	static int mAngleFromRadians(double radians)
	{
		double m = std::round(65535.0 * radians / (2.0 * M_PI));
		return int(m);
	}

	static int joyint(double val)
	{
		if (val >= 0)
		{
			return int(ceil(val));
		}
		else
		{
			return int(floor(val));
		}
	}

	void OpenVRMode::updateHmdPose(FRenderViewpoint& vp) const
	{
		float dummy=0;
		float hmdYaw=0;
		float hmdpitch=0;
		float hmdroll=0;

		// the yaw returned contains snapTurn input value
		VR_GetMove(&dummy, &dummy, &dummy, &dummy, &dummy, &hmdYaw, &hmdpitch, &hmdroll);
		if (VR_UseCinematicScreenLayer())
		{
			cinemamodePitch = hmdorientation[PITCH];
		}

		double hmdYawDeltaDegrees = 0;
		if (doTrackHmdYaw) {
			// Set HMD angle game state parameters for NEXT frame
			static double previousHmdYaw = 0;
			static bool havePreviousYaw = false;
			static float previousCinemaSnapTurn = 0.0f;
			static bool wasLockedToScreenLayerLastFrame = false;
			player_t* player = &players[consoleplayer];
			if (!havePreviousYaw || resetPreviousHmdYaw) {
				previousHmdYaw = hmdYaw;
				havePreviousYaw = true;
				resetPreviousHmdYaw = false;
			}
			if (resetDoomYaw || (player && player->resetDoomYaw))
			{
				previousHmdYaw = hmdYaw;
			}
			hmdYawDeltaDegrees = hmdYaw - previousHmdYaw;
			double cinemaTurnDeltaDegrees = 0.0;
			if (VR_UseScreenLayer())
			{
				if (!wasLockedToScreenLayerLastFrame)
				{
					previousCinemaSnapTurn = snapTurn;
					cinemamodeYaw = r_viewpoint.Angles.Yaw.Degrees();
				}
				cinemaTurnDeltaDegrees = ShortestAngleDeltaDeg(snapTurn, previousCinemaSnapTurn);
				previousCinemaSnapTurn = snapTurn;
				wasLockedToScreenLayerLastFrame = true;
			}
			else
			{
				wasLockedToScreenLayerLastFrame = false;
			}

			if (!VR_UseScreenLayer())
			{
				vrApplyingHmdYaw = true;
				G_AddViewAngle(mAngleFromRadians(DEG2RAD(-hmdYawDeltaDegrees)));
				vrApplyingHmdYaw = false;
			}
			else if (cinemaTurnDeltaDegrees != 0.0)
			{
				vrApplyingHmdYaw = true;
				G_AddViewAngle(mAngleFromRadians(DEG2RAD(-cinemaTurnDeltaDegrees)));
				vrApplyingHmdYaw = false;
				doomYaw += cinemaTurnDeltaDegrees;
				cinemamodeYaw = doomYaw;
			}
			previousHmdYaw = hmdYaw;
		}

		if (!forceDisableOverlay && VR_UseScreenLayer() && paused)
			doTrackHmdAngles = false;
		else
			doTrackHmdAngles = true;

		/* */
		// Pitch
		if (doTrackHmdPitch && doTrackHmdAngles) {
			if (resetPreviousPitch)
			{
				previousPitch = vp.HWAngles.Pitch.Degrees();
				resetPreviousPitch = false;
			}

			double hmdPitchDeltaDegrees = -hmdpitch - previousPitch;

			G_AddViewPitch(mAngleFromRadians(DEG2RAD(-hmdPitchDeltaDegrees)));
			previousPitch = -hmdpitch;
		}

		if (gamestate == GS_LEVEL && menuactive == MENU_Off)
		{
			if (!VR_UseScreenLayer())
			{
				doomYaw += hmdYawDeltaDegrees;

				// Roll can be local, because it doesn't affect gameplay.
				if (doTrackHmdRoll && doTrackHmdAngles)
				{
					vp.HWAngles.Roll = FAngle::fromDeg(-hmdroll);
				}
				if (doTrackHmdPitch && doTrackHmdAngles && doLateScheduledRotationTracking)
				{
					vp.HWAngles.Pitch = FAngle::fromDeg(-hmdpitch);
				}
			}
			else
			{
				vp.HWAngles.Roll = FAngle::fromDeg(0.0f);
				vp.HWAngles.Pitch = FAngle::fromDeg(-hmdorientation[PITCH]);
			}

			// Late-schedule update to renderer angles directly, too
			if (doTrackHmdYaw && doTrackHmdAngles && doLateScheduledRotationTracking)
			{
				double viewYaw = getViewpointYaw();
				while (viewYaw <= -180.0)
					viewYaw += 360.0;
				while (viewYaw > 180.0)
					viewYaw -= 360.0;
				vp.Angles.Yaw = DAngle::fromDeg(viewYaw);
			}
		}
	}

	static int GetVRAxisState(VRControllerState_t& state, int vrAxis, int axis)
	{
		float pos = axis == 0 ? state.rAxis[vrAxis].x : state.rAxis[vrAxis].y;
		return pos < -DEAD_ZONE ? 1 : pos > DEAD_ZONE ? 2 : 0;
	}

	void Joy_GenerateUIButtonEvents(int oldbuttons, int newbuttons, int numbuttons, const int* keys)
	{
		int changed = oldbuttons ^ newbuttons;
		if (changed != 0)
		{
			event_t ev = { 0, 0, 0, 0, 0, 0, 0 };
			int mask = 1;
			for (int j = 0; j < numbuttons; mask <<= 1, ++j)
			{
				if (changed & mask)
				{
					ev.data1 = keys[j];
					ev.type = EV_GUI_Event;
					ev.subtype = (newbuttons & mask) ? EV_GUI_KeyDown : EV_GUI_KeyUp;
					D_PostEvent(&ev);
				}
			}
		}
	}

	static void HandleVRAxis(VRControllerState_t& lastState, VRControllerState_t& newState, int vrAxis, int axis, int negativedoomkey, int positivedoomkey, int base)
	{
		int keys[] = { negativedoomkey + base, positivedoomkey + base };
		Joy_GenerateButtonEvents(GetVRAxisState(lastState, vrAxis, axis), GetVRAxisState(newState, vrAxis, axis), 2, keys);
	}

	static void HandleUIVRAxis(VRControllerState_t& lastState, VRControllerState_t& newState, int vrAxis, int axis, ESpecialGUIKeys negativedoomkey, ESpecialGUIKeys positivedoomkey)
	{
		int keys[] = { (int)negativedoomkey, (int)positivedoomkey };
		Joy_GenerateUIButtonEvents(GetVRAxisState(lastState, vrAxis, axis), GetVRAxisState(newState, vrAxis, axis), 2, keys);
	}

	static void HandleUIVRAxes(VRControllerState_t& lastState, VRControllerState_t& newState, int vrAxis,
		ESpecialGUIKeys xnegativedoomkey, ESpecialGUIKeys xpositivedoomkey, ESpecialGUIKeys ynegativedoomkey, ESpecialGUIKeys ypositivedoomkey)
	{
		int oldButtons = abs(lastState.rAxis[vrAxis].x) > abs(lastState.rAxis[vrAxis].y)
			? GetVRAxisState(lastState, vrAxis, 0)
			: GetVRAxisState(lastState, vrAxis, 1) << 2;
		int newButtons = abs(newState.rAxis[vrAxis].x) > abs(newState.rAxis[vrAxis].y)
			? GetVRAxisState(newState, vrAxis, 0)
			: GetVRAxisState(newState, vrAxis, 1) << 2;

		int keys[] = { xnegativedoomkey, xpositivedoomkey, ynegativedoomkey, ypositivedoomkey };

		Joy_GenerateUIButtonEvents(oldButtons, newButtons, 4, keys);
	}

	static void HandleVRButton(VRControllerState_t& lastState, VRControllerState_t& newState, long long vrindex, int doomkey, int base)
	{
		Joy_GenerateButtonEvents((lastState.ulButtonPressed & (1LL << vrindex)) ? 1 : 0, (newState.ulButtonPressed & (1LL << vrindex)) ? 1 : 0, 1, doomkey + base);
	}

	static void HandleUIVRButton(VRControllerState_t& lastState, VRControllerState_t& newState, long long vrindex, int doomkey)
	{
		Joy_GenerateUIButtonEvents((lastState.ulButtonPressed & (1LL << vrindex)) ? 1 : 0, (newState.ulButtonPressed & (1LL << vrindex)) ? 1 : 0, 1, &doomkey);
	}

	static void HandleControllerState(int device, int role, VRControllerState_t& newState)
	{
		VRControllerState_t& lastState = controllers[role].lastState;

		if (menuactive == MENU_On && menuactive != MENU_WaitKey)
		{
			if (axisTrackpad != -1)
			{
				HandleUIVRAxes(lastState, newState, axisTrackpad, GK_LEFT, GK_RIGHT, GK_DOWN, GK_UP);
			}
			if (axisJoystick != -1)
			{
				HandleUIVRAxes(lastState, newState, axisJoystick, GK_LEFT, GK_RIGHT, GK_DOWN, GK_UP);
			}

			HandleUIVRButton(lastState, newState, openvr::vr::k_EButton_Axis1, GK_RETURN);
			HandleUIVRButton(lastState, newState, openvr::vr::k_EButton_Grip, GK_BACK);
			HandleUIVRButton(lastState, newState, openvr::vr::k_EButton_A, GK_BACK);
			HandleUIVRButton(lastState, newState, openvr::vr::k_EButton_ApplicationMenu, GK_BACKSPACE);
		}
		else {
			const bool dominantHand = (vr_control_scheme < 10) ? (role == 1) : (role == 0);

			if (axisTrackpad != -1)
			{
				HandleVRAxis(lastState, newState, axisTrackpad, 0, KEY_PAD_LTHUMB_LEFT, KEY_PAD_LTHUMB_RIGHT, role * (KEY_PAD_RTHUMB_LEFT - KEY_PAD_LTHUMB_LEFT));
				HandleVRAxis(lastState, newState, axisTrackpad, 1, KEY_PAD_LTHUMB_DOWN, KEY_PAD_LTHUMB_UP, role * (KEY_PAD_RTHUMB_DOWN - KEY_PAD_LTHUMB_DOWN));
			}
			if (axisJoystick != -1)
			{
				HandleVRAxis(lastState, newState, axisJoystick, 0, KEY_JOYAXIS1MINUS, KEY_JOYAXIS1PLUS, role * (KEY_JOYAXIS3PLUS - KEY_JOYAXIS1PLUS));
				HandleVRAxis(lastState, newState, axisJoystick, 1, KEY_JOYAXIS2MINUS, KEY_JOYAXIS2PLUS, role * (KEY_JOYAXIS3PLUS - KEY_JOYAXIS1PLUS));
			}

			// k_EButton_Grip === k_EButton_IndexController_A
			if (vr_secondary_button_mappings && dominantHand)
			{
				const int gripKey = KEY_PAD_LSHOULDER + role * (KEY_PAD_RSHOULDER - KEY_PAD_LSHOULDER);
				Joy_GenerateButtonEvents(
					(lastState.ulButtonPressed & (1LL << openvr::vr::k_EButton_Grip)) ? 1 : 0,
					0,
					1,
					&gripKey);
			}
			else
			{
				HandleVRButton(lastState, newState, openvr::vr::k_EButton_Grip, KEY_PAD_LSHOULDER, role * (KEY_PAD_RSHOULDER - KEY_PAD_LSHOULDER));
			}

			// k_EButton_ApplicationMenu / k_EButton_IndexController_B
			HandleVRButton(lastState, newState, openvr::vr::k_EButton_ApplicationMenu, KEY_PAD_BACK, role * (KEY_PAD_START - KEY_PAD_BACK));

			// k_EButton_A
			HandleVRButton(lastState, newState, openvr::vr::k_EButton_A, KEY_PAD_A, role * (KEY_PAD_B - KEY_PAD_A));

			// k_EButton_Axis0 === k_EButton_SteamVR_Touchpad
			HandleVRButton(lastState, newState, openvr::vr::k_EButton_Axis0, KEY_PAD_LTHUMB, role * (KEY_PAD_RTHUMB - KEY_PAD_LTHUMB));

			// k_EButton_Axis1 === k_EButton_SteamVR_Trigger
			HandleVRButton(lastState, newState, openvr::vr::k_EButton_Axis1, KEY_PAD_LTRIGGER, role * (KEY_PAD_RTRIGGER - KEY_PAD_LTRIGGER));

			// k_EButton_Axis2 === SteamVR-binding "Right Axis 2 Press" (at least on Index Controller)
			HandleVRButton(lastState, newState, openvr::vr::k_EButton_Axis2, KEY_PAD_X, role * (KEY_PAD_Y - KEY_PAD_X));

			// k_EButton_Axis3 (unknown if used by any controller at all)
			HandleVRButton(lastState, newState, openvr::vr::k_EButton_Axis3, KEY_JOY1, role * (KEY_JOY2 - KEY_JOY1));

			// k_EButton_Axis4 (unknown if used by any controller at all)
			HandleVRButton(lastState, newState, openvr::vr::k_EButton_Axis4, KEY_JOY3, role * (KEY_JOY4 - KEY_JOY3));
		}

		lastState = newState;
	}

	VRControllerState_t leftTrackedRemoteState_old;
	VRControllerState_t leftTrackedRemoteState_new;

	VRControllerState_t rightTrackedRemoteState_old;
	VRControllerState_t rightTrackedRemoteState_new;

	void HandleInput_Default(
		int control_scheme, 
		VRControllerState_t *pDominantTrackedRemoteNew, VRControllerState_t *pDominantTrackedRemoteOld, Controller* pDominantTracking,
		VRControllerState_t *pOffTrackedRemoteNew, VRControllerState_t *pOffTrackedRemoteOld, Controller* pOffTracking,
		int domButton1, int domButton2, int offButton1, int offButton2 )
	{
#if 0
		if (leftTrackedRemoteState_new.ulButtonPressed)
			DPrintf(DMSG_NOTIFY, "leftTrackedRemoteState_new: %" PRIu64 "\n", leftTrackedRemoteState_new.ulButtonPressed);
		if (rightTrackedRemoteState_new.ulButtonPressed)
			DPrintf(DMSG_NOTIFY, "rightTrackedRemoteState_new: %" PRIu64 "\n", rightTrackedRemoteState_new.ulButtonPressed);
#endif
		//Dominant Grip works like a shift key
		bool dominantGripPushedOld = vr_secondary_button_mappings ?
				(pDominantTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Grip)) : false;
		bool dominantGripPushedNew = vr_secondary_button_mappings ?
				(pDominantTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Grip)) : false;
		static float analogTurnRateDegPerSec = 0.0f;
		static uint64_t lastAnalogTurnTime = 0;

		VRControllerState_t *pPrimaryTrackedRemoteNew, *pPrimaryTrackedRemoteOld,  *pSecondaryTrackedRemoteNew, *pSecondaryTrackedRemoteOld;
		if (vr_switch_sticks)
		{
			pPrimaryTrackedRemoteNew = pOffTrackedRemoteNew;
			pPrimaryTrackedRemoteOld = pOffTrackedRemoteOld;
			pSecondaryTrackedRemoteNew = pDominantTrackedRemoteNew;
			pSecondaryTrackedRemoteOld = pDominantTrackedRemoteOld;
		}
		else
		{
			pPrimaryTrackedRemoteNew = pDominantTrackedRemoteNew;
			pPrimaryTrackedRemoteOld = pDominantTrackedRemoteOld;
			pSecondaryTrackedRemoteNew = pOffTrackedRemoteNew;
			pSecondaryTrackedRemoteOld = pOffTrackedRemoteOld;
		}

		const auto vrmode = VRMode::GetVRModeCached(true);

		//All this to allow stick and button switching!
		uint64_t primaryButtonsNew;
		uint64_t primaryButtonsOld;
		uint64_t secondaryButtonsNew;
		uint64_t secondaryButtonsOld;
		int primaryButton1;
		int primaryButton2;
		int secondaryButton1;
		int secondaryButton2;

		if (control_scheme == 11) // Left handed Alt
		{
			primaryButtonsNew = pOffTrackedRemoteNew->ulButtonPressed;
			primaryButtonsOld = pOffTrackedRemoteOld->ulButtonPressed;
			secondaryButtonsNew = pDominantTrackedRemoteNew->ulButtonPressed;
			secondaryButtonsOld = pDominantTrackedRemoteOld->ulButtonPressed;

			primaryButton1 = offButton1;
			primaryButton2 = offButton2;
			secondaryButton1 = domButton1;
			secondaryButton2 = domButton2;
		}
		else // Left and right handed
		{
			primaryButtonsNew = pDominantTrackedRemoteNew->ulButtonPressed;
			primaryButtonsOld = pDominantTrackedRemoteOld->ulButtonPressed;
			secondaryButtonsNew = pOffTrackedRemoteNew->ulButtonPressed;
			secondaryButtonsOld = pOffTrackedRemoteOld->ulButtonPressed;

			primaryButton1 = domButton1;
			primaryButton2 = domButton2;
			secondaryButton1 = offButton1;
			secondaryButton2 = offButton2;
		}

		// Only do the following if we are definitely not in the menu
		if (gamestate == GS_LEVEL && menuactive == MENU_Off && !paused)
		{
			const HmdMatrix34_t& dominantControllerPose = pDominantTracking->pose.mDeviceToAbsoluteTracking;
			const HmdMatrix34_t& offhandControllerPose = pOffTracking->pose.mDeviceToAbsoluteTracking;

			float distance = sqrtf(powf(offhandControllerPose.m[0][3] -
										dominantControllerPose.m[0][3], 2) +
								powf(offhandControllerPose.m[1][3] -
										dominantControllerPose.m[1][3], 2) +
								powf(offhandControllerPose.m[2][3] -
										dominantControllerPose.m[2][3], 2));

			const bool offhandGripHeld =
				(pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Grip)) != 0;
			const bool dominantPoseValid = pDominantTracking->active && pDominantTracking->pose.bPoseIsValid;
			const bool offhandPoseValid = pOffTracking->active && pOffTracking->pose.bPoseIsValid;
			weaponStabilised = vr_two_handed_weapons && offhandGripHeld && dominantPoseValid && offhandPoseValid && distance < 0.50f;

			//dominant hand stuff first
			{
				//Weapon location relative to view
				weaponoffset[0] = dominantControllerPose.m[0][3] - hmdPosition[0];
				weaponoffset[1] = dominantControllerPose.m[1][3] - hmdPosition[1];
				weaponoffset[2] = dominantControllerPose.m[2][3] - hmdPosition[2];

				float yawRotation = getViewpointYaw() - hmdorientation[YAW];
				DVector2 v = DVector2(weaponoffset[0], weaponoffset[2]).Rotated(-yawRotation);
				weaponoffset[0] = v.Y;
				weaponoffset[2] = v.X;

				if (weaponStabilised) {
					float z = offhandControllerPose.m[2][3] -
							dominantControllerPose.m[2][3];
					float x = offhandControllerPose.m[0][3] -
							dominantControllerPose.m[0][3];
					float y = offhandControllerPose.m[1][3] -
							dominantControllerPose.m[1][3];
					float zxDist = length(x, z);

					// If the hands become nearly vertically stacked, the stabilised solve can
					// snap the weapon pitch toward straight up/down. Fall back to the tracked
					// dominant-hand orientation for that frame instead of preserving a bad latch.
					if (zxDist > 0.05f && distance > 0.05f) {
						VectorSet(weaponangles, -RAD2DEG(atanf(y / zxDist)), -RAD2DEG(atan2f(x, -z)),
								weaponangles[ROLL]);
					} else {
						weaponStabilised = false;
					}
				}
			}

			float controllerYawHeading = 0.0f;

			//off-hand stuff
			{
				const HmdMatrix34_t& offhandControllerPose = pOffTracking->pose.mDeviceToAbsoluteTracking;
				offhandoffset[0] = offhandControllerPose.m[0][3] - hmdPosition[0];
				offhandoffset[1] = offhandControllerPose.m[1][3] - hmdPosition[1];
				offhandoffset[2] = offhandControllerPose.m[2][3] - hmdPosition[2];

				float yawRotation = getViewpointYaw() - hmdorientation[YAW];
				DVector2 v = DVector2(offhandoffset[0], offhandoffset[2]).Rotated(-yawRotation);
				offhandoffset[0] = v.Y;
				offhandoffset[2] = v.X;

				if (vr_move_use_offhand) {
					controllerYawHeading = offhandangles[YAW] - hmdorientation[YAW];
				} else {
					controllerYawHeading = 0.0f;
				}
			}

			//Positional movement
			{
				DVector2 v = DVector2(positionDeltaThisFrame[0], positionDeltaThisFrame[2]).Rotated(DAngle::fromDeg(-(doomYaw - hmdorientation[YAW])));
				//DVector2 v = DVector2(-openvr_dpos.x, openvr_dpos.z).Rotated(openvr_to_doom_angle);
				positional_movementSideways = v.Y;
				positional_movementForward = v.X;
				if (multiplayer)
				{
					VR_AddMultiplayerRoomscaleWorldOffset(
						positional_movementSideways * VR_UnitsPerMeter(),
						positional_movementForward * VR_UnitsPerMeter());
				}
			}

			//Off-hand specific stuff
			{
				const bool suppressOffhandLocomotion = dominantGripPushedNew;
				if (suppressOffhandLocomotion)
				{
					ready_teleport = false;
					remote_movementSideways = 0.0f;
					remote_movementForward = 0.0f;
				}
				else
				{
					//Teleport - only does anything if vr_teleport cvar is true
					if (vr_teleport) {
						if ((pSecondaryTrackedRemoteOld->rAxis[axisJoystick].y > 0.7f) && !ready_teleport) {
							ready_teleport = true;
						} else if ((pSecondaryTrackedRemoteOld->rAxis[axisJoystick].y < 0.7f) && ready_teleport) {
							ready_teleport = false;
							trigger_teleport = true;
						}
					}

					//Apply a filter and quadratic scaler so small movements are easier to make
					//and we don't get movement jitter when the joystick doesn't quite center properly
					float dist = length(pSecondaryTrackedRemoteNew->rAxis[axisJoystick].x, pSecondaryTrackedRemoteNew->rAxis[axisJoystick].y);
					float nlf = nonLinearFilter(dist);
					dist = (dist > 1.0f) ? dist : 1.0f;
					float x = nlf * (pSecondaryTrackedRemoteNew->rAxis[axisJoystick].x / dist);
					float y = nlf * (pSecondaryTrackedRemoteNew->rAxis[axisJoystick].y / dist);

					//Apply a simple deadzone
					bool player_moving = (fabs(x) + fabs(y)) > 0.05f;
					x = player_moving ? x : 0;
					y = player_moving ? y : 0;

					//Adjust to be off-hand controller oriented
					//vec2_t v;
					//rotateAboutOrigin(x, y, controllerYawHeading, v);

					remote_movementSideways = x;
					remote_movementForward = y;
				}
			}

			if (!dominantGripPushedNew)
			{
				static int increaseSnap = true;
				static int decreaseSnap = true;

				const uint64_t currentTime = I_msTime();
				if (lastAnalogTurnTime == 0)
				{
					lastAnalogTurnTime = currentTime;
				}
				float deltaSeconds = float(currentTime - lastAnalogTurnTime) * 0.001f;
				if (deltaSeconds > 0.1f)
				{
					deltaSeconds = 0.1f;
				}
				lastAnalogTurnTime = currentTime;

				float joy = -I_OpenVRGetYaw();
				if (vr_snapTurn <= 10.0f)
				{
					snapTurn += VR_ApplyAnalogSmoothTurn(joy, 210.0f, deltaSeconds, VR_GetAnalogTurnResponseScale(vr_snapTurn), analogTurnRateDegPerSec);
					if (fabsf(joy) > 0.05f)
					{
						increaseSnap = false;
						decreaseSnap = false;
					}
					else
					{
						analogTurnRateDegPerSec = 0.0f;
					}
				}

				// Turning logic
				if (joy > 0.6f && increaseSnap) {
					snapTurn -= vr_snapTurn;
					if (vr_snapTurn > 10.0f) {
						increaseSnap = false;
					}
				} else if (joy < 0.4f) {
					increaseSnap = true;
				}

				if (joy < -0.6f && decreaseSnap) {
					snapTurn += vr_snapTurn;
					if (vr_snapTurn > 10.0f) {
						decreaseSnap = false;
					}
				} else if (joy > -0.4f) {
					decreaseSnap = true;
				}

				if (snapTurn < -180.0f) {
					snapTurn += 360.f;
				}
				else if (snapTurn > 180.0f) {
					snapTurn -= 360.f;
				}
			}
			else
			{
				analogTurnRateDegPerSec = 0.0f;
				lastAnalogTurnTime = 0;
			}

			//Menu button - invoke menu
			// Joy_GenerateButtonEvents(
			// 	((pPrimaryTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_ApplicationMenu)) != 0) && dominantGripPushedOld ? 1 : 0,
			// 	((pPrimaryTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_ApplicationMenu)) != 0) && dominantGripPushedNew ? 1 : 0,
			// 	1, KEY_ESCAPE);
				
		}  // in game section

			static int joy_mode = vr_joy_mode;
			if (joy_mode == 1) 
			{
				const bool suppressSelectAsKey = openvrMenuSuppressSelectAsKey;

				//if in cinema mode, then the dominant joystick is used differently
				if (!VR_UseCinematicScreenLayer() && axisJoystick != -1) 
				{
				//Default this is Weapon Chooser - This _could_ be remapped
				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].y < -0.7f && !dominantGripPushedOld ? 1 : 0), 
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].y < -0.7f && !dominantGripPushedNew ? 1 : 0), 
					1, KEY_MWHEELDOWN);

				//Default this is Weapon Chooser - This _could_ be remapped
				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].y > 0.7f && !dominantGripPushedOld ? 1 : 0), 
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].y > 0.7f && !dominantGripPushedNew ? 1 : 0), 
					1, KEY_MWHEELUP);

				//If snap turn set to 0, then we can use left/right on the stick as mappable functions
				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].x > 0.7f && !dominantGripPushedOld && !vr_snapTurn ? 1 : 0),
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].x > 0.7f && !dominantGripPushedNew && !vr_snapTurn ? 1 : 0),
					1, KEY_MWHEELLEFT);

				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].x < -0.7f && !dominantGripPushedOld && !vr_snapTurn ? 1 : 0),
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].x < -0.7f && !dominantGripPushedNew && !vr_snapTurn ? 1 : 0),
					1, KEY_MWHEELRIGHT);
			}

			//Dominant Hand - Primary keys (no grip pushed) - All keys are re-mappable, default bindngs are shown below
			{
				//"Use" (open door, toggle switch etc)
				Joy_GenerateButtonEvents(
					((primaryButtonsOld & (1ull << primaryButton1)) != 0) && !dominantGripPushedOld ? 1 : 0,
					((primaryButtonsNew & (1ull << primaryButton1)) != 0) && !dominantGripPushedNew ? 1 : 0,
					1, KEY_PAD_A);

				//No Binding
				Joy_GenerateButtonEvents(
					((primaryButtonsOld & (1ull << primaryButton2)) != 0) && !dominantGripPushedOld ? 1 : 0,
					((primaryButtonsNew & (1ull << primaryButton2)) != 0) && !dominantGripPushedNew ? 1 : 0,
					1, KEY_PAD_B);

				// Inv Use
				Joy_GenerateButtonEvents(
					((pDominantTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis0)) != 0) && !dominantGripPushedOld ? 1 : 0,
					((pDominantTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis0)) != 0) && !dominantGripPushedNew ? 1 : 0,
					1, KEY_ENTER);

					//Fire
					Joy_GenerateButtonEvents(
						((pDominantTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis1)) != 0) && !dominantGripPushedOld && !suppressSelectAsKey ? 1 : 0,
						((pDominantTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis1)) != 0) && !dominantGripPushedNew && !suppressSelectAsKey ? 1 : 0,
						1, KEY_PAD_RTRIGGER);

				// Joy_GenerateButtonEvents(
				// 	((pDominantTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis2)) != 0) && !dominantGripPushedOld ? 1 : 0,
				// 	((pDominantTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis2)) != 0) && !dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_F1);

				// Joy_GenerateButtonEvents(
				// 	((pDominantTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis3)) != 0) && !dominantGripPushedOld ? 1 : 0,
				// 	((pDominantTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis3)) != 0) && !dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_F5);

				//No Default Binding
				// Joy_GenerateButtonEvents(
				// 	((pDominantTrackedRemoteOld->Touches & xrButton_ThumbRest) != 0) && !dominantGripPushedOld ? 1 : 0,
				// 	((pDominantTrackedRemoteNew->Touches & xrButton_ThumbRest) != 0) && !dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_JOY5);

				//Use grip as an extra button
				//Alt-Fire
				Joy_GenerateButtonEvents(
					((pDominantTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Grip)) != 0) && !dominantGripPushedOld ? 1 : 0,
					((pDominantTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Grip)) != 0) && !dominantGripPushedNew ? 1 : 0,
					1, KEY_PAD_LTRIGGER);
			}
			
			//Dominant Hand - Secondary keys (grip pushed)
			{
				//Crouch
				Joy_GenerateButtonEvents(
					((primaryButtonsOld & (1ull << primaryButton1)) != 0) && dominantGripPushedOld ? 1 : 0,
					((primaryButtonsNew & (1ull << primaryButton1)) != 0) && dominantGripPushedNew ? 1 : 0,
					1, KEY_PAD_LTHUMB);

				//Main Menu
				Joy_GenerateButtonEvents(
					((primaryButtonsOld & (1ull << primaryButton2)) != 0) && dominantGripPushedOld ? 1 : 0,
					((primaryButtonsNew & (1ull << primaryButton2)) != 0) && dominantGripPushedNew ? 1 : 0,
					1, KEY_BACKSPACE);

				//No Binding
				Joy_GenerateButtonEvents(
					((pDominantTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis0)) != 0) && dominantGripPushedOld ? 1 : 0,
					((pDominantTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis0)) != 0) && dominantGripPushedNew ? 1 : 0,
					1, KEY_TAB);

					//Alt-Fire
					Joy_GenerateButtonEvents(
						((pDominantTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis1)) != 0) && dominantGripPushedOld && !suppressSelectAsKey ? 1 : 0,
						((pDominantTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis1)) != 0) && dominantGripPushedNew && !suppressSelectAsKey ? 1 : 0,
						1, KEY_PAD_LTRIGGER);

				//No Default Binding
				// Joy_GenerateButtonEvents(
				// 	((pDominantTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis2)) != 0) && dominantGripPushedOld ? 1 : 0,
				// 	((pDominantTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis2)) != 0) && dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_F3);

				//No Default Binding
				// Joy_GenerateButtonEvents(
				// 	((pDominantTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis3)) != 0) && dominantGripPushedOld ? 1 : 0,
				// 	((pDominantTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis3)) != 0) && dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_F6);

				//No Default Binding
				// Joy_GenerateButtonEvents(
				// 	((pDominantTrackedRemoteOld->Touches & xrButton_ThumbRest) != 0) && dominantGripPushedOld ? 1 : 0,
				// 	((pDominantTrackedRemoteNew->Touches & xrButton_ThumbRest) != 0) && dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_JOY6);

			}


			//Off Hand - Primary keys (no grip pushed)
			{
				//No Default Binding
				Joy_GenerateButtonEvents(
					((secondaryButtonsOld & (1ull << secondaryButton1)) != 0) && !dominantGripPushedOld ? 1 : 0,
					((secondaryButtonsNew & (1ull << secondaryButton1)) != 0) && !dominantGripPushedNew ? 1 : 0,
					1, KEY_PAD_X);

				//Toggle Map
				Joy_GenerateButtonEvents(
					((secondaryButtonsOld & (1ull << secondaryButton2)) != 0) && !dominantGripPushedOld ? 1 : 0,
					((secondaryButtonsNew & (1ull << secondaryButton2)) != 0) && !dominantGripPushedNew ? 1 : 0,
					1, KEY_PAD_Y);

				//"Use" (open door, toggle switch etc) - Can be rebound for other uses
				Joy_GenerateButtonEvents(
					((pOffTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis0)) != 0) && !dominantGripPushedOld ? 1 : 0,
					((pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis0)) != 0) && !dominantGripPushedNew ? 1 : 0,
					1, KEY_SPACE);

				//No Default Binding
				Joy_GenerateButtonEvents(
					((pOffTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis1)) != 0) && !dominantGripPushedOld ? 1 : 0,
					((pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis1)) != 0) && !dominantGripPushedNew ? 1 : 0,
					1, KEY_LSHIFT);

				//No Default Binding
				// Joy_GenerateButtonEvents(
				// 	((pOffTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis2)) != 0) && !dominantGripPushedOld ? 1 : 0,
				// 	((pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis2)) != 0) && !dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_F2);

				//No Default Binding
				// Joy_GenerateButtonEvents(
				// 	((pOffTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis3)) != 0) && !dominantGripPushedOld ? 1 : 0,
				// 	((pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis3)) != 0) && !dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_F7);

				//No Default Binding
				// Joy_GenerateButtonEvents(
				// 	((pOffTrackedRemoteOld->Touches & xrButton_ThumbRest) != 0) && !dominantGripPushedOld ? 1 : 0,
				// 	((pOffTrackedRemoteNew->Touches & xrButton_ThumbRest) != 0) && !dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_JOY7);

				Joy_GenerateButtonEvents(
					((pOffTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Grip)) != 0) && !dominantGripPushedOld ? 1 : 0,
					((pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Grip)) != 0) && !dominantGripPushedNew ? 1 : 0,
					1, KEY_PAD_RTHUMB);
			}

			//Off Hand - Secondary keys (grip pushed)
			{
				//Move Down
				Joy_GenerateButtonEvents(
					((secondaryButtonsOld & (1ull << secondaryButton1)) != 0) && dominantGripPushedOld ? 1 : 0,
					((secondaryButtonsNew & (1ull << secondaryButton1)) != 0) && dominantGripPushedNew ? 1 : 0,
					1, KEY_PGDN);

				//Move Up
				Joy_GenerateButtonEvents(
					((secondaryButtonsOld & (1ull << secondaryButton2)) != 0) && dominantGripPushedOld ? 1 : 0,
					((secondaryButtonsNew & (1ull << secondaryButton2)) != 0) && dominantGripPushedNew ? 1 : 0,
					1, KEY_PGUP);

				//Land
				Joy_GenerateButtonEvents(
					((pOffTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis0)) != 0) && dominantGripPushedOld ? 1 : 0,
					((pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis0)) != 0) && dominantGripPushedNew ? 1 : 0,
					1, KEY_HOME);

				//No Default Binding
				Joy_GenerateButtonEvents(
					((pOffTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis1)) != 0) && dominantGripPushedOld ? 1 : 0,
					((pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis1)) != 0) && dominantGripPushedNew ? 1 : 0,
					1, KEY_LALT);

				//No Default Binding
				// Joy_GenerateButtonEvents(
				// 	((pOffTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis2)) != 0) && dominantGripPushedOld ? 1 : 0,
				// 	((pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis2)) != 0) && dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_F4);

				//No Default Binding
				// Joy_GenerateButtonEvents(
				// 	((pOffTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis3)) != 0) && dominantGripPushedOld ? 1 : 0,
				// 	((pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Axis3)) != 0) && dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_F8);

				//No Default Binding
				// Joy_GenerateButtonEvents(
				// 	((pOffTrackedRemoteOld->Touches & xrButton_ThumbRest) != 0) && dominantGripPushedOld ? 1 : 0,
				// 	((pOffTrackedRemoteNew->Touches & xrButton_ThumbRest) != 0) && dominantGripPushedNew ? 1 : 0,
				// 	1, KEY_JOY8);

				Joy_GenerateButtonEvents(
					((pOffTrackedRemoteOld->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Grip)) != 0) && dominantGripPushedOld && !vr_two_handed_weapons ? 1 : 0,
					((pOffTrackedRemoteNew->ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Grip)) != 0) && dominantGripPushedNew && !vr_two_handed_weapons ? 1 : 0,
					1, KEY_PAD_DPAD_UP);
			}

			if (axisTrackpad != -1)
			{
				if (menuactive != MENU_Off && menuactive != MENU_WaitKey)
				{
					Joy_GenerateButtonEvents(
						(pPrimaryTrackedRemoteOld->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						(pPrimaryTrackedRemoteNew->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						1, GK_LEFT);

					Joy_GenerateButtonEvents(
						(pPrimaryTrackedRemoteOld->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						(pPrimaryTrackedRemoteNew->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						1, GK_RIGHT);

					Joy_GenerateButtonEvents(
						(pPrimaryTrackedRemoteOld->rAxis[axisTrackpad].y < -DEAD_ZONE ? 1 : 0), 
						(pPrimaryTrackedRemoteNew->rAxis[axisTrackpad].y < -DEAD_ZONE ? 1 : 0), 
						1, GK_DOWN);

					Joy_GenerateButtonEvents(
						(pPrimaryTrackedRemoteOld->rAxis[axisTrackpad].y > DEAD_ZONE ? 1 : 0), 
						(pPrimaryTrackedRemoteNew->rAxis[axisTrackpad].y > DEAD_ZONE ? 1 : 0), 
						1, GK_UP);

					Joy_GenerateButtonEvents(
						(pSecondaryTrackedRemoteOld->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						(pSecondaryTrackedRemoteNew->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						1, GK_LEFT);

					Joy_GenerateButtonEvents(
						(pSecondaryTrackedRemoteOld->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						(pSecondaryTrackedRemoteNew->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						1, GK_RIGHT);

					Joy_GenerateButtonEvents(
						(pSecondaryTrackedRemoteOld->rAxis[axisTrackpad].y < -DEAD_ZONE ? 1 : 0), 
						(pSecondaryTrackedRemoteNew->rAxis[axisTrackpad].y < -DEAD_ZONE ? 1 : 0), 
						1, GK_DOWN);

					Joy_GenerateButtonEvents(
						(pSecondaryTrackedRemoteOld->rAxis[axisTrackpad].y > DEAD_ZONE ? 1 : 0), 
						(pSecondaryTrackedRemoteNew->rAxis[axisTrackpad].y > DEAD_ZONE ? 1 : 0), 
						1, GK_UP);

				}
				else {
					Joy_GenerateButtonEvents(
						(pPrimaryTrackedRemoteOld->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						(pPrimaryTrackedRemoteNew->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						1, KEY_PAD_LTHUMB_LEFT);

					Joy_GenerateButtonEvents(
						(pPrimaryTrackedRemoteOld->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						(pPrimaryTrackedRemoteNew->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						1, KEY_PAD_LTHUMB_RIGHT);

					Joy_GenerateButtonEvents(
						(pPrimaryTrackedRemoteOld->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						(pPrimaryTrackedRemoteNew->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						1, KEY_PAD_LTHUMB_UP);

					Joy_GenerateButtonEvents(
						(pPrimaryTrackedRemoteOld->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						(pPrimaryTrackedRemoteNew->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						1, KEY_PAD_LTHUMB_DOWN);

					Joy_GenerateButtonEvents(
						(pSecondaryTrackedRemoteOld->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						(pSecondaryTrackedRemoteNew->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						1, KEY_PAD_RTHUMB_LEFT);

					Joy_GenerateButtonEvents(
						(pSecondaryTrackedRemoteOld->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						(pSecondaryTrackedRemoteNew->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						1, KEY_PAD_RTHUMB_RIGHT);

					Joy_GenerateButtonEvents(
						(pSecondaryTrackedRemoteOld->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						(pSecondaryTrackedRemoteNew->rAxis[axisTrackpad].x > DEAD_ZONE ? 1 : 0), 
						1, KEY_PAD_RTHUMB_UP);

					Joy_GenerateButtonEvents(
						(pSecondaryTrackedRemoteOld->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						(pSecondaryTrackedRemoteNew->rAxis[axisTrackpad].x < -DEAD_ZONE ? 1 : 0), 
						1, KEY_PAD_RTHUMB_DOWN);

				}
			}

			if (axisJoystick != -1)
			{
				Joy_GenerateButtonEvents(
					(pSecondaryTrackedRemoteOld->rAxis[axisJoystick].x > 0.7f && !dominantGripPushedOld ? 1 : 0), 
					(pSecondaryTrackedRemoteNew->rAxis[axisJoystick].x > 0.7f && !dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS1PLUS);

				Joy_GenerateButtonEvents(
					(pSecondaryTrackedRemoteOld->rAxis[axisJoystick].x < -0.7f && !dominantGripPushedOld ? 1 : 0), 
					(pSecondaryTrackedRemoteNew->rAxis[axisJoystick].x < -0.7f && !dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS1MINUS);

				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].x > 0.7f && !dominantGripPushedOld ? 1 : 0), 
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].x > 0.7f && !dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS3PLUS);

				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].x < -0.7f && !dominantGripPushedOld ? 1 : 0), 
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].x < -0.7f && !dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS3MINUS);

				Joy_GenerateButtonEvents(
					(pSecondaryTrackedRemoteOld->rAxis[axisJoystick].y < -0.7f && !dominantGripPushedOld ? 1 : 0), 
					(pSecondaryTrackedRemoteNew->rAxis[axisJoystick].y < -0.7f && !dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS2MINUS);

				Joy_GenerateButtonEvents(
					(pSecondaryTrackedRemoteOld->rAxis[axisJoystick].y > 0.7f && !dominantGripPushedOld ? 1 : 0), 
					(pSecondaryTrackedRemoteNew->rAxis[axisJoystick].y > 0.7f && !dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS2PLUS);
				
				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].y < -0.7f && !dominantGripPushedOld ? 1 : 0), 
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].y < -0.7f && !dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS4MINUS);

				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].y > 0.7f && !dominantGripPushedOld ? 1 : 0), 
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].y > 0.7f && !dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS4PLUS);

				Joy_GenerateButtonEvents(
					(pSecondaryTrackedRemoteOld->rAxis[axisJoystick].x > 0.7f && dominantGripPushedOld ? 1 : 0), 
					(pSecondaryTrackedRemoteNew->rAxis[axisJoystick].x > 0.7f && dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS5PLUS);

				Joy_GenerateButtonEvents(
					(pSecondaryTrackedRemoteOld->rAxis[axisJoystick].x < -0.7f && dominantGripPushedOld ? 1 : 0), 
					(pSecondaryTrackedRemoteNew->rAxis[axisJoystick].x < -0.7f && dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS5MINUS);

				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].x > 0.7f && dominantGripPushedOld ? 1 : 0), 
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].x > 0.7f && dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS7PLUS);

				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].x < -0.7f && dominantGripPushedOld ? 1 : 0), 
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].x < -0.7f && dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS7MINUS);

				Joy_GenerateButtonEvents(
					(pSecondaryTrackedRemoteOld->rAxis[axisJoystick].y < -0.7f && dominantGripPushedOld ? 1 : 0), 
					(pSecondaryTrackedRemoteNew->rAxis[axisJoystick].y < -0.7f && dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS6MINUS);

				Joy_GenerateButtonEvents(
					(pSecondaryTrackedRemoteOld->rAxis[axisJoystick].y > 0.7f && dominantGripPushedOld ? 1 : 0), 
					(pSecondaryTrackedRemoteNew->rAxis[axisJoystick].y > 0.7f && dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS6PLUS);
				
				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].y < -0.7f && dominantGripPushedOld ? 1 : 0), 
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].y < -0.7f && dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS8MINUS);

				Joy_GenerateButtonEvents(
					(pPrimaryTrackedRemoteOld->rAxis[axisJoystick].y > 0.7f && dominantGripPushedOld ? 1 : 0), 
					(pPrimaryTrackedRemoteNew->rAxis[axisJoystick].y > 0.7f && dominantGripPushedNew ? 1 : 0), 
					1, KEY_JOYAXIS8PLUS);
			}
		}

		//Save state
		pDominantTracking->lastState = rightTrackedRemoteState_old = rightTrackedRemoteState_new;
		pOffTracking->lastState = leftTrackedRemoteState_old = leftTrackedRemoteState_new;
	}

	// Teleport location where player sprite will be shown
	bool OpenVRMode::GetTeleportLocation(DVector3& out) const
	{
		player_t* player = r_viewpoint.camera ? r_viewpoint.camera->player : nullptr;
		if (vr_teleport &&
			ready_teleport &&
			(player && player->mo->health > 0) &&
			m_TeleportTarget == TRACE_HitFloor) {
			out = m_TeleportLocation;
			return true;
		}

		return false;
	}

	VRControllerState_t& OpenVR_GetState(int hand)
	{
		bool rightHanded = vr_control_scheme < 10;
		int controller = rightHanded ? hand : 1 - hand;
		return controllers[controller].lastState;
	}


	int OpenVR_GetTouchPadAxis()
	{
		return axisTrackpad;
	}

	int OpenVR_GetJoystickAxis()
	{
		return axisJoystick;
	}

	bool OpenVR_OnHandIsRight()
	{
		return vr_control_scheme < 10;
	}

	bool JustStoppedMoving(VRControllerState_t& lastState, VRControllerState_t& newState, int axis)
	{
		if (axis != -1)
		{
			bool wasMoving = (abs(lastState.rAxis[axis].x) > DEAD_ZONE || abs(lastState.rAxis[axis].y) > DEAD_ZONE);
			bool isMoving = (abs(newState.rAxis[axis].x) > DEAD_ZONE || abs(newState.rAxis[axis].y) > DEAD_ZONE);
			return !isMoving && wasMoving;
		}
		return false;
	}

	/* virtual */
	void OpenVRMode::SetUp() const
	{
		super::SetUp();

		if (vrCompositor == nullptr || vrSystem == nullptr || !hmdWasFound)
			return;

		// Set VR-appropriate settings
		const bool doAdjustVrSettings = true;
		if (doAdjustVrSettings) {
			// movebob = 0;
			// gl_billboard_faces_camera = true;
		}

		UpdateOverlaySettings();

		haptics->ProcessHaptics();

		if (gamestate == GS_LEVEL && menuactive == MENU_Off && !paused && ConsoleState == c_up && !VR_IsNetWaitShellActive()) {
			cachedScreenBlocks = screenblocks;
			screenblocks = 12; // always be full-screen during 3D scene render
			QzDoom_setUseScreenLayer(false);
		}
		else {
			// Ensure we are drawing on virtual screen
			QzDoom_setUseScreenLayer(true);
		}
		
		static TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
		
		// Keep the HMD fed on title/menu screens as well as in-game.
		// The menu lives in GS_TITLELEVEL, so skipping this block makes VR appear to freeze.
		const int eyeCount = mEyeCount;
		GLRenderer->mBuffers->CurrentEye() = 0;  // always begin at zero, in case eye count changed
		for (int eye_ix = 0; eye_ix < eyeCount; ++eye_ix)
		{
			GLRenderer->mBuffers->BindCurrentFB();
			glClearColor(0.f, 0.f, 0.f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			GLRenderer->mBuffers->NextEye(eyeCount);
		}

		if (vrCompositor != nullptr) {
			vrCompositor->WaitGetPoses(
				poses, k_unMaxTrackedDeviceCount, // current pose
				nullptr, 0 // future pose?
			);
		}

		TrackedDevicePose_t& hmdPose0 = poses[k_unTrackedDeviceIndex_Hmd];
		
		if (hmdPose0.bPoseIsValid) {
			const HmdMatrix34_t& hmdPose = hmdPose0.mDeviceToAbsoluteTracking;
			if (openvrHadPrevHmdPoseForRecenter)
			{
				const float prevYaw = YawFromPoseDeg(openvrPrevHmdPoseForRecenter);
				const float currYaw = YawFromPoseDeg(hmdPose);
				const float yawDelta = fabsf(ShortestAngleDeltaDeg(currYaw, prevYaw));
				const float prevY = openvrPrevHmdPoseForRecenter.m[1][3];
				const float currY = hmdPose.m[1][3];
				const float heightDelta = fabsf(currY - prevY);
				if ((vr_overlayscreen == 1 || vr_overlayscreen == 2) && (yawDelta > 25.0f || heightDelta > 0.20f))
				{
					// Respect runtime recenter / tracking-origin reset for stationary modes.
					openvrOverlayAnchorValid = false;
				}
			}
			openvrPrevHmdPoseForRecenter = hmdPose;
			openvrHadPrevHmdPoseForRecenter = true;
			openvrLatestHmdPose = hmdPose;
			openvrHasLatestHmdPose = true;
			HmdVector3d_t eulerAngles = eulerAnglesFromMatrix(hmdPose);

			// TODO we should prepare the hmd pos and orientation here
			VR_SetHMDPosition(hmdPose.m[0][3], hmdPose.m[1][3], hmdPose.m[2][3]);
			VR_SetHMDOrientation(RAD2DEG(eulerAngles.v[1]), RAD2DEG(eulerAngles.v[0]), RAD2DEG(eulerAngles.v[2]));
			
			leftEyeView->setCurrentHmdPose(&hmdPose0);
			rightEyeView->setCurrentHmdPose(&hmdPose0);

			player_t* player = r_viewpoint.camera ? r_viewpoint.camera->player : nullptr;

			// Check for existence of VR motion controllers...
			for (int i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
				if (i == k_unTrackedDeviceIndex_Hmd)
					continue; // skip the headset position
				TrackedDevicePose_t& pose = poses[i];
				if (!pose.bDeviceIsConnected)
					continue;
				if (!pose.bPoseIsValid)
					continue;
				ETrackedDeviceClass device_class = vrSystem->GetTrackedDeviceClass(i);
				if (device_class != ETrackedDeviceClass_TrackedDeviceClass_Controller)
					continue; // controllers only, please

				int role = vrSystem->GetControllerRoleForTrackedDeviceIndex(i) - openvr::ETrackedControllerRole_TrackedControllerRole_LeftHand;
				if (role >= 0 && role < MAX_ROLES)
				{
					char model_chars[101];
					ETrackedPropertyError propertyError;
					vrSystem->GetStringTrackedDeviceProperty(i, ETrackedDeviceProperty_Prop_RenderModelName_String, model_chars, 100, &propertyError);
					if (propertyError != ETrackedPropertyError_TrackedProp_Success)
						continue; // something went wrong...
					std::string model_name(model_chars);
					if (controllerMeshes.count(model_name) == 0) {
						controllerMeshes[model_name] = VRControllerModel(model_name, vrRenderModels);
						assert(controllerMeshes.count(model_name) == 1);
					}
					controllers[role].index = i;
					controllers[role].active = true;
					controllers[role].pose = pose;
					if (haptics != nullptr)
					{
						haptics->UpdateControllerId(role, i);
					}
					if (controllerMeshes[model_name].isLoaded())
					{
						controllers[role].model = &controllerMeshes[model_name];
					}
					VRControllerState_t newState;
					vrSystem->GetControllerState(i, &newState, sizeof(newState));

					if (role == 0)
						leftTrackedRemoteState_new = newState;
					else if (role == 1)
						rightTrackedRemoteState_new = newState;


					if (!identifiedAxes)
					{
						identifiedAxes = true;
						for (int a = 0; a < k_unControllerStateAxisCount; a++)
						{
							switch (vrSystem->GetInt32TrackedDeviceProperty(i, (ETrackedDeviceProperty)(vr::Prop_Axis0Type_Int32 + a), 0))
							{
							case vr::k_eControllerAxis_TrackPad:
								if (axisTrackpad == -1) axisTrackpad = a;
								break;
							case vr::k_eControllerAxis_Joystick:
								if (axisJoystick == -1) axisJoystick = a;
								break;
							case vr::k_eControllerAxis_Trigger:
								if (axisTrigger == -1) axisTrigger = a;
								break;
							}
						}
					}
					static int joy_mode = vr_joy_mode;
					if (joy_mode == 0)
					{
						HandleControllerState(i, role, newState);
					}
				}
			}

			//Some crazy stuff to ascertain the actual yaw that doom is using at the right times!
			if (gamestate == GS_LEVEL && resetDoomYaw && r_viewpoint.camera != nullptr)
			{
				doomYaw = (float)r_viewpoint.camera->Angles.Yaw.Degrees();
				resetDoomYaw = false;
				resetPreviousHmdYaw = true;
			}
			else if (gamestate != GS_LEVEL || menuactive != MENU_Off 
			|| ConsoleState == c_down || ConsoleState == c_falling 
			|| (player && player->playerstate == PST_DEAD)
			|| (player && player->resetDoomYaw)
			|| paused 
			)
			{
				resetDoomYaw = true;
				resetPreviousHmdYaw = true;
			}

			if (gamestate == GS_LEVEL && menuactive == MENU_Off)
			{
				if (player && player->mo)
				{
					double pixelstretch = level.info ? level.info->pixelstretch : 1.2;

					// Thanks to Emawind for the codes for natural crouching
					if (!multiplayer)
					{
						if (!vr_crouch_use_button)
						{
							static double defaultViewHeight = player->DefaultViewHeight();
							player->crouching = 10;
							player->crouchfactor = HmdHeight / defaultViewHeight;
						}
						else if (player->crouching == 10)
						{
							player->Uncrouch();
						}
					}
					else if (!vr_crouch_use_button)
					{
						VR_SetMultiplayerCrouchHeight((float)HmdHeight);
					}
					else
					{
						VR_ClearMultiplayerCrouchHeight();
					}

					LSMatrix44 mat;
					if (GetWeaponTransform(&mat, VR_MAINHAND))
					{
						getMainHandAngles();
						if (!multiplayer)
						{
							player->mo->AttackPos.X = mat[3][0];
							player->mo->AttackPos.Y = mat[3][2];
							player->mo->AttackPos.Z = mat[3][1];
							player->mo->AttackPitch = DAngle::fromDeg(VR_UseCinematicScreenLayer() ?
								-weaponangles[PITCH] - r_viewpoint.Angles.Pitch.Degrees() :
								-weaponangles[PITCH]);
							player->mo->AttackAngle = DAngle::fromDeg(-90 + getViewpointYaw() + (weaponangles[YAW] - playerYaw));
							player->mo->AttackRoll = DAngle::fromDeg(weaponangles[ROLL]);
							// Same value, kept somewhere the playsim will not zero it.
							player->mo->MainHandRoll = player->mo->AttackRoll;
						}
					}

					LSMatrix44 matOffhand;
					if (!multiplayer && GetWeaponTransform(&matOffhand, VR_OFFHAND))
					{
						player->mo->OffhandPos.X = matOffhand[3][0];
						player->mo->OffhandPos.Y = matOffhand[3][2];
						player->mo->OffhandPos.Z = matOffhand[3][1];

						getOffHandAngles();

						player->mo->OffhandPitch = DAngle::fromDeg(VR_UseCinematicScreenLayer() ? 
							-offhandangles[PITCH] - r_viewpoint.Angles.Pitch.Degrees() :
							-offhandangles[PITCH]);
						player->mo->OffhandAngle = DAngle::fromDeg(-90 + getViewpointYaw() + (offhandangles[YAW] - hmdorientation[YAW]));
						player->mo->OffhandRoll = DAngle::fromDeg(offhandangles[ROLL]);
					}

					// Teleport locomotion. Thanks to DrBeef for the codes
					if (vr_teleport && player->mo->health > 0) {

						DAngle yaw = DAngle::fromDeg(getViewpointYaw() - hmdorientation[YAW] + offhandangles[YAW]);
						DAngle pitch = DAngle::fromDeg(offhandangles[PITCH]);
						double pixelstretch = level.info ? level.info->pixelstretch : 1.2;

						// Teleport Logic
						if (ready_teleport) {
							FLineTraceData trace;
							if (P_LineTrace(player->mo, yaw, 8192, pitch, TRF_ABSOFFSET|TRF_BLOCKUSE|TRF_BLOCKSELF|TRF_SOLIDACTORS,
											((hmdPosition[1] + offhandoffset[1] + vr_height_adjust) *
											VR_UnitsPerMeter()) / pixelstretch,
											-(offhandoffset[2] * VR_UnitsPerMeter()),
											-(offhandoffset[0] * VR_UnitsPerMeter()), &trace))
							{
								m_TeleportTarget = trace.HitType;
								m_TeleportLocation = trace.HitLocation;
							}
							else {
								m_TeleportTarget = TRACE_HitNone;
								m_TeleportLocation = DVector3(0, 0, 0);
							}
						}
						else if (trigger_teleport && m_TeleportTarget == TRACE_HitFloor) {
							if (multiplayer)
							{
								QueueMultiplayerTeleportTarget(player, m_TeleportLocation);
							}
							else
							{
								auto vel = player->mo->Vel;
								player->mo->Vel = DVector3(m_TeleportLocation.X - player->mo->X(),
									m_TeleportLocation.Y - player->mo->Y(), 0);
								bool wasOnGround = player->mo->Z() <= player->mo->floorz + 0.1;
								double oldZ = player->mo->Z();
								P_XYMovement(player->mo, DVector2(0, 0));

								//if we were on the ground before offsetting, make sure we still are (this fixes not being able to move on lifts)
								if (player->mo->Z() >= oldZ && wasOnGround) {
									player->mo->SetZ(player->mo->floorz);
								}
								else {
									player->mo->SetZ(oldZ);
								}
								player->mo->Vel = vel;
							}

							m_TeleportTarget = TRACE_HitNone;
							m_TeleportLocation = DVector3(0, 0, 0);
						}

						trigger_teleport = false;
					}

					//Positional Movement
					float hmd_forward=0;
					float hmd_side=0;
					float dummy=0;
					VR_GetMove(&dummy, &dummy, &hmd_forward, &hmd_side, &dummy, &dummy, &dummy, &dummy);
					
					if (!multiplayer)
					{
						// Roomscale/HMD positional locomotion stays local to single-player until it has
						// an explicit deterministic netplay contract.
						auto vel = player->mo->Vel;
						player->mo->Vel = DVector3((DVector2(hmd_side, hmd_forward) * VR_UnitsPerMeter()), 0);
						//player->mo->Vel = DVector3((DVector2(-openvr_dpos.x, openvr_dpos.z) * vr_vunits_per_meter).Rotated(openvr_to_doom_angle), 0);
						bool wasOnGround = player->mo->Z() <= player->mo->floorz;
						float oldZ = player->mo->Z();
						P_XYMovement(player->mo, DVector2(0, 0));

						//if we were on the ground before offsetting, make sure we still are (this fixes not being able to move on lifts)
						if (player->mo->Z() >= oldZ && wasOnGround)
						{
							player->mo->SetZ(player->mo->floorz);
						}
						else
						{
							player->mo->SetZ(oldZ);
						}
						player->mo->Vel = vel;
					}
					if (!multiplayer)
					{
						// In single-player we convert roomscale deltas into pawn motion,
						// so the local HMD anchor advances with the consumed movement.
						openvr_origin += openvr_dpos;
					}
				}
				updateHmdPose(r_viewpoint);
			}  // not in menu section

#if 0
			// we will disable overlay mode based on controller pitch
			float controller1Pitch = DAngle::fromDeg(offhandangles[PITCH]).Degrees();
			float controller2Pitch = DAngle::fromDeg(weaponangles[PITCH]).Degrees();

			if (vr_overlayscreen > 0 && menuactive == MENU_On &&
				(controller1Pitch > 60 || controller1Pitch < -60 || controller2Pitch > 60 || controller2Pitch < -60)
				)
				forceDisableOverlay = true;
			else
				forceDisableOverlay = false;
#endif
			if (VR_UseScreenLayer() || ConsoleState != c_up)
			{
				forceDisableOverlay = false;
			}
				const bool overlayVisibleNow = VR_IsNetWaitShellActive() || VR_UseScreenLayer() || gamestate == GS_TITLELEVEL || menuactive != MENU_Off || ConsoleState != c_up;
				if ((vr_overlayscreen == 1 || vr_overlayscreen == 2) && overlayVisibleNow && !openvrOverlayWasVisible)
				{
					// Re-anchor stationary overlay whenever virtual screen is (re)entered.
					openvrOverlayAnchorValid = false;
				}
				openvrOverlayWasVisible = overlayVisibleNow;
				if (overlayVisibleNow)
				{
					UpdateOverlaySettings();
				}

				// Prevent click bleed-through across menu transitions (e.g. quit confirm -> main menu).
				if (CurrentMenu != openvrLastMenuContext)
				{
					if (openvrMenuPointerLastTriggerDown)
					{
						PostGuiMouseEvent(EV_GUI_LButtonUp, (int)openvrMenuPointerLastX, (int)openvrMenuPointerLastY);
						openvrMenuPointerLastTriggerDown = false;
					}
					openvrMenuSuppressTriggerUntilRelease = true;
					openvrLastMenuContext = CurrentMenu;
				}

				// OpenVR virtual menu mouse (right controller): keep behavior aligned
				// with OpenXR (master enable + optional hold-grip activation).
				openvrMenuPointerActive = false;
				openvrMenuPointerBeamVisible = false;
				openvrMenuSuppressSelectAsKey = false;
				menu_allow_mouse_override = false;
				const bool keybindCaptureMode = menuactive == MENU_WaitKey;
				// Enable virtual mouse only while explicit GUI is open.
				// This avoids pointer/beam during boot logo or idle title screen.
				const bool guiMouseContext = (menuactive != MENU_Off) || (ConsoleState != c_up);
				const bool menuMode = guiMouseContext && (vr_overlayscreen || vr_overlayscreen_always);
				const bool rightControllerValid = controllers[1].active && controllers[1].pose.bPoseIsValid;
				const bool rightGripDown = (rightTrackedRemoteState_new.ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_Grip)) != 0;
				const bool vrMouseEnabled = !keybindCaptureMode && (*vr_menu_pointer) && ((*vr_mouse_in_menu) || rightGripDown);
				openvrMenuSuppressSelectAsKey = !keybindCaptureMode && menuMode && *vr_menu_pointer && (vrMouseEnabled || openvrMenuSuppressTriggerUntilRelease || openvrMenuPointerLastTriggerDown);
				if (menuMode && rightControllerValid && vrMouseEnabled)
				{
					const HmdMatrix34_t& overlayAbs = openvrOverlayAbsTransform;

					const HmdVector3_t rayOrigin = {
						controllers[1].pose.mDeviceToAbsoluteTracking.m[0][3],
						controllers[1].pose.mDeviceToAbsoluteTracking.m[1][3],
						controllers[1].pose.mDeviceToAbsoluteTracking.m[2][3]
					};
						const HmdVector3_t planeOrigin = { overlayAbs.m[0][3], overlayAbs.m[1][3], overlayAbs.m[2][3] };
						const HmdVector3_t planeNormal = Normalize3({ overlayAbs.m[0][2], overlayAbs.m[1][2], overlayAbs.m[2][2] });
						const HmdVector3_t planeRight = Normalize3({ overlayAbs.m[0][0], overlayAbs.m[1][0], overlayAbs.m[2][0] });
						const HmdVector3_t planeUp = Normalize3({ overlayAbs.m[0][1], overlayAbs.m[1][1], overlayAbs.m[2][1] });
						const HmdVector3_t controllerRight = Normalize3({
							controllers[1].pose.mDeviceToAbsoluteTracking.m[0][0],
							controllers[1].pose.mDeviceToAbsoluteTracking.m[1][0],
							controllers[1].pose.mDeviceToAbsoluteTracking.m[2][0]
						});
						const float baseOverlayWidthMeters = std::max(0.1f, (1.0f + vr_overlayscreen_size) * 0.8f);
						const HmdVector3_t overlayRightAxisRaw = { overlayAbs.m[0][0], overlayAbs.m[1][0], overlayAbs.m[2][0] };
						const float overlayRightScale = std::sqrt(std::max(0.0f, Dot3(overlayRightAxisRaw, overlayRightAxisRaw)));
						const float screenWidthMeters = baseOverlayWidthMeters * std::max(1.0f, overlayRightScale);
						const int renderW = std::max(1, screen->GetWidth());
						const int renderH = std::max(1, screen->GetHeight());
						const float screenHeightMeters = screenWidthMeters * ((float)renderH / (float)renderW * 1.15); // extra x1.5 fixes mouse vertical position
						const float halfW = screenWidthMeters * 0.56f; // x0.5 actually drifts mouse position horizontally
						const float halfH = screenHeightMeters * 0.5f;

					struct RayHit
					{
						bool valid = false;
						float t = 0.0f;
						float localX = 0.0f;
						float localY = 0.0f;
						float unclampedU = 0.0f;
						float unclampedV = 0.0f;
						float overflow = 0.0f;
						HmdVector3_t origin = { 0.0f, 0.0f, 0.0f };
						HmdVector3_t hit = { 0.0f, 0.0f, 0.0f };
						HmdVector3_t rayDir = { 0.0f, 0.0f, 0.0f };
					};

					auto testAxis = [&](const HmdVector3_t& rayDirIn) -> RayHit
					{
						RayHit out;
						HmdVector3_t rayDir = Normalize3(rayDirIn);
						const float biasDeg = (float)vr_openvr_menu_pointer_pitch_bias;
						if (fabsf(biasDeg) > 0.001f)
						{
							rayDir = Normalize3(RotateAroundAxis(rayDir, controllerRight, biasDeg * (float)(M_PI / 180.0)));
						}
						const float tipOffset = std::max(0.0f, (float)vr_openvr_menu_pointer_tip_offset);
						const HmdVector3_t rayStart = Add3(rayOrigin, Scale3(rayDir, tipOffset));
						const float denom = Dot3(rayDir, planeNormal);
						if (fabsf(denom) <= 1e-4f) return out;
						const float t = Dot3(Sub3(planeOrigin, rayStart), planeNormal) / denom;
						if (t <= 0.05f) return out;
						const HmdVector3_t hit = Add3(rayStart, Scale3(rayDir, t));
						const HmdVector3_t local = Sub3(hit, planeOrigin);
						const float lx = Dot3(local, planeRight);
						const float ly = Dot3(local, planeUp);
						const float u = (lx + halfW) / std::max(screenWidthMeters, 0.0001f);
						const float v = (halfH - ly) / std::max(screenHeightMeters, 0.0001f);
						const float overflow =
							(u < 0.0f ? -u : 0.0f) +
							(u > 1.0f ? u - 1.0f : 0.0f) +
							(v < 0.0f ? -v : 0.0f) +
							(v > 1.0f ? v - 1.0f : 0.0f);
						out.valid = true;
						out.t = t;
						out.localX = lx;
						out.localY = ly;
						out.unclampedU = u;
						out.unclampedV = v;
						out.overflow = overflow;
						out.origin = rayStart;
						out.hit = hit;
						out.rayDir = rayDir;
						return out;
					};

					const HmdVector3_t rayAxis = {
						-controllers[1].pose.mDeviceToAbsoluteTracking.m[0][2],
						-controllers[1].pose.mDeviceToAbsoluteTracking.m[1][2],
						-controllers[1].pose.mDeviceToAbsoluteTracking.m[2][2]
					};
					const RayHit bestHit = testAxis(rayAxis);
					if (bestHit.valid)
					{
						const bool inside = bestHit.overflow <= 0.0001f;
						// OpenVR overlay horizontal orientation: use raw U so drag direction
						// matches controller motion (mirrored U inverts slider dragging).
						float u = bestHit.unclampedU;
						// OpenVR overlay space already aligns with screen Y orientation here;
						// inverting V again makes pitch feel reversed.
						float v = bestHit.unclampedV;
						u = clamp(u, 0.0f, 1.0f);
						v = clamp(v, 0.0f, 1.0f);
						const int mouseX = clamp((int)std::lround(u * (renderW - 1)), 0, renderW - 1);
						const int mouseY = clamp((int)std::lround(v * (renderH - 1)), 0, renderH - 1);
						openvrMenuPointerBeamVisible = true;
						openvrMenuPointerBeamStart = bestHit.origin;
						openvrMenuPointerBeamEnd = bestHit.hit;
						openvrMenuPointerHit = bestHit.hit;
						const bool dragHoldActive = openvrMenuPointerLastTriggerDown;
						const bool allowClampedInteraction = inside || dragHoldActive;
						if (allowClampedInteraction)
						{
							menu_allow_mouse_override = true;
							openvrMenuPointerActive = true;
							if (mouseX != (int)openvrMenuPointerLastX || mouseY != (int)openvrMenuPointerLastY)
							{
								PostGuiMouseEvent(EV_GUI_MouseMove, mouseX, mouseY);
								openvrMenuPointerLastX = (float)mouseX;
								openvrMenuPointerLastY = (float)mouseY;
							}

							const bool triggerDown = axisTrigger >= 0
								? rightTrackedRemoteState_new.rAxis[axisTrigger].x > 0.75f
								: ((rightTrackedRemoteState_new.ulButtonPressed & ButtonMaskFromId(openvr::vr::k_EButton_SteamVR_Trigger)) != 0);
							if (openvrMenuSuppressTriggerUntilRelease)
							{
								if (!triggerDown)
								{
									openvrMenuSuppressTriggerUntilRelease = false;
								}
							}
							else if (triggerDown != openvrMenuPointerLastTriggerDown)
							{
								PostGuiMouseEvent(triggerDown ? EV_GUI_LButtonDown : EV_GUI_LButtonUp, mouseX, mouseY);
								openvrMenuPointerLastTriggerDown = triggerDown;
							}

							if (axisJoystick >= 0)
							{
								const float y = rightTrackedRemoteState_new.rAxis[axisJoystick].y;
								constexpr float wheelTrigger = 0.85f;
								constexpr float wheelReset = 0.35f;
								const double now = I_msTimeF();
								if (fabsf(y) < wheelReset)
									openvrMenuWheelNeutral = true;
								if (openvrMenuWheelNeutral && now >= openvrMenuWheelCooldownUntil)
								{
									if (y > wheelTrigger)
									{
										PostGuiWheelEvent(EV_GUI_WheelUp, mouseX, mouseY);
										openvrMenuWheelNeutral = false;
										openvrMenuWheelCooldownUntil = now + 160.0;
									}
									else if (y < -wheelTrigger)
									{
										PostGuiWheelEvent(EV_GUI_WheelDown, mouseX, mouseY);
										openvrMenuWheelNeutral = false;
										openvrMenuWheelCooldownUntil = now + 160.0;
									}
								}
							}
						}
					}
				}

				if (!openvrMenuPointerActive && openvrMenuPointerLastTriggerDown)
				{
					PostGuiMouseEvent(EV_GUI_LButtonUp, (int)openvrMenuPointerLastX, (int)openvrMenuPointerLastY);
					openvrMenuPointerLastTriggerDown = false;
				}
				if (!openvrMenuPointerActive)
				{
					openvrMenuWheelNeutral = true;
				}
			}  // hmdPose0.bPoseIsValid

		I_StartupOpenVR();

		switch (vr_control_scheme)
		{
			case RIGHT_HANDED_DEFAULT:
				HandleInput_Default(vr_control_scheme,
				&rightTrackedRemoteState_new, &rightTrackedRemoteState_old, &controllers[1],
				&leftTrackedRemoteState_new, &leftTrackedRemoteState_old, &controllers[0],
				openvr::vr::k_EButton_A /*A*/, openvr::vr::k_EButton_ApplicationMenu /*B*/, openvr::vr::k_EButton_A /*X*/, openvr::vr::k_EButton_ApplicationMenu /*Y*/);
				break;
			case LEFT_HANDED_DEFAULT:
			case LEFT_HANDED_ALT:
				HandleInput_Default(vr_control_scheme, 
				&leftTrackedRemoteState_new, &leftTrackedRemoteState_old, &controllers[0],
				&rightTrackedRemoteState_new, &rightTrackedRemoteState_old, &controllers[1],
				openvr::vr::k_EButton_A /*X*/, openvr::vr::k_EButton_ApplicationMenu /*Y*/, openvr::vr::k_EButton_A /*A*/, openvr::vr::k_EButton_ApplicationMenu /*B*/);
				break;
		}
	}

	/* virtual */
	void OpenVRMode::TearDown() const
	{
		if (cachedScreenBlocks != 0 && gamestate == GS_LEVEL && menuactive == MENU_Off && !paused) {
			screenblocks = cachedScreenBlocks;
		}
		super::TearDown();
	}

	/* virtual */
	OpenVRMode::~OpenVRMode()
	{
		if (vrOverlay != nullptr)
		{
			if (overlayBeamHandle != 0)
			{
				vrOverlay->HideOverlay(overlayBeamHandle);
				vrOverlay->DestroyOverlay(overlayBeamHandle);
				overlayBeamHandle = 0;
			}
			if (overlayHandle != 0)
			{
				vrOverlay->HideOverlay(overlayHandle);
				vrOverlay->DestroyOverlay(overlayHandle);
				overlayHandle = 0;
			}
			if (overlayCursorHandle != 0)
			{
				vrOverlay->HideOverlay(overlayCursorHandle);
				vrOverlay->DestroyOverlay(overlayCursorHandle);
				overlayCursorHandle = 0;
			}
		}
		if (beamTexture != nullptr)
		{
			if (beamTexture->handle != nullptr)
			{
				GLuint beamTextureID = (GLuint)(std::ptrdiff_t)beamTexture->handle;
				glDeleteTextures(1, &beamTextureID);
			}
			delete beamTexture;
			beamTexture = nullptr;
		}
		if (cursorTexture != nullptr)
		{
			if (cursorTexture->handle != nullptr)
			{
				GLuint cursorTextureID = (GLuint)(std::ptrdiff_t)cursorTexture->handle;
				glDeleteTextures(1, &cursorTextureID);
			}
			delete cursorTexture;
			cursorTexture = nullptr;
		}
		if (vrSystem != nullptr) {
			vr::VR_Shutdown();
			vrSystem = nullptr;
			vrCompositor = nullptr;
			vrOverlay = nullptr;
			vrRenderModels = nullptr;
			vrSettings = nullptr;
			leftEyeView->dispose();
			rightEyeView->dispose();
		}
		if (haptics != nullptr) {
			delete haptics;
			haptics = nullptr;
		}
		if (crossHairDrawer != nullptr) {
			delete crossHairDrawer;
			crossHairDrawer = nullptr;
		}
	}

} /* namespace s3d */


ADD_STAT(remotestats)
{
	FString out;

	out.AppendFormat(
			"Pressed: lbtn=%" PRIu64 ", rbtn=%" PRIu64 "\n"
			"Touched: lbtn=%" PRIu64 ", rbtn=%" PRIu64 "\n"
			"Joystick: lx=1.3f, ly=1.3f, rx=1.3f, ry=1.3f\n", 
			"Trackpad: lx=1.3f, ly=1.3f, rx=1.3f, ry=1.3f\n", 
		s3d::leftTrackedRemoteState_new.ulButtonPressed,
		s3d::rightTrackedRemoteState_new.ulButtonPressed,
		s3d::leftTrackedRemoteState_new.ulButtonTouched,
		s3d::rightTrackedRemoteState_new.ulButtonTouched,
		s3d::leftTrackedRemoteState_new.rAxis[axisJoystick].x,
		s3d::leftTrackedRemoteState_new.rAxis[axisJoystick].y,
		s3d::rightTrackedRemoteState_new.rAxis[axisJoystick].x,
		s3d::rightTrackedRemoteState_new.rAxis[axisJoystick].y,
		s3d::leftTrackedRemoteState_new.rAxis[axisTrackpad].x,
		s3d::leftTrackedRemoteState_new.rAxis[axisTrackpad].y,
		s3d::rightTrackedRemoteState_new.rAxis[axisTrackpad].x,
		s3d::rightTrackedRemoteState_new.rAxis[axisTrackpad].y);

	if (s3d::controllers[1].active && s3d::controllers[1].pose.bPoseIsValid) {
		const HmdMatrix34_t& poseMatrix = s3d::controllers[1].pose.mDeviceToAbsoluteTracking;
		float x = poseMatrix.m[0][3];
		float y = poseMatrix.m[1][3];
		float z = poseMatrix.m[2][3];

		out.AppendFormat("x:%1.3f y:%1.3f z:%1.3f\n", x, y, z);

		HmdVector3d_t eulerAngles = s3d::eulerAnglesFromMatrixPitchRotate(poseMatrix, vr_weaponRotate * 2);
		out.AppendFormat("yaw:%2.f pitch:%2.f roll:%2.f\n", 
			RAD2DEG(eulerAngles.v[0]),
			-RAD2DEG(eulerAngles.v[1]), 
			normalizeAngle(-RAD2DEG(eulerAngles.v[2]) + 180.));
	}

	return out;
}

CCMD (cinemamode)
{
	cinemamode = !cinemamode;

	//Store these
	cinemamodeYaw = hmdorientation[YAW] + snapTurn;
	cinemamodePitch = hmdorientation[PITCH];

	//Reset angles back to normal view
	if (!cinemamode)
	{
		resetDoomYaw = true;
		resetPreviousPitch = true;
	}
}

#endif

