/**
 * @file prestigeoutfitbackup.cpp
 * @brief Outfit backup to a JSON file and restore from it.
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

#include "prestigeoutfitbackup.h"
#include "prestigeoutfitbackupformat.h"

#include "llagent.h"
#include "llaisapi.h"
#include "llappearancemgr.h"
#include "lldate.h"
#include "lldir.h"
#include "llfile.h"
#include "llfilepicker.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llnotificationsutil.h"
#include "llversioninfo.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewermenufile.h"
#include "llvoavatarself.h"
#include "llwearabletype.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace
{
    namespace obk = PrestigeOutfitBackupFormat;

    // Most missing item names listed in the restore report.
    const size_t MAX_MISSING_LISTED = 10;
    // Deepest subfolder path written to a backup.
    const S32 MAX_FOLDER_DEPTH = 32;

    const char* const SOURCE_OUTFIT = "outfit";
    const char* const SOURCE_CURRENT_LOOK = "current_look";

    std::string uuidString(const LLUUID& id)
    {
        return id.isNull() ? std::string() : id.asString();
    }

    std::string assetTypeName(LLAssetType::EType type)
    {
        const char* name = LLAssetType::lookup(type);
        return name ? std::string(name) : std::string();
    }

    void showError(const std::string& message)
    {
        LLSD args;
        args["MESSAGE"] = message;
        LLNotificationsUtil::add("PrestigeOutfitBackupError", args);
    }

    // Path of folder_id below root_id, such as "Sub/Inner"; "" for root_id itself.
    std::string relativeFolderPath(const LLUUID& folder_id, const LLUUID& root_id)
    {
        std::vector<std::string> parts;
        LLUUID id = folder_id;
        for (S32 depth = 0; depth < MAX_FOLDER_DEPTH && id.notNull() && id != root_id; ++depth)
        {
            const LLViewerInventoryCategory* cat = gInventory.getCategory(id);
            if (!cat)
            {
                break;
            }
            parts.push_back(cat->getName());
            id = cat->getParentUUID();
        }
        std::string path;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        {
            if (!path.empty())
            {
                path += "/";
            }
            path += *it;
        }
        return path;
    }

    // Where an attachment is worn now; false when it is not worn.
    bool wornAttachmentPoint(const LLUUID& item_id, S32& point_id, std::string& point_name)
    {
        if (!isAgentAvatarValid())
        {
            return false;
        }
        for (const auto& kv : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* point = kv.second;
            if (point && point->getAttachedObject(item_id))
            {
                point_id = kv.first;
                point_name = point->getName();
                return true;
            }
        }
        return false;
    }

    obk::Entry makeEntry(const LLViewerInventoryItem* link_or_item, const LLUUID& root_id)
    {
        obk::Entry entry;
        entry.folder = relativeFolderPath(link_or_item->getParentUUID(), root_id);

        const LLViewerInventoryItem* item = link_or_item->getIsLinkType() ? link_or_item->getLinkedItem() : link_or_item;
        if (!item)
        {
            // Broken link: the item is gone already. Keep what the link knows,
            // so a restore can still find the item if it comes back.
            entry.name = link_or_item->getName();
            entry.item_id = uuidString(link_or_item->getLinkedUUID());
            return entry;
        }

        entry.name = item->getName();
        entry.item_id = uuidString(item->getUUID());
        entry.asset_id = uuidString(item->getAssetUUID());
        entry.creator_id = uuidString(item->getCreatorUUID());
        entry.asset_type = assetTypeName(item->getType());
        entry.inv_type = LLInventoryType::lookup(item->getInventoryType());
        if (item->isWearableType())
        {
            entry.wearable_type = LLWearableType::getInstance()->getTypeName(item->getWearableType());
        }
        entry.no_copy = !item->getPermissions().allowCopyBy(gAgent.getID());
        if (item->getType() == LLAssetType::AT_OBJECT)
        {
            S32 point_id = 0;
            std::string point_name;
            if (wornAttachmentPoint(item->getUUID(), point_id, point_name))
            {
                entry.attach_point = point_name;
                entry.attach_point_id = point_id;
            }
        }
        return entry;
    }

    obk::Backup collectBackup(const LLUUID& root_id, const std::string& outfit_name, const char* source)
    {
        obk::Backup backup;
        backup.outfit_name = outfit_name;
        backup.created = LLDate::now().asString();
        backup.source = source;
        backup.generator = LLVersionInfo::instance().getChannelAndVersion();

        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(root_id, cats, items, LLInventoryModel::EXCLUDE_TRASH);
        for (const LLPointer<LLViewerInventoryCategory>& cat : cats)
        {
            if (cat.notNull())
            {
                backup.folders.push_back(relativeFolderPath(cat->getUUID(), root_id));
            }
        }
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            // Folder links (the base outfit link in the Current Outfit folder) are not items.
            if (item.isNull() || item->getActualType() == LLAssetType::AT_LINK_FOLDER)
            {
                continue;
            }
            backup.items.push_back(makeEntry(item.get(), root_id));
        }
        return backup;
    }

    void writeBackup(const LLUUID& root_id, const std::string& outfit_name, const char* source, const std::string& path)
    {
        if (!gInventory.getCategory(root_id))
        {
            showError("The outfit could not be found. Was it deleted?");
            return;
        }
        const obk::Backup backup = collectBackup(root_id, outfit_name, source);
        if (backup.items.empty())
        {
            showError("\"" + outfit_name + "\" has no items to back up.");
            return;
        }
        const std::string text = obk::toJson(backup);
        {
            llofstream file(path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                showError("Could not write \"" + path + "\". Check that the folder exists and is writable.");
                return;
            }
            file << text;
            file.close();
            if (file.fail())
            {
                showError("Could not finish writing \"" + path + "\". The disk may be full.");
                return;
            }
        }
        LL_INFOS("OutfitBackup") << "Backed up " << backup.items.size() << " items of '" << outfit_name
                                 << "' to " << path << LL_ENDL;
        LLSD args;
        args["NAME"] = outfit_name;
        args["COUNT"] = (S32)backup.items.size();
        args["FILE"] = gDirUtilp->getBaseFileName(path);
        LLNotificationsUtil::add("PrestigeOutfitBackupSaved", args);
    }

    // The folder and the items its links point to are in memory.
    bool isOutfitLoaded(const LLUUID& cat_id)
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
        if (!cat || cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN
            || cat->getDescendentCount() == LLViewerInventoryCategory::DESCENDENT_COUNT_UNKNOWN
            || cat->getDescendentCount() != cat->getViewerDescendentCount())
        {
            return false;
        }
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(cat_id, cats, items, LLInventoryModel::EXCLUDE_TRASH);
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            if (item.notNull() && item->getActualType() == LLAssetType::AT_LINK && !item->getLinkedItem())
            {
                return false;
            }
        }
        return true;
    }

    // ---------------------------------------------------------------- restore

    bool readFile(const std::string& path, std::string& text, std::string& error)
    {
        llifstream file(path, std::ios::in | std::ios::binary);
        if (!file.is_open())
        {
            error = "The file could not be opened.";
            return false;
        }
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        if (size < 0)
        {
            error = "The file could not be read.";
            return false;
        }
        if (static_cast<unsigned long long>(size) > obk::MAX_FILE_BYTES)
        {
            error = "The file is too large to be an outfit backup.";
            return false;
        }
        file.seekg(0, std::ios::beg);
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (file.bad())
        {
            error = "The file could not be read.";
            return false;
        }
        return true;
    }

    std::string missingLabel(const obk::Entry& entry)
    {
        std::string label = entry.name.empty() ? std::string("(unnamed item)") : entry.name;
        if (entry.no_copy)
        {
            label += " (no-copy)";
        }
        return label;
    }

    struct RestorePlan
    {
        LLInventoryObject::const_object_list_t to_link;
        S32 total = 0;
        S32 found = 0;
        S32 by_name = 0;
        bool had_attach_points = false;
        std::vector<std::string> missing;
    };

    void buildRestorePlan(const obk::Backup& backup, RestorePlan& plan)
    {
        // Every linkable item in the agent's own inventory, outside the trash
        // and the Marketplace listings; links are not candidates.
        const LLUUID marketplace_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MARKETPLACE_LISTINGS);
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH);

        obk::Matcher matcher;
        std::vector<LLUUID> candidate_ids;
        candidate_ids.reserve(items.size());
        for (const LLPointer<LLViewerInventoryItem>& item : items)
        {
            if (item.isNull() || item->getIsLinkType() || !LLAssetType::lookupCanLink(item->getType()))
            {
                continue;
            }
            if (marketplace_id.notNull() && gInventory.isObjectDescendentOf(item->getUUID(), marketplace_id))
            {
                continue;
            }
            obk::Candidate candidate;
            candidate.item_id = uuidString(item->getUUID());
            candidate.asset_id = uuidString(item->getAssetUUID());
            candidate.creator_id = uuidString(item->getCreatorUUID());
            candidate.name = item->getName();
            candidate.asset_type = assetTypeName(item->getType());
            if (item->isWearableType())
            {
                candidate.wearable_type = LLWearableType::getInstance()->getTypeName(item->getWearableType());
            }
            matcher.add(candidate);
            candidate_ids.push_back(item->getUUID());
        }

        std::set<LLUUID> linked;
        for (const obk::Entry& entry : backup.items)
        {
            ++plan.total;
            if (!entry.attach_point.empty())
            {
                plan.had_attach_points = true;
            }
            size_t index = 0;
            const obk::EMatch match = matcher.find(entry, index);
            const LLViewerInventoryItem* item = nullptr;
            if (match != obk::MATCH_NONE && index < candidate_ids.size())
            {
                item = gInventory.getItem(candidate_ids[index]);
            }
            if (!item)
            {
                plan.missing.push_back(missingLabel(entry));
                continue;
            }
            ++plan.found;
            if (match == obk::MATCH_NAME)
            {
                ++plan.by_name;
            }
            // Two entries found as the same item get one link.
            if (linked.insert(item->getUUID()).second)
            {
                plan.to_link.push_back(item);
            }
        }
    }

    std::string reportDetails(const RestorePlan& plan, bool inventory_complete)
    {
        std::string details;
        if (plan.by_name > 0)
        {
            details += "\n\n" + std::to_string(plan.by_name)
                       + (plan.by_name == 1 ? " item was" : " items were")
                       + " found by name and type only; check that they are the right ones.";
        }
        if (!plan.missing.empty())
        {
            details += "\n\nMissing: ";
            const size_t listed = std::min(plan.missing.size(), MAX_MISSING_LISTED);
            for (size_t i = 0; i < listed; ++i)
            {
                if (i > 0)
                {
                    details += ", ";
                }
                details += plan.missing[i];
            }
            if (plan.missing.size() > listed)
            {
                details += " and " + std::to_string(plan.missing.size() - listed) + " more";
            }
            details += ".";
        }
        if (plan.had_attach_points)
        {
            details += "\n\nAttachments go to the points they were last worn on, as with any outfit.";
        }
        if (!inventory_complete)
        {
            details += "\n\nYour inventory was still loading, so some items may not have been found."
                       " If this outfit is incomplete, delete it and restore again later.";
        }
        return details;
    }

    void restoreFromPath(const std::string& path)
    {
        const std::string file_name = gDirUtilp->getBaseFileName(path);
        std::string text;
        std::string error;
        obk::Backup backup;
        if (!readFile(path, text, error) || !obk::fromJson(text, backup, error))
        {
            showError("Could not restore an outfit from \"" + file_name + "\". " + error);
            return;
        }
        if (backup.items.empty())
        {
            showError("\"" + file_name + "\" lists no items, so there is nothing to restore.");
            return;
        }

        const LLUUID my_outfits_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_MY_OUTFITS);
        if (my_outfits_id.isNull())
        {
            showError("Your My Outfits folder could not be found. Try again once your inventory has loaded.");
            return;
        }

        const bool inventory_complete = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
        if (!inventory_complete)
        {
            LLInventoryModelBackgroundFetch::instance().start();
        }

        RestorePlan plan;
        buildRestorePlan(backup, plan);
        if (plan.found == 0 || plan.to_link.empty())
        {
            std::string message = "None of the " + std::to_string(plan.total)
                                  + " items in \"" + file_name + "\" were found in your inventory.";
            if (!inventory_complete)
            {
                message += " Your inventory is still loading; try again in a moment.";
            }
            showError(message);
            return;
        }

        // A unique name among the outfits and folders in My Outfits.
        std::vector<std::string> taken;
        LLInventoryModel::cat_array_t* outfit_cats = nullptr;
        LLInventoryModel::item_array_t* outfit_items = nullptr;
        gInventory.getDirectDescendentsOf(my_outfits_id, outfit_cats, outfit_items);
        if (outfit_cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *outfit_cats)
            {
                if (cat.notNull())
                {
                    taken.push_back(cat->getName());
                }
            }
        }
        std::string outfit_name = backup.outfit_name;
        LLInventoryObject::correctInventoryName(outfit_name);
        const std::string folder_name = obk::restoredFolderName(outfit_name, taken);

        LLSD args;
        args["FOUND"] = plan.found;
        args["TOTAL"] = plan.total;
        args["FOLDER"] = folder_name;
        args["DETAILS"] = reportDetails(plan, inventory_complete);
        const LLInventoryObject::const_object_list_t to_link = plan.to_link;

        LL_INFOS("OutfitBackup") << "Restoring '" << folder_name << "' from " << path << ": " << plan.found
                                 << " of " << plan.total << " items found (" << plan.by_name << " by name)" << LL_ENDL;

        gInventory.createNewCategory(
            my_outfits_id,
            LLFolderType::FT_OUTFIT,
            folder_name,
            [args, to_link](const LLUUID& new_cat_id)
            {
                if (new_cat_id.isNull())
                {
                    showError("The new outfit folder could not be created. Try again later.");
                    return;
                }
                // The report shows once every link request has been answered.
                LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback(
                    no_op_inventory_func,
                    [args]()
                    {
                        LLNotificationsUtil::add("PrestigeOutfitRestoreReport", args);
                    });
                LLInvCreateLog::Site inv_site("outfit restore from file");
                link_inventory_array_verified(new_cat_id, to_link, cb);
            });
    }
}

// static
void PrestigeOutfitBackup::backupOutfit(const LLUUID& outfit_id)
{
    const LLViewerInventoryCategory* cat = gInventory.getCategory(outfit_id);
    if (!cat)
    {
        showError("Select an outfit to back up.");
        return;
    }
    const std::string outfit_name = cat->getName();
    LLFilePickerReplyThread::startPicker(
        [outfit_id, outfit_name](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (files.empty())
            {
                return;
            }
            const std::string path = files.front();
            if (isOutfitLoaded(outfit_id) || !AISAPI::isAvailable())
            {
                writeBackup(outfit_id, outfit_name, SOURCE_OUTFIT, path);
                return;
            }
            // Fetch the folder and the items its links point to first.
            LLInventoryModelBackgroundFetch::instance().fetchFolderAndLinks(outfit_id,
                [outfit_id, outfit_name, path]()
                {
                    writeBackup(outfit_id, outfit_name, SOURCE_OUTFIT, path);
                });
        },
        LLFilePicker::FFSAVE_ALL, outfit_name + ".json");
}

// static
void PrestigeOutfitBackup::backupCurrentLook()
{
    const LLUUID cof_id = LLAppearanceMgr::instance().getCOF();
    if (cof_id.isNull())
    {
        showError("What you are wearing is not known yet. Try again in a moment.");
        return;
    }
    std::string look_name;
    if (!LLAppearanceMgr::instance().getBaseOutfitName(look_name) || look_name.empty())
    {
        look_name = "Current look";
    }
    LLFilePickerReplyThread::startPicker(
        [cof_id, look_name](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (!files.empty())
            {
                writeBackup(cof_id, look_name, SOURCE_CURRENT_LOOK, files.front());
            }
        },
        LLFilePicker::FFSAVE_ALL, look_name + ".json");
}

// static
void PrestigeOutfitBackup::restoreFromFile()
{
    LLFilePickerReplyThread::startPicker(
        [](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (!files.empty())
            {
                restoreFromPath(files.front());
            }
        },
        LLFilePicker::FFLOAD_ALL, false);
}
