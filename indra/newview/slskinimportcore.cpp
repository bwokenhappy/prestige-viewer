/**
 * @file slskinimportcore.cpp
 * @brief Skin importer (#1599): see slskinimportcore.h.
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

// No precompiled header: this file builds on its own in the check fixture.
#include "slskinimportcore.h"

#ifndef XML_STATIC
#define XML_STATIC
#endif
#if __has_include("expat/expat.h")
#include "expat/expat.h"
#else
#include <expat.h>
#endif
#include <minizip-ng/unzip.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace slskin
{
namespace
{
constexpr uint64_t MAX_XML_BYTES = 8ull * 1024 * 1024;     // one XML / JSON file we read
constexpr size_t   MAX_XML_NODES = 200000;
constexpr size_t   MAX_NODE_TEXT = 1024 * 1024;
constexpr size_t   MAX_LIST_SCAN = 20000;                  // directory entries looked at per walk

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

std::string extensionOf(const std::string& rel)
{
    const size_t slash = rel.find_last_of('/');
    const size_t dot = rel.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
    return lower(rel.substr(dot + 1));
}

bool isDir(const fs::path& p)
{
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    return !ec && fs::is_directory(st);
}

bool isRegular(const fs::path& p)
{
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    return !ec && fs::is_regular_file(st);
}

bool writeFile(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), std::streamsize(text.size()));
    return bool(out);
}

// Child directories / files of `dir`, never through symlinks, sorted, capped.
std::vector<fs::path> listDir(const fs::path& dir, bool want_dirs)
{
    std::vector<fs::path> out;
    std::error_code ec;
    fs::directory_iterator it(dir, ec), end;
    size_t seen = 0;
    for (; !ec && it != end && seen < MAX_LIST_SCAN; it.increment(ec), ++seen)
    {
        const fs::file_status st = it->symlink_status(ec);
        if (ec) break;
        if (want_dirs ? fs::is_directory(st) : fs::is_regular_file(st)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Every regular file under `dir` (relative, '/'-separated), never following symlinks.
void walkFiles(const fs::path& dir, const std::string& prefix, size_t depth, size_t& budget, std::vector<std::string>& out)
{
    if (depth > 8) return;
    for (const fs::path& f : listDir(dir, false))
    {
        if (budget == 0) return;
        --budget;
        out.push_back(prefix + utf8(f.filename()));
    }
    for (const fs::path& d : listDir(dir, true))
    {
        if (budget == 0) return;
        walkFiles(d, prefix + utf8(d.filename()) + "/", depth + 1, budget, out);
    }
}

// dir/rel, matching each component case-insensitively when the exact one is missing
// (Firestorm is mostly used on Windows, where textures.xml and the files disagree on case).
bool findFile(const fs::path& dir, const std::string& rel, fs::path& out)
{
    const fs::path exact = dir / fromUtf8(rel);
    if (isRegular(exact))
    {
        out = exact;
        return true;
    }
    fs::path cur = dir;
    std::stringstream ss(rel);
    std::string comp;
    std::vector<std::string> comps;
    while (std::getline(ss, comp, '/')) comps.push_back(comp);
    for (size_t i = 0; i < comps.size(); ++i)
    {
        const bool last = (i + 1 == comps.size());
        const fs::path direct = cur / fromUtf8(comps[i]);
        if (last ? isRegular(direct) : isDir(direct))
        {
            cur = direct;
            continue;
        }
        bool found = false;
        const std::string want = lower(comps[i]);
        for (const fs::path& p : listDir(cur, !last))
        {
            if (lower(utf8(p.filename())) == want)
            {
                cur = p;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    out = cur;
    return true;
}

bool parseDouble(const std::string& s, double& v)
{
    const char* b = s.data();
    const char* e = b + s.size();
    // from_chars takes no leading '+' and needs a digit before '.' in some implementations: ".5"
    std::string tmp;
    if (!s.empty() && s[0] == '.')
    {
        tmp = "0" + s;
        b = tmp.data();
        e = b + tmp.size();
    }
    const auto r = std::from_chars(b, e, v);
    return r.ec == std::errc() && r.ptr == e && std::isfinite(v);
}

std::string formatNumber(double v)
{
    v = std::round(v * 10000.0) / 10000.0;
    if (v == 0.0) v = 0.0;  // no "-0"
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, r.ptr);
}

bool isInteger(const std::string& s)
{
    if (s.empty() || s.size() > 6) return false;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i == s.size()) return false;
    for (; i < s.size(); ++i)
    {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

void listLines(std::ostringstream& out, const char* title, const std::vector<std::string>& lines, size_t max_lines)
{
    if (lines.empty()) return;
    out << title << " (" << lines.size() << "):\n";
    size_t n = 0;
    for (const std::string& l : lines)
    {
        if (n++ == max_lines)
        {
            out << "  ... and " << (lines.size() - max_lines) << " more (see the log)\n";
            break;
        }
        out << "  " << l << "\n";
    }
}
}

// ---------------------------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------------------------

void Report::skip(const std::string& what, const std::string& why)
{
    skipped.push_back(what + ": " + why);
}

std::string Report::summary(size_t max_lines) const
{
    std::ostringstream out;
    if (!skin_name.empty()) out << "Installed \"" << skin_name << "\" (skins/" << folder << ")\n";
    if (!source.empty()) out << "From: " << source << "\n";
    out << "Colours: " << colors_kept << " imported, " << colors_aliased << " converted, " << colors_dropped
        << " unused, " << colors_invalid << " invalid\n";
    out << "Textures: " << textures_kept << " imported, " << textures_fallback << " kept ours (bad image), "
        << textures_unknown << " unused images";
    if (textures_base) out << ", " << textures_base << " unchanged from the other viewer's default";
    out << "\n";
    out << "Layout files: " << xui_kept << " imported, " << xui_skipped << " skipped\n";
    if (settings_kept || settings_dropped)
        out << "Skin settings: " << settings_kept << " imported, " << settings_dropped << " unknown\n";
    if (entries_rejected) out << "Rejected files: " << entries_rejected << "\n";
    listLines(out, "Notes", notes, max_lines);
    listLines(out, "Converted", converted, max_lines);
    listLines(out, "Skipped", skipped, max_lines);
    return out.str();
}

// ---------------------------------------------------------------------------------------------
// Paths and names
// ---------------------------------------------------------------------------------------------

bool sanitizeEntryPath(const std::string& raw, const Limits& limits, std::string& out, std::string* why)
{
    auto fail = [&](const char* reason)
    {
        if (why) *why = reason;
        out.clear();
        return false;
    };
    if (raw.empty()) return fail("empty name");
    if (raw.size() > limits.max_path) return fail("path too long");
    for (unsigned char c : raw)
    {
        if (c < 0x20 || c == 0x7f) return fail("control character in the name");
    }
    std::string s = raw;
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s[0] == '/') return fail("absolute path");
    if (s.find(':') != std::string::npos) return fail("':' in the name (drive letter or stream)");

    static const std::set<std::string> devices = { "con", "prn", "aux", "nul", "conin$", "conout$",
        "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
        "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9" };
    std::vector<std::string> comps;
    std::stringstream ss(s);
    std::string comp;
    while (std::getline(ss, comp, '/'))
    {
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") return fail("'..' in the path");
        if (comp.back() == '.' || comp.back() == ' ') return fail("name ends in '.' or a space");
        const std::string stem = lower(comp.substr(0, comp.find('.')));
        if (devices.count(stem)) return fail("reserved device name");
        comps.push_back(comp);
    }
    if (comps.empty()) return fail("empty name");
    if (comps.size() > limits.max_depth) return fail("path too deep");
    out.clear();
    for (size_t i = 0; i < comps.size(); ++i)
    {
        if (i) out += '/';
        out += comps[i];
    }
    return true;
}

bool isAllowedSkinFile(const std::string& rel)
{
    static const std::set<std::string> allowed = { "xml", "json", "txt", "png", "tga", "jpg", "jpeg", "j2c", "jp2",
                                                   "bmp", "svg" };
    return allowed.count(extensionOf(rel)) != 0;
}

std::string safeFolderName(const std::string& display_name)
{
    std::string out;
    bool pending = false;
    for (unsigned char c : display_name)
    {
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum)
        {
            pending = true;
            continue;
        }
        if (pending && !out.empty()) out += '_';
        pending = false;
        out += char((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? std::string("skin") : out;
}

bool isReservedFolderName(const std::string& folder, const std::set<std::string>& system_skins)
{
    if (folder.empty() || folder[0] == '.') return true;
    const std::string l = lower(folder);
    if (l == "default") return true;
    for (const std::string& s : system_skins)
    {
        if (lower(s) == l) return true;
    }
    return false;
}

std::string uniqueFolderName(const std::string& base, const std::function<bool(const std::string&)>& taken)
{
    if (!taken(base)) return base;
    for (int i = 2; i < 1000; ++i)
    {
        const std::string candidate = base + "_" + std::to_string(i);
        if (!taken(candidate)) return candidate;
    }
    return base + "_" + std::to_string(std::rand() % 100000 + 1000);
}

// ---------------------------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------------------------

namespace
{
// A small JSON reader: top-level object of scalars; nested values are skipped.
struct JsonReader
{
    const std::string& s;
    size_t i = 0;

    explicit JsonReader(const std::string& text) : s(text) {}

    void ws()
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    static void putUtf8(std::string& out, uint32_t cp)
    {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }

    bool hex4(uint32_t& v)
    {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool str(std::string& out)
    {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size())
        {
            const char c = s[i++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e)
            {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u')
                    {
                        i += 2;
                        uint32_t lo = 0;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    // A scalar as text; nested containers are skipped (out empty).
    bool value(std::string& out, int depth)
    {
        ws();
        if (i >= s.size() || depth > 32) return false;
        const char c = s[i];
        if (c == '"') return str(out);
        if (c == '{' || c == '[')
        {
            const char close = (c == '{') ? '}' : ']';
            ++i;
            out.clear();
            ws();
            if (i < s.size() && s[i] == close)
            {
                ++i;
                return true;
            }
            while (true)
            {
                std::string tmp;
                if (c == '{')
                {
                    ws();
                    if (!str(tmp)) return false;
                    ws();
                    if (i >= s.size() || s[i] != ':') return false;
                    ++i;
                }
                if (!value(tmp, depth + 1)) return false;
                ws();
                if (i >= s.size()) return false;
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == close) { ++i; out.clear(); return true; }
                return false;
            }
        }
        const size_t b = i;
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' || s[i] == '.')) ++i;
        out = s.substr(b, i - b);
        if (out == "true" || out == "false" || out == "null") return true;
        double d = 0;
        return parseDouble(out, d);
    }

    bool object(std::map<std::string, std::string>& fields)
    {
        ws();
        if (i >= s.size() || s[i] != '{') return false;
        ++i;
        ws();
        if (i < s.size() && s[i] == '}')
        {
            ++i;
            ws();
            return i == s.size();
        }
        while (true)
        {
            ws();
            std::string key, val;
            if (!str(key)) return false;
            ws();
            if (i >= s.size() || s[i] != ':') return false;
            ++i;
            if (!value(val, 1)) return false;
            if (val != "null") fields[key] = val;
            ws();
            if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; ws(); return i == s.size(); }
            return false;
        }
    }
};

std::string cleanField(const std::string& v, size_t cap)
{
    std::string out;
    for (unsigned char c : v)
    {
        if (c < 0x20 && c != '\n') continue;
        out += char(c);
    }
    out = trim(out);
    if (out.size() > cap)
    {
        out.resize(cap);
        // do not leave half a UTF-8 sequence
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && ((unsigned char)out.back() & 0x80)) out.pop_back();
    }
    return out;
}
}

bool parseManifest(const std::string& json, Manifest& out)
{
    if (json.size() > 256 * 1024) return false;
    std::string text = json;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text = text.substr(3);
    std::map<std::string, std::string> f;
    JsonReader reader(text);
    if (!reader.object(f)) return false;
    Manifest m;
    auto get = [&](const char* key, size_t cap) { auto it = f.find(key); return it == f.end() ? std::string() : cleanField(it->second, cap); };
    m.name = get("name", 64);
    std::replace(m.name.begin(), m.name.end(), '\n', ' ');   // one line: list rows, dialogs
    if (m.name.empty()) return false;
    m.author = get("author", 128);
    m.url = get("url", 512);
    m.version = get("version", 32);
    m.notes = get("notes", 2000);
    m.compatibility = get("compatibility", 32);
    m.date = get("date", 64);
    m.source_viewer = get("viewer", 64);
    if (m.source_viewer.empty()) m.source_viewer = get("source_viewer", 64);
    out = m;
    return true;
}

Manifest resolveManifest(const std::string* json, const std::string& fallback_name, const std::string& source_viewer)
{
    Manifest m;
    if (!json || !parseManifest(*json, m))
    {
        m = Manifest();
        m.name = cleanField(fallback_name, 64);
        std::replace(m.name.begin(), m.name.end(), '\n', ' ');
        if (m.name.empty()) m.name = "Imported skin";
        m.notes = "Imported skin (no usable manifest.json; this one was generated).";
        m.generated = true;
    }
    if (m.source_viewer.empty()) m.source_viewer = source_viewer;
    return m;
}

std::string jsonEscape(const std::string& text)
{
    std::string out;
    for (unsigned char c : text)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += char(c);
                }
        }
    }
    return out;
}

std::string manifestJson(const Manifest& m)
{
    std::ostringstream out;
    out << "{\n  \"name\": \"" << jsonEscape(m.name) << "\"";
    const std::pair<const char*, const std::string*> fields[] = { { "author", &m.author }, { "url", &m.url },
        { "version", &m.version }, { "compatibility", &m.compatibility }, { "date", &m.date },
        { "viewer", &m.source_viewer }, { "notes", &m.notes } };
    for (const auto& f : fields)
    {
        if (!f.second->empty()) out << ",\n  \"" << f.first << "\": \"" << jsonEscape(*f.second) << "\"";
    }
    out << "\n}\n";
    return out.str();
}

std::string nameFromPath(const std::string& path)
{
    std::string p = path;
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    const size_t slash = p.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
    if (lower(extensionOf(base)) == "zip") base = base.substr(0, base.size() - 4);
    std::replace(base.begin(), base.end(), '_', ' ');
    return trim(base);
}

// ---------------------------------------------------------------------------------------------
// XML
// ---------------------------------------------------------------------------------------------

const std::string* XmlNode::attr(const std::string& key) const
{
    for (const auto& a : attrs)
    {
        if (a.first == key) return &a.second;
    }
    return nullptr;
}

namespace
{
struct XmlBuilder
{
    XML_Parser parser = nullptr;
    std::unique_ptr<XmlNode> root;
    std::vector<XmlNode*> stack;
    size_t count = 0;
    std::string error;

    void stop(const char* why)
    {
        if (error.empty()) error = why;
        XML_StopParser(parser, XML_FALSE);
    }

    static void XMLCALL onStart(void* ud, const XML_Char* tag, const XML_Char** atts)
    {
        XmlBuilder* b = static_cast<XmlBuilder*>(ud);
        if (++b->count > MAX_XML_NODES)
        {
            b->stop("too many elements");
            return;
        }
        auto node = std::make_unique<XmlNode>();
        node->tag = tag;
        for (int k = 0; atts && atts[k]; k += 2) node->attrs.emplace_back(atts[k], atts[k + 1]);
        XmlNode* raw = node.get();
        if (b->stack.empty())
        {
            if (b->root)
            {
                b->stop("more than one root element");
                return;
            }
            b->root = std::move(node);
        }
        else
        {
            b->stack.back()->children.push_back(std::move(node));
        }
        b->stack.push_back(raw);
    }

    static void XMLCALL onEnd(void* ud, const XML_Char*)
    {
        XmlBuilder* b = static_cast<XmlBuilder*>(ud);
        if (b->stack.empty()) return;
        b->stack.back()->text = trim(b->stack.back()->text);
        b->stack.pop_back();
    }

    static void XMLCALL onText(void* ud, const XML_Char* s, int len)
    {
        XmlBuilder* b = static_cast<XmlBuilder*>(ud);
        if (b->stack.empty() || len <= 0) return;
        std::string& t = b->stack.back()->text;
        if (t.size() + size_t(len) > MAX_NODE_TEXT)
        {
            b->stop("text too long");
            return;
        }
        t.append(s, size_t(len));
    }

    static void XMLCALL onEntity(void* ud, const XML_Char*, int, const XML_Char*, int, const XML_Char*, const XML_Char*,
                                 const XML_Char*, const XML_Char*)
    {
        static_cast<XmlBuilder*>(ud)->stop("entity declarations are not allowed");
    }
};

void writeNode(std::ostringstream& out, const XmlNode& n, int depth)
{
    const std::string pad(size_t(depth) * 2, ' ');
    out << pad << "<" << n.tag;
    for (const auto& a : n.attrs) out << " " << a.first << "=\"" << xmlEscape(a.second) << "\"";
    if (n.children.empty() && n.text.empty())
    {
        out << " />\n";
        return;
    }
    out << ">";
    if (!n.text.empty()) out << xmlEscape(n.text);
    if (!n.children.empty())
    {
        out << "\n";
        for (const auto& c : n.children) writeNode(out, *c, depth + 1);
        out << pad;
    }
    out << "</" << n.tag << ">\n";
}
}

std::unique_ptr<XmlNode> parseXml(const std::string& text, std::string* error)
{
    if (text.size() > MAX_XML_BYTES)
    {
        if (error) *error = "file too large";
        return nullptr;
    }
    XmlBuilder b;
    b.parser = XML_ParserCreate(nullptr);
    if (!b.parser)
    {
        if (error) *error = "out of memory";
        return nullptr;
    }
    XML_SetUserData(b.parser, &b);
    XML_SetElementHandler(b.parser, &XmlBuilder::onStart, &XmlBuilder::onEnd);
    XML_SetCharacterDataHandler(b.parser, &XmlBuilder::onText);
    XML_SetEntityDeclHandler(b.parser, &XmlBuilder::onEntity);
    const XML_Status st = XML_Parse(b.parser, text.data(), int(text.size()), XML_TRUE);
    if (st != XML_STATUS_OK || !b.root)
    {
        if (error)
        {
            if (!b.error.empty()) *error = b.error;
            else
            {
                const XML_LChar* msg = XML_ErrorString(XML_GetErrorCode(b.parser));
                *error = msg ? std::string(msg) : std::string("not XML");
            }
        }
        XML_ParserFree(b.parser);
        return nullptr;
    }
    XML_ParserFree(b.parser);
    return std::move(b.root);
}

std::string xmlEscape(const std::string& text)
{
    std::string out;
    for (char c : text)
    {
        switch (c)
        {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string writeXml(const XmlNode& root)
{
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\" ?>\n";
    writeNode(out, root, 0);
    return out.str();
}

bool readRegularFile(const fs::path& path, uint64_t cap, std::string& out)
{
    if (!isRegular(path)) return false;
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size > cap) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(size_t(size), '\0');
    if (size) in.read(&out[0], std::streamsize(size));
    out.resize(size_t(in.gcount()));
    return true;
}

// ---------------------------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------------------------

std::vector<ColorDef> parseColors(const std::string& xml, bool* ok)
{
    std::vector<ColorDef> out;
    std::unique_ptr<XmlNode> root = parseXml(xml);
    if (ok) *ok = root && root->tag == "colors";
    if (!root || root->tag != "colors") return out;
    for (const auto& c : root->children)
    {
        if (c->tag != "color") continue;
        ColorDef d;
        if (const std::string* n = c->attr("name")) d.name = trim(*n);
        if (const std::string* v = c->attr("value")) d.value = trim(*v);
        if (const std::string* r = c->attr("reference")) d.reference = trim(*r);
        if (!d.name.empty()) out.push_back(d);
    }
    return out;
}

bool normalizeColorValue(const std::string& in, std::string& out)
{
    std::string s = in;
    std::replace(s.begin(), s.end(), ',', ' ');
    std::stringstream ss(s);
    std::string tok;
    std::vector<double> v;
    while (ss >> tok)
    {
        double d = 0;
        if (v.size() == 4 || !parseDouble(tok, d)) return false;
        if (d < -1e-6 || d > 1.0 + 1e-6) return false;
        v.push_back(std::clamp(d, 0.0, 1.0));
    }
    if (v.size() < 3) return false;
    if (v.size() == 3) v.push_back(1.0);
    out = formatNumber(v[0]) + " " + formatNumber(v[1]) + " " + formatNumber(v[2]) + " " + formatNumber(v[3]);
    return true;
}

const std::map<std::string, std::string>& firestormColorAliases()
{
    // Firestorm-only names whose meaning matches one of ours (the rest of Firestorm's extra
    // colours are for features we do not have and are dropped).
    static const std::map<std::string, std::string> aliases = {
        { "FriendsChatColor",       "FriendChatColor" },
        { "ChatNameColor",          "ChatHeaderDisplayNameColor" },
        { "NameTagUsername",        "NameTagLegacy" },
        { "FSChatHistoryBackground", "ChatHistoryBgColor" },
        { "NotecardTextColor",      "ScriptText" },
        { "NotecardBackgroundColor", "ScriptBackground" },
        { "NotecardCursorColor",    "ScriptCursorColor" },
        { "FpsDisplayUncappedColor", "FPSTextColor" },
    };
    return aliases;
}

ColorResult convertColors(const std::vector<std::vector<ColorDef>>& layers, const std::set<std::string>& ours,
                          const std::map<std::string, std::string>& aliases)
{
    ColorResult res;
    std::map<std::string, ColorDef> merged;
    for (const auto& layer : layers)
    {
        for (const ColorDef& d : layer) merged[d.name] = d;
    }

    // Resolve a name to a value, or to a reference to one of our names.
    std::function<bool(const std::string&, int, ColorDef&, std::string&)> resolve =
        [&](const std::string& name, int depth, ColorDef& out, std::string& why) -> bool
    {
        if (depth > 16)
        {
            why = "reference loop";
            return false;
        }
        const auto it = merged.find(name);
        if (it == merged.end())
        {
            if (ours.count(name))
            {
                out.value.clear();
                out.reference = name;
                return true;
            }
            why = "reference to unknown colour " + name;
            return false;
        }
        const ColorDef& d = it->second;
        if (!d.value.empty())
        {
            if (!normalizeColorValue(d.value, out.value))
            {
                why = "bad value \"" + d.value + "\"";
                return false;
            }
            out.reference.clear();
            return true;
        }
        if (d.reference.empty())
        {
            why = "no value";
            return false;
        }
        return resolve(d.reference, depth + 1, out, why);
    };

    for (const auto& kv : merged)
    {
        const std::string& name = kv.first;
        std::string target;
        if (ours.count(name))
        {
            target = name;
        }
        else
        {
            const auto a = aliases.find(name);
            if (a != aliases.end() && ours.count(a->second) && !merged.count(a->second))
            {
                target = a->second;
            }
        }
        if (target.empty())
        {
            res.dropped.push_back(name);
            continue;
        }
        ColorDef out;
        out.name = target;
        std::string why;
        if (!resolve(name, 0, out, why))
        {
            res.invalid.push_back(name + ": " + why);
            continue;
        }
        if (target != name) res.aliased.push_back(name + " -> " + target);
        res.out[target] = out;
    }
    return res;
}

std::string colorsXml(const ColorResult& colors)
{
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\" ?>\n";
    out << "<!-- Imported by the skin importer (#1599): only colour names this viewer uses. -->\n<colors>\n";
    for (const auto& kv : colors.out)
    {
        out << "  <color name=\"" << xmlEscape(kv.first) << "\" ";
        if (!kv.second.reference.empty()) out << "reference=\"" << xmlEscape(kv.second.reference) << "\" />\n";
        else out << "value=\"" << kv.second.value << "\" />\n";
    }
    out << "</colors>\n";
    return out.str();
}

// ---------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------

std::map<std::string, TextureDef> parseTextures(const std::string& xml, bool* ok)
{
    std::map<std::string, TextureDef> out;
    std::unique_ptr<XmlNode> root = parseXml(xml);
    if (ok) *ok = root && root->tag == "textures";
    if (!root || root->tag != "textures") return out;
    for (const auto& c : root->children)
    {
        if (c->tag != "texture") continue;
        TextureDef d;
        if (const std::string* n = c->attr("name")) d.name = trim(*n);
        if (d.name.empty()) continue;
        if (const std::string* f = c->attr("file_name")) d.file_name = trim(*f);
        if (const std::string* v = c->attr("vector_file")) d.has_vector = !trim(*v).empty();
        for (const char* key : { "scale.left", "scale.right", "scale.top", "scale.bottom" })
        {
            const std::string* s = c->attr(key);
            if (s && isInteger(trim(*s))) d.scale[key] = trim(*s);
        }
        out[d.name] = d;
    }
    return out;
}

bool sniffImage(const fs::path& file, uint64_t cap, std::string& why)
{
    std::error_code ec;
    if (!isRegular(file))
    {
        why = "not a regular file";
        return false;
    }
    const uintmax_t size = fs::file_size(file, ec);
    if (ec || size < 16)
    {
        why = "empty or truncated";
        return false;
    }
    if (size > cap)
    {
        why = "image too large";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    unsigned char h[32] = {};
    in.read(reinterpret_cast<char*>(h), sizeof(h));
    const std::string ext = extensionOf(utf8(file.filename()));
    const uint32_t max_dim = 4096;
    if (ext == "png")
    {
        static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (std::memcmp(h, sig, 8) != 0)
        {
            why = "not a PNG";
            return false;
        }
        const uint32_t w = (uint32_t(h[16]) << 24) | (uint32_t(h[17]) << 16) | (uint32_t(h[18]) << 8) | h[19];
        const uint32_t hgt = (uint32_t(h[20]) << 24) | (uint32_t(h[21]) << 16) | (uint32_t(h[22]) << 8) | h[23];
        if (!w || !hgt || w > max_dim || hgt > max_dim)
        {
            why = "bad image size";
            return false;
        }
        return true;
    }
    if (ext == "jpg" || ext == "jpeg")
    {
        if (!(h[0] == 0xFF && h[1] == 0xD8 && h[2] == 0xFF))
        {
            why = "not a JPEG";
            return false;
        }
        return true;
    }
    if (ext == "bmp")
    {
        if (!(h[0] == 'B' && h[1] == 'M'))
        {
            why = "not a BMP";
            return false;
        }
        return true;
    }
    if (ext == "j2c" || ext == "jp2")
    {
        const bool codestream = h[0] == 0xFF && h[1] == 0x4F && h[2] == 0xFF && h[3] == 0x51;
        const bool box = h[4] == 'j' && h[5] == 'P' && h[6] == ' ' && h[7] == ' ';
        if (!codestream && !box)
        {
            why = "not a JPEG 2000 image";
            return false;
        }
        return true;
    }
    if (ext == "tga")
    {
        const unsigned type = h[2];
        const uint32_t w = uint32_t(h[12]) | (uint32_t(h[13]) << 8);
        const uint32_t hgt = uint32_t(h[14]) | (uint32_t(h[15]) << 8);
        const unsigned bpp = h[16];
        const bool type_ok = type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11;
        if (!type_ok || !w || !hgt || w > max_dim || hgt > max_dim || (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32))
        {
            why = "not a TGA image";
            return false;
        }
        return true;
    }
    if (ext == "svg")
    {
        std::string text;
        if (!readRegularFile(file, std::min<uint64_t>(cap, 4 * 1024 * 1024), text) || text.find("<svg") == std::string::npos)
        {
            why = "not an SVG";
            return false;
        }
        return true;
    }
    why = "unsupported image format ." + ext;
    return false;
}

TexturePlan planTextures(const std::vector<Layer>& layers, const std::map<std::string, TextureDef>& ours,
                         const std::map<std::string, TextureDef>& theirs, const ImageCheck& check, const Limits& limits)
{
    TexturePlan plan;
    std::set<std::string> used_rel;        // lower case, for the "unused images" count
    std::map<std::string, bool> rel_ok;    // rel -> passed the check (each file is checked once)

    for (const auto& kv : ours)
    {
        const std::string& name = kv.first;
        const TextureDef& o = kv.second;
        const auto t = theirs.find(name);
        std::string raw = (t != theirs.end()) ? t->second.file_name : o.file_name;
        if (raw.empty()) raw = name;
        std::string rel, why;
        if (!sanitizeEntryPath(raw, limits, rel, &why))
        {
            plan.fallback.push_back(name + ": unsafe file name (" + why + ")");
            continue;
        }

        // Most specific layer first.
        fs::path found;
        bool found_base = false;
        for (auto it = layers.rbegin(); it != layers.rend(); ++it)
        {
            fs::path p;
            if (findFile(it->dir / "textures", rel, p))
            {
                found = p;
                found_base = it->base;
                break;
            }
        }
        if (found.empty()) continue;            // they do not have it: ours stays, nothing to say
        used_rel.insert(lower(rel));
        if (found_base)
        {
            ++plan.same_as_base;
            continue;
        }
        auto ok_it = rel_ok.find(rel);
        if (ok_it == rel_ok.end())
        {
            std::string bad;
            const bool ok = check ? check(found, bad) : sniffImage(found, limits.max_file, bad);
            ok_it = rel_ok.emplace(rel, ok).first;
            if (!ok)
            {
                plan.fallback.push_back(name + " (" + rel + "): " + bad);
                continue;
            }
        }
        else if (!ok_it->second)
        {
            plan.fallback.push_back(name + " (" + rel + "): bad image");
            continue;
        }
        TexturePlanItem item;
        item.name = name;
        item.rel = rel;
        item.src = found;
        if (t != theirs.end()) item.scale = t->second.scale;
        plan.items.push_back(item);
    }

    // Images in the source's own layers that none of our names point at.
    for (const Layer& layer : layers)
    {
        if (layer.base) continue;
        size_t budget = limits.max_files;
        std::vector<std::string> files;
        walkFiles(layer.dir / "textures", std::string(), 0, budget, files);
        for (const std::string& f : files)
        {
            const std::string ext = extensionOf(f);
            if (ext == "xml" || !isAllowedSkinFile(f)) continue;
            if (!used_rel.count(lower(f)))
            {
                used_rel.insert(lower(f));
                plan.unknown.push_back(f);
            }
        }
    }
    return plan;
}

std::string texturesXml(const TexturePlan& plan)
{
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\" ?>\n";
    out << "<!-- Imported by the skin importer (#1599). vector_file=\"\" shows the imported bitmap\n"
           "     instead of this viewer's vector icon. -->\n<textures version=\"101\">\n";
    for (const TexturePlanItem& item : plan.items)
    {
        out << "  <texture name=\"" << xmlEscape(item.name) << "\" file_name=\"" << xmlEscape(item.rel)
            << "\" vector_file=\"\"";
        for (const auto& s : item.scale) out << " " << s.first << "=\"" << s.second << "\"";
        out << " />\n";
    }
    out << "</textures>\n";
    return out.str();
}

// ---------------------------------------------------------------------------------------------
// XUI
// ---------------------------------------------------------------------------------------------

XuiInfo inspectXui(const std::string& text)
{
    XuiInfo info;
    std::unique_ptr<XmlNode> root = parseXml(text, &info.error);
    if (!root) return info;
    info.parsed = true;
    info.root_tag = root->tag;
    if (const std::string* n = root->attr("name")) info.root_name = *n;
    std::vector<const XmlNode*> todo = { root.get() };
    while (!todo.empty())
    {
        const XmlNode* n = todo.back();
        todo.pop_back();
        if (const std::string* nm = n->attr("name"))
        {
            if (!nm->empty()) info.names.insert(*nm);
        }
        for (const auto& c : n->children) todo.push_back(c.get());
    }
    return info;
}

XuiVerdict validateXui(const XuiInfo& ours, const XuiInfo& theirs, const std::set<std::string>& looked_up)
{
    XuiVerdict v;
    if (!theirs.parsed)
    {
        v.reason = "does not parse (" + theirs.error + ")";
        return v;
    }
    if (!ours.parsed)
    {
        v.reason = "no layout file of ours has this name";
        return v;
    }
    if (theirs.root_tag != ours.root_tag)
    {
        v.reason = "root element <" + theirs.root_tag + "> is not ours (<" + ours.root_tag + ">)";
        return v;
    }
    if (!ours.root_name.empty() && theirs.root_name != ours.root_name)
    {
        v.reason = "root name \"" + theirs.root_name + "\" is not ours (\"" + ours.root_name + "\")";
        return v;
    }
    for (const std::string& n : ours.names)
    {
        if (looked_up.count(n) && !theirs.names.count(n)) v.missing.push_back(n);
    }
    if (!v.missing.empty())
    {
        std::string list;
        for (size_t i = 0; i < v.missing.size() && i < 5; ++i) list += (i ? ", " : "") + v.missing[i];
        if (v.missing.size() > 5) list += ", ...";
        v.reason = std::to_string(v.missing.size()) + " control(s) our code needs are missing: " + list;
        return v;
    }
    v.ok = true;
    return v;
}

std::set<std::string> parseLookupNames(const std::string& text)
{
    std::set<std::string> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
    {
        line = trim(line);
        if (!line.empty() && line[0] != '#') out.insert(line);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// skin_settings.xml / fonts.xml
// ---------------------------------------------------------------------------------------------

namespace
{
// <map><key>K</key><X/>...</map> -> K -> X
std::vector<std::pair<std::string, const XmlNode*>> llsdMap(const XmlNode* map)
{
    std::vector<std::pair<std::string, const XmlNode*>> out;
    if (!map || map->tag != "map") return out;
    for (size_t i = 0; i + 1 < map->children.size(); ++i)
    {
        const XmlNode* k = map->children[i].get();
        if (k->tag != "key") continue;
        out.emplace_back(k->text, map->children[i + 1].get());
        ++i;
    }
    return out;
}

const XmlNode* llsdRoot(const XmlNode* root, const char* want)
{
    if (!root || root->tag != "llsd") return nullptr;
    for (const auto& c : root->children)
    {
        if (c->tag == want) return c.get();
    }
    return nullptr;
}

const XmlNode* llsdValue(const XmlNode* map, const std::string& key)
{
    for (const auto& kv : llsdMap(map))
    {
        if (kv.first == key) return kv.second;
    }
    return nullptr;
}

bool llsdScalarOk(const XmlNode& v)
{
    const std::string t = trim(v.text);
    if (v.tag == "boolean") return t == "0" || t == "1" || t == "true" || t == "false";
    if (v.tag == "integer") return isInteger(t) || t.empty();
    if (v.tag == "real")
    {
        double d = 0;
        return t.empty() || parseDouble(t, d);
    }
    if (v.tag == "string") return t.size() <= 1024;
    return false;
}

std::unique_ptr<XmlNode> cloneNode(const XmlNode& n)
{
    auto c = std::make_unique<XmlNode>();
    c->tag = n.tag;
    c->attrs = n.attrs;
    c->text = n.text;
    for (const auto& ch : n.children) c->children.push_back(cloneNode(*ch));
    return c;
}
}

std::string filterSkinSettings(const std::string& ours_xml, const std::string& theirs_xml, int& kept, int& dropped)
{
    kept = dropped = 0;
    std::unique_ptr<XmlNode> ours = parseXml(ours_xml);
    std::unique_ptr<XmlNode> theirs = parseXml(theirs_xml);
    const XmlNode* ours_map = llsdRoot(ours.get(), "map");
    const XmlNode* theirs_map = llsdRoot(theirs.get(), "map");
    if (!ours_map || !theirs_map) return std::string();

    auto out_root = std::make_unique<XmlNode>();
    out_root->tag = "llsd";
    auto out_map = std::make_unique<XmlNode>();
    out_map->tag = "map";
    for (const auto& kv : llsdMap(theirs_map))
    {
        const XmlNode* our_entry = llsdValue(ours_map, kv.first);
        const XmlNode* their_value = llsdValue(kv.second, "Value");
        const XmlNode* our_value = llsdValue(our_entry, "Value");
        if (!our_entry || !their_value || !our_value || their_value->tag != our_value->tag || !llsdScalarOk(*their_value))
        {
            ++dropped;
            continue;
        }
        auto key = std::make_unique<XmlNode>();
        key->tag = "key";
        key->text = kv.first;
        out_map->children.push_back(std::move(key));
        std::unique_ptr<XmlNode> entry = cloneNode(*our_entry);
        for (size_t i = 0; i + 1 < entry->children.size(); ++i)
        {
            if (entry->children[i]->tag == "key" && entry->children[i]->text == "Value")
            {
                entry->children[i + 1]->text = trim(their_value->text);
                break;
            }
        }
        out_map->children.push_back(std::move(entry));
        ++kept;
    }
    if (!kept) return std::string();
    out_root->children.push_back(std::move(out_map));
    return writeXml(*out_root);
}

std::string filterFonts(const std::string& ours_xml, const std::string& theirs_xml, int& kept, int& dropped)
{
    // A skin's fonts.xml replaces ours outright (the registry reads the most specific one), so
    // the result is ours with only the known font sizes taken from theirs. Font faces are not
    // imported: their files may not exist here.
    kept = dropped = 0;
    std::unique_ptr<XmlNode> ours = parseXml(ours_xml);
    std::unique_ptr<XmlNode> theirs = parseXml(theirs_xml);
    if (!ours || !theirs || ours->tag != "fonts" || theirs->tag != "fonts") return std::string();
    std::map<std::string, std::string> sizes;
    for (const auto& c : theirs->children)
    {
        const std::string* name = c->attr("name");
        if (c->tag == "font_size" && name)
        {
            const std::string* size = c->attr("size");
            double d = 0;
            if (size && parseDouble(trim(*size), d) && d >= 4.0 && d <= 72.0) sizes[*name] = trim(*size);
            else ++dropped;
        }
        else if (c->tag == "font")
        {
            ++dropped;
        }
    }
    for (auto& c : ours->children)
    {
        const std::string* name = c->attr("name");
        if (c->tag != "font_size" || !name) continue;
        const auto it = sizes.find(*name);
        if (it == sizes.end()) continue;
        for (auto& a : c->attrs)
        {
            if (a.first == "size") a.second = it->second;
        }
        sizes.erase(it);
        ++kept;
    }
    dropped += int(sizes.size());   // sizes we do not define
    return kept ? writeXml(*ours) : std::string();
}

// ---------------------------------------------------------------------------------------------
// Firestorm discovery
// ---------------------------------------------------------------------------------------------

namespace
{
bool safeComponent(const std::string& c)
{
    Limits l;
    std::string out;
    return !c.empty() && sanitizeEntryPath(c, l, out) && out == c && c.find('/') == std::string::npos;
}

bool hasSkinFiles(const fs::path& dir)
{
    return isRegular(dir / "colors.xml") || isDir(dir / "textures") || isDir(dir / "xui") || isDir(dir / "themes");
}

std::vector<FsTheme> themesOf(const fs::path& skin_dir)
{
    std::vector<FsTheme> out;
    for (const fs::path& t : listDir(skin_dir / "themes", true))
    {
        const std::string folder = utf8(t.filename());
        if (safeComponent(folder) && (isRegular(t / "colors.xml") || isDir(t / "textures"))) out.push_back({ folder, folder });
    }
    return out;
}
}

std::vector<FsSkin> parseFsSkinsXml(const std::string& xml)
{
    std::vector<FsSkin> out;
    std::unique_ptr<XmlNode> root = parseXml(xml);
    const XmlNode* arr = llsdRoot(root.get(), "array");
    if (!arr) return out;
    for (const auto& m : arr->children)
    {
        if (m->tag != "map") continue;
        FsSkin skin;
        for (const auto& kv : llsdMap(m.get()))
        {
            if (kv.first == "name") skin.name = trim(kv.second->text);
            else if (kv.first == "folder") skin.folder = trim(kv.second->text);
            else if (kv.first == "themes" && kv.second->tag == "array")
            {
                for (const auto& tm : kv.second->children)
                {
                    FsTheme theme;
                    for (const auto& tkv : llsdMap(tm.get()))
                    {
                        if (tkv.first == "name") theme.name = trim(tkv.second->text);
                        else if (tkv.first == "folder") theme.folder = trim(tkv.second->text);
                    }
                    if (theme.folder.empty() || safeComponent(theme.folder))
                    {
                        if (theme.name.empty()) theme.name = theme.folder.empty() ? skin.name : theme.folder;
                        skin.themes.push_back(theme);
                    }
                }
            }
        }
        if (safeComponent(skin.folder))
        {
            if (skin.name.empty()) skin.name = skin.folder;
            out.push_back(skin);
        }
    }
    return out;
}

std::vector<FsSkin> scanSkinsDir(const fs::path& skins_dir)
{
    std::vector<FsSkin> out;
    std::set<std::string> listed;
    std::string xml;
    if (readRegularFile(skins_dir / "skins.xml", MAX_XML_BYTES, xml))
    {
        for (FsSkin skin : parseFsSkinsXml(xml))
        {
            const fs::path dir = skins_dir / fromUtf8(skin.folder);
            if (!isDir(dir)) continue;
            std::vector<FsTheme> themes;
            for (const FsTheme& t : skin.themes)
            {
                if (t.folder.empty() || isDir(dir / "themes" / fromUtf8(t.folder))) themes.push_back(t);
            }
            if (themes.empty()) themes.push_back({ skin.name, std::string() });
            skin.themes = themes;
            listed.insert(lower(skin.folder));
            out.push_back(skin);
        }
    }
    for (const fs::path& d : listDir(skins_dir, true))
    {
        const std::string folder = utf8(d.filename());
        if (listed.count(lower(folder)) || !safeComponent(folder) || folder[0] == '.' || !hasSkinFiles(d)) continue;
        FsSkin skin;
        skin.name = folder;
        skin.folder = folder;
        skin.themes.push_back({ folder, std::string() });
        for (const FsTheme& t : themesOf(d)) skin.themes.push_back(t);
        out.push_back(skin);
    }
    return out;
}

bool looksLikeFsSkinsDir(const fs::path& skins_dir)
{
    if (isRegular(skins_dir / "skins.xml")) return true;
    if (!isRegular(skins_dir / "default" / "textures" / "textures.xml")) return false;
    for (const fs::path& d : listDir(skins_dir, true))
    {
        if (isDir(d / "themes")) return true;
    }
    return false;
}

std::vector<fs::path> findFsInstallSkinsDirs(const std::vector<fs::path>& bases)
{
    std::vector<fs::path> out;
    auto add = [&](const fs::path& skins)
    {
        if (!looksLikeFsSkinsDir(skins)) return;
        std::error_code ec;
        const fs::path canon = fs::weakly_canonical(skins, ec);
        const fs::path& key = ec ? skins : canon;
        if (std::find(out.begin(), out.end(), key) == out.end()) out.push_back(key);
    };
    for (const fs::path& base : bases)
    {
        if (base.empty() || !isDir(base)) continue;
        for (const fs::path& d : listDir(base, true))
        {
            const std::string n = lower(utf8(d.filename()));
            const bool fs_name = n.rfind("firestorm", 0) == 0 || n.rfind("phoenix-firestorm", 0) == 0 ||
                                 n.rfind("phoenix_firestorm", 0) == 0 || n.rfind("phoenixfirestorm", 0) == 0;
            if (!fs_name) continue;
            add(d / "skins");
            add(d / "Contents" / "Resources" / "skins");    // macOS bundle (Firestorm*.app)
            for (const fs::path& sub : listDir(d, true))    // a folder holding the bundle, or bin/
            {
                const std::string s = lower(utf8(sub.filename()));
                if (s.size() > 4 && s.compare(s.size() - 4, 4, ".app") == 0) add(sub / "Contents" / "Resources" / "skins");
            }
        }
    }
    return out;
}

std::vector<Layer> fsLayers(const fs::path& skins_dir, const std::string& skin_folder, const std::string& theme_folder,
                            const fs::path& fallback_default)
{
    std::vector<Layer> layers;
    if (!safeComponent(skin_folder) || (!theme_folder.empty() && !safeComponent(theme_folder))) return layers;
    fs::path def = skins_dir / "default";
    if (!isDir(def)) def = fallback_default;
    if (!def.empty() && isDir(def) && lower(skin_folder) != "default") layers.push_back({ def, true, "Firestorm default" });
    const fs::path skin = skins_dir / fromUtf8(skin_folder);
    if (isDir(skin)) layers.push_back({ skin, lower(skin_folder) == "default" && !theme_folder.empty(), skin_folder });
    if (!theme_folder.empty())
    {
        const fs::path theme = skin / "themes" / fromUtf8(theme_folder);
        if (isDir(theme)) layers.push_back({ theme, false, skin_folder + "/themes/" + theme_folder });
    }
    return layers;
}

SourceGuess classifySource(const fs::path& dir)
{
    SourceGuess g;
    fs::path root = dir;
    auto has_marker = [](const fs::path& d)
    {
        return hasSkinFiles(d) || isRegular(d / "skins.xml") || isRegular(d / "manifest.json");
    };
    for (int step = 0; step < 2 && isDir(root) && !has_marker(root); ++step)
    {
        std::vector<fs::path> subs;
        for (const fs::path& d : listDir(root, true))
        {
            const std::string n = utf8(d.filename());
            if (n == "__MACOSX" || n[0] == '.') continue;
            subs.push_back(d);
        }
        if (subs.size() != 1) break;
        root = subs.front();
    }
    g.root = root;
    if (!isDir(root)) return g;

    const fs::path parent = root.parent_path();
    if (isRegular(root / "skins.xml") || (looksLikeFsSkinsDir(root) && !hasSkinFiles(root)))
    {
        g.kind = SourceKind::FsSkinsDir;
        g.skins_dir = root;
        return g;
    }
    if (utf8(parent.filename()) == "themes" && (isRegular(root / "colors.xml") || isDir(root / "textures")))
    {
        g.kind = SourceKind::FsTheme;
        g.theme_folder = utf8(root.filename());
        const fs::path skin = parent.parent_path();
        g.skin_folder = utf8(skin.filename());
        g.skins_dir = skin.parent_path();
        return g;
    }
    if (isDir(root / "themes") || (hasSkinFiles(root) && !parent.empty() && looksLikeFsSkinsDir(parent)))
    {
        g.kind = SourceKind::FsSkin;
        g.skin_folder = utf8(root.filename());
        g.skins_dir = parent;
        return g;
    }
    if (hasSkinFiles(root)) g.kind = SourceKind::OurSkin;
    return g;
}

// ---------------------------------------------------------------------------------------------
// Unpacking
// ---------------------------------------------------------------------------------------------

bool checkArchiveEntry(const ArchiveEntry& entry, const Limits& limits, EntryBudget& budget, std::string& rel,
                       std::string& why, bool* abort)
{
    rel.clear();
    why.clear();
    if (abort) *abort = false;
    if (entry.symlink)
    {
        why = "symbolic link";
        return false;
    }
    std::string clean;
    if (!sanitizeEntryPath(entry.raw_name, limits, clean, &why)) return false;
    if (entry.dir) return true;
    if (clean.rfind("__MACOSX/", 0) == 0 || clean == "__MACOSX" || clean.find("/.") != std::string::npos || clean[0] == '.')
    {
        why = "hidden or system file";
        return false;
    }
    if (!isAllowedSkinFile(clean))
    {
        why = "not a skin file type";
        return false;
    }
    if (entry.size > limits.max_file)
    {
        why = "file too large";
        return false;
    }
    if (budget.files + 1 > limits.max_files)
    {
        why = "too many files";
        if (abort) *abort = true;
        return false;
    }
    if (budget.bytes + entry.size > limits.max_total)
    {
        why = "archive too large";
        if (abort) *abort = true;
        return false;
    }
    ++budget.files;
    budget.bytes += entry.size;
    rel = clean;
    return true;
}

bool unpackZip(const fs::path& zip, const fs::path& dest, const Limits& limits, Report& report, std::string& error)
{
    unzFile uf = unzOpen64(utf8(zip).c_str());
    if (!uf)
    {
        error = "not a zip archive, or it is damaged";
        return false;
    }
    struct Closer
    {
        unzFile f;
        ~Closer() { unzClose(f); }
    } closer{ uf };

    EntryBudget budget;
    uint64_t written_total = 0;
    size_t extracted = 0;
    size_t visited = 0;
    std::set<std::string> seen;
    std::vector<char> buf(64 * 1024);

    int rc = unzGoToFirstFile(uf);
    if (rc == UNZ_END_OF_LIST_OF_FILE)
    {
        error = "the archive is empty";
        return false;
    }
    while (rc == UNZ_OK)
    {
        if (++visited > limits.max_files * 2 + 16)
        {
            error = "too many entries in the archive";
            return false;
        }
        char name[1024] = {};
        unz_file_info64 info;
        if (unzGetCurrentFileInfo64(uf, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0) != UNZ_OK)
        {
            error = "the archive is damaged (bad entry header)";
            return false;
        }
        ArchiveEntry entry;
        entry.raw_name = name;
        entry.size = info.uncompressed_size;
        entry.dir = !entry.raw_name.empty() && (entry.raw_name.back() == '/' || entry.raw_name.back() == '\\');
        entry.symlink = ((info.external_fa >> 16) & 0170000) == 0120000;
        if (info.size_filename >= sizeof(name) - 1)
        {
            ++report.entries_rejected;
            report.skip(entry.raw_name.substr(0, 64) + "...", "name too long");
            rc = unzGoToNextFile(uf);
            continue;
        }
        if (info.flag & 1)
        {
            error = "the archive is encrypted";
            return false;
        }

        std::string rel, why;
        bool abort = false;
        if (!checkArchiveEntry(entry, limits, budget, rel, why, &abort))
        {
            if (abort)
            {
                error = why;
                return false;
            }
            ++report.entries_rejected;
            report.skip(entry.raw_name, why);
            rc = unzGoToNextFile(uf);
            continue;
        }
        if (entry.dir || rel.empty())
        {
            rc = unzGoToNextFile(uf);
            continue;
        }
        if (!seen.insert(lower(rel)).second)
        {
            ++report.entries_rejected;
            report.skip(entry.raw_name, "duplicate entry");
            rc = unzGoToNextFile(uf);
            continue;
        }

        const fs::path out_path = dest / fromUtf8(rel);
        std::error_code ec;
        fs::create_directories(out_path.parent_path(), ec);
        if (ec || !isDir(out_path.parent_path()))
        {
            error = "could not create a folder for " + rel;
            return false;
        }
        if (unzOpenCurrentFile(uf) != UNZ_OK)
        {
            error = "the archive is damaged (" + rel + ")";
            return false;
        }
        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            unzCloseCurrentFile(uf);
            error = "could not write " + rel;
            return false;
        }
        uint64_t written = 0;
        while (true)
        {
            const int n = unzReadCurrentFile(uf, buf.data(), unsigned(buf.size()));
            if (n < 0)
            {
                unzCloseCurrentFile(uf);
                error = "the archive is damaged (" + rel + ")";
                return false;
            }
            if (n == 0) break;
            written += uint64_t(n);
            written_total += uint64_t(n);
            if (written > limits.max_file || written_total > limits.max_total)
            {
                unzCloseCurrentFile(uf);
                error = "the archive unpacks to more than its headers say (" + rel + ")";
                return false;
            }
            out.write(buf.data(), n);
            if (!out)
            {
                unzCloseCurrentFile(uf);
                error = "could not write " + rel;
                return false;
            }
        }
        out.close();
        if (unzCloseCurrentFile(uf) != UNZ_OK)
        {
            error = "the archive is damaged (checksum of " + rel + ")";
            return false;
        }
        ++extracted;
        rc = unzGoToNextFile(uf);
    }
    if (rc != UNZ_END_OF_LIST_OF_FILE)
    {
        error = "the archive is damaged (entry list)";
        return false;
    }
    if (!extracted)
    {
        error = "no skin files in the archive";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------------------------

namespace
{
bool validLanguageDir(const std::string& s)
{
    if (s.size() < 2 || s.size() > 6) return false;
    for (char c : s)
    {
        if (!((c >= 'a' && c <= 'z') || c == '-')) return false;
    }
    return true;
}

void importLayout(const std::vector<Layer>& layers, const OurSkin& ours, const BuildOptions& options,
                  const fs::path& out_dir, Report& report, bool& wrote)
{
    std::set<std::string> done;
    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
    {
        if (it->base) continue;
        size_t budget = options.limits.max_files;
        std::vector<std::string> files;
        walkFiles(it->dir / "xui", std::string(), 0, budget, files);
        for (const std::string& f : files)
        {
            if (extensionOf(f) != "xml") continue;
            std::string rel;
            const size_t slash = f.find('/');
            if (!sanitizeEntryPath(f, options.limits, rel) || slash == std::string::npos || !validLanguageDir(f.substr(0, slash)))
            {
                ++report.xui_skipped;
                report.skip("xui/" + f, "not in a language folder");
                continue;
            }
            if (!done.insert(lower(rel)).second) continue;
            const std::string lang = rel.substr(0, slash);
            const std::string file = rel.substr(slash + 1);
            std::string theirs_text;
            if (!readRegularFile(it->dir / "xui" / fromUtf8(rel), MAX_XML_BYTES, theirs_text))
            {
                ++report.xui_skipped;
                report.skip("xui/" + rel, "unreadable");
                continue;
            }
            if (file == "fonts.xml")
            {
                std::string ours_text;
                int kept = 0, dropped = 0;
                std::string merged;
                if (lang == "en" && readRegularFile(ours.default_dir / "xui" / "en" / "fonts.xml", MAX_XML_BYTES, ours_text))
                    merged = filterFonts(ours_text, theirs_text, kept, dropped);
                if (merged.empty())
                {
                    ++report.xui_skipped;
                    report.skip("xui/" + rel, "no font sizes we use");
                    continue;
                }
                writeFile(out_dir / "xui" / "en" / "fonts.xml", merged);
                report.converted.push_back("xui/en/fonts.xml: " + std::to_string(kept) + " font sizes (faces not imported)");
                ++report.xui_kept;
                wrote = true;
                continue;
            }
            // Ours: the same language, else (for translations) our English file.
            std::string ours_text;
            fs::path ours_path = ours.default_dir / "xui" / fromUtf8(rel);
            if (!isRegular(ours_path)) ours_path = ours.default_dir / "xui" / "en" / fromUtf8(file);
            XuiInfo ours_info;
            if (readRegularFile(ours_path, MAX_XML_BYTES, ours_text)) ours_info = inspectXui(ours_text);
            const XuiInfo theirs_info = inspectXui(theirs_text);
            // A translation only relabels: the names check applies to English files.
            static const std::set<std::string> none;
            const XuiVerdict v = validateXui(ours_info, theirs_info, lang == "en" ? ours.looked_up : none);
            if (!v.ok)
            {
                ++report.xui_skipped;
                report.skip("xui/" + rel, v.reason);
                continue;
            }
            writeFile(out_dir / "xui" / fromUtf8(rel), theirs_text);
            ++report.xui_kept;
            wrote = true;
        }
    }
}
}

bool buildSkin(const std::vector<Layer>& layers, const OurSkin& ours, const BuildOptions& options,
               const ImageCheck& check, const Manifest& manifest, const fs::path& out_dir, Report& report)
{
    bool wrote = false;

    // Colours
    std::vector<std::vector<ColorDef>> color_layers;
    for (const Layer& layer : layers)
    {
        std::string text;
        if (!readRegularFile(layer.dir / "colors.xml", MAX_XML_BYTES, text)) continue;
        bool ok = false;
        std::vector<ColorDef> defs = parseColors(text, &ok);
        if (!ok)
        {
            report.skip(layer.label + "/colors.xml", "does not parse");
            continue;
        }
        color_layers.push_back(std::move(defs));
    }
    if (!color_layers.empty())
    {
        const ColorResult colors = convertColors(color_layers, ours.colors, firestormColorAliases());
        report.colors_kept = int(colors.out.size()) - int(colors.aliased.size());
        report.colors_aliased = int(colors.aliased.size());
        report.colors_dropped = int(colors.dropped.size());
        report.colors_invalid = int(colors.invalid.size());
        for (const std::string& a : colors.aliased) report.converted.push_back("colour " + a);
        for (const std::string& i : colors.invalid) report.skip("colour " + i.substr(0, i.find(':')), i.substr(i.find(':') + 2));
        if (!colors.out.empty())
        {
            writeFile(out_dir / "colors.xml", colorsXml(colors));
            wrote = true;
        }
    }

    // Textures
    std::map<std::string, TextureDef> theirs;
    for (const Layer& layer : layers)
    {
        std::string text;
        if (!readRegularFile(layer.dir / "textures" / "textures.xml", MAX_XML_BYTES, text)) continue;
        bool ok = false;
        std::map<std::string, TextureDef> defs = parseTextures(text, &ok);
        if (!ok)
        {
            report.skip(layer.label + "/textures/textures.xml", "does not parse");
            continue;
        }
        for (auto& kv : defs) theirs[kv.first] = kv.second;
    }
    TexturePlan plan = planTextures(layers, ours.textures, theirs, check, options.limits);
    std::set<std::string> copied;
    std::vector<TexturePlanItem> kept_items;
    for (const TexturePlanItem& item : plan.items)
    {
        const fs::path dst = out_dir / "textures" / fromUtf8(item.rel);
        if (copied.insert(lower(item.rel)).second)
        {
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(item.src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                plan.fallback.push_back(item.name + " (" + item.rel + "): could not copy");
                copied.erase(lower(item.rel));
                continue;
            }
        }
        kept_items.push_back(item);
    }
    plan.items = kept_items;
    report.textures_kept = int(plan.items.size());
    report.textures_fallback = int(plan.fallback.size());
    report.textures_unknown = int(plan.unknown.size());
    report.textures_base = plan.same_as_base;
    for (const std::string& f : plan.fallback) report.skip("texture " + f.substr(0, f.find(':')), f.substr(f.find(':') + 2) + " (ours kept)");
    for (const std::string& u : plan.unknown) report.skip("image " + u, "no texture of ours uses it");
    if (!plan.items.empty())
    {
        writeFile(out_dir / "textures" / "textures.xml", texturesXml(plan));
        wrote = true;
    }

    // skin_settings.xml: the most specific layer that has one
    for (auto it = layers.rbegin(); it != layers.rend(); ++it)
    {
        std::string theirs_text, ours_text;
        if (!readRegularFile(it->dir / "skin_settings.xml", MAX_XML_BYTES, theirs_text)) continue;
        if (readRegularFile(ours.default_dir / "skin_settings.xml", MAX_XML_BYTES, ours_text))
        {
            const std::string out = filterSkinSettings(ours_text, theirs_text, report.settings_kept, report.settings_dropped);
            if (!out.empty())
            {
                writeFile(out_dir / "skin_settings.xml", out);
                wrote = true;
            }
        }
        break;
    }

    // Layout files
    if (options.include_layout)
    {
        importLayout(layers, ours, options, out_dir, report, wrote);
    }
    else
    {
        size_t count = 0;
        for (const Layer& layer : layers)
        {
            if (layer.base) continue;
            size_t budget = options.limits.max_files;
            std::vector<std::string> files;
            walkFiles(layer.dir / "xui", std::string(), 0, budget, files);
            count += files.size();
        }
        if (count)
        {
            report.notes.push_back(std::to_string(count) +
                                   " layout (XUI) files not imported: safe mode. Use \"Include layout files\" to try them.");
        }
    }

    if (!wrote)
    {
        report.skip("skin", "nothing usable: no colours, textures or layout files we could import");
        return false;
    }
    Manifest m = manifest;
    writeFile(out_dir / "manifest.json", manifestJson(m));
    return true;
}

void removeTree(const fs::path& dir)
{
    std::error_code ec;
    fs::remove_all(dir, ec);
}
}
