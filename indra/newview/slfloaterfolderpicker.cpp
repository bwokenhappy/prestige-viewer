/**
 * @file slfloaterfolderpicker.cpp
 * @brief Picks a folder in My Inventory. See slfloaterfolderpicker.h.
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

#include "llviewerprecompiledheaders.h"

#include "slfloaterfolderpicker.h"

#include "llbutton.h"
#include "llfiltereditor.h"
#include "llfloaterreg.h"
#include "llfolderview.h"
#include "llfolderviewmodelinventory.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorypanel.h"
#include "lltextbox.h"

SLFloaterFolderPicker::SLFloaterFolderPicker(const LLSD& key)
    : LLFloater(key)
{
}

bool SLFloaterFolderPicker::postBuild()
{
    mPanel = getChild<LLInventoryPanel>("folder_panel");
    mOKBtn = getChild<LLButton>("ok_btn");
    mNote = getChild<LLTextBox>("selection_note");

    // Folders only: no item type passes, every folder is shown.
    mPanel->setFilterTypes(1ULL << LLInventoryType::IT_CATEGORY);
    mPanel->setShowFolderState(LLInventoryFilter::SHOW_ALL_FOLDERS);
    mPanel->getRootFolder()->setAutoSelectOverride(true);
    mPanel->setSelectCallback([this](const std::deque<LLFolderViewItem*>& items, bool user_action)
                              { onSelectionChange(items, user_action); });

    getChild<LLFilterEditor>("folder_filter")->setCommitCallback(
        [this](LLUICtrl*, const LLSD& value) { onFilterEdit(value.asString()); });

    mOKBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickOK(); });
    getChild<LLButton>("cancel_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });

    refreshSelection();
    return true;
}

void SLFloaterFolderPicker::onClose(bool app_quitting)
{
    // The callbacks may hold a caller's state; don't keep them past this use.
    mValidate = nullptr;
    mOnPick = nullptr;
}

// static
void SLFloaterFolderPicker::show(const std::string& instructions, const LLUUID& selected_id,
                                 validate_cb_t validate, pick_cb_t on_pick)
{
    SLFloaterFolderPicker* picker = LLFloaterReg::showTypedInstance<SLFloaterFolderPicker>("inventory_folder_picker");
    if (!picker)
    {
        return;
    }
    picker->mValidate = validate;
    picker->mOnPick = on_pick;
    picker->getChild<LLTextBox>("instructions")->setText(instructions);
    picker->mSelectedID.setNull();
    if (selected_id.notNull() && gInventory.getCategory(selected_id))
    {
        picker->mSelectedID = selected_id;
        picker->mPanel->setSelection(selected_id, TAKE_FOCUS_NO);
    }
    picker->refreshSelection();
}

void SLFloaterFolderPicker::onSelectionChange(const std::deque<LLFolderViewItem*>& items, bool user_action)
{
    mSelectedID.setNull();
    if (!items.empty() && items.front())
    {
        const LLFolderViewModelItemInventory* model_item =
            static_cast<const LLFolderViewModelItemInventory*>(items.front()->getViewModelItem());
        if (model_item && gInventory.getCategory(model_item->getUUID()))
        {
            mSelectedID = model_item->getUUID();
        }
    }
    refreshSelection();
}

void SLFloaterFolderPicker::onFilterEdit(const std::string& search_string)
{
    std::string upper = search_string;
    LLStringUtil::toUpper(upper);
    mPanel->setFilterSubString(upper);
}

void SLFloaterFolderPicker::refreshSelection()
{
    if (mSelectedID.isNull())
    {
        mNote->setText(getString("none_selected"));
        mOKBtn->setEnabled(false);
        return;
    }

    std::string reason;
    if (mValidate && !mValidate(mSelectedID, &reason))
    {
        LLStringUtil::format_map_t args;
        args["[REASON]"] = reason;
        mNote->setText(getString("cannot_use", args));
        mOKBtn->setEnabled(false);
        return;
    }

    mNote->setText(get_category_path(mSelectedID));
    mOKBtn->setEnabled(true);
}

void SLFloaterFolderPicker::onClickOK()
{
    std::string reason;
    if (mSelectedID.isNull() || !gInventory.getCategory(mSelectedID) || (mValidate && !mValidate(mSelectedID, &reason)))
    {
        refreshSelection();
        return;
    }
    pick_cb_t on_pick = mOnPick;
    const LLUUID picked_id = mSelectedID;
    closeFloater();
    if (on_pick)
    {
        on_pick(picked_id);
    }
}
