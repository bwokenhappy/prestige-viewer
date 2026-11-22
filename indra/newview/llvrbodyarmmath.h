/**
 * @file llvrbodyarmmath.h
 * @brief #1584 VR arms and neck: the pure math (no viewer types), shared by LLVRBodyArms and the
 *        fixtures in scripts/check/test_vr_arms.cpp (checks/vr_arms.py, host compiler only).
 *
 * Conventions as llvrbodymath.h: SL axes (X forward, Y left, Z up), metres, Hamilton quaternions
 * (qmul(a, b): b first). Rest pose: every local rotation identity. The SL arm in the rest pose is
 * the T-pose: the left arm along +Y, the right along -Y, palms down (-Z), thumbs forward (+X); a
 * natural elbow bends with the hand going forward, so the elbow itself points back (-X).
 *
 * Pieces:
 *   solveTwoBone   the leg solver (slfbt::solveLeg) generalised over the bone's rest "bend side":
 *                  +X for the leg (the knee points forward), -X for the arm (the elbow points back).
 *                  With +X it is solveLeg exactly (a fixture checks this).
 *   elbowPole      the natural elbow direction when no tracker gives one: down, back and out,
 *                  blended by how high and how far forward the hand is.
 *   solveArm       collar (a share of the reach and raise), two-bone shoulder / elbow onto the
 *                  wrist target (never stretched), forearm twist shared between the elbow (the
 *                  forearm bone, about its own axis) and the wrist. The wrist's world rotation is
 *                  the hand target exactly.
 *   gripToHand     an OpenXR controller grip pose (in body space) -> the avatar wrist's target
 *                  (position, rotation) with a per-controller offset.
 *   splitNeck      the headset's rotation relative to the chest split between mNeck and mHead
 *                  (most of a tilt and a nod in the head, a little in the neck; a share of the
 *                  turn in the neck), the neck also leaning toward the head's position (little of
 *                  it sideways); the head's world rotation is the headset's exactly.
 */
#pragma once

#include "llvrbodymath.h"

namespace slfbt
{
constexpr float kPi = 3.14159265f;

// ---- Swing / twist ---------------------------------------------------------------------------
// q = swing * twist (twist first), twist about `axis` (unit), swing perpendicular to it.
inline void swingTwist(const Q& q, const V3& axis, Q& swing, Q& twist)
{
    const V3 v(q.x, q.y, q.z);
    const float d = dot(v, axis);
    const Q t(axis.x * d, axis.y * d, axis.z * d, q.w);
    const float l = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z + t.w * t.w);
    twist = l > 1e-6f ? Q(t.x / l, t.y / l, t.z / l, t.w / l) : Q();
    swing = qnormalized(qmul(q, qconj(twist)));
}
// The signed angle (radians, -pi..pi) of a twist about `axis` (unit).
inline float twistAngle(const Q& twist, const V3& axis)
{
    float a = 2.f * std::atan2(dot(V3(twist.x, twist.y, twist.z), axis), twist.w);
    while (a > kPi) a -= 2.f * kPi;
    while (a < -kPi) a += 2.f * kPi;
    return a;
}
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Limit a rotation's angle (keeps its axis).
inline Q qlimit(const Q& q, float maxAngle)
{
    const float a = qangle(Q(), q);
    return (a > maxAngle && a > 1e-6f) ? qslerp(Q(), q, maxAngle / a) : q;
}

