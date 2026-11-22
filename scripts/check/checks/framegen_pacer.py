"""Regression guard for the thread that paces generated frames (indra/newview/vkgfx/vk_frame_pacer.*).

Compiles vk_frame_pacer.cpp with scripts/check/test_frame_pacer.cpp on the host and runs it with
fake device calls: the first batch and a hitch are released at once, a batch after a steady one
opens its gates at even steps after its end, a fence timeout or a far-off due time releases at
once, and drain() / stop() return only when every value is signalled, in order. Where the
compiler has them, the test runs again under UndefinedBehaviorSanitizer and ThreadSanitizer. The pacer stays free of Vulkan
and the viewer (it includes only llvkframegenpolicy.h), so it builds here.
"""
import os
import platform
import re
import shutil
import subprocess
import tempfile

NAME = "framegen-pacer"
STAGE = "source"
DESCRIPTION = "frame generation pacer thread: due times, release at once, drain and stop (tests)"
GUARDS = ("Generated frames shown in a burst or held too long, a gate value signalled twice, out of "
          "order or never (a present that waits forever), or drain/stop returning early or hanging.")

SOURCES = ("indra/newview/vkgfx/vk_frame_pacer.h", "indra/newview/vkgfx/vk_frame_pacer.cpp")
TEST = "scripts/check/test_frame_pacer.cpp"
ALLOWED_QUOTED = ("vk_frame_pacer.h", "llvkframegenpolicy.h")
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
TSAN_UNSUPPORTED = "unexpected memory mapping"


def _includes_ok(ctx, result):
    ok = True
    for rel in SOURCES:
        with open(ctx.path(rel), encoding="utf-8") as f:
            text = f.read()
        for kind, name in INCLUDE.findall(text):
            base = os.path.basename(name)
            if kind == '"' and base not in ALLOWED_QUOTED:
                result.fail("%s includes \"%s\": the pacer may include only llvkframegenpolicy.h" % (rel, name))
                ok = False
            elif kind == "<" and "vulkan" in name.lower():
                result.fail("%s includes <%s>: the pacer reaches Vulkan only through its Io" % (rel, name))
                ok = False
    return ok


def _compile(cxx, ctx, exe, extra):
    return subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread"] + extra +
                          ["-I" + ctx.path("indra/newview/vkgfx"), ctx.path(SOURCES[1]), ctx.path(TEST),
                           "-o", exe], capture_output=True, text=True)


def _run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(cmd, 1, e.stdout or "", "timed out after 60 s (drain or stop hangs?)")


def run(ctx, result):
    missing = [p for p in SOURCES + (TEST,) if not os.path.exists(ctx.path(p))]
    if missing:
        result.fail("missing: " + ", ".join(missing))
        result.summary = "files missing"
        return
    if not _includes_ok(ctx, result):
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: frame pacer tests not run")
        result.summary = "not run"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_frame_pacer")
        c = _compile(cxx, ctx, exe, [])
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = _run([exe])
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return

        # Again with UndefinedBehaviorSanitizer (an overflowing clock fails), where the compiler has it.
        notes = []
        ubsan = os.path.join(tmp, "test_frame_pacer_ubsan")
        if _compile(cxx, ctx, ubsan, ["-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-g"]).returncode:
            notes.append("no UBSan")
        else:
            r = _run([ubsan])
            if r.returncode:
                result.fail("under UndefinedBehaviorSanitizer:\n" + r.stdout + r.stderr)
                return
            notes.append("UBSan")

        # And under ThreadSanitizer (skipped quietly where the compiler does not have it). Some
        # kernels map memory where TSan cannot run (it dies, or crashes now and then); without
        # address randomisation it can, so it runs under setarch -R where that exists.
        tsan = os.path.join(tmp, "test_frame_pacer_tsan")
        if _compile(cxx, ctx, tsan, ["-fsanitize=thread", "-g"]).returncode:
            notes.append("no TSan")
        else:
            setarch = shutil.which("setarch")
            r = _run([setarch, platform.machine(), "-R", tsan] if setarch else [tsan])
            if setarch and r.returncode and r.stderr.startswith("setarch:"):
                r = _run([tsan])    # the container refuses the personality change
            if r.returncode and TSAN_UNSUPPORTED in r.stderr:
                result.warn("ThreadSanitizer cannot run on this kernel: the thread test ran without it")
                notes.append("TSan could not run")
            elif r.returncode:
                result.fail("under ThreadSanitizer (exit %d):\n%s%s" % (r.returncode, r.stdout, r.stderr))
                return
            else:
                notes.append("TSan")
    result.summary = "tests pass (%s)" % ", ".join(notes)
