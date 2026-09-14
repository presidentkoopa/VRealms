/*
** model_reach.h
**
** RS FORK -- DRAW-TIME JOINT POSES AND REACH CHAINS.
**
** Two general capabilities for any rigged (IQM) world model, both render-only:
**
**   A. A JOINT DRAW POSE. Per actor, per model index, per joint: a rotation
**      multiplied into or replacing the joint's local rotation, or the joint
**      hidden (its subtree collapses). Applied on the drawn bones AFTER the
**      animation and the stock bone overrides, so the three compose.
**      Actor.SetModelJointDrawPose / ClearModelJointDrawPose.
**
**   B. A REACH CHAIN. Three joints (root -> mid -> end) that bend, every drawn
**      frame, so the end joint lands on a point of ANOTHER actor's model as that
**      model is drawn this frame -- its controller, its MODELDEF offsets and its
**      live placement sliders included. An arm reaching a hand, a leg reaching a
**      boot, a bipod reaching a surface. Tuned live through a cvar prefix the
**      renderer reads. Actor.SetModelReachChain and its neighbours.
**
** WHY THE STOCK BONE API CANNOT DO THIS (VR_BODY_IK_RETURN_PLAN.md section 3): it is saved
** with the game, stamped with the tic clock and blended toward a value script can
** change once a tic, refuses any actor without +DECOUPLEDANIMATIONS, and recomputes
** bones on every set. A hand moves at the display rate; an arm that follows it has to
** be computed at the display rate, from the same matrix that draws the hand.
**
** RENDER-ONLY, AND THAT IS THE NETPLAY CONTRACT. Nothing here is saved, nothing is
** readable from ZScript (setters only), nothing runs in AActor::CalcBones, so
** GetBoneMatrix / TransformByNamedBone / GetBonePosition answer exactly as before.
** A pose built from this machine's controllers can change only this machine's pixels.
**
** INERT UNTIL SET. With no pose, no chain and r_jointpose_test empty, a model draw
** costs one integer and one bool test here, and nothing else changes.
**
** Where it plugs in (models.cpp): RenderModel opens a FModelDrawPoseScope for the
** actor it is drawing, carrying that draw's exact object matrix; RenderModelFrame
** hands each finished bone palette to ModelDrawPose_Apply before it is uploaded.
** Nothing else in the model path -- ObjectToWorldMatrix, RenderHUDModel,
** ProcessModelFrame, CalcModelFrame -- is touched.
**
**---------------------------------------------------------------------------
*/

#pragma once

#include "tarray.h"
#include "matrix.h"

class AActor;
class FModel;

// How many actors currently carry a pose or a chain. Zero keeps every model draw on
// the old path. Maintained by the setters in model_reach.cpp.
extern int  ModelDrawPose_RegisteredCount;
// r_jointpose_test is non-empty: the test channel bends that joint on every world
// model drawn. Kept as a bool by the cvar's callback so a draw never reads a string.
extern bool ModelDrawPose_TestOn;

// The render-only window. Opened by RenderModel, around one world-model draw, for the
// actor being drawn; closed when RenderModel returns. The palette is posed only inside
// it and only for that actor, which is what keeps the layer off every other caller of
// RenderFrameModels -- the HUD path, the VR wheel -- and out of AActor::CalcBones,
// which never enters RenderModel at all.
class FModelDrawPoseScope
{
public:
	FModelDrawPoseScope(const AActor *actor, const VSMatrix &objectToWorld, double ticFrac)
	{
		if (ModelDrawPose_RegisteredCount > 0 || ModelDrawPose_TestOn) Open(actor, objectToWorld, ticFrac);
	}
	~FModelDrawPoseScope()
	{
		if (opened) Close();
	}
	FModelDrawPoseScope(const FModelDrawPoseScope &) = delete;
	FModelDrawPoseScope &operator=(const FModelDrawPoseScope &) = delete;

private:
	void Open(const AActor *actor, const VSMatrix &objectToWorld, double ticFrac);
	void Close();

	bool opened = false;
	// The window that was open before this one, restored on Close. Model draws do not
	// nest today; this costs nothing and keeps a future nested draw honest.
	bool prevOpen = false;
	const AActor *prevActor = nullptr;
	VSMatrix prevObjectToWorld;
	double prevTicFrac = 0.;
};

const TArray<VSMatrix> *ModelDrawPose_ApplyOpen(const AActor *actor, FModel *model, int modelIndex, const TArray<VSMatrix> &bones);

// A finished bone palette for model index `modelIndex` of `actor`, as the model path
// computed it -- animation and stock overrides already in. Returns `&bones` untouched
// unless a window is open for this actor and something poses it; otherwise a posed
// copy, valid until the next call. Never hand it the base-pose fallback: that is not
// a palette.
inline const TArray<VSMatrix> *ModelDrawPose_Apply(const AActor *actor, FModel *model, int modelIndex, const TArray<VSMatrix> &bones)
{
	if (ModelDrawPose_RegisteredCount <= 0 && !ModelDrawPose_TestOn) return &bones;
	return ModelDrawPose_ApplyOpen(actor, model, modelIndex, bones);
}