// ---- Two-bone IK (generalised solveLeg) ------------------------------------------------------
// Rest offsets: the first joint in its parent's frame, the middle joint in the first's, the end
// in the middle's.
struct TwoBoneRest
{
    V3 root, upper, lower;
};
struct TwoBonePose
{
    Q upperWorld, lowerWorld;          // the first and the middle joint's world rotations
    V3 rootPos, midPos, endPos;
    bool reached = false;              // the target was within reach (else the chain points at it)
    bool ok = false;
};
// The world rotations that put the end on `target`, the middle joint bending toward `pole` (a
// point), the middle joint a pure hinge. restBend: the side (in the bones' rest frames) the
// middle joint bends toward -- +X for a knee, -X for an elbow. Never NaN; out of reach the chain
// straightens toward the target without stretching (as solveLeg).
inline TwoBonePose solveTwoBone(const V3& parentPos, const Q& parentRot, const TwoBoneRest& r, const V3& target, const V3& pole,
                                const V3& restBend)
{
    TwoBonePose out;
    out.rootPos = parentPos + qrot(parentRot, r.root);
    const float l1 = length(r.upper), l2 = length(r.lower);
    if (l1 < 1e-4f || l2 < 1e-4f || !finite(target) || !finite(pole)) return out;
    const V3 restAlong = normalized(r.upper);
    const V3 restHinge = normalized(cross(restAlong, restBend), V3(0.f, -1.f, 0.f));
    const V3 d = target - out.rootPos;
    const float raw = length(d);
    const V3 dir = normalized(d, qrot(parentRot, restAlong));
    const float lo = std::fabs(l1 - l2) + 1e-3f, hi = (l1 + l2) * 0.9999f;
    const float dist = raw < lo ? lo : (raw > hi ? hi : raw);
    out.reached = raw <= l1 + l2 + 1e-3f;
    float c = (l1 * l1 + dist * dist - l2 * l2) / (2.f * l1 * dist);
    c = c > 1.f ? 1.f : (c < -1.f ? -1.f : c);
    const float s = std::sqrt(1.f - c * c);
    // The bend direction: the pole's part across the root -> target line (falls back to the
    // parent's rest bend side, then to the side across both).
    V3 p = pole - out.rootPos;
    V3 bend = p - dir * dot(p, dir);
    if (length(bend) < 1e-3f)
    {
        p = qrot(parentRot, restBend);
        bend = p - dir * dot(p, dir);
        if (length(bend) < 1e-3f)
        {
            p = qrot(parentRot, normalized(cross(restHinge, restAlong)));
            bend = p - dir * dot(p, dir);
            if (length(bend) < 1e-3f) bend = qrot(parentRot, normalized(cross(restBend, restAlong)));
        }
    }
    bend = normalized(bend);
    out.midPos = out.rootPos + dir * (l1 * c) + bend * (l1 * s);
    out.endPos = out.rootPos + dir * dist;
    const V3 upperV = out.midPos - out.rootPos, lowerV = out.endPos - out.midPos;
    // Both bones share the plane's normal (the hinge); each bone's bend side is built from it.
    const V3 hinge = normalized(cross(normalized(upperV), bend), qrot(parentRot, restHinge));
    out.upperWorld = qalignPair(r.upper, restBend, upperV, cross(hinge, normalized(upperV)));
    out.lowerWorld = qalignPair(r.lower, restBend, lowerV, cross(hinge, normalized(lowerV)));
    out.ok = finite(out.upperWorld) && finite(out.lowerWorld) && finite(out.midPos);
    return out;
}

// ---- Arms ------------------------------------------------------------------------------------
// Rest offsets (SL skeleton): the collar in the chest's frame, the shoulder in the collar's, the
// elbow in the shoulder's, the wrist in the elbow's. side: +1 left, -1 right.
struct ArmRest
{
    V3 collar, shoulder, elbow, wrist;
    float side = 1.f;
    float reach() const { return length(elbow) + length(wrist); }
};
struct ArmParams
{
    float collarRaise = 0.35f;      // share of the hand's elevation above the shoulder the collar takes
    float collarForward = 0.25f;    // share of the hand's forward / back angle (far reaches only)
    float collarMaxRaise = 0.40f;   // radians
    float collarMaxDrop = 0.08f;
    float collarMaxForward = 0.30f;
    float collarMaxBack = 0.15f;
    float twistShare = 0.5f;        // of the forearm roll, the forearm (mElbow) takes this much
    float maxTwist = 3.4f;          // radians the unwrapped roll may reach (a little past half a turn)
    float maxForearmTwist = 1.4f;   // radians the forearm bone takes at most (the wrist the rest)
};
struct ArmPose
{
    Q collarWorld, shoulderWorld, elbowWorld, wristWorld;
    V3 collarPos, shoulderPos, elbowPos, wristPos;
    float twist = 0.f;              // the forearm roll (radians) the hand target asked for
    bool reached = false;
    bool ok = false;
};

