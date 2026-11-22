/**
 * @file llvrbodymath.h
 * @brief #1565 VR full-body tracking: the pure math (no viewer types), shared by LLVRBody and
 *        the fixtures in scripts/check/test_vr_fbt.cpp (checks/vr_fbt.py, host compiler only).
 *
 * Conventions: SL axes (X forward / at, Y left, Z up), metres. Quaternions are Hamilton
 * (x, y, z, w): qmul(a, b) applies b first, then a; qrot(q, v) rotates v by q. (LLQuaternion
 * has the same components; its a * b is qmul(b, a).) A joint's world rotation is
 * qmul(parentWorld, local); a child's world position is parentPos + qrot(parentWorld, offset),
 * offset = the child's local position scaled by the parent's scale (LLXformMatrix::update).
 * Rest pose: every local rotation identity (the SL skeleton), so a bone points along its
 * child's offset and its front is +X.
 */
#pragma once

#include <cmath>

namespace slfbt
{
struct V3
{
    float x = 0.f, y = 0.f, z = 0.f;
    V3() = default;
    V3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
};
inline V3 operator+(const V3& a, const V3& b) { return V3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 operator-(const V3& a, const V3& b) { return V3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 operator*(const V3& a, float s) { return V3(a.x * s, a.y * s, a.z * s); }
inline float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalized(const V3& a, const V3& fallback = V3(1.f, 0.f, 0.f))
{
    const float l = length(a);
    return (l > 1e-6f && std::isfinite(l)) ? a * (1.f / l) : fallback;
}
inline V3 lerp(const V3& a, const V3& b, float t) { return a + (b - a) * t; }
inline bool finite(const V3& a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

struct Q
{
    float x = 0.f, y = 0.f, z = 0.f, w = 1.f;
    Q() = default;
    Q(float ax, float ay, float az, float aw) : x(ax), y(ay), z(az), w(aw) {}
};
inline Q qmul(const Q& a, const Q& b)
{
    return Q(a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z);
}
inline Q qconj(const Q& q) { return Q(-q.x, -q.y, -q.z, q.w); }
inline Q qnormalized(const Q& q)
{
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return (l > 1e-8f && std::isfinite(l)) ? Q(q.x / l, q.y / l, q.z / l, q.w / l) : Q();
}
inline bool finite(const Q& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }
inline V3 qrot(const Q& q, const V3& v)
{
    const V3 u(q.x, q.y, q.z);
    const V3 t = cross(u, v) * 2.f;
    return v + t * q.w + cross(u, t);
}
inline Q qaxis(const V3& axis, float angle)
{
    const V3 a = normalized(axis, V3(0.f, 0.f, 1.f));
    const float s = std::sin(0.5f * angle);
    return Q(a.x * s, a.y * s, a.z * s, std::cos(0.5f * angle));
}
inline Q qyaw(float yaw) { return qaxis(V3(0.f, 0.f, 1.f), yaw); }
// The heading of a rotation's forward (+X) in the horizontal plane (radians, CCW from +X).
// Looking (nearly) straight up or down the forward has no horizontal direction (a little head
// wobble would turn it anywhere): the heading then comes from the left axis (+Y), which stays
// level while the head only pitches.
inline float yawOf(const Q& q)
{
    const V3 f = qrot(q, V3(1.f, 0.f, 0.f));
    if (f.x * f.x + f.y * f.y >= 0.09f) return std::atan2(f.y, f.x);
    const V3 l = qrot(q, V3(0.f, 1.f, 0.f));
    float a = std::atan2(l.y, l.x) - 1.5707963f;
    if (a < -3.14159265f) a += 6.2831853f;
    return a;
}
// Shortest-path normalised lerp / slerp (t in [0, 1]).
inline Q qslerp(const Q& a, Q b, float t)
{
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.f)
    {
        b = Q(-b.x, -b.y, -b.z, -b.w);
        d = -d;
    }
    if (d > 0.9995f)
        return qnormalized(Q(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t));
    const float th = std::acos(d < 1.f ? d : 1.f);
    const float s = std::sin(th);
    const float wa = std::sin((1.f - t) * th) / s, wb = std::sin(t * th) / s;
    return qnormalized(Q(a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb));
}
// The smallest rotation taking direction a onto direction b.
inline Q qfromto(const V3& a, const V3& b)
{
    const V3 na = normalized(a), nb = normalized(b);
    const float d = dot(na, nb);
    if (d < -0.99999f)
    {
        V3 axis = cross(V3(1.f, 0.f, 0.f), na);
        if (length(axis) < 1e-3f) axis = cross(V3(0.f, 1.f, 0.f), na);
        return qaxis(axis, 3.14159265f);
    }
    const V3 c = cross(na, nb);
    return qnormalized(Q(c.x, c.y, c.z, 1.f + d));
}
// The angle (radians) between two rotations.
inline float qangle(const Q& a, const Q& b)
{
    const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.f * std::acos(d < 1.f ? d : 1.f);
}
// Rotation from an orthonormal basis given as columns (the images of X, Y, Z).
inline Q qfromBasis(const V3& c0, const V3& c1, const V3& c2)
{
    const float m00 = c0.x, m10 = c0.y, m20 = c0.z;
    const float m01 = c1.x, m11 = c1.y, m21 = c1.z;
    const float m02 = c2.x, m12 = c2.y, m22 = c2.z;
    const float tr = m00 + m11 + m22;
    Q q;
    if (tr > 0.f)
    {
        const float s = std::sqrt(tr + 1.f) * 2.f;
        q = Q((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s);
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
        q = Q(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
    }
    else if (m11 > m22)
    {
        const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
        q = Q((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
    }
    else
    {
        const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
        q = Q((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
    }
    return qnormalized(q);
}
// The rotation R with R * restA = tgtA (directions) and R * restB as close to tgtB as allowed
// (both B's made perpendicular to their A). Degenerate B's fall back to the smallest rotation.
inline Q qalignPair(const V3& restA, const V3& restB, const V3& tgtA, const V3& tgtB)
{
    const V3 ra = normalized(restA), ta = normalized(tgtA);
    V3 rb = restB - ra * dot(restB, ra), tb = tgtB - ta * dot(tgtB, ta);
    if (length(rb) < 1e-4f || length(tb) < 1e-4f) return qfromto(ra, ta);
    rb = normalized(rb);
    tb = normalized(tb);
    const Q fr = qfromBasis(ra, rb, cross(ra, rb));
    const Q ft = qfromBasis(ta, tb, cross(ta, tb));
    return qnormalized(qmul(ft, qconj(fr)));
}

// ---- OpenXR app space <-> body space ---------------------------------------------------------
// Body space: the VR room in SL axes (X forward, Y left, Z up), z above the floor. OpenXR app
// space: +X right, +Y up, -Z forward. SL = (-z, -x, y), less the floor's y. The axis map is a
// proper rotation, so a quaternion's vector part maps like a position.
inline V3 xrPosToBody(const float p[3], float floorY) { return V3(-p[2], -p[0], p[1] - floorY); }
inline Q xrRotToBody(const float q[4]) { return qnormalized(Q(-q[2], -q[0], q[1], q[3])); }
// The inverses (floorY 0): a body-space pose back into OpenXR app space.
inline void bodyPosToXr(const V3& b, float out[3])
{
    out[0] = -b.y;
    out[1] = b.z;
    out[2] = -b.x;
}
inline void bodyRotToXr(const Q& q, float out[4])
{
    const Q n = qnormalized(q);
    out[0] = -n.y;
    out[1] = n.z;
    out[2] = -n.x;
    out[3] = n.w;
}

// ---- Calibration: a tracker's fixed offset to its bone ---------------------------------------
struct Offset
{
    V3 pos;   // the bone's position in the tracker's frame
    Q rot;    // bone = tracker * rot
};
inline Offset calibrateOffset(const V3& trkPos, const Q& trkRot, const V3& bonePos, const Q& boneRot)
{
    Offset o;
    const Q inv = qconj(qnormalized(trkRot));
    o.rot = qnormalized(qmul(inv, boneRot));
    o.pos = qrot(inv, bonePos - trkPos);
    return o;
}
inline void applyOffset(const V3& trkPos, const Q& trkRot, const Offset& o, V3& bonePos, Q& boneRot)
{
    const Q t = qnormalized(trkRot);
    boneRot = qnormalized(qmul(t, o.rot));
    bonePos = trkPos + qrot(t, o.pos);
}

// ---- Roles -----------------------------------------------------------------------------------
enum Body : int { B_WAIST = 0, B_CHEST, B_LFOOT, B_RFOOT, B_LKNEE, B_RKNEE, B_COUNT };
inline const char* bodyName(int b)
{
    static const char* const k[B_COUNT] = {"hips", "chest", "left foot", "right foot", "left knee", "right knee"};
    return (b >= 0 && b < B_COUNT) ? k[b] : "?";
}

// Body proportions as fractions of the standing eye height above the floor (from the avatar's
// skeleton at calibration), and the eyes' distance in front of the spine (metres per metre of
// eye height).
struct Proportions
{
    float ankle = 0.055f, knee = 0.29f, pelvis = 0.56f, chest = 0.76f, eyeForward = 0.06f;
};

// Assign trackers to bones by where they are at calibration (the runtime gave no usable roles,
// or SlipstreamVRBodyAssignByPosition). pos: tracker positions in the room (SL axes, floor at
// z = 0), head: the headset, fwd: the facing (horizontal), eyeH: your eye height. out[b]: the
// index into pos for bone b, or -1. The two lowest near the floor are the feet (left / right by
// side), then the knees (shin height), the hips (nearest the pelvis height) and the chest.
inline void assignByPosition(const V3* pos, int n, const V3& head, const V3& fwd, float eyeH, const Proportions& pr,
                             int out[B_COUNT])
{
    for (int b = 0; b < B_COUNT; ++b) out[b] = -1;
    if (n <= 0 || eyeH < 0.3f) return;
    const V3 f = normalized(V3(fwd.x, fwd.y, 0.f));
    const V3 left = cross(V3(0.f, 0.f, 1.f), f);
    bool used[32] = {};
    const int cnt = n < 32 ? n : 32;
    auto side = [&](int i) { return dot(pos[i] - head, left); };   // > 0: on your left
    // Within a height band (fractions of eyeH), the best candidate for a target fraction.
    auto pick = [&](float lo, float hi, float target) {
        int best = -1;
        float bestD = 1e9f;
        for (int i = 0; i < cnt; ++i)
        {
            if (used[i]) continue;
            const float h = pos[i].z / eyeH;
            if (h < lo || h >= hi) continue;
            const float d = std::fabs(h - target);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best >= 0) used[best] = true;
        return best;
    };
    auto pair = [&](float lo, float hi, float target, int bl, int br) {
        const int a = pick(lo, hi, target), b = pick(lo, hi, target);
        if (a >= 0 && b >= 0)
        {
            const bool aLeft = side(a) >= side(b);
            out[bl] = aLeft ? a : b;
            out[br] = aLeft ? b : a;
        }
        else if (a >= 0)
        {
            out[side(a) >= 0.f ? bl : br] = a;
        }
    };
    const float kneeLo = 0.5f * (pr.ankle + pr.knee);
    const float hipLo = 0.5f * (pr.knee + pr.pelvis);
    const float chestLo = 0.5f * (pr.pelvis + pr.chest);
    pair(-0.2f, kneeLo, 0.f, B_LFOOT, B_RFOOT);
    pair(kneeLo, hipLo, pr.knee, B_LKNEE, B_RKNEE);
    out[B_WAIST] = pick(hipLo, chestLo, pr.pelvis);
    out[B_CHEST] = pick(chestLo, 0.95f, pr.chest);
}

// The bone pose a tracker stands for in a T-pose (calibration): facing `yaw`, rest pose
// (every bone's world rotation the facing), heights from the proportions. Feet and knees stay
// under their tracker; hips and chest on the spine under the head (the eyes are eyeForward in
// front of it).
inline void tposeBone(int body, const V3& trk, const V3& head, float yaw, float eyeH, const Proportions& pr,
                      V3& bonePos, Q& boneRot)
{
    boneRot = qyaw(yaw);
    const V3 spine = head - qrot(boneRot, V3(pr.eyeForward * eyeH, 0.f, 0.f));
    switch (body)
    {
    case B_LFOOT:
    case B_RFOOT: bonePos = V3(trk.x, trk.y, pr.ankle * eyeH); break;
    case B_LKNEE:
    case B_RKNEE: bonePos = V3(trk.x, trk.y, pr.knee * eyeH); break;
    case B_WAIST: bonePos = V3(spine.x, spine.y, pr.pelvis * eyeH); break;
    default:      bonePos = V3(spine.x, spine.y, pr.chest * eyeH); break;
    }
}

// ---- Two-bone leg IK -------------------------------------------------------------------------
// Rest offsets: the hip in the pelvis' frame, the knee in the hip's, the ankle in the knee's.
struct LegRest
{
    V3 hip, knee, ankle;
};
struct LegPose
{
    Q hipWorld, kneeWorld;
    V3 hipPos, kneePos, anklePos;
    bool reached = false;   // the target was within the leg's length (else the leg points at it)
    bool ok = false;
};
// The hip and knee world rotations that put the ankle on `target`, the knee bending toward
// `pole` (a point the knee should point at: a knee tracker, or in front of the leg). Keeps the
// knee a pure hinge about the bone's own Y (the SL knee). Never NaN: a target out of reach
// stretches the leg toward it, one at the hip folds it as far as the bones allow.
inline LegPose solveLeg(const V3& pelvisPos, const Q& pelvisRot, const LegRest& r, const V3& target, const V3& pole)
{
    LegPose out;
    out.hipPos = pelvisPos + qrot(pelvisRot, r.hip);
    const float l1 = length(r.knee), l2 = length(r.ankle);
    if (l1 < 1e-4f || l2 < 1e-4f || !finite(target) || !finite(pole)) return out;
    const V3 down = qrot(pelvisRot, V3(0.f, 0.f, -1.f));
    const V3 d = target - out.hipPos;
    const float raw = length(d);
    const V3 dir = normalized(d, down);
    const float lo = std::fabs(l1 - l2) + 1e-3f, hi = (l1 + l2) * 0.9999f;
    const float dist = raw < lo ? lo : (raw > hi ? hi : raw);
    out.reached = raw <= l1 + l2 + 1e-3f;
    float c = (l1 * l1 + dist * dist - l2 * l2) / (2.f * l1 * dist);
    c = c > 1.f ? 1.f : (c < -1.f ? -1.f : c);
    const float s = std::sqrt(1.f - c * c);
    // The bend direction: the pole's part across the hip -> target line (falls back to the
    // pelvis' forward, then its left).
    V3 p = pole - out.hipPos;
    V3 bend = p - dir * dot(p, dir);
    if (length(bend) < 1e-3f)
    {
        p = qrot(pelvisRot, V3(1.f, 0.f, 0.f));
        bend = p - dir * dot(p, dir);
        if (length(bend) < 1e-3f)
        {
            p = qrot(pelvisRot, V3(0.f, 1.f, 0.f));
            bend = p - dir * dot(p, dir);
        }
    }
    bend = normalized(bend);
    out.kneePos = out.hipPos + dir * (l1 * c) + bend * (l1 * s);
    out.anklePos = out.hipPos + dir * dist;
    const V3 thigh = out.kneePos - out.hipPos, shin = out.anklePos - out.kneePos;
    // Both bones share the leg plane's normal (the hinge): the knee's front is built from it,
    // not from `bend` (which flips against the shin in a deep fold: the foot would turn back).
    const V3 hinge = normalized(cross(normalized(thigh), bend), qrot(pelvisRot, V3(0.f, -1.f, 0.f)));
    out.hipWorld = qalignPair(r.knee, V3(1.f, 0.f, 0.f), thigh, cross(hinge, normalized(thigh)));
    out.kneeWorld = qalignPair(r.ankle, V3(1.f, 0.f, 0.f), shin, cross(hinge, normalized(shin)));
    out.ok = finite(out.hipWorld) && finite(out.kneeWorld) && finite(out.kneePos);
    return out;
}

// ---- Spine -----------------------------------------------------------------------------------
// Rest offsets: the torso in the pelvis' frame (through any Bento spine joints, held straight),
// the neck in the torso's frame (through the chest).
struct SpineRest
{
    V3 torso, neck;
};
struct SpinePose
{
    Q torsoWorld, chestWorld;
};
// Bend the spine from the pelvis toward the neck target (at most maxBend radians), and twist
// the chest toward the head's heading by twist (0..1) of the head-to-hips difference (at most
// maxTwist). The torso takes half of it, the chest the rest.
inline SpinePose solveSpine(const V3& pelvisPos, const Q& pelvisRot, const SpineRest& r, const V3& neckTarget, float headYaw,
                            float twist, float maxBend = 1.0f, float maxTwist = 1.2f)
{
    SpinePose out;
    const V3 torsoPos = pelvisPos + qrot(pelvisRot, r.torso);
    const V3 restUp = qrot(pelvisRot, r.neck);
    V3 want = neckTarget - torsoPos;
    if (length(want) < 1e-3f || !finite(want)) want = restUp;
    Q swing = qfromto(restUp, want);
    const float a = qangle(Q(), swing);
    if (a > maxBend) swing = qslerp(Q(), swing, maxBend / a);
    const Q bent = qmul(swing, pelvisRot);
    const V3 axis = normalized(qrot(bent, r.neck), V3(0.f, 0.f, 1.f));
    float dy = headYaw - yawOf(bent);
    while (dy > 3.14159265f) dy -= 6.2831853f;
    while (dy < -3.14159265f) dy += 6.2831853f;
    dy *= twist;
    if (dy > maxTwist) dy = maxTwist;
    if (dy < -maxTwist) dy = -maxTwist;
    // (About the spine's own axis; the heading difference stands in for the angle about it.)
    out.chestWorld = qnormalized(qmul(qaxis(axis, dy), bent));
    out.torsoWorld = qslerp(pelvisRot, out.chestWorld, 0.5f);
    return out;
}

// The hips under the head when no waist tracker is worn: the neck target less the rest
// pelvis -> neck vector, facing `yaw`.
inline V3 estimatePelvis(const V3& neckTarget, float yaw, const V3& pelvisToNeck)
{
    return neckTarget - qrot(qyaw(yaw), pelvisToNeck);
}

// ---- Room -> agent mapping -------------------------------------------------------------------
// A room point (body space: SL axes, z above the floor) in agent space: across, from the
// recentre reference (refXY, the room point that maps onto `anchor`), turned by the rig turn and
// scaled to the avatar (sv = avatar eye height / yours); up, from the avatar's feet. Never
// relative to the headset: your head swaying must not move a standing foot.
inline V3 roomToAgent(const V3& b, const V3& anchor, float refX, float refY, float turn, float sv, float footZ)
{
    const V3 d = qrot(qyaw(turn), V3(b.x - refX, b.y - refY, 0.f)) * sv;
    return V3(anchor.x + d.x, anchor.y + d.y, footZ + b.z * sv);
}

// ---- Time ------------------------------------------------------------------------------------
// A limb's weight fading in / out linearly over `seconds` (0.3 s: a tracker lost fades that limb
// back to the animation).
inline float fadeStep(float w, bool on, float dt, float seconds = 0.3f)
{
    const float step = seconds > 1e-4f ? dt / seconds : 1.f;
    w += on ? step : -step;
    return w < 0.f ? 0.f : (w > 1.f ? 1.f : w);
}
// Exponential smoothing toward a sample (time constant tau seconds; 0 = none). A jump past
// snapDist (re-centre, teleport, a tracker re-acquired elsewhere) is taken at once.
inline float smoothAlpha(float dt, float tau) { return tau > 1e-4f ? 1.f - std::exp(-dt / tau) : 1.f; }
inline void smoothPose(V3& pos, Q& rot, const V3& newPos, const Q& newRot, float dt, float tau, float snapDist = 0.75f)
{
    if (length(newPos - pos) > snapDist || !finite(pos) || !finite(rot))
    {
        pos = newPos;
        rot = newRot;
        return;
    }
    const float a = smoothAlpha(dt, tau);
    pos = lerp(pos, newPos, a);
    rot = qslerp(rot, newRot, a);
}
} // namespace slfbt
