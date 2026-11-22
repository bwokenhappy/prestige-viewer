"""Regression guard for RenderVKProjStochasticPasses (2026-11-15, debug: N passes of the K-pick stochastic local-light
shadow estimate per frame -- what N x K rays per texel buy, without widening the K <= 4 layout).

  (a) N = 1 IS TODAY'S FRAME: pass 0 pushes the unchanged frame word, and every new shader branch tests pdnPassIdx()
      (push rtr3.z bits 16-18, 0 unless an extra pass) -- the host sets those bits only in recordProjStochPasses.
  (b) INDEPENDENT PASSES: pass p pushes frame + p x kPassSeedStride. The stride is odd (the emitter R3's frame & 0xFFFF
      moves) and keeps p x stride x every `frame * C` seed multiplier of pass 15 far from 0 mod 2^32 (no texel's seed in
      pass p equals another texel's in pass 0 at any extent); frame + 7 x stride < 2^24 (exact in the push float).
  (c) PASS 0 OWNS THE FRAME: an extra pass reads / stores no reservoirs (host rtr2.w 0 AND the shader's rsResOn), no
      GEOM / MOT / stats / occlusion notes / probe / split samples, never the mover-exact list.
  (d) THE COMBINATION: SHADE's fold V += K (x - V) / (W0 + p K) with W0 = pass 0's weight (pdnPassBaseWeight: c + Ke - 1
      for a radK list) equals ONE list of c + N K - 1 samples under pdnSampleLight's confidence weights -- the
      reservoir's pick counted once; a replay shows the fold equal to that closed form, the estimate unbiased with and
      without a reused reservoir for N = 1..8, and its variance falling with N.
  (e) MULTI-RAY EXACT LISTS (2026-11-16, RenderVKProjStochasticAdaptiveSamples > 0): an adaptive extra pass's exact list
      of nL < K lights traces K rays, sample j = ray j / nL of light j % nL (R_l = (K - 1 - l) / nL + 1 rays, weight
      1 / R_l each; a hard light one ray at weight 1), capped at the rest's visibility slots; only behind the push bit
      the host sets for AdaptiveSamples > 0 in an adaptive extra pass, only in SELECT, never with the probe; the
      descriptor's nL in q0.x bits 16-20 only while nL < nProj (every other descriptor unchanged). The pass still folds
      as K samples (the rays it traced); in pass 1 pass 0's exact list then weighs its own rays (nProj) instead of K0. A
      replay checks the ray map covers every slot once (sum R_l = K), the combination unbiased (soft penumbrae, hard
      lights, exact and pick-list pass 0s) and Adaptive 1 x 16 at or below the variance of the owner's Adaptive 3 x 4.
"""
import random
import re

NAME = "pstoch-passes"
STAGE = "source"
DESCRIPTION = "RenderVKProjStochasticPasses: N = 1 unchanged, independent pass seeds, pass 0 owns the frame, the " \
              "N passes fold as one confidence-weighted sample list (unbiased)"
GUARDS = ("Extra passes repeating pass 0's random numbers (no noise reduction), storing reservoirs / guides / stats N "
          "times, the reservoir's pick weighed N times as if independent, a biased fold, and N = 1 changing.")

DL = "indra/vkclean/shaders/clean_deferred_light.comp"
PDN = "indra/newview/vkgfx/llcleanprojstochvk.cpp"
SETTINGS = "indra/newview/app_settings/settings.xml"


