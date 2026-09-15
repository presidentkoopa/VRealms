/*
** vk_smokevolume.h
**
** [SMOKEVOLUME] The smoke volume's GPU side: its 3D images and its simulation step.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** "Engine docs/SMOKE_VOLUME_PLAN.md" #13 (13a, 13b). The first client of
** VkComputeManager.
**
** THE VOLUME, allocated only while SmokeVolumeFrame::Active, at the quality it names
** (r_smoke_quality): density and heat RG16F and velocity RGBA16F, each a ping-pong pair,
** the R8 solid mask -- 25 bytes a cell -- plus two tiny R8 tile maps (one texel per
** SMOKE_TILE_CELLS^3 cells). Formats are probed first; a device that cannot, an
** allocation that fails, or a simulation program that does not build logs one line and
** leaves smoke off at that quality. Every image stays in layout GENERAL for its life.
**
** A FRAME (Run), in SmokeVolumeFrame's order: a new box (all empty, the mask all solid)
** or a recentre (smoke_shift.comp moves every volume by whole tiles), a clear, the mask
** tiles the CPU rasterised copied in, then each simulation step:
**   1. every kernel of that step (smoke_inject.comp, in place on the latest images),
**      each also marking its tiles active (smoke_tiles.comp pass 2);
**   2. smoke_advect.comp: the latest state of each pair -> the other image;
**   3. smoke_tiles.comp passes 0 and 1: which tiles hold anything, spread by a tile;
**   4. the pairs' "latest" flips.
** So after a step each pair holds the last two whole states, which 13c's march blends by
** TicFrac (owner answer 3). A kernel changes the latest image in place before the step
** reads it, so the older state already carries that tic's event: a puff appears at the
** start of the blend rather than fading in over one tic. After a recentre both images of
** a pair hold the moved latest state (no blend across the move).
**
** Hooks for the later steps, by name: [13c] GetDensityHeatView(0 / 1) for the march's
** ExternalImage resolve, GetTileActiveView to skip empty tiles, GetOriginCell and
** HasSmoke.
**
** [13d] THE LIGHT GRID (hw_framecompute.h, SmokeLightGridSpec): a sibling allocation
** beside the volume, made at SmokeLightFrame::Quality while the volume exists and re-made
** alone when that quality changes -- RGBA16F light, RGBA8 SNORM direction (12 bytes a
** light cell) and a 2D RGBA8 map of each column's ambient light. On every frame with smoke
** to draw, after the steps, smoke_light.comp fills it: the ambient pass over the whole grid,
** then one pass per light over the cells its sphere covers (group fx.smokelight). The march
** reads it through GetLightImage / GetLightDirectionImage, in the same layout dance as the
** density.
**
** [13e] SOOT rides the velocity image's w (smoke_inject.comp adds it, smoke_advect.comp carries it
** as the density, only while SmokeSimSettings::SootLive); smoke_light.comp reads the latest density
** and velocity (bindings 5 and 6) to darken the light where the smoke is sooty, and stores the
** darkness in the direction grid's w. THE BEAM LIST: a SMOKE_BEAMS_MAX x SMOKE_BEAM_TEXELS RGBA32F
** image (PPExternalImage::SmokeBeams), made on the first frame with beams in smoke, copied into when
** the frame's list changed, and kept in SHADER_READ_ONLY_OPTIMAL between copies.
**
** [EFFECTLIGHTS] LD: EFFECT LIGHTS IN THE SMOKE (hw_framecompute.h, SmokeEffectLightPass): on a frame
** an effect light reaches the grid, RunLight adds pass 2 after the dynamic lights -- one dispatch of
** smoke_light.comp's SMOKE_EFFECT_LIGHTS variant (the light set's seven bindings plus the effect light
** records and bins, storage buffers 7 and 8), made and given its own set the first time it is needed.
** A frame with no effect light in reach records exactly what it did before.
**
** [13F] SURFACE LIGHT IN PASS 0 (hw_framecompute.h, SmokeSurfaceLightFrame): on a frame the CPU side says surface light is live
** (a column's sector glows, a sweep band gives light, the darkness curve or a passed look is on), RunLight runs pass 0 through
** smoke_light.comp's SMOKE_SURFACE_LIGHT variant -- the light set's seven bindings plus two storage buffers, the columns (7,
** copied in when their serial moves on) and the records (8, every such frame), device-local behind their own staging buffers
** -- made, allocated and grown on first use and freed with the light grid. Any other frame, and any refusal, runs pass 0 exactly
** as before.
**
** CPU-side decisions -- when the volume exists, where its box is, what goes in, the
** mask -- are made in hw_smokevolume.cpp and arrive in SmokeVolumeFrame, so a render
** rebuild replaces only this file and the shaders.
**
*/

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <zvulkan/vulkanobjects.h>
#include "hw_framecompute.h"
#include "vulkan/textures/vk_imagetransition.h"	// [13c] VkTextureImage: a post-process pass binds the volumes

