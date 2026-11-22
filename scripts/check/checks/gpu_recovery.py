"""Regression guard: the device-lost state is per renderer epoch, and no raw-handle table survives an epoch.

#1545 (GPU-loss recovery) M0 / #1546. Before it, a device loss was one process-wide one-way latch
(vk_device.cpp sDeviceLost / sLostRaised / sLostConfirmed / sLostSimulated): every vkq* hook refused work
for the rest of the session, so a new VkDevice could never run. Now:
  * the lost / raised / confirmed / simulated / reaping flags live in EpochState, one per renderer epoch;
    vkDeviceIsLost() answers for the epoch the thread looks at (the current one, or a VkEpochView);
  * every vkq* hook gates on the epoch of the handle it is given (handleLost) and raises against that
    epoch (epochLostRaise): a retired epoch's straggler is refused and never marks the current epoch lost;
  * vkEpochBegin empties every process-wide table keyed by raw Vulkan handles (a new VkDevice can hand out
    the same values): the async command buffer registry, the copy-on-write descriptor masks, the address
    registry, the pipeline module / dynamic-state / module-hold registries, the descriptor set layout
    registry and the compile worker's layout recipes. A new process-wide handle-keyed table must be added
    to vkEpochBegin (and to TABLES below), or carry the comment  // epoch-table-ok  saying why it is safe.
Old code (the process-wide latch, hooks reading vkDeviceIsLost(), no vkEpochBegin) fails every part.

M1 / #1547 (in-session recovery), the order that keeps it safe:
  * recovery starts only on a loss the DRIVER reported (VK_ERROR_DEVICE_LOST) or a simulated one -- never on a
    hang verdict or a lost surface (no watchdog, owner decision O1) -- and never under VR (#1551);
  * a simulated loss's healthy device opens its gates only after the present thread stopped and a bounded idle
    proof; the teardown reaches every per-device singleton (renderer, frame generation, DLSS, facade, backend
    target ids, instant replay, RmlUi, process statics);
  * the reaper holds a VkEpochView of the retired epoch, destroys its swapchains first, then its device -- never
    the instance or messenger, never a wait on a lost device; a healthy one is proven idle again (the driver's
    own vkDeviceWaitIdle, after every submitter is joined) before any child goes -- and the new epoch begins
    only after the swapchains are gone and the retired device is destroyed (or kept): a vkDestroyDevice beside
    a live device on the same GPU crashed the NVIDIA driver (its event threads share one driver object);
  * a reused VkDevice / VkQueue value belongs to the newest epoch (registration scrubs it from older slots):
    the soak's cascade was a new device judged by the retired epoch's lost flag;
  * the new device is made on the same instance and surface with the startup device's requests -- ray tracing,
    the DLSS add-on and frame generation come back (#1550); the add-on released the retired device first;
  * a graveyard entry's CPU objects go only after its device did; the main loop tries the recovery before the
    #135 exit; RenderVKDeviceLossRecovery / RenderVKDebugRecoverySoak exist and do not persist.
"""
import os
import re

NAME = "gpu-recovery"
STAGE = "source"
DESCRIPTION = "device-lost state is per renderer epoch; handle-keyed tables are emptied when an epoch begins"
GUARDS = ("a GPU-loss recovery (#1545) whose new VkDevice inherits the old device's lost latch, or whose new "
          "handles alias the retired device's entries in a process-wide table (#1546)")

VK = os.path.join("indra", "newview", "vkgfx")
HOOKS = ("vkqQueueSubmit", "vkqQueueSubmit2", "vkqWaitForFences", "vkqWaitSemaphores", "vkqGetFenceStatus",
         "vkqQueueWaitIdle", "vkqGetSemaphoreCounterValue", "vkqGetQueryPoolResults", "vkqDeviceWaitIdle")
