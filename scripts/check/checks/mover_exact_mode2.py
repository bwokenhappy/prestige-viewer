"""Regression guard for EXACT LOCAL LIGHTS NEAR MOVERS (RenderVKShadowMoverMask 2, 2026-11-13;
research/DESIGN_avatar_exact_lights.md).

Mode 1 splits a local light's shadow into pass 15's STATIC visibility (long history) and the RT shadow mask's MOVER
visibility (short history), folded per pixel by a ratio: with several lights the two timelines recede against each other
(the band, the lit sliver between overlapping avatar shadows). Mode 2 drops the split for the local lights: wherever a
mover can lie between a pixel and a light that reaches it (the mask's mover cull, shared -- geometry only) or a ray met a
mover within the countdown, pass 15 evaluates EVERY light reaching the pixel with one all-caster ray and sums t_i V_i with
its own terms; every other local-light ray sees all casters too; the sun keeps mode 1's routing.

This check fails when any of that is undone:
  (a) ALL CASTERS: pass 15's walk mask resolves to RT_MASK_ALL for a local light under PDN_OPT_MOVER_ALL (and to the
      static set for the sun / emitters), the exact jobs and the light samples mark themselves local, the host sets the
      bit only with mode 2 confirmed;
  (b) NO FOLD in mode 2: the frame info's fold bit (3) is cleared and the mask's local mover passes are gated off by bit 5;
  (c) ONE RAY PER GATED LIGHT: SELECT reserves one job per light of ONE walk (bounded by the light table, no budget),
      writes them from the same walk, VIS traces each with pdnProjSample's single disc ray (no RS_MOVE_RAYS / no extra
      rays), SHADE sums dt * V / st * V over every job with no weight, clamp or ratio;
  (d) GEOMETRY-ONLY SELECTION: pdnMoverExactPlan reads no visibility of this frame -- the cull (mvCullBuild /
      mvSegHitAny) and the mover countdown only;
  (e) ONE CULL BODY: the shared bodies exist once, in clean_rt_movers.glsl (MV_CULL_BODIES), included by the mask and
      pass 15's SELECT;
  (f) THE JOB LIST: sized from the denoiser (render) extent, w x h jobs x 8 B, in the RT lighting scratch with the
      retry-in-30-s refusal policy, released when mode 2 is not wanted; host and shader agree on its offset;
  (g) HISTORY: the temporal cap keys on the mover countdown (a ray met a mover now / within it), never on exact-set
      membership; the countdown is max(persist, dynamic max); the a-trous gives an exact centre exact taps only;
  (h) MODES 0 / 1 UNTOUCHED: every new bit is gated on mode 2 confirmed; the setting is U32, nonpersistent, default 1,
      every C++ reader reads it as U32;
  (i) A REPLAY of the selection (the shared cull's tile and texel predicates, the reach over the texel's lights, the
      disc growth, THE AVATAR BOUND -- the real-joint filter and the joint clusters, ported line for line): every
      (texel, light) whose segment to any point of the light's disc meets a mover's caster is selected -- a superset,
      never a dropped mover shadow; and THE SMALL ROOM (2026-11-14, the owner's scene): an avatar in the middle of a
      room, ceiling lights, its joint table holding absent rows (identity, at the world origin) and the HUD points --
      no ceiling texel is selected, the floor under the avatar is (the old one-sphere-one-box bound over every row
      selected the ceiling: the replay of it must, or the case tests nothing);
  (j) THE OVERFLOW RULE: admission is a priority threshold from LAST frame's demand histogram (deterministic, never the
      atomic's order), this frame's demand goes to the other parity, SHADE clears the parity SELECT read; the slots of a
      run that crossed the cap are marked and VIS skips them; every texel carries its countdown (gate bits 28-31).
"""
import math
import os
import random
import re

NAME = "mover-exact-mode2"
STAGE = "source"
DESCRIPTION = "RenderVKShadowMoverMask 2: exact all-caster local lights near movers, one ray per light, geometry-only " \
              "selection, one shared cull, bounded job list; modes 0 / 1 untouched"
GUARDS = ("Avatar shadows from several local lights receding / lit slivers between overlapping shadows (the mode-1 fold), "
          "a mode-2 pixel whose mover shadow goes missing (cull miss, a light without a ray, a static-only ray), and "
          "mode 1 / 0 changing behind the A/B.")

SH = "indra/vkclean/shaders/"
DL = SH + "clean_deferred_light.comp"


def _strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _function(src, name):
    m = re.search(r"(?:bool|float|uint|void|vec3|vec4|PdnExWalk|VkDeviceSize|uint32_t)\s+(?:LLCleanRendererVK::)?"
                  + name + r"\([^)]*\)\s*(?:const\s*)?\{", src)
    if not m:
        return None
    depth, i = 0, m.end() - 1
    while i < len(src):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[m.start():i + 1]
        i += 1
    return None


