"""Regression guard for the VR body mapping (#1592; indra/newview/llvrbodymapmath.h).

Compiles scripts/check/test_vr_bodymap.cpp with the host C++ compiler and runs it: the human
profile is identity (today's solver unchanged), retargeting is exact (FK of the avatar's joints
with the converted locals equals the virtual skeleton's), the Open Pony (dev kit rest offsets)
stands up with the barrel vertical, head forward, forelegs hanging and hind legs under it, its
forelegs reach as T-pose arms, the profile detection (Open Pony rigging, Bento / classic human,
quadrupeds), the export / import text, and the arms-only reach shift.
"""
import os
import shutil
import subprocess
import tempfile

NAME = "vr-bodymap"
STAGE = "source"
DESCRIPTION = "VR body mapping fixtures (llvrbodymapmath.h): retargeting, the Open Pony profiles, detection, file format"
GUARDS = ("Pony forelegs driven as human arms (sideways, rolled), the hoof bent by finger tracking, a "
          "human avatar changed by the mapping layer, a body map file that no longer reads back.")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: VR body mapping fixtures not run")
        result.summary = "skipped (no compiler)"
        return
    src = ctx.path("scripts/check/test_vr_bodymap.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_vr_bodymap")
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
    result.summary = "human identity, retarget FK, upright pony stance, foreleg arms, detection, text, reach, pony foreleg IK (hoof on target), settle"
