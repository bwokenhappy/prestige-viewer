/*
 * slipstream_ngx.cpp -- the slip_nr C ABI (include/slipstream_ngx.h) implemented on NVIDIA NGX
 * (DLSS Ray Reconstruction, DLSS Super Resolution and DLSS Frame Generation, Vulkan).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The Slipstream Viewer contributors. See ../LICENSE.
 *
 * This file is original code written for this shim. It includes the NVIDIA DLSS SDK headers
 * and links the SDK's NGX loader (libnvsdk_ngx.a / nvsdk_ngx_*.lib) from the folder given
 * as DLSS_SDK_DIR; neither is part of this project (see README.md).
 */
#define SLIP_NR_BUILDING_LIBRARY 1
#include "slipstream_ngx.h"

#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_helpers_dlssd_vk.h"
#include "nvsdk_ngx_defs_dlssd.h"
// DLSS Frame Generation straight through NGX (slip_fg)
#include "nvsdk_ngx_defs_dlssg.h"
#include "nvsdk_ngx_params_dlssg.h"
#include "nvsdk_ngx_helpers_dlssg_vk.h"
#include "fg_caps.h"
#include "gpu_scheduling.h"
#include "last_error.h"
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#ifndef SLIP_NR_SDK_VERSION
#  define SLIP_NR_SDK_VERSION "unknown"
#endif
// 1.1.0: slip_nr_eval_info.transparency_layer / responsivity (the viewer only fills them for >= 1.1)
// 1.2.0: DLSS Super Resolution next to RR (slip_nr_create_info.feature, slip_nr_caps.super_resolution..,
//        slip_nr_optimal_render_size2); either feature may be missing (its own NVIDIA library)
// 1.3.0: DLSS Frame Generation (slip_fg: slip_nr_fg_caps / _create / _evaluate / _release), VK_NV_present_metering
//        and NGX's application id when an optional nvidia_app_id.txt sits next to the library
// 1.3.1: slip_nr_last_error per thread (the viewer's main thread and its present thread may fail at once);
//        Linux reads nvidia_app_id.txt too; DLSS Frame Generation's library installed on Linux too
// 1.3.2: slip_nr_fg_capabilities.unavailable_reason (NVIDIA's frame generation library missing, Windows' GPU
//        scheduling off, no answer from the runtime); every capability read checked; the slip_fg structs
//        accepted from their first release's size up (SLIP_NR_FG_*_SIZE_V1), a smaller one refused with a text
// 1.3.3: VK_NV_present_metering requested whenever frame generation's extensions are (the driver offers it
//        only in fullscreen; whether to ask it to pace is the caller's choice), not only with an application id
// 1.4.0: no application id: nvidia_app_id.txt is no longer read (the driver paces without one, in fullscreen);
//        NGX always gets the project id; slip_nr_fg_capabilities.application_id_set is always 0
// 1.4.1: frame generation: created with dynamic resolution (its depth / motion are the upscaler's render-size
//        inputs, which change with the DLSS mode while the feature lives; RenderWidth/Height were left 0); motion
//        vectors' invalid value is a sentinel the viewer never writes (0 marked every motionless pixel invalid)
#define SLIP_NR_LIBRARY_VERSION "1.4.1"

// Used when the application passes no project id (NGX wants a GUID-style string; this one
// identifies "a Slipstream build of this shim" with NVSDK_NGX_ENGINE_TYPE_CUSTOM).
static const char* kDefaultProjectId = "5d1f8a30-870c-4c0e-9b8e-51195e7a0870";

struct slip_nr_feature
{
    uint32_t             type = SLIP_NR_FEATURE_RAY_RECONSTRUCTION;   // slip_nr_feature_type
    NVSDK_NGX_Handle*    handle = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    bool                 ownParams = false;
    uint32_t             flags = 0;
};

namespace
{
std::mutex       g_logMutex;
slip_nr_log_fn   g_logFn = nullptr;
void*            g_logUser = nullptr;
// (the failure text: per thread, last_error.h)

bool                 g_init = false;
VkDevice             g_device = VK_NULL_HANDLE;
NVSDK_NGX_Parameter* g_caps = nullptr;

// Extension name storage (returned to the caller; valid until shutdown / unload).
std::vector<std::string> g_instNames, g_devNames, g_fgDevNames;
std::vector<const char*> g_instPtrs, g_devPtrs, g_fgDevPtrs;

// Path storage for NVSDK_NGX_FeatureCommonInfo (NGX may keep the pointers).
std::wstring              g_dataPath;
std::vector<std::wstring> g_paths;
std::vector<std::string>  g_pathsUtf8;   // (the same folders as the caller gave them: fgLibraryState)
std::vector<const wchar_t*> g_pathPtrs;
std::string               g_projectId, g_engineVersion;
NVSDK_NGX_FeatureCommonInfo g_common{};
// Why a feature's requirement query refused it (device_extensions; the caps reason uses it).
std::string               g_reqWhy[2];

void setError(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    slip_set_last_error(buf);
}

void log(uint32_t level, const char* msg)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFn) g_logFn(g_logUser, level, msg);
}

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature)
{
    log(level >= NVSDK_NGX_LOGGING_LEVEL_VERBOSE ? SLIP_NR_LOG_VERBOSE : SLIP_NR_LOG_INFO, message);
}

// UTF-8 -> wchar_t (UTF-32 where wchar_t is 4 bytes, UTF-16 on Windows).
std::wstring widen(const char* s)
{
    std::wstring out;
    if (!s) return out;
#if defined(_WIN32)
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n > 1)
    {
        out.resize((size_t)n - 1);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    }
#else
    const unsigned char* p = (const unsigned char*)s;
    while (*p)
    {
        uint32_t c = *p++;
        int extra = 0;
        if (c >= 0xF0) { c &= 0x07; extra = 3; }
        else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
        else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
        for (; extra > 0 && (*p & 0xC0) == 0x80; --extra) c = (c << 6) | (*p++ & 0x3F);
        out.push_back((wchar_t)c);
    }
#endif
    return out;
}

std::string narrow(const wchar_t* w)
{
    std::string out;
    if (!w) return out;
    for (; *w; ++w)
    {
        const uint32_t c = (uint32_t)*w;
        if (c < 0x80) out.push_back((char)c);
        else out.push_back('?');   // (diagnostic text only)
    }
    return out;
}

