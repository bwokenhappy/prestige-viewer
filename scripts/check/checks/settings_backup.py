"""Regression guard for settings backup / restore / export (#1423).

  * builds fake user folders (with bin_conf.dat, password.dat, a token file, telemetry, secret
    settings) and hostile archives (path traversal, absolute / drive / backslash paths, links,
    no manifest, another format, a newer version stamp, a bad account name, Windows device
    names, two manifests, truncated, not a zip), then compiles scripts/check/test_settings_backup.cpp
    against indra/newview/slsettingsbackupcore.cpp (host compiler, system expat + minizip-ng) and runs it;
  * reads the viewer source for what the fixture cannot see: the viewer uses the tested core,
    the restore is applied before the user settings load, the export map only names settings we
    declare and persist.
"""
import os
import shutil
import subprocess
import tempfile
import warnings
import zipfile
import xml.etree.ElementTree as ET

NAME = "settings-backup"
STAGE = "source"
DESCRIPTION = "settings backup: secrets never included, archive validation, staged restore, export map (#1423)"
GUARDS = ("A backup carrying passwords, MFA hashes, API keys or the secure store; a restore writing outside "
          "the settings folders, accepting another format or a newer version, or being overwritten at exit; "
          "an export writing settings another viewer does not share.")

SECRET_SETTINGS = (
    '<?xml version="1.0" ?>\n<llsd><map>\n'
    '<key>UserLoginInfo</key><map><key>Value</key><string>SECRET-LOGIN</string></map>\n'
    '<key>AFKTimeout</key><map><key>Type</key><string>S32</string><key>Value</key><integer>600</integer></map>\n'
    '<key>MFAHash</key><map><key>Value</key><string>SECRET-MFA</string></map>\n'
    '</map></llsd>\n')

MANIFEST = "format=slipstream-settings-backup\nversion=1\naccount=john_smith\nhistory=0\n"


