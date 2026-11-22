// Fixtures for the Pose Studio IK geometry (#530, #1566; indra/newview/slposeikmath.h), run by
// scripts/check/checks/pose_ik.py (compiled with the host C++ compiler, no viewer build).
//   TWOBONE   bone lengths kept; the goal reached when in range, straight and clamped to the
//             limb's length when not; the hinge never folds past max_bend.
//   NOFLIP    (#1566 item 4) the knee keeps the side it bent to at mouse down while the foot is
//             dragged anywhere in front of, behind, above or across the hip: the bend plane's
//             normal is held, and a continuous goal path never makes the knee jump.
//   POLE      a pole the user placed decides the side (Blender).
//   FABRIK    a 6-link tail reaches a reachable goal with every link length kept, points
//             straight when out of reach, and moves a little when the goal moves a little.
//   REACH     neck + head + nose as one chain: the nose lands on the pulled point.
//   GAZE      (#1566 item 6) eyes first, then head, then neck, each within its limit.
//   EYES      (#1566) an eye aims from its own rest axis (+X for every SL eye bone, any other
//             forward too) within its cone; "Auto" picks the alt eyes on a Bento mesh head,
//             the nose left / right bones (Open Pony) only when no eye bone is rigged.
//   FOLLOW    (#1566) a placed target rides on the bone its chain hangs from: turning and
//             moving the head carries an ear target with it (same place and direction
//             relative to the head); a pinned target, a planted leg and the dragged chain hold.
//   RIGGED    (#1566) "a worn rigged mesh uses this joint": a mesh's CPU vertex weights when it
//             has them, else its skin's joint list (GPU-skinned wings still count).
//   LOOKAT    (#1567) the live look-at gazes naturally: a glance inside the eye range moves
//             only the eyes; past it the head follows until it has caught up; both eyes aim
//             at the target point and converge at its distance (near targets clamped).
//   PUPIL     (#1567) pupil eyes (the Open Pony): the dev kit's eye rig (an arm on a pivot aimed
//             at the target, its tip's nearest point on the eye shell, the bone turned to the
//             shell's normal there) reproduced: the straight and 30 degree looks land where the
//             rig puts them on its mesh (0.3 mm) and the pupil lies flat on the eye (0.5 deg),
//             a tip deep under the eye finds the eye's front like the rig, the look is held in the eye range,
//             left / right mirror and slide the same way, and like the rig (each eye aims at
//             the target moved by its own offset) a near target does not cross the eyes; the
//             linear model's vergence grows as the target nears, inside its ellipse.
//   ANIM      (#1566 item 8) a saved .anim's ease-in / ease-out found past the emote name and
//             rewritten in place; anything that is not a version 1.0 header is refused.
#include "slanimheader.h"
#include "slposeikmath.h"

#include <cstring>

#include <cstdio>
#include <vector>

using namespace SLPoseIKMath;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static const double DEG = 3.14159265358979323846 / 180.0;

