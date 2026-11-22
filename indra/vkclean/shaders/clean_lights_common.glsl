// vkclean — local-light data shared by clean_lights.comp and clean_draw.frag.
// Byte layout matches LLCleanRendererVK's VKLightGPU / VKLightInfo (std430).

#define GX 16u
#define GY 9u
#define GZ 24u
#define CLUSTER_COUNT (GX * GY * GZ)
// CLUSTER_CAP (= LLCleanRendererVK::CLUSTER_CAP): 127, the packing's limit (clusterShadowedLen: 7 bits).
// Was 64: a light-dense view (StallionsDale 2026-11-13, ~200 lights, an overhead camera whose far depth
// slices span tens of metres) overflowed ~15 clusters per frame at up to 112 lights. The cap is a list
// BUDGET, never a light limit: a consumer that must see every light (the shadow paths) walks the whole
// light table where its list overflowed (clusterWalkAll / clusterShadowedWalkAll below).
#define CLUSTER_CAP 127u
// CAPTURE LIGHT CAP (#829): the reflection-probe capture FS (clean_draw.frag -DCLEAN_CAPTURE)
// walks only the nearest this many lights of its probe's list; clean_probe_gpu.comp phaseLights
// sorts a list by distance to the probe origin whenever it holds more.
#define CAPTURE_LIGHT_CAP 16u
// A point light's shadow rays stop short of its centre by its prim's bounding radius (proj_axis.x) at
// most this much (metres): a sign's or panel's half-diagonal otherwise let the wall right behind its
// light pass rays from the next room (light leaking through the wall). The lamp's own shade / bulb /
// lantern is the own-fixture rule's (RenderVKReSTIRFixtureSize), not this clip's.
#define RS_POINT_NEAR_MAX 0.02