def _strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _structure(dl, pdn, settings):
    bad = []
    if "uint pdnPassIdx() { return (g_draw_push.rtr3.z >> 16u) & 7u; }" not in dl:
        bad.append("(a) pdnPassIdx is not push rtr3.z bits 16-18")
    # (b) the VIS rays' disc phase: float(frame) x R2 has 1/32 .. 1/4 resolution at an extra pass's frame word
    if "rt_noise_shift = fract(rt_noise_shift + float(pdnPassIdx()) * vec2(0.3819660113, 0.6180339887));" not in dl:
        bad.append("(b) an extra pass's VIS disc phase has no explicit per-pass offset (pdnVisRecv)")
    # (c) reservoirs: both rsResOn definitions refuse an extra pass
    if dl.count("&& pdnPassIdx() == 0u;") < 2:
        bad.append("(c) SELECT's / SHADE's rsResOn do not refuse an extra pass")
    if "if (pdnPassIdx() != 0u) return;" not in dl:
        bad.append("(c) an extra SELECT stores the GEOM guide / the stats")
    if "if (pdnPassIdx() != 0u) { pdn_sel[pdnSelIndex(m, D)] = uvec4(0u); return; }" not in dl:
        bad.append("(c) an extra SELECT's empty texel stores more than its descriptor")
    if "if (pdnPassIdx() != 0u && (spT >= 0 || ocP)) continue;" not in dl:
        bad.append("(c) an extra SELECT gates the split / probe samples")
    if "pdnPassIdx() != 0u || pdnRand(pseed)" not in dl:
        bad.append("(c) an extra pass keeps the occlusion probe (or pass 0's probe draw changed)")
    if "ocOn && pdnPassIdx() == 0u && sLi != 0xFFFFFFFFu && ocEv" not in dl:
        bad.append("(c) an extra SHADE writes occlusion notes")
    if "rsFrameOn() && pdnPassIdx() == 0u" not in dl or "emOn && pdnPassIdx() == 0u" not in dl \
            or "ocOn && pdnPassIdx() == 0u)" not in dl:
        bad.append("(c) an extra pass adds to the ReSTIR / emitter / occlusion stats")
    # (d) the fold + the base weight
    if "const float t = kx / (w0 + float(pdnPassIdx()) * kx);" not in dl \
            or "imageStore(uPdnIn, ivec3(m, PDN_L(0)), vec4(mix(v0.rgb, l0, t), v0.a));" not in dl \
            or "imageStore(uPdnIn, ivec3(m, PDN_L(1)), vec4(mix(v1.rgb, l1, t), v1.a));" not in dl \
            or "if (radOn && (opts & PDN_OPT_DBG_RS) == 0u)" not in dl:
        bad.append("(d) SHADE's fold is not V += K (x - V) / (W0 + p K) on layers 0 / 1 rgb (radiance only)")
    if "if (q0.w != 0u && c > 0.0 && c < 64.0) return c + float(Ke) - 1.0;" not in dl \
            or "return (q0.x & PDN_SEL_REUSED) != 0u ? 1.0 : float(Ke);" not in dl \
            or "if (pdnPassIdx() > 1u) return uintBitsToFloat(pdn_sel[si + 3u].y);" not in dl:
        bad.append("(d) pdnPassBaseWeight is not c + Ke - 1 (radK) / 1 (a lone reused pick) / Ke, carried past pass 1")
    # (2026-11-15) an extra pass may take its own K (RenderVKProjStochasticAdaptiveSamples): W0 of an exact / dense pass-0
    # list is pass 0's K (pdnPassK0: push rtr.w bits 13-17), the fold's kx the extra pass's
    if "const float rsMw = pdnPassIdx() != 0u ? pdnPassBaseWeight(si, pdnPassK0(ctl, K), exL != 0u) : rsM;" not in dl \
            or "uint pdnPassK0(uint ctl, uint K) { const uint k0 = (ctl >> 13u) & 31u; return k0 != 0u ? k0 : K; }" not in dl \
            or "const float w0 = rsM > 0.0 && rsM < 1.0e6 ? rsM : float(pdnPassK0(ctl, K));" not in dl \
            or "pdn_sel[si + 3u] = uvec4(rsYl, floatBitsToUint(rsMw)," not in dl:
        bad.append("(d/a) SELECT's q3.y is not rsM in pass 0 / the base weight in an extra pass")
    if "layout(set = 3, binding = 13, rgba16f) uniform image2DArray uPdnIn;" not in dl:
        bad.append("(d) SHADE cannot read IN (the fold)")
    # host
    if "mPdnPasses = std::clamp<U32>(passes, 1u, 8u);" not in pdn:
        bad.append("(a) the host does not clamp RenderVKProjStochasticPasses to 1..8")
    if "xp.rtr2[3] = 0.f;" not in pdn or "xp.rtr3[2] = rp.rtr3[2] | (pass << 16u);" not in pdn \
            or "xp.rtr2[2] = (float)(opts0 & ~512u);" not in pdn:
        bad.append("(c) an extra pass's push keeps the reservoirs / the mover-exact bit, or lacks its pass index")
    if "xp.rtr[2] = (float)(frame0 + pass * kPassSeedStride);" not in pdn:
        bad.append("(b) an extra pass's frame word is not frame + p x kPassSeedStride")
    if "else if (!mPdnRadiance) why = 2u;" not in pdn or "else if (mLightPrep.exact) why = 3u;" not in pdn \
            or "if (!split) why = 1u;" not in pdn:
        bad.append("(c) N > 1 is not refused off the split core / radiance mode / with the mover-exact list")
    sp = pdn[pdn.find("void LLCleanRendererVK::recordProjStochPasses("):]
    for mark in ("c_defer_pstochsel", "c_defer_pstochvis", "\"c_defer_pstoch\"", "c_defer_shadowsel",
                 "c_defer_shadowvis", "c_defer_shadowmerge"):
        if mark not in sp[:6000]:
            bad.append("(timers) the extra passes do not record pass 0's mark %s" % mark)
    m = re.search(r"kPassSeedStride = (\d+)u;", pdn)
    stride = int(m.group(1)) if m else None
    if stride is None:
        bad.append("(b) kPassSeedStride not found")
    i = settings.find("<key>RenderVKProjStochasticPasses</key>")
    blk = settings[i:i + 2600] if i >= 0 else ""
    if i < 0 or not re.search(r"<key>Persist</key>\s*<integer>0</integer>", blk) \
            or not re.search(r"<key>Type</key>\s*<string>U32</string>", blk) \
            or not re.search(r"<key>Value</key>\s*<integer>1</integer>", blk):
        bad.append("settings.xml: RenderVKProjStochasticPasses is not a nonpersistent U32 defaulting to 1")
    return bad, stride


