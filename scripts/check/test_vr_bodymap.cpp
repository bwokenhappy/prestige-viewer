// Fixtures for the VR body mapping (#1592; indra/newview/llvrbodymapmath.h), run by
// scripts/check/checks/vr_bodymap.py (compiled with the host C++ compiler, no viewer build).
//   HUMAN     the human profile's frames are identity: the solver's locals reach the joints
//             unchanged (today's behaviour).
//   PONY      the Open Pony's joints (dev kit V3.2 weights): forelegs on mCollar / mShoulder /
//             mElbow / mWrist, the hoof (mHandRing1) never finger-tracked; hind legs on mHip /
//             mKnee / mAnkle; the three modes' role states.
//   FK        retargeting is exact: FK of the actual skeleton with the converted locals puts every
//             joint where the virtual (human-like) skeleton's FK does, for random poses.
//   UPRIGHT   the bipedal stance: the barrel stands up, the head faces forward on top, the
//             forelegs hang as arms, the hind legs hang under the hips; its eye height is the
//             upright one (higher than on four legs).
//   ARMS      a quadruped's forelegs reach as T-pose arms (virtual offsets along +-Y), mirrored.
//   DETECT    Open Pony rigging, Bento human, classic human, a horizontal spine, Bento hind limbs,
//             nothing loaded yet.
//   TEXT      export / import round trips; edits survive; bad lines are skipped; no header fails.
//   REACH     your shoulder under the eye; your reach scaled to the limb's from its own shoulder.
//   LEG       the Open Pony's foreleg as an arm (arms only and bipedal, both sides): hanging comes
//             out at rest; reaching, the hoof joint lands on the target and the hoof turns as the
//             controller (the hand), the cannon on the forearm as it rests, the elbow back, the
//             collar unturned; a rolled hand rolls the hoof, the forearm and cannon only a
//             forearm's share; never stretched.
//   SETTLE    a pony profile with its spine tracked stands up and drives its hips and hind legs;
//             back on four legs undoes it; human / arms / head only untouched.
//   EYES      the VR eye point: the Open Pony's pupils (0.14 above its head joint, not 0.06), a
//             human's eye joints, fallbacks; on the middle.
#include "llvrbodyarmmath.h"
#include "llvrbodymapmath.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace slbmap;
using slfbt::length;
using slfbt::normalized;
using slfbt::qaxis;
using slfbt::qmul;
using slfbt::qrot;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static bool nearV(const V3& a, const V3& b, float eps) { return length(a - b) <= eps; }
static bool nearQ(const Q& a, const Q& b, float eps) { return slfbt::qangle(a, b) <= eps; }

// ---- Rigs ---------------------------------------------------------------------------------------
// The Open Pony's rest offsets (SL axes, from the dev kit's bone heads: SL = (-y, x, z) of Blender).
// Left side; the right is mirrored (Y -> -Y).
static RestRig ponyRig()
{
    RestRig r;
    auto set = [&](int role, float x, float y, float z) {
        r.have[role] = true;
        r.off[role] = V3(x, y, z);
    };
    set(R_PELVIS, 0.f, 0.f, 0.f);
    set(R_SPINE, 0.07666f, 0.f, 0.00021f);
    set(R_CHEST, 0.11869f, 0.f, -0.00025f);
    set(R_NECK, 0.16423f, 0.f, 0.0363f);
    set(R_HEAD, -0.00054f, 0.f, 0.16232f);
    for (int side = 0; side < 2; ++side)
    {
        const float s = side ? -1.f : 1.f;
        const int c = side ? R_R_COLLAR : R_L_COLLAR;
        set(c, 0.1513f, s * 0.1039f, 0.02846f);
        set(c + 1, -0.00837f, s * -0.00004f, -0.07848f);
        set(c + 2, -0.01556f, s * -0.01997f, -0.13425f);
        set(c + 3, 0.00622f, s * -0.00028f, -0.13175f);
        const int t = side ? R_R_THIGH : R_L_THIGH;
        set(t, -0.00092f, s * 0.08907f, 0.02958f);
        set(t + 1, -0.01704f, s * 0.00078f, -0.14459f);
        set(t + 2, -0.08625f, s * 0.00137f, -0.06048f);
        set(hoofOf(side), 0.00535f, s * -0.00043f, -0.15157f);   // (mHandRing1 under mWrist: the cannon)
    }
    return r;
}
// The front hoof (mHandRing1 under mWrist) and the hind cannon + hoof (mFoot under mAnkle; its sole).
static const V3 kPonyHoof(0.00535f, -0.00043f, -0.15157f);
static const V3 kPonyCannon(-0.02636f, 0.f, -0.29536f);
static const V3 kPonySole(0.01642f, 0.f, -0.09582f);
static const float kPonyPelvisH = 0.57357f;

// The SL default skeleton (avatar_skeleton.xml), roughly.
static RestRig humanRig()
{
    RestRig r;
    auto set = [&](int role, float x, float y, float z) {
        r.have[role] = true;
        r.off[role] = V3(x, y, z);
    };
    set(R_PELVIS, 0.f, 0.f, 0.f);
    set(R_SPINE, 0.f, 0.f, 0.084f);
    set(R_CHEST, -0.015f, 0.f, 0.205f);
    set(R_NECK, -0.01f, 0.f, 0.251f);
    set(R_HEAD, 0.f, 0.f, 0.076f);
    for (int side = 0; side < 2; ++side)
    {
        const float s = side ? -1.f : 1.f;
        const int c = side ? R_R_COLLAR : R_L_COLLAR;
        set(c, -0.021f, s * 0.085f, 0.165f);
        set(c + 1, 0.f, s * 0.079f, 0.f);
        set(c + 2, 0.f, s * 0.248f, 0.f);
        set(c + 3, 0.f, s * 0.205f, 0.f);
        const int t = side ? R_R_THIGH : R_L_THIGH;
        set(t, 0.034f, s * 0.127f, -0.041f);
        set(t + 1, -0.001f, s * -0.046f, -0.491f);
        set(t + 2, -0.029f, s * 0.001f, -0.468f);
    }
    return r;
}