// cluster_count[c] packing (clean_lights.comp; clean_probe_gpu.comp's capture lists set the
// list length only):
//   bits  0-7   list length (<= CLUSTER_CAP)
//   bits  8-14  how many SHADOWED projectors (tex.z != 0) lead the list -- they are listed
//               first so the cap never drops one while it has room (the RT shadow mask and
//               the stochastic sampler trace only the projectors its cluster lists: one the
//               cap dropped is lit UNSHADOWED by the lossless froxel / tile lists, white tile
//               x depth-slice blocks where the cap overflowed -- bit 15, view 26 magenta)
//   bit   15    the cluster's shadowed projectors alone exceeded the cap
//   bits 16-31  every light that touched the cluster (> CLUSTER_CAP: overflow)
// THE REST SLOT (exact projector shadows, RenderVKProjStochastic 0): the RT shadow mask
// (clean_rt_shadow_mask.comp) and the deferred SHADOW resolve (clean_deferred_light.comp
// deferResolveProjectors) keep 4 visibility slots per pixel. When more shadowed projectors
// light the pixel, slots 0-2 keep their own projectors and slot 3 becomes the REST slot: every
// other one of them traced and folded into one light-weighted visibility ratio (sum of
// light x vis / sum of light, the light being the projector's unshadowed gobo x attenuation at
// the pixel). A reader that does not find its projector in a slot takes the REST slot's value
// when there is one -- never visibility 1 (the 5th+ projector used to be lit UNSHADOWED: a club's
// projector in the next room painted its gobo through the wall). Over the rest set the ratio
// reproduces the shadowed sum exactly where the readers' light equals the producer's weight.
#define PROJ_MASK_REST_ID 0xFFFFu   // the RT mask's ids (rgba16ui; light index + 1 elsewhere)
// MOVER ROUTING (clean_rt_shadow_mask.comp, RenderVKShadowMoverMask): the mover lights use these slots as the RT
// projectors do -- an own slot per light, the REST slot the ratio sum w Vs Vm / sum w Vs over the others (the RATIO
// mode; the radiance packing folds at the pixel, below). (0xFFFE, the old two-sum REST's denominator, and 0xFFFD /
// 0xFFFC, the radiance REST's two specular ratios, are retired: the exact fold takes each light's own specular term.)
// THE EXACT PIXEL FOLD (radiance packing, ANY number of mover lights at a texel; clean_rt_shadow_mask.comp mvFoldPlan,
// clean_deferred_light.comp deferMoverPixelFold). Pass 15's radiance is one sum over its lights, so a mover can only
// scale it, by R = sum t_i Vs_i Vm_i / sum t_i Vs_i over EVERY light pass 15 carries at the pixel (t_i: projDirectTerms'
// diffuse / specular term of light i there; Vm its mover visibility, Vs its static one). The mask stores, per texel:
//   * EXPLICIT lights: the lights a mover lies in front of (the mask's segHit cull -- never this frame's Vm < 1, whose
//     outer penumbra flips frame to frame: a slot must not come and go with that noise, or its history never forms)
//     that some static light reaches, lowest LIGHT INDEX first (a key every texel ranks alike -- never a per-texel
//     importance order, whose slot set changes between neighbours: a seam), each its Vm (id light + 1; 1 where it is
//     not blocked) and, in a static shadow (Vs luminance < MV_VS_FULL), its Vs (id MV_VS_TAG | light + 1); then other
//     lights in a static shadow, lowest index first, a Vs slot alone (Vm reads 1). Where no blocked light is
//     statically reached the unblocked hit lights' Vm 1 slots alone (R is 1 there; the slots keep their histories).
//   * THE FREE SHARES of every other light the walk traced (U): MV_FOLD_N_ID = sum_U w Vs Vm / sum_U w and
//     MV_FOLD_D_ID = sum_U w Vs / sum_U w (w the mask's unshadowed weight) -- written only when they are not 1 (D:
//     some U light in a static shadow; N: a mover-blocked light left over past the 4 slots). A missing slot reads 1
//     (N: reads D).
// The lighting walks pass 15's lights at the pixel: an explicit light adds t Vs Vm / t Vs with its own slots, every
// other light adds its t to T_U, and R = (sum_K t Vs Vm + N T_U) / (sum_K t Vs + D T_U). EXACT wherever the U lights
// are unblocked and statically lit alike (no N / D slot: the open floor of any number of lights), and wherever the hit
// lights fit the slots (<= 4 in no static shadow, or fewer in one); the mask's weights only split the U set's
// OWN light between its members (never the explicit lights' share against it, the old REST's lit sliver).
#define MV_VS_TAG  0x4000u
#define MV_VS_OUT  0.02
#define MV_VS_FULL 0.98
#define MV_FOLD_N_ID 0xFFFBu
#define MV_FOLD_D_ID 0xFFFAu
// The pixel fold's ratio num / den per channel (num = sum t Vs Vm, den = sum t Vs), clamped to [0, 1]; the luminance
// ratio where a channel barely has light (the REST's rule); no light at all: 1 (nothing a mover can take away). (Also
// the mask's free shares N / D: a part of the U set's unshadowed weight over that weight.)
vec3 mvPixelRatio(vec3 num, vec3 den)
{
    const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
    const float dl = dot(den, lw);
    if (!(dl > 1e-8)) return vec3(1.0);
    const float rl = clamp(dot(num, lw) / dl, 0.0, 1.0);
    vec3 r;
    for (int ch = 0; ch < 3; ++ch) r[ch] = den[ch] > 1e-3 * dl ? clamp(num[ch] / den[ch], 0.0, 1.0) : rl;
    return r;
}
#ifndef PROJ_DEFER_REST_ID          // (clean_deferred_light.comp defines it before this include)
#define PROJ_DEFER_REST_ID 0xFFu    // the deferred SHADOW slots' 8-bit ids (lights 0..253 listed)
#endif

uint clusterLen(uint raw)            { return min(raw & 0xFFu, CLUSTER_CAP); }
uint clusterShadowedLen(uint raw)    { return min((raw >> 8) & 0x7Fu, clusterLen(raw)); }
bool clusterShadowedOverflow(uint raw) { return (raw & 0x8000u) != 0u; }
uint clusterCandidates(uint raw)     { return raw >> 16; }
// LOSSLESS CLUSTER WALKS (2026-11-13, light / shadow dropout): a list that overflowed CLUSTER_CAP lost the
// lights past the cap (camera-importance order) -- a shadow consumer that walked it alone gave those lights
// no shadow: lit UNSHADOWED by the lossless froxel walk, in tile x depth-slice blocks that popped on a small
// camera move. Where these say so, the consumer walks the whole light table [0, count) instead (index i =
// light i) and its own reach tests reject the lights that cannot touch P; elsewhere the list as before.
// Bounded: <= 256 cheap tests per pixel, only in overflowed clusters.
bool clusterWalkAll(uint raw)         { return clusterCandidates(raw) > CLUSTER_CAP; }   // (any light dropped)
bool clusterShadowedWalkAll(uint raw) { return clusterShadowedOverflow(raw); }           // (a shadowed projector dropped)

