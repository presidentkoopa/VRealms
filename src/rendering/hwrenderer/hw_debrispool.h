/*
** hw_debrispool.h
**
** [DEBRISPOOL] The debris pool's CPU side: which bursts go in, which slots they take, when the GPU steps,
** what pushes and wakes the pieces, and the two draws.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #9, "Engine docs/DEBRIS_9_IMPL_NOTES.md". Everything here survives a
** render rebuild: it reads the level and fills DebrisPoolFrame (hw_debrisframe.h); the GPU side (vk_debrispool.cpp)
** acts on it, and shaders/compute/debris_step.comp moves the pieces.
**
**   - WHAT GOES IN: a SpawnParticles burst of a definition with `restitution` (FLevelLocals::QueueDebrisBurst, the
**     DebrisBursts queue), while Takes() says the pool can draw it. Each burst is expanded into pieces here with
**     SpawnParticles' own maths (the cone or disc, the jitters, the seed), so a piece starts exactly where a ring
**     record would.
**   - SLOTS: the used slots [0, high water) in a min-heap by the time each piece's stay ends -- birth + life + its
**     scaled rest + its fade. A new piece takes the slot at the heap's top when that stay has ended; otherwise a
**     never-used slot, so the used range grows only to the most pieces alive at once; and with every slot in use,
**     the piece with the least time left (for equal rests, the oldest). Nothing is read back from the GPU: a piece
**     the GPU freed early just leaves its slot idle until then. "Debris time on the ground" (r_debris_life) re-keys
**     the heap.
**   - STEPS: one per world tic, at most DEBRIS_STEPS_PER_FRAME a frame, on the smoke volume's clock. A burst's
**     pieces go in before the step of the tic it was spawned in.
**   - PUSHES AND WAKES: SH3's PushEffectImpulse queue (the one the smoke reads, from this side's own cursor), and
**     SH2's moved sectors under the area the pieces were spawned over (a door, a lift) as wake boxes.
**   - DRAWS: the billboards inside the particle ring's draw (same effect, blend and depth), from a quad buffer whose
**     vertices say "pool" (aParticle.w = 1); the meshes after the ring's mesh draw, one instanced draw per mesh
**     definition drawn as a mesh this frame (MeshParticleBuffer::GetBillboardHidden).
**   - LANDING SOUNDS ([DEBRISSOUNDS], hw_debrislanding.h): a burst of a definition with `landsound` is also flown on
**     the CPU as it goes in, and its group makes one sound where and when its first piece is drawn landing.
**
** Main thread only. Presentation only: nothing here writes to the playsim, nothing is read back from the GPU, and no
** RNG is used (a piece's jitter is SpawnParticles' GpuParticleHash of its burst's seed).
**
*/

#pragma once

#include <cstdint>
#include <vector>

#include "hw_debrisframe.h"
#include "hw_debrislanding.h"	// [DEBRISSOUNDS] DebrisLandingSounds
#include "zstring.h"

struct FLevelLocals;
class FRenderState;
class IVertexBuffer;
struct FDebrisBurstEvent;

class DebrisPool
{
public:
	// The pool lingers this long (level seconds) after its last burst with no piece left alive, then is freed.
	static constexpr int LINGER_SECONDS = 60;
	// Pieces expanded but not yet in the pool (the pool not allocated yet, or more than a frame's spawns); past this
	// the newest bursts' pieces are dropped (once per map in the log).
	static constexpr int MAX_PENDING_PIECES = 65536;
	// Added to every piece's stay: frames already recorded can draw it a little later than its end.
	static constexpr double STAY_MARGIN_SECONDS = 0.25;
	// SH2 is polled over the box of the live pieces' spawn points grown by this (map units): as far as a piece is
	// expected to travel from where it was spawned.
	static constexpr double POLL_MARGIN = 1024.0;

	static DebrisPool& Get();

	// Once per frame for the main view, after LevelField::PrepareFrame (hw_entrypoint.cpp's PrepareFrameCompute, right
	// before RunFrameCompute). Fills DebrisPoolFrameForBackend().
	void PrepareFrame(FLevelLocals* Level, uint64_t levelSerial);

	// SpawnParticles' question (DebrisPoolTakes): does this machine's pool take a burst of this named definition slot
	// now? Its definition is debris (or `collide = level` while r_debris_test is on), Vulkan, r_gpuparticles, r_debris,
	// the pool not refused, and PrepareFrame ran within the last second (so a build without the frame hook, a
	// minimised window or a renderer change sends every burst to the ring, as before).
	bool Takes(int definitionSlot) const;

	// The level collision field's question (hw_levelfield.cpp's demand): pieces that collide with the level are alive or
	// waiting to go in.
	bool WantsLevelField() const;

	// The draw's gates, at the draw's level time (hw_drawinfo.cpp): a piece whose definition needs the premultiplied
	// blend / the view lights / its own soft distance may be alive; billboards or meshes may need drawing.
	bool OccludersAliveAt(float levelTime) const { return mOccludersUntil > levelTime; }
	bool LitAliveAt(float levelTime) const { return mLitUntil > levelTime; }
	bool SoftAliveAt(float levelTime) const { return mSoftUntil > levelTime; }
	bool HasBillboardsAt(float levelTime) const;
	bool HasMeshesAt(float levelTime) const;

	// Inside the particle ring's draw in RenderTranslucent, with its effect, blend and depth set: the pool's quad
	// buffer and one Draw of every slot used. Leaves the quad buffer bound; the ring's block restores the vertex data.
	void DrawBillboards(FRenderState& state, float levelTime);