std::string resultText(NVSDK_NGX_Result r)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "0x%08x", (unsigned)r);
    std::string s = narrow(GetNGXResultAsString(r));
    return s.empty() ? std::string(buf) : s + " (" + buf + ")";
}

slip_nr_result mapResult(NVSDK_NGX_Result r)
{
    if (!NVSDK_NGX_FAILED(r)) return SLIP_NR_OK;
    switch (r)
    {
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
    case NVSDK_NGX_Result_FAIL_PlatformError:      return SLIP_NR_ERROR_UNSUPPORTED;
    case NVSDK_NGX_Result_FAIL_FeatureNotFound:    return SLIP_NR_ERROR_FEATURE_MISSING;
    case NVSDK_NGX_Result_FAIL_InvalidParameter:
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:
    case NVSDK_NGX_Result_FAIL_RWFlagMissing:
    case NVSDK_NGX_Result_FAIL_MissingInput:
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat:
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return SLIP_NR_ERROR_INVALID_ARGUMENT;
    case NVSDK_NGX_Result_FAIL_NotInitialized:     return SLIP_NR_ERROR_NOT_INITIALIZED;
    case NVSDK_NGX_Result_FAIL_OutOfDate:          return SLIP_NR_ERROR_DRIVER_TOO_OLD;
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:     return SLIP_NR_ERROR_OUT_OF_MEMORY;
    default:                                       return SLIP_NR_ERROR_FAILED;
    }
}

slip_nr_result fail(const char* what, NVSDK_NGX_Result r)
{
    setError("%s failed: %s", what, resultText(r).c_str());
    return mapResult(r);
}

// The app info -> NGX's common info + discovery info (stored in globals: NGX may keep them).
void applyAppInfo(const slip_nr_app_info* app)
{
    g_projectId = (app && app->project_id && *app->project_id) ? app->project_id : kDefaultProjectId;
    g_engineVersion = (app && app->engine_version) ? app->engine_version : "0";
    g_dataPath = widen(app ? app->app_data_path : nullptr);
    g_paths.clear();
    g_pathsUtf8.clear();
    g_pathPtrs.clear();
    if (app && app->feature_paths)
        for (uint32_t i = 0; i < app->feature_path_count; ++i)
            if (app->feature_paths[i] && *app->feature_paths[i])
            {
                g_paths.push_back(widen(app->feature_paths[i]));
                g_pathsUtf8.push_back(app->feature_paths[i]);
            }
    for (const std::wstring& p : g_paths) g_pathPtrs.push_back(p.c_str());
    g_common = NVSDK_NGX_FeatureCommonInfo{};
    g_common.PathListInfo.Path = g_pathPtrs.empty() ? nullptr : g_pathPtrs.data();
    g_common.PathListInfo.Length = (unsigned int)g_pathPtrs.size();
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_logFn = app ? app->log_fn : nullptr;
        g_logUser = app ? app->log_user : nullptr;
    }
    if (app && app->log_fn)
    {
        g_common.LoggingInfo.LoggingCallback = ngxLog;
        g_common.LoggingInfo.MinimumLoggingLevel = app->log_verbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE
                                                                    : NVSDK_NGX_LOGGING_LEVEL_ON;
        // The application's log only (NGX would also print every line to stdout).
        g_common.LoggingInfo.DisableOtherLoggingSinks = true;
    }
}

// ---- DLSS Frame Generation (slip_fg) ----
NVSDK_NGX_Handle* g_dfg = nullptr;
NVSDK_NGX_Parameter* g_dfgParams = nullptr;
// The device slip_nr_init was given (the GPU whose scheduling Windows is asked about).
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;
PFN_vkGetInstanceProcAddr g_getInstanceProcAddr = nullptr;

// NVIDIA's frame generation library is in one of the feature folders (the only places the caller told the
// runtime to look). UNKNOWN: no folder given, or none could be read.
slip_fg_state fgLibraryState()
{
    slip_fg_state s = SLIP_FG_STATE_UNKNOWN;
#ifdef _WIN32
    for (const std::wstring& dir : g_paths) s = slip_fg_merge_library(s, slip_fg_library_in(dir));
#else
    for (const std::string& dir : g_pathsUtf8) s = slip_fg_merge_library(s, slip_fg_library_in(dir.c_str()));
#endif
    return s;
}

// Windows' hardware-accelerated GPU scheduling for the device's GPU (gpu_scheduling.h). Elsewhere, and
// whenever the GPU's adapter cannot be told: UNKNOWN.
slip_fg_state fgGpuScheduling()
{
#ifdef _WIN32
    if (!g_getInstanceProcAddr || !g_instance || !g_physicalDevice) return SLIP_FG_STATE_UNKNOWN;
    const PFN_vkGetPhysicalDeviceProperties2 props2 =
        (PFN_vkGetPhysicalDeviceProperties2)g_getInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2");
    if (!props2) return SLIP_FG_STATE_UNKNOWN;
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 p{};
    p.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p.pNext = &id;
    props2(g_physicalDevice, &p);
    if (!id.deviceLUIDValid) return SLIP_FG_STATE_UNKNOWN;
    LUID adapter{};
    static_assert(sizeof(adapter) == VK_LUID_SIZE, "a LUID is VK_LUID_SIZE bytes");
    std::memcpy(&adapter, id.deviceLUID, sizeof(adapter));
    return slip_gpu_scheduling(adapter);
#else
    return SLIP_FG_STATE_UNKNOWN;
#endif
}
const char* featureName(uint32_t type)
{
    return type == SLIP_NR_FEATURE_SUPER_RESOLUTION ? "Super Resolution" : "Ray Reconstruction";
}

NVSDK_NGX_Feature ngxFeature(uint32_t type)
{
    return type == SLIP_NR_FEATURE_SUPER_RESOLUTION ? NVSDK_NGX_Feature_SuperSampling
                                                    : NVSDK_NGX_Feature_RayReconstruction;
}

NVSDK_NGX_FeatureDiscoveryInfo discoveryInfo(uint32_t type)
{
    NVSDK_NGX_FeatureDiscoveryInfo d{};
    d.SDKVersion = NVSDK_NGX_Version_API;
    d.FeatureID = ngxFeature(type);
    d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d.Identifier.v.ProjectDesc.ProjectId = g_projectId.c_str();
    d.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    d.Identifier.v.ProjectDesc.EngineVersion = g_engineVersion.c_str();
    d.ApplicationDataPath = g_dataPath.empty() ? L"." : g_dataPath.c_str();
    d.FeatureInfo = &g_common;
    return d;
}

