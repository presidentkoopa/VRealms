/*
** hw_debrislanding.h
**
** [DEBRISSOUNDS] Debris landing sounds: when and where a group of debris pieces first lands, and the one sound it makes
** there.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #11 (review D8), "Engine docs/DEBRIS_SOUNDS_11_IMPL_NOTES.md".
**
**   - A GROUP is one SpawnParticles burst of a debris definition (PARTICLEDEFS `restitution`) that names a `landsound`,
**     taken by the debris pool (hw_debrispool.cpp). A group with the same sounds landing within kMergeSeconds of a waiting
**     group, and within kMergeRadius of where that one was first placed, joins it: the chip shapes an impact profile shares
**     its count between, a shotgun's pellets.
**   - ITS FIRST LANDING is PREDICTED on the CPU as the burst goes into the pool; nothing is read back from the GPU. Each
**     piece is flown with debris_step.comp's own step -- gravity, drag, the speed limit, the sweep onto a flat surface, the
**     contact's bounce and friction -- against what the burst knows (SpawnParticles' plane and floor), and the kCandidates
**     that land soonest are kept (GroupBuilder). Where the definition collides with the level, those are traced against the
**     LEVEL in that order (TraceLanding: the lines of each sector the piece passes into, its floor -- slopes included -- and
**     ceiling), as the GPU step flies them on the level field baked from the same lines and planes. A landing is the first
**     step that ends within reach of a floor-like surface while moving into it: the step's own bounce condition.
**   - ONE SOUND starts through the normal sound path at that point, when the draw shows the piece land (the pool is drawn
**     one step behind its step clock): no actor, no thinker. The group's size sets the volume, and past its big count the
**     bigger variant plays instead; the pitch comes from a client-side RNG.
**   - LIMITS: kMaxStartsPerFrame starts a frame; per landing sound, kAreaBurst at once and kAreaRate a second within
**     kAreaRadius (SNDINFO $limit counts each file of a $random on its own, so it cannot cap a $random alone); an event
**     more than kLateSeconds late -- a frame without the 3D view, a stall -- is dropped, never played in a heap.
**   - THE CLOCK is the draw's level time, never going back within a map: a menu that pauses the game freezes it, so
**     nothing comes due while paused and nothing piles up for the unpause.
**
** The prediction, the trace, the group builder and the scheduler (namespace DebrisLanding) use nothing from the engine, so
** they are checked outside it ("Engine docs/DEBRIS_SOUNDS_11_IMPL_NOTES.md": mirror11.py and the C++ harness).
** DebrisLandingSounds is the engine side: the definitions, the level, the cvars and the sound call.
**
** Main thread only. Presentation only: nothing here writes to the playsim, nothing is read back from the GPU, and the only
** RNGs are client-side (pr_debrisland for the pitch; the sound engine's pr_randsound for a $random).
**
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace DebrisLanding
{
	// debris_step.comp's constants that decide a contact. They must agree with the step.
	inline constexpr double kStepSeconds = 1.0 / 35.0;	// TICRATE
	inline constexpr double kContactSlack = 0.05;
	inline constexpr double kFloorUp = 0.7;
	inline constexpr double kBounceSpeed = 24.0;
	inline constexpr double kMaxSpeed = 4096.0;
	inline constexpr double kMinRadius = 0.25;

	// How far ahead a landing is looked for: 3 seconds of steps. A piece still in the air then makes no sound.
	inline constexpr int kMaxSteps = 105;
	// The pieces of a burst that land soonest against its own surfaces, kept; at most kMaxTraced of them traced against the
	// level; at most kMaxLineHops lines passed in one step of a trace, each search starting kHopNudge map units past the last;
	// a move shorter than kMinHorizontal across meets no line.
	inline constexpr int kCandidates = 12;
	inline constexpr int kMaxTraced = 12;
	inline constexpr int kMaxLineHops = 4;
	inline constexpr double kHopNudge = 1.0e-6;
	inline constexpr double kMinHorizontal = 1.0e-9;

	// Groups with the same sounds landing this close in time, and this close to where the waiting one was first placed, are
	// one group.
	inline constexpr double kMergeSeconds = 3.0 / 35.0;
	inline constexpr double kMergeRadius = 64.0;
	// An event later than this (level seconds past due) is dropped.
	inline constexpr double kLateSeconds = 0.1;
	// At most this many landing sounds start in one frame; the rest wait (unless late).
	inline constexpr int kMaxStartsPerFrame = 4;
	// Per landing sound, within kAreaRadius map units: kAreaBurst at once, refilled kAreaRate a second.
	inline constexpr double kAreaRadius = 192.0;
	inline constexpr double kAreaBurst = 3.0;
	inline constexpr double kAreaRate = 6.0;
	inline constexpr int kAreas = 32;
	// Waiting events; past this the newest is dropped.
	inline constexpr int kMaxPending = 256;
	// The volume share of a group smaller than its big count: sqrt(pieces / big count), never under this.
	inline constexpr double kMinVolumeShare = 0.2;
	inline constexpr int kDefaultBigCount = 12;

	struct V3
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};

	inline V3 Add(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline V3 Sub(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline V3 Scale(const V3& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
	inline double Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline double Length(const V3& a) { return std::sqrt(Dot(a, a)); }

	// One piece's flight in SHADER axes (y up: Doom x, z, y), as the pool writes it at spawn, and what its definition and
	// burst give it to collide with.
	struct Flight
	{
		V3 Position;
		V3 Velocity;					// map units a second
		double Gravity = 0.0;			// map units a second squared, down
		double Drag = 0.0;				// 1/s
		double Restitution = 0.0;		// the bounce off a wall or a ceiling (a floor ends the flight)
		double Friction = 0.5;
		double Radius = kMinRadius;		// the step's radius: the sweep stops it this far from a surface
		double Reach = kMinRadius;		// the contact's reach: a billboard's radius; a tumbling mesh's box, on average
		bool HasPlane = false;			// a plane: PlaneNormal . p = PlaneOffset, PlaneNormal unit
		V3 PlaneNormal;
		double PlaneOffset = 0.0;
		bool HasFloor = false;			// a floor height
		double FloorHeight = 0.0;
		int MaxSteps = kMaxSteps;		// a piece that never rests is freed at the end of its life
	};

	struct Landing
	{
		bool Landed = false;
		int Step = 0;			// the step it lands in: 1 is the first step after its spawn
		V3 Point;				// where it meets the surface (shader axes)
		double IntoSpeed = 0.0;	// its speed into the surface
	};

	// The nearest surface to p and its outward normal, as debris_step.comp's DebrisSurface without the level field: the
	// plane, and the floor where that is nearer. False with neither (the step flies such a piece until it falls out).
	inline bool NearestSurface(const Flight& f, const V3& p, double& distance, V3& normal)
	{
		bool found = false;
		distance = 1.0e9;
		normal = { 0.0, 1.0, 0.0 };
		if (f.HasPlane)
		{
			distance = Dot(p, f.PlaneNormal) - f.PlaneOffset;
			normal = f.PlaneNormal;
			found = true;
		}
		if (f.HasFloor)
		{
			const double floorDistance = p.y - f.FloorHeight;
			if (!found || floorDistance < distance)
			{
				distance = floorDistance;
				normal = { 0.0, 1.0, 0.0 };
			}
			found = true;
		}
		return found;
	}

	// debris_step.comp's contact with a surface that is not a floor: the speed into it kept times restitution above the
	// bounce speed, and the sliding speed less friction x the contact's push (Coulomb).
	inline V3 Bounce(const Flight& f, const V3& v, const V3& normal, double normalSpeed)
	{
		const V3 sliding = Sub(v, Scale(normal, normalSpeed));
		const double intoSpeed = -normalSpeed;
		const double bounceFloor = std::max(kBounceSpeed, 1.5 * std::fabs(f.Gravity) * kStepSeconds);
		const double bounceSpeed = intoSpeed > bounceFloor ? intoSpeed * std::clamp(f.Restitution, 0.0, 1.0) : 0.0;
		const double slidingSpeed = Length(sliding);
		const double kept = slidingSpeed > 1.0e-4 ?
			std::max(slidingSpeed - std::clamp(f.Friction, 0.0, 1.0) * (intoSpeed + bounceSpeed), 0.0) / slidingSpeed : 0.0;
		return Add(Scale(sliding, kept), Scale(normal, bounceSpeed));
	}

	// The first step in which the piece lands on a floor-like surface of its own plane and floor, looking at most maxSteps
	// (and f.MaxSteps) ahead.
	inline Landing PredictFirstLanding(const Flight& f, int maxSteps)
	{
		Landing out;
		V3 p = f.Position;
		V3 v = f.Velocity;
		double distance = 0.0;
		V3 normal;
		if (!NearestSurface(f, p, distance, normal))
			return out;
		const double decay = std::exp(-std::max(f.Drag, 0.0) * kStepSeconds);
		const int steps = std::min(std::min(maxSteps, f.MaxSteps), kMaxSteps);
		for (int k = 1; k <= steps; k++)
		{
			// FORCES: gravity, then drag, then the speed limit.
			v.y -= f.Gravity * kStepSeconds;
			v = Scale(v, decay);
			const double speed = Length(v);
			if (speed > kMaxSpeed)
				v = Scale(v, kMaxSpeed / speed);

			// THE SWEEP against a flat surface: in open air, or moving away, the whole move; closing in, onto its radius from
			// the surface and the rest of the move along it (the step's closing and sliding branches).
			const V3 move = Scale(v, kStepSeconds);
			NearestSurface(f, p, distance, normal);
			const double across = Dot(move, normal);
			if (across < 0.0 && distance + across < f.Radius)
			{
				const double share = distance > f.Radius ? std::clamp((distance - f.Radius) / -across, 0.0, 1.0) : 0.0;
				p = Add(p, Scale(move, share));
				V3 rest = Scale(move, 1.0 - share);
				rest = Sub(rest, Scale(normal, Dot(rest, normal)));
				p = Add(p, rest);
			}
			else
			{
				p = Add(p, move);
			}

			// THE CONTACT: within its reach and the slack it touches; not moving away, it is put on the surface.
			if (NearestSurface(f, p, distance, normal) && distance < f.Reach + kContactSlack)
			{
				const double normalSpeed = Dot(v, normal);
				if (distance < f.Reach || normalSpeed <= 0.0)
					p = Add(p, Scale(normal, f.Reach - distance));
				if (normalSpeed < 0.0)
				{
					if (normal.y > kFloorUp)
					{
						out.Landed = true;
						out.Step = k;
						out.IntoSpeed = -normalSpeed;
						out.Point = Sub(p, Scale(normal, f.Reach));
						return out;
					}
					v = Bounce(f, v, normal, normalSpeed);
				}
			}
		}
		return out;
	}

	// One line a trace meets (the query fills it).
	struct Crossing
	{
		double Share = 1.0;			// how far along the searched segment, 0..1
		intptr_t Beyond = -1;		// the sector on the other side (the query's own handle); -1 = none, a wall
		double FloorBeyond = 0.0;	// that sector's floor and ceiling at the crossing
		double CeilingBeyond = 0.0;
		double NormalX = 0.0;		// the line's unit normal (Doom x, y), facing the side the segment starts on
		double NormalY = 0.0;
		double Offset = 0.0;		// NormalX * x + NormalY * y on the line
	};

	// A candidate flown against the LEVEL, as the GPU step flies a `collide = level` piece on the level field, which is baked
	// from the same lines and sector planes; the level stands in for SpawnParticles' plane and floor. The query answers, in
	// Doom axes (x, y across, z up), with sectors as its own non-negative handles:
	//   intptr_t SectorAt(double x, double y)                           the sector a point is in, -1 none
	//   double FloorAt(intptr_t sector, double x, double y, V3& normal)   its floor's height there, and the unit normal (up)
	//   double CeilingAt(intptr_t sector, double x, double y, V3& normal) its ceiling's, and the unit normal (down)
	//   bool FirstCrossing(intptr_t sector, double x0, double y0, double x1, double y1, Crossing& crossing)
	//                                                                   the first of that sector's lines the segment crosses
	// Each step: the forces; the move, its centre carried through each line into the sector beyond while it fits the opening
	// there (its height between that floor and ceiling, the opening wider than twice its reach) -- else, at a line it does not
	// fit (a wall, a closed door, a step up) and that lies within its reach of the path, stopped its reach short of it, put its
	// reach off it, the rest of the move along it, with the wall's bounce (the step touches a surface within its reach); then
	// the floor or ceiling of the sector it is in, whichever is nearer, as the step's contact. False when the spawn is in no
	// sector.
	template<class Query>
	inline bool TraceLanding(const Flight& f, Query& query, Landing& out)
	{
		out = Landing();
		intptr_t sector = query.SectorAt(f.Position.x, f.Position.z);
		if (sector < 0)
			return false;
		V3 p = f.Position;
		V3 v = f.Velocity;
		const double decay = std::exp(-std::max(f.Drag, 0.0) * kStepSeconds);
		const int steps = std::min(f.MaxSteps, kMaxSteps);
		for (int k = 1; k <= steps; k++)
		{
			v.y -= f.Gravity * kStepSeconds;
			v = Scale(v, decay);
			const double speed = Length(v);
			if (speed > kMaxSpeed)
				v = Scale(v, kMaxSpeed / speed);
			const V3 move = Scale(v, kStepSeconds);

			// THE LINES on the way, searched along the move and its reach past the move's end (map units along the ground).
			const V3 start = p;
			const double horizontal = std::sqrt(move.x * move.x + move.z * move.z);
			bool stopped = false;
			if (horizontal > kMinHorizontal)
			{
				const double ux = move.x / horizontal;
				const double uz = move.z / horizontal;
				const double span = horizontal + f.Reach;
				double from = 0.0;
				for (int hop = 0; hop < kMaxLineHops && from < span; hop++)
				{
					Crossing crossing;
					if (!query.FirstCrossing(sector, start.x + ux * from, start.z + uz * from, start.x + ux * span, start.z + uz * span, crossing))
						break;
					const double at = from + (span - from) * crossing.Share;
					const double centreShare = at / horizontal;
					const double height = start.y + move.y * centreShare;
					const bool fits = crossing.Beyond >= 0 && height >= crossing.FloorBeyond && height <= crossing.CeilingBeyond &&
						crossing.CeilingBeyond - crossing.FloorBeyond > 2.0 * f.Reach;
					if (fits)
					{
						if (centreShare > 1.0)
							break;	// the centre stops short of it this step
						sector = crossing.Beyond;
						from = at + kHopNudge;
						continue;
					}
					const double share = std::max(at - f.Reach, 0.0) / horizontal;
					const V3 wall = { crossing.NormalX, 0.0, crossing.NormalY };
					p = Add(start, Scale(move, share));
					p = Add(p, Scale(wall, f.Reach - (Dot(p, wall) - crossing.Offset)));
					V3 rest = Scale(move, 1.0 - share);
					rest = Sub(rest, Scale(wall, Dot(rest, wall)));
					p = Add(p, rest);
					const double normalSpeed = Dot(v, wall);
					if (normalSpeed < 0.0)
						v = Bounce(f, v, wall, normalSpeed);
					stopped = true;
					break;
				}
			}
			if (!stopped)
				p = Add(start, move);

			// THE FLOOR AND THE CEILING of the sector it is in: the nearer one is the contact.
			V3 floorNormal;
			const double floorHeight = query.FloorAt(sector, p.x, p.z, floorNormal);
			V3 ceilingNormal;
			const double ceilingHeight = query.CeilingAt(sector, p.x, p.z, ceilingNormal);
			const V3 up = { floorNormal.x, floorNormal.z, floorNormal.y };
			const V3 down = { ceilingNormal.x, ceilingNormal.z, ceilingNormal.y };
			const double floorDistance = (p.y - floorHeight) * up.y;
			const double ceilingDistance = (p.y - ceilingHeight) * down.y;
			const bool floorNearer = floorDistance <= ceilingDistance;
			const double distance = floorNearer ? floorDistance : ceilingDistance;
			const V3 normal = floorNearer ? up : down;
			if (distance < f.Reach + kContactSlack)
			{
				const double normalSpeed = Dot(v, normal);
				if (distance < f.Reach || normalSpeed <= 0.0)
					p = Add(p, Scale(normal, f.Reach - distance));
				if (normalSpeed < 0.0)
				{
					if (normal.y > kFloorUp)
					{
						out.Landed = true;
						out.Step = k;
						out.IntoSpeed = -normalSpeed;
						out.Point = Sub(p, Scale(normal, f.Reach));
						return true;
					}
					v = Bounce(f, v, normal, normalSpeed);
				}
			}
		}
		return true;
	}

	struct Candidate
	{
		Flight Piece;
		Landing First;		// against the burst's own plane and floor
	};

	// A burst's pieces in, the kCandidates that land soonest against the burst's own surfaces kept, soonest first (ties: the
	// earlier piece).
	class GroupBuilder
	{
	public:
		void Begin(const Flight& shared)
		{
			mShared = shared;
			mCount = 0;
			mPieces = 0;
		}

		void AddPiece(const V3& position, const V3& velocity, int maxSteps)
		{
			mPieces++;
			Flight f = mShared;
			f.Position = position;
			f.Velocity = velocity;
			f.MaxSteps = std::clamp(maxSteps, 0, kMaxSteps);
			// Only a landing sooner than the latest one kept can matter.
			const int horizon = mCount < kCandidates ? f.MaxSteps : mBest[mCount - 1].First.Step - 1;
			if (horizon < 1)
				return;
			const Landing landing = PredictFirstLanding(f, horizon);
			if (!landing.Landed)
				return;
			int at = mCount < kCandidates ? mCount : kCandidates - 1;
			while (at > 0 && mBest[at - 1].First.Step > landing.Step)
			{
				mBest[at] = mBest[at - 1];
				at--;
			}
			mBest[at].Piece = f;
			mBest[at].First = landing;
			if (mCount < kCandidates)
				mCount++;
		}

		int Count() const { return mCount; }
		int Pieces() const { return mPieces; }
		const Candidate& Get(int i) const { return mBest[i]; }

	private:
		Flight mShared;
		Candidate mBest[kCandidates];
		int mCount = 0;
		int mPieces = 0;
	};

	// The group's first landing. Without the level (`level` null: the definition collides with SpawnParticles' plane only),
	// the soonest candidate's. With it, every kept candidate is traced (at most kMaxTraced) and the soonest traced landing
	// wins: the level can bring a landing sooner (a raised floor, a ramp) or later (a pit, a wall) than the burst's own
	// surfaces said, so their order is not trusted. False when none lands; `traced` says how many were traced.
	template<class Query>
	inline bool GroupFirstLanding(const GroupBuilder& builder, Query* level, Landing& first, int& traced)
	{
		bool have = false;
		traced = 0;
		for (int i = 0; i < builder.Count(); i++)
		{
			const Candidate& candidate = builder.Get(i);
			Landing landing = candidate.First;
			if (level != nullptr)
			{
				if (traced >= kMaxTraced)
					break;
				Landing trace;
				if (TraceLanding(candidate.Piece, *level, trace))
					landing = trace;
				traced++;
			}
			if (landing.Landed && (!have || landing.Step < first.Step))
			{
				first = landing;
				have = true;
			}
			if (level == nullptr && have)
				break;	// against its own plane and floor, the soonest candidate is the group's
		}
		return have;
	}

	// One group's landing sound, waiting for its time. Points in DOOM axes.
	struct Event
	{
		double Time = 0.0;			// level seconds it is heard
		V3 Point;					// where it is heard
		V3 Anchor;					// where it was first placed (Scheduler::Add sets it): later groups join within kMergeRadius of it
		int Pieces = 0;
		int Sound = 0;				// sound index (> 0)
		int BigSound = 0;			// the bigger variant's, 0 = none
		int BigCount = kDefaultBigCount;
		double Volume = 1.0;		// the definition's landvolume
		double PitchMin = 1.0;
		double PitchMax = 1.0;
	};

	// The share of its volume a group of `pieces` gets: 1 at the big count or more, else sqrt(pieces / bigCount), never
	// under kMinVolumeShare.
	inline double VolumeShare(int pieces, int bigCount)
	{
		const int big = std::max(bigCount, 1);
		if (pieces >= big)
			return 1.0;
		return std::max(std::sqrt((double)std::max(pieces, 1) / (double)big), kMinVolumeShare);
	}

	// Whether a group plays its bigger variant.
	inline bool PlaysBig(const Event& e)
	{
		return e.BigSound > 0 && e.Pieces > e.BigCount;
	}

	class Scheduler
	{
	public:
		struct Stats
		{
			uint64_t Groups = 0;		// events added
			uint64_t Merged = 0;		// of those, joined to a waiting one
			uint64_t Started = 0;		// handed to the sound call
			uint64_t Late = 0;			// dropped: more than kLateSeconds late
			uint64_t Limited = 0;		// dropped: the area's tokens were used up
			uint64_t Full = 0;			// dropped: kMaxPending were waiting
			uint64_t Waited = 0;		// frames an event due waited for the per-frame limit
		};

		void Clear()
		{
			mPending.clear();
			for (Area& area : mAreas)
				area = Area();
		}

		// A group's event: joined to a waiting one with the same sounds that lands within kMergeSeconds and was first placed
		// within kMergeRadius, else waiting on its own. False when it was dropped (the queue is full).
		bool Add(const Event& e)
		{
			mStats.Groups++;
			for (Event& waiting : mPending)
			{
				if (waiting.Sound != e.Sound || waiting.BigSound != e.BigSound)
					continue;
				if (std::fabs(waiting.Time - e.Time) > kMergeSeconds + 1.0e-9)
					continue;
				if (DistanceSquared(waiting.Anchor, e.Point) > kMergeRadius * kMergeRadius)
					continue;
				waiting.Pieces += e.Pieces;
				if (e.Time < waiting.Time)
				{
					waiting.Time = e.Time;
					waiting.Point = e.Point;
				}
				waiting.Volume = std::max(waiting.Volume, e.Volume);
				waiting.BigCount = std::min(waiting.BigCount, e.BigCount);
				mStats.Merged++;
				return true;
			}
			if ((int)mPending.size() >= kMaxPending)
			{
				mStats.Full++;
				return false;
			}
			mPending.push_back(e);
			mPending.back().Anchor = e.Point;
			return true;
		}

		// The events due at `now` (level seconds), earliest first: late ones dropped, then up to kMaxStartsPerFrame played
		// through play(const Event&) while their area has a token (else dropped); the rest wait. Returns the starts.
		template<class Play>
		int Update(double now, Play&& play)
		{
			if (mPending.empty())
				return 0;
			mDue.clear();
			for (size_t i = 0; i < mPending.size(); i++)
			{
				if (mPending[i].Time <= now)
					mDue.push_back((int)i);
			}
			if (mDue.empty())
				return 0;
			std::sort(mDue.begin(), mDue.end(), [&](int a, int b)
			{
				return mPending[a].Time < mPending[b].Time || (mPending[a].Time == mPending[b].Time && a < b);
			});
			int starts = 0;
			mDone.assign(mPending.size(), 0);
			for (int index : mDue)
			{
				const Event& e = mPending[index];
				if (now - e.Time > kLateSeconds)
				{
					mStats.Late++;
					mDone[index] = 1;
					continue;
				}
				if (starts >= kMaxStartsPerFrame)
				{
					mStats.Waited++;
					continue;
				}
				mDone[index] = 1;
				if (!TakeToken(e, now))
				{
					mStats.Limited++;
					continue;
				}
				play(e);
				mStats.Started++;
				starts++;
			}
			size_t kept = 0;
			for (size_t i = 0; i < mPending.size(); i++)
			{
				if (!mDone[i])
					mPending[kept++] = mPending[i];
			}
			mPending.resize(kept);
			return starts;
		}

		size_t Waiting() const { return mPending.size(); }
		const Stats& GetStats() const { return mStats; }

	private:
		struct Area
		{
			int Sound = 0;			// 0 = unused
			V3 Point;
			double Tokens = 0.0;
			double Time = 0.0;
		};

		static double DistanceSquared(const V3& a, const V3& b)
		{
			const V3 d = Sub(a, b);
			return Dot(d, d);
		}

		// The landing sound's area around e: a token taken, or false when they are used up. A new area starts full; with all
		// kAreas in use, the one used least recently (an unused one first) is replaced.
		bool TakeToken(const Event& e, double now)
		{
			int found = -1;
			int replace = 0;
			for (int i = 0; i < kAreas; i++)
			{
				const Area& area = mAreas[i];
				if (area.Sound == e.Sound && DistanceSquared(area.Point, e.Point) <= kAreaRadius * kAreaRadius)
				{
					found = i;
					break;
				}
				const Area& worst = mAreas[replace];
				if (worst.Sound != 0 && (area.Sound == 0 || area.Time < worst.Time))
					replace = i;
			}
			if (found < 0)
			{
				Area& area = mAreas[replace];
				area.Sound = e.Sound;
				area.Point = e.Point;
				area.Tokens = kAreaBurst;
				area.Time = now;
				found = replace;
			}
			Area& area = mAreas[found];
			area.Tokens = std::min(kAreaBurst, area.Tokens + std::max(now - area.Time, 0.0) * kAreaRate);
			area.Time = now;
			if (area.Tokens < 1.0)
				return false;
			area.Tokens -= 1.0;
			return true;
		}

		std::vector<Event> mPending;
		std::vector<int> mDue;
		std::vector<uint8_t> mDone;
		Area mAreas[kAreas];
		Stats mStats;
	};
}

#ifndef DEBRISLANDING_MATH_ONLY

#include "hw_debrisframe.h"
#include "zstring.h"

struct FLevelLocals;
struct FDebrisBurstEvent;

// [DEBRISSOUNDS] The engine side, owned by the debris pool (DebrisPool::mLanding) and driven from its frame.
class DebrisLandingSounds
{
public:
	// Every frame before any burst goes in (DebrisPool::PrepareFrame): the definitions' landing sounds, resolved to sound ids
	// again when the debris list or the map changed.
	void SyncDefinitions(uint64_t levelSerial);

	// A burst going into the pool (DebrisPool::ExpandBurst): true when its definition makes a landing sound; then each
	// piece's spawn record (shader axes: a xyz position, w birth; b xyz velocity, w life), then EndBurst with the pieces
	// that went in.
	bool BeginBurst(FLevelLocals* Level, const FDebrisBurstEvent& burst, int tic, const DebrisDefinitionGpu& definition, float sizeTuning);
	void AddPiece(const float* spawnA, const float* spawnB);
	void EndBurst(int pieces);

	// Every frame after the bursts: the sounds due on the draw's level clock. `simulating`: this machine's pool takes debris
	// now (otherwise every waiting event is dropped -- its pieces are not drawn).
	void Update(FLevelLocals* Level, int maptime, double ticFrac, bool simulating);

	// A new map, a savegame load, ClearGpuParticles: every waiting event dropped, the clock restarted.
	void Clear();

	// For `particles` (DebrisPool::Report).
	FString Report() const;

private:
	struct SlotSound
	{
		int Sound = 0;			// resolved sound index, 0 = this slot makes no landing sound
		int BigSound = 0;
		int BigCount = DebrisLanding::kDefaultBigCount;
		double Volume = 1.0;
		double PitchMin = 1.0;
		double PitchMax = 1.0;
	};

	SlotSound mSlots[DEBRIS_DEFINITION_SLOTS];
	int mSoundSlots = 0;
	uint64_t mDebrisListSeen = 0;
	uint64_t mLevelSerialSeen = 0;
	bool mSynced = false;
	uint64_t mMissingLoggedGeneration = 0;

	// The burst being built.
	DebrisLanding::GroupBuilder mBuilder;
	FLevelLocals* mBurstLevel = nullptr;
	int mBurstSlot = -1;
	double mBurstBirth = 0.0;
	double mBurstRestLife = 0.0;
	bool mBurstLevelCollide = false;
	uint64_t mBurstStartNs = 0;

	DebrisLanding::Scheduler mScheduler;
	double mClock = 0.0;
	bool mClockValid = false;
	uint64_t mCostNs = 0;
	bool mFullLogged = false;
	uint64_t mPredicted = 0;
	uint64_t mTraced = 0;
};

#endif
