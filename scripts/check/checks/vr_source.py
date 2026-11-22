"""Regression guard for Auto's VR body source order (indra/newview/llvrbodysourcepick.h).

Compiles scripts/check/test_vr_source.cpp with the host C++ compiler and runs it: SlimeVR streaming
wins over the trackers the runtime lists (its SteamVR driver lists the same ones), then OpenXR
trackers, headset body tracking, the 3-point estimate.
"""
import os
import shutil
import subprocess
import tempfile

NAME = "vr-source"
STAGE = "source"
DESCRIPTION = "VR body source order fixtures (llvrbodysourcepick.h)"
GUARDS = ("A SlimeVR user stuck on the SteamVR copies of their trackers, never reaching the VMC / OSC stream.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: VR source fixtures not run")
        result.summary = "Auto order"
        return
    src = ctx.path("scripts/check/test_vr_source.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vr_source")
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
    result.summary = "Auto order"
