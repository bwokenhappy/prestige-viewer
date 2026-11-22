/**
 * @file llvkframegenpolicy.h
 * @brief Frame generation's rules, free of the viewer: which generator runs (DLSS or FSR), when it
 *        may run (and the reason the player sees when it may not), the requested multiplier as frames
 *        to generate, a frame cap as Reflex's limit on real frames, the pacer's clock, a chain's
 *        present mode and a hidden window. Tested by scripts/check/test_framegen_policy.cpp.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLVKFRAMEGENPOLICY_H
#define LL_LLVKFRAMEGENPOLICY_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace llfg
{

// #1544 what makes the generated frames (LLVKFrameGenGenerator's backends).
enum class Generator : uint8_t { None, Dlss, Fsr };
inline const char* generatorName(Generator g)
{
    return g == Generator::Dlss ? "DLSS" : g == Generator::Fsr ? "FSR" : "";
}

// What a generator says it can do on this device, at startup (LLVKFrameGenGenerator::init).
struct GeneratorCaps
{
    uint32_t maxGenerated = 0;      // generated frames per rendered frame: 1 = up to 2x ...
    bool driverPacing = false;      // its presents may be paced by the driver (present metering)
    bool reflex = false;            // Reflex can pace its frames on this device
};

// What the generator in use and the device offer (LLVKFrameGen::initDirect).
struct Caps
{
    bool loaded = false;            // DLSS: the DLSS add-on loaded (FSR has none: not read)
    bool frameGeneration = false;   // the generator can run on this GPU / driver
    uint32_t maxGenerated = 0;      // 1 = up to 2x, 2 = 3x, 3 = 4x
    bool reflex = false;
    Generator generator = Generator::Dlss;   // which one (last: the aggregates before #1544 stay DLSS)
};

// FSR frame generation's caps: one generated frame per rendered frame (it interpolates the half-way frame),
// the viewer's own pacer always (present metering is the DLSS driver's), Reflex as the device offers it.
inline GeneratorCaps fsrGeneratorCaps(bool deviceReflex)
{
    GeneratorCaps c;
    c.maxGenerated = 1;
    c.driverPacing = false;
    c.reflex = deviceReflex;
    return c;
}

// The driver may be asked to pace the presents: the device has present metering and the generator in use
// lets the driver pace (LLVKFrameGen's mMeteringFeature; driverPacing below).
inline bool meteringFeature(bool devicePresentMetering, const GeneratorCaps& generator)
{
    return devicePresentMetering && generator.driverPacing;
}

// #1536 the output in use: what the frame is presented as.
enum class Output : uint8_t
{
    Sdr8,       // SDR in an 8-bit UNORM format (B8G8R8A8 / R8G8B8A8), sRGB-encoded: frame generation runs
    SdrOther,   // SDR in any other format (an *_SRGB surface blends in linear light): not supported
    Hdr10,      // HDR output with the HDR10 encoding: frame generation runs
    ScRgb,      // HDR output with the scRGB (FP16) encoding: not supported
};

// The outputs frame generation runs on.
inline bool outputGenerates(Output o) { return o == Output::Sdr8 || o == Output::Hdr10; }

// #1536 the SDR interface layer: SDR draws the interface into its own layer exactly while the main
// swapchain is a frame generation chain (chainImages: its generated frames, 0 on a normal chain); on a
// normal chain SDR draws the interface straight onto the world as always.
inline bool sdrLayer(Output o, uint32_t chainImages) { return o == Output::Sdr8 && chainImages != 0; }

// This frame's situation.
struct Context
{
    bool platform = false;          // a platform frame generation runs on: Windows or Linux (#1517 Task 11b)
    bool inputsOn = false;          // an upscaler that writes complete depth and motion vectors ran on the
                                    // last frame: DLSS or FSR (they exist only then)
    Output output = Output::Sdr8;   // the output of the chain actually presenting
    bool vr = false;
    bool background = false;        // the window cannot be seen (minimised, hidden, occluded: windowHidden)
    bool mainWindow = true;         // the main window's frame (not a torn-off floater window)
    bool queueHeld = false;         // #1544: the present thread shares the main queue and this display offers no
                                    // IMMEDIATE mode -- a present could hold the queue (sharedQueueHeld)
};

// Why frame generation does not run; checked in this order, the first that applies wins.
enum class Block
{
    None,
    Platform,
    NoAddon,
    AddonOff,           // the DLSS add-on is switched off (RenderVKDlssShim 0); decided at startup, not by blockReason
    Unsupported,
    NoFgLibrary,        // NVIDIA's frame generation library is not next to the add-on (capsBlock)
    GpuSchedulingOff,   // Windows' hardware-accelerated GPU scheduling is off (capsBlock)
    CapsFailed,         // the add-on could not say what the GPU can do (capsBlock)
    OldAddon,           // the add-on has no frame generation entry points
    NoPresentQueue,     // no queue can present generated frames
    NeedsUpscaler,      // no upscaler wrote depth and motion vectors (neither DLSS nor FSR upscaling runs)
    NeedsHdr10,
    SdrFormat,          // #1536 SDR in a format frame generation does not take (Output::SdrOther)
    NeedsImmediate,     // #1544 one graphics queue, and the display offers no IMMEDIATE present mode (Context::queueHeld)
    Vr,
    Background,
    NotMainWindow,
};

inline Block blockReason(const Caps& caps, const Context& c)
{
    if (!c.platform) return Block::Platform;
    if (caps.generator == Generator::Dlss && !caps.loaded) return Block::NoAddon;   // (the add-on is DLSS's only)
    if (!caps.frameGeneration || caps.maxGenerated == 0) return Block::Unsupported;
    if (!c.inputsOn) return Block::NeedsUpscaler;
    if (c.output == Output::ScRgb) return Block::NeedsHdr10;
    if (c.output == Output::SdrOther) return Block::SdrFormat;
    if (c.queueHeld) return Block::NeedsImmediate;
    if (c.vr) return Block::Vr;
    if (c.background) return Block::Background;
    if (!c.mainWindow) return Block::NotMainWindow;
    return Block::None;
}

inline const char* blockText(Block b)
{
    switch (b)
    {
    case Block::None:           return "";
    // (The add-on loads once, at startup: installing, updating or switching it on needs a restart; the
    // multiplier and on / off never do.)
    case Block::Platform:       return "only on Windows and Linux";
    case Block::NoAddon:        return "no DLSS add-on loaded at startup: install it, then restart the viewer";
    case Block::AddonOff:       return "the DLSS add-on is switched off (RenderVKDlssShim); switch it on and restart the viewer";
    case Block::Unsupported:    return "this GPU or driver cannot run frame generation";
    case Block::NoFgLibrary:    return "NVIDIA's frame generation library is missing: reinstall the DLSS add-on with the frame "
                                       "generation library, then restart the viewer";
    case Block::GpuSchedulingOff: return "Windows hardware-accelerated GPU scheduling is off: turn it on under Settings > "
                                         "System > Display > Graphics, then restart Windows";
    case Block::CapsFailed:     return "the DLSS add-on could not report what this GPU can do: rebuild it and restart the viewer";
    case Block::OldAddon:       return "the DLSS add-on is too old for frame generation; rebuild it and restart the viewer";
    case Block::NoPresentQueue: return "this GPU or driver cannot present generated frames";
    case Block::NeedsUpscaler:  return "needs DLSS or FSR upscaling";
    case Block::NeedsHdr10:     return "not with scRGB HDR output: switch Display HDR off, or use an HDR10 display mode";
    case Block::SdrFormat:      return "this display's SDR format is not supported by frame generation";
    case Block::NeedsImmediate: return "this GPU has one graphics queue, and this display offers no immediate present mode";
    case Block::Vr:             return "not in VR";
    case Block::Background:     return "paused in the background";
    case Block::NotMainWindow:  return "only in the main window";
    }
    return "";
}

// The upscalers that write the generators' inputs (RenderVKUpscaler): 1-6 FSR, 7-11 DLSS; 0 (off: no temporal resolve) writes none.
// The chosen mode, not whether it ran last frame: that changes at every login or teleport.
inline bool upscalerWritesInputs(uint32_t upscalerMode) { return upscalerMode >= 1u && upscalerMode <= 11u; }

// A startup block the player can do something about (install, switch on or rebuild the add-on, a Windows
// setting); the others say what this GPU, driver or platform cannot do.
inline bool userCanAct(Block b)
{
    return b == Block::NoAddon || b == Block::AddonOff || b == Block::NoFgLibrary || b == Block::GpuSchedulingOff
        || b == Block::CapsFailed || b == Block::OldAddon;
}

// Which generator runs (RenderVKFrameGenGenerator: 0 automatic, 1 DLSS, 2 FSR; anything else is automatic),
// from each one's startup block. Automatic: DLSS where it runs, else FSR, else none -- then the reason shown
// is FSR's, unless DLSS's is one the player can act on (FSR being blocked too, that is the way to frame
// generation). 1 / 2: that generator or none, with its own reason. reasonOf: whose reason (and detail) it is.
struct GeneratorChoice
{
    Generator generator = Generator::None;
    Block reason = Block::None;
    Generator reasonOf = Generator::None;
};
inline GeneratorChoice chooseGenerator(uint32_t override, Block dlssBlock, Block fsrBlock)
{
    const GeneratorChoice dlss{Generator::Dlss, Block::None, Generator::None};
    const GeneratorChoice fsr{Generator::Fsr, Block::None, Generator::None};
    const GeneratorChoice noDlss{Generator::None, dlssBlock, Generator::Dlss};
    const GeneratorChoice noFsr{Generator::None, fsrBlock, Generator::Fsr};
    if (override == 1u) return dlssBlock == Block::None ? dlss : noDlss;
    if (override == 2u) return fsrBlock == Block::None ? fsr : noFsr;
    if (dlssBlock == Block::None) return dlss;
    if (fsrBlock == Block::None) return fsr;
    return userCanAct(dlssBlock) ? noDlss : noFsr;
}

// The status line and the log start with the generator and the multiplier in use: "DLSS 3x", "FSR 2x".
inline std::string statusPrefix(Generator g, uint32_t generated)
{
    return std::string(generatorName(g)) + " " + std::to_string(generated + 1) + "x";
}

// The format of the image a generator writes its frames into (the present thread's scratch image), from the
// format of the world image it reads (VkDisplayEncode::fgHudlessFormat). DLSS writes that same format. FSR's
// shaders write a storage image of a declared format, and GLSL has no BGRA8 one: on a B8G8R8A8 chain its
// scratch image is R8G8B8A8 (the composite samples it, so the channel order does not show). The formats are
// VkFormat's numbers (llvkframegengenerator.h pins them): this header knows no Vulkan.
constexpr uint32_t kFormatR8G8B8A8Unorm = 37, kFormatB8G8R8A8Unorm = 44, kFormatA2B10G10R10Unorm = 64;
inline uint32_t scratchFormat(Generator g, uint32_t hudlessFormat)
{
    return (g == Generator::Fsr && hudlessFormat == kFormatB8G8R8A8Unorm) ? kFormatR8G8B8A8Unorm : hudlessFormat;
}

// The DLSS add-on's own block, decided once at startup (LLVKFrameGenDlss::init): switched off
// (RenderVKDlssShim 0: never loaded -- the reason says so, never "install it"), else not loaded, else too old
// for frame generation (no present-queue entry points), else unable to run (NGX did not start, or the GPU /
// driver runs no DLSS feature). None: the add-on can run; the GPU's own answer comes next (Caps).
inline Block addonBlock(bool loaded, bool switchedOff, bool fgEntryPoints, bool available)
{
    if (!loaded) return switchedOff ? Block::AddonOff : Block::NoAddon;
    if (!fgEntryPoints) return Block::OldAddon;
    if (!available) return Block::Unsupported;
    return Block::None;
}

// Why the add-on says frame generation cannot run (its slip_nr_fg_capabilities.unavailable_reason, add-on
// 1.3.2+; LLVKNeuralRecon checks these against the ABI's SLIP_FG_WHY_* values). An older add-on does not
// write the field: NotStated.
enum class AddonWhy : uint32_t { NotStated = 0, LibraryMissing = 1, GpuSchedulingOff = 2, QueryFailed = 3 };

// The GPU's own answer, after the add-on's (addonBlock), decided once at startup. `reported` false: the
// add-on's capability call itself failed. A stated cause blocks whatever `available` says; a cause this viewer
// does not know (a newer add-on's), or none stated (add-on 1.3.1), is the general "cannot run". None: it can.
inline Block capsBlock(bool reported, bool available, uint32_t maxGenerated, uint32_t why)
{
    if (!reported) return Block::CapsFailed;
    switch ((AddonWhy)why)
    {
    case AddonWhy::LibraryMissing:   return Block::NoFgLibrary;
    case AddonWhy::GpuSchedulingOff: return Block::GpuSchedulingOff;
    case AddonWhy::QueryFailed:      return Block::CapsFailed;
    case AddonWhy::NotStated:        break;
    }
    return (available && maxGenerated != 0) ? Block::None : Block::Unsupported;
}

// The highest multiplier, whatever RenderVKFrameGen holds. Without driver pacing the viewer's own pacer spaces
// the presents, and five or six presents per rendered frame are beyond what it does well: 4x. Only while the
// driver paces them (present metering, driverPacingAccepted below) may 5x and 6x run.
constexpr uint32_t kMaxMultiplier = 4;
constexpr uint32_t kMaxMultiplierDriverPaced = 6;

// The most generated frames per real frame this viewer runs: the add-on's answer, within the highest
// multiplier there is (kMaxMultiplierDriverPaced) and -- the second line of defence -- within what one batch of presents carries (the swapchain's kMaxBatchPresents = the generated frames + the real one). The present
// thread's per-batch arrays have that size; an answer above it must never reach them.
inline uint32_t maxGeneratedFor(uint32_t reported, uint32_t maxBatchPresents)
{
    const uint32_t batch = maxBatchPresents ? maxBatchPresents - 1 : 0;
    const uint32_t most = batch < kMaxMultiplierDriverPaced - 1 ? batch : kMaxMultiplierDriverPaced - 1;
    return reported < most ? reported : most;
}

// A failure in mid-session (the generator could not be set up, or refused a frame) stops frame generation
// until the viewer starts again. What the player reads: plain, with what brings it back. The add-on's own
// error text goes to the log only.
enum class Stop { Create, Evaluate };
inline std::string stoppedText(Stop s)
{
    return std::string("Frame generation stopped: ")
         + (s == Stop::Create ? "the frame generator could not be set up" : "the frame generator failed")
         + ". Restart the viewer to try again.";
}

// An add-on of library version `version` ("major.minor.patch") runs frame generation but predates the fixes
// of 1.3.2 (1.3.1: each thread's own error text, frame generation's library on Linux; 1.3.2: the cause it
// states when frame generation cannot run): still accepted, and the log suggests rebuilding it. False for anything else, or a version that does not parse.
inline bool addonPredatesFixes(const char* version)
{
    unsigned major = 0, minor = 0, patch = 0;
    if (!version || std::sscanf(version, "%u.%u.%u", &major, &minor, &patch) != 3) return false;
    return major == 1 && minor == 3 && patch < 2;
}

// The requested multiplier (0 / 1 = off, 2..4) as frames to generate per real frame, within
// what the GPU can do (0 = off); a request above the highest multiplier runs as that one (4x, or 6x while
// the driver paces: driverPacingActive = driverPacingAccepted).
inline uint32_t generatedFrames(uint32_t requestedMultiplier, const Caps& caps, bool driverPacingActive)
{
    if (!caps.frameGeneration || requestedMultiplier < 2) return 0;
    const uint32_t top = driverPacingActive ? kMaxMultiplierDriverPaced : kMaxMultiplier;
    const uint32_t want = (requestedMultiplier < top ? requestedMultiplier : top) - 1;
    return want < caps.maxGenerated ? want : caps.maxGenerated;
}

// A frame cap counts the frames on screen; Reflex limits the real ones: with N generated frames
// each real frame may take (N + 1) / cap seconds. 0 = no cap.
inline uint32_t frameLimitUs(uint32_t generated, float capFps)
{
    if (!(capFps > 0.f)) return 0;
    return (uint32_t)std::lround(1.0e6 * (double)(generated + 1) / (double)capFps);
}

// The generator must drop its frame history (the evaluate's reset) unless the last evaluated frame
// is the one right before this one: after a teleport or loading screen, a snapshot, or generation
// coming back on, it would otherwise interpolate against an old frame. lastEvaluated 0: never.
inline bool historyReset(bool rendererReset, uint64_t lastEvaluated, uint64_t frame)
{
    return rendererReset || lastEvaluated == 0 || lastEvaluated + 1 != frame;
}

// ---- Pacing the generated frames ourselves (all times in nanoseconds) ----

// A frame this long or longer is not paced (a stall, a loading screen).
constexpr int64_t kMaxPacedFrameNs = 100'000'000;
inline bool paceable(int64_t frameToFrameNs)
{
    return frameToFrameNs > 0 && frameToFrameNs < kMaxPacedFrameNs;
}

// The gap between presents: the real frame interval split over the generated frames and the real one.
inline int64_t presentSpacingNs(int64_t frameToFrameNs, uint32_t generated)
{
    return frameToFrameNs / ((int64_t)generated + 1);   // (64-bit: no wrap to a division by zero)
}

// Presents of a batch in order: generated 1..N, then the real frame (index N). The first is
// released at once (due 0 = no gate); index k >= 1 is due at doneNs + k * spacing.
inline int64_t dueNs(int64_t doneNs, int64_t spacingNs, uint32_t index)
{
    return index == 0 ? 0 : doneNs + (int64_t)index * spacingNs;
}

// A GPU timestamp on the CPU clock, from one reference pair; ticks before the reference give a
// time before refCpuNs. Rounded to the nearest nanosecond.
inline int64_t gpuToCpuNs(uint64_t gpuTicks, uint64_t refGpuTicks, int64_t refCpuNs, double nsPerGpuTick)
{
    const double delta = gpuTicks >= refGpuTicks ? (double)(gpuTicks - refGpuTicks)
                                                 : -(double)(refGpuTicks - gpuTicks);
    return refCpuNs + (int64_t)std::llround(delta * nsPerGpuTick);
}

// Swapchain images a batch of this many generated frames per real one needs (the reference
// implementation's sizing). The renderer never holds one: it renders into its own backbuffers, and the
// present thread takes each swapchain image just before that frame is shown -- the generated frames'
// one by one, then the real frame's after the first generated frame has been presented (which frees the
// previous batch's real frame, on screen until then). (#1517 Task 7c: holding the real frame's image from
// the frame's start, 4x needed 9 of an 8-image display and lost a quarter of its real frames.)
inline uint32_t swapchainImagesNeeded(uint32_t generated)
{
    return generated + 1;
}

// The most generated frames (up to wantGenerated) the swapchain's image count allows; 0 if none.
inline uint32_t generatedForImages(uint32_t wantGenerated, uint32_t imageCount)
{
    if (imageCount < 2) return 0;
    return wantGenerated < imageCount - 1 ? wantGenerated : imageCount - 1;   // (swapchainImagesNeeded(g) <= imageCount)
}

// Frame generation's images (#1517): per present slot (a backbuffer, the generator's inputs) and shared
// (the scratch image; #1544 the generator's own images, LLVKFrameGenGenerator::imageBytes -- 0 for DLSS).
// They are admitted as ONE set against the device's free video memory, never one slot admitted and another
// refused (that alternated 4x and 1x).
struct ImageSet
{
    uint64_t perSlot = 0;
    uint64_t shared = 0;
    uint64_t generatorBytes = 0;
};
constexpr uint32_t kMaxPresentSlots = 3;   // one for the renderer while two batches are with the present thread
constexpr uint32_t kMinPresentSlots = 2;   // fewer and the renderer waits for every job

// The present slots frame generation gets: kMaxPresentSlots when the whole set for that many fits
// `freeBytes` with `margin` left over, else kMinPresentSlots, else 0 (refused). budgetKnown false (no
// VK_EXT_memory_budget): kMaxPresentSlots -- nothing to check against (every allocation is soft anyway).
inline uint32_t presentSlotsFor(const ImageSet& s, bool budgetKnown, uint64_t freeBytes, uint64_t margin)
{
    if (!budgetKnown) return kMaxPresentSlots;
    if (margin > freeBytes) return 0;
    const uint64_t room = freeBytes - margin;
    for (uint32_t slots = kMaxPresentSlots; slots >= kMinPresentSlots; --slots)
    {
        if (s.perSlot > room / slots) continue;            // (no overflow: slots * perSlot <= room)
        const uint64_t rest = room - slots * s.perSlot;
        if (s.shared <= rest && s.generatorBytes <= rest - s.shared) return slots;
    }
    return 0;
}

// Driver pacing (present metering). The driver accepts it -- no application id needed -- only while the window
// is shown directly: fullscreen (exclusive, or a borderless window covering the display with nothing over it).
// A window the desktop composes is refused, and so are a new swapchain's first batches. Hence:
//   - refusals for kMeteringWarmupNs after a reset (a new chain, another batch size) are forgiven;
//   - a counted refusal ends driver pacing at once (5x / 6x fall to 4x the next frame);
//   - 5x / 6x run again only after kPacingRaiseNs of accepted batches (a menu or tooltip over the fullscreen
//     window does not flip the multiplier, and the swapchain rebuild that comes with it, back and forth);
//   - after kMeteringAskLimit refusals in a row it stops asking (each refused batch goes out unpaced) until a
//     new chain, or -- in fullscreen, where acceptance can come back without a rebuild -- it asks again with ONE
//     batch every kMeteringRetryNs (more would judder a fullscreen window the desktop composes).
constexpr uint32_t kMeteringAskLimit = 30;
constexpr uint64_t kMeteringWarmupNs = 500'000'000ull;
constexpr uint64_t kPacingRaiseNs = 2'000'000'000ull;
constexpr uint64_t kMeteringRetryNs = 3'000'000'000ull;
struct PacingState
{
    uint32_t refusedInARow = 0;
    uint64_t warmupUntilNs = 0;         // refusals before this are forgiven (0 once a batch was accepted)
    bool accepted = false;              // the driver paces the presents now
    uint64_t acceptedSinceNs = 0;       // since when, without a counted refusal
    uint64_t stoppedAtNs = 0;           // when it stopped asking (refusedInARow reached kMeteringAskLimit)
    bool ask() const { return refusedInARow < kMeteringAskLimit; }
    void reset(uint64_t nowNs)          // a new chain, another multiplier
    {
        refusedInARow = 0;
        warmupUntilNs = nowNs + kMeteringWarmupNs;
    }
    void batchChanged(uint64_t nowNs)   // another batch size: forgiven a while, but never asked again by itself
    {
        if (ask()) warmupUntilNs = nowNs + kMeteringWarmupNs;
    }
    void retry(uint64_t /*nowNs*/)      // one batch asks again: refused, it stops asking at once
    {
        refusedInARow = kMeteringAskLimit - 1;
        warmupUntilNs = 0;
    }
    void result(bool ok, uint64_t nowNs)
    {
        if (ok)
        {
            refusedInARow = 0;
            warmupUntilNs = 0;
            if (!accepted) { accepted = true; acceptedSinceNs = nowNs; }
            return;
        }
        if (++refusedInARow == kMeteringAskLimit) stoppedAtNs = nowNs;
        if (nowNs < warmupUntilNs) return;
        accepted = false;
    }
    bool raised(uint64_t nowNs) const { return accepted && ask() && nowNs - acceptedSinceNs >= kPacingRaiseNs; }
    bool retryDue(bool fullscreen, uint64_t nowNs) const
    {
        return !ask() && fullscreen && nowNs - stoppedAtNs >= kMeteringRetryNs;
    }
};

