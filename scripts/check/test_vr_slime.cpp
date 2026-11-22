// Fixtures for #1582 SlimeVR body trackers over VRChat OSC (indra/newview/llvrbodyslimeosc.h), run by
// scripts/check/checks/vr_slime.py (host C++ compiler, no viewer build).
//   OSC      a SlimeVR-shaped bundle parses to the values sent; a single message too; every
//            truncation, bad padding, oversized element, too-deep nesting, non-finite or absurd
//            number is rejected whole (the output untouched); other addresses / argument types are
//            ignored, not failures; random corruption never crashes or reads out of bounds.
//   CONVERT  positions / rotations encoded exactly as SlimeVR's VRCOSCHandler does (z flip, then
//            Euler YXZ in degrees) decode back to the right-handed pose; a Unity yaw of -90 deg is a
//            turn to the left, Unity +z is forward.
//   ALIGN    from an unknown SlimeVR heading / origin: the feet land under the head at ankle height,
//            left stays left; walking and turning carries the feet along; a head sway SlimeVR sees as
//            the body moving under a fixed head leaves planted feet where they are; the hip-heading
//            fallback and the reset re-alignment give the same yaw.
#include "llvrbodyslimeosc.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace slslime;
using slfbt::Q;
using slfbt::V3;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static bool nearV(const V3& a, const V3& b, float eps) { return slfbt::length(a - b) <= eps; }
static float angleDiff(float a, float b) { return std::fabs(wrapAngle(a - b)); }

// ---- An OSC encoder (the sender's side) -------------------------------------------------------
using Bytes = std::vector<uint8_t>;
static void putStr(Bytes& b, const std::string& s)
{
    for (char c : s) b.push_back((uint8_t)c);
    b.push_back(0);
    while (b.size() & 3) b.push_back(0);
}
static void put32(Bytes& b, uint32_t u)
{
    b.push_back(uint8_t(u >> 24));
    b.push_back(uint8_t(u >> 16));
    b.push_back(uint8_t(u >> 8));
    b.push_back(uint8_t(u));
}
static void putF(Bytes& b, float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    put32(b, u);
}
static Bytes msg3(const std::string& addr, float a, float b, float c)
{
    Bytes m;
    putStr(m, addr);
    putStr(m, ",fff");
    putF(m, a);
    putF(m, b);
    putF(m, c);
    return m;
}
static Bytes bundle(const std::vector<Bytes>& elems)
{
    Bytes b;
    putStr(b, "#bundle");
    put32(b, 0);
    put32(b, 1);   // time tag: immediately
    for (const Bytes& e : elems)
    {
        put32(b, (uint32_t)e.size());
        b.insert(b.end(), e.begin(), e.end());
    }
    return b;
}

// ---- SlimeVR's sender, ported (VRCOSCHandler.update + EulerAngles YXZ) --------------------------
// The YXZ Euler angles of q = qY * qX * qZ (radians), from its rotation matrix.
static void toEulerYXZ(const Q& q, float& x, float& y, float& z)
{
    const float m02 = 2.f * (q.x * q.z + q.w * q.y);
    const float m12 = 2.f * (q.y * q.z - q.w * q.x);
    const float m22 = 1.f - 2.f * (q.x * q.x + q.y * q.y);
    const float m10 = 2.f * (q.x * q.y + q.w * q.z);
    const float m11 = 1.f - 2.f * (q.x * q.x + q.z * q.z);
    float s = -m12;
    s = s > 1.f ? 1.f : (s < -1.f ? -1.f : s);
    x = std::asin(s);
    y = std::atan2(m02, m22);
    z = std::atan2(m10, m11);
}
// A right-handed (SlimeVR / OpenXR axes) pose as SlimeVR sends it.
static void slimeEncode(const V3& p, const Q& q, float pos[3], float eulerDeg[3])
{
    pos[0] = p.x;
    pos[1] = p.y;
    pos[2] = -p.z;
    const Q f(-q.x, -q.y, q.z, q.w);   // Quaternion(w, -x1, -y1, z1)
    float x, y, z;
    toEulerYXZ(f, x, y, z);
    const float r2d = 180.f / 3.14159265358979f;
    eulerDeg[0] = x * r2d;
    eulerDeg[1] = y * r2d;
    eulerDeg[2] = z * r2d;
}
static void trackerMsgs(int n, const V3& p, const Q& q, std::vector<Bytes>& out)
{
    float pos[3], e[3];
    slimeEncode(p, q, pos, e);
    const std::string base = "/tracking/trackers/" + (n == 0 ? std::string("head") : std::to_string(n));
    out.push_back(msg3(base + "/position", pos[0], pos[1], pos[2]));
    if (n) out.push_back(msg3(base + "/rotation", e[0], e[1], e[2]));
    // (head: position only; its rotation comes on a reset)
}