// Appends (deduplicated); finishNames() then points ptrs at the strings.
void addNames(uint32_t n, const VkExtensionProperties* props, std::vector<std::string>& names)
{
    for (uint32_t i = 0; i < n && props; ++i)
    {
        const std::string e(props[i].extensionName);
        bool have = false;
        for (const std::string& x : names) have = have || x == e;
        if (!have) names.push_back(e);
    }
}

void finishNames(const std::vector<std::string>& names, std::vector<const char*>& ptrs)
{
    ptrs.clear();
    for (const std::string& s : names) ptrs.push_back(s.c_str());
}

void copyStr(char* dst, size_t cap, const char* src)
{
    std::snprintf(dst, cap, "%s", src ? src : "");
}

NVSDK_NGX_PerfQuality_Value perfQuality(slip_nr_quality q)
{
    switch (q)
    {
    case SLIP_NR_QUALITY_DLAA:              return NVSDK_NGX_PerfQuality_Value_DLAA;
    case SLIP_NR_QUALITY_QUALITY:           return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case SLIP_NR_QUALITY_BALANCED:          return NVSDK_NGX_PerfQuality_Value_Balanced;
    case SLIP_NR_QUALITY_PERFORMANCE:       return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case SLIP_NR_QUALITY_ULTRA_PERFORMANCE: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default:                                return NVSDK_NGX_PerfQuality_Value_Balanced;
    }
}

const char* presetParam(uint32_t type, slip_nr_quality q)
{
    if (type == SLIP_NR_FEATURE_SUPER_RESOLUTION)
    {
        switch (q)
        {
        case SLIP_NR_QUALITY_DLAA:              return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
        case SLIP_NR_QUALITY_QUALITY:           return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
        case SLIP_NR_QUALITY_BALANCED:          return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
        case SLIP_NR_QUALITY_PERFORMANCE:       return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
        case SLIP_NR_QUALITY_ULTRA_PERFORMANCE: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
        default:                                return nullptr;
        }
    }
    switch (q)
    {
    case SLIP_NR_QUALITY_DLAA:              return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA;
    case SLIP_NR_QUALITY_QUALITY:           return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality;
    case SLIP_NR_QUALITY_BALANCED:          return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced;
    case SLIP_NR_QUALITY_PERFORMANCE:       return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance;
    case SLIP_NR_QUALITY_ULTRA_PERFORMANCE: return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance;
    default:                                return nullptr;
    }
}

// slip_nr_image -> NGX's Vulkan resource wrapper (nullptr when not provided).
NVSDK_NGX_Resource_VK* wrap(const slip_nr_image& im, NVSDK_NGX_Resource_VK& storage, bool writable)
{
    if (!im.image || !im.view) return nullptr;
    VkImageSubresourceRange r{};
    r.aspectMask = im.aspect ? im.aspect : VK_IMAGE_ASPECT_COLOR_BIT;
    r.baseMipLevel = 0; r.levelCount = 1;
    r.baseArrayLayer = 0; r.layerCount = 1;
    storage = NVSDK_NGX_Create_ImageView_Resource_VK(im.view, im.image, r, im.format, im.width, im.height, writable);
    return &storage;
}
} // namespace

