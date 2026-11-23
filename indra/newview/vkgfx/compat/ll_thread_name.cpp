/**
 * @file ll_thread_name.cpp
 * @brief ll_set_current_thread_name(): names the calling thread for debuggers and crash reports.
 *
 * Prestige Firestorm-Vulkan port: Slipstream's llcommon has this (llthread.cpp); Firestorm's
 * does not, and the ported Vulkan code declares and calls it. Copied from Slipstream
 * (LGPL 2.1), indra/llcommon/llthread.cpp @ prestige cd94540c.
 */
#include "linden_common.h"

#include <cstring>
#include <string>

#if LL_WINDOWS
#include "llwin32headers.h"
#include "llstring.h"
#elif LL_LINUX
#include <pthread.h>
#endif

void ll_set_current_thread_name(const char* name)
{
    if (!name || !*name)
    {
        return;
    }
#if LL_WINDOWS
    // SetThreadDescription: Windows 10 1607+, resolved at run time (no hard import).
    typedef HRESULT (WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
    static const SetThreadDescriptionFn s_set = []() -> SetThreadDescriptionFn
    {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        return k32 ? (SetThreadDescriptionFn)GetProcAddress(k32, "SetThreadDescription") : nullptr;
    }();
    if (s_set)
    {
        const std::wstring wname = ll_convert_string_to_wide(std::string(name));
        s_set(GetCurrentThread(), wname.c_str());
    }
#elif LL_LINUX
    char buf[16];   // the kernel's limit, terminator included
    strncpy(buf, name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    pthread_setname_np(pthread_self(), buf);
#endif
}
