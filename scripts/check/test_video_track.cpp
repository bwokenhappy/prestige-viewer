// Tests for indra/newview/slvideotrack.h: Video Studio's track (#1668). Straight and curved rails
// (both directions, tilt), the bend handle's radius, the ride's easing (continuous, monotonic, the
// right cruise speed), repeat modes, the cart's aim (along / target / blend, no roll), the gizmos'
// drag math, and the recorder's clock (preroll, fixed step, dropped frames, the end of a ride).
// Built and run by scripts/check/checks/video_track.py.
#include "slvideotrack.h"

#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
#define NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

using namespace slvtrack;

static bool nearV(const V3& a, const V3& b, double eps = 1e-6) { return length(a - b) <= eps; }

static void straightRail()
{
    Track t;
    t.origin = V3(10, 20, 30);
    t.yaw = kPi / 2;   // +Y
    t.length = 6;
    const Frame f0 = at(t, 0.0), f1 = at(t, 1.0), fm = at(t, 0.5);
    CHECK(nearV(f0.pos, V3(10, 20, 30)));
    CHECK(nearV(f1.pos, V3(10, 26, 30)));
    CHECK(nearV(fm.pos, V3(10, 23, 30)));
    CHECK(nearV(f1.fwd, V3(0, 1, 0)));
    CHECK(nearV(f1.left, V3(-1, 0, 0)));
    CHECK(nearV(f1.up, V3(0, 0, 1)));
    // clamped outside 0..1
    CHECK(nearV(at(t, 2.0).pos, f1.pos));
    CHECK(nearV(at(t, -1.0).pos, f0.pos));
    // tilt: the rail rises along its direction
    t.tilt = kPi / 6;
    const Frame ft = at(t, 1.0);
    NEAR(ft.pos.z - 30, 6 * std::sin(kPi / 6), 1e-9);
    NEAR(length(ft.pos - t.origin), 6, 1e-9);
    NEAR(dot(ft.up, ft.fwd), 0, 1e-9);
}

static void curvedRail()
{
    Track t;
    t.length = kPi * 5;   // a half circle of radius 5
    t.radius = 5;         // curving left (+Y side when heading +X)
    Frame end = at(t, 1.0);
    CHECK(nearV(end.pos, V3(0, 10, 0), 1e-9));
    CHECK(nearV(end.fwd, V3(-1, 0, 0), 1e-9));
    NEAR(arcAngle(t), kPi, 1e-12);
    // every point is on the circle around the centre (0, 5, 0)
    for (int i = 0; i <= 10; ++i)
        NEAR(length(at(t, i / 10.0).pos - V3(0, 5, 0)), 5, 1e-9);
    // the arc's length matches: sum of small chords
    double sum = 0;
    for (int i = 0; i < 1000; ++i)
        sum += length(at(t, (i + 1) / 1000.0).pos - at(t, i / 1000.0).pos);
    NEAR(sum, t.length, 1e-3);
    // curving right mirrors it
    t.radius = -5;
    end = at(t, 1.0);
    CHECK(nearV(end.pos, V3(0, -10, 0), 1e-9));
    // the tangent is the derivative of the position
    t.radius = 3;
    t.length = 4;
    t.tilt = 0.3;
    t.yaw = 1.1;
    for (int i = 1; i < 10; ++i)
    {
        const double s = i / 10.0, h = 1e-6;
        const V3 d = normalize(at(t, s + h).pos - at(t, s - h).pos);
        CHECK(nearV(d, at(t, s).fwd, 1e-5));
        NEAR(dot(at(t, s).fwd, at(t, s).up), 0, 1e-9);   // the cart stays in the rail's plane
    }
    // setArcAngle keeps the radius
    setArcAngle(t, kPi / 2);
    NEAR(t.radius, 3, 1e-12);
    NEAR(t.length, 3 * kPi / 2, 1e-9);
    // sanitize: never a full circle; tiny radius = straight
    Track c;
    c.radius = 1;
    c.length = 100;
    sanitize(c);
    CHECK(c.length <= kMaxArc * 1 + 1e-9);
    c.radius = 0.1;
    sanitize(c);
    CHECK(!curved(c));
    Track bad;
    bad.length = NAN;
    bad.yaw = INFINITY;
    sanitize(bad);
    CHECK(std::isfinite(bad.length) && std::isfinite(bad.yaw));
}

