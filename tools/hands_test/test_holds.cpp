// tools/hands_test/test_holds.cpp -- DIFFERENTIAL TEST of the held-object and weapon-grip selection against
// UZDXREMA's OWN CODE.
//
// run.sh cuts these lines out of UZDXREMA (read-only) and they are #included below, unedited:
//   vr_avatar_pose.cpp:476-656   which named pose shapes each finger joint of a hand holding a weapon (gun_grip /
//                                grip_<class> / grip_<prop>, the thumb, the index ready->fire by the trigger) or a held
//                                object (hold_<subject>[_<prop>|_<class>], the fallbacks, one pose wins whole)
//   vr_avatar_pose.cpp:672-684   that choice as each joint's rotation target (the index's QSlerp(ready, fire, t); a
//                                named pose's rotation)
//   vr_avatar_pose.cpp:392-414   QSlerp (used by :680)
//   vk_openxrdevice.h:105-131    EGripSubject
// Supplied here: the engine names those lines use (FName, FString, the rig's trigger and validity, the hand's held
// state, VRAvatar_NamedPose / VRAvatar_NamedPoseRot answered from the port's own per-side table -- see "PER SIDE").
//
// THE PORT (vr_handpose_math.h SelectHold + HoldTargets) and the source's lines get the same tables, held states and
// triggers; every joint's target quaternion and every choice (which pose, the index pair, the trigger) must be
// identical. Where the source's joint has no named pose (:685-690, its curl fallback) both sides take the same
// controller target (the port's PoseTargets): that fallback is not ported (checklist 19).
//
// PER SIDE. VRAvatar_NamedPose(pose, finger, ...) has no side: UZDXREMA's table always carries both sides (mkfbxposes.py
// mirrors a one-handed pose over). The port's table carries a side only from that side's own take, so its probe is per
// side; the stub answers the source's side-free probe for the side being posed (the only change the lines see), and
// the tables are built with the real one-sided pattern (kOwnerPoses) plus random holes.
//
//   ./test_holds

