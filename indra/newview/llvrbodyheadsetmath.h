/**
 * @file llvrbodyheadsetmath.h
 * @brief #1583 VR headset body tracking: the runtime's skeleton joints -> LLVRBody's roles (pure
 *        math, no viewer or OpenXR types), shared by LLVRBodyHeadset and the fixtures in
 *        scripts/check/test_vr_body_headset.cpp (checks/vr_body_headset.py).
 *
 * Joint sets (the joint indices are the OpenXR enums' values):
 *  - JS_FB_UPPER  XR_FB_body_tracking, XR_BODY_JOINT_SET_DEFAULT_FB (70 joints: upper body only;
 *                 the legs stay on the animation).
 *  - JS_FB_FULL   XR_META_body_tracking_full_body, XR_BODY_JOINT_SET_FULL_BODY_META (84: the 70
 *                 plus the legs).
 *  - JS_HTC       XR_HTC_body_tracking, XR_BODY_JOINT_SET_FULL_HTC (26).
 *  - JS_BD        XR_BD_body_tracking, XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD (24).
 *
 * Only joint POSITIONS are used. Each vendor gives its joints its own local axes (none of them
 * is SL's "bone along its child, front +X"), so a role's rotation is built from where the joints
 * are: the hips from the hip line (left upper leg - right upper leg) and the spine, the chest
 * from the shoulder line and the neck, a foot's heading from the ankle -> ball of the foot. The
 * roles are then the bones themselves: LLVRBody needs no T-pose offsets for them.
 *
 * Feet turn by heading only (yaw): the ankle -> ball line points ~25-30 degrees down on a flat
 * foot, which taken as the bone's front would tip every standing foot toes-down.
 */
#pragma once

#include "llvrbodymath.h"

namespace slfbt
{
enum JointSet : int { JS_NONE = 0, JS_FB_UPPER, JS_FB_FULL, JS_HTC, JS_BD, JS_COUNT };
constexpr int kMaxBodyJoints = 84;   // the largest set (FULL_BODY_META)

inline const char* jointSetName(int s)
{
    static const char* const k[JS_COUNT] = {"none", "FB upper body (DEFAULT, 70 joints)",
                                            "Meta full body (FULL_BODY_META, 84 joints)", "HTC full body (26 joints)",
                                            "BD full body (24 joints)"};
    return (s >= 0 && s < JS_COUNT) ? k[s] : "?";
}
inline int jointSetCount(int s)
{
    switch (s)
    {
    case JS_FB_UPPER: return 70;
    case JS_FB_FULL: return 84;
    case JS_HTC: return 26;
    case JS_BD: return 24;
    default: return 0;
    }
}

// The joints a set's roles are built from (-1: the set has none).
struct JointMap
{
    int hips, spine, chest, neck;     // spine: the first spine joint above the hips
    int shoulder[2];                  // left, right: the upper arms' roots (the chest's left-right line)
    int hip[2], knee[2], ankle[2], toe[2];
};
inline const JointMap& jointMap(int s)
{
    // XR_FB_body_tracking / XR_META_body_tracking_full_body: HIPS 1, SPINE_LOWER 2, CHEST 5,
    // NECK 6, LEFT / RIGHT_ARM_UPPER 10 / 15; legs (full body only): UPPER_LEG 70 / 77 (the hip
    // joint), LOWER_LEG 71 / 78 (the knee), FOOT_ANKLE 73 / 80, FOOT_BALL 76 / 83.
    static const JointMap fbUpper = {1, 2, 5, 6, {10, 15}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}};
    static const JointMap fbFull = {1, 2, 5, 6, {10, 15}, {70, 77}, {71, 78}, {73, 80}, {76, 83}};
    // XR_HTC_body_tracking: PELVIS 0, LEFT_HIP 1, LEFT_KNEE 2, LEFT_ANKLE 3, LEFT_FEET 4,
    // RIGHT_HIP 5 .. RIGHT_FEET 8, SPINE_LOWER 10, CHEST 13, NECK 14, LEFT_ARM 18, RIGHT_ARM 23.
    static const JointMap htc = {0, 10, 13, 14, {18, 23}, {1, 5}, {2, 6}, {3, 7}, {4, 8}};
    // XR_BD_body_tracking: PELVIS 0, LEFT / RIGHT_HIP 1 / 2, SPINE1 3, LEFT / RIGHT_KNEE 4 / 5,
    // LEFT / RIGHT_ANKLE 7 / 8, SPINE3 9 (the chest), LEFT / RIGHT_FOOT 10 / 11, NECK 12,
    // LEFT / RIGHT_SHOULDER 16 / 17.
    static const JointMap bd = {0, 3, 9, 12, {16, 17}, {1, 2}, {4, 5}, {7, 8}, {10, 11}};
    static const JointMap none = {-1, -1, -1, -1, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}};
    switch (s)
    {
    case JS_FB_UPPER: return fbUpper;
    case JS_FB_FULL: return fbFull;
    case JS_HTC: return htc;
    case JS_BD: return bd;
    default: return none;
    }
}
// The set has legs (feet / knees roles).
inline bool jointSetHasLegs(int s) { return jointMap(s).ankle[0] >= 0; }
// #1584: a set's elbow joint (side 0 left, 1 right; -1 none): the arms' elbow poles.
// XR_FB_body_tracking LEFT / RIGHT_ARM_LOWER 11 / 16; XR_HTC_body_tracking LEFT / RIGHT_ELBOW
// 19 / 24; XR_BD_body_tracking LEFT / RIGHT_ELBOW 18 / 19.
inline int elbowJoint(int s, int side)
{
    switch (s)
    {
    case JS_FB_UPPER:
    case JS_FB_FULL: return side ? 16 : 11;
    case JS_HTC: return side ? 24 : 19;
    case JS_BD: return side ? 19 : 18;
    default: return -1;
    }
}

