// ============================================================================
// MAIN-LOOP HANG WATCHDOG -- the Windows half. See i_hangwatch.h for why.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdint.h>
#include <atomic>
#include <thread>
#include <chrono>

#include "i_hangwatch.h"

namespace
{
	// THE HEARTBEAT. Written by the main thread, read by the watcher, and that is
	// the whole synchronisation: a counter that only goes up and a pointer to a
	// string literal. No lock, because a watchdog that can block is a second hang.
	std::atomic<uint64_t>     g_beat{ 0 };
	std::atomic<const char *> g_stage{ "starting up" };

	std::atomic<bool> g_running{ false };
	std::atomic<bool> g_stop{ false };
	std::thread       g_thread;
	DWORD             g_mainThreadId = 0;

	// HOW LONG IS TOO LONG. Generous on purpose: a big level on a cold cache can
	// legitimately hold the loop for several seconds, and a watchdog that cries at
	// a slow load is a watchdog everyone learns to ignore.
	const int kHangSeconds = 12;

	// ONE REPORT PER RUN. The interesting stack is the first one; after that the
	// process is wedged and every later dump says the same thing while suspending
	// threads that are already going nowhere.
	bool g_reported = false;

	// ------------------------------------------------------------------------
	// Symbols. dbghelp reads the PDB that sits beside the exe, which is why this
	// is worth doing at all: the report names functions and source lines rather
	// than bare addresses nobody can act on.
	//
	// SymInitialize may already have been called by the crash reporter. A second
	// call fails harmlessly, so its result is deliberately not checked.
	// ------------------------------------------------------------------------
	void EnsureSymbols()
	{
		static bool once = false;
		if (once) return;
		once = true;
		SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
		SymInitialize(GetCurrentProcess(), nullptr, TRUE);
	}

