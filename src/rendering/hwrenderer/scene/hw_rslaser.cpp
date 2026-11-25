/*
** hw_rslaser.cpp
**
** VREALMS COPY (2026-11-16) of RemaSelaco 383ef89c src/rendering/hwrenderer/scene/hw_rslaser.cpp, for RSQ Distance
** Grab's engine laser (its laser.zs asks for "RSLaserService"; 9eb2766d). Copied whole; ONE change: the six rs_laser*
** cvars are not archived (flags 0, were CVAR_ARCHIVE | CVAR_GLOBALCONFIG) -- VRealms shares the owner's doomxr.ini
** with his hands build and never writes archived settings into it (CLAUDE.md). Drawn from HWDrawInfo::DrawScene
** beside UZDXREMA's own laser sight (hw_drawinfo.cpp, DM_MAINVIEW). Everything below is RemaSelaco's text.
**
** RS -- THE RS LASER: UZDXREMA's laser sight, ported to RemaSelaco.
**
** The owner: "UZDXREMA has a much nicer laser that i want, for the laser sight and
** our distancegrab". This is that laser -- UZDXREMA's DrawLaserBeamGeometry
** (src/rendering/hwrenderer/scene/hw_weapon.cpp there) -- moved over with its
** look intact, and opened up so script can ask for it in two shapes:
**
**   a SIGHT  per hand: a beam out of a point on the gun (given in the gun's own
**            frame, so it is exactly where the gun's mesh is, every frame), down
**            the barrel, traced every frame, a dot where it lands, a lock when
**            the dot is on something alive.            RSLaser.Sight()
**   a BEAM   per slot: a line between two points, optionally riding a hand at
**            frame rate (its start) and an actor (its end), optionally clipped
**            by the world every frame, with the same core, halo and dot.
**                                                       RSLaser.Beam()
**
** THE LOOK, as UZDXREMA draws it (see the comments kept with each part below):
** a 16-sided CORE inside two soft HALO tubes, the halo tapered toward the muzzle,
** the whole beam faded along its length by distance, the core pushed past white so
** an HDR scene's bloom picks it up on its own; a DOT that holds one apparent size
** at any distance, with three glow discs round it; and the LOCK -- on something
** alive the dot tightens, brightens and breathes while its glow swells outward.
**
** WHAT IS NOT PORTED, and why (all of it UZDXREMA-only machinery this fork does
** not have): the darkness-curve exemption (no rs_darkness here), the wheel and
** script-cursor gates, per-slot / per-hand / cycling colour tiers and the headshot
** tint (Selaco is played one way: the caller says the colour), and the per-pixel
** SetBeam "air glow" volume (a main.fp + viewpoint-uniform system that is Vulkan
** first there). The grab beam's look from that system -- energy scrolling along the
** line and a flare at its bright end -- is drawn here with the same geometry as the
** sight (RSL_SCROLL, RSL_FLARE), which works on GL, GLES and Vulkan alike.
**
** Renderer-agnostic: only FRenderState and the shared vertex buffer, like every
** other hwrenderer scene draw. Called once per eye from HWDrawInfo::DrawScene.
**
** PRESENTATION ONLY. Nothing here is read by the playsim, nothing is saved, and every
** request goes out on its own two tics after the script stops making it -- a script
** that dies mid-level cannot leave a laser welded on.
*/

#include <algorithm>
#include <cmath>
#include "hw_drawinfo.h"
#include "hw_renderstate.h"
#include "hw_vrmodes.h"
#include "flatvertices.h"
#include "v_video.h"
#include "r_utility.h"
#include "d_player.h"
#include "doomstat.h"
#include "g_levellocals.h"
#include "p_trace.h"
#include "actor.h"
#include "actorinlines.h"
#include "menustate.h"
#include "c_cvars.h"
#include "vm.h"
#include "dobjgc.h"

// ---------------------------------------------------------------------------------------
// The few knobs. UZDXREMA's vr_laser_* defaults, renamed into this fork's rs_ space. No
// menu: Selaco's own pack decides when and in what colour (RS_Selaco_VRWeapons).
// ---------------------------------------------------------------------------------------
CVARD(Bool, rs_laser, true, 0, "RS laser: draw script-requested laser sights and beams")
// vr_laser_beam_width: the core's radius is a quarter of this, in map units.
CVARD(Float, rs_laser_width, 0.15f, 0, "RS laser: sight beam width")
// vr_laser_beam_fade: how far the SIGHT's beam stays visible before it has run out (map
// units). The owner, for UZDXREMA: "a laser sight that can be super short and taper off so
// i can rely on the reactive dot". 0 = no fade, the whole length.
CVARD(Float, rs_laser_fade, 100.f, 0, "RS laser: sight beam fade length (map units, 0 = none)")
// vr_laser_pointer_scale
CVARD(Float, rs_laser_dot_size, 0.1f, 0, "RS laser: dot size")
// vr_laser_beam_glow: halo strength. 0 = the bare core.
CVARD(Float, rs_laser_glow, 1.f, 0, "RS laser: halo strength")
// vr_laser_beam_emissive: the core's colour is multiplied past 1.0 so bloom picks it up on an
// HDR scene target. 1 = no overbright.
CVARD(Float, rs_laser_emissive, 1.8f, 0, "RS laser: core overbright for bloom")