// A pole from a tracker on the middle joint (an elbow / knee tracker, a thigh tracker): the bend
// direction is the tracker's offset ACROSS the root -> end line, which vanishes as the limb
// straightens (a tracker sits on the limb, so on a straight limb it lies on that line) -- then a
// centimetre of noise flips the bend 180 degrees and the limb twists in on itself (a tester:
// "left arm twists in on itself constantly; same with ankles / feet"). Below minOff across the
// line the fallback pole is used, from fullOff on the tracker's, blended between.
inline V3 stablePole(const V3& root, const V3& end, const V3& tracker, const V3& fallback, float minOff = 0.03f,
                     float fullOff = 0.08f)
{
    const V3 axis = normalized(end - root, V3(0.f, 0.f, -1.f));
    const V3 mid = (root + end) * 0.5f;
    V3 across = tracker - root;
    across = across - axis * dot(across, axis);
    const float off = length(across);
    const float w = clampf((off - minOff) / (fullOff - minOff > 1e-4f ? fullOff - minOff : 1e-4f), 0.f, 1.f);
    if (w <= 0.f) return fallback;
    // The tracker's side, as a point one limb-length out (same scale as the fallback poles).
    const float reach = length(end - root) + 0.3f;
    const V3 fromTracker = mid + normalized(across) * reach;
    V3 fb = fallback - mid;
    fb = fb - axis * dot(fb, axis);
    const V3 fromFallback = mid + normalized(fb, normalized(across)) * reach;
    return lerp(fromFallback, fromTracker, w);
}

// The natural elbow pole when no tracker / bone input gives one: a point the elbow should bend
// toward. Down and a little back with the hand low or in front, out to the side as the hand
// rises over the shoulder; reaching far forward brings the elbow under the arm. Chest frame
// directions, turned by chestRot; the point is one reach from the shoulder -> wrist midpoint.
inline V3 elbowPole(const V3& shoulderPos, const V3& wristTarget, const Q& chestRot, float side, float reach)
{
    const float r = reach > 1e-3f ? reach : 0.5f;
    const V3 v = qrot(qconj(chestRot), wristTarget - shoulderPos);
    const float lift = clampf(v.z / r + 0.2f, 0.f, 1.f);   // 0: hand at / below the shoulder - 0.2 reach, 1: overhead
    const float fwd = clampf(v.x / r, 0.f, 1.f);
    const float down = 1.f - lift;
    const float out = 0.3f + 0.7f * lift;
    const float back = 0.15f + 0.35f * (1.f - fwd) * (1.f - lift);
    const V3 dirChest = normalized(V3(-back, side * out, -down), V3(0.f, side, 0.f));
    return (shoulderPos + wristTarget) * 0.5f + qrot(chestRot, dirChest) * r;
}

// #1592 the elbow pole of a quadruped's foreleg driven as an arm (the Open Pony): its elbow
// points straight back at rest (a human's hanging arm points it back and out, and that
// outward share rolls a foreleg's bones about their axis: the leg twists). The reach turned a
// quarter turn back in the body's side view: back while the hoof hangs, down under the leg as it
// reaches forward, forward as it reaches overhead; never out to the side (a reach out to the side
// bends it back and down). (It was "back, a little down": reaching forward and up, the pole lay almost
// on the shoulder -> hoof line and its small remainder put the elbow up and out, the forearm
// rolled 130 degrees.)
inline V3 forelegPole(const V3& shoulderPos, const V3& wristTarget, const Q& chestRot, float reach)
{
    const float r = reach > 1e-3f ? reach : 0.5f;
    const V3 v = qrot(qconj(chestRot), wristTarget - shoulderPos);
    const float side = std::sqrt(v.x * v.x + v.z * v.z);   // (the reach in the body's side view)
    const float w = clampf(side / (length(v) > 1e-4f ? length(v) : 1e-4f) * 2.f, 0.f, 1.f);   // (straight out sideways: 0)
    const V3 sideways = normalized(V3(-0.5f, 0.f, -1.f));
    const V3 turned = side > 1e-4f ? V3(v.z / side, 0.f, -v.x / side) : sideways;
    const V3 dirChest = normalized(lerp(sideways, turned, w), V3(-1.f, 0.f, 0.f));
    return (shoulderPos + wristTarget) * 0.5f + qrot(chestRot, dirChest) * r;
}