#include "vr_handpose_math.h"
#include "roth/roth_handhold.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace UZ
{
	// --- the engine names the cut lines use
	enum ENamedName { NAME_None };
	struct FName
	{
		std::string s;
		FName() = default;
		FName(ENamedName) {}
		explicit FName(const std::string &c) : s(c) {}
		const char *GetChars() const { return s.empty() ? "None" : s.c_str(); }	// FName(NAME_None).GetChars() is "None"
		bool operator!=(ENamedName) const { return !s.empty(); }
		bool operator==(ENamedName) const { return s.empty(); }
	};
	struct FString
	{
		std::string s;
		FString() = default;
		FString(const char *c) : s(c ? c : "") {}
		FString &operator=(const char *c) { s = c ? c : ""; return *this; }
		void Format(const char *fmt, ...)
		{
			char b[512];
			va_list ap;
			va_start(ap, fmt);
			vsnprintf(b, sizeof(b), fmt, ap);
			va_end(ap);
			s = b;
		}
		bool IsEmpty() const { return s.empty(); }
		const char *GetChars() const { return s.c_str(); }
	};
	struct FVector3 { float X = 0, Y = 0, Z = 0; };

	namespace s3d
	{
#include "gripsubject.inc"
	}

	enum { VRFINGER_THUMB = 0, VRFINGER_INDEX, VRFINGER_MIDDLE, VRFINGER_RING, VRFINGER_PINKY, VRFINGER_COUNT };
	enum { VRFINGER_JOINTS = 3 };
	enum { VRRIG_MAIN = 0, VRRIG_OFF = 1 };

#include "qmath.inc"

	// the rig: per role, the trigger and whether the controller is tracked (VAP :545)
	struct FHand { float trigger = 0.f; };
	struct FRig
	{
		bool valid[2] = { true, true };
		FHand h[2];
		bool Valid(int i) const { return valid[i]; }
		const FHand &Hand(int i) const { return h[i]; }
	};
	FRig rig;

	// what each role holds (vr_rig.cpp:1802-1853)
	struct FHeld { bool weapon = false; FName cls, prop; int subj = 0; };
	FHeld gHeld[2];
	bool VRRig_HandHoldsWeapon(int hand) { return gHeld[hand].weapon; }
	FName VRRig_HandGripPose(int hand) { return gHeld[hand].cls; }
	FName VRRig_HandPropClass(int hand) { return gHeld[hand].prop; }
	int VRRig_HandGripSubject(int hand) { return gHeld[hand].subj; }

	// the named poses, answered from the port's per-side tables
	const VRHandPose::HoldPoses *gTable[2] = { nullptr, nullptr };
	int gSide = 0;	// the side being posed: the side-free probe is answered for it (see "PER SIDE")
	bool VRAvatar_NamedPose(const char *pose, int finger, int joint, float &curlOut)
	{
		(void)joint;
		if (!VRHandPose::HoldHas(gTable[gSide], VRHandPose::FindOwnerPose(pose), finger)) return false;
		curlOut = 0.5f;
		return true;
	}
	bool VRAvatar_NamedPoseRot(const char *pose, int side, int finger, int joint, float qOut[4])
	{
		return VRHandPose::HoldRot(gTable[side], VRHandPose::FindOwnerPose(pose), finger, joint, qOut);
	}

	// VAP :476-656 for side sd, holding role hidx
	void Select(int sd, int hidx, FString srcPose[VRFINGER_COUNT][VRFINGER_JOINTS], FString &idxReady, FString &idxFire, float &idxT)
	{
		float want[VRFINGER_COUNT][VRFINGER_JOINTS] = {};
		(void)sd;
#include "hold_select.inc"
		(void)want;
	}

	// VAP :672-684 for joint (f, j)
	bool RotTarget(int sd, int f, int j, FString srcPose[VRFINGER_COUNT][VRFINGER_JOINTS], const FString &idxReady, const FString &idxFire,
		float idxT, float out[4])
	{
#include "hold_rot.inc"
		if (have) for (int k = 0; k < 4; ++k) out[k] = wq[k];
		return have;
	}
}

using namespace VRHandPose;

static void RandQ(std::mt19937 &rng, float q[4])
{
	std::normal_distribution<float> g(0.f, 1.f);
	float n = 0;
	for (int k = 0; k < 4; k++) { q[k] = g(rng); n += q[k] * q[k]; }
	n = sqrtf(n);
	for (int k = 0; k < 4; k++) q[k] /= n;
}

// the enum is the source's, value for value
static void TestEnum()
{
	const int src[] = { UZ::s3d::GRIPSUBJ_None, UZ::s3d::GRIPSUBJ_Round, UZ::s3d::GRIPSUBJ_Shell, UZ::s3d::GRIPSUBJ_Inserting, UZ::s3d::GRIPSUBJ_Magazine,
		UZ::s3d::GRIPSUBJ_Grip, UZ::s3d::GRIPSUBJ_Forend, UZ::s3d::GRIPSUBJ_Foregrip, UZ::s3d::GRIPSUBJ_Slide, UZ::s3d::GRIPSUBJ_Support,
		UZ::s3d::GRIPSUBJ_Holster, UZ::s3d::GRIPSUBJ_Pouch, UZ::s3d::GRIPSUBJ_MAX };
	const int port[] = { GRIPSUBJ_None, GRIPSUBJ_Round, GRIPSUBJ_Shell, GRIPSUBJ_Inserting, GRIPSUBJ_Magazine, GRIPSUBJ_Grip, GRIPSUBJ_Forend,
		GRIPSUBJ_Foregrip, GRIPSUBJ_Slide, GRIPSUBJ_Support, GRIPSUBJ_Holster, GRIPSUBJ_Pouch, GRIPSUBJ_MAX };
	bool ok = true;
	for (size_t i = 0; i < sizeof(src) / sizeof(src[0]); i++) ok = ok && src[i] == port[i];
	CHECK(ok, "EGripSubject differs from vk_openxrdevice.h:105-131");
	printf("EGripSubject: %d values, identical to the source's\n", (int)(sizeof(src) / sizeof(src[0])));
}