// ---- FK over the role tree ------------------------------------------------------------------------
struct Pose
{
    V3 pos[R_COUNT];
    Q world[R_COUNT];
};
// locals: per role, under its chain parent (the root: identity at the origin).
static Pose fk(const RestRig& rig, const Q* locals, const Q* rParent /* nullptr: actual offsets */)
{
    Pose p;
    for (int r = 0; r < R_COUNT; ++r)   // (roles are listed parents first)
    {
        if (!rig.have[r] || isFingers(r)) continue;
        const int pr = roleInfo(r).parent;
        const Q pw = pr < 0 ? Q() : p.world[pr];
        const V3 pp = pr < 0 ? V3() : p.pos[pr];
        const V3 off = rParent ? virtualOffset(rParent[r], rig.off[r]) : rig.off[r];
        p.pos[r] = pp + qrot(pw, off);
        p.world[r] = slfbt::qnormalized(qmul(pw, locals[r]));
    }
    return p;
}
static float frand() { return (float)std::rand() / (float)RAND_MAX * 2.f - 1.f; }
static Q randQ(float maxAngle)
{
    return qaxis(normalized(V3(frand(), frand(), frand()), V3(0.f, 0.f, 1.f)), frand() * maxAngle);
}

// Checks the retarget identity on a rig and a profile's frames.
static void checkFK(const char* name, const RestRig& rig, const Frames& f)
{
    Q parentR[R_COUNT];
    for (int r = 0; r < R_COUNT; ++r)
    {
        const int pr = roleInfo(r).parent;
        parentR[r] = pr < 0 ? Q() : f.R[pr];
    }
    for (int trial = 0; trial < 50; ++trial)
    {
        Q vl[R_COUNT], al[R_COUNT];
        for (int r = 0; r < R_COUNT; ++r)
        {
            vl[r] = randQ(1.2f);
            al[r] = actualLocal(parentR[r], f.R[r], vl[r]);
        }
        const Pose v = fk(rig, vl, parentR);
        const Pose a = fk(rig, al, nullptr);
        for (int r = 0; r < R_COUNT; ++r)
        {
            if (!rig.have[r] || isFingers(r)) continue;
            CHECK(nearV(v.pos[r], a.pos[r], 1e-4f), "%s FK: %s at (%.4f %.4f %.4f) vs virtual (%.4f %.4f %.4f)", name, roleInfo(r).key,
                  a.pos[r].x, a.pos[r].y, a.pos[r].z, v.pos[r].x, v.pos[r].y, v.pos[r].z);
            // actual world = virtual world * R (5 mrad: qangle's acos near 1 in floats)
            CHECK(nearQ(a.world[r], qmul(v.world[r], f.R[r]), 5e-3f), "%s FK: %s world rotation", name, roleInfo(r).key);
        }
    }
}

static void testHuman()
{
    const Profile p = humanProfile();
    Frames f;
    computeFrames(p, humanRig(), f);
    CHECK(!f.upright && f.identity, "HUMAN: not upright, identity");
    for (int r = 0; r < R_COUNT; ++r)
    {
        CHECK(nearQ(f.R[r], Q(), 1e-5f), "HUMAN: %s frame not identity", roleInfo(r).key);
        CHECK(p.state[r] == S_TRACKED, "HUMAN: %s tracked", roleInfo(r).key);
        CHECK(p.joint[r] == roleInfo(r).human, "HUMAN: %s joint", roleInfo(r).key);
    }
    const Q l = randQ(1.f);
    CHECK(nearQ(actualLocal(f.R[R_L_COLLAR], f.R[R_L_UPPER_ARM], l), l, 1e-5f), "HUMAN: locals pass unchanged");
    checkFK("human", humanRig(), f);
}

static void testPonyProfiles()
{
    const Profile b = ponyBipedal(), a = ponyArms(), h = ponyHead();
    for (const Profile* p : {&b, &a, &h})
    {
        CHECK(p->joint[R_L_HAND] == "mWristLeft" && p->joint[R_R_UPPER_ARM] == "mShoulderRight", "PONY %s: forelegs on the arm chain", p->id.c_str());
        CHECK(p->joint[R_L_FOOT] == "mAnkleLeft" && p->joint[R_R_THIGH] == "mHipRight", "PONY %s: hind legs on the leg chain", p->id.c_str());
        CHECK(p->state[R_L_FINGERS] == S_ANIMATION && p->state[R_R_FINGERS] == S_ANIMATION, "PONY %s: the hoof (mHandRing1) is never finger-tracked", p->id.c_str());
        CHECK(p->joint[R_L_HOOF] == "mHandRing1Left" && p->joint[R_R_HOOF] == "mHandRing1Right", "PONY %s: the hoof role on mHandRing1", p->id.c_str());
    }
    CHECK(b.tracked(R_L_HOOF) && a.tracked(R_R_HOOF) && h.state[R_L_HOOF] == S_ANIMATION, "PONY: the hoof held with the arm");
    CHECK(b.reachRebase, "PONY bipedal: the forelegs reach from the pony's shoulders, scaled");
    CHECK(!humanProfile().enabled(R_L_HOOF) && !humanProfile().enabled(R_R_HOOF), "HUMAN: no hoof");
    CHECK(b.upright && !a.upright && !h.upright, "PONY: only bipedal stands up");
    for (int r : {R_PELVIS, R_SPINE, R_CHEST, R_NECK, R_HEAD, R_L_HAND, R_R_FOREARM, R_L_THIGH, R_R_FOOT})
        CHECK(b.tracked(r), "PONY bipedal: %s tracked", roleInfo(r).key);
    for (int r : {R_PELVIS, R_SPINE, R_CHEST, R_L_THIGH, R_L_SHIN, R_L_FOOT, R_R_THIGH, R_R_FOOT})
        CHECK(a.state[r] == S_ANIMATION, "PONY arms only: %s on the animation", roleInfo(r).key);
    for (int r : {R_NECK, R_HEAD, R_L_COLLAR, R_L_UPPER_ARM, R_L_FOREARM, R_L_HAND, R_R_HAND})
        CHECK(a.tracked(r), "PONY arms only: %s tracked", roleInfo(r).key);
    CHECK(a.reachRebase && !a.neckLean, "PONY arms only: reach from its shoulders, no neck lean");
    for (int r = 0; r < R_COUNT; ++r)
        CHECK((r == R_NECK || r == R_HEAD) ? h.tracked(r) : h.state[r] == S_ANIMATION, "PONY head only: %s", roleInfo(r).key);
}

