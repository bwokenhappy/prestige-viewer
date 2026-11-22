"""Regression guard for camera mode's quick-shot bar (indra/newview/slcamerabarlayout.h, #1572).

Compiles scripts/check/test_camera_bar_layout.cpp with the host C++ compiler and runs it: the shutter on the
window's centre line at any width and UI scale, mirrored side widths (104 / 168 dp), the bar's width, labels
shrinking one type step before truncating, and the 8 s hint row.

Source rule:
  LAYOUT  slcameraview.cpp places the bar through slcambar::layoutBar (one geometry for the tests and the view).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "camera-bar-layout"
STAGE = "source"
DESCRIPTION = "camera mode's bar: shutter exactly centred, mirrored sides, label fit, hint time (tests)"
GUARDS = "The shutter drifting off centre, or one side of the camera bar wider than the other."

HEADER = "indra/newview/slcamerabarlayout.h"
TEST = "scripts/check/test_camera_bar_layout.cpp"
VIEW = "indra/newview/slcameraview.cpp"


def _code(ctx, rel):
    """The file without comments ("" when it is missing)."""
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return re.sub(r"//[^\n]*|/\*.*?\*/", "", f.read(), flags=re.S)
    except OSError:
        return ""


def source_rules(ctx):
    c = _code(ctx, VIEW)
    if '#include "slcamerabarlayout.h"' not in c or "slcambar::layoutBar(" not in c:
        return ["LAYOUT: %s must include slcamerabarlayout.h and place the bar through slcambar::layoutBar" % VIEW]
    return []


def run(ctx, result):
    if not os.path.exists(ctx.path(HEADER)):
        result.fail("%s is missing" % HEADER)
        result.summary = "header missing"
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: camera bar tests not run")
        result.summary = "not run"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_camera_bar_layout")
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
        result.summary = "tests pass, the view uses the tested layout"
