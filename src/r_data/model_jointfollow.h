/*
** model_jointfollow.h
**
** RS FORK -- A CHILD RIDING A MODEL JOINT AS IT WAS DRAWN (AActor::FollowActorJoint). Engine docs/MODEL_JOINT_DRIVE_PLAN.md
** piece E, under the Body IK lane's section 11 condition 8.
**
** THE CATCH IS DRAW ORDER. FollowActorSlot solves its surface part again for the child (SurfaceSlotPoseForFollower): a
** surface part is one formula. A joint is not. It is the parent's animation, then the joint draw poses, joint offsets
** and hand drives, reach chains, target aims and the test channel, in model_reach.cpp's order, some of them solved once
** a frame from state the parent's own draw keeps. A second copy of all that for the child would drift the first time
** the layer gains a step, and a solve started from the child's draw would be a second writer on the layer's per-frame
** state.
**
** SO THE CHILD READS WHAT THE PARENT'S DRAW PUBLISHED (condition 8, "Option 2"). ModelDrawPose_Apply hands over the
** finished palette -- the matrices the GPU skins with, after every edit -- and for each joint a follower asked for, the
** draw keeps that joint's matrix. A child drawn after its parent rides this frame's. One drawn before rides the frame
** the parent was last drawn in: one frame late. WHICH drawn frame is decided at the first read of a frame and held for
** the rest of it, for every follower of that parent, so the second eye, a mirror, and a hand and a sight on one gun all
** ride the same one, whatever the parent draws in between.
**
** READ ONLY IN THE RENDER WINDOW (model_reach.h, FModelRenderWindow). ObjectToWorldMatrix also answers script --
** ModelPointToWorld, GetBonePosition, GetObjectToWorldMatrix -- and there a joint follower rides the whole model, so a
** picture built from this machine's hands never reaches a playsim decision (netplay).
**
** THIS FILE IS THE PURE HALF: no actor, no model, no renderer, no VR runtime, so a harness can replay it. model_reach.cpp
** keeps the per-parent table (GC-marked), publishes from the draw and reads for ModelFollowFrame (models.cpp), which
** carries the motion into the world with JointFollowCarry. Nothing here is saved.
*/

#pragma once

#include <cmath>
#include <cstdint>
#include "vectors.h"
#include "matrix.h"

// ---- what one draw of the parent published for one joint a follower asked for ---------------------------------------

struct FJointFollowDrawn
{
	bool     valid = false;
	uint64_t frame = 0;							// DFrameBuffer::FrameCount of the draw
	bool     found = false;						// the joint is on the model that draw skinned
	VSMatrix palette;							// its final palette matrix, exactly as uploaded: bind-pose model space -> drawn
	FVector3 bindOrigin = FVector3(0, 0, 0);	// its own origin at the bind pose, in model space (the file's (x, z, y))
};

// ---- one joint a follower asked for ----------------------------------------------------------------------------------

struct FJointFollowWant
{
	bool     used = false;
	int      modelIndex = 0;		// the parent's model index the joint is on
	bool     shared = false;		// the parent skins every model with ONE palette (+DECOUPLEDANIMATIONS, MODELSAREATTACHMENTS):
									// served by whichever model index that palette is drawn at, and modelIndex is not read
	int      joint = 0;				// the joint's name, as its FName index
	uint64_t readMs = 0;			// DFrameBuffer::FrameTime of the last read: a want nobody reads is dropped
	bool     missingSaid = false;	// the parent's line saying the joint is not on its model was printed
	bool     servedNothing = false;	// a follower read this want and rode nothing ...
	uint64_t servedNothingFrame = 0;	// ... in this frame
	int      cur = 0;
	FJointFollowDrawn drawn[2];		// the latest two drawn frames: the one being ridden may be the older

	bool Serves(int drawnModelIndex) const { return used && (shared || modelIndex == drawnModelIndex); }

	// THE FIRST DRAW OF A FRAME STANDS. A second draw of the parent in the same frame -- the other eye, a mirror --
	// skins with the same palette (the layer replays its frame cache, the hand drive its frame stamp), and a follower may
	// already be riding the first, so it is never replaced. False when nothing was written.
	bool Publish(uint64_t frame, bool found, const VSMatrix &palette, const FVector3 &bindOrigin)
	{
		if (drawn[cur].valid && drawn[cur].frame == frame) return false;
		cur ^= 1;
		FJointFollowDrawn &d = drawn[cur];
		d.valid = true;
		d.frame = frame;
		d.found = found;
		if (found)
		{
			d.palette = palette;
			d.bindOrigin = bindOrigin;
		}
		return true;
	}