struct Light {
    vec4 pos_radius;     // world pos, radius (LL deferred size)
    vec4 color_falloff;  // linear rgb, falloff
    vec4 proj_params;    // focus, range, lod range, ambiance
    vec4 proj_origin;    // apex .xyz, .w = 1 projector / 0 point
    vec4 proj_axis;      // forward axis .xyz, .w near clip (legacy projector reflection plane)
    mat4 proj_mat;       // world -> projector texcoord
    uvec4 tex;           // .x bindless index, .y = 1 if textured
};                       // 160 B

#ifndef CLEAN_MAX_VIEWS   // (#1608: the stereo tail's bound, = clean_view.glsl's -- not included here: a compute includer
#define CLEAN_MAX_VIEWS 4 //  defines its CLEAN_VIEW before that file)
#endif
struct LightInfo {
    vec4 cam_pos;        // .xyz camera world pos, .w light count
    vec4 cam_fwd;        // .xyz camera forward (world); .w 1: orthographic view
    vec4 zinfo;          // near, far (draw distance), GZ / log(far / near), projection far
    vec4 grid;           // tile px w, tile px h, tan(half fov x), tan(half fov y)
    mat4 view;           // world -> view (-Z forward)
    vec4 cull_planes[6]; // the main CULL frustum (ax+by+cz+d >= 0 inside; the frozen one under
                         // "Freeze culling camera"): clean_probe_gpu.comp skips probes off it (#517)
    // ---- FROXEL GRID (RenderVKLightCulling 2; clean_light_froxel.comp builds, the deferred
    //      lighting reads: froxelSlice below) ----
    mat4  fx_vp;         // the main view's JITTERED P * rotation-only V: (world - fx_eye) -> VK clip,
                         // the matrix the deferred lighting inverts (its inv_vp_rel)
    vec4  fx_eye;        // .xyz the main view's draw origin (its pc.u_cam_time.xyz), .w 0
    vec4  fx_slices;     // .x end of slice 0 (m), .y (slices - 1) / ln(far / near), .z far, .w 0
    uvec4 fx_dims;       // tiles x, tiles y, depth slices, log2(tile px)
    vec4  fx_extent;     // render extent w, h (px), 1 / w, 1 / h
    // ---- FORWARD LIT FS (the main view's forward surfaces: alpha-blend, forward opaque) ----
    uvec4 fwd;           // .xy the FROXEL MASK buffer's device address (lo, hi) while the forward
                         // walk reads it this frame (RenderVKForwardFroxels + RenderVKLightCulling
                         // 2 + the masks built for this extent), 0 = walk the cluster lists;
                         // .z options: bit 0 = alpha-blend receivers trace ray-traced projector
                         // shadows inline (RenderVKRTBlendProjShadows 1); bit 1 = the stochastic
                         // light shadows' RADIANCE MODE (RenderVKReSTIRRadiance: clean_deferred_light
                         // .comp pdnRadOn / deferPdnRadOn); bit 2 exact local-shadow sample grid; bit 5
                         // the mover report's light TAG, bit 6 the RT mask's id remap (RenderVKShadowFixLightTag
                         // bits 0 / 1: LIGHT_TAG_REPORT / LIGHT_TAG_MASK_REMAP below)
    // pass 15's LIGHT SET this frame (bit = light index, the ReSTIR header's rs_set: LLCleanRendererVK::rsLightSet) --
    // the RT shadow mask's mover fold weighs exactly the lights the stochastic radiance carries (clean_rt_shadow_mask
    // .comp mvLightEligible)
    uvec4 rs_set[2];
    // LAST FRAME'S LIGHT INDEX -> THIS FRAME'S (byte i of the 256: 0xFF = no longer listed; = the ReSTIR header's
    // rs_remap, LLCleanRendererVK::rsLightSet, matched by the light's key). The list is re-sorted by importance every
    // frame, so an index stored in a history names another light next frame: with LIGHT_TAG_MASK_REMAP the RT shadow
    // mask's history ids go through it (clean_rt_mask_common.glsl rtMaskRemapId).
    uvec4 rs_remap[16];
    // ---- (#1608) THE STEREO FROXEL TAIL (appended: nothing above moves) ----
    mat4  fx_vp_v[CLEAN_MAX_VIEWS];   // view v's fx_vp about ITS eye (jittered P * rotation-only V: (world - fx_eye_v[v]) -> clip)
    vec4  fx_eye_v[CLEAN_MAX_VIEWS];  // .xyz view v's eye (world), .w 0
    uvec4 radiance_points[2]; // ASTRA: point subset of rs_set, optional deferred walk only (fwd.z bit8)
};
// last frame's light index lp (< 256) -> this frame's (0xFFu: gone) through LightInfo L's rs_remap (a macro: the block
// is read in place, never copied)
#define LIGHT_REMAP_INDEX(L, lp) (((L).rs_remap[(lp) >> 4u][((lp) >> 2u) & 3u] >> (((lp) & 3u) * 8u)) & 0xFFu)
// (RenderVKShadowFixLightTag, A/B, 2026-11-15; default off = index-keyed) LightInfo.fwd.z bits:
//   LIGHT_TAG_REPORT     pass 15's MOVER REPORT carries the light's tag (clean_pdn_common.glsl pdnMotReportTag) -- the
//                        radiance denoiser's mover record compares it across frames -- instead of light index + 1
//   LIGHT_TAG_MASK_REMAP the RT shadow mask reads last frame's slot ids through rs_remap (rtMaskRemapIds)
#define LIGHT_TAG_REPORT     32u
#define LIGHT_TAG_MASK_REMAP 64u
// (RenderVKShadowKPickWeights 0, A/B 2026-11-15: moving-avatar shadow smear) LightInfo.fwd.z bit 7: pass 15's radK
// picks back to the plain 1 / K mean and a neighbour's blocked pick skipped (before ee74dc859c); clear = the
// confidence weights (the reservoir's pick up to half the estimate) and blocked neighbours counted
#define LIGHT_KPICK_PLAIN    128u

