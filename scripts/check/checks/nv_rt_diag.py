"""Regression guard for the NVIDIA GPU-hang diagnostics switch (#1508 / #1128, Xid 109 CTX SWITCH TIMEOUT).

SLIPSTREAM_NV_RT_DIAG=1 (environment, never a saved setting) arms NVIDIA's own instruments in the device
(indra/newview/vkgfx/vk_device.cpp):
  * VK_NV_ray_tracing_validation: the driver validates every acceleration-structure build / refit / copy and
    traversal input and reports through the debug-utils messenger. It only works when
    NV_ALLOW_RAYTRACING_VALIDATION=1 is in the process environment BEFORE the driver loads, so the viewer
    sets it at the top of createInstance, ahead of the first Vulkan call.
  * VK_NV_device_diagnostics_config with resource tracking + shader debug info + shader error reporting: a
    bare Xid 109 (no fault address) then names the faulting buffer / AS and shader.
The DEVICE_LOST report prints whether both were armed and how many validation messages preceded the loss:
that line is what makes the next hang decisive (validator armed and silent = the AS-input class is out;
a message names the bad input; resource tracking names the resource).

This check keeps that wiring in place: the env var is set before the instance exists, the feature struct is
chained whenever the extension is requested, the diagnostics config reaches the device create info, and the
loss report carries the status line.
"""
import re

NAME = "nv-rt-diag"
STAGE = "source"
DESCRIPTION = ("SLIPSTREAM_NV_RT_DIAG arms VK_NV_ray_tracing_validation + VK_NV_device_diagnostics_config "
               "(env set before the instance, feature chained, config in the device chain, status in the loss report)")
GUARDS = ("A GPU hang (#1508 / #1128 Xid 109) report that cannot say whether NVIDIA's RT validation and resource "
          "tracking were armed, or a run where they silently did not arm.")


def run(ctx, result):
    path = "indra/newview/vkgfx/vk_device.cpp"
    with open(ctx.path(path), encoding="utf-8", errors="replace") as f:
        src = f.read()
    problems = []

    # 1. The env var is set at the process entry points (the loader opens the ICD at the first
    #    instance-level call -- the hardware-stats probe, SDL's extension query -- long before
    #    vkCreateInstance; a setenv in createInstance alone came too late: the 2026-11-12 07:17Z loss
    #    report said "ray tracing validation extension not listed") and again in createInstance.
    with open(ctx.path("indra/llcommon/slenv.h"), encoding="utf-8", errors="replace") as f:
        env = f.read()
    if not re.search(r'_putenv_s\("NV_ALLOW_RAYTRACING_VALIDATION", "1"\)', env) \
            or not re.search(r'setenv\("NV_ALLOW_RAYTRACING_VALIDATION", "1", 0\)', env) \
            or "inline bool sl_nv_rt_diag_env()" not in env:
        problems.append("slenv.h sl_nv_rt_diag_env no longer sets NV_ALLOW_RAYTRACING_VALIDATION on both platforms")
    for entry, marker in (("indra/newview/llappviewerlinux.cpp", "int main( int argc, char **argv )"),
                          ("indra/newview/llappviewerwin32.cpp", "int APIENTRY WINMAIN(")):
        with open(ctx.path(entry), encoding="utf-8", errors="replace") as f:
            es = f.read()
        at = es.find(marker)
        if at < 0 or "sl_nv_rt_diag_env();" not in es[at:at + 1200]:
            problems.append("%s does not call sl_nv_rt_diag_env() first thing in its entry point" % entry)
    ci = src.find("bool VkDeviceContext::createInstance(")
    first_call = src.find("vkEnumerateInstanceVersion(", ci)
    setenv_at = src.find("sl_nv_rt_diag_env()", ci)
    if ci < 0 or setenv_at < 0 or first_call < 0 or setenv_at > first_call:
        problems.append("createInstance no longer calls sl_nv_rt_diag_env() ahead of its first Vulkan call")

    # 2. The feature struct is chained (rayTracingValidation = VK_TRUE) whenever the extension is requested.
    push = src.find("devExts.push_back(VK_NV_RAY_TRACING_VALIDATION_EXTENSION_NAME)")
    block = src[max(0, push - 600):push] if push >= 0 else ""
    if push < 0 or "fNvRtVal.rayTracingValidation = VK_TRUE" not in block or "f2.pNext = &fNvRtVal" not in block:
        problems.append("VK_NV_ray_tracing_validation is requested without chaining its feature struct")
    if "sNvRtDiagWanted = sl_nv_rt_diag_env()" not in src:
        problems.append("the SLIPSTREAM_NV_RT_DIAG gate (sNvRtDiagWanted = sl_nv_rt_diag_env()) is gone")

    # 3. The diagnostics config reaches the device create info chain.
    if not re.search(r"if \(sNvDiagCfgFlags\) \{ nvDiagCI\.pNext = dci\.pNext; dci\.pNext = &nvDiagCI; \}", src):
        problems.append("VkDeviceDiagnosticsConfigCreateInfoNV is not chained into VkDeviceCreateInfo")
    for bit in ("VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV",
                "VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV"):
        if bit not in src:
            problems.append("diagnostics config no longer asks for " + bit)

    # 4. The loss report says whether the instruments were armed.
    rep = src.find("void vkReportDeviceLost()")
    if rep < 0 or "vkNvRtDiagReportLine();" not in src[rep:rep + 1500]:
        problems.append("vkReportDeviceLost no longer prints the NV RT diagnostics status line")
    if "validation messages before the loss" not in src:
        problems.append("the loss report's NV RT diagnostics line lost its message count")

    for p in problems:
        result.fail("%s: %s" % (path, p))
    result.summary = "NV RT diagnostics wiring %s" % ("intact" if not problems else "BROKEN")