class VulkanRenderDevice;
class VkComputeManager;
class VkComputeProgram;

class VkSmokeVolume
{
public:
	explicit VkSmokeVolume(VkComputeManager* compute);
	~VkSmokeVolume();

	// One frame: allocate, re-allocate, clear or release as the frame says, then do its
	// work. Records GPU commands only when there is something to do.
	void Run(const SmokeVolumeFrame& frame);

	bool IsAllocated() const { return mDensityHeat[0].Image != nullptr; }
	int GetQuality() const { return mAllocatedQuality; }
	const SmokeGridSpec& GetGrid() const { return mGrid; }

	// [13c] Density (r) and heat (g). stepsAgo 0 = the latest state, 1 = the state one
	// step before it; the march blends the two by TicFrac.
	VulkanImageView* GetDensityHeatView(int stepsAgo) const;
	// Velocity (xyz, cells per step), the same way.
	VulkanImageView* GetVelocityView(int stepsAgo) const;
	// 1 = solid (SH1's LevelSolidity mask), 0 = open.
	VulkanImageView* GetSolidMaskView() const { return mSolidMask.View.get(); }
	// [13c] One texel per SMOKE_TILE_CELLS^3 cells: 1 = the tile may hold smoke.
	VulkanImageView* GetTileActiveView() const { return mTileActive.View.get(); }
	// [13c] The images themselves, for the drawing's external image resolve (VkTextureManager::
	// GetTexture, PPTextureType::ExternalImage). A post-process pass may bind one only while its
	// Layout is SHADER_READ_ONLY_OPTIMAL. Run puts density, heat and the tile map there at its end on
	// a frame with smoke to draw, and takes every volume back to GENERAL before its next compute work.
	VkTextureImage* GetDensityHeatImage(int stepsAgo) { return &mDensityHeat[stepsAgo == 0 ? mLatest : 1 - mLatest]; }
	VkTextureImage* GetTileActiveImage() { return &mTileActive; }
	// [13c] The box's first cell in world cells (x CellSize = map units), and whether the
	// CPU's bounds say visible smoke may exist -- both as of the last Run.
	const int* GetOriginCell() const { return mOriginCell; }
	bool HasSmoke() const { return mHasSmoke; }

	// [13d] The light grid, for the march's external image resolve (PPExternalImage::SmokeLight,
	// SmokeLightDirection), under the same layout rule as GetDensityHeatImage. Image is null while no
	// grid is held. GetLightQuality: the r_smoke_light_quality it was made at, 0 = none.
	VkTextureImage* GetLightImage() { return &mLight; }
	VkTextureImage* GetLightDirectionImage() { return &mLightDirection; }
	int GetLightQuality() const { return mLightQuality; }

	// [13e] The beam list (PPExternalImage::SmokeBeams), for the drawing's external image resolve under the same layout
	// rule. Image is null until the first frame with beams in smoke.
	VkTextureImage* GetBeamListImage() { return &mBeamList; }

private:
	// [13c] A VkTextureImage (Image and View, as before), so a post-process pass can bind one
	// (VkDescriptorSetManager::GetInput) and its Layout is tracked. Every volume is GENERAL from its
	// allocation on, except while the drawing reads it (RestoreComputeLayouts, PrepareDrawLayouts).
	using Volume = VkTextureImage;

	bool Allocate(int quality, const SmokeGridSpec& grid);
	bool CreateVolume(Volume& volume, VkFormat format, int width, int height, int depth, const char* name);
	void DestroyVolumesNow();
	void Release(const char* why);
	void ClearImage(Volume& volume, float value);
	void ClearContents();
	void ClearEverything();
	// [13c] Every volume back to GENERAL (start of Run), and density, heat and the tile map to
	// SHADER_READ_ONLY_OPTIMAL for the drawing (end of Run, only with smoke to draw). Each records
	// one barrier, and only when a layout actually changes.
	void RestoreComputeLayouts();
	void PrepareDrawLayouts();
	bool EnsurePrograms();
	bool EnsureSets();
	void UploadMask(const SmokeVolumeFrame& frame);
	void Shift(const int shift[3]);
	void RunStep(const SmokeVolumeFrame& frame, int stepIndex);
	void DispatchTiles(int pass, int latest, const int tileMin[3], const int tileMax[3]);
	void DispatchGrid(VkComputeProgram* program, VulkanDescriptorSet* set, const void* constants);

