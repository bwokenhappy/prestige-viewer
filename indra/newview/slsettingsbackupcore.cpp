/**
 * @file slsettingsbackupcore.cpp
 * @brief Settings backup and restore (#1423). See the header.
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

#include "slsettingsbackupcore.h"

#ifndef XML_STATIC
#define XML_STATIC
#endif
#if __has_include("expat/expat.h")
#include "expat/expat.h"
#else
#include <expat.h>
#endif
#include <minizip-ng/unzip.h>
#include <minizip-ng/zip.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace slbackup
{
namespace
{
constexpr uint64_t MAX_MANIFEST_BYTES = 64 * 1024;
constexpr uint64_t MAX_SETTINGS_BYTES = 64ull * 1024 * 1024;   // a settings file we filter in memory
constexpr size_t   MAX_COLLECT        = 100000;                // files looked at per folder walk
constexpr int      ZIP_DEFLATE        = 8;                     // Z_DEFLATED
constexpr int      ZIP_LEVEL          = 6;

std::string lower(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return s;
}

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return s.substr(b, e - b);
}

bool endsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> splitPath(const std::string& path)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        const size_t slash = path.find('/', start);
        parts.push_back(path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return parts;
}

std::string extensionOf(const std::string& name)
{
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot));
}

// ---- the allowlist ---------------------------------------------------------------------------
// One table: what a backup takes, and the only paths a restore writes. Each row says why.

struct FileRule
{
    const char* name;       // file name (lower case)
    const char* why;
};

// user_settings/<name>
const FileRule USER_FILES[] = {
    { "settings.xml",             "global settings (secret settings stripped)" },
    { "colors.xml",               "saved colours" },
    { "key_bindings.xml",         "key bindings" },
    { "ignorable_dialogs.xml",    "\"don't show this again\" choices" },
    { "autoreplace.xml",          "auto-replace lists" },
    { "fx_state.xml",             "post-processing effect stack" },
    { "pose_studio_sequence.xml", "Pose Studio sequence" },
};

// user_settings/<dir>/... : depth counts the components below user_settings (file included).
struct DirRule
{
    const char* dir;
    size_t      min_depth;
    size_t      max_depth;
    const char* exts;       // space-separated, lower case
    const char* why;
};

const DirRule USER_DIRS[] = {
    { "presets",      3, 3, ".xml",      "graphics, camera and look presets (presets/<kind>/<name>.xml)" },
    { "windlight",    3, 3, ".xml",      "saved sky, water and day presets" },
    { "poses",        2, 5, ".xml .bvh", "saved poses" },
    { "dictionaries", 2, 2, ".dic",      "your spelling words" },
};

// account/<name>
const FileRule ACCOUNT_FILES[] = {
    { "settings_per_account.xml",    "per-account settings, incl. keyword alerts (secret settings stripped)" },
    { "toolbars.xml",                "toolbar layout" },
    { "gmdx_toolbar_pins.xml",       "toolbar pins" },
    { "settings_friends_groups.xml", "contact sets" },
    { "highlights.xml",              "chat highlights" },
    { "gesture_sets.xml",            "gesture sets" },
    { "render_mute_settings.txt",    "always / never render choices per avatar" },
    { "sound_emitter_blacklist.xml", "blocked sound sources" },
    { "minimap_marks.json",          "mini-map marks" },
};

// account/<name>, only in a backup with history
const FileRule ACCOUNT_HISTORY_FILES[] = {
    { "teleport_history.txt", "teleport history" },
};

// .txt files in the account folder that are not transcripts (never collected as chat logs).
const char* const NOT_TRANSCRIPTS[] = { "teleport_history.txt", "typed_locations.txt", "render_mute_settings.txt" };

bool inTable(const FileRule* begin, const FileRule* end, const std::string& name)
{
    const std::string low = lower(name);
    for (const FileRule* r = begin; r != end; ++r)
    {
        if (low == r->name) return true;
    }
    return false;
}

bool extAllowed(const char* exts, const std::string& name)
{
    const std::string ext = extensionOf(name);
    if (ext.empty()) return false;
    std::istringstream in(exts);
    std::string e;
    while (in >> e)
    {
        if (e == ext) return true;
    }
    return false;
}

bool isSettingsFile(const std::string& path)
{
    const std::string low = lower(path);
    return low == "user_settings/settings.xml" || low == "account/settings_per_account.xml";
}

bool keepSetting(const std::string& name)
{
    return !isSecretName(name);
}

bool readFile(const fs::path& path, std::string& out, uint64_t max_bytes)
{
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > max_bytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(size_t(size), '\0');
    if (size && !in.read(&out[0], std::streamsize(size))) return false;
    return true;
}

bool writeFileAtomic(const fs::path& path, const std::string& data)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".restore.tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), std::streamsize(data.size()));
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool copyFileAtomic(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::path tmp = to;
    tmp += ".restore.tmp";
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::rename(tmp, to, ec);
    if (ec)
    {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string nowString()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}
}

std::string utf8(const fs::path& p)
{
#if defined(__cpp_char8_t)
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.u8string();
#endif
}

fs::path fromUtf8(const std::string& s)
{
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

// ---- rules -----------------------------------------------------------------------------------

bool isSecretName(const std::string& name)
{
    // Substrings, not words: "RememberPassword", "GoogleTranslateAPIKey", "UserLoginInfo",
    // "bin_conf.dat". "mfa" alone would also hit "AvatarAnimFarM", so the MFA hash is named.
    static const char* const NEEDLES[] = {
        "password", "passwd", "token", "secret", "credential", "oauth", "apikey", "api_key",
        "logininfo", "login_info", "mfahash", "mfa_hash", "privatekey", "private_key", "bin_conf",
    };
    const std::string low = lower(name);
    for (const char* n : NEEDLES)
    {
        if (low.find(n) != std::string::npos) return true;
    }
    // The secure store's files (bin_conf.dat, password.dat, ...).
    return endsWith(low, ".dat");
}

bool isSafePath(const std::string& path, std::string& why)
{
    if (path.empty())
    {
        why = "empty name";
        return false;
    }
    if (path.size() > 1024)
    {
        why = "name too long";
        return false;
    }
    for (unsigned char c : path)
    {
        if (c < 0x20 || c == 0x7f)
        {
            why = "control character in the name";
            return false;
        }
        if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        {
            why = "character not allowed in a file name";
            return false;
        }
    }
    if (path.front() == '/')
    {
        why = "absolute path";
        return false;
    }
    for (const std::string& part : splitPath(path))
    {
        if (part.empty() || part == "." || part == "..")
        {
            why = "path leaves its folder (\"" + part + "\" component)";
            return false;
        }
        if (part.back() == '.' || part.back() == ' ')
        {
            why = "name ends in a dot or space";
            return false;
        }
        // Windows device names (CON, NUL, COM1, ...), with or without an extension.
        const std::string stem = lower(part.substr(0, part.find('.')));
        static const char* const DEVICES[] = { "con", "prn", "aux", "nul" };
        bool device = std::find_if(std::begin(DEVICES), std::end(DEVICES),
                                   [&](const char* d) { return stem == d; }) != std::end(DEVICES);
        if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) && stem[3] >= '0' && stem[3] <= '9')
        {
            device = true;
        }
        if (device)
        {
            why = "reserved device name";
            return false;
        }
    }
    return true;
}

bool isAllowedPath(const std::string& path, std::string& why)
{
    if (!isSafePath(path, why)) return false;
    const std::vector<std::string> parts = splitPath(path);
    const std::string& file = parts.back();
    // Transcripts are named after people and groups ("tokenmaster_resident.txt" is a chat log,
    // not a token); the other roots hold only fixed names and presets.
    if (parts[0] != "chat_logs" && isSecretName(file))
    {
        why = "may hold a password or other secret";
        return false;
    }
    if (parts.size() < 2)
    {
        why = "not in a backup folder";
        return false;
    }
    const std::string root = parts[0];
    if (root == "user_settings")
    {
        if (parts.size() == 2 && inTable(std::begin(USER_FILES), std::end(USER_FILES), file)) return true;
        const size_t depth = parts.size() - 1;
        for (const DirRule& r : USER_DIRS)
        {
            if (lower(parts[1]) != r.dir) continue;
            if (depth < r.min_depth || depth > r.max_depth)
            {
                why = "not where its kind of file lives";
                return false;
            }
            if (!extAllowed(r.exts, file))
            {
                why = "file type not kept for " + std::string(r.dir);
                return false;
            }
            return true;
        }
        why = "not a file Prestige backs up";
        return false;
    }
    if (root == "account")
    {
        if (parts.size() == 2 && (inTable(std::begin(ACCOUNT_FILES), std::end(ACCOUNT_FILES), file)
                                  || inTable(std::begin(ACCOUNT_HISTORY_FILES), std::end(ACCOUNT_HISTORY_FILES), file)))
        {
            return true;
        }
        why = "not a file Prestige backs up";
        return false;
    }
    if (root == "chat_logs")
    {
        if (parts.size() != 2)
        {
            why = "transcripts are not kept in sub-folders";
            return false;
        }
        const std::string low = lower(file);
        for (const char* n : NOT_TRANSCRIPTS)
        {
            if (low == n)
            {
                why = "not a transcript";
                return false;
            }
        }
        if (low == "conversation.log" || extensionOf(low) == ".txt") return true;
        why = "not a transcript";
        return false;
    }
    why = "unknown folder \"" + root + "\"";
    return false;
}

bool isSafeAccountName(const std::string& name)
{
    if (name.empty() || name.size() > 128) return false;
    if (!((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9'))) return false;
    for (char c : name)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    if (name.back() == '.') return false;
    static const char* const OURS[] = { "user_settings", "logs", "cache", "skins", "browser_profile", "dlss",
                                        "effects", "fx", "bench", "region_maps", "speech_models", "vram_peers" };
    for (const char* o : OURS)
    {
        if (name == o) return false;
    }
    return true;
}

namespace
{
// expat 2.9 deprecates the 32-bit position calls; the bundled 2.6 has only those.
#if XML_MAJOR_VERSION > 2 || (XML_MAJOR_VERSION == 2 && XML_MINOR_VERSION >= 9)
size_t    byteIndex(XML_Parser p) { return size_t(XML_GetCurrentByteIndex64(p)); }
long long byteCount(XML_Parser p) { return (long long)XML_GetCurrentByteCount64(p); }
#else
size_t    byteIndex(XML_Parser p) { return size_t(XML_GetCurrentByteIndex(p)); }
long long byteCount(XML_Parser p) { return (long long)XML_GetCurrentByteCount(p); }
#endif

struct FilterState
{
    XML_Parser parser = nullptr;
    const std::function<bool(const std::string&)>* keep = nullptr;
    int depth = 0;
    bool root_map = false;
    bool in_key = false;
    bool have_key = false;
    std::string key;
    size_t key_start = 0;
    size_t elem_start_end = 0;
    std::vector<std::pair<size_t, size_t>> drops;
    std::vector<std::string> dropped;
    std::string error;

    void fail(const std::string& why)
    {
        if (error.empty()) error = why;
        XML_StopParser(parser, XML_FALSE);
    }
};

void XMLCALL filterStart(void* ud, const XML_Char* name, const XML_Char**)
{
    FilterState& st = *static_cast<FilterState*>(ud);
    ++st.depth;
    const std::string n = name;
    if (st.depth == 1 && n != "llsd") return st.fail("not an LLSD file");
    if (st.depth == 2)
    {
        if (n != "map" || st.root_map) return st.fail("not a settings map");
        st.root_map = true;
    }
    if (st.depth == 3)
    {
        const size_t idx = byteIndex(st.parser);
        st.elem_start_end = idx + size_t(byteCount(st.parser));
        if (n == "key")
        {
            if (st.have_key) return st.fail("a setting without a value");
            st.in_key = true;
            st.key.clear();
            st.key_start = idx;
        }
        else if (!st.have_key)
        {
            return st.fail("a value without a setting name");
        }
    }
}

void XMLCALL filterEnd(void* ud, const XML_Char*)
{
    FilterState& st = *static_cast<FilterState*>(ud);
    if (st.depth == 3)
    {
        const size_t idx = byteIndex(st.parser);
        const long long cnt = byteCount(st.parser);
        const size_t end = cnt > 0 ? idx + size_t(cnt) : st.elem_start_end;
        if (st.in_key)
        {
            st.in_key = false;
            st.have_key = true;
        }
        else
        {
            if (!(*st.keep)(st.key))
            {
                st.drops.emplace_back(st.key_start, end);
                st.dropped.push_back(st.key);
            }
            st.have_key = false;
        }
    }
    --st.depth;
}

void XMLCALL filterText(void* ud, const XML_Char* s, int len)
{
    FilterState& st = *static_cast<FilterState*>(ud);
    if (st.in_key && st.depth == 3 && st.key.size() < 4096) st.key.append(s, size_t(len));
}
}

bool filterSettingsXml(const std::string& in, const std::function<bool(const std::string&)>& keep,
                       std::string& out, std::vector<std::string>* dropped, std::string& error)
{
    if (in.size() > MAX_SETTINGS_BYTES)
    {
        error = "settings file too large";
        return false;
    }
    FilterState st;
    st.keep = &keep;
    XML_Parser p = XML_ParserCreate(nullptr);
    if (!p)
    {
        error = "out of memory";
        return false;
    }
    st.parser = p;
    XML_SetUserData(p, &st);
    XML_SetElementHandler(p, filterStart, filterEnd);
    XML_SetCharacterDataHandler(p, filterText);
    const XML_Status status = XML_Parse(p, in.data(), int(in.size()), XML_TRUE);
    if (status != XML_STATUS_OK && st.error.empty())
    {
        st.error = std::string("not valid XML (") + XML_ErrorString(XML_GetErrorCode(p)) + ")";
    }
    XML_ParserFree(p);
    if (st.error.empty() && !st.root_map) st.error = "not a settings map";
    if (st.error.empty() && st.have_key) st.error = "a setting without a value";
    if (!st.error.empty())
    {
        error = st.error;
        return false;
    }
    out.clear();
    out.reserve(in.size());
    size_t pos = 0;
    for (const auto& d : st.drops)
    {
        if (d.first < pos || d.second > in.size() || d.second < d.first)
        {
            error = "could not filter the settings file";
            return false;
        }
        out.append(in, pos, d.first - pos);
        pos = d.second;
    }
    out.append(in, pos, std::string::npos);
    if (dropped) *dropped = st.dropped;
    return true;
}

// ---- manifest --------------------------------------------------------------------------------

std::string formatManifest(const Manifest& m)
{
    auto clean = [](std::string s)
    {
        for (char& c : s)
        {
            if (c == '\n' || c == '\r') c = ' ';
        }
        return s;
    };
    std::ostringstream out;
    out << "format=" << FORMAT_ID << "\n"
        << "version=" << m.version << "\n"
        << "created=" << clean(m.created) << "\n"
        << "viewer=" << clean(m.viewer) << "\n"
        << "account=" << clean(m.account) << "\n"
        << "history=" << (m.history ? 1 : 0) << "\n";
    return out.str();
}

bool parseManifest(const std::string& text, Manifest& m, std::string& error)
{
    std::map<std::string, std::string> kv;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    if (kv["format"] != FORMAT_ID)
    {
        error = "not a Prestige settings backup";
        return false;
    }
    const std::string& v = kv["version"];
    if (v.empty() || v.size() > 6 || v.find_first_not_of("0123456789") != std::string::npos)
    {
        error = "the backup has no valid version stamp";
        return false;
    }
    m.version = std::stoi(v);
    if (m.version < 1)
    {
        error = "the backup has no valid version stamp";
        return false;
    }
    if (m.version > FORMAT_VERSION)
    {
        error = "the backup was made by a newer Prestige (format " + v + "); update Prestige to restore it";
        return false;
    }
    m.created = kv["created"];
    m.viewer = kv["viewer"];
    m.account = kv["account"];
    m.history = kv["history"] == "1";
    if (!m.account.empty() && !isSafeAccountName(m.account))
    {
        error = "the backup names an invalid account folder";
        return false;
    }
    return true;
}

// ---- writing ---------------------------------------------------------------------------------

std::vector<Entry> collectFiles(const Sources& src, std::vector<std::string>* skipped)
{
    std::vector<Entry> out;
    std::error_code ec;
    auto add = [&](const std::string& path, const fs::path& file)
    {
        std::string why;
        if (!isAllowedPath(path, why))
        {
            if (skipped) skipped->push_back(path + ": " + why);
            return;
        }
        Entry e;
        e.path = path;
        e.source = file;
        out.push_back(std::move(e));
    };
    auto regular = [&](const fs::path& p)
    {
        std::error_code e2;
        return fs::is_regular_file(fs::symlink_status(p, e2));
    };

    if (!src.user_settings.empty())
    {
        for (const FileRule& r : USER_FILES)
        {
            if (std::string(r.name) == "settings.xml") continue;     // from the live settings
            const fs::path p = src.user_settings / r.name;
            if (regular(p)) add(std::string("user_settings/") + r.name, p);
        }
        for (const DirRule& r : USER_DIRS)
        {
            const fs::path dir = src.user_settings / r.dir;
            if (!fs::is_directory(dir, ec)) continue;
            size_t seen = 0;
            fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec))
            {
                if (++seen > MAX_COLLECT) break;
                if (it.depth() + 2 >= int(r.max_depth))
                {
                    it.disable_recursion_pending();
                }
                if (!regular(it->path())) continue;
                std::string generic = utf8(it->path().lexically_relative(src.user_settings));
                std::replace(generic.begin(), generic.end(), '\\', '/');
                add("user_settings/" + generic, it->path());
            }
            ec.clear();
        }
    }
    if (!src.account_dir.empty())
    {
        for (const FileRule& r : ACCOUNT_FILES)
        {
            if (std::string(r.name) == "settings_per_account.xml") continue;   // from the live settings
            const fs::path p = src.account_dir / r.name;
            if (regular(p)) add(std::string("account/") + r.name, p);
        }
        if (src.include_history)
        {
            for (const FileRule& r : ACCOUNT_HISTORY_FILES)
            {
                const fs::path p = src.account_dir / r.name;
                if (regular(p)) add(std::string("account/") + r.name, p);
            }
        }
    }
    if (src.include_history && !src.chat_dir.empty() && fs::is_directory(src.chat_dir, ec))
    {
        size_t seen = 0;
        for (fs::directory_iterator it(src.chat_dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (++seen > MAX_COLLECT) break;
            if (!regular(it->path())) continue;
            const std::string name = utf8(it->path().filename());
            std::string why;
            if (!isAllowedPath("chat_logs/" + name, why)) continue;   // not a transcript: not mentioned
            add("chat_logs/" + name, it->path());
        }
    }
    return out;
}

namespace
{
struct ZipWriter
{
    zipFile zf = nullptr;
    ~ZipWriter()
    {
        if (zf) zipClose(zf, nullptr);
    }
};

zip_fileinfo fileInfoNow()
{
    zip_fileinfo zi{};
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    zi.tmz_date.tm_sec = tm.tm_sec;
    zi.tmz_date.tm_min = tm.tm_min;
    zi.tmz_date.tm_hour = tm.tm_hour;
    zi.tmz_date.tm_mday = tm.tm_mday;
    zi.tmz_date.tm_mon = tm.tm_mon;
    zi.tmz_date.tm_year = tm.tm_year + 1900;
    zi.external_fa = (0100644u << 16);      // regular file, rw-r--r--
    return zi;
}

bool addData(zipFile zf, const std::string& name, const std::string& data)
{
    const zip_fileinfo zi = fileInfoNow();
    if (zipOpenNewFileInZip64(zf, name.c_str(), &zi, nullptr, 0, nullptr, 0, nullptr, ZIP_DEFLATE, ZIP_LEVEL, 1) != ZIP_OK)
    {
        return false;
    }
    size_t pos = 0;
    while (pos < data.size())
    {
        const uint32_t n = uint32_t(std::min<size_t>(data.size() - pos, 1u << 20));
        if (zipWriteInFileInZip(zf, data.data() + pos, n) != ZIP_OK)
        {
            zipCloseFileInZip(zf);
            return false;
        }
        pos += n;
    }
    return zipCloseFileInZip(zf) == ZIP_OK;
}

bool addFile(zipFile zf, const std::string& name, const fs::path& file, std::string& why)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        why = "could not be read";
        return false;
    }
    const zip_fileinfo zi = fileInfoNow();
    if (zipOpenNewFileInZip64(zf, name.c_str(), &zi, nullptr, 0, nullptr, 0, nullptr, ZIP_DEFLATE, ZIP_LEVEL, 1) != ZIP_OK)
    {
        why = "could not be added to the archive";
        return false;
    }
    std::vector<char> buf(256 * 1024);
    while (in)
    {
        in.read(buf.data(), std::streamsize(buf.size()));
        const std::streamsize n = in.gcount();
        if (n > 0 && zipWriteInFileInZip(zf, buf.data(), uint32_t(n)) != ZIP_OK)
        {
            zipCloseFileInZip(zf);
            why = "could not be written to the archive";
            return false;
        }
    }
    if (zipCloseFileInZip(zf) != ZIP_OK)
    {
        why = "could not be written to the archive";
        return false;
    }
    return true;
}
}

bool writeBackup(const fs::path& zip, const Manifest& manifest, const std::vector<Entry>& entries,
                 std::string& error, std::vector<std::string>* refused)
{
    fs::path partial = zip;
    partial += ".partial";
    std::error_code ec;
    fs::remove(partial, ec);
    {
        ZipWriter w;
        w.zf = zipOpen64(utf8(partial).c_str(), APPEND_STATUS_CREATE);
        if (!w.zf)
        {
            error = "could not create " + utf8(zip);
            return false;
        }
        if (!addData(w.zf, MANIFEST_NAME, formatManifest(manifest)))
        {
            error = "could not write to " + utf8(zip);
            w.zf = (zipClose(w.zf, nullptr), nullptr);
            fs::remove(partial, ec);
            return false;
        }
        std::set<std::string> seen;
        for (const Entry& e : entries)
        {
            std::string why;
            if (!isAllowedPath(e.path, why))
            {
                if (refused) refused->push_back(e.path + ": " + why);
                continue;
            }
            if (lower(e.path).rfind("chat_logs/", 0) == 0 && !manifest.history)
            {
                if (refused) refused->push_back(e.path + ": history not included");
                continue;
            }
            if (lower(e.path) == "account/teleport_history.txt" && !manifest.history)
            {
                if (refused) refused->push_back(e.path + ": history not included");
                continue;
            }
            if (!seen.insert(lower(e.path)).second)
            {
                if (refused) refused->push_back(e.path + ": duplicate");
                continue;
            }
            bool ok = false;
            if (isSettingsFile(e.path))
            {
                std::string raw;
                if (e.from_memory) raw = e.data;
                else if (!readFile(e.source, raw, MAX_SETTINGS_BYTES))
                {
                    if (refused) refused->push_back(e.path + ": could not be read");
                    continue;
                }
                std::string filtered, ferr;
                if (!filterSettingsXml(raw, keepSetting, filtered, nullptr, ferr))
                {
                    if (refused) refused->push_back(e.path + ": " + ferr);
                    continue;
                }
                ok = addData(w.zf, e.path, filtered);
                why = "could not be written to the archive";
            }
            else if (e.from_memory)
            {
                ok = addData(w.zf, e.path, e.data);
                why = "could not be written to the archive";
            }
            else
            {
                ok = addFile(w.zf, e.path, e.source, why);
            }
            if (!ok)
            {
                // A failed entry may leave the archive in a bad state: give up rather than
                // hand the user a backup that silently lacks files.
                error = e.path + " " + why;
                w.zf = (zipClose(w.zf, nullptr), nullptr);
                fs::remove(partial, ec);
                return false;
            }
        }
        const int rc = zipClose(w.zf, nullptr);
        w.zf = nullptr;
        if (rc != ZIP_OK)
        {
            error = "could not finish " + utf8(zip);
            fs::remove(partial, ec);
            return false;
        }
    }
    fs::rename(partial, zip, ec);
    if (ec)
    {
        error = "could not write " + utf8(zip) + " (" + ec.message() + ")";
        fs::remove(partial, ec);
        return false;
    }
    return true;
}

// ---- reading ---------------------------------------------------------------------------------

namespace
{
struct UnzipCloser
{
    unzFile f;
    ~UnzipCloser() { unzClose(f); }
};

struct ZipItem
{
    std::string name;
    uint64_t size = 0;
    bool dir = false;
};

// Read the current entry into memory (at most max bytes).
bool readCurrent(unzFile uf, uint64_t max, std::string& out)
{
    if (unzOpenCurrentFile(uf) != UNZ_OK) return false;
    out.clear();
    std::vector<char> buf(64 * 1024);
    while (true)
    {
        const int n = unzReadCurrentFile(uf, buf.data(), unsigned(buf.size()));
        if (n < 0)
        {
            unzCloseCurrentFile(uf);
            return false;
        }
        if (n == 0) break;
        if (out.size() + size_t(n) > max)
        {
            unzCloseCurrentFile(uf);
            return false;
        }
        out.append(buf.data(), size_t(n));
    }
    return unzCloseCurrentFile(uf) == UNZ_OK;
}
}

bool readBackup(const fs::path& zip, const fs::path& dest, const Limits& limits, ReadResult& result,
                std::string& error)
{
    result = ReadResult();
    unzFile uf = unzOpen64(utf8(zip).c_str());
    if (!uf)
    {
        error = "not a zip archive, or it is damaged";
        return false;
    }
    UnzipCloser closer{ uf };

    // Pass 1: every name and header is checked before anything is written.
    std::vector<ZipItem> items;
    uint64_t total = 0;
    bool have_manifest = false;
    std::string manifest_text;
    int rc = unzGoToFirstFile(uf);
    if (rc == UNZ_END_OF_LIST_OF_FILE)
    {
        error = "the archive is empty";
        return false;
    }
    while (rc == UNZ_OK)
    {
        if (items.size() >= limits.max_entries)
        {
            error = "too many entries in the archive";
            return false;
        }
        char name[1100] = {};
        unz_file_info64 info;
        if (unzGetCurrentFileInfo64(uf, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
        {
            error = "the archive is damaged (bad entry header)";
            return false;
        }
        if (info.size_filename >= sizeof(name) - 1)
        {
            error = "an entry name is too long";
            return false;
        }
        if (info.flag & 1)
        {
            error = "the archive is encrypted";
            return false;
        }
        ZipItem item;
        item.name = name;
        item.size = info.uncompressed_size;
        if (((info.external_fa >> 16) & 0170000) == 0120000)
        {
            error = "the archive contains a link (" + item.name + ")";
            return false;
        }
        item.dir = !item.name.empty() && item.name.back() == '/';
        const std::string check = item.dir ? item.name.substr(0, item.name.size() - 1) : item.name;
        std::string why;
        if (!isSafePath(check, why))
        {
            error = "unsafe entry \"" + item.name + "\": " + why;
            return false;
        }
        if (item.name.size() > limits.max_path)
        {
            error = "an entry name is too long";
            return false;
        }
        if (item.size > limits.max_file)
        {
            error = item.name + " is too large";
            return false;
        }
        total += item.size;
        if (total > limits.max_total)
        {
            error = "the archive unpacks to too much data";
            return false;
        }
        if (!item.dir && item.name == MANIFEST_NAME)
        {
            if (have_manifest)
            {
                error = "the archive has two manifests";
                return false;
            }
            if (!readCurrent(uf, MAX_MANIFEST_BYTES, manifest_text))
            {
                error = "the backup's manifest could not be read";
                return false;
            }
            have_manifest = true;
        }
        items.push_back(item);
        rc = unzGoToNextFile(uf);
    }
    if (rc != UNZ_END_OF_LIST_OF_FILE)
    {
        error = "the archive is damaged (entry list)";
        return false;
    }
    if (!have_manifest)
    {
        error = "not a Prestige settings backup (no " + std::string(MANIFEST_NAME) + ")";
        return false;
    }
    if (!parseManifest(manifest_text, result.manifest, error)) return false;

    // Which entries to unpack.
    std::set<std::string> wanted, seen;
    for (const ZipItem& item : items)
    {
        if (item.dir || item.name == MANIFEST_NAME) continue;
        std::string why;
        const std::string low = lower(item.name);
        if (!isAllowedPath(item.name, why))
        {
            result.ignored.push_back(item.name + ": " + why);
            continue;
        }
        if (!result.manifest.history && (low.rfind("chat_logs/", 0) == 0 || low == "account/teleport_history.txt"))
        {
            result.ignored.push_back(item.name + ": history not announced in the manifest");
            continue;
        }
        if (!seen.insert(low).second)
        {
            result.ignored.push_back(item.name + ": duplicate");
            continue;
        }
        wanted.insert(item.name);
    }

    // Pass 2: unpack.
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (!fs::is_directory(dest, ec))
    {
        error = "could not create " + utf8(dest);
        return false;
    }
    if (!fs::is_empty(dest, ec))
    {
        error = utf8(dest) + " is not empty";
        return false;
    }
    const fs::path dest_abs = fs::absolute(dest, ec);
    uint64_t written_total = 0;
    std::vector<char> buf(64 * 1024);
    rc = unzGoToFirstFile(uf);
    while (rc == UNZ_OK)
    {
        char name[1100] = {};
        unz_file_info64 info;
        if (unzGetCurrentFileInfo64(uf, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
        {
            error = "the archive is damaged (bad entry header)";
            return false;
        }
        const std::string entry = name;
        if (!wanted.erase(entry))
        {
            rc = unzGoToNextFile(uf);
            continue;
        }
        const fs::path out_path = (dest_abs / fromUtf8(entry)).lexically_normal();
        // Belt and braces: the joined path must still be inside dest.
        const std::string rel = utf8(out_path.lexically_relative(dest_abs));
        if (rel.empty() || rel.rfind("..", 0) == 0)
        {
            error = "unsafe entry \"" + entry + "\"";
            return false;
        }
        fs::create_directories(out_path.parent_path(), ec);
        if (unzOpenCurrentFile(uf) != UNZ_OK)
        {
            error = "the archive is damaged (" + entry + ")";
            return false;
        }
        std::string data;
        uint64_t written = 0;
        bool bad = false;
        {
            std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
            if (!out)
            {
                unzCloseCurrentFile(uf);
                error = "could not write " + entry;
                return false;
            }
            while (true)
            {
                const int n = unzReadCurrentFile(uf, buf.data(), unsigned(buf.size()));
                if (n < 0)
                {
                    bad = true;
                    break;
                }
                if (n == 0) break;
                written += uint64_t(n);
                written_total += uint64_t(n);
                if (written > limits.max_file || written_total > limits.max_total)
                {
                    unzCloseCurrentFile(uf);
                    error = "the archive unpacks to more than its headers say (" + entry + ")";
                    return false;
                }
                out.write(buf.data(), n);
                if (!out)
                {
                    unzCloseCurrentFile(uf);
                    error = "could not write " + entry;
                    return false;
                }
            }
        }
        if (unzCloseCurrentFile(uf) != UNZ_OK || bad)
        {
            error = "the archive is damaged (" + entry + ")";
            return false;
        }
        if (isSettingsFile(entry))
        {
            std::string raw, filtered, ferr;
            if (!readFile(out_path, raw, MAX_SETTINGS_BYTES)
                || !filterSettingsXml(raw, keepSetting, filtered, nullptr, ferr)
                || !writeFileAtomic(out_path, filtered))
            {
                fs::remove(out_path, ec);
                result.ignored.push_back(entry + ": not a valid settings file" + (ferr.empty() ? "" : " (" + ferr + ")"));
                rc = unzGoToNextFile(uf);
                continue;
            }
        }
        result.files.push_back(entry);
        rc = unzGoToNextFile(uf);
    }
    if (rc != UNZ_END_OF_LIST_OF_FILE)
    {
        error = "the archive is damaged (entry list)";
        return false;
    }
    if (result.files.empty())
    {
        error = "the backup holds nothing Prestige can restore";
        return false;
    }
    return true;
}

// ---- staged restore ------------------------------------------------------------------------

bool writePendingMarker(const fs::path& pending, const std::string& account, const std::string& source,
                        std::string& error)
{
    if (!account.empty() && !isSafeAccountName(account))
    {
        error = "invalid account folder name";
        return false;
    }
    std::string clean_source = source;
    for (char& c : clean_source)
    {
        if (c == '\n' || c == '\r') c = ' ';
    }
    const std::string text = "account=" + account + "\nsource=" + clean_source + "\nstaged=" + nowString() + "\n";
    if (!writeFileAtomic(pending / PENDING_MARKER, text))
    {
        error = "could not write " + utf8(pending / PENDING_MARKER);
        return false;
    }
    return true;
}

ApplyResult applyPendingRestore(const fs::path& user_settings, const fs::path& app_dir)
{
    ApplyResult res;
    std::error_code ec;
    const fs::path base = user_settings / RESTORE_DIR;
    const fs::path pending = base / "pending";
    // A preview that never finished (the viewer quit with the dialog open) is not applied.
    fs::remove_all(base / "staging", ec);
    if (!fs::is_directory(pending, ec)) return res;

    std::string marker;
    if (!readFile(pending / PENDING_MARKER, marker, MAX_MANIFEST_BYTES))
    {
        res.log.push_back("an incomplete settings restore was found and discarded");
        fs::remove_all(pending, ec);
        return res;
    }
    std::string account, source;
    {
        std::istringstream in(marker);
        std::string line;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = trim(line.substr(0, eq));
            if (k == "account") account = trim(line.substr(eq + 1));
            else if (k == "source") source = trim(line.substr(eq + 1));
        }
    }
    const bool account_ok = !account.empty() && isSafeAccountName(account);
    res.log.push_back("restoring settings from " + (source.empty() ? std::string("a backup") : source)
                      + (account_ok ? " (per-account files for " + account + ")" : std::string()));

    std::vector<std::pair<fs::path, fs::path>> copies;
    size_t seen = 0;
    for (fs::recursive_directory_iterator it(pending, ec), end; !ec && it != end; it.increment(ec))
    {
        if (++seen > MAX_COLLECT) break;
        if (!fs::is_regular_file(it->symlink_status(ec))) continue;
        std::string rel = utf8(it->path().lexically_relative(pending));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (rel == PENDING_MARKER) continue;
        std::string why;
        if (!isAllowedPath(rel, why))
        {
            res.log.push_back("not restored: " + rel + " (" + why + ")");
            continue;
        }
        const size_t slash = rel.find('/');
        const std::string root = rel.substr(0, slash);
        const std::string rest = rel.substr(slash + 1);
        if (root == "user_settings")
        {
            copies.emplace_back(it->path(), user_settings / fromUtf8(rest));
        }
        else if (root == "account" && account_ok)
        {
            copies.emplace_back(it->path(), app_dir / account / fromUtf8(rest));
        }
        else
        {
            res.log.push_back("not restored: " + rel + " (no account to restore it to)");
        }
    }
    for (const auto& c : copies)
    {
        if (copyFileAtomic(c.first, c.second))
        {
            ++res.files;
        }
        else
        {
            res.log.push_back("could not write " + utf8(c.second));
        }
    }
    res.applied = true;
    res.log.push_back("restored " + std::to_string(res.files) + " file(s)");
    fs::remove_all(pending, ec);
    return res;
}

void pruneBackups(const fs::path& dir, const std::string& prefix, size_t keep)
{
    std::error_code ec;
    std::vector<fs::path> found;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = utf8(it->path().filename());
        if (name.rfind(prefix, 0) == 0 && endsWith(lower(name), ".zip") && fs::is_regular_file(it->symlink_status(ec)))
        {
            found.push_back(it->path());
        }
    }
    // Names carry a sortable timestamp: newest last.
    std::sort(found.begin(), found.end());
    while (found.size() > keep)
    {
        fs::remove(found.front(), ec);
        found.erase(found.begin());
    }
}
}
