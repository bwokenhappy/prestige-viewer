/**
 * @file llvrbodyfingermath.h
 * @brief #1587 VR finger tracking: the pure math (no viewer types), shared by LLVRBodyFingers and
 *        scripts/check/test_vr_fingers.cpp (checks/vr_fingers.py, host compiler only).
 *
 * A hand pose is per finger (0 thumb, 1 index, 2 middle, 3 ring, 4 pinky) three flexion angles
 * (radians, + toward the palm; the SL Bento bones mHand<Finger>1..3) and a spread (radians, + away
 * from the middle finger; the first bone only). Sources:
 *   fromJoints      XR_EXT_hand_tracking's 26 joints (camera hand tracking; SteamVR turns the
 *                   Index controllers' finger sensing into these joints). Only joint POSITIONS
 *                   are used (angles between bones, the spread in the palm's plane): no vendor's
 *                   joint axes, so nothing depends on their orientation conventions.
 *   fromController  a natural grip from the controller's trigger / grip / touches (point, fist,
 *                   thumbs up, open hand) where no hand joints come.
 * Applied to the SL hand (rest pose: palm down, fingers along +side*Y, thumb toward +X): each
 * bone turns about the axis across its own rest direction and the palm's normal (flexion), the
 * first one also about the palm's normal (spread). A pose is applied in the bones' own rest
 * frames, so it is the same on either hand.
 */
#pragma once

#include "llvrbodymath.h"

#include <algorithm>

