/**
 * @file prestigefloaterwelcome.cpp
 * @brief "Where is everything?": a short guide of cards for people coming from Firestorm,
 *        shown once at login for each new version and kept in the Help menu.
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

#include "prestigefloaterwelcome.h"

#include "llapp.h"
#include "llappviewer.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llfloaterpreference.h"
#include "llfloaterreg.h"
#include "lliconctrl.h"
#include "lltextbox.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "slkeycommands.h"

namespace
{
std::string card_string(S32 card, const char* part)
{
    // cards are numbered from 1 in the XML
    return llformat("card_%d_%s", card + 1, part);
}

std::string current_version()
{
    return llformat("%d.%d", LLVersionInfo::instance().getMajor(), LLVersionInfo::instance().getMinor());
}
} // namespace

PrestigeFloaterWelcome::PrestigeFloaterWelcome(const LLSD& key)
:   LLFloater(key)
{
}

bool PrestigeFloaterWelcome::postBuild()
{
    mIcon = getChild<LLIconCtrl>("card_icon");
    mTitle = getChild<LLTextBox>("card_title");
    mText = getChild<LLTextBox>("card_text");
    mPage = getChild<LLTextBox>("card_page");
    mFooter = getChild<LLTextBox>("footer");
    mBackBtn = getChild<LLButton>("back_btn");
    mNextBtn = getChild<LLButton>("next_btn");
    mOpenBtn = getChild<LLButton>("open_btn");

    mCardCount = 0;
    while (hasString(card_string(mCardCount, "title")))
    {
        ++mCardCount;
    }

    mBackBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { showCard(mCard - 1); });
    mNextBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (mCard + 1 >= mCardCount)
        {
            closeFloater();
        }
        else
        {
            showCard(mCard + 1);
        }
    });
    mOpenBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onOpenFeature(); });
    getChild<LLUICtrl>("close_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    return true;
}

void PrestigeFloaterWelcome::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    mFooter->setText(withKeys(getString("footer_text")));
    showCard(0);
}

void PrestigeFloaterWelcome::showCard(S32 index)
{
    if (mCardCount <= 0)
    {
        return;
    }
    mCard = llclamp(index, 0, mCardCount - 1);

    mTitle->setText(getString(card_string(mCard, "title")));
    mText->setText(withKeys(getString(card_string(mCard, "text"))));
    const std::string icon = card_string(mCard, "icon");
    mIcon->setValue(hasString(icon) ? getString(icon) : std::string());
    mOpenBtn->setVisible(hasString(card_string(mCard, "open")));

    LLStringUtil::format_map_t args;
    args["[N]"] = llformat("%d", mCard + 1);
    args["[COUNT]"] = llformat("%d", mCardCount);
    mPage->setText(getString("page", args));

    mBackBtn->setEnabled(mCard > 0);
    mNextBtn->setLabel(getString(mCard + 1 >= mCardCount ? "done_label" : "next_label"));
}

std::string PrestigeFloaterWelcome::withKeys(const std::string& text) const
{
    // [KEY:<menu item name>] -> the item's key now ("Ctrl+P"), or a note that it has none
    static const std::string open_tag = "[KEY:";
    std::string result;
    size_t pos = 0;
    while (true)
    {
        const size_t start = text.find(open_tag, pos);
        const size_t end = start == std::string::npos ? std::string::npos : text.find(']', start);
        if (end == std::string::npos)
        {
            result += text.substr(pos);
            break;
        }
        result += text.substr(pos, start - pos);
        const std::string item = text.substr(start + open_tag.size(), end - start - open_tag.size());
        std::string keys = SLKeyCommands::instance().getMenuBindingString("viewer", item);
        result += keys.empty() ? getString("no_key") : keys;
        pos = end + 1;
    }
    return result;
}

void PrestigeFloaterWelcome::onOpenFeature()
{
    const std::string open = card_string(mCard, "open");
    if (!hasString(open))
    {
        return;
    }
    // "<floater>" or "preferences:<page>"
    const std::string target = getString(open);
    const size_t colon = target.find(':');
    const std::string floater = target.substr(0, colon);
    if (!LLFloaterReg::isRegistered(floater))
    {
        LL_WARNS("Welcome") << "No floater " << floater << " for card " << mCard + 1 << LL_ENDL;
        return;
    }
    LLFloater* shown = LLFloaterReg::showInstance(floater);
    if (colon != std::string::npos)
    {
        if (LLFloaterPreference* prefs = dynamic_cast<LLFloaterPreference*>(shown))
        {
            prefs->selectPanel(target.substr(colon + 1));
        }
    }
}

// static
void PrestigeFloaterWelcome::showIfNew()
{
    if (gSavedSettings.getBOOL("PrestigeWelcomeDontShow"))
    {
        return;
    }
    const std::string version = current_version();
    if (gSavedSettings.getString("PrestigeWelcomeShown") == version)
    {
        return;
    }
    // A new install first gets the "set up like my old viewer" window: one at a time
    doOnIdleRepeating([version]()
    {
        if (LLApp::isExiting() || gDisconnected)
        {
            return true;
        }
        if (LLFloaterReg::instanceVisible("sl_first_run"))
        {
            return false; // ask again next frame
        }
        gSavedSettings.setString("PrestigeWelcomeShown", version);
        LLFloaterReg::showInstance("prestige_welcome");
        return true;
    });
}