def _structure(read):
    out = []
    dl = _strip(read(DL))
    mv = read(SH + "clean_rt_movers.glsl")
    mvs = _strip(mv)
    mask = _strip(read(SH + "clean_rt_shadow_mask.comp"))
    fi = _strip(read(SH + "clean_frame_info.glsl"))
    tmp = _strip(read(SH + "clean_dn_temporal.comp"))
    atr = _strip(read(SH + "clean_dn_atrous.comp"))
    pc = _strip(read(SH + "clean_pdn_common.glsl"))
    ps = _strip(read("indra/newview/vkgfx/llcleanprojstochvk.cpp"))
    rv = _strip(read("indra/newview/vkgfx/llcleanrenderervk.cpp"))
    rtc = _strip(read("indra/newview/vkgfx/llcleanrtvk.cpp"))
    dnc = _strip(read("indra/newview/vkgfx/llcleandenoisevk.cpp"))

    # ---- (a) all casters for the local lights ----
    m = re.search(r"#define RT_WALK_MASK_AND ([^\n]+)\n#define RT_WALK_MOVER_FILTER ([^\n]+)", dl)
    if not m or "!pdnWalkAllCasters()" not in m.group(1) or "!pdnWalkAllCasters()" not in m.group(2) \
            or ": RT_MASK_ALL)" not in m.group(1):
        out.append("(a) pass 15's RT_WALK_MASK_AND / RT_WALK_MOVER_FILTER no longer resolve to ALL casters for a local "
                   "light under PDN_OPT_MOVER_ALL")
    wa = _function(dl, "pdnWalkAllCasters") or ""
    if "g_pdn_mv_local" not in wa or "PDN_OPT_MOVER_ALL" not in wa:
        out.append("(a) pdnWalkAllCasters: not keyed on the sample being a local light AND PDN_OPT_MOVER_ALL")
    if not re.search(r"#define PDN_OPT_MOVER_ALL\s+256u", dl) or not re.search(r"#define PDN_OPT_MOVER_EXACT\s+512u", dl):
        out.append("(a) PDN_OPT_MOVER_ALL / PDN_OPT_MOVER_EXACT are not 256u / 512u (the host's rtr2.z bits)")
    xj = _function(dl, "pdnVisExactJob") or ""
    if "g_pdn_mv_local = true;" not in xj or xj.find("g_pdn_mv_local = true;") > xj.find("shadowSampleVis("):
        out.append("(a) pdnVisExactJob: the exact ray is not marked local (all casters) before its walk")
    if not re.search(r"~SH_SPLIT; \}\s*\n\s*g_pdn_mv_local = true;", dl):
        out.append("(a) VIS: a light sample (not an emitter) no longer marks itself local")
    if "g_pdn_mv_local = sLi != 0xFFFFFFFFu;" not in dl:
        out.append("(a) the fused kernel no longer marks local-light samples (the sun / emitters stay mover-free)")
    if "(mMoverRouteNow && mMoverExactNow ? 256u : 0u)" not in ps \
            or "(mMoverRouteNow && mLightPrep.exact ? 512u : 0u)" not in ps:
        out.append("(a) llcleanprojstochvk.cpp: PDN_OPT_MOVER_ALL / _EXACT are not set from mode 2 confirmed")

    # ---- (b) no fold, no local mover passes ----
    if "(mPdnRadiance && !mMoverExactNow ? 8u : 0u)" not in rv or "(mMoverExactNow ? 32u : 0u)" not in rv:
        out.append("(b) moverBits: mode 2 must clear the fold bit (3) and set LOCAL LIGHTS EXACT (5)")
    if "return (moverBits() & 9u) == 9u;" not in fi or "return (moverBits() & 33u) == 33u;" not in fi:
        out.append("(b) clean_frame_info.glsl: moverFoldAll / moverLocalExact changed")
    if "const bool mvLocalExact = moverLocalExact();" not in mask or "if (lcount > 0u && !mvLocalExact)" not in mask \
            or "else if (mvRoute && !mvLocalExact && count > 0u" not in mask:
        out.append("(b) clean_rt_shadow_mask.comp: the local mover passes / their reach are not gated off in mode 2")
    if "if (moverFoldAll() && !litDbgMover(LDBG_NO_MOVER))" not in dl:
        out.append("(b) deferEmitterLight: the fold is no longer gated on the frame info's fold bit")

    # ---- (c) one ray per gated light ----
    walk = _function(dl, "pdnExactNext") or ""
    beg = _function(dl, "pdnExactWalkBegin") or ""
    plan = _function(dl, "pdnMoverExactPlan") or ""
    if "pdnTargetL(" not in walk or "emTaken(li)" not in walk or "min(min(count, uint(lights.length())), 256u)" not in beg \
            or "clusterWalkAll(craw)" not in beg:
        out.append("(c) pdnExactNext / pdnExactWalkBegin: the exact walk is not every light of pass 15's set reaching P, "
                   "bounded by the light table (clusterWalkAll where the cluster overflowed)")
    if re.search(r"MV_PIX_LIGHTS|BUDGET|MAX_LIGHTS|MV_CAND", walk + beg):
        out.append("(c) the exact walk has a light cap / budget")
    if plan.count("pdnExactNext(") < 3 or "const uint nJ = admit ? n : 0u;" not in plan \
            or "pdn_vis[pdnExactJob(D, jb + k)] = (tex << 8u) | li;" not in plan \
            or "for (uint k = 0u; k < n; ++k)" not in plan:
        out.append("(c) pdnMoverExactPlan: the texel no longer reserves and writes exactly one job per light of the walk")
    if "pdnProjSample(li," not in xj or "nRays" in xj or "rsMoveRays" in xj:
        out.append("(c) pdnVisExactJob: not ONE disc ray of pass 15's own sample (pdnProjSample, S.nRays untouched)")
    if "S.nRays = 1u;" not in (_function(dl, "pdnProjSample") or ""):
        out.append("(c) pdnProjSample no longer samples one disc ray")
    sh_i = dl.find("if (sel.allL && pdnExactOn(D))")
    sh_b = dl[sh_i:dl.find("CLK_MARK(PROJ);", sh_i)] if sh_i >= 0 else ""
    if not sh_b or "for (uint k = 0u; k < sel.u1; ++k)" not in sh_b \
            or "numD += dt * vis; denD += dt;" not in sh_b or "numS += st * vis; denS += st;" not in sh_b \
            or re.search(r"wgt|pdnRadClamp|mvPixelRatio|RestRatio|/ ", sh_b):
        out.append("(c) SHADE: the exact sum is not sum t_i V_i over every job (no weight, clamp, ratio or division)")
    if "if (pdnExactOn(D)) exAll = pdnMoverExactPlan(" not in dl or "PDN_SEL_ALL_LIGHTS" not in dl:
        out.append("(c) SELECT: the mover-exact selection is not wired into the descriptor")

    # ---- (d) geometry-only selection ----
    if not plan or "mvCullBuild(" not in plan or "mvSegHitFirst(" not in plan:
        out.append("(d) pdnMoverExactPlan: the selection does not run the shared mover cull")
    for bad in ("pdnVisUnpack", "pdnExactVisW", "shadowSampleVis", "rt_block_", "uPdnIn", "rs_res"):
        if bad in plan:
            out.append("(d) pdnMoverExactPlan reads this frame's visibility / estimate (%s): the selection must be "
                       "geometry + the mover countdown only" % bad)
    if "const bool want = n > 0u && (seg || prevCd > 0u);" not in plan:
        out.append("(d) pdnMoverExactPlan: membership is no longer (a light's segment meets a mover) OR (the countdown)")

    # ---- (e) one cull body ----
    shaders = {}
    for fn in os.listdir(read.root + "/" + SH):
        if fn.endswith((".glsl", ".comp", ".frag", ".vert", ".h")):
            shaders[fn] = _strip(read(SH + fn))
    for name in ("mvSphere", "mvAvCount", "mvObjCount", "mvAvBox", "mvBoxSeg", "mvCullBuild", "mvSegHitAny",
                 "mvSegHitFirst", "mvAvClusters", "mvAvCluster", "mvAvHolds", "mvMoverCode", "mvLightRadius",
                 "mvRankBelow"):
        defs = [f for f, s in shaders.items()
                if re.search(r"\b(?:bool|float|uint|void|vec4)\s+" + name + r"\s*\([^;{]*\)\s*\{", s)]
        if defs != ["clean_rt_movers.glsl"]:
            out.append("(e) %s is defined in %s -- the shared cull body must exist once, in clean_rt_movers.glsl"
                       % (name, defs or "no file"))
    if "#ifdef MV_CULL_BODIES" not in mvs:
        out.append("(e) clean_rt_movers.glsl: the shared bodies are not behind MV_CULL_BODIES")
    for f in ("clean_rt_shadow_mask.comp", "clean_deferred_light.comp"):
        s = shaders.get(f, "")
        if not re.search(r'#define MV_CULL_BODIES\s*\n#include "clean_rt_movers.glsl"', s):
            out.append("(e) %s does not include the shared cull bodies" % f)

    # ---- (f) the job list ----
    sf = _function(ps, "pdnSelFrame") or ""
    if "const uint32_t w = mPdn->width(), h = mPdn->height();" not in sf \
            or "const VkDeviceSize texels = (VkDeviceSize)w * h * mViewCount;" not in sf \
            or "const VkDeviceSize exactBytes = 16u + texels * 8u;" not in sf \
            or "visBytes + (exact ? exactBytes : 0u)" not in sf or "mPdnExactRetryAt = now + 30.0;" not in sf \
            or "mPdnVisExact == exactWant" not in sf:
        out.append("(f) pdnSelFrame: the exact list is not w x h (the denoiser = render extent) x 8 B in the RT lighting "
                   "scratch with the 30 s refusal retry, re-laid out (released) when mode 2 is not wanted")
    # (#1608: N = every view's texels on both sides -- the host's mViewCount, the shader's pdnTexels / kViewCount)
    if "return 16u + (VkDeviceSize)mPdnSelW * mPdnSelH * mViewCount * mPdnVisSlots * 8u;" not in ps \
            or "uint pdnExactBase(ivec2 D) { return pdnVisBase(D) + pdnTexels(D) * pdnVisSlots(); }" not in dl \
            or "uint pdnVisBase(ivec2 D) { return 4u + pdnTexels(D) * pdnVisSlots(); }" not in dl \
            or "uint pdnExactCap(ivec2 D)  { return pdnTexels(D); }" not in dl \
            or "uint pdnTexels(ivec2 D) { return uint(D.x * D.y) * uint(kViewCount); }" not in dl \
            or "&& cap < (1u << 24u)" not in dl:
        out.append("(f) the exact list's offset differs between the host (pdnExactWordOff) and the shader (pdnExactBase)")
    if "vkCmdUpdateBuffer(cmd, mPdnVis.buffer, mPdnVisOff + pdnExactWordOff(), sizeof(kHdr), kHdr);" not in ps \
            or "vkCmdDispatchIndirect(cmd, mPdnVis.buffer, mPdnVisOff + pdnExactWordOff() + 4u);" not in ps:
        out.append("(f) the exact list's header reset / indirect dispatch does not add mPdnVisOff (its scratch range)")
    if "uint(pdn_vis.length()) >= pdnExactBase(D) + 4u + 2u * cap" not in dl:
        out.append("(f) pdnExactOn: the shader no longer checks the bound range holds the exact list")
    if "const uint g = (hi + 63u) / 64u - (lo + 63u) / 64u;" not in plan \
            or "const bool fits = admit && jb <= cap && n <= cap - jb;" not in plan:
        out.append("(f) pdnMoverExactPlan: an overflowing texel must stay stochastic and the dispatch must never pass the "
                   "list's capacity")

    # ---- (j) the overflow rule: deterministic admission ----
    if "const bool admit = want && bin <= thr;" not in plan \
            or "const uint hb = MV_X_HIST + (par ^ 1u) * MV_X_BINS, budget = cap - cap / 8u;" not in plan \
            or "if (acc > budget) { thr = b == 0u ? 0u : b - 1u; break; }" not in plan \
            or "atomicAdd(mv[MV_X_HIST + par * MV_X_BINS + b], sb);" not in plan \
            or "const uint sb = subgroupAdd(want && bin == b ? n : 0u);" not in plan \
            or "bin = min(uint(length(P - mvSphere(segM).xyz) / MV_X_BIN_M), MV_X_BINS - 1u);" not in plan:
        out.append("(j) pdnMoverExactPlan: admission must be the priority threshold from last frame's demand histogram "
                   "(this frame's demand -- every wanting texel's, admitted or not -- to the other parity)")
    # the threshold and the bin: last frame's histogram and this texel's geometry only -- no list position, frame index,
    # random number or lane identity
    i_bin, i_thr, i_dem = plan.find("uint bin = 0u;"), plan.find("uint thr ="), plan.find("const uint sb = ")
    dec = plan[i_bin:i_dem] + plan[plan.find("const bool admit"):plan.find("const bool admit") + 50] \
        if min(i_bin, i_thr, i_dem) >= 0 else ""
    bad = [t for t in ("jb", "pdn_vis[XB]", "frame", "rand", "pdnPcg", "subgroup", "gl_", "atomic") if t in dec]
    if not dec or bad:
        out.append("(j) the admission / its threshold reads %s: it must not depend on the dispatch order"
                   % (", ".join(bad) or "(not found)"))
    hd = _function(dl, "pdnExactHistDone") or ""
    if "mv[MV_X_HIST + (par ^ 1u) * MV_X_BINS + b] = 0u;" not in hd or "if (!pdnMoverCdValid())" not in hd \
            or "pdnExactHistDone(m, D, (uint(g_draw_push.rtr.w + 0.5) >> 4u) & 1u);" not in dl:
        out.append("(j) SHADE no longer clears the histogram parity SELECT read (pdnExactHistDone)")
    if "pdn_vis[pdnExactJob(D, jb + k)] = PDN_EXACT_GAP;" not in plan or "if (job == PDN_EXACT_GAP) return;" not in xj:
        out.append("(j) the slots of a run that crossed the cap are no longer marked / skipped (VIS traced last frame's "
                   "result words there as jobs)")
    if "gateM | (min(exPrevCd, 15u) << PDN_GATE_CD_SHIFT)" not in dl \
            or "const uint cdPrev = gateM >> PDN_GATE_CD_SHIFT;" not in dl:
        out.append("(j) the mover countdown is carried only by exact texels again (a texel the list did not take dropped "
                   "it to 0)")
    if '"c_defer_shadowvisx"' not in ps or '"c_defer_pstochvisx"' not in ps:
        out.append("(f) the exact VIS dispatch has no GPU timer of its own (c_defer_shadowvisx / c_defer_pstochvisx)")

    # ---- (g) history ----
    if "if (dnMoverExact() && pdnMotMoverCd(mo.w) > 0u && dp.sun0.z > 0.0)" not in tmp \
            or "PDN_MOT_EXACT_SET" in tmp:
        out.append("(g) clean_dn_temporal.comp: the cap must key on the mover countdown only (never on membership)")
    if "const uint cd = casterMoved ? pdnMoverCountdownLen() : (cdPrev > 0u ? cdPrev - 1u : 0u);" not in dl:
        out.append("(g) SHADE: the mover countdown is no longer set on a mover hit and decremented otherwise")
    cl = _function(ps, "moverCountdownLen") or ""
    if "std::max<uint32_t>(std::max<uint32_t>(std::min<U32>(persist, 7u), moverDynMaxHistory()), 1u)" not in cl \
            or "RenderVKRTShadowMoverPersist" not in cl:
        out.append("(g) the countdown is no longer max(RenderVKRTShadowMoverPersist, RenderVKRTShadowDynamicMaxHistory)")
    if "if (exC && (imageLoad(uDnMot, DN_MOT(q)).w & PDN_MOT_EXACT_SET) == 0u) continue;" not in atr:
        out.append("(g) clean_dn_atrous.comp: an exact centre takes stochastic taps")
    if "#define PDN_MOT_MOVER_CD_SHIFT 14u" not in pc or "#define PDN_MOT_EXACT_SET 0x40000u" not in pc:
        out.append("(g) clean_pdn_common.glsl: the MOT w countdown / exact bits moved (14-17 / 18; 19+ are taken)")

    # ---- (h) modes 0 / 1 untouched ----
    if "dp.moverExact = mMoverRouteNow && mLightPrep.exact;" not in ps \
            or "(p.radiance && p.moverExact ? 4096u : 0u)" not in dnc:
        out.append("(h) the denoiser's exact-mover rule is not gated on mode 2 confirmed")
    if "if (mLightPrep.exact)" not in ps or "if ((opts & PDN_OPT_MOVER_EXACT) != 0u)" not in dl:
        out.append("(h) the countdown / exact dispatch are not gated on mode 2")
    if "mMoverExactNow = exactWant && mPdnRadiance && pdnSplitPlanned() && mPdnVisExact;" not in rv:
        out.append("(h) moverRouteDecide: mode 2 runs without radiance mode / the split core / its job list")
    for f, s in (("llcleanrenderervk.cpp", rv), ("llcleanrtvk.cpp", rtc), ("llcleanprojstochvk.cpp", ps)):
        for mm in re.finditer(r"LLCachedControl<(\w+)>\s+\w+\(gSavedSettings,\s*\"RenderVKShadowMoverMask\"", s):
            if mm.group(1) != "U32":
                out.append("(h) %s reads RenderVKShadowMoverMask as %s (it is U32 0 / 1 / 2)" % (f, mm.group(1)))
    st = read("indra/newview/app_settings/settings.xml")
    sm = re.search(r"<key>RenderVKShadowMoverMask</key>\s*<map>(.*?)</map>", st, re.S)
    if not sm or "<key>Type</key><string>U32</string>" not in sm.group(1) \
            or not re.search(r"<key>Persist</key>\s*<integer>0</integer>", sm.group(1)) \
            or not re.search(r"<key>Value</key>\s*<integer>0</integer>", sm.group(1)):
        out.append("(h) settings.xml: RenderVKShadowMoverMask must be U32, nonpersistent, default 0 (2026-11-14: mode 0 ships)")
    if re.search(r"<key>RenderVK\w*MoverExact\w*</key>", st):
        out.append("(h) settings.xml: mode 2 must not add a setting (it is RenderVKShadowMoverMask 2)")
    for root, _dirs, files in os.walk(read.root + "/indra/newview/skins"):
        for fn in files:
            if fn.endswith(".xml"):
                with open(os.path.join(root, fn), encoding="utf-8", errors="replace") as f:
                    if "RenderVKShadowMoverMask" in f.read():
                        out.append("(h) %s binds RenderVKShadowMoverMask (a checkbox would write a Boolean)" % fn)
    return out, mvs


