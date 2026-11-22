"""Every classic menu item and status bar part has a home in the new interface (#703).

UIRenderer 2 hides the classic menu bar and status bar in world (llnewuimenus.h). This check holds
that nothing is lost:

Menus (skins/default/xui/en/menu_viewer.xml): every call / check item is reached by the command
index's walk of the menu tree (llcommandindex.cpp walkMenu): a label, at most its depth, under
menus the classic viewer shows (or shows on request: Advanced, Develop, Admin, RLVa, Combat; or
Advanced > Shortcuts, which the index walks always). Items the classic viewer itself never shows
are listed as such. The "more" menu's items (llnewuimenus.cpp menuItem("...")) exist. The table of
every item and its new home, doc/newui-menus.md, is generated from the same walk and must be
current: regenerate with
    python3 -I scripts/check/run.py --only newui-menus --opt write=1

Status bar (panel_status_bar.xml, and the graphs llstatusbar.cpp adds): every part maps to a part
of the new status bar (llnewuistatusdocs.h) or a stated home; each new part's element exists; every
click('x') of the document is handled by the controller; every part's setting exists.
"""
import os
import re
import xml.etree.ElementTree as ET

NAME = "newui-menus"
STAGE = "source"
DESCRIPTION = "every classic menu item and status bar part is reachable in the new interface; doc/newui-menus.md is current"
GUARDS = "A menu item or status bar part with no home once UIRenderer 2 hides the classic menu bar and status bar."

MENU = "indra/newview/skins/default/xui/en/menu_viewer.xml"
STATUS_XML = "indra/newview/skins/default/xui/en/panel_status_bar.xml"
STATUS_DOCS = "indra/newview/llnewuistatusdocs.h"
STATUS_CPP = "indra/newview/llnewuistatus.cpp"
MENUS_CPP = "indra/newview/llnewuimenus.cpp"
INDEX_CPP = "indra/newview/llcommandindex.cpp"
COMMANDS = "indra/newview/app_settings/commands.xml"
SETTINGS = "indra/newview/app_settings/settings.xml"
TABLE = "doc/newui-menus.md"

MAX_DEPTH = 6  # walkMenu: `if (!menu || depth > 6) return;` (the bar is depth 0)
# Top-level menus shown on request in the classic viewer: indexed while shown.
GATED = {"Advanced": "while the Advanced menu is on (Ctrl+Alt+D; More menu)",
         "Develop": "while the Develop menu is on (Ctrl+Alt+Q; More menu)",
         "Admin": "while shown (god mode)",
         "RLVa Main": "while RLVa shows it",
         "Combat": "while CombatFX shows it"}
ALWAYS = {"Shortcuts"}  # walkMenu indexes these hidden menus anyway (alwaysIndexed)
NEVER = {"Deprecated": "hidden in classic too (deprecated duplicates)"}

# Items with another home besides the command surface (by item name).
MORE = "More menu"
EXTRA = {
    "Preferences": MORE, "Show Advanced Menu": MORE, "Debug Mode": MORE, "Slipstream Help": MORE,
    "Report Bug": MORE, "About Second Life": MORE, "Quit": MORE,
    "transaction_log": "L$ menu (status bar)", "Buy and Sell L$": "L$ menu: Buy L$ (status bar)",
    "About Land": "status bar: parcel (click)", "Performance": "status bar: frame rate (click)",
    "Statistics Bar": "status bar: frame rate (Shift+click), network (click)",
    "Gesture Sets": "status bar: gesture sets",
}

# The classic status bar's parts -> their new home: (element id in kStatusRml or None, words).
STATUS_MAP = {
    "status": (None, "the bar itself: the new status bar (UINewStatusBar)"),
    "menu_search_panel": (None, "menu search: the command surface (Ctrl+K)"),
    "search_menu_edit": (None, "menu search: the command surface (Ctrl+K)"),
    "balance_bg": ("st-money", "L$ balance"),
    "balance": ("st-money", "L$ balance: click for its menu (refresh, hide)"),
    "buyL": ("st-money", "L$ menu: Buy L$"),
    "goShop": ("st-money", "L$ menu: Shop (Marketplace)"),
    "TimeText": ("st-time", "time; the date in its tip"),
    "presets_icon_camera": ("st-camera", "camera presets (its pull-down)"),
    "presets_icon_graphic": ("st-presets", "graphics presets (its pull-down)"),
    "gesture_sets_icon": ("st-gestures", "gesture sets (its pull-down)"),
    "media_toggle_btn": ("st-media", "media: play / pause all; resting: nearby media"),
    "volume_btn": ("st-volume", "volume: mute; resting: the volume sliders"),
    "FPSText": ("st-fps", "frame rate: click timings, Shift+click statistics"),
    "BandwidthGraph": ("st-bw", "bandwidth (kbps)"),
    "PacketLossPercent": ("st-net", "ping and packet loss"),
}


