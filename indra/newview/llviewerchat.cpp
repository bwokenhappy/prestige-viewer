/**
 * @file llviewerchat.cpp
 * @brief Builds menus out of items.
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

#include "llviewerprecompiledheaders.h"
#include "llviewerchat.h"
#include "lggcontactsets.h"

// newview includes
#include "llagent.h"    // gAgent
#include "llslurl.h"
#include "lltrans.h"
#include "lluicolor.h"
#include "lluicolortable.h"
#include "llviewercontrol.h" // gSavedSettings
#include "llviewerregion.h"
#include "llworld.h"
#include "llinstantmessage.h" //SYSTEM_FROM
#include "llurlregistry.h"
#include "fskeywords.h"
#include "fscommon.h"
#include "llfloaterimcontainer.h"
#include "llfloaterreg.h"
// <Prestige> readable chat
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "prestigenamefold.h"
// </Prestige>

// LLViewerChat
LLViewerChat::font_change_signal_t LLViewerChat::sChatFontChangedSignal;

// <Prestige> calmer chat
namespace
{
    constexpr F32 QUIET_OBJECT_CHAT_ALPHA = 0.6f;
}

//static
bool LLViewerChat::isOwnScriptErrorInChat(const LLChat& chat)
{
    static LLCachedControl<bool> show_errors(gSavedSettings, "ShowScriptErrors", true);
    static LLCachedControl<bool> errors_in_chat(gSavedSettings, "PrestigeOwnScriptErrorsInChat", true);
    return show_errors && errors_in_chat
        && chat.mChatType == CHAT_TYPE_DEBUG_MSG
        && chat.mOwnerID == gAgentID;
}

//static
bool LLViewerChat::isQuietObjectChat(const LLChat& chat)
{
    static LLCachedControl<bool> quiet_object_chat(gSavedSettings, "PrestigeQuietObjectChat", true);
    if (!quiet_object_chat || chat.mSourceType != CHAT_SOURCE_OBJECT || chat.mMuted)
    {
        return false;
    }
    // lines from the chat transcript keep their own (grey) look
    if (chat.mChatStyle == CHAT_STYLE_HISTORY)
    {
        return false;
    }
    // llOwnerSay (RLV commands included), llRegionSayTo, llInstantMessage and script errors
    // are meant for us
    if (chat.mFromTaskIM || chat.mChatType == CHAT_TYPE_OWNER || chat.mChatType == CHAT_TYPE_DIRECT
        || chat.mChatType == CHAT_TYPE_DEBUG_MSG)
    {
        return false;
    }
    return chat.mOwnerID != gAgentID;
}
// </Prestige>

// <Prestige> readable chat
namespace
{
    bool is_resident_chat(const LLChat& chat)
    {
        return chat.mSourceType == CHAT_SOURCE_AGENT && chat.mFromID.notNull() && SYSTEM_FROM != chat.mFromName;
    }

    bool keyword_colored(const LLChat& chat)
    {
        static LLCachedControl<bool> sFSKeywordChangeColor(gSavedPerAccountSettings, "FSKeywordChangeColor", false);
        return sFSKeywordChangeColor && FSKeywords::getInstance()->chatContainsKeyword(chat, chat.mSessionID.isNull());
    }

    // "Display (username)": the two parts; false when the name has no username part
    bool split_complete_name(const std::string& name, std::string& display, std::string& username)
    {
        const size_t open = name.rfind(" (");
        if (open == std::string::npos || name.size() < open + 4 || name.back() != ')')
        {
            return false;
        }
        display = name.substr(0, open);
        username = name.substr(open + 2, name.size() - open - 3);
        return !display.empty() && !username.empty();
    }

    bool is_word_char(char ch)
    {
        // bytes of non-ASCII characters count as letters
        return (unsigned char)ch >= 0x80 || LLStringOps::isAlnum(ch) || ch == '_';
    }

    bool contains_word(const std::string& text, const std::string& word)
    {
        size_t pos = text.find(word);
        while (pos != std::string::npos)
        {
            const size_t end = pos + word.size();
            if ((pos == 0 || !is_word_char(text[pos - 1])) && (end >= text.size() || !is_word_char(text[end])))
            {
                return true;
            }
            pos = text.find(word, pos + 1);
        }
        return false;
    }

    void add_name_word(std::vector<std::string>& words, std::string name)
    {
        name = PrestigeNameFold::fold(name);
        LLStringUtil::trim(name);
        LLStringUtil::toLower(name);
        if (name.size() >= 3 && std::find(words.begin(), words.end(), name) == words.end())
        {
            words.push_back(name);
        }
    }

    std::string first_word(const std::string& name, char separator)
    {
        const size_t end = name.find(separator);
        return end == std::string::npos ? name : name.substr(0, end);
    }
}

//static
bool LLViewerChat::useNameColors()
{
    static LLCachedControl<bool> name_colors(gSavedSettings, "PrestigeChatNameColors", true);
    return name_colors;
}

//static
LLUIColor LLViewerChat::getChatNameColor(const LLChat& chat)
{
    LLUIColorTable& colors = LLUIColorTable::instance();
    // <Prestige> Firestorm skins: every resident's name in ChatNameColor (Firestorm's lime accent),
    // contact sets still win; object names in ChatNameObjectColor. Skins without ChatNameColor
    // (the Prestige skins) keep colouring names by who is speaking.
    static const bool skin_name_color = colors.colorExists("ChatNameColor");
    if (skin_name_color && !chat.mMuted)
    {
        if (is_resident_chat(chat))
        {
            // Only a contact-set colour (where the user turned that on) replaces it: getAvatarColor()
            // would also recolour by category (you, friends, ...) with the chat text colours, which
            // are white in the Firestorm skins, so every name came out white.
            LLColor4 name_color = colors.getColor("ChatNameColor").get();
            LGGContactSets::getInstance()->hasFriendColorThatShouldShow(chat.mFromID, ContactSetType::CHAT, name_color);
            return LLUIColor(name_color);
        }
        if (chat.mSourceType == CHAT_SOURCE_OBJECT && colors.colorExists("ChatNameObjectColor"))
        {
            return colors.getColor("ChatNameObjectColor");
        }
    }
    // </Prestige>
    if (!chat.mMuted && is_resident_chat(chat))
    {
        if (chat.mFromID == gAgentID)
        {
            return colors.getColor("UserChatColor");
        }
        // friends, Lindens, muted and contact sets as for the chat text before
        const LLColor4 agent_color = colors.getColor("AgentChatColor").get();
        return LLUIColor(LGGContactSets::getInstance()->getAvatarColor(chat.mFromID, agent_color, ContactSetType::CHAT));
    }
    if (!chat.mMuted && chat.mSourceType == CHAT_SOURCE_REGION)
    {
        return colors.getColor("SystemChatColor");
    }
    LLUIColor color = colors.getColor("White");
    F32 alpha = 1.f;
    getChatColor(chat, color, alpha);
    return color;
}

//static
LLUIColor LLViewerChat::getChatTextColor(const LLChat& chat, const LLUIColor& kind_color)
{
    if (!useNameColors() || chat.mMuted)
    {
        return kind_color;
    }
    if (chat.mSourceType == CHAT_SOURCE_REGION)
    {
        return LLUIColorTable::instance().getColor("SystemChatColor");
    }
    if (is_resident_chat(chat) && chat.mFromID != gAgentID && !keyword_colored(chat))
    {
        return LLUIColorTable::instance().getColor("ChatHistoryTextColor");
    }
    return kind_color;
}

//static
bool LLViewerChat::isSystemLine(const LLChat& chat)
{
    switch (chat.mSourceType)
    {
        case CHAT_SOURCE_SYSTEM:
        case CHAT_SOURCE_REGION:
            return true;
        case CHAT_SOURCE_AGENT:
            return chat.mFromID.isNull() || SYSTEM_FROM == chat.mFromName;
        default:
            return false;
    }
}

//static
std::string LLViewerChat::getChatTag(const LLChat& chat)
{
    static LLCachedControl<bool> chat_tags(gSavedSettings, "PrestigeChatTags", true);
    if (!chat_tags || chat.mSourceType == CHAT_SOURCE_TELEPORT)
    {
        return std::string();
    }
    if (!chat.mNoticeTag.empty())
    {
        return chat.mNoticeTag;
    }
    if (chat.mSourceType == CHAT_SOURCE_REGION)
    {
        return "Region";
    }
    if (chat.mSourceType == CHAT_SOURCE_OBJECT)
    {
        return chat.mChatType == CHAT_TYPE_DEBUG_MSG ? "Script" : "Object";
    }
    return isSystemLine(chat) ? "System" : std::string();
}

//static
LLColor4 LLViewerChat::getChatTagColor(const std::string& tag)
{
    LLUIColorTable& colors = LLUIColorTable::instance();
    const std::string name = "PrestigeChatTag" + tag + "Color";
    if (colors.colorExists(name))
    {
        return colors.getColor(name).get();
    }
    // built-in: readable on the dark chat backgrounds of every skin
    if (tag == "Item")    return LLColor4(0.45f, 0.80f, 1.00f, 1.f); // sky blue
    if (tag == "Friends") return LLColor4(0.55f, 0.90f, 0.55f, 1.f); // green
    if (tag == "Region")  return LLColor4(1.00f, 0.75f, 0.35f, 1.f); // amber
    if (tag == "Object")  return LLColor4(0.75f, 0.65f, 1.00f, 1.f); // lavender
    if (tag == "Script")  return LLColor4(1.00f, 0.45f, 0.45f, 1.f); // red
    return LLColor4(0.70f, 0.70f, 0.70f, 1.f);                      // System and others: grey
}

//static
bool LLViewerChat::mentionsAgent(const LLChat& chat)
{
    static LLCachedControl<bool> highlight_my_name(gSavedSettings, "PrestigeChatHighlightMyName", true);
    if (!highlight_my_name || chat.mMuted || !is_resident_chat(chat) || chat.mFromID == gAgentID
        || chat.mChatStyle == CHAT_STYLE_HISTORY || chat.mText.empty())
    {
        return false;
    }
    LLAvatarName av_name;
    if (!LLAvatarNameCache::get(gAgentID, &av_name))
    {
        return false;
    }
    std::vector<std::string> words;
    if (!av_name.isDisplayNameDefault())
    {
        const std::string display = av_name.getDisplayName(true);
        add_name_word(words, display);
        add_name_word(words, first_word(PrestigeNameFold::fold(display), ' '));
    }
    add_name_word(words, av_name.getAccountName());
    add_name_word(words, first_word(av_name.getAccountName(), '.'));
    add_name_word(words, first_word(av_name.getUserName(), ' '));

    std::string text = PrestigeNameFold::fold(chat.mText);
    LLStringUtil::toLower(text);
    for (const std::string& word : words)
    {
        if (contains_word(text, word))
        {
            return true;
        }
    }
    return false;
}

//static
bool LLViewerChat::forceUsernameSuffix(const LLAvatarName& av_name)
{
    static LLCachedControl<S32> plain_names(gSavedSettings, "PrestigeChatPlainNames", 0);
    return plain_names == 1 && LLAvatarName::useDisplayNames() && !av_name.isDisplayNameDefault()
        && !av_name.getAccountName().empty() && PrestigeNameFold::isFancy(av_name.getDisplayName());
}

//static
std::string LLViewerChat::getReadableName(const LLAvatarName& av_name, bool with_username)
{
    static LLCachedControl<S32> plain_names(gSavedSettings, "PrestigeChatPlainNames", 0);
    if (plain_names == 2)
    {
        return av_name.getUserName();
    }
    if (plain_names == 1 && forceUsernameSuffix(av_name))
    {
        std::string name = PrestigeNameFold::fold(av_name.getDisplayName());
        if (with_username)
        {
            name += " (" + av_name.getAccountName() + ")";
        }
        return name;
    }
    return with_username ? av_name.getCompleteName() : av_name.getDisplayName();
}

//static
std::string LLViewerChat::getReadableName(const std::string& from_name)
{
    static LLCachedControl<S32> plain_names(gSavedSettings, "PrestigeChatPlainNames", 0);
    if (plain_names == 1)
    {
        return PrestigeNameFold::fold(from_name);
    }
    if (plain_names == 2)
    {
        std::string display, username;
        if (split_complete_name(from_name, display, username))
        {
            return username;
        }
    }
    return from_name;
}
// </Prestige>

//static
void LLViewerChat::getChatColor(const LLChat& chat, LLUIColor& r_color, F32& r_color_alpha)
{
    if(chat.mMuted)
    {
        r_color= LLUIColorTable::instance().getColor("LtGray");
    }
    else
    {
        switch(chat.mSourceType)
        {
            case CHAT_SOURCE_SYSTEM:
                r_color = LLUIColorTable::instance().getColor("SystemChatColor");
                break;
            case CHAT_SOURCE_AGENT:
                if (chat.mFromID.isNull() || SYSTEM_FROM == chat.mFromName)
                {
                    r_color = LLUIColorTable::instance().getColor("SystemChatColor");
                }
                else
                {
                    if(gAgentID == chat.mFromID)
                    {
                        r_color = LLUIColorTable::instance().getColor("UserChatColor");
                    }
                    else
                    {
                        // <FS:CR> Friend / muted / Linden / contact-set colouring of chat text
                        const LLColor4 agent_color = LLUIColorTable::instance().getColor("AgentChatColor").get();
                        r_color = LLUIColor(LGGContactSets::getInstance()->getAvatarColor(chat.mFromID, agent_color, ContactSetType::CHAT));
                    }
                }
                break;
            case CHAT_SOURCE_OBJECT:
                if (chat.mChatType == CHAT_TYPE_DEBUG_MSG)
                {
                    r_color = LLUIColorTable::instance().getColor("ScriptErrorColor");
                }
                else if ( chat.mChatType == CHAT_TYPE_OWNER )
                {
                    r_color = LLUIColorTable::instance().getColor("llOwnerSayChatColor");
                }
                else if ( chat.mChatType == CHAT_TYPE_DIRECT )
                {
                    r_color = LLUIColorTable::instance().getColor("DirectChatColor");
                }
                else
                {
                    r_color = LLUIColorTable::instance().getColor("ObjectChatColor");
                }
                break;
            default:
                r_color = LLUIColorTable::instance().getColor("White");
        }

        // <FS:KC> Keyword alerts (nearby chat has no session id; IMs and group chat do)
        static LLCachedControl<bool> sFSKeywordChangeColor(gSavedPerAccountSettings, "FSKeywordChangeColor", false);
        if (sFSKeywordChangeColor && FSKeywords::getInstance()->chatContainsKeyword(chat, chat.mSessionID.isNull()))
        {
            static LLCachedControl<LLColor4> sFSKeywordColor(gSavedPerAccountSettings, "FSKeywordColor", LLColor4(1.f, 0.6f, 0.f, 1.f));
            r_color = LLUIColor(sFSKeywordColor());
        }
        // </FS:KC>

        if (!chat.mPosAgent.isExactlyZero())
        {
            LLVector3 pos_agent = gAgent.getPositionAgent();
            F32 distance_squared = dist_vec_squared(pos_agent, chat.mPosAgent);
            F32 dist_near_chat = gAgent.getNearChatRadius();
            if (distance_squared > dist_near_chat * dist_near_chat)
            {
                // diminish far-off chat
                r_color_alpha = 0.8f;
            }
            else
            {
                r_color_alpha = 1.0f;
            }
        }

        // <Prestige> quiet object chat: the object chat colour, dimmed
        if (isQuietObjectChat(chat))
        {
            r_color_alpha *= QUIET_OBJECT_CHAT_ALPHA;
        }
        // </Prestige>
    }
}


//static
void LLViewerChat::getChatColor(const LLChat& chat, std::string& r_color_name, F32& r_color_alpha)
{
    if(chat.mMuted)
    {
        r_color_name = "LtGray";
    }
    else
    {
        switch(chat.mSourceType)
        {
            case CHAT_SOURCE_SYSTEM:
                r_color_name = "SystemChatColor";
                break;

            case CHAT_SOURCE_AGENT:
                if (chat.mFromID.isNull())
                {
                    r_color_name = "SystemChatColor";
                }
                else
                {
                    if(gAgentID == chat.mFromID)
                    {
                        r_color_name = "UserChatColor";
                    }
                    else
                    {
                        r_color_name = "AgentChatColor";
                    }
                }
                break;

            case CHAT_SOURCE_OBJECT:
                if (chat.mChatType == CHAT_TYPE_DEBUG_MSG)
                {
                    r_color_name = "ScriptErrorColor";
                }
                else if ( chat.mChatType == CHAT_TYPE_OWNER )
                {
                    r_color_name = "llOwnerSayChatColor";
                }
                else if ( chat.mChatType == CHAT_TYPE_DIRECT )
                {
                    r_color_name = "DirectChatColor";
                }
                else
                {
                    r_color_name = "ObjectChatColor";
                }
                break;
            default:
                r_color_name = "White";
        }

        if (!chat.mPosAgent.isExactlyZero())
        {
            LLVector3 pos_agent = gAgent.getPositionAgent();
            F32 distance_squared = dist_vec_squared(pos_agent, chat.mPosAgent);
            F32 dist_near_chat = gAgent.getNearChatRadius();
            if (distance_squared > dist_near_chat * dist_near_chat)
            {
                // diminish far-off chat
                r_color_alpha = 0.8f;
            }
            else
            {
                r_color_alpha = 1.0f;
            }
        }
    }

}


//static
LLFontGL* LLViewerChat::getChatFont()
{
    S32 font_size = gSavedSettings.getS32("ChatFontSize");
    LLFontGL* fontp = NULL;
    switch(font_size)
    {
        case 0:
            fontp = LLFontGL::getFontSansSerifSmall();
            break;
        default:
        case 1:
            fontp = LLFontGL::getFontSansSerif();
            break;
        case 2:
            fontp = LLFontGL::getFontSansSerifBig();
            break;
    }

    return fontp;

}

//static
S32 LLViewerChat::getChatFontSize()
{
    return gSavedSettings.getS32("ChatFontSize");
}


//static
void LLViewerChat::formatChatMsg(const LLChat& chat, std::string& formated_msg)
{
    std::string tmpmsg = chat.mText;

    // show @name instead of slurl for chat mentions
    LLUrlMatch match;
    while (LLUrlRegistry::instance().findUrl(tmpmsg, match, LLUrlRegistryNullCallback, false, true))
    {
        tmpmsg.replace(match.getStart(), match.getEnd() - match.getStart() + 1, match.getLabel());
    }

    if(chat.mChatStyle == CHAT_STYLE_IRC)
    {
        formated_msg = chat.mFromName + tmpmsg.substr(3);
    }
    else
    {
        formated_msg = tmpmsg;
    }

}

//static
std::string LLViewerChat::getSenderSLURL(const LLChat& chat, const LLSD& args)
{
    switch (chat.mSourceType)
    {
    case CHAT_SOURCE_AGENT:
        return LLSLURL("agent", chat.mFromID, "about").getSLURLString();

    case CHAT_SOURCE_OBJECT:
        return getObjectImSLURL(chat, args);

    default:
        LL_WARNS() << "Getting SLURL for an unsupported sender type: " << chat.mSourceType << LL_ENDL;
    }

    return LLStringUtil::null;
}

//static
bool LLViewerChat::hideSenderName(EChatSourceType source_type, const std::string& from_name)
{
    static LLCachedControl<bool> hide_unnamed(gSavedSettings, "ChatHideUnnamedObjectNames", true);
    if (!hide_unnamed || source_type != CHAT_SOURCE_OBJECT)
    {
        return false;
    }
    // Live chat and object IMs substitute this placeholder for an empty or blank name
    // (process_chat_from_simulator, LLIMProcessing); transcripts store it verbatim.
    return from_name.empty() || from_name == LLTrans::getString("Unnamed");
}

//static
std::string LLViewerChat::getObjectImSLURL(const LLChat& chat, const LLSD& args)
{
    std::string url = LLSLURL("objectim", chat.mFromID, "").getSLURLString();
    url += "?name=" + chat.mFromName;
    url += "&owner=" + chat.mOwnerID.asString();

    std::string slurl = args["slurl"].asString();
    if (slurl.empty())
    {
        LLViewerRegion *region = LLWorld::getInstance()->getRegionFromPosAgent(chat.mPosAgent);
        if(region)
        {
            LLSLURL region_slurl(region->getName(), chat.mPosAgent);
            slurl = region_slurl.getLocationString();
        }
    }

    url += "&slurl=" + LLURI::escape(slurl);

    return url;
}

//static
boost::signals2::connection LLViewerChat::setFontChangedCallback(const font_change_signal_t::slot_type& cb)
{
    return sChatFontChangedSignal.connect(cb);
}

//static
void LLViewerChat::signalChatFontChanged()
{
    // Notify all observers that our font has changed
    sChatFontChangedSignal(getChatFont());
}

bool LLViewerChat::isNearbyChatOverlayVisible()
{
    if (gAgent.isDoNotDisturb())
    {
        return false;
    }
    static LLCachedControl<bool> hide_when_visible(gSavedSettings, "NearbyChatOverlayHideWhenVisible", true);
    if (!hide_when_visible)
    {
        return true;
    }
    auto* container = LLFloaterReg::findTypedInstance<LLFloaterIMContainer>("im_container");
    return !container || !container->isConversationVisible(LLUUID::null);
}

LLFontGL::StyleFlags LLViewerChat::getChatFontStyle(const LLChat& chat)
{
    if (chat.mChatStyle == CHAT_STYLE_IRC || chat.mChatType == CHAT_TYPE_WHISPER)
    {
        return LLFontGL::ITALIC;
    }
    return chat.mChatType == CHAT_TYPE_SHOUT ? LLFontGL::BOLD : LLFontGL::NORMAL;
}

std::string LLViewerChat::getNearbyChatOverlayText(const LLChat& chat)
{
    std::string name = hideSenderName(chat) ? std::string() : chat.mFromName;
    std::string text = chat.mText;
    const bool emote = FSCommon::is_irc_me_prefix(text);
    if (emote)
    {
        text.erase(0, 3);
        if (name.empty())
        {
            LLStringUtil::trimHead(text);
        }
        return name + text;
    }
    if (name.empty())
    {
        return text;
    }
    const bool spoken = chat.mChatType == CHAT_TYPE_SHOUT || chat.mChatType == CHAT_TYPE_WHISPER;
    return name + (spoken || chat.mSourceType == CHAT_SOURCE_SYSTEM ? " " : ": ") + text;
}
