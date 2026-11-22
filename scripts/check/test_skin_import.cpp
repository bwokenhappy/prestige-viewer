// Fixture for the skin importer core (#1599): path sanitising (zip-slip), manifest generation,
// colour aliasing, XUI validation, Firestorm skin + theme discovery and conversion, safe zip
// unpacking. Built and run by scripts/check/checks/skin_import.py, which makes the fake
// Firestorm tree and the archives and passes their folder as argv[1].
#include "slskinimportcore.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace slskin;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_failed; std::cerr << __LINE__ << ": CHECK failed: " #cond "\n"; } } while (0)

static std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void testSanitize()
{
    Limits l;
    std::string out, why;
    CHECK(sanitizeEntryPath("textures/icons/a.png", l, out, &why) && out == "textures/icons/a.png");
    CHECK(sanitizeEntryPath("./textures//b.png", l, out, &why) && out == "textures/b.png");
    CHECK(sanitizeEntryPath("textures\\c.png", l, out, &why) && out == "textures/c.png");
    // zip-slip and absolute paths
    CHECK(!sanitizeEntryPath("../evil.xml", l, out, &why));
    CHECK(!sanitizeEntryPath("textures/../../evil.xml", l, out, &why));
    CHECK(!sanitizeEntryPath("..\\..\\evil.xml", l, out, &why));
    CHECK(!sanitizeEntryPath("/etc/passwd", l, out, &why));
    CHECK(!sanitizeEntryPath("\\windows\\x.xml", l, out, &why));
    CHECK(!sanitizeEntryPath("C:\\x.xml", l, out, &why));
    CHECK(!sanitizeEntryPath("C:x.xml", l, out, &why));
    CHECK(!sanitizeEntryPath("//server/share/x.xml", l, out, &why));
    CHECK(!sanitizeEntryPath("colors.xml:stream", l, out, &why));
    CHECK(!sanitizeEntryPath("textures/CON.png", l, out, &why));
    CHECK(!sanitizeEntryPath("textures/nul", l, out, &why));
    CHECK(!sanitizeEntryPath("trailing./x.png", l, out, &why));
    CHECK(!sanitizeEntryPath(std::string("a\0b.png", 7), l, out, &why));
    CHECK(!sanitizeEntryPath("", l, out, &why));
    CHECK(!sanitizeEntryPath(std::string(300, 'a'), l, out, &why));
    CHECK(!sanitizeEntryPath("a/b/c/d/e/f/g/h/i/j/k/l/m/n.png", l, out, &why));
    CHECK(out.empty());

    CHECK(isAllowedSkinFile("colors.xml") && isAllowedSkinFile("textures/a.TGA") && isAllowedSkinFile("manifest.json"));
    CHECK(!isAllowedSkinFile("evil.exe") && !isAllowedSkinFile("lib.dll") && !isAllowedSkinFile("run.sh") && !isAllowedSkinFile("noext"));

    // archive entries
    EntryBudget budget;
    std::string rel;
    bool abort = false;
    ArchiveEntry e;
    e.raw_name = "../../.bashrc";
    CHECK(!checkArchiveEntry(e, l, budget, rel, why, &abort) && !abort);
    e.raw_name = "textures/link.png";
    e.symlink = true;
    CHECK(!checkArchiveEntry(e, l, budget, rel, why, &abort) && why == "symbolic link");
    e.symlink = false;
    e.raw_name = "__MACOSX/._colors.xml";
    CHECK(!checkArchiveEntry(e, l, budget, rel, why, &abort));
    e.raw_name = "colors.xml";
    e.size = 100;
    CHECK(checkArchiveEntry(e, l, budget, rel, why, &abort) && rel == "colors.xml");
    Limits tiny;
    tiny.max_files = 1;
    EntryBudget b2;
    CHECK(checkArchiveEntry(e, tiny, b2, rel, why, &abort));
    CHECK(!checkArchiveEntry(e, tiny, b2, rel, why, &abort) && abort);
    Limits capped;
    capped.max_total = 150;
    EntryBudget b3;
    CHECK(checkArchiveEntry(e, capped, b3, rel, why, &abort));
    CHECK(!checkArchiveEntry(e, capped, b3, rel, why, &abort) && abort);
    e.size = l.max_file + 1;
    CHECK(!checkArchiveEntry(e, l, budget, rel, why, &abort) && !abort);
}

