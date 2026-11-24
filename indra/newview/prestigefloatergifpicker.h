/**
 * @file prestigefloatergifpicker.h
 * @brief Prestige GIF picker: search GIPHY or KLIPY with the user's own key and
 *        add the picked GIF's link to the chat message being typed.
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

#ifndef PRESTIGE_FLOATERGIFPICKER_H
#define PRESTIGE_FLOATERGIFPICKER_H

#include "llfloater.h"
#include "llframetimer.h"
#include "lluuid.h"

#include <string>
#include <vector>

class LLComboBox;
class LLMediaCtrl;
class LLSearchEditor;
class LLTextBox;

// Opened by the GIF button next to the emoji button of a conversation (nearby chat
// or an IM); the key is that conversation's session id (null: nearby chat).
//
// Results show as thumbnails in an embedded browser (LLMediaCtrl) on a page built
// here and written to the cache folder. Each thumbnail links to
// secondlife:///app/prestigegif/pick/<page>/<index>, which PrestigeGifPickHandler
// hands back to pick(). Picking adds the GIF's https link to the end of that
// conversation's message and closes the picker.
//
// Nothing is sent to GIPHY / KLIPY until the picker opens, searches only run on
// Enter (free keys are rate limited), and the keys never reach the log (they are
// registered with LLCoreHttpUtil::addLogRedaction) or the screen.
class PrestigeFloaterGifPicker : public LLFloater
{
    LOG_CLASS(PrestigeFloaterGifPicker);
public:
    enum EProvider
    {
        PROVIDER_GIPHY = 0,
        PROVIDER_KLIPY
    };

    struct Result
    {
        std::string mThumbURL;   // shown in the picker
        std::string mGifURL;     // added to the message
        std::string mTitle;
    };

    PrestigeFloaterGifPicker(const LLSD& key);
    ~PrestigeFloaterGifPicker() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

    static bool hasKey(EProvider provider);
    // A key is set for at least one service: the GIF buttons show.
    static bool hasAnyKey();
    // The GIF button: opens the picker for this conversation, or closes it when it is
    // already open for it.
    static void toggleFor(const LLUUID& session_id);

    // A thumbnail was clicked (from PrestigeGifPickHandler).
    void pick(U32 page, U32 index);

private:
    EProvider currentProvider() const;
    void updateProviderControls();
    void onSearchCommit();
    void onProviderCommit();
    // Shows trending (empty search) or the search results; reuses what is shown
    // when the same search ran less than a few minutes ago, unless `force`.
    void runSearch(bool force);
    void onResults(U32 serial, EProvider provider, const std::string& query,
                   const std::vector<Result>& results, const std::string& error);
    // Writes the results page (or a message when `message` is not empty) and shows it.
    void showPage(const std::string& message);
    void setStatus(const std::string& text);
    std::string providerName(EProvider provider) const;

    static void fetchCoro(LLHandle<LLFloater> handle, U32 serial, EProvider provider,
                          std::string query, std::string url);

    LLSearchEditor* mSearchEditor = nullptr;
    LLComboBox*     mProviderCombo = nullptr;
    LLMediaCtrl*    mBrowser = nullptr;
    LLTextBox*      mStatusText = nullptr;

    LLUUID              mSessionID;     // conversation the picked GIF goes to
    std::vector<Result> mResults;       // what the page shows
    U32                 mRequestSerial = 0;
    U32                 mPageSerial = 0;
    bool                mBusy = false;

    // What mResults are, so reopening or pressing Enter again does not ask again
    bool                mHaveResults = false;
    EProvider           mShownProvider = PROVIDER_GIPHY;
    std::string         mShownQuery;
    LLFrameTimer        mShownAge;
};

#endif // PRESTIGE_FLOATERGIFPICKER_H
