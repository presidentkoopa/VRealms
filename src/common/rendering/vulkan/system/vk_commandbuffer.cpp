/*
** vk_commandbuffer.cpp
**
** Vulkan backend
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#include "vk_commandbuffer.h"
#include "vk_renderdevice.h"
#include "zvulkan/vulkanswapchain.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "hw_clock.h"
#include "hw_perflog.h"	// RS FORK -- r_perflog: UpdateGpuStats feeds it
#include "v_video.h"
#include "doomtype.h" // Printf

extern int rendered_commandbuffers;
int current_rendered_commandbuffers;

extern bool gpuStatActive;
extern bool keepGpuStatActive;
extern FString gpuStatOutput;

#include "c_cvars.h"

// RS FORK -- HOW LONG ANY GPU FENCE MAY BE WAITED ON.
//
// Every wait in this file was UINT64_MAX -- wait forever. A fence that never
// signals then froze the main thread with no error, no dump and nothing in the
// log: the long-parked "crash on death / respawn / level change", which is a
// HANG and is this. Five seconds is far beyond any legitimate frame; a GPU that
// has not finished in five seconds is never going to.
//
// One constant so the two waits cannot drift apart, and so raising it is one edit.
static const uint64_t kFenceWaitNs = 5ull * 1000ull * 1000ull * 1000ull;	// 5 s

// RS FORK -- GPU CHECKPOINTS (vk_gpu_checkpoints, defined in vk_renderdevice.cpp).
//
// Every PushGroup/PopGroup -- the names the renderer already gives its passes
// for "stat gpu" -- also drops a marker into the command stream. After a device
// loss the driver reports the last marker each queue STARTED and the last it
// FINISHED (VulkanDevice::DescribeDeviceLoss), which brackets the work that
// killed it.
EXTERN_CVAR(Bool, vk_gpu_checkpoints)

// A marker is read back AFTER the device is lost, possibly frames after it was
// recorded, so it has to be a pointer that is still valid then. Labels are
// interned for the life of the program. Pass names are a small fixed set; the
// cap only means a caller that ever built names dynamically would cost a wrong
// label, never unbounded memory.
static const char* InternCheckpointLabel(const char* prefix, const FString& name)
{
	static std::set<std::string> labels;
	std::string s = std::string(prefix) + name.GetChars();
	auto found = labels.find(s);
	if (found != labels.end())
		return found->c_str();
	if (labels.size() >= 1024)
		return "(checkpoint label table full)";
	return labels.insert(std::move(s)).first->c_str();
}

static const char* LastGroup = "(no render pass opened yet)";

const char* VkCommandBufferManager::LastGroupLabel()
{
	return LastGroup;
}

void VkCommandBufferManager::GpuCheckpoint(const char* label)
{
	// Decided once, on first use, from what the device was actually created
	// with -- the cvar alone only asks.
	if (mCheckpoints < 0)
	{
		mCheckpoints = (vk_gpu_checkpoints
			&& fb->device->SupportsExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)
			&& vkCmdSetCheckpointNV != nullptr) ? 1 : 0;
	}
	if (mCheckpoints == 0)
		return;
	vkCmdSetCheckpointNV(GetDrawCommands()->buffer, label);
}

VkCommandBufferManager::VkCommandBufferManager(VulkanRenderDevice* fb, VkQueue* queue, int queueFamily, bool uploadOnly)
	: fb(fb), fbQueue(queue), mIsUploadOnly(uploadOnly)
{
	mCommandPool = CommandPoolBuilder()
		.QueueFamily(queueFamily)
		.DebugName("mCommandPool")
		.Create(fb->device.get());

	for (auto& semaphore : mSubmitSemaphore)
		semaphore.reset(new VulkanSemaphore(fb->device.get()));

	for (auto& fence : mSubmitFence)
		fence.reset(new VulkanFence(fb->device.get()));

	for (int i = 0; i < maxConcurrentSubmitCount; i++)
		mSubmitWaitFences[i] = mSubmitFence[i]->fence;

	if (!mIsUploadOnly && fb->device->GraphicsTimeQueries)
	{
		mTimestampQueryPool = QueryPoolBuilder()
			.QueryType(VK_QUERY_TYPE_TIMESTAMP, MaxTimestampQueries)
			.Create(fb->device.get());

		GetDrawCommands()->resetQueryPool(mTimestampQueryPool.get(), 0, MaxTimestampQueries);
	}
}

VkCommandBufferManager::~VkCommandBufferManager()
{
}

VulkanCommandBuffer* VkCommandBufferManager::GetTransferCommands()
{
	if (!mTransferCommands)
	{
		mTransferCommands = mCommandPool->createBuffer();
		mTransferCommands->SetDebugName("VulkanRenderDevice.mTransferCommands");
		mTransferCommands->begin();
	}
	return mTransferCommands.get();
}

VulkanCommandBuffer* VkCommandBufferManager::GetDrawCommands()
{
	if (!mDrawCommands && !mIsUploadOnly)
	{
		mDrawCommands = mCommandPool->createBuffer();
		mDrawCommands->SetDebugName("VulkanRenderDevice.mDrawCommands");
		mDrawCommands->begin();
	}
	return mDrawCommands.get();
}

std::unique_ptr<VulkanCommandBuffer> VkCommandBufferManager::CreateUnmanagedCommands()
{
	std::unique_ptr<VulkanCommandBuffer> cmds = mCommandPool->createBuffer();
	cmds->SetDebugName("VulkanRenderDevice.arbitraryCommands");
	cmds->begin();
	return cmds;
}

void VkCommandBufferManager::BeginFrame()
{
	if (mNextTimestampQuery > 0)
	{
		GetDrawCommands()->resetQueryPool(mTimestampQueryPool.get(), 0, mNextTimestampQuery);
		mNextTimestampQuery = 0;
	}
}

void VkCommandBufferManager::FlushCommands(VulkanCommandBuffer** commands, size_t count, VkQueue* queue, bool finish, bool lastsubmit)
{
	int currentIndex = mNextSubmit % maxConcurrentSubmitCount;

	if (mNextSubmit >= maxConcurrentSubmitCount)
	{
		// RS FORK -- checked. A loss here used to be ignored and reported one
		// submit later, further from whatever caused it.
		//
		// AND BOUNDED, for the same reason as WaitForCommands below: an
		// unbounded wait on a fence that never signals is a silent freeze with
		// nothing in the log. This one is the RECYCLE wait -- we are about to
		// reuse a submit slot and are waiting for its previous frame to finish
		// -- so it is reachable on any frame, not only across a teardown.
		VkResult recycleWait = vkWaitForFences(fb->device->device, 1, &mSubmitFence[currentIndex]->fence, VK_TRUE, kFenceWaitNs);
		if (recycleWait == VK_TIMEOUT)
		{
			static int recycleTimeouts = 0;
			if (++recycleTimeouts <= 8)
			{
				Printf(TEXTCOLOR_RED "GPU FENCE TIMEOUT (submit slot %d): the previous frame in this slot "
					"never finished.\n" TEXTCOLOR_NORMAL "  See Engine docs/LEVEL_CHANGE_HANG.md. Continuing "
					"so you can quit cleanly and keep the log.\n", currentIndex);
			}
		}
		else
		{
			CheckVulkanError(recycleWait, "vkWaitForFences failed");
		}
		vkResetFences(fb->device->device, 1, &mSubmitFence[currentIndex]->fence);
	}

	QueueSubmit submit;

	for (size_t i = 0; i < count; i++)
		submit.AddCommandBuffer(commands[i]);

	if (mNextSubmit > 0)
		submit.AddWait(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mSubmitSemaphore[(mNextSubmit - 1) % maxConcurrentSubmitCount].get());

	auto framebuffers = fb->GetFramebufferManager();
	if (finish && framebuffers->PresentImageIndex != -1)
	{
		submit.AddWait(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, framebuffers->SwapChainImageAvailableSemaphore.get());
		submit.AddSignal(framebuffers->RenderFinishedSemaphores[framebuffers->PresentImageIndex].get());
	}

	if (!lastsubmit)
		submit.AddSignal(mSubmitSemaphore[currentIndex].get());

	submit.Execute(fb->device.get(), *queue, mSubmitFence[currentIndex].get());
	mNextSubmit++;
}

void VkCommandBufferManager::FlushCommands(bool finish, bool lastsubmit, bool uploadOnly)
{
	if (!uploadOnly)
		fb->GetRenderState()->EndRenderPass();

	if ((!uploadOnly && mDrawCommands) || mTransferCommands)
	{
		VulkanCommandBuffer* commands[2];
		size_t count = 0;

		if (mTransferCommands)
		{
			mTransferCommands->end();
			commands[count++] = mTransferCommands.get();
			TransferDeleteList->Add(std::move(mTransferCommands));
		}

		if (!uploadOnly && mDrawCommands)
		{
			mDrawCommands->end();
			commands[count++] = mDrawCommands.get();
			DrawDeleteList->Add(std::move(mDrawCommands));
		}

		FlushCommands(commands, count, fbQueue, finish, lastsubmit);

		current_rendered_commandbuffers += (int)count;
	}
}

void VkCommandBufferManager::WaitForCommands(bool finish, bool uploadOnly, bool acquireImageForPresent)
{
	if (finish)
	{
		Finish.Reset();
		Finish.Clock();

		if (acquireImageForPresent)
			fb->GetFramebufferManager()->AcquireImage();

	}

	FlushCommands(finish, true, uploadOnly);

	if (finish)
	{
		if (!fb->GetVSync())
			fb->FPSLimit();

		fb->GetFramebufferManager()->QueuePresent();
	}

	int numWaitFences = min(mNextSubmit, (int)maxConcurrentSubmitCount);

	if (numWaitFences > 0)
	{
		// RS FORK -- A BOUNDED WAIT, BECAUSE AN UNBOUNDED ONE IS A SILENT HANG.
		//
		// This was UINT64_MAX: wait forever. A fence that never signals froze the
		// main thread with no error, no dump and nothing in the log -- the game
		// simply stopped, one thread spinning in the driver, and the last line
		// written was whatever happened before the frame. That is the
		// long-parked "crash on death / respawn / level change", which is none
		// of those things and is this line. Captured live off a hung process:
		//
		//   NtWaitForMultipleObjects -> WaitForMultipleObjects -> (driver)
		//   VkCommandBufferManager::WaitForCommands   here
		//   VulkanRenderDevice::Update                vk_renderdevice.cpp
		//   End2DAndUpdate / D_Display / D_DoomLoop   d_main.cpp
		//
		// THIS DOES NOT FIX THE ROOT CAUSE and is not pretending to. Something
		// loses or orphans a submit across a level teardown and its fence never
		// signals; finding which submit is the real work. What this does is turn
		// a PERMANENT SILENT HANG into a LOUD, DIAGNOSABLE FAILURE -- which is
		// the difference between a bug that survives being parked for months and
		// one somebody can act on.
		//
		// Five seconds is far beyond any legitimate frame: a GPU that has not
		// finished a frame in five seconds is not busy, it is never going to
		// finish. On timeout we say so, name the fence count, and carry on
		// rather than calling CheckVulkanError -- a hard error here would abort
		// the process on what may be a recoverable stall, and the whole point is
		// to leave the player somewhere they can quit cleanly and send a log.
		VkResult waitResult = vkWaitForFences(fb->device->device, numWaitFences, mSubmitWaitFences, VK_TRUE, kFenceWaitNs);
		if (waitResult == VK_TIMEOUT)
		{
			static int timeouts = 0;
			if (++timeouts <= 8)
			{
				Printf(TEXTCOLOR_RED "GPU FENCE TIMEOUT: %d submit fence(s) did not signal in 5 s.\n"
					TEXTCOLOR_NORMAL "  The renderer is waiting on work the GPU never finished. This is the "
					"level-change / respawn hang;\n  see Engine docs/LEVEL_CHANGE_HANG.md. The game is "
					"continuing so you can quit cleanly and keep the log.\n", numWaitFences);
			}
		}
		else
		{
			CheckVulkanError(waitResult, "vkWaitForFences failed");
		}
		vkResetFences(fb->device->device, numWaitFences, mSubmitWaitFences);
	}

	DeleteFrameObjects(uploadOnly);
	mNextSubmit = 0;

	if (finish)
	{
		Finish.Unclock();
		rendered_commandbuffers = current_rendered_commandbuffers;
		current_rendered_commandbuffers = 0;
	}
}

void VkCommandBufferManager::DeleteFrameObjects(bool uploadOnly)
{
	TransferDeleteList = std::make_unique<DeleteList>();
	if (!uploadOnly)
		DrawDeleteList = std::make_unique<DeleteList>();
}

void VkCommandBufferManager::PushGroup(const FString& name, int timestampViews)
{
	// RS FORK -- the checkpoint is recorded whether or not "stat gpu" is on: a
	// device loss does not wait for anybody to be profiling.
	const char* started = InternCheckpointLabel("started ", name);
	LastGroup = started;
	mCheckpointStack.push_back(InternCheckpointLabel("finished ", name));
	GpuCheckpoint(started);

	if (!gpuStatActive)
		return;

	// RS FORK -- reserve timestampViews indices (see the header); with the
	// default of one this is the old "< MaxTimestampQueries" test and "++".
	if (mNextTimestampQuery + timestampViews <= MaxTimestampQueries && fb->device->GraphicsTimeQueries)
	{
		TimestampQuery q;
		q.name = name;
		q.startIndex = mNextTimestampQuery;
		mNextTimestampQuery += timestampViews;
		q.endIndex = 0;
		GetDrawCommands()->writeTimestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, mTimestampQueryPool.get(), q.startIndex);
		mGroupStack.push_back(timeElapsedQueries.size());
		timeElapsedQueries.push_back(q);
	}
}

void VkCommandBufferManager::PopGroup(int timestampViews)
{
	// RS FORK -- see PushGroup.
	if (!mCheckpointStack.empty())
	{
		GpuCheckpoint(mCheckpointStack.back());
		mCheckpointStack.pop_back();
	}

	if (!gpuStatActive || mGroupStack.empty())
		return;

	TimestampQuery& q = timeElapsedQueries[mGroupStack.back()];
	mGroupStack.pop_back();

	// RS FORK -- as in PushGroup.
	if (mNextTimestampQuery + timestampViews <= MaxTimestampQueries && fb->device->GraphicsTimeQueries)
	{
		q.endIndex = mNextTimestampQuery;
		mNextTimestampQuery += timestampViews;
		GetDrawCommands()->writeTimestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, mTimestampQueryPool.get(), q.endIndex);
	}
}

void VkCommandBufferManager::UpdateGpuStats()
{
	uint64_t timestamps[MaxTimestampQueries];
	if (mNextTimestampQuery > 0)
		mTimestampQueryPool->getResults(0, mNextTimestampQuery, sizeof(uint64_t) * mNextTimestampQuery, timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

	double timestampPeriod = fb->device->PhysicalDevice.Properties.Properties.limits.timestampPeriod;

	gpuStatOutput = "";
	for (auto& q : timeElapsedQueries)
	{
		if (q.endIndex <= q.startIndex)
			continue;

		int64_t timeElapsed = max(static_cast<int64_t>(timestamps[q.endIndex] - timestamps[q.startIndex]), (int64_t)0);
		double timeNS = timeElapsed * timestampPeriod;

		FString out;
		out.Format("%s=%04.2f ms\n", q.name.GetChars(), timeNS / 1000000.0f);
		gpuStatOutput += out;

		// RS FORK -- r_perflog accumulates the same numbers over its window.
		// Checked per group, so with no groups timed this costs nothing.
		if (*r_perflog > 0)
			PerfLog::AddGpuSample(q.name.GetChars(), timeNS / 1000000.0);
	}
	timeElapsedQueries.clear();
	mGroupStack.clear();

	gpuStatActive = keepGpuStatActive;
	keepGpuStatActive = false;
}
