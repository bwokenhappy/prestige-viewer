"""Regression guard for VR full-body tracking's pure math (#1565; indra/newview/llvrbodymath.h).

Compiles scripts/check/test_vr_fbt.cpp with the host C++ compiler and runs it: quaternion
conventions, calibration offsets under rigid tracker motion, two-bone leg IK (target hit, bone
lengths kept, knee a hinge bending toward the pole, out-of-reach and degenerate targets stay
finite), the spine bend / twist limits, tracker assignment by height and side, and the 0.3 s
limb fade.
"""
import os
import shutil
import subprocess
import tempfile

NAME = "vr-fbt"
STAGE = "source"
DESCRIPTION = "VR full-body tracking IK / calibration fixtures (llvrbodymath.h)"
GUARDS = ("Knees bending sideways or backward, legs stretching past their length, NaN joints from a "
          "tracker at the hip, feet swapped left / right by the position-based assignment.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: VR body tracking fixtures not run")
        result.summary = "skipped (no compiler)"
        return
    src = ctx.path("scripts/check/test_vr_fbt.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vr_fbt")
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
    result.summary = "quaternions, offsets, leg IK, spine, assignment, fade"
