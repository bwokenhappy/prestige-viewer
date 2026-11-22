/**
 * @file llvrbodysharemath.h
 * @brief #1584 VR body share: the pure send decisions (no viewer types), shared by LLVRBodyShare
 *        and the fixtures in scripts/check/test_vr_body_share.cpp (checks/vr_body_share.py).
 *
 * The VR body pose goes out as Pose Studio lines (P1 F / D, slposesync.h) through the helper:
 * llRegionSay, one bridge command per 0.2 s for every feature together, and each receiving helper
 * forwards at most 6 lines a second per sender and 12 for everyone (slipstream_helper.lsl). So
 * lines must be few, and a bone is only sent when it has really moved.
 *
 * Per bone (BoneTrack): a deadband against the value last SENT, with hysteresis (the numbers for
 * limbs; the neck / head, spine, hips and fingers get wider ones: deadbandFor) --
 *   quiet:  a change past startRot (4 deg) / startPos (2 cm) starts it moving (sent);
 *   moving: a change past keepRot (2 deg) / keepPos (1 cm) is sent;
 *   after quietAfter (0.6 s) without such a change it is quiet again, and its last value (if
 *   more than settleRot / settlePos off what was sent) goes out once as a settle, so a limb never
 *   rests visibly off where it stopped.
 * Why these numbers: 2 deg on a 0.6 m arm is ~2 cm at the hand -- under what anyone notices at
 * conversation distance once the receiver eases between lines; 4 deg to wake a quiet bone is
 * above controller / IMU jitter (Index / Vive ~0.1-0.3 deg, SlimeVR IMUs ~0.5-1 deg) and
 * breathing (the chest moves ~1-2 deg), so a person standing still sends nothing. 1 / 2 cm for the
 * hips: smaller than a step, larger than a tracker's noise (~1-3 mm).
 *
 * Per stream (RateState): at most one line per minInterval (0.2 s; SLPoseSync ticks every 0.25 s,
 * so 4 lines/s at most while moving), a token bucket on bytes (bytesPerSec 2400, burst 4800: a
 * full keyframe is ~300 bytes, ~600 with the fingers (#1587), a busy delta ~250-450; LSL chat lines
 * of that size cost the helpers per line, not per byte, so the bucket only stops a runaway), a
 * full keyframe every fullInterval (15 s) or on
 * request (a newcomer's Q1, a stream start); nearly still, nothing at all between keyframes.
 */
#pragma once

#include "llvrbodymath.h"

#include <algorithm>

