// Fixtures for the VR body share's send decisions (#1584; indra/newview/llvrbodysharemath.h), run
// by scripts/check/checks/vr_body_share.py (host compiler only, no viewer build).
//   DEADBAND   a quiet bone needs more than startRot to send, a moving one more than keepRot.
//   SETTLE     a bone that stops sends its final value once (when off by more than settleRot).
//   RATE       constant motion of every bone: at most one line per tick (4/s at 0.25 s ticks) and
//              within the byte budget; the receivers' ease follows the line interval.
//   STILL      motion then stillness: lines stop (one settle), then only the 15 s keyframe.
//   JITTER     a still pose with sensor jitter just under the start threshold sends nothing; a
//              moving bone with jitter around the keep threshold stops once the motion does.
//   SET        a bone that stops being driven forces a line (its release) and resends when back.
#include "llvrbodysharemath.h"

#include <cstdio>
#include <cstdlib>

using namespace slshare;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static Q rotZ(float deg) { return slfbt::qaxis(V3(0.f, 0.f, 1.f), deg * kDeg); }
static Q rotX(float deg) { return slfbt::qaxis(V3(1.f, 0.f, 0.f), deg * kDeg); }

// A deterministic pseudo-random in [-1, 1].
static unsigned g_seed = 12345u;
static float rnd()
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (float)((g_seed >> 8) & 0xFFFF) / 32767.5f - 1.f;
}

static void testDeadband()
{
    const DeadbandParams p;
    BoneTrack b;
    CHECK(evaluate(b, Q(), V3(), false, 0.25f, p) == SEND, "first value sends");
    commit(b, Q(), V3(), SEND);
    CHECK(evaluate(b, rotZ(3.f), V3(), false, 0.25f, p) == HOLD, "quiet: 3 deg holds");
    CHECK(evaluate(b, rotZ(4.5f), V3(), false, 0.25f, p) == SEND, "quiet: 4.5 deg starts");
    commit(b, rotZ(4.5f), V3(), SEND);
    CHECK(b.moving, "moving");
    CHECK(evaluate(b, rotZ(7.f), V3(), false, 0.25f, p) == SEND, "moving: 2.5 deg sends");
    commit(b, rotZ(7.f), V3(), SEND);
    CHECK(evaluate(b, rotZ(8.5f), V3(), false, 0.25f, p) == HOLD, "moving: 1.5 deg holds");
    // Position deadband (the hips): 1 cm moving, 2 cm quiet.
    BoneTrack h;
    commit(h, Q(), V3(), SEND);
    CHECK(evaluate(h, Q(), V3(0.015f, 0.f, 0.f), true, 0.25f, p) == HOLD, "quiet: 1.5 cm holds");
    CHECK(evaluate(h, Q(), V3(0.025f, 0.f, 0.f), true, 0.25f, p) == SEND, "quiet: 2.5 cm starts");
    commit(h, Q(), V3(0.025f, 0.f, 0.f), SEND);
    CHECK(evaluate(h, Q(), V3(0.037f, 0.f, 0.f), true, 0.25f, p) == SEND, "moving: 1.2 cm sends");
}

static void testSettle()
{
    const DeadbandParams p;
    BoneTrack b;
    commit(b, Q(), V3(), SEND);
    evaluate(b, rotZ(5.f), V3(), false, 0.25f, p);
    commit(b, rotZ(5.f), V3(), SEND);
    // Creeps 1.5 deg further (under keep) and stops.
    int settles = 0, sends = 0;
    for (int i = 0; i < 12; ++i)
    {
        const Decision d = evaluate(b, rotZ(6.5f), V3(), false, 0.25f, p);
        settles += d == SETTLE;
        sends += d == SEND;
        if (d != HOLD) commit(b, rotZ(6.5f), V3(), d);
    }
    CHECK(settles == 1 && sends == 0, "one settle (%d settles, %d sends)", settles, sends);
    CHECK(rotDelta(b.rot, rotZ(6.5f)) < 1e-4f && !b.moving, "settled at the final value");
}

struct Sim
{
    static constexpr int N = 23;   // the body layer's bones (pelvis, spine, legs, arms, neck, head)
    BoneTrack tracks[N];
    Q rot[N];
    V3 pos[N];
    bool hasPos[N] = {};
    bool driven[N];
    RateState rs;
    RateParams rp;
    DeadbandParams dp[N];
    int lines = 0, fulls = 0;
    float bytes = 0.f;
    Plan last;
    Sim()
    {
        hasPos[0] = true;
        for (bool& d : driven) d = true;
    }
    Plan step(float dt, bool setChanged = false)
    {
        const Plan p = plan(tracks, rot, pos, hasPos, driven, N, dt, setChanged, rs, rp, dp);
        last = p;
        if (p.kind)
        {
            ++lines;
            fulls += p.kind == 2;
            bytes += p.bytes;
        }
        return p;
    }
};

