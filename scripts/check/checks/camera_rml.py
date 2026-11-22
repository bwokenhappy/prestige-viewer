"""Regression guard for camera mode's overlay document (indra/newview/slcameraview.cpp, kCameraRml, #1572).

RmlUi tolerates unbalanced markup silently: one stray closing tag ended the data-model element early, and the
rest of the overlay showed raw {{bindings}} with every panel open. This check takes the RML out of the source
and requires: every tag closed in order, everything inside the one data-model element, and every {{variable}}
and data-* expression naming a variable the model binds (m.Bind / m.BindFunc / data-for aliases).
"""
import os
import re
from html.parser import HTMLParser

NAME = "camera-rml"
STAGE = "source"
DESCRIPTION = "camera mode's overlay document: balanced tags, one data model around everything, bound variables"
GUARDS = "Raw {{bindings}} and every panel open at once in camera mode (a stray closing tag)."

SOURCE = "indra/newview/slcameraview.cpp"
KIT = "indra/newview/llnewuikitstyle.cpp"
VOID = {"img", "input", "br"}


def rml_of(text, var="kCameraRml"):
    m = re.search(var + r' = R"rml\((.*?)\)rml"', text, flags=re.S)
    return m.group(1) if m else None


def flex_selectors(rml):
    """Class and id names whose rule sets display: flex (simple selectors' last part)."""
    names = set()
    for sel, body in re.findall(r"([^{}]+)\{([^}]*)\}", rml):
        if re.search(r"display:\s*flex", body):
            for one in sel.split(","):
                last = one.strip().split()[-1] if one.strip() else ""
                for part in re.findall(r"[.#][\w-]+", last):
                    names.add(part)
    return names


class Walker(HTMLParser):
    def __init__(self, flex=()):
        super().__init__(convert_charrefs=True)
        self.stack, self.problems, self.model_depth, self.after_model = [], [], None, False
        self.flex, self.attrs = set(flex), []

    def handle_starttag(self, tag, attrs):
        if tag in VOID:
            return
        a = dict(attrs)
        if self.model_depth is not None and len(self.stack) < self.model_depth and tag not in ("body", "rml"):
            self.problems.append("<%s> at line %d is outside the data-model element" % (tag, self.getpos()[0]))
        self.stack.append(tag)
        self.attrs.append(a)
        if "data-model" in a:
            self.model_depth = len(self.stack)

    def handle_startendtag(self, tag, attrs):
        pass

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack:
            if tag == "rml":
                return  # the body is read from <body> on
            self.problems.append("</%s> at line %d closes nothing" % (tag, self.getpos()[0]))
            return
        if self.stack[-1] != tag:
            self.problems.append("</%s> at line %d closes <%s>" % (tag, self.getpos()[0], self.stack[-1]))
            return
        if self.model_depth is not None and len(self.stack) == self.model_depth:
            # the data model ends: only </body> and </rml> may follow
            self.after_model = True
        self.stack.pop()
        self.attrs.pop()

    def handle_data(self, data):
        # RmlUi draws no bare text directly inside a flex container: it must sit in an element.
        if data.strip() and self.attrs:
            a = self.attrs[-1]
            names = {"." + c for c in (a.get("class") or "").split()} | ({"#" + a["id"]} if a.get("id") else set())
            if names & self.flex:
                self.problems.append("bare text %r directly in flex container <%s %s> at line %d (wrap it in an element)"
                                     % (data.strip()[:30], self.stack[-1], " ".join(sorted(names & self.flex)), self.getpos()[0]))
        if self.after_model and self.stack and self.stack[-1] not in ("body", "rml") and data.strip():
            self.problems.append("text after the data-model element at line %d" % self.getpos()[0])


def run(ctx, result):
    problems = document_problems(ctx, SOURCE, "kCameraRml")
    for p in problems:
        result.fail(p)
    if not problems:
        result.summary = "balanced, one data model, every variable and event callback bound"


def document_problems(ctx, source, var, rml_source=None):
    """The problems of one kit overlay document (the RML string `var` in `source`, or in `rml_source`
    when the document lives in its own header); also used by video_studio_rml.py for Video Studio's
    overlay (#1668)."""
    text = ""
    for rel in [source] + ([rml_source] if rml_source else []):
        try:
            with open(ctx.path(rel), encoding="utf-8") as f:
                text += f.read() + "\n"
        except OSError:
            return ["%s is missing" % rel]
    rml = rml_of(text, var)
    if rml is None:
        return ["no %s in %s" % (var, source)]
    body = rml[rml.find("<body"):] if "<body" in rml else rml
    flex = flex_selectors(rml)
    try:
        with open(ctx.path(KIT), encoding="utf-8") as f:
            kit = re.search(r'kKit = R"rcss\((.*?)\)rcss"', f.read(), flags=re.S)
    except OSError:
        kit = None
    if not kit:
        return ["no kKit stylesheet in %s (the flex rules come from it)" % KIT]
    flex |= flex_selectors(kit.group(1))
    w = Walker(flex)
    w.feed(body)
    w.close()
    problems = list(w.problems)
    if w.model_depth is None:
        problems.append("no data-model element")
    if w.stack:
        problems.append("unclosed at the end: " + " > ".join(w.stack))
    # Every top-level variable used in {{...}} or data-* expressions is bound (or a data-for alias).
    bound = set(re.findall(r'm\.Bind(?:Func)?\("(\w+)"', text)) | set(re.findall(r'std::make_pair\("(\w+)", &View::', text))
    aliases = set(re.findall(r'data-for="(\w+)\s*:', body)) | {"it_index"}
    exprs = re.findall(r"\{\{(.*?)\}\}", body) + re.findall(r'data-(?:if|class-[\w-]+|attr-[\w-]+|style-[\w-]+|value|for)="([^"]*)"', body)
    for e in exprs:
        e = re.sub(r"'[^']*'", "", e)  # string literals
        if ":" in e and " : " in e:
            e = e.split(":", 1)[1]
        for name in re.findall(r"(?<![\w.])([A-Za-z_]\w*)", e):
            if name in ("size",) or name in aliases or name in bound:
                continue
            problems.append("unbound variable %r in %r" % (name, e.strip()))
    # Every event callback the document calls is bound, under a name RmlUi accepts: it refuses a
    # reserved name ("size" once left the size menu's rows dead) with only a log line.
    reserved = {"it", "it_index", "ev", "true", "false", "size", "literal"}
    callbacks = set(re.findall(r'BindEventCallback\(\s*"(\w+)"', text))
    for name in sorted((bound | callbacks) & reserved):
        problems.append("%r is a name RmlUi reserves: the binding is refused" % name)
    # A callback is called as name(...) or named bare (data-event-click="name").
    for name in sorted(set(re.findall(r'data-event-[\w-]+="\s*(\w+)\s*(?:\(|")', body))):
        if name not in callbacks or name in reserved:
            problems.append("event callback %r is not bound" % name)
    return sorted(set(problems))
