/**
 * @file slcamerarestore.cpp
 * @brief Camera mode (#1572): put back everything camera mode changed. See slcamerarestore.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slcamerarestore.h"

#include "slfloaterphotostudio.h"

#include "llagentcamera.h"
#include "llenvironment.h"
#include "llfloaterreg.h"
#include "llpresetsmanager.h"
#include "llsdutil.h"
#include "llviewercontrol.h"

#include <set>

namespace
{
constexpr const char* RECORD = "SLCameraModeRestore";

// Written last on restore: putting back a Look's or a camera preset's keys one by one marks the
// preset "modified" (clears its active name) on the way.
const char* const PRESET_NAMES[] = { "PresetLooksActive", "PresetLooksLastApplied", "PresetCameraActive" };

struct Session
{
    bool captured = false;
    F32 roll = 0.f;
    bool skyTouched = false;
    bool skyWasFrozen = false;
    F32 skyPosition = 0.f;
};
Session sSession;

bool isPresetName(const std::string& name)
{
    for (const char* p : PRESET_NAMES)
        if (name == p) return true;
    return false;
}

void put(const std::string& name, const LLSD& value)
{
    LLControlVariable* control = gSavedSettings.getControl(name);
    if (control && !llsd_equals(control->getValue(), value))
    {
        control->setValue(value);
    }
}

// Put the recorded values back; false when there was no record.
bool restoreRecord(const LLSD& record)
{
    if (!record.isMap() || !record.has("settings")) return false;
    const LLSD& settings = record["settings"];
    for (LLSD::map_const_iterator it = settings.beginMap(); it != settings.endMap(); ++it)
    {
        if (!isPresetName(it->first)) put(it->first, it->second);
    }
    for (const char* p : PRESET_NAMES)
    {
        if (settings.has(p)) put(p, settings[p]);
    }
    return true;
}
} // namespace

namespace SLCameraRestore
{
const std::vector<std::string>& guardedSettings()
{
    static std::vector<std::string> s_names;
    if (!s_names.empty()) return s_names;
    std::set<std::string> names = {
        // The dials (#1575 / #1576) and the DoF preview.
        "CameraFNumber", "RenderVKMotionBlurShutter", "RenderVKMotionBlur", "RenderDepthOfField",
        "RenderExposure", "RenderDynamicExposureEnabled",
        "RenderColorGrade", "RenderColorGradeWhiteBalanceCCT", "RenderColorGradeWhiteBalanceDuv",
        "CameraAngle",
        // The rail (#1578): sky, haze, effects, scene. (Its Look rows are the Look keys below.)
        "RenderAuroraIntensity",
        "RenderVKHazeStartDistance", "RenderVKHazeMaxOpacity", "RenderVKVolumetricFogSunScatter", "RenderVKWaterMist",
        "SLFxEnabled", "SLFxActivePreset",
        "RenderBloomStrength", "RenderLensFlareStrength", "RenderVignetteAmount", "RenderFilmGrainAmount",
        "RenderCASSharpness",
        "RenderShadowDetail", "RenderDeferredSSAO", "RenderVKGodRays", "RenderFarClip", "SLSnapshotAvatarScope",
        // Camera presets (the rail's preset row): LLPresetsManager's camera keys.
        "CameraOffsetScale", "CameraOffsetBuild", "TrackFocusObject", "CameraOffsetRearView",
        "FocusOffsetRearView", "AvatarSitRotation", "ZoomTime", "CameraPositionSmoothing",
    };
    // A Look (the rail's Look row, the Q menu's picture style) writes every key on its list.
    std::vector<std::string> looks;
    LLPresetsManager::getInstance()->getLooksControlNames(looks);
    names.insert(looks.begin(), looks.end());
    for (const char* p : PRESET_NAMES) names.insert(p);
    s_names.assign(names.begin(), names.end());
    return s_names;
}

void capture()
{
    LLSD settings = LLSD::emptyMap();
    for (const std::string& name : guardedSettings())
    {
        if (LLControlVariable* control = gSavedSettings.getControl(name))
        {
            settings[name] = control->getValue();
        }
    }
    LLSD record = LLSD::emptyMap();
    record["settings"] = settings;
    gSavedSettings.setLLSD(RECORD, record);

    sSession = Session();
    sSession.captured = true;
    sSession.roll = gAgentCamera.getRollAngle();
    LLEnvironment& env = LLEnvironment::instance();
    sSession.skyWasFrozen = (bool)env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
    if (SLFloaterPhotoStudio* studio = SLFloaterPhotoStudio::findInstance())
    {
        sSession.skyPosition = studio->getDayCycleState().mPosition;
    }
    else
    {
        sSession.skyPosition = llmax(env.getProgress(), 0.f);
    }
}

void restore(bool in_session)
{
    restoreRecord(gSavedSettings.getLLSD(RECORD));
    gSavedSettings.setLLSD(RECORD, LLSD::emptyMap());
    if (!sSession.captured) return;
    if (in_session)
    {
        gAgentCamera.setRollAngle(sSession.roll);
        if (sSession.skyTouched)
        {
            if (SLFloaterPhotoStudio* studio = LLFloaterReg::getTypedInstance<SLFloaterPhotoStudio>("photo_studio"))
            {
                // Not frozen at entry: thawing gives back the environment Photo Studio kept when
                // the rail first froze it. Frozen at entry: back to that point of the day.
                if (sSession.skyWasFrozen)
                    studio->scrubDay(sSession.skyPosition);
                else
                    studio->setDayFrozen(false);
            }
        }
    }
    sSession = Session();
}

void restoreAfterCrash()
{
    const LLSD record = gSavedSettings.getLLSD(RECORD);
    if (restoreRecord(record))
    {
        LL_INFOS("CameraMode") << "the viewer last closed in camera mode: its settings are put back ("
                               << record["settings"].size() << " checked)" << LL_ENDL;
    }
    gSavedSettings.setLLSD(RECORD, LLSD::emptyMap());
}

void noteSkyTouched()
{
    sSession.skyTouched = true;
}
} // namespace SLCameraRestore
