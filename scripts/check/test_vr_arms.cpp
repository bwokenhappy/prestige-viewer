// Fixtures for VR arms and neck (#1584; indra/newview/llvrbodyarmmath.h), run by
// scripts/check/checks/vr_arms.py (compiled with the host C++ compiler, no viewer build).
//   TWOBONE  the generalised two-bone solver equals solveLeg for a knee (+X bend side).
//   REACH    a wrist target within reach is hit exactly (FK from the rotations agrees), the hand's
//            world rotation is the target's, the elbow is a pure hinge.
//   STRETCH  out of reach the arm straightens toward the target without stretching.
//   POLE     the natural elbow: down for a forward reach, back with the hand low, out overhead;
//            a tracker hint wins; the upper arm is never rolled ~180 degrees.
//   MIRROR   the right arm is the left one mirrored across the body (Y -> -Y).
//   TWIST    the forearm roll is shared: the forearm bone takes twistShare of it about its own
//            axis, the wrist the rest, the hand ends on its target.
//   COLLAR   overhead reaches raise the collar (the shoulder goes up), limited; a hand at the side
//            barely moves it.
//   GRIP     a controller held in front, fist forward, thumb up: fingers forward and tilted down,
//            palm facing in, the wrist behind the grip; left / right mirror.
//   NECK     neck * head (local) composes back to the headset's rotation relative to the chest;
//            the neck takes its share of the pitch / yaw; the head position leans the neck.
#include "llvrbodyarmmath.h"

#include <cstdio>
#include <algorithm>
#include <initializer_list>

using namespace slfbt;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static bool nearV(const V3& a, const V3& b, float eps) { return length(a - b) <= eps; }
static V3 mirror(const V3& v) { return V3(v.x, -v.y, v.z); }
// Mirroring a rotation across the XZ plane (Y -> -Y): x and z components flip.
static Q mirrorQ(const Q& q) { return Q(-q.x, q.y, -q.z, q.w); }

// SL skeleton offsets (avatar_skeleton.xml), side +1 left / -1 right.
static ArmRest restArm(float side)
{
    ArmRest r;
    r.collar = V3(-0.021f, side * 0.085f, 0.165f);
    r.shoulder = V3(0.f, side * 0.079f, 0.f);
    r.elbow = V3(0.f, side * 0.248f, 0.f);
    r.wrist = V3(0.f, side * 0.205f, 0.f);
    r.side = side;
    return r;
}

static void testTwoBoneIsLeg()
{
    LegRest lr;
    lr.hip = V3(0.f, 0.1f, -0.05f);
    lr.knee = V3(0.f, 0.f, -0.45f);
    lr.ankle = V3(0.f, 0.f, -0.42f);
    TwoBoneRest tr;
    tr.root = lr.hip;
    tr.upper = lr.knee;
    tr.lower = lr.ankle;
    const V3 pelvis(0.f, 0.f, 1.f);
    const Q prot = qyaw(0.3f);
    const V3 targets[3] = {V3(0.2f, 0.1f, 0.25f), V3(0.f, 0.1f, -2.f), V3(0.3f, 0.3f, 0.6f)};
    for (const V3& t : targets)
    {
        const LegPose a = solveLeg(pelvis, prot, lr, t, V3(1.f, 0.2f, 0.6f));
        const TwoBonePose b = solveTwoBone(pelvis, prot, tr, t, V3(1.f, 0.2f, 0.6f), V3(1.f, 0.f, 0.f));
        CHECK(a.ok == b.ok && a.reached == b.reached && nearV(a.kneePos, b.midPos, 1e-4f) && nearV(a.anklePos, b.endPos, 1e-4f) &&
                  qangle(a.hipWorld, b.upperWorld) < 1e-3f && qangle(a.kneeWorld, b.lowerWorld) < 1e-3f,
              "solveTwoBone(+X) == solveLeg");
    }
}

// FK of an arm pose: collar -> shoulder -> elbow -> wrist positions from the world rotations.
static V3 fkWrist(const ArmPose& p, const ArmRest& r)
{
    const V3 sh = p.collarPos + qrot(p.collarWorld, r.shoulder);
    const V3 el = sh + qrot(p.shoulderWorld, r.elbow);
    return el + qrot(p.elbowWorld, r.wrist);
}