static void testUpright()
{
    const RestRig rig = ponyRig();
    Frames f;
    computeFrames(ponyBipedal(), rig, f);
    CHECK(f.upright && !f.identity, "UPRIGHT: frames upright");
    // The pitch puts the rest pelvis -> neck line on +Z.
    const V3 pn = rig.off[R_SPINE] + rig.off[R_CHEST] + rig.off[R_NECK];
    const V3 up = qrot(f.pitch, pn);
    CHECK(std::fabs(up.x) < 1e-4f && up.z > 0.f, "UPRIGHT: pelvis -> neck (%.3f %.3f %.3f) not vertical", up.x, up.y, up.z);
    const float deg = slfbt::qangle(Q(), f.pitch) * 57.29578f;
    CHECK(deg > 80.f && deg < 90.f, "UPRIGHT: pitch %.1f deg (the pony's neck rises a little from the chest's front)", deg);
    CHECK(qrot(f.pitch, V3(1.f, 0.f, 0.f)).z > 0.99f, "UPRIGHT: nose up (forward -> up)");
    checkFK("pony bipedal", rig, f);

    // The stance: base locals through FK.
    Q bl[R_COUNT];
    for (int r = 0; r < R_COUNT; ++r) bl[r] = baseLocal(f, r);
    CHECK(nearQ(bl[R_PELVIS], f.pitch, 1e-4f), "UPRIGHT: pelvis local = the pitch");
    CHECK(nearQ(bl[R_NECK], slfbt::qconj(f.pitch), 1e-4f), "UPRIGHT: neck local = the pitch undone");
    CHECK(nearQ(bl[R_L_UPPER_ARM], slfbt::qconj(f.pitch), 1e-4f) && nearQ(bl[R_R_UPPER_ARM], slfbt::qconj(f.pitch), 1e-4f) &&
              nearQ(bl[R_R_THIGH], slfbt::qconj(f.pitch), 1e-4f),
          "UPRIGHT: upper arms and hips undo the pitch");
    // The collars (the scapulae, weighted L_CLAVICLE) ride with the barrel: no shoulder twisted
    // against the body.
    for (int r : {R_SPINE, R_CHEST, R_HEAD, R_L_COLLAR, R_R_COLLAR, R_L_FOREARM, R_L_HAND, R_L_HOOF, R_R_HOOF, R_L_SHIN, R_L_FOOT})
        CHECK(nearQ(bl[r], Q(), 1e-4f), "UPRIGHT: %s local identity", roleInfo(r).key);
    const Pose p = fk(rig, bl, nullptr);
    const V3 barrel = normalized(p.pos[R_NECK] - p.pos[R_PELVIS]);
    CHECK(barrel.z > 0.999f, "UPRIGHT: the barrel stands (%.3f %.3f %.3f)", barrel.x, barrel.y, barrel.z);
    CHECK(nearQ(p.world[R_HEAD], Q(), 1e-4f), "UPRIGHT: the head faces forward, level");
    for (int side = 0; side < 2; ++side)
    {
        const int c = side ? R_R_COLLAR : R_L_COLLAR;
        const V3 leg = normalized(p.pos[c + 3] - p.pos[c + 1]);
        CHECK(leg.z < -0.95f, "UPRIGHT: foreleg %d hangs (%.2f %.2f %.2f)", side, leg.x, leg.y, leg.z);
        CHECK((side ? -1.f : 1.f) * (p.pos[c + 1].y - p.pos[R_PELVIS].y) > 0.05f, "UPRIGHT: foreleg %d on its side", side);
        const int t = side ? R_R_THIGH : R_L_THIGH;
        CHECK(p.pos[t + 2].z < p.pos[t + 1].z && p.pos[t + 1].z < p.pos[t].z, "UPRIGHT: hind leg %d hangs under the hips", side);
        CHECK(std::fabs(p.pos[t + 2].x - p.pos[R_PELVIS].x) < 0.15f, "UPRIGHT: hind foot %d under the body", side);
    }
    // Eye heights: on four legs and standing (the LLVRBody measure: pelvis height + pelvis ->
    // neck + neck -> head + the eye point, the pony's pupils 14 cm above the head joint). The foot
    // joint keeps its height above the floor.
    const float ankleUp = kPonyPelvisH + (rig.off[R_L_THIGH] + rig.off[R_L_SHIN] + rig.off[R_L_FOOT]).z;
    const float quadEye = kPonyPelvisH + pn.z + rig.off[R_HEAD].z + 0.14f;
    const float pelvisUp = ankleUp - (p.pos[R_L_FOOT] - p.pos[R_PELVIS]).z;
    const float upEye = pelvisUp + (p.pos[R_HEAD] - p.pos[R_PELVIS]).z + 0.14f;
    CHECK(upEye > quadEye * 1.3f, "UPRIGHT: eye %.2f standing vs %.2f on four legs", upEye, quadEye);
    CHECK(std::fabs(ankleUp + kPonyCannon.z + kPonySole.z) < 0.02f, "UPRIGHT: the dev kit's hind hoof reaches the floor (%.3f)",
          ankleUp + kPonyCannon.z + kPonySole.z);
    std::printf("upright pony: pitch %.1f deg, eye %.3f standing, %.3f on four legs, pelvis %.3f / %.3f\n", deg, upEye, quadEye,
                pelvisUp, kPonyPelvisH);
    (void)kPonyHoof;
}

static void testArms()
{
    const RestRig rig = ponyRig();
    Frames f;
    computeFrames(ponyArms(), rig, f);
    CHECK(!f.upright, "ARMS: not upright");
    CHECK(nearQ(f.R[R_CHEST], Q(), 1e-5f), "ARMS: the barrel as it is");
    checkFK("pony arms", rig, f);
    for (int side = 0; side < 2; ++side)
    {
        const float s = side ? -1.f : 1.f;
        const int c = side ? R_R_COLLAR : R_L_COLLAR;
        // (From the shoulder on: the elbow, the wrist and -- the hand's bone being the cannon --
        // the hoof. The collar -> shoulder offset stays as it rests: the collar is not turned.)
        for (int r : {c + 2, c + 3, hoofOf(side)})
        {
            const V3 v = normalized(virtualOffset(f.R[roleInfo(r).parent], rig.off[r]));
            CHECK(v.y * s > 0.95f, "ARMS: %s reaches sideways in the virtual skeleton (%.2f %.2f %.2f)", roleInfo(r).key, v.x, v.y, v.z);
        }
        CHECK(f.armRetarget[side] && nearQ(f.R[c], f.R[R_CHEST], 1e-6f), "ARMS: the retargeted foreleg's collar keeps the chest's frame");
        CHECK(nearQ(f.R[c + 4], f.R[c + 3], 1e-6f) && nearQ(f.R[hoofOf(side)], f.R[c + 3], 1e-6f), "ARMS: fingers / hoof take the hand's frame");
        // About the forward axis: the hoof's front stays forward.
        const V3 fwd = qrot(f.R[c + 2], V3(1.f, 0.f, 0.f));
        CHECK(fwd.x > 0.95f, "ARMS: the foreleg turned about the forward axis");
    }
    // Mirror: the right arm's frame is the left's mirrored.
    const Q l = f.R[R_L_UPPER_ARM], r = f.R[R_R_UPPER_ARM];
    CHECK(nearQ(Q(-l.x, l.y, -l.z, l.w), r, 0.05f), "ARMS: right frame mirrors the left");
    // Base (off): the forelegs hang as they rest.
    Q bl[R_COUNT];
    for (int k = 0; k < R_COUNT; ++k) bl[k] = baseLocal(f, k);
    const Pose p = fk(rig, bl, nullptr);
    CHECK(normalized(p.pos[R_L_HAND] - p.pos[R_L_UPPER_ARM]).z < -0.95f, "ARMS: off holds the foreleg hanging");
    for (int r = 0; r < R_COUNT; ++r)
        if (!isFingers(r)) CHECK(nearQ(bl[r], Q(), 1e-4f), "ARMS: off is the rest pose (%s)", roleInfo(r).key);
    // A human keeps its collars turning (no retarget).
    Frames hf;
    computeFrames(humanProfile(), humanRig(), hf);
    CHECK(!hf.armRetarget[0] && !hf.armRetarget[1], "ARMS: a human arm is not retargeted");
}

