//==========================================================================
//
// [BB] SDF font loading. See hw_sdffont.h for what a distance field is and
// why the atlas is built offline instead of here.
//
// This file does nothing clever on purpose. The generator
// (tools/sdffont/mksdf.ps1) already did the work; all that is left is to find
// two lumps, read a handful of numbers out of one of them, and turn cell
// rectangles into UVs. If something looks expensive in here it is a bug.
//
//==========================================================================

#include "hw_sdffont.h"

#include "filesystem.h"
#include "texturemanager.h"
#include "printf.h"
#include "c_cvars.h"		// EXTERN_CVAR, for the roster's slot-0 default
#include "m_random.h"		// FRandom, for the roster shuffle

//==========================================================================
//
// The cache. Keyed by the name the caller asked for, and it stores FAILURES
// as null rather than dropping them -- otherwise a mod that names a font it
// does not ship makes the engine hunt for two missing lumps on every single
// glyph of every frame, which is a stutter nobody would connect to a typo.
//
//==========================================================================

static TMap<FString, FSDFFont *> sSDFFonts;

FSDFFont *FSDFFont::Get(const char *name)
{
	if (name == nullptr || *name == 0) return nullptr;

	FString key = name;
	key.ToLower();

	if (FSDFFont **found = sSDFFonts.CheckKey(key))
		return *found;

	FSDFFont *font = new FSDFFont;
	if (!font->Load(name))
	{
		delete font;
		font = nullptr;
	}
	sSDFFonts.Insert(key, font);
	return font;
}

void FSDFFont::FlushAll()
{
	TMap<FString, FSDFFont *>::Iterator it(sSDFFonts);
	TMap<FString, FSDFFont *>::Pair *pair;
	while (it.NextPair(pair))
	{
		if (pair->Value != nullptr) delete pair->Value;
	}
	sSDFFonts.Clear();
}

const FSDFGlyph *FSDFFont::Glyph(int code) const
{
	const int i = code - mFirst;
	if (i < 0 || (unsigned)i >= mGlyphs.Size()) return nullptr;
	const FSDFGlyph &g = mGlyphs[i];
	return g.present ? &g : nullptr;
}

//==========================================================================
//
// Load. "<name>" is a texture; "sdffonts/<name>.txt" is its metrics.
//
// Returns false rather than complaining loudly for a font that simply is not
// there -- callers fall back to the bitmap glyph path, and a mod is allowed
// not to ship one. A metrics file that exists but does not parse IS worth a
// line in the log, because that is a broken asset rather than an absent one.
//
//==========================================================================

