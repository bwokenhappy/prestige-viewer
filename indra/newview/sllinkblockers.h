/**
 * @file sllinkblockers.h
 * @brief Linking (#1418): which selected root objects block a link, and the list of their
 *        names a link failure notice shows. No viewer objects here (LLSelectMgr::linkObjects
 *        fills the facts from the selection's root nodes when a link check fails), so the
 *        rules are unit-tested standalone (tests/sllinkblockers_test.cpp).
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

#ifndef SL_SLLINKBLOCKERS_H
#define SL_SLLINKBLOCKERS_H

#include <cstddef>
#include <string>
#include <vector>

// What a link check needs to know about one selected root object.
struct SLLinkRoot
{
    std::string name;       // object name from the selection's properties; empty until they arrive
    std::string where;      // short position text, to tell unnamed or same-named objects apart
    std::string owner;      // owner key (the group when group-owned); empty when unknown
    std::string region;     // region key; empty when the object has no region
    bool valid = false;     // the object's properties have arrived (LLSelectNode::mValid)
    bool modify = false;    // the agent may modify it
    bool permanent = false; // permanent / pathfinding-enforced
};

// The link checks that can name their offenders, in the order linkObjects() runs them.
enum class SLLinkRule
{
    Downloading,    // properties not arrived yet
    Modify,         // no modify permission
    Permanent,      // permanent / pathfinding-enforced
    Owner,          // owner differs from the majority owner
    Region,         // in a different region from the majority
};

// The most common non-empty key. A tie goes to `prefer` when it is among the tied keys,
// otherwise to the tied key seen first. Empty when there is no non-empty key.
std::string sl_link_majority(const std::vector<std::string>& keys, const std::string& prefer = std::string());

// Indices (into roots) of the roots that break `rule`. For Owner, the majority owner is
// chosen among the valid roots with a tie going to `agent_key`; for Region, the majority
// region among all roots. Roots without a key always differ from the majority.
std::vector<size_t> sl_link_blockers(const std::vector<SLLinkRoot>& roots, SLLinkRule rule,
                                     const std::string& agent_key = std::string());

// The majority key the Owner / Region rules compare against (empty for the other rules).
std::string sl_link_rule_majority(const std::vector<SLLinkRoot>& roots, SLLinkRule rule,
                                  const std::string& agent_key = std::string());

// Replaces every "[KEY]" in tmpl with value.
std::string sl_link_substitute(std::string tmpl, const std::string& key, const std::string& value);

// One label per index in `which`: the object's name; `ambiguous_tmpl` ([NAME], [POS]) when
// another listed object has the same name; `loading_tmpl` ([POS]) when the name is unknown.
std::vector<std::string> sl_link_blocker_labels(const std::vector<SLLinkRoot>& roots,
                                                const std::vector<size_t>& which,
                                                const std::string& loading_tmpl,
                                                const std::string& ambiguous_tmpl);

// The labels one per line ("• label"), at most `cap` of them, then `more_tmpl` with [N] set
// to the number left out. Empty when there are no labels.
std::string sl_link_blocker_list(const std::vector<std::string>& labels, size_t cap,
                                 const std::string& more_tmpl);

#endif // SL_SLLINKBLOCKERS_H
