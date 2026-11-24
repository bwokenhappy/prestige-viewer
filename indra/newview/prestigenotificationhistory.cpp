/**
 * @file prestigenotificationhistory.cpp
 * @brief Notification Center model: history of the notifications the viewer showed.
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

#include "llviewerprecompiledheaders.h"

#include "prestigenotificationhistory.h"

#include "llavataractions.h"
#include "llchannelmanager.h"
#include "lldir.h"
#include "lleventtimer.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llnotifications.h"
#include "llscreenchannel.h"
#include "llscriptfloater.h"
#include "llsdserialize.h"
#include "lltoast.h"
#include "llviewercontrol.h"

namespace
{
    constexpr F32 TIMER_PERIOD_SECONDS = 5.f;
    constexpr S32 FILE_VERSION = 1;
    const char HISTORY_FILE_NAME[] = "prestige_notification_history.xml";

    // Notification types the center records. Hints, IM toasts, browser and login-form
    // notifications are left out.
    bool isRecordedType(const std::string& type)
    {
        static LLCachedControl<bool> log_tips(gSavedSettings, "PrestigeNotificationCenterLogTips", true);
        static LLCachedControl<bool> log_alerts(gSavedSettings, "PrestigeNotificationCenterLogAlerts", true);

        if (type == "notify" || type == "groupnotify" || type == "offer")
        {
            return true;
        }
        if (type == "alert" || type == "alertmodal")
        {
            return log_alerts;
        }
        if (type == "notifytip")
        {
            return log_tips;
        }
        return false;
    }

    bool startsWith(const std::string& s, const char* prefix)
    {
        return s.rfind(prefix, 0) == 0;
    }

    PrestigeNotificationHistory::ECategory categorize(const LLNotificationPtr& n)
    {
        const std::string& name = n->getName();
        const std::string type = n->getType();

        if (PrestigeNotificationHistory::isScriptDialog(name) || startsWith(name, "ScriptQuestion"))
        {
            return PrestigeNotificationHistory::CAT_SCRIPT;
        }
        if (type == "groupnotify" || name == "JoinGroup")
        {
            return PrestigeNotificationHistory::CAT_GROUP;
        }
        if (type == "alert" || type == "alertmodal")
        {
            return PrestigeNotificationHistory::CAT_ALERT;
        }
        if (name.find("Friendship") != std::string::npos)
        {
            return PrestigeNotificationHistory::CAT_FRIENDSHIP;
        }
        if (startsWith(name, "TeleportOffered") || name == "TeleportRequest")
        {
            return PrestigeNotificationHistory::CAT_TELEPORT;
        }
        if (type == "offer" || name == "ObjectGiveItem" || name == "UserGiveItem")
        {
            return PrestigeNotificationHistory::CAT_INVENTORY;
        }
        if (type == "notifytip")
        {
            return PrestigeNotificationHistory::CAT_TIP;
        }
        return PrestigeNotificationHistory::CAT_SYSTEM;
    }

    // Number of buttons in the notification's form.
    S32 countButtons(const LLNotificationPtr& n)
    {
        S32 buttons = 0;
        LLNotificationFormPtr form = n->getForm();
        if (form)
        {
            for (S32 i = 0; i < form->getNumElements(); ++i)
            {
                if (form->getElement(i)["type"].asString() == "button")
                {
                    ++buttons;
                }
            }
        }
        return buttons;
    }

    // Label of the button chosen in the notification's response (empty if none).
    std::string chosenButtonLabel(const LLNotificationPtr& n)
    {
        const LLSD& response = n->getResponse();
        LLNotificationFormPtr form = n->getForm();
        if (!form || !response.isMap())
        {
            return std::string();
        }
        for (S32 i = 0; i < form->getNumElements(); ++i)
        {
            const LLSD element = form->getElement(i);
            if (element["type"].asString() == "button" && response[element["name"].asString()].asBoolean())
            {
                std::string label = element["text"].asString();
                return label.empty() ? element["name"].asString() : label;
            }
        }
        return std::string();
    }

    std::string makeTitle(const LLNotificationPtr& n, PrestigeNotificationHistory::ECategory category)
    {
        const LLSD& payload = n->getPayload();
        const LLSD& subs = n->getSubstitutions();
        std::string title;
        if (category == PrestigeNotificationHistory::CAT_GROUP && payload.has("subject"))
        {
            title = payload["subject"].asString();
            const std::string sender = payload["sender_name"].asString();
            if (!sender.empty())
            {
                title += " - " + sender;
            }
        }
        else if (PrestigeNotificationHistory::isScriptDialog(n->getName()) && subs.has("TITLE"))
        {
            title = subs["TITLE"].asString();
        }
        else if (n->hasLabel())
        {
            title = n->getLabel();
        }
        else if (subs.has("OBJECTNAME"))
        {
            title = subs["OBJECTNAME"].asString();
        }
        LLStringUtil::trim(title);
        return title;
    }

    std::string makeMessage(const LLNotificationPtr& n, PrestigeNotificationHistory::ECategory category)
    {
        const LLSD& payload = n->getPayload();
        if (category == PrestigeNotificationHistory::CAT_GROUP && payload.has("message"))
        {
            return payload["message"].asString();
        }
        return n->getMessage();
    }
}

PrestigeNotificationHistory::PrestigeNotificationHistory()
{
}

PrestigeNotificationHistory::~PrestigeNotificationHistory()
{
    delete mTimer;
    mTimer = nullptr;
    mVisibleConnection.disconnect();
    if (mDirty)
    {
        save();
    }
}

void PrestigeNotificationHistory::initSingleton()
{
    // Everything that passed the viewer's own filters (enabled, not expired, not ignored,
    // visibility rules). Connected at the front so no listener that "handles" an event can
    // hide it from us; onVisibleChannelChanged() never stops the signal.
    if (LLNotificationChannelPtr channel = LLNotifications::instance().getChannel("Visible"))
    {
        mVisibleConnection = channel->connectAtFrontChanged(
            [this](const LLSD& payload) { return onVisibleChannelChanged(payload); });
    }
    else
    {
        LL_WARNS() << "No Visible notification channel; the notification center stays empty" << LL_ENDL;
    }

    mTimer = LLEventTimer::run_every(TIMER_PERIOD_SECONDS, [this]() { onTimer(); });
}

// static
bool PrestigeNotificationHistory::isScriptDialog(const std::string& name)
{
    return name == "ScriptDialog" || name == "ScriptDialogGroup" || name == "LoadWebPage";
}

bool PrestigeNotificationHistory::onVisibleChannelChanged(const LLSD& payload)
{
    const std::string sigtype = payload["sigtype"].asString();
    const LLUUID id = payload["id"].asUUID();
    LLNotificationPtr notification = LLNotifications::instance().find(id);

    if (sigtype == "add" || sigtype == "load")
    {
        if (notification)
        {
            record(notification);
        }
    }
    else if (sigtype == "change")
    {
        if (notification && !notification->isActive())
        {
            finish(notification, id, false);
        }
    }
    else if (sigtype == "delete")
    {
        finish(notification, id, true);
    }
    return false; // never stop the channel's other listeners
}

void PrestigeNotificationHistory::record(const LLNotificationPtr& n)
{
    const std::string type = n->getType();
    if (!isRecordedType(type))
    {
        return;
    }

    const bool actionable = type != "notifytip" && countButtons(n) > 0;

    if (Entry* existing = findEntryMutable(n->getID()))
    {
        // Seen before: a notification restored at login (saved history + persistent
        // notifications) or one that came back after being hidden.
        if (actionable && n->isActive() && existing->mState != STATE_WAITING)
        {
            existing->mState = STATE_WAITING;
            existing->mAnswer.clear();
            changed();
        }
        return;
    }

    Entry entry;
    entry.mID = n->getID();
    entry.mTime = n->getDate().notNull() ? n->getDate() : LLDate::now();
    entry.mName = n->getName();
    entry.mCategory = categorize(n);
    entry.mTitle = makeTitle(n, entry.mCategory);
    entry.mMessage = makeMessage(n, entry.mCategory);
    entry.mState = actionable ? STATE_WAITING : STATE_INFO;
    mEntries.push_back(entry);

    if (actionable && !n->isActive())
    {
        finish(n, entry.mID, false);
    }
    prune();
    changed();
}

void PrestigeNotificationHistory::finish(const LLNotificationPtr& n, const LLUUID& id, bool deleted)
{
    Entry* entry = findEntryMutable(id);
    if (!entry || entry->mState != STATE_WAITING)
    {
        return;
    }

    static LLCachedControl<bool> auto_remove_scripts(gSavedSettings, "PrestigeNotificationCenterAutoRemoveScripts", true);
    if (auto_remove_scripts && isScriptDialog(entry->mName) && (deleted || !n || !n->isActive()))
    {
        // Answered, ignored, blocked, replaced by a newer menu or closed: drop it.
        eraseEntry(id);
        changed();
        return;
    }

    if (n && n->isRespondedTo())
    {
        if (countButtons(n) > 1)
        {
            entry->mState = STATE_ANSWERED;
            entry->mAnswer = chosenButtonLabel(n);
        }
        else
        {
            entry->mState = STATE_DISMISSED;
        }
    }
    else if (n && n->isExpired())
    {
        entry->mState = STATE_EXPIRED;
    }
    else if (deleted || !n || n->isCancelled())
    {
        entry->mState = STATE_CLOSED;
    }
    else
    {
        return; // still live
    }
    changed();
}

const PrestigeNotificationHistory::Entry* PrestigeNotificationHistory::findEntry(const LLUUID& id) const
{
    for (const Entry& entry : mEntries)
    {
        if (entry.mID == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

PrestigeNotificationHistory::Entry* PrestigeNotificationHistory::findEntryMutable(const LLUUID& id)
{
    for (Entry& entry : mEntries)
    {
        if (entry.mID == id)
        {
            return &entry;
        }
    }
    return nullptr;
}

void PrestigeNotificationHistory::eraseEntry(const LLUUID& id)
{
    for (entry_list_t::iterator it = mEntries.begin(); it != mEntries.end(); ++it)
    {
        if (it->mID == id)
        {
            mEntries.erase(it);
            return;
        }
    }
}

S32 PrestigeNotificationHistory::countWaiting() const
{
    S32 count = 0;
    for (const Entry& entry : mEntries)
    {
        if (entry.mState == STATE_WAITING)
        {
            ++count;
        }
    }
    return count;
}

void PrestigeNotificationHistory::removeEntry(const LLUUID& id)
{
    if (findEntry(id))
    {
        eraseEntry(id);
        changed();
    }
}

void PrestigeNotificationHistory::clearHistory()
{
    const size_t before = mEntries.size();
    entry_list_t kept;
    for (const Entry& entry : mEntries)
    {
        if (entry.mState == STATE_WAITING)
        {
            kept.push_back(entry);
        }
    }
    mEntries.swap(kept);
    if (mEntries.size() != before)
    {
        changed();
    }
}

bool PrestigeNotificationHistory::openEntry(const LLUUID& id)
{
    if (!findEntry(id))
    {
        return false;
    }

    LLNotificationPtr n = LLNotifications::instance().find(id);
    if (!n || !n->isActive())
    {
        finish(n, id, !n);
        return false;
    }

    // Script dialogs, llLoadURL and object inventory offers live in script floaters with a chiclet.
    if (LLScriptFloaterManager::instance().findObjectId(id).notNull())
    {
        LLScriptFloater* floater = LLFloaterReg::findTypedInstance<LLScriptFloater>("script_floater", id);
        if (floater && floater->getVisible())
        {
            floater->setFrontmost(true);
        }
        else
        {
            LLScriptFloaterManager::instance().toggleScriptFloater(id);
        }
        return true;
    }

    // A toast still on screen needs nothing; a stored one is answered from the notifications window.
    LLNotificationsUI::LLScreenChannel* channel = dynamic_cast<LLNotificationsUI::LLScreenChannel*>(
        LLNotificationsUI::LLChannelManager::getInstance()->findChannelByID(LLNotificationsUI::NOTIFICATION_CHANNEL_UUID));
    if (channel)
    {
        LLNotificationsUI::LLToast* toast = channel->getToastByNotificationID(id);
        if (toast && toast->getVisible())
        {
            return true;
        }
    }

    // Offers from residents are answered in the conversation they were added to.
    const LLUUID from_id = n->getPayload()["from_id"].asUUID();
    if (n->getType() == "offer" && n->canLogToIM() && n->hasFormElements() && from_id.notNull())
    {
        LLAvatarActions::startIM(from_id);
        return true;
    }

    LLFloaterReg::showInstance(gSkinSettings.getBOOL("LegacyNotificationWell") ? "legacy_notification_well_window"
                                                                                : "notification_well_window");
    return true;
}

void PrestigeNotificationHistory::onTimer()
{
    static LLCachedControl<S32> script_timeout_minutes(gSavedSettings, "PrestigeNotificationCenterScriptTimeout", 15);
    static LLCachedControl<bool> auto_remove_scripts(gSavedSettings, "PrestigeNotificationCenterAutoRemoveScripts", true);

    const F64 now = LLDate::now().secondsSinceEpoch();
    const F64 timeout_seconds = 60.0 * (F64)script_timeout_minutes();

    // Catch notifications that ended without passing the Visible channel, and drop script
    // menus that have waited too long.
    std::vector<LLUUID> waiting;
    bool removed = false;
    for (entry_list_t::iterator it = mEntries.begin(); it != mEntries.end();)
    {
        if (it->mState == STATE_WAITING)
        {
            if (auto_remove_scripts && timeout_seconds > 0.0 && isScriptDialog(it->mName)
                && now - it->mTime.secondsSinceEpoch() > timeout_seconds)
            {
                it = mEntries.erase(it);
                removed = true;
                continue;
            }
            waiting.push_back(it->mID);
        }
        ++it;
    }
    if (removed)
    {
        changed();
    }

    for (const LLUUID& id : waiting)
    {
        LLNotificationPtr n = LLNotifications::instance().find(id);
        if (!n || !n->isActive())
        {
            finish(n, id, !n);
        }
    }

    if (mDirty)
    {
        save();
    }
}

void PrestigeNotificationHistory::prune()
{
    static LLCachedControl<S32> max_entries(gSavedSettings, "PrestigeNotificationCenterMaxEntries", 300);
    const size_t limit = (size_t)llmax(20, (S32)max_entries);

    for (entry_list_t::iterator it = mEntries.begin(); mEntries.size() > limit && it != mEntries.end();)
    {
        if (it->mState != STATE_WAITING)
        {
            it = mEntries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void PrestigeNotificationHistory::changed()
{
    mDirty = true; // saved by the timer
    mChangedSignal();
}

std::string PrestigeNotificationHistory::getFilename() const
{
    if (gDirUtilp->getLindenUserDir().empty())
    {
        return std::string();
    }
    return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, HISTORY_FILE_NAME);
}

void PrestigeNotificationHistory::onAccountReady()
{
    const std::string filename = getFilename();
    if (filename.empty() || filename == mFilename)
    {
        return;
    }
    if (mDirty && !mFilename.empty())
    {
        save(); // still the previous account's file
    }
    mFilename = filename;
    load();
}

void PrestigeNotificationHistory::load()
{
    static LLCachedControl<bool> save_history(gSavedSettings, "PrestigeNotificationCenterSaveHistory", true);
    if (!save_history || mFilename.empty() || !LLFile::isfile(mFilename))
    {
        return;
    }

    llifstream file(mFilename.c_str());
    if (!file.is_open())
    {
        return;
    }
    LLSD data;
    if (LLSDSerialize::fromXML(data, file) == LLSDParser::PARSE_FAILURE || !data.isMap())
    {
        LL_WARNS() << "Could not read the notification history " << mFilename << LL_ENDL;
        return;
    }

    // Saved entries are older than anything recorded this session: put them in front.
    entry_list_t loaded;
    const LLSD& entries = data["entries"];
    for (LLSD::array_const_iterator it = entries.beginArray(); it != entries.endArray(); ++it)
    {
        const LLSD& sd = *it;
        Entry entry;
        entry.mID = sd["id"].asUUID();
        if (entry.mID.isNull() || findEntry(entry.mID))
        {
            continue;
        }
        entry.mTime = sd["time"].asDate();
        entry.mName = sd["name"].asString();
        const S32 category = sd["category"].asInteger();
        entry.mCategory = (category >= 0 && category < CAT_COUNT) ? (ECategory)category : CAT_SYSTEM;
        entry.mTitle = sd["title"].asString();
        entry.mMessage = sd["message"].asString();
        const S32 state = sd["state"].asInteger();
        entry.mState = (state >= 0 && state < STATE_COUNT) ? (EState)state : STATE_CLOSED;
        if (entry.mState == STATE_WAITING)
        {
            // Its notification did not survive the restart (unless the viewer restores it,
            // in which case record() marks it waiting again).
            entry.mState = STATE_MISSED;
        }
        entry.mAnswer = sd["answer"].asString();
        loaded.push_back(entry);
    }
    mEntries.insert(mEntries.begin(), loaded.begin(), loaded.end());
    prune();
    mChangedSignal();
}

void PrestigeNotificationHistory::save()
{
    mDirty = false;

    static LLCachedControl<bool> save_history(gSavedSettings, "PrestigeNotificationCenterSaveHistory", true);
    if (!save_history || mFilename.empty())
    {
        return;
    }

    LLSD entries = LLSD::emptyArray();
    for (const Entry& entry : mEntries)
    {
        if (isScriptDialog(entry.mName))
        {
            continue; // script menus are never kept across sessions
        }
        LLSD sd;
        sd["id"] = entry.mID;
        sd["time"] = entry.mTime;
        sd["name"] = entry.mName;
        sd["category"] = (S32)entry.mCategory;
        sd["title"] = entry.mTitle;
        sd["message"] = entry.mMessage;
        sd["state"] = (S32)entry.mState;
        if (!entry.mAnswer.empty())
        {
            sd["answer"] = entry.mAnswer;
        }
        entries.append(sd);
    }

    LLSD data;
    data["version"] = FILE_VERSION;
    data["entries"] = entries;

    llofstream file(mFilename.c_str());
    if (!file.is_open())
    {
        LL_WARNS() << "Could not write the notification history " << mFilename << LL_ENDL;
        return;
    }
    LLSDSerialize::toPrettyXML(data, file);
}