	const FJointFollowDrawn *Drawn(uint64_t frame) const
	{
		for (const auto &d : drawn)
			if (d.valid && d.frame == frame) return &d;
		return nullptr;
	}
};

// ---- one parent: its wants, and the drawn frame its followers ride this frame ----------------------------------------

struct FJointFollowParentState
{
	static constexpr int kWants = 8;
	FJointFollowWant wants[kWants];

	bool     published = false;		// a draw has published for this parent
	uint64_t lastDrawn = 0;			// ... the frame of the latest one
	bool     servedValid = false;	// this frame's choice is made
	uint64_t servedFrame = 0;		// ... for this frame
	bool     servedAny = false;		// ... and there was a drawn frame to choose
	uint64_t servedFrom = 0;		// ... which one

	int Count() const
	{
		int c = 0;
		for (const auto &w : wants) c += w.used ? 1 : 0;
		return c;
	}

	FJointFollowWant *Find(int modelIndex, bool shared, int joint)
	{
		for (auto &w : wants)
			if (w.used && w.joint == joint && w.shared == shared && (shared || w.modelIndex == modelIndex)) return &w;
		return nullptr;
	}

	FJointFollowWant *Add(int modelIndex, bool shared, int joint, uint64_t nowMs)
	{
		for (auto &w : wants)
		{
			if (w.used) continue;
			w = FJointFollowWant();
			w.used = true;
			w.modelIndex = modelIndex;
			w.shared = shared;
			w.joint = joint;
			w.readMs = nowMs;
			return &w;
		}
		return nullptr;
	}

	void DropStale(uint64_t nowMs, uint64_t keepMs)
	{
		for (auto &w : wants)
			if (w.used && nowMs > w.readMs && nowMs - w.readMs > keepMs) w = FJointFollowWant();
	}

	// ONE DRAW OF MODEL INDEX `modelIndex`: its final palette (n joints) and its bind pose (n matrices, the joints'
	// globals in file space, IQMModel's baseframe). Every want it serves takes its joint's matrix and bind origin.
	// findJoint(nameIndex) answers the joint's index on the model drawn, or -1; sayMissing(want) is told once when a
	// want's joint is not on it, and again only after it has been found in between.
	template<class FindJoint, class SayMissing>
	void PublishDraw(uint64_t frame, int modelIndex, const VSMatrix *palette, const VSMatrix *bind, int n, FindJoint findJoint, SayMissing sayMissing)
	{
		bool any = false;
		for (auto &w : wants)
		{
			if (!w.Serves(modelIndex)) continue;
			any = true;
			if (w.drawn[w.cur].valid && w.drawn[w.cur].frame == frame) continue;	// the frame's first draw stands
			// A want already served NOTHING from this very frame stays empty for the rest of it. It began between two draws
			// of the frame (after the first eye's parent, before the second's): filled now, it would ride the joint in one
			// eye and the whole model in the other. It rides from the next drawn frame on.
			if (servedValid && servedFrame == frame && servedAny && servedFrom == frame && w.servedNothing && w.servedNothingFrame == frame) continue;
			const int j = findJoint(w.joint);
			if (j >= 0 && j < n)
			{
				// The bind origin in model space: the baseframe's translation, file (x, y, z) -> model (x, z, y).
				const FLOATTYPE *b = bind[j].get();
				w.Publish(frame, true, palette[j], FVector3((float)b[12], (float)b[14], (float)b[13]));
				w.missingSaid = false;
			}
			else
			{
				w.Publish(frame, false, VSMatrix(0), FVector3(0, 0, 0));
				if (!w.missingSaid)
				{
					w.missingSaid = true;
					sayMissing(w);
				}
			}
		}
		if (any)
		{
			published = true;
			lastDrawn = frame;
		}
	}

