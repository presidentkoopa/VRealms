/*
** hw_vrmodes.h
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

#pragma once

#include "r_utility.h"
#include "matrix.h"
#include <cstdint>

class DFrameBuffer;
class FCanvasTexture;
class FCanvas;
class FGameTexture;
class VulkanRenderDevice;
class VulkanImage;
class VkTextureImage;

enum
{
	VR_MONO = 0,
	VR_GREENMAGENTA = 1,
	VR_REDCYAN = 2,
	VR_SIDEBYSIDEFULL = 3,
	VR_SIDEBYSIDESQUISHED = 4,
	VR_LEFTEYEVIEW = 5,
	VR_RIGHTEYEVIEW = 6,
	VR_QUADSTEREO = 7,
	VR_SIDEBYSIDELETTERBOX = 8,
	VR_AMBERBLUE = 9,
	VR_OPENVR = 10,
	VR_TOPBOTTOM = 11,
	VR_ROWINTERLEAVED = 12,
	VR_COLUMNINTERLEAVED = 13,
	VR_CHECKERINTERLEAVED = 14,
	VR_OPENXR_MOBILE = 15,

	VR_MAINHAND = 0,
	VR_OFFHAND = 1
};

// RS FORK -- MAIN/OFF HAND IS NOT A CONTROLLER NUMBER.
//
// VR_MAINHAND/VR_OFFHAND say which hand a thing belongs to. VRMode::
// GetHandTransform takes the physical CONTROLLER instead, and for a right-handed
// player the main hand is controller 1. Passing VR_MAINHAND (0) straight in
// returns the OFF hand's pose. That silently broke the draw-rate hand drive:
// a slide followed the hand holding its own gun, which never moves relative to
// that gun, so the slide and the magazine never moved at all. GetWeaponTransform
// always did this conversion inline; it lives here now so every caller uses the
// same rule.
int VR_ControllerForHand(int hand);

struct HWDrawInfo;
struct HWViewpointUniforms;

struct VRBenchmarkInfo
{
	bool IsVR = false;
	bool IsOpenXR = false;
	bool MultiviewEnabled = false;
	bool MultiviewSupported = false;
	bool MultiviewActive = false;
	bool SceneLayered = false;
	bool PostprocessLayered = false;
	bool FinalizeLayered = false;
	bool DirectXrRender = false;
	bool DedicatedMirrorTextures = false;
	uint32_t ViewCount = 0;
	uint32_t ViewMask = 0;
	uint32_t RecommendedWidth = 0;
	uint32_t RecommendedHeight = 0;
	uint32_t PresentWidth = 0;
	uint32_t PresentHeight = 0;
	int SceneSamples = 1;
	int DesktopViewMode = 0;
	int RequestedRefreshRate = 0;
	int SyncMode = 0;
	float RuntimeRefreshRate = 0.0f;
	float RenderScale = 1.0f;
};

struct VRHudSurface
{
	// Shared texture-backed HUD surface for VR, and later canvas/model texture reuse.
	VRHudSurface();
	~VRHudSurface();

	void Clear();
	void EnsureSize(int width, int height);
	void BeginUpdate();
	void EndUpdate();
	void MarkDirty();
	bool IsCanvasLive() const;
	bool IsValid() const { return Texture != nullptr; }
	bool HasGameTexture() const { return GameTexture != nullptr; }
	int GetWidth() const { return Texture != nullptr ? Texture->GetWidth() : 0; }
	int GetHeight() const { return Texture != nullptr ? Texture->GetHeight() : 0; }
	FCanvasTexture* GetTexture() const { return Texture; }
	FGameTexture* GetGameTexture() const { return GameTexture; }
	FCanvas* GetCanvas() const { return Canvas; }

private:
	FCanvasTexture* Texture = nullptr;
	FGameTexture* GameTexture = nullptr;
	FCanvas* Canvas = nullptr;
};

struct VREyeInfo
{
	float mShiftFactor;
	float mScaleFactor;

	VREyeInfo() {}
	VREyeInfo(float shiftFactor, float scaleFactor);
	virtual ~VREyeInfo() {}

	virtual VSMatrix GetProjection(float fov, float aspectRatio, float fovRatio, bool iso_ortho) const;
	virtual DAngle GetRenderFov(DAngle fallback) const;
	virtual VSMatrix GetHUDProjection() const;
	virtual DVector3 GetViewShift(FRenderViewpoint& vp) const;
	virtual void AdjustViewpointUniforms(HWViewpointUniforms& uniforms) const {}
	virtual void SetUp() const { m_isActive = true; }
	virtual void TearDown() const { m_isActive = false; }
	virtual void AdjustHud() const {}
	virtual void AdjustBlend(HWDrawInfo* di) const {}
	bool isActive() const { return m_isActive; }

private:
	mutable bool m_isActive;
	float getShift() const;

};

struct VRMode
{
	int mEyeCount;
	float mHorizontalViewportScale;
	float mVerticalViewportScale;
	float mWeaponProjectionScale;
	VREyeInfo* mEyes[2];

	VRMode(int eyeCount, float horizontalViewportScale, 
		float verticalViewportScalem, float weaponProjectionScale, VREyeInfo eyes[2]);
	virtual ~VRMode() {}

	static const VRMode *GetVRMode(bool toscreen = true);
	static const VRMode *GetVRModeCached(bool toscreen = true);
	virtual void AdjustViewport(DFrameBuffer *fb) const;
	VSMatrix GetHUDSpriteProjection() const;
	virtual VSMatrix GetHUDProjection() const { return GetHUDSpriteProjection(); }

	/* hooks for setup and cleanup operations for each stereo mode */
	virtual void SetUp() const;
	virtual void TearDown() const {};

	virtual bool IsMono() const { return mEyeCount == 1; }
	virtual bool IsVR() const { return false; }
	virtual bool GetRecommendedRenderSize(int& outWidth, int& outHeight) const { outWidth = 0; outHeight = 0; return false; }
	virtual bool ShouldUseRecommendedRenderSizeThisFrame() const { return false; }
	virtual bool SupportsMultiview() const { return false; }
	virtual bool ShouldUseMultiviewThisFrame() const { return false; }
	virtual int GetMultiviewLayerCount() const { return 1; }
	virtual uint32_t GetMultiviewViewMask() const { return 0; }
	virtual bool ShouldUseScreenLayerForCurrentFrame() const { return false; }
	virtual void AdjustPlayerSprites(FRenderState &state, int hand = 0) const {};
	virtual void UnAdjustPlayerSprites(FRenderState &state) const {};
	virtual void AdjustCrossHair() const {}
	virtual void UnAdjustCrossHair() const {}

	virtual void SetupOverlay() {}
	virtual void UpdateOverlaySettings() const {}
	virtual void DrawControllerModels(HWDrawInfo* di, FRenderState& state) const {}
	virtual void DrawMountedHud(HWDrawInfo* di, FRenderState& state) const {}
	virtual bool IsRenderingVirtualScreen() const { return false; }
	// True when this frame keeps the live stereo world in the headset behind an
	// open menu (OpenXR: vr_menu_keep_world). Scene-wide menu effects that would
	// hide the world -- the menu blur -- check it. Default false: inert elsewhere.
	virtual bool IsMenuOverWorldFrame() const { return false; }
	virtual bool RenderVirtualScreen() const { return false; }
	virtual void FinalizeEyeImage(VulkanRenderDevice* fb, int eyeIndex) const {}
	virtual bool RenderDesktopMirror(VulkanRenderDevice* fb, VulkanImage* dstImage) const { return false; }
	
	virtual void Present() const;
	virtual void PollXREvents() const {}
	virtual bool BeginXRFrame() const { return true; }
	virtual void ApplyRefreshRate() const {}
	virtual bool AcquireXRSwapchain() const { return true; }
	virtual bool SubmitFrame() const { return true; }

	virtual bool GetHandTransform(int hand, VSMatrix* out) const { return false; }
	// THE FRAME EVERYTHING WORN IS PLACED IN, at the display's clock.
	//
	// The hand transforms above let a world model ride a controller at draw rate.
	// Nothing offered the same for the BODY, so anything anchored to it had to
	// sample the head pose from script at 35Hz and re-place itself -- and between
	// two samples the whole rig swims, the further from the anchor the wider the
	// arc it sweeps.
	//
	// YAW ONLY, and that is the definition rather than a simplification. A
	// holster does not tip when you look at the floor; a body-worn thing that
	// pitched and rolled with the headset would be attached to your face rather
	// than to you.
	// bodyOfs seats something INSIDE that frame, in MAP UNITS, on the body's own
	// axes: X forward, Y right, Z up -- the same convention as
	// AActor::FollowBodyOfs and as _ofs_x/_ofs_y/_ofs_z everywhere else here.
	//
	// IT IS APPLIED BY THE IMPLEMENTATION, NOT BY THE CALLER, and that is the
	// entire reason it is a parameter instead of a translate() the caller makes
	// afterwards. Placing it correctly needs three things that only exist inside
	// the function that built the basis: the metres-per-map-unit scale, the extra
	// pixelstretch on the vertical axis alone, and the fact that after the
	// heading rotation the local axes are (right, up, backward) rather than
	// (forward, up, right). A caller outside has to re-derive all three, and the
	// first one to try got all three wrong at once.
	//
	// outBodyYaw reports the heading the frame was built on, in degrees, so a
	// caller holding WORLD angles can subtract it rather than applying the
	// heading twice. Both parameters are optional and default to inert.
	// yawOverride replaces the frame's own heading when it is not NaN. A caller
	// that filters its heading cannot express anything in the renderer's basis,
	// because that basis is not visible from script -- so it supplies the heading
	// instead of trying to match it.
	virtual bool GetHmdTransform(VSMatrix* out, DVector3 bodyOfs = DVector3(0, 0, 0), float* outBodyYaw = nullptr, double yawOverride = NAN) const { return false; }
	// mirroredOut, when given, says whether the frame came back mirrored (the
	// auto-reverse scale on controller 0) -- see AActor::FollowHandRot.
	virtual bool GetWeaponTransform(VSMatrix* out, int hand = 0, bool allowAutoReverse = true, bool *mirroredOut = nullptr) const;
	virtual bool RenderPlayerSpritesInScene() const;
	virtual bool GetTeleportLocation(DVector3 &out) const { return false; }
	virtual bool IsInitialized() const { return true; }
	virtual void Vibrate(float duration, int channel, float intensity) const { }
	virtual bool GetBenchmarkInfo(VRBenchmarkInfo& out) const { out.IsVR = IsVR(); return false; }
};

