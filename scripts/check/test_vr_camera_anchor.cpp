// Fixtures for the VR camera's locally owned anchor (#1607; indra/newview/llvrcameramath.h), run
// by scripts/check/checks/vr_camera_anchor.py (host C++ compiler only).
//   TURN     a snap / smooth turn keeps the head where it is (any room offset, angle, world
//            scale); turns compose; the rest of the room turns about the head; the old rule
//            (turning about the reference) moved the head by the arc this replaced.
//   ABSORB   the origin moving under an absorb leaves every room point where it was; the avatar
//            walking to the head drives the head's offset to zero with the view still.
//   DRAW     the draw offset puts the avatar's eyes on the head for any root yaw; a body turn
//            pivots the body about the head (the eyes never move).
//   REANCHOR the head onto the origin: offset zero, the view jumps by the old offset.
//   FOLLOW   start / stop hysteresis, the controls in the body's frame (forward, strafe,
//            diagonal), nudge when near, the tail after stopping, nothing when not allowed.
//   JUMP     locomotion is not a jump; a correction is; velocity explains fast motion.
#include "llvrcameramath.h"

#include <cmath>
#include <cstdio>

using namespace slvrcam;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static bool nearV(const V2& a, const V2& b, float eps) { return length(a - b) <= eps; }

static Mapping makeMap(float ox, float oy, float rx, float ry, float turn, float k)
{
    Mapping m;
    m.origin = V2(ox, oy);
    m.ref = V2(rx, ry);
    m.turn = turn;
    m.k = k;
    return m;
}

static void testTurn()
{
    const float scales[] = {1.f, 0.5f, 2.5f};
    const float angles[] = {0.5235988f, -0.5235988f, 1.5707963f, 3.f, -2.9f, 0.01f};
    const V2 heads[] = {V2(0.f, 0.f), V2(1.2f, -0.7f), V2(-2.f, 1.5f), V2(0.05f, 0.f)};
    for (float k : scales)
        for (float a : angles)
            for (const V2& h : heads)
            {
                Mapping m = makeMap(10.f, -4.f, 0.3f, 0.4f, 0.7f, k);
                const V2 before = toAgent(m, h);
                const V2 other(h.x + 0.5f, h.y - 0.25f);   // a hand / a foot near the head
                const V2 otherBefore = toAgent(m, other);
                turnAboutHead(m, h, a);
                CHECK(nearV(toAgent(m, h), before, 1e-4f), "head moved on a turn (k %.2f, angle %.2f, head %.2f %.2f)", k, a,
                      h.x, h.y);
                // Everything else turned about the head by a.
                const V2 expect = before + rot(otherBefore - before, a);
                CHECK(nearV(toAgent(m, other), expect, 1e-4f), "the room did not turn about the head (k %.2f, angle %.2f)", k, a);
                CHECK(std::fabs(wrapAngle(m.turn - (0.7f + a))) < 1e-5f, "turn not added");
            }
    // Turns compose: twelve 30 degree snaps are a full circle, the head never moving.
    Mapping m = makeMap(0.f, 0.f, 0.f, 0.f, 0.f, 1.f);
    const V2 h(0.8f, 0.6f);   // a metre from the reference
    const V2 start = toAgent(m, h);
    for (int i = 0; i < 12; ++i)
    {
        turnAboutHead(m, h, 0.5235988f);
        CHECK(nearV(toAgent(m, h), start, 1e-4f), "head drifted on snap %d", i);
    }
    CHECK(std::fabs(m.turn) < 1e-4f, "twelve snaps are not a full circle (%.5f)", m.turn);
    CHECK(nearV(m.ref, V2(0.f, 0.f), 1e-4f), "a full circle did not bring the reference back");
    // The rule this replaced: turning about the reference, a head 1 m from it swept a 30 degree
    // arc of ~0.52 m.
    Mapping old = makeMap(0.f, 0.f, 0.f, 0.f, 0.f, 1.f);
    const V2 o0 = toAgent(old, h);
    old.turn += 0.5235988f;
    CHECK(length(toAgent(old, h) - o0) > 0.5f, "fixture: the old rule's swing (%.3f m)", length(toAgent(old, h) - o0));
}