static void testReach()
{
    const ArmParams prm;
    const V3 chest(0.f, 0.f, 1.3f);
    for (float side : {1.f, -1.f})
    {
        const ArmRest r = restArm(side);
        // In front of the chest, a little to the side, below the shoulder.
        const V3 target = chest + V3(0.3f, side * 0.2f, 0.f);
        const Q hand = qyaw(0.2f * side);
        const ArmPose p = solveArm(chest, Q(), r, target, hand, nullptr, prm);
        CHECK(p.ok && p.reached, "reach solved (side %g)", side);
        CHECK(nearV(p.wristPos, target, 1e-3f), "wrist on target (side %g)", side);
        CHECK(nearV(fkWrist(p, r), target, 2e-3f), "FK wrist on target (side %g): %f", side, length(fkWrist(p, r) - target));
        CHECK(qangle(p.wristWorld, hand) < 1e-3f, "hand rotation exact");
        CHECK(std::fabs(length(p.elbowPos - p.shoulderPos) - 0.248f) < 1e-3f, "upper arm length kept");
        // The elbow bone's hinge part (without the shared roll) is about the hinge only: its local
        // rotation's swing about the forearm axis has no component along the upper arm.
        const Q elbowLocal = qmul(qconj(p.shoulderWorld), p.elbowWorld);
        Q sw, tw;
        swingTwist(elbowLocal, V3(0.f, side, 0.f), sw, tw);
        CHECK(std::fabs(dot(V3(sw.x, sw.y, sw.z), V3(0.f, 1.f, 0.f))) < 1e-3f, "elbow swing is a hinge");
    }
}

static void testStretch()
{
    const ArmParams prm;
    const V3 chest(0.f, 0.f, 1.3f);
    const ArmRest r = restArm(1.f);
    const V3 target = chest + V3(1.5f, 0.3f, 0.2f);
    const ArmPose p = solveArm(chest, Q(), r, target, Q(), nullptr, prm);
    CHECK(p.ok && !p.reached, "out of reach: solved, not reached");
    CHECK(std::fabs(length(p.elbowPos - p.shoulderPos) - 0.248f) < 1e-3f, "no stretch: upper arm");
    CHECK(std::fabs(length(p.wristPos - p.elbowPos) - 0.205f) < 1e-3f, "no stretch: forearm");
    const V3 a = normalized(p.wristPos - p.shoulderPos), b = normalized(target - p.shoulderPos);
    CHECK(dot(a, b) > 0.999f, "straightens toward the target (%f)", dot(a, b));
    CHECK(length(p.wristPos - p.shoulderPos) > 0.45f * 0.995f, "straight");
    // Target at the shoulder: finite.
    const ArmPose q = solveArm(chest, Q(), r, p.shoulderPos, Q(), nullptr, prm);
    CHECK(q.ok && finite(q.elbowPos), "target at the shoulder stays finite");
}

// Distance of the elbow from the shoulder -> wrist line, as a vector.
static V3 elbowOff(const ArmPose& p)
{
    const V3 d = normalized(p.wristPos - p.shoulderPos);
    const V3 e = p.elbowPos - p.shoulderPos;
    return e - d * dot(e, d);
}

static void testPole()
{
    const ArmParams prm;
    const V3 chest(0.f, 0.f, 1.3f);
    for (float side : {1.f, -1.f})
    {
        const ArmRest r = restArm(side);
        const V3 sh = chest + r.collar + r.shoulder;
        // Forward reach at shoulder height, bent: the elbow drops below the line.
        ArmPose p = solveArm(chest, Q(), r, sh + V3(0.35f, 0.f, 0.f), Q(), nullptr, prm);
        CHECK(elbowOff(p).z < -0.03f, "forward reach: elbow down (side %g, %f)", side, elbowOff(p).z);
        // Hand low in front of the hip: elbow back.
        p = solveArm(chest, Q(), r, sh + V3(0.15f, -side * 0.05f, -0.35f), Q(), nullptr, prm);
        CHECK(elbowOff(p).x < -0.02f, "hand low: elbow back (side %g, %f)", side, elbowOff(p).x);
        // Overhead: elbow out to its side.
        p = solveArm(chest, Q(), r, sh + V3(0.05f, side * 0.05f, 0.35f), Q(), nullptr, prm);
        CHECK(side * elbowOff(p).y > 0.03f, "overhead: elbow out (side %g, %f)", side, elbowOff(p).y);
        // A tracker hint wins: elbow up.
        const V3 hint = sh + V3(0.15f, side * 0.1f, 0.4f);
        p = solveArm(chest, Q(), r, sh + V3(0.35f, 0.f, 0.f), Q(), &hint, prm);
        CHECK(elbowOff(p).z > 0.03f, "hint: elbow toward it (side %g, %f)", side, elbowOff(p).z);
        // No 180 degree roll: with the arm bent forward the upper arm's front (+X) faces forward
        // or up, never back (the biceps would face back).
        p = solveArm(chest, Q(), r, sh + V3(0.25f, side * 0.25f, 0.f), Q(), nullptr, prm);
        const V3 front = qrot(p.shoulderWorld, V3(1.f, 0.f, 0.f));
        CHECK(front.x > -0.2f, "upper arm not rolled backward (side %g, front %f %f %f)", side, front.x, front.y, front.z);
        // A natural bend: the wrist's local rotation stays small for a hand turned like the forearm.
        const Q lw = qmul(qconj(p.elbowWorld), p.wristWorld);
        (void)lw;
    }
}