// The collar's rotation (local to the chest) for a wrist target: a share of the hand's elevation
// (shrug on overhead reaches) and, on far reaches, of its forward / back angle.
inline Q collarTurn(const V3& collarPos, const Q& chestRot, const ArmRest& r, const V3& wristTarget, const ArmParams& p)
{
    const V3 d = qrot(qconj(chestRot), wristTarget - collarPos);
    const float dl = length(d);
    if (dl < 1e-3f) return Q();
    const V3 dn = d * (1.f / dl);
    const float total = length(r.shoulder) + r.reach();
    const float farReach = clampf((dl / (total > 1e-3f ? total : 1.f) - 0.6f) / 0.4f, 0.f, 1.f);
    const float elev = std::asin(clampf(dn.z, -1.f, 1.f));
    float raise = elev > 0.f ? p.collarRaise * elev * (0.5f + 0.5f * farReach) : 0.1f * elev;
    raise = clampf(raise, -p.collarMaxDrop, p.collarMaxRaise);
    const float horiz = std::atan2(dn.x, r.side * dn.y);   // 0 out to the side, + forward, - back
    const float fwd = clampf(p.collarForward * horiz * farReach, -p.collarMaxBack, p.collarMaxForward);
    // Raise: the arm's axis (side * Y) toward +Z is a turn about X by side * raise; forward:
    // toward +X, a turn about Z by -side * fwd.
    return qnormalized(qmul(qaxis(V3(0.f, 0.f, 1.f), -r.side * fwd), qaxis(V3(1.f, 0.f, 0.f), r.side * raise)));
}

// The arm from the chest onto a wrist target (position and the hand's world rotation).
// elbowHint: a point the elbow should bend toward (a tracker, a bone input), else the natural
// pole. collarLocal: a collar rotation from a bone input (else collarTurn).
// twistState (optional, per arm, kept by the caller): the last frame's forearm roll -- the roll is
// unwrapped toward it (a hand turned through 180 degrees must not flip the forearm's share from
// +90 to -90 between two frames) and the forearm's share clamped to maxForearmTwist.
inline ArmPose solveArm(const V3& chestPos, const Q& chestRot, const ArmRest& r, const V3& wristTarget, const Q& handTarget,
                        const V3* elbowHint, const ArmParams& p, const Q* collarLocal = nullptr, float* twistState = nullptr)
{
    ArmPose out;
    out.collarPos = chestPos + qrot(chestRot, r.collar);
    if (!finite(wristTarget) || !finite(handTarget) || r.reach() < 1e-3f) return out;
    const Q cl = collarLocal ? qnormalized(*collarLocal) : collarTurn(out.collarPos, chestRot, r, wristTarget, p);
    out.collarWorld = qnormalized(qmul(chestRot, cl));
    out.shoulderPos = out.collarPos + qrot(out.collarWorld, r.shoulder);
    const V3 pole = (elbowHint && finite(*elbowHint)) ? *elbowHint : elbowPole(out.shoulderPos, wristTarget, chestRot, r.side, r.reach());
    TwoBoneRest tb;
    tb.root = r.shoulder;
    tb.upper = r.elbow;
    tb.lower = r.wrist;
    const TwoBonePose tp = solveTwoBone(out.collarPos, out.collarWorld, tb, wristTarget, pole, V3(-1.f, 0.f, 0.f));
    if (!tp.ok) return out;
    out.shoulderWorld = tp.upperWorld;
    out.elbowPos = tp.midPos;
    out.wristPos = tp.endPos;
    out.reached = tp.reached;
    // Forearm roll: the hand target's twist about the forearm (relative to the hinge frame)
    // shared between the forearm bone and the wrist.
    const V3 axis = normalized(r.wrist, V3(0.f, r.side, 0.f));
    Q sw, tw;
    swingTwist(qmul(qconj(tp.lowerWorld), qnormalized(handTarget)), axis, sw, tw);
    out.twist = twistAngle(tw, axis);
    if (twistState)
    {
        // Continuous with last frame (no +180 / -180 jump), within a forearm's range.
        while (out.twist - *twistState > kPi) out.twist -= 2.f * kPi;
        while (out.twist - *twistState < -kPi) out.twist += 2.f * kPi;
        out.twist = clampf(out.twist, -p.maxTwist, p.maxTwist);
        *twistState = out.twist;
    }
    const float forearm = clampf(out.twist * clampf(p.twistShare, 0.f, 1.f), -p.maxForearmTwist, p.maxForearmTwist);
    out.elbowWorld = qnormalized(qmul(tp.lowerWorld, qaxis(axis, forearm)));
    out.wristWorld = qnormalized(handTarget);
    out.ok = finite(out.collarWorld) && finite(out.shoulderWorld) && finite(out.elbowWorld) && finite(out.wristPos);
    return out;
}