// UZDXREMA's remaining shaping defaults, fixed here (the owner's look, not a menu).
static constexpr float kBeamAlpha      = 0.30f;   // vr_laser_beam_alpha
static constexpr float kBeamTaper      = 0.45f;   // vr_laser_beam_taper
static constexpr float kDotAlpha       = 0.90f;   // vr_laser_pointer_alpha
static constexpr float kDotGlowScale   = 1.50f;   // vr_laser_pointer_glow_scale
static constexpr float kDotRange       = 320.f;   // vr_laser_dot_range
static constexpr float kLockTighten    = 0.40f;   // vr_laser_lock_tighten
static constexpr float kLockRate       = 0.42f;   // vr_laser_lock_rate (radians per tic, ~2.3 Hz)
static constexpr double kSightRange    = 8192.0;
static constexpr int   kExpireTics     = 2;       // a request lives this many tics past its last call

enum ERSLaserFlags
{
	RSL_NODOT   = 1,      // Sight: beam only
	RSL_NOBEAM  = 2,      // Sight: dot only
	RSL_NOLOCK  = 4,      // Sight: no lock reaction on live monsters
	RSL_TRACE   = 8,      // Beam: clip against walls every frame along start -> end
	RSL_DOT     = 16,     // Beam: a dot at its far end (the end that is not the hand)
	RSL_LOCK    = 32,     // Beam: the dot locks (tightens, brightens, breathes)
	RSL_REVERSE = 64,     // Beam: the energy flows toward the start and the flare sits there
	RSL_SCROLL  = 128,    // Beam: energy beads travel along it
	RSL_FLARE   = 256,    // Beam: a soft flare where it ends (as drawn)
};

static constexpr int kMaxBeams = 32;

struct FRSLaserSight
{
	int Tic = -1000;
	FVector3 Muzzle;      // the gun's frame: F forward, L left, U up, cm (RS_Selaco_VRWeapons' measuring frame)
	FVector3 Dir;         // the same frame, unit
	PalEntry Color;
	float Alpha = 1.f;
	int Flags = 0;
	// RemaSelaco 2026-11-15: HIT CONFIRMATION (RSLaser.Flash). The dot takes this colour and throws a burst ring
	// for Len tics from Start (maptime), Grow times its size at the ring's widest. Len 0: none.
	PalEntry FlashColor;
	double FlashStart = -1000;
	float FlashLen = 0.f, FlashGrow = 0.f;
};

struct FRSLaserBeam
{
	int Tic = -1000;
	DVector3 Start, End;              // map space, as set
	int Hand = -1;                    // start rides this hand (VR_MAINHAND / VR_OFFHAND), -1 none
	bool Anchored = false;
	DVector3 LocalStart, LocalEnd;    // in the hand's frame when anchored
	TObjPtr<AActor*> EndActor;        // end rides this actor (its interpolated position), or null
	DVector3 EndOfs;
	PalEntry Color;
	float Alpha = 1.f, Thick = 1.f;
	int Flags = 0;
};

static FRSLaserSight Sights[2];
static FRSLaserBeam Beams[kMaxBeams];

// Whether a script beam rides `hand` now (requested within its expiry): RSQ Distance Grab's cone beam on that hand.
// VRealms' own pointer (roth_vrpick.cpp THE BEAM) stands down while it shows, so a hand never has two lasers.
bool RSLaser_HandBeamActive(int hand)
{
	for (const FRSLaserBeam &b : Beams)
		if (b.Hand == hand && gametic - b.Tic <= kExpireTics) return true;
	return false;
}

// The hand as last drawn: what a beam's points are carried into the hand's frame with.
struct FRSHandFrame
{
	DVector3 T;           // origin, map space
	DVector3 A[3];        // the frame's axes in map space (columns), scale and pixel stretch included
	bool Valid = false;
};
static FRSHandFrame LastHand[2];

static void MarkLaserActors()
{
	for (auto& b : Beams) GC::Mark(b.EndActor);
}

// ---------------------------------------------------------------------------------------
// The hand frame: VRMode::GetWeaponTransform, the very matrix the gun's model is drawn with
// (r_data/models.cpp RenderHUDModel), read into map space (render space is x, z, y).
// ---------------------------------------------------------------------------------------
static bool GetHandFrame(int hand, FRSHandFrame& f)
{
	auto vrmode = VRMode::GetVRMode(true);
	VSMatrix m;
	if (vrmode == nullptr || !vrmode->IsVR() || !vrmode->GetWeaponTransform(&m, hand))
	{
		f.Valid = false;
		return false;
	}
	const FLOATTYPE* M = m.get();
	for (int j = 0; j < 3; j++) f.A[j] = DVector3(M[j * 4 + 0], M[j * 4 + 2], M[j * 4 + 1]);
	f.T = DVector3(M[12], M[14], M[13]);
	f.Valid = true;
	return true;
}

static DVector3 FrameApply(const FRSHandFrame& f, const DVector3& l)
{
	return f.T + f.A[0] * l.X + f.A[1] * l.Y + f.A[2] * l.Z;
}

static DVector3 FrameApplyDir(const FRSHandFrame& f, const DVector3& l)
{
	return f.A[0] * l.X + f.A[1] * l.Y + f.A[2] * l.Z;
}