void VR_HapticEvent(const char* event, int position, int intensity, float angle, float yHeight );

// [BB] Script-side VR input suppression.
//
// The native wheel already suppresses turning and stick movement while it is
// open (VRWheel_ShouldSuppressStickMove and friends), but every one of those
// lives in C++ with no ZScript reach -- so a mod that puts its own selector in
// the world has no way to say "the stick is mine right now", and snap turn
// fires while you are trying to pick something with it.
//
// One flag, set and cleared by script, checked in the same two places the
// native wheel is checked. Deliberately not a cvar: it is transient state, not
// a preference, and it must not survive a crash or end up archived.
void VR_SetScriptInputSuppressed(bool suppressed);
bool VR_IsScriptInputSuppressed();

// [BB] A script-side menu can force the laser sight on for its duration without
// writing to the archived cvars that normally control it.
//
// hand: -1 both, 0 main, 1 off. A menu worn on one hand wants a pointer on that
// hand only -- forcing both put a second beam on the hand still holding a gun.
// While forced, that hand's laser also ignores the empty-hand and melee-weapon
// gates: the pointer is the menu's cursor, and whether the hand happens to be
// holding a shotgun or nothing at all has no bearing on needing one.
void VR_SetScriptLaserForced(bool forced, int hand = -1);
bool VR_IsScriptLaserForced();
bool VR_IsScriptLaserForcedFor(bool offhand);

