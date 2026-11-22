// Optional current-renderer final display transfer. No ownership of world/UI images.
#pragma once
#include "vk_display_hdr.h"
#include "vk_device.h"
#include <array>

class VkDisplayEncode {
public:
    // CompleteDisplay: the world (linear nits) with the UI layer composited, encoded for the
    // HDR swapchain. CompositeNits: the same composite written as FP16 nits (an HDR snapshot's
    // source when it includes the HUD or UI). WorldBeforeUi: the SDR world capture.
    enum class Content : uint8_t { CompleteDisplay, WorldBeforeUi, CompositeNits };
    // Hdr: the HDR encode (HDR10 / scRGB target). SdrCapture: the SDR world capture (B8G8R8A8 target).
    // SdrLayer (#1536): SDR frame generation's composite -- the real picture (recordLayerComposite) and
    // every generated one (recordFgComposite), encoding 5; no HDR encode (record refuses).
    enum class Mode : uint8_t { Hdr, SdrCapture, SdrLayer };
    VkDisplayEncode() = default;
    VkDisplayEncode(const VkDisplayEncode&) = delete;
    VkDisplayEncode& operator=(const VkDisplayEncode&) = delete;
    // All prior uses retired before prepare/destroy. Never called per frame.
    // Disabled preparation creates no Vulkan objects. Shader binaries are supplied
    // by the current renderer's existing embedded-shader build, not render2.
    // sdrLayer (#1536): an SDR target (B8G8R8A8 / R8G8B8A8 UNORM, sRGB-nonlinear) in the SDR-layer mode.
    bool prepare(VkDeviceContext& ctx, bool enabled, VkSurfaceFormatKHR target,
                 std::span<const uint32_t> vertex, std::span<const uint32_t> fragment, bool sdrLayer = false);
    Mode mode() const { return mMode; }
    bool validForTarget(VkSurfaceFormatKHR target) const;
    void swap(VkDisplayEncode& other) noexcept;
    void destroy();
    // Caller has retired THIS descriptor slot (same frame-slot fence as UI replay).
    // linearNits: the world, opaque Rec.709 LINEAR NITS in SHADER_READ_ONLY, not SDR/PQ bytes.
    // uiLayer: the HDR UI layer (RGBA8, premultiplied sRGB-encoded colour + coverage) in
    // SHADER_READ_ONLY, composited at paper white; the SDR world capture ignores it.
    // sdrWorld (#1451 G5): this frame's world went through the SDR post chain (an SDR snapshot
    // frame): it is shown at paper white, and CompositeNits then writes the SDR-encoded composite.
    // SDR output requires WorldBeforeUi: tone-map the world, then replay UI/HUD
    // into SDR. A composed HDR UI frame must not be tone-mapped as scene colour.
    // Caller owns barriers; all image bytes, including fenced copies and other HDR
    // intermediates, must fit declaredBudget. False records no commands.
    bool record(VkCommandBuffer cmd, uint32_t slot, VkImageView linearNits, VkImageView uiLayer,
                VkImageView target, VkExtent2D extent, vkdisplay::Request request, Content content,
                uint64_t allHdrImageBytes, uint64_t declaredBudget, bool dither = true, bool sdrWorld = false,
                VkImageView fgHudless = VK_NULL_HANDLE, VkImageView fgUi = VK_NULL_HANDLE,
                VkImageView labelAtlas = VK_NULL_HANDLE, const GpuBuffer* labelBuffer = nullptr);
    // labelAtlas + labelBuffer (#1517 Task 7e, HDR10 CompleteDisplay only): the slot's
    // moving labels (name tags, bubbles, hover text: SHADER_READ_ONLY, uiLayer's encoding) and their list
    // (llvkframegenlabels.h), drawn between the world and the interface at this real frame's moment;
    // fgHudless and fgUi never contain them. Null: none (the labels are in uiLayer).
    // Frame generation (#1517): CompleteDisplay on HDR10 can also write the world alone
    // (fgHudless, A2B10G10R10, the output's encoding) and a copy of uiLayer's texels (fgUi, RGBA8:
    // the interface for the generated frames, nothing of the world in it) in the same pass; both
    // COLOR_ATTACHMENT_OPTIMAL, caller's barriers. False when this encode has no such pipeline (not HDR10).
    bool frameGenOutputs() const { return mFgPipeline != VK_NULL_HANDLE; }
    // Frame generation (#1517): one picture of a batch -- a world frame already in the output's
    // encoding (a generated one, or the real one) with the UI layer (uiLayer: a frame's fgUi) composed
    // over it as the real frame's is -- in nits, at paperWhiteNits, clamped to peakNits (the encode's own,
    // sanitized: always needed) -- into target (COLOR_ATTACHMENT_OPTIMAL). Both inputs in GENERAL. slot: the
    // present thread's slot; index: the composite's place in that slot's job (< kMaxFgComposites). Each
    // (slot, index) has its own descriptor set, used again only once that slot's last job is done on the
    // GPU (the present thread's guard). False records nothing.
    // Labels (#1517 Task 7e): labelAtlas (SHADER_READ_ONLY) + labelBuffer, the slot's, drawn between the
    // world and the interface where they were at `phase` (k / (n + 1)). Null: none.
    bool recordFgComposite(VkCommandBuffer cmd, uint32_t slot, uint32_t index, VkImageView worldEncoded,
                           VkImageView uiLayer, VkImageView target, VkExtent2D extent,
                           VkImageView labelAtlas, VkBuffer labelBuffer, float phase,
                           float paperWhiteNits, float peakNits);
    // The composites' descriptor sets exist (optional: without them no frame is generated).
    bool frameGenComposites() const { return mPipeline != VK_NULL_HANDLE && mFgCompSets[0] != VK_NULL_HANDLE; }
    // Everything frame generation needs from this encode: HDR10, its three-output pipeline and the composites;
    // the SDR layer (#1536), the composites (the real picture is one of them).
    bool frameGenReady() const
    {
        return mMode == Mode::SdrLayer ? frameGenComposites()
                                       : mMode == Mode::Hdr && frameGenOutputs() && frameGenComposites();
    }
    // #1536 SDR frame generation: the real picture -- world (the slot's SDR world image) with the labels
    // (labelAtlas + labelBuffer, at the real frame's moment; null: none) and the UI layer over it, encoding 5,
    // into target (the slot's backbuffer, COLOR_ATTACHMENT_OPTIMAL). Inputs in SHADER_READ_ONLY; caller's
    // barriers; descriptor set `slot` (retired by the caller, as for record). Only in the SDR-layer mode.
    // capture: a snapshot's composite earlier in the same command buffer (its own set per slot, so the frame's
    // final composite never rewrites a set the buffer already bound).
    bool recordLayerComposite(VkCommandBuffer cmd, uint32_t slot, VkImageView world, VkImageView uiLayer,
                              VkImageView target, VkExtent2D extent, VkImageView labelAtlas,
                              const GpuBuffer* labelBuffer, bool capture = false);
    // The generator's colour input and output format for a chain of this target: HDR10's A2B10G10R10 (the
    // encode writes it), or (#1536 SDR) the chain's own format -- the SDR world is rendered straight into it.
    static VkFormat fgHudlessFormat(VkSurfaceFormatKHR target)
    {
        return vkdisplay::encodingOf(target) == vkdisplay::Encoding::Hdr10 ? kFgHudlessFormat : target.format;
    }
    static constexpr uint32_t kSlots = 3;             // descriptor slots (frames in flight it serves)
    static constexpr uint32_t kMaxFgComposites = 6;   // per slot: 5 generated frames + 1 spare
    static constexpr VkFormat kFgHudlessFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;   // (HDR10's; fgHudlessFormat)
    // The interface layer for the generated frames is the UI layer itself, copied per slot (the UI layer
    // is redrawn by the next frame while the present thread still shows this one): its format, texel for
    // texel. Nothing of the world is in it, so a generated frame composes it over its own world in nits as
    // the real frame does (no trace of the real frame's world under a translucent window), and it costs
    // half of what an FP16 layer did.
    static constexpr VkFormat kFgUiFormat = VK_FORMAT_R8G8B8A8_UNORM;
    // The per-slot descriptor cache compares view handles; a destroyed view's handle value can
    // come back for a new one. Callers that destroy an input (a new frame's images, an offscreen
    // shot's transient views) invalidate it, so the next record rewrites every set it uses.
    void invalidateInputs() { mInputs = {}; mCaptureInputs = {}; mLabelSerial = {}; mCaptureLabelSerial = {}; }
private:
    // A slot's descriptor set for these inputs (SHADER_READ_ONLY; rewritten only when they changed): the frame's
    // (mSets) or a capture's (mCaptureSets), each with its own cache.
    void updateSlotSet(uint32_t slot, const std::array<VkImageView,3>& inputs, const GpuBuffer& labels,
                       bool capture = false);
    // The one composite draw (encodings 4 and 5, with 0x200 for labels): the real SDR-layer picture and every
    // generated picture go through it.
    void drawComposite(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t encoding, float phase,
                       float paperWhiteNits, float peakNits, VkImageView target, VkExtent2D extent);
    VkDeviceContext* mContext = nullptr;
    Mode mMode = Mode::Hdr;
    VkPipeline mPipeline = VK_NULL_HANDLE;
    VkPipeline mCompositePipeline = VK_NULL_HANDLE;   // CompositeNits: FP16 target (HDR only)
    VkPipeline mFgPipeline = VK_NULL_HANDLE;          // frame generation: + hudless + UI (HDR10 only)
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool mPool = VK_NULL_HANDLE;
    VkSampler mSampler = VK_NULL_HANDLE;
    std::array<VkDescriptorSet,kSlots> mSets{};
    std::array<VkDescriptorSet,kSlots> mCaptureSets{};   // #1536: a snapshot's SDR-layer composite (per frame slot)
    std::array<VkDescriptorSet,kSlots*kMaxFgComposites> mFgCompSets{};   // [slot * kMaxFgComposites + index]
    static constexpr uint32_t kBindings = 4;   // world, UI layer, label atlas, label list (a storage buffer)
    static constexpr uint32_t kImageBindings = 3;
    std::array<std::array<VkImageView,kImageBindings>,kSlots> mInputs{};   // per slot: world, UI layer, label atlas
    std::array<uint64_t,kSlots> mLabelSerial{};   // per slot: the label list bound (GpuBuffer::serial; 0: the stand-in)
    std::array<std::array<VkImageView,kImageBindings>,kSlots> mCaptureInputs{};   // (mCaptureSets' caches)
    std::array<uint64_t,kSlots> mCaptureLabelSerial{};
    GpuBuffer mStandInLabels;                      // binding 3 when a frame has no labels (never read)
    VkSurfaceFormatKHR mTarget{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    vkdisplay::Encoding mEncoding = vkdisplay::Encoding::Sdr;
};
