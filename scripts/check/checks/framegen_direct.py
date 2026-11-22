"""Source guards for DLSS Frame Generation called directly through the DLSS add-on (#1517), the only
route: the viewer presents and paces the generated frames itself. One function per rule, called from run().

  NO-STREAMLINE  (Task 11; scoped by the final fix pass) nothing of the older route stays in frame
           generation's files -- its own (OWNED_*), the renderer files it is wired into (RENDERER_FILES) and
           the viewer files it touches (VIEWER_FILES); code anywhere else may use these words, and a
           self-test on a made-up tree (SCOPE) proves both on every run. There is no contrib/slshim, and
           none of those files (this one excepted) names streamline (not "streamlined", not the colour SlipstreamLine), slshim,
           slipstream_sl, slip_fg_set_, setProcs, noteTextInput, PauseTyping, UiBlend, TagUi or
           RenderVKFrameGenDirect (any case). LEFTOVERS: the code of the own and renderer files (comments
           stripped; not the viewer files, whose generic names are their own) uses none of that route's names (sessionActive, decideFrame, tagFrame, skipFrame,
           afterPresent, typingHold, untaggedPresent, Block::Typing, QueueSlot, FgRequest, FgQueues,
           FgProvider, setFrameGenProvider, frameGenQueues, libraryOverride, nativeFeature,
           vkqDeviceWaitIdleVia, vkSetDeviceWaitIdleProxy, mProxied, waitDeviceIdle, SurfaceProcs,
           SwapchainProcs, mSurfaceDestroy, pause/resumeAfterSwapchainChange, productionBinaries,
           mProcs, LLVKFrameGen's .direct()); llvkframegen.h declares no direct(), startup(), Paths,
           requestedMultiplier() or Fns; llvkframegenpolicy.h has no typing rule nor
           llfg::reflexCapsFrames; vk_swapchain.h no Procs.
  MAIN-ONLY  only the main window's swapchain is built for frame generation (every setFrameGen( is
           mSwapchain.setFrameGen(, none inside renderUIWindow), the real frame is handed to the present
           thread (mFgPresent.push() only in renderFrame, and llxrsession.cpp names none of setFrameGen /
           acquireOne / presentBatch / mFgPresent.
  MAIN-VIEW-ONLY  frameGenConstants( runs once, under f.frameGenView && LLVKFrameGen::instance().
           directReady(); f.frameGenView requires v == 0, mViewCount == 1, !mOffscreen, !mLiveCapNow and
           !mSdrFrame; the encode's frame generation outputs (fgFrame) need !sdrFrame and frameGenInputs(.
  MARKERS  one call per PC Latency marker: SimStart / SimEnd in llappviewer.cpp, the other four in
           llvulkancontext.cpp; RenderStart at renderFrame's top level.
  SETTING-DEFAULT  settings.xml has RenderVKFrameGen (U32, Persist 1, default 0); its comment names
           the DLSS add-on and neither a restart nor another route.
  REFLEX-PACING  where Reflex paces the frame its sleep replaces the slot wait: llappviewer.cpp calls
           LLVKFrameGen::instance().beginFrame( inside `if (fg_paced)`, before any waitFrameSlot(), and every
           waitFrameSlot() sits in that if's else chain.
  OUTPUT-CHAIN  frameGenContext's fc.output (set once) comes from the chain actually presenting: its
           encoding (mSwapchain.displayStatus().encoding) and, in SDR, its format (mSwapchain.format()),
           directly or through a local's initializer; the lambda never reads the HDR target in flight
           (mHdrEncodeTarget).
  FPS-SHOWN  the status bar's multiplier is shownPerRendered(); while generating the FPS box shows
           llfloor(fps * fg.shownPerRendered()) in getColor("FPSFrameGenTextColor") (never "%dx%d") with
           the fps_framegen tooltip; colors.xml has FPSFrameGenTextColor, panel_status_bar.xml the
           fps_framegen string and a 40 px FPSText.
  PREFS    the RenderVKFrameGen combo's items are 0, 2, 3, 4, and no frame generation text of the
           floater (the combo, FrameGen* controls, framegen_* / vsync_framegen strings) names another
           viewer (known here by hash only).

  DEVICE   vk_device.h declares presentQueue(), calibratedTimestamps(), lowLatency2(),
           presentMetering() and timestampPeriodNs(), and no presentMeteringState(). In
           vk_device.cpp the graphics family's request grows by the present queue only inside
           `#if LL_WINDOWS || LL_LINUX`, the present queue is fetched (vkGetDeviceQueue, index after the
           viewer's own), and the timing extensions (calibrated timestamps, low latency 2) and the
           optional provider's extensions are asked for only inside `#if LL_WINDOWS || LL_LINUX`. A refusal
           drops them again (a retry of its own). setOptionalExtensionProvider exists and
           llvulkancontext.cpp sets it from LLVKNeuralRecon's fgDeviceExtensions. No line of
           vk_device.h / vk_device.cpp contains SPIKE.
  SWAPCHAIN vk_swapchain.h declares setFrameGen, frameGenImages, frameGenMax, acquireOne, presentBatch,
           gatedPresentsPending and gatesReleased; neither vk_swapchain file contains "spike" (any case).
           Inside presentBatch every present and every vkQueueSubmit2 (the gates) goes to
           ctx.presentQueue(), none to ctx.queue(), and the main thread's frame slot (mFrame) is never
           moved (the present thread calls it). acquireOne fetches one image with the caller's semaphore
           and never touches mFrame. acquire() and present() refuse a chain built for frame generation
           (mFgImages) before they reach the driver, and present() uses ctx.queue().
           VkSetPresentConfigNV appears only inside presentBatch. recreate and destroy check that no gated
           present is pending (the owner drains the pacer first, else a quit hangs on a gate).
  DIRECT   llvkframegen.h declares LLVKFrameGen's direct members (initDirect, shutdownDirect,
           directReady, maxGenerated, decideDirect, evaluate, drainPacer, gate, frameDone,
           nextFrameDoneValue, pacer, meteringResult, meteringReset, generatedDone,
           nextGeneratedDoneValue, prepareFeature, driverPacing, timestampQuery, batchPresented,
           takeConstants). No "spike" (any case) in llvkneuralrecon.*, llvkframegen.* or
           llcleandlssrrvk.cpp; no spikeWanted / struct Spike / FG SPIKE in llvulkancontext.cpp and
           nothing under indra/newview reads RenderVKFrameGenSpike*. LLVulkanContext::init calls
           initDirect( after LLVKNeuralRecon's initDevice; shutdown calls shutdownDirect( before
           LLVKNeuralRecon shuts down, before the main swapchain and the device go. initDirect on
           other platforms (the `#else` of `#if LL_WINDOWS || LL_LINUX`) reports Block::Platform and
           touches nothing else. initDirect blocks
           with NoAddon only on !loaded() (never on available(): a loaded add-on that cannot run is
           Unsupported), then OldAddon, then Unsupported on !available(), in that order (#1544: decided by the
           DLSS generator's own init, LLVKFrameGenDlss::init; initDirect shows its block, detail and log
           text). Only initDirect, prepareFeature, evaluate and shutdownDirect reach the generator from
           llvkframegen.cpp (mGen->init / prepare / release / evaluate: one seam), and the file names no
           LLVKNeuralRecon; evaluate (the present thread) only records -- no device wait, create,
           release or pacer drain in it. slipstream_ngx.h is included by no file under indra/ but llvkneuralrecon.cpp.
           llvkframegen.cpp has no Sleep( call: the pacer waits on a high-resolution waitable timer,
           then spins.
  PRESENT  (Task 7c fix round 1) renderFrame acts on mFgPresent.needsRecreate() only on a frame generation
           chain, needsRecreate() answers `mReady && ...`, releaseChain() and stop() forget the request.
           Frame generation's images are admitted against frameGenBudget() (liveDeviceBudget with
           vkLargeRequestMargin) in mFgPresent.prepare( and prepareInputs(, never mHdrBudget; the display
           frame holds none of them; prepare admits them as one set (llfg::presentSlotsFor). A refused set
           is imagesRefused( (never directFailed() and re-armed (rearmImages() on every rebuild but the one
           the refusal asked for; chainGenerated honours it (imagesRefusedNow()). renderFrame times its
           waitSlot( (noteSlotWait() and the 5 s line shows slot waits and present calls (avg / worst). The
           job carries the frame's Reflex id (reflexFrame() before push(); presentJob sends OobRenderStart
           before its first acquireOne( and OobRenderEnd after its generate submit(, and each presentBatch(
           sits between OobPresentStart and OobPresentEnd. DIRECT also checks that evaluate stops at a sticky
           mEvalStopped before the add-on and meteringResult counts with fetch_add.
  FRAME    The render loop and frame generation's present thread (Task 7c, the reference
           implementation's layout). No "spike" (any case) in llvulkancontext.cpp, the present thread's
           files (llvkframegenpresent.*), vk_display_encode.*, vk_display_frame.* or
           clean_display_output.frag, and no RenderVKFrameGenSpike in settings.xml.
           The present thread owns a frame generation chain's swapchain and the present queue:
           llvulkancontext.cpp, llcleanrenderervk.* never name presentQueue(), and llvulkancontext.cpp calls
           no acquireOne / acquireExtra / presentBatch / evaluate. renderFrame decides `const bool fgChain
           = ...frameGenImages()...`; its mSwapchain.acquire( and mSwapchain.present( sit under !fgChain
           (an `if (!fgChain`, the else of `if (fgChain`, or `fgChain ? ... : <here>`); on a frame
           generation chain it waits for its backbuffer slot (waitSlot( && beginFrameWithoutImage(), hands
           the frame over once (mFgPresent.push(, inside `if (fgChain`, after the frame's submits) and
           ends its frame slot (endFrameWithoutPresent().
           The present thread (llvkframegenpresent.cpp): presentJob fetches each generated frame's image
           inside its loop (acquireOne() and the real frame's only after the first presentBatch( (a chain
           of generated + 1 images has none for it before); it presents twice (the generated frames, then
           the real one); pacer().push( sits between the first and the last presentBatch( (no gated present
           before its gate has a job) and a refused push opens the gates (releaseGates(); no return between
           the first acquireOne( and the last presentBatch(; every vkQueueSubmit2 goes to the present queue;
           its submits wait frameDone(); evaluate( is fed the job's slot and its own inputs (<job>.eval, or a
           local copy of it; a present call may go through one local lambda around presentBatch). runJob ends every
           job with signalRelease( after presentJob( (no return before it). The file has no
           vkDeviceWaitIdle( or submitImmediate(.
           Rebuilds and waits: recreateMainSwapchain calls drainFrameGen() before mSwapchain.recreate( and
           mFgPresent.prepare( after it, and is the only place that recreates the main chain; drainFrameGen
           drains the present thread, then the pacer, then gatesReleased(); shutdown calls drainFrameGen()
           and mFgPresent.shutdown( before shutdownDirect( and the main swapchain's destroy; fseRelease drains
           first. (The device wait: rule STALL.)
           The generator: fillGenInputs's colour is fgHudless(...)'s, its output the present thread's
           scratch image (fgScratch()), and it never touches fgUi / a swapchain image / a backbuffer; the
           job's inputs (<job>.eval) come from fillGenInputs. The single submit carries wsi.frameDone and
           wsi.released, asyncSubmitFrame both, submitEmptyFrame wsi.frameDone; renderFrame sets
           wsi.released / wsi.releasedValue once each from mFgPresent.released() / releasedValue(fgSlot),
           under `if (fgChain`, before asyncSubmitFrame(. Task 7b's late composites and in-frame generated
           images are gone (no extraAcquire, lateDone, mFgLate, fgLateSlotFree, ensureFgLate, releaseFgLate,
           kFgLateSlots, acquireExtra, mExtraAcquire, fgInterp, kMaxFgInterp, presentFrameGenDirect,
           recordFrameGenDirect, trimFrameGen, FgBatch in the render loop, the renderer, the display frame,
           the swapchain or LLVKFrameGen). A static_assert keeps LLVKFrameGenPresent::kMaxSlots <=
           VkDisplayEncode::kSlots. The pre-check says why it refuses (directRefused( between decideDirect(
           and waitSlot(), and so does recordFrameGenInputs (the generator's input copies). Every
           recordFgComposite( result is checked. llfg::swapchainImagesNeeded(g) is g + 1, and wantFrameGenChain
           rebuilds a chain built for fewer generated frames (frameGenMax()). chainGenerated(bool
           outputGenerates) reads RenderVKUpscaler and renderFrame gives it
           llfg::outputGenerates(the Context's output): HDR10 and (#1536) 8-bit SDR. No slip_ / NGX / ngx in
           llvulkancontext.cpp's code nor in renderFrame / recordFrameGenInputs / fillGenInputs (comments
           included), and none of slip_ / NGX / ngx / DLSS in the present thread's files (a generator-neutral
           seam: they talk to LLVKFrameGen only).
  HDR-UI   indra/newview/tests/hdr_ui_layer_contract.py passes (run with the host compiler): frame
           generation shares the display encode's shader with the normal HDR path, whose interface
           layer contract must keep holding.
           Vsync returns when generation cannot run: chainGenerated reads the upscaler choice
           (RenderVKUpscaler) and is given the output's test (the Context's output).
  (LINUX moved to framegen_linux.py with the hidden-window rule HIDDEN, Task 11b fix round 1.)
  (STALL -- the one stall rule, Task 7c fix round 4 -- is framegen_stall.py.)
  REFLEX   Reflex (VK_NV_low_latency2). vkLatencySleepNV, vkSetLatencyMarkerNV,
           vkSetLatencySleepModeNV and vkQueueNotifyOutOfBandNV are named nowhere under indra/newview but
           llvkframegen.cpp, and there only inside `#if LL_WINDOWS || LL_LINUX` and only as the name handed to
           vkGetDeviceProcAddr (never a call of the loader's prototype). llappviewer.cpp's fg_paced
           requires LLVKFrameGen::instance().reflexCapsFrames() (no other choice). In
           llvulkancontext.cpp setReflexSwapchain( runs before and after mSwapchain.recreate( in
           recreateMainSwapchain, after mSwapchain.init( in init, and in shutdown before shutdownDirect(
           and the main swapchain's destroy; the main swapchain's low latency mode (setLatencyMode() follows
           reflexAvailable(), and VkSwapchainLatencyCreateInfoNV is chained only in vk_swapchain.cpp's
           createSwapchainObjects. The 5 s direct line says whether Reflex is on.
           (fix round 1) Low latency mode is chained only on a chain built for frame generation
           (mLatencyWanted && fg), and a creation refused with it is tried once more without it.
           recreateMainSwapchain's first setReflexSwapchain( passes VK_NULL_HANDLE. reflexSleep waits once,
           with a finite constant timeout (never UINT64_MAX), and has a breaker: a counter compared with a
           constant sets an off flag that directReflex() reads; the 5 s line says "did not answer" when
           it tripped and gives the sleeps' average and worst wait. The CPU limiter steps aside on the
           pacing branch's own decision (a variable set from fg_paced). shutdownDirect waits the
           Reflex semaphore (bounded) before destroying it.
  UI       Preferences and the status bar. In the advanced graphics floater the FrameGen combo's
           tooltip, every framegen_* string and vsync_framegen name neither a restart nor another
           route, and there is no FrameGenRestart note. The panel's FrameGen block enables the combo
           exactly when directReady(), reads multiplierOffered(), startupBlock() with blockText( and
           reason(), and for the status line shownPerRendered(), statusText(), driverPacing() and
           reflexCapsFrames(). The vsync checkbox is greyed by chainForcesNoVsync(), never by a
           condition of its own; chainForcesNoVsync() is inline in llvkframegen.h, reads the main chain
           as built (a member setReflexSwapchain sets), and every setReflexSwapchain( of a real chain in
           llvulkancontext.cpp hands it VkSwapchain::ignoresVsync() (declared in vk_swapchain.h: the
           present mode chosen for frame generation, whether its objects were made or not). Both
           blocks live in refreshFrameGen(), which refreshEnabledState() calls, and updateForDraw()
           calls on the 1 s mVramTextTimer tick (the status line) and when chainForcesNoVsync() is no
           longer what the checkbox shows (the render loop rebuilds the chain after a setting signal).
           The status bar's multiplier is `(fg.directReady() && fg.multiplierOn()) ? ... : 1` (no
           multiplied rate lingers after the setting goes below 2).
"""
import os
import re