// One located joint, body space (SL axes; any floor offset: only differences are used).
struct BodyJoint
{
    bool valid = false;     // position valid this frame
    bool tracked = false;   // ... and tracked (not inferred / generated)
    V3 pos;
};
// A role built from the joints, body space.
struct RolePose
{
    bool valid = false;
    bool tracked = false;   // every joint it was built from was tracked
    V3 pos;
    Q rot;
};

// The roles (slfbt::Body) from one frame's joints. n: the joints in `j` (the set's count).
// A role needs its own joint and the joints its rotation is built from; a foot without its
// ball, or a knee, falls back to the hips' heading for its rotation (a knee is only a position:
// LLVRBody uses it as the leg's pole).
inline void mapBodyJoints(int set, const BodyJoint* j, int n, RolePose out[B_COUNT])
{
    for (int b = 0; b < B_COUNT; ++b) out[b] = RolePose{};
    const JointMap& m = jointMap(set);
    auto ok = [&](int i) { return i >= 0 && i < n && j[i].valid && finite(j[i].pos); };
    auto trk = [&](int i) { return i >= 0 && i < n && j[i].tracked; };
    const V3 up(0.f, 0.f, 1.f);
    // A frame with `primary` along restA (Z: up the spine) and the left-right line toward +Y.
    auto frame = [&](const V3& along, const V3& left) { return qalignPair(V3(0.f, 0.f, 1.f), V3(0.f, 1.f, 0.f), along, left); };

    // The chest: up toward the neck, left along the shoulders.
    V3 chestLeft;
    bool haveChestLeft = false;
    if (ok(m.shoulder[0]) && ok(m.shoulder[1]))
    {
        chestLeft = j[m.shoulder[0]].pos - j[m.shoulder[1]].pos;
        haveChestLeft = length(chestLeft) > 0.05f;
    }
    if (ok(m.chest) && haveChestLeft)
    {
        RolePose& r = out[B_CHEST];
        const V3 along = ok(m.neck) ? j[m.neck].pos - j[m.chest].pos : up;
        r.valid = true;
        r.pos = j[m.chest].pos;
        r.rot = frame(length(along) > 0.02f ? along : up, chestLeft);
        r.tracked = trk(m.chest) && trk(m.shoulder[0]) && trk(m.shoulder[1]) && (!ok(m.neck) || trk(m.neck));
    }
    // The hips: up toward the spine, left along the hip joints (no legs in the set: the shoulders').
    if (ok(m.hips))
    {
        V3 left;
        bool haveLeft = false, leftTracked = false;
        if (ok(m.hip[0]) && ok(m.hip[1]))
        {
            left = j[m.hip[0]].pos - j[m.hip[1]].pos;
            haveLeft = length(left) > 0.04f;
            leftTracked = trk(m.hip[0]) && trk(m.hip[1]);
        }
        if (!haveLeft && haveChestLeft)
        {
            left = chestLeft;
            haveLeft = true;
            leftTracked = trk(m.shoulder[0]) && trk(m.shoulder[1]);
        }
        if (haveLeft)
        {
            RolePose& r = out[B_WAIST];
            const V3 along = ok(m.spine) ? j[m.spine].pos - j[m.hips].pos : up;
            r.valid = true;
            r.pos = j[m.hips].pos;
            r.rot = frame(length(along) > 0.02f ? along : up, left);
            r.tracked = trk(m.hips) && leftTracked && (!ok(m.spine) || trk(m.spine));
        }
    }
    const float hipsYaw = out[B_WAIST].valid ? yawOf(out[B_WAIST].rot) : (out[B_CHEST].valid ? yawOf(out[B_CHEST].rot) : 0.f);
    const bool haveYaw = out[B_WAIST].valid || out[B_CHEST].valid;
    for (int side = 0; side < 2; ++side)
    {
        const int footB = side ? B_RFOOT : B_LFOOT, kneeB = side ? B_RKNEE : B_LKNEE;
        float footYaw = hipsYaw;
        bool footYawOk = haveYaw;
        bool footYawTracked = false;
        if (ok(m.ankle[side]) && ok(m.toe[side]))
        {
            const V3 f = j[m.toe[side]].pos - j[m.ankle[side]].pos;
            if (std::sqrt(f.x * f.x + f.y * f.y) > 0.03f)
            {
                footYaw = std::atan2(f.y, f.x);
                footYawOk = true;
                footYawTracked = trk(m.toe[side]);
            }
        }
        if (ok(m.ankle[side]) && footYawOk)
        {
            RolePose& r = out[footB];
            r.valid = true;
            r.pos = j[m.ankle[side]].pos;
            r.rot = qyaw(footYaw);
            r.tracked = trk(m.ankle[side]) && footYawTracked;
        }
        if (ok(m.knee[side]))
        {
            RolePose& r = out[kneeB];
            r.valid = true;
            r.pos = j[m.knee[side]].pos;
            r.rot = qyaw(footYawOk ? footYaw : hipsYaw);
            r.tracked = trk(m.knee[side]);
        }
    }
}

// Hold a role briefly through a dropout: a joint flickering invalid for a frame or two would
// otherwise start the 0.3 s fade-out and back (visible pumping). Returns the role to report:
// the sample while valid; the last valid pose (marked untracked) for up to holdSeconds after it
// went invalid; then invalid. `held` / `heldAge` are the caller's per-role state.
inline RolePose holdRole(const RolePose& sample, RolePose& held, float& heldAge, float dt, float holdSeconds = 0.15f)
{
    if (sample.valid)
    {
        held = sample;
        heldAge = 0.f;
        return sample;
    }
    if (!held.valid) return sample;
    heldAge += dt;
    if (heldAge > holdSeconds)
    {
        held.valid = false;
        return sample;
    }
    RolePose r = held;
    r.tracked = false;
    return r;
}

// Smoothing time constant for a role at a confidence (0..1): 1 keeps the base, 0 four times it
// (low-confidence / generated joints are steadied more, never jitter through).
inline float confidenceTau(float tau, float confidence)
{
    const float c = confidence < 0.f ? 0.f : (confidence > 1.f ? 1.f : confidence);
    return tau * (1.f + 3.f * (1.f - c));
}
} // namespace slfbt
