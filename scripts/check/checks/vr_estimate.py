"""Regression guard for the VR 3-point body estimate (#1586; indra/newview/llvrbodyestimatemath.h).

Compiles scripts/check/test_vr_estimate.cpp with the host C++ compiler and runs it: the body
heading's torso-twist budget, the crouch (hips drop, feet planted, no steps), bending forward
without steps, the step trigger (drift, turn, one foot at a time, step time, lift, settling), and
the same motion at 90 and 144 Hz.
"""
import os
import shutil
import subprocess
import tempfile

NAME = "vr-estimate"
STAGE = "source"
DESCRIPTION = "VR 3-point body estimate fixtures (llvrbodyestimatemath.h)"
GUARDS = ("The body spinning with every glance, feet sliding or stepping while you stand, crouch or lean, "
          "both feet stepping at once, motion that changes with the frame rate.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: VR estimate fixtures not run")
        result.summary = "skipped (no compiler)"
        return
    src = ctx.path("scripts/check/test_vr_estimate.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vr_estimate")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-I" + inc, src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        if c.stderr.strip():
            result.fail("compiler warnings:\n" + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "heading, crouch, lean, steps, 90 / 144 Hz"
