// COMPILE ONLY WHAT IS NEEDED (indra/newview/vkgfx/llcleanpipeneeds.h): the settings -> optional
// pipeline families mapping, compiled from the real header. No GPU.
#include "llcleanpipeneeds.h"

#include <cassert>
#include <cstdio>

using namespace llpipeneeds;

// Every RT consumer off (the "RT off" configuration the owner measures against).
static Settings rtOff()
{
    Settings s;
    s.rtDevice = true;
    s.sunShadowsOn = true;
    s.sunShadows = 1;      // cascades
    s.projShadows = 1;     // shadow maps
    s.giMode = 0;
    s.probeDetail = 2;     // realtime (raster) probes
    s.rtReflections = 0;
    s.rtMirrors = false;
    s.aoMethod = 1;        // GTAO
    s.debugView = 0;
    s.alphaOit = 2;
    s.specPrewarm = 0;
    return s;
}

int main()
{
    const uint32_t kRt = FAM_RT_KERNELS | FAM_RT_LIT | FAM_WBOIT_RT;

    // 1. RT off: no ray-traced family is ever requested, whatever the WBOIT mode.
    for (uint32_t oit = 0; oit <= 2; ++oit)
    {
        Settings s = rtOff();
        s.alphaOit = oit;
        assert((needs(s) & kRt) == 0u);
    }
    // 2. No RT foundation (GPU without ray query, RenderVKRayTracing 0): every RT setting on still
    //    requests nothing ray traced.
    {
        Settings s = rtOff();
        s.rtDevice = false;
        s.sunShadows = 2; s.projShadows = 2; s.giMode = 3; s.probeDetail = 3; s.rtReflections = 1;
        s.rtMirrors = true; s.aoMethod = 2; s.debugView = 13; s.alphaOit = 1;
        assert((needs(s) & kRt) == 0u);
    }
    // 3. Per consumer: the kernels for any RT consumer; the ray-query LIT variant (the heaviest
    //    compiles) only for ray-traced sun / projector shadows.
    {
        Settings s = rtOff(); s.giMode = 1;
        assert((needs(s) & FAM_RT_KERNELS) && !(needs(s) & FAM_RT_LIT) && !(needs(s) & FAM_WBOIT_RT));
        s = rtOff(); s.rtReflections = 1;
        assert((needs(s) & FAM_RT_KERNELS) && !(needs(s) & FAM_RT_LIT));
        s = rtOff(); s.aoMethod = 2;
        assert((needs(s) & FAM_RT_KERNELS) && !(needs(s) & FAM_RT_LIT));
        s = rtOff(); s.aoMethod = 2; s.ssao = false;           // (RTAO with AO off: nothing traces)
        assert(!(needs(s) & kRt));
        s = rtOff(); s.probeDetail = 3;
        assert((needs(s) & FAM_RT_KERNELS) && !(needs(s) & FAM_RT_LIT));
        s = rtOff(); s.probeDetail = 3; s.reflectionsOn = false;
        assert(!(needs(s) & kRt));
        s = rtOff(); s.rtMirrors = true;
        assert((needs(s) & FAM_RT_KERNELS) && !(needs(s) & FAM_RT_LIT));
        s = rtOff(); s.sunShadows = 2;
        assert((needs(s) & FAM_RT_KERNELS) && (needs(s) & FAM_RT_LIT) && (needs(s) & FAM_WBOIT_RT));
        s = rtOff(); s.sunShadows = 2; s.sunShadowsOn = false;  // (shadows off: the sun does not trace)
        assert(!(needs(s) & kRt));
        s = rtOff(); s.projShadows = 2;
        assert((needs(s) & FAM_RT_LIT) && (needs(s) & FAM_RT_KERNELS));
        s = rtOff(); s.projShadows = 2; s.shadowDetail = 1;     // (projector shadows need detail 2)
        assert(!(needs(s) & kRt));
    }
    // 4. WBOIT: exactly the selected mode; mode 2 falls to mode 1 below 5 colour attachments;
    //    the ray-query twins only with the ray-query lit variant.
    {
        Settings s = rtOff();
        s.alphaOit = 0;
        assert(!(needs(s) & (FAM_WBOIT_M1 | FAM_WBOIT_M2 | FAM_WBOIT_RT)));
        s.alphaOit = 1;
        assert((needs(s) & FAM_WBOIT_M1) && !(needs(s) & FAM_WBOIT_M2));
        s.alphaOit = 2;
        assert((needs(s) & FAM_WBOIT_M2) && !(needs(s) & FAM_WBOIT_M1));
        s.maxColorAttachments = 4;
        assert((needs(s) & FAM_WBOIT_M1) && !(needs(s) & FAM_WBOIT_M2) && oitMode(s) == 1u);
        s.maxColorAttachments = 8;
        s.alphaOit = 7;   // (out of range: clamped to 2, as the renderer does)
        assert(oitMode(s) == 2u);
        s.sunShadows = 2;
        assert((needs(s) & FAM_WBOIT_M2) && (needs(s) & FAM_WBOIT_RT));
        s.alphaOit = 0;
        assert(!(needs(s) & FAM_WBOIT_RT));
    }
    // 5. Speculative setting-specialised sets: never at the default.
    {
        Settings s;
        assert(s.specPrewarm == 0u && !(needs(s) & FAM_SPEC_PREWARM));
        s.specPrewarm = 1;
        assert(needs(s) & FAM_SPEC_PREWARM);
    }
    // 5b. MBOIT (RenderVKAlphaMBOIT): nothing at the default; its ray-query shade only with the ray-query lit variant.
    {
        Settings s;
        assert(s.alphaMboit == 0u && !(needs(s) & (FAM_MBOIT | FAM_MBOIT_RT)));
        s.alphaMboit = 1;
        assert((needs(s) & FAM_MBOIT) && !(needs(s) & FAM_MBOIT_RT));
        s.rtDevice = true; s.sunShadowsOn = true; s.sunShadows = 2;
        assert((needs(s) & FAM_MBOIT) && !(needs(s) & FAM_MBOIT_RT));   // (shared visibility: the full tier is plain)
        s.blendFrontLight = false;
        assert((needs(s) & FAM_MBOIT) && (needs(s) & FAM_MBOIT_RT));
    }
    // 5c. Split transparency (RenderVKBlendSplit): nothing at the default; its ray-query twins only with the ray-query
    //     lit variant.
    {
        Settings s;
        assert(s.blendSplit == 0u && !(needs(s) & (FAM_BLEND_SPLIT | FAM_BLEND_SPLIT_RT)));
        s.blendSplit = 1;
        assert((needs(s) & FAM_BLEND_SPLIT) && !(needs(s) & FAM_BLEND_SPLIT_RT));
        s.rtDevice = true; s.sunShadowsOn = true; s.sunShadows = 2;
        assert((needs(s) & FAM_BLEND_SPLIT) && (needs(s) & FAM_BLEND_SPLIT_RT));
    }
    // 6. Every family has a name (the "not compiled" log line).
    for (uint32_t bit = 1; bit < (1u << FAM_COUNT_BITS); bit <<= 1)
        assert(familyName(bit)[0] != '?');
    std::puts("pipeline needs: all checks passed");
    return 0;
}
