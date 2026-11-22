// Tests for frame generation's present thread (indra/newview/vkgfx/llvkframegenpresent.*, #1517): the job
// logic of LLVKFrameGenPresent against a fake Vulkan. The fake GPU runs every accepted submit at once: it
// applies the recorded layout transitions, signals the submit's semaphores and consumes its waits. A submit
// stays pending (its command buffers may not be begun again, a host signal may not pass its signals) until
// the CPU has seen it complete: a host wait for a value it signalled, or its queue idled. The fake
// swapchain hands out images, checks each present (acquired, its render-finished signalled by an accepted
// submit, in PRESENT_SRC) and gives the image back. The generator, the composite and the pacer's gates are
// fakes that record what they were given. The device's waits are the fake's too: an infinite wait
// (UINT64_MAX) for something the fake GPU never finishes is what the device's hang watchdog would end as a
// "GPU hang" -- the fake raises it at once, under the waiting call's name, as vkSlicedWait does after its
// slices. The present queue is watched: a call on it from any thread but the present thread, while that
// thread exists, is a violation.
// Built and run by scripts/check/checks/framegen_present.py (also under UBSan and TSan).
#include "vk_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define private public
#include "vk_device.h"
#include "vk_swapchain.h"
#include "vk_display_encode.h"
#include "llvkframegen.h"
#include "llvkframegenpresent.h"
#undef private
#include "llvkaddonlock.h"
#include "vk_hang_watchdog.h"   // vkLostDeviceReported: the device layer's rule, as vk_device.cpp applies it

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

namespace fake
{
std::recursive_mutex m;   // every fake below (the present thread and the test thread call them)
using Lock = std::lock_guard<std::recursive_mutex>;
std::atomic<uintptr_t> next{0x1000};
template <class T> T handle() { return reinterpret_cast<T>(next.fetch_add(8)); }

struct Ev
{
    enum Kind { Barrier, Copy, Composite, Evaluate, Timestamp } kind;
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout from = VK_IMAGE_LAYOUT_UNDEFINED, to = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t index = 0, count = 0;
    bool reset = false;
};
struct Cmd
{
    bool recording = false;
    std::vector<Ev> ev;
};
struct Wait { VkSemaphore sem; uint64_t value; VkPipelineStageFlags2 stage; };
struct Submit
{
    VkQueue queue;
    std::vector<Wait> waits;
    std::vector<std::pair<VkSemaphore, uint64_t>> signals;
    std::vector<Ev> ev;
    bool done = false;   // the CPU has seen it complete (a host wait reached it, or its queue was idled)
};
struct Present { uint32_t image; uint64_t gate; uint32_t metering; };

std::map<VkCommandBuffer, Cmd> cmds;
std::map<VkImage, VkImageLayout> layout;
std::map<VkSemaphore, uint64_t> timeline;
std::set<VkSemaphore> binaryPending;          // an acquire signalled it, no submit waited it yet
std::set<VkSemaphore> renderFinished;         // signalled by an accepted submit, not yet presented
std::vector<Submit> submits;                  // accepted
std::map<VkCommandBuffer, size_t> cmdSubmit;  // a command buffer's last accepted submit (index in submits)
std::vector<Present> presents;
std::vector<std::string> violations;
std::vector<std::string> logs;
std::deque<VkResult> acquireScript, submitScript, presentScript, endScript, idleScript;
std::deque<bool> evalScript;
uint32_t acquireCalls = 0;                    // acquireOne calls
// The device's waits, as the main thread (or any other) made them.
struct HostWait { VkSemaphore sem; uint64_t value; uint64_t timeout; };
std::vector<HostWait> hostWaits;              // vkWaitSemaphores
uint32_t frameSlotWaits = 0;                  // VkSwapchain::waitFrameSlots
bool frameFenceStuck = false;                 // a renderer frame the GPU never finishes
uint32_t abandonedNoted = 0;                  // vkNoteThreadAbandoned
VkQueue presentQueue = VK_NULL_HANDLE;        // the fixture's
// A present the compositor holds (Wayland: the window cannot be seen) until the test lets it go; the fake
// present blocks before it takes the fakes' lock.
std::mutex presentHoldM;
std::condition_variable presentHoldCv;
bool holdPresents = false;
uint32_t presentsHolding = 0;
// An evaluate the test holds (the present thread waits inside a job until it is let go).
std::mutex holdM;
std::condition_variable holdCv;
bool holdEval = false;
uint32_t evalsHeld = 0;
std::vector<Ev> evaluates;                    // what the generator was asked (index, count, reset)
std::vector<std::pair<uint64_t, uint32_t>> gatesReleased;   // releaseGates(first, count)
std::vector<std::pair<uint64_t, uint32_t>> markers;         // (frame id, marker)
std::vector<std::pair<VkSemaphore, uint64_t>> hostSignals;  // vkSignalSemaphore
VkSemaphore holdTimeline = VK_NULL_HANDLE;                  // its submitted signals wait for flushHeld()
std::vector<std::pair<VkSemaphore, uint64_t>> held;
uint32_t lostRaised = 0, imagesMade = 0, failImageAfter = ~0u;
struct Made { VkFormat format; VkImageUsageFlags usage; };
std::vector<Made> made;                       // every image made (createImage2D), in order
VkFormat noColor = VK_FORMAT_UNDEFINED;
VkFormat noStorage = VK_FORMAT_UNDEFINED;     // a format the device cannot write as a storage image (#1536)
uint64_t genBytes = 0;                        // #1544: LLVKFrameGen::generatorBytes's answer
uint32_t blits = 0;                           // #1544: vkCmdBlitImage calls (a fallback across formats)
uint32_t lostQuiet = 0;                       // a raise that only set the state (vkLostDeviceReported false)
std::string lostCall;                         // the last raise's call and result
VkResult lostResult = VK_SUCCESS;
bool compositeOk = true;
std::atomic<bool> deviceLost{false};          // (the present thread's raise sets it too)
VkSwapchain* chain = nullptr;
LLVKFrameGenPresent* presenter = nullptr;     // the fixture's (whose thread owns the present queue)
std::vector<int> imageState;                  // per swapchain image: 0 free, 1 acquired

void violation(const std::string& v) { Lock l(m); violations.push_back(v); }

void reset()
{
    Lock l(m);
    cmds.clear(); layout.clear(); timeline.clear(); binaryPending.clear(); renderFinished.clear();
    submits.clear(); cmdSubmit.clear(); presents.clear(); violations.clear(); logs.clear();
    acquireScript.clear(); submitScript.clear(); presentScript.clear(); endScript.clear(); idleScript.clear();
    evalScript.clear();
    acquireCalls = 0;
    hostWaits.clear(); frameSlotWaits = 0; frameFenceStuck = false; abandonedNoted = 0;
    evaluates.clear(); gatesReleased.clear(); markers.clear(); hostSignals.clear(); held.clear();
    holdTimeline = VK_NULL_HANDLE;
    lostRaised = 0; lostQuiet = 0; imagesMade = 0; failImageAfter = ~0u; compositeOk = true; deviceLost = false;
    made.clear(); noColor = VK_FORMAT_UNDEFINED; noStorage = VK_FORMAT_UNDEFINED; genBytes = 0; blits = 0;
    lostCall.clear(); lostResult = VK_SUCCESS;
}
// The device's hang watchdog (vkSlicedWait): an infinite wait that made no progress for its slices raises the
// lost device as a GPU hang, under the waiting call's name, with VK_TIMEOUT.
VkResult gpuHang(const char* call);
// A call on the present queue: only the present thread's while that thread exists.
void presentQueueCall(VkQueue q, const char* what);
VkImage swapImage(uint32_t i) { return chain->mImages[i]; }
// Submit `last` and every submit before it on its queue are complete.
void completeThrough(size_t last)
{
    Lock l(m);
    for (size_t i = 0; i <= last && last < submits.size(); ++i)
        if (submits[i].queue == submits[last].queue) submits[i].done = true;
}
// Every submit is complete (the device idle: a swapchain rebuild).
void completeAll()
{
    Lock l(m);
    for (Submit& s : submits) s.done = true;
}
bool pending(VkCommandBuffer cmd)
{
    Lock l(m);
    const auto it = cmdSubmit.find(cmd);
    return it != cmdSubmit.end() && !submits[it->second].done;
}
void flushHeld()
{
    Lock l(m);
    for (const auto& h : held) timeline[h.first] = std::max(timeline[h.first], h.second);
    held.clear();
    holdTimeline = VK_NULL_HANDLE;
}
int imageOfView(VkImageView v)
{
    for (size_t i = 0; i < chain->mImageViews.size(); ++i)
        if (chain->mImageViews[i] == v) return (int)i;
    return -1;
}
template <class T> T take(std::deque<T>& q, T dflt)
{
    if (q.empty()) return dflt;
    T v = q.front();
    q.pop_front();
    return v;
}
} // namespace fake

namespace llstub
{
void logLine(const char* level, const std::string& line)
{
    fake::Lock l(fake::m);
    fake::logs.push_back(std::string(level) + ": " + line);
}
}

// ---- the device layer ----
bool vkDeviceIsLost() { return fake::deviceLost; }
void vkDeviceLostRaise(const char* call, VkResult result, const char*, const char*, int)
{
    fake::Lock l(fake::m);
    // (as vk_device.cpp: the state first; the report, the marker and the quarantine only while no renderer
    // thread was left behind -- the exit is under way then)
    fake::deviceLost = true;
    if (!vkLostDeviceReported(fake::abandonedNoted != 0))
    {
        ++fake::lostQuiet;
        return;
    }
    ++fake::lostRaised;
    fake::lostCall = call ? call : "";
    fake::lostResult = result;
}
void vkNoteThreadAbandoned()
{
    fake::Lock l(fake::m);
    ++fake::abandonedNoted;
}
VkResult fake::gpuHang(const char* call)
{
    Lock l(m);
    if (!deviceLost) vkDeviceLostRaise(call, VK_TIMEOUT, nullptr, __FILE__, __LINE__);   // (once: then lost)
    deviceLost = true;
    return VK_ERROR_DEVICE_LOST;
}
void fake::presentQueueCall(VkQueue q, const char* what)
{
    if (q != presentQueue || !presenter) return;
    if (presenter->mHandoff.alive() && !presenter->mHandoff.onThread())
        violation(std::string(what) + " on the present queue from another thread while the present thread exists");
}
const char* vkResultString(VkResult) { return "fake Vulkan result"; }
VkMemScope::VkMemScope(VkMemCat) : mPrev(0) {}
VkMemScope::~VkMemScope() {}
static thread_local bool tSoft = false;
void VkDeviceContext::setSoftAllocFail(bool on) { tSoft = on; }
bool VkDeviceContext::softAllocFail() const { return tSoft; }
static uint32_t bytesPerPixel(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R8G8B8A8_UNORM: return 4;
    default: return 4;
    }
}
uint64_t VkDeviceContext::imageMemoryBytes(uint32_t w, uint32_t h, uint32_t, uint32_t, VkFormat format,
                                           VkImageUsageFlags, bool, const struct VkImageExtra*) const
{
    return uint64_t(w) * h * bytesPerPixel(format);
}
GpuImage VkDeviceContext::createImage2D(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags,
                                        uint32_t, VkFormat, const char*)
{
    fake::Lock l(fake::m);
    GpuImage g;
    if (fake::imagesMade >= fake::failImageAfter)
    {
        if (!tSoft) fake::violation("a hard image allocation failed");
        return g;
    }
    ++fake::imagesMade;
    fake::made.push_back({format, usage});
    g.image = fake::handle<VkImage>();
    g.view = fake::handle<VkImageView>();
    g.memory = fake::handle<VkDeviceMemory>();
    g.format = format;
    g.extent = {w, h};
    return g;
}
void VkDeviceContext::destroyImage(GpuImage& img)
{
    fake::Lock l(fake::m);
    if (fake::imagesMade == 0) fake::violation("an image destroyed twice");
    else --fake::imagesMade;
    img = GpuImage{};
}
// #1517 Task 7e: the label buffers (ensureLabels)
GpuBuffer VkDeviceContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags, VkMemoryPropertyFlags, bool, const char*)
{
    GpuBuffer b;
    b.buffer = fake::handle<VkBuffer>();
    b.size = size;
    return b;
}
void VkDeviceContext::destroyBuffer(GpuBuffer& b) { b = GpuBuffer{}; }

