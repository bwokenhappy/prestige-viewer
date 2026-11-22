/**
 * @file slacceptdestination.h
 * @brief Where accepted inventory offers go (Preferences > Chat > Alerts, under the
 *        auto-accept options): the usual folders, a chosen folder, or a subfolder named
 *        after the giver.
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

#ifndef SL_ACCEPTDESTINATION_H
#define SL_ACCEPTDESTINATION_H

#include "lluuid.h"

#include <string>

class LLOfferInfo;

// How an offer lands, and so how the destination is applied:
// - Object offers (IM_TASK_INVENTORY_OFFERED): the server creates the item when we accept,
//   in the folder named in the accept message's binary bucket (RLVa's "Give to #RLV" relies
//   on this). The accept is held until the destination is known (a giver subfolder may have
//   to be created first) and is then always sent exactly once: with the destination, or
//   with the usual folder if anything fails or takes too long.
// - Resident offers (IM_INVENTORY_OFFERED): the server has already put the item in our
//   inventory when the offer arrives (the viewer answers on receipt, unchanged). On accept
//   it is moved from the usual folder to the destination. A failed or skipped move leaves
//   it where it is.
//
// Never applied to: calling cards, "#RLV/~" folder gives (RLVa decides where they go),
// group notice attachments, or anything while inventory isn't usable.
namespace SLAcceptDestination
{
    // AcceptedInventoryDestination
    enum EMode
    {
        MODE_DEFAULT = 0,  // the usual per-type folders
        MODE_CHOSEN  = 1,  // AcceptedInventoryFolder (per account)
        MODE_GIVER   = 2,  // a subfolder named after the giver
    };

    // AcceptedInventoryDestinationScope
    enum EScope
    {
        SCOPE_ALL       = 0,  // items accepted with the Accept button and auto-accepted ones
        SCOPE_AUTO_ONLY = 1,  // auto-accepted items only
    };

    // True if the destination setting should change where this offer goes.
    bool appliesTo(const LLOfferInfo& offer, bool auto_accepted);

    // True if a folder can receive accepted items: it exists, is in My Inventory (not the
    // Library), and is not in the Trash, the Current Outfit, the Marketplace listings,
    // Received Items or the Outbox. reason (optional) says why not.
    bool isUsableFolder(const LLUUID& folder_id, std::string* reason = nullptr);

    // The resident's chosen folder (null if none; may no longer be usable).
    LLUUID getChosenFolder();

    // Object offer accepted (by the Accept button or auto-accept): sends the accept once the
    // destination is known. Clears the offer's transaction id so the offer itself can't
    // answer again; the accept goes out exactly once.
    void acceptObjectOffer(LLOfferInfo& offer);

    // Resident offer accepted (already in inventory): moves it to the destination.
    void moveAcceptedAgentOffer(const LLOfferInfo& offer);

    // For Preferences: where accepted items go, in words.
    std::string describeDestination();
}

#endif // SL_ACCEPTDESTINATION_H
