// Fixtures for the VR 3-point body estimate (#1586; indra/newview/llvrbodyestimatemath.h), run by
// scripts/check/checks/vr_estimate.py (host compiler only, no viewer build).
//   HEADING  a glance within the twist budget barely turns the body; a turn past it brings the
//            body within the budget quickly; within it the body settles toward the head slowly.
//   CROUCH   the head dropping straight down drops the hips by as much (never above standing),
//            the feet stay planted, no steps in a deep crouch.
//   LEAN     bending forward (head out and down) keeps the feet planted and the hips over them.
//   STEP     standing still: no steps; walking: a foot steps once its spot drifts past stepDist,
//            one foot at a time, each step lasting stepTime with a lift; turning past stepTurn
//            steps; a step lands near the foot's spot.
//   RATE     90 Hz and 144 Hz give the same body (heading, hips, feet) within a few mm / degrees.
#include "llvrbodyestimatemath.h"

#include <cstdio>

using namespace slest;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static Input headAt(const V3& eye, float yaw, float pitch = 0.f)
{
    Input in;
    in.head = eye;
    in.headRot = slfbt::qmul(slfbt::qyaw(yaw), slfbt::qaxis(V3(0.f, 1.f, 0.f), pitch));
    return in;
}

struct Run
{
    State st;
    Shape s = makeShape(1.6f);
    Params p;
    int steps = 0;
    bool wasStepping[2] = {};
    int concurrent = 0;
    Output o;
    void frame(const Input& in, float dt)
    {
        o = estimate(st, in, s, dt, p);
        int now = 0;
        for (int i = 0; i < 2; ++i)
        {
            if (st.foot[i].stepping && !wasStepping[i]) ++steps;
            wasStepping[i] = st.foot[i].stepping;
            now += st.foot[i].stepping ? 1 : 0;
        }
        concurrent = now > concurrent ? now : concurrent;
    }
};

static void testHeading()
{
    const Params p;
    float h = 0.f;
    // A 30 deg glance for 0.3 s: the body barely turns.
    for (int i = 0; i < 27; ++i) h = stepHeading(h, 0.5236f, 1.f / 90.f, p);
    CHECK(h < 0.07f, "glance: body turns little (%f)", h);
    // A 100 deg turn: within the budget (+ a little) after 0.4 s.
    h = 0.f;
    for (int i = 0; i < 36; ++i) h = stepHeading(h, 1.745f, 1.f / 90.f, p);
    CHECK(1.745f - h < p.twistBudget + 0.06f, "turn: body within the budget (%f behind)", 1.745f - h);
    // Holding the head turned 30 deg for 4 s: the body settles most of the way.
    h = 0.f;
    for (int i = 0; i < 360; ++i) h = stepHeading(h, 0.5236f, 1.f / 90.f, p);
    CHECK(h > 0.4f, "held glance: body settles (%f)", h);
    // Wrap-around: from 170 to -170 deg is a 20 deg turn, not 340.
    h = stepHeading(2.967f, -2.967f, 0.1f, p);
    CHECK(std::fabs(wrapAngle(h - 2.967f)) < 0.05f, "wrap");
    // The hands nudge the heading toward them.
    const V3 head(0.f, 0.f, 1.6f), l(0.4f, 0.4f, 1.1f), r(0.4f, 0.2f, 1.1f);
    const float t = targetHeading(0.f, head, &l, &r, p);
    CHECK(t > 0.1f && t < 0.4f, "hands steer (%f)", t);
}