def _multiray(dl, pdn, settings):
    """(e) the multi-ray exact list's structure."""
    bad = []
    if "#define PDN_PASS_MULTIRAY 0x800000u" not in dl \
            or "bool pdnPassMultiRay() { return (g_draw_push.rtr3.z & PDN_PASS_MULTIRAY) != 0u && pdnPassIdx() != 0u; }" \
            not in dl:
        bad.append("(e) pdnPassMultiRay is not push rtr3.z bit 23 in an extra pass")
    if "if (tiles && mPdnKx != 0u) xp.rtr3[2] |= 0x800000u;" not in pdn or pdn.count("0x800000u") != 1:
        bad.append("(e) the host pushes the multi-ray bit outside an adaptive extra pass with AdaptiveSamples > 0")
    i = dl.find("uint exL = 0u;")
    blk = dl[i:i + 600] if i >= 0 else ""
    if i < 0 or "#if CLEAN_SHADOW_STAGE == SH_STAGE_SELECT" not in blk \
            or "if (exact && nProj > 0u && nProj < K && nOcP == 0u && pdnPassMultiRay())" not in blk \
            or "const uint vs = min(pdnVisSlots(), PDN_GATE_CD_SHIFT);" not in blk \
            or "const uint rays = min(K, vs - min(vs, spN + nOcP));" not in blk \
            or "if (rays > nProj) { exL = nProj; nProj = rays; }" not in blk:
        bad.append("(e) SELECT's multi-ray list is not K rays over an extra pass's exact list, capped at the rest's slots")
    if dl.find("uint exL = 0u;") < dl.find("const uint nOcP = ocProbe != 0xFFFFFFFFu ? 1u : 0u;"):
        bad.append("(e) the multi-ray cap is taken before the probe is decided")
    if "if (exL != 0u) q0.x |= exL << PDN_SEL_EXL_SHIFT;" not in dl or "#define PDN_SEL_EXL_SHIFT 16u" not in dl \
            or "sel.nL = exL != 0u ? exL : nProj;" not in dl:
        bad.append("(e) SELECT does not store a multi-ray list's lights in q0.x bits 16-20 (and nL = nProj otherwise)")
    if "if (d.exact && (sd0.x & PDN_SEL_NOCP) == 0u && ((sd0.x >> PDN_SEL_EXL_SHIFT) & 31u) != 0u)" not in dl \
            or "d.nL = min((sd0.x >> PDN_SEL_EXL_SHIFT) & 31u, d.nProj);" not in dl or "d.nL = d.nProj;" not in dl:
        bad.append("(e) pdnSelLoad does not read nL from an exact list without the probe (else nProj)")
    s = dl.find("uint pdnSampleLight(")
    sl = dl[s:s + 6000] if s >= 0 else ""
    if "if (d.exact && d.nL != 0u && d.nL < d.nProj)" not in sl \
            or "const uint l = j % d.nL, ray = j / d.nL;" not in sl \
            or "const uint Rl = (d.nProj - 1u - l) / d.nL + 1u;" not in sl \
            or "wgt = 1.0 / float(Rl);" not in sl \
            or "if (ray != 0u) ni |= (ray << 16u) | (Rl << 20u);" not in sl \
            or "if (li < REGIR_EM_KEY && !(pdnSampleDisc(li) > 0.0))" not in sl \
            or sl.find("if (d.exact && d.nL != 0u && d.nL < d.nProj)") > sl.find("if (d.exact)\n"):
        bad.append("(e) pdnSampleLight's multi-ray map is not ray j / nL of light j % nL at weight 1 / R_l")
    if "const uint ray = pdnNiRay(ni);" not in dl or "ni &= 0xFFFFu;" not in dl \
            or "if (ray != 0u) rt_noise_shift = fract(rt_noise_shift + rayShift);" not in dl \
            or "if (ray != 0u) xi = fract(xi + vec3(rayShift, float(ray) * 0.7548776662));" not in dl:
        bad.append("(e) VIS does not draw each ray r > 0 its own sample (ray 0 unchanged)")
    if "if (multi && (q0.x & PDN_SEL_SURF) != 0u && (q0.x & PDN_SEL_ALL_LIGHTS) == PDN_SEL_EXACT && (q0.x & 31u) != 0u)" \
            not in dl or "return float(min(q0.x & 31u, K));" not in dl:
        bad.append("(e) pdnPassBaseWeight does not weigh pass 0's exact list by its rays beside a multi-ray pass 1")
    if dl.count("const float kx = float(K);") != 1:
        bad.append("(e) SHADE's fold weight is no longer the pass's K (the rays a multi-ray list traced)")
    i = settings.find("<key>RenderVKProjStochasticAdaptiveSamples</key>")
    if i < 0 or "splits the spare rays over its lights" not in settings[i:i + 2400]:
        bad.append("settings.xml: RenderVKProjStochasticAdaptiveSamples does not describe the multi-ray exact lists")
    return bad


