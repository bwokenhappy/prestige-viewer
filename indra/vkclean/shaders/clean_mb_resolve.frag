#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
// vkclean -- MBOIT RESOLVE (RenderVKAlphaMBOIT; clean_mboit.glsl, LLCleanAlphaMBOITVK).
//
// A fullscreen triangle inside the pass-2 scope, at the static alpha list's place in the blend order:
//     colour' = sum(c a T) / sum(a T) x (1 - R) + colour x R          (blend ONE / SRC_ALPHA)
// R the exact revealage prod(1 - a) of the shaded layers (the paper's eq. 2 with that coverage), the glow
// sum likewise over the glow (unnormalised: each layer's glow x its glow coverage x T), and the coverage
// 1 - R into the TAA reactive mask (MAX, R). Untouched pixels (no layer shaded: the glow target's count) leave.
// Debug (RenderVKMBOITDebug, painted opaque over every covered pixel): 2 the moments' total absorbance b0
// against the cutoff's -ln(eps) in grey (white: dense enough for the cutoff to drop layers), red where it did
// (pass 1 rasterised more than pass 3 shaded); 4 the cull: hue = the fraction of the rasterised layers the
// cutoff culled (green none .. red all), brightness = the layer count on a log2 scale (64+ = full).
#include "clean_frame_info.glsl"    // ssr.atmos (the HDR scale for the debug colours)
#define MB_SET 2
#include "clean_mboit.glsl"

layout(location = 0) out vec4 outMbColor;
layout(location = 1) out vec4 outMbGlow;
layout(location = 2) out vec4 outMbReact;

void main()
{
    const ivec2 px = ivec2(gl_FragCoord.xy);
    // (touched: any shaded layer -- the glow sum's count; a glow-only layer of alpha ~0 leaves R at 1 but adds glow)
    const vec4 G = texelFetch(uMbGlow, px, 0);
    if (!(G.a > 0.5)) discard;
    const float R = clamp(texelFetch(uMbReveal, px, 0).r, 0.0, 1.0);
    const vec4 S = texelFetch(uMbSum, px, 0);
    const float cov = 1.0 - R;
    const vec3 c = S.a > 1e-8 ? S.rgb / S.a : vec3(0.0);
    outMbColor = vec4(c * cov, R);
    outMbGlow = vec4(max(G.rgb, vec3(0.0)), R);
    outMbReact = vec4(cov, 0.0, 0.0, 0.0);
    const uint dbg = mb.flags.x & (MBD_TRANS | MBD_COUNT);
    if (dbg != 0u)
    {
        // the moments at this pixel (half resolution: the texel it lies in)
        const ivec2 mp = mb.sizes.z != 0u ? px / 2 : px;
        const vec4 B = texelFetch(uMbMomB, mp, 0);
        const float raster = mb.sizes.w == 2u ? B.w : B.y;
        const float shaded = G.a;
        const float k = ssr.atmos[6].y;   // (atmosHdrScale)
        vec3 d;
        if ((dbg & MBD_TRANS) != 0u)
        {
            // the moments' total absorbance against the cutoff's: white where the stack is dense enough for the
            // cutoff to drop anything (exp(-b0) <= RenderVKMBOITCutoff), darker the fainter it is; red where it did
            const float need = -log(max(mb.knobs.x, 1e-6));
            d = vec3(clamp(B.x / need, 0.0, 1.0));
            if (raster > shaded + 0.5) d = mix(d, vec3(1.0, 0.1, 0.1), 0.6);
        }
        else
        {
            // hue = the fraction the cutoff culled (green none .. red all), brightness = the layer count on a log2
            // scale (1 layer dim, every doubling brighter, 64 layers and more full)
            const float n = max(raster, shaded);
            const float culled = raster > 0.5 ? clamp(1.0 - shaded / raster, 0.0, 1.0) : 0.0;
            d = mix(vec3(0.1, 1.0, 0.1), vec3(1.0, 0.1, 0.1), culled) * clamp(log2(1.0 + n) / 6.0, 0.08, 1.0);
        }
        outMbColor = vec4(d * k, 0.0);
        outMbGlow = vec4(0.0, 0.0, 0.0, 0.0);
    }
}
