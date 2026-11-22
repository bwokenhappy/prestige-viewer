/**
 * @file slcamerarail.cpp
 * @brief Camera mode (#1578): the Photo Studio rail. See slcamerarail.h.
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

#include "slcamerarail.h"

#include "slcameramode.h"
#include "slvideostudio.h"
#include "slcamerarestore.h"
#include "slfloaterphotostudio.h"
#include "slfxpresets.h"
#include "slgodraysavail.h"
#include "slphotostudiohaze.h"

#include "llenvironment.h"
#include "llfloatercamera.h"
#include "llfloaterreg.h"
#include "llhazeenv.h"
#include "llpresetsmanager.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llsdutil.h"

#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <vector>

namespace
{

// ---- settings access (any control type, as a number) ----

LLControlVariable* control(const char* name)
{
    return gSavedSettings.getControl(name);
}

F32 getNumber(const char* name)
{
    LLControlVariable* c = control(name);
    return c ? (F32)c->getValue().asReal() : 0.f;
}

void setNumber(const char* name, F32 value)
{
    LLControlVariable* c = control(name);
    if (!c) return;
    switch (c->type())
    {
    case TYPE_F32:     c->setValue(LLSD((F64)value)); break;
    case TYPE_BOOLEAN: c->setValue(LLSD(value != 0.f)); break;
    default:           c->setValue(LLSD(ll_round(value))); break;
    }
}

SLFloaterPhotoStudio* existingStudio()
{
    return LLFloaterReg::findTypedInstance<SLFloaterPhotoStudio>("photo_studio");
}

// The day cycle is Photo Studio's (it keeps the restore point for a frozen sky): built on
// first use, hidden, when the rail changes the sky.
SLFloaterPhotoStudio* studio()
{
    SLCameraRestore::noteSkyTouched();   // the sky is put back when camera mode closes
    return LLFloaterReg::getTypedInstance<SLFloaterPhotoStudio>("photo_studio");
}

// ---- the rows ----

enum RowKind
{
    ROW_SLIDER,    // a number: wheel steps, drag sets
    ROW_TOGGLE,    // on / off: click
    ROW_CHOICE,    // one of several: click right half next, left half previous; wheel steps
    ROW_BUTTONS,   // a few actions side by side
};

struct Row
{
    RowKind kind = ROW_SLIDER;
    std::string label;
    // ROW_SLIDER
    F32 lo = 0.f, hi = 1.f, step = 0.01f;
    std::function<F32()> get;
    std::function<void(F32)> set;
    std::function<std::string()> valueText;   // optional: overrides the number
    // ROW_TOGGLE
    std::function<bool()> isOn;
    std::function<void(bool)> setOn;
    // ROW_CHOICE
    std::function<std::vector<std::string>()> options;
    std::function<std::string()> current;
    std::function<void(const std::string&)> pick;
    // ROW_BUTTONS
    std::vector<std::pair<std::string, std::function<void()>>> buttons;
    // Greyed and inert when false.
    std::function<bool()> enabled;
    // Shown as the tooltip instead of the row's help while it is greyed (optional).
    std::string disabledHelp;
    // The setting behind the row, when there is one: it can go back to its default.
    const char* setting = nullptr;
};

struct Section
{
    std::string title;
    std::vector<Row> rows;
};

Row slider(const std::string& label, const char* setting, F32 lo, F32 hi, F32 step)
{
    Row r;
    r.kind = ROW_SLIDER;
    r.label = label;
    r.lo = lo;
    r.hi = hi;
    r.step = step;
    r.get = [setting]() { return getNumber(setting); };
    r.set = [setting](F32 v) { setNumber(setting, v); };
    r.setting = setting;
    return r;
}

Row toggle(const std::string& label, const char* setting)
{
    Row r;
    r.kind = ROW_TOGGLE;
    r.label = label;
    r.isOn = [setting]() { return getNumber(setting) != 0.f; };
    r.setOn = [setting](bool on) { setNumber(setting, on ? 1.f : 0.f); };
    r.setting = setting;
    return r;
}

// A numeric setting with named values.
Row choice(const std::string& label, const char* setting, std::vector<std::pair<std::string, S32>> values)
{
    Row r;
    r.kind = ROW_CHOICE;
    r.label = label;
    r.options = [values]()
    {
        std::vector<std::string> names;
        for (const auto& v : values) names.push_back(v.first);
        return names;
    };
    r.current = [setting, values]()
    {
        const S32 cur = ll_round(getNumber(setting));
        for (const auto& v : values)
            if (v.second == cur) return v.first;
        return llformat("%d", cur);
    };
    r.pick = [setting, values](const std::string& name)
    {
        for (const auto& v : values)
            if (v.first == name) setNumber(setting, (F32)v.second);
    };
    r.setting = setting;
    return r;
}

// Height fog: the resolved value, and a manual preference once it is turned (click: back to
// Automatic). Same rules as Photo Studio's Haze section.
Row hazeRow(const std::string& label, const char* setting, bool opacity)
{
    Row r;
    r.kind = ROW_SLIDER;
    r.label = label;
    r.lo = 0.f;
    r.hi = opacity ? 95.f : 512.f;
    r.step = opacity ? 1.f : 2.f;
    r.get = [opacity]()
    {
        const LLHazeDisplay d = llGetHazeDisplay();
        return opacity ? d.maxOpacity * 100.f : d.startDistance;
    };
    r.set = [setting, opacity](F32 display)
    {
        F32 preference = 0.f;
        if (SLPhotoStudioHaze::preferenceFromDisplay(display, opacity ? 100.0 : 1.0, opacity ? 0.95 : 512.0, preference))
        {
            gSavedSettings.setF32(setting, preference);
        }
    };
    r.valueText = [setting, opacity]()
    {
        const LLHazeDisplay d = llGetHazeDisplay();
        const F32 v = opacity ? d.maxOpacity * 100.f : d.startDistance;
        const bool manual = SLPhotoStudioHaze::isManual(gSavedSettings.getF32(setting));
        return llformat(opacity ? "%s%.0f%%" : "%s%.0f m", manual ? "" : "A ", v);
    };
    r.enabled = []() { return llGetHazeDisplay().physicalActive; };
    r.setting = setting;
    return r;
}

std::vector<std::string> presetNames(const std::string& kind)
{
    LLPresetsManager::preset_name_list_t names;
    LLPresetsManager::getInstance()->loadPresetNamesFromDir(kind, names, DEFAULT_HIDE);
    return std::vector<std::string>(names.begin(), names.end());
}

const std::vector<Section>& sections()
{
    static std::vector<Section> s_sections;
    if (!s_sections.empty()) return s_sections;

    {
        Section look{ "Look", {} };
        Row looks;
        looks.kind = ROW_CHOICE;
        looks.label = "Look";
        looks.options = []() { return presetNames(PRESETS_LOOKS); };
        looks.current = []()
        {
            const std::string active = gSavedSettings.getString("PresetLooksActive");
            const std::string last = gSavedSettings.getString("PresetLooksLastApplied");
            return !active.empty() ? active : (last.empty() ? std::string("(none)") : last + " *");
        };
        looks.pick = [](const std::string& name) { LLPresetsManager::getInstance()->loadLooksPreset(name); };
        look.rows.push_back(looks);
        look.rows.push_back(toggle("Colour grade", "RenderColorGrade"));
        auto graded = []() { return gSavedSettings.getBOOL("RenderColorGrade"); };
        for (Row r : { slider("Contrast", "RenderColorGradeContrast", 0.f, 2.f, 0.02f),
                       slider("Saturation", "RenderColorGradeSaturation", 0.f, 2.f, 0.02f),
                       slider("Vibrance", "RenderColorGradeVibrance", -1.f, 1.f, 0.02f),
                       slider("Brightness", "RenderColorGradeBrightness", -0.5f, 0.5f, 0.01f),
                       slider("Tint", "RenderColorGradeWhiteBalanceDuv", -1.f, 1.f, 0.02f) })
        {
            r.enabled = graded;
            look.rows.push_back(r);
        }
        s_sections.push_back(look);
    }
    {
        Section sky{ "Sky and time", {} };
        Row freeze;
        freeze.kind = ROW_TOGGLE;
        freeze.label = "Hold the sky";
        freeze.isOn = []()
        {
            SLFloaterPhotoStudio* s = existingStudio();
            return s ? s->getDayCycleState().mFrozen : false;
        };
        freeze.setOn = [](bool on)
        {
            if (SLFloaterPhotoStudio* s = studio()) s->setDayFrozen(on);
        };
        sky.rows.push_back(freeze);
        Row position;
        position.kind = ROW_SLIDER;
        position.label = "Time of day";
        position.lo = 0.f;
        position.hi = 1.f;
        position.step = 0.01f;
        position.get = []()
        {
            SLFloaterPhotoStudio* s = existingStudio();
            return s ? s->getDayCycleState().mPosition : LLEnvironment::instance().getProgress();
        };
        position.set = [](F32 v)
        {
            if (SLFloaterPhotoStudio* s = studio()) s->scrubDay(v);
        };
        position.valueText = [get = position.get]() { return llformat("%.0f%%", get() * 100.f); };
        sky.rows.push_back(position);
        Row presets;
        presets.kind = ROW_BUTTONS;
        for (const char* which : { "sunrise", "noon", "sunset", "midnight" })
        {
            std::string label = which;
            label[0] = (char)toupper(label[0]);
            presets.buttons.emplace_back(label, [w = std::string(which)]()
            {
                if (SLFloaterPhotoStudio* s = studio()) s->goToDayPreset(w);
            });
        }
        sky.rows.push_back(presets);
        sky.rows.push_back(slider("Aurora", "RenderAuroraIntensity", 0.f, 2.f, 0.05f));
        s_sections.push_back(sky);
    }
    {
        Section haze{ "Haze", {} };
        haze.rows.push_back(hazeRow("Haze start", "RenderVKHazeStartDistance", false));
        haze.rows.push_back(hazeRow("Haze opacity", "RenderVKHazeMaxOpacity", true));
        Row automatic;
        automatic.kind = ROW_BUTTONS;
        automatic.buttons.emplace_back("Haze: automatic", []()
        {
            gSavedSettings.setF32("RenderVKHazeStartDistance", -1.f);
            gSavedSettings.setF32("RenderVKHazeMaxOpacity", -1.f);
        });
        haze.rows.push_back(automatic);
        haze.rows.push_back(slider("Sun shafts", "RenderVKVolumetricFogSunScatter", 0.f, 4.f, 0.05f));
        haze.rows.push_back(slider("Water mist", "RenderVKWaterMist", 0.f, 4.f, 0.05f));
        s_sections.push_back(haze);
    }
    {
        Section fx{ "Effects", {} };
        fx.rows.push_back(toggle("Effects (FX)", "SLFxEnabled"));
        Row preset;
        preset.kind = ROW_CHOICE;
        preset.label = "FX preset";
        preset.options = []()
        {
            SLFxPresets::instance().init();
            return SLFxPresets::instance().listPresets();
        };
        preset.current = []() { return gSavedSettings.getString("SLFxActivePreset"); };
        preset.pick = [](const std::string& name) { gSavedSettings.setString("SLFxActivePreset", name); };
        preset.enabled = []() { return gSavedSettings.getBOOL("SLFxEnabled"); };
        preset.setting = "SLFxActivePreset";
        fx.rows.push_back(preset);
        fx.rows.push_back(slider("Bloom", "RenderBloomStrength", 0.f, 2.f, 0.02f));
        fx.rows.push_back(slider("Lens flare", "RenderLensFlareStrength", 0.f, 1.f, 0.02f));
        fx.rows.push_back(slider("Vignette", "RenderVignetteAmount", 0.f, 1.f, 0.02f));
        fx.rows.push_back(slider("Film grain", "RenderFilmGrainAmount", 0.f, 1.f, 0.02f));
        fx.rows.push_back(slider("Sharpen", "RenderCASSharpness", 0.f, 1.f, 0.02f));
        s_sections.push_back(fx);
    }
    {
        Section scene{ "Scene", {} };
        scene.rows.push_back(choice("Shadows", "RenderShadowDetail", { { "None", 0 }, { "Sun/Moon", 1 }, { "+ Projectors", 2 } }));
        scene.rows.push_back(toggle("Ambient occlusion", "RenderDeferredSSAO"));
        {
            // Greyed while volumetric fog or the physical sky replaces the pass (the value is kept).
            Row godRays = choice("God rays", "RenderVKGodRays", { { "Off", 0 }, { "Low", 1 }, { "Medium", 2 }, { "High", 3 } });
            godRays.enabled = []() { return SLGodRays::availableFromSettings(); };
            godRays.disabledHelp = SLGodRays::unavailableTip();
            scene.rows.push_back(godRays);
        }
        scene.rows.push_back(slider("Draw distance", "RenderFarClip", 8.f, 4096.f, 16.f));
        // The shot's avatars: the snapshot avatar scope (#1328), which camera mode's capture uses.
        scene.rows.push_back(choice("Avatars in shot", "SLSnapshotAvatarScope", { { "Only me", 0 }, { "Selected", 1 }, { "Everyone", 2 } }));
        s_sections.push_back(scene);
    }
    {
        Section cam{ "Camera presets", {} };
        Row preset;
        preset.kind = ROW_CHOICE;
        preset.label = "View";
        preset.options = []() { return presetNames(PRESETS_CAMERA); };
        preset.current = []() { return gSavedSettings.getString("PresetCameraActive"); };
        preset.pick = [](const std::string& name) { LLFloaterCamera::switchToPreset(name); };
        cam.rows.push_back(preset);
        Row views;
        views.kind = ROW_BUTTONS;
        for (const std::string& name : { PRESETS_REAR_VIEW, PRESETS_FRONT_VIEW, PRESETS_SIDE_VIEW })
        {
            views.buttons.emplace_back(name.substr(0, name.find(' ')), [name]() { LLFloaterCamera::switchToPreset(name); });
        }
        cam.rows.push_back(views);
        // Video Studio (#1668): the moving camera (tracks, rides, recording) -- leaves camera mode.
        Row video;
        video.kind = ROW_BUTTONS;
        video.buttons.emplace_back("Video Studio...", []() { SLVideoStudio::open(SLVideoStudio::PANEL_TRACK); });
        cam.rows.push_back(video);
        s_sections.push_back(cam);
    }
    return s_sections;
}

// ---- state ----

bool railOpen()
{
    static LLCachedControl<bool> s_open(gSavedSettings, "SLCameraModeRailOpen", true);
    return s_open;
}

U32 expandedMask()
{
    static LLCachedControl<U32> s_sections(gSavedSettings, "SLCameraModeRailSections", 1);
    return s_sections;
}

// The row's setting differs from its default (numbers within a hair).
bool rowChanged(const Row& r)
{
    LLControlVariable* c = r.setting ? control(r.setting) : nullptr;
    if (!c)
        return false;
    const LLSD value = c->getValue(), def = c->getDefault();
    if (value.isReal() || def.isReal())
        return std::fabs(value.asReal() - def.asReal()) > 1e-4;
    return !llsd_equals(value, def);
}

bool rowEnabled(const Row& r)
{
    return !r.enabled || r.enabled();
}

void stepChoice(const Row& r, S32 dir)
{
    const std::vector<std::string> names = r.options();
    if (names.empty()) return;
    const std::string cur = r.current();
    S32 index = -1;
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == cur) index = (S32)i;
    const S32 n = (S32)names.size();
    index = index < 0 ? (dir > 0 ? 0 : n - 1) : ((index + dir) % n + n) % n;
    r.pick(names[(size_t)index]);
}

void stepSlider(const Row& r, S32 clicks)
{
    const F32 v = llclamp(r.get() + r.step * (F32)clicks, r.lo, r.hi);
    r.set(v);
}

} // namespace

namespace SLCameraRail
{
bool dockedRight()
{
    static LLCachedControl<U32> s_side(gSavedSettings, "SLCameraModeRailSide", 0);
    return s_side != 0;
}

size_t sectionCount()
{
    return sections().size();
}

const std::string& sectionTitle(size_t section)
{
    static const std::string none;
    return section < sections().size() ? sections()[section].title : none;
}

size_t cameraSection()
{
    return sections().size() - 1; // the last: "Camera presets"
}

bool sectionOpen(size_t section)
{
    return section < 32 && (expandedMask() & (1u << section)) != 0;
}

void toggleSection(size_t section)
{
    if (section < 32)
        gSavedSettings.setU32("SLCameraModeRailSections", expandedMask() ^ (1u << section));
}

std::vector<RowData> rows(size_t section)
{
    std::vector<RowData> out;
    if (section >= sections().size())
        return out;
    for (const Row& r : sections()[section].rows)
    {
        RowData d;
        d.label   = r.label;
        d.changed = rowChanged(r);
        d.kind    = (S32)r.kind;
        d.enabled = rowEnabled(r);
        if (!d.enabled)
            d.why = r.disabledHelp;
        switch (r.kind)
        {
        case ROW_SLIDER:
        {
            const F32 v = r.get();
            d.fraction = llclamp((v - r.lo) / llmax(r.hi - r.lo, 1e-6f), 0.f, 1.f);
            d.value = r.valueText ? r.valueText() : (r.hi - r.lo >= 50.f ? llformat("%.0f", v) : llformat("%.2f", v));
            break;
        }
        case ROW_TOGGLE:
            d.on    = r.isOn();
            d.value = d.on ? "On" : "Off";
            break;
        case ROW_CHOICE:
            if (r.options().empty())
            {
                // Nothing to choose from (no saved Looks yet): say so, inert.
                d.enabled = false;
                d.value   = LLTrans::getString("CameraModeRailNoneSaved");
            }
            else
                d.value = r.current();
            break;
        case ROW_BUTTONS:
            for (const auto& b : r.buttons)
                d.buttons.push_back(b.first);
            break;
        }
        out.push_back(std::move(d));
    }
    return out;
}

namespace
{
const Row* rowAt(size_t section, size_t row)
{
    if (section >= sections().size() || row >= sections()[section].rows.size())
        return nullptr;
    const Row* r = &sections()[section].rows[row];
    return rowEnabled(*r) ? r : nullptr;
}
} // namespace

void turn(size_t section, size_t row, S32 dir)
{
    const Row* r = rowAt(section, row);
    if (!r || dir == 0)
        return;
    switch (r->kind)
    {
    case ROW_SLIDER: stepSlider(*r, dir); break;
    case ROW_TOGGLE: r->setOn(!r->isOn()); break;
    case ROW_CHOICE: stepChoice(*r, dir); break;
    default: break;
    }
}

void setFraction(size_t section, size_t row, F32 fraction)
{
    const Row* r = rowAt(section, row);
    if (!r)
        return;
    if (r->kind == ROW_TOGGLE)
    {
        r->setOn(fraction > 0.5f);
        return;
    }
    if (r->kind != ROW_SLIDER)
        return;
    F32 v = r->lo + (r->hi - r->lo) * llclamp(fraction, 0.f, 1.f);
    if (r->step > 0.f)
        v = r->lo + std::round((v - r->lo) / r->step) * r->step;
    r->set(llclamp(v, r->lo, r->hi));
}

void resetRow(size_t section, size_t row)
{
    const Row* r = rowAt(section, row);
    if (LLControlVariable* c = r && r->setting ? control(r->setting) : nullptr)
        c->resetToDefault(true);
}

void press(size_t section, size_t row, size_t button)
{
    const Row* r = rowAt(section, row);
    if (r && r->kind == ROW_BUTTONS && button < r->buttons.size())
        r->buttons[button].second();
}

bool open()
{
    return railOpen();
}

void setOpen(bool open)
{
    gSavedSettings.setBOOL("SLCameraModeRailOpen", open);
}

void setDockedRight(bool right)
{
    gSavedSettings.setU32("SLCameraModeRailSide", right ? 1 : 0);
}

void openPhotoStudio()
{
    SLCameraMode::stop();
    LLFloaterReg::showInstance("photo_studio");
}

} // namespace SLCameraRail
