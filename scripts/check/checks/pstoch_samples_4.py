"""Regression guard for RenderVKProjStochasticSamples 1-4 (2026-11-13: the stochastic local-light shadow ray budget,
3 and 4 rays per denoiser texel for the owner's A/B; it was clamped to 1-2). 2026-11-15: 1-16 in ONE pass -- K > 4 on
the split core's high-K SELECT (-DPDN_HIGH_K: streams 4..K-1 + exact candidates 5..16 in shared memory) with a SECOND
descriptor extension (5 uvec4 per texel past the first, 160 B per texel); bits 0-2 keep min(K, 4) for every K <= 4 reader,
bits 8-12 carry K > 4, bits 13-17 pass 0's K in an extra pass with its own (RenderVKProjStochasticAdaptiveSamples).

K reaches pass 15 through push rtr.w bits 0-2 (bit 4: the GEOM parity) and lays out:
  (a) THE CONTROL WORD: host clamp 1..4, `K | parity << 4`; both kernels read `ctl & 7u` clamped to 1..4 (the generic
      core through pdnSamplesK); no reader keeps the old two-bit mask.
  (b) THE SELECTION DESCRIPTORS: 4 uvec4 per texel exactly as before, + ONE uvec4 per texel past all of them (picks 2-3
      and their targets, or the exact list's 3rd / 4th candidate) only while K > 2: the host sizes 64 / 80 B per texel
      (pdnSelBytesWanted), reallocates on the change and keeps the split core off until the bytes fit; SELECT takes
      K > 2 only with the extension bound (else K = 2: never a write past the range). The extension is written and read
      for the same lists (a pick / exact list longer than two samples).
  (c) THE VISIBILITY SLOTS / JOBS / GATE: pdnVisSlotsWanted takes K <= 4; every reachable slot count fits the job
      word's slot field (PDN_JOB_SLOT_BITS) and the gate's 28 bits (bits 28-31: the mover countdown).
  (d) THE WEIGHTING: pick j weighs f2 / (Ke p_j); in radiance mode with a reused reservoir (radK) the K - 1 fresh
      picks' targets are stored scaled by Wsum / WsumF, so the reservoir pick weighs W / K and each fresh one
      WsumF / (K p_j) -- K unbiased estimates of the same light, combined with the CONFIDENCE WEIGHTS (2026-11-14,
      pdnSampleLight): SELECT stores c = min(M, Ke - 1) in the pick list's u2 and the reservoir's pick takes
      c / (c + Ke - 1), each fresh pick 1 / (c + Ke - 1) (Ke = 2: c = 1, the plain mean as before). A REPLAY of
      SELECT's K streaming reservoirs (the shader's loop, line for line) and that weighting shows the estimate
      unbiased for K = 1..4, with and without the reused reservoir, and its variance falling with K. The evidence weight of a reused list's fresh picks is Ke / (Ke - 1)
      (x 2 at Ke = 2, as before).
  (e) K = 1 / 2 UNCHANGED: 64 B descriptors, no extension read or write at nProj <= 2, Ke = 2 from PDN_SEL_KE2 there,
      the evidence weight 2.
"""
import random
import re

NAME = "pstoch-samples-4"
STAGE = "source"
DESCRIPTION = "RenderVKProjStochasticSamples 1-16: the control word, the descriptors' K > 2 / K > 4 extensions, the slot / job " \
              "/ gate layout, unbiased K-pick weighting; K = 1 / 2 unchanged"
GUARDS = ("A K = 3 / 4 frame writing past the selection descriptors into the job list / GI prefix, picks 2-3 traced "
          "with the wrong light or weight (biased / brighter / darker local lights), a slot count past the job word or "
          "gate, and K = 1 / 2 changing behind the A/B.")

DL = "indra/vkclean/shaders/clean_deferred_light.comp"
COMMON = "indra/vkclean/shaders/clean_pdn_common.glsl"
PDN = "indra/newview/vkgfx/llcleanprojstochvk.cpp"
HDR = "indra/newview/vkgfx/llcleanrenderervk.h"
SETTINGS = "indra/newview/app_settings/settings.xml"
REGIR_RES_MAX = 16          # llcleanrenderervk.h kRegirResMax (pdnVisSlotsWanted's dense window bound)
SPLIT_MAX = 4               # pdnVisSlotsWanted's split-light bound
K_MAX = 16                  # llcleanrenderervk.h kPdnKMax (RenderVKProjStochasticSamples / AdaptiveSamples)
KS = (1, 2, 3, 4, 8, 16)    # the replay's K (5-16: the high-K SELECT's streams 4..K-1, the same loop)
TRIALS = {1: 60000, 2: 60000, 3: 60000, 4: 60000, 8: 20000, 16: 12000}


