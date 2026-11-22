/**
 * @file llscriptfileupload.h
 * @brief Upload .lsl / .txt files as LSL scripts named after the file.
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

#ifndef LL_LLSCRIPTFILEUPLOAD_H
#define LL_LLSCRIPTFILEUPLOAD_H

#include <string>
#include <vector>

// File > Upload > Script (.lsl / .txt)...: each file becomes a new LSL script in the Scripts
// folder, named after the file (extension dropped, at most 63 characters). The source is read
// as UTF-8 (BOM stripped, CRLF/CR -> LF), then uploaded and compiled as Mono through the
// region's UpdateScriptAgent capability, the same path the script editor's Save takes. A file
// that is unreadable, not valid UTF-8, or larger than the LSL source limit (64 KiB) is
// refused, never truncated. A script that fails to compile stays in inventory with its
// source saved. Free: no cost prompt. One toast per file reports the result.
void start_script_file_upload(const std::vector<std::string>& filenames);

#endif // LL_LLSCRIPTFILEUPLOAD_H
