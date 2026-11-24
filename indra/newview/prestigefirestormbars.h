/**
 * @file prestigefirestormbars.h
 * @brief Firestorm-style top and bottom bars: the location in the menu bar row, the
 *        nearby chat bar at the left of the bottom toolbar row, and the one-time switch
 *        of existing settings to that layout.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * Parts of the menu bar location follow Firestorm's LLStatusBar parcel information
 * (Phoenix Firestorm Viewer, LGPL 2.1).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef PRESTIGE_FIRESTORMBARS_H
#define PRESTIGE_FIRESTORMBARS_H

#include "llframetimer.h"
#include "llrect.h"

#include <boost/signals2.hpp>
#include <string>

class LLButton;
class LLControlGroup;
class LLIconCtrl;
class LLPanel;
class LLStatusBar;
class LLTextBox;
class LLToolBar;
class LLView;

namespace PrestigeFirestormBars
{
    // Once per install (PrestigeFirestormBarsApplied): the settings behind Firestorm's default
    // bars -- location in the menu bar, navigation bar with its search box, favorites bar on its
    // own row, the nearby chat bar kept at the left of the bottom toolbar row. Run right after
    // the user settings file loads (llappviewer.cpp), so the user can change them afterwards.
    void applyDefaultsOnce(LLControlGroup& group);

    // True while the nearby chat bar lives in the bottom toolbar row: PrestigeChatBarInToolbar
    // and PrestigeChatBarAlwaysVisible on, the MMO chat log (which docks the bar to itself) off,
    // and the chat bar shown.
    bool chatBarInToolbar();

    // Where the chat bar goes in the toolbar row ("chat_bar_dock" in panel_toolbar_view.xml),
    // in `parent`'s coordinates. False when the row is not shown (a skin without it, toolbars
    // hidden, the option off): the bar floats as before.
    bool chatBarDockRect(const LLView* parent, LLRect& rect);

    // LLToolBarView::postBuild: the chat bar part's saved width, and saving it when dragged.
    void initBottomRow(LLView* toolbar_view);

    // LLToolBarView::updateForDraw, every frame: shows the chat bar part of the bottom row while
    // the bar lives there, and keeps the row as tall as the toolbar (which can wrap to more rows)
    // and at least as tall as the chat bar.
    void layoutBottomRow(LLView* toolbar_view, LLView* bottom_panel, LLToolBar* bottom_toolbar, bool show_toolbars);
}

// Firestorm's location in the menu bar row (PrestigeShowMenuBarLocation): the parcel's
// restriction icons and "Region (x, y, z) - Maturity - Parcel", right of the menus and cut off
// before the menu search box / L$ balance. The widgets are "parcel_info_panel" in
// panel_status_bar.xml; LLStatusBar owns one of these and calls refresh() every frame.
class PrestigeMenuBarLocation
{
public:
    explicit PrestigeMenuBarLocation(LLStatusBar* status_bar);
    ~PrestigeMenuBarLocation();

    void refresh();
    void setVisibleForMouselook(bool visible) { mMouselookVisible = visible; }

    // The V1-style location text ("Region (x, y, z) - Maturity - Parcel"); RLVa @showloc hides
    // the region and parcel names.
    static std::string buildLocationString(bool show_coords);

private:
    enum EIcon { VOICE_ICON, FLY_ICON, PUSH_ICON, BUILD_ICON, SCRIPTS_ICON, SEE_AVATARS_ICON, DAMAGE_ICON, ICON_COUNT };

    void updateIcons();
    void layout();
    void onIconClick(EIcon icon);
    void onTextClick();
    void onBuyLandClick();
    S32 layoutWidget(LLView* view, S32 left);

    LLStatusBar*    mStatusBar = nullptr;
    LLPanel*        mPanel = nullptr;
    LLTextBox*      mText = nullptr;
    LLTextBox*      mDamageText = nullptr;
    LLButton*       mBuyLand = nullptr;
    LLIconCtrl*     mIcons[ICON_COUNT] = {};
    LLFrameTimer    mUpdateTimer;
    std::string     mLocation;
    S32             mLastHealth = -1;
    bool            mMouselookVisible = true;
    bool            mDirty = true;
    boost::signals2::connection mParcelConnection;
};

#endif // PRESTIGE_FIRESTORMBARS_H
