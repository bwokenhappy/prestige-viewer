#!/usr/bin/env python3
"""Build Prestige's "Firestorm (Grey)" and "Firestorm (Dark)" skins from Firestorm's own skin.

Firestorm's look is its skins/default plus skins/firestorm (themes/dark on top for Dark).
Prestige's skins/default is a different, re-themed base (Phosphor vector icons, its own
colours), so a Prestige skin cannot simply be Firestorm's skins/firestorm folder. This script
merges Firestorm's files against Prestige's skins/default and writes:

  skins/firestorm[_dark]/colors.xml     every colour name of Prestige's skins/default (colours
                                        are resolved when they load, so a skin has to define a
                                        name again for a reference to pick up the skin's value),
                                        with Firestorm's definition where Firestorm has one, plus
                                        the Firestorm palette those reference and Firestorm names
                                        Prestige's code reads. Names only Prestige has keep
                                        Prestige's definition or a value from MANUAL below.
  skins/firestorm[_dark]/textures/      every widget / window / container image whose Firestorm
                                        version differs from Prestige's (same path, so no
                                        textures.xml entry is needed: the geometry is the same),
                                        and Firestorm's bitmap icons for every icon Firestorm has
                                        (textures.xml entries with vector_file="" so the bitmap
                                        is drawn instead of Prestige's vector glyph).
  skins/firestorm[_dark]/xui/en/widgets copies of Prestige's widget defaults with Firestorm's
                                        look settings (EDITS below).

The manifest.json files are written by hand, not by this script.

    python3 scripts/content_tools/prestige_firestorm_skin.py <firestorm>/indra/newview/skins

<firestorm> is a Firestorm (phoenix-firestorm) checkout. Safe to re-run: it never reads its
own output. Needs only the Python standard library.
"""

import filecmp
import os
import re
import shutil
import sys
import xml.etree.ElementTree as ET
from xml.sax.saxutils import quoteattr

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
NEWVIEW = os.path.join(ROOT, "indra", "newview")
SKINS = os.path.join(NEWVIEW, "skins")

# Firestorm images we do not take: branding (Prestige keeps its own) and world graphics.
KEEP_BITMAP_PREFIXES = ("Inv_", "Object_", "Parcel_")   # <Prestige> colourful icons
SKIP_TEXTURES = {"SL_Logo", "System_Notification", "login_sl_logo", "login_sl_logo_small",
                 "first_login_image", "direction_arrow.tga", "Edit_Flip_X"}
# Images XUI names by file (no textures.xml entry) that Firestorm draws differently.
EXTRA_FILES = ("tabarea.tga", "widgets/SegmentedBtn_Right_On_Selected.png")
# <Prestige> Firestorm-look icons drawn as Prestige's layered vectors (textures/prestige_icons),
# sharp at any interface scale, instead of Firestorm's 16 px bitmap.
VECTOR_OVERRIDES = {
    "Inv_Object": 'vector_file="prestige_icons/cube_base.svg" vector_color="#A06C16" vector_box="0.07,0.05,0.93,0.96" '
                  'vector_file2="prestige_icons/cube_top.svg" vector_color2="#F7D57E" vector_box2="0.14,0.09,0.86,0.47" '
                  'vector_file3="prestige_icons/cube_left.svg" vector_color3="#DDA43A" vector_box3="0.11,0.33,0.47,0.90"',
}

# Colour names only Prestige has (or that Firestorm spells differently): (kind, value, why).
MANUAL = {
    # Preferences > Your colours (prestigeuicolors.cpp) shows these three as its swatches
    "SlipstreamBlue": ("r", "EmphasisColor", "Your colours: the accent swatch (Firestorm's accent)"),
    "SlipstreamPanel": ("r", "floater_bg_color", "Your colours: the window swatch (Firestorm's window grey)"),
    "SlipstreamText": ("r", "LabelTextColor", "Your colours: the text swatch"),
    "FriendChatColor": ("r", "FriendsChatColor", "readable chat: friends' names (Firestorm: FriendsChatColor)"),
    "ChatHeaderDisplayNameColor": ("r", "ChatNameColor", "chat header names (Firestorm: ChatNameColor)"),
    "ChatHeaderTimestampColor": ("r", "ChatTimestampColor", "chat header timestamps like Firestorm's timestamps"),
    "AccordionHeaderBgColor": ("r", "SL-Background", "Firestorm's accordion_tab header_bg_color"),
    "ChatAltRowColor": ("v", "1 1 1 0.03", "readable chat: faint tint behind every other speaker"),
    "GMDXChatBarBackground": ("v", "0.125 0.125 0.125 0.82", "MMO chat bar fill: Firestorm grey, not blue-black"),
    "GMDXChatBarEdge": ("v", "0.302 0.302 0.302 1", "MMO chat bar edge: neutral grey"),
}
# Firestorm colour names our widget copies below use.
WIDGET_COLORS = ("floater_bg_color", "floater_bg_color_a", "lineeditor_highlight",
                 "lineeditor_preedit_bg", "simpleeditor_bg_selected")

