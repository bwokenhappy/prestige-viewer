/**
 * @file llfloaterpreferencesgraphicsadvanced.cpp
 * @brief floater for adjusting camera position
 *
 * $LicenseInfo:firstyear=2021&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2021, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "llgpuinfo.h"
#include "llfloaterpreferencesgraphicsadvanced.h"

#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfeaturemanager.h"
#include "llfloaterpreference.h"
#include "llagentcamera.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llsliderctrl.h"
#include "slgodraysavail.h"
#include "lltextbox.h"
#include "lltrans.h"
#include <map>
#include <utility>
#include "llviewertexturelist.h"
#include "llvoavatar.h"
#include "pipeline.h"
// [RLVa:KB] - Checked: 2010-03-18 (RLVa-1.2.0a)
#include "rlvactions.h"
#include "vkgfx/llvulkancontext.h"   // ray-query capability (projector shadows)
#include "vkgfx/vk_requirements.h"   // #277 graphics card list
#include "vkgfx/llvkneuralrecon.h"   // #870 / #962 DLSS availability per network (the user-installed shim)
#include "vkgfx/llvkframegen.h"     // #1517 DLSS Frame Generation: caps, reason, live status
// [/RLVa:KB]


LLFloaterPreferenceGraphicsAdvanced::LLFloaterPreferenceGraphicsAdvanced(const LLSD& key)
    : LLFloater(key)
{
    mCommitCallbackRegistrar.add("Pref.RenderOptionUpdate",            boost::bind(&LLFloaterPreferenceGraphicsAdvanced::onRenderOptionEnable, this));
    mCommitCallbackRegistrar.add("Pref.UpdateIndirectMaxNonImpostors", boost::bind(&LLFloaterPreferenceGraphicsAdvanced::updateMaxNonImpostors,this));
    mCommitCallbackRegistrar.add("Pref.UpdateIndirectMaxComplexity",   boost::bind(&LLFloaterPreferenceGraphicsAdvanced::updateMaxComplexity,this));

    mCommitCallbackRegistrar.add("Pref.Cancel", boost::bind(&LLFloaterPreferenceGraphicsAdvanced::onBtnCancel, this, _2));
    mCommitCallbackRegistrar.add("Pref.OK",     boost::bind(&LLFloaterPreferenceGraphicsAdvanced::onBtnOK, this, _2));

    mImpostorsChangedSignal = gSavedSettings.getControl("RenderAvatarMaxNonImpostors")->getSignal()->connect(boost::bind(&LLFloaterPreferenceGraphicsAdvanced::updateIndirectMaxNonImpostors, this, _2));
    // Projector Shadows is only enabled at Shadows >= "Sun/Moon + Projectors": re-evaluate
    // whenever the shadow level changes (this combo, Quick Prefs, a quality preset, debug
    // settings). The Shadows combo had no commit hook, so after raising it by hand the
    // Projector Shadows combo stayed greyed until something else refreshed the floater.
    mShadowDetailChangedSignal = gSavedSettings.getControl("RenderShadowDetail")->getSignal()->connect(
        [this](LLControlVariable*, const LLSD&, const LLSD&) { refreshEnabledState(); });
    // RT shadow resolution is enabled only while something is ray traced.
    // ...and Antialiasing Quality only while FXAA / SMAA actually runs.
    // ...and the Ray-traced lighting section's prerequisites (refreshRtLightingState).
    for (const char* n : {"RenderVKSunShadows", "RenderVKProjectorShadows",
                          "RenderFSAAType", "RenderVKUpscaler", "RenderVKDlssRayReconstruction",
                          "RenderVKRayTracing", "RenderVKRTReflections", "RenderVKGI", "RenderVKProjStochastic",
                          "RenderVKDeferredSplit", "RenderVKFrameGen"})
        mRtModeChangedSignals.push_back(gSavedSettings.getControl(n)->getSignal()->connect(
            [this](LLControlVariable*, const LLSD&, const LLSD&) { refreshEnabledState(); }));
}

LLFloaterPreferenceGraphicsAdvanced::~LLFloaterPreferenceGraphicsAdvanced()
{
    mImpostorsChangedSignal.disconnect();
    mShadowDetailChangedSignal.disconnect();
    for (auto& c : mRtModeChangedSignals) c.disconnect();
    mComplexityChangedSignal.disconnect();
    mComplexityModeChangedSignal.disconnect();
    mLODFactorChangedSignal.disconnect();
    mBackgroundFPSChangedSignal.disconnect();
}

// #277: fill the graphics card combo from the Vulkan pre-flight (every enumerated device;
// ones that do not meet the requirements are listed greyed out with the reason).
static void populate_graphics_card_combo(LLComboBox* combo)
{
    if (!combo) return;
    const std::string saved = gSavedSettings.getString("RenderVKDeviceSelect");
    bool found = saved.empty() || saved == "auto";
    for (const VkGpuRequirements& g : vkLastRequirementsReport().gpus)
    {
        std::string label = g.name + " (" + vkDeviceTypeName(g.type);
        if (g.vramBytes)
        {
            label += ", " + llformat("%.1f GB", (double)g.vramBytes / (1024.0 * 1024.0 * 1024.0));
        }
        label += ", driver " + g.driverText + ")";
        const bool ok = g.qualifies();
        if (!ok)
        {
            label += " - unsupported: " + (g.missing.empty() ? std::string("requirements not met") : g.missing.front());
        }
        combo->add(label, LLSD(g.id), ADD_BOTTOM, ok);
        found |= (g.id == saved);
    }
    if (!found)
    {
        // The saved card is not here (unplugged eGPU, driver change): show it, keep the setting.
        combo->add("Not found: " + saved + " (Automatic is used)", LLSD(saved), ADD_BOTTOM, true);
    }
    combo->setValue(LLSD(saved.empty() ? std::string("auto") : saved));
}

bool LLFloaterPreferenceGraphicsAdvanced::postBuild()
{
    populate_graphics_card_combo(findChild<LLComboBox>("GraphicsCardSelect"));
    for (const char* name : {"RenderClassicGlow", "RenderGlow", "RenderHDREnabled",
                             "RenderVKVolumetricFog", "RenderVKPhysicalSky"})
        mRtModeChangedSignals.push_back(gSavedSettings.getControl(name)->getSignal()->connect(
            [this](LLControlVariable*, const LLSD&, const LLSD&) { refreshEnabledState(); }));
    for (const char* name : {"RenderVKDisplayAutoPaperWhite", "RenderVKDisplayAutoPeak",
                             "RenderVKDisplayPaperWhiteNits", "RenderVKDisplayPeakNits",
                             "RenderVKDisplayCalibrationGeneration", "RenderVKDisplayHDR", "RenderVKDisplayHDRAvailable"})
        mRtModeChangedSignals.push_back(gSavedSettings.getControl(name)->getSignal()->connect(
            [this](LLControlVariable*, const LLSD&, const LLSD&) { refresh(); }));

#if !LL_DARWIN
    LLCheckBoxCtrl *use_HiDPI = getChild<LLCheckBoxCtrl>("use HiDPI");
    use_HiDPI->setVisible(false);
#endif
#if !LL_LINUX
    // Wayland / X11 choice (LinuxDisplayBackend), Linux only.
    getChildView("DisplayBackendText")->setVisible(false);
    getChildView("DisplayBackend")->setVisible(false);
    getChildView("DisplayBackendRestart")->setVisible(false);
#endif

    mComplexityChangedSignal = gSavedSettings.getControl("RenderAvatarMaxComplexity")->getCommitSignal()->connect(
        [this](LLControlVariable* control, const LLSD& new_val, const LLSD& old_val)
        {
            updateComplexityText();
        });
    mComplexityModeChangedSignal = gSavedSettings.getControl("RenderAvatarComplexityMode")->getSignal()->connect(
        [this](LLControlVariable* control, const LLSD& new_val, const LLSD& old_val)
        {
            updateComplexityMode(new_val);
        });
    mLODFactorChangedSignal = gSavedSettings.getControl("RenderVolumeLODFactor")->getCommitSignal()->connect(
        [this](LLControlVariable* control, const LLSD& new_val, const LLSD& old_val)
        {
            updateObjectMeshDetailText();
        });
    mNumImpostorsChangedSignal = gSavedSettings.getControl("RenderAvatarMaxNonImpostors")->getSignal()->connect(
        [this](LLControlVariable* control, const LLSD& new_val, const LLSD& old_val)
        {
            updateIndirectMaxNonImpostors(new_val);
        });
    // Keep the "N fps" / "Off" label in sync with the slider and with any other writer
    // (debug settings, preset load, Cancel restoring the saved value).
    mBackgroundFPSChangedSignal = gSavedSettings.getControl("BackgroundFPSLimit")->getSignal()->connect(
        [this](LLControlVariable*, const LLSD&, const LLSD&) { updateBackgroundFPSText(); });
    return true;
}

void LLFloaterPreferenceGraphicsAdvanced::onOpen(const LLSD& key)
{
    LLVulkanContext::instance().refreshDisplayCalibration();
    refresh();
}

void LLFloaterPreferenceGraphicsAdvanced::updateForDraw()
{
    if (mVramTextTimer.getElapsedTimeF32() >= 1.f)
    {
        mVramTextTimer.reset();
        updateVideoMemoryText();
        updateNetDrawDistanceText();
        refreshFrameGen();   // #1517 the status line (5 s statistics)
    }
    // #1517 frame generation: the render loop rebuilt the main chain after the setting's signal
    // refreshed the panel (or generation failed, or the HDR output changed): the vsync checkbox follows.
    if (LLVKFrameGen::instance().chainForcesNoVsync() != mFgNoVsyncShown)
        refreshFrameGen();
    // SETTING SPECIALIZATION (llcleanspecvk.cpp): while a changed setting's optimized shaders
    // compile in the background, one line by the buttons says which -- animated dots, and
    // whether the new values already apply (on the generic shaders, after
    // RenderVKSpecApplyTimeoutSec) or the previous ones still render.
    if (LLTextBox* spec = findChild<LLTextBox>("SpecApplyingText"))
    {
        bool fallback = false;
        const std::string what = LLVulkanContext::instance().isInitialized()
                                     ? LLVulkanContext::specStatusText(&fallback) : std::string();
        spec->setVisible(!what.empty());
        if (!what.empty())
        {
            static const char* const kDots[4] = {"", ".", "..", "..."};
            const std::string dots = kDots[(S32)(LLFrameTimer::getElapsedSeconds() * 3.0) & 3];
            spec->setText(fallback ? "Applied; optimizing " + what + dots
                                   : "Applying " + what + dots);
            spec->setToolTip(fallback ? std::string("The new settings already apply; shaders optimized for them are still "
                                                    "compiling in the background (rendering may be a little slower until then).")
                                      : std::string("Shaders optimized for the new settings are compiling in the background; "
                                                    "the previous settings keep rendering until they are ready (a few seconds "
                                                    "at most -- then the new settings apply either way)."));
        }
    }
}

void LLFloaterPreferenceGraphicsAdvanced::draw()
{
    updateForDraw();
    LLFloater::draw();
}

// #405: one VRAM budget -- what it is, what uses it, what the soft ladder holds back right
// now (temporary; the settings above keep the user's choices). Full detail in the tooltip.
void LLFloaterPreferenceGraphicsAdvanced::updateVideoMemoryText()
{
    LLTextBox* box = findChild<LLTextBox>("VideoMemoryStatus");
    if (!box) return;
    if (!LLVulkanContext::instance().isInitialized())
    {
        box->setText(std::string("Video memory: n/a (Vulkan renderer not running)"));
        return;
    }
    box->setText(LLVulkanContext::instance().vramReadoutText());
    box->setToolTip(LLVulkanContext::instance().vramStatusText(2));
}

// #971: the cap is SLNetDrawDistanceMax; what is actually asked of the simulator (AgentUpdate
// Far) can be lower for a while (fast movement, ramp-down), so show both.
void LLFloaterPreferenceGraphicsAdvanced::updateNetDrawDistanceText()
{
    LLTextBox* box = findChild<LLTextBox>("NetDrawDistanceStatus");
    if (!box) return;
    const F32 cap = gSavedSettings.getF32("SLNetDrawDistanceMax");
    F32 now = gAgentCamera.getNetFarSent();
    if (now < 0.f) now = gAgentCamera.getNetworkDrawDistance();
    const F32 eff_cap = (cap > 0.f) ? llclamp(cap, 64.f, 512.f) : 512.f;   // (LLAgentCamera::getNetworkDrawDistance)
    std::string text = llformat("Limit %d m", (S32)(eff_cap + 0.5f));
    text += llformat("; asking the simulator for %d m now", (S32)(now + 0.5f));
    if (gAgentCamera.isNetFastMoving())
    {
        text += " (lowered while moving fast)";
    }
    box->setText(text);
}

void LLFloaterPreferenceGraphicsAdvanced::onClickCloseBtn(bool app_quitting)
{
    LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->cancel({"RenderQualityPerformance"});
    }
    updateMaxComplexity();
}

void LLFloaterPreferenceGraphicsAdvanced::onRenderOptionEnable()
{
    LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->refresh();
    }

    refreshEnabledGraphics();
}

void LLFloaterPreferenceGraphicsAdvanced::onAdvancedAtmosphericsEnable()
{
    LLFloaterPreference* instance = LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->refresh();
    }

    refreshEnabledGraphics();
}

void LLFloaterPreferenceGraphicsAdvanced::refresh()
{
    const auto output = LLVulkanContext::instance().displayHdrInfo();
    gSavedSettings.setBOOL("RenderVKDisplayHDRAvailable", output.displayCapable);
    if (auto* control = findChild<LLCheckBoxCtrl>("DisplayHdrEnabled")) control->setEnabled(output.displayCapable);
    if (auto* label = findChild<LLTextBox>("DisplayHdrStatus"))
        label->setText(LLStringExplicit(llformat("%s\nWhite %.0f nits (%s); peak %.0f nits (%s)",
            output.reason, output.paperWhiteNits, output.whiteSource, output.peakNits, output.peakSource)));
    for (const auto& item : {std::pair{"DisplayHdrPaperWhite", "RenderVKDisplayAutoPaperWhite"},
                             std::pair{"DisplayHdrPeak", "RenderVKDisplayAutoPeak"}})
        if (auto* control = findChild<LLSliderCtrl>(item.first)) {
            control->setVisible(output.displayCapable);
            control->setEnabled(output.displayCapable && output.requested && !gSavedSettings.getBOOL(item.second));
        }
    for (const char* name : {"DisplayHdrAutoPaperWhite", "DisplayHdrAutoPeak"})
        if (auto* control = findChild<LLCheckBoxCtrl>(name)) {
            control->setVisible(output.displayCapable);
            control->setEnabled(output.displayCapable && output.requested);
        }
    // slider text boxes
    updateSliderText(getChild<LLSliderCtrl>("ObjectMeshDetail",     true), getChild<LLTextBox>("ObjectMeshDetailText",      true), true);
    updateSliderText(getChild<LLSliderCtrl>("FlexibleMeshDetail",   true), getChild<LLTextBox>("FlexibleMeshDetailText",    true), true);
    updateSliderText(getChild<LLSliderCtrl>("TreeMeshDetail",       true), getChild<LLTextBox>("TreeMeshDetailText",        true), true);
    updateSliderText(getChild<LLSliderCtrl>("AvatarMeshDetail",     true), getChild<LLTextBox>("AvatarMeshDetailText",      true));
    updateSliderText(getChild<LLSliderCtrl>("AvatarPhysicsDetail",  true), getChild<LLTextBox>("AvatarPhysicsDetailText",       true));
    updateSliderText(getChild<LLSliderCtrl>("TerrainMeshDetail",    true), getChild<LLTextBox>("TerrainMeshDetailText",     true), true);
    LLAvatarComplexityControls::setIndirectControls();
    setMaxNonImpostorsText(
        gSavedSettings.getU32("RenderAvatarMaxNonImpostors"),
        getChild<LLTextBox>("IndirectMaxNonImpostorsText", true));
    LLAvatarComplexityControls::setText(
        gSavedSettings.getU32("RenderAvatarMaxComplexity"),
        getChild<LLTextBox>("IndirectMaxComplexityText", true));
    updateBackgroundFPSText();
    updateVideoMemoryText();
    updateNetDrawDistanceText();
    refreshEnabledState();

    bool enable_complexity = gSavedSettings.getS32("RenderAvatarComplexityMode") != LLVOAvatar::AV_RENDER_ONLY_SHOW_FRIENDS;
    getChild<LLSliderCtrl>("IndirectMaxComplexity")->setEnabled(enable_complexity);
    getChild<LLSliderCtrl>("IndirectMaxNonImpostors")->setEnabled(enable_complexity);
}

void LLFloaterPreferenceGraphicsAdvanced::refreshEnabledGraphics()
{
    refreshEnabledState();
}

void LLFloaterPreferenceGraphicsAdvanced::updateMaxComplexity()
{
    // Called when the IndirectMaxComplexity control changes
    LLAvatarComplexityControls::updateMax(
        getChild<LLSliderCtrl>("IndirectMaxComplexity"),
        getChild<LLTextBox>("IndirectMaxComplexityText"));
}

void LLFloaterPreferenceGraphicsAdvanced::updateComplexityMode(const LLSD& newvalue)
{
    bool enable_complexity = newvalue.asInteger() != LLVOAvatar::AV_RENDER_ONLY_SHOW_FRIENDS;
    getChild<LLSliderCtrl>("IndirectMaxComplexity")->setEnabled(enable_complexity);
    getChild<LLSliderCtrl>("IndirectMaxNonImpostors")->setEnabled(enable_complexity);
}

void LLFloaterPreferenceGraphicsAdvanced::updateComplexityText()
{
    LLAvatarComplexityControls::setText(gSavedSettings.getU32("RenderAvatarMaxComplexity"),
        getChild<LLTextBox>("IndirectMaxComplexityText", true));
}

void LLFloaterPreferenceGraphicsAdvanced::updateObjectMeshDetailText()
{
    updateSliderText(getChild<LLSliderCtrl>("ObjectMeshDetail", true), getChild<LLTextBox>("ObjectMeshDetailText", true), true);
}

void LLFloaterPreferenceGraphicsAdvanced::updateBackgroundFPSText()
{
    LLTextBox* text_box = getChild<LLTextBox>("BackgroundFPSLimitText", true);
    const S32 fps = gSavedSettings.getS32("BackgroundFPSLimit");
    if (fps <= 0)
    {
        text_box->setText(getString("BackgroundFPSOff"));
    }
    else
    {
        LLStringUtil::format_map_t args;
        args["[FPS]"] = llformat("%d", fps);
        text_box->setText(getString("BackgroundFPSValue", args));
    }
}

void LLFloaterPreferenceGraphicsAdvanced::updateSliderText(LLSliderCtrl* ctrl, LLTextBox* text_box, bool has_ultra)
{
    if (text_box == NULL || ctrl== NULL)
        return;

    // get range and points when text should change
    F32 value = (F32)ctrl->getValue().asReal();
    F32 min = ctrl->getMinValue();
    F32 max = ctrl->getMaxValue();
    // The upper half of an Ultra slider is Ultra; Low/Mid/High keep the bands of the lower half.
    if (has_ultra)
    {
        max = min + (max - min) * 0.5f;
    }
    F32 range = max - min;
    llassert(range > 0);
    F32 midPoint = min + range / 3.0f;
    F32 highPoint = min + (2.0f * range / 3.0f);

    // choose the right text
    if (has_ultra && value > max)
    {
        text_box->setText(LLTrans::getString("GraphicsQualityUltra"));
    }
    else if (value < midPoint)
    {
        text_box->setText(LLTrans::getString("GraphicsQualityLow"));
    }
    else if (value < highPoint)
    {
        text_box->setText(LLTrans::getString("GraphicsQualityMid"));
    }
    else
    {
        text_box->setText(LLTrans::getString("GraphicsQualityHigh"));
    }
}

void LLFloaterPreferenceGraphicsAdvanced::updateMaxNonImpostors()
{
    // Called when the IndirectMaxNonImpostors control changes
    // Responsible for fixing the slider label (IndirectMaxNonImpostorsText) and setting RenderAvatarMaxNonImpostors
    LLSliderCtrl* ctrl = getChild<LLSliderCtrl>("IndirectMaxNonImpostors",true);
    U32 value = ctrl->getValue().asInteger();

    if (0 == value || LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER <= value)
    {
        value=0;
    }
    gSavedSettings.setU32("RenderAvatarMaxNonImpostors", value);
    LLVOAvatar::updateImpostorRendering(value); // make it effective immediately
    setMaxNonImpostorsText(value, getChild<LLTextBox>("IndirectMaxNonImpostorsText"));
}

void LLFloaterPreferenceGraphicsAdvanced::updateIndirectMaxNonImpostors(const LLSD& newvalue)
{
    U32 value = newvalue.asInteger();
    if ((value != 0) && (value != gSavedSettings.getU32("IndirectMaxNonImpostors")))
    {
        gSavedSettings.setU32("IndirectMaxNonImpostors", value);
    }
    setMaxNonImpostorsText(value, getChild<LLTextBox>("IndirectMaxNonImpostorsText"));
}

void LLFloaterPreferenceGraphicsAdvanced::setMaxNonImpostorsText(U32 value, LLTextBox* text_box)
{
    if (0 == value)
    {
        text_box->setText(LLTrans::getString("no_limit"));
    }
    else
    {
        text_box->setText(llformat("%d", value));
    }
}

void LLFloaterPreferenceGraphicsAdvanced::disableUnavailableSettings()
{
    LLComboBox* ctrl_shadows = getChild<LLComboBox>("ShadowDetail");
    LLTextBox* shadows_text = getChild<LLTextBox>("RenderShadowDetailText");
    LLCheckBoxCtrl* ctrl_ssao = getChild<LLCheckBoxCtrl>("UseSSAO");
    LLCheckBoxCtrl* ctrl_dof = getChild<LLCheckBoxCtrl>("UseDoF");
    LLSliderCtrl* cas_slider = getChild<LLSliderCtrl>("RenderSharpness");

    // disabled windlight
    if (!LLFeatureManager::getInstance()->isFeatureAvailable("WindLightUseAtmosShaders"))
    {
        //deferred needs windlight, disable deferred
        ctrl_shadows->setEnabled(false);
        ctrl_shadows->setValue(0);
        shadows_text->setEnabled(false);

        ctrl_ssao->setEnabled(false);
        ctrl_ssao->setValue(false);

        ctrl_dof->setEnabled(false);
        ctrl_dof->setValue(false);
    }

    // disabled deferred
    if (!LLFeatureManager::getInstance()->isFeatureAvailable("RenderDeferred"))
    {
        ctrl_shadows->setEnabled(false);
        ctrl_shadows->setValue(0);
        shadows_text->setEnabled(false);

        ctrl_ssao->setEnabled(false);
        ctrl_ssao->setValue(false);

        ctrl_dof->setEnabled(false);
        ctrl_dof->setValue(false);
    }

    // disabled deferred SSAO
    if (!LLFeatureManager::getInstance()->isFeatureAvailable("RenderDeferredSSAO"))
    {
        ctrl_ssao->setEnabled(false);
        ctrl_ssao->setValue(false);
    }

    // disabled deferred shadows
    if (!LLFeatureManager::getInstance()->isFeatureAvailable("RenderShadowDetail"))
    {
        ctrl_shadows->setEnabled(false);
        ctrl_shadows->setValue(0);
        shadows_text->setEnabled(false);
    }

    // Vintage mode
    static LLCachedControl<bool> scene_hdr_enabled(gSavedSettings, "RenderHDREnabled");
    LLSliderCtrl*         tonemapMix    = getChild<LLSliderCtrl>("TonemapMix");
    LLComboBox*           tonemapSelect = getChild<LLComboBox>("TonemapType");
    LLTextBox*            tonemapLabel  = getChild<LLTextBox>("TonemapTypeText");
    LLSliderCtrl*         exposureSlider = getChild<LLSliderCtrl>("RenderExposure");

    tonemapSelect->setEnabled(scene_hdr_enabled);
    tonemapLabel->setEnabled(scene_hdr_enabled);
    tonemapMix->setEnabled(scene_hdr_enabled);
    exposureSlider->setEnabled(scene_hdr_enabled);
    // Sharpening runs in both modes (the VK final pass / FSR's RCAS; upstream applyCAS).
    cas_slider->setEnabled(true);

    LLComboBox* ctrl_anisotropic = getChild<LLComboBox>("anisotropic_filter");
    if (!LLFeatureManager::instance().isFeatureAvailable("RenderAnisotropicLevel"))
    {
        ctrl_anisotropic->setEnabled(false);
    }
}

// Ray-traced lighting controls: enabled only while their pipeline can run; greyed otherwise with
// the first unmet requirement appended to the XUI tooltip (cleared again once it is met). The
// settings themselves are never changed here.
static void gate_rt_control(LLView* root, const char* ctrl_name, const char* label_name,
                            const std::string& why_not)
{
    static std::map<std::string, std::string> s_base_tips;   // XUI tooltips, captured unmodified
    LLUICtrl* ctrl = root->findChild<LLUICtrl>(ctrl_name);
    if (!ctrl) return;
    auto it = s_base_tips.find(ctrl_name);
    if (it == s_base_tips.end()) it = s_base_tips.emplace(ctrl_name, ctrl->getToolTip()).first;
    const bool ok = why_not.empty();
    ctrl->setEnabled(ok);
    ctrl->setToolTip(ok ? it->second : it->second + "\n\nUnavailable: " + why_not + ".");
    if (label_name)
    {
        if (LLTextBox* t = root->findChild<LLTextBox>(label_name)) t->setEnabled(ok);
    }
}

// The "Ray-traced lighting" section (GI, ReSTIR DI, stochastic / merged RT shadows) and the
// reflection light grid. shadows_ok: the feature table allows shadows at all.
static void refresh_rt_lighting_state(LLView* root, bool shadows_ok)
{
    LLVulkanContext& vk = LLVulkanContext::instance();
    const bool rt_ok = vk.rayTracingEnabled();
    const bool rt_hw = vk.rayQuerySupported();
    const bool rt_wanted = gSavedSettings.getU32("RenderVKRayTracing") != 0u;

    // Why every RT option is greyed, and the section's one-line status (RenderVKRayTracing is
    // read at device creation: the running state can differ from the setting until a restart).
    std::string rt_why, status;
    if (!rt_hw)
    {
        rt_why = "this graphics card has no hardware ray tracing (" + vk.rayTracingStatus() + ")";
        status = "Not available on this graphics card.";
    }
    else if (!rt_ok)
    {
        rt_why = rt_wanted ? "hardware ray tracing turns on after a restart"
                           : "turn on Hardware ray tracing (General tab, Hardware) and restart the viewer";
        status = rt_wanted ? "Restart the viewer to turn ray tracing on." : "Ray tracing is off.";
    }
    else if (!rt_wanted)
    {
        status = "Ray tracing turns off after a restart.";
    }
    if (LLTextBox* t = root->findChild<LLTextBox>("RtLightingStatus")) t->setText(status);
    gate_rt_control(root, "RayTracingEnabled", nullptr,
                    rt_hw ? std::string() : "this graphics card has no hardware ray tracing ("
                                            + vk.rayTracingStatus() + ")");

    // GI and the stochastic shadow pass run in the split deferred lighting (on by default; only
    // Debug Settings turn it off).
    const bool defer_ok = gSavedSettings.getU32("RenderVKDeferredSplit") != 0u;
    const std::string defer_why = "needs the split deferred lighting (RenderVKDeferredSplit in Debug Settings)";
    const S32 detail = gSavedSettings.getS32("RenderShadowDetail");
    const bool gi_on = gSavedSettings.getU32("RenderVKGI") != 0u;
    const bool proj_rt = shadows_ok && detail >= 2 && gSavedSettings.getU32("RenderVKProjectorShadows") == 2u;
    const bool sun_rt = shadows_ok && detail >= 1 && gSavedSettings.getU32("RenderVKSunShadows") == 2u;
    const bool stoch_on = gSavedSettings.getU32("RenderVKProjStochastic") != 0u;

    // First unmet requirement in order (empty = all met).
    auto first_unmet = [](std::initializer_list<std::pair<bool, std::string>> reqs)
    {
        for (const auto& r : reqs)
            if (!r.first) return r.second;
        return std::string();
    };
    const std::pair<bool, std::string> need_rt{rt_ok, rt_why};
    const std::pair<bool, std::string> need_defer{defer_ok, defer_why};
    const std::pair<bool, std::string> need_proj_rt{
        proj_rt, "needs Shadows: Sun/Moon + Projectors and Projector shadows: Ray traced"};
    const std::pair<bool, std::string> need_stoch{stoch_on, "needs Stochastic projector shadows on"};

    const std::string gi_why = first_unmet({need_rt, need_defer});
    gate_rt_control(root, "GlobalIllumination", "GIText", gi_why);
    const std::string gi_opt_why =
        first_unmet({need_rt, need_defer, {gi_on, "needs Global illumination set to Low or higher"}});
    gate_rt_control(root, "GIScale", "GIScaleText", gi_opt_why);
    gate_rt_control(root, "GITraceEvery", "GITraceEveryText", gi_opt_why);

    gate_rt_control(root, "ProjStochastic", nullptr, first_unmet({need_rt, need_defer, need_proj_rt}));
    const std::string stoch_opt_why = first_unmet({need_rt, need_defer, need_proj_rt, need_stoch});
    gate_rt_control(root, "ProjStochasticSamples", "ProjStochasticSamplesText", stoch_opt_why);
    gate_rt_control(root, "ProjStochAdaptive", "ProjStochAdaptiveText", stoch_opt_why);
    gate_rt_control(root, "ProjStochAdaptiveSamples", "ProjStochAdaptiveSamplesText", stoch_opt_why);
    gate_rt_control(root, "RTShadowMerge", nullptr,
                    first_unmet({need_rt, need_defer, need_proj_rt, need_stoch,
                                 {sun_rt, "needs Sun/moon shadows: Ray traced"}}));

    // Reflections tab: the light grid only shades the ray-traced reflection hits.
    gate_rt_control(root, "RTReflLightGrid", nullptr,
                    first_unmet({need_rt, {gSavedSettings.getU32("RenderVKRTReflections") == 1u,
                                           "needs Ray-traced reflection fallback on"}}));
}

void LLFloaterPreferenceGraphicsAdvanced::refreshEnabledState()
{
    // Classic glow takes precedence, without changing the saved bloom choice.
    const bool bloom_available = gSavedSettings.getBOOL("RenderHDREnabled") &&
                                 !gSavedSettings.getBOOL("RenderClassicGlow");
    const bool bloom_controls = bloom_available && gSavedSettings.getBOOL("RenderGlow");
    if (LLView* bloom = findChild<LLView>("Bloom")) bloom->setEnabled(bloom_available);
    for (const char* name : {"BloomStrength", "BloomThreshold", "BloomSatPreserve", "BloomHueHighlights"})
        if (LLView* control = findChild<LLView>(name)) control->setEnabled(bloom_controls);

    bool enabled = true;

    LLCheckBoxCtrl* ctrl_ssao = getChild<LLCheckBoxCtrl>("UseSSAO");
    LLCheckBoxCtrl* ctrl_dof = getChild<LLCheckBoxCtrl>("UseDoF");
    LLComboBox* ctrl_shadow = getChild<LLComboBox>("ShadowDetail");
    LLTextBox* shadow_text = getChild<LLTextBox>("RenderShadowDetailText");

    // note, okay here to get from ctrl_deferred as it's twin, ctrl_deferred2 will alway match it
    enabled = enabled && LLFeatureManager::getInstance()->isFeatureAvailable("RenderDeferredSSAO");// && ctrl_deferred->get();

    //ctrl_deferred->set(gSavedSettings.getBOOL("RenderDeferred"));

    ctrl_ssao->setEnabled(enabled);
    ctrl_dof->setEnabled(enabled);

    enabled = enabled && LLFeatureManager::getInstance()->isFeatureAvailable("RenderShadowDetail");

    ctrl_shadow->setEnabled(enabled);
    shadow_text->setEnabled(enabled);

    // Sun / moon shadow technique (Vulkan, #185): needs sun shadows in LL's Shadows combo;
    // "Ray traced" only when the GPU exposes hardware ray queries (else it falls back to
    // shadow maps anyway).
    if (LLComboBox* ctrl_sun = findChild<LLComboBox>("SunShadows"))
    {
        const bool sun_ok = enabled && gSavedSettings.getS32("RenderShadowDetail") >= 1;
        ctrl_sun->setEnabled(sun_ok);
        if (LLTextBox* t = findChild<LLTextBox>("SunShadowsText")) t->setEnabled(sun_ok);
        const bool rt_ok = LLVulkanContext::instance().rayTracingEnabled();
        ctrl_sun->setEnabledByValue(LLSD(2), rt_ok);
        if (!rt_ok)
        {
            static const std::string base_tip = ctrl_sun->getToolTip();
            ctrl_sun->setToolTip(base_tip + "\n\nRay traced unavailable: " +
                                 LLVulkanContext::instance().rayTracingStatus());
        }
    }

    // Hybrid ray-traced reflections (#859, RenderVKRTReflections): only with hardware ray
    // queries; greyed with the reason otherwise (the setting itself is left alone).
    if (LLCheckBoxCtrl* ctrl_rtrefl = findChild<LLCheckBoxCtrl>("RTReflections"))
    {
        const bool rt_ok = LLVulkanContext::instance().rayTracingEnabled();
        ctrl_rtrefl->setEnabled(rt_ok);
        if (!rt_ok)
        {
            static const std::string base_tip = ctrl_rtrefl->getToolTip();
            ctrl_rtrefl->setToolTip(base_tip + "\n\nRay traced unavailable: " +
                                    LLVulkanContext::instance().rayTracingStatus());
        }
    }

    // Projector shadows (Vulkan): need projector shadows enabled in LL's Shadows combo;
    // "Ray traced" only when the GPU exposes hardware ray queries.
    if (LLComboBox* ctrl_proj = findChild<LLComboBox>("ProjectorShadows"))
    {
        const bool proj_ok = enabled && gSavedSettings.getS32("RenderShadowDetail") >= 2;
        ctrl_proj->setEnabled(proj_ok);
        if (LLTextBox* t = findChild<LLTextBox>("ProjectorShadowsText")) t->setEnabled(proj_ok);
        const bool rt_ok = LLVulkanContext::instance().rayTracingEnabled();
        ctrl_proj->setEnabledByValue(LLSD(2), rt_ok);
        if (!rt_ok)
        {
            // XUI tooltip + why "Ray traced" is greyed (captured once; refresh reruns).
            static const std::string base_tip = ctrl_proj->getToolTip();
            ctrl_proj->setToolTip(base_tip + "\n\nRay traced unavailable: " +
                                  LLVulkanContext::instance().rayTracingStatus());
        }
    }

    // The RT shadow resolution (#195: the RT mask, and the stochastic denoiser, which needs
    // ray-traced projector shadows) is meaningful only when something is ray traced.
    if (LLComboBox* ctrl_rtres = findChild<LLComboBox>("RtShadowRes"))
    {
        const S32 detail = gSavedSettings.getS32("RenderShadowDetail");
        const bool traced = (detail >= 1 && gSavedSettings.getU32("RenderVKSunShadows") == 2u)
                         || (detail >= 2 && gSavedSettings.getU32("RenderVKProjectorShadows") == 2u);
        const bool on = enabled && traced && LLVulkanContext::instance().rayTracingEnabled();
        ctrl_rtres->setEnabled(on);
        if (LLTextBox* t = findChild<LLTextBox>("RtShadowResText")) t->setEnabled(on);
    }

    // RT disc sampling is fixed at one ray, so cascade quality only applies to shadow maps.
    // Keep its saved value untouched while an eligible RT sun/moon or projector path is selected.
    if (LLComboBox* ctrl_quality = findChild<LLComboBox>("ShadowQuality"))
    {
        const S32 detail = gSavedSettings.getS32("RenderShadowDetail");
        const bool rt_selected = (detail >= 1 && gSavedSettings.getU32("RenderVKSunShadows") == 2u)
                              || (detail >= 2 && gSavedSettings.getU32("RenderVKProjectorShadows") == 2u);
        const bool rt_active = enabled && rt_selected && LLVulkanContext::instance().rayTracingEnabled();
        static const std::string base_tip = ctrl_quality->getToolTip();
        ctrl_quality->setEnabled(enabled && !rt_active);
        ctrl_quality->setToolTip(rt_active ? "Ray-traced shadows use a fixed sample count" : base_tip);
        if (LLTextBox* t = findChild<LLTextBox>("ShadowQualityText")) t->setEnabled(enabled && !rt_active);
    }

    // Ray-traced lighting section (GI, stochastic / merged RT shadows).
    refresh_rt_lighting_state(this, enabled);

    // God rays: replaced by volumetric fog and the physical sky, so greyed (values kept) with
    // the reason while either is on. The same predicate drives Photo Studio and Camera mode.
    {
        const bool godrays_ok = SLGodRays::availableFromSettings();
        for (const char* name : {"GodRaysText", "GodRays", "GodRaysIntensity"})
        {
            LLView* view = findChild<LLView>(name);
            if (!view) continue;
            static std::map<std::string, std::string> base_tips;   // the XUI tooltips, captured once
            const auto base = base_tips.emplace(name, view->getToolTip()).first;
            view->setEnabled(godrays_ok);
            view->setToolTip(godrays_ok ? base->second : std::string(SLGodRays::unavailableTip()));
        }
    }

    // Hardware settings

    if (!LLFeatureManager::getInstance()->isFeatureAvailable("RenderVBOEnable"))
    {
        getChildView("vbo")->setEnabled(false);
    }

    // Texture compression (RenderVKTextureCompression, #263): GPU block compression needs
    // the device's BC formats (every desktop GPU); live, no restart.
    {
        const bool bc = LLVulkanContext::instance().textureCompressionBC();
        getChildView("texture compression")->setEnabled(bc);
        getChildView("TextureCompressionText")->setEnabled(bc);
    }

    getChildView("antialiasing restart")->setVisible(!LLFeatureManager::getInstance()->isFeatureAvailable("RenderDeferred"));

    LLComboBox* af_combo = getChild<LLComboBox>("anisotropic_filter");
    if (2.f > gGPUInfo.mMaxAnisotropy) {
        af_combo->remove("2x");
    }
    if (4.f > gGPUInfo.mMaxAnisotropy) {
        af_combo->remove("4x");
    }
    if (8.f > gGPUInfo.mMaxAnisotropy) {
        af_combo->remove("8x");
    }
    if (16.f > gGPUInfo.mMaxAnisotropy) {
        af_combo->remove("16x");
    }

    // Antialiasing Quality is the FXAA / SMAA preset; the temporal resolve (FSR / DLSS, picked in the
    // upscaler list -- an old preset's TAA value 3 runs as FSR Native AA, llups::effectiveMode) replaces
    // that pass entirely.
    {
        // An active upscaler is temporal by itself, so the Antialiasing type is moot too.
        const U32 aa_type = gSavedSettings.getU32("RenderFSAAType");
        const bool upscaler = gSavedSettings.getU32("RenderVKUpscaler") != 0;
        const bool taa = upscaler || aa_type == 3;
        const bool quality_used = (aa_type == 1 || aa_type == 2) && !taa;
        LLComboBox* aa_combo = getChild<LLComboBox>("fsaa");
        // Custom render scale (#410, RenderVKRenderScale) only applies with the upscaler on FSR Custom (6).
        if (LLSliderCtrl* scale = findChild<LLSliderCtrl>("RenderScale"))
        {
            scale->setEnabled(gSavedSettings.getU32("RenderVKUpscaler") == 6);
        }
        // #870 / #962: the DLSS choices (7-11) need the user-installed shim, found at startup:
        // greyed out with the reason otherwise (a selected one runs the FSR preset).
        const LLVKNeuralRecon& nr = LLVKNeuralRecon::instance();
        if (LLComboBox* up = findChild<LLComboBox>("UpscaleMode"))
        {
            static std::string s_tip;
            if (s_tip.empty()) s_tip = up->getToolTip();
            for (S32 v = 7; v <= 11; ++v) up->setEnabledByValue(LLSD(v), nr.available());
            std::string tip = s_tip;
            if (!nr.available())
            {
                LLStringUtil::format_map_t args;
                args["[REASON]"] = nr.reason().empty() ? std::string("not loaded") : nr.reason();
                tip += "\n\n" + getString("upscale_rr_unavailable", args);
            }
            up->setToolTip(tip);
        }
        // #962 Ray Reconstruction Auto / On / Off: only with a DLSS mode; On needs the RR library,
        // Off the SR one (greyed with the reason when missing), Auto either.
        if (LLComboBox* rr = findChild<LLComboBox>("DlssRayReconstruction"))
        {
            static std::string s_rr_tip;
            if (s_rr_tip.empty()) s_rr_tip = rr->getToolTip();
            const U32 up_mode = gSavedSettings.getU32("RenderVKUpscaler");
            const bool dlss = up_mode >= 7 && up_mode <= 11 && nr.available();
            const bool rr_ok = nr.available(LLVKNeuralRecon::F_RR), sr_ok = nr.available(LLVKNeuralRecon::F_SR);
            rr->setEnabled(dlss);
            if (LLTextBox* label = findChild<LLTextBox>("DlssRayReconstructionText")) label->setEnabled(dlss);
            rr->setEnabledByValue(LLSD(1), rr_ok);
            rr->setEnabledByValue(LLSD(0), sr_ok);
            rr->setEnabledByValue(LLSD(2), rr_ok || sr_ok);
            std::string tip = s_rr_tip;
            if (!dlss) tip += "\n\n" + getString("dlss_rr_needs_dlss");
            if (nr.available() && !rr_ok)
            {
                LLStringUtil::format_map_t args;
                args["[REASON]"] = nr.reason(LLVKNeuralRecon::F_RR);
                tip += "\n\n" + getString("dlss_rr_unavailable", args);
            }
            if (nr.available() && !sr_ok)
            {
                LLStringUtil::format_map_t args;
                args["[REASON]"] = nr.reason(LLVKNeuralRecon::F_SR);
                tip += "\n\n" + getString("dlss_sr_unavailable", args);
            }
            rr->setToolTip(tip);
        }
        // Denoiser quality (RenderVKDenoiseAtrous): the renderer forces one a-trous iteration under DLSS
        // Ray Reconstruction. On is certain to run it; Auto picks per frame, so it stays enabled there.
        if (LLComboBox* dn = findChild<LLComboBox>("DenoiseAtrous"))
        {
            static std::string s_dn_tip;
            if (s_dn_tip.empty()) s_dn_tip = dn->getToolTip();
            const U32 up_mode = gSavedSettings.getU32("RenderVKUpscaler");
            const bool rr_on = up_mode >= 7 && up_mode <= 11 && nr.available(LLVKNeuralRecon::F_RR)
                            && gSavedSettings.getU32("RenderVKDlssRayReconstruction") == 1u;
            dn->setEnabled(!rr_on);
            if (LLTextBox* label = findChild<LLTextBox>("DenoiseAtrousText")) label->setEnabled(!rr_on);
            dn->setToolTip(rr_on ? s_dn_tip + "\n\n" + getString("denoise_by_rr") : s_dn_tip);
        }
        refreshFrameGen();   // #1517 frame generation and the vsync checkbox
        aa_combo->setEnabled(!upscaler);
        aa_combo->setToolTip(upscaler ? getString("aa_by_upscaler") : LLStringUtil::null);
        getChild<LLTextBox>("antialiasing label")->setEnabled(!upscaler);
        getChild<LLComboBox>("fsaa quality")->setEnabled(quality_used);
        getChild<LLTextBox>("antialiasing quality label")->setEnabled(quality_used);
    }

    // now turn off any features that are unavailable
    disableUnavailableSettings();
}

// #1517 DLSS Frame Generation and the vsync checkbox: from refreshEnabledState, and from updateForDraw
// while the floater is open (the render loop rebuilds the chain after a setting's signal ran
// refreshEnabledState; the status line changes every 5 s).
void LLFloaterPreferenceGraphicsAdvanced::refreshFrameGen()
{
    // #1517 DLSS Frame Generation (Off / 2x / 3x / 4x): the DLSS add-on generates, the viewer presents;
    // live, no restart. The choice is usable when it was set up at startup (directReady()); else
    // greyed, the tooltip says why. Multipliers above what this GPU can do are greyed; 5x and 6x are
    // listed only while the driver paces the presents (LLVKFrameGen::multiplierOffered).
    // The vsync checkbox: greyed while the main chain in use was built for frame generation (it
    // presents without vsync).
    mFgNoVsyncShown = LLVKFrameGen::instance().chainForcesNoVsync();   // (updateForDraw: the chain rebuilt since)
    if (LLCheckBoxCtrl* vs = findChild<LLCheckBoxCtrl>("vsync"))
    {
        static std::string s_vs_tip;
        if (s_vs_tip.empty()) s_vs_tip = vs->getToolTip();
        const bool fg_no_vsync = LLVKFrameGen::instance().chainForcesNoVsync();
        vs->setEnabled(!fg_no_vsync);
        vs->setToolTip(fg_no_vsync ? s_vs_tip + "\n\n" + getString("vsync_framegen") : s_vs_tip);
    }
    // #1544: which generator runs (RenderVKFrameGenGenerator, live): Automatic, and DLSS / FSR only where they can run
    // on this GPU. A saved choice that cannot run here shows as Automatic (frame generation then says why).
    if (LLComboBox* gc = findChild<LLComboBox>("FrameGenGenerator"))
    {
        const LLVKFrameGen& fg = LLVKFrameGen::instance();
        gc->setEnabled(fg.directReady());
        if (LLTextBox* label = findChild<LLTextBox>("FrameGenGeneratorText")) label->setEnabled(fg.directReady());
        const bool dlss = fg.generatorCanRun(llfg::Generator::Dlss), fsr = fg.generatorCanRun(llfg::Generator::Fsr);
        bool same = gc->getItemCount() == 1 + (dlss ? 1 : 0) + (fsr ? 1 : 0);
        same = same && (gc->findItemByValue("1") != nullptr) == dlss && (gc->findItemByValue("2") != nullptr) == fsr;
        if (!same)
        {
            // (the first item, Automatic, is the XUI's)
            while (gc->getItemCount() > 1) gc->remove(gc->getItemCount() - 1);
            if (dlss) gc->add("DLSS", LLSD(1));
            if (fsr) gc->add("FSR", LLSD(2));
        }
        const U32 asked = gSavedSettings.getU32("RenderVKFrameGenGenerator");
        const S32 show = gc->findItemByValue(llformat("%u", asked)) ? (S32)asked : 0;
        if (gc->getValue().asInteger() != show) gc->setSelectedByValue(LLSD(show), true);
    }
    if (LLComboBox* fgc = findChild<LLComboBox>("FrameGen"))
    {
        static std::string s_fg_tip;
        if (s_fg_tip.empty()) s_fg_tip = fgc->getToolTip();
        const LLVKFrameGen& fg = LLVKFrameGen::instance();
        LLTextBox* label = findChild<LLTextBox>("FrameGenText");
        LLStringUtil::format_map_t args;
        fgc->setEnabled(fg.directReady());
        if (label) label->setEnabled(fg.directReady());
        // The multipliers listed are exactly those the generator in use can run now (multiplierOffered): not
        // greyed, not there. FSR: Off and 2x (#1544); 5x and 6x only while the driver paces the presents (and the
        // GPU generates that many). A saved higher multiplier runs as the highest offered one and shows as it;
        // the setting is kept. (Off is always there.)
        {
            std::vector<S32> offered;
            for (S32 m = 2; m <= 6; ++m)
                if (fg.multiplierOffered((U32)m)) offered.push_back(m);
            bool same = fgc->getItemCount() == (S32)offered.size() + 1;
            for (size_t i = 0; same && i < offered.size(); ++i)
                same = fgc->findItemByValue(llformat("%d", offered[i])) != nullptr;
            if (!same)
            {
                // (the first item, Off, is the XUI's; the multipliers after it are listed again in order)
                while (fgc->getItemCount() > 1) fgc->remove(fgc->getItemCount() - 1);
                for (S32 m : offered) fgc->add(llformat("%dx", m), LLSD(m));
            }
            const U32 asked = gSavedSettings.getU32("RenderVKFrameGen");
            const S32 top = offered.empty() ? -1 : offered.back();
            const S32 show = asked < 2 ? 0 : fgc->findItemByValue(llformat("%u", asked)) ? (S32)asked : top;
            if (show >= 0 && fgc->getValue().asInteger() != show) fgc->setSelectedByValue(LLSD(show), true);
        }
        std::string tip = s_fg_tip;
        if (!fg.directReady())
        {
            // (reason() is the block's text with its detail; the block's text alone if it is empty)
            const llfg::Block block = fg.startupBlock();
            if (block == llfg::Block::Platform)
                tip += "\n\n" + getString("framegen_platform");
            else
            {
                args["[REASON]"] = !fg.reason().empty() ? fg.reason()
                                 : block != llfg::Block::None ? std::string(llfg::blockText(block))
                                                               : std::string("not set up this session");
                tip += "\n\n" + getString("framegen_unavailable", args);
            }
        }
        else if (fg.stopped())
        {
            // A failure in mid-session: generation is off until the viewer starts again, whatever the combo
            // says. The plain line (what happened, what brings it back), shown for every choice.
            tip += "\n\n" + fg.reason();
        }
        else if (gSavedSettings.getU32("RenderVKFrameGen") >= 2)
        {
            // The generator in use and its multiplier now ("FSR 2x", "DLSS 3x"; the name alone while none are
            // generated), the last 5 s: frames shown per rendered frame and the status ("ok", or why no frames
            // were generated), then how the frames are paced.
            std::string status = fg.generatedNow() ? llfg::statusPrefix(fg.generator(), fg.generatedNow())
                                                   : std::string(llfg::generatorName(fg.generator()));
            if (fg.shownPerRendered() > 0.f)
                status += llformat(", %.2f frames shown per rendered frame", fg.shownPerRendered());
            if (!fg.statusText().empty())
                status += (status.empty() ? "" : ", ") + fg.statusText();
            if (fg.driverPacing()) status += (status.empty() ? "" : ", ") + std::string("driver pacing");
            if (*fg.cappedMultiplierText()) status += (status.empty() ? "" : ", ") + std::string(fg.cappedMultiplierText());
            if (fg.reflexCapsFrames()) status += (status.empty() ? "" : ", ") + std::string("Reflex");
            if (!status.empty())
            {
                args["[STATUS]"] = status;
                tip += "\n\n" + getString("framegen_status", args);
            }
        }
        fgc->setToolTip(tip);
    }
}

void LLFloaterPreferenceGraphicsAdvanced::onBtnOK(const LLSD& userdata)
{
    LLFloaterPreference* instance = LLFloaterReg::getTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->onBtnOK(userdata);
    }
}

void LLFloaterPreferenceGraphicsAdvanced::onBtnCancel(const LLSD& userdata)
{
    LLFloaterPreference* instance = LLFloaterReg::getTypedInstance<LLFloaterPreference>("preferences");
    if (instance)
    {
        instance->onBtnCancel(userdata);
    }
}
