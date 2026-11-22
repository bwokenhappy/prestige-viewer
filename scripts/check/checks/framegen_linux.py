"""Source guards for frame generation on Linux (#1517 Task 11b) and for a window the compositor hides: one
function per rule, called from run(). (The other frame generation rules: framegen_direct.py, whose source
helpers this module shares.)

  LINUX    frame generation runs on Linux as on Windows: every `#if LL_WINDOWS` in llvkframegen.cpp has an
           `#elif LL_LINUX` side (the pacer's CPU clock), and initDirect, releaseGates, initReflex, reflexSleep,
           outOfBandMarker and reflexMarker run inside `#if LL_WINDOWS || LL_LINUX`, as does
           createLogicalDevice's request; llfg::Context carries `platform` (frameGenContext sets it there). The
           pacer's clock comes from llfg::pacerClock( (CLOCK_MONOTONIC, else CLOCK_MONOTONIC_RAW; the
           calibration's CPU domain is the chosen clock's), sleeps with clock_nanosleep( / TIMER_ABSTIME or a
           relative step (llfg::pacerSleepsAbsolute, llfg::pacerSleepNs) and reads the same clock
           (clock_gettime); reflexSleep never reads qpcNowNs(). vk_swapchain.cpp picks a frame generation chain's
           present mode with llfg::frameGenPresentMode( (no inline choice) and sizes it with
           llfg::swapchainImagesFor(<generated>, <its mode>) (one image more on MAILBOX / FIFO); the context logs
           the mode (llfg::presentModeText) and whether the multiplier is refresh-bound (llfg::refreshBound).
           Preferences' framegen_platform names Linux, and neither the FrameGen tooltip nor RenderVKFrameGen's
           comment names Windows without Linux.
  HIDDEN   (Task 11b fix 1; the waits themselves: framegen_direct.py's rule STALL, Task 7c fix 4) a window the
           compositor hides, whose presents it may hold (MAILBOX / FIFO: Wayland, while the window cannot be
           seen), never freezes the viewer:
           - the window's state (frameGenWindowState: SDL_WINDOW_OCCLUDED / HIDDEN / minimised through
             llfg::windowHidden() is read on Linux only (`#if LL_LINUX`), tells the presenter
             (setWindowHidden() before anything else and never drains; on Linux onWindowMinimized tells it
             (setWindowHidden(true)) and drains only on the other platforms; a frame on a frame generation
             chain is dropped while hidden (fgJob.dropped = ... mFgHidden) and the context's background is
             that state or, as before, the minimised window (mFseMinimized);
           - drainFrameGen returns whether the present thread drained (`if (!mFgPresent.drain()) return
             false;` before the pacer's drain and gatesReleased();
           - renderFrame defers the main chain's rebuild by llfg::deferRebuild( (frameGenRebuildWaits(), fed
             the hidden window and mFgPresent.held()) and when drainFrameGen() fails before
             recreateMainSwapchain(), makes the generator's feature only after a drainFrameGen() that
             succeeded, takes a slot only when waitSlot( answers SlotWait::Free; shutdown is deferred when
             drainFrameGen() fails, before its device wait, and then stops the present thread
             (mFgPresent.stop(): bounded, left behind if it does not end);
           - the presenter: drain() is one bounded try, only a look while the thread is held(), and ends on a
             lost device; mWindowHidden is a std::atomic<bool>, runJob drops a job while the window is hidden,
             presentJob reaches acquireOne( and presentBatch( once each, each after a check of the hidden
             window.
  X11      (#1536) frame generation's gates never read the window system: no code in llvkframegen.h/.cpp,
           llvkframegenpresent.h/.cpp, llvkframegenpolicy.h or renderFrame names x11 / wayland, the video
           driver or vkdisplay::Platform, and blockReason's only output gate is llfg::Context::output -- X11 runs
           in SDR exactly as Wayland does (it was excluded only through "needs HDR"). (#1544: queueHeld is the
           surface's own answer -- no IMMEDIATE mode offered to a device with one graphics queue -- whatever the
           window system.)
"""
import re

