#include "vk_swapchain.h"
#include "vk_enumerate.h"

#include "llvkframegenpolicy.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <new>

bool VkSwapchain::checkResult(VkResult result) const
{
    if (result == VK_SUCCESS) return true;
    if (result > VK_SUCCESS)
    {
        // Enumeration churn / readiness are success statuses, never LL_ERRS.
        std::fprintf(stderr, "[vkgfx] %s swapchain operation deferred: %s (%d)\n",
                     mUIOnly ? "auxiliary" : "main", vkResultString(result), int(result));
        return false;
    }
    // Keep the main-window error policy intact. Auxiliary allocation failures
    // return to their owner, which leaves the existing floater in the viewer.
    if (!mUIOnly) VK_CHECK(result);
    std::fprintf(stderr, "[vkgfx] auxiliary swapchain allocation: %s\n", vkResultString(result));
    if (result == VK_ERROR_DEVICE_LOST)
        vkDeviceLostRaise("auxiliary swapchain allocation", result, "graphics", __FILE__, __LINE__);
    return false;
}

bool VkSwapchain::init(VkDeviceContext& ctx, VkSurfaceKHR surface,
                       uint32_t width, uint32_t height, uint32_t framesInFlight) {
    mSurface = surface;
    mMaintenance = mMaintenance || ctx.swapchainMaintenance();
    mFrame = 0;
    mUIFailed = false;
    mPresentPending = false;
    mUnresolvedPresent = false;
    mFramesInFlight = framesInFlight ? framesInFlight : kDefaultFramesInFlight;

    // Sanity: confirm the graphics queue family can present to this surface.
    VkBool32 presentSupported = VK_FALSE;
    if (!checkResult(vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physicalDevice(), ctx.queueFamily(),
                                         mSurface, &presentSupported))) return false;
    if (!presentSupported) {
        std::fprintf(stderr,
            "[vkgfx] queue family %u does not support present to surface\n",
            ctx.queueFamily());
        return false;
    }

    // createSwapchainObjects() also (re)creates the per-IMAGE render-finished
    // semaphores, since the image count is only known there and can change on
    // recreate().
    if (!createSwapchainObjects(ctx, width, height)) return false;

    // Per-frame-in-flight sync objects: image-available semaphore + in-flight
    // fence. (render-finished is per-image; owned by createSwapchainObjects.)
    if (mUIOnly) mSubmissionArmed.resize(mFramesInFlight, false);
    mImageAvailable.resize(mFramesInFlight);
    mInFlight.resize(mFramesInFlight);
    if (mUIOnly)
    {
        mAcquireArmed.resize(mFramesInFlight, false);
        mAcquireFences.resize(mFramesInFlight);
    }
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first acquire doesn't hang
    for (uint32_t i = 0; i < mFramesInFlight; ++i) {
        if (!checkResult(vkCreateSemaphore(ctx.device(), &sci, nullptr, &mImageAvailable[i])) ||
            !checkResult(vkCreateFence(ctx.device(), &fci, nullptr, &mInFlight[i])))
            return false;
        if (mUIOnly && !checkResult(vkCreateFence(ctx.device(), &fci, nullptr, &mAcquireFences[i]))) return false;
    }
    return true;
}

bool VkSwapchain::sVSync = true;
uint32_t VkSwapchain::sNoVsyncMode = 0;

