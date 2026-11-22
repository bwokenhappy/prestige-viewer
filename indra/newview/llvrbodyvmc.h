/**
 * @file llvrbodyvmc.h
 * @brief #1582 SlimeVR's whole solved skeleton over the VMC protocol: the pure part (no viewer
 *        types). Unity humanoid bones, their hierarchy, the axis conversion, and the mapping onto
 *        the Second Life skeleton (with the spine spread over the Bento spine joints). Shared by
 *        LLVRBodySlime / LLVRBody and scripts/check/test_vr_slime.cpp.
 *
 * What SlimeVR server sends (Settings > OSC > VMC; VMCHandler.kt / UnityArmature.kt), one bundle
 * every ~3 ms to 127.0.0.1:39539 by default:
 *   /VMC/Ext/Bone/Pos  s f f f f f f f   Unity humanoid bone name, local position (x y z), local
 *                                         rotation (x y z w), Unity axes (left-handed, Y up, +Z
 *                                         forward): SlimeVR flips z (position) and z, w (rotation).
 *   /VMC/Ext/Root/Pos  "root", zero position, identity rotation (carries nothing).
 *   /VMC/Ext/T, /VMC/Ext/OK, /VMC/Ext/Tra|Hmd|Con/Pos   (not used here).
 * The rotation is LOCAL: the bone's global rotation relative to its parent bone's in the Unity
 * humanoid hierarchy below (Hips: global). Globals are relative to the T-pose (Unity's humanoid
 * rest: every global rotation identity, standing in a T-pose facing forward). Arm bones are left
 * out while SlimeVR tracks the arms from controllers; finger bones are sent only with finger
 * trackers; toes, eyes and jaw never. The local POSITIONS come from a VRM model SlimeVR may be
 * given (zero without one): unused -- the rotations drive the avatar's own bone lengths.
 */
#pragma once

#include "llvrbodymath.h"

#include <cstring>

