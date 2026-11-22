// Fixtures for VR full-body tracking's pure math (#1565; indra/newview/llvrbodymath.h), run by
// scripts/check/checks/vr_fbt.py (compiled with the host C++ compiler, no viewer build).
//   QUAT     Hamilton product / rotation / basis alignment agree with each other.
//   OFFSET   a calibrated tracker offset reproduces its bone after the tracker moves rigidly.
//   LEG      two-bone IK: a reachable target is hit with the thigh / shin lengths kept, the knee
//            bends toward the pole and is a pure hinge (local Y); out of reach stretches toward
//            it; a target at the hip never gives NaN.
//   SPINE    no bend for a neck straight above; leaning bends toward it; twist is limited.
//   ASSIGN   trackers assigned by height / side (feet, knees, hips, chest).
//   FADE     a lost limb fades out in 0.3 s; smoothing snaps on a jump.
#include "llvrbodymath.h"

#include <cstdio>
#include <initializer_list>

using namespace slfbt;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static bool nearV(const V3& a, const V3& b, float eps) { return length(a - b) <= eps; }

static void testQuat()
{
    const Q a = qaxis(V3(0.f, 0.f, 1.f), 1.5707963f);   // 90 deg about Z: X -> Y
    CHECK(nearV(qrot(a, V3(1.f, 0.f, 0.f)), V3(0.f, 1.f, 0.f), 1e-5f), "qaxis/qrot");
    const Q b = qaxis(V3(0.f, 1.f, 0.f), 1.5707963f);   // 90 deg about Y: X -> -Z
    // qmul(a, b): b first. X -b-> -Z -a-> -Z.
    CHECK(nearV(qrot(qmul(a, b), V3(1.f, 0.f, 0.f)), V3(0.f, 0.f, -1.f), 1e-5f), "qmul order");
    CHECK(nearV(qrot(qmul(b, a), V3(1.f, 0.f, 0.f)), V3(0.f, 1.f, 0.f), 1e-5f), "qmul order 2");
    const Q r = qalignPair(V3(0.f, 0.f, -1.f), V3(1.f, 0.f, 0.f), V3(1.f, 0.f, 0.f), V3(0.f, 0.f, 1.f));
    CHECK(nearV(qrot(r, V3(0.f, 0.f, -1.f)), V3(1.f, 0.f, 0.f), 1e-4f), "alignPair primary");
    CHECK(nearV(qrot(r, V3(1.f, 0.f, 0.f)), V3(0.f, 0.f, 1.f), 1e-4f), "alignPair secondary");
    const Q ft = qfromto(V3(1.f, 0.f, 0.f), V3(-1.f, 0.f, 0.f));
    CHECK(nearV(qrot(ft, V3(1.f, 0.f, 0.f)), V3(-1.f, 0.f, 0.f), 1e-4f), "fromto opposite");
    CHECK(std::fabs(yawOf(qyaw(0.7f)) - 0.7f) < 1e-5f, "yawOf");
    // Looking straight down / up (pitch about the head's left axis, +-89 deg) with a 3 deg wobble
    // about the head's own up axis: the heading stays the head's (the forward's horizontal
    // remainder alone would point anywhere). Also across the +-pi seam.
    for (float yaw : {0.7f, -2.9f, 3.1f})
    {
        for (float pitch : {1.553f, -1.553f})
        {
            const Q q = qmul(qyaw(yaw), qmul(qaxis(V3(0.f, 1.f, 0.f), pitch), qaxis(V3(0.f, 0.f, 1.f), 0.05f)));
            float d = yawOf(q) - yaw;
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            CHECK(std::fabs(d) < 1e-2f, "yawOf looking down / up (yaw %.2f pitch %.2f: %.3f)", yaw, pitch, yawOf(q));
        }
    }
}

