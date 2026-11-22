// Fixtures for VR headset body tracking's joint -> role mapping (#1583;
// indra/newview/llvrbodyheadsetmath.h) and the OpenXR <-> body space conversion
// (llvrbodymath.h), run by scripts/check/checks/vr_body_headset.py (host C++ compiler only).
//   AXES     OpenXR app space -> body space: -Z forward -> +X, +X right -> -Y, +Y up -> +Z;
//            a turn about OpenXR +Y is the same turn about SL +Z; body -> OpenXR round trips.
//   MAP      each joint set's table points at the right joints (hips, chest, knees, ankles);
//            the upper-body set has no legs.
//   POSE     a person standing facing a direction gives hips / chest / feet facing it, at the
//            joints' positions; turning the whole body turns every role.
//   VALID    a role needs its joints: a missing hip line falls back to the shoulders, a missing
//            ankle drops the foot, untracked joints mark the role untracked.
//   HOLD     a short dropout is held (untracked), a long one goes invalid; low confidence
//            smooths more.
#include "llvrbodyheadsetmath.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace slfbt;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static bool nearV(const V3& a, const V3& b, float eps) { return length(a - b) <= eps; }

static void testAxes()
{
    const float fwd[3] = {0.f, 0.f, -1.f}, right[3] = {1.f, 0.f, 0.f}, upv[3] = {0.f, 1.f, 0.f};
    CHECK(nearV(xrPosToBody(fwd, 0.f), V3(1.f, 0.f, 0.f), 1e-6f), "-Z_xr -> +X");
    CHECK(nearV(xrPosToBody(right, 0.f), V3(0.f, -1.f, 0.f), 1e-6f), "+X_xr -> -Y");
    CHECK(nearV(xrPosToBody(upv, 0.f), V3(0.f, 0.f, 1.f), 1e-6f), "+Y_xr -> +Z");
    const float p[3] = {0.3f, 1.2f, -0.5f};
    CHECK(nearV(xrPosToBody(p, 0.2f), V3(0.5f, -0.3f, 1.0f), 1e-6f), "floor offset");
    // +90 deg about OpenXR +Y (turn left: -Z forward -> -X) is +90 deg about SL +Z (X -> Y).
    const float s = std::sqrt(0.5f);
    const float qy[4] = {0.f, s, 0.f, s};
    const Q qb = xrRotToBody(qy);
    CHECK(qangle(qb, qyaw(1.5707963f)) < 1e-4f, "yaw about +Y_xr = yaw about +Z_sl");
    // Round trips.
    const Q any = qnormalized(Q(0.2f, -0.5f, 0.3f, 0.8f));
    float qx[4];
    bodyRotToXr(any, qx);
    CHECK(qangle(xrRotToBody(qx), any) < 1e-5f, "rotation round trip");
    const V3 pb(0.4f, -1.1f, 1.7f);
    float px[3];
    bodyPosToXr(pb, px);
    CHECK(nearV(xrPosToBody(px, 0.f), pb, 1e-6f), "position round trip");
    // Conjugation: rotating in OpenXR space then mapping = mapping then rotating in body space.
    const V3 vb(0.3f, 0.7f, -0.2f);
    float vx[3];
    bodyPosToXr(vb, vx);
    const Q qxq(qx[0], qx[1], qx[2], qx[3]);
    const V3 rx = qrot(qxq, V3(vx[0], vx[1], vx[2]));
    const float rxa[3] = {rx.x, rx.y, rx.z};
    CHECK(nearV(xrPosToBody(rxa, 0.f), qrot(any, vb), 1e-5f), "rotation maps like the axes");
}

static void testMap()
{
    const JointMap& fu = jointMap(JS_FB_UPPER);
    CHECK(fu.hips == 1 && fu.chest == 5 && fu.neck == 6 && fu.spine == 2, "FB upper torso joints");
    CHECK(!jointSetHasLegs(JS_FB_UPPER) && fu.knee[0] < 0, "FB upper: no legs");
    const JointMap& ff = jointMap(JS_FB_FULL);
    CHECK(ff.hip[0] == 70 && ff.knee[0] == 71 && ff.ankle[0] == 73 && ff.toe[0] == 76, "Meta full body left leg");
    CHECK(ff.hip[1] == 77 && ff.knee[1] == 78 && ff.ankle[1] == 80 && ff.toe[1] == 83, "Meta full body right leg");
    CHECK(jointSetCount(JS_FB_UPPER) == 70 && jointSetCount(JS_FB_FULL) == 84, "FB counts");
    const JointMap& h = jointMap(JS_HTC);
    CHECK(h.hips == 0 && h.knee[0] == 2 && h.ankle[1] == 7 && h.toe[1] == 8 && h.chest == 13, "HTC");
    const JointMap& b = jointMap(JS_BD);
    CHECK(b.hips == 0 && b.knee[1] == 5 && b.ankle[0] == 7 && b.toe[0] == 10 && b.chest == 9, "BD");
    // Every index inside its set.
    for (int s = JS_FB_UPPER; s < JS_COUNT; ++s)
    {
        const JointMap& m = jointMap(s);
        const int idx[] = {m.hips, m.spine, m.chest, m.neck, m.shoulder[0], m.shoulder[1], m.hip[0], m.hip[1],
                           m.knee[0], m.knee[1], m.ankle[0], m.ankle[1], m.toe[0], m.toe[1]};
        for (int i : idx) CHECK(i < jointSetCount(s), "set %d index %d out of range", s, i);
    }
}

