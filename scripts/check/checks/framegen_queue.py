"""Source guard for frame generation on a device with ONE graphics queue (#1544, design D8).

Where the graphics family has no queue to spare, frame generation's present thread uses the main queue
(VkDeviceContext::presentQueueShared()). Vulkan wants every call on a queue serialised by the application, so
every driver call on it goes through one lock, VkDeviceContext::QueueLock -- nothing on a device whose present
thread has a queue of its own. The policy's cases (which queue, which present mode, the reason shown) are
framegen_policy.py's tests; these are the rules for where the viewer applies them. Product sources: indra/newview
without tests/ and third_party/ (the bench tools under indra/ have a device of their own and no present thread).

  HOOK     vkQueueSubmit, vkQueueSubmit2 and vkQueueWaitIdle are macros (vk_common.h) for vkqQueueSubmit /
           vkqQueueSubmit2 / vkqQueueWaitIdle; only vk_device.cpp takes them off (#undef), nothing defines
           VK_NO_ASYNC_BARRIER_HOOK, and every product file that calls one reaches vk_common.h through its
           includes -- so every such call in the product is the wrapper's.
  LOCK     The driver's own calls -- vkQueueSubmit(, gVkCompat.QueueSubmit2( and vkQueueWaitIdle( in vk_device.cpp
           after the #undefs (the wrappers), and every vkQueuePresentKHR( anywhere -- are each inside the scope
           of a VkDeviceContext::QueueLock made for the very queue the call names. gVkCompat.QueueSubmit2 is
           called nowhere else.
  OWN      The reviewed list of sites that take a queue entry point by name (PFN_vkQueue..., a
           vkGetDeviceProcAddr of one): vk_compat.{h,cpp} (called only by the wrapper: LOCK) and
           llcleanreplayvk.cpp, whose one call of its own pointer sits behind the device's submit2 hook
           (fnSubmit2: `if (g_hookSubmit2) return g_hookSubmit2(`); the viewer fills the hook with a function
           that calls vkQueueSubmit2 (llinstantreplay.cpp: the wrapper, HOOK).
  DEVICE   createLogicalDevice decides the queue with llfg::presentQueueFor( and publishes a shared one
           (sSharedQueue.store(); QueueLock takes the lock only for that queue (and once per thread), raises
           nothing, and gives up only on a device lost meanwhile or a thread left behind (vkThreadAbandoned: the call
           it sits in may hold the lock for good) -- then no driver call is made (gaveUp()); the device
           wait made while the present thread exists (waitIdleExceptPresentQueue) leaves the present queue out
           only when it is not shared (`!mPresentQueueShared`).
  BLOCK    LLVKFrameGen::initDirect blocks for the queue only through llfg::presentQueueBlock( (none: no usable
           queue at all); Reflex stays off on a shared queue (initReflex: `else if (dev.presentQueueShared())
           why = ...` before the queue is marked out of band -- the renderer's queue is not).
  CHAIN    VkSwapchain builds no frame generation chain a shared queue's lock could be held under: fg is 0 and
           mFgQueueHeld set where llfg::sharedQueueHeld( says so; the render loop's context reads it
           (fc.queueHeld = ...frameGenQueueHeld()) and asks for no such chain (chainGenerated(... && !...queueHeld)).
  GATE     VkSwapchain::presentBatch on a shared queue (llfg::gateOnCpu() submits no gate: the present thread
           waits the gate itself (vkWaitSemaphores) for at most llfg::kGateWaitBoundMs; the gate's
           vkQueueSubmit2 is the other branch only.
  SETTING  settings.xml has RenderVKFrameGenSharedQueue (Boolean, default 0) and LLVulkanContext hands it to the
           device before it is created (setFrameGenSharedQueue(gSavedSettings.getBOOL("RenderVKFrameGenSharedQueue"))).
"""
import os
import re