// ---- A limb that ends in a hoof (#1592: the Open Pony's foreleg) --------------------------------
// The foreleg is upper arm (mShoulder), forearm (mElbow), cannon (mWrist) and hoof (mHandRing1).
// Your hand drives the HOOF, the way a hand turns at the wrist: the hoof joint goes where the
// controller's grip is and the hoof takes the controller's rotation exactly. The forearm and the
// cannon are the "forearm" between the elbow and that joint: they keep their rest bend (the knee)
// and twist only the small share of the roll a human forearm takes, the hoof the rest. (#1592: the
// cannon took the controller's whole rotation and the hoof was held straight on it -- the owner:
// "my hooves don't rotate with my hands, my forearms end up rotating with my hands".)
struct HoofParams
{
    float forearmShare = 0.25f;      // of the hand's roll, the forearm bone (mElbow) takes this ...
    float cannonShare = 0.5f;        // ... and the cannon this much in all (the forearm's included)
    float maxForearmTwist = 0.7f;    // radians the forearm bone takes at most
    float maxCannonTwist = 1.4f;     // radians the cannon takes at most in all (a human forearm's range)
    float maxTwist = 3.4f;           // radians the unwrapped roll may reach (as ArmParams)
};
struct HoofPose
{
    ArmPose arm;                     // collar, upper arm, forearm (elbowWorld) and cannon (wristWorld)
    Q hoofWorld;                     // the hand target exactly
    V3 hoofPos;
    float forearmTwist = 0.f, cannonTwist = 0.f;   // the roll each took (radians; the cannon's in all)
};
// r: the arm's rest (r.wrist the forearm -> cannon joint); cannon: the hoof joint in the cannon's
// frame; cannonRest: the cannon's rest local under the forearm (all in the solver's virtual
// skeleton). hoofTarget / handTarget: where the hoof joint goes and the hoof's world rotation.
// elbowHint, collarLocal, twistState: as solveArm.
inline HoofPose solveHoofArm(const V3& chestPos, const Q& chestRot, const ArmRest& r, const V3& cannon, const Q& cannonRest,
                             const V3& hoofTarget, const Q& handTarget, const V3* elbowHint, const ArmParams& ap, const HoofParams& p,
                             const Q* collarLocal = nullptr, float* twistState = nullptr)
{
    HoofPose out;
    ArmPose& a = out.arm;
    a.collarPos = chestPos + qrot(chestRot, r.collar);
    const V3 lower = r.wrist + qrot(cannonRest, cannon);   // the elbow -> hoof joint, the knee as it rests
    if (!finite(hoofTarget) || !finite(handTarget) || length(r.elbow) < 1e-3f || length(lower) < 1e-3f) return out;
    const Q cl = collarLocal ? qnormalized(*collarLocal) : collarTurn(a.collarPos, chestRot, r, hoofTarget, ap);
    a.collarWorld = qnormalized(qmul(chestRot, cl));
    a.shoulderPos = a.collarPos + qrot(a.collarWorld, r.shoulder);
    const float reach = length(r.elbow) + length(lower);
    const V3 pole = (elbowHint && finite(*elbowHint)) ? *elbowHint : forelegPole(a.shoulderPos, hoofTarget, chestRot, reach);
    TwoBoneRest tb;
    tb.root = r.shoulder;
    tb.upper = r.elbow;
    tb.lower = lower;
    const TwoBonePose tp = solveTwoBone(a.collarPos, a.collarWorld, tb, hoofTarget, pole, V3(-1.f, 0.f, 0.f));
    if (!tp.ok) return out;
    a.shoulderWorld = tp.upperWorld;
    a.elbowPos = tp.midPos;
    a.reached = tp.reached;
    out.hoofPos = tp.endPos;
    // The roll: the hand target's twist about the cannon's own axis, from the cannon as it rests
    // on the solved forearm.
    const V3 cannonAxis = normalized(cannon, V3(0.f, r.side, 0.f));
    const Q cannon0 = qnormalized(qmul(tp.lowerWorld, cannonRest));
    Q sw, tw;
    swingTwist(qmul(qconj(cannon0), qnormalized(handTarget)), cannonAxis, sw, tw);
    a.twist = twistAngle(tw, cannonAxis);
    if (twistState)
    {
        while (a.twist - *twistState > kPi) a.twist -= 2.f * kPi;
        while (a.twist - *twistState < -kPi) a.twist += 2.f * kPi;
        a.twist = clampf(a.twist, -p.maxTwist, p.maxTwist);
        *twistState = a.twist;
    }
    // The forearm turns about the elbow -> hoof line (the hoof joint stays on the target), the
    // cannon on about its own axis (ditto): each a share of the roll, within a forearm's range.
    out.forearmTwist = clampf(a.twist * clampf(p.forearmShare, 0.f, 1.f), -p.maxForearmTwist, p.maxForearmTwist);
    out.cannonTwist = clampf(a.twist * clampf(p.cannonShare, 0.f, 1.f), -p.maxCannonTwist, p.maxCannonTwist);
    a.elbowWorld = qnormalized(qmul(tp.lowerWorld, qaxis(normalized(lower), out.forearmTwist)));
    a.wristWorld = qnormalized(qmul(qmul(a.elbowWorld, cannonRest), qaxis(cannonAxis, out.cannonTwist - out.forearmTwist)));
    a.wristPos = a.elbowPos + qrot(a.elbowWorld, r.wrist);
    out.hoofWorld = qnormalized(handTarget);
    a.ok = finite(a.collarWorld) && finite(a.shoulderWorld) && finite(a.elbowWorld) && finite(a.wristWorld) && finite(a.wristPos);
    return out;
}

