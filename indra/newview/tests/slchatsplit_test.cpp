/**
 * @file slchatsplit_test.cpp
 * @brief Auto-split of long chat / IM lines (#1505).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "../slchatsplit.h"

#include "../test/lltut.h"

namespace
{
bool validUtf8(const std::string& s)
{
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char)s[i];
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size()) return false;
        for (size_t j = 1; j < n; ++j)
            if (((unsigned char)s[i + j] & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

std::string words(int n)
{
    std::string t;
    for (int i = 0; i < n; ++i) t += "word" + std::to_string(i) + " ";
    return t + "end";
}
}

namespace tut
{
    struct chatsplit_data {};
    typedef test_group<chatsplit_data> chatsplit_test;
    typedef chatsplit_test::object chatsplit_object;
    tut::chatsplit_test chatsplit("SLChatSplit");

    // A line that fits is sent as it is.
    template<> template<>
    void chatsplit_object::test<1>()
    {
        const auto v = SLChatSplit::split("hello there");
        ensure_equals("one piece", v.size(), (size_t)1);
        ensure_equals("unchanged", v[0], std::string("hello there"));
    }

    // Every piece fits, words are not cut, markers mark the joins.
    template<> template<>
    void chatsplit_object::test<2>()
    {
        const std::string text = words(500);
        const auto v = SLChatSplit::split(text);
        ensure("split", v.size() > 1);
        std::string joined;
        for (size_t i = 0; i < v.size(); ++i)
        {
            ensure("fits", v[i].size() <= SLChatSplit::MAX_MESSAGE_BYTES);
            std::string body = v[i];
            if (i > 0)
            {
                ensure("continued piece starts with ...", body.compare(0, 3, "...") == 0);
                body = body.substr(3);
            }
            if (i + 1 < v.size())
            {
                ensure("continuing piece ends with ...", body.size() >= 3 && body.compare(body.size() - 3, 3, "...") == 0);
                body = body.substr(0, body.size() - 3);
                joined += body + " ";
            }
            else
            {
                joined += body;
            }
        }
        ensure_equals("nothing lost, no word cut", joined, text);
    }

    // No markers when turned off; UTF-8 is never cut inside a character.
    template<> template<>
    void chatsplit_object::test<3>()
    {
        std::string text;
        for (int i = 0; i < 900; ++i) text += "\xC3\xA9\xE2\x82\xAC";   // no spaces at all
        const auto v = SLChatSplit::split(text, SLChatSplit::MAX_MESSAGE_BYTES, false);
        size_t total = 0;
        for (const std::string& p : v)
        {
            ensure("fits", p.size() <= SLChatSplit::MAX_MESSAGE_BYTES);
            ensure("valid UTF-8", validUtf8(p));
            total += p.size();
        }
        ensure_equals("nothing added or lost", total, text.size());
    }

    // An emote stays an emote on every piece.
    template<> template<>
    void chatsplit_object::test<4>()
    {
        const auto v = SLChatSplit::split("/me " + words(500));
        ensure("split", v.size() > 1);
        for (const std::string& p : v)
        {
            ensure("emote", p.compare(0, 4, "/me ") == 0);
            ensure("fits", p.size() <= SLChatSplit::MAX_MESSAGE_BYTES);
        }
    }
}
