#pragma once
//
// roth_track.h -- THINGS WORKED BY HAND, THE TRACKS, ENGINE-FREE (plain C++, tools/track_test). docs/VR_HANDWORKS.md.
//
// A thing in the world that a hand moves along a fixed track: a door leaf or a lever turning on its hinge (Hinge), a
// key or a knob turning in place (Dial), a thing sliding along a line (Slider), a plate pushed in (Button). Each is a
// port of VRExpansionPlugin's interactible of the same job (MIT, Joshua Statzer, github.com/mordentral/
// VRExpansionPlugin at 3ced69d0, Source/VRExpansionPlugin/{Private,Public}/Interactibles/): VRLeverComponent,
// VRDialComponent, VRSliderComponent, VRButtonComponent. The numbered checklist of what was ported, from which line,
// to which line here, and what was not and why, is docs/VR_HANDWORKS.md section 3 (CLAUDE.md's porting rule).
//
// What they do NOT do: decide anything of the game. A track reports when it passes its threshold (a lever's toggle, a
// dial's angle, a slider's end, a button's engage depth); the engine half (roth_handworks.cpp) then makes the
// ORIGINAL's use there, and the original's chain decides what happens (a door opens or says "Locked.").
//
// UNITS AND FRAMES. Metres, degrees, seconds. Each track works in its own frame, fixed where the thing rests (the
// source's "InitialRelativeTransform * ParentTransform"): the caller hands in the controller's point (the source's
// "pivot location": the real hand, not the drawn one) already in that frame. A Hinge and a Dial turn about their
// frame's +Z (the source's Axis_Z; positive turns +X toward +Y, as an Unreal yaw does: the same sense in our
// right-handed frames, so no sign changes); a Slider runs along its frame's axes; a Button is pressed along -Z.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
// Ported parts: MIT, Copyright Joshua Statzer (docs/licenses/VRExpansionPlugin.txt).
//---------------------------------------------------------------------------
//

#include <cmath>
#include <vector>
#include <algorithm>

