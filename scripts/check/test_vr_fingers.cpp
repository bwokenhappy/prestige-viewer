// Fixtures for VR finger tracking (#1587; indra/newview/llvrbodyfingermath.h), run by
// scripts/check/checks/vr_fingers.py (host compiler only, no viewer build).
//   JOINTS     a synthetic hand (26 XR_EXT_hand_tracking joints) bent by known angles gives those
//              angles back; the spread too; the right hand (mirrored) gives the same pose; any
//              turn of the whole hand changes nothing; a missing joint is refused.
//   CONTROLLER trigger untouched: the index points; a full grip: a fist; the thumb down when
//              touched, up when not; an open hand spreads.
//   BONES      a curl turns a finger's bone toward the palm (-Z) on either hand; the thumb bends
//              toward the palm too; a spread moves the index thumbward (+X) on either hand.
#include "llvrbodyfingermath.h"

#include <cstdio>
#include <initializer_list>

using namespace slfinger;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// A hand in a frame where the fingers point +Y * side... built for the LEFT hand in the SL rest
// frame (fingers +Y, thumb side +X, palm -Z), then mirrored (Y -> -Y) for the right hand.
// Each finger: metacarpal from the wrist, then three bones bending about -X (toward -Z) by the
// pose's curls; its first bone also turned in the palm's plane by the spread.
static void makeHand(const HandPose& p, const Limits& lim, V3 j[JOINTS])
{
    j[1] = V3(0.f, 0.f, 0.f);       // wrist
    j[0] = V3(0.f, 0.05f, 0.f);     // palm
    const float metaX[5] = {0.f, 0.03f, 0.01f, -0.01f, -0.03f};
    const float len[3] = {0.045f, 0.028f, 0.022f};
    for (int f = 1; f < 5; ++f)
    {
        const int b = 6 + (f - 1) * 5;
        j[b] = V3(metaX[f] * 0.5f, 0.01f, 0.f);              // metacarpal base
        j[b + 1] = V3(metaX[f], 0.09f, 0.f);                  // knuckle
        // In-plane spread (+ away from the middle: index +X, ring / little -X) on top of the natural one.
        const float away = f == 1 ? 1.f : (f == 2 ? 0.f : -1.f);
        const float spread = (p.splay[f] + lim.naturalSplay[f]) * away;
        V3 dir(std::sin(spread), std::cos(spread), 0.f);
        float bendSoFar = 0.f;
        for (int k = 0; k < 3; ++k)
        {
            bendSoFar += p.curl[f][k];
            // Bend toward -Z about the axis across the finger in the palm plane.
            const V3 flat(std::sin(spread), std::cos(spread), 0.f);
            dir = flat * std::cos(bendSoFar) + V3(0.f, 0.f, -1.f) * std::sin(bendSoFar);
            j[b + 2 + k] = j[b + 1 + k] + dir * len[k];
        }
    }
    // Thumb: metacarpal from near the wrist toward +X / +Y, lifted toward the palm by curl[0][0].
    const float tAng = lim.naturalSplay[0] + p.splay[0];   // from the middle finger's direction
    const V3 tFlat(std::sin(tAng), std::cos(tAng), 0.f);
    j[2] = V3(0.02f, 0.015f, 0.f);
    V3 d = tFlat * std::cos(p.curl[0][0]) + V3(0.f, 0.f, -1.f) * std::sin(p.curl[0][0]);
    j[3] = j[2] + d * 0.04f;
    const V3 across = slfbt::normalized(slfbt::cross(d, slfbt::cross(V3(0.f, 0.f, -1.f), d)));   // (in the bend plane)
    (void)across;
    // The two thumb joints bend in the plane of the metacarpal and the palm normal.
    const V3 n = slfbt::normalized(V3(0.f, 0.f, -1.f) - d * slfbt::dot(V3(0.f, 0.f, -1.f), d));
    V3 d1 = d * std::cos(p.curl[0][1]) + n * std::sin(p.curl[0][1]);
    j[4] = j[3] + d1 * 0.032f;
    const V3 n1 = slfbt::normalized(V3(0.f, 0.f, -1.f) - d1 * slfbt::dot(V3(0.f, 0.f, -1.f), d1), n);
    const V3 d2 = d1 * std::cos(p.curl[0][2]) + n1 * std::sin(p.curl[0][2]);
    j[5] = j[4] + d2 * 0.03f;
}

static HandPose samplePose()
{
    HandPose p;
    const float c[5][3] = {{0.3f, 0.4f, 0.5f}, {0.2f, 0.6f, 0.3f}, {0.9f, 1.2f, 0.7f}, {1.1f, 1.3f, 0.8f}, {0.5f, 0.2f, 0.1f}};
    for (int f = 0; f < 5; ++f)
        for (int k = 0; k < 3; ++k) p.curl[f][k] = c[f][k];
    p.splay[0] = 0.1f;
    p.splay[1] = 0.15f;
    p.splay[3] = 0.05f;
    p.splay[4] = 0.2f;
    return p;
}

