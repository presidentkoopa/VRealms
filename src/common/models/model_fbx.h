/*
** model_fbx.h
**
** RS FORK -- AN FBX FILE AS A MODEL, read at run time by ufbx (libraries/ufbx).
**
** One FFBXModel is ONE skinned mesh of the file with the skeleton its skin binds to, and every
** frame of every take of the file. A file with several skinned meshes is named once per mesh:
**
**     Model 0 "hand_final.fbx:Hand_Low"      (model.cpp FindModel: "<file>.fbx:<mesh node name>")
**     Model 0 "hand_final.fbx:Hand_Low1?mirrorskin=Hand_Low"   (options after '?': fbx_skinned.h Selector)
**
** The material's maps the FBX embeds (normal, roughness, metalness) are registered as engine textures from the
** file's own bytes and hung on the skin as the engine's PBR material (models_fbx.cpp E10).
**
** The data is read by fbx_skinned.cpp (engine-free; tools/fbxload_test runs the same code against the
** file) into exactly what an IQM carries, and this class is an IQMModel underneath, so the IQM bone
** path draws it unchanged: CalculateBones, FindJoint/GetJointPose (vr_handpose's joint lookups),
** FrameIndex/Frame, the decoupled-animation calls by take name, bone overrides and bone lengths.
**
** What a frame is: frame k of a take is the take sampled at the file's own frame rate, k / fps
** seconds after its first key. Takes follow each other in file order; each is an animation under
** its own name ("Take 001"), so FindFrame("Take 001:500") and FindFirstFrame("Take 001") work.
** Joint names are the FBX nodes' own. docs/FBX_LOADER.md has the checklist and the measured numbers.
**
**---------------------------------------------------------------------------
*/

#pragma once

#include "model_iqm.h"

class FFBXModel : public IQMModel
{
public:
	// mesh: what follows the colon in the MODELDEF file name -- the mesh's node name and any options
	// (fbx_skinned.h ParseSelector); empty = the file's only skinned mesh.
	explicit FFBXModel(const char *mesh) : MeshSelector(mesh ? mesh : "") {}

	bool Load(const char* fn, int lumpnum, const char* buffer, int length) override;

	const FString &GetMeshSelector() const { return MeshSelector; }

protected:
	// The vertices are built once, in Load; a vertex buffer rebuilt later (FlushModels, a renderer
	// restart, the Vulkan background loader) copies them instead of reading the file again.
	void LoadGeometry() override;
	void LoadGeometry(FileSys::FileData* lumpData) override;

private:
	FString MeshSelector;
	TArray<FModelVertex> BuiltVertices;
};
