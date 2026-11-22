// Standalone WSI protocol tests: no display, Vulkan driver, or viewer process.
// Link this file with ../vk_swapchain.cpp; every Vulkan entry used is a mock.
// Give the mocked device a non-null sentinel. No production test hooks or Vulkan
// device are created; include library headers before exposing only this fixture.
#include "../vk_common.h"
#include "../vk_requirements.h"
#include <map>
#include <functional>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <chrono>
#define private public
#include "../vk_device.h"
#include "../vk_swapchain.h"
#undef private
#include "../vk_retirement_retry.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>

namespace
{
uintptr_t nextHandle = 1;
template<class T> T handle() { return reinterpret_cast<T>(nextHandle++); }
VkResult acquireResult = VK_SUCCESS;
VkResult presentResult = VK_SUCCESS;
VkResult deviceWaitResult = VK_SUCCESS;   // vkqDeviceWaitIdle's answer
bool failView = false, failFenceOnce = false;
unsigned resets = 0, lostCalls = 0, deviceWaits = 0;
unsigned formatIncomplete = 0, imageIncomplete = 0;
bool completeAcquire = true, completePresent = true, failReset = false;
std::unordered_map<VkFence, bool> signaled;
VkFence lastAcquire = VK_NULL_HANDLE, lastPresent = VK_NULL_HANDLE;
uint64_t acquireTimeout = UINT64_MAX;
VkPresentModeKHR createdMode = VK_PRESENT_MODE_FIFO_KHR;
bool offerMailbox = true, offerImmediate = true;
VkImageUsageFlags createdUsage = 0;
uint32_t createdImages = 0;              // the minImageCount the last chain was created with
uint32_t surfaceMinImages = 2, surfaceMaxImages = 3;
unsigned acquireCalls = 0, presentCalls = 0, gateSubmits = 0;
VkSemaphore acquiredWith = VK_NULL_HANDLE;
VkSemaphore waitedPresent = VK_NULL_HANDLE;
std::unordered_set<VkSemaphore> semaphores;
std::unordered_set<VkFence> fences;
std::unordered_set<VkImageView> views;
std::unordered_set<VkSwapchainKHR> chains;
// #1517 (Task 7c fix 4): a fence the mock never signals -- a present proof the compositor withholds (a hidden
// window), or a frame the GPU never finishes. A wait for one: finite, VK_TIMEOUT; infinite, the hang watchdog's
// (it would raise a GPU hang after its slices: counted, and answered as a lost device).
std::unordered_set<VkFence> withheld;
unsigned finiteWaitsOnWithheld = 0, infiniteWaitsOnWithheld = 0;
std::vector<std::pair<VkFence, uint64_t>> fenceWaits;   // every wait: its fence and timeout
unsigned fencePolls = 0;                   // vkGetFenceStatus calls
bool loseDeviceInFenceWait = false;        // the next wait on a withheld fence finds the device lost
}

