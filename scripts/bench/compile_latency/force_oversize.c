/* Owned Linux fault-injection runs only. Never load this in an owner's viewer.
 * Build: cc -shared -fPIC -O2 force_oversize.c -ldl -o force_oversize.so
 * LD_PRELOAD=... ASTRA_FORCE_CACHE_OVERSIZE=1 makes a size-only cache query
 * report cap+1 once the real cache exceeds 24 MiB. It never supplies fake data
 * or changes rendering/compilation. This exercises the production save policy
 * with an otherwise valid, useful driver seed; it is NOT natural cache growth.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan_core.h>

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice dev, VkPipelineCache cache, size_t* bytes, void* data)
{
    PFN_vkGetPipelineCacheData real = (PFN_vkGetPipelineCacheData)dlsym(RTLD_NEXT, "vkGetPipelineCacheData");
    if (!real) abort();
    VkResult result = real(dev, cache, bytes, data);
    if (result == VK_SUCCESS && !data && *bytes > 24u * 1024u * 1024u
        && getenv("ASTRA_FORCE_CACHE_OVERSIZE"))
    {
        size_t actual = *bytes;
        *bytes = 256u * 1024u * 1024u + 1u;
        fprintf(stderr, "[astra fault injection] cache size query actual=%zu reported=%zu\n", actual, *bytes);
    }
    return result;
}
