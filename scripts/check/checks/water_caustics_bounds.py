"""Compile the production wetness certificate with fast-math and replay its old dry-side failure."""
import os
import shutil
import subprocess
import tempfile

NAME = "water-caustics-bounds"
STAGE = "source"
DESCRIPTION = "Published-surface caustic lower envelope and old horizontal-displacement counterexample"
GUARDS = "Dry-side caustics, stale/unpublished height bounds, nonfinite half fields and flat fallback errors."


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++")
    if not cxx:
        result.fail("C++ compiler required for the caustic wetness proof")
        return
    clang = shutil.which("clang++")
    compilers = [cxx] + ([clang] if clang else [])
    if not clang:
        result.warn("Clang O3 fast-math finite-guard regression not exercised on this host")
    for compiler in compilers:
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            executable = os.path.join(directory, "caustic-bounds")
            compiled = subprocess.run([compiler, "-std=c++17", "-O3", "-ffast-math", "-Wall", "-Wextra",
                                       "-I" + ctx.path("indra/newview"), ctx.path("scripts/check/test_water_caustics_bounds.cpp"),
                                       "-o", executable], capture_output=True, text=True)
            if compiled.returncode:
                result.fail(compiled.stdout + compiled.stderr)
                return
            before = subprocess.run([executable, "old"], capture_output=True, text=True)
            after = subprocess.run([executable], capture_output=True, text=True)
            if before.returncode != 1 or "BEFORE:" not in before.stderr or after.returncode:
                result.fail("before/after fixture mismatch: " + before.stderr + after.stdout + after.stderr)
                return
    result.summary = "old dry-world-XY predicate FAILS; production bounds/publication/half/flat fixtures PASS (O3 fast-math; %d compilers)" % len(compilers)
