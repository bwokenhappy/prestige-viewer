"""Regression guard for the DLSS add-on's frame generation block (contrib/ngxshim).

Compiles scripts/check/test_framegen_addon.cpp with the host C++ compiler and runs it, also under
ThreadSanitizer where the compiler has it: the per-thread failure text slip_nr_last_error returns (the main thread's
DLSS evaluate and the present thread's frame generation evaluate may fail at once). Checks that the
add-on carries no leftovers of the throwaway experiment and that header and source both name the
slip_fg entry points; that the source keeps its failure text only through last_error.h (no shared
g_lastError string) and reads no NVIDIA application id (1.4.0: the driver paces without one, in fullscreen;
NGX always gets the project id, and no nvidia_app_id.txt is read); and
(Task 11b, Linux) that CMakeLists.txt globs and lists DLSS Frame Generation's library on both platforms
(libnvidia-ngx-dlssg.so.* / nvngx_dlssg.dll), build.sh and build.ps1 install it with the others, and
README.md's install table names it; (fix 1) and that CMakeLists.txt's project version is the library's.

(The final fix pass) fg_caps.h's rules are tested by the same host test: the reason frame generation cannot
run (NVIDIA's library missing, Windows' GPU scheduling off, the runtime gave no answer), and that a caller's
older, shorter struct gets only the fields that fit. Source rules: slip_nr_fg_caps discards no
NVSDK_NGX_Parameter_Get result, answers through slip_fg_decide / slip_fg_write_caps, and accepts any
struct_size from SLIP_NR_FG_CAPABILITIES_SIZE_V1 up (slip_nr_fg_evaluate: SLIP_NR_FG_EVAL_SIZE_V1), refusing a
smaller one with a text; the header's SLIP_FG_WHY_* are pinned to fg_caps.h's values; the README says what the
viewer tells the player.
"""
import os
import platform
import re
import shutil
import subprocess
import tempfile

NAME = "framegen-addon"
STAGE = "source"
DESCRIPTION = "DLSS add-on slip_fg block: capabilities and failure text tests, no application id, entry points"
GUARDS = ("An NVIDIA application id read or passed to NGX again, experiment code left in the add-on, or a "
          "slip_fg entry point missing from the header or the source.")

ROOT = "contrib/ngxshim"
HEADER = ROOT + "/include/slipstream_ngx.h"
SOURCE = ROOT + "/src/slipstream_ngx.cpp"
TEST = "scripts/check/test_framegen_addon.cpp"
CMAKE = ROOT + "/CMakeLists.txt"
BUILD_SH = ROOT + "/build.sh"
BUILD_PS1 = ROOT + "/build.ps1"
README = ROOT + "/README.md"
TSAN_UNSUPPORTED = "unexpected memory mapping"
FORBIDDEN = re.compile(r"spike|fg_spike|slip_dfg|slip_nr_dfg", re.I)
NAMES = ("SLIP_FG_FN_DEVICE_EXTENSIONS", "SLIP_FG_FN_CAPS", "SLIP_FG_FN_CREATE", "SLIP_FG_FN_EVALUATE", "SLIP_FG_FN_RELEASE")


def run(ctx, result):
    for rel in (HEADER, SOURCE):
        text = ""
        if os.path.exists(ctx.path(rel)):
            with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
                text = f.read()
        else:
            result.fail("%s is missing" % rel)
        for n in NAMES:
            if n not in text:
                result.fail("%s lacks %s" % (rel, n))
    # slip_nr_device_extensions is all-or-nothing in the viewer: frame generation's extensions must
    # come from slip_nr_fg_device_extensions, so a GPU lacking one never costs DLSS.
    if os.path.exists(ctx.path(SOURCE)):
        with open(ctx.path(SOURCE), encoding="utf-8", errors="replace") as f:
            text = f.read()
        m = re.search(r"slip_nr_device_extensions\s*\(.*?(?=\nSLIP_NR_API)", text, re.S)
        if not m:
            result.fail("slip_nr_device_extensions not found in %s" % SOURCE)
        elif re.search(r"FrameGeneration|present_metering", m.group(0), re.I):
            result.fail("slip_nr_device_extensions mentions frame generation or present metering "
                        "(they belong in slip_nr_fg_device_extensions)")
    _linux_files(ctx, result)
    _caps_rules(ctx, result)
    for dirpath, _, files in os.walk(ctx.path(ROOT)):
        for fn in files:
            p = os.path.join(dirpath, fn)
            try:
                with open(p, encoding="utf-8") as f:
                    for i, line in enumerate(f, 1):
                        if FORBIDDEN.search(line):
                            result.fail("%s:%d still has experiment code: %s" % (os.path.relpath(p, ctx.root), i, line.strip()))
            except (UnicodeDecodeError, OSError):
                pass
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: the app id tests were not run")
        result.summary = "not run"
        return
    notes = []
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_framegen_appid")
        c = _compile(cxx, ctx, exe, [])
        if c.returncode:
            result.fail("add-on test did not compile:\n" + c.stdout + c.stderr)
            return
        r = _run([exe])
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
        # ThreadSanitizer: two threads' failures at once (a shared failure string is a data race).
        tsan = exe + "_tsan"
        if _compile(cxx, ctx, tsan, ["-fsanitize=thread", "-g"]).returncode:
            notes.append("no TSan")
        else:
            setarch = shutil.which("setarch")
            r = _run([setarch, platform.machine(), "-R", tsan] if setarch else [tsan])
            if setarch and r.returncode and r.stderr.startswith("setarch:"):
                r = _run([tsan])
            if r.returncode and TSAN_UNSUPPORTED in r.stderr:
                result.warn("ThreadSanitizer cannot run on this kernel: the add-on test ran without it")
                notes.append("TSan could not run")
            elif r.returncode:
                result.fail("add-on test under ThreadSanitizer (exit %d):\n%s%s" % (r.returncode, r.stdout, r.stderr))
                return
            else:
                notes.append("TSan")
    result.summary = "tests pass (%s)" % ", ".join(notes)