static void bendHandle()
{
    // the radius that bows the midpoint by h, measured back, gives h (both sides)
    for (double h : { 0.05, 0.3, 1.0, 1.9, -0.7 })
    {
        Track t;
        t.length = 6;
        t.radius = radiusForBulge(t.length, h);
        CHECK(curved(t));
        CHECK((t.radius > 0) == (h > 0));
        sanitize(t);
        const Frame mid = at(t, 0.5);
        NEAR(dot(mid.pos - t.origin, basis(t).left), h, 1e-6);
    }
    CHECK(radiusForBulge(6, 0.001) == 0.0);   // straight
    // monotonic: more bulge, tighter radius
    double prev = 1e30;
    for (double h = 0.05; h < 1.8; h += 0.05)
    {
        const double r = radiusForBulge(6, h);
        CHECK(r <= prev + 1e-9);
        prev = r;
    }
    // past the largest bow: the tightest arc allowed, not a wrap-around
    const double r = radiusForBulge(6, 100);
    NEAR(6 / r, kMaxArc, 1e-6);
}

static void easing()
{
    for (double in : { 0.0, 0.1, 0.25, 0.5 })
        for (double out : { 0.0, 0.2, 0.5 })
        {
            NEAR(ease(0, in, out), 0, 1e-12);
            NEAR(ease(1, in, out), 1, 1e-12);
            double prev = -1, prevV = 0;
            for (int i = 0; i <= 1000; ++i)
            {
                const double u = i / 1000.0, p = ease(u, in, out);
                CHECK(p >= prev - 1e-12);   // monotonic
                if (i > 0)
                {
                    const double v = (p - prev) * 1000;   // speed
                    if (i > 1) CHECK(std::fabs(v - prevV) < 0.05);   // continuous speed (no jerk jumps)
                    prevV = v;
                }
                prev = p;
            }
        }
    // cruise speed: the middle of the ride moves at cruiseSpeed
    const double len = 10, T = 8, in = 0.25, out = 0.25;
    const double v = cruiseSpeed(len, T, in, out);
    const double mid = (ease(0.5 + 1e-4, in, out) - ease(0.5 - 1e-4, in, out)) / 2e-4 * len / T;
    NEAR(mid, v, 1e-6);
    NEAR(durationForSpeed(len, v, in, out), T, 1e-9);
    NEAR(ease(0.5, 0, 0), 0.5, 1e-12);   // linear
}

static void repeats()
{
    Ride r;
    r.duration = 4;
    r.easeIn = r.easeOut = 0;
    bool done = true;
    NEAR(position(r, 0, &done), 0, 1e-12);
    CHECK(!done);
    NEAR(position(r, 2, &done), 0.5, 1e-12);
    NEAR(position(r, 5, &done), 1, 1e-12);
    CHECK(done);
    r.reverse = true;
    NEAR(position(r, 1, &done), 0.75, 1e-12);
    r.reverse = false;
    r.repeat = Repeat::Loop;
    NEAR(position(r, 5, &done), 0.25, 1e-12);
    CHECK(!done);
    r.repeat = Repeat::PingPong;
    NEAR(position(r, 2, &done), 0.5, 1e-12);
    NEAR(position(r, 4, &done), 1, 1e-12);
    NEAR(position(r, 6, &done), 0.5, 1e-12);
    NEAR(position(r, 8, &done), 0, 1e-12);
    CHECK(!done);
    // ping-pong with easing slows at both ends of each leg
    r.easeIn = r.easeOut = 0.3;
    const double nearEnd = position(r, 4 - 0.01), atEnd = position(r, 4), past = position(r, 4 + 0.01);
    CHECK(nearEnd < atEnd && past < atEnd);
    CHECK(atEnd - nearEnd < 0.001 && atEnd - past < 0.001);
    Ride bad;
    bad.duration = -3;
    bad.repeat = (Repeat)7;
    sanitize(bad);
    CHECK(bad.duration == kMinDuration && bad.repeat == Repeat::Once);
}

