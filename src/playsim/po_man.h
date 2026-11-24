/*
** po_man.h
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 1994-1996 Raven Software
** Copyright 1999-2016 Marisa Heit
** Copyright 2002-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#ifndef __PO_MAN_H
#define __PO_MAN_H

#include "tarray.h"
#include "r_defs.h"
#include "m_bbox.h"
#include "dthinker.h"

struct FPolyObj;

class DPolyAction : public DThinker
{
	DECLARE_CLASS(DPolyAction, DThinker)
	HAS_OBJECT_POINTERS
public:
	void Construct(FPolyObj *polyNum);
	void Serialize(FSerializer &arc);
	void OnDestroy() override;
	void Stop();
	double GetSpeed() const { return m_Speed; }

	void StopInterpolation();
protected:
	FPolyObj *m_PolyObj;
	double m_Speed;
	double m_Dist;
	TObjPtr<DInterpolation*> m_Interpolation;

	void SetInterpolation();
};

struct FPolyVertex
{
	DVector2 pos;

	FPolyVertex &operator=(vertex_t *v)
	{
		pos = v->fPos();
		return *this;
	}
};

struct FPolySeg
{
	FPolyVertex v1;
	FPolyVertex v2;
	side_t *wall;
};

//
// Linked lists of polyobjects
//
struct FPolyObj;
struct FPolyNode
{
	FPolyObj *poly;				// owning polyobject
	FPolyNode *pnext;			// next polyobj in list
	FPolyNode *pprev;			// previous polyobj

	subsector_t *subsector;		// containimg subsector
	FPolyNode *snext;			// next subsector

	TArray<FPolySeg> segs;		// segs for this node
	int state;
};

// ===== Polyobj data =====
struct FPolyObj
{
	FLevelLocals			*Level;
	TArray<side_t *>		Sidedefs;
	TArray<line_t *>		Linedefs;
	TArray<vertex_t *>		Vertices;
	TArray<FPolyVertex>		OriginalPts;
	TArray<FPolyVertex>		PrevPts;
	FPolyVertex				StartSpot;
	FPolyVertex				CenterSpot;
	FBoundingBox			Bounds;	// Bounds in map coordinates
	subsector_t				*CenterSubsector;
	int						MirrorNum;

	DAngle		Angle;
	int			tag;			// reference tag assigned in HereticEd
	int			bbox[4];		// bounds in blockmap coordinates
	int			validcount;
	int			crush; 			// should the polyobj attempt to crush mobjs?
	bool		bHurtOnTouch;	// should the polyobj hurt anything it touches?
	bool		bBlocked;
	// HELD BY AN OUTSIDE DRIVER (EV_HoldPoly below): its own action thinker waits while something else turns it.
	// Default off; not saved (a hold lasts while a hand is on it).
	bool		bHeldByDriver = false;
	uint8_t		bHasPortals;	// 1 for any portal, 2 for a linked portal (2 must block rotations.)
	int			seqType;
	double		Size;			// polyobj size (area of POLY_AREAUNIT == size of FRACUNIT)
	FPolyNode	*subsectorlinks;
	TObjPtr<DPolyAction*> specialdata;	// pointer to a thinker, if the poly is moving
	TObjPtr<DInterpolation*> interpolation;

	FPolyObj();
	DInterpolation *SetInterpolation();
	void StopInterpolation();

	int GetMirror();
	bool MovePolyobj (const DVector2 &pos, bool force = false);
	bool RotatePolyobj (DAngle angle, bool fromsave = false);
	void ClosestPoint(const DVector2 &fpos, DVector2 &out, side_t **side) const;
	void LinkPolyobj ();
	void RecalcActorFloorCeil(FBoundingBox bounds) const;
	void CreateSubsectorLinks();
	void ClearSubsectorLinks();
	void CalcCenter();
	void UpdateLinks();
	static void ClearAllSubsectorLinks();

private:

	void ThrustMobj (AActor *actor, side_t *side);
	void UpdateBBox ();
	void DoMovePolyobj (const DVector2 &pos);
	void UnLinkPolyobj ();
	bool CheckMobjBlocking (side_t *sd);

};

struct polyblock_t
{
	FPolyObj *polyobj;
	struct polyblock_t *prev;
	struct polyblock_t *next;
};


void PO_LinkToSubsectors(FLevelLocals *Level);


// ===== PO_MAN =====

typedef enum
{
	PODOOR_NONE,
	PODOOR_SLIDE,
	PODOOR_SWING,
} podoortype_t;

bool EV_RotatePoly (FLevelLocals *Level, line_t *line, int polyNum, int speed, int byteAngle, int direction, bool overRide);
bool EV_MovePoly (FLevelLocals *Level, line_t *line, int polyNum, double speed, DAngle angle, double dist, bool overRide);
bool EV_MovePolyTo (FLevelLocals *Level, line_t *line, int polyNum, double speed, const DVector2 &pos, bool overRide);
bool EV_OpenPolyDoor (FLevelLocals *Level, line_t *line, int polyNum, double speed, DAngle angle, int delay, double distance, podoortype_t type);
bool EV_StopPoly (FLevelLocals *Level, int polyNum);

// A POLYOBJECT TURNED BY AN OUTSIDE DRIVER. Made for VRealms' hands moving a door leaf about its hinge
// (src/roth/roth_handworks.cpp, docs/VR_HANDWORKS.md), and for any caller that turns a polyobject by something other
// than its own thinker. Inert until called: nothing else sets the hold.
// - EV_HoldPoly: while held, the polyobject's action thinker (a door's swing) does not advance.
// - EV_TurnHeldPoly: turn a held polyobject by `delta`. What it turns along its swing's own direction is taken off
//   the swing's remaining arc (and what it turns back is added), so when the hold ends the swing finishes its arc from
//   where the driver left the leaf, at its own speed. False: something in the way stopped it (RotatePolyobj's test).
// - EV_PolySwingState: the swing action under way, if any: its sense (+1 turning to positive degrees, -1 negative),
//   the arc it still has to turn, and whether it is a door's closing half.
bool EV_HoldPoly (FLevelLocals *Level, int polyNum, bool hold);
bool EV_TurnHeldPoly (FLevelLocals *Level, int polyNum, DAngle delta);
bool EV_PolySwingState (FLevelLocals *Level, int polyNum, int &sense, double &remainingDeg, bool &closing);

bool PO_Busy (FLevelLocals *Level, int polyobj);


#endif
