/*
 * gpu_scheduling.h -- Windows only: whether hardware-accelerated GPU scheduling is on for one GPU, and
 * whether a folder holds NVIDIA's frame generation library. No NGX, no Vulkan.
 *
 * DLSS Frame Generation needs GPU scheduling on Windows (Settings > System > Display > Graphics). The
 * vendor runtime does not report it, so the add-on asks Windows: the graphics kernel's capabilities of
 * the adapter (its LUID is the Vulkan device's), which say whether scheduling is enabled right now --
 * not a registry value, which only says what the next restart will do.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The Slipstream Viewer contributors. See contrib/ngxshim/LICENSE.
 */
#ifndef SLIPSTREAM_NGX_GPU_SCHEDULING_H
#define SLIPSTREAM_NGX_GPU_SCHEDULING_H

#include "fg_caps.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <winternl.h>    // NTSTATUS, which d3dkmthk.h needs
#  include <d3dkmthk.h>
#  include <string>

// YES: on. NO: the GPU supports it and it is off. UNKNOWN: Windows could not say (before Windows 10 2004,
// an adapter that cannot be opened), or the GPU does not support it (nothing for the player to turn on).
inline slip_fg_state slip_gpu_scheduling(const LUID& adapter)
{
    slip_fg_state state = SLIP_FG_STATE_UNKNOWN;
    const HMODULE gdi = LoadLibraryW(L"gdi32.dll");   // (the D3DKMT entry points; resolved by name: no import)
    if (!gdi) return state;
    const PFND3DKMT_OPENADAPTERFROMLUID openAdapter = (PFND3DKMT_OPENADAPTERFROMLUID)(void*)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid");
    const PFND3DKMT_QUERYADAPTERINFO queryInfo = (PFND3DKMT_QUERYADAPTERINFO)(void*)GetProcAddress(gdi, "D3DKMTQueryAdapterInfo");
    const PFND3DKMT_CLOSEADAPTER closeAdapter = (PFND3DKMT_CLOSEADAPTER)(void*)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    if (openAdapter && queryInfo && closeAdapter)
    {
        D3DKMT_OPENADAPTERFROMLUID open{};
        open.AdapterLuid = adapter;
        if (openAdapter(&open) >= 0)
        {
            D3DKMT_WDDM_2_7_CAPS caps{};
            D3DKMT_QUERYADAPTERINFO query{};
            query.hAdapter = open.hAdapter;
            query.Type = KMTQAITYPE_WDDM_2_7_CAPS;
            query.pPrivateDriverData = &caps;
            query.PrivateDriverDataSize = sizeof(caps);
            if (queryInfo(&query) >= 0)
                state = caps.HwSchEnabled ? SLIP_FG_STATE_YES : caps.HwSchSupported ? SLIP_FG_STATE_NO : SLIP_FG_STATE_UNKNOWN;
            D3DKMT_CLOSEADAPTER close{};
            close.hAdapter = open.hAdapter;
            closeAdapter(&close);
        }
    }
    FreeLibrary(gdi);
    return state;
}

// Whether the folder `dir` holds nvngx_dlssg.dll; UNKNOWN when the folder itself cannot be found.
inline slip_fg_state slip_fg_library_in(const std::wstring& dir)
{
    if (dir.empty()) return SLIP_FG_STATE_UNKNOWN;
    const DWORD folder = GetFileAttributesW(dir.c_str());
    if (folder == INVALID_FILE_ATTRIBUTES || !(folder & FILE_ATTRIBUTE_DIRECTORY)) return SLIP_FG_STATE_UNKNOWN;
    const wchar_t last = dir[dir.size() - 1];
    const std::wstring file = dir + ((last == L'\\' || last == L'/') ? L"" : L"\\") + L"nvngx_dlssg.dll";
    const DWORD a = GetFileAttributesW(file.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? SLIP_FG_STATE_YES : SLIP_FG_STATE_NO;
}
#endif

#endif
