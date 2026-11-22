/*
 * fg_caps.h -- what slip_nr_fg_caps answers: whether DLSS Frame Generation can run, and when it cannot,
 * the cause the viewer can tell the player about. Pure C++ (no NGX, no Vulkan), so a host test can
 * include it (scripts/check/test_framegen_appid.cpp).
 *
 * The vendor runtime only says "not available". Three causes have a fix the player can make, so the
 * add-on looks for them itself: NVIDIA's frame generation library is in none of the feature folders (an
 * add-on installed for DLSS before frame generation existed), Windows' hardware-accelerated GPU
 * scheduling is off (frame generation needs it), and the runtime gave no answer at all.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The Slipstream Viewer contributors. See contrib/ngxshim/LICENSE.
 */
#ifndef SLIPSTREAM_NGX_FG_CAPS_H
#define SLIPSTREAM_NGX_FG_CAPS_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#if !defined(_WIN32)
#  include <dirent.h>
#endif

// slip_nr_fg_capabilities.unavailable_reason: the header's SLIP_FG_WHY_* (slipstream_ngx.cpp checks they agree).
enum : uint32_t
{
    SLIP_FG_REASON_NOT_STATED = 0,
    SLIP_FG_REASON_LIBRARY_MISSING = 1,
    SLIP_FG_REASON_GPU_SCHEDULING_OFF = 2,
    SLIP_FG_REASON_QUERY_FAILED = 3,
};

// A fact the add-on may not be able to establish: nothing is ever claimed from UNKNOWN.
enum slip_fg_state { SLIP_FG_STATE_UNKNOWN = 0, SLIP_FG_STATE_YES = 1, SLIP_FG_STATE_NO = 2 };

struct slip_fg_query
{
    bool          availableRead = false;     // the runtime answered whether frame generation is available
    bool          available = false;
    bool          maxRead = false;           // ... and how many frames it generates at most
    uint32_t      maxGenerated = 0;
    bool          needsNewerDriver = false;  // (false when that read failed)
    slip_fg_state library = SLIP_FG_STATE_UNKNOWN;         // NVIDIA's library is in a feature folder
    slip_fg_state gpuScheduling = SLIP_FG_STATE_UNKNOWN;   // Windows' hardware-accelerated GPU scheduling is on
};

struct slip_fg_answer
{
    uint32_t available = 0;
    uint32_t maxGenerated = 0;
    uint32_t reason = SLIP_FG_REASON_NOT_STATED;
};

inline slip_fg_answer slip_fg_decide(const slip_fg_query& q)
{
    slip_fg_answer a;
    if (!q.availableRead)
    {
        a.reason = SLIP_FG_REASON_QUERY_FAILED;
        return a;
    }
    if (q.available)
    {
        // (The runtime does not look at Windows' GPU scheduling; without it the feature cannot be created.)
        if (q.gpuScheduling == SLIP_FG_STATE_NO)
        {
            a.reason = SLIP_FG_REASON_GPU_SCHEDULING_OFF;
            return a;
        }
        a.available = 1;
        // One generated frame (2x) is what every GPU with frame generation runs: an unreadable or zero
        // maximum must not turn such a GPU into "unsupported".
        a.maxGenerated = (q.maxRead && q.maxGenerated) ? q.maxGenerated : 1;
        return a;
    }
    // Not available. A library that is in none of the folders explains it whatever else is set; a driver that
    // is too old is said by the driver fields; then the scheduling. Else no cause is claimed.
    if (q.library == SLIP_FG_STATE_NO) a.reason = SLIP_FG_REASON_LIBRARY_MISSING;
    else if (q.needsNewerDriver) a.reason = SLIP_FG_REASON_NOT_STATED;
    else if (q.gpuScheduling == SLIP_FG_STATE_NO) a.reason = SLIP_FG_REASON_GPU_SCHEDULING_OFF;
    return a;
}

// A caller's struct_size is accepted from the first release's size up (the ABI only grows).
inline bool slip_fg_size_ok(uint32_t structSize, uint32_t firstReleaseSize) { return structSize >= firstReleaseSize; }

// Fills the caller's capabilities struct (Caps = slip_nr_fg_capabilities) of `structSize` bytes: the first
// release's fields always, later ones only when the caller's struct has them; nothing beyond is touched.
template <class Caps>
inline void slip_fg_write_caps(Caps* out, uint32_t structSize, const slip_fg_answer& a, bool needsNewerDriver,
                               uint32_t minDriverMajor, uint32_t minDriverMinor)
{
    out->struct_size = structSize;
    out->available = a.available;
    out->max_generated = a.maxGenerated;
    out->needs_newer_driver = needsNewerDriver ? 1u : 0u;
    out->min_driver_major = minDriverMajor;
    out->min_driver_minor = minDriverMinor;
    out->application_id_set = 0u;   // (1.4.0: no application id is read; the field stays for the ABI)
    if (structSize >= offsetof(Caps, unavailable_reason) + sizeof(uint32_t)) out->unavailable_reason = a.reason;   // 1.3.2
}

// NVIDIA's frame generation library by its file name: nvngx_dlssg.dll (any case), or on Linux
// libnvidia-ngx-dlssg.so with its version suffix.
inline bool slip_fg_is_library_name(const char* name)
{
    if (!name) return false;
    static const char win[] = "nvngx_dlssg.dll";
    size_t i = 0;
    for (; win[i] && name[i]; ++i)
    {
        const char c = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] - 'A' + 'a') : name[i];
        if (c != win[i]) break;
    }
    if (!win[i] && !name[i]) return true;
    static const char lin[] = "libnvidia-ngx-dlssg.so";
    const size_t n = sizeof(lin) - 1;
    return std::strncmp(name, lin, n) == 0 && (name[n] == '\0' || name[n] == '.');
}

// Several feature folders: the library is there when any holds it; missing only when a folder was read
// and none read so far holds it (`soFar` starts UNKNOWN; a folder that cannot be read changes nothing).
inline slip_fg_state slip_fg_merge_library(slip_fg_state soFar, slip_fg_state folder)
{
    if (soFar == SLIP_FG_STATE_YES || folder == SLIP_FG_STATE_YES) return SLIP_FG_STATE_YES;
    return folder == SLIP_FG_STATE_NO ? SLIP_FG_STATE_NO : soFar;
}

#if !defined(_WIN32)
// Whether the folder `dir` (UTF-8) holds the library; UNKNOWN when it cannot be read.
inline slip_fg_state slip_fg_library_in(const char* dir)
{
    if (!dir || !*dir) return SLIP_FG_STATE_UNKNOWN;
    DIR* d = opendir(dir);
    if (!d) return SLIP_FG_STATE_UNKNOWN;
    slip_fg_state found = SLIP_FG_STATE_NO;
    while (const dirent* e = readdir(d))
        if (slip_fg_is_library_name(e->d_name))
        {
            found = SLIP_FG_STATE_YES;
            break;
        }
    closedir(d);
    return found;
}
#endif

#endif
