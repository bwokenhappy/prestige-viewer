/**
 * @file prestigefloaterquicklibrary.h
 * @brief Prestige quick library (Ctrl+Space): a compact search over the
 *        inventory's textures, materials, objects, animations, sounds,
 *        notecards, landmarks, scripts and gestures, with favourites and
 *        recently used items. Rows open the native way on double-click /
 *        Enter and can be dragged into the world like inventory items.
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

#ifndef PRESTIGE_FLOATERQUICKLIBRARY_H
#define PRESTIGE_FLOATERQUICKLIBRARY_H

#include "llfloater.h"
#include "llframetimer.h"
#include "llassettype.h"
#include "llinventoryobserver.h"
#include "llinventorytype.h"
#include "llscrolllistctrl.h"
#include "lluuid.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLFilterEditor;
class LLTextBox;

// The results list: a scroll list whose rows (inventory item ids) can be
// dragged out with the standard inventory drag and drop, and that reports
// Enter as "open the selected row".
class PrestigeQuickLibraryList : public LLScrollListCtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLScrollListCtrl::Params>
    {
        Params() {}
    };

    typedef std::function<void(const LLUUID&)> item_callback_t;

    ~PrestigeQuickLibraryList() override = default;

    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleKeyHere(KEY key, MASK mask) override;

    void setActivateCallback(const item_callback_t& cb) { mActivateCallback = cb; }
    void setDragStartedCallback(const item_callback_t& cb) { mDragStartedCallback = cb; }

protected:
    friend class LLUICtrlFactory;
    PrestigeQuickLibraryList(const Params& p);

private:
    bool            mDragArmed = false;
    LLUUID          mDragItem;  // the row pressed, dragged once the mouse moves far enough
    item_callback_t mActivateCallback;
    item_callback_t mDragStartedCallback;
};

class PrestigeFloaterQuickLibrary : public LLFloater, public LLInventoryObserver
{
    LOG_CLASS(PrestigeFloaterQuickLibrary);
public:
    PrestigeFloaterQuickLibrary(const LLSD& key);
    ~PrestigeFloaterQuickLibrary() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;
    bool handleKeyHere(KEY key, MASK mask) override;

    // LLInventoryObserver
    void changed(U32 mask) override;

    // The top-level My Inventory folder used as the "Library folder" scope,
    // or null when the user hasn't created it.
    static LLUUID findLibraryFolder();

private:
    struct FolderInfo
    {
        std::string mPath;              // from My Inventory down
        bool        mFavorite = false;  // the folder or one above it is a favourite
    };
    struct Entry
    {
        LLUUID                 mID;
        std::string            mName;
        std::string            mFolded;            // lower-case name, for the search
        const FolderInfo*      mFolder = nullptr;  // the folder it is in (points into mFolders)
        LLAssetType::EType     mType = LLAssetType::AT_NONE;
        LLInventoryType::EType mInvType = LLInventoryType::IT_NONE;
        U32                    mFlags = 0;
        S32                    mKind = 0;          // index into the kinds table
        bool                   mFavorite = false;
    };

    void rebuildCache();
    void rebuildList();
    const FolderInfo& folderInfo(const LLUUID& cat_id, S32 depth = 0);

    void onFilterChanged();
    void onTypesChanged();
    void onScopeChanged();
    void onCreateLibraryFolder();
    void onToggleFavorite();
    void onShowInInventory();
    void updateButtons();

    void activateItem(const LLUUID& item_id);
    void noteUsed(const LLUUID& item_id);
    LLUUID selectedItem() const;

    LLFilterEditor*           mSearch = nullptr;
    LLComboBox*               mScopeCombo = nullptr;
    LLCheckBoxCtrl*           mFavoritesCheck = nullptr;
    LLCheckBoxCtrl*           mRecentCheck = nullptr;
    std::vector<LLCheckBoxCtrl*> mTypeChecks;
    PrestigeQuickLibraryList* mList = nullptr;
    LLButton*                 mCreateFolderBtn = nullptr;
    LLButton*                 mOpenBtn = nullptr;
    LLButton*                 mFavoriteBtn = nullptr;
    LLButton*                 mShowBtn = nullptr;
    LLTextBox*                mStatusText = nullptr;

    std::vector<Entry>                       mEntries;   // sorted by name
    std::unordered_map<LLUUID, size_t>       mIndex;     // item id -> mEntries index
    std::unordered_map<LLUUID, FolderInfo>   mFolders;   // memo for folderInfo()
    LLUUID      mScopeRoot;
    bool        mCacheDirty = true;
    bool        mCacheTruncated = false;
    LLFrameTimer mRefreshTimer;
    F32         mRefreshInterval = 1.5f;  // grows for inventories that take long to read
};

#endif // PRESTIGE_FLOATERQUICKLIBRARY_H
