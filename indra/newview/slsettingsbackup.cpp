/**
 * @file slsettingsbackup.cpp
 * @brief Settings backup, restore and export for another viewer (#1423). See the header.
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

#include "slsettingsbackup.h"
#include "slsettingsbackupcore.h"
#include "slfloaterviewerimport.h"
#include "slviewerimportcore.h"
#include "fsyspath.h"

#include "llappviewer.h"
#include "llconversationlog.h"
#include "lldirpicker.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
constexpr size_t KEEP_UNDO_BACKUPS = 5;
const char* const UNDO_PREFIX = "before_restore_";

// ---- paths -----------------------------------------------------------------------------------

fs::path userSettingsDir()
{
    return fsyspath(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, ""));
}

fs::path appDir()
{
    return fsyspath(gDirUtilp->getOSUserAppDir());
}

bool loggedIn()
{
    return !gDirUtilp->getLindenUserDir().empty();
}

// The logged-in account's folder name, if it is one a restore may write to.
std::string currentAccount()
{
    const std::string name = SLFloaterViewerImport::currentAccountName();
    return slbackup::isSafeAccountName(name) ? name : std::string();
}

std::string stamp(const char* fmt)
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if LL_WINDOWS
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[40];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

std::string toXml(const LLSD& sd)
{
    std::ostringstream out;
    LLSDSerialize::toPrettyXML(sd, out);
    return out.str();
}

bool writeText(const fs::path& path, const std::string& text)
{
    fs::path tmp = path;
    tmp += ".slipstream.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool readSettings(const fs::path& path, LLSD& out)
{
    return LLControlGroup::readSettingsFile(slbackup::utf8(path), out);
}

// ---- live settings ---------------------------------------------------------------------------

// What the group would save to its file, minus secrets: the same entries saveToFile writes.
LLSD savedSettings(LLControlGroup& group)
{
    struct Collect : public LLControlGroup::ApplyFunctor
    {
        LLSD out = LLSD::emptyMap();
        void apply(const std::string& name, LLControlVariable* c) override
        {
            if (!c || !c->isPersisted() || !c->shouldSave(true) || slbackup::isSecretName(name)) return;
            out[name]["Type"] = LLControlGroup::typeEnumToString(c->type());
            out[name]["Comment"] = c->getComment();
            out[name]["Value"] = c->getSaveValue();
        }
    } collect;
    group.applyToAll(&collect);
    return collect.out;
}

struct FilterCount
{
    int kept = 0;
    int gone = 0;           // no longer exists
    int not_saved = 0;      // not persisted (Persist 0) or a secret
    int type_changed = 0;
};

// Keep only what this build declares, saves and reads as the same type; rewrite in place.
bool filterAgainst(LLControlGroup& group, const fs::path& file, FilterCount& count)
{
    LLSD in;
    if (!readSettings(file, in)) return false;
    LLSD out = LLSD::emptyMap();
    for (LLSD::map_const_iterator it = in.beginMap(); it != in.endMap(); ++it)
    {
        const std::string& name = it->first;
        LLControlVariable* c = group.getControl(name);
        if (!c)
        {
            ++count.gone;
            continue;
        }
        if (!c->isPersisted() || slbackup::isSecretName(name))
        {
            ++count.not_saved;
            continue;
        }
        const LLSD& entry = it->second;
        if (!entry.isMap() || !entry.has("Value")
            || (entry.has("Type") && entry["Type"].asString() != LLControlGroup::typeEnumToString(c->type())))
        {
            ++count.type_changed;
            continue;
        }
        out[name] = entry;
        ++count.kept;
    }
    return writeText(file, toXml(out));
}

// ---- backup ----------------------------------------------------------------------------------

bool makeBackup(const fs::path& zip, bool history, std::string& error, size_t& files)
{
    slbackup::Sources src;
    src.user_settings = userSettingsDir();
    const bool account = loggedIn();
    if (account)
    {
        src.account_dir = fsyspath(gDirUtilp->getLindenUserDir());
        src.chat_dir = fsyspath(gDirUtilp->getPerAccountChatLogsDir());
    }
    src.include_history = history;
    std::vector<std::string> skipped;
    std::vector<slbackup::Entry> entries = slbackup::collectFiles(src, &skipped);

    slbackup::Entry global;
    global.path = "user_settings/settings.xml";
    global.data = toXml(savedSettings(gSavedSettings));
    global.from_memory = true;
    entries.push_back(global);
    if (account)
    {
        slbackup::Entry per;
        per.path = "account/settings_per_account.xml";
        per.data = toXml(savedSettings(gSavedPerAccountSettings));
        per.from_memory = true;
        entries.push_back(per);
    }

    slbackup::Manifest m;
    m.created = stamp("%Y-%m-%d %H:%M:%S");
    m.viewer = LLVersionInfo::instance().getChannelAndVersion();
    m.account = account ? currentAccount() : std::string();
    m.history = history;
    std::vector<std::string> refused;
    const bool ok = slbackup::writeBackup(zip, m, entries, error, &refused);
    for (const std::string& s : skipped) LL_INFOS("SettingsBackup") << "left out: " << s << LL_ENDL;
    for (const std::string& s : refused) LL_INFOS("SettingsBackup") << "refused: " << s << LL_ENDL;
    files = entries.size() - refused.size();
    LL_INFOS("SettingsBackup") << (ok ? "wrote " : "failed to write ") << slbackup::utf8(zip) << " (" << files
                               << " files, history " << (history ? "included" : "not included") << ")"
                               << (ok ? std::string() : ": " + error) << LL_ENDL;
    return ok;
}

void onBackupPicked(const std::vector<std::string>& filenames, bool history)
{
    if (filenames.empty() || filenames[0].empty()) return;
    fs::path zip = fsyspath(filenames[0]);
    std::string ext = slbackup::utf8(zip.extension());
    LLStringUtil::toLower(ext);
    if (ext != ".zip") zip += ".zip";
    std::string error;
    size_t files = 0;
    LLSD args;
    args["PATH"] = slbackup::utf8(zip);
    if (makeBackup(zip, history, error, files))
    {
        args["COUNT"] = (LLSD::Integer)files;
        LLNotificationsUtil::add("SettingsBackupDone", args);
    }
    else
    {
        args["REASON"] = error;
        LLNotificationsUtil::add("SettingsBackupFailed", args);
    }
}

// ---- restore ---------------------------------------------------------------------------------

struct Preview
{
    fs::path    staging;
    std::string source;
    std::string account;        // where per-account files go (empty: nowhere)
    bool        has_chat = false;
};
Preview sPreview;

fs::path restoreBase()
{
    return userSettingsDir() / slbackup::RESTORE_DIR;
}

void discardPreview()
{
    std::error_code ec;
    if (!sPreview.staging.empty()) fs::remove_all(sPreview.staging, ec);
    sPreview = Preview();
}

std::string describe(const slbackup::ReadResult& r, const FilterCount& global, const FilterCount& per,
                     bool has_global, bool has_per, const std::string& account)
{
    int presets = 0, poses = 0, windlight = 0, chat = 0;
    std::vector<std::string> singles;
    bool teleports = false;
    for (const std::string& f : r.files)
    {
        if (f.rfind("user_settings/presets/", 0) == 0) ++presets;
        else if (f.rfind("user_settings/poses/", 0) == 0) ++poses;
        else if (f.rfind("user_settings/windlight/", 0) == 0) ++windlight;
        else if (f.rfind("chat_logs/", 0) == 0) ++chat;
        else if (f == "account/teleport_history.txt") teleports = true;
        else if (f != "user_settings/settings.xml" && f != "account/settings_per_account.xml")
        {
            singles.push_back(f.substr(f.find('/') + 1));
        }
    }
    std::ostringstream out;
    out << "Backup of " << (r.manifest.created.empty() ? std::string("unknown date") : r.manifest.created);
    if (!r.manifest.viewer.empty()) out << " (" << r.manifest.viewer << ")";
    out << "\n\n";
    if (has_global)
    {
        out << "Settings: " << global.kept << " restored";
        const int skipped = global.gone + global.not_saved + global.type_changed;
        if (skipped) out << ", " << skipped << " skipped (no longer exist or not kept)";
        out << "\n";
    }
    if (has_per)
    {
        if (account.empty())
        {
            out << "Per-account settings: not restored (log in first)\n";
        }
        else
        {
            out << "Per-account settings for " << account << ": " << per.kept << " restored";
            const int skipped = per.gone + per.not_saved + per.type_changed;
            if (skipped) out << ", " << skipped << " skipped";
            out << "\n";
        }
    }
    if (!singles.empty())
    {
        out << "Files: ";
        for (size_t i = 0; i < singles.size(); ++i) out << (i ? ", " : "") << singles[i];
        out << "\n";
    }
    if (presets) out << "Presets: " << presets << "\n";
    if (poses) out << "Poses: " << poses << "\n";
    if (windlight) out << "Windlight presets: " << windlight << "\n";
    if (teleports && !account.empty()) out << "Teleport history\n";
    if (chat)
    {
        out << "Chat and IM transcripts: " << chat
            << (account.empty() ? " (not restored: log in first)" : " (merged into yours; nothing is deleted)") << "\n";
    }
    if (!r.ignored.empty()) out << r.ignored.size() << " other file(s) in the archive are ignored (see the log)\n";
    return out.str();
}

void onRestoreConfirmed(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0 || sPreview.staging.empty())
    {
        discardPreview();
        return;
    }
    std::error_code ec;
    LLSD args;

    // 1. A backup of what is here now, so the restore can be undone.
    const fs::path undo_dir = userSettingsDir() / "settings_backups";
    fs::create_directories(undo_dir, ec);
    const fs::path undo = undo_dir / (std::string(UNDO_PREFIX) + stamp("%Y%m%d-%H%M%S") + ".zip");
    std::string error;
    size_t files = 0;
    if (!makeBackup(undo, false, error, files))
    {
        args["REASON"] = "could not back up the current settings first (" + error + "); nothing was changed";
        LLNotificationsUtil::add("SettingsRestoreFailed", args);
        discardPreview();
        return;
    }
    slbackup::pruneBackups(undo_dir, UNDO_PREFIX, KEEP_UNDO_BACKUPS);

    // 2. Transcripts are merged now (never overwritten or deleted; a changed file keeps a
    //    .pre-import copy), with the importer's tested merge.
    const fs::path chat_src = sPreview.staging / "chat_logs";
    std::string chat_note;
    if (fs::is_directory(chat_src, ec) && !sPreview.account.empty())
    {
        fs::path chat_dst;
        const bool live = loggedIn() && currentAccount() == sPreview.account;
        if (live)
        {
            chat_dst = fsyspath(gDirUtilp->getPerAccountChatLogsDir());
            // The conversation list is rewritten from memory at logout: save it now, merge on
            // disk, read it back (as the viewer importer does) so the merge survives the restart.
            LLConversationLog::instance().cache();
        }
        else
        {
            const std::string log_path = slimport::readInstantMessageLogPath(appDir() / sPreview.account / "settings_per_account.xml");
            chat_dst = (log_path.empty() ? appDir() : fsyspath(log_path)) / sPreview.account;
        }
        fs::create_directories(chat_dst, ec);
        const slimport::Stats st = slimport::importChatLogs(chat_src, chat_dst, false, false);
        if (live) LLConversationLog::instance().reloadFromDisk();
        chat_note = llformat("\nTranscripts: %d added, %d merged, %d unchanged.", st.copied, st.merged, st.same);
        for (const std::string& l : st.log) LL_INFOS("SettingsBackup") << "chat: " << l << LL_ENDL;
    }
    fs::remove_all(chat_src, ec);

    // 3. Everything else waits for the next start (the running viewer saves its settings at exit).
    const fs::path pending = restoreBase() / "pending";
    fs::remove_all(pending, ec);
    fs::rename(sPreview.staging, pending, ec);
    if (ec || !slbackup::writePendingMarker(pending, sPreview.account, sPreview.source, error))
    {
        fs::remove_all(pending, ec);
        args["REASON"] = error.empty() ? std::string("could not stage the restore") : error;
        LLNotificationsUtil::add("SettingsRestoreFailed", args);
        discardPreview();
        return;
    }
    LL_INFOS("SettingsBackup") << "restore of " << sPreview.source << " staged for the next start; undo backup "
                               << slbackup::utf8(undo) << LL_ENDL;
    sPreview = Preview();
    args["UNDO"] = slbackup::utf8(undo);
    args["CHAT"] = chat_note;
    LLNotificationsUtil::add("SettingsRestoreRestart", args, LLSD(), [](const LLSD& n, const LLSD& r)
    {
        if (LLNotificationsUtil::getSelectedOption(n, r) == 0) LLAppViewer::instance()->requestQuit();
    });
}

void onRestorePicked(const std::vector<std::string>& filenames)
{
    if (filenames.empty() || filenames[0].empty()) return;
    discardPreview();
    const fs::path zip = fsyspath(filenames[0]);
    sPreview.staging = restoreBase() / "staging";
    sPreview.source = slbackup::utf8(zip.filename());
    std::error_code ec;
    fs::remove_all(sPreview.staging, ec);

    slbackup::ReadResult r;
    std::string error;
    LLSD args;
    if (!slbackup::readBackup(zip, sPreview.staging, slbackup::Limits(), r, error))
    {
        LL_WARNS("SettingsBackup") << "rejected " << slbackup::utf8(zip) << ": " << error << LL_ENDL;
        args["REASON"] = error;
        LLNotificationsUtil::add("SettingsRestoreFailed", args);
        discardPreview();
        return;
    }
    for (const std::string& s : r.ignored) LL_INFOS("SettingsBackup") << "ignored in backup: " << s << LL_ENDL;

    sPreview.account = loggedIn() ? currentAccount() : r.manifest.account;
    FilterCount global, per;
    const fs::path g = sPreview.staging / "user_settings" / "settings.xml";
    const fs::path a = sPreview.staging / "account" / "settings_per_account.xml";
    const bool has_global = fs::exists(g, ec) && filterAgainst(gSavedSettings, g, global);
    const bool has_per = fs::exists(a, ec) && filterAgainst(gSavedPerAccountSettings, a, per);
    if (sPreview.account.empty()) fs::remove_all(sPreview.staging / "account", ec);

    args["SUMMARY"] = describe(r, global, per, has_global, has_per, sPreview.account);
    LLNotificationsUtil::add("SettingsRestoreConfirm", args, LLSD(), &onRestoreConfirmed);
}

// ---- export ----------------------------------------------------------------------------------

struct ExportTarget
{
    std::string id;         // "firestorm", "secondlife" (settings_export_map.xml, viewer importer ids)
    std::string label;
};

const LLSD& exportMap()
{
    static LLSD map;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "settings_export_map.xml");
        llifstream in(path.c_str(), std::ios::binary);
        if (!in.is_open() || LLSDSerialize::fromXML(map, in) <= 0 || !map.isMap())
        {
            LL_WARNS("SettingsBackup") << "could not read " << path << LL_ENDL;
            map = LLSD::emptyMap();
        }
    }
    return map;
}

// The settings to write for a viewer: shared ones whose value you changed here.
LLSD exportSettings(const std::string& target, const char* group_key, LLControlGroup& group)
{
    LLSD out = LLSD::emptyMap();
    for (const LLSD& n : llsd::inArray(exportMap()[target][group_key]))
    {
        const std::string name = n.asString();
        LLControlVariable* c = group.getControl(name);
        if (!c || !c->isPersisted() || slbackup::isSecretName(name)) continue;
        if (llsd_equals(c->getSaveValue(), c->getDefault())) continue;
        out[name]["Type"] = LLControlGroup::typeEnumToString(c->type());
        out[name]["Comment"] = c->getComment();
        out[name]["Persist"] = 1;
        out[name]["Value"] = c->getSaveValue();
    }
    return out;
}

// Update their file: only our keys change, everything else stays. A backup copy comes first.
bool mergeFile(const fs::path& file, const LLSD& ours, std::string& backup, std::string& error)
{
    std::error_code ec;
    LLSD theirs = LLSD::emptyMap();
    if (fs::exists(file, ec))
    {
        if (!readSettings(file, theirs))
        {
            error = slbackup::utf8(file) + " could not be read; it was left alone";
            return false;
        }
        fs::path bak = file;
        bak += ".slipstream-" + stamp("%Y%m%d-%H%M%S") + ".bak";
        fs::copy_file(file, bak, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = "could not make the backup copy " + slbackup::utf8(bak) + "; nothing was changed";
            return false;
        }
        backup = slbackup::utf8(bak);
    }
    for (LLSD::map_const_iterator it = ours.beginMap(); it != ours.endMap(); ++it)
    {
        LLSD& entry = theirs[it->first];
        if (entry.isMap())
        {
            entry["Type"] = it->second["Type"];
            entry["Value"] = it->second["Value"];
        }
        else
        {
            entry = it->second;
        }
    }
    if (!writeText(file, toXml(theirs)))
    {
        error = "could not write " + slbackup::utf8(file);
        return false;
    }
    return true;
}

void mergeInto(const ExportTarget& target, const fs::path& base)
{
    const LLSD global = exportSettings(target.id, "global", gSavedSettings);
    const LLSD per = loggedIn() ? exportSettings(target.id, "account", gSavedPerAccountSettings) : LLSD::emptyMap();
    std::vector<std::pair<fs::path, const LLSD*>> jobs;
    if (global.size()) jobs.emplace_back(base / "user_settings" / "settings.xml", &global);
    std::error_code ec;
    const std::string account = currentAccount();
    std::string per_note;
    if (per.size())
    {
        // Their folder for the same account must exist (they logged in there at least once).
        if (!account.empty() && fs::is_directory(base / account, ec))
        {
            jobs.emplace_back(base / account / "settings_per_account.xml", &per);
        }
        else
        {
            per_note = "\nPer-account settings were not written: " + target.label
                       + " has no folder for this account yet (log in with it there once).";
        }
    }
    std::string files, baks, error;
    size_t count = 0;
    for (const auto& job : jobs)
    {
        std::string bak;
        if (!mergeFile(job.first, *job.second, bak, error))
        {
            LLSD args;
            args["REASON"] = error + (files.empty() ? std::string() : "\nAlready written:" + files + "\nBackups:" + baks);
            LLNotificationsUtil::add("SettingsExportFailed", args);
            return;
        }
        count += job.second->size();
        files += "\n" + slbackup::utf8(job.first);
        if (!bak.empty()) baks += "\n" + bak;
    }
    LLSD args;
    args["VIEWER"] = target.label;
    args["COUNT"] = (LLSD::Integer)count;
    args["FILES"] = files + per_note;
    args["BACKUPS"] = baks.empty() ? std::string("\n(none: there was no file yet)") : baks;
    LL_INFOS("SettingsBackup") << "exported " << count << " settings to " << target.label << ":" << files << LL_ENDL;
    LLNotificationsUtil::add("SettingsExportDone", args);
}

void saveExportFile(const ExportTarget& target, const std::vector<std::string>& filenames)
{
    if (filenames.empty() || filenames[0].empty()) return;
    const fs::path file = fsyspath(filenames[0]);
    const LLSD global = exportSettings(target.id, "global", gSavedSettings);
    const LLSD per = loggedIn() ? exportSettings(target.id, "account", gSavedPerAccountSettings) : LLSD::emptyMap();
    LLSD args;
    args["VIEWER"] = target.label;
    args["FILE"] = slbackup::utf8(file);
    std::string per_note;
    bool ok = writeText(file, toXml(global));
    if (ok && per.size())
    {
        fs::path per_file = file.parent_path() / (slbackup::utf8(file.stem()) + "_per_account.xml");
        std::error_code ec;
        if (fs::exists(per_file, ec))
        {
            per_file = file.parent_path() / (slbackup::utf8(file.stem()) + "_per_account_" + stamp("%Y%m%d-%H%M%S") + ".xml");
        }
        ok = writeText(per_file, toXml(per));
        per_note = "\nPer-account settings (" + std::to_string(per.size()) + "): " + slbackup::utf8(per_file)
                   + " (for the account folder, as settings_per_account.xml)";
    }
    if (!ok)
    {
        args["REASON"] = "could not write " + slbackup::utf8(file);
        LLNotificationsUtil::add("SettingsExportFailed", args);
        return;
    }
    args["COUNT"] = (LLSD::Integer)global.size();
    args["PERACCOUNT"] = per_note;
    LLNotificationsUtil::add("SettingsExportSaved", args);
}

void pickExportFile(const ExportTarget& target)
{
    LLFilePickerReplyThread::startPicker(
        [target](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            saveExportFile(target, filenames);
        },
        LLFilePicker::FFSAVE_XML, "settings_for_" + target.id + ".xml");
}

void pickExportFolder(const ExportTarget& target);

void confirmMerge(const ExportTarget& target, const fs::path& base)
{
    const size_t count = exportSettings(target.id, "global", gSavedSettings).size()
        + (loggedIn() ? exportSettings(target.id, "account", gSavedPerAccountSettings).size() : 0);
    LLSD args;
    args["VIEWER"] = target.label;
    args["PATH"] = slbackup::utf8(base);
    args["COUNT"] = (LLSD::Integer)count;
    LLNotificationsUtil::add("SettingsExportConfirm", args, LLSD(), [target, base](const LLSD& n, const LLSD& r)
    {
        switch (LLNotificationsUtil::getSelectedOption(n, r))
        {
            case 0: mergeInto(target, base); break;
            case 1: pickExportFolder(target); break;
            case 2: pickExportFile(target); break;
            default: break;
        }
    });
}

void onExportFolderPicked(const ExportTarget& target, const std::vector<std::string>& filenames)
{
    if (filenames.empty() || filenames[0].empty()) return;
    fs::path base = fsyspath(filenames[0]);
    std::error_code ec;
    std::string leaf = slbackup::utf8(base.filename());
    LLStringUtil::toLower(leaf);
    if (leaf.empty())
    {
        base = base.parent_path();
        leaf = slbackup::utf8(base.filename());
        LLStringUtil::toLower(leaf);
    }
    if (leaf == "user_settings") base = base.parent_path();
    LLSD args;
    args["VIEWER"] = target.label;
    args["PATH"] = slbackup::utf8(base);
    if (!fs::is_directory(base / "user_settings", ec))
    {
        LLNotificationsUtil::add("SettingsExportBadFolder", args);
        return;
    }
    if (fs::equivalent(base, appDir(), ec))
    {
        args["REASON"] = "that is Slipstream's own settings folder";
        LLNotificationsUtil::add("SettingsExportFailed", args);
        return;
    }
    confirmMerge(target, base);
}

void pickExportFolder(const ExportTarget& target)
{
    (new LLDirPickerThread([target](const std::vector<std::string>& filenames, std::string)
                           {
                               onExportFolderPicked(target, filenames);
                           },
                           std::string()))->getFile();
}

void exportTo(const ExportTarget& target)
{
    const size_t count = exportSettings(target.id, "global", gSavedSettings).size()
        + (loggedIn() ? exportSettings(target.id, "account", gSavedPerAccountSettings).size() : 0);
    LLSD args;
    args["VIEWER"] = target.label;
    if (!count)
    {
        LLNotificationsUtil::add("SettingsExportNothing", args);
        return;
    }
    for (const slimport::SourceViewer& v : SLFloaterViewerImport::findInstalledViewers())
    {
        if (v.id == target.id)
        {
            confirmMerge(target, v.dir);
            return;
        }
    }
    LLNotificationsUtil::add("SettingsExportNoFolder", args, LLSD(), [target](const LLSD& n, const LLSD& r)
    {
        switch (LLNotificationsUtil::getSelectedOption(n, r))
        {
            case 0: pickExportFolder(target); break;
            case 1: pickExportFile(target); break;
            default: break;
        }
    });
}
}

namespace SLSettingsBackup
{
void backUp()
{
    LLNotificationsUtil::add("SettingsBackupAsk", LLSD(), LLSD(), [](const LLSD& n, const LLSD& r)
    {
        const S32 option = LLNotificationsUtil::getSelectedOption(n, r);
        if (option != 0 && option != 1) return;
        const bool history = option == 1;
        LLFilePickerReplyThread::startPicker(
            [history](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
            {
                onBackupPicked(filenames, history);
            },
            LLFilePicker::FFSAVE_ALL, "slipstream_settings_" + stamp("%Y-%m-%d") + ".zip");
    });
}

void restore()
{
    LLFilePickerReplyThread::startPicker(
        [](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            onRestorePicked(filenames);
        },
        LLFilePicker::FFLOAD_ZIP, false);
}

void exportForViewer()
{
    LLNotificationsUtil::add("SettingsExportPickViewer", LLSD(), LLSD(), [](const LLSD& n, const LLSD& r)
    {
        switch (LLNotificationsUtil::getSelectedOption(n, r))
        {
            case 0: exportTo({ "firestorm", "Firestorm" }); break;
            case 1: exportTo({ "secondlife", "Second Life" }); break;
            default: break;
        }
    });
}

void applyPendingRestore()
{
    const slbackup::ApplyResult res = slbackup::applyPendingRestore(userSettingsDir(), appDir());
    for (const std::string& line : res.log) LL_INFOS("SettingsBackup") << line << LL_ENDL;
}
}
