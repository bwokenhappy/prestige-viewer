"""Regression guard for camera mode's input decisions (indra/newview/slcamerainput.h, #1572).

Compiles scripts/check/test_camera_input.cpp with the host C++ compiler and runs it: single click focuses,
double-click on the same spot within the double-click time focuses and shoots, the window-activation click and
drags do nothing, right-click vs right-drag, the wheel's target by state and modifiers, and the camera keys
standing back while text is edited or a popup is open.

Source rule:
  ROUTE  SLCameraMode classifies clicks through slcamin::onLeftUp and routes the wheel through slcamin::wheelTarget
         (the tested decisions are the ones the viewer runs).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "camera-input"
STAGE = "source"
DESCRIPTION = "camera mode input: click focuses, double-click shoots, activation click ignored, wheel targets, typing keeps keys (tests)"
GUARDS = "A shot taken by the click that focuses the window, by two separate clicks, or by typing in a field."

HEADER = "indra/newview/slcamerainput.h"
TEST = "scripts/check/test_camera_input.cpp"
MODE = "indra/newview/slcameramode.cpp"


def _code(ctx, rel):
    """The file without comments ("" when it is missing)."""
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return re.sub(r"//[^\n]*|/\*.*?\*/", "", f.read(), flags=re.S)
    except OSError:
        return ""


def source_rules(ctx):
    c = _code(ctx, MODE)
    bad = []
    for call in ("slcamin::beginPress(", "slcamin::endPress(", "slcamin::onLeftUp(", "slcamin::wheelTarget(", "slcamin::commandTakesKeyFor(", "slcamin::takeReturn("):
        if call not in c:
            bad.append("ROUTE: %s must call %s" % (MODE, call))
    # The menu shortcuts camera mode lets through name menu_viewer.xml items: a renamed item
    # would silently be swallowed in camera mode.
    allowed = re.search(r"kAllowed\[\]\s*=\s*\{(.*?)\};", c, flags=re.S)
    try:
        with open(ctx.path("indra/newview/skins/default/xui/en/menu_viewer.xml"), encoding="utf-8") as f:
            items = set(re.findall(r'<menu_item_(?:call|check)\b[^>]*?\bname="([^"]+)"', f.read(), flags=re.S))
    except OSError:
        items = set()
    if not allowed:
        bad.append("ROUTE: %s has no kAllowed menu shortcut list" % MODE)
    else:
        for name in re.findall(r'"([^"]+)"', allowed.group(1)):
            if name not in items:
                bad.append("MENU: camera mode lets through %r, which is not a menu_viewer.xml item" % name)
    return bad


def run(ctx, result):
    if not os.path.exists(ctx.path(HEADER)):
        result.fail("%s is missing" % HEADER)
        result.summary = "header missing"
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: camera input tests not run")
        result.summary = "not run"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_camera_input")
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
        result.summary = "tests pass, the camera routes input through them"
