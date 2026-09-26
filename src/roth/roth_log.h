#pragma once
//
// Diagnostics for the Realms of the Haunting loader.
//
// Loading a foreign format from a stranger's disk fails in ways that are hard
// to guess at from a screenshot, so every load writes a full report to its own
// file: what was opened, what was in it, what could not be understood, and how
// long each stage took. When something looks wrong in game, that file should
// say why without anyone having to reproduce it.
//
// Counters are the important part. Anything the loader cannot handle is
// COUNTED rather than silently skipped, so "97% of this map converted" can
// never hide "and the other 3% vanished".
//

#include <stdint.h>
#include <string>
#include <vector>
#include <map>

namespace roth
{

class Log
{
public:
	// Starts a fresh report. Path is chosen from the map name; an empty
	// directory means alongside the executable.
	void Begin(const char *mapName, const char *dir = nullptr);
	void End();

	void Line(const char *fmt, ...);
	void Section(const char *title);

	// Counted, not printed: totals appear in the summary at the end. Use this
	// for anything unhandled, so gaps are always visible.
	void Count(const char *what, int n = 1);

	// A problem worth surfacing in the console too, not just the file.
	void Warn(const char *fmt, ...);

	// Stage timing, so a slow load can be attributed rather than guessed at.
	void StageBegin(const char *name);
	void StageEnd();

	int Warnings() const { return mWarnings; }
	const std::string &Path() const { return mPath; }

private:
	void Write(const char *text);

	void *mFile = nullptr;
	std::string mPath;
	int mWarnings = 0;
	std::map<std::string, int> mCounts;
	std::vector<std::pair<std::string, uint64_t>> mStages;
	std::string mStage;
	uint64_t mStageStart = 0;
	uint64_t mBegin = 0;
};

Log &TheLog();

} // namespace roth
