/**
 * @file slreceiveditemlog.h
 * @brief #1436: local "item received" lines in nearby chat / the giver's IM session
 *
 * Slipstream: written for this viewer. Purely local, nothing is sent to the
 * server or to the giver.
 */

#ifndef SL_SLRECEIVEDITEMLOG_H
#define SL_SLRECEIVEDITEMLOG_H

class LLOfferInfo;
class LLUUID;

namespace SLReceivedItemLog
{
    // The user answered an offer with `button` (an InventoryOfferResponse). IOR_ACCEPT and
    // IOR_SHOW log "Received ...", IOR_DECLINE logs "Declined ...", anything else (mute,
    // closing the dialog) logs nothing. `folder_id` is where the item went, when known.
    void noteResponse(const LLOfferInfo& info, int button, const LLUUID& folder_id);

    // The offer was accepted without asking (AutoAcceptNewInventory).
    void noteAutoAccepted(const LLOfferInfo& info, const LLUUID& folder_id);
}

#endif // SL_SLRECEIVEDITEMLOG_H