// v (map space, relative to the frame's origin) -> the frame's coordinates. The axes are
// not orthonormal (scale, pixel stretch, a mirrored left hand), so a real 3x3 inverse.
static bool FrameInverse(const FRSHandFrame& f, const DVector3& v, DVector3& out)
{
	const DVector3 &a = f.A[0], &b = f.A[1], &c = f.A[2];
	const DVector3 bc = b ^ c, ca = c ^ a, ab = a ^ b;
	const double det = a | bc;
	if (fabs(det) < 1e-12) return false;
	out = DVector3(v | bc, v | ca, v | ab) / det;
	return true;
}

// The gun frame of RS_Selaco_VRWeapons (F forward, L left, U up, cm) -> the hand matrix's
// own units: RenderHUDModel scales the hand by 0.01 before anything of the model's, and its
// axes are x = -L, y = U, z = -F.
static DVector3 FLUToLocal(const FVector3& flu)
{
	return DVector3(-flu.Y, flu.Z, -flu.X) * 0.01;
}

static double PixelStretch()
{
	return (primaryLevel && primaryLevel->info) ? primaryLevel->info->pixelstretch : 1.2;
}

// ---------------------------------------------------------------------------------------
// THE DRAWING -- UZDXREMA's DrawLaserBeamGeometry, kept as it was with its reasons, and
// widened only where a grab beam needs it (scroll, flare, reverse, per-call thickness).
// ---------------------------------------------------------------------------------------
struct FRSLaserLook
{
	PalEntry BeamColor, DotColor;
	float BeamAlpha = kBeamAlpha;     // the core's authored alpha
	float CoreRadius = 0.0375f;       // map units
	float Fade = 0.f;                 // 0 = whole length
	bool DrawBeam = true, DrawDot = true, Lock = false, Scroll = false, Flare = false;
	bool FlowToStart = false;
	float DotAlpha = 1.f;             // multiplies the dot's alpha
	float DotScale = 1.f;             // multiplies the dot's size         // RSL_REVERSE: the energy flows toward the start and the flare sits there
	bool DotOnSurface = false;        // the dot lies on a wall/floor (DotNormal) instead of facing the eye
	bool DotRangeLimited = true;      // past kDotRange the dot only shows on a target (the sight)
	DVector3 DotNormal;
	// RemaSelaco 2026-11-15: a hit's flash (RSLaser.Flash), FlashT 0..1 through it, < 0 none
	float FlashT = -1.f, FlashGrow = 0.f;
	PalEntry FlashColor;
};

static void SetupState(FRenderState& state)
{
	state.EnableModelMatrix(false);
	state.SetLightIndex(-1);
	state.AlphaFunc(Alpha_Greater, 0.0f);
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
	state.EnableBrightmap(false);
	state.EnableTexture(false);
	state.EnableDepthTest(true);
	state.SetDepthMask(false);
}

static void RestoreState(FRenderState& state)
{
	state.EnableTexture(true);
	state.SetDepthMask(true);
	state.SetRenderStyle(DefaultRenderStyle());
	state.SetTextureMode(TM_NORMAL);
	state.SetColor(1.f, 1.f, 1.f, 1.f);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	state.EnableBrightmap(false);
	state.EnableModelMatrix(false);
	state.ResetColor();
	state.AlphaFunc(Alpha_GEqual, 0.f);
}

// One disc (a triangle fan) round `centre`, in the plane of `right`/`up`.
static void DrawDisc(FRenderState& state, const DVector3& centre, const DVector3& right, const DVector3& up, double radius, double vsquash)
{
	constexpr int seg = 16;
	constexpr int count = seg + 2;
	screen->mVertexData->Map();
	auto v = screen->mVertexData->AllocVertices(count);
	auto vp = v.first;
	vp[0].Set((float)centre.X, (float)centre.Z, (float)centre.Y, 0.5f, 0.5f);
	for (int i = 0; i <= seg; ++i)
	{
		const double ang = (double)i / seg * 6.28318530717958647692;
		DVector3 o = (right * cos(ang) + up * sin(ang)) * radius;
		o.Z *= vsquash;   // a round disc on screen: the renderer stretches map height by the level's pixel ratio
		const DVector3 p = centre + o;
		vp[i + 1].Set((float)p.X, (float)p.Z, (float)p.Y, 0.f, 0.f);
	}
	screen->mVertexData->Unmap();
	state.Draw(DT_TriangleFan, v.second, count, true);
}

