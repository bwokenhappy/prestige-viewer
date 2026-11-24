/**
 * @file prestigefloaterquicklibrary.cpp
 * @brief Prestige quick library (Ctrl+Space).
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

#include "prestigefloaterquicklibrary.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfiltereditor.h"
#include "llfocusmgr.h"
#include "llgesturemgr.h"
#include "llinventorybridge.h"
#include "llinventoryfunctions.h"
#include "llinventoryicon.h"
#include "llinventorymodel.h"
#include "lltextbox.h"
#include "lltimer.h"
#include "lltooldraganddrop.h"
#include "llviewerassettype.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"

#include <algorithm>

static LLDefaultChildRegistry::Register<PrestigeQuickLibraryList> r_prestige_quick_library_list("prestige_quick_library_list");

namespace
{
    // The item types the window lists, in the order of its type check boxes.
    struct Kind
    {
        LLAssetType::EType mType;
        const char*        mCheck;  // check box name
        const char*        mLabel;  // floater string for the Type column
    };

    const Kind KINDS[] =
    {
        { LLAssetType::AT_TEXTURE,   "type_textures",   "type_texture" },
        { LLAssetType::AT_MATERIAL,  "type_materials",  "type_material" },
        { LLAssetType::AT_OBJECT,    "type_objects",    "type_object" },
        { LLAssetType::AT_ANIMATION, "type_animations", "type_animation" },
        { LLAssetType::AT_SOUND,     "type_sounds",     "type_sound" },
        { LLAssetType::AT_NOTECARD,  "type_notecards",  "type_notecard" },
        { LLAssetType::AT_LANDMARK,  "type_landmarks",  "type_landmark" },
        { LLAssetType::AT_LSL_TEXT,  "type_scripts",    "type_script" },
        { LLAssetType::AT_GESTURE,   "type_gestures",   "type_gesture" },
    };
    constexpr S32 KIND_COUNT = S32(sizeof(KINDS) / sizeof(KINDS[0]));
    constexpr U32 ALL_KINDS  = (1u << KIND_COUNT) - 1u;

    // The "Library folder" scope: a folder of this name at the top of My Inventory.
    // Only a convention: the user creates it (or lets the window create it) and
    // fills it with whatever they like, links included.
    const char* const LIBRARY_FOLDER_NAME = "Prestige Library";

    const char* const SETTING_RECENT = "PrestigeQuickLibraryRecent";
    const char* const SETTING_TYPES  = "PrestigeQuickLibraryTypes";
    const char* const SETTING_SCOPE  = "PrestigeQuickLibraryScope";

    constexpr S32    MAX_RESULTS     = 500;     // rows put in the list
    constexpr size_t MAX_CACHED      = 250000;  // items read from the inventory per refresh
    constexpr size_t MAX_RECENT      = 20;
    constexpr F32    REFRESH_SECONDS = 1.5f;    // at most one re-read per this while the inventory changes
    constexpr F32    REFRESH_MAX     = 15.f;    // ... or per this for an inventory that takes long to read

    S32 kind_of(LLAssetType::EType type)
    {
        for (S32 i = 0; i < KIND_COUNT; ++i)
        {
            if (KINDS[i].mType == type)
            {
                return i;
            }
        }
        return -1;
    }

    std::string fold(const std::string& s)
    {
        std::string out = s;
        LLStringUtil::toLower(out);
        return out;
    }

    // A listed item, or a link to one (the original is what the window opens and drags).
    const LLViewerInventoryItem* resolve(const LLViewerInventoryItem* item)
    {
        if (item && item->getIsLinkType())
        {
            return item->getLinkedItem();
        }
        return item;
    }

    class QuickLibraryCollector : public LLInventoryCollectFunctor
    {
    public:
        QuickLibraryCollector(bool allow_links) : mAllowLinks(allow_links) {}

        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            if (!item)
            {
                return false;
            }
            LLAssetType::EType type = item->getType();
            if (item->getIsLinkType())
            {
                // Links only count in the Library folder (in All inventory the original is listed already).
                if (!mAllowLinks)
                {
                    return false;
                }
                const LLViewerInventoryItem* target = gInventory.getItem(item->getLinkedUUID());
                if (!target || target->getIsLinkType())
                {
                    return false;
                }
                type = target->getType();
            }
            if (kind_of(type) < 0)
            {
                return false;
            }
            ++mCount;
            return true;
        }

        bool exceedsLimit() override { return mCount >= MAX_CACHED; }

    private:
        const bool mAllowLinks;
        size_t     mCount = 0;
    };
}

//-----------------------------------------------------------------------------
// PrestigeQuickLibraryList
//-----------------------------------------------------------------------------

PrestigeQuickLibraryList::PrestigeQuickLibraryList(const Params& p)
:   LLScrollListCtrl(p)
{
}

bool PrestigeQuickLibraryList::handleMouseDown(S32 x, S32 y, MASK mask)
{
    LLScrollListItem* hit = hitItem(x, y);
    mDragItem  = hit ? hit->getUUID() : LLUUID::null;
    mDragArmed = mDragItem.notNull() && mask == MASK_NONE;
    if (mDragArmed)
    {
        S32 screen_x = 0;
        S32 screen_y = 0;
        localPointToScreen(x, y, &screen_x, &screen_y);
        LLToolDragAndDrop::getInstance()->setDragStart(screen_x, screen_y);
    }
    return LLScrollListCtrl::handleMouseDown(x, y, mask);
}

bool PrestigeQuickLibraryList::handleMouseUp(S32 x, S32 y, MASK mask)
{
    mDragArmed = false;
    return LLScrollListCtrl::handleMouseUp(x, y, mask);
}

bool PrestigeQuickLibraryList::handleHover(S32 x, S32 y, MASK mask)
{
    if (mDragArmed && hasMouseCapture())
    {
        S32 screen_x = 0;
        S32 screen_y = 0;
        localPointToScreen(x, y, &screen_x, &screen_y);
        if (LLToolDragAndDrop::getInstance()->isOverThreshold(screen_x, screen_y))
        {
            // The standard inventory drag: objects rez where they are dropped, textures and
            // materials go on faces, anything can go into an object's contents or to an avatar.
            mDragArmed = false;
            const LLUUID item_id = mDragItem;
            const LLViewerInventoryItem* item = gInventory.getItem(item_id);
            if (item)
            {
                const EDragAndDropType type = LLViewerAssetType::lookupDragAndDropType(item->getType());
                if (type != DAD_NONE)
                {
                    LLToolDragAndDrop::getInstance()->beginDrag(type, item_id, LLToolDragAndDrop::SOURCE_AGENT);
                    if (mDragStartedCallback)
                    {
                        mDragStartedCallback(item_id);
                    }
                    return true;
                }
            }
        }
    }
    return LLScrollListCtrl::handleHover(x, y, mask);
}

bool PrestigeQuickLibraryList::handleKeyHere(KEY key, MASK mask)
{
    if (key == KEY_RETURN && mask == MASK_NONE && mActivateCallback)
    {
        const LLUUID item_id = getStringUUIDSelectedItem();
        if (item_id.notNull())
        {
            mActivateCallback(item_id);
            return true;
        }
    }
    return LLScrollListCtrl::handleKeyHere(key, mask);
}

//-----------------------------------------------------------------------------
// PrestigeFloaterQuickLibrary
//-----------------------------------------------------------------------------

PrestigeFloaterQuickLibrary::PrestigeFloaterQuickLibrary(const LLSD& key)
:   LLFloater(key)
{
}

PrestigeFloaterQuickLibrary::~PrestigeFloaterQuickLibrary()
{
    if (gInventory.containsObserver(this))
    {
        gInventory.removeObserver(this);
    }
}

bool PrestigeFloaterQuickLibrary::postBuild()
{
    mSearch          = getChild<LLFilterEditor>("search");
    mScopeCombo      = getChild<LLComboBox>("scope");
    mFavoritesCheck  = getChild<LLCheckBoxCtrl>("favorites_only");
    mRecentCheck     = getChild<LLCheckBoxCtrl>("recent_only");
    mList            = getChild<PrestigeQuickLibraryList>("results");
    mCreateFolderBtn = getChild<LLButton>("create_folder");
    mOpenBtn         = getChild<LLButton>("open");
    mFavoriteBtn     = getChild<LLButton>("favorite");
    mShowBtn         = getChild<LLButton>("show_in_inventory");
    mStatusText      = getChild<LLTextBox>("status");

    const U32 type_mask = gSavedSettings.getU32(SETTING_TYPES);
    mTypeChecks.clear();
    for (S32 i = 0; i < KIND_COUNT; ++i)
    {
        LLCheckBoxCtrl* check = getChild<LLCheckBoxCtrl>(KINDS[i].mCheck);
        check->set((type_mask & (1u << i)) != 0);
        check->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTypesChanged(); });
        mTypeChecks.push_back(check);
    }

    mScopeCombo->setValue(gSavedSettings.getU32(SETTING_SCOPE) == 1 ? LLSD("library") : LLSD("all"));
    mScopeCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onScopeChanged(); });
    mSearch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFilterChanged(); });
    mFavoritesCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { rebuildList(); });
    mRecentCheck->setCommitCallback([this](LLUICtrl*, const LLSD&) { rebuildList(); });

    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
    mList->setDoubleClickCallback([this]()
    {
        const LLUUID item_id = selectedItem();
        if (item_id.notNull())
        {
            activateItem(item_id);
        }
    });
    mList->setActivateCallback([this](const LLUUID& item_id) { activateItem(item_id); });
    mList->setDragStartedCallback([this](const LLUUID& item_id) { noteUsed(item_id); });

    mCreateFolderBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onCreateLibraryFolder(); });
    mOpenBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        const LLUUID item_id = selectedItem();
        if (item_id.notNull())
        {
            activateItem(item_id);
        }
    });
    mFavoriteBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onToggleFavorite(); });
    mShowBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { onShowInInventory(); });

    gInventory.addObserver(this);
    return true;
}

void PrestigeFloaterQuickLibrary::onOpen(const LLSD& key)
{
    // A fresh search each time; the inventory is re-read when it changed since the last look.
    mSearch->clear();
    if (mCacheDirty)
    {
        rebuildCache();
    }
    rebuildList();
    mSearch->setFocus(true);
}

void PrestigeFloaterQuickLibrary::draw()
{
    // Inventory changes (items added, renamed, moved, favourites) re-read the inventory,
    // at most once per mRefreshInterval and not in the middle of a click or drag on the list.
    if (mCacheDirty
        && mRefreshTimer.getElapsedTimeF32() >= mRefreshInterval
        && !(mList && mList->hasMouseCapture()))
    {
        rebuildCache();
        rebuildList();
    }
    LLFloater::draw();
}

bool PrestigeFloaterQuickLibrary::handleKeyHere(KEY key, MASK mask)
{
    // From the search box: Up / Down walk the results, Enter opens the selected (or first) one.
    if (mask == MASK_NONE && mList && mList->getItemCount() > 0)
    {
        if (key == KEY_DOWN || key == KEY_UP)
        {
            if (key == KEY_DOWN)
            {
                mList->selectNextItem(false);
            }
            else
            {
                mList->selectPrevItem(false);
            }
            mList->scrollToShowSelected();
            updateButtons();
            return true;
        }
        if (key == KEY_RETURN)
        {
            LLUUID item_id = selectedItem();
            if (item_id.isNull())
            {
                mList->selectFirstItem();
                item_id = selectedItem();
            }
            if (item_id.notNull())
            {
                activateItem(item_id);
                return true;
            }
        }
    }
    return LLFloater::handleKeyHere(key, mask);
}

void PrestigeFloaterQuickLibrary::changed(U32 mask)
{
    // Gesture activation and calling card status don't change what is listed.
    const U32 ignored = LLInventoryObserver::GESTURE | LLInventoryObserver::CALLING_CARD | LLInventoryObserver::SORT;
    if (mask & ~ignored)
    {
        mCacheDirty = true;
    }
}

// static
LLUUID PrestigeFloaterQuickLibrary::findLibraryFolder()
{
    const LLUUID& root = gInventory.getRootFolderID();
    if (root.isNull())
    {
        return LLUUID::null;
    }
    LLInventoryModel::cat_array_t*  cats  = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(root, cats, items);
    if (cats)
    {
        const std::string wanted(LIBRARY_FOLDER_NAME);
        for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
        {
            if (cat.notNull() && LLStringUtil::compareInsensitive(cat->getName(), wanted) == 0)
            {
                return cat->getUUID();
            }
        }
    }
    return LLUUID::null;
}

const PrestigeFloaterQuickLibrary::FolderInfo& PrestigeFloaterQuickLibrary::folderInfo(const LLUUID& cat_id, S32 depth)
{
    auto found = mFolders.find(cat_id);
    if (found != mFolders.end())
    {
        return found->second;
    }
    FolderInfo info;
    const LLViewerInventoryCategory* cat = gInventory.getCategory(cat_id);
    if (cat && cat_id != gInventory.getRootFolderID() && depth < 64)
    {
        const FolderInfo& parent = folderInfo(cat->getParentUUID(), depth + 1);
        info.mPath     = parent.mPath.empty() ? cat->getName() : parent.mPath + " / " + cat->getName();
        info.mFavorite = parent.mFavorite || cat->getIsFavorite();
    }
    return mFolders.emplace(cat_id, std::move(info)).first->second;
}

void PrestigeFloaterQuickLibrary::rebuildCache()
{
    LLTimer read_time;
    mCacheDirty = false;
    mEntries.clear();
    mIndex.clear();
    mFolders.clear();
    mCacheTruncated = false;

    const bool library_scope = mScopeCombo && mScopeCombo->getValue().asString() == "library";
    mScopeRoot = library_scope ? findLibraryFolder() : gInventory.getRootFolderID();
    if (mScopeRoot.isNull())
    {
        mRefreshTimer.reset();
        return;
    }

    LLInventoryModel::cat_array_t  cats;
    LLInventoryModel::item_array_t items;
    QuickLibraryCollector collector(library_scope);
    gInventory.collectDescendentsIf(mScopeRoot, cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);
    mCacheTruncated = collector.exceedsLimit();

    mEntries.reserve(items.size());
    for (const LLPointer<LLViewerInventoryItem>& listed : items)
    {
        const LLViewerInventoryItem* item = resolve(listed.get());
        if (!item)
        {
            continue;
        }
        const S32 kind = kind_of(item->getType());
        if (kind < 0)
        {
            continue;
        }
        // The folder shown is where it sits (for a link, the folder the link is in).
        const FolderInfo& folder = folderInfo(listed->getParentUUID());
        Entry entry;
        entry.mID       = item->getUUID();
        entry.mName     = item->getName();
        entry.mFolded   = fold(entry.mName);
        entry.mFolder   = &folder;
        entry.mType     = item->getType();
        entry.mInvType  = item->getInventoryType();
        entry.mFlags    = item->getFlags();
        entry.mKind     = kind;
        entry.mFavorite = item->getIsFavorite() || folder.mFavorite;
        mEntries.push_back(std::move(entry));
    }

    std::sort(mEntries.begin(), mEntries.end(), [](const Entry& a, const Entry& b)
    {
        if (a.mFolded != b.mFolded)
        {
            return a.mFolded < b.mFolded;
        }
        return a.mName < b.mName;
    });

    // One row per item (an original and its link in the Library folder show once).
    std::vector<Entry> unique;
    unique.reserve(mEntries.size());
    for (Entry& entry : mEntries)
    {
        if (mIndex.emplace(entry.mID, unique.size()).second)
        {
            unique.push_back(std::move(entry));
        }
    }
    mEntries.swap(unique);

    // A big inventory that takes long to read is re-read less often while it keeps changing
    // (during login, for example), so the window doesn't stutter.
    const F32 read_seconds = read_time.getElapsedTimeF32().value();
    mRefreshInterval = llclamp(read_seconds * 20.f, REFRESH_SECONDS, REFRESH_MAX);
    mRefreshTimer.reset();
}

void PrestigeFloaterQuickLibrary::rebuildList()
{
    if (!mList)
    {
        return;
    }
    const LLUUID selected = mList->getStringUUIDSelectedItem();
    mList->deleteAllItems();

    std::string query = mSearch ? mSearch->getText() : std::string();
    LLStringUtil::trim(query);
    query = fold(query);

    U32 type_mask = 0;
    for (size_t i = 0; i < mTypeChecks.size(); ++i)
    {
        if (mTypeChecks[i] && mTypeChecks[i]->get())
        {
            type_mask |= 1u << i;
        }
    }
    const bool favorites_only = mFavoritesCheck && mFavoritesCheck->get();
    const bool recent_only    = mRecentCheck && mRecentCheck->get();

    std::vector<std::string> labels;
    for (S32 i = 0; i < KIND_COUNT; ++i)
    {
        labels.push_back(getString(KINDS[i].mLabel));
    }
    const std::string top_level = getString("top_level");

    S32 shown = 0;
    S32 total = 0;
    auto consider = [&](const Entry& entry)
    {
        if (!(type_mask & (1u << entry.mKind))
            || (favorites_only && !entry.mFavorite)
            || (!query.empty() && entry.mFolded.find(query) == std::string::npos))
        {
            return;
        }
        ++total;
        if (shown >= MAX_RESULTS)
        {
            return;
        }
        ++shown;
        LLSD row;
        row["id"] = entry.mID;
        LLSD& columns = row["columns"];
        columns[0]["column"] = "icon";
        columns[0]["type"]   = "icon";
        columns[0]["value"]  = LLInventoryIcon::getIconName(entry.mType, entry.mInvType, entry.mFlags);
        columns[1]["column"] = "name";
        columns[1]["value"]  = entry.mName;
        if (entry.mFavorite)
        {
            columns[1]["font"]["name"]  = "SANSSERIF_SMALL";
            columns[1]["font"]["style"] = "BOLD";
        }
        columns[2]["column"] = "type";
        columns[2]["value"]  = labels[size_t(entry.mKind)];
        columns[3]["column"] = "folder";
        columns[3]["value"]  = (entry.mFolder && !entry.mFolder->mPath.empty()) ? entry.mFolder->mPath : top_level;
        mList->addElement(row);
    };

    if (recent_only)
    {
        // Most recently used first
        const LLSD recent = gSavedSettings.getLLSD(SETTING_RECENT);
        if (recent.isArray())
        {
            for (LLSD::array_const_iterator it = recent.beginArray(); it != recent.endArray(); ++it)
            {
                auto found = mIndex.find(it->asUUID());
                if (found != mIndex.end())
                {
                    consider(mEntries[found->second]);
                }
            }
        }
    }
    else
    {
        for (const Entry& entry : mEntries)
        {
            consider(entry);
        }
    }

    const bool library_scope = mScopeCombo && mScopeCombo->getValue().asString() == "library";
    const bool library_missing = library_scope && mScopeRoot.isNull();
    mCreateFolderBtn->setEnabled(findLibraryFolder().isNull());

    LLStringUtil::format_map_t args;
    args["[SHOWN]"] = llformat("%d", shown);
    args["[TOTAL]"] = llformat("%d", total);
    std::string status;
    if (library_missing)
    {
        status = getString("status_no_library");
        mList->setCommentText(getString("comment_no_library"));
    }
    else if (total == 0)
    {
        status = getString("status_none");
        mList->setCommentText(getString(recent_only ? "comment_no_recent" : "comment_no_results"));
    }
    else if (total > shown)
    {
        status = getString("status_capped", args);
    }
    else
    {
        status = getString("status_count", args);
    }
    if (mCacheTruncated)
    {
        status += " " + getString("status_truncated");
    }
    mStatusText->setText(status);

    if (selected.notNull())
    {
        mList->selectByID(selected);
    }
    updateButtons();
}

void PrestigeFloaterQuickLibrary::onFilterChanged()
{
    rebuildList();
}

void PrestigeFloaterQuickLibrary::onTypesChanged()
{
    U32 type_mask = 0;
    for (size_t i = 0; i < mTypeChecks.size(); ++i)
    {
        if (mTypeChecks[i] && mTypeChecks[i]->get())
        {
            type_mask |= 1u << i;
        }
    }
    gSavedSettings.setU32(SETTING_TYPES, type_mask & ALL_KINDS);
    rebuildList();
}

void PrestigeFloaterQuickLibrary::onScopeChanged()
{
    gSavedSettings.setU32(SETTING_SCOPE, mScopeCombo->getValue().asString() == "library" ? 1u : 0u);
    rebuildCache();
    rebuildList();
}

void PrestigeFloaterQuickLibrary::onCreateLibraryFolder()
{
    const LLUUID& root = gInventory.getRootFolderID();
    if (root.isNull() || findLibraryFolder().notNull())
    {
        return;
    }
    mCreateFolderBtn->setEnabled(false);
    LLHandle<LLFloater> handle = getHandle();
    gInventory.createNewCategory(root, LLFolderType::FT_NONE, LIBRARY_FOLDER_NAME, [handle](const LLUUID& new_id)
    {
        PrestigeFloaterQuickLibrary* self = static_cast<PrestigeFloaterQuickLibrary*>(handle.get());
        if (self && new_id.notNull())
        {
            // Show the new (empty) folder's scope
            self->mScopeCombo->setValue(LLSD("library"));
            self->onScopeChanged();
        }
    });
}

void PrestigeFloaterQuickLibrary::onToggleFavorite()
{
    const LLUUID item_id = selectedItem();
    if (item_id.notNull() && gInventory.getObject(item_id))
    {
        // Second Life's own inventory favourites, shared with the inventory's Favorites tab
        toggle_favorite(item_id);
    }
}

void PrestigeFloaterQuickLibrary::onShowInInventory()
{
    const LLUUID item_id = selectedItem();
    if (item_id.notNull())
    {
        show_item_original(item_id);
    }
}

void PrestigeFloaterQuickLibrary::updateButtons()
{
    const LLUUID item_id = selectedItem();
    const LLInventoryObject* object = item_id.notNull() ? gInventory.getObject(item_id) : nullptr;
    mOpenBtn->setEnabled(object != nullptr);
    mShowBtn->setEnabled(object != nullptr);
    mFavoriteBtn->setEnabled(object != nullptr);
    mFavoriteBtn->setLabel(getString(object && object->getIsFavorite() ? "unfavorite_label" : "favorite_label"));
}

LLUUID PrestigeFloaterQuickLibrary::selectedItem() const
{
    return mList ? mList->getStringUUIDSelectedItem() : LLUUID::null;
}

void PrestigeFloaterQuickLibrary::activateItem(const LLUUID& item_id_in)
{
    // A copy: the inventory's bridge actions keep a reference to the id they are given.
    const LLUUID item_id = item_id_in;
    LLViewerInventoryItem* item = gInventory.getItem(item_id);
    if (!item)
    {
        return;
    }
    if (item->getType() == LLAssetType::AT_GESTURE)
    {
        // The inventory's Activate / Deactivate
        LLGestureMgr& gestures = LLGestureMgr::instance();
        if (gestures.isGestureActive(item_id))
        {
            gestures.deactivateGesture(item_id);
        }
        else
        {
            gestures.activateGesture(item_id);
        }
        gInventory.updateItem(item);
        gInventory.notifyObservers();
    }
    else
    {
        // What a double-click in the inventory does: texture, animation and sound previews,
        // notecard and script editors, the material editor, a landmark's teleport offer,
        // and wearing (or taking off) an object.
        LLInvFVBridgeAction::doAction(item->getType(), item_id, &gInventory);
    }
    noteUsed(item_id);
}

void PrestigeFloaterQuickLibrary::noteUsed(const LLUUID& item_id_in)
{
    const LLUUID item_id = item_id_in;
    if (item_id.isNull())
    {
        return;
    }
    const LLSD old_list = gSavedSettings.getLLSD(SETTING_RECENT);
    LLSD new_list = LLSD::emptyArray();
    new_list.append(LLSD(item_id));
    if (old_list.isArray())
    {
        for (LLSD::array_const_iterator it = old_list.beginArray(); it != old_list.endArray(); ++it)
        {
            if (new_list.size() >= MAX_RECENT)
            {
                break;
            }
            const LLUUID id = it->asUUID();
            if (id.notNull() && id != item_id)
            {
                new_list.append(LLSD(id));
            }
        }
    }
    gSavedSettings.setLLSD(SETTING_RECENT, new_list);

    if (mRecentCheck && mRecentCheck->get())
    {
        rebuildList();
    }
}
