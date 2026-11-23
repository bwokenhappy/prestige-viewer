/**
 * @file prestigefloaterposestand.h
 * @brief Prestige pose stand (Me > Pose stand): holds your avatar in a still
 *        pose for fitting clothes, hair, jewelry and attachments.
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

#ifndef PRESTIGE_FLOATERPOSESTAND_H
#define PRESTIGE_FLOATERPOSESTAND_H

#include "llfloater.h"
#include "lluuid.h"

class LLComboBox;

class PrestigeFloaterPoseStand : public LLFloater
{
    LOG_CLASS(PrestigeFloaterPoseStand);
public:
    PrestigeFloaterPoseStand(const LLSD& key);
    ~PrestigeFloaterPoseStand() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

private:
    void startPose(const LLUUID& anim);
    void stopPose();
    void onPoseChanged();

    LLComboBox* mPoseCombo = nullptr;
    LLUUID      mPlaying;
    bool        mPausedAO = false;   // we paused the AO and must resume it
};

#endif // PRESTIGE_FLOATERPOSESTAND_H
