"""Source guard for the ONE stall rule of frame generation's present thread (#1517 Task 7c fix round 4): one
function, called from run(). (The other frame generation rules: framegen_direct.py, whose source helpers this
module shares; the hidden window: framegen_linux.py. The behaviour itself: framegen_handoff.py and
framegen_present.py, which run the host tests.)

  STALL    The present thread never raises a lost device and never judges: llvkframegenpresent.* and
           vk_present_handoff.* name no vkDeviceLostRaise, and the handoff has
           no stall report, give-up or hung state (StallReport, GiveUp, Held, setStallReport, waitSliced, hung).
           There is no label of its own (kVkPresentThreadStalled / vkHangIsPresentThreadStall are gone from the
           tree) and no quarantine exception (SLFxStack::deviceLostSummary() takes no argument; the device-lost
           handler calls it so). The main thread's waits for the thread are bounded: the handoff's waitSlot,
           drain and stop each take a bound and wait with wait_for only; the presenter's waitSlot waits
           mBounds.slotMs (set in prepare from llfg::slotWaitBoundMs(, never 0) and, on Busy with the thread
           held(), calls checkGpu(), which waits VkSwapchain::waitFrameSlots( and waitAccepted( and raises
           nothing itself; waitAccepted waits (vkWaitSemaphores, UINT64_MAX) only the values the present thread
           stored after a submit that succeeded (mReleaseAccepted / mGeneratedAccepted: std::atomic, stored
           after `== VK_SUCCESS` / `!= Outcome::None`, and reset to 0 with the presenter's objects in
           destroyObjects), never mSerial nor nextGeneratedDoneValue(). A device
           wait never touches the present queue while the thread exists: vkqDeviceWaitIdle asks the installed
           function (sDeviceIdleWait) before the driver's vkDeviceWaitIdle( and returns when it waited;
           llvulkancontext.cpp installs deviceIdleWait (vkSetDeviceIdleWait), which asks ownsPresentQueue(),
           waits mDevice.waitIdleExceptPresentQueue() and mFgPresent.waitAccepted( and never drains. Owners of
           what the thread uses drain first and check it: prepareFrameGen (#1544 Task 6: ONE `!drainFrameGen() ||
           vkDeviceWaitIdle(...) != VK_SUCCESS` before prepareInputs() and then prepareFeature()), fseRelease (returns before mFseRelease(),
           LLVKFrameGen::prepareFeature (mGen->release() only after `if (vkDeviceWaitIdle(...) != VK_SUCCESS)
           return false;`), and the snapshot path no longer drains. prepare() refuses when
           !chain.deviceIdled() or the thread was left behind, before it touches anything. VkSwapchain::recreate
           and destroy wait the present proofs with a bound (waitPresentProofs: no UINT64_MAX wait on
           mPresentFences anywhere). stop() is bounded (mHandoff.stop( with a bound), tells the process of a
           thread left behind (vkNoteThreadAbandoned() and shutdown destroys nothing then; both platform mains
           _exit when LLAppViewer::rendererThreadAbandoned(). A frame not rendered costs no history
           (frameSkipped( on a Busy slot wait in renderFrame; llfg::lastEvaluatedAfterSkip().

  EDGES    (fix round 5: the rule's edges) A rebuild that was not made settles nothing: renderFrame keeps the
           result of recreateMainSwapchain() (`rebuilt`), only defers when it is false, runs what follows a
           rebuild inside `if (rebuilt)`, and chooses commit / cancel of the output-mode request through
           vkdisplay::transaction::outputRequestAfterRebuild(rebuilt, ...). A thread left behind at quit stays
           quiet: the viewer's exit stops the present thread before its window goes (LLAppViewer::cleanup ->
           LLVulkanContext::stopFrameGenAtExit -> mFgPresent.stop(); LLVulkanContext::shutdown() is not reached
           on a normal exit); stop() sets mStopping (std::atomic<bool>) before it waits, and the present thread
           reads it (stoppingNow()) before presentJob and before every acquireOne( and presentBatch(;
           vkDeviceLostRaise sets the state and then asks vkLostDeviceReported(vkThreadAbandoned()) before the
           handler or the report; LLViewerWindow::destroyWindow keeps the native window (and SDL) when
           LLAppViewer::rendererThreadAbandoned(), and LLVulkanContext::shutdown reaches the surface's destroy
           only past the thread's join. The encoder's composite sets are swapped or destroyed only after a
           drainFrameGen() that succeeded (commitHdrOutputStage when the stage holds a frame or an encoder,
           releaseHdrOutputAfterSdrCommit). The add-on's feature create / release and the frame generation
           evaluate are serialised by LLVKAddonLock (llvkaddonlock.h: no handoff, the main thread's side
           bounded -- wait_for -- so no wait of the main thread sits behind an evaluate without a bound):
           every mFn->create / destroy / fgCreate / fgRelease / shutdown follows mAddonLock.tryFor(, every
           mFn->fgEvaluate follows mAddonLock.wait(. A rebuild whose device wait failed is asked again only
           while the presenter holds images (holdsImages()), and -- like every request recreateMainSwapchain
           makes after the rebuild was made -- through mFgRebuildAgain, which renderFrame turns into
           mNeedsRecreate at the top of the next frame: the caller forgets mNeedsRecreate after a rebuild that
           returned true (final review A-I1: a retry wiped by its caller froze the picture). releaseChain stops the idle thread (the driver's
           own device wait again). A deferred rebuild's retries only look at the proofs (waitPresentProofs with
           0: vkGetFenceStatus) and are logged at first and once a minute (rebuildDeferredNote()). An owed
           release is settled (settleOwedRelease() before mSwapchain.recreate( destroys the old chain's images,
           and in prepare before makeObjects(. VkSwapchain::destroy says "withheld" only for a withheld proof
           (not on a lost device). The status names what the thread waits for (heldReason(): llfg::heldText).
           Every deferred return of LLVulkanContext::shutdown before the encoder's own shutdown joins instant
           replay's save thread (LLInstantReplay::shutdown(true)).
"""
import re

from .framegen_direct import (APP, CTX, DEV, FG, FX, FX_H, NR, NR_H, PT, PT_H, POLICY_H, SWAP, SWAP_H, _bodies, _code,
                              _function, _mask, _raw)

