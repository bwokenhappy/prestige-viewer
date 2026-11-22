#include "vk_display_encode.h"
#include "vk_pipeline.h"
#include <utility>

void VkDisplayEncode::destroy()
{
    if (!mContext) return;
    const VkDevice device=mContext->device();
    if (mPipeline) { vkForgetPipeline(mPipeline); vkDestroyPipeline(device,mPipeline,nullptr); }
    if (mCompositePipeline) { vkForgetPipeline(mCompositePipeline); vkDestroyPipeline(device,mCompositePipeline,nullptr); }
    if (mFgPipeline) { vkForgetPipeline(mFgPipeline); vkDestroyPipeline(device,mFgPipeline,nullptr); }
    if (mLayout) vkDestroyPipelineLayout(device,mLayout,nullptr);
    if (mPool) vkDestroyDescriptorPool(device,mPool,nullptr);
    if (mSetLayout) vkDestroyDescriptorSetLayout(device,mSetLayout,nullptr);
    if (mSampler) vkDestroySampler(device,mSampler,nullptr);
    if (mStandInLabels.buffer) mContext->destroyBuffer(mStandInLabels);
    mStandInLabels=GpuBuffer{};
    mPipeline=VK_NULL_HANDLE; mCompositePipeline=VK_NULL_HANDLE; mFgPipeline=VK_NULL_HANDLE; mLayout=VK_NULL_HANDLE; mPool=VK_NULL_HANDLE;
    mSetLayout=VK_NULL_HANDLE; mSampler=VK_NULL_HANDLE;
    mSets={};mCaptureSets={};mFgCompSets={};mInputs={};mLabelSerial={};mCaptureInputs={};mCaptureLabelSerial={};
    mContext=nullptr;
    mTarget={VK_FORMAT_UNDEFINED,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    mEncoding=vkdisplay::Encoding::Sdr;
    mMode=Mode::Hdr;
}

bool VkDisplayEncode::validForTarget(VkSurfaceFormatKHR target) const
{
    return mContext && mPipeline && mTarget.format==target.format &&
        mTarget.colorSpace==target.colorSpace;
}

void VkDisplayEncode::swap(VkDisplayEncode& other) noexcept
{
    using std::swap;
    swap(mContext,other.mContext);swap(mPipeline,other.mPipeline);swap(mLayout,other.mLayout);
    swap(mCompositePipeline,other.mCompositePipeline);swap(mFgPipeline,other.mFgPipeline);
    swap(mSetLayout,other.mSetLayout);swap(mPool,other.mPool);swap(mSampler,other.mSampler);
    swap(mSets,other.mSets);swap(mFgCompSets,other.mFgCompSets);swap(mInputs,other.mInputs);swap(mTarget,other.mTarget);
    swap(mLabelSerial,other.mLabelSerial);swap(mStandInLabels,other.mStandInLabels);
    swap(mCaptureSets,other.mCaptureSets);swap(mCaptureInputs,other.mCaptureInputs);
    swap(mCaptureLabelSerial,other.mCaptureLabelSerial);
    swap(mEncoding,other.mEncoding);swap(mMode,other.mMode);
}

bool VkDisplayEncode::prepare(VkDeviceContext& ctx,bool enabled,VkSurfaceFormatKHR target,
                             std::span<const uint32_t> vertex,std::span<const uint32_t> fragment,bool sdrLayer)
{
    destroy(); // Caller drained; toggling off releases once, never allocates.
    if (!enabled) return true;
    if (vertex.empty() || fragment.empty()) return false;
    mEncoding=vkdisplay::encodingOf(target);
    mTarget=target;
    const bool sdrCapture=target.format==VK_FORMAT_B8G8R8A8_UNORM &&
                          target.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    // #1536: the SDR layer takes either 8-bit UNORM order (the formats frame generation runs on in SDR)
    const bool sdrLayerTarget=(target.format==VK_FORMAT_B8G8R8A8_UNORM || target.format==VK_FORMAT_R8G8B8A8_UNORM) &&
                              target.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (sdrLayer && (mEncoding!=vkdisplay::Encoding::Sdr || !sdrLayerTarget)) return false;
    if (mEncoding==vkdisplay::Encoding::Sdr && !sdrLayer && !sdrCapture) return false;
    mMode=mEncoding!=vkdisplay::Encoding::Sdr ? Mode::Hdr : sdrLayer ? Mode::SdrLayer : Mode::SdrCapture;
    mContext=&ctx;
    const VkDevice device=ctx.device();
    auto failed=[this] { destroy();return false; };
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter=VK_FILTER_NEAREST;sampler.minFilter=VK_FILTER_NEAREST;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device,&sampler,nullptr,&mSampler)!=VK_SUCCESS) return failed();
    // 0: the world in linear nits, 1: the UI layer, 2: the label atlas, 3: the label list (#1517 Task 7e;
    // stand-ins when the frame has none -- the shader reads them only with the 0x200 flag)
    VkDescriptorSetLayoutBinding bindings[kBindings]{};
    for (uint32_t i=0;i<kBindings;++i) {
        bindings[i].binding=i;bindings[i].descriptorCount=1;
        bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;bindings[i].pImmutableSamplers=&mSampler;
    }
    bindings[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[3].pImmutableSamplers=nullptr;
    mStandInLabels=ctx.createBuffer(64,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!mStandInLabels.buffer) return failed();
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount=kBindings;set.pBindings=bindings;
    if (vkCreateDescriptorSetLayout(device,&set,nullptr,&mSetLayout)!=VK_SUCCESS) return failed();
    constexpr uint32_t sets=uint32_t(std::tuple_size<decltype(mSets)>::value+std::tuple_size<decltype(mCaptureSets)>::value+
                                     std::tuple_size<decltype(mFgCompSets)>::value);
    const VkDescriptorPoolSize poolSizes[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,kImageBindings*sets},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,sets}};   // (+ frame generation's composites)
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets=sets;pool.poolSizeCount=2;pool.pPoolSizes=poolSizes;
    if (vkCreateDescriptorPool(device,&pool,nullptr,&mPool)!=VK_SUCCESS) return failed();
    std::array<VkDescriptorSetLayout,kSlots> layouts;
    layouts.fill(mSetLayout);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool=mPool;allocate.descriptorSetCount=kSlots;allocate.pSetLayouts=layouts.data();
    if (vkAllocateDescriptorSets(device,&allocate,mSets.data())!=VK_SUCCESS) return failed();
    // #1536 a snapshot's SDR-layer composite: optional (without them the shot has the world only).
    if (vkAllocateDescriptorSets(device,&allocate,mCaptureSets.data())!=VK_SUCCESS) mCaptureSets={};
    // Frame generation's composites: optional (a refusal only leaves frame generation unavailable).
    std::array<VkDescriptorSetLayout,std::tuple_size<decltype(mFgCompSets)>::value> fgLayouts;
    fgLayouts.fill(mSetLayout);
    VkDescriptorSetAllocateInfo fgAllocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    fgAllocate.descriptorPool=mPool;fgAllocate.descriptorSetCount=uint32_t(fgLayouts.size());
    fgAllocate.pSetLayouts=fgLayouts.data();
    if (vkAllocateDescriptorSets(device,&fgAllocate,mFgCompSets.data())!=VK_SUCCESS) mFgCompSets={};
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,20};   // (Push: + labelPhase, #1517 Task 7e)
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount=1;layout.pSetLayouts=&mSetLayout;
    layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
    if (vkCreatePipelineLayout(device,&layout,nullptr,&mLayout)!=VK_SUCCESS) return failed();
    VkShaderModule modules[2]{};
    const std::span<const uint32_t> binaries[]{vertex,fragment};
    for (unsigned i=0;i<2;++i) {
        VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shader.codeSize=binaries[i].size_bytes();shader.pCode=binaries[i].data();
        if (vkCreateShaderModule(device,&shader,nullptr,&modules[i])!=VK_SUCCESS) {
            if (modules[0]) vkDestroyShaderModule(device,modules[0],nullptr);
            return failed();
        }
    }
    GraphicsPipelineDesc description;
    description.vert=modules[0];description.frag=modules[1];description.layout=mLayout;
    description.colorFormat=target.format;description.depthFormat=VK_FORMAT_UNDEFINED;
    description.depthTest=false;description.depthWrite=false;description.blend=false;
    // Existing pipeline factory returns null on bounded allocation/OOM recovery.
    mPipeline=createGraphicsPipeline(device,description);
    if (mPipeline && mEncoding!=vkdisplay::Encoding::Sdr) {
        // The composite into an FP16 snapshot source (same shader, encoding 3).
        description.colorFormat=VK_FORMAT_R16G16B16A16_SFLOAT;
        mCompositePipeline=createGraphicsPipeline(device,description);
    }
    if (mPipeline && mCompositePipeline && mEncoding==vkdisplay::Encoding::Hdr10) {
        // Frame generation (#1517): the same pass also writes the world alone in the output's encoding
        // and a copy of the UI layer. Optional: without it frame generation is unavailable, nothing else.
        GraphicsPipelineDesc fg=description;
        fg.mrtCount=3;
        fg.mrtFormats[0]=target.format;fg.mrtFormats[1]=kFgHudlessFormat;fg.mrtFormats[2]=kFgUiFormat;
        mFgPipeline=createGraphicsPipeline(device,fg);
    }
    for (auto module:modules) vkDestroyShaderModule(device,module,nullptr);
    if (!mPipeline || (mEncoding!=vkdisplay::Encoding::Sdr && !mCompositePipeline)) return failed();
    return true;
}