static void testCrouch()
{
    Run r;
    const float dt = 1.f / 90.f;
    for (int i = 0; i < 90; ++i) r.frame(headAt(V3(0.f, 0.f, 1.6f), 0.f), dt);
    const V3 feet0 = r.st.foot[0].pos, feet1 = r.st.foot[1].pos;
    const float hips0 = r.o.hips.z;
    CHECK(std::fabs(hips0 - r.s.pelvisH) < 0.01f, "standing: hips at standing height (%f vs %f)", hips0, r.s.pelvisH);
    CHECK(r.steps == 0, "standing still: no steps (%d)", r.steps);
    // Crouch 0.5 m over 1 s, hold 1 s.
    for (int i = 0; i < 180; ++i)
    {
        const float k = i < 90 ? (float)i / 90.f : 1.f;
        r.frame(headAt(V3(0.f, 0.f, 1.6f - 0.5f * k), 0.f), dt);
    }
    CHECK(std::fabs((hips0 - r.o.hips.z) - 0.5f) < 0.02f, "hips drop with the head (%f)", hips0 - r.o.hips.z);
    CHECK(slfbt::length(r.st.foot[0].pos - feet0) < 1e-4f && slfbt::length(r.st.foot[1].pos - feet1) < 1e-4f, "feet planted");
    CHECK(r.steps == 0, "crouch: no steps (%d)", r.steps);
    CHECK(std::fabs(r.o.ankle[0].z - r.s.ankleH) < 1e-4f, "ankles on the floor");
    // Shuffling the head 25 cm sideways deep in the crouch: still no steps.
    for (int i = 0; i < 90; ++i) r.frame(headAt(V3(0.f, 0.25f * (float)i / 90.f, 1.0f), 0.f), dt);
    CHECK(r.steps == 0, "deep crouch: feet stay (%d)", r.steps);
    // Never above standing: on tiptoe (head 5 cm up) the hips stay at standing height.
    Run t;
    for (int i = 0; i < 90; ++i) t.frame(headAt(V3(0.f, 0.f, 1.65f), 0.f), dt);
    CHECK(t.o.hips.z <= t.s.pelvisH + 1e-4f, "hips never above standing");
}

static void testLean()
{
    Run r;
    const float dt = 1.f / 90.f;
    for (int i = 0; i < 90; ++i) r.frame(headAt(V3(0.f, 0.f, 1.6f), 0.f), dt);
    const V3 mid = (r.st.foot[0].pos + r.st.foot[1].pos) * 0.5f;
    // Bend forward 45 deg at the hips over 1 s: the head goes out and down.
    const V3 hips0 = r.o.hips;
    for (int i = 0; i <= 90; ++i)
    {
        // The torso and head turn about the hips by `a` (nose down: + about Y).
        const float a = 0.785f * (float)i / 90.f;
        const Q bend = slfbt::qaxis(V3(0.f, 1.f, 0.f), a);
        const V3 eye = hips0 + slfbt::qrot(bend, V3(0.f, 0.f, r.s.torso) + r.s.eyeFromNeck);
        r.frame(headAt(eye, 0.f, a), dt);
    }
    CHECK(r.steps == 0, "bending forward: no steps (%d)", r.steps);
    CHECK(slfbt::length(flat(r.o.hips - mid)) < 0.06f, "hips stay over the feet (%f)", slfbt::length(flat(r.o.hips - mid)));
}