namespace slfinger
{
using slfbt::Q;
using slfbt::V3;

constexpr int FINGERS = 5;
constexpr int SEGMENTS = 3;
constexpr int JOINTS = 26;   // XrHandJointEXT

struct HandPose
{
    float curl[FINGERS][SEGMENTS] = {};
    float splay[FINGERS] = {};
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float angleBetween(const V3& a, const V3& b)
{
    const float la = slfbt::length(a), lb = slfbt::length(b);
    if (la < 1e-6f || lb < 1e-6f) return 0.f;
    return std::acos(clampf(slfbt::dot(a, b) / (la * lb), -1.f, 1.f));
}
inline HandPose lerpPose(const HandPose& a, const HandPose& b, float t)
{
    HandPose o;
    for (int f = 0; f < FINGERS; ++f)
    {
        for (int k = 0; k < SEGMENTS; ++k) o.curl[f][k] = a.curl[f][k] + (b.curl[f][k] - a.curl[f][k]) * t;
        o.splay[f] = a.splay[f] + (b.splay[f] - a.splay[f]) * t;
    }
    return o;
}
// Exponential smoothing toward a pose (tau seconds).
inline void smoothPose(HandPose& p, const HandPose& target, float dt, float tau)
{
    const float a = tau > 1e-4f ? 1.f - std::exp(-dt / tau) : 1.f;
    p = lerpPose(p, target, a);
}

// The most a finger bends at each joint (radians), and the natural spread of a relaxed hand
// (subtracted from what hand tracking measures: the SL rest hand is already a little spread).
struct Limits
{
    float fingerMax[SEGMENTS] = {1.45f, 1.65f, 1.15f};
    float thumbMax[SEGMENTS] = {0.7f, 0.9f, 1.2f};
    float naturalSplay[FINGERS] = {0.65f, 0.08f, 0.f, 0.06f, 0.16f};
    float splayMin = -0.25f, splayMax = 0.45f;
};

// ---- From hand joints ------------------------------------------------------------------------------
// XrHandJointEXT: PALM 0, WRIST 1, THUMB_METACARPAL 2, THUMB_PROXIMAL 3, THUMB_DISTAL 4, THUMB_TIP 5,
// then per finger METACARPAL, PROXIMAL, INTERMEDIATE, DISTAL, TIP from 6 (index), 11 (middle),
// 16 (ring), 21 (little). side: +1 left, -1 right (the palm's side of the hand's plane).
inline bool fromJoints(const V3 j[JOINTS], const bool valid[JOINTS], float side, const Limits& lim, HandPose& out)
{
    for (int i = 1; i < JOINTS; ++i)
        if (!valid[i]) return false;
    auto base = [](int f) { return 6 + (f - 1) * 5; };
    // The hand's frame: forward along the middle metacarpal, the thumb side from the little
    // finger's metacarpal to the index's, the palm's normal from the two (and the hand's side).
    const V3 fwd = slfbt::normalized(j[base(2) + 1] - j[base(2)], V3(1.f, 0.f, 0.f));
    V3 thumbward = j[base(1)] - j[base(4)];
    thumbward = slfbt::normalized(thumbward - fwd * slfbt::dot(thumbward, fwd), V3(0.f, 1.f, 0.f));
    const V3 palm = slfbt::normalized(slfbt::cross(fwd, thumbward) * side, V3(0.f, 0.f, -1.f));   // toward the palm side
    auto inPlane = [&](const V3& v) { return v - palm * slfbt::dot(v, palm); };
    for (int f = 1; f < FINGERS; ++f)
    {
        const int b = base(f);
        const V3 meta = j[b + 1] - j[b], prox = j[b + 2] - j[b + 1], mid = j[b + 3] - j[b + 2], dist = j[b + 4] - j[b + 3];
        // Unsigned bends; a joint bent backward (hyperextended) reads as a small bend: clamp them
        // where they bend toward the back of the hand.
        auto bend = [&](const V3& a, const V3& c) {
            const float ang = angleBetween(a, c);
            return slfbt::dot(slfbt::cross(a, c), slfbt::cross(fwd, palm)) < 0.f ? -std::min(ang, 0.2f) : ang;
        };
        // The knuckle: only the bend out of the palm's plane (the metacarpal and the first bone
        // also differ IN the plane: that is the spread, below).
        auto elevation = [&](const V3& v) { return std::atan2(slfbt::dot(v, palm), slfbt::length(inPlane(v))); };
        out.curl[f][0] = clampf(elevation(prox) - elevation(meta), -0.2f, lim.fingerMax[0]);
        out.curl[f][1] = clampf(bend(prox, mid), 0.f, lim.fingerMax[1]);
        out.curl[f][2] = clampf(bend(mid, dist), 0.f, lim.fingerMax[2]);
    }
    // Spread: the proximal bone in the palm's plane against the middle finger's.
    const V3 midDir = inPlane(j[base(2) + 2] - j[base(2) + 1]);
    for (int f = 1; f < FINGERS; ++f)
    {
        if (f == 2)
        {
            out.splay[f] = 0.f;
            continue;
        }
        const V3 d = inPlane(j[base(f) + 2] - j[base(f) + 1]);
        const float ang = angleBetween(midDir, d);
        const float away = f == 1 ? 1.f : -1.f;   // index spreads thumbward, ring / little away from it
        const float s = slfbt::dot(slfbt::normalized(d) - slfbt::normalized(midDir), thumbward) * away >= 0.f ? ang : -ang;
        out.splay[f] = clampf(s - lim.naturalSplay[f], lim.splayMin, lim.splayMax);
    }
    // Thumb: the metacarpal's lift toward the palm (opposition), the two joint bends; its spread
    // in the palm's plane from the index side.
    const V3 tMeta = j[3] - j[2], tProx = j[4] - j[3], tDist = j[5] - j[4];
    const float lift = std::asin(clampf(slfbt::dot(slfbt::normalized(tMeta), palm), -1.f, 1.f));
    out.curl[0][0] = clampf(lift, 0.f, lim.thumbMax[0]);
    out.curl[0][1] = clampf(angleBetween(tMeta, tProx), 0.f, lim.thumbMax[1]);
    out.curl[0][2] = clampf(angleBetween(tProx, tDist), 0.f, lim.thumbMax[2]);
    out.splay[0] = clampf(angleBetween(inPlane(tMeta), midDir) - lim.naturalSplay[0], lim.splayMin, lim.splayMax);
    return true;
}

// ---- From the controller -----------------------------------------------------------------------------
struct ControllerInput
{
    float trigger = 0.f;         // 0..1
    bool triggerTouch = false, triggerTouchBound = false;
    float squeeze = 0.f;         // 0..1 (-1: none)
    bool thumbTouch = false, thumbTouchBound = false;
};
// Curl fractions 0..1 per finger, then angles: index on the trigger (pointing straight when the
// trigger is not touched), middle / ring / pinky on the grip (a relaxed curl at least), the thumb
// down on the controller when touched, else up (thumbs up with a fist, open with an open hand).
inline HandPose fromController(const ControllerInput& in, const Limits& lim)
{
    float c[FINGERS];
    const float trig = clampf(in.trigger, 0.f, 1.f);
    c[1] = in.triggerTouchBound ? (in.triggerTouch ? 0.3f + 0.7f * trig : 0.f) : 0.15f + 0.85f * trig;
    const float grip = in.squeeze >= 0.f ? clampf(in.squeeze, 0.f, 1.f) : 0.f;
    c[2] = c[3] = c[4] = 0.15f + 0.85f * grip;
    c[0] = in.thumbTouchBound ? (in.thumbTouch ? 0.7f : 0.1f) : 0.4f;
    HandPose p;
    for (int f = 0; f < FINGERS; ++f)
        for (int k = 0; k < SEGMENTS; ++k) p.curl[f][k] = c[f] * (f == 0 ? lim.thumbMax[k] : lim.fingerMax[k]);
    // An open hand spreads a little; a fist closes the gaps.
    const float open = 1.f - std::max({c[1], c[2], c[3], c[4]});
    p.splay[1] = 0.12f * open;
    p.splay[3] = 0.08f * open;
    p.splay[4] = 0.18f * open;
    p.splay[0] = 0.2f * open;
    return p;
}

// ---- Onto the SL bones -----------------------------------------------------------------------------
// The local rotation of finger f's bone k (0..2) with rest direction `dir` (the bone's offset to
// its child, in its rest frame = the hand's frame), on the hand of `side` (+1 left, -1 right).
inline Q boneLocal(int f, int k, const V3& dir, float side, const HandPose& p)
{
    const V3 d = slfbt::normalized(dir, V3(0.f, side, 0.f));
    // The palm's normal (fingers bend toward it); the thumb bends toward the palm and across it
    // (the palm's centre is on the -X side of the thumb, on both hands).
    const V3 n = f == 0 ? slfbt::normalized(V3(-0.4f, 0.f, -1.f)) : V3(0.f, 0.f, -1.f);
    const V3 flexAxis = slfbt::normalized(slfbt::cross(d, n), V3(-side, 0.f, 0.f));
    Q q = slfbt::qaxis(flexAxis, p.curl[f][k]);
    if (k == 0)
    {
        // Spread away from the middle finger: the index and the thumb toward +X, ring / little -X.
        const V3 away = (f <= 1) ? V3(1.f, 0.f, 0.f) : V3(-1.f, 0.f, 0.f);
        const V3 spreadAxis = slfbt::normalized(slfbt::cross(d, away), V3(0.f, 0.f, -side));
        q = slfbt::qmul(slfbt::qaxis(spreadAxis, p.splay[f]), q);
    }
    return slfbt::qnormalized(q);
}
} // namespace slfinger