// ---- OSC --------------------------------------------------------------------------------------
static void testOsc()
{
    std::vector<Bytes> el;
    for (int n = 1; n <= 8; ++n) trackerMsgs(n, V3(0.1f * n, 1.f - 0.1f * n, -0.05f * n), slfbt::qaxis(V3(0.f, 1.f, 0.f), 0.1f * n), el);
    trackerMsgs(0, V3(0.f, 1.7f, 0.f), Q(), el);
    el.push_back(msg3("/avatar/parameters/Foo", 1.f, 2.f, 3.f));   // someone else's
    const Bytes full = bundle(el);

    Packet p;
    CHECK(parsePacket(full.data(), full.size(), p), "valid bundle");
    CHECK(p.messages == 17 && p.ignored == 1, "counts %u / %u", p.messages, p.ignored);
    for (int n = 1; n <= 8; ++n)
    {
        CHECK(p.t[n].hasPos && p.t[n].hasRot, "slot %d present", n);
        CHECK(nearV(unityPosToXr(p.t[n].pos), V3(0.1f * n, 1.f - 0.1f * n, -0.05f * n), 1e-5f), "slot %d pos", n);
        CHECK(slfbt::qangle(unityEulerToXr(p.t[n].euler), slfbt::qaxis(V3(0.f, 1.f, 0.f), 0.1f * n)) < 1e-3f, "slot %d rot", n);
    }
    CHECK(p.t[T_HEAD].hasPos && !p.t[T_HEAD].hasRot, "head position only");

    // A lone message (no bundle).
    const Bytes one = msg3("/tracking/trackers/3/position", 1.f, 2.f, 3.f);
    Packet q1;
    CHECK(parsePacket(one.data(), one.size(), q1) && q1.t[T_RFOOT].hasPos && q1.t[T_RFOOT].pos[2] == 3.f, "lone message");

    // Every truncation of the bundle is rejected and leaves the output as it was -- except a cut
    // exactly between two elements, which is a well-formed shorter bundle (OSC bundles carry no
    // total length; UDP never delivers part of a datagram anyway): it gives whole messages only.
    std::vector<size_t> boundaries;
    {
        size_t off = 16;
        boundaries.push_back(off);
        for (const Bytes& e : el)
        {
            off += 4 + e.size();
            boundaries.push_back(off);
        }
    }
    for (size_t n = 0; n < full.size(); ++n)
    {
        Packet t;
        t.messages = 12345;
        const bool ok = parsePacket(full.data(), n, t);
        bool boundary = false;
        for (size_t k = 0; k < boundaries.size(); ++k)
            if (boundaries[k] == n)
            {
                boundary = true;
                CHECK(ok && t.messages + t.ignored == k, "cut after %zu elements: %u messages", k, t.messages);
            }
        if (!boundary) CHECK(!ok && t.messages == 12345, "truncated at %zu: accepted or touched the output", n);
    }

    auto rejects = [](Bytes b, const char* what) {
        Packet t;
        t.messages = 777;
        const bool ok = parsePacket(b.data(), b.size(), t);
        CHECK(!ok && t.messages == 777, "should reject: %s", what);
    };
    {
        Bytes b = one;
        b[30] = 'x';   // a padding byte after the address' NUL (29 characters, NUL at 29)
        rejects(b, "non-zero padding");
    }
    {
        Bytes b = bundle({one});
        b[16 + 3] = 200;   // element size past the end
        rejects(b, "oversized element");
    }
    {
        Bytes b = bundle({one});
        b[16 + 3] = 6;   // element size not a multiple of 4
        rejects(b, "unaligned element");
    }
    {
        Bytes b = bundle({one});
        b[16 + 0] = b[16 + 1] = b[16 + 2] = b[16 + 3] = 0;   // zero-size element
        rejects(b, "zero-size element");
    }
    {
        Bytes b = one;
        b.pop_back();
        rejects(b, "length not a multiple of 4");
    }
    {
        const uint32_t qnan = 0x7fc00000u;
        float nanF;
        std::memcpy(&nanF, &qnan, 4);
        rejects(msg3("/tracking/trackers/1/position", nanF, 0.f, 0.f), "NaN");
        const uint32_t inf = 0x7f800000u;
        float infF;
        std::memcpy(&infF, &inf, 4);
        rejects(msg3("/tracking/trackers/1/rotation", 0.f, infF, 0.f), "infinity");
        rejects(msg3("/tracking/trackers/1/position", 5000.f, 0.f, 0.f), "absurd position");
    }
    {
        Bytes b = one;
        b.insert(b.end(), {0, 0, 0, 0});
        rejects(b, "extra argument bytes");
    }
    {
        Bytes inner = bundle({one});
        for (int d = 0; d < MAX_BUNDLE_DEPTH + 1; ++d) inner = bundle({inner});
        rejects(inner, "nesting too deep");
        Bytes ok = bundle({bundle({one})});
        Packet t;
        CHECK(parsePacket(ok.data(), ok.size(), t) && t.t[T_RFOOT].hasPos, "nested bundle within the limit");
    }
    {
        std::vector<Bytes> many(MAX_MESSAGES + 1, one);
        rejects(bundle(many), "too many messages");
    }
    {
        Bytes b;
        putStr(b, "/tracking/trackers/1/position");   // no terminator inside: cut the NUL and padding
        b.resize(28);
        rejects(b, "address without its NUL");
    }
    // Ignored (well-formed, not ours): other types on our address, an unknown slot, no type tags.
    {
        Bytes b;
        putStr(b, "/tracking/trackers/1/position");
        putStr(b, ",ff");
        putF(b, 1.f);
        putF(b, 2.f);
        Packet t;
        CHECK(parsePacket(b.data(), b.size(), t) && t.ignored == 1 && !t.t[T_HIP].hasPos, ",ff ignored");
        const Bytes nine = msg3("/tracking/trackers/9/position", 1.f, 2.f, 3.f);
        Packet t2;
        CHECK(parsePacket(nine.data(), nine.size(), t2) && t2.ignored == 1 && t2.messages == 0, "tracker 9 ignored");
        Bytes nt;
        putStr(nt, "/tracking/trackers/1/position");
        Packet t3;
        CHECK(parsePacket(nt.data(), nt.size(), t3) && t3.ignored == 1, "no type tag ignored");
    }
    rejects(Bytes(), "empty");
    rejects(Bytes{'h', 'i', 0, 0, 0, 0, 0, 0}, "not an OSC address");

    // Random corruption: never crashes (run under ASan / UBSan by the check), never accepts NaN.
    uint32_t seed = 12345;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int it = 0; it < 20000; ++it)
    {
        Bytes b = full;
        const int flips = 1 + int(rnd() % 8);
        for (int k = 0; k < flips; ++k) b[rnd() % b.size()] = (uint8_t)rnd();
        if (rnd() % 4 == 0) b.resize((rnd() % (b.size() / 4)) * 4);
        Packet t;
        if (parsePacket(b.data(), b.size(), t))
        {
            for (const Sample& s : t.t)
                for (int k = 0; k < 3; ++k)
                    CHECK(std::isfinite(s.pos[k]) && std::isfinite(s.euler[k]), "accepted a non-finite value");
        }
    }
}

