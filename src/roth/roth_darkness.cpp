// roth_darkness.cpp -- the darkness pass (RS_Radiance's, already in this engine) driven from the game, not cvars.
//
// The owner's idea (2026-11-13): darkness that each lit candle pushes back, "look at my online repo RS Radiance,
// particularly its Darkness settings". This engine carries that pass: FLevelLocals::Dark* read by DarknessAt in
// main.fp (the curve, then distance and height), applied to the sector's own light BEFORE dynamic lights are added
// (main.fp ~3986 vs ProcessMaterialLight), so a candle's light is never darkened.
//
// RS_Radiance keeps its settings in saved (server) cvars. The owner's doomxr.ini is shared with his hands build and
// must never be written by this engine or its tests (CLAUDE.md), so VRealms sets the level's fields directly.
//
// This file is the TEST HOOK only: `roth_darktest` sets the pass by hand so the look can be judged in captures
// (first over roth_palette_shading 1). The per-map / per-room values the game will set come after the owner has
// judged those captures. The engine clears the level's darkness on a map change (p_setup.cpp), so a test sets it
// after the map has loaded (roth_at).

#include "g_levellocals.h"
#include "c_dispatch.h"
#include "printf.h"

#include <stdlib.h>

// roth_darktest off
// roth_darktest <mode> <adjust> [minlight pregain postgain [distdepth distrange [actorspare]]]
//   mode: 0 off, 1 subtract, 2 compress, 3 cap brightest, 4 deepen shadows (RS_Radiance's curves, same constants)
//   adjust 0..256 (the curve's amount); distdepth 0..1 at distrange map units; actorspare 0..1
CCMD(roth_darktest)
{
	FLevelLocals *lv = primaryLevel;
	if (lv == nullptr)
		return;
	if (argv.argc() < 2)
	{
		Printf("roth_darktest off | <mode 0-4> <adjust> [minlight pregain postgain [distdepth distrange [actorspare]]]\n"
			"  now: mode %d adjust %.1f minlight %.1f pregain %.1f postgain %.1f, distance %.2f at %.0f, actors spared %.2f\n",
			lv->DarkMode, lv->DarkAdjust, lv->DarkMinLight, lv->DarkPreGain, lv->DarkPostGain,
			lv->DarkDistDepth, lv->DarkDistRange, lv->DarkActorExempt);
		return;
	}
	if (!stricmp(argv[1], "off"))
	{
		lv->DarkMode = 0;
		lv->DarkDistDepth = 0;
		lv->DarkHeightDepth = 0;
		lv->DarkActorExempt = 0;
		Printf("roth_darktest: off\n");
		return;
	}
	auto f = [&](int i, double def) { return argv.argc() > i ? atof(argv[i]) : def; };
	lv->DarkMode = atoi(argv[1]);
	lv->DarkAdjust = f(2, 0.0);
	lv->DarkMinLight = f(3, 0.0);
	lv->DarkPreGain = f(4, 0.0);
	lv->DarkPostGain = f(5, 0.0);
	lv->DarkDistDepth = f(6, 0.0);
	lv->DarkDistRange = f(7, 1024.0);
	lv->DarkActorExempt = f(8, 0.4);
	lv->DarkHeightDepth = 0;
	Printf("roth_darktest: mode %d adjust %.1f minlight %.1f pregain %.1f postgain %.1f, distance %.2f at %.0f units, "
		"actors spared %.2f\n", lv->DarkMode, lv->DarkAdjust, lv->DarkMinLight, lv->DarkPreGain, lv->DarkPostGain,
		lv->DarkDistDepth, lv->DarkDistRange, lv->DarkActorExempt);
}
