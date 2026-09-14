/*
** vk_debrispool.cpp
**
** [DEBRISPOOL] The debris pool's GPU side. See the header.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include <algorithm>
#include <cstring>
#include <exception>
#include <vector>

#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkanobjects.h>

#include "vk_debrispool.h"
#include "vk_compute.h"
#include "vk_levelfield.h"
#include "vk_descriptorset.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/textures/vk_samplers.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"	// PPFilterMode / PPWrapMode for the header sampler
#include "hw_debrisframe.h"
#include "hw_perflog.h"
#include "printf.h"
#include "v_text.h"

namespace
{
	// shaders/compute/debris_step.comp, std430: every member an ivec4 / vec4, so the offsets are 0, 16, 32, 48 in both
	// languages (checked from SPIR-V, "Engine docs/DEBRIS_9_IMPL_NOTES.md").
	struct DebrisStepConstants
	{
		int32_t Range[4];	// x the first piece, y how many, z the mode (0 step, 1 clear), w this step's index
		int32_t Lists[4];	// x impulses, y wake boxes, z 1 = wake every resting piece this step, w 0
		float Clock[4];		// x level seconds this step ends at, y seconds a step, z rest scale, w rests this long or more are scaled
		float Tuning[4];	// x r_gpuparticles_sizescale
	};
	static_assert(sizeof(DebrisStepConstants) == 64, "DebrisStepConstants must match debris_step.comp's push constant block (64 bytes)");

	const int MODE_STEP = 0;
	const int MODE_CLEAR = 1;

	const size_t STAGING_BYTES = (size_t)DEBRIS_SPAWNS_PER_FRAME * (size_t)DEBRIS_PIECE_BYTES;

	// debris_step.comp's local_size_x.
	uint32_t Groups(int count)
	{
		return (uint32_t)std::max((count + 255) / 256, 1);
	}

	size_t DefinitionBufferBytes(int capacity)
	{
		return (size_t)DEBRIS_MESH_INSTANCES_OFFSET + (size_t)capacity * (size_t)DEBRIS_MESH_INSTANCE_BYTES;
	}
}

VkDebrisPool::VkDebrisPool(VkComputeManager* compute) : mCompute(compute), fb(compute->GetDevice())
{
	// One step's cost per step, not per frame: some frames step twice, some not at all.
	PerfLog::CountEachRun("fx.debrissim");
}

VkDebrisPool::~VkDebrisPool()
{
	// Members only. At device teardown the GPU is idle (the render device waits for it before anything is destroyed). The
	// CPU side must not believe a pool still exists.
	DebrisPoolStatus() = DebrisPoolBackendStatus();
}

//-----------------------------------------------------------------------------
//
// The frame
//
//-----------------------------------------------------------------------------

void VkDebrisPool::Run(const DebrisPoolFrame& frame)
{
	if (frame.Serial == 0 || frame.Serial == mLastSerial)
		return;
	mLastSerial = frame.Serial;

	DebrisPoolBackendStatus& status = DebrisPoolStatus();

	if (!frame.Active)
	{
		Release("debris no longer asked for");
		mRefusedCapacity = 0;	// asked again later: try again
		status.Allocated = false;
		status.Capacity = 0;
		status.RefusedCapacity = 0;
		status.DefinitionGeneration = 0;
		status.MeshInstanceGeneration = 0;
		return;
	}

	if (IsAllocated() && mCapacity != frame.Capacity)
		Release("the pool's size changed");

	if (!IsAllocated())
	{
		const auto refuse = [&]()
		{
			mRefusedCapacity = frame.Capacity;
			status.Allocated = false;
			status.Capacity = 0;
			status.RefusedCapacity = mRefusedCapacity;
		};
		if (frame.Capacity == mRefusedCapacity)
		{
			refuse();
			return;
		}
		// The program before any memory: a device that cannot build it gets no pool.
		if (!EnsureProgram() || !Allocate(frame.Capacity))
		{
			refuse();
			return;
		}
		static uint64_t epochs = 0;
		status.Epoch = ++epochs;
		// Freshly allocated and cleared: every piece free. What the CPU side sent this frame was decided before it knew of
		// this allocation; it starts from the next frame.
		status.Allocated = true;
		status.Capacity = mCapacity;
		status.RefusedCapacity = 0;
		status.DefinitionGeneration = 0;
		status.MeshInstanceGeneration = 0;
		return;
	}
	status.Allocated = true;
	status.Capacity = mCapacity;
	status.RefusedCapacity = 0;

	// THE HOST-VISIBLE PARTS. Every frame's submissions finish before the next frame records (as the level field's staging
	// buffer relies on), so these are written again in place.
	const int meshInstances = frame.MeshInstances != nullptr ? std::clamp(frame.MeshInstanceCount, 0, mCapacity) : 0;
	{
		uint8_t* data = (uint8_t*)mDefinitions->Map(0, DefinitionBufferBytes(mCapacity));
		if (data != nullptr)
		{
			if (frame.Definitions != nullptr && frame.DefinitionGeneration != status.DefinitionGeneration)
			{
				memcpy(data + DEBRIS_DEFINITIONS_HEADER_BYTES, frame.Definitions, (size_t)DEBRIS_DEFINITION_SLOTS * DEBRIS_DEFINITION_BYTES);
				status.DefinitionGeneration = frame.DefinitionGeneration;
			}
			if (frame.MeshInstanceGeneration != status.MeshInstanceGeneration)
			{
				if (meshInstances > 0)
					memcpy(data + DEBRIS_MESH_INSTANCES_OFFSET, frame.MeshInstances, (size_t)meshInstances * DEBRIS_MESH_INSTANCE_BYTES);
				status.MeshInstanceGeneration = frame.MeshInstanceGeneration;
			}
			// The info vec4, every frame: the draw reads the rest scale live (r_debris_life with a menu open).
			const float info[4] = { frame.RestScale, DEBRIS_REST_SCALE_FROM, (float)mCapacity, (float)meshInstances };
			memcpy(data, info, sizeof(info));
			mDefinitions->Unmap();
		}
	}

	int spawnCount = frame.Spawns != nullptr ? std::clamp(frame.SpawnCount, 0, DEBRIS_SPAWNS_PER_FRAME) : 0;
	if (spawnCount > 0)
	{
		uint8_t* data = (uint8_t*)mStaging->Map(0, (size_t)spawnCount * DEBRIS_PIECE_BYTES);
		if (data != nullptr)
		{
			for (int i = 0; i < spawnCount; i++)
				memcpy(data + (size_t)i * DEBRIS_PIECE_BYTES, &frame.Spawns[i].Piece, DEBRIS_PIECE_BYTES);
			mStaging->Unmap();
		}
		else
		{
			spawnCount = 0;
		}
	}

	const int steps = std::clamp(frame.Steps, 0, DEBRIS_STEPS_PER_FRAME);
	const int impulseCount = frame.Impulses != nullptr ? std::clamp(frame.ImpulseCount, 0, DEBRIS_IMPULSES_PER_FRAME) : 0;
	const int wakeCount = frame.Wakes != nullptr ? std::clamp(frame.WakeCount, 0, DEBRIS_WAKES_PER_FRAME) : 0;
	if (steps > 0 && (impulseCount > 0 || wakeCount > 0))
	{
		uint8_t* data = (uint8_t*)mEvents->Map(0, (size_t)DEBRIS_STEP_EVENT_BYTES);
		if (data != nullptr)
		{
			if (impulseCount > 0)
				memcpy(data, frame.Impulses, (size_t)impulseCount * sizeof(DebrisImpulseGpu));
			if (wakeCount > 0)
				memcpy(data + (size_t)DEBRIS_IMPULSES_PER_FRAME * sizeof(DebrisImpulseGpu), frame.Wakes, (size_t)wakeCount * sizeof(DebrisWakeGpu));
			mEvents->Unmap();
		}
	}

	const int highWater = std::clamp(frame.HighWater, 0, mCapacity);
	const bool stepping = steps > 0 && highWater > 0;
	if ((frame.Clear || stepping) && !MakeStepSet())
		return;

	if (frame.Clear)
		Dispatch(MODE_CLEAR, mCapacity, 0, frame, 0, 0);

	VkCommandBufferManager* commands = fb->GetCommands();
	for (int i = 0; i < steps; i++)
	{
		CopySpawns(frame, i, spawnCount);
		if (highWater > 0)
		{
			mCompute->BeginWork();	// so fx.debrissim nests inside fx.compute
			commands->PushGroup("fx.debrissim");
			Dispatch(MODE_STEP, highWater, i, frame, impulseCount, wakeCount);
			commands->PopGroup();
		}
	}
	// The pieces for no step this frame (none ran): they are stepped from the next.
	CopySpawns(frame, -1, spawnCount);
}

//-----------------------------------------------------------------------------
//
// Buffers
//
//-----------------------------------------------------------------------------

bool VkDebrisPool::Allocate(int capacity)
{
	VulkanDevice* device = fb->device.get();
	if (capacity < DEBRIS_POOL_MIN || capacity > DEBRIS_POOL_MAX)
	{
		Printf(TEXTCOLOR_RED "DebrisPool: a pool of %d pieces is outside %d .. %d -- debris is drawn as ring particles\n", capacity, DEBRIS_POOL_MIN, DEBRIS_POOL_MAX);
		return false;
	}

	const size_t pieceBytes = (size_t)capacity * DEBRIS_PIECE_BYTES;
	const size_t definitionBytes = DefinitionBufferBytes(capacity);
	bool created = true;
	try
	{
		mPieces = BufferBuilder()
			.Size(pieceBytes)
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY)
			.DebugName("DebrisPool.Pieces")
			.Create(device);
		mDefinitions = BufferBuilder()
			.Size(definitionBytes)
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			.DebugName("DebrisPool.Definitions")
			.Create(device);
		mEvents = BufferBuilder()
			.Size((size_t)DEBRIS_STEP_EVENT_BYTES)
			.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			.DebugName("DebrisPool.Events")
			.Create(device);
		mStaging = BufferBuilder()
			.Size(STAGING_BYTES)
			.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
			.DebugName("DebrisPool.Staging")
			.Create(device);
		created = mPieces != nullptr && mDefinitions != nullptr && mEvents != nullptr && mStaging != nullptr;
	}
	catch (const std::exception& e)
	{
		Printf(TEXTCOLOR_RED "DebrisPool: %s\n", e.what());
		created = false;
	}
	if (!created)
	{
		DestroyNow();
		Printf(TEXTCOLOR_RED "DebrisPool: could not allocate %d pieces (%llu bytes) -- out of video memory? Debris is drawn as ring particles at this size\n",
			capacity, (unsigned long long)pieceBytes);
		return false;
	}
	mCapacity = capacity;

	// The host-visible buffers start zeroed: no definition is debris, no instance, no event.
	for (VulkanBuffer* buffer : { mDefinitions.get(), mEvents.get() })
	{
		void* data = buffer->Map(0, buffer->size);
		if (data != nullptr)
		{
			memset(data, 0, buffer->size);
			buffer->Unmap();
		}
	}

	// A device-local allocation is not zeroed: every piece freed by the step program's clear before anything reads it.
	if (!MakeStepSet())
	{
		DestroyNow();
		mCapacity = 0;
		Printf(TEXTCOLOR_RED "DebrisPool: no descriptor set for the step -- debris is drawn as ring particles\n");
		return false;
	}
	DebrisPoolFrame clearFrame;
	Dispatch(MODE_CLEAR, capacity, 0, clearFrame, 0, 0);

	Printf("DebrisPool: allocated %d pieces -- %llu bytes of pieces (+%llu definitions and mesh instances, +%llu events, +%llu staging)\n",
		capacity, (unsigned long long)pieceBytes, (unsigned long long)definitionBytes, (unsigned long long)DEBRIS_STEP_EVENT_BYTES,
		(unsigned long long)STAGING_BYTES);
	return true;
}

void VkDebrisPool::DestroyNow()
{
	// Only for objects no command has used yet (a half-finished Allocate).
	mStepSet.reset();
	mPieces.reset();
	mDefinitions.reset();
	mEvents.reset();
	mStaging.reset();
}

void VkDebrisPool::Release(const char* why)
{
	if (!IsAllocated())
		return;

	// This frame's buffer set (written at the start of the frame) and its commands may still name these, so they go on the
	// frame's delete list. The next frame's set binds the stand-ins.
	auto deleteList = fb->GetCommands()->DrawDeleteList.get();
	deleteList->Add(std::move(mStepSet));
	const size_t pieceBytes = mPieces != nullptr ? mPieces->size : 0;
	deleteList->Add(std::move(mPieces));
	deleteList->Add(std::move(mDefinitions));
	deleteList->Add(std::move(mEvents));
	deleteList->Add(std::move(mStaging));

	Printf("DebrisPool: released (%s) -- %llu bytes of pieces freed\n", why, (unsigned long long)pieceBytes);
	mCapacity = 0;
	DebrisPoolStatus().Allocated = false;
}

//-----------------------------------------------------------------------------
//
// The program, its set, the copies and the dispatches
//
//-----------------------------------------------------------------------------

bool VkDebrisPool::EnsureProgram()
{
	if (mStepProgram)
		return true;
	if (mProgramFailed)
		return false;

	mStepProgram = mCompute->CreateProgram("shaders/compute/debris_step.comp",
		{
			{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },			// the pieces
			{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },			// the definitions (and the mesh instance list, unread)
			{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER },			// the step's events
			{ 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },	// the level field, fine
			{ 4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },	// coarse
			{ 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER },	// its header
		},
		(uint32_t)sizeof(DebrisStepConstants));
	if (!mStepProgram)
	{
		// CreateProgram logged why. Not retried this session.
		Printf(TEXTCOLOR_RED "DebrisPool: the step program did not build -- debris is drawn as ring particles this session\n");
		mProgramFailed = true;
		return false;
	}
	return true;
}

// A fresh set for this frame's dispatches: the level field it samples is whatever exists now (the field runs before the pool
// in VkComputeManager::RunFrame, so a field made or freed this frame is already settled), or the descriptor manager's
// stand-ins that say "no level". The last set goes on the delete list.
bool VkDebrisPool::MakeStepSet()
{
	if (!mStepProgram || mPieces == nullptr)
		return false;

	VkDescriptorSetManager* descriptors = fb->GetDescriptorSetManager();
	VkLevelField* field = mCompute->GetLevelField();
	const bool fieldBound = field != nullptr && field->IsAllocated();
	VulkanImageView* fine = fieldBound ? field->GetFieldView(0) : descriptors->GetLevelFieldStandInView();
	VulkanImageView* coarse = fieldBound ? field->GetFieldView(1) : descriptors->GetLevelFieldStandInView();
	VulkanImageView* header = fieldBound ? field->GetHeaderView() : descriptors->GetLevelFieldHeaderStandInView();
	VulkanSampler* fieldSampler = descriptors->GetLevelFieldSampler();
	VulkanSampler* headerSampler = fb->GetSamplerManager()->Get(PPFilterMode::Nearest, PPWrapMode::Clamp);	// texelFetch only
	if (fine == nullptr || coarse == nullptr || header == nullptr || fieldSampler == nullptr || headerSampler == nullptr)
		return false;

	std::unique_ptr<VulkanDescriptorSet> set = mCompute->AllocateSet(mStepProgram.get());
	if (!set)
		return false;
	WriteDescriptors()
		.AddBuffer(set.get(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mPieces.get())
		.AddBuffer(set.get(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mDefinitions.get())
		.AddBuffer(set.get(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mEvents.get())
		.AddCombinedImageSampler(set.get(), 3, fine, fieldSampler, VK_IMAGE_LAYOUT_GENERAL)
		.AddCombinedImageSampler(set.get(), 4, coarse, fieldSampler, VK_IMAGE_LAYOUT_GENERAL)
		.AddCombinedImageSampler(set.get(), 5, header, headerSampler, VK_IMAGE_LAYOUT_GENERAL)
		.Execute(fb->device.get());

	if (mStepSet)
		fb->GetCommands()->DrawDeleteList->Add(std::move(mStepSet));
	mStepSet = std::move(set);
	return true;
}

void VkDebrisPool::Dispatch(int mode, int count, int stepIndex, const DebrisPoolFrame& frame, int impulseCount, int wakeCount)
{
	if (count <= 0 || !mStepSet)
		return;
	const int step = std::clamp(stepIndex, 0, DEBRIS_STEPS_PER_FRAME - 1);
	DebrisStepConstants constants = {};
	constants.Range[0] = 0;
	constants.Range[1] = count;
	constants.Range[2] = mode;
	constants.Range[3] = step;
	constants.Lists[0] = impulseCount;
	constants.Lists[1] = wakeCount;
	constants.Lists[2] = frame.WakeAll[step] ? 1 : 0;
	constants.Clock[0] = frame.StepTime[step];
	constants.Clock[1] = DEBRIS_STEP_SECONDS;
	constants.Clock[2] = frame.RestScale;
	constants.Clock[3] = DEBRIS_REST_SCALE_FROM;
	constants.Tuning[0] = frame.SizeScale;
	mCompute->Dispatch(mStepProgram.get(), mStepSet.get(), &constants, Groups(count), 1, 1);
}

// The frame's new pieces for one step (stepIndex), or those for no step (-1: a Step below 0 or at or past this frame's steps),
// from the staging buffer into their slots. A slot outside the pool is skipped.
void VkDebrisPool::CopySpawns(const DebrisPoolFrame& frame, int stepIndex, int spawnCount)
{
	if (spawnCount <= 0)
		return;
	const int steps = std::clamp(frame.Steps, 0, DEBRIS_STEPS_PER_FRAME);
	std::vector<VkBufferCopy> regions;
	for (int i = 0; i < spawnCount; i++)
	{
		const DebrisSpawnUpload& spawn = frame.Spawns[i];
		const bool noStep = spawn.Step < 0 || spawn.Step >= steps;
		if (stepIndex < 0 ? !noStep : (noStep || spawn.Step != stepIndex))
			continue;
		if (spawn.Slot >= (uint32_t)mCapacity)
			continue;
		VkBufferCopy region = {};
		region.srcOffset = (VkDeviceSize)i * (VkDeviceSize)DEBRIS_PIECE_BYTES;
		region.dstOffset = (VkDeviceSize)spawn.Slot * (VkDeviceSize)DEBRIS_PIECE_BYTES;
		region.size = (VkDeviceSize)DEBRIS_PIECE_BYTES;
		regions.push_back(region);
	}
	if (regions.empty())
		return;
	mCompute->BeginWork();	// outside any render pass, inside fx.compute
	fb->GetCommands()->GetDrawCommands()->copyBuffer(mStaging->buffer, mPieces->buffer, (uint32_t)regions.size(), regions.data());
}
