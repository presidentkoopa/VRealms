// roth_handworks.cpp -- see roth_handworks.h and docs/VR_HANDWORKS.md (what a door does in a hand, the checklist of
// what was ported from VRExpansionPlugin's VRLeverComponent and where it lands here).
//
// THE GAME UNDERNEATH: the only thing a door in a hand does to the game is the ORIGINAL's use of the door's face (L1,
// RulesUse: activate_targeted_object, the face's chain, the item in that hand), once per grip, when the latched leaf
// is drawn 4 cm along its swing. Whether the door opens is that chain's answer (a FREE door's 0x2f, a KEY door's 0x27
// refusing). Everything after -- the leaf following the hand, swung on by the hand's momentum, finishing by its own
// swing -- moves the polyobject the original's own swing moves, between the same two stops, and never fires a use.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roth_handworks.h"
#include "roth_track.h"
#include "roth_vrpick.h"
#include "roth_runtime.h"
#include "roth_physics.h"
#include "roth_worldhands.h"

#include "actor.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "playsim/po_man.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "printf.h"
#include "hw_vrmodes.h"
#include "r_data/vr_handpose.h"

#include <math.h>
#include <algorithm>
#include <vector>

EXTERN_CVAR(Bool, roth_vrpick_debug)   // roth_vrpick.cpp; file scope, never inside a namespace