def _strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _body(src, sig):
    i = src.find(sig)
    if i < 0:
        return None
    j = src.find("{", i)
    depth = 0
    for k in range(j, len(src)):
        depth += {"{": 1, "}": -1}.get(src[k], 0)
        if depth == 0:
            return src[i:k + 1]
    return None


def _structure(dl, common, pdn, hdr, settings):
    bad = []
    # (a) the control word
    if "mPdnK = std::clamp<U32>(k, 1u, kPdnKMax);" not in pdn or "static constexpr uint32_t kPdnKMax = 16u;" not in hdr:
        bad.append("(a) pdnFrame no longer clamps RenderVKProjStochasticSamples to 1..16 (kPdnKMax)")
    if "rp.rtr[3] = (float)pdnCtlWord(mPdnKNow, mPdn->parity(), 0u);" not in pdn:
        bad.append("(a) pass 0's control word is not pdnCtlWord(pass 0's K, parity)")
    cw = _body(pdn, "uint32_t LLCleanRendererVK::pdnCtlWord(") or ""
    if "return std::min<uint32_t>(k, 4u) | ((parity & 1u) << 4) | (k > 4u ? (k << 8) : 0u)" not in cw \
            or "| (std::min<uint32_t>(k0, kPdnKMax) << 13);" not in cw:
        bad.append("(a/e) pdnCtlWord is not min(K, 4) | parity << 4 | K << 8 (K > 4) | k0 << 13 (K <= 4, k0 0: as before)")
    # (a) K > 4 runs only on the split core with the high-K SELECT for this frame's flavour; else 4 (the fused kernels)
    if "mPdnKNow = (mPdnK > 4u && !(mLightPrep.split && mPdnSelectHkPipe[mLightPrep.merged ? 1 : 0])) ? 4u : mPdnK;" \
            not in pdn or "mPdnKNow > 4u ? mPdnSelectHkPipe[merged ? 1 : 0] : mPdnSelectPipe[merged ? 1 : 0]" not in pdn:
        bad.append("(a) pass 0 pushes K > 4 without the split core's high-K SELECT bound")
    if "if (kx > 4u && !mPdnSelectHkPipe[0]) kx = 4u;" not in pdn \
            or "const VkPipeline selX = kx > 4u ? mPdnSelectHkPipe[0] : mPdnSelectPipe[0];" not in pdn:
        bad.append("(a) an extra pass pushes K > 4 without the high-K SELECT bound")
    if "ctl & 3u" in dl:
        bad.append("(a) a pass-15 reader still masks K with two bits (ctl & 3u)")
    if dl.count("const uint K = clamp(ctl & 7u, 1u, 4u);") != 2:
        bad.append("(a) the per-light-type kernel / pdnSamplesK do not read K as ctl & 7u clamped to 1..4")
    if "const uint K = pdnSamplesK(ctl, D);" not in dl:
        bad.append("(a) the generic core's K is not pdnSamplesK (the extension guard)")
    sk = _body(dl, "uint pdnSamplesK(") or ""
    if "if (K > 2u && !pdnSelExtOn(D)) return 2u;" not in sk:
        bad.append("(b) pdnSamplesK no longer drops to K = 2 without the descriptors' extension bound")
    if "#if CLEAN_SHADOW_STAGE != SH_STAGE_SELECT || defined(PDN_HIGH_K)" not in sk \
            or "if (Kh > 4u && pdnSelExt2On(D)) return min(Kh, 4u + PDN_SEL_E2_N);" not in sk \
            or sk.find("if (K > 2u && !pdnSelExtOn(D))") > sk.find("const uint Kh"):
        bad.append("(b) pdnSamplesK takes K > 4 without the second extension bound, or in the K <= 4 SELECT")
    # (b) THE SECOND EXTENSION (K > 4): 5 uvec4 per texel past the first, the binding's length tested
    # (#1608: N = every view's texels, pdnTexels)
    if "uint pdnSelExt2Index(uint sdi, ivec2 D) { return 5u * pdnTexels(D) + (PDN_SEL_E2_WORDS / 4u) * (sdi >> 2u); }" \
            not in dl or "bool pdnSelExt2On(ivec2 D) { return uint(pdn_sel.length()) >= (5u + PDN_SEL_E2_WORDS / 4u) * pdnTexels(D); }" \
            not in dl or "uint pdnTexels(ivec2 D) { return uint(D.x * D.y) * uint(kViewCount); }" not in dl \
            or "#define PDN_SEL_E2_WORDS 20u" not in dl or "#define PDN_SEL_E2_N     12u" not in dl:
        bad.append("(b) the second extension is not 5 uvec4 per texel past 5 N (its length tested)")
    if "d.e2 = (!d.allL && !d.dense && d.nProj > 4u) ? pdnSelExt2Index(sdi, ivec2(g_draw_push.rtr.xy + 0.5)) : 0xFFFFFFFFu;" \
            not in (_body(dl, "PdnSelD pdnSelLoad(") or ""):
        bad.append("(b) pdnSelLoad does not name the second extension for exactly the > 4-sample pick / exact lists")
    # the high-K SELECT: streams 4..K-1 after picks 0-3 (stream order: K <= 4 draws as before), exact 5..16, stored
    # before the gate loop (its pdnSampleLight reads it back)
    if dl.count("for (uint k = 0u; k < K4; ++k)") != 3 \
            or len(re.findall(r"PDN_HK_STREAMS\(seed, Wsum, \w+, li, p\);", dl)) != 3 \
            or dl.count("else PDN_HK_EXACT(nCand, li, ") != 3 or "const uint K4 = min(K, 4u);" not in dl:
        bad.append("(d) SELECT's three walks do not draw picks 0-3 then the high-K streams 4..K-1 / exact 5..16")
    hs = _body(dl, "void pdnHkStreams(") or ""
    if "for (uint k = 4u; k < K; ++k)" not in hs or "if (pdnRand(seed) * Wsum < w)" not in hs:
        bad.append("(d) pdnHkStreams is not SELECT's streaming reservoir for streams 4..K-1")
    i_st, i_gate = dl.find("pdnHkStore(sel.e2, exact, radK ? Wsum / WsumF : 1.0);"), dl.find("uint gateM = 0u;")
    if i_st < 0 or i_gate < 0 or i_st > i_gate:
        bad.append("(b) the high-K SELECT does not store the second extension before its gate loop (radK's scale)")
    if "#if CLEAN_SHADOW_STAGE == SH_STAGE_SELECT && defined(PDN_HIGH_K)" not in dl:
        bad.append("(e) the high-K SELECT's shared memory is not confined to its own variant")
    sel_bytes = _body(pdn, "VkDeviceSize LLCleanRendererVK::pdnSelBytesWanted(") or ""
    if "return (VkDeviceSize)w * h * mViewCount * (k > 4u ? 160u : (k > 2u ? 80u : 64u));" not in sel_bytes \
            or "const uint32_t k = pdnKMax();" not in sel_bytes:
        bad.append("(b/e) the host does not size the descriptors 64 / 80 / 160 B per texel for the largest K")
    # (b) the descriptors + the extension
    # (#1608: the texel index is view-inclusive -- pdnTexel, one view: m.y x D.x + m.x)
    if "uint pdnSelIndex(ivec2 m, ivec2 D) { return 4u * pdnTexel(m, D); }" not in dl \
            or "uint pdnTexel(ivec2 m, ivec2 D) { return (CLEAN_VIEW * uint(D.y) + uint(m.y)) * uint(D.x) + uint(m.x); }" \
            not in dl:
        bad.append("(b/e) the descriptors' stride changed (4 uvec4 per texel)")
    if "uint pdnSelExtIndex(uint sdi, ivec2 D) { return 4u * pdnTexels(D) + (sdi >> 2u); }" not in dl:
        bad.append("(b) the extension is not one uvec4 per texel past all the descriptors")
    if "bool pdnSelExtOn(ivec2 D) { return uint(pdn_sel.length()) >= 5u * pdnTexels(D); }" not in dl:
        bad.append("(b) pdnSelExtOn does not test the binding's length for the extension")
    sf = _body(pdn, "void LLCleanRendererVK::pdnSelFrame(") or ""
    if "const VkDeviceSize selBytes = pdnSelBytesWanted(w, h);" not in sf or "mPdnSelBytes == selBytes) return;" not in sf:
        bad.append("(b) pdnSelFrame does not reallocate when the descriptors' size changes with K")
    sp = _body(pdn, "bool LLCleanRendererVK::pdnSplitPlanned(") or ""
    if "mPdnSelBytes >= pdnSelBytesWanted(mPdnSelW, mPdnSelH)" not in sp:
        bad.append("(b) pdnSplitPlanned runs the split core before the descriptors' extension is allocated")
    if "VkDeviceSize pdnSelBytesWanted(uint32_t w, uint32_t h) const;" not in hdr:
        bad.append("(b) llcleanrenderervk.h lacks pdnSelBytesWanted")
    if "if (!exAll && !dense && nProj > 2u) pdn_sel[pdnSelExtIndex(si, D)] = sel.x;" not in dl:
        bad.append("(b) SELECT does not write the extension for exactly the >2-sample pick / exact lists")
    ld = _body(dl, "PdnSelD pdnSelLoad(") or ""
    if "if (!d.allL && !d.dense && d.nProj > 2u)" not in ld \
            or "d.x = pdn_sel[pdnSelExtIndex(sdi, ivec2(g_draw_push.rtr.xy + 0.5))];" not in ld:
        bad.append("(b) pdnSelLoad does not read the extension for the same lists SELECT writes it for")
    if "d.Ke = (sd0.x & PDN_SEL_KE2) == 0u ? 1u : (picks && d.nProj > 2u ? d.nProj : 2u);" not in ld:
        bad.append("(d/e) pdnSelLoad's Ke is not 1 / 2 / nProj (a pick list's length)")
    if "if (Ke >= 2u) q0.x |= PDN_SEL_KE2;" not in dl:
        bad.append("(d) SELECT does not flag every Ke >= 2 list")
    # SELECT's pick streams (the replay's loop): three walks, four named picks each
    if dl.count("else if (k == 2u) { pick2 = li; pp2 = p; }") != 3 \
            or dl.count("else              { pick3 = li; pp3 = p; }") != 3:
        bad.append("(d) SELECT's three walks (grid, emitters, cluster) do not all keep picks 2-3")
    if dl.count("else if (nCand == 4.0) { ex3 = li;") != 3:
        bad.append("(b) SELECT's three walks do not all record the exact list's 3rd / 4th candidate")
    if "uint pick[4] = uint[4](" not in dl or "wgt = Wsum / (float(K) * ppick[min(i, 3u)]);" not in dl:
        bad.append("(d) the per-light-type kernel does not keep 4 picks")
    # (c) slots / jobs / gate
    if "std::clamp<U32>(pdnKMax(), 1u, kPdnKMax), dense)" not in pdn:
        bad.append("(c) pdnVisSlotsWanted does not take the largest K (<= 16)")
    m = re.search(r"#define PDN_JOB_SLOT_BITS (\d+)u", dl)
    if not m:
        bad.append("(c) PDN_JOB_SLOT_BITS not found")
    m2 = re.search(r"#define PDN_GATE_CD_SHIFT (\d+)u", dl)
    if not m2:
        bad.append("(c) PDN_GATE_CD_SHIFT not found")
    # (d) the weighting
    sl = _body(dl, "uint pdnSampleLight(") or ""
    if "wgt = d.f2 * share / pp;" not in sl or "const float pp = pdnSelPickP(d, j);" not in sl \
            or "float share = 1.0 / float(d.Ke);" not in sl \
            or "share = (j == 0u ? c : 1.0) / (c + float(d.Ke) - 1.0);" not in sl \
            or "if (!d.allL && d.u2 != 0u && (linfo.fwd.z & LIGHT_KPICK_PLAIN) == 0u)" not in sl:
        bad.append("(d) pdnSampleLight's pick weight is not f2 x share / p_j (1 / Ke, or the confidence weights)")
    if "sel.u2 = floatBitsToUint(clamp(rsM, 1.0, float(min(Ke, 4u) - 1u)));" not in dl:
        bad.append("(d) radK does not store the reservoir's confidence capped at min(Ke, 4) - 1 in the pick list's u2")
    if "const uint Ke = (reused && !radK) ? 1u : K;" not in dl:
        bad.append("(d) Ke is not K (radiance with reuse: radK) / 1 (a lone reused pick)")
    if "sel.f1 = pp1 * (Wsum / WsumF);" not in dl \
            or "if (Ke > 2u) sel.x.zw = floatBitsToUint(vec2(pp2, pp3) * (Wsum / WsumF));" not in dl:
        bad.append("(d) radK does not scale every fresh pick's target by Wsum / WsumF")
    if "pdnRadClamp(pdnSelPickP(sel, si - s0), radDf, radSf, dt, st);" not in dl:
        bad.append("(d) the radiance firefly clamp does not read each pick's own target")
    ew = _body(common, "float pdnEvidenceWeight(") or ""
    if "return !reused ? 1.0 : (j == 0u ? 0.0 : float(Ke) / float(max(Ke, 2u) - 1u));" not in ew:
        bad.append("(d/e) pdnEvidenceWeight is not Ke / (Ke - 1) for a reused list's fresh picks")
    # the setting
    i = settings.find("<key>RenderVKProjStochasticSamples</key>")
    if i < 0 or "per frame, 1-16 (" not in settings[i:i + 1200]:
        bad.append("settings.xml: RenderVKProjStochasticSamples' comment does not say 1-16")
    return bad, (int(m.group(1)) if m else None), (int(m2.group(1)) if m2 else None)