	void WriteFrameLine(FILE *f, DWORD64 addr)
	{
		HANDLE proc = GetCurrentProcess();

		// A symbol name, if the PDB has one for this address.
		char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)] = {};
		SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buffer);
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = MAX_SYM_NAME;
		DWORD64 symDisp = 0;

		// The module, which is the part that still answers when there is no PDB --
		// and the most useful single word in the report when the frame is inside a
		// driver or the OS (nvoglv64, ntdll, openxr_loader).
		IMAGEHLP_MODULE64 mod = {};
		mod.SizeOfStruct = sizeof(mod);
		const char *modName = SymGetModuleInfo64(proc, addr, &mod) ? mod.ModuleName : "?";

		if (SymFromAddr(proc, addr, &symDisp, sym))
		{
			IMAGEHLP_LINE64 line = {};
			line.SizeOfStruct = sizeof(line);
			DWORD lineDisp = 0;
			if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line))
				fprintf(f, "    %s!%s + 0x%llx        %s:%lu\n", modName, sym->Name,
					(unsigned long long)symDisp, line.FileName, (unsigned long)line.LineNumber);
			else
				fprintf(f, "    %s!%s + 0x%llx\n", modName, sym->Name, (unsigned long long)symDisp);
		}
		else
		{
			fprintf(f, "    %s!0x%llx\n", modName, (unsigned long long)addr);
		}
	}

	// ------------------------------------------------------------------------
	// Walk one thread.
	//
	// SUSPENDED ONE AT A TIME, AND RESUMED BEFORE THE NEXT. Suspending a thread
	// that happens to hold the CRT heap lock, and then allocating in here, would
	// deadlock the watchdog itself -- the one failure this must not have. Holding
	// exactly one thread at a time makes that window as small as it can be while
	// still being able to read a stack at all. It is not zero, and it does not
	// need to be: by the time this runs the process is already lost.
	// ------------------------------------------------------------------------
	void DumpThread(FILE *f, DWORD tid, bool isMain)
	{
		HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, tid);
		if (!h)
		{
			fprintf(f, "\n  thread %lu%s -- could not be opened (error %lu)\n",
				(unsigned long)tid, isMain ? "  <-- MAIN THREAD" : "", (unsigned long)GetLastError());
			return;
		}

		fprintf(f, "\n  thread %lu%s\n", (unsigned long)tid,
			isMain ? "   <<<<<< MAIN THREAD -- THE LOOP THAT STOPPED" : "");

		if (SuspendThread(h) == (DWORD)-1)
		{
			fprintf(f, "    (could not suspend)\n");
			CloseHandle(h);
			return;
		}

		CONTEXT ctx = {};
		ctx.ContextFlags = CONTEXT_FULL;
		if (GetThreadContext(h, &ctx))
		{
			STACKFRAME64 frame = {};
#if defined(_M_X64)
			const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
			frame.AddrPC.Offset = ctx.Rip;
			frame.AddrFrame.Offset = ctx.Rbp;
			frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
			const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
			frame.AddrPC.Offset = ctx.Pc;
			frame.AddrFrame.Offset = ctx.Fp;
			frame.AddrStack.Offset = ctx.Sp;
#else
			const DWORD machine = IMAGE_FILE_MACHINE_I386;
			frame.AddrPC.Offset = ctx.Eip;
			frame.AddrFrame.Offset = ctx.Ebp;
			frame.AddrStack.Offset = ctx.Esp;
#endif
			frame.AddrPC.Mode = AddrModeFlat;
			frame.AddrFrame.Mode = AddrModeFlat;
			frame.AddrStack.Mode = AddrModeFlat;

			// 48 frames is past the point of usefulness; a blocked stack is short.
			for (int i = 0; i < 48; i++)
			{
				if (!StackWalk64(machine, GetCurrentProcess(), h, &frame, &ctx,
						nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
					break;
				if (frame.AddrPC.Offset == 0)
					break;
				WriteFrameLine(f, frame.AddrPC.Offset);
			}
		}
		else
		{
			fprintf(f, "    (could not read context)\n");
		}

		ResumeThread(h);
		CloseHandle(h);
	}

	void DumpAllThreads(uint64_t beat, const char *stage, int stalledSeconds)
	{
		EnsureSymbols();

		// fopen, NOT Printf. A hung process gets killed, and buffered console output
		// dies with it -- which is precisely how a hang hides. The file is closed
		// before this returns, so the answer survives the kill that follows.
		FILE *f = fopen("hang_stack.txt", "at");
		if (!f) return;

		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(f, "\n================================================================\n");
		fprintf(f, "MAIN LOOP STOPPED -- %04u-%02u-%02u %02u:%02u:%02u\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		fprintf(f, "  no heartbeat for %d seconds (beat %llu)\n", stalledSeconds, (unsigned long long)beat);
		fprintf(f, "  the loop was about to run: %s\n", stage ? stage : "?");
		fprintf(f, "  main thread id: %lu\n", (unsigned long)g_mainThreadId);
		fprintf(f, "\nREAD THE MAIN THREAD FIRST. Its top frame is the wait that never\n"
		           "returned. If that frame is a lock or a condition variable, the thread\n"
		           "holding it is in one of the stacks below.\n");
		fprintf(f, "================================================================\n");

		// Every thread in this process. The main thread's stack names the wait; the
		// others name who is holding whatever it waits on, which is the half a
		// single-thread dump always leaves out.
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap != INVALID_HANDLE_VALUE)
		{
			THREADENTRY32 te = {};
			te.dwSize = sizeof(te);
			const DWORD self = GetCurrentThreadId();
			const DWORD pid = GetCurrentProcessId();
			if (Thread32First(snap, &te))
			{
				do
				{
					if (te.th32OwnerProcessID != pid) continue;
					if (te.th32ThreadID == self) continue;      // never suspend the watcher
					DumpThread(f, te.th32ThreadID, te.th32ThreadID == g_mainThreadId);
				} while (Thread32Next(snap, &te));
			}
			CloseHandle(snap);
		}

		fprintf(f, "\n================================================================\n\n");
		fclose(f);
	}

	void WatchLoop()
	{
		uint64_t lastBeat = g_beat.load();
		int      still = 0;

		while (!g_stop.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));

			const uint64_t now = g_beat.load();
			if (now != lastBeat)
			{
				lastBeat = now;
				still = 0;
				continue;
			}

			still++;
			if (still >= kHangSeconds * 2 && !g_reported)
			{
				g_reported = true;
				DumpAllThreads(now, g_stage.load(), kHangSeconds);
			}
		}
	}
}

void I_HangWatchStart()
{
	if (g_running.exchange(true)) return;
	g_mainThreadId = GetCurrentThreadId();
	g_stop.store(false);
	g_thread = std::thread(WatchLoop);
}

void I_HangWatchBeat(const char *stage)
{
	g_stage.store(stage, std::memory_order_relaxed);
	g_beat.fetch_add(1, std::memory_order_relaxed);
}

void I_HangWatchStop()
{
	if (!g_running.exchange(false)) return;
	g_stop.store(true);
	if (g_thread.joinable()) g_thread.join();
}