static void testNames()
{
    CHECK(safeFolderName("FS Starlight - Silver Blue") == "fs_starlight_silver_blue");
    CHECK(safeFolderName("../../x") == "x");
    CHECK(safeFolderName("....") == "skin");
    CHECK(safeFolderName("Ünïcode ☃") == "n_code");
    CHECK(safeFolderName(std::string(100, 'a')).size() == 48);
    std::set<std::string> sys = { "default", "slipstream" };
    CHECK(isReservedFolderName("default", sys) && isReservedFolderName("Slipstream", sys) && isReservedFolderName(".import-1", sys));
    CHECK(!isReservedFolderName("fs_starlight", sys));
    std::set<std::string> taken = { "a", "a_2" };
    CHECK(uniqueFolderName("a", [&](const std::string& s) { return taken.count(s) != 0; }) == "a_3");
    CHECK(uniqueFolderName("b", [&](const std::string& s) { return taken.count(s) != 0; }) == "b");
    CHECK(nameFromPath("/x/y/starlight_silver.zip") == "starlight silver");
    CHECK(nameFromPath("C:\\skins\\My Skin\\") == "My Skin");
}

static void testManifest()
{
    Manifest m;
    CHECK(parseManifest("{\"name\":\"Dark\",\"author\":\"A\",\"version\":\"1.2\",\"viewer\":\"Alchemy\",\"extra\":{\"x\":[1,2]}}", m));
    CHECK(m.name == "Dark" && m.author == "A" && m.version == "1.2" && m.source_viewer == "Alchemy");
    CHECK(parseManifest("\xEF\xBB\xBF{\"name\":\"B\\u00e9\"}", m) && m.name == "B\xC3\xA9");
    CHECK(!parseManifest("{\"author\":\"no name\"}", m));
    CHECK(!parseManifest("{\"name\":\"\"}", m));
    CHECK(!parseManifest("[\"name\"]", m));
    CHECK(!parseManifest("{\"name\":\"x\"", m));
    CHECK(!parseManifest("{\"name\":\"x\"} trailing", m));
    CHECK(!parseManifest("not json", m));

    const std::string bad = "{oops";
    Manifest g = resolveManifest(&bad, "starlight silver", "Firestorm");
    CHECK(g.generated && g.name == "starlight silver" && g.source_viewer == "Firestorm");
    Manifest g2 = resolveManifest(nullptr, "", "");
    CHECK(g2.generated && g2.name == "Imported skin");
    const std::string good = "{\"name\":\"Mine\"}";
    Manifest g3 = resolveManifest(&good, "folder", "Firestorm");
    CHECK(!g3.generated && g3.name == "Mine" && g3.source_viewer == "Firestorm");

    // round trip, with escaping
    Manifest r;
    r.name = "Quote \" and \\ and\nnewline";
    r.author = "Me";
    r.source_viewer = "Firestorm";
    Manifest back;
    r.notes = "line 1\nline 2";
    CHECK(parseManifest(manifestJson(r), back) && back.name == "Quote \" and \\ and newline" && back.source_viewer == "Firestorm");
    CHECK(back.notes == "line 1\nline 2");
}

