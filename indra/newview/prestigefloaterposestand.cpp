/**
 * @file prestigefloaterposestand.cpp
 * @brief Prestige pose stand (Me > Pose stand).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "prestigefloaterposestand.h"

#include "llagent.h"
#include "llanimationstates.h"
#include "llbvhloader.h"
#include "lldatapacker.h"
#include "llkeyframemotion.h"

#include <cstring>
#include <vector>
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llvoavatarself.h"
#include "slaoengine.h"

namespace
{
    struct Pose
    {
        const char*   mValue;
        const LLUUID* mAnim;
    };

    // Built-in Second Life animations, so everyone around you sees the pose
    // and nothing has to be in your inventory.
    const Pose POSES[] =
    {
        { "arms_out", &ANIM_AGENT_CUSTOMIZE },
        { "stand_1",  &ANIM_AGENT_STAND_1 },
        { "stand_2",  &ANIM_AGENT_STAND_2 },
        { "stand_3",  &ANIM_AGENT_STAND_3 },
        { "stand_4",  &ANIM_AGENT_STAND_4 },
    };

    // T-pose / A-pose: Second Life has no built-in animation for them, so they are made here
    // as a small BVH (the avatar's bind pose is the T-pose: arms straight out, palms down) and
    // played on your own avatar only -- they are for fitting, which is about what you see.
    // Frame 0 is the BVH reference frame (ignored, but every joint must differ from it to be
    // kept), frames 1 and 2 hold the pose.
    const char* BVH_JOINTS[] = { "abdomen", "chest", "neck", "head",
                                 "lCollar", "lShldr", "lForeArm", "lHand",
                                 "rCollar", "rShldr", "rForeArm", "rHand",
                                 "lThigh", "lShin", "lFoot", "rThigh", "rShin", "rFoot" };

    std::string pose_bvh(bool a_pose)
    {
        std::string s =
            "HIERARCHY\nROOT hip\n{\n OFFSET 0 0 0\n CHANNELS 6 Xposition Yposition Zposition Xrotation Zrotation Yrotation\n"
            " JOINT abdomen\n {\n  OFFSET 0 3.4 0\n  CHANNELS 3 Xrotation Zrotation Yrotation\n"
            "  JOINT chest\n  {\n   OFFSET 0 8.2 0\n   CHANNELS 3 Xrotation Zrotation Yrotation\n"
            "   JOINT neck\n   {\n    OFFSET 0 9.0 0\n    CHANNELS 3 Xrotation Zrotation Yrotation\n"
            "    JOINT head\n    {\n     OFFSET 0 3.5 0\n     CHANNELS 3 Xrotation Zrotation Yrotation\n     End Site\n     {\n      OFFSET 0 3 0\n     }\n    }\n   }\n";
        for (int side = 0; side < 2; ++side)
        {
            const char* c = side == 0 ? "l" : "r";
            const char* x = side == 0 ? "" : "-";
            s += llformat("   JOINT %sCollar\n   {\n    OFFSET %s3.5 6.7 -0.8\n    CHANNELS 3 Xrotation Zrotation Yrotation\n"
                          "    JOINT %sShldr\n    {\n     OFFSET %s2.6 0 0\n     CHANNELS 3 Xrotation Zrotation Yrotation\n"
                          "     JOINT %sForeArm\n     {\n      OFFSET %s10 0 0\n      CHANNELS 3 Xrotation Zrotation Yrotation\n"
                          "      JOINT %sHand\n      {\n       OFFSET %s8 0 0\n       CHANNELS 3 Xrotation Zrotation Yrotation\n"
                          "       End Site\n       {\n        OFFSET %s4 0 0\n       }\n      }\n     }\n    }\n   }\n",
                          c, x, c, x, c, x, c, x, x);
        }
        s += "  }\n }\n";
        for (int side = 0; side < 2; ++side)
        {
            const char* c = side == 0 ? "l" : "r";
            const char* x = side == 0 ? "" : "-";
            s += llformat(" JOINT %sThigh\n {\n  OFFSET %s4 -1.4 0\n  CHANNELS 3 Xrotation Zrotation Yrotation\n"
                          "  JOINT %sShin\n  {\n   OFFSET 0 -18 0\n   CHANNELS 3 Xrotation Zrotation Yrotation\n"
                          "   JOINT %sFoot\n   {\n    OFFSET 0 -18 -1\n    CHANNELS 3 Xrotation Zrotation Yrotation\n"
                          "    End Site\n    {\n     OFFSET 0 -2 5\n    }\n   }\n  }\n }\n",
                          c, x, c, c);
        }
        s += "}\nMOTION\nFrames: 3\nFrame Time: 0.5\n";
        for (int frame = 0; frame < 3; ++frame)
        {
            const bool ref = frame == 0;
            s += "0 43.5 0";                                        // hip position: the same every frame (kept still)
            s += ref ? " 7 7 7" : " 0 0 0";                         // hip rotation: upright
            for (const char* j : BVH_JOINTS)
            {
                F32 xr = 0.f, zr = 0.f, yr = 0.f;
                if (a_pose && !strcmp(j, "lShldr")) zr = -45.f;     // arms down to an A
                if (a_pose && !strcmp(j, "rShldr")) zr = 45.f;
                if (ref) { xr += 7.f; zr += 7.f; yr += 7.f; }
                s += llformat(" %.2f %.2f %.2f", xr, zr, yr);
            }
            s += "\n";
        }
        return s;
    }

    // Builds the pose as a local keyframe motion on your avatar and returns its id (null on failure).
    LLUUID make_local_pose(bool a_pose)
    {
        if (!isAgentAvatarValid()) return LLUUID::null;
        auto aliases = gAgentAvatarp->getJointAliases();
        ELoadStatus status = E_ST_OK;
        S32 line = 0;
        const std::string bvh = pose_bvh(a_pose);
        LLBVHLoader loader(bvh.c_str(), status, line, aliases);
        if (!loader.isInitialized())
        {
            LL_WARNS("PoseStand") << "pose BVH did not load (status " << (S32)status << ", line " << line << ")" << LL_ENDL;
            return LLUUID::null;
        }
        LLUUID id;
        id.generate();
        LLKeyframeMotion* motion = dynamic_cast<LLKeyframeMotion*>(gAgentAvatarp->createMotion(id));
        if (!motion) return LLUUID::null;
        std::vector<U8> buffer(loader.getOutputSize());
        LLDataPackerBinaryBuffer dp(buffer.data(), (S32)buffer.size());
        loader.serialize(dp);
        dp.reset();
        if (!motion->deserialize(dp, id, false))
        {
            gAgentAvatarp->removeMotion(id);
            return LLUUID::null;
        }
        motion->setName(a_pose ? "Prestige A-pose" : "Prestige T-pose");
        motion->setPriority(LLJoint::HIGHEST_PRIORITY);
        motion->setLoop(true);
        motion->setEaseIn(0.4f);
        motion->setEaseOut(0.4f);
        return id;
    }

    LLUUID anim_for(const std::string& value)
    {
        for (const Pose& pose : POSES)
        {
            if (value == pose.mValue)
            {
                return *pose.mAnim;
            }
        }
        return ANIM_AGENT_CUSTOMIZE;
    }
}

PrestigeFloaterPoseStand::PrestigeFloaterPoseStand(const LLSD& key)
    : LLFloater(key)
{
}

PrestigeFloaterPoseStand::~PrestigeFloaterPoseStand()
{
}

bool PrestigeFloaterPoseStand::postBuild()
{
    mPoseCombo = getChild<LLComboBox>("pose_combo");
    mPoseCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPoseChanged(); });
    getChild<LLUICtrl>("next_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            mPoseCombo->selectNextItem();
            onPoseChanged();
        });
    getChild<LLUICtrl>("prev_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            mPoseCombo->selectPrevItem();
            onPoseChanged();
        });
    getChild<LLUICtrl>("height_btn")->setCommitCallback([](LLUICtrl*, const LLSD&)
        {
            LLFloaterReg::showInstance("edit_hover_height");
        });
    getChild<LLUICtrl>("done_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    return true;
}

void PrestigeFloaterPoseStand::onOpen(const LLSD& key)
{
    if (!isAgentAvatarValid())
    {
        return;
    }
    // Sitting or flying would fight the pose.
    if (gAgent.getFlying())
    {
        gAgent.setFlying(false);
    }
    // The AO would replace the pose with its own stands: pause it while posing.
    SLAOEngine& ao = SLAOEngine::instance();
    if (ao.isEnabled() && !ao.isUserPaused())
    {
        ao.setUserPaused(true);
        mPausedAO = true;
    }
    onPoseChanged();
}

void PrestigeFloaterPoseStand::onClose(bool app_quitting)
{
    if (app_quitting)
    {
        mPlaying.setNull();
        mLocalPose.setNull();
        mPausedAO = false;
        return;
    }
    stopPose();
    if (mPausedAO && !app_quitting)
    {
        SLAOEngine::instance().setUserPaused(false);
    }
    mPausedAO = false;
}

void PrestigeFloaterPoseStand::onPoseChanged()
{
    const std::string value = mPoseCombo ? mPoseCombo->getValue().asString() : std::string();
    if (value == "t_pose" || value == "a_pose")
    {
        stopPose();
        const LLUUID id = make_local_pose(value == "a_pose");
        if (id.notNull())
        {
            gAgentAvatarp->startMotion(id);
            mLocalPose = id;
            return;
        }
        // (could not build it: fall back to the arms-out fitting pose)
    }
    startPose(anim_for(value));
}

void PrestigeFloaterPoseStand::startPose(const LLUUID& anim)
{
    if (anim == mPlaying)
    {
        return;
    }
    stopPose();
    gAgent.sendAnimationRequest(anim, ANIM_REQUEST_START);
    mPlaying = anim;
}

void PrestigeFloaterPoseStand::stopPose()
{
    if (mLocalPose.notNull())
    {
        if (isAgentAvatarValid())
        {
            gAgentAvatarp->stopMotion(mLocalPose);
            gAgentAvatarp->removeMotion(mLocalPose);
        }
        mLocalPose.setNull();
    }
    if (mPlaying.notNull())
    {
        gAgent.sendAnimationRequest(mPlaying, ANIM_REQUEST_STOP);
        mPlaying.setNull();
    }
}