# ---- (i) THE REPLAY: the selection is a superset of the mover-blocked (texel, light) pairs ----
def _v(a, b, op):
    return tuple(op(x, y) for x, y in zip(a, b))


def _sub(a, b):
    return _v(a, b, lambda x, y: x - y)


def _add(a, b):
    return _v(a, b, lambda x, y: x + y)


def _mul(a, s):
    return tuple(x * s for x in a)


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _len(a):
    return math.sqrt(_dot(a, a))


def seg_hit(a, b, c, r):            # clean_rt_movers.glsl mvSegHit
    ab = _sub(b, a)
    l2 = _dot(ab, ab)
    t = min(max(_dot(_sub(c, a), ab) / l2, 0.0), 1.0) if l2 > 1e-12 else 0.0
    d = _sub(_add(a, _mul(ab, t)), c)
    return _dot(d, d) <= r * r


def seg_hit_box(a, b, lo, hi, r):   # clean_rt_movers.glsl mvSegHitBox
    lo = tuple(x - r for x in lo)
    hi = tuple(x + r for x in hi)
    d = _sub(b, a)
    t0, t1 = 0.0, 1.0
    for k in range(3):
        if abs(d[k]) < 1e-9:
            if a[k] < lo[k] or a[k] > hi[k]:
                return False
            continue
        inv = 1.0 / d[k]
        ta, tb = (lo[k] - a[k]) * inv, (hi[k] - a[k]) * inv
        if ta > tb:
            ta, tb = tb, ta
        t0, t1 = max(t0, ta), min(t1, tb)
        if t0 > t1:
            return False
    return True