def _seeds(dl, stride):
    bad = []
    seeds = sorted({tuple(int(v) for v in s) for s in
                    re.findall(r"pdnPcg\(uint\(m\.x\) \* (\d+)u \+ uint\(m\.y\) \* (\d+)u \+ frame \* (\d+)u", dl)})
    if len(seeds) < 5:
        return ["(b) pass 15's `A m.x + B m.y + C frame` seeds not found (%d)" % len(seeds)], 0.0
    # pass p's seed of texel m equals pass 0's of texel m + d only if A dx + B dy == p stride C (mod 2^32): never while
    # |p stride C| mod 2^32 exceeds the texel reach A W + B H (an 8K denoiser extent)
    worst = 1e18
    for a, b, c in seeds:
        reach = a * 7680 + b * 4320
        for p in range(1, 8):
            v = (p * stride * c) % (1 << 32)
            worst = min(worst, min(v, (1 << 32) - v) / float(reach))
    if not worst > 1.0:
        bad.append("(b) p x stride x a seed's frame multiplier comes within its texel reach of 0 mod 2^32 (%.3f)" % worst)
    if stride % 2 == 0 or any((p * stride) % 0x10000 == 0 for p in range(1, 8)):
        bad.append("(b) the stride leaves the emitter R3's frame & 0xFFFF unchanged for some pass")
    if 0xFFFFF + 7 * stride >= (1 << 24):
        bad.append("(b) frame + 7 x stride is not exact in the push float")
    return bad, worst


