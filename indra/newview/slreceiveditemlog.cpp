/**
 * @file slreceiveditemlog.cpp
 * @brief #1436: local "item received" lines in nearby chat / the giver's IM session
 *
 * Slipstream: written for this viewer. Everything here is local: lines are added to the
 * nearby chat model and to the giver's IM session (and so to the chat log files through
 * their normal logging). Nothing is sent to the server or to the giver.
 */

#include "llviewerprecompiledheaders.h"

#include "slreceiveditemlog.h"

#include "llagent.h"
#include "llassettype.h"
#include "llavatarnamecache.h"
#include "llcachename.h"
#include "llchat.h"
#include "llfloaterimnearbychat.h"
#include "llfloaterreg.h"
#include "llinstantmessage.h"
#include "llinventorymodel.h"
#include "llmutelist.h"
#include "llnotificationhandler.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewermessage.h"

// [RLVa:KB]
#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhandler.h"
#include "rlvhelper.h"
#include "rlvui.h"
// [/RLVa:KB]

namespace
{

// Everything the lines need, copied out of the LLOfferInfo (which can be gone by the time an
// avatar name resolves).
struct ReceivedItem
{
    bool mReceived = true;      // false: declined
    bool mFromGroup = false;
    bool mFromObject = false;
    bool mHideNames = false;    // RLVa @shownames: the giver's name must not appear
    LLUUID mFromID;
    std::string mFromName;      // the giver; the object's name for object offers
    std::string mItem;
    std::string mType;
    std::string mFolder;        // empty when unknown
};

// The offer's description as the server sends it: an item name, or for objects
// "'name'  ( http://slurl )". Reduce it to the bare name.
std::string item_name(const std::string& desc)
{
    std::string name = desc;
    const auto slurl = name.find(" ( http");
    if (slurl != std::string::npos)
    {
        name.erase(slurl);
    }
    LLStringUtil::trim(name);
    if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'')
    {
        name = name.substr(1, name.size() - 2);
    }
    return name;
}

std::string folder_name(const LLOfferInfo& info, const LLUUID& hint)
{
    LLUUID folder_id = hint;
    // An agent's item is already in inventory by now: trust where it actually is.
    if (info.mObjectID.notNull())
    {
        if (const LLInventoryCategory* cat = gInventory.getCategory(info.mObjectID))
        {
            folder_id = cat->getParentUUID();
        }
        else if (const LLInventoryItem* item = gInventory.getItem(info.mObjectID))
        {
            folder_id = item->getParentUUID();
        }
    }
    if (folder_id.isNull())
    {
        return std::string();
    }
    const LLInventoryCategory* cat = gInventory.getCategory(folder_id);
    return cat ? cat->getName() : std::string();
}

void add_nearby_chat_line(const std::string& text)
{
    LLFloaterIMNearbyChat* nearby_chat = LLFloaterReg::getTypedInstance<LLFloaterIMNearbyChat>("nearby_chat");
    if (nearby_chat)
    {
        LLChat chat(text);
        chat.mSourceType = CHAT_SOURCE_SYSTEM;
        chat.mFromName = SYSTEM_FROM;
        chat.mFromID = LLUUID::null;
        nearby_chat->addMessage(chat);
    }
}

std::string in_folder(const std::string& folder)
{
    if (folder.empty())
    {
        return std::string();
    }
    LLStringUtil::format_map_t args;
    args["[FOLDER]"] = folder;
    return " " + LLTrans::getString("ReceivedItemInFolder", args);
}

