/*
** models_fbx.cpp
**
** RS FORK -- an FBX file as a model (model_fbx.h): the engine half. fbx_skinned.cpp reads the file
** with ufbx into plain arrays; this fills the IQMModel the arrays describe, the same members, in the
** same conventions, that IQMModel::Load fills from an .iqm (models_iqm.cpp:37-296). From here on the
** model is drawn, posed and queried by IQMModel's own code. Checklist: docs/FBX_LOADER.md (E1-E9).
**
**---------------------------------------------------------------------------
*/

#include <string>

#include "model_fbx.h"
#include "fbx_skinned.h"
#include "filesystem.h"
#include "printf.h"
#include "texturemanager.h"

//===========================================================================
//
// FFBXModel::Load
//
//===========================================================================

bool FFBXModel::Load(const char* path, int lumpnum, const char* buffer, int length)
{
	mLumpNum = lumpnum;

	FBXSkinned::Result r;
	std::string err;
	const FString fileName = fileSystem.GetFileFullName(lumpnum);
	if (!FBXSkinned::Load(buffer, (size_t)length, MeshSelector.GetChars(), r, err))
	{
		Printf(PRINT_HIGH, TEXTCOLOR_RED "FBX model %s:%s -- %s\n", fileName.GetChars(), MeshSelector.GetChars(), err.c_str());
		return false;
	}
	const int nj = (int)r.joints.size();
	if (nj <= 0 || r.numFrames <= 0 || r.verts.empty() || r.tris.empty())
	{
		Printf(PRINT_HIGH, TEXTCOLOR_RED "FBX model %s:%s -- nothing to draw (%d joints, %d frames, %d vertices)\n",
			fileName.GetChars(), MeshSelector.GetChars(), nj, r.numFrames, (int)r.verts.size());
		return false;
	}

	// E1: the joints, as models_iqm.cpp:127-177 reads them. Translate/Quaternion/Scale is the rest
	// LOCAL transform (a root: its model-space transform). Position/Rotation/Scaling, which the IQM
	// loader accumulates from the locals, are taken from the rest WORLD matrix decomposed instead:
	// the IQM accumulation is not exact under a mirrored parent (it carries the reflection as a
	// per-axis scale that never meets the child's rotation), and the left hand's root is mirrored.
	Joints.Resize(nj);
	for (int j = 0; j < nj; j++)
	{
		const FBXSkinned::Joint &s = r.joints[j];
		IQMJoint &J = Joints[j];
		J.Name = FName(s.name.c_str());
		NamedJoints.Insert(J.Name, j);
		J.Parent = s.parent;
		J.Translate = FVector3((float)s.rest.t[0], (float)s.rest.t[1], (float)s.rest.t[2]);
		J.Quaternion = FQuaternion((float)s.rest.q[0], (float)s.rest.q[1], (float)s.rest.q[2], (float)s.rest.q[3]);
		J.Quaternion.MakeUnit();
		J.Scale = FVector3((float)s.rest.s[0], (float)s.rest.s[1], (float)s.rest.s[2]);
		J.Position = FVector3((float)s.restWorldTRS.t[0], (float)s.restWorldTRS.t[1], (float)s.restWorldTRS.t[2]);
		J.Rotation = FQuaternion((float)s.restWorldTRS.q[0], (float)s.restWorldTRS.q[1], (float)s.restWorldTRS.q[2], (float)s.restWorldTRS.q[3]);
		J.Rotation.MakeUnit();
		J.Scaling = FVector3((float)s.restWorldTRS.s[0], (float)s.restWorldTRS.s[1], (float)s.restWorldTRS.s[2]);
		if (J.Parent < 0) RootJoints.Push(j);
		else Joints[J.Parent].Children.Push(j);	// fbx_skinned orders parents first
	}

	// E2: baseframe / inversebaseframe, exactly models_iqm.cpp:201-226.
	baseframe.Resize(nj);
	inversebaseframe.Resize(nj);
	for (int i = 0; i < nj; i++)
	{
		const IQMJoint& j = Joints[i];
		VSMatrix m, invm;
		m.loadIdentity();
		m.translate(j.Translate.X, j.Translate.Y, j.Translate.Z);
		m.multQuaternion(j.Quaternion);
		m.scale(j.Scale.X, j.Scale.Y, j.Scale.Z);
		m.inverseMatrix(invm);
		if (j.Parent >= 0)
		{
			baseframe[i] = baseframe[j.Parent];
			baseframe[i].multMatrix(m);
			inversebaseframe[i] = invm;
			inversebaseframe[i].multMatrix(inversebaseframe[j.Parent]);
		}
		else
		{
			baseframe[i] = m;
			inversebaseframe[i] = invm;
		}
	}

	// E3: every frame of every take, [frame * joints + joint], as models_iqm.cpp:228-257 lays it out.
	TRSData.Resize(r.numFrames * nj);
	for (unsigned k = 0; k < TRSData.Size(); k++)
	{
		const FBXSkinned::TRSd &s = r.frames[k];
		TRS &t = TRSData[k];
		t.translation = FVector3((float)s.t[0], (float)s.t[1], (float)s.t[2]);
		t.rotation = FQuaternion((float)s.q[0], (float)s.q[1], (float)s.q[2], (float)s.q[3]);
		t.rotation.MakeUnit();
		t.scaling = FVector3((float)s.s[0], (float)s.s[1], (float)s.s[2]);
	}

	// E4: one animation per take, by the take's own name (models_iqm.cpp:188-199).
	for (const FBXSkinned::Take &tk : r.takes)
	{
		IQMAnim a;
		a.Name = tk.name.c_str();
		a.FirstFrame = (uint32_t)tk.firstFrame;
		a.NumFrames = (uint32_t)tk.numFrames;
		a.Framerate = (float)tk.fps;
		a.Loop = false;
		NamedAnimations.Insert(FName(a.Name.GetChars()), (int)Anims.Size());
		Anims.Push(a);
	}

	// E5: one surface: the mesh, its material, and the material's own base-colour texture looked up
	// beside the model (models_iqm.cpp:108 looks the material name up the same way). MODELDEF Skin
	// overrides it as it does for any model.
	IQMMesh mesh;
	mesh.Name = r.meshName.c_str();
	mesh.Material = r.diag.materialName.c_str();
	mesh.FirstVertex = 0;
	mesh.NumVertices = (uint32_t)r.verts.size();
	mesh.FirstTriangle = 0;
	mesh.NumTriangles = (uint32_t)(r.tris.size() / 3);
	mesh.Skin = r.diag.textureFile.empty() ? FTextureID(FNullTextureID()) : LoadSkin(path, r.diag.textureFile.c_str());
	Meshes.Push(mesh);

	// E6: triangles as stored (fbx_skinned winds them as the engine's IQMs are wound).
	Triangles.Resize(mesh.NumTriangles);
	for (unsigned t = 0; t < Triangles.Size(); t++)
		for (int k = 0; k < 3; k++) Triangles[t].Vertex[k] = r.tris[t * 3 + k];

	// E7: vertices in the IQM load convention (models_iqm.cpp LoadPosition/LoadNormal: file Z is the
	// engine's up, so (x, y, z) is stored (x, z, y); CalculateBones conjugates the bones to match).
	NumVertices = (uint32_t)r.verts.size();
	BuiltVertices.Resize(NumVertices);
	for (unsigned i = 0; i < NumVertices; i++)
	{
		const FBXSkinned::Vertex &s = r.verts[i];
		FModelVertex &v = BuiltVertices[i];
		v.Set(s.pos[0], s.pos[2], s.pos[1], s.uv[0], s.uv[1]);
		v.SetNormal(s.nrm[0], s.nrm[2], s.nrm[1]);
		v.SetBoneSelector(s.bone[0], s.bone[1], s.bone[2], s.bone[3]);
		v.SetBoneWeight(s.weight[0], s.weight[1], s.weight[2], s.weight[3]);
	}

	// E8: what was read, and what the format could not carry, said once at load.
	const FBXSkinned::Diag &d = r.diag;
	FString takes;
	for (const FBXSkinned::Take &tk : r.takes)
		takes.AppendFormat(" '%s' frames %d-%d (%d at %g fps, keys %.3f-%.3f s)", tk.name.c_str(), tk.firstFrame, tk.firstFrame + tk.numFrames - 1, tk.numFrames, tk.fps, tk.keyFirst, tk.keyLast);
	Printf("FBX model %s:%s -- mesh %s, %d joints, %u vertices, %u triangles, material %s%s%s;%s%s\n",
		fileName.GetChars(), MeshSelector.GetChars(), r.meshName.c_str(), nj, NumVertices, mesh.NumTriangles,
		d.materialName.c_str(), d.textureFile.empty() ? " (no texture)" : ", texture ", d.textureFile.c_str(),
		takes.IsEmpty() ? " no take: one frame, the rest pose" : "", takes.GetChars());
	Printf("FBX model %s:%s -- rest: skin det %+.0f%s, bind-to-rest %.2g; faces %s; normals %s (file's face the surface by %.3f); "
		"weights: %d of %d control points had more than 4 (largest cut %.4f, kept 4 renormalised), byte rounding %.4f%s%s\n",
		fileName.GetChars(), MeshSelector.GetChars(), d.restDeterminant, d.restDeterminant < 0 ? " (mirrored)" : "", d.rebaseMaxDev,
		d.reversedForOutward ? "reversed (the file's order was inside out at rest)" : "as the file orders them",
		d.normalsFromSurface ? "from the surface" : "from the file", d.normalAgreeFile,
		d.verticesOver4, d.controlPoints, d.maxDroppedWeight, d.maxQuantError,
		d.unweightedControlPoints ? ", unweighted control points ride joint 0" : "",
		d.blendShapeDeformers ? ", blend shapes NOT loaded" : "");
	if (d.rebaseMaxDev > 1e-4)
		Printf(PRINT_HIGH, TEXTCOLOR_RED "FBX model %s:%s -- the skin's bind is not one transform at the rest pose (%.3g): the drawn skin will differ from the file's\n",
			fileName.GetChars(), MeshSelector.GetChars(), d.rebaseMaxDev);
	return true;
}

//===========================================================================
//
// E9: geometry from what Load built; the file is not read again.
//
//===========================================================================

void FFBXModel::LoadGeometry()
{
	if (Vertices.Size() > 0) return;
	Vertices = BuiltVertices;
}

void FFBXModel::LoadGeometry(FileSys::FileData* /*lumpData: the vertices were built in Load*/)
{
	LoadGeometry();
}
