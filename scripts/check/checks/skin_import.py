"""Regression guard for the skin importer (#1599).

  * builds a fake Firestorm install (skins.xml, default + a skin + a theme, a broken TGA, an
    unused image, layout files that pass and fail), our own default skin, and zips (a good one,
    a zip-slip one with absolute / '..' / symlink / executable entries, a truncated one, a
    non-zip, an oversized one), then compiles scripts/check/test_skin_import.cpp against
    indra/newview/slskinimportcore.cpp (host compiler, system expat + minizip-ng) and runs it;
  * reads the viewer source for what the fixture cannot see: the importer uses the tested core,
    the runtime XUI fallback is in LLUICtrlFactory::getLayeredXMLNode, the lookup-names list
    is current (a warning: any new getChild call changes it).
"""
import os
import shutil
import struct
import subprocess
import tempfile
import zipfile
import zlib

NAME = "skin-import"
STAGE = "source"
DESCRIPTION = "skin importer: zip-slip, manifests, colour aliases, XUI rules, Firestorm skins (#1599)"
GUARDS = ("An imported skin writing outside the skins folder, a symlink or executable unpacked, a broken "
          "zip half-installed, Firestorm colours / textures not converted, incompatible layout files "
          "imported, or a broken skin layout file leaving a floater without its controls.")