def _slots(k, move_cut, move_lights, split_k, exact_spec):
    """llcleanprojstochvk.cpp pdnVisSlotsWanted."""
    dense = min(move_lights, REGIR_RES_MAX - 1) + 1 if move_cut != 0 else 0
    return max(min(max(k, 1), K_MAX), dense) + min(max(split_k, 1 if exact_spec else 0), SPLIT_MAX) + 1


def _layout(job_bits, gate_shift):
    bad = []
    worst = 0
    for k in range(1, K_MAX + 1):
        for move_cut in (0, 1):
            for move_lights in range(0, 20):
                for split_k in range(0, 6):
                    for es in (False, True):
                        s = _slots(k, move_cut, move_lights, split_k, es)
                        worst = max(worst, s)
                        if s < k + 1:
                            bad.append("(c) K = %d: %d slots cannot hold the K picks + the probe" % (k, s))
    if job_bits is not None and worst > (1 << job_bits):
        bad.append("(c) %d slots overflow the job word's %d-bit slot field" % (worst, job_bits))
    if gate_shift is not None and worst > gate_shift:
        bad.append("(c) %d slots overflow the gate's %d bits" % (worst, gate_shift))
    # the descriptors and the extension never overlap, and the extension stays inside 5 N uvec4
    for w, h in ((1, 1), (3, 2), (17, 5), (64, 36)):
        n = w * h
        idx = [4 * (y * w + x) for y in range(h) for x in range(w)]
        ext = [4 * n + (i >> 2) for i in idx]
        if max(i + 3 for i in idx) >= 4 * n or min(ext) < 4 * n or max(ext) >= 5 * n or len(set(ext)) != n:
            bad.append("(b) %dx%d: the extension overlaps the descriptors or leaves 5 N" % (w, h))
        # (K > 4) the second extension: 5 uvec4 per texel in [5 N, 10 N), disjoint per texel
        e2 = [5 * n + 5 * (i >> 2) + q for i in idx for q in range(5)]
        if min(e2) < 5 * n or max(e2) >= 10 * n or len(set(e2)) != 5 * n:
            bad.append("(b) %dx%d: the second extension overlaps the first or leaves 10 N" % (w, h))
    # its words: 6 light pairs + 12 targets (picks) or 12 candidates (exact) within 20 words, samples 4..15
    if 6 + 12 > 20 or 12 > 20 or 4 + 12 != K_MAX:
        bad.append("(b) the second extension's words do not hold samples 4..15")
    return bad, worst


