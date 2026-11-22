"""Regression guard for the RT memory budget and the RT memory fallback
(indra/newview/vkgfx/llcleanrtbudget.h).

Compiles scripts/check/test_rt_budget.cpp with the host C++ compiler and runs it: the automatic
RT budget (headroom rule + the ladder's pressure cap) can never trip the over-budget test that
puts ray-traced shadows on shadow maps; the user's numbers (12 GB cap, 4.5 GB in use) reproduce
the old 384 MB floor tripping it and the old hold keeping RT off for minutes after the ladder
calmed; under an explicit RenderVKRTBlasBudgetMB the fallback does not flap and lifts within
seconds once the budget holds the working set.
Also a source guard: LLCleanRTVK takes the budget arithmetic from the header (pressureFloor,
updateHeadroomBudget, autoBudgetBytes, the over-budget test) and the renderer's fallback is
the header's Fallback state machine.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "rt-budget"
STAGE = "source"
DESCRIPTION = "the automatic RT budget never forces RT shadows onto shadow maps (llcleanrtbudget.h fixtures)"
GUARDS = ("RT shadows on shadow maps for 10-20 min after VRAM pressure ended (2026-11-09: the headroom "
          "rule's 384 MB floor under the working set tripped the RT memory fallback).")

RT = "indra/newview/vkgfx/llcleanrtvk.cpp"
REN = "indra/newview/vkgfx/llcleanrenderervk.cpp"


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _function(text, signature):
    at = text.find(signature)
    if at < 0:
        return None
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


def _source_guard(ctx, result):
    rt = _strip_comments(open(ctx.path(RT), encoding="utf-8").read())
    ren = _strip_comments(open(ctx.path(REN), encoding="utf-8").read())
    wants = [
        (rt, "uint64_t LLCleanRTVK::pressureFloor() const", "LLCleanRTBudget::workingFloor("),
        (rt, "void LLCleanRTVK::updateHeadroomBudget()", "LLCleanRTBudget::headroomTarget("),
        (rt, "void LLCleanRTVK::updateHeadroomBudget()", "LLCleanRTBudget::headroomStep("),
        (rt, "uint64_t LLCleanRTVK::autoBudgetBytes() const", "LLCleanRTBudget::autoBudget("),
        (rt, "uint64_t LLCleanRTVK::budgetBytes() const", "LLCleanRTBudget::withPressureCap("),
        (rt, "void LLCleanRTVK::idleFrame()", "updateHeadroomBudget()"),
        (ren, "void LLCleanRendererVK::selectLitVariant()", "mRtFb.step("),
    ]
    for text, sig, call in wants:
        fn = _function(text, sig)
        if fn is None:
            result.fail("%s not found; update this check" % sig)
        elif call not in fn:
            result.fail("%s no longer uses %s (llcleanrtbudget.h is the one place the RT budget is computed)" % (sig, call))
    if not re.search(r"LLCleanRTBudget::overEnter\(legacy_aux,\s*legacy_budget\)", rt):
        result.fail("recordBuild's over-budget test no longer uses LLCleanRTBudget::overEnter")
    if re.search(r"\bkRtBudgetFloor\b", rt):
        result.fail("a separate headroom floor (kRtBudgetFloor) is back: it must be LLCleanRTBudget's, never under the working floor")


def run(ctx, result):
    _source_guard(ctx, result)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: RT budget fixtures not run")
        result.summary = "source guard only (no compiler)"
        return
    src = ctx.path("scripts/check/test_rt_budget.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_rt_budget")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I" + inc, src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "auto budget never over; user case reproduced and fixed; explicit budget lifts once it fits"
