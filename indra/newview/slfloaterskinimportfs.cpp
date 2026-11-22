/**
 * @file slfloaterskinimportfs.cpp
 * @brief "Import from Firestorm" (#1599). See the header.
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

#include "slfloaterskinimportfs.h"
#include "slskinimport.h"

#include "llcheckboxctrl.h"
#include "lldirpicker.h"
#include "llfloaterpreference.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"

namespace fs = std::filesystem;

namespace
{
std::string utf8(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
}

SLFloaterSkinImportFS::SLFloaterSkinImportFS(const LLSD& key)
:   LLFloater(key)
{
}

bool SLFloaterSkinImportFS::postBuild()
{
    mList = getChild<LLScrollListCtrl>("fs_skin_list");
    mIncludeLayout = getChild<LLCheckBoxCtrl>("include_layout");
    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
    mList->setDoubleClickCallback([this]() { onImport(); });
    getChild<LLUICtrl>("browse_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onBrowse(); });
    getChild<LLUICtrl>("import_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onImport(); });
    getChild<LLUICtrl>("close_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    return true;
}

void SLFloaterSkinImportFS::onOpen(const LLSD& key)
{
    if (key.has("include_layout")) mIncludeLayout->set(key["include_layout"].asBoolean());
    if (key.has("dir"))
    {
        const std::string dir = key["dir"].asString();
        if (std::find(mPickedDirs.begin(), mPickedDirs.end(), dir) == mPickedDirs.end()) mPickedDirs.push_back(dir);
    }
    refresh();
}

void SLFloaterSkinImportFS::addSource(const slskin::FsSkinsDir& source, const std::string& fallback_default)
{
    for (const slskin::FsSkin& skin : source.skins)
    {
        for (const slskin::FsTheme& theme : skin.themes)
        {
            Row row;
            row.skins_dir = utf8(source.dir);
            row.skin_folder = skin.folder;
            row.skin_name = skin.name;
            row.theme_folder = theme.folder;
            row.theme_name = theme.name;
            row.fallback_default = source.user ? fallback_default : std::string();

            LLSD element;
            element["value"] = LLSD::Integer(mRows.size());
            element["columns"][0]["column"] = "skin";
            element["columns"][0]["value"] = skin.name;
            element["columns"][1]["column"] = "theme";
            element["columns"][1]["value"] = theme.name;
            element["columns"][2]["column"] = "where";
            element["columns"][2]["value"] = source.label;
            mList->addElement(element);
            mRows.push_back(row);
        }
    }
}

void SLFloaterSkinImportFS::refresh()
{
    mList->deleteAllItems();
    mRows.clear();
    std::string fallback_default;
    std::vector<slskin::FsSkinsDir> sources;
    for (const std::string& dir : mPickedDirs)
    {
        slskin::FsSkinsDir picked;
        if (SLSkinImport::firestormSkinsIn(dir, picked)) sources.push_back(picked);
    }
    for (const slskin::FsSkinsDir& found : SLSkinImport::findFirestormSkins()) sources.push_back(found);
    for (const slskin::FsSkinsDir& s : sources)
    {
        std::error_code ec;
        if (!s.user && fallback_default.empty() && fs::is_directory(s.dir / "default", ec)) fallback_default = utf8(s.dir / "default");
    }
    for (const slskin::FsSkinsDir& s : sources) addSource(s, fallback_default);
    if (mRows.empty())
    {
        mList->setCommentText(getString("none_found"));
    }
    else
    {
        mList->selectFirstItem();
    }
    updateButtons();
}

void SLFloaterSkinImportFS::updateButtons()
{
    const LLScrollListItem* item = mList->getFirstSelected();
    getChildView("import_btn")->setEnabled(item != nullptr && !SLSkinImport::busy());
}

void SLFloaterSkinImportFS::onBrowse()
{
    LLHandle<LLFloater> handle = getHandle();
    (new LLDirPickerThread([handle](const std::vector<std::string>& filenames, std::string)
        {
            if (SLFloaterSkinImportFS* self = static_cast<SLFloaterSkinImportFS*>(handle.get())) self->onBrowsed(filenames);
        }, std::string()))->getFile();
}

void SLFloaterSkinImportFS::onBrowsed(const std::vector<std::string>& filenames)
{
    if (filenames.empty() || filenames[0].empty()) return;
    const std::string& dir = filenames[0];
    slskin::FsSkinsDir picked;
    if (!SLSkinImport::firestormSkinsIn(dir, picked))
    {
        // Not a Firestorm skin: import it as a skin folder in our layout.
        SLSkinImport::importFolder(dir, mIncludeLayout->get(), &LLFloaterPreference::onSkinInstalled);
        return;
    }
    if (std::find(mPickedDirs.begin(), mPickedDirs.end(), dir) == mPickedDirs.end()) mPickedDirs.push_back(dir);
    refresh();
}

void SLFloaterSkinImportFS::onImport()
{
    const LLScrollListItem* item = mList->getFirstSelected();
    if (!item) return;
    const S32 index = item->getValue().asInteger();
    if (index < 0 || index >= S32(mRows.size())) return;
    const Row row = mRows[index];
    LLHandle<LLFloater> handle = getHandle();
    SLSkinImport::importFirestorm(row.skins_dir, row.skin_folder, row.skin_name, row.theme_folder, row.theme_name,
                                  row.fallback_default, mIncludeLayout->get(),
                                  [handle](const std::string& folder)
                                  {
                                      LLFloaterPreference::onSkinInstalled(folder);
                                      if (LLFloater* self = handle.get()) static_cast<SLFloaterSkinImportFS*>(self)->updateButtons();
                                  });
    updateButtons();
}