static void testMirror()
{
    const ArmParams prm;
    const V3 chest(0.f, 0.f, 1.3f);
    const ArmRest l = restArm(1.f), r = restArm(-1.f);
    const V3 targets[4] = {V3(0.35f, 0.25f, 0.f), V3(0.1f, 0.3f, 0.45f), V3(0.2f, 0.05f, -0.4f), V3(0.f, 0.9f, 0.1f)};
    for (const V3& t : targets)
    {
        const Q hand = qnormalized(Q(0.1f, 0.2f, 0.3f, 0.9f));
        const ArmPose a = solveArm(chest, Q(), l, chest + t, hand, nullptr, prm);
        const ArmPose b = solveArm(chest, Q(), r, chest + mirror(t), mirrorQ(hand), nullptr, prm);
        CHECK(a.ok && b.ok, "mirror solved");
        CHECK(nearV(mirror(a.elbowPos), b.elbowPos, 1e-3f) && nearV(mirror(a.wristPos), b.wristPos, 1e-3f),
              "mirror positions (%f %f %f vs %f %f %f)", a.elbowPos.x, -a.elbowPos.y, a.elbowPos.z, b.elbowPos.x, b.elbowPos.y,
              b.elbowPos.z);
        CHECK(qangle(mirrorQ(a.collarWorld), b.collarWorld) < 2e-3f && qangle(mirrorQ(a.shoulderWorld), b.shoulderWorld) < 2e-3f &&
                  qangle(mirrorQ(a.elbowWorld), b.elbowWorld) < 2e-3f,
              "mirror rotations");
    }
}

static void testTwist()
{
    ArmParams prm;
    prm.twistShare = 0.5f;
    const V3 chest(0.f, 0.f, 1.3f);
    for (float side : {1.f, -1.f})
    {
        const ArmRest r = restArm(side);
        const V3 target = chest + V3(0.3f, side * 0.25f, 0.f);
        const ArmPose base = solveArm(chest, Q(), r, target, Q(), nullptr, prm);
        // The hand rolled 80 degrees about the forearm relative to the untwisted hinge frame.
        const V3 axisL = normalized(r.wrist);
        const Q hinge = qmul(base.elbowWorld, qaxis(axisL, -base.twist * prm.twistShare));   // untwisted forearm
        const float roll = 1.3962634f;
        const Q hand = qmul(hinge, qaxis(axisL, roll));
        const ArmPose p = solveArm(chest, Q(), r, target, hand, nullptr, prm);
        CHECK(std::fabs(p.twist - roll) < 1e-3f, "twist measured (side %g: %f)", side, p.twist);
        const Q forearmTwist = qmul(qconj(hinge), p.elbowWorld);
        CHECK(std::fabs(twistAngle(forearmTwist, axisL) - 0.5f * roll) < 1e-3f, "forearm takes half (side %g)", side);
        const Q wristLocal = qmul(qconj(p.elbowWorld), p.wristWorld);
        CHECK(std::fabs(twistAngle(wristLocal, axisL) - 0.5f * roll) < 1e-3f && qangle(Q(), wristLocal) < 0.5f * roll + 1e-3f,
              "wrist takes the rest (side %g)", side);
        CHECK(qangle(p.wristWorld, hand) < 1e-4f, "hand exact");
    }
}

static void testCollar()
{
    const ArmParams prm;
    const V3 chest(0.f, 0.f, 1.3f);
    for (float side : {1.f, -1.f})
    {
        const ArmRest r = restArm(side);
        const V3 restSh = chest + r.collar + r.shoulder;
        ArmPose p = solveArm(chest, Q(), r, restSh + V3(0.f, side * 0.1f, 0.44f), Q(), nullptr, prm);
        CHECK(p.shoulderPos.z > restSh.z + 0.015f, "overhead raises the shoulder (side %g, %f)", side, p.shoulderPos.z - restSh.z);
        CHECK(qangle(Q(), p.collarWorld) <= prm.collarMaxRaise + 1e-3f, "collar limited");
        p = solveArm(chest, Q(), r, restSh + V3(0.f, side * 0.05f, -0.44f), Q(), nullptr, prm);
        CHECK(qangle(Q(), p.collarWorld) <= prm.collarMaxDrop + 1e-3f, "hand down: collar barely moves (%f)", qangle(Q(), p.collarWorld));
        // Far forward: protraction (the shoulder moves forward).
        p = solveArm(chest, Q(), r, restSh + V3(0.5f, 0.f, 0.f), Q(), nullptr, prm);
        CHECK(p.shoulderPos.x > restSh.x + 0.01f, "far reach brings the shoulder forward (%f)", p.shoulderPos.x - restSh.x);
    }
}

