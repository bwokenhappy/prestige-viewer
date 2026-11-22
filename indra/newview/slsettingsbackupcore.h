/**
 * @file slsettingsbackupcore.h
 * @brief Settings backup and restore (#1423): what a backup holds, the archive format, its
 *        validation, the "secrets never included" rule and applying a staged restore at
 *        startup. Plain C++17 plus expat / minizip-ng (no viewer types) so the rules can be
 *        tested on their own (scripts/check/test_settings_backup.cpp); slsettingsbackup.cpp
 *        drives it from Preferences.
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

#ifndef SL_SETTINGSBACKUPCORE_H
#define SL_SETTINGSBACKUPCORE_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace slbackup
{
namespace fs = std::filesystem;

// The archive is a zip. Its first entry is MANIFEST_NAME (key=value lines):
//   format=slipstream-settings-backup
//   version=1
//   created=2026-11-19 14:02:11
//   viewer=Slipstream 1.5.0.1234
//   account=john_smith            (per-account folder name; empty when none was included)
//   history=0|1                   (chat / IM transcripts and teleport history included)
// Every other entry sits under one of three roots:
//   user_settings/...   files of the global user_settings folder
//   account/...         files of the per-account folder
//   chat_logs/...       chat and IM transcripts (only with history=1)
// and must be on the allowlist (isAllowedPath); anything else is never written anywhere.
constexpr const char* FORMAT_ID      = "slipstream-settings-backup";
constexpr int         FORMAT_VERSION = 1;
constexpr const char* MANIFEST_NAME  = "slipstream_backup.txt";

struct Manifest
{
    int         version = FORMAT_VERSION;
    std::string created;
    std::string viewer;
    std::string account;
    bool        history = false;
};

struct Limits
{
    size_t   max_entries = 100000;                     // chat logs can be many small files
    uint64_t max_total   = 4ull * 1024 * 1024 * 1024;  // bytes unpacked from one archive
    uint64_t max_file    = 512ull * 1024 * 1024;       // bytes in one file
    size_t   max_path    = 240;                        // bytes in one entry path
};

// ---- rules -----------------------------------------------------------------------------------

// A name (file name or setting name) that may hold a credential or secret: passwords, login
// info, MFA hash, API keys, tokens. Case-insensitive. Such files are never put in a backup or
// written from one; such settings are stripped from every settings file going in or out.
bool isSecretName(const std::string& name);

// Structural safety of an archive entry name: relative, '/'-separated, no "." / ".." / empty
// components, no backslash, drive letter, colon or control character. False (with why) means
// the archive is hostile or broken and is rejected as a whole.
bool isSafePath(const std::string& path, std::string& why);

// Whether a (safe) entry path is one a backup may contain: under a known root, on that root's
// allowlist, not a secret. False (with why) means the entry is ignored.
bool isAllowedPath(const std::string& path, std::string& why);

// The per-account folder name a backup or restore may name: one path component, lower-case
// letters / digits / '_' / '.' / '-', not one of the viewer's own top-level folders.
bool isSafeAccountName(const std::string& name);

// Remove the top-level entries of an LLSD settings file (<llsd><map><key>Name</key>...) whose
// name keep() refuses; everything else is copied byte for byte. False if the text is not an
// LLSD map (error says why). dropped: the names removed.
bool filterSettingsXml(const std::string& in, const std::function<bool(const std::string&)>& keep,
                       std::string& out, std::vector<std::string>* dropped, std::string& error);

// ---- writing ---------------------------------------------------------------------------------

struct Entry
{
    std::string path;           // archive path, e.g. "user_settings/presets/graphic/Fast.xml"
    fs::path    source;         // read from here when data is empty and from_memory is false
    std::string data;           // or this content
    bool        from_memory = false;
};

struct Sources
{
    fs::path user_settings;     // ~/.slipstreamviewer/user_settings
    fs::path account_dir;       // ~/.slipstreamviewer/john_smith (empty: not logged in)
    fs::path chat_dir;          // where this account's transcripts live (InstantMessageLogPath)
    bool     include_history = false;
};

// The files a backup takes from disk, by allowlist (never by walking everything). The two
// settings files (settings.xml, settings_per_account.xml) are not collected: the viewer adds
// them from its live settings. skipped: files near the allowlist that were left out, and why.
std::vector<Entry> collectFiles(const Sources& sources, std::vector<std::string>* skipped = nullptr);

// Write the archive (via "<zip>.partial", renamed into place). Every entry must pass
// isSafePath + isAllowedPath; settings files have secret settings stripped. refused: entries
// left out, with why. False with error when nothing could be written.
bool writeBackup(const fs::path& zip, const Manifest& manifest, const std::vector<Entry>& entries,
                 std::string& error, std::vector<std::string>* refused = nullptr);

// ---- reading ---------------------------------------------------------------------------------

struct ReadResult
{
    Manifest                 manifest;
    std::vector<std::string> files;     // extracted, archive paths
    std::vector<std::string> ignored;   // "<path>: <why>" for entries not on the allowlist
};

// Validate and unpack into dest (which must not exist yet or be empty). Rejected outright:
// not a zip, no manifest, another format, a newer version, an unsafe path, a symlink, an
// encrypted or oversized entry, a duplicate. Entries not on the allowlist are ignored and
// listed. Settings files are stripped of secret settings as they are unpacked.
bool readBackup(const fs::path& zip, const fs::path& dest, const Limits& limits, ReadResult& result,
                std::string& error);

std::string formatManifest(const Manifest& manifest);
bool        parseManifest(const std::string& text, Manifest& manifest, std::string& error);

// ---- staged restore ------------------------------------------------------------------------

// A restore is staged in <user_settings>/settings_restore/pending (user_settings/ and account/
// subfolders plus PENDING_MARKER naming the target account) and applied by applyPendingRestore
// at the next start, before any settings file is read, so nothing the running viewer saves at
// exit can overwrite it.
constexpr const char* RESTORE_DIR    = "settings_restore";
constexpr const char* PENDING_MARKER = "restore.txt";

// Mark a staged folder as complete (it is only applied with the marker).
bool writePendingMarker(const fs::path& pending, const std::string& account, const std::string& source,
                        std::string& error);

struct ApplyResult
{
    bool                     applied = false;
    int                      files = 0;
    std::vector<std::string> log;
};

// Apply <user_settings>/settings_restore/pending, if it is complete, into user_settings and
// <app_dir>/<account>; then remove it. An incomplete (unmarked) folder is removed unapplied.
// Every path is checked again; only allowlisted files are written (temp file + rename).
ApplyResult applyPendingRestore(const fs::path& user_settings, const fs::path& app_dir);

// Keep the newest `keep` files named prefix*.zip in dir (the automatic before-restore backups).
void pruneBackups(const fs::path& dir, const std::string& prefix, size_t keep);

std::string utf8(const fs::path& p);
fs::path    fromUtf8(const std::string& s);
}

#endif
