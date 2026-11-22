/**
 * @file slfloaterfolderpicker.h
 * @brief Picks a folder in My Inventory (folders only). First used by Preferences > Chat >
 *        Alerts for the accepted-items folder; any caller can pass its own check and result.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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

#ifndef SL_FLOATERFOLDERPICKER_H
#define SL_FLOATERFOLDERPICKER_H

#include "llfloater.h"

#include <deque>
#include <functional>

class LLButton;
class LLFolderViewItem;
class LLInventoryPanel;
class LLTextBox;

class SLFloaterFolderPicker : public LLFloater
{
public:
    // Returns false (and why) if the folder can't be picked.
    typedef std::function<bool(const LLUUID& folder_id, std::string* reason)> validate_cb_t;
    typedef std::function<void(const LLUUID& folder_id)> pick_cb_t;

    SLFloaterFolderPicker(const LLSD& key);

    bool postBuild() override;
    void onClose(bool app_quitting) override;

    // Opens the picker on 'selected_id' (may be null). on_pick runs on OK, with a folder
    // that passed 'validate'. A second call replaces the first caller's callbacks.
    static void show(const std::string& instructions, const LLUUID& selected_id,
                     validate_cb_t validate, pick_cb_t on_pick);

private:
    void onSelectionChange(const std::deque<LLFolderViewItem*>& items, bool user_action);
    void onFilterEdit(const std::string& search_string);
    void onClickOK();
    void refreshSelection();

    LLInventoryPanel* mPanel = nullptr;
    LLButton*         mOKBtn = nullptr;
    LLTextBox*        mNote = nullptr;
    LLUUID            mSelectedID;
    validate_cb_t     mValidate;
    pick_cb_t         mOnPick;
};

#endif // SL_FLOATERFOLDERPICKER_H
