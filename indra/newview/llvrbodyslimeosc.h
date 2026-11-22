/**
 * @file llvrbodyslimeosc.h
 * @brief #1582 SlimeVR body trackers over VRChat OSC: the pure part (no viewer types, no sockets),
 *        shared by LLVRBodySlime (llvrbodyslime.cpp) and the fixtures in
 *        scripts/check/test_vr_slime.cpp (checks/vr_slime.py, host compiler only).
 *
 * What SlimeVR server sends (Settings > OSC > VRChat OSC Trackers; VRCOSCHandler.kt), one OSC
 * bundle per server tick to 127.0.0.1:9000 by default:
 *   /tracking/trackers/<n>/position   f f f   metres, Unity axes (left-handed, Y up, +Z forward)
 *   /tracking/trackers/<n>/rotation   f f f   Euler degrees (x, y, z), Unity order: q = Y * X * Z
 *   /tracking/trackers/head/position  f f f   SlimeVR's head point (the HMD's, if it has one)
 *   /tracking/trackers/head/rotation  0 y 0   sent once, on a SlimeVR reset (the reset's heading)
 * n is fixed by body part (SlimeVR keeps it stable for third-party apps): 1 hip, 2 left foot,
 * 3 right foot, 4 left knee (upper leg), 5 right knee, 6 chest (upper chest), 7 left elbow
 * (upper arm), 8 right elbow.
 * SlimeVR's own space is right-handed, Y up, -Z forward (it flips z to talk to VRChat); undoing
 * that flip gives OpenXR axes directly: p = (x, y, -z), q = (-qx, -qy, qz, qw).
 *
 * Alignment (OpenXR app space): SlimeVR's space has its own heading and origin. A yaw about +Y
 * (solved once, at the T-pose calibration, from the left / right pairs of trackers against the
 * headset's heading) and a translation re-anchored on the headset every frame: SlimeVR's head
 * point maps onto (HMD - yaw(HMD) * headOffset), headOffset solved at the calibration so that
 * the feet land under the head at ankle height. Every frame, because without a headset feed
 * SlimeVR's skeleton hangs from a fixed head (its positions are relative to the head); with one,
 * its head follows the headset and the per-frame anchor is constant anyway.
 *
 * Conventions: slfbt::V3 / Q (Hamilton x y z w; qmul(a, b) applies b first), here in OpenXR
 * axes (X right, Y up, -Z forward).
 */
#pragma once

