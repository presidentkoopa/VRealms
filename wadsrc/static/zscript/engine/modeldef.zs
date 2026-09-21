// ============================================================================
// GUN FIT MODE -- the engine's MODELDEF natives (src/r_data/model_fit.cpp).
//
// Offset is in MODELDEF units and axis order: exactly the three numbers an
// `Offset x y z` line holds. Stage 1 changes it in memory only; nothing here
// writes a file.
// ============================================================================

struct ModelDef
{
	// GetOffset's status, and what SetOffset returns when it refuses.
	enum EFitStatus
	{
		FIT_OK        =  1,
		FIT_NOMODEL   =  0,
		FIT_DISAGREE  = -1,	// the class's frames carry different Offsets
		FIT_INHERITED = -2,	// its frames come from `inherits`
	}

	// The class's current Offset, and an EFitStatus.
	native static Vector3, int GetOffset(class<Actor> cls);
	// Writes Offset into every frame of the class and its base frame, live.
	// Returns how many frames changed, or the (<= 0) status when it refuses. The
	// first change of a session keeps the original for RestoreOffset.
	native static int SetOffset(class<Actor> cls, Vector3 ofs);
	// Puts the kept original back. Cancel uses this.
	native static void RestoreOffset(class<Actor> cls);
}