VkResult vkqQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence, const char*, int)
{
    fake::presentQueueCall(q, "vkQueueSubmit2");
    fake::Lock l(fake::m);
    const VkResult r = fake::take(fake::submitScript, VK_SUCCESS);
    if (r != VK_SUCCESS)
    {
        if (r == VK_ERROR_DEVICE_LOST) fake::deviceLost = true;
        return r;   // (a refused submit changes nothing)
    }
    for (uint32_t b = 0; b < n; ++b)
    {
        fake::Submit s;
        s.queue = q;
        for (uint32_t i = 0; i < si[b].waitSemaphoreInfoCount; ++i)
        {
            const VkSemaphoreSubmitInfo& w = si[b].pWaitSemaphoreInfos[i];
            s.waits.push_back({w.semaphore, w.value, w.stageMask});
            if (fake::timeline.count(w.semaphore))
            {
                if (fake::timeline[w.semaphore] < w.value)
                    fake::violation("a submit waits a timeline value nothing has signalled (the fake GPU would hang)");
            }
            else if (fake::binaryPending.erase(w.semaphore) == 0 && fake::renderFinished.erase(w.semaphore) == 0)
                fake::violation("a submit waits a binary semaphore with no signal pending");
        }
        for (uint32_t i = 0; i < si[b].commandBufferInfoCount; ++i)
        {
            fake::Cmd& c = fake::cmds[si[b].pCommandBufferInfos[i].commandBuffer];
            if (c.recording) fake::violation("a command buffer submitted while recording");
            fake::cmdSubmit[si[b].pCommandBufferInfos[i].commandBuffer] = fake::submits.size();
            for (const fake::Ev& e : c.ev)
            {
                s.ev.push_back(e);
                if (e.kind == fake::Ev::Barrier)
                {
                    const auto it = fake::layout.find(e.image);
                    const VkImageLayout now = it == fake::layout.end() ? VK_IMAGE_LAYOUT_UNDEFINED : it->second;
                    if (e.from != VK_IMAGE_LAYOUT_UNDEFINED && e.from != now)
                        fake::violation("a barrier from a layout the image is not in");
                    fake::layout[e.image] = e.to;
                }
            }
        }
        for (uint32_t i = 0; i < si[b].signalSemaphoreInfoCount; ++i)
        {
            const VkSemaphoreSubmitInfo& sg = si[b].pSignalSemaphoreInfos[i];
            s.signals.push_back({sg.semaphore, sg.value});
            if (sg.semaphore == fake::holdTimeline)
                fake::held.push_back({sg.semaphore, sg.value});   // (the GPU has not got there yet)
            else if (fake::timeline.count(sg.semaphore))
            {
                if (sg.value <= fake::timeline[sg.semaphore]) fake::violation("a timeline signalled backwards");
                fake::timeline[sg.semaphore] = sg.value;
            }
            else
                fake::renderFinished.insert(sg.semaphore);
        }
        fake::submits.push_back(s);
    }
    return VK_SUCCESS;
}
VkResult vkqWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo* wi, uint64_t timeout, const char*, int)
{
    fake::Lock l(fake::m);
    if (fake::deviceLost) return VK_ERROR_DEVICE_LOST;   // (as the wrapper: no wait on a lost device)
    for (uint32_t i = 0; i < wi->semaphoreCount; ++i)
        fake::hostWaits.push_back({wi->pSemaphores[i], wi->pValues[i], timeout});
    for (uint32_t i = 0; i < wi->semaphoreCount; ++i)
        if (fake::timeline[wi->pSemaphores[i]] < wi->pValues[i])
            // (the fake GPU never runs late: a value not there never comes -- a bounded wait times out, an
            // infinite one is the watchdog's GPU hang)
            return timeout == UINT64_MAX ? fake::gpuHang("vkWaitSemaphores") : VK_TIMEOUT;
    // The CPU has seen each value: the first submit that signalled it, and its queue's before it, are complete.
    for (uint32_t i = 0; i < wi->semaphoreCount; ++i)
        for (size_t k = 0; k < fake::submits.size(); ++k)
        {
            bool reached = false;
            for (const auto& sg : fake::submits[k].signals)
                reached = reached || (sg.first == wi->pSemaphores[i] && sg.second >= wi->pValues[i]);
            if (!reached) continue;
            fake::completeThrough(k);
            break;
        }
    return VK_SUCCESS;
}
VkResult vkqQueueWaitIdle(VkQueue q, const char*, int)
{
    fake::presentQueueCall(q, "vkQueueWaitIdle");
    fake::Lock l(fake::m);
    const VkResult r = fake::take(fake::idleScript, VK_SUCCESS);
    if (r == VK_ERROR_DEVICE_LOST) fake::deviceLost = true;
    if (r != VK_SUCCESS) return r;   // (nothing seen complete)
    for (fake::Submit& s : fake::submits)
        if (s.queue == q) s.done = true;
    return VK_SUCCESS;
}
VkResult vkqGetSemaphoreCounterValue(VkDevice, VkSemaphore s, uint64_t* v, const char*, int)
{
    fake::Lock l(fake::m);
    if (fake::deviceLost) return VK_ERROR_DEVICE_LOST;
    *v = fake::timeline[s];
    return VK_SUCCESS;
}
void vkqCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* dep)
{
    fake::Lock l(fake::m);
    for (uint32_t i = 0; i < dep->imageMemoryBarrierCount; ++i)
    {
        const VkImageMemoryBarrier2& b = dep->pImageMemoryBarriers[i];
        fake::Ev e{fake::Ev::Barrier};
        e.image = b.image;
        e.from = b.oldLayout;
        e.to = b.newLayout;
        fake::cmds[cmd].ev.push_back(e);
    }
}
extern "C"
{
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat format, VkFormatProperties* p)
{
    *p = VkFormatProperties{};
    p->optimalTilingFeatures = (format == fake::noColor ? 0u : (VkFormatFeatureFlags)VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                               (format == fake::noStorage ? 0u : (VkFormatFeatureFlags)VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* p)
{ *p = fake::handle<VkCommandPool>(); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out)
{
    for (uint32_t i = 0; i < ai->commandBufferCount; ++i) out[i] = fake::handle<VkCommandBuffer>();
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo* ci, const VkAllocationCallbacks*, VkSemaphore* out)
{
    fake::Lock l(fake::m);
    *out = fake::handle<VkSemaphore>();
    const auto* t = static_cast<const VkSemaphoreTypeCreateInfo*>(ci->pNext);
    if (t && t->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) fake::timeline[*out] = t->initialValue;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice, VkSemaphore s, const VkAllocationCallbacks*)
{
    fake::Lock l(fake::m);
    fake::binaryPending.erase(s);
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo*)
{
    fake::Lock l(fake::m);
    if (fake::pending(cmd)) fake::violation("a command buffer begun while still pending (VUID-vkBeginCommandBuffer-commandBuffer-00049)");
    fake::cmds[cmd] = fake::Cmd{true, {}};
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cmd)
{
    fake::Lock l(fake::m);
    fake::cmds[cmd].recording = false;
    return fake::take(fake::endScript, VK_SUCCESS);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer cmd, VkImage, VkImageLayout, VkImage dst, VkImageLayout dstLayout,
                                          uint32_t, const VkImageCopy*)
{
    fake::Lock l(fake::m);
    fake::Ev e{fake::Ev::Copy};
    e.image = dst;
    e.to = dstLayout;
    fake::cmds[cmd].ev.push_back(e);
}
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer cmd, VkImage, VkImageLayout, VkImage dst, VkImageLayout dstLayout,
                                          uint32_t, const VkImageBlit*, VkFilter)
{
    fake::Lock l(fake::m);
    ++fake::blits;
    fake::Ev e{fake::Ev::Copy};   // (as a copy for the checks on the image: it writes the whole of it)
    e.image = dst;
    e.to = dstLayout;
    fake::cmds[cmd].ev.push_back(e);
}
VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphore(VkDevice, const VkSemaphoreSignalInfo* si)
{
    fake::Lock l(fake::m);
    if (si->value <= fake::timeline[si->semaphore]) fake::violation("a host signal backwards");
    for (const fake::Submit& s : fake::submits)
        for (const auto& sg : s.signals)
            if (!s.done && sg.first == si->semaphore && sg.second <= si->value)
                fake::violation("a host signal past a signal of it still pending (VUID-VkSemaphoreSignalInfo-value-03259)");
    fake::timeline[si->semaphore] = si->value;
    fake::hostSignals.push_back({si->semaphore, si->value});
    return VK_SUCCESS;
}
}

// ---- the swapchain: the main thread's wait for its frames in flight, the present thread's calls ----
bool VkSwapchain::waitFrameSlots(VkDeviceContext&)
{
    fake::Lock l(fake::m);
    if (fake::deviceLost) return false;
    ++fake::frameSlotWaits;
    if (!fake::frameFenceStuck) return true;
    fake::gpuHang("vkWaitForFences");   // (an infinite fence wait on a frame the GPU never finishes)
    return false;
}
bool VkSwapchain::acquireOne(VkDeviceContext&, VkSemaphore sem, uint64_t, uint32_t* outIndex, bool* outNeedsRecreate)
{
    fake::Lock l(fake::m);
    ++fake::acquireCalls;
    if (outNeedsRecreate) *outNeedsRecreate = false;
    const VkResult r = fake::take(fake::acquireScript, VK_SUCCESS);
    if (r == VK_ERROR_OUT_OF_DATE_KHR && outNeedsRecreate) *outNeedsRecreate = true;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;
    if (fake::binaryPending.count(sem)) fake::violation("an acquire semaphore reused with its signal still pending");
    int image = -1;
    for (size_t i = 0; i < fake::imageState.size() && image < 0; ++i)
        if (fake::imageState[i] == 0) image = (int)i;
    if (image < 0) return false;   // (none free: as a timeout)
    fake::imageState[image] = 1;
    fake::layout[mImages[image]] = VK_IMAGE_LAYOUT_UNDEFINED;
    fake::binaryPending.insert(sem);
    if (r == VK_SUBOPTIMAL_KHR && outNeedsRecreate) *outNeedsRecreate = true;
    *outIndex = (uint32_t)image;
    return true;
}
bool VkSwapchain::presentBatch(VkDeviceContext&, const BatchPresent* presents, uint32_t count, VkSemaphore, uint32_t metering,
                               uint32_t* outFeedback, bool* outNeedsRecreate)
{
    {
        // (Task 11b fix 1) the compositor holds it: the call blocks (outside the fakes' lock)
        std::unique_lock<std::mutex> h(fake::presentHoldM);
        ++fake::presentsHolding;
        fake::presentHoldCv.notify_all();
        fake::presentHoldCv.wait(h, [] { return !fake::holdPresents; });
        --fake::presentsHolding;
    }
    fake::Lock l(fake::m);
    if (outNeedsRecreate) *outNeedsRecreate = false;
    if (outFeedback) *outFeedback = ~0u;
    bool ok = true;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t image = presents[i].image;
        if (image >= fake::imageState.size() || fake::imageState[image] != 1)
            fake::violation("an image presented that was not acquired");
        if (fake::renderFinished.erase(mRenderFinished[image]) == 0)
            fake::violation("a present waits a render-finished no accepted submit signalled");
        if (fake::layout[mImages[image]] != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            fake::violation("an image presented in layout " + std::to_string((int)fake::layout[mImages[image]]));
        fake::presents.push_back({image, presents[i].gateValue, i == 0 ? metering : 0});
        fake::imageState[image] = 0;
        const VkResult r = fake::take(fake::presentScript, VK_SUCCESS);
        if (r != VK_SUCCESS) ok = false;
        // (as the real presentBatch: a lost surface or device is raised by the call that saw it)
        if (r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_DEVICE_LOST)
            vkDeviceLostRaise("vkQueuePresentKHR", r, nullptr, __FILE__, __LINE__);
        if ((r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) && outNeedsRecreate) *outNeedsRecreate = true;
    }
    return ok;
}

// ---- the encode's composite ----
bool VkDisplayEncode::recordFgComposite(VkCommandBuffer cmd, uint32_t, uint32_t, VkImageView, VkImageView, VkImageView target,
                                        VkExtent2D, VkImageView, VkBuffer, float, float, float)
{
    fake::Lock l(fake::m);
    if (!fake::compositeOk) return false;
    fake::Ev e{fake::Ev::Composite};
    const int i = fake::imageOfView(target);
    e.image = i >= 0 ? fake::swapImage((uint32_t)i) : VK_NULL_HANDLE;
    fake::cmds[cmd].ev.push_back(e);
    return true;
}

// ---- the generator (LLVKFrameGen's calls the present thread makes) ----
LLVKFrameGen& LLVKFrameGen::instance()
{
    static LLVKFrameGen s;
    return s;
}
bool LLVKFrameGen::evaluate(VkCommandBuffer cmd, uint32_t, const llfg::GenInputs& e, uint32_t index, uint32_t count)
{
    {
        std::unique_lock<std::mutex> h(fake::holdM);
        ++fake::evalsHeld;
        fake::holdCv.notify_all();
        fake::holdCv.wait(h, [] { return !fake::holdEval; });
        --fake::evalsHeld;
    }
    fake::Lock l(fake::m);
    const bool ok = fake::take(fake::evalScript, true);
    fake::Ev ev{fake::Ev::Evaluate};
    ev.index = index;
    ev.count = count;
    ev.reset = e.reset;
    fake::evaluates.push_back(ev);
    if (ok) fake::cmds[cmd].ev.push_back(ev);
    return ok;
}
uint32_t LLVKFrameGen::timestampQuery(VkCommandBuffer cmd)
{
    fake::Lock l(fake::m);
    fake::cmds[cmd].ev.push_back(fake::Ev{fake::Ev::Timestamp});
    return 0;
}
bool LLVKFrameGen::driverPacing() const { return false; }
uint64_t LLVKFrameGen::generatorBytes(VkExtent2D, VkFormat, VkFormat) const { return fake::genBytes; }
// #1544: a generator of a given kind, for the scratch format (LLVKFrameGen::generator()); never evaluated here
struct KindGen : LLVKFrameGenGenerator
{
    llfg::Generator k;
    explicit KindGen(llfg::Generator g) : k(g) {}
    llfg::Generator kind() const override { return k; }
    llfg::Block init(VkDeviceContext&, llfg::GeneratorCaps&, std::string&, std::string&) override { return llfg::Block::None; }
    bool fits(const llfg::GenSize&) const override { return false; }
    bool prepare(VkDeviceContext&, const llfg::GenSize&, std::string&) override { return false; }
    uint64_t imageBytes(const llfg::GenSize&) const override { return 0; }
    bool evaluate(VkCommandBuffer, uint32_t, const llfg::GenInputs&, uint32_t, uint32_t, std::string&) override { return false; }
    void release(VkDeviceContext&) override {}
};
KindGen gFsrKind(llfg::Generator::Fsr), gDlssKind(llfg::Generator::Dlss);
void LLVKFrameGen::outOfBandMarker(uint64_t frame, OobMarker marker)
{
    fake::Lock l(fake::m);
    if (frame) fake::markers.push_back({frame, (uint32_t)marker});
}
void LLVKFrameGen::releaseGates(uint64_t first, uint32_t count)
{
    fake::Lock l(fake::m);
    fake::gatesReleased.push_back({first, count});
}