EDITS = {
    "floater.xml": ("window colours floater_bg_color / floater_bg_color_a",
                    [('bg_opaque_color="FloaterFocusBackgroundColor"', 'bg_opaque_color="floater_bg_color"'),
                     ('bg_alpha_color="FloaterDefaultBackgroundColor"', 'bg_alpha_color="floater_bg_color_a"')]),
    "line_editor.xml": ("selection and pre-edit colours",
                        [('highlight_color="EmphasisColor"', 'highlight_color="lineeditor_highlight"'),
                         ('preedit_bg_color="White"', 'preedit_bg_color="lineeditor_preedit_bg"')]),
    "simple_text_editor.xml": ("selection colour",
                               [('bg_selected_color="EmphasisColor"', 'bg_selected_color="simpleeditor_bg_selected"')]),
    "button.xml": ("hover glow 0.25 and 2 px bottom padding",
                   [('hover_glow_amount="0.15"', 'hover_glow_amount="0.25"'), ('pad_bottom="1"', 'pad_bottom="2"')]),
    "combo_box.xml": ("hover glow 0.25", [('hover_glow_amount="0.15"', 'hover_glow_amount="0.25"')]),
    "drop_down.xml": ("hover glow 0.25", [('hover_glow_amount="0.15"', 'hover_glow_amount="0.25"')]),
    "tab_container.xml": ("21 px tabs up to 160 px wide, flashing tabs in ButtonFlashBgColor",
                          [('tab_max_width="150"', 'tab_max_width="160"'),
                           ('tab_height="22"', 'tab_height="21"\n               tabs_flashing_color="ButtonFlashBgColor"')]),
}

THEMES = (("firestorm", "Grey", None), ("firestorm_dark", "Dark", "dark"))


# ------------------------------------------------------------------------------------------
# Colours
# ------------------------------------------------------------------------------------------

def load_colors(path):
    defs, order = {}, []
    for c in ET.parse(path).getroot().iter("color"):
        name = c.get("name")
        if name not in defs:
            order.append(name)
        defs[name] = ("v", c.get("value")) if c.get("value") is not None else ("r", c.get("reference"))
    return defs, order


def valid_value(v):
    try:
        parts = [float(x) for x in v.split()]
    except ValueError:
        return False
    return 3 <= len(parts) <= 4


def all_color_names():
    names = set()
    for skin in os.listdir(SKINS):
        path = os.path.join(SKINS, skin, "colors.xml")
        if os.path.isfile(path) and not skin.startswith("firestorm"):
            names |= set(load_colors(path)[0])
    return names


def used_color_names(candidates):
    """Colour names Prestige's C++ or default XUI mention (quoted)."""
    used = set()
    quoted = re.compile(r'"([A-Za-z_][A-Za-z0-9_\-]*)"')
    for top in ("llui", "newview", "llrender", "llwindow"):
        for dirpath, _, files in os.walk(os.path.join(ROOT, "indra", top)):
            if os.sep + "skins" in dirpath:
                continue
            for f in files:
                if f.endswith((".cpp", ".h")):
                    with open(os.path.join(dirpath, f), errors="ignore") as fh:
                        used |= {m for m in quoted.findall(fh.read()) if m in candidates}
    for dirpath, _, files in os.walk(os.path.join(SKINS, "default", "xui", "en")):
        for f in files:
            with open(os.path.join(dirpath, f), errors="ignore") as fh:
                used |= {m for m in quoted.findall(fh.read()) if m in candidates}
    return used