bool VkSwapchain::createSwapchainObjects(VkDeviceContext& ctx, uint32_t width,
                                         uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    if (!checkResult(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        ctx.physicalDevice(), mSurface, &caps))) return false;

    // Choose a format: prefer BGRA8 SRGB, else the first available.
    std::vector<VkSurfaceFormatKHR> fmts;
    const VkResult formatResult = vkenum::collect(fmts,
        [&](uint32_t* count, VkSurfaceFormatKHR* formats)
        { return vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), mSurface, count, formats); },
        "vkGetPhysicalDeviceSurfaceFormatsKHR", 256);
    if (!checkResult(formatResult) || fmts.empty()) return false;
    const uint32_t fmtCount = uint32_t(fmts.size());
    if (fmtCount == 1 && fmts[0].format == VK_FORMAT_UNDEFINED &&
        fmts[0].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        fmts[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    // An HDR-only first entry cannot be used as an SDR fallback: SDR shader bytes
    // on that target would be interpreted as PQ or absolute linear luminance.
    mFormat = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            mFormat = f; break;
        }
    }
    if (mFormat.format == VK_FORMAT_UNDEFINED) {
        for (const auto& f : fmts) {
            if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_SRGB ||
                 f.format == VK_FORMAT_B8G8R8A8_SRGB) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                mFormat = f; break;
            }
        }
    }
    if (mFormat.format == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr, "[vkgfx] surface offers no supported SDR fallback\n");
        return false;
    }
    const VkSurfaceFormatKHR sdrFormat = mFormat;
    mDisplayStatus = vkdisplay::choose(mDisplayRequest, mDisplayCapability, fmts,
                                      mFormat, mLinearOutputReady);
    mFormat = mDisplayStatus.actual;
    if (mDisplayRequest.enabled)
        std::fprintf(stderr, "[vkgfx] HDR display: %s (format %u, colour space %u)\n",
                     vkdisplay::reasonText(mDisplayStatus.reason), unsigned(mFormat.format), unsigned(mFormat.colorSpace));

    // Present mode: FIFO is always available (vsync). With vsync off take MAILBOX
    // (latest frame wins, no tearing), else IMMEDIATE (may tear), else FIFO; with
    // sNoVsyncMode 1 the order of the first two flips (IMMEDIATE bypasses a compositor
    // that releases MAILBOX images on its own cadence).
    // #1517: a chain built for frame generation presents its own batches (setFrameGen): never
    // vsync, IMMEDIATE first -- the pacer or the driver spaces the frames (G-Sync, or the driver's
    // own vsync, keeps it tear-free) -- else MAILBOX (Wayland without tearing control), else FIFO
    // (llfg::frameGenPresentMode; generation runs in all three).
    // #1544 (D8): where the present thread shares the renderer's queue, only an IMMEDIATE chain is built for
    // frame generation (llfg::sharedQueueHeld: a present the compositor holds would hold the queue's lock, and
    // the renderer's next submit with it). The modes are asked for on every main chain of such a device, so the
    // render loop knows before it asks for a chain (frameGenQueueHeld: Block::NeedsImmediate).
    const llfg::PresentQueue fgQueue = llfg::presentQueueOf(ctx.presentQueue() != VK_NULL_HANDLE, ctx.presentQueueShared());
    const bool sharedQueue = !mUIOnly && fgQueue == llfg::PresentQueue::Shared;
    uint32_t fg = (!mUIOnly && ctx.presentQueue()) ? mFgWanted : 0;
    mFgMax = fg;
    mFgImages = 0;
    mFgPresentMode = fg != 0;   // (ignoresVsync; kept when the objects fail)
    mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (mUIOnly || !sVSync || fg || sharedQueue)
    {
        std::vector<VkPresentModeKHR> pms;
        // #919: with a full-screen-exclusive chain, the modes valid for it.
        if (!(mCreateChain && mPresentModeQuery && mPresentModeQuery(pms)))
        {
            pms.clear();
            const VkResult modeResult = vkenum::collect(pms,
                [&](uint32_t* count, VkPresentModeKHR* modes)
                { return vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice(), mSurface, count, modes); },
                "vkGetPhysicalDeviceSurfacePresentModesKHR", 64);
            if (!checkResult(modeResult) || pms.empty()) return false;
        }
        bool mailbox = false, immediate = false;
        for (VkPresentModeKHR m : pms)
        {
            mailbox   |= (m == VK_PRESENT_MODE_MAILBOX_KHR);
            immediate |= (m == VK_PRESENT_MODE_IMMEDIATE_KHR);
        }
        if (!mUIOnly)
        {
            mFgQueueHeld = llfg::sharedQueueHeld(fgQueue, llfg::frameGenPresentMode(immediate, mailbox));
            fg = mFgQueueHeld ? 0u : fg;
            mFgMax = fg;
            mFgPresentMode = fg != 0;
        }
        if (fg)
        {
            const llfg::PresentMode m = llfg::frameGenPresentMode(immediate, mailbox);
            mPresentMode = m == llfg::PresentMode::Immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
                         : m == llfg::PresentMode::Mailbox   ? VK_PRESENT_MODE_MAILBOX_KHR
                                                             : VK_PRESENT_MODE_FIFO_KHR;
        }
        else if (!mUIOnly && sVSync)
            mPresentMode = VK_PRESENT_MODE_FIFO_KHR;   // (#1544: the modes were asked for the shared queue's rule only)
        else if (sNoVsyncMode == 1)
        {
            if (immediate)    mPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (mailbox) mPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        else if (mailbox)   mPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (immediate) mPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    // Extent.
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        mExtent = caps.currentExtent;
    } else {
        mExtent.width  = std::clamp(width,  caps.minImageExtent.width,
                                    caps.maxImageExtent.width);
        mExtent.height = std::clamp(height, caps.minImageExtent.height,
                                    caps.maxImageExtent.height);
    }
    if (mExtent.width == 0 || mExtent.height == 0) {
        // Minimized; caller should skip. Report benign.
        return false;
    }

    uint32_t imageCount = caps.minImageCount + 1;
    // #1517: a batch's images, one more on MAILBOX / FIFO (llfg::swapchainImagesFor: so MAILBOX can replace a
    // frame not yet shown), never below the surface's minimum; clamped below.
    if (fg) imageCount = std::max(caps.minImageCount, llfg::swapchainImagesFor(fg, fgPresentMode()));
    if (mUIOnly && (mExtent.width > 8192 || mExtent.height > 8192 || imageCount > 4 ||
        uint64_t(mExtent.width) * mExtent.height * 4 * imageCount > 128ull * 1024 * 1024)) return false;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.pNext = mCreateChain;   // #919: full-screen exclusive info (Windows), else nullptr
    // #1517 Reflex: low latency mode in front of it (setLatencyMode; Reflex's sleep never ends without it),
    // only on a chain built for frame generation -- any other chain is created exactly as before.
    const bool latencyOn = mLatencyWanted && fg;
    VkSwapchainLatencyCreateInfoNV latency{VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV};
    latency.latencyModeEnable = VK_TRUE;
    latency.pNext = mCreateChain;
    if (latencyOn) ci.pNext = &latency;
    mLatencyBuilt = false;
    ci.surface = mSurface;
    ci.minImageCount = imageCount;
    ci.imageFormat = mFormat.format;
    ci.imageColorSpace = mFormat.colorSpace;
    ci.imageExtent = mExtent;
    ci.imageArrayLayers = 1;
    // COLOR_ATTACHMENT for rendering; TRANSFER_DST so a compute/blit path can
    // also copy into the swapchain image (the clean pipeline blits its resolved
    // color target). Both are near-universally supported.
    // TRANSFER_SRC: the world renderer copies the lit scene out for next frame's
    // screen-space reflections.
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (mUIOnly) ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // single queue family
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (mUIOnly)
    {
        for (auto mode : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & mode) { ci.compositeAlpha = mode; break; }
    }
    ci.presentMode = mPresentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    VkResult cr = vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &mSwapchain);
    mChainRejected = false;
    if (cr != VK_SUCCESS && ci.pNext == &latency && cr != VK_ERROR_DEVICE_LOST) {
        // #1517: refused in low latency mode -- once more without it (no Reflex on this chain; the owner
        // sees latencyMode() false). The exclusive chain below then meets exactly its old inputs.
        std::fprintf(stderr, "[vkgfx] vkCreateSwapchainKHR in low latency mode = %s; retrying without it\n",
                     vkResultString(cr));
        mSwapchain = VK_NULL_HANDLE;
        ci.pNext = mCreateChain;
        cr = vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &mSwapchain);
    }
    if (cr != VK_SUCCESS && mCreateChain && cr != VK_ERROR_DEVICE_LOST) {
        // #919: a driver refusing the full-screen exclusive swapchain must not take the
        // viewer down: fall back to a plain (composited) swapchain and report it to the owner.
        std::fprintf(stderr, "[vkgfx] vkCreateSwapchainKHR with the full-screen exclusive chain = %s; "
                     "retrying without it (FIFO)\n", vkResultString(cr));
        mChainRejected = true;
        mSwapchain = VK_NULL_HANDLE;
        ci.pNext = nullptr;   // (#1517: low latency mode, if it was asked for, was already refused above)
        mPresentMode = VK_PRESENT_MODE_FIFO_KHR;   // (the mode came from the exclusive query)
        ci.presentMode = mPresentMode;
        cr = vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &mSwapchain);
    }
    if (cr != VK_SUCCESS && cr != VK_ERROR_DEVICE_LOST && mDisplayStatus.encoding != vkdisplay::Encoding::Sdr)
    {
        // Optional output failure is not fatal. Rebuild a real SDR target, never
        // relabel the failed HDR format and feed it SDR pixels.
        mFormat = sdrFormat;
        mDisplayStatus.actual = sdrFormat;
        mDisplayStatus.encoding = vkdisplay::Encoding::Sdr;
        mDisplayStatus.reason = vkdisplay::Reason::ResourcesUnavailable;
        ci.imageFormat = sdrFormat.format;
        ci.imageColorSpace = sdrFormat.colorSpace;
        mSwapchain = VK_NULL_HANDLE;
        std::fprintf(stderr, "[vkgfx] HDR display swapchain refused; retrying SDR\n");
        cr = vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &mSwapchain);
    }
    if (!checkResult(cr)) return false;
    mLatencyBuilt = ci.pNext == &latency;   // (the create that succeeded carried it)
    if (mDisplayStatus.encoding == vkdisplay::Encoding::Hdr10 &&
        ctx.optionalExtensionEnabled(VK_EXT_HDR_METADATA_EXTENSION_NAME))
    {
        const auto setMetadata = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
            vkGetDeviceProcAddr(ctx.device(), "vkSetHdrMetadataEXT"));
        VkHdrMetadataEXT metadata{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
        metadata.displayPrimaryRed = {.708f, .292f};
        metadata.displayPrimaryGreen = {.170f, .797f};
        metadata.displayPrimaryBlue = {.131f, .046f};
        metadata.whitePoint = {.3127f, .3290f};
        metadata.maxLuminance = mDisplayStatus.peakNits;
        metadata.minLuminance = 0.f;
        metadata.maxContentLightLevel = mDisplayStatus.peakNits;
        metadata.maxFrameAverageLightLevel = mDisplayStatus.peakNits; // conservative bound, not a measured frame statistic
        if (setMetadata) setMetadata(ctx.device(), 1, &mSwapchain, &metadata);
    }

    const VkResult imageResult = vkenum::collect(mImages,
        [&](uint32_t* count, VkImage* images)
        { return vkGetSwapchainImagesKHR(ctx.device(), mSwapchain, count, images); },
        "vkGetSwapchainImagesKHR", mUIOnly ? 4u : 256u);
    if (!checkResult(imageResult) || mImages.empty()) return false;
    const uint32_t n = uint32_t(mImages.size()); // exact successful fill count, including shrink

    {   // #407: what these images take in device memory. The same create info the WSI
        // uses for them (no VkImageSwapchainCreateInfoKHR: not allowed in this query).
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = ci.imageFormat;
        ici.extent = {mExtent.width, mExtent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = ci.imageArrayLayers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = ci.imageUsage;
        ici.sharingMode = ci.imageSharingMode;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkDeviceImageMemoryRequirements dr{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
        dr.pCreateInfo = &ici;
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        vkGetDeviceImageMemoryRequirements(ctx.device(), &dr, &req);
        mMemBytes = (uint64_t)req.memoryRequirements.size * n;
    }

    mImageViews.resize(n);
    mRenderFinished.resize(n);
    if (!mUIOnly) mMaintenance = ctx.swapchainMaintenance();
    if (mMaintenance)
    {
        try
        {
            // (#1517: a frame presents up to fg + 1 images, each with its own proof)
            mPresentArmed.resize(n + mFramesInFlight * (fg + 1), false);
            mPresentFences.resize(n + mFramesInFlight * (fg + 1));
        }
        catch (const std::bad_alloc&)
        {
            if (mUIOnly) return false;
            mPresentArmed.clear();
            mPresentFences.clear();
            mMaintenance = false; // optional main proof allocation never fails the viewer
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = mImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = mFormat.format;
        vci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!checkResult(vkCreateImageView(ctx.device(), &vci, nullptr, &mImageViews[i])))
            return false;
    }

    // Per-IMAGE render-finished semaphores (one per swapchain image). Must be
    // (re)created here because the image count is only known now and can change
    // across recreate(); present() waits mRenderFinished[imageIndex].
    VkSemaphoreCreateInfo rsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; ++i) {
        if (!checkResult(vkCreateSemaphore(ctx.device(), &rsci, nullptr, &mRenderFinished[i])))
            return false;
    }
    if (fg) createFrameGenObjects(ctx, n);
    if (mMaintenance)
    {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (auto& fence : mPresentFences)
        {
            const VkResult result = vkCreateFence(ctx.device(), &fci, nullptr, &fence);
            if (result == VK_SUCCESS) continue;
            if (result == VK_ERROR_DEVICE_LOST)
            {
                vkDeviceLostRaise("present proof allocation", result, "graphics", __FILE__, __LINE__);
                return false;
            }
            if (mUIOnly) return false;
            // Main already has a usable chain: keep it with the conventional
            // existing idle-teardown policy, then retry proofs at next recreate.
            for (VkFence allocated : mPresentFences) if (allocated) vkDestroyFence(ctx.device(), allocated, nullptr);
            mPresentFences.clear();
            mPresentArmed.clear();
            mMaintenance = false;
            std::fprintf(stderr, "[vkgfx] optional main present-proof allocation refused: %s\n", vkResultString(result));
            break;
        }
    }
    if (mFgMax)
    {
        mFgImages = n;
        mNextPresentFence = mPresentFences.size();   // (no proof reserved by acquire(): the present thread's own)
        mExtraCount = 0;
    }
    return true;
}