static void testSteps()
{
    Run r;
    const float dt = 1.f / 90.f;
    for (int i = 0; i < 45; ++i) r.frame(headAt(V3(0.f, 0.f, 1.6f), 0.f), dt);
    // Walk forward 1 m over 2 s.
    int stepStarts = 0;
    bool lifted = false;
    for (int i = 0; i < 180; ++i)
    {
        r.frame(headAt(V3((float)i / 180.f, 0.f, 1.6f), 0.f), dt);
        lifted = lifted || r.o.ankle[0].z > r.s.ankleH + 0.03f || r.o.ankle[1].z > r.s.ankleH + 0.03f;
    }
    stepStarts = r.steps;
    for (int i = 0; i < 90; ++i) r.frame(headAt(V3(1.f, 0.f, 1.6f), 0.f), dt);   // settle
    CHECK(stepStarts >= 3 && stepStarts <= 9, "walking 1 m steps (%d)", stepStarts);
    CHECK(r.concurrent == 1, "one foot at a time (%d)", r.concurrent);
    CHECK(lifted, "steps lift the foot");
    const V3 mid = (r.st.foot[0].pos + r.st.foot[1].pos) * 0.5f;
    const float neckX = 1.f - r.s.eyeFromNeck.x;   // (the hips stand under the neck, behind the eyes)
    CHECK(std::fabs(mid.x - neckX) < 0.07f, "feet followed the body and settled under it (%f vs %f)", mid.x, neckX);
    CHECK(std::fabs(r.st.foot[0].pos.y - r.st.foot[1].pos.y) > 0.15f, "stance kept (%f)", r.st.foot[0].pos.y - r.st.foot[1].pos.y);
    // A step lasts stepTime.
    Run q;
    for (int i = 0; i < 45; ++i) q.frame(headAt(V3(0.f, 0.f, 1.6f), 0.f), dt);
    for (int i = 0; i < 3; ++i) q.frame(headAt(V3(0.3f, 0.f, 1.6f), 0.f), dt);   // a sudden 30 cm shift
    int frames = 0;
    const int first = q.st.foot[0].stepping ? 0 : 1;
    while (q.st.foot[first].stepping && frames < 500)
    {
        q.frame(headAt(V3(0.3f, 0.f, 1.6f), 0.f), dt);
        ++frames;
    }
    CHECK(frames >= 25 && frames <= 32, "a step lasts ~0.32 s (%d frames)", frames);
    // Small sway (5 cm) never steps.
    Run w;
    for (int i = 0; i < 360; ++i) w.frame(headAt(V3(0.05f * std::sin((float)i * 0.05f), 0.f, 1.6f), 0.f), dt);
    CHECK(w.steps == 0, "sway: no steps (%d)", w.steps);
    // Turning the body 120 deg in place: the feet step round.
    Run t;
    for (int i = 0; i < 45; ++i) t.frame(headAt(V3(0.f, 0.f, 1.6f), 0.f), dt);
    for (int i = 0; i < 270; ++i) t.frame(headAt(V3(0.f, 0.f, 1.6f), 2.094f * std::min(1.f, (float)i / 90.f)), dt);
    CHECK(t.steps >= 2, "turning steps (%d)", t.steps);
    CHECK(std::fabs(wrapAngle(t.st.foot[0].yaw - t.st.heading)) < 0.75f && std::fabs(wrapAngle(t.st.foot[1].yaw - t.st.heading)) < 0.75f,
          "feet turned with the body");
}

static void testRate()
{
    // The same walk-and-turn at 90 and 144 Hz.
    Run a, b;
    for (int i = 0; i < 270; ++i)
    {
        const float t = (float)i / 90.f;
        a.frame(headAt(V3(0.4f * t, 0.1f * t, 1.6f - 0.1f * t), 0.6f * t), 1.f / 90.f);
    }
    for (int i = 0; i < 432; ++i)
    {
        const float t = (float)i / 144.f;
        b.frame(headAt(V3(0.4f * t, 0.1f * t, 1.6f - 0.1f * t), 0.6f * t), 1.f / 144.f);
    }
    CHECK(std::fabs(wrapAngle(a.st.heading - b.st.heading)) < 0.03f, "heading 90 vs 144 Hz (%f)", a.st.heading - b.st.heading);
    CHECK(slfbt::length(a.o.hips - b.o.hips) < 0.01f, "hips 90 vs 144 Hz (%f)", slfbt::length(a.o.hips - b.o.hips));
    const V3 ma = (a.st.foot[0].pos + a.st.foot[1].pos) * 0.5f, mb = (b.st.foot[0].pos + b.st.foot[1].pos) * 0.5f;
    CHECK(slfbt::length(ma - mb) < 0.08f, "feet 90 vs 144 Hz (%f)", slfbt::length(ma - mb));
    CHECK(std::abs(a.steps - b.steps) <= 1, "steps 90 vs 144 Hz (%d vs %d)", a.steps, b.steps);
}