bool FSDFFont::Load(const char *name)
{
	mName = name;

	FTextureID id = TexMan.CheckForTexture(name, ETextureType::Any);
	if (!id.isValid())
	{
		Printf(TEXTCOLOR_YELLOW "SDF font '%s': no such texture; falling back to bitmap glyphs\n", name);
		return false;
	}
	mAtlas = TexMan.GetGameTexture(id, true);
	if (mAtlas == nullptr) return false;

	FString lumpName;
	lumpName.Format("sdffonts/%s.txt", name);
	const int lump = fileSystem.CheckNumForFullName(lumpName.GetChars(), true);
	if (lump < 0)
	{
		Printf(TEXTCOLOR_YELLOW "SDF font '%s': texture found but '%s' is missing; "
			"falling back to bitmap glyphs\n", name, lumpName.GetChars());
		return false;
	}

	auto fileData = fileSystem.ReadFile(lump);
	const size_t len = fileData.size();
	if (len == 0) return false;

	// Own a NUL-terminated copy: the parse below walks it destructively and
	// the filesystem's buffer is not ours to chew on.
	TArray<char> text((unsigned)len + 1, true);
	memcpy(text.Data(), fileData.data(), len);
	text[(unsigned)len] = 0;

	// The generator writes 32..126 today, but the table is sized to the whole
	// byte range so a future atlas can add glyphs without touching this.
	mFirst = 0;
	mLast = 255;
	mGlyphs.Clear();
	mGlyphs.Resize(256);
	for (auto &g : mGlyphs) g = FSDFGlyph();

	float atlasW = (float)mAtlas->GetTexelWidth();
	float atlasH = (float)mAtlas->GetTexelHeight();
	int glyphCount = 0;

	// Walked by hand rather than with strtok: the reentrant spelling differs
	// between MSVC and everyone else, and this needs no state to avoid the
	// question entirely.
	char *cursor = text.Data();
	while (*cursor != 0)
	{
		char *line = cursor;
		while (*cursor != 0 && *cursor != '\n' && *cursor != '\r') cursor++;
		if (*cursor != 0) { *cursor = 0; cursor++; }
		while (*cursor == '\n' || *cursor == '\r') cursor++;

		while (*line == ' ' || *line == '\t') line++;
		if (*line == 0 || *line == '#') continue;

		if (!strncmp(line, "cell ", 5))
		{
			mCell = (float)atof(line + 5);
		}
		else if (!strncmp(line, "spread ", 7))
		{
			mSpread = (float)atof(line + 7);
		}
		else if (!strncmp(line, "atlas ", 6))
		{
			// Trust the generator's dimensions over the texture's. A texture
			// can be padded to a power of two on upload, and UVs computed
			// against the padded size would slide every glyph.
			float w = 0.f, h = 0.f;
			if (sscanf(line + 6, "%f %f", &w, &h) == 2 && w > 0.f && h > 0.f)
			{
				atlasW = w;
				atlasH = h;
			}
		}
		else if (!strncmp(line, "g ", 2))
		{
			int code = 0;
			float gx = 0.f, gy = 0.f, gw = 0.f, gh = 0.f, adv = 0.f;
			if (sscanf(line + 2, "%d %f %f %f %f %f", &code, &gx, &gy, &gw, &gh, &adv) != 6)
				continue;
			if (code < mFirst || code > mLast || gw <= 0.f || gh <= 0.f)
				continue;

			FSDFGlyph &g = mGlyphs[code - mFirst];
			g.u0 = gx / atlasW;
			g.v0 = gy / atlasH;
			g.u1 = (gx + gw) / atlasW;
			g.v1 = (gy + gh) / atlasH;
			g.advance = adv;
			g.present = true;
			glyphCount++;
		}
	}

	if (glyphCount == 0 || mCell <= 0.f)
	{
		Printf(TEXTCOLOR_RED "SDF font '%s': metrics lump found but unusable "
			"(%d glyphs, cell %g)\n", name, glyphCount, mCell);
		return false;
	}

	// A spread of zero would make the shader's halo term meaningless and is
	// almost certainly a generator that was interrupted. The field still
	// draws, so this is a warning rather than a rejection.
	if (mSpread <= 0.f)
	{
		Printf(TEXTCOLOR_YELLOW "SDF font '%s': no spread in metrics; glow will be flat\n", name);
	}

	// Say so, once. The fallback to bitmap glyphs is deliberately invisible in
	// the frame -- text still appears, just soft -- so without a line here the
	// only symptom of a missing atlas is "the letters look a bit off", which is
	// not something anyone diagnoses quickly. Caching means this prints once.
	Printf("SDF font '%s': %d glyphs, cell %g, spread %g\n",
		name, glyphCount, mCell, mSpread);
	return true;
}

//==========================================================================
//
// [BB] THE FONT ROSTER.
//
// Scans for every metrics lump the load actually contains, keeps the ones
// that resolve to a real atlas, shuffles them, and hands them out by slot.
// See hw_sdffont.h for what a slot means and why the order is rolled.
//
//==========================================================================

EXTERN_CVAR(String, bb_sdffont)

// Named, so it shows up in the RNG list like every other consumer rather
// than being an anonymous source of nondeterminism.
//
// FCRandom, NOT FRandom, and that is the whole point: an FRandom is PLAYSIM RNG
// -- it sits on RNGList and is serialized with the game -- while this roster is
// rolled LAZILY off a DRAW path (FSDFFontRoster::Slot, from hw_sprites.cpp). A
// machine that never draws an SDF billboard never rolled it and one that did
// rolled it at a frame-dependent moment, so two peers' serialized RNG state
// drifted apart over something purely cosmetic. Every other rendering-side RNG
// in this tree is already FCRandom; this was the one exception.
static FCRandom pr_sdfroster("SDFFontRoster");