// The driver's answer to a metered batch (VkSetPresentConfigNV::presentConfigFeedback; the spec says only that
// it "will return the success or error status"): the driver writes it, 0 when it paces the batch (seen in world,
// both pre-set values coming back changed). It is pre-set to ~0u, so an answer left unset reads as a refusal.
inline bool meteringAccepted(uint32_t answer) { return answer == 0u; }

// The driver is asked to pace the presents whenever the device has present metering, while it still asks.
inline bool driverPacing(bool meteringFeature, const PacingState& g)
{
    return meteringFeature && g.ask();
}

// Driver pacing is accepted right now, for long enough (PacingState::raised): only then do 5x and 6x run.
inline bool driverPacingAccepted(bool meteringFeature, const PacingState& g, uint64_t nowNs)
{
    return driverPacing(meteringFeature, g) && g.raised(nowNs);
}

// Preferences offers this multiplier: 2x..4x as far as the GPU generates, 5x and 6x only while the driver paces
// (they are not shown otherwise).
inline bool multiplierOffered(uint32_t multiplier, const Caps& caps, bool driverPacingActive)
{
    if (multiplier < 2 || !caps.frameGeneration || multiplier - 1 > caps.maxGenerated) return false;
    return multiplier <= (driverPacingActive ? kMaxMultiplierDriverPaced : kMaxMultiplier);
}

