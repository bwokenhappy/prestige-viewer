/**
 * @file slskinimportcore.h
 * @brief Skin importer (#1599): safe unpacking, Firestorm skin + theme discovery and the
 *        conversion of another viewer's skin into one of ours (colours, textures, optional
 *        layout files). Plain C++17 plus expat / minizip (no viewer types) so the rules can be
 *        tested on their own (scripts/check/test_skin_import.cpp); slskinimport.cpp drives it.
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

#ifndef SL_SKINIMPORTCORE_H
#define SL_SKINIMPORTCORE_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace slskin
{
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------------
// Limits and the import report
// ---------------------------------------------------------------------------------------------

struct Limits
{
    size_t   max_files      = 6000;                   // files unpacked / read from one source
    uint64_t max_total      = 256ull * 1024 * 1024;   // bytes unpacked / read from one source
    uint64_t max_file       = 32ull * 1024 * 1024;    // bytes in one file
    size_t   max_depth      = 12;                     // path components in one entry
    size_t   max_path       = 240;                    // bytes in one entry path
};

// What happened, for the summary dialog and the log (tag "SkinImport").
struct Report
{
    std::string skin_name;      // the installed skin's display name
    std::string folder;         // its folder under the user skins directory
    std::string source;         // where it came from (path or "Firestorm <skin> / <theme>")

    int colors_kept = 0, colors_aliased = 0, colors_dropped = 0, colors_invalid = 0;
    int textures_kept = 0, textures_fallback = 0, textures_unknown = 0, textures_base = 0;
    int xui_kept = 0, xui_skipped = 0;
    int settings_kept = 0, settings_dropped = 0;
    int entries_rejected = 0;   // unsafe or unexpected archive / folder entries

    std::vector<std::string> converted;   // "FriendsChatColor -> FriendChatColor"
    std::vector<std::string> skipped;     // "<what>: <why>"
    std::vector<std::string> notes;       // anything else worth showing

    void skip(const std::string& what, const std::string& why);
    // Short multi-line text for the dialog (lists cut to `max_lines` each); full text for the log.
    std::string summary(size_t max_lines) const;
};

// ---------------------------------------------------------------------------------------------
// Paths and names
// ---------------------------------------------------------------------------------------------

// Normalise one archive / folder entry path to a safe relative path ("a/b/c.png").
// Rejects: empty, absolute ("/x", "\\x", "C:x", "//server"), any ".." component, NUL / control
// characters, ':' (drive letters, NTFS streams), Windows device names (CON, NUL, COM1, ...),
// components ending in '.' or ' ', too deep or too long. "./" components and repeated
// separators are dropped; '\\' counts as a separator.
bool sanitizeEntryPath(const std::string& raw, const Limits& limits, std::string& out, std::string* why = nullptr);

// Whether a (sanitised) relative path may be unpacked from a skin archive at all: skin XML /
// JSON / text files and images in the formats we decode. Everything else is skipped.
bool isAllowedSkinFile(const std::string& rel);

// A folder name for a skin's display name: lower case ASCII letters, digits and '_', no
// leading dot, at most 48 bytes; "skin" when nothing is left.
std::string safeFolderName(const std::string& display_name);
// Folder names we never install into: our own skins ("default" would override every skin),
// and anything starting with a dot (our staging folders).
bool isReservedFolderName(const std::string& folder, const std::set<std::string>& system_skins);
// base, base_2, base_3 ... (the first one `taken` says is free).
std::string uniqueFolderName(const std::string& base, const std::function<bool(const std::string&)>& taken);

// ---------------------------------------------------------------------------------------------
// Manifest (manifest.json)
// ---------------------------------------------------------------------------------------------

struct Manifest
{
    std::string name, author, url, version, notes, compatibility, date;
    std::string source_viewer;      // "Firestorm", "Alchemy", ... (optional "viewer" field)
    bool generated = false;         // made up by the importer (no usable manifest in the source)
};

// Read a manifest. False when the text is not a JSON object or has no non-empty "name".
// Optional fields: author, url, version, notes, compatibility, date, viewer / source_viewer.
bool parseManifest(const std::string& json, Manifest& out);
// The manifest to install: the source's own when it parses, else one generated from
// `fallback_name` (folder or zip name). `source_viewer` fills an empty viewer field.
Manifest resolveManifest(const std::string* json, const std::string& fallback_name, const std::string& source_viewer);
std::string manifestJson(const Manifest& manifest);
std::string jsonEscape(const std::string& text);
// "starlight_silver-blue.zip" -> "starlight silver-blue"; "My Skin/" -> "My Skin".
std::string nameFromPath(const std::string& path);

// ---------------------------------------------------------------------------------------------
// A small XML tree (expat)
// ---------------------------------------------------------------------------------------------

struct XmlNode
{
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;   // document order
    std::string text;                                          // character data, trimmed
    std::vector<std::unique_ptr<XmlNode>> children;

    const std::string* attr(const std::string& key) const;
};
// Parse a document. Null on a parse error (message in *error) or past the limits.
std::unique_ptr<XmlNode> parseXml(const std::string& text, std::string* error = nullptr);
std::string xmlEscape(const std::string& text);
std::string writeXml(const XmlNode& root);   // with an XML declaration

// Read a regular file (never through a symlink), at most `cap` bytes. False otherwise.
bool readRegularFile(const fs::path& path, uint64_t cap, std::string& out);

// ---------------------------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------------------------

struct ColorDef
{
    std::string name, value, reference;   // value: "r g b a"; or a reference to another name
};
std::vector<ColorDef> parseColors(const std::string& xml, bool* ok = nullptr);
// "0.1 .2 1" -> "0.1 0.2 1 1". Three or four finite numbers in 0..1. False otherwise.
bool normalizeColorValue(const std::string& in, std::string& out);

struct ColorResult
{
    std::map<std::string, ColorDef> out;    // our name -> value (or a reference to one of OUR names)
    std::vector<std::string> aliased;       // "TheirName -> OurName"
    std::vector<std::string> dropped;       // their names we do not use
    std::vector<std::string> invalid;       // "<name>: <why>"
};
// layers: most general first (FS default, skin root, theme). References are resolved across
// all layers (cycles and dead ends count as invalid); a reference to a name only WE define is
// kept as a reference. Then: our names are kept, aliased names map to ours (when the source
// does not define our name itself), everything else is dropped.
ColorResult convertColors(const std::vector<std::vector<ColorDef>>& layers, const std::set<std::string>& ours,
                          const std::map<std::string, std::string>& aliases);
// Known Firestorm colour names that mean one of ours.
const std::map<std::string, std::string>& firestormColorAliases();
std::string colorsXml(const ColorResult& colors);

// ---------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------

struct TextureDef
{
    std::string name, file_name;
    std::map<std::string, std::string> scale;   // scale.left / .right / .top / .bottom (integers)
    bool has_vector = false;
};
std::map<std::string, TextureDef> parseTextures(const std::string& xml, bool* ok = nullptr);

// One directory of a source skin (holds colors.xml, textures/, xui/ ...).
struct Layer
{
    fs::path    dir;
    bool        base = false;   // the other viewer's own default skin: colours come along, textures do not
    std::string label;          // "Firestorm default", "starlight", "starlight/themes/silver_blue"
};

// Image check: true when the file decodes in a format we load. The viewer passes an LLImage
// decode; tests and the fallback use sniffImage (magic bytes + size).
using ImageCheck = std::function<bool(const fs::path& file, std::string& why)>;
bool sniffImage(const fs::path& file, uint64_t cap, std::string& why);

struct TexturePlanItem
{
    std::string name;       // our texture name
    std::string rel;        // where it goes, relative to textures/ (sanitised)
    fs::path    src;
    std::map<std::string, std::string> scale;
};
struct TexturePlan
{
    std::vector<TexturePlanItem> items;
    std::vector<std::string> fallback;   // "<name>: <why>" (ours stays)
    std::vector<std::string> unknown;    // image files in the source no name of ours points at
    int same_as_base = 0;                // names the source only has from its base skin (ours stays)
};
// For each texture name we define, find the source's image (their textures.xml file_name, or
// ours when they have no entry) in the most specific layer; only non-base layers count. A
// missing, unsafe or undecodable image leaves ours in place.
TexturePlan planTextures(const std::vector<Layer>& layers, const std::map<std::string, TextureDef>& ours,
                         const std::map<std::string, TextureDef>& theirs, const ImageCheck& check, const Limits& limits);
// The textures.xml overlay: one entry per imported image, with vector_file="" so the bitmap
// shows instead of our vector icon, and the source's scale.* (its own 9-slice).
std::string texturesXml(const TexturePlan& plan);

// ---------------------------------------------------------------------------------------------
// Layout files (XUI)
// ---------------------------------------------------------------------------------------------

struct XuiInfo
{
    bool parsed = false;
    std::string error;
    std::string root_tag, root_name;
    std::set<std::string> names;    // every name="..." in the file
};
XuiInfo inspectXui(const std::string& text);

struct XuiVerdict
{
    bool ok = false;
    std::string reason;
    std::vector<std::string> missing;   // looked-up names ours has and theirs lacks
};
// Theirs must parse, ours must exist (same file name), the root element and its name must
// match, and every name in ours that our code looks up (looked_up) must still be there.
XuiVerdict validateXui(const XuiInfo& ours, const XuiInfo& theirs, const std::set<std::string>& looked_up);
// The looked-up names list (app_settings/xui_lookup_names.txt): one per line, '#' comments.
std::set<std::string> parseLookupNames(const std::string& text);

// ---------------------------------------------------------------------------------------------
// skin_settings.xml and fonts.xml: known keys only
// ---------------------------------------------------------------------------------------------

// Keep the source's Value for keys our skin_settings.xml defines, when its LLSD type matches
// ours; everything else comes from ours. Empty when nothing is kept.
std::string filterSkinSettings(const std::string& ours_xml, const std::string& theirs_xml, int& kept, int& dropped);
// Keep <font> / <font_size> entries whose name we define; <file> entries must be bare file names.
std::string filterFonts(const std::string& ours_xml, const std::string& theirs_xml, int& kept, int& dropped);

// ---------------------------------------------------------------------------------------------
// Firestorm discovery
// ---------------------------------------------------------------------------------------------

struct FsTheme
{
    std::string name;       // "Silver Blue"
    std::string folder;     // "silver_blue"; empty = the skin root itself
};
struct FsSkin
{
    std::string name;       // "Starlight"
    std::string folder;     // "starlight"
    std::vector<FsTheme> themes;
};
struct FsSkinsDir
{
    fs::path    dir;            // .../skins
    std::string label;          // "Firestorm (C:\Program Files\Firestorm-Releasex64)"
    bool        user = false;   // a per-user skins folder (modifications), not an install
    std::vector<FsSkin> skins;
};

// FS skins.xml (LLSD array of {name, folder, themes: [{name, folder}]}).
std::vector<FsSkin> parseFsSkinsXml(const std::string& xml);
// The skins in a skins directory: skins.xml when present, plus folders it does not list
// (themes from their themes/ subfolders). Folders without skin files are ignored.
std::vector<FsSkin> scanSkinsDir(const fs::path& skins_dir);
// Install-like: skins.xml, or default/ with textures/textures.xml and some skin with themes/.
bool looksLikeFsSkinsDir(const fs::path& skins_dir);

// Where Firestorm installs may be, per OS: directories named Firestorm* / Phoenix-Firestorm*
// under each base (Program Files, ~, /opt, ~/Applications, /Applications); a macOS bundle's
// skins are in Contents/Resources/skins. Returns existing skins directories.
std::vector<fs::path> findFsInstallSkinsDirs(const std::vector<fs::path>& bases);

// The layers to read for skin + theme in an FS skins directory: FS default (base), the skin
// root (base when it is "default"), the theme folder. `fallback_default`: an install's
// skins/default to use when skins_dir has none (a per-user skins folder).
std::vector<Layer> fsLayers(const fs::path& skins_dir, const std::string& skin_folder, const std::string& theme_folder,
                            const fs::path& fallback_default = fs::path());

// What a picked folder (or unpacked archive) is.
enum class SourceKind
{
    Unknown,
    OurSkin,            // colors.xml / textures / xui at its root (Slipstream, Alchemy, LL layout)
    FsSkinsDir,         // a whole skins directory (skins.xml)
    FsSkin,             // one FS skin folder (has themes/)
    FsTheme,            // one FS theme folder (inside <skin>/themes/)
};
struct SourceGuess
{
    SourceKind kind = SourceKind::Unknown;
    fs::path   root;            // after stepping into a single wrapper folder
    fs::path   skins_dir;       // FS: the skins directory around it (may be empty)
    std::string skin_folder, theme_folder;
};
SourceGuess classifySource(const fs::path& dir);

// ---------------------------------------------------------------------------------------------
// Unpacking and building
// ---------------------------------------------------------------------------------------------

// One entry as an archive describes it.
struct ArchiveEntry
{
    std::string raw_name;
    uint64_t    size = 0;       // declared uncompressed size (not trusted: reads are counted too)
    bool        dir = false;
    bool        symlink = false;
};
// Accounting across one archive: entry count and declared bytes.
struct EntryBudget
{
    size_t   files = 0;
    uint64_t bytes = 0;
};
// Decide one entry: accepted (rel set) or rejected (why set). Directories are "accepted" with
// an empty rel. Exceeding the budget rejects the entry and sets *abort.
bool checkArchiveEntry(const ArchiveEntry& entry, const Limits& limits, EntryBudget& budget, std::string& rel,
                       std::string& why, bool* abort);

// Unpack a zip into dest (which must exist and be empty) through checkArchiveEntry; reads are
// counted, so a lying header cannot exceed the caps. False (with error) when the archive is
// unreadable or a cap is hit: the caller deletes dest.
bool unpackZip(const fs::path& zip, const fs::path& dest, const Limits& limits, Report& report, std::string& error);

// What we know about our own skin, for the conversion.
struct OurSkin
{
    std::set<std::string> colors;                       // names in skins/default/colors.xml
    std::map<std::string, TextureDef> textures;         // skins/default/textures/textures.xml
    fs::path default_dir;                               // skins/default (xui/, skin_settings.xml)
    std::set<std::string> looked_up;                    // xui_lookup_names.txt
};

struct BuildOptions
{
    bool   include_layout = false;  // also import XUI files that pass validateXui (and fonts.xml)
    Limits limits;
};

// Convert `layers` (most general first) into a skin of ours under out_dir (must exist, empty):
// colors.xml, textures/ (+ textures.xml), skin_settings.xml, xui/ (option), manifest.json.
// False when nothing usable came out (no colours, textures or layout files).
bool buildSkin(const std::vector<Layer>& layers, const OurSkin& ours, const BuildOptions& options,
               const ImageCheck& check, const Manifest& manifest, const fs::path& out_dir, Report& report);

// Remove a directory tree without following symlinks. Never throws.
void removeTree(const fs::path& dir);
}

#endif