static void testColors()
{
    std::string v;
    CHECK(normalizeColorValue(".27 .66 1.0", v) && v == "0.27 0.66 1 1");
    CHECK(normalizeColorValue("0,0.5,1,0.25", v) && v == "0 0.5 1 0.25");
    CHECK(!normalizeColorValue("255 0 0 1", v));
    CHECK(!normalizeColorValue("0 0", v));
    CHECK(!normalizeColorValue("0 0 0 0 0", v));
    CHECK(!normalizeColorValue("nan 0 0", v));
    CHECK(!normalizeColorValue("red", v));

    const std::set<std::string> ours = { "EmphasisColor", "FriendChatColor", "TextFgColor", "IconAccentColor", "LabelTextColor" };
    const std::map<std::string, std::string> aliases = { { "FriendsChatColor", "FriendChatColor" }, { "FSThing", "Missing" } };
    std::vector<ColorDef> base = { { "EmphasisColor", "0.1 0.2 0.3 1", "" }, { "SL-Normal", "0.5 0.5 0.5 1", "" },
                                   { "TextFgColor", "", "SL-Normal" } };
    std::vector<ColorDef> theme = { { "FriendsChatColor", "1 0 0 1", "" }, { "LabelTextColor", "", "IconAccentColor" },
                                    { "Loop1", "", "Loop2" }, { "Loop2", "", "Loop1" }, { "EmphasisColor", "", "Loop1" },
                                    { "FSThing", "0 0 0 1", "" }, { "IconAccentColor", "9 9 9", "" } };
    ColorResult r = convertColors({ base }, ours, aliases);
    CHECK(r.out.count("EmphasisColor") && r.out["EmphasisColor"].value == "0.1 0.2 0.3 1");
    CHECK(r.out.count("TextFgColor") && r.out["TextFgColor"].value == "0.5 0.5 0.5 1");   // reference through a dropped name
    CHECK(!r.out.count("SL-Normal") && r.dropped.size() == 1);

    r = convertColors({ base, theme }, ours, aliases);
    CHECK(r.out.count("FriendChatColor") && r.out["FriendChatColor"].value == "1 0 0 1");
    CHECK(r.aliased.size() == 1 && r.aliased[0] == "FriendsChatColor -> FriendChatColor");
    CHECK(!r.out.count("EmphasisColor"));               // the theme turned it into a loop: invalid, ours stays
    CHECK(!r.out.count("IconAccentColor"));             // out of range
    CHECK(!r.out.count("LabelTextColor"));              // references the invalid one
    CHECK(r.invalid.size() == 3);
    CHECK(std::find(r.dropped.begin(), r.dropped.end(), "FSThing") != r.dropped.end());   // alias target we lack

    // A reference to a name only we define stays a reference.
    std::vector<ColorDef> ref = { { "LabelTextColor", "", "EmphasisColor" } };
    r = convertColors({ ref }, ours, aliases);
    CHECK(r.out.count("LabelTextColor") && r.out["LabelTextColor"].reference == "EmphasisColor");
    // The source defines our name itself: the alias is not used.
    std::vector<ColorDef> both = { { "FriendsChatColor", "1 0 0 1", "" }, { "FriendChatColor", "0 1 0 1", "" } };
    r = convertColors({ both }, ours, aliases);
    CHECK(r.out["FriendChatColor"].value == "0 1 0 1" && r.aliased.empty());

    bool ok = false;
    std::vector<ColorDef> parsed = parseColors(colorsXml(r), &ok);
    CHECK(ok && parsed.size() == 1 && parsed[0].name == "FriendChatColor");
    parseColors("<colors><color name='x' value='1 1 1'", &ok);
    CHECK(!ok);
    // entity expansion is refused
    parseColors("<?xml version='1.0'?><!DOCTYPE c [<!ENTITY a 'aaaa'>]><colors><color name='&a;' value='1 1 1'/></colors>", &ok);
    CHECK(!ok);

    CHECK(firestormColorAliases().count("FriendsChatColor"));
}