NAME = "framegen-queue"
STAGE = "source"
DESCRIPTION = "frame generation on one graphics queue: every queue call under the device's queue lock"
GUARDS = ("Two threads inside the driver on one queue (the renderer's submit and the present thread's submit or "
          "present: undefined behaviour, a crash or a lost device on the GPUs with one graphics queue), a viewer "
          "frozen behind a present that holds the queue's lock, or frame generation refused on those GPUs.")

ROOT = "indra/newview"
VK = ROOT + "/vkgfx"
COMMON = VK + "/vk_common.h"
DEVICE = VK + "/vk_device.cpp"
SWAP = VK + "/vk_swapchain.cpp"
FG = VK + "/llvkframegen.cpp"
CTX = VK + "/llvulkancontext.cpp"
REPLAY = VK + "/llcleanreplayvk.cpp"
REPLAY_USER = ROOT + "/llinstantreplay.cpp"
SETTINGS = ROOT + "/app_settings/settings.xml"
REVIEWED = (VK + "/vk_compat.h", VK + "/vk_compat.cpp", REPLAY)
HOOKED = ("vkQueueSubmit", "vkQueueSubmit2", "vkQueueWaitIdle")
SKIP_DIRS = ("tests", "third_party")


def _mask(text, blank_strings=True):
    """`text` without comments and (blank_strings) with every string's contents blanked: same length."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == '"' or (c == "'" and i + 2 < n and (text[i + 2] == "'" or text[i + 1] == "\\")):
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + " " * (j - i - 2) + c if blank_strings and j - i >= 2 else text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _read(ctx, rel):
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def _sources(ctx):
    """The product's C++ files: {relative path: text}."""
    files = {}
    for base, dirs, names in os.walk(ctx.path(ROOT)):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in names:
            if name.endswith((".cpp", ".h", ".inl")):
                rel = os.path.relpath(os.path.join(base, name), ctx.path(".")).replace(os.sep, "/")
                files[rel] = _read(ctx, rel)
    return files


def _body(code, signature):
    """The body of the function whose definition starts with `signature` (a regex); "" when there is none."""
    m = re.search(signature + r"[^;{]*\{", code)
    if not m:
        return ""
    depth, i = 1, m.end()
    while i < len(code) and depth:
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    return code[m.end():i]


def _first_arg(code, open_paren):
    """The first argument of the call whose `(` is at open_paren, without white space."""
    depth, i = 0, open_paren + 1
    while i < len(code):
        ch = code[i]
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            if depth == 0:
                break
            depth -= 1
        elif ch == "," and depth == 0:
            break
        i += 1
    return re.sub(r"\s+", "", code[open_paren + 1:i])


LOCK_DECL = re.compile(r"\b(?:VkDeviceContext::)?QueueLock\s+\w+\s*\(")


def _locked(code, call, queue):
    """A QueueLock for `queue` is declared before `call` in a scope that still holds there."""
    for m in LOCK_DECL.finditer(code, 0, call):
        if _first_arg(code, m.end() - 1) != queue:
            continue
        depth = 0
        for ch in code[m.end():call]:
            depth += {"{": 1, "}": -1}.get(ch, 0)
            if depth < 0:
                break
        if depth >= 0:
            return True
    return False


def _line(code, pos):
    return code.count("\n", 0, pos) + 1


def _reaches_common(rel, files, by_name, seen=None):
    seen = set() if seen is None else seen
    if rel in seen:
        return False
    seen.add(rel)
    if rel == COMMON:
        return True
    for inc in re.findall(r'^\s*#\s*include\s*"([^"]+)"', _mask(files.get(rel, ""), blank_strings=False), re.M):
        for target in by_name.get(os.path.basename(inc), ()):
            if _reaches_common(target, files, by_name, seen):
                return True
    return False