void VkSwapchain::setFrameGen(uint32_t maxGenerated)
{
    mFgWanted = std::min(maxGenerated, kMaxBatchPresents - 1);
}

void VkSwapchain::createFrameGenObjects(VkDeviceContext& ctx, uint32_t images)
{
    // Optional, like the present proofs: a refusal builds the chain without frame generation
    // (frameGenImages() 0), never fails the viewer.
    try
    {
        mGateRelease.assign(images, VK_NULL_HANDLE);
    }
    catch (const std::bad_alloc&)
    {
        destroyFrameGenObjects(ctx);
        return;
    }
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkSemaphore& sem : mGateRelease)
        {
            const VkResult result = vkCreateSemaphore(ctx.device(), &sci, nullptr, &sem);
            if (result == VK_SUCCESS) continue;
            sem = VK_NULL_HANDLE;
            if (result == VK_ERROR_DEVICE_LOST)
                vkDeviceLostRaise("frame generation semaphores", result, "graphics", __FILE__, __LINE__);
            else
                std::fprintf(stderr, "[vkgfx] frame generation semaphores refused: %s; "
                             "the swapchain is built without frame generation\n", vkResultString(result));
            destroyFrameGenObjects(ctx);
            return;
        }
}

void VkSwapchain::destroyFrameGenObjects(VkDeviceContext& ctx)
{
    for (VkSemaphore s : mGateRelease) if (s) vkDestroySemaphore(ctx.device(), s, nullptr);
    mGateRelease.clear();
    mFgMax = 0;
    mFgImages = 0;
    mExtraCount = 0;
}