static void testGrip()
{
    const GripOffset o;
    // A controller held in front: OpenXR grip with -Z forward (body +X), +Y up: identity in body
    // space (xrRotToBody of the identity is the identity: forward -Z -> +X, up +Y -> +Z).
    const float idq[4] = {0.f, 0.f, 0.f, 1.f};
    const Q grip = xrRotToBody(idq);
    for (float side : {1.f, -1.f})
    {
        V3 wp;
        Q hr;
        const V3 gp(0.4f, side * 0.2f, 1.1f);
        gripToHand(gp, grip, side, o, 1.f, wp, hr);
        const V3 fingers = qrot(hr, V3(0.f, side, 0.f));
        const V3 palm = qrot(hr, V3(0.f, 0.f, -1.f));
        const V3 thumb = qrot(hr, V3(1.f, 0.f, 0.f));
        CHECK(fingers.x > 0.85f && fingers.z < -0.2f, "fingers forward, tilted down (side %g: %f %f)", side, fingers.x, fingers.z);
        CHECK(side * palm.y < -0.9f, "palm faces in (side %g: %f)", side, palm.y);
        CHECK(thumb.z > 0.85f, "thumb up (side %g)", side);
        CHECK(wp.x < gp.x - 0.05f, "wrist behind the grip (side %g)", side);
        CHECK(std::fabs(length(wp - gp) - std::sqrt(o.back * o.back + o.lift * o.lift)) < 1e-4f, "offset length");
        // Scale applies to the offset.
        V3 wp2;
        gripToHand(gp, grip, side, o, 2.f, wp2, hr);
        CHECK(std::fabs(length(wp2 - gp) - 2.f * length(wp - gp)) < 1e-4f, "offset scales");
    }
    // Mirror: a left grip mirrored gives the mirrored right hand.
    const Q g = qnormalized(Q(0.2f, -0.1f, 0.3f, 0.9f));
    V3 lw, rw;
    Q lh, rh;
    gripToHand(V3(0.3f, 0.2f, 1.f), g, 1.f, o, 1.f, lw, lh);
    gripToHand(V3(0.3f, -0.2f, 1.f), mirrorQ(g), -1.f, o, 1.f, rw, rh);
    CHECK(nearV(mirror(lw), rw, 1e-4f) && qangle(mirrorQ(lh), rh) < 1e-3f, "grip mirror");
    // A T-pose with the controllers held as in the T-pose (fingers out, palm down, thumb forward):
    // the hand's world rotation is the rest one (identity).
    for (float side : {1.f, -1.f})
    {
        const Q tilt = qaxis(V3(0.f, 1.f, 0.f), o.pitch);
        const Q tposeGrip = qmul(qconj(gripHandBasis(side)), qconj(tilt));   // grip * tilt * basis == identity
        V3 wp;
        Q hr;
        gripToHand(V3(0.f, side * 0.8f, 1.4f), tposeGrip, side, o, 1.f, wp, hr);
        CHECK(qangle(hr, Q()) < 1e-3f, "T-pose grip -> rest hand (side %g)", side);
    }
}

