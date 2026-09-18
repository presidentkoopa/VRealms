#pragma once

#include "hw_vrmodes.h"
#include "vulkan/system/vk_renderdevice.h"
#include "common/rendering/stereo3d/openxr/oxr_loader.h"
#include "zvulkan/vulkanobjects.h"
#include "p_vrdemo.h"

#include <vector>
#include <memory>

class VkTextureImage;

namespace s3d {

bool OpenXRInputDeviceAvailable();
bool OpenXROnHandIsRight();

// [BB] Raw per-hand thumbstick, addressed by abstract hand (VR_MAINHAND /
// VR_OFFHAND) rather than by physical side, so callers do not have to repeat
// the handedness swap that Vibrate's channel argument needs.
//
// The device already reads both sticks per hand every frame as full-precision
// Vector2f, then splits them by ROLE: the movement hand's pair reaches game
// code as remote_movementForward/Sideways, while the turn hand contributes only
// its X, accumulated into snapTurn, and its Y is discarded outright. Anything
// wanting both sticks -- an in-world menu with a ring per hand -- cannot be
// built from what escapes. This returns the values before that split.
//
// Untouched by the deadzone and response curve applied on the movement path, so
// a caller gets to choose its own; a menu wants a different feel from walking.
// Returns false when not in OpenXR VR or the hand has no tracked stick, leaving
// x and y alone.
bool OpenXR_GetThumbstick(int abstractHand, float& x, float& y);

class VKOpenXRDeviceEyePose : public VREyeInfo
{
public:
	friend class VKOpenXRDeviceMode;

