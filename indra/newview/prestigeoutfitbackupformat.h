/**
 * @file prestigeoutfitbackupformat.h
 * @brief The outfit backup file format (JSON) and the matching of backed-up
 *        items to inventory items. Has no viewer dependencies (only the
 *        standard library and boost::json), so it can be tested on its own;
 *        prestigeoutfitbackup.cpp connects it to the inventory and the UI.
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

#ifndef PRESTIGE_OUTFITBACKUPFORMAT_H
#define PRESTIGE_OUTFITBACKUPFORMAT_H

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace PrestigeOutfitBackupFormat
{
    // Written to "format"; a file without it is not an outfit backup.
    extern const char* const FORMAT_ID;
    // Written to "version". Files with a higher version are refused.
    constexpr int VERSION = 1;
    // Larger files are refused before parsing.
    constexpr size_t MAX_FILE_BYTES = 8 * 1024 * 1024;
    // Backups with more items are refused.
    constexpr size_t MAX_ITEMS = 10000;
    // Inventory folder names are limited to this many bytes.
    constexpr size_t MAX_FOLDER_NAME_BYTES = 63;

    // One backed-up item. UUIDs are lowercase strings, empty when unknown.
    struct Entry
    {
        std::string name;
        std::string item_id;        // inventory item the outfit linked to
        std::string asset_id;
        std::string creator_id;
        std::string asset_type;     // LLAssetType name: "clothing", "bodypart", "object", ...
        std::string inv_type;       // LLInventoryType name: "wearable", "object", ...
        std::string wearable_type;  // "shirt", "shape", ...; empty for non-wearables
        std::string attach_point;   // attachment point name; empty unless worn when backed up
        int attach_point_id = 0;    // attachment point number; 0 when unknown
        bool no_copy = false;       // the item could not be copied
        std::string folder;         // path below the outfit folder, "" for the top
    };

    struct Backup
    {
        int version = VERSION;
        std::string outfit_name;
        std::string created;        // ISO-8601, UTC
        std::string source;         // "outfit" or "current_look"
        std::string generator;      // viewer name and version
        std::vector<std::string> folders;   // subfolder paths, flat
        std::vector<Entry> items;
    };

    // Serialise a backup to indented JSON text.
    std::string toJson(const Backup& backup);

    // Parse JSON text. On failure returns false and sets error to a short,
    // user-facing sentence. Never throws.
    bool fromJson(const std::string& text, Backup& backup, std::string& error);

    // Lowercase UUID string, or "" when the text is not a UUID or is the null UUID.
    std::string normalizeUUID(const std::string& text);

    // Cut a UTF-8 string to at most max_bytes bytes without splitting a character.
    std::string truncateUTF8(const std::string& text, size_t max_bytes);

    // "<name> (restored)", or "<name> (restored N)" when that name is taken;
    // never longer than MAX_FOLDER_NAME_BYTES.
    std::string restoredFolderName(const std::string& outfit_name, const std::vector<std::string>& taken);

    enum EMatch
    {
        MATCH_NONE = 0,
        MATCH_ITEM_ID,      // the same inventory item
        MATCH_ASSET_ID,     // another item with the same asset
        MATCH_NAME          // an item with the same name and type (weaker)
    };

    // An inventory item that a backed-up entry may be matched to.
    struct Candidate
    {
        std::string item_id;
        std::string asset_id;
        std::string creator_id;
        std::string name;
        std::string asset_type;
        std::string wearable_type;
    };

    class Matcher
    {
    public:
        // Candidates are numbered in the order they are added, from 0.
        void add(const Candidate& candidate);
        size_t size() const { return mCandidates.size(); }

        // Finds the best candidate for the entry: by item UUID, then by asset
        // UUID, then by name plus type. Sets index when a match is found.
        EMatch find(const Entry& entry, size_t& index) const;

    private:
        static std::string nameKey(const std::string& asset_type, const std::string& name);

        std::vector<Candidate> mCandidates;
        std::map<std::string, size_t> mByItem;
        std::multimap<std::string, size_t> mByAsset;
        std::multimap<std::string, size_t> mByName;
    };
}

#endif // PRESTIGE_OUTFITBACKUPFORMAT_H
