"""Regression guard for frame generation's present thread (indra/newview/vkgfx/llvkframegenpresent.*, #1517)
and the swapchain calls it makes (VkSwapchain::acquireOne / presentBatch).

Compiles llvkframegenpresent.cpp (with vk_present_handoff.cpp and vk_frame_pacer.cpp) and
scripts/check/test_framegen_present.cpp on the host against a fake Vulkan, the generator, the composite and
the swapchain faked (scripts/check/stubs stands in for llcommon's logging), and runs it: every job releases
its slot exactly once on every path (shown, evaluate failed, no image in time, out of date / suboptimal,
dropped after stop, a submit refused for lack of memory); a refused push keeps the slot's serial; the rebuild
request belongs to its frame generation chain (a normal chain, a new chain or a stop forgets it); a job never
reuses a slot whose previous job is not done on the GPU; short batches keep their gate values (n reserved,
the real frame at its own time); the generator resets after a frame it did not see; a refused submit never
presents an image in UNDEFINED nor raises a lost device, nor begins a command buffer still pending on the
fake GPU (two refusals in one job); the release's CPU fallback signals only after the present queue went idle
(else the release is owed and signalled at the next prepare -- after the present queue was waited for with the
thread stopped; prepare refuses untouched after a rebuild whose device wait failed); acquired images are waited
where they are first used. THE STALL RULE (Task 7c fix 4): a present the compositor holds, for a hundred of the
renderer's slot waits, on IMMEDIATE and FIFO, is never a lost device -- each slot wait is bounded (Busy: the
frame skipped), the GPU is checked through the device's own infinite waits (the frames in flight, the present
queue's accepted work) and found done, a rebuild is deferred, and everything goes on when the present returns,
the generator's history intact; a renderer frame or an accepted present-queue submit the GPU never finishes is
raised once as the ordinary GPU hang by the waiting call; a value merely issued (a release before its submit, a
batch whose submit was refused) is never waited; a device wait from another thread never touches the present
queue while the thread exists; shutdown or a lost device with the thread inside a call is bounded, destroys
nothing, raises nothing and leaves the thread behind (the process told). (fix 5) Once a stop has begun the
thread takes no image and presents nothing more (left behind inside a present, or joined out of the generator);
a lost surface or device answered after it was left behind is not reported; a normal chain has no present thread
(releaseChain stops the idle one, never one inside a job) and the presenter says whether it holds images; an owed
release is settled before the rebuild (the thread stopped, the present queue waited, one signal), never a release
the queue accepted nor a running job's; the status names the call a held thread is inside; the add-on's lock
(llvkaddonlock.h) keeps an evaluate and a feature create / release apart, the main thread's side with a bound.
(Task 11b fix 1) the present thread
takes no image and presents nothing while the window is hidden;
frame generation's images are admitted as one set (3 slots, else 2, else none; all or none); Reflex's
out-of-band markers bracket the present thread's work. A source rule (final review): gate values the present
thread opens itself after a withheld lead present are signalled only after the pacer was drained. Also builds and runs the swapchain's own mock test
(indra/newview/vkgfx/tests/vk_swapchain_ui_test.cpp with vk_swapchain.cpp), which covers a frame generation
chain's sizing (one image more on MAILBOX / FIFO), the main thread's refused acquire / present, the batch limit, a rebuild with images still
acquired, a rebuild whose device wait failed (deviceIdled()), a present proof the compositor withholds (the
rebuild deferred and the destroy refused after bounded waits, never the hang watchdog's; (fix 5) asked again, the
proofs are only looked at, and a deferral is one to log at first and once a minute; a destroy on a lost device
is not "withheld") and the wait for every frame in flight (infinite, never a reset). Where the compiler has them, both run again under UndefinedBehaviorSanitizer, and the present
thread's test under ThreadSanitizer.
"""
import os
import platform
import shutil
import subprocess
import tempfile