// ORTHOGRAPHIC main view (LightInfo cam_fwd.w 1, FSCameraOrtho; ported from GunMetal-DX,
// squeedledorf, LGPL 2.1, v0.3.0 cf5f1f4): grid.zw are the view's half extents in metres,
// not tangents, so a tile is the same width at every depth. Every "tangent x depth" /
// "x / depth" of the cluster grid goes through CL_VIEW_Z (L: the block).
#define CL_VIEW_Z(L, z) ((L).cam_fwd.w > 0.5 ? 1.0 : (z))
#define CL_VIEW_EYE(L, P, eye) ((L).cam_fwd.w > 0.5 ? (P) - (L).cam_fwd.xyz * dot((P) - (L).cam_pos.xyz, (L).cam_fwd.xyz) : (eye))

// ---- FROXEL LIGHT MASKS (RenderVKLightCulling 2) ----
// Cell (tile tx, ty of 2^fx_dims.w render px; depth slice s) = FROXEL_WORDS uints at
// ((s * fx_dims.y + ty) * fx_dims.x + tx) * FROXEL_WORDS: bit li set = light li may reach a
// surface in the cell (256 lights = MAX_VK_LIGHTS: one bit each, it can never overflow).
// View depth vz = dot(P - fx_eye, cam_fwd), the build's and the lookup's one definition.
// Slice 0: vz <= fx_slices.x (and NaN); 1 .. n-1: log-spaced over [fx_slices.x, far], the last
// one open-ended (everything beyond far). Monotone non-decreasing in vz (the build bounds a
// light's slice run by the slices of its depth interval's two ends).
#define FROXEL_WORDS 8u
// (#1608) LAYERED STEREO: a grid per view -- view v's cells after the first v views' (the build dispatched z = N); each
// view's tiles / slices from its own eye and matrix (the depth axis cam_fwd and the slices shared). Expanded at the use
// site (the includer has clean_view.glsl's kViewCount / CLEAN_VIEW); one view (kViewCount 1): the fields of before,
// base 0.
#define FX_VP(L) (kViewCount > 1 ? (L).fx_vp_v[CLEAN_VIEW] : (L).fx_vp)
#define FX_EYE(L) (kViewCount > 1 ? (L).fx_eye_v[CLEAN_VIEW] : (L).fx_eye)
#define FX_VIEW_BASE(L) (kViewCount > 1 ? uint(CLEAN_VIEW) * (L).fx_dims.x * (L).fx_dims.y * (L).fx_dims.z * FROXEL_WORDS : 0u)
uint froxelSlice(float vz, vec4 fs, uint nz)
{
    if (!(vz > fs.x)) return 0u;
    return 1u + uint(min(log(vz / fs.x) * fs.y, float(nz - 2u)));   // (clamped before the uint: inf-safe)
}
// The depth interval of slice s (slice 0 from -1e30, the last to 1e30): what froxelSlice maps
// into s, up to the rounding of log / exp (the build widens it).
vec2 froxelSliceRange(uint s, vec4 fs, uint nz)
{
    const float z0 = s == 0u ? -1e30 : fs.x * exp(float(s - 1u) / fs.y);
    const float z1 = s == 0u ? fs.x : (s + 1u >= nz ? 1e30 : fs.x * exp(float(s) / fs.y));
    return vec2(z0, z1);
}