	// After the ring's mesh draw in DrawScene: sets what it needs, as MeshParticleBuffer::Draw does, and leaves
	// EFF_NONE, no culling and the flat vertex buffer; the caller restores its render style and depth function.
	void DrawMeshes(FRenderState& state, float levelTime);

	// For `particles` (particledefs.cpp, DebrisPoolReport).
	FString Report() const;

private:
	struct QueueCursor
	{
		uint64_t Serial = 0;
		int Count = 0;
	};

	struct SlotState
	{
		double Stay = -1.0;			// level seconds the piece's stay ends at the current rest scale
		double FixedEnd = 0.0;		// birth + life + fade + margin
		float ScaledRest = 0.f;		// a rest of DEBRIS_REST_SCALE_FROM or more (scaled by r_debris_life)
		float FixedRest = 0.f;		// a shorter rest (as written)
		float SpawnX = 0.f;			// where it was spawned, map x and y (the SH2 poll box)
		float SpawnY = 0.f;
		int Definition = -1;
		bool Mesh = false;
	};

	struct PendingPiece
	{
		DebrisPieceGpu Piece;
		int Tic = 0;
		int Definition = 0;
	};

	struct PendingImpulse
	{
		DebrisImpulseGpu Impulse = {};
		int Tic = 0;
	};

	void Reset();
	void ReadQueues(FLevelLocals* Level, bool keep);
	void ExpandBurst(FLevelLocals* Level, const FDebrisBurstEvent& burst, int tic);
	void SyncDefinitions(bool test);
	double StayEnd(const SlotState& slot) const;
	void RebuildHeap();
	void SiftDown(size_t position);
	void SiftUp(size_t position);
	bool HeapLess(uint32_t a, uint32_t b) const;
	void PlacePending(int maptime, int steps);
	void GatherImpulses(int maptime, int steps);
	void GatherWakes(FLevelLocals* Level, int maptime, int steps);
	void RefreshSpawnBox(double now);
	void BuildMeshInstances(double now);
	void EnsureQuads(int capacity);
	bool EnsureDrawModel(int meshIndex);

	// Frame and demand.
	uint64_t mLevelSerial = 0;
	uint64_t mClearSerial = 0;
	uint64_t mEpoch = 0;			// the backend allocation the slot record describes (0: none)
	uint64_t mFrameSerial = 0;
	uint64_t mPreparedMs = 0;
	bool mHasDemand = false;
	int mLastDemandTime = 0;
	bool mClockValid = false;
	int mLastStepTime = 0;
	bool mNeedClear = true;
	int mCapacity = 0;				// the capacity the slot record describes (0: none yet)
	float mRestScale = 1.f;
	bool mLevelDemand = false;

	QueueCursor mBurstCursor;
	QueueCursor mImpulseCursor;

	// Slots: [0, mHighWater) have been used since the last Reset, and exactly those are in the heap.
	std::vector<SlotState> mSlots;
	std::vector<uint32_t> mHeap;
	std::vector<uint32_t> mHeapPosition;
	int mHighWater = 0;
	double mSpawnMin[2] = { 0.0, 0.0 };	// the box of the live pieces' spawn points, map units (x, y)
	double mSpawnMax[2] = { 0.0, 0.0 };
	bool mSpawnBoxValid = false;
	double mBoxRefreshTime = 0.0;

	// This frame's and the waiting hand-over.
	std::vector<PendingPiece> mPending;
	std::vector<PendingImpulse> mPendingImpulses;
	std::vector<DebrisSpawnUpload> mSpawns;
	std::vector<DebrisImpulseGpu> mImpulses;
	std::vector<DebrisWakeGpu> mWakes;
	bool mWakeAll[DEBRIS_STEPS_PER_FRAME] = {};

	// The definition table on the GPU.
	DebrisDefinitionGpu mDefinitions[DEBRIS_DEFINITION_SLOTS] = {};
	uint64_t mDefinitionGeneration = 0;
	uint64_t mDefinitionTableSeen = 0;
	uint64_t mDebrisListSeen = 0;
	uint64_t mMeshListSeen = 0;
	int mTestSeen = -1;
	bool mIsMeshSlot[DEBRIS_DEFINITION_SLOTS] = {};

	// Mesh instances.
	std::vector<uint32_t> mMeshInstances;
	int mMeshFirst[DEBRIS_DEFINITION_SLOTS] = {};
	int mMeshCount[DEBRIS_DEFINITION_SLOTS] = {};
	uint64_t mMeshInstanceGeneration = 0;
	uint64_t mMeshInstancesSent = 0;
	bool mMeshListDirty = false;
	double mMeshListExpiry = 1.0e30;	// the soonest a listed piece's stay ends: the list is rebuilt then
	int mDrawModelIndex[DEBRIS_DEFINITION_SLOTS] = {};

	// The draw's gates.
	static constexpr float NEVER_ALIVE = -1.0e30f;
	float mAliveUntil = NEVER_ALIVE;
	float mMeshesUntil = NEVER_ALIVE;
	float mOccludersUntil = NEVER_ALIVE;
	float mLitUntil = NEVER_ALIVE;
	float mSoftUntil = NEVER_ALIVE;
	float mLevelUntil = NEVER_ALIVE;

	// [DEBRISSOUNDS] The groups' landing sounds: fed by ExpandBurst, played from PrepareFrame.
	DebrisLandingSounds mLanding;

	IVertexBuffer* mQuads = nullptr;
	int mQuadCapacity = 0;

	// For the report and the log.
	uint64_t mPiecesSpawned = 0;
	uint64_t mPiecesRecycled = 0;
	uint64_t mPiecesDropped = 0;
	bool mFullLogged = false;
	bool mDropLogged = false;
	bool mRecycleLogged = false;
};
