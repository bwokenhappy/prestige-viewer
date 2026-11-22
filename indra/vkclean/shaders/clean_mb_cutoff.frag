#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
// vkclean -- MBOIT CUTOFF DEPTH (RenderVKAlphaMBOIT; clean_mboit.glsl, LLCleanAlphaMBOITVK).
//
// A fullscreen triangle into the cutoff depth (no colour; depth compare ALWAYS, write on), between the
// moments pass and the shade pass of one water side. Per render pixel, reversed-Z (nearer = larger):
//   opaque  -- the opaque depth pushed BACK by the #731 tolerance's widest window there (base + scale x
//              distance, x the grazing cap), so every fragment the shade FS's own tolerance test would keep
//              passes the hardware test; that FS repeats the exact test.
//   moments -- the depth beyond which the CONSERVATIVE transmittance (the reconstruction's upper bound,
//              eta = 0) is below the cutoff epsilon (mbCutoff); half-resolution moments: the FARTHEST such
//              depth over the four moment texels around the pixel (a strand one moment texel wide never
//              cuts the pixel next to it).
// The nearer of the two is written. RenderVKMBOITDebug bit 8 / the cutoff off / orthographic views: the
// opaque term only. Layout: draw sets 0 / 1 (the scene depth, the frame info, the main view entry by the
// pushed view index) + the module's set at 2.
#include "clean_frame_info.glsl"    // ssr.blend_tol
#include "clean_draw_view.glsl"     // pc: the main view (u_inv_view_proj)
#define MB_SET 2
#include "clean_mboit.glsl"
layout(set = 0, binding = 40) uniform sampler2DArray uBlendSceneDepth;

float mbCutAt(ivec2 t)
{
    return mbCutoff(mb.sizes.w, texelFetch(uMbMomA, t, 0), texelFetch(uMbMomB, t, 0), mb.knobs.x);
}

void main()
{
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const float zO = texelFetch(uBlendSceneDepth, ivec3(px, 0), 0).r;
    const float kz = pc.u_inv_view_proj[2][3];
    const float i33 = pc.u_inv_view_proj[3][3];
    const float graze = max(ssr.blend_tol.z, 1.0);
    float zOut = 0.0;   // (sky: nothing opaque -- everything passes)
    if (zO > 0.0)
    {
        if (mb.flags.z != 0u || abs(kz) < 1e-12)
        {   // orthographic: depth is linear (metres per unit of depth: |column 2|), the base tolerance only
            const float mpz = max(length(vec3(pc.u_inv_view_proj[2].xyz)), 1e-12);
            zOut = max(zO - ssr.blend_tol.x * graze * 1.01 / mpz, 0.0);
        }
        else
        {
            const float vO = 1.0 / max(kz * zO + i33, 1e-12);   // view depth of the opaque surface
            // (the widest window: distance along the ray <= 2 x view depth inside any sane field of view)
            const float d = 2.0 * vO;
            const float tol = max((ssr.blend_tol.x + ssr.blend_tol.y * d) * graze, 2e-5 * d) * 1.01;
            zOut = max((1.0 / (vO + tol) - i33) / kz, 0.0);
        }
    }
    if (mb.flags.y != 0u && (mb.flags.x & MBD_NOCUT) == 0u && mb.flags.z == 0u && abs(kz) >= 1e-12)
    {
        float zc;
        if (mb.sizes.z == 0u) zc = mbCutAt(px);
        else
        {
            const ivec2 ms = ivec2(mb.sizes.x & 0xFFFFu, mb.sizes.x >> 16u);
            const ivec2 b = ivec2(floor((vec2(px) + 0.5) * 0.5 - 0.5));
            zc = max(max(mbCutAt(clamp(b, ivec2(0), ms - 1)), mbCutAt(clamp(b + ivec2(1, 0), ivec2(0), ms - 1))),
                     max(mbCutAt(clamp(b + ivec2(0, 1), ivec2(0), ms - 1)), mbCutAt(clamp(b + ivec2(1, 1), ivec2(0), ms - 1))));
        }
        if (zc <= 1.0)
        {
            // warped depth -> view depth (inverse of mbWarpWith) -> reversed-Z depth along the pixel's ray
            const float vC = exp((zc + 1.0) / mb.warp.y + mb.warp.x);
            const float zC = (1.0 / vC - i33) / kz;
            zOut = max(zOut, clamp(zC, 0.0, 1.0));
        }
    }
    gl_FragDepth = zOut;
}
