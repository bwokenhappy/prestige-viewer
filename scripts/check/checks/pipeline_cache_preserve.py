"""Exercise production cache persistence with fake driver data and real temporary files."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

NAME = "pipeline-cache-preserve"
STAGE = "source"
DESCRIPTION = ("pipeline cache: failed / oversized saves keep the last file; atomic replace; cap >= the derived"
               " single-build worst case; self-pruning by build generations and the over-cap marker")
GUARDS = ("Over-cap saves deleted a useful seed and marked unsaved bytes as saved (2026-11-19); the 256 MB cap"
          " saturated with older builds' entries and every launch recompiled the overflow (2026-11-19).")


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++")
    if not cxx:
        result.fail("C++ compiler required for cache persistence regression test")
        return
    # Compile the real load/save implementation; only viewer/Vulkan dependencies are stubbed.
    source = Path(ctx.path("indra/newview/vkgfx/vk_pipecache.cpp")).read_text()
    source = source.split("// ---- #471 learned compile costs ----")[0]
    source = re.sub(r'^#include "[^\n]+"[^\n]*\n', '', source, flags=re.M)
    # The real header too (its cap derivation static_asserts, LoadInfo, generations); only its
    # includes are dropped -- the test supplies the Vulkan / viewer stubs.
    header = Path(ctx.path("indra/newview/vkgfx/vk_pipecache.h")).read_text()
    header = re.sub(r'^#(include|pragma)[^\n]*\n', '', header, flags=re.M)
    with tempfile.TemporaryDirectory() as tmp:
        Path(tmp, "cache_under_test.inc").write_text(header + "\n" + source)
        exe = str(Path(tmp, "cache_test"))
        proc = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                               "-I" + tmp, ctx.path("scripts/check/test_pipeline_cache_preserve.cpp"),
                               "-o", exe], capture_output=True, text=True)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
        proc = subprocess.run([exe], cwd=tmp, capture_output=True, text=True)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
    result.summary = ("real load/save: repeated oversize, read/write/replace failures, atomic replacement, cross-build"
                      " seed, generation prune, over-cap marker, crashed-writer temp sweep, cap derivation")