namespace roth
{
namespace track
{

struct V3
{
	double x = 0, y = 0, z = 0;
	V3() = default;
	V3(double a, double b, double c) : x(a), y(b), z(c) {}
	V3 operator+(const V3 &o) const { return V3(x + o.x, y + o.y, z + o.z); }
	V3 operator-(const V3 &o) const { return V3(x - o.x, y - o.y, z - o.z); }
	V3 operator*(double s) const { return V3(x * s, y * s, z * s); }
	double Dot(const V3 &o) const { return x * o.x + y * o.y + z * o.z; }
	double Length() const { return std::sqrt(x * x + y * y + z * z); }
	double LengthSq() const { return x * x + y * y + z * z; }
};

constexpr double kPi = 3.14159265358979323846;
constexpr double kSmall = 1e-8;   // Unreal's SMALL_NUMBER

//==========================================================================
// THE HELPERS (VRInteractibleFunctionLibrary.h, and the few Unreal math calls the components use)
//==========================================================================

// FRotator::NormalizeAxis: (-180, 180].
inline double NormalizeAxis(double a)
{
	a = std::fmod(a, 360.0);
	if (a < 0) a += 360.0;
	if (a > 180.0) a -= 360.0;
	return a;
}
// FRotator::ClampAxis: [0, 360).
inline double ClampAxis(double a)
{
	a = std::fmod(a, 360.0);
	if (a < 0) a += 360.0;
	return a;
}
// FMath::ClampAngle [R: Unreal's math library, not in the plugin; reproduced as remembered, tools/track_test checks
// the cases the lever meets: a range about 0 and a range wholly on one side].
inline double ClampAngle(double a, double lo, double hi)
{
	const double maxDelta = ClampAxis(hi - lo) * 0.5;
	const double centre = ClampAxis(lo + maxDelta);
	const double d = NormalizeAxis(a - centre);
	if (d > maxDelta) return NormalizeAxis(centre + maxDelta);
	if (d < -maxDelta) return NormalizeAxis(centre - maxDelta);
	return NormalizeAxis(a);
}
// FMath::FindDeltaAngleDegrees: b - a, wrapped to (-180, 180].
inline double DeltaAngle(double a, double b) { return NormalizeAxis(b - a); }
// FMath::FInterpConstantTo / FInterpTo.
inline double InterpConstantTo(double cur, double target, double dt, double speed)
{
	const double dist = target - cur;
	if (dist * dist < kSmall) return target;
	const double step = speed * dt;
	return cur + std::clamp(dist, -step, step);
}
inline double InterpTo(double cur, double target, double dt, double speed)
{
	if (speed <= 0) return target;
	const double dist = target - cur;
	if (dist * dist < kSmall) return target;
	return cur + dist * std::clamp(dt * speed, 0.0, 1.0);
}
inline V3 VInterpTo(const V3 &cur, const V3 &target, double dt, double speed)
{
	if (speed <= 0) return target;
	const V3 dist = target - cur;
	if (dist.LengthSq() < kSmall) return target;
	return cur + dist * std::clamp(dt * speed, 0.0, 1.0);
}
inline double Sign(double v) { return v > 0 ? 1.0 : (v < 0 ? -1.0 : 0.0); }
// FMath::GridSnap.
inline double GridSnap(double v, double grid) { return grid == 0 ? v : std::floor((v + 0.5 * grid) / grid) * grid; }
// GetAtan2Angle(Axis_Z, p, initial): the angle of `p` about +Z, degrees, less `initial`.
inline double Atan2AngleZ(const V3 &p, double initial = 0.0) { return std::atan2(p.y, p.x) * (180.0 / kPi) - initial; }
// Interactible_GetThresholdSnappedValue.
inline double ThresholdSnapped(double v, double inc, double thresh)
{
	if (inc > 0 && std::fmod(v, inc) <= std::min(inc, thresh)) return GridSnap(v, inc);
	return v;
}
// A point in the frame turned by `deg` about +Z, brought back into the turned thing's own frame (the source's
// GetComponentTransform().InverseTransformPosition for a component turned about its axis).
inline V3 UnturnZ(const V3 &p, double deg)
{
	const double r = -deg * (kPi / 180.0), c = std::cos(r), s = std::sin(r);
	return V3(p.x * c - p.y * s, p.x * s + p.y * c, p.z);
}

//==========================================================================
// THE HINGE: VRLeverComponent with one axis (its Axis_X / Y / Z are one case here: the caller's frame puts the hinge
// on +Z). A door leaf, a lever, a lid. `angle` is the thing's turn from where it rests, degrees.
//==========================================================================

enum class Return { Stay, ReturnToZero, LerpToMax, LerpToMaxIfOverThreshold, RetainMomentum };

struct Hinge
{
	// --- settings (the source's defaults, VRLeverComponent.cpp:28-69) ---
	double limitPositive = 90.0;     // LeverLimitPositive
	double limitNegative = 0.0;      // LeverLimitNegative
	double togglePercentage = 0.8;   // LeverTogglePercentage
	Return returnType = Return::ReturnToZero;
	double returnSpeed = 50.0;       // LeverReturnSpeed, degrees a second
	int framesToAverage = 3;
	double momentumFriction = 5.0;   // LeverMomentumFriction
	double maxMomentum = 180.0;      // MaxLeverMomentum, degrees a second
	double restitution = 0.0;        // LeverRestitution
	double breakDistance = 1.0;      // BreakDistance (100 cm), metres; 0 never
	bool sendEventsDuringLerp = false;
	bool ungripAtTarget = false;     // bUngripAtTargetRotation
	bool locked = false;             // bIsLocked
	bool autoDropWhenLocked = true;  // bAutoDropWhenLocked

	// --- state ---
	double angle = 0.0;              // FullCurrentAngle
	double lastDeltaAngle = 0.0;
	double initialGripRot = 0.0;
	double rotAtGrab = 0.0;
	double momentum = 0.0;           // MomentumAtDrop
	double lastAngle = 0.0;          // LastLeverAngle
	bool state = false;              // bLeverState
	bool firstTick = false;          // bIsInFirstTick
	bool lerping = false;            // bIsLerping
	bool held = false;
	V3 dropLocation;                 // InitialInteractorDropLocation: the hand's grip point in the thing's own frame