// the table's sides: a raw pose only on the hand(s) that authored it; composed poses only where all their frames are
static void TestSides()
{
	int right = 0, left = 0, both = 0;
	bool ok = true;
	for (int p = 0; p < kOwnerPoseCount; p++)
	{
		bool have[2] = { true, true };
		for (int s = 0; s < 2; s++)
			for (int f = 0; f < FINGER_COUNT; f++)
			{
				const int fr = OwnerFingerFrame(p, f, s);
				if (fr == -2) { ok = false; printf("  malformed: %s\n", kOwnerPoses[p].name); }
				if (fr < 0) have[s] = false;
			}
		if (kOwnerPoses[p].blender > 0)
		{
			const int h = kOwnerPoses[p].hands;
			ok = ok && have[0] == (h != HANDS_B) && have[1] == (h != HANDS_A);
		}
		if (have[0] && have[1]) both++; else if (have[0]) right++; else if (have[1]) left++;
	}
	printf("the pose table: %d poses (the owner's %d + %d measured); from each hand's own take: %d on both, %d the right only, %d the left only "
		"(the rest of each hand's are copied from the other take)\n", kOwnerPoseCount, kOwnerJsonPoseCount, kOwnerPoseCount - kOwnerJsonPoseCount, both, right, left);
	CHECK(ok, "a raw pose is on a side its hand did not author, or the table is malformed");
}

