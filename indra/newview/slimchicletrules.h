/**
 * @file slimchicletrules.h
 * @brief IM icons in the top bar (#1489): which conversations get one, and what their badge says.
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

#ifndef SL_SLIMCHICLETRULES_H
#define SL_SLIMCHICLETRULES_H

#include <string>

// Pure rules (no viewer types) so they can be tested on their own: indra/newview/tests/slimchicletrules_test.cpp.
namespace slimchiclet
{
// What an open conversation shows as: a person's picture, a group's insignia, or a conference (several
// people) -- or nothing (nearby chat, and anything that is not one of the three).
enum class Kind { None, Person, Group, Conference };

inline Kind kindFor(bool null_session, bool p2p, bool group, bool adhoc)
{
    if (null_session) return Kind::None;   // nearby chat
    if (p2p) return Kind::Person;
    if (group) return Kind::Group;
    if (adhoc) return Kind::Conference;
    return Kind::None;
}

// The menu a right click opens for each kind (the files already in the skin).
inline const char* menuFile(Kind kind)
{
    switch (kind)
    {
        case Kind::Person:     return "menu_imchiclet_p2p.xml";
        case Kind::Group:      return "menu_imchiclet_group.xml";
        case Kind::Conference: return "menu_imchiclet_adhoc.xml";
        default:               return nullptr;
    }
}

// The unread messages the badge counts: none while the conversation is in front with focus (they are
// being read), none for a muted group chat; otherwise the other participants' messages.
inline int badgeCount(int participant_unread, bool showing_with_focus, bool muted_group)
{
    if (showing_with_focus || muted_group || participant_unread <= 0) return 0;
    return participant_unread;
}

// The badge's text: empty without unread messages, "99+" past 99.
inline std::string badgeLabel(int count, int max_shown = 99)
{
    if (count <= 0) return std::string();
    if (count > max_shown) return std::to_string(max_shown) + "+";
    return std::to_string(count);
}
}   // namespace slimchiclet

#endif   // SL_SLIMCHICLETRULES_H
