/**
 * @file slcamerapresets.cpp
 * @brief Camera mode presets (#1231, #1572). See slcamerapresets.h.
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

#include "slcamerapresets.h"

#include "slcamerarestore.h"
#include "slfloaterphotostudio.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llenvironment.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llpresetsmanager.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"

#include <algorithm>

namespace
{
constexpr const char* DIR_NAME = "camera_mode_presets";
constexpr S32 FORMAT_VERSION = 1;

// What a preset carries (besides the roll, the sky and the camera's placement).
const char* const PRESET_SETTINGS[] = {
    // Camera mode's own: the mode dial, ISO, framing, focus.
    "SLCameraModeExposure", "SLCameraModeISO", "SLCameraModeGrid", "SLCameraModeAspect", "SLCameraModeLevel",
    "SLCameraModeFocus", "SLCameraModeFocusDistance",
    // Exposure, aperture, shutter and their switches.
    "CameraFNumber", "RenderVKMotionBlurShutter", "RenderVKMotionBlur", "RenderDepthOfField",
    "RenderExposure", "RenderDynamicExposureEnabled",
    // White balance and the grade.
    "RenderColorGrade", "RenderColorGradeWhiteBalanceCCT", "RenderColorGradeWhiteBalanceDuv",
    "RenderColorGradeContrast", "RenderColorGradeSaturation", "RenderColorGradeVibrance", "RenderColorGradeBrightness",
    // Focal length.
    "CameraAngle",
    // The rail: sky, haze, effects, scene.
    "RenderAuroraIntensity", "RenderVKHazeStartDistance", "RenderVKHazeMaxOpacity", "RenderVKVolumetricFogSunScatter",
    "RenderVKWaterMist", "SLFxEnabled", "SLFxActivePreset", "RenderBloomStrength", "RenderLensFlareStrength",
    "RenderVignetteAmount", "RenderFilmGrainAmount", "RenderCASSharpness", "RenderShadowDetail", "RenderDeferredSSAO",
    "RenderVKGodRays", "RenderFarClip", "SLSnapshotAvatarScope",
};

std::string presetsDir()
{
    const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, DIR_NAME);
    if (!LLFile::isdir(dir)) LLFile::mkdir(dir);
    return dir;
}

std::string pathFor(const std::string& name)
{
    return presetsDir() + gDirUtilp->getDirDelimiter() + LLDir::getScrubbedFileName(name) + ".xml";
}

LLSD readPreset(const std::string& name)
{
    LLSD sd;
    llifstream in(pathFor(name).c_str());
    if (in.is_open()) LLSDSerialize::fromXML(sd, in);
    return sd;
}

} // namespace

namespace SLCameraPresets
{
std::vector<std::string> list()
{
    std::vector<std::string> names;
    const std::string dir = presetsDir();
    LLDirIterator it(dir, "*.xml");
    std::string file;
    while (it.next(file))
    {
        LLSD sd;
        llifstream in((dir + gDirUtilp->getDirDelimiter() + file).c_str());
        if (in.is_open() && LLSDSerialize::fromXML(sd, in) > 0 && sd.has("name"))
        {
            names.push_back(sd["name"].asString());
        }
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b)
              { return LLStringUtil::compareInsensitive(a, b) < 0; });
    return names;
}

bool save(const std::string& name)
{
    LLSD sd = LLSD::emptyMap();
    sd["name"] = name;
    sd["version"] = FORMAT_VERSION;
    LLSD settings = LLSD::emptyMap();
    for (const char* key : PRESET_SETTINGS)
    {
        if (LLControlVariable* control = gSavedSettings.getControl(key)) settings[key] = control->getValue();
    }
    sd["settings"] = settings;
    sd["look"] = gSavedSettings.getString("PresetLooksActive");
    sd["roll"] = gAgentCamera.getRollAngle();
    {
        LLEnvironment& env = LLEnvironment::instance();
        const bool frozen = (bool)env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL);
        sd["sky_frozen"] = frozen;
        SLFloaterPhotoStudio* studio = SLFloaterPhotoStudio::findInstance();
        sd["sky_position"] = studio ? studio->getDayCycleState().mPosition : llmax(env.getProgress(), 0.f);
    }
    // #1231: where the camera is, put back in the same region.
    if (LLViewerRegion* region = gAgent.getRegion())
    {
        sd["region"] = region->getName();
        sd["camera_position"] = gAgentCamera.getCameraPositionGlobal().getValue();
        sd["camera_focus"] = gAgentCamera.getFocusGlobal().getValue();
    }
    llofstream out(pathFor(name).c_str());
    if (!out.is_open()) return false;
    LLSDSerialize::toPrettyXML(sd, out);
    LL_INFOS("CameraMode") << "camera preset saved: " << name << LL_ENDL;
    return true;
}

bool load(const std::string& name)
{
    const LLSD sd = readPreset(name);
    if (!sd.isMap() || !sd.has("settings")) return false;
    // A Look first (it writes its whole key list), then the preset's own values over it.
    const std::string look = sd["look"].asString();
    if (!look.empty()) LLPresetsManager::getInstance()->loadLooksPreset(look);
    const LLSD& settings = sd["settings"];
    for (LLSD::map_const_iterator it = settings.beginMap(); it != settings.endMap(); ++it)
    {
        LLControlVariable* control = gSavedSettings.getControl(it->first);
        if (control && !llsd_equals(control->getValue(), it->second)) control->setValue(it->second);
    }
    gAgentCamera.setRollAngle((F32)sd["roll"].asReal());
    if (sd["sky_frozen"].asBoolean())
    {
        SLCameraRestore::noteSkyTouched();
        if (SLFloaterPhotoStudio* studio = LLFloaterReg::getTypedInstance<SLFloaterPhotoStudio>("photo_studio"))
        {
            studio->scrubDay((F32)sd["sky_position"].asReal());
        }
    }
    LLViewerRegion* region = gAgent.getRegion();
    if (region && sd.has("camera_position") && sd["region"].asString() == region->getName())
    {
        gAgentCamera.setCameraPosAndFocusGlobal(LLVector3d(sd["camera_position"]),
                                                LLVector3d(sd["camera_focus"]), LLUUID::null);
    }
    LL_INFOS("CameraMode") << "camera preset loaded: " << name << LL_ENDL;
    return true;
}

bool remove(const std::string& name)
{
    return LLFile::remove(pathFor(name)) == 0;
}
} // namespace SLCameraPresets