// A person standing at `at` facing `yaw` (body space), Meta full-body indices.
static void standing(BodyJoint* j, float yaw, const V3& at)
{
    for (int i = 0; i < kMaxBodyJoints; ++i) j[i] = BodyJoint{};
    const Q t = qyaw(yaw);
    auto put = [&](int i, float x, float y, float z) {
        j[i].valid = j[i].tracked = true;
        j[i].pos = at + qrot(t, V3(x, y, z));
    };
    put(1, 0.f, 0.f, 0.95f);           // hips
    put(2, 0.f, 0.f, 1.05f);           // spine lower
    put(5, 0.f, 0.f, 1.30f);           // chest
    put(6, 0.f, 0.f, 1.50f);           // neck
    put(10, 0.f, 0.18f, 1.42f);        // left arm upper
    put(15, 0.f, -0.18f, 1.42f);       // right arm upper
    put(70, 0.f, 0.09f, 0.90f);        // left upper leg (hip joint)
    put(71, 0.02f, 0.09f, 0.50f);      // left knee
    put(73, 0.f, 0.09f, 0.08f);        // left ankle
    put(76, 0.14f, 0.10f, 0.02f);      // left ball of the foot
    put(77, 0.f, -0.09f, 0.90f);
    put(78, 0.02f, -0.09f, 0.50f);
    put(80, 0.f, -0.09f, 0.08f);
    put(83, 0.14f, -0.10f, 0.02f);
}

static void testPose()
{
    BodyJoint j[kMaxBodyJoints];
    RolePose r[B_COUNT];
    for (float yaw : {0.f, 0.8f, -2.5f})
    {
        const V3 at(1.f, -2.f, 0.f);
        standing(j, yaw, at);
        mapBodyJoints(JS_FB_FULL, j, 84, r);
        for (int b = 0; b < B_COUNT; ++b) CHECK(r[b].valid && r[b].tracked, "yaw %.2f: %s valid", yaw, bodyName(b));
        // Upright, facing yaw: the SL rest frame turned by yaw.
        CHECK(qangle(r[B_WAIST].rot, qyaw(yaw)) < 0.02f, "yaw %.2f: hips facing (%.3f)", yaw, qangle(r[B_WAIST].rot, qyaw(yaw)));
        CHECK(qangle(r[B_CHEST].rot, qyaw(yaw)) < 0.02f, "yaw %.2f: chest facing", yaw);
        CHECK(std::fabs(yawOf(r[B_LFOOT].rot) - yawOf(qyaw(yaw))) < 0.1f, "yaw %.2f: left foot heading", yaw);
        CHECK(qangle(r[B_RFOOT].rot, qyaw(yawOf(r[B_RFOOT].rot))) < 1e-4f, "foot: heading only (no pitch)");
        CHECK(nearV(r[B_WAIST].pos, j[1].pos, 1e-6f) && nearV(r[B_LFOOT].pos, j[73].pos, 1e-6f) &&
                  nearV(r[B_RKNEE].pos, j[78].pos, 1e-6f) && nearV(r[B_CHEST].pos, j[5].pos, 1e-6f),
              "roles at their joints");
    }
    // Bending forward pitches the hips' frame toward +X (up the spine leans forward).
    standing(j, 0.f, V3());
    j[2].pos = V3(0.08f, 0.f, 1.03f);
    mapBodyJoints(JS_FB_FULL, j, 84, r);
    CHECK(qrot(r[B_WAIST].rot, V3(0.f, 0.f, 1.f)).x > 0.3f, "hips lean forward");
    // The upper-body set: hips and chest only.
    standing(j, 0.3f, V3());
    mapBodyJoints(JS_FB_UPPER, j, 70, r);
    CHECK(r[B_WAIST].valid && r[B_CHEST].valid, "upper body: torso");
    CHECK(!r[B_LFOOT].valid && !r[B_RKNEE].valid, "upper body: no legs");
    CHECK(qangle(r[B_WAIST].rot, qyaw(0.3f)) < 0.02f, "upper body: hips face along the shoulders");
}

