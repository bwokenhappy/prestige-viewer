/**
 * @file llvkframegenpresent.cpp
 * @brief Frame generation's present thread: see llvkframegenpresent.h.
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
#include "linden_common.h"

#include "llvkframegenpresent.h"
#include "llvkframegenlabels.h"   // #1517 Task 7e: llfgl::bufferBytes

#include "llerror.h"
#include "llvkframegen.h"
#include "vk_display_encode.h"
#include "vk_display_frame.h"   // vkDisplayTransition

#include <algorithm>
#include <chrono>

namespace
{
// A swapchain image the present thread asks for comes within this, or that frame is not shown (the
// previous batch's presents give the images back at the display's pace; this only bounds a stall).
constexpr uint64_t kAcquireTimeoutNs = 1000000000ull;   // 1 s
// The CPU wait for a slot's previous job on the GPU (normally reached long before).
constexpr uint64_t kSlotWaitNs = 1000000000ull;          // 1 s
constexpr size_t kMaxResults = 64;                       // (the render loop takes them every frame)
// Where a submit first touches an acquired swapchain image: its layout transition before the composite
// (COLOR_ATTACHMENT_OUTPUT) or before the fallback copy (COPY). Waiting there, not at ALL_COMMANDS, lets the
// generator's evaluate run while the presentation engine still holds the image.
constexpr VkPipelineStageFlags2 kAcquireStages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;

VkSemaphoreSubmitInfo sem(VkSemaphore s, uint64_t value, VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)
{
    VkSemaphoreSubmitInfo i{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    i.semaphore = s;
    i.value = value;
    i.stageMask = stages;
    return i;
}

// An acquired swapchain image (UNDEFINED) for drawing. Its source scope is the stage the acquire's
// semaphore is waited at, so the transition waits for the presentation engine.
void toColorTarget(VkCommandBuffer cmd, VkImage target)
{
    vkDisplayTransition(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kAcquireStages, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void colorTargetToPresent(VkCommandBuffer cmd, VkImage target)
{
    vkDisplayTransition(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
}

// The whole of src into dst (same size); dst left in PRESENT_SRC. The same format: a copy. Another one (#1544: FSR's
// scratch image is RGBA8 on a BGRA8 chain -- GLSL has no BGRA8 storage format): a blit, which converts the channels
// (a copy would only move bits: red and blue swapped).
void copyToPresent(VkCommandBuffer cmd, VkImage src, VkFormat srcFormat, VkImageLayout srcLayout, VkImage dst,
                   VkFormat dstFormat, VkExtent2D ext)
{
    // (the copy and the blit are both transfer commands: VK_PIPELINE_STAGE_2_COPY_BIT does not cover a blit)
    const VkPipelineStageFlags2 stage = srcFormat == dstFormat ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_BLIT_BIT;
    vkDisplayTransition(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, kAcquireStages, 0,
                        stage, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (srcFormat == dstFormat)
    {
        VkImageCopy region{};
        region.srcSubresource = layers;
        region.dstSubresource = layers;
        region.extent = {ext.width, ext.height, 1};
        vkCmdCopyImage(cmd, src, srcLayout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    else
    {
        VkImageBlit region{};
        region.srcSubresource = layers;
        region.dstSubresource = layers;
        region.srcOffsets[1] = {(int32_t)ext.width, (int32_t)ext.height, 1};
        region.dstOffsets[1] = region.srcOffsets[1];
        vkCmdBlitImage(cmd, src, srcLayout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    }
    vkDisplayTransition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        stage, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
}

uint64_t mb(uint64_t bytes) { return (bytes + (1ull << 19)) >> 20; }
} // namespace

// ---- main thread ----

bool LLVKFrameGenPresent::makeAcquireSemaphores(VkDeviceContext& dev)
{
    const VkDevice d = dev.device();
    VkSemaphoreCreateInfo bci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    bool ok = true;
    for (Slot& s : mSlots)
        for (VkSemaphore& a : s.acquire)
        {
            if (a) vkDestroySemaphore(d, a, nullptr);   // (the device is idle and the chain they were used on gone)
            a = VK_NULL_HANDLE;
            ok = ok && vkCreateSemaphore(d, &bci, nullptr, &a) == VK_SUCCESS;
        }
    mAcquireStale.store(false);
    return ok;
}

bool LLVKFrameGenPresent::makeObjects(VkDeviceContext& dev)
{
    if (mPool) return !mAcquireStale.load() || makeAcquireSemaphores(dev);
    const VkDevice d = dev.device();
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dev.queueFamily();   // (the present queue is a second queue of this family)
    bool ok = vkCreateCommandPool(d, &pci, nullptr, &mPool) == VK_SUCCESS;
    for (Slot& s : mSlots)
    {
        VkCommandBuffer cmds[4] = {};
        if (ok)
        {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = mPool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 4;
            ok = vkAllocateCommandBuffers(d, &ai, cmds) == VK_SUCCESS;
        }
        s.gen = cmds[0];
        s.real = cmds[1];
        s.blankGen = cmds[2];
        s.blankReal = cmds[3];
    }
    ok = ok && makeAcquireSemaphores(dev);
    if (ok)
    {
        VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sci.pNext = &tci;
        ok = vkCreateSemaphore(d, &sci, nullptr, &mReleased) == VK_SUCCESS;
    }
    if (ok) return true;
    destroyObjects(dev);
    return false;
}

void LLVKFrameGenPresent::destroyObjects(VkDeviceContext& dev)
{
    const VkDevice d = dev.device();
    for (Slot& s : mSlots)
    {
        for (VkSemaphore& a : s.acquire)
        {
            if (d && a) vkDestroySemaphore(d, a, nullptr);
            a = VK_NULL_HANDLE;
        }
        s.gen = s.real = s.blankGen = s.blankReal = VK_NULL_HANDLE;   // (with the pool)
        s.serial = s.prevSerial = 0;
    }
    if (d && mPool) vkDestroyCommandPool(d, mPool, nullptr);
    // (no wait names them any more; nothing of the present queue's is pending when its objects go, and a new
    // release timeline starts from nothing, as mSerial)
    mReleaseAccepted.semaphore.store(VK_NULL_HANDLE);
    mReleaseAccepted.value.store(0);
    mGeneratedAccepted.semaphore.store(VK_NULL_HANDLE);
    mGeneratedAccepted.value.store(0);
    if (d && mReleased) vkDestroySemaphore(d, mReleased, nullptr);
    mPool = VK_NULL_HANDLE;
    mReleased = VK_NULL_HANDLE;
    mSerial = 0;
}

void LLVKFrameGenPresent::releaseInputCopies()
{
    for (Slot& s : mSlots)
        for (GpuImage* i : {&s.depth, &s.motion})
        {
            if (i->image && mDev) mDev->destroyImage(*i);
            *i = GpuImage{};
        }
}

void LLVKFrameGenPresent::releaseImages()
{
    releaseInputCopies();
    for (Slot& s : mSlots)
        for (GpuImage* i : {&s.backbuffer, &s.hudless, &s.ui})
        {
            if (i->image && mDev) mDev->destroyImage(*i);
            *i = GpuImage{};
        }
    if (mScratch.image && mDev) mDev->destroyImage(mScratch);
    mScratch = GpuImage{};
    for (Slot& s : mSlots)
    {   // #1517 Task 7e (the device is idle, or the thread drained with nothing owed: see the callers)
        if (s.labelAtlas.image && mDev) mDev->destroyImage(s.labelAtlas);
        s.labelAtlas = GpuImage{};
        if (s.labelBuffer.buffer && mDev) mDev->destroyBuffer(s.labelBuffer);
        s.labelBuffer = GpuBuffer{};
    }
    collectRetiredLabels(true);   // (mRetiredLabels)
    mImagesExtent = {0, 0};
    mImagesFormat = VK_FORMAT_UNDEFINED;
    mImagesHudless = VK_FORMAT_UNDEFINED;
    mImagesScratch = VK_FORMAT_UNDEFINED;
    mImagesGeneratorBytes = 0;
    mInputsRefused = false;
}

void LLVKFrameGenPresent::collectRetiredLabels(bool all)
{
    // A retired atlas may still be read by a frame's display encode (frame done) or a job's composites
    // (released): freed once both timelines are past every use (all: the caller knows the device is idle).
    auto reached = [&](VkSemaphore sem, uint64_t value)
    {
        if (!sem || !value) return true;
        uint64_t now = 0;
        return mDev && vkGetSemaphoreCounterValue(mDev->device(), sem, &now) == VK_SUCCESS && now >= value;
    };
    for (size_t i = 0; i < mRetiredLabels.size();)
    {
        RetiredLabels& r = mRetiredLabels[i];
        uint64_t released = 0;
        const bool jobsDone = !r.releaseValue ||
            (mReleased && mDev && vkGetSemaphoreCounterValue(mDev->device(), mReleased, &released) == VK_SUCCESS &&
             released >= r.releaseValue);
        if (all || (jobsDone && reached(r.frameDone, r.frameDoneValue)))
        {
            if (r.atlas.image && mDev) mDev->destroyImage(r.atlas);
            mRetiredLabels.erase(mRetiredLabels.begin() + std::ptrdiff_t(i));
            continue;
        }
        ++i;
    }
}

void LLVKFrameGenPresent::tendLabels(uint32_t slot, bool idle, VkSemaphore frameDone, uint64_t lastFrameDone)
{
    if (!mDev) return;
    // (the caller owns `slot`: its last job has returned, so the present thread reads nothing of it -- as in
    // ensureLabels. Another slot's job may be running and reading that slot's atlas view: left alone until
    // the render loop comes round to it)
    if (idle && mReady && slot < mSlotCount && mSlots[slot].labelAtlas.image)
    {
        Slot& s = mSlots[slot];
        LL_INFOS("Vulkan") << "Frame generation: label atlas of slot " << slot << " (" << s.labelAtlas.extent.width << "x"
                           << s.labelAtlas.extent.height << ") goes back: no labels drawn for a while" << LL_ENDL;
        mRetiredLabels.push_back(RetiredLabels{s.labelAtlas, frameDone, lastFrameDone, mSerial});
        s.labelAtlas = GpuImage{};
    }
    collectRetiredLabels(false);
}

bool LLVKFrameGenPresent::ensureLabels(uint32_t slot, VkExtent2D atlas, VkSemaphore frameDone, uint64_t lastFrameDone,
                                       const Budget& budget)
{
    if (!mDev || !mReady || slot >= mSlotCount || !atlas.width || !atlas.height || !mImagesExtent.width)
        return false;
    collectRetiredLabels(false);
    Slot& s = mSlots[slot];
    VkDeviceContext& dev = *mDev;
    const bool atlasFits = s.labelAtlas.view && s.labelAtlas.extent.width == atlas.width &&
                           s.labelAtlas.extent.height == atlas.height;
    if (atlasFits && s.labelBuffer.buffer)
        return true;
    const VkDeviceSize bufferBytes =
        VkDeviceSize(llfgl::bufferBytes(llfgl::Extent{mImagesExtent.width, mImagesExtent.height}));
    uint64_t need = atlasFits ? 0 : dev.imageMemoryBytes(atlas.width, atlas.height, 1, 1, kLabelAtlasFormat,
                                                         kLabelAtlasUsage, false);
    if (!s.labelBuffer.buffer) need += bufferBytes;
    uint64_t freeBytes = 0, margin = 0;
    const bool ok = !(budget && budget(freeBytes, margin)) || (margin <= freeBytes && need <= freeBytes - margin);
    if (!ok)
        return s.labelAtlas.view && s.labelBuffer.buffer;   // (the old size, if any: labels that do not fit step)
    VkMemScope category(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(&dev);
    if (!s.labelBuffer.buffer)
    {
        s.labelBuffer = dev.createBuffer(bufferBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!s.labelBuffer.buffer)
            return false;
    }
    if (!atlasFits)
    {
        GpuImage made = dev.createImage2D(atlas.width, atlas.height, kLabelAtlasFormat, kLabelAtlasUsage,
                                          VK_IMAGE_ASPECT_COLOR_BIT);
        if (!made.view)
            return s.labelAtlas.view != VK_NULL_HANDLE;
        if (s.labelAtlas.image)
            mRetiredLabels.push_back(RetiredLabels{s.labelAtlas, frameDone, lastFrameDone, mSerial});
        LL_INFOS("Vulkan") << "Frame generation: label atlas of slot " << slot << " is " << atlas.width << "x"
                           << atlas.height << " (" << mb(dev.imageMemoryBytes(atlas.width, atlas.height, 1, 1,
                                                                             kLabelAtlasFormat, kLabelAtlasUsage, false))
                           << " MB)" << LL_ENDL;
        s.labelAtlas = made;
    }
    return true;
}

bool LLVKFrameGenPresent::imagesFit(VkExtent2D ext, VkFormat format, VkFormat hudless, VkFormat scratch) const
{
    return mSlotCount && mScratch.view && mImagesExtent.width == ext.width && mImagesExtent.height == ext.height &&
           mImagesFormat == format && mImagesHudless == hudless && mImagesScratch == scratch;
}

bool LLVKFrameGenPresent::makeImages(VkDeviceContext& dev, uint32_t slots, VkExtent2D ext, VkFormat format,
                                     VkFormat hudless, VkFormat scratch, bool sdr)
{
    VkMemScope category(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(&dev);
    auto make = [&](GpuImage& out, VkFormat f, VkImageUsageFlags usage)
    {
        out = dev.createImage2D(ext.width, ext.height, f, usage, VK_IMAGE_ASPECT_COLOR_BIT);
        return out.image && out.view;
    };
    bool ok = make(mScratch, scratch, kInputUsage | VK_IMAGE_USAGE_STORAGE_BIT);
    for (uint32_t k = 0; k < slots && ok; ++k)
    {
        Slot& s = mSlots[k];
        // The backbuffer has a swapchain image's usage: the frame renders, clears and copies through it as through one.
        ok = make(s.backbuffer, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
             make(s.hudless, hudless, sdr ? kSdrWorldUsage : kInputUsage) &&
             make(s.ui, VkDisplayEncode::kFgUiFormat, kInputUsage);
    }
    if (!ok) return false;
    mImagesExtent = ext;
    mImagesFormat = format;
    mImagesHudless = hudless;
    mImagesScratch = scratch;
    ++mImagesSerial;   // (#1544: a generator that bound the old images makes its feature again)
    return true;
}

void LLVKFrameGenPresent::fillGenSize(llfg::GenSize& size) const
{
    size.slots = mSlotCount;
    for (uint32_t k = 0; k < kMaxSlots; ++k)
    {
        const bool used = k < mSlotCount;
        size.color[k] = used ? llfg::imageRef(mSlots[k].hudless) : llfg::GpuImageRef{};
        size.depth[k] = used ? llfg::imageRef(mSlots[k].depth) : llfg::GpuImageRef{};
        size.motion[k] = used ? llfg::imageRef(mSlots[k].motion) : llfg::GpuImageRef{};
    }
    size.output = llfg::imageRef(mScratch);
    size.imagesSerial = mImagesSerial;
}

bool LLVKFrameGenPresent::settleOwedRelease(VkDeviceContext& dev)
{
    // The thread is drained: every job has returned. The last job's release is owed when no submit that reached
    // the queue signals it (the accepted value: then the GPU will, and a device wait waits it) and it was not
    // signalled from the CPU either (the counter: the fallback after the queue went idle signals at once).
    uint64_t value = 0;
    if (mHandoff.abandoned()) return false;   // (it may be inside a call on that queue, for good)
    if (!mReleased || !mSerial) return true;
    // (a look: with a job still queued or running nothing can be said yet -- its release is still to come)
    if (!mHandoff.drain(std::chrono::milliseconds(0))) return false;
    if (mReleaseAccepted.value.load() >= mSerial) return true;
    if (vkGetSemaphoreCounterValue(dev.device(), mReleased, &value) != VK_SUCCESS) return false;
    if (value >= mSerial) return true;
    // Owed. No accepted value stands for that job's last commands on the present queue, so a device wait --
    // made without that queue -- does not cover them. The thread is idle: it is stopped (started again by
    // prepare), and with no present thread the queue is this thread's to wait. Then nothing is pending on the
    // timeline and a host signal of the last job's value is allowed (it covers every job before it).
    if (!mHandoff.stop() || !dev.presentQueue() || vkQueueWaitIdle(dev.presentQueue()) != VK_SUCCESS) return false;
    VkSemaphoreSignalInfo hs{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    hs.semaphore = mReleased;
    hs.value = mSerial;
    if (vkSignalSemaphore(dev.device(), &hs) != VK_SUCCESS) return false;
    LL_INFOS("Vulkan") << "Frame generation: the slot releases owed up to " << mSerial << " were signalled" << LL_ENDL;
    return true;
}

VkPresentHandoff::SlotWait LLVKFrameGenPresent::waitSlot(uint32_t slot)
{
    using SlotWait = VkPresentHandoff::SlotWait;
    // Never without a bound, on any chain: past it the frame is skipped and the main loop goes on pumping
    // events (it learns a window is hidden; frames resume when the slot is free). The bound passing judges
    // nothing -- see checkGpu.
    const SlotWait w = mHandoff.waitSlot(slot, std::chrono::milliseconds(mBounds.slotMs));
    if (w == SlotWait::Busy)
    {
        ++mSkipped;
        if (held()) checkGpu();
        return w;
    }
    if (mSkipped >= 3)
        LL_INFOS("Vulkan") << "Frame generation: " << mSkipped << " frames skipped while the present thread had "
                           << "their slot (" << mBounds.slotMs << " ms each); the main loop went on" << LL_ENDL;
    mSkipped = 0;
    mHeldLoggedMinute = -1;
    return w;
}

VkResult LLVKFrameGenPresent::waitAccepted(VkDevice device) const
{
    // Only what a submit that succeeded will signal: an issued value whose submit never reached the queue (a
    // thread held before it, a refused submit) would never come, and waiting it would call a held present a
    // GPU hang.
    for (const Accepted* a : {&mReleaseAccepted, &mGeneratedAccepted})
    {
        const VkSemaphore semaphore = a->semaphore.load();
        const uint64_t value = a->value.load();
        if (!semaphore || !value) continue;
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &semaphore;
        wi.pValues = &value;
        // (UINT64_MAX: sliced by the device -- a GPU that makes no progress on it is its "GPU hang")
        const VkResult r = vkWaitSemaphores(device, &wi, UINT64_MAX);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

void LLVKFrameGenPresent::checkGpu()
{
    if (!mDev || !mChain || vkDeviceIsLost()) return;
    // Everything the GPU was given: the renderer's frames in flight, then the present queue's accepted work.
    // Each wait is the device's own (sliced: the hang watchdog), so a GPU that stopped is raised there, as a
    // GPU hang, with nothing special about this thread.
    if (!mChain->waitFrameSlots(*mDev) || waitAccepted(mDev->device()) != VK_SUCCESS) return;
    // They completed: the GPU is idle or advancing, and the thread sits in a call that has not returned -- a
    // present the compositor holds (a hidden window), an acquire, the generator; or the process was paused.
    // Not a hang, and never ended by the viewer: said at first and once a minute.
    const int64_t seconds = mHandoff.jobAge().count() / 1000;
    const int64_t minute = seconds / 60;
    if (minute == mHeldLoggedMinute) return;
    mHeldLoggedMinute = minute;
    LL_WARNS("Vulkan") << "Frame generation: the present thread has been inside one job for " << seconds
                       << " s (" << heldReason() << "); the GPU has finished everything it was given, so this is "
                       << "not a GPU hang: frames are skipped and rebuilds wait until that call returns" << LL_ENDL;
}

bool LLVKFrameGenPresent::drain()
{
    // One bounded try; none (a look) while the thread is held. In steps: a device lost meanwhile ends it
    // within one.
    const auto step = std::chrono::milliseconds(50);
    auto left = std::chrono::milliseconds(held() ? 0u : mBounds.drainMs);
    for (;;)
    {
        if (mHandoff.drain(std::min(left, step))) return true;
        if (left <= step || vkDeviceIsLost()) return false;
        left -= step;
    }
}

bool LLVKFrameGenPresent::prepare(VkDeviceContext& dev, VkSwapchain& chain, VkDisplayEncode& encode, const Budget& budget)
{
    mReady = false;
    mRefusal = nullptr;
    // Everything below releases, remakes or host-signals on the premise that nothing is pending: only after a
    // rebuild whose device wait succeeded (one that failed proves nothing), and never with a thread left
    // behind inside a job. Refused untouched -- the caller asks for another rebuild, after a while.
    if (!chain.deviceIdled() || mHandoff.abandoned())
    {
        mRefusal = "the device could not be waited for";
        LL_WARNS("Vulkan") << "Frame generation: " << mRefusal << " (the rebuild's device wait failed, or the present "
                           << "thread was left behind); nothing of frame generation is released or made until a "
                           << "rebuild whose wait succeeds" << LL_ENDL;
        return false;
    }
    mDev = &dev;
    mChain = &chain;
    mEncode = &encode;
    mNeedsRecreate.store(false);   // (the chain was just rebuilt)
    mInputsRefused = false;        // (a new chain tries the generator's input copies again)
    // (its present mode: the same rule the swapchain chose it by; the slot wait's bound follows it)
    mPresentMode = chain.fgPresentMode();
    mBounds.slotMs = llfg::slotWaitBoundMs(mPresentMode);
    // (first: before anything below remakes or releases what the last job's commands may still use. The render
    // loop settled it before the rebuild; this is for a caller that did not)
    if (!settleOwedRelease(dev))
    {
        // (the present queue could not be waited for: its last job may still read the images released below)
        mRefusal = "the device could not be waited for";
        LL_WARNS("Vulkan") << "Frame generation: " << mRefusal << " (a slot release is still owed and the present "
                           << "queue's wait failed); tried again with the next rebuild" << LL_ENDL;
        return false;
    }
    if (!chain.frameGenImages() || !makeObjects(dev))
    {
        mRefusal = "the present thread's objects could not be made";
        LL_WARNS("Vulkan") << "Frame generation: " << mRefusal << LL_ENDL;
        return false;
    }
    mReleaseAccepted.semaphore.store(mReleased);
    mGeneratedAccepted.semaphore.store(LLVKFrameGen::instance().generatedDone());
    const VkExtent2D ext = chain.extent();
    const VkFormat format = chain.format();
    // The generator's colour input and output: HDR10's 10-bit images, or (#1536) an 8-bit SDR chain's own format.
    const VkSurfaceFormatKHR surface{format, chain.colorSpace()};
    const VkFormat hudless = VkDisplayEncode::fgHudlessFormat(surface);
    // #1544: the scratch image -- what the generator writes -- in the generator's format (llfg::scratchFormat: FSR's
    // shaders have no BGRA8 storage format, so RGBA8 on a BGRA8 chain). Everything below uses this one.
    const VkFormat scratch = llfg::scratchFormat(LLVKFrameGen::instance().generator(), hudless);
    const bool sdr = vkdisplay::encodingOf(surface) == vkdisplay::Encoding::Sdr;
    {
        // The generator writes its frames into the scratch image as a storage image: a format the device
        // cannot do that in is refused before anything is made (Vulkan guarantees it for RGBA8 only).
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(dev.physicalDevice(), scratch, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        {
            mRefusal = sdr ? "this GPU cannot write generated frames in the display's SDR format"
                           : "this GPU cannot write generated frames in the display's HDR format";
            LL_WARNS("Vulkan") << "Frame generation: " << mRefusal << " (format " << (int)scratch << ")" << LL_ENDL;
            return false;
        }
    }
    // (#1544: the generator's own images, for the largest render size -- the output size -- it may be fed. Another
    // generator's figure -- a live switch -- admits the whole set again, as a new chain would)
    const uint64_t generatorBytes = LLVKFrameGen::instance().generatorBytes(ext, hudless, scratch);
    uint32_t slots = mSlotCount;
    if (!imagesFit(ext, format, hudless, scratch) || generatorBytes != mImagesGeneratorBytes)
    {
        // (the device is idle: no job and no frame uses the old ones). The whole set, admitted at once:
        // per slot the backbuffer, the world and the interface layer (RGBA8), and the depth / motion copies at
        // the output size (the largest the upscaler renders at); the scratch image once.
        releaseImages();
        llfg::ImageSet set;
        auto bytes = [&](VkFormat f, VkImageUsageFlags usage)
        { return dev.imageMemoryBytes(ext.width, ext.height, 1, 1, f, usage, false); };
        set.perSlot = bytes(format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT) +
                      bytes(hudless, sdr ? kSdrWorldUsage : kInputUsage) + bytes(VkDisplayEncode::kFgUiFormat, kInputUsage) +
                      bytes(kDepthAdmitFormat, kInputCopyUsage) + bytes(kMotionAdmitFormat, kInputCopyUsage);
        set.shared = bytes(scratch, kInputUsage | VK_IMAGE_USAGE_STORAGE_BIT);
        set.generatorBytes = generatorBytes;
        uint64_t freeBytes = 0, margin = 0;
        const bool known = budget && budget(freeBytes, margin);
        slots = llfg::presentSlotsFor(set, known, freeBytes, margin);
        if (!slots)
        {
            mRefusal = "not enough video memory for frame generation's images";
            LL_WARNS("Vulkan") << "Frame generation: " << mRefusal << " (" << llfg::kMinPresentSlots << " slots at "
                               << ext.width << "x" << ext.height << " need " << mb(llfg::kMinPresentSlots * set.perSlot + set.shared + set.generatorBytes)
                               << " MB + " << mb(margin) << " MB margin; " << mb(freeBytes) << " MB free)" << LL_ENDL;
            return false;
        }
        if (!makeImages(dev, slots, ext, format, hudless, scratch, sdr))
        {
            releaseImages();
            mRefusal = "no memory for frame generation's images";
            LL_WARNS("Vulkan") << "Frame generation: " << mRefusal << " (" << slots << " slots at " << ext.width << "x"
                               << ext.height << ")" << LL_ENDL;
            return false;
        }
        mImagesGeneratorBytes = generatorBytes;
        LL_INFOS("Vulkan") << "Frame generation: " << slots << " present slots, " << mb(slots * set.perSlot + set.shared)
                           << " MB of images at " << ext.width << "x" << ext.height << " (+ " << mb(set.generatorBytes)
                           << " MB admitted for the generator's own)"
                           << (known ? "" : " (no memory budget to check against)") << LL_ENDL;
    }
    if (slots != mSlotCount)
    {
        // (a new count: the thread starts again with it; it is idle and nothing is queued)
        mHandoff.stop();
        mSlotCount = slots;
        mNext = 0;
    }
    if (!mHandoff.running()) mStopping.store(false);   // (no thread: the one started now takes images again)
    if (!mHandoff.running() && !mHandoff.start(mSlotCount, [this](uint32_t slot, bool dropped) { runJob(slot, dropped); }))
    {
        mRefusal = "the present thread could not start";
        LL_WARNS("Vulkan") << "Frame generation: " << mRefusal << LL_ENDL;
        return false;
    }
    mReady = true;
    return true;
}

void LLVKFrameGenPresent::releaseChain()
{
    mReady = false;
    mNeedsRecreate.store(false);   // (asked of the frame generation chain that is gone)
    // (as prepare: the images go only once nothing of the present queue's can still read them -- never with
    // the thread left behind, nor while an owed release's queue wait fails; kept for a later rebuild then)
    // (... nor under a job: the caller drained the thread, and a look says so)
    if (mHandoff.abandoned() || !mHandoff.drain(std::chrono::milliseconds(0)) || (mDev && !settleOwedRelease(*mDev)))
    {
        LL_WARNS("Vulkan") << "Frame generation: its images are kept (the present queue could not be waited for)" << LL_ENDL;
        return;
    }
    // A normal chain has no present thread. Idle, it ends at once; from then on the present queue is the
    // device's again and a device wait is the driver's own, with its elision of a wait on an idle device
    // (#699), as before frame generation was used. Not ended (not expected): nothing goes under it.
    if (!stop())
    {
        LL_WARNS("Vulkan") << "Frame generation: its images are kept (the present thread did not end)" << LL_ENDL;
        return;
    }
    releaseImages();
}

bool LLVKFrameGenPresent::stop()
{
    // (first: from here the present thread takes no image and presents nothing more -- it may be left behind
    // inside a present to a window that goes next)
    mStopping.store(true);
    // (queued jobs: dropped, their slots released on the present queue)
    const bool ended = mHandoff.stop(std::chrono::milliseconds(mBounds.drainMs));
    mReady = false;
    mNeedsRecreate.store(false);
    if (!ended && !mAbandonedSaid)
    {
        // Still inside a call (no verdict on why): left behind with everything it uses.
        mAbandonedSaid = true;
        vkNoteThreadAbandoned();
        LL_WARNS("Vulkan") << "Frame generation: the present thread did not end within " << mBounds.drainMs
                           << " ms (inside a present, an acquire or the generator); it is left behind and nothing "
                           << "it uses is destroyed" << LL_ENDL;
    }
    return ended;
}

void LLVKFrameGenPresent::shutdown(VkDeviceContext& dev)
{
    // The thread left behind may still be inside a driver call with these objects: nothing is destroyed. Nor
    // on a lost device, as everywhere.
    if (!stop() || vkDeviceIsLost()) return;
    if (!mPool && !mReleased) return;
    // The thread is gone: this thread may wait for its last submits.
    if (dev.presentQueue() && !vkDeviceIsLost()) vkQueueWaitIdle(dev.presentQueue());
    releaseImages();
    destroyObjects(dev);
    mSlotCount = 0;
    mNext = 0;
    mChain = nullptr;
    mEncode = nullptr;
    std::lock_guard<std::mutex> lock(mResultMutex);
    mResults.clear();
}

bool LLVKFrameGenPresent::inputsFit(VkExtent2D depthExtent, VkFormat depthFormat, VkExtent2D motionExtent,
                                    VkFormat motionFormat) const
{
    auto same = [](const GpuImage& i, VkExtent2D e, VkFormat f)
    { return i.view && i.extent.width == e.width && i.extent.height == e.height && i.format == f; };
    if (!mSlotCount) return false;
    for (uint32_t k = 0; k < mSlotCount; ++k)
        if (!same(mSlots[k].depth, depthExtent, depthFormat) || !same(mSlots[k].motion, motionExtent, motionFormat))
            return false;
    return true;
}

// Driver pacing's answer to a metered batch (present thread): judged by llfg::meteringAccepted and reported to
// fg; a changed answer is logged with how many batches had the one before.
void LLVKFrameGenPresent::meteringAnswer(LLVKFrameGen& fg, uint32_t answer)
{
    if (answer != mLastMeteringAnswer && mMeteringAnswerRun)
    {
        LL_INFOS("Vulkan") << "Frame generation: driver pacing answer now 0x" << std::hex << answer << ", after "
                           << std::dec << mMeteringAnswerRun << " batch(es) answered 0x" << std::hex
                           << mLastMeteringAnswer << std::dec << LL_ENDL;
    }
    mMeteringAnswerRun = answer == mLastMeteringAnswer ? mMeteringAnswerRun + 1 : 1;
    mLastMeteringAnswer = answer;
    fg.meteringResult(llfg::meteringAccepted(answer));
}

bool LLVKFrameGenPresent::inputsRefused(VkExtent2D depthExtent, VkFormat depthFormat, VkExtent2D motionExtent,
                                        VkFormat motionFormat, uint32_t generated) const
{
    const InputKey& k = mRefusedInputs;
    return mInputsRefused && k.depth.width == depthExtent.width && k.depth.height == depthExtent.height &&
           k.depthFormat == depthFormat && k.motion.width == motionExtent.width && k.motion.height == motionExtent.height &&
           k.motionFormat == motionFormat && k.generated == generated;
}

bool LLVKFrameGenPresent::prepareInputs(VkExtent2D depthExtent, VkFormat depthFormat, VkExtent2D motionExtent,
                                        VkFormat motionFormat, uint32_t generated, const Budget& budget)
{
    if (inputsFit(depthExtent, depthFormat, motionExtent, motionFormat)) return true;
    if (!mDev || !mSlotCount || !depthExtent.width || !depthExtent.height || !motionExtent.width || !motionExtent.height ||
        inputsRefused(depthExtent, depthFormat, motionExtent, motionFormat, generated))
        return false;
    // Overlay guides raster into these very copies (never into the upscaler's).
    // Refuse before image creation on a device that cannot attach either format.
    for (VkFormat format : {depthFormat, motionFormat}) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(mDev->physicalDevice(), format, &properties);
        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
            mInputsRefused = true;
            mRefusedInputs = InputKey{depthExtent, depthFormat, motionExtent, motionFormat, generated};
            LL_WARNS("Vulkan") << "Frame generation: cannot raster overlay guides in format " << int(format) << LL_ENDL;
            return false;
        }
    }
    // (the device is idle: the old copies are no job's and no frame's any more) Every slot's, all or none.
    releaseInputCopies();
    VkDeviceContext& dev = *mDev;
    const uint64_t need = uint64_t(mSlotCount) *
        (dev.imageMemoryBytes(depthExtent.width, depthExtent.height, 1, 1, depthFormat, kInputCopyUsage, false) +
         dev.imageMemoryBytes(motionExtent.width, motionExtent.height, 1, 1, motionFormat, kInputCopyUsage, false));
    uint64_t freeBytes = 0, margin = 0;
    bool ok = !(budget && budget(freeBytes, margin)) || (margin <= freeBytes && need <= freeBytes - margin);
    if (ok)
    {
        VkMemScope category(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(&dev);
        for (uint32_t k = 0; k < mSlotCount && ok; ++k)
        {
            Slot& s = mSlots[k];
            s.depth = dev.createImage2D(depthExtent.width, depthExtent.height, depthFormat, kInputCopyUsage,
                                        VK_IMAGE_ASPECT_COLOR_BIT);
            s.motion = dev.createImage2D(motionExtent.width, motionExtent.height, motionFormat, kInputCopyUsage,
                                         VK_IMAGE_ASPECT_COLOR_BIT);
            ok = s.depth.view && s.motion.view;
        }
    }
    if (ok)
    {
        mInputsRefused = false;
        ++mImagesSerial;   // (#1544: a generator that bound the old copies makes its feature again)
        return true;
    }
    releaseInputCopies();
    mInputsRefused = true;
    mRefusedInputs = InputKey{depthExtent, depthFormat, motionExtent, motionFormat, generated};
    LL_WARNS("Vulkan") << "Frame generation: no memory for the generator's depth / motion copies (" << mSlotCount
                       << " slots, " << mb(need) << " MB; " << mb(freeBytes) << " MB free, " << mb(margin)
                       << " MB margin); tried again when their size or the multiplier changes" << LL_ENDL;
    return false;
}

void LLVKFrameGenPresent::beginInputs(VkCommandBuffer cmd, uint32_t slot) const
{
    // UNDEFINED: fully rewritten by the encode. The present thread's last reads of this slot are ordered
    // before by the frame's wait for its release (the first submission of the frame waits it).
    if (slot >= mSlotCount) return;
    for (const GpuImage* i : {&mSlots[slot].hudless, &mSlots[slot].ui})
        vkDisplayTransition(cmd, i->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void LLVKFrameGenPresent::endInputs(VkCommandBuffer cmd, uint32_t slot) const
{
    if (slot >= mSlotCount) return;
    for (const GpuImage* i : {&mSlots[slot].hudless, &mSlots[slot].ui})
        vkDisplayTransition(cmd, i->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
}

bool LLVKFrameGenPresent::push(uint32_t slot, const Job& job)
{
    if (slot >= mSlotCount || !mReady) return false;
    Slot& s = mSlots[slot];
    // (the slot is free: waitSlot -- the present thread reads none of this until the push below)
    const uint64_t prevSerial = s.prevSerial, serial = s.serial;
    s.job = job;
    s.prevSerial = s.serial;
    s.serial = ++mSerial;
    if (!mHandoff.push(slot))
    {
        s.prevSerial = prevSerial;   // (not queued: the slot's release value stays its last real job's)
        s.serial = serial;
        --mSerial;
        return false;
    }
    mNext = (slot + 1) % mSlotCount;
    return true;
}

void LLVKFrameGenPresent::takeResults(std::vector<Result>& out)
{
    std::lock_guard<std::mutex> lock(mResultMutex);
    out.insert(out.end(), mResults.begin(), mResults.end());
    mResults.clear();
}

// ---- the present thread ----

bool LLVKFrameGenPresent::waitReleased(uint64_t value)
{
    if (!value) return true;
    if (!mReleased || vkDeviceIsLost()) return false;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &mReleased;
    wi.pValues = &value;
    return vkWaitSemaphores(mDev->device(), &wi, kSlotWaitNs) == VK_SUCCESS;   // (a lost device: raised by the wait)
}

LLVKFrameGenPresent::Outcome LLVKFrameGenPresent::submit(VkCommandBuffer cmd, VkCommandBuffer blank,
                                                         const VkSemaphoreSubmitInfo* waits, uint32_t waitCount,
                                                         const VkSemaphoreSubmitInfo* signals, uint32_t signalCount,
                                                         const uint32_t* images, uint32_t imageCount)
{
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = waitCount;
    si.pWaitSemaphoreInfos = waitCount ? waits : nullptr;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = signalCount;
    si.pSignalSemaphoreInfos = signalCount ? signals : nullptr;
    // (cmd null: it could not even be begun -- as a refused submit)
    VkResult r = cmd ? vkEndCommandBuffer(cmd) : VK_ERROR_OUT_OF_HOST_MEMORY;
    if (r == VK_SUCCESS) r = vkQueueSubmit2(mDev->presentQueue(), 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) return Outcome::Drawn;
    if (vkDeviceIsLost()) return Outcome::None;   // (raised by the submit; the graceful lost-device exit)
    // Refused (out of memory; a refused submit changed no semaphore): the same waits and signals around a
    // command buffer that only takes each image to PRESENT_SRC, so every image is still presented (its
    // contents undefined, never an image in UNDEFINED) and nothing waits for ever.
    LL_WARNS("Vulkan") << "Frame generation: a present thread submit was refused (" << vkResultString(r)
                       << "); its images are shown without a picture" << LL_ENDL;
    // (`blank` is this submit's own: the job's other replacement may still be pending on the GPU)
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(blank, &bi);
    if (r == VK_SUCCESS)
    {
        for (uint32_t k = 0; k < imageCount; ++k)
            vkDisplayTransition(blank, mChain->image(images[k]), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                kAcquireStages, 0, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        r = vkEndCommandBuffer(blank);
    }
    if (r == VK_SUCCESS)
    {
        cbi.commandBuffer = blank;
        r = vkQueueSubmit2(mDev->presentQueue(), 1, &si, VK_NULL_HANDLE);
    }
    if (r == VK_SUCCESS) return Outcome::Blank;
    if (vkDeviceIsLost()) return Outcome::None;
    // Not even that (no memory: not a lost device). Its images are never presented -- nothing would signal
    // their presents' semaphores -- and stay acquired until the chain is rebuilt; their acquire semaphores
    // were signalled and never waited, so they are made again with the new chain.
    LL_WARNS("Vulkan") << "Frame generation: the replacement submit was refused too (" << vkResultString(r)
                       << "); the swapchain is rebuilt" << LL_ENDL;
    mAcquireStale.store(true);
    mNeedsRecreate.store(true);
    return Outcome::None;
}

void LLVKFrameGenPresent::signalRelease(uint64_t serial)
{
    // An empty submit after the job's own: a signal also covers every command submitted before it on the
    // queue, so the release comes once the job's reads of the slot are done. Every job, shown or not.
    if (!serial || !mReleased || vkDeviceIsLost()) return;
    const VkSemaphoreSubmitInfo release = sem(mReleased, serial);
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &release;
    if (vkQueueSubmit2(mDev->presentQueue(), 1, &si, VK_NULL_HANDLE) == VK_SUCCESS)
    {
        mReleaseAccepted.value.store(serial);   // (the queue has it: the GPU will signal it -- waitAccepted)
        return;
    }
    if (vkDeviceIsLost()) return;
    // Refused (no memory): from the CPU once the present queue -- this thread's -- has finished everything
    // submitted to it (the job's reads; its gated presents wait at most for the pacer). Never left unsignalled:
    // the renderer's next frame on this slot waits it on the GPU.
    const VkResult idle = vkQueueWaitIdle(mDev->presentQueue());
    if (vkDeviceIsLost()) return;   // (raised by the wait)
    if (idle != VK_SUCCESS)
    {
        // Not even that (no memory, not a lost device): an earlier job's release may still be pending, and a
        // host signal past it is not allowed. The chain is rebuilt -- every job until then only gives its slot
        // back. A later job's release covers this one only if its own submit reaches the queue; otherwise the
        // value stays owed (a frame of this slot waits it on the GPU) until prepare() signals it, after a
        // rebuild whose device wait succeeded (VkSwapchain::deviceIdled()) -- one whose wait failed too leaves
        // it owed, and a device wait while it is owed blocks on that frame.
        LL_WARNS("Vulkan") << "Frame generation: a slot's release could not be signalled (" << vkResultString(idle)
                           << "); the swapchain is rebuilt" << LL_ENDL;
        mNeedsRecreate.store(true);
        return;
    }
    VkSemaphoreSignalInfo hs{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    hs.semaphore = mReleased;
    hs.value = serial;
    vkSignalSemaphore(mDev->device(), &hs);
}

void LLVKFrameGenPresent::runJob(uint32_t slot, bool dropped)
{
    Slot& s = mSlots[slot];
    Result r;
    // (dropped: stop() came first; the frame's work dropped: nothing to show; a rebuild asked for: the
    // chain may be gone -- every job until the new chain only gives its slot back)
    // (Task 11b fix 1: the window hidden -- on MAILBOX / FIFO no image is taken while it is; fix 5: a stop begun)
    if (!dropped && !s.job.dropped && !mNeedsRecreate.load() && !vkDeviceIsLost() && !hiddenNow() && !stoppingNow())
        presentJob(slot, r);
    else
        mHistoryBroken = true;   // (the generator never saw this frame)
    signalRelease(s.serial);
    if (dropped) return;
    std::lock_guard<std::mutex> lock(mResultMutex);
    if (mResults.size() < kMaxResults) mResults.push_back(r);
}

void LLVKFrameGenPresent::presentJob(uint32_t slot, Result& r)
{
    LLVKFrameGen& fg = LLVKFrameGen::instance();
    VkDeviceContext& dev = *mDev;
    VkSwapchain& chain = *mChain;
    Slot& s = mSlots[slot];
    const Job& j = s.job;
    // The slot's command buffers, composite sets and acquire semaphores: its previous job's GPU work is done.
    if (!waitReleased(s.prevSerial))
    {
        r.refused = "the previous frame was not shown in time";
        mHistoryBroken = true;
        return;
    }
    const uint32_t n = j.generated;
    if (llfg::swapchainImagesNeeded(n) > chain.imageCount())
    {
        // (a batch this chain cannot carry: rebuilt for it, as an out-of-date chain)
        mNeedsRecreate.store(true);
        r.refused = "too few swapchain images";
        mHistoryBroken = true;
        return;
    }
    const VkExtent2D ext = chain.extent();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool recreate = false;
    bool withheld = false;   // (Task 11b fix 1) the window hid: an image drawn and not presented
    uint32_t images[VkSwapchain::kMaxBatchPresents] = {};
    uint32_t made = 0, got = 0;
    Outcome gen = Outcome::None;
    uint64_t generatedDone = 0;
    uint32_t ticket = 0;
    double presentMs = 0.0;
    // (Task 11b fix 1) Every image this job takes and every present it makes: never while the window cannot be
    // seen on a chain whose presents the compositor may hold (hiddenNow, read before each) -- the image is not
    // taken, or the present not made: an image already drawn then stays acquired (withheld) and the chain is
    // rebuilt, after the window shows again (llfg::deferRebuild). A present already in the driver may stay held
    // there: only this thread waits for it (the main thread's waits for this thread are all bounded).
    // (fix 5) The same once a stop has begun (stoppingNow, on any chain): the viewer is closing -- this thread may
    // be left behind, and its window destroyed -- so nothing more is taken or presented.
    // Where this thread is, for the status of a held thread (heldReason): named around each call.
    struct In
    {
        std::atomic<uint8_t>& at;
        In(std::atomic<uint8_t>& a, llfg::HeldIn in) : at(a) { at.store(static_cast<uint8_t>(in)); }
        ~In() { at.store(static_cast<uint8_t>(llfg::HeldIn::Other)); }
    };
    auto acquire = [&](VkSemaphore sem, uint32_t* image)
    {
        if (stoppingNow() || hiddenNow()) return false;
        const In in(mHeldIn, llfg::HeldIn::Acquire);
        return chain.acquireOne(dev, sem, kAcquireTimeoutNs, image, &recreate);
    };
    // Each present call, timed, between Reflex's out-of-band present markers (VK_NV_low_latency2: the
    // frame's own present is the main thread's PRESENT_START / END around push()). False: withheld.
    auto present = [&](const VkSwapchain::BatchPresent* p, uint32_t count, uint32_t metering, uint32_t* feedback)
    {
        if (stoppingNow() || hiddenNow())
        {
            withheld = true;
            return false;
        }
        bool again = false;
        fg.outOfBandMarker(j.reflexFrame, LLVKFrameGen::OobPresentStart);
        const auto t0 = std::chrono::steady_clock::now();
        {
            const In in(mHeldIn, llfg::HeldIn::Present);
            chain.presentBatch(dev, p, count, fg.gate(), metering, feedback, &again);
        }
        presentMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        fg.outOfBandMarker(j.reflexFrame, LLVKFrameGen::OobPresentEnd);
        recreate = recreate || again;
        return true;
    };

    // 1. The generated frames, each: ONE swapchain image taken, the frame generated into the scratch image
    //    and the generated world drawn into the image with this frame's interface over it. One submit for
    //    all, between Reflex's out-of-band render-submit markers. The image comes first: an image that is
    //    not free in time costs no evaluate (the generator would see a frame that is never shown), and an
    //    evaluate that fails still leaves its image a picture -- the real frame's world with its interface.
    if (n && vkBeginCommandBuffer(s.gen, &bi) == VK_SUCCESS)
    {
        fg.outOfBandMarker(j.reflexFrame, LLVKFrameGen::OobRenderStart);
        llfg::GenInputs eval = j.eval;
        eval.reset = j.eval.reset || mHistoryBroken;   // (the generator did not see the frame before this one)
        bool evalFailed = false;
        for (uint32_t k = 0; k < n && !evalFailed && !recreate; ++k)
        {
            uint32_t image = 0;
            if (!acquire(s.acquire[k], &image)) break;
            images[got++] = image;
            // The scratch image is rewritten whole, after its last reader (the composite before it, on this
            // queue: the first scope covers every earlier command of the queue).
            vkDisplayTransition(s.gen, eval.output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            {
                const In in(mHeldIn, llfg::HeldIn::Generator);
                evalFailed = !fg.evaluate(s.gen, slot, eval, k + 1, n);   // (the reason reaches the render loop)
            }
            if (!evalFailed) ++made;
            vkDisplayTransition(s.gen, eval.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                                    VK_PIPELINE_STAGE_2_BLIT_BIT,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
            // The world under the interface: the generated one, or (its evaluate failed) the real one.
            const GpuImage& world = evalFailed ? s.hudless : mScratch;
            const VkImage target = chain.image(image);
            toColorTarget(s.gen, target);
            // #1517 Task 7e: the labels where they were at this frame's moment (generated frame k + 1 of n; the
            // real world under them, when the evaluate failed: where they are now).
            // (a generator reset -- the job's, or this thread's broken history: its world has no history to move
            // between -- the labels hold still too)
            const float phase = (evalFailed || eval.reset) ? 1.f : llfgl::phase(k + 1, n);
            if (mEncode->recordFgComposite(s.gen, slot, k, world.view, j.ui, chain.imageView(image), ext,
                                           j.labels ? s.labelAtlas.view : VK_NULL_HANDLE,
                                           j.labels ? s.labelBuffer.buffer : VK_NULL_HANDLE, phase,
                                           j.paperWhiteNits, j.peakNits))
                colorTargetToPresent(s.gen, target);
            else
            {
                // (nothing recorded -- never an undefined picture: the world without the interface, in the
                // chain's own format -- HDR10's, or #1536 an 8-bit SDR chain's (fgHudlessFormat); #1544 FSR's
                // scratch image in RGBA8 on a BGRA8 chain is blitted, not copied. The transition starts from
                // UNDEFINED)
                LL_WARNS_ONCE("Vulkan") << "Frame generation: a generated frame could not be drawn with the interface; "
                                           "shown without it" << LL_ENDL;
                copyToPresent(s.gen, world.image, world.format, VK_IMAGE_LAYOUT_GENERAL, target, chain.format(), ext);
            }
        }
        if (got)
        {
            ticket = fg.timestampQuery(s.gen);   // the batch's GPU end: the last command of its submit
            // After the frame's own work (frame-done) and each image's release; signals each image's
            // render-finished (its present waits it) and the batch's generated-done value (the pacer's).
            VkSemaphoreSubmitInfo waits[1 + VkSwapchain::kMaxBatchPresents], signals[1 + VkSwapchain::kMaxBatchPresents];
            waits[0] = sem(fg.frameDone(), j.done);
            for (uint32_t k = 0; k < got; ++k)
            {
                waits[1 + k] = sem(s.acquire[k], 0, kAcquireStages);
                signals[k] = sem(chain.renderFinishedSemaphore(images[k]), 0);
            }
            generatedDone = fg.nextGeneratedDoneValue();
            signals[got] = sem(fg.generatedDone(), generatedDone);
            gen = submit(s.gen, s.blankGen, waits, 1 + got, signals, got + 1, images, got);
            // (the queue has it, drawn or blank: the GPU will signal that value -- waitAccepted; a submit
            // refused outright leaves the value issued and never signalled, and nothing waits it)
            if (gen != Outcome::None) mGeneratedAccepted.value.store(generatedDone);
        }
        else
            (void)vkEndCommandBuffer(s.gen);   // (nothing recorded but barriers; not submitted)
        fg.outOfBandMarker(j.reflexFrame, LLVKFrameGen::OobRenderEnd);
    }
    // (the generator saw this frame only when its evaluates were submitted and run)
    mHistoryBroken = !(made && gen == Outcome::Drawn);
    if (n && !got && !hiddenNow() && !stoppingNow()) r.refused = "no swapchain image free in time";
    else if (n && !made) r.refused = "the generated frames could not be made";
    if (gen == Outcome::None && got)
    {
        // (nothing reached the queue: these images are never presented; the chain is rebuilt)
        recreate = true;
        got = 0;
    }

    // 2. Their presents: the first at once (it carries the batch size when the driver paces), then the pacer
    //    gets the batch -- BEFORE any gated present (a present call may idle the queue) -- then the others,
    //    each held until its gate. Generated frame k + 1 was made for phase (k + 1) / (n + 1) of the motion
    //    and the real frame belongs at phase n / (n + 1), however many images came in time: n gate values
    //    are reserved and the pacer spaces them for n generated frames; generated frame k + 1 waits gate
    //    first + k - 1, the real frame first + n - 1; a value nobody waits for is signalled in order with
    //    the rest.
    const bool drawn = gen == Outcome::Drawn && made;
    if (got != mLastShown) fg.meteringBatchChanged();   // (another count: its first refusals forgiven)
    mLastShown = got;
    const bool driver = drawn && fg.driverPacing();
    // (no gate values for a batch that is not presented: the window hidden, a stop begun)
    const uint64_t first = (drawn && !driver && !hiddenNow() && !stoppingNow()) ? fg.pacer().nextValue(n) : 0;   // (values start at 1)
    uint32_t feedback = ~0u;
    uint32_t genShown = 0;   // the generated frames presented
    if (got)
    {
        const VkSwapchain::BatchPresent lead{images[0], 0};
        if (present(&lead, 1, driver ? got + 1 : 0, &feedback))
        {
            genShown = 1;
            if (driver)
                meteringAnswer(fg, feedback);   // (refused: this batch went out unpaced; the next one is the pacer's)
            else if (first && !fg.pacer().push({generatedDone, ticket, n, first, n}))
            {
                // The pacer has no way to signal: the gates open now, or their presents would wait for ever.
                LL_WARNS_ONCE("Vulkan") << "Frame generation: the pacer could not take a batch; its frames go out at once"
                                        << LL_ENDL;
                fg.releaseGates(first, n);
            }
            if (got > 1)
            {
                VkSwapchain::BatchPresent rest[VkSwapchain::kMaxBatchPresents];
                for (uint32_t k = 1; k < got; ++k) rest[k - 1] = {images[k], first ? first + k - 1 : 0};
                if (present(rest, got - 1, 0, &feedback)) genShown = got;
            }
        }
        else if (first)
        {
            // Reserved and never the pacer's (the lead present was withheld: the window hid, or a stop began,
            // after the values were taken): opened here, nothing waits on them. The pacer first: it may still
            // be signalling the batch before, and a lower value signalled after these is not allowed on a
            // timeline. (This thread is the pacer's pushing thread: its drain is this thread's to call; it
            // opens that batch's gates at once, within the pacer's fence bound.)
            fg.pacer().drain();
            fg.releaseGates(first, n);
        }
    }

    // 3. The real frame: its image only now -- the first generated frame went out, so the previous batch's
    //    real frame (on screen until then) comes free -- the backbuffer copied in, presented at its own time.
    //    (Outside the out-of-band render-submit markers: the real frame's copy is not generation work.)
    uint32_t real = 0;
    bool realShown = false;
    if (!recreate && !withheld && acquire(s.acquire[n], &real))
    {
        const VkImage target = chain.image(real);
        const bool begun = vkBeginCommandBuffer(s.real, &bi) == VK_SUCCESS;
        // RenderVKFrameGenShowGenerated (debug): the last generated world with the interface instead, so
        // only generated pictures are seen (its own descriptor set: index n). Refused: the real picture.
        bool shown = false;
        if (begun && j.showGenerated && drawn && got == n && made == n)
        {
            toColorTarget(s.real, target);
            shown = mEncode->recordFgComposite(s.real, slot, n, mScratch.view, j.ui, chain.imageView(real), ext,
                                               j.labels ? s.labelAtlas.view : VK_NULL_HANDLE,
                                               j.labels ? s.labelBuffer.buffer : VK_NULL_HANDLE, llfgl::phase(n, n),
                                               j.paperWhiteNits, j.peakNits);
            if (shown) colorTargetToPresent(s.real, target);
        }
        if (begun && !shown)   // (the render loop left the backbuffer in TRANSFER_SRC_OPTIMAL)
            copyToPresent(s.real, s.backbuffer.image, s.backbuffer.format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                          chain.format(), ext);
        const VkSemaphoreSubmitInfo waits[2] = {sem(fg.frameDone(), j.done), sem(s.acquire[n], 0, kAcquireStages)};
        const VkSemaphoreSubmitInfo signal = sem(chain.renderFinishedSemaphore(real), 0);
        const Outcome out = submit(begun ? s.real : VK_NULL_HANDLE, s.blankReal, waits, 2, &signal, 1, &real, 1);
        if (out != Outcome::None)
        {
            const VkSwapchain::BatchPresent last{real, first ? first + n - 1 : 0};
            realShown = present(&last, 1, 0, &feedback) && out == Outcome::Drawn;
        }
    }
    if (withheld)
    {
        // (its drawn images stay acquired: the chain is rebuilt -- after the window shows again)
        recreate = true;
        if (stoppingNow())
            LL_INFOS("Vulkan") << "Frame generation: the present thread is stopping; a batch's drawn frames were not "
                                  "presented" << LL_ENDL;
        else
            LL_INFOS("Vulkan") << "Frame generation: the window cannot be seen; a batch's drawn frames were not presented "
                                  "(the swapchain is rebuilt once it shows again)" << LL_ENDL;
    }
    if (recreate) mNeedsRecreate.store(true);
    r.shown = (realShown ? 1u : 0u) + (gen == Outcome::Drawn ? genShown : 0u);
    r.generated = made;
    r.presentMs = presentMs;
}