	// [13d] The light grid: made or re-made at the frame's light quality (false: none held), freed, its
	// descriptor set, the ambient columns copied in when their serial changed, and the frame's fill.
	bool EnsureLightGrid(const SmokeLightFrame& light);
	bool CreateImage2D(Volume& image, VkFormat format, int width, int height, const char* name);
	void DestroyLightImagesNow();
	void ReleaseLightGrid(const char* why);
	bool EnsureLightSet();
	void UploadAmbient(const SmokeLightFrame& light);
	void RunLight(const SmokeVolumeFrame& frame);
	// [EFFECTLIGHTS] LD: the pass-2 variant (made on first use; false: refused this session) and its dispatch, from RunLight.
	bool EnsureEffectLightProgram();
	void DispatchEffectLights(const SmokeVolumeFrame& frame);
	// [13F] Pass 0's surface light variant (made on first use; false: refused this session), a surface buffer copied in through its
	// staging buffer (made or grown to fit; false: not made), and pass 0 through the variant -- false, with nothing dispatched,
	// when this frame's surface light is not live or anything is refused (RunLight then dispatches pass 0 as before).
	bool EnsureSurfaceProgram();
	bool UploadSurfaceBuffer(std::unique_ptr<VulkanBuffer>& buffer, std::unique_ptr<VulkanBuffer>& staging, size_t& capacity,
		const float* data, size_t bytes, const char* name, const char* stagingName);
	bool DispatchSurfaceAmbient(const SmokeVolumeFrame& frame, const void* ambientConstants);

	// [13e] The beam list image: made once (false: refused this session), the frame's list copied in (returns the beams
	// the image holds for this frame, 0 = none), freed with the volume.
	bool EnsureBeamList();
	int UploadBeams(const SmokeBeamFrame& beams);
	void ReleaseBeamList();

	VkComputeManager* mCompute = nullptr;
	VulkanRenderDevice* fb = nullptr;

	// Programs first, then the images, the staging buffer and the descriptor sets, so on
	// destruction the sets (which name the views) go before the images.
	std::unique_ptr<VkComputeProgram> mInjectProgram;
	std::unique_ptr<VkComputeProgram> mAdvectProgram;
	std::unique_ptr<VkComputeProgram> mTilesProgram;
	std::unique_ptr<VkComputeProgram> mShiftRG;		// target rg16f
	std::unique_ptr<VkComputeProgram> mShiftRGBA;	// target rgba16f
	std::unique_ptr<VkComputeProgram> mShiftR8;		// target r8
	std::unique_ptr<VkComputeProgram> mLightProgram;	// [13d] smoke_light.comp
	std::unique_ptr<VkComputeProgram> mEffectLightProgram;	// [EFFECTLIGHTS] LD: smoke_light.comp (SMOKE_EFFECT_LIGHTS), pass 2
	bool mProgramsReady = false;
	bool mProgramsFailed = false;
	bool mEffectLightProgramFailed = false;	// [EFFECTLIGHTS] LD: not retried this session
	std::unique_ptr<VkComputeProgram> mSurfaceProgram;	// [13F] smoke_light.comp (SMOKE_SURFACE_LIGHT), pass 0 with surface light
	bool mSurfaceProgramFailed = false;	// [13F] not retried this session

	Volume mDensityHeat[2];
	Volume mVelocity[2];
	Volume mSolidMask;
	Volume mTileContent;
	Volume mTileActive;
	int mLatest = 0;	// which image of both pairs holds the latest state

	// [13d] The light grid (GENERAL, but for the drawing's read of the first two) and its ambient column map
	// (2D, GENERAL for life).
	Volume mLight;				// RGBA16F: rgb light, a luminance weight
	Volume mLightDirection;		// RGBA8 SNORM: xyz the weight-averaged direction light travels
	Volume mAmbientColumns;		// 2D RGBA8: each column's sector light
	std::unique_ptr<VulkanBuffer> mAmbientStaging;