// ---- the fixture ----
namespace
{
void holdEvaluates(bool on)
{
    std::lock_guard<std::mutex> h(fake::holdM);
    fake::holdEval = on;
    fake::holdCv.notify_all();
}
void holdPresents(bool on)
{
    std::lock_guard<std::mutex> h(fake::presentHoldM);
    fake::holdPresents = on;
    fake::presentHoldCv.notify_all();
}
bool presentHeld()    // the present thread waits in a present the compositor holds (bounded)
{
    std::unique_lock<std::mutex> h(fake::presentHoldM);
    return fake::presentHoldCv.wait_for(h, std::chrono::seconds(2), [] { return fake::presentsHolding > 0; });
}
bool evaluateHeld()   // the present thread waits in an evaluate (bounded)
{
    std::unique_lock<std::mutex> h(fake::holdM);
    return fake::holdCv.wait_for(h, std::chrono::seconds(2), [] { return fake::evalsHeld > 0; });
}

struct Fixture
{
    VkDeviceContext dev;
    VkSwapchain chain;
    VkDisplayEncode encode;
    LLVKFrameGenPresent present;
    uint64_t frame = 0;
    bool budgetKnown = true;
    uint64_t freeBytes = 1ull << 40, margin = 1ull << 20;

    explicit Fixture(uint32_t images = 4, bool prepareNow = true)
    {
        fake::reset();
        holdEvaluates(false);
        holdPresents(false);
        dev.mDevice = fake::handle<VkDevice>();
        dev.mQueue = fake::handle<VkQueue>();
        dev.mPresentQueue = fake::handle<VkQueue>();
        fake::presentQueue = dev.mPresentQueue;
        chain.mSwapchain = fake::handle<VkSwapchainKHR>();
        chain.mExtent = {64, 32};
        chain.mFormat = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT};
        for (uint32_t i = 0; i < images; ++i)
        {
            chain.mImages.push_back(fake::handle<VkImage>());
            chain.mImageViews.push_back(fake::handle<VkImageView>());
            chain.mRenderFinished.push_back(fake::handle<VkSemaphore>());
        }
        chain.mFgImages = images;
        chain.mFgMax = images - 1;
        chain.mPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;   // (as on Windows; the hidden-window tests take FIFO)
        fake::chain = &chain;
        fake::presenter = &present;
        fake::imageState.assign(images, 0);
        LLVKFrameGen& fg = LLVKFrameGen::instance();
        VkSemaphoreTypeCreateInfo t{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        t.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ci.pNext = &t;
        vkCreateSemaphore(dev.mDevice, &ci, nullptr, &fg.mFrameDone);
        vkCreateSemaphore(dev.mDevice, &ci, nullptr, &fg.mGeneratedDone);
        vkCreateSemaphore(dev.mDevice, &ci, nullptr, &fg.mGate);
        fg.mGeneratedDoneNext = 1;
        fg.mDirectReady = true;
        fg.mGen = nullptr;   // (#1544: no generator -- the scratch image in the world's format, as DLSS)
        if (prepareNow) CHECK(prepare());
    }
    ~Fixture()
    {
        holdEvaluates(false);
        holdPresents(false);
        present.shutdown(dev);
        fake::chain = nullptr;
        fake::presenter = nullptr;
    }
    LLVKFrameGenPresent::Budget budget()
    {
        return [this](uint64_t& f, uint64_t& mg)
        {
            f = freeBytes;
            mg = margin;
            return budgetKnown;
        };
    }
    bool prepare() { return present.prepare(dev, chain, encode, budget()); }
    // The main thread's bounds, shortened (after a prepare: it sets the slot wait's by the present mode).
    void fast()
    {
        present.mBounds.slotMs = 2;
        present.mBounds.heldMs = 40;
        present.mBounds.drainMs = 80;
    }
    // Waits (bounded) until the present thread counts as held.
    bool waitHeld()
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!present.held() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return present.held();
    }
    // Waits (bounded) until the present thread is drained (a drain only looks while the thread counts as held).
    bool waitDrained()
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!present.drain() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return present.drain();
    }
    // Waits (bounded) until a thread left behind has ended on its own (before the fixture's objects go).
    bool waitThreadGone()
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            {
                std::lock_guard<std::mutex> lock(present.mHandoff.mS->mutex);
                if (present.mHandoff.mS->exited) return true;
            }
            if (std::chrono::steady_clock::now() >= until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    // The chain rebuilt (VkSwapchain::recreate, the device idle): every image of it free again, every submit done.
    void rebuild()
    {
        fake::Lock l(fake::m);
        fake::imageState.assign(chain.mImages.size(), 0);
        fake::renderFinished.clear();   // (the chain's semaphores are made again)
        fake::completeAll();
    }
    LLVKFrameGenPresent::Job job(uint32_t generated, bool reset = false, uint64_t reflexFrame = 0)
    {
        LLVKFrameGenPresent::Job j;
        LLVKFrameGen& fg = LLVKFrameGen::instance();
        const uint32_t slot = present.nextSlot();
        j.done = ++frame;
        {
            fake::Lock l(fake::m);
            fake::timeline[fg.mFrameDone] = j.done;   // (the frame's own work, done on the fake GPU)
        }
        j.generated = generated;
        j.reflexFrame = reflexFrame;
        j.eval.color = llfg::imageRef(present.fgHudless(slot));
        j.eval.output = llfg::imageRef(present.fgScratch());
        j.eval.reset = reset;
        j.ui = present.fgUi(slot).view;
        return j;
    }
    // One real frame through the present thread: its slot, the job, then everything run.
    bool run(const LLVKFrameGenPresent::Job& j)
    {
        const uint32_t slot = present.nextSlot();
        if (present.waitSlot(slot) != VkPresentHandoff::SlotWait::Free) return false;
        const bool pushed = present.push(slot, j);
        present.drain();
        return pushed;
    }
    std::vector<LLVKFrameGenPresent::Result> results()
    {
        std::vector<LLVKFrameGenPresent::Result> r;
        present.takeResults(r);
        return r;
    }
};

// How often the release timeline was signalled at `value` (by a submit, or from the CPU).
uint32_t releasesFor(VkSemaphore released, uint64_t value)
{
    fake::Lock l(fake::m);
    uint32_t n = 0;
    for (const fake::Submit& s : fake::submits)
        for (const auto& sg : s.signals)
            if (sg.first == released && sg.second == value) ++n;
    for (const auto& h : fake::hostSignals)
        if (h.first == released && h.second == value) ++n;
    return n;
}
// Every job pushed so far (release values 1..last) released exactly once.
bool releasedOnce(const LLVKFrameGenPresent& p, uint64_t last)
{
    bool ok = true;
    for (uint64_t v = 1; v <= last; ++v)
        if (releasesFor(p.released(), v) != 1)
        {
            std::printf("  release %llu signalled %u times\n", (unsigned long long)v, releasesFor(p.released(), v));
            ok = false;
        }
    return ok;
}
bool noViolations()
{
    fake::Lock l(fake::m);
    for (const std::string& v : fake::violations) std::printf("  violation: %s\n", v.c_str());
    return fake::violations.empty();
}
std::vector<uint64_t> gatesOfPresents()
{
    fake::Lock l(fake::m);
    std::vector<uint64_t> g;
    for (const fake::Present& p : fake::presents) g.push_back(p.gate);
    return g;
}
size_t evaluatesSoFar()
{
    fake::Lock l(fake::m);
    return fake::evaluates.size();
}

void testShowsABatch()
{
    Fixture f;
    CHECK(f.present.slots() == 3);
    CHECK(f.run(f.job(3)));
    const auto r = f.results();
    CHECK(r.size() == 1 && r[0].shown == 4 && r[0].generated == 3 && !r[0].refused);
    CHECK(fake::presents.size() == 4);
    CHECK(releasedOnce(f.present, 1));
    CHECK(noViolations());
}

void testRebuildRequestDoesNotOutliveTheChain()
{
    // (review C1) the present thread's rebuild request belongs to the frame generation chain it was
    // made on: a rebuild to a normal chain, a new chain, a stop -- each forgets it, so the render loop
    // never rebuilds the swapchain every frame after HDR goes off mid-generation.
    Fixture f;
    fake::acquireScript = {VK_ERROR_OUT_OF_DATE_KHR};
    CHECK(f.run(f.job(1)));
    CHECK(f.present.needsRecreate());
    const size_t presents = fake::presents.size();
    CHECK(f.run(f.job(1)));                        // (dropped until the chain is rebuilt: only released)
    CHECK(fake::presents.size() == presents);
    CHECK(releasedOnce(f.present, 2));
    f.present.releaseChain();                      // the rebuild made a normal chain
    CHECK(!f.present.needsRecreate());
    f.rebuild();
    CHECK(f.prepare());                            // ... and later a frame generation chain again
    CHECK(!f.present.needsRecreate());
    fake::acquireScript = {VK_SUBOPTIMAL_KHR};
    CHECK(f.run(f.job(1)));
    CHECK(f.present.needsRecreate());
    f.rebuild();
    CHECK(f.prepare());                            // a frame generation chain rebuilt for it
    CHECK(!f.present.needsRecreate());
    fake::acquireScript = {VK_ERROR_OUT_OF_DATE_KHR};
    CHECK(f.run(f.job(1)));
    CHECK(f.present.needsRecreate());
    f.present.stop();                              // a lost device
    CHECK(!f.present.needsRecreate());
    CHECK(noViolations());
}

void testShortBatchKeepsItsTiming()
{
    // (review M4, re-graded) 4x, the second generated frame's image not in time: the frames shown keep
    // their evaluated places, the real frame its own (n spacings after the batch's end), n gate values
    // reserved and released.
    {
        Fixture f;
        fake::acquireScript = {VK_SUCCESS, VK_TIMEOUT};
        CHECK(f.run(f.job(3)));
        const auto g = gatesOfPresents();
        CHECK(evaluatesSoFar() == 1);              // (the image first: no evaluate for a frame never shown)
        CHECK(g.size() == 2);
        CHECK(fake::gatesReleased.size() == 1);    // (no pacer here: the values are released at once)
        if (g.size() == 2 && fake::gatesReleased.size() == 1)
        {
            const uint64_t first = fake::gatesReleased[0].first;
            CHECK(fake::gatesReleased[0].second == 3);   // n values, not the frames made
            CHECK(g[0] == 0);                            // the first generated frame: at once
            CHECK(g[1] == first + 3 - 1);                // the real frame: at its own time
        }
        CHECK(f.run(f.job(3)));                          // the next batch's values follow all n reserved
        CHECK(fake::gatesReleased.size() == 2 &&
              fake::gatesReleased[1].first == fake::gatesReleased[0].first + 3);
        CHECK(noViolations());
    }
    {
        // The second evaluate fails: its image still gets a picture (the real world with the interface) at
        // that frame's place; the real frame keeps its own.
        Fixture f;
        fake::evalScript = {true, false};
        CHECK(f.run(f.job(3)));
        const auto g = gatesOfPresents();
        CHECK(g.size() == 3 && fake::gatesReleased.size() == 1);
        if (g.size() == 3 && fake::gatesReleased.size() == 1)
        {
            const uint64_t first = fake::gatesReleased[0].first;
            CHECK(fake::gatesReleased[0].second == 3);
            CHECK(g[0] == 0 && g[1] == first && g[2] == first + 2);
        }
        const auto r = f.results();
        CHECK(r.size() == 1 && r[0].generated == 1);
        CHECK(noViolations());
    }
}

void testResetAfterAFrameTheGeneratorDidNotSee()
{
    // (review M5) the generator never saw the frame before: a dropped job, a job whose first image was not
    // in time (nothing evaluated), a job whose evaluates were recorded but refused, a frame without
    // generated frames -- the next evaluate resets. Frames that follow each other do not.
    Fixture f;
    CHECK(f.run(f.job(1)));
    CHECK(f.run(f.job(1)));
    CHECK(fake::evaluates.size() == 2 && !fake::evaluates.back().reset);
    LLVKFrameGenPresent::Job dropped = f.job(1);
    dropped.dropped = true;
    CHECK(f.run(dropped));
    CHECK(f.run(f.job(1)));
    CHECK(fake::evaluates.size() == 3 && fake::evaluates.back().reset);
    fake::acquireScript = {VK_TIMEOUT};
    CHECK(f.run(f.job(1)));
    CHECK(fake::evaluates.size() == 3);
    CHECK(f.run(f.job(1)));
    CHECK(fake::evaluates.size() == 4 && fake::evaluates.back().reset);
    fake::submitScript = {VK_ERROR_OUT_OF_DEVICE_MEMORY};   // (the generate submit: its evaluates never run)
    CHECK(f.run(f.job(1)));
    CHECK(f.run(f.job(1)));
    CHECK(fake::evaluates.size() == 6 && fake::evaluates.back().reset);
    CHECK(f.run(f.job(0)));
    CHECK(f.run(f.job(1)));
    CHECK(fake::evaluates.size() == 7 && fake::evaluates.back().reset);
    CHECK(f.run(f.job(1)));
    CHECK(fake::evaluates.size() == 8 && !fake::evaluates.back().reset);
    CHECK(releasedOnce(f.present, 11));
    CHECK(noViolations());
}