// ---- CONVERT ----------------------------------------------------------------------------------
static void testConvert()
{
    // Round trips through SlimeVR's encoding.
    const Q qs[] = {Q(), slfbt::qaxis(V3(0.f, 1.f, 0.f), 0.7f), slfbt::qaxis(V3(1.f, 0.f, 0.f), -0.4f),
                    slfbt::qnormalized(Q(0.3f, -0.5f, 0.2f, 0.8f)), slfbt::qnormalized(Q(-0.6f, 0.1f, 0.7f, 0.2f))};
    for (const Q& q : qs)
    {
        float pos[3], e[3];
        slimeEncode(V3(0.3f, 1.2f, -0.7f), q, pos, e);
        CHECK(nearV(unityPosToXr(pos), V3(0.3f, 1.2f, -0.7f), 1e-6f), "position round trip");
        CHECK(slfbt::qangle(unityEulerToXr(e), q) < 2e-3f, "rotation round trip (%.4f)", slfbt::qangle(unityEulerToXr(e), q));
    }
    // Unity yaw -90 deg (a left turn in Unity's left-handed frame): forward (-Z) turns to -X (left).
    const float left[3] = {0.f, -90.f, 0.f};
    CHECK(nearV(slfbt::qrot(unityEulerToXr(left), V3(0.f, 0.f, -1.f)), V3(-1.f, 0.f, 0.f), 1e-5f), "unity yaw -90 = left turn");
    // Unity pitch +30 (x: nose down in Unity) -> nose down here too.
    const float down[3] = {30.f, 0.f, 0.f};
    CHECK(slfbt::qrot(unityEulerToXr(down), V3(0.f, 0.f, -1.f)).y < -0.4f, "unity pitch +30 = nose down");
    // Unity +z is forward: -Z here.
    const float fwd[3] = {0.f, 0.f, 1.f};
    CHECK(nearV(unityPosToXr(fwd), V3(0.f, 0.f, -1.f), 1e-6f), "unity +z = forward");
    // The reset's head message (0, -y, 0), y the right-handed heading: decodes to that heading.
    const float headDeg = 40.f;
    const float reset[3] = {0.f, -headDeg, 0.f};
    CHECK(angleDiff(headingOf(unityEulerToXr(reset)), headDeg * 3.14159265f / 180.f) < 1e-4f, "reset heading");
}

