//
// roth_physics_core -- see the header. Jolt Physics v5.3.0 (libraries/JoltPhysics, MIT), single threaded: the world is
// a few rooms, two hands and a handful of loose items, stepped once a game tic.
//
//---------------------------------------------------------------------------
// SPDX-License-Identifier: GPL-3.0-or-later
//---------------------------------------------------------------------------
//
#include "roth_physics_core.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>

JPH_SUPPRESS_WARNINGS

namespace roth
{
namespace phys
{

double V3::Length() const { return std::sqrt(x * x + y * y + z * z); }

Q Q::FromBasis(const V3 &X, const V3 &Y, const V3 &Z)
{
	// Shepperd's method on the matrix whose columns are X Y Z.
	const double m00 = X.x, m10 = X.y, m20 = X.z, m01 = Y.x, m11 = Y.y, m21 = Y.z, m02 = Z.x, m12 = Z.y, m22 = Z.z;
	const double tr = m00 + m11 + m22;
	Q q;
	if (tr > 0)
	{
		const double s = std::sqrt(tr + 1.0) * 2;
		q = Q((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25 * s);
	}
	else if (m00 > m11 && m00 > m22)
	{
		const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2;
		q = Q(0.25 * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
	}
	else if (m11 > m22)
	{
		const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2;
		q = Q((m01 + m10) / s, 0.25 * s, (m12 + m21) / s, (m02 - m20) / s);
	}
	else
	{
		const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2;
		q = Q((m02 + m20) / s, (m12 + m21) / s, 0.25 * s, (m10 - m01) / s);
	}
	return q.Normalized();
}

void Q::Basis(V3 &X, V3 &Y, V3 &Z) const
{
	X = Rotate(V3(1, 0, 0));
	Y = Rotate(V3(0, 1, 0));
	Z = Rotate(V3(0, 0, 1));
}

Q Q::operator*(const Q &o) const
{
	return Q(w * o.x + x * o.w + y * o.z - z * o.y,
	         w * o.y - x * o.z + y * o.w + z * o.x,
	         w * o.z + x * o.y - y * o.x + z * o.w,
	         w * o.w - x * o.x - y * o.y - z * o.z);
}

V3 Q::Rotate(const V3 &v) const
{
	const V3 u(x, y, z);
	const V3 t = u.Cross(v) * 2.0;
	return v + t * w + u.Cross(t);
}

Q Q::Normalized() const
{
	const double l = std::sqrt(x * x + y * y + z * z + w * w);
	return l > 1e-12 ? Q(x / l, y / l, z / l, w / l) : Q();
}

void ModelAngles(const Q &q, double &yaw, double &pitch, double &roll)
{
	// R = Rz(yaw) Ry(theta) Rx(roll) with theta = -pitch: R[2][0] = -sin(theta).
	V3 X, Y, Z;
	q.Basis(X, Y, Z);   // columns
	const double R20 = X.z, R10 = X.y, R00 = X.x, R21 = Y.z, R22 = Z.z;
	const double R2D = 57.29577951308232;
	const double theta = std::asin(std::max(-1.0, std::min(1.0, -R20)));
	if (std::fabs(R20) < 0.99999)
	{
		yaw = std::atan2(R10, R00) * R2D;
		roll = std::atan2(R21, R22) * R2D;
	}
	else
	{
		// gimbal: straight up or down; the yaw takes it all
		yaw = std::atan2(-Y.x, Y.y) * R2D;
		roll = 0;
	}
	pitch = -theta * R2D;
}

namespace
{

using namespace JPH;

namespace Layer
{
	constexpr ObjectLayer Static = 0;
	constexpr ObjectLayer Item = 1;
	constexpr ObjectLayer Hand = 2;
	constexpr ObjectLayer Count = 3;
}
namespace BPLayer
{
	constexpr BroadPhaseLayer Still(0);
	constexpr BroadPhaseLayer Moving(1);
	constexpr uint Count = 2;
}

class PairFilter final : public ObjectLayerPairFilter
{
public:
	bool ShouldCollide(ObjectLayer a, ObjectLayer b) const override
	{
		if (a == Layer::Static) return b != Layer::Static;
		if (a == Layer::Hand) return b != Layer::Hand;   // the hands pass through each other
		return true;
	}
};

class BPInterface final : public BroadPhaseLayerInterface
{
public:
	uint GetNumBroadPhaseLayers() const override { return BPLayer::Count; }
	BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer l) const override { return l == Layer::Static ? BPLayer::Still : BPLayer::Moving; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char *GetBroadPhaseLayerName(BroadPhaseLayer l) const override { return l == BPLayer::Still ? "still" : "moving"; }
#endif
};

class ObjVsBP final : public ObjectVsBroadPhaseLayerFilter
{
public:
	bool ShouldCollide(ObjectLayer a, BroadPhaseLayer b) const override { return a != Layer::Static || b == BPLayer::Moving; }
};

// Body user data: kind in the top byte, the hand's index below.
constexpr uint64 kUserHand = 1ull << 56;
constexpr uint64 kUserItem = 2ull << 56;

class Contacts final : public ContactListener
{
public:
	std::mutex lock;
	double hit[2] = { 0, 0 };
	int touching[2] = { 0, 0 };   // contacts added minus removed is unknowable here; counted per step instead
	bool touchedThisStep[2] = { false, false };

	void Note(const Body &a, const Body &b, const ContactManifold &m)
	{
		const RVec3 p = m.GetWorldSpaceContactPointOn1(0);
		const Vec3 rel = b.GetPointVelocity(p) - a.GetPointVelocity(p);
		const double closing = -(double)rel.Dot(m.mWorldSpaceNormal);   // normal points from a to b
		for (const Body *x : { &a, &b })
		{
			const uint64 u = x->GetUserData();
			if ((u & (0xffull << 56)) != kUserHand) continue;
			const int h = (int)(u & 1);
			std::lock_guard<std::mutex> g(lock);
			touchedThisStep[h] = true;
			if (closing > hit[h]) hit[h] = closing;
		}
	}
	void OnContactAdded(const Body &a, const Body &b, const ContactManifold &m, ContactSettings &) override { Note(a, b, m); }
	void OnContactPersisted(const Body &a, const Body &b, const ContactManifold &m, ContactSettings &) override
	{
		const uint64 ua = a.GetUserData(), ub = b.GetUserData();
		std::lock_guard<std::mutex> g(lock);
		for (uint64 u : { ua, ub })
			if ((u & (0xffull << 56)) == kUserHand) touchedThisStep[u & 1] = true;
	}
};

int gRefs = 0;

void JoltTrace(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	fprintf(stderr, "[jolt] %s\n", buf);
}

void JoltUp()
{
	if (gRefs++ > 0) return;
	RegisterDefaultAllocator();
	JPH::Trace = JoltTrace;
	Factory::sInstance = new Factory();
	RegisterTypes();
}

void JoltDown()
{
	if (--gRefs > 0) return;
	UnregisterTypes();
	delete Factory::sInstance;
	Factory::sInstance = nullptr;
}

inline Vec3 J(const V3 &v) { return Vec3((float)v.x, (float)v.y, (float)v.z); }
inline Quat J(const Q &q) { return Quat((float)q.x, (float)q.y, (float)q.z, (float)q.w).Normalized(); }
inline V3 P(Vec3Arg v) { return V3(v.GetX(), v.GetY(), v.GetZ()); }
inline Q P(QuatArg q) { return Q(q.GetX(), q.GetY(), q.GetZ(), q.GetW()); }

// A convex hull of the points, or a box round them when they are flat or too few (paper, a coin seen edge on).
RefConst<Shape> ItemShapeOf(const std::vector<V3> &pts)
{
	if (pts.empty()) return new BoxShape(Vec3(0.02f, 0.02f, 0.02f));
	V3 lo = pts[0], hi = pts[0];
	for (const V3 &p : pts)
	{
		lo = V3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
		hi = V3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
	}
	const V3 ext = hi - lo;
	const double thin = std::min(ext.x, std::min(ext.y, ext.z));
	if (pts.size() >= 4 && thin > 0.004)
	{
		Array<Vec3> a;
		a.reserve(pts.size());
		for (const V3 &p : pts) a.push_back(J(p));
		ConvexHullShapeSettings s(a, std::min(0.005f, (float)thin * 0.2f));
		Shape::ShapeResult r = s.Create();
		if (r.IsValid()) return r.Get();
	}
	// a box, at least 4 mm thick so it can rest
	const V3 half(std::max(ext.x * 0.5, 0.002), std::max(ext.y * 0.5, 0.002), std::max(ext.z * 0.5, 0.002));
	const V3 c = (lo + hi) * 0.5;
	RotatedTranslatedShapeSettings rt(J(c), Quat::sIdentity(), new BoxShapeSettings(J(half), std::min(0.002f, (float)std::min(half.x, std::min(half.y, half.z)) * 0.5f)));
	Shape::ShapeResult r = rt.Create();
	return r.IsValid() ? r.Get() : RefConst<Shape>(new BoxShape(Vec3(0.02f, 0.02f, 0.02f)));
}

} // namespace

struct World::Impl
{
	BPInterface bp;
	ObjVsBP objVsBp;
	PairFilter pairs;
	Contacts contacts;
	std::unique_ptr<TempAllocatorImpl> temp;
	std::unique_ptr<JobSystemSingleThreaded> jobs;
	PhysicsSystem sys;
	std::map<int, BodyID> statics;
	std::map<int, size_t> staticTris;
	struct HandRec { BodyID body; HandShape palm; Pose target; bool haveTarget = false; double hit = 0; bool touching = false; };
	std::vector<HandRec> hands;
	std::vector<BodyID> items;
	bool ok = false;

	BodyInterface &BI() { return sys.GetBodyInterface(); }
	const BodyInterface &BI() const { return sys.GetBodyInterface(); }

	RefConst<Shape> HandShapeWith(const HandShape &palm, const ItemShape *held, const Pose &rel)
	{
		StaticCompoundShapeSettings c;
		c.AddShape(J(palm.centre), Quat::sIdentity(), new BoxShape(J(palm.half), 0.01f));
		if (held != nullptr)
		{
			RefConst<Shape> hs = ItemShapeOf(held->hull);
			c.AddShape(J(rel.p), J(rel.q), hs);
		}
		Shape::ShapeResult r = c.Create();
		if (r.IsValid()) return r.Get();
		return new BoxShape(J(palm.half), 0.01f);
	}
};

World::World() : I(new Impl)
{
	JoltUp();
	I->temp.reset(new TempAllocatorImpl(8 * 1024 * 1024));
	I->jobs.reset(new JobSystemSingleThreaded(cMaxPhysicsJobs));
	I->sys.Init(8192, 0, 8192, 8192, I->bp, I->objVsBp, I->pairs);
	I->sys.SetContactListener(&I->contacts);
	I->sys.SetGravity(Vec3(0, 0, -9.81f));   // the map's z is up
	PhysicsSettings ps = I->sys.GetPhysicsSettings();
	ps.mPointVelocitySleepThreshold = 0.02f;
	I->sys.SetPhysicsSettings(ps);
	I->ok = true;
}

World::~World()
{
	ClearStatic();
	for (BodyID id : I->items) { I->BI().RemoveBody(id); I->BI().DestroyBody(id); }
	for (auto &h : I->hands) { I->BI().RemoveBody(h.body); I->BI().DestroyBody(h.body); }
	I.reset();
	JoltDown();
}

bool World::Ok() const { return I && I->ok; }

void World::SetStatic(int key, const std::vector<float> &tris)
{
	auto it = I->statics.find(key);
	if (it != I->statics.end())
	{
		I->BI().RemoveBody(it->second);
		I->BI().DestroyBody(it->second);
		I->statics.erase(it);
		I->staticTris.erase(key);
	}
	if (tris.size() < 9) return;
	TriangleList list;
	list.reserve(tris.size() / 9);
	for (size_t i = 0; i + 8 < tris.size(); i += 9)
	{
		const Float3 a(tris[i], tris[i + 1], tris[i + 2]), b(tris[i + 3], tris[i + 4], tris[i + 5]), c(tris[i + 6], tris[i + 7], tris[i + 8]);
		// both faces: a Jolt mesh collides on its front only, and a level's wall is solid from either side
		list.push_back(Triangle(a, b, c));
		list.push_back(Triangle(a, c, b));
	}
	MeshShapeSettings ms(list);
	ms.mActiveEdgeCosThresholdAngle = 0.996195f;   // 5 degrees
	Shape::ShapeResult r = ms.Create();
	if (!r.IsValid()) return;
	BodyCreationSettings bs(r.Get(), RVec3::sZero(), Quat::sIdentity(), EMotionType::Static, Layer::Static);
	bs.mFriction = 0.7f;
	const BodyID id = I->BI().CreateAndAddBody(bs, EActivation::DontActivate);
	if (id.IsInvalid()) return;
	I->statics[key] = id;
	I->staticTris[key] = list.size() / 2;
}

void World::ClearStatic()
{
	for (auto &p : I->statics) { I->BI().RemoveBody(p.second); I->BI().DestroyBody(p.second); }
	I->statics.clear();
	I->staticTris.clear();
}

size_t World::StaticTriangles() const
{
	size_t n = 0;
	for (auto &p : I->staticTris) n += p.second;
	return n;
}

Id World::AddItem(const ItemShape &shape, const Pose &at, const V3 &vel, const V3 &angVel)
{
	RefConst<Shape> s = ItemShapeOf(shape.hull);
	BodyCreationSettings bs(s, RVec3(J(at.p)), J(at.q), EMotionType::Dynamic, Layer::Item);
	bs.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
	bs.mMassPropertiesOverride.mMass = (float)std::max(0.02, shape.massKg);
	bs.mMotionQuality = EMotionQuality::LinearCast;
	bs.mFriction = (float)shape.friction;
	bs.mRestitution = (float)shape.restitution;
	bs.mLinearDamping = 0.05f;
	bs.mAngularDamping = 0.15f;
	bs.mLinearVelocity = J(vel);
	bs.mAngularVelocity = J(angVel);
	bs.mUserData = kUserItem;
	const BodyID id = I->BI().CreateAndAddBody(bs, EActivation::Activate);
	if (id.IsInvalid()) return kNone;
	I->items.push_back(id);
	return id.GetIndexAndSequenceNumber();
}

void World::Remove(Id id)
{
	const BodyID b(id);
	auto it = std::find(I->items.begin(), I->items.end(), b);
	if (it == I->items.end()) return;
	I->items.erase(it);
	I->BI().RemoveBody(b);
	I->BI().DestroyBody(b);
}

bool World::PoseOf(Id id, Pose &out) const
{
	const BodyID b(id);
	if (!I->BI().IsAdded(b)) return false;
	RVec3 p;
	Quat q;
	I->BI().GetPositionAndRotation(b, p, q);
	out.p = P(Vec3(p));
	out.q = P(q);
	return true;
}

bool World::VelocityOf(Id id, V3 &lin, V3 &ang) const
{
	const BodyID b(id);
	if (!I->BI().IsAdded(b)) return false;
	lin = P(I->BI().GetLinearVelocity(b));
	ang = P(I->BI().GetAngularVelocity(b));
	return true;
}

bool World::Resting(Id id) const
{
	const BodyID b(id);
	return I->BI().IsAdded(b) && !I->BI().IsActive(b);
}

Id World::ItemNear(const V3 &p, double radius, double *dist) const
{
	Id best = kNone;
	double bestD = radius;
	const BodyLockInterfaceNoLock &locks = I->sys.GetBodyLockInterfaceNoLock();
	for (BodyID b : I->items)
	{
		BodyLockRead lk(locks, b);
		if (!lk.Succeeded()) continue;
		const Body &body = lk.GetBody();
		// the distance from p to the body's surface: its shape's closest point, through a ball collision
		const AABox box = body.GetWorldSpaceBounds();
		const Vec3 q = box.GetClosestPoint(J(p));
		double d = P(q - J(p)).Length();
		if (d > bestD) continue;
		// inside the bounds: measure against the shape's own surface (local space)
		const RMat44 inv = body.GetInverseCenterOfMassTransform();
		const Vec3 local = Vec3(inv * RVec3(J(p)));
		const Shape *shape = body.GetShape();
		const AABox lb = shape->GetLocalBounds();
		const Vec3 lq = lb.GetClosestPoint(local);
		d = std::max(d, P(lq - local).Length());
		if (d <= bestD) { bestD = d; best = b.GetIndexAndSequenceNumber(); }
	}
	if (dist != nullptr) *dist = best != kNone ? bestD : -1.0;
	return best;
}

Id World::AddHand(const HandShape &palm, const Pose &at)
{
	Impl::HandRec h;
	h.palm = palm;
	RefConst<Shape> s = I->HandShapeWith(palm, nullptr, Pose());
	BodyCreationSettings bs(s, RVec3(J(at.p)), J(at.q), EMotionType::Dynamic, Layer::Hand);
	bs.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
	bs.mMassPropertiesOverride.mMass = (float)palm.massKg;
	bs.mMotionQuality = EMotionQuality::LinearCast;
	bs.mGravityFactor = 0.0f;
	bs.mAllowSleeping = false;
	bs.mFriction = 0.9f;
	bs.mLinearDamping = 0.0f;
	bs.mAngularDamping = 0.0f;
	bs.mUserData = kUserHand | (uint64)(I->hands.size() & 1);
	h.body = I->BI().CreateAndAddBody(bs, EActivation::Activate);
	if (h.body.IsInvalid()) return kNone;
	I->hands.push_back(h);
	return (Id)(I->hands.size() - 1);
}

void World::HandHolds(Id hand, const ItemShape *shape, const Pose &rel)
{
	if (hand >= I->hands.size()) return;
	Impl::HandRec &h = I->hands[hand];
	I->BI().SetShape(h.body, I->HandShapeWith(h.palm, shape, rel), false, EActivation::Activate);
}

void World::DriveHand(Id hand, const Pose &target, double dt)
{
	if (hand >= I->hands.size() || dt <= 0) return;
	Impl::HandRec &h = I->hands[hand];
	h.target = target;
	h.haveTarget = true;
	RVec3 p;
	Quat q;
	I->BI().GetPositionAndRotation(h.body, p, q);
	const V3 cur = P(Vec3(p));
	if ((target.p - cur).Length() > TeleportM) { TeleportHand(hand, target); return; }
	// Jolt moves the centre of mass and turns about it: aim the centre of mass at where it will be on the target
	const V3 com = P(Vec3(I->BI().GetCenterOfMassPosition(h.body)));
	const V3 comLocal = P(Quat(q).Conjugated() * (Vec3(I->BI().GetCenterOfMassPosition(h.body)) - Vec3(p)));
	const V3 d = target.Apply(comLocal) - com;
	V3 v = d * (1.0 / dt);
	const double sp = v.Length();
	if (sp > MaxHandSpeed) v = v * (MaxHandSpeed / sp);
	// the turn that takes the body's rotation to the target's, as an angular velocity
	Q err = (target.q * P(q).Conj()).Normalized();
	if (err.w < 0) err = Q(-err.x, -err.y, -err.z, -err.w);
	const double s = std::sqrt(err.x * err.x + err.y * err.y + err.z * err.z);
	V3 w(0, 0, 0);
	if (s > 1e-9)
	{
		const double ang = 2.0 * std::atan2(s, err.w);
		w = V3(err.x / s, err.y / s, err.z / s) * (ang / dt);
		const double wl = w.Length();
		if (wl > MaxHandSpin) w = w * (MaxHandSpin / wl);
	}
	I->BI().SetLinearAndAngularVelocity(h.body, J(v), J(w));
}

void World::TeleportHand(Id hand, const Pose &target)
{
	if (hand >= I->hands.size()) return;
	Impl::HandRec &h = I->hands[hand];
	I->BI().SetPositionAndRotation(h.body, RVec3(J(target.p)), J(target.q), EActivation::Activate);
	I->BI().SetLinearAndAngularVelocity(h.body, Vec3::sZero(), Vec3::sZero());
}

bool World::HandPose(Id hand, Pose &out) const
{
	if (hand >= I->hands.size()) return false;
	RVec3 p;
	Quat q;
	I->BI().GetPositionAndRotation(I->hands[hand].body, p, q);
	out.p = P(Vec3(p));
	out.q = P(q);
	return true;
}

double World::HandHit(Id hand) const { return hand < I->hands.size() ? I->hands[hand].hit : 0.0; }
bool World::HandTouching(Id hand) const { return hand < I->hands.size() && I->hands[hand].touching; }

void World::Step(double dt, int collisionSteps)
{
	if (dt <= 0) return;
	{
		std::lock_guard<std::mutex> g(I->contacts.lock);
		I->contacts.hit[0] = I->contacts.hit[1] = 0;
		I->contacts.touchedThisStep[0] = I->contacts.touchedThisStep[1] = false;
	}
	I->sys.Update((float)dt, std::max(1, collisionSteps), I->temp.get(), I->jobs.get());
	for (size_t k = 0; k < I->hands.size() && k < 2; k++)
	{
		I->hands[k].hit = I->contacts.hit[k];
		I->hands[k].touching = I->contacts.touchedThisStep[k];
	}
}

size_t World::Bodies() const { return I->sys.GetNumBodies(); }

} // namespace phys
} // namespace roth
