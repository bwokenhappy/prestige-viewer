/**
 * @file llheadlessbody_test.cpp
 * @brief #1585 headless self (llheadlessbody.h): the head set from a bone hierarchy (mHead and
 *        everything under it, Bento face bones and head attachment points included, mNeck not),
 *        the per-skin head words, and the per-vertex 0.5 head-weight rule -- a human mesh head /
 *        hair and Open Pony-style ears, nose and mane.
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
 */

#include "linden_common.h"

#include "../llheadlessbody.h"

#include <vector>

#include "../test/lltut.h"

namespace
{
    using namespace LLHeadlessBody;

    // A cut-down SL skeleton in the viewer's numbering order (bones, collision volumes, then
    // attachment points), each entry's parent by joint number.
    enum J : S32
    {
        PELVIS, TORSO, CHEST, NECK, HEAD, SKULL, EYE_L, EYE_R, FACE_ROOT,
        EAR1_L, EAR2_L, NOSE_C, NOSE_BASE, JAW, CHIN, TONGUE_BASE, TONGUE_TIP, LIP_UP_C, FOREHEAD_C,
        COLLAR_L, SHOULDER_L, ELBOW_L, WRIST_L, HIP_L, KNEE_L, TAIL1, TAIL2,
        CV_HEAD, CV_NECK, CV_CHEST,                       // collision volumes
        AP_SKULL, AP_CHIN, AP_NOSE, AP_L_EAR, AP_TONGUE, AP_JAW, AP_L_EYEBALL, AP_ALT_L_EAR,
        AP_NECK, AP_CHEST, AP_L_HAND, AP_TAIL_TIP,        // attachment points
        COUNT
    };

    std::vector<S32> skeleton()
    {
        std::vector<S32> p(COUNT, -1);
        p[TORSO] = PELVIS; p[CHEST] = TORSO; p[NECK] = CHEST; p[HEAD] = NECK;
        p[SKULL] = HEAD; p[EYE_L] = HEAD; p[EYE_R] = HEAD; p[FACE_ROOT] = HEAD;
        p[EAR1_L] = FACE_ROOT; p[EAR2_L] = EAR1_L; p[NOSE_C] = FACE_ROOT; p[NOSE_BASE] = FACE_ROOT;
        p[JAW] = FACE_ROOT; p[CHIN] = JAW; p[TONGUE_BASE] = JAW; p[TONGUE_TIP] = TONGUE_BASE;
        p[LIP_UP_C] = FACE_ROOT; p[FOREHEAD_C] = FACE_ROOT;
        p[COLLAR_L] = CHEST; p[SHOULDER_L] = COLLAR_L; p[ELBOW_L] = SHOULDER_L; p[WRIST_L] = ELBOW_L;
        p[HIP_L] = PELVIS; p[KNEE_L] = HIP_L; p[TAIL1] = PELVIS; p[TAIL2] = TAIL1;
        p[CV_HEAD] = HEAD; p[CV_NECK] = NECK; p[CV_CHEST] = CHEST;
        p[AP_SKULL] = HEAD; p[AP_CHIN] = HEAD; p[AP_NOSE] = HEAD; p[AP_L_EAR] = HEAD;
        p[AP_TONGUE] = TONGUE_TIP; p[AP_JAW] = JAW; p[AP_L_EYEBALL] = EYE_L; p[AP_ALT_L_EAR] = EAR1_L;
        p[AP_NECK] = NECK; p[AP_CHEST] = CHEST; p[AP_L_HAND] = WRIST_L; p[AP_TAIL_TIP] = TAIL2;
        return p;
    }

    HeadSet headSet()
    {
        const std::vector<S32> p = skeleton();
        return buildHeadSet(p.data(), (U32)p.size(), HEAD);
    }

    // One vertex: up to four (skin joint index, unorm16 weight) influences summing to 65535.
    bool isHead(const SkinWords& w, std::initializer_list<std::pair<U32, U32>> infl)
    {
        U32 j[4] = {0u, 0u, 0u, 0u}, q[4] = {0u, 0u, 0u, 0u};
        int i = 0;
        for (const auto& e : infl)
        {
            j[i] = e.first;
            q[i] = e.second;
            ++i;
        }
        return headVertex(j, q, w);
    }
}