// ---- ALIGN ------------------------------------------------------------------------------------
// A standing body in SlimeVR space (no headset feed: the head at the origin, facing heading h).
struct Body
{
    V3 p[T_COUNT];
    Q q[T_COUNT];
};
static Body slimeBody(float heading, const V3& shift = V3())
{
    const Q R = yawQ(heading);
    Body b;
    const V3 local[T_COUNT] = {V3(0.f, 0.f, 0.f),      V3(0.f, -0.75f, 0.05f),  V3(-0.1f, -1.55f, 0.f), V3(0.1f, -1.55f, 0.f),
                               V3(-0.1f, -1.15f, 0.f), V3(0.1f, -1.15f, 0.f),   V3(0.f, -0.35f, 0.05f), V3(-0.45f, -0.3f, 0.f),
                               V3(0.45f, -0.3f, 0.f)};
    for (int i = 0; i < T_COUNT; ++i)
    {
        b.p[i] = slfbt::qrot(R, local[i]) + (i ? shift : V3());
        b.q[i] = R;
    }
    return b;
}
static AlignInput inputOf(const Body& b, bool pairs = true)
{
    AlignInput in;
    for (int i = 0; i < T_COUNT; ++i)
    {
        in.have[i] = true;
        in.pos[i] = b.p[i];
        in.rot[i] = b.q[i];
    }
    if (!pairs) in.have[T_LELBOW] = in.have[T_RELBOW] = in.have[T_LKNEE] = in.have[T_RKNEE] = false;
    if (!pairs) in.pos[T_LFOOT] = in.pos[T_RFOOT] = (b.p[T_LFOOT] + b.p[T_RFOOT]) * 0.5f;   // feet together
    return in;
}
static void testAlign()
{
    const float slimeHeading = 1.9f;            // SlimeVR's forward: anywhere
    const float hmdHeading = -0.6f;
    const V3 hmd(2.f, 1.65f, 3.f);
    const Q hmdRot = slfbt::qmul(yawQ(hmdHeading), slfbt::qaxis(V3(1.f, 0.f, 0.f), -0.2f));   // looking a little down
    const Body b = slimeBody(slimeHeading);
    YawFrom from;
    const Alignment a = solveAlignment(inputOf(b), hmd, hmdRot, true, 0.f, from);
    CHECK(a.valid && from == YawFrom::PAIRS, "solved from pairs");
    CHECK(angleDiff(a.yaw, hmdHeading - slimeHeading) < 1e-3f, "yaw %.4f vs %.4f", a.yaw, hmdHeading - slimeHeading);

    auto mapped = [&](const Alignment& al, const Body& bb, int i, const V3& hp, const Q& hq) {
        V3 p;
        Q q;
        applyAlignment(al, bb.p[T_HEAD], hp, hq, bb.p[i], bb.q[i], p, q);
        return p;
    };
    const V3 fwd = slfbt::qrot(yawQ(hmdHeading), V3(0.f, 0.f, -1.f));
    const V3 xrLeft = slfbt::qrot(yawQ(hmdHeading), V3(-1.f, 0.f, 0.f));
    const V3 lf = mapped(a, b, T_LFOOT, hmd, hmdRot), rf = mapped(a, b, T_RFOOT, hmd, hmdRot);
    const V3 mid = (lf + rf) * 0.5f;
    CHECK(std::fabs(mid.y - 0.08f) < 1e-3f, "feet at ankle height (%.3f)", mid.y);
    const V3 under = hmd - fwd * 0.08f;
    CHECK(std::fabs(mid.x - under.x) < 1e-3f && std::fabs(mid.z - under.z) < 1e-3f, "feet under the head");
    CHECK(slfbt::dot(lf - rf, xrLeft) > 0.15f, "left foot on the left");
    // Rotations: SlimeVR's hip faces SlimeVR's forward -> faces the headset's heading here.
    {
        V3 p;
        Q q;
        applyAlignment(a, b.p[T_HEAD], hmd, hmdRot, b.p[T_HIP], b.q[T_HIP], p, q);
        CHECK(angleDiff(headingOf(q), hmdHeading) < 1e-3f, "hip heading mapped");
    }
    // Walk 1.2 m and turn 0.5 rad (SlimeVR has no headset: its body does not move or turn its
    // head point... it turns the body around the head): the feet follow.
    {
        const float turn = 0.5f;
        const Body b2 = slimeBody(slimeHeading + turn);
        const V3 hmd2 = hmd + slfbt::qrot(yawQ(hmdHeading), V3(0.f, 0.f, -1.2f));
        const Q hmdRot2 = slfbt::qmul(yawQ(turn), hmdRot);
        const V3 mid2 = (mapped(a, b2, T_LFOOT, hmd2, hmdRot2) + mapped(a, b2, T_RFOOT, hmd2, hmdRot2)) * 0.5f;
        const V3 fwd2 = slfbt::qrot(yawQ(hmdHeading + turn), V3(0.f, 0.f, -1.f));
        const V3 under2 = hmd2 - fwd2 * 0.08f;
        CHECK(std::fabs(mid2.x - under2.x) < 2e-3f && std::fabs(mid2.z - under2.z) < 2e-3f, "feet follow a walk and turn");
        CHECK(std::fabs(mid2.y - 0.08f) < 2e-3f, "feet stay on the floor");
    }
    // A head sway of 10 cm forward and 5 cm down with the feet planted (no head turn): SlimeVR,
    // head-anchored, sees the body move back / up under its fixed head. The feet stay put.
    {
        const V3 swayXr(fwd.x * 0.1f, -0.05f, fwd.z * 0.1f);
        const V3 swaySlime = slfbt::qrot(yawQ(-a.yaw), swayXr);
        const Body b3 = slimeBody(slimeHeading, V3() - swaySlime);
        const V3 lf3 = mapped(a, b3, T_LFOOT, hmd + swayXr, hmdRot);
        CHECK(nearV(lf3, lf, 1e-3f), "planted foot under a head sway (moved %.4f m)", slfbt::length(lf3 - lf));
    }
    // With a SlimeVR head feed (its head follows the headset): the same mapping of the feet.
    {
        Body b4 = b;
        const V3 headAt(0.4f, 1.6f, -0.2f);
        for (int i = 0; i < T_COUNT; ++i) b4.p[i] = b4.p[i] + headAt;
        const Alignment a4 = solveAlignment(inputOf(b4), hmd, hmdRot, true, 0.f, from);
        V3 p;
        Q q;
        applyAlignment(a4, b4.p[T_HEAD], hmd, hmdRot, b4.p[T_LFOOT], b4.q[T_LFOOT], p, q);
        CHECK(nearV(p, lf, 1e-3f), "head-fed SlimeVR maps the same");
    }
    // The hip-heading fallback (feet together, no knees / elbows) solves the same yaw.
    {
        const Alignment ah = solveAlignment(inputOf(b, false), hmd, hmdRot, true, 0.f, from);
        CHECK(ah.valid && from == YawFrom::HIP && angleDiff(ah.yaw, a.yaw) < 1e-3f, "hip fallback (%.4f vs %.4f)", ah.yaw, a.yaw);
    }
    // LOCAL space (no floor): the feet keep SlimeVR's head -> feet height below the headset.
    {
        const Alignment al = solveAlignment(inputOf(b), hmd, hmdRot, false, 0.f, from);
        const V3 l = mapped(al, b, T_LFOOT, hmd, hmdRot);
        CHECK(std::fabs(l.y - (hmd.y - 1.55f)) < 1e-3f, "LOCAL space height (%.3f)", l.y);
    }
    // Looking straight down at your feet: the heading stays the head's (no jump of the body).
    {
        // (With a 3 degree wobble about the head's own up axis: straight down, the forward's
        // horizontal remainder points anywhere.)
        const Q down89 = slfbt::qmul(yawQ(hmdHeading), slfbt::qmul(slfbt::qaxis(V3(1.f, 0.f, 0.f), -1.553f),
                                                                    slfbt::qaxis(V3(0.f, 1.f, 0.f), 0.05f)));
        CHECK(angleDiff(headingOf(down89), hmdHeading) < 1e-2f, "heading looking down (%.4f)", headingOf(down89));
        const Q up89 = slfbt::qmul(yawQ(hmdHeading), slfbt::qaxis(V3(1.f, 0.f, 0.f), 1.553f));
        CHECK(angleDiff(headingOf(up89), hmdHeading) < 1e-2f, "heading looking up");
        const V3 lfDown = mapped(a, b, T_LFOOT, hmd, down89);
        CHECK(nearV(lfDown, lf, 2e-3f), "feet stay when looking down (moved %.4f m)", slfbt::length(lfDown - lf));
    }
    // Nothing to solve from: invalid.
    {
        AlignInput none;
        none.have[T_HEAD] = true;
        CHECK(!solveAlignment(none, hmd, hmdRot, true, 0.f, from).valid, "no trackers: no alignment");
    }
    // A SlimeVR reset to its heading 0.3 while you face hmdHeading: the yaw re-solves to that.
    {
        Alignment r = a;
        realignYaw(r, yawQ(0.3f), hmdRot);
        CHECK(angleDiff(r.yaw, hmdHeading - 0.3f) < 1e-4f, "reset realign");
    }
}


