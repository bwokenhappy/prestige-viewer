"""Regression guard for VR finger tracking (#1587; indra/newview/llvrbodyfingermath.h).

Compiles scripts/check/test_vr_fingers.cpp with the host C++ compiler and runs it: a synthetic hand's
26 XR_EXT_hand_tracking joints read back as the curls and spreads they were bent by (left, mirrored
right, moved / turned), the controller grips (point, thumbs up, open, trigger only), and the curls /
spreads turning the SL Bento bones the right way on either hand.
"""
import os
import shutil
import subprocess
import tempfile

NAME = "vr-fingers"
STAGE = "source"
DESCRIPTION = "VR finger tracking fixtures (llvrbodyfingermath.h)"
GUARDS = ("Fingers bending backward or sideways, the right hand bending the wrong way, spread read as curl, "
          "a hand turned in the room curling the fingers.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: VR finger fixtures not run")
        result.summary = "hand joints, controller grips, bones"
        return
    src = ctx.path("scripts/check/test_vr_fingers.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vr_fingers")
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
    result.summary = "hand joints, controller grips, bones"
