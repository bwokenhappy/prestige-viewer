// The optional live HDR display surfaces, reused in graphics submission order: the world in
// FP16 linear nits, and the UI layer (#1451) -- an RGBA8 image the UI, HUDs and overlays are
// drawn into with the SDR pipelines (premultiplied sRGB-encoded colour, coverage in alpha),
// composited over the world at paper white by the display encode. Both exist only while HDR
// output is active.
#pragma once
#include "vk_device.h"

// One image layout transition (sync2, colour aspect, one mip and layer). The display images' barriers and
// frame generation's (llvkframegenpresent) use it.
inline void vkDisplayTransition(VkCommandBuffer cmd,VkImage image,VkImageLayout before,VkImageLayout after,
                                VkPipelineStageFlags2 source,VkAccessFlags2 sourceAccess,
                                VkPipelineStageFlags2 destination,VkAccessFlags2 destinationAccess) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask=source;barrier.srcAccessMask=sourceAccess;
    barrier.dstStageMask=destination;barrier.dstAccessMask=destinationAccess;
    barrier.oldLayout=before;barrier.newLayout=after;barrier.image=image;
    barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount=1;dependency.pImageMemoryBarriers=&barrier;
    vkCmdPipelineBarrier2(cmd,&dependency);
}

// A UI layer image (this frame's, or an offscreen snapshot's own): clear -> transparent black from
// any previous use, else back from sampling; sample -> SHADER_READ_ONLY for the encode.
void vkDisplayLayerBegin(VkCommandBuffer cmd,const GpuImage& layer,bool clear);
void vkDisplayLayerSample(VkCommandBuffer cmd,const GpuImage& layer);

class VkDisplayFrame {
public:
    VkDisplayFrame() = default;
    ~VkDisplayFrame() { destroy(); }
    VkDisplayFrame(const VkDisplayFrame&) = delete;
    VkDisplayFrame& operator=(const VkDisplayFrame&) = delete;
    // Caller drained prior graphics/compute uses (existing swapchain recreation
    // boundary). callerOwnedHdrBytes includes EVERY other extra HDR image/copy,
    // including renderer AA/FX surfaces and not-yet-retired allocations.
    bool prepare(VkDeviceContext& context,bool enabled,VkExtent2D extent,
                 uint64_t callerOwnedHdrBytes,uint64_t declaredBudget);
    bool validForExtent(VkExtent2D extent) const;
    void swap(VkDisplayFrame& other) noexcept;
    // Snapshot source (FP16 nits, linear-sized): the world with the UI layer composited, for
    // captures that include the HUD or the UI. Made on the first such shot, kept with the frame.
    // Allocation refusal leaves live HDR intact; the shot then has the world only.
    bool prepareCapture(uint64_t callerOwnedHdrBytes,uint64_t declaredBudget);
    // Frees the capture image; the caller has retired its uses (shots stopped a while ago).
    void releaseCapture();
    void destroy(); // prior uses retired, context still alive
    uint64_t residentBytes() const;
    const GpuImage& linear() const { return mLinear; }
    const GpuImage& uiLayer() const { return mUiLayer; }
    const GpuImage& capture() const { return mCapture; }
    void beginLinear(VkCommandBuffer cmd,const VkClearColorValue& clear) const;
    void sampleLinear(VkCommandBuffer cmd) const;
    void resumeLinear(VkCommandBuffer cmd) const;
    // clear: the frame's first use, transparent black; otherwise back from sampleUiLayer.
    void beginUiLayer(VkCommandBuffer cmd,bool clear) const;
    void sampleUiLayer(VkCommandBuffer cmd) const;
    void beginCapture(VkCommandBuffer cmd) const;
private:
    VkDeviceContext* mContext = nullptr;
    GpuImage mLinear{},mUiLayer{},mCapture{};
};
