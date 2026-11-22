/**
 * @file llheadlessbody.h
 * @brief #1585 HEADLESS SELF (VR "Show my body in VR", SlipstreamVRShowBody): which avatar joints
 *        count as the HEAD, and which of a skinned vertex's weight is on them. Header only, no
 *        viewer state: LLCleanSkinStore (the per-skin record the rigged VS reads), the worn
 *        attachment tagging (llvovolume.cpp) and the unit test (tests/llheadlessbody_test.cpp)
 *        use it as is. The GPU side of the rule is clean_headless_self.glsl.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 *
 * THE HEAD SET. A joint is head-side when mHead is the joint itself or one of its ancestors in
 * the avatar's skeleton hierarchy (LLJoint::getParent), over the avatar joint numbers the skin
 * meta and the joint table use (bones, then collision volumes, then attachment points:
 * LLVOAvatar::getJoint(S32)). So it covers mHead, mSkull, mEye*, mFaceRoot and every Bento face
 * bone (ears, nose, jaw, tongue, lips, ...), the HEAD collision volume, and the head attachment
 * points (Skull, Mouth, Chin, Ears, Nose, Eyeballs, Jaw, Alt Ears / Eyes, Tongue) -- from the
 * hierarchy, not a name list, so a rig that weights its head parts to any bone under mHead (an
 * Open Pony's ears and nose on the face bones) is covered without naming it.
 * mNeck is NOT head-side (nor the Neck attachment point): a typical neck blends mNeck -> mHead,
 * and the 0.5 cut below then lands where the head bone dominates, just under the skull, as
 * VRChat's head-bone shrink does. Counting mNeck would take the whole neck (and a mane or long
 * hair weighted to it) off the body.
 *
 * THE VERTEX RULE. A skinned vertex is a head vertex when its summed weight on head-side joints
 * is at least one half: in the exact unorm16 terms of the encoded weights (llcleanqvertex.h
 * QWeight: the four weights sum to exactly 65535), 2 * head >= 65535. The rigged VS collapses
 * such a vertex onto the mHead joint in the MAIN view only (clean_draw_rigged.vert); mirrors,
 * probes, shadows and RT draw the full avatar.
 *
 * THE PER-SKIN RECORD (LLCleanSkinStore::SkinMeta's spare words): bit k set = the skin's joint k
 * (an index of its joint_nums, < 110) is head-side. Words: head_lo[0] bits 0..31, head_lo[1]
 * 32..63, head_hi[0] 64..95, head_hi[1] bits 0..13 = 96..109, and head_hi[1] bits 16..31 = the
 * mHead joint number + 1 (the collapse target; 0 = none). A skin with no head-side joint has all
 * four words 0: the VS skips it after one read.
 */

#pragma once

#include "stdtypes.h"

namespace LLHeadlessBody
{
constexpr U32 kMaxJoints  = 216;   // = LLCleanSkinStore::MAX_AVATAR_JOINTS (the joint table's row)
constexpr U32 kSetWords   = (kMaxJoints + 31) / 32;
constexpr U32 kSkinJoints = 110;   // = LLCleanSkinStore::MAX_JOINTS (a skin's joint_nums)
constexpr U32 kWeightOne  = 65535; // = LLCleanQV::kWeightOne
constexpr U32 kMaskHiBits = 0x3FFFu;   // head_hi[1]: skin joints 96..109
constexpr U32 kHeadShift  = 16;        // head_hi[1]: the mHead joint number + 1

// The head-side joints of one skeleton (joint numbers < kMaxJoints).
struct HeadSet
{
    U32 bits[kSetWords] = {};
    S32 head = -1;   // mHead's joint number (-1: not found -- nothing is head-side)
    bool valid() const { return head >= 0; }
    bool has(S32 j) const
    {
        return j >= 0 && (U32)j < kMaxJoints && ((bits[(U32)j >> 5] >> ((U32)j & 31u)) & 1u) != 0u;
    }
};

// parent[j] = the joint number of joint j's parent (-1: a root, or no joint j), for j < count.
// head = mHead's joint number. Each joint walks its parent chain (bounded: a malformed cycle
// stops after count steps and counts as not head-side).
inline HeadSet buildHeadSet(const S32* parent, U32 count, S32 head)
{
    HeadSet s;
    if (!parent || head < 0 || (U32)head >= count || (U32)head >= kMaxJoints) return s;
    s.head = head;
    if (count > kMaxJoints) count = kMaxJoints;
    for (U32 j = 0; j < count; ++j)
    {
        S32 k = (S32)j;
        for (U32 steps = 0; k >= 0 && steps <= count; ++steps)
        {
            if (k == head)
            {
                s.bits[j >> 5] |= 1u << (j & 31u);
                break;
            }
            k = (U32)k < count ? parent[k] : -1;
        }
    }
    return s;
}

// The four SkinMeta words of a skin (see the header). joint_nums: the skin's avatar joint numbers.
struct SkinWords
{
    U32 lo[2] = {0u, 0u};
    U32 hi[2] = {0u, 0u};
    bool any() const { return (lo[0] | lo[1] | hi[0] | (hi[1] & kMaskHiBits)) != 0u; }
    bool jointIsHead(U32 k) const
    {
        if (k >= kSkinJoints) return false;
        const U32 w = k < 64u ? lo[k >> 5] : (k < 96u ? hi[0] : (hi[1] & kMaskHiBits));
        return ((w >> (k & 31u)) & 1u) != 0u;
    }
    S32 headJoint() const { return (S32)(hi[1] >> kHeadShift) - 1; }
};

inline SkinWords skinWords(const S32* joint_nums, U32 joint_count, const HeadSet& s)
{
    SkinWords w;
    if (!joint_nums || !s.valid()) return w;
    if (joint_count > kSkinJoints) joint_count = kSkinJoints;
    for (U32 k = 0; k < joint_count; ++k)
    {
        if (!s.has(joint_nums[k])) continue;
        if (k < 64u)      w.lo[k >> 5] |= 1u << (k & 31u);
        else if (k < 96u) w.hi[0] |= 1u << (k - 64u);
        else              w.hi[1] |= 1u << (k - 96u);
    }
    if (w.any()) w.hi[1] |= (U32)(s.head + 1) << kHeadShift;
    return w;
}

// One vertex's weight on head-side joints, in unorm16 (0..65535). joint[4]: its influences' skin
// joint indices (as decoded: the pre-skin clamps them to kSkinJoints - 1); q[4]: their unorm16
// weights, summing to kWeightOne (the encoder's guarantee; influence 3's is implied).
inline U32 headWeight(const U32 joint[4], const U32 q[4], const SkinWords& w)
{
    U32 h = 0;
    for (int i = 0; i < 4; ++i)
    {
        const U32 j = joint[i] < kSkinJoints ? joint[i] : kSkinJoints - 1u;
        if (w.jointIsHead(j)) h += q[i];
    }
    return h;
}

inline bool headVertex(const U32 joint[4], const U32 q[4], const SkinWords& w)
{
    return 2u * headWeight(joint, q, w) >= kWeightOne;
}
}
