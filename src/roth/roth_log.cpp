//
// Diagnostics for the Realms of the Haunting loader. See roth_log.h.
//

#include "roth_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <chrono>

namespace roth
{

namespace
{
uint64_t NowMs()
{
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(
		steady_clock::now().time_since_epoch()).count();
}
} // namespace

void Log::Begin(const char *mapName, const char *dir)
{
	End();
	mWarnings = 0;
	mCounts.clear();
	mStages.clear();
	mBegin = NowMs();

	char name[256];
	snprintf(name, sizeof(name), "%sroth_%s.log",
		(dir && *dir) ? dir : "", mapName ? mapName : "map");
	mPath = name;

	mFile = fopen(mPath.c_str(), "w");
	if (!mFile) return;

	time_t now = time(nullptr);
	char when[64] = { 0 };
	struct tm *tmv = localtime(&now);
	if (tmv) strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", tmv);

	Line("Realms of the Haunting loader report");
	Line("map      %s", mapName ? mapName : "?");
	Line("time     %s", when);
	Line("");
}

void Log::End()
{
	if (!mFile) return;

	StageEnd();

	Section("Stage timing");
	uint64_t total = NowMs() - mBegin;
	for (auto &s : mStages)
		Line("  %-28s %6llu ms", s.first.c_str(), (unsigned long long)s.second);
	Line("  %-28s %6llu ms", "TOTAL", (unsigned long long)total);

	Section("Counters");
	if (mCounts.empty())
	{
		Line("  (nothing counted)");
	}
	else
	{
		// Anything here that is not zero is something the loader met and did
		// not fully handle. This list is the honest gap report.
		for (auto &kv : mCounts)
			Line("  %-40s %8d", kv.first.c_str(), kv.second);
	}

	Line("");
	Line("%d warning(s)", mWarnings);

	fclose((FILE *)mFile);
	mFile = nullptr;
}

void Log::Write(const char *text)
{
	if (!mFile) return;
	fputs(text, (FILE *)mFile);
	fputc('\n', (FILE *)mFile);
	fflush((FILE *)mFile);   // survive a crash mid-load
}

void Log::Line(const char *fmt, ...)
{
	if (!mFile) return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	Write(buf);
}

void Log::Section(const char *title)
{
	if (!mFile) return;
	Write("");
	char bar[80];
	size_t n = strlen(title);
	if (n > sizeof(bar) - 1) n = sizeof(bar) - 1;
	memset(bar, '-', n);
	bar[n] = 0;
	Write(title);
	Write(bar);
}

void Log::Count(const char *what, int n)
{
	mCounts[what] += n;
}

void Log::Warn(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	mWarnings++;
	if (mFile)
	{
		char out[1100];
		snprintf(out, sizeof(out), "WARNING: %s", buf);
		Write(out);
	}
}

void Log::StageBegin(const char *name)
{
	StageEnd();
	mStage = name ? name : "";
	mStageStart = NowMs();
	// Written immediately, not just timed: if the loader dies mid-stage the
	// last line in the file names the stage it died in.
	Line("[stage] %s", mStage.c_str());
}

void Log::StageEnd()
{
	if (mStage.empty()) return;
	mStages.emplace_back(mStage, NowMs() - mStageStart);
	mStage.clear();
}

Log &TheLog()
{
	static Log instance;
	return instance;
}

} // namespace roth