from .framegen_direct import (CTX, DEV, FG, FG_H, PT, PT_H, SETTINGS, SWAP, POLICY_H, PLATFORM_GATE, PLATFORM_IF,
                              PREFS_XUI, _code, _function, _mask, _raw, _windows_spans)

NAME = "framegen-linux"
STAGE = "source"
DESCRIPTION = "frame generation on Linux: gates, pacer clock, present modes, a window the compositor hides"
GUARDS = ("Frame generation gated to Windows only, a Linux pacer whose CPU clock is not the calibrated one, a frame "
          "generation chain's present mode chosen inline or without MAILBOX's extra image, or a viewer frozen "
          "while its window is hidden: a drain at hide, a swapchain rebuild or shutdown under a present the "
          "compositor holds, an image taken or presented while hidden.")

COMMON_H = "indra/newview/vkgfx/vk_common.h"
LINUX_IF = r"^[ \t]*#[ \t]*if[ \t]+LL_LINUX[ \t]*$"


def _conditionals(text):
    """Every preprocessor conditional of text: a list of its branch lines (#if... / #elif / #else), outermost
    and nested alike, in order of their #if."""
    out, stack = [], []
    for m in re.finditer(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b(.*)$", text, flags=re.M):
        kw = m.group(1)
        if kw.startswith("if"):
            stack.append([(kw + m.group(2)).strip()])
            out.append(stack[-1])
        elif kw in ("elif", "else") and stack:
            stack[-1].append((kw + m.group(2)).strip())
        elif kw == "endif" and stack:
            stack.pop()
    return out


def _rule_linux(ctx, bad):
    """(Task 11b) Frame generation runs on Linux as on Windows."""
    raw_fg = _raw(ctx, FG) or ""
    c = _code(raw_fg, blank_strings=True)
    # No frame generation gate is Windows-only: each `#if LL_WINDOWS` of llvkframegen.cpp is one side of the
    # pacer's CPU clock (the performance counter and its timer), with an `#elif LL_LINUX` side.
    for branches in _conditionals(c):
        head = re.sub(r"\s+", " ", branches[0])
        if re.fullmatch(r"if (?:defined ?\( ?LL_WINDOWS ?\)|LL_WINDOWS)", head) and \
                not any(re.fullmatch(r"elif LL_LINUX", re.sub(r"\s+", " ", b)) for b in branches[1:]):
            bad.append("LINUX: %s has an `#if LL_WINDOWS` without an `#elif LL_LINUX` side (frame generation "
                       "runs on Linux too)" % FG)
    for sig in ("void LLVKFrameGen::initDirect(", "void LLVKFrameGen::releaseGates(", "void LLVKFrameGen::initReflex(",
                "void LLVKFrameGen::reflexSleep(", "void LLVKFrameGen::outOfBandMarker(", "void LLVKFrameGen::reflexMarker("):
        body = _function(c, sig) or ""
        if not re.search(PLATFORM_IF, body, flags=re.M):
            bad.append("LINUX: %s must run inside %s (it is frame generation's code on both platforms)"
                       % (sig.split("::")[1].rstrip("("), PLATFORM_GATE))
    # The pacer's clock on Linux: chosen by the rule; the calibrated domain and the CPU "now" are one clock.
    init = _function(c, "void LLVKFrameGen::initDirect(") or ""
    if "llfg::pacerClock(" not in init:
        bad.append("LINUX: initDirect must choose the pacer's clock with llfg::pacerClock(")
    for word in ("VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR", "VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR"):
        if word not in init:
            bad.append("LINUX: initDirect never considers %s" % word)
    if re.search(r"timeDomain\s*=\s*VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR", init):
        bad.append("LINUX: the calibration's CPU domain must be the chosen clock's (not always the performance counter)")
    for word, why in (("clock_nanosleep(", "the absolute sleep on CLOCK_MONOTONIC"), ("TIMER_ABSTIME", "an absolute deadline"),
                      ("CLOCK_MONOTONIC_RAW", "the raw clock's fallback"), ("clock_gettime(", "the CPU now on Linux"),
                      ("llfg::pacerSleepNs(", "the sleep steps"), ("llfg::pacerSleepsAbsolute(", "the sleep's kind"),
                      ("llfg::timespecNs(", "a timespec in nanoseconds")):
        if word not in c:
            bad.append("LINUX: %s lacks %s (%s)" % (FG, word.rstrip("("), why))
    if "qpcNowNs(" in (_function(c, "void LLVKFrameGen::reflexSleep(") or ""):
        bad.append("LINUX: reflexSleep reads qpcNowNs() (Windows only): use the pacer's CPU clock")
    # The context flag is the platform's, true on Windows and Linux.
    pol = _code(_raw(ctx, POLICY_H) or "")
    cm = re.search(r"struct\s+Context\s*\{(.*?)\};", pol, flags=re.S)
    if not cm or re.search(r"\bbool\s+windows\b", cm.group(1)) or not re.search(r"\bbool\s+platform\s*=", cm.group(1)):
        bad.append("LINUX: llfg::Context must carry `platform` (Windows or Linux), not `windows`")
    for name in ("frameGenPresentMode(", "pacerClock(", "windowHidden(", "swapchainImagesFor(",
                 "refreshBound(", "slotWaitBoundMs(", "deferRebuild("):
        if not re.search(r"\binline\b[^;{]*\b%s" % re.escape(name), pol):
            bad.append("LINUX: llvkframegenpolicy.h must define the pure rule llfg::%s)" % name)
    cx = _code(_raw(ctx, CTX) or "", blank_strings=True)
    lam = cx[cx.find("auto frameGenContext"):]
    lam = lam[:lam.find("};") + 2] if "};" in lam else ""
    if not re.search(PLATFORM_IF + r"\s*fc\.platform\s*=\s*true\s*;", lam, flags=re.M):
        bad.append("LINUX: frameGenContext must set fc.platform = true inside %s" % PLATFORM_GATE)
    if "fc.windows" in cx:
        bad.append("LINUX: %s still sets fc.windows" % CTX)
    # The device asks for frame generation's queue and extensions on Linux too (the DEVICE rule's gate).
    dev = _code(_raw(ctx, DEV) or "", blank_strings=True)
    body = _function(dev, "bool VkDeviceContext::createLogicalDevice(") or ""
    if not re.search(PLATFORM_IF, body, flags=re.M):
        bad.append("LINUX: createLogicalDevice must request frame generation's queue and extensions inside %s" % PLATFORM_GATE)
    # The present mode: one rule, IMMEDIATE > MAILBOX > FIFO, logged once per chain; the chain's images by the
    # mode (one more on MAILBOX / FIFO).
    sw = _code(_raw(ctx, SWAP) or "", blank_strings=True)
    if "llfg::frameGenPresentMode(" not in sw:
        bad.append("LINUX: %s must choose a frame generation chain's present mode with llfg::frameGenPresentMode(" % SWAP)
    if re.search(r"sNoVsyncMode\s*==\s*1\s*\|\|\s*fg\b", sw):
        bad.append("LINUX: %s still decides a frame generation chain's present mode inline" % SWAP)
    if not re.search(r"llfg::swapchainImagesFor\s*\(\s*fg\s*,\s*fgPresentMode\s*\(\s*\)\s*\)", sw):
        bad.append("LINUX: %s must size a frame generation chain by its present mode "
                   "(llfg::swapchainImagesFor(fg, fgPresentMode()): one image more on MAILBOX / FIFO)" % SWAP)
    log = _function(cx, "void logFrameGenChain(") or ""
    if not re.search(r"llfg::presentModeText\s*\(", log) or not re.search(r"llfg::refreshBound\s*\(", log):
        bad.append("LINUX: logFrameGenChain must log a frame generation chain's present mode (llfg::presentModeText) "
                   "and whether its multiplier is refresh-bound (llfg::refreshBound)")
    # Preferences and the setting: Linux named, nothing Windows-only.
    xml = _raw(ctx, PREFS_XUI) or ""
    m = re.search(r'name="framegen_platform">\s*([^<]*)<', xml)
    if not m or "Linux" not in m.group(1):
        bad.append("LINUX: the framegen_platform string must name Linux")
    m = re.search(r'name="FrameGen"[^>]*?tool_tip="([^"]*)"', xml, flags=re.S) or \
        re.search(r'tool_tip="([^"]*)"[^>]*?name="FrameGen"', xml, flags=re.S)
    if not m or ("Windows" in m.group(1) and "Linux" not in m.group(1)):
        bad.append("LINUX: the FrameGen combo's tooltip names Windows without Linux")
    st = _raw(ctx, SETTINGS) or ""
    m = re.search(r"<key>RenderVKFrameGen</key>\s*<map>\s*<key>Comment</key>\s*<string>([^<]*)</string>", st)
    if not m or ("Windows" in m.group(1) and "Linux" not in m.group(1)):
        bad.append("LINUX: RenderVKFrameGen's comment names Windows without Linux")


def _branches(text, opener):
    """(if-branch, rest) of the first `opener` conditional in text: the text up to its #else / #elif / #endif,
    and from there to its #endif. (None, None) without one."""
    spans = _windows_spans(text, opener)
    if not spans:
        return None, None
    a, b = spans[0]
    end = text.find("#endif", b)
    return text[a:b], text[b:end if end >= 0 else len(text)]


def _statement_before(text, pos):
    """The text of the statement (or condition) that holds pos: from the last `;`, `{` or `}` before it."""
    cut = max(text.rfind(";", 0, pos), text.rfind("{", 0, pos), text.rfind("}", 0, pos))
    return text[cut + 1:pos]


def _rule_hidden(ctx, bad):
    """(Task 11b fix 1) The main thread never waits without a bound for a present the compositor holds."""
    cx = _mask(_raw(ctx, CTX) or "", blank_strings=True)
    # The window's state: Linux only, the presenter told first, never a drain.
    ws = _function(cx, "void frameGenWindowState(") or ""
    linux, _rest = _branches(ws, LINUX_IF)
    if linux is None:
        bad.append("HIDDEN: frameGenWindowState must read the window's state on Linux only (`#if LL_LINUX`; Windows "
                   "already skips a minimised or hidden window)")
    else:
        for word in ("SDL_WINDOW_OCCLUDED", "SDL_WINDOW_HIDDEN", "llfg::windowHidden("):
            if word not in linux:
                bad.append("HIDDEN: frameGenWindowState lacks %s" % word.rstrip("("))
        tell, state = linux.find("setWindowHidden("), linux.find("mFgHidden =")
        if tell < 0 or (state >= 0 and state < tell) or re.search(r"\bLL_(?:INFOS|WARNS)\b", linux[:tell] if tell >= 0 else ""):
            bad.append("HIDDEN: frameGenWindowState must tell the presenter (setWindowHidden() before anything else")
    if re.search(r"drain", ws, flags=re.I):
        bad.append("HIDDEN: frameGenWindowState must never drain (the drain at hide waits for the presents the "
                   "compositor holds)")
    mini = _function(cx, "void LLVulkanContext::onWindowMinimized(") or ""
    linux, rest = _branches(mini, LINUX_IF)
    if linux is None or "drainFrameGen(" in linux or not re.search(r"setWindowHidden\s*\(\s*true\s*\)", linux) \
            or "drainFrameGen(" not in (rest or ""):
        bad.append("HIDDEN: onWindowMinimized must tell the presenter on Linux (setWindowHidden(true), no drain) and "
                   "drain only on the other platforms (`#if LL_LINUX ... #else drainFrameGen()`)")
    if not re.search(r"fgJob\.dropped\s*=[^;]*mFgHidden", cx):
        bad.append("HIDDEN: a frame on a frame generation chain must be dropped (nothing presented) while the window "
                   "is hidden (fgJob.dropped = ... mFgHidden)")
    lam = cx[cx.find("auto frameGenContext"):]
    lam = lam[:lam.find("};") + 2] if "};" in lam else ""
    bg = re.search(r"fc\.background\s*=([^;]*);", lam)
    if not bg or "mFgHidden" not in bg.group(1) or "mFseMinimized" not in bg.group(1):
        bad.append("HIDDEN: frameGenContext's background must be the minimised window as before (mFseMinimized) or, "
                   "on Linux, the hidden one (mFgHidden)")
    # The drain says whether it drained; the pacer and the gates only after it did.
    dr = _function(cx, "bool drainFrameGen(") or ""
    held = re.search(r"if\s*\(\s*!\s*mFgPresent\.drain\s*\(\s*\)\s*\)\s*return\s+false\s*;", dr)
    pacer, gates = dr.find("drainPacer("), dr.find("gatesReleased(")
    if not held or pacer < held.end() or gates < held.end():
        bad.append("HIDDEN: drainFrameGen must return false when the present thread did not drain "
                   "(`if (!mFgPresent.drain()) return false;`) before the pacer's drain and gatesReleased()")
    # renderFrame: rebuilds deferred under a held present, the feature made only after a drain, a slot taken only
    # when free; shutdown deferred.
    rf = _function(cx, "bool LLVulkanContext::renderFrame(") or ""
    rw = _function(cx, "bool frameGenRebuildWaits(") or ""
    if not re.search(r"llfg::deferRebuild\s*\([^;]*mFgHidden\s*,\s*mFgPresent\.held\s*\(\s*\)\s*\)", rw, flags=re.S):
        bad.append("HIDDEN: frameGenRebuildWaits must ask llfg::deferRebuild( with the hidden window (mFgHidden) and "
                   "the held present thread (mFgPresent.held())")
    if not re.search(r"frameGenRebuildWaits\s*\(\s*\)[^;{]*\)\s*(?:\{[^}]*?)?deferSwapchainRecreate\s*=\s*true", rf):
        bad.append("HIDDEN: renderFrame must defer the main chain's rebuild while frameGenRebuildWaits()")
    first = rf.find("mImpl->recreateMainSwapchain(")
    guard = re.search(r"!\s*mImpl->drainFrameGen\s*\(\s*\)\s*\)\s*(?:\{[^}]*?)?deferSwapchainRecreate\s*=\s*true",
                      rf[:first] if first >= 0 else "")
    if first < 0 or not guard:
        bad.append("HIDDEN: renderFrame must defer the rebuild when drainFrameGen() fails, before recreateMainSwapchain()")
    # (#1544 Task 6: the feature and the inputs are made in prepareFrameGen, after ONE drain -- rule STALL)
    pg = _function(cx, "bool prepareFrameGen(") or ""
    dr_at = pg.find("drainFrameGen()")
    pf = pg.find("prepareFeature(")
    if "prepareFeature(" in rf or pf < 0 or dr_at < 0 or dr_at > pf:
        bad.append("HIDDEN: the generator's feature is made only in prepareFrameGen, after its drainFrameGen() "
                   "(never in renderFrame itself)")
    sw = re.search(r"\bconst\s+VkPresentHandoff::SlotWait\s+(\w+)\s*=[^;]*mFgPresent\.waitSlot\s*\(\s*fgSlot\s*\)", rf)
    if not sw or not re.search(r"=\s*%s\s*==\s*VkPresentHandoff::SlotWait::Free\s*;" % sw.group(1), rf[sw.end():]):
        bad.append("HIDDEN: renderFrame must take its slot only when waitSlot(fgSlot) answers VkPresentHandoff::SlotWait::Free")
    sd = _function(cx, "void LLVulkanContext::shutdown(") or ""
    g = re.search(r"if\s*\(\s*!\s*mImpl->drainFrameGen\s*\(\s*\)\s*\)", sd)
    w = sd.find("vkDeviceWaitIdle(")
    if not g or w < 0 or g.start() > w or not re.search(r"return\s*;", sd[g.end():w]) or \
            "mFgPresent.stop()" not in sd[g.end():w]:
        bad.append("HIDDEN: shutdown must be deferred (return) when drainFrameGen() fails, before its device wait, "
                   "and stop the present thread (mFgPresent.stop(): bounded, left behind if it does not end)")
    # The presenter.
    pth = _code(_raw(ctx, PT_H) or "", blank_strings=True)
    pt = _mask(_raw(ctx, PT) or "", blank_strings=True)
    if not re.search(r"std::atomic<bool>\s+mWindowHidden\b", pth):
        bad.append("HIDDEN: %s: mWindowHidden must be a std::atomic<bool> (the present thread reads it)" % PT_H)
    dr = _function(pt, "bool LLVKFrameGenPresent::drain(") or ""
    if not re.search(r"\bheld\s*\(\s*\)\s*\?\s*0u?\s*:\s*mBounds\.drainMs", dr) or "mHandoff.drain(" not in dr or \
            "vkDeviceIsLost()" not in dr:
        bad.append("HIDDEN: LLVKFrameGenPresent::drain must be one bounded try (mBounds.drainMs), only a look while the "
                   "thread is held (`held() ? 0u : mBounds.drainMs`), and end on a lost device (vkDeviceIsLost())")
    rj = _function(pt, "void LLVKFrameGenPresent::runJob(") or ""
    cond = rj[:rj.find("presentJob(")] if "presentJob(" in rj else ""
    if "hiddenNow()" not in cond:
        bad.append("HIDDEN: runJob must drop a job while the window is hidden (hiddenNow() before presentJob()")
    job = _function(pt, "void LLVKFrameGenPresent::presentJob(") or ""
    for call in ("acquireOne(", "presentBatch("):
        at = [m.start() for m in re.finditer(re.escape(call), job)]
        if len(at) != 1:
            bad.append("HIDDEN: presentJob must reach %s once (one lambda that checks the hidden window first), "
                       "found %d" % (call.rstrip("("), len(at)))
            continue
        lam_at = job.rfind("[&]", 0, at[0])
        if lam_at < 0 or "hiddenNow()" not in job[lam_at:at[0]]:
            bad.append("HIDDEN: presentJob must check the hidden window (hiddenNow()) before every %s"
                       % call.rstrip("("))


WINDOW_SYSTEM = re.compile(r"(?i)\bx11\b|wayland|SDL_GetCurrentVideoDriver|vkdisplay::Platform")


def _rule_x11(ctx, bad):
    """(#1536) No frame generation gate reads the window system."""
    for rel in (FG_H, FG, PT_H, PT, POLICY_H):
        code = _code(_raw(ctx, rel) or "")
        for m in WINDOW_SYSTEM.finditer(code):
            bad.append("X11: %s line %d names the window system (%s): frame generation's gates are the same on X11 "
                       "and Wayland" % (rel, code.count("\n", 0, m.start()) + 1, m.group(0)))
    rf = _function(_code(_raw(ctx, CTX) or ""), "bool LLVulkanContext::renderFrame(") or ""
    if not rf:
        bad.append("X11: %s has no renderFrame" % CTX)
    for m in WINDOW_SYSTEM.finditer(rf):
        bad.append("X11: renderFrame names the window system (%s)" % m.group(0))
    br = _function(_code(_raw(ctx, POLICY_H) or ""), "inline Block blockReason(") or ""
    outs = set(re.findall(r"\bc\.(\w+)", br))
    if "output" not in outs or outs - {"platform", "inputsOn", "output", "vr", "background", "mainWindow", "queueHeld"}:
        bad.append("X11: blockReason reads the Context's platform, inputsOn, output, queueHeld, vr, background and mainWindow only "
                   "(read: %s)" % ", ".join(sorted(outs)))


def run(ctx, result):
    bad = []
    _rule_linux(ctx, bad)
    _rule_hidden(ctx, bad)
    _rule_x11(ctx, bad)
    for b in bad:
        result.fail(b)
    result.summary = "%d problem(s)" % len(bad) if bad else "the Linux and hidden-window rules hold"
