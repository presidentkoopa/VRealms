/*
** openxr_stubs_posix.cpp
**
** OpenXR is wired up for Windows only (src/CMakeLists.txt, ENABLE_OPENXR feeds
** PLAT_WIN32_SOURCES), but the SDL video path, the Vulkan device and the VR
** wheel call into it unconditionally. These stubs let a non-Windows build link
** and simply report "no OpenXR runtime". Compiled only on non-Windows targets,
** so the Windows build is unaffected. Added 2026-10-01 for the headless Linux
** verification build.
*/

#include "common/rendering/stereo3d/openxr/oxr_loader.h"
#include "c_cvars.h"

// Defined in vk_openxrdevice.cpp on Windows; the menu ZScript and gl_openvr.cpp need them everywhere.
CVAR(Bool, vr_menu_pointer, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_menu_pointer_color, 0xffffff, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

bool QueryOpenXRVulkanBootstrapInfo(OpenXRVulkanBootstrapInfo& outInfo)
{
	(void)outInfo;
	return false;
}

#ifdef HAVE_VULKAN
bool QueryOpenXRVulkanPreferredPhysicalDevice(VkInstance instance, VkPhysicalDevice& outPhysicalDevice)
{
	(void)instance; (void)outPhysicalDevice;
	return false;
}
#endif

namespace s3d
{
	bool OpenXR_GetThumbstick(int abstractHand, float& x, float& y)
	{
		(void)abstractHand;
		x = y = 0.f;
		return false;
	}
}
