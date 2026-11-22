// vkgfx -- the hang watchdog's rule (#135), the one rule for a wait on the GPU that could otherwise block for
// ever: the device's infinite fence / semaphore waits (vk_device.cpp vkSlicedWait). Such a wait runs in slices.
// One still blocked after a full watchdog slice gives up if the device was lost meanwhile (nothing it waits
// for will come); after kVkHangSlices slices it is a GPU hang -- the wait raises the device lost with
// VK_TIMEOUT -- instead of freezing the viewer. A wait that ends is never one. Slices that timed out are
// counted, never the clock: a paused process (a debugger break, a system sleep) costs one slice.
// (#1517) Nothing else decides a GPU hang. The renderer's waits for frame generation's present thread are
// bounded and judge nothing; with that thread held they wait, through these waits, what the GPU was given.
#pragma once

#include <cstdint>

constexpr uint64_t kVkHangSliceNs = 5000000000ull;   // 5 s
constexpr uint32_t kVkHangSlices = 12;               // 60 s

enum class VkHangVerdict
{
    Wait,     // go on waiting
    GiveUp,   // the device was lost meanwhile (raised elsewhere): stop waiting
    Hang,     // no progress for kVkHangSlices slices: raise the device lost (VK_TIMEOUT), stop waiting
};

// A wait blocked for waitedNs so far; lost: vkDeviceIsLost() now.
inline VkHangVerdict vkHangVerdict(uint64_t waitedNs, bool lost)
{
    if (waitedNs < kVkHangSliceNs) return VkHangVerdict::Wait;
    if (lost) return VkHangVerdict::GiveUp;
    return waitedNs >= kVkHangSliceNs * kVkHangSlices ? VkHangVerdict::Hang : VkHangVerdict::Wait;
}

// (#1517) A lost device is reported -- the diagnostics block, the marker the next launch reads, the effects
// quarantine -- unless a renderer thread was left behind inside a driver call (vkThreadAbandoned: frame
// generation's present thread, at quit). The exit is under way then, and what that call answers once its
// window or the rest of the session is gone (a lost surface, a lost device) is no fault of this session: the
// state is still set, so nothing more goes to the GPU, and one line is logged.
inline bool vkLostDeviceReported(bool threadAbandoned) { return !threadAbandoned; }

