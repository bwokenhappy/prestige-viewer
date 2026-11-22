"""The new interface's documents draw from the kit, and its tool grid runs real commands (#703).

Documents (indra/newview/llnewuidocs.h, the windows' llnewuiwindowdocs.h, and camera mode's kCameraRml): no colour, font-family or
font-size literals (the kit's tokens and classes carry them, so a theme or a classic skin is one
table); every $token resolves in both themes; every <ic name="..."/> template is one the kit has;
markup balanced, one data model around everything, every {{variable}} and event callback bound
by its controller (as camera-rml checks camera mode). Every RmlUi document the viewer loads goes
through LLNewUIKit::themed, except the allow-listed ones that are not the new interface.

Tool grid (llnewuitools.cpp): every command in app_settings/commands.xml has its icon in
textures.xml and its functions registered in the source; the default toolbars name only commands
that exist (the grid shows the person's toolbars).
"""
import html
import os
import re

from . import camera_rml

NAME = "newui-kit"
STAGE = "source"
DESCRIPTION = "new interface documents use the kit (tokens, classes, icon templates); the tool grid's commands are valid"
GUARDS = "Hardcoded colours and fonts in new-interface documents (a theme or skin cannot reach them); dead tool grid buttons."

DOCS = "indra/newview/llnewuidocs.h"
WINDOW_DOCS = "indra/newview/llnewuiwindowdocs.h"
# The window framework binds "title" and "active" on every window's model (llnewuiwindow.h).
FRAMEWORK = "indra/newview/llnewuiwindow.cpp"
MORE_DOCS = ("indra/newview/llnewuistatusdocs.h",)  # documents kept in headers of their own
KIT = "indra/newview/llnewuikitstyle.cpp"
CAMERA = "indra/newview/slcameraview.cpp"
# Data model -> the controller that binds it.
CONTROLLERS = {"bar": "indra/newview/llnewui.cpp", "palette": "indra/newview/llnewui.cpp",
               "tools": "indra/newview/llnewuitools.cpp", "chat": "indra/newview/llnewuichat.cpp",
               "people": "indra/newview/llnewuipeople.cpp", "inv": "indra/newview/llnewuiinventory.cpp",
               "status": "indra/newview/llnewuistatus.cpp", "smenu": "indra/newview/llnewuimenus.cpp"}
WINDOW_MODELS = {"chat", "people", "inv"}
# Documents that are not the new interface: the proof overlay and the classic translator.
LOAD_ALLOWED = {"indra/newview/llrmluihost.cpp", "indra/newview/llrmluiclassic.cpp",
                # Synthetic CPU regression embeds the production stylesheet, and
                # deliberately removes clip:always in its failing-baseline arm.
                "indra/newview/tests/newui_clipping_checks.cpp",
                "indra/newview/tests/newui_emoji_checks.cpp"}
COMMANDS = "indra/newview/app_settings/commands.xml"
TOOLBARS = "indra/newview/app_settings/toolbars.xml"
TEXTURES = "indra/newview/skins/default/textures/textures.xml"
SOURCE_DIRS = ("indra/newview", "indra/llui")

COLOUR = re.compile(r"#[0-9a-fA-F]{3,8}\b|\b(?:rgba?|hsla?)\s*\(")


def read(ctx, path):
    with open(ctx.path(path), encoding="utf-8") as f:
        return f.read()


def strip_comments(css):
    return re.sub(r"/\*.*?\*/", "", css, flags=re.S)


def literal_problems(name, rml):
    """Colour / font literals in a document's style, inline styles and data-style bindings."""
    problems = []
    style = "".join(re.findall(r"<style>(.*?)</style>", rml, flags=re.S))
    for sel, body in re.findall(r"([^{}]+)\{([^}]*)\}", strip_comments(style)):
        sel = " ".join(sel.split())
        if COLOUR.search(body):
            problems.append("%s: colour literal in '%s' (use a kit token)" % (name, sel))
        if re.search(r"font-family\s*:", body):
            problems.append("%s: font-family in '%s' (the kit sets the faces)" % (name, sel))
        for value in re.findall(r"font-size\s*:\s*([^;]+)", body):
            if not value.strip().startswith("$"):
                problems.append("%s: font-size %s in '%s' (use $fontCaption / $fontBody / $fontTitle / $fontDisplay)"
                                % (name, value.strip(), sel))
    body = rml[rml.find("<body"):] if "<body" in rml else ""
    for attr in re.findall(r'\s(?:style|data-style-[\w-]+)="([^"]*)"', body):
        if COLOUR.search(attr) or "font-family" in attr or re.search(r"font-size\s*:\s*\d", attr):
            problems.append("%s: colour or font literal in markup: %r" % (name, attr[:60]))
    return problems


