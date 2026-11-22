"""Regression guard for VR headset body tracking's joint -> role mapping (#1583;
indra/newview/llvrbodyheadsetmath.h) and the OpenXR <-> body space conversion (llvrbodymath.h).

Compiles scripts/check/test_vr_body_headset.cpp with the host C++ compiler and runs it: the axis
map (and that a turn about OpenXR +Y is a turn about SL +Z), every joint set's table (Meta upper /
full body, HTC, BD), roles built facing the body's heading at their joints, fallbacks when joints
are lost, generated joints marked, the short-dropout hold and confidence smoothing.
"""
import os
import shutil
import subprocess
import tempfile

NAME = "vr-body-headset"
STAGE = "source"
DESCRIPTION = "VR headset body tracking joint mapping fixtures (llvrbodyheadsetmath.h)"
GUARDS = ("Feet / knees taken from the wrong joints, hips facing sideways or backwards, the OpenXR -> SL axis "
          "map mirrored, a lost joint read past the array or as NaN.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: VR headset body tracking fixtures not run")
        result.summary = "skipped (no compiler)"
        return
    src = ctx.path("scripts/check/test_vr_body_headset.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vr_body_headset")
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
    result.summary = "axes, joint sets, role poses, fallbacks, hold, confidence"
