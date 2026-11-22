"""Regression guard for VR arms and neck (#1584; indra/newview/llvrbodyarmmath.h).

Compiles scripts/check/test_vr_arms.cpp with the host C++ compiler and runs it: the generalised
two-bone solver against solveLeg, the wrist on its target within reach, no stretch out of reach,
the natural elbow pole (down / back / out) and tracker hints, left / right mirroring, the forearm
twist split, the collar's share of overhead reaches, the controller grip -> wrist mapping, the
neck / head split summing to the headset's rotation, and the fly cam mode machine (D: head
released / C: frozen, the 0.3 s eases at 90 and 144 Hz).
"""
import os
import shutil
import subprocess
import tempfile

NAME = "vr-arms"
STAGE = "source"
DESCRIPTION = "VR arm IK / neck split fixtures (llvrbodyarmmath.h)"
GUARDS = ("Upper arms rolled 180 degrees (the leg solver's bend side), arms stretching past their length, "
          "elbows flipping between the arms, candy-wrapper wrists, the head turned twice or not at all.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: VR arm fixtures not run")
        result.summary = "skipped (no compiler)"
        return
    src = ctx.path("scripts/check/test_vr_arms.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vr_arms")
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
    result.summary = "two-bone, reach, stretch, pole, mirror, twist, collar, grip, neck (roll split), fly cam modes"