def theme_tokens(kit):
    """Token names of the light and the dark table (the common push_backs belong to both)."""
    m = re.search(r"LLNewUIKit::Tokens LLNewUIKit::tokens\(.*?\n\}", kit, flags=re.S)
    body = m.group(0) if m else ""
    light = re.search(r"if \(theme == Theme::Light\)(.*?)else", body, flags=re.S)
    dark = re.search(r"\belse\b(.*?);\s*\n\s*t\.push_back", body, flags=re.S)
    common = set(re.findall(r'push_back\(\{\s*"(\w+)"', body))
    names = lambda block: set(re.findall(r'\{\s*"(\w+)",', block.group(1))) if block else set()
    return names(light) | common, names(dark) | common


def token_problems(where, text, tokens):
    """Every $name resolves: the longest token that prefixes it, then nothing or a 2-digit alpha."""
    problems = []
    ordered = sorted(tokens, key=len, reverse=True)
    for ref in set(re.findall(r"\$([A-Za-z]\w*)", text)):
        hit = next((t for t in ordered if ref.startswith(t)), None)
        rest = ref[len(hit):] if hit else None
        if hit is None or (rest and not re.fullmatch(r"[0-9a-fA-F]{2}", rest)):
            problems.append("%s: $%s is no kit token" % (where, ref))
    return problems


def markup_problems(ctx, name, rml, model):
    """camera-rml's markup checks for a document whose model `model` its controller binds."""
    problems = []
    body = rml[rml.find("<body"):] if "<body" in rml else rml
    flex = camera_rml.flex_selectors(rml)
    kit_sheet = re.search(r'kKit = R"rcss\((.*?)\)rcss"', read(ctx, KIT), flags=re.S)
    if kit_sheet:
        flex |= camera_rml.flex_selectors(kit_sheet.group(1))
    w = camera_rml.Walker(flex)
    w.feed(body)
    w.close()
    problems += ["%s: %s" % (name, p) for p in w.problems]
    if w.model_depth is None:
        problems.append("%s: no data-model element" % name)
    if w.stack:
        problems.append("%s: unclosed at the end: %s" % (name, " > ".join(w.stack)))
    path = CONTROLLERS.get(model)
    if not path:
        return problems + ["%s: data model %r has no controller listed in the newui-kit check" % (name, model)]
    src = read(ctx, path)
    if model in WINDOW_MODELS:
        src += read(ctx, FRAMEWORK)
        for part in ("<whead/>", "<wgrip/>"):
            if part not in body:
                problems.append("%s: a window document without its frame part %s" % (name, part))
    bound = set(re.findall(r'\.Bind(?:Func)?\(\s*"(\w+)"', src))
    callbacks = set(re.findall(r'BindEventCallback\(\s*"(\w+)"', src))
    aliases = set(re.findall(r'data-for="(\w+)\s*:', body)) | {"it_index", "ev"}
    exprs = re.findall(r"\{\{(.*?)\}\}", body) + re.findall(r'data-(?:if|class-[\w-]+|attr-[\w-]+|style-[\w-]+|rml|value|for)="([^"]*)"', body)
    for e in exprs:
        e = html.unescape(e)
        e = re.sub(r"'[^']*'", "", e)
        if " : " in e:
            e = e.split(":", 1)[1]
        for ident in re.findall(r"(?<![\w.])([A-Za-z_]\w*)", e):
            if ident not in ("size",) and ident not in aliases and ident not in bound:
                problems.append("%s: unbound variable %r" % (name, ident))
    # A style bound straight to a string member ('data-style-width="width"', '...="r.deco"') is
    # applied as soon as the document loads, before the controller fills it: an empty default is
    # 'width: ;' / 'decorator: ;' in the viewer's log. Such members start with a value.
    for expr in re.findall(r'data-style-[\w-]+="\s*([A-Za-z_]\w*(?:\.\w+)?)\s*"', body):
        member = expr.split(".")[-1]
        if member in aliases:
            continue
        if not re.search(r'\b%s\s*=\s*"[^"]+"' % re.escape(member), src):
            problems.append("%s: style bound to %r, which has no non-empty default in %s ('%s: ;' before it is set)"
                            % (name, expr, path, member))
    for cb in sorted(set(re.findall(r'data-event-[\w-]+="\s*(\w+)\s*(?:\(|")', body))):
        if cb not in callbacks:
            problems.append("%s: event callback %r is not bound in %s" % (name, cb, path))
    return problems


