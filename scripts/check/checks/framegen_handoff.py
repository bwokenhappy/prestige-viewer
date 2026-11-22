"""Regression guard for the handoff between the renderer and frame generation's present thread
(indra/newview/vkgfx/vk_present_handoff.*, #1517).

Compiles vk_present_handoff.cpp with scripts/check/test_present_handoff.cpp on the host and runs it
with fake jobs: the renderer blocks on a slot until the present thread is done with it, jobs run in
the order they were pushed and each exactly once, stop() while the renderer waits does not hang (and
runs the jobs still queued as dropped), and drain() says "idle" only when nothing is queued and the current
job has finished. (Task 7c fix 4) Every wait for the thread has a bound, and a bound that passes only says so:
a slot wait comes back Busy with the slot untouched, a drain false with the job running on, and however many
bounds pass the handoff reports nothing and gives nothing up -- the thread still runs and takes work. stop()
waits a bound for the thread to end; a thread still inside its job then is left behind for good (never joined,
no new thread, a drain says "not idle"), and runs the jobs queued behind it as dropped if its call ever
returns. A process paused meanwhile (SIGSTOP of a child process, as a debugger break or a system sleep) only
gets timeouts. Where the compiler has them, the test runs again under
UndefinedBehaviorSanitizer and ThreadSanitizer. The handoff stays free of Vulkan and the viewer (it
includes only its own header), so it builds here.
"""
import os
import platform
import re
import shutil
import subprocess
import tempfile

NAME = "framegen-handoff"
STAGE = "source"
DESCRIPTION = "frame generation present thread handoff: slots, order, drain and stop (tests)"
GUARDS = ("A renderer that overwrites a frame the present thread still shows, frames shown out of order "
          "or twice, a job never run (its slot never released), drain / stop saying idle early, or any wait "
          "for the present thread without a bound or with a verdict (a viewer frozen under a held present, a "
          "quit that never ends, a held present taken for a hang).")

SOURCES = ("indra/newview/vkgfx/vk_present_handoff.h", "indra/newview/vkgfx/vk_present_handoff.cpp")
TEST = "scripts/check/test_present_handoff.cpp"
ALLOWED_QUOTED = ("vk_present_handoff.h",)
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
                result.fail("%s includes \"%s\": the handoff includes only its own header" % (rel, name))
                ok = False
            elif kind == "<" and "vulkan" in name.lower():
                result.fail("%s includes <%s>: the handoff knows nothing of Vulkan" % (rel, name))
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
        result.warn("no C++ compiler: present thread handoff tests not run")
        result.summary = "not run"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_present_handoff")
        c = _compile(cxx, ctx, exe, [])
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = _run([exe])
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return

        notes = []
        ubsan = os.path.join(tmp, "test_present_handoff_ubsan")
        if _compile(cxx, ctx, ubsan, ["-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-g"]).returncode:
            notes.append("no UBSan")
        else:
            r = _run([ubsan])
            if r.returncode:
                result.fail("under UndefinedBehaviorSanitizer:\n" + r.stdout + r.stderr)
                return
            notes.append("UBSan")

        # ThreadSanitizer: the handoff is all threads. As the pacer's check: under setarch -R where
        # the kernel maps memory TSan cannot use.
        tsan = os.path.join(tmp, "test_present_handoff_tsan")
        if _compile(cxx, ctx, tsan, ["-fsanitize=thread", "-g"]).returncode:
            notes.append("no TSan")
        else:
            setarch = shutil.which("setarch")
            r = _run([setarch, platform.machine(), "-R", tsan] if setarch else [tsan])
            if setarch and r.returncode and r.stderr.startswith("setarch:"):
                r = _run([tsan])
            if r.returncode and TSAN_UNSUPPORTED in r.stderr:
                result.warn("ThreadSanitizer cannot run on this kernel: the thread test ran without it")
                notes.append("TSan could not run")
            elif r.returncode:
                result.fail("under ThreadSanitizer (exit %d):\n%s%s" % (r.returncode, r.stdout, r.stderr))
                return
            else:
                notes.append("TSan")
    result.summary = "tests pass (%s)" % ", ".join(notes)