// ---- Controller grip -> avatar wrist ---------------------------------------------------------
// OpenXR's grip pose: -Z along the tube the closed fingers make, +Y up out of the top of the
// fist (the thumb side), +X out of the palm for the left hand, into it for the right. In body
// space (xrRotToBody) those local axes become: forward (-Z) -> +X, up (+Y) -> +Z, OpenXR +X ->
// -Y. The avatar's hand rest frame: thumb +X, fingers +side*Y, palm facing -Z.
//
// Holding a controller, the fingers (wrist -> knuckles) run along the fist's tube tilted down by
// `pitch` (a power grip's handle crosses the palm diagonally), the palm faces across the body
// (left palm to the right), the thumb up; the wrist joint sits behind the fist and toward the
// back of the hand.
struct GripOffset
{
    float pitch = 0.35f;     // radians the fingers point below the fist's tube
    float back = 0.085f;     // metres from the grip's centre back to the wrist joint (along the fingers)
    float lift = 0.035f;     // metres toward the back of the hand (away from the handle)
    float across = 0.f;      // metres toward the thumb
};
// The hand's rest frame in the grip's body-space frame (pitch 0), for side +1 left / -1 right.
inline Q gripHandBasis(float side)
{
    // Columns: where the hand's X (thumb), Y (fingers * side), Z go in the grip's frame.
    // Left: thumb up (+Z), fingers forward (+X), palm (-Z_hand) to the right (-Y): Z_hand -> +Y.
    // Right: fingers are -Y_hand, so Y_hand -> -X; palm faces left (+Y): Z_hand -> -Y.
    return qfromBasis(V3(0.f, 0.f, 1.f), V3(side, 0.f, 0.f), V3(0.f, side, 0.f));
}
// The wrist target from a grip pose (body or agent space, SL axes; gripRot as xrRotToBody gives).
// scale: the body scale (the avatar's eye height over yours) applied to the offset.
inline void gripToHand(const V3& gripPos, const Q& gripRot, float side, const GripOffset& o, float scale, V3& wristPos, Q& handRot)
{
    // Fingers tilted down by pitch: about the grip's across axis (Y), +pitch takes +X toward -Z.
    const Q tilt = qaxis(V3(0.f, 1.f, 0.f), o.pitch);
    handRot = qnormalized(qmul(qnormalized(gripRot), qmul(tilt, gripHandBasis(side))));
    // In the hand's frame: back along the fingers, toward the back of the hand (+Z), to the thumb.
    const V3 local(o.across, -side * o.back, o.lift);
    wristPos = gripPos + qrot(handRot, local) * scale;
}