namespace tut
{
    struct headless_data {};
    typedef test_group<headless_data> headless_test;
    typedef headless_test::object headless_object;
    tut::headless_test headless("LLHeadlessBody");

    // The head set: mHead and its whole subtree, by hierarchy.
    template<> template<>
    void headless_object::test<1>()
    {
        const HeadSet s = headSet();
        ensure("valid", s.valid());
        for (S32 j : {HEAD, SKULL, EYE_L, EYE_R, FACE_ROOT, EAR1_L, EAR2_L, NOSE_C, NOSE_BASE, JAW, CHIN,
                      TONGUE_BASE, TONGUE_TIP, LIP_UP_C, FOREHEAD_C})
            ensure("head / Bento face bone is head-side", s.has(j));
        ensure("the HEAD collision volume is head-side", s.has(CV_HEAD));
        for (S32 j : {AP_SKULL, AP_CHIN, AP_NOSE, AP_L_EAR, AP_TONGUE, AP_JAW, AP_L_EYEBALL, AP_ALT_L_EAR})
            ensure("head attachment point is head-side", s.has(j));
        for (S32 j : {PELVIS, TORSO, CHEST, NECK, COLLAR_L, SHOULDER_L, ELBOW_L, WRIST_L, HIP_L, KNEE_L, TAIL1, TAIL2,
                      CV_NECK, CV_CHEST, AP_NECK, AP_CHEST, AP_L_HAND, AP_TAIL_TIP})
            ensure("body joint is not head-side (mNeck and the Neck point included)", !s.has(j));
        ensure("out of range", !s.has(-1) && !s.has((S32)kMaxJoints) && !s.has(1000));
    }

    // No mHead, a bad head index, a parent cycle: nothing (or only the head's own chain).
    template<> template<>
    void headless_object::test<2>()
    {
        const std::vector<S32> p = skeleton();
        ensure("no head: invalid", !buildHeadSet(p.data(), (U32)p.size(), -1).valid());
        ensure("head out of range: invalid", !buildHeadSet(p.data(), (U32)p.size(), (S32)p.size()).valid());
        ensure("no parents: invalid", !buildHeadSet(nullptr, 10, 2).valid());
        // 0 -> 1 -> 0 cycle, 2 the head with 3 under it: the cycle terminates, not head-side
        const S32 cyc[4] = {1, 0, -1, 2};
        const HeadSet c = buildHeadSet(cyc, 4, 2);
        ensure("cycle: valid", c.valid());
        ensure("cycle members are not head-side", !c.has(0) && !c.has(1));
        ensure("head and child", c.has(2) && c.has(3));
    }

    // The per-skin words: bit k = the skin's joint k is head-side, in all three word ranges,
    // and the head joint number rides in head_hi[1].
    template<> template<>
    void headless_object::test<3>()
    {
        const HeadSet s = headSet();
        std::vector<S32> nums(kSkinJoints, CHEST);   // a 110-joint skin, mostly body
        nums[0] = HEAD; nums[5] = NECK; nums[31] = JAW; nums[32] = EAR2_L; nums[63] = SKULL;
        nums[64] = NOSE_C; nums[95] = TONGUE_TIP; nums[96] = EYE_L; nums[109] = FOREHEAD_C;
        const SkinWords w = skinWords(nums.data(), (U32)nums.size(), s);
        ensure("any", w.any());
        for (U32 k : {0u, 31u, 32u, 63u, 64u, 95u, 96u, 109u})
            ensure("head joint bit", w.jointIsHead(k));
        for (U32 k : {1u, 5u, 30u, 33u, 62u, 65u, 94u, 97u, 108u})
            ensure("body joint bit clear", !w.jointIsHead(k));
        ensure("index past the skin", !w.jointIsHead(kSkinJoints));
        ensure_equals("head joint", w.headJoint(), (S32)HEAD);
        ensure_equals("hi[1] keeps 14 mask bits under the head number", w.hi[1] & ~kMaskHiBits,
                      (U32)(HEAD + 1) << kHeadShift);
        // a body-only skin: all words zero (the VS skips it on one read)
        const S32 body[3] = {CHEST, NECK, SHOULDER_L};
        const SkinWords b = skinWords(body, 3, s);
        ensure("body skin: no bits", !b.any() && b.lo[0] == 0u && b.lo[1] == 0u && b.hi[0] == 0u && b.hi[1] == 0u);
        // no head set -> nothing
        ensure("no head set", !skinWords(nums.data(), (U32)nums.size(), HeadSet()).any());
    }