static void TestDifferential()
{
	std::mt19937 rng(20261113);
	std::uniform_real_distribution<float> u01(0.f, 1.f);
	const char *classes[] = { "", "pistol", "shotgun", "ssg", "rifle", "rocket", "plasma", "bfg", "saw", "nonesuch", "PISTOL" };
	const char *props[] = { "", "", "pistol", "rocket", "RealmsWeapon9", "nonesuch" };
	const int nc = sizeof(classes) / sizeof(classes[0]), np = sizeof(props) / sizeof(props[0]);

	long cases = 0, joints = 0, held = 0, blends = 0, weaponCases = 0, objectCases = 0;
	double worst = 0;
	long choiceDiffs = 0;
	for (int trial = 0; trial < 400; trial++)
	{
		// both sides' tables: the real one-sided pattern, then (most trials) random holes
		static HoldPoses T[2];
		for (int s = 0; s < 2; s++)
		{
			T[s] = HoldPoses();
			T[s].Built = true;
			for (int p = 0; p < kOwnerPoseCount; p++)
			{
				bool have = true;
				for (int f = 0; f < FINGER_COUNT; f++) if (OwnerFingerFrame(p, f, s) < 0) have = false;
				if (trial % 4 != 0 && u01(rng) < 0.2f) have = !have;
				T[s].Have[p] = have;
				for (int f = 0; f < FINGER_COUNT; f++) for (int j = 0; j < FINGER_JOINTS; j++) RandQ(rng, T[s].Q[p][f][j]);
			}
			UZ::gTable[s] = &T[s];
		}
		for (int k = 0; k < 60; k++)
		{
			// the two roles' held states
			HeldState st[2];
			for (int r = 0; r < 2; r++)
			{
				st[r].weapon = u01(rng) < 0.4f;
				st[r].gripClass = classes[(int)(u01(rng) * nc) % nc];
				st[r].propClass = props[(int)(u01(rng) * np) % np];
				st[r].subject = (int)(u01(rng) * GRIPSUBJ_MAX) % GRIPSUBJ_MAX;
				UZ::gHeld[r].weapon = st[r].weapon;
				UZ::gHeld[r].cls = UZ::FName(st[r].gripClass);
				UZ::gHeld[r].prop = UZ::FName(st[r].propClass);
				UZ::gHeld[r].subj = st[r].subject;
			}
			Inputs in[2];
			for (int r = 0; r < 2; r++)
			{
				in[r].trigger = (k % 5 == 0) ? 0.f : (k % 5 == 1 ? 1.f : u01(rng));
				in[r].grip = u01(rng);
				in[r].triggerTouch = u01(rng) < 0.5f;
				in[r].thumbTouch = u01(rng) < 0.5f;
				in[r].valid = u01(rng) < 0.85f;
				UZ::rig.valid[r] = in[r].valid;
				UZ::rig.h[r].trigger = in[r].trigger;
			}
			const bool rightHanded = (k & 1) == 0;
			for (int sd = 0; sd < 2; sd++)
			{
				const bool isMain = (sd == SIDE_RIGHT) == rightHanded;	// VAP :437
				const int hidx = isMain ? 0 : 1;
				UZ::gSide = sd;

				// the source
				UZ::FString srcPose[5][3], idxReady, idxFire;
				float idxT = 0.f;
				UZ::Select(sd, hidx, srcPose, idxReady, idxFire, idxT);

				// the port
				HoldChoice ch;
				SelectHold(&T[sd], st[hidx], st[1 - hidx], in[hidx], ch);

				// the choices
				bool same = true;
				for (int f = 0; f < 5; f++)
					for (int j = 0; j < 3; j++)
					{
						const std::string a = srcPose[f][j].s, b = ch.Src[f][j] >= 0 ? kOwnerPoses[ch.Src[f][j]].name : "";
						if (!EqualNoCase(a.c_str(), b.c_str())) same = false;
					}
				const std::string ra = idxReady.s, rb = ch.IdxReady >= 0 ? kOwnerPoses[ch.IdxReady].name : "";
				const std::string fa = idxFire.s, fb = ch.IdxFire >= 0 ? kOwnerPoses[ch.IdxFire].name : "";
				if (!EqualNoCase(ra.c_str(), rb.c_str()) || !EqualNoCase(fa.c_str(), fb.c_str()) || idxT != ch.IdxT) same = false;
				if (!same)
				{
					if (choiceDiffs < 5) printf("  choice differs: side %d role %d weapon %d cls '%s' prop '%s' subj %d; index source '%s'/'%s' port '%s'/'%s'\n",
						sd, hidx, (int)st[hidx].weapon, st[hidx].gripClass.c_str(), st[hidx].propClass.c_str(), st[hidx].subject,
						ra.c_str(), fa.c_str(), rb.c_str(), fb.c_str());
					choiceDiffs++;
				}

				// the targets: the controller's pose under both, the source's :672-684 / the port's HoldTargets over it
				HandPoses hp;
				for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) { RandQ(rng, hp.Open[f][j]); for (int p = 0; p < POSE_COUNT; p++) RandQ(rng, hp.Target[p][f][j]); }
				hp.Have = true;
				PoseInputs pi;
				PoseRule(in[hidx], pi);
				float ctl[5][3][4], palm[4];
				PoseTargets(&hp, pi, ctl, palm);
				float port[5][3][4];
				memcpy(port, ctl, sizeof(port));
				HoldTargets(&T[sd], ch, port, palm);
				for (int f = 0; f < 5; f++)
					for (int j = 0; j < 3; j++)
					{
						float wq[4];
						const bool have = UZ::RotTarget(sd, f, j, srcPose, idxReady, idxFire, idxT, wq);
						const float *src = have ? wq : ctl[f][j];
						for (int q = 0; q < 4; q++) worst = fmax(worst, fabs((double)src[q] - port[f][j][q]));
						joints++;
						if (have) held++;
					}
				if (!idxReady.IsEmpty()) blends++;
				if (st[hidx].weapon) weaponCases++; else if (st[hidx].subject != GRIPSUBJ_None) objectCases++;
				cases++;
			}
		}
	}
	printf("selection, %ld hands (%ld holding a weapon, %ld an object subject), %ld joints (%ld set by a named pose, %ld hands with the index "
		"blended ready->fire): choices differing from VAP :476-656: %ld; max |target - VAP :672-684| %.3g\n",
		cases, weaponCases, objectCases, joints, held, blends, choiceDiffs, worst);
	CHECK(choiceDiffs == 0, "the selection differs from UZDXREMA's own lines in %ld hands", choiceDiffs);
	CHECK(worst == 0.0, "the targets differ from UZDXREMA's own lines (%.3g)", worst);
	CHECK(held > 0 && blends > 0 && weaponCases > 0 && objectCases > 0, "the differential did not exercise every branch");
}

