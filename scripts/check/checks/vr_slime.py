"""Regression guard for SlimeVR body trackers over VRChat OSC (#1582; indra/newview/llvrbodyslimeosc.h).

Compiles scripts/check/test_vr_slime.cpp with the host C++ compiler and runs it: the OSC parser
(valid bundles / messages, every truncation, bad padding, oversized / unaligned elements, deep
nesting, non-finite numbers, random corruption), SlimeVR's Unity-axes encoding back to OpenXR
axes, and the SlimeVR -> play space alignment (feet under the head, left stays left, walking
carries the feet, a head sway leaves planted feet, the hip fallback, LOCAL space, reset re-yaw),
and the VMC skeleton (bone messages: valid, truncated, malformed, odd names; Unity bones onto the
SL joints, the spine spread over the Bento spine, limbs, fingers, locals back to worlds).
"""
import os
import shutil
import subprocess
import tempfile

NAME = "vr-slime"
STAGE = "source"
DESCRIPTION = "VR SlimeVR OSC parser / conversion / alignment fixtures (llvrbodyslimeosc.h)"
GUARDS = ("A malformed or hostile UDP packet crashing or poisoning the body pose, SlimeVR trackers "
          "mirrored or turned (Unity axes), feet not under the user or sliding with head sway.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: SlimeVR OSC fixtures not run")
        result.summary = "skipped (no compiler)"
        return
    src = ctx.path("scripts/check/test_vr_slime.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vr_slime")
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
    result.summary = "OSC / VMC parser, Unity -> OpenXR axes, alignment, VMC -> SL skeleton"
