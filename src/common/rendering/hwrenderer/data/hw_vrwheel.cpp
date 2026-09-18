#include "hw_vrwheel.h"

#include <cmath>

#include "playsim/actor.h"
#include "c_console.h"
#include "c_cvars.h"
#include "d_player.h"
#include "g_game.h"
#include "gamedata/a_weapons.h"
#include "gamedata/info.h"
#include "menu.h"
#include "common/textures/gametexture.h"
#include "common/textures/textures.h"
#include "common/textures/texturemanager.h"
#include "common/statusbar/base_sbar.h"
#include "common/rendering/hwrenderer/data/flatvertices.h"
#include "common/rendering/hwrenderer/data/hw_renderstate.h"
#include "common/rendering/hwrenderer/data/hw_viewpointbuffer.h"
#include "common/utility/i_time.h"
#include "events.h"
#include "g_levellocals.h"
#include "g_statusbar/sbar.h"
#include "sound/s_doomsound.h"
#include "vm.h"
#include "d_net.h"
#include "playsim/p_pspr.h"
#include <QzDoom/VrCommon.h>
#include "hw_vrmodes.h"
#include "r_data/models.h"
#include "rendering/hwrenderer/scene/hw_drawinfo.h"
#include "rendering/hwrenderer/hw_models.h"
#include "r_data/sprites.h"
#include "r_utility.h"

EXTERN_CVAR(Int, vr_control_scheme)
EXTERN_CVAR(Float, gl_mask_sprite_threshold)