static void testJoints()
{
    const Limits lim;
    const HandPose want = samplePose();
    V3 j[JOINTS];
    bool valid[JOINTS];
    for (bool& v : valid) v = true;
    makeHand(want, lim, j);
    HandPose got;
    CHECK(fromJoints(j, valid, 1.f, lim, got), "left hand read");
    for (int f = 1; f < 5; ++f)
        for (int k = 0; k < 3; ++k)
            CHECK(std::fabs(got.curl[f][k] - want.curl[f][k]) < 0.02f, "finger %d joint %d curl %f vs %f", f, k, got.curl[f][k], want.curl[f][k]);
    for (int k = 0; k < 3; ++k)
        CHECK(std::fabs(got.curl[0][k] - want.curl[0][k]) < 0.03f, "thumb joint %d curl %f vs %f", k, got.curl[0][k], want.curl[0][k]);
    for (int f : {1, 3, 4})
        CHECK(std::fabs(got.splay[f] - want.splay[f]) < 0.02f, "finger %d spread %f vs %f", f, got.splay[f], want.splay[f]);
    CHECK(std::fabs(got.splay[0] - want.splay[0]) < 0.05f, "thumb spread %f vs %f", got.splay[0], want.splay[0]);
    // The right hand: mirrored joints, the same pose.
    V3 r[JOINTS];
    for (int i = 0; i < JOINTS; ++i) r[i] = V3(j[i].x, -j[i].y, j[i].z);
    HandPose gr;
    CHECK(fromJoints(r, valid, -1.f, lim, gr), "right hand read");
    float worst = 0.f;
    for (int f = 0; f < 5; ++f)
    {
        for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(gr.curl[f][k] - got.curl[f][k]));
        worst = std::max(worst, std::fabs(gr.splay[f] - got.splay[f]));
    }
    CHECK(worst < 1e-3f, "mirror gives the same pose (%f)", worst);
    // The whole hand turned and moved: the same pose.
    const Q t = slfbt::qnormalized(Q(0.3f, -0.5f, 0.2f, 0.8f));
    V3 m[JOINTS];
    for (int i = 0; i < JOINTS; ++i) m[i] = slfbt::qrot(t, j[i]) + V3(1.f, 2.f, 1.2f);
    HandPose gm;
    fromJoints(m, valid, 1.f, lim, gm);
    worst = 0.f;
    for (int f = 0; f < 5; ++f)
    {
        for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(gm.curl[f][k] - got.curl[f][k]));
        worst = std::max(worst, std::fabs(gm.splay[f] - got.splay[f]));
    }
    CHECK(worst < 1e-3f, "rigid motion changes nothing (%f)", worst);
    // A flat open hand reads (almost) zero.
    HandPose flat;
    makeHand(flat, lim, j);
    fromJoints(j, valid, 1.f, lim, got);
    float sum = 0.f;
    for (int f = 0; f < 5; ++f)
        for (int k = 0; k < 3; ++k) sum += std::fabs(got.curl[f][k]);
    CHECK(sum < 0.05f, "flat hand (%f)", sum);
    // A missing joint: refused (the caller fades to the controller pose).
    valid[12] = false;
    CHECK(!fromJoints(j, valid, 1.f, lim, got), "missing joint refused");
}

static void testController()
{
    const Limits lim;
    ControllerInput in;
    in.triggerTouchBound = in.thumbTouchBound = true;
    // Pointing: trigger not touched, grip closed, thumb down.
    in.triggerTouch = false;
    in.squeeze = 1.f;
    in.thumbTouch = true;
    HandPose p = fromController(in, lim);
    CHECK(p.curl[1][0] == 0.f && p.curl[2][1] > 1.5f && p.curl[0][2] > 0.8f, "point");
    // Thumbs up: fist, thumb not touching.
    in.triggerTouch = true;
    in.trigger = 1.f;
    in.thumbTouch = false;
    p = fromController(in, lim);
    CHECK(p.curl[1][1] > 1.6f && p.curl[0][2] < 0.2f, "thumbs up");
    // Open hand: nothing pressed, nothing touched: little curl, a spread.
    in = ControllerInput{};
    in.triggerTouchBound = in.thumbTouchBound = true;
    p = fromController(in, lim);
    CHECK(p.curl[1][1] < 0.01f && p.curl[3][1] < 0.3f && p.splay[4] > 0.1f, "open hand");
    // No touch sensors (simple controller): the trigger alone.
    in = ControllerInput{};
    in.trigger = 1.f;
    p = fromController(in, lim);
    CHECK(p.curl[1][1] > 1.6f, "trigger only");
    // Smoothing reaches the target.
    HandPose s;
    for (int i = 0; i < 90; ++i) smoothPose(s, p, 1.f / 90.f, 0.06f);
    CHECK(std::fabs(s.curl[1][1] - p.curl[1][1]) < 0.01f, "smoothing converges");
}

static void testBones()
{
    HandPose p;
    for (int f = 0; f < 5; ++f) p.curl[f][1] = 0.8f;
    for (float side : {1.f, -1.f})
    {
        // An index middle bone (rest along +side * Y).
        const V3 d(0.017f, 0.036f * side, -0.006f);
        const V3 bent = slfbt::qrot(boneLocal(1, 1, d, side, p), d);
        CHECK(bent.z < d.z - 0.02f, "finger curls toward the palm (side %g: %f)", side, bent.z);
        // The thumb (rest toward +X / +side Y).
        const V3 t(0.028f, 0.032f * side, -0.001f);
        const V3 tb = slfbt::qrot(boneLocal(0, 1, t, side, p), t);
        CHECK(tb.z < t.z - 0.01f, "thumb bends toward the palm (side %g)", side);
        // Spread: the index's first bone moves thumbward (+X).
        HandPose s;
        s.splay[1] = 0.3f;
        const V3 i1(0.f, 0.036f * side, 0.f);
        CHECK(slfbt::qrot(boneLocal(1, 0, i1, side, s), i1).x > 0.008f, "index spreads thumbward (side %g)", side);
        s.splay[4] = 0.3f;
        CHECK(slfbt::qrot(boneLocal(4, 0, i1, side, s), i1).x < -0.008f, "little finger spreads away (side %g)", side);
        // Zero pose: identity.
        CHECK(slfbt::qangle(boneLocal(2, 0, i1, side, HandPose{}), Q()) < 1e-5f, "rest is identity");
    }
}

int main()
{
    testJoints();
    testController();
    testBones();
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