	VKOpenXRDeviceEyePose(int eye);
	virtual ~VKOpenXRDeviceEyePose() override;
	virtual VSMatrix GetProjection(FLOATTYPE fov, FLOATTYPE aspectRatio, FLOATTYPE fovRatio, bool iso_ortho) const override;
	virtual DAngle GetRenderFov(DAngle fallback) const override;
	virtual VSMatrix GetHUDProjection() const override;
	DVector3 GetViewShift(FRenderViewpoint& vp) const override;
	virtual void AdjustViewpointUniforms(HWViewpointUniforms& uniforms) const override;
	virtual void SetUp() const override;
	virtual void TearDown() const override;
	virtual void AdjustHud() const override;
	virtual void AdjustBlend(HWDrawInfo* di) const override;

protected:
	int eye;
	mutable VSMatrix projection;
	mutable XrPosef currentEyePose{};
	mutable XrFovf currentFov{};
};

// What a hand's grip button means on a given frame.
//
// Grip carries several jobs at once in VR -- two-hand stabilize, the
// secondary-button shift layer, a plain bind, and now holsters -- and every
// one of them used to test the raw button independently. Nothing stopped two
// firing on the same frame, and nothing could tell you which one "won",
// because none of them knew the others existed.
//
// This is the single place that decides. Resolved once per hand per frame in
// PRIORITY ORDER, and every consumer asks what the context is rather than
// asking whether the button is down. Adding a fifth job means adding a value
// here and one branch in ResolveGripContexts -- not another blind test
// scattered somewhere else.
enum EGripContext
{
	GRIPCTX_None = 0,   // grip not held, or held with nothing claiming it
	GRIPCTX_Holster,    // hand is inside a holster volume; highest priority
	GRIPCTX_Stabilize,  // off hand supporting the main hand's weapon
	GRIPCTX_Modifier,   // dominant grip acting as the shift layer
	GRIPCTX_Plain,      // ordinary grip, whatever it is bound to
	// Appended rather than inserted at its priority position. These values are
	// published to ZScript and mods already read them, so the numbering is API
	// and renumbering it would silently change what every existing test means.
	// Priority lives in the branch order of ResolveGripContexts, not here.
	GRIPCTX_Object,     // hand is closed on a physical thing that script claimed
	GRIPCTX_Hardpoint,  // hand is at a body hardpoint (not a weapon holster)
};

// WHAT a hand is closed on, as distinct from what its grip MEANS.
//
// Deliberately a separate field from EGripContext, because they answer
// different questions: "the off hand spent its grip on an object" is a
// priority question, and "that object is a shotgun forend" is a pose question.
// Folding both into one enum would force every consumer of one to care about
// the other.
//
// The engine cannot work any of this out for itself -- there is no shell in
// the world for it to test against -- so script CLAIMS a subject and the
// engine arbitrates, exactly the split HolsterClaim* already uses. The one
// subject the engine does know is Holster, and it fills that in itself.
//
// The values are pose-shaped rather than object-shaped: a hand does not care
// whether it is on an SMG or a shotgun, it cares whether it is wrapping a fat
// cylinder or squeezing a vertical grip. Two guns held the same way should
// claim the same subject.
enum EGripSubject
{
	GRIPSUBJ_None = 0,
	GRIPSUBJ_Round,      // one pistol or rifle cartridge -- fingertip pinch
	GRIPSUBJ_Shell,      // a shotgun shell -- fatter, and a fuller grip
	GRIPSUBJ_Inserting,  // that round being pushed home, thumb driving it
	GRIPSUBJ_Magazine,   // magazine, clip or speedloader -- wrapped in the palm
	GRIPSUBJ_Grip,       // a pistol grip: a one-handed gun, or a longarm's firing hand
	GRIPSUBJ_Forend,     // pump or handguard -- a fat cylinder across the palm
	GRIPSUBJ_Foregrip,   // vertical foregrip, as on an SMG
	GRIPSUBJ_Slide,      // slide or charging handle -- pinched from the sides
	GRIPSUBJ_Support,    // supporting the OTHER hand's weapon, wrapped round its fist
	GRIPSUBJ_Holster,    // inside a WEAPON holster volume: reaching, not yet holding
	// Inside the AMMUNITION pouch on the chest, which is a different place with
	// a different answer: a hand in a holster is fetching a gun, a hand in the
	// pouch is fetching a magazine. They cannot share a value, because the mod
	// reading this has to decide which of those two things to hand over.
	//
	// The engine itself writes GRIPSUBJ_Holster (see the arbitration below,
	// driven by HolsterClaimMain/Off), so the two would otherwise be
	// indistinguishable the moment both volumes exist.
	//
	// Appended before MAX rather than inserted, so every existing value keeps
	// its number and nothing already compiled against them shifts.
	GRIPSUBJ_Pouch,
	GRIPSUBJ_MAX
};

// Capacitive touch reports skin CONTACT without a press, which is what says
// where a finger is resting rather than what it is doing -- a thumb lying on
// the stick versus lifted clear, an index along the frame versus on the
// trigger. Published to ZScript as a bitfield so more sensors (Index reads
// all four fingers) can be added later without changing the field.
enum
{
	FINGERTOUCH_THUMB = 1 << 0,
	FINGERTOUCH_INDEX = 1 << 1,
};

class VKOpenXRDeviceMode : public VRMode
{
public:
	enum class FrameRenderMode
	{
		GameplayEyes,
		VirtualScreen,
		// vr_menu_keep_world: a menu is open over a level, but the headset keeps
		// the live stereo world (GameplayEyes' eye path, recommended size, HMD
		// pose) and the menu goes on the virtual-screen quad WITHOUT the flat
		// scene copy and the opaque backdrop wall. Exists so renderer-read
		// sliders (holster/model placement) can be tuned while looking at the
		// result on your body. ShouldUseScreenLayerForCurrentFrame() is true
		// here (quad + pointer), ShouldUseRecommendedRenderSizeThisFrame() too.
		MenuOverWorld
	};

	friend class VKOpenXRDeviceEyePose;
	static const VRMode& getInstance();

	VKOpenXRDeviceMode();
	virtual ~VKOpenXRDeviceMode() override;
	
