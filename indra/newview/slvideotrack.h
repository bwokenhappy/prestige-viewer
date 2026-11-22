/**
 * @file slvideotrack.h
 * @brief Video Studio (#1668): the camera track's math, the ride's timing, the cart's aim, the
 *        gizmos' drag math and the recorder's clock. Standard C++ only (no viewer types), so
 *        scripts/check/test_video_track.cpp tests exactly what the viewer runs.
 *
 * A TRACK is a dolly / slider rail: a start point, a heading (yaw about +Z), a tilt (the rail rises
 * along its direction), a length along the rail, and a signed radius (0 = straight slider; > 0
 * curves to the left, < 0 to the right). Positions are in any right-handed Z-up frame (the viewer
 * uses global metres for storage and agent metres for drawing); nothing here knows which.
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

#ifndef SL_VIDEOTRACK_H
#define SL_VIDEOTRACK_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace slvtrack
{
constexpr double kPi = 3.14159265358979323846;

// ---- vectors ----
struct V3
{
    double x = 0.0, y = 0.0, z = 0.0;
    V3() = default;
    V3(double ax, double ay, double az) : x(ax), y(ay), z(az) {}
};
inline V3 operator+(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(const V3& a, double s) { return { a.x * s, a.y * s, a.z * s }; }
inline V3 operator*(double s, const V3& a) { return a * s; }
inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline double length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(const V3& a, const V3& fallback = V3(1.0, 0.0, 0.0))
{
    const double l = length(a);
    return l > 1e-12 ? a * (1.0 / l) : fallback;
}

// ---- the track ----
constexpr double kMinLength = 0.25, kMaxLength = 200.0;   // metres along the rail
constexpr double kMinRadius = 0.5, kMaxRadius = 1000.0;   // |radius| of a curved rail
constexpr double kMaxTilt = 60.0 * kPi / 180.0;           // the rail's rise
constexpr double kMaxArc = 2.0 * kPi * 0.95;              // just short of a full circle

struct Track
{
    V3 origin;            // the start of the rail
    double yaw = 0.0;     // heading of the rail at its start, radians about +Z (0 = +X)
    double tilt = 0.0;    // the rail's rise, radians (+: up along the rail)
    double length = 4.0;  // metres along the rail
    double radius = 0.0;  // 0: straight; signed: > 0 curves left, < 0 right
};

// Clamps every value into its range (a straight rail for a radius under kMinRadius; an arc never
// closes a full circle: the length is clamped to kMaxArc * |radius|).
inline void sanitize(Track& t)
{
    if (!std::isfinite(t.yaw)) t.yaw = 0.0;
    if (!std::isfinite(t.tilt)) t.tilt = 0.0;
    if (!std::isfinite(t.length)) t.length = 4.0;
    if (!std::isfinite(t.radius)) t.radius = 0.0;
    t.yaw = std::remainder(t.yaw, 2.0 * kPi);
    t.tilt = std::clamp(t.tilt, -kMaxTilt, kMaxTilt);
    t.length = std::clamp(t.length, kMinLength, kMaxLength);
    if (std::fabs(t.radius) < kMinRadius) t.radius = 0.0;
    t.radius = std::clamp(t.radius, -kMaxRadius, kMaxRadius);
    if (t.radius != 0.0) t.length = std::min(t.length, kMaxArc * std::fabs(t.radius));
}

inline bool curved(const Track& t) { return t.radius != 0.0; }

// The rail's frame at its start: forward (along the rail), left, up (the rail's plane normal).
struct Basis
{
    V3 fwd, left, up;
};
inline Basis basis(const Track& t)
{
    const double cy = std::cos(t.yaw), sy = std::sin(t.yaw), ct = std::cos(t.tilt), st = std::sin(t.tilt);
    Basis b;
    b.fwd = { cy * ct, sy * ct, st };
    b.left = { -sy, cy, 0.0 };
    b.up = cross(b.fwd, b.left);
    return b;
}

// The arc a curved rail sweeps (radians, signed like the radius); 0 when straight.
inline double arcAngle(const Track& t) { return curved(t) ? t.length / t.radius : 0.0; }
// Sets the swept arc keeping the radius (the length changes). A straight rail stays straight.
inline void setArcAngle(Track& t, double angle)
{
    if (!curved(t)) return;
    t.length = std::fabs(angle * t.radius);
    sanitize(t);
}

struct Frame
{
    V3 pos, fwd, left, up;   // the cart on the rail: its place and the rail's direction there
};
// The cart `s` of the way along the rail (0 = start, 1 = end; clamped).
inline Frame at(const Track& t, double s)
{
    s = std::clamp(s, 0.0, 1.0);
    const Basis b = basis(t);
    const double d = s * t.length;
    Frame f;
    f.up = b.up;
    if (!curved(t))
    {
        f.pos = t.origin + b.fwd * d;
        f.fwd = b.fwd;
        f.left = b.left;
        return f;
    }
    const double r = t.radius, phi = d / r;
    // the centre is r along `left` (r < 0: to the right); the cart turns about `up`
    f.pos = t.origin + b.fwd * (r * std::sin(phi)) + b.left * (r * (1.0 - std::cos(phi)));
    f.fwd = b.fwd * std::cos(phi) + b.left * std::sin(phi);
    f.left = cross(b.up, f.fwd);
    return f;
}

// How far the middle of a rail of this length bows out from its straight line when it sweeps
// `angle` (radians, >= 0): the sagitta of the arc at its midpoint, measured along `left`.
inline double bulgeForArc(double len, double angle)
{
    if (angle <= 1e-9) return 0.0;
    const double r = len / angle;
    // the midpoint sits r(1 - cos(angle / 2)) to the side and the straight line's midpoint is
    // at len / 2 along fwd; the rail's own midpoint is r sin(angle / 2) along fwd. Sideways only:
    return r * (1.0 - std::cos(angle * 0.5));
}
// The SHAPE gizmo's bend handle: the signed radius that bows the rail's midpoint `bulge` metres to
// the side (> 0 left), keeping its length. 0 (straight) for a bulge under 1 cm. Monotonic in the
// bulge (bisection on the swept angle, which is capped at kMaxArc).
inline double radiusForBulge(double len, double bulge)
{
    len = std::clamp(len, kMinLength, kMaxLength);
    const double h = std::fabs(bulge);
    if (h < 0.01) return 0.0;
    double lo = 0.0, hi = kMaxArc;
    if (h >= bulgeForArc(len, hi)) lo = hi;
    else
        for (int i = 0; i < 60; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            (bulgeForArc(len, mid) < h ? lo : hi) = mid;
        }
    const double angle = std::max(lo, 1e-6);
    double r = len / angle;
    r = std::clamp(r, kMinRadius, kMaxRadius);
    return bulge < 0.0 ? -r : r;
}

// ---- the ride's timing ----
// A trapezoidal speed profile: the cart speeds up over the first `in` of the time, cruises, and
// slows down over the last `out` (fractions 0..0.5 each). Maps time 0..1 to the way along 0..1,
// continuous in position and speed, monotonic; in = out = 0 is linear.
inline double ease(double u, double in, double out)
{
    u = std::clamp(u, 0.0, 1.0);
    in = std::clamp(in, 0.0, 0.5);
    out = std::clamp(out, 0.0, 0.5);
    const double vmax = 1.0 / (1.0 - 0.5 * (in + out));
    if (in > 0.0 && u < in) return vmax * u * u / (2.0 * in);
    if (out > 0.0 && u > 1.0 - out)
    {
        const double w = 1.0 - u;
        return 1.0 - vmax * w * w / (2.0 * out);
    }
    return std::clamp(vmax * (0.5 * in + (u - in)), 0.0, 1.0);
}
// The cruise speed (m/s) of a ride of `seconds` over `len` metres with these easings, and its inverse.
inline double cruiseSpeed(double len, double seconds, double in, double out)
{
    const double vmax = 1.0 / (1.0 - 0.5 * (std::clamp(in, 0.0, 0.5) + std::clamp(out, 0.0, 0.5)));
    return seconds > 0.0 ? vmax * len / seconds : 0.0;
}
inline double durationForSpeed(double len, double speed, double in, double out)
{
    const double vmax = 1.0 / (1.0 - 0.5 * (std::clamp(in, 0.0, 0.5) + std::clamp(out, 0.0, 0.5)));
    return speed > 0.0 ? vmax * len / speed : 0.0;
}

enum class Repeat : int
{
    Once = 0,
    Loop = 1,
    PingPong = 2
};
constexpr double kMinDuration = 0.5, kMaxDuration = 600.0;
struct Ride
{
    double duration = 8.0;            // seconds for one pass
    double easeIn = 0.2, easeOut = 0.2;
    Repeat repeat = Repeat::Once;
    bool reverse = false;             // ride from the end to the start
};
inline void sanitize(Ride& r)
{
    if (!std::isfinite(r.duration)) r.duration = 8.0;
    if (!std::isfinite(r.easeIn)) r.easeIn = 0.0;
    if (!std::isfinite(r.easeOut)) r.easeOut = 0.0;
    r.duration = std::clamp(r.duration, kMinDuration, kMaxDuration);
    r.easeIn = std::clamp(r.easeIn, 0.0, 0.5);
    r.easeOut = std::clamp(r.easeOut, 0.0, 0.5);
    if ((int)r.repeat < 0 || (int)r.repeat > 2) r.repeat = Repeat::Once;
}
// Where the cart is (0..1 along the rail) `elapsed` seconds into the ride, eased per pass. A Once
// ride holds at its end and says so (`finished`); Loop and PingPong never finish.
inline double position(const Ride& r, double elapsed, bool* finished = nullptr)
{
    const double T = std::clamp(r.duration, kMinDuration, kMaxDuration);
    elapsed = std::max(0.0, elapsed);
    double u = 0.0;
    bool done = false;
    switch (r.repeat)
    {
    case Repeat::Once:
        u = std::min(elapsed / T, 1.0);
        done = elapsed >= T;
        break;
    case Repeat::Loop:
        u = std::fmod(elapsed, T) / T;
        break;
    case Repeat::PingPong:
    {
        const double x = std::fmod(elapsed, 2.0 * T);
        u = x < T ? x / T : 2.0 - x / T;
        // each leg eases on its own: ease the leg's time, then map back to the rail
        const bool back = x >= T;
        const double leg = back ? (x - T) / T : x / T;
        const double s = ease(leg, r.easeIn, r.easeOut);
        if (finished) *finished = false;
        const double p = back ? 1.0 - s : s;
        return r.reverse ? 1.0 - p : p;
    }
    }
    if (finished) *finished = done;
    const double s = ease(u, r.easeIn, r.easeOut);
    return r.reverse ? 1.0 - s : s;
}

// ---- the cart's aim ----
enum class AimMode : int
{
    Along = 0,    // a fixed turn and tilt relative to the rail's direction at the cart
    Target = 1,   // looks at a target point (or a picked object, resolved to a point each frame)
    Blend = 2     // from the start aim to the end aim, captured from the camera ("aim here")
};
struct Aim
{
    AimMode mode = AimMode::Target;
    double pan = 0.0, tiltOffset = 0.0;   // Along: radians, + = left / up
    V3 target;                            // Target: the point (the caller resolves a picked object)
    // Blend: yaw / pitch relative to the rail's start frame, so the aims move with the rail
    double startYaw = 0.0, startPitch = 0.0, endYaw = 0.0, endPitch = 0.0;
};

// A direction from yaw / pitch in a frame (yaw about `up` from `fwd` toward `left`).
inline V3 dirIn(const V3& fwd, const V3& left, const V3& up, double yaw, double pitch)
{
    return normalize((fwd * std::cos(yaw) + left * std::sin(yaw)) * std::cos(pitch) + up * std::sin(pitch));
}
// Yaw / pitch of a direction in the rail's start frame ("aim here").
inline void yawPitchIn(const Basis& b, const V3& dir, double& yaw, double& pitch)
{
    const V3 d = normalize(dir);
    yaw = std::atan2(dot(d, b.left), dot(d, b.fwd));
    pitch = std::asin(std::clamp(dot(d, b.up), -1.0, 1.0));
}
// Spherical interpolation of two unit directions (shortest way; a half-turn picks `up`'s side).
inline V3 slerpDir(const V3& a0, const V3& b0, double t, const V3& up = V3(0.0, 0.0, 1.0))
{
    const V3 a = normalize(a0), b = normalize(b0);
    const double c = std::clamp(dot(a, b), -1.0, 1.0);
    if (c > 0.9995) return normalize(a * (1.0 - t) + b * t);
    double theta = std::acos(c);
    V3 axisPerp;   // the unit vector in the a-b plane perpendicular to a
    if (c < -0.9995)
    {
        axisPerp = normalize(cross(cross(a, up), a), normalize(cross(a, V3(0.0, 1.0, 0.0))));
        theta = kPi;
    }
    else
        axisPerp = normalize(b - a * c);
    return normalize(a * std::cos(theta * t) + axisPerp * std::sin(theta * t));
}
// Where the cart's camera looks (a unit vector) at frame `f`, `s` of the way along the track.
inline V3 aimDir(const Track& t, const Aim& a, const Frame& f, double s)
{
    switch (a.mode)
    {
    case AimMode::Along:
        return dirIn(f.fwd, f.left, f.up, a.pan, a.tiltOffset);
    case AimMode::Target:
        return normalize(a.target - f.pos, f.fwd);
    case AimMode::Blend:
    {
        const Basis b = basis(t);
        const V3 d0 = dirIn(b.fwd, b.left, b.up, a.startYaw, a.startPitch);
        const V3 d1 = dirIn(b.fwd, b.left, b.up, a.endYaw, a.endPitch);
        return slerpDir(d0, d1, std::clamp(s, 0.0, 1.0), b.up);
    }
    }
    return f.fwd;
}
// The camera's axes for an aim: at (forward), left and up, level with the world (no roll).
struct CamAxes
{
    V3 at, left, up;
};
inline CamAxes cameraAxes(const V3& aim)
{
    CamAxes c;
    c.at = normalize(aim);
    const V3 zUp(0.0, 0.0, 1.0);
    c.left = normalize(cross(zUp, c.at), V3(0.0, 1.0, 0.0));   // straight up / down: any level left
    c.up = cross(c.at, c.left);
    return c;
}

// ---- the gizmos' drag math ----
// The point on the line (p + u t) nearest the ray (o + d s, s > 0): its t. False when the ray is
// (nearly) parallel to the line.
inline bool lineParamNearestRay(const V3& p, const V3& u0, const V3& o, const V3& d0, double& t)
{
    const V3 u = normalize(u0), d = normalize(d0), w = p - o;
    const double b = dot(u, d), denom = 1.0 - b * b;
    if (denom < 1e-6) return false;
    t = (b * dot(d, w) - dot(u, w)) / denom;
    return std::isfinite(t);
}
// The ray (o + d s) meets the plane through `p` with normal `n` at `hit` (s > 0).
inline bool rayPlane(const V3& o, const V3& d0, const V3& p, const V3& n0, V3& hit)
{
    const V3 d = normalize(d0), n = normalize(n0);
    const double denom = dot(d, n);
    if (std::fabs(denom) < 1e-6) return false;
    const double s = dot(p - o, n) / denom;
    if (s <= 0.0) return false;
    hit = o + d * s;
    return true;
}
// The distance between the ray (o + d s, s >= 0) and the point q.
inline double rayPointDistance(const V3& o, const V3& d0, const V3& q, double* along = nullptr)
{
    const V3 d = normalize(d0);
    const double s = std::max(0.0, dot(q - o, d));
    if (along) *along = s;
    return length(o + d * s - q);
}
// The distance between the ray and the segment a-b (closest approach; the ray's depth there).
inline double raySegmentDistance(const V3& o, const V3& d0, const V3& a, const V3& b, double* along = nullptr)
{
    // sampled closest approach: robust for the short gizmo parts (exact enough for picking)
    double best = 1e30, bestAlong = 0.0;
    for (int i = 0; i <= 32; ++i)
    {
        double s = 0.0;
        const double dist = rayPointDistance(o, d0, a + (b - a) * (i / 32.0), &s);
        if (dist < best)
        {
            best = dist;
            bestAlong = s;
        }
    }
    if (along) *along = bestAlong;
    return best;
}
// The angle (radians) of `hit` around `centre` in the plane spanned by e1 (0) and e2 (+pi/2).
inline double ringAngle(const V3& centre, const V3& e1, const V3& e2, const V3& hit)
{
    const V3 v = hit - centre;
    return std::atan2(dot(v, e2), dot(v, e1));
}
// Snapping: the nearest multiple of `step` (step <= 0: unchanged).
inline double snap(double v, double step) { return step > 0.0 ? std::round(v / step) * step : v; }

// ---- the recorder's clock ----
// One recording of a ride. The ride's clock starts at the first frame the encoder takes (before it
// the cart waits at the start: the PREROLL, while the encoder sets itself up), so the video starts at
// the ride's start. FIXED STEP: the ride advances exactly 1 / fps per frame that reached the video,
// whatever the real frame time (the video is smooth; the session may run slower than real time);
// the encoder sees a clock on the same grid. REAL TIME: the ride and the encoder follow the real clock.
// A frame counts only once its submission is known (submitted(true)): a dropped frame is
// re-rendered at the same ride time.
enum class RecState : int
{
    Idle = 0,
    Preroll = 1,     // waiting for the encoder's first frame
    Recording = 2,
    Finishing = 3    // stopped; the file is being written
};
struct RecClock
{
    RecState state = RecState::Idle;
    bool fixedStep = true;
    double fps = 30.0;
    double t0 = 0.0;          // real time of the first frame
    int64_t frames = 0;       // frames that reached the video
    bool pending = false;     // a frame was captured this frame, its submission not yet known
    double pendingAt = 0.0;   // the real time it was captured at

    void start(bool fixed, double framesPerSecond)
    {
        state = RecState::Preroll;
        fixedStep = fixed;
        fps = std::clamp(framesPerSecond, 1.0, 240.0);
        t0 = 0.0;
        frames = 0;
        pending = false;
    }
    bool running() const { return state == RecState::Preroll || state == RecState::Recording; }
    // The ride's time for the frame being rendered now.
    double rideTime(double now) const
    {
        if (state != RecState::Recording) return 0.0;
        return fixedStep ? (double)frames / fps : std::max(0.0, now - t0);
    }
    // The clock the encoder is given for this frame (its rate gate and timestamps).
    double encoderClock(double now) const
    {
        if (state == RecState::Recording && fixedStep) return t0 + (double)frames / fps;
        return now;
    }
    // The encoder took this frame (at encoderClock(now)).
    void captured(double now)
    {
        if (!running()) return;
        pending = true;
        pendingAt = now;
    }
    // The frame's submission: ran = it reached the GPU (else its capture is lost).
    void submitted(bool ran)
    {
        if (!pending) return;
        pending = false;
        if (!ran || !running()) return;
        if (state == RecState::Preroll)
        {
            state = RecState::Recording;
            t0 = pendingAt;
        }
        ++frames;
    }
    // Seconds of video so far.
    double videoSeconds(double now) const
    {
        if (state != RecState::Recording && state != RecState::Finishing) return 0.0;
        return fixedStep ? (double)frames / fps : std::max(0.0, now - t0);
    }
    void stop() { if (state != RecState::Idle) state = RecState::Finishing; pending = false; }
    void finished() { state = RecState::Idle; pending = false; }
};
// A Once ride is fully recorded once the frame at its end reached the video: frames covering
// 0 .. duration inclusive (fixed step), or the real clock past the duration.
inline bool rideRecorded(const RecClock& c, const Ride& r, double now)
{
    if (c.state != RecState::Recording || r.repeat != Repeat::Once) return false;
    if (c.fixedStep) return (double)(c.frames - 1) / c.fps >= r.duration - 1e-9;
    return now - c.t0 >= r.duration;
}
} // namespace slvtrack

#endif // SL_VIDEOTRACK_H
