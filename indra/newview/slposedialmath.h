/**
 * @file slposedialmath.h
 * @brief Pose Studio (#500): the dependency-free math behind the dials -- the OP Pose HUD
 *        generator's forward model (used by SLBoneModel, slposemath.cpp) and the dial's
 *        mouse drag. Header-only, plain doubles / floats, so it can be checked on its own
 *        (tests/slposedialmath_test.cpp).
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

#ifndef SL_POSEDIALMATH_H
#define SL_POSEDIALMATH_H

#include <cmath>

namespace SLPoseGen
{
// The generator's types and helpers, on doubles, in its conventions (Hamilton products,
// left applied last). Names follow src/Anim.c.
struct HQ
{
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
};

struct DV
{
    double x = 0.0, y = 0.0, z = 0.0;
};

constexpr double PI_D = 3.14159265358979323846;

// The same comparisons as llclamp / llmax, so the results match bit for bit.
inline double clampD(double a, double lo, double hi)
{
    return (a < lo) ? lo : ((a > hi) ? hi : a);
}

inline double maxD(double a, double b)
{
    return (b < a) ? a : b;
}

// vecDegToRad: deg * M_PI / 180.0, in exactly that order. The generator hits the sphere's
// singular points (a swing of 180 degrees about a near-zero cross product) where the last
// bit decides the swing axis, so the arithmetic has to match bit for bit (see offLegPole
// for the one value dialToQuat moves off).
inline double degToRad(double deg)
{
    return deg * PI_D / 180.0;
}

inline HQ hmulRaw(const HQ& l, const HQ& r)
{
    HQ q;
    q.w = l.w * r.w - l.x * r.x - l.y * r.y - l.z * r.z;
    q.x = l.w * r.x + l.x * r.w + l.y * r.z - l.z * r.y;
    q.y = l.w * r.y - l.x * r.z + l.y * r.w + l.z * r.x;
    q.z = l.w * r.z + l.x * r.y - l.y * r.x + l.z * r.w;
    return q;
}

// multiplyQuaternions: Hamilton product, normalized.
inline HQ hmul(const HQ& l, const HQ& r)
{
    HQ q = hmulRaw(l, r);
    const double n = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n > 0.0)
    {
        q.w /= n; q.x /= n; q.y /= n; q.z /= n;
    }
    return q;
}

inline HQ conjQ(const HQ& q)
{
    HQ c = q;
    c.x = -c.x; c.y = -c.y; c.z = -c.z;
    return c;
}

// eulerToQuaternion
inline HQ eulerQ(double ex, double ey, double ez)
{
    const double cr = cos(ex * 0.5), sr = sin(ex * 0.5);
    const double cp = cos(ey * 0.5), sp = sin(ey * 0.5);
    const double cy = cos(ez * 0.5), sy = sin(ez * 0.5);
    HQ q;
    q.w = cr * cp * cy + sr * sp * sy;
    q.x = sr * cp * cy - cr * sp * sy;
    q.y = cr * sp * cy + sr * cp * sy;
    q.z = cr * cp * sy - sr * sp * cy;
    return q;
}

// sphericalToCartesian, including its theta > pi remap.
inline DV sph(double theta, double phi)
{
    if (theta > PI_D)
    {
        theta = 2.0 * PI_D - theta;
        phi = fmod(phi + PI_D, 2.0 * PI_D);
    }
    DV v;
    v.x = sin(theta) * cos(phi);
    v.y = sin(theta) * sin(phi);
    v.z = cos(theta);
    return v;
}

inline DV normV(const DV& v)
{
    if (v.x == 0.0 && v.y == 0.0 && v.z == 0.0)
        return v;
    const double n = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    DV r;
    r.x = v.x / n; r.y = v.y / n; r.z = v.z / n;
    return r;
}

inline DV crossV(const DV& a, const DV& b)
{
    DV r;
    r.x = a.y * b.z - a.z * b.y;
    r.y = a.z * b.x - a.x * b.z;
    r.z = a.x * b.y - a.y * b.x;
    return r;
}

inline double dotV(const DV& a, const DV& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// axisAngleToQuaternion (a zero axis gives the identity)
inline HQ aaQ(const DV& v, double angle)
{
    HQ q;
    const double n = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (n > 0.0)
    {
        const double s = sin(angle / 2.0);
        q.x = v.x / n * s; q.y = v.y / n * s; q.z = v.z / n * s;
        q.w = cos(angle / 2.0);
    }
    return q;
}

// calculateAxisAngle(v1, v2) as a quaternion (acos clamped: his unclamped acos of a dot
// a hair above 1 only ever happens with a zero cross product, which is the identity anyway)
inline HQ swingQ(const DV& from, const DV& to)
{
    const DV a = normV(from), b = normV(to);
    return aaQ(normV(crossV(a, b)), acos(clampD(dotV(a, b), -1.0, 1.0)));
}

inline DV rotateV(const HQ& q, const DV& v)
{
    HQ p;
    p.w = 0.0; p.x = v.x; p.y = v.y; p.z = v.z;
    const HQ r = hmulRaw(hmulRaw(q, p), conjQ(q));
    DV out;
    out.x = r.x; out.y = r.y; out.z = r.z;
    return out;
}

inline DV refAxis(int axis)
{
    DV r;
    if (axis == 0) r.x = 1.0;
    else if (axis == 1) r.y = 1.0;
    else r.z = 1.0;
    return r;
}

// One bone's generator settings (SLBoneModel's, bone_axes.xml).
struct Model
{
    int axis = 0;               // twist axis: 0 X, 1 Y, 2 Z
    bool leg = false;           // leg twist mitigation
    double pre[3] = { 0.0, 0.0, 0.0 };  // pre_rotate, degrees
    HQ base;                    // rest rotation
    double offset[3] = { 0.0, -90.0, 0.0 };
    double invert[3] = { 1.0, 1.0, 1.0 };
};

// rotate_quats_spherical for applied angles (radians), then the .anim writer's w rebuild.
inline HQ forwardApplied(const Model& m, double ax, double ay, double az, bool invert)
{
    DV pos;
    HQ swing, twist;
    if (m.axis == 0)
    {
        pos = sph(ay, az);
        swing = swingQ(refAxis(0), pos);
        twist = aaQ(pos, ax);
    }
    else if (m.axis == 1)
    {
        pos = sph(ax, az);
        swing = swingQ(refAxis(1), pos);
        twist = aaQ(pos, ay);
    }
    else
    {
        pos = sph(ay, ax);
        swing = swingQ(refAxis(2), pos);
        if (m.leg)
        {
            const double add = (ay < PI_D / 2.0) ? ax * sin(ay) : ax - ax * cos(ay);
            twist = aaQ(pos, az + add);
        }
        else
        {
            twist = aaQ(pos, az);
        }
    }

    HQ rot = m.base;
    const HQ pre = eulerQ(degToRad(m.pre[0]), degToRad(m.pre[1]), degToRad(m.pre[2]));
    if (m.leg)
    {
        if (ay > 0.0 && ay < PI_D)
            rot = hmul(rot, pre);
        else if (ay >= PI_D || ay < -PI_D / 2.0)
            rot = hmul(rot, eulerQ(0.0, PI_D * 1.5, 0.0));
        else
            rot = hmul(rot, eulerQ(0.0, -PI_D / 2.0, 0.0));
    }
    else
    {
        rot = hmul(rot, pre);
    }

    HQ rotation = hmul(twist, swing);
    if (invert)
        rotation = conjQ(rotation);
    rot = hmul(rot, rotation);

    // The .anim keys store x, y, z only; w comes back as +sqrt(1 - |xyz|^2).
    rot.w = sqrt(maxD(0.0, 1.0 - (rot.x * rot.x + rot.y * rot.y + rot.z * rot.z)));
    return rot;
}

// The generator's one discontinuity inside the dial ranges (#1459): a leg-model bone at an
// applied y of exactly 180 degrees (dial Y = +90: the elbow's end stop, a shoulder, hip or
// collar snapped to 90) takes the "y >= 180" pre-rotation without the sphere remap that goes
// with it past 180, and comes out turned 180 degrees about its own length -- the HUD's own
// animation for that one value. Both sides of the pole agree with each other, so the pole is
// moved a hair off it (POLE_NUDGE radians, about 0.00006 degrees); every other value is
// computed exactly as the generator did.
constexpr double POLE_NUDGE = 1.0e-6;

inline double offLegPole(const Model& m, double ay)
{
    return (m.leg && fabs(ay - PI_D) < POLE_NUDGE) ? PI_D - POLE_NUDGE : ay;
}

// The joint rotation for dial values in degrees (applied angle = invert * (dial - offset), as
// the generator's loop and -ix/-iy/-iz did), off the leg pole. Not normalized.
inline HQ dialToQuat(const Model& m, double dx, double dy, double dz, bool follower_invert = false)
{
    const double ax = degToRad(m.invert[0] * (dx - m.offset[0]));
    const double ay = offLegPole(m, degToRad(m.invert[1] * (dy - m.offset[1])));
    const double az = degToRad(m.invert[2] * (dz - m.offset[2]));
    return forwardApplied(m, ax, ay, az, follower_invert);
}

inline double angleBetweenDeg(const HQ& a, const HQ& b)
{
    const double d = fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    return 2.0 * acos((1.0 < d) ? 1.0 : d) * 180.0 / PI_D;
}
}

// The dial's mouse drag (SLPoseDialCtrl, #1459): relative to where it was pressed. The
// value runs over the upper half ring, min at the left end, max at the right, so a turn of
// the pointer by pi covers the whole range; clockwise is toward max. Angles are radians,
// counter-clockwise, as atan2 gives them.
namespace SLPoseDial
{
constexpr float PI_F = 3.14159265358979f;
constexpr float FINE = 0.1f;    // Shift: a tenth of the speed

// An angle step wrapped into [-pi, pi): a drag across the left end below the centre line
// (pi to -pi) is a small turn, not a full circle.
inline float wrapAngle(float d)
{
    while (d >= PI_F)
        d -= 2.f * PI_F;
    while (d < -PI_F)
        d += 2.f * PI_F;
    return d;
}

// The limits of a drag that starts at start: the dial's range, widened to the start value
// when that lies outside it (a pose decoded past the dial's end). The drag never snaps such
// a value to the end stop; it only stops it going further outward.
inline void dragLimits(float start, float min_value, float max_value, float& lo, float& hi)
{
    lo = (start < min_value) ? start : min_value;
    hi = (start > max_value) ? start : max_value;
}

// The value after the pointer turned from last_angle to angle, clamped to [lo, hi]. The turn
// is scaled by the dial's own range (min_value..max_value over the half ring).
inline float dragValue(float value, float last_angle, float angle, float min_value, float max_value, bool fine,
                       float lo, float hi)
{
    const float turn = wrapAngle(angle - last_angle);
    float v = value - turn * (max_value - min_value) / PI_F * (fine ? FINE : 1.f);
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}

inline float dragValue(float value, float last_angle, float angle, float min_value, float max_value, bool fine)
{
    return dragValue(value, last_angle, angle, min_value, max_value, fine, min_value, max_value);
}

// Where the pointer is drawn: a value past an end sits at that end (its number is still
// shown as it is).
inline float shownValue(float value, float min_value, float max_value)
{
    return (value < min_value) ? min_value : ((value > max_value) ? max_value : value);
}

// The pointer angle for a mouse point around the dial's centre, or false when the point is
// within min_radius of it (the angle is meaningless there; the drag waits).
inline bool pointerAngle(float x, float y, float cx, float cy, float min_radius, float& angle)
{
    const float dx = x - cx, dy = y - cy;
    if (dx * dx + dy * dy < min_radius * min_radius)
        return false;
    angle = atan2f(dy, dx);
    return true;
}
}

#endif // SL_POSEDIALMATH_H