bool VkSwapchain::previewDisplayStatus(VkDeviceContext& ctx, vkdisplay::Status& out) const
{
    if (!mSurface) return false;
    std::vector<VkSurfaceFormatKHR> formats;
    if (vkenum::collect(formats,
        [&](uint32_t* n, VkSurfaceFormatKHR* p)
        { return vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), mSurface, n, p); },
        "vkGetPhysicalDeviceSurfaceFormatsKHR(preview)", 256) != VK_SUCCESS || formats.empty()) return false;
    const uint32_t count = uint32_t(formats.size());
    if (count == 1 && formats[0].format == VK_FORMAT_UNDEFINED &&
        formats[0].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        formats[0].format = VK_FORMAT_B8G8R8A8_UNORM;

    VkSurfaceFormatKHR sdr{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sdr = f; break; }
    if (sdr.format == VK_FORMAT_UNDEFINED)
        for (const auto& f : formats)
            if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_SRGB ||
                 f.format == VK_FORMAT_B8G8R8A8_SRGB) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sdr = f; break; }
    if (sdr.format == VK_FORMAT_UNDEFINED) return false;
    out = vkdisplay::choose(mDisplayRequest, mDisplayCapability, formats, sdr,
                            mLinearOutputReady);
    return true;
}

bool VkSwapchain::previewExtent(VkDeviceContext& ctx, uint32_t width, uint32_t height,
                                VkExtent2D& out) const
{
    if (!mSurface) return false;
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice(),mSurface,&caps)!=VK_SUCCESS)
        return false;
    if (caps.currentExtent.width!=0xffffffffu)
        out=caps.currentExtent;
    else
    {
        out.width=std::clamp(width,caps.minImageExtent.width,caps.maxImageExtent.width);
        out.height=std::clamp(height,caps.minImageExtent.height,caps.maxImageExtent.height);
    }
    return out.width!=0 && out.height!=0;
}

void VkSwapchain::destroySwapchainObjects(VkDeviceContext& ctx) {
    destroyFrameGenObjects(ctx);
    for (VkFence fence : mPresentFences) if (fence) vkDestroyFence(ctx.device(), fence, nullptr);
    mPresentFences.clear();
    mPresentArmed.clear();
    for (VkSemaphore s : mRenderFinished) {
        if (s) vkDestroySemaphore(ctx.device(), s, nullptr);
    }
    mRenderFinished.clear();
    for (VkImageView v : mImageViews) {
        if (v) vkDestroyImageView(ctx.device(), v, nullptr);
    }
    mImageViews.clear();
    mImages.clear();
    mMemBytes = 0;
    if (mSwapchain) {
        vkDestroySwapchainKHR(ctx.device(), mSwapchain, nullptr);
        mSwapchain = VK_NULL_HANDLE;
    }
    mFgPresentMode = false;
    mLatencyBuilt = false;
}

void VkSwapchain::gatesPendingError(const char* where) const
{
    std::fprintf(stderr, "[vkgfx] ERROR: VkSwapchain::%s with gated presents pending -- the owner must open "
                 "every gate (drain the frame pacer) first, or the device wait hangs on the present queue\n", where);
}

bool VkSwapchain::waitPresentProofs(VkDevice device, uint64_t totalNs) const
{
    // The presentation engine signals these, not the GPU's queues: a wait for one is never infinite (that is
    // the hang watchdog's wait, and a present the compositor holds is no GPU hang).
    const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(totalNs);
    for (size_t i = 0; i < mPresentFences.size(); ++i)
    {
        if (!mPresentArmed[i]) continue;
        const auto now = std::chrono::steady_clock::now();
        const uint64_t left = now < until ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(until - now).count()) : 0;
        // (no bound at all: a look -- the retry of a rebuild already deferred never waits again)
        if (!totalNs)
        {
            if (vkGetFenceStatus(device, mPresentFences[i]) != VK_SUCCESS) return false;
            continue;
        }
        // (VK_TIMEOUT: withheld; a lost device: raised by the wait)
        if (vkWaitForFences(device, 1, &mPresentFences[i], VK_TRUE, left) != VK_SUCCESS) return false;
    }
    return true;
}

bool VkSwapchain::recreate(VkDeviceContext& ctx, uint32_t width, uint32_t height) {
    mRebuildDeferred = false;
    if (vkDeviceIsLost()) return false;   // #135
    if (gatedPresentsPending()) gatesPendingError("recreate");
    mLastDeviceIdleWaitMs = 0.0;
    if (mUIOnly)
    {
        if (!retired(ctx.device())) return false;
    }
    else
    {
        const auto idleStart = std::chrono::steady_clock::now();
        const VkResult idle = vkDeviceWaitIdle(ctx.device());
        mLastDeviceIdleWaitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - idleStart).count();
        if (vkDeviceIsLost()) return false;   // #135 (raised by the wait)
        // #1517: only a wait that succeeded says nothing is pending (deviceIdled(): frame generation's owed
        // slot release is host-signalled only then).
        mDeviceIdled = idle == VK_SUCCESS;
        if (mDeviceIdled)
            mGatesPending = false;   // (every gate submit has completed)
        else
            std::fprintf(stderr, "[vkgfx] WARN: VkSwapchain::recreate: the device wait failed (%s); the chain is "
                         "rebuilt with work possibly still pending\n", vkResultString(idle));
        // Device-wide maintenance1: main WSI teardown also needs present proof. Withheld (a hidden window's
        // presents the compositor still holds): the chain stays as it is, the owner asks again.
        // One bounded wait per row of deferrals: a try after the first only looks (the owner asks about twice
        // a second for as long as the window stays away -- never half a second of the main thread each time).
        if (!waitPresentProofs(ctx.device(), mProofDeferrals ? 0 : kRebuildProofWaitNs))
        {
            if (vkDeviceIsLost()) return false;   // #135 (raised by the wait)
            const auto now = std::chrono::steady_clock::now();
            if (!mProofDeferrals) mProofDeferredSince = now;
            ++mProofDeferrals;
            // (to log: the first of the row, then one a minute)
            const int64_t minute = std::chrono::duration_cast<std::chrono::minutes>(now - mProofDeferredSince).count();
            mProofNote = minute != mProofNotedMinute;
            mProofNotedMinute = minute;
            mRebuildDeferred = true;
            return false;
        }
        mProofDeferrals = 0;
        mProofNotedMinute = -1;
        mProofNote = false;
    }
    destroySwapchainObjects(ctx);
    return createSwapchainObjects(ctx, width, height);
}