bool deviceLost = false;                   // vkDeviceIsLost()
bool vkDeviceIsLost() { return deviceLost; }
bool vkDeviceLostSimulated() { return false; }
bool vkSimulateDeviceLostTake(int) { return false; }
void vkDriverCallBegin(const char*) {}   // #1546 thread state at a loss (not exercised here)
void vkDriverCallEnd() {}
void vkDeviceLostRaise(const char*, VkResult, const char*, const char*, int) { ++lostCalls; }
const char* vkResultString(VkResult) { return "mock Vulkan result"; }
void vkAddrFrameTick() {}
void (*gVkDeviceLostHandler)(const char*, VkResult, const char*, const char*, int) = nullptr;
void (*gVkFatalHandler)(const char*, VkResult, const char*, int) = nullptr;
VkResult vkqDeviceWaitIdle(VkDevice, const char*, int) { ++deviceWaits; return deviceWaitResult; }
VkResult vkqWaitForFences(VkDevice, uint32_t count, const VkFence* fs, VkBool32, uint64_t timeout, const char*, int)
{
    bool blocked = false;
    for (uint32_t i = 0; i < count; ++i)
    {
        fenceWaits.push_back({fs[i], timeout});
        blocked = blocked || withheld.count(fs[i]) != 0;
    }
    if (!blocked) return VK_SUCCESS;
    if (loseDeviceInFenceWait) { loseDeviceInFenceWait = false; deviceLost = true; return VK_ERROR_DEVICE_LOST; }
    if (timeout != UINT64_MAX) { ++finiteWaitsOnWithheld; return VK_TIMEOUT; }
    ++infiniteWaitsOnWithheld;
    return VK_ERROR_DEVICE_LOST;   // (vkSlicedWait: "GPU hang"; the caller's VK_CHECK raises it here)
}
VkResult vkqGetFenceStatus(VkDevice, VkFence fence, const char*, int) { ++fencePolls; return signaled[fence] ? VK_SUCCESS : VK_NOT_READY; }
VkResult vkqQueueSubmit2(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence, const char*, int) { ++gateSubmits; return VK_SUCCESS; }
// #1544 (D8): the queue lock's scopes, counted per queue (the real one locks only a shared queue), and the
// present thread's own wait for a gate on a shared queue.
namespace
{
std::vector<VkQueue> queueLocks;           // every QueueLock made, in order
unsigned queueLocksOpen = 0;               // ... and not yet left
unsigned presentsOutsideLock = 0;          // vkQueuePresentKHR calls made outside any QueueLock scope
unsigned gateWaits = 0;                    // vkqWaitSemaphores calls
VkSemaphore gateWaited = VK_NULL_HANDLE;
uint64_t gateWaitedValue = 0, gateWaitTimeout = 0;
VkResult gateWaitResult = VK_SUCCESS;
}
VkDeviceContext::QueueLock::QueueLock(VkQueue q) { queueLocks.push_back(q); ++queueLocksOpen; }
VkDeviceContext::QueueLock::~QueueLock() { --queueLocksOpen; }
VkResult vkqWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo* wi, uint64_t timeout, const char*, int)
{
    ++gateWaits;
    gateWaited = wi->pSemaphores[0];
    gateWaitedValue = wi->pValues[0];
    gateWaitTimeout = timeout;
    return gateWaitResult;
}
// Entry points vk_swapchain.cpp reaches that this fixture does not exercise (HDR metadata only).
bool VkDeviceContext::optionalExtensionEnabled(const char*) const { return false; }
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char*) { return nullptr; }
VkResult vkqCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* out)
{
    if (failView) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = handle<VkImageView>(); views.insert(*out); return VK_SUCCESS;
}
void vkqDestroyImageView(VkDevice, VkImageView view, const VkAllocationCallbacks*) { assert(views.erase(view) == 1); }
void vkCompatGetDeviceImageMemoryRequirements(VkDevice, const VkDeviceImageMemoryRequirements*, VkMemoryRequirements2* out)
{ out->memoryRequirements.size = 4096; }

extern "C"
{
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32* out)
{ *out = VK_TRUE; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* out)
{
    *out = {};
    out->minImageCount = surfaceMinImages;
    out->maxImageCount = surfaceMaxImages;
    out->currentExtent = {800, 600};
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count, VkSurfaceFormatKHR* out)
{
    if (out && formatIncomplete) { --formatIncomplete; return VK_INCOMPLETE; }
    if (out) out[0] = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    *count = 1; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count, VkPresentModeKHR* out)
{
    // (offerMailbox / offerImmediate: a surface without them, as Wayland without tearing control)
    VkPresentModeKHR modes[3] = {VK_PRESENT_MODE_FIFO_KHR};
    uint32_t n = 1;
    if (offerMailbox) modes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
    if (offerImmediate) modes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    if (out) for (uint32_t i = 0; i < n && i < *count; ++i) out[i] = modes[i];
    *count = n; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks*, VkSwapchainKHR* out)
{
    createdMode = info->presentMode;
    createdUsage = info->imageUsage;
    createdImages = info->minImageCount;
    *out = handle<VkSwapchainKHR>(); chains.insert(*out); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR chain, const VkAllocationCallbacks*)
{ assert(chains.erase(chain) == 1); }
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t* count, VkImage* out)
{
    // (as many as asked for: the chain's images are what the swapchain was created with)
    if (out && imageIncomplete) { --imageIncomplete; return VK_INCOMPLETE; }
    if (out) for (unsigned i = 0; i < createdImages && i < *count; ++i) out[i] = handle<VkImage>();
    *count = createdImages; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* out)
{ *out = handle<VkSemaphore>(); semaphores.insert(*out); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice, VkSemaphore semaphore, const VkAllocationCallbacks*)
{ assert(semaphores.erase(semaphore) == 1); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice, const VkFenceCreateInfo* info, const VkAllocationCallbacks*, VkFence* out)
{
    if (failFenceOnce) { failFenceOnce = false; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    *out = handle<VkFence>(); fences.insert(*out);
    signaled[*out] = bool(info->flags & VK_FENCE_CREATE_SIGNALED_BIT);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*)
{ assert(fences.erase(fence) == 1); }
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t count, const VkFence* values)
{ ++resets; if (failReset) return VK_ERROR_OUT_OF_DEVICE_MEMORY; for (uint32_t i = 0; i < count; ++i) signaled[values[i]] = false; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t* out)
{
    ++acquireCalls; acquiredWith = sem;
    acquireTimeout = timeout; *out = 2; lastAcquire = fence;
    if (fence && (acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR)) signaled[fence] = completeAcquire;
    return acquireResult;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR* info)
{
    assert(info->waitSemaphoreCount == 1 && info->pImageIndices[0] == 2);
    ++presentCalls;
    if (!queueLocksOpen || queueLocks.back() != q) ++presentsOutsideLock;   // (#1544: every present, on any chain)
    waitedPresent = info->pWaitSemaphores[0];
    if (info->pNext)
    {
        const auto* proof = static_cast<const VkSwapchainPresentFenceInfoEXT*>(info->pNext);
        assert(proof->swapchainCount == 1);
        lastPresent = proof->pFences[0];
        if (presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR ||
            presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_ERROR_SURFACE_LOST_KHR)
            signaled[lastPresent] = completePresent;
    }
    return presentResult;
}
}