// ---- Neck ------------------------------------------------------------------------------------
struct NeckRest
{
    V3 neck;     // the neck in the chest's frame
    V3 head;     // the head in the neck's frame
};
// The split follows a real neck: a head tilt (ear toward the shoulder) happens mostly at the top,
// the head rocking on the neck, and a nod mostly there too; the neck below takes a little of
// each. (#1584: with 60% of the tilt in mNeck the owner's head tilt swung the whole neck
// sideways, the head riding on top of it.) A turn (yaw) keeps its split.
struct NeckParams
{
    float pitchShare = 0.4f;   // of the head's pitch (nod) relative to the chest, the neck takes this
    float rollShare = 0.2f;    // ... of its roll (tilt about the forward axis)
    float yawShare = 0.35f;    // ... of its yaw
    float follow = 1.f;        // 0..1: lean the neck toward the head's position
    float leanRollShare = 0.3f;   // of the lean's sideways part (a head moved sideways), the neck takes this
    float maxLean = 0.6f;      // radians the position may lean the neck
};
// A rotation as its axis * angle (radians), and back.
inline V3 rotVec(const Q& q)
{
    Q n = qnormalized(q);
    if (n.w < 0.f) n = Q(-n.x, -n.y, -n.z, -n.w);
    const V3 v(n.x, n.y, n.z);
    const float s = length(v);
    if (s < 1e-7f) return v * 2.f;
    return v * (2.f * std::atan2(s, n.w) / s);
}
inline Q fromRotVec(const V3& v)
{
    const float a = length(v);
    return a < 1e-7f ? Q() : qaxis(v * (1.f / a), a);
}
// The neck's share of a swing about a horizontal axis (chest frame: X forward, Y left): roll
// (about the face's forward) and pitch (about its left) each scaled by their own share. facing:
// the head's turn about the chest's up (a nod with the head turned is still a nod).
inline Q neckShareOf(const Q& swing, float rollShare, float pitchShare, const Q& facing = Q())
{
    const V3 v = qrot(qconj(facing), rotVec(swing));
    return fromRotVec(qrot(facing, V3(v.x * clampf(rollShare, 0.f, 1.f), v.y * clampf(pitchShare, 0.f, 1.f), 0.f)));
}
struct NeckPose
{
    Q neckWorld, headWorld;
    float leanWeight = 0.f;    // how much the position lean was used (0 = the rotation split only)
};
// chestRot: the chest's world rotation; headTarget: the head's world rotation (the headset's);
// neckPos: the neck joint's world position; headPos (optional): where the head joint should be
// (from the headset's position). headWorld is headTarget exactly, so neck local * head local
// composes back to the headset's rotation relative to the chest.
inline NeckPose splitNeck(const Q& chestRot, const Q& headTarget, const V3& neckPos, const V3* headPos, const NeckRest& r,
                          const NeckParams& p)
{
    NeckPose out;
    const Q rel = qnormalized(qmul(qconj(chestRot), headTarget));
    Q swing, twist;
    swingTwist(rel, V3(0.f, 0.f, 1.f), swing, twist);
    Q neckSwing = neckShareOf(swing, p.rollShare, p.pitchShare, twist);
    if (headPos && finite(*headPos) && p.follow > 0.f)
    {
        // Lean toward the head's position, as long as it is about one neck length away (a target
        // far from it -- a crouch the animated body does not make -- is not a lean).
        const V3 want = qrot(qconj(chestRot), *headPos - neckPos);
        const float l = length(r.head), wl = length(want);
        const float fit = clampf(1.f - (std::fabs(wl - l) - 0.03f) / 0.07f, 0.f, 1.f);
        if (fit > 0.f && wl > 1e-3f)
        {
            Q lean = qlimit(qfromto(r.head, want), p.maxLean);
            // Only the lean (no twist about the neck's own axis), and little of it sideways: a
            // tilted head moves the eyes sideways too, and that is the head's tilt, not the neck's.
            Q ls, lt;
            swingTwist(lean, normalized(r.head, V3(0.f, 0.f, 1.f)), ls, lt);
            // (Sideways is the chest's: the lean is where the head sits over the chest.)
            ls = neckShareOf(ls, p.leanRollShare, 1.f);
            out.leanWeight = fit * clampf(p.follow, 0.f, 1.f);
            neckSwing = qslerp(neckSwing, ls, out.leanWeight);
        }
    }
    const Q neckRel = qnormalized(qmul(neckSwing, qslerp(Q(), twist, clampf(p.yawShare, 0.f, 1.f))));
    out.neckWorld = qnormalized(qmul(chestRot, neckRel));
    out.headWorld = qnormalized(headTarget);
    return out;
}

