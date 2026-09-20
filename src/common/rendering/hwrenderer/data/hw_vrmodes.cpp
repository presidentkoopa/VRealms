/*
** hw_vrmodes.cpp
**
** Matrix handling for stereo 3D rendering
**
**---------------------------------------------------------------------------
**
** Copyright 2015 Christopher Bruns
** Copyright 2016-2021 Christoph Oelckers
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

#include "vectors.h"
#include "hw_cvars.h"
#include "hw_vrmodes.h"
#include "v_video.h"
#include "i_time.h"
#include "g_input.h"
#include "version.h"
#include "i_interface.h"
#include "menu.h"
#include "gl_load/gl_system.h"

#include "gl_renderer.h"
#include "d_player.h"
#include "actorinlines.h"
#include "LSMatrix.h"
#include "hw_vrwheel.h"
#include "gl/stereo3d/gl_openvr.h"
#include "gl/stereo3d/gl_openxrdevice.h"
#include "vulkan/stereo3d/vk_openxrdevice.h"
#include <QzDoom/VrCommon.h>
#include "p_vrdemo.h"

#include "textures.h"
#include "gametexture.h"
#include "common/2d/v_2ddrawer.h"
#include <chrono>
#include <thread>
#include <algorithm>
#include <functional>
#include "c_dispatch.h"
#include "c_bind.h"
#include "c_console.h"
#include "d_eventbase.h"
#include "common/scripting/jit/jit.h"
#include "common/fonts/v_font.h"
#include "common/2d/v_draw.h"
#include "v_text.h"
#include "i_net.h"
#include "keydef.h"

EXTERN_CVAR(Int, developer);

using namespace OpenGLRenderer;

extern thread_local bool isWorkerThread;

// [UZDXREMA] Upstream 5.0.0 dropped the `int V_GetBackend();` prototype from
// v_video.h (it now reads vid_preferbackend directly at each use site), but the
// fork's definition still lives in common/rendering/v_video.cpp because the VR
// paths need the range-clamped value. Declare it locally until the prototype is
// restored to v_video.h. Returns a BACKEND_* value (BACKEND_OPENGL / BACKEND_VULKAN
// / BACKEND_OPENGLES), clamped to BACKEND_OPENGL when vid_preferbackend is bogus.
int V_GetBackend();

EXTERN_CVAR(Bool, vr_hud_mount);
EXTERN_CVAR(Int, vr_hud_mount_pos);
EXTERN_CVAR(Float, vr_hud_mount_scale);
EXTERN_CVAR(Float, vr_hud_mount_xoffset);
EXTERN_CVAR(Float, vr_hud_mount_yoffset);
EXTERN_CVAR(Float, vr_hud_mount_zoffset);
EXTERN_CVAR(Float, vr_hud_mount_pitch);
EXTERN_CVAR(Float, vr_hud_mount_yaw);
EXTERN_CVAR(Bool, vr_hud_mount_roll);
EXTERN_CVAR(Bool, vr_automap_mount);
EXTERN_CVAR(Int, vr_automap_mount_pos);
EXTERN_CVAR(Float, vr_automap_mount_scale);
EXTERN_CVAR(Float, vr_automap_mount_xoffset);
EXTERN_CVAR(Float, vr_automap_mount_yoffset);
EXTERN_CVAR(Float, vr_automap_mount_zoffset);
EXTERN_CVAR(Float, vr_automap_mount_pitch);
EXTERN_CVAR(Float, vr_automap_mount_yaw);
EXTERN_CVAR(Bool, vr_automap_mount_roll);
EXTERN_CVAR(Bool, portablehud);
EXTERN_CVAR(Int, vr_mode);

extern float weaponangles[3];
extern float offhandangles[3];
extern float hmdorientation[3];

static int gSuppressMountedHudFrames = 0;
static uint64_t gSuppressMountedHudLastFrameTime = 0;

VRHudSurface::VRHudSurface() = default;

VRHudSurface::~VRHudSurface()
{
	Clear();
}

void VRHudSurface::Clear()
{
	if (Canvas != nullptr)
	{
		if (Texture != nullptr && Texture->Canvas == Canvas)
		{
			Texture->Canvas = nullptr;
		}
		Canvas->Tex = nullptr;
		auto idx = AllCanvases.Find(Canvas);
		if (idx != -1)
		{
			AllCanvases.Delete(idx);
		}
		Canvas = nullptr;
	}
	GameTexture = nullptr;
	Texture = nullptr;
}

bool VRHudSurface::IsCanvasLive() const
{
	if (Texture == nullptr || Canvas == nullptr)
	{
		return false;
	}
	if (Texture->Canvas != Canvas || Canvas->Tex != Texture)
	{
		return false;
	}
	return AllCanvases.Find(Canvas) != -1;
}

void VRHudSurface::EnsureSize(int width, int height)
{
	if (width <= 0 || height <= 0)
	{
		return;
	}
	if (Texture && Texture->GetWidth() == width && Texture->GetHeight() == height && IsCanvasLive())
	{
		return;
	}
	Clear();
	Texture = new FCanvasTexture(width, height);
	// Mark this canvas as translucent so the render loop clears the FBO to
	// transparent black before Draw2D, and ApplyMaterial uses TM_NORMAL
	// instead of TM_OPAQUE. This propagates automatically to all surfaces
	// that sample this texture: VR quad, world geometry, model textures.
	Texture->bTranslucentCanvas = true;
	GameTexture = MakeGameTexture(Texture, nullptr, ETextureType::Wall);
	Canvas = Create<FCanvas>();
	Texture->Canvas = Canvas;
	Canvas->Tex = Texture;
	Canvas->Drawer.SetSize(width, height);
	AllCanvases.Push(Canvas);
}

void VRHudSurface::BeginUpdate()
{
	if (Canvas != nullptr)
	{
		Canvas->Drawer.Clear();
	}
}

void VRHudSurface::EndUpdate()
{
	MarkDirty();
}

void VRHudSurface::MarkDirty()
{
	if (Texture != nullptr)
	{
		Texture->NeedUpdate();
	}
}

VRHudSurface& GetVRHudSurface()
{
	static VRHudSurface surface;
	return surface;
}

void VR_DestroyHudSurface()
{
	GetVRHudSurface().Clear();
}

void VR_EnsureHudSurface(int width, int height)
{
	GetVRHudSurface().EnsureSize(width, height);
}

void VR_SuppressMountedHudForFrames(int frames)
{
	if (frames > gSuppressMountedHudFrames)
	{
		gSuppressMountedHudFrames = frames;
	}
}

bool VR_ShouldDrawMountedHud()
{
	if (gSuppressMountedHudFrames > 0 && screen != nullptr)
	{
		if (gSuppressMountedHudLastFrameTime != screen->FrameTime)
		{
			gSuppressMountedHudLastFrameTime = screen->FrameTime;
			gSuppressMountedHudFrames--;
		}
		return false;
	}

	const bool portableHud = VR_UsePortableHud();
	if (!portableHud && !vr_hud_mount && !vr_automap_mount)
	{
		return false;
	}

	// [MR] Hide mounted HUD/Map when menu or console is active to allow facial overlay restoration
	if (menuactive || ConsoleState != c_up)
	{
		return false;
	}

	// [MR] Only draw if the respective feature is active
	if (automapactive && !portableHud && !vr_automap_mount) return false;
	if (!automapactive && !portableHud && !vr_hud_mount) return false;

	auto& surface = GetVRHudSurface();
	return surface.HasGameTexture() && surface.GetWidth() > 0 && surface.GetHeight() > 0;
}

bool VR_GetMountedHudTransform(VSMatrix& out)
{
	const bool portableHud = VR_UsePortableHud();
	if (!portableHud && !vr_hud_mount && !vr_automap_mount)
	{
		return false;
	}

	VSMatrix mountTransform;
	int mountedHand;
	if (automapactive && (portableHud || vr_automap_mount))
	{
		mountedHand = vr_automap_mount_pos == 0 ? VR_MAINHAND : VR_OFFHAND;
		if (!VRMode::GetVRModeCached(true)->GetWeaponTransform(&mountTransform, mountedHand)) return false;

		const float handSign = mountedHand == VR_MAINHAND ? -1.f : 1.f;
		mountTransform.translate(-vr_automap_mount_xoffset * handSign, -vr_automap_mount_zoffset, -vr_automap_mount_yoffset);
		mountTransform.rotate(vr_automap_mount_yaw * handSign, 0, 1, 0);
		mountTransform.rotate(-vr_automap_mount_pitch, 1, 0, 0);
		if (!vr_automap_mount_roll)
		{
			const float controllerRoll = mountedHand == VR_MAINHAND ? weaponangles[2] : offhandangles[2];
			mountTransform.rotate(-controllerRoll, 0, 0, 1);
		}
	}
	else
	{
		mountedHand = vr_hud_mount_pos == 0 ? VR_MAINHAND : VR_OFFHAND;
		if (!VRMode::GetVRModeCached(true)->GetWeaponTransform(&mountTransform, mountedHand)) return false;

		const float handSign = mountedHand == VR_MAINHAND ? -1.f : 1.f;
		mountTransform.translate(-vr_hud_mount_xoffset * handSign, -vr_hud_mount_zoffset, -vr_hud_mount_yoffset);
		mountTransform.rotate(vr_hud_mount_yaw * handSign, 0, 1, 0);
		mountTransform.rotate(-vr_hud_mount_pitch, 1, 0, 0);
		if (!vr_hud_mount_roll)
		{
			const float controllerRoll = mountedHand == VR_MAINHAND ? weaponangles[2] : offhandangles[2];
			mountTransform.rotate(-controllerRoll, 0, 0, 1);
		}
	}
	out = mountTransform;
	return true;
}

namespace
{
	bool IsOpenVRNetWaitModeActive()
	{
#ifdef USE_OPENVR
		auto* mode = dynamic_cast<const s3d::OpenVRMode*>(VRMode::GetVRModeCached(true));
		return mode != nullptr && mode->IsVR();
#else
		return false;
#endif
	}

#ifdef USE_OPENXR
	const s3d::VKOpenXRDeviceMode* GetOpenXRNetWaitMode()
	{
		// The OpenXR device mode is Vulkan-only; on the GL/GLES backends the
		// active stereo mode can never be a VKOpenXRDeviceMode.
		if (V_GetBackend() != BACKEND_VULKAN)
			return nullptr;
		auto* mode = dynamic_cast<const s3d::VKOpenXRDeviceMode*>(VRMode::GetVRModeCached(true));
		return (mode != nullptr && mode->IsVR()) ? mode : nullptr;
	}
#endif

	FFont* GetNetWaitFont()
	{
		if (SmallFont != nullptr)
			return SmallFont;
		if (AlternativeSmallFont != nullptr)
			return AlternativeSmallFont;
		if (BigFont != nullptr)
			return BigFont;
		if (NewSmallFont != nullptr)
			return NewSmallFont;
		return NewConsoleFont;
	}

	FString GetNetWaitPrimaryMessage()
	{
		const char* message = I_GetNetWaitMessage();
		if (message != nullptr && message[0] != '\0')
		{
			return FString(message);
		}
		if (I_GetNetWaitRole() == NETWAITROLE_Client)
		{
			return FString("Contacting host");
		}
		return FString("Waiting for players");
	}

	FString GetNetWaitSecondaryMessage()
	{
		if (I_GetNetWaitTotalPlayers() > 0)
		{
			return FStringf("%d/%d players", I_GetNetWaitFoundPlayers(), I_GetNetWaitTotalPlayers());
		}
		if (I_GetNetWaitPhase() == NETWAITPHASE_Contacting || I_GetNetWaitRole() == NETWAITROLE_Client)
		{
			static const char spinnerChars[4] = { '|', '/', '-', '\\' };
			return FStringf("Contacting host %c", spinnerChars[(I_msTime() / 250) & 3]);
		}
		return FString();
	}

	FString GetNetWaitAddressMessage()
	{
		const char* localAddress = I_GetLocalAddress();
		if (localAddress != nullptr && localAddress[0] != '\0')
		{
			return FStringf("Local IP: %s", localAddress);
		}
		return FString("Local IP: unavailable");
	}

	void MarkRecentCommandsAsOutside2D(F2DDrawer* drawer, int startIndex)
	{
		if (drawer == nullptr)
		{
			return;
		}

		for (int i = startIndex; i < drawer->mData.Size(); ++i)
		{
			drawer->mData[i].mOutside2D = true;
		}

		drawer->mHasInside2DCommands = false;
		drawer->mHasOutside2DCommands = false;
		for (const auto& cmd : drawer->mData)
		{
			if (cmd.mOutside2D)
				drawer->mHasOutside2DCommands = true;
			else
				drawer->mHasInside2DCommands = true;
		}
	}

	bool ConsumeNetWaitCancelEvent()
	{
		auto isMenuAbortBinding = [](int key)
		{
			const char* bind = Bindings.GetBind((unsigned int)key);
			if (bind == nullptr || bind[0] == '\0')
			{
				return false;
			}

			return stricmp(bind, "menu_main") == 0 ||
				stricmp(bind, "toggleconsole") == 0 ||
				strstr(bind, "menu_main") != nullptr;
		};

		for (int evnum = eventtail; evnum != eventhead; evnum = (evnum + 1) & (MAXEVENTS - 1))
		{
			event_t& ev = events[evnum];
			if (ev.type != EV_KeyDown)
			{
				continue;
			}

			if (ev.data1 == KEY_ESCAPE || ev.data1 == KEY_PAD_BACK || ev.data1 == KEY_PAD_B || isMenuAbortBinding(ev.data1))
			{
				ev.type = EV_None;
				return true;
			}
		}
		return false;
	}
}

bool VR_IsNetWaitShellActive()
{
	return I_IsNetWaitSessionActive() && I_IsUsingVRNetWaitShell();
}

bool VR_CanUseNetWaitShell()
{
	if (IsOpenVRNetWaitModeActive())
	{
		return true;
	}
#ifdef USE_OPENXR
	return GetOpenXRNetWaitMode() != nullptr;
#else
	return false;
#endif
}

bool VR_NetWaitLoop(bool (*timer_callback)(void*), void* userdata)
{
	if (!VR_CanUseNetWaitShell())
	{
		return false;
	}

	uint64_t nextCallbackTime = I_msTime();
	const bool cancelOnMenuOpen = (menuactive == MENU_Off);
	const bool cancelOnConsoleOpen = (ConsoleState == c_up);
	for (;;)
	{
		I_GetEvent();
		if (ConsumeNetWaitCancelEvent())
		{
			return false;
		}
		D_ProcessEvents();
		if ((cancelOnMenuOpen && menuactive != MENU_Off) ||
			(cancelOnConsoleOpen && ConsoleState != c_up))
		{
			return false;
		}
		if (screen != nullptr)
		{
			screen->BeginFrame();
			screen->Update();
		}

		const uint64_t now = I_msTime();
		if (now >= nextCallbackTime)
		{
			if (timer_callback != nullptr && timer_callback(userdata))
			{
				return true;
			}
			nextCallbackTime = now + 500;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
}

void VR_RenderNetWaitShellContents(int width, int height, bool outside2D)
{
	if (screen == nullptr || twod == nullptr)
	{
		return;
	}

	twod->Clear();
	twod->Begin(width, height);
	twod->AddColorOnlyQuad(0, 0, width, height, PalEntry(255, 0, 0, 0), nullptr, false, outside2D);

	FFont* font = GetNetWaitFont();
	if (font == nullptr)
	{
		font = NewConsoleFont;
	}
	if (font != nullptr)
	{
		const int textScaleX = outside2D ? 8 : 4;
		const int textScaleY = outside2D ? 10 : 4;
		const int virtualTextWidth = std::max(1, width / textScaleX);
		const int virtualTextHeight = std::max(1, height / textScaleY);
		const FString primary = GetNetWaitPrimaryMessage();
		const FString secondary = GetNetWaitSecondaryMessage();
		const FString address = GetNetWaitAddressMessage();
		const FString hint = FString("Press Menu\\Cancel to Abort");
		const int lineHeight = font->GetHeight();
		const int lineGap = 8;
		const int hintGap = std::max(12, lineHeight * 2);
		int blockHeight = lineHeight + hintGap + lineHeight;
		if (!secondary.IsEmpty())
		{
			blockHeight += lineHeight + lineGap;
		}
		if (!address.IsEmpty())
		{
			blockHeight += lineHeight + lineGap;
		}
		int currentY = std::max(4, (virtualTextHeight - blockHeight) / 2);

		auto drawCenteredLine = [&](EColorRange color, const FString& text)
		{
			if (text.IsEmpty())
			{
				return;
			}
			const int textWidth = font->StringWidth(text);
			const int textX = std::max(4, (virtualTextWidth - textWidth) / 2);
			const int commandStart = twod->mData.Size();
			DrawText(twod, font, color, textX, currentY, text.GetChars(),
				DTA_VirtualWidth, virtualTextWidth,
				DTA_VirtualHeight, virtualTextHeight,
				TAG_DONE);
			if (outside2D)
			{
				MarkRecentCommandsAsOutside2D(twod, commandStart);
			}
			currentY += lineHeight + lineGap;
		};

		drawCenteredLine(CR_RED, primary);
		drawCenteredLine(CR_GRAY, secondary);
		drawCenteredLine(CR_CREAM, address);
		currentY += hintGap - lineGap;
		drawCenteredLine(secondary.IsEmpty() ? CR_GRAY : CR_CREAM, hint);
	}

	twod->End();
}

bool VR_UsePortableHud()
{
	// Portable HUD is world-space only. While virtual screen/screen-layer mode
	// is active, use the normal screen composition path and skip the portable pass.
	return portablehud && !VR_UseScreenLayer();
}

const VRMode *VRMode::GetVRModeCached(bool toscreen)
{
	if (isWorkerThread)
	{
		static VREyeInfo safeMonoEyes[2] = { VREyeInfo(0.f, 1.f), VREyeInfo(0.f, 0.f) };
		static VRMode safeMono(1, 1.f, 1.f, 1.f, safeMonoEyes);
		return &safeMono;
	}

	struct CacheEntry
	{
		bool valid = false;
		uint64_t frameTime = 0;
		int vrMode = 0;
		int backend = 0;
		bool disableTextureFilter = false;
		const VRMode* mode = nullptr;
	};

	thread_local CacheEntry cache[2];
	auto& entry = cache[toscreen ? 1 : 0];
	const uint64_t frameTime = screen != nullptr ? screen->FrameTime : 0;
	const int currentVrMode = (int)vr_mode;
	const int currentBackend = V_GetBackend();
	const bool currentDisableTextureFilter = sysCallbacks.DisableTextureFilter && sysCallbacks.DisableTextureFilter();

	if (entry.valid &&
		entry.frameTime == frameTime &&
		entry.vrMode == currentVrMode &&
		entry.backend == currentBackend &&
		entry.disableTextureFilter == currentDisableTextureFilter)
	{
		return entry.mode;
	}

	entry.valid = true;
	entry.frameTime = frameTime;
	entry.vrMode = currentVrMode;
	entry.backend = currentBackend;
	entry.disableTextureFilter = currentDisableTextureFilter;
	entry.mode = GetVRMode(toscreen);
	return entry.mode;
}
// Set up 3D-specific console variables:
CUSTOM_CVAR(Int, vr_mode, 0, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
{
	// Keep the selected VR mode stable across renderers.
	// OpenGL can use OpenVR (10), Vulkan can use OpenXR (15).
	if (self < 0)
		self = 0;
}

#define PITCH 0
#define YAW 1
#define ROLL 2

typedef float vec_t;
typedef vec_t vec3_t[3];

// switch left and right eye views
CVAR(Bool, vr_swap_eyes, false, CVAR_GLOBALCONFIG   | CVAR_ARCHIVE)
// intraocular distance in meters
CVAR(Float, vr_ipd, 0.064f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG) // METERS

// distance between viewer and the display screen
CVAR(Float, vr_screendist, 0.80f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // METERS

CVAR(Int, vr_desktop_view, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_overlayscreen, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_overlayscreen_always, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_overlayscreen_size, 1., CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_overlayscreen_dist, 0., CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_overlayscreen_vpos, 0., CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_overlayscreen_bg, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// default conversion between (vertical) DOOM units and meters
CVAR(Float, vr_vunits_per_meter, 34.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // METERS
CVAR(Float, vr_height_adjust, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // METERS
CVAR(Float, vr_openxr_fov_adjust_deg, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // DEGREES PER SIDE
CVAR(Float, vr_openxr_eye_shift_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_openxr_debug_submit_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_openxr_sync_mode, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Gate the layered OpenXR/Vulkan multiview path separately from the current per-eye render path
CVARD(Bool, vr_openxr_multiview, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Enable the experimental OpenXR Vulkan multiview path when available")
// Experimental: render the OpenXR scene at runtime-recommended eye size
// instead of desktop framebuffer size to reduce upscale aliasing.
CVAR(Bool, vr_openxr_force_recommended_viewport, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// OpenXR-only internal render scale relative to recommended eye dimensions.
// 1.0 means recommended size, below 1.0 trades quality for performance.
CVAR(Float, vr_openxr_render_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Use the full screen viewport as XR present source by default to reduce
// aspect-stretch upscaling artifacts compared to mSceneViewport.
CVAR(Bool, vr_openxr_use_screen_viewport_for_submit, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVARD(Bool, vr_desktop_view_openxr_render, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Reuse the XR-submitted present texture for the desktop mirror to skip the separate unbiased mirror pass")
CVARD(Bool, vr_openxr_multiview_mirror_reuse, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "When multiview is active, reuse the XR-submitted eye textures for the desktop mirror unless an unbiased mirror path is explicitly needed")
CVARD(Bool, vr_openxr_multiview_postprocess, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Use a shared layered pipeline image and a single scene handoff copy when multiview is active")
// Retuned for UZDoom 5.0.0. present.fp now linearises with pow(c, 2.2) before the
// colour ops, which the pre-5.0 shader did not, so the effective exponent is
// 2.2 * InvGamma rather than InvGamma. The old 1.95 therefore landed at ~4.29 and
// the headset came out roughly five times too dark in the midtones. 1.95 / 2.2
// keeps the fork's previous look under the new pipeline. Tune to taste at runtime.
CVAR(Float, vr_openxr_present_gamma_bias, 0.886f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_openxr_present_contrast_bias, 0.85f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_openxr_present_brightness_bias, -0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_openxr_present_saturation_bias, 1.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, vr_control_scheme, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	M_ResetButtonStates();
}
CUSTOM_CVAR(Int, vr_joy_mode, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("Changing the control mode requires a restart for " GAMENAME ".\n");
}
// WHICH COLOUR SPACE THE EYES ASK THE RUNTIME FOR.
//
//   0  MATCH THE DESKTOP (default) -- the eye swapchain prefers the same colour space the
//      flat/mirror swapchain does, so both take the identical gamma branch in the present
//      pass and the headset and the window agree.
//   1  PREFER sRGB -- what this build did before. Kept so the difference can be A/B'd in
//      the headset, and as a way back if a runtime behaves unexpectedly.
//
// WHY THIS EXISTS: the eyes preferred sRGB while the flat view preferred UNORM, from the
// SAME rendered frame and the SAME preferredFormat. An sRGB image gets the sRGB transfer
// function applied IN HARDWARE on write and a UNORM one does not, so one path carried an
// extra ~^(1/2.2) the other never saw. The present shader tried to absorb it with
// `InvGamma = 1/sqrt(gamma)` instead of `1/gamma` (vk_postprocess.cpp), which is a fitted
// correction rather than an identity -- halving an exponent cannot undo a different curve
// except at one value of vid_gamma, so the two views could never actually match. Measured:
// mid grey 0.50 read 134/255 in the window and 160 in the headset.
//
// A CHANGE ONLY WHERE THE RUNTIME GIVES US A CHOICE. The preference list still falls
// through to sRGB, so a runtime offering only sRGB formats lands exactly where it does
// today. Presentation only -- nothing here is visible to another machine.
CVAR(Int, vr_eye_colorspace, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, vr_move_use_offhand, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_teleport, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_weaponRotate, -30.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_weaponScale, 1.02f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_3dweaponOffsetX, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_3dweaponOffsetY, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_3dweaponOffsetZ, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_2dweaponOffsetX, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_2dweaponOffsetY, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_2dweaponOffsetZ, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_2dweaponScale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, vr_laser_sight, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color, 0xff0000, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, vr_laser_show_melee, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, vr_laser_hide_on_wheel, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, vr_laser_beam, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVARD(Bool, vr_laser_other_players_beam, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Draw laser beams for other players in multiplayer")
CVARD(Bool, vr_laser_other_players_pointer, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Draw laser pointers for other players in multiplayer")
CVAR(Float, vr_laser_beam_alpha, 0.3f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_laser_beam_width, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_laser_pointer_scale, 0.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_laser_pointer_alpha, 0.9f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, vr_laser_pointer_glow, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_laser_pointer_glow_scale, 1.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_laser_pointer_glow_intensity, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, vr_laser_beam_length, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Int, vr_laser_fixed_length, 100, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_laser_source_offset_x, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_laser_source_offset_y, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_laser_source_offset_z, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// ---------------------------------------------------------------------
// LASER SIGHT -- shaping and the target lock.
//
// DECLARED HERE, MENU'D ELSEWHERE. These have to be C++ because the
// renderer reads them every frame in hw_weapon.cpp, but this fork's
// convention is that the CONTROLS for engine features live outside the
// engine -- in the Radiance Control Panel alongside the rest of the
// fork's knobs -- rather than being bolted into GZDoom's own options
// tree. So there is deliberately no MENUDEF entry for any of these in
// wadsrc. Declaring them without menu'ing them is the whole point.
//
// Every default is chosen so an existing config behaves exactly as it did
// before these existed: glow 1.0 is the intended look, taper 0.45 is what
// the shaping was written around, and lock on is the feature. Set
// vr_laser_beam_glow to 0 and you have the old flat tube back.
// ---------------------------------------------------------------------

// Halo strength around the beam core. 0 disables the halo passes entirely
// and restores the single flat tube the sight used to draw.
CVAR(Float, vr_laser_beam_glow, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// EMISSIVE, WITHOUT A SINGLE DYNAMIC LIGHT.
//
// The scene is rendered into VK_FORMAT_R16G16B16A16_SFLOAT -- a half-float
// HDR target (vk_renderdevice.cpp:1254) -- so colour channels above 1.0
// survive rather than clipping. gl_bloom_threshold defaults to exactly 1.0
// (hw_postprocess_cvars.cpp:45), which means anything drawn brighter than
// white is picked up by the bloom extract pass and bleeds on its own.
//
// So this multiplies the CORE's colour past 1.0 and lets the existing
// bloom do the work. No lights are spawned, nothing is added to the light
// list, and the cost is a multiply -- the glow is a post-process the
// engine was already running.
//
// THE CORE ONLY, never the halo. The halo is already a wide soft additive
// wash; pushing that overbright would flood the screen rather than make
// the beam look hot. The look comes from a small intensely bright centre
// inside a soft one, which is the same reason the beam is drawn as core
// plus halo in the first place.
//
// 1.0 = no overbright, exactly the old behaviour. 1.8 is a clearly lit
// filament without hazing the view.
CVAR(Float, vr_laser_beam_emissive, 1.8f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// How much narrower the halo is at the muzzle than at the impact. 1.0 is a
// parallel-sided tube; lower is more cone. The core never tapers -- it is
// the part you aim with.
CVAR(Float, vr_laser_beam_taper, 0.45f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// HOW FAR THE BEAM STAYS VISIBLE, in world units, fading to nothing as it
// goes. Past this there is only the dot on whatever you are pointing at.
//
// THIS IS THE ONE THAT MAKES IT A SIGHT RATHER THAN A ROD. vr_laser_beam_taper
// above only narrows the halo, and it narrows it at the MUZZLE -- nothing in
// the beam ever dimmed with distance, so a laser aimed down a corridor drew a
// line of identical brightness for two thousand units and read as a solid bar
// bolted to the gun.
//
// vr_laser_beam_length 2 already offered a hard cutoff, but a beam that simply
// stops in mid-air looks broken. This ends it the way a real one ends: it runs
// out.
//
// 0 disables the fade entirely and restores the old full-length beam.
// DEFAULTS SHORT, so the sight leans on the reactive dot rather than on a
// long visible line -- the owner's stated preference: "i want a laser sight
// that can be super short and taper off so i can rely on the reactive dot."
// 100 is a short taper right at the gun; the slider still reaches 8192 for
// whoever wants the old long-beam look back.
CVAR(Float, vr_laser_beam_fade, 100.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// The lock: does the sight react when it is resting on something alive.
CVAR(Bool, vr_laser_lock, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// How far the dot draws in when locked, as a fraction of its idle size.
// 0 keeps the dot the same size and leaves only the brightening and the
// glow swell.
CVAR(Float, vr_laser_lock_tighten, 0.40f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// Breath rate of the lock, in radians per tic. ~0.42 is about 2.3Hz, fast
// enough to read as agitation rather than a slow throb.
CVAR(Float, vr_laser_lock_rate, 0.42f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// Beyond this many map units the dot only draws when it is resting on
// something shootable -- so it stops riding over every far wall in the room
// and starts meaning "there is a thing there". Faded across the last quarter
// of the range rather than cut, or it would pop as you pan. 0 disables the
// behaviour and the dot draws on everything at any distance, as before.
//
// 320 is about a small room across: close enough that the dot is still a
// useful pointer at conversational range, far enough that it goes quiet
// before it becomes clutter.
CVAR(Float, vr_laser_dot_range, 320.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// ---------------------------------------------------------------------
// LASER SIGHT COLOUR, in four tiers.
//
// Resolved in hw_weapon.cpp, highest wins:
//
//   1. the WEAPON's own Weapon.LaserBeamColor, if it set one
//   2. the per-SLOT colour, if per-slot is on
//   3. the OFFHAND colour, if per-hand is on and this is the off hand
//   4. vr_laser_color, the global
//
// Each tier is off by default, so a config that has never touched any of
// this keeps one red sight on both hands exactly as before.
//
// WHY THE WEAPON WINS. Weapon.LaserBeamColor is a mod's deliberate
// statement about a specific gun -- a plasma weapon that wants a blue
// sight should not be overruled by a player's slot preference, in the
// same way Weapon.LaserBeamOffset already overrules nothing and is
// simply obeyed. A player who disagrees can still turn the tiers off.
// ---------------------------------------------------------------------

// HOW MANY COLOURS ARE IN PLAY. One knob rather than a pile of toggles,
// because the three useful answers are a ladder and not a set of
// independent switches:
//
//   0  ONE     -- vr_laser_color for everything, both hands, beam and dot.
//                 The way it has always worked.
//   1  PER HAND-- mainhand and offhand differ, but within a hand the beam
//                 and its dot match. In a two-gun game this is the one that
//                 earns its keep: two identical red lines converging on the
//                 same wall tell you nothing about which hand is which.
//   2  ALL FOUR-- mainhand beam, mainhand dot, offhand beam, offhand dot,
//                 each its own colour. For a dim beam with a hot dot, or
//                 any other split where the pointer wants to read
//                 differently from the line that leads to it.
//
// Higher modes simply use more of the same four cvars, so stepping up the
// ladder never invalidates what you already set.
CVAR(Int, vr_laser_color_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// The other three. vr_laser_color, declared above, is the mainhand beam and
// doubles as the single colour in mode 0.
CVAR(Color, vr_laser_color_offhand, 0x40a0ff, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_dot_color, 0xff2020, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_dot_color_offhand, 0x60c0ff, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// PER SLOT. Ten, matching the number keys, so "the shotgun is orange and
// the plasma is cyan" is a thing a player can simply set.
CVAR(Bool, vr_laser_color_per_slot, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color_slot1, 0xff4040, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color_slot2, 0xffa040, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color_slot3, 0xffe040, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color_slot4, 0x60ff60, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color_slot5, 0x40ffd0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color_slot6, 0x40a0ff, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color_slot7, 0xa060ff, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color_slot8, 0xff60c0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color_slot9, 0xffffff, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_laser_color_slot0, 0x909090, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// ---------------------------------------------------------------------
// COLOUR CYCLING -- a fifth tier that sits ON TOP of the four above rather
// than replacing them.
//
// Every resolved colour is still whatever the ladder above says: a weapon's
// own LaserBeamColor, a slot colour, a per-hand colour, or the global. What
// changes is that colour's HUE gets replaced by a slowly drifting one before
// it is drawn, while its saturation and brightness are kept exactly as
// authored. A cheap red pointer stays a cheap, undersaturated red as it
// cycles; a rich saturated cyan stays rich and saturated. Cycling a hue
// number is what "smooth fade" means here -- it is a walk around the colour
// wheel, not a crossfade between two RGB triples, so it never dips through
// grey or dims on the way.
//
// EACH TARGET HAS ITS OWN INDEPENDENT PHASE, offset at startup, so two
// cycling sights never lock into a duplicate colour and drift apart together
// -- they read as genuinely separate lights rather than as one light with
// four names.
//
// DRIVEN OFF WALL-CLOCK TIME, not the tic counter. The tic rate is fixed at
// 35 permanently (see the fork's own TICRATE decision) and a hue step tied
// to it would visibly stair-step at a slow speed, the same way the old beam
// used to strobe before it got interpolation. A millisecond clock cycles as
// smoothly as the display refreshes.
CVAR(Bool, vr_laser_color_cycle, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// Degrees of hue per second. 360 is one full trip around the wheel every
// second -- fast enough to look electric; 6 is a slow, ambient drift that
// takes a minute to repeat. 0 would be legal but pointless: use the tier
// above instead of turning cycling on to stand still.
CVAR(Float, vr_laser_color_cycle_speed, 60.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// Whether the DOT cycles too. Off by default: the dot is the reactive
// element -- see vr_laser_dot_range -- and it is easier to read as "locked
// on target" if its colour holds still while the beam leading to it cycles.
CVAR(Bool, vr_laser_color_cycle_dot, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// ---------------------------------------------------------------------
// HEADSHOT LINE-UP REACTION -- the sight tells you when it is resting on a
// head, not just on something that can die (that is vr_laser_lock, above).
//
// The engine only draws the reaction; a gameplay mod (see the Headshots
// mod's HS_Handler) decides what counts as a head and writes the answer
// back to AActor.LaserHeadshotLinedUpMain/Off each tic, reading the exact
// same trace the beam itself is drawn from (AActor.LaserTraceTarget*/
// LaserTraceHitPos*). This is deliberately not a beam/dot split like
// colour cycling above -- a headshot lineup is meant to be unmissable, so
// it always tints both.
CVAR(Bool, vr_laser_headshot_react, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// The colour the sight blends toward when lined up. Default is a hot red,
// deliberately far from the cool default sight colours so it reads as an
// alert rather than as one more available hue.
CVAR(Color, vr_laser_headshot_color, 0xff2020, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// Pulse toward the colour above instead of holding it solid. Off means a
// flat colour swap the instant the sight lines up; on means it breathes,
// which reads as "live" rather than as a static UI state change.
CVAR(Bool, vr_laser_headshot_pulse, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// Pulse rate in cycles per second. Wall-clock driven (see CycleHue's own
// comment on why) so it stays smooth regardless of the fixed 35 tic rate.
CVAR(Float, vr_laser_headshot_pulse_speed, 6.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

CUSTOM_CVAR(Int, vr_hitscan_tracer, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
	{
		self = 0;
	}
	else if (self > 2)
	{
		self = 2;
	}
}
CVAR(Color, vr_hitscan_tracer_color, 0xffc040, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_hitscan_tracer_alpha, 0.75f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_hitscan_tracer_length, 50.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_hitscan_tracer_width, 0.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_hitscan_tracer_speed, 26.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_hitscan_tracer_offset, 8.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, vr_hitscan_ricochet, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_hitscan_ricochet_chance, 20.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_snapTurn, 45.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_switch_sticks, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_secondary_button_mappings, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Two-hand stabilize: the weapon is REPOSITIONED to point along the line
// between the two controllers.
//
// Off, and it should stay off. It conflated two jobs that turned out to be
// separable, and only one of them was ever wanted. Steadying the aim is worth
// having; moving the gun is not, because nothing grabbed it -- the weapon
// snapped because two controllers came within a fixed distance of each other,
// whether or not the off hand had anything to do with the weapon at all.
//
// What replaces it is a real grab: a second hand on the pistol grip or on the
// shotgun's forend, published as AActor::TwoHandedHold, which weapons read to
// tighten their spread. The gun stays exactly where your hands are holding it,
// and being two-handed makes you shoot straighter rather than making the gun
// move by itself.
CVAR(Bool, vr_two_handed_weapons, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// ============================================================================
// SUPPORTING A WEAPON WITH THE OTHER HAND -- how it is taken and let go.
//
// WHAT THE OLD PROXIMITY TEST GOT WRONG, and it was one thing, not the thing
// everyone assumed. The version this fork inherited (QuestZDoom VrInputDefault)
// engaged on a GRIP PRESS and checked the hands were within half a metre AT THAT
// MOMENT -- a sanity check so you could not grab a gun across the room. It was
// never re-tested while you held it. This fork dropped the button and kept the
// distance, which turned a deliberate act into something that happened TO you
// whenever your hands drifted near each other. Reloading brings the hands
// together, so it fired constantly during exactly the gesture that least wanted
// it, and that is what made it unusable rather than the distance itself.
//
// WHAT REPLACES IT is the test FRIK (Fallout 4 VR Body, GPL-3) settled on: the
// off hand must lie ALONG THE BARREL, not merely near the other hand. A cone
// about the weapon's own forward axis, with a wider release angle than grab
// angle so an engaged hold cannot flicker at the edge, and a MINIMUM hand
// separation -- which is the part that kills the reload false-fire, because a
// reload brings the hands together and a support hold never does.
//
// Angles are stated as COSINES, which is what the test actually compares: the
// dot product of the barrel direction and the direction to the off hand. 1.0 is
// exactly along the barrel, 0.0 is square to it.
CVAR(Float, vr_support_cone_enter, 0.955f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// ~17 degrees
CVAR(Float, vr_support_cone_exit,  0.900f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// ~26 degrees
// Hand separation, in metres. Below the minimum the hands are on top of each
// other, which is a reload and not a hold; above the maximum they are not on the
// same weapon.
CVAR(Float, vr_support_dist_min, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_support_dist_max, 0.90f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// HOW THE HOLD IS TAKEN AND LET GO. FRIK ships four and exposes them in the
// headset, because which one feels right is a preference and not a fact:
//   0  off          -- no support hold at all
//   1  auto         -- taken whenever the hand is in the cone, let go by moving
//                      the hand sharply away from the weapon
//   2  auto, button -- taken in the cone, released with the grip button, and not
//                      retaken until the hand has left the wider exit cone
//   3  hold         -- hold grip to support, release grip to let go
//   4  toggle       -- press grip to take, press again to let go
// 3 is the default because it is what the stock option did and what the owner
// asked to have back: a deliberate act with an obvious end.
CVAR(Int, vr_support_mode, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Fallback two-hand stabilize reach in real-world inches, used by any weapon
// that does not set Weapon.StabilizeDistance (0, the ZScript default).
CVAR(Float, vr_stabilize_distance_inches, 8.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Require the off hand to actually GRAB the weapon before it stabilises,
// instead of inferring it from the two controllers being close together.
//
// The proximity test predates being able to hold anything: with no hands in
// the world there was nothing to grab, so nearness was the only signal
// available. A weapon that publishes a forend or foregrip (Actor.GripClaimOff,
// GRIPSUBJ_Forend / GRIPSUBJ_Foregrip) no longer needs the guess.
//
// Off by default, because a weapon that claims nothing would otherwise lose
// two-handed aim altogether, and most weapons claim nothing.
CVAR(Bool, vr_stabilize_requires_grab, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Should a holster claim take over that hand's GRIP button?
//
// Off (default): a claim still suppresses two-hand stabilize for that hand,
// but grip keeps its normal meaning and the holster is worked with its own
// bound keys instead. Grip already carries several jobs -- stabilize, the
// secondary-button modifier layer, a plain bind -- and taking it over is the
// part of this that is still unsettled, so it is opt-in rather than assumed.
CVAR(Bool, vr_holster_use_grip, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_momentum, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // Only used in player.zs
CVAR(Float, vr_momentum_threshold, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_crouch_use_button, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, use_action_spawn_yzoffset, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Live placement for hand models, applied on top of their own MODELDEF Offset
// and angle/pitch/roll offsets, for any model that opts in with the MODELDEF
// keyword 'usehandoffsets'.
//
// These exist because MODELDEF is parsed once at load: every trial value costs a
// repack and a restart, which is a miserable way to find six numbers by eye in a
// headset. Units match MODELDEF's own -- model units for the offsets, degrees
// for the rotations -- so once a value looks right it can be folded straight
// back into the MODELDEF and the CVAR returned to zero.
CVAR(Float, vr_hand_ofs_x, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hand_ofs_y, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hand_ofs_z, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hand_yaw,   0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hand_pitch, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hand_roll,  0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// The off hand needs its own set. Both hands are the same mesh mirrored by a
// negative X scale, so one shared value pushes them in opposite directions and
// no single number can place both.
CVAR(Float, vr_offhand_ofs_x, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_offhand_ofs_y, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_offhand_ofs_z, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_offhand_yaw,   0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_offhand_pitch, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_offhand_roll,  0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, vr_enable_haptics, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_pickup_haptic_level, 0.2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_quake_haptic_level, 0.8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_missile_haptic_level, 0.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

//HUD control
CVAR(Float, vr_hud_scale, 0.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_stereo, 1.4f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_distance, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_rotate, 10.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_hud_fixed_pitch, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_hud_fixed_roll, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_hud_mount, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_hud_mount_pos, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_xoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_yoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_zoffset, -0.20f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_scale, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_pitch, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_yaw, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_hud_mount_roll, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Runtime override that forces the mounted HUD/automap path on without
// changing the individual menu toggles.
CVAR(Bool, portablehud, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

//AutoMap control
CVAR(Bool, vr_automap_use_hud, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_scale, 0.4f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_stereo, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_distance, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_rotate, 13.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_automap_fixed_pitch, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_automap_fixed_roll, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_automap_mount, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_automap_mount_pos, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_scale, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_xoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_yoffset, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_zoffset, -0.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_pitch, 45.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_yaw, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_automap_mount_roll, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_automap_border, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_automap_border_color, 0x636363, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CCMD(toggleportablehud)
{
	portablehud = !portablehud;
	Printf("portablehud %s\n", portablehud ? "enabled" : "disabled");
}

void VR_InitPortableHudBinding()
{
	// Allow the menu/keybind entry named "portablehud" to invoke the toggle
	// path instead of only querying the boolean cvar.
	C_SetAlias("portablehud", "toggleportablehud");
}


CVARD(Bool, vr_override_weap_pos, false, 0, "Only used for testing VR environment on PC");
CVARD(Bool, vr_render_weap_in_scene, false, 0, "Only used for testing VR environment on PC");

EXTERN_CVAR(Bool, puristmode);
EXTERN_CVAR(Float, turbo);

CUSTOM_CVAR(Int, vr_move_speed, 19, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	turbo->Callback();
}
CUSTOM_CVAR(Float, vr_run_multiplier, 1.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	turbo->Callback();
}
CUSTOM_CVAR(Float, vr_walk_multiplier, 1.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	turbo->Callback();
}

#define isqrt2 0.7071067812f

VRMode::VRMode(int eyeCount, float horizontalViewportScale,
	float verticalViewportScale, float weaponProjectionScale, VREyeInfo eyes[2])
{
	mEyeCount = eyeCount;
	mHorizontalViewportScale = horizontalViewportScale;
	mVerticalViewportScale = verticalViewportScale;
	mWeaponProjectionScale = weaponProjectionScale;
	mEyes[0] = &eyes[0];
	mEyes[1] = &eyes[1];

}

static float DEG2RAD(float deg)
{
	return deg * float(M_PI / 180.0);
}

static float RAD2DEG(float rad)
{
	return rad * float(180. / M_PI);
}

static const char* VRModeName(int mode)
{
	switch (mode)
	{
	case VR_MONO: return "mono";
	case VR_GREENMAGENTA: return "greenmagenta";
	case VR_REDCYAN: return "redcyan";
	case VR_SIDEBYSIDEFULL: return "side-by-side-full";
	case VR_SIDEBYSIDESQUISHED: return "side-by-side-squished";
	case VR_LEFTEYEVIEW: return "left-eye";
	case VR_RIGHTEYEVIEW: return "right-eye";
	case VR_SIDEBYSIDELETTERBOX: return "side-by-side-letterbox";
	case VR_TOPBOTTOM: return "top-bottom";
	case VR_CHECKERINTERLEAVED: return "checker";
#ifdef USE_OPENVR
	case VR_OPENVR: return "openvr";
#endif
#ifdef USE_OPENXR
	case VR_OPENXR_MOBILE: return "openxr";
#endif
	default: return "unknown";
	}
}

const VRMode *VRMode::GetVRMode(bool toscreen)
{
	static VREyeInfo vrmi_mono_eyes[2] = { VREyeInfo(0.f, 1.f), VREyeInfo(0.f, 0.f) };
	static VREyeInfo vrmi_stereo_eyes[2] = { VREyeInfo(-.5f, 1.f), VREyeInfo(.5f, 1.f) };
	static VREyeInfo vrmi_sbsfull_eyes[2] = { VREyeInfo(-.5f, .5f), VREyeInfo(.5f, .5f) };
	static VREyeInfo vrmi_sbssquished_eyes[2] = { VREyeInfo(-.5f, 1.f), VREyeInfo(.5f, 1.f) };
	static VREyeInfo vrmi_lefteye_eyes[2] = { VREyeInfo(-.5f, 1.f), VREyeInfo(0.f, 0.f) };
	static VREyeInfo vrmi_righteye_eyes[2] = { VREyeInfo(.5f, 1.f), VREyeInfo(0.f, 0.f) };
	static VREyeInfo vrmi_topbottom_eyes[2] = { VREyeInfo(-.5f, 1.f), VREyeInfo(.5f, 1.f) };
	static VREyeInfo vrmi_checker_eyes[2] = { VREyeInfo(-.5f, 1.f), VREyeInfo(.5f, 1.f) };
#if 0 //def USE_OPENVR
	static s3d::OpenVREyePose vrmi_openvr_eyes[2] = { s3d::OpenVREyePose(0, -.5f, 1.f), s3d::OpenVREyePose(1, .5f, 1.f) };
#endif

	static VRMode vrmi_mono(1, 1.f, 1.f, 1.f, vrmi_mono_eyes);
	static VRMode vrmi_stereo(2, 1.f, 1.f, 1.f, vrmi_stereo_eyes);
	static VRMode vrmi_sbsfull(2, .5f, 1.f, 2.f, vrmi_sbsfull_eyes);
	static VRMode vrmi_sbssquished(2, .5f, 1.f, 1.f, vrmi_sbssquished_eyes);
	static VRMode vrmi_lefteye(1, 1.f, 1.f, 1.f, vrmi_lefteye_eyes);
	static VRMode vrmi_righteye(1, 1.f, 1.f, 1.f, vrmi_righteye_eyes);
	static VRMode vrmi_topbottom(2, 1.f, .5f, 1.f, vrmi_topbottom_eyes);
	static VRMode vrmi_checker(2, isqrt2, isqrt2, 1.f, vrmi_checker_eyes);
#if 0 //def USE_OPENVR
	static s3d::OpenVRMode vrmi_openvr(vrmi_openvr_eyes);
#endif

	int mode = !toscreen || (sysCallbacks.DisableTextureFilter && sysCallbacks.DisableTextureFilter()) ? 0 : vr_mode;
	static int lastLoggedRequestedMode = -999999;
	static int lastLoggedResolvedMode = -999999;
	auto logModeSelect = [&](int requestedMode, int resolvedMode)
	{
		if (developer <= 0 || (requestedMode == lastLoggedRequestedMode && resolvedMode == lastLoggedResolvedMode))
		{
			return;
		}
		Printf("VRMode select: requested=%s(%d) resolved=%s(%d)\n",
			VRModeName(requestedMode), requestedMode,
			VRModeName(resolvedMode), resolvedMode);
		lastLoggedRequestedMode = requestedMode;
		lastLoggedResolvedMode = resolvedMode;
	};

	switch (mode)
	{
	default:
	case VR_MONO:
		return &vrmi_mono;

	case VR_GREENMAGENTA:
	case VR_REDCYAN:
	case VR_QUADSTEREO:
	case VR_AMBERBLUE:
	case VR_SIDEBYSIDELETTERBOX:
		return &vrmi_stereo;

	case VR_SIDEBYSIDESQUISHED:
	case VR_COLUMNINTERLEAVED:
		return &vrmi_sbssquished;

	case VR_SIDEBYSIDEFULL:
		return &vrmi_sbsfull;

	case VR_TOPBOTTOM:
	case VR_ROWINTERLEAVED:
		return &vrmi_topbottom;

	case VR_LEFTEYEVIEW:
		return &vrmi_lefteye;

	case VR_RIGHTEYEVIEW:
		return &vrmi_righteye;

	case VR_CHECKERINTERLEAVED:
		return &vrmi_checker;
#ifdef USE_OPENVR
	case VR_OPENVR:
	{
		const VRMode &vrmode = s3d::OpenVRMode::getInstance();
		const bool initialized = vrmode.IsInitialized();
		logModeSelect(mode, initialized ? mode : VR_MONO);
		return initialized ? &vrmode : &vrmi_mono;
		//return vrmi_openvr.IsInitialized() ? &vrmi_openvr : &vrmi_mono;
	}
#endif
#ifdef USE_OPENXR
	case VR_OPENXR_MOBILE:
		// VR_OPENXR_MOBILE is only implemented on the Vulkan backend; fall back
		// to mono on OpenGL (which uses VR_OPENVR instead) and OpenGLES.
		if (V_GetBackend() == BACKEND_VULKAN)
		{
			const VRMode& vrmode = s3d::VKOpenXRDeviceMode::getInstance();
			const bool initialized = vrmode.IsInitialized();
			logModeSelect(mode, initialized ? mode : VR_MONO);
			return initialized ? &vrmode : &vrmi_mono;
		}
		logModeSelect(mode, VR_MONO);
		return &vrmi_mono;
#endif
	}
}

void VRMode::AdjustViewport(DFrameBuffer *screen) const
{
	screen->mSceneViewport.height = (int)(screen->mSceneViewport.height * mVerticalViewportScale);
	screen->mSceneViewport.top = (int)(screen->mSceneViewport.top * mVerticalViewportScale);
	screen->mSceneViewport.width = (int)(screen->mSceneViewport.width * mHorizontalViewportScale);
	screen->mSceneViewport.left = (int)(screen->mSceneViewport.left * mHorizontalViewportScale);

	screen->mScreenViewport.height = (int)(screen->mScreenViewport.height * mVerticalViewportScale);
	screen->mScreenViewport.top = (int)(screen->mScreenViewport.top * mVerticalViewportScale);
	screen->mScreenViewport.width = (int)(screen->mScreenViewport.width * mHorizontalViewportScale);
	screen->mScreenViewport.left = (int)(screen->mScreenViewport.left * mHorizontalViewportScale);
}

void VRMode::Present() const {
	GLRenderer->PresentStereo();
}

VSMatrix VRMode::GetHUDSpriteProjection() const
{
	VSMatrix mat;
	int w = screen->GetWidth();
	int h = screen->GetHeight();
	float scaled_w = w / mWeaponProjectionScale;
	float left_ofs = (w - scaled_w) / 2.f;
	mat.ortho(left_ofs, left_ofs + scaled_w, (float)h, 0, -1.0f, 1.0f);
	return mat;
}

VREyeInfo::VREyeInfo(float shiftFactor, float scaleFactor)
{
	mShiftFactor = shiftFactor;
	mScaleFactor = scaleFactor;
	m_isActive = false;
}

float VREyeInfo::getShift() const
{
	auto res = mShiftFactor * vr_ipd;
	return vr_swap_eyes ? -res : res;
}

VSMatrix VREyeInfo::GetProjection(float fov, float aspectRatio, float fovRatio, bool iso_ortho) const
{
	VSMatrix result;

	if (iso_ortho) // Orthographic projection for isometric viewpoint
	{
		double zNear = -3.0/fovRatio; // screen->GetZNear();
		double zFar = screen->GetZFar();

		double fH = tan(DEG2RAD(fov) / 2) / fovRatio;
		double fW = fH * aspectRatio * mScaleFactor;
		double left = -fW;
		double right = fW;
		double bottom = -fH;
		double top = fH;

		VSMatrix fmat(1);
		fmat.ortho((float)left, (float)right, (float)bottom, (float)top, (float)zNear, (float)zFar);
		return fmat;
	}
	else if (mShiftFactor == 0)
	{
		float fovy = (float)(2 * RAD2DEG(atan(tan(DEG2RAD(fov) / 2) / fovRatio)));
		result.perspective(fovy, aspectRatio, screen->GetZNear(), screen->GetZFar());
		return result;
	}
	else
	{
		double zNear = screen->GetZNear();
		double zFar = screen->GetZFar();

		// For stereo 3D, use asymmetric frustum shift in projection matrix
		// Q: shouldn't shift vary with roll angle, at least for desktop display?
		// A: No. (lab) roll is not measured on desktop display (yet)
		double frustumShift = zNear * getShift() / vr_screendist; // meters cancel, leaving doom units
																  // double frustumShift = 0; // Turning off shift for debugging
		double fH = zNear * tan(DEG2RAD(fov) / 2) / fovRatio;
		double fW = fH * aspectRatio * mScaleFactor;
		double left = -fW - frustumShift;
		double right = fW - frustumShift;
		double bottom = -fH;
		double top = fH;

		VSMatrix fmat(1);
		fmat.frustum((float)left, (float)right, (float)bottom, (float)top, (float)zNear, (float)zFar);
		return fmat;
	}
}

DAngle VREyeInfo::GetRenderFov(DAngle fallback) const
{
	return fallback;
}

VSMatrix VREyeInfo::GetHUDProjection() const
{
	VSMatrix mat;
	int w = screen->GetWidth();
	int h = screen->GetHeight();
	mat.ortho(0.f, (float)w, (float)h, 0.f, -1.0f, 1.0f);
	return mat;
}



/* virtual */
DVector3 VREyeInfo::GetViewShift(FRenderViewpoint& vp) const
{
	if (mShiftFactor == 0)
	{
		// pass-through for Mono view
		return { 0, 0, 0 };
	}
	else
	{
		float yaw = vp.HWAngles.Yaw.Degrees();
		double dx = -cos(DEG2RAD(yaw)) * vr_vunits_per_meter * getShift();
		double dy = sin(DEG2RAD(yaw)) * vr_vunits_per_meter * getShift();
		return { dx, dy, 0 };
	}
}

//Fishbiter's Function.. Thank-you!!
static DVector3 MapWeaponDir(AActor* actor, DAngle yaw, DAngle pitch, int hand = 0)
{
	LSMatrix44 mat;
	auto vrmode = VRMode::GetVRModeCached(true);
	if (multiplayer)
	{
		double pc = pitch.Cos();
		DVector3 direction = { pc * yaw.Cos(), pc * yaw.Sin(), -pitch.Sin() };
		return direction;
	}
	if (!vrmode->GetWeaponTransform(&mat, hand))
	{
		double pc = pitch.Cos();
		DVector3 direction = { pc * yaw.Cos(), pc * yaw.Sin(), -pitch.Sin() };
		return direction;
	}

	yaw -= actor->Angles.Yaw;
	pitch -= actor->Angles.Pitch;

	double pc = pitch.Cos();

	LSVec3 local = { (float)(pc * yaw.Cos()), (float)(pc * yaw.Sin()), (float)(-pitch.Sin()), 0.0f };

	DVector3 dir;
	dir.X = local.x * -mat[2][0] + local.y * -mat[0][0] + local.z * -mat[1][0];
	dir.Y = local.x * -mat[2][2] + local.y * -mat[0][2] + local.z * -mat[1][2];
	dir.Z = local.x * -mat[2][1] + local.y * -mat[0][1] + local.z * -mat[1][1];
	dir.MakeUnit();

	return dir;
}

static DVector3 MapAttackDir(AActor* actor, DAngle yaw, DAngle pitch)
{
	return MapWeaponDir(actor, yaw, pitch, 0);
}

static DVector3 MapOffhandDir(AActor* actor, DAngle yaw, DAngle pitch)
{
	return MapWeaponDir(actor, yaw, pitch, 1);
}

bool VRMode::RenderPlayerSpritesInScene() const
{
	return vr_render_weap_in_scene;
}

void VRMode::SetUp() const
{
	player_t* player = &players[consoleplayer];
	if (player && player->mo)
	{
		// A replaying VR demo owns PlayInVR and the pose below (p_vrdemo.cpp restores
		// them every tic); the renderer writing them here would overwrite the recording
		// with whatever this machine's headset -- or lack of one -- says.
		const bool replayingVrDemo = VRDemo_IsReplaying();
		// NOT IN MULTIPLAYER, and the playsim already said so: P_SpawnPlayer sets
		// `p->PlayInVR = !multiplayer && vr_mode != VR_MONO` (p_mobj.cpp) precisely
		// so VR and flatscreen peers do not simulate different player logic. This
		// line overwrote that on the next rendered frame, which made that guard dead
		// from the moment it was written.
		//
		// What it cost: PlayInVR is read on gameplay paths -- player.zs's
		// bJustAttacked auto-forward, and the melee weapons' `angle =
		// t.angleFromSource` (weaponfist.zs, weaponchainsaw.zs, weapongauntlets.zs
		// and five more). In co-op the desktop peer forced forwardmove and snapped
		// the attacker's angle to its target while the VR machine did neither, for
		// the SAME player on the SAME tic, and bJustAttacked then cleared on one
		// side only so it repeated.
		if (!replayingVrDemo)
			player->PlayInVR = !multiplayer && IsVR();
		player->mo->AttackDir = MapAttackDir;
		player->mo->OffhandDir = MapOffhandDir;

		// Multiplayer reconstructs attack pose from synchronized input in playsim.
		// In local VR, keep render-time attack pose aligned to the current controller
		// transform instead of resetting to the generic head-height fallback.
		if (!multiplayer && !replayingVrDemo)
		{
			player->mo->OverrideAttackPosDir = !puristmode && (IsVR() || vr_override_weap_pos);
			if (player->mo->OverrideAttackPosDir && IsVR())
			{
				VSMatrix attackTransform;
				if (GetWeaponTransform(&attackTransform, VR_MAINHAND))
				{
					const FLOATTYPE* attackMatrix = attackTransform.get();
					player->mo->AttackPos.X = attackMatrix[12];
					player->mo->AttackPos.Y = attackMatrix[14];
					player->mo->AttackPos.Z = attackMatrix[13];
					player->mo->AttackPitch = DAngle::fromDeg(VR_UseCinematicScreenLayer()
						? -weaponangles[PITCH] - r_viewpoint.Angles.Pitch.Degrees()
						: -weaponangles[PITCH]);
					player->mo->AttackAngle = DAngle::fromDeg(-90 + r_viewpoint.Angles.Yaw.Degrees() + (weaponangles[YAW] - playerYaw));
					player->mo->AttackRoll = DAngle::fromDeg(weaponangles[ROLL]);
					// Same value, kept somewhere the playsim will not zero it.
					player->mo->MainHandRoll = player->mo->AttackRoll;
				}

				VSMatrix offhandTransform;
				if (GetWeaponTransform(&offhandTransform, VR_OFFHAND))
				{
					const FLOATTYPE* offhandMatrix = offhandTransform.get();
					player->mo->OffhandPos.X = offhandMatrix[12];
					player->mo->OffhandPos.Y = offhandMatrix[14];
					player->mo->OffhandPos.Z = offhandMatrix[13];
					player->mo->OffhandPitch = DAngle::fromDeg(VR_UseCinematicScreenLayer()
						? -offhandangles[PITCH] - r_viewpoint.Angles.Pitch.Degrees()
						: -offhandangles[PITCH]);
					player->mo->OffhandAngle = DAngle::fromDeg(-90 + r_viewpoint.Angles.Yaw.Degrees() + (offhandangles[YAW] - hmdorientation[YAW]));
					player->mo->OffhandRoll = DAngle::fromDeg(offhandangles[ROLL]);
				}
			}
			else
			{
				double shootz = player->mo->Center() - player->mo->Floorclip + player->mo->AttackOffset();
				player->mo->AttackPos = player->mo->PosAtZ(shootz);
				player->mo->AttackAngle = r_viewpoint.Angles.Yaw - DAngle::fromDeg(90.);
				player->mo->AttackPitch = -r_viewpoint.Angles.Pitch;
				player->mo->OffhandPos = player->mo->PosAtZ(shootz);
				player->mo->OffhandAngle = r_viewpoint.Angles.Yaw - DAngle::fromDeg(90.);
				player->mo->OffhandPitch = -r_viewpoint.Angles.Pitch;
			}
		}
	}
}

//---------------------------------------------------------------------------
//
// The parameter hand_weapon is 0 for mainhand and 1 for offhand
// you can use the enum VR_MAINHAND and VR_OFFHAND
//
// allowAutoReverse lets a caller opt out of the non-dominant-hand mirror
// entirely. The mirror exists so that one weapon model can serve both hands,
// and it is normally suppressed only by +Weapon.NoAutoReverse on the weapon
// occupying the relevant slot -- which means a model drawn by anything that is
// not a Weapon cannot reach that flag at all, and would be mirrored whether or
// not that is correct for it. Pass false when the model supplies its own left
// and right variants.
//
//---------------------------------------------------------------------------
int VR_ControllerForHand(int hand)
{
	const bool rightHanded = vr_control_scheme < 10;
	return (hand == VR_OFFHAND) ? 1 - rightHanded : rightHanded;
}

bool VRMode::GetHmdTransform(VSMatrix* out, DVector3 bodyOfs, float* outBodyYaw, double yawOverride) const
{
	// No headset here. A replaying VR demo still has the recorded head (p_vrdemo.cpp).
	return VRDemo_GetHmdTransform(out, bodyOfs, outBodyYaw, yawOverride);
}

// THE SECOND HALF OF THE BODY FRAME (see VKOpenXRDeviceMode::GetHmdTransform for the first).
// Moved here unchanged so the live headset and a replaying VR demo finish the frame the
// same way.
void VR_FinishHmdTransform(VSMatrix* mat, double pixelstretch, float bodyYawDeg, const DVector3& bodyOfs, double unitsPerMeter)
{
	mat->rotate(-90 + bodyYawDeg, 0, 1, 0);

	// THE SEAT, in map units on the body's axes. Three corrections, and every
	// one of them was a separate live bug before this moved in here:
	//
	//   THE UNIT. Everything from the scale() above onward is stated in METRES.
	//   VSMatrix::translate post-multiplies, so its arguments are scaled by the
	//   basis columns already in the matrix. A map-unit offset pushed in raw is
	//   multiplied by vr_vunits_per_meter -- 34 by default. A hip holster asked
	//   for 9 units to the side arrived 306 units out and 600 below the floor,
	//   outside any sector, and was culled rather than drawn.
	//
	//   THE VERTICAL IS NOT THE SAME SCALE AS THE OTHER TWO. The Y column
	//   carries an extra 1/pixelstretch from the scale() two lines up, so it
	//   needs pixelstretch multiplied back in on its own. Dividing all three
	//   axes by vr_vunits_per_meter uniformly is the obvious fix and it is
	//   wrong: a 21-unit hip drop still lands 3.6 units low.
	//
	//   THE AXIS ROLES. After the heading rotation the local axes are the body's
	//   RIGHT, UP and BACKWARD -- not forward, up, right. Feeding (forward, up,
	//   right) straight in turns the whole rig a quarter circle, which reads as
	//   holsters on the wrong sides rather than as an obviously broken transform.
	const double vu = unitsPerMeter;
	if (bodyOfs.X != 0. || bodyOfs.Y != 0. || bodyOfs.Z != 0.)
	{
		mat->translate((float)( bodyOfs.Y / vu),
		               (float)( bodyOfs.Z * pixelstretch / vu),
		               (float)(-bodyOfs.X / vu));
	}

	// AND NOW HAND BACK A MAP-UNIT FRAME, because the caller draws a MODEL in it
	// and a model's vertices are map units like every other world model's.
	//
	// Without this the metres scale above multiplies the MESH too, not just the
	// seat: a weapon solved to a 4.4-unit radius is drawn with a 4.4-METRE one,
	// which puts the camera inside it, and a mesh seen from the inside back-face
	// culls to nothing. That is the "not drawn" half of the holster report, and
	// it is not a distance or a culling problem at all.
	//
	// The alternative -- leaving the frame in metres and dividing in MODELDEF, as
	// the hand-frame models do -- bakes vr_vunits_per_meter into content, so
	// moving that slider would silently resize every worn thing. The hand path
	// carries that debt already because its content depends on it; the body path
	// is new and does not have to.
	//
	// The three factors are the exact inverses of the column lengths above: vu on
	// X and Z, vu/pixelstretch on Y. Z stays POSITIVE so the negative-Z handedness
	// of the frame is preserved -- flipping it here would silently invert triangle
	// winding on everything worn.
	mat->scale(1.f / (float)vu, (float)pixelstretch / (float)vu, 1.f / (float)vu);
}

bool VRMode::GetWeaponTransform(VSMatrix* out, int hand_weapon, bool allowAutoReverse, bool *mirroredOut) const
{
	player_t* player = &players[consoleplayer];
	bool autoReverse = allowAutoReverse;
	if (allowAutoReverse && player)
	{
		AActor *weap = (hand_weapon == VR_OFFHAND) ? player->OffhandWeapon : player->ReadyWeapon;
		autoReverse = weap == nullptr || !(weap->IntVar(NAME_WeaponFlags) & WIF_NO_AUTO_REVERSE);
	}
	int hand = VR_ControllerForHand(hand_weapon);
	if (mirroredOut) *mirroredOut = false;
	// A VR DEMO ANSWERS FIRST, and only for the playsim: while one replays, the recorded
	// hand; while one records, during a tic, the hand captured at the tic's start (the
	// same frame the live call returns, and the one written to the demo).
	//
	// With the controller that hand was on when it was recorded, because the mirror
	// below keys off the controller and the hand-to-controller map is a user setting --
	// a left-handed viewer would otherwise mirror a right-handed recording. The
	// auto-reverse itself still comes from the weapon held now, as it always has.
	int recordedController = hand;
	if (VRDemo_GetWeaponHandTransform(hand_weapon, out, &recordedController))
	{
		hand = recordedController;
	}
	else if (!GetHandTransform(hand, out))
	{
		return false;
	}

	if (!hand && autoReverse)
	{
		out->scale(-1.0f, 1.0f, 1.0f);
		// Reported rather than left for the caller to re-derive: GetHandTransform's
		// own -Z world scale makes the determinant negative in BOTH hands, so the
		// matrix alone cannot say whether this mirror went in.
		if (mirroredOut) *mirroredOut = true;
	}
	return true;
}

float length(float x, float y)
{
    return sqrtf(powf(x, 2.0f) + powf(y, 2.0f));
}

#define NLF_DEADZONE 0.1
#define NLF_POWER 2.2

float nonLinearFilter(float in)
{
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

float VR_GetAnalogTurnResponseScale(float smoothTurnSetting)
{
    const float clamped = clamp(smoothTurnSetting, 0.0f, 10.0f);

    if (clamped <= 0.0f)
    {
        return 15.0f;
    }

    return 1.0f + (10.0f - clamped);
}

float VR_ApplyAnalogSmoothTurn(float turnAxis, float maxTurnRateDegPerSec, float deltaSeconds, float responseScale, float& currentTurnRateDegPerSec)
{
    constexpr float analogTurnDeadzone = 0.10f;
    constexpr float analogTurnResponse = 8.0f;

    if (deltaSeconds < 0.0f)
    {
        deltaSeconds = 0.0f;
    }

    float targetTurnRate = 0.0f;
    const float absTurnAxis = fabsf(turnAxis);
    if (absTurnAxis > analogTurnDeadzone)
    {
        float t = (absTurnAxis - analogTurnDeadzone) / (1.0f - analogTurnDeadzone);
        if (t < 0.0f)
        {
            t = 0.0f;
        }
        else if (t > 1.0f)
        {
            t = 1.0f;
        }

        // Ease in and out so the turn starts gently, then ramps toward the cap.
        const float eased = t * t * (3.0f - 2.0f * t);
        targetTurnRate = (turnAxis > 0.0f ? -1.0f : 1.0f) * maxTurnRateDegPerSec * eased;
    }

    const float response = 1.0f - expf(-analogTurnResponse * responseScale * deltaSeconds);
    currentTurnRateDegPerSec += (targetTurnRate - currentTurnRateDegPerSec) * response;
    return currentTurnRateDegPerSec * deltaSeconds;
}

bool between(float min, float val, float max)
{
    return (min < val) && (val < max);
}

// Function to normalize an angle to the [-180, 180] range
double normalizeAngle(double angle) {
	// Reduce the angle to [0, 359]
	angle = fmod(angle, 360.0);
	// Force it to be the positive remainder
	angle = fmod(angle + 360.0, 360.0);
	// Normalize to the [-180, 180] range
	if (angle > 180.0) {
		angle -= 360.0;
	}
	return angle;
}

extern float weaponoffset[3];
extern float weaponangles[3];
extern float offhandoffset[3];
extern float offhandangles[3];
extern float hmdPosition[3];

ADD_STAT(vrstats)
{
	FString out;

	player_t* player = &players[consoleplayer];
	if (player && player->mo)
	{
		out.AppendFormat("AttackPos: X=%2.f, Y=%2.f, Z=%2.f\n"
			"AttackAngle=%2.f, AttackPitch=%2.f, AttackRoll=%2.f\n", 
			player->mo->AttackPos.X, player->mo->AttackPos.Y, player->mo->AttackPos.Z,
			player->mo->AttackAngle.Degrees(), player->mo->AttackPitch.Degrees(), player->mo->AttackRoll.Degrees());

		out.AppendFormat("OffhandPos: X=%2.f Y=%2.f Z=%2.f\n"
			"OffhandAngle=%2.f, OffhandPitch=%2.f, OffhandRoll=%2.f\n", 
			player->mo->OffhandPos.X, player->mo->OffhandPos.Y, player->mo->OffhandPos.Z,
			player->mo->OffhandAngle.Degrees(), player->mo->OffhandPitch.Degrees(), player->mo->OffhandRoll.Degrees());
	}

	out.AppendFormat("weaponangles: yaw=%2.f, pitch=%2.f, roll=%2.f\n",
		weaponangles[YAW], weaponangles[PITCH], weaponangles[ROLL]);

	out.AppendFormat("weaponoffset: x=%1.3f, y=%1.3f, z=%1.3f\n",
		weaponoffset[0], weaponoffset[1], weaponoffset[2]);
	
	out.AppendFormat("offhandangles: yaw=%2.f, pitch=%2.f, roll=%2.f\n",
		offhandangles[YAW], offhandangles[PITCH], offhandangles[ROLL]);

	out.AppendFormat("hmdorientation: yaw=%2.f, pitch:%2.f, roll:%2.f\n", 
		hmdorientation[YAW], hmdorientation[PITCH], hmdorientation[ROLL]);

	out.AppendFormat("hmdpos: x=%1.3f, y:%1.3f, z:%1.3f\n", 
		hmdPosition[0], hmdPosition[1], hmdPosition[2]);

	out.AppendFormat("gamestate:%d - menuactive:%d - paused:%d", gamestate, menuactive, paused);

	return out;
}

// [BB] Script-side VR input suppression -- see the note in hw_vrmodes.h.
//
// A plain global rather than a cvar because it is transient: a mod sets it while
// its selector is open and clears it on close, and a value that outlived a crash
// would leave the player unable to turn with no way to find out why.
static bool g_scriptVRInputSuppressed = false;

void VR_SetScriptInputSuppressed(bool suppressed)
{
	g_scriptVRInputSuppressed = suppressed;
}

bool VR_IsScriptInputSuppressed()
{
	return g_scriptVRInputSuppressed;
}

// [BB] Script-forced laser sight -- see the note in vmthunks.cpp.
//
// An override rather than a cvar write: vr_laser_sight is archived, and a mod
// that wrote to it would be editing the player's saved settings to draw a line
// for four seconds.
// Per-hand, because an in-world menu is worn on ONE hand. Forcing both lit the
// hand still holding a gun with a beam clamped to the menu's distance -- a
// second pointer, in the wrong place, doing nothing.
static bool g_scriptLaserForced = false;
static int  g_scriptLaserHand = -1;   // -1 both, 0 main, 1 off

void VR_SetScriptLaserForced(bool forced, int hand)
{
	g_scriptLaserForced = forced;
	g_scriptLaserHand = forced ? hand : -1;
}

bool VR_IsScriptLaserForced()
{
	return g_scriptLaserForced;
}

bool VR_IsScriptLaserForcedFor(bool offhand)
{
	if (!g_scriptLaserForced) return false;
	if (g_scriptLaserHand < 0) return true;
	return g_scriptLaserHand == (offhand ? 1 : 0);
}

// [BB] Script-supplied laser termination distance. 0 means "no opinion".
//
// Republished every frame by whoever wants it, the same way the volumetric beam
// is: a value left behind by a menu that closed would clamp the player's laser
// for the rest of the session with nothing to point at.
static double g_scriptLaserRange = 0.0;

void VR_SetScriptLaserRange(double range)
{
	g_scriptLaserRange = range;
}

double VR_GetScriptLaserRange()
{
	return g_scriptLaserRange;
}

// Fires a pulse directly, bypassing every gameplay trigger. This separates
// "the haptic pipeline is broken" from "nothing is calling it", which is
// otherwise guesswork -- both present as a silent controller.
CCMD(vr_haptictest)
{
	const VRMode *vrmode = VRMode::GetVRModeCached();
	if (vrmode == nullptr)
	{
		Printf("[HAPTIC] no VR mode active -- nothing to pulse\n");
		return;
	}
	Printf("[HAPTIC] test: 300ms at 0.8 on both hands, vr_enable_haptics=%d\n", *vr_enable_haptics ? 1 : 0);
	vrmode->Vibrate(300.0f, 0, 0.8f);
	vrmode->Vibrate(300.0f, 1, 0.8f);
}

// [BB] Script-requested haptic pulse -- see the note in hw_vrmodes.h.
//
// The handedness swap is the whole reason this exists rather than exposing
// Vibrate directly: Vibrate's channel is a PHYSICAL side (0 left, 1 right),
// while everything script-facing is addressed as main/off. Getting that
// backwards buzzes the wrong wrist, which is a maddening thing to debug because
// it still works -- just on the other arm. Same swap the native wheel does in
// hw_vrwheel.cpp.
void VR_ScriptHaptic(int hand, double intensity, double durationMs)
{
	const VRMode *vrmode = VRMode::GetVRModeCached();
	if (vrmode == nullptr) return;

	if (hand != VR_MAINHAND && hand != VR_OFFHAND) hand = VR_MAINHAND;

	const bool rightHanded = vr_control_scheme < 10;
	const int channel = rightHanded ? (hand == VR_MAINHAND ? 1 : 0) : hand;

	// Clamped rather than trusted. A script asking for a two-second pulse at
	// full strength is a script with a bug, and the controller has no way to
	// refuse it.
	const float amp = (float)clamp(intensity, 0.0, 1.0);
	const float ms  = (float)clamp(durationMs, 0.0, 500.0);

	vrmode->Vibrate(ms, channel, amp);
}