def _fold(v0, xs, k, w0):
    """SHADE's fold, pass by pass: V += K (x - V) / (W0 + p K)."""
    v = v0
    for p, x in enumerate(xs, start=1):
        t = k / (w0 + p * k)
        v = v + (x - v) * t
    return v


def _ris(rng, p, f):
    """One RIS pick over the candidates (target p): the one-sample estimate Wsum f(y) / p(y)."""
    wsum = 0.0
    y = None
    for i, pi in enumerate(p):
        wsum += pi
        if rng.random() * wsum < pi:
            y = i
    return wsum * f[y] / p[y]


def _replay():
    bad = []
    rng = random.Random(0xA55E5)
    # (1) the fold == the closed-form one list: (c R + sum of all N K - 1 fresh) / (c + N K - 1)
    for _ in range(2000):
        k = rng.randint(2, 4)
        n = rng.randint(1, 8)
        c = rng.uniform(1.0, k - 1.0)
        r = rng.uniform(0.0, 5.0)
        fresh0 = [rng.uniform(0.0, 5.0) for _ in range(k - 1)]
        extra = [[rng.uniform(0.0, 5.0) for _ in range(k)] for _ in range(n - 1)]
        v0 = (c * r + sum(fresh0)) / (c + k - 1)
        folded = _fold(v0, [sum(e) / k for e in extra], k, c + k - 1)
        closed = (c * r + sum(fresh0) + sum(sum(e) for e in extra)) / (c + n * k - 1)
        if abs(folded - closed) > 1e-9 * max(1.0, abs(closed)):
            bad.append("(d) the fold %.9f differs from the one-list combination %.9f (K %d, N %d)" % (folded, closed, k, n))
            break
    # (1b) the extra passes with their own K (RenderVKProjStochasticAdaptiveSamples, 1-16) after a pass 0 of K0: the fold
    #      with kx and W0 = c + K0 - 1 is the one list of c + K0 - 1 + (N - 1) kx samples
    for _ in range(2000):
        k0 = rng.randint(2, 16)
        kx = rng.randint(1, 16)
        n = rng.randint(2, 8)
        c = rng.uniform(1.0, k0 - 1.0)
        r = rng.uniform(0.0, 5.0)
        fresh0 = [rng.uniform(0.0, 5.0) for _ in range(k0 - 1)]
        extra = [[rng.uniform(0.0, 5.0) for _ in range(kx)] for _ in range(n - 1)]
        v0 = (c * r + sum(fresh0)) / (c + k0 - 1)
        folded = _fold(v0, [sum(e) / kx for e in extra], kx, c + k0 - 1)
        closed = (c * r + sum(fresh0) + sum(sum(e) for e in extra)) / (c + k0 - 1 + (n - 1) * kx)
        if abs(folded - closed) > 1e-9 * max(1.0, abs(closed)):
            bad.append("(d) the fold with K0 %d / Kx %d (N %d) differs from the one-list combination" % (k0, kx, n))
            break
    # (2) unbiased, variance falling with N: the shadowed sum of a toy light set, without / with a reused reservoir
    p = [1.0, 0.6, 0.3, 0.25, 0.05, 0.9]
    vis = [1.0, 0.0, 0.5, 1.0, 0.0, 0.25]
    f = [pi * vi for pi, vi in zip(p, vis)]
    truth = sum(f)
    trials = 30000
    k = 4
    out = {}
    for reuse in (False, True):
        for n in (1, 2, 4, 8):
            acc = acc2 = 0.0
            for _ in range(trials):
                if reuse:
                    c = float(k - 1)   # (a converged reservoir: SELECT's cap min(M, Ke - 1))
                    r = _ris(rng, p, f)   # (an unbiased reservoir estimate, independent of this frame's picks)
                    v0 = (c * r + sum(_ris(rng, p, f) for _ in range(k - 1))) / (c + k - 1)
                    w0 = c + k - 1
                else:
                    v0 = sum(_ris(rng, p, f) for _ in range(k)) / k
                    w0 = float(k)
                xs = [sum(_ris(rng, p, f) for _ in range(k)) / k for _ in range(n - 1)]
                est = _fold(v0, xs, float(k), w0)
                acc += est
                acc2 += est * est
            mean = acc / trials
            var = max(acc2 / trials - mean * mean, 0.0)
            out[(reuse, n)] = (mean, var)
            se = (var / trials) ** 0.5
            if abs(mean - truth) > 5.0 * se + 1e-9:
                bad.append("(d) replay: N = %d%s estimates %.5f, the truth is %.5f (+- %.5f): biased"
                           % (n, " (reused)" if reuse else "", mean, truth, se))
    for reuse in (False, True):
        for a, b in ((1, 2), (2, 4), (4, 8)):
            if not out[(reuse, b)][1] < out[(reuse, a)][1]:
                bad.append("(d) replay: N = %d%s does not lower the variance below N = %d"
                           % (b, " (reused)" if reuse else "", a))
    return bad, truth, out


