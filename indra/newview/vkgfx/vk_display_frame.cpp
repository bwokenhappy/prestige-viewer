#include "vk_display_frame.h"
#include <limits>
#include <utility>

namespace {
constexpr VkImageUsageFlags displayUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
// The UI layer is drawn, cleared by its first rendering scope and sampled by the encode.
constexpr VkImageUsageFlags uiLayerUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
bool admitted(uint64_t owned,uint64_t others,uint64_t budget) {
    return owned && others<=budget && owned<=budget-others;
}
}
void VkDisplayFrame::destroy() {
    if (!mContext) return;
    if (mLinear.image) mContext->destroyImage(mLinear);
    if (mUiLayer.image) mContext->destroyImage(mUiLayer);
    if (mCapture.image) mContext->destroyImage(mCapture);
    mLinear={};mUiLayer={};mCapture={};mContext=nullptr;
}
uint64_t VkDisplayFrame::residentBytes() const {
    if (!mContext) return 0;
    constexpr uint64_t most=(std::numeric_limits<uint64_t>::max)();
    uint64_t total=0;
    auto add=[&](uint64_t bytes) { total=bytes>most-total ? most : total+bytes; };
    for (const uint64_t bytes:{mContext->allocBytes(mLinear),mContext->allocBytes(mUiLayer),
                               mContext->allocBytes(mCapture)})
        add(bytes);
    return total;
}
bool VkDisplayFrame::validForExtent(VkExtent2D extent) const {
    return mContext && mLinear.image && mLinear.view && mUiLayer.image && mUiLayer.view &&
        extent.width && extent.height &&
        mLinear.extent.width==extent.width && mLinear.extent.height==extent.height;
}
void VkDisplayFrame::swap(VkDisplayFrame& other) noexcept {
    using std::swap;
    swap(mContext,other.mContext);swap(mLinear,other.mLinear);swap(mUiLayer,other.mUiLayer);
    swap(mCapture,other.mCapture);
}
bool VkDisplayFrame::prepare(VkDeviceContext& context,bool enabled,VkExtent2D extent,
                             uint64_t others,uint64_t budget) {
    if (!enabled) { destroy();return true; }
    if (!extent.width || !extent.height || extent.width>context.limits().maxImageDimension2D ||
        extent.height>context.limits().maxImageDimension2D || !budget || budget>(512ull<<20)) {
        destroy();return false;
    }
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(context.physicalDevice(),VK_FORMAT_R16G16B16A16_SFLOAT,&format);
    constexpr VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures&needed)!=needed) { destroy();return false; }
    VkFormatProperties layerFormat{};
    vkGetPhysicalDeviceFormatProperties(context.physicalDevice(),VK_FORMAT_R8G8B8A8_UNORM,&layerFormat);
    constexpr VkFormatFeatureFlags layerNeeded=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((layerFormat.optimalTilingFeatures&layerNeeded)!=layerNeeded) { destroy();return false; }
    const uint64_t linearBytes=context.imageMemoryBytes(extent.width,extent.height,1,1,
        VK_FORMAT_R16G16B16A16_SFLOAT,displayUsage,false);
    const uint64_t layerBytes=context.imageMemoryBytes(extent.width,extent.height,1,1,
        VK_FORMAT_R8G8B8A8_UNORM,uiLayerUsage,false);
    const uint64_t required=linearBytes>(std::numeric_limits<uint64_t>::max)()-layerBytes ?
        0 : linearBytes+layerBytes;
    if (!admitted(required,others,budget)) { destroy();return false; }
    if (mContext==&context && mLinear.image && mUiLayer.image && mLinear.extent.width==extent.width &&
        mLinear.extent.height==extent.height && admitted(residentBytes(),others,budget)) return true;
    destroy();mContext=&context;
    VkMemScope category(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(&context);
    mLinear=context.createImage2D(extent.width,extent.height,VK_FORMAT_R16G16B16A16_SFLOAT,
                                  displayUsage,VK_IMAGE_ASPECT_COLOR_BIT);
    if (mLinear.image)
        mUiLayer=context.createImage2D(extent.width,extent.height,VK_FORMAT_R8G8B8A8_UNORM,
                                       uiLayerUsage,VK_IMAGE_ASPECT_COLOR_BIT);
    if (!mLinear.image || !mUiLayer.image || !admitted(residentBytes(),others,budget)) { destroy();return false; }
    return true;
}
void VkDisplayFrame::releaseCapture() {
    if (mContext && mCapture.image) mContext->destroyImage(mCapture);
    mCapture={};
}
bool VkDisplayFrame::prepareCapture(uint64_t others,uint64_t budget) {
    if (!mContext || !mLinear.image) return false;
    if (mCapture.image) return admitted(residentBytes(),others,budget);
    const auto size=mLinear.extent;
    const uint64_t required=mContext->imageMemoryBytes(size.width,size.height,1,1,
        VK_FORMAT_R16G16B16A16_SFLOAT,displayUsage,false);
    const uint64_t live=residentBytes();
    if (others>budget || live>budget-others || !admitted(required,others+live,budget)) return false;
    VkMemScope category(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(mContext);
    mCapture=mContext->createImage2D(size.width,size.height,VK_FORMAT_R16G16B16A16_SFLOAT,
                                     displayUsage,VK_IMAGE_ASPECT_COLOR_BIT);
    if (!mCapture.image) return false;
    if (!admitted(residentBytes(),others,budget)) { mContext->destroyImage(mCapture);mCapture={};return false; }
    return true;
}
void VkDisplayFrame::beginLinear(VkCommandBuffer cmd,const VkClearColorValue& clear) const {
    // UNDEFINED discards contents, but ALL_COMMANDS memory dependencies still order
    // the preceding frame's sampling/transfer reads before this same-queue overwrite.
    vkDisplayTransition(cmd,mLinear.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(cmd,mLinear.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
    vkDisplayTransition(cmd,mLinear.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_CLEAR_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}
void VkDisplayFrame::sampleLinear(VkCommandBuffer cmd) const {
    vkDisplayTransition(cmd,mLinear.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
void VkDisplayFrame::resumeLinear(VkCommandBuffer cmd) const {
    vkDisplayTransition(cmd,mLinear.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}
void VkDisplayFrame::beginUiLayer(VkCommandBuffer cmd,bool clear) const { vkDisplayLayerBegin(cmd,mUiLayer,clear); }
void VkDisplayFrame::sampleUiLayer(VkCommandBuffer cmd) const { vkDisplayLayerSample(cmd,mUiLayer); }
void vkDisplayLayerBegin(VkCommandBuffer cmd,const GpuImage& layer,bool clear) {
    if (!clear) {
        vkDisplayTransition(cmd,layer.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        return;
    }
    // UNDEFINED discards last frame's layer; ALL_COMMANDS orders its encode read first. The
    // clear is an empty rendering scope (no transfer usage), then made visible to every
    // later scope that LOADs the layer (overlays, HUD, UI).
    vkDisplayTransition(cmd,layer.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView=layer.view;colour.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;colour.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    colour.clearValue.color={{0.f,0.f,0.f,0.f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea={{0,0},{layer.extent.width,layer.extent.height}};
    rendering.layerCount=1;rendering.colorAttachmentCount=1;rendering.pColorAttachments=&colour;
    vkCmdBeginRendering(cmd,&rendering);
    vkCmdEndRendering(cmd);
    VkMemoryBarrier2 cleared{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    cleared.srcStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    cleared.srcAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    cleared.dstStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    cleared.dstAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount=1;dependency.pMemoryBarriers=&cleared;
    vkCmdPipelineBarrier2(cmd,&dependency);
}
void vkDisplayLayerSample(VkCommandBuffer cmd,const GpuImage& layer) {
    vkDisplayTransition(cmd,layer.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
void VkDisplayFrame::beginCapture(VkCommandBuffer cmd) const {
    // UNDEFINED: fully rewritten; ALL_COMMANDS orders the previous shot's copy first.
    vkDisplayTransition(cmd,mCapture.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}