def _compile(cxx, ctx, exe, extra):
    return subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread"] + extra +
                          ["-I" + ctx.root, ctx.path(TEST), "-o", exe], capture_output=True, text=True)


def _run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(cmd, 1, e.stdout or "", "timed out after 120 s")


def _read(ctx, rel):
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def _fn_body(code, name):
    """The body of the exported function `name` ("" when there is none)."""
    m = re.search(r"SLIP_NR_CALL\s+%s\s*\([^)]*\)\s*\{" % re.escape(name), code)
    if not m:
        return ""
    depth, i = 1, m.end()
    while i < len(code) and depth:
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    return code[m.end():i]


def _caps_rules(ctx, result):
    """slip_nr_fg_caps: every read checked, the reason decided by fg_caps.h, struct sizes from V1 up."""
    src = _read(ctx, SOURCE) or ""
    code = re.sub(r"//[^\n]*|/\*.*?\*/", "", src, flags=re.S)
    caps = _fn_body(code, "slip_nr_fg_caps")
    ev = _fn_body(code, "slip_nr_fg_evaluate")
    if not caps or not ev:
        result.fail("%s: slip_nr_fg_caps / slip_nr_fg_evaluate not found" % SOURCE)
        return
    gets = re.findall(r"NVSDK_NGX_Parameter_Get\w*\s*\(", caps)
    used = re.findall(r"(?:NVSDK_NGX_SUCCEED|!\s*NVSDK_NGX_FAILED)\s*\(\s*NVSDK_NGX_Parameter_Get\w*\s*\(", caps)
    if len(gets) < 5 or len(gets) != len(used):
        result.fail("%s: slip_nr_fg_caps must check the result of every NVSDK_NGX_Parameter_Get (%d of %d checked)"
                    % (SOURCE, len(used), len(gets)))
    if "slip_fg_decide(" not in caps or "slip_fg_write_caps(" not in caps:
        result.fail("%s: slip_nr_fg_caps must answer through slip_fg_decide and slip_fg_write_caps (fg_caps.h)" % SOURCE)
    for body, fn, v1 in ((caps, "slip_nr_fg_caps", "SLIP_NR_FG_CAPABILITIES_SIZE_V1"), (ev, "slip_nr_fg_evaluate", "SLIP_NR_FG_EVAL_SIZE_V1")):
        m = re.search(r"if\s*\([^{;]*!\s*slip_fg_size_ok\s*\(\s*\w+->struct_size\s*,\s*%s\s*\)[^{;]*\)\s*\{([^}]*)\}" % v1, body)
        if not m or "setError(" not in m.group(1) or "SLIP_NR_ERROR_INVALID_ARGUMENT" not in m.group(1):
            result.fail("%s: %s must accept any struct_size from %s up (slip_fg_size_ok) and refuse a smaller one "
                        "with a text and SLIP_NR_ERROR_INVALID_ARGUMENT" % (SOURCE, fn, v1))
        if re.search(r"struct_size\s*<\s*sizeof", body):
            result.fail("%s: %s compares struct_size with sizeof (a newer library would refuse an older caller)" % (SOURCE, fn))
    for a, b in (("SLIP_FG_WHY_NOT_STATED", "SLIP_FG_REASON_NOT_STATED"), ("SLIP_FG_WHY_LIBRARY_MISSING", "SLIP_FG_REASON_LIBRARY_MISSING"),
                 ("SLIP_FG_WHY_GPU_SCHEDULING_OFF", "SLIP_FG_REASON_GPU_SCHEDULING_OFF"),
                 ("SLIP_FG_WHY_QUERY_FAILED", "SLIP_FG_REASON_QUERY_FAILED")):
        if not re.search(r"static_assert\s*\(\s*%s\s*==\s*%s\b" % (a, b), code):
            result.fail("%s must static_assert %s == %s" % (SOURCE, a, b))
    rd = _read(ctx, README) or ""
    flat = " ".join(rd.split())
    for words in ("reinstall the DLSS add-on with the frame generation library", "hardware-accelerated GPU scheduling",
                  "Settings > System > Display > Graphics"):
        if words not in flat:
            result.fail("%s must say what the viewer tells the player: \"%s\"" % (README, words))