def command_problems(ctx):
    problems = []
    commands = read(ctx, COMMANDS)
    textures = set(re.findall(r'<texture\s+name="([^"]+)"', read(ctx, TEXTURES)))
    registered = set()
    for d in SOURCE_DIRS:
        for root, _, files in os.walk(ctx.path(d)):
            for f in files:
                if f.endswith((".cpp", ".h")):
                    with open(os.path.join(root, f), encoding="utf-8", errors="replace") as fh:
                        registered |= set(re.findall(r'"([A-Z][\w]*\.[\w.]+)"', fh.read()))
    names = set()
    for block in re.findall(r"<command\b(.*?)/>", commands, flags=re.S):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', block))
        name = attrs.get("name", "?")
        names.add(name)
        if attrs.get("icon") not in textures:
            problems.append("command %s: icon %r is not in textures.xml" % (name, attrs.get("icon")))
        for key in ("execute_function", "execute_stop_function", "is_enabled_function", "is_running_function"):
            fn = attrs.get(key)
            if fn and fn not in registered:
                problems.append("command %s: %s %r is registered nowhere in the source" % (name, key, fn))
    for name in re.findall(r'<command\s+name="([^"]+)"', read(ctx, TOOLBARS)):
        if name not in names:
            problems.append("toolbars.xml: command %r is not in commands.xml (its button would be dropped)" % name)
    return problems, len(names)


