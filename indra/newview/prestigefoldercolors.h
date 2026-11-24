/**
 * @file prestigefoldercolors.h
 * @brief Prestige folder colours: a colour of your own on any of your inventory
 *        folders, seen by you only, plus optional icon colours by item type.
 *
 * The colours live in a file in this account's settings folder
 * (prestige_folder_colors.xml, folder UUID -> colour). Nothing is sent to the server.
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

#ifndef PRESTIGE_FOLDERCOLORS_H
#define PRESTIGE_FOLDERCOLORS_H

#include "llassettype.h"
#include "llfloater.h"
#include "lluuid.h"
#include "v4color.h"

#include <string>
#include <utility>
#include <vector>

class LLColorSwatchCtrl;
class LLTextBox;

// Static store (no singleton: getColor() is asked for every visible inventory row each frame).
class PrestigeFolderColors
{
public:
    // The colour the user gave this folder, if any. Cheap; loads the file on first use after login.
    static bool getColor(const LLUUID& folder_id, LLColor4& color);
    static void setColor(const LLUUID& folder_id, const LLColor4& color);
    static void clearColor(const LLUUID& folder_id);

    // A preset from the folder colour menu ("red", "orange", ... "pink"), from colors.xml.
    static bool getPresetColor(const std::string& preset, LLColor4& color);

    // The icon colour for an item of this type when PrestigeInventoryColoredIcons is on.
    static bool getItemTypeColor(LLAssetType::EType type, LLColor4& color);

    // Right-click menu actions "prestige_folder_color_<preset|custom|clear>" on one folder.
    // selection: every selected folder, for "custom", which opens one window for all of them.
    static bool isMenuAction(const std::string& action);
    static void doMenuAction(const std::string& action, const LLUUID& folder_id, const uuid_vec_t& selection);
};

// Folder colour window: a colour swatch (opens the colour picker) for one or more folders.
class PrestigeFloaterFolderColor : public LLFloater
{
    LOG_CLASS(PrestigeFloaterFolderColor);
public:
    PrestigeFloaterFolderColor(const LLSD& key);
    ~PrestigeFloaterFolderColor() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;

private:
    void onSwatchCommit();
    void onSwatchCancel();
    void onClickClear();

    LLColorSwatchCtrl* mSwatch = nullptr;
    LLTextBox*         mNameText = nullptr;
    uuid_vec_t         mFolders;
    // the colours the folders had when the window opened (cleared ones: false), for Cancel
    std::vector<std::pair<LLUUID, std::pair<bool, LLColor4>>> mOriginal;
};

#endif // PRESTIGE_FOLDERCOLORS_H
