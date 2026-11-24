/**
 * @file prestigefloaternotificationcenter.cpp
 * @brief Notification Center window: recent notifications, those still waiting for an
 *        answer, and the history of handled ones, with Remove and Clear all.
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

#include "prestigefloaternotificationcenter.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "llurlmatch.h"
#include "llurlregistry.h"

namespace
{
    constexpr F32 PERIODIC_REFRESH_SECONDS = 5.f; // names in SLURLs resolve over time
    constexpr size_t MAX_COLUMN_CHARS = 300;
    constexpr S32 MAX_URLS_PER_TEXT = 32;

    typedef PrestigeNotificationHistory PNH;

    const char* const CATEGORY_ICONS[PNH::CAT_COUNT] =
    {
        "Info_Small",               // CAT_SYSTEM
        "OutboxStatus_Warning",     // CAT_ALERT
        "Group_Notices",            // CAT_GROUP
        "Conv_toolbar_add_person",  // CAT_FRIENDSHIP
        "Inv_Landmark",             // CAT_TELEPORT
        "Inv_Object",               // CAT_INVENTORY
        "Inv_Script",               // CAT_SCRIPT
        "Info_Small",               // CAT_TIP
    };

    const char* const CATEGORY_STRINGS[PNH::CAT_COUNT] =
    {
        "cat_system",
        "cat_alert",
        "cat_group",
        "cat_friendship",
        "cat_teleport",
        "cat_inventory",
        "cat_script",
        "cat_tip",
    };

    // Replaces SLURLs and <nolink>/<icon> tags with the text they display as.
    std::string resolveUrls(const std::string& raw)
    {
        std::string out;
        std::string rest = raw;
        LLUrlMatch match;
        for (S32 i = 0; i < MAX_URLS_PER_TEXT && LLUrlRegistry::instance().findUrl(rest, match); ++i)
        {
            const size_t start = match.getStart();
            const size_t end = match.getEnd();
            if (end < start || end >= rest.size())
            {
                break;
            }
            out += rest.substr(0, start);
            out += match.getLabel();
            rest = rest.substr(end + 1);
        }
        out += rest;
        return out;
    }

    // One line for a list column: whitespace runs become single spaces, long text is cut.
    std::string oneLine(const std::string& text)
    {
        std::string out;
        out.reserve(llmin(text.size(), MAX_COLUMN_CHARS + 4));
        bool space = false;
        for (char c : text)
        {
            if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
            {
                space = !out.empty();
                continue;
            }
            if (space)
            {
                out += ' ';
                space = false;
            }
            out += c;
            if (out.size() >= MAX_COLUMN_CHARS)
            {
                // Do not cut a UTF-8 sequence in half.
                while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
                {
                    out.pop_back();
                }
                if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80) != 0)
                {
                    out.pop_back();
                }
                out += "...";
                break;
            }
        }
        return out;
    }

    // Plain text for the detail pane: tags removed, URLs kept so the editor links them.
    std::string stripTags(const std::string& raw)
    {
        std::string out = raw;
        for (const char* tag : { "<nolink>", "</nolink>" })
        {
            const std::string t(tag);
            for (size_t pos = out.find(t); pos != std::string::npos; pos = out.find(t, pos))
            {
                out.erase(pos, t.size());
            }
        }
        return out;
    }
}

PrestigeFloaterNotificationCenter::PrestigeFloaterNotificationCenter(const LLSD& key)
    : LLFloater(key)
{
}

PrestigeFloaterNotificationCenter::~PrestigeFloaterNotificationCenter()
{
    mHistoryConnection.disconnect();
}

bool PrestigeFloaterNotificationCenter::postBuild()
{
    mList = getChild<LLScrollListCtrl>("notification_list");
    mDetail = getChild<LLTextEditor>("detail_text");
    mFilter = getChild<LLComboBox>("filter_combo");
    mCounter = getChild<LLTextBox>("counter");
    mOpenBtn = getChild<LLButton>("open_btn");
    mRemoveBtn = getChild<LLButton>("remove_btn");
    mClearAllBtn = getChild<LLButton>("clear_all_btn");

    mDetail->setContentTrusted(false); // messages come from residents and objects

    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateSelection(); });
    mList->setDoubleClickCallback([this]() { onOpenSelected(); });
    mFilter->setCommitCallback([this](LLUICtrl*, const LLSD&) { mDirty = true; });

    mOpenBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onOpenSelected(); });
    mRemoveBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onRemoveSelected(); });
    mClearAllBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClearAll(); });

    mHistoryConnection = PrestigeNotificationHistory::getInstance()->setChangedCallback([this]() { mDirty = true; });

    updateSelection();
    return true;
}

void PrestigeFloaterNotificationCenter::onOpen(const LLSD& key)
{
    mDirty = true;
}

void PrestigeFloaterNotificationCenter::draw()
{
    if (mDirty || mRefreshTimer.getElapsedTimeF32() > PERIODIC_REFRESH_SECONDS)
    {
        refresh();
    }
    LLFloater::draw();
}

bool PrestigeFloaterNotificationCenter::passesFilter(const PrestigeNotificationHistory::Entry& entry) const
{
    const std::string filter = mFilter->getValue().asString();
    if (filter.empty() || filter == "all")
    {
        return true;
    }
    if (filter == "waiting")
    {
        return entry.mState == PNH::STATE_WAITING;
    }
    if (filter == "group")
    {
        return entry.mCategory == PNH::CAT_GROUP;
    }
    if (filter == "offers")
    {
        return entry.mCategory == PNH::CAT_FRIENDSHIP || entry.mCategory == PNH::CAT_TELEPORT
            || entry.mCategory == PNH::CAT_INVENTORY;
    }
    if (filter == "scripts")
    {
        return entry.mCategory == PNH::CAT_SCRIPT;
    }
    if (filter == "alerts")
    {
        return entry.mCategory == PNH::CAT_ALERT;
    }
    if (filter == "system")
    {
        return entry.mCategory == PNH::CAT_SYSTEM || entry.mCategory == PNH::CAT_TIP;
    }
    return true;
}

std::string PrestigeFloaterNotificationCenter::formatTime(const LLDate& time) const
{
    std::string day_key = getString("time_day_key");
    std::string today_key = day_key;
    LLStringUtil::format(day_key, LLSD().with("datetime", (S32)time.secondsSinceEpoch()));
    LLStringUtil::format(today_key, LLSD().with("datetime", (S32)LLDate::now().secondsSinceEpoch()));

    std::string text = getString(day_key == today_key ? "time_today" : "time_older");
    LLStringUtil::format(text, LLSD().with("datetime", (S32)time.secondsSinceEpoch()));
    return text;
}

std::string PrestigeFloaterNotificationCenter::statusText(const PrestigeNotificationHistory::Entry& entry) const
{
    switch (entry.mState)
    {
    case PNH::STATE_WAITING:
        return getString("state_waiting");
    case PNH::STATE_ANSWERED:
        if (!entry.mAnswer.empty())
        {
            LLStringUtil::format_map_t args;
            args["[ANSWER]"] = entry.mAnswer;
            return getString("state_answered_with", args);
        }
        return getString("state_answered");
    case PNH::STATE_DISMISSED:
        return getString("state_dismissed");
    case PNH::STATE_CLOSED:
        return getString("state_closed");
    case PNH::STATE_EXPIRED:
        return getString("state_expired");
    case PNH::STATE_MISSED:
        return getString("state_missed");
    case PNH::STATE_INFO:
    default:
        return std::string();
    }
}

void PrestigeFloaterNotificationCenter::refresh()
{
    mDirty = false;
    mRefreshTimer.reset();

    const LLUUID selected_id = getSelectedID();
    const S32 scroll_pos = mList->getScrollPos();
    mList->clearRows();

    const PNH::entry_list_t& entries = PrestigeNotificationHistory::getInstance()->getEntries();
    S32 shown = 0;
    S32 waiting = 0;
    S32 history = 0;

    // Newest first.
    for (PNH::entry_list_t::const_reverse_iterator it = entries.rbegin(); it != entries.rend(); ++it)
    {
        const PNH::Entry& entry = *it;
        if (entry.mState == PNH::STATE_WAITING)
        {
            ++waiting;
        }
        else
        {
            ++history;
        }
        if (!passesFilter(entry))
        {
            continue;
        }

        const S32 category = llclamp((S32)entry.mCategory, 0, (S32)PNH::CAT_COUNT - 1);
        const std::string message = oneLine(resolveUrls(entry.mMessage));
        std::string title = oneLine(resolveUrls(entry.mTitle));
        if (title.empty())
        {
            title = message;
        }

        LLSD row;
        row["value"] = entry.mID;
        LLSD& cols = row["columns"];
        cols[0]["column"] = "icon";
        cols[0]["type"] = "icon";
        cols[0]["value"] = CATEGORY_ICONS[category];
        cols[1]["column"] = "time";
        cols[1]["value"] = formatTime(entry.mTime);
        cols[2]["column"] = "type";
        cols[2]["value"] = getString(CATEGORY_STRINGS[category]);
        cols[3]["column"] = "title";
        cols[3]["value"] = title;
        cols[4]["column"] = "status";
        cols[4]["value"] = statusText(entry);
        if (entry.mState == PNH::STATE_WAITING)
        {
            cols[4]["font"]["style"] = "BOLD";
        }
        cols[5]["column"] = "message";
        cols[5]["value"] = message;
        mList->addElement(row, ADD_BOTTOM);
        ++shown;
    }

    if (selected_id.notNull())
    {
        mList->selectByID(selected_id);
    }
    mList->setScrollPos(scroll_pos);

    LLStringUtil::format_map_t args;
    args["[WAITING]"] = llformat("%d", waiting);
    args["[HISTORY]"] = llformat("%d", history);
    mCounter->setText(getString("counter", args));
    if (shown == 0)
    {
        mList->setCommentText(getString(entries.empty() ? "empty_list" : "empty_filter"));
    }

    mClearAllBtn->setEnabled(history > 0);
    updateSelection();
}

LLUUID PrestigeFloaterNotificationCenter::getSelectedID() const
{
    if (LLScrollListItem* item = mList->getFirstSelected())
    {
        return item->getValue().asUUID();
    }
    return LLUUID::null;
}

void PrestigeFloaterNotificationCenter::updateSelection()
{
    const LLUUID id = getSelectedID();
    const PNH::Entry* entry = id.notNull() ? PrestigeNotificationHistory::getInstance()->findEntry(id) : nullptr;

    mOpenBtn->setEnabled(entry && entry->mState == PNH::STATE_WAITING);
    mRemoveBtn->setEnabled(!mList->getAllSelected().empty());

    if (!entry)
    {
        if (mDetailID.notNull())
        {
            mDetail->clear();
        }
        mDetailID.setNull();
        return;
    }
    if (entry->mID == mDetailID)
    {
        return; // unchanged; keep the reader's scroll position
    }
    mDetailID = entry->mID;

    std::string text;
    if (!entry->mTitle.empty())
    {
        text = stripTags(entry->mTitle) + "\n\n";
    }
    text += stripTags(entry->mMessage);
    mDetail->setText(text);
}

void PrestigeFloaterNotificationCenter::onOpenSelected()
{
    const LLUUID id = getSelectedID();
    const PNH::Entry* entry = id.notNull() ? PrestigeNotificationHistory::getInstance()->findEntry(id) : nullptr;
    if (!entry || entry->mState != PNH::STATE_WAITING)
    {
        return;
    }
    PrestigeNotificationHistory::getInstance()->openEntry(id);
    mDirty = true;
}

void PrestigeFloaterNotificationCenter::onRemoveSelected()
{
    std::vector<LLUUID> ids;
    for (LLScrollListItem* item : mList->getAllSelected())
    {
        ids.push_back(item->getValue().asUUID());
    }
    for (const LLUUID& id : ids)
    {
        PrestigeNotificationHistory::getInstance()->removeEntry(id);
    }
    mDirty = true;
}

void PrestigeFloaterNotificationCenter::onClearAll()
{
    PrestigeNotificationHistory::getInstance()->clearHistory();
    mDirty = true;
}