def _rays_of(n_rays, n_l, l):
    """pdnSampleLight's R_l: light l's rays in a multi-ray exact list of n_rays samples over n_l lights."""
    return (n_rays - 1 - l) // n_l + 1


def _exact_pass(rng, t, v, hard, rays, strat=True):
    """One exact list over the lights (t: terms, v: the visible share of each light's disc, hard: one ray always) with
    `rays` samples (one ray each when rays == len(t)): sample j is ray j // nL of light j % nL, weight 1 / R_l (a hard
    light: ray 0 only, weight 1); each ray's disc point u = base + r / R_l (mod 1) -- VIS's rank-1 shift over one
    uniform base -- visible where u < v (a penumbra edge across the disc)."""
    n_l = len(t)
    base = [rng.random() for _ in range(n_l)]
    est = 0.0
    for j in range(rays):
        l, r = j % n_l, j // n_l
        rl = _rays_of(rays, n_l, l)
        if hard[l]:
            if r != 0:
                continue
            est += t[l] * (1.0 if v[l] >= 0.5 else 0.0)
            continue
        u = (base[l] + float(r) / rl) % 1.0 if strat else rng.random()   # (strat False: independent rays)
        est += t[l] * (1.0 if u < v[l] else 0.0) / rl
    return est


def _pick_pass(rng, t, v, hard, k):
    """K fresh RIS picks over the lights (target t), each one ray: the plain mean of Wsum f / p."""
    wsum = sum(t)
    est = 0.0
    for _ in range(k):
        x = rng.random() * wsum
        i = 0
        while i < len(t) - 1 and x >= t[i]:
            x -= t[i]
            i += 1
        vis = (1.0 if v[i] >= 0.5 else 0.0) if hard[i] else (1.0 if rng.random() < v[i] else 0.0)
        est += wsum * vis / k
    return est


