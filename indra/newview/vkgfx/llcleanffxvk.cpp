/**
 * @file llcleanffxvk.cpp
 * @brief #1600 AMD FSR 4 through the FSR SDK 2.4 runtime (see llcleanffxvk.h).
 *
 * The only file that includes the SDK's headers (third_party/fidelityfx-sdk): ffx_api_vk.h declares a
 * struct VkDeviceContext, which must never meet the renderer's class of that name (vk_device.h is not
 * included here, directly or through another header).
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

#include "llcleanffxvk.h"

#include "llvkframegenfsrmath.h"   // llfsr::deviceToViewDepth (no Vulkan, no vk_device.h)

#include "lldir.h"
#include "llerror.h"
#include "llstring.h"

// AMD's headers are written for MSVC: FFX_API_ENTRY is __declspec(dllexport) (only the DLL's exports
// use it; we load the runtime and call through function pointers), and they write
// `#pragma FFX_PRAGMA_WARNING_PUSH`, which GCC does not macro-expand (-Wunknown-pragmas, an error
// here). Off Windows the file only has to compile: the runtime is Windows-only (api() says so).
#if !LL_WINDOWS
#define __declspec(x)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#include "third_party/fidelityfx-sdk/api/include/ffx_api.h"
#include "third_party/fidelityfx-sdk/api/include/ffx_api_types.h"
#include "third_party/fidelityfx-sdk/api/include/vk/ffx_api_vk.h"
#include "third_party/fidelityfx-sdk/upscalers/include/ffx_upscale.h"
#include "third_party/fidelityfx-sdk/framegeneration/include/ffx_framegeneration.h"
#include "third_party/fidelityfx-sdk/framegeneration/include/vk/ffx_api_framegeneration_vk.h"
#if !LL_WINDOWS
#pragma GCC diagnostic pop
#undef __declspec
#endif

#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#if LL_WINDOWS
#include "llwin32headers.h"
#endif

namespace
{
// ---- the runtime: amd_fidelityfx_loader_vk.dll and its five entry points ----
struct Api
{
    bool tried = false;
    std::string why;   // why it is not loaded
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    bool ok() const { return create && destroy && configure && query && dispatch; }
};
Api sApi;
std::mutex sApiMutex;

const Api& api()
{
    std::lock_guard<std::mutex> lk(sApiMutex);
    if (sApi.tried) return sApi;
    sApi.tried = true;
#if LL_WINDOWS
    // The executable folder first (the package puts it there; the loader finds its effect DLLs beside
    // itself), then the plain name. Kept loaded for the session: contexts made from it may live as long.
    const char* name = "amd_fidelityfx_loader_vk.dll";
    std::vector<std::string> tries;
    if (gDirUtilp) tries.push_back(gDirUtilp->getExecutableDir() + "\\" + name);
    tries.push_back(name);
    HMODULE lib = nullptr;
    std::string errs;
    for (const std::string& t : tries)
    {
        const std::wstring w = ll_convert_string_to_wide(t);
        // LOAD_WITH_ALTERED_SEARCH_PATH: the library's own folder resolves its dependencies.
        lib = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (lib) break;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "LoadLibrary error %lu", (unsigned long)GetLastError());
        errs += (errs.empty() ? "" : "; ") + t + ": " + buf;
    }
    if (!lib)
    {
        sApi.why = std::string("the FSR SDK runtime (") + name + ") could not be loaded (" + errs + ")";
        return sApi;
    }
    sApi.create = (PfnFfxCreateContext)GetProcAddress(lib, "ffxCreateContext");
    sApi.destroy = (PfnFfxDestroyContext)GetProcAddress(lib, "ffxDestroyContext");
    sApi.configure = (PfnFfxConfigure)GetProcAddress(lib, "ffxConfigure");
    sApi.query = (PfnFfxQuery)GetProcAddress(lib, "ffxQuery");
    sApi.dispatch = (PfnFfxDispatch)GetProcAddress(lib, "ffxDispatch");
    if (!sApi.ok())
        sApi.why = std::string(name) + " lacks an ffxCreateContext / ffxDestroyContext / ffxConfigure / ffxQuery / "
                   "ffxDispatch entry point";
    else
        LL_INFOS("Vulkan") << "FSR SDK runtime loaded (" << name << ")" << LL_ENDL;
#else
    sApi.why = "the FSR SDK runtime exists for Windows only";
#endif
    return sApi;
}

// The SDK's messages (errors and warnings from its context), to the log; at most a few a second. Called from
// the render thread (the upscaler) and the present thread (frame generation) alike.
void onMessage(uint32_t type, const wchar_t* message)
{
    static std::mutex s_mutex;
    static std::chrono::steady_clock::time_point s_last{};
    static uint32_t s_dropped = 0;
    std::lock_guard<std::mutex> lk(s_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last < std::chrono::milliseconds(250)) { ++s_dropped; return; }
    s_last = now;
#if LL_WINDOWS
    const std::string text = message ? ll_convert_wide_to_string(message) : std::string();
#else
    // (Never called off Windows: the runtime does not load there. ASCII is enough to compile.)
    std::string text;
    for (const wchar_t* p = message; p && *p; ++p) text.push_back(*p < 0x80 ? (char)*p : '?');
#endif
    LL_WARNS("Vulkan") << "FSR SDK " << (type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning") << ": " << text
                       << (s_dropped ? " (" + std::to_string(s_dropped) + " more since the last)" : std::string())
                       << LL_ENDL;
    s_dropped = 0;
}

// "4.1.2", "FSR 4.1.2", ... -> {4, 1, 2}; a name without a number -> {0, 0, 0}.
struct Ver { int major = 0, minor = 0, patch = 0; };
Ver parseVersion(const char* name)
{
    Ver v;
    if (!name) return v;
    const char* p = name;
    while (*p && (*p < '0' || *p > '9')) ++p;
    if (!*p) return v;
    std::sscanf(p, "%d.%d.%d", &v.major, &v.minor, &v.patch);
    return v;
}
bool newer(const Ver& a, const Ver& b)
{
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    return a.patch > b.patch;
}

// The newest version of an effect this device offers with a major version of at least minMajor: its id for
// ffxOverrideVersion. false: none (all: every version offered, for the log).
bool pickVersion(uint64_t createType, VkDeviceContext* dc, int minMajor, uint64_t& id, std::string& name,
                 std::string& all)
{
    const Api& a = api();
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = createType;
    q.device = dc;
    q.outputCount = &count;
    if (a.query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0) return false;
    std::vector<uint64_t> ids((size_t)count);
    std::vector<const char*> names((size_t)count, nullptr);
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (a.query(nullptr, &q.header) != FFX_API_RETURN_OK) return false;
    bool found = false;
    Ver best;
    for (size_t i = 0; i < (size_t)count && i < ids.size(); ++i)
    {
        const char* n = names[i] ? names[i] : "?";
        all += (all.empty() ? "" : ", ") + std::string(n);
        const Ver v = parseVersion(n);
        if (v.major < minMajor) continue;
        if (!found || newer(v, best))
        {
            found = true;
            best = v;
            id = ids[i];
            name = n;
        }
    }
    return found;
}

std::string providerName(ffxContext* ctx)
{
    ffxQueryGetProviderVersion q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (api().query(ctx, &q.header) != FFX_API_RETURN_OK || !q.versionName) return std::string();
    return q.versionName;
}

const char* resultText(ffxReturnCode_t r)
{
    switch (r)
    {
    case FFX_API_RETURN_OK: return "ok";
    case FFX_API_RETURN_ERROR: return "error";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown description type";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error";
    case FFX_API_RETURN_NO_PROVIDER: return "no provider";
    case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
    case FFX_API_RETURN_ERROR_PARAMETER: return "bad parameter";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "provider too old for this description";
    default: return "unknown result";
    }
}

// One of our images as an SDK resource: a single-layer 2D texture in the layout it is in now (GENERAL: the
// SDK's "unordered access" when it writes it, "generic read" when it only reads it; SHADER_READ_ONLY_OPTIMAL:
// "compute read"), returned to that layout after the dispatch. An empty Image: an empty resource (optional
// inputs).
FfxApiResource resource(const llffx::Image& im)
{
    if (!im.image) return FfxApiResource{};
    FfxApiResourceDescription d{};
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    d.format = ffxApiGetSurfaceFormatVK(im.format);
    d.width = im.w;
    d.height = im.h;
    d.depth = 1;
    d.mipCount = 1;
    d.flags = FFX_API_RESOURCE_FLAGS_NONE;
    d.usage = im.write ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    uint32_t state = FFX_API_RESOURCE_STATE_COMPUTE_READ;
    if (im.layout == VK_IMAGE_LAYOUT_GENERAL)
        state = im.write ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_GENERIC_READ;
    return ffxApiGetResourceVK((void*)im.image, d, state);
}

VkDeviceContext deviceContext(const llffx::Device& dev)
{
    VkDeviceContext dc{};
    dc.vkDevice = dev.device;
    dc.vkPhysicalDevice = dev.physical;
    dc.vkDeviceProcAddr = vkGetDeviceProcAddr;
    return dc;
}
} // namespace

namespace llffx
{
Camera cameraFromProjection(const float proj[16], const float invProj[16], const float jitterPx[2])
{
    Camera c;
    c.jitterPx[0] = jitterPx[0];
    c.jitterPx[1] = jitterPx[1];
    float d2v[4];
    llfsr::deviceToViewDepth(proj, invProj, d2v);
    // View depth Z = d2v[1] / (device - d2v[0]): near at device depth 1, far at 0 (reversed-Z).
    const double f0 = d2v[0], f1 = d2v[1];
    const double zNear = f1 / (1.0 - f0), zFar = f1 / (0.0 - f0);
    c.nearDist = std::isfinite(zNear) && zNear > 0.0 ? (float)zNear : 0.1f;
    c.infiniteFar = !(std::isfinite(zFar) && std::fabs(zFar) < 1.0e7);
    c.farDist = c.infiniteFar ? FLT_MAX : (float)std::fabs(zFar);
    c.fovY = 2.f * std::atan(d2v[3]);   // (d2v[3]: tan of half the vertical field of view)
    return c;
}

bool runtimeAvailable(std::string* why)
{
    const Api& a = api();
    if (!a.ok() && why) *why = a.why;
    return a.ok();
}

// ---- Upscaler ----
Upscaler::~Upscaler()
{
    destroy();
}

bool Upscaler::fits(uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH, bool infiniteFar) const
{
    return mCtx && mRw == renderW && mRh == renderH && mDw == displayW && mDh == displayH && mInfinite == infiniteFar;
}

bool Upscaler::create(const Device& dev, uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH,
                      bool infiniteFar, std::string& why)
{
    destroy();
    if (!dev.shaderFeatures)
    {
        why = "the device was made without FSR 4's shader features (FSR 4 runs on AMD GPUs, Windows, Vulkan 1.3)";
        return false;
    }
    if (!runtimeAvailable(&why)) return false;
    const Api& a = api();
    VkDeviceContext dc = deviceContext(dev);
    uint64_t id = 0;
    std::string name, all;
    if (!pickVersion(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &dc, 4, id, name, all))
    {
        why = all.empty() ? std::string("the runtime offers no upscaler for this GPU / driver")
                          : "no FSR 4 upscaler for this GPU / driver (offered: " + all + ")";
        return false;
    }
    ffxCreateBackendVKDesc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext = dc;
    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion ov{};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = id;
    ffxCreateContextDescUpscale cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED
             | (infiniteFar ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0u);
    cd.maxRenderSize = FfxApiDimensions2D{renderW, renderH};
    cd.maxUpscaleSize = FfxApiDimensions2D{displayW, displayH};
    cd.fpMessage = onMessage;
    cd.header.pNext = &be.header;
    be.header.pNext = &ver.header;
    ver.header.pNext = &ov.header;
    ffxContext ctx = nullptr;
    const ffxReturnCode_t r = a.create(&ctx, &cd.header, nullptr);
    if (r != FFX_API_RETURN_OK || !ctx)
    {
        why = "the FSR " + name + " upscaler refused a " + std::to_string(renderW) + "x" + std::to_string(renderH) + " -> "
            + std::to_string(displayW) + "x" + std::to_string(displayH) + " context (" + resultText(r) + ")";
        return false;
    }
    mCtx = ctx;
    mRw = renderW; mRh = renderH; mDw = displayW; mDh = displayH;
    mInfinite = infiniteFar;
    const std::string provider = providerName(&mCtx);
    mVersion = provider.empty() ? name : provider;
    LL_INFOS("Vulkan") << "FSR 4 upscaler made: " << mVersion << ", " << renderW << "x" << renderH << " -> " << displayW
                       << "x" << displayH << (infiniteFar ? ", infinite far plane" : "") << " (offered: " << all << ")"
                       << LL_ENDL;
    return true;
}

bool Upscaler::dispatch(VkCommandBuffer cmd, const Inputs& in, std::string& err)
{
    if (!mCtx) { err = "the FSR 4 upscaler is not made"; return false; }
    if (in.renderW != mRw || in.renderH != mRh || in.output.w != mDw || in.output.h != mDh)
    {
        err = "the frame's sizes are not the ones the FSR 4 upscaler was made for";
        return false;
    }
    if (!in.color.image || !in.depth.image || !in.motion.image || !in.output.image)
    {
        err = "an FSR 4 input is missing";
        return false;
    }
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cmd;
    d.color = resource(in.color);
    d.depth = resource(in.depth);
    d.motionVectors = resource(in.motion);
    d.exposure = resource(in.exposure);
    d.reactive = resource(in.reactive);
    d.transparencyAndComposition = resource(in.composition);
    d.output = resource(in.output);
    d.jitterOffset = FfxApiFloatCoords2D{in.camera.jitterPx[0], in.camera.jitterPx[1]};
    // The inputs pass writes UV units (prev_uv - uv): x the render size = pixels, current -> previous.
    d.motionVectorScale = FfxApiFloatCoords2D{(float)in.renderW, (float)in.renderH};
    d.renderSize = FfxApiDimensions2D{in.renderW, in.renderH};
    d.upscaleSize = FfxApiDimensions2D{mDw, mDh};
    d.enableSharpening = in.sharpness > 0.f;
    d.sharpness = std::min(std::max(in.sharpness, 0.f), 1.f);
    d.frameTimeDelta = std::min(std::max(in.frameMs, 0.f), 1000.f);
    d.preExposure = 1.f;   // (the scene is not pre-exposed; the exposure image carries ours)
    d.reset = in.reset;
    d.cameraNear = in.camera.nearDist;
    d.cameraFar = in.camera.farDist;
    d.cameraFovAngleVertical = in.camera.fovY;
    d.viewSpaceToMetersFactor = 1.f;   // (SL: metres)
    d.flags = 0;
    const ffxReturnCode_t r = api().dispatch(&mCtx, &d.header);
    if (r != FFX_API_RETURN_OK)
    {
        err = std::string("the FSR 4 upscale was refused (") + resultText(r) + ")";
        return false;
    }
    return true;
}

void Upscaler::destroy()
{
    if (!mCtx) return;
    // (the caller has made the GPU idle: the context's resources may be in use by frames in flight)
    api().destroy(&mCtx, nullptr);
    mCtx = nullptr;
    mRw = mRh = mDw = mDh = 0;
    mVersion.clear();
}

// ---- FrameGen ----
FrameGen::~FrameGen()
{
    destroy();
}

bool FrameGen::fits(uint32_t displayW, uint32_t displayH, uint32_t renderW, uint32_t renderH, VkFormat colorFormat,
                    bool hdr, bool infiniteFar) const
{
    return mCtx && mDw == displayW && mDh == displayH && mRw == renderW && mRh == renderH && mFormat == colorFormat
        && mHdr == hdr && mInfinite == infiniteFar;
}

bool FrameGen::create(const Device& dev, uint32_t displayW, uint32_t displayH, uint32_t renderW, uint32_t renderH,
                      VkFormat colorFormat, bool hdr, bool infiniteFar, std::string& why)
{
    destroy();
    if (!dev.shaderFeatures)
    {
        why = "the device was made without FSR 4's shader features (FSR 4 runs on AMD GPUs, Windows, Vulkan 1.3)";
        return false;
    }
    if (!runtimeAvailable(&why)) return false;
    const Api& a = api();
    VkDeviceContext dc = deviceContext(dev);
    uint64_t id = 0;
    std::string name, all;
    if (!pickVersion(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &dc, 4, id, name, all))
    {
        why = all.empty() ? std::string("the runtime offers no frame generation for this GPU / driver")
                          : "no FSR 4 frame generation for this GPU / driver (offered: " + all
                                + "; FSR 4 frame generation needs an RX 9000 series GPU and Windows 11)";
        return false;
    }
    ffxCreateBackendVKDesc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext = dc;
    ffxCreateContextDescFrameGenerationVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxOverrideVersion ov{};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = id;
    ffxCreateContextDescFrameGeneration cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | (hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0u)
             | (infiniteFar ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0u);
    cd.displaySize = FfxApiDimensions2D{displayW, displayH};
    cd.maxRenderSize = FfxApiDimensions2D{renderW, renderH};
    cd.backBufferFormat = ffxApiGetSurfaceFormatVK(colorFormat);
    cd.header.pNext = &be.header;
    be.header.pNext = &ver.header;
    ver.header.pNext = &ov.header;
    ffxContext ctx = nullptr;
    ffxReturnCode_t r = a.create(&ctx, &cd.header, nullptr);
    if (r != FFX_API_RETURN_OK || !ctx)
    {
        why = "FSR " + name + " frame generation refused a " + std::to_string(displayW) + "x" + std::to_string(displayH)
            + " context (" + resultText(r) + ")";
        return false;
    }
    // Generation on, dispatched by hand: no SDK swapchain and no callbacks (the present chain is ours), so
    // FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY -- without it the provider hands the configuration
    // to a swapchain context and fails on the null one. dispatch() configures again every frame (its frame id).
    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = nullptr;
    cfg.frameGenerationEnabled = true;
    cfg.allowAsyncWorkloads = false;
    cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    cfg.generationRect = FfxApiRect2D{0, 0, (int32_t)displayW, (int32_t)displayH};
    cfg.frameID = 0;
    r = a.configure(&ctx, &cfg.header);
    if (r != FFX_API_RETURN_OK)
    {
        a.destroy(&ctx, nullptr);
        why = "FSR " + name + " frame generation refused its configuration (" + resultText(r) + ")";
        return false;
    }
    mCtx = ctx;
    mDw = displayW; mDh = displayH; mRw = renderW; mRh = renderH;
    mFormat = colorFormat;
    mHdr = hdr;
    mInfinite = infiniteFar;
    const std::string provider = providerName(&mCtx);
    mVersion = provider.empty() ? name : provider;
    LL_INFOS("Vulkan") << "FSR 4 frame generation made: " << mVersion << ", " << displayW << "x" << displayH << " (render "
                       << renderW << "x" << renderH << ", " << (hdr ? "HDR10" : "SDR") << ") (offered: " << all << ")"
                       << LL_ENDL;
    return true;
}

bool FrameGen::dispatch(VkCommandBuffer cmd, const Inputs& in, std::string& err)
{
    if (!mCtx) { err = "FSR 4 frame generation is not made"; return false; }
    if (in.renderW != mRw || in.renderH != mRh || in.color.w != mDw || in.color.h != mDh || in.output.w != mDw
        || in.output.h != mDh)
    {
        err = "the frame's sizes are not the ones FSR 4 frame generation was made for";
        return false;
    }
    if (!in.color.image || !in.depth.image || !in.motion.image || !in.output.image)
    {
        err = "an FSR 4 frame generation input is missing";
        return false;
    }
    if (in.hdr != mHdr)
    {
        err = std::string("FSR 4 frame generation was made for ") + (mHdr ? "HDR10" : "SDR") + " frames";
        return false;
    }
    const Api& a = api();
    // Every frame, as AMD's sample does: the configuration carries the frame id the provider checks the prepare
    // and the dispatch against (a configuration of another frame id makes every frame a reset).
    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = nullptr;
    cfg.frameGenerationEnabled = true;
    cfg.allowAsyncWorkloads = false;
    cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    cfg.generationRect = FfxApiRect2D{0, 0, (int32_t)mDw, (int32_t)mDh};
    cfg.frameID = in.frameId;
    ffxReturnCode_t r = a.configure(&mCtx, &cfg.header);
    if (r != FFX_API_RETURN_OK)
    {
        err = std::string("FSR 4 frame generation refused its configuration (") + resultText(r) + ")";
        return false;
    }
    ffxDispatchDescFrameGenerationPrepareV2 p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = in.frameId;
    p.flags = cfg.flags;   // (as AMD's sample: the prepare takes the configuration's flags)
    p.commandList = cmd;
    p.renderSize = FfxApiDimensions2D{in.renderW, in.renderH};
    p.jitterOffset = FfxApiFloatCoords2D{in.camera.jitterPx[0], in.camera.jitterPx[1]};
    p.motionVectorScale = FfxApiFloatCoords2D{(float)in.renderW, (float)in.renderH};
    p.frameTimeDelta = std::min(std::max(in.frameMs, 0.f), 1000.f);
    p.reset = in.reset;
    p.cameraNear = in.camera.nearDist;
    p.cameraFar = in.camera.farDist;
    p.cameraFovAngleVertical = in.camera.fovY;
    p.viewSpaceToMetersFactor = 1.f;
    p.depth = resource(in.depth);
    p.motionVectors = resource(in.motion);
    for (int i = 0; i < 3; ++i)
    {
        p.cameraPosition[i] = in.camPos[i];
        p.cameraUp[i] = in.camUp[i];
        p.cameraRight[i] = in.camRight[i];
        p.cameraForward[i] = in.camFwd[i];
    }
    r = a.dispatch(&mCtx, &p.header);
    if (r != FFX_API_RETURN_OK)
    {
        err = std::string("FSR 4 frame generation's prepare was refused (") + resultText(r) + ")";
        return false;
    }
    ffxDispatchDescFrameGeneration g{};
    g.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    g.commandList = cmd;
    g.presentColor = resource(in.color);
    g.outputs[0] = resource(in.output);
    g.numGeneratedFrames = 1;
    g.reset = in.reset;
    g.backbufferTransferFunction = mHdr ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    g.minMaxLuminance[0] = 0.f;
    // (as FSR 3.1's: the display's peak, 1000 nits when not stated, within 80 .. 10000)
    g.minMaxLuminance[1] = std::min(std::max(in.peakNits > 0.f ? in.peakNits : 1000.f, 80.f), 10000.f);
    g.generationRect = FfxApiRect2D{0, 0, (int32_t)mDw, (int32_t)mDh};
    g.frameID = in.frameId;
    r = a.dispatch(&mCtx, &g.header);
    if (r != FFX_API_RETURN_OK)
    {
        err = std::string("FSR 4 frame generation was refused (") + resultText(r) + ")";
        return false;
    }
    return true;
}

void FrameGen::destroy()
{
    if (!mCtx) return;
    api().destroy(&mCtx, nullptr);
    mCtx = nullptr;
    mDw = mDh = mRw = mRh = 0;
    mFormat = VK_FORMAT_UNDEFINED;
    mVersion.clear();
}
} // namespace llffx