	struct Events
	{
		bool toggled = false;        // OnLeverStateChanged
		bool newState = false;
		bool positive = true;        // LeverPositive / LeverNegative
		bool finishedLerp = false;   // OnLeverFinishedLerping
		bool drop = false;           // the hand is let go of (CheckAutoDrop, or bUngripAtTargetRotation)
	};

	// ProccessCurrentState (VRLeverComponent.cpp:222-250).
	void ProcessState(bool wasLerping, bool throwEvents, bool checkAutoDrop, Events &ev)
	{
		const bool newState = (std::fabs(limitNegative) > kSmall && angle <= -(limitNegative * togglePercentage))
			|| (std::fabs(limitPositive) > kSmall && angle >= (limitPositive * togglePercentage));
		if (newState == state) return;
		state = newState;
		if (throwEvents && (sendEventsDuringLerp || !wasLerping))
		{
			ev.toggled = true;
			ev.newState = state;
			ev.positive = angle >= 0.0;
		}
		if (checkAutoDrop && !wasLerping && ungripAtTarget && state && held) ev.drop = true;
	}

	// CalcAngle (VRLeverComponent.cpp:814-840).
	double CalcAngle(const V3 &local, bool skipLimits = false)
	{
		double a = Atan2AngleZ(local, initialGripRot);
		if (skipLimits) return a;
		const bool full = limitPositive > 0 && limitNegative > 0 && std::fabs(limitNegative - 180.0) <= 0.01 && std::fabs(limitPositive - 180.0) <= 0.01;
		if (!full)
		{
			a = ClampAngle(NormalizeAxis(rotAtGrab + a), -limitNegative, limitPositive);
			// a turn that would flip the lever to its other side is ignored (the source's "90 degree allowance")
			if (!firstTick && ((limitPositive > 0 && lastDeltaAngle >= limitPositive) || (limitNegative > 0 && lastDeltaAngle <= -limitNegative))
				&& Sign(lastDeltaAngle) != Sign(a))
				a = lastDeltaAngle;
		}
		firstTick = false;
		return a;
	}

	// OnGrip (VRLeverComponent.cpp:377-452), single axis. `local`: the controller's point in the rest frame.
	void Grip(const V3 &local)
	{
		initialGripRot = Atan2AngleZ(local);
		rotAtGrab = angle;
		dropLocation = UnturnZ(local, angle);
		lastAngle = angle;
		lerping = false;
		firstTick = true;
		momentum = 0.0;
		held = true;
	}

	// CheckAutoDrop (VRLeverComponent.cpp:257-276): the hand is BreakDistance from where it took hold, on the thing.
	bool CheckAutoDrop(const V3 &local) const
	{
		return breakDistance > 0 && (dropLocation - UnturnZ(local, angle)).LengthSq() >= breakDistance * breakDistance;
	}

	// TickGrip (VRLeverComponent.cpp:278-375), single axis; with the momentum sample the source takes in its component
	// tick while held (:200-209).
	Events TickGrip(const V3 &local, double dt)
	{
		Events ev;
		if (locked)
		{
			if (autoDropWhenLocked && CheckAutoDrop(local)) ev.drop = true;
			return ev;
		}
		const double d = CalcAngle(local);
		lastDeltaAngle = d;
		angle = d;
		if (returnType == Return::RetainMomentum && dt > 0)
		{
			momentum -= momentum / framesToAverage;
			momentum += ((angle - lastAngle) / dt) / framesToAverage;
			momentum = std::min(maxMomentum, momentum);
			lastAngle = angle;
		}
		ProcessState(lerping, true, true, ev);
		if (CheckAutoDrop(local)) ev.drop = true;
		return ev;
	}

	// OnGripRelease (VRLeverComponent.cpp:454-470).
	void Release()
	{
		held = false;
		lerping = returnType != Return::Stay;
	}

