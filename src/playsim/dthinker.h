/*
** dthinker.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 1998-2016 Marisa Heit
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#ifndef __DTHINKER_H__
#define __DTHINKER_H__

#include <stdlib.h>
#include "dobject.h"
#include "statnums.h"

class AActor;
class player_t;
struct pspdef_s;
struct FState;
class DThinker;
class FSerializer;
struct FLevelLocals;
struct ProfileInfo;

class FThinkerIterator;

enum { MAX_STATNUM = 127 };

// Doubly linked ring list of thinkers
struct FThinkerList
{
	// No destructor. If this list goes away it's the GC's task to clean the orphaned thinkers. Otherwise this may clash with engine shutdown.
	void AddTail(DThinker *thinker);
	void AddHead(DThinker *thinker);					// [SLEEP] for LinkSleeper
	DThinker *GetHead() const;
	DThinker *GetTail() const;
	bool IsEmpty() const;
	void DestroyThinkers();
	bool DoDestroyThinkers(bool& destroyed);
	void RemoveTravellers(bool saveGame);
	void OnLoad();
	int TickThinkers(FThinkerList *dest, int& counter);	// Returns: # of thinkers ticked
	int CheckSleepingThinkers(int ticsElapsed = 1);		// [SLEEP] one tic of a sleep list; returns # woken
	int ProfileThinkers(FThinkerList *dest, int& counter, TMap<FName, ProfileInfo>& profiles);
	void SaveList(FSerializer &arc);

private:
	DThinker *Sentinel = nullptr;

	friend struct FThinkerCollection;
};

struct FThinkerCollection
{
	void DestroyThinkersInList(int statnum)
	{
		Thinkers[statnum].DestroyThinkers();
		FreshThinkers[statnum].DestroyThinkers();
	}

	void RunThinkers(FLevelLocals *Level);	// The level is needed to tick the lights
	// RS FORK -- WORLD CLOCK: worldStep false means the world is between steps, so the
	// client-side thinkers do not tick this time. The timers, the dynamic-light
	// recreation and the light ticks still run every call, because the sound update and
	// the renderer read them every client tic. Defaulted, so an existing caller is
	// unchanged. p_tick.cpp ticks the real-time client-side actors on the skipped tics.
	void RunClientSideThinkers(FLevelLocals* Level, bool worldStep = true);
	void DestroyAllThinkers(bool fullgc = true);
	void CleanUpTravellers(bool saveGame);
	void SerializeThinkers(FSerializer &arc, bool keepPlayers);
	void MarkRoots();
	void OnLoad();
	DThinker *FirstThinker(int statnum);
	void Link(DThinker *thinker, int statnum);
	// [SLEEP] Thinker sleep (GZSelaco): see DThinker::Sleep in dthinker.cpp.
	void LinkSleeper(DThinker *thinker, int statnum);
	void RunSleepCycle();		// once per tic, before anything ticks: count sleepers down, wake the due
	bool IsSleepCycle() const { return inSleepCycle; }
	void AddWaker(DThinker *thinker) { tempWakers.Push(thinker); }

private:
	FThinkerList Thinkers[MAX_STATNUM + 2];
	FThinkerList FreshThinkers[MAX_STATNUM + 1];

	// [SLEEP] While RunSleepCycle walks STAT_SLEEP, a thinker woken by its timer or by a callback is moved back to its
	// list only after the walk, so a ShouldWake or Wake override that wakes others cannot send the walk into another
	// list (GZSelaco aad9387aa0). Raw pointers: the collector does not run inside a tic, and destroyed ones are skipped.
	bool inSleepCycle = false;
	TArray<DThinker*> tempWakers;

	friend class FThinkerIterator;
};

extern bool bTravelling;

class DThinker : public DObject
{
	DECLARE_CLASS (DThinker, DObject)
public:
	static const int DEFAULT_STAT = STAT_DEFAULT;
	void OnDestroy () override;
	virtual ~DThinker ();
	virtual void Tick ();
	void CallTick();
	virtual void PostBeginPlay ();	// Called just before the first tick
	virtual void CallPostBeginPlay(); // different in actor.
	virtual void PostSerialize();
	void CallPostSerialize();
	void Serialize(FSerializer &arc) override;
	size_t PropagateMark();

	void ChangeStatNum (int statnum);
	inline int GetStatNum() const { return _statNum; }
	// This is temporary and should only be used with the rollback functionality.
	inline void RollbackStatNum(int statNum) { _statNum = statNum; }

	// [SLEEP] Thinker sleep, GZSelaco's script API (f6ebcea025 and its fixes; "Engine docs/SELACO_S1_PLAYSIM_IMPL_NOTES.md").
	// A sleeping thinker is not ticked. Sleep(tics) parks it in STAT_SLEEP until its timer runs out and ShouldWake
	// agrees; SleepIndefinite parks it in STAT_SLEEP_FOREVER until Wake. It stays linked into the world, keeps its TID
	// and pointers, and can be found, iterated (in its sleep list) and destroyed. Waking returns it to the list it
	// slept from, and it ticks that same tic. All of it is playsim state, saved with the thinker.
	virtual bool ShouldWake();          // asked when a timed sleep is up; false keeps it asleep, asked again next tic
	virtual void Wake();
	virtual void Sleep(int tics = 10);  // tics <= 0 does nothing (GZSelaco 8a2ca710d3)
	virtual void SleepIndefinite();
	void CallSleep(int tics);
	bool CallShouldWake();
	void CallWake();
	inline bool IsSleeping() const { return sleepInterval != 0; }

private:
	void Remove();

	friend struct FThinkerList;
	friend struct FThinkerCollection;
	friend class FThinkerIterator;
	friend class DObject;
	friend class FDoomSerializer;

	int8_t _statNum = -1;
	// [SLEEP] The statnum list this thinker is linked into right now (FThinkerCollection::Link, LinkSleeper and loading
	// keep it; _statNum is only set by ChangeStatNum), and the list it returns to when it wakes.
	int8_t linkedStatNum = -1;
	int8_t sleepWakeStatNum = -1;
	DThinker *NextThinker = nullptr, *PrevThinker = nullptr;
	// [SLEEP] GZSelaco's sleep fields: the interval (> 0 timed, -1 indefinite, 0 awake) and the countdown.
	int sleepInterval = 0;
	int sleepTimer = 0;
	int SleepReturnStatNum() const;
	FThinkerCollection &OwnCollection() const;

public:
	FLevelLocals *Level;

	friend struct FLevelLocals;	// Needs access to FreshThinkers until the thinker storage gets refactored.
};

class FThinkerIterator
{
protected:
	const PClass *m_ParentType;
private:
	FLevelLocals *Level;
	FThinkerCollection* m_ThinkerPool;
	DThinker *m_CurrThinker;
	uint8_t m_Stat;
	bool m_SearchStats;
	bool m_SearchingFresh;

public:
	FThinkerIterator (FLevelLocals *Level, const PClass *type, int statnum=MAX_STATNUM+1, bool clientside = false);
	FThinkerIterator (FLevelLocals *Level, const PClass *type, int statnum, DThinker *prev, bool clientside = false);
	DThinker *Next (bool exact = false);
	void Reinit ();
};

template <class T> class TThinkerIterator : public FThinkerIterator
{
public:
	TThinkerIterator (FLevelLocals *Level, int statnum=MAX_STATNUM+1, bool clientside = false) : FThinkerIterator (Level, RUNTIME_CLASS(T), statnum, clientside)
	{
	}
	TThinkerIterator (FLevelLocals *Level, int statnum, DThinker *prev, bool clientside = false) : FThinkerIterator (Level, RUNTIME_CLASS(T), statnum, prev, clientside)
	{
	}
	TThinkerIterator (FLevelLocals *Level, const PClass *subclass, int statnum=MAX_STATNUM+1, bool clientside = false) : FThinkerIterator(Level, subclass, statnum, clientside)
	{
	}
	TThinkerIterator (FLevelLocals *Level, FName subclass, int statnum=MAX_STATNUM+1, bool clientside = false) : FThinkerIterator(Level, PClass::FindClass(subclass), statnum, clientside)
	{
	}
	TThinkerIterator (FLevelLocals *Level, FName subclass, int statnum, DThinker *prev, bool clientside = false) : FThinkerIterator(Level, PClass::FindClass(subclass), statnum, prev, clientside)
	{
	}
	T *Next (bool exact = false)
	{
		return static_cast<T *>(FThinkerIterator::Next (exact));
	}
};


#endif //__DTHINKER_H__