static bool approx(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// A human leg at rest: hip at the origin, knee slightly forward (+X is forward), ankle below.
static const V3 HIP(0, 0, 0);
static const V3 KNEE(0.04, 0, -0.45);
static const V3 ANKLE(0, 0, -0.9);
static const V3 BEND_SIDE(1, 0, 0);     // a straight leg's knee goes forward

static TwoBoneResult solveLeg(const BendFrame& frame, const V3& goal, const V3* pole, double max_bend = 160.0)
{
    const double la = length(KNEE - HIP), lb = length(ANKLE - KNEE);
    const V3 n = normalized(goal - HIP, V3(0, 0, -1));
    return twoBone(HIP, la, lb, goal, bendSide(HIP, n, frame, pole), max_bend, pole ? 10.0 : 0.0);
}

static void testTwoBone()
{
    const BendFrame frame = captureBendFrame(HIP, KNEE, ANKLE, BEND_SIDE);
    const double la = length(KNEE - HIP), lb = length(ANKLE - KNEE);
    // In reach.
    const V3 goal(0.2, 0.1, -0.7);
    TwoBoneResult r = solveLeg(frame, goal, nullptr);
    CHECK(approx(length(r.mKnee - HIP), la, 1e-6) && approx(length(r.mReach - r.mKnee), lb, 1e-6), "bone lengths kept");
    CHECK(length(r.mReach - goal) < 1e-6, "reachable goal reached (off by %g)", length(r.mReach - goal));
    // Out of reach: straight toward the goal, no longer than the limb.
    const V3 far_goal(0.5, 0, -2.0);
    r = solveLeg(frame, far_goal, nullptr);
    CHECK(length(r.mReach - HIP) <= la + lb + 1e-6, "out of reach: clamped to the limb's length");
    CHECK(angleBetween(r.mKnee - HIP, far_goal - HIP) < 0.5 * DEG, "out of reach: straight at the goal");
    // Too close: the hinge stops at max_bend.
    r = solveLeg(frame, V3(0.01, 0, -0.02), nullptr, 150.0);
    const double inner = angleBetween(HIP - r.mKnee, r.mReach - r.mKnee);
    CHECK(inner >= (180.0 - 150.0) * DEG - 1e-3, "hinge folds no more than max_bend (inner %g deg)", inner / DEG);
}

static void testNoFlip()
{
    const BendFrame frame = captureBendFrame(HIP, KNEE, ANKLE, BEND_SIDE);
    // Kick: the foot swept from straight down, forward, up to hip height and back behind --
    // the knee stays in front of the hip -> foot line on the same side of the bend plane.
    V3 last_side;
    bool have_last = false;
    for (int step = 0; step <= 180; ++step)
    {
        const double ang = (-60.0 + step) * DEG;     // -60 (behind) .. 120 (forward and up)
        const V3 goal(0.75 * std::sin(ang), 0.0, -0.75 * std::cos(ang));
        const TwoBoneResult r = solveLeg(frame, goal, nullptr);
        const V3 plane = normalized(cross(r.mReach - HIP, r.mKnee - HIP), V3());
        CHECK(dot(plane, frame.mNormal) > 0.999, "kick %d deg: bend plane turned (dot %g)", step - 60, dot(plane, frame.mNormal));
        const V3 n = normalized(goal - HIP, V3());
        const V3 side = normalized(across(r.mKnee - HIP, n), V3());
        if (have_last)
            CHECK(angleBetween(side, last_side) < 3.0 * DEG, "kick %d deg: knee side jumped %g deg", step - 60,
                  angleBetween(side, last_side) / DEG);
        last_side = side;
        have_last = true;
    }
    // Sideways and through the ambiguous direction: the foot dragged from far left to far
    // right at knee height (passes along the bend plane's normal) -- never a jump.
    have_last = false;
    for (int step = 0; step <= 240; ++step)
    {
        const double y = -0.8 + step * (1.6 / 240.0);
        const V3 goal(0.05, y, -0.15);
        const TwoBoneResult r = solveLeg(frame, goal, nullptr);
        const V3 n = normalized(goal - HIP, V3());
        const V3 side = normalized(across(r.mKnee - HIP, n), V3());
        if (have_last)
            CHECK(angleBetween(side, last_side) < 12.0 * DEG, "sideways y=%.3f: knee side jumped %g deg", y,
                  angleBetween(side, last_side) / DEG);
        last_side = side;
        have_last = true;
    }
    // A straight leg at mouse down still knows its side (the chain's bend direction).
    const BendFrame straight = captureBendFrame(HIP, V3(0, 0, -0.45), ANKLE, BEND_SIDE);
    const TwoBoneResult r = solveLeg(straight, V3(0, 0, -0.6), nullptr);
    CHECK(r.mKnee.x > 0.1, "straight leg bends its knee forward (x %g)", r.mKnee.x);
}

static void testPole()
{
    const BendFrame frame = captureBendFrame(HIP, KNEE, ANKLE, BEND_SIDE);
    const V3 pole(-0.6, 0, -0.45);       // behind the knee
    const TwoBoneResult r = solveLeg(frame, V3(0, 0, -0.6), &pole);
    CHECK(r.mKnee.x < -0.1, "a pole behind the knee bends it backward (x %g)", r.mKnee.x);
    // A set pole keeps a little bend even when the goal is far.
    const TwoBoneResult far_r = solveLeg(frame, V3(0, 0, -3.0), &pole);
    const double bend = 180.0 * DEG - angleBetween(HIP - far_r.mKnee, far_r.mReach - far_r.mKnee);
    CHECK(bend >= 10.0 * DEG - 1e-3, "pole keeps its 10 degree bend (%g)", bend / DEG);
}

static std::vector<V3> tail()
{
    std::vector<V3> p;
    for (int i = 0; i < 7; ++i)
        p.push_back(V3(-0.1 * i, 0, -0.02 * i * i * 0.1));
    return p;
}

static void testFabrik()
{
    const std::vector<V3> p = tail();
    std::vector<double> len;
    double total = 0.0;
    for (size_t i = 0; i + 1 < p.size(); ++i)
    {
        len.push_back(length(p[i + 1] - p[i]));
        total += len.back();
    }
    const V3 goal(-0.35, 0.25, 0.2);
    std::vector<V3> q = fabrik(p, goal, 64, 1e-5);
    CHECK(length(q.back() - goal) < 1e-4, "tail reaches a reachable goal (off by %g)", length(q.back() - goal));
    CHECK(length(q.front() - p.front()) < 1e-12, "tail root stays");
    for (size_t i = 0; i + 1 < q.size(); ++i)
        CHECK(approx(length(q[i + 1] - q[i]), len[i], 1e-6), "tail link %d keeps its length", (int)i);
    const V3 far_goal(-2.0, 0.0, 1.0);
    q = fabrik(p, far_goal, 64, 1e-5);
    for (size_t i = 1; i < q.size(); ++i)
        CHECK(angleBetween(q[i] - q[0], far_goal - q[0]) < 1e-3, "out of reach: joint %d on the line", (int)i);
    // Continuity: a 1 mm goal move moves no joint more than a few mm.
    const std::vector<V3> a = fabrik(p, goal, 64, 1e-6);
    const std::vector<V3> b = fabrik(p, goal + V3(0.001, 0, 0), 64, 1e-6);
    double worst = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, length(a[i] - b[i]));
    CHECK(worst < 0.02, "tail: 1 mm goal move moves a joint %g m", worst);
    (void)total;
}

