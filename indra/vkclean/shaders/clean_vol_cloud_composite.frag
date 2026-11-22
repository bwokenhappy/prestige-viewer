#version 450
#extension GL_GOOGLE_include_directive : require
// vkclean -- VOLUMETRIC CLOUDS (#719): the main-view COMPOSITE (render resolution).
//
// A fullscreen draw over the HDR scene after pass 1 (the opaque world + the sky), before the
// pass-2 transparents / water (they draw over the clouds; the water refraction copy and next
// frame's SSR see them). Blend ONE / SRC_ALPHA: scene' = scene x T + in-scatter.
//
// DEPTH-AWARE UPSAMPLE: the four quarter-resolution texels around the pixel are weighted
// bilinearly, and a texel's clouds only apply where this pixel's surface lies BEYOND the
// texel's cloud entry distance (B.x): a tree, a roof or a mountain in front of the layer
// keeps T = 1 even where its 2x2 block marched to the sky behind it. Pixels inside a cloud
// (entry < surface < the march end) take the texel's full result: a slight over-density
// there, not a halo.
layout(location = 0) out vec4 outColor;

#include "clean_frame_info.glsl"
#include "clean_atmos_common.glsl"
#define VC_SET 1
#include "clean_vol_cloud_ubo.glsl"
// (#1608 LAYERED STEREO: a multiview pass -- every input a layer per view, read at this view's, VC_L; the view's own
//  eye / unprojection, VC_INV_VP / VC_CAM)
layout(set = 1, binding = 1) uniform sampler2DArray uDepth;   // world depth (texelFetch)
layout(set = 1, binding = 2) uniform sampler2DArray uCloudA;  // rgb in-scatter, a transmittance
layout(set = 1, binding = 3) uniform sampler2DArray uCloudB;  // x entry distance, y mean distance

void main()
{
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const vec2 rdim = vc.dims.xy, cdim = vc.dims.zw;
    const float d = texelFetch(uDepth, ivec3(px, VC_L), 0).r;
    float dist = 1e9;   // sky
    if (d > 0.0)
    {
        vec2 ndc = (vec2(px) + 0.5) / rdim * 2.0 - 1.0;
        const vec4 w = VC_INV_VP * vec4(ndc, d, 1.0);   // (jitter: sub-pixel, irrelevant at km)
        dist = length(w.xyz / w.w);
        if(cmOn())
        {
            vec3 O,V;cmRay((vec2(px)+0.5)/rdim,MEDIUM_EYE,O,V);
            dist=max(dot(VC_CAM+w.xyz/w.w-O,V),0.0);
        }
    }
    // Bilinear footprint in the cloud image.
    const vec2 cp = (vec2(px) + 0.5) * (cdim / rdim) - 0.5;
    const ivec2 c0 = ivec2(floor(cp));
    const vec2 f = cp - vec2(c0);
    const ivec2 cmax = ivec2(cdim) - 1;
    vec4 acc = vec4(0.0);
    float wsum = 0.0,meanNumerator=0.0,meanDenominator=0.0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i)
        {
            const ivec2 q = clamp(c0 + ivec2(i, j), ivec2(0), cmax);
            const float w = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
            vec4 a = texelFetch(uCloudA, ivec3(q, VC_L), 0);
            const vec2 distances=texelFetch(uCloudB,ivec3(q, VC_L),0).xy;
            const float entry = distances.x;
            if(dist>entry)
            {
                const float opacityWeight=w*(1.0-a.a);
                meanNumerator+=opacityWeight*distances.y;
                meanDenominator+=opacityWeight;
            }
            // In front of this texel's clouds: nothing (T = 1).
            acc += w * (dist > entry ? a : vec4(0.0, 0.0, 0.0, 1.0));
            wsum += w;
        }
    acc /= max(wsum, 1e-6);
    if (acc.a >= 0.999 && dot(acc.rgb, vec3(1.0)) <= 1e-5) discard;
    // LIGHTNING (#783 M2, vc.flash): the flash lights the clouds from within -- in proportion to
    // their opacity, most toward the strike (cool white).
    if (vc.flash.w > 0.0)
    {
        const vec2 ndc = (vec2(px) + 0.5) / rdim * 2.0 - 1.0;
        const vec4 w = VC_INV_VP * vec4(ndc, 0.5, 1.0);
        const vec3 dir = normalize(w.xyz / w.w);
        const float toward = max(dot(dir, vc.flash.xyz), 0.0);
        acc.rgb += vec3(0.80, 0.88, 1.0) * (vc.flash.w * (1.0 - acc.a) * (0.3 + 0.7 * toward * toward));
    }
    if(cmOn())
    {
        vec3 O,V;cmRay((vec2(px)+0.5)/rdim,MEDIUM_EYE,O,V);
        const float mean=meanNumerator/max(meanDenominator,1e-7);
        const CameraTransport prefix=cmAt(O+V*mean,MEDIUM_EYE);
        acc.rgb=prefix.T*acc.rgb+(1.0-acc.a)*prefix.L;
    }
    outColor = vec4(max(acc.rgb, vec3(0.0)), clamp(acc.a, 0.0, 1.0));
}