bool VkSwapchain::destroy(VkDeviceContext& ctx) {
    mDestroyWithheld = false;
    if (vkDeviceIsLost()) return false;   // #135: leak to process exit, never touch the lost device
    if (gatedPresentsPending()) gatesPendingError("destroy");
    if (mUIOnly)
    {
        if (!retired(ctx.device())) return false; // owner retains this entry and polls
    }
    else if (ctx.device())
    {
        vkDeviceWaitIdle(ctx.device());
        if (!waitPresentProofs(ctx.device(), kDestroyProofWaitNs))
        {
            if (vkDeviceIsLost()) return false;   // #135 (raised by the wait: no present is "held")
            // (the presentation engine still holds a present: nothing of the chain may go -- kept to process exit)
            mDestroyWithheld = true;
            std::fprintf(stderr, "[vkgfx] WARN: VkSwapchain::destroy: a present proof is still withheld; the "
                         "swapchain is kept until process exit\n");
            return false;
        }
    }
    for (VkFence fence : mAcquireFences) if (fence) vkDestroyFence(ctx.device(), fence, nullptr);
    mAcquireFences.clear();
    mAcquireArmed.clear();
    mSubmissionArmed.clear();
    // Per-frame-in-flight objects (image-available semaphores + fences).
    for (VkSemaphore s : mImageAvailable) if (s) vkDestroySemaphore(ctx.device(), s, nullptr);
    for (VkFence f : mInFlight) if (f) vkDestroyFence(ctx.device(), f, nullptr);
    mImageAvailable.clear();
    mInFlight.clear();
    // Per-image render-finished semaphores are destroyed by destroySwapchainObjects.
    destroySwapchainObjects(ctx);
    // Surface is owned by the window layer; not destroyed here.
    mSurface = VK_NULL_HANDLE;
    mPresentPending = false;
    mGatesPending = false;
    mProofDeferrals = 0;
    mProofNotedMinute = -1;
    mProofNote = false;
    return true;
}

bool VkSwapchain::retired(VkDevice device) const
{
    if (vkDeviceIsLost() || mUnresolvedPresent || needsPresentDrain()) return false;
    for (size_t i = 0; i < mInFlight.size(); ++i)
        if ((!mUIOnly || mSubmissionArmed[i]) && vkGetFenceStatus(device, mInFlight[i]) != VK_SUCCESS) return false;
    for (size_t i = 0; i < mAcquireFences.size(); ++i)
        if (mAcquireArmed[i] && vkGetFenceStatus(device, mAcquireFences[i]) != VK_SUCCESS) return false;
    for (size_t i = 0; i < mPresentFences.size(); ++i)
        if (mPresentArmed[i] && vkGetFenceStatus(device, mPresentFences[i]) != VK_SUCCESS) return false;
    return true;
}

bool VkSwapchain::slotDone(VkDevice dev, uint32_t slot) const
{
    return dev && slot < mInFlight.size() && mInFlight[slot] && vkGetFenceStatus(dev, mInFlight[slot]) == VK_SUCCESS;
}

void VkSwapchain::waitFrameSlot(VkDeviceContext& ctx) {
    if (vkDeviceIsLost() || !ctx.device()) return;   // #135
    if (mFrame >= mInFlight.size() || !mInFlight[mFrame]) return;   // (not created / destroyed)
    // The same hooked wait as acquire() (vk_common.h: device loss raises with this site,
    // an infinite wait is sliced into the hang watchdog). The fence is NOT reset here:
    // acquire() resets it only once a submit is certain.
    VK_CHECK(vkWaitForFences(ctx.device(), 1, &mInFlight[mFrame], VK_TRUE, UINT64_MAX));
}

bool VkSwapchain::waitFrameSlots(VkDeviceContext& ctx)
{
    if (vkDeviceIsLost() || !ctx.device()) return false;   // #135
    if (mUIOnly) return true;
    // (as waitFrameSlot: the hooked wait, never a reset)
    for (const VkFence& fence : mInFlight)
    {
        if (!fence) continue;
        VK_CHECK(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX));
        if (vkDeviceIsLost()) return false;
    }
    return true;
}

