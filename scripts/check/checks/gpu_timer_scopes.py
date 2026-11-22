"""Actual bounded timestamp allocator/queue complement and line-format regressions."""
import os
import runpy
import shutil
import subprocess
import tempfile

NAME = "gpu-timer-scopes"
STAGE = "source"
DESCRIPTION = "Explicit pairs, bounded multiview queries, queue-local gaps and parser isolation"
GUARDS = "No previous-mark pass attribution, no async clock mixing, no inherited empty-pass cost."


def run(ctx, result):
    compiler = os.environ.get("CXX") or shutil.which("c++")
    if not compiler:
        result.fail("C++ compiler required for production GPU scope math fixture")
        return
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
        exe = os.path.join(directory, "scopes")
        proc = subprocess.run([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                               "-I" + ctx.path("indra/newview"), ctx.path("scripts/check/test_gpu_timer_scopes.cpp"),
                               "-o", exe], capture_output=True, text=True)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
        proc = subprocess.run([exe], capture_output=True, text=True, timeout=15)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
    recorder = runpy.run_path(ctx.path("scripts/check/gpu_scope_recording_fixture.py"))["fixture"]
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
        source = os.path.join(directory, "recorder.cpp")
        with open(source, "w") as f:
            f.write(recorder(ctx.root))
        executable = os.path.join(directory, "recorder")
        proc = subprocess.run([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                               "-I" + ctx.path("indra/newview"), source, "-o", executable], capture_output=True, text=True)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
        proc = subprocess.run([executable], capture_output=True, text=True, timeout=15)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
    parser = runpy.run_path(ctx.path("scripts/perf/gpu_scopes.py"))["parse_sub_line"]
    parsed = parser("GPU sub ms/frame: c_shadow_rigcull=0.02 | async queue: c_gi_trace=2 | gaps: wait_total=3 before_c_shadow_rigcull=2 | scope_meta: queries_per_frame=80 sampled_frames=20 truncated_frames=0 | worst: c_shadow_rigcull=0.04")
    if parsed != {"graphics": {"c_shadow_rigcull": .02}, "async": {"c_gi_trace": 2},
                  "gaps": {"wait_total": 3, "before_c_shadow_rigcull": 2}, "worst": {"c_shadow_rigcull": .04}}:
        result.fail("parser mixes async/gaps/worst with pass timings")
    with open(ctx.path("indra/newview/vkgfx/llcleanrenderervk.cpp")) as f:
        renderer = f.read()
    for needle in ("VKGpuScopeMath::reserve", "mScopeOverflowR[h]", "scope.cancel()"):
        # Cancellation policy lives in the header's template, not the CPP.
        if needle == "scope.cancel()":
            with open(ctx.path("indra/newview/vkgfx/llcleanrenderervk.h")) as f:
                present = needle in f.read()
        else:
            present = needle in renderer
        if not present:
            result.fail("missing production scope safety contract: " + needle)
    shadow = renderer.split("void LLCleanRendererVK::recordShadowRigCull(", 1)[1].split("\nvoid LLCleanRendererVK::", 1)[0]
    if shadow.index("if (nv == 0)") > shadow.index("GpuPassScope"):
        result.fail("zero-view shadow cull reserves a pass query before returning")
    if "addSub(names[i], d, false)" in renderer or "addSub(names[i], d, true)" in renderer:
        result.fail("legacy previous-boundary pass attribution survived")
    result.summary = "actual pair capacity/multiview/clock-wrap/union gaps and isolated line sections pass"