	// LerpAxis (VRLeverComponent.cpp:738-812).
	void LerpAxis(double dt)
	{
		double target = 0.0, speed = returnSpeed;
		switch (returnType)
		{
		case Return::LerpToMax:
			target = angle >= 0 ? std::round(limitPositive) : -std::round(limitNegative);
			break;
		case Return::LerpToMaxIfOverThreshold:
			if (std::fabs(limitPositive) > kSmall && angle >= limitPositive * togglePercentage) target = std::round(limitPositive);
			else if (std::fabs(limitNegative) > kSmall && angle <= -(limitNegative * togglePercentage)) target = -std::round(limitNegative);
			break;
		case Return::RetainMomentum:
			if (std::fabs(momentum * dt) <= 0.1)
			{
				momentum = 0.0;
				lerping = false;
				return;
			}
			momentum = InterpTo(momentum, 0.0, dt, momentumFriction);
			speed = std::fabs(momentum);
			target = momentum >= 0 ? std::round(limitPositive) : -std::round(limitNegative);
			break;
		default:
			break;
		}
		const double v = InterpConstantTo(angle, target, dt, speed);
		if (std::fabs(v - target) <= kSmall)
		{
			if (restitution > 0) momentum = -(momentum * restitution);
			else lerping = false;
			angle = target;
		}
		else angle = v;
	}

	// TickComponent while not held (VRLeverComponent.cpp:139-220), single axis.
	Events Tick(double dt)
	{
		Events ev;
		const bool wasLerping = lerping;
		if (locked)
		{
			if (wasLerping) { lerping = false; ev.finishedLerp = true; }
			return ev;
		}
		if (lerping) LerpAxis(dt);
		ProcessState(wasLerping, true, true, ev);
		if (wasLerping && !lerping) ev.finishedLerp = true;
		return ev;
	}

	// SetLeverAngle (VRLeverComponent.cpp:648-668): the thing put at an angle by something else (the original's swing).
	Events SetAngle(double a, bool throwEvents = true)
	{
		Events ev;
		angle = a;
		ProcessState(lerping, throwEvents, throwEvents, ev);
		return ev;
	}
};

//==========================================================================
// THE DIAL: VRDialComponent. A key turned in its lock, a knob. `turned` is its angle (CurRotBackEnd), degrees.
// Two ways to turn it (bDialUseDirectHandRotation): the hand's point going round the axis, or the hand's own twist
// about the axis (a key: the caller measures the hand's turn about the key's axis since the last tic and hands it in).
//==========================================================================

struct Dial
{
	// --- settings (VRDialComponent.cpp:27-57) ---
	double clockwiseMax = 180.0;     // ClockwiseMaximumDialAngle
	double cclockwiseMax = 180.0;    // CClockwiseMaximumDialAngle
	bool rollover = false;           // bUseRollover
	double rotationScaler = 1.0;
	bool usesAngleSnap = false;      // bDialUsesAngleSnap
	double snapIncrement = 45.0, snapThreshold = 45.0;
	bool lerpBackOnRelease = false;
	double returnSpeed = 90.0;       // DialReturnSpeed
	double breakDistance = 1.0;      // metres; 0 never

	// --- state ---
	double turned = 0.0;             // CurRotBackEnd
	double current = 0.0;            // CurrentDialAngle (rounded, snapped)
	double lastSnap = 0.0;
	double lastGripRot = 0.0, initialGripRot = 0.0, initialRotBackEnd = 0.0;
	bool lerping = false, held = false;
	V3 dropLocation;

	struct Events { bool hitSnap = false; double snapAngle = 0.0; bool finishedLerp = false; bool drop = false; };