def _replay_multiray():
    bad = []
    # (1) the ray map: every slot j < K one (light, ray), each light R_l rays, sum R_l = K, R_l in {floor, ceil}(K / nL)
    for n_l in range(1, 17):
        for k in range(n_l + 1, 17):
            seen = {}
            for j in range(k):
                seen.setdefault(j % n_l, []).append(j // n_l)
            for l in range(n_l):
                rl = _rays_of(k, n_l, l)
                if sorted(seen.get(l, [])) != list(range(rl)) or rl not in (k // n_l, -(-k // n_l)):
                    bad.append("(e) the ray map gives light %d of %d (K %d) rays %s, R_l %d" % (l, n_l, k, seen.get(l), rl))
            if sum(_rays_of(k, n_l, l) for l in range(n_l)) != k:
                bad.append("(e) the ray map's R_l do not sum to K (%d lights, K %d)" % (n_l, k))
    if bad:
        return bad, {}
    # (2) unbiased + the owner's comparison: Samples 4 + Adaptive 3 (AdaptiveSamples 0: every pass K 4, one ray per
    #     light in an exact list) against Samples 4 + Adaptive 1 + AdaptiveSamples 16 (one multi-ray pass, W0 = pass 0's
    #     exact rays). The lights' terms / disc visibility (a light half in penumbra, ...) and a hard light.
    rng = random.Random(0x5EED16)
    lights_t = [1.0, 0.7, 0.45, 0.3, 0.2, 0.15]
    lights_v = [0.5, 0.3, 0.8, 0.6, 0.45, 0.9]
    trials = 20000
    out = {}
    for n_l in range(1, 7):
        for hard_one in (False, True):
            t, v = lights_t[:n_l], lights_v[:n_l]
            hard = [hard_one and i == n_l - 1 for i in range(n_l)]
            truth = sum(ti * ((1.0 if vi >= 0.5 else 0.0) if h else vi) for ti, vi, h in zip(t, v, hard))
            k0, kx = 4, 16
            stats = {}
            for mode in ("base3x4", "multi1x16", "multi1x16i"):
                acc = acc2 = 0.0
                for _ in range(trials):
                    exact0 = n_l <= k0
                    x0 = _exact_pass(rng, t, v, hard, n_l) if exact0 else _pick_pass(rng, t, v, hard, k0)
                    if mode == "base3x4":
                        xs = [(_exact_pass(rng, t, v, hard, n_l) if exact0 else _pick_pass(rng, t, v, hard, k0))
                              for _ in range(3)]
                        est = _fold(x0, xs, float(k0), float(k0))
                    else:
                        # pass 1: exact (n_l <= 16), K rays over the lights; W0 = pass 0's exact rays, else K0's picks
                        w0 = float(n_l) if exact0 else float(k0)
                        est = _fold(x0, [_exact_pass(rng, t, v, hard, kx, mode == "multi1x16")], float(kx), w0)
                    acc += est
                    acc2 += est * est
                mean = acc / trials
                var = max(acc2 / trials - mean * mean, 0.0)
                se = (var / trials) ** 0.5
                if abs(mean - truth) > 5.0 * se + 1e-9:
                    bad.append("(e) replay %s, %d lights%s: estimates %.5f, the truth is %.5f (+- %.5f): biased"
                               % (mode, n_l, " (+hard)" if hard_one else "", mean, truth, se))
                stats[mode] = var
            out[(n_l, hard_one)] = stats
            # (independent rays too: the gain must not rest on the stratification alone)
            for mode in ("multi1x16", "multi1x16i"):
                if stats[mode] > stats["base3x4"] * 1.05 + 1e-9:
                    bad.append("(e) replay: %d lights%s -- Adaptive 1 x 16 (%s, var %.5f) is noisier than Adaptive 3 x 4 "
                               "(%.5f)" % (n_l, " (+hard)" if hard_one else "", mode, stats[mode], stats["base3x4"]))
    return bad, out


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as fh:
            return fh.read()
    dl, pdn, settings = _strip(read(DL)), _strip(read(PDN)), read(SETTINGS)
    problems, stride = _structure(dl, pdn, settings)
    worst = 0
    if stride is not None:
        sb, worst = _seeds(dl, stride)
        problems += sb
    rb, truth, out = _replay()
    problems += rb
    problems += _multiray(dl, pdn, settings)
    mb, mout = _replay_multiray()
    problems += mb
    if problems:
        result.fail("\n".join(problems))
        return
    result.summary = ("stride %d (seed distance / texel reach >= %.3f) | replay truth %.4f: %s | multi-ray (Adaptive "
                      "3 x 4 -> 1 x 16 stratified / independent rays, var): %s"
                      % (stride, worst, truth, ", ".join("N%d%s var %.4f" % (n, "r" if r else "", v)
                                                          for (r, n), (_, v) in sorted(out.items())),
                         ", ".join("%dL%s %.4f->%.4f/%.4f" % (n, "h" if h else "", s["base3x4"], s["multi1x16"],
                                                               s["multi1x16i"])
                                   for (n, h), s in sorted(mout.items()))))
