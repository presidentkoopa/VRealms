// test_roth_surface.cpp -- unit tests for the surface rules.
//
// These check the parts of roth_surface that can be checked without a map: the
// three-way texture classification, the units-per-texel ladder including the
// 256x256 opaque exception, the wrap on negative world coordinates, and the
// mirror. The map-wide static texel test (every sector in every map, against
// the engine values the loader chose) is a separate, larger harness; this one
// exists so that a mistake in the arithmetic is caught in a second rather than
// inside a 44-map run.
//
// Build:
//   i686-w64-mingw32-g++ -std=c++17 -O1 -I ../../src/roth \
//       -o test_roth_surface.exe test_roth_surface.cpp ../../src/roth/roth_surface.cpp

#include "roth_surface.h"
#include <stdio.h>

static int g_fail = 0;

static void ck(bool cond, const char *what)
{
	if (!cond) { printf("  FAIL  %s\n", what); g_fail++; }
}

static void ckeq(long got, long want, const char *what)
{
	if (got != want) { printf("  FAIL  %s: got %ld want %ld\n", what, got, want); g_fail++; }
}

int main()
{
	using namespace roth;
	const uint16_t KEY = 1;      // STUDY1's pack carries key 0; 1 is used here
	                             // so the "zero is not special" case is covered.

	printf("ClassifySurface (the THREE-way test, renderer.c:9222-9225)\n");
	{
		SurfaceClass a = ClassifySurface(0xFF78, KEY);
		ck(a.kind == SurfaceKind::SolidColour, "0xFF78 is a solid colour");
		ckeq(a.solidIndex, 0x78, "0xFF78 solid index");

		SurfaceClass b = ClassifySurface(KEY, KEY);
		ck(b.kind == SurfaceKind::Nothing, "the colour key draws nothing");

		SurfaceClass c = ClassifySurface(58, KEY);
		ck(c.kind == SurfaceKind::Textured, "an ordinary index is textured");

		// Zero is an ordinary texture index unless it IS the key. This mattered:
		// the old sky test compared against a marker that is 0 in most packs,
		// so every index-0 surface was being treated as sky.
		SurfaceClass d = ClassifySurface(0, KEY);
		ck(d.kind == SurfaceKind::Textured, "index 0 is textured when the key is not 0");
		SurfaceClass e = ClassifySurface(0, 0);
		ck(e.kind == SurfaceKind::Nothing, "index 0 draws nothing when the key IS 0");
	}

	printf("FlatUnitsPerTexel (2^s, and the 256x256 opaque exception)\n");
	{
		FlatSetup s = {};
		s.texW = 64; s.texH = 64; s.textureWord = 58;
		for (int i = 0; i < 4; i++)
		{
			s.scaleBits = (uint8_t)i;
			const double want = (double)(1 << i);
			ck(FlatUnitsPerTexel(s) == want, "2^s ladder");
		}

		// STUDY1, measured by the oracle: floors s=1 on 32x32 and 64x64, and a
		// 256x256 ceiling at s=2. Both must come out at 2 units per texel --
		// that agreement is the whole reason to believe the exception.
		FlatSetup fl = {}; fl.texW = 64; fl.texH = 64; fl.scaleBits = 1; fl.textureWord = 58;
		FlatSetup ce = {}; ce.texW = 256; ce.texH = 256; ce.scaleBits = 2; ce.textureWord = 10;
		ce.opaque256 = true;
		ck(FlatUnitsPerTexel(fl) == 2.0, "STUDY1 floor s=1 -> 2 units/texel");
		ck(FlatUnitsPerTexel(ce) == 2.0, "STUDY1 256x256 ceiling s=2 -> 2 units/texel");

		// What we shipped instead, for the record: 2^(s+1) gives 4 and 8.
		ck(FlatUnitsPerTexel(fl) != 4.0, "not the shipped 2^(s+1) value");
	}

	printf("FlatTexel: wrap, negatives, shifts, mirrors\n");
	{
		FlatSetup s = {};
		s.textureWord = 58; s.texW = 64; s.texH = 64; s.scaleBits = 1;  // 2 units/texel

		Texel t = FlatTexel(s, 0, 0, KEY);
		ck(t.ok, "origin samples");
		ckeq(t.col, 0, "origin col");
		ckeq(t.row, 0, "origin row");

		// col = -x/2 mod 64. x = 128 -> -64 -> wraps to 0. x = 2 -> -1 -> 63.
		ckeq(FlatTexel(s, 128, 0, KEY).col, 0, "x=128 wraps a whole texture");
		ckeq(FlatTexel(s, 2, 0, KEY).col, 63, "x=2 steps one texel BACKWARDS");

		// NEGATIVE world coordinates are the common case west of the origin,
		// and C's % would give a negative texel there.
		ckeq(FlatTexel(s, -2, 0, KEY).col, 1, "x=-2 steps forwards");
		ckeq(FlatTexel(s, -256, 0, KEY).col, 0, "large negative x still wraps in range");
		ckeq(FlatTexel(s, 0, -2, KEY).row, 63, "negative y wraps in range");

		// row = +y/2 mod 64, the opposite sign to col.
		ckeq(FlatTexel(s, 0, 2, KEY).row, 1, "y=2 steps forwards");

		// A shift unit is half a texel.
		FlatSetup sh = s; sh.shiftX = 4;
		ckeq(FlatTexel(sh, 0, 0, KEY).col, 2, "shiftX 4 = 2 texels");

		// ...and a FULL texel on a 256x256 opaque image.
		FlatSetup op = {}; op.textureWord = 10; op.texW = 256; op.texH = 256;
		op.scaleBits = 2; op.opaque256 = true; op.shiftX = 4;
		ckeq(FlatTexel(op, 0, 0, KEY).col, 4, "256 opaque: shiftX 4 = 4 texels");

		// The mirror negates the axis INCLUDING the shift.
		FlatSetup mi = sh; mi.mirrorX = true;
		ckeq(FlatTexel(mi, 0, 0, KEY).col, 62, "mirrorX negates the shift too");

		// A surface that draws nothing yields no texel at all, rather than
		// silently sampling texel 0 -- that distinction is the ceiling holes.
		FlatSetup key = s; key.textureWord = KEY;
		ck(!FlatTexel(key, 0, 0, KEY).ok, "the colour key yields no texel");
		FlatSetup sol = s; sol.textureWord = 0xFF78;
		ck(!FlatTexel(sol, 0, 0, KEY).ok, "a solid colour yields no texel");
	}

	printf("FlatToEngine round trip: the engine must sample what ROTH samples\n");
	{
		// THE STATIC TEXEL TEST, in miniature. For every combination of scale,
		// shift and mirror, and a spread of world points including negatives
		// and far-from-origin ones, the engine encoding must agree with
		// FlatTexel exactly. This is what makes "the loader is right" a
		// property of one function instead of a claim about five call sites.
		const int32_t pts[] = { 0, 1, 2, 3, 7, 64, 127, 128, 1024, 3840,
		                        -1, -2, -7, -64, -128, -1025, -3840 };
		const uint16_t sizes[][2] = { {32,32}, {64,64}, {32,16}, {193,59} };

		int checked = 0, bad = 0;
		for (int si = 0; si < 4; si++)
		for (int sc = 0; sc < 4; sc++)
		for (int shx = 0; shx <= 5; shx++)
		for (int shy = 0; shy <= 5; shy++)
		for (int mx = 0; mx < 2; mx++)
		for (int my = 0; my < 2; my++)
		{
			FlatSetup s = {};
			s.textureWord = 58;
			s.texW = sizes[si][0]; s.texH = sizes[si][1];
			s.scaleBits = (uint8_t)sc;
			s.shiftX = (uint8_t)shx; s.shiftY = (uint8_t)shy;
			s.mirrorX = mx != 0; s.mirrorY = my != 0;

			const FlatEngineSetup e = FlatToEngine(s);
			for (size_t i = 0; i < sizeof(pts)/sizeof(pts[0]); i++)
			for (size_t j = 0; j < sizeof(pts)/sizeof(pts[0]); j++)
			{
				const Texel want = FlatTexel(s, pts[i], pts[j], KEY);
				const Texel got  = EngineTexel(e, s.texW, s.texH, pts[i], pts[j]);
				checked++;
				if (want.col != got.col || want.row != got.row)
				{
					if (bad < 4)
						printf("  FAIL  s=%d sh=(%d,%d) mir=(%d,%d) %ux%u "
						       "at (%ld,%ld): roth (%u,%u) engine (%u,%u)\n",
						       sc, shx, shy, mx, my, s.texW, s.texH,
						       (long)pts[i], (long)pts[j],
						       want.col, want.row, got.col, got.row);
					bad++;
				}
			}
		}
		if (bad) { printf("  %d of %d sample points disagree\n", bad, checked); g_fail++; }
		else     printf("  %d sample points agree across every scale, shift and mirror\n", checked);

		// And the 256x256 opaque exception, whose shift unit is a whole texel.
		FlatSetup op = {};
		op.textureWord = 10; op.texW = 256; op.texH = 256;
		op.scaleBits = 2; op.opaque256 = true;
		int obad = 0;
		for (int shx = 0; shx <= 5; shx++)
		for (int mx = 0; mx < 2; mx++)
		{
			op.shiftX = (uint8_t)shx; op.mirrorX = mx != 0;
			const FlatEngineSetup e = FlatToEngine(op);
			for (int32_t x = -600; x <= 600; x += 37)
			{
				if (FlatTexel(op, x, 0, KEY).col != EngineTexel(e, 256, 256, x, 0).col)
					obad++;
			}
		}
		ckeq(obad, 0, "256x256 opaque round trip");
	}

	if (g_fail == 0) printf("\nall roth_surface tests passed\n");
	else             printf("\n%d FAILURE(S)\n", g_fail);
	return g_fail ? 1 : 0;
}