static void testReach()
{
    // Neck base, head, nose (in front of the head).
    const std::vector<V3> p = { V3(0, 0, 0), V3(0, 0, 0.1), V3(0.11, 0, 0.13) };
    const V3 goal(0.18, 0.06, 0.05);    // pulled forward, down and to one side
    const std::vector<V3> q = fabrik(p, goal, 64, 1e-5);
    CHECK(length(q.back() - goal) < 1e-4, "nose lands on the pulled point (off by %g)", length(q.back() - goal));
    CHECK(approx(length(q[1] - q[0]), 0.1, 1e-6), "neck keeps its length");
    CHECK(approx(length(q[2] - q[1]), length(p[2] - p[1]), 1e-6), "head -> nose keeps its length");
}

static void testGaze()
{
    const double eyes = 30 * DEG, head = 45 * DEG, neck = 35 * DEG;
    GazeShares s = gazeShares(10 * DEG, eyes, head, neck);
    CHECK(approx(s.mEyes, 10 * DEG, 1e-9) && s.mHead == 0.0 && s.mNeck == 0.0, "small turn: eyes only");
    s = gazeShares(50 * DEG, eyes, head, neck);
    CHECK(approx(s.mEyes, eyes, 1e-9) && approx(s.mHead, 20 * DEG, 1e-9) && s.mNeck == 0.0, "medium: eyes full, head the rest");
    s = gazeShares(100 * DEG, eyes, head, neck);
    CHECK(approx(s.mEyes, eyes, 1e-9) && approx(s.mHead, head, 1e-9) && approx(s.mNeck, 25 * DEG, 1e-9), "large: neck engaged");
    s = gazeShares(200 * DEG, eyes, head, neck);
    CHECK(approx(s.mNeck, neck, 1e-9) && approx(s.mUnreached, 90 * DEG, 1e-9), "beyond every limit: clamped, rest unreached");
    s = gazeShares(-1.0, eyes, head, neck);
    CHECK(s.mEyes == 0.0 && s.mHead == 0.0 && s.mNeck == 0.0, "no turn asked: nothing moves");
}

// v turned by angle about the unit axis (Rodrigues).
static V3 turned(const V3& v, const V3& axis, double angle)
{
    const double c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c));
}

