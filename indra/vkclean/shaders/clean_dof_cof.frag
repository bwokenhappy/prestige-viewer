#version 450
// vkclean — DoF pass 1 (LL cofF.glsl): scene colour + signed circle of confusion.
#include "clean_dof_common.glsl"
layout(set = 0, binding = 0) uniform sampler2DArray uColor;   // tonemapped scene (NEAREST)
layout(set = 0, binding = 1) uniform sampler2DArray uDepth;   // world depth, render size (texelFetch)

// The depth for this DISPLAY pixel. mHdr is the resolved (unjittered) colour, display size
// under DLSS / FSR; the world depth is render size and jittered. A nearest read at v_uv
// took a render texel up to a whole render pixel from the display pixel's centre, moving
// with the jitter every frame and in blocks of display / render pixels, so the CoC
// silhouette of a foreground object flickered: an avatar-coloured pixel given the far
// background's CoC is accepted into every background neighbour's gather and smears the
// avatar's colour across the whole disc (the avatar-shaped flicker).
// Render texel q shows the scene at q + 0.5 - jitter (as clean_depth_up.frag), so:
//   native / DLAA: the texel whose sample is nearest the unjittered centre;
//   scaled: the closest (reversed-Z: max) of the 2x2 texels whose samples bracket it. A
//   pixel showing the foreground always gets the foreground's CoC; the error left is a
//   band of at most one render pixel of background that keeps the foreground's (sharp)
//   CoC -- rejected by its neighbours' gathers, so it does not spread.
float cocDepth()
{
    const ivec2 rsize = textureSize(uDepth, 0).xy;
    const ivec2 lim = rsize - 1;
    const vec2 pr = v_uv * vec2(rsize) + pc.jitter.xy;
    if (pc.jitter.z < 0.5)
        return texelFetch(uDepth, ivec3(clamp(ivec2(floor(pr)), ivec2(0), lim), DOF_L), 0).r;
    const ivec2 q0 = ivec2(floor(pr - 0.5));
    const ivec2 a = clamp(q0, ivec2(0), lim), b = clamp(q0 + 1, ivec2(0), lim);
    float z = texelFetch(uDepth, ivec3(a, DOF_L), 0).r;
    z = max(z, texelFetch(uDepth, ivec3(ivec2(b.x, a.y), DOF_L), 0).r);
    z = max(z, texelFetch(uDepth, ivec3(ivec2(a.x, b.y), DOF_L), 0).r);
    z = max(z, texelFetch(uDepth, ivec3(b, DOF_L), 0).r);
    return z;
}

float calc_cof(float depth)
{
    const float focal_distance = pc.lens.x, blur_constant = pc.lens.y;
    const float tan_pixel_angle = pc.lens.z, magnification = pc.lens.w;
    float sc = (depth - focal_distance) / -depth * blur_constant;
    sc /= magnification;
    // tan_pixel_angle = pixel_length/-depth;
    float pixel_length = tan_pixel_angle * -focal_distance;
    sc = sc / pixel_length;
    sc *= 1.414;
    return sc;
}

void main()
{
    float z = cocDepth();
    vec4 p = pc.inv_proj * vec4(0.0, 0.0, z, 1.0);   // VK NDC z = depth
    float depth = p.z / p.w;
    vec4 diff = texture(uColor, DOF_UV(v_uv));
    const float max_cof = pc.params.x;
    float sc = clamp(calc_cof(depth), -max_cof, max_cof);
    outColor = vec4(diff.rgb, sc / max_cof * 0.5 + 0.5);
}