static void testOffset()
{
    const V3 tp(0.3f, -0.1f, 1.0f), bp(0.25f, 0.f, 0.95f);
    const Q tr = qnormalized(Q(0.2f, -0.4f, 0.1f, 0.9f)), br = qyaw(0.4f);
    const Offset o = calibrateOffset(tp, tr, bp, br);
    V3 p;
    Q q;
    applyOffset(tp, tr, o, p, q);
    CHECK(nearV(p, bp, 1e-5f) && qangle(q, br) < 1e-3f, "offset round trip");
    // Move the tracker rigidly (turn 30 deg about Z around the origin, shift): the bone moves with it.
    const Q m = qyaw(0.5235988f);
    const V3 shift(1.f, 2.f, 0.f);
    applyOffset(qrot(m, tp) + shift, qmul(m, tr), o, p, q);
    CHECK(nearV(p, qrot(m, bp) + shift, 1e-4f) && qangle(q, qmul(m, br)) < 1e-3f, "offset rigid motion");
}

static LegRest restLeg()
{
    LegRest r;
    r.hip = V3(0.f, 0.1f, -0.05f);     // left hip, in the pelvis frame
    r.knee = V3(0.f, 0.f, -0.45f);     // thigh
    r.ankle = V3(0.f, 0.f, -0.42f);    // shin
    return r;
}

static void testLeg()
{
    const LegRest r = restLeg();
    const V3 pelvis(0.f, 0.f, 1.f);
    const Q prot;
    // Reachable, knee bent: ankle 0.2 m forward and 0.75 m below the hip.
    const V3 hip = pelvis + r.hip;
    const V3 target = hip + V3(0.2f, 0.f, -0.75f);
    const V3 pole = hip + V3(1.f, 0.f, -0.4f);
    LegPose lp = solveLeg(pelvis, prot, r, target, pole);
    CHECK(lp.ok && lp.reached, "leg solved");
    CHECK(nearV(lp.anklePos, target, 1e-3f), "ankle on target");
    CHECK(std::fabs(length(lp.kneePos - lp.hipPos) - 0.45f) < 1e-3f, "thigh length");
    CHECK(std::fabs(length(lp.anklePos - lp.kneePos) - 0.42f) < 1e-3f, "shin length");
    // Forward kinematics from the rotations reproduces the positions.
    const V3 kneeFk = lp.hipPos + qrot(lp.hipWorld, r.knee);
    const V3 ankleFk = kneeFk + qrot(lp.kneeWorld, r.ankle);
    CHECK(nearV(kneeFk, lp.kneePos, 1e-3f) && nearV(ankleFk, target, 1e-3f), "FK matches");
    // The knee points forward (toward the pole), off the hip -> ankle line.
    const V3 mid = (lp.hipPos + lp.anklePos) * 0.5f;
    CHECK(lp.kneePos.x > mid.x + 0.05f, "knee bends toward the pole (%f vs %f)", lp.kneePos.x, mid.x);
    // Pure hinge: the knee's local rotation is about Y only.
    const Q kneeLocal = qmul(qconj(lp.hipWorld), lp.kneeWorld);
    CHECK(std::fabs(kneeLocal.x) < 1e-3f && std::fabs(kneeLocal.z) < 1e-3f, "knee is a Y hinge (%f %f %f %f)",
          kneeLocal.x, kneeLocal.y, kneeLocal.z, kneeLocal.w);
    // The foot's front stays forward: the hip's +X has no sideways part.
    CHECK(std::fabs(qrot(lp.hipWorld, V3(1.f, 0.f, 0.f)).y) < 1e-3f, "no hip twist");

    // Pole behind: the knee bends backward.
    lp = solveLeg(pelvis, prot, r, target, hip + V3(-1.f, 0.f, -0.4f));
    CHECK(lp.ok && lp.kneePos.x < mid.x, "knee follows a pole behind");

    // Out of reach: straight leg pointing at it, no NaN.
    const V3 far2 = hip + V3(0.f, 0.f, -2.f);
    lp = solveLeg(pelvis, prot, r, far2, pole);
    CHECK(lp.ok && !lp.reached, "stretch: solved, not reached");
    CHECK(std::fabs(length(lp.anklePos - lp.hipPos) - 0.87f) < 2e-3f, "stretch: full length");
    CHECK(lp.anklePos.z < hip.z - 0.8f, "stretch: points at the target");

    // Deep fold (ankle just past |l1 - l2| below the hip): still a forward hinge, no 180 twist.
    lp = solveLeg(pelvis, prot, r, hip + V3(0.f, 0.f, -(0.03f + 0.02f)), pole);
    {
        const Q kl = qmul(qconj(lp.hipWorld), lp.kneeWorld);
        CHECK(lp.ok && std::fabs(kl.x) < 1e-3f && std::fabs(kl.z) < 1e-3f, "deep fold: knee still a Y hinge (%f %f %f %f)",
              kl.x, kl.y, kl.z, kl.w);
        CHECK(qrot(lp.kneeWorld, V3(1.f, 0.f, 0.f)).y > -1e-3f && std::fabs(qrot(lp.kneeWorld, V3(1.f, 0.f, 0.f)).y) < 1e-3f,
              "deep fold: shin not twisted sideways");
    }
    // Target at the hip: folded, finite.
    lp = solveLeg(pelvis, prot, r, hip, pole);
    CHECK(lp.ok && finite(lp.kneePos) && finite(lp.anklePos), "target at the hip stays finite");

    // Turned pelvis (90 deg yaw): same leg, turned.
    const Q yaw90 = qyaw(1.5707963f);
    const V3 hip2 = pelvis + qrot(yaw90, r.hip);
    const V3 t2 = hip2 + qrot(yaw90, V3(0.2f, 0.f, -0.75f));
    lp = solveLeg(pelvis, yaw90, r, t2, hip2 + qrot(yaw90, V3(1.f, 0.f, -0.4f)));
    CHECK(lp.ok && nearV(lp.anklePos, t2, 1e-3f), "turned pelvis");
}