	// AddDialAngle (VRDialComponent.cpp:504-631), without the snap list.
	void AddAngle(double delta, Events *ev)
	{
		const double maxCheck = rollover ? -cclockwiseMax : 360.0 - cclockwiseMax;
		const double temp = rollover ? turned + delta : ClampAxis(turned + delta);
		if (std::fabs(cclockwiseMax) <= kSmall) turned = std::clamp(turned + delta, 0.0, clockwiseMax);
		else if (std::fabs(clockwiseMax) <= kSmall)
		{
			if (rollover) turned = std::clamp(turned + delta, -cclockwiseMax, 0.0);
			else if (turned < maxCheck) turned = std::clamp(360.0 + delta, maxCheck, 360.0);
			else turned = std::clamp(turned + delta, maxCheck, 360.0);
		}
		else if (!rollover && temp > clockwiseMax && temp < maxCheck) turned = turned < maxCheck ? clockwiseMax : maxCheck;
		else if (rollover)
		{
			if (temp > clockwiseMax) turned = clockwiseMax;
			else if (temp < maxCheck) turned = maxCheck;
			else turned = temp;
		}
		else turned = temp;

		if (usesAngleSnap && snapIncrement > 0)
		{
			const double offset = std::fabs(ClampAxis(turned) - ClampAxis(lastSnap));
			const double targetSnap = std::round(GridSnap(turned, snapIncrement));
			if (std::fabs(turned - targetSnap) <= std::min(snapIncrement, snapThreshold))
			{
				if (offset >= snapThreshold)
				{
					current = std::round(GridSnap(turned, snapIncrement));
					if (ev != nullptr && std::fabs(lastSnap - current) > kSmall) { ev->hitSnap = true; ev->snapAngle = current; }
					lastSnap = current;
				}
			}
			else current = std::round(turned);
		}
		else current = std::round(turned);
	}

	// A signed angle, the dial's own: turned as -180..180 (rollover off keeps it in [0, 360)).
	double Signed() const { return rollover ? turned : NormalizeAxis(turned); }

	// OnGrip (VRDialComponent.cpp:211-239). `local`: the hand's point in the dial's rest frame.
	void Grip(const V3 &local)
	{
		dropLocation = UnturnZ(local, turned);
		lastGripRot = ClampAxis(Atan2AngleZ(local));
		initialGripRot = lastGripRot;
		initialRotBackEnd = turned;
		lerping = false;
		held = true;
	}

	// TickGrip (VRDialComponent.cpp:146-209). `local`: the hand's point; `handTurnDeg`: with direct hand rotation, the
	// hand's turn about the dial's axis since the last tic (the caller measures it), else NAN.
	Events TickGrip(const V3 &local, double handTurnDeg)
	{
		Events ev;
		double delta = 0.0;
		if (std::isnan(handTurnDeg))
		{
			const double newRot = ClampAxis(Atan2AngleZ(local));
			delta = rotationScaler * DeltaAngle(lastGripRot, newRot);
			const double limitTest = ClampAxis((newRot - initialGripRot) + initialRotBackEnd);
			const double maxCheck = rollover ? -cclockwiseMax : 360.0 - cclockwiseMax;
			if (std::fabs(cclockwiseMax) <= kSmall)
			{
				if (limitTest > clockwiseMax && (turned == clockwiseMax || turned == 0.0)) delta = 0.0;
			}
			else if (std::fabs(clockwiseMax) <= kSmall)
			{
				if (limitTest < maxCheck && (turned == maxCheck || turned == 0.0)) delta = 0.0;
			}
			else if (limitTest > clockwiseMax && limitTest < maxCheck && (turned == clockwiseMax || turned == maxCheck)) delta = 0.0;
			lastGripRot = newRot;
		}
		else delta = rotationScaler * handTurnDeg;
		AddAngle(delta, &ev);
		if (breakDistance > 0 && (dropLocation - UnturnZ(local, turned)).LengthSq() >= breakDistance * breakDistance) ev.drop = true;
		return ev;
	}