    // A human mesh body + mesh head + hair: the 0.5 rule per vertex.
    template<> template<>
    void headless_object::test<4>()
    {
        const HeadSet s = headSet();
        // the skin's joint list: 0 mChest, 1 mNeck, 2 mHead, 3 mSkull, 4 mFaceJaw, 5 mFaceForeheadCenter,
        // 6 mCollarLeft, 7 mFaceEar1Left
        const S32 nums[8] = {CHEST, NECK, HEAD, SKULL, JAW, FOREHEAD_C, COLLAR_L, EAR1_L};
        const SkinWords w = skinWords(nums, 8, s);
        ensure("face (all mHead)", isHead(w, {{2u, 65535u}}));
        ensure("hair (mSkull)", isHead(w, {{3u, 65535u}}));
        ensure("jaw / face bones", isHead(w, {{4u, 40000u}, {5u, 25535u}}));
        ensure("chest", !isHead(w, {{0u, 65535u}}));
        ensure("neck base (mNeck / mChest)", !isHead(w, {{1u, 40000u}, {0u, 25535u}}));
        ensure("whole mNeck is body", !isHead(w, {{1u, 65535u}}));
        ensure("upper neck (mHead dominant)", isHead(w, {{2u, 45000u}, {1u, 20535u}}));
        ensure("lower neck (mNeck dominant)", !isHead(w, {{2u, 20535u}, {1u, 45000u}}));
        // the exact half: 32768 of 65535 is head, 32767 is not (2 * h >= 65535)
        ensure("half up", isHead(w, {{2u, 32768u}, {1u, 32767u}}));
        ensure("half down", !isHead(w, {{2u, 32767u}, {1u, 32768u}}));
        // head weight split over several head bones still sums
        ensure("split over three head bones", isHead(w, {{2u, 12000u}, {3u, 11000u}, {7u, 10000u}, {0u, 32535u}}));
        ensure("shoulder strap", !isHead(w, {{6u, 50000u}, {1u, 15535u}}));
        // a joint index past the skin's list clamps to the last joint (as the pre-skin does)
        ensure("clamped index", !isHead(w, {{200u, 65535u}}));
    }

    // Open Pony-style rig: ears and nose on the Bento face bones, a mane along mSkull / mNeck,
    // a tail on the tail bones. Nothing named: the hierarchy decides.
    template<> template<>
    void headless_object::test<5>()
    {
        const HeadSet s = headSet();
        const S32 nums[9] = {PELVIS, NECK, HEAD, SKULL, EAR1_L, EAR2_L, NOSE_C, NOSE_BASE, TAIL2};
        const SkinWords w = skinWords(nums, 9, s);
        ensure("ear tip", isHead(w, {{5u, 65535u}}));
        ensure("ear base blended to the head", isHead(w, {{4u, 30000u}, {2u, 35535u}}));
        ensure("muzzle", isHead(w, {{6u, 30000u}, {7u, 35535u}}));
        ensure("mane top (mSkull dominant)", isHead(w, {{3u, 40000u}, {1u, 25535u}}));
        ensure("mane down the neck (mNeck dominant)", !isHead(w, {{3u, 20000u}, {1u, 45535u}}));
        ensure("tail", !isHead(w, {{8u, 65535u}}));
        ensure("hoof / body", !isHead(w, {{0u, 65535u}}));
    }
}