	virtual void SetUp() const override;
	virtual void TearDown() const override;
	virtual bool IsVR() const override { return true; }
	virtual VSMatrix GetHUDProjection() const override;
	virtual void Present() const override;
	virtual void PollXREvents() const override;
	virtual bool BeginXRFrame() const override;
	virtual void ApplyRefreshRate() const override;
	virtual bool AcquireXRSwapchain() const override;
	virtual bool SubmitFrame() const override;
	virtual bool SupportsMultiview() const override { return xrMultiviewSupported; }
	virtual bool ShouldUseMultiviewThisFrame() const override;
	virtual int GetMultiviewLayerCount() const override;
	virtual uint32_t GetMultiviewViewMask() const override;
	virtual void AdjustViewport(DFrameBuffer* screen) const override;
	virtual void AdjustPlayerSprites(FRenderState& state, int hand = 0) const override;
	virtual void UnAdjustPlayerSprites(FRenderState& state) const override;
	virtual void DrawMountedHud(HWDrawInfo* di, FRenderState& state) const override;
	virtual bool IsRenderingVirtualScreen() const override;
	virtual bool RenderVirtualScreen() const override;
	virtual void FinalizeEyeImage(VulkanRenderDevice* fb, int eyeIndex) const override;
	virtual bool RenderDesktopMirror(VulkanRenderDevice* fb, VulkanImage* dstImage) const override;
	bool RenderSpectatorEye(VulkanRenderDevice* fb, int eyeIndex, float sourceScaleX, float sourceScaleY, float sourceOffsetX, float sourceOffsetY) const;	// [SPECTATOR]
	bool EnsureSpectatorTexture(VulkanRenderDevice* fb, int width, int height) const;	// [SPECTATOR]
	bool GetRecommendedRenderSize(int& outWidth, int& outHeight) const override;
	virtual bool ShouldUseRecommendedRenderSizeThisFrame() const override;
	virtual bool ShouldUseScreenLayerForCurrentFrame() const override;
	virtual bool IsMenuOverWorldFrame() const override;
	virtual bool IsInitialized() const override;
	bool HasActiveInputSession() const;
	
	virtual bool GetHmdTransform(VSMatrix* out, DVector3 bodyOfs = DVector3(0, 0, 0), float* outBodyYaw = nullptr, double yawOverride = NAN) const override;
	virtual bool GetHmdBaseTransform(VSMatrix* out, double* outPixelStretch, float* outBodyYaw) const override;
	virtual bool GetHandTransform(int hand, VSMatrix* out) const override;
	virtual bool RenderPlayerSpritesInScene() const { return true; }
	virtual bool GetTeleportLocation(DVector3 &out) const override;
	virtual void Vibrate(float duration, int channel, float intensity) const override;
	virtual bool GetBenchmarkInfo(VRBenchmarkInfo& out) const override;
	// [BB] physicalHand is 0 = left, 1 = right, matching the input arrays and
	// Vibrate's channel -- not the abstract main/off indexing.
	bool GetThumbstickState(int physicalHand, float& x, float& y) const;

    // Vulkan specific multiview setup
    void InitializeMultiview() const;

protected:

	void updateHmdPose(FRenderViewpoint& vp) const;
	void UpdateControllerState() const;
	void ProcessHaptics() const;
	void StopHaptics() const;
	bool InitializeOpenXR() const;
	bool CreateSwapchain() const;
	bool CreatePresentTextures(VulkanRenderDevice* fb) const;
	bool CreateVirtualScreenSwapchain(uint32_t width, uint32_t height) const;
	bool CreateVirtualScreenBackdropSwapchain(uint32_t width, uint32_t height) const;
	bool CreateMenuPointerBeamSwapchain() const;
	void DestroyVirtualScreenSwapchain() const;
	void DestroyVirtualScreenBackdropSwapchain() const;
	void DestroyMenuPointerBeamSwapchain() const;
	void DestroyOpenXR() const;

	void updateVirtualScreenLayer() const;
	FrameRenderMode DetermineFrameRenderMode() const;
	void ApplyFrameRenderMode(FrameRenderMode mode) const;
	bool ShouldRenderVirtualScreen() const;
	void PurgeDeferredOpenXRResources() const;

	std::unique_ptr<VKOpenXRDeviceEyePose> mEyes[2];

	mutable bool isSetup;
	mutable bool isOpenXRReady = false;
	mutable uint64_t xrInitProbeFrameTime = UINT64_MAX;
	mutable bool xrInitProbeResult = false;
	mutable bool isSessionRunning = false;
	mutable bool isSessionReadyToBegin = false;
	mutable FrameRenderMode mFrameRenderMode = FrameRenderMode::VirtualScreen;
	// vr_menu_keep_world: head yaw that turned the rendered view (doomYaw) while
	// a MenuOverWorld frame was up, but was NOT sent to the player actor -- a
	// menu-paused playsim drops ticcmd turns. Paid into G_AddViewAngle on the
	// first gameplay frame after the menu closes; zeroed when doomYaw is snapped
	// back to the actor. Always 0 when the cvar is off.
	mutable float xrMenuHeldYawDegrees = 0.0f;
	mutable bool mInVRSceneRender = false;
	mutable bool mInVirtualScreenRender = false;
	mutable uint32_t sceneWidth = 0;
	mutable uint32_t sceneHeight = 0;
	mutable int cachedScreenBlocks = 0;
	mutable XrInstance xrInstance = XR_NULL_HANDLE;
	mutable XrSystemId xrSystemId = XR_NULL_SYSTEM_ID;
	mutable XrSession xrSession = XR_NULL_HANDLE;
	mutable XrSpace xrSpace = XR_NULL_HANDLE;
	mutable bool xrUsingStageSpace = false;
	mutable bool xrHasLocalHeightAnchor = false;
	mutable float xrLocalHeightAnchor = 0.0f;
	mutable XrSwapchain xrSwapchain = XR_NULL_HANDLE;
	mutable std::shared_ptr<VulkanInstance> xrVkInstance;
	mutable std::shared_ptr<VulkanDevice> xrVkDevice;
	mutable std::unique_ptr<VulkanCommandPool> xrVkCommandPool;
	mutable std::unique_ptr<VulkanCommandBuffer> xrVkCommandBuffer;
	mutable std::unique_ptr<VulkanFence> xrVkSubmitFence;