	// OnGripRelease (VRDialComponent.cpp:241-323), the increment snap only.
	Events Release()
	{
		Events ev;
		held = false;
		if (usesAngleSnap && snapIncrement > 0)
		{
			const double offset = std::fabs(ClampAxis(turned) - ClampAxis(lastSnap));
			const double targetSnap = std::round(GridSnap(turned, snapIncrement));
			if (std::fabs(turned - targetSnap) <= std::min(snapIncrement, snapThreshold) && offset >= snapThreshold)
			{
				turned = GridSnap(turned, snapIncrement);
				current = rollover ? std::round(turned) : ClampAxis(std::round(turned));
			}
			else turned = current;
			if (std::fabs(lastSnap - current) > kSmall) { ev.hitSnap = true; ev.snapAngle = current; lastSnap = current; }
		}
		lerping = lerpBackOnRelease;
		return ev;
	}

	// TickComponent (VRDialComponent.cpp:115-144): the lerp back to 0.
	Events Tick(double dt)
	{
		Events ev;
		if (!lerping) return ev;
		if (rollover) SetAngle(InterpConstantTo(turned, 0.0, dt, returnSpeed));
		else if (current > clockwiseMax) SetAngle(InterpConstantTo(turned, 360.0, dt, returnSpeed));
		else SetAngle(InterpConstantTo(turned, 0.0, dt, returnSpeed));
		if (turned == 0.0 || turned == 360.0) { turned = 0.0; current = 0.0; lerping = false; ev.finishedLerp = true; }
		return ev;
	}

	// SetDialAngle (VRDialComponent.cpp:498-502).
	void SetAngle(double a) { turned = a; AddAngle(0.0, nullptr); }
};

//==========================================================================
// THE SLIDER: VRSliderComponent along straight axes (no spline). Its position is a point of its rest frame within the
// box -minSlide..+maxSlide; `progress` 0..1 along that line.
//==========================================================================

enum class SliderDrop { Stay, RetainMomentum };

struct Slider
{
	// --- settings (VRSliderComponent.cpp:23-64) ---
	V3 minSlide = V3(0, 0, 0);       // MinSlideDistance (its absolute values: bUseLegacyLogic off)
	V3 maxSlide = V3(0.10, 0, 0);    // MaxSlideDistance (10 cm)
	SliderDrop dropBehaviour = SliderDrop::Stay;
	double restitution = 0.0;        // SliderRestitution
	double momentumFriction = 3.0;   // SliderMomentumFriction
	double maxMomentum = 1.0;        // MaxSliderMomentum (track lengths a second)
	int framesToAverage = 3;
	bool usesSnapPoints = false;
	double snapIncrement = 0.1, snapThreshold = 0.1;
	bool incrementBetweenSnaps = false;
	double eventThrowThreshold = 1.0;
	double breakDistance = 1.0;      // metres; 0 never

	// --- state ---
	V3 at;                           // the thing's offset from rest (its relative location in the rest frame)
	double progress = 0.0;           // CurrentSliderProgress
	double lastProgressState = -1.0;
	bool hitEventThreshold = false;
	V3 initialInteractor, initialGripLoc, lastAt, momentum;
	V3 dropLocation;
	bool lerping = false, held = false;

	struct Events { bool hitPoint = false; double point = 0.0; bool finishedLerp = false; bool drop = false; };

	V3 MinAbs() const { return V3(std::fabs(minSlide.x), std::fabs(minSlide.y), std::fabs(minSlide.z)); }
	V3 MaxAbs() const { return V3(std::fabs(maxSlide.x), std::fabs(maxSlide.y), std::fabs(maxSlide.z)); }

	// ClampSlideVector (VRSliderComponent.cpp:743-774), the scale factor 1.
	V3 Clamp(const V3 &v) const
	{
		const V3 mn = MinAbs(), dist = MinAbs() + MaxAbs();
		double p[3] = { dist.x > 0 ? (v.x + mn.x) / dist.x : 0.0, dist.y > 0 ? (v.y + mn.y) / dist.y : 0.0, dist.z > 0 ? (v.z + mn.z) / dist.z : 0.0 };
		for (double &q : p) q = std::clamp(usesSnapPoints ? ThresholdSnapped(q, snapIncrement, snapThreshold) : q, 0.0, 1.0);
		return V3(p[0] * dist.x - mn.x, p[1] * dist.y - mn.y, p[2] * dist.z - mn.z);
	}