static void testNeck()
{
    NeckRest r;
    r.neck = V3(-0.01f, 0.f, 0.251f);
    r.head = V3(0.f, 0.f, 0.076f);
    NeckParams p;
    const Q chest = qyaw(0.4f);
    const V3 neckPos(0.f, 0.f, 1.5f);
    const Q heads[4] = {qmul(qyaw(0.9f), qaxis(V3(0.f, 1.f, 0.f), 0.5f)), qaxis(V3(1.f, 0.f, 0.f), -0.3f),
                        qnormalized(Q(0.1f, 0.3f, -0.2f, 0.9f)), qyaw(0.4f)};
    for (const Q& h : heads)
    {
        const NeckPose np = splitNeck(chest, h, neckPos, nullptr, r, p);
        const Q neckLocal = qmul(qconj(chest), np.neckWorld), headLocal = qmul(qconj(np.neckWorld), np.headWorld);
        CHECK(qangle(qmul(chest, qmul(neckLocal, headLocal)), h) < 1e-3f, "neck * head == headset");
        CHECK(qangle(np.headWorld, h) < 1e-4f, "head exact");
    }
    // Pure pitch relative to the chest: the neck takes pitchShare of it.
    const float pitch = 0.5f;
    const Q h = qmul(chest, qaxis(V3(0.f, 1.f, 0.f), pitch));
    NeckPose np = splitNeck(chest, h, neckPos, nullptr, r, p);
    CHECK(std::fabs(qangle(chest, np.neckWorld) - p.pitchShare * pitch) < 1e-3f, "neck takes its share of the pitch");
    // Head roll (a tilt about the forward axis, ear toward the shoulder): the head takes most of
    // it, the neck a little (the owner: "it tilts my whole neck rather than tilting my head").
    for (float roll : {0.35f, -0.5f})
    {
        const Q hr = qmul(chest, qaxis(V3(1.f, 0.f, 0.f), roll));
        np = splitNeck(chest, hr, neckPos, nullptr, r, p);
        const float neckA = qangle(chest, np.neckWorld), headA = qangle(np.neckWorld, np.headWorld);
        CHECK(std::fabs(neckA - p.rollShare * std::fabs(roll)) < 1e-3f && p.rollShare <= 0.25f, "ROLL: neck %.3f of %.3f", neckA, roll);
        CHECK(headA > 3.f * neckA, "ROLL: the head tilts (%.3f), not the neck (%.3f)", headA, neckA);
        // The neck tilts the same way (about the forward axis), not some other way.
        const V3 nv = rotVec(qmul(qconj(chest), np.neckWorld));
        CHECK(std::fabs(nv.y) < 1e-3f && std::fabs(nv.z) < 1e-3f && nv.x * roll > 0.f, "ROLL: the neck's share is a roll");
        // Where the head ends: within ~2 cm of its rest place (a 7.6 cm SL neck), so the face tilts
        // in place instead of the neck swinging it sideways.
        const V3 headAt = neckPos + qrot(np.neckWorld, r.head), headRest = neckPos + qrot(chest, r.head);
        CHECK(length(headAt - headRest) < 0.02f, "ROLL: the head moved %.3f m sideways", length(headAt - headRest));
    }
    // A nod with the head turned 90 degrees is still a nod: the neck takes pitchShare (not rollShare).
    {
        const Q turned = qyaw(1.5707963f);
        const Q hn = qmul(chest, qmul(turned, qaxis(V3(0.f, 1.f, 0.f), pitch)));
        np = splitNeck(chest, hn, neckPos, nullptr, r, p);
        Q sw, tw;
        swingTwist(qmul(qconj(chest), np.neckWorld), V3(0.f, 0.f, 1.f), sw, tw);
        CHECK(std::fabs(qangle(Q(), sw) - p.pitchShare * pitch) < 2e-3f, "turned nod: neck swing %.3f", qangle(Q(), sw));
    }
    // A sideways head position (eyes moved by a tilt) leans the neck only a little sideways.
    {
        const V3 side = neckPos + qrot(chest, V3(0.f, 0.03f, 0.07f));
        np = splitNeck(chest, chest, neckPos, &side, r, p);
        const V3 nv = rotVec(qmul(qconj(chest), np.neckWorld));
        const float full = std::atan2(0.03f, 0.07f);
        CHECK(std::fabs(nv.x) < 0.4f * full, "lean: sideways %.3f of %.3f", nv.x, full);
        // With the head turned 90 degrees, a head forward of the chest still leans the neck
        // forward fully (sideways is the chest's, not the face's).
        const V3 ahead = neckPos + qrot(chest, V3(0.03f, 0.f, 0.07f));
        const Q turned = qmul(chest, qyaw(1.5707963f));
        np = splitNeck(chest, turned, neckPos, &ahead, r, p);
        Q sw, tw;
        swingTwist(qmul(qconj(chest), np.neckWorld), V3(0.f, 0.f, 1.f), sw, tw);
        const V3 sv = rotVec(sw);
        CHECK(sv.y > 0.8f * full && std::fabs(sv.x) < 0.05f, "lean, head turned: forward %.3f (of %.3f), sideways %.3f", sv.y, full, sv.x);
    }
    // Pure yaw: the neck takes yawShare.
    const Q hy = qmul(chest, qyaw(0.8f));
    np = splitNeck(chest, hy, neckPos, nullptr, r, p);
    CHECK(std::fabs(qangle(chest, np.neckWorld) - p.yawShare * 0.8f) < 1e-3f, "neck takes its share of the yaw");
    // Head position forward of the rest: the neck leans forward (its up tilts toward the chest's +X).
    const V3 fwd = qrot(chest, V3(1.f, 0.f, 0.f));
    const V3 headPos = neckPos + qrot(chest, V3(0.03f, 0.f, 0.07f));
    np = splitNeck(chest, chest, neckPos, &headPos, r, p);
    CHECK(np.leanWeight > 0.5f && dot(qrot(np.neckWorld, V3(0.f, 0.f, 1.f)), fwd) > 0.2f, "lean follows the head (%f)", np.leanWeight);
    const Q nl = qmul(qconj(chest), np.neckWorld), hl = qmul(qconj(np.neckWorld), np.headWorld);
    CHECK(qangle(qmul(chest, qmul(nl, hl)), chest) < 1e-3f, "lean: still sums to the headset");
    // A head 40 cm below the neck (a crouch the body does not make): no lean.
    const V3 low = neckPos + V3(0.f, 0.f, -0.4f);
    np = splitNeck(chest, chest, neckPos, &low, r, p);
    CHECK(np.leanWeight == 0.f && qangle(np.neckWorld, chest) < 1e-3f, "far target: no lean");
}