static void DrawLaserGeometry(HWDrawInfo* di, FRenderState& state, const DVector3& beamStart, const DVector3& beamEndIn,
	const DVector3& dotPos, bool onTarget, const FRSLaserLook& look)
{
	bool drawBeam = look.DrawBeam;
	bool drawPointer = look.DrawDot;
	if (!drawBeam && !drawPointer) return;

	const auto& vp = di->Viewpoint;
	const double vsquash = 1.0 / PixelStretch();

	// ---- THE DOT GOES QUIET AT RANGE, UNLESS IT HAS FOUND SOMETHING ----
	// (UZDXREMA) Close up the dot is useful on anything; at distance it is a bright speck
	// riding over every far surface. Past the range it only draws on something it locked,
	// faded across the last quarter rather than cut, so panning never pops it.
	float dotVisibility = 1.0f;
	if (drawPointer && !onTarget && look.DotRangeLimited)
	{
		const double dotDist = (dotPos - beamStart).Length();
		const float fadeStart = kDotRange * 0.75f;
		if (dotDist >= kDotRange) dotVisibility = 0.0f;
		else if (dotDist > fadeStart) dotVisibility = 1.0f - (float)((dotDist - fadeStart) / (kDotRange - fadeStart));
	}
	if (dotVisibility <= 0.002f) drawPointer = false;

	// The dot is held a hair off what it lands on: flat on a surface, 0.4 unit out along its
	// normal; facing the eye (on a body, in the air), 4 units back toward the muzzle (UZDXREMA).
	DVector3 pointerCenter = dotPos;
	if (drawPointer)
	{
		if (look.DotOnSurface) pointerCenter += look.DotNormal * 0.4;
		else
		{
			DVector3 back = dotPos - beamStart;
			if (back.LengthSquared() > 1e-8) { back.MakeUnit(); pointerCenter -= back * std::min(4.0, (dotPos - beamStart).Length() * 0.5); }
		}
	}

	// ---- THE LOCK ----
	// (UZDXREMA) Off a target it is 0 and nothing changes. On one it runs 0..1 on a ~2.3 Hz
	// breath and drives three things at once: the dot TIGHTENS, BRIGHTENS, and its glow SWELLS.
	const bool lockActive = onTarget && look.Lock;
	const double lockTime = (primaryLevel ? (double)primaryLevel->maptime : 0.0) + vp.TicFrac;
	const float lockPulse = lockActive ? (float)(0.5 + 0.5 * sin(lockTime * kLockRate)) : 0.0f;
	const float lock = lockActive ? 1.0f : 0.0f;

	SetupState(state);

	const PalEntry beamColor = look.BeamColor;
	PalEntry dotColor = look.DotColor;
	// RemaSelaco: a hit's flash takes the dot over at once and hands it back as it fades
	const bool flashing = look.FlashT >= 0.f && look.FlashT < 1.f;
	const float flashFade = flashing ? (1.f - look.FlashT) : 0.f;
	if (flashing)
	{
		const float k = std::min(1.f, flashFade * 1.6f);
		dotColor = PalEntry(255, (uint8_t)(dotColor.r + (look.FlashColor.r - dotColor.r) * k),
			(uint8_t)(dotColor.g + (look.FlashColor.g - dotColor.g) * k), (uint8_t)(dotColor.b + (look.FlashColor.b - dotColor.b) * k));
	}
	const float emissive = std::max(1.0f, (float)rs_laser_emissive);
	const float beamAlpha = std::clamp(look.BeamAlpha, 0.0f, 1.0f);

	if (drawBeam && beamAlpha > 0.0f)
	{
		const DVector3 beamTarget = (drawPointer && !look.DotOnSurface && (dotPos - beamEndIn).LengthSquared() < 1.0) ? pointerCenter : beamEndIn;
		DVector3 beamVec = beamTarget - beamStart;
		const double beamLength = beamVec.Length();
		if (beamLength > 0.01)
		{
			beamVec.MakeUnit();
			DVector3 beamRight, beamUp;
			beamVec.GetRightUp(beamRight, beamUp);

			// SIXTEEN SEGMENTS, NOT EIGHT (UZDXREMA): eight is an octagon at arm's length.
			constexpr int beamSegments = 16;
			const int vertexCount = (beamSegments + 1) * 2;

			// CORE AND HALO (UZDXREMA): a hard narrow core inside a wide soft halo. Either alone
			// reads as a drawn line or a smear; together they read as incandescent. The halo is
			// scaled by rs_laser_glow; at 0 only the core draws.
			const float halo = std::max(0.0f, (float)rs_laser_glow);
			struct BeamPass { float radius; float alpha; };
			const BeamPass passes[] = {
				{ 1.00f, 1.00f },
				{ 2.60f, 0.30f * halo },
				{ 5.20f, 0.11f * halo },
			};

			// TAPERED: the halo only, tighter at the aperture and blooming toward what it lands
			// on. The core stays straight -- it is what you aim with.
			const float kTaperStart = kBeamTaper;

			// FADED ALONG ITS LENGTH BY DISTANCE, not by the fraction drawn: a beam that ends
			// early on a near wall arrives there at full strength.
			const double fadeLen = look.Fade;
			const bool fading = fadeLen > 1.0;
			const double drawLen = fading ? std::min(beamLength, fadeLen) : beamLength;

			// Steps along the length: 8 for a plain beam (UZDXREMA); a scrolling beam needs enough
			// of them to carry its beads (one step per 6 units, up to 64).
			const int lengthSteps = look.Scroll ? std::clamp((int)(drawLen / 6.0), 8, 64) : 8;

			// THE SCROLL (UZDXREMA's grab beam, SetBeamLook scroll 6 / depth 0.25): beads of energy
			// travelling along the line, start to end as drawn, so a held beam keeps reading as
			// carrying something instead of going static.
			const double scrollPhase = (primaryLevel ? (double)primaryLevel->maptime + vp.TicFrac : 0.0) / TICRATE * 6.0
				* (look.FlowToStart ? -1.0 : 1.0);
			constexpr double kScrollWave = 24.0;   // map units bead to bead
			constexpr float kScrollDepth = 0.25f;

			for (const BeamPass& pass : passes)
			{
				const float passAlpha = std::clamp(beamAlpha * pass.alpha, 0.0f, 1.0f);
				if (passAlpha <= 0.002f) continue;
				const bool isCore = (pass.radius <= 1.0f);

				// Only an opaque, unfading, unscrolling core draws solid; everything else adds.
				const bool solidCore = isCore && beamAlpha >= 0.999f && !fading && !look.Scroll;
				state.SetRenderStyle(solidCore ? STYLE_Source : STYLE_Add);

				// OVERBRIGHT THE CORE PAST WHITE so bloom picks it up (core only: an overbright
				// halo hazes the view instead of looking hot).
				const float em = isCore ? emissive : 1.0f;
				const float rEnd = look.CoreRadius * pass.radius;
				const float rStart = isCore ? rEnd : rEnd * kTaperStart;

				for (int s = 0; s < lengthSteps; ++s)
				{
					const double t0 = (double)s / lengthSteps;
					const double t1 = (double)(s + 1) / lengthSteps;
					float stepAlpha = passAlpha;
					if (fading)
					{
						const double mid = drawLen * 0.5 * (t0 + t1);
						stepAlpha *= (float)std::clamp(1.0 - mid / fadeLen, 0.0, 1.0);
					}
					if (look.Scroll)
					{
						const double mid = drawLen * 0.5 * (t0 + t1);
						const double w = 0.5 + 0.5 * sin((mid / kScrollWave - scrollPhase) * 6.28318530717958647692);
						stepAlpha *= (1.0f - kScrollDepth) + kScrollDepth * 2.0f * (float)w;
					}
					if (stepAlpha <= 0.002f) continue;

					state.SetColor(beamColor.r / 255.0f * em, beamColor.g / 255.0f * em, beamColor.b / 255.0f * em, std::min(stepAlpha, 1.0f));

					const double r0 = rStart + (rEnd - rStart) * t0;
					const double r1 = rStart + (rEnd - rStart) * t1;
					const DVector3 p0 = beamStart + beamVec * (drawLen * t0);
					const DVector3 p1 = beamStart + beamVec * (drawLen * t1);

					screen->mVertexData->Map();
					auto verts = screen->mVertexData->AllocVertices(vertexCount);
					auto vp2 = verts.first;
					for (int i = 0; i <= beamSegments; ++i)
					{
						const double ang = (double)i / beamSegments * 6.28318530717958647692;
						const DVector3 dir = beamRight * cos(ang) + beamUp * sin(ang);
						const DVector3 a = p0 + dir * r0;
						const DVector3 b = p1 + dir * r1;
						vp2[i * 2 + 0].Set((float)a.X, (float)a.Z, (float)a.Y, 0.0f, 0.0f);
						vp2[i * 2 + 1].Set((float)b.X, (float)b.Z, (float)b.Y, 0.0f, 1.0f);
					}
					screen->mVertexData->Unmap();
					state.Draw(DT_TriangleStrip, verts.second, vertexCount, true);
				}
			}

			// THE FLARE (UZDXREMA's grab beam, flare 1.5): the bright end of the line blooms --
			// three soft discs facing the eye where the energy arrives. Kept small: on a reeled
			// grab beam that end is the palm, a forearm from the eye.
			if (look.Flare)
			{
				const DVector3 at = look.FlowToStart ? beamStart : beamStart + beamVec * drawLen;
				DVector3 toEye = vp.Pos - at;
				if (toEye.LengthSquared() > 1e-6)
				{
					toEye.MakeUnit();
					DVector3 fr, fu;
					toEye.GetRightUp(fr, fu);
					state.SetRenderStyle(STYLE_Add);
					const double fbase = std::max(0.3, (double)look.CoreRadius * 2.5);
					const float fa[] = { 0.35f, 0.18f, 0.08f };
					for (int pass = 0; pass < 3; ++pass)
					{
						state.SetColor(beamColor.r / 255.0f, beamColor.g / 255.0f, beamColor.b / 255.0f, beamAlpha * fa[pass]);
						DrawDisc(state, at, fr, fu, fbase * (1.0 + pass * 0.8), vsquash);
					}
				}
			}
		}
	}

	if (drawPointer)
	{
		// The disc's plane: the surface it lands on, or facing the eye.
		DVector3 right, up, nrm;
		if (look.DotOnSurface) nrm = look.DotNormal;
		else
		{
			nrm = vp.Pos - pointerCenter;
			if (nrm.LengthSquared() < 1e-8) nrm = DVector3(0, 0, 1);
			nrm.MakeUnit();
		}
		nrm.GetRightUp(right, up);

		// ONE APPARENT SIZE at any distance (UZDXREMA): radius grows with distance and FOV.
		// TIGHTENS ON A TARGET, the breath on top so it never settles.
		const double pointerDistance = (dotPos - vp.Pos).Length();
		const double fovScale = tan(vp.GetFieldOfView().Radians() * 0.5);
		const double pointerScale = std::max(0.02, (double)rs_laser_dot_size) * look.DotScale;
		const float lockScale = 1.0f - lock * kLockTighten * (0.65f + 0.35f * lockPulse);
		const double pointerRadius = std::max(0.006, pointerDistance * fovScale * 0.01 * pointerScale * lockScale);

		// BRIGHTENS WITH IT, so the smaller dot is not also a fainter one; and BURNS -- the
		// dot is what you look at, so it is pushed past white like the core.
		const float pointerAlpha = std::clamp(kDotAlpha * look.DotAlpha * dotVisibility * (1.0f + lock * (0.35f + 0.30f * lockPulse)), 0.0f, 1.0f);
		const float dotEm = emissive * (1.0f + lock * 0.5f * lockPulse);
		state.SetColor(dotColor.r / 255.0f * dotEm, dotColor.g / 255.0f * dotEm, dotColor.b / 255.0f * dotEm, pointerAlpha);
		state.SetRenderStyle(pointerAlpha >= 0.999f ? STYLE_Source : STYLE_Add);
		DrawDisc(state, pointerCenter, right, up, pointerRadius, vsquash);

		// AND THE GLOW SWELLS: the dot draws in while the halo round it pushes out --
		// opposite directions, which is what makes the lock read as pressure.
		const float glowScale = kDotGlowScale * (1.0f + lock * (0.45f + 0.55f * lockPulse));
		const float glowIntensity = 1.0f + lock * 0.8f * lockPulse;
		state.SetRenderStyle(STYLE_Add);
		for (int pass = 0; pass < 3; ++pass)
		{
			const float passScale = glowScale * (1.0f + pass * 0.5f);
			const float passAlpha = pointerAlpha * glowIntensity * (pass == 0 ? 0.45f : pass == 1 ? 0.22f : 0.10f);
			state.SetColor(dotColor.r / 255.0f, dotColor.g / 255.0f, dotColor.b / 255.0f, std::min(passAlpha, 1.0f));
			DrawDisc(state, pointerCenter, right, up, pointerRadius * passScale, vsquash);
		}

		// RemaSelaco 2026-11-15: HIT CONFIRMATION. The flash's burst: a soft disc in the flash's colour that throws out
		// from the dot to FlashGrow times its size and fades on the way (ease-out), two layers so it has a bright edge
		// over a fainter fill. Drawn where the dot is, in both eyes, at the dot's own apparent size: the eye is already there.
		if (flashing)
		{
			const float e = 1.f - (1.f - look.FlashT) * (1.f - look.FlashT);
			const double r = pointerRadius * (1.0 + look.FlashGrow * e);
			const float a = std::min(1.f, flashFade * flashFade * 0.9f) * dotVisibility;
			const PalEntry fc = look.FlashColor;
			state.SetRenderStyle(STYLE_Add);
			state.SetColor(fc.r / 255.0f * emissive, fc.g / 255.0f * emissive, fc.b / 255.0f * emissive, a * 0.35f);
			DrawDisc(state, pointerCenter, right, up, r, vsquash);
			state.SetColor(fc.r / 255.0f, fc.g / 255.0f, fc.b / 255.0f, a * 0.5f);
			DrawDisc(state, pointerCenter, right, up, r * 0.6, vsquash);
		}
	}

	RestoreState(state);
}