static void testAbsorb()
{
    Mapping m = makeMap(5.f, 5.f, 0.f, 0.f, 1.1f, 0.8f);
    const V2 pts[] = {V2(0.f, 0.f), V2(1.f, 2.f), V2(-0.4f, 0.3f)};
    V2 before[3];
    for (int i = 0; i < 3; ++i) before[i] = toAgent(m, pts[i]);
    const V2 dP(0.37f, -1.2f);
    m.origin = m.origin + dP;
    absorb(m, dP);
    for (int i = 0; i < 3; ++i) CHECK(nearV(toAgent(m, pts[i]), before[i], 1e-4f), "absorb moved room point %d", i);

    // The avatar walking to the head in steps: the head's offset goes to zero, the head's agent
    // position (the view) never moves.
    Mapping w = makeMap(0.f, 0.f, 0.f, 0.f, -0.6f, 1.f);
    const V2 head(0.9f, -0.5f);   // walked ~1 m about the room
    const V2 view = toAgent(w, head);
    for (int step = 0; step < 200; ++step)
    {
        const V2 off = headOffset(w, head);
        if (length(off) < 1e-3f) break;
        const V2 d = off * (length(off) > 0.02f ? 0.02f / length(off) : 1.f);   // 2 cm a frame toward the head
        w.origin = w.origin + d;
        absorb(w, d);
        CHECK(nearV(toAgent(w, head), view, 1e-4f), "the view moved while the avatar caught up (step %d)", step);
    }
    CHECK(length(headOffset(w, head)) < 1e-3f, "the avatar did not end under the head (%.4f m)", length(headOffset(w, head)));
    CHECK(nearV(w.origin, view, 1e-3f), "the collision shape did not end at the view");

    // Locomotion (no absorb): the view moves with the origin.
    Mapping l = makeMap(0.f, 0.f, 0.f, 0.f, 0.f, 1.f);
    const V2 v0 = toAgent(l, head);
    l.origin = l.origin + V2(1.f, 0.f);
    CHECK(nearV(toAgent(l, head), v0 + V2(1.f, 0.f), 1e-5f), "locomotion did not move the view");
}

static void testDraw()
{
    const V2 eyeLocal(0.15f, 0.02f);   // the eyes in front of the root
    const V2 headOff(0.3f, -0.2f);
    const V2 origin(10.f, 20.f);
    for (int i = 0; i < 16; ++i)
    {
        const float yaw = -3.f + 0.4f * (float)i;
        const V2 root = origin + drawOffset(headOff, eyeLocal, yaw);
        const V2 eyes = root + rot(eyeLocal, yaw);
        CHECK(nearV(eyes, origin + headOff, 1e-5f), "eyes not under the head at root yaw %.2f", yaw);
    }
    // A body turn moves the root on a circle about the head, not the eyes.
    const V2 r0 = drawOffset(V2(), eyeLocal, 0.f);
    const V2 r1 = drawOffset(V2(), eyeLocal, 1.2f);
    CHECK(std::fabs(length(r0) - length(r1)) < 1e-5f, "the root does not pivot about the head");
    CHECK(length(r0 - r1) > 0.1f, "fixture: the root should move on a body turn");
    // No room offset, no eye offset: no draw offset.
    CHECK(nearV(drawOffset(V2(), V2(), 0.7f), V2(), 1e-6f), "zero offset expected");
}

static void testReanchor()
{
    Mapping m = makeMap(1.f, 1.f, 0.f, 0.f, 0.3f, 1.f);
    const V2 head(0.6f, 0.8f);
    CHECK(length(headOffset(m, head)) > 0.9f, "fixture: offset before");
    reanchor(m, head);
    CHECK(length(headOffset(m, head)) < 1e-6f, "re-anchor left an offset");
    CHECK(nearV(toAgent(m, head), m.origin, 1e-6f), "re-anchor: the head is not on the origin");
}