static void testRate()
{
    Sim s;
    const float dt = 0.25f;
    float minEase = 1.f, maxEase = 0.f;
    // 10 s, every bone turning 40 deg/s (well past the deadband every tick).
    for (int t = 0; t < 40; ++t)
    {
        for (int i = 0; i < Sim::N; ++i) s.rot[i] = rotZ(40.f * dt * (float)t + (float)i);
        s.pos[0] = V3(0.1f * (float)t, 0.f, 0.f);
        const Plan p = s.step(dt);
        if (p.kind == 1)
        {
            minEase = p.ease < minEase ? p.ease : minEase;
            maxEase = p.ease > maxEase ? p.ease : maxEase;
        }
    }
    CHECK(s.lines <= 40, "at most one line per tick (%d)", s.lines);
    CHECK(s.lines >= 30, "keeps up with constant motion (%d lines in 10 s)", s.lines);
    CHECK(s.bytes <= s.rp.bytesPerSec * 10.f + s.rp.burstBytes, "byte budget (%.0f bytes in 10 s)", s.bytes);
    CHECK(minEase >= 0.25f - 1e-4f && maxEase <= 0.5f + 1e-4f, "ease follows the interval (%f .. %f)", minEase, maxEase);
    // A faster tick never beats the line interval.
    Sim f;
    for (int t = 0; t < 300; ++t)
    {
        for (int i = 0; i < Sim::N; ++i) f.rot[i] = rotZ(60.f * 0.0333f * (float)t);
        f.step(0.0333f);
    }
    CHECK(f.lines <= (int)(10.f / f.rp.minInterval) + 1, "30 Hz ticks: lines capped by the interval (%d in 10 s)", f.lines);
    // A tight byte budget throttles below the interval cap.
    Sim b;
    b.rp.bytesPerSec = 300.f;
    b.rp.burstBytes = 600.f;
    b.rs.tokens = 600.f;
    for (int t = 0; t < 40; ++t)
    {
        for (int i = 0; i < Sim::N; ++i) b.rot[i] = rotZ(40.f * 0.25f * (float)t);
        b.step(0.25f);
    }
    CHECK(b.bytes <= 300.f * 10.f + 600.f, "tight budget kept (%.0f bytes)", b.bytes);
}

static void testStill()
{
    Sim s;
    const float dt = 0.25f;
    for (int t = 0; t < 8; ++t)   // 2 s of motion
    {
        for (int i = 0; i < Sim::N; ++i) s.rot[i] = rotX(10.f * (float)t + 0.7f);
        s.step(dt);
    }
    const int moving = s.lines;
    for (int i = 0; i < Sim::N; ++i) s.rot[i] = rotX(10.f * 7.f + 1.5f);   // a last small creep, then still
    int after = 0, fullsAfter = 0;
    for (int t = 0; t < 4 * 30; ++t)   // 30 s still
    {
        const Plan p = s.step(dt);
        if (p.kind) ++after;
        if (p.kind == 2) ++fullsAfter;
    }
    CHECK(moving >= 7, "moving sends (%d)", moving);
    CHECK(fullsAfter == 2, "keyframes every 15 s while still (%d)", fullsAfter);
    CHECK(after == fullsAfter + 1, "still: one settle line, then keyframes only (%d lines)", after);
}

static void testJitter()
{
    // A person standing still: every bone jitters +-1.9 deg around its pose (the worst IMU), the
    // hips +-9 mm. Nothing goes out after the first keyframe except the periodic one.
    Sim s;
    const float dt = 0.25f;
    for (int t = 0; t < 4 * 30; ++t)
    {
        for (int i = 0; i < Sim::N; ++i) s.rot[i] = slfbt::qmul(rotZ(20.f + (float)i), rotX(1.9f * rnd()));
        s.pos[0] = V3(0.009f * rnd(), 0.f, 1.f);
        s.step(dt);
    }
    CHECK(s.lines == s.fulls && s.fulls == 2, "still with jitter: keyframes only (%d lines, %d fulls)", s.lines, s.fulls);
    // A slow motion with jitter around the keep threshold: once the motion stops, the bone goes
    // quiet within quietAfter + a settle, and jitter does not wake it.
    Sim m;
    for (int i = 0; i < Sim::N; ++i) m.driven[i] = i == 5;
    for (int t = 0; t < 20; ++t)
    {
        m.rot[5] = slfbt::qmul(rotZ(3.f * (float)t), rotX(1.f * rnd()));
        m.step(dt);
    }
    const int before = m.lines;
    for (int t = 0; t < 40; ++t)   // 10 s still with jitter +-1.9 deg
    {
        m.rot[5] = slfbt::qmul(rotZ(57.f), rotX(1.9f * rnd()));
        m.step(dt);
    }
    CHECK(m.lines - before <= 3, "motion stopped: the jitter is quiet (%d lines after)", m.lines - before);
    CHECK(!m.tracks[5].moving, "bone quiet again");
}