# (file, the reset function, what it must clear)
TABLES = (
    ("vk_device.cpp", "vkEpochBegin", ("sAsyncCmds", "sCowMask", "addrRegEpochReset", "vkPipelineEpochReset",
                                       "vkDescPoolEpochReset", "vkPipeworkerEpochReset")),
    ("vk_device.cpp", "addrRegEpochReset", ("live.clear", "byBuf.clear", "imgLive.clear", "views.clear",
                                            "descSets.clear", "blkLive.clear")),
    ("vk_pipeline.cpp", "vkPipelineEpochReset", ("modRegistry().clear", "dynRegistry().clear", "holds().clear")),
    ("vk_descpool.cpp", "vkDescPoolEpochReset", ("sLayouts.clear",)),
    ("vk_pipeworker.cpp", "vkPipeworkerEpochReset", ("setLayouts().clear", "pipeLayouts().clear")),
)
# Process-wide (static / function-local static) containers keyed by a Vulkan handle type.
STATIC_TABLE = re.compile(r"^\s*(?:static\s+)?std::(?:unordered_map|map|unordered_set|set)<\s*Vk[A-Z]\w*\s*,")
KNOWN_STATIC = ("sLayouts", "modRegistry", "dynRegistry", "holds", "setLayouts", "pipeLayouts")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def body_of(src, name):
    """The brace-balanced body of the definition of `name` (the one followed by a '{'), or None."""
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", src):
        depth, i = 0, m.end() - 1
        while i < len(src):
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            if depth == 0:
                return src[m.start():i + 1]
            i += 1
    return None


def check_epochs(ctx, result):
    dev = read(ctx.path(VK, "vk_device.cpp"))
    for old in ("sDeviceLost{", "sLostRaised{", "sLostConfirmed{", "sLostSimulated{"):
        if old in dev:
            result.fail("vk_device.cpp: the process-wide lost latch (%s) is back; the state is per epoch" % old[:-1])
    state = re.search(r"struct EpochState\s*\{.*?\n\};", dev, re.S)
    if not state:
        result.fail("vk_device.cpp: struct EpochState (the per-epoch lost state) is missing")
    else:
        for f in ("lost", "raised", "simulated", "confirmed", "reaping"):
            if not re.search(r"std::atomic<bool>\s+%s\b" % f, state.group(0)):
                result.fail("vk_device.cpp: EpochState has no '%s' flag" % f)
    is_lost = body_of(dev, "bool vkDeviceIsLost")
    if not is_lost or "viewEpoch()" not in is_lost:
        result.fail("vk_device.cpp: vkDeviceIsLost must answer for the viewing epoch (viewEpoch())")
    for hook in HOOKS:
        body = body_of(dev, "VkResult " + hook)
        if body is None:
            result.fail("vk_device.cpp: %s is not defined" % hook)
            continue
        if "handleLost(" not in body:
            result.fail("vk_device.cpp: %s does not gate on its handle's epoch (handleLost)" % hook)
        if re.search(r"\bvkDeviceLostRaise\s*\(", body) and "VkEpochView" not in body:
            result.fail("vk_device.cpp: %s raises against the viewing epoch; raise against the handle's "
                        "(epochLostRaise / vkLostCheck with the handle)" % hook)
    raise_body = body_of(dev, "static void epochLostRaise")
    if not raise_body or "vkEpochCurrent()" not in raise_body:
        result.fail("vk_device.cpp: epochLostRaise must not report a retired epoch's loss as the current one")
    if raise_body and "vkDriverCallsText" not in raise_body:
        result.fail("vk_device.cpp: a loss no longer logs which threads were inside driver calls (#1546)")
    reg = body_of(dev, "bool VkDeviceContext::createLogicalDevice")
    if not reg or "vkEpochRegisterHandles" not in reg:
        result.fail("vk_device.cpp: createLogicalDevice does not register the device / queues with the epoch")