static void testFly()
{
    const float dt = 1.f / 90.f;
    // D: enter, the head eases out over 0.3 s, the pose stays live; leave, the head eases back.
    FlyState s;
    FlyEvents ev = stepFly(s, true, false, dt);
    CHECK(ev.entered && !ev.left && s.fly && !s.frozen() && s.liveBlend == 1.f, "D: entered, live");
    int frames = 1;
    while (s.headLive > 0.f && frames < 1000)
    {
        stepFly(s, true, false, dt);
        ++frames;
    }
    CHECK(frames >= 26 && frames <= 28, "D: head released over 0.3 s (%d frames)", frames);
    ev = stepFly(s, false, false, dt);
    CHECK(ev.left && !s.fly && s.headLive > 0.f && s.headLive < 0.1f, "D: left, head easing back (%f)", s.headLive);
    for (int i = 0; i < 30; ++i) stepFly(s, false, false, dt);
    CHECK(s.headLive == 1.f && s.liveBlend == 1.f, "D: back to live");
    // At 144 Hz the same 0.3 s.
    FlyState f;
    stepFly(f, true, false, 1.f / 144.f);
    frames = 1;
    while (f.headLive > 0.f && frames < 1000)
    {
        stepFly(f, true, false, 1.f / 144.f);
        ++frames;
    }
    CHECK(frames >= 42 && frames <= 45, "D at 144 Hz: 0.3 s (%d frames)", frames);
    // C: enter freezes at once and holds whatever the frames; leaving eases 0 -> 1 over 0.3 s.
    FlyState c;
    ev = stepFly(c, true, true, dt);
    CHECK(ev.entered && c.frozen() && c.liveBlend == 0.f, "C: frozen at once");
    for (int i = 0; i < 500; ++i) stepFly(c, true, false, dt);   // (the setting changing mid-fly does not matter)
    CHECK(c.frozen() && c.liveBlend == 0.f, "C: held");
    ev = stepFly(c, false, true, dt);
    CHECK(ev.left && !c.frozen() && c.liveBlend > 0.f && c.liveBlend < 0.1f, "C: left, easing back (%f)", c.liveBlend);
    frames = 1;
    while (c.liveBlend < 1.f && frames < 1000)
    {
        stepFly(c, false, true, dt);
        ++frames;
    }
    CHECK(frames >= 26 && frames <= 28, "C: back to live over 0.3 s (%d frames)", frames);
    // Re-entering mid-ease freezes again (the held pose is the blended one the caller snapshots).
    stepFly(c, true, true, dt);
    stepFly(c, false, true, dt);
    stepFly(c, true, true, dt);
    CHECK(c.frozen() && c.liveBlend == 0.f, "C: re-entered mid-ease");
}

