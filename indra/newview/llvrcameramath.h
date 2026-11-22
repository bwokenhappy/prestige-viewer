/**
 * @file llvrcameramath.h
 * @brief #1607 the VR camera's locally owned anchor (pure math, no viewer or OpenXR types), shared
 *        by LLVRCamera and the fixtures in scripts/check/test_vr_camera_anchor.cpp
 *        (checks/vr_camera_anchor.py).
 *
 * The horizontal room -> agent mapping is
 *
 *     agent = origin + rot(room - ref, turn) * k
 *
 * origin: the avatar's collision shape centre (standing), the seat's eye anchor (sitting) or the
 * fly cam's point; ref: the room point that maps onto it; turn: room -> agent about up; k: world
 * scale (camera metres per real metre). ref and turn are owned by the camera, never read back
 * from the avatar's rotation: turning the body never moves the view.
 *
 *  - turnAboutHead: a snap / smooth turn rotates the view about your head (the head maps to the
 *    same agent point before and after), not about the reference.
 *  - absorb: the origin moved but the view must not (the avatar walking to catch up with your
 *    room-scale position, a server correction held until the blink): the reference moves with it.
 *  - headOffset: where your head is relative to the origin (agent space): the room-scale distance
 *    the avatar still has to catch up.
 *  - drawOffset: where the local avatar's root is drawn so its eyes stand under the headset.
 *  - Follow: the room-scale catch-up controller (start / stop hysteresis, the catch-up's tail).
 *
 * Axes: SL agent space, X = at, Y = left, Z = up; yaw is about Z (+ = left), radians.
 */
#pragma once

#include <cmath>

namespace slvrcam
{
struct V2
{
    float x = 0.f, y = 0.f;
    V2() = default;
    V2(float ax, float ay) : x(ax), y(ay) {}
    V2 operator+(const V2& o) const { return V2(x + o.x, y + o.y); }
    V2 operator-(const V2& o) const { return V2(x - o.x, y - o.y); }
    V2 operator*(float s) const { return V2(x * s, y * s); }
};
inline float length(const V2& v) { return std::sqrt(v.x * v.x + v.y * v.y); }
inline bool finite(const V2& v) { return std::isfinite(v.x) && std::isfinite(v.y); }

constexpr float kPi = 3.14159265358979f;

// v rotated by a about up (+ = left).
inline V2 rot(const V2& v, float a)
{
    const float c = std::cos(a), s = std::sin(a);
    return V2(v.x * c - v.y * s, v.x * s + v.y * c);
}
// a wrapped into (-pi, pi].
inline float wrapAngle(float a)
{
    if (!std::isfinite(a)) return 0.f;
    a = std::fmod(a, 2.f * kPi);
    if (a > kPi) a -= 2.f * kPi;
    if (a <= -kPi) a += 2.f * kPi;
    return a;
}

struct Mapping
{
    V2 origin;          // agent space
    V2 ref;             // room space (the point that maps onto origin)
    float turn = 0.f;   // room -> agent, about up
    float k = 1.f;      // world scale
};

inline V2 toAgent(const Mapping& m, const V2& room) { return m.origin + rot(room - m.ref, m.turn) * m.k; }

// Where the head (room point `head`) is from the origin, in agent space.
inline V2 headOffset(const Mapping& m, const V2& head) { return rot(head - m.ref, m.turn) * m.k; }

// Turn the view by d (+ = left) about the head: toAgent(head) is unchanged, everything else turns
// around it. Independent of k and of the origin.
inline void turnAboutHead(Mapping& m, const V2& head, float d)
{
    m.ref = head - rot(head - m.ref, -d);
    m.turn = wrapAngle(m.turn + d);
}

// The origin moved by dP (agent space): the reference moves so that every room point maps where it
// did before (the view does not move). Apply after m.origin += dP.
inline void absorb(Mapping& m, const V2& dP)
{
    const float k = m.k > 1e-4f ? m.k : 1e-4f;
    m.ref = m.ref + rot(dP, -m.turn) * (1.f / k);
}

// Re-anchor: the head onto the origin (headOffset 0). The view jumps: under a blink, a teleport's
// fade or a recentre.
inline void reanchor(Mapping& m, const V2& head) { m.ref = head; }

// The avatar's root, drawn offset from the collision shape so that its eyes (eyeLocal: the eyes
// from the root in the root's yaw frame) land on the head: root + rot(eyeLocal, rootYaw) =
// origin + headOff. A body turn pivots the body about the head.
inline V2 drawOffset(const V2& headOff, const V2& eyeLocal, float rootYaw) { return headOff - rot(eyeLocal, rootYaw); }

// ---- Room-scale catch-up: the avatar walks to stand under your head (sent as normal movement).
struct FollowConfig
{
    float start = 0.35f;    // the head further than this from the collision shape: walk
    float stop = 0.10f;     // ... until it is within this
    float nudgeBelow = 0.6f; // the slow (nudge) controls under this distance, walking above
    float tail = 0.35f;     // seconds the catch-up's motion is still absorbed after the controls stop
};
struct FollowState
{
    bool active = false;
    float tail = 0.f;
};
struct FollowCommand
{
    int at = 0;      // +1 forward, -1 back, 0 none (in the body's facing)
    int left = 0;    // +1 left, -1 right
    bool nudge = false;
    bool absorb = false;   // the origin's motion this frame is the catch-up's: hold the view
};
// headOff: headOffset (agent space); bodyYaw: the agent's facing; allowed: standing, no stick
// walking, no autopilot / blink / fly cam / sit. A diagonal gives both controls.
inline FollowCommand stepFollow(FollowState& s, const V2& headOff, float bodyYaw, bool allowed, float dt,
                                const FollowConfig& c = FollowConfig())
{
    FollowCommand cmd;
    const float d = length(headOff);
    const bool was = s.active;
    if (!allowed || !finite(headOff)) s.active = false;
    else if (!s.active && d > c.start) s.active = true;
    else if (s.active && d < c.stop) s.active = false;
    if (s.active)
    {
        const V2 local = rot(headOff, -bodyYaw);
        const float comp = std::fmax(0.04f, 0.38f * d);   // ~8 directions
        cmd.at = local.x > comp ? 1 : (local.x < -comp ? -1 : 0);
        cmd.left = local.y > comp ? 1 : (local.y < -comp ? -1 : 0);
        cmd.nudge = d < c.nudgeBelow;
        s.tail = c.tail;
    }
    else if (was && allowed)
    {
        s.tail = c.tail;   // (just stopped: the sim still decelerates the avatar)
    }
    else
    {
        s.tail = allowed ? std::fmax(0.f, s.tail - (dt > 0.f ? dt : 0.f)) : 0.f;
    }
    cmd.absorb = s.active || s.tail > 0.f;
    return cmd;
}

// The origin's motion this frame (dP, agent metres; the expected motion speed * dt) is a jump --
// a server correction, a push, a region placement -- not locomotion: more than `jump` metres past
// what its velocity explains.
inline bool isJump(float moved, float speed, float dt, float jump = 0.5f)
{
    const float expect = (speed > 0.f ? speed : 0.f) * (dt > 0.f ? dt : 0.f);
    return moved > jump + 2.f * expect;
}
} // namespace slvrcam
