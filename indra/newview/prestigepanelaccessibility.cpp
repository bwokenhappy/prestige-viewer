/**
 * @file prestigepanelaccessibility.cpp
 * @brief Preferences > Accessibility: bigger text, reduced motion, high contrast colours and
 *        a dyslexia-friendly font in one place.
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

#include "prestigepanelaccessibility.h"

#include "llcheckboxctrl.h"
#include "lldir.h"
#include "llfile.h"
#include "llviewercontrol.h"

static LLPanelInjector<PrestigePanelAccessibility> t_prestige_accessibility("prestige_panel_accessibility");

namespace
{
// The OpenDyslexic font set shipped in the viewer's fonts folder (source: newview/fontsets).
const char* const DYSLEXIC_FONT_SET = "fonts_opendyslexic.xml";
// The default font set, used again when the dyslexia-friendly font is turned off.
const char* const DEFAULT_FONT_SET = "fonts.xml";
} // namespace

bool PrestigePanelAccessibility::postBuild()
{
    LLPanelPreference::postBuild();

    mDyslexicFontCheck = getChild<LLCheckBoxCtrl>("dyslexic_font_check");
    const std::string font_set = gDirUtilp->add(gDirUtilp->getAppRODataDir(), "fonts", DYSLEXIC_FONT_SET);
    if (!LLFile::isfile(font_set))
    {
        // not installed with this build: say so instead of offering a switch that does nothing
        mDyslexicFontCheck->setEnabled(false);
        getChildView("dyslexic_font_missing")->setVisible(true);
        return true;
    }

    mDyslexicFontCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDyslexicFontCommit(); });
    // the font can also be picked in Preferences > Skins > Font
    if (LLControlVariable* control = gSavedSettings.getControl("FSFontSettingsFile"))
    {
        mFontSetConnection = control->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) { refreshDyslexicFont(); });
    }
    refreshDyslexicFont();
    return true;
}

void PrestigePanelAccessibility::refreshDyslexicFont()
{
    mDyslexicFontCheck->set(gSavedSettings.getString("FSFontSettingsFile") == DYSLEXIC_FONT_SET);
}

void PrestigePanelAccessibility::onDyslexicFontCommit()
{
    const bool on = mDyslexicFontCheck->get();
    const bool is_on = gSavedSettings.getString("FSFontSettingsFile") == DYSLEXIC_FONT_SET;
    if (on != is_on)
    {
        gSavedSettings.setString("FSFontSettingsFile", on ? DYSLEXIC_FONT_SET : DEFAULT_FONT_SET);   // applies live
    }
}