static void testValidity()
{
    BodyJoint j[kMaxBodyJoints];
    RolePose r[B_COUNT];
    standing(j, 0.f, V3());
    j[73].valid = false;   // left ankle lost
    j[70].valid = false;   // left hip joint lost: the hips take the shoulder line
    j[78].tracked = false; // right knee generated
    mapBodyJoints(JS_FB_FULL, j, 84, r);
    CHECK(!r[B_LFOOT].valid, "no ankle, no foot");
    CHECK(r[B_WAIST].valid && qangle(r[B_WAIST].rot, Q()) < 0.02f, "hips from the shoulders");
    CHECK(r[B_RKNEE].valid && !r[B_RKNEE].tracked, "generated knee: valid, marked");
    j[76].valid = false;   // left ball lost too; the right foot keeps its own
    j[73].valid = true;
    mapBodyJoints(JS_FB_FULL, j, 84, r);
    CHECK(r[B_LFOOT].valid && !r[B_LFOOT].tracked, "no ball: the hips' heading, marked");
    // A NaN joint is not valid.
    j[5].pos = V3(NAN, 0.f, 1.3f);
    mapBodyJoints(JS_FB_FULL, j, 84, r);
    CHECK(!r[B_CHEST].valid, "NaN chest rejected");
    // A count below the set's (a short array) never reads past it.
    standing(j, 0.f, V3());
    mapBodyJoints(JS_FB_FULL, j, 70, r);
    CHECK(!r[B_LFOOT].valid && r[B_CHEST].valid, "short array: legs dropped");
    // HTC and BD sets map too.
    BodyJoint h[26];
    for (BodyJoint& b : h) b = BodyJoint{};
    auto put = [&](BodyJoint* a, int i, float x, float y, float z) { a[i].valid = a[i].tracked = true; a[i].pos = V3(x, y, z); };
    put(h, 0, 0.f, 0.f, 0.95f); put(h, 10, 0.f, 0.f, 1.05f); put(h, 13, 0.f, 0.f, 1.3f); put(h, 14, 0.f, 0.f, 1.5f);
    put(h, 18, 0.f, 0.18f, 1.42f); put(h, 23, 0.f, -0.18f, 1.42f);
    put(h, 1, 0.f, 0.09f, 0.9f); put(h, 5, 0.f, -0.09f, 0.9f);
    put(h, 2, 0.f, 0.09f, 0.5f); put(h, 3, 0.f, 0.09f, 0.08f); put(h, 4, 0.14f, 0.09f, 0.02f);
    put(h, 6, 0.f, -0.09f, 0.5f); put(h, 7, 0.f, -0.09f, 0.08f); put(h, 8, 0.14f, -0.09f, 0.02f);
    mapBodyJoints(JS_HTC, h, 26, r);
    for (int b = 0; b < B_COUNT; ++b) CHECK(r[b].valid, "HTC %s", bodyName(b));
    CHECK(qangle(r[B_WAIST].rot, Q()) < 0.02f && nearV(r[B_RFOOT].pos, V3(0.f, -0.09f, 0.08f), 1e-6f), "HTC pose");
    BodyJoint d[24];
    for (BodyJoint& b : d) b = BodyJoint{};
    put(d, 0, 0.f, 0.f, 0.95f); put(d, 3, 0.f, 0.f, 1.05f); put(d, 9, 0.f, 0.f, 1.3f); put(d, 12, 0.f, 0.f, 1.5f);
    put(d, 16, 0.f, 0.18f, 1.42f); put(d, 17, 0.f, -0.18f, 1.42f);
    put(d, 1, 0.f, 0.09f, 0.9f); put(d, 2, 0.f, -0.09f, 0.9f);
    put(d, 4, 0.f, 0.09f, 0.5f); put(d, 7, 0.f, 0.09f, 0.08f); put(d, 10, 0.14f, 0.09f, 0.02f);
    put(d, 5, 0.f, -0.09f, 0.5f); put(d, 8, 0.f, -0.09f, 0.08f); put(d, 11, 0.14f, -0.09f, 0.02f);
    mapBodyJoints(JS_BD, d, 24, r);
    for (int b = 0; b < B_COUNT; ++b) CHECK(r[b].valid, "BD %s", bodyName(b));
    CHECK(nearV(r[B_LKNEE].pos, V3(0.f, 0.09f, 0.5f), 1e-6f), "BD knee");
}

static void testHold()
{
    RolePose held, s;
    float age = 0.f;
    s.valid = s.tracked = true;
    s.pos = V3(1.f, 2.f, 3.f);
    RolePose o = holdRole(s, held, age, 0.011f);
    CHECK(o.valid && o.tracked, "valid passes");
    const RolePose gone;
    o = holdRole(gone, held, age, 0.05f);
    CHECK(o.valid && !o.tracked && nearV(o.pos, s.pos, 1e-6f), "short dropout held, marked");
    o = holdRole(gone, held, age, 0.05f);
    CHECK(o.valid, "still held at 0.10 s");
    o = holdRole(gone, held, age, 0.1f);
    CHECK(!o.valid, "long dropout invalid");
    o = holdRole(gone, held, age, 0.01f);
    CHECK(!o.valid, "stays invalid");
    o = holdRole(s, held, age, 0.01f);
    CHECK(o.valid && age == 0.f, "back");
    CHECK(std::fabs(confidenceTau(0.03f, 1.f) - 0.03f) < 1e-7f && std::fabs(confidenceTau(0.03f, 0.f) - 0.12f) < 1e-6f &&
              std::fabs(confidenceTau(0.03f, 2.f) - 0.03f) < 1e-7f,
          "confidence smoothing");
}

int main()
{
    testAxes();
    testMap();
    testPose();
    testValidity();
    testHold();
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("vr body headset: all fixtures passed\n");
    return 0;
}
