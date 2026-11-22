// vkclean -- A LOCAL LIGHT'S SHADOW RAY AT A RAY-TRACED RECEIVER: the light list a hit walks (the main-view
// cluster it projects into, or every light) and the hard shadow ray toward a light's centre / apex
// (rtrProjRaySetup / rtrProjRay). Shared by the RT reflection / GI hits (clean_deferred_light.comp, through
// clean_rt_hit_lights.glsl's RT_SHADE_LIGHT_CLUSTER / RT_SHADE_PROJ_RAY hooks) and the forward alpha-blend
// layers' own light (clean_draw.frag CLEAN_BLEND_HIT), which are shaded the way a hit is.
// Requires: clean_lights_common.glsl + the light buffers, clean_rt_common.glsl, clean_vis_walk.glsl (rtVisTraceK),
// mainClusterProject. DEFER_GI_TRACE (the GI trace kernels) and CLEAN_BLEND_HIT (the blend layers) add the light's
// own fixture rule (RTR_FIX_MAX).
#ifndef CLEAN_RT_LIGHT_RAY_GLSL
#define CLEAN_RT_LIGHT_RAY_GLSL
// Local lights at a hit: the main-view cluster it projects into while it lies inside the view (the
// cluster lists are culled to their cells), else every light of the frame (each light's range
// test decides: off-screen hits -- behind the camera, beside the view -- no longer take an edge
// cell's lights).
#define RT_SHADE_ALL_LIGHTS 0xFFFFFFFFu
uint rtrLightCluster(vec3 P)
{
    const vec4 v = linfo.view * vec4(P, 1.0);   // -Z forward
    const float z = -v.z;
    if (z <= 1e-3) return RT_SHADE_ALL_LIGHTS;
    const float sx = (v.x / CL_VIEW_Z(linfo, z)) / max(linfo.grid.z, 1e-4);
    const float sy = (v.y / CL_VIEW_Z(linfo, z)) / max(linfo.grid.w, 1e-4);
    if (abs(sx) > 1.0 || abs(sy) > 1.0) return RT_SHADE_ALL_LIGHTS;
    return mainClusterProject(P);
}
// A ray-traced projector record at a hit: one hard shadow ray toward its origin, from the hit
// on the light's side of its geometric plane, over the OPAQUE casters only (alpha-mask cut-outs still
// alpha-tested): the alpha-blend class -- light-beam cones, glow cards, haze sheets, which in a
// club sit right along every stage light's own path -- never becomes a candidate here (the main
// view keeps their translucent tint; a reflection hit's projector shadow does not pay a texture
// fetch per beam prim).
// (#1039 M2) its set-up apart (the visibility queue's producer queues the very same ray): the origin o, the
// direction l, the length tmax, and whether the light's own fixture rule applies (fix; rt_fix_P = its centre,
// rt_fix_max = gi.prm5.z). Returns false for a hit turned away from the light under the back-face self-shadow:
// its own surface shadows it -- visibility 0, no ray.
// THE ORIGIN (clean_rt_ray_origin.h): Ng = the hit's GEOMETRIC normal on the side its ray arrived from (a forward
// fragment: the side the camera sees), err = its position error beyond the float error (a ray-traced hit: 0; a
// rasterized fragment: rtReceiverErr(camd)); the side by rtHitLightSide, the offset by rtHitOrigin -- the sun's rule
// at a hit (clean_rt_shade.glsl rtHitSunRay) and the main view's receivers'. The old origin, 1 cm along the SHADING
// normal turned toward the light, jumped thin geometry: an ear, a roof's trim over its shingles, two faces a few mm
// apart -- a lamp lit hits it could not reach, the rare bright GI hits the reservoirs spread as fireflies.
#define RTR_PROJ_MASK (RT_MASK_SHADOW & ~RT_MASK_BLEND)
// THE OWN-FIXTURE RULE's size limit (m) where this file's rays take it: the GI trace kernels (gi.prm5.z) and the
// forward blend layers (light info fwd.w, clean_draw_common.glsl blendHitFixMax; written every lit frame -- the GI
// header is not). Both RenderVKReSTIRFixtureSize, pass 15's. Without it a point light inside an opaque bulb / shade /
// panel prim larger than its 2 cm stop (RS_POINT_NEAR_MAX) is blocked by that prim on every ray: the layer lost the
// light over its whole reach (and kept its glare: a dark disc).
#if DEFER_GI_TRACE
#define RTR_FIX_MAX gi.prm5.z
#elif defined(CLEAN_BLEND_HIT)
#define RTR_FIX_MAX blendHitFixMax()
#endif
bool rtrProjRaySetup(Light L, vec3 P, vec3 Ng, float err, out vec3 o, out vec3 l, out float tmax, out bool fix)
{
    // (#87 GI, RT_SHADE_POINT_BUDGET) a POINT light in the budget: toward its centre, stopping at its
    // fixture sphere (proj_axis.x = its prim's bounding radius, pass 15's SH_SPHERE rule: the bulb
    // never shadows its own light)
    const bool point = L.proj_origin.w < 0.5;
    const vec3 lv = (point ? L.pos_radius.xyz : L.proj_origin.xyz) - P;
    const float d = length(lv);
    l = lv / max(d, 1e-4);
    vec3 n;
    const bool ray = rtHitLightSide(Ng, l, rtBackfaceSelfShadow(), n);
    o = rtHitOrigin(P, n, err);
    const float stop = point ? clamp(L.proj_axis.x, 0.0, RS_POINT_NEAR_MAX) : 0.05;   // (pass 15's cap)
    tmax = max(d - stop, 0.0);
#ifdef RTR_FIX_MAX
    // (#1041) ... and its OWN FIXTURE (pass 15's SH_FIXTURE, clean_vis_walk.glsl): a lamp's shade /
    // lantern / sconce around the point -- a small box holding its centre -- neither shadows nor tints
    // its light; the ray is traced again up to where it enters that box. (Projectors: their near
    // plane is that clip already.)
    fix = point && RTR_FIX_MAX > 0.0;
#else
    fix = false;
#endif
    return ray;
}
// THE HIT the light walk (clean_rt_hit_lights.glsl rtHitLocalLights -> RT_SHADE_PROJ_RAY) is shading: its geometric
// normal on the arrival side and its position error (rtrProjRaySetup's Ng / err). Set by every caller of the walk
// right before it (rtShadeHit, the visibility queue's producer, blendHitLocal).
vec3  rt_hit_ng  = vec3(0.0, 0.0, 1.0);
float rt_hit_err = 0.0;
// (N: the shading normal the walk lit the hit with -- not the origin's; rt_hit_ng is)
#ifdef RTR_LAMP_WHY_ON
// (RenderVKGIDebug 11, clean_gi_lamp_why.h) the reasons of every lamp ray of the hit being shaded that came back
// with light (GLW_PASSED | the walk's rt_walk_why | GLW_FACING when the ray re-traced without the facing cull is
// darker); the caller clears it before the hit's shading and reads it after. Needs RT_WALK_WHY (clean_vis_walk.glsl).
uint g_gi_lamp_why = 0u;
#endif
// (RenderVKBlendFrontLight; clean_deferred_light.comp pass 14 rfvLight) g_rtr_novis: every visibility 1 and no ray --
// the same walk, picks and terms unshadowed, for THE LAYER'S LIGHT's shadowed / unshadowed ratio. False everywhere else.
bool g_rtr_novis = false;
vec3 rtrProjRay(Light L, vec3 P, vec3 N)
{
    if (g_rtr_novis) return vec3(1.0);
    vec3 o, l;
    float tmax;
    bool fix;
    if (!rtrProjRaySetup(L, P, rt_hit_ng, rt_hit_err, o, l, tmax, fix)) return vec3(0.0);
#ifdef RTR_FIX_MAX
    rt_fix_on = fix;
    rt_fix_P = L.pos_radius.xyz;
    rt_fix_max = RTR_FIX_MAX;
#endif
#ifdef RTR_LAMP_WHY_ON
    const bool why = RTR_LAMP_WHY_ON;
    rt_walk_why = 0u;
#endif
    rtFacingCullSet(ssr.foliage_rt);   // (clean_rt_common.glsl: the sun's facing cull, shadow-map parity)
    const vec3 v = rtVisTraceK(o, l, tmax, RTR_PROJ_MASK, RT_WALK_RGB, 1u << RT_WALK_RGB);
    rtFoliageEnd();
#ifdef RTR_LAMP_WHY_ON
    if (why && max(max(v.r, v.g), v.b) > 0.0)
    {
        uint w = rt_walk_why | GLW_PASSED;
        // (the hardware facing cull drops faces before the walk sees them: the same ray without it)
        const vec3 v2 = rtVisTraceK(o, l, tmax, RTR_PROJ_MASK, RT_WALK_RGB, 1u << RT_WALK_RGB);
        if (max(max(v2.r, v2.g), v2.b) < max(max(v.r, v.g), v.b)) w |= GLW_FACING;
        g_gi_lamp_why |= w;
    }
#endif
    rt_fix_on = false;
    return v;
}
// THE OCCLUSION RAY of a reflection (the RT reflection hits under GI, clean_deferred_light.comp rtrOccRay / rtrHitEnv;
// the forward blend layers' environment, clean_surface_light.glsl blendHitEnv): from P along R, R bent back onto the
// arrival side of the receiver's plane (ng: its geometric normal turned toward the side the view / reflection ray
// arrived from) where it grazes / crosses it, the origin lifted off that plane on that side by its position error
// (err: rtHitOrigin's -- 0 at a ray-traced hit, rtReceiverErr(camd) at a forward fragment; it was a flat 1 cm,
// through thin layers); bent: whether R was bent.
// Traced to RTR_OCC_FAR against RT_MASK_WORLD_BLOCK: a first-hit query, and only a true miss is the open sky.
#define RTR_OCC_FAR 4096.0
// (One body, a statement macro: rtrOccRay expands it in place, exactly the statements it always ran.)
#define RT_OCC_RAY_SETUP(P, R, ng, err, o, Ro, bent) \
    Ro = R; \
    const float rn = dot(Ro, ng); \
    bent = rn < 0.02; \
    if (bent) Ro = normalize(Ro + ng * (0.02 - rn)); \
    o = rtHitOrigin(P, ng, err);
void rtOccRaySetup(vec3 P, vec3 R, vec3 ng, float err, out vec3 o, out vec3 Ro, out bool bent)
{
    RT_OCC_RAY_SETUP(P, R, ng, err, o, Ro, bent)
}
#endif