void testRefusedGenerateSubmitPresentsNothingUndefined()
{
    // (review M11) the generate submit refused for lack of memory: its images are taken to PRESENT_SRC by a
    // replacement with the same waits and signals (never presented in UNDEFINED); that refused too: they
    // are not presented at all, the chain is rebuilt, and nothing is raised as a lost device.
    {
        Fixture f;
        fake::submitScript = {VK_ERROR_OUT_OF_DEVICE_MEMORY};
        CHECK(f.run(f.job(2)));
        CHECK(fake::lostRaised == 0);
        CHECK(fake::presents.size() == 3);
        const auto r = f.results();
        CHECK(r.size() == 1 && r[0].shown == 1);   // (the generated images had no picture: only the real one counts)
        CHECK(!f.present.needsRecreate());
        CHECK(releasedOnce(f.present, 1));
        CHECK(noViolations());
    }
    {
        Fixture f;
        fake::submitScript = {VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY};
        CHECK(f.run(f.job(2)));
        CHECK(fake::lostRaised == 0);
        CHECK(fake::presents.empty());
        CHECK(f.present.needsRecreate());
        CHECK(releasedOnce(f.present, 1));
        // The rebuild: the acquire semaphores signalled and never waited are made again (the slot comes
        // round again: its semaphores are taken anew).
        f.rebuild();
        CHECK(f.prepare());
        for (uint32_t k = 0; k < f.present.slots(); ++k) CHECK(f.run(f.job(2)));
        CHECK(fake::presents.size() == 3 * f.present.slots());
        CHECK(releasedOnce(f.present, 1 + f.present.slots()));
        CHECK(noViolations());
    }
    {
        // The real frame's copy refused: the same replacement.
        Fixture f;
        fake::submitScript = {VK_SUCCESS, VK_ERROR_OUT_OF_DEVICE_MEMORY};
        CHECK(f.run(f.job(1)));
        CHECK(fake::presents.size() == 2 && fake::lostRaised == 0);
        CHECK(noViolations());
    }
    {
        // The release submit refused: released from the CPU once the present queue is idle.
        Fixture f;
        fake::submitScript = {VK_SUCCESS, VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY};
        CHECK(f.run(f.job(1)));
        CHECK(fake::hostSignals.size() == 1);
        CHECK(releasedOnce(f.present, 1));
        CHECK(noViolations());
    }
}

void testTwoRefusalsInOneJobNeverBeginAPendingBuffer()
{
    // (fix 2, item 1) the generate submit refused, its replacement accepted, then the real frame's submit
    // refused too: the real frame's replacement never begins a command buffer the first replacement left
    // pending on the GPU (VUID-vkBeginCommandBuffer-commandBuffer-00049).
    Fixture f;
    fake::submitScript = {VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_SUCCESS, VK_ERROR_OUT_OF_DEVICE_MEMORY};
    CHECK(f.run(f.job(2)));
    CHECK(fake::presents.size() == 3 && fake::lostRaised == 0);
    CHECK(releasedOnce(f.present, 1));
    CHECK(noViolations());
    // Every slot round again: a slot's buffers are begun again only once its last job was seen done.
    for (uint32_t k = 0; k < f.present.slots(); ++k) CHECK(f.run(f.job(2)));
    CHECK(releasedOnce(f.present, 1 + f.present.slots()));
    CHECK(noViolations());
}

bool logged(const char* text)
{
    fake::Lock l(fake::m);
    for (const std::string& line : fake::logs)
        if (line.find(text) != std::string::npos) return true;
    return false;
}

void testReleaseFallbackSignalsOnlyAfterAnIdleQueue()
{
    // (fix 2, item 2) the release submit refused and the present queue's wait idle refused too (no memory,
    // not a lost device): no host signal -- the previous job's lower release may still be pending
    // (VUID-VkSemaphoreSignalInfo-value-03259). The chain is rebuilt instead, and the release it owes is
    // signalled once the device is idle (prepare), exactly once.
    Fixture f;
    CHECK(f.run(f.job(1)));                        // release 1: submitted, not yet seen done
    fake::submitScript = {VK_SUCCESS, VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY};
    fake::idleScript = {VK_ERROR_OUT_OF_HOST_MEMORY};
    CHECK(f.run(f.job(1)));                        // (another slot: no wait for a previous job)
    CHECK(fake::hostSignals.empty());
    CHECK(f.present.needsRecreate() && fake::lostRaised == 0);
    CHECK(noViolations());
    // (fix 4) An owed release is not accepted work: a device wait meanwhile does not wait for it (it would
    // never come: a false GPU hang).
    CHECK(f.present.waitAccepted(f.dev.device()) == VK_SUCCESS && fake::lostRaised == 0);
    // (fix 4) The rebuild's device wait is made without the present queue and covers what that queue accepted;
    // the owed job's last commands have no accepted value. So prepare waits the present queue itself -- with
    // the thread stopped: no call on the queue from here while the present thread exists -- and only then
    // signals. That wait refused: prepare refuses untouched, and the next rebuild tries again.
    {
        fake::Lock l(fake::m);                     // (the rebuild's device wait: every queue but the present queue)
        for (fake::Submit& s : fake::submits)
            if (s.queue != f.dev.mPresentQueue) s.done = true;
        fake::imageState.assign(f.chain.mImages.size(), 0);
        fake::renderFinished.clear();
    }
    const uint32_t made = fake::imagesMade;
    fake::idleScript = {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_HOST_MEMORY};
    CHECK(!f.prepare() && !f.present.ready());
    CHECK(fake::hostSignals.empty() && fake::imagesMade == made);
    f.present.releaseChain();                      // (the render loop's answer to a refusal: a normal chain)
    CHECK(fake::hostSignals.empty() && fake::imagesMade == made);   // (its images kept: the queue not waited)
    CHECK(noViolations());
    CHECK(f.prepare() && f.present.ready() && f.present.mHandoff.running());
    CHECK(fake::hostSignals.size() == 1);
    CHECK(releasedOnce(f.present, 2));
    CHECK(noViolations());
    CHECK(f.run(f.job(1)));                        // and the chain goes on
    CHECK(releasedOnce(f.present, 3));
    // A rebuild with every release done signals nothing.
    f.rebuild();
    CHECK(f.prepare());
    CHECK(fake::hostSignals.size() == 1);
    CHECK(noViolations());
}

void testOwedReleaseWaitsForASuccessfulDeviceIdle()
{
    // (fix 3, item 3; fix 4) prepare() releases, remakes and host-signals on the premise that nothing is pending:
    // after a rebuild whose device wait failed without a lost device (no memory; VkSwapchain::deviceIdled()
    // false) it is refused untouched -- no image freed or made, no host signal of an owed release (a signal
    // past one still pending is not allowed), the thread kept -- and does all of it after a rebuild whose wait
    // did succeed.
    Fixture f;
    CHECK(f.run(f.job(1)));
    fake::submitScript = {VK_SUCCESS, VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY};
    fake::idleScript = {VK_ERROR_OUT_OF_HOST_MEMORY};
    CHECK(f.run(f.job(1)));                        // release 2 owed
    CHECK(fake::hostSignals.empty() && f.present.needsRecreate());
    const uint32_t made = fake::imagesMade;
    f.chain.mDeviceIdled = false;                  // the rebuild's device wait failed (nothing completed)
    f.chain.mExtent = {32, 32};                    // (another size: a prepare would remake every image)
    CHECK(!f.prepare());
    CHECK(!f.present.ready() && f.present.refusal() &&
          std::string(f.present.refusal()) == "the device could not be waited for");
    CHECK(fake::hostSignals.empty() && fake::imagesMade == made && f.present.mImagesExtent.width == 64);
    CHECK(f.present.mHandoff.running());
    CHECK(noViolations());
    f.chain.mDeviceIdled = true;                   // a later rebuild's did
    f.rebuild();
    CHECK(f.prepare() && f.present.ready() && f.present.mImagesExtent.width == 32);
    CHECK(fake::hostSignals.size() == 1);
    CHECK(releasedOnce(f.present, 2));
    CHECK(noViolations());
}

uint32_t loggedTimes(const char* text)
{
    fake::Lock l(fake::m);
    uint32_t n = 0;
    for (const std::string& line : fake::logs)
        if (line.find(text) != std::string::npos) ++n;
    return n;
}
// The device's waits made so far on `sem`: how many, and how many of them without a bound.
std::pair<uint32_t, uint32_t> hostWaitsOn(VkSemaphore sem)
{
    fake::Lock l(fake::m);
    uint32_t n = 0, infinite = 0;
    for (const fake::HostWait& w : fake::hostWaits)
        if (w.sem == sem)
        {
            ++n;
            if (w.timeout == UINT64_MAX) ++infinite;
        }
    return {n, infinite};
}

void testAHeldPresentIsNeverAHang()
{
    // (fix 4: a, d) THE STALL RULE. A present the compositor holds -- for 100 of the renderer's slot waits here,
    // on IMMEDIATE and on FIFO -- with the GPU fine: the renderer's wait for the slot only ever times out (Busy:
    // the frame is skipped, the main loop goes on), and each time, once the thread is held, the GPU is checked
    // through the device's own waits (the frames in flight, the present queue's accepted work) and found done.
    // No lost device, however long. It is logged once (then once a minute). A rebuild asked for meanwhile is
    // deferred (the drain only looks). When the compositor lets go -- on FIFO after the window was known hidden
    // and shows again -- the job goes on through its acquires, submits and release with the renderer still
    // polling: no verdict there either (the earlier rounds' false exit), the slot comes back, the deferred
    // rebuild is made, and the generator's history was never broken.
    for (const VkPresentModeKHR mode : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR})
    {
        const bool fifo = mode == VK_PRESENT_MODE_FIFO_KHR;
        Fixture f;
        f.chain.mPresentMode = mode;
        f.rebuild();
        CHECK(f.prepare());
        CHECK(f.present.mBounds.slotMs == llfg::slotWaitBoundMs(f.chain.fgPresentMode()));   // (never unbounded)
        f.fast();
        CHECK(f.run(f.job(1)));
        const size_t shownBefore = fake::presents.size();
        holdPresents(true);                                        // the compositor holds the next present
        uint32_t slot = f.present.nextSlot();
        const uint32_t heldSlot = slot;
        CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(2)));
        CHECK(presentHeld());
        for (int k = 0; k < 2; ++k)                                // two more frames handed over meanwhile
        {
            slot = f.present.nextSlot();
            CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
        }
        CHECK(f.present.nextSlot() == heldSlot);
        CHECK(f.waitHeld());
        if (fifo) f.present.setWindowHidden(true);                 // (the viewer learns the window is hidden)
        const uint32_t checks = fake::frameSlotWaits;
        const uint32_t generatedWaits = hostWaitsOn(LLVKFrameGen::instance().mGeneratedDone).second;
        for (int k = 0; k < 100; ++k) CHECK(f.present.waitSlot(heldSlot) == VkPresentHandoff::SlotWait::Busy);
        CHECK(fake::lostRaised == 0 && !fake::deviceLost);
        CHECK(fake::frameSlotWaits == checks + 100);               // the GPU checked on every skipped frame:
        CHECK(hostWaitsOn(LLVKFrameGen::instance().mGeneratedDone).second == generatedWaits + 100);   // its accepted batch
        CHECK(loggedTimes("not a GPU hang") == 1);
        {
            f.present.mBounds.drainMs = 3000;                      // (a bound it must not wait out)
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(!f.present.drain());                             // a rebuild asked for now: deferred, at once
            CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));
            f.present.mBounds.drainMs = 80;
        }
        CHECK(f.present.mHandoff.running() && !f.present.mHandoff.abandoned());
        if (fifo) f.present.setWindowHidden(false);                // the window shows again
        holdPresents(false);                                       // ... and the compositor lets the present go
        {
            VkPresentHandoff::SlotWait w = VkPresentHandoff::SlotWait::Busy;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while ((w = f.present.waitSlot(heldSlot)) == VkPresentHandoff::SlotWait::Busy &&
                   std::chrono::steady_clock::now() < until) {}
            CHECK(w == VkPresentHandoff::SlotWait::Free);          // (polled all the way through the job's end)
        }
        CHECK(fake::lostRaised == 0);
        CHECK(f.present.drain());                                  // the deferred rebuild: drained now
        CHECK(!f.present.held());
        CHECK(fake::presents.size() == shownBefore + 3 + 2 + 2);   // everything handed over went out
        f.rebuild();
        CHECK(f.prepare());
        f.fast();
        CHECK(f.run(f.job(1)));
        CHECK(!fake::evaluates.empty() && !fake::evaluates.back().reset);   // (no history lost to the hold)
        CHECK(releasedOnce(f.present, 5));
        CHECK(fake::lostRaised == 0);
        CHECK(noViolations());
    }
}

