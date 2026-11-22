// vkclean -- GOD RAYS (#208): the LAYERED STEREO view block (#1608; vkgfx/llcleangodraysvk.cpp).
//
// The god rays' pushes are full (128 B each), so a layered stereo frame's per-view cameras ride a small UBO of their
// own (a dynamic uniform buffer, one region per frame slot): view v's unjittered VK clip -> (P - its eye) and that
// eye, its reprojection to last frame's clip, and the temporal passes' view-ray coefficients (xyz) with the eye's
// move since last frame (w). Every view is filled (the desktop's view 0 = the pushes' values); read only under
// kViewCount > 1 -- one view folds every GR_* to the push field of before. The includer defines GR_VIEW_SET /
// GR_VIEW_BINDING and has clean_view.glsl (kViewCount, CLEAN_VIEW) included first.
#ifndef CLEAN_GODRAYS_VIEW_GLSL
#define CLEAN_GODRAYS_VIEW_GLSL
#ifndef CLEAN_MAX_VIEWS
#define CLEAN_MAX_VIEWS 4
#endif
struct GrView
{
    mat4 inv_vp;   // UNJITTERED VK clip -> (world - cam.xyz)
    mat4 reproj;   // (world - cam.xyz) this frame -> last frame's unjittered clip of this view
    vec4 cam;      // xyz this view's eye (world), w unused
    vec4 ray_a;    // the temporal passes' view ray: xyz = ray_a ndc.x + ray_b ndc.y + ray_c, w = the eye's move
    vec4 ray_b;
    vec4 ray_c;
};
layout(std140, set = GR_VIEW_SET, binding = GR_VIEW_BINDING) uniform GodRaysViews { GrView grv[CLEAN_MAX_VIEWS]; };
// This invocation's layer: the kernels z = N (CLEAN_VIEW_Z), the composite multiview (gl_ViewIndex).
#define GR_L int(CLEAN_VIEW)
#endif