static void aims()
{
    Track t;
    t.length = 4;
    Aim a;
    a.mode = AimMode::Target;
    a.target = V3(2, 5, 0);
    Frame f = at(t, 0.5);
    CHECK(nearV(aimDir(t, a, f, 0.5), V3(0, 1, 0)));
    // along: a quarter-turn left of the rail
    a.mode = AimMode::Along;
    a.pan = kPi / 2;
    CHECK(nearV(aimDir(t, a, f, 0.5), V3(0, 1, 0), 1e-9));
    a.pan = 0;
    a.tiltOffset = kPi / 4;
    CHECK(nearV(aimDir(t, a, f, 0.5), normalize(V3(1, 0, 1)), 1e-9));
    // blend: start and end aims reached exactly, in between on the great circle
    a.mode = AimMode::Blend;
    const Basis b = basis(t);
    yawPitchIn(b, V3(0, 1, 0), a.startYaw, a.startPitch);
    yawPitchIn(b, V3(1, 0, 0), a.endYaw, a.endPitch);
    CHECK(nearV(aimDir(t, a, at(t, 0), 0), V3(0, 1, 0), 1e-9));
    CHECK(nearV(aimDir(t, a, at(t, 1), 1), V3(1, 0, 0), 1e-9));
    CHECK(nearV(aimDir(t, a, at(t, 0.5), 0.5), normalize(V3(1, 1, 0)), 1e-9));
    // the aims follow the rail when it turns: rotate the rail by 90 degrees
    Track t2 = t;
    t2.yaw = kPi / 2;
    CHECK(nearV(aimDir(t2, a, at(t2, 0), 0), V3(-1, 0, 0), 1e-9));
    // yaw / pitch round trip
    double y = 0, p = 0;
    yawPitchIn(basis(t2), normalize(V3(0.3, -0.4, 0.5)), y, p);
    CHECK(nearV(dirIn(basis(t2).fwd, basis(t2).left, basis(t2).up, y, p), normalize(V3(0.3, -0.4, 0.5)), 1e-9));
    // slerp of opposite directions stays unit length and turns through `up`'s side
    const V3 half = slerpDir(V3(1, 0, 0), V3(-1, 0, 0), 0.5);
    NEAR(length(half), 1, 1e-9);
    NEAR(dot(half, V3(1, 0, 0)), 0, 1e-6);
    // camera axes: level (no roll), orthonormal
    for (const V3& d : { V3(1, 0, 0), V3(0.3, 0.2, -0.9), V3(0, 0, 1) })
    {
        const CamAxes c = cameraAxes(d);
        NEAR(c.left.z, 0, 1e-9);
        NEAR(dot(c.at, c.left), 0, 1e-9);
        NEAR(dot(c.at, c.up), 0, 1e-9);
        NEAR(length(c.up), 1, 1e-9);
        CHECK(nearV(cross(c.at, c.left), c.up, 1e-9));
    }
}