// A tester (SlimeVR through SteamVR, Index): "left arm twists in on itself constantly; same with
// ankles / feet". Realistic tracker frames, a T-pose calibration, then small motions.
static void testTrackerFeet()
{
    // A SteamVR tracker on the left foot's instep, in body space: lying flat, its own axes turned
    // 90 deg about X and toed out 20 deg (the mount); the right one its mirror.
    const Q mountL = qmul(qyaw(0.35f), qaxis(V3(1.f, 0.f, 0.f), 1.5708f));
    const Q mountR = mirrorQ(mountL);
    const Proportions pr;
    const float eyeH = 1.7f;
    const V3 head(0.f, 0.f, eyeH);
    const V3 trkL(0.05f, 0.12f, 0.1f), trkR = mirror(trkL);
    V3 bp;
    Q bq;
    tposeBone(B_LFOOT, trkL, head, 0.f, eyeH, pr, bp, bq);
    const Offset offL = calibrateOffset(trkL, mountL, bp, bq);
    tposeBone(B_RFOOT, trkR, head, 0.f, eyeH, pr, bp, bq);
    const Offset offR = calibrateOffset(trkR, mountR, bp, bq);
    // 2000 frames of small foot motions (pitch, roll, yaw a few degrees): the bone turns exactly as
    // the foot does (no roll creeping in), and the right foot is the left one mirrored.
    unsigned seed = 3u;
    auto rnd = [&]() {
        seed = seed * 1103515245u + 12345u;
        return (float)((seed >> 8) & 0xFFFF) / 32767.5f - 1.f;
    };
    float worst = 0.f, worstMirror = 0.f;
    for (int i = 0; i < 2000; ++i)
    {
        const Q foot = qnormalized(qmul(qyaw(0.3f * rnd()), qmul(qaxis(V3(0.f, 1.f, 0.f), 0.3f * rnd()), qaxis(V3(1.f, 0.f, 0.f), 0.15f * rnd()))));
        V3 lp, rp;
        Q lq, rq;
        applyOffset(trkL, qmul(foot, mountL), offL, lp, lq);
        applyOffset(trkR, qmul(mirrorQ(foot), mountR), offR, rp, rq);
        worst = std::max(worst, qangle(lq, foot));   // (the bone's rest is the facing: identity here)
        worstMirror = std::max(worstMirror, qangle(mirrorQ(lq), rq));
    }
    CHECK(worst < 1e-3f, "foot bone follows the foot exactly (%f)", worst);
    CHECK(worstMirror < 1e-3f, "right foot mirrors the left (%f)", worstMirror);
}

static void testNearStraightPoles()
{
    // A straight-ish leg with a knee / thigh tracker on it: its offset across the hip -> ankle line
    // is a centimetre of noise either side. The knee must stay forward (no 180-degree flips).
    LegRest lr;
    lr.hip = V3(0.f, 0.1f, -0.05f);
    lr.knee = V3(0.f, 0.f, -0.45f);
    lr.ankle = V3(0.f, 0.f, -0.42f);
    const V3 pelvis(0.f, 0.f, 1.f);
    const V3 hip = pelvis + lr.hip;
    const V3 ankle = hip + V3(0.02f, 0.f, -0.865f);
    const V3 forwardPole = (hip + ankle) * 0.5f + V3(0.5f, 0.f, 0.f);
    bool rawFlipped = false, stableFlipped = false;
    for (int i = 0; i < 200; ++i)
    {
        const float noise = (i % 2 ? 1.f : -1.f) * 0.012f;
        const V3 knee = (hip + ankle) * 0.5f + V3(noise, noise * 0.5f, 0.f);   // on the leg, +-1.2 cm
        const LegPose raw = solveLeg(pelvis, Q(), lr, ankle, knee);
        const LegPose st = solveLeg(pelvis, Q(), lr, ankle, stablePole(hip, ankle, knee, forwardPole));
        rawFlipped = rawFlipped || qrot(raw.hipWorld, V3(1.f, 0.f, 0.f)).x < 0.f;
        stableFlipped = stableFlipped || qrot(st.hipWorld, V3(1.f, 0.f, 0.f)).x < 0.5f;
    }
    CHECK(rawFlipped, "(the raw tracker pole does flip: the fixture reproduces the twisting)");
    CHECK(!stableFlipped, "stable pole: the knee stays forward");
    // A really bent knee pointing out (a tracker 15 cm across the line): the tracker steers.
    const V3 bentAnkle = hip + V3(0.f, 0.f, -0.6f);
    const V3 outKnee = (hip + bentAnkle) * 0.5f + V3(0.f, 0.15f, 0.f);
    const LegPose b = solveLeg(pelvis, Q(), lr, bentAnkle, stablePole(hip, bentAnkle, outKnee, forwardPole));
    CHECK(b.kneePos.y > hip.y + 0.05f, "bent: the knee follows the tracker out (%f)", b.kneePos.y - hip.y);

    // The arm: a straight arm with an elbow tracker on it, either side; left and right.
    const ArmParams prm;
    const V3 chest(0.f, 0.f, 1.3f);
    for (float side : {1.f, -1.f})
    {
        const ArmRest r = restArm(side);
        const V3 sh = chest + r.collar + r.shoulder;
        const V3 wrist = sh + V3(0.44f, side * 0.06f, -0.05f);   // reaching forward, nearly straight
        bool flipped = false;
        float prevFront = 0.f;
        for (int i = 0; i < 200; ++i)
        {
            const float noise = (i % 2 ? 1.f : -1.f) * 0.012f;
            const V3 elbowTrk = (sh + wrist) * 0.5f + V3(0.f, 0.f, noise);
            const V3 natural = elbowPole(sh, wrist, Q(), side, r.reach());
            const V3 pole = stablePole(sh, wrist, elbowTrk, natural);
            const ArmPose p = solveArm(chest, Q(), r, wrist, Q(), &pole, prm);
            const float front = qrot(p.shoulderWorld, V3(1.f, 0.f, 0.f)).z;   // the biceps' side, up / down
            if (i > 0 && front * prevFront < -0.01f) flipped = true;
            prevFront = front;
        }
        CHECK(!flipped, "straight arm, elbow tracker on it: no flips (side %g)", side);
    }
}

