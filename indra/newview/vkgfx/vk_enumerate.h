// Complete, bounded snapshots for Vulkan's count/fill property enumerations.
// Do not use for consuming queues (e.g. past presentation timing): their partial
// records are useful and re-querying does not recover the same snapshot.
#pragma once
#include "vk_common.h"
#include <atomic>
#include <cstdarg>
#include <new>
#include <stdexcept>

namespace vkenum
{
constexpr uint32_t kAttempts = 4;
constexpr uint32_t kProperties = 4096;
using LogSink = void(*)(const char*);
inline std::atomic<LogSink> logSink{nullptr};
inline void setLogSink(LogSink sink) noexcept { logSink.store(sink, std::memory_order_relaxed); }
inline void report(const char* format, ...) noexcept
{
    char line[384]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fprintf(stderr, "%s\n", line);
    // The viewer installs its logger; standalone tools / CPU fixtures need no
    // viewer symbol. Diagnostic allocation failure must not terminate collection.
    if (auto sink = logSink.load(std::memory_order_relaxed)) {
        try { sink(line); } catch (const std::bad_alloc&) {}
    }
}

template<class T, class Allocator, class Query>
VkResult collect(std::vector<T, Allocator>& output, Query&& query, const char* operation,
                 uint32_t limit = kProperties) noexcept
{
    std::vector<T, Allocator> candidate(output.get_allocator());
    for (uint32_t attempt = 0; attempt < kAttempts; ++attempt)
    {
        uint32_t count = 0;
        VkResult result = query(&count, nullptr);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            report("[vkgfx] %s count failed: %d", operation, int(result));
            return result;
        }
        if (count > limit)
        {
            report("[vkgfx] %s count %u exceeds bounded limit %u", operation, count, limit);
            return VK_INCOMPLETE; // soft refusal, not a driver/device failure
        }
        if (!count)
        {
            if (result == VK_INCOMPLETE)
            {
                report("[vkgfx] %s count incomplete with zero entries; retry %u/%u",
                             operation, attempt + 1, kAttempts);
                continue;
            }
            output.clear();
            return VK_SUCCESS;
        }
        try { candidate.resize(count); }
        catch (const std::bad_alloc&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        catch (const std::length_error&) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
        uint32_t written = count;
        result = query(&written, candidate.data());
        if (result == VK_INCOMPLETE || (result == VK_SUCCESS && written > count))
        {
            report("[vkgfx] %s changed during enumeration: capacity %u, returned %u; retry %u/%u",
                         operation, count, written, attempt + 1, kAttempts);
            continue; // fresh COUNT, never publish partial results or trust the old size
        }
        if (result != VK_SUCCESS)
        {
            report("[vkgfx] %s fill failed: %d", operation, int(result));
            return result;
        }
        candidate.resize(written);
        output.swap(candidate);
        return VK_SUCCESS;
    }
    report("[vkgfx] %s remained incomplete after %u attempts; deferring", operation, kAttempts);
    return VK_INCOMPLETE;
}

inline std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice physical)
{
    std::vector<VkExtensionProperties> output;
    collect(output, [physical](uint32_t* n, VkExtensionProperties* p)
            { return vkEnumerateDeviceExtensionProperties(physical, nullptr, n, p); },
            "vkEnumerateDeviceExtensionProperties");
    return output;
}
inline std::vector<VkExtensionProperties> instanceExtensions()
{
    std::vector<VkExtensionProperties> output;
    collect(output, [](uint32_t* n, VkExtensionProperties* p)
            { return vkEnumerateInstanceExtensionProperties(nullptr, n, p); },
            "vkEnumerateInstanceExtensionProperties");
    return output;
}
} // namespace vkenum