namespace slshare
{
using slfbt::Q;
using slfbt::V3;

constexpr float kDeg = 0.017453293f;

struct DeadbandParams
{
    float keepRot = 2.f * kDeg;
    float startRot = 4.f * kDeg;
    float keepPos = 0.01f;
    float startPos = 0.02f;
    float settleRot = 0.5f * kDeg;
    float settlePos = 0.003f;
    float quietAfter = 0.6f;
};

// Per bone kind (the owner's first 45 min on the 3-point estimate: 3.6-3.9 lines/s all session, the
// neck / head -- which follow the headset, never still -- and the spine / hips the estimate hangs
// from the head among them):
//   limbs   arms, legs, collars: 2 / 4 deg (hands matter most)
//   spine   torso, chest, Bento spine: 3 / 5 deg
//   pelvis  3 / 5 deg; position 1.5 / 3 cm (it sways with the head on the estimate)
//   head    neck, head: 4 / 6 deg, settle 1 deg (a glance is ~10 deg; sway is 1-3)
//   finger  Bento fingers: 5 / 8 deg (capacitive sensing flickers a few degrees)
enum BoneClass { BC_LIMB = 0, BC_SPINE, BC_PELVIS, BC_HEAD, BC_FINGER, BC_COUNT };
inline DeadbandParams deadbandFor(int cls)
{
    DeadbandParams p;
    auto set = [&](float keep, float start, float settle) {
        p.keepRot = keep * kDeg;
        p.startRot = start * kDeg;
        p.settleRot = settle * kDeg;
    };
    switch (cls)
    {
    case BC_SPINE: set(3.f, 5.f, 0.75f); break;
    case BC_PELVIS:
        set(3.f, 5.f, 0.75f);
        p.keepPos = 0.015f;
        p.startPos = 0.03f;
        p.settlePos = 0.005f;
        break;
    case BC_HEAD: set(4.f, 6.f, 1.f); break;
    case BC_FINGER: set(5.f, 8.f, 1.5f); break;
    default: break;
    }
    return p;
}
inline bool startsWith(const char* s, const char* prefix)
{
    while (*prefix)
        if (*s++ != *prefix++) return false;
    return true;
}
// The kind of an SL joint (by name).
inline int boneClassOf(const char* name)
{
    if (!name) return BC_LIMB;
    if (startsWith(name, "mPelvis")) return BC_PELVIS;
    if (startsWith(name, "mHand")) return BC_FINGER;
    if (startsWith(name, "mNeck") || startsWith(name, "mHead")) return BC_HEAD;
    if (startsWith(name, "mTorso") || startsWith(name, "mChest") || startsWith(name, "mSpine")) return BC_SPINE;
    return BC_LIMB;
}

// The pelvis' position goes out as Pose Studio's offset: receivers add it to the joint's position
// when their pose motion started (FSJointState's base), which -- no animation translating it at
// that moment -- is the joint's REST position: a rigged mesh's joint position override when the
// avatar wears one, else where the skeleton put it: AT THE ROOT, (0, 0, 0)
// (LLAvatarAppearance::buildCharacter, SL-315; the root itself sits at the pelvis' height). The
// skeleton's "default position" of mPelvis is the bind pose's (0, 0, 1.067) above the feet, never
// where the joint rests in the viewer. So the offset is taken from that same rest position.
// (#1584 / #1592: it was taken from the default -- then from the override, else the default -- so a
// body without a pelvis override, the Open Pony's and a human's, went out 1.07 m low: the partner
// saw the bipedal pony with everything below its pelvis in the ground.)
inline V3 pelvisRest(bool hasOverride, const V3& overridePos) { return hasOverride ? overridePos : V3(); }
inline V3 pelvisOffset(const V3& drawn, bool hasOverride, const V3& overridePos)
{
    return drawn - pelvisRest(hasOverride, overridePos);
}

struct BoneTrack
{
    bool sent = false;        // a value went out (the receiver has one)
    bool moving = false;
    bool settled = true;      // the last movement's final value went out
    float stillFor = 0.f;     // seconds without a change past the threshold
    Q rot;                    // the value last sent
    V3 pos;
};

enum Decision
{
    HOLD = 0,     // nothing to send
    SEND = 1,     // moved past the threshold
    SETTLE = 2    // stopped: its final value (once)
};

inline float rotDelta(const Q& a, const Q& b) { return slfbt::qangle(a, b); }

// One bone, one tick: what to send. Updates the moving / quiet state; the sent value changes only
// with commit() (a line may be held back by the rate limit: the bone then asks again next tick).
// errOut (optional): how far past the keep threshold the value is (1 = at it), for the rate
// choice and the debug log.
inline Decision evaluate(BoneTrack& b, const Q& rot, const V3& pos, bool hasPos, float dt, const DeadbandParams& p,
                         float* errOut = nullptr)
{
    const float dr = b.sent ? rotDelta(b.rot, rot) : 0.f;
    const float dp = (b.sent && hasPos) ? slfbt::length(pos - b.pos) : 0.f;
    if (errOut) *errOut = b.sent ? std::max(dr / p.keepRot, hasPos ? dp / p.keepPos : 0.f) : 100.f;
    if (!b.sent) return SEND;
    const float tr = b.moving ? p.keepRot : p.startRot;
    const float tp = b.moving ? p.keepPos : p.startPos;
    if (dr > tr || dp > tp)
    {
        b.moving = true;
        b.settled = false;
        b.stillFor = 0.f;
        return SEND;
    }
    b.stillFor += dt;
    if (b.moving && b.stillFor >= p.quietAfter)
    {
        b.moving = false;
        if (!b.settled)
        {
            if (dr > p.settleRot || dp > p.settlePos) return SETTLE;   // (commit marks it settled)
            b.settled = true;
        }
    }
    else if (!b.moving && !b.settled)
    {
        // A settle the rate limit held back: still owed.
        if (dr > p.settleRot || dp > p.settlePos) return SETTLE;
        b.settled = true;
    }
    return HOLD;
}

// The value went out.
inline void commit(BoneTrack& b, const Q& rot, const V3& pos, Decision d)
{
    b.sent = true;
    b.rot = rot;
    b.pos = pos;
    if (d == SETTLE || !b.moving) b.settled = true;
}

struct RateParams
{
    float minInterval = 0.2f;
    // Gentle motion (every bone wanting a line is under fastRatio x its keep threshold: head sway,
    // a slow lean, breathing that crossed the threshold) goes out at most every slowInterval;
    // real movement at minInterval.
    float slowInterval = 1.f;
    float fastRatio = 2.f;
    float bytesPerSec = 2400.f;   // (#1587: with the fingers a keyframe is ~600 B)
    float burstBytes = 4800.f;
    float fullInterval = 15.f;
};

struct RateState
{
    float sinceLine = 1e9f;
    float sinceFull = 1e9f;
    float tokens = 4800.f;
    bool fullWanted = true;   // a stream start / a request
};

inline void tickRate(RateState& s, float dt, const RateParams& p)
{
    s.sinceLine += dt;
    s.sinceFull += dt;
    s.tokens += dt * p.bytesPerSec;
    if (s.tokens > p.burstBytes) s.tokens = p.burstBytes;
}
inline bool fullDue(const RateState& s, const RateParams& p) { return s.fullWanted || s.sinceFull >= p.fullInterval; }
// A line of about `bytes` may go now.
inline bool canSend(const RateState& s, float bytes, const RateParams& p)
{
    return s.sinceLine >= p.minInterval - 1e-4f && s.tokens >= bytes;
}
inline void noteSent(RateState& s, float bytes, bool full)
{
    s.sinceLine = 0.f;
    s.tokens -= bytes;
    if (full)
    {
        s.sinceFull = 0.f;
        s.fullWanted = false;
    }
}
// The bytes one pose line with `bones` rotation records (posBones of them with a position, plus
// the ease record) takes on the wire: "say|P1|<seq>|D|1/1|-|" + base64url of 8 bytes per bone
// (+6 with a position) and 8 for the ease.
inline float lineBytes(int bones, int posBones)
{
    const int raw = 8 + bones * 8 + posBones * 6;
    return 22.f + (float)((raw * 4 + 2) / 3);
}
// The receivers' ease into a line: about the time to the next one (moving: the interval just
// seen), never shorter than a tick, nor so long a stop looks floaty.
inline float easeFor(float sinceLast)
{
    return sinceLast < 0.25f ? 0.25f : (sinceLast > 0.5f ? 0.5f : sinceLast);
}

// One stream (our own avatar's driven bones), one tick. Per bone i < n: its track, this tick's
// value, whether it carries a position (the pelvis), and whether the layer drives it now (an
// undriven bone's track resets: the receiver gets it released by the caller, and it is sent
// again when driven again). setChanged: a bone started / stopped being driven (a line must go,
// for the releases). Commits what it sends.
struct Plan
{
    int kind = 0;          // 0 nothing, 1 delta, 2 full keyframe
    int bones = 0;         // rotation records in it
    int posBones = 0;
    float bytes = 0.f;     // the estimate the rate limit charged
    float ease = 0.f;      // the receivers' ease (seconds)
    // The bones that asked for the line (the largest first, at most kTrig) and how far past their
    // keep threshold (1 = at it; a SETTLE is below 1): the debug log explains the rate with them.
    static constexpr int kTrig = 8;
    int trig[kTrig] = {};
    float trigErr[kTrig] = {};
    int nTrig = 0;
};
constexpr int kMaxBones = 256;
// dp: each bone's deadband (n of them: the head and neck, which follow the headset, and the
// fingers get wider ones than the arms).
inline Plan plan(BoneTrack* tracks, const Q* rot, const V3* pos, const bool* hasPos, const bool* driven, int n, float dt,
                 bool setChanged, RateState& rs, const RateParams& rp, const DeadbandParams* dp)
{
    Plan out;
    tickRate(rs, dt, rp);
    if (n > kMaxBones) n = kMaxBones;
    Decision d[kMaxBones];
    float err[kMaxBones];
    int want = 0, wantPos = 0, all = 0, allPos = 0;
    float worst = 0.f;
    for (int i = 0; i < n; ++i)
    {
        d[i] = HOLD;
        if (!driven[i])
        {
            tracks[i] = BoneTrack{};
            continue;
        }
        ++all;
        allPos += hasPos[i] ? 1 : 0;
        d[i] = evaluate(tracks[i], rot[i], pos[i], hasPos[i], dt, dp[i], &err[i]);
        if (d[i] != HOLD)
        {
            ++want;
            wantPos += hasPos[i] ? 1 : 0;
            if (d[i] == SEND && err[i] > worst) worst = err[i];
        }
    }
    const bool full = fullDue(rs, rp) && all > 0;
    if (!full && want == 0 && !setChanged) return out;
    // Gentle drift waits for the slow interval; real movement (or a release, a keyframe) does not.
    if (!full && !setChanged && worst < rp.fastRatio && rs.sinceLine < rp.slowInterval - 1e-4f) return out;
    const int nb = full ? all : want, np = full ? allPos : wantPos;
    const float bytes = lineBytes(nb, np);
    if (!canSend(rs, bytes, rp)) return out;   // (asked again next tick)
    out.kind = full ? 2 : 1;
    out.bones = nb;
    out.posBones = np;
    out.bytes = bytes;
    out.ease = full ? 0.3f : easeFor(rs.sinceLine);
    for (int i = 0; i < n; ++i)
    {
        if (!driven[i]) continue;
        if (d[i] != HOLD)
        {
            // Keep the kTrig largest.
            int at = out.nTrig < Plan::kTrig ? out.nTrig++ : -1;
            if (at < 0)
            {
                at = 0;
                for (int k = 1; k < Plan::kTrig; ++k)
                    if (out.trigErr[k] < out.trigErr[at]) at = k;
                if (out.trigErr[at] >= err[i]) at = -1;
            }
            if (at >= 0)
            {
                out.trig[at] = i;
                out.trigErr[at] = err[i];
            }
        }
        if (full || d[i] != HOLD) commit(tracks[i], rot[i], pos[i], d[i]);
    }
    noteSent(rs, bytes, full);
    return out;
}
} // namespace slshare
