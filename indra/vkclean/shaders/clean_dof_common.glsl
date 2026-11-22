// vkclean — depth of field (RenderDepthOfField), port of LL's renderDoF chain:
// cofF (colour + circle of confusion) -> postDeferredF (gather at CameraDoFResScale)
// -> dofCombineF. Runs on the LINEAR HDR scene ahead of bloom and the tonemapper
// (upstream Alchemy's order: gathering display-space values gathers already
// compressed highlights). Shared push block for the three passes.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;
// (#1608 LAYERED STEREO: multiview passes -- every input a layer per view, sampled at this view's (DOF_UV / DOF_L);
//  the CoC's depth linearisation (inv_proj's z / w rows) is the same for every eye: their projections differ only in
//  the x / y offsets)
#include "clean_view.glsl"
#define DOF_L int(CLEAN_VIEW)
#define DOF_UV(uv) vec3((uv), float(CLEAN_VIEW))

layout(push_constant) uniform DofPush {
    mat4 inv_proj;   // VK clip -> view (current frame)
    vec4 lens;       // focal_distance (view z, m), blur_constant, tan_pixel_angle, magnification
    vec4 params;     // max_cof (px), res_scale, near blur (0/1), -
    vec4 screen;     // full-res width, height, 1/width, 1/height
    vec4 jitter;     // TAA jitter (render px, x right / y down; 0 when the colour is not resolved),
                     // closest-of-2x2 depth (1 = scaled frame), -, -   (128 B: the push budget)
} pc;