extern "C" {

SLIP_NR_API uint32_t SLIP_NR_CALL slip_nr_abi_version(void)
{
    return SLIP_NR_ABI_VERSION;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_get_info(slip_nr_library_info* out)
{
    if (!out || out->struct_size < sizeof(slip_nr_library_info)) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    out->abi_version = SLIP_NR_ABI_VERSION;
    copyStr(out->backend, sizeof(out->backend), "NVIDIA NGX");
    copyStr(out->sdk_version, sizeof(out->sdk_version), SLIP_NR_SDK_VERSION);
    copyStr(out->library_version, sizeof(out->library_version), SLIP_NR_LIBRARY_VERSION);
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_instance_extensions(const slip_nr_app_info* app,
                                                                    uint32_t* count, const char* const** names)
{
    if (!count || !names) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    *count = 0; *names = nullptr;
    applyAppInfo(app);
    // Both features' extensions (union): either one alone is enough to go on.
    g_instNames.clear();
    bool any = false;
    NVSDK_NGX_Result firstFail = NVSDK_NGX_Result_Success;
    for (uint32_t type : {(uint32_t)SLIP_NR_FEATURE_RAY_RECONSTRUCTION, (uint32_t)SLIP_NR_FEATURE_SUPER_RESOLUTION})
    {
        NVSDK_NGX_FeatureDiscoveryInfo d = discoveryInfo(type);
        uint32_t n = 0;
        VkExtensionProperties* props = nullptr;
        const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&d, &n, &props);
        if (NVSDK_NGX_FAILED(r))
        {
            if (firstFail == NVSDK_NGX_Result_Success) firstFail = r;
            continue;
        }
        any = true;
        addNames(n, props, g_instNames);
    }
    if (!any) return fail("NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements", firstFail);
    finishNames(g_instNames, g_instPtrs);
    *count = (uint32_t)g_instPtrs.size();
    *names = g_instPtrs.empty() ? nullptr : g_instPtrs.data();
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_device_extensions(const slip_nr_app_info* app, VkInstance instance,
                                                                  VkPhysicalDevice physical_device,
                                                                  uint32_t* count, const char* const** names)
{
    if (!count || !names || !instance || !physical_device) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    *count = 0; *names = nullptr;
    applyAppInfo(app);
    // Per feature: supported on this GPU / OS / driver at all? The union of the supported ones'
    // extensions; fails only when neither is supported (the error names both reasons).
    g_devNames.clear();
    bool any = false, driverOld = false;
    std::string why;
    for (uint32_t type : {(uint32_t)SLIP_NR_FEATURE_RAY_RECONSTRUCTION, (uint32_t)SLIP_NR_FEATURE_SUPER_RESOLUTION})
    {
        g_reqWhy[type].clear();
        NVSDK_NGX_FeatureDiscoveryInfo d = discoveryInfo(type);
        NVSDK_NGX_FeatureRequirement req{};
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical_device, &d, &req);
        char buf[256];
        if (NVSDK_NGX_FAILED(r))
        {
            std::snprintf(buf, sizeof(buf), "%s: the requirement query failed: %s", featureName(type),
                          resultText(r).c_str());
            g_reqWhy[type] = buf;
        }
        else if (req.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported)
        {
            std::snprintf(buf, sizeof(buf), "%s is not supported here (support flags 0x%x, minimum GPU architecture "
                          "0x%x, minimum OS %s)", featureName(type), (unsigned)req.FeatureSupported,
                          req.MinHWArchitecture, req.MinOSVersion);
            g_reqWhy[type] = buf;
            if (req.FeatureSupported & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) driverOld = true;
        }
        else
        {
            uint32_t n = 0;
            VkExtensionProperties* props = nullptr;
            r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical_device, &d, &n, &props);
            if (NVSDK_NGX_FAILED(r))
            {
                std::snprintf(buf, sizeof(buf), "%s: the device extension query failed: %s", featureName(type),
                              resultText(r).c_str());
                g_reqWhy[type] = buf;
            }
            else
            {
                any = true;
                addNames(n, props, g_devNames);
            }
        }
        if (!g_reqWhy[type].empty()) why += (why.empty() ? "" : "; ") + g_reqWhy[type];
    }
    if (!any)
    {
        setError("%s", why.c_str());
        return driverOld ? SLIP_NR_ERROR_DRIVER_TOO_OLD : SLIP_NR_ERROR_UNSUPPORTED;
    }
    finishNames(g_devNames, g_devPtrs);
    *count = (uint32_t)g_devPtrs.size();
    *names = g_devPtrs.empty() ? nullptr : g_devPtrs.data();
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_init(const slip_nr_app_info* app, const slip_nr_vulkan_device* dev)
{
    if (g_init) return SLIP_NR_OK;
    if (!app || !dev || dev->struct_size < sizeof(slip_nr_vulkan_device) || !dev->instance || !dev->physical_device
        || !dev->device || !app->app_data_path || !*app->app_data_path)
    {
        setError("slip_nr_init: missing instance / device / app_data_path");
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    }
    applyAppInfo(app);
    // Over-the-air model updates are never requested (NVSDK_NGX_UpdateFeature is not called).
    const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        g_projectId.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, g_engineVersion.c_str(), g_dataPath.c_str(),
        dev->instance, dev->physical_device, dev->device, dev->get_instance_proc_addr, dev->get_device_proc_addr,
        &g_common, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) return fail("NVSDK_NGX_VULKAN_Init_with_ProjectID", r);
    g_init = true;
    g_device = dev->device;
    g_instance = dev->instance;
    g_physicalDevice = dev->physical_device;
    g_getInstanceProcAddr = dev->get_instance_proc_addr;
    const NVSDK_NGX_Result c = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_caps);
    if (NVSDK_NGX_FAILED(c))
    {
        g_caps = nullptr;
        return fail("NVSDK_NGX_VULKAN_GetCapabilityParameters", c);
    }
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_get_caps(slip_nr_caps* out)
{
    // (An ABI-1 caller from before 1.2 passes the shorter struct: only what fits is written.)
    if (!out || out->struct_size < SLIP_NR_CAPS_SIZE_V1) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    const uint32_t size = out->struct_size;
    const bool v12 = size >= sizeof(slip_nr_caps);
    std::memset(out, 0, v12 ? sizeof(slip_nr_caps) : SLIP_NR_CAPS_SIZE_V1);
    out->struct_size = size;
    if (!g_init || !g_caps)
    {
        copyStr(out->reason, sizeof(out->reason), "NGX is not initialised");
        if (v12) copyStr(out->sr_reason, sizeof(out->sr_reason), "NGX is not initialised");
        return SLIP_NR_ERROR_NOT_INITIALIZED;
    }
    struct One { uint32_t avail, needs, maj, minr, init; };
    auto query = [](const char* pAvail, const char* pNeeds, const char* pMaj, const char* pMin, const char* pInit)
    {
        int avail = 0, needs = 0, initRes = 0;
        unsigned int maj = 0, minr = 0;
        NVSDK_NGX_Parameter_GetI(g_caps, pAvail, &avail);
        NVSDK_NGX_Parameter_GetI(g_caps, pNeeds, &needs);
        NVSDK_NGX_Parameter_GetUI(g_caps, pMaj, &maj);
        NVSDK_NGX_Parameter_GetUI(g_caps, pMin, &minr);
        NVSDK_NGX_Parameter_GetI(g_caps, pInit, &initRes);
        return One{avail ? 1u : 0u, needs ? 1u : 0u, maj, minr, (uint32_t)initRes};
    };
    auto reason = [](uint32_t type, const One& o, char* dst, size_t cap)
    {
        if (o.avail) return;
        if (o.needs)
            std::snprintf(dst, cap, "the NVIDIA driver is too old for %s (needs %u.%u or newer)", featureName(type),
                          o.maj, o.minr);
        else if (!g_reqWhy[type].empty())
            std::snprintf(dst, cap, "%s", g_reqWhy[type].c_str());
        else
            std::snprintf(dst, cap, "%s is not available (feature init %s; is its NVIDIA library, %s, installed?)",
                          featureName(type), resultText((NVSDK_NGX_Result)o.init).c_str(),
#ifdef _WIN32
                          type == SLIP_NR_FEATURE_SUPER_RESOLUTION ? "nvngx_dlss" : "nvngx_dlssd");
#else
                          type == SLIP_NR_FEATURE_SUPER_RESOLUTION ? "libnvidia-ngx-dlss.so" : "libnvidia-ngx-dlssd.so");
#endif
    };
    const One rr = query(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,
                         NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,
                         NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor,
                         NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor,
                         NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult);
    out->ray_reconstruction = rr.avail;
    out->needs_newer_driver = rr.needs;
    out->min_driver_major = rr.maj;
    out->min_driver_minor = rr.minr;
    out->vendor_init_result = rr.init;
    reason(SLIP_NR_FEATURE_RAY_RECONSTRUCTION, rr, out->reason, sizeof(out->reason));
    if (v12)
    {
        const One sr = query(NVSDK_NGX_Parameter_SuperSampling_Available,
                             NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                             NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor,
                             NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor,
                             NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult);
        out->super_resolution = sr.avail;
        out->sr_needs_newer_driver = sr.needs;
        out->sr_min_driver_major = sr.maj;
        out->sr_min_driver_minor = sr.minr;
        out->sr_vendor_init_result = sr.init;
        reason(SLIP_NR_FEATURE_SUPER_RESOLUTION, sr, out->sr_reason, sizeof(out->sr_reason));
    }
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_optimal_render_size(slip_nr_quality quality,
                                                                    uint32_t display_width, uint32_t display_height,
                                                                    uint32_t* render_width, uint32_t* render_height,
                                                                    uint32_t* min_width, uint32_t* min_height,
                                                                    uint32_t* max_width, uint32_t* max_height)
{
    return slip_nr_optimal_render_size2(SLIP_NR_FEATURE_RAY_RECONSTRUCTION, quality, display_width, display_height,
                                        render_width, render_height, min_width, min_height, max_width, max_height);
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_optimal_render_size2(uint32_t feature, slip_nr_quality quality,
                                                                     uint32_t display_width, uint32_t display_height,
                                                                     uint32_t* render_width, uint32_t* render_height,
                                                                     uint32_t* min_width, uint32_t* min_height,
                                                                     uint32_t* max_width, uint32_t* max_height)
{
    if (!render_width || !render_height || !display_width || !display_height
        || (feature != SLIP_NR_FEATURE_RAY_RECONSTRUCTION && feature != SLIP_NR_FEATURE_SUPER_RESOLUTION))
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    if (!g_init || !g_caps) return SLIP_NR_ERROR_NOT_INITIALIZED;
    void* cb = nullptr;
    NVSDK_NGX_Parameter_GetVoidPointer(g_caps, feature == SLIP_NR_FEATURE_SUPER_RESOLUTION
                                                   ? NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback
                                                   : NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, &cb);
    if (!cb)
    {
        setError("the installed %s library has no optimal-settings callback (missing or out of date?)",
                 featureName(feature));
        return SLIP_NR_ERROR_DRIVER_TOO_OLD;
    }
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Width, display_width);
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Height, display_height);
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_PerfQualityValue, perfQuality(quality));
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_RTXValue, 0);
    const NVSDK_NGX_Result r = ((PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)cb)(g_caps);
    if (NVSDK_NGX_FAILED(r)) return fail("the optimal-settings callback", r);
    unsigned int w = 0, h = 0, mnw = 0, mnh = 0, mxw = 0, mxh = 0;
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutWidth, &w);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutHeight, &h);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &mnw);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &mnh);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &mxw);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &mxh);
    if (!w || !h)
    {
        setError("the optimal-settings callback returned no render size for mode %u", (unsigned)quality);
        return SLIP_NR_ERROR_UNSUPPORTED;
    }
    *render_width = w; *render_height = h;
    if (min_width) *min_width = mnw ? mnw : w;
    if (min_height) *min_height = mnh ? mnh : h;
    if (max_width) *max_width = mxw ? mxw : w;
    if (max_height) *max_height = mxh ? mxh : h;
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_create(VkCommandBuffer cmd, const slip_nr_create_info* info,
                                                       slip_nr_feature** out)
{
    if (!cmd || !info || !out || info->struct_size < SLIP_NR_CREATE_INFO_SIZE_V1 || !info->render_width
        || !info->render_height || !info->display_width || !info->display_height)
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!g_init) return SLIP_NR_ERROR_NOT_INITIALIZED;
    // (a caller from before 1.2 has no feature field: Ray Reconstruction, the only feature then)
    const uint32_t type = info->struct_size >= sizeof(slip_nr_create_info) ? info->feature
                                                                           : (uint32_t)SLIP_NR_FEATURE_RAY_RECONSTRUCTION;
    if (type != SLIP_NR_FEATURE_RAY_RECONSTRUCTION && type != SLIP_NR_FEATURE_SUPER_RESOLUTION)
    {
        setError("slip_nr_create: unknown feature %u", type);
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    }

    slip_nr_feature* f = new slip_nr_feature();
    f->type = type;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_AllocateParameters(&f->params);
    if (NVSDK_NGX_FAILED(r))
    {
        // (The SDK's GetParameters fallback for very old drivers is deprecated; such a driver
        // can't run these features anyway.)
        delete f;
        return fail("NVSDK_NGX_VULKAN_AllocateParameters", r);
    }
    f->ownParams = true;
    f->flags = info->flags;

    NVSDK_NGX_Parameter_SetUI(f->params, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
    if (info->preset)
    {
        const char* p = presetParam(type, info->quality);
        if (p) NVSDK_NGX_Parameter_SetUI(f->params, p, info->preset);
    }
    int ff = NVSDK_NGX_DLSS_Feature_Flags_None;
    if (info->flags & SLIP_NR_CREATE_HDR)            ff |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (info->flags & SLIP_NR_CREATE_DEPTH_INVERTED) ff |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (info->flags & SLIP_NR_CREATE_MV_RENDER_RES)  ff |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (info->flags & SLIP_NR_CREATE_MV_JITTERED)    ff |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;

    if (type == SLIP_NR_FEATURE_SUPER_RESOLUTION)
    {
        if (info->flags & SLIP_NR_CREATE_AUTO_EXPOSURE) ff |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        NVSDK_NGX_DLSS_Create_Params sp{};
        sp.Feature.InWidth = info->render_width;
        sp.Feature.InHeight = info->render_height;
        sp.Feature.InTargetWidth = info->display_width;
        sp.Feature.InTargetHeight = info->display_height;
        sp.Feature.InPerfQualityValue = perfQuality(info->quality);
        sp.InFeatureCreateFlags = ff;
        sp.InEnableOutputSubrects = false;
        r = NGX_VULKAN_CREATE_DLSS_EXT1(g_device, cmd, 1, 1, &f->handle, f->params, &sp);
        if (NVSDK_NGX_FAILED(r) || !f->handle)
        {
            const slip_nr_result e = fail("Super Resolution feature creation", r);
            if (f->ownParams) NVSDK_NGX_VULKAN_DestroyParameters(f->params);
            delete f;
            return e == SLIP_NR_OK ? SLIP_NR_ERROR_FAILED : e;
        }
        *out = f;
        return SLIP_NR_OK;
    }

    NVSDK_NGX_DLSSD_Create_Params cp{};
    cp.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    cp.InRoughnessMode = (info->flags & SLIP_NR_CREATE_ROUGHNESS_PACKED) ? NVSDK_NGX_DLSS_Roughness_Mode_Packed
                                                                         : NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
    cp.InUseHWDepth = (info->flags & SLIP_NR_CREATE_HW_DEPTH) ? NVSDK_NGX_DLSS_Depth_Type_HW
                                                              : NVSDK_NGX_DLSS_Depth_Type_Linear;
    cp.InWidth = info->render_width;
    cp.InHeight = info->render_height;
    cp.InTargetWidth = info->display_width;
    cp.InTargetHeight = info->display_height;
    cp.InPerfQualityValue = perfQuality(info->quality);
    cp.InFeatureCreateFlags = ff;
    cp.InEnableOutputSubrects = false;
    r = NGX_VULKAN_CREATE_DLSSD_EXT1(g_device, cmd, 1, 1, &f->handle, f->params, &cp);
    if (NVSDK_NGX_FAILED(r) || !f->handle)
    {
        const slip_nr_result e = fail("Ray Reconstruction feature creation", r);
        if (f->ownParams) NVSDK_NGX_VULKAN_DestroyParameters(f->params);
        delete f;
        return e == SLIP_NR_OK ? SLIP_NR_ERROR_FAILED : e;
    }
    *out = f;
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_evaluate(VkCommandBuffer cmd, slip_nr_feature* f,
                                                         const slip_nr_eval_info* info)
{
    if (!cmd || !f || !f->handle || !info || info->struct_size < SLIP_NR_EVAL_INFO_SIZE_V1)
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    const bool v11 = info->struct_size >= offsetof(slip_nr_eval_info, responsivity) + sizeof(slip_nr_image);
    if (f->type == SLIP_NR_FEATURE_SUPER_RESOLUTION)
    {
        if (!info->color.image || !info->output.image || !info->depth.image || !info->motion.image)
        {
            setError("slip_nr_evaluate (Super Resolution): a required image is missing");
            return SLIP_NR_ERROR_INVALID_ARGUMENT;
        }
        NVSDK_NGX_Resource_VK rColor, rOut, rDepth, rMv, rExp, rBias;
        NVSDK_NGX_VK_DLSS_Eval_Params e;
        std::memset(&e, 0, sizeof(e));
        e.Feature.pInColor = wrap(info->color, rColor, false);
        e.Feature.pInOutput = wrap(info->output, rOut, true);
        e.pInDepth = wrap(info->depth, rDepth, false);
        e.pInMotionVectors = wrap(info->motion, rMv, false);
        e.pInExposureTexture = wrap(info->exposure, rExp, false);
        if (v11) e.pInBiasCurrentColorMask = wrap(info->responsivity, rBias, false);
        e.InJitterOffsetX = info->jitter_x;
        e.InJitterOffsetY = info->jitter_y;
        e.InRenderSubrectDimensions.Width = info->render_width ? info->render_width : info->color.width;
        e.InRenderSubrectDimensions.Height = info->render_height ? info->render_height : info->color.height;
        e.InReset = info->reset ? 1 : 0;
        e.InMVScaleX = info->mv_scale_x != 0.f ? info->mv_scale_x : 1.f;
        e.InMVScaleY = info->mv_scale_y != 0.f ? info->mv_scale_y : 1.f;
        e.InPreExposure = info->pre_exposure != 0.f ? info->pre_exposure : 1.f;
        e.InExposureScale = 1.f;
        e.InFrameTimeDeltaInMsec = info->frame_time_ms;
        const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, f->handle, f->params, &e);
        if (NVSDK_NGX_FAILED(r)) return fail("Super Resolution evaluate", r);
        return SLIP_NR_OK;
    }
    if (!info->color.image || !info->output.image || !info->depth.image || !info->motion.image
        || !info->diffuse_albedo.image || !info->specular_albedo.image || !info->normals.image)
    {
        setError("slip_nr_evaluate: a required image is missing");
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    }
    NVSDK_NGX_Resource_VK rColor, rOut, rDepth, rMv, rDiff, rSpec, rNorm, rRough, rHit, rExp, rBefore, rTransp, rResp;
    NVSDK_NGX_VK_DLSSD_Eval_Params e;
    std::memset(&e, 0, sizeof(e));
    e.pInColor = wrap(info->color, rColor, false);
    e.pInOutput = wrap(info->output, rOut, true);
    e.pInDepth = wrap(info->depth, rDepth, false);
    e.pInMotionVectors = wrap(info->motion, rMv, false);
    e.pInDiffuseAlbedo = wrap(info->diffuse_albedo, rDiff, false);
    e.pInSpecularAlbedo = wrap(info->specular_albedo, rSpec, false);
    e.pInNormals = wrap(info->normals, rNorm, false);
    if (!(f->flags & SLIP_NR_CREATE_ROUGHNESS_PACKED)) e.pInRoughness = wrap(info->roughness, rRough, false);
    e.pInSpecularHitDistance = wrap(info->specular_hit_distance, rHit, false);
    e.pInExposureTexture = wrap(info->exposure, rExp, false);
    e.pInColorBeforeTransparency = wrap(info->color_before_transparency, rBefore, false);
    if (v11)
    {
        e.pInTransparencyLayer = wrap(info->transparency_layer, rTransp, false);
        e.pInResponsivityMask = wrap(info->responsivity, rResp, false);
    }
    e.InJitterOffsetX = info->jitter_x;
    e.InJitterOffsetY = info->jitter_y;
    e.InRenderSubrectDimensions.Width = info->render_width ? info->render_width : info->color.width;
    e.InRenderSubrectDimensions.Height = info->render_height ? info->render_height : info->color.height;
    e.InReset = info->reset ? 1 : 0;
    e.InMVScaleX = info->mv_scale_x != 0.f ? info->mv_scale_x : 1.f;
    e.InMVScaleY = info->mv_scale_y != 0.f ? info->mv_scale_y : 1.f;
    e.InPreExposure = info->pre_exposure != 0.f ? info->pre_exposure : 1.f;
    e.InExposureScale = 1.f;
    e.InFrameTimeDeltaInMsec = info->frame_time_ms;
    // (NGX takes non-const pointers; it only reads them.)
    float w2v[16], v2c[16];
    std::memcpy(w2v, info->world_to_view, sizeof(w2v));
    std::memcpy(v2c, info->view_to_clip, sizeof(v2c));
    if (e.pInSpecularHitDistance)
    {
        e.pInWorldToViewMatrix = w2v;
        e.pInViewToClipMatrix = v2c;
    }
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd, f->handle, f->params, &e);
    if (NVSDK_NGX_FAILED(r)) return fail("Ray Reconstruction evaluate", r);
    return SLIP_NR_OK;
}