static void testXui()
{
    const std::set<std::string> looked_up = { "skin_list", "apply_skin", "not_in_file" };
    const XuiInfo ours = inspectXui("<panel name='skins'><scroll_list name='skin_list'/><button name='apply_skin'/><text name='label'/></panel>");
    CHECK(ours.parsed && ours.root_tag == "panel" && ours.root_name == "skins" && ours.names.size() == 4);

    XuiVerdict v = validateXui(ours, inspectXui("<panel name='skins'><scroll_list name='skin_list'/><button name='apply_skin'/></panel>"), looked_up);
    CHECK(v.ok);   // "label" is not looked up by code: may go
    v = validateXui(ours, inspectXui("<panel name='skins'><scroll_list name='skin_list'/></panel>"), looked_up);
    CHECK(!v.ok && v.missing.size() == 1 && v.missing[0] == "apply_skin");
    v = validateXui(ours, inspectXui("<floater name='skins'><scroll_list name='skin_list'/><button name='apply_skin'/></floater>"), looked_up);
    CHECK(!v.ok && v.reason.find("root element") != std::string::npos);
    v = validateXui(ours, inspectXui("<panel name='other'><scroll_list name='skin_list'/><button name='apply_skin'/></panel>"), looked_up);
    CHECK(!v.ok && v.reason.find("root name") != std::string::npos);
    v = validateXui(ours, inspectXui("<panel name='skins'><unclosed></panel>"), looked_up);
    CHECK(!v.ok && v.reason.find("parse") != std::string::npos);
    v = validateXui(XuiInfo(), inspectXui("<panel name='new'/>"), looked_up);
    CHECK(!v.ok && v.reason.find("no layout file") != std::string::npos);

    const std::set<std::string> names = parseLookupNames("# comment\nskin_list\n\n  apply_skin \n");
    CHECK(names.size() == 2 && names.count("apply_skin"));
}

static void testSettingsAndFonts()
{
    const std::string ours =
        "<llsd><map><key>LegacyProfile</key><map><key>Comment</key><string>c</string><key>Type</key><string>Boolean</string>"
        "<key>Value</key><boolean>0</boolean></map>"
        "<key>Count</key><map><key>Type</key><string>S32</string><key>Value</key><integer>3</integer></map></map></llsd>";
    const std::string theirs =
        "<llsd><map><key>LegacyProfile</key><map><key>Value</key><boolean>1</boolean></map>"
        "<key>Count</key><map><key>Value</key><string>x</string></map>"
        "<key>FSUnknown</key><map><key>Value</key><integer>1</integer></map></map></llsd>";
    int kept = 0, dropped = 0;
    const std::string out = filterSkinSettings(ours, theirs, kept, dropped);
    CHECK(kept == 1 && dropped == 2);
    CHECK(out.find("LegacyProfile") != std::string::npos && out.find("<boolean>1</boolean>") != std::string::npos);
    CHECK(out.find("FSUnknown") == std::string::npos && out.find("Count") == std::string::npos);

    const std::string ours_fonts = "<fonts><font name='SansSerif'><file>DejaVuSans.ttf</file></font>"
                                   "<font_size name='Small' size='9'/><font_size name='Large' size='12'/></fonts>";
    const std::string their_fonts = "<fonts><font name='SansSerif'><file>../../evil.ttf</file></font>"
                                    "<font_size name='Small' size='10'/><font_size name='Huge' size='99'/><font_size name='FSNew' size='8'/></fonts>";
    const std::string f = filterFonts(ours_fonts, their_fonts, kept, dropped);
    CHECK(kept == 1 && dropped == 3);
    CHECK(f.find("size=\"10\"") != std::string::npos && f.find("size=\"12\"") != std::string::npos);
    CHECK(f.find("evil") == std::string::npos && f.find("DejaVuSans.ttf") != std::string::npos);
}

static OurSkin loadOurs(const fs::path& ours_dir)
{
    OurSkin o;
    o.default_dir = ours_dir;
    for (const ColorDef& c : parseColors(slurp(ours_dir / "colors.xml"))) o.colors.insert(c.name);
    o.textures = parseTextures(slurp(ours_dir / "textures" / "textures.xml"));
    o.looked_up = { "skin_list", "apply_skin" };
    return o;
}

