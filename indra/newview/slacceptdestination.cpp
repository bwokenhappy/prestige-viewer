/**
 * @file slacceptdestination.cpp
 * @brief Where accepted inventory offers go. See slacceptdestination.h.
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

#include "slacceptdestination.h"

#include "llagent.h"
#include "llappearancemgr.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llcachename.h"
#include "llcallbacklist.h"
#include "llframetimer.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewermessage.h"
#include "rlvactions.h"
#include "rlvdefines.h"
#include "rlvhandler.h"
#include "rlvlocks.h"

#include <functional>
#include <map>
#include <memory>

namespace
{
    // How often a pending resolution or move looks at the inventory model.
    constexpr F32 POLL_SECONDS = 0.25f;
    // Giver name + parent fetch + folder creation. An object offer's accept waits at most
    // this long; then it goes to the parent folder (or the usual folder).
    constexpr F64 RESOLVE_TIMEOUT_SECONDS = 20.0;
    // A resident's gift must show up in the inventory model within this time to be moved.
    constexpr F64 MOVE_WAIT_SECONDS = 30.0;
    // A creation that never called back is forgotten after this, so the table stays bounded.
    constexpr F64 PENDING_CREATE_STALE_SECONDS = 60.0;

    typedef std::function<void(const LLUUID&)> destination_cb_t;

    F64 now_seconds()
    {
        return LLFrameTimer::getTotalSeconds();
    }

    bool is_under(const LLUUID& id, const LLUUID& ancestor_id)
    {
        // isObjectDescendentOf() says yes when both ids are null.
        return ancestor_id.notNull() && gInventory.isObjectDescendentOf(id, ancestor_id);
    }

    std::string offer_label(const LLOfferInfo& offer)
    {
        return "'" + offer.mDesc + "' type " + std::to_string((S32)offer.mType) + " from '" + offer.mFromName + "' " +
               offer.mFromID.asString() + (offer.mIM == IM_TASK_INVENTORY_OFFERED ? " (object)" : "");
    }

    // One folder creation per (parent, name) at a time: two gifts from the same giver
    // arriving together must not make two "<Giver>" folders.
    struct PendingCreate
    {
        bool   mDone = false;
        LLUUID mResult;
        F64    mStarted = 0.0;
    };
    typedef std::map<std::string, std::shared_ptr<PendingCreate>> pending_create_map_t;

    pending_create_map_t& pending_creates()
    {
        static pending_create_map_t sPending;
        return sPending;
    }

    std::string pending_key(const LLUUID& parent_id, const std::string& name)
    {
        std::string lower = name;
        LLStringUtil::toLower(lower);
        return parent_id.asString() + "/" + lower;
    }

    void prune_pending_creates()
    {
        const F64 now = now_seconds();
        pending_create_map_t& pending = pending_creates();
        for (auto it = pending.begin(); it != pending.end();)
        {
            if (now - it->second->mStarted > PENDING_CREATE_STALE_SECONDS)
            {
                LL_WARNS("InvCreate") << "Accept destination: creating folder " << it->first << " never answered; forgotten" << LL_ENDL;
                it = pending.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // An existing plain folder directly under parent_id with this name (case ignored).
    LLUUID find_child_folder(const LLUUID& parent_id, const std::string& name)
    {
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(parent_id, cats, items);
        if (!cats)
        {
            return LLUUID::null;
        }
        for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
        {
            if (cat && cat->getPreferredType() == LLFolderType::FT_NONE
                && LLStringUtil::compareInsensitive(cat->getName(), name) == 0)
            {
                return cat->getUUID();
            }
        }
        return LLUUID::null;
    }

    // Resolves a giver subfolder. Driven by a poll so every wait (name lookup, parent
    // fetch, folder creation) shares one deadline and the answer goes out exactly once.
    struct GiverResolve
    {
        LLUUID           mParent;    // validated: the chosen folder or the usual folder
        LLUUID           mFallback;  // the usual folder for this offer (today's behaviour)
        std::string      mLabel;     // for the log
        std::string      mName;      // sanitised folder name, once known
        bool             mNameReady = false;
        bool             mNameFailed = false;
        bool             mFetchStarted = false;
        std::shared_ptr<PendingCreate> mCreate;
        F64              mDeadline = 0.0;
        destination_cb_t mDone;
        bool             mAnswered = false;
        LLAvatarNameCache::callback_connection_t mNameConnection;

        void answer(const LLUUID& folder_id, const std::string& why)
        {
            if (mAnswered)
            {
                return;
            }
            mAnswered = true;
            if (mNameConnection.connected())
            {
                mNameConnection.disconnect();
            }

            LLUUID dest = folder_id;
            std::string reason;
            if (dest != mFallback && !SLAcceptDestination::isUsableFolder(dest, &reason))
            {
                LL_WARNS("InvCreate") << "Accept destination: " << mLabel << ": folder " << dest << " unusable at send time ("
                                      << reason << "); using the usual folder" << LL_ENDL;
                dest = mFallback;
            }
            LL_INFOS("InvCreate") << "Accept destination: " << mLabel << " -> " << dest << " (" << why << ")" << LL_ENDL;

            destination_cb_t done = mDone;
            mDone = nullptr;
            if (done)
            {
                done(dest);
            }
        }

        void setName(std::string name)
        {
            LLInventoryObject::correctInventoryName(name);
            if (name.empty())
            {
                mNameFailed = true;
            }
            else
            {
                mName = name;
                mNameReady = true;
            }
        }

        // Returns true when finished (doPeriodically stops).
        bool step()
        {
            if (mAnswered)
            {
                return true;
            }
            if (now_seconds() > mDeadline)
            {
                answer(mParent, "timed out waiting for the giver folder");
                return true;
            }
            if (!gInventory.isInventoryUsable())
            {
                return false;
            }
            if (mNameFailed)
            {
                answer(mParent, "giver name unavailable");
                return true;
            }
            if (!mNameReady)
            {
                return false;
            }

            // Look for an existing "<Giver>" folder only once the parent's children are
            // known; otherwise a folder right after login could be created twice.
            if (!gInventory.isCategoryComplete(mParent))
            {
                if (!mFetchStarted)
                {
                    mFetchStarted = true;
                    LLInventoryModelBackgroundFetch::instance().start(mParent, false);
                }
                return false;
            }

            if (!mCreate)
            {
                const LLUUID existing_id = find_child_folder(mParent, mName);
                if (existing_id.notNull())
                {
                    answer(existing_id, "existing giver folder '" + mName + "'");
                    return true;
                }

                prune_pending_creates();
                const std::string key = pending_key(mParent, mName);
                pending_create_map_t& pending = pending_creates();
                auto it = pending.find(key);
                if (it != pending.end())
                {
                    mCreate = it->second;
                }
                else
                {
                    std::shared_ptr<PendingCreate> create = std::make_shared<PendingCreate>();
                    create->mStarted = now_seconds();
                    pending[key] = create;
                    mCreate = create;
                    LL_INFOS("InvCreate") << "Accept destination: creating giver folder '" << mName << "' in " << mParent << LL_ENDL;
                    gInventory.createNewCategory(mParent, LLFolderType::FT_NONE, mName,
                        [create, key](const LLUUID& new_id)
                        {
                            create->mResult = new_id;
                            create->mDone = true;
                            pending_create_map_t& pending_map = pending_creates();
                            auto found = pending_map.find(key);
                            if (found != pending_map.end() && found->second == create)
                            {
                                pending_map.erase(found);
                            }
                        });
                }
                // The creation may already have answered (inventory unusable); the next
                // poll reads it.
                return false;
            }

            if (!mCreate->mDone)
            {
                return false;
            }
            if (mCreate->mResult.isNull())
            {
                answer(mParent, "creating the giver folder failed");
                return true;
            }
            std::string reason;
            if (!gInventory.getCategory(mCreate->mResult) || !SLAcceptDestination::isUsableFolder(mCreate->mResult, &reason))
            {
                answer(mParent, "the new giver folder is not usable (" + reason + ")");
                return true;
            }
            answer(mCreate->mResult, "created giver folder '" + mName + "'");
            return true;
        }
    };

    // The folder accepted items would go into before any giver subfolder: the chosen
    // folder if it is usable, else the offer's usual folder.
    LLUUID base_folder(const LLOfferInfo& offer, const std::string& label)
    {
        const LLUUID chosen_id = SLAcceptDestination::getChosenFolder();
        if (chosen_id.isNull())
        {
            return offer.mFolderID;
        }
        std::string reason;
        if (!SLAcceptDestination::isUsableFolder(chosen_id, &reason))
        {
            LL_WARNS("InvCreate") << "Accept destination: " << label << ": chosen folder " << chosen_id << " can't be used ("
                                  << reason << "); using the usual folder" << LL_ENDL;
            return offer.mFolderID;
        }
        return chosen_id;
    }

    // Calls done exactly once with the destination (possibly before returning).
    void resolve(const LLOfferInfo& offer, destination_cb_t done)
    {
        static LLCachedControl<S32> mode(gSavedSettings, "AcceptedInventoryDestination", SLAcceptDestination::MODE_DEFAULT);

        const std::string label = offer_label(offer);
        const LLUUID parent_id = (mode() == SLAcceptDestination::MODE_DEFAULT) ? offer.mFolderID : base_folder(offer, label);
        if (mode() != SLAcceptDestination::MODE_GIVER)
        {
            LL_INFOS("InvCreate") << "Accept destination: " << label << " -> " << parent_id << LL_ENDL;
            done(parent_id);
            return;
        }

        // A folder named after the giver would show a name RLVa is hiding.
        if (RlvActions::isRlvEnabled() && !offer.mFromGroup && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, offer.mFromID))
        {
            LL_INFOS("InvCreate") << "Accept destination: " << label << " -> " << parent_id
                                  << " (no giver folder: RLVa is hiding names)" << LL_ENDL;
            done(parent_id);
            return;
        }

        std::shared_ptr<GiverResolve> state = std::make_shared<GiverResolve>();
        state->mParent = parent_id;
        state->mFallback = offer.mFolderID;
        state->mLabel = label;
        state->mDeadline = now_seconds() + RESOLVE_TIMEOUT_SECONDS;
        state->mDone = done;

        // The giver: the resident for a resident's gift, the object's owner for an object's.
        if (offer.mFromGroup)
        {
            std::string group_name;
            if (gCacheName && gCacheName->getGroupName(offer.mFromID, group_name))
            {
                state->setName(group_name);
            }
            else
            {
                state->mNameFailed = true;
            }
        }
        else
        {
            LLAvatarName av_name;
            if (LLAvatarNameCache::get(offer.mFromID, &av_name))
            {
                state->setName(av_name.getLegacyName());
            }
            else
            {
                std::weak_ptr<GiverResolve> weak_state = state;
                state->mNameConnection = LLAvatarNameCache::get(offer.mFromID,
                    [weak_state](const LLUUID&, const LLAvatarName& name)
                    {
                        if (std::shared_ptr<GiverResolve> pending_state = weak_state.lock())
                        {
                            pending_state->setName(name.getLegacyName());
                        }
                    });
            }
        }

        if (state->step())
        {
            return;
        }
        doPeriodically([state]() { return state->step(); }, POLL_SECONDS);
    }
}

namespace SLAcceptDestination
{
    LLUUID getChosenFolder()
    {
        return LLUUID(gSavedPerAccountSettings.getString("AcceptedInventoryFolder"));
    }

    bool isUsableFolder(const LLUUID& folder_id, std::string* reason)
    {
        auto fail = [reason](const char* why)
        {
            if (reason)
            {
                *reason = why;
            }
            return false;
        };

        if (folder_id.isNull())
        {
            return fail("no folder");
        }
        const LLViewerInventoryCategory* cat = gInventory.getCategory(folder_id);
        if (!cat)
        {
            return fail("the folder no longer exists");
        }
        if (!is_under(folder_id, gInventory.getRootFolderID()))
        {
            return fail("the folder is not in My Inventory");
        }
        if (is_under(folder_id, gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH)))
        {
            return fail("the folder is in the Trash");
        }
        if (is_under(folder_id, LLAppearanceMgr::instance().getCOF()))
        {
            return fail("the folder is in the Current Outfit");
        }
        if (depth_nesting_in_marketplace(folder_id) >= 0)
        {
            return fail("the folder is in the Marketplace listings");
        }
        if (is_under(folder_id, gInventory.findCategoryUUIDForType(LLFolderType::FT_INBOX)))
        {
            return fail("the folder is in Received Items");
        }
        if (is_under(folder_id, gInventory.findCategoryUUIDForType(LLFolderType::FT_OUTBOX)))
        {
            return fail("the folder is in the Outbox");
        }
        switch (cat->getPreferredType())
        {
            case LLFolderType::FT_TRASH:
            case LLFolderType::FT_CURRENT_OUTFIT:
            case LLFolderType::FT_INBOX:
            case LLFolderType::FT_OUTBOX:
            case LLFolderType::FT_MARKETPLACE_LISTINGS:
            case LLFolderType::FT_MARKETPLACE_STOCK:
            case LLFolderType::FT_MARKETPLACE_VERSION:
                return fail("that system folder can't hold accepted items");
            default:
                break;
        }
        return true;
    }

    bool appliesTo(const LLOfferInfo& offer, bool auto_accepted)
    {
        static LLCachedControl<S32> mode(gSavedSettings, "AcceptedInventoryDestination", MODE_DEFAULT);
        static LLCachedControl<S32> scope(gSavedSettings, "AcceptedInventoryDestinationScope", SCOPE_ALL);

        if (mode() == MODE_DEFAULT)
        {
            return false;
        }
        if (!auto_accepted && scope() == SCOPE_AUTO_ONLY)
        {
            return false;
        }
        // Group notice attachments and anything else keep their own handling.
        if (offer.mIM != IM_INVENTORY_OFFERED && offer.mIM != IM_TASK_INVENTORY_OFFERED)
        {
            return false;
        }
        if (!gInventory.isInventoryUsable() || offer.mFolderID.isNull())
        {
            return false;
        }
        if (offer.mIM == IM_TASK_INVENTORY_OFFERED && offer.mTransactionID.isNull())
        {
            return false;
        }
        if (offer.mIM == IM_INVENTORY_OFFERED && offer.mObjectID.isNull())
        {
            return false;
        }
        // Calling cards keep the server's handling.
        if (offer.mType == LLAssetType::AT_CALLINGCARD)
        {
            return false;
        }
        // "#RLV/~..." folders: RLVa's shared-folder handling decides (resident gives carry the
        // prefix at the start of the description, object gives after the opening quote).
        if (offer.mType == LLAssetType::AT_CATEGORY)
        {
            const std::string::size_type prefix_at = offer.mDesc.find(RLV_PUTINV_PREFIX);
            if (prefix_at == 0 || prefix_at == 1)
            {
                return false;
            }
        }
        return true;
    }

    void acceptObjectOffer(LLOfferInfo& offer)
    {
        // The continuation owns a copy: the offer itself may be deleted when its
        // notification callback returns.
        LLOfferInfo* pending = new LLOfferInfo(offer);
        offer.mTransactionID.setNull();
        resolve(*pending, [pending](const LLUUID& dest)
            {
                pending->sendAccept(dest);
                delete pending;
            });
    }

    void moveAcceptedAgentOffer(const LLOfferInfo& offer)
    {
        const LLUUID object_id = offer.mObjectID;
        const LLUUID usual_id = offer.mFolderID;
        const std::string label = offer_label(offer);

        resolve(offer, [object_id, usual_id, label](const LLUUID& dest)
            {
                if (dest == usual_id)
                {
                    return;
                }
                const F64 deadline = now_seconds() + MOVE_WAIT_SECONDS;
                // Moves run from the idle loop, never from inside an inventory notification.
                doPeriodically([object_id, usual_id, label, dest, deadline]()
                    {
                        LLInventoryObject* obj = gInventory.getObject(object_id);
                        if (!obj)
                        {
                            if (now_seconds() > deadline)
                            {
                                LL_WARNS("InvCreate") << "Accept destination: " << label << ": " << object_id
                                                      << " never showed up in inventory; left where it is" << LL_ENDL;
                                return true;
                            }
                            return false;
                        }

                        const LLUUID parent_id = obj->getParentUUID();
                        if (parent_id == dest)
                        {
                            return true;
                        }
                        // Only move a gift that is still where the server put it: a resident
                        // may already have filed it, deleted it, or RLVa may have moved it.
                        if (parent_id != usual_id)
                        {
                            LL_INFOS("InvCreate") << "Accept destination: " << label << ": " << object_id
                                                  << " is in " << parent_id << ", not its usual folder " << usual_id
                                                  << "; not moved" << LL_ENDL;
                            return true;
                        }
                        std::string reason;
                        if (!SLAcceptDestination::isUsableFolder(dest, &reason))
                        {
                            LL_WARNS("InvCreate") << "Accept destination: " << label << ": destination " << dest
                                                  << " unusable (" << reason << "); left in its usual folder" << LL_ENDL;
                            return true;
                        }

                        LLViewerInventoryCategory* cat = gInventory.getCategory(object_id);
                        LLViewerInventoryItem* item = cat ? nullptr : gInventory.getItem(object_id);
                        if (rlv_handler_t::isEnabled())
                        {
                            const bool allowed = cat ? RlvFolderLocks::instance().canMoveFolder(object_id, dest)
                                                     : RlvFolderLocks::instance().canMoveItem(object_id, dest);
                            if (!allowed)
                            {
                                LL_INFOS("InvCreate") << "Accept destination: " << label << ": RLVa folder locks forbid the move; not moved" << LL_ENDL;
                                return true;
                            }
                        }

                        LL_INFOS("InvCreate") << "Accept destination: moving " << label << " (" << object_id << ") to " << dest << LL_ENDL;
                        if (cat)
                        {
                            gInventory.changeCategoryParent(cat, dest, false);
                        }
                        else if (item)
                        {
                            gInventory.changeItemParent(item, dest, false);
                        }
                        return true;
                    }, POLL_SECONDS);
            });
    }

    std::string describeDestination()
    {
        static LLCachedControl<S32> mode(gSavedSettings, "AcceptedInventoryDestination", MODE_DEFAULT);

        if (mode() == MODE_DEFAULT)
        {
            return LLTrans::getString("AcceptDestUsualFolders");
        }
        if (!gInventory.isInventoryUsable())
        {
            return LLTrans::getString("AcceptDestInventoryNotReady");
        }

        const LLUUID chosen_id = getChosenFolder();
        std::string reason;
        LLStringUtil::format_map_t args;
        if (chosen_id.notNull() && isUsableFolder(chosen_id, &reason))
        {
            args["[PATH]"] = get_category_path(chosen_id);
            return LLTrans::getString(mode() == MODE_GIVER ? "AcceptDestGiverIn" : "AcceptDestChosen", args);
        }
        if (chosen_id.notNull())
        {
            args["[REASON]"] = reason;
            return LLTrans::getString(mode() == MODE_GIVER ? "AcceptDestGiverChosenUnusable" : "AcceptDestChosenUnusable", args);
        }
        return LLTrans::getString(mode() == MODE_GIVER ? "AcceptDestGiverInUsual" : "AcceptDestNoneChosen");
    }
}