int main()
{
    // A permanent queue OOM must not turn a 1000 Hz main loop into 1000 idle
    // waits per second. Only three attempts are admitted; success resets policy.
    VkRetirementRetry retry;
    unsigned attempts = 0;
    const auto epoch = VkRetirementRetry::Clock::time_point{};
    for (unsigned tick = 0; tick < 10000; ++tick)
    {
        const auto now = epoch + std::chrono::milliseconds(tick);
        if (retry.ready(now)) { ++attempts; retry.refused(now); }
    }
    assert(attempts == 3 && retry.exhausted());
    retry.succeeded();
    assert(retry.ready(epoch));
    VkDeviceContext device;
    device.mDevice = handle<VkDevice>();
    device.mPhysical = handle<VkPhysicalDevice>();
    device.mQueue = handle<VkQueue>();
    // Main-window count/fill churn during repeated FG chain rebuilds: neither
    // surface enumeration nor swapchain image enumeration may enter VK_CHECK.
    device.mPresentQueue = handle<VkQueue>();
    for (unsigned rebuild = 0; rebuild < 8; ++rebuild)
    {
        VkSwapchain rebuilt;
        rebuilt.setFrameGen(rebuild % 4);
        formatIncomplete = 1;
        imageIncomplete = 1;
        assert(rebuilt.init(device, handle<VkSurfaceKHR>(), 800, 600));
        assert(formatIncomplete == 0 && imageIncomplete == 0);
        assert(rebuilt.destroy(device));
    }
    {
        VkSwapchain deferred;
        formatIncomplete = 4;
        assert(!deferred.init(device, handle<VkSurfaceKHR>(), 800, 600));
        assert(formatIncomplete == 0);
        assert(deferred.destroy(device));
    }
    device.mPresentQueue = VK_NULL_HANDLE;
    VkSwapchain primary, auxiliary;
    VkSwapchain::sVSync = true;
    const VkSurfaceKHR surface = handle<VkSurfaceKHR>();
    assert(primary.init(device, surface, 800, 600));
    assert(createdMode == VK_PRESENT_MODE_FIFO_KHR);
    auxiliary.setUIOnly();
    assert(auxiliary.init(device, surface, 800, 600));
    assert(createdMode == VK_PRESENT_MODE_MAILBOX_KHR);
    assert(createdUsage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    assert(VkSwapchain::sVSync); // auxiliary policy never changes the main setting
    assert(primary.presentMode() == VK_PRESENT_MODE_FIFO_KHR);
    assert(auxiliary.imageCount() == 3 && auxiliary.framesInFlight() == 2);
    assert(auxiliary.memoryBytes() == 3 * 4096);
    assert(auxiliary.renderFinishedSemaphore(0) != auxiliary.renderFinishedSemaphore(2));
    assert(auxiliary.slotDone(handle<VkDevice>(), 0));
    uint32_t image = 0;
    bool recreate = false;
    acquireResult = VK_TIMEOUT;
    assert(!auxiliary.acquire(device, &image, &recreate));
    assert(acquireTimeout == 0 && resets == 1 && !recreate);
    acquireResult = VK_ERROR_OUT_OF_DATE_KHR;
    assert(!auxiliary.acquire(device, &image, &recreate));
    assert(recreate && resets == 2);
    acquireResult = VK_SUBOPTIMAL_KHR;
    assert(auxiliary.acquire(device, &image, &recreate));
    assert(recreate && resets == 4 && image == 2); // delivered image MUST be presented
    assert(auxiliary.present(device, image, &recreate));
    assert(waitedPresent == auxiliary.renderFinishedSemaphore(2));
    assert(auxiliary.frameSlot() == 1);
    acquireResult = VK_ERROR_SURFACE_LOST_KHR;
    assert(!auxiliary.acquire(device, &image, &recreate));
    assert(auxiliary.failed() && lostCalls == 0); // never turns an auxiliary surface loss into app loss
    assert(!auxiliary.retired(device.device())); // fallback present still needs the retirement batch
    const unsigned beforeRetire = deviceWaits;
    auxiliary.presentingQueueIdle(); // owner has performed the one batched presenting-queue wait
    assert(auxiliary.retired(device.device()));
    auxiliary.destroy(device);
    assert(deviceWaits == beforeRetire); // aux destruction itself NEVER waits device idle
    primary.destroy(device);
    assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());

    // Allocation failure during view creation returns control and partial
    // teardown releases the chain, with no fatal/abort or leaked sync objects.
    VkSwapchain refused;
    refused.setUIOnly(); failView = true;
    assert(!refused.init(device, surface, 800, 600));
    refused.destroy(device);
    assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());

    // Present-fence fast path: stop resize/close until both GPU and PE proofs
    // complete. No device wait, no successor chain, reset-but-unsubmitted fence
    // does not prevent destruction of an abandoned acquisition.
    failView = false;
    VkSwapchain fast;
    fast.setUIOnly(true);
    assert(fast.init(device, surface, 800, 600));
    acquireResult = VK_SUCCESS; presentResult = VK_SUCCESS;
    completePresent = false;
    assert(fast.acquire(device, &image, &recreate));
    const VkFence gpu = fast.inFlightFence();
    fast.submitted();
    assert(fast.present(device, image, &recreate));
    const unsigned fastWaits = deviceWaits;
    assert(!fast.retired(device.device()));
    assert(!fast.recreate(device, 900, 700));
    signaled[gpu] = true;
    assert(!fast.retired(device.device()));
    signaled[lastPresent] = true;
    assert(fast.retired(device.device()));
    assert(fast.recreate(device, 900, 700));
    assert(deviceWaits == fastWaits);
    completeAcquire = false;
    assert(fast.acquire(device, &image, &recreate));
    assert(!fast.retired(device.device())); // acquire signal is not queue work
    signaled[lastAcquire] = true;
    assert(fast.retired(device.device())); // no accepted GPU submit for reset slot
    fast.destroy(device);
    assert(deviceWaits == fastWaits);
    assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());

    VkSwapchain resetRefused;
    resetRefused.setUIOnly(true);
    assert(resetRefused.init(device, surface, 800, 600));
    completeAcquire = true;
    assert(resetRefused.acquire(device, &image, &recreate));
    const VkFence resetGpu = resetRefused.inFlightFence();
    resetRefused.submitted();
    failReset = true;
    assert(!resetRefused.present(device, image, &recreate));
    assert(resetRefused.failed()); // no future acquire can silently exhaust images
    assert(!resetRefused.retired(device.device()));
    signaled[resetGpu] = true;
    assert(resetRefused.retired(device.device())); // no present was accepted
    failReset = false;
    resetRefused.destroy(device);
    assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());

    // Optional present proofs must not turn main-window allocation refusal into
    // VK_CHECK abort. Keep its usable chain on the existing teardown policy.
    device.mSwapchainMaintenance = true;
    failFenceOnce = true;
    VkSwapchain mainRefusedProof;
    assert(mainRefusedProof.init(device, surface, 800, 600));
    assert(mainRefusedProof.valid());
    mainRefusedProof.destroy(device);
    assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());
    device.mSwapchainMaintenance = false;

    // #1517 frame generation: a chain built for it has the generated frames + the real one in images
    // (the present thread takes each just before it is shown), never fewer than the surface's minimum
    // and never more than its maximum, and only the present thread's calls reach its images.
    acquireResult = VK_SUCCESS; presentResult = VK_SUCCESS;
    device.mPresentQueue = handle<VkQueue>();
    for (const uint32_t g : {1u, 2u, 3u})
    {
        VkSwapchain fg;
        VkSwapchain::sVSync = true;
        fg.setFrameGen(g);
        assert(fg.init(device, surface, 800, 600));
        const uint32_t expect = std::min(std::max(g + 1, surfaceMinImages), surfaceMaxImages);
        assert(createdImages == expect && fg.imageCount() == expect);
        assert(fg.frameGenImages() == expect && fg.frameGenMax() == g);
        assert(fg.ignoresVsync() && createdMode == VK_PRESENT_MODE_IMMEDIATE_KHR);   // (vsync asked for: ignored)
        fg.destroy(device);
        assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());
    }
    // (#1517 Task 11b fix 1, M3) without IMMEDIATE the chain is MAILBOX (else FIFO) with one image more, so a
    // newer present can replace one not yet shown: generated + 2, within the surface's limits.
    surfaceMaxImages = 8;
    for (const bool mailbox : {true, false})
    {
        offerImmediate = false;
        offerMailbox = mailbox;
        for (const uint32_t g : {1u, 2u, 3u})
        {
            VkSwapchain fg;
            fg.setFrameGen(g);
            assert(fg.init(device, surface, 800, 600));
            const uint32_t expect = std::min(std::max(g + 2, surfaceMinImages), surfaceMaxImages);
            assert(createdMode == (mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR));
            assert(fg.fgPresentMode() == (mailbox ? llfg::PresentMode::Mailbox : llfg::PresentMode::Fifo));
            assert(createdImages == expect && fg.frameGenImages() == expect && fg.frameGenMax() == g);
            fg.destroy(device);
            assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());
        }
    }
    offerImmediate = offerMailbox = true;
    surfaceMaxImages = 3;
    {
        surfaceMinImages = 3;   // a surface that wants more than 2x needs: its minimum
        VkSwapchain fg;
        fg.setFrameGen(1);
        assert(fg.init(device, surface, 800, 600));
        assert(createdImages == 3 && fg.frameGenImages() == 3);
        fg.destroy(device);
        surfaceMinImages = 2;
    }
    {
        VkSwapchain fg;
        fg.setFrameGen(2);
        assert(fg.init(device, surface, 800, 600) && fg.frameGenImages() == 3);
        // The main thread's acquire / present refuse the chain: the driver never sees them.
        const unsigned acquires = acquireCalls, presents = presentCalls;
        assert(!fg.acquire(device, &image, &recreate) && !recreate);
        assert(!fg.present(device, 2, &recreate));
        assert(acquireCalls == acquires && presentCalls == presents);
        // The frame slot is the main thread's; the present thread's calls never move it.
        assert(fg.beginFrameWithoutImage(device));
        const uint32_t slot = fg.frameSlot();
        const VkSemaphore mine = handle<VkSemaphore>();
        uint32_t got = 0;
        assert(fg.acquireOne(device, mine, 1000000, &got, &recreate) && got == 2 && !recreate);
        assert(acquiredWith == mine && acquireTimeout == 1000000);
        VkSwapchain::BatchPresent first{got, 0};
        uint32_t feedback = 0;
        assert(fg.presentBatch(device, &first, 1, VK_NULL_HANDLE, 0, &feedback, &recreate));
        assert(waitedPresent == fg.renderFinishedSemaphore(2) && feedback == ~0u && !fg.gatedPresentsPending());
        // A gated present waits the gate's release, not the image's render-finished, through one submit.
        assert(fg.acquireOne(device, mine, 1000000, &got, &recreate));
        const VkSemaphore gate = handle<VkSemaphore>();
        VkSwapchain::BatchPresent gated{got, 7};
        const unsigned submits = gateSubmits;
        assert(fg.presentBatch(device, &gated, 1, gate, 0, &feedback, &recreate));
        assert(gateSubmits == submits + 1 && waitedPresent != fg.renderFinishedSemaphore(2));
        assert(fg.gatedPresentsPending());
        fg.gatesReleased();
        assert(fg.frameSlot() == slot);
        // An image that is not ready in time: no image, nothing to present, no rebuild asked for.
        acquireResult = VK_TIMEOUT;
        assert(!fg.acquireOne(device, mine, 1000000, &got, &recreate) && !recreate);
        acquireResult = VK_ERROR_OUT_OF_DATE_KHR;
        assert(!fg.acquireOne(device, mine, 1000000, &got, &recreate) && recreate);
        acquireResult = VK_SUCCESS;
        fg.endFrameWithoutPresent();
        // At most kMaxBatchPresents images held without a present: the next acquire is refused before the
        // driver sees it (the present thread's batches never hold more).
        const unsigned before = acquireCalls;
        for (uint32_t k = 0; k < VkSwapchain::kMaxBatchPresents; ++k)
            assert(fg.acquireOne(device, mine, 1000000, &got, &recreate));
        assert(acquireCalls == before + VkSwapchain::kMaxBatchPresents);
        assert(!fg.acquireOne(device, mine, 1000000, &got, &recreate) && !recreate);
        assert(acquireCalls == before + VkSwapchain::kMaxBatchPresents);
        // A rebuild with images acquired and never presented (a submit refused outright, an out-of-date
        // chain): the new chain starts with none held, and every object of the old one is gone.
        const VkSwapchainKHR old = fg.handle();
        assert(fg.recreate(device, 800, 600) && fg.handle() != old && fg.frameGenImages() == 3);
        assert(chains.size() == 1);
        assert(fg.acquireOne(device, mine, 1000000, &got, &recreate) && got == 2);
        VkSwapchain::BatchPresent again{got, 0};
        assert(fg.presentBatch(device, &again, 1, VK_NULL_HANDLE, 0, &feedback, &recreate));
        // #1517 (fix 3): a rebuild whose device wait failed without a lost device (no memory) is still rebuilt,
        // but says the device was not seen idle (frame generation's owed release waits for one that was).
        assert(fg.deviceIdled());
        deviceWaitResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        const unsigned waitsBefore = deviceWaits;
        assert(fg.recreate(device, 800, 600) && deviceWaits == waitsBefore + 1);
        assert(!fg.deviceIdled() && lostCalls == 0);
        deviceWaitResult = VK_SUCCESS;
        assert(fg.recreate(device, 800, 600) && fg.deviceIdled());
        fg.destroy(device);
        assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());
    }
    assert(presentsOutsideLock == 0 && queueLocksOpen == 0);

    // #1544 (D8) one graphics queue: the present thread shares the renderer's. Every present is made inside the
    // queue's lock, for the queue it presents on; a gated present submits nothing -- the caller waits the gate
    // itself, with the policy's bound, and presents with the image's own render-finished (timed out or not);
    // and a surface without IMMEDIATE gets no frame generation chain at all, vsync as asked.
    {
        const VkQueue mainQueue = handle<VkQueue>();
        device.mQueue = mainQueue;
        device.mPresentQueue = mainQueue;
        device.mPresentQueueShared = true;
        VkSwapchain::sVSync = true;
        VkSwapchain fg;
        fg.setFrameGen(2);   // (3 images: the mock hands out image 2)
        assert(fg.init(device, surface, 800, 600));
        assert(fg.frameGenImages() == 3 && !fg.frameGenQueueHeld() && createdMode == VK_PRESENT_MODE_IMMEDIATE_KHR);
        bool recreate = false;
        uint32_t got = 0, feedback = 0;
        const VkSemaphore mine = handle<VkSemaphore>();
        const VkSemaphore gate = handle<VkSemaphore>();
        for (const VkResult waited : {VK_SUCCESS, VK_TIMEOUT})
        {
            gateWaitResult = waited;
            assert(fg.acquireOne(device, mine, 1000000, &got, &recreate));
            const unsigned submits = gateSubmits, waits = gateWaits, presents = presentCalls;
            queueLocks.clear();
            VkSwapchain::BatchPresent gated{got, 7};
            assert(fg.presentBatch(device, &gated, 1, gate, 0, &feedback, &recreate));
            assert(gateSubmits == submits && gateWaits == waits + 1 && presentCalls == presents + 1);
            assert(gateWaited == gate && gateWaitedValue == 7);
            assert(gateWaitTimeout == uint64_t(llfg::kGateWaitBoundMs) * 1000000ull && gateWaitTimeout != UINT64_MAX);
            assert(waitedPresent == fg.renderFinishedSemaphore(got) && !fg.gatedPresentsPending());
            assert(queueLocks.size() == 1 && queueLocks[0] == mainQueue && queueLocksOpen == 0);
        }
        gateWaitResult = VK_SUCCESS;
        // (an ungated present waits for nothing)
        assert(fg.acquireOne(device, mine, 1000000, &got, &recreate));
        const unsigned waits = gateWaits;
        VkSwapchain::BatchPresent plain{got, 0};
        assert(fg.presentBatch(device, &plain, 1, gate, 0, &feedback, &recreate) && gateWaits == waits);
        fg.destroy(device);

        for (const bool mailbox : {true, false})
        {
            offerImmediate = false;
            offerMailbox = mailbox;
            for (const bool vsync : {true, false})
            {
                VkSwapchain::sVSync = vsync;
                VkSwapchain held;
                held.setFrameGen(1);
                assert(held.init(device, surface, 800, 600));
                assert(held.frameGenQueueHeld() && held.frameGenImages() == 0 && held.frameGenMax() == 0 && !held.ignoresVsync());
                assert(createdMode == (vsync || !mailbox ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_MAILBOX_KHR));
                // (the renderer's own present on that chain: inside the lock too)
                uint32_t image = 0;
                queueLocks.clear();
                assert(held.acquire(device, &image, &recreate));
                (void)held.present(device, image, &recreate);
                assert(queueLocks.size() == 1 && queueLocks[0] == mainQueue && queueLocksOpen == 0);
                held.destroy(device);
            }
            // ... found without frame generation being asked for (the render loop reads it before it asks)
            VkSwapchain plainChain;
            assert(plainChain.init(device, surface, 800, 600) && plainChain.frameGenQueueHeld());
            plainChain.destroy(device);
        }
        offerImmediate = offerMailbox = true;
        // a queue of its own: never held, whatever the surface offers
        device.mPresentQueueShared = false;
        device.mPresentQueue = handle<VkQueue>();
        offerImmediate = false;
        VkSwapchain own;
        own.setFrameGen(1);
        assert(own.init(device, surface, 800, 600) && !own.frameGenQueueHeld() && own.frameGenImages() != 0);
        own.destroy(device);
        offerImmediate = true;
        VkSwapchain::sVSync = true;
        assert(presentsOutsideLock == 0 && queueLocksOpen == 0);
        assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());
    }
    device.mPresentQueue = VK_NULL_HANDLE;

    // #1517 (Task 7c fix 4, h): a main chain's present proof the compositor withholds (a FIFO present of a
    // hidden window) is waited with a bound, never through the hang watchdog. Withheld: the rebuild is
    // deferred -- nothing destroyed, the chain as it was, no lost device -- and made once the proof is there;
    // a destroy keeps the chain. The device wait still comes first.
    gVkDeviceLostHandler = [](const char*, VkResult, const char*, const char*, int) {};   // (a raise returns)
    {
        device.mSwapchainMaintenance = true;
        acquireResult = VK_SUCCESS; presentResult = VK_SUCCESS;
        completePresent = false;
        VkSwapchain shown;
        assert(shown.init(device, surface, 800, 600));
        assert(shown.acquire(device, &image, &recreate));
        shown.submitted();
        assert(shown.present(device, image, &recreate));
        const VkFence proof = lastPresent;
        assert(proof && !signaled[proof]);
        withheld.insert(proof);
        const VkSwapchainKHR old = shown.handle();
        const size_t views0 = views.size(), fences0 = fences.size(), sems0 = semaphores.size();
        const unsigned lost0 = lostCalls, waits0 = deviceWaits;
        finiteWaitsOnWithheld = infiniteWaitsOnWithheld = 0;
        assert(!shown.recreate(device, 900, 700));
        assert(shown.rebuildDeferred() && shown.handle() == old && shown.valid());
        assert(views.size() == views0 && fences.size() == fences0 && semaphores.size() == sems0 && chains.count(old) == 1);
        assert(finiteWaitsOnWithheld >= 1 && infiniteWaitsOnWithheld == 0 && lostCalls == lost0);
        assert(deviceWaits == waits0 + 1);
        for (const auto& w : fenceWaits)
            if (w.first == proof) assert(w.second <= VkSwapchain::kRebuildProofWaitNs);
        assert(shown.rebuildDeferredNote());                  // (the first of a row: the owner logs it)
        // (fix 5, Minor 5) Asked again while the proof is still withheld: only a look (vkGetFenceStatus) -- the
        // main thread never waits half a second per try -- and nothing to log, until a minute has passed.
        for (int k = 0; k < 3; ++k)
        {
            const size_t waitsOnProof = std::count_if(fenceWaits.begin(), fenceWaits.end(),
                                                      [&](const auto& w) { return w.first == proof; });
            const unsigned polls0 = fencePolls, finite0 = finiteWaitsOnWithheld;
            assert(!shown.recreate(device, 900, 700) && shown.rebuildDeferred() && shown.handle() == old);
            assert(!shown.rebuildDeferredNote());
            assert(fencePolls > polls0 && finiteWaitsOnWithheld == finite0 && infiniteWaitsOnWithheld == 0);
            assert((size_t)std::count_if(fenceWaits.begin(), fenceWaits.end(),
                                         [&](const auto& w) { return w.first == proof; }) == waitsOnProof);
            assert(chains.count(old) == 1 && lostCalls == lost0);
        }
        shown.mProofDeferredSince -= std::chrono::seconds(61);   // a minute later: said once more
        assert(!shown.recreate(device, 900, 700) && shown.rebuildDeferredNote());
        assert(!shown.recreate(device, 900, 700) && !shown.rebuildDeferredNote());
        assert(!shown.destroy(device));                       // (kept: the proof is still withheld)
        assert(shown.destroyWithheld());
        assert(chains.count(old) == 1 && infiniteWaitsOnWithheld == 0 && lostCalls == lost0);
        // (Minor 7) A destroy on a lost device keeps the chain too, but no present is held: not "withheld".
        deviceLost = true;
        assert(!shown.destroy(device) && !shown.destroyWithheld());
        deviceLost = false;
        loseDeviceInFenceWait = true;                         // ... nor when the proof's wait found it lost
        assert(!shown.destroy(device) && !shown.destroyWithheld() && deviceLost);
        deviceLost = false;
        withheld.erase(proof);                                // the window shows again: the present completes
        signaled[proof] = true;
        assert(shown.recreate(device, 900, 700) && !shown.rebuildDeferred() && shown.handle() != old);
        assert(!shown.rebuildDeferredNote());
        // A new row of deferrals starts with a bounded wait and a line again.
        assert(shown.acquire(device, &image, &recreate));
        shown.submitted();
        assert(shown.present(device, image, &recreate));
        const VkFence proof2 = lastPresent;
        withheld.insert(proof2);
        const unsigned finite1 = finiteWaitsOnWithheld;
        assert(!shown.recreate(device, 900, 700) && shown.rebuildDeferredNote() && finiteWaitsOnWithheld == finite1 + 1);
        withheld.erase(proof2);
        signaled[proof2] = true;
        assert(shown.recreate(device, 900, 700));
        assert(shown.destroy(device) && !shown.destroyWithheld());
        assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());
        completePresent = true;
        device.mSwapchainMaintenance = false;
    }
    // ... while the renderer's own frames are the GPU's: every frame-in-flight slot's fence is waited without a
    // bound (the hang watchdog's wait) and never reset -- a frame the GPU never finishes is its GPU hang.
    {
        VkSwapchain frames;
        assert(frames.init(device, surface, 800, 600));
        const unsigned resets0 = resets;
        fenceWaits.clear();
        assert(frames.waitFrameSlots(device));
        assert(fenceWaits.size() == VkSwapchain::kDefaultFramesInFlight && resets == resets0);
        for (const auto& w : fenceWaits) assert(w.second == UINT64_MAX);
        assert(frames.acquire(device, &image, &recreate));    // (its slot's fence reset: a frame in flight)
        const VkFence inFlight = frames.inFlightFence();
        withheld.insert(inFlight);                            // ... that the GPU never finishes
        const unsigned lost0 = lostCalls;
        infiniteWaitsOnWithheld = finiteWaitsOnWithheld = 0;
        (void)frames.waitFrameSlots(device);
        assert(infiniteWaitsOnWithheld == 1 && finiteWaitsOnWithheld == 0 && lostCalls == lost0 + 1);
        withheld.erase(inFlight);
        signaled[inFlight] = true;
        frames.submitted();
        assert(frames.present(device, image, &recreate));
        assert(frames.destroy(device));
        assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());
    }
    gVkDeviceLostHandler = nullptr;

    VkSwapchain ambiguous;
    ambiguous.setUIOnly(true);
    assert(ambiguous.init(device, surface, 800, 600));
    assert(ambiguous.acquire(device, &image, &recreate));
    signaled[ambiguous.inFlightFence()] = true;
    ambiguous.submitted();
    presentResult = VK_ERROR_UNKNOWN;
    assert(!ambiguous.present(device, image, &recreate));
    assert(ambiguous.failed() && !ambiguous.retired(device.device()));
    // Unresolved PE ownership intentionally remains bounded until process exit.
}