SLIP_NR_API void SLIP_NR_CALL slip_nr_destroy(slip_nr_feature* f)
{
    if (!f) return;
    if (f->handle) NVSDK_NGX_VULKAN_ReleaseFeature(f->handle);
    if (f->params && f->ownParams) NVSDK_NGX_VULKAN_DestroyParameters(f->params);
    delete f;
}

SLIP_NR_API void SLIP_NR_CALL slip_nr_shutdown(void)
{
    if (g_init)
    {
        if (g_dfg) NVSDK_NGX_VULKAN_ReleaseFeature(g_dfg);
        g_dfg = nullptr;
        if (g_dfgParams) NVSDK_NGX_VULKAN_DestroyParameters(g_dfgParams);
        g_dfgParams = nullptr;
        if (g_caps) NVSDK_NGX_VULKAN_DestroyParameters(g_caps);
        g_caps = nullptr;
        NVSDK_NGX_VULKAN_Shutdown1(g_device);
    }
    g_init = false;
    g_device = VK_NULL_HANDLE;
    g_instance = VK_NULL_HANDLE;
    g_physicalDevice = VK_NULL_HANDLE;
    g_getInstanceProcAddr = nullptr;
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logFn = nullptr;
    g_logUser = nullptr;
}

SLIP_NR_API const char* SLIP_NR_CALL slip_nr_last_error(void)
{
    return slip_last_error_text();   // (the calling thread's: last_error.h)
}