// The normal of what a trace hit, turned toward `from`. False for an actor, the sky, nothing.
static bool SurfaceNormal(const FTraceResults& res, const DVector3& from, DVector3& n)
{
	if (res.HitType == TRACE_HitWall && res.Line != nullptr)
	{
		const DVector2 d = res.Line->Delta();
		n = DVector3(d.Y, -d.X, 0);
	}
	else if (res.HitType == TRACE_HitFloor && res.Sector != nullptr)
	{
		n = res.ffloor ? DVector3(0, 0, 1) : res.Sector->floorplane.Normal();
	}
	else if (res.HitType == TRACE_HitCeiling && res.Sector != nullptr)
	{
		n = res.ffloor ? DVector3(0, 0, -1) : res.Sector->ceilingplane.Normal();
	}
	else return false;
	if (n.LengthSquared() < 1e-12) return false;
	n.MakeUnit();
	if ((n | (from - res.HitPos)) < 0) n = -n;
	return true;
}

static bool IsLockTarget(AActor* a, AActor* self)
{
	return a != nullptr && a != self && (a->flags3 & MF3_ISMONSTER) && (a->flags & MF_SHOOTABLE)
		&& !(a->flags & MF_CORPSE) && !(a->flags & MF_FRIENDLY) && a->health > 0;
}