CVAR(Bool, vr_wheel_weapon_all, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_switch_hands, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_hide_hand_weapon, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_hide_other_class_weapons, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_sound, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_icon_load_model, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_auto_split, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_wheel_icon_bg_color, (int)MAKEARGB(128, 63, 63, 63), CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_wheel_icon_select_color, (int)MAKEARGB(160, 255, 208, 0), CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_wheel_icon_disable_color, (int)MAKEARGB(160, 96, 16, 16), CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_distance, 0.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [BB] The wheel no longer slows time itself. It announces that a wheel opened
// or closed and leaves the decision to whoever is listening, because "what
// should time do while I pick a weapon" is a gameplay question and the engine
// is the wrong place to answer it -- a mod that already owns time (Bullet-Time-X
// and its adrenaline meter, a freeze mod, nothing at all) would have to fight
// the engine for control otherwise.
//
// Both events are netevents, so they arrive in ZScript's NetworkProcess, which
// is where mods of this kind already listen. Arguments are:
//   arg1  wheel type   1 = main weapon, 2 = offhand weapon, 3 = inventory
//   arg2  anchor hand  0 = main hand, 1 = off hand
//   arg3  how many wheels are open after this change
// arg3 is what a listener uses to avoid double-triggering: act when it becomes
// 1 on open and when it reaches 0 on close, and two rings behave like one.
CVAR(String, vr_wheel_event_open, "vrwheel_open", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, vr_wheel_event_close, "vrwheel_close", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [BB] Convenience hook for a time mod that wants poking directly instead of
// through a shim. Empty means "send nothing". Set vr_wheel_time_event to
// "bt_activate" and stock Bullet-Time-X 4.3.3 responds -- though note that its
// KEYCONF exposes no matching stop event and it spends adrenaline, so the off
// event is provided for mods that do have one rather than because BT-X does.
// Fired only on the first open and the last close.
CVAR(String, vr_wheel_time_event, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(String, vr_wheel_time_event_off, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_xoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_yoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_radius, 8.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_deadzone, 0.30f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_icon_scale, 1.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_icon_model_scale, 0.8f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_icon_model_yaw, -135.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_icon_model_xoffset, -40.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_icon_model_zoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_select_angle, 30.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [BB] DESKTOP WHEEL. Added 2026-08-08 at the owner's request -- the wheel
// was hard-gated to IsVR() in four places, so on a flat screen the binds ran
// a function that returned on its first line and the buttons did nothing.
//
// This is a TESTING AFFORDANCE, not a second UI. It substitutes the two
// things the wheel cannot get without a headset:
//   * the anchor hand pose  -> a point in front of the camera, because the
//     non-VR branch of GetHandPose reads AttackPos, which is the ACTOR'S
//     FEET unless OverrideAttackPosDir is set. Anchoring there put the
//     wheel around the player's ankles.
//   * the thumbstick        -> how far the view has turned since the wheel
//     opened. OpenYaw/OpenPitch were already being stored, so "turn to
//     select" costs no new state and no new input path.
// Haptics and the weapon transform stay VR-only and simply do not fire.
CVAR(Bool, vr_wheel_desktop, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Degrees of view turn that equals full stick deflection.
CVAR(Float, vr_wheel_desktop_range, 22.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// 0 touch, 1 aim (relative to the pose the wheel opened at), 2 thumbstick,
// 3 pointer -- a ray from the hand, struck against the wheel's own plane.
//
// Pointer is the default. Touch held the spot because it was the only mode
// that existed, and it is the weakest of the four: it asks the hand to arrive
// at a 4cm target it cannot feel, it is measured against OpenXR's AIM pose
// rather than the grip pose so the tested point sits out past the knuckles,
// and it breaks outright the moment the wheel moves, because a reach is
// positional and the icons are what moved. Pointing is angular, so none of
// that applies -- it lands where it is aimed however far away the ring is and
// however fast it is drifting.
CVAR(Int, vr_wheel_selection_type, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// How far the stick must leave centre before it points at anything. Below this
// the ring keeps whatever was already chosen rather than snapping to whichever
// icon a resting thumb happens to lean toward.
CVAR(Float, vr_wheel_stick_deadzone, 0.45f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [BB] Wrist leash. 0 parks the ring where it bloomed, which is the original
// behaviour. Above 0 the ring stays perfectly still until the hand pulls past
// its edge, then gets dragged along.
//
// A leash rather than a chase, and the difference is not a detail. A ring that
// follows at a speed limit, or on a spring, cannot hold a selection: standing
// still to aim is exactly the condition under which the ring catches up, so the
// hover you were about to commit slides out from under your hand. With a dead
// region there is no motion at all inside it, so hover is as stable as a parked
// wheel and the rule stays legible -- the ring stays put until you drag it.
//
// Scales the radius that puts every icon in reach with the ring dead still, so
// 1.0 means "exactly far enough" and it survives changes to wheel radius and
// icon scale.
CVAR(Float, vr_wheel_leash, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [BB] Draw a mark where the pointer meets the wheel. The beam itself stays
// invisible -- a line drawn from the hand to the ring is mostly in the way of
// the thing it is pointing at -- but landing a ray on a plane with no feedback
// at all is aiming blind, so the point of contact is shown.
CVAR(Bool, vr_wheel_pointer_dot, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [BB] Info panel for the entry under the hand. 0 off, 1 in the ring's hub,
// 2 outside the ring on the side away from the body.
//
// Both placements exist because they fail in opposite ways. The hub is where
// the eye already is and needs no extra room, but it sits under the reaching
// hand and a crowded inner ring can reach it. Beside is always legible and
// never occluded, but it costs view space and has to pick a side.
CVAR(Int, vr_wheel_info, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_info_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_wheel_info_bg_color, (int)MAKEARGB(190, 12, 12, 14), CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// [BB] Declared rather than included: vk_openxrdevice.h pulls in the OpenXR and
// Vulkan SDK headers, and this file needs one function out of it.
namespace s3d { bool OpenXR_GetThumbstick(int abstractHand, float& x, float& y); }

// [BB] The wheel may run when a headset is present, or when the desktop
// affordance above is switched on. Every gate that used to test IsVR()
// directly now asks this instead, so the two can never drift apart.
static bool VRWheel_Available(const VRMode* vrmode)
{
	return vrmode != nullptr && (vrmode->IsVR() || vr_wheel_desktop);
}
using s3d::OpenXR_GetThumbstick;

// NOTE: this file deliberately does not include models.h, so this declaration
// is a duplicate of the one in models.cpp and MUST be kept in step with it --
// a signature change here is a link error, not a compile error. The trailing
// DPSprite* is the RS fork's direct model-frame override; the wheel renders
// world items, not psprites, so it always takes the default.
class DPSprite;
void RenderFrameModels(FModelRenderer* renderer, FLevelLocals* Level, const FSpriteModelFrame* smf, const FState* curState, const int curTics, double ticFrac, FTranslationID translation, AActor* actor, const DPSprite* psp = nullptr);

namespace
{
	enum class EVRWheelType
	{
		None,
		MainWeapon,
		OffhandWeapon,
		Inventory
	};

	struct VRWheelEntry
	{
		// [BB] Item is for building this entry and nothing else -- never
		// dereference it after RefreshEntries has returned. Entries survive across
		// frames, nothing here is visible to the garbage collector, and the refresh
		// stops running long before the entry is used: VRWheel_Draw bails above it
		// while the player is dead, while a menu or the console is up, and while
		// the automap screen layer is showing, but the bind release that commits
		// the selection does not care about any of that. So a hovered actor can be
		// destroyed and collected in the gap, and committing would read freed
		// memory.
		//
		// ItemClass is what commit uses instead. A PClassActor lives for the
		// process, so re-finding the real actor through it at the moment of use is
		// always safe, and if the player no longer has the thing the lookup simply
		// fails.
		AActor* Item = nullptr;
		PClassActor* ItemClass = nullptr;
		FGameTexture* Icon = nullptr;
		FSpriteModelFrame* ModelFrame = nullptr;
		FState* ModelState = nullptr;
		bool Selectable = false;
		bool Owned = false;
	};

	struct VRWheelState
	{
		EVRWheelType Type = EVRWheelType::None;
		int AnchorHand = VR_MAINHAND;
		AActor* Owner = nullptr;
		FLevelLocals* Level = nullptr;
		VSMatrix Transform;
		DVector3 HeadLocalOffset = {};
		DAngle OpenYaw = nullAngle;
		DAngle OpenPitch = nullAngle;
		int HoveredIndex = -1;
		bool HoverValid = false;
		TArray<VRWheelEntry> Entries;
	};

	struct VRWheelRingLayout
	{
		int StartIndex = 0;
		int Count = 0;
		float Radius = 0.0f;
		float IconSize = 0.0f;
		double AngleOffset = 0.0;
	};

	struct VRWheelLayoutInfo
	{
		int RingCount = 0;
		VRWheelRingLayout Rings[2];
	};

	// [BB] One wheel per hand, indexed by VR_MAINHAND / VR_OFFHAND, so both can
	// be open at once and each is worked by the hand it belongs to. A hand
	// holds at most one wheel; asking for a second on the same hand replaces
	// what is there.
	VRWheelState GVRWheels[2];

	static VRWheelState& WheelForHand(int hand)
	{
		return GVRWheels[hand == VR_OFFHAND ? VR_OFFHAND : VR_MAINHAND];
	}

	static void UpdateHover(player_t* player, VRWheelState& wheel);
	static VRWheelLayoutInfo BuildWheelLayoutInfo(int count);
	static void AnnounceWheelClosed(EVRWheelType type, int anchorHand);
	static void PlayWheelHaptics(const VRMode* vrmode, int hand, float intensity);
	static void MoveWeaponToHand(player_t* player, AActor* weapon, bool targetOffhand)
	{
		if (player == nullptr || player->mo == nullptr || weapon == nullptr)
		{
			return;
		}

		IFVIRTUALPTRNAME(player->mo, NAME_PlayerPawn, MoveWeaponToHand)
		{
			// Fourth argument is exactInstance (see PlayerPawn.MoveWeaponToHand).
			// FALSE here deliberately: the native wheel lists one card per class,
			// so class equivalence is the match it wants.
			VMValue param[] = { player->mo, weapon, targetOffhand ? 1 : 0, 0 };
			VMCall(func, param, 4, nullptr, 0);
		}
	}

	static DVector3 ToGamePoint(const double* xyz)
	{
		return { xyz[0], xyz[2], xyz[1] };
	}

	static DVector3 MatrixPointToGame(VSMatrix& mat, double x, double y, double z)
	{
		FLOATTYPE in[4] =
		{
			(FLOATTYPE)x,
			(FLOATTYPE)y,
			(FLOATTYPE)z,
			(FLOATTYPE)1.0
		};
		FLOATTYPE out[4] = {};
		mat.multMatrixPoint(in, out);
		const double point[3] = { (double)out[0], (double)out[1], (double)out[2] };
		return ToGamePoint(point);
	}

	static bool GetLocalControllerPose(int abstractHand, DVector3& pos)
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		if (vrmode == nullptr || !vrmode->IsVR())
		{
			return false;
		}

		VSMatrix mat;
		if (!vrmode->GetWeaponTransform(&mat, abstractHand))
		{
			return false;
		}

		pos = MatrixPointToGame(mat, 0.0, 0.0, 0.0);
		return true;
	}

	static DVector3 AngleToVector(DAngle yaw, DAngle pitch)
	{
		const double pc = pitch.Cos();
		DVector3 vec = { pc * yaw.Cos(), pc * yaw.Sin(), -pitch.Sin() };
		vec.MakeUnit();
		return vec;
	}

	static void PlayWheelSound(const char* sound)
	{
		if (!vr_wheel_sound || sound == nullptr || *sound == '\0')
		{
			return;
		}

		S_Sound(CHAN_VOICE, CHANF_UI, sound, snd_menuvolume, ATTN_NONE);
	}

	static void PlayWheelHaptics(const VRMode* vrmode, int hand, float intensity)
	{
		if (vrmode == nullptr)
		{
			return;
		}

		if (hand != VR_MAINHAND && hand != VR_OFFHAND)
		{
			hand = VR_MAINHAND;
		}

		const bool rightHanded = vr_control_scheme < 10;
		const int hapticChannel = rightHanded
			? (hand == VR_MAINHAND ? 1 : 0)
			: hand;

		vrmode->Vibrate(35.0f, hapticChannel, intensity);
	}

	static int GetPreferredAnchorHand(EVRWheelType type)
	{
		if (type == EVRWheelType::MainWeapon)
		{
			return VR_MAINHAND;
		}
		if (type == EVRWheelType::OffhandWeapon)
		{
			return VR_OFFHAND;
		}
		return vr_wheel_switch_hands ? VR_MAINHAND : VR_OFFHAND;
	}

	static bool IsWeaponWheelType(EVRWheelType type)
	{
		return type == EVRWheelType::MainWeapon || type == EVRWheelType::OffhandWeapon;
	}

	static bool GetHandPose(player_t* player, int abstractHand, DVector3& pos, DVector3& dir)
	{
		if (GetLocalControllerPose(abstractHand, pos))
		{
			DVector3 head = r_viewpoint.CenterEyePos.LengthSquared() > 1e-8 ? r_viewpoint.CenterEyePos : r_viewpoint.Pos;
			dir = head - pos;
			if (dir.LengthSquared() <= 1e-8)
			{
				dir = AngleToVector(r_viewpoint.Angles.Yaw, r_viewpoint.Angles.Pitch);
			}
			if (dir.LengthSquared() <= 1e-8)
			{
				dir = { 1.0, 0.0, 0.0 };
			}
			dir.MakeUnit();
			return true;
		}

		if (player == nullptr || player->mo == nullptr)
		{
			return false;
		}

		// [BB] NO TRACKED HANDS: SYNTHESISE ONE IN FRONT OF THE CAMERA.
		// The branch below reads AttackPos/OffhandPos, which the engine only
		// writes when OverrideAttackPosDir is set. Without it they hold the
		// actor's origin -- its FEET -- so the wheel anchored at ankle height
		// and looked broken rather than absent. Same trap the in-world panels
		// hit. Held to the same side the real hand would be on, so the layout
		// code sees what it expects.
		if (!player->mo->OverrideAttackPosDir)
		{
			const DVector3 head = r_viewpoint.CenterEyePos.LengthSquared() > 1e-8
				? r_viewpoint.CenterEyePos : r_viewpoint.Pos;
			const DVector3 fwd = AngleToVector(r_viewpoint.Angles.Yaw, nullAngle);
			const DVector3 rt  = AngleToVector(r_viewpoint.Angles.Yaw - DAngle::fromDeg(90.0), nullAngle);
			const double side  = (abstractHand == VR_MAINHAND) ? 1.0 : -1.0;

			pos = head + fwd * 22.0 + rt * (side * 11.0) - DVector3(0.0, 0.0, 8.0);
			dir = head - pos;
			if (dir.LengthSquared() <= 1e-8)
			{
				dir = AngleToVector(r_viewpoint.Angles.Yaw, r_viewpoint.Angles.Pitch);
			}
			dir.MakeUnit();
			return true;
		}

		if (abstractHand == VR_OFFHAND)
		{
			pos = player->mo->OffhandPos;
			dir = AngleToVector(player->mo->OffhandAngle + DAngle::fromDeg(90.0), player->mo->OffhandPitch);
		}
		else
		{
			pos = player->mo->AttackPos;
			dir = AngleToVector(player->mo->AttackAngle + DAngle::fromDeg(90.0), player->mo->AttackPitch);
		}
		return true;
	}

	static void GetHandAimAngles(player_t* player, int abstractHand, DAngle& yaw, DAngle& pitch)
	{
		if (abstractHand == VR_OFFHAND)
		{
			yaw = player->mo->OffhandAngle + DAngle::fromDeg(90.0);
			pitch = player->mo->OffhandPitch;
		}
		else
		{
			yaw = player->mo->AttackAngle + DAngle::fromDeg(90.0);
			pitch = player->mo->AttackPitch;
		}
	}

	static DVector3 GetHeadAnchorOrigin()
	{
		if (r_viewpoint.CenterEyePos.LengthSquared() > 1e-8)
		{
			return r_viewpoint.CenterEyePos;
		}
		return r_viewpoint.Pos;
	}

	static void GetHeadAnchorBasis(DVector3& forward, DVector3& right, DVector3& up)
	{
		forward = AngleToVector(r_viewpoint.Angles.Yaw, r_viewpoint.Angles.Pitch);
		if (forward.LengthSquared() <= 1e-8)
		{
			forward = AngleToVector(r_viewpoint.Angles.Yaw, nullAngle);
		}
		if (forward.LengthSquared() <= 1e-8)
		{
			forward = { 1.0, 0.0, 0.0 };
		}
		forward.MakeUnit();

		const DVector3 worldUp = { 0.0, 0.0, 1.0 };
		right = worldUp ^ forward;
		if (right.LengthSquared() <= 1e-8)
		{
			right = AngleToVector(r_viewpoint.Angles.Yaw + DAngle::fromDeg(90.0), nullAngle);
		}
		if (right.LengthSquared() <= 1e-8)
		{
			right = { 0.0, 1.0, 0.0 };
		}
		right.MakeUnit();
		up = forward ^ right;
		up.MakeUnit();
	}

	static bool UseCinemaWheelOverride()
	{
		return VR_UseCinematicScreenLayer();
	}


	static bool GetControllerAnchoredCenter(player_t* player, int abstractHand, DVector3& center)
	{
		DVector3 handPos;
		DVector3 handDir;
		if (!GetHandPose(player, abstractHand, handPos, handDir))
		{
			return false;
		}

		const double doomUnitsPerMeter = 60.0;
		const double handSign = abstractHand == VR_MAINHAND ? -1.0 : 1.0;
		const DVector3 worldUp = { 0.0, 0.0, 1.0 };
		DVector3 cameraForward = GetHeadAnchorOrigin() - handPos;
		cameraForward.Z = 0.0;
		if (cameraForward.LengthSquared() <= 1e-8)
		{
			cameraForward = AngleToVector(r_viewpoint.Angles.Yaw, nullAngle);
			cameraForward.Z = 0.0;
		}
		if (cameraForward.LengthSquared() <= 1e-8)
		{
			cameraForward = { 1.0, 0.0, 0.0 };
		}
		cameraForward.MakeUnit();

		DVector3 right = worldUp ^ cameraForward;
		if (right.LengthSquared() <= 1e-8)
		{
			right = { 1.0, 0.0, 0.0 };
		}
		else
		{
			right.MakeUnit();
		}
		DVector3 up = worldUp;
		DVector3 forward;
		forward = up ^ right;
		forward.MakeUnit();

		center = handPos
			+ forward * (max(0.0f, (float)vr_wheel_distance) * doomUnitsPerMeter)
			+ right * (vr_wheel_xoffset * handSign * doomUnitsPerMeter)
			+ up * (vr_wheel_yoffset * doomUnitsPerMeter);
		return true;
	}

	// [BB] A wheel is touched by the hand that opened it. That only works
	// because the ring parks when it blooms instead of following the wrist --
	// a ring welded to a wrist moves exactly as fast as the hand reaching for
	// it, so that hand can never arrive.
	static bool GetTouchPoint(player_t* player, const VRWheelState& wheel, DVector3& out)
	{
		DVector3 unusedDir;
		return GetHandPose(player, wheel.AnchorHand, out, unusedDir);
	}

	static void CaptureHeadLockedAnchor(VRWheelState& wheel, const DVector3& center)
	{
		DVector3 forward;
		DVector3 right;
		DVector3 up;
		GetHeadAnchorBasis(forward, right, up);

		const DVector3 offset = center - GetHeadAnchorOrigin();
		wheel.HeadLocalOffset = {
			offset.X * right.X + offset.Y * right.Y + offset.Z * right.Z,
			offset.X * forward.X + offset.Y * forward.Y + offset.Z * forward.Z,
			offset.X * up.X + offset.Y * up.Y + offset.Z * up.Z
		};
	}

	static bool GetHeadLockedCenter(const VRWheelState& wheel, DVector3& center)
	{
		DVector3 forward;
		DVector3 right;
		DVector3 up;
		GetHeadAnchorBasis(forward, right, up);

		center = GetHeadAnchorOrigin()
			+ right * wheel.HeadLocalOffset.X
			+ forward * wheel.HeadLocalOffset.Y
			+ up * wheel.HeadLocalOffset.Z;
		return true;
	}

	// [BB] Drag the ring's head-local anchor if the wrist has left the leash.
	// Runs in head-local space because that is the frame the anchor already lives
	// in, so no conversion is needed and locomotion stays solved by the existing
	// head-lock.
	static void UpdateWheelLeash(player_t* player, VRWheelState& wheel)
	{
		const float leashScale = vr_wheel_leash;
		if (leashScale <= 0.0f || wheel.Entries.Size() == 0)
		{
			return;
		}

		DVector3 handPos;
		DVector3 handDir;
		if (!GetHandPose(player, wheel.AnchorHand, handPos, handDir))
		{
			return;
		}

		// Reach far enough that the outermost icon is grabbable without the ring
		// having to move at all -- the radius plus half a backdrop plus the touch
		// slack UpdateHover allows.
		const VRWheelLayoutInfo layout = BuildWheelLayoutInfo(wheel.Entries.Size());
		const VRWheelRingLayout& outer = layout.Rings[max(0, layout.RingCount - 1)];
		const double leash = (outer.Radius + (outer.IconSize * 1.45) * 0.75 + 1.0) * leashScale;
		if (leash <= 0.0)
		{
			return;
		}

		DVector3 forward;
		DVector3 right;
		DVector3 up;
		GetHeadAnchorBasis(forward, right, up);
		const DVector3 offset = handPos - GetHeadAnchorOrigin();
		const DVector3 wristLocal = {
			offset.X * right.X + offset.Y * right.Y + offset.Z * right.Z,
			offset.X * forward.X + offset.Y * forward.Y + offset.Z * forward.Z,
			offset.X * up.X + offset.Y * up.Y + offset.Z * up.Z
		};

		DVector3 delta = wheel.HeadLocalOffset - wristLocal;
		const double distance = delta.Length();
		if (distance <= leash || distance <= 1e-6)
		{
			return;
		}

		delta /= distance;
		wheel.HeadLocalOffset = wristLocal + delta * leash;
	}

	static bool GetWheelLayout(const VRWheelState& wheel, DVector3& center, DVector3& right, DVector3& up, DVector3& forward)
	{
		if (!GetHeadLockedCenter(wheel, center))
		{
			return false;
		}

		const DVector3 worldUp = { 0.0, 0.0, 1.0 };
		DVector3 cameraForward = GetHeadAnchorOrigin() - center;
		cameraForward.Z = 0.0;
		if (cameraForward.LengthSquared() <= 1e-8)
		{
			cameraForward = AngleToVector(r_viewpoint.Angles.Yaw, nullAngle);
			cameraForward.Z = 0.0;
		}
		if (cameraForward.LengthSquared() <= 1e-8)
		{
			cameraForward = { 1.0, 0.0, 0.0 };
		}
		cameraForward.MakeUnit();

		right = worldUp ^ cameraForward;
		if (right.LengthSquared() <= 1e-8)
		{
			right = { 1.0, 0.0, 0.0 };
		}
		else
		{
			right.MakeUnit();
		}
		up = worldUp;
		forward = up ^ right;
		forward.MakeUnit();
		return true;
	}

	static FGameTexture* ResolveStateIcon(FState* state)
	{
		for (int steps = 0; state != nullptr && steps < 16; ++steps, state = state->GetNextState())
		{
			if (state->sprite <= 0 || state->sprite >= (int)sprites.Size())
			{
				continue;
			}
			if (memcmp(sprites[state->sprite].name, "TNT1", 4) == 0 || memcmp(sprites[state->sprite].name, "NULL", 4) == 0 || sprites[state->sprite].numframes <= state->GetFrame())
			{
				continue;
			}

			bool mirror = false;
			FTextureID texid = sprites[state->sprite].GetSpriteFrame(state->GetFrame(), 0, nullAngle, &mirror);
			if (texid.isValid())
			{
				return TexMan.GetGameTexture(texid, true);
			}
		}
		return nullptr;
	}

	static FState* FindFirstUsableStateFrame(FState* state)
	{
		for (int steps = 0; state != nullptr && steps < 16; ++steps, state = state->GetNextState())
		{
			if (state->sprite <= 0 || state->sprite >= (int)sprites.Size())
			{
				continue;
			}
			if (memcmp(sprites[state->sprite].name, "TNT1", 4) == 0 || memcmp(sprites[state->sprite].name, "NULL", 4) == 0 || sprites[state->sprite].numframes <= state->GetFrame())
			{
				continue;
			}
			return state;
		}
		return nullptr;
	}

	static FGameTexture* ResolveWheelIcon(AActor* item)
	{
		if (item == nullptr)
		{
			return nullptr;
		}

		const FTextureID icon = item->TextureIDVar(NAME_Icon);
		if (icon.isValid())
		{
			return TexMan.GetGameTexture(icon, true);
		}

		if (!item->GetClass()->IsDescendantOf(NAME_Weapon))
		{
			const FTextureID inventoryIcon = FSetTextureID(GetInventoryIcon(item, DI_ALTICONFIRST));
			if (inventoryIcon.isValid())
			{
				return TexMan.GetGameTexture(inventoryIcon, true);
			}
		}

		if (item->GetClass()->IsDescendantOf(NAME_Weapon))
		{
			if (item->SpawnState != nullptr)
			{
				if (auto tex = ResolveStateIcon(item->SpawnState))
				{
					return tex;
				}
			}
			if (auto readyState = item->FindState(NAME_Ready))
			{
				return ResolveStateIcon(readyState);
			}
		}
		return nullptr;
	}

	static FSpriteModelFrame* ResolveWheelModel(AActor* item, bool owned, FState*& outState)
	{
		outState = nullptr;
		if (!vr_wheel_icon_load_model || item == nullptr)
		{
			return nullptr;
		}

		// For wheel models icons, probe the first usable non-empty frame from Ready state
		outState = FindFirstUsableStateFrame(item->FindState(NAME_Ready));
		if (outState == nullptr)
		{
			return nullptr;
		}

		auto modelFrame = FindModelFrame(item, outState->sprite, outState->GetFrame(), false);
		if (modelFrame != nullptr && owned && item->Level != nullptr)
		{
			return modelFrame;
		}

		outState = nullptr;
		return nullptr;
	}

	static bool IsWheelWeaponUsable(AActor* weapon)
	{
		if (weapon == nullptr || !weapon->GetClass()->IsDescendantOf(NAME_Weapon))
		{
			return false;
		}

		// Match the game's own usable-weapon filtering as closely as we can from C++.
		if (weapon->IntVar(NAME_WeaponFlags) & WIF_POWERED_UP)
		{
			return false;
		}

		// Stowed in a VR holster -- same rule CheckAmmo already enforces for
		// weapnext/weapprev/slot-select (weapons.zs), applied here too. This
		// path calls PlayerPawn::MoveWeaponToHand directly rather than going
		// through CheckAmmo at all, so without this the wheel could select a
		// holstered weapon straight into a hand and leave it permanently
		// stuck (bHolsterHidden never gets cleared outside the holster
		// mod's own draw path).
		if (weapon->BoolVar("bHolsterHidden"))
		{
			return false;
		}

		auto sisterWeapon = weapon->PointerVar<AActor>(NAME_SisterWeapon);
		if (sisterWeapon != nullptr && weapon->GetClass()->IsDescendantOf(sisterWeapon->GetClass()))
		{
			return false;
		}

		auto ammo1 = weapon->PointerVar<AActor>(NAME_Ammo1);
		auto ammo2 = weapon->PointerVar<AActor>(NAME_Ammo2);
		const int weaponFlags = weapon->IntVar(NAME_WeaponFlags);

		if (!(weaponFlags & WIF_AMMO_OPTIONAL))
		{
			if (ammo1 != nullptr)
			{
				const int use1 = weapon->IntVar(NAME_AmmoUse1);
				if (use1 > 0 && ammo1->IntVar(NAME_Amount) < use1)
				{
					return false;
				}
			}

			if ((weaponFlags & WIF_PRIMARY_USES_BOTH) && ammo2 != nullptr)
			{
				const int use2 = weapon->IntVar("AmmoUse2");
				if (use2 > 0 && ammo2->IntVar(NAME_Amount) < use2)
				{
					return false;
				}
			}
		}

		if (weapon->IntVar(NAME_MinSelAmmo1) > 0 && (ammo1 == nullptr || ammo1->IntVar(NAME_Amount) < weapon->IntVar(NAME_MinSelAmmo1)))
		{
			return false;
		}

		if (weapon->IntVar("MinSelAmmo2") > 0 && (ammo2 == nullptr || ammo2->IntVar(NAME_Amount) < weapon->IntVar("MinSelAmmo2")))
		{
			return false;
		}

		return true;
	}

	static void AddWheelEntry(TArray<VRWheelEntry>& entries, AActor* item, bool owned, bool selectable)
	{
		auto icon = ResolveWheelIcon(item);
		FState* modelState = nullptr;
		auto modelFrame = ResolveWheelModel(item, owned, modelState);
		if (item == nullptr || (icon == nullptr && modelFrame == nullptr))
		{
			return;
		}

		VRWheelEntry entry;
		entry.Item = item;
		entry.ItemClass = item->GetClass();
		entry.Icon = icon;
		entry.ModelFrame = modelFrame;
		entry.ModelState = modelState;
		entry.Selectable = selectable;
		entry.Owned = owned;
		entries.Push(entry);
	}

	static bool IsWeaponAllowedForCurrentPlayerClass(player_t* player, AActor* weapon)
	{
		if (!vr_wheel_hide_other_class_weapons || player == nullptr || weapon == nullptr)
		{
			return true;
		}

		PClassActor* playerClass = player->cls;
		if (playerClass == nullptr && player->mo != nullptr)
		{
			playerClass = player->mo->GetClass();
		}
		if (playerClass == nullptr)
		{
			return true;
		}

		auto restricted = static_cast<TArray<PClassActor*>*>(weapon->ScriptVar(NAME_RestrictedToPlayerClass, nullptr));
		if (restricted != nullptr && restricted->Size() > 0)
		{
			bool allowed = false;
			for (auto cls : *restricted)
			{
				if (cls != nullptr && playerClass->IsDescendantOf(cls))
				{
					allowed = true;
					break;
				}
			}
			if (!allowed)
			{
				return false;
			}
		}

		auto forbidden = static_cast<TArray<PClassActor*>*>(weapon->ScriptVar(NAME_ForbiddenToPlayerClass, nullptr));
		if (forbidden != nullptr)
		{
			for (auto cls : *forbidden)
			{
				if (cls != nullptr && playerClass->IsDescendantOf(cls))
				{
					return false;
				}
			}
		}

		return true;
	}

	// [BB] Would MoveWeaponToHand actually take this weapon into this hand?
	// PlayerPawn::MoveWeaponToHand refuses outright when a weapon is flagged
	// NoHandSwitch and already belongs to the other hand, so listing it is
	// listing a dead entry: the wheel would highlight it, play the confirm
	// sound, and nothing would happen. Mods that never set NoHandSwitch are
	// untouched by this -- nothing is filtered and both wheels list everything,
	// exactly as before. Mods that split a weapon per hand (Radiant Silvergun
	// gives every gun three main-hand and three off-hand identities) get a
	// wheel per wrist showing only what that wrist can hold.
	static bool CanHandTakeWeapon(AActor* weapon, bool targetOffhand)
	{
		if (weapon == nullptr)
		{
			return false;
		}
		const int weaponFlags = weapon->IntVar(NAME_WeaponFlags);
		if (!(weaponFlags & WIF_NOHANDSWITCH))
		{
			return true;
		}
		return ((weaponFlags & WIF_OFFHANDWEAPON) != 0) == targetOffhand;
	}

	static void BuildWeaponEntries(player_t* player, TArray<VRWheelEntry>& out, bool targetOffhand)
	{
		out.Clear();
		if (player == nullptr || player->mo == nullptr)
		{
			return;
		}

		for (int slot = 0; slot < NUM_WEAPON_SLOTS; ++slot)
		{
			for (int index = 0; index < player->weapons.SlotSize(slot); ++index)
			{
				auto weapType = player->weapons.GetWeapon(slot, index);
				if (weapType == nullptr)
				{
					continue;
				}

				auto owned = player->mo->FindInventory(weapType);
				AActor* weapon = owned != nullptr ? owned : GetDefaultByType(weapType);
				if (weapon == nullptr || !IsWheelWeaponUsable(weapon))
				{
					continue;
				}
				if (!IsWeaponAllowedForCurrentPlayerClass(player, weapon))
				{
					continue;
				}
				if (!CanHandTakeWeapon(weapon, targetOffhand))
				{
					continue;
				}

				if (!vr_wheel_weapon_all && owned == nullptr)
				{
					continue;
				}
				AddWheelEntry(out, weapon, owned != nullptr, owned != nullptr);
			}
		}
	}

	static void BuildInventoryEntries(player_t* player, TArray<VRWheelEntry>& out)
	{
		out.Clear();
		if (player == nullptr || player->mo == nullptr)
		{
			return;
		}

		for (AActor* inv = player->mo->Inventory; inv != nullptr; inv = inv->Inventory)
		{
			if (inv->GetClass()->IsDescendantOf(NAME_Weapon) || !inv->BoolVar("bInvBar") || inv->IntVar(NAME_Amount) <= 0)
			{
				continue;
			}
			AddWheelEntry(out, inv, true, true);
		}
	}

	static void RefreshEntries(player_t* player, VRWheelState& wheel)
	{
		if (IsWeaponWheelType(wheel.Type))
		{
			BuildWeaponEntries(player, wheel.Entries, wheel.Type == EVRWheelType::OffhandWeapon);
		}
		else if (wheel.Type == EVRWheelType::Inventory)
		{
			BuildInventoryEntries(player, wheel.Entries);
		}
		else
		{
			wheel.Entries.Clear();
		}
	}

	static bool IsWheelOpen(const VRWheelState& wheel)
	{
		return wheel.Type != EVRWheelType::None;
	}

	static int OpenWheelCount()
	{
		int count = 0;
		for (auto& wheel : GVRWheels)
		{
			if (IsWheelOpen(wheel))
			{
				++count;
			}
		}
		return count;
	}

	static void ResetWheel(VRWheelState& wheel)
	{
		wheel = {};
	}

	static bool IsWheelOwnerValid(player_t* player, const VRWheelState& wheel)
	{
		if (!IsWheelOpen(wheel))
		{
			return true;
		}

		if (player == nullptr || player->mo == nullptr)
		{
			return false;
		}

		return wheel.Owner == player->mo && wheel.Level == player->mo->Level;
	}

	static void InvalidateWheelIfOwnerChanged(player_t* player)
	{
		for (auto& wheel : GVRWheels)
		{
			if (IsWheelOwnerValid(player, wheel))
			{
				continue;
			}
			const EVRWheelType droppedType = wheel.Type;
			const int droppedHand = wheel.AnchorHand;
			ResetWheel(wheel);
			AnnounceWheelClosed(droppedType, droppedHand);
		}
	}

	// [BB] Ask the mod what to say about this entry. Empty means "you decide",
	// which is the default the base PlayerPawn returns -- so an unmodded game
	// falls through to the engine's own tag-and-ammo readout below rather than
	// showing a blank panel.
	static FString GetScriptedEntryInfo(player_t* player, AActor* item, int hand)
	{
		FString result;
		if (player == nullptr || player->mo == nullptr || item == nullptr)
		{
			return result;
		}

		IFVIRTUALPTRNAME(player->mo, NAME_PlayerPawn, GetVRWheelInfo)
		{
			VMValue param[] = { player->mo, item, hand };
			VMReturn ret(&result);
			VMCall(func, param, 3, &ret, 1);
		}
		return result;
	}

	// The engine's own answer, used when no mod supplies one. Deliberately thin:
	// the name and what it is loaded with are the only things the engine can
	// state about a weapon without inventing them.
	static FString BuildFallbackEntryInfo(player_t* player, AActor* item, bool owned)
	{
		FString result;
		if (item == nullptr)
		{
			return result;
		}

		result = item->GetTag();
		if (!owned)
		{
			result += "\nNOT CARRIED";
			return result;
		}

		if (!item->GetClass()->IsDescendantOf(NAME_Weapon))
		{
			const int amount = item->IntVar(NAME_Amount);
			if (amount > 1)
			{
				result.AppendFormat("\nx%d", amount);
			}
			return result;
		}

		auto ammo1 = item->PointerVar<AActor>(NAME_Ammo1);
		auto ammo2 = item->PointerVar<AActor>(NAME_Ammo2);
		if (ammo1 != nullptr)
		{
			result.AppendFormat("\n%d / %d", ammo1->IntVar(NAME_Amount), ammo1->IntVar(NAME_MaxAmount));
		}
		if (ammo2 != nullptr && ammo2 != ammo1)
		{
			result.AppendFormat("\n%d / %d", ammo2->IntVar(NAME_Amount), ammo2->IntVar(NAME_MaxAmount));
		}
		return result;
	}

	static int WheelTypeToEventArg(EVRWheelType type)
	{
		switch (type)
		{
		case EVRWheelType::MainWeapon:		return 1;
		case EVRWheelType::OffhandWeapon:	return 2;
		case EVRWheelType::Inventory:		return 3;
		default:							return 0;
		}
	}

	static void SendWheelEvent(const char* name, EVRWheelType type, int anchorHand, int openCount)
	{
		if (name == nullptr || *name == '\0' || primaryLevel == nullptr || primaryLevel->localEventManager == nullptr)
		{
			return;
		}
		primaryLevel->localEventManager->SendNetworkEvent(name, WheelTypeToEventArg(type), anchorHand, openCount, false);
	}

	// [BB] Announce, do not act. Called after the wheel array has already been
	// updated, so OpenWheelCount() is the count the listener should see.
	static void AnnounceWheelOpened(EVRWheelType type, int anchorHand)
	{
		const int openCount = OpenWheelCount();
		SendWheelEvent(vr_wheel_event_open, type, anchorHand, openCount);
		if (openCount == 1)
		{
			SendWheelEvent(vr_wheel_time_event, type, anchorHand, openCount);
		}
	}

	static void AnnounceWheelClosed(EVRWheelType type, int anchorHand)
	{
		const int openCount = OpenWheelCount();
		SendWheelEvent(vr_wheel_event_close, type, anchorHand, openCount);
		if (openCount == 0)
		{
			SendWheelEvent(vr_wheel_time_event_off, type, anchorHand, openCount);
		}
	}

	static void OpenWheel(EVRWheelType type)
	{
		// A LOADED MOD MAY SUPPRESS THIS WHEEL ENTIRELY.
		//
		// Read the MOD's cvar rather than keeping an engine-side one for it
		// to write. That direction is not a style preference, it is the only
		// one that works: CVar.SetInt/SetBool refuse any cvar lacking
		// CVAR_MOD when called outside menu code (vmnatives.cpp:944), so a
		// mod writing an engine cvar from its own tick aborts the VM. An
		// engine READ of a mod cvar has no such restriction, and needs
		// nothing declared here at all.
		//
		// Absent -- no such mod loaded -- means enabled, so a plain session
		// behaves exactly as it always has. Looked up per call rather than
		// cached because a pk3 can be loaded, and the cvar created, long
		// after this translation unit's statics are initialised; OpenWheel
		// runs on a button press, so the cost is irrelevant.
		if (FBaseCVar *suppress = FindCVar("wr_suppress_native_wheel", nullptr))
		{
			UCVarValue v = suppress->GetGenericRep(CVAR_Bool);
			if (v.Bool)
			{
				return;
			}
		}

		auto vrmode = VRMode::GetVRModeCached(true);
		auto player = &players[consoleplayer];
		if (!VRWheel_Available(vrmode) || player == nullptr || player->mo == nullptr)
		{
			return;
		}

		const int anchorHand = GetPreferredAnchorHand(type);
		DVector3 initialCenter;
		if (!GetControllerAnchoredCenter(player, anchorHand, initialCenter))
		{
			return;
		}

		// [BB] Build first and refuse to open on nothing, for every wheel type
		// rather than only for inventory. An open wheel takes the trigger away --
		// g_game.cpp masks attack, use and reload while any wheel is live -- so an
		// empty one leaves the player unable to shoot or open a door while staring
		// at no icons, recovering only when they let go of a bind they cannot see
		// a reason to be holding.
		//
		// Weapon wheels used to be exempt because they were assumed never to come
		// back empty. CanHandTakeWeapon broke that assumption: a mod that gives
		// each hand its own weapon identities has a genuinely empty wheel for the
		// hand holding none of them, which is exactly the mod this filter was
		// written for.
		TArray<VRWheelEntry> entries;
		if (type == EVRWheelType::Inventory)
		{
			BuildInventoryEntries(player, entries);
		}
		else
		{
			BuildWeaponEntries(player, entries, type == EVRWheelType::OffhandWeapon);
		}
		if (entries.Size() == 0)
		{
			PlayWheelSound("menu/invalid");
			return;
		}

		// [BB] A hand can only hold one wheel. Opening a second on the same hand
		// replaces it, and the replaced one is announced as closed so a listener
		// never sees an open it will not get a close for.
		VRWheelState& wheel = WheelForHand(anchorHand);
		if (IsWheelOpen(wheel))
		{
			const EVRWheelType replacedType = wheel.Type;
			ResetWheel(wheel);
			AnnounceWheelClosed(replacedType, anchorHand);
		}

		wheel.Type = type;
		wheel.AnchorHand = anchorHand;
		wheel.Owner = player->mo;
		wheel.Level = player->mo->Level;
		CaptureHeadLockedAnchor(wheel, initialCenter);
		GetHandAimAngles(player, anchorHand, wheel.OpenYaw, wheel.OpenPitch);
		wheel.HoveredIndex = -1;
		wheel.HoverValid = false;
		wheel.Entries = entries;

		UpdateHover(player, wheel);
		PlayWheelSound("menu/activate");
		PlayWheelHaptics(vrmode, wheel.AnchorHand, 0.20f);
		AnnounceWheelOpened(type, anchorHand);
	}

	static void CommitWheelSelection(VRWheelState& wheel)
	{
		auto player = &players[consoleplayer];
		if (!wheel.HoverValid || wheel.HoveredIndex < 0 || wheel.HoveredIndex >= wheel.Entries.Size())
		{
			return;
		}

		const auto& entry = wheel.Entries[wheel.HoveredIndex];
		if (!entry.Selectable || entry.ItemClass == nullptr || player == nullptr || player->mo == nullptr)
		{
			return;
		}

		if (IsWeaponWheelType(wheel.Type))
		{
			auto weapon = player->mo->FindInventory(entry.ItemClass);
			if (weapon != nullptr)
			{
				const bool targetOffhand = wheel.Type == EVRWheelType::OffhandWeapon;
				// Always queue it, singleplayer included -- same reason as the
				// switchhand CCMD in g_game.cpp. This runs from the wheel's close
				// handler, off the game tick, and MoveWeaponToHand mutates the
				// weapon-hand slots. Applied outside P_Ticker, the psprite tick
				// can observe the slots half-updated and destroy both layers,
				// because TickPSprites deletes any psprite whose Caller does not
				// match its slot. DEM_ZSC_CMD is consumed inside the tick, so the
				// change is never seen half-applied.
				Net_WriteInt8(DEM_ZSC_CMD);
				Net_WriteString("vr_moveweaphand");
				Net_WriteInt16(5);
				Net_WriteInt32(weapon->InventoryID);
				Net_WriteInt8(targetOffhand ? 1 : 0);
			}
		}
		else if (wheel.Type == EVRWheelType::Inventory)
		{
			// [BB] Re-find rather than trusting the stored pointer, for the reason
			// spelled out on VRWheelEntry. A miss means the item is gone, which is
			// a no-op rather than a use of nothing.
			auto item = player->mo->FindInventory(entry.ItemClass);
			if (item != nullptr)
			{
				player->mo->PointerVar<AActor>(NAME_InvSel) = item;
				player->inventorytics = 0;
				SendItemUse = item;
			}
		}
	}

	// [BB] Two wheels can legitimately hold the same type -- bind the inventory
	// wheel to two keys and flip vr_wheel_switch_hands between presses and both
	// hands end up holding one. Closing "the first one found" then commits the
	// wrong wheel, so this closes the one on the hand that type prefers now, and
	// only falls back to a scan if that hand is holding something else.
	static void CloseWheel(EVRWheelType type)
	{
		VRWheelState& preferred = WheelForHand(GetPreferredAnchorHand(type));
		if (preferred.Type == type)
		{
			const int closedHand = preferred.AnchorHand;
			CommitWheelSelection(preferred);
			ResetWheel(preferred);
			PlayWheelSound("menu/clear");
			AnnounceWheelClosed(type, closedHand);
			return;
		}

		for (auto& wheel : GVRWheels)
		{
			if (wheel.Type != type)
			{
				continue;
			}
			const int closedHand = wheel.AnchorHand;
			CommitWheelSelection(wheel);
			ResetWheel(wheel);
			PlayWheelSound("menu/clear");
			AnnounceWheelClosed(type, closedHand);
			return;
		}
	}

	static void GetIconQuadSize(FGameTexture* texture, float maxSize, float& outWidth, float& outHeight)
	{
		outWidth = maxSize;
		outHeight = maxSize;
		if (texture == nullptr)
		{
			return;
		}

		const double texWidth = max<double>(1.0, texture->GetDisplayWidth());
		const double texHeight = max<double>(1.0, texture->GetDisplayHeight());
		if (texWidth >= texHeight)
		{
			outHeight = (float)(maxSize * (texHeight / texWidth));
		}
		else
		{
			outWidth = (float)(maxSize * (texWidth / texHeight));
		}
	}

	// [BB] Text in the world, built from font glyphs as quads, because the wheel
	// draws real geometry and the screen-space text routines have nowhere to put
	// their output here. One quad per character, advancing along `right`.
	//
	// Sized in map units: glyphHeight is what one line occupies, and each glyph's
	// width comes from the font so proportional fonts stay proportional.
	static void DrawWorldQuad(HWDrawInfo* di, FRenderState& state, const DVector3& center, const DVector3& right, const DVector3& up, float width, float height, FGameTexture* texture, PalEntry color, bool textured, bool rotate180 = false, FTranslationID translation = NO_TRANSLATION);

	static float MeasureWorldText(FFont* font, const char* text, float glyphHeight)
	{
		if (font == nullptr || text == nullptr)
		{
			return 0.0f;
		}
		const float unitsPerPixel = glyphHeight / (float)max(1, font->GetHeight());
		// StringWidth already skips colour escapes, so this matches what draws.
		return (float)font->StringWidth(text) * unitsPerPixel;
	}

	// Draws one line, centred on `center`. Returns nothing; callers lay out lines
	// themselves so they can mix sizes.
	static void DrawWorldTextLine(HWDrawInfo* di, FRenderState& state, const DVector3& center, const DVector3& right, const DVector3& up, float glyphHeight, FFont* font, const char* text, EColorRange baseColor)
	{
		if (di == nullptr || font == nullptr || text == nullptr || *text == 0 || glyphHeight <= 0.0f)
		{
			return;
		}

		const float unitsPerPixel = glyphHeight / (float)max(1, font->GetHeight());
		const float totalWidth = MeasureWorldText(font, text, glyphHeight);

		EColorRange activeColor = baseColor;
		FTranslationID translation = font->GetColorTranslation(activeColor);

		// Walk from the left edge so the line ends up centred.
		double penOffset = -0.5 * (double)totalWidth;
		const uint8_t* c = (const uint8_t*)text;
		while (*c != 0)
		{
			// [BB] Honour \c colour escapes rather than drawing them. A mod that
			// puts rarity on the name expects the name to be that colour, and the
			// alternative is the escape appearing as literal characters in the
			// middle of the word.
			if (*c == TEXTCOLOR_ESCAPE)
			{
				++c;
				const EColorRange parsed = V_ParseFontColor(c, CR_UNTRANSLATED, CR_YELLOW);
				if (parsed != CR_UNDEFINED)
				{
					activeColor = (parsed == CR_UNTRANSLATED) ? baseColor : parsed;
					translation = font->GetColorTranslation(activeColor);
				}
				continue;
			}

			int charWidthPixels = 0;
			FGameTexture* glyph = font->GetChar((int)*c, activeColor, &charWidthPixels);
			const float advance = (float)charWidthPixels * unitsPerPixel;
			if (glyph != nullptr && advance > 0.0f)
			{
				// The glyph texture can be wider than its advance (kerning slack),
				// so draw at the texture's own aspect rather than the advance.
				const float glyphWidth = (float)glyph->GetDisplayWidth() * unitsPerPixel;
				const float glyphDrawHeight = (float)glyph->GetDisplayHeight() * unitsPerPixel;
				const DVector3 glyphCenter = center + right * (penOffset + advance * 0.5);
				DrawWorldQuad(di, state, glyphCenter, right, up, glyphWidth, glyphDrawHeight, glyph, PalEntry(255, 255, 255, 255), true, false, translation);
			}
			penOffset += advance;
			++c;
		}
	}

	static void DrawWheelInfoPanel(HWDrawInfo* di, FRenderState& state, player_t* player, const VRWheelState& wheel,
		const VRWheelLayoutInfo& layout, const DVector3& center, const DVector3& right, const DVector3& up)
	{
		if (vr_wheel_info <= 0 || wheel.HoveredIndex < 0 || wheel.HoveredIndex >= (int)wheel.Entries.Size())
		{
			return;
		}

		const auto& entry = wheel.Entries[wheel.HoveredIndex];
		if (entry.Item == nullptr)
		{
			return;
		}

		FString info = GetScriptedEntryInfo(player, entry.Item, wheel.AnchorHand);
		if (info.IsEmpty())
		{
			info = BuildFallbackEntryInfo(player, entry.Item, entry.Owned);
		}
		if (info.IsEmpty())
		{
			return;
		}

		FFont* font = SmallFont;
		if (font == nullptr)
		{
			return;
		}

		TArray<FString> lines;
		info.Split(lines, "\n");
		if (lines.Size() == 0)
		{
			return;
		}

		const float scale = clamp<float>(vr_wheel_info_scale, 0.2f, 4.0f);
		const float bodyHeight = max(0.6f, layout.Rings[0].IconSize * 0.30f) * scale;
		const float headingHeight = bodyHeight * 1.35f;
		const float linePad = bodyHeight * 0.35f;

		float widest = 0.0f;
		float totalHeight = 0.0f;
		for (unsigned i = 0; i < lines.Size(); ++i)
		{
			const float h = (i == 0) ? headingHeight : bodyHeight;
			widest = max(widest, MeasureWorldText(font, lines[i].GetChars(), h));
			totalHeight += h + (i + 1 < lines.Size() ? linePad : 0.0f);
		}
		if (widest <= 0.0f || totalHeight <= 0.0f)
		{
			return;
		}

		// [BB] Placement. Hub sits at the ring centre. Beside pushes out past the
		// outermost ring, away from the body -- which is the opposite side to the
		// hand holding the wheel, so the panel never lands where that arm is.
		DVector3 panelCenter = center;
		if (vr_wheel_info >= 2)
		{
			const auto& outer = layout.Rings[max(0, layout.RingCount - 1)];
			const double clearance = outer.Radius + (outer.IconSize * 1.45) * 0.5 + widest * 0.5 + bodyHeight;
			const double side = (wheel.AnchorHand == VR_MAINHAND) ? -1.0 : 1.0;
			panelCenter = center + right * (clearance * side);
		}

		const float padding = bodyHeight * 0.9f;
		DrawWorldQuad(di, state, panelCenter, right, up, widest + padding * 2.0f, totalHeight + padding * 2.0f,
			nullptr, PalEntry(vr_wheel_info_bg_color), false);

		double penY = totalHeight * 0.5;
		for (unsigned i = 0; i < lines.Size(); ++i)
		{
			const float h = (i == 0) ? headingHeight : bodyHeight;
			penY -= h * 0.5;
			const EColorRange lineColor = (i == 0) ? CR_WHITE : CR_GRAY;
			DrawWorldTextLine(di, state, panelCenter + up * penY, right, up, h, font, lines[i].GetChars(), lineColor);
			penY -= h * 0.5 + linePad;
		}
	}

	static void DrawWorldDisc(HWDrawInfo* di, FRenderState& state, const DVector3& center, const DVector3& right, const DVector3& up, float radius, PalEntry color)
	{
		if (di == nullptr || radius <= 0.0f)
		{
			return;
		}

		state.SetLightIndex(-1);
		state.SetRenderStyle(STYLE_Translucent);
		state.AlphaFunc(Alpha_Greater, 0.0f);
		state.SetTextureMode(TM_NORMAL);
		state.ResetColor();
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(0);
		state.SetDynLight(0, 0, 0);
		state.SetNoSoftLightLevel();
		state.SetLightParms(1.f, 0.f);
		state.EnableFog(false);
		state.SetFog(0, 0);
		state.ResetFadeColor();
		state.EnableTextureMatrix(false);
		state.mModelMatrix.loadIdentity();
		state.EnableModelMatrix(false);
		state.SetVertexBuffer(screen->mVertexData);
		state.EnableBrightmap(false);
		state.EnableDepthTest(false);
		state.SetDepthMask(false);
		state.EnableTexture(false);
		state.SetColor(color);

		static constexpr int Segments = 24;
		screen->mVertexData->Map();
		auto vert = screen->mVertexData->AllocVertices(Segments + 2);
		auto vp = vert.first;
		vp[0].Set((float)center.X, (float)center.Z, (float)center.Y, 0.5f, 0.5f);
		for (int i = 0; i <= Segments; ++i)
		{
			const double ang = (2.0 * M_PI * double(i)) / double(Segments);
			const DVector3 point = center + right * (cos(ang) * radius) + up * (sin(ang) * radius);
			vp[i + 1].Set((float)point.X, (float)point.Z, (float)point.Y, 0.5f, 0.5f);
		}
		screen->mVertexData->Unmap();
		state.Draw(DT_TriangleFan, vert.second, Segments + 2);
	}

	static void DrawWorldQuad(HWDrawInfo* di, FRenderState& state, const DVector3& center, const DVector3& right, const DVector3& up, float width, float height, FGameTexture* texture, PalEntry color, bool textured, bool rotate180, FTranslationID translation)
	{
		if (di == nullptr || width <= 0.0f || height <= 0.0f)
		{
			return;
		}

		const DVector3 halfRight = right * (width * 0.5);
		const DVector3 halfUp = up * (height * 0.5);
		const DVector3 corners[4] =
		{
			center - halfRight - halfUp,
			center + halfRight - halfUp,
			center - halfRight + halfUp,
			center + halfRight + halfUp,
		};

		state.SetLightIndex(-1);
		state.SetRenderStyle(STYLE_Translucent);
		state.AlphaFunc(Alpha_Greater, 0.0f);
		state.SetTextureMode(TM_NORMAL);
		state.ResetColor();
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(0);
		state.SetDynLight(0, 0, 0);
		state.SetNoSoftLightLevel();
		state.SetLightParms(1.f, 0.f);
		state.EnableFog(false);
		state.SetFog(0, 0);
		state.ResetFadeColor();
		state.EnableTextureMatrix(false);
		state.mModelMatrix.loadIdentity();
		state.EnableModelMatrix(false);
		state.SetVertexBuffer(screen->mVertexData);
		state.EnableBrightmap(false);
		state.EnableDepthTest(false);
		state.SetDepthMask(false);

		if (textured && texture != nullptr)
		{
			state.SetColorAlpha(0xffffff, color.a / 255.0f, 0);
			state.EnableTexture(true);
			state.SetMaterial(texture, UF_Texture, 0, CLAMP_XY_NOMIP, translation, -1);
		}
		else
		{
			state.SetColor(color);
			state.EnableTexture(false);
		}

		screen->mVertexData->Map();
		auto vert = screen->mVertexData->AllocVertices(4);
		auto vp = vert.first;
		const float u0 = rotate180 ? 1.0f : 0.0f;
		const float u1 = rotate180 ? 0.0f : 1.0f;
		const float v0 = rotate180 ? 1.0f : 0.0f;
		const float v1 = rotate180 ? 0.0f : 1.0f;
		for (int i = 0; i < 4; ++i)
		{
			const float u = (i & 1) ? u1 : u0;
			const float v = (i >= 2) ? v1 : v0;
			vp[i].Set((float)corners[i].X, (float)corners[i].Z, (float)corners[i].Y, u, v);
		}
		screen->mVertexData->Unmap();
		state.Draw(DT_TriangleStrip, vert.second, 4);
	}

	static void DrawWheelModel(HWDrawInfo* di, FRenderState& state, const VRWheelEntry& entry, const DVector3& center, const DVector3& wheelForward, float iconSize)
	{
		if (di == nullptr || entry.Item == nullptr || entry.ModelFrame == nullptr || entry.ModelState == nullptr || entry.Item->Level == nullptr)
		{
			return;
		}

		const unsigned int smfFlags = entry.ModelFrame->getFlags(entry.Item->modelData);
		FTranslationID translation = NO_TRANSLATION;
		if (!(smfFlags & MDL_IGNORETRANSLATION))
		{
			translation = entry.Item->Translation;
		}

		const float wheelModelScale = max(0.01f, iconSize * 0.025f * max(0.01f, (float)vr_wheel_icon_model_scale));
		const float scaleFactorX = -entry.Item->Scale.X * entry.ModelFrame->xscale * wheelModelScale;
		const float scaleFactorY = entry.Item->Scale.X * entry.ModelFrame->yscale * wheelModelScale;
		const float scaleFactorZ = entry.Item->Scale.Y * entry.ModelFrame->zscale * wheelModelScale;
		const float yaw = (float)wheelForward.Angle().Degrees();

		VSMatrix objectToWorldMatrix;
		objectToWorldMatrix.loadIdentity();
		objectToWorldMatrix.translate((FLOATTYPE)center.X, (FLOATTYPE)center.Z, (FLOATTYPE)center.Y);
		objectToWorldMatrix.rotate(-(yaw - (float)vr_wheel_icon_model_yaw), 0, 1, 0);
		objectToWorldMatrix.scale(scaleFactorX, scaleFactorZ, scaleFactorY);
		objectToWorldMatrix.translate(
			vr_wheel_icon_model_xoffset + (entry.ModelFrame->xoffset / entry.ModelFrame->xscale),
			entry.ModelFrame->zoffset / entry.ModelFrame->zscale,
			vr_wheel_icon_model_zoffset + (entry.ModelFrame->yoffset / entry.ModelFrame->yscale));
		objectToWorldMatrix.rotate(-entry.ModelFrame->angleoffset, 0, 1, 0);
		objectToWorldMatrix.rotate(entry.ModelFrame->pitchoffset, 0, 0, 1);
		objectToWorldMatrix.rotate(-entry.ModelFrame->rolloffset, 1, 0, 0);

		FHWModelRenderer renderer(di, state, -1);
		const bool mirrored = (scaleFactorX * scaleFactorY * scaleFactorZ) < 0.0f;
		state.SetRenderStyle(STYLE_Normal);
		state.EnableTexture(true);
		state.SetTextureMode(TM_NORMAL);
		state.AlphaFunc(Alpha_GEqual, 0.5f);
		state.SetColorAlpha(0xffffff, 1.0f, 0);
		state.SetNoSoftLightLevel();
		state.SetLightParms(1.f, 0.f);
		state.EnableFog(false);
		state.SetFog(0, 0);
		state.SetDynLight(0, 0, 0);
		state.ResetFadeColor();
		state.EnableTextureMatrix(false);
		// The wheel is UI, so it draws on top of the world like the rest of the HUD instead of being
		// clipped by whatever you happen to be standing next to (Ermac's DoomXR c48d266). Its own
		// panels and icons already rely on draw order rather than depth, so this only removes the
		// clipping against the scene; nothing writes depth, so the wheel leaves no hole behind it.
		state.EnableDepthTest(false);
		state.SetDepthMask(false);
		state.EnableBrightmap(true);
		state.SetCulling(Cull_None);
		state.ClearDepthBias();
		state.ResetColor();
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(0);
		renderer.BeginDrawModel(DefaultRenderStyle(), (int)smfFlags, objectToWorldMatrix, mirrored);
		// ticFrac 0.0: wheel icons are posed, not interpolated between tics.
		RenderFrameModels(&renderer, entry.Item->Level, entry.ModelFrame, entry.ModelState, 0, 0.0, translation, entry.Item);
		renderer.EndDrawModel(DefaultRenderStyle(), (int)smfFlags);
		state.SetVertexBuffer(screen->mVertexData);
	}

	static float GetWheelIconSizeForCount(int count, float ringRadius)
	{
		const float baseSize = clamp<float>(2.0f * max(0.1f, (float)vr_wheel_icon_scale), 0.5f, 4.0f);
		if (count <= 1)
		{
			return baseSize;
		}

		// Keep the default feel for small wheels, then shrink toward the slot chord
		// length once the wheel becomes crowded.
		const double slotChord = 2.0 * double(ringRadius) * sin(M_PI / double(count));
		const float maxBackdropSize = max(0.4f, float(slotChord * 0.82));
		const float autoSize = clamp<float>(maxBackdropSize / 1.45f, 0.25f, baseSize);
		return autoSize;
	}

	static VRWheelLayoutInfo BuildWheelLayoutInfo(int count)
	{
		VRWheelLayoutInfo layout = {};
		if (count <= 0)
		{
			return layout;
		}

		const float innerRadius = max(5.0f, (float)vr_wheel_radius);
		if (!vr_wheel_auto_split || count <= 15)
		{
			layout.RingCount = 1;
			layout.Rings[0].StartIndex = 0;
			layout.Rings[0].Count = count;
			layout.Rings[0].Radius = innerRadius;
			layout.Rings[0].IconSize = GetWheelIconSizeForCount(count, innerRadius);
			layout.Rings[0].AngleOffset = 0.0;
			return layout;
		}

		const int innerCount = (count + 1) / 2;
		const int outerCount = count - innerCount;
		layout.RingCount = outerCount > 0 ? 2 : 1;
		layout.Rings[0].StartIndex = 0;
		layout.Rings[0].Count = innerCount;
		layout.Rings[0].Radius = innerRadius;
		layout.Rings[0].IconSize = GetWheelIconSizeForCount(innerCount, innerRadius);
		layout.Rings[0].AngleOffset = 0.0;

		if (outerCount > 0)
		{
			const float innerBackdrop = layout.Rings[0].IconSize * 1.45f;
			float outerRadius = innerRadius + innerBackdrop + 2.0f;
			float outerIconSize = GetWheelIconSizeForCount(outerCount, outerRadius);
			const float outerBackdrop = outerIconSize * 1.45f;
			outerRadius = innerRadius + (innerBackdrop * 0.65f) + (outerBackdrop * 0.65f) + 1.25f;
			outerIconSize = GetWheelIconSizeForCount(outerCount, outerRadius);

			layout.Rings[1].StartIndex = innerCount;
			layout.Rings[1].Count = outerCount;
			layout.Rings[1].Radius = outerRadius;
			layout.Rings[1].IconSize = outerIconSize;
			layout.Rings[1].AngleOffset = outerCount > 0 ? (M_PI / double(outerCount)) : 0.0;
		}

		return layout;
	}

	static const VRWheelRingLayout* FindRingForEntry(const VRWheelLayoutInfo& layout, int index, int& localIndex)
	{
		for (int ring = 0; ring < layout.RingCount; ++ring)
		{
			const auto& ringLayout = layout.Rings[ring];
			if (index >= ringLayout.StartIndex && index < ringLayout.StartIndex + ringLayout.Count)
			{
				localIndex = index - ringLayout.StartIndex;
				return &ringLayout;
			}
		}
		localIndex = -1;
		return nullptr;
	}

	static double GetWheelEntryAngle(const VRWheelRingLayout& ring, int localIndex)
	{
		const double slice = (2.0 * M_PI) / double(max(1, ring.Count));
		return (M_PI * 0.5) - (slice * localIndex) + ring.AngleOffset;
	}

	static double DotProduct(const DVector3& a, const DVector3& b)
	{
		return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
	}

	static int GetAimRingIndex(player_t* player, const VRWheelState& wheel, const VRWheelLayoutInfo& layout, const DVector3& center, const DVector3& wheelRight, const DVector3& wheelUp)
	{
		if (layout.RingCount <= 1)
		{
			return 0;
		}

		DVector3 touchPoint;
		if (!GetTouchPoint(player, wheel, touchPoint))
		{
			return 0;
		}

		const DVector3 delta = touchPoint - center;
		const double planeX = DotProduct(delta, wheelRight);
		const double planeY = DotProduct(delta, wheelUp);
		const double radialDistance = sqrt((planeX * planeX) + (planeY * planeY));
		const double switchRadius = layout.Rings[0].Radius * 0.58;
		return radialDistance <= switchRadius ? 0 : 1;
	}

	// [BB] Renamed from UpdateHover: the wrapper below is what callers use, and
	// it is the one that reports a change in hover to the player.
	// [BB] The inverse of GetWheelEntryAngle: which entry lies in the direction
	// (dirX, dirY), with +Y up and +X right in the ring's own plane.
	//
	// This has to be derived from the layout rather than written out again.
	// GetWheelEntryAngle places entry i at (pi/2 - slice*i + offset), so index 0
	// sits at twelve o'clock and indices run CLOCKWISE. Binning a direction with
	// int(angle / slice) walks counter-clockwise instead, which mirrors the wheel:
	// point at three o'clock, select the icon drawn at nine. The aim mode did
	// exactly that, and the stick mode reproduced it before this.
	//
	// Rounds to the nearest entry rather than flooring, so each icon owns the arc
	// centred on it -- which is what the drawn wheel looks like it means.
	static int RingIndexForDirection(const VRWheelRingLayout& ring, double dirX, double dirY)
	{
		if (ring.Count <= 0)
		{
			return -1;
		}

		const double twoPi = 2.0 * M_PI;
		const double slice = twoPi / double(ring.Count);
		// slice * i, recovered from the angle the layout would have produced.
		double sliceOffsets = (M_PI * 0.5) + ring.AngleOffset - atan2(dirY, dirX);
		sliceOffsets = fmod(fmod(sliceOffsets, twoPi) + twoPi, twoPi);

		const int localIndex = int((sliceOffsets + slice * 0.5) / slice) % ring.Count;
		return ring.StartIndex + localIndex;
	}

	static bool UsePointerSelection()
	{
		return !UseCinemaWheelOverride() && vr_wheel_selection_type == 3;
	}

	// [BB] Where the hand is pointing, struck against the plane the wheel is
	// drawn on.
	//
	// This is absolute where the aim mode is relative. Aim measures how far the
	// pose has turned from wherever it happened to be when the wheel opened, so
	// the same physical gesture means different things depending on how you were
	// standing; a ray hitting a plane means the icon it lands on and nothing
	// else. It also stops caring how fast the wheel moves -- pointing is angular,
	// so a hand outrunning a leashed ring still lands where it is aimed, which a
	// reach cannot do.
	static bool GetPointerHit(player_t* player, const VRWheelState& wheel, const DVector3& center,
		const DVector3& forward, DVector3& outHit)
	{
		DVector3 origin;
		if (!GetLocalControllerPose(wheel.AnchorHand, origin))
		{
			if (player == nullptr || player->mo == nullptr)
			{
				return false;
			}
			origin = (wheel.AnchorHand == VR_OFFHAND) ? player->mo->OffhandPos : player->mo->AttackPos;
		}

		DAngle aimYaw = nullAngle;
		DAngle aimPitch = nullAngle;
		GetHandAimAngles(player, wheel.AnchorHand, aimYaw, aimPitch);
		const DVector3 direction = AngleToVector(aimYaw, aimPitch);

		const double denominator = direction | forward;
		if (fabs(denominator) < 1e-6)
		{
			// Pointing along the plane rather than at it; there is no crossing.
			return false;
		}

		const double distance = ((center - origin) | forward) / denominator;
		if (distance <= 0.0)
		{
			// The plane is behind the hand.
			return false;
		}

		outHit = origin + direction * distance;
		return true;
	}

	// Which ring the hit landed on: whichever ring's radius it is nearest, so the
	// gap between two rings resolves to the closer one instead of nothing.
	static int RingIndexForRadius(const VRWheelLayoutInfo& layout, double radius)
	{
		int best = 0;
		double bestDistance = DBL_MAX;
		for (int i = 0; i < layout.RingCount; ++i)
		{
			const double d = fabs(radius - (double)layout.Rings[i].Radius);
			if (d < bestDistance)
			{
				bestDistance = d;
				best = i;
			}
		}
		return best;
	}

	static bool UseStickSelection()
	{
		return !UseCinemaWheelOverride() && vr_wheel_selection_type == 2;
	}

	// [BB] Point the ring with the thumbstick of the hand that owns it.
	//
	// The direction is absolute, not accumulated: where the stick points is which
	// icon is chosen, so the same physical thumb position always means the same
	// weapon. That is the property a stick has and a reach does not -- it can be
	// learned and then performed without looking.
	//
	// Holding past the deadzone keeps the choice; releasing to centre leaves the
	// last one standing rather than clearing it, so letting go of the stick and
	// then the bind still commits what was chosen.
	static bool SolveStickHover(VRWheelState& wheel, const VRWheelLayoutInfo& layout, int& outIndex)
	{
		float stickX = 0.0f;
		float stickY = 0.0f;
		if (!OpenXR_GetThumbstick(wheel.AnchorHand, stickX, stickY))
		{
			// [BB] DESKTOP: THE VIEW IS THE STICK. How far you have turned
			// since the wheel opened, normalised against vr_wheel_desktop_range,
			// is the deflection. OpenYaw/OpenPitch are already recorded at open
			// time for the VR path, so this needs no extra state.
			//
			// Yaw grows counter-clockwise in Doom while stick X is right-
			// positive, hence the negation -- without it the wheel selects the
			// mirror of what you turned toward, which reads as "the wheel is
			// backwards" rather than as a sign error.
			if (!vr_wheel_desktop)
			{
				return false;
			}

			const float range = max<float>(1.0f, vr_wheel_desktop_range);
			const double dYaw   = (r_viewpoint.Angles.Yaw   - wheel.OpenYaw  ).Normalized180().Degrees();
			const double dPitch = (r_viewpoint.Angles.Pitch - wheel.OpenPitch).Normalized180().Degrees();

			stickX = clamp<float>((float)(-dYaw   / range), -1.0f, 1.0f);
			stickY = clamp<float>((float)(-dPitch / range), -1.0f, 1.0f);
		}

		const float deadzone = clamp<float>(vr_wheel_stick_deadzone, 0.05f, 0.95f);
		const float magnitude = sqrtf(stickX * stickX + stickY * stickY);
		if (magnitude < deadzone)
		{
			return false;
		}

		// Push past the deadzone selects the outer ring when there is one, so the
		// two rings are reachable without a second control.
		const int ringIndex = (layout.RingCount > 1 && magnitude >= 0.85f) ? 1 : 0;
		const VRWheelRingLayout& ring = layout.Rings[ringIndex];
		if (ring.Count <= 0)
		{
			return false;
		}

		outIndex = RingIndexForDirection(ring, (double)stickX, (double)stickY);
		return outIndex >= 0;
	}

	static void SolveHover(player_t* player, VRWheelState& wheel)
	{
		UpdateWheelLeash(player, wheel);
		const int previousHover = wheel.HoveredIndex;
		const bool previousValid = wheel.HoverValid;
		wheel.HoveredIndex = -1;
		wheel.HoverValid = false;
		if (player == nullptr || player->mo == nullptr || wheel.Entries.Size() == 0)
		{
			return;
		}

		auto vrmode = VRMode::GetVRModeCached(true);
		if (!VRWheel_Available(vrmode))
		{
			return;
		}

		if (UsePointerSelection())
		{
			DVector3 center;
			DVector3 wheelRight;
			DVector3 wheelUp;
			DVector3 wheelForward;
			DVector3 hit;
			if (!GetWheelLayout(wheel, center, wheelRight, wheelUp, wheelForward)
				|| !GetPointerHit(player, wheel, center, wheelForward, hit))
			{
				return;
			}

			const DVector3 planar = hit - center;
			const double planeX = planar | wheelRight;
			const double planeY = planar | wheelUp;
			const VRWheelLayoutInfo layout = BuildWheelLayoutInfo(wheel.Entries.Size());
			const VRWheelRingLayout& ring = layout.Rings[RingIndexForRadius(layout, sqrt(planeX * planeX + planeY * planeY))];

			const int hover = RingIndexForDirection(ring, planeX, planeY);
			if (hover >= 0 && hover < (int)wheel.Entries.Size())
			{
				wheel.HoveredIndex = hover;
				wheel.HoverValid = wheel.Entries[hover].Selectable;
			}
			return;
		}

		if (UseStickSelection())
		{
			const VRWheelLayoutInfo layout = BuildWheelLayoutInfo(wheel.Entries.Size());
			int stickIndex = -1;
			if (SolveStickHover(wheel, layout, stickIndex) && stickIndex < (int)wheel.Entries.Size())
			{
				wheel.HoveredIndex = stickIndex;
				wheel.HoverValid = wheel.Entries[stickIndex].Selectable;
			}
			else if (previousHover >= 0 && previousHover < (int)wheel.Entries.Size())
			{
				// Thumb back at centre. Keep the pick rather than dropping it, so
				// releasing the stick and then the bind still commits.
				wheel.HoveredIndex = previousHover;
				wheel.HoverValid = previousValid;
			}
			return;
		}

		if (!UseCinemaWheelOverride() && vr_wheel_selection_type == 0)
		{
			DVector3 center;
			DVector3 wheelRight;
			DVector3 wheelUp;
			DVector3 wheelForward;
			if (!GetWheelLayout(wheel, center, wheelRight, wheelUp, wheelForward))
			{
				return;
			}

			DVector3 touchPoint;
			if (!GetTouchPoint(player, wheel, touchPoint))
			{
				return;
			}

			const int count = wheel.Entries.Size();
			const VRWheelLayoutInfo layout = BuildWheelLayoutInfo(count);

			int hoveredIndex = -1;
			double hoveredDistanceSq = DBL_MAX;
			for (int i = 0; i < count; ++i)
			{
				int localIndex = -1;
				const auto* ring = FindRingForEntry(layout, i, localIndex);
				if (ring == nullptr)
				{
					continue;
				}

				const float backdropSize = ring->IconSize * 1.45f;
				const float iconRadius = backdropSize * 0.50f;
				const float touchRadius = backdropSize * 0.25f;
				const float selectDistance = iconRadius + touchRadius;
				const double angle = GetWheelEntryAngle(*ring, localIndex);
				const DVector3 iconCenter = center
					+ wheelRight * (cos(angle) * ring->Radius)
					+ wheelUp * (sin(angle) * ring->Radius);
				const double distanceSq = (touchPoint - iconCenter).LengthSquared();
				if (distanceSq <= double(selectDistance * selectDistance) && distanceSq < hoveredDistanceSq)
				{
					hoveredIndex = i;
					hoveredDistanceSq = distanceSq;
				}
			}

			if (hoveredIndex >= 0)
			{
				wheel.HoveredIndex = hoveredIndex;
				wheel.HoverValid = wheel.Entries[hoveredIndex].Selectable;
			}
			return;
		}

		DAngle aimYaw;
		DAngle aimPitch;
		GetHandAimAngles(player, wheel.AnchorHand, aimYaw, aimPitch);

		const double selectAngle = max(5.0f, (float)vr_wheel_select_angle);
		double x = sin((aimYaw - wheel.OpenYaw).Radians()) / sin(DAngle::fromDeg(selectAngle).Radians());
		double y = (aimPitch - wheel.OpenPitch).Degrees() / selectAngle;
		const double len = sqrt(x * x + y * y);
		if (len > 1.0)
		{
			x /= len;
			y /= len;
		}

		if (len < clamp<float>(vr_wheel_deadzone, 0.05f, 0.95f))
		{
			return;
		}

		const VRWheelLayoutInfo layout = BuildWheelLayoutInfo(wheel.Entries.Size());
		DVector3 center;
		DVector3 wheelRight;
		DVector3 wheelUp;
		DVector3 wheelForward;
		const bool hasWheelLayout = GetWheelLayout(wheel, center, wheelRight, wheelUp, wheelForward);
		const int ringIndex = hasWheelLayout ? GetAimRingIndex(player, wheel, layout, center, wheelRight, wheelUp) : (layout.RingCount > 1 && len >= 0.75 ? 1 : 0);
		const VRWheelRingLayout& ring = layout.Rings[ringIndex];
		const int hover = RingIndexForDirection(ring, x, y);
		if (hover >= 0 && hover < (int)wheel.Entries.Size())
		{
			wheel.HoveredIndex = hover;
			wheel.HoverValid = wheel.Entries[hover].Selectable;
		}
	}

	// [BB] Tell the player the highlight moved. Until now haptics fired exactly
	// once, in OpenWheel, and nothing at all marked a change of hover -- so the
	// only way to know which weapon was about to be committed was to look
	// straight at the ring. A short pulse on acquire means the hand can be
	// checked by feel while the eyes stay on whatever is trying to kill you,
	// which is the whole reason to pick a weapon without opening a menu.
	//
	// Fires on acquire, not on loss: leaving an icon is already reported by the
	// icon it moves on to, and buzzing on the way out doubles every crossing.
	static void UpdateHover(player_t* player, VRWheelState& wheel)
	{
		const int previousHover = wheel.HoveredIndex;
		SolveHover(player, wheel);
		if (wheel.HoveredIndex == previousHover || wheel.HoveredIndex < 0)
		{
			return;
		}

		PlayWheelSound(wheel.HoverValid ? "menu/cursor" : "menu/invalid");
		PlayWheelHaptics(VRMode::GetVRModeCached(true), wheel.AnchorHand, wheel.HoverValid ? 0.35f : 0.12f);
	}

}

void VRWheel_OpenWeapon()
{
	OpenWheel(EVRWheelType::MainWeapon);
}

void VRWheel_CloseWeapon()
{
	CloseWheel(EVRWheelType::MainWeapon);
}

void VRWheel_OpenOffhandWeapon()
{
	OpenWheel(EVRWheelType::OffhandWeapon);
}

void VRWheel_CloseOffhandWeapon()
{
	CloseWheel(EVRWheelType::OffhandWeapon);
}

void VRWheel_OpenInventory()
{
	OpenWheel(EVRWheelType::Inventory);
}

void VRWheel_CloseInventory()
{
	CloseWheel(EVRWheelType::Inventory);
}

void VRWheel_Reset()
{
	for (auto& wheel : GVRWheels)
	{
		const EVRWheelType droppedType = wheel.Type;
		const int droppedHand = wheel.AnchorHand;
		ResetWheel(wheel);
		AnnounceWheelClosed(droppedType, droppedHand);
	}
}

bool VRWheel_IsActive()
{
	return OpenWheelCount() > 0;
}

bool VRWheel_ShouldSuppressGameplayInput()
{
	return VRWheel_IsActive();
}

// [BB] The point of a wheel per hand is that the other hand keeps playing. A
// hand busy holding a ring should lose its trigger; a hand doing nothing of the
// sort should not. Callers that genuinely have no hand to name -- the weapon
// cycling commands, for instance -- still ask VRWheel_ShouldSuppressGameplayInput.
bool VRWheel_ShouldSuppressStickMove()
{
	if (!UseStickSelection())
	{
		return false;
	}
	return OpenWheelCount() > 0;
}

bool VRWheel_ShouldSuppressHandInput(int hand)
{
	if (hand != VR_MAINHAND && hand != VR_OFFHAND)
	{
		return VRWheel_IsActive();
	}
	return IsWheelOpen(WheelForHand(hand));
}

bool VRWheel_ShouldSuppressWeaponHand(int hand)
{
	if (!vr_wheel_hide_hand_weapon || hand != VR_MAINHAND && hand != VR_OFFHAND)
	{
		return false;
	}
	return IsWheelOpen(WheelForHand(hand));
}

// [BB] Reports the main hand's wheel when it has one, otherwise the off hand's.
// With two wheels open there is no single transform to hand back, and every
// caller of this wants one -- so it answers for a wheel rather than pretending
// to answer for both.
bool VRWheel_GetTransform(VSMatrix& out)
{
	VRWheelState* active = nullptr;
	for (auto& wheel : GVRWheels)
	{
		if (IsWheelOpen(wheel))
		{
			active = &wheel;
			break;
		}
	}
	if (active == nullptr)
	{
		return false;
	}

	DVector3 center;
	if (!GetHeadLockedCenter(*active, center))
	{
		return false;
	}

	out.loadIdentity();
	out.translate((FLOATTYPE)center.X, (FLOATTYPE)center.Z, (FLOATTYPE)center.Y);
	active->Transform = out;
	return true;
}

// [BB] One wheel's worth of drawing. Split out of VRWheel_Draw so the public
// entry point can run it once per open wheel -- the guards above it are about
// the frame, not about any particular wheel.
static void DrawOneWheel(HWDrawInfo* di, FRenderState& state, const VRMode* vrmode, player_t* player, VRWheelState& wheel)
{
	RefreshEntries(player, wheel);
	UpdateHover(player, wheel);
	if (wheel.Entries.Size() == 0)
	{
		return;
	}

	DVector3 center;
	DVector3 wheelRight;
	DVector3 wheelUp;
	DVector3 wheelForward;
	if (!GetWheelLayout(wheel, center, wheelRight, wheelUp, wheelForward))
	{
		return;
	}

	const int count = wheel.Entries.Size();
	const VRWheelLayoutInfo layout = BuildWheelLayoutInfo(count);
	const float maxIconSize = layout.RingCount > 1
		? max(layout.Rings[0].IconSize, layout.Rings[1].IconSize)
		: layout.Rings[0].IconSize;
	const float touchIndicatorRadius = (maxIconSize * 1.45f) * 0.25f;
	const float centerIndicatorRadius = maxIconSize * 0.38f;
	const float outerIndicatorRadius = centerIndicatorRadius * 1.85f;
	const float outerIndicatorInnerRadius = centerIndicatorRadius * 1.20f;
	const PalEntry bgColor = PalEntry(MAKEARGB(128,
		RPART(vr_wheel_icon_bg_color),
		GPART(vr_wheel_icon_bg_color),
		BPART(vr_wheel_icon_bg_color)));
	const PalEntry selectedBgColor = PalEntry(MAKEARGB(160,
		RPART(vr_wheel_icon_select_color),
		GPART(vr_wheel_icon_select_color),
		BPART(vr_wheel_icon_select_color)));
	const PalEntry disabledBgColor = PalEntry(MAKEARGB(160,
		RPART(vr_wheel_icon_disable_color),
		GPART(vr_wheel_icon_disable_color),
		BPART(vr_wheel_icon_disable_color)));

	if (!UseCinemaWheelOverride() && vr_wheel_selection_type == 0)
	{
		DVector3 touchPoint;
		if (GetTouchPoint(player, wheel, touchPoint))
		{
			DrawWorldDisc(di, state, touchPoint, wheelRight, wheelUp, touchIndicatorRadius, selectedBgColor);
		}
	}
	else if (UsePointerSelection() && vr_wheel_pointer_dot)
	{
		// [BB] The beam stays invisible; only where it lands is drawn. Nudged
		// toward the viewer off the wheel's own plane so it does not fight the
		// backdrop it sits exactly on top of.
		DVector3 hit;
		if (GetPointerHit(player, wheel, center, wheelForward, hit))
		{
			DrawWorldDisc(di, state, hit + wheelForward * 0.05, wheelRight, wheelUp, touchIndicatorRadius * 0.6f, selectedBgColor);
		}
	}
	else if (layout.RingCount > 1)
	{
		const int ringIndex = GetAimRingIndex(player, wheel, layout, center, wheelRight, wheelUp);
		const PalEntry innerColor = ringIndex == 0 ? selectedBgColor : bgColor;
		const PalEntry outerColor = ringIndex == 1 ? selectedBgColor : bgColor;
		DrawWorldDisc(di, state, center, wheelRight, wheelUp, outerIndicatorRadius, outerColor);
		DrawWorldDisc(di, state, center, wheelRight, wheelUp, outerIndicatorInnerRadius, bgColor);
		DrawWorldDisc(di, state, center, wheelRight, wheelUp, centerIndicatorRadius, innerColor);
	}

	for (int i = 0; i < count; ++i)
	{
		const auto& entry = wheel.Entries[i];
		int localIndex = -1;
		const auto* ring = FindRingForEntry(layout, i, localIndex);
		if (ring == nullptr)
		{
			continue;
		}

		const float iconSize = ring->IconSize;
		const float backdropSize = iconSize * 1.45f;
		const double angle = GetWheelEntryAngle(*ring, localIndex);
		const DVector3 iconCenter = center
			+ wheelRight * (cos(angle) * ring->Radius)
			+ wheelUp * (sin(angle) * ring->Radius);

		PalEntry iconColor = entry.Selectable ? PalEntry(235, 255, 255, 255) : PalEntry(115, 180, 180, 180);
		if (i == wheel.HoveredIndex)
		{
			iconColor = PalEntry(255, 255, 255, 255);
		}

		float iconWidth = iconSize;
		float iconHeight = iconSize;
		GetIconQuadSize(entry.Icon, iconSize, iconWidth, iconHeight);

		const PalEntry backdropColor = i == wheel.HoveredIndex
			? selectedBgColor
			: (entry.Selectable ? bgColor : disabledBgColor);
		DrawWorldDisc(di, state, iconCenter, wheelRight, wheelUp, backdropSize * 0.50f, backdropColor);
		if (entry.ModelFrame != nullptr)
		{
			DrawWheelModel(di, state, entry, iconCenter, wheelForward, iconSize);
		}
		else
		{
			DrawWorldQuad(di, state, iconCenter, wheelRight, wheelUp, iconWidth, iconHeight, entry.Icon, iconColor, true, true);
		}
	}

	// [BB] Last, so it draws over the icons rather than under them -- depth
	// testing is off for all of this, so order is the only thing deciding what
	// wins, and a panel half-hidden behind an icon is worse than no panel.
	DrawWheelInfoPanel(di, state, player, wheel, layout, center, wheelRight, wheelUp);

	state.EnableTexture(true);
	state.EnableBrightmap(true);
	state.SetRenderStyle(STYLE_Translucent);
	state.SetTextureMode(TM_NORMAL);
	state.ResetColor();
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.EnableModelMatrix(false);
	state.EnableDepthTest(true);
	state.SetDepthMask(true);
}

void VRWheel_Draw(HWDrawInfo* di, FRenderState& state)
{
	if (di == nullptr)
	{
		return;
	}

	auto vrmode = VRMode::GetVRModeCached(true);
	if (!VRWheel_Available(vrmode))
	{
		return;
	}

	auto player = &players[consoleplayer];
	if (player == nullptr || player->mo == nullptr || player->health <= 0)
	{
		return;
	}

	InvalidateWheelIfOwnerChanged(player);

	if (menuactive != MENU_Off || ConsoleState != c_up || (VR_UseScreenLayer() && !UseCinemaWheelOverride()))
	{
		return;
	}

	for (auto& wheel : GVRWheels)
	{
		if (IsWheelOpen(wheel))
		{
			DrawOneWheel(di, state, vrmode, player, wheel);
		}
	}
}