def run(ctx, result):
    problems = []
    try:
        kit = read(ctx, KIT)
        docs_src = read(ctx, DOCS) + "".join(read(ctx, d) for d in MORE_DOCS)
        camera_src = read(ctx, CAMERA)
    except OSError as e:
        result.fail("missing: %s" % e)
        return
    sheet = re.search(r'kKit = R"rcss\((.*?)\)rcss"', kit, flags=re.S)
    if not sheet:
        result.fail("no kKit stylesheet in %s" % KIT)
        return
    icons = set(re.findall(r'\{\s*"(\w+)",\s*\{', kit))
    light, dark = theme_tokens(kit)
    if not light or not dark:
        result.fail("could not read the theme token tables in %s" % KIT)
        return
    for extra in sorted(light ^ dark):
        problems.append("token %r is in one theme only" % extra)
    tokens = light & dark
    problems += token_problems("kit", strip_comments(sheet.group(1)), tokens)

    docs = re.findall(r'(k\w+Rml) = R"rml\((.*?)\)rml"', docs_src, flags=re.S)
    try:
        docs += re.findall(r'(k\w+Rml) = R"rml\((.*?)\)rml"', read(ctx, WINDOW_DOCS), flags=re.S)
    except OSError as e:
        problems.append("missing: %s" % e)
    if not docs:
        result.fail("no documents in %s" % DOCS)
        return
    camera = camera_rml.rml_of(camera_src)
    if camera is None:
        problems.append("no kCameraRml in %s" % CAMERA)
    else:
        docs.append(("kCameraRml", camera))
    for name, rml in docs:
        problems += literal_problems(name, rml)
        problems += token_problems(name, rml, tokens)
        for icon in re.findall(r'<ic\s+name="(\w+)"', rml):
            if icon not in icons:
                problems.append("%s: <ic name=%r/> is no kit icon (%s)" % (name, icon, ", ".join(sorted(icons))))
        # A decorator built in the markup from a value ('llimage(' + r.icon + ')') is 'llimage()'
        # for an empty value, which the viewer's instancer refuses (a warning per row): the
        # controller binds the whole decorator, 'none' when there is no image.
        if re.search(r"data-style-decorator=\"[^\"]*'llimage\(", rml):
            problems.append("%s: llimage() assembled in markup (bind the decorator, 'none' when empty)" % name)
        if name != "kCameraRml":  # camera-rml checks camera mode's markup
            model = re.search(r'data-model="(\w+)"', rml)
            problems += markup_problems(ctx, name, rml, model.group(1) if model else "?")

    # Every document the viewer loads goes through the kit (outside the allow-list).
    newview = ctx.path("indra/newview")
    for root, _, files in os.walk(newview):
        for f in files:
            if not f.endswith(".cpp"):
                continue
            path = os.path.join(root, f)
            rel = os.path.relpath(path, ctx.path(".")).replace(os.sep, "/")
            if rel in LOAD_ALLOWED:
                continue
            with open(path, encoding="utf-8", errors="replace") as fh:
                for no, line in enumerate(fh, 1):
                    if "LoadDocumentFromMemory(" in line and "themed(" not in line:
                        problems.append("%s:%d loads a document without LLNewUIKit::themed" % (rel, no))

    # Bottom-anchored history has negative overflow; RmlUi's automatic clip test
    # sees no larger scroll height. This protects rendering AND Context hit-testing.
    viewport = re.search(r"\.viewport\s*\{([^}]+)\}", strip_comments(sheet.group(1)))
    if not viewport or not re.search(r"clip\s*:\s*always\s*;", viewport.group(1)) or not re.search(r"overflow\s*:\s*hidden\s*;", viewport.group(1)):
        problems.append("viewport: square list/history viewports must use overflow:hidden and clip:always (negative overflow must not cover title bars)")
    window_docs = read(ctx, WINDOW_DOCS)
    if 'id="historyBar"' not in window_docs or 'id="historyThumb"' not in window_docs:
        problems.append("chat: bounded history needs its interactive scrollbar")

    chat = read(ctx, CONTROLLERS["chat"]) + read(ctx, "indra/newview/llnewuichatmarkup.cpp")
    people = read(ctx, CONTROLLERS["people"])
    fonts = read(ctx, "indra/newview/llnewuikit.cpp")
    if '"TwemojiSVG.ttf"' not in fonts or 'slRmlSetSvgHooks(&hooks)' not in fonts:
        problems.append("emoji: native font library must load Twemoji and classic OT-SVG hooks")
    if 'LLUrlRegistry::instance().findUrl(rest, match, changed, false, false, &state)' not in chat:
        problems.append("chat markup: reuse the classic untrusted URL registry")
    if 'style.is_link.isProvided() && !style.is_link()' not in chat or 'escapeText(match.getLabel())' not in chat:
        problems.append("chat markup: nolink labels must stay non-clickable and escaped")
    if 'LLUrlAction::clickAction(url, false)' not in chat or 'data-rml="m.rich"' not in window_docs:
        problems.append("chat markup: escaped rich view and untrusted classic click action must stay wired")
    if 'if (mRangeLimited) LLWorld::getInstance()->getAvatars(&ids, &positions, me, mRange);' not in people or 'else LLWorld::getInstance()->getAvatars(&ids, &positions);' not in people:
        problems.append("nearby: obey the classic optional radar range, not an unconditional chat/distance cutoff")
    if 'data-attr-disabled="!rangeLimited"' not in window_docs or 'data-event-change="rangeChanged()"' not in window_docs:
        problems.append("nearby: the range slider and limit switch must stay connected")
    if 'std::unique_ptr<LLNetMap>' not in people or 'drawClassicOverlays' not in read(ctx, FRAMEWORK):
        problems.append("nearby: embedded map must reuse classic LLNetMap")

    if 'mView == Nearby ? "zoom"' not in people or 'SLAvatarActions::zoomIn(who)' not in people:
        problems.append("nearby: double-click must use the classic avatar camera zoom, not IM")
    if 'data-event-click="rowIM(it_index)"' not in window_docs or '"rowIM"' not in people:
        problems.append("nearby: IM must remain one click away on each row")
    for action in ("zoom", "track", "face", "teleport_to", "history", "contact_set", "report", "copy_full", "copy_display", "copy_username", "copy_slurl", "copy_id", "render_normal", "render_never", "render_always", "freeze", "eject", "estate_home", "estate_kick", "estate_ban"):
        if '"' + action + '"' not in people:
            problems.append("nearby: missing classic action " + action)

    if 'LLNearbyChatLog::instance().addChangedCallback' not in chat or 'addNewMsgCallback' not in chat:
        problems.append("chat latency: subscribe to Nearby and IM model events")
    sync_body=chat[chat.find("bool sync(F64 now) override"):]
    if not (0 <= sync_body.find("if (mMessagesPending)") < sync_body.find("if (now < mNext")):
        problems.append("chat latency: message updates must bypass the metadata refresh timer")
    if 'line.sameMessage(mLines[reuse])' not in chat:
        problems.append("chat append: preserve unchanged rich strings and existing row DOM")

    if not re.search(r'if \(showName\)\s*if \(const auto it = radar.find', people):
        problems.append("nearby privacy: cached radar labels must not bypass the live name policy")

    if not (0 <= sync_body.find("if (!mQueued.empty()) flushQueued();") < sync_body.find("if (now < mNext")):
        problems.append("chat latency: the bounded session-initialization queue must bypass the metadata timer")
    if 'queued.first=new_session_id' not in chat or 'removeSessionObserver(this)' not in chat:
        problems.append("chat sessions: rebind queued messages with the model, and unregister the view observer")

    if 'chat.mTimeStr.empty() ? appendTime() : chat.mTimeStr' not in read(ctx, "indra/newview/llfloaterimnearbychat.cpp"):
        problems.append("nearby chat: publish a classic-formatted timestamp when the raw event has none")

    bar_docs=read(ctx, "indra/newview/llnewuidocs.h")
    if '#navscrim { pointer-events: auto; }' not in bar_docs or 'id="navscrim" class="scrim clear"' not in bar_docs:
        problems.append("navigation: the outside-click scrim must opt back into pointer input")

    if 'flag(e.notes, showDetails && !data["notes"].asString().empty()' not in people or 'if (showDetails && !data["age"]' not in people:
        problems.append("nearby readouts: personal-note presence and age must obey the live privacy policy")

    # Every decorative image, not only toolbar commands, must resolve to a shipped asset.
    import xml.etree.ElementTree as ET
    texture_table = {item.get("name"): item for item in ET.fromstring(read(ctx, TEXTURES)) if item.tag == "texture"}
    for name in set(re.findall(r"llimage\(([A-Za-z0-9_.-]+)", docs_src + read(ctx, WINDOW_DOCS))):
        entry = texture_table.get(name)
        if entry is None:
            problems.append("native icon %r is not in textures.xml" % name)
        elif entry.get("vector_file") and not os.path.isfile(ctx.path("indra/newview/skins/default/textures/" + entry.get("vector_file"))):
            problems.append("native icon %r has no shipped vector" % name)
    if "LLFontGL::getFallbackFontFiles()" not in fonts or "LLWindow::findFallbackFontForChar" not in fonts:
        problems.append("native text must share classic configured and on-demand system fallback")
    fallback_patch = read(ctx, "indra/cmake/PatchRmlUiFallback.cmake")
    if "FontFaceLoadResult::Duplicate, face.face.get()" not in fallback_patch or "result == FontFaceLoadResult::Error" not in fallback_patch:
        problems.append("preloaded host fonts must be promotable to native fallback faces")
    if 'slRmlSetFallbackResolver(resolveNativeGlyph)' not in fonts:
        problems.append("native missing glyphs must reach the system resolver before replacement")

    if "Rml::ScrollParentage::Closest" not in chat or "conversationOption-" not in window_docs:
        problems.append("picker keyboard selection must reveal its row without scrolling chat history")

    if "field->IsVisible(true) && field->Focus()" not in chat:
        problems.append("picker focus must remain pending until the data-if field is visible and focused")
    if "owner && owner == LLNewUIWindows::instance().activeWindow()" not in read(ctx, "indra/newview/llnewui.cpp"):
        problems.append("deferred native input focus must only adopt the active native window")

    cmd_problems, count = command_problems(ctx)
    problems += cmd_problems
    for p in sorted(set(problems)):
        result.fail(p)
    if not problems:
        result.summary = "%d documents on the kit's tokens and classes, %d commands valid" % (len(docs), count)
