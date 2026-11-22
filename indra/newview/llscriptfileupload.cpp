/**
 * @file llscriptfileupload.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llscriptfileupload.h"

#include <map>
#include <memory>

#include "lldbstrings.h"
#include "lldir.h"
#include "llfile.h"
#include "llnotificationsutil.h"
#include "llpermissionsflags.h"
#include "llstring.h"
#include "lltrans.h"

#include "llagent.h"
#include "llappviewer.h"          // gDisconnected
#include "llcallbacklist.h"       // doAfterInterval()
#include "llfloaterperms.h"
#include "llinventorymodel.h"
#include "llviewerassetupload.h"  // LLScriptAssetUpload
#include "llviewerinventory.h"
#include "llviewermessage.h"      // suppress_inventory_auto_open_for_folder()

// llviewerinventory.cpp (not in a header): applies the Scripts everyone / group defaults.
void set_default_permissions(LLViewerInventoryItem* item, std::string perm_type);

namespace
{
    // The most source text a script upload takes. The script editor does not cap the text
    // itself; the sim refuses anything past 64 KiB, so refuse here with a clear message
    // instead of uploading an item that can never compile.
    constexpr size_t kMaxScriptSource = 65536;

    // The compile target the script editor's Save uses for a new LSL script.
    const char* const kCompileTarget = "mono";

    // Compile errors shown in the toast; the rest are summarised.
    constexpr size_t kMaxErrorLines = 8;

    // Inventory name from a file name: the same clean-up the uploaders apply to asset names,
    // then clipped to the inventory name limit.
    std::string script_name_for(const std::string& filename)
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
            name = "New Script";
        }
        return name;
    }

    // Strict UTF-8 check. Rejects NUL (binary data), overlong forms, surrogates and code
    // points past U+10FFFF.
    bool valid_script_utf8(const std::string& s)
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
            if (i + extra >= n && extra > 0)
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
            if (cp < min_cp || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            {
                return false;
            }
            i += extra + 1;
        }
        return true;
    }

    // Read a script file. On failure returns false and sets `reason`.
    bool read_script_text(const std::string& filename, std::string& text, std::string& reason)
    {
        // Read at most twice the limit (CRLF -> LF can shrink a file by up to half); the
        // exact limit is enforced on the converted text below.
        const size_t max_read = kMaxScriptSource * 2 + 3;
        llifstream in(filename.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open())
        {
            reason = LLTrans::getString("ScriptFileUnreadable");
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
            reason = LLTrans::getString("ScriptFileUnreadable");
            return false;
        }
        if (raw.size() > max_read)
        {
            reason = LLTrans::getString("ScriptFileTooLarge");
            return false;
        }

        if (raw.size() >= 3 && (U8)raw[0] == 0xEF && (U8)raw[1] == 0xBB && (U8)raw[2] == 0xBF)
        {
            raw.erase(0, 3);
        }
        if (!valid_script_utf8(raw))
        {
            reason = LLTrans::getString("ScriptFileNotUtf8");
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

        if (text.size() > kMaxScriptSource)
        {
            reason = LLTrans::getString("ScriptFileTooLarge");
            return false;
        }
        if (text.find_first_not_of(" \t\n") == std::string::npos)
        {
            reason = LLTrans::getString("ScriptFileEmpty");
            return false;
        }
        return true;
    }

    void report(const char* notification, const std::string& file, const std::string& detail)
    {
        LLSD args;
        args["FILE"] = file;
        args["REASON"] = detail;
        LLNotificationsUtil::add(notification, args);
    }

    // The compile errors of an upload response, one per line, capped.
    std::string format_compile_errors(const LLSD& errors)
    {
        std::string out;
        size_t shown = 0;
        size_t total = 0;
        for (LLSD::array_const_iterator it = errors.beginArray(); it != errors.endArray(); ++it)
        {
            ++total;
            if (shown >= kMaxErrorLines)
            {
                continue;
            }
            std::string line = it->asString();
            LLStringUtil::stripNonprintable(line);
            if (line.empty())
            {
                continue;
            }
            if (!out.empty())
            {
                out += "\n";
            }
            out += line;
            ++shown;
        }
        if (total > shown)
        {
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", (S32)(total - shown));
            out += "\n" + LLTrans::getString("ScriptFileMoreErrors", args);
        }
        return out;
    }

    // Keeps the new-item auto-open (a script editor window per new script) suppressed in
    // the folder while any of this batch's creates is outstanding. The suppression set in
    // llviewermessage.cpp is not counted, so count here.
    std::map<LLUUID, S32> sPendingCreates;

    void hold_auto_open(const LLUUID& folder_id)
    {
        if (sPendingCreates[folder_id]++ == 0)
        {
            suppress_inventory_auto_open_for_folder(folder_id, true);
        }
    }

    void release_auto_open(const LLUUID& folder_id)
    {
        auto it = sPendingCreates.find(folder_id);
        if (it == sPendingCreates.end())
        {
            return;
        }
        if (--it->second <= 0)
        {
            sPendingCreates.erase(it);
            suppress_inventory_auto_open_for_folder(folder_id, false);
        }
    }

    // Create one script item (never replacing anything), then upload and compile its source.
    void create_script_with_source(const std::string& base, const std::string& name, const std::string& source,
                                   const LLUUID& folder_id)
    {
        // Released exactly once: by the create callback, or by the timer when the sim never
        // answers the create.
        auto released = std::make_shared<bool>(false);
        auto release = [folder_id, released]()
        {
            if (!*released)
            {
                *released = true;
                release_auto_open(folder_id);
            }
        };
        hold_auto_open(folder_id);
        doAfterInterval(release, 60.f);

        LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback(
            [base, source, release](const LLUUID& new_item_id)
            {
                release();

                if (new_item_id.isNull())
                {
                    LL_WARNS() << "Script upload failed for " << base << ": item create refused" << LL_ENDL;
                    report("ScriptFileFailed", base, LLTrans::getString("ScriptFileItemCreateFailed"));
                    return;
                }

                if (LLViewerInventoryItem* item = gInventory.getItem(new_item_id))
                {
                    set_default_permissions(item, "Scripts");
                    gInventory.updateItem(item);
                    gInventory.notifyObservers();
                }

                const std::string url = gAgent.getRegionCapability("UpdateScriptAgent");
                if (url.empty())
                {
                    LL_WARNS() << "Script upload failed for " << base << ": no UpdateScriptAgent capability" << LL_ENDL;
                    report("ScriptFileFailedEmpty", base, LLTrans::getString("ScriptFileNoCap"));
                    return;
                }

                LLResourceUploadInfo::ptr_t info = std::make_shared<LLScriptAssetUpload>(
                    new_item_id, kCompileTarget, source,
                    [base](LLUUID, LLUUID, LLUUID, LLSD response)
                    {
                        if (response["compiled"].asBoolean())
                        {
                            report("ScriptFileCreated", base, std::string());
                        }
                        else
                        {
                            const std::string errors = format_compile_errors(response["errors"]);
                            LL_WARNS() << "Script upload compile failed for " << base << ": " << errors << LL_ENDL;
                            report("ScriptFileCompileFailed", base, errors);
                        }
                    },
                    [base](LLUUID, LLUUID, LLSD, std::string reason)
                    {
                        LL_WARNS() << "Script upload failed for " << base << ": " << reason << LL_ENDL;
                        report("ScriptFileFailedEmpty", base,
                               reason.empty() ? LLTrans::getString("ScriptFileUploadFailed") : reason);
                        return true;   // reported here, no second error dialog
                    });
                LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
            });

        create_inventory_item(
            gAgentID,
            gAgentSessionID,
            folder_id,
            LLTransactionID::tnull,
            name,
            std::string(),
            LLAssetType::AT_LSL_TEXT,
            LLInventoryType::IT_LSL,
            NO_INV_SUBTYPE,
            LLFloaterPerms::getNextOwnerPerms("Scripts"),
            cb);
    }
}

void start_script_file_upload(const std::vector<std::string>& filenames)
{
    if (filenames.empty())
    {
        return;
    }

    if (gDisconnected)
    {
        report("ScriptFileFailed", gDirUtilp->getBaseFileName(filenames.front(), false),
               LLTrans::getString("SoundSplitDisconnected"));
        return;
    }

    const LLUUID folder_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_LSL_TEXT);
    const bool have_cap = !gAgent.getRegionCapability("UpdateScriptAgent").empty();

    for (const std::string& filename : filenames)
    {
        const std::string base = gDirUtilp->getBaseFileName(filename, false);

        if (folder_id.isNull())
        {
            report("ScriptFileFailed", base, LLTrans::getString("ScriptFileNoFolder"));
            continue;
        }
        if (!have_cap)
        {
            // Checked before the create: no empty script item is left behind.
            report("ScriptFileFailed", base, LLTrans::getString("ScriptFileNoCap"));
            continue;
        }

        std::string source;
        std::string reason;
        if (!read_script_text(filename, source, reason))
        {
            LL_WARNS() << "Script upload refused for " << filename << ": " << reason << LL_ENDL;
            report("ScriptFileFailed", base, reason);
            continue;
        }

        create_script_with_source(base, script_name_for(filename), source, folder_id);
    }
}
