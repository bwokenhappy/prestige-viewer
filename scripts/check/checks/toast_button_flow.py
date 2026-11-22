"""Regression guard for the buttons of notifications shown in chat / IM (indra/newview/lltoastbuttonflow.h).

Compiles scripts/check/test_toast_button_flow.cpp with the host C++ compiler and runs it: buttons shrunk to
their labels flow left to right, a new row only when the next one does not fit, each row from the left edge.

Source rule:
  FLOW  LLIMToastNotifyPanel::compactButtons places a non-script notification's buttons through
        lltoastflow::flow (never one running offset over the toast's rows: an inventory offer's Block sat alone
        on its row, pushed right).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "toast-button-flow"
STAGE = "source"
DESCRIPTION = "buttons of notifications in chat / IM flow into rows from the left edge (tests)"
GUARDS = "An inventory offer's Block button alone on a row, pushed right, in chat / IM."

HEADER = "indra/newview/lltoastbuttonflow.h"
TEST = "scripts/check/test_toast_button_flow.cpp"
PANEL = "indra/newview/lltoastnotifypanel.cpp"


def _code(ctx, rel):
    """The file without comments ("" when it is missing)."""
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return re.sub(r"//[^\n]*|/\*.*?\*/", "", f.read(), flags=re.S)
    except OSError:
        return ""


def source_rules(ctx):
    bad = []
    c = _code(ctx, PANEL)
    m = re.search(r"void\s+LLIMToastNotifyPanel::compactButtons\s*\(\s*\)\s*\{(.*?)\n\}", c, flags=re.S)
    if not m:
        bad.append("FLOW: %s has no LLIMToastNotifyPanel::compactButtons" % PANEL)
    elif "lltoastflow::flow(" not in m.group(1) or "lltoastflow::rowCount(" not in m.group(1):
        bad.append("FLOW: LLIMToastNotifyPanel::compactButtons must place the buttons through lltoastflow::flow "
                   "and size the panel by lltoastflow::rowCount")
    return bad


def run(ctx, result):
    if not os.path.exists(ctx.path(HEADER)):
        result.fail("%s is missing" % HEADER)
        result.summary = "header missing"
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: toast button flow tests not run")
        result.summary = "not run"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_toast_button_flow")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + ctx.path("indra/newview"),
                            ctx.path(TEST), "-o", exe], capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    bad = source_rules(ctx)
    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "tests pass, the flow rule holds"