static RigFacts openPonyFacts()
{
    RigFacts f;
    f.riggedCount = 53;
    f.ring1[0] = f.ring1[1] = true;
    f.foot[0] = f.foot[1] = true;
    f.pelvisToNeck = V3(0.3596f, 0.f, 0.0363f);
    return f;
}
static void testDetect()
{
    std::string why;
    CHECK(detect(openPonyFacts(), &why) == D_OPEN_PONY, "DETECT: Open Pony (%s)", why.c_str());
    CHECK(std::string(autoProfileFor(D_OPEN_PONY)) == "pony_arms", "DETECT: Auto for the Open Pony");
    RigFacts f = openPonyFacts();
    f.pelvisToNeck = V3(0.f, 0.f, 0.5f);   // (joint positions not applied yet: the rigging alone)
    CHECK(detect(f) == D_OPEN_PONY, "DETECT: Open Pony by rigging alone");
    // A Bento human hand: every finger.
    RigFacts h = openPonyFacts();
    h.otherFingers[0] = h.otherFingers[1] = true;
    h.pelvisToNeck = V3(-0.025f, 0.f, 0.54f);
    CHECK(detect(h, &why) == D_HUMAN, "DETECT: Bento human (%s)", why.c_str());
    // A classic human body: no fingers, no feet weighted beyond the legs.
    RigFacts c;
    c.riggedCount = 20;
    c.pelvisToNeck = V3(-0.025f, 0.f, 0.54f);
    CHECK(detect(c) == D_HUMAN, "DETECT: classic human");
    // One hoof only (a human with a ring on one finger bone): human.
    RigFacts one = h;
    one.otherFingers[0] = one.otherFingers[1] = false;
    one.ring1[1] = false;
    CHECK(detect(one) == D_HUMAN, "DETECT: one ring bone is not a pony");
    // Another quadruped: horizontal spine, or Bento hind limbs.
    RigFacts q = c;
    q.pelvisToNeck = V3(0.4f, 0.f, 0.1f);
    CHECK(detect(q) == D_QUADRUPED && std::string(autoProfileFor(D_QUADRUPED)) == "pony_head", "DETECT: horizontal spine");
    RigFacts hl = c;
    hl.hindLimbs = true;
    CHECK(detect(hl) == D_QUADRUPED, "DETECT: Bento hind limbs");
    RigFacts none;
    none.pelvisToNeck = V3(0.f, 0.f, 0.5f);
    CHECK(detect(none, &why) == D_HUMAN, "DETECT: nothing loaded (%s)", why.c_str());
    CHECK(horizontalBody(V3(0.36f, 0.f, 0.04f)) && !horizontalBody(V3(0.05f, 0.f, 0.5f)), "DETECT: horizontalBody");
}

static void testText()
{
    for (const char* id : {"human", "pony_bipedal", "pony_arms", "pony_head"})
    {
        Profile p, back;
        CHECK(builtinProfile(id, p), "TEXT: built-in %s", id);
        CHECK(fromText(toText(p), back), "TEXT: %s reads back", id);
        CHECK(sameMapping(p, back) && back.id == id && back.custom, "TEXT: %s round trip", id);
    }
    Profile p = ponyBipedal();
    p.joint[R_L_HAND] = "mHandRing1Left";
    p.state[R_R_FOREARM] = S_OFF;
    p.joint[R_HEAD].clear();
    p.weight[P_SPINE] = 0.25f;
    Profile back;
    CHECK(fromText(toText(p), back) && sameMapping(p, back), "TEXT: edits survive");
    CHECK(back.joint[R_HEAD].empty() && back.joint[R_L_HAND] == "mHandRing1Left" && back.state[R_R_FOREARM] == S_OFF, "TEXT: edit values");
    int skipped = 0;
    const std::string messy = "# comment\r\nslipstream-body-map 1\r\nprofile pony_arms\nrole l_hand m Bad;Name tracked\nrole nosuch mPelvis tracked\n"
                              "role pelvis mPelvis sideways\nweight hips 7\nweight spine nan\nwhatever 1\nrole l_fingers mHandIndex1Left tracked\n"
                              "role neck mNeck off   # trailing comment\n";
    CHECK(fromText(messy, back, &skipped), "TEXT: messy file reads");
    CHECK(skipped == 6, "TEXT: %d lines skipped (6 expected)", skipped);
    CHECK(back.id == "pony_arms" && back.state[R_NECK] == S_OFF && back.weight[P_HIPS] == 1.f, "TEXT: messy values");
    CHECK(back.joint[R_L_FINGERS] == "fingers", "TEXT: fingers cannot be remapped");
    CHECK(!fromText("role pelvis mPelvis tracked\n", back), "TEXT: no header -> not a body map");
    CHECK(validJointName("mHandRing1Left") && !validJointName("") && !validJointName("a b") && !validJointName(std::string(41, 'm')),
          "TEXT: joint names");
}

