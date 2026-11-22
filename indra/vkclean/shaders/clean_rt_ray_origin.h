// vkclean -- WHERE A SECONDARY RAY LEAVES A SURFACE: the origin offset (self-intersection avoidance) of every
// shadow / visibility / occlusion ray that starts on a receiver or a ray-traced hit, the side it leaves from,
// and where a visibility ray toward a stored surface point must stop. Shared by the shaders
// (clean_rt_common.glsl includes it) and the CPU fixture scripts/check/test_hit_ray_origin.cpp
// (scripts/check/checks/hit_ray_origin.py), so the rule the check runs is the rule the GPU runs.
//
// ---- SECONDARY-RAY ORIGIN (self-intersection avoidance) ----
// Wachter & Binder, "A Fast and Robust Method for Avoiding Self-Intersection" (Ray
// Tracing Gems, ch. 6): move the origin off the receiver along its geometric normal n
// (on the side the ray leaves from) by a few float ULPs of the coordinates themselves,
// so the offset follows the real float error instead of a fixed distance.
//
// OUR SPACE: receiver positions and the TLAS are ABSOLUTE agent (region-local) world
// space -- the RT shadow mask rebuilds P = camera + camera-relative offset
// (clean_rt_mask_common.glsl worldAt), the lit FS interpolates absolute v_world_pos, the
// rigged BLAS holds pre-skinned positions + the eye (clean_rt_instances.comp rigCopy) and
// static BLASes are placed by absolute instance transforms. |x|, |y| are up to ~256 m
// in the agent's region (a few hundred beyond it), z a few tens of metres on the
// ground and up to 4096 m for skyboxes. One ULP: 1.5e-5 m at 128-256 m, 3e-5 m at
// 256-512 m, 4.9e-4 m at 4096 m.
// The error sources between the rasterized receiver and the traced triangle:
//   * rounding of each coordinate (P = cam + offset, the rigged BLAS = eye-relative
//     skin + eye, the hit interpolation): ~1-2 ULPs of THAT coordinate -> the W&B
//     integer step, RT_OFS_INT_SCALE ULPs per component;
//   * static geometry rasterizes through the ABSOLUTE view-projection (clean_draw.vert:
//     world terms of ~|coordinate| cancel against the camera translation down to a
//     view depth of ~camd), so its depth carries ~2-4 ULPs of the LARGEST coordinate
//     in every direction, whatever n's dominant axis (ground at (200, 200, 25): the z
//     step alone would be 8x too small) -> a frame floor of RT_OFS_FRAME_ULPS ULPs of
//     max |p_i| along n. It also is W&B's near-zero fallback: components below
//     RT_OFS_ORIGIN take no integer step (their bits would cross zero);
//   * RT_OFS_FLOAT_SCALE: an absolute floor (m) for a frame whose every coordinate is
//     tiny.
// W&B's own constants (int_scale 256, origin 1/32, float_scale 1/65536) are sized for
// unit-scale scenes with deep transform chains: 256 ULPs is ~8 mm at 256 m and jumps
// the stacked layers of SL mesh avatars. These give ~0.1-0.25 mm in a region and
// ~4-6 mm at skybox altitude.
#ifndef CLEAN_RT_RAY_ORIGIN_H
#define CLEAN_RT_RAY_ORIGIN_H
#ifdef __cplusplus
#define RRO_FN inline
#define RRO_OUT(T) T&
#else
#define RRO_FN
#define RRO_OUT(T) out T
#endif
#define RT_OFS_ORIGIN      float(1.0 / 32.0)
#define RT_OFS_INT_SCALE   float(12.0)
#define RT_OFS_FRAME_ULPS  float(6.0)
#define RT_OFS_FLOAT_SCALE float(1.0 / 131072.0)
// DEPTH RECONSTRUCTION (receivers rebuilt from the reversed-Z D32F depth buffer): the
// stored depth ~ near / view depth keeps ~2^-24 relative precision; rasterizer plane
// interpolation and the un-projection add a few ULPs. 2^-21 (8 relative ULPs) of the
// camera distance: 0.5 um at 1 m, 0.05 mm at 100 m. (The absolute-space raster error
// of static geometry is the frame floor above, not this.) The same bound covers a point
// found PARAMETRICALLY along a ray, P = o + d t (8 relative ULPs of t).
#define RT_DEPTH_REL_ERR   float(1.0 / 2097152.0)

