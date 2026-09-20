#pragma once

#include "model_md2.h"

//
// Quake 1 alias models (.mdl, magic "IDPO").
//
// WHY THIS DERIVES FROM FDMDModel. MDL is MD2's direct ancestor: byte-packed
// vertices reconstructed with a scale and an origin, frame-based vertex
// animation, and the SAME 162-entry avertexnormals table for lighting. FDMDModel
// already implements RenderFrame, BuildVertexBuffer, AddSkins, FindFrame and
// GetLocalExtent against its own DMDInfo/ModelFrame structures, and FMD2Model
// already demonstrates the pattern: override Load and LoadGeometry, translate
// into those structures, inherit everything else. This does the same.
//
// TWO THINGS MDL DOES THAT MD2 DOES NOT, and both are handled in Load():
//
//   THE SKIN IS EMBEDDED, not named. MD2 stores a 64-byte filename per skin and
//   LoadSkin() goes and finds it. MDL stores the pixels inline, 8-bit indexed
//   into QUAKE's palette -- which is not Doom's, so it has to be converted to
//   truecolour at load and registered as a texture. See QuakePalette in the cpp.
//
//   VERTICES SIT ON A SEAM. MDL stores one texcoord per vertex plus an `onseam`
//   flag, and a triangle whose `facesfront` is 0 shifts its seam vertices by
//   half the skin width -- so one vertex needs TWO texcoords depending on which
//   triangle is using it. DMD has one texcoord per vertex and no such concept,
//   so seam vertices are DUPLICATED at load: the back copy gets the shifted s,
//   and back-facing triangles are repointed at it. This is why numVertices here
//   can exceed the count in the file header.
//
// The scale and origin are GLOBAL in MDL (in the file header) where MD2 has them
// per frame. Every frame is expanded with the same pair.
//
// Group frames and group skins (an animated skin, or several frames sharing one
// interval table) are read and their FIRST entry used. Nothing in the model sets
// this fork ships uses them, and silently taking one frame is better than
// refusing to load the model.
//

class FMDLModel : public FDMDModel
{
public:
	FMDLModel() { mLumpNum = -1; }
	virtual ~FMDLModel();

	virtual bool Load(const char * fn, int lumpnum, const char * buffer, int length) override;
	virtual void LoadGeometry() override;
	virtual void LoadGeometry(FileSys::FileData* lumpData) override;
	virtual void AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids) override;
	// Frees ONLY what LoadGeometry made. See models_mdl.cpp.
	virtual void UnloadGeometry() override;

private:
	// Filled by Load(), consumed by LoadGeometry(). MDL's sections are laid out
	// back to back with no offset table, and the skin size is variable, so the
	// offsets have to be computed once while walking the header rather than read
	// out of it.
	int mFrameDataOfs = 0;      // first byte of the frame array
	int mVertexCount = 0;       // vertices IN THE FILE, before seam duplication

	float mScale[3] = { 1.f, 1.f, 1.f };
	float mOrigin[3] = { 0.f, 0.f, 0.f };

	// Seam expansion, built in Load() and applied per frame in LoadGeometry().
	// mSourceVert[i] is which FILE vertex the i'th expanded vertex came from, so
	// a duplicated seam vertex reads the same packed position as its original
	// and differs only in texcoord.
	TArray<int> mSourceVert;
};
