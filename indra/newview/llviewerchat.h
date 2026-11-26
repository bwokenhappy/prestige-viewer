/**
 * @file llviewerchat.h
 * @brief wrapper of LLChat in viewer
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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

#ifndef LL_LLVIEWERCHAT_H
#define LL_LLVIEWERCHAT_H

#include "llchat.h"
#include "llfontgl.h"
#include "lluicolor.h"

class LLAvatarName; // <Prestige/> readable chat

class LLViewerChat
{
public:
    typedef boost::signals2::signal<void (LLFontGL*)> font_change_signal_t;

    static void getChatColor(const LLChat& chat, LLUIColor& r_color, F32& r_color_alpha);
    static void getChatColor(const LLChat& chat, std::string& r_color_name, F32& r_color_alpha);
    static bool isNearbyChatOverlayVisible();
    static std::string getNearbyChatOverlayText(const LLChat& chat);
    static LLFontGL::StyleFlags getChatFontStyle(const LLChat& chat);
    static LLFontGL* getChatFont();
    static S32 getChatFontSize();
    static void formatChatMsg(const LLChat& chat, std::string& formated_msg);
    static std::string getSenderSLURL(const LLChat& chat, const LLSD& args);

    // #463: true when this sender is an object without a name (the "(Unnamed)" placeholder)
    // and ChatHideUnnamedObjectNames is on, so the chat views print the message alone.
    static bool hideSenderName(EChatSourceType source_type, const std::string& from_name);
    static bool hideSenderName(const LLChat& chat) { return hideSenderName(chat.mSourceType, chat.mFromName); }

    // <Prestige> calmer chat
    // A script error or warning (DEBUG_CHANNEL) from an object the user owns that goes in
    // nearby chat as a red line rather than the Script Debug window (ShowScriptErrors and
    // PrestigeOwnScriptErrorsInChat on).
    static bool isOwnScriptErrorInChat(const LLChat& chat);
    // Chat said by an object the user doesn't own (not llOwnerSay, llRegionSayTo or a script
    // error) while PrestigeQuietObjectChat is on: a smaller, dimmer line in nearby chat that
    // makes no pop-up, flash or sound.
    static bool isQuietObjectChat(const LLChat& chat);
    // Either of the above: nearby chat history only, no pop-up and no ping.
    static bool isCalmChat(const LLChat& chat) { return isOwnScriptErrorInChat(chat) || isQuietObjectChat(chat); }
    // </Prestige>

    // <Prestige> readable chat (nearby chat and IM windows)
    // PrestigeChatNameColors: names take the colour of who is speaking, other residents' text
    // the chat text colour.
    static bool useNameColors();
    // The colour of a sender's name: UserChatColor, FriendChatColor (or contact set),
    // AgentChatColor, LindenChatColor, MutedChatColor, ObjectChatColor, llOwnerSayChatColor, ...
    static LLUIColor getChatNameColor(const LLChat& chat);
    // The colour of the message text with name colours on (ChatHistoryTextColor for other
    // residents unless a keyword alert colours it); kind_color: getChatColor()'s colour.
    static LLUIColor getChatTextColor(const LLChat& chat, const LLUIColor& kind_color);
    // System, region and estate lines: shown in italics.
    static bool isSystemLine(const LLChat& chat);
    // PrestigeChatTags: the short label nearby chat puts in front of a line that isn't a resident
    // talking ("System", "Item", "Friends", "Region", "Object", "Script"); empty for none.
    static std::string getChatTag(const LLChat& chat);
    // The label's colour (PrestigeChatTag<Kind>Color in the skin, a built-in one otherwise).
    static LLColor4 getChatTagColor(const std::string& tag);
    // A message from someone else that names the user as a whole word (PrestigeChatHighlightMyName).
    static bool mentionsAgent(const LLChat& chat);
    // A resident's name as chat headers show it, after PrestigeChatPlainNames (0 as is, 1 fancy
    // letters folded and the username in brackets, 2 the username only).
    // with_username: the complete name ("Display (username)") rather than the display name.
    static std::string getReadableName(const LLAvatarName& av_name, bool with_username);
    // The same for a name given as text (transcripts, "Display (username)" or a legacy name).
    static std::string getReadableName(const std::string& from_name);
    // PrestigeChatPlainNames is 1 and this display name has fancy letters: the username shows
    // after it, whatever the username settings.
    static bool forceUsernameSuffix(const LLAvatarName& av_name);
    // </Prestige>

    static boost::signals2::connection setFontChangedCallback(const font_change_signal_t::slot_type& cb);
    static void signalChatFontChanged();

private:
    static std::string getObjectImSLURL(const LLChat& chat, const LLSD& args);
    static font_change_signal_t sChatFontChangedSignal;

};

#endif
