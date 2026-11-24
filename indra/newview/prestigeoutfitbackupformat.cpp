/**
 * @file prestigeoutfitbackupformat.cpp
 * @brief The outfit backup file format (JSON) and item matching.
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

#include "llviewerprecompiledheaders.h"

#include "prestigeoutfitbackupformat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>

#include <boost/json.hpp>

namespace PrestigeOutfitBackupFormat
{
const char* const FORMAT_ID = "prestige-outfit-backup";
}

namespace
{
    using namespace PrestigeOutfitBackupFormat;

    // Longest name, path or type string kept from a file.
    const size_t MAX_STRING_BYTES = 1024;
    const size_t MAX_SHORT_STRING_BYTES = 64;
    // Longest subfolder list kept from a file.
    const size_t MAX_FOLDERS = 1000;

    bool isHex(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    char toLowerAscii(char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    std::string trimSpaces(const std::string& text)
    {
        const std::string spaces = " \t\r\n";
        const size_t first = text.find_first_not_of(spaces);
        if (first == std::string::npos)
        {
            return std::string();
        }
        const size_t last = text.find_last_not_of(spaces);
        return text.substr(first, last - first + 1);
    }

    std::string getString(const boost::json::object& obj, const char* key, size_t max_bytes)
    {
        const boost::json::value* value = obj.if_contains(key);
        if (!value || !value->is_string())
        {
            return std::string();
        }
        const boost::json::string& str = value->get_string();
        return truncateUTF8(std::string(str.data(), str.size()), max_bytes);
    }

    // Reads a whole number; false when the key is missing or not a whole number.
    bool getInteger(const boost::json::object& obj, const char* key, std::int64_t& out)
    {
        const boost::json::value* value = obj.if_contains(key);
        if (!value)
        {
            return false;
        }
        if (value->is_int64())
        {
            out = value->get_int64();
            return true;
        }
        if (value->is_uint64())
        {
            const std::uint64_t u = value->get_uint64();
            if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            {
                return false;
            }
            out = static_cast<std::int64_t>(u);
            return true;
        }
        if (value->is_double())
        {
            const double d = value->get_double();
            if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 1.0e9)
            {
                out = static_cast<std::int64_t>(d);
                return true;
            }
        }
        return false;
    }

    void appendIndent(std::string& out, size_t indent)
    {
        out.append(indent, ' ');
    }

    // Indented JSON; boost::json::serialize writes everything on one line.
    void writeValue(std::string& out, const boost::json::value& value, size_t indent)
    {
        if (value.is_object())
        {
            const boost::json::object& obj = value.get_object();
            if (obj.empty())
            {
                out += "{}";
                return;
            }
            out += "{\n";
            bool first = true;
            for (const boost::json::key_value_pair& kv : obj)
            {
                if (!first)
                {
                    out += ",\n";
                }
                first = false;
                appendIndent(out, indent + 2);
                out += boost::json::serialize(boost::json::value(kv.key()));
                out += ": ";
                writeValue(out, kv.value(), indent + 2);
            }
            out += "\n";
            appendIndent(out, indent);
            out += "}";
        }
        else if (value.is_array())
        {
            const boost::json::array& arr = value.get_array();
            if (arr.empty())
            {
                out += "[]";
                return;
            }
            out += "[\n";
            bool first = true;
            for (const boost::json::value& element : arr)
            {
                if (!first)
                {
                    out += ",\n";
                }
                first = false;
                appendIndent(out, indent + 2);
                writeValue(out, element, indent + 2);
            }
            out += "\n";
            appendIndent(out, indent);
            out += "]";
        }
        else
        {
            out += boost::json::serialize(value);
        }
    }

    bool parseEntry(const boost::json::object& obj, Entry& entry)
    {
        entry.name = trimSpaces(getString(obj, "name", MAX_STRING_BYTES));
        entry.item_id = normalizeUUID(getString(obj, "item_id", MAX_SHORT_STRING_BYTES));
        entry.asset_id = normalizeUUID(getString(obj, "asset_id", MAX_SHORT_STRING_BYTES));
        entry.creator_id = normalizeUUID(getString(obj, "creator_id", MAX_SHORT_STRING_BYTES));
        entry.asset_type = getString(obj, "asset_type", MAX_SHORT_STRING_BYTES);
        entry.inv_type = getString(obj, "inv_type", MAX_SHORT_STRING_BYTES);
        entry.wearable_type = getString(obj, "wearable_type", MAX_SHORT_STRING_BYTES);
        entry.attach_point = getString(obj, "attach_point", MAX_SHORT_STRING_BYTES);
        std::int64_t point_id = 0;
        if (getInteger(obj, "attach_point_id", point_id) && point_id > 0 && point_id < 256)
        {
            entry.attach_point_id = static_cast<int>(point_id);
        }
        const boost::json::value* no_copy = obj.if_contains("no_copy");
        entry.no_copy = no_copy && no_copy->is_bool() && no_copy->get_bool();
        entry.folder = getString(obj, "folder", MAX_STRING_BYTES);
        // Nothing to look the item up by.
        return !(entry.name.empty() && entry.item_id.empty() && entry.asset_id.empty());
    }
}

namespace PrestigeOutfitBackupFormat
{

std::string normalizeUUID(const std::string& text)
{
    const std::string trimmed = trimSpaces(text);
    if (trimmed.size() != 36)
    {
        return std::string();
    }
    std::string out;
    out.reserve(36);
    bool all_zero = true;
    for (size_t i = 0; i < trimmed.size(); ++i)
    {
        const char c = trimmed[i];
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (c != '-')
            {
                return std::string();
            }
        }
        else if (!isHex(c))
        {
            return std::string();
        }
        else if (c != '0')
        {
            all_zero = false;
        }
        out += toLowerAscii(c);
    }
    return all_zero ? std::string() : out;
}

std::string truncateUTF8(const std::string& text, size_t max_bytes)
{
    if (text.size() <= max_bytes)
    {
        return text;
    }
    size_t cut = max_bytes;
    // text[cut] is the first byte dropped; never keep half a character.
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
    {
        --cut;
    }
    return text.substr(0, cut);
}

std::string restoredFolderName(const std::string& outfit_name, const std::vector<std::string>& taken)
{
    std::string base = trimSpaces(outfit_name);
    if (base.empty())
    {
        base = "Outfit";
    }
    for (int n = 1; n < 1000; ++n)
    {
        const std::string suffix = n == 1 ? std::string(" (restored)") : " (restored " + std::to_string(n) + ")";
        const std::string name = trimSpaces(truncateUTF8(base, MAX_FOLDER_NAME_BYTES - suffix.size())) + suffix;
        if (std::find(taken.begin(), taken.end(), name) == taken.end())
        {
            return name;
        }
    }
    return trimSpaces(truncateUTF8(base, MAX_FOLDER_NAME_BYTES - 11)) + " (restored)";
}

std::string toJson(const Backup& backup)
{
    boost::json::object root;
    root["format"] = FORMAT_ID;
    root["version"] = backup.version;
    root["outfit_name"] = backup.outfit_name;
    root["created"] = backup.created;
    root["source"] = backup.source;
    if (!backup.generator.empty())
    {
        root["generator"] = backup.generator;
    }

    boost::json::array folders;
    for (const std::string& folder : backup.folders)
    {
        folders.emplace_back(folder);
    }
    root["folders"] = std::move(folders);

    boost::json::array items;
    for (const Entry& entry : backup.items)
    {
        boost::json::object item;
        item["name"] = entry.name;
        if (!entry.item_id.empty())
        {
            item["item_id"] = entry.item_id;
        }
        if (!entry.asset_id.empty())
        {
            item["asset_id"] = entry.asset_id;
        }
        item["asset_type"] = entry.asset_type;
        if (!entry.inv_type.empty())
        {
            item["inv_type"] = entry.inv_type;
        }
        if (!entry.wearable_type.empty())
        {
            item["wearable_type"] = entry.wearable_type;
        }
        if (!entry.creator_id.empty())
        {
            item["creator_id"] = entry.creator_id;
        }
        if (!entry.attach_point.empty())
        {
            item["attach_point"] = entry.attach_point;
        }
        if (entry.attach_point_id > 0)
        {
            item["attach_point_id"] = entry.attach_point_id;
        }
        if (entry.no_copy)
        {
            item["no_copy"] = true;
        }
        item["folder"] = entry.folder;
        items.emplace_back(std::move(item));
    }
    root["items"] = std::move(items);

    std::string out;
    writeValue(out, boost::json::value(std::move(root)), 0);
    out += "\n";
    return out;
}

bool fromJson(const std::string& input, Backup& backup, std::string& error)
{
    backup = Backup();
    error.clear();
    try
    {
        if (input.size() > MAX_FILE_BYTES)
        {
            error = "The file is too large to be an outfit backup.";
            return false;
        }
        std::string text = input;
        if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        {
            text.erase(0, 3); // UTF-8 byte order mark
        }
        if (trimSpaces(text).empty())
        {
            error = "The file is empty.";
            return false;
        }

        boost::system::error_code ec;
        const boost::json::value root = boost::json::parse(text, ec);
        if (ec)
        {
            error = "The file is not valid JSON (" + ec.message() + ").";
            return false;
        }
        if (!root.is_object())
        {
            error = "The file is not an outfit backup.";
            return false;
        }
        const boost::json::object& obj = root.get_object();
        if (getString(obj, "format", MAX_SHORT_STRING_BYTES) != FORMAT_ID)
        {
            error = "The file is not a Prestige outfit backup.";
            return false;
        }
        std::int64_t version = 0;
        if (!getInteger(obj, "version", version) || version < 1)
        {
            error = "The backup has no valid format version.";
            return false;
        }
        if (version > VERSION)
        {
            error = "The backup was made by a newer version of Prestige (format "
                    + std::to_string(version) + "). Update the viewer to restore it.";
            return false;
        }
        backup.version = static_cast<int>(version);
        backup.outfit_name = trimSpaces(getString(obj, "outfit_name", MAX_STRING_BYTES));
        backup.created = getString(obj, "created", MAX_SHORT_STRING_BYTES);
        backup.source = getString(obj, "source", MAX_SHORT_STRING_BYTES);
        backup.generator = getString(obj, "generator", MAX_STRING_BYTES);

        const boost::json::value* folders = obj.if_contains("folders");
        if (folders && folders->is_array())
        {
            for (const boost::json::value& folder : folders->get_array())
            {
                if (folder.is_string() && backup.folders.size() < MAX_FOLDERS)
                {
                    const boost::json::string& str = folder.get_string();
                    backup.folders.push_back(truncateUTF8(std::string(str.data(), str.size()), MAX_STRING_BYTES));
                }
            }
        }

        const boost::json::value* items = obj.if_contains("items");
        if (!items || !items->is_array())
        {
            error = "The backup has no item list.";
            return false;
        }
        const boost::json::array& list = items->get_array();
        if (list.size() > MAX_ITEMS)
        {
            error = "The backup lists too many items (" + std::to_string(list.size()) + ").";
            return false;
        }
        for (const boost::json::value& element : list)
        {
            if (!element.is_object())
            {
                continue;
            }
            Entry entry;
            if (parseEntry(element.get_object(), entry))
            {
                backup.items.push_back(std::move(entry));
            }
        }
        return true;
    }
    catch (const std::exception& e)
    {
        backup = Backup();
        error = std::string("The file could not be read (") + e.what() + ").";
        return false;
    }
    catch (...)
    {
        backup = Backup();
        error = "The file could not be read.";
        return false;
    }
}

// static
std::string Matcher::nameKey(const std::string& asset_type, const std::string& name)
{
    return asset_type + '\n' + name;
}

void Matcher::add(const Candidate& candidate)
{
    Candidate c = candidate;
    c.item_id = normalizeUUID(c.item_id);
    c.asset_id = normalizeUUID(c.asset_id);
    c.creator_id = normalizeUUID(c.creator_id);

    const size_t index = mCandidates.size();
    if (!c.item_id.empty())
    {
        mByItem.emplace(c.item_id, index);
    }
    if (!c.asset_id.empty())
    {
        mByAsset.emplace(c.asset_id, index);
    }
    if (!c.name.empty() && !c.asset_type.empty())
    {
        mByName.emplace(nameKey(c.asset_type, c.name), index);
    }
    mCandidates.push_back(std::move(c));
}

EMatch Matcher::find(const Entry& entry, size_t& index) const
{
    if (!entry.item_id.empty())
    {
        const auto it = mByItem.find(entry.item_id);
        if (it != mByItem.end())
        {
            index = it->second;
            return MATCH_ITEM_ID;
        }
    }

    if (!entry.asset_id.empty())
    {
        // Several copies can share an asset; prefer one with the same name.
        bool found = false;
        int best_score = -1;
        const auto range = mByAsset.equal_range(entry.asset_id);
        for (auto it = range.first; it != range.second; ++it)
        {
            const Candidate& c = mCandidates[it->second];
            if (!entry.asset_type.empty() && c.asset_type != entry.asset_type)
            {
                continue;
            }
            const int score = (c.name == entry.name) ? 1 : 0;
            if (score > best_score)
            {
                best_score = score;
                index = it->second;
                found = true;
            }
        }
        if (found)
        {
            return MATCH_ASSET_ID;
        }
    }

    if (!entry.name.empty() && !entry.asset_type.empty())
    {
        // Same name and type; prefer the same creator.
        bool found = false;
        int best_score = -1;
        const auto range = mByName.equal_range(nameKey(entry.asset_type, entry.name));
        for (auto it = range.first; it != range.second; ++it)
        {
            const Candidate& c = mCandidates[it->second];
            if (!entry.wearable_type.empty() && !c.wearable_type.empty() && c.wearable_type != entry.wearable_type)
            {
                continue;
            }
            const int score = (!entry.creator_id.empty() && c.creator_id == entry.creator_id) ? 1 : 0;
            if (score > best_score)
            {
                best_score = score;
                index = it->second;
                found = true;
            }
        }
        if (found)
        {
            return MATCH_NAME;
        }
    }

    return MATCH_NONE;
}

} // namespace PrestigeOutfitBackupFormat
