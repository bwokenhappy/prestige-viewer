/**
 * @file slchatsplit.cpp
 * @brief Splitting a long chat / IM line into messages the server accepts (#1505).
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

#include "linden_common.h"

#include "slchatsplit.h"

namespace
{
bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }
bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

// The largest n <= limit such that text[0, n) ends on a UTF-8 character boundary.
size_t boundaryAtOrBefore(const std::string& text, size_t limit)
{
    size_t n = std::min(limit, text.size());
    while (n > 0 && n < text.size() && isContinuation((unsigned char)text[n])) --n;
    return n;
}

size_t skipSpaces(const std::string& text, size_t at)
{
    while (at < text.size() && isSpace(text[at])) ++at;
    return at;
}
}

std::vector<std::string> SLChatSplit::split(const std::string& text, size_t max_bytes, bool markers)
{
    std::vector<std::string> out;
    if (text.size() <= max_bytes)
    {
        out.push_back(text);
        return out;
    }
    // (a limit too small for markers / the emote prefix: plain pieces)
    const bool decorate = max_bytes >= 32;
    const std::string marker = markers && decorate ? "..." : "";
    const bool emote = decorate && (text.compare(0, 4, "/me ") == 0 || text.compare(0, 4, "/me'") == 0);
    const std::string cont_prefix = emote ? "/me " : "";

    std::string head;          // what the current piece starts with
    size_t pos = 0;            // start of the rest of the text
    while (pos < text.size())
    {
        const std::string rest = text.substr(pos);
        if (head.size() + rest.size() <= max_bytes)
        {
            out.push_back(head + rest);
            break;
        }
        // Room for this piece's share of the text (it continues: leave room for the marker).
        const size_t room = max_bytes - head.size() - marker.size();   // (decorate: >= 32 - 10)
        size_t cut = boundaryAtOrBefore(rest, room);
        if (cut == 0)
        {
            // not even one character fits after the head: take one whole character
            cut = 1;
            while (cut < rest.size() && isContinuation((unsigned char)rest[cut])) ++cut;
        }
        // Prefer a word boundary that keeps the piece at least half full.
        size_t end = cut;
        if (cut < rest.size() && !isSpace(rest[cut]))
        {
            size_t sp = cut;
            while (sp > cut / 2 && !isSpace(rest[sp - 1])) --sp;
            if (sp > cut / 2 && sp > 1) end = sp - 1;   // break before that space
        }
        size_t piece_end = end;
        while (piece_end > 0 && isSpace(rest[piece_end - 1])) --piece_end;   // no trailing blanks
        if (piece_end == 0) piece_end = end;                                  // (all blank: keep as is)
        out.push_back(head + rest.substr(0, piece_end) + marker);
        pos = skipSpaces(text, pos + end);
        head = cont_prefix + marker;
    }
    return out;
}