static void gizmoMath()
{
    // a ray looking down the -Z axis from above (5, 2, 10) meets the X axis line at t = 5
    double t = 0;
    CHECK(lineParamNearestRay(V3(0, 0, 0), V3(1, 0, 0), V3(5, 2, 10), V3(0, 0, -1), t));
    NEAR(t, 5, 1e-9);
    CHECK(!lineParamNearestRay(V3(0, 0, 0), V3(1, 0, 0), V3(0, 2, 0), V3(1, 0, 0), t));   // parallel
    V3 hit;
    CHECK(rayPlane(V3(1, 2, 10), V3(0, 0, -1), V3(0, 0, 3), V3(0, 0, 1), hit));
    CHECK(nearV(hit, V3(1, 2, 3)));
    CHECK(!rayPlane(V3(1, 2, 10), V3(0, 0, 1), V3(0, 0, 3), V3(0, 0, 1), hit));   // behind
    double along = 0;
    NEAR(rayPointDistance(V3(0, 0, 0), V3(1, 0, 0), V3(5, 3, 4), &along), 5, 1e-9);
    NEAR(along, 5, 1e-9);
    NEAR(raySegmentDistance(V3(0, 0, 10), V3(0, 0, -1), V3(-1, 1, 0), V3(1, 1, 0)), 1, 1e-6);
    NEAR(ringAngle(V3(0, 0, 0), V3(1, 0, 0), V3(0, 1, 0), V3(0, 2, 0)), kPi / 2, 1e-12);
    NEAR(snap(0.37, 0.25), 0.25, 1e-12);
    NEAR(snap(0.38, 0.25), 0.5, 1e-12);
    NEAR(snap(0.38, 0), 0.38, 1e-12);
}

static void recorderClock()
{
    // fixed step at 30 fps: the preroll holds the cart at the start until the first frame is taken
    RecClock c;
    c.start(true, 30);
    CHECK(c.state == RecState::Preroll);
    NEAR(c.rideTime(100.0), 0, 1e-12);
    NEAR(c.encoderClock(100.0), 100.0, 1e-12);   // real time while the encoder sets up
    c.submitted(true);                             // nothing captured: nothing counted
    CHECK(c.frames == 0 && c.state == RecState::Preroll);
    c.captured(100.5);
    c.submitted(true);
    CHECK(c.state == RecState::Recording && c.frames == 1);
    // the encoder's clock is on the 1/fps grid from the first frame, whatever the real time
    NEAR(c.encoderClock(250.0), 100.5 + 1.0 / 30, 1e-9);
    NEAR(c.rideTime(250.0), 1.0 / 30, 1e-12);
    // a frame captured but dropped is rendered again at the same ride time
    c.captured(101.0);
    c.submitted(false);
    CHECK(c.frames == 1);
    NEAR(c.rideTime(300.0), 1.0 / 30, 1e-12);
    // a frame the encoder skipped (no slot): no capture, no advance
    c.submitted(true);
    CHECK(c.frames == 1);
    // a 1 s Once ride: frames at 0, 1/30, ..., 30/30 -> 31 frames
    Ride r;
    r.duration = 1;
    int guard = 0;
    while (!rideRecorded(c, r, 0) && guard++ < 1000)
    {
        c.captured(0);
        c.submitted(true);
    }
    CHECK(c.frames == 31);
    NEAR(c.videoSeconds(0), 31.0 / 30, 1e-12);
    c.stop();
    CHECK(c.state == RecState::Finishing && !c.running());
    c.captured(0);
    c.submitted(true);
    CHECK(c.frames == 31);   // nothing counts once stopped
    c.finished();
    CHECK(c.state == RecState::Idle);

    // real time: the ride follows the real clock from the first frame
    RecClock rt;
    rt.start(false, 60);
    rt.captured(10.0);
    rt.submitted(true);
    NEAR(rt.rideTime(12.5), 2.5, 1e-12);
    NEAR(rt.encoderClock(12.5), 12.5, 1e-12);
    Ride r2;
    r2.duration = 3;
    CHECK(!rideRecorded(rt, r2, 12.9));
    CHECK(rideRecorded(rt, r2, 13.0));
    r2.repeat = Repeat::Loop;
    CHECK(!rideRecorded(rt, r2, 1000.0));   // a loop never ends by itself
}

int main()
{
    straightRail();
    curvedRail();
    bendHandle();
    easing();
    repeats();
    aims();
    gizmoMath();
    recorderClock();
    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("video track: all tests pass\n");
    return 0;
}