def check_tables(ctx, result):
    for fname, func, needs in TABLES:
        src = read(ctx.path(VK, fname))
        body = body_of(src, func)
        if body is None:
            result.fail("%s: %s is not defined" % (fname, func))
            continue
        for n in needs:
            if n not in body:
                result.fail("%s: %s does not clear %s" % (fname, func, n))
    scanned = 0
    for base, dirs, files in os.walk(ctx.path(VK)):
        dirs[:] = [d for d in dirs if d not in ("third_party", "tests", "tools")]
        for f in files:
            if not f.endswith((".cpp", ".h")):
                continue
            scanned += 1
            path = os.path.join(base, f)
            for n, raw in enumerate(read(path).splitlines(), 1):
                if "epoch-table-ok" in raw or raw.strip().startswith("//"):
                    continue
                if not STATIC_TABLE.search(raw):
                    continue
                # Members of epoch-owned objects (an `m`-prefixed member, a local) die with their epoch.
                name = re.search(r">\s*&?\s*([A-Za-z_]\w*)\s*(?:\(|;|=|\{)", raw)
                ident = name.group(1) if name else ""
                is_static = raw.lstrip().startswith("static") or (ident and ident[0] == "s" and ident[1:2].isupper()) \
                    or "static std::" in raw
                if not is_static:
                    continue
                if not any(k in raw for k in KNOWN_STATIC):
                    result.fail("%s:%d a process-wide table keyed by a Vulkan handle (%s) is not emptied by "
                                "vkEpochBegin (add it, or mark it  // epoch-table-ok  with the reason)"
                                % (ctx.relative(path), n, ident or raw.strip()[:60]))
    return scanned


def order(body, *needles):
    """True when every needle occurs in body, in this order (first occurrences)."""
    at = -1
    for n in needles:
        p = body.find(n, at + 1)
        if p < 0 or p < at:
            return False
        at = p
    return True