NAME = "framegen-present"
STAGE = "source"
DESCRIPTION = "frame generation present thread: job paths, releases, rebuilds, admission, markers (fake Vulkan tests)"
GUARDS = ("A slot never released (the renderer waits for ever) or released twice, a rebuild every frame after "
          "HDR goes off, generated frames shown at the wrong time, an image presented in UNDEFINED, a lost "
          "device raised for lack of memory, one slot's images admitted and another's refused (4x / 1x "
          "judder), the present thread's work missing from Reflex's markers, a pending command buffer begun again "
          "or an illegal host signal on the out-of-memory paths, a held present freezing the viewer or taken for a "
          "GPU hang, a real GPU hang missed or misnamed while frames are skipped, or a present thread's objects "
          "destroyed under it at quit, a thread left behind at quit presenting to a window that is gone or writing "
          "a lost-device marker after a normal quit, an owed release settled after the old chain's images went, or "
          "a main thread waiting half a second per try for a present the compositor holds.")

VK = "indra/newview/vkgfx"
PRESENT_SOURCES = (VK + "/llvkframegenpresent.cpp", VK + "/vk_present_handoff.cpp", VK + "/vk_frame_pacer.cpp")
PRESENT_TEST = "scripts/check/test_framegen_present.cpp"
SWAP_SOURCES = (VK + "/vk_swapchain.cpp",)
SWAP_TEST = VK + "/tests/vk_swapchain_ui_test.cpp"
STUBS = "scripts/check/stubs"
TSAN_UNSUPPORTED = "unexpected memory mapping"


def _compile(cxx, ctx, sources, test, exe, extra):
    return subprocess.run([cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
                           "-pthread"] + extra +
                          ["-I" + ctx.path(STUBS), "-I" + ctx.path(VK), "-I" + ctx.path(VK + "/third_party/vulkan/include")] +
                          [ctx.path(s) for s in sources] + [ctx.path(test), "-o", exe], capture_output=True, text=True)


