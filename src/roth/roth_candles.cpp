// roth_candles.cpp -- see roth_candles.h (the design) and roth_wicks.h (the pictures, the state, the light).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "roth_candles.h"
#include "roth_wicks.h"
#include "roth_objects.h"
#include "roth_runtime.h"
#include "roth_texture.h"
#include "roth_das.h"
#include "roth_raw.h"
#include "roth_log.h"
#include "roth_vrpick.h"
#include "roth_interface.h"
#include "roth_inventory.h"

#include "actor.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "printf.h"
#include "c_cvars.h"
#include "r_utility.h"
#include "r_data/vr_handpose.h"
#include "r_data/vr_handpose_math.h"   // VRHandPose::SIDE_RIGHT
#include "hw_vrmodes.h"                // VR_MAINHAND
#include "hw_effectlights.h"

#include <math.h>
#include <algorithm>
#include <map>
#include <vector>

EXTERN_CVAR(Bool, roth_vrpick_debug)   // roth_vrpick.cpp; file scope, never inside a namespace

namespace roth
{

namespace
{

// A flame this close to a wick lights it (docs/WORLD_DESIGN.md 2.3: "within 3 cm"), in metres.
constexpr double kWickReach = 0.03;
// The first wick of a use made with the palm on the candle: the wick nearest the hand if within this.
constexpr double kFirstWickNear = 0.12;
// Past the catch, a holder whose original art never became the lit picture is handed back anyway.
constexpr int kHandBackLate = 70;
// The light's colour per unit of intensity (a candle's warm yellow), times kLightPerWick.
constexpr float kLightColour[3] = { 1.0f, 0.56f, 0.22f };

struct Kind
{
	WickModel model;
	uint16_t litWord = 0;
	std::vector<int> maskSprite;   // [mask] -> sprite, 1..full
	std::string name;              // "DEMO[4098] -> DEMO[4148]"
};

struct Candle
{
	LightableObject spec;
	int kind = -1;
	WickLighting w;
	int startTic = -1;             // the level's maptime when the lighting started
	int litSeenTic = -1;           // the maptime the original's art was first seen lit, unlit wicks not tracked
	bool handedBack = false;
	int lastWritten = -1, origSprite = -1, origFrame = 0;
	uint16_t idBeforeUse = 0;
	bool inUse = false;
	int loggedLit = -1;
	int loggedNear = -1;           // roth_vrpick_debug: the wick a flame was last reported near
};

std::vector<Kind> g_kinds;
std::vector<Candle> g_candles;
std::map<uint32_t, int> g_byObject;     // (sector << 16 | index) -> candle
FLevelLocals *g_level = nullptr;
int g_forceWick = -1;                   // the wick a flame-at-a-wick use is fired for (CandlesTick)

int Bits(uint32_t m) { int n = 0; for (; m; m &= m - 1) n++; return n; }
constexpr double kPi = 3.14159265358979323846;

uint32_t Key(int s, int i) { return ((uint32_t)(s & 0xffff) << 16) | (uint32_t)(i & 0xffff); }

Candle *ForObject(int s, int i)
{
	auto it = g_byObject.find(Key(s, i));
	return it == g_byObject.end() ? nullptr : &g_candles[it->second];
}

Candle *ForActor(const AActor *mo)
{
	int s = -1, i = -1;
	if (mo == nullptr || !ObjectForActor(mo, s, i)) return nullptr;
	return ForObject(s, i);
}

uint16_t WordOf(const Object &o) { return (uint16_t)(o.textureIndex | (o.textureSource << 8)); }

DVector3 WickAt(const Candle &c, const AActor *mo, int k, double yawDeg)
{
	const WickFlame &f = g_kinds[c.kind].model.flames[k];
	const double yaw = yawDeg * (kPi / 180.0);
	const double across = (mo->renderflags & RF_XFLIP) ? -f.across : f.across;
	// Screen-right of a view looking along yaw: (sin, -cos); a face sprite is drawn across it.
	return DVector3(mo->X() + sin(yaw) * across, mo->Y() - cos(yaw) * across, mo->Z() + f.up);
}

double ViewYawDeg(FLevelLocals *level, AActor *pmo)
{
	if (r_viewpoint.ViewLevel == level) return r_viewpoint.Angles.Yaw.Degrees();
	return pmo != nullptr ? pmo->Angles.Yaw.Degrees() : 0.0;
}

void Override(Candle &c, AActor *mo, int spr)
{
	if (spr < 0) return;
	// Whatever the original put on the actor since we last wrote is the original's picture.
	if (c.lastWritten < 0 || mo->sprite != c.lastWritten) { c.origSprite = mo->sprite; c.origFrame = mo->frame; }
	mo->sprite = spr;
	mo->frame = 0;
	c.lastWritten = spr;
}

void HandBack(Candle &c, AActor *mo)
{
	if (mo != nullptr && c.lastWritten >= 0 && mo->sprite == c.lastWritten) { mo->sprite = c.origSprite; mo->frame = c.origFrame; }
	c.lastWritten = -1;
}

int RightItem()
{
	Inventory *inv = InterfaceInventory();
	if (inv == nullptr || inv->RightHand() < 0) return 0;
	return (int16_t)inv->Slot(inv->RightHand()).id;
}

const char *RoleWord(int role) { return role == VR_MAINHAND ? "main" : "off"; }

} // namespace

//==========================================================================
// Load
//==========================================================================

void CandlesPrepare(const Map &rm, TextureSet &levelArt, TextureSet *sharedArt, Log *log)
{
	g_kinds.clear();
	g_candles.clear();
	g_byObject.clear();
	g_level = nullptr;
	g_forceWick = -1;
	std::map<std::pair<uint16_t, uint16_t>, int> kindOf;   // (unlit, lit) word -> kind, -1 not a candle
	int composites = 0;
	for (const LightableObject &L : FindLightableObjects(rm))
	{
		auto key = std::make_pair(L.unlitWord, L.litWord);
		auto found = kindOf.find(key);
		if (found == kindOf.end())
		{
			int kindIndex = -1;
			int iu = 0, il = 0;
			const bool su = Pack::ResolveObjectArt((uint8_t)(L.unlitWord & 0xff), (uint8_t)(L.unlitWord >> 8), iu);
			const bool sl = Pack::ResolveObjectArt((uint8_t)(L.litWord & 0xff), (uint8_t)(L.litWord >> 8), il);
			TextureSet *tu = su ? sharedArt : &levelArt, *tl = sl ? sharedArt : &levelArt;
			const Pack *pu = tu != nullptr ? tu->PackData() : nullptr, *pl = tl != nullptr ? tl->PackData() : nullptr;
			char nm[96];
			snprintf(nm, sizeof nm, "%s[%d] -> %s[%d]", tu ? tu->Name().c_str() : "?", iu, tl ? tl->Name().c_str() : "?", il);
			if (pu != nullptr && pl != nullptr && pu->Entry(iu) != nullptr && pl->Entry(il) != nullptr)
			{
				Kind k;
				k.model = BuildWickModel(WickPictureFromImage(pu->ReadImage(iu, true, true), pu->ReadAnchor(iu)),
					WickPictureFromImage(pl->ReadImage(il, true, true), pl->ReadAnchor(il)));
				k.litWord = L.litWord;
				k.name = nm;
				if (k.model.ok)
				{
					const WickModel &M = k.model;
					k.maskSprite.assign((size_t)M.FullMask() + 1, -1);
					std::vector<std::vector<uint8_t>> frames(M.frames);
					for (uint32_t mask = 1; mask <= M.FullMask(); mask++)
					{
						for (int f = 0; f < M.frames; f++) ComposeWicks(M, mask, f, frames[f]);
						const FString texName = FStringf("ROTH_%s_W%05d_%05d_%02x", tl->Name().c_str(), il, iu, (unsigned)mask);
						FTextureID tex = tl->CompositeSprite(texName.GetChars(), il, frames, M.W, M.H, log);
						if (!tex.isValid()) continue;
						k.maskSprite[mask] = MakeObjectSprite(&tex, nullptr, 1, log);
						composites++;
					}
					kindIndex = (int)g_kinds.size();
					g_kinds.push_back(std::move(k));
				}
				else if (log) log->Line("  candles: %s is not a candle (%s)", nm, k.model.why.c_str());
			}
			found = kindOf.emplace(key, kindIndex).first;
		}
		if (found->second < 0) continue;
		Candle c;
		c.spec = L;
		c.kind = found->second;
		c.w.Reset(g_kinds[c.kind].model.Wicks());
		g_byObject[Key(L.sector, L.index)] = (int)g_candles.size();
		g_candles.push_back(c);
	}
	if (log != nullptr || !g_candles.empty())
	{
		Printf("[roth_candles] %d candle holder(s) of %d kind(s), %d per-wick picture(s) composed from the install's unlit and lit art\n",
			(int)g_candles.size(), (int)g_kinds.size(), composites);
		for (const Kind &k : g_kinds)
			Printf("[roth_candles]   %s: %d wick(s), %d changed texels (%d by column, %d by the nearest wick across)\n", k.name.c_str(),
				k.model.Wicks(), k.model.changed, k.model.inColumn, k.model.byNearest);
		for (const Candle &c : g_candles)
			Printf("[roth_candles]   object %d/%d (id %u, chain #%u%s): %s\n", c.spec.sector, c.spec.index, (unsigned)c.spec.clickId,
				(unsigned)c.spec.trigger, c.spec.item ? FStringf(", needs item %d", c.spec.item).GetChars() : ", no item", g_kinds[c.kind].name.c_str());
	}
}

//==========================================================================
// The use
//==========================================================================

void CandlesBeforeUse(AActor *target)
{
	Candle *c = ForActor(target);
	if (c == nullptr) return;
	const Object *o = RuntimeObject(c->spec.sector, c->spec.index);
	c->idBeforeUse = o != nullptr ? o->commandID : 0;
	c->inUse = true;
}

void CandlesAfterUse(AActor *target, int role, const DVector3 &tip, const DVector3 &palm, const DVector3 &hit, bool inReach, bool acted)
{
	Candle *c = ForActor(target);
	if (c == nullptr || !c->inUse) return;
	c->inUse = false;
	const Object *o = RuntimeObject(c->spec.sector, c->spec.index);
	if (o == nullptr || c->w.IsStarted()) return;
	// THE ORIGINAL STARTED LIGHTING IT: its chain re-keyed the candle (0x3a, right after the item test in every
	// retail candle chain). A refused use (no item, auto-selected) leaves the id as it was.
	if (o->commandID == c->idBeforeUse)
	{
		Printf("[roth_candles] object %d/%d: the %s hand's use did not start the lighting (id still %u, %s)\n", c->spec.sector, c->spec.index,
			RoleWord(role), (unsigned)o->commandID, acted ? "the chain acted" : "nothing happened");
		return;
	}
	const double upm = VRPickUnitsPerMetre();
	const double yaw = ViewYawDeg(target->Level, players[consoleplayer].mo);
	int k = g_forceWick;
	const char *why = "the wick the flame was at";
	if (k < 0)
	{
		double best = 1e30;
		for (int j = 0; j < c->w.Wicks(); j++)
		{
			const DVector3 W = WickAt(*c, target, j, yaw);
			const double d = std::min((tip - W).Length(), (palm - W).Length());
			if (d < best) { best = d; k = j; }
		}
		why = "the wick nearest the hand";
		if (!inReach || best > kFirstWickNear * upm)
		{
			best = 1e30;
			for (int j = 0; j < c->w.Wicks(); j++)
			{
				const double d = (hit - WickAt(*c, target, j, yaw)).Length();
				if (d < best) { best = d; k = j; }
			}
			why = "the wick nearest where the hand pointed";
		}
	}
	c->w.Started(k);
	c->startTic = target->Level->maptime;
	c->handedBack = false;
	Printf("[roth_candles] object %d/%d: the %s hand's use started the original's lighting (id %u -> %u); wick %d lit first (%s)\n",
		c->spec.sector, c->spec.index, RoleWord(role), (unsigned)c->idBeforeUse, (unsigned)o->commandID, k + 1, why);
}

int CandleOriginalSprite(const AActor *mo, int shown)
{
	const Candle *c = ForActor(mo);
	return (c != nullptr && c->lastWritten >= 0 && shown == c->lastWritten) ? c->origSprite : shown;
}

//==========================================================================
// Each tic
//==========================================================================

void CandlesTick(FLevelLocals *level)
{
	if (level == nullptr || g_candles.empty()) return;
	g_level = level;
	player_t *player = level->GetConsolePlayer();
	AActor *pmo = player != nullptr ? player->mo : nullptr;
	const double yaw = ViewYawDeg(level, pmo);
	const double reach = kWickReach * VRPickUnitsPerMetre();

	// The flames: each hand's fingertip and palm, while the hands are live.
	DVector3 tip[2], palm[2];
	bool have[2] = { false, false };
	for (int role = 0; role < 2; role++) have[role] = VRPickFlamePoints(role, tip[role], palm[role]);
	const int itemRole = VRHandPose_RoleForSide(VRHandPose::SIDE_RIGHT);
	const int rightItem = RightItem();

	for (Candle &c : g_candles)
	{
		const Object *o = RuntimeObject(c.spec.sector, c.spec.index);
		if (o == nullptr) continue;
		const Kind &kind = g_kinds[c.kind];
		const bool litNow = WordOf(*o) == kind.litWord;
		AActor *mo = FindObjectActor(c.spec.sector, c.spec.index);
		if (c.w.IsStarted()) c.w.Tick();

		// Which wicks a flame is at, and from which hand. An item candle lights from the hand holding the
		// original's right-hand item, only while it is the item the chain tests (the Matchbox); a candle whose
		// chain tests none, from any fingertip.
		uint32_t near = 0;
		int nearRole = -1;
		double nearDist = 0;
		int closest = -1, closestRole = -1;
		double closestDist = 1e30;
		const bool tracking = mo != nullptr && !c.handedBack && (c.w.IsStarted() || (!litNow && o->commandID == c.spec.clickId));
		if (tracking)
		{
			for (int role = 0; role < 2; role++)
			{
				if (!have[role]) continue;
				if (c.spec.item != 0 && (role != itemRole || rightItem != c.spec.item)) continue;
				for (int k = 0; k < c.w.Wicks(); k++)
				{
					const DVector3 W = WickAt(c, mo, k, yaw);
					double d = (tip[role] - W).Length();
					if (c.spec.item != 0) d = std::min(d, (palm[role] - W).Length());   // the box in the palm
					if (d <= reach) { near |= 1u << k; if (nearRole < 0) { nearRole = role; nearDist = d; } }
					if (d < closestDist) { closestDist = d; closest = k; closestRole = role; }
				}
			}
		}
		// roth_vrpick_debug: a flame within 10 cm of a wick, each time the nearest wick changes.
		{
			const int nowNear = closestDist <= 0.10 * VRPickUnitsPerMetre() ? closest : -1;
			if (roth_vrpick_debug && nowNear != c.loggedNear)
			{
				if (nowNear >= 0)
					Printf("[roth_candles] object %d/%d: the %s hand's flame is %.3f m (%.2f units) from wick %d (lights within %.3f m)\n", c.spec.sector,
						c.spec.index, RoleWord(closestRole), closestDist / VRPickUnitsPerMetre(), closestDist, nowNear + 1, kWickReach);
				else Printf("[roth_candles] object %d/%d: no flame within 0.10 m of a wick\n", c.spec.sector, c.spec.index);
			}
			c.loggedNear = nowNear;
		}
		const uint32_t before = c.w.Mask();
		const int ask = c.w.Flames(near);
		if (ask >= 0 && nearRole >= 0 && mo != nullptr)
		{
			Printf("[roth_candles] object %d/%d: the %s hand's flame at wick %d (%.2f m): the original's use\n", c.spec.sector, c.spec.index,
				RoleWord(nearRole), ask + 1, nearDist / VRPickUnitsPerMetre());
			g_forceWick = ask;
			VRPickUseObject(nearRole, mo, WickAt(c, mo, ask, yaw));
			g_forceWick = -1;
		}
		else if (c.w.IsStarted() && c.w.Mask() != before)
		{
			const uint32_t added = c.w.Mask() & ~before;
			for (int k = 0; k < c.w.Wicks(); k++)
				if (added & (1u << k))
					Printf("[roth_candles] object %d/%d: wick %d lit by the %s hand (%.2f m), %d of %d lit, tic %d of the lighting\n", c.spec.sector,
						c.spec.index, k + 1, RoleWord(nearRole), nearDist / VRPickUnitsPerMetre(), Bits(c.w.Mask()), c.w.Wicks(),
						c.w.TicsSinceStart());
		}
		if (c.w.Caught() && c.w.TicsSinceStart() == WickLighting::CATCH_TICS)
			Printf("[roth_candles] object %d/%d: 15 s after the first wick the others catch: all %d lit\n", c.spec.sector, c.spec.index, c.w.Wicks());

		// The picture.
		if (c.w.IsStarted() && !c.handedBack && mo != nullptr)
		{
			const bool late = c.w.TicsSinceStart() >= WickLighting::CATCH_TICS + kHandBackLate;
			if ((c.w.AllLit() && litNow) || late)
			{
				HandBack(c, mo);
				c.handedBack = true;
				Printf("[roth_candles] object %d/%d: every wick lit%s; the picture is the original's again (art 0x%04x%s)\n", c.spec.sector,
					c.spec.index, c.w.Caught() ? " (the catch)" : "", (unsigned)WordOf(*o), litNow ? ", the lit picture" : ", NOT the lit picture: handed back late");
			}
			else Override(c, mo, kind.maskSprite[c.w.Mask()]);
		}
		if (!c.w.IsStarted() && litNow && c.litSeenTic < 0) c.litSeenTic = level->maptime;   // lit by the flat click, all at once
	}
}

//==========================================================================
// THE TALL WICKS (owner, VR_CHAPTER01_SHADOWS.md 6 Q13: "point the lit match at a wick in reach + grip lights
// it", for seated and shorter players). The FLAME is whatever CandlesTick takes for one now (FlameHand: the hand
// holding the original's right-hand item when the candle's chain tests that item -- the Matchbox -- or any hand
// when it tests none); the real match is another lane's, and slots in there. The wick is the one nearest the
// pointing ray within kPointWick of it, in reach by the original's own use band (z < 268 along the ray's yaw,
// PickPlanarZ). Then the same route as a flame brought to it: an unlit candle -> the original's use with that wick
// as the first (VRPickUseObject, g_forceWick); a candle being lit -> that wick lights (WickLighting::Flames).
//==========================================================================

namespace
{
constexpr double kPointWick = 0.06;   // metres from the pointing ray to a wick

bool FlameHand(const Candle &c, int role, int itemRole, int rightItem)
{
	return c.spec.item == 0 || (role == itemRole && rightItem == c.spec.item);
}
}

bool CandlesPointGrip(int role, const DVector3 &tip, const DVector3 &dir)
{
	if (g_level == nullptr || g_candles.empty()) return false;
	AActor *pmo = players[consoleplayer].mo;
	if (pmo == nullptr) return false;
	const double yaw = ViewYawDeg(g_level, pmo);
	const double upm = VRPickUnitsPerMetre();
	const int itemRole = VRHandPose_RoleForSide(VRHandPose::SIDE_RIGHT);
	const int rightItem = RightItem();
	Candle *best = nullptr;
	AActor *bestMo = nullptr;
	int bestK = -1;
	double bestT = 1e30, bestPerp = 0, bestZ = 0;
	DVector3 bestW;
	int farK = -1;
	const Candle *farC = nullptr;
	double farZ = 0;
	for (Candle &c : g_candles)
	{
		const Object *o = RuntimeObject(c.spec.sector, c.spec.index);
		AActor *mo = FindObjectActor(c.spec.sector, c.spec.index);
		if (o == nullptr || mo == nullptr || c.handedBack) continue;
		const bool litNow = WordOf(*o) == g_kinds[c.kind].litWord;
		if (!(c.w.IsStarted() || (!litNow && o->commandID == c.spec.clickId))) continue;
		if (!FlameHand(c, role, itemRole, rightItem)) continue;
		for (int k = 0; k < c.w.Wicks(); k++)
		{
			if (c.w.IsStarted() && (c.w.Mask() & (1u << k))) continue;
			const DVector3 W = WickAt(c, mo, k, yaw);
			const DVector3 v = W - tip;
			const double t = v | dir;
			if (t <= 0) continue;
			const double perp = (v - dir * t).Length();
			if (perp > kPointWick * upm || t >= bestT) continue;
			// IN REACH: the original's use range, the reach every hand use has (CursorClass::CanUse: depth 2z + 64 < 0x258,
			// z the planar distance from the player along the ray's yaw, under 268 units)
			const double z = PickPlanarZ(pmo, W, dir);
			if (PickDepthOfZ(z) >= 0x258) { if (farC == nullptr) { farC = &c; farK = k; farZ = z; } continue; }
			best = &c; bestMo = mo; bestK = k; bestT = t; bestPerp = perp; bestW = W; bestZ = z;
		}
	}
	if (best == nullptr)
	{
		if (farC != nullptr)
			Printf("[roth_candles] object %d/%d: the %s hand points its flame at wick %d, out of reach (planar %.0f units, the use range is 268): nothing\n",
				farC->spec.sector, farC->spec.index, RoleWord(role), farK + 1, farZ);
		return false;
	}
	Candle &c = *best;
	Printf("[roth_candles] object %d/%d: the %s hand points its flame at wick %d (%.1f units along the ray, %.1f off it; in reach: planar %.0f"
		" of the use range's 268) and grips\n", c.spec.sector, c.spec.index, RoleWord(role), bestK + 1, bestT, bestPerp, bestZ);
	if (!c.w.IsStarted())
	{
		g_forceWick = bestK;
		VRPickUseObject(role, bestMo, bestW);
		g_forceWick = -1;
		return true;
	}
	const uint32_t before = c.w.Mask();
	c.w.Flames(1u << bestK);
	if (c.w.Mask() != before)
		Printf("[roth_candles] object %d/%d: wick %d lit by the %s hand's pointed flame, %d of %d lit, tic %d of the lighting\n", c.spec.sector,
			c.spec.index, bestK + 1, RoleWord(role), Bits(c.w.Mask()), c.w.Wicks(), c.w.TicsSinceStart());
	return true;
}

//==========================================================================
// Saves
//==========================================================================

void CandlesBeforeSave(FLevelLocals *level)
{
	if (level == nullptr || level != g_level) return;
	for (Candle &c : g_candles)
	{
		if (c.lastWritten < 0) continue;
		HandBack(c, FindObjectActor(c.spec.sector, c.spec.index));   // the next tic composes again
	}
}

void CandlesRestored(FLevelLocals *level)
{
	(void)level;
	for (Candle &c : g_candles)
	{
		c.w.Reset(g_kinds[c.kind].model.Wicks());
		c.startTic = c.litSeenTic = -1;
		c.handedBack = false;
		c.lastWritten = -1;
		c.inUse = false;
		c.loggedLit = -1;
	}
	if (!g_candles.empty()) Printf("[roth_candles] restored: no wick state survives a restore; each candle shows the original's picture\n");
}

//==========================================================================
// The light
//==========================================================================

void CandlesFrameLights(FLevelLocals *level, double ticFrac, double viewYawDeg)
{
	if (level == nullptr || level != g_level || g_candles.empty()) return;
	const double now = (level->maptime + ticFrac) / 35.0;
	for (Candle &c : g_candles)
	{
		AActor *mo = FindObjectActor(c.spec.sector, c.spec.index);
		if (mo == nullptr) continue;
		const Kind &kind = g_kinds[c.kind];
		double age[kMaxWicks];
		for (int k = 0; k < kMaxWicks; k++) age[k] = -1.0;
		if (c.w.IsStarted())
		{
			for (int k = 0; k < c.w.Wicks(); k++)
				if (c.w.LitAt(k) >= 0) age[k] = (level->maptime - c.startTic - c.w.LitAt(k) + ticFrac) / 35.0;
		}
		else if (c.litSeenTic >= 0)
		{
			for (int k = 0; k < c.w.Wicks(); k++) age[k] = (level->maptime - c.litSeenTic + ticFrac) / 35.0;
		}
		else continue;
		CandleLightOut L;
		if (!CandleLight(kind.model, age, now, L)) continue;
		const double yaw = viewYawDeg * (kPi / 180.0);
		const double across = (mo->renderflags & RF_XFLIP) ? -L.across : L.across;
		const DVector3 at(mo->X() + sin(yaw) * across, mo->Y() - cos(yaw) * across, mo->Z() + L.up);
		EffectLights::FrameLight f;
		f.A = { at.X, at.Y, at.Z };
		f.B = f.A;
		for (int ch = 0; ch < 3; ch++) f.Color[ch] = (float)(kLightColour[ch] * kLightPerWick * L.intensity);
		f.Radius = L.radius;
		f.Flags = 0;
		EffectLights::Get().AddFrameLight(f);
		if (L.lit != c.loggedLit)
		{
			Printf("[roth_candles] object %d/%d: its light now %d wick(s): radius %.0f units, colour %.2f %.2f %.2f, at (%.0f %.0f %.0f)\n", c.spec.sector,
				c.spec.index, L.lit, L.radius, f.Color[0], f.Color[1], f.Color[2], at.X, at.Y, at.Z);
			c.loggedLit = L.lit;
		}
	}
}

//==========================================================================
// The flat test's placements
//==========================================================================

bool CandleWickWorld(int sector, int index, int wick, double viewYawDeg, DVector3 &out)
{
	const Candle *c = ForObject(sector, index);
	AActor *mo = FindObjectActor(sector, index);
	if (c == nullptr || mo == nullptr || wick < 0 || wick >= c->w.Wicks()) return false;
	out = WickAt(*c, mo, wick, viewYawDeg);
	return true;
}

bool CandleExtent(int sector, int index, DVector3 &centre, double &height, int &wicks)
{
	const Candle *c = ForObject(sector, index);
	AActor *mo = FindObjectActor(sector, index);
	if (c == nullptr || mo == nullptr) return false;
	const WickModel &M = g_kinds[c->kind].model;
	height = M.H * M.upp;
	centre = DVector3(mo->X(), mo->Y(), mo->Z() + height * 0.5);
	wicks = M.Wicks();
	return true;
}

} // namespace roth