static void testFirestorm(const fs::path& root)
{
    const fs::path skins = root / "fs_install" / "skins";
    CHECK(looksLikeFsSkinsDir(skins));
    CHECK(!looksLikeFsSkinsDir(root / "ours"));

    std::vector<FsSkin> found = scanSkinsDir(skins);
    // skins.xml lists starlight (two themes, one missing on disk) and a missing skin; "loose" is not listed.
    const FsSkin* starlight = nullptr;
    const FsSkin* loose = nullptr;
    bool ghost = false;
    for (const FsSkin& s : found)
    {
        if (s.folder == "starlight") starlight = &s;
        if (s.folder == "loose") loose = &s;
        if (s.folder == "ghost") ghost = true;
    }
    CHECK(starlight && starlight->name == "Starlight");
    CHECK(starlight && starlight->themes.size() == 2);   // "" (root) + silver_blue; nonexistent theme dropped
    CHECK(loose && loose->themes.size() == 2 && loose->themes[1].folder == "neon");
    CHECK(!ghost);

    // Install discovery from a base directory.
    std::vector<fs::path> installs = findFsInstallSkinsDirs({ root / "programs" });
    CHECK(installs.size() == 2);   // Firestorm-Releasex64/skins and Firestorm.app/Contents/Resources/skins

    // Classification of picked folders.
    SourceGuess g = classifySource(skins / "starlight" / "themes" / "silver_blue");
    CHECK(g.kind == SourceKind::FsTheme && g.skin_folder == "starlight" && g.theme_folder == "silver_blue");
    g = classifySource(skins / "starlight");
    CHECK(g.kind == SourceKind::FsSkin && g.skin_folder == "starlight");
    g = classifySource(skins);
    CHECK(g.kind == SourceKind::FsSkinsDir);
    g = classifySource(root / "wrapped");     // wrapped/MySkin/colors.xml
    CHECK(g.kind == SourceKind::OurSkin && g.root.filename() == "MySkin");
    g = classifySource(root / "empty");
    CHECK(g.kind == SourceKind::Unknown);

    // Layers and the conversion.
    std::vector<Layer> layers = fsLayers(skins, "starlight", "silver_blue");
    CHECK(layers.size() == 3 && layers[0].base && !layers[1].base && !layers[2].base);
    CHECK(fsLayers(skins, "../x", "").empty() && fsLayers(skins, "starlight", "../../y").empty());

    const OurSkin ours = loadOurs(root / "ours" / "skins" / "default");
    const fs::path out = root / "out_fs";
    fs::create_directories(out);
    Report report;
    BuildOptions opts;
    Manifest m = resolveManifest(nullptr, "FS Starlight - Silver Blue", "Firestorm");
    CHECK(buildSkin(layers, ours, opts, ImageCheck(), m, out, report));
    // colours: EmphasisColor from the theme, TextFgColor via FS default's reference, FriendsChatColor aliased
    const std::string colors = slurp(out / "colors.xml");
    CHECK(colors.find("name=\"EmphasisColor\" value=\"0 0 1 1\"") != std::string::npos);
    CHECK(colors.find("name=\"TextFgColor\" value=\"0.5 0.5 0.5 1\"") != std::string::npos);
    CHECK(colors.find("FriendChatColor") != std::string::npos);
    CHECK(colors.find("SL-Normal") == std::string::npos && colors.find("FSOnlyColor") == std::string::npos);
    CHECK(report.colors_aliased == 1 && report.colors_dropped >= 2);
    // textures: Icon_A from the skin root (good PNG); Icon_B from the theme is a broken TGA -> ours;
    // Icon_C only in FS default -> ours; theme's stray.png unused.
    CHECK(fs::exists(out / "textures" / "icons" / "a.png"));
    CHECK(!fs::exists(out / "textures" / "icons" / "b.tga"));
    const std::string tex = slurp(out / "textures" / "textures.xml");
    CHECK(tex.find("name=\"Icon_A\"") != std::string::npos && tex.find("vector_file=\"\"") != std::string::npos);
    CHECK(tex.find("scale.left=\"2\"") != std::string::npos);
    CHECK(tex.find("Icon_B") == std::string::npos && tex.find("Icon_C") == std::string::npos);
    CHECK(report.textures_kept == 1 && report.textures_fallback == 1 && report.textures_base == 1 && report.textures_unknown == 1);
    // layout files: safe mode by default
    CHECK(!fs::exists(out / "xui"));
    CHECK(!report.notes.empty());
    Manifest back;
    CHECK(parseManifest(slurp(out / "manifest.json"), back) && back.name == "FS Starlight - Silver Blue" && back.source_viewer == "Firestorm");

    // With layout files: panel_good.xml passes, panel_test.xml lacks apply_skin, panel_new.xml is not ours.
    const fs::path out2 = root / "out_fs_layout";
    fs::create_directories(out2);
    Report r2;
    opts.include_layout = true;
    CHECK(buildSkin(layers, ours, opts, ImageCheck(), m, out2, r2));
    CHECK(fs::exists(out2 / "xui" / "en" / "panel_good.xml"));
    CHECK(!fs::exists(out2 / "xui" / "en" / "panel_test.xml"));
    CHECK(!fs::exists(out2 / "xui" / "en" / "panel_new.xml"));
    CHECK(r2.xui_kept == 1 && r2.xui_skipped == 2);

    std::cout << r2.summary(5);
}