static void testReach()
{
    const V3 eye(1.f, 2.f, 1.5f);
    const float eyeH = 1.6f;
    const V3 yours = humanShoulder(eye, 0.5f, eyeH, 1.f);
    CHECK(nearV(yours, eye + qrot(slfbt::qyaw(0.5f), V3(-0.06f, 0.11f, -0.17f) * eyeH), 1e-5f), "REACH: your left shoulder");
    CHECK(humanShoulder(eye, 0.f, eyeH, -1.f).y < eye.y, "REACH: the right shoulder on the right");
    // A limb as long as your arm: unscaled; from your own shoulder: unmoved.
    CHECK(std::fabs(reachScale(kHumanReach * eyeH, eyeH) - 1.f) < 1e-5f, "REACH: your own reach scales by 1");
    const V3 hand = yours + V3(0.3f, 0.1f, -0.2f);
    CHECK(nearV(remapReach(hand, yours, yours, 1.f), hand, 1e-6f), "REACH: a human arm is untouched");
    // The Open Pony on four legs (eye 0.83, shoulders ~0.3 below and 0.14 behind the eye), foreleg
    // shoulder -> hoof 0.42: your arm stretched forward is the foreleg stretched forward.
    const float ponyEyeH = 0.83f;
    const V3 ponyEye(0.f, 0.f, 0.83f), ponyShoulder(-0.14f, 0.10f, 0.52f);
    const float k = reachScale(0.42f, ponyEyeH);
    CHECK(k > 1.f && k < 1.5f, "REACH: the pony's foreleg is long for its eye height (%.2f)", k);
    const V3 you = humanShoulder(ponyEye, 0.f, ponyEyeH, 1.f);
    const V3 outFwd = remapReach(you + V3(kHumanReach * ponyEyeH, 0.f, 0.f), you, ponyShoulder, k);
    CHECK(nearV(outFwd, ponyShoulder + V3(0.42f, 0.f, 0.f), 1e-4f), "REACH: arm out -> foreleg out (%.3f %.3f %.3f)", outFwd.x, outFwd.y,
          outFwd.z);
    CHECK(reachScale(10.f, 0.5f) == 2.5f && reachScale(0.01f, 2.f) == 0.3f && reachScale(0.4f, 0.f) == 1.f, "REACH: scale kept in range");
}

// The Open Pony's foreleg driven as an arm (the arms layer's solve, in the pure math): the hoof
// lands on the remapped controller point turned as the controller, the forearm and cannon twist a
// forearm's share of its roll, the collar stays on the body, and bones keep their rest lengths.
// Returns the actual FK of the limb.
struct LimbFK
{
    V3 collar, shoulder, elbow, wrist, hoof;
    Q collarLocal, hoofWorld, handWorld;
    Q local[5];   // the actual locals: collar, upper arm, forearm, cannon, hoof
    float forearmTwist = 0.f, cannonTwist = 0.f;
    bool reached = false;
};
static LimbFK solvePonyLeg(const RestRig& rig, const Frames& f, int side, const V3& chestP, const V3& hoofT, const Q& handQ,
                           float* twistState = nullptr)
{
    const int c = side ? R_R_COLLAR : R_L_COLLAR, hf = hoofOf(side);
    slfbt::ArmRest r;
    r.side = side ? -1.f : 1.f;
    r.collar = virtualOffset(f.R[R_CHEST], rig.off[c]);
    r.shoulder = virtualOffset(f.R[c], rig.off[c + 1]);
    r.elbow = virtualOffset(f.R[c + 1], rig.off[c + 2]);
    r.wrist = virtualOffset(f.R[c + 2], rig.off[c + 3]);
    const V3 cannon = virtualOffset(f.R[c + 3], rig.off[hf]);
    const Q cannonRest = slfbt::qnormalized(qmul(f.R[c + 2], slfbt::qconj(f.R[c + 3])));   // (as the arms layer)
    const Q cQ;   // (the chest level: its virtual world rotation)
    const Q collarL;   // (held: a retargeted collar is not turned)
    float twist = 0.f;
    const slfbt::ArmParams prm;
    const slfbt::HoofParams hpm;
    const V3 shoulderP = chestP + qrot(cQ, r.collar) + qrot(cQ, r.shoulder);
    const float reach = length(r.elbow) + length(r.wrist + qrot(cannonRest, cannon));
    const V3 pole = slfbt::forelegPole(shoulderP, hoofT, cQ, reach);   // (the arms layer's for a retargeted limb)
    const slfbt::HoofPose hp = slfbt::solveHoofArm(chestP, cQ, r, cannon, cannonRest, hoofT, handQ, &pole, prm, hpm, &collarL,
                                                   twistState ? twistState : &twist);
    const slfbt::ArmPose& p = hp.arm;
    CHECK(p.ok, "LEG: solved");
    // Virtual locals -> actual locals -> FK with the actual (rest) offsets.
    const Q vl[5] = {qmul(slfbt::qconj(cQ), p.collarWorld), qmul(slfbt::qconj(p.collarWorld), p.shoulderWorld),
                     qmul(slfbt::qconj(p.shoulderWorld), p.elbowWorld), qmul(slfbt::qconj(p.elbowWorld), p.wristWorld),
                     qmul(slfbt::qconj(p.wristWorld), hp.hoofWorld)};
    const int roles[5] = {c, c + 1, c + 2, c + 3, hf};
    LimbFK out;
    Q w = qmul(cQ, f.R[R_CHEST]);   // the chest's actual world rotation
    V3 pos = chestP;
    V3* at[5] = {&out.collar, &out.shoulder, &out.elbow, &out.wrist, &out.hoof};
    for (int i = 0; i < 5; ++i)
    {
        const int pr = roleInfo(roles[i]).parent;
        pos = pos + qrot(w, rig.off[roles[i]]);
        const Q al = actualLocal(f.R[pr], f.R[roles[i]], vl[i]);
        w = slfbt::qnormalized(qmul(w, al));
        *at[i] = pos;
        out.local[i] = al;
        if (i == 0) out.collarLocal = al;
        if (i == 3) out.handWorld = w;
        if (i == 4) out.hoofWorld = w;
    }
    out.reached = p.reached;
    out.forearmTwist = hp.forearmTwist;
    out.cannonTwist = hp.cannonTwist;
    return out;
}
// A local rotation's roll about a bone's axis (radians) and its swing (the rest: radians).
static float rollAbout(const Q& local, const V3& axis, float* swingAngle = nullptr)
{
    Q sw, tw;
    slfbt::swingTwist(local, normalized(axis), sw, tw);
    if (swingAngle) *swingAngle = slfbt::qangle(Q(), sw);
    return slfbt::twistAngle(tw, normalized(axis));
}