def _linux_files(ctx, result):
    """The failure text per thread, no application id, DLSS-G's library installed on Linux too."""
    src = _read(ctx, SOURCE) or ""
    code = re.sub(r"//[^\n]*|/\*.*?\*/", "", src, flags=re.S)
    if re.search(r"\bstd::string\s+g_lastError\b", code) or "g_lastError" in code:
        result.fail("%s keeps a shared g_lastError (the main thread and the present thread may fail at once): "
                    "use last_error.h" % SOURCE)
    if '#include "last_error.h"' not in code or "slip_set_last_error(" not in code \
            or not re.search(r"slip_nr_last_error\s*\(\s*void\s*\)\s*\{\s*return\s+slip_last_error_text\s*\(\s*\)", code):
        result.fail("%s must set failures with slip_set_last_error and return slip_last_error_text() from "
                    "slip_nr_last_error (last_error.h)" % SOURCE)
    # (1.4.0) no NVIDIA application id: the driver paces without one (in fullscreen), so none is read or passed on
    for word, why in (("nvidia_app_id", "reads an application id file"),
                      ("Application_Identifier_Type_Application_Id", "identifies itself to NGX by an application id"),
                      ("app_id.h", "includes the application id parser")):
        if word in code:
            result.fail("%s %s: the add-on uses the project id only (1.4.0)" % (SOURCE, why))
    if re.search(r"\bNVSDK_NGX_VULKAN_Init\s*\(", code):
        result.fail("%s calls NVSDK_NGX_VULKAN_Init (an application id): use NVSDK_NGX_VULKAN_Init_with_ProjectID" % SOURCE)
    if os.path.exists(ctx.path(ROOT + "/src/app_id.h")):
        result.fail("%s/src/app_id.h is back: the add-on reads no application id" % ROOT)
    cm = _read(ctx, CMAKE) or ""
    if "libnvidia-ngx-dlssg.so.*" not in cm or "nvngx_dlssg.dll" not in cm:
        result.fail("%s must glob DLSS Frame Generation's library (libnvidia-ngx-dlssg.so.* and nvngx_dlssg.dll) "
                    "with the others" % CMAKE)
    sh = _read(ctx, BUILD_SH) or ""
    if not re.search(r"FG_LIBS=\(\"\$LIBDIR\"/rel/libnvidia-ngx-dlssg\.so\.\*\)", sh) \
            or not re.search(r'for f in [^\n]*"\$\{FG_LIBS\[@\]\}"; do\s*\n\s*install_file\b', sh):
        result.fail("%s must install libnvidia-ngx-dlssg.so.* (FG_LIBS) with the other runtime libraries" % BUILD_SH)
    ps = _read(ctx, BUILD_PS1) or ""
    if "nvngx_dlssg.dll" not in ps:
        result.fail("%s must install nvngx_dlssg.dll with the other runtime libraries" % BUILD_PS1)
    # (Task 11b fix 1, M9) the CMake project's version is the library's own
    lib = re.search(r'#define\s+SLIP_NR_LIBRARY_VERSION\s+"([0-9.]+)"', code)
    proj = re.search(r"project\(\s*slipstream_ngx\s+VERSION\s+([0-9.]+)", cm)
    if not lib or not proj or lib.group(1) != proj.group(1):
        result.fail("%s: project(slipstream_ngx VERSION ...) must be the library's version (SLIP_NR_LIBRARY_VERSION %s)"
                    % (CMAKE, lib.group(1) if lib else "?"))
    rd = _read(ctx, README) or ""
    table = [l for l in rd.splitlines() if l.startswith("| Linux")]
    if not table or "libnvidia-ngx-dlssg.so" not in table[0]:
        result.fail("%s: the install table's Linux row must name libnvidia-ngx-dlssg.so.<version>" % README)