// ---- DLSS Frame Generation (slip_fg) ----
// The exported names are the header's SLIP_FG_FN_* (a renamed definition must fail to build).
static_assert(sizeof(SLIP_FG_FN_DEVICE_EXTENSIONS) == sizeof("slip_nr_fg_device_extensions"), "SLIP_FG_FN_DEVICE_EXTENSIONS");
static_assert(sizeof(SLIP_FG_FN_CAPS) == sizeof("slip_nr_fg_caps"), "SLIP_FG_FN_CAPS");
static_assert(sizeof(SLIP_FG_FN_CREATE) == sizeof("slip_nr_fg_create"), "SLIP_FG_FN_CREATE");
static_assert(sizeof(SLIP_FG_FN_EVALUATE) == sizeof("slip_nr_fg_evaluate"), "SLIP_FG_FN_EVALUATE");
static_assert(sizeof(SLIP_FG_FN_RELEASE) == sizeof("slip_nr_fg_release"), "SLIP_FG_FN_RELEASE");
SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_fg_device_extensions(const slip_nr_app_info* app, VkInstance instance,
                                                                     VkPhysicalDevice physical_device,
                                                                     uint32_t* count, const char* const** names)
{
    if (!count || !names || !instance || !physical_device) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    *count = 0; *names = nullptr;
    applyAppInfo(app);
    // Frame generation's own device extensions (when the driver reports its requirements), and present
    // metering (enabled only where the device lists it; whether the driver is asked to pace is the caller's).
    g_fgDevNames.clear();
    NVSDK_NGX_FeatureDiscoveryInfo d = discoveryInfo(SLIP_NR_FEATURE_SUPER_RESOLUTION);
    d.FeatureID = NVSDK_NGX_Feature_FrameGeneration;
    NVSDK_NGX_FeatureRequirement req{};
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical_device, &d, &req);
    if (NVSDK_NGX_FAILED(r)) return fail("NVSDK_NGX_VULKAN_GetFeatureRequirements (frame generation)", r);
    uint32_t n = 0;
    VkExtensionProperties* props = nullptr;
    r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical_device, &d, &n, &props);
    if (NVSDK_NGX_FAILED(r)) return fail("NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements (frame generation)", r);
    addNames(n, props, g_fgDevNames);
    {
        VkExtensionProperties metering{};
        copyStr(metering.extensionName, sizeof(metering.extensionName), "VK_NV_present_metering");
        addNames(1, &metering, g_fgDevNames);
    }
    finishNames(g_fgDevNames, g_fgDevPtrs);
    *count = (uint32_t)g_fgDevPtrs.size();
    *names = g_fgDevPtrs.empty() ? nullptr : g_fgDevPtrs.data();
    return SLIP_NR_OK;
}