// Names only. The fonts themselves stay in FSDFFont::Get's cache, which
// already dedupes and already survives across rolls -- a re-roll reorders
// the roster, it does not reload any atlas.
static TArray<FString> sRoster;
static bool sRosterRolled = false;

void FSDFFontRoster::Invalidate()
{
	sRoster.Clear();
	sRosterRolled = false;
}

void FSDFFontRoster::Roll()
{
	sRoster.Clear();
	sRosterRolled = true;

	// Every "sdffonts/<name>.txt" in the load order, whatever shipped it.
	// Deliberately a scan rather than a hardcoded list: a mod that adds a
	// face should join the roster by dropping in two files, with no engine
	// change and no registration step to forget.
	const int count = fileSystem.GetNumEntries();
	for (int i = 0; i < count; i++)
	{
		const char *full = fileSystem.GetFileFullName(i, false);
		if (full == nullptr) continue;

		FString name = full;
		name.ToLower();
		if (name.IndexOf("sdffonts/") != 0) continue;
		if (name.Len() < 13 || name.Right(4).Compare(".txt") != 0) continue;

		// "sdffonts/foo.txt" -> "foo"
		FString base = name.Mid(9, name.Len() - 9 - 4);
		if (base.IsEmpty()) continue;

		// A metrics file with no atlas beside it is not a font. Checked
		// BEFORE FSDFFont::Get so a stray text file in the folder -- a
		// manifest, a readme -- is skipped in silence instead of printing
		// a scary warning about a font nobody was asking for.
		if (!TexMan.CheckForTexture(base.GetChars(), ETextureType::Any).isValid())
			continue;

		// Load it now rather than on first draw. A font that parses badly
		// should drop out of the roster here, where the cost is one skipped
		// entry, not mid-game where the cost is a card of invisible text.
		if (FSDFFont::Get(base.GetChars()) == nullptr)
			continue;

		// The default face is reachable as slot 0 by every caller already;
		// letting it also appear in the roster would make it twice as likely
		// as any other font and make "slot 0 vs slot 3" sometimes a no-op.
		if (base.Compare(FString(bb_sdffont).MakeLower()) == 0)
			continue;

		sRoster.Push(base);
	}

	// Fisher-Yates. Walking backwards and swapping with a random earlier
	// index touches each entry exactly once and cannot bias toward the
	// front the way repeated random-index swapping does.
	for (int i = (int)sRoster.Size() - 1; i > 0; i--)
	{
		const int j = pr_sdfroster(i + 1);
		if (j == i) continue;
		// Element swap, written out: TArray::Swap swaps two whole ARRAYS,
		// so the obvious-looking sRoster.Swap(i, j) does not compile.
		FString tmp = sRoster[i];
		sRoster[i] = sRoster[j];
		sRoster[j] = tmp;
	}

	Printf("SDF font roster: %u face(s) rolled, slot 0 = '%s'\n",
		sRoster.Size(), *bb_sdffont);
}

int FSDFFontRoster::Count()
{
	if (!sRosterRolled) Roll();
	return (int)sRoster.Size();
}

FSDFFont *FSDFFontRoster::Slot(int n)
{
	if (!sRosterRolled) Roll();

	// Slot 0, anything negative, and anything past the end all resolve to
	// the default face. Falling back to the WRONG font is recoverable and
	// visible; falling back to nothing is a blank card that reads as a
	// content bug.
	if (n <= 0 || n > (int)sRoster.Size())
		return FSDFFont::Get(bb_sdffont);

	if (FSDFFont *f = FSDFFont::Get(sRoster[n - 1].GetChars()))
		return f;
	return FSDFFont::Get(bb_sdffont);
}

const char *FSDFFontRoster::SlotName(int n)
{
	if (!sRosterRolled) Roll();
	if (n <= 0 || n > (int)sRoster.Size())
		return bb_sdffont;
	return sRoster[n - 1].GetChars();
}