def _evidence(ke, j, reused):
    """clean_pdn_common.glsl pdnEvidenceWeight."""
    return 1.0 if not reused else (0.0 if j == 0 else float(ke) / float(max(ke, 2) - 1))


def _ris_streams(rng, w, k):
    """SELECT's K streaming reservoirs, line for line: per candidate, k draws `pdnRand * Wsum < w`."""
    wsum = 0.0
    picks = [None] * k
    for i, wi in enumerate(w):
        if not wi > 0.0:
            continue
        wsum += wi
        for kk in range(k):
            if rng.random() * wsum < wi:
                picks[kk] = i
    return picks, wsum


def _replay():
    """The estimate of sum_i f_i (f_i = p_i v_i: the shadowed target) through the K picks and their weights,
    without (fresh picks: f2 = Wsum) and with a reused reservoir (radK: the reservoir's pick at W, f2 = W p(y),
    the fresh picks' targets scaled by f2 / WsumF)."""
    rng = random.Random(0x5EED4)
    p = [1.0, 0.6, 0.3, 0.25, 0.05, 0.9]
    v = [1.0, 0.0, 0.5, 1.0, 0.0, 0.25]
    truth = sum(pi * vi for pi, vi in zip(p, v))
    out = {}
    for reuse in (False, True):
        for k in KS:
            trials = TRIALS[k]
            acc = acc2 = 0.0
            for _ in range(trials):
                picks, wsum = _ris_streams(rng, p, k)
                if reuse:
                    # a reused reservoir: an independent RIS pick y with W = Wsum' / p(y) (its own frame's candidates)
                    (y,), wsum_r = _ris_streams(rng, p, 1)
                    W = wsum_r / p[y]
                    ke = k if k >= 2 else 1           # Ke = (reused && !radK) ? 1 : K  (radK needs K >= 2)
                    f2 = W * p[y]                     # Wsum = rsW * py
                    pp = [p[y]] + [p[i] * (f2 / wsum) for i in picks[1:ke]]   # sel.f1 / sel.x.zw scaled
                    li = [y] + picks[1:ke]
                    # (THE CONFIDENCE WEIGHTS: radK stores c = min(M, min(Ke, 4) - 1); a converged reservoir, M >= 3)
                    c = float(max(min(ke, 4) - 1, 1)) if ke >= 2 else 0.0
                    share = [((c if j == 0 else 1.0) / (c + ke - 1)) if c > 0.0 else 1.0 / ke for j in range(ke)]
                else:
                    ke, f2 = k, wsum
                    li = picks[:ke]
                    pp = [p[i] for i in li]
                    share = [1.0 / ke] * ke
                est = sum(f2 * sh / pj * p[i] * v[i] for i, pj, sh in zip(li, pp, share))   # wgt x (dt x vis)
                acc += est
                acc2 += est * est
            mean = acc / trials
            var = max(acc2 / trials - mean * mean, 0.0)
            out[(reuse, k)] = (mean, var)
    bad = []
    for (reuse, k), (mean, var) in sorted(out.items()):
        se = (var / TRIALS[k]) ** 0.5
        if abs(mean - truth) > 5.0 * se + 1e-9:
            bad.append("(d) replay: K = %d%s estimates %.5f, the truth is %.5f (+- %.5f): biased"
                       % (k, " (reused)" if reuse else "", mean, truth, se))
    for reuse in (False, True):
        for a, k in zip(KS, KS[1:]):
            if not out[(reuse, k)][1] < out[(reuse, a)][1]:
                bad.append("(d) replay: K = %d%s does not lower the variance below K = %d"
                           % (k, " (reused)" if reuse else "", k, a))
    # the evidence weights: K = 2 as before (x 2), K = 3 / 4 fresh picks stand for all Ke
    if _evidence(2, 1, True) != 2.0 or _evidence(1, 0, True) != 0.0 or _evidence(2, 1, False) != 1.0:
        bad.append("(e) pdnEvidenceWeight changed for K <= 2")
    for ke in (3, 4, 8, 16):
        if abs(sum(_evidence(ke, j, True) for j in range(ke)) - ke) > 1e-6:
            bad.append("(d) pdnEvidenceWeight: a reused %d-pick list's fresh picks do not stand for all %d" % (ke, ke))
    return bad, truth, out


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    dl, common = _strip(read(DL)), _strip(read(COMMON))
    pdn, hdr = _strip(read(PDN)), _strip(read(HDR))
    settings = read(SETTINGS)
    problems, job_bits, gate_shift = _structure(dl, common, pdn, hdr, settings)
    lay, worst = _layout(job_bits, gate_shift)
    problems += lay
    rep, truth, out = _replay()
    problems += rep
    if problems:
        result.fail("\n".join(problems))
        return
    result.summary = ("slots <= %d (job field %d, gate %d bits) | replay truth %.4f: %s"
                      % (worst, 1 << job_bits, gate_shift, truth,
                         ", ".join("K%d%s %.4f var %.4f" % (k, "r" if r else "", m, v)
                                   for (r, k), (m, v) in sorted(out.items()))))
