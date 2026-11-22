"""Regression guard for the light-visibility work (research/LVC_DESIGN_2026-11-16.md).

Stage 0 (RenderVKReSTIROcclusionCull 3, statistics only):
  * mode 3 never culls: every exclusion path of the occlusion cache (pass 15's candidate walks, the dense window) reads
    occCullOn(), which is false under RS_F_OCC_STATS; the host never sets the hard rules for mode 3;
  * the statistics live in VIS's own variant (LVC_VIS_STATS, DeferShadowSplitVisLvc): the production VIS kernels never
    compile the static-only re-walk or the counters (they cost VIS ~11 registers on NVIDIA: 244 -> 255);
  * the LVC region sits after the emissive budget on both sides (shader REGIR_LVC_BASE = REGIR_EM_BUDGET_END, host
    kRegirLvcOffset = the old end) and is zeroed by the host on every statistics frame.
Stage 3 (RenderVKShadowReachMap, the reach maps):
  * only a USABLE map may skip a light (PLAN: valid, built after the last dirty mark, the light's pose unchanged,
    not moved), only in radiance mode with RS_F_LVC_SKIP; verify mode (2) never skips;
  * the skip sites are the window walk, the cluster walk and rsTap -- W0 is never touched;
  * the builds never count movers, spinners or the light's own fixture as occluders and see only the opaque + terrain
    classes; a run of skips past the bound reads OPEN (the radius), never blocked;
  * dirty marks cover an instance's old AND new bounds (and instances that leave the static set);
  * the host layout mirrors the shader's (kRegirLvcMapBytes), the region is zeroed when the maps come on.
Stage 3b (#439 completion, RenderVKPointShadowMaps on the non-RT tier):
  * a point light's tex.z holds PSM_CUBE | six 5-bit face records only when all six faces hold a tile, never where
    ray tracing shadows point lights (rtCovers) and only on the shadow-map projector mode;
  * the lookup bounds the record index, and rsRecordLight never reads a point light's tex.z as a record.
"""
import re

NAME = "lvc-light-visibility"
STAGE = "source"
DESCRIPTION = "light-visibility statistics never cull, stay out of the production VIS kernel, keep their layout"
GUARDS = ("the old occlusion cull removed real light (hard boxes, false evidence, lost lamps); its statistics-only "
          "mode must never exclude a light, and the instrument must not cost the production VIS kernel registers.")

DL = "indra/vkclean/shaders/clean_deferred_light.comp"
RG = "indra/vkclean/shaders/clean_regir.glsl"
VW = "indra/vkclean/shaders/clean_vis_walk.glsl"
REN = "indra/newview/vkgfx/llcleanrenderervk.cpp"
REN_H = "indra/newview/vkgfx/llcleanrenderervk.h"
PS = "indra/newview/vkgfx/llcleanprojstochvk.cpp"
REGEN = "indra/vkclean/regen_world_spv.py"


def _read(ctx, p):
    return open(ctx.path(p), encoding="utf-8").read()