// ---- VMC --------------------------------------------------------------------------------------
// A right-handed (OpenXR axes) local rotation as SlimeVR's VMCHandler sends it: (x, y, -z, -w).
static Bytes vmcBone(const std::string& name, const Q& q, const V3& p = V3())
{
    Bytes m;
    putStr(m, "/VMC/Ext/Bone/Pos");
    putStr(m, ",sfffffff");
    putStr(m, name);
    putF(m, p.x);
    putF(m, p.y);
    putF(m, -p.z);
    putF(m, q.x);
    putF(m, q.y);
    putF(m, -q.z);
    putF(m, -q.w);
    return m;
}
static Bytes vmcSimple(const std::string& addr, const std::string& tags)
{
    Bytes m;
    putStr(m, addr);
    putStr(m, tags);
    if (tags == ",f") putF(m, 1.5f);
    if (tags == ",i") put32(m, 1);
    return m;
}

static void testVmcParser()
{
    const Q qa = slfbt::qnormalized(Q(0.1f, 0.7f, -0.2f, 0.6f));
    std::vector<Bytes> el = {vmcSimple("/VMC/Ext/T", ",f"), vmcSimple("/VMC/Ext/OK", ",i")};
    {
        Bytes root;
        putStr(root, "/VMC/Ext/Root/Pos");
        putStr(root, ",sfffffff");
        putStr(root, "root");
        for (int k = 0; k < 6; ++k) putF(root, 0.f);
        putF(root, -1.f);
        el.push_back(root);
    }
    el.push_back(vmcBone("Hips", qa, V3(0.f, 0.9f, 0.f)));
    el.push_back(vmcBone("Spine", Q()));
    el.push_back(vmcBone("LeftUpperArm", Q()));
    el.push_back(vmcBone("RightLittleDistal", Q()));
    el.push_back(vmcBone("LeftEye", Q()));        // not a body bone: ignored
    const Bytes full = bundle(el);
    Packet p;
    CHECK(parsePacket(full.data(), full.size(), p), "VMC bundle parses");
    CHECK(p.vmcBones == 4, "VMC bones used %u", p.vmcBones);
    CHECK(p.boneHave[slvmc::U_HIPS] && p.boneHave[slvmc::U_SPINE] && p.boneHave[slvmc::U_L_UPPER_ARM] &&
              p.boneHave[slvmc::U_R_FINGER0 + 14],
          "VMC bone names");
    CHECK(slfbt::qangle(slvmc::unityRotToXr(p.boneRot[slvmc::U_HIPS]), qa) < 1e-4f, "VMC rotation round trip");
    CHECK(p.ignored == 4, "VMC others ignored (%u)", p.ignored);   // T, OK, Root (not a bone), LeftEye

    // Truncations: rejected unless exactly between elements.
    std::vector<size_t> boundaries;
    {
        size_t off = 16;
        boundaries.push_back(off);
        for (const Bytes& e : el)
        {
            off += 4 + e.size();
            boundaries.push_back(off);
        }
    }
    for (size_t n = 0; n < full.size(); ++n)
    {
        bool boundary = false;
        for (size_t b : boundaries) boundary |= b == n;
        if (boundary) continue;
        Packet t;
        t.vmcBones = 999;
        CHECK(!parsePacket(full.data(), n, t) && t.vmcBones == 999, "VMC truncated at %zu accepted", n);
    }

    auto rejects = [](const Bytes& b, const char* what) {
        Packet t;
        t.vmcBones = 777;
        CHECK(!parsePacket(b.data(), b.size(), t) && t.vmcBones == 777, "VMC should reject: %s", what);
    };
    auto ignores = [](const Bytes& b, const char* what) {
        Packet t;
        CHECK(parsePacket(b.data(), b.size(), t) && t.vmcBones == 0 && t.ignored == 1, "VMC should ignore: %s", what);
    };
    {
        Bytes b = vmcBone("Hips", Q());
        b.resize(b.size() - 4);   // one float short
        rejects(b, "six floats");
        Bytes c = vmcBone("Hips", Q());
        c.insert(c.end(), {0, 0, 0, 0});
        rejects(c, "eight floats");
    }
    {
        // The name's NUL and padding overwritten: the string runs into the floats.
        Bytes b = vmcBone("Hips", Q());
        const size_t nameOff = 20 + 12;   // "/VMC/Ext/Bone/Pos" (20) + ",sfffffff" (12)
        for (size_t k = nameOff + 4; k < nameOff + 8; ++k) b[k] = 'x';
        Packet t;
        t.vmcBones = 5;
        const bool ok = parsePacket(b.data(), b.size(), t);
        CHECK(!ok || t.vmcBones == 0, "VMC name without its NUL used as a bone");
        Bytes c = vmcBone("Hips", Q());
        c[nameOff + 6] = 'q';   // a padding byte after "Hips\0"
        rejects(c, "non-zero padding in the name");
    }
    {
        const uint32_t qnan = 0x7fc00000u;
        float nanF;
        std::memcpy(&nanF, &qnan, 4);
        rejects(vmcBone("Hips", Q(nanF, 0.f, 0.f, 1.f)), "NaN rotation");
        rejects(vmcBone("Hips", Q(), V3(nanF, 0.f, 0.f)), "NaN position");
        rejects(vmcBone("Hips", Q(0.f, 0.f, 0.f, 0.1f)), "rotation far from unit length");
        rejects(vmcBone("Hips", Q(3.f, 0.f, 0.f, 1.f)), "rotation component past 1");
    }
    ignores(vmcBone("Hips2", Q()), "a name with a digit");
    ignores(vmcBone("Left Foot", Q()), "a name with a space");
    ignores(vmcBone(std::string(60, 'A'), Q()), "an overlong name");
    ignores(vmcBone("", Q()), "an empty name");
    ignores(vmcBone("hips", Q()), "a wrong-case name");
    {
        Bytes b;
        putStr(b, "/VMC/Ext/Bone/Pos");
        putStr(b, ",sfff");
        putStr(b, "Hips");
        putF(b, 0.f);
        putF(b, 0.f);
        putF(b, 0.f);
        ignores(b, "other argument types");
    }
    {
        std::string odd = "Hi";
        odd += char(0xC3);
        odd += char(0xA9);
        ignores(vmcBone(odd, Q()), "non-ASCII bytes");
    }
}

