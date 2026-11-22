/**
 * @file slimchiclet.h
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

#ifndef SL_SLIMCHICLET_H
#define SL_SLIMCHICLET_H

#include "llchiclet.h"
#include "llgroupiconctrl.h"
#include "lloutputmonitorctrl.h"
#include "slimchicletrules.h"

// An open IM conversation in the top bar's chiclet list (LLChicletBar, while SLShowIMChiclets is on):
// the person's picture, the group's insignia, or the conference's other participant tinted green (the
// conference icon without one); while unread messages wait, a dot on the picture and their count beside
// it; while the conversation's voice is live, a speaking indicator beside it. Left click shows the
// conversation selected in Conversations, or hides it again when it is already in front; right click
// opens the menu for its kind (menu_imchiclet_p2p / _group / _adhoc.xml). Widget: widgets/chiclet_im.xml.
class SLIMChiclet : public LLIMChiclet
{
public:
    struct Params : public LLInitParam::Block<Params, LLIMChiclet::Params>
    {
        Optional<LLButton::Params>                chiclet_button;
        Optional<LLChicletAvatarIconCtrl::Params> avatar_icon;
        Optional<LLGroupIconCtrl::Params>         group_icon;
        Optional<LLIconCtrl::Params>              conference_icon;
        Optional<LLIconCtrl::Params>              new_message_icon;
        Optional<LLOutputMonitorCtrl::Params>     speaker;
        Params();
    };

    // The conversation this icon stands for. Call once, after the panel created it with the session id.
    void setup(const std::string& name, const LLUUID& other_participant_id, slimchiclet::Kind kind);
    slimchiclet::Kind getKind() const { return mKind; }

    // Re-reads the unread count (badge) and the conversation's name (tooltip) from the IM model.
    void refresh();

    void draw() override;
    void onMouseDown() override;
    // the width for the icon, the speaking indicator while it shows and the count while there is one
    void setRequiredWidth() override;
    S32 notifyParent(const LLSD& info) override;   // (the speaking indicator showing / hiding)

protected:
    SLIMChiclet(const Params& p);
    friend class LLUICtrlFactory;

    void createPopupMenu() override;
    void updateMenuItems() override;

private:
    void onMenuItemClicked(const LLSD& action);
    // RLVa @shownames hides who the other side of a person IM (or a conference's other participant) is:
    // no picture, an anonym for a name.
    bool nameHidden() const;
    S32 counterLeft() const;   // where the count is drawn: after the icon and the speaking indicator

    LLChicletAvatarIconCtrl* mAvatarIcon = nullptr;
    LLGroupIconCtrl*         mGroupIcon = nullptr;
    LLIconCtrl*              mConferenceIcon = nullptr;
    LLOutputMonitorCtrl*     mSpeaker = nullptr;
    slimchiclet::Kind        mKind = slimchiclet::Kind::None;
    std::string              mName;
    S32                      mCounter = 0;
};

#endif   // SL_SLIMCHICLET_H