	// GetCurrentSliderProgress (VRSliderComponent.cpp:847-893), no spline.
	double ProgressOf(const V3 &loc) const
	{
		const V3 a = MinAbs() * -1.0, b = MaxAbs();
		const double len = (b - a).Length();
		double p = len > 0 ? std::clamp((loc - a).Length() / len, 0.0, 1.0) : 0.0;
		if (usesSnapPoints && snapThreshold < snapIncrement)
		{
			if (std::fmod(p, snapIncrement) < snapThreshold) p = GridSnap(p, snapIncrement);
			else if (!incrementBetweenSnaps) p = progress;
		}
		return p;
	}

	// CheckSliderProgress (VRSliderComponent.cpp: the hit-point events, :382-417 of the trimmed listing).
	void CheckProgress(Events &ev)
	{
		if (lastProgressState < 0.0) lastProgressState = usesSnapPoints ? progress : std::round(progress);
		else if (lastProgressState != progress || hitEventThreshold)
		{
			const double mod = std::fmod(progress, snapIncrement);
			if ((!usesSnapPoints && (progress == 1.0 || progress == 0.0))
				|| (usesSnapPoints && snapIncrement > 0 && (std::fabs(mod) <= 0.001 || std::fabs(mod - snapIncrement) <= 0.00001)))
			{
				lastProgressState = usesSnapPoints ? progress : std::round(progress);
				ev.hitPoint = true;
				ev.point = lastProgressState;
				hitEventThreshold = false;
			}
		}
		if (std::fabs(lastProgressState - progress) >= (usesSnapPoints ? std::min(eventThrowThreshold, snapIncrement / 2.0) : eventThrowThreshold))
			hitEventThreshold = true;
	}

	// OnGrip (VRSliderComponent.cpp: OnGrip_Implementation). `local`: the hand's point in the rest frame.
	void Grip(const V3 &local)
	{
		initialInteractor = local;
		initialGripLoc = at;
		dropLocation = local - at;
		hitEventThreshold = false;
		lastAt = at;
		lerping = false;
		momentum = V3();
		held = true;
	}

	// TickGrip (VRSliderComponent.cpp: TickGrip_Implementation), no spline.
	Events TickGrip(const V3 &local, double dt)
	{
		Events ev;
		at = Clamp(initialGripLoc + (local - initialInteractor));
		progress = ProgressOf(at);
		if (dropBehaviour == SliderDrop::RetainMomentum && dt > 0)
		{
			momentum = momentum - momentum * (1.0 / framesToAverage);
			momentum = momentum + ((at - lastAt) * (1.0 / dt)) * (1.0 / framesToAverage);
			lastAt = at;
		}
		CheckProgress(ev);
		if (breakDistance > 0 && (dropLocation - (local - at)).LengthSq() >= breakDistance * breakDistance) ev.drop = true;
		return ev;
	}

	// OnGripRelease (VRSliderComponent.cpp: OnGripRelease_Implementation).
	void Release()
	{
		held = false;
		if (dropBehaviour != SliderDrop::Stay)
		{
			lerping = true;
			const double total = (MinAbs() + MaxAbs()).Length();
			const double m = momentum.Length();
			if (maxMomentum * total < m && m > 0) momentum = momentum * ((total * maxMomentum) / m);
		}
	}