static void testVmcSkeleton()
{
    using namespace slvmc;
    const float d90 = 1.5707963f;
    // Every local identity: the SL skeleton in its rest pose (identity worlds).
    bool hl[U_COUNT] = {}, have[U_COUNT];
    Q local[U_COUNT], g[U_COUNT];
    for (int b = 0; b < U_COUNT; ++b)
    {
        hl[b] = b < U_L_FINGER0;   // no fingers
        local[b] = Q();
    }
    hl[U_L_TOES] = hl[U_R_TOES] = false;
    composeGlobals(hl, local, have, g);
    bool out[S_COUNT];
    Q w[S_COUNT];
    toSecondLife(have, g, out, w);
    for (int j = 0; j < S_COUNT; ++j)
        if (!isFingerJoint(j)) CHECK(out[j] && slfbt::qangle(w[j], Q()) < 1e-5f, "rest: %s identity", sjointInfo(j).name);
    CHECK(!out[S_L_FINGER0], "no fingers sent: none driven");
    CHECK(out[S_L_FOOT] && out[S_R_FOOT], "feet from the ankles (no toes)");

    // Hips turned 90 deg to the left (right-handed about +Y in OpenXR): SL pelvis faces +Y (left).
    local[U_HIPS] = slfbt::qaxis(V3(0.f, 1.f, 0.f), d90);
    composeGlobals(hl, local, have, g);
    toSecondLife(have, g, out, w);
    CHECK(std::fabs(slfbt::yawOf(w[S_PELVIS]) - d90) < 1e-4f, "hips left turn -> SL yaw +90 (%.3f)", slfbt::yawOf(w[S_PELVIS]));
    CHECK(slfbt::qangle(w[S_L_HIP], w[S_PELVIS]) < 1e-4f, "children inherit the hips' turn");
    local[U_HIPS] = Q();

    // The left upper arm down (about +Z: -X -> -Y): SL's left arm (+Y) points down (-Z).
    local[U_L_UPPER_ARM] = slfbt::qaxis(V3(0.f, 0.f, 1.f), d90);
    // The left knee bent 90 deg (the shin -Y -> +Z, backwards): SL's shin (-Z) points back (-X).
    local[U_L_LOWER_LEG] = slfbt::qaxis(V3(1.f, 0.f, 0.f), -d90);
    composeGlobals(hl, local, have, g);
    toSecondLife(have, g, out, w);
    CHECK(nearV(slfbt::qrot(w[S_L_SHOULDER], V3(0.f, 1.f, 0.f)), V3(0.f, 0.f, -1.f), 1e-4f), "left arm down");
    CHECK(nearV(slfbt::qrot(w[S_L_ELBOW], V3(0.f, 1.f, 0.f)), V3(0.f, 0.f, -1.f), 1e-4f), "forearm follows the arm");
    CHECK(nearV(slfbt::qrot(w[S_L_KNEE], V3(0.f, 0.f, -1.f)), V3(-1.f, 0.f, 0.f), 1e-4f), "knee bends backwards");
    CHECK(nearV(slfbt::qrot(w[S_L_ANKLE], V3(1.f, 0.f, 0.f)), V3(0.f, 0.f, -1.f), 1e-4f), "foot follows the shin");
    CHECK(slfbt::qangle(w[S_R_KNEE], Q()) < 1e-5f, "right leg untouched");
    local[U_L_UPPER_ARM] = local[U_L_LOWER_LEG] = Q();

    // SlimeVR's own arm convention (UnityArmature.setGlobalRotationForBone): its arm bones rest
    // hanging down, and it sends global * OFFSET (left: +90 deg about Z, right: -90). A T-pose arm
    // (hanging -> out to the side: left -90 deg about Z) then arrives as identity, and lands on SL's
    // rest arm (+Y left, -Y right) unchanged.
    {
        const Q offL = slfbt::qaxis(V3(0.f, 0.f, 1.f), d90), offR = slfbt::qaxis(V3(0.f, 0.f, 1.f), -d90);
        const Q gL = slfbt::qmul(slfbt::qaxis(V3(0.f, 0.f, 1.f), -d90), offL);   // left arm out, sent
        const Q gR = slfbt::qmul(slfbt::qaxis(V3(0.f, 0.f, 1.f), d90), offR);    // right arm out, sent
        // The shoulder (collar) and the arm get the same offset: the arm's local is shoulder^-1 * arm.
        local[U_L_SHOULDER] = gL;
        local[U_L_UPPER_ARM] = slfbt::qmul(slfbt::qconj(gL), gL);
        local[U_R_SHOULDER] = gR;
        local[U_R_UPPER_ARM] = slfbt::qmul(slfbt::qconj(gR), gR);
        composeGlobals(hl, local, have, g);
        toSecondLife(have, g, out, w);
        CHECK(nearV(slfbt::qrot(w[S_L_SHOULDER], V3(0.f, 1.f, 0.f)), V3(0.f, 1.f, 0.f), 1e-4f), "T-pose left arm stays out");
        CHECK(nearV(slfbt::qrot(w[S_R_SHOULDER], V3(0.f, -1.f, 0.f)), V3(0.f, -1.f, 0.f), 1e-4f), "T-pose right arm stays out");
        // Hanging arms (SlimeVR's rest: global identity) arrive as the offset: SL's left arm points down.
        local[U_L_SHOULDER] = offL;
        local[U_L_UPPER_ARM] = Q();
        composeGlobals(hl, local, have, g);
        toSecondLife(have, g, out, w);
        CHECK(nearV(slfbt::qrot(w[S_L_SHOULDER], V3(0.f, 1.f, 0.f)), V3(0.f, 0.f, -1.f), 1e-4f), "hanging left arm points down");
        local[U_L_SHOULDER] = local[U_L_UPPER_ARM] = local[U_R_SHOULDER] = local[U_R_UPPER_ARM] = Q();
    }

    // Spine: Spine, Chest, UpperChest each lean forward 15 deg (about the left axis +X in OpenXR:
    // the head goes forward, -Z). The SL spine curves through every segment.
    const float lean = 0.2617994f;
    local[U_SPINE] = local[U_CHEST] = local[U_UPPER_CHEST] = slfbt::qaxis(V3(1.f, 0.f, 0.f), -lean);
    composeGlobals(hl, local, have, g);
    toSecondLife(have, g, out, w);
    auto pitchFwd = [](const Q& q) {   // how far the joint's up (+Z) leans forward (+X), radians
        const V3 up = slfbt::qrot(q, V3(0.f, 0.f, 1.f));
        return std::atan2(up.x, up.z);
    };
    CHECK(std::fabs(pitchFwd(w[S_PELVIS])) < 1e-4f, "pelvis upright");
    CHECK(std::fabs(pitchFwd(w[S_SPINE2]) - 0.5f * lean) < 1e-3f, "mSpine2 halfway (%.4f)", pitchFwd(w[S_SPINE2]));
    CHECK(std::fabs(pitchFwd(w[S_TORSO]) - lean) < 1e-3f, "mTorso = Spine");
    CHECK(std::fabs(pitchFwd(w[S_SPINE4]) - 2.f * lean) < 1e-3f, "mSpine4 = Chest");
    CHECK(std::fabs(pitchFwd(w[S_CHEST]) - 3.f * lean) < 1e-3f, "mChest = UpperChest (%.4f)", pitchFwd(w[S_CHEST]));
    // Each step of the chain bends a little more (no joint takes it all).
    const int chain[] = {S_PELVIS, S_SPINE1, S_SPINE2, S_TORSO, S_SPINE3, S_SPINE4, S_CHEST};
    for (int k = 1; k < 7; ++k) CHECK(pitchFwd(w[chain[k]]) >= pitchFwd(w[chain[k - 1]]) - 1e-5f, "spine monotonic at %d", k);
    // The locals put back together give the worlds.
    const Q rootW = yawQ(0.3f);
    for (int j = 0; j < S_COUNT; ++j)
    {
        if (!out[j]) continue;
        const int par = sjointInfo(j).parent;
        const Q pw = par < 0 ? rootW : w[par];
        CHECK(slfbt::qangle(slfbt::qmul(pw, localOf(j, w, rootW)), w[j]) < 1e-4f, "local of %s", sjointInfo(j).name);
    }
    // No UpperChest: mChest takes the Chest.
    hl[U_UPPER_CHEST] = false;
    composeGlobals(hl, local, have, g);
    toSecondLife(have, g, out, w);
    CHECK(std::fabs(pitchFwd(w[S_CHEST]) - 2.f * lean) < 1e-3f, "mChest = Chest without UpperChest");
    CHECK(!out[S_L_COLLAR] && !out[S_L_SHOULDER], "arms hang from UpperChest: not driven without it");
    hl[U_UPPER_CHEST] = true;
    // Arms not sent (SlimeVR tracks them from the controllers): not driven; the rest is.
    for (int b : {U_L_SHOULDER, U_L_UPPER_ARM, U_L_LOWER_ARM, U_L_HAND}) hl[b] = false;
    composeGlobals(hl, local, have, g);
    toSecondLife(have, g, out, w);
    CHECK(!out[S_L_COLLAR] && !out[S_L_WRIST] && out[S_R_WRIST] && out[S_L_KNEE], "left arm not sent: not driven");
    // Fingers: a curled index (each joint 30 deg about the hand's forward... any axis) maps 1 / 2 / 3.
    for (int b : {U_R_FINGER0 + 3, U_R_FINGER0 + 4, U_R_FINGER0 + 5})
    {
        hl[b] = true;
        local[b] = slfbt::qaxis(V3(0.f, 0.f, 1.f), 0.5f);
    }
    composeGlobals(hl, local, have, g);
    toSecondLife(have, g, out, w);
    CHECK(out[S_R_FINGER0 + 3] && out[S_R_FINGER0 + 5] && !out[S_R_FINGER0], "right index driven, thumb not");
    CHECK(std::fabs(slfbt::qangle(w[S_R_FINGER0 + 5], w[S_R_WRIST]) - 1.5f) < 1e-3f, "index curls through its joints");
    // Names: every Unity name round-trips; parents come before children.
    for (int b = 0; b < U_COUNT; ++b)
    {
        const char* n = uboneInfo(b).name;
        CHECK(uboneByName(n, std::strlen(n)) == b, "name %s", n);
        CHECK(uboneInfo(b).parent < b, "parent order %s", n);
    }
    for (int j = 0; j < S_COUNT; ++j) CHECK(sjointInfo(j).parent < j, "SL parent order %s", sjointInfo(j).name);
}

int main()
{
    testOsc();
    testConvert();
    testAlign();
    testVmcParser();
    testVmcSkeleton();
    if (g_fail)
    {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("vr_slime: all checks passed\n");
    return 0;
}
