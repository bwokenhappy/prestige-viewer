// Prestige Vulkan port, phase 3a: bring Slipstream's VkDeviceContext up and down (see build_and_run.sh).
#include "vk_device.h"
#include <cstdio>
int main()
{
    VkDeviceContext ctx;
    bool ok = ctx.init(false);
    printf("VkDeviceContext::init -> %s\n", ok ? "OK" : "FAILED");
    for (auto& l : vkRequirementsLogLines(ctx.requirementsReport())) printf("  %s\n", l.c_str());
    if (ok) ctx.shutdown();
    printf("shutdown done\n");
    return ok ? 0 : 1;
}