void testAGpuThatStoppedIsAGpuHang()
{
    // (fix 4: b1, b2) With the thread held the main thread no longer reaches its frame's own fence wait (the
    // frame is skipped first), so the GPU check on a skipped frame is where a real GPU hang surfaces: the
    // device's infinite waits on what the GPU was given. A renderer frame that never completes, or a submit the
    // present queue accepted that never completes: raised once, by the waiting call -- the ordinary GPU hang
    // (VK_TIMEOUT under that call's name), nothing of the present thread's own.
    {
        Fixture f;
        f.fast();
        holdEvaluates(true);                                       // (the thread inside a call, any call)
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(1)));
        CHECK(evaluateHeld());
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Busy);
        CHECK(f.waitHeld());
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Busy && fake::lostRaised == 0);   // (the GPU is fine)
        fake::frameFenceStuck = true;                              // the renderer's frame never completes
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Busy);
        CHECK(fake::lostRaised == 1 && fake::lostResult == VK_TIMEOUT && fake::lostCall == "vkWaitForFences");
        for (int k = 0; k < 5; ++k) (void)f.present.waitSlot(0);
        CHECK(fake::lostRaised == 1);                              // (once)
        CHECK(noViolations());
    }
    {
        Fixture f;
        f.fast();
        {
            fake::Lock l(fake::m);
            fake::holdTimeline = LLVKFrameGen::instance().mGeneratedDone;   // the GPU never finishes the batch
        }
        holdPresents(true);
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(1)));
        CHECK(presentHeld());                                      // (its generate submit accepted, then held)
        CHECK(f.waitHeld());
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Busy);
        CHECK(fake::lostRaised == 1 && fake::lostResult == VK_TIMEOUT && fake::lostCall == "vkWaitSemaphores");
        CHECK(fake::frameSlotWaits == 1);                          // (the frames were fine: checked first)
        for (int k = 0; k < 5; ++k) (void)f.present.waitSlot(0);
        CHECK(fake::lostRaised == 1);
        CHECK(noViolations());
    }
    {
        Fixture f;
        f.fast();
        {
            fake::Lock l(fake::m);
            fake::holdTimeline = f.present.released();             // the GPU never finishes a job's release
        }
        CHECK(f.run(f.job(1)));                                    // (its release submit accepted: pending)
        CHECK(fake::lostRaised == 0);
        holdEvaluates(true);
        const uint32_t slot = f.present.nextSlot();
        CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
        CHECK(evaluateHeld());
        CHECK(f.waitHeld());
        CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Busy);
        CHECK(fake::lostRaised == 1 && fake::lostResult == VK_TIMEOUT && fake::lostCall == "vkWaitSemaphores");
        CHECK(noViolations());
    }
}

void testOnlyAcceptedWorkIsWaited()
{
    // (fix 4: b3) What the GPU owes is what a queue ACCEPTED. A value merely issued never comes: the release of
    // a job held before its release submit (the main thread took the value at push), the generated-done value
    // of a batch whose submit was refused outright. Waiting for one would call a held present a GPU hang.
    {
        Fixture f;
        f.fast();
        holdPresents(true);
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(0)));
        CHECK(presentHeld());                                      // (the real frame's present: before the release)
        CHECK(f.present.releasedValue(0) == 1);                    // (issued)
        CHECK(f.waitHeld());
        for (int k = 0; k < 20; ++k) CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Busy);
        CHECK(fake::lostRaised == 0);
        CHECK(fake::frameSlotWaits == 20);                         // (checked each time ...)
        CHECK(hostWaitsOn(f.present.released()).second == 0);      // (... and the issued release never waited)
        CHECK(f.present.waitAccepted(f.dev.device()) == VK_SUCCESS);
        CHECK(noViolations());
    }
    {
        Fixture f;
        f.fast();
        fake::submitScript = {VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY};
        CHECK(f.run(f.job(1)));                                    // the generate submit refused outright:
        CHECK(LLVKFrameGen::instance().mGeneratedDoneNext > 1);    // its value issued, never signalled
        f.rebuild();
        CHECK(f.prepare());
        f.fast();
        holdEvaluates(true);
        const uint32_t slot = f.present.nextSlot();
        CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
        CHECK(evaluateHeld());
        CHECK(f.waitHeld());
        for (int k = 0; k < 20; ++k) CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Busy);
        CHECK(fake::lostRaised == 0);
        CHECK(hostWaitsOn(LLVKFrameGen::instance().mGeneratedDone).first == 0);
        CHECK(hostWaitsOn(f.present.released()).second == 20);     // (the first job's release: accepted, done)
        CHECK(noViolations());
    }
}

void testADeviceWaitNeverTouchesThePresentQueue()
{
    // (fix 4: f) The present queue is the present thread's own for as long as that thread exists. A device wait
    // from any other thread then waits what the queue accepted (waitAccepted: the device's infinite timeline
    // waits) and never calls the queue -- here while the thread is held inside a present, from the main thread
    // and from another one; the fake counts any such call as a violation.
    Fixture f(4, false);
    CHECK(!f.present.ownsPresentQueue());                          // (no thread yet: the device wait is the driver's)
    CHECK(f.prepare());
    f.fast();
    CHECK(f.present.ownsPresentQueue());
    holdPresents(true);
    CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(2)));
    CHECK(presentHeld());
    const VkSemaphore generated = LLVKFrameGen::instance().mGeneratedDone;
    {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(f.present.waitAccepted(f.dev.device()) == VK_SUCCESS);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    }
    CHECK(hostWaitsOn(generated) == std::make_pair(1u, 1u));       // (the accepted batch, without a bound)
    VkResult other = VK_ERROR_UNKNOWN;
    std::thread t([&] { other = f.present.waitAccepted(f.dev.device()); });
    t.join();
    CHECK(other == VK_SUCCESS && hostWaitsOn(generated) == std::make_pair(2u, 2u));
    CHECK(noViolations());
    holdPresents(false);
    CHECK(f.present.drain());
    CHECK(f.present.ownsPresentQueue());                           // (idle, but it exists)
    CHECK(f.present.waitAccepted(f.dev.device()) == VK_SUCCESS);
    CHECK(hostWaitsOn(f.present.released()).second == 1);          // (its release too, once submitted)
    f.present.shutdown(f.dev);                                     // (joined: the queue is anyone's again)
    CHECK(!f.present.ownsPresentQueue());
    CHECK(f.present.waitAccepted(f.dev.device()) == VK_SUCCESS);   // (its objects gone: nothing to wait)
    // Its objects made again (a new release timeline, from nothing): no value of the old one is waited on it.
    f.rebuild();
    CHECK(f.prepare());
    CHECK(f.present.waitAccepted(f.dev.device()) == VK_SUCCESS);
    CHECK(f.run(f.job(1)));
    CHECK(f.present.waitAccepted(f.dev.device()) == VK_SUCCESS);
    CHECK(fake::lostRaised == 0);
    CHECK(noViolations());
}

void testShutdownLeavesAHeldThreadBehind()
{
    // (fix 4: g) Quit, or a lost device, while the thread is inside a call that does not return: the wait for
    // it is bounded, then it is left behind -- no verdict, no lost device raised -- with everything it uses:
    // nothing is destroyed, nothing can be prepared again, the present queue stays its own, and the process is
    // told to end without static destructors (vkNoteThreadAbandoned, once).
    {
        Fixture f;
        f.fast();
        holdPresents(true);
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(1)));
        CHECK(presentHeld());
        const uint32_t made = fake::imagesMade;
        {
            const auto t0 = std::chrono::steady_clock::now();
            f.present.shutdown(f.dev);
            const auto took = std::chrono::steady_clock::now() - t0;
            CHECK(took >= std::chrono::milliseconds(70) && took < std::chrono::milliseconds(1500));
        }
        CHECK(fake::imagesMade == made && f.present.mPool && f.present.mReleased && f.present.mSlotCount);
        CHECK(f.present.mHandoff.abandoned() && f.present.ownsPresentQueue() && !f.present.ready());
        CHECK(fake::abandonedNoted == 1 && fake::lostRaised == 0);
        CHECK(logged("left behind"));
        CHECK(!f.present.drain());                                 // (never drained: nothing may be released)
        CHECK(!f.prepare() && fake::imagesMade == made);           // (nor remade)
        f.present.releaseChain();
        CHECK(fake::imagesMade == made);                           // (nor let go with a chain that goes)
        f.present.shutdown(f.dev);                                 // (again: still nothing)
        CHECK(!f.present.stop());
        CHECK(fake::imagesMade == made && f.present.mPool && fake::abandonedNoted == 1);
        CHECK(noViolations());                                     // (the present queue never waited from here)
        holdPresents(false);                                       // its call returns after all
        CHECK(f.waitThreadGone());
        CHECK(releasedOnce(f.present, 1));
        // Left behind is for good, though the thread has ended since: nothing is remade or let go.
        f.rebuild();
        f.chain.mExtent = {32, 32};
        CHECK(!f.prepare() && std::string(f.present.refusal()) == "the device could not be waited for");
        f.present.releaseChain();
        CHECK(fake::imagesMade == made && f.present.mImagesExtent.width == 64 && f.present.mHandoff.abandoned());
        CHECK(noViolations());
    }
    {
        Fixture f;
        f.fast();
        holdEvaluates(true);
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(1)));
        CHECK(evaluateHeld());
        const uint32_t made = fake::imagesMade;
        f.present.mBounds.heldMs = 3600000;                        // (not counted as held: the lost device decides)
        f.present.mBounds.drainMs = 3000;
        fake::deviceLost = true;                                   // a device lost elsewhere
        {
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(!f.present.drain());                             // (ended by the lost device, not its bound)
            CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));
        }
        f.present.mBounds.drainMs = 80;
        CHECK(!f.present.stop());
        CHECK(f.present.mHandoff.abandoned() && fake::abandonedNoted == 1 && fake::lostRaised == 0);
        f.present.shutdown(f.dev);
        CHECK(fake::imagesMade == made && f.present.mPool);
        holdEvaluates(false);
        CHECK(f.waitThreadGone());
    }
    {
        // A thread that ends within the bound is joined, on a lost device too, and nothing is destroyed there.
        Fixture f;
        CHECK(f.run(f.job(1)));
        const uint32_t made = fake::imagesMade;
        fake::deviceLost = true;
        CHECK(f.present.stop());
        CHECK(!f.present.mHandoff.abandoned() && !f.present.ownsPresentQueue() && fake::abandonedNoted == 0);
        f.present.shutdown(f.dev);
        CHECK(fake::imagesMade == made && f.present.mPool);
        fake::deviceLost = false;
    }
}

void testAHiddenWindowNeverFreezesTheRenderer()
{
    // (Task 11b fix 1, I1; fix 4) Wayland, FIFO: the compositor holds a present while the window cannot be seen,
    // until it shows again -- possibly before SDL reports the window hidden. The renderer (the main thread)
    // never waits for it without a bound: its wait for a slot comes back Busy after the bound (the frame
    // skipped, the main loop goes on pumping events), and a drain (a rebuild, shutdown) is one bounded try,
    // then -- the thread held -- only a look: false, so the caller defers. The present thread is told the
    // window is hidden before anything waits; when the held present returns while the window is still hidden,
    // the rest of its batch is not presented (a rebuild is asked for, made after the window shows) and the jobs
    // queued meanwhile only give their slots back. Shown again and rebuilt, frames go out as before. A present
    // held until the window shows again goes on from there.
    Fixture f;
    f.chain.mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    f.rebuild();
    CHECK(f.prepare());
    f.fast();
    CHECK(f.run(f.job(1)));
    const size_t shownBefore = fake::presents.size();
    CHECK(shownBefore == 2);
    holdPresents(true);                                            // the window goes away
    uint32_t slot = f.present.nextSlot();
    const uint32_t heldSlot = slot;
    CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(2)));
    CHECK(presentHeld());                                          // (inside its first present call)
    for (int k = 0; k < 2; ++k)                                    // two more frames handed over meanwhile
    {
        slot = f.present.nextSlot();
        CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
    }
    CHECK(f.present.nextSlot() == heldSlot);
    {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(f.present.waitSlot(heldSlot) == VkPresentHandoff::SlotWait::Busy);   // skipped, never frozen
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));
    }
    {
        // a drain before the window is known hidden: its one bounded try at most, false
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!f.present.drain());
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));
    }
    CHECK(f.waitHeld());
    f.present.setWindowHidden(true);                               // known hidden: the presenter is told
    {
        f.present.mBounds.drainMs = 3000;                          // (a bound it must not wait out)
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!f.present.drain());                                 // ... and a drain only looks
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));
        f.present.mBounds.drainMs = 80;
    }
    CHECK(f.present.waitSlot(heldSlot) == VkPresentHandoff::SlotWait::Busy);
    // a rebuild asked for now is not made under the held present (the render loop's rule)
    CHECK(llfg::deferRebuild(true, f.chain.fgPresentMode(), true, f.present.held()));
    // the compositor lets the present go while the window is still hidden (a suspend timeout)
    const size_t evals = evaluatesSoFar();
    const uint32_t acquires = fake::acquireCalls;
    holdPresents(false);
    {
        bool drained = false;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!(drained = f.present.drain()) && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(drained);                                            // (nothing held: drained)
    }
    CHECK(!f.present.held());
    CHECK(fake::presents.size() == shownBefore + 1);               // the held present only: the rest withheld
    CHECK(f.present.needsRecreate());                              // (its drawn images wait for the rebuild)
    CHECK(evaluatesSoFar() == evals && fake::acquireCalls == acquires);   // (the queued jobs: given back only)
    CHECK(releasedOnce(f.present, 4));
    CHECK(fake::lostRaised == 0);
    CHECK(noViolations());
    // shown again: rebuilt, and frames go out as before
    f.present.setWindowHidden(false);
    f.rebuild();
    CHECK(f.prepare());
    f.fast();
    CHECK(!f.present.needsRecreate());
    CHECK(f.run(f.job(1)));
    CHECK(fake::presents.size() == shownBefore + 1 + 2);
    CHECK(releasedOnce(f.present, 5));
    CHECK(noViolations());
    // The compositor holds a present until the window shows again: once it shows, the batch and the frames
    // queued behind it go out as ever: nothing withheld, no rebuild.
    const size_t before = fake::presents.size();
    holdPresents(true);
    slot = f.present.nextSlot();
    CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
    CHECK(presentHeld());
    slot = f.present.nextSlot();
    CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
    f.present.setWindowHidden(true);
    CHECK(f.waitHeld());
    CHECK(!f.present.drain());
    f.present.setWindowHidden(false);                              // shown: the compositor lets the present go
    holdPresents(false);
    {
        bool drained = false;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!(drained = f.present.drain()) && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(drained);
    }
    CHECK(fake::presents.size() == before + 4 && !f.present.needsRecreate());
    CHECK(releasedOnce(f.present, 7));
    CHECK(fake::lostRaised == 0);
    CHECK(noViolations());
}