	mutable XrActionSet xrActionSet = XR_NULL_HANDLE;
	mutable XrAction xrPoseAction = XR_NULL_HANDLE;
	mutable XrSpace xrHandSpaces[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	// MEASUREMENT ONLY. xrPoseAction above is bound to /input/aim/pose on every
	// interaction profile -- the pointer ray, not the fist. These bind the grip
	// pose alongside it so the difference between the two can be logged. Nothing
	// reads them for placement; they exist to put a number on how far the frame
	// the engine calls "the hand" is from the player's actual hand.
	mutable XrAction xrGripPoseAction = XR_NULL_HANDLE;
	mutable XrSpace xrGripSpaces[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	mutable XrAction xrSelectAction = XR_NULL_HANDLE;
	mutable XrAction xrMenuAction = XR_NULL_HANDLE;
	mutable XrAction xrGripAction = XR_NULL_HANDLE;
	mutable XrAction xrThumbClickAction = XR_NULL_HANDLE;
	mutable XrAction xrThumbstickAction = XR_NULL_HANDLE;
	mutable XrAction xrTrackpadAction = XR_NULL_HANDLE;
	mutable XrAction xrAAction = XR_NULL_HANDLE;
	mutable XrAction xrBAction = XR_NULL_HANDLE;
	mutable XrAction xrXAction = XR_NULL_HANDLE;
	mutable XrAction xrYAction = XR_NULL_HANDLE;
	mutable XrAction xrPrimaryAction = XR_NULL_HANDLE;
	mutable XrAction xrSecondaryAction = XR_NULL_HANDLE;
	mutable XrAction xrThumbTouchAction = XR_NULL_HANDLE;
	mutable XrAction xrTriggerTouchAction = XR_NULL_HANDLE;

	// ANALOG TRIGGER AND SQUEEZE, alongside the boolean ones above rather
	// than replacing them.
	//
	// The .../trigger/value and .../squeeze/value input paths were already
	// bound -- to xrSelectAction and xrGripAction, which are BOOLEAN actions,
	// so the runtime thresholded a continuous 0..1 down to pressed/not and
	// the rest was thrown away before anything could see it. Every mechanic
	// that wants how HARD or how FAR, rather than merely whether -- trigger
	// travel with a distinct release point, a grip that tightens, a partial
	// pull that never reaches the shot -- needs the value that was being
	// discarded.
	//
	// Both actions stay: nothing that reads the booleans today has to change,
	// and OpenXR is content to have two actions bound to one input path.
	mutable XrAction xrTriggerValueAction = XR_NULL_HANDLE;
	mutable XrAction xrGripValueAction = XR_NULL_HANDLE;
	mutable XrPath xrLeftHandPath = XR_NULL_PATH;
	mutable XrPath xrRightHandPath = XR_NULL_PATH;
	mutable XrPosef xrHandPoses[2] = { { {0,0,0,1}, {0,0,0} }, { {0,0,0,1}, {0,0,0} } };
	mutable bool xrHandPoseValid[2] = { false, false };
	mutable bool xrLastSelectState[2] = { false, false };
	mutable bool xrLastMenuState[2] = { false, false };
	mutable bool xrLastGripState[2] = { false, false };
	mutable bool xrLastHolsterState[2] = { false, false };
	// What this hand's grip MEANS this frame -- see EGripContext and
	// ResolveGripContexts. Every consumer of grip reads this instead of
	// re-deriving intent from the raw button, which is how two of them used
	// to fire at once.
	mutable int xrGripContext[2] = { 0, 0 };

	// What each hand is closed on this frame, EGripSubject. Resolved beside the
	// context above and published to the pawn.
	mutable int xrGripSubject[2] = { 0, 0 };

	// Capacitive finger contact per hand, as FINGERTOUCH_* bits.
	mutable int xrFingerTouch[2] = { 0, 0 };
	// Tap-vs-combo resolution for grip. A grip press that STARTS inside a
	// holster arms a pending store; if any other button joins it before
	// release then it was a modifier combo and the store is cancelled. So the
	// same button serves both, decided on release rather than guessed at press.
	mutable bool xrHolsterArmed[2] = { false, false };
	mutable bool xrHolsterCombo[2] = { false, false };
	mutable bool xrHolsterFire[2]  = { false, false };
	// A grip press that came and went with no other button joining it. Lets
	// the DOMINANT hand's grip emit its key on release without breaking the
	// shift layer -- combos still suppress it, so grip+X is a combo and grip
	// alone is a bindable button.
	mutable bool xrGripTapFire[2]  = { false, false };
	mutable bool xrLastGripTapState[2] = { false, false };
	mutable bool xrLastThumbClickState[2] = { false, false };
	mutable bool xrLastTrackpadClickState[2] = { false, false };
	mutable bool xrLastAState[2] = { false, false };
	mutable bool xrLastBState[2] = { false, false };
	mutable bool xrLastXState[2] = { false, false };
	mutable bool xrLastYState[2] = { false, false };
	mutable bool xrLastPrimaryState[2] = { false, false };
	mutable bool xrLastSecondaryState[2] = { false, false };
	mutable XrVector2f xrLastThumbstickState[2] = { {0.0f, 0.0f}, {0.0f, 0.0f} };
	mutable XrVector2f xrLastTrackpadState[2] = { {0.0f, 0.0f}, {0.0f, 0.0f} };
	mutable bool xrLastMenuReturnState = false;
	mutable bool xrLastMenuBackState = false;
	mutable bool xrLastMenuBackspaceState = false;
	mutable XrAction xrHapticAction = XR_NULL_HANDLE;
	mutable double xrHapticDuration[2] = { 0.0, 0.0 };
	mutable float xrHapticIntensity[2] = { 0.0f, 0.0f };
	mutable bool xrHapticActive[2] = { false, false };
	mutable DVector3 m_TeleportLocation = DVector3(0.0, 0.0, 0.0);
	mutable int m_TeleportTarget = 0;

	mutable std::vector<XrViewConfigurationView> xrViewConfigs;
	mutable std::vector<XrView> xrViews;
	mutable std::vector<XrCompositionLayerProjectionView> xrProjectionViews;
	mutable std::vector<XrSwapchainImageVulkanKHR> xrSwapchainImages;
	mutable std::vector<VkTextureImage> xrSwapchainTextures;
	mutable std::vector<VkTextureImage> xrPresentTextures;
	mutable std::vector<VkTextureImage> xrMirrorPresentTextures;
	mutable std::vector<std::vector<VkTextureImage>> xrDeferredPresentTextures;
	mutable std::vector<std::vector<VkTextureImage>> xrDeferredMirrorPresentTextures;
	// [SPECTATOR] vr_spectator: the stabilized desktop view. At most one texture, the
	// size of the window; FinalizeEyeImage draws it, RenderDesktopMirror shows it.
	mutable std::vector<VkTextureImage> xrSpectatorTextures;
	mutable bool xrSpectatorFrameReady = false;
	mutable FVRViewStabilizer xrSpectatorStabilizer;
	mutable std::vector<XrSwapchainImageVulkanKHR> xrVirtualScreenSwapchainImages;
	mutable std::vector<XrSwapchainImageVulkanKHR> xrVirtualScreenBackdropSwapchainImages;
	mutable std::vector<XrSwapchainImageVulkanKHR> xrMenuPointerBeamSwapchainImages;
	mutable std::vector<VkTextureImage> xrVirtualScreenTextures;
	mutable std::vector<VkTextureImage> xrVirtualScreenBackdropTextures;
	mutable std::vector<VkTextureImage> xrMenuPointerBeamTextures;
	mutable std::vector<std::vector<VkTextureImage>> xrDeferredVirtualScreenTextures;
	mutable std::vector<std::vector<VkTextureImage>> xrDeferredVirtualScreenBackdropTextures;
	mutable std::vector<std::vector<VkTextureImage>> xrDeferredMenuPointerBeamTextures;
	mutable uint32_t xrViewCount = 0;
	mutable int xrCurrentImageIndex = -1;
	mutable int xrVirtualScreenImageIndex = -1;
	mutable int xrVirtualScreenBackdropImageIndex = -1;
	mutable int xrMenuPointerBeamImageIndex = -1;
	mutable uint32_t xrVirtualScreenWidth = 0;
	mutable uint32_t xrVirtualScreenHeight = 0;
	mutable uint32_t xrPresentWidth = 0;
	mutable uint32_t xrPresentHeight = 0;
	mutable int64_t xrSwapchainFormat = VK_FORMAT_UNDEFINED;
	mutable int64_t xrVirtualScreenSwapchainFormat = VK_FORMAT_UNDEFINED;
	mutable XrSwapchain xrVirtualScreenSwapchain = XR_NULL_HANDLE;
	mutable XrSwapchain xrVirtualScreenBackdropSwapchain = XR_NULL_HANDLE;
	mutable XrSwapchain xrMenuPointerBeamSwapchain = XR_NULL_HANDLE;
	mutable XrCompositionLayerQuad xrVirtualScreenLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	mutable XrCompositionLayerQuad xrVirtualScreenBackdropLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	mutable XrCompositionLayerQuad xrMenuPointerBeamLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	mutable XrCompositionLayerEquirectKHR xrVirtualScreenBackdropEquirectLayer{ XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR };
	mutable XrPosef xrVirtualScreenPose{};
	mutable XrPosef xrVirtualScreenBackdropPose{};
	mutable XrPosef xrMenuPointerBeamPose{};
	mutable bool xrStationaryAnchorValid = false;
	mutable int xrStationaryAnchorMode = -1;
	mutable bool xrVirtualScreenWasVisibleLastFrame = false;
	mutable XrPosef xrStationaryAnchorPose{};
	mutable XrPosef xrStationaryFollowCurrentPose{};
	mutable XrPosef xrStationaryFollowTargetPose{};
	mutable double xrStationaryFollowNextTargetTimeMs = 0.0;
	mutable double xrStationaryFollowLastStepTimeMs = 0.0;
	mutable bool xrHasPrevHeadSampleForRecenter = false;
	mutable XrVector3f xrPrevHeadCenterForRecenter{ 0.0f, 0.0f, 0.0f };
	mutable float xrPrevHeadYawDegForRecenter = 0.0f;
	mutable float xrMenuPointerBeamLength = 0.0f;
	mutable XrFrameState xrFrameState = { XR_TYPE_FRAME_STATE };
	mutable bool xrFrameInProgress = false;
	mutable bool xrVirtualScreenVisible = false;
	mutable bool xrVirtualScreenBackdropVisible = false;
	mutable bool xrMenuPointerActive = false;
	mutable bool xrMenuPointerHasHit = false;
	mutable bool xrMenuPointerBeamVisible = false;
	mutable float xrMenuPointerX = 0.0f;
	mutable float xrMenuPointerY = 0.0f;
	mutable bool xrMenuPointerHadPos = false;
	mutable int xrMenuPointerLastX = 0;
	mutable int xrMenuPointerLastY = 0;
	mutable bool xrMenuPointerLastLeftDown = false;
	mutable bool xrMenuPointerLastRightDown = false;
#ifdef XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME
	mutable bool xrHasDisplayRefreshRate = false;
	mutable bool xrLoggedDisplayRefreshRates = false;
	mutable float xrRequestedDisplayRefreshRate = 0.0f;
	mutable float xrCurrentDisplayRefreshRate = 0.0f;
#endif
	mutable bool mSetUpInProgress = false;
	mutable uint64_t xrFrameCounter = 0;
	mutable bool xrHasFBColorSpace = false;
	mutable bool xrHasEquirectBackdrop = false;
	mutable bool xrMultiviewProbed = false;
	mutable bool xrMultiviewSupported = false;
	mutable bool xrMultiviewUsesCoreVulkan = false;
	mutable uint32_t xrMultiviewMaxViewCount = 0;
	mutable uint32_t xrMultiviewMaxInstanceIndex = 0;
    
private:
	typedef VRMode super;
};

} /* namespace s3d */
