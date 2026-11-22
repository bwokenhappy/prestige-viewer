/**
 * @file sllinkblockers_test.cpp
 * @brief #1418 link failures name their objects: which roots break which link rule, the
 *        majority owner / region they are compared against, and the capped name list.
 *
 * Standalone (no viewer libraries):
 *   g++ -std=c++17 -Wall -Wextra -Werror -I indra/newview indra/newview/tests/sllinkblockers_test.cpp \
 *       indra/newview/sllinkblockers.cpp -o /tmp/sllinkblockers_test && /tmp/sllinkblockers_test
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

#include "sllinkblockers.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
int g_fail = 0;
void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

using Idx = std::vector<size_t>;

SLLinkRoot root(const std::string& name, const std::string& owner = "me", const std::string& region = "r1")
{
    SLLinkRoot r;
    r.name = name;
    r.where = "pos-" + name;
    r.owner = owner;
    r.region = region;
    r.valid = true;
    r.modify = true;
    r.permanent = false;
    return r;
}
} // namespace

int main()
{
    // ---- rules ----
    {
        std::vector<SLLinkRoot> roots = { root("a"), root("b"), root("c"), root("d") };
        roots[1].modify = false;
        roots[3].modify = false;
        check(sl_link_blockers(roots, SLLinkRule::Modify) == Idx({ 1, 3 }), "Modify names the no-mod roots in selection order");
        check(sl_link_blockers(roots, SLLinkRule::Permanent).empty(), "Permanent: nothing when no root is permanent");
        check(sl_link_blockers(roots, SLLinkRule::Downloading).empty(), "Downloading: nothing when all properties arrived");
    }
    {
        std::vector<SLLinkRoot> roots = { root("a"), root("b"), root("c") };
        roots[0].permanent = true;
        check(sl_link_blockers(roots, SLLinkRule::Permanent) == Idx({ 0 }), "Permanent names the permanent root");
        check(sl_link_blockers(roots, SLLinkRule::Modify).empty(), "Modify ignores permanent-but-modifiable roots");
    }
    {
        std::vector<SLLinkRoot> roots = { root("a"), root(""), root("c") };
        roots[1].valid = false;
        roots[1].modify = false;
        check(sl_link_blockers(roots, SLLinkRule::Downloading) == Idx({ 1 }), "Downloading names the roots without properties");
        check(sl_link_blockers(roots, SLLinkRule::Modify).empty(), "Modify only judges roots whose properties arrived");
    }

    // ---- majority ----
    check(sl_link_majority({ "x", "y", "y", "x", "y" }) == "y", "majority: most common key");
    check(sl_link_majority({ "x", "y" }) == "x", "majority: tie goes to the first seen");
    check(sl_link_majority({ "x", "y" }, "y") == "y", "majority: tie goes to the preferred key");
    check(sl_link_majority({ "x", "x", "y" }, "y") == "x", "majority: the preferred key does not beat a real majority");
    check(sl_link_majority({ "", "", "z" }) == "z", "majority: empty keys never win");
    check(sl_link_majority({ "", "" }).empty(), "majority: none when no key is known");
    check(sl_link_majority({}).empty(), "majority: none for no keys");

    {
        // 3 owned by bob, 2 by me: bob is the majority, mine are named.
        std::vector<SLLinkRoot> roots = { root("a", "bob"), root("b", "me"), root("c", "bob"), root("d", "me"), root("e", "bob") };
        check(sl_link_rule_majority(roots, SLLinkRule::Owner, "me") == "bob", "Owner: majority owner");
        check(sl_link_blockers(roots, SLLinkRule::Owner, "me") == Idx({ 1, 3 }), "Owner names the roots not owned by the majority owner");
    }
    {
        // Tie: the agent's own objects are the ones kept.
        std::vector<SLLinkRoot> roots = { root("a", "bob"), root("b", "me") };
        check(sl_link_blockers(roots, SLLinkRule::Owner, "me") == Idx({ 0 }), "Owner: a tie keeps the agent's objects");
    }
    {
        // Unknown owner differs from everyone.
        std::vector<SLLinkRoot> roots = { root("a"), root("b", ""), root("c") };
        check(sl_link_blockers(roots, SLLinkRule::Owner, "me") == Idx({ 1 }), "Owner: an unknown owner is named");
        check(sl_link_rule_majority(roots, SLLinkRule::Modify).empty(), "rule majority only for Owner / Region");
    }
    {
        std::vector<SLLinkRoot> roots = { root("a", "me", "r1"), root("b", "me", "r2"), root("c", "me", "r2"), root("d", "me", "") };
        check(sl_link_rule_majority(roots, SLLinkRule::Region) == "r2", "Region: majority region");
        check(sl_link_blockers(roots, SLLinkRule::Region) == Idx({ 0, 3 }), "Region names the roots outside the majority region");
        std::vector<SLLinkRoot> same = { root("a"), root("b") };
        check(sl_link_blockers(same, SLLinkRule::Region).empty(), "Region: nothing when all share a region");
    }

    // ---- substitution ----
    check(sl_link_substitute("and [N] more ([N])", "N", "3") == "and 3 more (3)", "substitute replaces every [KEY]");
    check(sl_link_substitute("[NAME]", "N", "3") == "[NAME]", "substitute leaves other keys alone");
    check(sl_link_substitute("[N]", "N", "[N]") == "[N]", "substitute does not loop on its own value");

    // ---- labels ----
    {
        std::vector<SLLinkRoot> roots = { root("Chair"), root("Table"), root("Chair"), root("") };
        roots[3].valid = false;
        std::vector<std::string> labels = sl_link_blocker_labels(roots, { 0, 1, 2, 3 }, "(loading) at [POS]", "[NAME] at [POS]");
        check(labels.size() == 4, "one label per listed root");
        check(labels.size() == 4 && labels[0] == "Chair at pos-Chair", "same-named roots get their position");
        check(labels.size() == 4 && labels[1] == "Table", "a unique name stands alone");
        check(labels.size() == 4 && labels[3] == "(loading) at pos-", "an unknown name shows loading and the position");

        // Same name, but only one of them is listed: no position needed.
        std::vector<std::string> one = sl_link_blocker_labels(roots, { 0, 1 }, "(loading) at [POS]", "[NAME] at [POS]");
        check(one.size() == 2 && one[0] == "Chair", "ambiguity only among the listed roots");

        // A name that looks like a template key stays as typed.
        std::vector<SLLinkRoot> keyed = { root("x[POS]"), root("x[POS]") };
        std::vector<std::string> k = sl_link_blocker_labels(keyed, { 0, 1 }, "(loading) at [POS]", "[NAME] at [POS]");
        check(k.size() == 2 && k[0] == "x[POS] at pos-x[POS]", "a name holding [POS] is not substituted into");

        // A valid root with an empty name is still unknown.
        std::vector<SLLinkRoot> blank = { root("") };
        std::vector<std::string> b = sl_link_blocker_labels(blank, { 0 }, "(loading) at [POS]", "[NAME] at [POS]");
        check(b.size() == 1 && b[0] == "(loading) at pos-", "a blank name falls back to the position");
    }

    // ---- list with the cap ----
    {
        const std::string bullet = "\xE2\x80\xA2 ";
        check(sl_link_blocker_list({}, 10, "and [N] more").empty(), "list: empty for no labels");
        check(sl_link_blocker_list({ "a", "b" }, 10, "and [N] more") == bullet + "a\n" + bullet + "b", "list: one bulleted line each");

        std::vector<std::string> ten, twelve;
        for (int i = 0; i < 12; ++i)
        {
            (i < 10 ? ten : twelve).push_back("n" + std::to_string(i));
        }
        twelve.insert(twelve.begin(), ten.begin(), ten.end());
        std::string all10 = sl_link_blocker_list(ten, 10, "and [N] more");
        check(all10.find("more") == std::string::npos, "list: exactly the cap shows no 'more' line");
        std::string capped = sl_link_blocker_list(twelve, 10, "and [N] more");
        check(capped.find("n9") != std::string::npos && capped.find("n10") == std::string::npos, "list: stops at the cap");
        check(capped.size() >= 11 && capped.substr(capped.size() - 11) == "\nand 2 more", "list: then 'and N more' on its own line");
        check(sl_link_blocker_list({ "a", "b" }, 0, "and [N] more") == "and 2 more", "list: cap 0 is just the count");
    }

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