def write(path, text, mode="w"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as f:
        f.write(text)


def make_zip(path, entries, manifest=MANIFEST):
    with warnings.catch_warnings(), zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        warnings.simplefilter("ignore")     # twomanifests.zip duplicates a name on purpose
        if manifest is not None:
            z.writestr("slipstream_backup.txt", manifest)
        for name, data in entries:
            if isinstance(name, zipfile.ZipInfo):
                z.writestr(name, data)
            else:
                z.writestr(zipfile.ZipInfo(name), data)


def build_fixture(root):
    us = os.path.join(root, "src", "user_settings")
    for name in ("colors.xml", "key_bindings.xml", "ignorable_dialogs.xml", "filed_reports.xml",
                 "stored_favorites_agni.xml", "settings_rc.xml", "fx_state.xml"):
        write(os.path.join(us, name), "<x/>\n")
    write(os.path.join(us, "settings.xml"), SECRET_SETTINGS)
    write(os.path.join(us, "bin_conf.dat"), "SECRET-STORE")
    write(os.path.join(us, "password.dat"), "SECRET-PASSWORD")
    write(os.path.join(us, "testctl.token"), "SECRET-TOKEN")
    write(os.path.join(us, "telemetry", "queue", "a.json"), "{}")
    write(os.path.join(us, "presets", "graphic", "Fast.xml"), "<llsd/>")
    write(os.path.join(us, "presets", "graphic", "evil.exe"), "MZ")
    write(os.path.join(us, "presets", "camera", "Front.xml"), "<llsd/>")
    write(os.path.join(us, "poses", "mine", "sit.xml"), "<llsd/>")
    write(os.path.join(us, "dictionaries", "user_custom.dic"), "word\n")
    write(os.path.join(us, "mfa_hash.xml"), "SECRET-MFA-FILE")
    write(os.path.join(root, "src", "cache", "x.xml"), "<x/>")
    acct = os.path.join(root, "src", "john_smith")
    write(os.path.join(acct, "settings_per_account.xml"),
          '<llsd><map><key>FSKeywords</key><map><key>Value</key><string>pony</string></map>'
          '<key>SomeToken</key><map><key>Value</key><string>SECRET-ACCT</string></map></map></llsd>\n')
    for name in ("toolbars.xml", "url_history.xml", "inbox_new_items.xml"):
        write(os.path.join(acct, name), "<x/>")
    for name in ("teleport_history.txt", "typed_locations.txt", "chat.txt", "bob_smith.txt", "conversation.log"):
        write(os.path.join(acct, name), "[2026/11/19 12:00]  Bob: hi\n")

    z = os.path.join(root, "zips")
    os.makedirs(z)
    good = [("user_settings/colors.xml", "<x/>")]
    make_zip(os.path.join(z, "traversal.zip"), good + [("../evil.xml", "EVIL")])
    make_zip(os.path.join(z, "traversal_deep.zip"), good + [("user_settings/../../evil.xml", "EVIL")])
    make_zip(os.path.join(z, "absolute.zip"), good + [("/tmp/abs.xml", "EVIL")])
    make_zip(os.path.join(z, "backslash.zip"), good + [("user_settings\\..\\..\\evil.xml", "EVIL")])
    make_zip(os.path.join(z, "drive.zip"), good + [("C:/evil.xml", "EVIL")])
    make_zip(os.path.join(z, "device.zip"), good + [("user_settings/presets/graphic/CON.xml", "EVIL")])
    link = zipfile.ZipInfo("user_settings/key_bindings.xml")
    link.create_system = 3
    link.external_attr = (0o120777 << 16)
    make_zip(os.path.join(z, "symlink.zip"), good + [(link, "/etc/passwd")])
    make_zip(os.path.join(z, "nomanifest.zip"), good, manifest=None)
    make_zip(os.path.join(z, "wrongformat.zip"), good, manifest="format=firestorm-backup\nversion=1\n")
    make_zip(os.path.join(z, "newer.zip"), good, manifest="format=slipstream-settings-backup\nversion=2\n")
    make_zip(os.path.join(z, "noversion.zip"), good, manifest="format=slipstream-settings-backup\n")
    make_zip(os.path.join(z, "badaccount.zip"), good,
             manifest="format=slipstream-settings-backup\nversion=1\naccount=../../evil\n")
    make_zip(os.path.join(z, "twomanifests.zip"), good + [("slipstream_backup.txt", MANIFEST)])
    make_zip(os.path.join(z, "mixed.zip"), good + [
        ("user_settings/settings.xml", SECRET_SETTINGS),
        ("user_settings/bin_conf.dat", "SECRET"),
        ("user_settings/run.exe", "MZ"),
        ("other/x.xml", "<x/>"),
        ("chat_logs/chat.txt", "not announced"),
    ])
    with open(os.path.join(z, "mixed.zip"), "rb") as f:
        data = f.read()
    write(os.path.join(z, "truncated.zip"), data[:len(data) // 2], "wb")
    write(os.path.join(z, "notzip.zip"), "this is not a zip\n")


def run_fixture(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: settings backup fixtures not run")
        return False
    src = ctx.path("scripts/check/test_settings_backup.cpp")
    core = ctx.path("indra/newview/slsettingsbackupcore.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_settings_backup")
        c = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-I" + ctx.path("indra/newview"), src, core, "-o", exe,
                            "-lexpat", "-lminizip-ng"], capture_output=True, text=True)
        if c.returncode:
            out = c.stdout + c.stderr
            if "expat.h" in out or "minizip-ng" in out or "cannot find -l" in out:
                result.warn("expat / minizip-ng development files not found: settings backup fixtures not run")
                return False
            result.fail("compile failed:\n" + out)
            return False
        fixture = os.path.join(tmp, "fixture")
        build_fixture(fixture)
        r = subprocess.run([exe, fixture], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return False
        for d in (tmp, fixture, os.path.join(fixture, "unpack")):
            for evil in ("evil.xml", "abs.xml"):
                p = os.path.join(d, evil)
                if os.path.exists(p):
                    result.fail("path traversal: %s was written" % p)
    return True


def read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def check_viewer(ctx, result):
    drv = read(ctx, "indra/newview/slsettingsbackup.cpp")
    for needle, why in (('#include "slsettingsbackupcore.h"', "the viewer must use the tested core"),
                        ("slbackup::writeBackup(", "backups must be written by the tested writer"),
                        ("slbackup::readBackup(", "restores must be read by the tested validator"),
                        ("slbackup::isSecretName(", "live settings must be filtered by the secret rule"),
                        ("isPersisted()", "non-persistent settings (Persist 0) must never be restored or exported"),
                        (".bak", "another viewer's file is only rewritten after a backup copy")):
        if needle not in drv:
            result.fail("slsettingsbackup.cpp: " + why)
    app = read(ctx, "indra/newview/llappviewer.cpp")
    apply_at = app.find("SLSettingsBackup::applyPendingRestore(")
    load_at = app.find('loadSettingsFromDirectory("User")')
    if apply_at < 0 or load_at < 0 or apply_at > load_at:
        result.fail("llappviewer.cpp: the staged restore must be applied before the user settings load")

    # The export map names only settings we declare and save.
    ours = {}
    for name in ("settings.xml", "settings_per_account.xml"):
        root = ET.parse(ctx.path("indra/newview/app_settings", name)).getroot()
        m = root.find("map")
        children = list(m)
        for i in range(0, len(children) - 1, 2):
            persist = "1"
            val = children[i + 1]
            kids = list(val)
            for j in range(0, len(kids) - 1, 2):
                if kids[j].text == "Persist":
                    persist = kids[j + 1].text or "0"
            ours[children[i].text] = (name, persist)
    emap = ET.parse(ctx.path("indra/newview/app_settings/settings_export_map.xml")).getroot()
    count = 0
    for key in (s for arr in emap.iter("array") for s in arr.iter("string")):
        n = key.text or ""
        count += 1
        if n not in ours:
            result.fail("settings_export_map.xml: %s is not one of our settings" % n)
        elif ours[n][1] == "0":
            result.fail("settings_export_map.xml: %s is not persisted (Persist 0)" % n)
        if any(s in n.lower() for s in ("password", "token", "logininfo", "apikey", "mfahash", "secret")):
            result.fail("settings_export_map.xml: %s may hold a secret" % n)
    if count < 100:
        result.fail("settings_export_map.xml: only %d settings (expected the shared LL / Firestorm sets)" % count)


def run(ctx, result):
    for rel in ("indra/newview/slsettingsbackupcore.cpp", "scripts/check/test_settings_backup.cpp",
                "indra/newview/slsettingsbackup.cpp", "indra/newview/app_settings/settings_export_map.xml"):
        if not os.path.exists(ctx.path(rel)):
            result.fail("%s is missing (settings backup)" % rel)
            result.summary = "settings backup missing"
            return
    ran = run_fixture(ctx, result)
    check_viewer(ctx, result)
    result.summary = "fixtures + source rules" if ran else "source rules (fixtures not run)"
