#!/usr/bin/env python3
"""Link failures name the objects that block the link (#1418).

LLSelectMgr::linkObjects() keeps its checks in their old order and meaning; each failure that
can name its offenders tries the named notice first (with "Deselect these" / "Select only
these") and falls back to the old generic one. The rules themselves (which roots fail, the
majority owner / region, the capped list) are tested in sllinkblockers_test.cpp; this checks
the wiring that only exists in source.

Usage:
  python3 <source>/indra/newview/tests/link_blockers_checks.py --source-root <source>
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

FAILURES: list[str] = []


def check(cond: bool, what: str) -> None:
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILURES.append(what)


def function_body(src: str, signature: str) -> str:
    start = src.find(signature)
    if start < 0:
        return ""
    brace = src.find("{", start)
    depth = 0
    for i in range(brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[brace:i + 1]
    return ""


def notification(xml: str, name: str) -> str:
    m = re.search(r'<notification\b[^>]*\bname="%s"[^>]*>(.*?)</notification>' % re.escape(name), xml, re.S)
    return m.group(0) if m else ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True)
    args = ap.parse_args()
    root = pathlib.Path(args.source_root)
    nv = root / "indra/newview"
    sel = (nv / "llselectmgr.cpp").read_text(encoding="utf-8")
    notes = (nv / "skins/default/xui/en/notifications.xml").read_text(encoding="utf-8")
    strings = (nv / "skins/default/xui/en/strings.xml").read_text(encoding="utf-8")
    cmake = (nv / "CMakeLists.txt").read_text(encoding="utf-8")

    link = function_body(sel, "bool LLSelectMgr::linkObjects()")
    check(bool(link), "linkObjects() found")

    # The checks run in the same order as before, and the link is still sent last.
    order = ["selectGetAllRootsValid()", "MAX_CHILDREN_PER_TASK", "getRootObjectCount() < 2",
             "selectGetRootsModify()", "selectGetRootsNonPermanentEnforced()", "selectGetOwner(owner_id, owner_name)",
             "selectGetSameRegion()", "sendLink()"]
    pos = [link.find(token) for token in order]
    check(all(p >= 0 for p in pos) and pos == sorted(pos), "linkObjects keeps its checks in the old order")

    # Each failure names its objects first and keeps the old notice as the fallback.
    for rule, named, generic in [("Downloading", "UnableToLinkWhileDownloadingNamed", "UnableToLinkWhileDownloading"),
                                 ("Modify", "CannotLinkModifyNamed", "CannotLinkModify"),
                                 ("Permanent", "CannotLinkPermanentNamed", "CannotLinkPermanent"),
                                 ("Owner", "CannotLinkDifferentOwnersNamed", "CannotLinkDifferentOwners"),
                                 ("Region", "CannotLinkAcrossRegionsNamed", "CannotLinkAcrossRegions")]:
        pat = (r'if \(!sl_notify_link_blockers\(SLLinkRule::%s, "%s", (true|false)\)\)\s*\{\s*'
               r'LLNotificationsUtil::add\("%s"\);' % (rule, named, generic))
        check(re.search(pat, link) is not None, "%s: named notice, generic fallback" % rule)
        body = notification(notes, named)
        check(bool(body) and "[OBJECTS]" in body and "[COUNT]" in body, "%s: notification with [COUNT] and [OBJECTS]" % named)
        check(bool(notification(notes, generic)), "%s: generic notification kept" % generic)
        if rule != "Downloading":
            check('name="linkblockerbuttons"' in body, "%s: Deselect / Select only / Close buttons" % named)

    check("[OWNER]" in notification(notes, "CannotLinkDifferentOwnersNamed"), "owner notice names the majority owner")
    check("[REGION]" in notification(notes, "CannotLinkAcrossRegionsNamed"), "region notice names the majority region")

    tmpl = re.search(r'<template name="linkblockerbuttons">(.*?)</template>', notes, re.S)
    t = tmpl.group(1) if tmpl else ""
    buttons = re.findall(r'<button\b(.*?)/>', t, re.S)
    idx = [re.search(r'index="(\d)"', b).group(1) for b in buttons]
    texts = [re.search(r'text="([^"]*)"', b).group(1) for b in buttons]
    check(idx == ["0", "1", "2"] and texts == ["Deselect these", "Select only these", "Close"],
          "button template: 0 Deselect these, 1 Select only these, 2 Close")
    check(len(buttons) == 3 and 'default="true"' in buttons[2], "Close is the default button")

    for name, key in [("LinkBlockersMore", "[N]"), ("LinkBlockerLoading", "[POS]"),
                      ("LinkBlockerSameName", "[NAME]"), ("LinkBlockerOwnedBy", "[OWNER]")]:
        m = re.search(r'<string name="%s">([^<]*)</string>' % name, strings)
        check(m is not None and key in m.group(1), "strings.xml %s has %s" % (name, key))

    # The buttons act on whatever is selected when they are pressed.
    resp = function_body(sel, "void sl_link_blockers_response(")
    check("gObjectList.findObject" in resp and "isDead()" in resp, "response: skips objects that died")
    check(re.search(r"isSelected\(\)\)\s*\{[^}]*deselectObjectAndFamily\(object, true, true\)", resp) is not None,
          "Deselect these: only objects still selected, with their link set")
    check(re.search(r"deselectAll\(\);\s*select_mgr->selectObjectAndFamily\(objects\);", resp) is not None,
          "Select only these: the live offenders replace the selection")
    check('notification["payload"]["ids"]' in resp, "response: offenders travel in the payload")

    # One pass over the roots, only when a link check failed.
    check(len(re.findall(r"\bsl_collect_link_roots\(", sel)) == 2, "roots are collected from one place")
    check(len(re.findall(r"\bsl_notify_link_blockers\(", sel)) - 1 == len(re.findall(r"\bsl_notify_link_blockers\(", link)),
          "named notices are only raised from linkObjects")
    names = function_body(sel, "void sl_collect_link_roots(")
    check("mName" in names and "<nolink>" in names, "names from the selection node, never turned into links")

    check(re.search(r"^\s+sllinkblockers\.cpp$", cmake, re.M) is not None, "sllinkblockers.cpp is built")

    print("%s (%d failed)" % ("FAILED" if FAILURES else "PASSED", len(FAILURES)))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
