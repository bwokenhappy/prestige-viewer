"""Regression guard for the VR wrist panel's button layout and space warp's status (#1072, #1594).

Compiles scripts/check/test_vr_wrist_warp.cpp with the host C++ compiler and runs it (the button grid:
at most `columns` per row, evenly spread, nothing lost; the plain-language space warp status), then checks
the source rules:
  - vr_wrist_panel.xml: the quick actions wrap (columns 1..4), a button is at least 30 mm tall and at least
    40 mm wide at the column limit (a controller laser's hit target; a 14 mm line of text fits 2 lines).
  - llvrwrist.cpp: the action buttons come from the grid (computeGrid; no hand-placed per-button
    coordinates), every button label is drawn with drawLabel (wraps, never a squeezed single line), and the
    panel's height is sW.panelH, not the layout's fixed height.
  - space warp: the session's mono case is logged (it is why warp never engaged in the first session that
    asked for it), the status is shown in the VR settings menu, and the status bar labels the shown rate.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "vr-wrist-warp"
STAGE = "source"
DESCRIPTION = "VR wrist panel button grid + space warp status (tests and source rules)"
GUARDS = ("Wrist menu buttons too small to read (#1072: six buttons squeezed in one row), a label cut to '...', "
          "hand-placed coordinates that break when a button is added; space warp silently doing nothing "
          "(#1594: a mono session never engages it and nothing said so).")

TEST = "scripts/check/test_vr_wrist_warp.cpp"


def _read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def _xml_values(text, key):
    m = re.search(r"<key>%s</key>\s*<(?:array|integer|real)>(.*?)</(?:array|integer|real)>" % re.escape(key), text, re.S)
    return [float(v) for v in re.findall(r"<(?:real|integer)>([-0-9.]+)</(?:real|integer)>", m.group(0))] if m else []


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: wrist grid / warp status tests not run")
    else:
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "test_vr_wrist_warp")
            c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + ctx.path("indra/newview"),
                                ctx.path(TEST), "-o", exe], capture_output=True, text=True)
            if c.returncode:
                result.fail("compile failed:\n" + c.stdout + c.stderr)
                return
            r = subprocess.run([exe], capture_output=True, text=True)
            if r.returncode:
                result.fail(r.stdout + r.stderr)
                return

    xml = _read(ctx, "indra/newview/app_settings/vr_wrist_panel.xml")
    acts = xml[xml.index("<key>actions</key>"):]
    cols = _xml_values(acts, "columns")
    rect = _xml_values(acts, "rect_mm")
    gap = _xml_values(acts, "gap_mm")
    if not cols or not (1 <= cols[0] <= 4):
        result.fail("vr_wrist_panel.xml: actions.columns must be set, 1 to 4 (more per row squeezes the labels)")
    elif len(rect) == 4 and gap:
        w = (rect[2] - gap[0] * (cols[0] - 1)) / cols[0]
        if rect[3] < 30.0:
            result.fail("vr_wrist_panel.xml: action buttons are %.0f mm tall; at least 30 mm for the laser" % rect[3])
        if w < 40.0:
            result.fail("vr_wrist_panel.xml: action buttons are %.0f mm wide at %d per row; at least 40 mm" % (w, cols[0]))

    cpp = _read(ctx, "indra/newview/llvrwrist.cpp")
    flat = re.sub(r"\s+", "", cpp)
    if "computeGrid()" not in cpp or "llvrwrist::gridRows(" not in cpp:
        result.fail("llvrwrist.cpp: the action buttons come from the grid (computeGrid / llvrwrist::gridRows)")
    if re.search(r"P\.text\((label|labelText|it\.buttons|settingLabel)[^;]*t\.r\.x[^;]*HCENTER", cpp):
        result.fail("llvrwrist.cpp: a button label is drawn with drawLabel (wraps), not one squeezed P.text line")
    if "sW.L.sizeMm[1] *" in cpp or "L.sizeMm[1] * 0.001f" in cpp:
        result.fail("llvrwrist.cpp: the panel's height is sW.panelH (grown by the action rows), not the layout's")
    if "ph=sW.panelH*0.001f*scale" not in flat:
        result.fail("llvrwrist.cpp: the quad's height must come from sW.panelH")

    sw = _read(ctx, "indra/newview/vkgfx/llvrspacewarp.cpp")
    if "NOT available" not in sw or "mLastStereo = -1" not in _read(ctx, "indra/newview/vkgfx/llvrspacewarp.h"):
        result.fail("llvrspacewarp: a mono session must be logged on the first frame (mLastStereo starts unknown)")
    if "LLXrSession::instance().stereo()" not in _read(ctx, "indra/newview/llfloatervrsettings.cpp"):
        result.fail("llfloatervrsettings.cpp: the space warp buttons are greyed out while the session is mono")
    if "addStatus(graphics" not in _read(ctx, "indra/newview/llfloatervrsettings.cpp"):
        result.fail("llfloatervrsettings.cpp: the Graphics tab shows space warp's status (addStatus)")
    if "fps_spacewarp" not in _read(ctx, "indra/newview/llstatusbar.cpp"):
        result.fail("llstatusbar.cpp: while space warp is on the FPS box says rendered / shown (fps_spacewarp)")
    result.summary = "tests pass, layout rules hold"