MV_CAND = 8


def _consts(mvs):
    """THE AVATAR BOUND's constants, read from clean_rt_movers.glsl (the replay runs the production values)."""
    out = {}
    for k in ("MV_JOINT_PAD", "MV_CL_LINK", "MV_CL_EXT", "MV_HUD_R"):
        m = re.search(r"#define " + k + r"\s+([\d.]+)", mvs)
        out[k] = float(m.group(1)) if m else None
    m = re.search(r"#define MV_CL_N\s+(\d+)u", mvs)
    out["MV_CL_N"] = int(m.group(1)) if m else None
    return out


def joint_real(row, C, K):         # clean_rt_instances.comp mvJointReal (a row: (position, kind))
    p, kind = row
    if kind in ("identity", "zero"):
        return False
    if _len(p) < K["MV_HUD_R"] and _len(C) > 4.0 * K["MV_HUD_R"]:
        return False
    return _len(_sub(p, C)) < 1.0e3


def _box_dist(lo, hi, p):
    return _len(tuple(max(lo[k] - p[k], p[k] - hi[k], 0.0) for k in range(3)))


def _extent(lo, hi, p):
    return max(max(hi[k], p[k]) - min(lo[k], p[k]) for k in range(3))


def avatar_bound(rows, atts, K, old=False):
    """clean_rt_instances.comp moverAvatar (the first contributor + every attachment contributor), ported: the sphere
    (centre: the pelvis, rows[0]), the padded joint-cluster boxes and the attachment box. old=True: the bound before
    2026-11-14 (every finite row within 1 km, one box, pad 0.75, radius >= 1.5) -- the replay of the owner's bug."""
    C = rows[0][0]
    if old:
        pad = 0.75
        r = 1.5
        lo, hi = _sub(C, (pad,) * 3), _add(C, (pad,) * 3)
        for (p, _kind) in rows[1:]:
            if _len(_sub(p, C)) < 1.0e3:
                r = max(r, _len(_sub(p, C)) + pad)
                lo = tuple(min(lo[k], p[k] - pad) for k in range(3))
                hi = tuple(max(hi[k], p[k] + pad) for k in range(3))
        for (c, rs) in atts:
            r = max(r, _len(_sub(c, C)) + rs)
            lo = tuple(min(lo[k], c[k] - rs) for k in range(3))
            hi = tuple(max(hi[k], c[k] + rs) for k in range(3))
        return C, r, [(lo, hi)], None
    pad, n_max = K["MV_JOINT_PAD"], K["MV_CL_N"]
    clo, chi = [C], [C]
    r = pad
    for row in rows[1:]:
        if not joint_real(row, C, K):
            continue
        p = row[0]
        r = max(r, _len(_sub(p, C)) + pad)
        best, near1, best_d, near_d = None, 0, 1e30, 1e30
        for k in range(len(clo)):
            d = _box_dist(clo[k], chi[k], p)
            if d < near_d:
                near_d, near1 = d, k
            if d <= K["MV_CL_LINK"] and d < best_d and _extent(clo[k], chi[k], p) <= K["MV_CL_EXT"]:
                best_d, best = d, k
        if best is None and len(clo) < n_max:
            clo.append(p)
            chi.append(p)
            best = len(clo) - 1
        if best is None:
            best = near1
        clo[best] = tuple(min(clo[best][k], p[k]) for k in range(3))
        chi[best] = tuple(max(chi[best][k], p[k]) for k in range(3))
    boxes = [(_sub(lo, (pad,) * 3), _add(hi, (pad,) * 3)) for lo, hi in zip(clo, chi)]
    abox = None
    for (c, rs) in atts:
        r = max(r, _len(_sub(c, C)) + rs)
        lo, hi = _sub(c, (rs,) * 3), _add(c, (rs,) * 3)
        abox = (lo, hi) if abox is None else (tuple(min(abox[0][k], lo[k]) for k in range(3)),
                                               tuple(max(abox[1][k], hi[k]) for k in range(3)))
    return C, r, boxes, abox