def read(ctx, path):
    with open(ctx.path(path), encoding="utf-8") as f:
        return f.read()


def keys_of(shortcut):
    if not shortcut:
        return ""
    names = {"control": "Ctrl", "shift": "Shift", "alt": "Alt"}
    parts = shortcut.split("|")
    return "+".join(names.get(p, p.upper() if len(p) == 1 else p.capitalize()) for p in parts)


def clean(label):
    label = " ".join((label or "").split())
    return label.rstrip(". :")


def floater_of(item):
    """The floater an item opens (Floater.Show / Toggle ... parameter), or None."""
    for child in item:
        if child.tag.endswith("on_click") and (child.get("function") or "").startswith("Floater.") and child.get("parameter"):
            return child.get("parameter")
    return None


def walk(node, path, depth, gate, out, problems):
    """Mirror of walkMenu: (path, name, label, keys, home) for every call / check item."""
    for c in node:
        if c.tag == "menu":
            name, label = c.get("name") or "", clean(c.get("label"))
            sub_gate = gate
            hidden = c.get("visible") == "false"
            if depth == 0 and name in GATED:
                sub_gate = GATED[name]
            elif name in NEVER:
                sub_gate = "NEVER:" + NEVER[name]
            elif hidden and name in ALWAYS:
                sub_gate = ""  # walked always (keys only in classic)
            elif hidden:
                problems.append("menu %r (%s) is hidden and not indexed: add it to GATED / ALWAYS / NEVER" % (name, " > ".join(path)))
                continue
            if depth + 1 > MAX_DEPTH:
                problems.append("menu %r is deeper than the index walks (%d)" % (name, MAX_DEPTH))
                continue
            walk(c, path + [label or name], depth + 1, sub_gate, out, problems)
        elif c.tag in ("menu_item_call", "menu_item_check"):
            name, label = c.get("name") or "", clean(c.get("label"))
            keys = keys_of(c.get("shortcut"))
            if gate and gate.startswith("NEVER:"):
                home = gate[6:] + ("; its keys work" if keys else "")
            elif c.get("visible") == "false" and not any(ch.tag.endswith("on_visible") for ch in c):
                home = "hidden in classic too" + ("; its keys work" if keys else "")
            elif not label:
                problems.append("item %r (%s) has no label: the command surface cannot list it" % (name, " > ".join(path)))
                continue
            else:
                home = "Command surface"
                if c.get("visible") == "false" or any(ch.tag.endswith("on_visible") for ch in c):
                    home += " (while shown, as in classic)"
                if gate:
                    home += " " + gate
                elif path and path[-1] == "Shortcuts":
                    home += " (keys-only menu in classic)"
                else:
                    home += "; More > Browse the classic menus"
            out.append((path, name, label, keys, home, floater_of(c)))


def table(rows, tool_floaters):
    lines = ["# New interface: where the classic menu items went (#703)", "",
             "Generated by the newui-menus check (`python3 -I scripts/check/run.py --only newui-menus --opt write=1`)",
             "from `menu_viewer.xml`. In the new interface (UIRenderer 2) the classic menu bar and status bar",
             "are hidden in world; every item below keeps its keys (the hidden menu bar stays enabled), and the",
             "command surface (Ctrl+K) lists it with its keys and its enabled / checked state.", "",
             "| Menu | Item | Keys | New home |", "| --- | --- | --- | --- |"]
    for path, name, label, keys, home, floater in rows:
        homes = [home]
        if name in EXTRA:
            homes.append(EXTRA[name])
        if floater and floater in tool_floaters:
            homes.append("tool grid: %s (on a toolbar)" % tool_floaters[floater])
        cell = lambda t: t.replace("|", "\\|")
        lines.append("| %s | %s | %s | %s |" % (cell(" > ".join(path)), cell(label or name), cell(keys), cell("; ".join(homes))))
    lines += ["", "## Status bar parts", "", "| Classic part | New home |", "| --- | --- |"]
    for name, (_, words) in STATUS_MAP.items():
        lines.append("| %s | %s |" % (name, words))
    return "\n".join(lines) + "\n"