// ---------------------------------------------------------------------------------------
// Per eye, after the translucent pass of the main view (HWDrawInfo::DrawScene).
// ---------------------------------------------------------------------------------------
void RSLaser_Draw(HWDrawInfo* di, FRenderState& state)
{
	static bool marked = false;
	if (!marked) { GC::AddMarkerFunc(MarkLaserActors); marked = true; }

	// The hand as drawn this frame, kept for the beams' anchoring (RSLaser.Beam's hand).
	FRSHandFrame hands[2];
	GetHandFrame(VR_MAINHAND, hands[0]);
	GetHandFrame(VR_OFFHAND, hands[1]);
	LastHand[0] = hands[0];
	LastHand[1] = hands[1];

	if (!rs_laser || menuactive != MENU_Off || primaryLevel == nullptr) return;
	player_t* player = &players[consoleplayer];
	if (player->mo == nullptr || di->Viewpoint.camera != player->mo) return;

	// ---- the sights ----
	for (int h = 0; h < 2; h++)
	{
		const FRSLaserSight& s = Sights[h];
		if (gametic - s.Tic > kExpireTics || !hands[h].Valid) continue;

		const DVector3 start = FrameApply(hands[h], FLUToLocal(s.Muzzle));
		DVector3 dir = FrameApplyDir(hands[h], FLUToLocal(s.Dir));
		if (dir.LengthSquared() < 1e-12) continue;
		dir.MakeUnit();

		FTraceResults res{};
		sector_t* sec = primaryLevel->PointInSector(start.XY());
		const bool hit = Trace(start, sec, dir, kSightRange, MF_SHOOTABLE, ML_BLOCKEVERYTHING | ML_BLOCKHITSCAN,
			player->mo, res, TRACE_NoSky);
		const DVector3 hitEnd = hit ? res.HitPos : start + dir * kSightRange;
		const bool onTarget = hit && res.HitType == TRACE_HitActor && !(s.Flags & RSL_NOLOCK) && IsLockTarget(res.Actor, player->mo);

		FRSLaserLook look;
		look.BeamColor = look.DotColor = s.Color;
		look.BeamAlpha = kBeamAlpha * s.Alpha;
		look.CoreRadius = 0.25f * std::max(0.05f, (float)rs_laser_width);
		look.Fade = std::max(0.0f, (float)rs_laser_fade);
		look.DrawBeam = !(s.Flags & RSL_NOBEAM);
		look.DrawDot = !(s.Flags & RSL_NODOT) && hit;
		look.Lock = true;
		look.DotAlpha = s.Alpha;
		look.DotOnSurface = SurfaceNormal(res, start, look.DotNormal);
		if (s.FlashLen > 0.f)
		{
			const double now = (double)primaryLevel->maptime + di->Viewpoint.TicFrac;
			const float t = (float)((now - s.FlashStart) / s.FlashLen);
			if (t >= 0.f && t < 1.f)
			{
				look.FlashT = t;
				look.FlashGrow = s.FlashGrow;
				look.FlashColor = s.FlashColor;
				look.DotAlpha = 1.f;
				look.DotScale = 1.f + 0.6f * (1.f - t);	// the dot itself pops a little too
			}
		}
		DrawLaserGeometry(di, state, start, hitEnd, hitEnd, onTarget, look);
	}

	// ---- the beams ----
	for (auto& b : Beams)
	{
		if (gametic - b.Tic > kExpireTics) continue;
		DVector3 start = b.Start, end = b.End;
		if (b.Anchored && b.Hand >= 0 && hands[b.Hand].Valid)
		{
			start = FrameApply(hands[b.Hand], b.LocalStart);
			end = FrameApply(hands[b.Hand], b.LocalEnd);
		}
		AActor* ea = b.EndActor;
		if (ea != nullptr) end = ea->InterpolatedPosition(di->Viewpoint.TicFrac) + b.EndOfs;

		FTraceResults res{};
		bool onSurface = false;
		DVector3 nrm;
		if (b.Flags & RSL_TRACE)
		{
			DVector3 d = end - start;
			const double len = d.Length();
			if (len > 0.01)
			{
				d /= len;
				sector_t* sec = primaryLevel->PointInSector(start.XY());
				if (Trace(start, sec, d, len + 16.0, 0, ML_BLOCKEVERYTHING | ML_BLOCKHITSCAN, player->mo, res, TRACE_NoSky))
				{
					end = res.HitPos;
					onSurface = SurfaceNormal(res, start, nrm);
				}
			}
		}

		FRSLaserLook look;
		look.BeamColor = look.DotColor = b.Color;
		look.BeamAlpha = b.Alpha;
		look.CoreRadius = 0.5f * std::max(0.01f, b.Thick);
		look.Fade = 0.f;
		look.DrawBeam = true;
		look.DrawDot = !!(b.Flags & RSL_DOT);
		look.Lock = !!(b.Flags & RSL_LOCK);
		look.Scroll = !!(b.Flags & RSL_SCROLL);
		look.Flare = !!(b.Flags & RSL_FLARE);
		look.DotOnSurface = onSurface;
		look.DotNormal = nrm;
		look.DotRangeLimited = false;
		look.DotAlpha = b.Alpha;
		look.DotScale = 1.f + 2.f * b.Thick;   // a grab's dot is a target marker, bigger than a sight's pinpoint
		// Always drawn from the hand side: the halo's taper keeps the wide end away from the eye.
		// RSL_REVERSE turns the flow and moves the flare to the start (a reeled grab beam).
		look.FlowToStart = !!(b.Flags & RSL_REVERSE);
		DrawLaserGeometry(di, state, start, end, end, look.Lock, look);
	}
}