def read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def write(path, text, mode="w"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as f:
        f.write(text)


def png(w=4, h=4):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    raw = b"".join(b"\x00" + b"\xff\x00\x00\xff" * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def colors(entries):
    body = "".join('  <color name="%s" %s="%s" />\n' % e for e in entries)
    return '<?xml version="1.0" encoding="utf-8" standalone="yes" ?>\n<colors>\n%s</colors>\n' % body


def build_fixture(root):
    # Our skin.
    ours = os.path.join(root, "ours", "skins", "default")
    write(os.path.join(ours, "colors.xml"), colors([("EmphasisColor", "value", "0.38 0.69 0.57 1"),
                                                     ("TextFgColor", "value", "1 1 1 1"),
                                                     ("FriendChatColor", "value", "1 1 0 1"),
                                                     ("LabelTextColor", "reference", "TextFgColor")]))
    write(os.path.join(ours, "textures", "textures.xml"),
          '<textures version="101">\n'
          '  <texture name="Icon_A" file_name="icons/a.png" vector_file="phosphor/a.svg" />\n'
          '  <texture name="Icon_B" file_name="icons/b.tga" />\n'
          '  <texture name="Icon_C" file_name="icons/c.png" />\n'
          '</textures>\n')
    write(os.path.join(ours, "xui", "en", "panel_test.xml"),
          '<panel name="test"><scroll_list name="skin_list"/><button name="apply_skin"/></panel>\n')
    write(os.path.join(ours, "xui", "en", "panel_good.xml"),
          '<panel name="good"><scroll_list name="skin_list"/><text name="decoration"/></panel>\n')

    # A Firestorm install's skins folder.
    skins = os.path.join(root, "fs_install", "skins")
    write(os.path.join(skins, "skins.xml"),
          '<?xml version="1.0" ?>\n<llsd><array>\n'
          '<map><key>name</key><string>Starlight</string><key>folder</key><string>starlight</string>'
          '<key>themes</key><array>'
          '<map><key>name</key><string>Starlight Classic</string><key>folder</key><string/></map>'
          '<map><key>name</key><string>Silver Blue</string><key>folder</key><string>silver_blue</string></map>'
          '<map><key>name</key><string>Nope</string><key>folder</key><string>nope</string></map>'
          '</array></map>\n'
          '<map><key>name</key><string>Ghost</string><key>folder</key><string>ghost</string>'
          '<key>themes</key><array/></map>\n'
          '<map><key>name</key><string>Evil</string><key>folder</key><string>../../etc</string></map>\n'
          '</array></llsd>\n')
    d = os.path.join(skins, "default")
    write(os.path.join(d, "colors.xml"), colors([("EmphasisColor", "value", ".1 .2 .3 1"),
                                                  ("SL-Normal", "value", "0.5 0.5 0.5 1"),
                                                  ("TextFgColor", "reference", "SL-Normal"),
                                                  ("FSOnlyColor", "value", "0 0 0 1")]))
    write(os.path.join(d, "textures", "textures.xml"),
          '<textures version="101">\n'
          '  <texture name="Icon_A" file_name="icons/a.png" scale.left="2" scale.top="14" scale.right="14" scale.bottom="2" />\n'
          '  <texture name="Icon_B" file_name="icons/b.tga" />\n'
          '  <texture name="Icon_C" file_name="icons/c.png" />\n'
          '</textures>\n')
    write(os.path.join(d, "textures", "icons", "c.png"), png(), "wb")
    write(os.path.join(d, "xui", "en", "panel_test.xml"), '<panel name="test"/>\n')
    s = os.path.join(skins, "starlight")
    write(os.path.join(s, "colors.xml"), colors([("FriendsChatColor", "value", "1 0 0 1")]))
    write(os.path.join(s, "textures", "icons", "a.png"), png(16, 16), "wb")
    write(os.path.join(s, "xui", "en", "panel_test.xml"), '<panel name="test"><scroll_list name="skin_list"/></panel>\n')
    write(os.path.join(s, "xui", "en", "panel_good.xml"), '<panel name="good"><scroll_list name="skin_list"/><icon name="fs_extra"/></panel>\n')
    write(os.path.join(s, "xui", "en", "panel_new.xml"), '<panel name="new"/>\n')
    t = os.path.join(s, "themes", "silver_blue")
    write(os.path.join(t, "colors.xml"), colors([("EmphasisColor", "value", "0 0 1 1")]))
    write(os.path.join(t, "textures", "icons", "b.tga"), b"\x00" * 64, "wb")
    write(os.path.join(t, "textures", "stray.png"), png(), "wb")
    lo = os.path.join(skins, "loose")
    write(os.path.join(lo, "colors.xml"), colors([("EmphasisColor", "value", "1 1 1 1")]))
    write(os.path.join(lo, "themes", "neon", "colors.xml"), colors([("EmphasisColor", "value", "0 1 0 1")]))

    # Install discovery: two Firestorm installs and one other folder under a "Program Files".
    programs = os.path.join(root, "programs")
    shutil.copytree(skins, os.path.join(programs, "Firestorm-Releasex64", "skins"))
    shutil.copytree(skins, os.path.join(programs, "Firestorm.app", "Contents", "Resources", "skins"))
    shutil.copytree(skins, os.path.join(programs, "OtherViewer", "skins"))

    write(os.path.join(root, "wrapped", "MySkin", "colors.xml"), colors([("EmphasisColor", "value", "1 0 1 1")]))
    write(os.path.join(root, "empty", "readme.txt"), "nothing here\n")

    # Archives.
    zips = os.path.join(root, "zips")
    os.makedirs(zips)
    good = os.path.join(zips, "good.zip")
    with zipfile.ZipFile(good, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Alchemy Dark/", "")
        z.writestr("Alchemy Dark/manifest.json", '{"name": "Alchemy Dark", "author": "Someone"}')
        z.writestr("Alchemy Dark/colors.xml", colors([("EmphasisColor", "value", "1 0 0 1")]))
        z.writestr("Alchemy Dark/textures/icons/a.png", png())
    with zipfile.ZipFile(os.path.join(zips, "slip.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("colors.xml", colors([("EmphasisColor", "value", "1 0 0 1")]))
        z.writestr(zipfile.ZipInfo("../evil.xml"), "<x/>")
        z.writestr(zipfile.ZipInfo("..\\..\\evil.xml"), "<x/>")
        z.writestr(zipfile.ZipInfo("/abs.xml"), "<x/>")
        z.writestr(zipfile.ZipInfo("C:\\evil.xml"), "<x/>")
        z.writestr(zipfile.ZipInfo("run.exe"), "MZ")
        link = zipfile.ZipInfo("link.png")
        link.create_system = 3
        link.external_attr = (0o120777 << 16)
        z.writestr(link, "/etc/passwd")
    with open(good, "rb") as f:
        data = f.read()
    write(os.path.join(zips, "broken.zip"), data[:len(data) // 2], "wb")
    write(os.path.join(zips, "notzip.zip"), "this is not a zip file\n")
    with zipfile.ZipFile(os.path.join(zips, "big.zip"), "w", zipfile.ZIP_STORED) as z:
        z.writestr("colors.xml", colors([("EmphasisColor", "value", "1 0 0 1")]))
        z.writestr("textures/big.png", os.urandom(200 * 1024))


def run_fixture(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: skin import fixtures not run")
        return False
    src = ctx.path("scripts/check/test_skin_import.cpp")
    core = ctx.path("indra/newview/slskinimportcore.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_skin_import")
        c = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-I" + inc, src, core, "-o", exe,
                            "-lexpat", "-lminizip-ng"], capture_output=True, text=True)
        if c.returncode:
            out = c.stdout + c.stderr
            if "expat.h" in out or "minizip-ng" in out or "cannot find -l" in out:
                result.warn("expat / minizip-ng development files not found: skin import fixtures not run")
                return False
            result.fail("compile failed:\n" + out)
            return False
        fixture = os.path.join(tmp, "fixture")
        build_fixture(fixture)
        r = subprocess.run([exe, fixture], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return False
        # Nothing may have been written next to the fixture by a zip-slip entry.
        for evil in ("evil.xml", "abs.xml"):
            for d in (tmp, fixture, os.path.join(fixture, "unpack")):
                if os.path.exists(os.path.join(d, evil)):
                    result.fail("zip-slip: %s was written outside the unpack folder" % evil)
    return True


def check_viewer(ctx, result):
    imp = read(ctx, "indra/newview/slskinimport.cpp") + read(ctx, "indra/newview/slskinimport.h")
    for needle, why in (('#include "slskinimportcore.h"', "the importer must use the tested core"),
                        ("slskin::unpackZip(", "zips must be unpacked by the tested safe unpacker"),
                        ("slskin::buildSkin(", "skins must be built by the tested conversion"),
                        ("\"SkinImport\"", "the import must log under the SkinImport tag"),
                        ("fs::rename(", "an install must be moved into place from the staging folder")):
        if needle not in imp:
            result.fail("slskinimport.cpp: " + why)
    if "SLUnZip" in imp:
        result.fail("slskinimport.cpp: do not use SLUnZip (it extracts entries unchecked)")
    prefs = read(ctx, "indra/newview/llfloaterpreference.cpp")
    if "zip->extract(" in prefs:
        result.fail("llfloaterpreference.cpp: Add skin must not extract zips blindly (use SLSkinImport)")
    factory = read(ctx, "indra/llui/lluictrlfactory.cpp")
    if "userSkinXuiFallback" not in factory and "SkinImport" not in factory:
        result.fail("lluictrlfactory.cpp: getLayeredXMLNode must fall back to our XUI when a user skin's file fails")
    panel = read(ctx, "indra/newview/skins/default/xui/en/panel_preferences_skins.xml")
    for name in ("add_skin_folder", "import_fs_skin", "skin_import_layout"):
        if 'name="%s"' % name not in panel:
            result.fail("panel_preferences_skins.xml: no %s" % name)
    gen = ctx.path("scripts/ui/gen_xui_lookup_names.py")
    r = subprocess.run(["python3", "-I", gen, "--check"], capture_output=True, text=True)
    if r.returncode:
        result.warn("xui_lookup_names.txt is out of date: run scripts/ui/gen_xui_lookup_names.py")


def run(ctx, result):
    for rel in ("indra/newview/slskinimportcore.cpp", "scripts/check/test_skin_import.cpp", "indra/newview/slskinimport.cpp"):
        if not os.path.exists(ctx.path(rel)):
            result.fail("%s is missing (skin importer)" % rel)
            result.summary = "skin importer missing"
            return
    ran = run_fixture(ctx, result)
    check_viewer(ctx, result)
    result.summary = "fixtures + source rules" if ran else "source rules (fixtures not run)"
