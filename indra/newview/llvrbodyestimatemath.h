/**
 * @file llvrbodyestimatemath.h
 * @brief #1586 VR body estimate from the headset and the two controllers (3-point): the pure
 *        math (no viewer types), shared by LLVRBodyEstimate and scripts/check/test_vr_estimate.cpp
 *        (checks/vr_estimate.py, host compiler only).
 *
 * Body space as llvrbodymath.h (the room in SL axes, z above the floor, metres of YOUR body). Each
 * frame from the head pose (and the hands when tracked):
 *   heading  the body's facing: smoothed toward the head's heading (nudged toward the hands), with
 *            a torso-twist budget -- the head may turn `twistBudget` from the body before the body
 *            follows (quickly past the budget, slowly within it), so a glance never turns the hips.
 *   hips     under the neck while you stand tall; the hips stay over the feet while you bend or
 *            crouch, as far as the torso's length allows (bend forward: the head goes out and down,
 *            the hips stay; crouch: the hips drop straight down); never above standing height.
 *   feet     planted in the room. A foot steps (a short eased step with a small lift) when its
 *            spot under the hips has drifted past `stepDist`, or the body has turned past
 *            `stepTurn` from it; one foot at a time, the further first; no steps in a deep crouch.
 * Everything is per second (dt), so 90 Hz and 144 Hz give the same motion.
 */
#pragma once

#include "llvrbodymath.h"

#include <algorithm>