// A RenderVKFrameGen above 4x that runs as 4x because the driver does not pace: what the status says ("" when
// that is not the case -- also on a GPU whose own maximum is lower). FSR: a stored multiplier above 2x runs
// as 2x, and the status says why.
// meteringFeature: the device has present metering, so what is missing is the window being shown directly;
// without it the driver is too old to pace at all.
inline const char* cappedMultiplierText(uint32_t requestedMultiplier, const Caps& caps, bool driverPacingActive,
                                        bool meteringFeature)
{
    if (caps.generator == Generator::Fsr)
        return (requestedMultiplier > 2 && generatedFrames(requestedMultiplier, caps, driverPacingActive) == 1)
                   ? "2x (FSR generates one frame per rendered frame)" : "";
    const bool capped = requestedMultiplier > kMaxMultiplier && !driverPacingActive
                     && generatedFrames(requestedMultiplier, caps, false) == kMaxMultiplier - 1;
    if (!capped) return "";
    return meteringFeature ? "4x (5x and 6x need fullscreen)" : "4x (5x and 6x need a newer NVIDIA driver)";
}

// ---- Linux (#1517 Task 11b): the pacer's clock, the present mode, a hidden window ----

// The CPU clock the pacer runs on: the time domain the GPU's timestamps are calibrated against
// (vkGetCalibratedTimestampsKHR), and the clock its "now" and its sleeps read -- always the same one.
enum class PacerClock { None, Qpc, Monotonic, MonotonicRaw };
// Windows: QueryPerformanceCounter. Linux: CLOCK_MONOTONIC when the device calibrates against it (it has an
// absolute sleep, clock_nanosleep with TIMER_ABSTIME), else CLOCK_MONOTONIC_RAW (relative sleeps). None: the
// device calibrates against no clock of this platform (frame generation is not set up).
inline PacerClock pacerClock(bool windows, bool qpc, bool monotonic, bool monotonicRaw)
{
    if (windows) return qpc ? PacerClock::Qpc : PacerClock::None;
    return monotonic ? PacerClock::Monotonic : monotonicRaw ? PacerClock::MonotonicRaw : PacerClock::None;
}
inline const char* pacerClockName(PacerClock c)
{
    switch (c)
    {
    case PacerClock::Qpc:          return "the performance counter";
    case PacerClock::Monotonic:    return "CLOCK_MONOTONIC";
    case PacerClock::MonotonicRaw: return "CLOCK_MONOTONIC_RAW";
    case PacerClock::None:         break;
    }
    return "none";
}
// The pacer sleeps to an absolute deadline on this clock (else a relative sleep, then a spin).
inline bool pacerSleepsAbsolute(PacerClock c) { return c == PacerClock::Monotonic; }
// A timespec (seconds, nanoseconds) in nanoseconds, and back (ns >= 0).
inline int64_t timespecNs(int64_t sec, int64_t nsec) { return sec * 1'000'000'000LL + nsec; }
inline void splitNs(int64_t ns, int64_t& sec, int64_t& nsec)
{
    sec = ns / 1'000'000'000LL;
    nsec = ns % 1'000'000'000LL;
}
// The pacer's wait for a due time `leftNs` away: a sleep of at most 1 ms (a drain is seen between them)
// until 1 ms before, then a spin (0: spin now). Sleeps wake late by up to the timer's slack; the spin does not.
constexpr int64_t kPacerSpinNs = 1'000'000;
constexpr int64_t kPacerSleepStepNs = 1'000'000;
inline int64_t pacerSleepNs(int64_t leftNs)
{
    const int64_t s = leftNs - kPacerSpinNs;
    return s <= 0 ? 0 : s < kPacerSleepStepNs ? s : kPacerSleepStepNs;
}

// A frame generation chain's present mode: IMMEDIATE first (each present shows at once; the pacer or the
// driver spaces them), else MAILBOX (common on Wayland without tearing control: a newer present can replace
// one not yet shown), else FIFO (each present waits for its vblank: the present thread blocks per frame).
// Generation runs in all three.
enum class PresentMode { Fifo, Mailbox, Immediate };
inline PresentMode frameGenPresentMode(bool immediateOffered, bool mailboxOffered)
{
    return immediateOffered ? PresentMode::Immediate : mailboxOffered ? PresentMode::Mailbox : PresentMode::Fifo;
}
inline const char* presentModeText(PresentMode m)
{
    switch (m)
    {
    case PresentMode::Immediate: return "IMMEDIATE (each present shows at once; the pacer spaces them)";
    case PresentMode::Mailbox:   return "MAILBOX (a newer present can replace one not yet shown; no tearing)";
    case PresentMode::Fifo:      return "FIFO (each present waits for a vblank: the present thread blocks per frame)";
    }
    return "?";
}
// A present call in this mode may be held by the compositor (Wayland: while the window cannot be seen) --
// FIFO, and MAILBOX on some drivers; IMMEDIATE never waits.
inline bool presentCanBlockOnCompositor(PresentMode m) { return m != PresentMode::Immediate; }
// (Task 11b fix 1, M3) The swapchain images a frame generation chain asks for: a batch's (swapchainImagesNeeded),
// and one more on MAILBOX / FIFO -- with only those, every acquire waits for the compositor to let go of the
// image shown before, so MAILBOX would never replace a frame not yet shown. Never below the surface's minimum,
// never above its maximum (VkSwapchain clamps; the extra image is not required: generatedForImages).
inline uint32_t swapchainImagesFor(uint32_t generated, PresentMode m)
{
    return swapchainImagesNeeded(generated) + (m == PresentMode::Immediate ? 0u : 1u);
}
// On MAILBOX / FIFO no more frames are shown than the display refreshes: the multiplier is refresh-bound (a
// 3x / 4x batch on a 60 Hz display shows at most 60 frames a second; MAILBOX drops the rest, FIFO waits).
inline bool refreshBound(PresentMode m) { return m != PresentMode::Immediate; }

// ---- #1544 (D8) The queue the present thread submits and presents on ----
// Its own -- a second queue of the graphics family -- where the family has one to spare. Many drivers give the
// graphics family exactly one queue (Mesa's RADV and ANV state it in their source); there the present thread
// shares the renderer's queue, and every call on it goes through one lock (VkDeviceContext::QueueLock). None
// only where there is no usable queue at all (no desktop device: VR's, another platform's).
enum class PresentQueue : uint8_t { None, Own, Shared };
// mainQueue: a desktop device with its graphics queue; spareQueue: the family has a queue beyond the viewer's
// own; forceShared: RenderVKFrameGenSharedQueue (the shared mode on a GPU that has a spare queue, for testing).
inline PresentQueue presentQueueFor(bool mainQueue, bool spareQueue, bool forceShared)
{
    if (!mainQueue) return PresentQueue::None;
    return (spareQueue && !forceShared) ? PresentQueue::Own : PresentQueue::Shared;
}
// The device's answer, read back (VkDeviceContext::presentQueue() / presentQueueShared()).
inline PresentQueue presentQueueOf(bool hasPresentQueue, bool shared)
{
    return !hasPresentQueue ? PresentQueue::None : shared ? PresentQueue::Shared : PresentQueue::Own;
}
inline Block presentQueueBlock(PresentQueue q) { return q == PresentQueue::None ? Block::NoPresentQueue : Block::None; }
// A present call holds the shared queue's lock for as long as the driver stays inside it, and the renderer's
// next submit waits for that lock without a bound. So a shared queue presents generated frames only in a mode
// whose presents never wait for the compositor (IMMEDIATE: frameGenPresentMode takes it whenever it is
// offered); on any other chain frame generation does not run (Block::NeedsImmediate) -- the main thread's only
// wait for the present thread stays the bounded slot wait. A queue of its own generates in all three modes.
inline bool sharedQueueHeld(PresentQueue q, PresentMode m)
{
    return q == PresentQueue::Shared && presentCanBlockOnCompositor(m);
}
// The pacer's gates. On its own queue the present thread submits each gated present's wait to the queue (the
// GPU holds the present until the pacer signals). A queue runs its submits in order, so on a shared queue such
// a wait would hold every submit of the renderer behind it until the gate opens: there the present thread
// waits the gate itself, then presents -- for at most kGateWaitBoundMs (above any frame the pacer paces; a
// gate that never opens costs that long once, never a frame: the slot wait's bound is longer).
inline bool gateOnCpu(PresentQueue q) { return q == PresentQueue::Shared; }
constexpr uint32_t kGateWaitBoundMs = 200;

// The main window cannot be seen: minimised, hidden, or occluded (SDL's window flags; on Wayland SDL reports
// the compositor's "suspended" state -- another workspace, fully covered -- as occluded). Linux only: on
// Windows the main loop already skips a minimised or hidden window, as before frame generation ran on Linux.
// While it is hidden the render loop hands no frame over to present (its job is dropped), generation stops
// through the context (background: Block::Background), and the present thread -- told at once, before
// anything waits for it -- takes no image and presents nothing on a MAILBOX / FIFO chain.
inline bool windowHidden(bool minimised, bool hidden, bool occluded) { return minimised || hidden || occluded; }

// The main thread never waits without a bound for the present thread, on any chain, and a bound that passes only
// drops or defers -- it never judges (the one stall rule, Task 7c fix 4): a job may sit inside a present the
// compositor holds (Wayland, on MAILBOX / FIFO: while the window cannot be seen, possibly before SDL says so),
// an acquire or the generator for as long as that takes. Whether the GPU hung is decided only by the device's
// own waits (vk_hang_watchdog.h), on work a queue accepted.
// The wait for a slot: at most this long, then the frame is not rendered (nothing changes: the slot is left as
// it was, and the generator skipped nothing -- lastEvaluatedAfterSkip) and the main loop goes on pumping events.
// On MAILBOX / FIFO a few frame intervals (at 4x on a 60 Hz FIFO chain one job holds 4 vblanks, 67 ms); on
// IMMEDIATE, where no present waits for the display, well above a healthy job at low frame rates.
constexpr uint32_t kSlotWaitBoundMs = 100;
constexpr uint32_t kSlotWaitBoundImmediateMs = 250;
inline uint32_t slotWaitBoundMs(PresentMode m) { return m == PresentMode::Immediate ? kSlotWaitBoundImmediateMs : kSlotWaitBoundMs; }
// The present thread is held: one job has run for this long (a healthy job is a few frame intervals; its own
// waits for an image or for the slot's last job are 1 s at most). From then on the main thread checks the GPU
// each time a slot wait passes its bound -- through the device's sliced waits, on everything a queue accepted --
// and a drain no longer waits for the thread at all (it looks, and the caller defers).
constexpr int64_t kHeldJobMs = 1000;
// A drain before something the present thread uses is released or rebuilt (the swapchain, the generator's
// feature, its input copies, exclusive fullscreen, shutdown): one try of at most this long (a healthy job: 1 s
// of acquires, 1 s for the slot's last job), none while the thread is held. Not drained: deferred, nothing
// released. The same bound for the thread to end at shutdown, before it is left behind.
constexpr uint32_t kDrainBoundMs = 2000;
// The main swapchain's rebuild waits (the request kept) on a frame generation chain whose presents the
// compositor may hold, while the window is hidden or the present thread is held: a swapchain is never rebuilt
// under a present call into it (Vulkan's external synchronisation). (On IMMEDIATE the drain before the rebuild
// decides: a thread that is held there is not drained.)
inline bool deferRebuild(bool frameGenChain, PresentMode m, bool windowIsHidden, bool held)
{
    return frameGenChain && presentCanBlockOnCompositor(m) && (windowIsHidden || held);
}
// Where a present thread that is held sits, for the status text (never a verdict): the display holding a frame
// (a present the compositor holds), a swapchain image that does not come, the generator -- or anywhere else
// (its own bounded waits, or the process was paused: the neutral text).
enum class HeldIn : uint8_t { Other, Acquire, Generator, Present };
inline const char* heldText(HeldIn in)
{
    switch (in)
    {
    case HeldIn::Present:   return "waiting for the display to take a frame";
    case HeldIn::Acquire:   return "waiting for a free swapchain image";
    case HeldIn::Generator: return "waiting for the frame generator";
    case HeldIn::Other:     break;
    }
    return "frame generation is waiting";
}
// A frame that was not rendered at all (its slot wait passed its bound): the generator skipped nothing, so its
// history carries over the frame -- the last frame evaluated counts as this one when it was the one right
// before (historyReset then sees no gap); a real gap stays one.
inline uint64_t lastEvaluatedAfterSkip(uint64_t lastEvaluated, uint64_t frame)
{
    return (lastEvaluated && lastEvaluated + 1 == frame) ? frame : lastEvaluated;
}
// #1559 Reflex's low latency sleep mode is set off on a chain before it goes, when it was set on it
// (modeSetUs: the minimumIntervalUs last set, -1 never): left on, the driver went on pacing the next chain's
// plain presents at the last frame time -- with frame generation turned off, the main thread blocked in the
// present and the GPU idle, until frame generation came back.
inline bool sleepModeOffBeforeChainGoes(int64_t modeSetUs, bool chainGoing) { return chainGoing && modeSetUs >= 0; }

} // namespace llfg

#endif // LL_LLVKFRAMEGENPOLICY_H