def _run(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(cmd, 1, e.stdout or "", "timed out after 120 s (a job or a drain hangs?)")


def _suite(cxx, ctx, result, tmp, label, sources, test, tsan):
    """Builds and runs one test: plain, UBSan, TSan (when asked). Returns the notes, or None on a failure."""
    exe = os.path.join(tmp, label)
    c = _compile(cxx, ctx, sources, test, exe, [])
    if c.returncode:
        result.fail("%s: compile failed:\n%s%s" % (label, c.stdout, c.stderr))
        return None
    r = _run([exe])
    if r.returncode:
        result.fail("%s:\n%s%s" % (label, r.stdout, r.stderr))
        return None
    notes = []
    ubsan = exe + "_ubsan"
    if _compile(cxx, ctx, sources, test, ubsan, ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"]).returncode:
        notes.append("no UBSan")
    else:
        r = _run([ubsan])
        if r.returncode:
            result.fail("%s under UndefinedBehaviorSanitizer:\n%s%s" % (label, r.stdout, r.stderr))
            return None
        notes.append("UBSan")
    if not tsan:
        return notes
    # ThreadSanitizer (the present thread and the renderer): as the pacer's check, under setarch -R where
    # the kernel maps memory TSan cannot use.
    t = exe + "_tsan"
    if _compile(cxx, ctx, sources, test, t, ["-fsanitize=thread"]).returncode:
        notes.append("no TSan")
        return notes
    setarch = shutil.which("setarch")
    r = _run([setarch, platform.machine(), "-R", t] if setarch else [t])
    if setarch and r.returncode and r.stderr.startswith("setarch:"):
        r = _run([t])
    if r.returncode and TSAN_UNSUPPORTED in r.stderr:
        result.warn("%s: ThreadSanitizer cannot run on this kernel: the thread test ran without it" % label)
        notes.append("TSan could not run")
    elif r.returncode:
        result.fail("%s under ThreadSanitizer (exit %d):\n%s%s" % (label, r.returncode, r.stdout, r.stderr))
        return None
    else:
        notes.append("TSan")
    return notes


def _source_rules(ctx):
    """(final review A minor 2) Gate values the present thread opens itself, beside a pacer that may still be
    signalling the batch before: the pacer is drained first (the present thread is its pushing thread, so the
    pacer's one-thread rule holds), or a later signal of a lower value from the pacer would move the gate's
    timeline backwards (VUID-VkSemaphoreSignalInfo-value-03258)."""
    import re
    from .framegen_direct import _function, _mask
    with open(ctx.path(PRESENT_SOURCES[0]), encoding="utf-8", errors="replace") as f:
        pt = _mask(f.read(), blank_strings=True)
    job = _function(pt, "void LLVKFrameGenPresent::presentJob(") or ""
    problems = []
    m = re.search(r"else\s+if\s*\(\s*first\s*\)", job)
    block = (_function(job[m.start():], "else") or "") if m else ""
    d = block.find("fg.pacer().drain();")
    r = block.find("fg.releaseGates(first, n);")
    if not m or d < 0 or r < 0 or d > r:
        problems.append("presentJob: the gates of a batch whose lead present was withheld (`else if (first) { ... }`) "
                        "are opened only after `fg.pacer().drain();` -- the pacer may still be signalling the batch "
                        "before, and a lower value signalled later is not allowed on a timeline")
    # (any other releaseGates in the job: only where the pacer refused the push -- never started, nothing pending)
    for c in re.finditer(r"fg\.releaseGates\s*\(", job):
        before = job[:c.start()]
        if m and before.rfind("else if (first)") > before.rfind("fg.pacer().push("):
            continue
        if not re.search(r"!\s*fg\.pacer\(\)\.push\([^;]*\)\s*\)\s*\{[^}]*$", before):
            problems.append("presentJob: releaseGates( outside the refused-push branch and the withheld-lead branch: "
                            "drain the pacer before the present thread signals gate values itself")
    problems += _source_rules_1544(ctx, pt)
    return problems


def _source_rules_1544(ctx, pt):
    """(#1544 Task 6) What a second generator needs of the present thread and the render loop:
      FALLBACK  a generated frame that could not be composed is copied into the swapchain image only when the
                scratch and chain formats are the same, else blitted (FSR's scratch is RGBA8 on a BGRA8 chain: a
                copy would swap red and blue);
      SERIAL    every time the slots' images or the input copies are made again, imagesSerial counts (a new image
                may get an old handle back), and fillGenSize hands the generator every slot's images, the scratch
                image and that serial;
      INPUTS    fillGenInputs fills the display's peak, the time since the rendered frame before and the frame's
                serial (FSR's constants and its frame-id rule);
      SWITCH    a change of RenderVKFrameGenGenerator asks the render loop for a rebuild (generatorChangeAsked),
                where the switch is made (rule STALL)."""
    import re
    from .framegen_direct import CTX, FG, _function, _mask, _raw
    problems = []
    cp = _function(pt, "void copyToPresent(") or ""
    if "vkCmdBlitImage(" not in cp or "vkCmdCopyImage(" not in cp or \
            not re.search(r"if\s*\(\s*srcFormat\s*==\s*dstFormat\s*\)", cp):
        problems.append("FALLBACK: copyToPresent copies when srcFormat == dstFormat and blits otherwise")
    job = _function(pt, "void LLVKFrameGenPresent::presentJob(") or ""
    if not re.search(r"copyToPresent\s*\(\s*s\.gen\s*,\s*world\.image\s*,\s*world\.format\s*,[^;]*chain\.format\s*\(\s*\)", job):
        problems.append("FALLBACK: the generated frame's fallback passes the world's format and the chain's "
                        "(copyToPresent(s.gen, world.image, world.format, ..., chain.format(), ...))")
    for fn in ("bool LLVKFrameGenPresent::makeImages(", "bool LLVKFrameGenPresent::prepareInputs("):
        body = _function(pt, fn) or ""
        if "++mImagesSerial" not in body:
            problems.append("SERIAL: %s..) must count ++mImagesSerial when it makes images" % fn)
    fs = _function(pt, "void LLVKFrameGenPresent::fillGenSize(") or ""
    flat = re.sub(r"\s+", "", fs).replace("size.", "")
    for need in ("imagesSerial=mImagesSerial;", "slots=mSlotCount;", "output=llfg::imageRef(mScratch);"):
        if need not in flat:
            problems.append("SERIAL: fillGenSize must set %s" % need)
    for field, img in (("color", "hudless"), ("depth", "depth"), ("motion", "motion")):
        if not re.search(r"%s\[k\]=[^;]*llfg::imageRef\(mSlots\[k\]\.%s\)" % (field, img), flat):
            problems.append("SERIAL: fillGenSize must set every slot's %s (llfg::imageRef(mSlots[k].%s))" % (field, img))
    cx = _mask(_raw(ctx, CTX) or "", blank_strings=True)
    gi = re.sub(r"\s+", "", _function(cx, "static llfg::GenInputs fillGenInputs(") or "")
    for need in ("e.peakNits=", "e.frameMs=", "e.serial="):
        if need not in gi:
            problems.append("INPUTS: fillGenInputs must set %s" % need.rstrip("="))
    rf = _function(cx, "bool LLVulkanContext::renderFrame(") or ""
    # (final review) the serial counts rendered frames that generate (frameReset counts them): a skipped frame leaves
    # no gap, so FSR's frame-id rule does not reset where the history carries over; it is read before the count
    fgh = _mask(_raw(ctx, "indra/newview/vkgfx/llvkframegen.h") or "", blank_strings=True)
    fr = _function(_mask(_raw(ctx, FG) or "", blank_strings=True), "bool LLVKFrameGen::frameReset(") or ""
    fs, fz = rf.find("fgd.frameSerial()"), rf.find("fgd.frameReset(")
    if fs < 0 or fz < 0 or fs > fz or "++mJobSerial" not in fr \
            or not re.search(r"uint64_t\s+frameSerial\s*\(\s*\)\s*const\s*\{\s*return\s+mJobSerial\s*\+\s*1\s*;", fgh):
        problems.append("INPUTS: the serial counts rendered frames that generate (frameSerial() = mJobSerial + 1, "
                        "frameReset counts ++mJobSerial), read before frameReset")
    if not re.search(r"if\s*\(\s*fgDirect\s*&&\s*fgd\.generatorChangeAsked\s*\(\s*\)\s*\)", rf):
        problems.append("SWITCH: renderFrame asks for a rebuild when the generator setting changed "
                        "(if (fgDirect && fgd.generatorChangeAsked()) ...)")
    fg = _mask(_raw(ctx, FG) or "", blank_strings=True)
    if not _function(fg, "bool LLVKFrameGen::generatorChangeAsked("):
        problems.append("SWITCH: LLVKFrameGen::generatorChangeAsked is missing")
    return problems


def run(ctx, result):
    missing = [p for p in PRESENT_SOURCES + (PRESENT_TEST,) + SWAP_SOURCES + (SWAP_TEST,) if not os.path.exists(ctx.path(p))]
    if missing:
        result.fail("missing: " + ", ".join(missing))
        result.summary = "files missing"
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: present thread tests not run")
        result.summary = "not run"
        return
    with tempfile.TemporaryDirectory() as tmp:
        present = _suite(cxx, ctx, result, tmp, "test_framegen_present", PRESENT_SOURCES, PRESENT_TEST, True)
        if present is None:
            return
        swap = _suite(cxx, ctx, result, tmp, "vk_swapchain_ui_test", SWAP_SOURCES, SWAP_TEST, False)
        if swap is None:
            return
        enumeration = _suite(cxx, ctx, result, tmp, "vk_enumeration_test", (),
                             "scripts/check/test_vk_enumeration.cpp", False)
        if enumeration is None:
            return
    problems = _source_rules(ctx)
    for p in problems:
        result.fail(p)
    if problems:
        result.summary = "%d rule(s) broken" % len(problems)
        return
    result.summary = "present thread tests pass (%s); swapchain mock passes (%s)" % (", ".join(present), ", ".join(swap))
