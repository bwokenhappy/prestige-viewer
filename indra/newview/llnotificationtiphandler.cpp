/**
 * @file llnotificationtiphandler.cpp
 * @brief Notification Handler Class for Notification Tips
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */


#include "llviewerprecompiledheaders.h" // must be first include

#include "llfloaterreg.h"
#include "llfloaterimnearbychat.h"
#include "llfloaterimnearbychat.h"
#include "llnotificationhandler.h"
#include "lggcontactsets.h"
#include "llnotifications.h"
#include "lltoastnotifypanel.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llnotificationmanager.h"
#include "llpaneltiptoast.h"
#include "llchat.h"            // <Prestige/> friends' log in / out as a nearby chat pop-up
#include "llinstantmessage.h"  // <Prestige/> SYSTEM_FROM
#include "llavatarnamecache.h" // <Prestige/> friend names as in Firestorm

using namespace LLNotificationsUI;

//--------------------------------------------------------------------------
LLTipHandler::LLTipHandler()
:   LLSystemNotificationHandler("NotificationTips", "notifytip")
{
    // Getting a Channel for our notifications
    LLScreenChannel* channel = LLChannelManager::getInstance()->createNotificationChannel();
    if(channel)
    {
        mChannel = channel->getHandle();
    }
}

//--------------------------------------------------------------------------
LLTipHandler::~LLTipHandler()
{
}

//--------------------------------------------------------------------------
void LLTipHandler::initChannel()
{
    S32 channel_right_bound = gViewerWindow->getWorldViewRectScaled().mRight - gSavedSettings.getS32("NotificationChannelRightMargin");
    mChannel.get()->init(channel_right_bound - NOTIFY_BOX_WIDTH, channel_right_bound);
}

//--------------------------------------------------------------------------
bool LLTipHandler::processNotification(const LLNotificationPtr& notification, bool should_log)
{
    if(mChannel.isDead())
    {
        return false;
    }

    // arrange a channel on a screen
    if(!mChannel.get()->getVisible())
    {
        initChannel();
    }

    // <Prestige> friends logging in / out show at the bottom left, as a nearby chat pop-up like
    // Firestorm's (PrestigeFriendStatusNearby), not as a tip at the top right. The line is kept in
    // Nearby Chat. ChatOnlineNotification (Preferences > Notifications) still turns them off.
    static LLCachedControl<bool> friend_status_nearby(gSavedSettings, "PrestigeFriendStatusNearby", true);
    if (friend_status_nearby && "FriendOnlineOffline" == notification->getName())
    {
        const LLUUID agent_id = notification->getPayload()["FROM_ID"].asUUID();
        if (!gSavedSettings.getBOOL("ChatOnlineNotification")
            && !(gSavedSettings.getBOOL("FSContactSetsNotificationToast") && LGGContactSets::getInstance()->notifyForFriend(agent_id)))
        {
            if (notification->canLogToChat() || notification->isSendToChatForced())
                LLHandlerUtil::logToNearbyChat(notification, CHAT_SOURCE_SYSTEM);
            return false;
        }
        // Firestorm's wording: "Display Name (username) is online."
        std::string friend_name = notification->getSubstitutions()["NAME"].asString();
        LLAvatarName av_name;
        if (agent_id.notNull() && LLAvatarNameCache::get(agent_id, &av_name))
        {
            friend_name = av_name.getCompleteName(true, true);
        }
        std::string status = notification->getSubstitutions()["STATUS"].asString();
        LLStringUtil::toLower(status);
        LLChat chat_msg(friend_name.empty() ? notification->getMessage()
                                            : friend_name + " is " + status + ".");
        chat_msg.mSourceType = CHAT_SOURCE_SYSTEM;
        chat_msg.mFromName = SYSTEM_FROM;
        chat_msg.mFromID = LLUUID::null;
        chat_msg.mNoticeTag = "Friends";
        LLSD args;
        args["prestige_friend_status"] = true;   // the bottom-left line (Conversations tick still applies)
        if (!friend_name.empty())
        {
            chat_msg.mSubjectID = agent_id;   // the name as a link in Nearby Chat
        }
        LLNotificationsUI::LLNotificationManager::instance().onChat(chat_msg, args);
        return false;
    }
    // </Prestige>

        // archive message in nearby chat
    if (notification->canLogToChat() || notification->isSendToChatForced())
    {
        LLHandlerUtil::logToNearbyChat(notification, CHAT_SOURCE_SYSTEM);
    }

    std::string session_name = notification->getPayload()["SESSION_NAME"];
    const std::string name = LLHandlerUtil::getSubstitutionOriginalName(notification);
    if (session_name.empty())
    {
        session_name = name;
    }
    LLUUID from_id = notification->getPayload()["from_id"];
    if (notification->canLogToIM())
    {
        LLHandlerUtil::logToIM(IM_NOTHING_SPECIAL, session_name, name,
                notification->getMessage(), from_id, from_id);
    }

    if (notification->canLogToIM() && notification->hasFormElements())
    {
        LLHandlerUtil::spawnIMSession(name, from_id);
    }

    if (notification->canLogToIM() && LLHandlerUtil::isIMFloaterOpened(notification))
    {
        return false;
    }

    // [FIRE-3522 : SJ] Only show the online/offline toast when ChatOnlineNotification is enabled, or the
    // friend is in a contact set with notices on and FSContactSetsNotificationToast is set.
    // <Prestige> local notifications off (the tick on the Conversations button): no friends
    // logging in / out pop-ups either
    static LLCachedControl<bool> local_notifications(gSavedSettings, "PrestigeLocalChatNotifications", true);
    if ("FriendOnlineOffline" == notification->getName() && !local_notifications)
    {
        return false;
    }
    // </Prestige>
    if ("FriendOnlineOffline" == notification->getName() && !gSavedSettings.getBOOL("ChatOnlineNotification"))
    {
        const LLUUID agent_id = notification->getPayload()["FROM_ID"].asUUID();
        if (!(gSavedSettings.getBOOL("FSContactSetsNotificationToast") && LGGContactSets::getInstance()->notifyForFriend(agent_id)))
        {
            return false;
        }
    }

    LLToastPanel* notify_box = LLToastPanel::buidPanelFromNotification(notification);

    LLToast::Params p;
    p.notif_id = notification->getID();
    p.notification = notification;
    p.panel = notify_box;
    p.is_tip = true;
    p.can_be_stored = false;

    LLDate cur_time = LLDate::now();
    LLDate exp_time = notification->getExpiration();
    if (exp_time > cur_time)
    {
        // we have non-default expiration time - keep visible until expires
        p.lifetime_secs = (F32)(exp_time.secondsSinceEpoch() - cur_time.secondsSinceEpoch());
    }
    else
    {
        // use default time
        p.lifetime_secs = (F32)gSavedSettings.getS32("NotificationTipToastLifeTime");
    }

    LLScreenChannel* channel = dynamic_cast<LLScreenChannel*>(mChannel.get());
    if(channel)
        channel->addToast(p);
    return false;
}
