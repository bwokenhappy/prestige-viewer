/**
 * @file prestigepanelaccessibility.h
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

#ifndef PRESTIGE_PANELACCESSIBILITY_H
#define PRESTIGE_PANELACCESSIBILITY_H

#include "llfloaterpreference.h"

#include <boost/signals2.hpp>

class LLCheckBoxCtrl;

// The other options are plain controls bound to their settings (so Preferences' Cancel
// reverts them); the dyslexia-friendly font switches the interface font set (FSFontSettingsFile).
class PrestigePanelAccessibility : public LLPanelPreference
{
public:
    PrestigePanelAccessibility() = default;
    ~PrestigePanelAccessibility() override = default;

    bool postBuild() override;

private:
    void refreshDyslexicFont();
    void onDyslexicFontCommit();

    LLCheckBoxCtrl* mDyslexicFontCheck = nullptr;
    boost::signals2::scoped_connection mFontSetConnection;
};

#endif // PRESTIGE_PANELACCESSIBILITY_H