// Spacing of floats at |x| (normal range; 0 below it).
RRO_FN float rtUlp(float x)
{
    const uint e = (floatBitsToUint(x) >> 23) & 0xFFu;
    return e > 23u ? uintBitsToFloat((e - 23u) << 23) : float(0.0);
}
RRO_FN float rtOffsetComp(float p, float n, float fl)
{
    const float pf = p + n * fl;                    // frame floor
    if (abs(p) < RT_OFS_ORIGIN) return pf;          // near zero: no integer step
    const int oi = int(RT_OFS_INT_SCALE * n);
    const float pi = intBitsToFloat(floatBitsToInt(p) + (p < float(0.0) ? -oi : oi));
    return abs(pi - p) > abs(pf - p) ? pi : pf;     // both move along sign(n): the farther
}
// p: the receiver / hit; n: unit GEOMETRIC normal on the side the ray leaves from.
RRO_FN vec3 offsetRayOrigin(vec3 p, vec3 n)
{
    const float m  = max(max(abs(p.x), abs(p.y)), abs(p.z));
    const float fl = max(RT_OFS_FRAME_ULPS * rtUlp(m), RT_OFS_FLOAT_SCALE);
    return vec3(rtOffsetComp(p.x, n.x, fl), rtOffsetComp(p.y, n.y, fl), rtOffsetComp(p.z, n.z, fl));
}

// ---- THE ONE SURFACE-RAY ORIGIN: a receiver's or a hit's ----
// p: the point; n: the unit GEOMETRIC normal (the triangle's plane, never a shading / normal-mapped normal) on the
// side the ray leaves from; err: how far p itself may lie off that plane beyond the float error offsetRayOrigin
// already covers (m): the main-view receivers' depth reconstruction (rtReceiverOrigin: camd x RT_DEPTH_REL_ERR + the
// user's bias), a parametric point's t x RT_DEPTH_REL_ERR -- and 0 for a RAY-TRACED HIT: rtSurfaceEval builds its P
// from the barycentrics of the very triangle the traversal hit (clean_rt_surface.glsl), so it lies on that plane to
// the float error of its coordinates, whatever the hit distance or the ray cone. The cone's footprint is the area a
// hit's SHADING stands for (texture LOD), not an error of its position: an offset by it -- or the old flat 1 cm --
// is exactly what jumped thin geometry (an ear, a roof's trim over its shingles, two faces millimetres apart) and
// lit hits a lamp cannot reach. Rigged hits included: the hit and its continuation ray see the same (possibly stale,
// clean_rt pose skip) BLAS, so staleness is no error between them.
RRO_FN vec3 rtHitOrigin(vec3 p, vec3 n, float err)
{
    return offsetRayOrigin(p + n * err, n);
}
// THE SIDE a hit's ray toward a light leaves from (rtHitSunRay's and the main view's rule, clean_shadow_core.glsl
// shPosOrigin): ng = the hit's geometric normal on the side its ray ARRIVED from (rtSurfaceEval s.Ng; a forward
// fragment: the side the camera sees), l = toward the light. Lit side: n = ng, trace. Turned away (ng . l <= 0):
// under the back-face self-shadow (bfs, RenderVKRTBackfaceSelfShadow) its own surface shadows it -- false, no ray,
// visibility 0; without it the old flip (n = -ng). The shading normal plays no part: a normal map / a smooth vertex
// normal leaning toward a light behind the facet put the old origin (1 cm along N) through the surface.
RRO_FN bool rtHitLightSide(vec3 ng, vec3 l, bool bfs, RRO_OUT(vec3) n)
{
    n = ng;
    if (dot(ng, l) > float(0.0)) return true;
    if (bfs) return false;
    if (dot(ng, l) < float(0.0)) n = ng * float(-1.0);
    return true;
}

// ---- WHERE A VISIBILITY RAY TOWARD A STORED SURFACE POINT STOPS ----
// (GI reuse, clean_deferred_light.comp: a reused sample x_s seen from this receiver.) The ray must stop short of
// x_s's own surface -- else it meets it and rejects every sample -- but by no more than that surface's possible
// displacement since the sample was taken, measured PERPENDICULAR to it (a layer lying a gap g in front of x_s is
// crossed g / cos before x_s: stopping c / cos short tests every layer with g > c). c: RT_REUSE_END_CLEAR, the
// largest drift a sample's surface may have without a change sphere (the rigged pose skip's metric threshold,
// RenderVKRigRefitPoseThreshold, 2 mm; static geometry does not move, moved prims raise hard change spheres), or
// the float error at x_s if larger (skybox altitude). The old flat max(1 cm, 0.1 % of dl) along the ray let a
// neighbour's sample behind any layer within ~1 cm of it light this receiver through that layer.
// dl: the distance from the ray origin to x_s; cosEnd: |n_s . d| at x_s. Returns the ray's tmax.
#define RT_REUSE_END_CLEAR float(0.002)
RRO_FN float rtReuseSegEnd(vec3 xs, float dl, float cosEnd)
{
    const float m = max(max(abs(xs.x), abs(xs.y)), abs(xs.z));
    const float c = max(RT_REUSE_END_CLEAR, RT_OFS_FRAME_ULPS * rtUlp(m) + dl * RT_DEPTH_REL_ERR);
    return max(dl - c / max(cosEnd, float(0.05)), float(0.0));
}
#endif
