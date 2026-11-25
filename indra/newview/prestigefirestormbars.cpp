/**
 * @file prestigefirestormbars.cpp
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

#include "llviewerprecompiledheaders.h"

#include "prestigefirestormbars.h"

#include "gmdxchatlog.h"
#include "llagent.h"
#include "llbutton.h"
#include "llcontrol.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "lliconctrl.h"
#include "lllayoutstack.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llparcel.h"
#include "llresizebar.h"
#include "llstatusbar.h"
#include "lltextbox.h"
#include "lltoolbar.h"
#include "lltoolbarview.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
// [RLVa:KB]
#include "rlvcommon.h"
#include "rlvhandler.h"
// [/RLVa:KB]

namespace PrestigeFirestormBars
{
namespace
{
    // Height of the bottom row while the chat bar is in it (the toolbar's own height when it
    // is taller, e.g. wrapped to two rows).
    constexpr S32 CHAT_ROW_MIN_HEIGHT = 30;
    constexpr S32 CHAT_PANEL_MIN_WIDTH = 150;

    void setBool(LLControlGroup& group, const char* name, bool value)
    {
        if (group.controlExists(name))
        {
            group.setBOOL(name, value);
        }
    }
}

void applyDefaultsOnce(LLControlGroup& group)
{
    // Firestorm: the People toolbar button opens the People window (Nearby / Friends / Groups /
    // Recent / Blocked), not the Contacts pane in Conversations (Contacts stays in that list). Once,
    // also for accounts that already had the bars switch.
    if (group.controlExists("PrestigeFirestormPeopleApplied") && !group.getBOOL("PrestigeFirestormPeopleApplied"))
    {
        setBool(group, "ContactsInConversations", false);
        group.setBOOL("PrestigeFirestormPeopleApplied", true);
        LL_INFOS("Prestige") << "People button opens the People window, as in Firestorm (once)" << LL_ENDL;
    }

    if (!group.controlExists("PrestigeFirestormBarsApplied") || group.getBOOL("PrestigeFirestormBarsApplied"))
    {
        return;
    }
    // Firestorm 7's default top bar: menus, then the location with the parcel icons, the menu
    // search box, L$ and the clock / media / FPS on the right; the navigation bar (back, forward,
    // home, Land, Lighting, location, search) under it and the favorites bar on its own row.
    setBool(group, "PrestigeShowMenuBarLocation", true);
    setBool(group, "NavBarShowParcelProperties", true);
    setBool(group, "NavBarShowCoordinates", true);
    setBool(group, "MenuSearch", true);
    setBool(group, "ShowStatusBarFPS", true);
    setBool(group, "ShowNavbarNavigationPanel", true);
    setBool(group, "ShowMiniLocationPanel", false);   // the menu bar shows the location instead
    setBool(group, "PrestigeNavBarSearch", true);
    setBool(group, "FavoritesBarOwnRow", true);
    // Firestorm's bottom bar: the nearby chat bar at the left of the toolbar row.
    setBool(group, "SlipstreamNearbyChatInput", true);
    setBool(group, "PrestigeChatBarAlwaysVisible", true);
    setBool(group, "PrestigeChatBarInToolbar", true);
    group.setBOOL("PrestigeFirestormBarsApplied", true);
    LL_INFOS("Prestige") << "Firestorm-style top and bottom bars switched on (once)" << LL_ENDL;
}

bool chatBarInToolbar()
{
    static LLCachedControl<bool> in_toolbar(gSavedSettings, "PrestigeChatBarInToolbar", true);
    static LLCachedControl<bool> always_visible(gSavedSettings, "PrestigeChatBarAlwaysVisible", true);
    if (!in_toolbar || !always_visible || GMDXChatLog::active())
    {
        return false;
    }
    const LLFloater* bar = LLFloaterReg::findInstance("chatbar");
    return bar && bar->getVisible() && !bar->isMinimized();
}

bool chatBarDockRect(const LLView* parent, LLRect& rect)
{
    if (!parent || !gToolBarView || !chatBarInToolbar())
    {
        return false;
    }
    const LLView* dock = gToolBarView->findChildView("chat_bar_dock");
    if (!dock || !dock->isInVisibleChain() || dock->getRect().getWidth() < 20)
    {
        return false;
    }
    return dock->localRectToOtherView(dock->getLocalRect(), &rect, parent);
}

void initBottomRow(LLView* toolbar_view)
{
    LLLayoutPanel* chat_panel = toolbar_view ? toolbar_view->findChild<LLLayoutPanel>("chat_bar_layout_panel") : nullptr;
    if (!chat_panel)
    {
        return;   // a skin without the chat bar part of the row
    }
    const S32 width = llmax(CHAT_PANEL_MIN_WIDTH, gSavedSettings.getS32("PrestigeChatBarDockWidth"));
    chat_panel->reshape(width, chat_panel->getRect().getHeight());
    if (LLResizeBar* bar = chat_panel->getResizeBar())
    {
        LLHandle<LLPanel> handle = chat_panel->getHandle();
        bar->setResizeListener([handle](void*)
        {
            if (LLPanel* panel = handle.get())
            {
                gSavedSettings.setS32("PrestigeChatBarDockWidth", panel->getRect().getWidth());
            }
        });
    }
}

void layoutBottomRow(LLView* toolbar_view, LLView* bottom_panel, LLToolBar* bottom_toolbar, bool show_toolbars)
{
    if (!toolbar_view || !bottom_panel || !bottom_toolbar)
    {
        return;
    }
    static LLView* s_chat_panel = nullptr;   // the toolbar view lives for the whole session
    static const LLView* s_looked_in = nullptr;
    if (s_looked_in != toolbar_view)
    {
        s_looked_in = toolbar_view;
        s_chat_panel = toolbar_view->findChildView("chat_bar_layout_panel");
    }
    LLView* toolbar_panel = bottom_toolbar->getParent();
    if (!s_chat_panel || !toolbar_panel || toolbar_panel == bottom_panel)
    {
        return;   // the classic layout: the toolbar fills the bottom row on its own
    }
    const bool chat_shown = show_toolbars && chatBarInToolbar();
    if (s_chat_panel->getVisible() != chat_shown)
    {
        s_chat_panel->setVisible(chat_shown);
    }
    const bool toolbar_shown = toolbar_panel->getVisible();
    const bool row_shown = chat_shown || toolbar_shown;
    if (bottom_panel->getVisible() != row_shown)
    {
        bottom_panel->setVisible(row_shown);
    }
    S32 height = toolbar_shown ? bottom_toolbar->getRect().getHeight() : 0;
    if (chat_shown)
    {
        height = llmax(height, CHAT_ROW_MIN_HEIGHT);
    }
    if (height > 0 && height != bottom_panel->getRect().getHeight())
    {
        bottom_panel->reshape(bottom_panel->getRect().getWidth(), height);
    }
}
}

//
// PrestigeMenuBarLocation
//

PrestigeMenuBarLocation::PrestigeMenuBarLocation(LLStatusBar* status_bar)
:   mStatusBar(status_bar)
{
    mPanel = status_bar ? status_bar->findChild<LLPanel>("parcel_info_panel") : nullptr;
    if (!mPanel)
    {
        return;   // a skin's panel_status_bar.xml without it
    }
    mText = mPanel->findChild<LLTextBox>("parcel_info_text");
    mDamageText = mPanel->findChild<LLTextBox>("damage_text");
    mBuyLand = mPanel->findChild<LLButton>("buy_land_btn");
    static const char* const icon_names[ICON_COUNT] =
        { "voice_icon", "fly_icon", "push_icon", "build_icon", "scripts_icon", "see_avs_icon", "damage_icon" };
    for (S32 i = 0; i < ICON_COUNT; ++i)
    {
        mIcons[i] = mPanel->findChild<LLIconCtrl>(icon_names[i]);
        if (mIcons[i])
        {
            const EIcon icon = (EIcon)i;
            mIcons[i]->setMouseDownCallback([this, icon](LLUICtrl*, S32, S32, MASK) { onIconClick(icon); });
        }
    }
    if (mText)
    {
        mText->setClickedCallback([this](void*) { onTextClick(); });
        // Firestorm: the text lights up under the mouse (it opens About Land)
        mText->setMouseEnterCallback([this](LLUICtrl*, const LLSD&)
        {
            static LLUIColor hover = LLUIColorTable::instance().getColor("ParcelHoverColor");
            mText->setColor(hover);
            mText->setReadOnlyColor(hover);
        });
        mText->setMouseLeaveCallback([this](LLUICtrl*, const LLSD&)
        {
            static LLUIColor normal = LLUIColorTable::instance().getColor("TimeTextColor");
            mText->setColor(normal);
            mText->setReadOnlyColor(normal);
        });
    }
    if (mBuyLand)
    {
        mBuyLand->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBuyLandClick(); });
    }
    mParcelConnection = gAgent.addParcelChangedCallback([this]() { mDirty = true; });
}

PrestigeMenuBarLocation::~PrestigeMenuBarLocation()
{
    mParcelConnection.disconnect();
}

// static
std::string PrestigeMenuBarLocation::buildLocationString(bool show_coords)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return std::string();
    }
    std::string region_name = region->getName();
    std::string parcel_name = LLViewerParcelMgr::getInstance()->getAgentParcelName();
// [RLVa:KB]
    if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
    {
        region_name = RlvStrings::getString(RlvStringKeys::Hidden::Region);
        parcel_name = RlvStrings::getString(RlvStringKeys::Hidden::Parcel);
    }
// [/RLVa:KB]
    std::string text = region_name;
    if (show_coords)
    {
        const LLVector3 pos = gAgent.getPositionAgent();
        text += llformat(" (%d, %d, %d)", ll_round(pos.mV[VX]), ll_round(pos.mV[VY]), ll_round(pos.mV[VZ]));
    }
    const std::string maturity = region->getSimAccessString();
    if (!maturity.empty())
    {
        text += " - " + maturity;
    }
    if (!parcel_name.empty())
    {
        text += " - " + parcel_name;
    }
    return text;
}

void PrestigeMenuBarLocation::refresh()
{
    if (!mPanel || !mText)
    {
        return;
    }
    static LLCachedControl<bool> show_location(gSavedSettings, "PrestigeShowMenuBarLocation", true);
    const bool visible = show_location && mMouselookVisible && gAgent.getRegion() != nullptr;
    if (mPanel->getVisible() != visible)
    {
        mPanel->setVisible(visible);
        mDirty = true;
    }
    if (!visible)
    {
        return;
    }
    // Icons twice a second (parcel and region flags arrive any time) or on a parcel change;
    // the text and the place in the row every frame (position, menus and the right side move).
    if (mDirty || mUpdateTimer.getElapsedTimeF32() > 0.5f)
    {
        mDirty = false;
        mUpdateTimer.reset();
        updateIcons();
    }
    static LLCachedControl<bool> show_coords(gSavedSettings, "NavBarShowCoordinates", true);
    const std::string text = buildLocationString(show_coords);
    if (text != mLocation)
    {
        mLocation = text;
        mText->setText(text);
    }
    layout();
}

void PrestigeMenuBarLocation::updateIcons()
{
    static LLCachedControl<bool> show_icons(gSavedSettings, "NavBarShowParcelProperties", true);
    LLViewerParcelMgr* vpm = LLViewerParcelMgr::getInstance();
    LLViewerRegion* agent_region = gAgent.getRegion();
    LLParcel* agent_parcel = vpm->getAgentParcel();
    const bool have = show_icons && agent_region && agent_parcel;
    bool shown[ICON_COUNT] = {};
    bool for_sale = false;
    if (have)
    {
        // As LLPanelTopInfoBar: the selected parcel's properties are updated more often when
        // the agent stands in it (EXT-2987).
        LLParcel* current_parcel = agent_parcel;
        LLParcel* selected_parcel = vpm->getParcelSelection()->getParcel();
        if (selected_parcel && selected_parcel->getLocalID() == agent_parcel->getLocalID()
            && vpm->getSelectionRegion() == agent_region)
        {
            current_parcel = selected_parcel;
        }
        shown[VOICE_ICON] = !vpm->allowAgentVoice(agent_region, current_parcel);
        shown[FLY_ICON] = !vpm->allowAgentFly(agent_region, current_parcel);
        shown[PUSH_ICON] = !vpm->allowAgentPush(agent_region, current_parcel);
        shown[BUILD_ICON] = !vpm->allowAgentBuild(current_parcel);
        shown[SCRIPTS_ICON] = !vpm->allowAgentScripts(agent_region, current_parcel);
        shown[SEE_AVATARS_ICON] = !current_parcel->getSeeAVs();
        shown[DAMAGE_ICON] = vpm->allowAgentDamage(agent_region, current_parcel);
        for_sale = !current_parcel->isPublic() && vpm->canAgentBuyParcel(current_parcel, false);
    }
    for (S32 i = 0; i < ICON_COUNT; ++i)
    {
        if (mIcons[i] && mIcons[i]->getVisible() != shown[i])
        {
            mIcons[i]->setVisible(shown[i]);
        }
    }
    if (mDamageText)
    {
        mDamageText->setVisible(shown[DAMAGE_ICON]);
        const S32 health = mStatusBar ? mStatusBar->getHealth() : 100;
        if (shown[DAMAGE_ICON] && health != mLastHealth)
        {
            mLastHealth = health;
            mDamageText->setText(llformat("%d%%", health));
        }
    }
    if (mBuyLand)
    {
        mBuyLand->setVisible(for_sale);
    }
}

S32 PrestigeMenuBarLocation::layoutWidget(LLView* view, S32 left)
{
    if (view && view->getVisible())
    {
        LLRect rect = view->getRect();
        rect.setOriginAndSize(left, rect.mBottom, rect.getWidth(), rect.getHeight());
        if (rect != view->getRect())
        {
            view->setShape(rect);
        }
        left += rect.getWidth() + 2;
    }
    return left;
}

// Firestorm's order: the icons, then the text, the whole right of the last menu and cut off
// before the first thing on the right side of the row.
void PrestigeMenuBarLocation::layout()
{
    const S32 MENU_PARCEL_SPACING = 4;
    const S32 RIGHT_SPACING = 8;
    S32 left = 2;
    for (S32 i = 0; i < ICON_COUNT; ++i)
    {
        left = layoutWidget(mIcons[i], left);
    }
    left = layoutWidget(mDamageText, left);
    left = layoutWidget(mBuyLand, left);

    S32 panel_left = (gMenuBarView ? gMenuBarView->getRightmostMenuEdge() : 0) + MENU_PARCEL_SPACING;
    S32 right_limit = mStatusBar->getRect().getWidth();
    for (const char* name : { "menu_search_panel", "balance_bg", "TimeText" })
    {
        const LLView* view = mStatusBar->findChildView(name, false);
        if (view && view->getVisible())
        {
            right_limit = llmin(right_limit, view->getRect().mLeft);
        }
    }
    right_limit -= RIGHT_SPACING;

    const LLFontGL* font = mText->getFont();
    const S32 text_width = font ? font->getWidth(mLocation) + 4 : 200;
    const S32 room = right_limit - panel_left - left;
    mText->setVisible(room >= 40);
    const S32 shown_width = llclamp(text_width, 0, llmax(room, 0));
    LLRect text_rect = mText->getRect();
    text_rect.setOriginAndSize(left, text_rect.mBottom, llmax(shown_width, 1), text_rect.getHeight());
    if (text_rect != mText->getRect())
    {
        mText->setShape(text_rect);
    }
    LLRect panel_rect = mPanel->getRect();
    panel_rect.setOriginAndSize(panel_left, panel_rect.mBottom, llmax(left + shown_width, 1), panel_rect.getHeight());
    if (panel_rect != mPanel->getRect())
    {
        mPanel->setShape(panel_rect);
    }
}

void PrestigeMenuBarLocation::onIconClick(EIcon icon)
{
    switch (icon)
    {
    case VOICE_ICON:
        LLNotificationsUtil::add("NoVoice");
        break;
    case FLY_ICON:
        LLNotificationsUtil::add("NoFly");
        break;
    case PUSH_ICON:
        LLNotificationsUtil::add("PushRestricted");
        break;
    case BUILD_ICON:
        LLNotificationsUtil::add("NoBuild");
        break;
    case SCRIPTS_ICON:
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (region && region->getRegionFlag(REGION_FLAGS_ESTATE_SKIP_SCRIPTS))
        {
            LLNotificationsUtil::add("ScriptsStopped");
        }
        else if (region && region->getRegionFlag(REGION_FLAGS_SKIP_SCRIPTS))
        {
            LLNotificationsUtil::add("ScriptsNotRunning");
        }
        else
        {
            LLNotificationsUtil::add("NoOutsideScripts");
        }
        break;
    }
    case SEE_AVATARS_ICON:
        LLNotificationsUtil::add("SeeAvatars");
        break;
    case DAMAGE_ICON:
        LLNotificationsUtil::add("NotSafe");
        break;
    case ICON_COUNT:
        break;
    }
}

void PrestigeMenuBarLocation::onTextClick()
{
// [RLVa:KB]
    if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
    {
        return;
    }
// [/RLVa:KB]
    LLFloaterReg::toggleInstance("about_land", LLSD().with("current_parcel", true));
}

void PrestigeMenuBarLocation::onBuyLandClick()
{
// [RLVa:KB]
    if (gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC))
    {
        return;
    }
// [/RLVa:KB]
    handle_buy_land();
}
