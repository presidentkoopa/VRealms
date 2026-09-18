#include "hw_vrmodes.h"
#include "hw_vrwheel.h"
#include "vk_openxrdevice.h"

#include "common/rendering/stereo3d/openxr/oxr_loader.h"
#include "hw_clock.h"
#include "hw_effectsgovernor.h"	// [GOVERNOR] E8: the frame wait and the swapchain wait are pacing
#include "v_video.h"
#include "hw_cvars.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/textures/vk_imagetransition.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "zvulkan/vulkanbuilders.h"
#include "zvulkan/vulkancompatibledevice.h"
#include "zvulkan/vulkanswapchain.h"
#include "QzDoom/VrCommon.h"
#include "p_vrdemo.h"
#include "d_player.h"
#include "g_game.h"
#include "g_levellocals.h"
#include "doomdef.h"
#include "c_console.h"
#include "d_eventbase.h"
#include "d_gui.h"
#include "menu.h"
#include "i_time.h"
#include "p_trace.h"
#include "p_linetracedata.h"
#include "p_local.h"
#include "LSMatrix.h"
#include "rendering/hwrenderer/scene/hw_drawinfo.h"
#include "common/rendering/hwrenderer/data/hw_viewpointbuffer.h"
#include "v_draw.h"
#include "i_net.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <chrono>
#include <vector>

EXTERN_CVAR(Int, developer);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern float hmdorientation[3];
extern float hmdPosition[3];
extern float weaponoffset[3];
extern float weaponangles[3];
extern float offhandoffset[3];
extern float offhandangles[3];

// REAL controller velocity, from OpenXR itself -- not differenced from two
// stale AttackPos samples 28ms apart in ZScript. XrSpaceVelocity is chained
// onto the same xrLocateSpace call that already gets pose (see
// updateHandPose below), so this costs one more struct in the query, not a
// second query. File-local: both the writer (updateHandPose) and the one
// reader (the AttackVel/OffhandVel assignment near AttackPos) live in this
// file, so there is no reason to put these through the same
// extern-global-defined-elsewhere pattern weaponoffset/weaponangles use.
// linear is metres/second in the same post-yaw-rotation frame offset[] ends
// up in; angular is radians/second about the same (base-space, post-yaw)
// axes -- OpenXR reports XrSpaceVelocity.angularVelocity in the base space,
// and updateHandPose rotates it exactly like linear. NOT hand-local.
static float weaponLinearVel[3]  = { 0, 0, 0 };
static float weaponAngularVel[3] = { 0, 0, 0 };
static float offhandLinearVel[3]  = { 0, 0, 0 };
static float offhandAngularVel[3] = { 0, 0, 0 };
static bool  weaponVelValid  = false;
static bool  offhandVelValid = false;
extern float doomYaw;
extern float previousPitch;
extern float playerYaw;
extern float snapTurn;
extern vec3_t positionDeltaThisFrame;
extern float remote_movementSideways;
extern float remote_movementForward;
extern float positional_movementSideways;
extern float positional_movementForward;
extern bool resetDoomYaw;
extern bool resetPreviousPitch;
extern bool ready_teleport;
extern bool trigger_teleport;
extern bool automapactive;
extern bool cinemamode;
bool VR_UseScreenLayer();
bool VR_UseCinematicScreenLayer();
void VR_SetHMDOrientation(float pitch, float yaw, float roll);
void VR_SetHMDPosition(float x, float y, float z);
double P_XYMovement(AActor* mo, DVector2 scroll);
void QzDoom_setUseScreenLayer(bool use);

EXTERN_CVAR(Float, vr_ipd);
EXTERN_CVAR(Float, vr_vunits_per_meter);
EXTERN_CVAR(Float, vr_height_adjust);
EXTERN_CVAR(Float, vr_openxr_fov_adjust_deg);
EXTERN_CVAR(Float, vr_openxr_eye_shift_scale);
EXTERN_CVAR(Float, vr_openxr_render_scale);
EXTERN_CVAR(Int, vr_openxr_debug_submit_mode);
EXTERN_CVAR(Int, vr_openxr_sync_mode);
EXTERN_CVAR(Bool, vr_openxr_multiview);
EXTERN_CVAR(Bool, vr_desktop_view_openxr_render);
EXTERN_CVAR(Bool, vr_openxr_multiview_mirror_reuse);
EXTERN_CVAR(Bool, vr_openxr_multiview_postprocess);
EXTERN_CVAR(Int, vid_refreshrate);
EXTERN_CVAR(Float, vr_snapTurn);
EXTERN_CVAR(Bool, vr_move_use_offhand);
EXTERN_CVAR(Bool, vr_switch_sticks);
EXTERN_CVAR(Bool, vr_secondary_button_mappings);
EXTERN_CVAR(Bool, vr_teleport);
// A0 harness. ON by default. A measurement nobody remembers to switch on is a
// measurement that never happens, and this one exists precisely because twelve
// sessions went without it. See the block in UpdateControllerState for what it
// measures and how to read what it prints.
// OFF. This is the euler round-trip harness -- three lines of residual stats
// every three seconds, forever, which drowned every other diagnostic in the
// log during three separate test sessions.
//
// RENAMED from vr_a0 for the same reason it is now false: the old name shipped
// archived-true, so changing the default alone would not have reached anyone
// who had already run it.
CVAR(Bool, vr_posestats, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// One line per hand per context CHANGE. See the block in ResolveGripContexts.
CVAR(Bool, vr_grip_debug, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

EXTERN_CVAR(Float, vr_weaponRotate);
EXTERN_CVAR(Float, vr_weaponScale);
EXTERN_CVAR(Bool, vr_enable_haptics);

// Traces the haptic path end to end. A pulse that never reaches the runtime
// and a pulse the controller ignores are indistinguishable from inside the
// headset, so each stage says whether it ran.
//
// Was `true, 0`: on by default AND flagless, so the ini was never read for it
// and it came back every launch no matter what a menu or console had set. An
// autoexec line was the only thing that could hold it down. Now off by default
// and archived, matching its neighbours above. Nothing inherits a stale `true`
// the way vr_posestats would have -- a flagless cvar was never written to any
// ini in the first place, so there is nothing on disk to override the new
// default.
CVAR(Bool, vr_haptic_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Float, vr_2dweaponScale);
EXTERN_CVAR(Float, vr_2dweaponOffsetX);
EXTERN_CVAR(Float, vr_2dweaponOffsetY);
EXTERN_CVAR(Float, vr_2dweaponOffsetZ);
EXTERN_CVAR(Int, screenblocks);
EXTERN_CVAR(Int, vid_defwidth);
EXTERN_CVAR(Int, vid_defheight);
EXTERN_CVAR(Float, vr_automap_stereo);
EXTERN_CVAR(Float, vr_hud_stereo);
EXTERN_CVAR(Float, vr_automap_rotate);
EXTERN_CVAR(Float, vr_hud_rotate);
EXTERN_CVAR(Float, vr_automap_distance);
EXTERN_CVAR(Float, vr_hud_distance);
EXTERN_CVAR(Float, vr_automap_scale);
EXTERN_CVAR(Float, vr_hud_scale);
EXTERN_CVAR(Bool, vr_hud_mount);
EXTERN_CVAR(Int, vr_hud_mount_pos);
EXTERN_CVAR(Float, vr_hud_mount_scale);
EXTERN_CVAR(Bool, vr_automap_fixed_roll);
EXTERN_CVAR(Bool, vr_hud_fixed_roll);
EXTERN_CVAR(Bool, vr_automap_fixed_pitch);
EXTERN_CVAR(Bool, vr_hud_fixed_pitch);
EXTERN_CVAR(Bool, vr_automap_mount);
EXTERN_CVAR(Int, vr_automap_mount_pos);
EXTERN_CVAR(Float, vr_automap_mount_scale);
EXTERN_CVAR(Int, vr_automap_border);
EXTERN_CVAR(Color, vr_automap_border_color);
EXTERN_CVAR(Int, vr_desktop_view);
EXTERN_CVAR(Int, vr_spectator);		// [SPECTATOR] p_vrdemo.cpp
EXTERN_CVAR(Float, vr_spectator_fov);
EXTERN_CVAR(Int, vr_mode);
EXTERN_CVAR(Bool, vr_swap_eyes);
EXTERN_CVAR(Bool, vr_automap_use_hud);
EXTERN_CVAR(Int, vr_overlayscreen);
EXTERN_CVAR(Bool, vr_overlayscreen_always);
EXTERN_CVAR(Float, vr_overlayscreen_size);
EXTERN_CVAR(Float, vr_overlayscreen_dist);
EXTERN_CVAR(Float, vr_overlayscreen_vpos);
EXTERN_CVAR(Int, vr_overlayscreen_bg);
EXTERN_CVAR(Int, vr_control_scheme);
EXTERN_CVAR(Bool, vr_two_handed_weapons);
EXTERN_CVAR(Bool, vr_stabilize_requires_grab);
EXTERN_CVAR(Float, vr_stabilize_distance_inches);
EXTERN_CVAR(Bool, vr_holster_use_grip);
CVAR(Bool, vr_menu_pointer, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_menu_pointer_color, 0xffffff, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, vr_mouse_in_menu, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
// Keep the live stereo world and head tracking in the headset while a menu is
// open over a level, with the menu on the virtual-screen quad (no flat scene
// copy, no backdrop wall, no menu blur). For tuning renderer-read sliders --
// holster/model placement -- while looking at your body. Playsim stays paused;
// head yaw is held off the actor until the menu closes (xrMenuHeldYawDegrees).
// Off = the old behaviour: menu collapses the view onto a flat head-follow
// screen and head yaw stops turning the world. See FrameRenderMode::MenuOverWorld.
CVAR(Bool, vr_menu_keep_world, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

namespace s3d {

namespace
{
class ScopedCycleTimer
{
public:
	explicit ScopedCycleTimer(glcycle_t& timer) : mTimer(timer)
	{
		mTimer.Clock();
	}

	~ScopedCycleTimer()
	{
		mTimer.Unclock();
	}

private:
	glcycle_t& mTimer;
};

constexpr XrViewConfigurationType viewType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr XrEnvironmentBlendMode environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
XrSessionState xrSessionState = XR_SESSION_STATE_UNKNOWN;

// For diagnostics that would otherwise have to guess. A pose refusal that names
// the session state is the difference between "your controller is broken" and
// "you had the dashboard open", and those two send you to very different places.
// THE BODY FRAME, WATCHED.
//
// Everything about a body-worn thing can be printed from script EXCEPT the one
// number that decides where it lands: the heading the renderer actually rotates
// by. doomYaw is not readable from ZScript and is NOT AActor::HmdYaw -- it
// accumulates the per-frame head-turn delta on top of it -- so a mod that
// filters its own heading has no way to check its own work.
//
// That gap cost a full session. The filter was correct, the transform cancelled
// it, the holsters tracked the head anyway, and NOTHING anywhere was wrong
// enough to report. This is the line that would have said so in one glance.
//
// ONCE A SECOND, not per call: GetHmdTransform runs once per body-followed
// actor per frame, so a dozen worn parts at ninety frames is a thousand lines a
// second. A watcher that floods does not merely add noise -- it buries the thing
// it was built to show.
CVAR(Bool, vr_body_debug, false, 0)

static const char *XrSessionStateName(XrSessionState s)
{
	switch (s)
	{
		case XR_SESSION_STATE_IDLE:         return "IDLE";
		case XR_SESSION_STATE_READY:        return "READY";
		case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
		case XR_SESSION_STATE_VISIBLE:      return "VISIBLE";
		case XR_SESSION_STATE_FOCUSED:      return "FOCUSED";
		case XR_SESSION_STATE_STOPPING:     return "STOPPING";
		case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
		case XR_SESSION_STATE_EXITING:      return "EXITING";
		default:                            return "UNKNOWN";
	}
}

using PFN_xrGetVulkanGraphicsRequirementsKHR_t = XrResult (XRAPI_PTR *)(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR*);
using PFN_xrGetVulkanGraphicsDeviceKHR_t = XrResult (XRAPI_PTR *)(XrInstance, XrSystemId, VkInstance, VkPhysicalDevice*);
using PFN_xrGetVulkanGraphicsRequirements2KHR_t = XrResult (XRAPI_PTR *)(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR*);
using PFN_xrGetVulkanGraphicsDevice2KHR_t = XrResult (XRAPI_PTR *)(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR*, VkPhysicalDevice*);

PFN_xrGetVulkanGraphicsRequirementsKHR_t xrGetVulkanGraphicsRequirementsKHR_inst = nullptr;
PFN_xrGetVulkanGraphicsDeviceKHR_t xrGetVulkanGraphicsDeviceKHR_inst = nullptr;
PFN_xrGetVulkanGraphicsRequirements2KHR_t xrGetVulkanGraphicsRequirements2KHR_inst = nullptr;
PFN_xrGetVulkanGraphicsDevice2KHR_t xrGetVulkanGraphicsDevice2KHR_inst = nullptr;
#ifdef XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME
PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB_inst = nullptr;
PFN_xrGetDisplayRefreshRateFB xrGetDisplayRefreshRateFB_inst = nullptr;
PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB_inst = nullptr;
#endif

static const std::vector<XrExtensionProperties>& GetOpenXRExtensions()
{
	static std::vector<XrExtensionProperties> cachedExtensions;
	static bool cacheInitialized = false;
	if (cacheInitialized)
		return cachedExtensions;

	cacheInitialized = true;
	uint32_t extensionCount = 0;
	if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr)) || extensionCount == 0)
		return cachedExtensions;

	cachedExtensions.assign(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
	if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, cachedExtensions.data())))
	{
		cachedExtensions.clear();
		return cachedExtensions;
	}

	if (cachedExtensions.size() != extensionCount)
		cachedExtensions.resize(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });

	return cachedExtensions;
}

static bool IsGameplaySceneActive()
{
	return gamestate == GS_LEVEL && menuactive == MENU_Off && !paused && ConsoleState == c_up;
}

static bool IsLevelSceneState()
{
	return gamestate == GS_LEVEL || gamestate == GS_TITLELEVEL;
}

static bool HasOpenXRExtension(const char* extensionName)
{
	const auto& extensions = GetOpenXRExtensions();

	for (const auto& extension : extensions)
	{
		if (strcmp(extension.extensionName, extensionName) == 0)
			return true;
	}

	return false;
}

static bool HasDeviceExtension(const VulkanPhysicalDevice& device, const char* extensionName)
{
	return std::any_of(device.Extensions.begin(), device.Extensions.end(),
		[extensionName](const VkExtensionProperties& extension)
		{
			return strcmp(extension.extensionName, extensionName) == 0;
		});
}

static bool ShouldRetryCreateSession(XrResult xrResult)
{
	// Retry only for transient runtime failures
	return xrResult == XR_ERROR_RUNTIME_FAILURE;
}

static XrColorSpaceFB SelectPreferredColorSpace(const std::vector<XrColorSpaceFB>& supportedColorSpaces)
{
	const XrColorSpaceFB preferredOrder[] = {
		// Match the OpenVR/OpenGL handoff as closely as possible: submit the
		// engine's already-presented LDR output without asking the runtime to
		// reinterpret it into a managed display color space first.
		XR_COLOR_SPACE_UNMANAGED_FB,
		XR_COLOR_SPACE_REC709_FB,
		XR_COLOR_SPACE_RIFT_S_FB,
		XR_COLOR_SPACE_QUEST_FB,
		XR_COLOR_SPACE_P3_FB,
		XR_COLOR_SPACE_REC2020_FB,
	};

	for (XrColorSpaceFB preferred : preferredOrder)
	{
		if (std::find(supportedColorSpaces.begin(), supportedColorSpaces.end(), preferred) != supportedColorSpaces.end())
			return preferred;
	}

	return supportedColorSpaces.empty() ? XR_COLOR_SPACE_UNMANAGED_FB : supportedColorSpaces.front();
}

static float DEG2RAD(float deg)
{
	return deg * (float)(M_PI / 180.0);
}

static uint32_t GetMaxRecommendedViewWidth(const std::vector<XrViewConfigurationView>& views)
{
	uint32_t width = 0;
	for (const auto& view : views)
	{
		width = std::max(width, view.recommendedImageRectWidth);
	}
	return width;
}

static uint32_t GetMaxRecommendedViewHeight(const std::vector<XrViewConfigurationView>& views)
{
	uint32_t height = 0;
	for (const auto& view : views)
	{
		height = std::max(height, view.recommendedImageRectHeight);
	}
	return height;
}

static bool HasMismatchedRecommendedViewExtents(const std::vector<XrViewConfigurationView>& views)
{
	if (views.size() < 2)
		return false;

	const uint32_t width = views[0].recommendedImageRectWidth;
	const uint32_t height = views[0].recommendedImageRectHeight;
	for (size_t i = 1; i < views.size(); ++i)
	{
		if (views[i].recommendedImageRectWidth != width || views[i].recommendedImageRectHeight != height)
			return true;
	}
	return false;
}

static XrVector3f RotateVector(const XrQuaternionf& q, const XrVector3f& v)
{
	XrVector3f result;

	const float qx = q.x;
	const float qy = q.y;
	const float qz = q.z;
	const float qw = q.w;

	const float tx = 2.0f * (qy * v.z - qz * v.y);
	const float ty = 2.0f * (qz * v.x - qx * v.z);
	const float tz = 2.0f * (qx * v.y - qy * v.x);

	result.x = v.x + qw * tx + (qy * tz - qz * ty);
	result.y = v.y + qw * ty + (qz * tx - qx * tz);
	result.z = v.z + qw * tz + (qx * ty - qy * tx);
	return result;
}

static XrVector3f NormalizeVector(const XrVector3f& v)
{
	const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	if (length <= 0.0f)
	{
		return { 0.0f, 0.0f, 0.0f };
	}
	return { v.x / length, v.y / length, v.z / length };
}