static void testZips(const fs::path& root)
{
    Limits l;
    const fs::path tmp = root / "unpack";

    // A good zip in our layout, wrapped in a folder.
    fs::create_directories(tmp / "good");
    Report r;
    std::string err;
    CHECK(unpackZip(root / "zips" / "good.zip", tmp / "good", l, r, err));
    CHECK(fs::exists(tmp / "good" / "Alchemy Dark" / "colors.xml"));
    CHECK(classifySource(tmp / "good").kind == SourceKind::OurSkin);

    // zip-slip / absolute / symlink / executable entries are rejected, the rest unpacks, nothing escapes.
    fs::create_directories(tmp / "slip");
    Report rs;
    CHECK(unpackZip(root / "zips" / "slip.zip", tmp / "slip", l, rs, err));
    CHECK(fs::exists(tmp / "slip" / "colors.xml"));
    CHECK(!fs::exists(root / "evil.xml") && !fs::exists(tmp / "evil.xml") && !fs::exists(root / "zips" / "evil.xml"));
    CHECK(!fs::exists(tmp / "slip" / "link.png") && !fs::exists(tmp / "slip" / "run.exe"));
    CHECK(rs.entries_rejected >= 5);

    // A truncated zip and a file that is not a zip fail cleanly.
    fs::create_directories(tmp / "broken");
    Report rb;
    CHECK(!unpackZip(root / "zips" / "broken.zip", tmp / "broken", l, rb, err) && !err.empty());
    fs::create_directories(tmp / "notzip");
    CHECK(!unpackZip(root / "zips" / "notzip.zip", tmp / "notzip", l, rb, err) && !err.empty());

    // Caps: a 200 KB entry against a 100 KB per-file cap is skipped; against a 100 KB total cap the import stops.
    Limits capped;
    capped.max_file = 100 * 1024;
    fs::create_directories(tmp / "big1");
    Report rbig;
    CHECK(unpackZip(root / "zips" / "big.zip", tmp / "big1", capped, rbig, err));
    CHECK(!fs::exists(tmp / "big1" / "textures" / "big.png") && rbig.entries_rejected == 1);
    Limits total;
    total.max_total = 100 * 1024;
    fs::create_directories(tmp / "big2");
    CHECK(!unpackZip(root / "zips" / "big.zip", tmp / "big2", total, rbig, err) && err == "archive too large");
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: test_skin_import <fixture dir>\n";
        return 2;
    }
    const fs::path root = argv[1];
    testSanitize();
    testNames();
    testManifest();
    testColors();
    testXui();
    testSettingsAndFonts();
    testFirestorm(root);
    testZips(root);
    if (g_failed)
    {
        std::cerr << g_failed << " check(s) failed\n";
        return 1;
    }
    std::cout << "skin import: all checks passed\n";
    return 0;
}