static void testFollow()
{
    FollowConfig c;
    FollowState s;
    const float dt = 1.f / 90.f;
    // Inside the start distance: nothing.
    FollowCommand cmd = stepFollow(s, V2(0.2f, 0.f), 0.f, true, dt, c);
    CHECK(!s.active && cmd.at == 0 && cmd.left == 0 && !cmd.absorb, "moved inside the start distance");
    // Past it, straight ahead of a body facing +X: forward, walking (not nudge) far, no strafe.
    cmd = stepFollow(s, V2(0.8f, 0.f), 0.f, true, dt, c);
    CHECK(s.active && cmd.at == 1 && cmd.left == 0 && !cmd.nudge && cmd.absorb, "forward walk expected");
    // Between stop and start: keeps going (hysteresis), nudging when near.
    cmd = stepFollow(s, V2(0.2f, 0.f), 0.f, true, dt, c);
    CHECK(s.active && cmd.at == 1 && cmd.nudge, "hysteresis: should keep nudging forward");
    // The body facing +Y (yaw 90): a head at +X is to its right: strafe right, no at.
    cmd = stepFollow(s, V2(0.5f, 0.f), 1.5707963f, true, dt, c);
    CHECK(cmd.at == 0 && cmd.left == -1, "strafe right expected (at %d left %d)", cmd.at, cmd.left);
    // Behind and to the left: back + left.
    cmd = stepFollow(s, V2(-0.5f, 0.5f), 0.f, true, dt, c);
    CHECK(cmd.at == -1 && cmd.left == 1, "diagonal back-left expected (at %d left %d)", cmd.at, cmd.left);
    // Within stop: done, but the tail still absorbs.
    cmd = stepFollow(s, V2(0.05f, 0.f), 0.f, true, dt, c);
    CHECK(!s.active && cmd.at == 0 && cmd.left == 0 && cmd.absorb, "stop with a tail expected");
    float t = 0.f;
    while (cmd.absorb && t < 2.f)
    {
        cmd = stepFollow(s, V2(0.05f, 0.f), 0.f, true, dt, c);
        t += dt;
    }
    CHECK(!cmd.absorb && t > c.tail * 0.8f && t < c.tail * 1.5f, "tail length %.3f s", t);
    // Not allowed (stick walking, a blink): nothing, no absorb, no tail.
    FollowState s2;
    stepFollow(s2, V2(1.f, 0.f), 0.f, true, dt, c);
    cmd = stepFollow(s2, V2(1.f, 0.f), 0.f, false, dt, c);
    CHECK(!s2.active && cmd.at == 0 && !cmd.absorb, "not allowed must stop and not absorb");
    // A NaN offset never moves.
    FollowState s3;
    cmd = stepFollow(s3, V2(NAN, 0.f), 0.f, true, dt, c);
    CHECK(!s3.active && cmd.at == 0, "NaN moved");
}

static void testJump()
{
    const float dt = 1.f / 90.f;
    CHECK(!isJump(3.2f * dt, 3.2f, dt), "walking is not a jump");
    CHECK(!isJump(0.f, 0.f, dt), "standing still is not a jump");
    CHECK(isJump(0.8f, 0.f, dt), "a 0.8 m correction standing is a jump");
    CHECK(!isJump(1.4f, 15.f, 0.05f), "flying fast at 20 fps is not a jump");
    CHECK(isJump(6.f, 15.f, 0.05f), "6 m at 15 m/s in 50 ms is a jump");
    CHECK(!isJump(1.f, 60.f, dt, 5.f), "a seated vehicle's frame is not a jump");
}

int main()
{
    testTurn();
    testAbsorb();
    testDraw();
    testReanchor();
    testFollow();
    testJump();
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("vr camera anchor: all passed\n");
    return 0;
}
