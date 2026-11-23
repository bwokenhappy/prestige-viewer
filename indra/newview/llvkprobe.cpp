/**
 * @file llvkprobe.cpp
 * @brief Prestige Firestorm-Vulkan port, phase 1: Vulkan pre-flight on the Firestorm viewer.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer, built on Firestorm (Phoenix Firestorm Project, Inc.) and Slipstream.
 * GNU Lesser General Public License, version 2.1.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llvkprobe.h"

#if PRESTIGE_VULKAN

#include "vkgfx/vk_requirements.h"

#include <mutex>

namespace
{
    std::once_flag   sOnce;
    bool             sReady = false;
    std::string      sSummary;

    std::string api_version_text(uint32_t v)
    {
        return llformat("%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v));
    }

    void do_check()
    {
        VkRequirementsReport report;
        try
        {
            report = vkCheckRequirements();
        }
        catch (const std::exception& e)
        {
            LL_WARNS("Vulkan") << "Vulkan requirements check threw: " << e.what() << LL_ENDL;
            sSummary = "check failed";
            return;
        }

        for (const std::string& line : vkRequirementsLogLines(report))
        {
            LL_INFOS("Vulkan") << line << LL_ENDL;
        }

        if (!report.instanceOk)
        {
            sSummary = "no Vulkan driver found";
            LL_INFOS("Vulkan") << "Vulkan pre-flight: no usable Vulkan instance ("
                               << vkResultString(report.instanceResult) << ")" << LL_ENDL;
            return;
        }

        if (report.best < 0 || report.best >= (int)report.gpus.size())
        {
            // Name the first device and what it is missing, so the About box is useful.
            if (!report.gpus.empty())
            {
                const VkGpuRequirements& g = report.gpus.front();
                std::string missing;
                for (const std::string& m : g.missing)
                {
                    if (!missing.empty()) missing += ", ";
                    missing += m;
                }
                sSummary = llformat("%s (Vulkan %s) - not supported: %s", g.name.c_str(),
                                    api_version_text(g.apiVersion).c_str(), missing.c_str());
            }
            else
            {
                sSummary = "no Vulkan graphics card found";
            }
            LL_INFOS("Vulkan") << "Vulkan pre-flight: no qualifying device" << LL_ENDL;
            return;
        }

        const VkGpuRequirements& g = report.gpus[report.best];
        sReady = true;
        sSummary = llformat("%s (driver %s, Vulkan %s%s) - ready", g.name.c_str(), g.driverText.c_str(),
                            api_version_text(g.apiVersion).c_str(), g.useApi12 ? ", 1.2 path" : "");
        LL_INFOS("Vulkan") << "Vulkan pre-flight: renderer would use " << g.name << " (" << report.bestWhy << ")" << LL_ENDL;
    }
}

namespace LLVkProbe
{
    void run()
    {
        std::call_once(sOnce, do_check);
    }

    bool deviceReady()
    {
        run();
        return sReady;
    }

    std::string summary()
    {
        run();
        return sSummary;
    }
}

#endif // PRESTIGE_VULKAN