NAME = "framegen-direct"
STAGE = "source"
DESCRIPTION = "frame generation: the device, swapchain, pacer, Reflex, HDR10, FPS display and Preferences rules"
GUARDS = ("Frame generation's device requests outside Windows / Linux, a present queue that "
          "is the viewer's own queue, a device lost to a refused frame generation extension, a generated "
          "frame presented on the viewer's queue, a swapchain rebuilt with a gate still shut, frame "
          "generation's objects outliving the device or the DLSS add-on, the generator reached from "
          "more than one seam, the interface handed to the generator, a batch presented from more than "
          "one place or paced before it was presented, experiment code left in the device, "
          "swapchain, display encode or frame generation, anything of the older frame generation "
          "route left in the tree, a slot wait on a frame Reflex paces, the HDR10 test read from the "
          "HDR target in flight, an FPS box that shows only the real frames or no longer fits, or a "
          "Preferences choice beyond Off / 2x / 3x / 4x or naming another viewer, or a vsync note that is "
          "wrong off Windows; the add-on's own block decided out of order or its loader's reason in the "
          "player's line.")

DEV_H = "indra/newview/vkgfx/vk_device.h"
DEV = "indra/newview/vkgfx/vk_device.cpp"
CTX = "indra/newview/vkgfx/llvulkancontext.cpp"
SWAP_H = "indra/newview/vkgfx/vk_swapchain.h"
SWAP = "indra/newview/vkgfx/vk_swapchain.cpp"
NR_H = "indra/newview/vkgfx/llvkneuralrecon.h"
NR = "indra/newview/vkgfx/llvkneuralrecon.cpp"
FG_H = "indra/newview/vkgfx/llvkframegen.h"
FG = "indra/newview/vkgfx/llvkframegen.cpp"
FG_DLSS = "indra/newview/vkgfx/llvkframegendlss.cpp"
RR = "indra/newview/vkgfx/llcleandlssrrvk.cpp"
ENC_H = "indra/newview/vkgfx/vk_display_encode.h"
ENC = "indra/newview/vkgfx/vk_display_encode.cpp"
DF_H = "indra/newview/vkgfx/vk_display_frame.h"
DF = "indra/newview/vkgfx/vk_display_frame.cpp"
FRAG = "indra/vkclean/shaders/clean_display_output.frag"
REN = "indra/newview/vkgfx/llcleanrenderervk.cpp"
SETTINGS = "indra/newview/app_settings/settings.xml"
NGX_H = "contrib/ngxshim/include/slipstream_ngx.h"
PT_H = "indra/newview/vkgfx/llvkframegenpresent.h"
PT = "indra/newview/vkgfx/llvkframegenpresent.cpp"


def _code(text, blank_strings=False):
    """C++ source without comments (newlines kept); string / char literal contents optionally
    blanked. One pass, so a '/*' inside a string or a quote inside a comment can't mislead it."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            body = text[i + 1:j]
            out.append(c + ("" if blank_strings else body) + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _mask(text, blank_strings=False):
    """_code with every removed character replaced by a space (newlines kept): the same length as
    text, so a position in it is the same position in the original."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] in "/*":
            j = (text.find("\n", i) if text[i + 1] == "/" else text.find("*/", i + 2))
            j = n if j < 0 else (j if text[i + 1] == "/" else j + 2)
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            if blank_strings:
                for k in range(i + 1, min(j, n)):
                    out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def _function(text, signature):
    """The body of the function whose definition starts with signature (brace matched)."""
    at = text.find(signature)
    if at < 0:
        return None
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


def _raw(ctx, rel):
    try:
        return open(ctx.path(rel), encoding="utf-8", errors="replace").read()
    except OSError:
        return None