	// [13e] The beam list (2D RGBA32F, SHADER_READ_ONLY_OPTIMAL between copies), its staging buffer, the texels it holds
	// (empty: nothing copied in yet), and a refusal logged once.
	Volume mBeamList;
	std::unique_ptr<VulkanBuffer> mBeamStaging;
	std::vector<float> mBeamTexels;
	bool mBeamListRefused = false;

	std::unique_ptr<VulkanBuffer> mStaging;	// SMOKE_MASK_UPLOAD_BYTES_PER_FRAME, for the mask tiles

	// [13F] The surface light buffers (device-local storage buffers, each with its staging buffer; hw_framecompute.h): the columns,
	// copied in when their serial moves on, and the records, every frame surface light is live; their capacities in bytes.
	std::unique_ptr<VulkanBuffer> mSurfaceColumns;
	std::unique_ptr<VulkanBuffer> mSurfaceColumnsStaging;
	std::unique_ptr<VulkanBuffer> mSurfaceRecords;
	std::unique_ptr<VulkanBuffer> mSurfaceRecordsStaging;
	size_t mSurfaceColumnsCapacity = 0;
	size_t mSurfaceRecordsCapacity = 0;
	uint64_t mSurfaceColumnSerialUploaded = 0;

	// [i] = the sets whose "latest" is image i of the pairs.
	std::unique_ptr<VulkanDescriptorSet> mInjectSets[2];			// storage D[i], V[i]; sampled mask
	std::unique_ptr<VulkanDescriptorSet> mAdvectSets[2];			// sampled D[i], V[i], mask; storage tiles, D[1-i], V[1-i]
	std::unique_ptr<VulkanDescriptorSet> mTilesSets[2];				// sampled D[i], V[i]; storage both tile maps
	std::unique_ptr<VulkanDescriptorSet> mShiftDensitySets[2];		// D[i] -> D[1-i]
	std::unique_ptr<VulkanDescriptorSet> mShiftVelocitySets[2];		// V[i] -> V[1-i]
	std::unique_ptr<VulkanDescriptorSet> mShiftMaskOutSets[2];		// mask -> D[i] (scratch)
	std::unique_ptr<VulkanDescriptorSet> mShiftMaskInSets[2];		// D[i] -> mask
	bool mSetsReady = false;
	// [13d] sampled tile map, ambient columns, shadow map; storage light, direction. Written again on every
	// frame it is used, before its first dispatch: the engine's shadow map image can be re-made between
	// frames (gl_shadowmap_quality), and the frame before has finished by then.
	std::unique_ptr<VulkanDescriptorSet> mLightSet;
	// [EFFECTLIGHTS] LD: the pass-2 variant's set -- the light set's bindings plus the effect light records (7) and bins (8).
	// Written every frame pass 2 runs, before its dispatch; freed with the light set.
	std::unique_ptr<VulkanDescriptorSet> mEffectLightSet;
	// [13F] Pass 0's surface light variant's set -- the light set's bindings plus the columns (7) and records (8) buffers. Written
	// every frame the variant runs, before its dispatch; freed with the light set.
	std::unique_ptr<VulkanDescriptorSet> mSurfaceSet;

	SmokeGridSpec mGrid;
	int mTiles[3] = { 0, 0, 0 };
	int mAllocatedQuality = 0;
	uint64_t mTexelBytes = 0;
	uint64_t mLevelSerial = 0;
	int mOriginCell[3] = { 0, 0, 0 };
	bool mHasSmoke = false;
	bool mUploadWarned = false;

	// A quality this device refused (format, memory, a program, a descriptor set): not
	// retried until the quality changes or smoke stops being asked for, so a refusal
	// logs once, not every frame.
	int mRefusedQuality = 0;

	// [13d] The light grid held (0 = none), its size, the light quality refused the same way, and the
	// ambient column serial last copied in (0 = none: a new grid always takes the next one).
	int mLightQuality = 0;
	SmokeLightGridSpec mLightGrid;
	uint64_t mLightTexelBytes = 0;
	int mRefusedLightQuality = 0;
	uint64_t mAmbientSerialUploaded = 0;
	bool mLightWarned = false;
	bool mEffectLightWarned = false;	// [EFFECTLIGHTS] LD: no set for pass 2 (logged once)
	bool mSurfaceWarned = false;		// [13F] no set or no buffers for the surface light variant (logged once)
};
