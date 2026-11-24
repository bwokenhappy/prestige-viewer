/**
 * @file prestigenotificationhistory.h
 * @brief Notification Center model: keeps a history of the notifications the viewer showed
 *        (group notices, offers, alerts, script dialogs, tips), whether they still wait for
 *        an answer, and how they ended. Shown by PrestigeFloaterNotificationCenter.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 * Copyright (C) 2026, Prestige Viewer contributors
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

#ifndef PRESTIGE_NOTIFICATIONHISTORY_H
#define PRESTIGE_NOTIFICATIONHISTORY_H

#include "llsingleton.h"
#include "lldate.h"
#include "llevents.h"
#include "llnotificationptr.h"
#include "lluuid.h"

#include <boost/signals2.hpp>
#include <deque>
#include <string>

class LLEventTimer;

// Listens to the "Visible" notification channel (everything the viewer actually shows) and
// records each notification once. An entry stays "waiting" while its notification is live and
// has buttons to answer; it then becomes answered / dismissed / closed / expired and stays in
// the history until the user removes it or clears the history.
//
// Script dialogs (llDialog, llTextBox, llLoadURL) are removed again as soon as they are answered,
// ignored or closed, and when they have waited longer than PrestigeNotificationCenterScriptTimeout
// minutes, so finished script menus never pile up.
//
// The history (not the live notifications) is saved per account in
// prestige_notification_history.xml when PrestigeNotificationCenterSaveHistory is on.
class PrestigeNotificationHistory : public LLSingleton<PrestigeNotificationHistory>
{
    LLSINGLETON(PrestigeNotificationHistory);
    ~PrestigeNotificationHistory() override;
    LOG_CLASS(PrestigeNotificationHistory);

public:
    enum ECategory
    {
        CAT_SYSTEM = 0,
        CAT_ALERT,
        CAT_GROUP,
        CAT_FRIENDSHIP,
        CAT_TELEPORT,
        CAT_INVENTORY,
        CAT_SCRIPT,
        CAT_TIP,
        CAT_COUNT
    };

    enum EState
    {
        STATE_WAITING = 0,  // live, has buttons: still actionable
        STATE_INFO,         // shown, nothing to answer
        STATE_ANSWERED,     // a button was chosen (mAnswer holds its label)
        STATE_DISMISSED,    // the only button (OK / Close) was used
        STATE_CLOSED,       // went away without an answer
        STATE_EXPIRED,      // timed out
        STATE_MISSED,       // was still waiting when the viewer closed (restored from the saved history)
        STATE_COUNT
    };

    struct Entry
    {
        LLUUID      mID;            // the notification's id
        LLDate      mTime;
        std::string mName;          // notification template name
        ECategory   mCategory = CAT_SYSTEM;
        std::string mTitle;
        std::string mMessage;       // raw message (may hold SLURLs and <nolink> tags)
        EState      mState = STATE_INFO;
        std::string mAnswer;        // label of the chosen button, for STATE_ANSWERED
    };
    typedef std::deque<Entry> entry_list_t; // oldest first

    const entry_list_t& getEntries() const { return mEntries; }
    const Entry* findEntry(const LLUUID& id) const;
    S32 countWaiting() const;

    // Removes one entry from the list (a live notification itself is left alone).
    void removeEntry(const LLUUID& id);
    // Removes every entry that no longer waits for an answer.
    void clearHistory();

    // Shows a waiting notification again: its script dialog, its stored toast, or the
    // notifications window. Returns false (and updates the entry) when it is gone.
    bool openEntry(const LLUUID& id);

    // Called at login once the per-account directory is known: loads the saved history.
    void onAccountReady();

    typedef boost::signals2::signal<void()> changed_signal_t;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb)
    {
        return mChangedSignal.connect(cb);
    }

    // llDialog / llTextBox / llLoadURL notifications, which are dropped once finished.
    static bool isScriptDialog(const std::string& name);

private:
    void initSingleton() override;

    bool onVisibleChannelChanged(const LLSD& payload);
    void record(const LLNotificationPtr& notification);
    // The live notification ended (answered, closed, expired); deleted: it left the channel.
    void finish(const LLNotificationPtr& notification, const LLUUID& id, bool deleted);
    Entry* findEntryMutable(const LLUUID& id);
    void eraseEntry(const LLUUID& id);

    void onTimer();
    void prune();
    void changed();

    std::string getFilename() const;
    void load();
    void save();

    entry_list_t      mEntries;
    LLBoundListener   mVisibleConnection;
    changed_signal_t  mChangedSignal;
    LLEventTimer*     mTimer = nullptr;
    std::string       mFilename;
    bool              mDirty = false;
};

#endif // PRESTIGE_NOTIFICATIONHISTORY_H
