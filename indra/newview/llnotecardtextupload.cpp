/**
 * @file llnotecardtextupload.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llnotecardtextupload.h"

#include "lldbstrings.h"
#include "lldir.h"
#include "llfile.h"
#include "llnotecard.h"
#include "llnotificationsutil.h"
#include "llstring.h"
#include "lltrans.h"

#include "llappviewer.h"          // gDisconnected
#include "llinventorymodel.h"
#include "llsoundnotecardupload.h"   // create_notecard_with_text()

namespace
{
    // Inventory name from a file name: the same clean-up the uploaders apply to asset names,
    // then clipped to the inventory name limit.
    std::string notecard_name_for(const std::string& filename)
    {
        std::string name = gDirUtilp->getBaseFileName(filename, true);
        LLStringUtil::replaceNonstandardASCII(name, '?');
        LLStringUtil::replaceChar(name, '|', '?');
        LLStringUtil::stripNonprintable(name);
        LLStringUtil::trim(name);
        if (name.size() > (size_t)DB_INV_ITEM_NAME_STR_LEN)
        {
            name.resize(DB_INV_ITEM_NAME_STR_LEN);   // ASCII only after the clean-up above
            LLStringUtil::trim(name);
        }
        if (name.empty())
        {
            name = "Notecard";
        }
        return name;
    }

    // Strict UTF-8 check. Rejects NUL (binary data) and code points in the planes the notecard
    // reserves for embedded items (0x100000 and up), which would corrupt the body.
    bool valid_notecard_utf8(const std::string& s)
    {
        const size_t n = s.size();
        for (size_t i = 0; i < n;)
        {
            const U8 c = (U8)s[i];
            size_t extra = 0;
            U32 cp = 0;
            U32 min_cp = 0;
            if (c == 0)
            {
                return false;
            }
            else if (c < 0x80) { cp = c; }
            else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; min_cp = 0x80; }
            else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; min_cp = 0x800; }
            else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; min_cp = 0x10000; }
            else { return false; }
            if (i + extra >= n)
            {
                return false;
            }
            for (size_t k = 1; k <= extra; ++k)
            {
                const U8 cc = (U8)s[i + k];
                if ((cc & 0xC0) != 0x80)
                {
                    return false;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (cp < min_cp || (cp >= 0xD800 && cp <= 0xDFFF) || cp >= 0x100000)
            {
                return false;
            }
            i += extra + 1;
        }
        return true;
    }

    // Read a text file for a notecard. On failure returns false and sets `reason`.
    bool read_notecard_text(const std::string& filename, std::string& text, std::string& reason)
    {
        // Read at most twice the body limit (CRLF -> LF can shrink a file by up to half);
        // the exact limit is enforced on the converted text below.
        const size_t max_read = (size_t)LLNotecard::MAX_SIZE * 2 + 3;
        llifstream in(filename.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open())
        {
            reason = LLTrans::getString("NotecardFileUnreadable");
            return false;
        }
        std::string raw;
        char buf[8192];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
        {
            raw.append(buf, (size_t)in.gcount());
            if (raw.size() > max_read)
            {
                break;
            }
        }
        if (in.bad())
        {
            reason = LLTrans::getString("NotecardFileUnreadable");
            return false;
        }
        if (raw.size() > max_read)
        {
            reason = LLTrans::getString("NotecardTooLarge");
            return false;
        }

        if (raw.size() >= 3 && (U8)raw[0] == 0xEF && (U8)raw[1] == 0xBB && (U8)raw[2] == 0xBF)
        {
            raw.erase(0, 3);
        }
        if (!valid_notecard_utf8(raw))
        {
            reason = LLTrans::getString("NotecardNotUtf8");
            return false;
        }

        // CRLF and lone CR -> LF.
        text.clear();
        text.reserve(raw.size());
        for (size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i] == '\r')
            {
                text.push_back('\n');
                if (i + 1 < raw.size() && raw[i + 1] == '\n')
                {
                    ++i;
                }
            }
            else
            {
                text.push_back(raw[i]);
            }
        }

        if (text.size() > (size_t)LLNotecard::MAX_SIZE)
        {
            reason = LLTrans::getString("NotecardTooLarge");
            return false;
        }
        return true;
    }

    void report(const std::string& file, bool ok, const std::string& detail)
    {
        LLSD args;
        args["FILE"] = file;
        args["REASON"] = detail;
        LLNotificationsUtil::add(ok ? "TextNotecardCreated" : "TextNotecardFailed", args);
    }
}

void start_text_notecard_upload(const std::vector<std::string>& filenames)
{
    if (filenames.empty())
    {
        return;
    }

    if (gDisconnected)
    {
        report(gDirUtilp->getBaseFileName(filenames.front(), false), false, LLTrans::getString("SoundSplitDisconnected"));
        return;
    }

    const LLUUID folder_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_NOTECARD);

    for (const std::string& filename : filenames)
    {
        const std::string base = gDirUtilp->getBaseFileName(filename, false);

        std::string text;
        std::string reason;
        if (!read_notecard_text(filename, text, reason))
        {
            LL_WARNS() << "Text notecard upload refused for " << filename << ": " << reason << LL_ENDL;
            report(base, false, reason);
            continue;
        }

        const std::string name = notecard_name_for(filename);
        create_notecard_with_text(name, text, folder_id,
            [base](bool ok, const std::string& detail)
            {
                if (!ok)
                {
                    LL_WARNS() << "Text notecard upload failed for " << base << ": " << detail << LL_ENDL;
                }
                report(base, ok, detail);
            });
    }
}