NAME = "framegen-stall"
STAGE = "source"
DESCRIPTION = "frame generation: one stall rule -- bounded waits that never judge, a GPU hang decided by the GPU"
GUARDS = ("A held present taken for a GPU hang (a false lost device when a window is hidden, a debugger breaks or "
          "the machine sleeps), a real GPU hang given another name and no effects quarantine, or missed while "
          "frames are skipped; a viewer frozen under a present the compositor holds; the present queue touched "
          "from another thread; something the present thread uses released under it; static destructors run "
          "under a thread left inside a driver call.")

ADDON_LOCK_H = "indra/newview/vkgfx/llvkaddonlock.h"
TXN_H = "indra/newview/vkgfx/vk_display_mode_transaction.h"
VIEWER_WINDOW = "indra/newview/llviewerwindow.cpp"
CTX_H = "indra/newview/vkgfx/llvulkancontext.h"
HO_H = "indra/newview/vkgfx/vk_present_handoff.h"
HO = "indra/newview/vkgfx/vk_present_handoff.cpp"
WATCHDOG_H = "indra/newview/vkgfx/vk_hang_watchdog.h"
COMMON_H = "indra/newview/vkgfx/vk_common.h"
MAIN_WIN = "indra/newview/llappviewerwin32.cpp"
MAIN_LINUX = "indra/newview/llappviewerlinux.cpp"