def check_recovery(ctx, result):
    """#1547 (M1): recovery only on a confirmed (or simulated) loss; the reaper destroys only after it."""
    lvc = read(ctx.path(VK, "llvulkancontext.cpp"))
    rec = body_of(lvc, "bool LLVulkanContext::recoverFromDeviceLoss")
    if rec is None:
        result.fail("llvulkancontext.cpp: LLVulkanContext::recoverFromDeviceLoss is not defined")
        return
    # Reaper only after confirmation: the driver's VK_ERROR_DEVICE_LOST (or the simulation of one) is checked
    # before anything of the epoch is torn down or handed to the reaper (no watchdog, owner decision O1).
    if not order(rec, "vkEpochLostConfirmed(", "vkEpochLostSimulated(", "if (!confirmed && !simulated)",
                 "teardownLostEpoch(", "RecoveryReaper::get().push("):
        result.fail("llvulkancontext.cpp: recoverFromDeviceLoss must refuse an unconfirmed loss (not reported by the "
                    "driver, not simulated) before the teardown and before anything goes to the reaper")
    if "mXrHooked" not in rec:
        result.fail("llvulkancontext.cpp: recoverFromDeviceLoss must refuse a VR device (rebuilt in place: #1551)")
    # The reaper: on the retired epoch's view; the swapchains (and pop-out surfaces) first, then the device; the
    # main thread starts the new epoch only after the swapchains are gone (one swapchain per native window).
    job = rec[rec.find("RecoveryReaper::get().push("):]
    if not order(job, "VkEpochView", "vkDestroySwapchainKHR", "swapchainsReleased.store(true",
                 "releaseDeviceForRecovery()", "deviceReleased.store(true"):
        result.fail("llvulkancontext.cpp: the reaper job must hold a VkEpochView of the retired epoch and destroy its "
                    "swapchains before its device")
    if not order(rec, "RecoveryReaper::get().push(", "swapchainsReleased.load(", "vkEpochBegin(", "rebuildDeviceEpoch("):
        result.fail("llvulkancontext.cpp: the new epoch must begin after the reaper released the old swapchains, "
                    "and before the new device is built")
    # #1547 soak crash (GTX 1080, 580.178.04, third recovery): the retired device's vkDestroyDevice on the reaper
    # overlapped the new device's life; both devices' driver event threads wait on one shared driver object, and the old
    # device's thread handled an event inside its destroy (SIGSEGV in libnvidia-eglcore). The new device is built
    # only after the reaper destroyed the old one (or it is kept, never destroyed this session).
    if not order(rec, "RecoveryReaper::get().push(", "swapchainsReleased.load(", "deviceReleased.load(",
                 "vkEpochBegin(", "rebuildDeviceEpoch("):
        result.fail("llvulkancontext.cpp: recoverFromDeviceLoss must wait for the reaper to destroy the retired "
                    "device (deviceReleased, unless keepDevice) before the new epoch's device is created: a "
                    "vkDestroyDevice beside a live device crashes the NVIDIA driver")
    elif not re.search(r"!\s*g->keepDevice\s*&&\s*!\s*g->deviceReleased\.load\(", rec):
        result.fail("llvulkancontext.cpp: the device-release wait must skip a kept device (keepDevice: never destroyed)")
    td = body_of(lvc, "bool LLVulkanContext::teardownLostEpoch")
    if td is None:
        result.fail("llvulkancontext.cpp: LLVulkanContext::teardownLostEpoch is not defined")
    else:
        if not order(td, "mFgPresent.stop()", "boundedIdleProof(", "vkEpochSetReaping(g.epoch, true)"):
            result.fail("llvulkancontext.cpp: a simulated loss's gates open only after the present thread stopped and "
                        "the device was proven idle (boundedIdleProof)")
        for need in ("->shutdown()", "shutdownDirect(", "LLVKNeuralRecon::instance().releaseDevice(", "gVK.forgetDevice()",
                     "LLRenderVKBackend::epochReset()", "LLInstantReplay::shutdown(", "mUIRecorderShutdown",
                     "releaseProcessStatics()"):
            if need not in td:
                result.fail("llvulkancontext.cpp: teardownLostEpoch does not reach %s" % need)
    rb = body_of(lvc, "bool LLVulkanContext::rebuildDeviceEpoch")
    if rb is None or "initOnInstance(" not in rb or "old.mSurface" not in rb:
        result.fail("llvulkancontext.cpp: rebuildDeviceEpoch must build the new device on the same instance "
                    "(initOnInstance) and reuse the window surface")
    elif ("setRayTracingRequested(false)" in rb or not order(rb, "applyDeviceRequests(", "installAddonDeviceProviders(",
                                                               "initOnInstance(", "initAddonAndFrameGen(",
                                                               "mSwapchain.init(")):
        # #1550 (owner: recover with all features unchanged): the new device is made from the startup device's
        # requests (ray tracing included) with the DLSS add-on's / frame generation's extension providers, and the
        # add-on + frame generation start on it before its swapchain (Reflex mode), as init() does.
        result.fail("llvulkancontext.cpp: rebuildDeviceEpoch must make the new device from the startup requests "
                    "(applyDeviceRequests, installAddonDeviceProviders) and start the DLSS add-on and frame generation "
                    "on it (initAddonAndFrameGen) before the swapchain: every feature comes back (#1550)")
    if td is not None and not order(td, "->shutdown()", "shutdownDirect(", "releaseDevice(") :
        # #1550: the add-on releases the retired device (its features, then its own shutdown on that device) after
        # the renderer let go of its features and before the reaper destroys the device; the new device's
        # initDevice then binds it afresh (the shim binds one device: a skipped release left it on the dead one).
        result.fail("llvulkancontext.cpp: teardownLostEpoch must release the DLSS add-on from the retired device "
                    "(LLVKNeuralRecon::releaseDevice) after the renderer's shutdown and frame generation's (#1550)")
    # #1547 AMD / Windows TDR: vkCreateDevice was refused one second before the driver's reset finished. A
    # driver-reported loss retries the device (on the adopted instance, never a second adoption) for a bounded
    # window with backoff, pumping the window between attempts; a missing extension / feature fails at once.
    if rb is not None and not (order(rb, "initOnInstance(", "while (", "recoveryCreateRetryable(", "kRecoveryCreateRetryMs",
                                     "waitPumping(", "retryOnInstance(") and "driverReported" in rb):
        result.fail("llvulkancontext.cpp: rebuildDeviceEpoch must retry a refused device after a driver-reported loss "
                    "(retryOnInstance, bounded by kRecoveryCreateRetryMs, waitPumping between attempts): a Windows "
                    "driver reset can still be in progress when the first vkCreateDevice runs")
    if not re.search(r"constexpr double kRecoveryCreateRetryMs = \d+(?:\.\d*)?;", lvc):
        result.fail("llvulkancontext.cpp: the device retry window (kRecoveryCreateRetryMs) must be a bounded constant")
    if rec is not None and "waitPumping" not in rec:
        result.fail("llvulkancontext.cpp: recoverFromDeviceLoss must hand rebuildDeviceEpoch a wait that pumps the "
                    "window and the wait notice")
    gy = body_of(lvc, "void LLVulkanContext::pollRecoveryGraveyard")
    if gy is None or not order(gy, "deviceReleased.load(", "VkEpochView", "delete g->impl"):
        result.fail("llvulkancontext.cpp: a graveyard entry is freed only after the reaper destroyed its device, "
                    "with the retired epoch in view")

    dev = read(ctx.path(VK, "vk_device.cpp"))
    retry = body_of(dev, "bool VkDeviceContext::retryOnInstance")
    if retry is None or "mReqReport = VkRequirementsReport{}" not in retry or "mAsyncMode = mRecoveryAsyncMode" not in retry:
        result.fail("vk_device.cpp: retryOnInstance must start each attempt from a fresh requirements report (a failed "
                    "enumeration's instanceResult sticks otherwise) and the requested async compute mode")
    m = re.search(r"bool VkDeviceContext::recoveryCreateRetryable\([^;{]*\)\s*const\s*\{.*?\n\}", dev, re.S)
    cls = m.group(0) if m else None
    if cls is None or "VK_ERROR_EXTENSION_NOT_PRESENT" not in cls or "VK_ERROR_FEATURE_NOT_PRESENT" not in cls:
        result.fail("vk_device.cpp: recoveryCreateRetryable must fail fast on a missing extension / feature")
    rel = body_of(dev, "void VkDeviceContext::releaseDeviceForRecovery")
    if rel is None:
        result.fail("vk_device.cpp: VkDeviceContext::releaseDeviceForRecovery is not defined")
    elif "vkDestroyInstance" in rel or "vkDestroyDebugUtilsMessenger" in rel or "vkDeviceWaitIdle" in rel:
        result.fail("vk_device.cpp: the reaper never destroys the instance / messenger (the next epoch's) and never "
                    "waits on the retired device itself (a healthy one: retiredDeviceIdle)")
    # No destroy on a HEALTHY retired device before it is idle again after the whole teardown (the earlier
    # proof alone does not cover what was submitted after it), and never a wait on a LOST one: the lost
    # test short-circuits the idle wait, and both come before the first child destroy.
    elif not order(rel, "vkDeviceIsLost() || !retiredDeviceIdle()", "vkDestroyCommandPool", "vkDestroyDevice"):
        result.fail("vk_device.cpp: releaseDeviceForRecovery must prove a healthy device idle (retiredDeviceIdle, "
                    "skipped when lost) before destroying any of its children")
    idle = body_of(dev, "bool VkDeviceContext::retiredDeviceIdle")
    if idle is None or "vkDeviceWaitIdle(mDevice)" not in idle:
        result.fail("vk_device.cpp: retiredDeviceIdle must be the driver's own vkDeviceWaitIdle (not the hook: its "
                    "installed idle function and #699 short-cut belong to the current epoch)")
    elif dev.find("#undef vkDeviceWaitIdle") < 0 or dev.find("#undef vkDeviceWaitIdle") > dev.find(idle):
        result.fail("vk_device.cpp: retiredDeviceIdle must come after '#undef vkDeviceWaitIdle' (the raw call)")
    # #1547 handle reuse: a destroyed device's VkDevice / VkQueue values come back from the next vkCreateDevice.
    # The newest epoch listing a value owns it, and registering scrubs it from older epochs -- else the new
    # device inherits the retired epoch's lost state and every hook refuses it (the soak cascade).
    eoh = body_of(dev, "uint32_t epochOfHandle")
    if eoh is None or "e > best" not in eoh:
        result.fail("vk_device.cpp: epochOfHandle must answer the NEWEST epoch that lists a handle (a reused "
                    "VkDevice / VkQueue value belongs to the new device)")
    reg = body_of(dev, "void vkEpochRegisterHandles")
    if reg is None or "&other == s" not in reg or "slot.store(0" not in reg:
        result.fail("vk_device.cpp: vkEpochRegisterHandles must scrub the new handles from every older epoch slot")
    imm = body_of(dev, "void VkDeviceContext::submitImmediate")
    if imm is None or not order(imm, "vkWaitForFences(", "if (vkDeviceIsLost()) return;", "vkDestroyFence("):
        result.fail("vk_device.cpp: submitImmediate must not destroy its fence / command buffer after a loss (they may "
                    "still be pending: VUID-vkDestroyFence-fence-01120)")
    # Leak-free healthy reap: the renderer's shutdown frees the static-caster shadow layers (VUID 05137).
    rnd = body_of(read(ctx.path(VK, "llcleanrenderervk.cpp")), "void LLCleanRendererVK::shutdown")
    if rnd is None or "destroyShadowStatic()" not in rnd:
        result.fail("llcleanrenderervk.cpp: LLCleanRendererVK::shutdown must destroy the static-caster shadow layers "
                    "(destroyShadowStatic): leaked at vkDestroyDevice otherwise")
    # The recovery teardown does not wait for queued spec-set compiles whose pipelines it destroys anyway.
    spec = body_of(read(ctx.path(VK, "llcleanspecvk.cpp")), "void LLCleanRendererVK::specShutdown")
    if spec is None or not order(spec, "pipelineJobCancel(s.gfxJob)", "pipelineJobFinish(s.gfxJob)",
                                 "pipelineJobCancel(s.rtJob)", "pipelineJobFinish(s.rtJob)"):
        result.fail("llcleanspecvk.cpp: specShutdown must cancel a set's compile jobs before finishing them")
    dcb = body_of(dev, "static VKAPI_ATTR VkBool32 VKAPI_CALL debugCb")
    if dcb is None or "sMessengerCtx" not in dcb:
        result.fail("vk_device.cpp: the debug messenger must report through sMessengerCtx (the instance outlives "
                    "the context that made it)")

    app = read(ctx.path("indra", "newview", "llappviewer.cpp"))
    if not re.search(r"if\s*\(\s*!\s*recoverGraphicsDevice\s*\(\s*\)\s*\)\s*onGraphicsDeviceLost\s*\(\s*\)\s*;", app):
        result.fail("llappviewer.cpp: the main loop must try recoverGraphicsDevice() before the exit "
                    "(onGraphicsDeviceLost)")
    rgd = body_of(app, "bool LLAppViewer::recoverGraphicsDevice")
    if rgd is None or "RenderVKDeviceLossRecovery" not in rgd or "kGpuLossRecoveriesPerWindow" not in rgd:
        result.fail("llappviewer.cpp: recoverGraphicsDevice must honour RenderVKDeviceLossRecovery and the "
                    "repeat-loss window")
    settings = read(ctx.path("indra", "newview", "app_settings", "settings.xml"))
    for key in ("RenderVKDeviceLossRecovery", "RenderVKDebugRecoverySoak"):
        m = re.search(r"<key>%s</key>\s*<map>(.*?)</map>" % key, settings, re.S)
        if not m:
            result.fail("settings.xml: %s is missing" % key)
        elif not re.search(r"<key>Persist</key>\s*<integer>0</integer>", m.group(1)):
            result.fail("settings.xml: %s must not persist (toggles never persist)" % key)