// The reasons of the ABI header are fg_caps.h's (the rules there are tested without the ABI header).
static_assert(SLIP_FG_WHY_NOT_STATED == SLIP_FG_REASON_NOT_STATED, "SLIP_FG_WHY_NOT_STATED");
static_assert(SLIP_FG_WHY_LIBRARY_MISSING == SLIP_FG_REASON_LIBRARY_MISSING, "SLIP_FG_WHY_LIBRARY_MISSING");
static_assert(SLIP_FG_WHY_GPU_SCHEDULING_OFF == SLIP_FG_REASON_GPU_SCHEDULING_OFF, "SLIP_FG_WHY_GPU_SCHEDULING_OFF");
static_assert(SLIP_FG_WHY_QUERY_FAILED == SLIP_FG_REASON_QUERY_FAILED, "SLIP_FG_WHY_QUERY_FAILED");

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_fg_caps(slip_nr_fg_capabilities* out)
{
    // (A caller from before 1.3.2 passes the shorter struct: only what fits is written. Smaller than the
    // first release's is not a caller of this ABI.)
    if (!out || !slip_fg_size_ok(out->struct_size, SLIP_NR_FG_CAPABILITIES_SIZE_V1))
    {
        setError("slip_nr_fg_caps: no struct, or one smaller than the first release's (%u bytes)",
                 (unsigned)SLIP_NR_FG_CAPABILITIES_SIZE_V1);
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    }
    if (!g_init || !g_caps)
    {
        setError("slip_nr_fg_caps: NGX is not initialised");
        return SLIP_NR_ERROR_NOT_INITIALIZED;
    }
    // Every read is checked: a read that failed is "no answer", never a silent "no".
    int avail = 0;
    unsigned int maxMulti = 0, needsDriver = 0, minMaj = 0, minMin = 0;
    slip_fg_query q;
    q.availableRead = NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_FrameGeneration_Available, &avail));
    q.available = q.availableRead && avail != 0;
    q.maxRead = NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &maxMulti));
    q.maxGenerated = q.maxRead ? maxMulti : 0;
    const bool needsRead =
        NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &needsDriver));
    q.needsNewerDriver = needsRead && needsDriver != 0;
    // (the oldest driver that runs it: stated only when both parts were read)
    const bool minRead =
        NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &minMaj))
        && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &minMin));
    q.gpuScheduling = fgGpuScheduling();
    // (the folders are only searched when the answer is "no": the runtime may find the library elsewhere)
    q.library = q.available ? SLIP_FG_STATE_UNKNOWN : fgLibraryState();
    const slip_fg_answer a = slip_fg_decide(q);
    slip_fg_write_caps(out, out->struct_size, a, q.needsNewerDriver, minRead ? minMaj : 0u, minRead ? minMin : 0u);
    if (!q.availableRead) setError("slip_nr_fg_caps: NGX did not answer whether frame generation is available");
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_fg_create(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                                                          uint32_t format)
{
    if (!g_init) return SLIP_NR_ERROR_NOT_INITIALIZED;
    if (g_dfg) return SLIP_NR_OK;
    if (!cmd || !width || !height) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    if (!g_dfgParams)
    {
        const NVSDK_NGX_Result a = NVSDK_NGX_VULKAN_AllocateParameters(&g_dfgParams);
        if (NVSDK_NGX_FAILED(a)) { g_dfgParams = nullptr; return fail("NVSDK_NGX_VULKAN_AllocateParameters (frame generation)", a); }
    }
    NVSDK_NGX_DLSSG_Create_Params cp{};
    cp.Width = width;
    cp.Height = height;
    cp.NativeBackbufferFormat = format;
    // The depth and motion vectors are the upscaler's render-size inputs: their size follows the DLSS mode while
    // this feature (made per output size) lives, so dynamic resolution (the internal size is then ignored). Set on
    // the parameter map: the create helper forwards only the sizes and the format.
    cp.DynamicResolutionScaling = true;
    NVSDK_NGX_Parameter_SetUI(g_dfgParams, NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 1u);
    const NVSDK_NGX_Result r = NGX_VK_CREATE_DLSSG(cmd, 1, 1, &g_dfg, g_dfgParams, &cp);
    if (NVSDK_NGX_FAILED(r) || !g_dfg)
    {
        g_dfg = nullptr;
        return fail("NGX_VK_CREATE_DLSSG", r);
    }
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_fg_evaluate(VkCommandBuffer cmd, const slip_nr_fg_eval* e)
{
    if (!g_init || !g_dfg || !g_dfgParams) return SLIP_NR_ERROR_NOT_INITIALIZED;
    // (Only the first release's fields are read: any struct from that size up is accepted.)
    if (!cmd || !e || !slip_fg_size_ok(e->struct_size, SLIP_NR_FG_EVAL_SIZE_V1))
    {
        setError("slip_nr_fg_evaluate: no command buffer or struct, or a struct smaller than the first release's (%u bytes)",
                 (unsigned)SLIP_NR_FG_EVAL_SIZE_V1);
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    }
    NVSDK_NGX_Resource_VK color, depth, motion, output;
    NVSDK_NGX_VK_DLSSG_Eval_Params ep{};
    ep.pBackbuffer = wrap(e->color, color, false);
    ep.pDepth = wrap(e->depth, depth, false);
    ep.pMVecs = wrap(e->motion, motion, false);
    ep.pOutputInterpFrame = wrap(e->output, output, true);
    if (!ep.pBackbuffer || !ep.pDepth || !ep.pMVecs || !ep.pOutputInterpFrame) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    NVSDK_NGX_DLSSG_Opt_Eval_Params c{};
    std::memcpy(c.cameraViewToClip, e->view_to_clip, sizeof(c.cameraViewToClip));
    std::memcpy(c.clipToCameraView, e->clip_to_view, sizeof(c.clipToCameraView));
    std::memcpy(c.clipToPrevClip, e->clip_to_prev_clip, sizeof(c.clipToPrevClip));
    std::memcpy(c.prevClipToClip, e->prev_clip_to_clip, sizeof(c.prevClipToClip));
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) c.clipToLensClip[i][j] = i == j ? 1.f : 0.f;
    for (int i = 0; i < 3; ++i)
    {
        c.cameraPos[i] = e->camera_pos[i];
        c.cameraUp[i] = e->camera_up[i];
        c.cameraRight[i] = e->camera_right[i];
        c.cameraFwd[i] = e->camera_fwd[i];
    }
    for (int i = 0; i < 2; ++i)
    {
        c.jitterOffset[i] = e->jitter[i];
        c.mvecScale[i] = e->mvec_scale[i];
        c.cameraPinholeOffset[i] = 0.f;
    }
    c.cameraNear = e->camera_near;
    c.cameraFar = e->camera_far;
    c.cameraFOV = e->camera_fov;
    c.cameraAspectRatio = e->camera_aspect;
    c.colorBuffersHDR = e->color_hdr != 0;
    c.depthInverted = e->depth_inverted != 0;
    c.cameraMotionIncluded = true;
    c.reset = e->reset != 0;
    c.notRenderingGameFrames = false;
    c.orthoProjection = false;
    // ("which value represents an invalid (un-initialized) value in the motion vectors buffer": the viewer writes
    // none -- every pixel has its motion, a motionless one 0 -- so a value its RG16F vectors can never hold.)
    c.motionVectorsInvalidValue = (std::numeric_limits<float>::max)();   // (parenthesised: windows.h max macro)
    c.motionVectorsDilated = false;
    c.multiFrameCount = e->frame_count ? e->frame_count : 1;
    c.multiFrameIndex = e->frame_index ? e->frame_index : 1;
    const NVSDK_NGX_Result r = NGX_VK_EVALUATE_DLSSG(cmd, g_dfg, g_dfgParams, &ep, &c);
    if (NVSDK_NGX_FAILED(r)) return fail("NGX_VK_EVALUATE_DLSSG", r);
    return SLIP_NR_OK;
}

SLIP_NR_API void SLIP_NR_CALL slip_nr_fg_release(void)
{
    if (g_dfg) NVSDK_NGX_VULKAN_ReleaseFeature(g_dfg);
    g_dfg = nullptr;
    if (g_dfgParams) NVSDK_NGX_VULKAN_DestroyParameters(g_dfgParams);
    g_dfgParams = nullptr;
}

} // extern "C"
