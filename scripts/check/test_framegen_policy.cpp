// Tests for indra/newview/vkgfx/llvkframegenpolicy.h: when frame generation is available, how a
// requested multiplier maps to generated frames, and how a frame cap maps to Reflex's limit.
// Built and run by scripts/check/checks/framegen_policy.py.
#include "vkgfx/llvkframegenpolicy.h"
#include "vkgfx/vk_allocation_limit.h"
#include "vkgfx/vk_display_mode_transaction.h"
#include "vkgfx/vk_hang_watchdog.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main()
{
    using namespace llfg;
    const Caps rtx50{true, true, 3, true};
    const Caps rtx40{true, true, 1, true};
    const Caps none{true, false, 0, true};
    const Context ok{true, true, Output::Hdr10, false, false, true};

    // generatedFrames: requested multiplier -> frames to generate
    CHECK(generatedFrames(4, rtx50, false) == 3);
    CHECK(generatedFrames(3, rtx50, false) == 2);
    CHECK(generatedFrames(2, rtx50, false) == 1);
    CHECK(generatedFrames(4, rtx40, false) == 1);      // RTX 40: 2x only
    CHECK(generatedFrames(0, rtx50, false) == 0);
    CHECK(generatedFrames(1, rtx50, false) == 0);
    CHECK(generatedFrames(3, none, false) == 0);

    // frameLimitUs: the cap is on-screen frames; Reflex limits the real ones
    CHECK(frameLimitUs(3, 120.f) == 33333);     // 4x at 120 -> 30 real fps
    CHECK(frameLimitUs(1, 60.f) == 33333);      // 2x at 60 -> 30 real fps
    CHECK(frameLimitUs(3, 0.f) == 0);
    CHECK(frameLimitUs(0, 60.f) == 16667);      // off: the plain cap

    // blockReason: first failing rule, in the enum's order
    CHECK(blockReason(rtx50, ok) == Block::None);
    { Context c = ok; c.platform = false; CHECK(blockReason(Caps{false, false, 0, false}, c) == Block::Platform); }
    CHECK(blockReason(Caps{false, false, 0, false}, ok) == Block::NoAddon);
    CHECK(blockReason(none, ok) == Block::Unsupported);
    // (#1544) no upscaler wrote depth and motion vectors: either upscaler would do
    { Context c = ok; c.inputsOn = false; CHECK(blockReason(rtx50, c) == Block::NeedsUpscaler); }
    CHECK(std::strcmp(blockText(Block::NeedsUpscaler), "needs DLSS or FSR upscaling") == 0);
    // #1536 the output in use: HDR10 and 8-bit SDR run; scRGB and an SDR surface that blends in linear light do not
    { Context c = ok; c.output = Output::Sdr8; CHECK(blockReason(rtx50, c) == Block::None); }   // HDR off mid-session
    { Context c = ok; c.output = Output::ScRgb; CHECK(blockReason(rtx50, c) == Block::NeedsHdr10); }
    { Context c = ok; c.output = Output::SdrOther; CHECK(blockReason(rtx50, c) == Block::SdrFormat); }
    // (the output is checked after DLSS and before VR / the background, as before)
    { Context c = ok; c.output = Output::ScRgb; c.inputsOn = false; CHECK(blockReason(rtx50, c) == Block::NeedsUpscaler); }
    { Context c = ok; c.output = Output::SdrOther; c.vr = true; CHECK(blockReason(rtx50, c) == Block::SdrFormat); }
    { Context c = ok; c.output = Output::Sdr8; c.vr = true; CHECK(blockReason(rtx50, c) == Block::Vr); }
    { Context c = ok; c.output = Output::Sdr8; c.background = true; CHECK(blockReason(rtx50, c) == Block::Background); }
    CHECK(Context{}.output == Output::Sdr8);
    CHECK(outputGenerates(Output::Hdr10) && outputGenerates(Output::Sdr8));
    CHECK(!outputGenerates(Output::ScRgb) && !outputGenerates(Output::SdrOther));
    // the SDR interface layer: SDR 8-bit on a frame generation chain, nowhere else
    CHECK(sdrLayer(Output::Sdr8, 3));
    CHECK(sdrLayer(Output::Sdr8, 1));
    CHECK(!sdrLayer(Output::Sdr8, 0));
    CHECK(!sdrLayer(Output::Hdr10, 3));
    CHECK(!sdrLayer(Output::SdrOther, 3));
    CHECK(!sdrLayer(Output::ScRgb, 3));
    { Context c = ok; c.vr = true; CHECK(blockReason(rtx50, c) == Block::Vr); }
    { Context c = ok; c.background = true; CHECK(blockReason(rtx50, c) == Block::Background); }
    { Context c = ok; c.mainWindow = false; CHECK(blockReason(rtx50, c) == Block::NotMainWindow); }

    CHECK(std::strcmp(blockText(Block::Platform), "only on Windows and Linux") == 0);
    CHECK(std::strcmp(blockText(Block::NeedsHdr10),
                      "not with scRGB HDR output: switch Display HDR off, or use an HDR10 display mode") == 0);
    CHECK(std::strcmp(blockText(Block::SdrFormat),
                      "this display's SDR format is not supported by frame generation") == 0);
    CHECK(std::strcmp(blockText(Block::None), "") == 0);

    // historyReset (review I3): the generator drops its history unless the last evaluated frame
    // is the one right before this one (after a teleport, a snapshot, generation back on)
    CHECK(!historyReset(false, 41, 42));
    CHECK(historyReset(true, 41, 42));          // the renderer's own reset
    CHECK(historyReset(false, 0, 42));          // never evaluated
    CHECK(historyReset(false, 40, 42));         // a frame went without generated frames in between

    // pacing generated frames ourselves: which frames are paced, when each present is due
    CHECK(!paceable(0)); CHECK(!paceable(-5)); CHECK(!paceable(100'000'000));
    CHECK(paceable(16'666'667));
    CHECK(presentSpacingNs(16'000'000, 1) == 8'000'000);
    CHECK(presentSpacingNs(16'000'000, 3) == 4'000'000);
    CHECK(dueNs(1000, 4'000'000, 0) == 0);                  // first: no gate
    CHECK(dueNs(1000, 4'000'000, 1) == 4'001'000);
    CHECK(dueNs(1000, 4'000'000, 3) == 12'001'000);         // the real frame at 4x
    CHECK(gpuToCpuNs(1500, 1000, 50'000, 2.0) == 51'000);
    CHECK(gpuToCpuNs(500, 1000, 50'000, 2.0) == 49'000);    // before the reference
    // swapchain images for the generated frames: the present thread takes each image just before it is
    // shown, so a batch needs its generated frames + the real one
    CHECK(swapchainImagesNeeded(1) == 2); CHECK(swapchainImagesNeeded(2) == 3);
    CHECK(swapchainImagesNeeded(3) == 4);
    CHECK(swapchainImagesFor(1, PresentMode::Immediate) == 2); CHECK(swapchainImagesFor(3, PresentMode::Immediate) == 4);
    // (Task 11b fix 1, M3) MAILBOX and FIFO chains ask for one image more: with only generated + 1, every
    // acquire waits for the compositor to let go of the image shown before, so MAILBOX never replaces one
    CHECK(swapchainImagesFor(1, PresentMode::Mailbox) == 3); CHECK(swapchainImagesFor(3, PresentMode::Mailbox) == 5);
    CHECK(swapchainImagesFor(2, PresentMode::Fifo) == 4);
    // ... and the multiplier is then refresh-bound (no more frames shown than the display refreshes)
    CHECK(refreshBound(PresentMode::Mailbox) && refreshBound(PresentMode::Fifo) && !refreshBound(PresentMode::Immediate));
    // the generated frames a chain can carry: the images a batch needs (an extra one is never required)
    CHECK(generatedForImages(3, 4) == 3); CHECK(generatedForImages(3, 3) == 2);
    CHECK(generatedForImages(3, 1) == 0); CHECK(generatedForImages(3, 8) == 3);
    CHECK(generatedForImages(0, 16) == 0);
    // driver pacing (present metering): stop asking after kMeteringAskLimit refusals in a row; ask again after
    // a reset, or in fullscreen every kMeteringRetryNs
    {
        PacingState g;   // (never reset: no refusal is forgiven)
        for (uint32_t i = 0; i + 1 < kMeteringAskLimit; ++i) g.result(false, 1000);
        CHECK(g.ask());
        g.result(false, 5000); CHECK(!g.ask());
        CHECK(!g.retryDue(false, 5000 + kMeteringRetryNs));        // windowed: only a reset asks again
        CHECK(!g.retryDue(true, 5000 + kMeteringRetryNs - 1));
        CHECK(g.retryDue(true, 5000 + kMeteringRetryNs));          // fullscreen: asked again now and then
        // a retry asks with ONE batch: refused, it stops asking again at once (each refused batch goes out
        // unpaced: a retry of 30 every few seconds would judder a fullscreen window the desktop composes)
        g.retry(5000 + kMeteringRetryNs);
        CHECK(g.ask());
        g.result(false, 5000 + kMeteringRetryNs);
        CHECK(!g.ask());
        CHECK(!g.retryDue(true, 5000 + 2 * kMeteringRetryNs - 1));       // (the next retry counts from this one)
        CHECK(g.retryDue(true, 5000 + 2 * kMeteringRetryNs));
        g.retry(5000 + 2 * kMeteringRetryNs);
        g.result(true, 5000 + 2 * kMeteringRetryNs);                     // accepted: asked as usual again
        CHECK(g.ask() && g.refusedInARow == 0 && g.accepted);
        for (uint32_t i = 0; i + 1 < kMeteringAskLimit; ++i) g.result(false, 1ull << 39);
        CHECK(g.ask());
        g.result(false, 1ull << 39); CHECK(!g.ask());
        // another batch size (the present thread): forgives a while when asking, never re-asks when stopped
        g.batchChanged(1ull << 39);
        CHECK(!g.ask());
        g.reset(6000); CHECK(g.ask()); CHECK(!g.retryDue(true, 1ull << 40));
        g.result(true, 6000);
        g.batchChanged(7000);
        g.result(false, 7000 + kMeteringWarmupNs - 1);
        CHECK(g.accepted);                                          // forgiven
        g.result(false, 7000 + kMeteringWarmupNs);
        CHECK(!g.accepted);
        g.result(false, 6000); g.result(true, 6000); CHECK(g.refusedInARow == 0);
        CHECK(kMeteringAskLimit == 30);
    }
    {
        PacingState g;   // asked whenever the device has present metering (no application id needed)
        CHECK(driverPacing(true, g));
        CHECK(!driverPacing(false, g));
    }
    // frame generation's images, admitted as ONE set against the free video memory (never one slot
    // admitted and another refused): 3 present slots when the whole set for 3 fits with the margin,
    // else 2, else none. No budget to check against: 3 (the allocations themselves are soft).
    {
        const ImageSet set{100, 10};                                  // per slot, shared (the scratch)
        CHECK(presentSlotsFor(set, true, 310 + 50, 50) == 3);         // 3*100 + 10 + margin
        CHECK(presentSlotsFor(set, true, 310 + 49, 50) == 2);         // one byte short of 3
        CHECK(presentSlotsFor(set, true, 210 + 50, 50) == 2);
        CHECK(presentSlotsFor(set, true, 210 + 49, 50) == 0);         // not even 2: refused
        CHECK(presentSlotsFor(set, true, 0, 50) == 0);
        CHECK(presentSlotsFor(set, false, 0, 0) == 3);                // no VK_EXT_memory_budget
        CHECK(presentSlotsFor(ImageSet{~0ull / 2, ~0ull / 2}, true, ~0ull, 0) == 0);   // (no overflow)
        CHECK(kMaxPresentSlots == 3 && kMinPresentSlots == 2);
    }
    // the large-request margin the renderer's own large targets keep free: max(budget / 32, 256 MiB)
    CHECK(vkLargeRequestMargin(32ull << 30) == 1ull << 30);
    CHECK(vkLargeRequestMargin(4ull << 30) == 256ull << 20);
    CHECK(vkLargeRequestMargin(0) == 256ull << 20);
    CHECK(std::strstr(blockText(Block::OldAddon), "too old") != nullptr);
    CHECK(std::strcmp(blockText(Block::NoPresentQueue), "this GPU or driver cannot present generated frames") == 0);

    // (Task 11b) Linux is a platform: the flag is "Windows or Linux", the text names both
    { Context c = ok; c.platform = false; CHECK(blockReason(rtx50, c) == Block::Platform); }
    CHECK(std::strstr(blockText(Block::Platform), "Windows") && std::strstr(blockText(Block::Platform), "Linux"));
    // The add-on loads once at startup: installing or updating it needs a restart, and the reasons say so;
    // switched off (RenderVKDlssShim 0) says that instead. Short (the Preferences tooltip, the status line).
    CHECK(std::strstr(blockText(Block::NoAddon), "restart") && std::strstr(blockText(Block::NoAddon), "install"));
    CHECK(std::strstr(blockText(Block::OldAddon), "rebuild it and restart"));
    CHECK(std::strstr(blockText(Block::AddonOff), "RenderVKDlssShim") && !std::strstr(blockText(Block::AddonOff), "install"));
    // (Task 11b fix 1, M7) switching the add-on on needs a restart too, and the reason says so
    CHECK(std::strstr(blockText(Block::AddonOff), "switch it on") && std::strstr(blockText(Block::AddonOff), "restart"));
    // The add-on's own block, decided once at startup: switched off wins over "not loaded" (never "install it"
    // to a player who switched it off), then too old, then unable to run.
    CHECK(addonBlock(false, true, false, false) == Block::AddonOff);
    CHECK(addonBlock(false, false, false, false) == Block::NoAddon);
    CHECK(addonBlock(true, false, false, true) == Block::OldAddon);
    CHECK(addonBlock(true, false, true, false) == Block::Unsupported);
    CHECK(addonBlock(true, false, true, true) == Block::None);
    CHECK(addonBlock(true, true, true, true) == Block::None);      // (loaded: the switch was on at startup)
    // (M9) a 1.3.0 / 1.3.1 add-on runs frame generation but predates the fixes of 1.3.2 (the reasons it
    // states when frame generation cannot run): a rebuild is suggested
    CHECK(addonPredatesFixes("1.3.0") && addonPredatesFixes("1.3.1"));
    CHECK(!addonPredatesFixes("1.3.2") && !addonPredatesFixes("1.4.0") && !addonPredatesFixes("2.0.0"));
    CHECK(!addonPredatesFixes("1.2.0") && !addonPredatesFixes("") && !addonPredatesFixes(nullptr) && !addonPredatesFixes("x"));
    for (Block b : {Block::NoAddon, Block::OldAddon, Block::AddonOff, Block::Platform}) CHECK(std::strlen(blockText(b)) <= 90);
    for (Block b : {Block::NeedsUpscaler, Block::Unsupported, Block::Background}) CHECK(!std::strstr(blockText(b), "restart"));

    // (Task 11b) a frame generation chain's present mode: IMMEDIATE, else MAILBOX (Wayland without tearing
    // control), else FIFO -- generation runs in all three
    CHECK(frameGenPresentMode(true, true) == PresentMode::Immediate);
    CHECK(frameGenPresentMode(true, false) == PresentMode::Immediate);
    CHECK(frameGenPresentMode(false, true) == PresentMode::Mailbox);
    CHECK(frameGenPresentMode(false, false) == PresentMode::Fifo);
    CHECK(std::strstr(presentModeText(PresentMode::Fifo), "FIFO") && std::strstr(presentModeText(PresentMode::Mailbox), "MAILBOX")
          && std::strstr(presentModeText(PresentMode::Immediate), "IMMEDIATE"));
    CHECK(!presentCanBlockOnCompositor(PresentMode::Immediate));
    CHECK(presentCanBlockOnCompositor(PresentMode::Mailbox) && presentCanBlockOnCompositor(PresentMode::Fifo));

    // (Task 11b) the pacer's CPU clock is the domain the GPU timestamps are calibrated against: Windows the
    // performance counter; Linux CLOCK_MONOTONIC (an absolute sleep exists for it), else CLOCK_MONOTONIC_RAW
    CHECK(pacerClock(true, true, true, true) == PacerClock::Qpc);
    CHECK(pacerClock(true, false, true, true) == PacerClock::None);          // (Windows never takes a Linux clock)
    CHECK(pacerClock(false, true, true, true) == PacerClock::Monotonic);     // (Linux never takes the counter)
    CHECK(pacerClock(false, false, false, true) == PacerClock::MonotonicRaw);
    CHECK(pacerClock(false, true, false, false) == PacerClock::None);
    CHECK(pacerSleepsAbsolute(PacerClock::Monotonic));
    CHECK(!pacerSleepsAbsolute(PacerClock::MonotonicRaw) && !pacerSleepsAbsolute(PacerClock::Qpc));
    CHECK(std::strstr(pacerClockName(PacerClock::MonotonicRaw), "CLOCK_MONOTONIC_RAW"));
    CHECK(timespecNs(2, 500) == 2'000'000'500LL);
    { int64_t s = 0, n = 0; splitNs(3'000'000'007LL, s, n); CHECK(s == 3 && n == 7); }
    { int64_t s = 0, n = 0; splitNs(999'999'999LL, s, n); CHECK(s == 0 && n == 999'999'999LL); }
    // its waits: sleep in steps of at most 1 ms (a drain is seen between them) until 1 ms before, then spin
    CHECK(pacerSleepNs(10'000'000) == 1'000'000);
    CHECK(pacerSleepNs(1'500'000) == 500'000);
    CHECK(pacerSleepNs(1'000'000) == 0 && pacerSleepNs(200'000) == 0 && pacerSleepNs(-5) == 0);

    // (Task 11b) a hidden window: minimised, hidden or occluded (Wayland: another workspace, fully covered)
    CHECK(windowHidden(true, false, false) && windowHidden(false, true, false) && windowHidden(false, false, true));
    CHECK(!windowHidden(false, false, false));
    // (Task 7c fix 4) the renderer never waits without a bound for the present thread, on any chain: its wait for
    // a slot ends after a few frame intervals on MAILBOX / FIFO (at 4x on a 60 Hz FIFO chain one job holds 4
    // vblanks, 67 ms) and, on IMMEDIATE, well above a healthy job at low frame rates; then the frame is skipped
    CHECK(slotWaitBoundMs(PresentMode::Immediate) == kSlotWaitBoundImmediateMs);
    CHECK(slotWaitBoundMs(PresentMode::Mailbox) == kSlotWaitBoundMs && slotWaitBoundMs(PresentMode::Fifo) == kSlotWaitBoundMs);
    CHECK(kSlotWaitBoundMs >= 70 && kSlotWaitBoundMs <= 200);
    CHECK(kSlotWaitBoundImmediateMs >= 200 && kSlotWaitBoundImmediateMs <= 500);
    // ... the thread is held once one job has run for a second (its own waits are 1 s at most): from then on the
    // GPU is checked on every skipped frame, and a drain only looks
    CHECK(kHeldJobMs >= 1000 && kHeldJobMs <= 2000);
    CHECK(kDrainBoundMs >= 2000 && kDrainBoundMs <= 3000);
    // ... and the main chain is not rebuilt under a present the compositor may hold: a frame generation chain on
    // MAILBOX / FIFO waits while the window is hidden or the thread is held; any other chain is rebuilt at once
    CHECK(deferRebuild(true, PresentMode::Fifo, true, false) && deferRebuild(true, PresentMode::Mailbox, false, true));
    CHECK(!deferRebuild(true, PresentMode::Fifo, false, false));
    CHECK(!deferRebuild(true, PresentMode::Immediate, true, true));
    CHECK(!deferRebuild(false, PresentMode::Fifo, true, true));
    // ... a frame that was not rendered costs the generator no history: frame 6 skipped after 5 was evaluated,
    // frame 7 follows on (no reset); several in a row too; a real gap, or nothing evaluated yet, stays a reset
    CHECK(lastEvaluatedAfterSkip(5, 6) == 6 && !historyReset(false, lastEvaluatedAfterSkip(5, 6), 7));
    CHECK(!historyReset(false, lastEvaluatedAfterSkip(lastEvaluatedAfterSkip(5, 6), 7), 8));
    CHECK(historyReset(false, 5, 7));                                   // (without it: a gap)
    CHECK(lastEvaluatedAfterSkip(3, 6) == 3 && historyReset(false, lastEvaluatedAfterSkip(3, 6), 7));
    CHECK(lastEvaluatedAfterSkip(0, 1) == 0 && historyReset(false, lastEvaluatedAfterSkip(0, 1), 2));

    // (Task 7c fix 5, I1) an output-mode request (an HDR switch) pending when the main chain's rebuild is asked
    // for: a rebuild that was not made (deferred: the present thread not drained, a present proof withheld)
    // settles nothing -- the old chain is still in use, so its format says nothing about the request, which is
    // neither committed nor cancelled; it is still pending, and the rebuild that is made settles it
    {
        using namespace vkdisplay::transaction;
        // HDR switched on: the old chain is SDR (it does not match the request), the rebuild defers
        CHECK(outputRequestAfterRebuild(false, true, false) == OutputRequestStep::Keep);
        CHECK(outputRequestAfterRebuild(false, true, true) == OutputRequestStep::Keep);
        CHECK(outputRequestAfterRebuild(false, false, false) == OutputRequestStep::Keep);
        // ... still pending: the next try's rebuild is made, the chain has the requested format: committed
        CHECK(outputRequestAfterRebuild(true, true, true) == OutputRequestStep::Commit);
        // the surface negotiated another format than the request's: cancelled, asked again (as before)
        CHECK(outputRequestAfterRebuild(true, true, false) == OutputRequestStep::Cancel);
        // the renderer's bank for it is not ready yet: nothing to commit or cancel here
        CHECK(outputRequestAfterRebuild(true, false, true) == OutputRequestStep::Keep);
    }
    // (I2) a lost device raised after a renderer thread was left behind at exit is not reported
    CHECK(vkLostDeviceReported(false) && !vkLostDeviceReported(true));
    // (Minor 9) the status names what a held present thread waits for
    CHECK(std::strstr(heldText(HeldIn::Present), "display") && std::strstr(heldText(HeldIn::Generator), "generator"));
    CHECK(std::strstr(heldText(HeldIn::Acquire), "image"));
    CHECK(std::strcmp(heldText(HeldIn::Other), "frame generation is waiting") == 0);

    // (final fix C5) the add-on's answer for the most generated frames is clamped to what one batch of
    // presents carries (the swapchain's kMaxBatchPresents: N generated + the real frame), and the rules stay
    // safe for absurd values (a division by zero, a wrap to 0 images, a loop of four billion steps)
    CHECK(maxGeneratedFor(5, 6) == 5 && maxGeneratedFor(6, 6) == 5 && maxGeneratedFor(3, 3) == 2);
    CHECK(maxGeneratedFor(0xFFFFFFFFu, 6) == 5 && maxGeneratedFor(0, 6) == 0);
    CHECK(maxGeneratedFor(4, 0) == 0 && maxGeneratedFor(4, 1) == 0);
    // (final fix C6) the multiplier ends at 4x unless the driver paces the presents (present metering:
    // supported, and accepted right now -- in fullscreen): the viewer's own pacer does 5 or 6
    // presents per rendered frame badly. Only then may 5x and 6x run, within what the GPU generates. A
    // RenderVKFrameGen above 4 without it runs as 4x, at once when the driver refuses (no restart).
    {
        const Caps drv6{true, true, 5, true};                       // an RTX 50 whose driver offers up to 6x
        CHECK(kMaxMultiplier == 4 && kMaxMultiplierDriverPaced == 6);
        CHECK(generatedFrames(6, rtx50, false) == 3 && generatedFrames(6, drv6, false) == 3);
        CHECK(generatedFrames(5, drv6, false) == 3 && generatedFrames(255, drv6, false) == 3);
        CHECK(generatedFrames(0xFFFFFFFFu, drv6, false) == 3 && generatedFrames(4, drv6, false) == 3);
        CHECK(generatedFrames(6, drv6, true) == 5 && generatedFrames(5, drv6, true) == 4);
        CHECK(generatedFrames(6, rtx50, true) == 3);                // (the GPU's own maximum still bounds it)
        CHECK(generatedFrames(6, rtx40, true) == 1 && generatedFrames(5, rtx40, false) == 1);
        CHECK(generatedFrames(255, drv6, true) == 5 && generatedFrames(0xFFFFFFFFu, drv6, true) == 5);
        CHECK(generatedFrames(4, drv6, true) == 3 && generatedFrames(2, drv6, true) == 1 && generatedFrames(1, drv6, true) == 0);
        // what the viewer keeps of the driver's answer: one batch of presents (the second line of defence)
        CHECK(maxGeneratedFor(5, 6) == 5 && maxGeneratedFor(6, 6) == 5 && maxGeneratedFor(3, 6) == 3);
        CHECK(maxGeneratedFor(0xFFFFFFFFu, 6) == 5 && maxGeneratedFor(9, 16) == 5);   // (never above 6x)
        // "accepted right now": the driver accepted its batches for kPacingRaiseNs, and still does
        const uint64_t T = 10'000'000'000ull;
        PacingState g;
        CHECK(!driverPacingAccepted(true, g, T));                   // nothing accepted yet: 4x first
        g.reset(T);
        g.result(false, T); g.result(false, T + kMeteringWarmupNs - 1);   // a new chain's refusals: forgiven a while
        CHECK(!g.accepted);
        g.result(true, T + kMeteringWarmupNs - 1);
        CHECK(!driverPacingAccepted(true, g, T + kMeteringWarmupNs));     // accepted, not yet for long
        const uint64_t A = T + kMeteringWarmupNs - 1;               // accepted since
        CHECK(!driverPacingAccepted(true, g, A + kPacingRaiseNs - 1));
        CHECK(driverPacingAccepted(true, g, A + kPacingRaiseNs));
        CHECK(!driverPacingAccepted(false, g, A + kPacingRaiseNs));       // no present metering on this device
        // a new chain (a rebuild, exclusive fullscreen taken again) at 6x: refusals for a while do not drop 6x
        const uint64_t R = A + 3 * kPacingRaiseNs;
        g.reset(R);
        for (int k = 0; k < 20; ++k) g.result(false, R + k * (kMeteringWarmupNs / 25));
        CHECK(driverPacingAccepted(true, g, R + kMeteringWarmupNs - 1));
        g.result(true, R + kMeteringWarmupNs - 1);
        // a counted refusal (the window composed: a desktop overlay over it, or no longer fullscreen): 4x at once
        g.result(false, R + kMeteringWarmupNs);
        CHECK(!driverPacingAccepted(true, g, R + kMeteringWarmupNs));
        // and 5x / 6x again only after kPacingRaiseNs more of accepted batches (no flapping; no session lock)
        const uint64_t B = R + 2 * kPacingRaiseNs;
        g.result(true, B);
        CHECK(!driverPacingAccepted(true, g, B + kPacingRaiseNs / 2));
        CHECK(driverPacingAccepted(true, g, B + kPacingRaiseNs));
        for (int k = 0; k < 10; ++k) { g.result(false, B + kPacingRaiseNs); g.result(true, B + 2 * kPacingRaiseNs); }
        CHECK(driverPacingAccepted(true, g, B + 3 * kPacingRaiseNs));     // however often it was taken back
        // a reset forgives refusals only for kMeteringWarmupNs, and an accepted batch ends the forgiving
        g.reset(B + 4 * kPacingRaiseNs);
        g.result(true, B + 4 * kPacingRaiseNs);
        g.result(false, B + 4 * kPacingRaiseNs + 1);
        CHECK(!g.accepted);
        CHECK(kMeteringWarmupNs >= 250'000'000ull && kMeteringWarmupNs <= 1'000'000'000ull);
        CHECK(kPacingRaiseNs >= 1'000'000'000ull && kPacingRaiseNs <= 5'000'000'000ull);
        // the multipliers Preferences offers: 2x..4x as the GPU does, 5x and 6x only with accepted driver pacing
        CHECK(multiplierOffered(2, drv6, false) && multiplierOffered(4, drv6, false));
        CHECK(!multiplierOffered(5, drv6, false) && !multiplierOffered(6, drv6, false));
        CHECK(multiplierOffered(5, drv6, true) && multiplierOffered(6, drv6, true) && !multiplierOffered(7, drv6, true));
        CHECK(!multiplierOffered(5, rtx50, true) && multiplierOffered(4, rtx50, true));
        CHECK(multiplierOffered(2, rtx40, true) && !multiplierOffered(3, rtx40, true) && !multiplierOffered(1, drv6, true));
        // a saved 5x / 6x that runs as 4x says so
        // no present metering on this device (an older driver): it cannot pace
        CHECK(std::strcmp(cappedMultiplierText(6, drv6, false, false), "4x (5x and 6x need a newer NVIDIA driver)") == 0);
        // present metering, but the driver does not pace: the window is composed, not fullscreen
        CHECK(std::strcmp(cappedMultiplierText(6, drv6, false, true), "4x (5x and 6x need fullscreen)") == 0);
        CHECK(std::strcmp(cappedMultiplierText(6, drv6, true, true), "") == 0);
        CHECK(std::strcmp(cappedMultiplierText(5, rtx50, false, false), "4x (5x and 6x need a newer NVIDIA driver)") == 0);
        CHECK(std::strcmp(cappedMultiplierText(6, drv6, true, false), "") == 0 && std::strcmp(cappedMultiplierText(4, drv6, false, false), "") == 0);
        CHECK(std::strcmp(cappedMultiplierText(6, rtx40, false, false), "") == 0);      // (2x there: not about pacing)
    }
    // (final fix C1) why the GPU's answer is "no": the add-on (1.3.2+) says which cause, and each has its own
    // text saying what to do. An add-on that states nothing (1.3.1: the field is not written) keeps the
    // general text; a cause this viewer does not know (a newer add-on) too.
    CHECK(capsBlock(true, true, 3, (uint32_t)AddonWhy::NotStated) == Block::None);
    CHECK(capsBlock(false, false, 0, 0) == Block::CapsFailed);              // the add-on could not report
    CHECK(capsBlock(false, true, 3, 0) == Block::CapsFailed);
    CHECK(capsBlock(true, false, 0, (uint32_t)AddonWhy::LibraryMissing) == Block::NoFgLibrary);
    CHECK(capsBlock(true, false, 0, (uint32_t)AddonWhy::GpuSchedulingOff) == Block::GpuSchedulingOff);
    CHECK(capsBlock(true, false, 0, (uint32_t)AddonWhy::QueryFailed) == Block::CapsFailed);
    CHECK(capsBlock(true, false, 0, (uint32_t)AddonWhy::NotStated) == Block::Unsupported);   // add-on 1.3.1
    CHECK(capsBlock(true, false, 0, 77) == Block::Unsupported);             // a cause from a newer add-on
    CHECK(capsBlock(true, true, 0, 0) == Block::Unsupported);               // available, but no multiplier
    // (a stated cause wins even over "available": the add-on clears it then, an odd one must not run)
    CHECK(capsBlock(true, true, 3, (uint32_t)AddonWhy::GpuSchedulingOff) == Block::GpuSchedulingOff);
    CHECK((uint32_t)AddonWhy::NotStated == 0 && (uint32_t)AddonWhy::LibraryMissing == 1
          && (uint32_t)AddonWhy::GpuSchedulingOff == 2 && (uint32_t)AddonWhy::QueryFailed == 3);
    // the library: what is missing, what to do (the add-on's README says the same), and the restart
    CHECK(std::strstr(blockText(Block::NoFgLibrary), "NVIDIA's frame generation library"));
    CHECK(std::strstr(blockText(Block::NoFgLibrary), "reinstall the DLSS add-on with the frame generation library"));
    CHECK(std::strstr(blockText(Block::NoFgLibrary), "restart the viewer"));
    // Windows' GPU scheduling: its name in Windows' settings, where to turn it on, and Windows' restart
    CHECK(std::strstr(blockText(Block::GpuSchedulingOff), "hardware-accelerated GPU scheduling is off"));
    CHECK(std::strstr(blockText(Block::GpuSchedulingOff), "Settings > System > Display > Graphics"));
    CHECK(std::strstr(blockText(Block::GpuSchedulingOff), "restart Windows"));
    CHECK(std::strstr(blockText(Block::CapsFailed), "DLSS add-on") && std::strstr(blockText(Block::CapsFailed), "rebuild"));
    CHECK(std::strstr(blockText(Block::CapsFailed), "restart the viewer"));
    for (Block b : {Block::NoFgLibrary, Block::GpuSchedulingOff, Block::CapsFailed})
    {
        CHECK(std::strlen(blockText(b)) <= 140);
        CHECK(std::strcmp(blockText(b), blockText(Block::Unsupported)) != 0);
        CHECK(!std::strstr(blockText(b), "NGX"));
    }

    // (final fix C2) a failure in mid-session stops frame generation until the next start: the player reads a
    // plain line saying so and what brings it back, never the add-on's raw error (that goes to the log)
    CHECK(stoppedText(Stop::Evaluate) == "Frame generation stopped: the frame generator failed. Restart the viewer to try again.");
    CHECK(stoppedText(Stop::Create) == "Frame generation stopped: the frame generator could not be set up. Restart the viewer to try again.");
    for (Stop s : {Stop::Create, Stop::Evaluate})
    {
        const std::string t = stoppedText(s);
        CHECK(t.rfind("Frame generation stopped: ", 0) == 0 && t.find("Restart the viewer to try again.") != std::string::npos);
        CHECK(t.find("NGX") == std::string::npos && t.find("result") == std::string::npos && t.size() <= 110);
    }

    // ---- #1544 two generators ----
    {
        CHECK(std::strcmp(generatorName(Generator::Dlss), "DLSS") == 0 && std::strcmp(generatorName(Generator::Fsr), "FSR") == 0);
        CHECK(std::strcmp(generatorName(Generator::None), "") == 0);
        CHECK(Caps{}.generator == Generator::Dlss && rtx50.generator == Generator::Dlss);   // (the aggregates above stay DLSS)

        // Which one runs. Automatic: DLSS where it runs, else FSR.
        auto is = [](GeneratorChoice c, Generator g, Block reason, Generator of)
        { return c.generator == g && c.reason == reason && c.reasonOf == of; };
        CHECK(is(chooseGenerator(0, Block::None, Block::None), Generator::Dlss, Block::None, Generator::None));
        CHECK(is(chooseGenerator(0, Block::NoAddon, Block::None), Generator::Fsr, Block::None, Generator::None));
        CHECK(is(chooseGenerator(0, Block::Unsupported, Block::None), Generator::Fsr, Block::None, Generator::None));
        CHECK(is(chooseGenerator(0, Block::None, Block::Unsupported), Generator::Dlss, Block::None, Generator::None));
        // neither runs: DLSS's reason when the player can act on it (FSR is blocked too) ...
        CHECK(is(chooseGenerator(0, Block::NoAddon, Block::Unsupported), Generator::None, Block::NoAddon, Generator::Dlss));
        for (Block b : {Block::NoAddon, Block::AddonOff, Block::NoFgLibrary, Block::GpuSchedulingOff, Block::CapsFailed, Block::OldAddon})
        {
            CHECK(userCanAct(b));
            CHECK(is(chooseGenerator(0, b, Block::NoPresentQueue), Generator::None, b, Generator::Dlss));
        }
        // ... else FSR's (what this GPU cannot do says more than what an add-on for another GPU cannot)
        for (Block b : {Block::Unsupported, Block::Platform, Block::NoPresentQueue})
        {
            CHECK(!userCanAct(b));
            CHECK(is(chooseGenerator(0, b, Block::Unsupported), Generator::None, Block::Unsupported, Generator::Fsr));
        }
        CHECK(!userCanAct(Block::None) && !userCanAct(Block::NeedsUpscaler) && !userCanAct(Block::Background));
        // forced: that one, or none with its own reason (never the other generator)
        CHECK(is(chooseGenerator(1, Block::NoAddon, Block::None), Generator::None, Block::NoAddon, Generator::Dlss));
        CHECK(is(chooseGenerator(1, Block::None, Block::Unsupported), Generator::Dlss, Block::None, Generator::None));
        CHECK(is(chooseGenerator(2, Block::None, Block::None), Generator::Fsr, Block::None, Generator::None));
        CHECK(is(chooseGenerator(2, Block::None, Block::Unsupported), Generator::None, Block::Unsupported, Generator::Fsr));
        CHECK(is(chooseGenerator(2, Block::NoAddon, Block::NoPresentQueue), Generator::None, Block::NoPresentQueue, Generator::Fsr));
        // a value above 2 behaves as automatic
        for (uint32_t v : {3u, 7u, ~0u})
        {
            CHECK(is(chooseGenerator(v, Block::None, Block::None), Generator::Dlss, Block::None, Generator::None));
            CHECK(is(chooseGenerator(v, Block::NoAddon, Block::None), Generator::Fsr, Block::None, Generator::None));
            CHECK(is(chooseGenerator(v, Block::NoAddon, Block::Unsupported), Generator::None, Block::NoAddon, Generator::Dlss));
        }

        // FSR's caps: one generated frame per rendered frame, whatever the multiplier and the pacing
        const GeneratorCaps fg = fsrGeneratorCaps(true);
        CHECK(fg.maxGenerated == 1 && !fg.driverPacing && fg.reflex && !fsrGeneratorCaps(false).reflex);
        const Caps fsr{false, true, fg.maxGenerated, fg.reflex, Generator::Fsr};
        for (bool paced : {false, true})
        {
            CHECK(generatedFrames(0, fsr, paced) == 0 && generatedFrames(1, fsr, paced) == 0);
            for (uint32_t m = 2; m <= 6; ++m) CHECK(generatedFrames(m, fsr, paced) == 1);
            CHECK(multiplierOffered(2, fsr, paced) && !multiplierOffered(3, fsr, paced) && !multiplierOffered(4, fsr, paced));
        }
        CHECK(multiplierOffered(3, fsr, true) == false);
        // the driver is never asked to pace FSR's presents, whatever the device
        PacingState fresh;
        CHECK(!meteringFeature(true, fg) && !meteringFeature(false, fg));
        CHECK(!driverPacing(meteringFeature(true, fg), fresh) && !driverPacingAccepted(meteringFeature(true, fg), fresh, 1));
        GeneratorCaps dg; dg.maxGenerated = 3; dg.driverPacing = true;
        CHECK(meteringFeature(true, dg) && !meteringFeature(false, dg) && driverPacing(meteringFeature(true, dg), fresh));
        // a stored multiplier above 2x runs as 2x with FSR, and says why
        CHECK(std::strcmp(cappedMultiplierText(4, fsr, false, false), "2x (FSR generates one frame per rendered frame)") == 0);
        CHECK(std::strcmp(cappedMultiplierText(6, fsr, true, true), "2x (FSR generates one frame per rendered frame)") == 0);
        CHECK(std::strcmp(cappedMultiplierText(3, fsr, false, true), "2x (FSR generates one frame per rendered frame)") == 0);
        CHECK(std::strcmp(cappedMultiplierText(2, fsr, false, false), "") == 0 && std::strcmp(cappedMultiplierText(0, fsr, false, false), "") == 0);
        CHECK(std::strcmp(cappedMultiplierText(4, Caps{false, false, 0, false, Generator::Fsr}, false, false), "") == 0);

        // availability: the same rules, but the add-on's blocks are DLSS's -- FSR has no add-on
        CHECK(blockReason(fsr, ok) == Block::None);
        CHECK(blockReason(Caps{false, false, 0, false, Generator::Fsr}, ok) == Block::Unsupported);   // (never NoAddon)
        CHECK(blockReason(Caps{false, true, 0, false, Generator::Fsr}, ok) == Block::Unsupported);
        CHECK(blockReason(Caps{false, true, 1, false, Generator::Dlss}, ok) == Block::NoAddon);
        { Context c = ok; c.platform = false; CHECK(blockReason(fsr, c) == Block::Platform); }
        { Context c = ok; c.inputsOn = false; CHECK(blockReason(fsr, c) == Block::NeedsUpscaler); }
        { Context c = ok; c.output = Output::ScRgb; CHECK(blockReason(fsr, c) == Block::NeedsHdr10); }
        { Context c = ok; c.output = Output::SdrOther; CHECK(blockReason(fsr, c) == Block::SdrFormat); }
        { Context c = ok; c.output = Output::Sdr8; CHECK(blockReason(fsr, c) == Block::None); }
        { Context c = ok; c.vr = true; CHECK(blockReason(fsr, c) == Block::Vr); }
        { Context c = ok; c.background = true; CHECK(blockReason(fsr, c) == Block::Background); }
        { Context c = ok; c.mainWindow = false; CHECK(blockReason(fsr, c) == Block::NotMainWindow); }
        for (Output o : {Output::Sdr8, Output::SdrOther, Output::Hdr10, Output::ScRgb})
            for (int bits = 0; bits < 16; ++bits)
            {
                Context c{(bits & 1) != 0, (bits & 2) != 0, o, (bits & 4) != 0, (bits & 8) != 0, true};
                const Block b = blockReason(fsr, c);
                CHECK(b != Block::NoAddon && b != Block::AddonOff && b != Block::OldAddon && b != Block::NoFgLibrary
                      && b != Block::GpuSchedulingOff && b != Block::CapsFailed);
                // the order of the blocks is the same for both generators
                CHECK(b == blockReason(rtx50, c));
            }

        // the inputs: either upscaler's mode (RenderVKUpscaler 1-6 FSR, 7-11 DLSS), not TAA, not an unknown mode
        CHECK(!upscalerWritesInputs(0) && upscalerWritesInputs(1) && upscalerWritesInputs(6) && upscalerWritesInputs(7));
        CHECK(upscalerWritesInputs(11) && !upscalerWritesInputs(12) && !upscalerWritesInputs(~0u));

        // the scratch image: FSR's shaders have no BGRA8 storage format
        CHECK(scratchFormat(Generator::Fsr, kFormatB8G8R8A8Unorm) == kFormatR8G8B8A8Unorm);
        CHECK(scratchFormat(Generator::Fsr, kFormatR8G8B8A8Unorm) == kFormatR8G8B8A8Unorm);
        CHECK(scratchFormat(Generator::Fsr, kFormatA2B10G10R10Unorm) == kFormatA2B10G10R10Unorm);
        for (uint32_t f : {kFormatB8G8R8A8Unorm, kFormatR8G8B8A8Unorm, kFormatA2B10G10R10Unorm, 97u})
            CHECK(scratchFormat(Generator::Dlss, f) == f && scratchFormat(Generator::None, f) == f);

        // the status and the log start with the generator and the multiplier in use
        CHECK(statusPrefix(Generator::Fsr, 1) == "FSR 2x" && statusPrefix(Generator::Dlss, 2) == "DLSS 3x");
        CHECK(statusPrefix(Generator::Dlss, 5) == "DLSS 6x");

        // the generator's own images join the one-set admission: the thresholds move by exactly their size
        const ImageSet plain{100, 10};
        const ImageSet with{100, 10, 40};
        CHECK(plain.generatorBytes == 0);
        CHECK(presentSlotsFor(with, true, 350 + 50, 50) == 3 && presentSlotsFor(with, true, 350 + 49, 50) == 2);
        CHECK(presentSlotsFor(with, true, 250 + 50, 50) == 2 && presentSlotsFor(with, true, 250 + 49, 50) == 0);
        CHECK(presentSlotsFor(with, false, 0, 0) == 3);
        CHECK(presentSlotsFor(ImageSet{1, 1, ~0ull}, true, ~0ull, 0) == 0);   // (no overflow)
        for (uint64_t free = 0; free < 450; ++free)
            CHECK(presentSlotsFor(with, true, free + 40, 50) == presentSlotsFor(plain, true, free, 50));
    }

    // #1544 (D8) the queue the present thread uses: its own where the graphics family has one to spare, else the
    // main queue, shared under one lock; none only where there is no usable queue at all
    {
        CHECK(presentQueueFor(true, true, false) == PresentQueue::Own);
        CHECK(presentQueueFor(true, false, false) == PresentQueue::Shared);   // one graphics queue: shared, not blocked
        CHECK(presentQueueFor(true, true, true) == PresentQueue::Shared);     // RenderVKFrameGenSharedQueue (testing)
        CHECK(presentQueueFor(true, false, true) == PresentQueue::Shared);
        for (bool spare : {false, true})
            for (bool force : {false, true})
                CHECK(presentQueueFor(false, spare, force) == PresentQueue::None);
        CHECK(presentQueueBlock(PresentQueue::None) == Block::NoPresentQueue);
        CHECK(presentQueueBlock(PresentQueue::Own) == Block::None && presentQueueBlock(PresentQueue::Shared) == Block::None);
        // a device with a main queue is never blocked for its queue
        for (bool spare : {false, true})
            for (bool force : {false, true})
                CHECK(presentQueueBlock(presentQueueFor(true, spare, force)) == Block::None);

        // the present mode: IMMEDIATE whenever it is offered -- on a shared queue a present the compositor holds
        // would hold the queue's lock, and the main thread's next submit with it
        for (bool mailbox : {false, true})
        {
            CHECK(frameGenPresentMode(true, mailbox) == PresentMode::Immediate);
            CHECK(!presentCanBlockOnCompositor(frameGenPresentMode(true, mailbox)));
            CHECK(!sharedQueueHeld(PresentQueue::Shared, frameGenPresentMode(true, mailbox)));
        }
        // ... and where it is not offered, a shared queue's chain is not built for frame generation at all
        CHECK(sharedQueueHeld(PresentQueue::Shared, frameGenPresentMode(false, true)));
        CHECK(sharedQueueHeld(PresentQueue::Shared, frameGenPresentMode(false, false)));
        // a queue of its own generates in all three modes, as before
        for (PresentMode m : {PresentMode::Immediate, PresentMode::Mailbox, PresentMode::Fifo})
            CHECK(!sharedQueueHeld(PresentQueue::Own, m) && !sharedQueueHeld(PresentQueue::None, m));
        // the reason the player reads, after the output's own
        { Context c = ok; c.queueHeld = true; CHECK(blockReason(rtx50, c) == Block::NeedsImmediate); }
        { Context c = ok; c.queueHeld = true; c.output = Output::ScRgb; CHECK(blockReason(rtx50, c) == Block::NeedsHdr10); }
        { Context c = ok; c.queueHeld = true; c.vr = true; CHECK(blockReason(rtx50, c) == Block::NeedsImmediate); }
        CHECK(!Context{}.queueHeld);
        CHECK(std::strcmp(blockText(Block::NeedsImmediate),
                          "this GPU has one graphics queue, and this display offers no immediate present mode") == 0);
        CHECK(!userCanAct(Block::NeedsImmediate));

        // the gates: a shared queue never holds a submit that waits for the pacer (it would hold the renderer's
        // submits behind it); the present thread waits the gate itself, never longer than this -- above any
        // frame the pacer paces
        CHECK(gateOnCpu(PresentQueue::Shared) && !gateOnCpu(PresentQueue::Own) && !gateOnCpu(PresentQueue::None));
        CHECK(int64_t(kGateWaitBoundMs) * 1'000'000 > kMaxPacedFrameNs);
        CHECK(kGateWaitBoundMs <= kSlotWaitBoundImmediateMs);   // (a slot wait outlasts a gate that never opens)
    }

    // Driver pacing's answer (presentConfigFeedback): 0 accepted; anything else, or left unset (~0u), refused
    CHECK(meteringAccepted(0u) && !meteringAccepted(1u) && !meteringAccepted(~0u));

    // #1559 sleepModeOffBeforeChainGoes: a chain whose Reflex sleep mode was set has it set off before it goes --
    // left on, the driver went on pacing the next chain's plain presents at the last frame time
    CHECK(sleepModeOffBeforeChainGoes(0, true));        // set (no cap) on the chain that goes
    CHECK(sleepModeOffBeforeChainGoes(33333, true));    // set with a cap
    CHECK(!sleepModeOffBeforeChainGoes(-1, true));      // never set on it (no frame Reflex paced)
    CHECK(!sleepModeOffBeforeChainGoes(0, false));      // no chain going (none, or Reflex never ran)

    if (failures) std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