	// THE DRAWN FRAME A FOLLOWER RIDES IN `frame`. Decided at the first read of the frame -- the parent's latest draw at
	// that moment: this frame's when the parent was drawn first, the last frame it was drawn in otherwise -- and held for
	// every later read in the frame, so both eyes and every follower of this parent ride one drawn frame. Null when this
	// want has none from that frame (the parent was never drawn with bones since, or the want is newer than that draw),
	// and then null for the rest of the frame (PublishDraw).
	const FJointFollowDrawn *Choose(FJointFollowWant &w, uint64_t frame)
	{
		if (!servedValid || servedFrame != frame)
		{
			servedValid = true;
			servedFrame = frame;
			servedAny = published;
			servedFrom = lastDrawn;
		}
		const FJointFollowDrawn *d = servedAny ? w.Drawn(servedFrom) : nullptr;
		if (d == nullptr)
		{
			w.servedNothing = true;
			w.servedNothingFrame = frame;
		}
		return d;
	}
};

// ---- the joint's motion, as a rigid part transform -------------------------------------------------------------------
//
// A palette matrix takes a bind-pose point to where a vertex skinned wholly to the joint is drawn, in the parent's model
// space: the identity at the bind pose, as a surface part's transform is at rest. A child rides it RIGIDLY. The joint's
// own origin goes exactly where the joint is drawn; the child turns with the joint's X axis, and its Y axis as near as a
// rotation can; the joint's scale, or a mirror, never reaches the child -- as FollowActor never takes the parent's scale.
// On a joint of unit scale (every rigid rig) this IS the palette matrix, to rounding, so a point on the bind-pose mesh
// lands on the drawn mesh. Column-major doubles, as VSMatrix lays them out.
//
// False when there is no rotation to ride: the joint is drawn collapsed (a hidden joint's scale is 0), sheared flat, or
// something is not finite. The caller then rides the whole model.
inline bool JointFollowRigid(const VSMatrix &palette, const FVector3 &bindOrigin, double out[16])
{
	const FLOATTYPE *m = palette.get();
	static const int used[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };
	for (int i : used)
		if (!std::isfinite((double)m[i])) return false;

	double x[3] = { m[0], m[1], m[2] };
	double y[3] = { m[4], m[5], m[6] };
	const double lx = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
	if (!(lx > 1e-6)) return false;
	for (double &c : x) c /= lx;
	const double along = y[0] * x[0] + y[1] * x[1] + y[2] * x[2];
	for (int i = 0; i < 3; i++) y[i] -= along * x[i];
	const double ly = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
	if (!(ly > 1e-6)) return false;
	for (double &c : y) c /= ly;
	const double z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };

	// The joint's origin where the palette draws it, less where the rotation alone would carry it.
	const double o[3] = { bindOrigin.X, bindOrigin.Y, bindOrigin.Z };
	for (int i = 0; i < 3; i++)
	{
		const double drawn = double(m[i]) * o[0] + double(m[4 + i]) * o[1] + double(m[8 + i]) * o[2] + double(m[12 + i]);
		out[i] = x[i];
		out[4 + i] = y[i];
		out[8 + i] = z[i];
		out[12 + i] = drawn - (x[i] * o[0] + y[i] * o[1] + z[i] * o[2]);
	}
	out[3] = out[7] = out[11] = 0.0;
	out[15] = 1.0;
	return true;
}