def _first_args(text, name):
    """The first argument of every call of name( in text (parentheses matched)."""
    args = []
    for m in re.finditer(r"(?<![\w.>])" + re.escape(name) + r"\s*\(", text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            c = text[i]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "," and depth == 1:
                break
            i += 1
        args.append(re.sub(r"\s+", "", text[m.end():i]))
    return args


# (Task 11b) Frame generation's platforms: Windows and Linux. Its device requests, its objects and Reflex sit
# inside this gate; anything else (macOS) reports Block::Platform.
PLATFORM_IF = r"^[ \t]*#[ \t]*if[ \t]+LL_WINDOWS[ \t]*\|\|[ \t]*LL_LINUX[ \t]*$"
PLATFORM_GATE = "`#if LL_WINDOWS || LL_LINUX`"


def _windows_spans(text, opener=PLATFORM_IF):
    """(start, end) of every frame generation platform branch (`#if LL_WINDOWS || LL_LINUX`, or the
    opener given): up to its #else / #elif / #endif."""
    spans = []
    for m in re.finditer(opener, text, flags=re.M):
        depth = 0
        for d in re.finditer(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|else|elif|endif)\b", text[m.end():], flags=re.M):
            kw = d.group(1)
            if kw.startswith("if"):
                depth += 1
            elif kw == "endif" and depth:
                depth -= 1
            elif depth == 0:
                spans.append((m.end(), m.end() + d.start()))
                break
    return spans


def _inside(spans, pos):
    return any(a <= pos < b for a, b in spans)


def _rule_device(ctx, bad):
    raw_h = _raw(ctx, DEV_H)
    raw_c = _raw(ctx, DEV)
    if raw_h is None or raw_c is None:
        bad.append("DEVICE: %s or %s is missing" % (DEV_H, DEV))
        return
    for rel, raw in ((DEV_H, raw_h), (DEV, raw_c)):
        for i, line in enumerate(raw.splitlines(), 1):
            if "SPIKE" in line:
                bad.append("DEVICE: %s:%d still has experiment code: %s" % (rel, i, line.strip()))
    h = _code(raw_h)
    for ret, fn in (("VkQueue", "presentQueue"), ("bool", "calibratedTimestamps"), ("bool", "lowLatency2"),
                    ("bool", "presentMetering"), ("double", "timestampPeriodNs")):
        if not re.search(r"\b%s\s+%s\s*\(\s*\)\s*const\b" % (ret, fn), h):
            bad.append("DEVICE: %s must declare %s %s() const" % (DEV_H, ret, fn))
    if "presentMeteringState" in h:
        bad.append("DEVICE: %s still has presentMeteringState (replaced by presentMetering())" % DEV_H)
    if not re.search(r"void\s+setOptionalExtensionProvider\s*\(", h):
        bad.append("DEVICE: %s must declare setOptionalExtensionProvider(" % DEV_H)

    c = _code(raw_c, blank_strings=True)
    body = _function(c, "bool VkDeviceContext::createLogicalDevice(")
    if not body:
        bad.append("DEVICE: VkDeviceContext::createLogicalDevice not found in %s" % DEV)
        return
    at = c.find(body)
    spans = _windows_spans(c)
    grows = [m for m in re.finditer(r"queueCount\s*=\s*mPresentQueueIndex\s*\+\s*1\b", body)]
    if not grows:
        bad.append("DEVICE: createLogicalDevice must grow the graphics family's request by the present queue "
                   "(queueCount = mPresentQueueIndex + 1)")
    for m in grows:
        if not _inside(spans, at + m.start()):
            bad.append("DEVICE: the present queue is requested outside %s" % PLATFORM_GATE)
    for m in re.finditer(r"mPresentQueueIndex\s*=(?!=)\s*([^;]*);", body):
        if m.group(1).strip() != "~0u" and not _inside(spans, at + m.start()):
            bad.append("DEVICE: mPresentQueueIndex is set outside %s" % PLATFORM_GATE)
    if not re.search(r"vkGetDeviceQueue\(\s*mDevice\s*,\s*mQueueFamily\s*,\s*mPresentQueueIndex\s*,\s*&mPresentQueue\s*\)", body):
        bad.append("DEVICE: createLogicalDevice must fetch the present queue "
                   "(vkGetDeviceQueue(mDevice, mQueueFamily, mPresentQueueIndex, &mPresentQueue))")
    if re.search(r"mPresentQueueIndex\s*=\s*0\s*;", body):
        bad.append("DEVICE: the present queue must never be index 0 of the graphics family (the viewer's own)")
    names = ("VK_NV_LOW_LATENCY_2_EXTENSION_NAME", "VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME",
             "VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME", "mOptExtProvider(")
    for n in names:
        hits = [m.start() for m in re.finditer(re.escape(n), body)]
        if not hits:
            bad.append("DEVICE: createLogicalDevice never asks for %s" % n.rstrip("("))
        for p in hits:
            if not _inside(spans, at + p):
                bad.append("DEVICE: %s is asked for outside %s" % (n.rstrip("("), PLATFORM_GATE))
    if not re.search(r"dr\s*!=\s*VK_SUCCESS\s*&&\s*fgd\.added\b", body):
        bad.append("DEVICE: a device refused with the present queue / timing extensions must be created again "
                   "without them (if (dr != VK_SUCCESS && fgd.added))")

    cx = _code(_raw(ctx, CTX) or "")
    if not re.search(r"setOptionalExtensionProvider\s*\([^;]*fgDeviceExtensions\s*\(", cx, flags=re.S):
        bad.append("DEVICE: %s must set the optional extension provider from "
                   "LLVKNeuralRecon::instance().fgDeviceExtensions(" % CTX)
    if "presentMeteringState" in cx:
        bad.append("DEVICE: %s still calls presentMeteringState()" % CTX)
    nh = _code(_raw(ctx, NR_H) or "")
    nr = _code(_raw(ctx, NR) or "")
    if not re.search(r"bool\s+fgDeviceExtensions\s*\(", nh) \
            or not _function(nr, "bool LLVKNeuralRecon::fgDeviceExtensions(") \
            or "SLIP_FG_FN_DEVICE_EXTENSIONS" not in nr:
        bad.append("DEVICE: LLVKNeuralRecon needs fgDeviceExtensions (the add-on's SLIP_FG_FN_DEVICE_EXTENSIONS)")


def _rule_swapchain(ctx, bad):
    raw_h = _raw(ctx, SWAP_H)
    raw_c = _raw(ctx, SWAP)
    if raw_h is None or raw_c is None:
        bad.append("SWAPCHAIN: %s or %s is missing" % (SWAP_H, SWAP))
        return
    for rel, raw in ((SWAP_H, raw_h), (SWAP, raw_c)):
        for i, line in enumerate(raw.splitlines(), 1):
            if "spike" in line.lower():
                bad.append("SWAPCHAIN: %s:%d still has experiment code: %s" % (rel, i, line.strip()))
    h = _code(raw_h)
    for label, pat in (("void setFrameGen(uint32_t", r"void\s+setFrameGen\s*\(\s*uint32_t"),
                       ("uint32_t frameGenImages() const", r"uint32_t\s+frameGenImages\s*\(\s*\)\s*const"),
                       ("uint32_t frameGenMax() const", r"uint32_t\s+frameGenMax\s*\(\s*\)\s*const"),
                       ("bool acquireOne(", r"bool\s+acquireOne\s*\("),
                       ("bool presentBatch(", r"bool\s+presentBatch\s*\("),
                       ("bool gatedPresentsPending() const", r"bool\s+gatedPresentsPending\s*\(\s*\)\s*const"),
                       ("void gatesReleased(", r"void\s+gatesReleased\s*\(")):
        if not re.search(pat, h):
            bad.append("SWAPCHAIN: %s must declare %s" % (SWAP_H, label))
    if "VkSetPresentConfigNV" in h:
        bad.append("SWAPCHAIN: %s uses VkSetPresentConfigNV (only presentBatch may)" % SWAP_H)

    c = _code(raw_c, blank_strings=True)
    body = _function(c, "bool VkSwapchain::presentBatch(")
    if not body:
        bad.append("SWAPCHAIN: VkSwapchain::presentBatch not found in %s" % SWAP)
        return
    presents = _first_args(body, "vkQueuePresentKHR") + _first_args(body, "mProcs.present")
    if not presents:
        bad.append("SWAPCHAIN: presentBatch never presents")
    for q in presents:
        if q != "ctx.presentQueue()":
            bad.append("SWAPCHAIN: presentBatch presents on %s (every present goes to ctx.presentQueue())" % q)
    gates = _first_args(body, "vkQueueSubmit2")
    if not gates:
        bad.append("SWAPCHAIN: presentBatch has no gate submit (vkQueueSubmit2)")
    for q in gates:
        if q != "ctx.presentQueue()":
            bad.append("SWAPCHAIN: presentBatch submits a gate on %s (it goes to ctx.presentQueue())" % q)
    if "ctx.queue()" in body:
        bad.append("SWAPCHAIN: presentBatch uses ctx.queue() (the batch belongs to the present queue)")
    rest = c.replace(body, "")
    if "VkSetPresentConfigNV" not in body:
        bad.append("SWAPCHAIN: presentBatch never chains VkSetPresentConfigNV (driver pacing)")
    if "VkSetPresentConfigNV" in rest:
        bad.append("SWAPCHAIN: VkSetPresentConfigNV is used outside presentBatch in %s" % SWAP)
    # (Task 7c) A frame generation chain's images belong to the present thread: acquire() and present()
    # (the main thread's) refuse such a chain before they reach the driver, and present() uses the
    # viewer's queue (it never meets a batch held on the present queue).
    plain = _function(c, "bool VkSwapchain::present(") or ""
    qs = _first_args(plain, "vkQueuePresentKHR")
    if not qs or any(q != "ctx.queue()" for q in qs):
        bad.append("SWAPCHAIN: present() presents on ctx.queue() (found %s)" % (", ".join(qs) or "no present"))
    for sig, call in (("bool VkSwapchain::acquire(", "vkAcquireNextImageKHR("), ("bool VkSwapchain::present(", "vkQueuePresentKHR(")):
        fn = _function(c, sig) or ""
        g, k = fn.find("mFgImages"), fn.find(call)
        if not (0 <= g < k):
            bad.append("SWAPCHAIN: %s must refuse a frame generation chain (mFgImages) before %s"
                       % (sig.split("::")[1].rstrip("("), call.rstrip("(")))
    one = _function(c, "bool VkSwapchain::acquireOne(") or ""
    if "vkAcquireNextImageKHR(" not in one or "mFrame" in one:
        bad.append("SWAPCHAIN: acquireOne( fetches one image with the caller's semaphore and never touches the main "
                   "thread's frame slot (mFrame)")
    if re.search(r"\bmFrame\b", body):
        bad.append("SWAPCHAIN: presentBatch must not move the main thread's frame slot (mFrame): the present thread "
                   "calls it")
    for sig in ("bool VkSwapchain::recreate(", "bool VkSwapchain::destroy("):
        fn = _function(c, sig)
        if not fn or not re.search(r"\bgatedPresentsPending\s*\(\s*\)", fn):
            bad.append("SWAPCHAIN: %s must check gatedPresentsPending() (the owner drains the pacer first)"
                       % sig.split("::")[1].rstrip("("))


def _windows_else(text):
    """The `#else` branch of the first `#if LL_WINDOWS || LL_LINUX` in text (None without one)."""
    m = re.search(PLATFORM_IF, text, flags=re.M)
    if not m:
        return None
    depth, start = 0, None
    for d in re.finditer(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|else|elif|endif)\b.*$", text[m.end():], flags=re.M):
        kw = d.group(1)
        if kw.startswith("if"):
            depth += 1
        elif kw == "endif" and depth:
            depth -= 1
        elif depth == 0 and kw == "else" and start is None:
            start = m.end() + d.end()
        elif depth == 0 and kw == "endif":
            return text[start:m.end() + d.start()] if start is not None else None
    return None


def _rule_direct(ctx, bad):
    raw_h = _raw(ctx, FG_H)
    raw_c = _raw(ctx, FG)
    if raw_h is None or raw_c is None:
        bad.append("DIRECT: %s or %s is missing" % (FG_H, FG))
        return
    h = _code(raw_h)
    decls = (("void initDirect(VkDeviceContext&", r"\bvoid\s+initDirect\s*\(\s*VkDeviceContext\s*&"),
             ("void shutdownDirect(VkDeviceContext&", r"\bvoid\s+shutdownDirect\s*\(\s*VkDeviceContext\s*&"),
             ("bool directReady() const", r"\bbool\s+directReady\s*\(\s*\)\s*const"),
             ("uint32_t maxGenerated() const", r"\buint32_t\s+maxGenerated\s*\(\s*\)\s*const"),
             ("uint32_t decideDirect(const llfg::Context&, uint32_t",
              r"\buint32_t\s+decideDirect\s*\(\s*const\s+llfg::Context\s*&\s*\w*\s*,\s*uint32_t\b"),
             ("bool evaluate(VkCommandBuffer, uint32_t, const llfg::GenInputs&, uint32_t, uint32_t",
              r"\bbool\s+evaluate\s*\(\s*VkCommandBuffer\s+\w*\s*,\s*uint32_t\s+\w*\s*,\s*"
              r"const\s+llfg::GenInputs\s*&\s*\w*\s*,\s*uint32_t\s+\w*\s*,\s*uint32_t\b"),
             ("void drainPacer()", r"\bvoid\s+drainPacer\s*\(\s*\)"),
             ("VkSemaphore gate() const", r"\bVkSemaphore\s+gate\s*\(\s*\)\s*const"),
             ("VkSemaphore frameDone() const", r"\bVkSemaphore\s+frameDone\s*\(\s*\)\s*const"),
             ("uint64_t nextFrameDoneValue()", r"\buint64_t\s+nextFrameDoneValue\s*\(\s*\)"),
             ("VkFramePacer& pacer()", r"\bVkFramePacer\s*&\s*pacer\s*\(\s*\)"),
             ("void meteringResult(bool", r"\bvoid\s+meteringResult\s*\(\s*bool\b"),
             ("void meteringReset()", r"\bvoid\s+meteringReset\s*\(\s*\)"),
             ("VkSemaphore generatedDone() const", r"\bVkSemaphore\s+generatedDone\s*\(\s*\)\s*const"),
             ("uint64_t nextGeneratedDoneValue()", r"\buint64_t\s+nextGeneratedDoneValue\s*\(\s*\)"),
             ("bool prepareFeature(VkDeviceContext&", r"\bbool\s+prepareFeature\s*\(\s*VkDeviceContext\s*&"),
             ("bool driverPacing() const", r"\bbool\s+driverPacing\s*\(\s*\)\s*const"),
             ("uint32_t timestampQuery(VkCommandBuffer)", r"\buint32_t\s+timestampQuery\s*\(\s*VkCommandBuffer\s+\w*\s*\)"),
             ("void batchPresented(uint32_t", r"\bvoid\s+batchPresented\s*\(\s*uint32_t\b"),
             ("bool takeConstants(llfg::GenCamera&", r"\bbool\s+takeConstants\s*\(\s*llfg::GenCamera\s*&"))
    for label, pat in decls:
        if not re.search(pat, h):
            bad.append("DIRECT: %s must declare %s" % (FG_H, label))

    # The throwaway experiment is gone: its names, its block in the render loop, its settings' readers.
    for rel in (NR_H, NR, FG_H, FG, RR):
        for i, line in enumerate((_raw(ctx, rel) or "").splitlines(), 1):
            if "spike" in line.lower():
                bad.append("DIRECT: %s:%d still has experiment code: %s" % (rel, i, line.strip()))
    raw_cx = _raw(ctx, CTX) or ""
    for word in ("spikeWanted", "struct Spike", "FG SPIKE"):
        if word in raw_cx:
            bad.append("DIRECT: %s still has the experiment's %s" % (CTX, word))
    tree = ctx.path("indra/newview")
    for root, _dirs, files in os.walk(tree):
        for f in files:
            if not f.endswith((".cpp", ".h")):
                continue
            p = os.path.join(root, f)
            t = _code(open(p, encoding="utf-8", errors="replace").read())
            if re.search(r'"RenderVKFrameGenSpike\w*"', t):
                bad.append("DIRECT: %s still reads RenderVKFrameGenSpike*" % os.path.relpath(p, ctx.path(".")))

    # Init after the DLSS add-on is up on the device; shutdown before it, the swapchain and the device go.
    cx = _code(raw_cx, blank_strings=True)
    init = _function(cx, "bool LLVulkanContext::init(") or ""
    nr_init = init.find("LLVKNeuralRecon::instance().initDevice(")
    di = [m.start() for m in re.finditer(r"\binitDirect\s*\(", init)]
    if len(di) != 1 or nr_init < 0 or di[0] < nr_init:
        bad.append("DIRECT: LLVulkanContext::init must call initDirect( once, after LLVKNeuralRecon's initDevice(")
    shut = _function(cx, "void LLVulkanContext::shutdown(") or ""
    sd = [m.start() for m in re.finditer(r"\bshutdownDirect\s*\(", shut)]
    later = [shut.find(s) for s in ("LLVKNeuralRecon::instance().shutdown()", "mSwapchain.destroy(",
                                     "mDevice.shutdown()")]
    if len(sd) != 1 or any(p < 0 or p < sd[0] for p in later):
        bad.append("DIRECT: LLVulkanContext::shutdown must call shutdownDirect( once, before LLVKNeuralRecon's "
                   "shutdown, the main swapchain's destroy and the device's shutdown")

    # initDirect on other platforms (not Windows, not Linux): Block::Platform and nothing else.
    c = _code(raw_c, blank_strings=True)
    body = _function(c, "void LLVKFrameGen::initDirect(")
    nw = _windows_else(body) if body else None
    if nw is None:
        bad.append("DIRECT: LLVKFrameGen::initDirect needs an %s ... `#else` (other platforms) branch" % PLATFORM_GATE)
    else:
        stmts = [s.strip() for s in nw.split(";") if s.strip()]
        if not any("Block::Platform" in s for s in stmts):
            bad.append("DIRECT: initDirect on other platforms must report Block::Platform")
        for s in stmts:
            if not re.fullmatch(r"\(\s*void\s*\)\s*\w+", s) and "Block::Platform" not in s:
                bad.append("DIRECT: initDirect on other platforms does more than report Block::Platform: %s"
                           % " ".join(s.split()))

    # (Task 11b fix 1, M7) initDirect's add-on decision is llfg::addonBlock( (its order -- switched off over not
    # loaded, then too old, then unable to run -- tested in test_framegen_policy.cpp), given the add-on's own
    # answers in that order; the loader's reason for NoAddon goes to the log only (the player's line stays short).
    # (#1544) That decision is the DLSS generator's own (LLVKFrameGenDlss::init: its block, a detail for the
    # player's line, one for the log); initDirect shows what the generator in use answered, unchanged.
    init_body = body
    body = _function(_code(_raw(ctx, FG_DLSS) or "", blank_strings=True), "llfg::Block LLVKFrameGenDlss::init(")
    if not body:
        bad.append("DIRECT: %s must define LLVKFrameGenDlss::init (the add-on's and the GPU's block)" % FG_DLSS)
    # (each generator's answer is kept -- block, caps, detail, log -- and the one llfg::chooseGenerator names as the
    # reason is shown with ITS detail and log text)
    gi = re.search(r"(\w+)->block\s*=\s*\1->gen->init\s*\(\s*\w+\s*,\s*\1->caps\s*,\s*\1->detail\s*,\s*\1->log\s*\)",
                   init_body or "")
    why = re.search(r"const\s+Backend\s*&\s*(\w+)\s*=\s*choice\.reasonOf\s*==\s*llfg::Generator::Fsr\s*\?\s*mFsr\s*:\s*mDlss\s*;",
                    init_body or "")
    if not gi or not why or not re.search(r"directBlocked\(\s*choice\.reason\s*,\s*%s\.detail\s*,\s*%s\.log\s*\)"
                                          % (why.group(1), why.group(1)), init_body or ""):
        bad.append("DIRECT: initDirect must keep each generator's own answer (`b->block = b->gen->init(dev, b->caps, "
                   "b->detail, b->log)`) and show the chosen reason with that generator's detail and log text "
                   "(`directBlocked(choice.reason, why.detail, why.log)`, why = the generator choice.reasonOf names)")
    ab = re.search(r"llfg::addonBlock\(\s*(\w+)\.loaded\(\)\s*,\s*\1\.switchedOff\(\)\s*,\s*\1\.fgHasEntryPoints\(\)\s*,"
                   r"\s*\1\.available\(\)\s*\)", body or "")
    if not ab:
        bad.append("DIRECT: LLVKFrameGenDlss::init must decide the add-on's block with llfg::addonBlock(nr.loaded(), "
                   "nr.switchedOff(), nr.fgHasEntryPoints(), nr.available())")
    for m in re.finditer(r"\bdetail\s*=[^;]*reason\(\)", body or ""):
        head = (body or "")[:m.start()]
        guard = head[max(head.rfind(";"), head.rfind("}"), head.rfind("{")) + 1:]
        if not re.search(r"==\s*llfg::Block::Unsupported\s*\)\s*$", guard):
            bad.append("DIRECT: LLVKFrameGenDlss::init puts the add-on's reason into the player's text (detail) of a "
                       "block other than Unsupported: only Unsupported carries it; NoAddon's loader reason goes to "
                       "the log")
    if body and "llfg::addonPredatesFixes(" not in body:
        bad.append("DIRECT: LLVKFrameGenDlss::init must suggest rebuilding an add-on that predates 1.3.1's fixes "
                   "(llfg::addonPredatesFixes()")
    # (before fix 1) NoAddon on !loaded() only, then OldAddon, then Unsupported on !available().
    if body and not ab:
        def guards(block):
            out = []
            for m in re.finditer(r"directBlocked\(\s*llfg::Block::%s\b" % block, body):
                head = body[:m.start()]
                cut = max(head.rfind(";"), head.rfind("}"))
                out.append((m.start(), head[cut + 1:]))
            return out
        no_addon, old_addon, unsup = guards("NoAddon"), guards("OldAddon"), guards("Unsupported")
        if not no_addon or any("available()" in g or "loaded()" not in g for _p, g in no_addon):
            bad.append("DIRECT: initDirect must block with NoAddon on !loaded() only, never on available() "
                       "(a loaded add-on that cannot run is Unsupported)")
        avail = [p for p, g in unsup if "available()" in g]
        if not avail:
            bad.append("DIRECT: initDirect must block with Unsupported (the add-on's reason) on !available()")
        if no_addon and old_addon and avail and not (no_addon[0][0] < old_addon[0][0] < avail[0]):
            bad.append("DIRECT: initDirect must decide NoAddon (!loaded()), then OldAddon, then Unsupported (!available())")

    # One seam for the generator: only initDirect, prepareFeature (main thread: the feature for an image
    # size), evaluate (the present thread) and shutdownDirect reach it (#1544: through LLVKFrameGenGenerator --
    # mGen->init / prepare / release / evaluate; the add-on itself is LLVKFrameGenDlss's, rule framegen-generator).
    allowed = [_function(c, "void LLVKFrameGen::initDirect("), _function(c, "bool LLVKFrameGen::prepareFeature("),
               _function(c, "bool LLVKFrameGen::evaluate("), _function(c, "void LLVKFrameGen::shutdownDirect("),
               _function(c, "void LLVKFrameGen::applyGenerator(")]   # (#1544 Task 6: a live switch releases the old feature)
    ev = _function(c, "bool LLVKFrameGen::evaluate(") or ""
    # (Task 7c fix) One failure, one report: after a failed evaluate the jobs already queued never reach the
    # add-on again (a sticky flag, checked before it), and the driver pacing state changes under one lock.
    nr_at = ev.find("mGen->evaluate(")
    if not re.search(r"\bif\s*\([^;]*mEvalStopped\s*\.\s*load\s*\(\s*\)[^;]*\)\s*return\s+false", ev) \
            or nr_at < 0 or ev.find("mEvalStopped") > nr_at or "mEvalStopped.store(true)" not in ev:
        bad.append("DIRECT: LLVKFrameGen::evaluate must stop at a sticky mEvalStopped (set on its failure) before it "
                   "reaches the add-on: the jobs queued behind a failure would call it again and report twice")
    # (driver pacing) the present thread's answers and the main thread's resets change llfg::PacingState together,
    # under one lock: a refusal counted between a reset's halves would be lost or a reset undone
    for fn in ("meteringResult", "meteringReset", "meteringBatchChanged"):
        mr = re.search(r"void\s+" + fn + r"\s*\([^)]*\)\s*\{(.*?)\n\s*\}", h, flags=re.S)
        if not mr or not re.search(r"lock_guard<std::mutex>\s+\w+\s*\(\s*mPacingMutex\s*\)", mr.group(1)):
            bad.append("DIRECT: %s must change the driver pacing state under mPacingMutex: the present thread's "
                       "answers race the main thread's resets" % fn)
    for word in ("vkDeviceWaitIdle(", "submitImmediate(", "drainPacer(", "fgCreate(", "fgRelease(", "directFailed(",
                 "->prepare(", "->release(", "->init("):
        if word in ev:
            bad.append("DIRECT: LLVKFrameGen::evaluate (the present thread) calls %s: it only records the evaluate"
                       % word.rstrip("("))
    spans = []
    for b in allowed:
        if b:
            at = c.find(b)
            spans.append((at, at + len(b)))
    for m in re.finditer(r"\b(?:mGen|gen)\s*->\s*(?:init|prepare|release|evaluate)\s*\(", c):
        if not _inside(spans, m.start()):
            bad.append("DIRECT: %s reaches the generator outside initDirect / prepareFeature / evaluate / shutdownDirect / applyGenerator (line %d)"
                       % (FG, c.count("\n", 0, m.start()) + 1))
    for m in re.finditer(r"\bLLVKNeuralRecon\b", c):
        bad.append("DIRECT: %s names LLVKNeuralRecon (line %d): the add-on is reached through the DLSS generator only (%s)"
                   % (FG, c.count("\n", 0, m.start()) + 1, FG_DLSS))

    # The pacer's waits: a high-resolution waitable timer, never Sleep (up to ~15 ms late).
    for m in re.finditer(r"(?<![\w:.>])Sleep\s*\(", c):
        bad.append("DIRECT: %s calls Sleep( (line %d): the pacer waits on a high-resolution timer"
                   % (FG, c.count("\n", 0, m.start()) + 1))

    # The add-on's ABI header stays inside LLVKNeuralRecon.
    for root, _dirs, files in os.walk(ctx.path("indra")):
        for f in files:
            if not f.endswith((".cpp", ".h", ".hpp", ".inl", ".c", ".mm")):
                continue
            p = os.path.join(root, f)
            rel = os.path.relpath(p, ctx.path("."))
            if rel == NR:
                continue
            t = _code(open(p, encoding="utf-8", errors="replace").read())
            if re.search(r'#\s*include\s*[<"][^>"]*slipstream_ngx\.h[>"]', t):
                bad.append("DIRECT: %s includes slipstream_ngx.h (only %s may)" % (rel, NR))


def _bodies(text, name):
    """(start, end) of every brace-matched body of a definition of `name(` (qualified names allowed)."""
    out = []
    # (the parameter list may hold one level of parentheses -- never a call's own `())` before a block)
    for m in re.finditer(r"(?<![\w.>])" + re.escape(name) + r"\s*\([^;{()]*(?:\([^;{()]*\)[^;{()]*)*\)\s*(?:const\s*)?\{",
                         text):
        start = m.end() - 1
        depth = 0
        for i in range(start, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    out.append((start, i + 1))
                    break
    return out


def _wrapped_calls(body, target):
    """Positions in body of every call of target( outside a local lambda, and of every call of a local lambda
    (`auto name = [...](...) { ... }`) whose body calls target( -- such a lambda counts as that call where it is
    called (Task 11b fix 1: presentJob's acquire / present wrappers check the hidden window first)."""
    spans, names = [], []
    for m in re.finditer(r"\bauto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\([^)]*\)\s*\{", body):
        span = _block_after(body, m.start())
        if not span:
            continue
        spans.append(span)
        if re.search(r"\b%s\s*\(" % re.escape(target), body[span[0]:span[1]]):
            names.append((m.group(1), span[1]))
    out = [m.start() for m in re.finditer(r"\b%s\s*\(" % re.escape(target), body)
           if not any(a <= m.start() < b for a, b in spans)]
    for name, end in names:
        out += [m.start() for m in re.finditer(r"(?<![\w.>])%s\s*\(" % re.escape(name), body) if m.start() >= end]
    return sorted(out)


def _member_calls(text, name):
    """Positions of every member call of name( in text (obj.name( / ptr->name()."""
    return [m.start(1) for m in re.finditer(r"(?:\.|->)\s*(" + re.escape(name) + r")\s*\(", text)]


def _blank_lambdas(text):
    """text with every lambda body blanked (same length, newlines kept): a `return` in a lambda is not
    a return from the function around it."""
    out = list(text)
    for m in re.finditer(r"\]\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*(?:mutable\s*)?(?:->\s*[\w:<>&*\s]+?)?\{", text):
        start, depth = m.end() - 1, 0
        for i in range(start, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    for k in range(start + 1, i):
                        if out[k] != "\n":
                            out[k] = " "
                    break
    return "".join(out)


def _call_args(text, at):
    """The argument text of the call whose name ends just before the '(' at or after `at`."""
    o = text.index("(", at)
    depth = 0
    for i in range(o, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[o + 1:i]
    return ""


def _enclosing_heads(text, pos):
    """The heads of the blocks around pos, innermost first: for each `{` that encloses pos, the text
    of its statement head (from the previous `;`, `{` or `}` up to the `{`), with an `else` head
    resolved to `else of <the if's head>`."""
    heads = []
    depth = 0
    i = pos - 1
    while i >= 0:
        c = text[i]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth:
                depth -= 1
            else:
                cut = max(text.rfind(";", 0, i), text.rfind("{", 0, i), text.rfind("}", 0, i))
                head = " ".join(text[cut + 1:i].split())
                if head == "else" and text[cut] == "}":
                    # the if block that closed just before this else: its own head
                    d, j = 0, cut
                    while j >= 0:
                        if text[j] == "}":
                            d += 1
                        elif text[j] == "{":
                            d -= 1
                            if d == 0:
                                break
                        j -= 1
                    c2 = max(text.rfind(";", 0, j), text.rfind("{", 0, j), text.rfind("}", 0, j))
                    head = "else of " + " ".join(text[c2 + 1:j].split())
                heads.append(head)
        i -= 1
    return heads


def _not_fg_chain(text, pos):
    """pos is reached only on a chain NOT built for frame generation: an enclosing `if (!fgChain ...)`,
    the else of an `if (fgChain ...)`, or the third operand of `fgChain ? ... : <here>` in its
    statement."""
    for h in _enclosing_heads(text, pos):
        if re.search(r"^if\s*\(\s*!\s*fgChain\b", h):
            return True
        if re.search(r"^else of (?:else\s+)?if\s*\(\s*fgChain\b", h):
            return True
    cut = max(text.rfind(";", 0, pos), text.rfind("{", 0, pos), text.rfind("}", 0, pos))
    stmt = text[cut + 1:pos]
    if re.match(r"\s*else\b", stmt) and cut >= 0 and text[cut] == "}":
        # a braceless else: the if block that closed just before it
        heads = _enclosing_heads(text, cut)   # (the heads around that block's closing brace: its own first)
        if heads and re.search(r"^if\s*\(\s*fgChain\b", heads[0]):
            return True
    return bool(re.search(r"\bfgChain\s*\?[^;]*:\s*[^;]*$", stmt))


def _rule_frame(ctx, bad):
    # The throwaway experiment is gone from the render loop, the display encode and its shader.
    for rel in (CTX, ENC_H, ENC, DF_H, DF, FRAG, PT, PT_H):
        raw = _raw(ctx, rel)
        if raw is None:
            bad.append("FRAME: %s is missing" % rel)
            continue
        for i, line in enumerate(raw.splitlines(), 1):
            if "spike" in line.lower():
                bad.append("FRAME: %s:%d still has experiment code: %s" % (rel, i, line.strip()))
    st = _raw(ctx, SETTINGS) or ""
    if "RenderVKFrameGenSpike" in st:
        bad.append("FRAME: settings.xml still has RenderVKFrameGenSpike*")

    raw_cx = _raw(ctx, CTX) or ""
    cx = _mask(raw_cx, blank_strings=True)   # (positions in it are positions in raw_cx)
    line = lambda pos: cx.count("\n", 0, pos) + 1
    raw_pt = _raw(ctx, PT) or ""
    pt = _mask(raw_pt, blank_strings=True)
    pline = lambda pos: pt.count("\n", 0, pos) + 1

    # (Task 7c) The present thread owns a frame generation chain's swapchain: the main thread never
    # acquires or presents its images and never touches the present queue (Vulkan's external
    # synchronisation of the swapchain and the queue). The present thread's file holds every acquire,
    # present and present-queue submit of such a chain.
    if re.search(r"\bpresentQueue\s*\(", cx):
        bad.append("FRAME: %s names presentQueue() (line %d): only the present thread (%s) uses the present queue"
                   % (CTX, line(re.search(r"\bpresentQueue\s*\(", cx).start()), PT))
    for name in ("acquireOne", "acquireExtra", "presentBatch"):
        for p in _member_calls(cx, name):
            bad.append("FRAME: %s calls %s( (line %d): the present thread fetches and presents a frame generation "
                       "chain's images" % (CTX, name, line(p)))
    for rel in (REN, "indra/newview/vkgfx/llcleanrenderervk.h"):
        if re.search(r"\bpresentQueue\s*\(", _mask(_raw(ctx, rel) or "", blank_strings=True)):
            bad.append("FRAME: %s names presentQueue() (only the present thread submits there)" % rel)
    frame = _bodies(cx, "renderFrame")
    rf = cx[frame[0][0]:frame[0][1]] if frame else ""
    rf_at = frame[0][0] if frame else 0
    if not re.search(r"\bconst\s+bool\s+fgChain\s*=[^;]*frameGenImages\s*\(\s*\)", rf):
        bad.append("FRAME: renderFrame must decide `const bool fgChain = ...frameGenImages()...` (the chain in use "
                   "is built for frame generation)")
    for name in ("acquire", "present"):
        for m in re.finditer(r"mSwapchain\s*\.\s*%s\s*\(" % name, rf):
            if not _not_fg_chain(rf, m.start()):
                bad.append("FRAME: renderFrame's mSwapchain.%s( (line %d) is not limited to a chain without frame "
                           "generation (!fgChain)" % (name, line(rf_at + m.start())))
    # (the wait's answer, then `free` only when it is SlotWait::Free, then the frame slot only when free)
    gated = None
    ws = re.search(r"\bconst\s+VkPresentHandoff::SlotWait\s+(\w+)\s*=[^;]*\bwaitSlot\s*\(", rf)
    fr = ws and re.search(r"\bconst\s+bool\s+(\w+)\s*=\s*%s\s*==\s*VkPresentHandoff::SlotWait::Free\s*;" % ws.group(1),
                          rf[ws.end():])
    if fr:
        gated = re.search(r"\b%s\s*&&[^;]*beginFrameWithoutImage\s*\(" % fr.group(1), rf[ws.end() + fr.end():])
    if not gated:
        bad.append("FRAME: on a frame generation chain renderFrame must wait for its backbuffer slot (waitSlot() && "
                   "then take its frame slot (beginFrameWithoutImage(): a refused wait never leaves the in-flight "
                   "fence reset without a submit")
    pushes = [m.start() for m in re.finditer(r"\bmFgPresent\s*\.\s*push\s*\(", cx)]
    subs = [m.start() for m in re.finditer(r"\b(?:vkQueueSubmit2|asyncSubmitFrame|submitEmptyFrame)\s*\(", rf)]
    if len(pushes) != 1:
        bad.append("FRAME: %s must hand the real frame to the present thread in exactly one place (mFgPresent.push(; "
                   "found %d)" % (CTX, len(pushes)))
    for p in pushes:
        if not frame or not (frame[0][0] <= p < frame[0][1]):
            bad.append("FRAME: mFgPresent.push( outside renderFrame (line %d)" % line(p))
        elif not subs or p - rf_at < max(subs):
            bad.append("FRAME: mFgPresent.push( (line %d) must follow the frame's submits" % line(p))
        elif not any(re.search(r"^if\s*\(\s*fgChain\b", h) for h in _enclosing_heads(cx, p)):
            bad.append("FRAME: mFgPresent.push( (line %d) must be on a frame generation chain (if (fgChain ...))"
                       % line(p))
    if "endFrameWithoutPresent(" not in rf:
        bad.append("FRAME: a frame generation chain's frame must end its frame slot (endFrameWithoutPresent()")

    # The present thread's file: one image per present, fetched just before it is shown.
    jb = _bodies(pt, "LLVKFrameGenPresent::presentJob")
    if not raw_pt or len(jb) != 1:
        bad.append("FRAME: %s must show a job in one function, LLVKFrameGenPresent::presentJob" % PT)
    else:
        a, b = jb[0]
        body = pt[a:b]
        # (a local lambda around acquireOne / presentBatch -- the hidden window's check, the Reflex markers, the
        # timing -- counts as that call where it is called)
        acq = _wrapped_calls(body, "acquireOne")
        pbs = _wrapped_calls(body, "presentBatch")
        fors = [(m.start(), _block_after(body, m.start())) for m in re.finditer(r"\bfor\s*\(", body)]
        in_loop = [p for p in acq if any(f[1] and f[1][0] <= p < f[1][1] for f in fors)]
        if not in_loop:
            bad.append("FRAME: presentJob must fetch each generated frame's image inside its loop (acquireOne()")
        outside = [p for p in acq if p not in in_loop]
        after = [p for p in outside if pbs and p > pbs[0]]
        if not after or len(after) != len(outside):
            bad.append("FRAME: presentJob must fetch the real frame's image after the generated frames' first present "
                       "(acquireOne( after presentBatch(): a chain of generated + 1 images has no image for it before")
        if len(pbs) < 2:
            bad.append("FRAME: presentJob presents the generated frames and then the real frame (two presentBatch()")
        pu = re.search(r"pacer\s*\(\s*\)\s*\.\s*push\s*\(", body)
        if not pu or not pbs or not (pbs[0] < pu.start() < pbs[-1]):
            bad.append("FRAME: presentJob must give the pacer its job after the first (ungated) present and before the "
                       "gated ones (a present call may idle the queue: a gate nobody will open would hang it)")
        npu = re.search(r"!\s*\w+\s*\.\s*pacer\s*\(\s*\)\s*\.\s*push\s*\(", body)
        if not npu or "releaseGates(" not in body[npu.end():]:
            bad.append("FRAME: presentJob must call releaseGates( when pacer().push( returns false")
        if acq and pbs:
            for m in re.finditer(r"\breturn\b", _blank_lambdas(body)[acq[0]:pbs[-1]]):
                bad.append("FRAME: a return between presentJob's first acquireOne( and its last presentBatch( (line %d) "
                           "would leave a fetched image unpresented" % pline(a + acq[0] + m.start()))
        for p in [m.start() for m in re.finditer(r"\bvkQueueSubmit2\s*\(", pt)]:
            q = re.sub(r"\s+", "", _call_args(pt, p).split(",")[0])
            if not q.endswith("presentQueue()"):
                bad.append("FRAME: %s submits to %s (line %d), not the present queue" % (PT, q, pline(p)))
        if "frameDone()" not in body:
            bad.append("FRAME: presentJob's submits must wait the frame's frame-done value (frameDone())")
        for m in re.finditer(r"(?:\.|->)\s*evaluate\s*\(", body):
            parts = [s.strip() for s in _call_args(body, m.start()).split(",")]
            local = len(parts) == 5 and re.search(r"\bllfg::GenInputs\s+%s\s*=\s*\w+\s*\.\s*eval\s*;"
                                                  % re.escape(parts[2]), body)
            if len(parts) != 5 or parts[1] != "slot" or not (re.fullmatch(r"\w+\s*\.\s*eval", parts[2]) or local):
                bad.append("FRAME: presentJob's evaluate( must be fed the job's own inputs (<job>.eval, or a copy of it)")
        if not re.search(r"(?:\.|->)\s*evaluate\s*\(", body):
            bad.append("FRAME: presentJob never evaluates the generated frames (LLVKFrameGen's evaluate()")
    rb = _bodies(pt, "LLVKFrameGenPresent::runJob")
    if len(rb) != 1:
        bad.append("FRAME: %s must run each job in LLVKFrameGenPresent::runJob" % PT)
    else:
        body = _blank_lambdas(pt[rb[0][0]:rb[0][1]])
        pj, sr = body.find("presentJob("), body.rfind("signalRelease(")
        if pj < 0 or sr < pj or re.search(r"\breturn\b", body[:sr]):
            bad.append("FRAME: runJob must end every job with signalRelease( after presentJob( (no return before it): "
                       "a slot the renderer waits for is always released")
    # The present thread never waits for the whole device (it would race the main thread's queues).
    for m in re.finditer(r"\bvkDeviceWaitIdle\s*\(|\bsubmitImmediate\s*\(", pt):
        bad.append("FRAME: %s waits for the device or the graphics queue (line %d)" % (PT, pline(m.start())))

    # Every rebuild of the main swapchain stops the present thread first, then opens the gates.
    helper = _bodies(cx, "recreateMainSwapchain")
    if len(helper) != 1:
        bad.append("FRAME: recreateMainSwapchain must be defined once in %s" % CTX)
    else:
        a, b = helper[0]
        body = cx[a:b]
        d, r = body.find("drainFrameGen()"), body.find("mSwapchain.recreate(")
        if not (0 <= d < r):
            bad.append("FRAME: recreateMainSwapchain must call drainFrameGen() before mSwapchain.recreate(")
        pr = body.find("mFgPresent.prepare(")
        if pr < r:
            bad.append("FRAME: recreateMainSwapchain must prepare the present thread for the new chain "
                       "(mFgPresent.prepare( after mSwapchain.recreate()")
        for m in re.finditer(r"mSwapchain\s*\.\s*recreate\s*\(", cx):
            if not (a <= m.start() < b):
                bad.append("FRAME: mSwapchain.recreate( outside recreateMainSwapchain (line %d)" % line(m.start()))
    drain = _bodies(cx, "drainFrameGen")
    if len(drain) != 1:
        bad.append("FRAME: Impl::drainFrameGen must be defined once in %s" % CTX)
    else:
        body = cx[drain[0][0]:drain[0][1]]
        h, d, g = body.find("mFgPresent.drain()"), body.find("drainPacer()"), body.find("gatesReleased()")
        if not (0 <= h < d < g):
            bad.append("FRAME: drainFrameGen must drain the present thread (mFgPresent.drain()), then the pacer "
                       "(drainPacer()), then gatesReleased()")
    shut = _function(cx, "void LLVulkanContext::shutdown(") or ""
    d = shut.find("drainFrameGen()")
    sp = shut.find("mFgPresent.shutdown(")
    for later in ("shutdownDirect(", "mSwapchain.destroy("):
        p = shut.find(later)
        if d < 0 or p < 0 or p < d:
            bad.append("FRAME: LLVulkanContext::shutdown must call drainFrameGen() before %s" % later)
        if sp < 0 or p < 0 or p < sp:
            bad.append("FRAME: LLVulkanContext::shutdown must stop the present thread (mFgPresent.shutdown() before %s"
                       % later)
    # (a device wait while the present thread exists: rule STALL)

    # The generator gets the world without the interface: fgHudless, never fgUi or a swapchain image,
    # and writes the present thread's scratch image.
    fill = _bodies(cx, "fillGenInputs")
    if len(fill) != 1:
        bad.append("FRAME: %s must fill the evaluate's inputs in one function, fillGenInputs" % CTX)
    else:
        body = cx[fill[0][0]:fill[0][1]]
        for field in ("color",):   # (#1544: one reference -- image, view, format and extent together)
            m = re.search(r"\.%s\s*=\s*([^;]*);" % field, body)
            if not m or "fgHudless(" not in m.group(1):
                bad.append("FRAME: fillGenInputs's %s must be fgHudless()'s" % field)
        for word in ("fgUi(", "imageView(", "mSwapchain", "uiLayer(", "backbuffer("):
            if word in body:
                bad.append("FRAME: fillGenInputs touches %s (the generator never sees the interface or a "
                           "swapchain image)" % word.rstrip("("))
        m = re.search(r"\.output\s*=\s*([^;]*);", body)
        if not m or "fgScratch()" not in m.group(1):
            bad.append("FRAME: fillGenInputs's output must be the present thread's scratch image (fgScratch())")
    if not re.search(r"\.\s*eval\s*=\s*fillGenInputs\s*\(", cx):
        bad.append("FRAME: the job's generator inputs (job.eval) must come from fillGenInputs")
    for m in re.finditer(r"(?:\.|->)\s*evaluate\s*\(", cx):
        bad.append("FRAME: %s calls evaluate( (line %d): the present thread evaluates the generated frames"
                   % (CTX, line(m.start())))

    # The frame's submits: frame-done on the last one (the present thread waits it), and the wait for the
    # present thread's release of this slot (it read its backbuffer and inputs) before the first.
    sb = rf.find("VkSemaphoreSubmitInfo waitSem")
    single = rf[sb:rf.find("vkQueueSubmit2(", sb)] if sb >= 0 else ""
    for n in ("wsi.frameDone", "wsi.released"):
        if n not in single:
            bad.append("FRAME: renderFrame's single submit does not carry %s" % n)
    rv_raw = _raw(ctx, REN) or ""
    rh_raw = _raw(ctx, "indra/newview/vkgfx/llcleanrenderervk.h") or ""
    rv = _mask(rv_raw, blank_strings=True)
    for sig, names in (("bool LLCleanRendererVK::asyncSubmitFrame(", ("wsi.frameDone", "wsi.released")),
                       ("bool LLCleanRendererVK::submitEmptyFrame(", ("wsi.frameDone",))):
        fb = _function(rv, sig) or ""
        for n in names:
            if n not in fb:
                bad.append("FRAME: %s does not carry %s" % (sig.split("::")[1].rstrip("("), n))
    for field, src in (("released", r"mFgPresent\s*\.\s*released\s*\(\s*\)"),
                       ("releasedValue", r"mFgPresent\s*\.\s*releasedValue\s*\(\s*fgSlot\s*\)")):
        sets = [m for m in re.finditer(r"\bwsi\s*\.\s*%s\s*=\s*([^;]*);" % field, rf)]
        if len(sets) != 1 or not re.search(src, sets[0].group(0)):
            bad.append("FRAME: renderFrame must set wsi.%s once, from the present thread's release of this slot" % field)
            continue
        first_submit = rf.find("asyncSubmitFrame(")
        if not any(re.search(r"^if\s*\(\s*fgChain\b", h) for h in _enclosing_heads(rf, sets[0].start())) \
                or first_submit < 0 or sets[0].start() > first_submit:
            bad.append("FRAME: wsi.%s must be set on a frame generation chain (if (fgChain ...)), before the frame's "
                       "submits" % field)
    # What the present thread replaced is gone (Task 7b's late composites, the in-frame generated images).
    leftovers = ("extraAcquire", "extraRenderFinished", "lateDone", "mFgLate", "fgLateSlotFree", "ensureFgLate",
                 "releaseFgLate", "kFgLateSlots", "acquireExtra", "mExtraAcquire", "fgInterp", "kMaxFgInterp",
                 "presentFrameGenDirect", "recordFrameGenDirect", "trimFrameGen", "FgBatch")
    for rel in (CTX, REN, "indra/newview/vkgfx/llcleanrenderervk.h", DF_H, DF, SWAP_H, SWAP, FG_H, FG):
        code = _mask(_raw(ctx, rel) or "", blank_strings=True)
        for word in leftovers:
            if re.search(r"\b%s\b" % word, code):
                bad.append("FRAME: %s still names %s (replaced by the present thread)" % (rel, word))
    # The slots: one composite descriptor slot of the encode per slot of the present thread (whose images --
    # backbuffer and generator inputs -- are its own).
    if not re.search(r"static_assert\s*\([^;]*LLVKFrameGenPresent::kMaxSlots\s*<=\s*VkDisplayEncode::kSlots", cx):
        bad.append("FRAME: a static_assert must keep LLVKFrameGenPresent::kMaxSlots <= VkDisplayEncode::kSlots")
    # A frame that cannot be generated says why (the pre-check, before the frame's slot is taken).
    dd = rf.find("decideDirect(")
    ws = rf.find("waitSlot(")
    if dd < 0 or ws < 0 or "directRefused(" not in rf[dd:ws]:
        bad.append("FRAME: renderFrame's pre-check (between decideDirect( and the slot wait) must say why it refuses "
                   "(directRefused()")
    rec = _bodies(cx, "recordFrameGenInputs")
    if len(rec) != 1 or "directRefused(" not in cx[rec[0][0]:rec[0][1]]:
        bad.append("FRAME: %s must copy the generator's inputs in one helper, recordFrameGenInputs, which says why a "
                   "frame has none (directRefused()" % CTX)
    fse = _bodies(cx, "fseRelease")
    for a, b in fse:
        body = cx[a:b]
        d, r = body.find("drainFrameGen()"), body.find("mFseRelease(")
        if not (0 <= d < r):
            bad.append("FRAME: fseRelease must call drainFrameGen() before releasing exclusive mode")
    # Every composite's result is checked (the render loop's and the present thread's).
    for text, rel in ((cx, CTX), (pt, PT)):
        for m in re.finditer(r"recordFgComposite\s*\(", text):
            head = text[max(0, m.start() - 40):m.start()]
            if not re.search(r"(?:!|=|\bif\s*\()\s*(?:\w+\s*(?:\.|->)\s*)*$", head):
                bad.append("FRAME: recordFgComposite( in %s (line %d): its result is not checked"
                           % (rel, text.count("\n", 0, m.start()) + 1))
    # The chain: a batch needs generated + 1 images (each taken just before it is shown; MAILBOX / FIFO ask for
    # one more: framegen_linux.py); a higher multiplier than the chain was built for rebuilds it.
    pol = _code(_raw(ctx, POLICY_H) or "", blank_strings=True)
    sf = re.search(r"uint32_t\s+swapchainImagesNeeded\s*\(\s*uint32_t\s+(\w+)\s*\)\s*\{\s*return\s+([^;]*);", pol)
    if not sf or re.sub(r"\s+", "", sf.group(2)) not in ("%s+1" % sf.group(1), "1+%s" % sf.group(1)):
        bad.append("FRAME: llfg::swapchainImagesNeeded(g) must be g + 1 (each image is taken just before it is shown)")
    want = _bodies(cx, "wantFrameGenChain")
    if len(want) != 1 or not re.search(r"frameGenMax\s*\(\s*\)", cx[want[0][0]:want[0][1]]):
        bad.append("FRAME: wantFrameGenChain must rebuild a chain built for fewer generated frames "
                   "(VkSwapchain::frameGenMax())")
    # Vsync returns when generation cannot run: the chain follows the lasting conditions.
    fgc = _mask(_raw(ctx, FG) or "", blank_strings=False)
    cg = _function(fgc, "uint32_t LLVKFrameGen::chainGenerated(") or ""
    if ('"RenderVKUpscaler"' not in cg or not re.search(r"chainGenerated\(\s*bool\s+outputGenerates\s*\)", fgc)
            or not re.search(r"\boutputGenerates\b", cg)):
        bad.append("FRAME: LLVKFrameGen::chainGenerated(bool outputGenerates) must also need the DLSS upscaler choice "
                   "(RenderVKUpscaler) and an output frame generation runs on")
    cgc = [_call_args(rf, m.start()) for m in re.finditer(r"chainGenerated\s*\(", rf)]
    if not cgc or any(not re.search(r"llfg::outputGenerates\s*\(.*\.\s*output\b", a) for a in cgc):
        bad.append("FRAME: renderFrame must give chainGenerated( llfg::outputGenerates(the Context's output)")

    # A generator-neutral seam: the render loop and the present thread talk to LLVKFrameGen only.
    for rel, raw in ((CTX, raw_cx), (PT, raw_pt), (PT_H, _raw(ctx, PT_H) or "")):
        for m in re.finditer(r"slip_|NGX|ngx|DLSS|dlss", _mask(raw) if rel == CTX else raw):
            if rel == CTX and m.group(0).lower() == "dlss":
                continue   # (the render loop's DLSS upscaler; the rule is about the add-on's names)
            bad.append("FRAME: %s names the generator's add-on (%s, line %d)"
                       % (rel, m.group(0), raw.count("\n", 0, m.start()) + 1))
    for name in ("renderFrame", "recordFrameGenInputs", "fillGenInputs"):
        for a, b in _bodies(cx, name):
            for m in re.finditer(r"slip_|NGX|ngx", raw_cx[a:b]):
                bad.append("FRAME: %s names the generator's add-on (%s, line %d)" % (name, m.group(0), line(a + m.start())))


def _rule_present(ctx, bad):
    """Task 7c fix round 1: the rebuild request, the image admission, the statistics and Reflex's markers."""
    raw_cx = _raw(ctx, CTX) or ""
    cx = _mask(raw_cx, blank_strings=True)
    line = lambda pos: cx.count("\n", 0, pos) + 1
    pt = _mask(_raw(ctx, PT) or "", blank_strings=True)
    pth = _mask(_raw(ctx, PT_H) or "", blank_strings=True)
    frame = _bodies(cx, "renderFrame")
    rf = cx[frame[0][0]:frame[0][1]] if frame else ""

    # (C1) A rebuild request of the present thread is its frame generation chain's: the render loop acts on
    # it only on such a chain, and the presenter forgets it with the chain (and answers false when not ready).
    for m in re.finditer(r"\bmFgPresent\s*\.\s*needsRecreate\s*\(", cx):
        heads = _enclosing_heads(cx, m.start())
        cond = cx[cx.rfind("if", 0, m.start()):m.start()]
        if "frameGenImages()" not in cond and not any("fgChain" in h or "frameGenImages()" in h for h in heads):
            bad.append("PRESENT: mFgPresent.needsRecreate() (line %d) must be acted on only on a frame generation "
                       "chain (frameGenImages() / fgChain in its condition)" % line(m.start()))
    if not re.search(r"bool\s+needsRecreate\s*\(\s*\)\s*const\s*\{\s*return\s+mReady\s*&&", pth):
        bad.append("PRESENT: LLVKFrameGenPresent::needsRecreate() must answer only for the chain it was prepared for "
                   "(return mReady && ...)")
    for fn in ("void LLVKFrameGenPresent::releaseChain(", "bool LLVKFrameGenPresent::stop("):
        body = _function(pt, fn) or ""
        if not re.search(r"mNeedsRecreate\s*\.\s*store\s*\(\s*false\s*\)", body):
            bad.append("PRESENT: %s) must forget the rebuild request (mNeedsRecreate.store(false))" % fn)

    # (I2) Frame generation's images are admitted against the device's free video memory (frameGenBudget:
    # the driver's live budget with the large-request margin), as one set, never the HDR image budget.
    fbody = _function(cx, "LLVKFrameGenPresent::Budget frameGenBudget(") or ""
    if "liveDeviceBudget(" not in fbody or "vkLargeRequestMargin(" not in fbody:
        bad.append("PRESENT: Impl::frameGenBudget must measure the driver's live budget (liveDeviceBudget() with "
                   "the large-request margin (vkLargeRequestMargin()")
    for name in ("prepare", "prepareInputs"):
        calls = list(re.finditer(r"\bmFgPresent\s*\.\s*%s\s*\(" % name, cx))
        if not calls:
            bad.append("PRESENT: %s never calls mFgPresent.%s(" % (CTX, name))
        for m in calls:
            if "frameGenBudget()" not in _call_args(cx, m.start()):
                bad.append("PRESENT: mFgPresent.%s( (line %d) must be admitted against frameGenBudget()" % (name, line(m.start())))
    for m in re.finditer(r"\bmFgPresent\s*\.\s*\w+\s*\(", cx):
        if "mHdrBudget" in _call_args(cx, m.start()):
            bad.append("PRESENT: frame generation's images are charged to the HDR budget (line %d)" % line(m.start()))
    df = _mask((_raw(ctx, DF_H) or "") + (_raw(ctx, DF) or ""), blank_strings=True)
    for word in ("kFgSlots", "prepareFrameGen", "frameGenFits", "releaseFrameGen", "fgHudless", "fgScratch", "mFg"):
        if re.search(r"\b%s\b" % word, df):
            bad.append("PRESENT: vk_display_frame.* still holds %s (frame generation's images are the present "
                       "thread's, outside the HDR budget)" % word)
    if not re.search(r"\bpresentSlotsFor\s*\(", pt):
        bad.append("PRESENT: LLVKFrameGenPresent::prepare must admit its slots as one set (llfg::presentSlotsFor()")
    # (I2b / Minor 9) A refused set is retried, never per frame and never "off for the session".
    rb = _bodies(cx, "recreateMainSwapchain")
    rbody = cx[rb[0][0]:rb[0][1]] if rb else ""
    if "directFailed(" in rbody or "imagesRefused(" not in rbody:
        bad.append("PRESENT: recreateMainSwapchain must turn a refused prepare into imagesRefused( (tried again "
                   "later), not directFailed( (off for the session)")
    if not re.search(r"if\s*\(\s*mFgRefusalRebuild\s*\)[^;]*;\s*else\s+[^;]*rearmImages\s*\(", rbody):
        bad.append("PRESENT: recreateMainSwapchain must re-arm frame generation (rearmImages() on every rebuild but "
                   "the one its refusal asked for (mFgRefusalRebuild)")
    cg = _function(_mask(_raw(ctx, FG) or ""), "uint32_t LLVKFrameGen::chainGenerated(") or ""
    if "imagesRefusedNow()" not in cg:
        bad.append("PRESENT: chainGenerated must give a normal chain while the images are refused (imagesRefusedNow()")

    # (I3) The 5 s line shows the renderer's slot waits and the present thread's present calls.
    ws = rf.find("waitSlot(")
    if ws < 0 or not re.search(r"\bnoteSlotWait\s*\(", rf[ws:ws + 600]):
        bad.append("PRESENT: renderFrame must time its waitSlot( for the 5 s line (noteSlotWait()")
    ld = _function(_mask(_raw(ctx, FG) or "", blank_strings=False), "void LLVKFrameGen::logDirectLine(") or ""
    if "slot wait avg" not in ld or "present calls avg" not in ld:
        bad.append("PRESENT: the 5 s line must show the slot waits and the present calls (average / worst)")

    # (Reflex) The present thread's work is out of band under the frame's id: the job carries it, the generate
    # submit sits between the render-submit markers, every present call between the present markers.
    push = re.search(r"\bmFgPresent\s*\.\s*push\s*\(", rf)
    rfj = re.search(r"\.\s*reflexFrame\s*=\s*\w+\s*\.\s*reflexFrame\s*\(\s*\)", rf)
    if not push or not rfj or rfj.start() > push.start():
        bad.append("PRESENT: renderFrame must give the job its frame's Reflex id (<job>.reflexFrame = ...reflexFrame() "
                   "before mFgPresent.push(")
    jb = _bodies(pt, "LLVKFrameGenPresent::presentJob")
    body = pt[jb[0][0]:jb[0][1]] if jb else ""
    rs, re_ = body.find("OobRenderStart"), body.find("OobRenderEnd")
    acqs = _wrapped_calls(body, "acquireOne")
    acq = acqs[0] if acqs else -1
    sub = [m.start() for m in re.finditer(r"(?<![\w.>])submit\s*\(", body)]
    if not (0 <= rs < acq) or not sub or not (sub[0] < re_):
        bad.append("PRESENT: presentJob must send OobRenderStart before its first acquireOne( and OobRenderEnd after "
                   "the generated frames' submit(")
    for m in re.finditer(r"\bpresentBatch\s*\(", body):
        before, after = body[max(0, m.start() - 400):m.start()], body[m.end():m.end() + 400]
        if "OobPresentStart" not in before or "OobPresentEnd" not in after:
            bad.append("PRESENT: presentJob's presentBatch( must sit between OobPresentStart and OobPresentEnd")


APP = "indra/newview/llappviewer.cpp"
NV_LATENCY = ("vkLatencySleepNV", "vkSetLatencyMarkerNV", "vkSetLatencySleepModeNV", "vkQueueNotifyOutOfBandNV")


def _rule_reflex(ctx, bad):
    # The NV entry points: only llvkframegen.cpp names them, only on frame generation's platforms, only to look them up.
    names = re.compile(r"\b(%s)\b" % "|".join(NV_LATENCY))
    tree = ctx.path("indra/newview")
    for root, _dirs, files in os.walk(tree):
        for f in files:
            if not f.endswith((".cpp", ".h", ".hpp", ".inl", ".mm")):
                continue
            p = os.path.join(root, f)
            rel = os.path.relpath(p, ctx.path("."))
            if rel == FG or "third_party" in rel.split(os.sep):
                continue
            t = _code(open(p, encoding="utf-8", errors="replace").read())
            for m in names.finditer(t):
                bad.append("REFLEX: %s names %s (only llvkframegen.cpp may)" % (rel, m.group(1)))
    raw = _raw(ctx, FG) or ""
    c = _code(raw)                      # strings kept: the names handed to vkGetDeviceProcAddr
    cb = _code(raw, blank_strings=True)  # strings blanked: a call of the loader's prototype
    spans = _windows_spans(c)
    for n in NV_LATENCY:
        hits = [m for m in re.finditer(r"\b%s\b" % n, c)]
        if not hits:
            bad.append("REFLEX: %s never looks up %s" % (FG, n))
        for m in hits:
            if not _inside(spans, m.start()):
                bad.append("REFLEX: %s names %s outside %s (line %d)" % (FG, n, PLATFORM_GATE, c.count("\n", 0, m.start()) + 1))
            head = c[max(0, m.start() - 80):m.start()]
            if not re.search(r"vkGetDeviceProcAddr\s*\(\s*[\w.()]+\s*,\s*\"$", head):
                bad.append("REFLEX: %s: %s is not obtained through vkGetDeviceProcAddr (line %d)"
                           % (FG, n, c.count("\n", 0, m.start()) + 1))
        for m in re.finditer(r"\b%s\s*\(" % n, cb):
            bad.append("REFLEX: %s calls the loader's %s( (line %d): use the pointer from vkGetDeviceProcAddr"
                       % (FG, n, cb.count("\n", 0, m.start()) + 1))
    log = _function(c, "void LLVKFrameGen::logDirectLine(") or ""
    if "Reflex" not in log:
        bad.append("REFLEX: the 5 s direct line (logDirectLine) must say whether Reflex is on")

    # The main loop: Reflex paces a frame only where reflexCapsFrames().
    app = _code(_raw(ctx, APP) or "", blank_strings=True)
    fp = re.search(r"const bool fg_paced\s*=([^;]*);", app)
    if not fp or not re.search(r"LLVKFrameGen::instance\(\)\.reflexCapsFrames\(\)", fp.group(1)) \
            or "?" in fp.group(1):
        bad.append("REFLEX: llappviewer.cpp's fg_paced must require LLVKFrameGen::instance().reflexCapsFrames() "
                   "(Reflex paces only where it caps the frames)")
    lim = re.search(r"S32\s+limit\s*=\s*([^;]*);", app)
    # The swapchain Reflex talks to: registered after every (re)creation, dropped before it goes.
    cx = _code(_raw(ctx, CTX) or "", blank_strings=True)
    helper = _bodies(cx, "recreateMainSwapchain")
    if helper:
        body = cx[helper[0][0]:helper[0][1]]
        r = body.find("mSwapchain.recreate(")
        regs = [m.start() for m in re.finditer(r"setReflexSwapchain\s*\(", body)]
        if r < 0 or not any(p < r for p in regs) or not any(p > r for p in regs):
            bad.append("REFLEX: recreateMainSwapchain must call setReflexSwapchain( before (VK_NULL_HANDLE) and after "
                       "mSwapchain.recreate(")
    init = _function(cx, "bool LLVulkanContext::init(") or ""
    si = init.find("mSwapchain.init(")
    if si < 0 or not re.search(r"setReflexSwapchain\s*\(", init[si:]):
        bad.append("REFLEX: LLVulkanContext::init must call setReflexSwapchain( after mSwapchain.init(")
    if si < 0 or not re.search(r"setLatencyMode\s*\([^;]*reflexAvailable\s*\(\s*\)", init[:si]):
        bad.append("REFLEX: LLVulkanContext::init must set the main swapchain's low latency mode from "
                   "reflexAvailable() before mSwapchain.init(")
    shut = _function(cx, "void LLVulkanContext::shutdown(") or ""
    rs = shut.find("setReflexSwapchain(")
    for later in ("shutdownDirect(", "mSwapchain.destroy("):
        p = shut.find(later)
        if rs < 0 or p < 0 or p < rs:
            bad.append("REFLEX: LLVulkanContext::shutdown must call setReflexSwapchain( before %s" % later)
    sw = _code(_raw(ctx, SWAP) or "", blank_strings=True)
    cso = _function(sw, "bool VkSwapchain::createSwapchainObjects(") or ""
    if "VkSwapchainLatencyCreateInfoNV" not in cso or "VkSwapchainLatencyCreateInfoNV" in sw.replace(cso, ""):
        bad.append("REFLEX: VkSwapchainLatencyCreateInfoNV must be chained in VkSwapchain::createSwapchainObjects only")

    # (fix round 1) Low latency mode only on a chain built for frame generation (setFrameGen's wish, `fg`),
    # and a creation refused with it is tried once more without it.
    lat_vars = [m for m in re.finditer(r"const\s+bool\s+(\w+)\s*=\s*([^;]*);", cso)
                if "mLatencyWanted" in m.group(2)]
    if not lat_vars or not re.search(r"\bfg\b", lat_vars[0].group(2)):
        bad.append("REFLEX: createSwapchainObjects must chain low latency mode only on a chain built for frame "
                   "generation (const bool ... = mLatencyWanted && fg ...)")
    if not re.search(r"if\s*\(\s*cr\s*!=\s*VK_SUCCESS\s*&&\s*ci\.pNext\s*==\s*&latency\b", cso):
        bad.append("REFLEX: a swapchain refused with low latency mode must be created once more without it "
                   "(if (cr != VK_SUCCESS && ci.pNext == &latency ...))")
    # (fix round 1) recreateMainSwapchain lets go of the old chain first.
    if helper:
        body = cx[helper[0][0]:helper[0][1]]
        first = re.search(r"setReflexSwapchain\s*\(\s*([^,)]*)", body)
        if not first or first.group(1).strip() != "VK_NULL_HANDLE":
            bad.append("REFLEX: recreateMainSwapchain's first setReflexSwapchain( must pass VK_NULL_HANDLE")
    # (fix round 1) The sleep's wait is bounded by a finite constant, and Reflex steps aside for good after
    # a few sleeps the driver did not answer.
    cbm = _code(raw, blank_strings=True)
    sleep = _function(cbm, "void LLVKFrameGen::reflexSleep(") or ""
    waits = [_call_args(sleep, m.start()) for m in re.finditer(r"vkWaitSemaphores\s*\(", sleep)]
    consts = dict(re.findall(r"constexpr\s+(?:uint64_t|int64_t|uint32_t)\s+(\w+)\s*=\s*([^;]*);",
                             cbm + _code(_raw(ctx, FG_H) or "", blank_strings=True)))
    if len(waits) != 1:
        bad.append("REFLEX: reflexSleep must wait its semaphore once (vkWaitSemaphores)")
    else:
        t = waits[0].split(",")[-1].strip()
        val = consts.get(t, t)
        if not re.fullmatch(r"[\d][\d'\s*()]*[uUlL]*", val.strip()):
            bad.append("REFLEX: reflexSleep's vkWaitSemaphores timeout must be a finite constant (found %s = %s)" % (t, val))
    brk = re.search(r"(\w+)\s*>=\s*(k\w+)", sleep)
    hdr = _code(_raw(ctx, FG_H) or "", blank_strings=True)
    dr = re.search(r"bool\s+directReflex\s*\(\s*\)\s*const\s*\{([^}]*)\}", hdr)
    flag = None
    if brk:
        f = re.search(r"(m\w+)\s*=\s*true\s*;", sleep[brk.end():brk.end() + 300])
        flag = f.group(1) if f else None
    if not brk or not flag or not dr or not re.search(r"!\s*%s\b" % flag, dr.group(1)):
        bad.append("REFLEX: reflexSleep needs a timeout breaker (a counter >= a constant sets an off flag) that "
                   "directReflex() / reflexCapsFrames() reads")
    if flag and (flag not in log or "did not answer" not in _code(raw)[_code(raw).find("void LLVKFrameGen::logDirectLine("):]):
        bad.append("REFLEX: the 5 s line must say Reflex is off because the driver did not answer")
    if not re.search(r"\bavg\b|average", _code(raw)[_code(raw).find("void LLVKFrameGen::logDirectLine("):]) \
            or "worst" not in log:
        bad.append("REFLEX: the 5 s line must give the Reflex sleep's average and worst wait")
    # (fix round 1) The CPU limiter steps aside on the top-of-frame decision (the same frame's).
    latched = re.findall(r"\b(\w+)\s*=\s*fg_paced\s*;", app)
    if not lim or not latched or not re.search(r"\b(%s)\b" % "|".join(latched), lim.group(1)) \
            or "reflexCapsFrames" in lim.group(1):
        bad.append("REFLEX: the CPU limiter's foreground cap must step aside on the pacing branch's own decision "
                   "(a variable latched = fg_paced), never on a later reflexCapsFrames()")
    # (fix round 1) The sleep's semaphore is waited (bounded) before it is destroyed.
    sd = _function(cbm, "void LLVKFrameGen::shutdownDirect(") or ""
    w, d = sd.find("vkWaitSemaphores("), sd.find("vkDestroySemaphore(d, mReflexSem")
    if w < 0 or d < 0 or w > d:
        bad.append("REFLEX: shutdownDirect must wait the Reflex semaphore's last value (bounded) before destroying it")


PREFS = "indra/newview/llfloaterpreferencesgraphicsadvanced.cpp"
PREFS_XUI = "indra/newview/skins/default/xui/en/floater_preferences_graphics_advanced.xml"
STATUSBAR = "indra/newview/llstatusbar.cpp"
# Frame generation is live and has one route: no frame generation text names a restart or another route.
RESTART_WORDS = re.compile(r"restart|streamline", re.I)


def _block_after(text, pos):
    """(start, end) of the brace-matched block whose '{' is the first one at or after pos."""
    start = text.find("{", pos)
    if start < 0:
        return None
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return (start, i + 1)
    return None


def _rule_ui(ctx, bad):
    """UI: Preferences and the status bar (Task 9, one route since Task 11)."""
    xui = _raw(ctx, PREFS_XUI) or ""
    combo = re.search(r'<combo_box\b[^>]*\bname="FrameGen"[^>]*>', xui, flags=re.S)
    if not combo or RESTART_WORDS.search(combo.group(0)):
        bad.append("UI: the FrameGen combo's tooltip must name neither a restart nor another route")
    if 'name="FrameGenRestart"' in xui:
        bad.append("UI: the FrameGenRestart note is gone (frame generation is live)")
    strings = dict(re.findall(r'<string\s+name="(\w+)"\s*>([^<]*)</string>', xui))
    for name, text in sorted(strings.items()):
        if (name.startswith("framegen_") or name == "vsync_framegen") and RESTART_WORDS.search(text):
            bad.append("UI: floater string %s names a restart or another route" % name)
    # (Task 11b fix 1, M8) the vsync note is true on every platform and present mode: frame generation sets VSync
    # while it runs (IMMEDIATE has none, MAILBOX / FIFO do), and no vendor's control panel or display sync is named.
    vs = strings.get("vsync_framegen", "")
    if not vs or re.search(r"G-?Sync|Control Panel|NVIDIA|no VSync", vs, flags=re.I) \
            or not re.search(r"frame generation", vs, flags=re.I):
        bad.append("UI: the vsync_framegen string must say frame generation sets VSync while it runs, and name no "
                   "vendor's control panel or display sync (it shows on Linux and on MAILBOX / FIFO chains too)")

    panel_all = _code(_raw(ctx, PREFS) or "")
    rf = _bodies(panel_all, "LLFloaterPreferenceGraphicsAdvanced::refreshFrameGen")
    panel = panel_all[rf[0][0]:rf[0][1]] if rf else ""
    if not rf:
        bad.append("UI: %s needs refreshFrameGen() holding the vsync and FrameGen blocks" % PREFS)
    res = _bodies(panel_all, "LLFloaterPreferenceGraphicsAdvanced::refreshEnabledState")
    if not res or "refreshFrameGen()" not in panel_all[res[0][0]:res[0][1]]:
        bad.append("UI: refreshEnabledState() must call refreshFrameGen()")
    ufd = _bodies(panel_all, "LLFloaterPreferenceGraphicsAdvanced::updateForDraw")
    body = panel_all[ufd[0][0]:ufd[0][1]] if ufd else ""
    tick = re.search(r"if\s*\(\s*mVramTextTimer\.getElapsedTimeF32\(\)\s*>=\s*1\.f\s*\)", body)
    tblk = _block_after(body, tick.end()) if tick else None
    if not tblk or "refreshFrameGen()" not in body[tblk[0]:tblk[1]]:
        bad.append("UI: updateForDraw() must call refreshFrameGen() on the 1 s mVramTextTimer tick (the status line)")
    if not re.search(r"chainForcesNoVsync\(\)\s*!=\s*m\w+", body) or body.count("refreshFrameGen()") < 2:
        bad.append("UI: updateForDraw() must call refreshFrameGen() when chainForcesNoVsync() differs from what the "
                   "checkbox shows (a cached member)")
    m = re.search(r'if\s*\(\s*LLComboBox\s*\*\s*fgc\s*=\s*findChild<LLComboBox>\(\s*"FrameGen"\s*\)\s*\)', panel)
    blk = _block_after(panel, m.end()) if m else None
    if not blk:
        bad.append("UI: %s has no FrameGen combo block" % PREFS)
    else:
        block = panel[blk[0]:blk[1]]
        for need in ("directReady()", "multiplierOffered(", "startupBlock()", "blockText(", "reason()",
                     "shownPerRendered()", "statusText()", "driverPacing()", "reflexCapsFrames()"):
            if need not in block:
                bad.append("UI: the FrameGen block must read %s" % need)
        sets = re.findall(r"fgc->setEnabled\(([^;]*)\);", block)
        if [x.strip() for x in sets] != ["fg.directReady()"]:
            bad.append("UI: the combo is enabled exactly when directReady()")
        if "FrameGenRestart" in block or "framegen_restart" in block:
            bad.append("UI: the FrameGen block still shows a restart note")
    m = re.search(r'if\s*\(\s*LLCheckBoxCtrl\s*\*\s*vs\s*=\s*findChild<LLCheckBoxCtrl>\(\s*"vsync"\s*\)\s*\)', panel)
    vblk = _block_after(panel, m.end()) if m else None
    vs = panel[vblk[0]:vblk[1]] if vblk else ""
    if "chainForcesNoVsync()" not in vs:
        bad.append("UI: the vsync checkbox is greyed by chainForcesNoVsync()")
    if re.search(r"chainGenerated|RenderVKUpscaler|hdr10|maxGenerated", vs):
        bad.append("UI: the vsync checkbox must not recompute the chain's condition (LLVKFrameGen::chainForcesNoVsync)")
    fgh = _code(_raw(ctx, FG_H) or "")
    cf = re.search(r"bool\s+chainForcesNoVsync\(\)\s*const\s*\{([^}]*)\}", fgh)
    fgc = _code(_raw(ctx, FG) or "")
    srs = _bodies(fgc, "LLVKFrameGen::setReflexSwapchain")
    members = re.findall(r"\bm[A-Z]\w*", cf.group(1)) if cf else []
    assigned = [x for x in members if srs and re.search(r"\b%s\s*=[^=]" % x, fgc[srs[0][0]:srs[0][1]])]
    if not cf or not assigned:
        bad.append("UI: llvkframegen.h needs an inline chainForcesNoVsync() const: the main chain as built (a member "
                   "setReflexSwapchain sets after every (re)creation)")
    swh = _code(_raw(ctx, SWAP_H) or "")
    if not re.search(r"bool\s+ignoresVsync\(\)\s*const", swh):
        bad.append("UI: vk_swapchain.h must declare bool ignoresVsync() const (the present mode chosen for frame "
                   "generation)")
    if srs and not re.search(r"\bm[A-Z]\w*\s*=[^;]*\bignoresVsync\b", fgc[srs[0][0]:srs[0][1]]):
        bad.append("UI: setReflexSwapchain must set chainForcesNoVsync's member from its ignoresVsync parameter")
    cx = _code(_raw(ctx, CTX) or "", blank_strings=True)
    calls = [m.end() for m in re.finditer(r"setReflexSwapchain\s*\(", cx)]
    for at in calls:
        depth, i = 1, at
        while i < len(cx) and depth:
            depth += {"(": 1, ")": -1}.get(cx[i], 0)
            i += 1
        args = cx[at:i - 1]
        if not args.strip().startswith("VK_NULL_HANDLE") and "ignoresVsync()" not in args:
            bad.append("UI: llvulkancontext.cpp's setReflexSwapchain(%s) must pass the chain's ignoresVsync()"
                       % " ".join(args.split())[:40])
    sb = _code(_raw(ctx, STATUSBAR) or "")
    m = re.search(r"\bmult\s*=([^;]*);", sb)
    if not m or not re.search(r"^\s*\(\s*fg\.directReady\(\)\s*&&\s*fg\.multiplierOn\(\)\s*\)\s*\?", m.group(1)):
        bad.append("UI: the status bar's multiplier must be fed by the presented batches, and only while the setting "
                   "asks for frames ((fg.directReady() && fg.multiplierOn()) ? ... : 1)")


HDR_UI_CONTRACT = "indra/newview/tests/hdr_ui_layer_contract.py"


def _rule_hdr_ui(ctx, bad):
    # Frame generation shares the display encode's shader with the normal HDR path: the HDR interface
    # layer's own contract must keep holding.
    import subprocess
    import sys
    script = ctx.path(HDR_UI_CONTRACT)
    if not os.path.exists(script):
        bad.append("HDR-UI: %s is missing" % HDR_UI_CONTRACT)
        return
    cxx = os.environ.get("CXX") or "c++"
    try:
        r = subprocess.run([sys.executable, "-I", script, "--compiler", cxx], cwd=ctx.path("."),
                           capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.TimeoutExpired) as e:
        bad.append("HDR-UI: %s could not run: %s" % (HDR_UI_CONTRACT, e))
        return
    if r.returncode != 0:
        tail = " | ".join((r.stdout + r.stderr).strip().splitlines()[-4:])
        bad.append("HDR-UI: %s failed: %s" % (HDR_UI_CONTRACT, tail[:600]))


SELF = "scripts/check/checks/framegen_direct.py"
# The words the older frame generation route left behind. (The prefix "slip" keeps the colour
# SlipstreamLine; "streamlined" is plain English in upstream comments.)
NO_SL_GIT = "streamline|slshim|slipstream_sl|slip_fg_set_|setprocs|notetextinput|pausetyping|uiblend|tagui|framegendirect"
NO_SL_WORDS = re.compile(r"(?<!slip)streamline(?!d(?![a-z]))|slshim|slipstream_sl\b|slip_fg_set_|setProcs|noteTextInput"
                         r"|PauseTyping|UiBlend|TagUi|RenderVKFrameGenDirect", re.I)
# Code (comments stripped) under indra/newview: what only that route used.
LEFTOVER_NAMES = ("sessionActive", "decideFrame", "tagFrame", "skipFrame", "afterPresent", "typingHold",
                  "untaggedPresent", "Block::Typing", "QueueSlot", "FgRequest", "FgQueues", "FgProvider",
                  "setFrameGenProvider", "frameGenQueues", "libraryOverride", "nativeFeature",
                  "vkqDeviceWaitIdleVia", "vkSetDeviceWaitIdleProxy", "mProxied", "waitDeviceIdle", "SurfaceProcs",
                  "SwapchainProcs", "mSurfaceDestroy", "pauseForSwapchainChange", "resumeAfterSwapchainChange",
                  "productionBinaries", "mProcs")
XR = "indra/newview/vkgfx/llxrsession.cpp"
POLICY_H = "indra/newview/vkgfx/llvkframegenpolicy.h"


# The files the word bans look at (final fix C4) -- frame generation's own, and the files it is wired into.
# Code anywhere else (an upstream merge, another feature) may use the same words: "sessionActive" or
# "mProcs" in an unrelated file is not frame generation's leftover.
#   OWNED     written for frame generation: every ban applies.
#   RENDERER  this viewer's own renderer files the feature is wired into: every ban applies.
#   VIEWER    viewer files shared with upstream code: the older route's own words (NO-STREAMLINE) and
#             LLVKFrameGen's .direct(); not the generic names (LEFTOVERS), which are theirs to use.
OWNED_DIRS = ("contrib/ngxshim",)
OWNED_GLOBS = ("indra/newview/vkgfx/llvkframegen*", "indra/newview/vkgfx/llvkneuralrecon.*",
               "indra/newview/vkgfx/llvkaddonlock.h", "indra/newview/vkgfx/vk_frame_pacer.*",
               "indra/newview/vkgfx/vk_present_handoff.*", "indra/vkclean/shaders/clean_display_labels.glsl",
               "scripts/check/checks/framegen_*.py", "scripts/check/test_framegen_*", "scripts/check/test_frame_pacer.cpp",
               "scripts/check/test_present_handoff.cpp", "doc/frame-generation.md")
RENDERER_FILES = ("indra/newview/vkgfx/vk_device.h", "indra/newview/vkgfx/vk_device.cpp",
                  "indra/newview/vkgfx/vk_swapchain.h", "indra/newview/vkgfx/vk_swapchain.cpp",
                  "indra/newview/vkgfx/llvulkancontext.h", "indra/newview/vkgfx/llvulkancontext.cpp",
                  "indra/newview/vkgfx/vk_display_encode.h", "indra/newview/vkgfx/vk_display_encode.cpp",
                  "indra/newview/vkgfx/vk_display_frame.h", "indra/newview/vkgfx/vk_display_frame.cpp",
                  "indra/newview/vkgfx/llcleandlssrrvk.h", "indra/newview/vkgfx/llcleandlssrrvk.cpp",
                  "indra/newview/vkgfx/vk_hang_watchdog.h", "indra/newview/vkgfx/vk_display_mode_transaction.h",
                  "indra/vkclean/shaders/clean_display_output.frag")
VIEWER_FILES = ("indra/newview/llappviewer.cpp", "indra/newview/llappviewer.h", "indra/newview/llappviewerwin32.cpp",
                "indra/newview/llappviewerlinux.cpp", "indra/newview/llfloaterpreferencesgraphicsadvanced.cpp",
                "indra/newview/llfloaterpreferencesgraphicsadvanced.h", "indra/newview/llstatusbar.cpp",
                "indra/newview/slfloaterquickprefs.cpp", "indra/newview/app_settings/settings.xml",
                "indra/newview/skins/default/xui/en/floater_preferences_graphics_advanced.xml",
                "indra/newview/skins/default/xui/en/panel_status_bar.xml")


def _feature_files(ctx):
    """(own, viewer): the existing files of OWNED + RENDERER, and of VIEWER, relative to the checkout."""
    import glob
    own = set()
    for d in OWNED_DIRS:
        for root, _dirs, files in os.walk(ctx.path(d)):
            for f in files:
                own.add(os.path.relpath(os.path.join(root, f), ctx.path(".")).replace(os.sep, "/"))
    for g in OWNED_GLOBS:
        for p in glob.glob(ctx.path(g)):
            if os.path.isfile(p):
                own.add(os.path.relpath(p, ctx.path(".")).replace(os.sep, "/"))
    own.update(rel for rel in RENDERER_FILES if os.path.isfile(ctx.path(rel)))
    viewer = set(rel for rel in VIEWER_FILES if os.path.isfile(ctx.path(rel)))
    return sorted(own), sorted(viewer)


def _rule_words(ctx, bad):
    """NO-STREAMLINE and LEFTOVERS' word bans, on frame generation's files only (no git needed)."""
    if os.path.isdir(ctx.path("contrib/slshim")):
        bad.append("NO-STREAMLINE: contrib/slshim is back (README or code of the older route)")
    own, viewer = _feature_files(ctx)
    for rel in own + viewer:
        if rel == SELF:
            continue
        text = _raw(ctx, rel) or ""
        lines = [(i, l.strip()) for i, l in enumerate(text.splitlines(), 1) if NO_SL_WORDS.search(l)]
        if lines:
            shown = "; ".join("%d: %s" % (i, l[:90]) for i, l in lines[:2])
            bad.append("NO-STREAMLINE: %s (%d line(s)) %s" % (rel, len(lines), shown))
    # LEFTOVERS: the names only the older route used are gone from the code.
    names = re.compile(r"(?<![\w])(%s)\b" % "|".join(re.escape(n) for n in LEFTOVER_NAMES))
    for rel in own + viewer:
        if not rel.endswith((".cpp", ".h")) or not rel.startswith("indra/"):
            continue
        t = _code(_raw(ctx, rel) or "", blank_strings=True)
        found = sorted(set(m.group(1) for m in names.finditer(t))) if rel in own else []
        if "LLVKFrameGen" in t and re.search(r"\.\s*direct\s*\(\s*\)", t):
            found.append("direct()")
        if found:
            bad.append("LEFTOVERS: %s still uses %s" % (rel, ", ".join(found)))


def _rule_no_streamline(ctx, bad):
    """NO-STREAMLINE: frame generation has one route, the DLSS add-on's; nothing of the older one stays."""
    _rule_words(ctx, bad)
    fgh = _code(_raw(ctx, FG_H) or "", blank_strings=True)
    for pat, what in ((r"\bbool\s+direct\s*\(", "direct()"), (r"\bstartup\s*\(", "startup()"), (r"\bPaths\b", "Paths"),
                      (r"\brequestedMultiplier\s*\(", "requestedMultiplier()"), (r"\bstruct\s+Fns\b", "Fns")):
        if re.search(pat, fgh):
            bad.append("LEFTOVERS: %s still declares %s" % (FG_H, what))
    pol = _code(_raw(ctx, POLICY_H) or "", blank_strings=True)
    for pat, what in ((r"\btyping\b", "Context::typing"), (r"\bTyping\b", "Block::Typing"),
                      (r"\breflexCapsFrames\b", "llfg::reflexCapsFrames"), (r"\bkTypingHoldSec\b", "kTypingHoldSec")):
        if re.search(pat, pol):
            bad.append("LEFTOVERS: %s still has %s" % (POLICY_H, what))
    swh = _code(_raw(ctx, SWAP_H) or "", blank_strings=True)
    if re.search(r"\bstruct\s+Procs\b", swh):
        bad.append("LEFTOVERS: %s still declares VkSwapchain::Procs" % SWAP_H)


class _Tree:
    """A checkout-like tree for the scope self-test (only path() is used by the rule)."""

    def __init__(self, root):
        self.root = root

    def path(self, *parts):
        return os.path.join(self.root, *parts)


def _scope_selftest(bad):
    """The word bans look only at frame generation's files (final fix C4): unrelated code may use the
    same words, a feature file may not. Run on a small made-up tree on every run."""
    import tempfile
    files = {
        # unrelated code (an upstream merge, another feature): every banned word, never a finding
        "indra/newview/llunrelated.cpp": "bool sessionActive = false;\nvoid skipFrame();\nint mProcs; // streamline tagui\n"
                                         "void f() { afterPresent(); setProcs(); }\n",
        "indra/llcommon/llother.h": "struct QueueSlot { int UiBlend; };\n",
        "scripts/check/checks/other_check.py": "WORDS = 'streamline slshim'\n",
        # frame generation's own files and the renderer files it is wired into: clean
        "indra/newview/vkgfx/llvkframegen.cpp": "void LLVKFrameGen::initDirect() {}\n",
        "indra/newview/vkgfx/vk_swapchain.cpp": "void VkSwapchain::setFrameGen() {}\n",
        "indra/newview/llappviewer.cpp": "bool sessionActive = true;   // (viewer code: its own names are its own)\n",
        "contrib/ngxshim/src/slipstream_ngx.cpp": "int slip_nr_fg_caps();\n",
    }
    mutants = (
        ("indra/newview/vkgfx/llvkframegen.cpp", "#include \"sl_streamline.h\"\n", "NO-STREAMLINE"),
        ("indra/newview/vkgfx/llvkframegenpresent.cpp", "void f() { fg.tagFrame(); }\n", "LEFTOVERS"),
        ("indra/newview/vkgfx/vk_swapchain.cpp", "struct X { SwapchainProcs mProcs; };\n", "LEFTOVERS"),
        ("indra/newview/vkgfx/vk_device.cpp", "void VkDeviceContext::setFrameGenProvider() {}\n", "LEFTOVERS"),
        ("indra/newview/llappviewer.cpp", "void f() { LLVKFrameGen::instance().noteTextInput(); }\n", "NO-STREAMLINE"),
        ("indra/newview/app_settings/settings.xml", "<key>RenderVKFrameGenDirect</key>\n", "NO-STREAMLINE"),
        ("contrib/ngxshim/src/slipstream_ngx.cpp", "int slip_fg_set_procs();\n", "NO-STREAMLINE"),
        ("scripts/check/test_framegen_policy.cpp", "// the Streamline route\n", "NO-STREAMLINE"),
        ("contrib/slshim/README.md", "the older route\n", "NO-STREAMLINE"),
    )

    def write(root, rel, text):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)

    def findings(extra=None):
        with tempfile.TemporaryDirectory() as root:
            for rel, text in files.items():
                write(root, rel, text)
            if extra:
                write(root, extra[0], extra[1])
            out = []
            _rule_words(_Tree(root), out)
            return out

    clean = findings()
    if clean:
        bad.append("SCOPE self-test: the word bans flag code outside frame generation's files, or a clean tree: %s"
                   % "; ".join(clean)[:400])
    for rel, text, rule in mutants:
        got = findings((rel, text))
        if not any(g.startswith(rule) and rel.split("/")[-1].split(".")[0] in g for g in got):
            bad.append("SCOPE self-test: %s re-added to %s is not caught as %s (got: %s)"
                       % (text.strip()[:50], rel, rule, "; ".join(got)[:200] or "nothing"))


def _rule_carried(ctx, bad):
    """The rules of the older route's check that still hold, for the direct design."""
    cx = _code(_raw(ctx, CTX) or "", blank_strings=True)
    # MAIN-ONLY: only the main window's swapchain is built for frame generation, and only the main
    # window's frame presents a batch: never a torn-off floater window, never the VR session.
    ui = _bodies(cx, "renderUIWindow")
    for m in re.finditer(r"(?<![\w])setFrameGen\s*\(", cx):
        head = cx[max(0, m.start() - 12):m.start()]
        if not head.endswith("mSwapchain."):
            bad.append("MAIN-ONLY: %s calls setFrameGen( on a swapchain other than the main one (line %d)"
                       % (CTX, cx.count("\n", 0, m.start()) + 1))
        if _inside(ui, m.start()):
            bad.append("MAIN-ONLY: setFrameGen( inside renderUIWindow")
    rf = _bodies(cx, "renderFrame")
    calls = list(re.finditer(r"\bmFgPresent\s*\.\s*push\s*\(", cx))
    if not calls:
        bad.append("MAIN-ONLY: nothing hands a frame to the present thread (mFgPresent.push()")
    for m in calls:
        if not _inside(rf, m.start()):
            bad.append("MAIN-ONLY: mFgPresent.push( is called outside renderFrame (line %d)"
                       % (cx.count("\n", 0, m.start()) + 1))
    xr = _code(_raw(ctx, XR) or "", blank_strings=True)
    for n in ("setFrameGen", "acquireOne", "presentBatch", "mFgPresent"):
        if re.search(r"(?<![\w])%s\s*\(" % n, xr):
            bad.append("MAIN-ONLY: %s calls %s( (the VR session presents no generated frames)" % (XR, n))

    # MAIN-VIEW-ONLY: only frameGenView feeds frame generation's camera, and it excludes offscreen,
    # snapshot (live capture / SDR) and multi-view frames; the encode's frame generation outputs are
    # never asked for on an SDR snapshot frame.
    rr = _code(_raw(ctx, RR) or "", blank_strings=True)
    ren = _code(_raw(ctx, REN) or "", blank_strings=True)
    if not re.search(r"if\s*\(\s*f\.frameGenView\s*&&\s*LLVKFrameGen::instance\(\)\.directReady\(\)\s*\)\s*"
                     r"frameGenConstants\(", rr):
        bad.append("MAIN-VIEW-ONLY: frameGenConstants( must be guarded by f.frameGenView && "
                   "LLVKFrameGen::instance().directReady()")
    if len(re.findall(r"(?<![\w:])frameGenConstants\(f,", rr)) != 1:
        bad.append("MAIN-VIEW-ONLY: frameGenConstants is called once, before the evaluate")
    # (#1544: both upscalers' frames -- FSR's and DLSS's -- state it, each with every condition)
    fgv = re.findall(r"f\.frameGenView\s*=([^;]*);", ren)
    if len(fgv) != 2 or not all(all(k in g for k in ("v == 0", "mViewCount == 1", "!mOffscreen", "!mLiveCapNow", "!mSdrFrame"))
                                for g in fgv):
        bad.append("MAIN-VIEW-ONLY: f.frameGenView must require v == 0, mViewCount == 1, !mOffscreen, !mLiveCapNow, "
                   "!mSdrFrame")
    m = re.search(r"const bool fgFrame\s*=([^;]*);", cx)
    if not m or "!sdrFrame" not in m.group(1) or "frameGenInputs(" not in m.group(1):
        bad.append("MAIN-VIEW-ONLY: the encode's frame generation outputs need !sdrFrame and this frame's inputs "
                   "(frameGenInputs()")

    # MARKERS: one call per PC Latency marker: SimStart / SimEnd in llappviewer.cpp, the other four in
    # llvulkancontext.cpp; RenderStart at renderFrame's top level (every frame, Reflex or not).
    app = _code(_raw(ctx, APP) or "", blank_strings=True)
    for name, where, rel in (("SimStart", app, APP), ("SimEnd", app, APP), ("RenderStart", cx, CTX),
                             ("RenderEnd", cx, CTX), ("PresentStart", cx, CTX), ("PresentEnd", cx, CTX)):
        n = len(re.findall(r"\.marker\(\s*LLVKFrameGen::" + name + r"\s*\)", where))
        if n != 1:
            bad.append("MARKERS: %s marked %d times in %s (one)" % (name, n, rel))
    for name in ("SimStart", "SimEnd"):
        if re.search(r"LLVKFrameGen::" + name + r"\b", cx):
            bad.append("MARKERS: %s marked in %s" % (name, CTX))
    if rf:
        a, b = rf[0]
        m = re.search(r"\.marker\(\s*LLVKFrameGen::RenderStart\s*\)", cx[a:b])
        if m:
            seg = cx[a:a + m.start()]
            if seg.count("{") - seg.count("}") != 1:
                bad.append("MARKERS: RenderStart must be marked at renderFrame's top level")

    # SETTING-DEFAULT: RenderVKFrameGen (U32, saved, off by default), its comment the direct design:
    # live on / off, the DLSS add-on, no other route and no restart.
    st = _raw(ctx, SETTINGS) or ""
    m = re.search(r"<key>RenderVKFrameGen</key>\s*<map>(.*?)</map>", st, flags=re.S)
    if not m or not re.search(r"<key>Type</key>\s*<string>U32</string>", m.group(1)) \
            or not re.search(r"<key>Value</key>\s*<integer>0</integer>", m.group(1)) \
            or not re.search(r"<key>Persist</key>\s*<integer>1</integer>", m.group(1)):
        bad.append("SETTING-DEFAULT: settings.xml needs RenderVKFrameGen (U32, Persist 1, Value 0)")
    else:
        cm = re.search(r"<key>Comment</key>\s*<string>([^<]*)</string>", m.group(1))
        text = cm.group(1) if cm else ""
        if not text or NO_SL_WORDS.search(text) or re.search(r"restart|dlss/sl\b", text, re.I) \
                or "DLSS add-on" not in text:
            bad.append("SETTING-DEFAULT: RenderVKFrameGen's comment must describe the direct design (the DLSS add-on, "
                       "live; no other route, no restart)")


def _else_span(text, end):
    """(start, end) of the else chain that follows the if block ending at `end` (an `else { }`,
    `else if (...) { }`, and any further else), or None when no else follows."""
    m = re.match(r"\s*else\b", text[end:])
    if not m:
        return None
    start, at = end + m.start(), end + m.end()
    while True:
        blk = _block_after(text, at)
        if not blk:
            return None
        at = blk[1]
        m = re.match(r"\s*else\b", text[at:])
        if not m:
            return (start, at)
        at += m.end()


def _rule_reflex_pacing(ctx, bad):
    """REFLEX-PACING: where Reflex paces the frame its sleep replaces the slot wait: llappviewer.cpp calls
    beginFrame( before any waitFrameSlot(), inside `if (fg_paced)`, and every waitFrameSlot() sits in that
    if's else chain."""
    app = _code(_raw(ctx, APP) or "", blank_strings=True)
    b = app.find("LLVKFrameGen::instance().beginFrame(")
    waits = [m.start() for m in re.finditer(r"\bwaitFrameSlot\s*\(\s*\)", app)]
    if b < 0:
        bad.append("REFLEX-PACING: llappviewer.cpp never calls LLVKFrameGen::instance().beginFrame(")
        return
    if any(w < b for w in waits):
        bad.append("REFLEX-PACING: a waitFrameSlot() runs before beginFrame(")
    m = re.search(r"\bif\s*\(\s*fg_paced\s*\)\s*\{", app)
    blk = _block_after(app, m.end() - 1) if m else None
    if not blk:
        bad.append("REFLEX-PACING: no `if (fg_paced) { ... }` branch around beginFrame(")
        return
    if not (blk[0] <= b < blk[1]):
        bad.append("REFLEX-PACING: beginFrame( must be called inside the `if (fg_paced)` branch")
    other = _else_span(app, blk[1])
    if not other:
        bad.append("REFLEX-PACING: the `if (fg_paced)` branch needs an else holding the slot wait")
        return
    if not waits:
        bad.append("REFLEX-PACING: llappviewer.cpp never waits for the frame slot (waitFrameSlot())")
    for w in waits:
        if not (other[0] <= w < other[1]):
            bad.append("REFLEX-PACING: a waitFrameSlot() (line %d) is outside the else of `if (fg_paced)` (Reflex's "
                       "sleep replaces the slot wait)" % (app.count("\n", 0, w) + 1))


def _rule_output_chain(ctx, bad):
    """OUTPUT-CHAIN: the frame's Context (frameGenContext) takes its output from the chain actually
    presenting (mSwapchain.displayStatus().encoding; in SDR its format, mSwapchain.format()), never from
    the HDR target in flight (mHdrEncodeTarget); decideDirect and chainGenerated read it."""
    cx = _code(_raw(ctx, CTX) or "", blank_strings=True)
    m = re.search(r"\bauto\s+frameGenContext\s*=\s*\[[^\]]*\]\s*\(\s*\)\s*\{", cx)
    blk = _block_after(cx, m.end() - 1) if m else None
    if not blk:
        bad.append("OUTPUT-CHAIN: %s needs the frameGenContext lambda (the frame's llfg::Context)" % CTX)
        return
    body = cx[blk[0]:blk[1]]
    sets = list(re.finditer(r"\bfc\.output\s*=(?!=)\s*([^;]*);", body))
    if len(sets) != 1:
        bad.append("OUTPUT-CHAIN: frameGenContext must set fc.output once (found %d)" % len(sets))
        return
    src = sets[0].group(1)
    for ident in set(re.findall(r"\b[a-z]\w*\b", src)):   # a local it reads: that local's initializer
        d = re.search(r"\b%s\s*=(?!=)\s*([^;]*);" % re.escape(ident), body[:sets[0].start()])
        if d:
            src += " " + d.group(1)
    if not re.search(r"\bmSwapchain\s*\.\s*displayStatus\s*\(\s*\)\s*\.\s*encoding\b", src):
        bad.append("OUTPUT-CHAIN: fc.output must come from the presenting chain "
                   "(mSwapchain.displayStatus().encoding)")
    if not re.search(r"\bmSwapchain\s*\.\s*format\s*\(\s*\)", src):
        bad.append("OUTPUT-CHAIN: an SDR chain's output is told by the presenting chain's format (mSwapchain.format())")
    if "mHdrEncodeTarget" in body:
        bad.append("OUTPUT-CHAIN: frameGenContext must not read the HDR target in flight (mHdrEncodeTarget)")


STATUS_XUI = "indra/newview/skins/default/xui/en/panel_status_bar.xml"
COLORS = "indra/newview/skins/default/colors.xml"


def _rule_fps_shown(ctx, bad):
    """FPS-SHOWN: while frames are generated the status bar FPS box shows the frames on screen (the
    viewer counts real frames): rendered fps x the measured frames shown per rendered frame, in
    FPSFrameGenTextColor, both rates in the fps_framegen tooltip. The box stays 40 px: at most three
    digits, never "<fps>x<N>" (a "32x2" did not fit, 2026-11-09)."""
    sb = _code(_raw(ctx, STATUSBAR) or "")
    sbb = _code(_raw(ctx, STATUSBAR) or "", blank_strings=True)
    m = re.search(r"\bmult\s*=([^;]*);", sbb)
    if not m or "shownPerRendered()" not in m.group(1):
        bad.append("FPS-SHOWN: the multiplier must be the measured frames shown per rendered frame (shownPerRendered())")
    if not re.search(r"setText\(\s*std::to_string\(\s*llfloor\(\s*fps\s*\*\s*fg\.shownPerRendered\(\)\s*\)\s*\)\s*\)", sbb):
        bad.append("FPS-SHOWN: while generating the box must show the frames on screen "
                   "(setText(std::to_string(llfloor(fps * fg.shownPerRendered()))))")
    if not re.search(r'getColor\(\s*"FPSFrameGenTextColor"\s*\)', sb) or re.search(r'setText\(\s*llformat\(\s*"%dx%d"', sb):
        bad.append("FPS-SHOWN: the on-screen rate is coloured (getColor(\"FPSFrameGenTextColor\")), never "
                   "\"<fps>x<N>\" (does not fit)")
    if not re.search(r'getString\(\s*"fps_framegen"', sb):
        bad.append("FPS-SHOWN: the tooltip while generating is the fps_framegen string (both rates)")
    if 'name="FPSFrameGenTextColor"' not in (_raw(ctx, COLORS) or ""):
        bad.append("FPS-SHOWN: colors.xml needs FPSFrameGenTextColor")
    xui = _raw(ctx, STATUS_XUI) or ""
    t = re.search(r'<text\b[^>]*name="FPSText"[^>]*>', xui, flags=re.S)
    if not t or not re.search(r'\bwidth="40"', t.group(0)):
        bad.append("FPS-SHOWN: the status bar FPS box (FPSText) must stay 40 px wide")
    if not re.search(r'<(?:panel\.)?string\b[^>]*\bname="fps_framegen"', xui):
        bad.append("FPS-SHOWN: panel_status_bar.xml needs the fps_framegen tooltip string")


# Other viewers are named nowhere in the tree, this check included: it knows them by the first 16 hex
# digits of the SHA-256 of the lowercase name without spaces (the list framegen_session.py held).
OTHER_VIEWER_HASHES = frozenset((
    "6f5e90519c659be7", "924b68a85ca13710", "61933d3774170c68", "99ae31cfdd9f4f2a", "f821a90af1c57d27",
    "6155678307f8f50b", "249f412ae8f74f9b", "aeebad4a796fcc2e", "a15dc950febf703f", "91c81156245a63fe"))


def _names_other_viewer(text):
    """True when a word, or two adjacent words run together, of text is another viewer's name."""
    import hashlib
    words = re.findall(r"[a-z]+", text.lower())
    for w in words + [a + b for a, b in zip(words, words[1:])]:
        if hashlib.sha256(w.encode()).hexdigest()[:16] in OTHER_VIEWER_HASHES:
            return True
    return False


def _rule_prefs(ctx, bad):
    """PREFS: the Frame generation choice in the advanced graphics floater offers Off, 2x, 3x and 4x
    (items 0, 2, 3, 4), and no frame generation text (the combo, FrameGen* controls, framegen_* and
    vsync_framegen strings) names another viewer."""
    xui = _raw(ctx, PREFS_XUI)
    if xui is None:
        bad.append("PREFS: %s is missing" % PREFS_XUI)
        return
    combo = re.search(r'<combo_box\b[^>]*control_name="RenderVKFrameGen"[^>]*>(.*?)</combo_box>', xui, flags=re.S)
    if not combo:
        bad.append("PREFS: no combo_box with control_name=\"RenderVKFrameGen\" in the advanced graphics floater")
    else:
        values = re.findall(r'<combo_box\.item\b[^>]*\bvalue="(\d+)"', combo.group(1))
        if values != ["0", "2", "3", "4"]:
            bad.append("PREFS: the frame generation items must be 0, 2, 3, 4 (found %s)" % ", ".join(values))
    texts = [(combo.group(0), "the FrameGen combo")] if combo else []
    for m in re.finditer(r'<(\w[\w.]*)\b[^>]*\bname="(FrameGen\w*|framegen_\w+|vsync_framegen)"[^>]*>([^<]*)', xui):
        texts.append((m.group(0), m.group(2)))
    for text, where in texts:
        if _names_other_viewer(text):
            bad.append("PREFS: %s names another viewer" % where)

FX = "indra/newview/slfxstack.cpp"
FX_H = "indra/newview/slfxstack.h"


def run(ctx, result):
    bad = []
    _rule_no_streamline(ctx, bad)
    _scope_selftest(bad)
    _rule_carried(ctx, bad)
    _rule_reflex_pacing(ctx, bad)
    _rule_output_chain(ctx, bad)
    _rule_fps_shown(ctx, bad)
    _rule_prefs(ctx, bad)
    _rule_device(ctx, bad)
    _rule_swapchain(ctx, bad)
    _rule_direct(ctx, bad)
    _rule_frame(ctx, bad)
    _rule_present(ctx, bad)
    _rule_reflex(ctx, bad)
    _rule_ui(ctx, bad)
    _rule_hdr_ui(ctx, bad)
    for b in bad:
        result.fail(b)
    result.summary = "%d problem(s)" % len(bad) if bad else "the frame generation rules hold"