def _rule_stall(ctx, bad):
    """Task 7c fix round 4: one stall rule. The present thread never raises nor judges; the main thread's waits
    for it are bounded and only drop or defer; a GPU hang is the device's sliced waits' verdict, on accepted
    work; the present queue is never touched from another thread while the thread exists."""
    pt = _mask(_raw(ctx, PT) or "", blank_strings=True)
    pth = _mask(_raw(ctx, PT_H) or "", blank_strings=True)
    ho = _mask((_raw(ctx, HO_H) or "") + "\n" + (_raw(ctx, HO) or ""), blank_strings=True)
    cx = _mask(_raw(ctx, CTX) or "", blank_strings=True)
    dev = _mask(_raw(ctx, DEV) or "", blank_strings=True)
    # (e) nothing on the present thread's side raises, labels or judges
    for rel, text in ((PT, pt), (PT_H, pth), ("vk_present_handoff.*", ho)):
        if re.search(r"\bvkDeviceLostRaise\b", text):
            bad.append("STALL: %s names vkDeviceLostRaise (the present thread and the waits for it never raise a lost "
                       "device: only the device's own calls and sliced waits do)" % rel)
        for word in ("onStall", "onGiveUp", "vkHangVerdict", "stallIsGpuHang", "mInPresentCall", "presentsHeld"):
            if re.search(r"\b%s\b" % word, text):
                bad.append("STALL: %s still names %s (the stall handling the one rule replaced)" % (rel, word))
    for word in ("StallReport", "GiveUp", "Held", "setStallReport", "waitSliced", "hung", "Watch"):
        if re.search(r"\b%s\b" % word, ho):
            bad.append("STALL: vk_present_handoff.* still has %s (a bound that passes only says so: no report, no "
                       "give-up)" % word)
    for rel in (WATCHDOG_H, APP, PT, PT_H, FX, FX_H, DEV, COMMON_H):
        if re.search(r"kVkPresentThreadStalled|vkHangIsPresentThreadStall|present thread stalled", _raw(ctx, rel) or ""):
            bad.append("STALL: %s still has the present thread's own stall label (a GPU hang has one name)" % rel)
    app = _mask(_raw(ctx, APP) or "")
    h = _function(app, "static void vk_device_lost_handler(") or ""
    if not re.search(r"SLFxStack\s*::\s*deviceLostSummary\s*\(\s*\)", h):
        bad.append("STALL: vk_device_lost_handler must call SLFxStack::deviceLostSummary() (every lost device "
                   "quarantines: no exception)")
    if not re.search(r"static\s+std::string\s+deviceLostSummary\s*\(\s*\)\s*;", _mask(_raw(ctx, FX_H) or "")):
        bad.append("STALL: SLFxStack::deviceLostSummary takes no argument (no quarantine exception)")
    # the handoff: every wait bounded
    hoc = _mask(_raw(ctx, HO) or "", blank_strings=True)
    for name, sig in (("waitSlot", "VkPresentHandoff::SlotWait VkPresentHandoff::waitSlot(uint32_t slot, std::chrono::milliseconds bound)"),
                      ("drain", "bool VkPresentHandoff::drain(std::chrono::milliseconds bound)"),
                      ("stop", "bool VkPresentHandoff::stop(std::chrono::milliseconds bound)")):
        body = _function(hoc, sig)
        if body is None:
            bad.append("STALL: the handoff's %s must take a bound (%s)" % (name, sig))
        elif not re.search(r"\.wait_for\s*\(\s*lock\s*,\s*bound\b", body) or re.search(r"\.wait\s*\(", body):
            bad.append("STALL: the handoff's %s must wait with wait_for(lock, bound, ...) only" % name)
    # the presenter: the slot wait's bound, then the GPU check
    prep = _function(pt, "bool LLVKFrameGenPresent::prepare(") or ""
    if not re.search(r"mBounds\.slotMs\s*=\s*llfg::slotWaitBoundMs\s*\(\s*mPresentMode\s*\)\s*;", prep):
        bad.append("STALL: prepare must bound the slot wait by the chain's present mode (mBounds.slotMs = "
                   "llfg::slotWaitBoundMs(mPresentMode))")
    pol = _code(_raw(ctx, POLICY_H) or "")
    sb = re.search(r"inline\s+uint32_t\s+slotWaitBoundMs\s*\([^)]*\)\s*\{([^}]*)\}", pol)
    if not sb or re.search(r"\b0u?\b", sb.group(1)):
        bad.append("STALL: llfg::slotWaitBoundMs must give a bound on every present mode (never 0)")
    for name in ("kSlotWaitBoundMs", "kSlotWaitBoundImmediateMs", "kHeldJobMs", "kDrainBoundMs"):
        if not re.search(r"\bconstexpr\b[^;]*\b%s\s*=" % name, pol):
            bad.append("STALL: llvkframegenpolicy.h must define llfg::%s" % name)
    if not re.search(r"\binline\b[^;{]*\blastEvaluatedAfterSkip\s*\(", pol):
        bad.append("STALL: llvkframegenpolicy.h must define the pure rule llfg::lastEvaluatedAfterSkip(")
    ws = _function(pt, "VkPresentHandoff::SlotWait LLVKFrameGenPresent::waitSlot(") or ""
    if not re.search(r"mHandoff\.waitSlot\s*\(\s*slot\s*,\s*std::chrono::milliseconds\s*\(\s*mBounds\.slotMs\s*\)\s*\)", ws):
        bad.append("STALL: LLVKFrameGenPresent::waitSlot must wait mBounds.slotMs at most "
                   "(mHandoff.waitSlot(slot, std::chrono::milliseconds(mBounds.slotMs)))")
    if not re.search(r"if\s*\(\s*held\s*\(\s*\)\s*\)\s*checkGpu\s*\(\s*\)\s*;", ws):
        bad.append("STALL: LLVKFrameGenPresent::waitSlot must check the GPU when the bound passed with the thread "
                   "held (`if (held()) checkGpu();`): the frame's own fence wait is not reached then")
    cg = _function(pt, "void LLVKFrameGenPresent::checkGpu(") or ""
    fs, wa = cg.find("waitFrameSlots("), cg.find("waitAccepted(")
    if not (0 <= fs < wa):
        bad.append("STALL: checkGpu must wait the frames in flight (VkSwapchain::waitFrameSlots() and then the present "
                   "queue's accepted work (waitAccepted()")
    # only accepted values are waited
    wacc = _function(pt, "VkResult LLVKFrameGenPresent::waitAccepted(") or ""
    if not re.search(r"vkWaitSemaphores\s*\([^;]*UINT64_MAX\s*\)", wacc):
        bad.append("STALL: waitAccepted must wait through the device's sliced wait (vkWaitSemaphores(..., UINT64_MAX))")
    for word in ("mSerial", "nextGeneratedDoneValue", "presentQueue", "vkQueueWaitIdle"):
        if word in wacc:
            bad.append("STALL: waitAccepted names %s (it waits only values a submit that succeeded signals, and "
                       "never touches the queue)" % word)
    for name in ("mReleaseAccepted", "mGeneratedAccepted"):
        if not re.search(r"\bAccepted\s+%s\s*;" % name, pth):
            bad.append("STALL: %s must be an Accepted (atomic semaphore and value) in %s" % (name, PT_H))
    acc = re.search(r"struct\s+Accepted\s*\{(.*?)\n\s*\};", pth, flags=re.S)
    if not acc or not re.search(r"std::atomic<VkSemaphore>\s+semaphore", acc.group(1)) or \
            not re.search(r"std::atomic<uint64_t>\s+value", acc.group(1)):
        bad.append("STALL: LLVKFrameGenPresent::Accepted must hold std::atomic<VkSemaphore> semaphore and "
                   "std::atomic<uint64_t> value (read by any thread)")
    # (a store of 0 is the reset with the presenter's objects: destroyObjects, nothing pending then)
    stores = [(m.start(), m.group(1)) for m in re.finditer(r"\b(mReleaseAccepted|mGeneratedAccepted)\.value\.store\s*\((?!\s*0\s*\))", pt)]
    if sorted(n for _, n in stores) != ["mGeneratedAccepted", "mReleaseAccepted"]:
        bad.append("STALL: each accepted value must be stored in exactly one place (after its submit succeeded)")
    resets = _function(pt, "void LLVKFrameGenPresent::destroyObjects(") or ""
    for name in ("mReleaseAccepted", "mGeneratedAccepted"):
        if not re.search(r"\b%s\.value\.store\s*\(\s*0\s*\)" % name, resets) or \
                len(re.findall(r"\b%s\.value\.store\s*\(\s*0\s*\)" % name, pt)) != 1:
            bad.append("STALL: %s.value must start again (store(0)) with the presenter's objects, in destroyObjects only" % name)
    for at, name in stores:
        before = pt[max(0, at - 260):at]
        ok = re.search(r"==\s*VK_SUCCESS\s*\)\s*\{\s*$", before) if name == "mReleaseAccepted" else \
             re.search(r"if\s*\(\s*gen\s*!=\s*Outcome::None\s*\)\s*$", before)
        if not ok:
            bad.append("STALL: %s.value must be stored only after its submit succeeded (never an issued value)" % name)
    # a device wait never touches the present queue while the thread exists
    idle = _function(dev, "VkResult vkqDeviceWaitIdle(") or ""
    ask = re.search(r"if\s*\(\s*(\w+)\s*&&\s*\1\s*\([^;{]*\)\s*\)\s*\{", idle)
    drv = idle.find("vkDeviceWaitIdle(", ask.end()) if ask else -1
    if "sDeviceIdleWait" not in idle or not ask or drv < 0 or not re.search(r"\breturn\b", idle[ask.end():drv]):
        bad.append("STALL: vkqDeviceWaitIdle must ask the installed wait (sDeviceIdleWait) and return its answer "
                   "before the driver's vkDeviceWaitIdle( (the present queue is the present thread's)")
    com = _code(_raw(ctx, COMMON_H) or "")
    if not re.search(r"void\s+vkSetDeviceIdleWait\s*\(\s*bool\s*\(\s*\*\s*\w*\s*\)\s*\(\s*void\s*\*\s*\w*\s*,\s*VkResult\s*\*\s*\w*\s*\)",
                     com) or "vkSetBeforeDeviceIdle" in (_raw(ctx, COMMON_H) or "") + (_raw(ctx, CTX) or "") + (_raw(ctx, DEV) or ""):
        bad.append("STALL: vk_common.h must declare vkSetDeviceIdleWait(bool (*)(void*, VkResult*), void*) (and the "
                   "before-idle hook that drained is gone)")
    if not re.search(r"vkSetDeviceIdleWait\s*\(\s*\[\s*\]\s*\([^)]*\)\s*\{\s*return\s+[^;]*->\s*deviceIdleWait\s*\(", cx):
        bad.append("STALL: %s must install Impl::deviceIdleWait (vkSetDeviceIdleWait)" % CTX)
    diw = _function(cx, "bool deviceIdleWait(") or ""
    o, q, w = diw.find("ownsPresentQueue()"), diw.find("mDevice.waitIdleExceptPresentQueue()"), diw.find("mFgPresent.waitAccepted(")
    if not (0 <= o < q < w) or re.search(r"drain", diw, flags=re.I):
        bad.append("STALL: deviceIdleWait must ask ownsPresentQueue(), wait every other queue "
                   "(mDevice.waitIdleExceptPresentQueue()) and the present queue's accepted work "
                   "(mFgPresent.waitAccepted() -- and never drain")
    if not re.search(r"bool\s+ownsPresentQueue\s*\(\s*\)\s*const\s*\{\s*return\s+mHandoff\.alive\s*\(\s*\)\s*;", pth):
        bad.append("STALL: ownsPresentQueue() must be the thread's existence (mHandoff.alive(): running, stopping or "
                   "left behind)")
    wx = _function(dev, "VkResult VkDeviceContext::waitIdleExceptPresentQueue(") or ""
    if "vkQueueWaitIdle(" not in wx or not re.search(r"q\s*==\s*mPresentQueue", wx) or "mQueue" not in wx:
        bad.append("STALL: VkDeviceContext::waitIdleExceptPresentQueue must wait (vkQueueWaitIdle) every queue but "
                   "mPresentQueue and return the result")
    # owners of what the thread uses drain first, and check it
    # (#1544 Task 6) The generator's inputs and its feature share ONE drain: the copies first (a generator binds
    # them when its feature is made), then the feature for them -- never a feature made for copies about to go.
    pin = _function(cx, "bool prepareFrameGen(") or ""
    g = re.search(r"if\s*\(\s*!\s*drainFrameGen\s*\(\s*\)\s*\|\|\s*vkDeviceWaitIdle\s*\([^;]*?\)\s*!=\s*VK_SUCCESS\s*\)\s*return\s+false\s*;", pin)
    pi, pf = pin.find("prepareInputs("), pin.find("prepareFeature(")
    if not g or pin.count("drainFrameGen(") != 1 or pi < g.end() or pf < g.end() or pf < pi:
        bad.append("STALL: prepareFrameGen must drain the present thread and wait the device ONCE, and check both "
                   "(`if (!drainFrameGen() || vkDeviceWaitIdle(...) != VK_SUCCESS) return false;`), before "
                   "prepareInputs( releases the old copies and before prepareFeature( -- the inputs first")
    # (#1544 Task 6) A live switch of generator releases the old one's feature only with the present thread drained
    # (the rebuild's drainFrameGen) and the device idle (the rebuild's own wait: chain.deviceIdled()), the pacer
    # drained, and asks the driver about pacing again.
    sw = _function(_mask(_raw(ctx, FG) or "", blank_strings=True), "void LLVKFrameGen::applyGenerator(") or ""
    gi = re.search(r"if\s*\(\s*!\s*deviceIdle\s*\)\s*return\s*;", sw)
    rl, dp = sw.find("mGen->release("), sw.find("drainPacer();")
    if not gi or rl < 0 or dp < 0 or not (gi.end() <= dp < rl) or "meteringReset();" not in sw:
        bad.append("STALL: LLVKFrameGen::applyGenerator must return when !deviceIdle, then drainPacer(), then "
                   "mGen->release(...), and meteringReset()")
    rb = _function(cx, "bool recreateMainSwapchain(") or ""
    ap = rb.find("applyGenerator(")
    dr = rb.find("drainFrameGen()")
    pr = rb.find("mFgPresent.prepare(")
    if ap < 0 or dr < 0 or not (dr < ap < pr) or \
            not re.search(r"applyGenerator\s*\(\s*mDevice\s*,\s*mSwapchain\.deviceIdled\s*\(\s*\)\s*\)", rb):
        bad.append("STALL: recreateMainSwapchain applies the generator setting (applyGenerator(mDevice, "
                   "mSwapchain.deviceIdled())) after its drainFrameGen() and before the presenter's prepare (the scratch "
                   "image's format is the generator's)")
    for a, b in _bodies(cx, "fseRelease"):
        body = cx[a:b]
        g = re.search(r"if\s*\(\s*!\s*drainFrameGen\s*\(\s*\)\s*\)\s*return\s+false\s*;", body)
        if not g or body.find("mFseRelease(") < g.end() or body.find("mFseHeld = false") < g.end():
            bad.append("STALL: fseRelease must return (false) when drainFrameGen() fails, before it releases "
                       "exclusive mode")
    snap = _function(cx, "bool LLVulkanContext::renderOffscreen(") or ""
    if not snap or "vkDeviceWaitIdle(" not in snap or "drainFrameGen" in snap[:snap.find("vkDeviceWaitIdle(")]:
        bad.append("STALL: the offscreen snapshot's device wait needs no drain (it touches nothing of the present thread's)")
    fg = _mask(_raw(ctx, FG) or "", blank_strings=True)
    pf = _function(fg, "bool LLVKFrameGen::prepareFeature(") or ""
    rel = pf.find("mGen->release(")   # (#1544: the generator's release -- DLSS: the add-on's fgRelease)
    dw = pf.rfind("vkDeviceWaitIdle(", 0, rel) if rel >= 0 else -1
    head = pf[pf.rfind(";", 0, dw) + 1:dw] if dw >= 0 else ""
    between = pf[dw:rel] if dw >= 0 else ""
    if rel < 0 or dw < 0 or not re.match(r"\s*if\s*\(\s*$", head) or \
            not re.match(r"vkDeviceWaitIdle\s*\([^;]*?\)\s*!=\s*VK_SUCCESS\s*\)\s*return\s+false\s*;", between):
        bad.append("STALL: LLVKFrameGen::prepareFeature must release the feature (mGen->release() only after its "
                   "device wait succeeded: `if (vkDeviceWaitIdle(...) != VK_SUCCESS) return false;`")
    # prepare refuses untouched after a failed device wait / with the thread left behind
    g = re.search(r"if\s*\(\s*!\s*chain\.deviceIdled\s*\(\s*\)\s*\|\|\s*mHandoff\.abandoned\s*\(\s*\)\s*\)", prep)
    first = min([i for i in (prep.find("mDev ="), prep.find("makeObjects("), prep.find("releaseImages("),
                             prep.find("mNeedsRecreate.store(")) if i >= 0] or [-1])
    if not g or first < 0 or g.start() > first or not re.search(r"return\s+false\s*;", prep[g.end():first]):
        bad.append("STALL: prepare must refuse (return false) before it touches anything when !chain.deviceIdled() "
                   "or the thread was left behind (mHandoff.abandoned())")
    rc = _function(cx, "bool recreateMainSwapchain(") or ""
    di = re.search(r"if\s*\(\s*!\s*mSwapchain\.deviceIdled\s*\(\s*\)\s*&&\s*mFgPresent\.holdsImages\s*\(\s*\)\s*\)", rc)
    rcall = rc.find("mFgPresent.releaseChain(")
    if not di or rcall < di.end() or "rebuildBackoff()" not in rc[di.end():rcall] or \
            not re.search(r"return\s+true\s*;", rc[di.end():rcall]):
        bad.append("STALL: recreateMainSwapchain must leave frame generation's images alone and ask again after a "
                   "backoff when the rebuild's device wait failed (`if (!mSwapchain.deviceIdled() && mFgPresent.holdsImages()) { ... "
                   "rebuildBackoff(); return true; }` before any releaseChain()")
    if not re.search(r"rebuildDeferred\s*\(\s*\)", rc) or not re.search(r"rebuildBackedOff\s*\(\s*\)",
                                                                        _function(cx, "bool LLVulkanContext::renderFrame(") or ""):
        bad.append("STALL: a rebuild the swapchain deferred (rebuildDeferred(): a present proof withheld) must keep "
                   "its request and be asked again after the backoff (rebuildBackedOff() in renderFrame)")
    # the presentation engine's proofs: bounded waits only
    sw = _mask(_raw(ctx, SWAP) or "", blank_strings=True)
    for m in re.finditer(r"vkWaitForFences\s*\(([^;]*)\)\s*\)?\s*;", sw):
        if "mPresentFences" in m.group(1) and "UINT64_MAX" in m.group(1):
            bad.append("STALL: %s waits a present proof (mPresentFences) without a bound: the presentation engine "
                       "signals it, and a held present is no GPU hang" % SWAP)
    for sig in ("bool VkSwapchain::recreate(", "bool VkSwapchain::destroy("):
        if "waitPresentProofs(" not in (_function(sw, sig) or ""):
            bad.append("STALL: %s must wait its present proofs with a bound (waitPresentProofs()" % sig.rstrip("("))
    wfs = _function(sw, "bool VkSwapchain::waitFrameSlots(") or ""
    if not re.search(r"vkWaitForFences\s*\([^;]*UINT64_MAX\s*\)", wfs) or "vkResetFences" in wfs or "mInFlight" not in wfs:
        bad.append("STALL: VkSwapchain::waitFrameSlots must wait every in-flight fence (mInFlight) through the sliced "
                   "wait (UINT64_MAX), without a reset")
    # shutdown: bounded, a thread left behind pins everything, the process ends without static destructors
    st = _function(pt, "bool LLVKFrameGenPresent::stop(") or ""
    if not re.search(r"mHandoff\.stop\s*\(\s*std::chrono::milliseconds\s*\(", st) or "vkNoteThreadAbandoned()" not in st:
        bad.append("STALL: LLVKFrameGenPresent::stop must wait a bound for the thread (mHandoff.stop(std::chrono::"
                   "milliseconds(...))) and tell the process when it is left behind (vkNoteThreadAbandoned())")
    sh = _function(pt, "void LLVKFrameGenPresent::shutdown(") or ""
    g = re.search(r"if\s*\(\s*!\s*stop\s*\(\s*\)\s*\|\|\s*vkDeviceIsLost\s*\(\s*\)\s*\)\s*return\s*;", sh)
    if not g or min(i for i in (sh.find("releaseImages("), sh.find("destroyObjects("), sh.find("vkQueueWaitIdle(")) if i >= 0) < g.end():
        bad.append("STALL: LLVKFrameGenPresent::shutdown must destroy nothing (and wait no queue) unless stop() "
                   "joined the thread (`if (!stop() || vkDeviceIsLost()) return;` first)")
    sd = _function(cx, "void LLVulkanContext::shutdown(") or ""
    for m in re.finditer(r"vkSetDeviceIdleWait\s*\(\s*nullptr", sd):
        before = sd[:m.start()]
        if before.rfind("mFgPresent.shutdown(") < 0 or "ownsPresentQueue()" not in before[before.rfind("mFgPresent.shutdown("):]:
            bad.append("STALL: LLVulkanContext::shutdown may uninstall the device wait only after the present thread "
                       "was joined (mFgPresent.shutdown(, then !ownsPresentQueue())")
    if not re.search(r"bool\s+LLAppViewer::rendererThreadAbandoned\s*\(\s*\)\s*\{\s*return\s+vkThreadAbandoned\s*\(\s*\)\s*;", app):
        bad.append("STALL: LLAppViewer::rendererThreadAbandoned() must report vkThreadAbandoned()")
    for rel in (MAIN_WIN, MAIN_LINUX):
        src = _mask(_raw(ctx, rel) or "")
        at = src.find("rendererThreadAbandoned()")
        if at < 0 or "_exit(" not in src[at:at + 200]:
            bad.append("STALL: %s must end the process without static destructors (_exit) when "
                       "LLAppViewer::rendererThreadAbandoned()" % rel)
    # a frame not rendered costs the generator no history
    rf = _function(cx, "bool LLVulkanContext::renderFrame(") or ""
    if not re.search(r"SlotWait::Busy\b[^;{]*\)\s*\{[^}]*?frameSkipped\s*\(\s*\)", rf):
        bad.append("STALL: renderFrame must tell the generator of a frame not rendered (frameSkipped() on a Busy slot "
                   "wait): no history reset")
    fs = _function(fg, "void LLVKFrameGen::frameSkipped(") or ""
    if "llfg::lastEvaluatedAfterSkip(" not in fs:
        bad.append("STALL: LLVKFrameGen::frameSkipped must follow llfg::lastEvaluatedAfterSkip(")