static void testPonyArmChain()
{
    const RestRig rig = ponyRig();
    const V3 chestP(0.19535f, 0.f, kPonyPelvisH);   // (pelvis -> torso -> chest, on four legs)
    for (const Profile& prof : {ponyArms(), ponyBipedal()})
    {
        Frames f;
        computeFrames(prof, rig, f);
        for (int side = 0; side < 2; ++side)
        {
            const float s = side ? -1.f : 1.f;
            const int c = side ? R_R_COLLAR : R_L_COLLAR, hf = hoofOf(side);
            const V3 collarP = chestP + qrot(f.R[R_CHEST], rig.off[c]);
            const V3 shoulderP = collarP + qrot(f.R[R_CHEST], rig.off[c + 1]);
            // 1. Hanging (in the world, as it rests; under the upright barrel too): the hoof where
            //    it rests, the controller turned as the resting cannon (its virtual world =
            //    conj(R)): the foreleg comes out (nearly) at rest.
            {
                const V3 restHoof = shoulderP + rig.off[c + 2] + rig.off[c + 3] + rig.off[hf];
                const LimbFK k = solvePonyLeg(rig, f, side, chestP, restHoof, slfbt::qconj(f.R[c + 3]));
                CHECK(nearV(k.hoof, restHoof, 2e-3f), "LEG %s %d hanging: hoof %.4f m off", prof.id.c_str(), side, length(k.hoof - restHoof));
                CHECK(nearQ(k.collarLocal, Q(), 1e-4f), "LEG %s %d: the collar stays on the body", prof.id.c_str(), side);
                CHECK(nearV(k.shoulder, shoulderP, 1e-4f), "LEG %s %d: the shoulder joint where it rests", prof.id.c_str(), side);
                CHECK(nearQ(k.hoofWorld, Q(), 1e-3f), "LEG %s %d hanging: the hoof's rotation as at rest", prof.id.c_str(), side);
                // Every joint near its rest (the upright upper arm: the barrel's pitch undone). Within
                // 12 degrees: the solver bends the elbow straight back where the rest foreleg bends a
                // little back and in (a few degrees at each joint); no roll about any bone.
                for (int i = 2; i < 5; ++i)
                    CHECK(nearQ(k.local[i], Q(), 0.2f), "LEG %s %d hanging: joint %d bent %.2f rad", prof.id.c_str(), side, i,
                          slfbt::qangle(Q(), k.local[i]));
                CHECK(nearQ(k.local[1], slfbt::qconj(f.pitch), 0.2f), "LEG %s %d hanging: upper arm %.2f rad from rest", prof.id.c_str(),
                      side, slfbt::qangle(slfbt::qconj(f.pitch), k.local[1]));
                if (!f.upright)
                    for (int i = 1; i < 4; ++i)
                    {
                        // No roll about the bone's own axis (the "twisted leg").
                        const V3 axis = normalized(rig.off[c + i + 1 <= c + 3 ? c + i + 1 : hf]);
                        Q sw, tw;
                        slfbt::swingTwist(k.local[i], axis, sw, tw);
                        CHECK(std::fabs(slfbt::twistAngle(tw, axis)) < 0.05f, "LEG %s %d hanging: joint %d rolled %.2f rad", prof.id.c_str(), side,
                              i, slfbt::twistAngle(tw, axis));
                    }
                const V3 leg = normalized(k.hoof - k.shoulder);
                CHECK(nearV(leg, normalized(restHoof - shoulderP), 0.03f), "LEG %s %d hanging: the leg as it rests (%.2f %.2f %.2f)",
                      prof.id.c_str(), side, leg.x, leg.y, leg.z);
            }
            // 2. Reaching forward and up, the controller pointing forward (fingers forward,
            //    thumb up): the hoof joint on the target and the hoof turned exactly as the
            //    controller (retargeted) -- the hoof is the hand, bent at its joint; the cannon
            //    stays on the forearm as it rests (#1592: it was the cannon that took the turn).
            {
                const V3 target = shoulderP + V3(0.25f, s * 0.06f, 0.12f);
                const Q handQ = slfbt::qfromBasis(V3(0.f, 0.f, 1.f), V3(s, 0.f, 0.f), V3(0.f, s, 0.f));   // (gripHandBasis)
                const LimbFK k = solvePonyLeg(rig, f, side, chestP, target, handQ);
                CHECK(k.reached && nearV(k.hoof, target, 2e-3f), "LEG %s %d forward: hoof %.4f m off the target", prof.id.c_str(), side,
                      length(k.hoof - target));
                CHECK(nearQ(k.hoofWorld, qmul(handQ, f.R[hf]), 2e-3f), "LEG %s %d forward: the hoof turned as the controller", prof.id.c_str(), side);
                const V3 hoofDir = qrot(k.hoofWorld, normalized(rig.off[hf]));
                CHECK(hoofDir.x > 0.98f, "LEG %s %d forward: the hoof points where the controller's fingers do (%.2f %.2f %.2f)",
                      prof.id.c_str(), side, hoofDir.x, hoofDir.y, hoofDir.z);
                float cannonSwing = 0.f;
                const float cannonRoll = rollAbout(k.local[3], rig.off[hf], &cannonSwing);
                CHECK(cannonSwing < 0.02f, "LEG %s %d forward: the cannon keeps its rest bend on the forearm (%.3f rad off)", prof.id.c_str(),
                      side, cannonSwing);
                const float forearmRoll = rollAbout(k.local[2], rig.off[c + 3]);
                CHECK(std::fabs(forearmRoll) <= 0.75f && std::fabs(forearmRoll + cannonRoll) <= 1.45f,
                      "LEG %s %d forward: forearm %.2f / cannon %.2f rad of roll (a forearm's share)", prof.id.c_str(), side, forearmRoll,
                      forearmRoll + cannonRoll);
                CHECK(nearV(qrot(k.hoofWorld, V3(1.f, 0.f, 0.f)), V3(0.f, 0.f, 1.f), 0.05f),
                      "LEG %s %d forward: the hoof's front faces up (the thumb side)", prof.id.c_str(), side);
                // Rest lengths kept (FK on the actual offsets: the bones are rigid); the elbow
                // points back.
                CHECK(k.elbow.x < (k.shoulder.x + k.wrist.x) * 0.5f + 0.01f, "LEG %s %d forward: the elbow bends back", prof.id.c_str(), side);
                CHECK(nearQ(k.collarLocal, Q(), 1e-4f), "LEG %s %d forward: the collar stays on the body", prof.id.c_str(), side);
            }
            // 2b. Hanging, the hand rolled about its fingers (pronation / supination) by up to 2
            //    rad: the hoof takes the whole roll, the forearm a quarter and the cannon half in
            //    all (within 0.7 / 1.4 rad), the hoof joint still on the target.
            for (float roll : {0.6f, -0.6f, 2.0f, -2.0f})
            {
                const V3 restHoof = shoulderP + rig.off[c + 2] + rig.off[c + 3] + rig.off[hf];
                const Q hand0 = slfbt::qconj(f.R[c + 3]);
                const V3 fingers = qrot(hand0, V3(0.f, s, 0.f));   // (the virtual hand's fingers: down the hanging leg)
                const Q handQ = slfbt::qnormalized(qmul(slfbt::qaxis(fingers, roll), hand0));
                float state = 0.f;
                const LimbFK k = solvePonyLeg(rig, f, side, chestP, restHoof, handQ, &state);
                CHECK(nearV(k.hoof, restHoof, 2e-3f), "LEG %s %d roll %.1f: hoof %.4f m off", prof.id.c_str(), side, roll,
                      length(k.hoof - restHoof));
                CHECK(nearQ(k.hoofWorld, qmul(handQ, f.R[hf]), 2e-3f), "LEG %s %d roll %.1f: the hoof turned as the hand", prof.id.c_str(),
                      side, roll);
                const float wantFore = std::max(-0.7f, std::min(0.7f, 0.25f * roll));
                const float wantCannon = std::max(-1.4f, std::min(1.4f, 0.5f * roll));
                CHECK(std::fabs(k.forearmTwist - wantFore) < 0.03f && std::fabs(k.cannonTwist - wantCannon) < 0.05f,
                      "LEG %s %d roll %.1f: shares forearm %.2f (want %.2f), cannon %.2f (want %.2f)", prof.id.c_str(), side, roll,
                      k.forearmTwist, wantFore, k.cannonTwist, wantCannon);
                // (Within a few hundredths: the solver bends the elbow straight back where the rest
                // leg bends a little in, so the untwisted leg is a hair off its rest roll.)
                // Measured on the actual joints: the forearm and cannon roll only those shares, the
                // hoof the rest (relative to the cannon); the cannon keeps its rest bend.
                float cannonSwing = 0.f;
                const float fore = rollAbout(k.local[2], rig.off[c + 3]);
                const float can = rollAbout(k.local[3], rig.off[hf], &cannonSwing);
                const float hoofR = rollAbout(k.local[4], rig.off[hf]);
                CHECK(std::fabs(fore - wantFore) < 0.08f && std::fabs(fore + can - wantCannon) < 0.1f,
                      "LEG %s %d roll %.1f: forearm rolled %.2f, cannon %.2f in all (want %.2f / %.2f)", prof.id.c_str(), side, roll, fore,
                      fore + can, wantFore, wantCannon);
                CHECK(std::fabs(fore + can + hoofR - roll) < 0.08f, "LEG %s %d roll %.1f: the hoof takes the rest (%.2f in all)",
                      prof.id.c_str(), side, roll, fore + can + hoofR);
                CHECK(cannonSwing < 0.03f, "LEG %s %d roll %.1f: the cannon keeps its rest bend (%.3f rad off)", prof.id.c_str(), side, roll,
                      cannonSwing);
            }
            // 3. Out of reach: the leg points at the target, never stretched.
            {
                const V3 target = shoulderP + V3(1.5f, 0.f, 0.f);
                const Q handQ = slfbt::qfromBasis(V3(0.f, 0.f, 1.f), V3(s, 0.f, 0.f), V3(0.f, s, 0.f));
                const LimbFK k = solvePonyLeg(rig, f, side, chestP, target, handQ);
                CHECK(!k.reached && length(k.hoof - k.shoulder) < 0.43f, "LEG %s %d far: not stretched (%.3f)", prof.id.c_str(), side,
                      length(k.hoof - k.shoulder));
            }
        }
    }
}

