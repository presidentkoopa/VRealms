// RS -- THE RS LASER: UZDXREMA's laser sight, ported to RemaSelaco (src/rendering/hwrenderer/scene/hw_rslaser.cpp).
//
// A core inside a soft halo, faded along its length, a dot that keeps one size on screen and lies flat on what it
// lands on, and a lock that tightens and breathes on a live monster. Drawn by the renderer every frame, per eye, from
// the hand as it is drawn -- so it never trails the controller the way a 35 Hz actor does.
//
// PRESENTATION ONLY, LOCAL ONLY: nothing is saved and nothing reaches the playsim. A request lasts two tics past its
// last call, so CALL IT EVERY TIC you want it drawn and simply stop calling to put it out. The renderer draws the
// console player's requests; call it for him only. rs_laser 0 hides everything.

enum ERSLaserFlags
{
	RSL_NODOT   = 1,      // Sight: beam only
	RSL_NOBEAM  = 2,      // Sight: dot only
	RSL_NOLOCK  = 4,      // Sight: no lock reaction on live monsters
	RSL_TRACE   = 8,      // Beam: clipped against walls every frame along start -> end (+16 units)
	RSL_DOT     = 16,     // Beam: a dot at the end (the end that is not the hand)
	RSL_LOCK    = 32,     // Beam: the dot is locked (tightens, brightens, breathes)
	RSL_REVERSE = 64,     // Beam: the energy flows toward the start and the flare sits there (a beam reeling into the hand)
	RSL_SCROLL  = 128,    // Beam: energy beads travel along it
	RSL_FLARE   = 256,    // Beam: a soft flare where it ends (as drawn)
}

struct RSLaser native
{
	// rs_laser: false means nothing you request is drawn (use your own fallback).
	native static bool Enabled();

	// A LASER SIGHT ON A GUN, for this tic. hand: 0 main, 1 off. muzzle and dir are in the GUN'S OWN FRAME as
	// RS_Selaco_VRWeapons measures it: F forward, L left, U up, in cm, the hand's model frame (r_data/models.cpp
	// RenderHUDModel: x = -L, y = U, z = -F, after its 0.01 scale). The renderer puts them through the very matrix
	// the gun's model is drawn with, every frame, and traces from there: the beam leaves the muzzle, the dot is
	// where it lands. alpha scales the beam and the dot. Only in VR (no hand frame, no sight).
	native static void Sight(int hand, Vector3 muzzle, Color col, double alpha = 1.0, int flags = 0, Vector3 dir = (1, 0, 0));
	native static void SightOff(int hand = -1);

	// HIT CONFIRMATION (RemaSelaco 2026-11-15): the sight's dot on that hand takes col and throws a burst ring that
	// grows to `grow` times the dot's size and fades over `tics` tics (frame-smooth). Shows while the sight is drawn.
	native static void Flash(int hand, Color col, double tics = 8, double grow = 3);

	// A BEAM, for this tic. slot 0..31 (caller-managed; RSQ Distance Grab uses 24 and 25). start and end are map
	// points. thick is the core's width in map units (the halo is 2.6 and 5.2 times it). hand 0 / 1: the beam rides
	// that hand between tics (both points are carried in the hand's frame, the end too unless endActor is given).
	// endActor: the end rides that actor's drawn position (end - its position now is kept). flags: ERSLaserFlags.
	native static void Beam(int slot, Vector3 start, Vector3 end, Color col, double alpha = 1.0, double thick = 1.0,
		int hand = -1, Actor endActor = null, int flags = 0);
	native static void Off(int slot = -1);
}

// THE SAME, FOUND BY NAME -- for a mod that must also run where this engine is not (RSQ Distance Grab on stock
// QuestZDoom): nothing links against RSLaser, and an absent engine is simply no service.
//
//   let s = ServiceIterator.Find("RSLaserService").Next();
//   bool engineLaser = s && s.GetInt("available") == 1;
//
//   "available"  1 when this engine draws lasers now (rs_laser on), else 0
//   "beam"       intArg the slot (0..31), doubleArg the alpha, objectArg the end actor (or null),
//                stringArg "sx sy sz ex ey ez rrggbb thick hand flags" (spaces; colour in hex; hand -1/0/1;
//                flags ERSLaserFlags as a number). Returns 1 when it will be drawn, 0 when not (draw your own).
//   "beam.off"   intArg the slot (-1 all)
//   "sight"      intArg the hand (0 main, 1 off), doubleArg the alpha, stringArg "mx my mz rrggbb flags" (+ " dx dy dz"),
//                the muzzle and barrel in the gun's frame (RSLaser.Sight). Returns 1 when it will be drawn.
//   "sight.off"  intArg the hand (-1 both)
//   "flash"      intArg the hand, stringArg "rrggbb tics grow" (RSLaser.Flash). Returns 1. An engine older than
//                2026-11-15 answers 0 and draws nothing.
class RSLaserService : Service
{
	override int GetInt(String request, String stringArg, int intArg, double doubleArg, Object objectArg, Name nameArg)
	{
		if (request == "available") return RSLaser.Enabled() ? 1 : 0;
		if (request == "beam.off") { RSLaser.Off(intArg); return 1; }
		if (request == "beam")
		{
			Array<String> f;
			stringArg.Split(f, " ", TOK_SKIPEMPTY);
			if (f.Size() < 10 || intArg < 0 || intArg > 31) return 0;
			Vector3 a = (f[0].ToDouble(), f[1].ToDouble(), f[2].ToDouble());
			Vector3 b = (f[3].ToDouble(), f[4].ToDouble(), f[5].ToDouble());
			Color c = f[6].ToInt(16);
			RSLaser.Beam(intArg, a, b, c, doubleArg, f[7].ToDouble(), f[8].ToInt(), Actor(objectArg), f[9].ToInt());
			return RSLaser.Enabled() ? 1 : 0;
		}
		if (request == "sight.off") { RSLaser.SightOff(intArg); return 1; }
		if (request == "flash")
		{
			Array<String> f;
			stringArg.Split(f, " ", TOK_SKIPEMPTY);
			if (f.Size() < 3 || intArg < 0 || intArg > 1) return 0;
			Color c = f[0].ToInt(16);
			RSLaser.Flash(intArg, c, f[1].ToDouble(), f[2].ToDouble());
			return 1;
		}
		if (request == "sight")
		{
			Array<String> f;
			stringArg.Split(f, " ", TOK_SKIPEMPTY);
			if (f.Size() < 5 || intArg < 0 || intArg > 1) return 0;
			Vector3 m = (f[0].ToDouble(), f[1].ToDouble(), f[2].ToDouble());
			Color c = f[3].ToInt(16);
			Vector3 d = (1, 0, 0);
			if (f.Size() >= 8) d = (f[5].ToDouble(), f[6].ToDouble(), f[7].ToDouble());
			RSLaser.Sight(intArg, m, c, doubleArg, f[4].ToInt(), d);
			return RSLaser.Enabled() ? 1 : 0;
		}
		return 0;
	}
}
