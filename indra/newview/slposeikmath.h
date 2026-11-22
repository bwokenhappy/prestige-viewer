/**
 * @file slposeikmath.h
 * @brief Pose Studio IK (#530, #1566): the pure geometry of the solvers -- positions in,
 *        positions out. No viewer types, so scripts/check/test_pose_ik.cpp compiles and runs
 *        it on its own; slposeik.cpp turns the solved positions into bone rotations.
 *
 *   Limb (two-bone)   twoBone(): where the elbow / knee and the end go for a goal. The bend
 *                     side comes from a BendFrame captured once at mouse down: the plane the
 *                     limb bent in then keeps its normal while the goal moves, so the knee
 *                     never flips across the root -> goal line (only a goal straight along
 *                     that normal is ambiguous; there the fixed implicit pole decides). A pole
 *                     the user placed wins (Blender).
 *   Chains            fabrik(): every joint folds (tail, a pony leg grown to its hoof, the
 *                     neck + head + nose "reach" chain), warm-started from the pose so the
 *                     answer is a continuous function of the goal; straight when out of reach.
 *   Gaze              gazeShares(): how a look-at turn is shared, eyes first: the eyes turn
 *                     up to their limit, the head takes what is left up to its own, the neck
 *                     the rest (clamped). Bottom-up allocation, applied top-down.
 *                     eyeTurn(): one eye aimed from its own rest axis, within its cone;
 *                     autoEyeSet(): which eye bones "Auto" turns for the worn rigging.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SL_POSE_IK_MATH_H
#define SL_POSE_IK_MATH_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace SLPoseIKMath
{
struct V3
{
    double x = 0.0, y = 0.0, z = 0.0;
    V3() = default;
    V3(double px, double py, double pz) : x(px), y(py), z(pz) {}
    V3 operator+(const V3& o) const { return V3(x + o.x, y + o.y, z + o.z); }
    V3 operator-(const V3& o) const { return V3(x - o.x, y - o.y, z - o.z); }
    V3 operator*(double s) const { return V3(x * s, y * s, z * s); }
    V3 operator-() const { return V3(-x, -y, -z); }
};

inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline double length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalized(const V3& a, const V3& fallback)
{
    const double l = length(a);
    return (l > 1.0e-9) ? a * (1.0 / l) : fallback;
}
// The part of v at right angles to the unit vector n.
inline V3 across(const V3& v, const V3& n) { return v - n * dot(v, n); }
// Any unit vector at right angles to the unit vector v.
inline V3 anyPerpendicular(const V3& v)
{
    const V3 p = (std::fabs(v.x) < 0.9) ? cross(v, V3(1, 0, 0)) : cross(v, V3(0, 1, 0));
    return normalized(p, V3(0, 0, 1));
}
// Angle between two directions, radians.
inline double angleBetween(const V3& a, const V3& b)
{
    return std::atan2(length(cross(a, b)), dot(a, b));
}

// ---------------------------------------------------------------------------
// Two-bone limb
// ---------------------------------------------------------------------------

// Captured once per drag, from the limb as it was at mouse down.
struct BendFrame
{
    V3 mNormal;         // unit normal of the bend plane, oriented so that side = normal x n
    V3 mPole;           // implicit pole: a point in front of the knee at mouse down
    bool mValid = false;
};

// a / b / e: root, middle (elbow / knee), end at mouse down. bend_side: unit direction the
// knee should go when the limb is straight (in world space, already rotated with the root).
inline BendFrame captureBendFrame(const V3& a, const V3& b, const V3& e, const V3& bend_side)
{
    BendFrame frame;
    const double la = length(b - a);
    const double lb = length(e - b);
    const V3 n0 = normalized(e - a, normalized(b - a, V3(0, 0, -1)));
    V3 side = across(b - a, n0);
    if (length(side) < 0.05 * la)
        side = across(bend_side, n0);      // straight: the chain's own bend direction
    side = normalized(side, anyPerpendicular(n0));
    frame.mNormal = normalized(cross(n0, side), anyPerpendicular(n0));
    frame.mPole = b + side * (0.5 * (la + lb));
    frame.mValid = true;
    return frame;
}

// The direction from the root -> goal line the knee goes to, at right angles to n (unit).
// user_pole: the pole the user placed (null: none). Continuous in n except a goal exactly
// along the frame's normal, where the implicit pole takes over smoothly.
inline V3 bendSide(const V3& a, const V3& n, const BendFrame& frame, const V3* user_pole)
{
    if (user_pole)
    {
        const V3 to_pole = across(*user_pole - a, n);
        if (length(to_pole) > 1.0e-6)
            return normalized(to_pole, anyPerpendicular(n));
    }
    const V3 from_plane = cross(frame.mNormal, n);  // unit when n lies in the plane
    const double strength = length(from_plane);
    const V3 from_pole = normalized(across(frame.mPole - a, n), anyPerpendicular(n));
    if (strength >= 0.4)
        return from_plane * (1.0 / strength);
    if (strength <= 0.15)
        return from_pole;
    // Near the normal: ease over to the implicit pole (no jump).
    const double t = (strength - 0.15) / 0.25;
    const double w = t * t * (3.0 - 2.0 * t);
    return normalized(from_plane * (w / strength) + from_pole * (1.0 - w), from_pole);
}

struct TwoBoneResult
{
    V3 mKnee;           // where the middle joint goes
    V3 mReach;          // where the end joint goes (the goal, or as near as the limb allows)
    double mDistance = 0.0;
};

// la / lb: upper and lower bone lengths; max_bend_deg: how far the hinge may fold from
// straight; keep_bend_deg > 0: never fully straight (a set pole needs a bend to point with).
inline TwoBoneResult twoBone(const V3& a, double la, double lb, const V3& goal, const V3& side,
                             double max_bend_deg, double keep_bend_deg)
{
    TwoBoneResult r;
    const double deg = 3.14159265358979323846 / 180.0;
    const V3 n = normalized(goal - a, anyPerpendicular(side));
    const double dt = length(goal - a);
    const double inner_min = (180.0 - max_bend_deg) * deg;
    const double folded = std::sqrt(std::max(0.0, la * la + lb * lb - 2.0 * la * lb * std::cos(inner_min)));
    const double min_d = std::max(std::fabs(la - lb) + 1.0e-3, folded);
    double max_d = la + lb - 1.0e-5;
    if (keep_bend_deg > 0.0)
        max_d = std::min(max_d, std::sqrt(la * la + lb * lb + 2.0 * la * lb * std::cos(keep_bend_deg * deg)));
    const double d = (min_d < max_d) ? std::min(std::max(dt, min_d), max_d) : max_d;
    const double cos_a = std::min(1.0, std::max(-1.0, (la * la + d * d - lb * lb) / (2.0 * la * d)));
    const double sin_a = std::sqrt(std::max(0.0, 1.0 - cos_a * cos_a));
    r.mKnee = a + n * (la * cos_a) + side * (la * sin_a);
    r.mReach = a + n * d;
    r.mDistance = d;
    return r;
}

// ---------------------------------------------------------------------------
// FABRIK
// ---------------------------------------------------------------------------

// p: joint positions root -> end (the solve's start pose). Returns the solved positions: the
// root stays, every link keeps its length, the end reaches the goal when it can, else the
// chain points straight at it.
inline std::vector<V3> fabrik(const std::vector<V3>& p, const V3& goal, int iterations, double tolerance)
{
    std::vector<V3> q = p;
    const size_t count = p.size();
    if (count < 2)
        return q;
    std::vector<double> len(count - 1);
    double total = 0.0;
    for (size_t i = 0; i + 1 < count; ++i)
    {
        len[i] = length(p[i + 1] - p[i]);
        total += len[i];
    }
    const V3 root = p[0];
    if (length(goal - root) >= total - 1.0e-9)
    {
        const V3 dir = normalized(goal - root, normalized(p[count - 1] - root, V3(1, 0, 0)));
        for (size_t i = 0; i + 1 < count; ++i)
            q[i + 1] = q[i] + dir * len[i];
        return q;
    }
    for (int it = 0; it < iterations; ++it)
    {
        q[count - 1] = goal;
        for (size_t k = count - 1; k-- > 0;)
            q[k] = q[k + 1] + normalized(q[k] - q[k + 1], normalized(p[k] - p[k + 1], V3(1, 0, 0))) * len[k];
        q[0] = root;
        for (size_t k = 0; k + 1 < count; ++k)
            q[k + 1] = q[k] + normalized(q[k + 1] - q[k], normalized(p[k + 1] - p[k], V3(1, 0, 0))) * len[k];
        if (length(q[count - 1] - goal) < tolerance)
            break;
    }
    return q;
}

// ---------------------------------------------------------------------------
// Gaze: eyes -> head -> neck
// ---------------------------------------------------------------------------

struct GazeShares
{
    double mEyes = 0.0;     // radians each part turns toward the target
    double mHead = 0.0;
    double mNeck = 0.0;
    double mUnreached = 0.0;
};

// total: the turn asked for (radians, >= 0); limits: what each part can turn on its own.
inline GazeShares gazeShares(double total, double eye_limit, double head_limit, double neck_limit)
{
    GazeShares s;
    double rest = std::max(0.0, total);
    s.mEyes = std::min(rest, std::max(0.0, eye_limit));
    rest -= s.mEyes;
    s.mHead = std::min(rest, std::max(0.0, head_limit));
    rest -= s.mHead;
    s.mNeck = std::min(rest, std::max(0.0, neck_limit));
    rest -= s.mNeck;
    s.mUnreached = rest;
    return s;
}

// One eye's turn: from rest (the eye's aim axis in the world at rest: its set's forward
// turned by the eye's parent) toward dir (eye -> goal), at most limit radians. A world-space
// turn applied after the parent; angle 0 when dir already lies along rest.
struct EyeTurn
{
    V3 mAxis{ 0, 0, 1 };
    double mAngle = 0.0;
};

inline EyeTurn eyeTurn(const V3& rest, const V3& dir, double limit)
{
    EyeTurn t;
    const V3 axis = cross(rest, dir);
    if (length(axis) <= 1.0e-9)
        return t;
    t.mAxis = normalized(axis, V3(0, 0, 1));
    t.mAngle = std::min(angleBetween(rest, dir), std::max(0.0, limit));
    return t;
}

// Which eye bones "Auto" turns (SLPoseStudioIKEyeBones 0), from what the worn rigged meshes
// weight: a Bento mesh head's alternate eyes (with the classic eyes rigged too: both), else
// the classic eyes, else the nose left / right bones when no eye bone is rigged (the Open
// Pony rig's eyes live there), else the classic eyes (system avatar, mesh still loading).
enum EyeSetPick
{
    EYE_SET_EYES = 0,   // mEyeLeft / mEyeRight
    EYE_SET_ALT,        // mFaceEyeAltLeft / Right
    EYE_SET_NOSE,       // mFaceNoseLeft / Right
    EYE_SET_BOTH,       // eyes + alt
};

inline EyeSetPick autoEyeSet(bool eyes_rigged, bool alt_rigged, bool nose_rigged)
{
    if (alt_rigged)
        return eyes_rigged ? EYE_SET_BOTH : EYE_SET_ALT;
    if (eyes_rigged)
        return EYE_SET_EYES;
    return nose_rigged ? EYE_SET_NOSE : EYE_SET_EYES;
}

// ---------------------------------------------------------------------------
// Live look-at, natural gaze (#1567)
// ---------------------------------------------------------------------------

// Eyes lead, the head follows: while the look stays within eye_range (radians) of where the
// head is going, only the eyes move (a glance); once it leaves that range the head's goal
// follows the look until the head has caught up (within settle), and the eyes re-centre as it
// turns. yaw / pitch: the look; head_yaw / head_pitch: where the head is now (smoothed).
struct HeadFollow
{
    double mGoalYaw = 0.0;
    double mGoalPitch = 0.0;
    bool mTracking = false;
};

inline double wrapPi(double a)
{
    const double two_pi = 6.283185307179586;
    a = std::fmod(a + 3.141592653589793, two_pi);
    if (a < 0.0)
        a += two_pi;
    return a - 3.141592653589793;
}

inline void headFollow(HeadFollow& h, double yaw, double pitch, double head_yaw, double head_pitch,
                       double eye_range, double settle)
{
    const double off = std::hypot(wrapPi(yaw - h.mGoalYaw), pitch - h.mGoalPitch);
    if (off > eye_range)
        h.mTracking = true;
    if (!h.mTracking)
        return;
    h.mGoalYaw = yaw;
    h.mGoalPitch = pitch;
    if (std::hypot(wrapPi(head_yaw - yaw), head_pitch - pitch) <= settle)
        h.mTracking = false;
}

// Where both eyes aim: the target point itself (each eye turns toward it from its own place,
// so they converge at its real distance). distance <= 0 (unknown): default; very close
// targets are held at min_distance (LL's eye motion does the same for the classic eyes).
inline double gazeDistance(double distance, double min_distance, double max_distance, double unknown)
{
    if (!(distance > 0.0))
        return unknown;
    return std::min(std::max(distance, min_distance), max_distance);
}

// ---------------------------------------------------------------------------
// Pupil eyes (#1567): eyes that look by sliding, not turning
// ---------------------------------------------------------------------------

// The Open Pony's eyes are pupils on mFaceNoseLeft / Right that slide across the eye: looking
// moves the bone along the eye's surface. One bone's mapping (ik_chains.xml, an eye set of type
// "pupil"), everything in the bone's parent frame (every SL bone binds unrotated: +X forward, +Y
// left, +Z up) and relative to the bone's rest position, in metres:
//
//   model "shell" (the Open Pony dev kit's own eye-tracking rig, read from its .blend): an arm of
//                 length mArm pivots at mPivot and aims at the look target plus mAimOffset turned
//                 by the look's yaw around mAimCenter (the rig aims each eye at the focus point
//                 moved sideways by its own offset along the focus's side axis, so the eyes stay
//                 parallel instead of crossing); the pupil is the point of the eye shell nearest
//                 the arm's tip, and the bone turns so that its rest axis mAxis (head -> tail)
//                 lies along the shell's normal there -- the rig's Copy Location (IKEye*, tail) +
//                 Shrinkwrap (nearest surface point, "Align to Normal" on the bone's Y) onto
//                 OBIKEye. The shell is an ellipsoid: mShellCenter and three semi-axis vectors
//                 mShellAxes (at right angles; a sphere: three equal ones). The pupils are curved
//                 discs as wide as the eye: slid without the turn, one edge sinks into the eye
//                 white and the other lifts off it (~2.5 mm at 30 degrees).
//   model "linear" (any other rig, from gains): the look's yaw (left +) / pitch (up +) in degrees
//                 times mGainH / mGainV along mHAxis / mVAxis (signs mHSign / mVSign), plus
//                 mVergence / distance toward the nose (mInward), kept inside the mRangeH x
//                 mRangeV ellipse. No turn.
//   Both: the look is held inside a cone of `limit` radians around forward (the eye range), and
//   mCenter is added (a calibrated rest offset).
struct PupilBone
{
    enum Model { LINEAR = 0, SHELL = 1 };
    int mModel = LINEAR;
    // shell
    V3 mPivot;
    double mArm = 0.0;
    V3 mAimOffset;
    V3 mAimCenter;
    V3 mShellCenter;
    V3 mShellAxes[3];
    V3 mAxis;           // zero: the bone is never turned
    // linear
    int mHAxis = 1;
    double mHSign = 1.0;
    int mVAxis = 2;
    double mVSign = 1.0;
    double mGainH = 0.0;
    double mGainV = 0.0;
    double mRangeH = 0.0;
    double mRangeV = 0.0;
    double mVergence = 0.0;
    double mInward = -1.0;
    // both
    V3 mCenter;
};

// Where one pupil goes: its position offset from rest and its turn (both in its parent's frame;
// the turn takes the bone's rest axis onto the shell normal, angle 0 when it is not turned).
struct PupilPose
{
    V3 mOffset;
    EyeTurn mTurn;
};

inline double& axisRef(V3& v, int axis)
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

// Yaw (left +) and pitch (up +) of dir, radians, around the forward axis with +Z up.
inline void lookAngles(const V3& dir, const V3& forward, double& yaw, double& pitch)
{
    const V3 f = normalized(forward, V3(1, 0, 0));
    const V3 up(0, 0, 1);
    const V3 left = normalized(cross(up, f), V3(0, 1, 0));
    const double fx = dot(dir, f), ly = dot(dir, left), uz = dot(dir, up);
    yaw = std::atan2(ly, fx);
    pitch = std::atan2(uz, std::hypot(fx, ly));
}

// dir turned toward forward so it lies within limit radians of it (unit result).
inline V3 withinCone(const V3& dir, const V3& forward, double limit)
{
    const V3 f = normalized(forward, V3(1, 0, 0));
    const V3 d = normalized(dir, f);
    if (limit < 0.0)
        return d;
    const double angle = angleBetween(f, d);
    if (angle <= limit)
        return d;
    const V3 side = normalized(across(d, f), anyPerpendicular(f));
    return f * std::cos(limit) + side * std::sin(limit);
}

// Nearest point of an ellipse / ellipsoid with semi-axes e0 >= e1 (>= e2) to a point y with
// every coordinate >= 0, in the shape's own axes (D. Eberly, "Distance from a Point to an
// Ellipse, an Ellipsoid, or a Hyperellipsoid": bisection on the Lagrange multiplier; also right
// for points inside).
inline double ellipseRoot(const double* r, const double* z, int n, double g)
{
    double n0 = 0.0, scale = 0.0;
    for (int i = 0; i < n; ++i)
        scale = std::max(scale, std::fabs(r[i] * z[i]));
    if (g >= 0.0 && scale > 0.0)
    {
        for (int i = 0; i < n; ++i)
            n0 += (r[i] * z[i] / scale) * (r[i] * z[i] / scale);
        n0 = scale * std::sqrt(n0);
    }
    double s0 = z[n - 1] - 1.0, s1 = (g < 0.0) ? 0.0 : n0 - 1.0, s = 0.0;
    for (int it = 0; it < 200; ++it)
    {
        s = 0.5 * (s0 + s1);
        if (s == s0 || s == s1)
            break;
        double gs = -1.0;
        for (int i = 0; i < n; ++i)
            gs += (r[i] * z[i] / (s + r[i])) * (r[i] * z[i] / (s + r[i]));
        if (gs > 0.0)
            s0 = s;
        else if (gs < 0.0)
            s1 = s;
        else
            break;
    }
    return s;
}

inline void ellipseNearest(double e0, double e1, double y0, double y1, double& x0, double& x1)
{
    if (y1 > 0.0)
    {
        if (y0 > 0.0)
        {
            const double z[2] = { y0 / e0, y1 / e1 };
            const double g = z[0] * z[0] + z[1] * z[1] - 1.0;
            if (g != 0.0)
            {
                const double r[2] = { (e0 / e1) * (e0 / e1), 1.0 };
                const double s = ellipseRoot(r, z, 2, g);
                x0 = r[0] * y0 / (s + r[0]);
                x1 = y1 / (s + 1.0);
                return;
            }
            x0 = y0;
            x1 = y1;
            return;
        }
        x0 = 0.0;
        x1 = e1;
        return;
    }
    const double numer = e0 * y0, denom = e0 * e0 - e1 * e1;
    if (numer < denom)
    {
        const double xd = numer / denom;
        x0 = e0 * xd;
        x1 = e1 * std::sqrt(std::max(0.0, 1.0 - xd * xd));
        return;
    }
    x0 = e0;
    x1 = 0.0;
}

inline void ellipsoidNearest(const double e[3], const double y[3], double x[3])
{
    if (y[2] > 0.0)
    {
        if (y[1] > 0.0)
        {
            if (y[0] > 0.0)
            {
                const double z[3] = { y[0] / e[0], y[1] / e[1], y[2] / e[2] };
                const double g = z[0] * z[0] + z[1] * z[1] + z[2] * z[2] - 1.0;
                if (g != 0.0)
                {
                    const double r[3] = { (e[0] / e[2]) * (e[0] / e[2]), (e[1] / e[2]) * (e[1] / e[2]), 1.0 };
                    const double s = ellipseRoot(r, z, 3, g);
                    for (int i = 0; i < 3; ++i)
                        x[i] = r[i] * y[i] / (s + r[i]);
                    return;
                }
                x[0] = y[0];
                x[1] = y[1];
                x[2] = y[2];
                return;
            }
            x[0] = 0.0;
            ellipseNearest(e[1], e[2], y[1], y[2], x[1], x[2]);
            return;
        }
        x[1] = 0.0;
        if (y[0] > 0.0)
        {
            ellipseNearest(e[0], e[2], y[0], y[2], x[0], x[2]);
            return;
        }
        x[0] = 0.0;
        x[2] = e[2];
        return;
    }
    const double d0 = e[0] * e[0] - e[2] * e[2], d1 = e[1] * e[1] - e[2] * e[2];
    const double n0 = e[0] * y[0], n1 = e[1] * y[1];
    if (n0 < d0 && n1 < d1)
    {
        const double a = n0 / d0, b = n1 / d1, disc = 1.0 - a * a - b * b;
        if (disc > 0.0)
        {
            x[0] = e[0] * a;
            x[1] = e[1] * b;
            x[2] = e[2] * std::sqrt(disc);
            return;
        }
    }
    x[2] = 0.0;
    ellipseNearest(e[0], e[1], y[0], y[1], x[0], x[1]);
}

// The nearest point of the front half (x2 >= 0) of an ellipsoid (semi-axes e[0] >= e[1] >= e[2],
// its own axes) to y, a point behind its centre plane (y[2] < 0), where the nearest point of the
// whole ellipsoid would be on its back: the distance's local minimum over the front, by Newton
// on (a, b) = (x0 / e0, x1 / e1), x2 = e2 sqrt(1 - a^2 - b^2), from start (a front point).
inline void ellipsoidFrontNearest(const double e[3], const double y[3], const double start[3], double x[3])
{
    double a = start[0] / e[0], b = start[1] / e[1];
    for (int it = 0; it < 50; ++it)
    {
        const double h = std::sqrt(std::max(1.0e-12, 1.0 - a * a - b * b));
        const double ha = -a / h, hb = -b / h;
        const double h3 = h * h * h;
        const double haa = -(1.0 - b * b) / h3, hbb = -(1.0 - a * a) / h3, hab = -a * b / h3;
        const double r2 = e[2] * h - y[2];
        const double fa = 2.0 * e[0] * (e[0] * a - y[0]) + 2.0 * e[2] * r2 * ha;
        const double fb = 2.0 * e[1] * (e[1] * b - y[1]) + 2.0 * e[2] * r2 * hb;
        const double faa = 2.0 * e[0] * e[0] + 2.0 * e[2] * e[2] * ha * ha + 2.0 * e[2] * r2 * haa;
        const double fbb = 2.0 * e[1] * e[1] + 2.0 * e[2] * e[2] * hb * hb + 2.0 * e[2] * r2 * hbb;
        const double fab = 2.0 * e[2] * e[2] * ha * hb + 2.0 * e[2] * r2 * hab;
        const double det = faa * fbb - fab * fab;
        double da, db;
        if (faa > 0.0 && det > 0.0)
        {
            da = -(fbb * fa - fab * fb) / det;
            db = -(faa * fb - fab * fa) / det;
        }
        else
        {
            da = -fa * 1.0e-4;
            db = -fb * 1.0e-4;
        }
        // Stay inside the front (short of its rim).
        const double na = a + da, nb = b + db, r = std::hypot(na, nb);
        const double keep = (r > 0.999) ? 0.999 / r : 1.0;
        const double step = std::fabs(na * keep - a) + std::fabs(nb * keep - b);
        a = na * keep;
        b = nb * keep;
        if (step < 1.0e-12)
            break;
    }
    x[0] = e[0] * a;
    x[1] = e[1] * b;
    x[2] = e[2] * std::sqrt(std::max(0.0, 1.0 - a * a - b * b));
}

// The point of the shell (an ellipsoid: centre, three semi-axis vectors at right angles) nearest
// p, and the shell's outward normal there. Only the shell's front counts: the half, along its
// shortest axis (the eye's outward direction), that holds `front` (the bind pupil). The real eye
// mesh is not this ellipsoid behind its centre plane, and the rig's arm tip goes there on looks
// across toward the nose (~40 mm under the eye): then the front's own nearest point, as the rig
// finds on its mesh.
inline bool shellNearest(const V3& center, const V3 axes[3], const V3& front, const V3& p, V3& point, V3& normal)
{
    int order[3] = { 0, 1, 2 };
    double radii[3];
    for (int i = 0; i < 3; ++i)
        radii[i] = length(axes[i]);
    std::sort(order, order + 3, [&radii](int a, int b) { return radii[a] > radii[b]; });
    V3 u[3];
    double e[3], y[3], sign[3], x[3];
    for (int k = 0; k < 3; ++k)
    {
        e[k] = radii[order[k]];
        if (!(e[k] > 1.0e-9))
            return false;
        u[k] = axes[order[k]] * (1.0 / e[k]);
    }
    if (dot(front - center, u[2]) < 0.0)
        u[2] = -u[2];
    for (int k = 0; k < 3; ++k)
    {
        y[k] = dot(p - center, u[k]);
        sign[k] = (y[k] < 0.0) ? -1.0 : 1.0;
    }
    const double ay[3] = { std::fabs(y[0]), std::fabs(y[1]), std::fabs(y[2]) };
    ellipsoidNearest(e, ay, x);
    for (int k = 0; k < 3; ++k)
        x[k] *= sign[k];
    if (y[2] < 0.0)
    {
        // Behind the centre plane: start from the front point nearest p's mirror image.
        const double start[3] = { x[0], x[1], -x[2] };
        ellipsoidFrontNearest(e, y, start, x);
    }
    point = center;
    V3 n;
    for (int k = 0; k < 3; ++k)
    {
        point = point + u[k] * x[k];
        n = n + u[k] * (x[k] / (e[k] * e[k]));
    }
    normal = normalized(n, u[2]);
    return true;
}

// Where one pupil goes to look at a target that lies at to_target from the pupil's rest position
// (each eye uses its own, so a near target is seen from each eye's place). forward: the eye
// set's straight-ahead axis.
inline PupilPose pupilPose(const PupilBone& p, const V3& to_target, const V3& forward, double limit)
{
    PupilPose out;
    out.mOffset = p.mCenter;
    if (p.mModel == PupilBone::SHELL)
    {
        if (!(p.mArm > 0.0))
            return out;
        // The rig's target offset rides the focus, which faces the eyes' centre: turned by the
        // look's yaw (seen from that centre) about the vertical.
        double yaw = 0.0, pitch = 0.0;
        lookAngles(to_target - p.mAimCenter, forward, yaw, pitch);
        const double cy = std::cos(yaw), sy = std::sin(yaw);
        const V3 aim_offset(cy * p.mAimOffset.x - sy * p.mAimOffset.y, sy * p.mAimOffset.x + cy * p.mAimOffset.y, p.mAimOffset.z);
        const V3 aim = withinCone(to_target + aim_offset - p.mPivot, forward, limit);
        V3 point, normal;
        if (!shellNearest(p.mShellCenter, p.mShellAxes, V3(), p.mPivot + aim * p.mArm, point, normal))
            return out;
        out.mOffset = out.mOffset + point;
        if (length(p.mAxis) > 1.0e-9)
            out.mTurn = eyeTurn(normalized(p.mAxis, normal), normal, 3.14159265358979323846);
        return out;
    }
    const V3 dir = to_target + p.mAimOffset;
    if (length(dir) <= 1.0e-9)
        return out;
    double yaw = 0.0, pitch = 0.0;
    lookAngles(withinCone(dir, forward, limit), forward, yaw, pitch);
    const double to_deg = 180.0 / 3.14159265358979323846;
    double h = p.mHSign * p.mGainH * yaw * to_deg;
    double v = p.mVSign * p.mGainV * pitch * to_deg;
    const double distance = length(dir);
    if (distance > 1.0e-3)
        h += p.mInward * p.mVergence / distance;
    // The ellipse: scale back onto its rim when outside.
    if (p.mRangeH > 0.0 && p.mRangeV > 0.0)
    {
        const double e = std::hypot(h / p.mRangeH, v / p.mRangeV);
        if (e > 1.0)
        {
            h /= e;
            v /= e;
        }
    }
    else
    {
        if (p.mRangeH > 0.0) h = std::min(std::max(h, -p.mRangeH), p.mRangeH);
        if (p.mRangeV > 0.0) v = std::min(std::max(v, -p.mRangeV), p.mRangeV);
    }
    if (p.mHAxis >= 0 && p.mHAxis <= 2)
        axisRef(out.mOffset, p.mHAxis) += h;
    if (p.mVAxis >= 0 && p.mVAxis <= 2)
        axisRef(out.mOffset, p.mVAxis) += v;
    return out;
}

// ---------------------------------------------------------------------------
// "A worn rigged mesh uses this joint" (#1566)
// ---------------------------------------------------------------------------

// One worn rigged mesh's contribution, by joint number into used: the joints its vertices
// are weighted to when those weights are on the CPU (weighted, from the faces' rigging
// info), else every joint its skin lists (listed, LLMeshSkinInfo: always there, also when
// the mesh skins on the GPU and keeps no CPU weights). Negative / out of range numbers are
// skipped. A skin list longer than max_listed says nothing (Bento-ready bodies list every
// bone, wings and hind limbs included, at zero weight: counting them would make a human
// look like a pony to the look-at's Auto and show tail / wing bones on everyone), so such a
// mesh adds nothing without CPU weights. Returns how many joints it newly marked.
inline int addMeshJoints(std::vector<bool>& used, const std::vector<int>& weighted, const std::vector<int>& listed,
                         size_t max_listed = 48)
{
    static const std::vector<int> none;
    const std::vector<int>& from = !weighted.empty() ? weighted : (listed.size() <= max_listed ? listed : none);
    int added = 0;
    for (int num : from)
    {
        if (num < 0 || num >= (int)used.size() || used[num])
            continue;
        used[num] = true;
        ++added;
    }
    return added;
}

// ---------------------------------------------------------------------------
// Targets follow their anchor (#1566): the bone a chain hangs from
// ---------------------------------------------------------------------------

// A bone's frame in the world: its origin and its local X / Y / Z axes (unit, orthogonal).
struct Frame
{
    V3 mOrigin;
    V3 mX{ 1, 0, 0 }, mY{ 0, 1, 0 }, mZ{ 0, 0, 1 };
};

// Where point p goes when the frame it rides on moves from was to now: same place in the
// frame (position and orientation relative to the parent bone kept).
inline V3 carryPoint(const Frame& was, const Frame& now, const V3& p)
{
    const V3 d = p - was.mOrigin;
    return now.mOrigin + now.mX * dot(d, was.mX) + now.mY * dot(d, was.mY) + now.mZ * dot(d, was.mZ);
}

// A direction riding on the frame (a target's axis).
inline V3 carryDir(const Frame& was, const Frame& now, const V3& v)
{
    return now.mX * dot(v, was.mX) + now.mY * dot(v, was.mY) + now.mZ * dot(v, was.mZ);
}

// Whether a chain's IK target rides along when the bone its chain hangs from moves (moved,
// turned by another solve, an FK drag, the dials, a body box). A pinned target (a pinned limb,
// a pinned look-at) holds and its chain re-solves; a planted leg holds; the chain being
// dragged takes the mouse; a target never placed already follows its own bones.
inline bool targetFollowsAnchor(bool placed, bool pinned, bool planted, bool dragged)
{
    return placed && !pinned && !planted && !dragged;
}
} // namespace SLPoseIKMath

#endif // SL_POSE_IK_MATH_H
