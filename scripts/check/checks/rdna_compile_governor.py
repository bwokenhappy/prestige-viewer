"""RDNA1/2 compile admission and pressure-discard regression (#1678/#1651)."""
import os
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
NAME = "rdna-compile-governor"
STAGE = "source"
DESCRIPTION = "RDNA1/2 heavy compile 8 GiB floor, allocation plateau, newest-result requeue (CPU curves)"
GUARDS = "AMD maintenance drivers over-admit 6-12 GiB shader compiles and exhaust system commit at login"

def run(ctx, result):
    def read(path):
        with open(ctx.path(path), encoding="utf-8") as f:
            return f.read()
    pipe = read("indra/newview/vkgfx/vk_pipeline.cpp")
    worker = read("indra/newview/vkgfx/vk_pipeworker.cpp")
    context = read("indra/newview/vkgfx/llvulkancontext.cpp")
    required = ("vkcompilegovernor::estimate", "vkcompilegovernor::fits", "vkcompilegovernor::newestToAbandon",
                "r->curve.sample", "r->curve.plateau", "pick->retryFront = true", "if (r.abandon",
                "vkDestroyPipeline(device, made", "++s.pendingPrio[prio]")
    required += ("CreateMutexW", "WAIT_ABANDONED", "ReleaseMutex", "CloseHandle", "processGate.acquire(pick->governed")
    for token in required:
        if token not in pipe:
            result.fail("scheduler lost governor contract: " + token)
    for src, name in ((context, "viewer"), (worker, "worker")):
        if "vkcompilegovernor::rule" not in src or "vkSetPipelineRdnaGovernor" not in src:
            result.fail(name + " does not configure the same driver policy")
    if "pmem.PrivateUsage" not in worker:
        result.fail("Windows worker must observe private commit, not a paged-out working set")
    if "head->light ? !(critical && lightIn >= 1)" not in pipe:
        result.fail("retry-front light work incorrectly inherits heavy admission caps")
    if "--rdna-governor" not in worker:
        result.fail("forced policy is not propagated to child compilers")
    if "cap = sm.total / 4" not in context or "std::to_string(memMB / 2)" not in worker:
        result.fail("governor must not raise the existing process/in-flight memory caps")
    tree=ET.fromstring(read("indra/newview/app_settings/settings.xml"))
    root=tree.find("map"); elems=list(root)
    settings={elems[i].text:elems[i+1] for i in range(0,len(elems),2)}
    fields=list(settings["VKDebugForceRDNACompileGovernor"])
    values={fields[i].text:fields[i+1].text for i in range(0,len(fields),2)}
    if values.get("Persist")!="0" or values.get("Value")!="0":
        result.fail("force switch must be nonpersistent and off")
    cxx=shutil.which("c++")
    if not cxx:
        result.fail("C++ compiler required for real policy fixture"); return
    with tempfile.TemporaryDirectory(prefix="rdna-compile-",dir=os.environ.get("TMPDIR")) as tmp:
        out=os.path.join(tmp,"checks")
        p=subprocess.run([cxx,"-std=c++17","-Wall","-Wextra","-Werror",ctx.path("indra/newview/tests/rdna_compile_governor_checks.cpp"),"-o",out],capture_output=True,text=True)
        if p.returncode:
            result.fail(p.stdout+p.stderr); return
        p=subprocess.run([out],capture_output=True,text=True)
        if p.returncode:
            result.fail(p.stdout+p.stderr); return


    result.summary = "16/32 GiB curves, plateau, light lane, pressure/requeue and last-progress PASS"