def build_colors(fs_skins, theme, title, out_path):
    pd, porder = load_colors(os.path.join(SKINS, "default", "colors.xml"))
    fs = load_colors(os.path.join(fs_skins, "default", "colors.xml"))[0]
    fs.update(load_colors(os.path.join(fs_skins, "firestorm", "colors.xml"))[0])
    if theme:
        fs.update(load_colors(os.path.join(fs_skins, "firestorm", "themes", theme, "colors.xml"))[0])
    used = used_color_names(all_color_names() | set(fs))

    def fsdef(name):
        d = fs.get(name)
        return d if d and (d[0] == "r" or valid_value(d[1])) else None

    out, order = {}, []

    def add(name, d, comment=""):
        if name not in out:
            out[name] = (d[0], d[1], comment)
            order.append(name)

    for name in porder:
        if name in MANUAL:
            add(name, MANUAL[name][:2], MANUAL[name][2])
        elif fsdef(name):
            add(name, fsdef(name))
        else:
            add(name, pd[name], "Prestige colour (no Firestorm equivalent)" if name not in fs
                else "Firestorm's value is not a colour: Prestige's kept")
    for name in sorted(used) + list(WIDGET_COLORS):
        if fsdef(name):
            add(name, fsdef(name))
    for name, d in MANUAL.items():
        add(name, d[:2], d[2])
    changed = True
    while changed:   # the Firestorm palette the colours above reference
        changed = False
        for name in list(order):
            kind, val, _ = out[name]
            if kind == "r" and val not in out and fsdef(val):
                add(val, fsdef(val))
                changed = True
            elif kind == "r" and val not in out and val not in pd:
                print("warning: %s references unknown colour %s" % (name, val))

    palette = [n for n in order if n not in pd and n not in MANUAL]
    with open(out_path, "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8" standalone="yes" ?>\n')
        f.write("<!-- <Prestige> Firestorm (%s) skin colours: Firestorm viewer skins/default + skins/firestorm%s\n"
                "     colors.xml (LGPL 2.1), merged for every colour name Prestige uses. Colours resolve when\n"
                "     they load, so every name of skins/default/colors.xml is defined here again. Names only\n"
                "     Prestige has keep Prestige's definition (most reference a Firestorm colour) or carry a\n"
                "     comment with the value chosen for this look. Generated by\n"
                "     scripts/content_tools/prestige_firestorm_skin.py: edit there and re-run. -->\n"
                % (title, "/themes/" + theme if theme else ""))
        f.write("<colors>\n    <!-- Firestorm palette (names the colours below reference) -->\n")

        def write(name):
            kind, val, comment = out[name]
            if comment:
                f.write("    <!-- %s -->\n" % comment)
            f.write('    <color name="%s" %s="%s" />\n' % (name, "value" if kind == "v" else "reference", val))

        for name in palette:
            write(name)
        f.write("    <!-- Interface colours -->\n")
        for name in order:
            if name not in palette:
                write(name)
        f.write("</colors>\n")
    return len(order)


# ------------------------------------------------------------------------------------------
# Textures
# ------------------------------------------------------------------------------------------

def load_textures(path):
    defs, order = {}, []
    for t in ET.parse(path).getroot().iter("texture"):
        name = t.get("name")
        if name not in defs:
            order.append(name)
        defs.setdefault(name, {}).update(t.attrib)
    return defs, order


def find_ci(base, rel):
    """(path, relative path with the real case) of rel under base, matched case-insensitively."""
    cur, parts = base, []
    for part in rel.replace("\\", "/").split("/"):
        if not os.path.isdir(cur):
            return None, None
        match = [x for x in sorted(os.listdir(cur)) if x.lower() == part.lower()]
        if not match:
            return None, None
        cur = os.path.join(cur, match[0])
        parts.append(match[0])
    return (cur, "/".join(parts)) if os.path.isfile(cur) else (None, None)


def build_textures(fs_skins, theme, title, out_dir):
    ours, our_order = load_textures(os.path.join(SKINS, "default", "textures", "textures.xml"))
    layers = [os.path.join(fs_skins, "default", "textures"), os.path.join(fs_skins, "firestorm", "textures")]
    if theme:
        layers.append(os.path.join(fs_skins, "firestorm", "themes", theme, "textures"))
    theirs = {}
    for layer in layers:
        xml = os.path.join(layer, "textures.xml")
        if os.path.isfile(xml):
            for name, attrs in load_textures(xml)[0].items():
                theirs.setdefault(name, {}).update(attrs)

    def their_file(rel):
        for layer in reversed(layers):
            src, real = find_ci(layer, rel)
            if src:
                return src, real
        return None, None

    copies, entries = {}, []

    def want(rel, src):
        mine, _ = find_ci(os.path.join(SKINS, "default", "textures"), rel)
        if mine and filecmp.cmp(mine, src, shallow=False):
            return   # Prestige's default skin already has this exact image
        copies[rel] = src

    for name in our_order:
        if name not in theirs or name in SKIP_TEXTURES:
            continue
        attrs = ours[name]
        src, rel = their_file(theirs[name].get("file_name", name))
        if not src:
            print("warning: Firestorm has no image for %s" % name)
            continue
        if "vector_file" in attrs:
            # <Prestige> only colourful icons and stretchable frames take Firestorm's bitmap; every
            # other small icon (checkmarks, arrows, toolbar and window icons) keeps Prestige's sharp
            # vector glyph, coloured by the skin's IconWidget*/IconColor colours, because small
            # bitmaps go blocky when the interface is scaled.
            is_frame = any(k.startswith("scale") for k in attrs)
            if not (name.startswith(KEEP_BITMAP_PREFIXES) or is_frame):
                continue
            entries.append((name, rel))
            want(rel, src)
        elif attrs.get("file_name", name).lower() == rel.lower():
            _, mine_rel = find_ci(os.path.join(SKINS, "default", "textures"), rel)
            want(mine_rel or rel, src)
    for extra in EXTRA_FILES:
        src, rel = their_file(extra)
        if src:
            want(rel, src)

    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    for rel, src in sorted(copies.items()):
        os.makedirs(os.path.dirname(os.path.join(out_dir, rel)), exist_ok=True)
        shutil.copyfile(src, os.path.join(out_dir, rel))
    with open(os.path.join(out_dir, "textures.xml"), "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="utf-8" standalone="yes" ?>\n')
        f.write("<!-- <Prestige> Firestorm (%s) skin: Firestorm's bitmap icons in place of the Phosphor vector\n"
                "     icons, for every icon Firestorm has (vector_file=\"\" draws the bitmap). Icons only Prestige\n"
                "     has keep their vector glyph. Widget, window and container images that differ from\n"
                "     Prestige's are replaced by file (same path in this folder) and need no entry here.\n"
                "     Images: Firestorm viewer skins/default + skins/firestorm%s (LGPL 2.1).\n"
                "     Generated by scripts/content_tools/prestige_firestorm_skin.py. -->\n"
                % (title, "/themes/" + theme if theme else ""))
        f.write('<textures version="101">\n')
        for name, rel in entries:
            # <Prestige> icons drawn as Prestige's own layered vectors in Firestorm's look
            if name in VECTOR_OVERRIDES:
                f.write("  <!-- <Prestige> sharp vector gold box (the 16 px bitmap went blocky when drawn larger) -->\n")
                f.write("  <texture name=%s file_name=%s %s />\n" % (quoteattr(name), quoteattr(rel), VECTOR_OVERRIDES[name]))
                continue
            # </Prestige>
            f.write("  <texture name=%s file_name=%s vector_file=\"\" />\n" % (quoteattr(name), quoteattr(rel)))
        f.write("</textures>\n")
    return len(entries), len(copies)


# ------------------------------------------------------------------------------------------
# Widget defaults
# ------------------------------------------------------------------------------------------

def build_widgets(title, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for name, (what, subs) in EDITS.items():
        with open(os.path.join(SKINS, "default", "xui", "en", "widgets", name), encoding="utf-8") as fh:
            text = fh.read()
        for old, new in subs:
            if old not in text:
                sys.exit("%s: %s not found (Prestige's widget changed: update EDITS)" % (name, old))
            text = text.replace(old, new)
        cut = text.index("?>") + 2
        note = ("<!-- <Prestige> Firestorm (%s) skin: a copy of skins/default/xui/en/widgets/%s (a skin's\n"
                "     widget file replaces the default one) with Firestorm's look: %s. -->\n" % (title, name, what))
        text = text[:cut] + "\n" + note + text[cut:].lstrip("\n")
        with open(os.path.join(out_dir, name), "w", encoding="utf-8") as fh:
            fh.write(text)


def main():
    if len(sys.argv) != 2 or not os.path.isdir(os.path.join(sys.argv[1], "firestorm")):
        sys.exit(__doc__)
    fs_skins = os.path.abspath(sys.argv[1])
    for folder, title, theme in THEMES:
        skin = os.path.join(SKINS, folder)
        os.makedirs(skin, exist_ok=True)
        n_colors = build_colors(fs_skins, theme, title, os.path.join(skin, "colors.xml"))
        n_icons, n_files = build_textures(fs_skins, theme, title, os.path.join(skin, "textures"))
        build_widgets(title, os.path.join(skin, "xui", "en", "widgets"))
        print("%s: %d colours, %d bitmap icons, %d image files" % (folder, n_colors, n_icons, n_files))


if __name__ == "__main__":
    main()
