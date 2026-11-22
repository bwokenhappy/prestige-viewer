/**
 * @file slimchiclet.cpp
 * @brief One open IM conversation as an icon in the top bar (#1489, SLShowIMChiclets).
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

#include "slimchiclet.h"

#include "llagent.h"
#include "llavataractions.h"
#include "llfloaterimcontainer.h"
#include "llfloaterimsession.h"
#include "llfloaterimsessiontab.h"
#include "llfontgl.h"
#include "llgroupactions.h"
#include "llimview.h"
#include "llmenugl.h"
#include "llmutelist.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"
#include "lluictrlfactory.h"
#include "llviewermenu.h"
#include "rlvactions.h"
#include "rlvcommon.h"

static LLDefaultChildRegistry::Register<SLIMChiclet> r_sl_chiclet_im("chiclet_im");

SLIMChiclet::Params::Params()
:   chiclet_button("chiclet_button"),
    avatar_icon("avatar_icon"),
    group_icon("group_icon"),
    conference_icon("conference_icon"),
    new_message_icon("new_message_icon"),
    speaker("speaker")
{
}

SLIMChiclet::SLIMChiclet(const Params& p)
:   LLIMChiclet(p)
{
    LLButton::Params button_params = p.chiclet_button;
    mChicletButton = LLUICtrlFactory::create<LLButton>(button_params);
    addChild(mChicletButton);

    LLChicletAvatarIconCtrl::Params avatar_params = p.avatar_icon;
    mAvatarIcon = LLUICtrlFactory::create<LLChicletAvatarIconCtrl>(avatar_params);
    addChild(mAvatarIcon);

    LLGroupIconCtrl::Params group_params = p.group_icon;
    mGroupIcon = LLUICtrlFactory::create<LLGroupIconCtrl>(group_params);
    addChild(mGroupIcon);

    LLIconCtrl::Params conference_params = p.conference_icon;
    mConferenceIcon = LLUICtrlFactory::create<LLIconCtrl>(conference_params);
    addChild(mConferenceIcon);

    LLIconCtrl::Params new_msg_params = p.new_message_icon;
    mNewMessagesIcon = LLUICtrlFactory::create<LLIconCtrl>(new_msg_params);
    addChild(mNewMessagesIcon);

    LLOutputMonitorCtrl::Params speaker_params = p.speaker;
    mSpeaker = LLUICtrlFactory::create<LLOutputMonitorCtrl>(speaker_params);
    addChild(mSpeaker);
    mSpeaker->setVisible(false);   // (the voice indicator manager shows it while someone speaks)

    sendChildToFront(mNewMessagesIcon);

    // only the icon for the kind shows (setup)
    mAvatarIcon->setVisible(false);
    mGroupIcon->setVisible(false);
    mConferenceIcon->setVisible(false);
}

bool SLIMChiclet::nameHidden() const
{
    return (mKind == slimchiclet::Kind::Person || mKind == slimchiclet::Kind::Conference) && mOtherParticipantId.notNull()
        && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, mOtherParticipantId);
}

void SLIMChiclet::setup(const std::string& name, const LLUUID& other_participant_id, slimchiclet::Kind kind)
{
    mKind = kind;
    mName = name;
    setOtherParticipantId(other_participant_id);
    mGroupIcon->setVisible(kind == slimchiclet::Kind::Group);
    if (kind == slimchiclet::Kind::Group)
        mGroupIcon->setValue(other_participant_id);
    // the voice indicator: the other person speaking, or (group / conference) anyone in that voice channel
    if (kind == slimchiclet::Kind::Person)
        mSpeaker->setSpeakerId(other_participant_id, getSessionId());
    else
        mSpeaker->setSpeakerId(gAgentID, getSessionId(), true);
    refresh();
}

void SLIMChiclet::refresh()
{
    const LLUUID& session_id = getSessionId();
    const LLIMModel::LLIMSession* s = LLIMModel::getInstance()->findIMSession(session_id);

    // tooltip: the conversation's current name (display names arrive after the session opens)
    std::string name = s && !s->mName.empty() ? s->mName : mName;
    const bool hidden = nameHidden();
    if (hidden) name = RlvStrings::getAnonym(name);
    setToolTip(name);
    // the picture: the person's; a conference shows its other participant's tinted green, or the conference
    // icon without one (or while @shownames hides who that is)
    const bool conference_face = mKind == slimchiclet::Kind::Conference && mOtherParticipantId.notNull() && !hidden;
    mAvatarIcon->setVisible(mKind == slimchiclet::Kind::Person || conference_face);
    mConferenceIcon->setVisible(mKind == slimchiclet::Kind::Conference && !conference_face);
    if (mKind == slimchiclet::Kind::Person)
        mAvatarIcon->setValue(hidden ? LLUUID::null : mOtherParticipantId);
    else if (conference_face)
    {
        mAvatarIcon->setValue(mOtherParticipantId);
        mAvatarIcon->setColor(LLColor4(0.55f, 1.f, 0.55f, 1.f));
    }

    // badge: the other participants' unread messages, not while the conversation is being read
    LLFloaterIMSession* floater = LLFloaterIMSession::findInstance(session_id);
    const bool reading = floater && floater->getVisible() && floater->hasFocus();
    const bool muted_group = s && s->isGroupSessionType() && LLMuteList::getInstance()->isMuted(session_id);
    mCounter = slimchiclet::badgeCount(s ? s->mParticipantUnreadMessageCount : 0, reading, muted_group);
    setShowNewMessagesIcon(mCounter > 0);   // (the dot on the picture; also re-fits the width for the count)
}

S32 SLIMChiclet::counterLeft() const
{
    S32 x = mDefaultWidth;
    if (mSpeaker && mSpeaker->getVisible()) x += mSpeaker->getRect().getWidth();
    return x;
}

void SLIMChiclet::setRequiredWidth()
{
    if (mSpeaker) mSpeaker->setOrigin(mDefaultWidth, mSpeaker->getRect().mBottom);
    S32 width = counterLeft();
    const std::string label = slimchiclet::badgeLabel(mCounter);
    if (!label.empty()) width += LLFontGL::getFontSansSerifSmallBold()->getWidth(label) + 6;
    if (width != getRect().getWidth())
    {
        reshape(width, getRect().getHeight());
        onChicletSizeChanged();
    }
}

S32 SLIMChiclet::notifyParent(const LLSD& info)
{
    if (info.has("visibility_changed"))   // (LLOutputMonitorCtrl: the speaking indicator came or went)
    {
        setRequiredWidth();
        return 1;
    }
    return LLIMChiclet::notifyParent(info);
}

void SLIMChiclet::onMouseDown()
{
    // Shown and in front: hide it again (Conversations as its close button does: every conversation stays
    // open; a torn-off conversation only hides). Otherwise: show it, selected in the conversation list.
    const LLUUID& session_id = getSessionId();
    LLFloaterIMContainer* box = LLFloaterIMContainer::findInstance();
    LLFloaterIMSessionTab* conversation = LLFloaterIMSessionTab::findConversation(session_id);
    if (box && box->isConversationVisible(session_id))
    {
        if (conversation && conversation->isTornOff())
        {
            if (conversation->isFrontmost())
            {
                conversation->setVisible(false);
                return;
            }
        }
        else if (box->isFrontmost())
        {
            static_cast<LLFloater*>(box)->closeHostedFloater();   // (its close button: hide, keep every conversation)
            return;
        }
    }
    LLFloaterIMContainer::getInstance()->showConversation(session_id);
    refresh();
}

void SLIMChiclet::draw()
{
    LLIMChiclet::draw();
    const std::string label = slimchiclet::badgeLabel(mCounter);
    if (label.empty()) return;
    // the count beside the picture (and the speaking indicator), in the width setRequiredWidth made for it
    static LLUIColor text = LLUIColorTable::instance().getColor("LabelTextColor");
    LLFontGL::getFontSansSerifSmallBold()->renderUTF8(label, 0, (F32)(counterLeft() + 3), getRect().getHeight() * 0.5f,
                                                      text.get(), LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                                                      LLFontGL::NO_SHADOW);
}

void SLIMChiclet::createPopupMenu()
{
    if (!canCreateMenu()) return;
    const char* file = slimchiclet::menuFile(mKind);
    if (!file) return;

    LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;
    registrar.add("IMChicletMenu.Action", boost::bind(&SLIMChiclet::onMenuItemClicked, this, _2));
    LLMenuGL* menu = LLUICtrlFactory::getInstance()->createFromFile<LLMenuGL>(
        file, gMenuHolder, LLViewerMenuHolderGL::child_registry_t::instance());
    if (menu) mPopupMenuHandle = menu->getHandle();
}

void SLIMChiclet::updateMenuItems()
{
    LLMenuGL* menu = static_cast<LLMenuGL*>(mPopupMenuHandle.get());
    if (!menu) return;
    // Show Session: only while the conversation is not already showing
    LLFloaterIMContainer* box = LLFloaterIMContainer::findInstance();
    const bool showing = box && box->isConversationVisible(getSessionId());
    if (LLView* item = menu->findChild<LLView>(mKind == slimchiclet::Kind::Group ? "Chat" : "Send IM"))
        item->setEnabled(!showing);
    if (mKind != slimchiclet::Kind::Person) return;
    // (RLVa @shownames: nothing that would name or reach the person behind the anonym)
    const bool hidden = nameHidden();
    if (LLView* item = menu->findChild<LLView>("Show Profile")) item->setEnabled(!hidden);
    if (LLView* item = menu->findChild<LLView>("Add Friend"))
        item->setEnabled(!hidden && !LLAvatarActions::isFriend(mOtherParticipantId));
}

void SLIMChiclet::onMenuItemClicked(const LLSD& action)
{
    const std::string a = action.asString();
    switch (mKind)
    {
        case slimchiclet::Kind::Person:
            if (a == "profile")  LLAvatarActions::showProfile(mOtherParticipantId);
            else if (a == "im")  LLFloaterIMContainer::getInstance()->showConversation(getSessionId());
            else if (a == "add") LLAvatarActions::requestFriendshipDialog(mOtherParticipantId);
            else if (a == "end") LLAvatarActions::endIM(mOtherParticipantId);
            break;
        case slimchiclet::Kind::Group:
            if (a == "info")            LLGroupActions::show(mOtherParticipantId);
            else if (a == "group chat") LLFloaterIMContainer::getInstance()->showConversation(getSessionId());
            else if (a == "snooze")     LLGroupActions::snoozeIM(mOtherParticipantId);
            else if (a == "end")        LLGroupActions::endIM(mOtherParticipantId);
            break;
        case slimchiclet::Kind::Conference:
            if (a == "end") LLGroupActions::endIM(getSessionId());   // (ends any session by its id)
            break;
        default:
            break;
    }
}