def mover_seg(mover, P, X, rl):     # mvSegHit (the sphere) AND mvBoxSeg (the fine bound)
    c, r, boxes, abox = mover
    if not seg_hit(P, X, c, r + rl):
        return False
    if boxes is None:                # an object: its sphere alone
        return True
    if any(seg_hit_box(P, X, lo, hi, rl) for (lo, hi) in boxes):
        return True
    return abox is not None and seg_hit_box(P, X, abox[0], abox[1], rl)


# A standing humanoid's joints about the pelvis (m; LL table order: spine, head, arms, legs, collision volumes, then the
# attachment points) -- the avatar_skeleton.xml layout, coarse.
_BODY = [(0, 0, 0), (0, 0, .1), (0, 0, .2), (0, 0, .3), (0, 0, .4), (0, 0, .45), (0, 0, .55), (0, 0, .64),
         (0, 0, .74), (0, 0, .85), (.03, .08, .78), (-.03, .08, .78), (0, .09, .74),
         (.08, 0, .55), (.2, 0, .55), (.22, 0, .28), (.22, 0, .02), (.22, .02, -.08),
         (-.08, 0, .55), (-.2, 0, .55), (-.22, 0, .28), (-.22, 0, .02), (-.22, .02, -.08),
         (.1, 0, -.05), (.1, 0, -.5), (.1, 0, -.92), (.1, .1, -.98), (.1, .18, -.99),
         (-.1, 0, -.05), (-.1, 0, -.5), (-.1, 0, -.92), (-.1, .1, -.98), (-.1, .18, -.99),
         (0, -.08, -.02), (0, .08, .15), (0, -.08, .35), (0, .1, .45), (0, 0, .64), (0, 0, .78),
         (.2, 0, .4), (-.2, 0, .4), (.1, 0, -.3), (-.1, 0, -.3), (.1, 0, -.75), (-.1, 0, -.75),
         (0, .1, .9), (.15, 0, .7), (-.15, 0, .7), (0, -.15, .4)]


