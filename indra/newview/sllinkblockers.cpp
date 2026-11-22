/**
 * @file sllinkblockers.cpp
 * @brief Linking (#1418): which selected root objects block a link, and the list of their
 *        names a link failure notice shows.
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

#include "sllinkblockers.h"

#include <map>
#include <utility>

std::string sl_link_majority(const std::vector<std::string>& keys, const std::string& prefer)
{
    // Count each key and remember where it was first seen, so a tie is decided by order.
    std::map<std::string, std::pair<size_t, size_t>> counts; // key -> (count, first index)
    for (size_t i = 0; i < keys.size(); ++i)
    {
        if (keys[i].empty())
        {
            continue;
        }
        auto it = counts.find(keys[i]);
        if (it == counts.end())
        {
            counts.emplace(keys[i], std::make_pair(size_t(1), i));
        }
        else
        {
            ++it->second.first;
        }
    }

    std::string best;
    size_t best_count = 0;
    size_t best_first = 0;
    for (const auto& entry : counts)
    {
        const size_t count = entry.second.first;
        const size_t first = entry.second.second;
        bool better = false;
        if (best.empty() || count > best_count)
        {
            better = true;
        }
        else if (count == best_count)
        {
            if (!prefer.empty() && entry.first == prefer)
            {
                better = true;
            }
            else if (best != prefer && first < best_first)
            {
                better = true;
            }
        }
        if (better)
        {
            best = entry.first;
            best_count = count;
            best_first = first;
        }
    }
    return best;
}

std::string sl_link_rule_majority(const std::vector<SLLinkRoot>& roots, SLLinkRule rule, const std::string& agent_key)
{
    std::vector<std::string> keys;
    keys.reserve(roots.size());
    if (rule == SLLinkRule::Owner)
    {
        // Owners are only known once the properties arrived.
        for (const SLLinkRoot& root : roots)
        {
            keys.push_back(root.valid ? root.owner : std::string());
        }
        return sl_link_majority(keys, agent_key);
    }
    if (rule == SLLinkRule::Region)
    {
        for (const SLLinkRoot& root : roots)
        {
            keys.push_back(root.region);
        }
        return sl_link_majority(keys);
    }
    return std::string();
}

std::vector<size_t> sl_link_blockers(const std::vector<SLLinkRoot>& roots, SLLinkRule rule, const std::string& agent_key)
{
    const std::string majority = sl_link_rule_majority(roots, rule, agent_key);

    std::vector<size_t> out;
    for (size_t i = 0; i < roots.size(); ++i)
    {
        const SLLinkRoot& root = roots[i];
        bool blocks = false;
        switch (rule)
        {
        case SLLinkRule::Downloading:
            blocks = !root.valid;
            break;
        case SLLinkRule::Modify:
            blocks = root.valid && !root.modify;
            break;
        case SLLinkRule::Permanent:
            blocks = root.valid && root.permanent;
            break;
        case SLLinkRule::Owner:
            blocks = root.valid && !majority.empty() && root.owner != majority;
            break;
        case SLLinkRule::Region:
            blocks = !majority.empty() && root.region != majority;
            break;
        }
        if (blocks)
        {
            out.push_back(i);
        }
    }
    return out;
}

std::string sl_link_substitute(std::string tmpl, const std::string& key, const std::string& value)
{
    const std::string token = "[" + key + "]";
    size_t pos = 0;
    while ((pos = tmpl.find(token, pos)) != std::string::npos)
    {
        tmpl.replace(pos, token.size(), value);
        pos += value.size(); // never rescan the inserted value
    }
    return tmpl;
}

std::vector<std::string> sl_link_blocker_labels(const std::vector<SLLinkRoot>& roots,
                                                const std::vector<size_t>& which,
                                                const std::string& loading_tmpl,
                                                const std::string& ambiguous_tmpl)
{
    auto known = [&](size_t i) { return roots[i].valid && !roots[i].name.empty(); };

    std::map<std::string, size_t> name_counts;
    for (size_t i : which)
    {
        if (i < roots.size() && known(i))
        {
            ++name_counts[roots[i].name];
        }
    }

    std::vector<std::string> labels;
    labels.reserve(which.size());
    for (size_t i : which)
    {
        if (i >= roots.size())
        {
            continue;
        }
        const SLLinkRoot& root = roots[i];
        if (!known(i))
        {
            labels.push_back(sl_link_substitute(loading_tmpl, "POS", root.where));
        }
        else if (name_counts[root.name] > 1)
        {
            // [POS] before [NAME]: the name is user text and may itself hold "[POS]".
            labels.push_back(sl_link_substitute(sl_link_substitute(ambiguous_tmpl, "POS", root.where), "NAME", root.name));
        }
        else
        {
            labels.push_back(root.name);
        }
    }
    return labels;
}

std::string sl_link_blocker_list(const std::vector<std::string>& labels, size_t cap, const std::string& more_tmpl)
{
    static const std::string BULLET = "\xE2\x80\xA2 "; // U+2022 and a space

    std::string out;
    const size_t shown = labels.size() < cap ? labels.size() : cap;
    for (size_t i = 0; i < shown; ++i)
    {
        if (!out.empty())
        {
            out += '\n';
        }
        out += BULLET + labels[i];
    }
    if (labels.size() > shown)
    {
        if (!out.empty())
        {
            out += '\n';
        }
        out += sl_link_substitute(more_tmpl, "N", std::to_string(labels.size() - shown));
    }
    return out;
}