static void testSet()
{
    Sim s;
    s.step(0.25f);   // keyframe
    const int before = s.lines;
    s.driven[7] = false;   // an arm faded back to the animation
    Plan p = s.step(0.25f, true);
    CHECK(p.kind == 1 && s.lines == before + 1, "a release forces a line");
    CHECK(!s.tracks[7].sent, "track reset");
    s.driven[7] = true;
    p = s.step(0.25f, true);
    CHECK(p.kind == 1 && p.bones == 1, "driven again: resent (%d bones)", p.bones);
}

// The body layer's 23 bones as the share classes them (pelvis, spine x6, legs x6, collars / arms
// x8, neck, head) plus 10 finger bones.
static const char* const kNames[33] = {"mPelvis", "mSpine1", "mSpine2", "mTorso", "mSpine3", "mSpine4", "mChest",
                                       "mHipLeft", "mKneeLeft", "mAnkleLeft", "mHipRight", "mKneeRight", "mAnkleRight",
                                       "mCollarLeft", "mShoulderLeft", "mElbowLeft", "mWristLeft", "mCollarRight",
                                       "mShoulderRight", "mElbowRight", "mWristRight", "mNeck", "mHead",
                                       "mHandIndex1Left", "mHandIndex2Left", "mHandMiddle1Left", "mHandMiddle2Left",
                                       "mHandThumb1Left", "mHandIndex1Right", "mHandIndex2Right", "mHandMiddle1Right",
                                       "mHandMiddle2Right", "mHandThumb1Right"};
struct Body
{
    static constexpr int N = 33;
    BoneTrack tracks[N];
    Q rot[N];
    V3 pos[N];
    bool hasPos[N] = {};
    bool driven[N];
    DeadbandParams dp[N];
    int cls[N];
    RateState rs;
    RateParams rp;
    int lines = 0, fulls = 0, headLines = 0;
    Body()
    {
        for (int i = 0; i < N; ++i)
        {
            driven[i] = true;
            cls[i] = boneClassOf(kNames[i]);
            dp[i] = deadbandFor(cls[i]);
        }
        hasPos[0] = true;
    }
    void step(float dt)
    {
        const Plan p = plan(tracks, rot, pos, hasPos, driven, N, dt, false, rs, rp, dp);
        if (!p.kind) return;
        ++lines;
        fulls += p.kind == 2;
        for (int k = 0; k < p.nTrig; ++k) headLines += cls[p.trig[k]] == BC_HEAD ? 1 : 0;
    }
};

static void testClasses()
{
    CHECK(boneClassOf("mHead") == BC_HEAD && boneClassOf("mNeck") == BC_HEAD, "head class");
    CHECK(boneClassOf("mHandIndex1Left") == BC_FINGER && boneClassOf("mHandThumb3Right") == BC_FINGER, "finger class");
    CHECK(boneClassOf("mChest") == BC_SPINE && boneClassOf("mSpine3") == BC_SPINE && boneClassOf("mPelvis") == BC_PELVIS, "spine / pelvis");
    CHECK(boneClassOf("mElbowLeft") == BC_LIMB && boneClassOf("mKneeRight") == BC_LIMB, "limbs");
}

// A VR user standing "still" on the 3-point estimate for 60 s: the head sways a few degrees, the
// spine / legs / hips follow it a little, the fingers flicker. Only the keyframes go out.
static void testStillUser()
{
    Body b;
    const float dt = 0.25f;
    for (int t = 0; t < 240; ++t)
    {
        const float time = dt * (float)t;
        const float sway = std::sin(time * 1.9f), sway2 = std::sin(time * 1.3f + 1.f);
        for (int i = 0; i < Body::N; ++i)
        {
            float deg = 1.5f;   // limbs / legs: the hips' sway through the leg IK
            if (b.cls[i] == BC_HEAD) deg = 2.5f;
            else if (b.cls[i] == BC_SPINE || b.cls[i] == BC_PELVIS) deg = 2.f;
            else if (b.cls[i] == BC_FINGER) deg = 3.5f * rnd();
            b.rot[i] = slfbt::qmul(rotZ(10.f * (float)i + deg * sway), rotX(0.7f * deg * sway2));
        }
        b.pos[0] = V3(0.012f * sway, 0.008f * sway2, 0.f);
        b.step(dt);
    }
    CHECK(b.lines == b.fulls, "still user: keyframes only (%d lines, %d keyframes in 60 s)", b.lines, b.fulls);
    CHECK(b.fulls <= 5, "keyframes every 15 s (%d)", b.fulls);
}