def status_problems(ctx):
    problems = []
    xml = read(ctx, STATUS_XML)
    names = set(re.findall(r'\bname="([^"]+)"', re.sub(r"<panel\.string.*?</panel\.string>", "", xml, flags=re.S)))
    bar_src = read(ctx, "indra/newview/llstatusbar.cpp")
    names |= set(re.findall(r'\.name\("([^"]+)"\)', bar_src))
    names -= {"search_button", "clear_button"}
    docs = read(ctx, STATUS_DOCS)
    status_rml = re.search(r'kStatusRml = R"rml\((.*?)\)rml"', docs, flags=re.S)
    if not status_rml:
        return ["no kStatusRml in %s" % STATUS_DOCS]
    rml = status_rml.group(1)
    ids = set(re.findall(r'\bid="([\w-]+)"', rml))
    for n in sorted(names):
        if n not in STATUS_MAP:
            problems.append("status bar part %r has no new home (add it to STATUS_MAP)" % n)
        elif STATUS_MAP[n][0] and STATUS_MAP[n][0] not in ids:
            problems.append("status bar part %r maps to #%s, which kStatusRml lacks" % (n, STATUS_MAP[n][0]))
    src = read(ctx, STATUS_CPP)
    for part in sorted(set(re.findall(r"click\('(\w+)'\)", rml))):
        if 'part == "%s"' % part not in src:
            problems.append("kStatusRml: click('%s') is not handled in %s" % (part, STATUS_CPP))
    settings = read(ctx, SETTINGS)
    for control in re.findall(r'\{ "show\w+", "(\w+)", "', src) + ["UINewStatusBar"]:
        m = re.search(r"<key>%s</key>\s*<map>(.*?)</map>" % re.escape(control), settings, flags=re.S)
        if not m:
            problems.append("status bar setting %s is not in settings.xml" % control)
        elif control.startswith("UINew") and not re.search(r"<key>Persist</key>\s*<integer>1</integer>", m.group(1)):
            problems.append("status bar setting %s does not persist" % control)
    return problems


def run(ctx, result):
    problems = []
    try:
        root = ET.parse(ctx.path(MENU)).getroot()
        menus_src = read(ctx, MENUS_CPP)
        index_src = read(ctx, INDEX_CPP)
        commands = read(ctx, COMMANDS)
    except (OSError, ET.ParseError) as e:
        result.fail("cannot read: %s" % e)
        return
    # The index's own rules this check mirrors: keep them in step.
    if "depth > %d" % MAX_DEPTH not in index_src:
        problems.append("%s: walkMenu's depth limit changed (this check assumes %d)" % (INDEX_CPP, MAX_DEPTH))
    for name in ALWAYS:
        if 'name == "%s"' % name not in index_src:
            problems.append("%s: alwaysIndexed() no longer names %r" % (INDEX_CPP, name))
    rows = []
    walk(root, [], 0, "", rows, problems)
    names = {r[1] for r in rows}
    for name in re.findall(r'menuItem\("([^"]+)"\)', menus_src):
        if name not in names:
            problems.append("%s: the more menu's item %r is not in menu_viewer.xml" % (MENUS_CPP, name))
    for name in EXTRA:
        if name not in names:
            problems.append("EXTRA names %r, which menu_viewer.xml lacks" % name)
    tool_floaters = {}
    for block in re.findall(r"<command\b(.*?)/>", commands, flags=re.S):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', block))
        if (attrs.get("execute_function") or "").startswith("Floater.") and attrs.get("execute_parameters"):
            tool_floaters.setdefault(attrs["execute_parameters"], attrs.get("name", "?"))
    problems += status_problems(ctx)

    text = table(rows, tool_floaters)
    path = ctx.path(TABLE)
    if ctx.options.get("write") == "1":
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        try:
            current = open(path, encoding="utf-8").read()
        except OSError:
            current = None
        if current != text:
            problems.append("%s is not current: regenerate it (--only newui-menus --opt write=1)" % TABLE)
    for p in problems:
        result.fail(p)
    if not problems:
        indexed = sum(1 for r in rows if r[4].startswith("Command surface"))
        result.summary = "%d menu items: %d in the command surface, %d hidden in classic too; %d status bar parts mapped" % (
            len(rows), indexed, len(rows) - indexed, len(STATUS_MAP))
