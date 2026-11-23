/**
 * @file llvkprobe.h
 * @brief Prestige Firestorm-Vulkan port, phase 1: Vulkan pre-flight on the Firestorm viewer.
 *
 * Runs Slipstream's Vulkan requirements check (vkgfx/vk_requirements) once, logs the
 * per-device report to the viewer log and exposes a one-line summary for Help > About.
 * It renders nothing: the OpenGL renderer is still the one in use. Later phases bring up
 * the Vulkan device context and renderer on the device this check picks.
 *
 * Only built when PRESTIGE_VULKAN is on; otherwise every call is an inline no-op.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer, built on Firestorm (Phoenix Firestorm Project, Inc.) and Slipstream.
 * GNU Lesser General Public License, version 2.1.
 * $/LicenseInfo$
 */
#ifndef LL_LLVKPROBE_H
#define LL_LLVKPROBE_H

#include <string>

namespace LLVkProbe
{
#if PRESTIGE_VULKAN
    // Runs the check (first call only) and writes the report to the log.
    void run();
    // True once run() found a device that meets the renderer's requirements.
    bool deviceReady();
    // One line for Help > About, e.g. "NVIDIA GeForce RTX 3060 (driver 560.94, Vulkan 1.4) - ready".
    // Runs the check first if it hasn't run yet.
    std::string summary();
#else
    inline void run() {}
    inline bool deviceReady() { return false; }
    inline std::string summary() { return std::string(); }
#endif
}

#endif // LL_LLVKPROBE_H
