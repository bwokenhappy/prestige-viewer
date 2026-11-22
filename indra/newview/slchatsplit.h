/**
 * @file slchatsplit.h
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

#ifndef SL_SLCHATSPLIT_H
#define SL_SLCHATSPLIT_H

#include <string>
#include <vector>

namespace SLChatSplit
{
// The server keeps at most 1023 bytes of one nearby chat or IM message (1024 with the NUL).
constexpr size_t MAX_MESSAGE_BYTES = 1023;
// What the chat input accepts while auto-split is on (16 messages' worth).
constexpr size_t MAX_INPUT_BYTES = 16 * MAX_MESSAGE_BYTES;

// Split a UTF-8 line into pieces of at most max_bytes each: at the last space / line break
// that keeps a piece at least half full, else at a character boundary (never inside a UTF-8
// sequence). markers: a piece that continues ends with "..." and the next starts with "...".
// A "/me" emote keeps "/me " on every piece, so each one shows as an emote. A line that
// already fits comes back unchanged as the only piece.
std::vector<std::string> split(const std::string& text, size_t max_bytes = MAX_MESSAGE_BYTES, bool markers = true);
}

#endif // SL_SLCHATSPLIT_H
