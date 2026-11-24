/**
 * @file prestigefloaterwelcome.h
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

#ifndef PRESTIGE_FLOATERWELCOME_H
#define PRESTIGE_FLOATERWELCOME_H

#include "llfloater.h"

#include <string>

class LLButton;
class LLIconCtrl;
class LLTextBox;

// The cards are strings of floater_prestige_welcome.xml (card_N_title, card_N_text,
// card_N_icon and, for a "Show me" button, card_N_open: a floater name, or
// "preferences:<page>"). [KEY:<menu item name>] in a text becomes that menu item's current
// key, so a key changed in Preferences > Controls reads right here too.
class PrestigeFloaterWelcome : public LLFloater
{
    LOG_CLASS(PrestigeFloaterWelcome);
public:
    PrestigeFloaterWelcome(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    // After login: shows the guide once for each new version (major.minor), unless "Don't
    // show this at login" was ticked. Waits for the first-run window of a new install to close.
    static void showIfNew();

private:
    void showCard(S32 index);
    std::string withKeys(const std::string& text) const;
    void onOpenFeature();

    S32 mCard = 0;
    S32 mCardCount = 0;

    LLIconCtrl* mIcon = nullptr;
    LLTextBox* mTitle = nullptr;
    LLTextBox* mText = nullptr;
    LLTextBox* mPage = nullptr;
    LLTextBox* mFooter = nullptr;
    LLButton* mBackBtn = nullptr;
    LLButton* mNextBtn = nullptr;
    LLButton* mOpenBtn = nullptr;
};

#endif // PRESTIGE_FLOATERWELCOME_H