// ---- What drives an arm ------------------------------------------------------------------------
// The controller in the hand, else a wrist tracker, else a tracked hand (XR_EXT_hand_tracking).
// Fly cam is not an input here: the arms stay on the controllers in fly cam too (a tester could not
// see their arms from outside -- fly cam is VR's third person). The fly cam's camera moves with the
// sticks, and the stick hand's pose is your real hand's; only the head is released (it would turn
// with the headset that is flying the camera).
enum ArmInput { AI_NONE = 0, AI_CONTROLLER, AI_WRIST_TRACKER, AI_HAND };
inline ArmInput armInput(bool usable, bool armsOn, bool controller, bool wristTracker, bool trackedHand)
{
    if (!usable || !armsOn) return AI_NONE;
    if (controller) return AI_CONTROLLER;
    if (wristTracker) return AI_WRIST_TRACKER;
    if (trackedHand) return AI_HAND;
    return AI_NONE;
}

// ---- Flying (#1584) ----------------------------------------------------------------------------
// While the avatar flies, the hips and legs are the fly animation's, whatever the source: tracked
// feet are measured from the room's floor, and in the air there is no floor under the avatar for
// them (a tester's screenshot: flying with the legs folded into a crouch). The spine, arms, neck
// and fingers stay tracked. Per body role (slfbt::Body): does it stay tracked in flight?
inline bool roleTrackedInFlight(int body) { return body == B_CHEST; }

// ---- Fly cam (#1584) ---------------------------------------------------------------------------
// In fly cam the headset flies the camera, the avatar stays where it was left. Two modes, chosen
// when fly cam starts (SlipstreamVRFlycamFreezePose):
//   D (default): the body stays live (hips, spine, legs, VMC / tracker arms) on the mapping from
//      before; the head (and the neck's head-turn share) is released to the animation: headLive
//      eases 1 -> 0 over `ease` seconds, and back on leaving.
//   C (freeze): the last pose is held, head included (frozen); on leaving the layer eases from the
//      held pose back to live over `ease` seconds (liveBlend 0 -> 1).
struct FlyState
{
    bool fly = false;
    bool freeze = false;      // this fly cam's mode is C
    float headLive = 1.f;     // 0..1: the headset turns the head
    float liveBlend = 1.f;    // 0..1: live pose (1) vs the held one (0)
    bool frozen() const { return fly && freeze; }
};
struct FlyEvents
{
    bool entered = false, left = false;
};
inline FlyEvents stepFly(FlyState& s, bool fly, bool freezeSetting, float dt, float ease = 0.3f)
{
    FlyEvents ev;
    const float step = ease > 1e-4f ? dt / ease : 1.f;
    if (fly && !s.fly)
    {
        ev.entered = true;
        s.fly = true;
        s.freeze = freezeSetting;
        if (s.freeze) s.liveBlend = 0.f;   // (held from this frame on)
    }
    else if (!fly && s.fly)
    {
        ev.left = true;
        s.fly = false;
        s.freeze = false;
    }
    const bool headOn = !s.fly;
    s.headLive = clampf(s.headLive + (headOn ? step : -step), 0.f, 1.f);
    if (s.frozen()) s.liveBlend = 0.f;
    else s.liveBlend = clampf(s.liveBlend + step, 0.f, 1.f);
    return ev;
}
} // namespace slfbt