bool VkSwapchain::acquire(VkDeviceContext& ctx, uint32_t* outImageIndex,
                          bool* outNeedsRecreate) {
    if (outNeedsRecreate) *outNeedsRecreate = false;
    if (vkDeviceIsLost()) return false;   // #135: no acquire on a lost device
    if (!mSwapchain) { if (outNeedsRecreate) *outNeedsRecreate = true; return false; }
    if (mFgImages)
    {
        // #1517: the present thread's images (acquireOne); the frame takes its slot with beginFrameWithoutImage.
        std::fprintf(stderr, "[vkgfx] ERROR: acquire() on a chain built for frame generation; refused\n");
        return false;
    }

    // Hooked (vk_common.h): DEVICE_LOST raises with this site (the #135 crash came
    // through here), an infinite wait is sliced into the hang watchdog. With frame pacing
    // (waitFrameSlot at the top of the main-loop frame) this fence is already signaled and
    // the wait returns at once; kept as the correctness backstop for slot reuse.
    if (mUIOnly)
    {
        if (!slotDone(ctx.device(), mFrame)) return false;
        if (mAcquireArmed[mFrame] && vkGetFenceStatus(ctx.device(), mAcquireFences[mFrame]) != VK_SUCCESS) return false;
        mAcquireArmed[mFrame] = false;
        if (!checkResult(vkResetFences(ctx.device(), 1, &mAcquireFences[mFrame]))) return false;
    }
    else VK_CHECK(vkWaitForFences(ctx.device(), 1, &mInFlight[mFrame], VK_TRUE, UINT64_MAX));
    if (vkDeviceIsLost()) return false;
    if (mMaintenance)
    {
        mNextPresentFence = 0;
        while (mNextPresentFence < mPresentFences.size() && mPresentArmed[mNextPresentFence] &&
               vkGetFenceStatus(ctx.device(), mPresentFences[mNextPresentFence]) != VK_SUCCESS) ++mNextPresentFence;
        if (mNextPresentFence == mPresentFences.size()) return false;
    }
    vkAddrFrameTick();

    VkResult r;
    {
        const VkDriverCallScope inDriver("vkAcquireNextImageKHR");   // #1546
        r = vkAcquireNextImageKHR(ctx.device(), mSwapchain, mAcquireTimeout,
                                  mImageAvailable[mFrame], mUIOnly ? mAcquireFences[mFrame] : VK_NULL_HANDLE,
                                  outImageIndex);
    }
    if (r == VK_TIMEOUT || r == VK_NOT_READY) {
        // #1074 (VR only: the timeout is UINT64_MAX otherwise): no image in time, the semaphore
        // was not signaled, the fence is not reset -- skip the window's frame.
        return false;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        // No image delivered and the semaphore was NOT signaled: skip the frame
        // and recreate. Do not reset the fence (no submit this frame).
        // #919: exclusive mode lost (alt-tab, another app took the display): the owner
        // re-acquires it or rebuilds without exclusive mode -- never a device loss. It does
        // not ask for a recreate: re-acquiring the mode is enough, and the owner falls back
        // to a rebuild without exclusive mode when that keeps failing.
        if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) mExclusiveLost = true;
        else if (outNeedsRecreate) *outNeedsRecreate = true;
        return false;
    }
    if (mUIOnly && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) mAcquireArmed[mFrame] = true;
    if (r == VK_SUBOPTIMAL_KHR) {
        // SUBOPTIMAL DID deliver an image and DID signal the semaphore; we must
        // render + present this frame (else the acquire semaphore stays signaled
        // with no matching wait). Flag recreate so the caller rebuilds AFTER
        // present. Fall through to the success path.
        if (outNeedsRecreate) *outNeedsRecreate = true;
    }
    else if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vkgfx] vkAcquireNextImageKHR = %s\n",
                     vkResultString(r));
        // A lost device, or a lost surface (the window's surface is gone: nothing can be
        // presented again without re-creating the device's WSI objects): the graceful
        // device-lost exit (#135) instead of skipping every frame from now on.
        if (mUIOnly) mUIFailed = true;
        if (r == VK_ERROR_DEVICE_LOST || (r == VK_ERROR_SURFACE_LOST_KHR && !mUIOnly))
            vkDeviceLostRaise("vkAcquireNextImageKHR", r, "graphics", __FILE__, __LINE__);
        return false;
    }
    // Only reset the fence once we know we'll submit work this frame.
    if (mUIOnly) mSubmissionArmed[mFrame] = false;
    if (mUIOnly)
    {
        if (!checkResult(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]))) { mUIFailed = true; return false; }
    }
    else VK_CHECK(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]));
    return true;
}

bool VkSwapchain::beginFrameWithoutImage(VkDeviceContext& ctx) {
    if (vkDeviceIsLost() || !ctx.device()) return false;   // #135
    if (mFrame >= mInFlight.size() || !mInFlight[mFrame]) return false;
    if (mUIOnly)
    {
        if (!slotDone(ctx.device(), mFrame)) return false;
        if (mAcquireArmed[mFrame] && vkGetFenceStatus(ctx.device(), mAcquireFences[mFrame]) != VK_SUCCESS) return false;
        mAcquireArmed[mFrame] = false;
        if (!checkResult(vkResetFences(ctx.device(), 1, &mAcquireFences[mFrame]))) return false;
    }
    else VK_CHECK(vkWaitForFences(ctx.device(), 1, &mInFlight[mFrame], VK_TRUE, UINT64_MAX));
    if (vkDeviceIsLost()) return false;
    // (the fault report's frame clock, as acquire() ticks it: without this every frame under frame generation was
    //  "frame N" and every image looked created and freed in the frame of the loss -- 2026-11-21 DEVICE_LOST report)
    vkAddrFrameTick();
    if (mUIOnly) mSubmissionArmed[mFrame] = false;
    if (mUIOnly)
    {
        if (!checkResult(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]))) { mUIFailed = true; return false; }
    }
    else VK_CHECK(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]));
    return true;
}

bool VkSwapchain::present(VkDeviceContext& ctx, uint32_t imageIndex,
                          bool* outNeedsRecreate) {
    if (outNeedsRecreate) *outNeedsRecreate = false;
    if (mFgImages)
    {
        // #1517: the present thread presents a frame generation chain's images (presentBatch).
        std::fprintf(stderr, "[vkgfx] ERROR: present() on a chain built for frame generation; refused\n");
        return false;
    }
    VkSwapchainPresentFenceInfoEXT presentFence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    if (mMaintenance)
    {
        // Reserved before acquire. An acquire may return before the prior PE
        // fence signals, so proof slots must be independent of image indices.
        if (mNextPresentFence >= mPresentFences.size()) { mUIFailed = true; return false; }
        mPresentArmed[mNextPresentFence] = false;
        const VkResult reset = vkResetFences(ctx.device(), 1, &mPresentFences[mNextPresentFence]);
        if (reset != VK_SUCCESS)
        {
            mUIFailed = true;
            if (outNeedsRecreate) *outNeedsRecreate = true;
            if (reset == VK_ERROR_DEVICE_LOST)
                vkDeviceLostRaise("present proof reset", reset, "graphics", __FILE__, __LINE__);
            return false;
        }
        presentFence.swapchainCount = 1;
        presentFence.pFences = &mPresentFences[mNextPresentFence];
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.pNext = mMaintenance ? &presentFence : nullptr;
    pi.waitSemaphoreCount = 1;
    // Wait the render-finished semaphore for THIS image (signaled by its submit).
    pi.pWaitSemaphores = &mRenderFinished[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &mSwapchain;
    pi.pImageIndices = &imageIndex;

    if (vkDeviceIsLost()) return false;   // #135: nothing reaches a lost device
    VkResult r = VK_SUCCESS;
    if (vkSimulateDeviceLostTake(VK_SIM_LOST_PRESENT))
        r = VK_ERROR_DEVICE_LOST;          // RenderVKDebugSimulateDeviceLost 3: not presented
    else
    {   // #1544: the driver call alone inside the queue's lock (a queue the present thread may share)
        const VkDeviceContext::QueueLock lock(ctx.queue());
        if (lock.gaveUp()) return false;   // (the device was lost while this waited for the queue: as above)
        const VkDriverCallScope inDriver("vkQueuePresentKHR");   // #1546
        r = vkQueuePresentKHR(ctx.queue(), &pi);
    }
    if (r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY) mPresentPending = true;
    if (mMaintenance && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR ||
        r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR ||
        r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)) mPresentArmed[mNextPresentFence] = true;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR &&
        r != VK_ERROR_SURFACE_LOST_KHR && r != VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT &&
        r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_DEVICE_LOST)
        mUnresolvedPresent = true; // unknown enqueue outcome is never a completion proof
    mFrame = (mFrame + 1) % mFramesInFlight;

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        // (#919: like OUT_OF_DATE, a MODE_LOST present still counts as enqueued -- its
        // semaphore wait executes -- so nothing is left signaled; no recreate, see acquire.)
        if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) mExclusiveLost = true;
        else if (outNeedsRecreate) *outNeedsRecreate = true;
        return false;
    }
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vkgfx] vkQueuePresentKHR = %s\n", vkResultString(r));
        if (mUIOnly) mUIFailed = true;
        if (r == VK_ERROR_DEVICE_LOST || (r == VK_ERROR_SURFACE_LOST_KHR && !mUIOnly))
            vkDeviceLostRaise(vkDeviceLostSimulated() ? "vkQueuePresentKHR (simulated)" : "vkQueuePresentKHR",
                              r, "graphics", __FILE__, __LINE__);
        return false;
    }
    return true;
}