	// TickComponent's lerp (VRSliderComponent.cpp: the non-spline branch).
	Events Tick(double dt)
	{
		Events ev;
		if (!lerping) return ev;
		if ((momentum * dt).LengthSq() <= 0.00001 * 0.00001) lerping = false;
		else
		{
			momentum = VInterpTo(momentum, V3(), dt, momentumFriction);
			at = Clamp(at + momentum * dt);
			progress = ProgressOf(at);
			if (restitution > 0)
			{
				const V3 mn = MinAbs(), mx = MaxAbs();
				if ((mn.x > 0 && at.x <= -mn.x) || (mn.y > 0 && at.y <= -mn.y) || (mn.z > 0 && at.z <= -mn.z)
					|| (mx.x > 0 && at.x >= mx.x) || (mx.y > 0 && at.y >= mx.y) || (mx.z > 0 && at.z >= mx.z))
					momentum = momentum * -restitution;
			}
			else if (progress <= 0.00001 || progress >= 1.0 - 0.00001)
			{
				lerping = false;
				progress = progress < 0.5 ? 0.0 : 1.0;
			}
		}
		if (!lerping) ev.finishedLerp = true;
		CheckProgress(ev);
		return ev;
	}
};

//==========================================================================
// THE BUTTON: VRButtonComponent. A plate pushed in along -Z of its frame by a touching point (a fingertip, a palm).
//==========================================================================

enum class ButtonType { Press, ToggleReturn, ToggleStay };

struct Button
{
	// --- settings (VRButtonComponent.cpp: the constructor) ---
	double depressDistance = 0.08;   // DepressDistance (8 cm)
	double engageDepth = 0.08;       // ButtonEngageDepth
	double depressSpeed = 0.50;      // DepressSpeed (50 cm/s), metres a second
	ButtonType type = ButtonType::ToggleReturn;
	double minTimeBetween = 0.1;     // MinTimeBetweenEngaging, seconds
	bool enabled = true;

	// --- state ---
	double depth = 0.0;              // the plate's offset along Z (0 at rest, negative pushed in)
	bool buttonState = false;
	bool touching = false;           // LocalInteractingComponent set
	bool toggledThisTouch = false;
	bool active = false;             // the component's tick is on (touched, or going back)
	double touchStart = 0.0;         // InitialLocation's Z: where the touching point came in
	double depthAtTouch = 0.0;       // InitialComponentLoc's Z
	double lastToggle = -1e9;

	struct Events { bool stateChanged = false; bool newState = false; bool endInteraction = false; };

	// The rest position (GetTargetRelativeLocation).
	double Target() const { return (type == ButtonType::ToggleStay && buttonState) ? -(engageDepth + 0.01 * 0.01) : 0.0; }

	// OnOverlapBegin / OnOverlapEnd, and TickComponent (VRButtonComponent.cpp): `inside` -- the touching point is on the
	// plate's face or through it this tic; `pointZ` its height along the button's axis; `now` seconds.
	Events Tick(bool inside, double pointZ, double now, double dt)
	{
		Events ev;
		if (inside && !touching && enabled)
		{
			touching = true;
			touchStart = pointZ;
			depthAtTouch = depth;
			toggledThisTouch = false;
		}
		else if (!inside) touching = false;
		if (touching)
		{
			if (!enabled) { touching = false; return ev; }
			const double check = std::clamp(touchStart - pointZ, 0.0, depressDistance);
			if (check > 0)
			{
				const double clampMin = (type == ButtonType::ToggleStay && buttonState) ? -(engageDepth + 0.01 * 0.01) : 0.0;
				depth = std::clamp(depthAtTouch - check, -depressDistance, clampMin);
				if ((type == ButtonType::ToggleReturn || type == ButtonType::ToggleStay) && !toggledThisTouch
					&& depth <= -engageDepth + 1e-4 && now - lastToggle >= minTimeBetween)
				{
					lastToggle = now;
					toggledThisTouch = true;
					buttonState = !buttonState;
					ev.stateChanged = true;
					ev.newState = buttonState;
				}
			}
		}
		else if (active)
		{
			// the source's tick runs only until the plate is back (SetComponentTickEnabled(false) at its target)
			const double t = Target();
			depth = InterpConstantTo(depth, t, dt, depressSpeed);
			if (std::fabs(depth - t) <= 1e-4) { depth = t; active = false; ev.endInteraction = true; }
		}
		if (touching) active = true;
		if (type == ButtonType::Press)
		{
			const bool check = depth <= -engageDepth + 1e-4;
			if (buttonState != check && now - lastToggle >= minTimeBetween)
			{
				lastToggle = now;
				buttonState = check;
				ev.stateChanged = true;
				ev.newState = buttonState;
			}
		}
		return ev;
	}
};

} // namespace track
} // namespace roth