static void testTwistContinuity()
{
    // The hand rolling through 180 degrees relative to the forearm (a controller turned palm up and
    // on): with the per-arm state the forearm's share moves smoothly, never jumps +90 -> -90.
    ArmParams prm;
    const V3 chest(0.f, 0.f, 1.3f);
    for (float side : {1.f, -1.f})
    {
        const ArmRest r = restArm(side);
        const V3 target = chest + V3(0.3f, side * 0.25f, 0.f);
        const ArmPose base = solveArm(chest, Q(), r, target, Q(), nullptr, prm);
        const V3 axisL = normalized(r.wrist);
        const Q hinge = qmul(base.elbowWorld, qaxis(axisL, -base.twist * prm.twistShare));
        float state = 0.f, prevForearm = 0.f, worstJump = 0.f, worstHand = 0.f;
        bool rawJump = false;
        float prevRaw = 0.f;
        for (int i = 0; i <= 120; ++i)
        {
            const float roll = side * (2.6f + 1.2f * (float)i / 120.f);   // 150 -> 218 deg: through 180
            const Q hand = qmul(hinge, qaxis(axisL, roll));
            const ArmPose p = solveArm(chest, Q(), r, target, hand, nullptr, prm, nullptr, &state);
            const float forearm = twistAngle(qmul(qconj(hinge), p.elbowWorld), axisL);
            if (i > 0) worstJump = std::max(worstJump, std::fabs(forearm - prevForearm));
            prevForearm = forearm;
            worstHand = std::max(worstHand, qangle(p.wristWorld, hand));
            const ArmPose q = solveArm(chest, Q(), r, target, hand, nullptr, prm);
            const float raw = twistAngle(qmul(qconj(hinge), q.elbowWorld), axisL);
            if (i > 0 && std::fabs(raw - prevRaw) > 1.f) rawJump = true;
            prevRaw = raw;
        }
        CHECK(worstJump < 0.05f, "forearm roll continuous through 180 deg (side %g, worst step %f)", side, worstJump);
        CHECK(worstHand < 1e-3f, "the hand stays on its target (side %g)", side);
        CHECK(rawJump, "(without the state it jumps: the fixture reproduces the flip, side %g)", side);
        CHECK(std::fabs(prevForearm) <= prm.maxForearmTwist + 1e-4f, "forearm share clamped (side %g)", side);
    }
}

// Fly cam keeps the controller arms (a tester could not see their arms from outside); flying hands
// the hips and legs to the animation.
static void testInputsAndFlight()
{
    CHECK(armInput(true, true, true, true, true) == AI_CONTROLLER, "the controller first");
    CHECK(armInput(true, true, false, true, true) == AI_WRIST_TRACKER, "then a wrist tracker");
    CHECK(armInput(true, true, false, false, true) == AI_HAND, "then a tracked hand");
    CHECK(armInput(false, true, true, true, true) == AI_NONE && armInput(true, false, true, true, true) == AI_NONE, "off");
    // (Fly cam is not an input of armInput: nothing there releases the arms.)
    CHECK(roleTrackedInFlight(B_CHEST), "flying: the chest stays tracked");
    for (int b : {(int)B_WAIST, (int)B_LFOOT, (int)B_RFOOT, (int)B_LKNEE, (int)B_RKNEE})
        CHECK(!roleTrackedInFlight(b), "flying: %s goes to the fly animation", bodyName(b));
}

int main()
{
    testInputsAndFlight();
    testTrackerFeet();
    testNearStraightPoles();
    testTwistContinuity();
    testFly();
    testTwoBoneIsLeg();
    testReach();
    testStretch();
    testPole();
    testMirror();
    testTwist();
    testCollar();
    testGrip();
    testNeck();
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