static void testSpine()
{
    SpineRest r;
    r.torso = V3(0.f, 0.f, 0.1f);
    r.neck = V3(0.f, 0.f, 0.5f);
    const V3 pelvis(0.f, 0.f, 1.f);
    SpinePose sp = solveSpine(pelvis, Q(), r, pelvis + V3(0.f, 0.f, 0.6f), 0.f, 0.5f);
    CHECK(qangle(sp.chestWorld, Q()) < 1e-3f && qangle(sp.torsoWorld, Q()) < 1e-3f, "upright: no bend");
    // Neck target forward: the chest leans forward (its up tilts toward +X).
    sp = solveSpine(pelvis, Q(), r, pelvis + V3(0.3f, 0.f, 0.5f), 0.f, 0.5f);
    const V3 up = qrot(sp.chestWorld, V3(0.f, 0.f, 1.f));
    CHECK(up.x > 0.3f, "lean forward (%f)", up.x);
    CHECK(qangle(sp.torsoWorld, Q()) < qangle(sp.chestWorld, Q()), "torso bends less than the chest");
    // Head turned 90 deg, twist 0.5: chest about 45 deg; 180 deg is capped.
    sp = solveSpine(pelvis, Q(), r, pelvis + V3(0.f, 0.f, 0.6f), 1.5707963f, 0.5f);
    CHECK(std::fabs(yawOf(sp.chestWorld) - 0.785398f) < 1e-2f, "twist half (%f)", yawOf(sp.chestWorld));
    sp = solveSpine(pelvis, Q(), r, pelvis + V3(0.f, 0.f, 0.6f), 3.1f, 1.f);
    CHECK(std::fabs(yawOf(sp.chestWorld)) <= 1.2f + 1e-3f, "twist capped");
    // Estimated hips: under the neck target.
    const V3 est = estimatePelvis(V3(1.f, 2.f, 1.6f), 0.f, V3(0.f, 0.f, 0.6f));
    CHECK(nearV(est, V3(1.f, 2.f, 1.0f), 1e-5f), "estimated pelvis");
}