// NOT IN THE SOURCE, so checked by value: a pose the held thing names (HeldState::pose) is taken as the subject's name
// is; the palm rule (HoldTargets); no hold = the controller's pose exactly.
static void TestExtensions()
{
	std::mt19937 rng(3);
	HoldPoses T;
	T.Built = true;
	for (int p = 0; p < kOwnerPoseCount; p++)
	{
		bool have = true;
		for (int f = 0; f < FINGER_COUNT; f++) if (OwnerFingerFrame(p, f, SIDE_LEFT) < 0) have = false;
		T.Have[p] = have;
		for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) RandQ(rng, T.Q[p][f][j]);
		T.PalmDriven[p] = kOwnerPoses[p].palm != nullptr;
		RandQ(rng, T.PalmQ[p]);
	}
	RandQ(rng, T.PalmOpen);
	HeldState none, curl, cup, mag, gun;
	curl.pose = "fbx_curl";
	cup.subject = GRIPSUBJ_Round;
	mag.subject = GRIPSUBJ_Magazine;
	gun.weapon = true;	// no class: gun_grip, the index left to the controller
	Inputs in; in.valid = true; in.grip = 0.3f; in.trigger = 0.6f;
	float ctl[5][3][4], palm0[4];
	for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) RandQ(rng, ctl[f][j]);
	RandQ(rng, palm0);
	auto run = [&](const HeldState &h, float t[5][3][4], float pm[4]) {
		HoldChoice ch;
		SelectHold(&T, h, none, in, ch);
		memcpy(t, ctl, sizeof(ctl));
		memcpy(pm, palm0, sizeof(palm0));
		HoldTargets(&T, ch, t, pm);
		return ch;
	};
	float t[5][3][4], pm[4];
	HoldChoice c = run(none, t, pm);
	CHECK(!c.Any() && !memcmp(t, ctl, sizeof(t)) && !memcmp(pm, palm0, sizeof(pm)), "nothing held: the controller's pose, untouched");
	const int pc = FindOwnerPose("fbx_curl");
	c = run(curl, t, pm);
	bool ok = c.Any();
	for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) ok = ok && c.Src[f][j] == pc && !memcmp(t[f][j], T.Q[pc][f][j], 16);
	CHECK(ok && !memcmp(pm, T.PalmQ[pc], 16), "a named pose (fbx_curl): every joint from it, and the palm from its frame");
	HeldState pointing;
	pointing.pose = "pointing";
	T.Have[FindOwnerPose("pointing")] = true;
	c = run(pointing, t, pm);
	CHECK(c.Any() && !memcmp(pm, T.PalmOpen, 16), "pointing (its straight index from another frame than its palm would be): the palm in the drawn frame");
	const int pr = FindOwnerPose("hold_round");
	c = run(cup, t, pm);
	CHECK(c.Src[FINGER_MIDDLE][0] == pr && !memcmp(pm, T.PalmQ[pr], 16), "hold_round (the cup): every finger from it, and the palm from its frame");
	c = run(gun, t, pm);
	CHECK(c.Src[FINGER_INDEX][0] < 0 && c.IdxReady < 0 && !memcmp(t[FINGER_INDEX], ctl[FINGER_INDEX], sizeof(t[FINGER_INDEX])) && !memcmp(pm, T.PalmOpen, 16),
		"a weapon with no grip class: gun_grip, the index the controller's, the palm in the drawn frame");
	// the right hand lacks the wraps: hold_magazine there is the controller's pose
	HoldPoses R;
	R.Built = true;
	for (int p = 0; p < kOwnerPoseCount; p++)
	{
		bool have = true;
		for (int f = 0; f < FINGER_COUNT; f++) if (OwnerFingerFrame(p, f, SIDE_RIGHT) < 0) have = false;
		R.Have[p] = have;
	}
	HoldChoice cr;
	SelectHold(&R, mag, none, in, cr);
	CHECK(!cr.Any(), "a pose with a frame neither take has (built here without the other hand): no pose, the controller's");
	printf("extensions checked by value: named pose, palm rule, nothing held, a one-sided pose on the other hand\n");
}

