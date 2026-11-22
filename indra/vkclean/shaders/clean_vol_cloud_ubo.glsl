// vkclean -- VOLUMETRIC CLOUDS (#719): the module's per-recording parameter block
// (LLCleanVolCloudsVK::Params, vkCmdUpdateBuffer before the first pass that reads it).
// Byte layout = LLCleanVolCloudsVK::Params (std140: vec4 / mat4 only). Set VC_SET, binding 0.
#ifndef CLEAN_VOL_CLOUD_UBO_GLSL
#define CLEAN_VOL_CLOUD_UBO_GLSL
#ifndef VC_SET
#define VC_SET 1
#endif
#ifndef VC_BINDING
#define VC_BINDING 0
#endif
#ifndef CLEAN_MAX_VIEWS   // (#1608: the view tail's bound = clean_view.glsl's; every includer has the frame info first)
#define CLEAN_MAX_VIEWS 4
#endif
layout(std140, set = VC_SET, binding = VC_BINDING) uniform VolCloudParams {
    vec4 shape0;   // x weather tile (m), y 1 / tile, z 1 / base noise period (m), w 1 / detail period (m)
    vec4 shape1;   // xy wind offset of the 3D noises (m), z extinction per m at density 1, w detail erosion
    vec4 shape2;   // x ambient scale, y max distance (m), z multi-scatter strength, w powder strength
    vec4 phase;    // x forward g, y backward g, z backward lobe weight, w (unused)
    vec4 march;    // x max steps (main view), y light steps, z frame index, w 1 = offscreen (snapshot)
    vec4 cam;      // xyz camera world position, w time (s)
    vec4 shadow;   // shadow map: xy centre (m), z extent (m), w steps
    vec4 pano;     // x first row, y rows this dispatch, z steps, w history blend (0 = overwrite)
    // Main view (march / temporal / composite):
    mat4 inv_vp;   // UNJITTERED VK clip -> CAMERA-RELATIVE world (this frame)
    mat4 reproj;   // camera-relative world (this frame) -> last frame's unjittered clip
    vec4 dims;     // render w, h, march w, h
    vec4 pat;      // x pattern (0 all, 1 checkerboard, 2 one per 2x2), y frame phase, z steps, w light steps
    vec4 temporal; // x max history (frames), y 1 = history reset, z clamp widening, w (unused)
    vec4 flash;    // #783 M2 lightning: xyz unit direction to the strike (world), w its radiance (0 = none)
    vec4 bodies;   // sculpted cloud bodies (RenderVKCloudShapes; GunMetal-DX port): x shapes loaded
                   // (0 = none), y their amount (0 = off), zw 0
    vec4 weather_map;   // agent corner xy, inverse world extent, extent
    vec4 weather_noise; // advected global corner / macro scale, inverse macro scale, reserved
    vec4 weather_eep;   // global agent-origin tile phase xy, rotated world corner phase zw
    // ---- (#1608) THE STEREO VIEW TAIL (appended: nothing above moves) ----
    // view v's inv_vp / reproj about ITS OWN eye (cam_v[v].xyz; w unused) -- the march / temporal / composite of a
    // layered stereo frame read them (VC_INV_VP / VC_REPROJ / VC_CAM). Every view filled (the desktop's view 0 = the
    // fields above); read only under kViewCount > 1.
    mat4 inv_vp_v[CLEAN_MAX_VIEWS];
    mat4 reproj_v[CLEAN_MAX_VIEWS];
    vec4 cam_v[CLEAN_MAX_VIEWS];
} vc;
// (#1608) this invocation's view: the march / temporal kernels z = N (CLEAN_VIEW_Z), the composite multiview
// (gl_ViewIndex); every target a layer per view (VC_L). One view: the fields of before, layer 0.
#define VC_INV_VP (kViewCount > 1 ? vc.inv_vp_v[CLEAN_VIEW] : vc.inv_vp)
#define VC_REPROJ (kViewCount > 1 ? vc.reproj_v[CLEAN_VIEW] : vc.reproj)
#define VC_CAM (kViewCount > 1 ? vc.cam_v[CLEAN_VIEW].xyz : vc.cam.xyz)
#define VC_L int(CLEAN_VIEW)
#endif