static void testAssign()
{
    Proportions pr;
    const float eyeH = 1.6f;
    const V3 head(0.f, 0.f, eyeH);
    const V3 fwd(1.f, 0.f, 0.f);   // facing +X: left is +Y
    // right foot, left foot, waist, left knee, chest, right knee -- shuffled.
    const V3 pos[6] = {V3(0.f, -0.12f, 0.08f), V3(0.f, 0.12f, 0.09f), V3(-0.12f, 0.f, 0.92f),
                       V3(0.05f, 0.11f, 0.47f), V3(0.1f, 0.f, 1.22f), V3(0.05f, -0.11f, 0.46f)};
    int out[B_COUNT];
    assignByPosition(pos, 6, head, fwd, eyeH, pr, out);
    CHECK(out[B_RFOOT] == 0 && out[B_LFOOT] == 1, "feet by side (%d %d)", out[B_LFOOT], out[B_RFOOT]);
    CHECK(out[B_WAIST] == 2, "waist (%d)", out[B_WAIST]);
    CHECK(out[B_LKNEE] == 3 && out[B_RKNEE] == 5, "knees (%d %d)", out[B_LKNEE], out[B_RKNEE]);
    CHECK(out[B_CHEST] == 4, "chest (%d)", out[B_CHEST]);
    // Three-point (feet + hips) facing -Y: left is +X.
    const V3 p3[3] = {V3(-0.12f, 0.f, 0.07f), V3(0.f, 0.05f, 0.95f), V3(0.12f, 0.f, 0.07f)};
    assignByPosition(p3, 3, head, V3(0.f, -1.f, 0.f), eyeH, pr, out);
    CHECK(out[B_LFOOT] == 2 && out[B_RFOOT] == 0 && out[B_WAIST] == 1 && out[B_CHEST] == -1 && out[B_LKNEE] == -1,
          "three-point (%d %d %d)", out[B_LFOOT], out[B_RFOOT], out[B_WAIST]);
    // A tracker held in the hand at head height is not a body bone.
    const V3 p1[1] = {V3(0.3f, 0.f, 1.55f)};
    assignByPosition(p1, 1, head, fwd, eyeH, pr, out);
    bool none = true;
    for (int b = 0; b < B_COUNT; ++b) none = none && out[b] < 0;
    CHECK(none, "hand-held tracker unassigned");
    // T-pose bones: the feet keep their tracker's xy at ankle height; the hips sit under the head.
    V3 bp;
    Q bq;
    tposeBone(B_LFOOT, pos[1], head, 0.f, eyeH, pr, bp, bq);
    CHECK(nearV(bp, V3(0.f, 0.12f, pr.ankle * eyeH), 1e-5f), "t-pose foot");
    tposeBone(B_WAIST, pos[2], head, 0.f, eyeH, pr, bp, bq);
    CHECK(nearV(bp, V3(-pr.eyeForward * eyeH, 0.f, pr.pelvis * eyeH), 1e-5f), "t-pose hips");
}

static void testMapping()
{
    // A standing foot maps to the same place whatever the head does (the mapping never reads
    // the headset), and the scale applies from the reference.
    const V3 anchor(128.f, 64.f, 22.f), foot(0.3f, -0.1f, 0.08f);
    const V3 a = roomToAgent(foot, anchor, 0.1f, 0.f, 0.f, 1.25f, 20.f);
    CHECK(nearV(a, V3(128.f + 0.25f, 64.f - 0.125f, 20.f + 0.1f), 1e-4f), "room -> agent (%f %f %f)", a.x, a.y, a.z);
    const V3 b = roomToAgent(foot, anchor, 0.1f, 0.f, 1.5707963f, 1.f, 20.f);   // rig turned 90 deg
    CHECK(nearV(b, V3(128.f + 0.1f, 64.f + 0.2f, 20.08f), 1e-4f), "room -> agent turned (%f %f)", b.x, b.y);
}

static void testTime()
{
    float w = 1.f;
    int frames = 0;
    while (w > 0.f && frames < 1000)
    {
        w = fadeStep(w, false, 1.f / 90.f);
        ++frames;
    }
    CHECK(frames >= 26 && frames <= 28, "fade out over 0.3 s at 90 Hz (%d frames)", frames);
    w = fadeStep(0.f, true, 1.f);
    CHECK(w == 1.f, "fade clamps");
    V3 p(0.f, 0.f, 0.f);
    Q q;
    smoothPose(p, q, V3(0.1f, 0.f, 0.f), Q(), 0.011f, 0.05f);
    CHECK(p.x > 0.01f && p.x < 0.05f, "smoothing moves part way (%f)", p.x);
    smoothPose(p, q, V3(5.f, 0.f, 0.f), Q(), 0.011f, 0.05f);
    CHECK(p.x == 5.f, "smoothing snaps on a jump");
}

int main()
{
    testQuat();
    testOffset();
    testLeg();
    testSpine();
    testAssign();
    testMapping();
    testTime();
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