static XrVector3f AddVector(const XrVector3f& a, const XrVector3f& b)
{
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

static XrVector3f SubtractVector(const XrVector3f& a, const XrVector3f& b)
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static XrVector3f ScaleVector(const XrVector3f& v, float scale)
{
	return { v.x * scale, v.y * scale, v.z * scale };
}

static float DotVector(const XrVector3f& a, const XrVector3f& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static XrVector3f CrossVector(const XrVector3f& a, const XrVector3f& b)
{
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

static XrQuaternionf MultiplyQuaternion(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
	};
}

static XrQuaternionf ConjugateQuaternion(const XrQuaternionf& q)
{
	return { -q.x, -q.y, -q.z, q.w };
}

static float DotQuaternion(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

static XrQuaternionf NormalizeQuaternion(const XrQuaternionf& q)
{
	const float length = std::sqrt(DotQuaternion(q, q));
	if (length <= 0.0f)
		return { 0.0f, 0.0f, 0.0f, 1.0f };

	const float invLength = 1.0f / length;
	return { q.x * invLength, q.y * invLength, q.z * invLength, q.w * invLength };
}

static XrQuaternionf GetCenteredViewOrientation(const std::vector<XrView>& views)
{
	if (views.empty())
		return { 0.0f, 0.0f, 0.0f, 1.0f };
	if (views.size() == 1)
		return NormalizeQuaternion(views[0].pose.orientation);

	XrQuaternionf accum = NormalizeQuaternion(views[0].pose.orientation);
	for (size_t i = 1; i < views.size(); ++i)
	{
		XrQuaternionf q = NormalizeQuaternion(views[i].pose.orientation);
		if (DotQuaternion(accum, q) < 0.0f)
		{
			q.x = -q.x;
			q.y = -q.y;
			q.z = -q.z;
			q.w = -q.w;
		}

		accum.x += q.x;
		accum.y += q.y;
		accum.z += q.z;
		accum.w += q.w;
	}

	return NormalizeQuaternion(accum);
}

static XrQuaternionf QuaternionFromBasis(const XrVector3f& xAxis, const XrVector3f& yAxis, const XrVector3f& zAxis)
{
	const float m00 = xAxis.x, m01 = yAxis.x, m02 = zAxis.x;
	const float m10 = xAxis.y, m11 = yAxis.y, m12 = zAxis.y;
	const float m20 = xAxis.z, m21 = yAxis.z, m22 = zAxis.z;

	const float trace = m00 + m11 + m22;
	XrQuaternionf q{};
	if (trace > 0.0f)
	{
		const float s = std::sqrt(trace + 1.0f) * 2.0f;
		q.w = 0.25f * s;
		q.x = (m21 - m12) / s;
		q.y = (m02 - m20) / s;
		q.z = (m10 - m01) / s;
	}
	else if (m00 > m11 && m00 > m22)
	{
		const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
		q.w = (m21 - m12) / s;
		q.x = 0.25f * s;
		q.y = (m01 + m10) / s;
		q.z = (m02 + m20) / s;
	}
	else if (m11 > m22)
	{
		const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
		q.w = (m02 - m20) / s;
		q.x = (m01 + m10) / s;
		q.y = 0.25f * s;
		q.z = (m12 + m21) / s;
	}
	else
	{
		const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
		q.w = (m10 - m01) / s;
		q.x = (m02 + m20) / s;
		q.y = (m12 + m21) / s;
		q.z = 0.25f * s;
	}
	return q;
}

static XrQuaternionf MakeAxisAngleQuaternion(const XrVector3f& axis, float angleRadians)
{
	const float halfAngle = angleRadians * 0.5f;
	const float s = std::sin(halfAngle);
	const float c = std::cos(halfAngle);
	return { axis.x * s, axis.y * s, axis.z * s, c };
}

static XrVector3f OpenVREulerAnglesFromQuaternion(const XrQuaternionf& quat)
{
	double q0 = quat.w;
	// Permute axes to match OpenVR's yaw/pitch/roll convention.
	double q2 = quat.x;
	double q3 = quat.y;
	double q1 = quat.z;

	double roll = std::atan2(2.0 * (q0 * q1 + q2 * q3), 1.0 - 2.0 * (q1 * q1 + q2 * q2));
	double pitch = std::asin(2.0 * (q0 * q2 - q3 * q1));
	double yaw = std::atan2(2.0 * (q0 * q3 + q1 * q2), 1.0 - 2.0 * (q2 * q2 + q3 * q3));

	return { (float)yaw, (float)pitch, (float)roll };
}

static int mAngleFromRadians(double radians)
{
	return (int)std::round(radians * 65536.0 / (2.0 * M_PI));
}

static float GetRawHmdHeightInMapUnit()
{
	const double pixelstretch = level.info ? level.info->pixelstretch : 1.2;
	return (float)(((double)hmdPosition[1] + (double)vr_height_adjust) * (double)vr_vunits_per_meter / pixelstretch);
}

static float GetHmdAdjustedHeightInMapUnit(bool applyLocalAnchor, float localHeightAnchor)
{
	const float rawHeight = GetRawHmdHeightInMapUnit();
	return applyLocalAnchor ? (rawHeight + localHeightAnchor) : rawHeight;
}

static float GetHmdAdjustedHeightInMapUnit()
{
	return GetHmdAdjustedHeightInMapUnit(false, 0.0f);
}

static float GetDoomPlayerHeightWithoutCrouch(const player_t* player)
{
	static float cachedHeight = 0.0f;
	if (cachedHeight == 0.0f && player != nullptr)
	{
		cachedHeight = player->DefaultViewHeight();
	}
	return cachedHeight != 0.0f ? cachedHeight : GetHmdAdjustedHeightInMapUnit();
}

static float GetViewpointYaw()
{
	if (cinemamode)
		return cinemamodeYaw;
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

struct XrSafeSourceRect
{
	IntRect rect = { 0, 0, 0, 0 };
	float scaleX = 1.0f;
	float scaleY = -1.0f;
	float offsetX = 0.0f;
	float offsetY = 1.0f;
	bool usedFallback = false;
	bool wasClamped = false;
};

static void GetStableOpenXRVirtualScreenSize(uint32_t& width, uint32_t& height)
{
	// Keep the virtual screen aligned with the live UI render size. The menu code still lays itself out
	// from the active screen dimensions, so forcing the OpenXR quad into a synthetic 4:3 target can make
	// the visible menu narrower than the pointer/raycast area.
	constexpr uint32_t kFallbackW = 960;
	constexpr uint32_t kFallbackH = 720;
	constexpr uint32_t kMinW = 640;
	constexpr uint32_t kMinH = 360;
	constexpr uint32_t kMaxW = 2048;
	constexpr uint32_t kMaxH = 2048;
	constexpr uint64_t kMaxPixels = 2048ull * 1536ull;

	uint32_t sourceW = (screen != nullptr) ? (uint32_t)std::max(0, screen->GetWidth()) : 0u;
	uint32_t sourceH = (screen != nullptr) ? (uint32_t)std::max(0, screen->GetHeight()) : 0u;
	if (sourceW == 0 || sourceH == 0)
	{
		sourceW = (uint32_t)std::max(0, DisplayWidth);
		sourceH = (uint32_t)std::max(0, DisplayHeight);
	}
	if (sourceW == 0 || sourceH == 0)
	{
		sourceW = (uint32_t)std::max(0, (int)vid_defwidth);
		sourceH = (uint32_t)std::max(0, (int)vid_defheight);
	}
	if (sourceW == 0 || sourceH == 0)
	{
		sourceW = kFallbackW;
		sourceH = kFallbackH;
	}

	sourceW = std::clamp(sourceW, kMinW, kMaxW);
	sourceH = std::clamp(sourceH, kMinH, kMaxH);

	uint32_t targetW = sourceW;
	uint32_t targetH = sourceH;

	uint64_t pixels = (uint64_t)targetW * (uint64_t)targetH;
	if (pixels > kMaxPixels)
	{
		const double scale = std::sqrt((double)kMaxPixels / (double)pixels);
		targetW = std::max<uint32_t>(kMinW, (uint32_t)std::floor((double)targetW * scale));
		targetH = std::max<uint32_t>(kMinH, (uint32_t)std::floor((double)targetH * scale));
	}
	targetW &= ~1u;
	targetH &= ~1u;
	if (targetW == 0 || targetH == 0)
	{
		targetW = kFallbackW;
		targetH = kFallbackH;
	}

	width = targetW;
	height = targetH;
}

static void GetOpenXRVirtualScreenMeters(uint32_t renderW, uint32_t renderH, float& widthMeters, float& heightMeters)
{
	const float baseWidthMeters = std::max(0.1f, (1.0f + vr_overlayscreen_size) * 1.2f);
	if (renderW == 0 || renderH == 0)
	{
		GetStableOpenXRVirtualScreenSize(renderW, renderH);
	}

	widthMeters = baseWidthMeters;
	// Match the texture aspect exactly. Any extra height fudge makes pointer hit area drift away from rendered menu edges
	heightMeters = std::max(0.1f, baseWidthMeters * ((float)renderH / (float)std::max(renderW, 1u)));
}

static float YawDegFromForward(const XrVector3f& forwardIn)
{
	XrVector3f forward = forwardIn;
	forward.y = 0.0f;
	forward = NormalizeVector(forward);
	if (DotVector(forward, forward) <= 0.000001f)
		return 0.0f;
	return std::atan2(forward.x, forward.z) * (180.0f / (float)M_PI);
}

static float ShortestAngleDeltaDeg(float a, float b)
{
	float d = std::fmod(a - b, 360.0f);
	if (d > 180.0f) d -= 360.0f;
	if (d < -180.0f) d += 360.0f;
	return d;
}

static bool ShouldPrepareDesktopMirrorEye(int eyeIndex)
{
	if (vr_desktop_view == -1)
		return false;

	// Side-by-side mirror consumes both prepared eye textures. Single-eye mirror modes only
	// ever sample one of them, so avoid the extra fullscreen pass for the unused eye.
	if (vr_desktop_view != 1 && vr_desktop_view != 2)
		return true;

	const int mirroredEyeIndex = (vr_desktop_view == 1)
		? (vr_swap_eyes ? 1 : 0)
		: (vr_swap_eyes ? 0 : 1);
	return eyeIndex == mirroredEyeIndex;
}

static bool ShouldReuseSubmittedPresentForDesktopMirror(const VKOpenXRDeviceMode* mode)
{
	if (vr_desktop_view == -1)
		return false;

	if (vr_desktop_view_openxr_render)
		return true;

	// Favor the already-prepared XR present images in multiview mode by default.
	// That keeps the old unbiased mirror path available as an opt-out, but avoids
	// paying for an extra fullscreen pass when we're optimizing the XR path.
	return mode != nullptr &&
		vr_openxr_multiview_mirror_reuse &&
		mode->ShouldUseMultiviewThisFrame();
}

static bool ShouldUseDedicatedDesktopMirrorTextures(const VKOpenXRDeviceMode* mode)
{
	return vr_desktop_view != -1 && !ShouldReuseSubmittedPresentForDesktopMirror(mode);
}

} // namespace

bool VKOpenXRDeviceMode::GetBenchmarkInfo(VRBenchmarkInfo& out) const
{
	out = {};
	out.IsVR = true;
	out.IsOpenXR = true;
	out.MultiviewEnabled = !!vr_openxr_multiview;
	out.MultiviewSupported = xrMultiviewSupported;
	out.MultiviewActive = ShouldUseMultiviewThisFrame();
	out.SceneLayered = out.MultiviewActive && xrViewCount > 1;
	out.PostprocessLayered = out.MultiviewActive && xrViewCount > 1 && !!vr_openxr_multiview_postprocess;
	out.FinalizeLayered = false;
	out.DirectXrRender = false;
	out.DedicatedMirrorTextures = ShouldUseDedicatedDesktopMirrorTextures(this);
	out.ViewCount = xrViewCount;
	out.ViewMask = GetMultiviewViewMask();
	out.RecommendedWidth = GetMaxRecommendedViewWidth(xrViewConfigs);
	out.RecommendedHeight = GetMaxRecommendedViewHeight(xrViewConfigs);
	out.PresentWidth = xrPresentWidth;
	out.PresentHeight = xrPresentHeight;
	out.DesktopViewMode = vr_desktop_view;
	out.RequestedRefreshRate = (int)vid_refreshrate;
	out.SyncMode = vr_openxr_sync_mode;
	out.RenderScale = (float)vr_openxr_render_scale;
#ifdef XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME
	out.RuntimeRefreshRate = xrCurrentDisplayRefreshRate > 0.0f ? xrCurrentDisplayRefreshRate : xrRequestedDisplayRefreshRate;
#endif

	auto* vkfb = dynamic_cast<VulkanRenderDevice*>(screen);
	if (vkfb != nullptr && vkfb->GetBuffers() != nullptr)
	{
		out.SceneSamples = (int)vkfb->GetBuffers()->GetSceneSamples();
	}
	return true;
}

namespace
{

static XrSafeSourceRect GetSafeXrSourceRect(VulkanRenderDevice* vkfb)
{
	XrSafeSourceRect result;
	auto* buffers = vkfb ? vkfb->GetBuffers() : nullptr;
	const int srcBufferW = buffers ? buffers->GetWidth() : 0;
	const int srcBufferH = buffers ? buffers->GetHeight() : 0;
	IntRect requestedRect = vkfb ? vkfb->mSceneViewport : IntRect{ 0, 0, 0, 0 };
	const bool overlayUIActive = menuactive != MENU_Off || ConsoleState != c_up || cinemamode;
	const auto& mode = (const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance();

	if (mode.ShouldUseRecommendedRenderSizeThisFrame() && !overlayUIActive && srcBufferW > 0 && srcBufferH > 0)
	{
		requestedRect.left = 0;
		requestedRect.top = 0;
		requestedRect.width = srcBufferW;
		requestedRect.height = srcBufferH;
	}

	auto useFullBufferFallback = [&]()
	{
		result.rect.left = 0;
		result.rect.top = 0;
		result.rect.width = std::max(1, srcBufferW);
		result.rect.height = std::max(1, srcBufferH);
		result.usedFallback = true;
	};

	if (srcBufferW <= 0 || srcBufferH <= 0 || requestedRect.width <= 0 || requestedRect.height <= 0 || overlayUIActive)
	{
		useFullBufferFallback();
	}
	else
	{
		const int requestedLeft = requestedRect.left;
		const int requestedTop = requestedRect.top;
		const int requestedRight = requestedRect.left + requestedRect.width;
		const int requestedBottom = requestedRect.top + requestedRect.height;

		const int clampedLeft = std::clamp(requestedLeft, 0, srcBufferW);
		const int clampedTop = std::clamp(requestedTop, 0, srcBufferH);
		const int clampedRight = std::clamp(requestedRight, 0, srcBufferW);
		const int clampedBottom = std::clamp(requestedBottom, 0, srcBufferH);

		if (clampedRight <= clampedLeft || clampedBottom <= clampedTop)
		{
			useFullBufferFallback();
		}
		else
		{
			result.rect.left = clampedLeft;
			result.rect.top = clampedTop;
			result.rect.width = clampedRight - clampedLeft;
			result.rect.height = clampedBottom - clampedTop;
			result.wasClamped = clampedLeft != requestedLeft || clampedTop != requestedTop ||
				clampedRight != requestedRight || clampedBottom != requestedBottom;
		}
	}

	if (srcBufferW > 0 && srcBufferH > 0)
	{
		result.scaleX = result.rect.width / (float)srcBufferW;
		result.scaleY = -result.rect.height / (float)srcBufferH;
		result.offsetX = result.rect.left / (float)srcBufferW;
		result.offsetY = (result.rect.top + result.rect.height) / (float)srcBufferH;
	}

	return result;
}

static const char* FrameRenderModeName(VKOpenXRDeviceMode::FrameRenderMode mode)
{
	switch (mode)
	{
	case VKOpenXRDeviceMode::FrameRenderMode::GameplayEyes:
		return "GameplayEyes";
	case VKOpenXRDeviceMode::FrameRenderMode::VirtualScreen:
		return "VirtualScreen";
	case VKOpenXRDeviceMode::FrameRenderMode::MenuOverWorld:
		return "MenuOverWorld";
	default:
		return "Unknown";
	}
}

static void AngleVectors(const float angles[3], float* forward, float* right, float* up)
{
	const float pitch = (float)(angles[0] * (M_PI / 180.0f));
	const float yaw = (float)(angles[1] * (M_PI / 180.0f));
	const float roll = (float)(angles[2] * (M_PI / 180.0f));

	const float sp = std::sin(pitch);
	const float cp = std::cos(pitch);
	const float sy = std::sin(yaw);
	const float cy = std::cos(yaw);
	const float sr = std::sin(roll);
	const float cr = std::cos(roll);

	if (forward != nullptr)
	{
		forward[0] = cp * cy;
		forward[1] = cp * sy;
		forward[2] = -sp;
	}

	if (right != nullptr)
	{
		right[0] = (-sr * sp * cy) + (cr * sy);
		right[1] = (-sr * sp * sy) - (cr * cy);
		right[2] = -sr * cp;
	}

	if (up != nullptr)
	{
		up[0] = (cr * sp * cy) + (sr * sy);
		up[1] = (cr * sp * sy) - (sr * cy);
		up[2] = cr * cp;
	}
}

// The frustum an eye is actually rendered with: the runtime's, widened or narrowed by
// vr_openxr_fov_adjust_deg. Shared with the [SPECTATOR] reprojection, which has to know
// exactly what the eye image covers.
static XrFovf AdjustOpenXREyeFov(const XrFovf& fov)
{
	const float fovAdjust = DEG2RAD(clamp<float>(vr_openxr_fov_adjust_deg, -30.0f, 30.0f));
	const XrFovf adjustedFov = {
		std::max(fov.angleLeft - fovAdjust, (float)(-0.5 * M_PI + 0.001)),
		std::min(fov.angleRight + fovAdjust, (float)(0.5 * M_PI - 0.001)),
		std::min(fov.angleUp + fovAdjust, (float)(0.5 * M_PI - 0.001)),
		std::max(fov.angleDown - fovAdjust, (float)(-0.5 * M_PI + 0.001))
	};
	return adjustedFov;
}

static VSMatrix BuildOpenXREyeProjection(const XrFovf& fov, float nearZ, float farZ, int eye)
{
	(void)eye;

	const XrFovf adjustedFov = AdjustOpenXREyeFov(fov);

	const float tanLeft = std::tan(adjustedFov.angleLeft);
	const float tanRight = std::tan(adjustedFov.angleRight);
	const float tanUp = std::tan(adjustedFov.angleUp);
	const float tanDown = std::tan(adjustedFov.angleDown);
	const float tanWidth = tanRight - tanLeft;
	const float tanHeight = tanUp - tanDown;
	const float offsetZ = nearZ;

	FLOATTYPE m[16];
	memset(m, 0, sizeof(m));

	// This renderer stage consumes an OpenGL-style [-1,1] Z projection even
	// when the final submitted image goes through a Vulkan/OpenXR bridge.
	m[0] = 2.0f / tanWidth;
	m[5] = 2.0f / tanHeight;
	m[8] = (tanRight + tanLeft) / tanWidth;
	m[9] = (tanUp + tanDown) / tanHeight;
	m[10] = -(farZ + offsetZ) / (farZ - nearZ);
	m[11] = -1.0f;
	m[14] = -(farZ * (nearZ + offsetZ)) / (farZ - nearZ);

	VSMatrix matrix;
	matrix.loadMatrix(m);
	return matrix;
}

void QuaternionToEuler(const XrQuaternionf& q, float& pitch, float& yaw, float& roll)
{
	double q0 = q.w;
	// permute axes to match OpenVR's yaw/pitch/roll convention
	double q2 = q.x;
	double q3 = q.y;
	double q1 = q.z;

	double outRoll = std::atan2(2.0 * (q0 * q1 + q2 * q3), 1.0 - 2.0 * (q1 * q1 + q2 * q2));
	double outPitch = std::asin(2.0 * (q0 * q2 - q3 * q1));
	double outYaw = -std::atan2(2.0 * (q0 * q3 + q1 * q2), 1.0 - 2.0 * (q2 * q2 + q3 * q3));

	pitch = (float)(outPitch * (180.0 / M_PI));
	yaw = (float)(outYaw * (180.0 / M_PI));
	roll = (float)(outRoll * (180.0 / M_PI));
}

static XrQuaternionf QuaternionFromAxisAngle(float x, float y, float z, float radians)
{
	const float halfAngle = radians * 0.5f;
	const float sine = sinf(halfAngle);
	return XrQuaternionf{ x * sine, y * sine, z * sine, cosf(halfAngle) };
}

static double NormalizeAngle(double angle)
{
	angle = fmod(angle, 360.0);
	angle = fmod(angle + 360.0, 360.0);
	if (angle > 180.0)
	{
		angle -= 360.0;
	}
	return angle;
}

static const float overlayBG[6][3] = {
	{ 0.0f, 0.0f, 0.0f },
	{ 0.11f, 0.0f, 0.01f },
	{ 0.0f, 0.11f, 0.02f },
	{ 0.0f, 0.02f, 0.11f },
	{ 0.0f, 0.11f, 0.10f },
	{ 0.10f, 0.10f, 0.10f }
};

static XrVector3f GetVirtualScreenBackgroundColor()
{
	const int idx = clamp<int>(vr_overlayscreen_bg, 0, 5);
	return { overlayBG[idx][0], overlayBG[idx][1], overlayBG[idx][2] };
}

static XrVector3f GetVirtualScreenBackdropColor()
{
	const XrVector3f base = GetVirtualScreenBackgroundColor();
	constexpr float kBackdropDimScale = 0.30f;
	return { base.x * kBackdropDimScale, base.y * kBackdropDimScale, base.z * kBackdropDimScale };
}

static bool IsRightHandedVrControls()
{
	return vr_control_scheme < 10;
}

static int GetMainHandIndex()
{
	return IsRightHandedVrControls() ? 1 : 0;
}

static int GetOffHandIndex()
{
	return IsRightHandedVrControls() ? 0 : 1;
}

static int HandKeyOffset(int hand)
{
	return hand * (KEY_PAD_RSHOULDER - KEY_PAD_LSHOULDER);
}

static int HandAxisKeyOffset(int hand)
{
	return hand * (KEY_PAD_RTHUMB_LEFT - KEY_PAD_LTHUMB_LEFT);
}

struct OpenXRHandInputState
{
	bool select = false;
	bool grip = false;
	bool thumbClick = false;
	bool menu = false;
	bool a = false;
	bool b = false;
	bool x = false;
	bool y = false;
	bool thumbTouch = false;
	bool triggerTouch = false;
	XrVector2f thumbstick = { 0.0f, 0.0f };
	XrVector2f trackpad = { 0.0f, 0.0f };

	// 0..1, how far each is actually pulled -- see the action declarations in
	// the header for why these exist next to the booleans rather than
	// instead of them. Zero on a runtime or controller that only reports a
	// click: a caller reading these still gets a usable answer (0 or 1 from
	// the click binding), just without the travel in between.
	float triggerValue = 0.0f;
	float gripValue = 0.0f;
};

static void PostControllerKeyTransition(bool oldState, bool newState, int key)
{
	if (oldState == newState)
		return;

	event_t ev = {};
	ev.data1 = key;
	ev.type = newState ? EV_KeyDown : EV_KeyUp;
	D_PostEvent(&ev);
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

static void PostGuiWheelEvent(EGUIEvent type, int x, int y, int modifiers = 0)
{
	event_t ev = {};
	ev.type = EV_GUI_Event;
	ev.subtype = type;
	ev.data1 = x;
	ev.data2 = y;
	ev.data3 = (int16_t)modifiers;
	D_PostEvent(&ev);
}

static void PostControllerAxisTransitions(const XrVector2f& oldValue, const XrVector2f& newValue, int leftKey, int rightKey, int downKey, int upKey)
{
	// Keep X/Y virtual-button zones mutually exclusive so diagonal thumbstick
	// movement cannot trigger both turn (left/right) and up/down binds together.
	// For right-stick vertical binds, make up/down significantly harder to
	// trigger than left/right to protect turning from accidental weapon changes.
	const bool strictRightVertical =
		(downKey == KEY_JOYAXIS4MINUS && upKey == KEY_JOYAXIS4PLUS) ||
		(downKey == KEY_JOYAXIS8MINUS && upKey == KEY_JOYAXIS8PLUS);
	const float xDeadZone = 0.25f;
	const float yDeadZone = strictRightVertical ? 0.72f : 0.25f;
	const float horizontalDominanceMargin = strictRightVertical ? 0.00f : 0.15f;
	const float verticalDominanceMargin = strictRightVertical ? 0.30f : 0.15f;
	auto resolveCardinalStates = [&](const XrVector2f& value, bool& left, bool& right, bool& down, bool& up)
	{
		const float absX = fabsf(value.x);
		const float absY = fabsf(value.y);
		const bool xActive = absX > xDeadZone;
		const bool yActive = absY > yDeadZone;
		const bool xDominant = xActive && (!yActive || (absX - absY) >= horizontalDominanceMargin);
		const bool yDominant = yActive && (!xActive || (absY - absX) >= verticalDominanceMargin);

		left = xDominant && value.x < 0.0f;
		right = xDominant && value.x > 0.0f;
		down = yDominant && value.y < 0.0f;
		up = yDominant && value.y > 0.0f;
	};

	bool oldLeft = false, oldRight = false, oldDown = false, oldUp = false;
	bool newLeft = false, newRight = false, newDown = false, newUp = false;
	resolveCardinalStates(oldValue, oldLeft, oldRight, oldDown, oldUp);
	resolveCardinalStates(newValue, newLeft, newRight, newDown, newUp);

	PostControllerKeyTransition(oldLeft, newLeft, leftKey);
	PostControllerKeyTransition(oldRight, newRight, rightKey);
	PostControllerKeyTransition(oldDown, newDown, downKey);
	PostControllerKeyTransition(oldUp, newUp, upKey);
}

static void PostRemappedControllerAxisTransitions(
	const XrVector2f& oldValue,
	const XrVector2f& newValue,
	bool oldModifier,
	bool newModifier,
	int baseLeftKey,
	int baseRightKey,
	int baseDownKey,
	int baseUpKey,
	int modifiedLeftKey,
	int modifiedRightKey,
	int modifiedDownKey,
	int modifiedUpKey)
{
	// Keep X/Y virtual-button zones mutually exclusive so diagonal thumbstick
	// movement cannot trigger both turn (left/right) and up/down binds together.
	// For right-stick vertical binds, make up/down significantly harder to
	// trigger than left/right to protect turning from accidental weapon changes.
	const bool strictRightVertical =
		(baseDownKey == KEY_JOYAXIS4MINUS && baseUpKey == KEY_JOYAXIS4PLUS) ||
		(modifiedDownKey == KEY_JOYAXIS8MINUS && modifiedUpKey == KEY_JOYAXIS8PLUS);
	const float xDeadZone = 0.25f;
	const float yDeadZone = strictRightVertical ? 0.72f : 0.25f;
	const float horizontalDominanceMargin = strictRightVertical ? 0.00f : 0.15f;
	const float verticalDominanceMargin = strictRightVertical ? 0.30f : 0.15f;
	auto resolveCardinalStates = [&](const XrVector2f& value, bool& left, bool& right, bool& down, bool& up)
	{
		const float absX = fabsf(value.x);
		const float absY = fabsf(value.y);
		const bool xActive = absX > xDeadZone;
		const bool yActive = absY > yDeadZone;
		const bool xDominant = xActive && (!yActive || (absX - absY) >= horizontalDominanceMargin);
		const bool yDominant = yActive && (!xActive || (absY - absX) >= verticalDominanceMargin);

		left = xDominant && value.x < 0.0f;
		right = xDominant && value.x > 0.0f;
		down = yDominant && value.y < 0.0f;
		up = yDominant && value.y > 0.0f;
	};

	bool oldLeft = false, oldRight = false, oldDown = false, oldUp = false;
	bool newLeft = false, newRight = false, newDown = false, newUp = false;
	resolveCardinalStates(oldValue, oldLeft, oldRight, oldDown, oldUp);
	resolveCardinalStates(newValue, newLeft, newRight, newDown, newUp);

	auto emitTransition = [&](bool oldState, bool newState, int oldBaseKey, int newBaseKey)
	{
		if (oldBaseKey == newBaseKey)
		{
			PostControllerKeyTransition(oldState, newState, oldBaseKey);
			return;
		}

		// If the grip modifier flips while the stick is still held, switch the
		// virtual key cleanly instead of leaving the old layer latched.
		if (oldState)
			PostControllerKeyTransition(true, false, oldBaseKey);
		if (newState)
			PostControllerKeyTransition(false, true, newBaseKey);
	};

	const int oldLeftKey = oldModifier ? modifiedLeftKey : baseLeftKey;
	const int oldRightKey = oldModifier ? modifiedRightKey : baseRightKey;
	const int oldDownKey = oldModifier ? modifiedDownKey : baseDownKey;
	const int oldUpKey = oldModifier ? modifiedUpKey : baseUpKey;
	const int newLeftKey = newModifier ? modifiedLeftKey : baseLeftKey;
	const int newRightKey = newModifier ? modifiedRightKey : baseRightKey;
	const int newDownKey = newModifier ? modifiedDownKey : baseDownKey;
	const int newUpKey = newModifier ? modifiedUpKey : baseUpKey;

	emitTransition(oldLeft, newLeft, oldLeftKey, newLeftKey);
	emitTransition(oldRight, newRight, oldRightKey, newRightKey);
	emitTransition(oldDown, newDown, oldDownKey, newDownKey);
	emitTransition(oldUp, newUp, oldUpKey, newUpKey);
}

static float NonLinearFilter(float value)
{
	const float absValue = fabsf(value);
	const float squared = absValue * absValue;
	return (value < 0.0f) ? -squared : squared;
}

static float OpenVRLength2D(float x, float y)
{
	return sqrtf(x * x + y * y);
}

static float OpenVRNonLinearFilter(float in)
{
	constexpr float NLF_DEADZONE = 0.1f;
	constexpr float NLF_POWER = 2.2f;

	float val = 0.0f;
	if (in > NLF_DEADZONE)
	{
		val = in > 1.0f ? 1.0f : in;
		val -= NLF_DEADZONE;
		val /= (1.0f - NLF_DEADZONE);
		val = powf(val, NLF_POWER);
	}
	else if (in < -NLF_DEADZONE)
	{
		val = in < -1.0f ? -1.0f : in;
		val += NLF_DEADZONE;
		val /= (1.0f - NLF_DEADZONE);
		val = -powf(fabsf(val), NLF_POWER);
	}

	return val;
}

static void PostRemappedControllerKeyTransition(bool oldPressed, bool newPressed, bool oldModifier, bool newModifier, int baseKey, int modifiedKey)
{
	const int oldKey = oldModifier ? modifiedKey : baseKey;
	const int newKey = newModifier ? modifiedKey : baseKey;

	if (oldKey == newKey)
	{
		PostControllerKeyTransition(oldPressed, newPressed, newKey);
		return;
	}

	if (oldPressed)
		PostControllerKeyTransition(true, false, oldKey);
	if (newPressed)
		PostControllerKeyTransition(false, true, newKey);
}

static bool GetActionBoolean(XrSession session, XrAction action, XrPath subactionPath)
{
	if (session == XR_NULL_HANDLE || action == XR_NULL_HANDLE)
		return false;

	XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
	getInfo.action = action;
	getInfo.subactionPath = subactionPath;

	XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
	if (XR_FAILED(xrGetActionStateBoolean(session, &getInfo, &state)))
		return false;
	return state.currentState != XR_FALSE;
}

static float GetActionFloat(XrSession session, XrAction action, XrPath subactionPath)
{
	if (session == XR_NULL_HANDLE || action == XR_NULL_HANDLE)
		return 0.0f;

	XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
	getInfo.action = action;
	getInfo.subactionPath = subactionPath;

	XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
	if (XR_FAILED(xrGetActionStateFloat(session, &getInfo, &state)))
		return 0.0f;
	return state.currentState;
}

static XrVector2f GetActionVector2f(XrSession session, XrAction action, XrPath subactionPath)
{
	if (session == XR_NULL_HANDLE || action == XR_NULL_HANDLE)
		return { 0.0f, 0.0f };

	XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
	getInfo.action = action;
	getInfo.subactionPath = subactionPath;

	XrActionStateVector2f state{ XR_TYPE_ACTION_STATE_VECTOR2F };
	if (XR_FAILED(xrGetActionStateVector2f(session, &getInfo, &state)))
		return { 0.0f, 0.0f };
	return state.currentState;
}

static FString PathToString(XrInstance instance, XrPath path)
{
	if (instance == XR_NULL_HANDLE || path == XR_NULL_PATH)
		return FString("<null>");

	uint32_t bufferLength = 0;
	if (XR_FAILED(xrPathToString(instance, path, 0, &bufferLength, nullptr)) || bufferLength == 0)
		return FString("<invalid>");

	std::vector<char> buffer(bufferLength);
	if (XR_FAILED(xrPathToString(instance, path, bufferLength, &bufferLength, buffer.data())))
		return FString("<invalid>");

	return FString(buffer.data());
}

static bool IsCurrentInteractionProfile(XrInstance instance, XrSession session, XrPath handPath, const char* profilePathSuffix)
{
	if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE || handPath == XR_NULL_PATH || profilePathSuffix == nullptr)
		return false;

	XrInteractionProfileState profileState{ XR_TYPE_INTERACTION_PROFILE_STATE };
	if (XR_FAILED(xrGetCurrentInteractionProfile(session, handPath, &profileState)) || profileState.interactionProfile == XR_NULL_PATH)
		return false;

	uint32_t bufferLength = 0;
	if (XR_FAILED(xrPathToString(instance, profileState.interactionProfile, 0, &bufferLength, nullptr)) || bufferLength == 0)
		return false;

	std::vector<char> buffer(bufferLength);
	if (XR_FAILED(xrPathToString(instance, profileState.interactionProfile, bufferLength, &bufferLength, buffer.data())))
		return false;

	return strstr(buffer.data(), profilePathSuffix) != nullptr;
}

static void LogBoundSourcesForAction(XrInstance instance, XrSession session, XrAction action, const char* label)
{
	if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE || action == XR_NULL_HANDLE || label == nullptr)
		return;
	if (!xrEnumerateBoundSourcesForAction)
		return;

	XrBoundSourcesForActionEnumerateInfo enumerateInfo{ XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO };
	enumerateInfo.action = action;

	uint32_t sourceCount = 0;
	if (XR_FAILED(xrEnumerateBoundSourcesForAction(session, &enumerateInfo, 0, &sourceCount, nullptr)) || sourceCount == 0)
	{
		// NOT gated on developer. An action with no bound sources is a feature
		// that is silently dead for this controller -- the thumb-touch action,
		// for one, is only ever suggested on the Oculus Touch profile, so on an
		// Index or WMR runtime it reports nothing forever and every pose that
		// asks "is the thumb resting?" gets a permanent no. That is exactly the
		// kind of failure that has to be visible in a log rather than inferred
		// from a hand that will not change shape.
		Printf("OpenXR: bound sources [%s] NONE -- this action is dead on the active controller profile.\n", label);
		return;
	}

	std::vector<XrPath> sources(sourceCount, XR_NULL_PATH);
	if (XR_FAILED(xrEnumerateBoundSourcesForAction(session, &enumerateInfo, sourceCount, &sourceCount, sources.data())))
	{
		if (developer > 0)
			Printf("OpenXR: bound sources [%s] query failed.\n", label);
		return;
	}

	FString list;
	for (uint32_t i = 0; i < sourceCount; ++i)
	{
		const FString source = PathToString(instance, sources[i]);
		if (i != 0)
			list += ", ";
		list += source.GetChars();
	}

	if (developer > 0)
		Printf("OpenXR: bound sources [%s] = %s\n", label, list.GetChars());
}

static void SuggestBindingsForProfile(XrInstance instance, XrPath profilePath, const char* profileName, const std::vector<XrActionSuggestedBinding>& bindings)
{
	if (instance == XR_NULL_HANDLE || profilePath == XR_NULL_PATH || bindings.empty())
		return;

	XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested.interactionProfile = profilePath;
	suggested.suggestedBindings = bindings.data();
	suggested.countSuggestedBindings = (uint32_t)bindings.size();
	const XrResult result = xrSuggestInteractionProfileBindings(instance, &suggested);
	if (XR_FAILED(result))
	{
		if (developer > 0)
		{
			Printf("OpenXR: xrSuggestInteractionProfileBindings failed for profile %s result=%d\n",
				profileName != nullptr ? profileName : "<unknown>",
				(int)result);
		}
	}
}

static void AddBinding(std::vector<XrActionSuggestedBinding>& bindings, XrAction action, XrPath path)
{
	if (action != XR_NULL_HANDLE && path != XR_NULL_PATH)
	{
		bindings.push_back({ action, path });
	}
}

}

static int64_t SelectSwapchainFormat(const std::vector<int64_t>& runtimeFormats, VkFormat preferredFormat)
{
	auto hasFormat = [&](int64_t format) -> bool
	{
		return std::find(runtimeFormats.begin(), runtimeFormats.end(), format) != runtimeFormats.end();
	};

	const VkFormat preferredSrgb =
		(preferredFormat == VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_SRGB :
		(preferredFormat == VK_FORMAT_B8G8R8A8_UNORM) ? VK_FORMAT_B8G8R8A8_SRGB :
		(preferredFormat == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB :
		(preferredFormat == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_SRGB :
		VK_FORMAT_UNDEFINED;
	const VkFormat preferredUnorm =
		(preferredFormat == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_UNORM :
		(preferredFormat == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_UNORM :
		VK_FORMAT_UNDEFINED;

	const int64_t preferred[] = {
		(int64_t)preferredSrgb,
		(preferredSrgb == VK_FORMAT_B8G8R8A8_SRGB) ? (int64_t)VK_FORMAT_R8G8B8A8_SRGB : (int64_t)VK_FORMAT_B8G8R8A8_SRGB,
		(preferredSrgb == VK_FORMAT_R8G8B8A8_SRGB) ? (int64_t)VK_FORMAT_B8G8R8A8_SRGB : (int64_t)VK_FORMAT_R8G8B8A8_SRGB,
		(int64_t)preferredUnorm,
		(preferredUnorm == VK_FORMAT_B8G8R8A8_UNORM) ? (int64_t)VK_FORMAT_R8G8B8A8_UNORM : (int64_t)VK_FORMAT_B8G8R8A8_UNORM,
		(preferredUnorm == VK_FORMAT_R8G8B8A8_UNORM) ? (int64_t)VK_FORMAT_B8G8R8A8_UNORM : (int64_t)VK_FORMAT_R8G8B8A8_UNORM
	};

	for (int64_t format : preferred)
	{
		if (format != VK_FORMAT_UNDEFINED && hasFormat(format))
			return format;
	}

	return runtimeFormats.empty() ? (int64_t)VK_FORMAT_B8G8R8A8_UNORM : runtimeFormats[0];
}

static int64_t SelectFlatSwapchainFormat(const std::vector<int64_t>& runtimeFormats, VkFormat preferredFormat)
{
	auto hasFormat = [&](int64_t format) -> bool
	{
		return std::find(runtimeFormats.begin(), runtimeFormats.end(), format) != runtimeFormats.end();
	};

	const VkFormat preferredUnorm =
		(preferredFormat == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_UNORM :
		(preferredFormat == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_UNORM :
		(preferredFormat == VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_UNORM :
		(preferredFormat == VK_FORMAT_B8G8R8A8_UNORM) ? VK_FORMAT_B8G8R8A8_UNORM :
		VK_FORMAT_UNDEFINED;
	const VkFormat preferredSrgb =
		(preferredFormat == VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_SRGB :
		(preferredFormat == VK_FORMAT_B8G8R8A8_UNORM) ? VK_FORMAT_B8G8R8A8_SRGB :
		(preferredFormat == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB :
		(preferredFormat == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_SRGB :
		VK_FORMAT_UNDEFINED;

	const int64_t preferred[] = {
		(int64_t)preferredUnorm,
		(preferredUnorm == VK_FORMAT_B8G8R8A8_UNORM) ? (int64_t)VK_FORMAT_R8G8B8A8_UNORM : (int64_t)VK_FORMAT_B8G8R8A8_UNORM,
		(preferredUnorm == VK_FORMAT_R8G8B8A8_UNORM) ? (int64_t)VK_FORMAT_B8G8R8A8_UNORM : (int64_t)VK_FORMAT_R8G8B8A8_UNORM,
		(int64_t)preferredSrgb,
		(preferredSrgb == VK_FORMAT_B8G8R8A8_SRGB) ? (int64_t)VK_FORMAT_R8G8B8A8_SRGB : (int64_t)VK_FORMAT_B8G8R8A8_SRGB,
		(preferredSrgb == VK_FORMAT_R8G8B8A8_SRGB) ? (int64_t)VK_FORMAT_B8G8R8A8_SRGB : (int64_t)VK_FORMAT_R8G8B8A8_SRGB
	};

	for (int64_t format : preferred)
	{
		if (format != VK_FORMAT_UNDEFINED && hasFormat(format))
			return format;
	}

	return SelectSwapchainFormat(runtimeFormats, preferredFormat);
}

static bool IsSRGBSwapchainFormat(VkFormat format)
{
	return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

static VREyeInfo* GetDummyOpenXREyes()
{
	static VREyeInfo eyes[2] = { VREyeInfo(0.0f, 1.0f), VREyeInfo(0.0f, 0.0f) };
	return eyes;
}

VKOpenXRDeviceEyePose::VKOpenXRDeviceEyePose(int eye) : VREyeInfo(0.0f, 1.0f), eye(eye)
{
}
VKOpenXRDeviceEyePose::~VKOpenXRDeviceEyePose() {}

VSMatrix VKOpenXRDeviceEyePose::GetProjection(FLOATTYPE fov, FLOATTYPE aspectRatio, FLOATTYPE fovRatio, bool iso_ortho) const
{
	(void)fov;
	(void)aspectRatio;
	(void)fovRatio;
	(void)iso_ortho;

	const float nearZ = (float)screen->GetZNear();
	const float farZ = (float)screen->GetZFar();
	projection = BuildOpenXREyeProjection(currentFov, nearZ, farZ, eye);
	return projection;
}

DAngle VKOpenXRDeviceEyePose::GetRenderFov(DAngle fallback) const
{
	const float fovAdjust = DEG2RAD(clamp<float>(vr_openxr_fov_adjust_deg, -30.0f, 30.0f));
	const float horizontalFov = (currentFov.angleRight - currentFov.angleLeft) + 2.0f * fovAdjust;
	const float verticalFov = (currentFov.angleUp - currentFov.angleDown) + 2.0f * fovAdjust;
	const float renderFovDegrees = std::max(horizontalFov, verticalFov) * (180.0f / (float)M_PI);
	return renderFovDegrees > 0.0f ? DAngle::fromDeg(renderFovDegrees) : fallback;
}

DVector3 VKOpenXRDeviceEyePose::GetViewShift(FRenderViewpoint& vp) const
{
	auto& mode = const_cast<VKOpenXRDeviceMode&>((const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance());
	const float hmdHeight = GetHmdAdjustedHeightInMapUnit(mode.xrUsingStageSpace ? false : mode.xrHasLocalHeightAnchor, mode.xrLocalHeightAnchor);
	const player_t* player = &players[consoleplayer];
	const float playerHeight = (player && player->mo) ? GetDoomPlayerHeightWithoutCrouch(player) : hmdHeight;
	float angles[3];
	angles[0] = vp.HWAngles.Pitch.Degrees();
	angles[1] = GetViewpointYaw();
	angles[2] = vp.HWAngles.Roll.Degrees();

	float forward[3];
	float right[3];
	float up[3];
	AngleVectors(angles, forward, right, up);

	const float stereoSeparation = (vr_ipd * 0.5f) * vr_vunits_per_meter * (eye == 0 ? -1.0f : 1.0f);
	return {
		right[0] * stereoSeparation,
		right[1] * stereoSeparation,
		right[2] * stereoSeparation + (hmdHeight - playerHeight)
	};
}

void VKOpenXRDeviceEyePose::AdjustViewpointUniforms(HWViewpointUniforms& uniforms) const
{
	// Keep the Vulkan scene path aligned with the stable OpenXR/OpenGL path:
	// use the centered head orientation for the gameplay camera and let the
	// per-eye OpenXR projection handle the asymmetric frustum
	uniforms.CalcDependencies();
}

void VKOpenXRDeviceEyePose::SetUp() const
{
	volatile int breakpoint = eye;
	(void)breakpoint;
	static thread_local bool inSetUp = false;
	if (inSetUp)
	{
		return;
	}
	inSetUp = true;
	struct Guard
	{
		bool& flag;
		~Guard()
		{
			flag = false;
		}
	} guard{ inSetUp };

	auto& mode = const_cast<VKOpenXRDeviceMode&>((const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance());
	if (eye >= 0 && (size_t)eye < mode.xrViews.size())
	{
		currentEyePose = mode.xrViews[(size_t)eye].pose;
		currentFov = mode.xrViews[(size_t)eye].fov;
		projection = BuildOpenXREyeProjection(currentFov, (float)screen->GetZNear(), (float)screen->GetZFar(), eye);

	}

	VREyeInfo::SetUp();
	mode.mInVRSceneRender = mode.ShouldUseRecommendedRenderSizeThisFrame();
}

void VKOpenXRDeviceEyePose::TearDown() const
{
	VREyeInfo::TearDown();
	if (eye == 1)
	{
		auto& mode = const_cast<VKOpenXRDeviceMode&>((const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance());
		mode.mInVRSceneRender = false;
	}
}

static void ApplyVPUniforms(HWDrawInfo* di)
{
	auto& renderState = *screen->RenderState();
	di->VPUniforms.CalcDependencies();
	if (screen->mViewpoints)
		di->vpIndex = screen->mViewpoints->SetViewpoint(renderState, &di->VPUniforms);
}

void VKOpenXRDeviceEyePose::AdjustHud() const
{
	if (r_viewpoint.ViewLevel == nullptr)
		return;
	if (VR_ShouldDrawMountedHud())
		return;
	VSMatrix hudProj = GetHUDProjection();

	auto* di = HWDrawInfo::StartDrawInfo(r_viewpoint.ViewLevel, nullptr, r_viewpoint, nullptr);
	if (di)
	{
		di->VPUniforms.mViewMatrix.loadIdentity();
		di->VPUniforms.mProjectionMatrix = hudProj;
		ApplyVPUniforms(di);
		di->EndDrawInfo();
	}
}

void VKOpenXRDeviceEyePose::AdjustBlend(HWDrawInfo* di) const
{
	if (r_viewpoint.ViewLevel == nullptr)
		return;

	bool new_di = false;
	if (di == nullptr)
	{
		di = HWDrawInfo::StartDrawInfo(r_viewpoint.ViewLevel, nullptr, r_viewpoint, nullptr);
		new_di = true;
	}

	di->VPUniforms.mViewMatrix.loadIdentity();
	di->VPUniforms.mProjectionMatrix.loadIdentity();
	di->VPUniforms.mProjectionMatrix.translate(-1, 1, 0);
	di->VPUniforms.mProjectionMatrix.scale(2.0 / SCREENWIDTH, -2.0 / SCREENHEIGHT, -1.0);
	di->ProjectionMatrix2 = di->VPUniforms.mProjectionMatrix;
	ApplyVPUniforms(di);

	if (new_di)
	{
		di->EndDrawInfo();
	}
}

VKOpenXRDeviceMode::VKOpenXRDeviceMode()
	: VRMode(2, 1.0f, 1.0f, 1.0f, GetDummyOpenXREyes())
{
	mEyes[0] = std::make_unique<VKOpenXRDeviceEyePose>(0);
	mEyes[1] = std::make_unique<VKOpenXRDeviceEyePose>(1);
	VRMode::mEyes[0] = mEyes[0].get();
	VRMode::mEyes[1] = mEyes[1].get();
	isSetup = false;
}

VKOpenXRDeviceMode::~VKOpenXRDeviceMode()
{
	DestroyOpenXR();
}

const VRMode& VKOpenXRDeviceMode::getInstance()
{
	static VKOpenXRDeviceMode instance;
	return instance;
}

bool OpenXRInputDeviceAvailable()
{
	const auto* mode = dynamic_cast<const VKOpenXRDeviceMode*>(VRMode::GetVRModeCached(true));
	return mode != nullptr && mode->HasActiveInputSession();
}

bool OpenXROnHandIsRight()
{
	return GetMainHandIndex() == 1;
}

bool OpenXR_GetThumbstick(int abstractHand, float& x, float& y)
{
	const auto* mode = dynamic_cast<const VKOpenXRDeviceMode*>(VRMode::GetVRModeCached(true));
	if (mode == nullptr)
	{
		return false;
	}
	const int physicalHand = (abstractHand == 1) ? GetOffHandIndex() : GetMainHandIndex();
	return mode->GetThumbstickState(physicalHand, x, y);
}

bool VKOpenXRDeviceMode::HasActiveInputSession() const
{
	return isOpenXRReady && isSessionRunning && xrSession != XR_NULL_HANDLE;
}

bool VKOpenXRDeviceMode::GetThumbstickState(int physicalHand, float& x, float& y) const
{
	if (physicalHand < 0 || physicalHand > 1 || !HasActiveInputSession())
	{
		return false;
	}
	x = xrLastThumbstickState[physicalHand].x;
	y = xrLastThumbstickState[physicalHand].y;
	return true;
}

static void DisableOpenXRModeForCurrentRun(const char* reason)
{
	if (vr_mode == VR_OPENXR_MOBILE)
	{
		Printf("OpenXR unavailable: %s. Falling back to vr_mode 0.\n", reason);
		vr_mode = VR_MONO;
	}
}

bool VKOpenXRDeviceMode::IsInitialized() const
{
	if (isOpenXRReady)
		return true;

	const uint64_t frameTime = screen != nullptr ? screen->FrameTime : 0;
	if (xrInitProbeFrameTime == frameTime)
		return xrInitProbeResult;

	OpenXRVulkanBootstrapInfo xrInfo;
	xrInitProbeResult = QueryOpenXRVulkanBootstrapInfo(xrInfo);
	xrInitProbeFrameTime = frameTime;
	if (!xrInitProbeResult)
	{
		DisableOpenXRModeForCurrentRun("runtime probe failed");
	}
	return xrInitProbeResult;
}

bool VKOpenXRDeviceMode::GetRecommendedRenderSize(int& outWidth, int& outHeight) const
{
	int width = 0;
	int height = 0;

	if (sceneWidth != 0 && sceneHeight != 0)
	{
		width = (int)sceneWidth;
		height = (int)sceneHeight;
	}
	else if (!xrViewConfigs.empty() && xrViewConfigs[0].recommendedImageRectWidth != 0 && xrViewConfigs[0].recommendedImageRectHeight != 0)
	{
		width = (int)xrViewConfigs[0].recommendedImageRectWidth;
		height = (int)xrViewConfigs[0].recommendedImageRectHeight;
	}
	else
	{
		outWidth = 0;
		outHeight = 0;
		return false;
	}

	const float renderScale = clamp<float>(vr_openxr_render_scale, 0.25f, 4.0f);
	outWidth = std::max(1, (int)std::lround(width * renderScale));
	outHeight = std::max(1, (int)std::lround(height * renderScale));
	return true;
}

bool VKOpenXRDeviceMode::ShouldUseRecommendedRenderSizeThisFrame() const
{
	// vr_menu_keep_world: MenuOverWorld renders the same stereo eye images as
	// gameplay, so it needs the same recommended per-eye size and viewport.
	return mFrameRenderMode == FrameRenderMode::GameplayEyes || mFrameRenderMode == FrameRenderMode::MenuOverWorld;
}

bool VKOpenXRDeviceMode::ShouldUseScreenLayerForCurrentFrame() const
{
	// Deliberately still true for MenuOverWorld: this is what keeps the menu 2D
	// out of the eye images and routes it (and the menu pointer) to the quad.
	return mFrameRenderMode != FrameRenderMode::GameplayEyes;
}

bool VKOpenXRDeviceMode::IsMenuOverWorldFrame() const
{
	return mFrameRenderMode == FrameRenderMode::MenuOverWorld;
}

static void ApplyOpenXRGameplayViewport(DFrameBuffer* screen, int width, int height)
{
	screen->mSceneViewport.left = 0;
	screen->mSceneViewport.top = 0;
	screen->mSceneViewport.width = width;
	screen->mSceneViewport.height = height;
	screen->mScreenViewport.left = 0;
	screen->mScreenViewport.top = 0;
	screen->mScreenViewport.width = width;
	screen->mScreenViewport.height = height;
}

template<class TYPE>
static TYPE& getHUDValue(TYPE& automap, TYPE& hud)
{
	return (automapactive && !vr_automap_use_hud) ? automap : hud;
}

VSMatrix VKOpenXRDeviceEyePose::GetHUDProjection() const
{
	VSMatrix hudProjection;
	hudProjection.loadIdentity();

	const float hudStereo = getHUDValue<FFloatCVarRef>(vr_automap_stereo, vr_hud_stereo);
	const float stereoSeparation =
		(vr_ipd * 0.5f) * vr_vunits_per_meter * hudStereo * (eye == 1 ? -1.0f : 1.0f);
	hudProjection.translate(stereoSeparation, 0.0f, 0.0f);

	hudProjection.scale(-vr_vunits_per_meter, vr_vunits_per_meter, -vr_vunits_per_meter);

	const double pixelstretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
	hudProjection.scale(1.0, (FLOATTYPE)pixelstretch, 1.0);

	if (eye >= 0)
	{
		const auto& mode = const_cast<VKOpenXRDeviceMode&>((const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance());
		if ((size_t)eye < mode.xrViews.size())
		{
			const XrQuaternionf headOrientation = GetCenteredViewOrientation(mode.xrViews);
			const XrVector3f eyeOffsetMeters = {
				currentEyePose.position.x - hmdPosition[0],
				currentEyePose.position.y - hmdPosition[1],
				currentEyePose.position.z - hmdPosition[2]
			};
			const XrVector3f localEyeOffset = RotateVector(ConjugateQuaternion(headOrientation), eyeOffsetMeters);
			// Match OpenVR's eye-to-head transform contribution so HUD depth, size, and stereo
			hudProjection.translate(localEyeOffset.x, localEyeOffset.y, localEyeOffset.z);
		}
	}

	if (getHUDValue<FBoolCVarRef>(vr_automap_fixed_roll, vr_hud_fixed_roll))
	{
		hudProjection.rotate(-hmdorientation[2], 0, 0, 1);
	}

	hudProjection.rotate(getHUDValue<FFloatCVarRef>(vr_automap_rotate, vr_hud_rotate), 1, 0, 0);

	if (getHUDValue<FBoolCVarRef>(vr_automap_fixed_pitch, vr_hud_fixed_pitch))
	{
		hudProjection.rotate(-hmdorientation[0], 1, 0, 0);
	}

	hudProjection.translate(0.0f, 0.0f, getHUDValue<FFloatCVarRef>(vr_automap_distance, vr_hud_distance));
	const float hudScale = getHUDValue<FFloatCVarRef>(vr_automap_scale, vr_hud_scale);
	hudProjection.scale(-hudScale, hudScale, -hudScale);

	float screenWidth = (float)SCREENWIDTH;
	float screenHeight = (float)SCREENHEIGHT;
	hudProjection.translate(-1.0f, 1.0f, 0.0f);
	hudProjection.scale(2.0f / screenWidth, -2.0f / screenHeight, -1.0f);

	// Match OpenVR/GL OpenXR behavior: compose eye projection with the HUD
	// transform so the HUD is camera-anchored in front of the user.
	VSMatrix finalProjection(projection);
	finalProjection.multMatrix(hudProjection);

	return finalProjection;
}

VSMatrix VKOpenXRDeviceMode::GetHUDProjection() const
{
	for (int i = 0; i < 2; ++i)
	{
		if (mEyes[i] != nullptr && mEyes[i]->isActive())
		{
			return mEyes[i]->GetHUDProjection();
		}
	}
	return GetHUDSpriteProjection();
}

bool VKOpenXRDeviceMode::InitializeOpenXR() const
{
	auto fail = [&]() -> bool
	{
		xrInitProbeFrameTime = screen != nullptr ? screen->FrameTime : 0;
		xrInitProbeResult = false;
		DestroyOpenXR();
		DisableOpenXRModeForCurrentRun("initialization failed");
		return false;
	};

	if (isOpenXRReady)
		return true;
	if (xrInstance != XR_NULL_HANDLE || xrSession != XR_NULL_HANDLE || xrSwapchain != XR_NULL_HANDLE ||
		xrSpace != XR_NULL_HANDLE || xrActionSet != XR_NULL_HANDLE || xrPoseAction != XR_NULL_HANDLE ||
		xrSelectAction != XR_NULL_HANDLE || xrMenuAction != XR_NULL_HANDLE || xrGripAction != XR_NULL_HANDLE ||
		xrThumbClickAction != XR_NULL_HANDLE || xrThumbstickAction != XR_NULL_HANDLE ||
		xrPrimaryAction != XR_NULL_HANDLE || xrSecondaryAction != XR_NULL_HANDLE || xrVkInstance != nullptr ||
		xrVkDevice != nullptr || xrVkCommandPool != nullptr || xrVkCommandBuffer != nullptr ||
		xrVkSubmitFence != nullptr || !xrSwapchainImages.empty() || !xrViewConfigs.empty() ||
		!xrViews.empty() || !xrProjectionViews.empty() || xrViewCount != 0 || sceneWidth != 0 || sceneHeight != 0)
	{
		DestroyOpenXR();
	}
	if (!IsOpenXRPresent())
	{
		return fail();
	}
	OpenXRVulkanBootstrapInfo xrBootstrapInfo;
	const bool hasBootstrapInfo = QueryOpenXRVulkanBootstrapInfo(xrBootstrapInfo);
	// Some runtimes expose the loader DLL but do not support Vulkan OpenXR.
	const bool hasVulkanEnable = hasBootstrapInfo ? xrBootstrapInfo.supportsVulkanEnable : HasOpenXRExtension(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	const bool hasVulkanEnable2 = hasBootstrapInfo ? xrBootstrapInfo.supportsVulkanEnable2 : HasOpenXRExtension(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	if (!hasVulkanEnable && !hasVulkanEnable2)
	{
		Printf("OpenXR: runtime does not advertise %s or %s, skipping OpenXR initialization.\n",
			XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
		return fail();
	}


	std::vector<const char*> extensions;
	if (hasVulkanEnable)
		extensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (hasVulkanEnable2)
		extensions.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	auto bootstrapHasExtension = [&](const char* extensionName)
	{
		if (!hasBootstrapInfo)
		{
			return HasOpenXRExtension(extensionName);
		}
		const auto hasExt = [&](const std::string& name) { return name == extensionName; };
		return std::any_of(xrBootstrapInfo.requiredInstanceExtensions.begin(), xrBootstrapInfo.requiredInstanceExtensions.end(), hasExt);
	};
	xrHasEquirectBackdrop = bootstrapHasExtension(XR_KHR_COMPOSITION_LAYER_EQUIRECT_EXTENSION_NAME);
	if (xrHasEquirectBackdrop)
	{
		extensions.push_back(XR_KHR_COMPOSITION_LAYER_EQUIRECT_EXTENSION_NAME);
	}
	xrHasFBColorSpace = bootstrapHasExtension(XR_FB_COLOR_SPACE_EXTENSION_NAME);
	if (xrHasFBColorSpace)
	{
		extensions.push_back(XR_FB_COLOR_SPACE_EXTENSION_NAME);
	}
#ifdef XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME
	xrHasDisplayRefreshRate = bootstrapHasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if (xrHasDisplayRefreshRate)
	{
		extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	}
#endif
	XrApplicationInfo appInfo{};
	appInfo.apiVersion = XR_API_VERSION_1_0;
	appInfo.applicationVersion = 1;
	appInfo.engineVersion = 1;
	strncpy(appInfo.applicationName, "DoomXR", sizeof(appInfo.applicationName) - 1);
	strncpy(appInfo.engineName, "DoomXR", sizeof(appInfo.engineName) - 1);

	XrInstanceCreateInfo instanceInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
	instanceInfo.applicationInfo = appInfo;
	instanceInfo.enabledExtensionCount = (uint32_t)extensions.size();
	instanceInfo.enabledExtensionNames = extensions.data();
	XrResult xrResult = xrCreateInstance(&instanceInfo, &xrInstance);
	if (XR_FAILED(xrResult))
	{
		return fail();
	}

	auto loadProc = [&](const char* name, PFN_xrVoidFunction* out) -> bool
	{
		if (XR_FAILED(xrGetInstanceProcAddr(xrInstance, name, out)))
		{
			return false;
		}
		return *out != nullptr;
	};

	loadProc("xrGetVulkanGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanGraphicsRequirementsKHR_inst));
	loadProc("xrGetVulkanGraphicsDeviceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanGraphicsDeviceKHR_inst));
	loadProc("xrGetVulkanGraphicsRequirements2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanGraphicsRequirements2KHR_inst));
	loadProc("xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanGraphicsDevice2KHR_inst));
#ifdef XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME
	if (xrHasDisplayRefreshRate)
	{
		loadProc("xrEnumerateDisplayRefreshRatesFB", reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateDisplayRefreshRatesFB_inst));
		loadProc("xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetDisplayRefreshRateFB_inst));
		loadProc("xrRequestDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&xrRequestDisplayRefreshRateFB_inst));
		if (xrEnumerateDisplayRefreshRatesFB_inst == nullptr || xrRequestDisplayRefreshRateFB_inst == nullptr)
		{
			xrHasDisplayRefreshRate = false;
		}
	}
#endif

	XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (XR_FAILED(xrGetSystem(xrInstance, &systemInfo, &xrSystemId)))
	{
		return fail();
	}

	auto* vkfb = dynamic_cast<VulkanRenderDevice*>(screen);
	if (!vkfb)
	{
		return fail();
	}

	XrGraphicsRequirementsVulkanKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
	if (xrGetVulkanGraphicsRequirements2KHR_inst)
	{
		if (XR_FAILED(xrGetVulkanGraphicsRequirements2KHR_inst(xrInstance, xrSystemId, &graphicsRequirements)))
		{
			return fail();
		}
	}
	else if (xrGetVulkanGraphicsRequirementsKHR_inst)
	{
		if (XR_FAILED(xrGetVulkanGraphicsRequirementsKHR_inst(xrInstance, xrSystemId, &graphicsRequirements)))
		{
			return fail();
		}
	}
	xrVkDevice = vkfb->device;
	xrVkInstance = xrVkDevice ? xrVkDevice->Instance : nullptr;
	if (!xrVkDevice || !xrVkInstance)
	{
		return fail();
	}

	XrGraphicsBindingVulkanKHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
	VkPhysicalDevice xrPhysicalDevice = VK_NULL_HANDLE;
	if (xrGetVulkanGraphicsDevice2KHR_inst)
	{
		XrVulkanGraphicsDeviceGetInfoKHR getInfo{ XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
		getInfo.systemId = xrSystemId;
		getInfo.vulkanInstance = xrVkInstance->Instance;
		if (XR_FAILED(xrGetVulkanGraphicsDevice2KHR_inst(xrInstance, &getInfo, &xrPhysicalDevice)))
		{
			return fail();
		}
		if (xrVkDevice->PhysicalDevice.Device != xrPhysicalDevice)
		{
			return fail();
		}
	}
	else if (xrGetVulkanGraphicsDeviceKHR_inst)
	{
		if (XR_FAILED(xrGetVulkanGraphicsDeviceKHR_inst(xrInstance, xrSystemId, xrVkInstance->Instance, &xrPhysicalDevice)))
		{
			return fail();
		}
		if (xrVkDevice->PhysicalDevice.Device != xrPhysicalDevice)
		{
			return fail();
		}
	}

	xrVkCommandPool = CommandPoolBuilder()
		.QueueFamily(xrVkDevice->GraphicsFamily)
		.DebugName("OpenXRCommandPool")
		.Create(xrVkDevice.get());
	xrVkCommandBuffer = xrVkCommandPool->createBuffer();
	xrVkCommandBuffer->SetDebugName("OpenXRCommandBuffer");
	xrVkSubmitFence = std::make_unique<VulkanFence>(xrVkDevice.get());

	binding.instance = xrVkInstance->Instance;
	binding.physicalDevice = xrVkDevice->PhysicalDevice.Device;
	binding.device = xrVkDevice->device;
	binding.queueFamilyIndex = xrVkDevice->GraphicsFamily;
	binding.queueIndex = 0;

	XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = xrSystemId;
	xrResult = xrCreateSession(xrInstance, &sessionInfo, &xrSession);
	if (XR_FAILED(xrResult) && ShouldRetryCreateSession(xrResult))
	{
		xrResult = xrCreateSession(xrInstance, &sessionInfo, &xrSession);
	}
	if (XR_FAILED(xrResult))
	{
		return fail();
	}

	if (xrHasFBColorSpace && xrEnumerateColorSpacesFB && xrSetColorSpaceFB)
	{
		uint32_t colorSpaceCount = 0;
		if (XR_SUCCEEDED(xrEnumerateColorSpacesFB(xrSession, 0, &colorSpaceCount, nullptr)) && colorSpaceCount > 0)
		{
			std::vector<XrColorSpaceFB> supportedColorSpaces(colorSpaceCount);
			if (XR_SUCCEEDED(xrEnumerateColorSpacesFB(xrSession, colorSpaceCount, &colorSpaceCount, supportedColorSpaces.data())))
			{
				const XrColorSpaceFB requestedColorSpace = SelectPreferredColorSpace(supportedColorSpaces);
				if (XR_SUCCEEDED(xrSetColorSpaceFB(xrSession, requestedColorSpace)))
				{
					if (developer > 0)
						Printf("OpenXR: requested FB color space %d from %u supported modes.\n", (int)requestedColorSpace, colorSpaceCount);
				}
			}
		}
	}

	XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.poseInReferenceSpace = XrPosef{ {0,0,0,1}, {0,0,0} };
	xrUsingStageSpace = false;
	xrSpace = XR_NULL_HANDLE;

	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	if (XR_SUCCEEDED(xrCreateReferenceSpace(xrSession, &spaceInfo, &xrSpace)))
	{
		xrUsingStageSpace = true;
	}
	else
	{
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		if (XR_FAILED(xrCreateReferenceSpace(xrSession, &spaceInfo, &xrSpace)))
		{
			return fail();
		}
	}

	if (xrSpace == XR_NULL_HANDLE)
	{
		return fail();
	}

	// Action setup
	XrActionSetCreateInfo actionSetInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strncpy(actionSetInfo.actionSetName, "gameplay", sizeof(actionSetInfo.actionSetName) - 1);
	strncpy(actionSetInfo.localizedActionSetName, "Gameplay", sizeof(actionSetInfo.localizedActionSetName) - 1);
	xrCreateActionSet(xrInstance, &actionSetInfo, &xrActionSet);

	xrStringToPath(xrInstance, "/user/hand/left", &xrLeftHandPath);
	xrStringToPath(xrInstance, "/user/hand/right", &xrRightHandPath);
	const XrPath subactionPaths[2] = { xrLeftHandPath, xrRightHandPath };

	auto createAction = [&](const char* name, const char* localizedName, XrActionType type, XrAction& out)
	{
		XrActionCreateInfo actionInfo{ XR_TYPE_ACTION_CREATE_INFO };
		actionInfo.actionType = type;
		strncpy(actionInfo.actionName, name, sizeof(actionInfo.actionName) - 1);
		strncpy(actionInfo.localizedActionName, localizedName, sizeof(actionInfo.localizedActionName) - 1);
		actionInfo.countSubactionPaths = 2;
		actionInfo.subactionPaths = subactionPaths;
		xrCreateAction(xrActionSet, &actionInfo, &out);
	};

	createAction("hand_pose", "Hand Pose", XR_ACTION_TYPE_POSE_INPUT, xrPoseAction);
	// Measurement only -- see the declaration in the header. hand_pose is bound
	// to the AIM pose; this one takes the GRIP pose so the two can be compared.
	createAction("grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT, xrGripPoseAction);
	createAction("select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT, xrSelectAction);
	createAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, xrMenuAction);
	createAction("grip", "Grip", XR_ACTION_TYPE_BOOLEAN_INPUT, xrGripAction);
	createAction("thumb_click", "Thumb Click", XR_ACTION_TYPE_BOOLEAN_INPUT, xrThumbClickAction);
	createAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, xrThumbstickAction);
	createAction("trackpad", "Trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT, xrTrackpadAction);
	createAction("button_a", "Button A", XR_ACTION_TYPE_BOOLEAN_INPUT, xrAAction);
	createAction("button_b", "Button B", XR_ACTION_TYPE_BOOLEAN_INPUT, xrBAction);
	createAction("button_x", "Button X", XR_ACTION_TYPE_BOOLEAN_INPUT, xrXAction);
	createAction("button_y", "Button Y", XR_ACTION_TYPE_BOOLEAN_INPUT, xrYAction);
	createAction("primary", "Primary", XR_ACTION_TYPE_BOOLEAN_INPUT, xrPrimaryAction);
	createAction("secondary", "Secondary", XR_ACTION_TYPE_BOOLEAN_INPUT, xrSecondaryAction);
	createAction("thumb_touch", "Thumb Touch", XR_ACTION_TYPE_BOOLEAN_INPUT, xrThumbTouchAction);
	createAction("trigger_touch", "Trigger Touch", XR_ACTION_TYPE_BOOLEAN_INPUT, xrTriggerTouchAction);
	createAction("trigger_value", "Trigger Value", XR_ACTION_TYPE_FLOAT_INPUT, xrTriggerValueAction);
	createAction("grip_value", "Grip Value", XR_ACTION_TYPE_FLOAT_INPUT, xrGripValueAction);
	createAction("haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, xrHapticAction);

	XrPath leftTriggerClickPath = XR_NULL_PATH;
	XrPath rightTriggerClickPath = XR_NULL_PATH;
	XrPath leftTriggerValuePath = XR_NULL_PATH;
	XrPath rightTriggerValuePath = XR_NULL_PATH;
	XrPath leftSqueezeClickPath = XR_NULL_PATH;
	XrPath rightSqueezeClickPath = XR_NULL_PATH;
	XrPath leftSqueezeValuePath = XR_NULL_PATH;
	XrPath rightSqueezeValuePath = XR_NULL_PATH;
	XrPath leftThumbClickPath = XR_NULL_PATH;
	XrPath rightThumbClickPath = XR_NULL_PATH;
	XrPath leftThumbstickPath = XR_NULL_PATH;
	XrPath rightThumbstickPath = XR_NULL_PATH;
	XrPath leftTrackpadPath = XR_NULL_PATH;
	XrPath rightTrackpadPath = XR_NULL_PATH;
	XrPath leftTrackpadClickPath = XR_NULL_PATH;
	XrPath rightTrackpadClickPath = XR_NULL_PATH;
	XrPath leftTrackpadTouchPath = XR_NULL_PATH;
	XrPath rightTrackpadTouchPath = XR_NULL_PATH;
	XrPath leftHapticPath = XR_NULL_PATH;
	XrPath rightHapticPath = XR_NULL_PATH;
	XrPath leftMenuClickPath = XR_NULL_PATH;
	XrPath rightMenuClickPath = XR_NULL_PATH;
	XrPath leftSelectClickPath = XR_NULL_PATH;
	XrPath rightSelectClickPath = XR_NULL_PATH;
	XrPath leftGripPosePath = XR_NULL_PATH;
	XrPath rightGripPosePath = XR_NULL_PATH;
	XrPath leftAimPosePath = XR_NULL_PATH;
	XrPath rightAimPosePath = XR_NULL_PATH;
	XrPath leftXClickPath = XR_NULL_PATH;
	XrPath leftYClickPath = XR_NULL_PATH;
	XrPath leftPrimaryClickPath = XR_NULL_PATH;
	XrPath rightPrimaryClickPath = XR_NULL_PATH;
	XrPath leftSecondaryClickPath = XR_NULL_PATH;
	XrPath rightSecondaryClickPath = XR_NULL_PATH;

	xrStringToPath(xrInstance, "/user/hand/left/input/trigger/click", &leftTriggerClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trigger/click", &rightTriggerClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/trigger/value", &leftTriggerValuePath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trigger/value", &rightTriggerValuePath);
	xrStringToPath(xrInstance, "/user/hand/left/input/squeeze/click", &leftSqueezeClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/squeeze/click", &rightSqueezeClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/squeeze/value", &leftSqueezeValuePath);
	xrStringToPath(xrInstance, "/user/hand/right/input/squeeze/value", &rightSqueezeValuePath);
	xrStringToPath(xrInstance, "/user/hand/left/input/thumbstick/click", &leftThumbClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/thumbstick/click", &rightThumbClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/thumbstick", &leftThumbstickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/thumbstick", &rightThumbstickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/trackpad", &leftTrackpadPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trackpad", &rightTrackpadPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/trackpad/click", &leftTrackpadClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trackpad/click", &rightTrackpadClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/trackpad/touch", &leftTrackpadTouchPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trackpad/touch", &rightTrackpadTouchPath);
	xrStringToPath(xrInstance, "/user/hand/left/output/haptic", &leftHapticPath);
	xrStringToPath(xrInstance, "/user/hand/right/output/haptic", &rightHapticPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/menu/click", &leftMenuClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/menu/click", &rightMenuClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/select/click", &leftSelectClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/select/click", &rightSelectClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/grip/pose", &leftGripPosePath);
	xrStringToPath(xrInstance, "/user/hand/right/input/grip/pose", &rightGripPosePath);
	xrStringToPath(xrInstance, "/user/hand/left/input/aim/pose", &leftAimPosePath);
	xrStringToPath(xrInstance, "/user/hand/right/input/aim/pose", &rightAimPosePath);
	xrStringToPath(xrInstance, "/user/hand/left/input/x/click", &leftXClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/y/click", &leftYClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/a/click", &leftPrimaryClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/a/click", &rightPrimaryClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/b/click", &leftSecondaryClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/b/click", &rightSecondaryClickPath);

	// Capacitive touch paths.
	//
	// Suggested ONLY for the Oculus Touch profile below. A suggested binding
	// naming a path the profile does not define makes
	// xrSuggestInteractionProfileBindings reject the ENTIRE profile rather than
	// the single binding, which presents as the controller losing all input --
	// so these are withheld from Vive, WMR and the simple profile, none of
	// which define them.
	XrPath leftThumbrestTouchPath = XR_NULL_PATH;
	XrPath rightThumbrestTouchPath = XR_NULL_PATH;
	XrPath leftThumbstickTouchPath = XR_NULL_PATH;
	XrPath rightThumbstickTouchPath = XR_NULL_PATH;
	XrPath leftXTouchPath = XR_NULL_PATH;
	XrPath leftYTouchPath = XR_NULL_PATH;
	XrPath rightATouchPath = XR_NULL_PATH;
	XrPath rightBTouchPath = XR_NULL_PATH;
	XrPath leftTriggerTouchPath = XR_NULL_PATH;
	XrPath rightTriggerTouchPath = XR_NULL_PATH;
	xrStringToPath(xrInstance, "/user/hand/left/input/thumbrest/touch", &leftThumbrestTouchPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/thumbrest/touch", &rightThumbrestTouchPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/thumbstick/touch", &leftThumbstickTouchPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/thumbstick/touch", &rightThumbstickTouchPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/x/touch", &leftXTouchPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/y/touch", &leftYTouchPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/a/touch", &rightATouchPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/b/touch", &rightBTouchPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/trigger/touch", &leftTriggerTouchPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trigger/touch", &rightTriggerTouchPath);

	XrPath simpleProfile = XR_NULL_PATH;
	XrPath viveProfile = XR_NULL_PATH;
	XrPath touchProfile = XR_NULL_PATH;
	XrPath indexProfile = XR_NULL_PATH;
	XrPath wmrProfile = XR_NULL_PATH;
	xrStringToPath(xrInstance, "/interaction_profiles/khr/simple_controller", &simpleProfile);
	xrStringToPath(xrInstance, "/interaction_profiles/htc/vive_controller", &viveProfile);
	xrStringToPath(xrInstance, "/interaction_profiles/oculus/touch_controller", &touchProfile);
	xrStringToPath(xrInstance, "/interaction_profiles/valve/index_controller", &indexProfile);
	xrStringToPath(xrInstance, "/interaction_profiles/microsoft/motion_controller", &wmrProfile);

	std::vector<XrActionSuggestedBinding> simpleBindings;
	AddBinding(simpleBindings, xrSelectAction, leftSelectClickPath);
	AddBinding(simpleBindings, xrSelectAction, rightSelectClickPath);
	AddBinding(simpleBindings, xrMenuAction, leftMenuClickPath);
	AddBinding(simpleBindings, xrMenuAction, rightMenuClickPath);
	AddBinding(simpleBindings, xrPoseAction, leftAimPosePath);
	AddBinding(simpleBindings, xrPoseAction, rightAimPosePath);
	AddBinding(simpleBindings, xrGripPoseAction, leftGripPosePath);
	AddBinding(simpleBindings, xrGripPoseAction, rightGripPosePath);
	AddBinding(simpleBindings, xrHapticAction, leftHapticPath);
	AddBinding(simpleBindings, xrHapticAction, rightHapticPath);
	SuggestBindingsForProfile(xrInstance, simpleProfile, "KHR simple", simpleBindings);

	std::vector<XrActionSuggestedBinding> viveBindings;
	AddBinding(viveBindings, xrSelectAction, leftTriggerClickPath);
	AddBinding(viveBindings, xrSelectAction, rightTriggerClickPath);
	AddBinding(viveBindings, xrGripAction, leftSqueezeClickPath);
	AddBinding(viveBindings, xrGripAction, rightSqueezeClickPath);
	// The Vive wand has a real analog trigger; its squeeze is a click only, so
	// the float action there resolves to 0 or 1. That is the correct answer
	// for that hardware rather than a gap -- a caller gets the ends of the
	// travel and simply has nothing in between to read.
	AddBinding(viveBindings, xrTriggerValueAction, leftTriggerValuePath);
	AddBinding(viveBindings, xrTriggerValueAction, rightTriggerValuePath);
	AddBinding(viveBindings, xrGripValueAction, leftSqueezeClickPath);
	AddBinding(viveBindings, xrGripValueAction, rightSqueezeClickPath);
	AddBinding(viveBindings, xrTrackpadAction, leftTrackpadPath);
	AddBinding(viveBindings, xrTrackpadAction, rightTrackpadPath);
	AddBinding(viveBindings, xrThumbClickAction, leftTrackpadClickPath);
	AddBinding(viveBindings, xrThumbClickAction, rightTrackpadClickPath);
	AddBinding(viveBindings, xrMenuAction, leftMenuClickPath);
	AddBinding(viveBindings, xrMenuAction, rightMenuClickPath);
	AddBinding(viveBindings, xrPoseAction, leftAimPosePath);
	AddBinding(viveBindings, xrPoseAction, rightAimPosePath);
	AddBinding(viveBindings, xrGripPoseAction, leftGripPosePath);
	AddBinding(viveBindings, xrGripPoseAction, rightGripPosePath);
	AddBinding(viveBindings, xrHapticAction, leftHapticPath);
	AddBinding(viveBindings, xrHapticAction, rightHapticPath);
	SuggestBindingsForProfile(xrInstance, viveProfile, "Vive", viveBindings);

	std::vector<XrActionSuggestedBinding> touchBindings;
	AddBinding(touchBindings, xrSelectAction, leftTriggerValuePath);
	AddBinding(touchBindings, xrSelectAction, rightTriggerValuePath);
	AddBinding(touchBindings, xrGripAction, leftSqueezeValuePath);
	AddBinding(touchBindings, xrGripAction, rightSqueezeValuePath);
	AddBinding(touchBindings, xrTriggerValueAction, leftTriggerValuePath);
	AddBinding(touchBindings, xrTriggerValueAction, rightTriggerValuePath);
	AddBinding(touchBindings, xrGripValueAction, leftSqueezeValuePath);
	AddBinding(touchBindings, xrGripValueAction, rightSqueezeValuePath);
	AddBinding(touchBindings, xrThumbClickAction, leftThumbClickPath);
	AddBinding(touchBindings, xrThumbClickAction, rightThumbClickPath);
	AddBinding(touchBindings, xrThumbstickAction, leftThumbstickPath);
	AddBinding(touchBindings, xrThumbstickAction, rightThumbstickPath);
	AddBinding(touchBindings, xrXAction, leftXClickPath);
	AddBinding(touchBindings, xrYAction, leftYClickPath);
	AddBinding(touchBindings, xrAAction, rightPrimaryClickPath);
	AddBinding(touchBindings, xrBAction, rightSecondaryClickPath);
	AddBinding(touchBindings, xrMenuAction, leftMenuClickPath);
	AddBinding(touchBindings, xrPoseAction, leftAimPosePath);
	AddBinding(touchBindings, xrPoseAction, rightAimPosePath);
	AddBinding(touchBindings, xrGripPoseAction, leftGripPosePath);
	AddBinding(touchBindings, xrGripPoseAction, rightGripPosePath);
	AddBinding(touchBindings, xrHapticAction, leftHapticPath);
	AddBinding(touchBindings, xrHapticAction, rightHapticPath);
	// One action bound to several paths: a boolean action is true when ANY
	// bound source is, so "the thumb is resting on something" needs no
	// per-surface logic here -- the runtime does the OR.
	AddBinding(touchBindings, xrThumbTouchAction, leftThumbrestTouchPath);
	AddBinding(touchBindings, xrThumbTouchAction, rightThumbrestTouchPath);
	AddBinding(touchBindings, xrThumbTouchAction, leftThumbstickTouchPath);
	AddBinding(touchBindings, xrThumbTouchAction, rightThumbstickTouchPath);
	AddBinding(touchBindings, xrThumbTouchAction, leftXTouchPath);
	AddBinding(touchBindings, xrThumbTouchAction, leftYTouchPath);
	AddBinding(touchBindings, xrThumbTouchAction, rightATouchPath);
	AddBinding(touchBindings, xrThumbTouchAction, rightBTouchPath);
	AddBinding(touchBindings, xrTriggerTouchAction, leftTriggerTouchPath);
	AddBinding(touchBindings, xrTriggerTouchAction, rightTriggerTouchPath);
	SuggestBindingsForProfile(xrInstance, touchProfile, "Oculus Touch", touchBindings);

	std::vector<XrActionSuggestedBinding> indexBindings;
	AddBinding(indexBindings, xrSelectAction, leftTriggerValuePath);
	AddBinding(indexBindings, xrSelectAction, rightTriggerValuePath);
	AddBinding(indexBindings, xrGripAction, leftSqueezeValuePath);
	AddBinding(indexBindings, xrGripAction, rightSqueezeValuePath);
	AddBinding(indexBindings, xrTriggerValueAction, leftTriggerValuePath);
	AddBinding(indexBindings, xrTriggerValueAction, rightTriggerValuePath);
	AddBinding(indexBindings, xrGripValueAction, leftSqueezeValuePath);
	AddBinding(indexBindings, xrGripValueAction, rightSqueezeValuePath);
	AddBinding(indexBindings, xrThumbClickAction, leftThumbClickPath);
	AddBinding(indexBindings, xrThumbClickAction, rightThumbClickPath);
	AddBinding(indexBindings, xrThumbstickAction, leftThumbstickPath);
	AddBinding(indexBindings, xrThumbstickAction, rightThumbstickPath);
	AddBinding(indexBindings, xrXAction, leftXClickPath);
	AddBinding(indexBindings, xrYAction, leftYClickPath);
	AddBinding(indexBindings, xrAAction, rightPrimaryClickPath);
	AddBinding(indexBindings, xrBAction, rightSecondaryClickPath);
	AddBinding(indexBindings, xrPoseAction, leftAimPosePath);
	AddBinding(indexBindings, xrPoseAction, rightAimPosePath);
	AddBinding(indexBindings, xrGripPoseAction, leftGripPosePath);
	AddBinding(indexBindings, xrGripPoseAction, rightGripPosePath);
	AddBinding(indexBindings, xrHapticAction, leftHapticPath);
	AddBinding(indexBindings, xrHapticAction, rightHapticPath);
	SuggestBindingsForProfile(xrInstance, indexProfile, "Valve Index", indexBindings);

	std::vector<XrActionSuggestedBinding> wmrBindings;
	AddBinding(wmrBindings, xrSelectAction, leftTriggerValuePath);
	AddBinding(wmrBindings, xrSelectAction, rightTriggerValuePath);
	AddBinding(wmrBindings, xrGripAction, leftSqueezeClickPath);
	AddBinding(wmrBindings, xrGripAction, rightSqueezeClickPath);
	AddBinding(wmrBindings, xrTriggerValueAction, leftTriggerValuePath);
	AddBinding(wmrBindings, xrTriggerValueAction, rightTriggerValuePath);
	// WMR's squeeze is a click, like the Vive's -- 0 or 1, no travel.
	AddBinding(wmrBindings, xrGripValueAction, leftSqueezeClickPath);
	AddBinding(wmrBindings, xrGripValueAction, rightSqueezeClickPath);
	AddBinding(wmrBindings, xrThumbClickAction, leftThumbClickPath);
	AddBinding(wmrBindings, xrThumbClickAction, rightThumbClickPath);
	AddBinding(wmrBindings, xrThumbstickAction, leftThumbstickPath);
	AddBinding(wmrBindings, xrThumbstickAction, rightThumbstickPath);
	AddBinding(wmrBindings, xrMenuAction, leftMenuClickPath);
	AddBinding(wmrBindings, xrMenuAction, rightMenuClickPath);
	AddBinding(wmrBindings, xrPoseAction, leftAimPosePath);
	AddBinding(wmrBindings, xrPoseAction, rightAimPosePath);
	AddBinding(wmrBindings, xrGripPoseAction, leftGripPosePath);
	AddBinding(wmrBindings, xrGripPoseAction, rightGripPosePath);
	AddBinding(wmrBindings, xrHapticAction, leftHapticPath);
	AddBinding(wmrBindings, xrHapticAction, rightHapticPath);
	SuggestBindingsForProfile(xrInstance, wmrProfile, "WMR", wmrBindings);

	XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &xrActionSet;
	xrAttachSessionActionSets(xrSession, &attachInfo);

	// The bound-source report was REMOVED. xrEnumerateBoundSourcesForAction
	// returns nothing on this runtime -- it reported the AIM pose as dead, and
	// the aim pose is what tracks the hands. A diagnostic that answers NONE for
	// a demonstrably working action is worse than no diagnostic: it invites a
	// wrong conclusion with the authority of a log line.

	for (int i = 0; i < 2; ++i)
	{
		XrActionSpaceCreateInfo actionSpaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		actionSpaceInfo.action = xrPoseAction;
		actionSpaceInfo.subactionPath = subactionPaths[i];
		actionSpaceInfo.poseInActionSpace = XrPosef{ {0,0,0,1}, {0,0,0} };
		xrCreateActionSpace(xrSession, &actionSpaceInfo, &xrHandSpaces[i]);

		// Measurement only. Same subaction path, grip pose instead of aim.
		XrActionSpaceCreateInfo gripSpaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		gripSpaceInfo.action = xrGripPoseAction;
		gripSpaceInfo.subactionPath = subactionPaths[i];
		gripSpaceInfo.poseInActionSpace = XrPosef{ {0,0,0,1}, {0,0,0} };
		xrCreateActionSpace(xrSession, &gripSpaceInfo, &xrGripSpaces[i]);
	}

	uint32_t viewCount = 0;
	xrEnumerateViewConfigurationViews(xrInstance, xrSystemId, viewType, 0, &viewCount, nullptr);
	xrViewCount = viewCount;
	xrViewConfigs.resize(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
	xrEnumerateViewConfigurationViews(xrInstance, xrSystemId, viewType, viewCount, &viewCount, xrViewConfigs.data());
	xrViews.resize(viewCount, { XR_TYPE_VIEW });
	xrProjectionViews.resize(viewCount, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });

	sceneWidth = GetMaxRecommendedViewWidth(xrViewConfigs);
	sceneHeight = GetMaxRecommendedViewHeight(xrViewConfigs);
	if (HasMismatchedRecommendedViewExtents(xrViewConfigs))
	{
		for (uint32_t i = 0; i < viewCount; ++i)
		{
			if (developer > 0)
			{
				Printf("OpenXR: view %u recommended extent=%ux%u sampleCount=%u.\n",
					i,
					xrViewConfigs[i].recommendedImageRectWidth,
					xrViewConfigs[i].recommendedImageRectHeight,
					xrViewConfigs[i].recommendedSwapchainSampleCount);
			}
		}
		if (developer > 0)
			Printf("OpenXR: using shared stereo extent=%ux%u for array swapchains.\n",
				sceneWidth, sceneHeight);
	}
	InitializeMultiview();

	isOpenXRReady = true;
	xrInitProbeFrameTime = screen != nullptr ? screen->FrameTime : 0;
	xrInitProbeResult = true;
	return true;
}

bool VKOpenXRDeviceMode::CreateSwapchain() const
{
	if (xrSwapchain != XR_NULL_HANDLE)
		return true;

	if (!isOpenXRReady)
		return false;

	const auto& cfg = xrViewConfigs[0];
	auto* vkfb = dynamic_cast<VulkanRenderDevice*>(screen);
	VkFormat preferredFormat = VK_FORMAT_B8G8R8A8_UNORM;
	if (vkfb && vkfb->GetFramebufferManager() && vkfb->GetFramebufferManager()->SwapChain)
		preferredFormat = vkfb->GetFramebufferManager()->SwapChain->Format().format;

	uint32_t formatCount = 0;
	std::vector<int64_t> runtimeFormats;
	if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(xrSession, 0, &formatCount, nullptr)) && formatCount > 0)
	{
		runtimeFormats.resize(formatCount);
		xrEnumerateSwapchainFormats(xrSession, formatCount, &formatCount, runtimeFormats.data());
	}
	xrSwapchainFormat = SelectSwapchainFormat(runtimeFormats, preferredFormat);
	xrVirtualScreenSwapchainFormat = SelectFlatSwapchainFormat(runtimeFormats, preferredFormat);

	XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swapchainInfo.format = xrSwapchainFormat;
	swapchainInfo.sampleCount = cfg.recommendedSwapchainSampleCount;
	swapchainInfo.width = GetMaxRecommendedViewWidth(xrViewConfigs);
	swapchainInfo.height = GetMaxRecommendedViewHeight(xrViewConfigs);
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = xrViewCount;
	swapchainInfo.mipCount = 1;

	if (XR_FAILED(xrCreateSwapchain(xrSession, &swapchainInfo, &xrSwapchain)))
	{
		return false;
	}

	uint32_t imageCount = 0;
	xrEnumerateSwapchainImages(xrSwapchain, 0, &imageCount, nullptr);
	xrSwapchainImages.resize(imageCount);
	for (auto& image : xrSwapchainImages)
		image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	xrEnumerateSwapchainImages(xrSwapchain, imageCount, &imageCount,
		reinterpret_cast<XrSwapchainImageBaseHeader*>(xrSwapchainImages.data()));

	xrSwapchainTextures.resize(imageCount * xrViewCount);
	for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
	{
		for (uint32_t layer = 0; layer < xrViewCount; ++layer)
		{
			auto& texture = xrSwapchainTextures[imageIndex * xrViewCount + layer];
			texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
			texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			texture.Image = std::make_unique<VulkanImage>(xrVkDevice.get(), xrSwapchainImages[imageIndex].image, nullptr,
				(int)swapchainInfo.width, (int)swapchainInfo.height, 1, (int)xrViewCount);
			texture.View = ImageViewBuilder()
				.Image(texture.Image.get(), (VkFormat)xrSwapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, (int)layer, 1, 1)
				.DebugName("OpenXRSwapchainLayerView")
				.Create(xrVkDevice.get());
		}
	}

	if (!xrPresentTextures.empty() && xrPresentTextures[0].Image != nullptr)
	{
		xrPresentWidth = (uint32_t)xrPresentTextures[0].Image->width;
		xrPresentHeight = (uint32_t)xrPresentTextures[0].Image->height;
	}

	return true;
}

bool VKOpenXRDeviceMode::CreatePresentTextures(VulkanRenderDevice* vkfb) const
{
	if (!vkfb || xrSwapchainFormat == VK_FORMAT_UNDEFINED || xrViewCount == 0)
		return false;

	const uint32_t width = GetMaxRecommendedViewWidth(xrViewConfigs);
	const uint32_t height = GetMaxRecommendedViewHeight(xrViewConfigs);
	if (width == 0 || height == 0)
		return false;

	const bool wantsDedicatedMirrorTextures = ShouldUseDedicatedDesktopMirrorTextures(this);
	const bool hasPresentTextures = xrPresentTextures.size() == xrViewCount &&
		std::all_of(xrPresentTextures.begin(), xrPresentTextures.end(),
			[](const VkTextureImage& texture)
			{
				return texture.Image != nullptr;
			});
	const bool hasDedicatedMirrorTextures = xrMirrorPresentTextures.size() == xrViewCount &&
		std::all_of(xrMirrorPresentTextures.begin(), xrMirrorPresentTextures.end(),
			[](const VkTextureImage& texture)
			{
				return texture.Image != nullptr;
			});

	if (hasPresentTextures &&
		xrPresentWidth == width &&
		xrPresentHeight == height &&
		wantsDedicatedMirrorTextures == hasDedicatedMirrorTextures)
		return true;

	if (!xrPresentTextures.empty())
		xrDeferredPresentTextures.emplace_back(std::move(xrPresentTextures));
	xrPresentTextures.clear();
	if (!xrMirrorPresentTextures.empty())
		xrDeferredMirrorPresentTextures.emplace_back(std::move(xrMirrorPresentTextures));
	xrMirrorPresentTextures.clear();

	for (auto& texture : xrPresentTextures)
		texture.Reset(vkfb);
	for (auto& texture : xrMirrorPresentTextures)
		texture.Reset(vkfb);

	xrPresentTextures.resize(xrViewCount);
	if (wantsDedicatedMirrorTextures)
		xrMirrorPresentTextures.resize(xrViewCount);
	xrPresentWidth = width;
	xrPresentHeight = height;

	const VkFormat presentFormat = (VkFormat)xrSwapchainFormat;
	for (uint32_t i = 0; i < xrViewCount; ++i)
	{
		auto& texture = xrPresentTextures[i];
		texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		texture.Image = ImageBuilder()
			.Format(presentFormat)
			.Size(width, height)
			.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
			.DebugName("OpenXRPresentTexture")
			.Create(vkfb->device.get());
		texture.View = ImageViewBuilder()
			.Image(texture.Image.get(), presentFormat)
			.DebugName("OpenXRPresentTextureView")
			.Create(vkfb->device.get());

		if (wantsDedicatedMirrorTextures)
		{
			auto& mirrorTexture = xrMirrorPresentTextures[i];
			mirrorTexture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
			mirrorTexture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			mirrorTexture.Image = ImageBuilder()
				.Format(presentFormat)
				.Size(width, height)
				.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
				.DebugName("OpenXRMirrorPresentTexture")
				.Create(vkfb->device.get());
			mirrorTexture.View = ImageViewBuilder()
				.Image(mirrorTexture.Image.get(), presentFormat)
				.DebugName("OpenXRMirrorPresentTextureView")
				.Create(vkfb->device.get());
		}
	}

	if (!xrPresentTextures.empty() && xrPresentTextures[0].Image != nullptr)
	{
		xrPresentWidth = (uint32_t)xrPresentTextures[0].Image->width;
		xrPresentHeight = (uint32_t)xrPresentTextures[0].Image->height;
	}

	return true;
}

bool VKOpenXRDeviceMode::CreateVirtualScreenSwapchain(uint32_t width, uint32_t height) const
{
	if (xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrVirtualScreenSwapchainFormat == VK_FORMAT_UNDEFINED || width == 0 || height == 0)
		return false;
	if (xrVirtualScreenSwapchain != XR_NULL_HANDLE &&
		xrVirtualScreenWidth == width &&
		xrVirtualScreenHeight == height &&
		!xrVirtualScreenTextures.empty())
	{
		return true;
	}

	if (!xrVirtualScreenTextures.empty())
		xrDeferredVirtualScreenTextures.emplace_back(std::move(xrVirtualScreenTextures));
	xrVirtualScreenTextures.clear();

	DestroyVirtualScreenSwapchain();

	XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swapchainInfo.format = xrVirtualScreenSwapchainFormat;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = width;
	swapchainInfo.height = height;
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	if (XR_FAILED(xrCreateSwapchain(xrSession, &swapchainInfo, &xrVirtualScreenSwapchain)))
	{
		Printf("OpenXR: failed to create virtual screen swapchain %ux%u.\n", width, height);
		xrVirtualScreenVisible = false;
		return false;
	}

	uint32_t imageCount = 0;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrVirtualScreenSwapchain, 0, &imageCount, nullptr)) || imageCount == 0)
	{
		DestroyVirtualScreenSwapchain();
		xrVirtualScreenVisible = false;
		return false;
	}

	xrVirtualScreenSwapchainImages.resize(imageCount);
	for (auto& image : xrVirtualScreenSwapchainImages)
		image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrVirtualScreenSwapchain, imageCount, &imageCount,
		reinterpret_cast<XrSwapchainImageBaseHeader*>(xrVirtualScreenSwapchainImages.data()))))
	{
		DestroyVirtualScreenSwapchain();
		xrVirtualScreenVisible = false;
		return false;
	}
	xrVirtualScreenSwapchainImages.resize(imageCount);

	xrVirtualScreenTextures.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		auto& texture = xrVirtualScreenTextures[i];
		texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		texture.Image = std::make_unique<VulkanImage>(xrVkDevice.get(), xrVirtualScreenSwapchainImages[i].image, nullptr,
			(int)width, (int)height, 1, 1);
		texture.View = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D)
			.Image(texture.Image.get(), (VkFormat)xrVirtualScreenSwapchainFormat)
			.DebugName("OpenXR.VirtualScreenView")
			.Create(xrVkDevice.get());
	}

	xrVirtualScreenWidth = width;
	xrVirtualScreenHeight = height;
	if (developer > 0)
		Printf("OpenXR: created virtual screen swapchain %ux%u with %u images.\n", width, height, imageCount);
	return true;
}

bool VKOpenXRDeviceMode::CreateVirtualScreenBackdropSwapchain(uint32_t width, uint32_t height) const
{
	if (xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrVirtualScreenSwapchainFormat == VK_FORMAT_UNDEFINED || width == 0 || height == 0)
		return false;
	if (xrVirtualScreenBackdropSwapchain != XR_NULL_HANDLE &&
		xrVirtualScreenWidth == width &&
		xrVirtualScreenHeight == height &&
		!xrVirtualScreenBackdropTextures.empty())
	{
		return true;
	}

	if (!xrVirtualScreenBackdropTextures.empty())
		xrDeferredVirtualScreenBackdropTextures.emplace_back(std::move(xrVirtualScreenBackdropTextures));
	xrVirtualScreenBackdropTextures.clear();

	DestroyVirtualScreenBackdropSwapchain();

	XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swapchainInfo.format = xrVirtualScreenSwapchainFormat;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = width;
	swapchainInfo.height = height;
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	if (XR_FAILED(xrCreateSwapchain(xrSession, &swapchainInfo, &xrVirtualScreenBackdropSwapchain)))
	{
		Printf("OpenXR: failed to create virtual screen backdrop swapchain %ux%u.\n", width, height);
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	uint32_t imageCount = 0;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrVirtualScreenBackdropSwapchain, 0, &imageCount, nullptr)) || imageCount == 0)
	{
		DestroyVirtualScreenBackdropSwapchain();
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	xrVirtualScreenBackdropSwapchainImages.resize(imageCount);
	for (auto& image : xrVirtualScreenBackdropSwapchainImages)
		image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrVirtualScreenBackdropSwapchain, imageCount, &imageCount,
		reinterpret_cast<XrSwapchainImageBaseHeader*>(xrVirtualScreenBackdropSwapchainImages.data()))))
	{
		DestroyVirtualScreenBackdropSwapchain();
		xrVirtualScreenBackdropVisible = false;
		return false;
	}
	xrVirtualScreenBackdropSwapchainImages.resize(imageCount);

	xrVirtualScreenBackdropTextures.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		auto& texture = xrVirtualScreenBackdropTextures[i];
		texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		texture.Image = std::make_unique<VulkanImage>(xrVkDevice.get(), xrVirtualScreenBackdropSwapchainImages[i].image, nullptr,
			(int)width, (int)height, 1, 1);
		texture.View = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D)
			.Image(texture.Image.get(), (VkFormat)xrVirtualScreenSwapchainFormat)
			.DebugName("OpenXR.VirtualScreenBackdropView")
			.Create(xrVkDevice.get());
	}

	xrVirtualScreenBackdropVisible = true;
	return true;
}

bool VKOpenXRDeviceMode::CreateMenuPointerBeamSwapchain() const
{
	constexpr uint32_t beamW = 8;
	constexpr uint32_t beamH = 8;
	if (xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrSwapchainFormat == VK_FORMAT_UNDEFINED)
		return false;
	if (xrMenuPointerBeamSwapchain != XR_NULL_HANDLE && !xrMenuPointerBeamTextures.empty())
		return true;

	if (!xrMenuPointerBeamTextures.empty())
		xrDeferredMenuPointerBeamTextures.emplace_back(std::move(xrMenuPointerBeamTextures));
	xrMenuPointerBeamTextures.clear();

	DestroyMenuPointerBeamSwapchain();

	XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	swapchainInfo.format = xrSwapchainFormat;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = beamW;
	swapchainInfo.height = beamH;
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	if (XR_FAILED(xrCreateSwapchain(xrSession, &swapchainInfo, &xrMenuPointerBeamSwapchain)))
		return false;

	uint32_t imageCount = 0;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrMenuPointerBeamSwapchain, 0, &imageCount, nullptr)) || imageCount == 0)
	{
		DestroyMenuPointerBeamSwapchain();
		return false;
	}

	xrMenuPointerBeamSwapchainImages.resize(imageCount);
	for (auto& image : xrMenuPointerBeamSwapchainImages)
		image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrMenuPointerBeamSwapchain, imageCount, &imageCount,
		reinterpret_cast<XrSwapchainImageBaseHeader*>(xrMenuPointerBeamSwapchainImages.data()))))
	{
		DestroyMenuPointerBeamSwapchain();
		return false;
	}
	xrMenuPointerBeamSwapchainImages.resize(imageCount);

	xrMenuPointerBeamTextures.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		auto& texture = xrMenuPointerBeamTextures[i];
		texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		texture.Image = std::make_unique<VulkanImage>(xrVkDevice.get(), xrMenuPointerBeamSwapchainImages[i].image, nullptr,
			(int)beamW, (int)beamH, 1, 1);
		texture.View = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D)
			.Image(texture.Image.get(), (VkFormat)xrSwapchainFormat)
			.DebugName("OpenXR.MenuPointerBeamView")
			.Create(xrVkDevice.get());
	}
	return true;
}

void VKOpenXRDeviceMode::DestroyVirtualScreenSwapchain() const
{
	xrVirtualScreenVisible = false;
	xrVirtualScreenImageIndex = -1;
	xrVirtualScreenTextures.clear();
	xrVirtualScreenSwapchainImages.clear();
	if (xrVirtualScreenSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(xrVirtualScreenSwapchain);
		xrVirtualScreenSwapchain = XR_NULL_HANDLE;
	}
	xrVirtualScreenWidth = 0;
	xrVirtualScreenHeight = 0;
}

void VKOpenXRDeviceMode::DestroyVirtualScreenBackdropSwapchain() const
{
	xrVirtualScreenBackdropVisible = false;
	xrVirtualScreenBackdropImageIndex = -1;
	xrVirtualScreenBackdropTextures.clear();
	xrVirtualScreenBackdropSwapchainImages.clear();
	if (xrVirtualScreenBackdropSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(xrVirtualScreenBackdropSwapchain);
		xrVirtualScreenBackdropSwapchain = XR_NULL_HANDLE;
	}
}

void VKOpenXRDeviceMode::DestroyMenuPointerBeamSwapchain() const
{
	xrMenuPointerBeamImageIndex = -1;
	xrMenuPointerBeamTextures.clear();
	xrMenuPointerBeamSwapchainImages.clear();
	if (xrMenuPointerBeamSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(xrMenuPointerBeamSwapchain);
		xrMenuPointerBeamSwapchain = XR_NULL_HANDLE;
	}
}

static bool IsNetWaitShellActive()
{
	return VR_IsNetWaitShellActive();
}

void VKOpenXRDeviceMode::DestroyOpenXR() const
{
	StopHaptics();
	DestroyVirtualScreenSwapchain();
	DestroyVirtualScreenBackdropSwapchain();
	DestroyMenuPointerBeamSwapchain();
	if (xrSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(xrSwapchain);
		xrSwapchain = XR_NULL_HANDLE;
	}
	if (xrSpace != XR_NULL_HANDLE)
	{
		xrDestroySpace(xrSpace);
		xrSpace = XR_NULL_HANDLE;
	}
	for (auto& handSpace : xrHandSpaces)
	{
		if (handSpace != XR_NULL_HANDLE)
		{
			xrDestroySpace(handSpace);
			handSpace = XR_NULL_HANDLE;
		}
	}
	for (auto& gripSpace : xrGripSpaces)
	{
		if (gripSpace != XR_NULL_HANDLE)
		{
			xrDestroySpace(gripSpace);
			gripSpace = XR_NULL_HANDLE;
		}
	}
	if (xrActionSet != XR_NULL_HANDLE)
	{
		if (xrHapticAction != XR_NULL_HANDLE)
		{
			xrDestroyAction(xrHapticAction);
			xrHapticAction = XR_NULL_HANDLE;
		}
		xrDestroyActionSet(xrActionSet);
		xrActionSet = XR_NULL_HANDLE;
	}
	if (xrSession != XR_NULL_HANDLE)
	{
		xrDestroySession(xrSession);
		xrSession = XR_NULL_HANDLE;
	}
	if (xrInstance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(xrInstance);
		xrInstance = XR_NULL_HANDLE;
	}
	isOpenXRReady = false;
	isSessionRunning = false;
	isSessionReadyToBegin = false;
	xrSessionState = XR_SESSION_STATE_UNKNOWN;
	xrFrameInProgress = false;
	isSetup = false;
	xrHasEquirectBackdrop = false;
	xrHasFBColorSpace = false;
#ifdef XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME
	xrHasDisplayRefreshRate = false;
	xrLoggedDisplayRefreshRates = false;
	xrRequestedDisplayRefreshRate = 0.0f;
	xrCurrentDisplayRefreshRate = 0.0f;
#endif
	xrMultiviewProbed = false;
	xrMultiviewSupported = false;
	xrMultiviewUsesCoreVulkan = false;
	xrMultiviewMaxViewCount = 0;
	xrMultiviewMaxInstanceIndex = 0;
	xrUsingStageSpace = false;
	xrHasLocalHeightAnchor = false;
	xrLocalHeightAnchor = 0.0f;
	xrPoseAction = XR_NULL_HANDLE;
	xrGripPoseAction = XR_NULL_HANDLE;
	xrSelectAction = XR_NULL_HANDLE;
	xrMenuAction = XR_NULL_HANDLE;
	xrGripAction = XR_NULL_HANDLE;
	xrThumbTouchAction = XR_NULL_HANDLE;
	xrTriggerTouchAction = XR_NULL_HANDLE;
	xrThumbClickAction = XR_NULL_HANDLE;
	xrThumbstickAction = XR_NULL_HANDLE;
	xrAAction = XR_NULL_HANDLE;
	xrBAction = XR_NULL_HANDLE;
	xrXAction = XR_NULL_HANDLE;
	xrYAction = XR_NULL_HANDLE;
	xrPrimaryAction = XR_NULL_HANDLE;
	xrSecondaryAction = XR_NULL_HANDLE;
	xrLeftHandPath = XR_NULL_PATH;
	xrRightHandPath = XR_NULL_PATH;
	xrHandPoseValid[0] = false;
	xrHandPoseValid[1] = false;
	xrLastSelectState[0] = xrLastSelectState[1] = false;
	xrLastMenuState[0] = xrLastMenuState[1] = false;
	xrLastGripState[0] = xrLastGripState[1] = false;
	xrLastHolsterState[0] = xrLastHolsterState[1] = false;
	xrLastThumbClickState[0] = xrLastThumbClickState[1] = false;
	xrLastTrackpadClickState[0] = xrLastTrackpadClickState[1] = false;
	xrLastAState[0] = xrLastAState[1] = false;
	xrLastBState[0] = xrLastBState[1] = false;
	xrLastXState[0] = xrLastXState[1] = false;
	xrLastYState[0] = xrLastYState[1] = false;
	xrLastPrimaryState[0] = xrLastPrimaryState[1] = false;
	xrLastSecondaryState[0] = xrLastSecondaryState[1] = false;
	xrLastThumbstickState[0] = { 0.0f, 0.0f };
	xrLastThumbstickState[1] = { 0.0f, 0.0f };
	xrLastTrackpadState[0] = { 0.0f, 0.0f };
	xrLastTrackpadState[1] = { 0.0f, 0.0f };
	xrLastMenuReturnState = false;
	xrLastMenuBackState = false;
	xrLastMenuBackspaceState = false;
	StopHaptics();
	xrHapticAction = XR_NULL_HANDLE;
	xrHapticDuration[0] = xrHapticDuration[1] = 0.0;
	xrHapticIntensity[0] = xrHapticIntensity[1] = 0.0f;
	xrHapticActive[0] = xrHapticActive[1] = false;
	m_TeleportTarget = 0;
	m_TeleportLocation = DVector3(0, 0, 0);
	xrSwapchainImages.clear();
	xrSwapchainTextures.clear();
	xrPresentTextures.clear();
	xrMirrorPresentTextures.clear();
	xrDeferredPresentTextures.clear();
	xrDeferredMirrorPresentTextures.clear();
	xrSpectatorTextures.clear();	// [SPECTATOR]
	xrSpectatorFrameReady = false;
	xrSpectatorStabilizer.Reset();
	xrViewConfigs.clear();
	xrViews.clear();
	xrProjectionViews.clear();
	xrViewCount = 0;
	xrCurrentImageIndex = -1;
	xrSwapchainFormat = VK_FORMAT_UNDEFINED;
	xrVirtualScreenSwapchainFormat = VK_FORMAT_UNDEFINED;
	xrPresentWidth = 0;
	xrPresentHeight = 0;
	xrFrameState = { XR_TYPE_FRAME_STATE };
	sceneWidth = 0;
	sceneHeight = 0;
	xrVkSubmitFence.reset();
	xrVkCommandBuffer.reset();
	xrVkCommandPool.reset();
	xrVkDevice.reset();
	xrVkInstance.reset();
	xrDeferredVirtualScreenTextures.clear();
	xrDeferredVirtualScreenBackdropTextures.clear();
	xrDeferredMenuPointerBeamTextures.clear();
}

void VKOpenXRDeviceMode::PurgeDeferredOpenXRResources() const
{
	xrDeferredPresentTextures.clear();
	xrDeferredMirrorPresentTextures.clear();
	xrDeferredVirtualScreenTextures.clear();
	xrDeferredVirtualScreenBackdropTextures.clear();
	xrDeferredMenuPointerBeamTextures.clear();
}

VKOpenXRDeviceMode::FrameRenderMode VKOpenXRDeviceMode::DetermineFrameRenderMode() const
{
	if (IsNetWaitShellActive())
	{
		return FrameRenderMode::VirtualScreen;
	}
	const bool forceVirtualScreen = gamestate == GS_LEVEL && menuactive == MENU_Off && (cinemamode || vr_overlayscreen_always);
	// THE MENU-STATE CHECK. IsGameplaySceneActive() is false whenever a menu is
	// open, which is what used to collapse the headset onto the flat virtual
	// screen. With vr_menu_keep_world on, a menu over a running level (not the
	// pause key, not the console, not cinema/always-overlay, which are flat
	// anyway) keeps the stereo world instead. Off: unchanged.
	const bool menuOverWorld = vr_menu_keep_world
		&& gamestate == GS_LEVEL
		&& menuactive != MENU_Off
		&& !paused
		&& ConsoleState == c_up
		&& !cinemamode
		&& !vr_overlayscreen_always;
	if (menuOverWorld)
	{
		return FrameRenderMode::MenuOverWorld;
	}
	return (IsGameplaySceneActive() && !forceVirtualScreen) ? FrameRenderMode::GameplayEyes : FrameRenderMode::VirtualScreen;
}

void VKOpenXRDeviceMode::ApplyFrameRenderMode(FrameRenderMode mode) const
{
	mFrameRenderMode = mode;

	// vr_menu_keep_world: MenuOverWorld is a stereo world frame, so the QzDoom
	// "screen layer" flag (cinema yaw/pitch, playerYaw freeze) stays off like
	// gameplay. The menu quad is driven by ShouldUseScreenLayerForCurrentFrame().
	if (mode == FrameRenderMode::GameplayEyes || mode == FrameRenderMode::MenuOverWorld)
	{
		QzDoom_setUseScreenLayer(false);
	}
	else
	{
		QzDoom_setUseScreenLayer(true);
	}
}

void VKOpenXRDeviceMode::SetUp() const
{
	ApplyFrameRenderMode(DetermineFrameRenderMode());

	super::SetUp();
	PurgeDeferredOpenXRResources();
	struct Guard
	{
		bool& flag;
		bool entered;

		explicit Guard(bool& inFlag) : flag(inFlag), entered(false)
		{
			if (!flag)
			{
				flag = true;
				entered = true;
			}
		}

		explicit operator bool() const
		{
			return entered;
		}

		~Guard()
		{
			if (entered)
				flag = false;
		}
	} guard{ mSetUpInProgress };

	if (!guard)
		return;

	static int setupCallCount = 0;
	setupCallCount++;
	if (!isSetup)
	{
		if (!InitializeOpenXR()) return;
		isSetup = true;
	}

	if (xrSession == XR_NULL_HANDLE) return;
	PollXREvents();
	ApplyFrameRenderMode(DetermineFrameRenderMode());

	UpdateControllerState();

	player_t* player = &players[consoleplayer];
	if (gamestate == GS_LEVEL && resetDoomYaw && player != nullptr && player->mo != nullptr)
	{
		doomYaw = (float)player->mo->Angles.Yaw.Degrees();
		resetDoomYaw = false;
		// vr_menu_keep_world: doomYaw now equals the actor again, so any yaw held
		// back during a MenuOverWorld frame is void.
		xrMenuHeldYawDegrees = 0.0f;
	}
	// vr_menu_keep_world: an open menu normally re-snaps doomYaw to the actor
	// every other frame, which would jitter a live world view. Not while the
	// frame is MenuOverWorld; updateHmdPose keeps doomYaw on the head instead.
	else if (gamestate != GS_LEVEL || (menuactive != MENU_Off && mFrameRenderMode != FrameRenderMode::MenuOverWorld)
		|| ConsoleState == c_down || ConsoleState == c_falling
		|| (player && player->playerstate == PST_DEAD)
		|| (player && player->resetDoomYaw)
		|| paused)
	{
		resetDoomYaw = true;
	}

	if (isSessionRunning)
	{
		updateHmdPose(r_viewpoint);
	}
}

void VKOpenXRDeviceMode::PollXREvents() const
{
	if (xrInstance == XR_NULL_HANDLE)
		return;

	XrEventDataBuffer eventData{ XR_TYPE_EVENT_DATA_BUFFER };
	XrResult result = xrPollEvent(xrInstance, &eventData);
	if (result == XR_EVENT_UNAVAILABLE || !XR_SUCCEEDED(result))
		return;

	if (eventData.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
	{
		auto* ev = reinterpret_cast<XrEventDataSessionStateChanged*>(&eventData);
		if (ev->session == xrSession)
		{
			xrSessionState = ev->state;
			if (ev->state == XR_SESSION_STATE_READY)
			{
				isSessionReadyToBegin = true;
			}
			else if (ev->state == XR_SESSION_STATE_STOPPING)
			{
				StopHaptics();
				xrEndSession(xrSession);
				isSessionRunning = false;
				isSessionReadyToBegin = false;
#ifdef XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME
				xrRequestedDisplayRefreshRate = 0.0f;
				xrCurrentDisplayRefreshRate = 0.0f;
#endif
			}
			else if (ev->state == XR_SESSION_STATE_LOSS_PENDING || ev->state == XR_SESSION_STATE_EXITING)
			{
				StopHaptics();
				DestroyOpenXR();
			}
			else
			{
			}
		}
	}
}

void VKOpenXRDeviceMode::updateHmdPose(FRenderViewpoint& vp) const
{
	if (xrViews.empty()) return;

	XrVector3f pos = { 0, 0, 0 };
	for (uint32_t i = 0; i < xrViewCount; ++i)
	{
		pos.x += xrViews[i].pose.position.x;
		pos.y += xrViews[i].pose.position.y;
		pos.z += xrViews[i].pose.position.z;
	}
	pos.x /= xrViewCount;
	pos.y /= xrViewCount;
	pos.z /= xrViewCount;

	hmdPosition[0] = pos.x;
	hmdPosition[1] = pos.y;
	hmdPosition[2] = pos.z;

	float p, y, r;
	QuaternionToEuler(GetCenteredViewOrientation(xrViews), p, y, r);

	hmdorientation[0] = -p;
	hmdorientation[1] = -y;
	hmdorientation[2] = -r;
	VR_SetHMDPosition(hmdPosition[0], hmdPosition[1], hmdPosition[2]);
	VR_SetHMDOrientation(hmdorientation[0], hmdorientation[1], hmdorientation[2]);
	if (VR_UseCinematicScreenLayer())
	{
		cinemamodePitch = hmdorientation[0];
	}
	positional_movementSideways = 0.0f;
	positional_movementForward = 0.0f;

	if (gamestate == GS_LEVEL && menuactive == MENU_Off && !paused)
	{
		const float rotation = GetViewpointYaw() - hmdorientation[1];
		DVector2 rotated = DVector2(positionDeltaThisFrame[0], positionDeltaThisFrame[2]).Rotated(DAngle::fromDeg(-rotation));
		positional_movementSideways = rotated.Y;
		positional_movementForward = rotated.X;
		if (multiplayer)
		{
			VR_AddMultiplayerRoomscaleWorldOffset(
				positional_movementSideways * vr_vunits_per_meter,
				positional_movementForward * vr_vunits_per_meter);
		}
	}

	if (!xrUsingStageSpace && !xrHasLocalHeightAnchor)
	{
		const player_t* player = &players[consoleplayer];
		if (player != nullptr && player->mo != nullptr)
		{
			xrLocalHeightAnchor = GetDoomPlayerHeightWithoutCrouch(player) - GetRawHmdHeightInMapUnit();
			xrHasLocalHeightAnchor = true;
		}
	}

	// vr_menu_keep_world: this early-out is what froze head tracking while a
	// menu was open (no yaw into doomYaw, no HMD pitch/roll on the viewpoint).
	// A MenuOverWorld frame goes on through, like gameplay.
	const bool menuOverWorld = mFrameRenderMode == FrameRenderMode::MenuOverWorld;
	if (gamestate != GS_LEVEL || (menuactive != MENU_Off && !menuOverWorld) || r_viewpoint.camera == nullptr || r_viewpoint.ViewLevel == nullptr)
		return;

	static float previousHmdYaw = 0;
	static bool havePreviousYaw = false;
	static float previousCinemaSnapTurn = 0.0f;
	static bool wasLockedToScreenLayerLastFrame = false;
	const float currentHmdYaw = hmdorientation[1] + snapTurn;
	// MenuOverWorld reports the screen layer (for the menu quad) but its view is
	// the free stereo head view, not the cinema lock.
	const bool lockGameplayViewToScreenLayer = ShouldUseScreenLayerForCurrentFrame() && !menuOverWorld;
	player_t* player = &players[consoleplayer];
	if (!havePreviousYaw)
	{
		previousHmdYaw = currentHmdYaw;
		doomYaw = r_viewpoint.Angles.Yaw.Degrees();
		havePreviousYaw = true;
	}
	if (resetDoomYaw || (player && player->resetDoomYaw))
	{
		previousHmdYaw = currentHmdYaw;
	}
	float hmdYawDeltaDegrees = currentHmdYaw - previousHmdYaw;
	float cinemaTurnDeltaDegrees = 0.0f;
	if (lockGameplayViewToScreenLayer)
	{
		if (!wasLockedToScreenLayerLastFrame)
		{
			previousCinemaSnapTurn = snapTurn;
			cinemamodeYaw = (float)r_viewpoint.Angles.Yaw.Degrees();
		}
		cinemaTurnDeltaDegrees = ShortestAngleDeltaDeg(snapTurn, previousCinemaSnapTurn);
		previousCinemaSnapTurn = snapTurn;
		wasLockedToScreenLayerLastFrame = true;
	}
	else
	{
		wasLockedToScreenLayerLastFrame = false;
	}
	if (menuOverWorld)
	{
		// vr_menu_keep_world: turn the rendered view only. A menu-paused playsim
		// would drop this turn from the ticcmd, leaving the actor behind doomYaw;
		// hold it and pay it in one go when the menu closes (below).
		xrMenuHeldYawDegrees += hmdYawDeltaDegrees;
	}
	else if (!lockGameplayViewToScreenLayer)
	{
		vrApplyingHmdYaw = true;
		G_AddViewAngle(mAngleFromRadians((float)DEG2RAD(-hmdYawDeltaDegrees)));
		vrApplyingHmdYaw = false;
	}
	else if (cinemaTurnDeltaDegrees != 0.0f)
	{
		vrApplyingHmdYaw = true;
		G_AddViewAngle(mAngleFromRadians((float)DEG2RAD(-cinemaTurnDeltaDegrees)));
		vrApplyingHmdYaw = false;
	}
	if (!menuOverWorld && xrMenuHeldYawDegrees != 0.0f)
	{
		// vr_menu_keep_world: first frame out of the menu. Same single catch-up
		// turn the old path gave (previousHmdYaw was frozen during the menu), so
		// the actor lands on the heading the view already has. Local input only.
		vrApplyingHmdYaw = true;
		G_AddViewAngle(mAngleFromRadians((float)DEG2RAD(-xrMenuHeldYawDegrees)));
		vrApplyingHmdYaw = false;
		xrMenuHeldYawDegrees = 0.0f;
	}
	previousHmdYaw = currentHmdYaw;

	// vr_menu_keep_world: MenuOverWorld also gets the live HMD yaw/pitch/roll.
	if (gamestate == GS_LEVEL && (menuactive == MENU_Off || menuOverWorld))
	{
		if (!lockGameplayViewToScreenLayer)
		{
			doomYaw += hmdYawDeltaDegrees;
			vp.HWAngles.Roll = FAngle::fromDeg(-r);
			vp.HWAngles.Pitch = FAngle::fromDeg(-p);
		}
		else
		{
			doomYaw += cinemaTurnDeltaDegrees;
			cinemamodeYaw = doomYaw;
			vp.HWAngles.Roll = FAngle::fromDeg(0.0f);
			vp.HWAngles.Pitch = FAngle::fromDeg(hmdorientation[0]);
		}

		double viewYaw = GetViewpointYaw();
		while (viewYaw <= -180.0) viewYaw += 360.0;
		while (viewYaw > 180.0) viewYaw -= 360.0;
		vp.Angles.Yaw = DAngle::fromDeg(viewYaw);
	}
}

void VKOpenXRDeviceMode::UpdateControllerState() const
{
	if (xrSession == XR_NULL_HANDLE || xrSpace == XR_NULL_HANDLE || xrActionSet == XR_NULL_HANDLE)
		return;
	if (!isSessionRunning)
		return;

	if (xrFrameState.predictedDisplayTime == 0)
		return;

	XrActiveActionSet activeActionSet{};
	activeActionSet.actionSet = xrActionSet;

	XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeActionSet;
	const XrResult syncResult = xrSyncActions(xrSession, &syncInfo);
	if (XR_FAILED(syncResult))
	{
		Printf("OpenXR: xrSyncActions failed result=%d\n", (int)syncResult);
	}

	const bool menuMode = menuactive != MENU_Off;
	const bool gameplayMode = gamestate == GS_LEVEL && !menuMode && !paused;
	const int mainHand = GetMainHandIndex();
	const int offHand = GetOffHandIndex();
	const int movementHand = *vr_switch_sticks ? mainHand : offHand;
	const int turnHand = *vr_switch_sticks ? offHand : mainHand;
	const bool useTrackpad = IsCurrentInteractionProfile(xrInstance, xrSession, xrRightHandPath, "vive_controller");

	OpenXRHandInputState handInput[2];
	for (int hand = 0; hand < 2; ++hand)
	{
		OpenXRHandInputState& input = handInput[hand];
		const XrPath handPath = (hand == 0) ? xrLeftHandPath : xrRightHandPath;
		input.select = GetActionBoolean(xrSession, xrSelectAction, handPath);
		input.grip = GetActionBoolean(xrSession, xrGripAction, handPath);
		input.triggerValue = GetActionFloat(xrSession, xrTriggerValueAction, handPath);
		input.gripValue = GetActionFloat(xrSession, xrGripValueAction, handPath);
		input.thumbClick = GetActionBoolean(xrSession, xrThumbClickAction, handPath);
		input.menu = GetActionBoolean(xrSession, xrMenuAction, handPath);
		input.a = GetActionBoolean(xrSession, xrAAction, handPath);
		input.b = GetActionBoolean(xrSession, xrBAction, handPath);
		input.x = GetActionBoolean(xrSession, xrXAction, handPath);
		input.y = GetActionBoolean(xrSession, xrYAction, handPath);
		input.trackpad = GetActionVector2f(xrSession, xrTrackpadAction, handPath);
		input.thumbstick = GetActionVector2f(xrSession, xrThumbstickAction, handPath);
		input.thumbTouch = GetActionBoolean(xrSession, xrThumbTouchAction, handPath);
		input.triggerTouch = GetActionBoolean(xrSession, xrTriggerTouchAction, handPath);

		xrFingerTouch[hand] = (input.thumbTouch ? FINGERTOUCH_THUMB : 0)
		                    | (input.triggerTouch ? FINGERTOUCH_INDEX : 0);
	}

	static bool lastMenuMode = false;
	const bool menuModeChanged = (lastMenuMode != menuMode);
	lastMenuMode = menuMode;

	auto syncHandState = [&](int hand)
	{
		const OpenXRHandInputState& input = handInput[hand];
		xrLastSelectState[hand] = input.select;
		xrLastMenuState[hand] = input.menu;
		xrLastGripState[hand] = input.grip;
		xrLastThumbClickState[hand] = input.thumbClick;
		xrLastTrackpadClickState[hand] = input.thumbClick;
		xrLastAState[hand] = input.a;
		xrLastBState[hand] = input.b;
		xrLastXState[hand] = input.x;
		xrLastYState[hand] = input.y;
		xrLastPrimaryState[hand] = input.a;
		xrLastSecondaryState[hand] = input.b;
		xrLastThumbstickState[hand] = input.thumbstick;
		xrLastTrackpadState[hand] = input.trackpad;
	};

	if (menuModeChanged)
	{
		syncHandState(0);
		syncHandState(1);
	}

	// The shift layer stays live while a holster store is merely ARMED -- that
	// is the whole tap-vs-combo idea. Pressing grip at your hip does not cost
	// you the modifier; pressing another button just converts the press into a
	// combo and cancels the pending store.
	const bool dominantGripModifierNew = *vr_secondary_button_mappings
		&& (xrGripContext[mainHand] == GRIPCTX_Modifier || xrGripContext[mainHand] == GRIPCTX_Holster);
	const bool dominantGripModifierOld = *vr_secondary_button_mappings && xrLastGripState[mainHand];
	static float analogTurnRateDegPerSec = 0.0f;
	static uint64_t lastAnalogTurnTime = 0;
	const XrVector2f rightTurnState = useTrackpad
		? handInput[turnHand].trackpad
		: handInput[turnHand].thumbstick;

		// [BB] Script can claim the stick. While a mod's in-world selector is
		// open the same thumbstick is picking an item, and snap turn firing
		// underneath it spins the player mid-choice -- which is the single most
		// disorienting thing a VR menu can do.
		//
		// The latches are reset rather than merely skipped: a stick held over
		// during suppression must not fire the instant it lifts.
		// VRWheel_ShouldSuppressStickMove covers the ENGINE's own wheel, which
		// turns out to have had this bug too: nothing in this file ever mentioned
		// VRWheel, so its stick-selection mode has always been snap-turning the
		// player while they picked a weapon with the same thumb.
		//
		// That function is deliberately narrow -- true only while a wheel is
		// actually being driven by the stick -- so pointer-mode wheels keep their
		// turning, which is right: the stick is free in that mode and there is
		// nothing to fight.
		if (gameplayMode && (VR_IsScriptInputSuppressed() || VRWheel_ShouldSuppressStickMove()))
		{
			analogTurnRateDegPerSec = 0.0f;
			lastAnalogTurnTime = 0;
		}
		else if (gameplayMode)
		{
			static bool turnRightLatched[2] = { false, false };
			static bool turnLeftLatched[2] = { false, false };

			if (dominantGripModifierNew)
			{
				analogTurnRateDegPerSec = 0.0f;
				lastAnalogTurnTime = 0;
				turnRightLatched[mainHand] = false;
				turnLeftLatched[mainHand] = false;
			}
			else
			{
				const float turnX = rightTurnState.x;
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

				if (*vr_snapTurn <= 10.0f)
				{
					snapTurn += VR_ApplyAnalogSmoothTurn(turnX, 210.0f, deltaSeconds, VR_GetAnalogTurnResponseScale(*vr_snapTurn), analogTurnRateDegPerSec);
					if (fabsf(turnX) > 0.05f)
					{
						turnRightLatched[mainHand] = false;
						turnLeftLatched[mainHand] = false;
					}
					else
					{
						analogTurnRateDegPerSec = 0.0f;
					}
				}
				else
				{
					analogTurnRateDegPerSec = 0.0f;
					if (turnX > 0.60f)
					{
						if (!turnRightLatched[mainHand])
						{
							snapTurn -= *vr_snapTurn;
							resetDoomYaw = true;
							turnRightLatched[mainHand] = true;
						}
					}
					else if (turnX < 0.40f)
					{
						turnRightLatched[mainHand] = false;
					}

					if (turnX < -0.60f)
					{
						if (!turnLeftLatched[mainHand])
						{
							snapTurn += *vr_snapTurn;
							resetDoomYaw = true;
							turnLeftLatched[mainHand] = true;
						}
					}
					else if (turnX > -0.40f)
					{
						turnLeftLatched[mainHand] = false;
					}
				}
			}
		}
		else
		{
			analogTurnRateDegPerSec = 0.0f;
			lastAnalogTurnTime = 0;
		}

	auto updateHandPose = [&](int hand, float* offset, float* angles, float* linVelOut, float* angVelOut, bool* velValidOut)
	{
		*velValidOut = false;
		linVelOut[0] = linVelOut[1] = linVelOut[2] = 0.0f;
		angVelOut[0] = angVelOut[1] = angVelOut[2] = 0.0f;

		// EVERY REFUSAL SAYS WHY. All three of grab's refusal paths in the reverted
		// physics module were a bare return, which is how "why is there no gun in
		// my off hand" became unanswerable from a log. Rate limited so a permanent
		// failure does not drown the file. A lambda, not a macro -- a multi-line
		// macro inside an existing lambda is asking for trouble for no benefit.
		static uint64_t lastRefusal[2] = { 0, 0 };
		auto refuse = [&](const char *why) -> bool
		{
			const uint64_t n = I_nsTime();
			if (n - lastRefusal[hand] > 2000000000ull)
			{
				Printf("[A0] hand %d pose REFUSED: %s\n", hand, why);
				lastRefusal[hand] = n;
			}
			xrHandPoseValid[hand] = false;
			return false;
		};

		if (xrHandSpaces[hand] == XR_NULL_HANDLE)
			return refuse("action space is XR_NULL_HANDLE -- the pose action never bound");

		// Chained via .next: one call, both pose AND velocity, sensor-fused by
		// the runtime -- not two backward-difference samples 28ms apart. Every
		// OpenXR runtime that reports controller pose at all is required to
		// support velocity on the same locate call; this is not an extension.
		XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY };
		XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION, &velocity };
		if (XR_FAILED(xrLocateSpace(xrHandSpaces[hand], xrSpace, xrFrameState.predictedDisplayTime, &location)))
			return refuse("xrLocateSpace failed -- runtime could not place this hand");

		// REPORT WHAT THE RUNTIME ACTUALLY SAID, NOT ONE GUESS AT WHY.
		//
		// This used to print "tracking lost" for every clear VALID bit, which is
		// only one of the reasons those bits go clear and is the alarming one.
		// The common causes are mundane: the session is not FOCUSED because a
		// system overlay or dashboard is up, the headset is off your head, or the
		// app has not finished coming up yet. A controller sitting on the desk
		// tracking perfectly well produces the identical message.
		//
		// It cost a real debugging session -- the log insisted the controllers
		// were not tracking while the same log showed grip transitions firing.
		// VALID and TRACKED are also different questions and both are worth
		// having: VALID clear means no usable pose at all, while VALID set with
		// TRACKED clear means the runtime is extrapolating from a lost sensor and
		// the pose is stale but usable.
		const bool posValid   = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
		const bool rotValid   = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
		const bool posTracked = (location.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
		const bool rotTracked = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;

		const bool valid = posValid && rotValid;
		xrHandPoseValid[hand] = valid;
		if (!valid)
		{
			char why[384];
			snprintf(why, sizeof(why),
				"no usable pose this frame -- position %s, orientation %s "
				"(tracked: pos %s, rot %s); session is %s%s",
				posValid ? "valid" : "INVALID",
				rotValid ? "valid" : "INVALID",
				posTracked ? "yes" : "no",
				rotTracked ? "yes" : "no",
				XrSessionStateName(xrSessionState),
				(xrSessionState != XR_SESSION_STATE_FOCUSED)
					? ". NOT FOCUSED -- a dashboard/overlay is up, the headset is off, "
					  "or the app is still starting. This is normal and is NOT a hardware fault."
					: ". Focused, so this one is worth looking at.");
			return refuse(why);
		}

		xrHandPoses[hand] = location.pose;

		offset[0] = location.pose.position.x - hmdPosition[0];
		offset[1] = location.pose.position.y - hmdPosition[1];
		offset[2] = location.pose.position.z - hmdPosition[2];

		const float yawRotation = GetViewpointYaw() - hmdorientation[1];
		DVector2 rotated = DVector2(offset[0], offset[2]).Rotated(-yawRotation);
		offset[0] = rotated.Y;
		offset[2] = rotated.X;

		// SAME TRANSFORM AS POSITION, MINUS THE TRANSLATION. Velocity is a
		// free vector, not a point -- it has no origin to subtract (hence no
		// "- hmdPosition" here, unlike offset[] above), but it still lives in
		// the runtime's tracking-space axes and still needs the identical
		// yaw rotation to land in the same local frame offset[] just landed
		// in. Angular velocity is rotated the same way for the same reason:
		// it is expressed about tracking-space axes, and the axis a
		// component points along has to agree with which local axis it ends
		// up labeled as.
		const bool linVelValid = (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
		const bool angVelValid = (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
		*velValidOut = linVelValid;   // angular riding along is a bonus; linear is what a throw needs at minimum

		if (linVelValid)
		{
			float lv[3] = { velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z };
			DVector2 lvRotated = DVector2(lv[0], lv[2]).Rotated(-yawRotation);
			linVelOut[0] = (float)lvRotated.Y;
			linVelOut[1] = lv[1];
			linVelOut[2] = (float)lvRotated.X;
		}
		if (angVelValid)
		{
			float av[3] = { velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z };
			DVector2 avRotated = DVector2(av[0], av[2]).Rotated(-yawRotation);
			angVelOut[0] = (float)avRotated.Y;
			angVelOut[1] = av[1];
			angVelOut[2] = (float)avRotated.X;
		}

		// Apply vr_weaponRotate as a pure pitch-domain adjustment
		const XrVector3f euler = OpenVREulerAnglesFromQuaternion(location.pose.orientation);
		angles[YAW] = (float)(euler.x * (180.0 / M_PI));
		// Match OpenVR pitch-adjust direction: negative vr_weaponRotate should tilt the weapon downward
		constexpr float openxrWeaponPitchBiasDeg = -40.0f;
		angles[PITCH] = -(float)(euler.y * (180.0 / M_PI)) - (vr_weaponRotate * 2.0f) + openxrWeaponPitchBiasDeg;
		// Apply extra 180-degree roll flip to match the expected OpenVR-facing baseline
		angles[ROLL] = (float)NormalizeAngle(-(float)(euler.z * (180.0 / M_PI)));
		return true;
	};

	const bool mainHandValid = updateHandPose(mainHand, weaponoffset, weaponangles, weaponLinearVel, weaponAngularVel, &weaponVelValid);
	const bool offHandValid = updateHandPose(offHand, offhandoffset, offhandangles, offhandLinearVel, offhandAngularVel, &offhandVelValid);

	// ======================================================================
	// A0 HARNESS -- measure before changing anything.
	//
	// This exists because twelve sessions shipped a fix and then went looking in
	// a headset for a feeling. A feeling cannot distinguish a constant offset
	// from a growing one, and those two have completely different causes and
	// completely different fixes. This turns that into a number.
	//
	// THE MEASUREMENT THAT MATTERS is the Euler round-trip residual. The runtime
	// hands us a quaternion. The engine decomposes it to yaw/pitch/roll and every
	// consumer downstream rebuilds from those three numbers. If that round trip
	// is lossless the residual is zero at every orientation. If it is NOT, the
	// residual is zero at rest and GROWS the further you rotate away -- which in
	// a headset reads as the model sliding over the surface of a sphere, is
	// indistinguishable from a bad export, and cannot be cancelled by any
	// constant. Sorting those two apart is the whole point of A0.
	//
	// Toggle: vr_posestats (menu). Off by default; costs nothing when off.
	// ======================================================================
	if (vr_posestats)
	{
		static uint64_t a0LastReport = 0;
		static int      a0Frames     = 0;
		static double   a0ResidSum[4]   = {0,0,0,0};   // binned by rotation magnitude
		static double   a0ResidMax[4]   = {0,0,0,0};
		static int      a0ResidCount[4] = {0,0,0,0};
		static double   a0LatSum = 0.0, a0LatMax = 0.0;
		static int      a0LatCount = 0;

		const uint64_t nowNs = I_nsTime();

		for (int h = 0; h < 2; ++h)
		{
			if (!xrHandPoseValid[h])
				continue;

			const XrQuaternionf& raw = xrHandPoses[h].orientation;

			// Decompose exactly as the engine does, then rebuild from those three
			// numbers using the inverse of the same permutation. Any difference is
			// information the round trip destroyed.
			const XrVector3f e = OpenVREulerAnglesFromQuaternion(raw);
			const double yaw = e.x, pitch = e.y, roll = e.z;

			const double cr = cos(roll * 0.5),  sr = sin(roll * 0.5);
			const double cp = cos(pitch * 0.5), sp = sin(pitch * 0.5);
			const double cy = cos(yaw * 0.5),   sy = sin(yaw * 0.5);

			// q0..q3 in the SAME slots OpenVREulerAnglesFromQuaternion reads them
			// from: q0=w, q1=z, q2=x, q3=y.
			const double q0 = cr * cp * cy + sr * sp * sy;
			const double q1 = sr * cp * cy - cr * sp * sy;
			const double q2 = cr * sp * cy + sr * cp * sy;
			const double q3 = cr * cp * sy - sr * sp * cy;

			XrQuaternionf back;
			back.w = (float)q0; back.z = (float)q1; back.x = (float)q2; back.y = (float)q3;

			double dot = fabs((double)raw.w * back.w + (double)raw.x * back.x +
			                  (double)raw.y * back.y + (double)raw.z * back.z);
			if (dot > 1.0) dot = 1.0;
			const double residualDeg = 2.0 * acos(dot) * (180.0 / M_PI);

			// How far from identity is this orientation? The bin is what separates
			// "constant offset" from "grows with rotation".
			double wAbs = fabs((double)raw.w);
			if (wAbs > 1.0) wAbs = 1.0;
			const double totalDeg = 2.0 * acos(wAbs) * (180.0 / M_PI);

			int bin = 0;
			if (totalDeg >= 30.0)  bin = 1;
			if (totalDeg >= 60.0)  bin = 2;
			if (totalDeg >= 120.0) bin = 3;

			a0ResidSum[bin] += residualDeg;
			if (residualDeg > a0ResidMax[bin]) a0ResidMax[bin] = residualDeg;
			a0ResidCount[bin]++;
		}

		// Pose-sample to now. predictedDisplayTime is in the runtime's clock, so
		// this is only meaningful as a RELATIVE figure across frames -- which is
		// all that is wanted: it is the shape of the number that matters, not its
		// absolute value.
		{
			static uint64_t a0PrevFrame = 0;
			if (a0PrevFrame != 0)
			{
				const double dtMs = double(nowNs - a0PrevFrame) / 1.0e6;
				a0LatSum += dtMs;
				if (dtMs > a0LatMax) a0LatMax = dtMs;
				a0LatCount++;
			}
			a0PrevFrame = nowNs;
		}

		++a0Frames;

		if (a0LastReport == 0) a0LastReport = nowNs;
		if (nowNs - a0LastReport > 3000000000ull)   // every 3 seconds
		{
			static const char *binName[4] = { "  0-30", " 30-60", " 60-120", "120+  " };
			Printf("[A0] ---- %d frames ----\n", a0Frames);
			for (int b = 0; b < 4; ++b)
			{
				if (!a0ResidCount[b]) continue;
				Printf("[A0] rot %s deg : euler round-trip residual mean %.4f max %.4f  (n=%d)\n",
					binName[b], a0ResidSum[b] / a0ResidCount[b], a0ResidMax[b], a0ResidCount[b]);
			}
			if (a0LatCount)
				Printf("[A0] frame interval mean %.2f ms  max %.2f ms  (n=%d)\n",
					a0LatSum / a0LatCount, a0LatMax, a0LatCount);
			Printf("[A0] READ IT LIKE THIS: residual flat across the bins = a constant\n"
			       "[A0] offset, cancellable by one number. Residual RISING with the bin =\n"
			       "[A0] the Euler round trip is lossy and no constant can ever fix it.\n");

			for (int b = 0; b < 4; ++b) { a0ResidSum[b]=0; a0ResidMax[b]=0; a0ResidCount[b]=0; }
			a0LatSum = 0; a0LatMax = 0; a0LatCount = 0;
			a0Frames = 0;
			a0LastReport = nowNs;
		}
	}

	if (mainHandValid && offHandValid)
	{
		const float dx = xrHandPoses[mainHand].position.x - xrHandPoses[offHand].position.x;
		const float dy = xrHandPoses[mainHand].position.y - xrHandPoses[offHand].position.y;
		const float dz = xrHandPoses[mainHand].position.z - xrHandPoses[offHand].position.z;
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		const bool offhandGrip = handInput[offHand].grip;

		// ---- GRIP ARBITER ----------------------------------------------
		// One resolution per hand per frame, in priority order, before any
		// consumer runs. Everything downstream reads xrGripContext and never
		// handInput[].grip directly -- that is the whole point.
		//
		// Priority: Holster > Stabilize > Modifier > Plain.
		// A holster wins because it is a small, deliberate, spatial target;
		// stabilize is a loose proximity test that false-positives near the
		// body, which is exactly where holsters live.
		AActor* consolePawn = players[consoleplayer].mo;

		// Per-weapon stabilize reach (ZScript-synced, see AActor::StabilizeReach)
		// in place of a fixed 0.50m for every weapon. 0 = use the global
		// fallback cvar, negative = this weapon does not stabilize at all.
		double reachInches = consolePawn ? consolePawn->StabilizeReach : 0.0;
		if (reachInches == 0.0) reachInches = vr_stabilize_distance_inches;
		const float stabilizeRangeMeters = (reachInches > 0.0) ? (float)(reachInches * 0.0254) : -1.0f;

		// Proximity stabilize: the off hand within reach of the main hand is
		// ASSUMED to be supporting the weapon, and the weapon is repositioned
		// to suit.
		//
		// Retired. It was the only option while hands were invisible -- there
		// was nothing in the world to grab, so nearness was the only signal
		// available -- and it is a guess that cannot tell a hand supporting the
		// weapon from a hand that drifted close. Reloading brings the hands
		// together, so it fired constantly during exactly the gesture that
		// least wanted it.
		//
		// It now requires a claim without exception: the off hand must actually
		// be on the grip or the forend. vr_two_handed_weapons additionally
		// defaults off, so even a claimed hold does not move the weapon unless
		// the repositioning is explicitly asked for.
		const bool stabilizeGeometryOk = false;
		(void)stabilizeRangeMeters;
		(void)vr_stabilize_requires_grab;

		for (int h = 0; h < 2; ++h)
		{
			const bool held = handInput[h].grip;
			const bool isMain = (h == mainHand);
			const bool holsterHere = consolePawn
				&& (isMain ? consolePawn->HolsterClaimMain : consolePawn->HolsterClaimOff);

			// What script says this hand is closed on. Anything non-zero means
			// the hand is busy with a physical object, which is what stands
			// stabilize down -- see GripClaim* in actor.h.
			const int claimed = consolePawn
				? (isMain ? consolePawn->GripClaimMain : consolePawn->GripClaimOff) : 0;

			// A hand on the forend or a foregrip is still SUPPORTING the
			// weapon, so it keeps stabilize -- that is what two-handed hold
			// is, and claiming it means the player deliberately grabbed the
			// gun there rather than the engine inferring it from two
			// controllers happening to be near each other.
			//
			// Every other subject -- a magazine, a shell, the slide -- is a
			// hand doing something ELSE, and must stand stabilize down.
			// Reloading is precisely the gesture that brings the hands
			// together, so without this every reload reads as gripping the
			// weapon two-handed.
			const bool supporting = (claimed == GRIPSUBJ_Forend || claimed == GRIPSUBJ_Foregrip);
			// Either already holding something, or pointed at something it could
			// take. The second half is the claim script writes -- see
			// GrabClaimMain/Off in actor.h.
			const bool grabHere = consolePawn
				&& (isMain ? consolePawn->GrabClaimMain : consolePawn->GrabClaimOff);
			const bool hardpointHere = consolePawn
				&& (isMain ? consolePawn->HardpointClaimMain : consolePawn->HardpointClaimOff);
			const bool objectHere = ((claimed > 0) && !supporting) || grabHere;

			// ---- tap vs combo -------------------------------------------
			// Grip alone does nothing as a modifier -- the shift layer only
			// matters once a SECOND button joins it. So grip can serve both
			// jobs on both hands, as long as the decision waits for release
			// instead of being guessed at press:
			//
			//   press inside a holster        -> arm a pending store
			//   any other button joins        -> it was a combo, cancel
			//   release, still inside, clean  -> store
			//
			// The modifier stays live the entire time, so nothing is lost.
			// This is why the main hand is not special-cased: it keeps its
			// whole grip+button roster AND can holster.
			const bool wasHeld = xrLastGripState[h];
			// SAME HAND ONLY, and no stick axes. A combo means grip plus another
			// button ON THIS CONTROLLER -- that is what the shift layer is made
			// of. Locomotion lives on the other hand's stick and must never
			// cancel a store: running while holstering is the normal case, not
			// an edge case. Thumb CLICK counts (it is a button in the roster),
			// thumb DEFLECTION does not.
			const bool otherButtonDown = handInput[h].select || handInput[h].thumbClick
				|| handInput[h].a || handInput[h].b || handInput[h].x || handInput[h].y;

			if (held && !wasHeld)
			{
				// rising edge: only a press that STARTS in a holster (or a
				// hardpoint -- same tap-completion path, see below) counts,
				// so drifting a held hand into one never stores by accident
				xrHolsterArmed[h] = (holsterHere || hardpointHere) && *vr_holster_use_grip;
				xrHolsterCombo[h] = false;
			}
			else if (held)
			{
				if (otherButtonDown)
					xrHolsterCombo[h] = true;
			}
			else if (!held && wasHeld)
			{
				// falling edge: a clean tap that began and ended in a holster
				// OR a hardpoint. hardpointHere was already being classified
				// into GRIPCTX_Hardpoint/GRIPSUBJ_Holster above (pose, debug
				// print) but never actually completed a tap here -- so a
				// hardpoint-only claim could never fire xrHolsterFire, and
				// the F13/F14 store/draw pulse (and RS_HardPoints' own
				// edit-mode drag, which rides the identical netevent) never
				// ran for a hand that was ONLY inside a hardpoint's radius.
				if (xrHolsterArmed[h] && !xrHolsterCombo[h] && (holsterHere || hardpointHere))
					xrHolsterFire[h] = true;

				// A clean tap ANYWHERE (no other button joined it) is also
				// what makes the dominant grip bindable at all -- see the
				// suppressGripButton branch. Without this it is swallowed
				// wholesale by the shift layer and never reaches the bind
				// system, which is why it could not be captured in Customize
				// Controls.
				if (!xrHolsterCombo[h])
					xrGripTapFire[h] = true;

				xrHolsterArmed[h] = false;
				xrHolsterCombo[h] = false;
			}

			int ctx = GRIPCTX_None;
			if (!held)
			{
				ctx = GRIPCTX_None;
			}
			else if (xrHolsterArmed[h] && !xrHolsterCombo[h])
			{
				// Armed for a store, but the modifier is deliberately NOT
				// suppressed while armed -- see dominantGripModifierNew.
				ctx = GRIPCTX_Holster;
			}
			else if (hardpointHere)
			{
				// Under a weapon holster, over everything else. Both are small
				// deliberate targets on your body and in practice they do not
				// overlap; when they somehow do, the holster is the one holding
				// a gun and wins.
				ctx = GRIPCTX_Hardpoint;
			}
			else if (objectHere)
			{
				// ABOVE THE MODIFIER, and that ordering is the point.
				//
				// It used to sit below, which meant the dominant grip was the
				// shift layer whenever it was held -- including when the hand
				// was closed on a magazine. A fist round an object is not a
				// shift key, and the modifier layer stands analog turning down,
				// so reaching for anything stopped you turning.
				//
				// The modifier is not lost: it is what this grip still means
				// whenever the hand has nothing to take and nothing in it, which
				// is nearly always. Holster still outranks both -- that is a
				// small deliberate spatial target and beats a general one.
				//
				// Also above stabilize, as before. Stabilize is proximity alone
				// and cannot tell a hand supporting the weapon from one that
				// merely happens to be near it; a hand on a named object is the
				// one case where we know which.
				ctx = GRIPCTX_Object;
			}
			else if (isMain && *vr_secondary_button_mappings)
			{
				ctx = GRIPCTX_Modifier;
			}
			else if (!isMain && (supporting || stabilizeGeometryOk))
			{
				ctx = GRIPCTX_Stabilize;
			}
			else
			{
				ctx = GRIPCTX_Plain;
			}
			// SAY WHAT IT DECIDED, on change only.
			//
			// The whole promise here is one hand, one meaning -- and there has
			// never been any way to see which meaning won. From inside a
			// headset a grip that does the wrong thing and a grip that arrived
			// as the wrong context are indistinguishable, and there is no
			// console to ask. Printed on transition rather than per frame, so a
			// steady state costs one line and not ninety a second.
			if (*vr_grip_debug && xrGripContext[h] != ctx)
			{
				static const char *ctxName[] = {
					"none", "HOLSTER", "stabilize", "modifier", "plain",
					"OBJECT", "HARDPOINT"
				};
				const int nctx = int(sizeof(ctxName) / sizeof(ctxName[0]));
				Printf("[GRIP] %s: %s -> %s   (held=%d holster=%d hardpoint=%d grab=%d claimed=%d)\n",
					isMain ? "MAIN" : "OFF ",
					(xrGripContext[h] >= 0 && xrGripContext[h] < nctx) ? ctxName[xrGripContext[h]] : "?",
					(ctx >= 0 && ctx < nctx) ? ctxName[ctx] : "?",
					held ? 1 : 0, holsterHere ? 1 : 0, hardpointHere ? 1 : 0,
					grabHere ? 1 : 0, claimed);
			}
			xrGripContext[h] = ctx;

			// Published whatever the context resolved to, including when the
			// grip is not held at all. A hand keeps holding a magazine while
			// the player relaxes the squeeze, and the pose has to keep showing
			// that -- what a grip MEANS is decided per frame, what a hand is ON
			// is not.
			// A claim beats the inferred holster subject. Script naming what
			// the hand holds is more specific than the engine noticing where
			// the hand is, and a hand that came out of the pouch WITH a
			// magazine, still inside the volume, should be shown holding the
			// magazine rather than still reaching for it.
			//
			// A stabilizing hand with nothing claimed is SUPPORTING the other
			// hand's weapon, and the engine can say so itself -- that is what
			// stabilize means, and it is the one subject derivable without
			// knowing anything about the weapon. It matters because stabilize
			// used to be a grab in the code only: the aim went two-handed
			// while the hand still hung there posed as an empty fist. Filling
			// this in means every weapon in every mod gets a support hand
			// without cooperating, and a weapon that knows better -- a shotgun
			// with a pump to grab -- overrides it by claiming.
			// SUPPORT: the off hand is on the weapon but not on a named part.
			//
			// This used to be derived from ctx == GRIPCTX_Stabilize, which made
			// it unreachable. Stabilize only wins when Forend or Foregrip has
			// been claimed -- and if one of those IS claimed, that is the
			// subject you get instead. So the two-handed pistol hold could
			// never happen: a dead branch, and a pose baked for nothing.
			//
			// It is simply what an unclaimed off-hand grip MEANS while the
			// other hand holds a weapon. Nothing else asked for that grip, so
			// supporting the gun is the answer.
			const bool holdingWeapon = consolePawn && consolePawn->player
				&& consolePawn->player->ReadyWeapon != nullptr;
			int subj = GRIPSUBJ_None;
			if (claimed > 0)                            subj = claimed;
			else if (holsterHere)                       subj = GRIPSUBJ_Holster;
			// A hardpoint reach LOOKS like a holster reach, because it is the
			// same thing: a splayed hand about to take hold of something on
			// your body. The two are distinguished by CONTEXT, which is the
			// question "what is this grip for"; the subject is the question
			// "what shape is the hand", and there they have the same answer.
			// Giving hardpoints their own subject would mean authoring a second
			// pose identical to the first.
			else if (hardpointHere)                     subj = GRIPSUBJ_Holster;
			else if (!isMain && held && holdingWeapon)  subj = GRIPSUBJ_Support;
			xrGripSubject[h] = subj;
		}

		// Publish to ZScript so a mod can see what grip is doing rather than
		// guessing from the raw button, same as the engine now does. Not while a VR
		// demo replays: the recording publishes these (p_vrdemo.cpp).
		const bool publishGrip = consolePawn != nullptr && !VRDemo_IsReplaying();
		if (publishGrip)
		{
			consolePawn->GripContextMain = xrGripContext[mainHand];
			consolePawn->GripContextOff  = xrGripContext[offHand];
			// The raw squeeze, published alongside what the arbiter made of it.
			// Held state needs the edge, and the context above cannot give it
			// one once something has claimed -- see actor.h.
			consolePawn->GripHeldMain    = handInput[mainHand].grip;
			consolePawn->GripHeldOff     = handInput[offHand].grip;
			consolePawn->GripSubjectMain = xrGripSubject[mainHand];
			consolePawn->GripSubjectOff  = xrGripSubject[offHand];
			consolePawn->FingerTouchMain = xrFingerTouch[mainHand];
			consolePawn->FingerTouchOff  = xrFingerTouch[offHand];

			// HOW FAR, not merely whether. The booleans above answer "is it
			// pressed"; these answer "how far in is it", which is what a
			// trigger with a real release point, a partial pull that never
			// fires, or a grip that tightens on a part all need. See the
			// action declarations in the header for why both exist.
			consolePawn->TriggerValueMain = handInput[mainHand].triggerValue;
			consolePawn->TriggerValueOff  = handInput[offHand].triggerValue;
			consolePawn->GripValueMain    = handInput[mainHand].gripValue;
			consolePawn->GripValueOff     = handInput[offHand].gripValue;

			// WHERE THE THUMB IS, not just that it is touching. FingerTouch
			// above is a bitfield -- thumb down or not -- which cannot express
			// a thumb sliding along a selector or a stick pushed part way.
			// The thumbstick axes were already being read every frame and
			// discarded here; publishing them is the whole change.
			consolePawn->ThumbPosMain = DVector2(handInput[mainHand].thumbstick.x, handInput[mainHand].thumbstick.y);
			consolePawn->ThumbPosOff  = DVector2(handInput[offHand].thumbstick.x, handInput[offHand].thumbstick.y);

			consolePawn->VRTurnYaw = snapTurn;
		}

		// A real two-handed hold: the off hand is ON the weapon, at the grip or
		// the forend, because script said so. This is the thing worth knowing,
		// and it is published rather than acted on -- weapons read it to
		// tighten their spread, which is the benefit a second hand should
		// actually buy.
		const int offSubject = xrGripSubject[offHand];
		const bool twoHanded = (offSubject == GRIPSUBJ_Support
			|| offSubject == GRIPSUBJ_Forend || offSubject == GRIPSUBJ_Foregrip);
		if (publishGrip)
			consolePawn->TwoHandedHold = twoHanded;

		// Moving the weapon is a separate, opt-in thing. Defaults off: your
		// hands hold the gun where you are holding it.
		weaponStabilised = twoHanded && vr_two_handed_weapons;

		if (weaponStabilised)
		{
			const float z = xrHandPoses[offHand].position.z - xrHandPoses[mainHand].position.z;
			const float x = xrHandPoses[offHand].position.x - xrHandPoses[mainHand].position.x;
			const float y = xrHandPoses[offHand].position.y - xrHandPoses[mainHand].position.y;
			const float zxDist = std::sqrt(x * x + z * z);
			// Avoid near-vertical hand stacking from forcing the stabilised aim
			// almost straight up/down for a frame.
			if (zxDist > 0.05f && distance > 0.05f)
			{
				weaponangles[0] = -(float)(atanf(y / zxDist) * (180.0 / M_PI));
				weaponangles[1] = -(float)(atan2f(x, -z) * (180.0 / M_PI));
			}
			else
			{
				weaponStabilised = false;
			}
		}
	}
	else
	{
		weaponStabilised = false;
	}

	if (menuModeChanged)
	{
		ProcessHaptics();
		return;
	}

	if (gameplayMode)
	{
		const bool suppressLocomotion = dominantGripModifierNew;
		remote_movementSideways = 0.0f;
		remote_movementForward = 0.0f;

		auto applyCurve = [](float value)
		{
			const float deadZone = 0.15f;
			const float absValue = fabsf(value);
			if (absValue < deadZone)
				return 0.0f;

			float scaled = (absValue - deadZone) / (1.0f - deadZone);
			scaled *= scaled;
			return (value < 0.0f) ? -scaled : scaled;
		};

		if (!suppressLocomotion)
		{
			const XrVector2f leftMovementState = useTrackpad
				? handInput[movementHand].trackpad
				: handInput[movementHand].thumbstick;
			float moveDist = OpenVRLength2D(leftMovementState.x, leftMovementState.y);
			const float nlf = OpenVRNonLinearFilter(moveDist);
			moveDist = (moveDist > 1.0f) ? moveDist : 1.0f;
			float moveX = nlf * (leftMovementState.x / moveDist);
			float moveY = nlf * (leftMovementState.y / moveDist);
			const bool playerMoving = (fabsf(moveX) + fabsf(moveY)) > 0.05f;
			moveX = playerMoving ? moveX : 0.0f;
			moveY = playerMoving ? moveY : 0.0f;

			remote_movementSideways = moveX;
			remote_movementForward = moveY;

			if (*vr_teleport)
			{
				if (moveY > 0.7f && !ready_teleport)
				{
					ready_teleport = true;
				}
				else if (moveY < 0.7f && ready_teleport)
				{
					ready_teleport = false;
					trigger_teleport = true;
				}
			}
		}
		else
		{
			remote_movementSideways = 0.0f;
			remote_movementForward = 0.0f;
		}

	}
	else
	{
		remote_movementSideways = 0.0f;
		remote_movementForward = 0.0f;
	}

	auto emitGameplayHandButtons = [&](int hand, bool emitAxes)
	{
		const bool dominantHand = (hand == mainHand);
		const bool modifierOld = dominantGripModifierOld;
		const bool modifierNew = dominantGripModifierNew;
		const int handOffset = HandKeyOffset(hand);
		const int axisOffset = HandAxisKeyOffset(hand);
		const int oppositeAxisOffset = HandAxisKeyOffset(1 - hand);
		const int gripKey = KEY_PAD_LSHOULDER + handOffset;
		const bool suppressGripButton = dominantHand && *vr_secondary_button_mappings;
		const bool face1Pressed = hand == 1 ? handInput[hand].a : handInput[hand].x;
		const bool face2Pressed = hand == 1 ? handInput[hand].b : handInput[hand].y;
		const bool face1Old = hand == 1 ? xrLastAState[hand] : xrLastXState[hand];
		const bool face2Old = hand == 1 ? xrLastBState[hand] : xrLastYState[hand];
		const int face1BaseKey = hand == 1 ? KEY_PAD_A : KEY_PAD_X;
		const int face2BaseKey = hand == 1 ? KEY_PAD_B : KEY_PAD_Y;
		const int face1AltKey = dominantHand ? KEY_PAD_LTHUMB : KEY_PGDN;
		const int face2AltKey = dominantHand ? KEY_BACKSPACE : KEY_PGUP;
		const int triggerBaseKey = dominantHand ? KEY_PAD_RTRIGGER : KEY_LSHIFT;
		const int triggerAltKey = dominantHand ? KEY_PAD_LTRIGGER : KEY_LALT;
		const int thumbBaseKey = dominantHand ? KEY_ENTER : KEY_SPACE;
		const int thumbAltKey = dominantHand ? KEY_TAB : KEY_HOME;
		const int gripAltKey = KEY_PAD_DPAD_UP;
		// Per-hand, not one shared key: which hand reached into the holster
		// decides which weapon moves, and a single key for both would be
		// ambiguous the moment the player has both hands in holsters at once.
		//
		// F13 main hand, F14 off hand. Deliberately obscure keys with nothing
		// else on them: PgUp/PgDn are wanted elsewhere, and every pad key is
		// already spoken for by the modifier roster.
		const int holsterKey = dominantHand ? 0x64 /* F13 */ : 0x65 /* F14 */;

		// [BB] A CLAIMED POSE TAKES THIS HAND'S BUTTONS.
		//
		// A pose that binds actions to buttons has to take those buttons away
		// from what they normally do: pull the trigger to fire a wrist mount and
		// you must not also fire the gun in that hand. That is the whole reason
		// the mounts were unreachable -- KEYCONF says their events could only
		// ever be bound to a spare keyboard key, "because a raw trigger read
		// ALSO fires whatever the off hand's held weapon does on that same
		// physical pull".
		//
		// So while HardpointClaim is up on a hand, its grip, face pad and
		// trigger stop emitting their normal keys and are published to script
		// instead. The engine suppresses, because it is the only place that can;
		// script decides what a mount does, because that is not the engine's
		// business.
		//
		// Nothing is seized permanently. The claim is raised by a deliberate
		// wrist position and drops the moment the hand leaves it, and every
		// button goes straight back to its normal key.
		AActor* hpPawn = players[consoleplayer].mo;
		const bool hpArmed = hpPawn
			&& (dominantHand ? hpPawn->HardpointClaimMain : hpPawn->HardpointClaimOff);

		if (hpArmed && hpPawn)
		{
			// Bit 0 grip, 1 pad, 2 trigger. Main in the low nibble, off in the
			// high one, so one field carries both hands.
			int bits = 0;
			if (handInput[hand].grip)   bits |= 1;
			if (face1Pressed)           bits |= 2;
			if (handInput[hand].select) bits |= 4;

			const int shift = dominantHand ? 0 : 4;
			hpPawn->HardpointButtons =
				(hpPawn->HardpointButtons & ~(0xF << shift)) | (bits << shift);
		}
		else if (hpPawn)
		{
			hpPawn->HardpointButtons &= ~(0xF << (dominantHand ? 0 : 4));
		}

		// A one-frame pulse set by the arbiter when a clean grip tap finished
		// inside a holster. Not a live "is grip held" test -- grip keeps doing
		// its normal jobs throughout, and this only fires on the release that
		// turned out not to be a combo.
		const bool holsterFiring = xrHolsterFire[hand];

		// When virtual menu mouse is active on the right hand, the trigger drives
		// GUI left-click events. Suppress trigger-as-key to avoid menu key-path
		// conflicts (notably messagebox yes/no confirmations).
		const bool suppressSelectAsKey = hpArmed || (menuMode && menuactive != MENU_WaitKey && hand == 1 && *vr_menu_pointer && (*vr_mouse_in_menu || handInput[1].grip));
		if (suppressSelectAsKey)
		{
			const int oldSelectKey = modifierOld ? triggerAltKey : triggerBaseKey;
			PostControllerKeyTransition(xrLastSelectState[hand], false, oldSelectKey);
		}
		else
		{
			PostRemappedControllerKeyTransition(xrLastSelectState[hand], handInput[hand].select, modifierOld, modifierNew, triggerBaseKey, triggerAltKey);
		}
		// The holster key is a clean down-then-up pulse: fire on the frame the
		// arbiter says a tap completed, release on the next. Grip itself is
		// NOT suppressed -- it goes on doing whatever else it does, which is
		// the point of resolving tap vs combo instead of seizing the button.
		PostControllerKeyTransition(xrLastHolsterState[hand], holsterFiring, holsterKey);
		// Unlike xrLastGripState (updated in syncHandState) this one is ours,
		// so it has to be advanced here or every frame looks like a fresh
		// press to PostControllerKeyTransition.
		xrLastHolsterState[hand] = holsterFiring;
		xrHolsterFire[hand] = false;
		const bool gripTapping = xrGripTapFire[hand];
		xrGripTapFire[hand] = false;

		// GRIP IS NEVER SUPPRESSED, and suppressing it was a real bug.
		//
		// HardpointClaim goes true whenever the hand is merely NEAR a mount,
		// not only when the palm-out gesture is armed -- and the store/draw
		// bind IS the grip (RShoulder). So gating grip on it meant reaching for
		// a mount silently killed the one button that stores into it. Nothing
		// could ever be put in a hardpoint while that was in place.
		//
		// The trigger and pad are still taken while armed, which is what stops
		// a mount firing your held weapon as well. Grip keeps its normal job.
		{
			if (suppressGripButton)
			{
				// The dominant grip is the shift layer, so it must not fire as a
				// button DURING a combo -- grip+B would otherwise also trigger
				// whatever plain grip is bound to.
				//
				// But suppressing it unconditionally meant it could never be
				// bound to anything at all: Customize Controls captures a key
				// by waiting for one to arrive, and this one never did. So a
				// CLEAN TAP -- pressed and released with no other button --
				// emits as a one-frame pulse. Combos stay silent, and grip on
				// its own becomes a real bindable key (RShoulder) without
				// costing the shift layer anything.
				PostControllerKeyTransition(xrLastGripTapState[hand], gripTapping, gripKey);
				xrLastGripTapState[hand] = gripTapping;
			}
			else if (!dominantHand && *vr_secondary_button_mappings)
			{
				PostRemappedControllerKeyTransition(xrLastGripState[hand], handInput[hand].grip, modifierOld, modifierNew, gripKey, gripAltKey);
			}
			else
			{
				PostControllerKeyTransition(xrLastGripState[hand], handInput[hand].grip, gripKey);
			}
		}
		PostRemappedControllerKeyTransition(xrLastThumbClickState[hand], handInput[hand].thumbClick, modifierOld, modifierNew, thumbBaseKey, thumbAltKey);
		const int thumbClickKey = (hand == 1) ? KEY_PAD_RTHUMB : KEY_PAD_LTHUMB;
		// Keep the physical key only while waiting for a bind capture.
		if (menuactive == MENU_WaitKey)
		{
			PostControllerKeyTransition(xrLastThumbClickState[hand], handInput[hand].thumbClick, thumbClickKey);
		}
		// face1 is the pose's second action while it is armed, so only face2
		// keeps its normal job. Suppressing both would take a button the pose
		// has no use for.
		if (hpArmed)
		{
			PostControllerKeyTransition(face2Old, face2Pressed, face2BaseKey);
		}
		else if (dominantHand)
		{
			PostRemappedControllerKeyTransition(face1Old, face1Pressed, dominantGripModifierOld, dominantGripModifierNew, face1BaseKey, face1AltKey);
			PostRemappedControllerKeyTransition(face2Old, face2Pressed, dominantGripModifierOld, dominantGripModifierNew, face2BaseKey, face2AltKey);
		}
		else if (*vr_secondary_button_mappings)
		{
			PostRemappedControllerKeyTransition(face1Old, face1Pressed, modifierOld, modifierNew, face1BaseKey, KEY_PGDN);
			PostRemappedControllerKeyTransition(face2Old, face2Pressed, modifierOld, modifierNew, face2BaseKey, KEY_PGUP);
		}
		else
		{
			PostControllerKeyTransition(face1Old, face1Pressed, face1BaseKey);
			PostControllerKeyTransition(face2Old, face2Pressed, face2BaseKey);
		}

		// Match the OpenVR control-mode split: mode 1 keeps the extra joystick
		// remapping layer, while mode 0 stays on the leaner controller mapping.
		if (emitAxes && vr_joy_mode == 1)
		{
			const XrVector2f& lastAxisState = useTrackpad ? xrLastTrackpadState[hand] : xrLastThumbstickState[hand];
			const XrVector2f& newAxisState = useTrackpad ? handInput[hand].trackpad : handInput[hand].thumbstick;
			const int baseLeftKey = hand == 1 ? KEY_JOYAXIS3MINUS : KEY_JOYAXIS1MINUS;
			const int baseRightKey = hand == 1 ? KEY_JOYAXIS3PLUS : KEY_JOYAXIS1PLUS;
			const int baseDownKey = hand == 1 ? KEY_JOYAXIS4MINUS : KEY_JOYAXIS2MINUS;
			const int baseUpKey = hand == 1 ? KEY_JOYAXIS4PLUS : KEY_JOYAXIS2PLUS;
			const int shiftedLeftKey = hand == 1 ? KEY_JOYAXIS7MINUS : KEY_JOYAXIS5MINUS;
			const int shiftedRightKey = hand == 1 ? KEY_JOYAXIS7PLUS : KEY_JOYAXIS5PLUS;
			const int shiftedDownKey = hand == 1 ? KEY_JOYAXIS8MINUS : KEY_JOYAXIS6MINUS;
			const int shiftedUpKey = hand == 1 ? KEY_JOYAXIS8PLUS : KEY_JOYAXIS6PLUS;

			PostRemappedControllerAxisTransitions(lastAxisState, newAxisState, modifierOld, modifierNew,
				baseLeftKey,
				baseRightKey,
				baseDownKey,
				baseUpKey,
				shiftedLeftKey,
				shiftedRightKey,
				shiftedDownKey,
				shiftedUpKey);
		}

		syncHandState(hand);
	};

	xrMenuPointerActive = false;
	xrMenuPointerHasHit = false;
	xrMenuPointerBeamVisible = false;
	xrMenuPointerBeamLength = 0.0f;
	menu_allow_mouse_override = false;
	const bool keybindCaptureMode = menuactive == MENU_WaitKey;

	// Keep the virtual screen pose in sync with the current frame before casting the OpenXR menu pointer ray.
	// Otherwise the ray/beam can intersect last frame's quad transform while the actual menu layer is updated
	// later in the frame, which shows up as the beam and cursor no longer meeting.
	if (menuMode && ShouldUseScreenLayerForCurrentFrame())
	{
		uint32_t virtualScreenWidth = 0;
		uint32_t virtualScreenHeight = 0;
		GetStableOpenXRVirtualScreenSize(virtualScreenWidth, virtualScreenHeight);
		if (virtualScreenWidth > 0 && virtualScreenHeight > 0)
		{
			xrVirtualScreenWidth = virtualScreenWidth;
			xrVirtualScreenHeight = virtualScreenHeight;
			updateVirtualScreenLayer();
		}
	}

	const int pointerHand = 1; // Always use the right controller for virtual menu mouse.
	const bool vrMouseEnabled = !keybindCaptureMode && (*vr_mouse_in_menu || handInput[pointerHand].grip);
	const bool canUseMenuPointer = menuMode &&
		!keybindCaptureMode &&
		*vr_menu_pointer &&
		vrMouseEnabled &&
		pointerHand >= 0 && pointerHand < 2 &&
		xrHandPoseValid[pointerHand] &&
		xrVirtualScreenWidth > 0 &&
		xrVirtualScreenHeight > 0 &&
		xrViewCount > 0 &&
		xrViews.size() >= xrViewCount;

	if (canUseMenuPointer)
	{
		float screenWidth = 0.0f;
		float screenHeight = 0.0f;
		GetOpenXRVirtualScreenMeters(xrVirtualScreenWidth, xrVirtualScreenHeight, screenWidth, screenHeight);
		const XrQuaternionf screenOrientation = xrVirtualScreenPose.orientation;
		const XrVector3f planeOrigin = xrVirtualScreenPose.position;

		const XrVector3f planeNormal = NormalizeVector(RotateVector(screenOrientation, { 0.0f, 0.0f, 1.0f }));
		const XrVector3f planeRight = NormalizeVector(RotateVector(screenOrientation, { 1.0f, 0.0f, 0.0f }));
		const XrVector3f planeUp = NormalizeVector(RotateVector(screenOrientation, { 0.0f, 1.0f, 0.0f }));
		const XrVector3f rayOrigin = xrHandPoses[pointerHand].position;
		const XrQuaternionf pointerAlignRotation = QuaternionFromAxisAngle(0.0f, 0.0f, 1.0f, -(vr_weaponRotate * 2.0f) * (float)(M_PI / 180.0));
		const XrQuaternionf pointerOrientation = MultiplyQuaternion(xrHandPoses[pointerHand].orientation, pointerAlignRotation);

		struct RayHit
		{
			bool valid = false;
			float t = 0.0f;
			float unclampedU = 0.0f;
			float unclampedV = 0.0f;
			float localX = 0.0f;
			float localY = 0.0f;
			float overflow = 0.0f;
			XrVector3f hitPoint = { 0.0f, 0.0f, 0.0f };
		};

		auto testRayAxis = [&](const XrVector3f& localAxis) -> RayHit
		{
			RayHit out;
			const XrVector3f rayDir = NormalizeVector(RotateVector(pointerOrientation, localAxis));
			const float denom = DotVector(rayDir, planeNormal);
			if (fabsf(denom) <= 0.0001f)
			{
				return out;
			}

			const float t = DotVector(SubtractVector(planeOrigin, rayOrigin), planeNormal) / denom;
			if (t <= 0.0f)
			{
				return out;
			}

			const XrVector3f hitPoint = AddVector(rayOrigin, ScaleVector(rayDir, t));
			const XrVector3f hitDelta = SubtractVector(hitPoint, planeOrigin);
			out.hitPoint = hitPoint;
			const float localX = DotVector(hitDelta, planeRight);
			const float localY = DotVector(hitDelta, planeUp);
			const float halfW = screenWidth * 0.5f;
			const float halfH = screenHeight * 0.5f;
			out.localX = localX;
			out.localY = localY;
			out.unclampedU = (localX + halfW) / std::max(screenWidth, 0.0001f);
			out.unclampedV = (halfH - localY) / std::max(screenHeight, 0.0001f);
			out.overflow =
				(out.unclampedU < 0.0f ? -out.unclampedU : 0.0f) +
				(out.unclampedU > 1.0f ? out.unclampedU - 1.0f : 0.0f) +
				(out.unclampedV < 0.0f ? -out.unclampedV : 0.0f) +
				(out.unclampedV > 1.0f ? out.unclampedV - 1.0f : 0.0f);
			out.t = t;
			out.valid = true;
			return out;
		};

		const XrVector3f rayAxis = { 0.0f, 0.0f, -1.0f };
		const RayHit bestHit = testRayAxis(rayAxis);

		if (bestHit.valid)
		{
			const bool inside = bestHit.overflow <= 0.0001f;
			const float mappedU = 1.0f - bestHit.unclampedU;
			const float mappedV = 1.0f - bestHit.unclampedV;
			float u = clamp<float>(mappedU, 0.0f, 1.0f);
			float v = clamp<float>(mappedV, 0.0f, 1.0f);
			const int mouseX = clamp<int>((int)std::lround(u * (float)(xrVirtualScreenWidth - 1)), 0, (int)xrVirtualScreenWidth - 1);
			const int mouseY = clamp<int>((int)std::lround(v * (float)(xrVirtualScreenHeight - 1)), 0, (int)xrVirtualScreenHeight - 1);
			// Build a world-space quad layer representing a laser beam from
			// controller pose to the virtual-screen hit point.
			const XrVector3f beamStart = rayOrigin;
			const XrVector3f beamEnd = bestHit.hitPoint;
			const XrVector3f beamVec = SubtractVector(beamEnd, beamStart);
			const float beamLength = std::sqrt(DotVector(beamVec, beamVec));
			if (beamLength > 0.01f)
			{
				const XrVector3f beamDir = ScaleVector(beamVec, 1.0f / beamLength);
				const XrVector3f beamCenter = AddVector(beamStart, ScaleVector(beamDir, beamLength * 0.5f));
				XrVector3f viewDir = NormalizeVector(SubtractVector(xrViews[0].pose.position, beamCenter));
				XrVector3f xAxis = NormalizeVector(CrossVector(viewDir, beamDir));
				if (DotVector(xAxis, xAxis) < 0.0001f)
				{
					xAxis = NormalizeVector(CrossVector({ 0.0f, 1.0f, 0.0f }, beamDir));
					if (DotVector(xAxis, xAxis) < 0.0001f)
						xAxis = NormalizeVector(CrossVector({ 1.0f, 0.0f, 0.0f }, beamDir));
				}
				const XrVector3f xBeamAxis = beamDir;
				const XrVector3f yBeamAxis = xAxis;
				const XrVector3f zAxis = NormalizeVector(CrossVector(xBeamAxis, yBeamAxis));
				xrMenuPointerBeamPose.position = beamCenter;
				xrMenuPointerBeamPose.orientation = QuaternionFromBasis(xBeamAxis, yBeamAxis, zAxis);
				xrMenuPointerBeamLength = beamLength;
				xrMenuPointerBeamVisible = true;
			}

			const bool dragHoldActive = xrMenuPointerLastLeftDown || xrMenuPointerLastRightDown;
			const bool allowClampedInteraction = inside || dragHoldActive;
			if (allowClampedInteraction)
			{
				xrMenuPointerActive = true;
				xrMenuPointerHasHit = true;
				xrMenuPointerX = (float)mouseX;
				xrMenuPointerY = (float)mouseY;
				menu_allow_mouse_override = true;

				if (!xrMenuPointerHadPos || mouseX != xrMenuPointerLastX || mouseY != xrMenuPointerLastY)
				{
					PostGuiMouseEvent(EV_GUI_MouseMove, mouseX, mouseY);
					xrMenuPointerLastX = mouseX;
					xrMenuPointerLastY = mouseY;
				}
				xrMenuPointerHadPos = true;

				if (handInput[pointerHand].select != xrMenuPointerLastLeftDown)
				{
					PostGuiMouseEvent(handInput[pointerHand].select ? EV_GUI_LButtonDown : EV_GUI_LButtonUp, xrMenuPointerLastX, xrMenuPointerLastY);
					xrMenuPointerLastLeftDown = handInput[pointerHand].select;
				}
				// If vr_mouse_in_menu is disabled, grip acts as a temporary "hold to use mouse"
				// modifier, so do not also emit right-click from the same button.
				const bool emitRightClick = *vr_mouse_in_menu;
				if (emitRightClick && handInput[pointerHand].grip != xrMenuPointerLastRightDown)
				{
					PostGuiMouseEvent(handInput[pointerHand].grip ? EV_GUI_RButtonDown : EV_GUI_RButtonUp, xrMenuPointerLastX, xrMenuPointerLastY);
					xrMenuPointerLastRightDown = handInput[pointerHand].grip;
				}
				else if (!emitRightClick)
				{
					xrMenuPointerLastRightDown = false;
				}
			}
		}

		// Allow scrolling long menus with right-thumbstick vertical movement,
		// even if the current ray cast misses the panel.
		if (vr_joy_mode == 1)
		{
			constexpr float wheelDeadZone = 0.55f;
			static int wheelRepeatCooldown = 0;
			if (wheelRepeatCooldown > 0) wheelRepeatCooldown--;
			const float wheelY = handInput[pointerHand].thumbstick.y;
			if (wheelRepeatCooldown == 0)
			{
				if (wheelY >= wheelDeadZone)
				{
					PostGuiWheelEvent(EV_GUI_WheelUp, xrMenuPointerLastX, xrMenuPointerLastY);
					wheelRepeatCooldown = 8;
				}
				else if (wheelY <= -wheelDeadZone)
				{
					PostGuiWheelEvent(EV_GUI_WheelDown, xrMenuPointerLastX, xrMenuPointerLastY);
					wheelRepeatCooldown = 8;
				}
			}
		}
	}
	if (!menuMode || !xrMenuPointerActive)
	{
		if (xrMenuPointerLastLeftDown || xrMenuPointerLastRightDown)
		{
			const int releaseX = xrMenuPointerHadPos ? xrMenuPointerLastX : 0;
			const int releaseY = xrMenuPointerHadPos ? xrMenuPointerLastY : 0;
			if (xrMenuPointerLastLeftDown)
			{
				PostGuiMouseEvent(EV_GUI_LButtonUp, releaseX, releaseY);
			}
			if (xrMenuPointerLastRightDown)
			{
				PostGuiMouseEvent(EV_GUI_RButtonUp, releaseX, releaseY);
			}
		}
		xrMenuPointerLastLeftDown = false;
		xrMenuPointerLastRightDown = false;
		if (!menuMode)
		{
			xrMenuPointerHadPos = false;
		}
	}

	if (menuMode)
	{
		emitGameplayHandButtons(0, true);
		emitGameplayHandButtons(1, menuactive == MENU_WaitKey);
		// Haptics still need servicing here. This returns before the
		// ProcessHaptics() at the end of the function, so without this call a
		// pulse in flight when a menu opens is never advanced and never
		// stopped -- the sibling early return above does the same thing for
		// exactly this reason.
		ProcessHaptics();
		return;
	}

	emitGameplayHandButtons(0, true);
	emitGameplayHandButtons(1, true);
	// While a VR demo replays, the pawn's head, hands, crouch and between-tic moves come
	// from the recording (p_vrdemo.cpp), not from the headset that happens to be on.
	if (gameplayMode && !VRDemo_IsReplaying())
	{
		player_t* player = &players[consoleplayer];
		if (player && player->mo)
		{
			const float hmdHeight = GetHmdAdjustedHeightInMapUnit(xrUsingStageSpace ? false : xrHasLocalHeightAnchor, xrLocalHeightAnchor);

			// Real head pose in world space, for body-relative UI (holsters and
			// similar) that needs the player's actual physical position rather
			// than an aim ray. CenterEyePos is DVector3 in the same space as
			// AActor.Pos (r_utility.cpp: CenterEyePos = viewPoint.Pos), so this
			// is a direct assignment -- no axis swap, unlike AttackPos below
			// which reads a raw OpenXR matrix in a different convention.
			if (!multiplayer)
			{
				player->mo->HmdPos  = r_viewpoint.CenterEyePos;
				player->mo->HmdYaw   = r_viewpoint.Angles.Yaw;
				// PITCH AND ROLL FROM THE HEADSET, NOT THE GAMEPLAY VIEW.
				//
				// r_viewpoint.Angles never carries the head's tilt, so these fields
				// reported a level head and RS_VRBody's torso never faded when you
				// looked down. Reading r_viewpoint.HWAngles instead does not fix it:
				// updateHmdPose writes the head's tilt there, but R_SetupFrame
				// (r_utility.cpp) rebuilds HWAngles from Angles every frame, so by the
				// time this runs it is the gameplay pitch again. hmdorientation is the
				// head's own, kept by updateHmdPose, in the sign HWAngles uses:
				// positive pitch is down.
				player->mo->HmdPitch = DAngle::fromDeg(hmdorientation[0]);
				player->mo->HmdRoll  = DAngle::fromDeg(hmdorientation[2]);
			}

			if (!multiplayer)
			{
				if (!vr_crouch_use_button)
				{
					const double defaultViewHeight = player->DefaultViewHeight();
					if (defaultViewHeight > 0.0)
					{
						player->crouching = 10;
						player->crouchfactor = hmdHeight / defaultViewHeight;
					}
				}
				else if (player->crouching == 10)
				{
					player->Uncrouch();
				}
			}
			else if (!vr_crouch_use_button)
			{
				VR_SetMultiplayerCrouchHeight(hmdHeight);
			}
			else
			{
				VR_ClearMultiplayerCrouchHeight();
			}

			LSMatrix44 mat;
			if (GetWeaponTransform(&mat, VR_MAINHAND))
			{
				if (!multiplayer)
				{
					player->mo->AttackPos.X = mat[3][0];
					player->mo->AttackPos.Y = mat[3][2];
					player->mo->AttackPos.Z = mat[3][1];
					player->mo->AttackPitch = DAngle::fromDeg(ShouldUseScreenLayerForCurrentFrame()
						? -weaponangles[PITCH] - r_viewpoint.Angles.Pitch.Degrees()
						: -weaponangles[PITCH]);
					player->mo->AttackAngle = DAngle::fromDeg(-90 + GetViewpointYaw() + (weaponangles[YAW] - playerYaw));
					player->mo->AttackRoll = DAngle::fromDeg(weaponangles[ROLL]);
					// Same value, kept somewhere the playsim will not zero it.
					player->mo->MainHandRoll = player->mo->AttackRoll;

					// REAL velocity, not two AttackPos samples differenced in
					// ZScript at 35Hz. Zeroed rather than left stale when the
					// runtime didn't report it this frame -- a stale nonzero
					// velocity is worse than an honest zero for anything that
					// gates a throw on "did this cross a speed threshold".
					//
					// SIGNS AND SCALE AS ATTACKPOS GETS THEM. GetHandTransform puts
					// the hand at eye - vu*offset[0] (map X) and eye - vu*offset[2]
					// (map Y, the -vu scale on GL z), with height vu*offset[1] /
					// pixelstretch. The velocity went through offset[]'s own
					// rotation and swap, so it takes the same signs: both
					// horizontal axes NEGATED, height divided by pixelstretch.
					// Published without them, every throw and let-go flew mirrored
					// horizontally and ~1.2x too fast vertically. Angular takes
					// the same horizontal flip -- the XR-to-map mapping is a proper
					// rotation, so the pseudo-vector maps like the linear one.
					if (weaponVelValid)
					{
						const double velStretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
						player->mo->AttackVel.X = -weaponLinearVel[0] * vr_vunits_per_meter;
						player->mo->AttackVel.Y = -weaponLinearVel[2] * vr_vunits_per_meter;
						player->mo->AttackVel.Z = weaponLinearVel[1] * vr_vunits_per_meter / velStretch;
						player->mo->AttackAngularVel.X = -weaponAngularVel[0];
						player->mo->AttackAngularVel.Y = -weaponAngularVel[2];
						player->mo->AttackAngularVel.Z = weaponAngularVel[1];
					}
					else
					{
						player->mo->AttackVel = DVector3(0, 0, 0);
						player->mo->AttackAngularVel = DVector3(0, 0, 0);
					}
				}
			}

			LSMatrix44 matOffhand;
			if (!multiplayer && GetWeaponTransform(&matOffhand, VR_OFFHAND))
			{
				player->mo->OffhandPos.X = matOffhand[3][0];
				player->mo->OffhandPos.Y = matOffhand[3][2];
				player->mo->OffhandPos.Z = matOffhand[3][1];
				player->mo->OffhandPitch = DAngle::fromDeg(VR_UseCinematicScreenLayer()
					? -offhandangles[PITCH] - r_viewpoint.Angles.Pitch.Degrees()
					: -offhandangles[PITCH]);
				player->mo->OffhandAngle = DAngle::fromDeg(-90 + GetViewpointYaw() + (offhandangles[YAW] - hmdorientation[YAW]));
				player->mo->OffhandRoll = DAngle::fromDeg(offhandangles[ROLL]);

				if (offhandVelValid)
				{
					// Same signs and scale as AttackVel above -- see there.
					const double velStretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
					player->mo->OffhandVel.X = -offhandLinearVel[0] * vr_vunits_per_meter;
					player->mo->OffhandVel.Y = -offhandLinearVel[2] * vr_vunits_per_meter;
					player->mo->OffhandVel.Z = offhandLinearVel[1] * vr_vunits_per_meter / velStretch;
					player->mo->OffhandAngularVel.X = -offhandAngularVel[0];
					player->mo->OffhandAngularVel.Y = -offhandAngularVel[2];
					player->mo->OffhandAngularVel.Z = offhandAngularVel[1];
				}
				else
				{
					player->mo->OffhandVel = DVector3(0, 0, 0);
					player->mo->OffhandAngularVel = DVector3(0, 0, 0);
				}
			}

			if (vr_teleport && player->mo->health > 0)
			{
				DAngle yaw = DAngle::fromDeg(GetViewpointYaw() - hmdorientation[YAW] + offhandangles[YAW]);
				DAngle pitch = DAngle::fromDeg(offhandangles[PITCH]);
				const double pixelstretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;

				if (ready_teleport)
				{
					FLineTraceData trace;
					if (P_LineTrace(player->mo, yaw, 8192, pitch, TRF_ABSOFFSET | TRF_BLOCKUSE | TRF_BLOCKSELF | TRF_SOLIDACTORS,
						((hmdPosition[1] + offhandoffset[1] + vr_height_adjust) * vr_vunits_per_meter) / pixelstretch,
						-(offhandoffset[2] * vr_vunits_per_meter),
						-(offhandoffset[0] * vr_vunits_per_meter), &trace))
					{
						m_TeleportTarget = trace.HitType;
						m_TeleportLocation = trace.HitLocation;
					}
					else
					{
						m_TeleportTarget = TRACE_HitNone;
						m_TeleportLocation = DVector3(0, 0, 0);
					}
				}
				else if (trigger_teleport && m_TeleportTarget == TRACE_HitFloor)
				{
					if (multiplayer)
					{
						QueueMultiplayerTeleportTarget(player, m_TeleportLocation);
					}
					else
					{
						// The move itself lives in VR_ApplyRenderMove (p_vrdemo.cpp),
						// unchanged, so a VR demo can record it and replay it exactly.
						VR_ApplyRenderMove(player, VRMOVE_TELEPORT,
							DVector2(m_TeleportLocation.X - player->mo->X(),
								m_TeleportLocation.Y - player->mo->Y()));
					}

					m_TeleportTarget = TRACE_HitNone;
					m_TeleportLocation = DVector3(0, 0, 0);
				}

				trigger_teleport = false;
			}

			if (!multiplayer)
			{
				// Roomscale/HMD positional locomotion stays local to single-player until it has
				// an explicit deterministic netplay contract.
				// Every frame, as before; the move itself lives in VR_ApplyRenderMove
				// (p_vrdemo.cpp) so a VR demo can record it and replay it exactly.
				VR_ApplyRenderMove(player, VRMOVE_ROOMSCALE,
					DVector2(positional_movementSideways, positional_movementForward) * vr_vunits_per_meter);
			}

		}
	}
	ProcessHaptics();
}

void VKOpenXRDeviceMode::TearDown() const
{
	// [BB] THIS USED TO CALL StopHaptics(), AND THAT IS WHY HAPTICS NEVER WORKED.
	//
	// TearDown is not a shutdown hook. hw_entrypoint.cpp:278 calls it at the end of
	// EVERY rendered scene frame. StopHaptics issues xrStopHapticFeedback for each
	// live hand and then zeroes xrHapticDuration/xrHapticIntensity unconditionally,
	// outside its own loop -- and those two arrays are the whole pump's state.
	//
	// So every pulse was cancelled and erased in the same frame it was requested.
	// The per-issue window is deliberately clamped to a few milliseconds by
	// MakeOpenXRHapticDuration BECAUSE the pump is meant to re-issue it every frame
	// while the local timer runs. It never got a second frame. An LRA in a Touch
	// controller needs 20-30ms to reach useful amplitude and was being braked well
	// before that, so a 300ms test pulse and an 18ms tap felt identical: nothing.
	//
	// It presented as "haptics have never fired, in any situation" because the
	// cancel is unconditional -- it takes no arguments, tests nothing, and cannot
	// vary by trigger, hand, amplitude or requested duration.
	//
	// Session-scoped cancellation was never this function's job and is already
	// handled where it belongs: DestroyOpenXR, and the STOPPING / LOSS_PENDING /
	// EXITING session states.
	//
	// Servicing the pump here rather than deleting the call outright: end of frame
	// is a legitimate third re-issue point alongside BeginFrame and RenderViewpoint
	// setup, so the hook keeps earning its place.
	ProcessHaptics();
}

void VKOpenXRDeviceMode::ApplyRefreshRate() const
{
#ifdef XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME
	if (!xrHasDisplayRefreshRate || xrSession == XR_NULL_HANDLE || !isSessionRunning ||
		xrEnumerateDisplayRefreshRatesFB_inst == nullptr || xrRequestDisplayRefreshRateFB_inst == nullptr)
	{
		return;
	}

	uint32_t rateCount = 0;
	XrResult xrResult = xrEnumerateDisplayRefreshRatesFB_inst(xrSession, 0, &rateCount, nullptr);
	if (XR_FAILED(xrResult) || rateCount == 0)
		return;

	std::vector<float> rates(rateCount, 0.0f);
	xrResult = xrEnumerateDisplayRefreshRatesFB_inst(xrSession, rateCount, &rateCount, rates.data());
	if (XR_FAILED(xrResult) || rateCount == 0)
		return;

	const float requestedRate = (float)std::max(0, (int)vid_refreshrate);
	float selectedRate = rates[0];
	float bestDelta = std::fabs(selectedRate - requestedRate);
	for (uint32_t i = 1; i < rateCount; ++i)
	{
		const float delta = std::fabs(rates[i] - requestedRate);
		if (delta < bestDelta)
		{
			selectedRate = rates[i];
			bestDelta = delta;
		}
	}

	if (!xrLoggedDisplayRefreshRates)
	{
		FString rateList;
		for (uint32_t i = 0; i < rateCount; ++i)
		{
			if (i > 0)
				rateList += ", ";
			rateList.AppendFormat("%.0f", (double)rates[i]);
		}
		if (developer > 0)
			Printf("OpenXR: supported display refresh rates: %s Hz\n", rateList.GetChars());
		xrLoggedDisplayRefreshRates = true;
	}

	if (std::fabs(xrRequestedDisplayRefreshRate - selectedRate) < 0.01f)
		return;

	xrResult = xrRequestDisplayRefreshRateFB_inst(xrSession, selectedRate);
	if (XR_SUCCEEDED(xrResult))
	{
		xrRequestedDisplayRefreshRate = selectedRate;
		if (xrGetDisplayRefreshRateFB_inst != nullptr)
		{
			float currentRate = 0.0f;
			if (XR_SUCCEEDED(xrGetDisplayRefreshRateFB_inst(xrSession, &currentRate)))
				xrCurrentDisplayRefreshRate = currentRate;
			else
				xrCurrentDisplayRefreshRate = selectedRate;
		}
		else
		{
			xrCurrentDisplayRefreshRate = selectedRate;
		}

		if (developer > 0)
		{
			Printf("OpenXR: requested display refresh rate %.0f Hz (menu=%d, current=%.0f Hz)\n",
				(double)selectedRate,
				(int)vid_refreshrate,
				(double)(xrCurrentDisplayRefreshRate > 0.0f ? xrCurrentDisplayRefreshRate : selectedRate));
		}
	}
	else
	{
		if (developer > 0)
			Printf("OpenXR: failed to request display refresh rate %d Hz.\n", (int)vid_refreshrate);
	}
#endif
}

bool VKOpenXRDeviceMode::SubmitFrame() const
{
	if (!BeginXRFrame())
		return false;
	if (!AcquireXRSwapchain())
		return false;
	return true;
}

bool VKOpenXRDeviceMode::BeginXRFrame() const
{
	const uint64_t nextFrame = xrFrameCounter + 1;
	++xrFrameCounter;

	if (xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr)
		return false;

	if (xrSwapchain == XR_NULL_HANDLE && !CreateSwapchain())
		return false;

	if (gamestate == GS_LEVEL && (r_viewpoint.camera == nullptr || r_viewpoint.ViewLevel == nullptr))
	{
		return false;
	}

	if (isSessionReadyToBegin && !isSessionRunning)
	{
		XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
		beginInfo.primaryViewConfigurationType = viewType;
		XrResult r = xrBeginSession(xrSession, &beginInfo);
		if (XR_SUCCEEDED(r))
		{
			isSessionRunning = true;
			ApplyRefreshRate();
		}
		else
			return false;
		isSessionReadyToBegin = false;
	}

	if (!isSessionRunning)
		return false;

	if (xrFrameInProgress)
	{
		return false;
	}

	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	// [GOVERNOR] E8: the frame wait is the headset's pacing, not the frame's work, and its display period is the frame budget
	// (hw_effectsgovernor.h).
	const uint64_t governorWaitStartNs = I_nsTime();
	XrResult xrResult = xrWaitFrame(xrSession, &waitInfo, &xrFrameState);
	EffectsGovernor::AddPacingWait(I_nsTime() - governorWaitStartNs);
	if (XR_SUCCEEDED(xrResult))
		EffectsGovernor::NoteDisplayPeriod((int64_t)xrFrameState.predictedDisplayPeriod);
	if (XR_FAILED(xrResult))
		return false;

	XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	xrResult = xrBeginFrame(xrSession, &beginInfo);
	if (XR_FAILED(xrResult))
		return false;

	XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = viewType;
	locateInfo.displayTime = xrFrameState.predictedDisplayTime;
	locateInfo.space = xrSpace;
	XrViewState viewState{ XR_TYPE_VIEW_STATE };
	uint32_t viewCount = xrViewCount;
	if (viewCount == 0 || xrViews.size() < viewCount || xrProjectionViews.size() < viewCount)
		return false;
	xrResult = xrLocateViews(xrSession, &locateInfo, &viewState, viewCount, &viewCount, xrViews.data());
	if (XR_FAILED(xrResult))
	{
		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		xrFrameInProgress = false;
		return false;
	}
	for (uint32_t i = 0; i < viewCount; ++i)
	{
	}
	if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 || (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
	{
	}

	updateHmdPose(r_viewpoint);
	xrVirtualScreenVisible = false;
	xrVirtualScreenBackdropVisible = false;
	xrVirtualScreenImageIndex = -1;
	xrVirtualScreenBackdropImageIndex = -1;
	xrFrameInProgress = true;
	return true;
}

bool VKOpenXRDeviceMode::AcquireXRSwapchain() const
{

	auto* vkfb = dynamic_cast<VulkanRenderDevice*>(screen);
	if (!xrFrameInProgress)
	{
		return true;
	}

	xrFrameInProgress = false;

	if (!vkfb || xrSession == XR_NULL_HANDLE || xrSwapchain == XR_NULL_HANDLE || !isSessionRunning || xrVkDevice == nullptr || xrVkCommandBuffer == nullptr)
		return true;

	auto* framebufferManager = vkfb->GetFramebufferManager();
	auto* postprocess = vkfb->GetPostprocess();
	const bool shouldSubmitProjectionLayer = xrFrameState.shouldRender;
	const bool sessionVisibleOrFocused = xrSessionState == XR_SESSION_STATE_VISIBLE || xrSessionState == XR_SESSION_STATE_FOCUSED;

	if (!shouldSubmitProjectionLayer || !sessionVisibleOrFocused)
	{
		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		xrFrameInProgress = false;
		return XR_SUCCEEDED(endResult);
	}

	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t imageIndex = 0;
	XrResult xrResult = xrAcquireSwapchainImage(xrSwapchain, &acquireInfo, &imageIndex);
	if (XR_FAILED(xrResult))
	{
		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		xrFrameInProgress = false;
		return false;
	}

	XrSwapchainImageWaitInfo imageWaitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	imageWaitInfo.timeout = 20 * 1000 * 1000; // 20 ms
	{
		Clocker submitWaitTimer(VRSubmitWait);
		const uint64_t governorWaitStartNs = I_nsTime();	// [GOVERNOR] E8: the compositor holding the image is pacing
		xrResult = xrWaitSwapchainImage(xrSwapchain, &imageWaitInfo);
		EffectsGovernor::AddPacingWait(I_nsTime() - governorWaitStartNs);
	}
	if (xrResult == XR_TIMEOUT_EXPIRED)
	{
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		xrFrameInProgress = false;
		return XR_SUCCEEDED(endResult);
	}
	if (XR_FAILED(xrResult))
	{
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		xrFrameInProgress = false;
		return false;
	}

	auto* buffers = vkfb->GetBuffers();
	if (buffers == nullptr || postprocess == nullptr)
	{
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		return XR_SUCCEEDED(endResult);
	}

	const uint32_t recommendedW = GetMaxRecommendedViewWidth(xrViewConfigs);
	const uint32_t recommendedH = GetMaxRecommendedViewHeight(xrViewConfigs);
	const uint32_t dstW = xrPresentWidth != 0 ? xrPresentWidth : recommendedW;
	const uint32_t dstH = xrPresentHeight != 0 ? xrPresentHeight : recommendedH;
	if (dstW == 0 || dstH == 0)
	{
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		xrFrameInProgress = false;
		return XR_SUCCEEDED(endResult);
	}

	ScopedCycleTimer cycle(VRSubmit);
	vkResetCommandPool(xrVkDevice->device, xrVkCommandPool->pool, 0);
	xrVkCommandBuffer->begin();

	VkImage dstImage = xrSwapchainImages[imageIndex].image;
	VkImageMemoryBarrier dstBarrier{};
	dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstBarrier.srcAccessMask = 0;
	dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.image = dstImage;
	dstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, xrViewCount };
	vkCmdPipelineBarrier(
		xrVkCommandBuffer->buffer,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

	{
		Clocker submitCopyTimer(VRSubmitCopy);
		for (uint32_t layer = 0; layer < xrViewCount; ++layer)
		{
			const uint32_t sourceEye = (vr_openxr_debug_submit_mode == 1 || vr_openxr_debug_submit_mode == 3) ? 0u : layer;
			if (sourceEye >= xrPresentTextures.size() || xrPresentTextures[sourceEye].Image == nullptr)
			{
				continue;
			}

			auto& preparedEyeImage = xrPresentTextures[sourceEye];
			const auto* preparedEyeTexture = preparedEyeImage.Image.get();
			const int32_t srcW = preparedEyeTexture ? preparedEyeTexture->width : 0;
			const int32_t srcH = preparedEyeTexture ? preparedEyeTexture->height : 0;
			if (srcW <= 0 || srcH <= 0)
			{
				continue;
			}

			VkImageTransition()
				.AddImage(&preparedEyeImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
				.Execute(xrVkCommandBuffer.get());

			VkImageBlit blitRegion{};
			blitRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			blitRegion.srcOffsets[0] = { 0, 0, 0 };
			blitRegion.srcOffsets[1] = { srcW, srcH, 1 };
			blitRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
			blitRegion.dstOffsets[0] = { 0, 0, 0 };
			blitRegion.dstOffsets[1] = { (int32_t)dstW, (int32_t)dstH, 1 };
			vkCmdBlitImage(
				xrVkCommandBuffer->buffer,
				preparedEyeImage.Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				1, &blitRegion, VK_FILTER_LINEAR);
			VRSubmitLayerBlits++;

			VkImageTransition()
				.AddImage(&preparedEyeImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
				.Execute(xrVkCommandBuffer.get());
		}
	}

	VkImageMemoryBarrier dstRestoreBarrier{};
	dstRestoreBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dstRestoreBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstRestoreBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	dstRestoreBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstRestoreBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	dstRestoreBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstRestoreBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstRestoreBarrier.image = dstImage;
	dstRestoreBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, xrViewCount };
	vkCmdPipelineBarrier(
		xrVkCommandBuffer->buffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, nullptr, 0, nullptr, 1, &dstRestoreBarrier);

	xrVkCommandBuffer->end();

	vkResetFences(xrVkDevice->device, 1, &xrVkSubmitFence->fence);
	VkCommandBuffer cmdBuf = xrVkCommandBuffer->buffer;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmdBuf;
	VkResult submitResult = vkQueueSubmit(xrVkDevice->GraphicsQueue, 1, &submitInfo, xrVkSubmitFence->fence);
	if (submitResult != VK_SUCCESS)
	{
		// RS FORK -- a loss noticed here used to vanish into "return false".
		if (submitResult == VK_ERROR_DEVICE_LOST) VulkanDeviceLost("OpenXR swapchain copy: vkQueueSubmit");
		return false;
	}
	VkResult waitResult = VK_SUCCESS;
	{
		Clocker submitWaitTimer(VRSubmitWait);
		waitResult = vkWaitForFences(xrVkDevice->device, 1, &xrVkSubmitFence->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	}
	if (waitResult != VK_SUCCESS)
	{
		if (waitResult == VK_ERROR_DEVICE_LOST) VulkanDeviceLost("OpenXR swapchain copy: vkWaitForFences");
		return false;
	}
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrResult = xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);
	if (XR_FAILED(xrResult))
	{
		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		xrFrameInProgress = false;
		return false;
	}

	XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	layer.space = xrSpace;
	layer.viewCount = xrViewCount;
	layer.views = xrProjectionViews.data();

	for (uint32_t i = 0; i < xrViewCount; ++i)
	{
		const uint32_t viewIndex = (vr_openxr_debug_submit_mode == 2 || vr_openxr_debug_submit_mode == 3) ? 0u : i;
		const uint32_t arrayIndex = i;
		xrProjectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		xrProjectionViews[i].pose = xrViews[viewIndex].pose;
		xrProjectionViews[i].fov = xrViews[viewIndex].fov;
		xrProjectionViews[i].subImage.swapchain = xrSwapchain;
		xrProjectionViews[i].subImage.imageArrayIndex = arrayIndex;
		xrProjectionViews[i].subImage.imageRect.offset = { 0, 0 };
		xrProjectionViews[i].subImage.imageRect.extent = { (int32_t)dstW, (int32_t)dstH };
	}

	XrCompositionLayerQuad backdropLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad quadLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad beamLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	bool submitVirtualScreen = xrVirtualScreenVisible && xrVirtualScreenSwapchain != XR_NULL_HANDLE && xrVirtualScreenImageIndex >= 0;
	bool submitBackdrop = submitVirtualScreen && xrVirtualScreenBackdropVisible && xrVirtualScreenBackdropSwapchain != XR_NULL_HANDLE && xrVirtualScreenBackdropImageIndex >= 0;
	bool submitMenuPointerBeam = submitVirtualScreen &&
		xrMenuPointerBeamVisible &&
		xrMenuPointerBeamSwapchain != XR_NULL_HANDLE &&
		xrMenuPointerBeamImageIndex >= 0 &&
		!xrMenuPointerBeamTextures.empty();

	const XrCompositionLayerBaseHeader* layers[4];
	int layerIndex = 0;
	// Always keep the projection layer alive. The virtual-screen path is an
	// overlay layer, not a replacement for the headset scene. Replacing the
	// projection layer made the whole HMD depend on the menu quad pass and could
	// drop straight into SteamVR's waiting screen if that overlay path failed.
	layers[layerIndex++] = (const XrCompositionLayerBaseHeader*)&layer;

	if (submitVirtualScreen)
	{
		if (submitBackdrop)
		{
			backdropLayer = xrVirtualScreenBackdropLayer;
			backdropLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			backdropLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			backdropLayer.subImage.swapchain = xrVirtualScreenBackdropSwapchain;
			backdropLayer.subImage.imageArrayIndex = 0;
			layers[layerIndex++] = (const XrCompositionLayerBaseHeader*)&backdropLayer;
		}

		quadLayer = xrVirtualScreenLayer;
		quadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quadLayer.subImage.swapchain = xrVirtualScreenSwapchain;
		quadLayer.subImage.imageArrayIndex = 0;
		layers[layerIndex++] = (const XrCompositionLayerBaseHeader*)&quadLayer;

		if (submitMenuPointerBeam)
		{
			beamLayer = xrMenuPointerBeamLayer;
			beamLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			beamLayer.layerFlags = 0;
			beamLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			beamLayer.subImage.swapchain = xrMenuPointerBeamSwapchain;
			beamLayer.subImage.imageArrayIndex = 0;
			layers[layerIndex++] = (const XrCompositionLayerBaseHeader*)&beamLayer;
		}
	}
	XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = xrFrameState.predictedDisplayTime;
	endInfo.environmentBlendMode = environmentBlendMode;
	endInfo.layerCount = layerIndex;
	endInfo.layers = layers;
	XrResult endResult = xrEndFrame(xrSession, &endInfo);
	xrFrameInProgress = false;
	return XR_SUCCEEDED(endResult);
}

void VKOpenXRDeviceMode::Present() const
{
	if (developer > 0)
		Printf("OpenXR: Present called\n");
}

void VKOpenXRDeviceMode::AdjustViewport(DFrameBuffer* screen) const
{
	if (screen == nullptr)
		return;
	if (!mInVRSceneRender)
		return;

	int width = 0;
	int height = 0;
	if (!GetRecommendedRenderSize(width, height))
	{
		VRMode::AdjustViewport(screen);
		return;
	}

	ApplyOpenXRGameplayViewport(screen, width, height);
}

void VKOpenXRDeviceMode::AdjustPlayerSprites(FRenderState& state, int hand) const
{
	if (GetWeaponTransform(&state.mModelMatrix, hand))
	{
		const float scale = 0.00125f * vr_weaponScale * vr_2dweaponScale;
		state.mModelMatrix.scale(scale, -scale, scale);
		state.mModelMatrix.translate(-viewwidth / 2, -viewheight * 3 / 4, 0.0f);

		const float offsetFactor = 40.f;
		state.mModelMatrix.translate(vr_2dweaponOffsetX * offsetFactor, -vr_2dweaponOffsetY * offsetFactor, vr_2dweaponOffsetZ * offsetFactor);
	}
	state.EnableModelMatrix(true);
}

void VKOpenXRDeviceMode::UnAdjustPlayerSprites(FRenderState& state) const
{
	state.EnableModelMatrix(false);
}

void VKOpenXRDeviceMode::DrawMountedHud(HWDrawInfo* di, FRenderState& state) const
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
		0.f, 0.f,
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

bool VKOpenXRDeviceMode::IsRenderingVirtualScreen() const
{
	return mInVirtualScreenRender;
}

void VKOpenXRDeviceMode::updateVirtualScreenLayer() const
{
	if (xrViewCount == 0 || xrViews.size() < xrViewCount)
		return;

	XrVector3f center{ 0.0f, 0.0f, 0.0f };
	for (uint32_t i = 0; i < xrViewCount; ++i)
	{
		center.x += xrViews[i].pose.position.x;
		center.y += xrViews[i].pose.position.y;
		center.z += xrViews[i].pose.position.z;
	}
	center.x /= xrViewCount;
	center.y /= xrViewCount;
	center.z /= xrViewCount;
	const int effectiveOverlayMode = (vr_overlayscreen == 0) ? 2 : vr_overlayscreen;

	const XrQuaternionf headOrientation = GetCenteredViewOrientation(xrViews);
	const XrVector3f headForward = RotateVector(headOrientation, { 0.0f, 0.0f, -1.0f });
	const XrVector3f headUp = RotateVector(headOrientation, { 0.0f, 1.0f, 0.0f });
	const float headYawDeg = YawDegFromForward(headForward);
	if (xrHasPrevHeadSampleForRecenter)
	{
		// Do not treat normal head turning as recenter. Only react to large
		// vertical/discontinuous jumps that indicate runtime origin reset.
		const float heightDelta = fabsf(center.y - xrPrevHeadCenterForRecenter.y);
		const XrVector3f deltaPos = SubtractVector(center, xrPrevHeadCenterForRecenter);
		const float deltaLen = std::sqrt(std::max(0.0f, DotVector(deltaPos, deltaPos)));
		if ((effectiveOverlayMode == 1 || effectiveOverlayMode == 2) && (heightDelta > 0.35f || deltaLen > 0.80f))
		{
			xrStationaryAnchorValid = false;
		}
	}
	xrPrevHeadCenterForRecenter = center;
	xrPrevHeadYawDegForRecenter = headYawDeg;
	xrHasPrevHeadSampleForRecenter = true;

	const float distance = std::max(0.25f, 2.5f + vr_overlayscreen_dist);
	float screenWidth = 0.0f;
	float screenHeight = 0.0f;
	GetOpenXRVirtualScreenMeters(xrVirtualScreenWidth, xrVirtualScreenHeight, screenWidth, screenHeight);
	const XrQuaternionf flipRotation = MakeAxisAngleQuaternion({ 0.0f, 0.0f, 1.0f }, (float)M_PI);
	const XrVector3f worldUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f yawForward = NormalizeVector({ headForward.x, 0.0f, headForward.z });
	const double now = I_msTimeF();

	auto BuildYawUprightPose = [&](const XrVector3f& anchorCenter, const XrVector3f& forwardIn) -> XrPosef
	{
		XrVector3f forward = NormalizeVector({ forwardIn.x, 0.0f, forwardIn.z });
		if (DotVector(forward, forward) < 0.0001f)
			forward = { 0.0f, 0.0f, -1.0f };
		const XrVector3f normal = ScaleVector(forward, -1.0f);
		XrVector3f right = NormalizeVector(CrossVector(worldUp, normal));
		if (DotVector(right, right) < 0.0001f)
			right = { 1.0f, 0.0f, 0.0f };
		const XrVector3f up = NormalizeVector(CrossVector(normal, right));
		const XrVector3f pos = AddVector(AddVector(anchorCenter, ScaleVector(forward, distance)), ScaleVector(worldUp, vr_overlayscreen_vpos));
		XrPosef pose{};
		pose.orientation = QuaternionFromBasis(right, up, normal);
		pose.orientation = MultiplyQuaternion(pose.orientation, flipRotation);
		pose.position = pos;
		return pose;
	};

	switch (effectiveOverlayMode)
	{
	case 1: // Stationary
		if (!xrStationaryAnchorValid || xrStationaryAnchorMode != effectiveOverlayMode)
		{
			xrStationaryAnchorPose = BuildYawUprightPose(center, yawForward);
			xrStationaryAnchorValid = true;
			xrStationaryAnchorMode = effectiveOverlayMode;
		}
		xrVirtualScreenPose = xrStationaryAnchorPose;
		break;
	case 2: // Stationary (follow)
		if (!xrStationaryAnchorValid || xrStationaryAnchorMode != effectiveOverlayMode)
		{
			xrStationaryFollowCurrentPose = BuildYawUprightPose(center, yawForward);
			xrStationaryFollowTargetPose = xrStationaryFollowCurrentPose;
			xrStationaryFollowNextTargetTimeMs = now + 1000.0;
			xrStationaryFollowLastStepTimeMs = now;
			xrStationaryAnchorValid = true;
			xrStationaryAnchorMode = effectiveOverlayMode;
		}
		if (now >= xrStationaryFollowNextTargetTimeMs)
		{
			const XrPosef candidateTarget = BuildYawUprightPose(center, yawForward);
			const float oldYaw = YawDegFromForward(RotateVector(xrStationaryFollowTargetPose.orientation, { 0.0f, 0.0f, -1.0f }));
			const float newYaw = YawDegFromForward(RotateVector(candidateTarget.orientation, { 0.0f, 0.0f, -1.0f }));
			if (fabsf(ShortestAngleDeltaDeg(newYaw, oldYaw)) >= 15.0f)
				xrStationaryFollowTargetPose = candidateTarget;
			xrStationaryFollowNextTargetTimeMs = now + 1000.0;
		}
		{
			const float dt = (float)clamp((now - xrStationaryFollowLastStepTimeMs) / 1000.0, 0.0, 0.1);
			xrStationaryFollowLastStepTimeMs = now;
			const float step = clamp(dt * 1.1f, 0.0f, 1.0f);
			const float eased = 1.0f - powf(1.0f - step, 3.0f);
			xrStationaryFollowCurrentPose.position = AddVector(
				xrStationaryFollowCurrentPose.position,
				ScaleVector(SubtractVector(xrStationaryFollowTargetPose.position, xrStationaryFollowCurrentPose.position), eased));
			const XrVector3f currentForward = RotateVector(xrStationaryFollowCurrentPose.orientation, { 0.0f, 0.0f, -1.0f });
			const XrVector3f targetForward = RotateVector(xrStationaryFollowTargetPose.orientation, { 0.0f, 0.0f, -1.0f });
			const XrVector3f blendedForward = NormalizeVector(AddVector(ScaleVector(currentForward, 1.0f - eased), ScaleVector(targetForward, eased)));
			XrPosef orientedPose = BuildYawUprightPose(center, blendedForward);
			orientedPose.position = xrStationaryFollowCurrentPose.position;
			xrStationaryFollowCurrentPose = orientedPose;
			xrVirtualScreenPose = xrStationaryFollowCurrentPose;
		}
		break;
	case 4: // Follow Main Hand
	case 5: // Follow Offhand
	{
		const int hand = (effectiveOverlayMode == 4) ? 1 : 0;
		if (hand >= 0 && hand < 2 && xrHandPoseValid[hand])
		{
			const XrQuaternionf handOrientation = xrHandPoses[hand].orientation;
			const XrVector3f handForward = RotateVector(handOrientation, { 0.0f, 0.0f, -1.0f });
			// Keep controller-follow overlay upright (no roll), like OpenVR overlay behavior.
			xrVirtualScreenPose = BuildYawUprightPose(xrHandPoses[hand].position, handForward);
		}
		else
		{
			xrVirtualScreenPose.orientation = MultiplyQuaternion(headOrientation, flipRotation);
			xrVirtualScreenPose.position = AddVector(center, ScaleVector(headForward, distance));
			xrVirtualScreenPose.position = AddVector(xrVirtualScreenPose.position, ScaleVector(headUp, vr_overlayscreen_vpos));
		}
		break;
	}
	case 3: // Follow Head movement
	default:
		xrVirtualScreenPose.orientation = MultiplyQuaternion(headOrientation, flipRotation);
		xrVirtualScreenPose.position = AddVector(center, ScaleVector(headForward, distance));
		xrVirtualScreenPose.position = AddVector(xrVirtualScreenPose.position, ScaleVector(headUp, vr_overlayscreen_vpos));
		break;
	}

	xrVirtualScreenLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	xrVirtualScreenLayer.layerFlags = 0;
	xrVirtualScreenLayer.space = xrSpace;
	xrVirtualScreenLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	xrVirtualScreenLayer.pose = xrVirtualScreenPose;
	xrVirtualScreenLayer.size = { screenWidth, screenHeight };
	xrVirtualScreenLayer.subImage.swapchain = xrVirtualScreenSwapchain;
	xrVirtualScreenLayer.subImage.imageArrayIndex = 0;
	xrVirtualScreenLayer.subImage.imageRect.offset = { 0, 0 };
	xrVirtualScreenLayer.subImage.imageRect.extent = { (int32_t)xrVirtualScreenWidth, (int32_t)xrVirtualScreenHeight };

	xrVirtualScreenBackdropPose.orientation = MultiplyQuaternion(headOrientation, flipRotation);
	xrVirtualScreenBackdropPose.position = AddVector(center, ScaleVector(headForward, distance + 0.15f));
	xrVirtualScreenBackdropLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	xrVirtualScreenBackdropLayer.layerFlags = 0;
	xrVirtualScreenBackdropLayer.space = xrSpace;
	xrVirtualScreenBackdropLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	xrVirtualScreenBackdropLayer.pose = xrVirtualScreenBackdropPose;
	xrVirtualScreenBackdropLayer.size = { screenWidth * 6.0f, screenHeight * 6.0f };
	xrVirtualScreenBackdropLayer.subImage.swapchain = xrVirtualScreenBackdropSwapchain;
	xrVirtualScreenBackdropLayer.subImage.imageArrayIndex = 0;
	xrVirtualScreenBackdropLayer.subImage.imageRect.offset = { 0, 0 };
	xrVirtualScreenBackdropLayer.subImage.imageRect.extent = { (int32_t)xrVirtualScreenWidth, (int32_t)xrVirtualScreenHeight };

	if (xrHasEquirectBackdrop)
	{
		xrVirtualScreenBackdropEquirectLayer.type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR;
		xrVirtualScreenBackdropEquirectLayer.layerFlags = 0;
		xrVirtualScreenBackdropEquirectLayer.space = xrSpace;
		xrVirtualScreenBackdropEquirectLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		xrVirtualScreenBackdropEquirectLayer.pose = xrVirtualScreenBackdropPose;
		xrVirtualScreenBackdropEquirectLayer.radius = 0.0f;
		xrVirtualScreenBackdropEquirectLayer.scale = { 1.0f, 1.0f };
		xrVirtualScreenBackdropEquirectLayer.bias = { 0.0f, 0.0f };
		xrVirtualScreenBackdropEquirectLayer.subImage.swapchain = xrVirtualScreenBackdropSwapchain;
		xrVirtualScreenBackdropEquirectLayer.subImage.imageArrayIndex = 0;
		xrVirtualScreenBackdropEquirectLayer.subImage.imageRect.offset = { 0, 0 };
		xrVirtualScreenBackdropEquirectLayer.subImage.imageRect.extent = { (int32_t)xrVirtualScreenWidth, (int32_t)xrVirtualScreenHeight };
	}

}

bool VKOpenXRDeviceMode::ShouldRenderVirtualScreen() const
{
	const bool renderNetWaitShell = IsNetWaitShellActive();
	const int effectiveOverlayMode = (vr_overlayscreen == 0) ? 2 : vr_overlayscreen;
	const bool overlayEnabled = renderNetWaitShell || (effectiveOverlayMode > 0) || vr_overlayscreen_always;
	return ShouldUseScreenLayerForCurrentFrame() &&
		(renderNetWaitShell || gamestate != GS_LEVEL || menuactive != MENU_Off || cinemamode || ConsoleState != c_up || vr_overlayscreen_always) &&
		overlayEnabled;
}

bool VKOpenXRDeviceMode::RenderVirtualScreen() const
{
	auto* vkfb = dynamic_cast<VulkanRenderDevice*>(screen);
	const bool renderNetWaitShell = IsNetWaitShellActive();
	if (!vkfb || !xrFrameInProgress || xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrVkCommandBuffer == nullptr)
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}
	if (!ShouldRenderVirtualScreen())
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}
	const int effectiveOverlayMode = (vr_overlayscreen == 0) ? 2 : vr_overlayscreen;
	if ((effectiveOverlayMode == 1 || effectiveOverlayMode == 2) && !xrVirtualScreenWasVisibleLastFrame)
	{
		xrStationaryAnchorValid = false;
	}
	xrVirtualScreenWasVisibleLastFrame = true;

	// Treat titlemap/titlelevel like an in-level scene for composition purposes.
	// It still uses virtual-screen mode, but should not inherit the blank overlay.
	const bool forceOverlay = renderNetWaitShell || !IsLevelSceneState() || menuactive != MENU_Off || cinemamode || ConsoleState != c_up || vr_overlayscreen_always;
	const bool allowBlankOverlay = renderNetWaitShell || vr_overlayscreen_always || cinemamode || !IsLevelSceneState();
	xrMenuPointerBeamImageIndex = -1;
	if (twod == nullptr || (twod->DrawCount() == 0 && !allowBlankOverlay && !forceOverlay))
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenImageIndex = -1;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}

	uint32_t screenWidth = 0;
	uint32_t screenHeight = 0;
	GetStableOpenXRVirtualScreenSize(screenWidth, screenHeight);
	if (!CreateVirtualScreenSwapchain(screenWidth, screenHeight))
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}
	if (!CreateVirtualScreenBackdropSwapchain(screenWidth, screenHeight))
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}
	if (!CreateMenuPointerBeamSwapchain())
	{
		xrMenuPointerBeamVisible = false;
	}
	if (xrVirtualScreenSwapchain == XR_NULL_HANDLE || xrVirtualScreenTextures.empty())
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}
	if (xrVirtualScreenBackdropSwapchain == XR_NULL_HANDLE || xrVirtualScreenBackdropTextures.empty())
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}

	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t imageIndex = 0;
	XrResult xrResult = xrAcquireSwapchainImage(xrVirtualScreenSwapchain, &acquireInfo, &imageIndex);
	if (XR_FAILED(xrResult))
	{
		Printf("OpenXR: virtual screen acquire failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}

	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 100 * 1000 * 1000;
	xrResult = xrWaitSwapchainImage(xrVirtualScreenSwapchain, &waitInfo);
	if (XR_FAILED(xrResult))
	{
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrVirtualScreenSwapchain, &releaseInfo);
		Printf("OpenXR: virtual screen wait failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}

	xrVirtualScreenImageIndex = (int)imageIndex;
	auto& target = xrVirtualScreenTextures[imageIndex];
	// vr_menu_keep_world: the flat copy of the scene behind the menu is the "2D
	// collapse". In MenuOverWorld the real world is in the eyes, so the quad is
	// just the menu on its plain background panel.
	const bool useSceneBackdrop = IsLevelSceneState() && !renderNetWaitShell && mFrameRenderMode != FrameRenderMode::MenuOverWorld;
	if (useSceneBackdrop)
	{
		vkfb->GetPostprocess()->BlitCurrentToImage(&target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		VkImageTransition()
			.AddImage(&target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
			.Execute(vkfb->GetCommands()->GetDrawCommands());
	}
	else
	{
		VkImageTransition()
			.AddImage(&target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
			.Execute(vkfb->GetCommands()->GetDrawCommands());
	}

	float savedClear[4];
	memcpy(savedClear, screen->mSceneClearColor, sizeof(savedClear));
	if (useSceneBackdrop)
	{
		screen->mSceneClearColor[0] = 0.0f;
		screen->mSceneClearColor[1] = 0.0f;
		screen->mSceneClearColor[2] = 0.0f;
		screen->mSceneClearColor[3] = 0.0f;
	}
	else
	{
		const XrVector3f bg = renderNetWaitShell ? XrVector3f{ 0.0f, 0.0f, 0.0f } : GetVirtualScreenBackgroundColor();
		screen->mSceneClearColor[0] = bg.x;
		screen->mSceneClearColor[1] = bg.y;
		screen->mSceneClearColor[2] = bg.z;
		screen->mSceneClearColor[3] = 1.0f;
	}

	auto* renderState = vkfb->GetRenderState();
	const IntRect savedScreenViewport = vkfb->mScreenViewport;
	const IntRect savedSceneViewport = vkfb->mSceneViewport;
	const IntRect savedOutputLetterbox = vkfb->mOutputLetterbox;
	const int savedGameScreenWidth = vkfb->mGameScreenWidth;
	const int savedGameScreenHeight = vkfb->mGameScreenHeight;
	vkfb->mScreenViewport = { 0, 0, (int)screenWidth, (int)screenHeight };
	vkfb->mSceneViewport = vkfb->mScreenViewport;
	vkfb->mOutputLetterbox = vkfb->mScreenViewport;
	vkfb->mGameScreenWidth = (int)screenWidth;
	vkfb->mGameScreenHeight = (int)screenHeight;
	renderState->SetRenderTarget(&target, nullptr, (int)screenWidth, (int)screenHeight, (VkFormat)xrVirtualScreenSwapchainFormat, VK_SAMPLE_COUNT_1_BIT);
	// Render the virtual-screen texture as a regular 2D target. The VR layer
	// compositor will handle the actual head-locked presentation.
	screen->mViewpoints->Set2D(*renderState, (int)screenWidth, (int)screenHeight);
	mInVirtualScreenRender = true;
	if (!useSceneBackdrop)
	{
		renderState->Clear(CT_Color);
	}
	if (renderNetWaitShell)
	{
		VR_RenderNetWaitShellContents((int)screenWidth, (int)screenHeight, false);
		screen->Draw2D(false);
		twod->Clear();
	}
	else if (twod != nullptr && twod->HasCommandsForPass(true))
	{
		screen->Draw2D(true);
	}
	if (!renderNetWaitShell && xrMenuPointerActive && xrMenuPointerHasHit && twod != nullptr)
	{
		const float beamEndX = xrMenuPointerX;
		const float beamEndY = xrMenuPointerY;
		const PalEntry cursorColor = PalEntry(vr_menu_pointer_color);
		// twod line colors are packed as AABBGGRR.
		const uint32_t cursorLineColor =
			0xFF000000u |
			((uint32_t)cursorColor.b << 16) |
			((uint32_t)cursorColor.g << 8) |
			(uint32_t)cursorColor.r;
		const float cursorRadius = 8.0f;
		const int cursorSegments = 16;
		for (int i = 0; i < cursorSegments; ++i)
		{
			const float a0 = (float)(2.0 * M_PI * (double)i / (double)cursorSegments);
			const float a1 = (float)(2.0 * M_PI * (double)(i + 1) / (double)cursorSegments);
			const DVector2 p0(beamEndX + std::cos(a0) * cursorRadius, beamEndY + std::sin(a0) * cursorRadius);
			const DVector2 p1(beamEndX + std::cos(a1) * cursorRadius, beamEndY + std::sin(a1) * cursorRadius);
			twod->AddThickLine(p0, p1, 3.0, cursorLineColor, 255);
		}
		twod->AddColorOnlyQuad((int)(beamEndX - 3.0f), (int)(beamEndY - 3.0f), 6, 6, PalEntry(255, cursorColor.r, cursorColor.g, cursorColor.b));
	}
	if (!renderNetWaitShell && twod != nullptr && twod->HasCommandsForPass(false))
	{
		screen->Draw2D(false);
	}
	mInVirtualScreenRender = false;
	renderState->EndRenderPass();
	vkfb->mScreenViewport = savedScreenViewport;
	vkfb->mSceneViewport = savedSceneViewport;
	vkfb->mOutputLetterbox = savedOutputLetterbox;
	vkfb->mGameScreenWidth = savedGameScreenWidth;
	vkfb->mGameScreenHeight = savedGameScreenHeight;

	memcpy(screen->mSceneClearColor, savedClear, sizeof(savedClear));

	auto* cmdbuffer = vkfb->GetCommands()->GetDrawCommands();
	auto& bounce = vkfb->GetBuffers()->PipelineImage[0];
	auto* postprocess = vkfb->GetPostprocess();
	const int32_t targetWidth = target.Image != nullptr ? target.Image->width : 0;
	const int32_t targetHeight = target.Image != nullptr ? target.Image->height : 0;
	const int32_t bounceWidth = bounce.Image != nullptr ? bounce.Image->width : 0;
	const int32_t bounceHeight = bounce.Image != nullptr ? bounce.Image->height : 0;

	// Run the same OpenXR headset-facing present bias used by the eye images so
	// the virtual-screen quad can share the tuned gamma/contrast/brightness/
	// saturation response instead of relying on desktop-style flat output.
	if (postprocess != nullptr && targetWidth > 0 && targetHeight > 0 && bounceWidth > 0 && bounceHeight > 0)
	{
		VkImageTransition()
			.AddImage(&target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.AddImage(&bounce, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
			.Execute(cmdbuffer);

		VkImageBlit copyToPipeline = {};
		copyToPipeline.srcOffsets[0] = { 0, 0, 0 };
		copyToPipeline.srcOffsets[1] = { targetWidth, targetHeight, 1 };
		copyToPipeline.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copyToPipeline.dstOffsets[0] = { 0, 0, 0 };
		copyToPipeline.dstOffsets[1] = { bounceWidth, bounceHeight, 1 };
		copyToPipeline.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		cmdbuffer->blitImage(target.Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			bounce.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &copyToPipeline, VK_FILTER_NEAREST);

		VkImageTransition()
			.AddImage(&bounce, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
			.Execute(cmdbuffer);

		const int previousPipelineImage = postprocess->GetCurrentPipelineImage();
		postprocess->SetCurrentPipelineImage(0);
		IntRect fullTargetRect = { 0, 0, targetWidth, targetHeight };
		postprocess->DrawPresentTextureToImage(
			&target,
			(VkFormat)xrVirtualScreenSwapchainFormat,
			fullTargetRect,
			true,
			false,
			1.0f,
			1.0f,
			0.0f,
			0.0f,
			cmdbuffer,
			true);
		postprocess->SetCurrentPipelineImage(previousPipelineImage);
	}

	VkImageTransition()
		.AddImage(&target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.AddImage(&bounce, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
		.Execute(cmdbuffer);
	VkImageBlit blit = {};
	blit.srcOffsets[0] = { 0, 0, 0 };
	blit.srcOffsets[1] = { targetWidth, targetHeight, 1 };
	blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	blit.dstOffsets[0] = { bounceWidth, 0, 0 };
	blit.dstOffsets[1] = { 0, bounceHeight, 1 };
	blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	cmdbuffer->blitImage(target.Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		bounce.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_NEAREST);
	VkImageTransition()
		.AddImage(&bounce, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.AddImage(&target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
		.Execute(cmdbuffer);
	VkImageBlit blitBack = {};
	blitBack.srcOffsets[0] = { 0, 0, 0 };
	blitBack.srcOffsets[1] = { bounceWidth, bounceHeight, 1 };
	blitBack.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	blitBack.dstOffsets[0] = { 0, 0, 0 };
	blitBack.dstOffsets[1] = { targetWidth, targetHeight, 1 };
	blitBack.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	cmdbuffer->blitImage(bounce.Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		target.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blitBack, VK_FILTER_NEAREST);
	VkImageTransition()
		.AddImage(&target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.AddImage(&bounce, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(cmdbuffer);

	XrSwapchainImageAcquireInfo backdropAcquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t backdropIndex = 0;
	xrResult = xrAcquireSwapchainImage(xrVirtualScreenBackdropSwapchain, &backdropAcquireInfo, &backdropIndex);
	if (XR_FAILED(xrResult))
	{
		Printf("OpenXR: virtual screen backdrop acquire failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	XrSwapchainImageWaitInfo backdropWaitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	backdropWaitInfo.timeout = 100 * 1000 * 1000;
	xrResult = xrWaitSwapchainImage(xrVirtualScreenBackdropSwapchain, &backdropWaitInfo);
	if (XR_FAILED(xrResult))
	{
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrVirtualScreenBackdropSwapchain, &releaseInfo);
		Printf("OpenXR: virtual screen backdrop wait failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	xrVirtualScreenBackdropImageIndex = (int)backdropIndex;
	auto& backdropTarget = xrVirtualScreenBackdropTextures[backdropIndex];

	VkImageTransition()
		.AddImage(&backdropTarget, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
		.Execute(vkfb->GetCommands()->GetDrawCommands());

	const XrVector3f backdropColor =
		(cinemamode || vr_overlayscreen_always || IsLevelSceneState())
		? GetVirtualScreenBackdropColor()
		: XrVector3f{ 0.0f, 0.0f, 0.0f };
	VkClearColorValue backdropColorValue = {};
	backdropColorValue.float32[0] = backdropColor.x;
	backdropColorValue.float32[1] = backdropColor.y;
	backdropColorValue.float32[2] = backdropColor.z;
	backdropColorValue.float32[3] = 1.0f;
	VkImageSubresourceRange backdropRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkfb->GetCommands()->GetDrawCommands()->clearColorImage(
		backdropTarget.Image->image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		&backdropColorValue,
		1,
		&backdropRange);

	VkImageTransition()
		.AddImage(&backdropTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(vkfb->GetCommands()->GetDrawCommands());

	if (xrMenuPointerBeamVisible && xrMenuPointerBeamSwapchain != XR_NULL_HANDLE && !xrMenuPointerBeamTextures.empty())
	{
		XrSwapchainImageAcquireInfo beamAcquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t beamIndex = 0;
		xrResult = xrAcquireSwapchainImage(xrMenuPointerBeamSwapchain, &beamAcquireInfo, &beamIndex);
		if (XR_SUCCEEDED(xrResult))
		{
			XrSwapchainImageWaitInfo beamWaitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			beamWaitInfo.timeout = 100 * 1000 * 1000;
			xrResult = xrWaitSwapchainImage(xrMenuPointerBeamSwapchain, &beamWaitInfo);
			if (XR_SUCCEEDED(xrResult))
			{
				xrMenuPointerBeamImageIndex = (int)beamIndex;
				auto& beamTarget = xrMenuPointerBeamTextures[beamIndex];

				VkImageTransition()
					.AddImage(&beamTarget, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
					.Execute(vkfb->GetCommands()->GetDrawCommands());

				const int beamColorRaw = (int)vr_menu_pointer_color;
				VkClearColorValue beamColorValue = {};
				beamColorValue.float32[0] = RPART(beamColorRaw) / 255.0f;
				beamColorValue.float32[1] = GPART(beamColorRaw) / 255.0f;
				beamColorValue.float32[2] = BPART(beamColorRaw) / 255.0f;
				beamColorValue.float32[3] = 1.0f;
				VkImageSubresourceRange beamRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkfb->GetCommands()->GetDrawCommands()->clearColorImage(
					beamTarget.Image->image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					&beamColorValue,
					1,
					&beamRange);

				VkImageTransition()
					.AddImage(&beamTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
					.Execute(vkfb->GetCommands()->GetDrawCommands());

				xrMenuPointerBeamLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				xrMenuPointerBeamLayer.layerFlags = 0;
				xrMenuPointerBeamLayer.space = xrSpace;
				xrMenuPointerBeamLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				xrMenuPointerBeamLayer.pose = xrMenuPointerBeamPose;
				xrMenuPointerBeamLayer.size = { std::max(0.02f, xrMenuPointerBeamLength), 0.005f };
				xrMenuPointerBeamLayer.subImage.swapchain = xrMenuPointerBeamSwapchain;
				xrMenuPointerBeamLayer.subImage.imageArrayIndex = 0;
				xrMenuPointerBeamLayer.subImage.imageRect.offset = { 0, 0 };
				xrMenuPointerBeamLayer.subImage.imageRect.extent = { beamTarget.Image->width, beamTarget.Image->height };
			}

			XrSwapchainImageReleaseInfo beamReleaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(xrMenuPointerBeamSwapchain, &beamReleaseInfo);
		}
	}

	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrResult = xrReleaseSwapchainImage(xrVirtualScreenBackdropSwapchain, &releaseInfo);
	if (XR_FAILED(xrResult))
	{
		Printf("OpenXR: virtual screen backdrop release failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrResult = xrReleaseSwapchainImage(xrVirtualScreenSwapchain, &releaseInfo);
	if (XR_FAILED(xrResult))
	{
		Printf("OpenXR: virtual screen release failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	updateVirtualScreenLayer();
	xrVirtualScreenVisible = true;
	// vr_menu_keep_world: the backdrop is an opaque quad 6x the menu's size right
	// behind it -- a wall over the world. Not submitted (or mirrored) in
	// MenuOverWorld; the image above is still acquired/released as before.
	xrVirtualScreenBackdropVisible = mFrameRenderMode != FrameRenderMode::MenuOverWorld;
	return true;
}

static int SpectatorEyeIndex();	// [SPECTATOR], below

void VKOpenXRDeviceMode::FinalizeEyeImage(VulkanRenderDevice* vkfb, int eyeIndex) const
{
	if (!vkfb || eyeIndex < 0 || xrSession == XR_NULL_HANDLE || xrSwapchainFormat == VK_FORMAT_UNDEFINED)
		return;
	if ((uint32_t)eyeIndex >= xrViewCount)
		return;
	if (!CreatePresentTextures(vkfb))
		return;

	auto* postprocess = vkfb->GetPostprocess();
	if (!postprocess)
		return;
	ScopedCycleTimer cycle(VRFinalizeEye);
	const int pipelineImageIndex = postprocess->GetCurrentPipelineImage();
	const XrSafeSourceRect sourceRect = GetSafeXrSourceRect(vkfb);
	postprocess->SetCurrentPipelineImage(pipelineImageIndex);

	IntRect targetBox;
	targetBox.left = 0;
	targetBox.top = 0;
	targetBox.width = (int)xrPresentWidth;
	targetBox.height = (int)xrPresentHeight;

	// 1) Unbiased image for desktop mirror parity.
	if (ShouldPrepareDesktopMirrorEye(eyeIndex) && eyeIndex >= 0 && eyeIndex < (int)xrMirrorPresentTextures.size())
	{
		Clocker mirrorPrepareTimer(VRFinalPresent);
		VRMirrorPreparePasses++;
		postprocess->DrawPresentTextureToImage(
			&xrMirrorPresentTextures[eyeIndex],
			(VkFormat)xrSwapchainFormat,
			targetBox,
			true,
			false,
			sourceRect.scaleX,
			sourceRect.scaleY,
			sourceRect.offsetX,
			sourceRect.offsetY,
			vkfb->GetCommands()->GetDrawCommands(),
			false);
	}

	// [SPECTATOR] The stabilized desktop view, cut from the same finished eye.
	if (vr_spectator > 0 && vr_desktop_view != -1 && eyeIndex == SpectatorEyeIndex())
	{
		xrSpectatorFrameReady = RenderSpectatorEye(vkfb, eyeIndex,
			sourceRect.scaleX, sourceRect.scaleY, sourceRect.offsetX, sourceRect.offsetY);
	}

	// 2) XR-submitted image with OpenXR bias knobs applied.
	Clocker finalPresentTimer(VRFinalPresent);
	VRFinalPresentPasses++;
	postprocess->DrawPresentTextureToImage(
		&xrPresentTextures[eyeIndex],
		(VkFormat)xrSwapchainFormat,
		targetBox,
		true,
		false,
		sourceRect.scaleX,
		sourceRect.scaleY,
		sourceRect.offsetX,
		sourceRect.offsetY,
		vkfb->GetCommands()->GetDrawCommands(),
		true);
}

// ---- [SPECTATOR] THE STABILIZED DESKTOP VIEW ----------------------------------
//
// A headset mirror is unwatchable because it moves exactly as the head does:
// every micro-correction, every tilt, every quick glance. vr_spectator replaces
// it with a calmer camera that follows the head at a distance.
//
// It is not a third render. The eye is already drawn, over a wider field than a
// window needs, so the spectator view is CUT from it: each window pixel's ray is
// built in a smoothed camera and turned into the eye's camera, and the eye image
// is read where it lands (present.fp, SPECTATOR_REPROJECT). A pure rotation
// reprojects a perspective image exactly, so this costs one fullscreen pass.
//
// The price is that the smoothed camera can only lag the head by as much margin
// as the eye image has around the window's frustum. When it would fall outside,
// it is pulled toward the head just far enough to fit -- a fast turn still pans,
// just with the jitter taken out -- and a jump past vr_spectator_cutangle (snap
// turn, teleport) is a cut. Position is not smoothed: moving the camera would
// need depth.

// The eye the plain mirror shows: vr_desktop_view 2 is the right eye, 1 and
// side-by-side use the left, and vr_swap_eyes swaps them as it does for the mirror.
static int SpectatorEyeIndex()
{
	const int eye = (vr_desktop_view == 2) ? 1 : 0;
	return vr_swap_eyes ? 1 - eye : eye;
}

// The rotation part of HWDrawInfo::SetViewMatrix for these HWAngles (no mirror),
// as a row-major 3x3: world (after SetViewMatrix's axis scale) -> view space.
static void SpectatorViewRotation(double yaw, double pitch, double roll, double out[9])
{
	VSMatrix m(0);
	m.rotate((FLOATTYPE)roll, 0.0f, 0.0f, 1.0f);
	m.rotate((FLOATTYPE)pitch, 1.0f, 0.0f, 0.0f);
	m.rotate((FLOATTYPE)yaw, 0.0f, 1.0f, 0.0f);
	const FLOATTYPE* a = m.get();	// column-major
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			out[r * 3 + c] = (double)a[c * 4 + r];
}

// eyeView * transpose(specView): a spectator view-space direction -> eye view space.
static void SpectatorRelativeRotation(const double eyeView[9], double yaw, double pitch, double roll, double out[9])
{
	double spec[9];
	SpectatorViewRotation(yaw, pitch, roll, spec);
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			out[r * 3 + c] = eyeView[r * 3 + 0] * spec[c * 3 + 0]
				+ eyeView[r * 3 + 1] * spec[c * 3 + 1]
				+ eyeView[r * 3 + 2] * spec[c * 3 + 2];
}

// Does the spectator frustum (dst) land entirely inside the eye's (src)? The map
// between the two tangent planes is projective, so it keeps straight edges straight
// and the rectangle convex: the four corners decide.
static bool SpectatorFrustumFits(const double rot[9], const double dst[4], const double src[4])
{
	const double insetX = 0.004 * (src[1] - src[0]);
	const double insetY = 0.004 * (src[3] - src[2]);
	for (int i = 0; i < 4; ++i)
	{
		const double x = (i & 1) ? dst[1] : dst[0];
		const double y = (i & 2) ? dst[3] : dst[2];
		const double ex = rot[0] * x + rot[1] * y - rot[2];
		const double ey = rot[3] * x + rot[4] * y - rot[5];
		const double ez = rot[6] * x + rot[7] * y - rot[8];
		if (-ez < 1e-3)
			return false;
		const double tx = ex / -ez;
		const double ty = ey / -ez;
		if (tx < src[0] + insetX || tx > src[1] - insetX || ty < src[2] + insetY || ty > src[3] - insetY)
			return false;
	}
	return true;
}

bool VKOpenXRDeviceMode::EnsureSpectatorTexture(VulkanRenderDevice* vkfb, int width, int height) const
{
	if (xrSpectatorTextures.size() == 1 && xrSpectatorTextures[0].Image != nullptr &&
		(int)xrSpectatorTextures[0].Image->width == width &&
		(int)xrSpectatorTextures[0].Image->height == height)
		return true;

	if (xrSpectatorTextures.empty())
		xrSpectatorTextures.resize(1);
	auto& texture = xrSpectatorTextures[0];
	texture.Reset(vkfb);	// the old one, if any, goes through the draw delete list

	const VkFormat format = (VkFormat)xrSwapchainFormat;
	texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
	texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	texture.Image = ImageBuilder()
		.Format(format)
		.Size(width, height)
		.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
		.DebugName("OpenXRSpectatorTexture")
		.Create(vkfb->device.get());
	if (texture.Image == nullptr)
		return false;
	texture.View = ImageViewBuilder()
		.Image(texture.Image.get(), format)
		.DebugName("OpenXRSpectatorTextureView")
		.Create(vkfb->device.get());
	return texture.View != nullptr;
}

bool VKOpenXRDeviceMode::RenderSpectatorEye(VulkanRenderDevice* vkfb, int eyeIndex, float sourceScaleX, float sourceScaleY, float sourceOffsetX, float sourceOffsetY) const
{
	// Only frames that show the world. The flat menu screen and the cinema layer
	// keep the plain mirror, and the camera starts fresh when the world comes back.
	auto* postprocess = vkfb ? vkfb->GetPostprocess() : nullptr;
	if (postprocess == nullptr || mFrameRenderMode == FrameRenderMode::VirtualScreen ||
		eyeIndex < 0 || (size_t)eyeIndex >= xrViews.size() || xrSwapchainFormat == VK_FORMAT_UNDEFINED)
	{
		xrSpectatorStabilizer.Reset();
		return false;
	}

	const IntRect window = vkfb->mOutputLetterbox;
	const int width = std::min(window.width, 4096);
	const int height = std::min(window.height, 4096);
	if (width < 16 || height < 16 || !EnsureSpectatorTexture(vkfb, width, height))
		return false;

	// The eye as it was drawn. Its frustum, after the fov adjust...
	const XrFovf fov = AdjustOpenXREyeFov(xrViews[(size_t)eyeIndex].fov);
	const double src[4] = { std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleDown), std::tan(fov.angleUp) };
	if (!(src[1] > src[0] && src[3] > src[2]))
		return false;

	// ...and its view: the HWAngles SetViewMatrix builds it from. Yaw as
	// FRenderViewpoint::SetViewAngle derives it from the heading updateHmdPose set;
	// pitch and roll as updateHmdPose left them (both eyes share the centred head
	// orientation -- VKOpenXRDeviceEyePose::AdjustViewpointUniforms).
	const double eyeYaw = 270.0 - r_viewpoint.Angles.Yaw.Degrees();
	const double eyePitch = r_viewpoint.HWAngles.Pitch.Degrees();
	const double eyeRoll = r_viewpoint.HWAngles.Roll.Degrees();
	double eyeView[9];
	SpectatorViewRotation(eyeYaw, eyePitch, eyeRoll, eyeView);

	// The window's frustum: the window's shape, never wider than the eye, and
	// CENTRED IN THE EYE'S FRUSTUM rather than on its axis. A headset eye is
	// asymmetric (a Quest 3 left eye runs about 52 degrees out and 43 in), so a
	// window centred on the axis would have three times the room to lag on one
	// side as on the other; centred in the frustum it has the same on both, which
	// is what the smoothing spends. The view sits a few degrees toward that eye's
	// outer side, which nobody watching can see.
	const double aspect = (double)height / (double)width;
	const double centreX = (src[0] + src[1]) * 0.5;
	const double centreY = (src[2] + src[3]) * 0.5;
	double tanH = std::tan(DEG2RAD(clamp<double>(vr_spectator_fov, 20.0, 150.0)) * 0.5);
	const double maxH = (src[1] - src[0]) * 0.5 * 0.97;
	const double maxV = (src[3] - src[2]) * 0.5 * 0.97;
	tanH = std::min(tanH, maxH);
	if (tanH * aspect > maxV)
		tanH = maxV / aspect;
	const double dst[4] = { centreX - tanH, centreX + tanH, centreY - tanH * aspect, centreY + tanH * aspect };

	// Smooth, then make sure the smoothed view is still inside what the eye drew.
	FVRViewStabilizer& stab = xrSpectatorStabilizer;
	stab.Update(eyeYaw, eyePitch, eyeRoll, I_nsTime());

	auto lerpYaw = [](double from, double to, double t)
	{
		double d = std::fmod(to - from, 360.0);
		if (d > 180.0) d -= 360.0;
		if (d < -180.0) d += 360.0;
		return from + d * t;
	};

	double rot[9];
	SpectatorRelativeRotation(eyeView, stab.Yaw, stab.Pitch, stab.Roll, rot);
	if (!SpectatorFrustumFits(rot, dst, src))
	{
		double yaw = stab.Yaw, pitch = stab.Pitch, roll = stab.Roll;
		SpectatorRelativeRotation(eyeView, eyeYaw, eyePitch, roll, rot);
		if (SpectatorFrustumFits(rot, dst, src))
		{
			// Pull heading and pitch toward the head only as far as it takes.
			double lo = 0.0, hi = 1.0;
			for (int i = 0; i < 10; ++i)
			{
				const double t = (lo + hi) * 0.5;
				SpectatorRelativeRotation(eyeView, lerpYaw(stab.Yaw, eyeYaw, t), stab.Pitch + (eyePitch - stab.Pitch) * t, roll, rot);
				if (SpectatorFrustumFits(rot, dst, src)) hi = t; else lo = t;
			}
			yaw = lerpYaw(stab.Yaw, eyeYaw, hi);
			pitch = stab.Pitch + (eyePitch - stab.Pitch) * hi;
		}
		else
		{
			// Even centred, the level horizon does not fit (a hard head tilt with a wide
			// window): give back as much of the tilt as it takes.
			yaw = eyeYaw;
			pitch = eyePitch;
			double lo = 0.0, hi = 1.0;
			for (int i = 0; i < 10; ++i)
			{
				const double t = (lo + hi) * 0.5;
				SpectatorRelativeRotation(eyeView, yaw, pitch, stab.Roll + (eyeRoll - stab.Roll) * t, rot);
				if (SpectatorFrustumFits(rot, dst, src)) hi = t; else lo = t;
			}
			roll = stab.Roll + (eyeRoll - stab.Roll) * hi;
		}
		stab.Force(yaw, pitch, roll);
		SpectatorRelativeRotation(eyeView, yaw, pitch, roll, rot);
	}

	// Row-major 3x3 -> column-major mat4 for the shader.
	float specRot[16] = {};
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c)
			specRot[c * 4 + r] = (float)rot[r * 3 + c];
	specRot[15] = 1.0f;

	IntRect box;
	box.left = 0;
	box.top = 0;
	box.width = width;
	box.height = height;
	postprocess->DrawSpectatorToImage(
		&xrSpectatorTextures[0],
		(VkFormat)xrSwapchainFormat,
		box,
		specRot,
		FVector4((float)src[0], (float)src[1], (float)src[2], (float)src[3]),
		FVector4((float)dst[0], (float)dst[1], (float)dst[2], (float)dst[3]),
		sourceScaleX,
		sourceScaleY,
		sourceOffsetX,
		sourceOffsetY,
		vkfb->GetCommands()->GetDrawCommands());
	return true;
}

bool VKOpenXRDeviceMode::RenderDesktopMirror(VulkanRenderDevice* fb, VulkanImage* dstImage) const
{
	if (!fb || !dstImage || vr_desktop_view == -1)
		return false;

	auto* cmdbuffer = fb->GetCommands()->GetDrawCommands();
	const bool sideBySide = vr_desktop_view != 1 && vr_desktop_view != 2;

	// [SPECTATOR] Consumed here, so a frame that does not draw it falls back.
	const bool spectatorReady = xrSpectatorFrameReady && vr_spectator > 0 &&
		xrSpectatorTextures.size() == 1 && xrSpectatorTextures[0].Image != nullptr;
	xrSpectatorFrameReady = false;
	const int leftSourceIndex = vr_swap_eyes ? 1 : 0;
	const int rightSourceIndex = vr_swap_eyes ? 0 : 1;
	const bool useDedicatedMirrorTextures = ShouldUseDedicatedDesktopMirrorTextures(this);
	auto& mirrorSources = useDedicatedMirrorTextures ? xrMirrorPresentTextures : xrPresentTextures;

	if (mirrorSources.empty() ||
		mirrorSources[leftSourceIndex].Image == nullptr ||
		(sideBySide && mirrorSources[rightSourceIndex].Image == nullptr))
		return false;

	const bool hasBackdrop = xrVirtualScreenBackdropVisible &&
		xrVirtualScreenBackdropImageIndex >= 0 &&
		xrVirtualScreenBackdropImageIndex < (int)xrVirtualScreenBackdropTextures.size();

	VkTextureImage* leftEyeSource = &mirrorSources[leftSourceIndex];
	VkTextureImage* rightEyeSource = &mirrorSources[rightSourceIndex];
	VkImageTransition()
		.AddImage(leftEyeSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.Execute(cmdbuffer);
	if ((sideBySide || vr_desktop_view == 2) && rightEyeSource != leftEyeSource)
	{
		VkImageTransition()
			.AddImage(rightEyeSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmdbuffer);
	}

	if (hasBackdrop)
	{
		auto& backdropSource = xrVirtualScreenBackdropTextures[xrVirtualScreenBackdropImageIndex];
		VkImageTransition()
			.AddImage(&backdropSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmdbuffer);
	}
	if (spectatorReady)
	{
		VkImageTransition()
			.AddImage(&xrSpectatorTextures[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmdbuffer);
	}

	VkImageMemoryBarrier dstBarrier = {};
	dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.image = dstImage->image;
	dstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	cmdbuffer->pipelineBarrier(
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

	const IntRect mirrorBox = fb->mOutputLetterbox;
	auto blitImage = [&](VkTextureImage* source, const IntRect& rect)
	{
		VkImageBlit blit = {};
		blit.srcOffsets[0] = { 0, 0, 0 };
		blit.srcOffsets[1] = { (int32_t)source->Image->width, (int32_t)source->Image->height, 1 };
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel = 0;
		blit.srcSubresource.baseArrayLayer = 0;
		blit.srcSubresource.layerCount = 1;
		blit.dstOffsets[0] = { rect.left, rect.top, 0 };
		blit.dstOffsets[1] = { rect.left + rect.width, rect.top + rect.height, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel = 0;
		blit.dstSubresource.baseArrayLayer = 0;
		blit.dstSubresource.layerCount = 1;
		cmdbuffer->blitImage(
			source->Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dstImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, VK_FILTER_LINEAR);
	};
	auto blitOverlayImage = [&](VkTextureImage* source, const IntRect& rect)
	{
		VkImageBlit blit = {};
		blit.srcOffsets[0] = { (int32_t)source->Image->width, (int32_t)source->Image->height, 0 };
		blit.srcOffsets[1] = { 0, 0, 1 };
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel = 0;
		blit.srcSubresource.baseArrayLayer = 0;
		blit.srcSubresource.layerCount = 1;
		blit.dstOffsets[0] = { rect.left, rect.top, 0 };
		blit.dstOffsets[1] = { rect.left + rect.width, rect.top + rect.height, 1 };
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel = 0;
		blit.dstSubresource.baseArrayLayer = 0;
		blit.dstSubresource.layerCount = 1;
		cmdbuffer->blitImage(
			source->Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dstImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, VK_FILTER_LINEAR);
	};

	// [SPECTATOR] The stabilized view, when this frame drew one. Anything else --
	// the flat menu screen, a frame that skipped it -- keeps the plain mirror.
	const bool useSpectator = spectatorReady;
	if (useSpectator)
	{
		blitImage(&xrSpectatorTextures[0], mirrorBox);
	}
	else if (vr_desktop_view == 1)
	{
		blitImage(leftEyeSource, mirrorBox);
	}
	else if (vr_desktop_view == 2)
	{
		blitImage(rightEyeSource, mirrorBox);
	}
	else
	{
		IntRect leftHalf = mirrorBox;
		leftHalf.width = mirrorBox.width / 2;
		IntRect rightHalf = mirrorBox;
		rightHalf.width = mirrorBox.width - leftHalf.width;
		rightHalf.left += leftHalf.width;

		blitImage(leftEyeSource, leftHalf);
		blitImage(rightEyeSource, rightHalf);
	}

	if (hasBackdrop)
	{
		auto& backdropSource = xrVirtualScreenBackdropTextures[xrVirtualScreenBackdropImageIndex];
		VkImageTransition()
			.AddImage(&backdropSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmdbuffer);
		blitOverlayImage(&backdropSource, mirrorBox);
	}
	if (xrVirtualScreenVisible && xrVirtualScreenImageIndex >= 0 && xrVirtualScreenImageIndex < (int)xrVirtualScreenTextures.size())
	{
		auto& contentSource = xrVirtualScreenTextures[xrVirtualScreenImageIndex];
		VkImageTransition()
			.AddImage(&contentSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmdbuffer);
		blitOverlayImage(&contentSource, mirrorBox);
	}

	if (hasBackdrop)
	{
		auto& backdropSource = xrVirtualScreenBackdropTextures[xrVirtualScreenBackdropImageIndex];
		VkImageTransition()
			.AddImage(&backdropSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmdbuffer);
	}
	if (xrVirtualScreenVisible && xrVirtualScreenImageIndex >= 0 && xrVirtualScreenImageIndex < (int)xrVirtualScreenTextures.size())
	{
		auto& contentSource = xrVirtualScreenTextures[xrVirtualScreenImageIndex];
		VkImageTransition()
			.AddImage(&contentSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmdbuffer);
	}
	VkImageTransition()
		.AddImage(leftEyeSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(cmdbuffer);
	if ((sideBySide || vr_desktop_view == 2) && rightEyeSource != leftEyeSource)
	{
		VkImageTransition()
			.AddImage(rightEyeSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmdbuffer);
	}
	if (spectatorReady)
	{
		VkImageTransition()
			.AddImage(&xrSpectatorTextures[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmdbuffer);
	}

	return true;
}

// THE BODY FRAME AT DRAW RATE. Head position and yaw, never pitch or roll.
//
// Deliberately the same construction as GetHandTransform below, minus the hand:
// the same CenterEyePos, the same vr_vunits_per_meter, the same pixelstretch
// division and the same doomYaw. Anything worn on the body has to land in the
// frame the hands already land in, and the only way to be sure of that is to
// build it the same way rather than to derive it a second time -- the comment
// on MDL_FOLLOWMAINHAND in models.cpp records what re-deriving this basis by
// hand cost the last two times someone tried.
//
// No hand offset and no hand angles, so what comes back is where YOU are and
// which way you are facing. Pitch and roll are left out on purpose: a holster
// does not tip when you look down.
bool VKOpenXRDeviceMode::GetHmdTransform(VSMatrix* mat, DVector3 bodyOfs, float* outBodyYaw, double yawOverride) const
{
	// A VR demo answers first: while one replays, the recorded head; while one records,
	// during a tic, the frame captured at the tic's start (p_vrdemo.cpp).
	if (VRDemo_GetHmdTransform(mat, bodyOfs, outBodyYaw, yawOverride))
		return true;

	double pixelstretch = 1.2;
	float rendererYawDeg = 0;
	if (!GetHmdBaseTransform(mat, &pixelstretch, &rendererYawDeg))
		return false;

	const float bodyYawDeg = !isnan(yawOverride) ? (float)yawOverride : rendererYawDeg;

	if (vr_body_debug)
	{
		static uint64_t lastReport = 0;
		static int calls = 0;
		++calls;
		const uint64_t now = I_nsTime();
		if (now - lastReport > 1000000000ull)
		{
			lastReport = now;
			player_t *pl = &players[consoleplayer];
			const double hmdYaw = (pl && pl->mo) ? pl->mo->HmdYaw.Degrees() : 0.0;
			double drift = hmdYaw - doomYaw;
			while (drift >  180.0) drift -= 360.0;
			while (drift < -180.0) drift += 360.0;
			Printf("[VRBODY] heading: doom=%.1f  hmd=%.1f  drift=%.1f%s  |  used=%.1f (%s)  |  seat=(%.1f %.1f %.1f)  |  %d calls/s\n",
				doomYaw, hmdYaw, drift,
				fabs(drift) > 1.0 ? "  <-- THESE DIFFER" : "",
				bodyYawDeg, isnan(yawOverride) ? "renderer" : "CALLER",
				bodyOfs.X, bodyOfs.Y, bodyOfs.Z, calls);
			calls = 0;
		}
	}

	if (outBodyYaw)
		*outBodyYaw = bodyYawDeg;

	// The heading, THE SEAT and the hand-back to map units: VR_FinishHmdTransform
	// (hw_vrmodes.cpp), which carries the notes on all three.
	VR_FinishHmdTransform(mat, pixelstretch, bodyYawDeg, bodyOfs, vr_vunits_per_meter);
	return true;
}

// The first half of GetHmdTransform: position and units, no heading, no seat.
bool VKOpenXRDeviceMode::GetHmdBaseTransform(VSMatrix* mat, double* outPixelStretch, float* outBodyYaw) const
{
	double pixelstretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
	player_t* player = &players[consoleplayer];
	if (!player)
		return false;

	mat->loadIdentity();
	mat->translate((float)r_viewpoint.CenterEyePos.X, (float)r_viewpoint.CenterEyePos.Z - GetDoomPlayerHeightWithoutCrouch(player), (float)r_viewpoint.CenterEyePos.Y);
	mat->scale((float)vr_vunits_per_meter, (float)vr_vunits_per_meter, (float)-vr_vunits_per_meter);

	mat->translate(0.f, (hmdPosition[1] + (float)vr_height_adjust) / (float)pixelstretch, 0.f);
	mat->scale(1, 1 / (float)pixelstretch, 1);

	// The cinematic screen layer takes its heading from the viewpoint for the
	// same reason the hand path does -- doomYaw is not the drawn heading there.
	*outBodyYaw = VR_UseCinematicScreenLayer()
		? (float)r_viewpoint.Angles.Yaw.Degrees()
		: doomYaw;
	*outPixelStretch = pixelstretch;
	return true;
}

bool VKOpenXRDeviceMode::GetHandTransform(int hand, VSMatrix* mat) const
{
	double pixelstretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
	player_t* player = &players[consoleplayer];
	if (player)
	{
		const bool rightHanded = IsRightHandedVrControls();
		const bool useMainHandPose = (rightHanded && hand == 1) || (!rightHanded && hand == 0);
		float* offset = useMainHandPose ? weaponoffset : offhandoffset;
		float* angles = useMainHandPose ? weaponangles : offhandangles;

		mat->loadIdentity();
		mat->translate((float)r_viewpoint.CenterEyePos.X, (float)r_viewpoint.CenterEyePos.Z - GetDoomPlayerHeightWithoutCrouch(player), (float)r_viewpoint.CenterEyePos.Y);
		mat->scale((float)vr_vunits_per_meter, (float)vr_vunits_per_meter, (float)-vr_vunits_per_meter);

		mat->translate(-offset[0], (hmdPosition[1] + offset[1] + (float)vr_height_adjust) / (float)pixelstretch, offset[2]);
		mat->scale(1, 1 / (float)pixelstretch, 1);

        if (VR_UseCinematicScreenLayer())
        {
            mat->rotate(-90 + r_viewpoint.Angles.Yaw.Degrees() + (angles[1] - hmdorientation[1]), 0, 1, 0);
            mat->rotate(-angles[0] - r_viewpoint.Angles.Pitch.Degrees(), 1, 0, 0);
        }
		else
		{
			mat->rotate(-90 + doomYaw + (angles[1] - hmdorientation[1]), 0, 1, 0);
			mat->rotate(-angles[0], 1, 0, 0);
		}
		mat->rotate(-angles[2], 0, 0, 1);
		return true;
	}
	return false;
}

bool VKOpenXRDeviceMode::GetTeleportLocation(DVector3 &out) const
{
	player_t* player = &players[consoleplayer];
	if (vr_teleport &&
		ready_teleport &&
		(player && player->mo->health > 0) &&
		m_TeleportTarget == TRACE_HitFloor)
	{
		out = m_TeleportLocation;
		return true;
	}

	return false;
}

void VKOpenXRDeviceMode::StopHaptics() const
{
	if (xrSession == XR_NULL_HANDLE || xrHapticAction == XR_NULL_HANDLE)
		return;

	for (int hand = 0; hand < 2; ++hand)
	{
		if (!xrHapticActive[hand])
			continue;

		XrHapticActionInfo actionInfo{ XR_TYPE_HAPTIC_ACTION_INFO };
		actionInfo.action = xrHapticAction;
		actionInfo.subactionPath = (hand == 0) ? xrLeftHandPath : xrRightHandPath;
		xrStopHapticFeedback(xrSession, &actionInfo);
		xrHapticActive[hand] = false;
	}

	xrHapticDuration[0] = xrHapticDuration[1] = 0.0;
	xrHapticIntensity[0] = xrHapticIntensity[1] = 0.0f;
}

static XrDuration MakeOpenXRHapticDuration(double remainingMs)
{
	// XR_MIN_HAPTIC_DURATION is valid, but several runtimes are much more reliable when we
	// send a small finite pulse and re-issue it while the local timer is still active.
	constexpr double kMinPulseMs = 1.0;

	// [BB] 50, NOT 10. The re-issue design above is sound, but 10ms is shorter than
	// a frame: at 90Hz a frame is 11.1ms and at 72Hz it is 13.9, so a 10ms window
	// left a dead gap in EVERY pulse even once TearDown stopped cancelling them --
	// the drive would expire before the next re-issue could renew it, and a pulse
	// that stutters at frame rate is felt as nothing rather than as a buzz.
	//
	// 50ms covers a dropped frame with margin and still cannot over-run: the pulse
	// is stopped on expiry by the branch above, and the local timer decays in
	// ProcessHaptics regardless of what the runtime was told.
	constexpr double kMaxPulseMs = 50.0;

	if (remainingMs < kMinPulseMs)
		remainingMs = kMinPulseMs;
	else if (remainingMs > kMaxPulseMs)
		remainingMs = kMaxPulseMs;

	return static_cast<XrDuration>(std::llround(remainingMs * 1000000.0));
}

void VKOpenXRDeviceMode::ProcessHaptics() const
{
	if (!vr_enable_haptics || xrSession == XR_NULL_HANDLE || xrHapticAction == XR_NULL_HANDLE || !isSessionRunning)
	{
		if (vr_haptic_debug)
		{
			const int reason = !vr_enable_haptics ? 0
			                 : (xrSession == XR_NULL_HANDLE) ? 1
			                 : (xrHapticAction == XR_NULL_HANDLE) ? 2 : 3;
			static int lastReason = -1;
			if (reason != lastReason)
			{
				lastReason = reason;
				static const char *why[4] = {
					"vr_enable_haptics is off",
					"no XR session",
					"haptic action was never created",
					"XR session is not running" };
				Printf("[HAPTIC] not processing: %s\n", why[reason]);
			}
		}
		StopHaptics();
		return;
	}

	static auto lastUpdate = std::chrono::steady_clock::now();
	static XrResult lastHapticError[2] = { XR_SUCCESS, XR_SUCCESS };
	const auto now = std::chrono::steady_clock::now();
	const double elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count();
	lastUpdate = now;

	for (int hand = 0; hand < 2; ++hand)
	{
		const bool active = xrHapticDuration[hand] != 0.0 && xrHapticIntensity[hand] > 0.0f;
		if (!active)
		{
			if (xrHapticActive[hand])
			{
				XrHapticActionInfo actionInfo{ XR_TYPE_HAPTIC_ACTION_INFO };
				actionInfo.action = xrHapticAction;
				actionInfo.subactionPath = (hand == 0) ? xrLeftHandPath : xrRightHandPath;
				xrStopHapticFeedback(xrSession, &actionInfo);
				xrHapticActive[hand] = false;
			}
			continue;
		}

		XrHapticVibration vibration{ XR_TYPE_HAPTIC_VIBRATION };
		vibration.amplitude = clamp<float>(xrHapticIntensity[hand], 0.0f, 1.0f);
		vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
		vibration.duration = MakeOpenXRHapticDuration(xrHapticDuration[hand]);

		XrHapticActionInfo actionInfo{ XR_TYPE_HAPTIC_ACTION_INFO };
		actionInfo.action = xrHapticAction;
		actionInfo.subactionPath = (hand == 0) ? xrLeftHandPath : xrRightHandPath;
		XrResult result = xrApplyHapticFeedback(xrSession, &actionInfo, (XrHapticBaseHeader*)&vibration);
		if (XR_SUCCEEDED(result))
		{
			// Only on the leading edge: this re-issues every frame for the life
			// of the pulse, and logging all of them would bury everything else.
			if (vr_haptic_debug && !xrHapticActive[hand])
			{
				Printf("[HAPTIC] runtime accepted %s pulse amp=%.2f dur=%lldns remaining=%.0fms\n",
					hand == 0 ? "left" : "right",
					(double)vibration.amplitude,
					(long long)vibration.duration,
					xrHapticDuration[hand]);
			}
			xrHapticActive[hand] = true;
			lastHapticError[hand] = XR_SUCCESS;
		}
		else
		{
			if (lastHapticError[hand] != result)
			{
				Printf("OpenXR: xrApplyHapticFeedback failed for %s hand result=%d amplitude=%.3f duration_ns=%lld\n",
					hand == 0 ? "left" : "right",
					(int)result,
					(double)vibration.amplitude,
					(long long)vibration.duration);
				lastHapticError[hand] = result;
			}

			if (xrHapticActive[hand])
			{
				xrStopHapticFeedback(xrSession, &actionInfo);
				xrHapticActive[hand] = false;
			}
		}

		if (xrHapticDuration[hand] > 0.0)
		{
			xrHapticDuration[hand] -= elapsedMs;
			if (xrHapticDuration[hand] <= 0.0)
			{
				xrHapticDuration[hand] = 0.0;
			}
		}
	}
}

void VKOpenXRDeviceMode::Vibrate(float duration, int channel, float intensity) const
{
	if (channel < 0)
		channel = 0;
	if (channel > 1)
		channel = 1;

	if (!vr_enable_haptics)
		return;

	if (vr_haptic_debug)
	{
		// If this line never appears, nothing is asking for haptics at all and
		// the fault is upstream of OpenXR -- most likely the active VRMode is
		// not this one, so Vibrate is landing on a different implementation.
		Printf("[HAPTIC] Vibrate ch=%d (%s) dur=%.0fms amp=%.2f\n",
			channel, channel == 0 ? "left" : "right", (double)duration, (double)intensity);
	}

	xrHapticDuration[channel] = duration;
	xrHapticIntensity[channel] = intensity;

	if (duration <= 0.0f || intensity <= 0.0f)
	{
		XrHapticActionInfo actionInfo{ XR_TYPE_HAPTIC_ACTION_INFO };
		actionInfo.action = xrHapticAction;
		actionInfo.subactionPath = (channel == 0) ? xrLeftHandPath : xrRightHandPath;
		if (xrSession != XR_NULL_HANDLE && xrHapticAction != XR_NULL_HANDLE)
			xrStopHapticFeedback(xrSession, &actionInfo);
		xrHapticActive[channel] = false;
		xrHapticDuration[channel] = 0.0;
		xrHapticIntensity[channel] = 0.0f;
		return;
	}

	ProcessHaptics();
}

void VKOpenXRDeviceMode::InitializeMultiview() const
{
	xrMultiviewProbed = false;
	xrMultiviewSupported = false;
	xrMultiviewUsesCoreVulkan = false;
	xrMultiviewMaxViewCount = 0;
	xrMultiviewMaxInstanceIndex = 0;

	if (!xrVkDevice || xrVkDevice->Instance == nullptr)
	{
		return;
	}

	const auto& physicalDevice = xrVkDevice->PhysicalDevice;
	const uint32_t apiVersion = xrVkDevice->Instance->ApiVersion;
	const bool multiviewIsCore = apiVersion >= VK_API_VERSION_1_1;
	const bool hasMultiviewExtension = HasDeviceExtension(physicalDevice, VK_KHR_MULTIVIEW_EXTENSION_NAME);
	const bool hasMultiviewApiPath = multiviewIsCore || hasMultiviewExtension;
	const bool multiviewFeature = physicalDevice.Features.Multiview.multiview == VK_TRUE;
	const bool runtimeStereoViews = xrViewCount >= 2;

	xrMultiviewProbed = true;
	xrMultiviewUsesCoreVulkan = multiviewIsCore;
	xrMultiviewMaxViewCount = physicalDevice.Properties.Multiview.maxMultiviewViewCount;
	xrMultiviewMaxInstanceIndex = physicalDevice.Properties.Multiview.maxMultiviewInstanceIndex;
	xrMultiviewSupported =
		hasMultiviewApiPath &&
		multiviewFeature &&
		runtimeStereoViews &&
		xrMultiviewMaxViewCount >= xrViewCount;

}

bool VKOpenXRDeviceMode::ShouldUseMultiviewThisFrame() const
{
	const bool shouldUse = vr_openxr_multiview &&
		xrMultiviewSupported &&
		// vr_menu_keep_world: MenuOverWorld renders the gameplay eye path too.
		(mFrameRenderMode == FrameRenderMode::GameplayEyes || mFrameRenderMode == FrameRenderMode::MenuOverWorld) &&
		xrViewCount > 1;
	return shouldUse;
}

int VKOpenXRDeviceMode::GetMultiviewLayerCount() const
{
	return ShouldUseMultiviewThisFrame() ? (int)xrViewCount : 1;
}

uint32_t VKOpenXRDeviceMode::GetMultiviewViewMask() const
{
	if (!ShouldUseMultiviewThisFrame())
		return 0;
	if (xrViewCount >= 32)
		return 0xffffffffu;
	return (1u << xrViewCount) - 1u;
}

} // namespace s3d
