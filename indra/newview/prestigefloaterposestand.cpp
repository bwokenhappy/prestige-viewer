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
    if (mPlaying.notNull())
    {
        gAgent.sendAnimationRequest(mPlaying, ANIM_REQUEST_STOP);
        mPlaying.setNull();
    }
}