// ---- #1517 frame generation: the present thread's acquires and presents ----
size_t VkSwapchain::freePresentFence(VkDevice device) const
{
    for (size_t i = 0; i < mPresentFences.size(); ++i)
    {
        bool reserved = (i == mNextPresentFence);
        for (uint32_t k = 0; k < mExtraCount && !reserved; ++k) reserved = (mExtra[k].fence == i);
        if (reserved) continue;
        if (!mPresentArmed[i] || vkGetFenceStatus(device, mPresentFences[i]) == VK_SUCCESS) return i;
    }
    return mPresentFences.size();
}

bool VkSwapchain::acquireOne(VkDeviceContext& ctx, VkSemaphore sem, uint64_t timeoutNs, uint32_t* outIndex,
                             bool* outNeedsRecreate)
{
    if (outNeedsRecreate) *outNeedsRecreate = false;
    if (!mFgImages || !mSwapchain || !sem || !outIndex || vkDeviceIsLost()) return false;
    if (mExtraCount >= kMaxBatchPresents)
    {
        std::fprintf(stderr, "[vkgfx] ERROR: acquireOne: %u images acquired and not presented; refused\n", mExtraCount);
        return false;
    }
    // The present proof first: presentBatch never meets an acquired image it cannot prove.
    size_t fence = ~size_t(0);
    if (mMaintenance)
    {
        fence = freePresentFence(ctx.device());
        if (fence == mPresentFences.size()) return false;
    }
    uint32_t index = 0;
    VkResult r;
    {
        const VkDriverCallScope inDriver("vkAcquireNextImageKHR (frame generation)");   // #1546
        r = vkAcquireNextImageKHR(ctx.device(), mSwapchain, timeoutNs, sem, VK_NULL_HANDLE, &index);
    }
    if (r == VK_SUBOPTIMAL_KHR && outNeedsRecreate) *outNeedsRecreate = true;   // (an image all the same)
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        // No image and the semaphore NOT signalled.
        if (r == VK_ERROR_OUT_OF_DATE_KHR && outNeedsRecreate) *outNeedsRecreate = true;
        else if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) mExclusiveLost = true;
        else if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR)
            vkDeviceLostRaise("vkAcquireNextImageKHR (frame generation)", r, "present", __FILE__, __LINE__);
        else if (r != VK_TIMEOUT && r != VK_NOT_READY)
            std::fprintf(stderr, "[vkgfx] vkAcquireNextImageKHR (frame generation) = %s\n", vkResultString(r));
        return false;
    }
    mExtra[mExtraCount++] = {index, fence};
    *outIndex = index;
    return true;
}