namespace roth
{

namespace
{

constexpr double kDt = 1.0 / 35.0;
constexpr double kLatchM = 0.04;              // WORLD_DESIGN.md 1.3: the hand 4 cm along the swing at the handle = the use
constexpr double kReachM = 0.10;              // a palm this near a door's face or leaf takes it (the picker's wall reach)
constexpr double kLatchHalf = 0.5;            // a door is taken on its latch half (WORLD_DESIGN.md 1.1: the handle's edge)
constexpr double kBreakM = 0.30;              // the controller this far from its hold on the leaf lets go [R]
constexpr double kOpenDeg = 90.0;             // the original's arc: 64 angle units of 1.40625 degrees (SwingDoor)
constexpr double kSwingDegPerSec = 98.4375;   // the original's swing at M = 1 (SwingDoor: 1.40625 deg x 70 Hz)

struct Hold
{
	bool active = false;     // the hand is on the door
	bool coasting = false;   // let go of, still swinging on the hand's momentum
	bool opened = false;     // the door is open (the use opened it, or it was open when taken): the leaf is the hand's
	bool used = false;       // this grip's one use was made
	bool driving = false;    // the leaf is held (EV_HoldPoly)
	int tag = 0;
	Pick pick;
	DoorHinge door;          // as taken
	track::Hinge hinge;
	double alongU = 0, outU = 0, z = 0;   // where the controller holds the leaf, in the leaf's own frame (map units)
	double latchDeg = 0;
	int loggedDeg = -1000;
	// THE HOLD STARTS WHERE THE HAND IS (VRSlider's InitialGripLoc + (CurInteractor - InitialInteractor)): the drawn
	// hand's grip point when it took hold (a hand stopped on the door's face, its controller a few cm inside it), then
	// moved by the controller's motion since.
	DVector3 handAtGrab, ctrlAtGrab;
	track::Slack slack;      // the controller gone on past the hand at the grip (roth_track.h THE SLACK)
};

struct State
{
	FLevelLocals *level = nullptr;
	Hold hand[2];
} S;

const char *RoleName(int role) { return role == VR_MAINHAND ? "main" : "off"; }
bool Logging() { return VRPickTesting() || roth_vrpick_debug; }
double UPM() { return VRPickUnitsPerMetre(); }
double PS() { return S.level != nullptr && S.level->pixelstretch > 0.1 ? S.level->pixelstretch : 1.0; }

// The door's frame, as taken: X the closed leaf (pivot to latch), Y the way it opens, Z = X x Y. The hinge turns +X
// toward +Y (roth_track.h).
DVector2 FrameX(const DoorHinge &d) { return d.closedDir; }
DVector2 FrameY(const DoorHinge &d) { return DVector2(-d.closedDir.Y, d.closedDir.X) * (double)d.sense; }

track::V3 Local(const DoorHinge &d, const DVector3 &p)
{
	const DVector2 r = p.XY() - d.pivot;
	return track::V3((r | FrameX(d)) / UPM(), (r | FrameY(d)) / UPM(), p.Z * PS() / UPM());
}

// The leaf's own axes at `deg` open.
void LeafAxes(const DoorHinge &d, double deg, DVector2 &along, DVector2 &out)
{
	const double a = deg * (M_PI / 180.0);
	along = FrameX(d) * cos(a) + FrameY(d) * sin(a);
	out = FrameY(d) * cos(a) - FrameX(d) * sin(a);
}

DVector3 LeafPoint(const Hold &h, double deg)
{
	DVector2 along, out;
	LeafAxes(h.door, deg, along, out);
	const DVector2 p = h.door.pivot + along * h.alongU + out * h.outU;
	return DVector3(p.X, p.Y, h.z);
}

bool Controller(int role, DVector3 &o)
{
	DVector3 f, r, u;
	return VRHandPose_TrackedMapFrame(role, o, f, r, u);
}

// The drawn palm's offset from the drawn hand's grip point (the pin places the grip point; the hold is the palm's).
DVector3 PalmOffset(int role, const VRPickHand &v)
{
	DVector3 o, f, r, u;
	return VRHandPose_MapFrame(role, o, f, r, u) ? v.palm - o : DVector3(0, 0, 0);
}

// The point the hold reads this tic (see Hold::handAtGrab).
DVector3 Interactor(Hold &h, const DVector3 &ctrl)
{
	const track::V3 p = h.slack.Point(track::V3(ctrl.X, ctrl.Y, ctrl.Z));
	return DVector3(p.x, p.y, p.z);
}

double LeafAngle(int tag, AActor *pmo)
{
	DoorHinge d;
	return DoorHingeOf(tag, pmo, d) ? d.angleDeg : 0.0;
}

// Turn the held leaf to `want` degrees open (along its sense). False: something in the way (the leaf stays).
bool TurnTo(const Hold &h, AActor *pmo, double want)
{
	const double cur = LeafAngle(h.tag, pmo);
	const double delta = want - cur;
	if (fabs(delta) < 0.01) return true;
	return DoorTurnHeld(h.tag, delta * h.door.sense);
}

// THE CLOSE BY HAND: a leaf the hand leaves within its latch of shut (the same 4 cm of arc that opens it) is the
// original's use on the open door's leaf -- toggle_door_open_state (roth_runtime.cpp ToggleDoor): it closes, from
// there. A door already closing by itself is left to it.
void ShutIfNearlyShut(int role, Hold &h, AActor *pmo)
{
	DoorHinge d;
	if (role < 0 || !h.opened || S.level == nullptr || !DoorHingeOf(h.tag, pmo, d) || d.closing) return;
	if (d.angleDeg > std::max(h.latchDeg, 1.0)) return;
	FPolyObj *po = S.level->GetPolyobj(h.tag);
	if (po == nullptr || po->Linedefs.Size() == 0) return;
	Pick p;
	p.kind = Pick::DoorLeaf;
	p.leafTag = h.tag;
	p.line = po->Linedefs[0];
	p.side = 0;
	p.hitPos = LeafPoint(h, d.angleDeg);
	const bool acted = RulesUse(role, p, "a door's leaf brought back to shut");
	Printf("[roth_handworks] %s hand brings door %d back to %.1f degrees (its latch is %.1f): the original's use on the leaf -> %s\n",
		RoleName(role), h.tag, d.angleDeg, h.latchDeg, acted ? "it closes" : "nothing happened");
	if (acted) VRPickHaptic(role, 0.5, 40, Logging() ? "the door shuts" : nullptr);
}

void Unhold(Hold &h, const char *why, int role = -1, AActor *pmo = nullptr)
{
	if (h.driving)
	{
		ShutIfNearlyShut(role, h, pmo);
		DoorHold(h.tag, false);
		Printf("[roth_handworks] door %d: the hand lets the leaf go at %.1f degrees (%s) -- its own swing takes it from there\n", h.tag,
			LeafAngle(h.tag, nullptr), why);
	}
	h.driving = false;
	h.coasting = false;
}

void Release(int role, AActor *pmo, const char *why)
{
	Hold &h = S.hand[role];
	if (!h.active) return;
	PhysicsHandPin(role, nullptr);
	h.active = false;
	if (h.driving)
	{
		h.hinge.Release();
		const double m = h.hinge.momentum;
		if (h.hinge.lerping && fabs(m) > kSwingDegPerSec)
		{
			h.coasting = true;
			Printf("[roth_handworks] %s hand lets go of door %d (%s) at %.1f degrees, swinging %.0f degrees a second: it swings on\n",
				RoleName(role), h.tag, why, h.hinge.angle, m);
			return;
		}
		Printf("[roth_handworks] %s hand lets go of door %d (%s) at %.1f degrees\n", RoleName(role), h.tag, why, h.hinge.angle);
		Unhold(h, "let go", role, pmo);
		return;
	}
	if (Logging() || !h.used)
		Printf("[roth_handworks] %s hand lets go of door %d (%s)%s\n", RoleName(role), h.tag, why,
			h.used ? "" : FStringf(", the leaf drawn %.1f of the %.1f degrees its latch needs: nothing", h.hinge.angle, h.latchDeg).GetChars());
	(void)pmo;
}

// The door is open now: the leaf is the hand's from here.
void TakeLeaf(int role, AActor *pmo)
{
	Hold &h = S.hand[role];
	if (!DoorHold(h.tag, true)) return;
	h.driving = true;
	h.opened = true;
	DoorHinge d;
	if (DoorHingeOf(h.tag, pmo, d)) h.hinge.SetAngle(std::clamp(d.angleDeg, 0.0, kOpenDeg), false);
}

void TickHold(int role, AActor *pmo)
{
	Hold &h = S.hand[role];
	VRPickHand v;
	DVector3 o;
	if (!VRPickHandNow(role, v) || !Controller(role, o)) { Release(role, pmo, "the hand is lost"); return; }
	if (!v.gripClosed) { Release(role, pmo, "the grip opened"); return; }
	// the controller's palm: the drawn palm less the drawn hand's offset from the controller (the pin, the physics stop)
	DVector3 ctrlPalm = o;
	{
		DVector3 d0, f, r, u;
		if (VRHandPose_MapFrame(role, d0, f, r, u)) ctrlPalm = v.palm - (d0 - o);
	}
	const track::V3 local = Local(h.door, Interactor(h, ctrlPalm));

	if (!h.opened)
	{
		// THE LATCH: the leaf does not move; the hand's turn about the hinge is measured, and at the latch the use.
		h.hinge.TickGrip(local, kDt);
		if (Logging() && (int)(h.hinge.angle * 2.0) != h.loggedDeg)
		{
			h.loggedDeg = (int)(h.hinge.angle * 2.0);
			Printf("[roth_handworks] %s hand on door %d's handle: %.1f of the %.1f degrees its latch needs (the hand at %.3f %.3f in the door's frame;"
				" controller (%.1f %.1f), drawn palm (%.1f %.1f))\n", RoleName(role), h.tag, h.hinge.angle, h.latchDeg, local.x, local.y, o.X, o.Y,
				v.palm.X, v.palm.Y);
		}
		if (!h.used && h.hinge.angle >= h.latchDeg)
		{
			h.used = true;
			const bool acted = RulesUse(role, h.pick, "a door's handle drawn along its swing");
			DoorHinge d;
			const bool open = DoorHingeOf(h.tag, pmo, d) && (d.swinging || d.settledOpen || fabs(d.angleDeg) >= 1.0);
			Printf("[roth_handworks] %s hand draws door %d's handle %.1f degrees along its swing (%.3f m): the original's use -> %s%s\n",
				RoleName(role), h.tag, h.hinge.angle, h.hinge.angle * (M_PI / 180.0) * sqrt(local.x * local.x + local.y * local.y),
				acted ? "acted" : "nothing happened", open ? "; the door opens: the leaf is the hand's" : "; the door stays shut");
			if (open)
			{
				TakeLeaf(role, pmo);
				VRPickHaptic(role, 0.5, 40, Logging() ? "the latch gives" : nullptr);
			}
		}
		// the hand stays on the handle meanwhile (the leaf is shut: it holds still)
		if (!h.opened)
		{
			const DVector3 pin = LeafPoint(h, std::clamp(LeafAngle(h.tag, pmo), 0.0, kOpenDeg)) - PalmOffset(role, v);
			PhysicsHandPin(role, &pin);
		}
		return;
	}

	// THE LEAF IN THE HAND.
	if (!h.driving) TakeLeaf(role, pmo);
	const track::Hinge::Events ev = h.hinge.TickGrip(local, kDt);
	if (h.driving && !TurnTo(h, pmo, h.hinge.angle))
	{
		const double cur = LeafAngle(h.tag, pmo);
		if (Logging()) Printf("[roth_handworks] %s hand: door %d is stopped at %.1f degrees (something in its way)\n", RoleName(role), h.tag, cur);
		h.hinge.SetAngle(cur, false);
	}
	const double now = LeafAngle(h.tag, pmo);
	const DVector3 pin = LeafPoint(h, now) - PalmOffset(role, v);
	PhysicsHandPin(role, &pin);
	if (Logging() && (int)(now / 5.0) != h.loggedDeg)
	{
		h.loggedDeg = (int)(now / 5.0);
		Printf("[roth_handworks] %s hand swings door %d: %.1f degrees open\n", RoleName(role), h.tag, now);
	}
	if (ev.drop) Release(role, pmo, FStringf("the hand %.2f m from its hold", h.hinge.breakDistance).GetChars());
}

void TickCoast(int role, AActor *pmo)
{
	Hold &h = S.hand[role];
	if (!h.coasting) return;
	if (!h.hinge.lerping || fabs(h.hinge.momentum) <= kSwingDegPerSec) { Unhold(h, "its momentum spent", role, pmo); return; }
	h.hinge.Tick(kDt);
	if (!TurnTo(h, pmo, h.hinge.angle)) { Unhold(h, "stopped by something in its way", role, pmo); return; }
	if (!h.hinge.lerping) Unhold(h, h.hinge.angle >= kOpenDeg - 0.01 ? "at its stop" : "at rest", role, pmo);
}

} // namespace (anonymous)

//===========================================================================
// The entry points
//===========================================================================

void HandworksTick(FLevelLocals *level, AActor *pmo, bool live)
{
	if (level != S.level) { HandworksEndLevel(); S.level = level; }
	for (int role = 0; role < 2; role++)
	{
		Hold &h = S.hand[role];
		if (h.active)
		{
			if (!live || pmo == nullptr) Release(role, pmo, "the world is held");
			else TickHold(role, pmo);
		}
		if (h.coasting && live && pmo != nullptr) TickCoast(role, pmo);
	}
}

void HandworksEndLevel()
{
	for (int role = 0; role < 2; role++)
	{
		if (S.hand[role].active) PhysicsHandPin(role, nullptr);
		if (S.hand[role].driving && S.level != nullptr) DoorHold(S.hand[role].tag, false);
		S.hand[role] = Hold();
	}
	S.level = nullptr;
}

bool HandworksGripTaken(int role, AActor *pmo)
{
	if (role < 0 || role > 1 || pmo == nullptr || S.level == nullptr) return false;
	Hold &h = S.hand[role];
	if (h.active) return true;
	if (h.coasting) Unhold(h, "taken again");
	VRPickHand v;
	DVector3 o;
	if (!VRPickHandNow(role, v) || !v.havePoints || !Controller(role, o)) return false;
	const double upm = UPM();
	Pick p;
	if (!PickTouchSurface(pmo, v.palm, kReachM * upm, p) || (p.kind != Pick::WallFace && p.kind != Pick::DoorLeaf)) return false;
	int tag = 0;
	if (!DoorTagOfPick(p, tag)) return false;
	// a face of the owner's pull table (the bookcase leaves, opened from their edge: Q30) keeps that rule
	if (WorldHandsPullRecord(UseRecordOf(p))) return false;
	// the item in this hand is the key this door's use tests: it is turned in the lock (roth_worldhands.cpp THE TWIST),
	// not pulled. Any other item drawn in the hand leaves its grip free (docs/VR_INTERACTION.md 4, THE ITEM IN THE HAND).
	const int item = VRPickHeldItem(role);
	if (item > 0)
	{
		std::vector<ChainStep> chain;
		const uint16_t rec = UseRecordOf(p);
		if (rec != 0 && RecordChain(rec, chain))
			for (const ChainStep &c : chain)
				if (c.opcode == 0x27 && c.key == (uint16_t)item) return false;
	}
	if (S.hand[role ^ 1].active && S.hand[role ^ 1].tag == tag) return false;   // one hand on a door (VRLever: no multiple grips)
	DoorHinge d;
	if (!DoorHingeOf(tag, pmo, d)) return false;
	if (v.palm.Z < d.zBottom || v.palm.Z > d.zTop) return false;
	const bool open = d.swinging || d.settledOpen || fabs(d.angleDeg) >= 1.0;
	DVector2 along, out;
	LeafAxes(d, std::clamp(d.angleDeg, 0.0, kOpenDeg), along, out);
	const double alongU = (v.palm.XY() - d.pivot) | along;
	if (alongU < kLatchHalf * d.length)
	{
		if (Logging()) Printf("[roth_handworks] %s hand closes on door %d %.2f of the way from its hinge: not its latch half -- the picker's\n",
			RoleName(role), tag, alongU / d.length);
		return false;
	}
	if (!open)
	{
		// a closed door: only one the original's cursor would let a click reach (too far or nothing: the picker answers)
		const rules::Class c = RulesClassOf(RulesClassify(p));
		if (c != rules::Class::Usable && c != rules::Class::Blocked) return false;
	}
	h = Hold();
	h.active = true;
	h.tag = tag;
	h.pick = p;
	h.door = d;
	h.opened = open;
	h.hinge.limitPositive = kOpenDeg;
	h.hinge.limitNegative = 0.0;
	h.hinge.togglePercentage = 1.0;
	h.hinge.returnType = track::Return::RetainMomentum;
	h.hinge.breakDistance = kBreakM;
	h.hinge.angle = std::clamp(d.angleDeg, 0.0, kOpenDeg);
	h.ctrlAtGrab = o;
	h.handAtGrab = v.palm;   // the palm the grab was tested at (the drawn hand's: on the door's face)
	{
		// the controller's palm: the drawn palm moved by the drawn hand's offset from the controller
		DVector3 d0, f, r, u;
		const DVector3 ctrlPalm = VRHandPose_MapFrame(role, d0, f, r, u) ? v.palm - (d0 - o) : o;
		h.slack.Grab(track::V3(v.palm.X, v.palm.Y, v.palm.Z), track::V3(ctrlPalm.X, ctrlPalm.Y, ctrlPalm.Z));
		h.ctrlAtGrab = ctrlPalm;
	}
	o = h.handAtGrab;
	const track::V3 local = Local(d, o);
	h.hinge.Grip(local);
	h.alongU = (o.XY() - d.pivot) | along;
	h.outU = (o.XY() - d.pivot) | out;
	h.z = o.Z;
	const double rM = std::max(0.10, sqrt(local.x * local.x + local.y * local.y));
	h.latchDeg = kLatchM / rM * (180.0 / M_PI);
	Printf("[roth_handworks] %s hand takes door %d %s (%.2f m from its hinge, %s): %s\n", RoleName(role), tag,
		open ? FStringf("by its leaf, %.1f degrees open", d.angleDeg).GetChars() : "by its handle, shut", rM,
		d.sense > 0 ? "it opens anticlockwise" : "it opens clockwise",
		open ? "the leaf follows the hand" : FStringf("draw it %.2f m (%.1f degrees) along its swing", kLatchM, h.latchDeg).GetChars());
	VRPickHaptic(role, 0.15, 12, Logging() ? "a door's handle" : nullptr);
	return true;
}

bool HandworksHolding(int role)
{
	return role >= 0 && role <= 1 && S.hand[role].active;
}

// A HAND'S USE ON A DOOR'S LEAF (a test instrument, inert until typed): `roth_handworks_leafuse <tag> [main|off]`
// makes that hand's L1 use (RulesUse, the reach and the click gate included) on door <tag>'s leaf at its middle, as a
// squeeze on it would. An open door's leaf: toggle_door_open_state (roth_runtime.cpp ToggleDoor).
CCMD(roth_handworks_leafuse)
{
	FLevelLocals *level = S.level;
	if (level == nullptr || argv.argc() < 2) { Printf("roth_handworks_leafuse <tag> [main|off]\n"); return; }
	const int tag = atoi(argv[1]);
	const int role = argv.argc() >= 3 && !stricmp(argv[2], "off") ? VR_OFFHAND : VR_MAINHAND;
	AActor *pmo = level->GetConsolePlayer() != nullptr ? level->GetConsolePlayer()->mo : nullptr;
	FPolyObj *po = level->GetPolyobj(tag);
	DoorHinge d;
	if (po == nullptr || po->Linedefs.Size() == 0 || !DoorHingeOf(tag, pmo, d)) { Printf("roth_handworks_leafuse: no swinging door %d\n", tag); return; }
	DVector2 along, out;
	LeafAxes(d, std::clamp(d.angleDeg, 0.0, kOpenDeg), along, out);
	Pick p;
	p.kind = Pick::DoorLeaf;
	p.leafTag = tag;
	p.line = po->Linedefs[0];
	const DVector2 mid = d.pivot + along * (d.length * 0.5);
	p.hitPos = DVector3(mid.X, mid.Y, d.zBottom + 0.43 * (d.zTop - d.zBottom));
	const bool acted = RulesUse(role, p, "test: a use on a door's leaf");
	Printf("[roth_handworks] test: %s hand's use on door %d's leaf at %.1f degrees open -> %s\n", RoleName(role), tag, d.angleDeg,
		acted ? "acted" : "nothing happened");
}

// THE DOOR CENSUS (a test instrument, inert until typed): every swinging door of the level -- its tag, hinge, length,
// sense, how open it is -- and the use on each room face beside it at handle height: the record, and what its chain
// does to a door (0x2f opens one, 0x27 tests an item in hand, 0x36 the refusal line, disabled).
CCMD(roth_handworks_doors)
{
	FLevelLocals *level = S.level;
	if (level == nullptr) { Printf("roth_handworks_doors: no level yet (the hands have not ticked)\n"); return; }
	AActor *pmo = level->GetConsolePlayer() != nullptr ? level->GetConsolePlayer()->mo : nullptr;
	int n = 0;
	for (auto &po : level->Polyobjects)
	{
		DoorHinge d;
		if (!DoorHingeOf(po.tag, pmo, d)) continue;
		n++;
		FString faces;
		const double hz = d.zBottom + 0.43 * (d.zTop - d.zBottom);
		for (line_t &ln : level->lines)
		{
			Pick fp;
			fp.kind = Pick::WallFace;
			fp.line = &ln;
			int tag = 0;
			if (!DoorTagOfPick(fp, tag) || tag != po.tag) continue;
			for (int sd = 0; sd < 2; sd++)
			{
				if (ln.sidedef[sd] == nullptr) continue;
				Pick p = fp;
				p.side = sd;
				p.band = 0x01;
				const DVector2 mid = (ln.v1->fPos() + ln.v2->fPos()) * 0.5;
				p.hitPos = DVector3(mid.X, mid.Y, hz);
				const uint16_t rec = UseRecordOf(p);
				if (rec == 0) continue;
				std::vector<ChainStep> chain;
				RecordChain(rec, chain);
				FString what;
				for (const ChainStep &c : chain)
				{
					if (c.opcode == 0x2f) what += " opens";
					else if (c.opcode == 0x27) what.AppendFormat(" tests-item-%u", c.key);
					else if (c.opcode == 0x36) what += " refusal-line";
				}
				faces.AppendFormat(" | line %d side %d #%u%s", ln.Index(), sd, rec, what.IsEmpty() ? " (no door step)" : what.GetChars());
			}
		}
		Printf("[roth_handworks] door %d: hinge (%.0f %.0f), %.0f long, %.0f..%.0f high, opens %s, %.0f degrees open%s%s\n", po.tag, d.pivot.X,
			d.pivot.Y, d.length, d.zBottom, d.zTop, d.sense > 0 ? "anticlockwise" : "clockwise", d.angleDeg, d.swinging ? " (swinging)" : "",
			faces.GetChars());
	}
	Printf("[roth_handworks] %s: %d swinging door(s)\n", level->MapName.GetChars(), n);
}

} // namespace roth
