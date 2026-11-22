#version 460
#extension GL_GOOGLE_include_directive : require
// vkclean -- SPLIT TRANSPARENCY: THE VOLUME SLICE VIEW (RenderVKBlendSplitDebug 2; clean_bs_common.glsl BSD_SLICE).
// A fullscreen triangle at the end of the pre-water static alpha list (inside pass 2): every pixel shows the volume's
// slice bs.knobs.w (0 = near .. 1 = far; RenderVKBlendSplitDebugSlice) at its screen position -- the ambient +
// local lights' irradiance facing up (L0 + L1z) plus the sun / moon cell term -- over a dimmed scene, with the slice
// grid's cell edges drawn dark. Cells the opaque cull zeroed read black.
#define BS_READ_VOLUME
#include "clean_bs_common.glsl"

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outReact;

void main()
{
    const vec2 res = vec2(float(bs.flags.w & 0xFFFFu), float(bs.flags.w >> 16u));
    const vec2 uv = gl_FragCoord.xy / res;
    const vec3 tc = vec3(uv, clamp(bs.knobs.w, 0.0, 1.0));
    const vec4 r = textureLod(uBsShR, tc, 0.0), g = textureLod(uBsShG, tc, 0.0), b = textureLod(uBsShB, tc, 0.0);
    const vec3 up = vec3(0.0, 0.0, 1.0);
    vec3 c = bsShIrradiance(r, g, b, up) + max(textureLod(uBsSun, tc, 0.0).rgb, vec3(0.0));
    const vec2 cellUv = fract(uv * bs.grid.xy);
    if (any(lessThan(cellUv, vec2(0.04)))) c *= 0.5;   // (the cell edges)
    outColor = vec4(c, 0.85);
    outGlow = vec4(0.0);
    outReact = vec4(0.0);
}
