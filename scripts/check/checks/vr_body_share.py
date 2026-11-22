"""Regression guard for the VR body share's send decisions (#1584; indra/newview/llvrbodysharemath.h).

Compiles scripts/check/test_vr_body_share.cpp with the host C++ compiler and runs it: the per-bone
deadband and its hysteresis, the settle line when a bone stops, the line / byte rate under constant
motion, motion then stillness (keyframes only), sensor jitter around the thresholds, and releases
when a bone stops being driven.
"""
import os
import shutil
import subprocess
import tempfile

NAME = "vr-body-share"
STAGE = "source"
DESCRIPTION = "VR body share send-decision fixtures (llvrbodysharemath.h)"
GUARDS = ("A VR user flooding the helper channel (every frame, every bone), jitter or breathing sending lines while "
          "standing still, limbs resting off their final pose, released bones never sent.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: VR body share fixtures not run")
        result.summary = "skipped (no compiler)"
        return
    src = ctx.path("scripts/check/test_vr_body_share.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vr_body_share")
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
    result.summary = "deadband, settle, rate, stillness, jitter, releases"