def _avatar_rows(rnd, C, scale=1.0, hud=False, absent=12):
    rows = [(C, "joint")]
    pose = [tuple(o + rnd.uniform(-0.06, 0.06) for o in off) for off in _BODY[1:]]
    for off in pose:
        rows.append((_add(C, _mul(off, scale)), "joint"))
    for _ in range(absent):                                     # absent rows: the identity, at the world origin
        rows.append(((0.0, 0.0, 0.0), "identity"))
    rows.append(((0.0, 0.0, 0.0), "zero"))                      # an unwritten row (a flexi pseudo slot's)
    if hud:                                                     # the own avatar's HUD points, under mScreen at 0
        for k in range(8):
            rows.append(((0.0, 0.3 * (k % 3) - 0.3, 0.3 * (k // 3) - 0.3), "joint"))
    return rows


def _caster_points(rnd, rows, atts, K, eps, n=24):
    """Points of the avatar's casters: skin within MV_JOINT_PAD of a REAL joint, attachments inside their spheres."""
    C = rows[0][0]
    real = [C] + [p for (p, kind) in rows[1:] if joint_real((p, kind), C, K)]
    pts = []
    while len(pts) < n:
        if atts and rnd.random() < 0.3:
            c, rs = rnd.choice(atts)
            rad = rs - eps
        else:
            c, rad = rnd.choice(real), K["MV_JOINT_PAD"] - eps
        d = (rnd.gauss(0, 1), rnd.gauss(0, 1), rnd.gauss(0, 1))
        pts.append(_add(c, _mul(d, rad * rnd.random() ** (1 / 3) / max(_len(d), 1e-9))))
    return pts


def _select(P, ls, movers, pool):
    return [any(mover_seg(movers[m], P, X, rl) for m in pool) for (X, rl) in ls]


def _replay(K, seed=1106):
    """Random scenes far from the world origin: avatars (THE AVATAR BOUND: joint rows with absent / HUD rows mixed in,
    attachments) and objects (spheres) whose CASTERS are points strictly inside what their bound covers (a 2 cm ball
    each), point / soft lights, 4 x 8 receiver tiles (one subgroup). Truth: a ray from the texel to a point of the
    light's disc passes through a caster ball. The selection must hold every such texel."""
    rnd = random.Random(seed)
    eps = 0.02
    base = (180.0, 60.0, 22.0)
    blocked = selected = total = wrong = 0
    for _scene in range(24):
        movers, casters = [], []
        for _ in range(rnd.randint(1, 10)):
            c = _add(base, (rnd.uniform(-5, 5), rnd.uniform(-5, 5), rnd.uniform(0.3, 2.0)))
            if rnd.random() < 0.7:
                rows = _avatar_rows(rnd, c, rnd.uniform(0.8, 1.3), hud=rnd.random() < 0.3)
                real = [p for (p, kind) in rows if kind == "joint" and joint_real((p, kind), c, K)]
                atts = [(_add(rnd.choice(real), (rnd.uniform(-.2, .2), rnd.uniform(-.2, .2), rnd.uniform(-.2, .2))),
                         rnd.uniform(0.05, 0.4)) for _ in range(rnd.randint(0, 4))]
                movers.append(avatar_bound(rows, atts, K))
                casters.append(_caster_points(rnd, rows, atts, K, eps))
            else:
                r = rnd.uniform(0.2, 1.0)
                movers.append((c, r, None, None))
                pts = []
                while len(pts) < 24:
                    p = tuple(c[k] + rnd.uniform(-r, r) for k in range(3))
                    if _len(_sub(p, c)) <= r - eps:
                        pts.append(p)
                casters.append(pts)
        lights = [(_add(base, (rnd.uniform(-6, 6), rnd.uniform(-6, 6), rnd.uniform(1.5, 4.0))), rnd.uniform(4.0, 12.0),
                   rnd.choice((0.0, 0.03, 0.15, 0.4, 0.8))) for _ in range(rnd.randint(1, 8))]
        for _tile in range(12):
            ox, oy = rnd.uniform(-6, 6), rnd.uniform(-6, 6)
            tl = []
            for j in range(4):
                for i in range(8):
                    P = _add(base, (ox + 0.05 * i, oy + 0.05 * j, 0.0))
                    ls = [(X, rl) for (X, rng, rl) in lights if _len(_sub(X, P)) < rng]
                    tl.append((P, ls, max([_len(_sub(X, P)) + rl for (X, rl) in ls], default=0.0)))
            # mvCullBuild: the subgroup's receivers' bounding sphere, grown by the farthest reach (the hl predicate)
            mn = tuple(min(P[k] for P, _, _ in tl) for k in range(3))
            mx = tuple(max(P[k] for P, _, _ in tl) for k in range(3))
            tc = _mul(_add(mn, mx), 0.5)
            tr = 0.5 * _len(_sub(mx, mn)) + 0.05
            rch = max(r for _, _, r in tl)
            cand = [m for m, (c, r, _, _) in enumerate(movers) if rch > 0.0 and _len(_sub(c, tc)) <= r + tr + rch]
            pool = range(len(movers)) if len(cand) > MV_CAND else cand
            for (P, ls, _reach) in tl:
                total += 1
                # pdnMoverExactPlan's segment test per light: mvSegHitFirst over the candidates, the disc growth rl
                # (the texel is exact when ANY light's is -- checked here per light, the stronger property)
                sel_l = _select(P, ls, movers, pool)
                selected += 1 if any(sel_l) else 0
                for li, (X, rl) in enumerate(ls):
                    # (a necessary condition only -- a caster ball within reach of the cone P -> disc -- to keep it fast)
                    near_m = [m for m, (c, r, _, _) in enumerate(movers) if seg_hit(P, X, c, r + rl + eps)]
                    hit = False
                    for _ in range(8 if near_m else 0):   # (the disc's outer part weighted: the penumbra blockers)
                        d = (rnd.gauss(0, 1), rnd.gauss(0, 1), rnd.gauss(0, 1))
                        Y = _add(X, _mul(d, rl * (0.6 + 0.4 * rnd.random()) / max(_len(d), 1e-9)))
                        if any(seg_hit(P, Y, q, eps) for m in near_m for q in casters[m]):
                            hit = True
                            break
                    if hit:
                        blocked += 1
                        if not sel_l[li]:
                            wrong += 1
    return total, selected, blocked, wrong


def _small_room(K, old=False):
    """THE OWNER'S SCENE (2026-11-14 log): a house room 9.4 x 3.8 m, floor z 21.0, ceiling 23.6, point lights in the
    ceiling at (231.02, 56.21, 23.41) and (239.48, 56.21, 23.41) (range 15 m, disc 0.03 m); the own avatar standing in the middle (pelvis 1.0 m up, ~1.9 m tall) with its HUD points and absent rows in its
    joint table, a hat and a backpack. Ceiling and upper-wall texels: none may be selected (no caster lies between them
    and a ceiling light). The floor around the feet: the avatar's real shadow -- must be selected."""
    rnd = random.Random(29)
    C = (235.0, 56.0, 22.0)
    rows = _avatar_rows(rnd, C, 1.0, hud=True, absent=40)
    atts = [((235.0, 56.0, 22.9), 0.15), ((235.0, 55.85, 22.45), 0.25)]
    av = avatar_bound(rows, atts, K, old=old)
    lights = [((231.02, 56.21, 23.41), 0.03), ((239.48, 56.21, 23.41), 0.03)]
    movers = [av]
    ceil_n = ceil_sel = wall_n = wall_sel = floor_n = floor_sel = 0
    for i in range(48):
        for j in range(20):
            x, y = 230.6 + 9.2 * i / 47.0, 54.3 + 3.6 * j / 19.0
            P = (x, y, 23.6)
            ls = [(X, rl) for (X, rl) in lights if _len(_sub(X, P)) < 15.0 and X[2] < P[2]]
            ceil_n += 1
            ceil_sel += 1 if any(_select(P, ls, movers, [0])) else 0
    for i in range(48):
        for k in range(6):                 # the upper 0.6 m of the far wall (y 57.95), facing into the room
            P = (230.6 + 9.2 * i / 47.0, 57.95, 23.0 + 0.1 * k)
            ls = [(X, rl) for (X, rl) in lights[:2]]
            wall_n += 1
            wall_sel += 1 if any(_select(P, ls, movers, [0])) else 0
    for i in range(13):
        for j in range(13):                # the floor within 0.3 m of the feet, toward the ceiling lights
            P = (234.7 + 0.6 * i / 12.0, 55.7 + 0.6 * j / 12.0, 21.0)
            floor_n += 1
            floor_sel += 1 if any(_select(P, [(X, rl) for (X, rl) in lights[:2]], movers, [0])) else 0
    return ceil_n, ceil_sel, wall_n, wall_sel, floor_n, floor_sel


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    read.root = ctx.root
    problems, mvs = _structure(read)
    inst = _strip(read(SH + "clean_rt_instances.comp"))
    K = _consts(mvs)
    # (i) the replay's predicates are the production ones (the shared bodies' lines, moverAvatar's bound)
    if "hl = locWant && length(s.xyz - tc) <= s.w + tr + rch;" not in mvs \
            or "const float tr = 0.5 * length(tmx - tmn) + 0.05;" not in mvs \
            or mvs.count("mvSegHit(P, X, s.xyz, s.w + rl) && mvBoxSeg(m, P, X, rl)") != 2 \
            or "if (mvSegHitBox(P, X, clo, chi, rl)) return true;" not in mvs \
            or "return mvAvBox(m, lo, hi) && mvSegHitBox(P, X, lo, hi, rl);" not in mvs \
            or "if (nc == 0u) return true;" not in mvs \
            or "lo -= vec3(r); hi += vec3(r);" not in mvs or "return dot(d, d) <= r * r;" not in mvs:
        problems.append("(i) the shared cull's predicates changed: update the replay (mover_exact_mode2.py) with them")
    ma = _function(inst, "moverAvatar") or ""
    jr = _function(inst, "mvJointReal") or ""
    if None in K.values() \
            or "if (d <= MV_CL_LINK && d < bestD && mvClExtent(clo[k], chi[k], p) <= MV_CL_EXT) { bestD = d; best = k; }" \
            not in ma or "if (best == MV_CL_N && nc < MV_CL_N) { best = nc; clo[nc] = p; chi[nc] = p; ++nc; }" not in ma \
            or "if (best == MV_CL_N) best = near1;" not in ma or "if (!mvJointReal(W, C)) continue;" not in ma \
            or "if (W == mat4(1.0)) return false;" not in jr \
            or "if (length(p) < MV_HUD_R && length(C) > 4.0 * MV_HUD_R) return false;" not in jr:
        problems.append("(i) moverAvatar's joint clustering / mvJointReal changed: update the replay (avatar_bound / "
                        "joint_real) with them")
        K = None
    if K is not None:
        total, selected, blocked, wrong = _replay(K)
        if wrong:
            problems.append("(i) replay: %d mover-blocked (texel, light) pairs were NOT selected -- a dropped mover "
                            "shadow" % wrong)
        if blocked == 0:
            problems.append("(i) replay: no blocked pair at all -- the fixture tests nothing")
        cn, cs, wn, ws, fn, fs = _small_room(K)
        ocn, ocs, own, ows, _ofn, _ofs = _small_room(K, old=True)
        if cs or ws:
            problems.append("(i) the small room: %d of %d ceiling and %d of %d upper-wall texels are selected -- no "
                            "mover lies between them and a ceiling light" % (cs, cn, ws, wn))
        if fs < fn:
            problems.append("(i) the small room: only %d of %d floor texels at the avatar's feet are selected (its real "
                            "shadow)" % (fs, fn))
        if ocs + ows == 0:
            problems.append("(i) the small room: the OLD bound (every row, one box) selects no ceiling texel either -- "
                            "the case no longer reproduces the owner's scene")
    if problems:
        result.fail("\n".join(problems))
        return
    result.summary = ("replay: %d texels, %d selected (%.1f %%), %d texel-light pairs whose rays a mover blocks, every "
                      "one selected | small room: ceiling %d/%d, upper walls %d/%d selected (the old bound: %d/%d, "
                      "%d/%d), feet %d/%d"
                      % (total, selected, 100.0 * selected / max(total, 1), blocked, cs, cn, ws, wn, ocs, ocn, ows, own,
                         fs, fn))