// `owner` is the resolved owner (or giver, for avatar offers) name.
void write_lines(const ReceivedItem& it, const std::string& owner, const LLAvatarName* av_name)
{
    LLStringUtil::format_map_t args;
    args["[ITEM]"] = it.mItem;
    args["[TYPE]"] = it.mType;
    args["[INFOLDER]"] = it.mReceived ? in_folder(it.mFolder) : std::string();

    std::string line;
    if (it.mFromObject)
    {
        args["[OBJECT]"] = it.mFromName;
        args["[OWNER]"] = owner;
        line = LLTrans::getString(it.mReceived ? "ReceivedItemLogFromObject" : "DeclinedItemLogFromObject", args);
    }
    else
    {
        args["[FROM]"] = owner;
        line = LLTrans::getString(it.mReceived ? "ReceivedItemLog" : "DeclinedItemLog", args);
    }
    add_nearby_chat_line(line);

    // Residents also get it in their IM session, opened if needed. Local only; accepts only.
    if (it.mReceived && av_name && !it.mFromObject && !it.mFromGroup && !it.mHideNames
        && it.mFromID.notNull() && it.mFromID != gAgentID
        // [RLVa:KB]
        && RlvActions::canStartIM(it.mFromID)
        // [/RLVa:KB]
        )
    {
        LLNotificationsUI::LLHandlerUtil::spawnIMSession(av_name->getDisplayName(), it.mFromID);
        LLNotificationsUI::LLHandlerUtil::logToIM(IM_NOTHING_SPECIAL, av_name->getUserName(), LLStringUtil::null,
                               LLTrans::getString("ReceivedItemLogIM", args), it.mFromID, LLUUID::null);
    }
}

void log_item(const LLOfferInfo& info, bool received, const LLUUID& folder_hint)
{
    if (!gSavedSettings.getBOOL("LogReceivedItems"))
    {
        return;
    }
    // Muted senders: offers are refused before they get here, but a mute can land in between.
    // They produce nothing.
    if (LLMuteList::getInstance()->isMuted(info.mFromID, info.mFromName))
    {
        return;
    }

    ReceivedItem it;
    it.mReceived = received;
    it.mFromGroup = info.mFromGroup;
    it.mFromObject = info.mFromObject;
    it.mFromID = info.mFromID;
    it.mFromName = info.mFromName;
    it.mItem = item_name(info.mDesc);

    const char* type_key = LLAssetType::lookupHumanReadable(info.mType);
    it.mType = (type_key && *type_key) ? LLTrans::getString(type_key) : std::string();
    if (received)
    {
        it.mFolder = folder_name(info, folder_hint);
    }

// [RLVa:KB]
    // Same condition as the offer dialog: don't name a nearby resident under @shownames.
    it.mHideNames = RlvActions::isRlvEnabled() && !info.mFromGroup
        && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, info.mFromID)
        && RlvUtil::isNearbyAgent(info.mFromID)
        && !RlvUIEnabler::hasOpenIM(info.mFromID) && !RlvUIEnabler::hasOpenProfile(info.mFromID);
// [/RLVa:KB]

    if (info.mFromGroup)
    {
        // A group notice attachment (the giver) or a group-owned object (the owner).
        std::string group_name;
        if (!gCacheName || !gCacheName->getGroupName(info.mFromID, group_name))
        {
            group_name = info.mFromObject ? LLTrans::getString("ReceivedItemUnknownOwner") : info.mFromName;
        }
        if (!info.mFromObject)
        {
            it.mFromName = group_name;
        }
        write_lines(it, group_name, nullptr);
        return;
    }

    if (it.mHideNames)
    {
        write_lines(it, RlvStrings::getAnonym(info.mFromObject ? info.mFromID.asString() : info.mFromName), nullptr);
        return;
    }

    // An avatar: the giver, or the owner of the giving object. The callback runs at once
    // when the name is cached.
    LLAvatarNameCache::get(info.mFromID, [it](const LLUUID&, const LLAvatarName& av_name)
    {
        write_lines(it, av_name.getCompleteName(), &av_name);
    });
}

} // namespace

void SLReceivedItemLog::noteResponse(const LLOfferInfo& info, int button, const LLUUID& folder_id)
{
    if (button == IOR_ACCEPT || button == IOR_SHOW)
    {
        log_item(info, true, folder_id);
    }
    else if (button == IOR_DECLINE)
    {
        log_item(info, false, folder_id);
    }
}

void SLReceivedItemLog::noteAutoAccepted(const LLOfferInfo& info, const LLUUID& folder_id)
{
    log_item(info, true, folder_id);
}