static void testSettle()
{
    // The owner's edit: Arms only with torso and chest set to Tracked -> it stands up, and standing
    // takes the hips and hind legs too.
    Profile p = ponyArms();
    p.state[R_SPINE] = p.state[R_CHEST] = S_TRACKED;
    p.custom = true;
    settle(p);
    CHECK(p.upright, "SETTLE: a pony with its spine tracked stands up");
    for (int r : {R_PELVIS, R_SPINE, R_CHEST, R_L_THIGH, R_L_SHIN, R_L_FOOT, R_R_THIGH, R_R_SHIN, R_R_FOOT})
        CHECK(p.tracked(r), "SETTLE: standing drives %s", roleInfo(r).key);
    CHECK(p.state[R_L_FINGERS] == S_ANIMATION, "SETTLE: the hoof is still not finger-tracked");
    // The stance through the frames and the body layer's spine solve (virtual skeleton, then the
    // actual joints): the neck target straight over the hips stands the barrel up; one ahead of
    // them leans it forward. Not the horizontal barrel the owner saw.
    Frames f;
    const RestRig rig = ponyRig();
    computeFrames(p, rig, f);
    CHECK(f.upright, "SETTLE: frames upright");
    slfbt::SpineRest sr;
    sr.torso = virtualOffset(f.R[R_PELVIS], rig.off[R_SPINE]);
    sr.neck = virtualOffset(f.R[R_SPINE], rig.off[R_CHEST]) + virtualOffset(f.R[R_CHEST], rig.off[R_NECK]);
    const V3 pelvisP(0.f, 0.f, 0.6f);
    for (float lean : {0.f, 0.15f})
    {
        const V3 neckT = pelvisP + V3(lean, 0.f, length(sr.torso + sr.neck));
        const slfbt::SpinePose sp = slfbt::solveSpine(pelvisP, Q(), sr, neckT, 0.f, 0.5f);
        // Actual locals: pelvis = virtual world (identity here) * R; torso and chest under it.
        Q vl[R_COUNT];
        vl[R_PELVIS] = Q();
        vl[R_SPINE] = sp.torsoWorld;
        vl[R_CHEST] = qmul(slfbt::qconj(sp.torsoWorld), sp.chestWorld);
        Q aw = actualLocal(Q(), f.R[R_PELVIS], vl[R_PELVIS]);
        V3 at = pelvisP + qrot(aw, rig.off[R_SPINE]);
        aw = qmul(aw, actualLocal(f.R[R_PELVIS], f.R[R_SPINE], vl[R_SPINE]));
        at = at + qrot(aw, rig.off[R_CHEST]);
        aw = qmul(aw, actualLocal(f.R[R_SPINE], f.R[R_CHEST], vl[R_CHEST]));
        const V3 neckAt = at + qrot(aw, rig.off[R_NECK]);
        const V3 barrel = normalized(neckAt - pelvisP);
        CHECK(lean > 0.f || nearV(neckAt, neckT, 0.01f), "SETTLE spine (lean %.2f): the neck reaches its target (%.3f off)", lean, length(neckAt - neckT));
        CHECK(lean > 0.f ? (barrel.x > 0.2f && barrel.z > 0.8f) : barrel.z > 0.999f, "SETTLE spine (lean %.2f): barrel (%.2f %.2f %.2f)", lean,
              barrel.x, barrel.y, barrel.z);
    }
    // An OFF role stays OFF (held), an unmapped one unmapped.
    Profile q = ponyBipedal();
    q.state[R_L_FOOT] = S_OFF;
    q.joint[R_R_FOOT].clear();
    settle(q);
    CHECK(q.state[R_L_FOOT] == S_OFF && !q.enabled(R_R_FOOT), "SETTLE: off and unmapped roles kept");
    // Not for a human, nor a pony that tracks only its head / arms.
    Profile h = humanProfile();
    settle(h);
    CHECK(!h.upright, "SETTLE: human not upright");
    Profile a = ponyArms(), hd = ponyHead();
    settle(a);
    settle(hd);
    CHECK(!a.upright && !hd.upright && a.state[R_PELVIS] == S_ANIMATION, "SETTLE: arms / head only stay on four legs");
    // An older custom map with the hand on mHandRing1 (the owner's try): the hoof role gives the
    // joint up (one joint, one role); the other side keeps its hoof.
    Profile old = ponyArms();
    old.joint[R_L_HAND] = "mHandRing1Left";
    settle(old);
    CHECK(!old.enabled(R_L_HOOF) && old.joint[R_R_HOOF] == "mHandRing1Right", "SETTLE: no joint on two roles");
    // Back on four legs: the stance's roles to the animation, and settle leaves it so.
    Profile b = ponyBipedal();
    onFourLegs(b);
    settle(b);
    CHECK(!b.upright && b.state[R_SPINE] == S_ANIMATION && b.state[R_L_THIGH] == S_ANIMATION && b.tracked(R_L_HAND),
          "SETTLE: on four legs again, the arms still tracked");
}