// ---------------------------------------------------------------------------------------
// ZScript: struct RSLaser (wadsrc/static/zscript/rs_laser.zs)
// ---------------------------------------------------------------------------------------
DEFINE_ACTION_FUNCTION(_RSLaser, Enabled)
{
	PARAM_PROLOGUE;
	ACTION_RETURN_BOOL(!!rs_laser);
}

DEFINE_ACTION_FUNCTION(_RSLaser, Sight)
{
	PARAM_PROLOGUE;
	PARAM_INT(hand);
	PARAM_FLOAT(mx); PARAM_FLOAT(my); PARAM_FLOAT(mz);
	PARAM_COLOR(col);
	PARAM_FLOAT(alpha);
	PARAM_INT(flags);
	PARAM_FLOAT(dx); PARAM_FLOAT(dy); PARAM_FLOAT(dz);
	if (hand != VR_MAINHAND && hand != VR_OFFHAND) return 0;
	auto& s = Sights[hand];
	s.Tic = gametic;
	s.Muzzle = FVector3((float)mx, (float)my, (float)mz);
	FVector3 d((float)dx, (float)dy, (float)dz);
	s.Dir = d.LengthSquared() > 1e-12f ? d.Unit() : FVector3(1, 0, 0);
	s.Color = col;
	s.Alpha = (float)std::clamp(alpha, 0.0, 1.0);
	s.Flags = flags;
	return 0;
}