static void testEyes()
{
    const double limit = 30 * DEG;
    // Classic / alt / nose eyes all bind unrotated: rest axis +X (the way the avatar faces).
    const V3 fwd(1, 0, 0);
    V3 dir(std::cos(20 * DEG), std::sin(20 * DEG), 0);
    EyeTurn t = eyeTurn(fwd, dir, limit);
    CHECK(approx(t.mAngle, 20 * DEG, 1e-9), "+X eye, 20 deg left: turns 20 (got %g)", t.mAngle / DEG);
    CHECK(length(turned(fwd, t.mAxis, t.mAngle) - dir) < 1e-9, "+X eye lands on the target");
    dir = V3(std::cos(70 * DEG), 0, std::sin(70 * DEG));
    t = eyeTurn(fwd, dir, limit);
    CHECK(approx(t.mAngle, limit, 1e-9), "+X eye, 70 deg up: clamped to the limit");
    CHECK(approx(angleBetween(turned(fwd, t.mAxis, t.mAngle), dir), 40 * DEG, 1e-9), "clamped eye stops 40 deg short, toward it");
    // Another forward axis (a data-defined eye set aiming along +Y): aimed from that axis.
    const V3 side(0, 1, 0);
    dir = V3(std::sin(15 * DEG), std::cos(15 * DEG), 0);
    t = eyeTurn(side, dir, limit);
    CHECK(approx(t.mAngle, 15 * DEG, 1e-9), "+Y eye: turns from its own axis (got %g)", t.mAngle / DEG);
    CHECK(length(turned(side, t.mAxis, t.mAngle) - dir) < 1e-9, "+Y eye lands on the target");
    t = eyeTurn(fwd, fwd * 3.0, limit);
    CHECK(t.mAngle == 0.0, "target straight ahead: no turn");

    CHECK(autoEyeSet(false, false, false) == EYE_SET_EYES, "nothing rigged (classic avatar): eyes");
    CHECK(autoEyeSet(true, false, false) == EYE_SET_EYES, "mesh eyes on mEye*: eyes");
    CHECK(autoEyeSet(false, true, false) == EYE_SET_ALT, "Bento head on alt eyes: alt");
    CHECK(autoEyeSet(false, true, true) == EYE_SET_ALT, "Bento head, nostrils on the nose bones: still alt");
    CHECK(autoEyeSet(true, true, false) == EYE_SET_BOTH, "both pairs rigged: both");
    CHECK(autoEyeSet(true, false, true) == EYE_SET_EYES, "eyes + nostrils rigged: eyes, never the nose");
    CHECK(autoEyeSet(false, false, true) == EYE_SET_NOSE, "only the nose bones (Open Pony): nose");
}

static void testFollow()
{
    // The head (an ear chain's anchor) at the origin, facing +X; the ear target 0.1 m above
    // it and 0.05 m to the left, pointing up.
    Frame was;
    was.mOrigin = V3(0, 0, 1.6);
    const V3 ear = V3(0, 0.05, 1.7);
    const V3 ear_dir(0, 0, 1);
    // Turn the head 40 degrees about Z (looking left), tip it 25 degrees forward about its
    // new Y, and move it 0.1 m forward (a nose reach bending the neck).
    const V3 z(0, 0, 1);
    Frame now;
    now.mOrigin = was.mOrigin + V3(0.1, 0, -0.02);
    now.mX = turned(was.mX, z, 40 * DEG);
    now.mY = turned(was.mY, z, 40 * DEG);
    now.mZ = was.mZ;
    const V3 pitch_axis = now.mY;
    now.mX = turned(now.mX, pitch_axis, 25 * DEG);
    now.mZ = turned(now.mZ, pitch_axis, 25 * DEG);
    const V3 moved = carryPoint(was, now, ear);
    const V3 d0 = ear - was.mOrigin, d1 = moved - now.mOrigin;
    CHECK(approx(dot(d1, now.mX), dot(d0, was.mX), 1e-9) && approx(dot(d1, now.mY), dot(d0, was.mY), 1e-9)
              && approx(dot(d1, now.mZ), dot(d0, was.mZ), 1e-9), "ear target keeps its place relative to the head");
    CHECK(length(moved - ear) > 0.02, "ear target moved with the head (%g m)", length(moved - ear));
    const V3 dir = carryDir(was, now, ear_dir);
    CHECK(approx(angleBetween(dir, now.mZ), 0.0, 1e-9), "ear target keeps its direction relative to the head");
    CHECK(approx(length(carryPoint(was, was, ear) - ear), 0.0, 1e-12), "head still: target still");

    CHECK(targetFollowsAnchor(true, false, false, false), "placed, unpinned: follows its bone");
    CHECK(!targetFollowsAnchor(true, true, false, false), "pinned: holds (its chain re-solves)");
    CHECK(!targetFollowsAnchor(true, false, true, false), "planted leg: holds");
    CHECK(!targetFollowsAnchor(true, false, false, true), "the chain being dragged: the mouse has it");
    CHECK(!targetFollowsAnchor(false, false, false, false), "never placed: follows its own bones already");
}