// ---- carried into the world ------------------------------------------------------------------------------------------
//
// FollowActorSlot's carry, with the joint's motion in the part transform's place:
//     frame = parentMat * motion * parentMat^-1 * frame
// into the parent's model space, the joint's motion, back out. At the bind pose the frame is unchanged, so a child seated
// against the whole model sits the same against the joint. With a FollowActorOfsInModel seat P (resolved against the
// frame BEFORE this, as the slot's is) the child lands at parentMat * motion * P. A mirror in parentMat (a negative
// MODELDEF Scale) cancels in the conjugation.
//
// DOUBLE PRECISION, TRANSLATIONS CANCELLED FIRST, as FollowSeatFromModelPoint works: parentMat and the frame both carry a
// world position thousands of units out, and cancelling that inside a float inverse costs a visible fraction of a unit.
// Only parentMat's 3x3 is inverted (Cramer's rule), and the frame's origin is taken relative to the parent's before
// anything turns it. False, with the frame untouched, when that 3x3 is singular, the frame is not affine, or anything is
// not finite.
inline bool JointFollowCarry(const VSMatrix &parentMat, const double motion[16], VSMatrix &frame)
{
	const FLOATTYPE *a = parentMat.get();
	const FLOATTYPE *f = frame.get();
	if (std::fabs((double)f[3]) > 1e-6 || std::fabs((double)f[7]) > 1e-6 || std::fabs((double)f[11]) > 1e-6
		|| std::fabs((double)f[15] - 1.0) > 1e-6) return false;

	// [r][c], from column-major storage.
	double L[3][3], F[3][3], M[3][3], Li[3][3];
	double tA[3], tF[3], tM[3];
	for (int r = 0; r < 3; r++)
	{
		for (int c = 0; c < 3; c++)
		{
			L[r][c] = a[c * 4 + r];
			F[r][c] = f[c * 4 + r];
			M[r][c] = motion[c * 4 + r];
		}
		tA[r] = a[12 + r];
		tF[r] = f[12 + r];
		tM[r] = motion[12 + r];
	}

	const double det = L[0][0] * (L[1][1] * L[2][2] - L[1][2] * L[2][1])
		- L[0][1] * (L[1][0] * L[2][2] - L[1][2] * L[2][0])
		+ L[0][2] * (L[1][0] * L[2][1] - L[1][1] * L[2][0]);
	double norm = 1.0;
	for (int c = 0; c < 3; c++) norm *= std::sqrt(L[0][c] * L[0][c] + L[1][c] * L[1][c] + L[2][c] * L[2][c]);
	if (!std::isfinite(det) || !(norm > 0.0) || !(std::fabs(det) > 1e-9 * norm)) return false;
	Li[0][0] = (L[1][1] * L[2][2] - L[1][2] * L[2][1]) / det;
	Li[0][1] = (L[0][2] * L[2][1] - L[0][1] * L[2][2]) / det;
	Li[0][2] = (L[0][1] * L[1][2] - L[0][2] * L[1][1]) / det;
	Li[1][0] = (L[1][2] * L[2][0] - L[1][0] * L[2][2]) / det;
	Li[1][1] = (L[0][0] * L[2][2] - L[0][2] * L[2][0]) / det;
	Li[1][2] = (L[0][2] * L[1][0] - L[0][0] * L[1][2]) / det;
	Li[2][0] = (L[1][0] * L[2][1] - L[1][1] * L[2][0]) / det;
	Li[2][1] = (L[0][1] * L[2][0] - L[0][0] * L[2][1]) / det;
	Li[2][2] = (L[0][0] * L[1][1] - L[0][1] * L[1][0]) / det;

	// Y = parentMat^-1 * frame, the frame's origin relative to the parent's first.
	double Y[3][3], tY[3], d[3];
	for (int r = 0; r < 3; r++) d[r] = tF[r] - tA[r];
	for (int r = 0; r < 3; r++)
	{
		for (int c = 0; c < 3; c++) Y[r][c] = Li[r][0] * F[0][c] + Li[r][1] * F[1][c] + Li[r][2] * F[2][c];
		tY[r] = Li[r][0] * d[0] + Li[r][1] * d[1] + Li[r][2] * d[2];
	}
	// Z = motion * Y
	double Z[3][3], tZ[3];
	for (int r = 0; r < 3; r++)
	{
		for (int c = 0; c < 3; c++) Z[r][c] = M[r][0] * Y[0][c] + M[r][1] * Y[1][c] + M[r][2] * Y[2][c];
		tZ[r] = M[r][0] * tY[0] + M[r][1] * tY[1] + M[r][2] * tY[2] + tM[r];
	}
	// W = parentMat * Z, the parent's origin added back last.
	FLOATTYPE out[16];
	for (int r = 0; r < 3; r++)
	{
		for (int c = 0; c < 3; c++)
		{
			const double v = L[r][0] * Z[0][c] + L[r][1] * Z[1][c] + L[r][2] * Z[2][c];
			if (!std::isfinite(v)) return false;
			out[c * 4 + r] = (FLOATTYPE)v;
		}
		const double t = L[r][0] * tZ[0] + L[r][1] * tZ[1] + L[r][2] * tZ[2] + tA[r];
		if (!std::isfinite(t)) return false;
		out[12 + r] = (FLOATTYPE)t;
	}
	out[3] = out[7] = out[11] = 0;
	out[15] = 1;
	frame.loadMatrix(out);
	return true;
}