def _strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def run(ctx, result):
    dl = _strip(_read(ctx, DL))
    rg = _read(ctx, RG)
    vw = _strip(_read(ctx, VW))
    ren = _strip(_read(ctx, REN))
    ren_h = _read(ctx, REN_H)
    ps = _strip(_read(ctx, PS))
    regen = _read(ctx, REGEN)

    # --- mode 3 never culls ---
    if "#define RS_F_OCC_STATS 32768u" not in rg:
        result.fail("clean_regir.glsl: RS_F_OCC_STATS (32768u) missing")
    if not re.search(r"bool occCullOn\(\)\s*\{\s*return occFrameOn\(\) && \(rg\.rs_cfg\.z & RS_F_OCC_STATS\) == 0u;", dl):
        result.fail("occCullOn() must be occFrameOn() without RS_F_OCC_STATS")
    if "occAny(ocCell) && occCullOn()" not in dl:
        result.fail("pass 15's culling patch (ocCull) must require occCullOn(): statistics mode would cull")
    if "&& !litDbg(LDBG_NO_LOCAL) && occCullOn())" not in dl:
        result.fail("the dense window's culling lookup must require occCullOn()")
    if re.search(r"occ\s*>=\s*2u\)\s*h\.rs_cfg\[2\]\s*\|=\s*1024u", ren):
        result.fail("RS_F_OCC_HARD is set for occ >= 2: mode 3 would take the hard rules (untraced = blocked evidence)")
    if "if (mRsOccStatsNow) h.rs_cfg[2] |= 32768u;" not in ren:
        result.fail("regirHeaderFill no longer sets RS_F_OCC_STATS for RenderVKReSTIROcclusionCull 3")

    # --- the statistics stay out of the production VIS kernel ---
    m = re.search(r"#elif CLEAN_SHADOW_STAGE == SH_STAGE_VIS\s*\n#define SH_KIND_POSITIONAL\s*\n#ifdef LVC_VIS_STATS(.*?)#endif",
                  _read(ctx, DL), flags=re.S)
    if not m or "#define RT_WALK_STATIC_HOOK" not in m.group(1) or "#define RT_TRACK_BLOCKER_KIND" not in m.group(1):
        result.fail("RT_WALK_STATIC_HOOK / RT_TRACK_BLOCKER_KIND must be defined only under LVC_VIS_STATS (VIS's variant)")
    for line in regen.splitlines():
        if '"kWorldDeferShadowSplitVisSpv"' in line or '"kWorldDeferShadowSplitVisIsoSpv"' in line:
            if "LVC_VIS_STATS" in line:
                result.fail("a production VIS kernel compiles LVC_VIS_STATS: " + line.strip())
    if "kWorldDeferShadowSplitVisLvcSpv" not in regen or "-DLVC_VIS_STATS" not in regen:
        result.fail("regen_world_spv.py: the statistics VIS variant (kWorldDeferShadowSplitVisLvcSpv) is missing")
    if "mRsOccStatsNow && mPdnVisLvcPipe != VK_NULL_HANDLE" not in ps:
        result.fail("the statistics VIS variant must be bound only under RenderVKReSTIROcclusionCull 3")
    if "#define RT_STATIC_SKIP(id) false" not in vw:
        result.fail("clean_vis_walk.glsl: the static-only skip must fold to false without RT_WALK_STATIC_HOOK")

    # --- layout ---
    if "#define REGIR_LVC_BASE REGIR_EM_BUDGET_END" not in rg:
        result.fail("REGIR_LVC_BASE must follow the emissive budget (REGIR_EM_BUDGET_END)")
    if "kRegirLvcOffset = kRsEmBudgetOffset + kRsEmBudgetBytes" not in ren_h:
        result.fail("host kRegirLvcOffset must be the emissive budget's end (= REGIR_LVC_BASE)")
    if "kRegirLvcMapOffset = kRegirLvcOffset + kRegirLvcBytes" not in ren_h:
        result.fail("the reach maps must follow the LVC statistics region (kRegirLvcMapOffset)")
    if not re.search(r"if \(mRsOccStatsNow\)\s*(?:\{[^}]*?)?vkCmdFillBuffer\(cmd, mRegir.buffer, kRegirLvcOffset, kRegirLvcBytes, 0u\);", ps, re.S):
        result.fail("the LVC counters are no longer zeroed on a statistics frame")
    # --- stage 3: the reach maps ---
    if "#define RS_F_LVC_SKIP 8u" not in rg or "#define RS_F_LVC      16u" not in rg:
        result.fail("clean_regir.glsl: RS_F_LVC (16u) / RS_F_LVC_SKIP (8u) missing")
    if not re.search(r"bool lvcSkipOn\(\)\s*\{\s*return lvcOn\(\) && \(rg\.rs_cfg\.z & RS_F_LVC_SKIP\) != 0u;", dl):
        result.fail("lvcSkipOn() must need RS_F_LVC_SKIP (verify mode never skips)")
    if "if ((e & LVC_LI_USABLE) == 0u) return false;" not in dl:
        result.fail("lvcBlocked must read visible unless PLAN marked the map usable")
    if "const bool lvcSkip = lvcSkipOn() && pdnRadOn();" not in dl:
        result.fail("pass 15's skip must be radiance mode only")
    if dl.count("if (lvcSkip && lvcBlocked(li, P)) { ++lvcSkips; if (!lvcSoft) continue; lvcW = lvcFloor; }") != 2:
        result.fail("the window walk and the cluster walk must both take the reach-map skip (or stage 2's soft weight)")
    if dl.count("p0) * lvcW;") != 2:
        result.fail("stage 2's soft weight must scale the RIS target in both walks (never p0 / W0)")
    if "const float lvcFloor = rg.rs_em_params2.z > 0.0 ? rg.rs_em_params2.z : 0.15;" not in dl:
        result.fail("stage 2's soft weight needs a floor > 0 (an unbiased RIS target keeps full support)")
    if "if ((rg.rs_cfg.z & RS_F_LVC_SOFT) == 0u) return 0.0;" not in dl:
        result.fail("rsTap must reject a reused pick the reach map calls statically blocked")
    if re.search(r"W0\s*=.*lvc", dl):
        result.fail("W0 (the denoiser's light-set reference) must not depend on the reach maps")
    if "if (rtMoverKind(id) != 0u || lvcInstDynamic(id) || rtFixtureBox(id, q, true, fa, fb)) { t0 = t + 0.002; continue; }" \
            not in dl:
        result.fail("the build ray must pass over movers / spinners / thrash-dynamic instances and the light's own fixture")
    if "RT_MASK_OPAQUE | RT_MASK_TERRAIN, o, t0, dir, tmax" not in dl:
        result.fail("the build ray must see the opaque + terrain classes only (alpha-tested / blend casters open)")
    if "    return tmax;\n}\nvoid lvcBuild()" not in dl:
        result.fail("the build ray must read OPEN (tmax) past its bound of skips")
    if "lvcDirtyMark(vec4(uintBitsToFloat(o1), uintBitsToFloat(o2.x), uintBitsToFloat(o0.y)), count, now);" not in dl \
            or "if (o0.x != 0u) lvcDirtyMark(old, count, now);" not in dl or dl.count("lvcDirtyMark(sph, count, now);") != 2:
        result.fail("DIRTY must mark an instance's old and new bounds (also on the change that makes it dynamic), the old "
                    "bounds of one leaving the occluder set, and the bounds of a dynamic one returning")
    if "const bool clean = valid && same && w3.x != w3.y && lvcAge(w3.x, w3.y) < 0x800000u;" not in dl:
        result.fail("PLAN: a map is usable only when built after the last dirty mark for the light's current pose")
    if "kRegirNoiseOffset = kRegirLvcMapOffset + kRegirLvcMapBytes" not in ren_h \
            or "kRegirBytes = kRegirNoiseOffset + kRegirNoiseBytes" not in ren_h:
        result.fail("kRegirBytes must include the reach maps and the noise metric's region")
    if "#define LVC_NOISE_BASE LVC_END" not in rg:
        result.fail("the noise metric's region must follow the reach maps (LVC_NOISE_BASE = LVC_END)")
    if "h = (h ^ (r.flags & ~LVC_FLAGS_IGNORED)) * 16777619u;" not in dl:
        result.fail("the instance signature must ignore the shadow-irrelevant flag bits (fullbright, tex anim, unlit, media)")
    if "vkCmdFillBuffer(cmd, mRegir.buffer, kRegirLvcMapOffset, kRegirLvcMapBytes, 0u);" not in ps:
        result.fail("the reach maps' region must be zeroed when they come on")
    # --- stage 3b: the cube-mapped point lights ---
    dc = _strip(_read(ctx, "indra/vkclean/shaders/clean_draw_common.glsl"))
    if "#define PSM_CUBE 0x80000000u" not in dc or "if (rec >= uint(pshadow.length())) return vec3(1.0);" not in dc:
        result.fail("pointCubeVisibility must bound the face record (PSM_CUBE word)")
    if "if (!pst && pointCubeMapped(li)) { const vec3 cv = pointCubeVisibility(li, P, N); pvd *= cv; pvs *= cv; }" not in dc:
        result.fail("localLight's point branch no longer applies the cube shadow map")
    if "if (rec == 0u || lights[li].proj_origin.w < 0.5) return false;" not in rg:
        result.fail("rsRecordLight must never read a point light's tex.z (PSM_CUBE) as a record")
    if "if (mode != 0u && !rtCovers && mProjMode == 1u)" not in ps:
        result.fail("cube faces may only be queued / tagged without RT point shadows and on the shadow-map projector mode")
    if "if (all) { mLightsCPU[s.light].tex[2] = word; ++mPsmMapped; }" not in ps:
        result.fail("a point light may be tagged only when all six faces hold a record")
    result.summary = "stage 0 statistics + stage 3 reach maps + stage 3b cube maps guarded"