static void testRiggedJoints()
{
    std::vector<bool> used(140, false);
    // A body with CPU weights: only its weighted joints count, not its long skin list.
    CHECK(addMeshJoints(used, { 0, 1, 2 }, { 0, 1, 2, 120, 121 }) == 3, "weights win over the skin list");
    CHECK(!used[120], "listed but unweighted joint not counted when weights exist");
    // GPU-skinned wings (no CPU weights): the skin's joint list counts.
    CHECK(addMeshJoints(used, {}, { 120, 121, 2 }) == 2, "no CPU weights: the skin joint list");
    CHECK(used[120] && used[121], "wing bones used");
    CHECK(addMeshJoints(used, {}, { -1, 500 }) == 0, "unknown / out of range joints skipped");
    std::vector<int> everything;
    for (int j = 0; j < 130; ++j)
        everything.push_back(j);
    std::vector<bool> human(140, false);
    CHECK(addMeshJoints(human, {}, everything) == 0 && !human[125], "a GPU-skinned body listing every bone adds nothing");
    CHECK(addMeshJoints(used, {}, {}) == 0, "a mesh with nothing adds nothing");
}

static void testLookAtGaze()
{
    const double range = 18 * DEG, settle = 2 * DEG;
    HeadFollow h;
    headFollow(h, 10 * DEG, 0, 0, 0, range, settle);
    CHECK(!h.mTracking && h.mGoalYaw == 0.0, "10 deg glance: the eyes only, the head stays");
    headFollow(h, 40 * DEG, 5 * DEG, 0, 0, range, settle);
    CHECK(h.mTracking && approx(h.mGoalYaw, 40 * DEG, 1e-12), "40 deg: the head follows");
    headFollow(h, 42 * DEG, 5 * DEG, 20 * DEG, 2 * DEG, range, settle);
    CHECK(h.mTracking && approx(h.mGoalYaw, 42 * DEG, 1e-12), "while catching up the head keeps following the look");
    headFollow(h, 42 * DEG, 5 * DEG, 41 * DEG, 5 * DEG, range, settle);
    CHECK(!h.mTracking, "caught up: back to glancing");
    headFollow(h, 50 * DEG, 5 * DEG, 41 * DEG, 5 * DEG, range, settle);
    CHECK(!h.mTracking && approx(h.mGoalYaw, 42 * DEG, 1e-12), "a small glance after that: eyes only again");
    HeadFollow w;
    w.mGoalYaw = 175 * DEG;
    headFollow(w, -175 * DEG, 0, 175 * DEG, 0, range, settle);
    CHECK(!w.mTracking, "across +-180: a 10 degree glance, not a full turn");

    // Vergence: each eye aims at the point itself, from 3.2 cm either side of the centre.
    const V3 left(0, 0.032, 0), right(0, -0.032, 0), fwd(1, 0, 0);
    const double limit = 30 * DEG;
    for (double d : { 2.0, 0.5 })
    {
        const V3 target(gazeDistance(d, 0.3, 50.0, 2.0), 0, 0);
        const EyeTurn tl = eyeTurn(fwd, target - left, limit), tr = eyeTurn(fwd, target - right, limit);
        const V3 al = turned(fwd, tl.mAxis, tl.mAngle), ar = turned(fwd, tr.mAxis, tr.mAngle);
        CHECK(length(cross(al, normalized(target - left, fwd))) < 1e-9 && length(cross(ar, normalized(target - right, fwd))) < 1e-9,
              "%.1f m: both eyes on the target", d);
        CHECK(al.y < 0 && ar.y > 0, "%.1f m: the eyes turn inward (converge)", d);
    }
    CHECK(gazeDistance(0.05, 0.3, 50.0, 2.0) == 0.3, "a target at the face: held at the near limit");
    CHECK(gazeDistance(-1.0, 0.3, 50.0, 2.0) == 2.0 && gazeDistance(500.0, 0.3, 50.0, 2.0) == 50.0, "unknown / far distances");
}

// The Open Pony entry of ik_chains.xml (SL, parent frame, metres, from the bind pupil).
static PupilBone ponyPupil(double side)
{
    PupilBone p;
    p.mModel = PupilBone::SHELL;
    p.mPivot = V3(-0.05818, -0.02276 * side, -0.00981);
    p.mArm = 0.05587;
    p.mAimOffset = V3(0, 0.09193 * side, 0);
    p.mAimCenter = V3(-0.07582, -0.09209 * side, 0.00272);
    p.mShellCenter = V3(-0.026377, -0.019559 * side, 0.000427);
    p.mShellAxes[0] = V3(0.000039, -0.000028 * side, 0.124796);
    p.mShellAxes[1] = V3(-0.065334, 0.067007 * side, 0.000035);
    p.mShellAxes[2] = V3(0.023134, 0.022557 * side, -0.000002);
    p.mAxis = V3(0.7281, 0.6856 * side, 0);
    return p;
}

