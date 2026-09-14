/*
** vk_debrispool.h
**
** [DEBRISPOOL] The debris pool's GPU side: its buffers, the pieces' uploads and the simulation steps.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/COLLISION_DEBRIS_MESH_PLAN.md" #9, "Engine docs/DEBRIS_9_IMPL_NOTES.md". A client of VkComputeManager, beside
** the level field (vk_levelfield.h).
**
** THE BUFFERS, allocated only while DebrisPoolFrame::Active, at the capacity it names (r_debris_pool):
**   - Pieces: capacity x DEBRIS_PIECE_BYTES, device-local, written by shaders/compute/debris_step.comp and by the spawn
**     copies; read by the draw at set 1 binding 10 (vk_descriptorset.cpp).
**   - Definitions: the info vec4, the debris definition table and the pool mesh instance list, host-visible; read by the
**     draw at set 1 binding 11 and by the step.
**   - Events: a frame's blast pushes and wake boxes, host-visible, the step's only.
**   - Staging: a frame's new pieces, host-visible, copied into Pieces.
** A capacity this device cannot allocate, or a step program that does not build, logs one line and refuses: SpawnParticles
** then writes those definitions' bursts to the ring (DebrisPool::Takes), exactly as without the pool.
**
** A FRAME (Run), in DebrisPoolFrame's order: allocate or free; Clear; the definitions, the mesh instance list and the info;
** for each step its new pieces, then the step (fx.debrissim, counted per step); then the new pieces for no step. All by
** commands in one stream, so a draw recorded after this frame's compute reads this frame's pieces.
**
** CPU-side decisions -- which bursts, which slots, when to step, what pushes and wakes -- are made in hw_debrispool.cpp and
** arrive in DebrisPoolFrame, so a render rebuild replaces only this file, the step lump and the shaders' pool paths.
**
*/

#pragma once

#include <cstdint>
#include <memory>

#include <zvulkan/vulkanobjects.h>
#include "hw_debrisframe.h"

class VulkanRenderDevice;
class VkComputeManager;
class VkComputeProgram;

class VkDebrisPool
{
public:
	explicit VkDebrisPool(VkComputeManager* compute);
	~VkDebrisPool();

	// One frame: allocate, free, clear, upload and step as the frame says. Records GPU commands only when there is
	// something to do. A frame whose serial it has already acted on is ignored.
	void Run(const DebrisPoolFrame& frame);

	bool IsAllocated() const { return mPieces != nullptr; }
	int GetCapacity() const { return mCapacity; }

	// For set 1 bindings 10 and 11 (VkDescriptorSetManager::UpdateHWBufferSet). Null while not allocated.
	VulkanBuffer* GetPieceBuffer() const { return mPieces.get(); }
	VulkanBuffer* GetDefinitionBuffer() const { return mDefinitions.get(); }

private:
	bool EnsureProgram();
	bool Allocate(int capacity);
	void DestroyNow();
	void Release(const char* why);
	bool MakeStepSet();
	void Dispatch(int mode, int count, int stepIndex, const DebrisPoolFrame& frame, int impulseCount, int wakeCount);
	void CopySpawns(const DebrisPoolFrame& frame, int stepIndex, int spawnCount);

	VkComputeManager* mCompute = nullptr;
	VulkanRenderDevice* fb = nullptr;

	// The program first, then the buffers, then the descriptor set, so on destruction the set (which names the buffers)
	// goes before what it names.
	std::unique_ptr<VkComputeProgram> mStepProgram;
	bool mProgramFailed = false;

	std::unique_ptr<VulkanBuffer> mPieces;
	std::unique_ptr<VulkanBuffer> mDefinitions;
	std::unique_ptr<VulkanBuffer> mEvents;
	std::unique_ptr<VulkanBuffer> mStaging;

	std::unique_ptr<VulkanDescriptorSet> mStepSet;

	int mCapacity = 0;
	uint64_t mLastSerial = 0;

	// A capacity this device refused: not retried until the capacity changes or the pool stops being asked for, so a
	// refusal logs once.
	int mRefusedCapacity = 0;
};