def run(ctx, result):
    check_epochs(ctx, result)
    scanned = check_tables(ctx, result)
    check_recovery(ctx, result)
    check_explicit_reclaim(ctx, result)
    result.summary = "%d hooks, %d table resets, %d vkgfx files scanned; recovery sequence, explicit reclaim, 6 memory verdicts, NGX discovery lifecycle" % (
        len(HOOKS), len(TABLES), scanned)


def check_explicit_reclaim(ctx, result):
    dev = read(ctx.path(VK, 'vk_device.cpp'))
    context = read(ctx.path(VK, 'llvulkancontext.cpp'))
    neural = read(ctx.path(VK, 'llvkneuralrecon.cpp'))
    release = body_of(dev, 'void VkDeviceContext::releaseDeviceForRecovery') or ''
    storage = body_of(dev, 'void VkDeviceContext::releaseLostStorageForRecovery') or ''
    pools = body_of(dev, 'void VkDeviceContext::destroyTexturePools') or ''
    if not order(release, 'releaseLostStorageForRecovery()', 'vkDestroyDevice('):
        result.fail('Lost storage must be explicitly reclaimed on the reaper before device destruction')
    if not order(storage, 'vkDestroyImageView(', 'vkDestroyImage(', 'vkDestroyBuffer(', 'destroyTexturePools(true)', 'vkFreeMemory('):
        result.fail('Retained views/images/buffers and VMA must be released before the remaining dedicated memory')
    if 'mAllocs.size()' not in storage or 'trackFree(memory)' not in storage:
        result.fail('Recovery must reclaim the bounded set of tracked dedicated/alias allocations too')
    if 'if (vkDeviceIsLost() && !recovery) return;' not in pools or 'vmaDestroyAllocator(mVma)' not in pools:
        result.fail('Recovery must explicitly destroy lost-device VMA, not abandon its pools')
    if 'std::vector<VmaAllocation> left' in pools:
        result.fail('Reaper pool destruction must not allocate a second list of all suballocations')
    teardown = body_of(context, 'bool LLVulkanContext::teardownLostEpoch') or ''
    if not order(teardown, 'boundedIdleProof(', 'g.healthy = false;'):
        result.fail('The simulated lost-storage test must prove the healthy device idle before closing its gates')
    predicate = body_of(context, 'bool recoveryMemoryReleased') or ''
    match = re.search(r'return (.*?);', predicate, re.S)
    if not match:
        result.fail('Missing whole-epoch reclaim verdict')
    else:
        expression = re.sub(r'(\d+)(?:ull|u)\b', r'\1', match.group(1)).replace('&&', 'and').replace('/', '//')
        code = compile(expression, 'production reclaim verdict', 'eval')
        for expected_mb, remaining_mb, good in ((9901,9644,False),(9901,257,True),(9901,0,True),(0,0,False),(9901,12000,False),(9901,5000,False)):
            verdict = eval(code, {'__builtins__': {}}, dict(expected=expected_mb<<20, remaining=remaining_mb<<20))
            if verdict != good:
                result.fail('Wrong whole-epoch verdict for %s -> %s MiB' % (expected_mb,remaining_mb))
    for function, prefix in (('bool LLVKNeuralRecon::deviceExtensions', 'm'), ('bool LLVKNeuralRecon::fgDeviceExtensions', 'mFg')):
        body = body_of(neural, function) or ''
        if not all(x in body for x in ('mReuseRequirements', 'inst == '+prefix+'RequirementsInstance', 'pd == '+prefix+'RequirementsPhysical')):
            result.fail('Recovery discovery reuse needs exact instance AND physical-device identity: ' + function)
    device = body_of(neural, 'bool LLVKNeuralRecon::deviceExtensions') or ''
    if not order(device, 'exts = mDevExts', 'mFn->deviceExtensions('):
        result.fail('Same-GPU recovery must avoid discovery in NGX shutdown interval')
    shutdown = body_of(neural, 'bool LLVKNeuralRecon::releaseDevice') or ''
    if 'mDevExts.clear()' in shutdown or 'mReuseRequirements = mRequirementsKnown' not in shutdown:
        result.fail('Successful device-only shutdown must retain prior extension discovery')
    init = body_of(neural, 'bool LLVKNeuralRecon::initDevice') or ''
    if not order(init, 'mFn->init(', 'mFn->getCaps('):
        result.fail('Cached extension discovery must NOT replace fresh NGX init/capability checks')