bool VkSwapchain::presentBatch(VkDeviceContext& ctx, const BatchPresent* presents, uint32_t count, VkSemaphore gate,
                               uint32_t meteringFrames, uint32_t* outFeedback, bool* outNeedsRecreate)
{
    if (outNeedsRecreate) *outNeedsRecreate = false;
    if (outFeedback) *outFeedback = ~0u;
    // Not a batch this chain takes (not built for frame generation, or a caller error): no effect.
    if (!mFgImages || !mSwapchain || !presents || count == 0 || count > kMaxBatchPresents)
    {
        for (uint32_t k = 0; k < mExtraCount; ++k)
            std::fprintf(stderr, "[vkgfx] ERROR: presentBatch refused (%u presents); frame generation image %u "
                         "stays acquired, not presented\n", count, mExtra[k].image);
        return false;
    }
    if (vkDeviceIsLost()) return false;   // #135: nothing reaches a lost device
    const VkDevice dev = ctx.device();
    bool ok = true;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t image = presents[i].image;
        if (image >= mImages.size()) { ok = false; continue; }   // (a caller error: no such image)
        bool repeated = false;
        for (uint32_t j = 0; j < i && !repeated; ++j) repeated = (presents[j].image == image);
        if (repeated)
        {
            // (a caller error: an image is presented once per acquire)
            std::fprintf(stderr, "[vkgfx] ERROR: presentBatch: image %u is in the batch twice; skipped\n", image);
            ok = false;
            continue;
        }
        // Proof slot: reserved by acquireOne. The image leaves the acquired list.
        size_t fence = mPresentFences.size();
        bool acquired = false;
        for (uint32_t k = 0; k < mExtraCount; ++k)
            if (mExtra[k].image == image)
            {
                if (mExtra[k].fence < mPresentFences.size()) fence = mExtra[k].fence;
                mExtra[k] = mExtra[--mExtraCount];
                acquired = true;
                break;
            }
        if (!acquired)
            std::fprintf(stderr, "[vkgfx] ERROR: presentBatch: image %u did not come from acquireOne\n", image);
        if (mMaintenance && fence >= mPresentFences.size()) fence = freePresentFence(dev);

        // The gate: an empty submit on the present queue holds this present until the pacer
        // signals `gate` at gateValue; it consumes renderFinished and signals the image's release.
        // #1544 (D8): on a queue shared with the renderer no submit waits for the pacer -- a queue runs its
        // submits in order, and the renderer's would sit behind the gate until it opens. This thread waits the
        // gate itself and then presents (llfg::gateOnCpu), for at most llfg::kGateWaitBoundMs: a gate that does
        // not open in time is presented anyway (the pacer signals every value in order, waited for or not).
        VkSemaphore wait = mRenderFinished[image];
        const bool gated = presents[i].gateValue != 0 && gate != VK_NULL_HANDLE;
        if (gated && llfg::gateOnCpu(llfg::presentQueueOf(ctx.presentQueue() != VK_NULL_HANDLE, ctx.presentQueueShared())))
        {
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &gate;
            wi.pValues = &presents[i].gateValue;
            // (VK_TIMEOUT: presented now; a lost device: raised by the wait)
            (void)vkWaitSemaphores(dev, &wi, uint64_t(llfg::kGateWaitBoundMs) * 1000000ull);
            if (vkDeviceIsLost()) break;
        }
        else if (gated)
        {
            VkSemaphoreSubmitInfo waits[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},
                                              {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
            waits[0].semaphore = mRenderFinished[image];
            waits[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            waits[1].semaphore = gate;
            waits[1].value = presents[i].gateValue;
            waits[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSemaphoreSubmitInfo release{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            release.semaphore = mGateRelease[image];
            release.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            si.waitSemaphoreInfoCount = 2;
            si.pWaitSemaphoreInfos = waits;
            si.signalSemaphoreInfoCount = 1;
            si.pSignalSemaphoreInfos = &release;
            const VkResult sr = vkQueueSubmit2(ctx.presentQueue(), 1, &si, VK_NULL_HANDLE);
            if (sr == VK_SUCCESS)
            {
                wait = mGateRelease[image];
                mGatesPending = true;
            }
            else
            {
                // A refused submit changed no semaphore: present ungated (this frame shows early).
                // (The submit wrapper raises a device loss itself.)
                ok = false;
                if (vkDeviceIsLost()) break;
                std::fprintf(stderr, "[vkgfx] frame generation gate submit = %s; presenting ungated\n",
                             vkResultString(sr));
            }
        }

        VkSwapchainPresentFenceInfoEXT proof{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        bool proved = false;
        if (mMaintenance)
        {
            if (fence < mPresentFences.size())
            {
                mPresentArmed[fence] = false;
                const VkResult reset = vkResetFences(dev, 1, &mPresentFences[fence]);
                if (reset == VK_ERROR_DEVICE_LOST)
                {
                    vkDeviceLostRaise("present proof reset", reset, "graphics", __FILE__, __LINE__);
                    break;
                }
                if (reset == VK_SUCCESS)
                {
                    proof.swapchainCount = 1;
                    proof.pFences = &mPresentFences[fence];
                    proved = true;
                }
            }
            // No proof (none free, or its reset refused): presented anyway -- the image was
            // acquired -- but nothing can prove this chain retired any more.
            if (!proved) { mUnresolvedPresent = true; ok = false; }
        }
        // Driver pacing (VK_NV_present_metering): the first present says how many frames follow.
        VkSetPresentConfigNV metering{VK_STRUCTURE_TYPE_SET_PRESENT_CONFIG_NV};
        metering.presentConfigFeedback = ~0u;   // (left unset by the driver: read as a refusal)
        const bool metered = (i == 0 && meteringFrames > 0 && ctx.presentMetering());
        const void* chain = proved ? &proof : nullptr;
        if (metered)
        {
            metering.numFramesPerBatch = meteringFrames;
            metering.pNext = chain;
            chain = &metering;
        }
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.pNext = chain;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &wait;
        pi.swapchainCount = 1;
        pi.pSwapchains = &mSwapchain;
        pi.pImageIndices = &image;
        VkResult r = VK_SUCCESS;
        if (vkSimulateDeviceLostTake(VK_SIM_LOST_PRESENT))
            r = VK_ERROR_DEVICE_LOST;          // RenderVKDebugSimulateDeviceLost 3: not presented
        else
        {   // #1544: inside the queue's lock where the renderer shares it -- IMMEDIATE there, so the call returns
            // at once (llfg::sharedQueueHeld: no such chain in a mode the compositor can hold)
            const VkDeviceContext::QueueLock lock(ctx.presentQueue());
            if (lock.gaveUp()) return false;   // (the device was lost while this waited for the queue)
            const VkDriverCallScope inDriver("vkQueuePresentKHR (frame generation)");   // #1546
            r = vkQueuePresentKHR(ctx.presentQueue(), &pi);
        }
        if (metered && outFeedback && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR))
            *outFeedback = metering.presentConfigFeedback;

        // As present(): what counts as queued, armed, or unknown.
        if (r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY) mPresentPending = true;
        if (proved && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR ||
            r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)) mPresentArmed[fence] = true;
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR &&
            r != VK_ERROR_SURFACE_LOST_KHR && r != VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT &&
            r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_DEVICE_LOST)
            mUnresolvedPresent = true; // unknown enqueue outcome is never a completion proof
        if (r == VK_SUCCESS) continue;
        ok = false;
        // The rest of the batch is still presented: each image was acquired, each gate submitted.
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        {
            if (outNeedsRecreate) *outNeedsRecreate = true;
            continue;
        }
        if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) { mExclusiveLost = true; continue; }
        std::fprintf(stderr, "[vkgfx] vkQueuePresentKHR (frame generation, %u of %u) = %s\n", i + 1, count,
                     vkResultString(r));
        if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR)
        {
            vkDeviceLostRaise(vkDeviceLostSimulated() ? "vkQueuePresentKHR (simulated)" : "vkQueuePresentKHR",
                              r, "graphics", __FILE__, __LINE__);
            break;
        }
        // Not queued: its wait semaphore stays signalled and its image acquired -- only a rebuild
        // gives them back (a binary semaphore must not be signalled again before a wait).
        if ((r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY) && outNeedsRecreate)
            *outNeedsRecreate = true;
    }
    return ok;
}
