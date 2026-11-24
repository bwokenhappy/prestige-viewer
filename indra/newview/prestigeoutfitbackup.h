/**
 * @file prestigeoutfitbackup.h
 * @brief Back an outfit (or what you are wearing) up to a JSON file, and
 *        restore it later as a new outfit of links, finding each item in
 *        inventory by item UUID, asset UUID or name plus type.
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

#ifndef PRESTIGE_OUTFITBACKUP_H
#define PRESTIGE_OUTFITBACKUP_H

class LLUUID;

class PrestigeOutfitBackup
{
public:
    // Asks for a file name, then writes the outfit folder's items to it.
    static void backupOutfit(const LLUUID& outfit_id);

    // Asks for a file name, then writes what is worn now (the Current Outfit folder).
    static void backupCurrentLook();

    // Asks for a backup file, then makes a new outfit folder of links to the
    // items found in inventory and reports what was missing.
    static void restoreFromFile();
};

#endif // PRESTIGE_OUTFITBACKUP_H