def source_rules(ctx):
    bad = []
    files = _sources(ctx)
    code = {rel: _mask(text) for rel, text in files.items()}
    by_name = {}
    for rel in files:
        by_name.setdefault(os.path.basename(rel), []).append(rel)

    # HOOK
    common = code.get(COMMON, "")
    for fn in HOOKED:
        if not re.search(r"#\s*define\s+%s\s*\([^)]*\)\s*vkq%s\s*\(" % (fn, fn[2:]), common):
            bad.append("HOOK: %s must define %s(...) as vkq%s(...)" % (COMMON, fn, fn[2:]))
    hooked_call = re.compile(r"\b(?:%s)\s*\(" % "|".join(HOOKED))
    for rel in sorted(files):
        c = code[rel]
        if rel != DEVICE and re.search(r"#\s*undef\s+(?:%s)\b" % "|".join(HOOKED), c):
            bad.append("HOOK: %s takes a queue call's hook off (#undef): only %s may" % (rel, DEVICE))
        if re.search(r"#\s*define\s+VK_NO_ASYNC_BARRIER_HOOK\b", c):
            bad.append("HOOK: %s defines VK_NO_ASYNC_BARRIER_HOOK: its queue calls would miss the wrappers" % rel)
        if rel not in (COMMON, VK + "/vk_compat.h") and hooked_call.search(c) and not _reaches_common(rel, files, by_name):
            bad.append("HOOK: %s calls a queue function and does not include vk_common.h (directly or through "
                       "its headers): the call would be the driver's, outside the queue lock" % rel)

    # LOCK
    dev = code.get(DEVICE, "")
    undef = [m.end() for m in re.finditer(r"#\s*undef\s+(?:%s)\b" % "|".join(HOOKED), dev)]
    if len(undef) != len(HOOKED):
        bad.append("LOCK: %s must #undef vkQueueSubmit, vkQueueSubmit2 and vkQueueWaitIdle once each, before its "
                   "wrappers" % DEVICE)
    start = max(undef) if undef else len(dev)
    raw = re.compile(r"\b(vkQueueSubmit|vkQueueSubmit2|gVkCompat\.QueueSubmit2|vkQueueWaitIdle)\s*\(")
    seen = set()
    for m in raw.finditer(dev, start):
        seen.add(m.group(1))
        queue = _first_arg(dev, m.end() - 1)
        if not _locked(dev, m.start(), queue):
            bad.append("LOCK: %s:%d %s(%s, ...) is not inside a VkDeviceContext::QueueLock for that queue"
                       % (DEVICE, _line(dev, m.start()), m.group(1), queue))
    for need in ("vkQueueSubmit", "gVkCompat.QueueSubmit2", "vkQueueWaitIdle"):
        if need not in seen:
            bad.append("LOCK: %s has no %s( after the #undefs (the wrapper is gone?)" % (DEVICE, need))
    if raw.search(dev, 0, start) and re.search(r"\bgVkCompat\.QueueSubmit2\s*\(", dev[:start]):
        bad.append("LOCK: %s calls gVkCompat.QueueSubmit2 before the wrappers" % DEVICE)
    presents = 0
    for rel in sorted(files):
        c = code[rel]
        if rel != DEVICE and re.search(r"\.\s*QueueSubmit2\s*\(", c):
            bad.append("LOCK: %s calls the device's QueueSubmit2 entry itself: only vkqQueueSubmit2 may" % rel)
        for m in re.finditer(r"\bvkQueuePresentKHR\s*\(", c):
            presents += 1
            queue = _first_arg(c, m.end() - 1)
            if not _locked(c, m.start(), queue):
                bad.append("LOCK: %s:%d vkQueuePresentKHR(%s, ...) is not inside a VkDeviceContext::QueueLock for "
                           "that queue" % (rel, _line(c, m.start()), queue))
    if presents < 2:
        bad.append("LOCK: the product's vkQueuePresentKHR calls were not found (%d; the frame's and the present "
                   "thread's are expected)" % presents)

    # OWN
    by_pointer = re.compile(r"\bPFN_vkQueue(?:Submit2?|PresentKHR|WaitIdle)\b|"
                            r"ProcAddr\s*\([^;]*\"vkQueue(?:Submit|Submit2|Submit2KHR|PresentKHR|WaitIdle)\"")
    for rel in sorted(files):
        if rel not in REVIEWED and by_pointer.search(_mask(files[rel], blank_strings=False)):
            bad.append("OWN: %s takes a queue entry point by name; it is not in the reviewed list (%s) -- call "
                       "vkQueueSubmit2 / vkQueueWaitIdle (the wrappers) instead" % (rel, ", ".join(REVIEWED)))
    replay = code.get(REPLAY, "")
    fn = _body(replay, r"VkResult\s+fnSubmit2\s*\(")
    if not re.search(r"if\s*\(\s*g_hookSubmit2\s*\)\s*return\s+g_hookSubmit2\s*\(\s*q\s*,", fn) \
            or len(re.findall(r"\bg_submit2\s*\(", replay)) != 1 or not re.search(r"\bg_submit2\s*\(", fn) \
            or not re.search(r"\bg_hookSubmit2\s*=\s*device\.submit2\s*;", replay):
        bad.append("OWN: %s fnSubmit2 must call the device's submit2 hook when there is one (g_hookSubmit2 = "
                   "device.submit2; `if (g_hookSubmit2) return g_hookSubmit2(q, ...)`), its own pointer only "
                   "there and only otherwise" % REPLAY)
    if not re.search(r"\.submit2\s*=\s*\[\s*\]\s*\([^)]*\)[^;]*\{\s*return\s+vkQueueSubmit2\s*\(", code.get(REPLAY_USER, "")):
        bad.append("OWN: %s must fill the replay encoder's submit2 hook with a function that calls vkQueueSubmit2 "
                   "(the wrapper)" % REPLAY_USER)

    # DEVICE
    create = _body(dev, r"bool\s+VkDeviceContext::createLogicalDevice\s*\(")
    if not re.search(r"llfg::presentQueueFor\s*\(", create) or len(re.findall(r"mPresentQueueShared\s*=\s*true\b", dev)) != 1 \
            or not re.search(r"sSharedQueue\.store\s*\(\s*mPresentQueueShared\s*\?\s*mPresentQueue\s*:", create):
        bad.append("DEVICE: %s createLogicalDevice must decide the present queue with llfg::presentQueueFor( and "
                   "publish a shared one (sSharedQueue.store(mPresentQueueShared ? mPresentQueue : ...))" % DEVICE)
    ctor = _body(dev, r"VkDeviceContext::QueueLock::QueueLock\s*\(")
    if not re.search(r"if\s*\(\s*!\s*q\s*\|\|\s*q\s*!=\s*sSharedQueue\.load\s*\([^)]*\)\s*\|\|\s*tQueueLockHeld\s*\)\s*return\s*;", ctor) \
            or not re.search(r"sSharedQueueMutex\.try_lock_for\s*\(", ctor) or not re.search(r"mHeld\s*=\s*true\s*;", ctor) \
            or not re.search(r"sSharedQueueMutex\.unlock\s*\(", _body(dev, r"VkDeviceContext::QueueLock::~QueueLock\s*\(")):
        bad.append("DEVICE: %s QueueLock must take sSharedQueueMutex exactly when its queue is the shared one and "
                   "this thread does not hold it yet (if (!q || q != sSharedQueue.load() || tQueueLockHeld) return;), "
                   "and give it back in its destructor" % DEVICE)
    # (it judges nothing: a wait for the lock ends without it only on a device lost meanwhile, and then no driver
    # call is made -- by the wrappers and by both presents)
    # (final review) ... or on a thread left behind (vkThreadAbandoned: the call it sits in may never return, with the
    # lock held -- the quit's device waits would hang on it)
    if "vkDeviceLostRaise" in ctor or not re.search(r"if\s*\(\s*vkDeviceIsLost\s*\(\s*\)\s*\|\|\s*vkThreadAbandoned\s*\(\s*\)\s*\)\s*\{\s*mGaveUp\s*=\s*true\s*;\s*return\s*;", ctor) \
            or len(re.findall(r"\bmGaveUp\s*=\s*true\b", dev)) != 1 \
            or len(re.findall(r"if\s*\(\s*lock\.gaveUp\s*\(\s*\)\s*\)\s*return\s+VK_ERROR_DEVICE_LOST\s*;", dev[start:])) != 3 \
            or len(re.findall(r"if\s*\(\s*lock\.gaveUp\s*\(\s*\)\s*\)\s*return\s+false\s*;", code.get(SWAP, ""))) != 2:
        bad.append("DEVICE: %s QueueLock raises nothing and gives up only on a lost device or a thread left behind (if (vkDeviceIsLost() || vkThreadAbandoned()) "
                   "{ mGaveUp = true; return; }); each wrapper then returns VK_ERROR_DEVICE_LOST and each present "
                   "returns false before its driver call (if (lock.gaveUp()) return ...)" % DEVICE)
    wait = _body(dev, r"VkResult\s+VkDeviceContext::waitIdleExceptPresentQueue\s*\(")
    if not re.search(r"\(\s*q\s*==\s*mPresentQueue\s*&&\s*!\s*mPresentQueueShared\s*\)", wait):
        bad.append("DEVICE: %s waitIdleExceptPresentQueue must leave the present queue out only when it is not "
                   "shared ((q == mPresentQueue && !mPresentQueueShared)): a shared queue carries the renderer's "
                   "work too" % DEVICE)

    # BLOCK
    fg = code.get(FG, "")
    init = _body(fg, r"void\s+LLVKFrameGen::initDirect\s*\(")
    if not re.search(r"llfg::presentQueueBlock\s*\(\s*llfg::presentQueueOf\s*\(\s*dev\.presentQueue\s*\(\s*\)\s*!=\s*VK_NULL_HANDLE\s*,"
                     r"\s*dev\.presentQueueShared\s*\(\s*\)\s*\)\s*\)", init) or re.search(r"!\s*dev\.presentQueue\s*\(", init) \
            or "NoPresentQueue" in fg:
        bad.append("BLOCK: %s initDirect must block for the queue only through llfg::presentQueueBlock(llfg::"
                   "presentQueueOf(dev.presentQueue() != VK_NULL_HANDLE, dev.presentQueueShared())), and name "
                   "Block::NoPresentQueue nowhere itself (a missing clock is Block::Unsupported)" % FG)
    reflex = _body(fg, r"void\s+LLVKFrameGen::initReflex\s*\(")
    shared = re.search(r"else\s+if\s*\(\s*dev\.presentQueueShared\s*\(\s*\)\s*\)\s*why\s*=", reflex)
    marked = re.search(r"\boutOfBand\s*\(\s*dev\.presentQueue\s*\(", reflex)
    if not shared or not marked or shared.start() > marked.start():
        bad.append("BLOCK: %s initReflex must leave Reflex off on a shared queue (else if (dev.presentQueueShared()) "
                   "why = ...) before it marks the present queue out of band" % FG)

    # CHAIN
    swap = code.get(SWAP, "")
    make = _body(swap, r"bool\s+VkSwapchain::createSwapchainObjects\s*\(")
    if not re.search(r"mFgQueueHeld\s*=\s*llfg::sharedQueueHeld\s*\(", make) \
            or not re.search(r"\bfg\s*=\s*mFgQueueHeld\s*\?\s*0u?\s*:", make) \
            or len(re.findall(r"\bmFgQueueHeld\s*=[^=]", swap)) != 1:
        bad.append("CHAIN: %s createSwapchainObjects must set mFgQueueHeld = llfg::sharedQueueHeld(...) (there only) "
                   "and build no frame generation chain then (fg = mFgQueueHeld ? 0 : ...)" % SWAP)
    cx = code.get(CTX, "")
    if not re.search(r"fc\.queueHeld\s*=\s*mImpl->mSwapchain\.frameGenQueueHeld\s*\(\s*\)\s*;", cx) \
            or not re.search(r"chainGenerated\s*\(\s*llfg::outputGenerates\s*\(\s*(\w+)\.output\s*\)\s*&&\s*!\s*\1\.queueHeld\s*\)", cx):
        bad.append("CHAIN: %s must set the context's queueHeld from mSwapchain.frameGenQueueHeld() and ask for no "
                   "chain then (chainGenerated(llfg::outputGenerates(c.output) && !c.queueHeld))" % CTX)

    # GATE
    batch = _body(swap, r"bool\s+VkSwapchain::presentBatch\s*\(")
    gate = re.search(r"if\s*\(\s*gated\s*&&\s*llfg::gateOnCpu\s*\(", batch)
    submit = re.search(r"\bvkQueueSubmit2\s*\(", batch)
    wait = re.search(r"\bvkWaitSemaphores\s*\([^;]*llfg::kGateWaitBoundMs[^;]*;", batch)
    els = re.search(r"\belse\b", batch[gate.end():]) if gate else None
    if not gate or not submit or not wait or not els or not (gate.end() < wait.start() < gate.end() + els.start() < submit.start()) \
            or len(re.findall(r"\bvkQueueSubmit2\s*\(", batch)) != 1 or "UINT64_MAX" in batch:
        bad.append("GATE: %s presentBatch must wait a gate itself on a shared queue (if (gated && llfg::gateOnCpu(...)) "
                   "vkWaitSemaphores(..., llfg::kGateWaitBoundMs ...)) and submit the gate (its one vkQueueSubmit2) "
                   "only in the else branch" % SWAP)

    # SETTING
    sm = re.search(r"<key>RenderVKFrameGenSharedQueue</key>\s*<map>(.*?)</map>", _read(ctx, SETTINGS), re.S)
    if not sm or not re.search(r"<key>Type</key>\s*<string>Boolean</string>", sm.group(1)) \
            or not re.search(r"<key>Value</key>\s*<integer>0</integer>", sm.group(1)):
        bad.append("SETTING: %s needs RenderVKFrameGenSharedQueue (Boolean, default 0)" % SETTINGS)
    raw_cx = _mask(files.get(CTX, ""), blank_strings=False)
    m = re.search(r"mDevice\.setFrameGenSharedQueue\s*\(\s*gSavedSettings\.getBOOL\s*\(\s*\"RenderVKFrameGenSharedQueue\"\s*\)\s*\)", raw_cx)
    init_at = re.search(r"mDevice\.initForPresent\s*\(", raw_cx)
    if not m:
        # #1550: the startup and the GPU-loss recovery device share one helper, applyDeviceRequests(d), called on
        # mImpl->mDevice before initForPresent: the setting is handed there.
        helper = re.search(r"void\s+LLVulkanContext::applyDeviceRequests\s*\(\s*VkDeviceContext\s*&\s*d\s*\)\s*\{(.*?)\n\}",
                           raw_cx, re.S)
        if helper and re.search(r"\bd\.setFrameGenSharedQueue\s*\(\s*gSavedSettings\.getBOOL\s*\(\s*"
                                r"\"RenderVKFrameGenSharedQueue\"\s*\)\s*\)", helper.group(1)):
            m = re.search(r"applyDeviceRequests\s*\(\s*mImpl->mDevice\s*\)", raw_cx)
    if not m or not init_at or m.start() > init_at.start():
        bad.append("SETTING: %s must hand RenderVKFrameGenSharedQueue to the device before it is created "
                   "(mDevice.setFrameGenSharedQueue(gSavedSettings.getBOOL(...)) before initForPresent)" % CTX)
    return bad


def run(ctx, result):
    missing = [p for p in (COMMON, DEVICE, SWAP, FG, CTX, REPLAY, REPLAY_USER, SETTINGS) if not os.path.exists(ctx.path(p))]
    if missing:
        result.fail("missing: " + ", ".join(missing))
        result.summary = "files missing"
        return
    bad = source_rules(ctx)
    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "8 source rules hold"
