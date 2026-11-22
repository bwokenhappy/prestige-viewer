/**
 * @file llnotecardtextupload.h
 * @brief Upload plain-text (.txt) files as notecards named after the file.
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

#ifndef LL_LLNOTECARDTEXTUPLOAD_H
#define LL_LLNOTECARDTEXTUPLOAD_H

#include <string>
#include <vector>

// File > Upload > Notecard (.txt)...: each file becomes a new notecard in the Notecards
// folder, named after the file (extension dropped, at most 63 characters). The text is read
// as UTF-8 (BOM stripped, CRLF/CR -> LF). A file that is unreadable, not valid UTF-8, or
// larger than the notecard limit (LLNotecard::MAX_SIZE bytes) is refused, never truncated.
// Free: no cost prompt. One toast per file reports the result.
void start_text_notecard_upload(const std::vector<std::string>& filenames);

#endif // LL_LLNOTECARDTEXTUPLOAD_H