void VkDisplayEncode::updateSlotSet(uint32_t slot,const std::array<VkImageView,kImageBindings>& inputs,
                                    const GpuBuffer& labels,bool capture)
{
    auto& cached=capture ? mCaptureInputs[slot] : mInputs[slot];
    uint64_t& cachedLabels=capture ? mCaptureLabelSerial[slot] : mLabelSerial[slot];
    const VkDescriptorSet set=capture ? mCaptureSets[slot] : mSets[slot];
    if (cached==inputs && cachedLabels==labels.serial) return;
    VkDescriptorImageInfo images[kImageBindings]{{mSampler,inputs[0],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                 {mSampler,inputs[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                                 {mSampler,inputs[2],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    const VkDescriptorBufferInfo list{labels.buffer,0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet write[kBindings]{};
    for (uint32_t i=0;i<kBindings;++i) {
        write[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[i].dstSet=set;write[i].dstBinding=i;write[i].descriptorCount=1;
        write[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write[i].pImageInfo=&images[i];
    }
    write[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write[3].pImageInfo=nullptr;write[3].pBufferInfo=&list;
    vkUpdateDescriptorSets(mContext->device(),kBindings,write,0,nullptr);
    cached=inputs;
    cachedLabels=labels.serial;
}

bool VkDisplayEncode::record(VkCommandBuffer cmd,uint32_t slot,VkImageView input,VkImageView uiLayer,
                            VkImageView target,VkExtent2D extent,vkdisplay::Request request,Content content,
                            uint64_t allHdrImageBytes,uint64_t declaredBudget,bool dither,bool sdrWorld,
                            VkImageView fgHudless,VkImageView fgUi,VkImageView labelAtlas,
                            const GpuBuffer* labelBuffer)
{
    if (mMode==Mode::SdrLayer) return false;   // (#1536: the SDR layer composes, it never encodes)
    const bool fg=fgHudless || fgUi;
    if (fg && (!fgHudless || !fgUi || !mFgPipeline || content!=Content::CompleteDisplay || sdrWorld ||
               fgHudless==target || fgUi==target)) return false;
    if (!mContext || !mPipeline || !cmd || slot>=mSets.size() || !input || !target || input==target ||
        uiLayer==target || !vkdisplay::imageBudget(extent.width,extent.height,1,allHdrImageBytes,declaredBudget))
        return false;
    if ((mEncoding==vkdisplay::Encoding::Sdr && content!=Content::WorldBeforeUi) ||
        (mEncoding!=vkdisplay::Encoding::Sdr && content==Content::WorldBeforeUi)) return false;
    const bool composite=content==Content::CompositeNits;
    if ((mEncoding!=vkdisplay::Encoding::Sdr && !uiLayer) || (composite && !mCompositePipeline)) return false;
    // Labels only on an HDR10 frame of the display (they are drawn between the world and the interface;
    // never on a snapshot / composite frame). Usually one that also writes frame generation's outputs.
    if ((labelAtlas || labelBuffer) &&
        (!labelAtlas || !labelBuffer || !labelBuffer->buffer || mEncoding!=vkdisplay::Encoding::Hdr10 ||
         content!=Content::CompleteDisplay || sdrWorld || labelAtlas==target)) return false;
    // The SDR world capture never reads binding 1, and no frame without labels reads bindings 2 and 3; all
    // still need a valid resource there.
    const VkImageView layer=uiLayer ? uiLayer : input;
    const std::array<VkImageView,kImageBindings> inputs{input,layer,labelAtlas ? labelAtlas : layer};
    updateSlotSet(slot,inputs,labelAtlas ? *labelBuffer : mStandInLabels);
    request=vkdisplay::sanitized(request);
    struct Push { uint32_t encoding;float paperWhite,peakNits,dither,labelPhase; };
    const uint32_t encoding=(composite ? 3u : uint32_t(mEncoding))|(sdrWorld ? 0x100u : 0u)|(labelAtlas ? 0x200u : 0u);
    const Push push{encoding,request.paperWhiteNits,request.peakNits,dither && !composite ? 1.f : 0.f,1.f};
    static_assert(sizeof(Push)==20);
    VkRenderingAttachmentInfo colour[3]{};
    const VkImageView views[3]{target,fgHudless,fgUi};
    for (uint32_t i=0;i<3;++i) {
        colour[i].sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colour[i].imageView=views[i];colour[i].imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colour[i].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;colour[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea={{0,0},extent};rendering.layerCount=1;
    rendering.colorAttachmentCount=fg ? 3u : 1u;rendering.pColorAttachments=colour;
    vkCmdBeginRendering(cmd,&rendering);
    VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1};
    VkRect2D scissor{{0,0},extent};
    vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
    bindGraphicsPipeline(cmd,composite ? mCompositePipeline : fg ? mFgPipeline : mPipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,mLayout,0,1,&mSets[slot],0,nullptr);
    vkCmdPushConstants(cmd,mLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
    vkCmdDraw(cmd,3,1,0,0);
    vkCmdEndRendering(cmd);
    return true;
}

// Frame generation (#1517): encoding 4 of the same shader -- the world (in the output's encoding) decoded
// to nits where the interface or a label reaches, composed as the real frame's (labels, then the UI layer,
// at paper white), encoded back; untouched elsewhere.
bool VkDisplayEncode::recordFgComposite(VkCommandBuffer cmd,uint32_t slot,uint32_t index,VkImageView worldEncoded,
                                       VkImageView uiLayer,VkImageView target,VkExtent2D extent,
                                       VkImageView labelAtlas,VkBuffer labelBuffer,float phase,float paperWhiteNits,
                                       float peakNits)
{
    const bool labels=labelAtlas && labelBuffer;
    if (!mContext || !mPipeline || !cmd || slot>=mSets.size() || index>=kMaxFgComposites || !worldEncoded ||
        !uiLayer || !target) return false;
    const VkDescriptorSet set=mFgCompSets[slot*kMaxFgComposites+index];
    if (!set) return false;
    VkDescriptorImageInfo images[kImageBindings]{{mSampler,worldEncoded,VK_IMAGE_LAYOUT_GENERAL},
                                                 {mSampler,uiLayer,VK_IMAGE_LAYOUT_GENERAL},
                                                 labels ? VkDescriptorImageInfo{mSampler,labelAtlas,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
                                                        : VkDescriptorImageInfo{mSampler,uiLayer,VK_IMAGE_LAYOUT_GENERAL}};   // (a stand-in)
    const VkDescriptorBufferInfo list{labels ? labelBuffer : mStandInLabels.buffer,0,VK_WHOLE_SIZE};
    VkWriteDescriptorSet write[kBindings]{};
    for (uint32_t i=0;i<kBindings;++i) {
        write[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write[i].dstSet=set;write[i].dstBinding=i;write[i].descriptorCount=1;
        write[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write[i].pImageInfo=&images[i];
    }
    write[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write[3].pImageInfo=nullptr;write[3].pBufferInfo=&list;
    vkUpdateDescriptorSets(mContext->device(),kBindings,write,0,nullptr);
    // (#1536: the SDR layer's pictures compose in the SDR encoded space, encoding 5; paper white unused)
    const uint32_t encoding=(mMode==Mode::SdrLayer ? 5u : 4u)|(labels ? 0x200u : 0u);
    drawComposite(cmd,set,encoding,phase,paperWhiteNits,peakNits,target,extent);
    return true;
}

// #1536 SDR frame generation: the real picture, with the same draw as the generated ones (encoding 5, the
// labels at the real frame's moment).
bool VkDisplayEncode::recordLayerComposite(VkCommandBuffer cmd,uint32_t slot,VkImageView world,VkImageView uiLayer,
                                          VkImageView target,VkExtent2D extent,VkImageView labelAtlas,
                                          const GpuBuffer* labelBuffer,bool capture)
{
    const bool labels=labelAtlas && labelBuffer && labelBuffer->buffer;
    if (mMode!=Mode::SdrLayer || !mContext || !mPipeline || !cmd || slot>=mSets.size() || !world || !uiLayer ||
        !target || world==target || uiLayer==target || (labelAtlas || labelBuffer) != labels || labelAtlas==target ||
        (capture && !mCaptureSets[slot]))
        return false;
    updateSlotSet(slot,{world,uiLayer,labels ? labelAtlas : uiLayer},labels ? *labelBuffer : mStandInLabels,capture);
    drawComposite(cmd,capture ? mCaptureSets[slot] : mSets[slot],5u|(labels ? 0x200u : 0u),1.f,0.f,0.f,target,extent);
    return true;
}

void VkDisplayEncode::drawComposite(VkCommandBuffer cmd,VkDescriptorSet set,uint32_t encoding,float phase,
                                    float paperWhiteNits,float peakNits,VkImageView target,VkExtent2D extent)
{
    struct Push { uint32_t encoding;float paperWhite,peakNits,dither,labelPhase; };
    const Push push{encoding,paperWhiteNits,peakNits,0.f,phase};
    static_assert(sizeof(Push)==20);
    VkRenderingAttachmentInfo colour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colour.imageView=target;colour.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;colour.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea={{0,0},extent};rendering.layerCount=1;
    rendering.colorAttachmentCount=1;rendering.pColorAttachments=&colour;
    vkCmdBeginRendering(cmd,&rendering);
    VkViewport viewport{0,0,float(extent.width),float(extent.height),0,1};
    VkRect2D scissor{{0,0},extent};
    vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
    bindGraphicsPipeline(cmd,mPipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,mLayout,0,1,&set,0,nullptr);
    vkCmdPushConstants(cmd,mLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
    vkCmdDraw(cmd,3,1,0,0);
    vkCmdEndRendering(cmd);
}