// Where the pupil's rest axis points once turned: the eye's normal under the pupil.
static V3 turnedAxis(const PupilBone& p, const PupilPose& pose)
{
    const V3 a = normalized(p.mAxis, V3(1, 0, 0)), k = pose.mTurn.mAxis;
    const double c = std::cos(pose.mTurn.mAngle), s = std::sin(pose.mTurn.mAngle);
    return a * c + cross(k, a) * s + k * (dot(k, a) * (1.0 - c));     // Rodrigues
}

static void testPupil()
{
    const V3 fwd(1, 0, 0);
    const double limit = 30 * DEG;
    const PupilBone left = ponyPupil(1), right = ponyPupil(-1);
    auto near_mm = [](const V3& a, const V3& b, double mm) { return length(a - b) <= mm * 0.001; };
    auto look = [](double yaw, double pitch)
    { return V3(50.0 * std::cos(yaw * DEG) * std::cos(pitch * DEG), 50.0 * std::sin(yaw * DEG) * std::cos(pitch * DEG), 50.0 * std::sin(pitch * DEG)); };
    // The dev kit's rig on its own mesh (OBIKEye, nearest point on the triangles, the normal
    // there; scratch measurement for #1567): the pupil lands on the eye within 0.3 mm and lies
    // flat on it (its rest axis along the eye's normal within half a degree).
    struct Ref { double yaw, pitch; V3 point, normal; double mm; };
    const Ref refs[] = {
        { 0, 0, V3(0.01025, -0.01212, -0.01014), V3(0.7676, 0.6405, -0.0230), 0.3 },
        { 30, 0, V3(-0.00770, 0.00730, -0.00987), V3(0.6983, 0.7155, -0.0226), 0.3 },
        { 0, 30, V3(0.00669, -0.00832, 0.01891), V3(0.7543, 0.6553, 0.0397), 0.3 },
        { 0, -30, V3(0.00580, -0.00939, -0.03965), V3(0.7535, 0.6514, -0.0889), 0.3 },
    };
    for (const Ref& r : refs)
    {
        const PupilPose lp = pupilPose(left, look(r.yaw, r.pitch), fwd, limit);
        CHECK(near_mm(lp.mOffset, r.point, r.mm), "yaw %g pitch %g: left pupil (%.2f, %.2f, %.2f) mm, rig (%.2f, %.2f, %.2f)", r.yaw, r.pitch,
              lp.mOffset.x * 1e3, lp.mOffset.y * 1e3, lp.mOffset.z * 1e3, r.point.x * 1e3, r.point.y * 1e3, r.point.z * 1e3);
        const double off = angleBetween(turnedAxis(left, lp), r.normal) / DEG;
        CHECK(off < 0.5, "yaw %g pitch %g: the left pupil lies flat on the eye (%.2f deg off the rig's normal)", r.yaw, r.pitch, off);
        // The right eye is the left's mirror image for the mirrored look.
        const PupilPose rp = pupilPose(right, look(-r.yaw, r.pitch), fwd, limit);
        CHECK(near_mm(rp.mOffset, V3(lp.mOffset.x, -lp.mOffset.y, lp.mOffset.z), 1e-6), "yaw %g pitch %g: the right pupil mirrors the left", r.yaw, r.pitch);
        const V3 ra = turnedAxis(right, rp), la = turnedAxis(left, lp);
        CHECK(length(ra - V3(la.x, -la.y, la.z)) < 1e-6, "yaw %g pitch %g: the right pupil's turn mirrors the left's", r.yaw, r.pitch);
    }
    // Sliding without turning was the clipping: at rest the axis is ~3.5 degrees off the eye's
    // normal under the straight-ahead pupil, and 30 degrees to the nose ~8 more.
    CHECK(pupilPose(left, look(0, 0), fwd, limit).mTurn.mAngle > 2.0 * DEG, "the straight look turns the pupil onto the eye");
    const V3 l0 = pupilPose(left, look(0, 0), fwd, limit).mOffset, r0 = pupilPose(right, look(0, 0), fwd, limit).mOffset;
    // Deep inside the shell (looks across toward the nose: the arm's tip lies ~40 mm under the
    // eye, behind the shell's centre plane) the pupil stays on the eye's front where the rig's
    // own nearest point is (its mesh: 30 degrees 22.57, -28.24, -10.80; 45: 25.66, -32.83, -11.33 mm).
    const Ref deep_refs[] = {
        { -30, 0, V3(0.02257, -0.02824, -0.01080), V3(0.8205, 0.5710, -0.0259), 0.3 },
        { -45, 0, V3(0.02566, -0.03283, -0.01133), V3(0.8362, 0.5477, -0.0279), 0.6 },
    };
    for (const Ref& r : deep_refs)
    {
        const PupilPose deep = pupilPose(left, look(r.yaw, r.pitch), fwd, 45 * DEG);
        CHECK(near_mm(deep.mOffset, r.point, r.mm), "%g to the nose: on the eye's front (%.2f, %.2f, %.2f) mm", -r.yaw,
              deep.mOffset.x * 1e3, deep.mOffset.y * 1e3, deep.mOffset.z * 1e3);
        const double off = angleBetween(turnedAxis(left, deep), r.normal) / DEG;
        CHECK(off < 0.5, "%g to the nose: flat on the eye (%.2f deg off the rig's normal)", -r.yaw, off);
    }
    const V3 l30 = pupilPose(left, look(30, 0), fwd, limit).mOffset, r30 = pupilPose(right, look(30, 0), fwd, limit).mOffset;
    CHECK(l30.y > l0.y + 0.01 && r30.y > r0.y + 0.01, "a look left slides both pupils left (conjugate, not mirrored)");
    const V3 lup = pupilPose(left, look(0, 30), fwd, limit).mOffset;
    CHECK(lup.z > l0.z + 0.025, "a look up raises the pupil ~1 mm per degree (%.2f mm)", (lup.z - l0.z) * 1e3);
    // The eye range holds the look.
    CHECK(near_mm(pupilPose(left, look(80, 0), fwd, limit).mOffset, l30, 0.01), "past the range: held at its edge");
    CHECK(pupilPose(left, look(80, 0), fwd, 80 * DEG).mOffset.y > l30.y + 0.01, "a wider range lets it go further");
    // Like the rig: each eye aims at the target moved by its own offset (the eyes' spacing), so
    // a near target between the eyes (bind pupils 18.4 cm apart) never crosses them; the arm's
    // pivot sits 23 mm inward of the pupil, so they drift a little outward as it nears.
    double drift = 0.0;
    for (double d : { 2.0, 0.5, 0.3 })
    {
        const V3 ln = pupilPose(left, V3(d, -0.09209, 0), fwd, limit).mOffset, rn = pupilPose(right, V3(d, 0.09209, 0), fwd, limit).mOffset;
        CHECK(near_mm(rn, V3(ln.x, -ln.y, ln.z), 1e-6), "%.1f m centred: the eyes mirror", d);
        CHECK(ln.y >= l0.y && length(ln - l0) < 0.0045, "%.1f m centred: no cross-eye, a small outward drift (%.2f mm)",
              d, length(ln - l0) * 1e3);
        CHECK(length(ln - l0) >= drift, "%.1f m centred: the drift grows as the target nears", d);
        drift = length(ln - l0);
    }
    // A sphere shell (shell_radius) is the ellipsoid with three equal axes.
    PupilBone sphere = left;
    sphere.mShellCenter = V3(-0.2, -0.2, 0);
    for (int i = 0; i < 3; ++i)
        sphere.mShellAxes[i] = V3(i == 0 ? 0.3 : 0, i == 1 ? 0.3 : 0, i == 2 ? 0.3 : 0);
    const PupilPose sp = pupilPose(sphere, look(10, 5), fwd, limit);
    CHECK(approx(length(sp.mOffset - sphere.mShellCenter), 0.3, 1e-6), "sphere shell: on the sphere");
    CHECK(angleBetween(turnedAxis(sphere, sp), sp.mOffset - sphere.mShellCenter) < 1e-6, "sphere shell: turned to its normal");
    // Linear model: gains along the axes, the ellipse, vergence growing as the target nears.
    PupilBone lin;
    lin.mGainH = 0.0003;
    lin.mGainV = 0.0002;
    lin.mRangeH = 0.006;
    lin.mRangeV = 0.004;
    lin.mVergence = 0.002;
    lin.mInward = -1.0;
    const V3 a = pupilPose(lin, V3(50, 50 * std::tan(10 * DEG), 0), fwd, limit).mOffset;
    CHECK(approx(a.y, 0.003 - 0.002 / length(V3(50, 50 * std::tan(10 * DEG), 0)), 1e-6) && approx(a.z, 0, 1e-9), "linear: 10 deg left = 3 mm along +Y");
    const V3 big = pupilPose(lin, V3(1, 1, 1), fwd, limit).mOffset;
    CHECK(std::hypot(big.y / 0.006, big.z / 0.004) <= 1.0 + 1e-9, "linear: kept inside the ellipse");
    CHECK(pupilPose(lin, V3(1, 1, 1), fwd, limit).mTurn.mAngle == 0.0, "linear: never turned");
    double last = 0.0;
    for (double d : { 5.0, 1.0, 0.5, 0.3 })
    {
        const double inward = -pupilPose(lin, V3(d, 0, 0), fwd, limit).mOffset.y;
        CHECK(inward > last, "linear: vergence grows as the target nears (%.1f m: %.2f mm)", d, inward * 1e3);
        last = inward;
    }
}

