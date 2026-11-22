/**
 * @file slskinimport.h
 * @brief Skin importer (#1599): installs a skin from a zip, a folder or a Firestorm skin + theme
 *        through the safe pipeline in slskinimportcore (unpack to a staging folder, convert,
 *        move into place), and shows / logs what was imported, converted and skipped.
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

#ifndef SL_SKINIMPORT_H
#define SL_SKINIMPORT_H

#include "slskinimportcore.h"

#include <functional>
#include <string>
#include <vector>

class SLSkinImport
{
public:
    // Called on the main thread when an import ends: the installed folder, or empty.
    using done_cb_t = std::function<void(const std::string& installed_folder)>;

    // Preferences > Skins > Add skin: a .zip (our layout, Alchemy / LL skins, or a zipped
    // Firestorm skin or theme), with or without a manifest.
    static void importZip(const std::string& zip_path, bool include_layout, done_cb_t done);
    // Add skin folder: a skin folder in our layout. A Firestorm skins / skin / theme folder
    // opens the Firestorm picker on it instead (the user picks the theme).
    static void importFolder(const std::string& dir, bool include_layout, done_cb_t done);
    // One Firestorm skin + theme from a skins directory (fallback_default: an install's
    // skins/default for a per-user skins folder that has none).
    static void importFirestorm(const std::string& skins_dir, const std::string& skin_folder, const std::string& skin_name,
                                const std::string& theme_folder, const std::string& theme_name,
                                const std::string& fallback_default, bool include_layout, done_cb_t done);

    // Firestorm skins folders on this machine: installs first, then per-user skin folders.
    static std::vector<slskin::FsSkinsDir> findFirestormSkins();
    // The skins in one picked folder, when it is a Firestorm skins / skin / theme folder.
    static bool firestormSkinsIn(const std::string& dir, slskin::FsSkinsDir& out);

    // <user app dir>/skins
    static std::string userSkinsDir();
    // Remove staging folders a crash or quit left behind.
    static void sweepStaging();
    static bool busy();
};

#endif
