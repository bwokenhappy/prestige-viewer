"""Regression guard for the VR camera's locally owned anchor (#1607; indra/newview/llvrcameramath.h).

Compiles scripts/check/test_vr_camera_anchor.cpp with the host C++ compiler and runs it: a snap /
smooth turn keeps the head still (any room offset, angle, world scale), the avatar catching up with
your room-scale position never moves the view, the draw offset stands the avatar's eyes under the
head for any body yaw, re-anchoring, the catch-up controller's hysteresis / directions / tail, and
the jump test.
"""
import os
import shutil
import subprocess
import tempfile

NAME = "vr-camera-anchor"
STAGE = "source"
DESCRIPTION = "VR camera anchor fixtures (llvrcameramath.h): turning about the head, offset absorption"
GUARDS = ("The view swinging on a snap / smooth turn (turning about a point that is not the head), the view "
          "moving while the avatar walks after you, the avatar drawn off the headset on a body turn, the "
          "catch-up oscillating or walking the wrong way.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: VR camera anchor fixtures not run")
        result.summary = "skipped (no compiler)"
        return
    src = ctx.path("scripts/check/test_vr_camera_anchor.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vr_camera_anchor")
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
    result.summary = "turn about the head, absorb, draw offset, re-anchor, catch-up, jumps"