static std::vector<unsigned char> animHeader(const char* emote, float ease_in, float ease_out)
{
    std::vector<unsigned char> b;
    auto u16 = [&](unsigned v) { b.push_back(v & 0xff); b.push_back((v >> 8) & 0xff); };
    auto f32 = [&](float f) { unsigned char t[4]; SLAnimHeader::writeF32(t, f); b.insert(b.end(), t, t + 4); };
    auto s32 = [&](int v) { for (int i = 0; i < 4; ++i) b.push_back((unsigned char)((unsigned)v >> (8 * i))); };
    u16(1); u16(0); s32(4); f32(2.5f);
    b.insert(b.end(), emote, emote + std::strlen(emote) + 1);
    f32(0.f); f32(2.5f); s32(1); f32(ease_in); f32(ease_out);
    s32(1); s32(0);                         // hand pose, num joints
    return b;
}

static void testAnimHeader()
{
    for (const char* emote : { "", "express_smile" })
    {
        std::vector<unsigned char> b = animHeader(emote, 0.5f, 0.75f);
        float in = 0.f, out = 0.f;
        CHECK(SLAnimHeader::readEase(b.data(), b.size(), in, out) && in == 0.5f && out == 0.75f,
              "emote '%s': ease read back (%g, %g)", emote, in, out);
        const std::vector<unsigned char> before = b;
        CHECK(SLAnimHeader::writeEase(b.data(), b.size(), 1.25f, 20.f), "emote '%s': ease written", emote);
        CHECK(SLAnimHeader::readEase(b.data(), b.size(), in, out) && in == 1.25f && out == 10.f,
              "emote '%s': new ease (%g, %g), ease-out clamped to 10", emote, in, out);
        size_t at = 0;
        SLAnimHeader::easeOffset(b.data(), b.size(), at);
        bool rest_same = true;
        for (size_t i = 0; i < b.size(); ++i)
            if ((i < at || i >= at + 8) && b[i] != before[i])
                rest_same = false;
        CHECK(rest_same, "emote '%s': only the ease bytes changed", emote);
    }
    std::vector<unsigned char> bad = animHeader("", 0.5f, 0.5f);
    bad[0] = 2;
    float in, out;
    CHECK(!SLAnimHeader::readEase(bad.data(), bad.size(), in, out), "version 2 refused");
    std::vector<unsigned char> cut = animHeader("x", 0.5f, 0.5f);
    cut.resize(20);
    CHECK(!SLAnimHeader::readEase(cut.data(), cut.size(), in, out), "truncated header refused");
    std::vector<unsigned char> noterm(12, 0);
    noterm[0] = 1;
    noterm.insert(noterm.end(), 400, 'a');
    CHECK(!SLAnimHeader::readEase(noterm.data(), noterm.size(), in, out), "unterminated emote refused");
}

int main()
{
    testAnimHeader();
    testTwoBone();
    testNoFlip();
    testPole();
    testFabrik();
    testReach();
    testGaze();
    testEyes();
    testFollow();
    testRiggedJoints();
    testLookAtGaze();
    testPupil();
    if (g_fail)
    {
        std::printf("%d pose IK fixture(s) failed\n", g_fail);
        return 1;
    }
    std::printf("pose IK fixtures passed\n");
    return 0;
}
