/**
 * @file slimchicletrules_test.cpp
 * @brief IM icons in the top bar: which conversations get one, which menu, what the badge says.
 *
 * Standalone (no viewer libraries):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I indra/newview indra/newview/tests/slimchicletrules_test.cpp \
 *       -o /tmp/slimchicletrules_test && /tmp/slimchicletrules_test
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */

#include "slimchicletrules.h"

#include <cstdio>
#include <cstring>

using namespace slimchiclet;

static int failures = 0;

static void check(bool ok, const char* what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

int main()
{
    // ---- which conversations get an icon ----
    check(kindFor(true, false, false, false) == Kind::None, "nearby chat (null session) gets no icon");
    check(kindFor(true, true, false, false) == Kind::None, "a null session never gets one, whatever its type");
    check(kindFor(false, true, false, false) == Kind::Person, "a person IM shows their picture");
    check(kindFor(false, false, true, false) == Kind::Group, "a group chat shows its insignia");
    check(kindFor(false, false, false, true) == Kind::Conference, "a conference shows the conference icon");
    check(kindFor(false, false, false, false) == Kind::None, "anything else gets no icon");

    // ---- right-click menus ----
    check(std::strcmp(menuFile(Kind::Person), "menu_imchiclet_p2p.xml") == 0, "a person: the P2P menu");
    check(std::strcmp(menuFile(Kind::Group), "menu_imchiclet_group.xml") == 0, "a group: the group menu");
    check(std::strcmp(menuFile(Kind::Conference), "menu_imchiclet_adhoc.xml") == 0, "a conference: the ad-hoc menu");
    check(menuFile(Kind::None) == nullptr, "no icon, no menu");

    // ---- badge count ----
    check(badgeCount(3, false, false) == 3, "unread messages are counted");
    check(badgeCount(3, true, false) == 0, "none while the conversation is in front with focus");
    check(badgeCount(3, false, true) == 0, "none for a muted group chat");
    check(badgeCount(0, false, false) == 0 && badgeCount(-1, false, false) == 0, "never below zero");

    // ---- badge text ----
    check(badgeLabel(0).empty(), "no badge without unread messages");
    check(badgeLabel(1) == "1" && badgeLabel(99) == "99", "counts up to 99 as they are");
    check(badgeLabel(100) == "99+" && badgeLabel(5000) == "99+", "past 99: 99+");

    std::printf(failures ? "FAILED (%d)\n" : "PASSED (0 failed)\n", failures);
    return failures ? 1 : 0;
}
