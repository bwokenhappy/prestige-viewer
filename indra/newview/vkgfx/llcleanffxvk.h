/**
 * @file llcleanffxvk.h
 * @brief #1600 AMD FSR 4 on Vulkan through the FSR SDK 2.4 runtime: FSR Upscaling 4.1.2 (ML) and FSR
 *        Frame Generation 4.0.2, Windows only.
 *
 * AMD ships FSR 4 only as signed DLLs (amd_fidelityfx_loader_vk.dll and one DLL per effect beside it;
 * cmake/FidelityFXSdk.cmake stages them for the Windows package). This file loads the loader at run time
 * and drives its two effects; nothing links against it. Where it is missing, on Linux, or where the
 * GPU / driver offers no 4.x version of an effect (FSR 4 upscaling: RX 6000 and newer; FSR 4 frame
 * generation: RX 9000 and newer, Windows 11, Adrenalin 26.10.1 or later), create() refuses with the
 * reason and the renderer keeps its own source-built FSR 3.1 (llcleanfsr3vk.h, llvkframegenfsr.h).
 * The SDK's own 3.1 versions are never used: ours stays the one FSR 3.1.
 *
 * The SDK headers are compiled in llcleanffxvk.cpp only: ffx_api_vk.h declares a struct VkDeviceContext
 * of its own, which clashes with the renderer's class of that name. This interface names no SDK type.
 *
 * Layouts: every image is handed over in the layout it is in (GENERAL, or SHADER_READ_ONLY_OPTIMAL for a
 * sampled input); the SDK records its own barriers and leaves each image in that layout again. Its own
 * resources are made at create() and freed at destroy(); nothing is allocated per frame.
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
#ifndef LL_LLCLEANFFXVK_H
#define LL_LLCLEANFFXVK_H

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>

namespace llffx
{
// One image the SDK reads or writes (not owned). A single 2D layer, one mip.
struct Image
{
    VkImage       image = VK_NULL_HANDLE;
    VkFormat      format = VK_FORMAT_UNDEFINED;
    uint32_t      w = 0, h = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;   // at the dispatch, and again after it
    bool          write = false;                      // the SDK writes it (an output; needs STORAGE usage)
};

// The device the effects run on (the renderer's).
struct Device
{
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice         device = VK_NULL_HANDLE;
    // The device was made with the shader features FSR 4's SPIR-V uses (VkDeviceContext::fsr4Features): the
    // runtime judges by what the GPU supports, so without them its pipelines would use disabled capabilities.
    bool             shaderFeatures = false;
};

// The camera both effects need, from the projection the frame was rendered with (reversed-Z).
struct Camera
{
    float jitterPx[2] = {0.f, 0.f};   // render pixels, as rendered (FSR takes it as it is)
    float nearDist = 0.1f;            // view depth at device depth 1
    float farDist = 1000.f;           // view depth at device depth 0 (ignored when infiniteFar)
    bool  infiniteFar = false;
    float fovY = 1.f;                 // vertical field of view, radians
};
// From the unjittered projection (column-major) and its inverse: llfsr::deviceToViewDepth's near / far.
Camera cameraFromProjection(const float proj[16], const float invProj[16], const float jitterPx[2]);

// Whether the runtime is loaded (it is loaded on the first call). false: why (for the log).
bool runtimeAvailable(std::string* why = nullptr);

// FSR Upscaling 4.x. Made for one render / display size, HDR linear colour, reversed-Z depth.
class Upscaler
{
public:
    Upscaler() = default;
    ~Upscaler();
    Upscaler(const Upscaler&) = delete;
    Upscaler& operator=(const Upscaler&) = delete;

    // The 4.x upscaler for these sizes. false: FSR 4 cannot run here (why: the reason, for the log --
    // the runtime missing, no 4.x version for this GPU / driver, a refused context).
    bool create(const Device& dev, uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH,
                bool infiniteFar, std::string& why);
    bool valid() const { return mCtx != nullptr; }
    bool fits(uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH, bool infiniteFar) const;
    // The version the runtime picked ("4.1.2", ...), after a successful create().
    const std::string& version() const { return mVersion; }

    struct Inputs
    {
        Image color;          // jittered linear HDR scene, render size
        Image depth;          // R32F reversed-Z device depth, render size
        Image motion;         // RG16F prev_uv - uv, unjittered, render size
        Image exposure;       // 1x1 R32F exposure
        Image reactive;       // R8 reactive mask (optional)
        Image composition;    // R8 transparency & composition (optional)
        Image output;         // display size, written
        uint32_t renderW = 0, renderH = 0;
        Camera camera;
        float frameMs = 16.7f;     // since the previous frame
        float sharpness = 0.f;     // 0..1, 0 = no sharpening
        bool  reset = false;       // history invalid (a cut, new targets)
    };
    // Records the upscale into cmd. false: refused (err says why; nothing usable was written).
    bool dispatch(VkCommandBuffer cmd, const Inputs& in, std::string& err);
    void destroy();

private:
    void* mCtx = nullptr;
    uint32_t mRw = 0, mRh = 0, mDw = 0, mDh = 0;
    bool mInfinite = false;
    std::string mVersion;
};

// FSR Frame Generation 4.x, dispatched by hand into our own present chain (no SDK swapchain, so no
// driver-assisted pacing: ours paces). One generated frame per rendered frame.
class FrameGen
{
public:
    FrameGen() = default;
    ~FrameGen();
    FrameGen(const FrameGen&) = delete;
    FrameGen& operator=(const FrameGen&) = delete;

    // colorFormat: the frames' encoding (8-bit sRGB SDR, or 10-bit HDR10 PQ when hdr).
    bool create(const Device& dev, uint32_t displayW, uint32_t displayH, uint32_t renderW, uint32_t renderH,
                VkFormat colorFormat, bool hdr, bool infiniteFar, std::string& why);
    bool valid() const { return mCtx != nullptr; }
    bool fits(uint32_t displayW, uint32_t displayH, uint32_t renderW, uint32_t renderH, VkFormat colorFormat,
              bool hdr, bool infiniteFar) const;
    const std::string& version() const { return mVersion; }

    struct Inputs
    {
        Image color;          // the rendered frame, display size (the world without the interface)
        Image depth;          // R32F reversed-Z device depth, render size
        Image motion;         // RG16F prev_uv - uv, unjittered, render size
        Image output;         // the generated frame, display size, written
        uint32_t renderW = 0, renderH = 0;
        Camera camera;
        float camPos[3] = {}, camUp[3] = {}, camRight[3] = {}, camFwd[3] = {};   // world space
        float frameMs = 16.7f;
        float peakNits = 0.f;      // HDR10: the display's peak (0: not stated)
        bool  hdr = false;         // the frame is HDR10 (must match create's)
        uint64_t frameId = 0;      // rendered frames, +1 each (another step resets the generator)
        bool  reset = false;
    };
    // Records the prepare and the generation of one frame into cmd. false: refused (err says why).
    bool dispatch(VkCommandBuffer cmd, const Inputs& in, std::string& err);
    void destroy();

private:
    void* mCtx = nullptr;
    uint32_t mDw = 0, mDh = 0, mRw = 0, mRh = 0;
    VkFormat mFormat = VK_FORMAT_UNDEFINED;
    bool mHdr = false, mInfinite = false;
    std::string mVersion;
};
} // namespace llffx

#endif // LL_LLCLEANFFXVK_H