// The VR camera's eye point on the avatar (and the head joint the body puts under the camera):
// the Open Pony's pupils, a human's eye joints, else the SL skeleton's eyes; never the old fixed
// (0.10, 0, 0.06) -- 8 cm under the pony's eyes, the view low on the neck.
static void testEyes()
{
    EyeJoints pony;   // (the dev kit: the eyes (0.12, +-0.09, 0.14) from mHead; the eye joints a human's)
    pony.pupils = true;
    pony.pupilMid = V3(0.12f, 0.f, 0.14f);
    pony.eyes = true;
    pony.eyeMid = V3(0.098f, 0.f, 0.079f);
    int src = -1;
    const V3 pe = eyeFromHead(true, pony, &src);
    CHECK(src == EYE_PUPILS && nearV(pe, V3(0.12f, 0.f, 0.14f), 1e-6f), "EYES pony: the pupils (%.3f, %.3f)", pe.x, pe.z);
    // The bipedal pony's eye above its neck joint: neck -> head 0.162 (the kit) + 0.14, and the
    // head joint 0.14 under the camera (it was 0.06: the head sat 8 cm high, the view on the neck).
    {
        const RestRig rig = ponyRig();
        const V3 neckToEye = rig.off[R_HEAD] + pe;
        CHECK(std::fabs(neckToEye.z - (rig.off[R_HEAD].z + 0.14f)) < 1e-5f && neckToEye.z > 0.29f,
              "EYES pony: the eye %.3f above the neck joint", neckToEye.z);
        CHECK(pe.z - 0.06f > 0.07f, "EYES pony: the eye point %.0f cm higher on the head than the old fixed one", (pe.z - 0.06f) * 100.f);
    }
    // A human (pupil bones are not eyes there): the eye joints.
    const V3 he = eyeFromHead(false, pony, &src);
    CHECK(src == EYE_JOINTS && nearV(he, V3(0.098f, 0.f, 0.079f), 1e-6f), "EYES human: the eye joints");
    // A pony whose pupils are nowhere near an eye (a stale override): the eye joints.
    EyeJoints odd = pony;
    odd.pupilMid = V3(0.6f, 0.f, -0.3f);
    eyeFromHead(true, odd, &src);
    CHECK(src == EYE_JOINTS, "EYES: implausible pupils fall back to the eye joints");
    // Nothing usable: the SL skeleton's eyes, on the middle.
    EyeJoints none;
    const V3 de = eyeFromHead(true, none, &src);
    CHECK(src == EYE_DEFAULT && nearV(de, V3(kEyeForward, 0.f, kEyeUp), 1e-6f), "EYES: the default eyes");
    EyeJoints side = pony;
    side.pupilMid = V3(0.12f, 0.03f, 0.14f);   // (a lopsided override: the camera stays on the middle)
    CHECK(std::fabs(eyeFromHead(true, side).y) < 1e-6f, "EYES: on the middle");
    // One standing height: the camera maps your head at its reference height onto the avatar's
    // eyes (its held anchor: feet + the standing eye height), and the body scales your height by
    // that same reference (LLVRBody's userEyeHeight), so the two meet. The owner's log: a body
    // calibrated at 1.69 m, a recentre at 1.60 m: the body's eyes 5% under the camera.
    {
        const float footZ = 20.f, avatarEye = 1.265f, ref = 1.60f, calibrated = 1.69f;
        const V3 head(0.3f, -0.2f, ref);   // (standing as at the recentre)
        const V3 now = slfbt::roomToAgent(head, V3(), 0.3f, -0.2f, 0.f, avatarEye / ref, footZ);
        CHECK(std::fabs(now.z - (footZ + avatarEye)) < 1e-4f, "EYES: the body's eye at the camera (%.3f vs %.3f)", now.z - footZ,
              avatarEye);
        const V3 old = slfbt::roomToAgent(head, V3(), 0.3f, -0.2f, 0.f, avatarEye / calibrated, footZ);
        CHECK(footZ + avatarEye - old.z > 0.06f, "EYES: the calibration's height left it %.1f cm under the camera",
              (footZ + avatarEye - old.z) * 100.f);
    }
}

int main()
{
    testHuman();
    testPonyProfiles();
    testUpright();
    testArms();
    testDetect();
    testText();
    testReach();
    testPonyArmChain();
    testSettle();
    testEyes();
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