#include "llvrbodymath.h"
#include "llvrbodyvmc.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace slslime
{
using slfbt::Q;
using slfbt::V3;

// ---- Tracker slots ---------------------------------------------------------------------------
enum : int
{
    T_HEAD = 0,   // /tracking/trackers/head
    T_HIP = 1,
    T_LFOOT,
    T_RFOOT,
    T_LKNEE,
    T_RKNEE,
    T_CHEST,
    T_LELBOW,
    T_RELBOW,
    T_COUNT       // 9
};
inline const char* slotName(int s)
{
    static const char* const k[T_COUNT] = {"head", "hip", "left foot", "right foot", "left knee",
                                           "right knee", "chest", "left elbow", "right elbow"};
    return (s >= 0 && s < T_COUNT) ? k[s] : "?";
}

// ---- OSC packet parser -----------------------------------------------------------------------
// One packet's tracker data. Filled only from a packet that parses completely: a malformed
// packet (truncated, bad padding, sizes past its end, too deep, non-finite numbers) leaves
// nothing behind.
struct Sample
{
    bool hasPos = false, hasRot = false;
    float pos[3] = {0.f, 0.f, 0.f};   // as sent (Unity axes)
    float euler[3] = {0.f, 0.f, 0.f}; // as sent (degrees)
};
struct Packet
{
    Sample t[T_COUNT];
    uint32_t messages = 0;   // VRChat OSC tracker messages used
    uint32_t ignored = 0;    // well-formed messages for other addresses (or other argument types)
    // VMC (/VMC/Ext/Bone/Pos): each Unity bone's local rotation as sent (Unity axes, x y z w).
    bool boneHave[slvmc::U_COUNT] = {};
    float boneRot[slvmc::U_COUNT][4] = {};
    uint32_t vmcBones = 0;   // VMC bone messages used
};

constexpr size_t MAX_ADDRESS = 64;      // longer addresses are not ours (skipped, not parsed)
constexpr int MAX_BUNDLE_DEPTH = 4;
constexpr uint32_t MAX_MESSAGES = 256;  // per packet (more: malformed)
constexpr float MAX_ABS_POS = 1000.f;   // metres: anything past this is garbage
constexpr float MAX_ABS_ANGLE = 100000.f;
constexpr size_t MAX_BONE_NAME = 40;    // a longer VMC bone name is not a humanoid bone (ignored)

namespace detail
{
inline uint32_t be32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
// Finite by its bits (std::isfinite is folded away under fast-math: the Windows build's /fp:fast).
inline bool finiteBits(uint32_t u) { return (u & 0x7f800000u) != 0x7f800000u; }
inline float beFloat(const uint8_t* p)
{
    const uint32_t u = be32(p);
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}
// An OSC string at data[off..len): NUL-terminated, padded with NULs to a multiple of 4.
// Returns the offset past the padding, or 0 when malformed. strLen: its length (no NUL).
inline size_t oscString(const uint8_t* data, size_t len, size_t off, size_t& strLen)
{
    size_t i = off;
    while (i < len && data[i] != 0) ++i;
    if (i >= len) return 0;   // no terminator inside the packet
    strLen = i - off;
    const size_t end = off + ((strLen + 4) & ~size_t(3));
    if (end > len) return 0;
    for (size_t k = i; k < end; ++k)
        if (data[k] != 0) return 0;
    return end;
}
// "/tracking/trackers/<head|1..8>/<position|rotation>": the slot and whether it is the rotation.
inline bool trackerAddress(const char* a, size_t n, int& slot, bool& rotation)
{
    static const char kPre[] = "/tracking/trackers/";
    const size_t pre = sizeof(kPre) - 1;
    if (n <= pre || std::memcmp(a, kPre, pre) != 0) return false;
    const char* s = a + pre;
    size_t rest = n - pre;
    if (rest >= 5 && std::memcmp(s, "head/", 5) == 0)
    {
        slot = T_HEAD;
        s += 5;
        rest -= 5;
    }
    else if (rest >= 2 && s[0] >= '1' && s[0] <= '8' && s[1] == '/')
    {
        slot = s[0] - '0';
        s += 2;
        rest -= 2;
    }
    else
    {
        return false;
    }
    if (rest == 8 && std::memcmp(s, "position", 8) == 0)
    {
        rotation = false;
        return true;
    }
    if (rest == 8 && std::memcmp(s, "rotation", 8) == 0)
    {
        rotation = true;
        return true;
    }
    return false;
}

// /VMC/Ext/Bone/Pos ,sfffffff: name, position x y z, rotation x y z w. A structurally broken
// message (a string without its NUL inside the message, bad padding, a length other than the
// arguments', a non-finite or absurd number, a rotation far from unit length) is malformed; a
// well-formed one with other argument types, or a name that is not a humanoid bone (too long,
// characters other than letters, eyes / jaw / unknown), is ignored.
inline bool parseVmcBone(const uint8_t* data, size_t len, size_t tagOff, size_t tagLen, size_t argOff, Packet& out)
{
    if (tagLen != 9 || std::memcmp(data + tagOff, ",sfffffff", 9) != 0)
    {
        ++out.ignored;
        return true;
    }
    size_t nameLen = 0;
    const size_t floatOff = oscString(data, len, argOff, nameLen);
    if (!floatOff || len - floatOff != 28) return false;
    float v[7];
    for (int k = 0; k < 7; ++k)
    {
        if (!finiteBits(be32(data + floatOff + 4 * k))) return false;
        v[k] = beFloat(data + floatOff + 4 * k);
        if (std::fabs(v[k]) > (k < 3 ? MAX_ABS_POS : 1.01f)) return false;
    }
    const float n2 = v[3] * v[3] + v[4] * v[4] + v[5] * v[5] + v[6] * v[6];
    if (n2 < 0.5f || n2 > 1.5f) return false;
    const char* name = reinterpret_cast<const char*>(data + argOff);
    bool letters = nameLen > 0 && nameLen <= MAX_BONE_NAME;
    for (size_t i = 0; letters && i < nameLen; ++i)
        letters = (name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z');
    const int b = letters ? slvmc::uboneByName(name, nameLen) : -1;
    if (b < 0)
    {
        ++out.ignored;
        return true;
    }
    out.boneHave[b] = true;
    for (int k = 0; k < 4; ++k) out.boneRot[b][k] = v[3 + k];
    ++out.vmcBones;
    return true;
}

inline bool parseMessage(const uint8_t* data, size_t len, Packet& out)
{
    if (len < 8 || (len & 3) || data[0] != '/') return false;
    size_t addrLen = 0;
    const size_t tagOff = oscString(data, len, 0, addrLen);
    if (!tagOff) return false;
    if (tagOff >= len || data[tagOff] != ',')
    {
        // No type tag string (pre-1.0 OSC): not one of ours.
        ++out.ignored;
        return true;
    }
    size_t tagLen = 0;
    const size_t argOff = oscString(data, len, tagOff, tagLen);
    if (!argOff) return false;
    if (addrLen == 17 && std::memcmp(data, "/VMC/Ext/Bone/Pos", 17) == 0)
        return parseVmcBone(data, len, tagOff, tagLen, argOff, out);
    int slot = -1;
    bool rot = false;
    if (addrLen > MAX_ADDRESS || !trackerAddress(reinterpret_cast<const char*>(data), addrLen, slot, rot))
    {
        ++out.ignored;   // someone else's message (its arguments are not ours to judge)
        return true;
    }
    if (tagLen != 4 || std::memcmp(data + tagOff, ",fff", 4) != 0)
    {
        ++out.ignored;   // our address with other arguments: not SlimeVR's format
        return true;
    }
    if (len - argOff != 12) return false;   // three floats exactly
    float v[3];
    for (int k = 0; k < 3; ++k)
    {
        if (!finiteBits(be32(data + argOff + 4 * k))) return false;
        v[k] = beFloat(data + argOff + 4 * k);
        if (std::fabs(v[k]) > (rot ? MAX_ABS_ANGLE : MAX_ABS_POS)) return false;
    }
    Sample& s = out.t[slot];
    float* dst = rot ? s.euler : s.pos;
    for (int k = 0; k < 3; ++k) dst[k] = v[k];
    (rot ? s.hasRot : s.hasPos) = true;
    ++out.messages;
    return true;
}

inline bool parseElement(const uint8_t* data, size_t len, Packet& out, int depth, uint32_t& count)
{
    if (++count > MAX_MESSAGES) return false;
    if (len >= 16 && std::memcmp(data, "#bundle\0", 8) == 0)
    {
        if (depth >= MAX_BUNDLE_DEPTH || (len & 3)) return false;
        size_t off = 16;   // "#bundle\0" + the 8-byte time tag (ignored: we take the latest)
        while (off < len)
        {
            if (len - off < 4) return false;
            const uint32_t sz = be32(data + off);
            off += 4;
            if (sz == 0 || (sz & 3) || sz > len - off) return false;
            if (!parseElement(data + off, sz, out, depth + 1, count)) return false;
            off += sz;
        }
        return true;
    }
    return parseMessage(data, len, out);
}
} // namespace detail

// Parse one UDP datagram (a message or a bundle). False: malformed; `out` is then left as it was.
inline bool parsePacket(const uint8_t* data, size_t len, Packet& out)
{
    if (!data || len < 8 || (len & 3)) return false;
    Packet tmp;
    uint32_t count = 0;
    if (!detail::parseElement(data, len, tmp, 0, count)) return false;
    out = tmp;
    return true;
}

// ---- Unity (VRChat OSC) -> OpenXR axes --------------------------------------------------------
// SlimeVR's EulerAngles(YXZ).toQuaternion (radians), verbatim: q = qY * qX * qZ.
inline Q eulerYXZ(float x, float y, float z)
{
    const float cX = std::cos(x * 0.5f), cY = std::cos(y * 0.5f), cZ = std::cos(z * 0.5f);
    const float sX = std::sin(x * 0.5f), sY = std::sin(y * 0.5f), sZ = std::sin(z * 0.5f);
    const float w = cX * cY * cZ + sX * sY * sZ;
    const float qx = cY * cZ * sX + cX * sY * sZ;
    const float qy = cX * cZ * sY - cY * sX * sZ;
    const float qz = cX * cY * sZ - cZ * sX * sY;
    return Q(qx, qy, qz, w);
}
inline V3 unityPosToXr(const float p[3]) { return V3(p[0], p[1], -p[2]); }
inline Q unityEulerToXr(const float eDeg[3])
{
    const float d2r = 3.14159265358979f / 180.f;
    const Q u = eulerYXZ(eDeg[0] * d2r, eDeg[1] * d2r, eDeg[2] * d2r);
    return slfbt::qnormalized(Q(-u.x, -u.y, u.z, u.w));
}

// ---- Alignment (OpenXR axes) ------------------------------------------------------------------
inline Q yawQ(float a) { return slfbt::qaxis(V3(0.f, 1.f, 0.f), a); }
// A horizontal direction's angle about +Y (rotating by yawQ(a) adds a).
inline float dirAngle(const V3& v) { return std::atan2(-v.z, v.x); }
// A rotation's heading about +Y (its forward is -Z): yawQ(headingOf(q)) has the same heading.
// Looking (nearly) straight up or down the forward has no horizontal direction: the heading then
// comes from the right axis (+X), which stays level while the head only pitches.
inline float headingOf(const Q& q)
{
    const V3 f = slfbt::qrot(q, V3(0.f, 0.f, -1.f));
    if (f.x * f.x + f.z * f.z >= 0.09f) return std::atan2(-f.x, -f.z);
    return dirAngle(slfbt::qrot(q, V3(1.f, 0.f, 0.f)));
}
inline float wrapAngle(float a)
{
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

struct Alignment
{
    bool valid = false;
    float yaw = 0.f;       // SlimeVR space -> app space, about +Y
    V3 headOffset;         // the headset from SlimeVR's mapped head point, in the headset's yaw frame
};

// The T-pose inputs, in SlimeVR space (already in OpenXR axes): the trackers' positions and which
// are present, SlimeVR's head point (hasHead false: its origin), and the hip's rotation.
struct AlignInput
{
    bool have[T_COUNT] = {};
    V3 pos[T_COUNT];
    Q rot[T_COUNT];
};
enum class YawFrom { NONE, PAIRS, HIP, CHEST };
inline const char* yawFromName(YawFrom f)
{
    switch (f)
    {
    case YawFrom::PAIRS: return "left / right tracker pairs";
    case YawFrom::HIP:   return "the hip tracker's heading";
    case YawFrom::CHEST: return "the chest tracker's heading";
    default:             return "nothing";
    }
}

// SlimeVR's "left" across the body (feet, knees, elbows: left minus right), horizontal.
inline bool slimeLeft(const AlignInput& in, V3& left)
{
    static const int kPairs[3][2] = {{T_LFOOT, T_RFOOT}, {T_LKNEE, T_RKNEE}, {T_LELBOW, T_RELBOW}};
    static const float kWeight[3] = {1.f, 1.f, 2.f};   // elbows: arms out in the T-pose, far apart
    V3 sum;
    for (int i = 0; i < 3; ++i)
    {
        const int l = kPairs[i][0], r = kPairs[i][1];
        if (!in.have[l] || !in.have[r]) continue;
        V3 d = in.pos[l] - in.pos[r];
        d.y = 0.f;
        if (slfbt::length(d) < 0.05f) continue;   // (on top of each other: no direction)
        sum = sum + slfbt::normalized(d) * kWeight[i];
    }
    if (slfbt::length(sum) < 0.5f) return false;
    left = slfbt::normalized(sum);
    return true;
}

// Solve the alignment at the T-pose (standing straight, facing where the headset faces).
// hmdPos / hmdRot: the headset (app space); floorKnown / floorY: the floor's app-space y (unknown
// in a LOCAL space: the feet then keep SlimeVR's own head -> feet height); ankleH: the ankle height
// the foot trackers stand for; eyeForward: how far the eyes sit in front of the spine.
inline Alignment solveAlignment(const AlignInput& in, const V3& hmdPos, const Q& hmdRot, bool floorKnown, float floorY,
                                YawFrom& from,
                                float ankleH = 0.08f, float eyeForward = 0.08f)
{
    Alignment a;
    from = YawFrom::NONE;
    const float hmdYaw = headingOf(hmdRot);
    V3 left;
    if (slimeLeft(in, left))
    {
        // The headset's left: yawQ(hmdYaw) * (-1, 0, 0).
        const V3 xrLeft = slfbt::qrot(yawQ(hmdYaw), V3(-1.f, 0.f, 0.f));
        a.yaw = wrapAngle(dirAngle(xrLeft) - dirAngle(left));
        from = YawFrom::PAIRS;
    }
    else if (in.have[T_HIP] || in.have[T_CHEST])
    {
        const int s = in.have[T_HIP] ? T_HIP : T_CHEST;
        a.yaw = wrapAngle(hmdYaw - headingOf(in.rot[s]));
        from = s == T_HIP ? YawFrom::HIP : YawFrom::CHEST;
    }
    else
    {
        return a;
    }
    // The point that goes under the head: the feet's midpoint, else one foot, else the hip.
    V3 low;
    bool feet = true;
    if (in.have[T_LFOOT] && in.have[T_RFOOT]) low = (in.pos[T_LFOOT] + in.pos[T_RFOOT]) * 0.5f;
    else if (in.have[T_LFOOT]) low = in.pos[T_LFOOT];
    else if (in.have[T_RFOOT]) low = in.pos[T_RFOOT];
    else if (in.have[T_HIP]) { low = in.pos[T_HIP]; feet = false; }
    else return a;
    const V3 head = in.have[T_HEAD] ? in.pos[T_HEAD] : V3();
    const Q R = yawQ(a.yaw);
    const V3 lowRel = slfbt::qrot(R, low - head);   // the low point from the mapped head
    // Where it goes: under the spine (the eyes eyeForward in front of it); at ankle height over
    // the floor (feet), else at SlimeVR's own height below the headset.
    const V3 fwd = slfbt::qrot(yawQ(hmdYaw), V3(0.f, 0.f, -1.f));
    V3 target = hmdPos - fwd * eyeForward;
    target.y = (feet && floorKnown) ? floorY + ankleH : hmdPos.y + lowRel.y;
    const V3 mappedHead = target - lowRel;
    a.headOffset = slfbt::qrot(yawQ(-hmdYaw), hmdPos - mappedHead);
    a.valid = slfbt::finite(a.headOffset) && std::isfinite(a.yaw);
    return a;
}

// Re-solve only the yaw from a SlimeVR reset: SlimeVR sent the heading it reset to (its head
// rotation message) while you faced where the headset faces.
inline void realignYaw(Alignment& a, const Q& slimeResetHeading, const Q& hmdRot)
{
    a.yaw = wrapAngle(headingOf(hmdRot) - headingOf(slimeResetHeading));
}

// A SlimeVR pose (OpenXR axes, SlimeVR space) in app space for this frame's headset.
inline void applyAlignment(const Alignment& a, const V3& slimeHead, const V3& hmdPos, const Q& hmdRot, const V3& p,
                           const Q& q, V3& outP, Q& outQ)
{
    const Q R = yawQ(a.yaw);
    const V3 mappedHead = hmdPos - slfbt::qrot(yawQ(headingOf(hmdRot)), a.headOffset);
    outP = mappedHead + slfbt::qrot(R, p - slimeHead);
    outQ = slfbt::qnormalized(slfbt::qmul(R, q));
}
} // namespace slslime
