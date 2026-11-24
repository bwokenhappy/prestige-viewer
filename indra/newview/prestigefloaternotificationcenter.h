/**
 * @file prestigefloaternotificationcenter.h
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

#ifndef PRESTIGE_FLOATERNOTIFICATIONCENTER_H
#define PRESTIGE_FLOATERNOTIFICATIONCENTER_H

#include "llfloater.h"
#include "llframetimer.h"
#include "prestigenotificationhistory.h"

class LLButton;
class LLComboBox;
class LLScrollListCtrl;
class LLTextBox;
class LLTextEditor;

class PrestigeFloaterNotificationCenter : public LLFloater
{
    friend class LLFloaterReg;
public:
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    PrestigeFloaterNotificationCenter(const LLSD& key);
    ~PrestigeFloaterNotificationCenter() override;

    void refresh() override;
    void updateSelection();
    bool passesFilter(const PrestigeNotificationHistory::Entry& entry) const;
    std::string formatTime(const LLDate& time) const;
    std::string statusText(const PrestigeNotificationHistory::Entry& entry) const;
    LLUUID getSelectedID() const;

    void onOpenSelected();
    void onRemoveSelected();
    void onClearAll();

    LLScrollListCtrl* mList = nullptr;
    LLTextEditor*     mDetail = nullptr;
    LLComboBox*       mFilter = nullptr;
    LLTextBox*        mCounter = nullptr;
    LLButton*         mOpenBtn = nullptr;
    LLButton*         mRemoveBtn = nullptr;
    LLButton*         mClearAllBtn = nullptr;

    bool                         mDirty = true;
    LLFrameTimer                 mRefreshTimer;
    LLUUID                       mDetailID;
    boost::signals2::connection  mHistoryConnection;
};

#endif // PRESTIGE_FLOATERNOTIFICATIONCENTER_H