// RemaSelaco 2026-11-15: HIT CONFIRMATION -- the sight's dot flashes col and throws a burst ring growing to `grow`
// times its size, over `tics` tics from now. A new flash replaces a running one.
DEFINE_ACTION_FUNCTION(_RSLaser, Flash)
{
	PARAM_PROLOGUE;
	PARAM_INT(hand);
	PARAM_COLOR(col);
	PARAM_FLOAT(tics);
	PARAM_FLOAT(grow);
	if (hand != VR_MAINHAND && hand != VR_OFFHAND) return 0;
	auto& s = Sights[hand];
	s.FlashColor = col;
	s.FlashStart = primaryLevel ? (double)primaryLevel->maptime : 0.0;
	s.FlashLen = (float)std::clamp(tics, 0.0, 350.0);
	s.FlashGrow = (float)std::clamp(grow, 0.0, 20.0);
	return 0;
}

DEFINE_ACTION_FUNCTION(_RSLaser, SightOff)
{
	PARAM_PROLOGUE;
	PARAM_INT(hand);
	if (hand == VR_MAINHAND || hand == VR_OFFHAND) Sights[hand].Tic = -1000;
	else Sights[0].Tic = Sights[1].Tic = -1000;
	return 0;
}

DEFINE_ACTION_FUNCTION(_RSLaser, Beam)
{
	PARAM_PROLOGUE;
	PARAM_INT(slot);
	PARAM_FLOAT(sx); PARAM_FLOAT(sy); PARAM_FLOAT(sz);
	PARAM_FLOAT(ex); PARAM_FLOAT(ey); PARAM_FLOAT(ez);
	PARAM_COLOR(col);
	PARAM_FLOAT(alpha);
	PARAM_FLOAT(thick);
	PARAM_INT(hand);
	PARAM_OBJECT(endActor, AActor);
	PARAM_INT(flags);
	if (slot < 0 || slot >= kMaxBeams) return 0;
	auto& b = Beams[slot];
	b.Tic = gametic;
	b.Start = DVector3(sx, sy, sz);
	b.End = DVector3(ex, ey, ez);
	b.Color = col;
	b.Alpha = (float)std::clamp(alpha, 0.0, 1.0);
	b.Thick = (float)std::clamp(thick, 0.01, 64.0);
	b.Flags = flags;
	b.EndActor = endActor;
	b.EndOfs = endActor ? b.End - endActor->Pos() : DVector3(0, 0, 0);

	// RIDING THE HAND. Script runs at 35 Hz, the hand is drawn every frame. The points are
	// carried into the hand's own frame -- relative to where the hand IS for the playsim
	// (AttackPos / OffhandPos, which the VR device writes from the same pose it draws), with
	// the axes the hand was last drawn with -- and put back through the hand's frame as drawn,
	// every frame. A beam from the palm stays in the palm at frame rate, and an aimed one
	// swings with the hand between tics instead of trailing it.
	b.Hand = (hand == VR_MAINHAND || hand == VR_OFFHAND) ? hand : -1;
	b.Anchored = false;
	player_t* player = &players[consoleplayer];
	if (b.Hand >= 0 && LastHand[b.Hand].Valid && player->mo != nullptr)
	{
		const DVector3 handPos = b.Hand == VR_OFFHAND ? player->mo->OffhandPos : player->mo->AttackPos;
		DVector3 ls, le;
		if (FrameInverse(LastHand[b.Hand], b.Start - handPos, ls) && FrameInverse(LastHand[b.Hand], b.End - handPos, le))
		{
			// The frame's own origin stands in for handPos when it is applied (FrameApply adds T).
			b.LocalStart = ls;
			b.LocalEnd = le;
			b.Anchored = true;
		}
	}
	return 0;
}

DEFINE_ACTION_FUNCTION(_RSLaser, Off)
{
	PARAM_PROLOGUE;
	PARAM_INT(slot);
	if (slot >= 0 && slot < kMaxBeams) { Beams[slot].Tic = -1000; Beams[slot].EndActor = nullptr; }
	else if (slot < 0) for (auto& b : Beams) { b.Tic = -1000; b.EndActor = nullptr; }
	return 0;
}
