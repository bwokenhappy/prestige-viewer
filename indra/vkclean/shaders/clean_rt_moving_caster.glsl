// vkclean — MOVING-CASTER detection for ray-traced shadow rays, shared by the RT shadow
// mask's trace (clean_rt_shadow_mask.comp) and the ray-traced god-ray march
// (clean_godrays.comp GODRAYS_RT, #657). The includer defines RT_TRACK_BLOCKER before
// clean_rt_common.glsl (so every shadow trace there reports its blocker through
// rtNoteBlocker) and includes this after it; set 0 must be the draw set (binding 23 =
// the motion-vector pre-pass's previous instance rows). Reset rt_block_dyn before the
// rays whose casters it should summarize, read it after.
//
// MOVING-CASTER FLAG (shadow denoiser anti-lag). A shadow on a static receiver moves only
// when its caster moves; with a static caster (grass, fences, buildings) every frame-to-
// frame change of the raw estimate is sub-pixel aliasing / ray noise the temporal pass
// must keep integrating. So the trace records whether any ray of a light met a caster
// that MOVES: rigged meshes (avatars, animesh; flags bit 0), worn rigid attachments
// (bit 9), GPU spinners (llTargetOmega) and any instance whose row differs from last
// frame's (set 0 binding 23, the motion-vector pre-pass's previous instances; it aliases
// the live rows without TAA, when the mask has no temporal pass anyway). Unchanged rows
// are bit-identical copies, so the exact compare never fires on a static prim.
// The bits go to the depth plane's w (sun bit 0, projector slot k bit k + 1) and ride
// the history-out copy, so the temporal pass also sees LAST frame's flag: the trailing
// edge of a moving shadow (lit now, no caster met) still knows a mover was there.
// RT_MOVER_SET: the set the draw set 0 is bound at (default 0; the ray-traced AO, #86, binds
// the AO set at 0 and the draw set 0 at 3).
#ifndef RT_MOVER_SET
#define RT_MOVER_SET 0
#endif
layout(std430, set = RT_MOVER_SET, binding = 23) readonly buffer PrevInstances { RtInstance prev_instances[]; };
bool rt_block_dyn = false;
// (LVC stage 0 / 1, research/LVC_DESIGN_2026-11-16.md) THE MOVER KIND of instance id -- the same tests as
// rtNoteBlocker below, as bits: 1 rigged (avatars, animesh), 2 worn rigid attachment, 4 GPU spinner, 8 row changed
// since last frame; 0 = a static caster. The pass-15 VIS kernel's statistics and its static-only walk
// (clean_vis_walk.glsl RT_WALK_STATIC_HOOK) read it.
#define RT_MOVER_RIGGED 1u
#define RT_MOVER_ATTACH 2u
#define RT_MOVER_SPIN   4u
#define RT_MOVER_ROW    8u
uint rtMoverKind(uint id)
{
    if (id >= uint(rt_instances.length())) return 0u;
    const uint f = rt_instances[id].flags;
    uint k = ((f & 1u) != 0u ? RT_MOVER_RIGGED : 0u) | ((f & 512u) != 0u ? RT_MOVER_ATTACH : 0u)
           | (dot(rt_instances[id].spin_axis.xyz, rt_instances[id].spin_axis.xyz) > 0.0 ? RT_MOVER_SPIN : 0u);
    if (k != 0u || id >= uint(prev_instances.length())) return k;
    bool moved = false;
    [[dont_unroll]] for (uint c = 0u; c < 4u && !moved; ++c)
        moved = any(notEqual(rt_instances[id].model_matrix[c].xyz, prev_instances[id].model_matrix[c].xyz));
    return moved ? RT_MOVER_ROW : 0u;
}
#ifdef RT_TRACK_BLOCKER_KIND
uint rt_block_kind = 0u;   // (the includer resets it with rt_block_dyn) the first moving blocker's kind bits
#endif
void rtNoteBlocker(uint id)
{
#ifdef RT_MOVER_PROBE
    if (g_mpr) g_mpr_bid = id;   // (RenderVKMoverProbePixel: the probed texel's blocker, before any early-out)
#endif
    if (rt_block_dyn || id >= uint(rt_instances.length())) return;
#ifdef RT_TRACK_BLOCKER_KIND
    rt_block_kind = rtMoverKind(id);
    rt_block_dyn = rt_block_kind != 0u;
    return;
#endif
    if ((rt_instances[id].flags & (1u | 512u)) != 0u
        || dot(rt_instances[id].spin_axis.xyz, rt_instances[id].spin_axis.xyz) > 0.0)
    {
        rt_block_dyn = true;
        return;
    }
    if (id >= uint(prev_instances.length())) return;
    // (#1039 occupancy) the rows compared a column at a time (a loop, not unrolled): the walks note blockers
    // at their candidates, and both matrices loaded up front were 24 floats live at that peak. The same
    // verdict (any column's xyz differs).
    bool moved = false;
    [[dont_unroll]] for (uint c = 0u; c < 4u && !moved; ++c)
        moved = any(notEqual(rt_instances[id].model_matrix[c].xyz, prev_instances[id].model_matrix[c].xyz));
    rt_block_dyn = moved;
}