static void testMoveGate()
{
    const float dt = 1.f / 90.f;
    // 45 s of a standing avatar whose sim speed jitters 0.0-0.55 m/s frame to frame (the owner's
    // log: 205 flips in 45 min with a plain 0.3 m/s test): never or almost never "walking".
    unsigned seed = 7u;
    auto rnd01 = [&]() {
        seed = seed * 1103515245u + 12345u;
        return (float)((seed >> 8) & 0xFFFF) / 65535.f;
    };
    MoveGate g;
    int flips = 0;
    bool was = false;
    for (int i = 0; i < 90 * 45; ++i)
    {
        // Mostly small, now and then a spike past 0.4 for a frame or two.
        const float v = rnd01() < 0.05f ? 0.45f + 0.1f * rnd01() : 0.25f * rnd01();
        const bool now = stepMoveGate(g, false, v, dt);
        flips += now != was;
        was = now;
    }
    CHECK(flips == 0, "jittery standing speed: no walking (%d flips)", flips);
    // A real walk: the stick for 3 s at 1.5 m/s, then standing: one enter, one leave.
    MoveGate w;
    flips = 0;
    was = false;
    int enterFrame = -1, leaveFrame = -1;
    for (int i = 0; i < 90 * 6; ++i)
    {
        const bool stick = i >= 45 && i < 45 + 270;
        const float v = stick ? 1.5f : (i >= 45 && i < 45 + 270 + 20 ? 0.6f : 0.05f);   // (slows over ~0.2 s)
        const bool now = stepMoveGate(w, stick, v, dt);
        if (now != was)
        {
            ++flips;
            if (now) enterFrame = i;
            else leaveFrame = i;
        }
        was = now;
    }
    CHECK(flips == 2, "walk then stand: one enter, one leave (%d)", flips);
    CHECK(enterFrame - 45 >= 12 && enterFrame - 45 <= 15, "enters after 0.15 s (%d frames)", enterFrame - 45);
    CHECK(leaveFrame - (45 + 270 + 20) >= 35 && leaveFrame - (45 + 270 + 20) <= 38, "leaves 0.4 s after slowing (%d frames)",
          leaveFrame - (45 + 270 + 20));
    // A brief stick tap (0.1 s) does not take the legs.
    MoveGate t;
    bool any = false;
    for (int i = 0; i < 90; ++i) any = stepMoveGate(t, i < 9, 0.f, dt) || any;
    CHECK(!any, "a 0.1 s tap: no walking");
    // Speed hovering between leave and enter while walking keeps it walking (hysteresis).
    MoveGate h;
    for (int i = 0; i < 30; ++i) stepMoveGate(h, true, 1.f, dt);
    for (int i = 0; i < 300; ++i) stepMoveGate(h, false, 0.25f, dt);
    CHECK(h.moving, "hysteresis: 0.25 m/s keeps walking");
    // The same at 144 Hz: enters after 0.15 s.
    MoveGate f;
    int frames = 0;
    while (!stepMoveGate(f, true, 1.f, 1.f / 144.f) && frames < 500) ++frames;
    CHECK(frames >= 20 && frames <= 22, "144 Hz: 0.15 s (%d frames)", frames);
}

// #1584: with the avatar's facing given (VR), a head turn inside the turn limit leaves the hips
// and feet alone; the avatar turning brings them round (feet stepping).
static void testBodyYaw()
{
    Run r;
    const float dt = 1.f / 90.f;
    // The head turns about the neck (the eyes swing with it), as a real one does.
    const V3 neck = V3(0.f, 0.f, 1.6f) - r.s.eyeFromNeck;
    auto at = [&](float headYaw, float bodyYaw) {
        Input in = headAt(neck + slfbt::qrot(slfbt::qyaw(headYaw), r.s.eyeFromNeck), headYaw);
        in.haveBodyYaw = true;
        in.bodyYaw = bodyYaw;
        return in;
    };
    for (int i = 0; i < 45; ++i) r.frame(at(0.f, 0.f), dt);
    // Head turns 60 deg over 0.5 s and holds 3 s: the body stays.
    for (int i = 0; i < 315; ++i) r.frame(at(1.047f * std::min(1.f, (float)i / 45.f), 0.f), dt);
    CHECK(std::fabs(r.st.heading) < 1e-3f, "head turn: the hips keep the avatar's facing (%f)", r.st.heading);
    CHECK(r.steps == 0, "head turn: no steps (%d)", r.steps);
    // The avatar turns 90 deg (bodyFollowHead, over ~0.6 s): the hips follow, the feet step round.
    for (int i = 0; i < 180; ++i) r.frame(at(1.571f, 1.571f * std::min(1.f, (float)i / 54.f)), dt);
    CHECK(std::fabs(wrapAngle(r.st.heading - 1.571f)) < 0.02f, "avatar turned: hips face it (%f)", r.st.heading);
    CHECK(r.steps >= 2, "avatar turned: feet step round (%d)", r.steps);
}

int main()
{
    testBodyYaw();
    testMoveGate();
    testHeading();
    testCrouch();
    testLean();
    testSteps();
    testRate();
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