// Looking around slowly (the head turning 3 deg/s for 20 s): about one line a second, not four;
// the head bones are what trigger them (the debug list says so).
static void testSlowHead()
{
    Body b;
    const float dt = 0.25f;
    b.step(dt);   // keyframe
    const int before = b.lines;
    for (int t = 1; t <= 80; ++t)
    {
        b.rot[21] = rotZ(1.f * dt * (float)t);   // neck: a third
        b.rot[22] = rotZ(2.f * dt * (float)t);   // head: two thirds
        b.step(dt);
    }
    const float rate = (float)(b.lines - before) / 20.f;
    CHECK(rate <= 1.05f && rate >= 0.2f, "slow head turn: ~1 line/s at most (%f)", rate);
    CHECK(b.headLines >= b.lines - before, "the head bones triggered them (%d of %d)", b.headLines, b.lines - before);
}

// A quick head turn (60 deg in 0.5 s) or a hand swing goes out at the fast rate.
static void testFastMotion()
{
    Body b;
    const float dt = 0.25f;
    b.step(dt);
    const int before = b.lines;
    for (int t = 1; t <= 8; ++t)
    {
        b.rot[16] = rotZ(40.f * (float)t);   // a wrist swinging 160 deg/s
        b.step(dt);
    }
    CHECK(b.lines - before >= 7, "hand swing: every tick (%d of 8)", b.lines - before);
}

// A receiver's mPelvis as LLJoint holds it with no animation (FSJointState's base): the mesh's
// override, else where LLAvatarAppearance::buildCharacter put it -- at the root (SL-315); the
// skeleton default (0, 0, 1.067) is only the bind pose.
static V3 receiverBase(bool hasOverride, const V3& overridePos) { return hasOverride ? overridePos : V3(); }

// The pelvis offset reproduces the sender's pelvis on a receiver -- with a mesh's position
// override (a body that moves the pelvis) and without (the Open Pony, a human).
static void testPelvisOffset()
{
    const V3 def(0.f, 0.f, 1.067f);   // the skeleton default (bind pose): never the rest
    const V3 overPos(-0.2f, 0.f, 0.05f);
    const V3 drawn(-0.15f, 0.02f, 0.03f);   // the VR layer's pelvis (crouching a little)
    // With an override: base = the override, target = base + offset.
    const V3 got = receiverBase(true, overPos) + pelvisOffset(drawn, true, overPos);
    CHECK(slfbt::length(got - drawn) < 1e-5f, "override: the receiver's pelvis is the sender's");
    CHECK(slfbt::length(pelvisRest(true, overPos) - overPos) < 1e-6f, "override: the rest is the override");
    // No override (the human, the Open Pony): the rest is the root.
    const V3 got2 = receiverBase(false, overPos) + pelvisOffset(drawn, false, overPos);
    CHECK(slfbt::length(got2 - drawn) < 1e-5f, "no override: the receiver's pelvis is the sender's");
    CHECK(slfbt::length(pelvisRest(false, overPos)) < 1e-6f, "no override: the rest is the root, not the default");
    // #1592 the bipedal Open Pony (no pelvis override): the layer stands it up with the pelvis a
    // few cm above its four-legged rest. The old reference (the skeleton default) sent it 1.07 m low:
    // the partner saw everything below the pelvis in the ground.
    const V3 upright(-0.02f, 0.f, 0.028f);
    const V3 got3 = receiverBase(false, V3()) + pelvisOffset(upright, false, V3());
    CHECK(std::fabs(got3.z - upright.z) < 1e-5f, "bipedal pony: the receiver's pelvis height is the sender's (%f vs %f)", got3.z,
          upright.z);
    const V3 old = receiverBase(false, V3()) + (upright - def);
    CHECK(old.z < upright.z - 1.f, "bipedal pony: the old reference sank it over a metre (%f vs %f)", old.z, upright.z);
}

int main()
{
    testPelvisOffset();
    testClasses();
    testStillUser();
    testSlowHead();
    testFastMotion();
    testDeadband();
    testSettle();
    testRate();
    testStill();
    testJitter();
    testSet();
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