// The Realms item map (src/roth/roth_handhold.h): every row resolves to a pose of the table (no name the table lacks),
// and what each hand then makes, given each hand's own take (the real one-sided pattern).
static void TestItemMap()
{
	// each hand: its own frames, the rest copied from the other take (BuildHoldPoses)
	HoldPoses T[2];
	for (int s = 0; s < 2; s++)
	{
		T[s].Built = true;
		for (int p = 0; p < kOwnerPoseCount; p++)
		{
			bool have = true, copied = false;
			for (int f = 0; f < FINGER_COUNT; f++)
			{
				const int raw = OwnerFingerRaw(p, f);
				if (raw < 0 || (!OwnerRawAuthored(raw, s) && !OwnerRawAuthored(raw, 1 - s))) have = false;
				else if (!OwnerRawAuthored(raw, s)) copied = true;
			}
			T[s].Have[p] = have;
			T[s].Copied[p] = copied;
		}
	}
	int bad = 0, rows = 0;
	printf("the Realms item map (roth_handhold.h), what each hand makes (trigger off; \"copied\": frames from the other hand's take):\n");
	for (const roth::ItemGrip &g : roth::kItemGrips)
	{
		rows++;
		const HeldState h = roth::HeldFor(g.item, g.weapon, g.weapon ? "RealmsWeaponN" : nullptr);
		std::string name;
		if (g.weapon) name = g.gripClass ? std::string("grip_") + g.gripClass : "gun_grip";
		else if (g.pose) name = g.pose;
		else
		{
			static const char *const hold[GRIPSUBJ_MAX] = { nullptr, "hold_round", "hold_shell", "hold_inserting", "hold_magazine", nullptr, "hold_forend", "hold_foregrip", "hold_slide", "hold_support", nullptr, nullptr };
			name = hold[g.subject] ? hold[g.subject] : "";
		}
		if (FindOwnerPose(name.c_str()) < 0) { bad++; printf("  FAIL item %d: '%s' is not in the table\n", g.item, name.c_str()); }
		std::string made[2];
		for (int s = 0; s < 2; s++)
		{
			HoldChoice ch;
			Inputs in; in.valid = true;
			SelectHold(&T[s], h, HeldState(), in, ch);
			const int m = ch.Src[FINGER_MIDDLE][0];
			made[s] = m >= 0 ? std::string(kOwnerPoses[m].name) + (T[s].Copied[m] ? " (copied)" : "") : "ABSENT (controller)";
		}
		printf("  %3d %-26s %-6s -> %-15s right: %-24s left: %-24s%s\n", g.item, g.name, g.weapon ? "weapon" : "item", name.c_str(),
			made[0].c_str(), made[1].c_str(), g.needsFrame ? "  [needs a new authored frame]" : "");
	}
	CHECK(bad == 0, "%d of %d item rows name a pose the table lacks", bad, rows);
	// (17, the Book, was this test's item until it got its row: wrap_half across the spine, owner Q8, 2026-11-13.)
	const HeldState none = roth::HeldFor(36, false);
	CHECK(none.subject == GRIPSUBJ_None && none.pose.empty() && !none.weapon, "an item with no design (36, the Note) holds nothing: the controller's pose");
}

int main()
{
	TestEnum();
	TestSides();
	TestDifferential();
	TestExtensions();
	TestItemMap();
	printf("test_holds: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