void testAHiddenWindowTakesNoImage()
{
    // (Task 11b fix 1, I1) the present thread reads the hidden flag before every acquire and every present: a job
    // that starts while the window is hidden takes no image, generates and presents nothing and gives its slot
    // back; a window that hides in the middle of a job ends its acquires there, and the images already drawn
    // are not presented (a rebuild is asked for). On IMMEDIATE (no present waits on the compositor) a job
    // handed over goes out as ever.
    Fixture f;
    f.chain.mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    f.rebuild();
    CHECK(f.prepare());
    f.present.setWindowHidden(true);
    CHECK(f.run(f.job(2)));
    CHECK(fake::presents.empty() && fake::acquireCalls == 0 && evaluatesSoFar() == 0);
    CHECK(!f.present.needsRecreate());                             // (nothing taken: nothing to rebuild for)
    CHECK(releasedOnce(f.present, 1));
    f.present.setWindowHidden(false);
    holdEvaluates(true);
    uint32_t slot = f.present.nextSlot();
    CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(3)));
    CHECK(evaluateHeld());                                         // (the first generated frame: one image taken)
    f.present.setWindowHidden(true);
    holdEvaluates(false);
    CHECK(f.present.drain());
    CHECK(fake::acquireCalls == 1 && fake::presents.empty());
    CHECK(f.present.needsRecreate());
    CHECK(releasedOnce(f.present, 2));
    CHECK(noViolations());
    // IMMEDIATE: the presenter goes on (the render loop drops what it hands over while hidden)
    f.chain.mPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    f.rebuild();
    CHECK(f.prepare());
    CHECK(f.run(f.job(1)));
    CHECK(fake::presents.size() == 2);
    CHECK(releasedOnce(f.present, 3));
    CHECK(noViolations());
    f.present.setWindowHidden(false);
}

