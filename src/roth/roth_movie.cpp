//
// Realms of the Haunting's movies in the engine. See roth_movie.h.
//
// NOT COMPILED OR RUN in the lane that wrote it: type-checked with g++
// against the engine headers (tools/roth_syntaxcheck.sh). The decoder it
// plays is differential-tested against the original (tools/gdv_test).
//

#include "roth_movie.h"
#include "roth_held.h"
#include "roth_install.h"
#include "roth_interface.h"
#include "roth_dbase100.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "c_dispatch.h"
#include "c_cvars.h"
#include "d_event.h"
#include "g_game.h"
#include "gamestate.h"
#include "i_interface.h"
#include "printf.h"
#include "screenjob.h"
#include "vm.h"

namespace roth
{

namespace
{

struct Pending
{
	std::string name;
	uint32_t subtitleOffset = 0;
	int32_t subtitleSize = 0;
};

std::vector<Pending> gQueue;        // asked for, not started
std::vector<Pending> gBatch;        // the cutscene now running, in order
bool gRunning = false;

// RothMovieJob.AddTo (wadsrc/static/zscript/engine/screenjob.zs): append one
// movie job to the runner, as ScreenJobRunner.AddGenericVideo does.
bool AddMovieJob(DObject *runner, const std::string &name)
{
	auto func = LookupFunction("RothMovieJob.AddTo", false);
	if (func == nullptr) return false;
	FString fn = name.c_str();
	VMValue val[] = { runner, &fn };
	VMCall(func, val, 2, nullptr, 0);
	return true;
}

} // namespace

// For unattended test runs only (the bridge's capture configs set it to 0, so a
// run does not sit through the boot's eight-minute INTRO before its map). Not
// archived, on by default: the game plays every movie the original plays. The
// game state a movie causes is set by the flow either way.
CVAR(Bool, roth_movies, true, 0)

static bool gSuppressed = false;
void MovieSetSuppressed(bool on) { gSuppressed = on; }

void MovieQueue(const char *name, uint32_t subtitleOffset, int32_t subtitleSize)
{
	if (gSuppressed)
	{
		Printf("Realms: movie %s.GDV not played (intro movies off: roth_intro_movies 0)\n", name != nullptr ? name : "");
		return;
	}
	if (!roth_movies)
	{
		Printf("Realms: movie %s.GDV not played (roth_movies 0)\n", name != nullptr ? name : "");
		return;
	}
	Pending p;
	p.name = name != nullptr ? name : "";
	p.subtitleOffset = subtitleOffset;
	p.subtitleSize = subtitleSize;
	if (p.name.empty()) return;
	gQueue.push_back(p);
	Printf("Realms: movie %s.GDV queued (subtitles %d bytes at DBASE400 +0x%x)\n", p.name.c_str(), (int)subtitleSize, subtitleOffset);
}

bool MovieBusy()
{
	return !gQueue.empty() || gRunning;
}

// The flow asks during a tic -- in a DBASE100 action, or during the level
// load for the boot's action 1 -- so the cutscene is started at the end of
// that level tic (or the first one after the load, which would overwrite the
// gameaction: G_DoLoadLevel sets ga_nothing), and the next tic is the
// cutscene's: the level does not tick in between.
// Every movie asked for by then goes into one runner, in order, so movie
// follows movie without the level showing between them; the original plays
// them back to back the same way, each one's call returning before the next
// op runs (gdv_cutscene.c:2840).
void MoviePump()
{
	// A runner ended without its completion (a level change deletes it): free the queue.
	if (gRunning && cutscene.runner == nullptr) gRunning = false;
	if (gQueue.empty() || gRunning) return;
	if (cutscene.runner != nullptr || gamestate != GS_LEVEL || gameaction != ga_nothing) return;

	gBatch = std::move(gQueue);
	gQueue.clear();
	cutscene.completion = [](bool)
	{
		// G_StartSlideshow's return to a running level (g_game.cpp).
		gamestate = GS_LEVEL;
		wipegamestate = GS_LEVEL;
		gRunning = false;
		Printf("Realms: movies over, back to the level\n");
		HeldMovieOver();   // a forced turn waiting for the black: the VR view takes it now (roth_held.h, owner Q7)
	};
	cutscene.runner = CreateRunner(true);
	GC::WriteBarrier(cutscene.runner);
	int added = 0;
	for (const Pending &p : gBatch) if (AddMovieJob(cutscene.runner, p.name)) added++;
	if (added == 0 || !ScreenJobValidate())
	{
		DeleteScreenJob();
		cutscene.completion = nullptr;
		Printf("Realms: no movie of %d could be opened (see the lines above)\n", (int)gBatch.size());
		gBatch.clear();
		return;
	}
	gRunning = true;
	// SJ_BLOCKUI: GS_INTRO, where Escape reaches the job instead of opening
	// the menu (d_event.cpp D_ProcessEvents) -- the original's skip key.
	if (sysCallbacks.StartCutscene) sysCallbacks.StartCutscene(true);
	Printf("Realms: playing %d movie(s)\n", (int)gBatch.size());
}

bool MovieFetch(const char *name, MovieSource &out, std::string &error)
{
	out = MovieSource();
	out.name = name != nullptr ? name : "";
	const Install &inst = TheInstall();
	if (!inst.IsOpen()) { error = "no Realms install is open (roth_path)"; return false; }
	// g_dir_gdv is ".\GDV\" beside the databases (play_record_gdv_cutscene
	// builds <dir><name>.GDV, gdv_cutscene.c:2827-2831).
	const std::string path = inst.DataFile(("GDV/" + out.name + ".GDV").c_str());
	if (path.empty()) { error = "GDV/" + out.name + ".GDV is not in the install"; return false; }
	out.bytes = Install::ReadWholeFile(path);
	if (out.bytes.empty()) { error = "cannot read " + path; return false; }

	// The subtitle stream the flow named with it (gdv_cutscene.c:2777-2797),
	// read from DBASE400.DAT as play_record_gdv_cutscene reads it.
	const Pending *p = nullptr;
	for (const Pending &b : gBatch) if (b.name == out.name) { p = &b; break; }
	if (p != nullptr && p->subtitleSize > 0)
	{
		const std::string d400 = inst.DataFile("DBASE400.DAT");
		if (FILE *f = d400.empty() ? nullptr : fopen(d400.c_str(), "rb"))
		{
			out.subtitles.resize((size_t)p->subtitleSize);
			if (fseek(f, (long)p->subtitleOffset, SEEK_SET) != 0
				|| fread(out.subtitles.data(), 1, out.subtitles.size(), f) != out.subtitles.size())
				out.subtitles.clear();
			fclose(f);
		}
	}
	out.haveGamePalette = InterfaceGamePalette6(out.gamePalette);
	return true;
}

void MovieFinished(const char *name, int records, int recordCount, bool skipped)
{
	Printf("Realms: movie %s.GDV %s after %d of %d records\n", name, skipped ? "skipped (Esc)" : "ended", records, recordCount);
}

} // namespace roth

// roth_movie <NAME> -- play a movie as the flow would, from the next level
// tic, with the subtitle stream its DBASE100 cutscene entry names (+0x10 and
// +0x0a, gdv_cutscene.c:2777-2797). A test instrument; it changes no game
// state (the gallery stamp and the mode are the flow's, PlayRecordGdvCutscene).
CCMD(roth_movie)
{
	if (argv.argc() < 2) { Printf("roth_movie <GDV name>\n"); return; }
	FString name = argv[1];
	name.ToUpper();
	uint32_t off = 0;
	int32_t size = 0;
	roth::Dbase100 &db = roth::TheDbase100();
	for (int i = 0; db.IsLoaded() && i < db.CutsceneCount(); i++)
	{
		const roth::DbCutscene *c = db.Cutscene(i);
		if (c != nullptr && stricmp(c->name.c_str(), name.GetChars()) == 0)
		{
			off = c->subtitleText & 0xffffffu;
			size = c->subtitleSize;
			Printf("roth_movie: %s is cutscene entry %d\n", name.GetChars(), i);
			break;
		}
	}
	roth::MovieQueue(name.GetChars(), off, size);
}