namespace slest
{
using slfbt::Q;
using slfbt::V3;

constexpr float kPi = 3.14159265f;

inline float wrapAngle(float a)
{
    while (a > kPi) a -= 2.f * kPi;
    while (a < -kPi) a += 2.f * kPi;
    return a;
}
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float smoothstep(float t)
{
    t = clampf(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
inline V3 flat(const V3& v) { return V3(v.x, v.y, 0.f); }

struct Params
{
    float twistBudget = 0.6f;     // radians the head may turn from the body before it follows
    float followTau = 0.12f;      // s: the body catching up past the budget
    float driftTau = 2.5f;        // s: the body settling toward the head within the budget
    float handWeight = 0.3f;      // how much the hands' direction steers the heading
    float stepDist = 0.17f;       // m (per 1.6 m of eye height): a foot's spot drifted this far steps
    float stepTurn = 0.7f;        // radians: the body turned this far from a foot steps it
    float stepTime = 0.32f;       // s per step
    float stepLift = 0.07f;       // m (per 1.6 m) the foot lifts mid-step
    float stance = 0.11f;         // m (per 1.6 m): each foot from the middle, sideways
    float crouchNoStep = 0.7f;    // hips below this share of their standing height: feet stay planted
    float settleAfter = 0.4f;     // s the body has been still before a settle step
    float settleNeed = 0.35f;     // ... for a foot this far off (share of stepDist / stepTurn)
    float stillSpeed = 0.12f;     // m/s: the head slower than this is "still"
    float hipsTilt = 0.5f;        // share of the torso's lean the hips take (the spine bends the rest)
};

// Your body's proportions in body space, from your standing eye height (slfbt::Proportions).
struct Shape
{
    float eyeH = 1.6f;
    float scale = 1.f;            // eyeH / 1.6
    float pelvisH = 0.896f;       // the hips' standing height
    float neckH = 1.464f;         // the neck's standing height
    float torso = 0.568f;         // hips -> neck
    float ankleH = 0.088f;
    V3 eyeFromNeck;               // the eye point from the neck, in the head's frame
};
inline Shape makeShape(float eyeH, const slfbt::Proportions& pr = slfbt::Proportions())
{
    Shape s;
    s.eyeH = eyeH > 0.5f ? eyeH : 1.6f;
    s.scale = s.eyeH / 1.6f;
    s.eyeFromNeck = V3(0.1f, 0.f, 0.136f) * s.scale;   // (LLVRCamera's eye point over the SL neck)
    s.neckH = s.eyeH - s.eyeFromNeck.z;
    s.pelvisH = pr.pelvis * s.eyeH;
    s.torso = s.neckH - s.pelvisH;
    s.ankleH = pr.ankle * s.eyeH;
    return s;
}

// ---- Walking (thumbstick locomotion) gate ---------------------------------------------------------
// While the avatar walks / flies with the sticks the animations own the legs; the estimate takes
// them back once it stands. The avatar's speed comes from the sim and jitters (a standing avatar
// reads 0.1-0.5 m/s now and then), so the gate has hysteresis and a debounce: it enters on the
// movement keys / stick (intent) or a speed above enterSpeed held for enterTime, and leaves only
// after leaveTime with no intent and the speed under leaveSpeed. (Flapping re-plants the feet.)
struct MoveParams
{
    float enterSpeed = 0.4f;   // m/s
    float enterTime = 0.15f;   // s
    float leaveSpeed = 0.15f;  // m/s
    float leaveTime = 0.4f;    // s
};
struct MoveGate
{
    bool moving = false;
    float enterFor = 0.f, leaveFor = 0.f;
};
inline bool stepMoveGate(MoveGate& g, bool intent, float speed, float dt, const MoveParams& p = MoveParams())
{
    if (!g.moving)
    {
        g.enterFor = (intent || speed > p.enterSpeed) ? g.enterFor + dt : 0.f;
        if (g.enterFor >= p.enterTime)
        {
            g.moving = true;
            g.leaveFor = 0.f;
        }
    }
    else
    {
        g.leaveFor = (!intent && speed < p.leaveSpeed) ? g.leaveFor + dt : 0.f;
        if (g.leaveFor >= p.leaveTime)
        {
            g.moving = false;
            g.enterFor = 0.f;
        }
    }
    return g.moving;
}

// ---- Heading -------------------------------------------------------------------------------------
// The heading the body wants: the head's, nudged toward the hands when both are out in front.
inline float targetHeading(float headYaw, const V3& head, const V3* hand0, const V3* hand1, const Params& p)
{
    if (!hand0 || !hand1) return headYaw;
    const V3 mid = flat((*hand0 + *hand1) * 0.5f - head);
    if (slfbt::length(mid) < 0.15f) return headYaw;
    const float handsYaw = std::atan2(mid.y, mid.x);
    const float d = wrapAngle(handsYaw - headYaw);
    if (std::fabs(d) > 1.2f) return headYaw;   // (hands behind / far to the side: ignored)
    return wrapAngle(headYaw + d * p.handWeight);
}
// One frame of the body's heading toward `target`: the part past the budget fast, the rest slowly.
inline float stepHeading(float heading, float target, float dt, const Params& p)
{
    const float d = wrapAngle(target - heading);
    const float over = std::fabs(d) > p.twistBudget ? d - (d > 0.f ? p.twistBudget : -p.twistBudget) : 0.f;
    const float within = d - over;
    const float aFast = p.followTau > 1e-4f ? 1.f - std::exp(-dt / p.followTau) : 1.f;
    const float aSlow = p.driftTau > 1e-4f ? 1.f - std::exp(-dt / p.driftTau) : 1.f;
    return wrapAngle(heading + over * aFast + within * aSlow);
}

// ---- Hips ----------------------------------------------------------------------------------------
// The hips under a neck point, kept over `anchor` (the feet's middle) as far as the torso allows:
// the torso hangs from the neck, the hips no higher than standing.
inline V3 hipsUnderNeck(const V3& neck, const V3& anchor, const Shape& s)
{
    const float rise = neck.z - s.pelvisH;   // the neck above standing hip height
    const float reach = rise >= s.torso ? 0.f : std::sqrt(s.torso * s.torso - (rise > 0.f ? rise * rise : 0.f));
    V3 off = flat(anchor - neck);
    const float d = slfbt::length(off);
    if (d > reach) off = d > 1e-6f ? off * (reach / d) : V3();
    const float dh = slfbt::length(off);
    float z = neck.z - std::sqrt(std::max(s.torso * s.torso - dh * dh, 0.f));
    z = clampf(z, 0.25f * s.pelvisH, s.pelvisH);
    return V3(neck.x + off.x, neck.y + off.y, z);
}

// ---- Feet ----------------------------------------------------------------------------------------
struct Foot
{
    V3 pos;                 // the ankle on the floor (z = 0: the caller adds the ankle height / lift)
    float yaw = 0.f;
    bool stepping = false;
    float t = 0.f;          // 0..1 through the step
    V3 from, to;
    float fromYaw = 0.f, toYaw = 0.f;
};
// A foot's spot under the hips: sideways from the hips by the stance, facing the heading.
inline V3 footHome(const V3& hips, float heading, int side, const Shape& s, const Params& p)
{
    const V3 left(-std::sin(heading), std::cos(heading), 0.f);
    const float sd = side == 0 ? 1.f : -1.f;
    return V3(hips.x, hips.y, 0.f) + left * (sd * p.stance * s.scale);
}
// How much a planted foot wants to step (0: no; above 1: past a threshold).
inline float stepNeed(const Foot& f, const V3& home, float heading, const Shape& s, const Params& p)
{
    const float drift = slfbt::length(flat(home - f.pos)) / (p.stepDist * s.scale);
    const float turn = std::fabs(wrapAngle(heading - f.yaw)) / p.stepTurn;
    return std::max(drift, turn);
}

struct State
{
    bool init = false;
    float heading = 0.f;
    Foot foot[2];
    V3 hips;
    V3 lastHead;
    float stillFor = 0.f;   // seconds the head has moved slower than stillSpeed
};
struct Input
{
    V3 head;                // the eye point, body space
    Q headRot;
    bool hand[2] = {};
    V3 handPos[2];
    // #1584 / #1586: the body's heading from outside (in VR: the avatar's own facing in the room,
    // which LLVRCamera::bodyFollowHead turns only past SlipstreamVRBodyTurnLimit). Then that is the
    // one body heading: the hips face it (eased over followTau), head turns move only the head,
    // neck and spine. Without it the estimate keeps its own heading (twist budget).
    bool haveBodyYaw = false;
    float bodyYaw = 0.f;
};
struct Output
{
    V3 hips;
    Q hipsRot;
    V3 ankle[2];            // with the ankle height and the step's lift
    float footYaw[2] = {};
    bool steppingAny = false;
};

inline Output estimate(State& st, const Input& in, const Shape& s, float dt, const Params& p)
{
    Output out;
    const float headYaw = slfbt::yawOf(in.headRot);
    const V3 neck = in.head - slfbt::qrot(in.headRot, s.eyeFromNeck);
    const float target = in.haveBodyYaw ? in.bodyYaw
                                        : targetHeading(headYaw, in.head, in.hand[0] ? &in.handPos[0] : nullptr,
                                                        in.hand[1] ? &in.handPos[1] : nullptr, p);
    // A jump (recentre, a new session, tracking back far away): start over under the head.
    if (!st.init || slfbt::length(in.head - st.lastHead) > 1.f)
    {
        st = State{};
        st.init = true;
        st.heading = target;
        st.hips = hipsUnderNeck(neck, flat(neck), s);
        for (int i = 0; i < 2; ++i)
        {
            st.foot[i].pos = footHome(st.hips, st.heading, i, s, p);
            st.foot[i].yaw = st.heading;
        }
    }
    const float speed = dt > 1e-5f ? slfbt::length(flat(in.head - st.lastHead)) / dt : 0.f;
    st.stillFor = speed < p.stillSpeed ? st.stillFor + dt : 0.f;
    st.lastHead = in.head;
    if (in.haveBodyYaw)
    {
        // The avatar's facing: followed (it turns smoothly already), no budget of our own.
        const float a = p.followTau > 1e-4f ? 1.f - std::exp(-dt / p.followTau) : 1.f;
        st.heading = wrapAngle(st.heading + wrapAngle(target - st.heading) * a);
    }
    else
    {
        st.heading = stepHeading(st.heading, target, dt, p);
    }

    // Hips: over the feet's middle while bending / crouching, under the neck standing tall.
    const V3 anchor = (st.foot[0].pos + st.foot[1].pos) * 0.5f;
    st.hips = hipsUnderNeck(neck, anchor, s);

    // Feet: advance a step in progress; else maybe start one (the neediest foot).
    bool stepping = false;
    for (Foot& f : st.foot)
    {
        if (!f.stepping) continue;
        f.t += p.stepTime > 1e-4f ? dt / p.stepTime : 1.f;
        const float e = smoothstep(f.t);
        f.pos = slfbt::lerp(f.from, f.to, e);
        f.yaw = wrapAngle(f.fromYaw + wrapAngle(f.toYaw - f.fromYaw) * e);
        if (f.t >= 1.f)
        {
            f.stepping = false;
            f.pos = f.to;
            f.yaw = f.toYaw;
        }
        else
        {
            stepping = true;
        }
    }
    if (!stepping && st.hips.z >= p.crouchNoStep * s.pelvisH)
    {
        float need[2];
        V3 home[2];
        for (int i = 0; i < 2; ++i)
        {
            home[i] = footHome(st.hips, st.heading, i, s, p);
            need[i] = stepNeed(st.foot[i], home[i], st.heading, s, p);
        }
        const int i = need[0] >= need[1] ? 0 : 1;
        // Past the threshold; or, standing still a moment, a foot left a little off its spot
        // settles under the body (people do not stop with one foot trailing).
        if (need[i] > 1.f || (st.stillFor >= p.settleAfter && need[i] > p.settleNeed))
        {
            Foot& f = st.foot[i];
            f.stepping = true;
            f.t = 0.f;
            f.from = f.pos;
            f.fromYaw = f.yaw;
            // A little past the spot along the drift, so a slow walk does not step every frame.
            const V3 drift = flat(home[i] - f.pos);
            const float dl = slfbt::length(drift);
            f.to = home[i] + (dl > 1e-4f ? drift * (std::min(dl, 0.1f * s.scale) * 0.3f / dl) : V3());
            f.toYaw = st.heading;
            stepping = true;
        }
    }

    out.hips = st.hips;
    // The hips face the heading and lean a share of the torso's lean (the spine bends the rest).
    const V3 up = slfbt::normalized(slfbt::lerp(V3(0.f, 0.f, 1.f), slfbt::normalized(neck - st.hips, V3(0.f, 0.f, 1.f)), p.hipsTilt),
                                    V3(0.f, 0.f, 1.f));
    const V3 fwd(std::cos(st.heading), std::sin(st.heading), 0.f);
    out.hipsRot = slfbt::qalignPair(V3(0.f, 0.f, 1.f), V3(1.f, 0.f, 0.f), up, fwd);
    for (int i = 0; i < 2; ++i)
    {
        const Foot& f = st.foot[i];
        const float lift = f.stepping ? p.stepLift * s.scale * std::sin(kPi * clampf(f.t, 0.f, 1.f)) : 0.f;
        out.ankle[i] = V3(f.pos.x, f.pos.y, s.ankleH + lift);
        out.footYaw[i] = f.yaw;
    }
    out.steppingAny = stepping;
    return out;
}
} // namespace slest