def _rule_edges(ctx, bad):
    """Task 7c fix round 5: the edges of the stall rule (see the module text, EDGES)."""
    pt = _mask(_raw(ctx, PT) or "", blank_strings=True)
    pth = _mask(_raw(ctx, PT_H) or "", blank_strings=True)
    cx = _mask(_raw(ctx, CTX) or "", blank_strings=True)
    dev = _mask(_raw(ctx, DEV) or "", blank_strings=True)
    sw = _mask(_raw(ctx, SWAP) or "", blank_strings=True)
    app = _mask(_raw(ctx, APP) or "", blank_strings=True)
    rf = _function(cx, "bool LLVulkanContext::renderFrame(") or ""
    # (1) a rebuild that was not made settles nothing
    call = re.search(r"\brebuilt\s*=\s*mImpl->recreateMainSwapchain\s*\(\s*\)\s*;", rf)
    gate = re.search(r"\bif\s*\(\s*rebuilt\s*\)\s*\{", rf[call.end():]) if call else None
    between = rf[call.end():call.end() + gate.start()] if gate else ""
    if not call or not gate or not re.search(r"if\s*\(\s*!\s*rebuilt\s*\)\s*deferSwapchainRecreate\s*=\s*true\s*;", between):
        bad.append("EDGES: renderFrame must keep recreateMainSwapchain()'s answer (`rebuilt = mImpl->"
                   "recreateMainSwapchain();`), defer when it is false (`if (!rebuilt) deferSwapchainRecreate = true;`) "
                   "and run what follows a rebuild only inside `if (rebuilt) {`")
    else:
        for word in ("cancelDisplayOutput", "mDisplayHdrPipelineReady", "mOutputRequestValid", "prepareHdrOutput",
                     "mOutputPreviewValid", "commitDisplayOutput", "releaseHdrOutputAfterSdrCommit", "mNeedsRecreate"):
            if re.search(r"\b%s\b" % word, between):
                bad.append("EDGES: renderFrame touches %s between the rebuild's try and `if (rebuilt)`: a rebuild that "
                           "was not made keeps its request and touches nothing else" % word)
        body = _function(rf[call.end() + gate.start():], "if") or ""
        for word in ("cancelDisplayOutput(", "commitDisplayOutput(", "prepareHdrOutput("):
            if word not in body:
                bad.append("EDGES: what follows a rebuild (%s...) must sit inside renderFrame's `if (rebuilt) { ... }`"
                           % word)
        if not re.search(r"outputRequestAfterRebuild\s*\(\s*rebuilt\s*,", body):
            bad.append("EDGES: renderFrame must choose commit / cancel of the output-mode request through "
                       "vkdisplay::transaction::outputRequestAfterRebuild(rebuilt, ...)")
    txn = _code(_raw(ctx, TXN_H) or "")
    if not re.search(r"constexpr\s+OutputRequestStep\s+outputRequestAfterRebuild\s*\(\s*bool\s+rebuilt\b", txn):
        bad.append("EDGES: %s must define the pure rule outputRequestAfterRebuild(bool rebuilt, ...)" % TXN_H)
    # (2a) the stopping flag
    if not re.search(r"std::atomic<bool>\s+mStopping\b", pth) or \
            not re.search(r"bool\s+stoppingNow\s*\(\s*\)\s*const\s*\{\s*return\s+mStopping\.load\s*\(\s*\)\s*;", pth):
        bad.append("EDGES: %s: mStopping must be a std::atomic<bool> read through stoppingNow()" % PT_H)
    st = _function(pt, "bool LLVKFrameGenPresent::stop(") or ""
    a, b = st.find("mStopping.store(true)"), st.find("mHandoff.stop(")
    if not (0 <= a < b):
        bad.append("EDGES: LLVKFrameGenPresent::stop must set mStopping (store(true)) before it waits for the thread")
    rj = _function(pt, "void LLVKFrameGenPresent::runJob(") or ""
    if "stoppingNow()" not in (rj[:rj.find("presentJob(")] if "presentJob(" in rj else ""):
        bad.append("EDGES: runJob must drop a job once a stop has begun (stoppingNow() before presentJob()")
    job = _function(pt, "void LLVKFrameGenPresent::presentJob(") or ""
    for callee in ("acquireOne(", "presentBatch("):
        at = job.find(callee)
        lam = job.rfind("[&]", 0, at) if at >= 0 else -1
        if lam < 0 or "stoppingNow()" not in job[lam:at]:
            bad.append("EDGES: presentJob must read the stopping flag (stoppingNow()) before every %s"
                       % callee.rstrip("("))
    # (2b) a lost device raised after a thread was left behind: the state, no report
    wd = _code(_raw(ctx, WATCHDOG_H) or "")
    if not re.search(r"inline\s+bool\s+vkLostDeviceReported\s*\(\s*bool\s+\w+\s*\)", wd):
        bad.append("EDGES: %s must define the pure rule vkLostDeviceReported(bool threadAbandoned)" % WATCHDOG_H)
    # (#1546: the raise is per renderer epoch -- vkDeviceLostRaise forwards to epochLostRaise, which sets that
    # epoch's lost flag)
    raise_ = _function(dev, "static void epochLostRaise(") or ""
    state = raise_.find("->lost.store(true")
    ask = re.search(r"if\s*\(\s*!\s*vkLostDeviceReported\s*\(\s*vkThreadAbandoned\s*\(\s*\)\s*\)\s*\)", raise_)
    handler, report = raise_.find("gVkDeviceLostHandler("), raise_.find("vkReportDeviceLost(")
    if state < 0 or not ask or not (state < ask.start() < handler) or not (ask.start() < report) or \
            not re.search(r"\breturn\s*;", raise_[ask.end():handler]):
        bad.append("EDGES: vkDeviceLostRaise must set the state, then return without the handler or the report when "
                   "`!vkLostDeviceReported(vkThreadAbandoned())` (a thread left behind at exit: no report, marker or "
                   "quarantine)")
    # (2c) the exit stops the thread before its window goes; a thread left behind keeps the window
    cl = _function(app, "bool LLAppViewer::cleanup(") or ""
    a, b = cl.find("stopFrameGenAtExit()"), cl.find("delete gViewerWindow")
    if not (0 <= a < b):
        bad.append("EDGES: LLAppViewer::cleanup must stop frame generation's present thread "
                   "(LLVulkanContext::instance().stopFrameGenAtExit()) before the window is destroyed (delete "
                   "gViewerWindow): LLVulkanContext::shutdown() is not reached on a normal exit")
    se = _function(cx, "void LLVulkanContext::stopFrameGenAtExit(") or ""
    if "mFgPresent.stop()" not in se or re.search(r"destroy|shutdown\s*\(|vkDeviceWaitIdle", se):
        bad.append("EDGES: LLVulkanContext::stopFrameGenAtExit must only stop the present thread (mImpl->mFgPresent."
                   "stop(): bounded, left behind if it does not end) -- it destroys and waits nothing")
    vw = _mask(_raw(ctx, VIEWER_WINDOW) or "", blank_strings=True)
    dw = _function(vw, "void LLViewerWindow::destroyWindow(") or ""
    a, b = dw.find("rendererThreadAbandoned()"), dw.find("LLWindowManager::destroyWindow(")
    if not (0 <= a < b):
        bad.append("EDGES: LLViewerWindow::destroyWindow must keep the native window when a renderer thread was left "
                   "behind (LLAppViewer::rendererThreadAbandoned() before LLWindowManager::destroyWindow(): the "
                   "thread may be inside a present to it")
    sd = _function(cx, "void LLVulkanContext::shutdown(") or ""
    join, surf = sd.find("mFgPresent.ownsPresentQueue()"), sd.find("SDL_Vulkan_DestroySurface(")
    if not (0 <= join < surf):
        bad.append("EDGES: LLVulkanContext::shutdown may destroy the surface only past the present thread's join "
                   "(after the `mFgPresent.ownsPresentQueue()` return)")
    # (3) composite sets: a drain of their own
    for name, first in (("commitHdrOutputStage", r"\.swap\s*\("), ("releaseHdrOutputAfterSdrCommit", r"\.destroy\s*\(")):
        found = False
        for a, b in _bodies(cx, name):
            found = True
            body = cx[a:b]
            g = re.search(r"!\s*drainFrameGen\s*\(\s*\)\s*\)\s*(?:\{[^}]*?)?return\b", body)
            use = re.search(first, body)
            if not g or not use or use.start() < g.end():
                bad.append("EDGES: %s must return when drainFrameGen() fails, before it swaps or destroys the live "
                           "encoder (its composite sets are the present thread's while a job runs)" % name)
        if not found:
            bad.append("EDGES: %s not found in %s" % (name, CTX))
    # (4) the add-on's lock
    lk = _raw(ctx, ADDON_LOCK_H)
    if lk is None:
        bad.append("EDGES: %s is missing (the add-on's lock)" % ADDON_LOCK_H)
    else:
        lkc = _code(lk)
        if re.search(r"handoff|llvkframegenpresent", lkc, flags=re.I):
            bad.append("EDGES: %s must know nothing of the handoff (no lock order with it)" % ADDON_LOCK_H)
        tf = _function(lkc, "Hold tryFor(") or ""
        if not re.search(r"\.wait_for\s*\(\s*lock\s*,\s*bound\b", tf) or re.search(r"\.wait\s*\(", tf):
            bad.append("EDGES: LLVKAddonLock::tryFor must wait with a bound (wait_for(lock, bound, ...)) only")
    nrh = _code(_raw(ctx, NR_H) or "")
    if not re.search(r"LLVKAddonLock\s+mAddonLock\s*;", nrh):
        bad.append("EDGES: LLVKNeuralRecon must own the add-on's lock (LLVKAddonLock mAddonLock)")
    nr = _mask(_raw(ctx, NR) or "", blank_strings=True)
    if re.search(r"vk_present_handoff|mFgPresent|VkPresentHandoff", nr):
        bad.append("EDGES: %s must not reach the handoff (the add-on's lock has no order with it)" % NR)
    for sig, callee, how in (("LLVKNeuralRecon::Feature* LLVKNeuralRecon::create(", "mFn->create(", "tryFor"),
                             ("void LLVKNeuralRecon::destroy(", "mFn->destroy(", "tryFor"),
                             ("int LLVKNeuralRecon::fgCreate(", "mFn->fgCreate(", "tryFor"),
                             ("void LLVKNeuralRecon::fgRelease(", "mFn->fgRelease(", "tryFor"),
                             ("void LLVKNeuralRecon::shutdown(", "mFn->shutdown(", "tryFor"),
                             ("int LLVKNeuralRecon::fgEvaluate(", "mFn->fgEvaluate(", "wait")):
        body = _function(nr, sig) or ""
        at = body.find(callee)
        lock = re.search(r"mAddonLock\.%s\s*\(" % how, body)
        if at < 0 or not lock or lock.start() > at or (how == "tryFor" and
                                                       not re.search(r"if\s*\(\s*!\s*\w+\s*\)", body[lock.end():at])):
            bad.append("EDGES: %s must hold the add-on's lock around %s (mAddonLock.%s(%s)"
                       % (sig.rstrip("("), callee.rstrip("("), how,
                          ", refused when the bound passes: `if (!hold)`" if how == "tryFor" else ""))
    # (5) a failed device wait is asked again only while the presenter holds images
    rc = _function(cx, "bool recreateMainSwapchain(") or ""
    if not re.search(r"if\s*\(\s*!\s*mSwapchain\.deviceIdled\s*\(\s*\)\s*&&\s*mFgPresent\.holdsImages\s*\(\s*\)\s*\)", rc):
        bad.append("EDGES: recreateMainSwapchain must ask again after a failed device wait only while the presenter "
                   "holds images (`if (!mSwapchain.deviceIdled() && mFgPresent.holdsImages())`)")
    if not re.search(r"bool\s+holdsImages\s*\(\s*\)\s*const", pth):
        bad.append("EDGES: %s must declare holdsImages()" % PT_H)
    # (5b, final review A-I1) a request the rebuild itself makes outlives its caller: renderFrame forgets
    # mNeedsRecreate after a rebuild that was made, so past that point the rebuild asks through mFgRebuildAgain,
    # which the next frame turns into the request (then the backoff applies as to any)
    made = rc.find("logFrameGenChain()")
    di = re.search(r"if\s*\(\s*!\s*mSwapchain\.deviceIdled\s*\(\s*\)\s*&&\s*mFgPresent\.holdsImages\s*\(\s*\)\s*\)", rc)
    blk = (_function(rc[di.start():], "if") or "") if di else ""
    if made < 0 or re.search(r"\bmNeedsRecreate\s*=\s*true", rc[made:]):
        bad.append("EDGES: recreateMainSwapchain must not set mNeedsRecreate once the rebuild was made (after "
                   "logFrameGenChain()): renderFrame clears it when the rebuild returns true -- the picture would "
                   "freeze with no frame generation chain usable and nothing asking again; set mFgRebuildAgain")
    if not re.search(r"\bmFgRebuildAgain\s*=\s*true\s*;", blk) or "rebuildBackoff()" not in blk:
        bad.append("EDGES: a rebuild whose device wait failed while the presenter holds images must ask again "
                   "through mFgRebuildAgain (with rebuildBackoff()), a request its caller does not clear")
    rf_again = re.search(r"if\s*\(\s*mImpl->mFgRebuildAgain\s*\)\s*\{\s*mImpl->mFgRebuildAgain\s*=\s*false\s*;\s*"
                         r"mImpl->mNeedsRecreate\s*=\s*true\s*;", rf)
    pend = rf.find("const bool displayRequestPending = mImpl->mNeedsRecreate;")
    if not rf_again or pend < 0 or rf_again.start() > pend:
        bad.append("EDGES: renderFrame must turn mFgRebuildAgain into the rebuild request (`if (mImpl->mFgRebuildAgain) "
                   "{ mImpl->mFgRebuildAgain = false; mImpl->mNeedsRecreate = true; ...`) before it reads the "
                   "request (displayRequestPending)")
    if len(re.findall(r"\bmFgRebuildAgain\s*=\s*false\b", cx)) != 2:   # (its declaration, and renderFrame's take)
        bad.append("EDGES: mFgRebuildAgain is forgotten only where renderFrame turns it into the request")
    # (6) a normal chain has no present thread
    rl = _function(pt, "void LLVKFrameGenPresent::releaseChain(") or ""
    look = re.search(r"mHandoff\.drain\s*\(\s*std::chrono::milliseconds\s*\(\s*0\s*\)\s*\)", rl)
    stop, images = (rl.find("stop()", look.end()) if look else -1), rl.find("releaseImages(")
    if not look or not (0 <= stop < images):
        bad.append("EDGES: LLVKFrameGenPresent::releaseChain must stop the idle present thread (a look: "
                   "mHandoff.drain(std::chrono::milliseconds(0)), then stop()) before the images go")
    # (7) a deferred rebuild's retries only look, and are logged at first and once a minute
    wp = _function(sw, "bool VkSwapchain::waitPresentProofs(") or ""
    if "vkGetFenceStatus(" not in wp:
        bad.append("EDGES: VkSwapchain::waitPresentProofs must only look (vkGetFenceStatus) when its bound is 0")
    rec = _function(sw, "bool VkSwapchain::recreate(") or ""
    if not re.search(r"waitPresentProofs\s*\(\s*ctx\.device\s*\(\s*\)\s*,\s*mProofDeferrals\s*\?\s*0\s*:\s*kRebuildProofWaitNs\s*\)", rec):
        bad.append("EDGES: VkSwapchain::recreate must wait the present proofs once per row of deferrals "
                   "(`waitPresentProofs(ctx.device(), mProofDeferrals ? 0 : kRebuildProofWaitNs)`)")
    d = rc.find("rebuildDeferred()")
    note = re.search(r"if\s*\(\s*mSwapchain\.rebuildDeferredNote\s*\(\s*\)\s*\)\s*(?:\{\s*)?LL_WARNS", rc)
    if d < 0 or not note or note.start() < d or len(re.findall(r"LL_WARNS", rc[d:rc.find("rebuildBackoff()", d)])) != 1:
        bad.append("EDGES: recreateMainSwapchain must log a deferred rebuild only when the swapchain says so "
                   "(`if (mSwapchain.rebuildDeferredNote()) LL_WARNS...`: at first and once a minute)")
    # (8) an owed release is settled before the old chain's images go
    a, b = rc.find("settleOwedRelease("), rc.find("mSwapchain.recreate(")
    if not (0 <= a < b) or not re.search(r"return\s+false\s*;", rc[a:b]):
        bad.append("EDGES: recreateMainSwapchain must settle an owed release (mFgPresent.settleOwedRelease() -- and "
                   "defer the rebuild when it cannot -- before mSwapchain.recreate( destroys the old chain's images")
    prep = _function(pt, "bool LLVKFrameGenPresent::prepare(") or ""
    a, b = prep.find("settleOwedRelease("), prep.find("makeObjects(")
    if not (0 <= a < b):
        bad.append("EDGES: prepare must settle an owed release before makeObjects( remakes anything")
    so = _function(pt, "bool LLVKFrameGenPresent::settleOwedRelease(") or ""
    a, b = so.find("mReleaseAccepted.value.load()"), so.find("mHandoff.stop(")
    if not (0 <= a < b):
        bad.append("EDGES: settleOwedRelease must leave a release the queue accepted alone "
                   "(mReleaseAccepted.value.load() before anything is stopped or signalled)")
    # (9) "withheld" only for a withheld proof
    ds = _function(sw, "bool VkSwapchain::destroy(") or ""
    w = ds.find("waitPresentProofs(")
    flag = ds.find("mDestroyWithheld = true", w)
    if w < 0 or flag < 0 or not re.search(r"if\s*\(\s*vkDeviceIsLost\s*\(\s*\)\s*\)\s*return\s+false\s*;", ds[w:flag]):
        bad.append("EDGES: VkSwapchain::destroy must return on a lost device before it says a proof is withheld "
                   "(`if (vkDeviceIsLost()) return false;` between waitPresentProofs( and mDestroyWithheld = true)")
    g = re.search(r"if\s*\(\s*!\s*mImpl->mSwapchain\.destroy\s*\(", sd)
    if not g or "destroyWithheld()" not in sd[g.end():g.end() + 900]:
        bad.append("EDGES: LLVulkanContext::shutdown must say \"still held\" only when the swapchain says so "
                   "(mSwapchain.destroyWithheld())")
    # (10) the comments that went stale
    if "or any device wait: a present queue held on a gate never idles" in \
            re.sub(r"\s*\n\s*//\s*", " ", _raw(ctx, SWAP_H) or ""):
        bad.append("EDGES: %s still says a device wait hangs on a gated present (a device wait no longer touches "
                   "the present queue while the present thread exists)" % SWAP_H)
    if "the thread stays, idle" in (_raw(ctx, PT_H) or ""):
        bad.append("EDGES: %s still says releaseChain leaves the thread running" % PT_H)
    # (11) the status names the cause
    if not re.search(r"directRefused\s*\(\s*mImpl->mFgPresent\.heldReason\s*\(\s*\)\s*\)", rf) or \
            "waiting for the display to take the last frame" in (_raw(ctx, CTX) or ""):
        bad.append("EDGES: renderFrame must show what the held thread waits for (fgd.directRefused(mImpl->mFgPresent."
                   "heldReason())), not one sentence for every cause")
    hr = re.search(r"const\s+char\s*\*\s*heldReason\s*\(\s*\)\s*const\s*\{([^}]*)\}", pth)
    if not hr or "llfg::heldText(" not in hr.group(1):
        bad.append("EDGES: LLVKFrameGenPresent::heldReason() must give llfg::heldText( of where the thread is")
    # (12) the deferred returns join instant replay's save thread
    enc = sd.find("LLInstantReplay::shutdown(false)")
    start = sd.find("drainFrameGen()")   # (past the lost path, which has its own)
    last = start
    for m in re.finditer(r"mInitialized\s*=\s*false\s*;\s*return\s*;", sd[:enc] if enc >= 0 else ""):
        if m.start() < start:
            continue
        if "LLInstantReplay::shutdown(true)" not in sd[last:m.start()]:
            bad.append("EDGES: a deferred return of LLVulkanContext::shutdown (before the encoder's own shutdown) "
                       "must join instant replay's save thread (LLInstantReplay::shutdown(true)), as the lost path")
        last = m.end()
    if enc < 0 or start < 0:
        bad.append("EDGES: LLVulkanContext::shutdown not as expected (drainFrameGen() .. "
                   "LLInstantReplay::shutdown(false))")


def run(ctx, result):
    bad = []
    _rule_stall(ctx, bad)
    _rule_edges(ctx, bad)
    for b in bad:
        result.fail(b)
    result.summary = "%d problem(s)" % len(bad) if bad else "the stall rule holds"
