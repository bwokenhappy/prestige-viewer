/**
 * @file slfloaterskinimportfs.h
 * @brief "Import from Firestorm" (#1599): lists the skins and themes of the Firestorm installs
 *        and per-user skin folders on this machine (or a picked folder) and imports the chosen
 *        skin + theme through SLSkinImport.
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

#ifndef SL_FLOATERSKINIMPORTFS_H
#define SL_FLOATERSKINIMPORTFS_H

#include "llfloater.h"
#include "slskinimportcore.h"

class LLCheckBoxCtrl;
class LLScrollListCtrl;

class SLFloaterSkinImportFS final : public LLFloater
{
public:
    SLFloaterSkinImportFS(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    struct Row
    {
        std::string skins_dir, skin_folder, skin_name, theme_folder, theme_name, fallback_default;
    };

    void refresh();
    void addSource(const slskin::FsSkinsDir& source, const std::string& fallback_default);
    void onBrowse();
    void onBrowsed(const std::vector<std::string>& filenames);
    void onImport();
    void updateButtons();

    LLScrollListCtrl* mList = nullptr;
    LLCheckBoxCtrl* mIncludeLayout = nullptr;
    std::vector<Row> mRows;
    std::vector<std::string> mPickedDirs;   // folders added with Browse, kept across refreshes
};

#endif