void testAcquireWaitsAtTheFirstUse()
{
    // (review M12) each submit waits its acquired images at the stage that first touches them, not
    // ALL_COMMANDS (the first evaluate overlaps the presentation engine's release).
    Fixture f;
    CHECK(f.run(f.job(2)));
    fake::Lock l(fake::m);
    uint32_t found = 0;
    for (const fake::Submit& s : fake::submits)
        for (const fake::Wait& w : s.waits)
            if (!fake::timeline.count(w.sem))
            {
                ++found;
                CHECK(w.stage != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
                CHECK((w.stage & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) != 0);
            }
    CHECK(found == 3);
}

void testReleasedExactlyOnceOnEveryPath()
{
    // Every pushed job releases its slot once, whatever happened to it.
    Fixture f;
    uint64_t jobs = 0;
    auto one = [&](uint32_t n) { CHECK(f.run(f.job(n))); ++jobs; };
    one(2);                                                     // shown
    fake::evalScript = {false};
    one(2);                                                     // the evaluate failed
    fake::acquireScript = {VK_TIMEOUT};
    one(2);                                                     // no image in time
    fake::acquireScript = {VK_SUCCESS, VK_SUCCESS, VK_TIMEOUT};
    one(2);                                                     // the real frame's image not in time
    fake::acquireScript = {VK_SUBOPTIMAL_KHR};
    one(2);                                                     // suboptimal: shown, then rebuilt
    one(2);                                                     // dropped until the rebuild
    f.rebuild();
    CHECK(f.prepare());
    fake::submitScript = {VK_ERROR_OUT_OF_DEVICE_MEMORY};
    one(2);                                                     // the replacement submit
    fake::submitScript = {VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY};
    one(2);                                                     // not even that
    f.rebuild();
    CHECK(f.prepare());
    one(0);                                                     // the real frame alone
    CHECK(releasedOnce(f.present, jobs));
    // Dropped after stop(): the job running finishes, the one queued behind it runs as dropped.
    holdEvaluates(true);
    uint32_t slot = f.present.nextSlot();
    CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
    CHECK(evaluateHeld());
    slot = f.present.nextSlot();
    CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
    jobs += 2;
    std::thread stopper([&] { f.present.stop(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    holdEvaluates(false);
    stopper.join();
    CHECK(releasedOnce(f.present, jobs));
    CHECK(noViolations());
}

void testPushRefusedKeepsTheSerial()
{
    // A push the handoff refuses (the slot still in flight) leaves the slot's release value and the serial
    // as they were: the renderer's next frame on that slot waits the right value.
    Fixture f;
    CHECK(f.run(f.job(1)));
    holdEvaluates(true);
    const uint32_t slot = f.present.nextSlot();
    CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
    CHECK(evaluateHeld());
    const uint64_t before = f.present.releasedValue(slot);
    CHECK(!f.present.push(slot, f.job(1)));                     // (no waitSlot: still in flight)
    CHECK(f.present.releasedValue(slot) == before);
    holdEvaluates(false);
    f.present.drain();
    CHECK(f.run(f.job(1)));
    CHECK(f.present.releasedValue((slot + 1) % f.present.slots()) == before + 1);
    CHECK(releasedOnce(f.present, 3));
    CHECK(noViolations());
}

void testPreviousJobOfTheSlotGuards()
{
    // A slot's command buffers, descriptor sets and semaphores are used again only once its previous job
    // is done on the GPU: not done in time -- the job shows nothing (and still releases its slot).
    // (2 slots: the slot comes round again on the third job)
    Fixture f(4, false);
    const uint64_t px = 64 * 32;
    f.margin = 0;
    f.freeBytes = 2 * px * (4 + 4 + 4 + 4 + 4) + px * 4;
    CHECK(f.prepare() && f.present.slots() == 2);
    {
        fake::Lock l(fake::m);
        fake::holdTimeline = f.present.released();
    }
    CHECK(f.run(f.job(1)));
    CHECK(f.run(f.job(1)));
    const size_t presents = fake::presents.size();
    CHECK(f.run(f.job(1)));                                     // slot 0 again, its last job not released
    CHECK(fake::presents.size() == presents);
    const auto r = f.results();
    CHECK(r.size() == 3 && r[2].refused && std::string(r[2].refused) == "the previous frame was not shown in time");
    fake::flushHeld();
    CHECK(releasedOnce(f.present, 3));
    CHECK(noViolations());
}

void testIdleLabelAtlasesGoBack()
{
    // (final review, 7e M7) Labels unused for a while (the setting off, snapshots...): each slot's label atlas is
    // retired when the render loop next owns that slot, and retired atlases are collected every frame, labels
    // or not -- never under the GPU: only once every frame that could have drawn or sampled the atlas is done
    // (frame done) and every job that could have sampled it has released its slot.
    Fixture f(4);
    LLVKFrameGen& fg = LLVKFrameGen::instance();
    const uint32_t slots = f.present.slots();
    const uint32_t base = fake::imagesMade;
    auto frameWithLabels = [&](bool labels)
    {
        const uint32_t slot = f.present.nextSlot();
        CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free);
        if (labels) CHECK(f.present.ensureLabels(slot, VkExtent2D{128, 64}, fg.mFrameDone, f.frame, f.budget()));
        LLVKFrameGenPresent::Job j = f.job(1);
        j.labels = labels;
        CHECK(f.present.push(slot, j));
        f.present.drain();
    };
    for (uint32_t k = 0; k < slots; ++k) frameWithLabels(true);
    CHECK(fake::imagesMade == base + slots);
    for (uint32_t k = 0; k < slots; ++k) CHECK(f.present.labelAtlas(k).view != VK_NULL_HANDLE);
    // not idle yet: tending a slot keeps its atlas
    f.present.tendLabels(f.present.nextSlot(), false, fg.mFrameDone, f.frame);
    CHECK(fake::imagesMade == base + slots && f.present.mRetiredLabels.empty());
    for (uint32_t k = 0; k < slots; ++k) CHECK(f.present.labelAtlas(k).view != VK_NULL_HANDLE);
    // idle, with a frame still in flight that may sample the atlas (frame done not reached): this slot's atlas
    // is retired -- no longer the slot's -- and not freed
    const uint32_t first = f.present.nextSlot();
    const uint64_t inFlight = f.frame + 2;                     // (a frame whose GPU work is not done)
    f.present.tendLabels(first, true, fg.mFrameDone, inFlight);
    CHECK(f.present.labelAtlas(first).view == VK_NULL_HANDLE);
    CHECK(f.present.mRetiredLabels.size() == 1 && fake::imagesMade == base + slots);
    for (uint32_t k = 0; k < slots; ++k)
        if (k != first) CHECK(f.present.labelAtlas(k).view != VK_NULL_HANDLE);   // (only the slot the caller owns)
    f.present.tendLabels(first, true, fg.mFrameDone, inFlight);                  // (again: nothing more to retire)
    CHECK(f.present.mRetiredLabels.size() == 1 && fake::imagesMade == base + slots);
    // a job that could sample it still holds its slot on the GPU (its release not signalled): still not freed,
    // though the frame is done now
    {
        fake::Lock l(fake::m);
        fake::holdTimeline = f.present.released();
    }
    frameWithLabels(false);                                    // (the next slot's job; its release is held)
    const uint32_t second = f.present.nextSlot();
    f.present.tendLabels(second, true, fg.mFrameDone, f.frame);   // retired after that job was pushed
    CHECK(f.present.labelAtlas(second).view == VK_NULL_HANDLE && f.present.mRetiredLabels.size() == 2);
    CHECK(fake::imagesMade == base + slots);                   // the first: its frame not done; the second: the job
    {
        fake::Lock l(fake::m);
        fake::timeline[fg.mFrameDone] = inFlight;              // the frame in flight is done
    }
    f.present.tendLabels(second, true, fg.mFrameDone, f.frame);
    CHECK(f.present.mRetiredLabels.size() == 1 && fake::imagesMade == base + slots - 1);   // the first went
    fake::flushHeld();                                         // the job's release reaches the GPU's end
    f.present.tendLabels(second, false, fg.mFrameDone, f.frame);   // collected on any frame, idle or not
    CHECK(f.present.mRetiredLabels.empty() && fake::imagesMade == base + slots - 2);
    // labels again: the slot gets a new atlas
    frameWithLabels(true);
    CHECK(fake::imagesMade == base + slots - 1);
    CHECK(noViolations());
}

// #1536 SDR frame generation: on an 8-bit SDR chain the generator's images (each slot's world, the scratch) are in
// the chain's own format -- the world renders straight into the slot's image, which therefore has a swapchain
// image's usage and is sampled -- and a device that cannot write that format as a storage image is refused before
// anything is made, with its own reason. HDR10 keeps its 10-bit images.
static size_t madeCount(VkFormat f, VkImageUsageFlags must)
{
    return (size_t)std::count_if(fake::made.begin(), fake::made.end(),
                                 [&](const fake::Made& m) { return m.format == f && (m.usage & must) == must; });
}

void testSdrSlots()
{
    const VkImageUsageFlags world = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (VkFormat sdr : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
    {
        Fixture f(4, false);
        f.chain.mFormat = {sdr, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        CHECK(f.prepare() && f.present.slots() == 3);
        CHECK(madeCount(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0) == 0);
        CHECK(madeCount(sdr, world) >= 3);                                      // each slot's world
        CHECK(madeCount(sdr, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 1);   // the scratch
        CHECK(f.present.fgHudless(0).format == sdr && f.present.fgScratch().format == sdr);
        CHECK(f.run(f.job(2)) && f.run(f.job(2)));
        CHECK(noViolations());
    }
    {
        Fixture f(4, false);   // (HDR10: the 10-bit images as before)
        CHECK(f.prepare());
        CHECK(f.present.fgHudless(0).format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
        CHECK(madeCount(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_USAGE_STORAGE_BIT) == 1);
    }
    {
        Fixture f(4, false);
        f.chain.mFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        fake::noStorage = VK_FORMAT_B8G8R8A8_UNORM;
        CHECK(!f.prepare() && !f.present.ready() && fake::imagesMade == 0);
        CHECK(f.present.refusal() &&
              std::string(f.present.refusal()) == "this GPU cannot write generated frames in the display's SDR format");
    }
    {
        // A chain that changes from HDR10 to SDR at the same size: new images in the new format.
        Fixture f;
        CHECK(f.present.fgHudless(0).format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
        f.chain.mFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        CHECK(f.prepare() && f.present.fgHudless(0).format == VK_FORMAT_B8G8R8A8_UNORM);
    }
}

// #1544 Task 6: what a second generator needs of the presenter.
void testSecondGenerator()
{
    const VkImageUsageFlags scratchUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    {
        // FSR on a BGRA8 SDR chain: its scratch image in RGBA8 (no BGRA8 storage format in its shaders), the world
        // in the chain's format; a generated frame that could not be composed is BLITTED (a copy would swap red
        // and blue), the real frame's own copy stays a copy.
        Fixture f(4, false);
        f.chain.mFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        LLVKFrameGen::instance().mGen = &gFsrKind;
        CHECK(f.prepare());
        CHECK(f.present.fgScratch().format == VK_FORMAT_R8G8B8A8_UNORM);
        CHECK(f.present.fgHudless(0).format == VK_FORMAT_B8G8R8A8_UNORM);
        CHECK(madeCount(VK_FORMAT_R8G8B8A8_UNORM, scratchUsage) == 1);
        fake::compositeOk = false;
        CHECK(f.run(f.job(1)));
        CHECK(fake::blits >= 1);
        CHECK(noViolations());
    }
    {
        // DLSS on the same chain: one format, copies only.
        Fixture f(4, false);
        f.chain.mFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        LLVKFrameGen::instance().mGen = &gDlssKind;
        CHECK(f.prepare() && f.present.fgScratch().format == VK_FORMAT_B8G8R8A8_UNORM);
        fake::compositeOk = false;
        CHECK(f.run(f.job(1)) && fake::blits == 0);
    }
    {
        // The storage refusal tests the image the generator writes: FSR's RGBA8 scratch, not the BGRA8 world.
        Fixture f(4, false);
        f.chain.mFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        LLVKFrameGen::instance().mGen = &gFsrKind;
        fake::noStorage = VK_FORMAT_B8G8R8A8_UNORM;
        CHECK(f.prepare());
        fake::noStorage = VK_FORMAT_R8G8B8A8_UNORM;
        Fixture g(4, false);
        g.chain.mFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        LLVKFrameGen::instance().mGen = &gFsrKind;
        fake::noStorage = VK_FORMAT_R8G8B8A8_UNORM;
        CHECK(!g.prepare() && fake::imagesMade == 0);
    }
    {
        // Another generator at the same size and format: the scratch image is made again in its format.
        Fixture f(4, false);
        f.chain.mFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        LLVKFrameGen::instance().mGen = &gDlssKind;
        CHECK(f.prepare() && f.present.fgScratch().format == VK_FORMAT_B8G8R8A8_UNORM);
        LLVKFrameGen::instance().mGen = &gFsrKind;
        CHECK(f.prepare() && f.present.fgScratch().format == VK_FORMAT_R8G8B8A8_UNORM);
    }
    {
        // The generator's own images join the one-set admission: each threshold moves by exactly their size.
        const uint64_t px = 64 * 32, perSlot = px * 20, shared = px * 4, gen = 12345;
        for (uint64_t extra : {uint64_t(0), uint64_t(-1)})
        {
            Fixture f(4, false);
            f.margin = 1000;
            fake::genBytes = gen;
            f.freeBytes = 3 * perSlot + shared + gen + f.margin + extra;
            CHECK(f.prepare() && f.present.slots() == (extra ? 2u : 3u));
        }
        Fixture f(4, false);
        f.margin = 1000;
        fake::genBytes = gen;
        f.freeBytes = 2 * perSlot + shared + gen + f.margin - 1;
        CHECK(!f.prepare() && fake::imagesMade == 0);
    }
    {
        // (final review) A live switch to a generator with its own images (DLSS -> FSR at the same scratch format:
        // HDR10) admits the set again with them: fewer slots, or refused -- never FSR's images unadmitted.
        const uint64_t px = 64 * 32, perSlot = px * 20, shared = px * 4, gen = 12345;
        Fixture f(4, false);
        f.margin = 1000;
        f.freeBytes = 3 * perSlot + shared + gen + f.margin - 1;   // (3 slots without the generator's, 2 with)
        LLVKFrameGen::instance().mGen = &gDlssKind;
        fake::genBytes = 0;
        CHECK(f.prepare() && f.present.slots() == 3);
        LLVKFrameGen::instance().mGen = &gFsrKind;
        fake::genBytes = gen;
        CHECK(f.prepare() && f.present.slots() == 2);
        f.freeBytes = 2 * perSlot + shared + gen + f.margin - 1;
        fake::genBytes = gen + 1;
        CHECK(!f.prepare());
    }
    {
        // imagesSerial: counted when the slots' images or the input copies are made again, not otherwise;
        // fillGenSize hands every slot's images and the scratch image.
        Fixture f;
        llfg::GenSize a;
        f.present.fillGenSize(a);
        CHECK(a.slots == 3 && a.imagesSerial != 0 && a.output.image == f.present.fgScratch().image);
        for (uint32_t k = 0; k < 3; ++k)
            CHECK(a.color[k].image == f.present.fgHudless(k).image && a.depth[k].image == f.present.fgDepth(k).image);
        CHECK(f.prepare());   // (same chain: kept)
        llfg::GenSize b;
        f.present.fillGenSize(b);
        CHECK(b.imagesSerial == a.imagesSerial);
        CHECK(f.present.prepareInputs({16, 8}, VK_FORMAT_R32_SFLOAT, {16, 8}, VK_FORMAT_R16G16_SFLOAT, 1, f.budget()));
        f.present.fillGenSize(b);
        CHECK(b.imagesSerial == a.imagesSerial + 1 && b.depth[0].extent.width == 16 && b.motion[2].image);
        f.chain.mExtent = {48, 32};
        CHECK(f.prepare());
        llfg::GenSize c;
        f.present.fillGenSize(c);
        CHECK(c.imagesSerial > b.imagesSerial);
    }
    LLVKFrameGen::instance().mGen = nullptr;
}

void testSlotsAdmittedAsOneSet()
{
    // (review I2 / I3) frame generation's images are admitted as one set: 3 slots when the set for 3 fits,
    // else 2, else none (refused, nothing allocated); an allocation that fails half way frees the lot.
    const uint64_t px = 64 * 32;
    const uint64_t perSlot = px * (4 + 4 + 4 + 4 + 4);   // backbuffer, hudless, ui (RGBA8), depth + motion (admitted)
    const uint64_t shared = px * 4;                       // the scratch image
    {
        Fixture f(4, false);
        f.margin = 1000;
        f.freeBytes = 3 * perSlot + shared + f.margin;
        CHECK(f.prepare() && f.present.slots() == 3);
        CHECK(fake::imagesMade == 3 * 3 + 1);             // (depth / motion: when the upscaler's size is known)
    }
    {
        Fixture f(4, false);
        f.margin = 1000;
        f.freeBytes = 3 * perSlot + shared + f.margin - 1;
        CHECK(f.prepare() && f.present.slots() == 2);
        CHECK(fake::imagesMade == 2 * 3 + 1);
        CHECK(f.run(f.job(2)) && f.run(f.job(2)) && f.run(f.job(2)));
        CHECK(releasedOnce(f.present, 3));
    }
    {
        Fixture f(4, false);
        f.margin = 1000;
        f.freeBytes = 2 * perSlot + shared + f.margin - 1;
        CHECK(!f.prepare() && f.present.slots() == 0 && !f.present.ready());
        CHECK(f.present.refusal() && std::string(f.present.refusal()) == "not enough video memory for frame generation's images");
        CHECK(fake::imagesMade == 0);
    }
    {
        Fixture f(4, false);
        f.budgetKnown = false;                            // (no budget extension: nothing to check against)
        CHECK(f.prepare() && f.present.slots() == 3);
    }
    {
        Fixture f(4, false);
        fake::failImageAfter = 5;                         // (the driver refuses one half way)
        CHECK(!f.prepare() && !f.present.ready());
        CHECK(fake::imagesMade == 0);
        CHECK(f.present.refusal() && std::string(f.present.refusal()) == "no memory for frame generation's images");
        fake::failImageAfter = ~0u;
        CHECK(f.prepare() && f.present.slots() == 3);     // (the next chain tries again)
    }
    {
        // A rebuild at another size with less room: 3 slots become 2, the thread starts again with 2.
        Fixture f;
        CHECK(f.present.slots() == 3);
        CHECK(f.run(f.job(1)) && f.run(f.job(1)));
        f.chain.mExtent = {32, 32};
        const uint64_t small = 32 * 32;
        f.margin = 0;
        f.freeBytes = 2 * small * (4 + 4 + 8 + 4 + 4) + small * 4;
        f.rebuild();
        CHECK(f.prepare() && f.present.slots() == 2 && f.present.nextSlot() == 0);
        CHECK(!f.present.push(2, f.job(1)));
        CHECK(f.run(f.job(1)) && f.run(f.job(1)) && f.run(f.job(1)));
        CHECK(releasedOnce(f.present, 5));
        CHECK(noViolations());
    }
}

void testInputCopiesAllOrNone()
{
    // The upscaler's depth / motion copies: every slot's at once, or none; a refusal is not tried again
    // for the same size, format and multiplier.
    Fixture f;
    const VkExtent2D d{40, 20}, m{40, 20};
    const VkFormat df = VK_FORMAT_R32_SFLOAT, mf = VK_FORMAT_R16G16_SFLOAT;
    const uint32_t made = fake::imagesMade;
    fake::failImageAfter = made + 3;                      // (the fourth copy fails)
    CHECK(!f.present.prepareInputs(d, df, m, mf, 3, f.budget()));
    CHECK(fake::imagesMade == made && !f.present.inputsFit(d, df, m, mf));
    CHECK(f.present.inputsRefused(d, df, m, mf, 3));
    fake::failImageAfter = ~0u;
    CHECK(!f.present.prepareInputs(d, df, m, mf, 3, f.budget()));   // (not again for the same)
    CHECK(fake::imagesMade == made);
    CHECK(!f.present.inputsRefused(d, df, m, mf, 2));
    CHECK(f.present.prepareInputs(d, df, m, mf, 2, f.budget()));    // (another multiplier: again)
    CHECK(fake::imagesMade == made + 2 * f.present.slots() && f.present.inputsFit(d, df, m, mf));
    // Not enough free memory for them: refused before anything is made.
    const VkExtent2D big{80, 40};
    f.margin = 0;
    f.freeBytes = 1;
    CHECK(!f.present.prepareInputs(big, df, big, mf, 2, f.budget()));
    CHECK(fake::imagesMade == made && !f.present.inputsFit(d, df, m, mf));
    CHECK(f.present.inputsRefused(big, df, big, mf, 2));
    f.rebuild();
    CHECK(f.prepare());                                   // (a chain rebuilt: tried again)
    CHECK(!f.present.inputsRefused(big, df, big, mf, 2));
    CHECK(noViolations());
}

void testInputCopyOverlayAdmission()
{
    Fixture f;
    const VkExtent2D extent{40, 20};
    const uint32_t made = fake::imagesMade;
    fake::noColor = VK_FORMAT_R32_SFLOAT;
    CHECK(!f.present.prepareInputs(extent, VK_FORMAT_R32_SFLOAT, extent, VK_FORMAT_R16G16_SFLOAT, 1, f.budget()));
    CHECK(fake::imagesMade == made);
    CHECK(f.present.inputsRefused(extent, VK_FORMAT_R32_SFLOAT, extent, VK_FORMAT_R16G16_SFLOAT, 1));
    fake::noColor = VK_FORMAT_UNDEFINED;
    CHECK(f.present.prepareInputs(extent, VK_FORMAT_R32_SFLOAT, extent, VK_FORMAT_R16G16_SFLOAT, 2, f.budget()));
    CHECK(fake::imagesMade == made + 2 * f.present.slots());
    for (size_t i = fake::made.size() - 2 * f.present.slots(); i < fake::made.size(); ++i)
        CHECK(fake::made[i].usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    CHECK(noViolations());
}

void testOutOfBandMarkers()
{
    // (Reflex) the present thread's work is out of band under the frame's id: its generate-and-draw submit
    // between RENDERSUBMIT markers, each present call between PRESENT markers. A frame Reflex does not
    // pace (id 0) sends none.
    Fixture f;
    CHECK(f.run(f.job(2, false, 7)));
    const std::vector<std::pair<uint64_t, uint32_t>> want{{7, 8}, {7, 9}, {7, 10}, {7, 11}, {7, 10}, {7, 11}, {7, 10}, {7, 11}};
    CHECK(fake::markers == want);
    CHECK(f.run(f.job(0, false, 8)));                     // the real frame alone: its present only
    CHECK(fake::markers.size() == 10 && fake::markers[8] == std::make_pair(uint64_t(8), 10u));
    CHECK(f.run(f.job(2)));
    CHECK(fake::markers.size() == 10);
}

void testTooFewImagesAsksForARebuild()
{
    Fixture f(3);
    CHECK(f.run(f.job(3)));                               // (3 generated + the real one need 4 images)
    CHECK(f.present.needsRecreate() && fake::presents.empty());
    const auto r = f.results();
    CHECK(r.size() == 1 && r[0].refused && std::string(r[0].refused) == "too few swapchain images");
    CHECK(releasedOnce(f.present, 1));
}

void testAStoppedThreadTakesNothingMore()
{
    // (fix 5, I2 a) Once stop() has begun the present thread takes no image and presents nothing more, as while
    // the window is hidden but on every chain: the call it is inside returns and the rest of its job only
    // gives the slot back. Left behind inside a present (quit while the compositor holds it)...
    {
        Fixture f;
        f.fast();
        holdPresents(true);
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(2)));
        CHECK(presentHeld());                                      // (two images drawn; inside the first present)
        const uint32_t acquires = fake::acquireCalls;
        CHECK(acquires == 2 && fake::presents.empty());
        CHECK(!f.present.stop());                                  // left behind
        holdPresents(false);                                       // its call returns after all
        CHECK(f.waitThreadGone());
        CHECK(fake::presents.size() == 1);                         // (that call's; not the batch's other frame)
        CHECK(fake::acquireCalls == acquires);                     // (no image for the real frame)
        CHECK(releasedOnce(f.present, 1));                         // (the slot is still given back)
        CHECK(fake::lostRaised == 0 && noViolations());
    }
    // ... or joined within the bound, out of the generator: the same.
    {
        Fixture f;
        f.fast();
        f.present.mBounds.drainMs = 5000;
        holdEvaluates(true);
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(2)));
        CHECK(evaluateHeld());                                     // (one image taken; inside its evaluate)
        const uint32_t acquires = fake::acquireCalls;
        CHECK(acquires == 1);
        bool ended = false;
        std::thread stopper([&] { ended = f.present.stop(); });
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (f.present.mHandoff.running() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(!f.present.mHandoff.running());                      // (the stop has begun)
        holdEvaluates(false);
        stopper.join();
        CHECK(ended && !f.present.mHandoff.abandoned());
        CHECK(fake::acquireCalls == acquires && fake::presents.empty());
        CHECK(releasedOnce(f.present, 1));
        CHECK(noViolations());
        // A thread started again (a new chain) shows frames as before.
        f.rebuild();
        CHECK(f.prepare());
        CHECK(f.run(f.job(1)));
        CHECK(fake::presents.size() == 2);
        CHECK(noViolations());
    }
}

void testALostDeviceAfterTheThreadWasLeftBehindIsQuiet()
{
    // (fix 5, I2 b) The call a thread left behind was inside may answer with a lost surface or device once the
    // exit goes on. That is no lost-device report, no marker for the next launch and no effects quarantine --
    // the session ended normally: the state is set (nothing more goes to the GPU) and nothing else.
    CHECK(vkLostDeviceReported(false) && !vkLostDeviceReported(true));
    {
        Fixture f;
        f.fast();
        holdPresents(true);
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(1)));
        CHECK(presentHeld());
        CHECK(!f.present.stop() && fake::abandonedNoted == 1);
        fake::presentScript = {VK_ERROR_SURFACE_LOST_KHR};
        holdPresents(false);
        CHECK(f.waitThreadGone());
        CHECK(fake::lostRaised == 0 && fake::lostQuiet == 1 && fake::deviceLost);
        fake::deviceLost = false;                                  // (the fixture's teardown)
    }
    {
        // With no thread left behind the same answer is the ordinary lost device.
        Fixture f;
        fake::presentScript = {VK_ERROR_SURFACE_LOST_KHR};
        CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(1)));
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!fake::deviceLost && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)f.present.stop();
        CHECK(fake::lostRaised == 1 && fake::lostQuiet == 0 && fake::lostCall == "vkQueuePresentKHR");
        fake::deviceLost = false;
    }
}

void testANormalChainHasNoPresentThread()
{
    // (fix 5, Minor 4) The chain in use is a normal one again (frame generation switched off): the idle thread
    // is stopped, so the present queue is the device's again and a device wait is the driver's own, as before
    // frame generation was used. (Minor 3) What the presenter holds says whether a rebuild whose device wait
    // failed must be asked again.
    Fixture f(4, false);
    CHECK(!f.present.holdsImages() && !f.present.ownsPresentQueue());
    CHECK(f.prepare());
    CHECK(f.present.holdsImages() && f.present.ownsPresentQueue());
    CHECK(f.run(f.job(1)));
    f.rebuild();
    f.present.releaseChain();
    CHECK(!f.present.holdsImages() && !f.present.ownsPresentQueue() && !f.present.mHandoff.abandoned());
    CHECK(fake::abandonedNoted == 0 && noViolations());
    // Switched on again: a new thread, frames as before.
    CHECK(f.prepare() && f.present.ownsPresentQueue() && f.present.holdsImages());
    CHECK(f.run(f.job(2)));
    CHECK(fake::presents.size() == 5);
    CHECK(releasedOnce(f.present, 2));
    CHECK(noViolations());
    // A thread that is not idle (the caller did not drain) is never stopped here.
    holdPresents(true);
    uint32_t slot = f.present.nextSlot();
    CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
    CHECK(presentHeld());
    f.present.releaseChain();
    CHECK(f.present.ownsPresentQueue() && !f.present.mHandoff.abandoned() && fake::abandonedNoted == 0);
    holdPresents(false);
    CHECK(f.waitDrained());
}

void testAnOwedReleaseIsSettledBeforeTheRebuild()
{
    // (fix 5, Minor 6) The last job's release never reached the queue (two refusals): no accepted value stands
    // for that job's last commands, so the rebuild's device wait -- made without the present queue -- would
    // destroy the old chain's images under them. The render loop settles it BEFORE the rebuild: the thread
    // stopped, the present queue waited (it is this thread's then), the value signalled.
    Fixture f;
    CHECK(f.run(f.job(1)));
    // A release the queue accepted and the GPU has not finished is not owed: nothing is stopped or signalled.
    fake::holdTimeline = f.present.released();
    CHECK(f.run(f.job(1)));
    CHECK(f.present.settleOwedRelease(f.dev));
    CHECK(fake::hostSignals.empty() && f.present.mHandoff.running() && noViolations());
    fake::flushHeld();
    fake::submitScript = {VK_SUCCESS, VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY};
    fake::idleScript = {VK_ERROR_OUT_OF_HOST_MEMORY};
    CHECK(f.run(f.job(1)));                        // release 3 owed
    CHECK(fake::hostSignals.empty() && f.present.needsRecreate());
    // The queue's wait refused: not settled -- the rebuild waits (nothing stopped for good, nothing signalled).
    fake::idleScript = {VK_ERROR_OUT_OF_HOST_MEMORY};
    CHECK(!f.present.settleOwedRelease(f.dev));
    CHECK(fake::hostSignals.empty() && noViolations());
    // Settled: before anything of the chain is rebuilt (the job's submits still count as pending until then).
    bool pendingBefore = false;
    {
        fake::Lock l(fake::m);
        for (const fake::Submit& s : fake::submits) pendingBefore = pendingBefore || (s.queue == f.dev.mPresentQueue && !s.done);
    }
    CHECK(pendingBefore);
    CHECK(f.present.settleOwedRelease(f.dev));
    CHECK(fake::hostSignals.size() == 1 && fake::hostSignals[0].second == 3);
    {
        fake::Lock l(fake::m);
        for (const fake::Submit& s : fake::submits) CHECK(s.queue != f.dev.mPresentQueue || s.done);
    }
    CHECK(!f.present.ownsPresentQueue());          // (stopped for the queue's wait: started again by prepare)
    CHECK(noViolations());
    f.rebuild();
    CHECK(f.prepare() && f.present.mHandoff.running());
    CHECK(fake::hostSignals.size() == 1);          // (not signalled a second time)
    CHECK(releasedOnce(f.present, 3));
    CHECK(f.run(f.job(1)));
    CHECK(releasedOnce(f.present, 4));
    CHECK(noViolations());
    // With a job still running nothing can be said to be owed: nothing is stopped, waited or signalled.
    holdPresents(true);
    const uint32_t slot = f.present.nextSlot();
    CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
    CHECK(presentHeld());
    CHECK(!f.present.settleOwedRelease(f.dev));
    CHECK(f.present.mHandoff.running() && !f.present.mHandoff.abandoned() && fake::hostSignals.size() == 1);
    CHECK(noViolations());
    holdPresents(false);
    CHECK(f.waitDrained());
    CHECK(releasedOnce(f.present, 5));
}

void testTheStatusNamesWhatTheThreadWaitsFor()
{
    // (fix 5, Minor 9) Held, the status names the call the thread is inside -- the display holding a frame, or
    // the generator -- and is neutral anywhere else.
    Fixture f;
    f.fast();
    CHECK(std::string(f.present.heldReason()) == llfg::heldText(llfg::HeldIn::Other));
    holdPresents(true);
    CHECK(f.present.waitSlot(0) == VkPresentHandoff::SlotWait::Free && f.present.push(0, f.job(1)));
    CHECK(presentHeld() && f.waitHeld());
    CHECK(std::string(f.present.heldReason()) == llfg::heldText(llfg::HeldIn::Present));
    holdPresents(false);
    CHECK(f.waitDrained());
    CHECK(std::string(f.present.heldReason()) == llfg::heldText(llfg::HeldIn::Other));
    holdEvaluates(true);
    uint32_t slot = f.present.nextSlot();
    CHECK(f.present.waitSlot(slot) == VkPresentHandoff::SlotWait::Free && f.present.push(slot, f.job(1)));
    CHECK(evaluateHeld() && f.waitHeld());
    CHECK(std::string(f.present.heldReason()) == llfg::heldText(llfg::HeldIn::Generator));
    holdEvaluates(false);
    CHECK(f.waitDrained());
    CHECK(std::string(llfg::heldText(llfg::HeldIn::Present)) != llfg::heldText(llfg::HeldIn::Generator));
    CHECK(std::string(llfg::heldText(llfg::HeldIn::Acquire)) != llfg::heldText(llfg::HeldIn::Other));
    CHECK(std::string(llfg::heldText(llfg::HeldIn::Other)) == "frame generation is waiting");
}

void testTheAddonLockSerialisesWithABound()
{
    // (fix 5, Minor 2) The add-on's lock: the present thread's frame generation evaluate and the main thread's
    // feature create / release never run inside the add-on at once. The main thread's side has a bound -- an
    // evaluate that does not return costs a refused create, never a frozen viewer -- and the present thread's
    // side waits (only ever behind a call of the main thread into the add-on).
    LLVKAddonLock lock;
    {
        LLVKAddonLock::Hold a = lock.tryFor(std::chrono::milliseconds(50));
        CHECK(bool(a));
    }
    std::atomic<int> inside{0}, overlaps{0};
    std::atomic<bool> evaluating{false}, release{false};
    std::thread presentThread([&]
    {
        LLVKAddonLock::Hold h = lock.wait();       // the evaluate
        CHECK(bool(h));
        if (inside.fetch_add(1) != 0) ++overlaps;
        evaluating.store(true);
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        inside.fetch_sub(1);
    });
    while (!evaluating.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {
        // The evaluate does not return: the main thread's try comes back, refused, after its bound.
        const auto t0 = std::chrono::steady_clock::now();
        LLVKAddonLock::Hold h = lock.tryFor(std::chrono::milliseconds(60));
        const auto took = std::chrono::steady_clock::now() - t0;
        CHECK(!h && took >= std::chrono::milliseconds(50) && took < std::chrono::milliseconds(3000));
    }
    release.store(true);
    presentThread.join();
    // Many of both: never two inside.
    std::thread evaluates([&]
    {
        for (int i = 0; i < 300; ++i)
        {
            LLVKAddonLock::Hold h = lock.wait();
            if (inside.fetch_add(1) != 0) ++overlaps;
            std::this_thread::yield();
            inside.fetch_sub(1);
        }
    });
    int got = 0;
    for (int i = 0; i < 300; ++i)
    {
        // (a long bound: each evaluate here is short, and a slow machine must not turn this into a timing test)
        LLVKAddonLock::Hold h = lock.tryFor(std::chrono::seconds(20));
        if (!h) continue;
        ++got;
        if (inside.fetch_add(1) != 0) ++overlaps;
        std::this_thread::yield();
        inside.fetch_sub(1);
    }
    evaluates.join();
    CHECK(overlaps.load() == 0 && got == 300);
}

} // namespace

int main()
{
    testShowsABatch();
    testRebuildRequestDoesNotOutliveTheChain();
    testShortBatchKeepsItsTiming();
    testResetAfterAFrameTheGeneratorDidNotSee();
    testRefusedGenerateSubmitPresentsNothingUndefined();
    testTwoRefusalsInOneJobNeverBeginAPendingBuffer();
    testReleaseFallbackSignalsOnlyAfterAnIdleQueue();
    testOwedReleaseWaitsForASuccessfulDeviceIdle();
    testAHeldPresentIsNeverAHang();
    testAGpuThatStoppedIsAGpuHang();
    testOnlyAcceptedWorkIsWaited();
    testADeviceWaitNeverTouchesThePresentQueue();
    testShutdownLeavesAHeldThreadBehind();
    testAStoppedThreadTakesNothingMore();
    testALostDeviceAfterTheThreadWasLeftBehindIsQuiet();
    testANormalChainHasNoPresentThread();
    testAnOwedReleaseIsSettledBeforeTheRebuild();
    testTheStatusNamesWhatTheThreadWaitsFor();
    testTheAddonLockSerialisesWithABound();
    testAHiddenWindowNeverFreezesTheRenderer();
    testAHiddenWindowTakesNoImage();
    testAcquireWaitsAtTheFirstUse();
    testReleasedExactlyOnceOnEveryPath();
    testPushRefusedKeepsTheSerial();
    testPreviousJobOfTheSlotGuards();
    testSlotsAdmittedAsOneSet();
    testSdrSlots();
    testSecondGenerator();
    testIdleLabelAtlasesGoBack();
    testInputCopiesAllOrNone();
    testInputCopyOverlayAdmission();
    testOutOfBandMarkers();
    testTooFewImagesAsksForARebuild();
    if (failures) std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