namespace slvmc
{
using slfbt::Q;
using slfbt::V3;

// ---- Unity humanoid bones (the ones SlimeVR can send) ------------------------------------------
enum UBone : int
{
    U_HIPS = 0,
    U_SPINE,
    U_CHEST,
    U_UPPER_CHEST,
    U_NECK,
    U_HEAD,
    U_L_UPPER_LEG,
    U_L_LOWER_LEG,
    U_L_FOOT,
    U_L_TOES,
    U_R_UPPER_LEG,
    U_R_LOWER_LEG,
    U_R_FOOT,
    U_R_TOES,
    U_L_SHOULDER,
    U_L_UPPER_ARM,
    U_L_LOWER_ARM,
    U_L_HAND,
    U_R_SHOULDER,
    U_R_UPPER_ARM,
    U_R_LOWER_ARM,
    U_R_HAND,
    U_L_FINGER0,                       // 15 left finger bones: thumb, index, middle, ring, little x 3
    U_R_FINGER0 = U_L_FINGER0 + 15,    // 15 right
    U_COUNT = U_R_FINGER0 + 15         // 52
};

struct UBoneInfo
{
    const char* name;   // Unity HumanBodyBones name (VMC's)
    int parent;         // -1: Hips (global)
};
inline const UBoneInfo& uboneInfo(int b)
{
    static const UBoneInfo k[U_COUNT] = {
        {"Hips", -1},
        {"Spine", U_HIPS},
        {"Chest", U_SPINE},
        {"UpperChest", U_CHEST},
        {"Neck", U_UPPER_CHEST},
        {"Head", U_NECK},
        {"LeftUpperLeg", U_HIPS},
        {"LeftLowerLeg", U_L_UPPER_LEG},
        {"LeftFoot", U_L_LOWER_LEG},
        {"LeftToes", U_L_FOOT},
        {"RightUpperLeg", U_HIPS},
        {"RightLowerLeg", U_R_UPPER_LEG},
        {"RightFoot", U_R_LOWER_LEG},
        {"RightToes", U_R_FOOT},
        {"LeftShoulder", U_UPPER_CHEST},
        {"LeftUpperArm", U_L_SHOULDER},
        {"LeftLowerArm", U_L_UPPER_ARM},
        {"LeftHand", U_L_LOWER_ARM},
        {"RightShoulder", U_UPPER_CHEST},
        {"RightUpperArm", U_R_SHOULDER},
        {"RightLowerArm", U_R_UPPER_ARM},
        {"RightHand", U_R_LOWER_ARM},
        {"LeftThumbProximal", U_L_HAND},
        {"LeftThumbIntermediate", U_L_FINGER0 + 0},
        {"LeftThumbDistal", U_L_FINGER0 + 1},
        {"LeftIndexProximal", U_L_HAND},
        {"LeftIndexIntermediate", U_L_FINGER0 + 3},
        {"LeftIndexDistal", U_L_FINGER0 + 4},
        {"LeftMiddleProximal", U_L_HAND},
        {"LeftMiddleIntermediate", U_L_FINGER0 + 6},
        {"LeftMiddleDistal", U_L_FINGER0 + 7},
        {"LeftRingProximal", U_L_HAND},
        {"LeftRingIntermediate", U_L_FINGER0 + 9},
        {"LeftRingDistal", U_L_FINGER0 + 10},
        {"LeftLittleProximal", U_L_HAND},
        {"LeftLittleIntermediate", U_L_FINGER0 + 12},
        {"LeftLittleDistal", U_L_FINGER0 + 13},
        {"RightThumbProximal", U_R_HAND},
        {"RightThumbIntermediate", U_R_FINGER0 + 0},
        {"RightThumbDistal", U_R_FINGER0 + 1},
        {"RightIndexProximal", U_R_HAND},
        {"RightIndexIntermediate", U_R_FINGER0 + 3},
        {"RightIndexDistal", U_R_FINGER0 + 4},
        {"RightMiddleProximal", U_R_HAND},
        {"RightMiddleIntermediate", U_R_FINGER0 + 6},
        {"RightMiddleDistal", U_R_FINGER0 + 7},
        {"RightRingProximal", U_R_HAND},
        {"RightRingIntermediate", U_R_FINGER0 + 9},
        {"RightRingDistal", U_R_FINGER0 + 10},
        {"RightLittleProximal", U_R_HAND},
        {"RightLittleIntermediate", U_R_FINGER0 + 12},
        {"RightLittleDistal", U_R_FINGER0 + 13},
    };
    return k[(b >= 0 && b < U_COUNT) ? b : 0];
}
// A VMC bone name -> UBone, or -1 (unknown: eyes, jaw, a typo). n: the name's length.
inline int uboneByName(const char* s, size_t n)
{
    for (int b = 0; b < U_COUNT; ++b)
    {
        const char* k = uboneInfo(b).name;
        if (std::strlen(k) == n && std::memcmp(k, s, n) == 0) return b;
    }
    return -1;
}

// ---- Axes ------------------------------------------------------------------------------------
// A VMC (Unity) rotation in OpenXR axes (right-handed, Y up, -Z forward): undo SlimeVR's flip
// (it sends (x, y, -z, -w) of its right-handed rotation; -q is the same rotation).
inline Q unityRotToXr(const float u[4]) { return slfbt::qnormalized(Q(-u[0], -u[1], u[2], u[3])); }

// The globals (OpenXR axes, SlimeVR's space; T-pose = identity) from the locals: Hips is global,
// every other bone its parent's global times its local. have[b] false: not sent (its children,
// sent or not, are dropped too: their parent's rotation is unknown).
inline void composeGlobals(const bool haveLocal[U_COUNT], const Q local[U_COUNT], bool have[U_COUNT], Q global[U_COUNT])
{
    // (The table lists every parent before its children.)
    for (int b = 0; b < U_COUNT; ++b)
    {
        const int p = uboneInfo(b).parent;
        have[b] = haveLocal[b] && (p < 0 || have[p]);
        global[b] = have[b] ? (p < 0 ? local[b] : slfbt::qnormalized(slfbt::qmul(global[p], local[b]))) : Q();
    }
}

// ---- The Second Life skeleton ----------------------------------------------------------------
// The SL joints a VMC skeleton drives (mNeck / mHead stay with the headset: #1121 head follow).
enum SJoint : int
{
    S_PELVIS = 0,
    S_SPINE1,
    S_SPINE2,
    S_TORSO,
    S_SPINE3,
    S_SPINE4,
    S_CHEST,
    S_NECK,
    S_HEAD,
    S_L_COLLAR,
    S_L_SHOULDER,
    S_L_ELBOW,
    S_L_WRIST,
    S_R_COLLAR,
    S_R_SHOULDER,
    S_R_ELBOW,
    S_R_WRIST,
    S_L_HIP,
    S_L_KNEE,
    S_L_ANKLE,
    S_L_FOOT,
    S_R_HIP,
    S_R_KNEE,
    S_R_ANKLE,
    S_R_FOOT,
    S_L_FINGER0,                         // 15: thumb, index, middle, ring, pinky x 1..3
    S_R_FINGER0 = S_L_FINGER0 + 15,
    S_COUNT = S_R_FINGER0 + 15           // 55
};
struct SJointInfo
{
    const char* name;   // the LLJoint name
    int parent;         // -1: the avatar's root (mRoot)
};
inline const SJointInfo& sjointInfo(int j)
{
    static const SJointInfo k[S_COUNT] = {
        {"mPelvis", -1},
        {"mSpine1", S_PELVIS},
        {"mSpine2", S_SPINE1},
        {"mTorso", S_SPINE2},
        {"mSpine3", S_TORSO},
        {"mSpine4", S_SPINE3},
        {"mChest", S_SPINE4},
        {"mNeck", S_CHEST},
        {"mHead", S_NECK},
        {"mCollarLeft", S_CHEST},
        {"mShoulderLeft", S_L_COLLAR},
        {"mElbowLeft", S_L_SHOULDER},
        {"mWristLeft", S_L_ELBOW},
        {"mCollarRight", S_CHEST},
        {"mShoulderRight", S_R_COLLAR},
        {"mElbowRight", S_R_SHOULDER},
        {"mWristRight", S_R_ELBOW},
        {"mHipLeft", S_PELVIS},
        {"mKneeLeft", S_L_HIP},
        {"mAnkleLeft", S_L_KNEE},
        {"mFootLeft", S_L_ANKLE},
        {"mHipRight", S_PELVIS},
        {"mKneeRight", S_R_HIP},
        {"mAnkleRight", S_R_KNEE},
        {"mFootRight", S_R_ANKLE},
        {"mHandThumb1Left", S_L_WRIST},
        {"mHandThumb2Left", S_L_FINGER0 + 0},
        {"mHandThumb3Left", S_L_FINGER0 + 1},
        {"mHandIndex1Left", S_L_WRIST},
        {"mHandIndex2Left", S_L_FINGER0 + 3},
        {"mHandIndex3Left", S_L_FINGER0 + 4},
        {"mHandMiddle1Left", S_L_WRIST},
        {"mHandMiddle2Left", S_L_FINGER0 + 6},
        {"mHandMiddle3Left", S_L_FINGER0 + 7},
        {"mHandRing1Left", S_L_WRIST},
        {"mHandRing2Left", S_L_FINGER0 + 9},
        {"mHandRing3Left", S_L_FINGER0 + 10},
        {"mHandPinky1Left", S_L_WRIST},
        {"mHandPinky2Left", S_L_FINGER0 + 12},
        {"mHandPinky3Left", S_L_FINGER0 + 13},
        {"mHandThumb1Right", S_R_WRIST},
        {"mHandThumb2Right", S_R_FINGER0 + 0},
        {"mHandThumb3Right", S_R_FINGER0 + 1},
        {"mHandIndex1Right", S_R_WRIST},
        {"mHandIndex2Right", S_R_FINGER0 + 3},
        {"mHandIndex3Right", S_R_FINGER0 + 4},
        {"mHandMiddle1Right", S_R_WRIST},
        {"mHandMiddle2Right", S_R_FINGER0 + 6},
        {"mHandMiddle3Right", S_R_FINGER0 + 7},
        {"mHandRing1Right", S_R_WRIST},
        {"mHandRing2Right", S_R_FINGER0 + 9},
        {"mHandRing3Right", S_R_FINGER0 + 10},
        {"mHandPinky1Right", S_R_WRIST},
        {"mHandPinky2Right", S_R_FINGER0 + 12},
        {"mHandPinky3Right", S_R_FINGER0 + 13},
    };
    return k[(j >= 0 && j < S_COUNT) ? j : 0];
}
// Which joints belong to an arm (the controller / #1584 arm IK may own them), a finger, a leg.
inline bool isArmJoint(int j) { return j >= S_L_COLLAR && j <= S_R_WRIST; }
inline bool isFingerJoint(int j) { return j >= S_L_FINGER0 && j < S_COUNT; }
inline bool isLeftSide(int j)
{
    return (j >= S_L_COLLAR && j <= S_L_WRIST) || (j >= S_L_HIP && j <= S_L_FOOT) || (j >= S_L_FINGER0 && j < S_R_FINGER0);
}

// The SL skeleton's world rotations (SL body axes: X forward, Y left, Z up; T-pose = identity, as
// SL's rest pose) from the Unity globals in OpenXR axes. The spine: SL has two real segments
// (pelvis -> torso, torso -> chest), each through a folded Bento pair (mSpine1/2, mSpine3/4: the
// pair's second joint orients the segment). Unity has Spine, Chest, UpperChest. So:
//   mPelvis = Hips, mSpine1 = Hips, mSpine2 = halfway Hips -> Spine (the lower back curves),
//   mTorso = Spine, mSpine3 = Spine, mSpine4 = Chest (orients torso -> chest), mChest = UpperChest
//   (or Chest), mNeck = Neck, mHead = Head (each the parent's when not sent).
// Limbs: Shoulder / UpperArm / LowerArm / Hand -> mCollar / mShoulder / mElbow / mWrist; UpperLeg
// / LowerLeg / Foot / Toes -> mHip / mKnee / mAnkle / mFoot (Toes never sent: the ankle's); fingers
// Proximal / Intermediate / Distal -> 1 / 2 / 3. A limb whose root bone was not sent is not driven
// (have false through its chain); a missing inner bone keeps its parent's world rotation.
inline void toSecondLife(const bool have[U_COUNT], const Q globalXr[U_COUNT], bool out[S_COUNT], Q world[S_COUNT])
{
    Q g[U_COUNT];
    for (int b = 0; b < U_COUNT; ++b)
    {
        float q[4] = {globalXr[b].x, globalXr[b].y, globalXr[b].z, globalXr[b].w};
        g[b] = slfbt::xrRotToBody(q);
    }
    for (int j = 0; j < S_COUNT; ++j)
    {
        out[j] = false;
        world[j] = Q();
    }
    if (!have[U_HIPS]) return;
    auto pick = [&](int j, int b, int fallbackJ) {
        // b sent: its rotation; else the fallback joint's (already set: the parent).
        if (b >= 0 && have[b])
        {
            out[j] = true;
            world[j] = g[b];
        }
        else if (fallbackJ >= 0 && out[fallbackJ])
        {
            out[j] = true;
            world[j] = world[fallbackJ];
        }
    };
    // Spine.
    pick(S_PELVIS, U_HIPS, -1);
    pick(S_SPINE1, U_HIPS, -1);
    pick(S_TORSO, U_SPINE, S_PELVIS);
    out[S_SPINE2] = true;
    world[S_SPINE2] = slfbt::qslerp(world[S_PELVIS], world[S_TORSO], 0.5f);
    pick(S_SPINE3, U_SPINE, S_TORSO);
    pick(S_SPINE4, U_CHEST, S_SPINE3);
    pick(S_CHEST, have[U_UPPER_CHEST] ? U_UPPER_CHEST : U_CHEST, S_SPINE4);
    pick(S_NECK, U_NECK, S_CHEST);
    pick(S_HEAD, U_HEAD, S_NECK);
    // Limbs: driven only from a sent root bone.
    struct Chain { int ub[4]; int sj[4]; };
    static const Chain kChains[4] = {
        {{U_L_SHOULDER, U_L_UPPER_ARM, U_L_LOWER_ARM, U_L_HAND}, {S_L_COLLAR, S_L_SHOULDER, S_L_ELBOW, S_L_WRIST}},
        {{U_R_SHOULDER, U_R_UPPER_ARM, U_R_LOWER_ARM, U_R_HAND}, {S_R_COLLAR, S_R_SHOULDER, S_R_ELBOW, S_R_WRIST}},
        {{U_L_UPPER_LEG, U_L_LOWER_LEG, U_L_FOOT, U_L_TOES}, {S_L_HIP, S_L_KNEE, S_L_ANKLE, S_L_FOOT}},
        {{U_R_UPPER_LEG, U_R_LOWER_LEG, U_R_FOOT, U_R_TOES}, {S_R_HIP, S_R_KNEE, S_R_ANKLE, S_R_FOOT}},
    };
    for (const Chain& c : kChains)
    {
        // An arm may start at the shoulder (collar) or, without it, at the upper arm (the collar
        // then keeps the chest's rotation).
        const bool arm = c.sj[0] == S_L_COLLAR || c.sj[0] == S_R_COLLAR;
        if (!have[c.ub[0]] && !(arm && have[c.ub[1]])) continue;
        pick(c.sj[0], c.ub[0], S_CHEST);
        for (int k = 1; k < 4; ++k) pick(c.sj[k], c.ub[k], c.sj[k - 1]);
    }
    // Fingers: each finger from its proximal bone.
    for (int side = 0; side < 2; ++side)
    {
        const int ub0 = side ? U_R_FINGER0 : U_L_FINGER0, sj0 = side ? S_R_FINGER0 : S_L_FINGER0;
        const int wrist = side ? S_R_WRIST : S_L_WRIST;
        if (!out[wrist]) continue;
        for (int f = 0; f < 5; ++f)
        {
            if (!have[ub0 + 3 * f]) continue;
            for (int k = 0; k < 3; ++k) pick(sj0 + 3 * f + k, ub0 + 3 * f + k, k ? sj0 + 3 * f + k - 1 : -1);
        }
    }
}

// A joint's local rotation from the world rotations (parent -1: rootWorld, the avatar's root).
inline Q localOf(int j, const Q world[S_COUNT], const Q& rootWorld)
{
    const int p = sjointInfo(j).parent;
    const Q& pw = p < 0 ? rootWorld : world[p];
    return slfbt::qnormalized(slfbt::qmul(slfbt::qconj(pw), world[j]));
}

// The heading the skeleton faces (its hips' forward, SL body axes).
inline float facing(const Q world[S_COUNT]) { return slfbt::yawOf(world[S_PELVIS]); }
} // namespace slvmc
