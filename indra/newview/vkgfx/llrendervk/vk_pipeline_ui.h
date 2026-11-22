// LLRenderVK — UI pipeline builder.
//
// Separate from the vktest spike's generic createGraphicsPipeline() because the
// immediate-mode emulator needs per-batch control over PRIMITIVE TOPOLOGY and
// ALPHA BLEND state (the two axes that key our pipeline cache), plus a fixed
// interleaved pos/color/uv vertex layout. Dynamic rendering, no VkRenderPass.
#pragma once

#include "vk_common.h"

#include <string>

VkShaderModule loadUiShaderModule(VkDevice device, const std::string& spvPath);

struct UiPipelineDesc {
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    bool frameGenGuide = false; // R32 depth + RG16 motion, no fixed-function depth/blending
    int guideCompare = VK_COMPARE_OP_ALWAYS;

    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    bool blendEnable = true;
    VkBlendFactor srcFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor dstFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    // Alpha blend factors: false = the UI's coverage accumulation (uiAlphaFactors);
    // true = the colour factors, as GL's glBlendFunc does (offscreen render targets:
    // the avatar compositor's alpha passes depend on it). Never on the HDR UI layer.
    bool glAlphaBlend = false;
    // HDR UI layer (#1451): the target is the transparent RGBA8 layer composited over the
    // world as colour + world * (1 - alpha), not the swapchain the world is already in.
    bool uiLayer = false;

    // Color write mask (bit0=R,1=G,2=B,3=A) baked into the blend attachment.
    uint8_t colorWriteMask = 0xF;

    // Depth test/write/compare. Baked in; requires the target to have a depth
    // attachment when depthTest is true. depthFormat must be set in that case.
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    // Depth bias toward the camera (world overlays drawn ON surfaces -- property
    // lines hug the terrain, parcel highlights the ground): slope-scaled so the
    // overlay wins the LEQUAL test at grazing angles instead of z-fighting away.
    uint8_t depthBias = 0;   // 1: toward the camera, conventional depth; 2: reversed-Z

    // Dynamic line width (enables VK_DYNAMIC_STATE_LINE_WIDTH). Requires the
    // wideLines device feature if the width ever exceeds 1.0.
    bool dynamicLineWidth = false;

    // Interleaved vertex layout.
    uint32_t stride = 0;
    uint32_t posOffset = 0;
    uint32_t colOffset = 0;
    uint32_t uvOffset = 0;
};

VkPipeline createUiPipeline(VkDevice device, const UiPipelineDesc& d);

// #1133 VR in-world text (ui_world.vert / .frag): triangle list, LLRenderVK::WtVert layout
// (pos vec3, uv vec2, RGBA8 colour, uint flags, vec4 per-eye occlusion = `stride` bytes),
// premultiplied blend, no depth attachment, multiview over `viewMask`. VR only.
VkPipeline createUiWorldTextPipeline(VkDevice device, VkShaderModule vert, VkShaderModule frag,
                                     VkPipelineLayout layout, VkFormat colorFormat, uint32_t viewMask,
                                     uint32_t stride);

// The alpha channel's blend factors. glAlphaBlend: the colour factors (GL glBlendFunc).
// Otherwise coverage accumulates as src.a + dst.a * (1 - src.a) -- what the SDR swapchain has
// always had, and in the HDR UI layer exactly the share of the world a draw hides, so the
// composite (layer colour + world * (1 - layer alpha)) equals the same draws blended straight
// onto the world: alpha over, premultiplied over, and darkening (ZERO, 1 - SRC_ALPHA). An additive
// draw (destination factor ONE) hides nothing: in the layer it adds colour and no coverage.
// Destination-colour factors (multiply) have no exact layer form; no swapchain UI uses them.
inline void uiAlphaFactors(const UiPipelineDesc& d, VkBlendFactor& src, VkBlendFactor& dst)
{
    if (d.glAlphaBlend) { src = d.srcFactor; dst = d.dstFactor; return; }
    const bool additive = d.uiLayer && d.dstFactor == VK_BLEND_FACTOR_ONE;
    src = additive ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE;
    dst = additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
}

// A layer draw with blending off (BT_REPLACE) replaces everything under it, the world too:
// its coverage is 1 whatever the fragment's alpha (the fragment shaders' kUiLayerOpaque).
inline bool uiLayerOpaque(const UiPipelineDesc& d)
{
    return d.uiLayer && !d.blendEnable;
}
