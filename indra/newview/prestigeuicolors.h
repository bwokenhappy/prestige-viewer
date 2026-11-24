/**
 * @file prestigeuicolors.h
 * @brief Prestige "Your colours": accent, window background and text colours of the
 *        interface, plus the high contrast colour set of the Accessibility page.
 *
 * The skin's colours.xml derives its role colours (FocusColor, PanelFocusBackgroundColor,
 * LabelTextColor, ...) from a small palette (SlipstreamBlue, SlipstreamPanel, SlipstreamText,
 * ...). References are resolved when the skin loads, so a change here sets every role colour
 * of a group as a user colour of LLUIColorTable: the change shows at once and is kept in
 * user_settings/colors.xml like any colour from Preferences > Colors.
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

#ifndef PRESTIGE_UICOLORS_H
#define PRESTIGE_UICOLORS_H

#include "llpanel.h"
#include "v4color.h"

#include <boost/signals2.hpp>

class LLColorSwatchCtrl;

class PrestigeUIColors
{
public:
    enum EGroup { ACCENT, WINDOW, TEXT };

    // The colour the interface uses now for this group (the user's, or the skin's).
    static LLColor4 getCurrent(EGroup group);
    // The skin's own colour for this group.
    static LLColor4 getDefault(EGroup group);

    // Set one group (and the colours derived from it). The skin's own colour resets the group.
    static void setColor(EGroup group, const LLColor4& color);

    // Back to the skin's colours for all three groups.
    static void resetAll();

    // PrestigeHighContrast changed: on = keep the user's colours aside and use white text on
    // black windows with a bright accent; off = bring the user's colours back.
    static void applyHighContrast(bool on);
};

// Preferences > Colors > Your colours
class PrestigePanelYourColors : public LLPanel
{
public:
    PrestigePanelYourColors() = default;
    ~PrestigePanelYourColors() override = default;

    bool postBuild() override;
    void onVisibilityChange(bool new_visibility) override;

private:
    void refreshSwatches();
    void onSwatchCommit(PrestigeUIColors::EGroup group, LLColorSwatchCtrl* swatch);
    void onClickReset();

    LLColorSwatchCtrl* mAccentSwatch = nullptr;
    LLColorSwatchCtrl* mWindowSwatch = nullptr;
    LLColorSwatchCtrl* mTextSwatch = nullptr;
    bool               mDirty = false;   // colours changed since last saved to colors.xml
    boost::signals2::scoped_connection mHighContrastConnection;
};

#endif // PRESTIGE_UICOLORS_H