// [BB] Where the laser should stop, in map units. 0 means the engine decides.
// Set by a script menu so the beam ends at the panel it is selecting rather than
// passing through it. Shortening only.
void VR_SetScriptLaserRange(double range);
double VR_GetScriptLaserRange();

// [BB] A haptic pulse a script can ask for, addressed by ABSTRACT hand
// (VR_MAINHAND / VR_OFFHAND) rather than physical side -- the handedness swap
// that Vibrate's channel argument needs is done here, once, instead of in every
// caller. Duration is milliseconds, intensity 0..1; both are clamped.
//
// Note that VR_HapticEvent, which the playsim calls in a dozen places, is an
// empty stub on this platform. VRMode::Vibrate is the path that actually
// reaches the controller.
void VR_ScriptHaptic(int hand, double intensity, double durationMs);
void QzDoom_GetScreenRes(uint32_t *width, uint32_t *height);

extern bool weaponStabilised;

VRHudSurface& GetVRHudSurface();
void VR_DestroyHudSurface();
void VR_EnsureHudSurface(int width, int height);
void VR_InitPortableHudBinding();
bool VR_UsePortableHud();
bool VR_ShouldDrawMountedHud();
void VR_SuppressMountedHudForFrames(int frames);
bool VR_GetMountedHudTransform(VSMatrix& out);
bool VR_IsNetWaitShellActive();
bool VR_CanUseNetWaitShell();
bool VR_NetWaitLoop(bool (*timer_callback)(void*), void* userdata);
void VR_RenderNetWaitShellContents(int width, int height, bool outside2D = false);
