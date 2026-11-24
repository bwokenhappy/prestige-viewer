#version 450
// vkclean — DoF pass 3 (LL dofCombineF.glsl): blend the reduced-resolution DoF
// result over the sharp image by the circle of confusion. Writes the linear HDR
// scene in place (colour only: the pipeline blend keeps the target's alpha).
#include "clean_dof_common.glsl"
layout(set = 0, binding = 0) uniform sampler2DArray uDof;     // gather result (LINEAR upsample)
layout(set = 0, binding = 1) uniform sampler2DArray uCoC;     // colour + CoC (NEAREST)

void main()
{
    const float max_cof = pc.params.x, res_scale = pc.params.y;
    vec4 dof  = texture(uDof, DOF_UV(v_uv));
    vec4 diff = texture(uCoC, DOF_UV(v_uv));
    // <Prestige> (focus peaking reads the pixel's own colour and CoC, before the transition blend)
    const vec4 sharp = diff;
    // </Prestige>
    // Without near blur (RenderDepthOfFieldNearBlur off, the upstream default) the
    // foreground (CoC above the midpoint) stays sharp.
    float a = (pc.params.z <= 0.5 && diff.a > 0.5) ? 0.0
            : min(abs(diff.a * 2.0 - 1.0) * max_cof * res_scale * res_scale, 1.0);
    if (a > 0.25 && a < 0.75)
    {   // help out the transition a bit
        float sc = a / res_scale;
        vec4 col;
        col  = texture(uCoC, DOF_UV(v_uv + vec2( sc,  sc) * pc.screen.zw));
        col += texture(uCoC, DOF_UV(v_uv + vec2(-sc,  sc) * pc.screen.zw));
        col += texture(uCoC, DOF_UV(v_uv + vec2( sc, -sc) * pc.screen.zw));
        col += texture(uCoC, DOF_UV(v_uv + vec2(-sc, -sc) * pc.screen.zw));
        diff = mix(diff, col * 0.25, a);
    }
    vec3 rgb = mix(diff, dof, a).rgb;
    // <Prestige> focus peaking (camera mode, MF; params.w = 1 on screen only, never in a shot): the
    // edges of what is in focus -- a blur circle of at most one pixel (prestigecamerafocus.h
    // SHARP_COC_PX) with a strong local contrast in the sharp image -- drawn in red.
    if (pc.params.w > 0.5 && abs(sharp.a * 2.0 - 1.0) * max_cof <= 1.0)
    {
        const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
        const vec2 px = pc.screen.zw;
        const float l  = dot(sharp.rgb, lw);
        const float lr = dot(texture(uCoC, DOF_UV(v_uv + vec2(px.x, 0.0))).rgb, lw);
        const float ll = dot(texture(uCoC, DOF_UV(v_uv - vec2(px.x, 0.0))).rgb, lw);
        const float lu = dot(texture(uCoC, DOF_UV(v_uv + vec2(0.0, px.y))).rgb, lw);
        const float ld = dot(texture(uCoC, DOF_UV(v_uv - vec2(0.0, px.y))).rgb, lw);
        const float mean = (l + lr + ll + lu + ld) * 0.2;
        // Relative contrast: the same edges at any exposure (the scene is linear HDR here).
        const float edge = length(vec2(lr - ll, lu - ld)) / max(mean, 1e-3);
        const float k = smoothstep(0.25, 0.6, edge);
        if (k > 0.0)
            rgb = mix(rgb, vec3(1.0, 0.08, 0.04) * max(mean * 3.0, 0.35), k);
    }
    // </Prestige>
    outColor = vec4(rgb, 1.0);
}
