// Fixture for the settings backup core (#1423): secret filtering, the allowlist, archive
// validation (path traversal, links, format / version stamp, sizes), staged restore. Built and
// run by scripts/check/checks/settings_backup.py, which makes the fake user folders and the
// hostile archives and passes their folder as argv[1].
#include "slsettingsbackupcore.h"

#include <minizip-ng/unzip.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace slbackup;

static int g_failed = 0;
#define CHECK(cond) do { if (!(cond)) { ++g_failed; std::cerr << __LINE__ << ": CHECK failed: " #cond "\n"; } } while (0)

static std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void spit(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

static bool has(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Every entry name and the full text of every entry of a zip.
static bool readZip(const fs::path& zip, std::vector<std::string>& names, std::string& all_text)
{
    unzFile uf = unzOpen64(utf8(zip).c_str());
    if (!uf) return false;
    int rc = unzGoToFirstFile(uf);
    while (rc == UNZ_OK)
    {
        char name[1024] = {};
        unz_file_info64 info;
        unzGetCurrentFileInfo64(uf, &info, name, sizeof(name) - 1, nullptr, 0, nullptr, 0);
        names.push_back(name);
        if (unzOpenCurrentFile(uf) == UNZ_OK)
        {
            char buf[4096];
            int n;
            while ((n = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) all_text.append(buf, size_t(n));
            unzCloseCurrentFile(uf);
        }
        rc = unzGoToNextFile(uf);
    }
    unzClose(uf);
    return true;
}

static void testNames()
{
    for (const char* s : { "UserLoginInfo", "MFAHash", "GoogleTranslateAPIKey", "DeepLTranslateAPIKey", "RememberPassword",
                           "bin_conf.dat", "password.dat", "testctl.token", "user_login_info", "SomeOAuthThing", "mfa_hash.xml" })
    {
        if (!isSecretName(s)) { ++g_failed; std::cerr << "not secret: " << s << "\n"; }
    }
    for (const char* s : { "RenderVKAvatarAnimFarM", "WeatherParcelSnowRateFromFall", "CookiesEnabled", "Socks5AuthType",
                           "settings.xml", "FSKeywords", "toolbars.xml" })
    {
        if (isSecretName(s)) { ++g_failed; std::cerr << "secret by mistake: " << s << "\n"; }
    }

    std::string why;
    CHECK(isSafePath("user_settings/presets/graphic/Fast.xml", why));
    CHECK(isSafePath("chat_logs/Bob Smith.txt", why));
    for (const char* s : { "../evil.xml", "user_settings/../../evil.xml", "/etc/passwd", "user_settings\\..\\evil.xml",
                           "C:/evil.xml", "C:evil.xml", "user_settings//settings.xml", "user_settings/./settings.xml",
                           "user_settings/settings.xml:stream", "user_settings/CON.xml", "user_settings/nul",
                           "chat_logs/com1.txt", "trailing./x.xml", "" })
    {
        if (isSafePath(s, why)) { ++g_failed; std::cerr << "unsafe path accepted: " << s << "\n"; }
    }
    CHECK(!isSafePath(std::string("user_settings/a\0b.xml", 22), why));

    CHECK(isAllowedPath("user_settings/settings.xml", why));
    CHECK(isAllowedPath("user_settings/presets/camera/Front.xml", why));
    CHECK(isAllowedPath("user_settings/poses/mine/sit.xml", why));
    CHECK(isAllowedPath("account/settings_per_account.xml", why));
    CHECK(isAllowedPath("account/toolbars.xml", why));
    CHECK(isAllowedPath("chat_logs/conversation.log", why));
    CHECK(isAllowedPath("chat_logs/tokenmaster_resident.txt", why));
    for (const char* s : { "user_settings/bin_conf.dat", "user_settings/password.dat", "user_settings/testctl.token",
                           "user_settings/telemetry/queue/a.json", "user_settings/presets/graphic/run.exe",
                           "user_settings/presets/Fast.xml", "user_settings/filed_reports.xml", "account/typed_locations.txt",
                           "account/url_history.xml", "chat_logs/sub/x.txt", "chat_logs/typed_locations.txt",
                           "chat_logs/a.exe", "other/settings.xml", "settings.xml", "cache/x.xml" })
    {
        if (isAllowedPath(s, why)) { ++g_failed; std::cerr << "not-allowlisted path accepted: " << s << "\n"; }
    }

    CHECK(isSafeAccountName("john_smith"));
    CHECK(isSafeAccountName("john_smith.osgrid"));
    for (const char* s : { "", "../evil", "a/b", "a\\b", "user_settings", "logs", "John_Smith", ".hidden", "x." })
    {
        if (isSafeAccountName(s)) { ++g_failed; std::cerr << "bad account name accepted: " << s << "\n"; }
    }
}

static void testManifest()
{
    Manifest m, out;
    std::string err;
    m.account = "john_smith";
    m.history = true;
    m.viewer = "Slipstream Test\n1.5";
    CHECK(parseManifest(formatManifest(m), out, err) && out.account == "john_smith" && out.history && out.version == FORMAT_VERSION);
    CHECK(!parseManifest("format=other\nversion=1\n", out, err));
    CHECK(!parseManifest("format=slipstream-settings-backup\n", out, err));
    CHECK(!parseManifest("format=slipstream-settings-backup\nversion=abc\n", out, err));
    CHECK(!parseManifest("format=slipstream-settings-backup\nversion=0\n", out, err));
    CHECK(!parseManifest("format=slipstream-settings-backup\nversion=2\n", out, err) && err.find("newer") != std::string::npos);
    CHECK(!parseManifest("format=slipstream-settings-backup\nversion=1\naccount=../../evil\n", out, err));
}

static const char* SETTINGS =
    "<?xml version=\"1.0\" ?>\n<llsd>\n<map>\n"
    "    <key>UserLoginInfo</key>\n    <map>\n      <key>Value</key>\n      <array><string>SECRET-LOGIN</string></array>\n    </map>\n"
    "    <key>AFKTimeout</key>\n    <map>\n      <key>Type</key><string>S32</string>\n      <key>Value</key><integer>600</integer>\n    </map>\n"
    "    <key>MFAHash</key>\n    <map><key>Value</key><string>SECRET-MFA</string></map>\n"
    "    <key>GoogleTranslateAPIKey</key>\n    <map><key>Value</key><map><key>id</key><string>SECRET-KEY</string></map></map>\n"
    "    <key>EmptyString</key>\n    <map><key>Value</key><string /></map>\n"
    "    <key>RememberPassword</key><undef />\n"
    "</map>\n</llsd>\n";

static void testFilter()
{
    std::string out, err;
    std::vector<std::string> dropped;
    auto keep = [](const std::string& n) { return !isSecretName(n); };
    CHECK(filterSettingsXml(SETTINGS, keep, out, &dropped, err));
    CHECK(dropped.size() == 4);
    CHECK(out.find("SECRET") == std::string::npos && out.find("UserLoginInfo") == std::string::npos);
    CHECK(out.find("RememberPassword") == std::string::npos);
    CHECK(out.find("<key>AFKTimeout</key>") != std::string::npos && out.find("<integer>600</integer>") != std::string::npos);
    CHECK(out.find("<key>EmptyString</key>") != std::string::npos);
    // The result is still a settings map.
    std::string again;
    CHECK(filterSettingsXml(out, keep, again, nullptr, err) && again == out);
    CHECK(!filterSettingsXml("not xml", keep, out, nullptr, err));
    CHECK(!filterSettingsXml("<llsd><array/></llsd>", keep, out, nullptr, err));
    CHECK(!filterSettingsXml("<other><map/></other>", keep, out, nullptr, err));
    CHECK(!filterSettingsXml("<llsd><map><key>A</key></map></llsd>", keep, out, nullptr, err));
}

static Sources sources(const fs::path& root, bool history)
{
    Sources s;
    s.user_settings = root / "src" / "user_settings";
    s.account_dir = root / "src" / "john_smith";
    s.chat_dir = s.account_dir;
    s.include_history = history;
    return s;
}

static std::vector<std::string> paths(const std::vector<Entry>& v)
{
    std::vector<std::string> out;
    for (const Entry& e : v) out.push_back(e.path);
    return out;
}

static void testCollect(const fs::path& root)
{
    const std::vector<std::string> plain = paths(collectFiles(sources(root, false)));
    const std::vector<std::string> hist = paths(collectFiles(sources(root, true)));
    CHECK(has(plain, "user_settings/colors.xml") && has(plain, "user_settings/key_bindings.xml"));
    CHECK(has(plain, "user_settings/presets/graphic/Fast.xml") && has(plain, "user_settings/poses/mine/sit.xml"));
    CHECK(has(plain, "account/toolbars.xml"));
    CHECK(!has(plain, "user_settings/settings.xml") && !has(plain, "account/settings_per_account.xml"));  // live settings
    CHECK(!has(plain, "account/teleport_history.txt"));
    CHECK(has(hist, "account/teleport_history.txt") && has(hist, "chat_logs/chat.txt")
          && has(hist, "chat_logs/bob_smith.txt") && has(hist, "chat_logs/conversation.log"));
    for (const auto* list : { &plain, &hist })
    {
        for (const std::string& p : *list)
        {
            if (p.rfind("chat_logs/", 0) == 0 && list == &plain) { ++g_failed; std::cerr << "chat log without history: " << p << "\n"; }
            for (const char* bad : { "bin_conf", "password", "token", "telemetry", "cache", "filed_reports", "stored_favorites",
                                     "typed_locations", "url_history", "evil", "mfa_hash", "settings_rc" })
            {
                if (p.find(bad) != std::string::npos) { ++g_failed; std::cerr << "collected: " << p << "\n"; }
            }
        }
    }
}

static void testWriteRead(const fs::path& root)
{
    std::vector<Entry> entries = collectFiles(sources(root, true));
    Entry s;
    s.path = "user_settings/settings.xml";
    s.data = SETTINGS;
    s.from_memory = true;
    entries.push_back(s);
    Entry a;
    a.path = "account/settings_per_account.xml";
    a.source = root / "src" / "john_smith" / "settings_per_account.xml";
    entries.push_back(a);
    // Hostile entries a caller might pass: refused, never written.
    for (const char* bad : { "../evil.xml", "user_settings/bin_conf.dat", "user_settings/password.dat", "account/../../evil.xml" })
    {
        Entry e;
        e.path = bad;
        e.data = "SECRET-HOSTILE";
        e.from_memory = true;
        entries.push_back(e);
    }
    Manifest m;
    m.account = "john_smith";
    m.history = true;
    m.created = "2026-11-19 12:00:00";
    const fs::path zip = root / "out" / "backup.zip";
    fs::create_directories(zip.parent_path());
    std::string err;
    std::vector<std::string> refused;
    CHECK(writeBackup(zip, m, entries, err, &refused));
    CHECK(refused.size() >= 4);
    std::vector<std::string> names;
    std::string text;
    CHECK(readZip(zip, names, text));
    CHECK(!names.empty() && names[0] == MANIFEST_NAME);
    CHECK(has(names, "user_settings/settings.xml") && has(names, "account/settings_per_account.xml") && has(names, "chat_logs/chat.txt"));
    for (const std::string& n : names)
    {
        std::string why;
        if (n != MANIFEST_NAME && !isAllowedPath(n, why)) { ++g_failed; std::cerr << "in archive: " << n << "\n"; }
    }
    // The "secrets never included" rule, on the bytes of the archive.
    CHECK(text.find("SECRET") == std::string::npos);
    CHECK(text.find("UserLoginInfo") == std::string::npos && text.find("MFAHash") == std::string::npos);
    CHECK(!fs::exists(root / "out" / "backup.zip.partial"));

    // Without history: no transcripts, no teleport history, even when passed in.
    Manifest m2 = m;
    m2.history = false;
    const fs::path zip2 = root / "out" / "plain.zip";
    CHECK(writeBackup(zip2, m2, entries, err));
    names.clear();
    text.clear();
    CHECK(readZip(zip2, names, text));
    for (const std::string& n : names)
    {
        if (n.rfind("chat_logs/", 0) == 0 || n == "account/teleport_history.txt") { ++g_failed; std::cerr << "history in plain backup: " << n << "\n"; }
    }

    // Round trip.
    ReadResult r;
    const fs::path dest = root / "out" / "unpacked";
    CHECK(readBackup(zip, dest, Limits(), r, err));
    CHECK(r.manifest.account == "john_smith" && r.manifest.history);
    CHECK(has(r.files, "user_settings/colors.xml") && has(r.files, "account/toolbars.xml"));
    CHECK(fs::exists(dest / "user_settings" / "presets" / "graphic" / "Fast.xml"));
    CHECK(slurp(dest / "user_settings" / "settings.xml").find("AFKTimeout") != std::string::npos);
    // Unpacking again into a non-empty folder is refused.
    CHECK(!readBackup(zip, dest, Limits(), r, err));

    // Size limits.
    Limits small;
    small.max_file = 16;
    CHECK(!readBackup(zip, root / "out" / "small", small, r, err));
}

static void testHostile(const fs::path& root)
{
    const fs::path z = root / "zips";
    std::string err;
    for (const char* name : { "traversal.zip", "traversal_deep.zip", "absolute.zip", "backslash.zip", "drive.zip", "symlink.zip",
                              "nomanifest.zip", "wrongformat.zip", "newer.zip", "noversion.zip", "badaccount.zip",
                              "notzip.zip", "truncated.zip", "twomanifests.zip", "device.zip" })
    {
        ReadResult r;
        const fs::path dest = root / "unpack" / name;
        if (readBackup(z / name, dest, Limits(), r, err))
        {
            ++g_failed;
            std::cerr << "accepted hostile archive " << name << "\n";
        }
        // Rejected before anything is written.
        std::error_code ec;
        if (fs::exists(dest, ec) && !fs::is_empty(dest, ec)) { ++g_failed; std::cerr << "wrote files from " << name << "\n"; }
    }
    // Unknown entries are ignored, the rest restores; secret settings inside are stripped.
    ReadResult r;
    const fs::path dest = root / "unpack" / "mixed";
    CHECK(readBackup(z / "mixed.zip", dest, Limits(), r, err));
    CHECK(has(r.files, "user_settings/colors.xml") && has(r.files, "user_settings/settings.xml"));
    CHECK(!fs::exists(dest / "user_settings" / "bin_conf.dat") && !fs::exists(dest / "user_settings" / "run.exe")
          && !fs::exists(dest / "other") && !fs::exists(dest / "chat_logs"));
    CHECK(r.ignored.size() >= 4);
    const std::string s = slurp(dest / "user_settings" / "settings.xml");
    CHECK(s.find("UserLoginInfo") == std::string::npos && s.find("AFKTimeout") != std::string::npos);
}

static void testApply(const fs::path& root)
{
    const fs::path app = root / "app";
    const fs::path us = app / "user_settings";
    const fs::path pending = us / RESTORE_DIR / "pending";
    std::string err;

    // Unmarked: discarded.
    spit(pending / "user_settings" / "colors.xml", "UNMARKED");
    ApplyResult a = applyPendingRestore(us, app);
    CHECK(!a.applied && !fs::exists(pending) && !fs::exists(us / "colors.xml"));

    // Marked: applied, then removed.
    spit(us / "colors.xml", "OLD");
    spit(pending / "user_settings" / "colors.xml", "NEW");
    spit(pending / "user_settings" / "presets" / "camera" / "Front.xml", "CAM");
    spit(pending / "account" / "toolbars.xml", "TOOLBARS");
    spit(pending / "user_settings" / "bin_conf.dat", "SECRET");                   // never written
    spit(pending / "chat_logs" / "chat.txt", "merged at restore time");           // not copied
    CHECK(!writePendingMarker(pending, "../evil", "x", err));
    CHECK(writePendingMarker(pending, "john_smith", "backup.zip", err));
    a = applyPendingRestore(us, app);
    CHECK(a.applied && a.files == 3);
    CHECK(slurp(us / "colors.xml") == "NEW" && slurp(us / "presets" / "camera" / "Front.xml") == "CAM");
    CHECK(slurp(app / "john_smith" / "toolbars.xml") == "TOOLBARS");
    CHECK(!fs::exists(us / "bin_conf.dat") && !fs::exists(pending));

    // A tampered marker cannot send account files outside the app folder.
    spit(pending / "account" / "toolbars.xml", "EVIL");
    spit(pending / PENDING_MARKER, "account=../../evil\n");
    a = applyPendingRestore(us, app);
    CHECK(!fs::exists(root / "evil") && !fs::exists(app / ".." / "evil") && !fs::exists(pending));

    // Before-restore backups are bounded.
    const fs::path bdir = app / "backups";
    for (int i = 0; i < 8; ++i) spit(bdir / ("before_restore_2026-11-1" + std::to_string(i) + ".zip"), "x");
    spit(bdir / "mine.zip", "keep");
    pruneBackups(bdir, "before_restore_", 5);
    CHECK(!fs::exists(bdir / "before_restore_2026-11-10.zip") && fs::exists(bdir / "before_restore_2026-11-17.zip"));
    CHECK(fs::exists(bdir / "mine.zip"));
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: test_settings_backup <fixture dir>\n";
        return 2;
    }
    const fs::path root = argv[1];
    testNames();
    testManifest();
    testFilter();
    testCollect(root);
    testWriteRead(root);
    testHostile(root);
    testApply(root);
    if (g_failed)
    {
        std::cerr << g_failed << " check(s) failed\n";
        return 1;
    }
    std::cout << "settings backup: all checks passed\n";
    return 0;
}
