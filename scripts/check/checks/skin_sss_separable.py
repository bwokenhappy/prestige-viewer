"""Regression guard for the skin subsurface scattering (RenderVKSkinSSS, clean_deferred_light.comp pass 17).

History (owner reports, RenderVKSkinSSSScale 4):
  1. The first pass-17 kernel gathered the d'Eon / Luebke six-Gaussian skin profile with stratified
     importance taps and a fixed per-pixel rotation: a static red grain across sharp shadow edges. Replaced by
     SEPARABLE screen-space scattering (Jimenez et al. 2009 / 2015, our own implementation): deterministic.
  2. That separable version (a319a122e4) spread far less than the scale asked and broke at corners:
     * it capped the 8 mm reach at 48 px by shrinking the WHOLE kernel (every lobe narrower at close range);
     * one 1D kernel a(x) for the sum of six Gaussians makes a(x) a(y) plus-shaped (cross terms between a
       narrow and a wide lobe): a diagonal shadow edge got up to ~30 % less red tail, a lit corner 0.2 - 0.6 x
       the radial profile's;
     * its follow-surface weights (distance from the centre's tangent plane) also cut the spread on curved skin.
     Now: per-lane separable (red split into lanes 1-4 / 5 / 6, green 1-2 / 3-6), the true reach at any
     distance (an adaptive apron, coarser entries only past SSS_ENT_MAX, no whole-kernel shrink), bins read as
     exact integrals from fixed-point prefix sums (no staircase at any width), and depth BARRIERS (eye-distance
     jumps) instead of the tangent-plane weights.

This check keeps that shape:
  * no random / rotated / hashed taps in the pass-17 block (no fract hash, no cos / sin, no frame counter);
  * two separable passes: the CLEAN_SSS_VERTICAL variant compiled beside the horizontal one, two pipelines,
    two dispatches in recordSkinSss with a barrier between them, the intermediate image (mDsSssTmp, binding
    32, two words per pixel) allocated and freed with the irradiance image; the host's run length = SSS_SEG;
  * the lanes: every lobe with weight in a channel in exactly one lane of that channel, red's lobes 5 and 6
    each alone, the lane weights their lobes' sums, the vertical pass summing w x (lane - irradiance);
  * the lane CDF tables (SSS_CDF1 / SSS_CDF2) rebuilt here with erf from SSS_VAR / SSS_W / SSS_LANE_LOBES;
  * the true reach: no SSS_MAX_PX / whole-kernel cap, the apron from the workgroup's widest reach;
  * the bins: fixed-point prefix sums (s_sssQ, SSS_QUANT), >= 16 one-texel bins per side;
  * depth barriers (SSS_TILT_MAX on the eye-distance step, the nearest-barrier search) and the non-skin rule.
  3. On 6dc44d6f33 the owner read the red as spreading "a mm or two". Not reproduced: the offline replica of
     both passes (scripts/check/test_skin_sss_replica.py, run by hand) matches the profile's analytic edge
     response in world mm at 0.05 - 1 m, tilted to 70 deg, with strands and at both ends of a run; the owner's
     screenshot fits the full scale-4 profile (R and G) at the shader's own px/mm. The objective in-world
     check: RenderVKDebugView 34 (the kernel's reach in px, its step ratio and where barriers fire) and 35
     (the world mm grid over the scattered image), kept wired here.
"""
import math
import re

NAME = "skin-sss-separable"
STAGE = "source"
DESCRIPTION = ("RenderVKSkinSSS pass 17 stays separable per profile lane (H + V passes, freed intermediate), "
               "deterministic, true-reach (no whole-kernel cap) and depth-barriered; its lane tables match the profile")
GUARDS = ("The skin scattering regressions the owner reported at RenderVKSkinSSSScale 4: the static red grain of "
          "random / rotated taps; a single 1D kernel for the whole six-Gaussian profile (thin red at diagonal "
          "edges and corners); the reach capped by shrinking the whole kernel up close; sparse taps that "
          "staircase a shadow edge; follow-surface weights that cut the spread on curved skin; lane tables "
          "drifting from the measured d'Eon / Luebke profile.")

SHADER = "indra/vkclean/shaders/clean_deferred_light.comp"
HOST = "indra/newview/vkgfx/llcleanrenderervk.cpp"
HEADER = "indra/newview/vkgfx/llcleanrenderervk.h"
REGEN = "indra/vkclean/regen_world_spv.py"
SETTINGS = "indra/newview/app_settings/settings.xml"


def _read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def _strip_comments(src):
    return re.sub(r"//[^\n]*", "", src)


def _floats(text):
    return [float(v) for v in re.findall(r"-?\d+\.\d+(?:e-?\d+)?", text)]


def _method(src, signature):
    at = src.find(signature)
    if at < 0:
        return ""
    pos = src.find("{", at)
    depth, end = 1, pos + 1
    while depth and end < len(src):
        depth += {"{": 1, "}": -1}.get(src[end], 0)
        end += 1
    return src[at:end]


def _array(code, name):
    m = re.search(r"const\s+\w+\s+" + name + r"\[\d+\]\s*=\s*\w+\[\d+\]\((.*?)\);", code, re.S)
    return m.group(1) if m else None


def _vec3(code, name):
    m = re.search(r"const\s+vec3\s+" + name + r"\s*=\s*vec3\(([^)]*)\);", code)
    return _floats(m.group(1)) if m else None


# Lane order (SSS_LANE_LOBES): SSS_CDF1 / SSS_LANE_W1 = (A.r, A.g, A.b), SSS_CDF2 / SSS_LANE_W2 = (B.r, C.r, B.g).
LANE_CHANNEL = (0, 1, 2, 0, 0, 1)


def _check_lanes(code, problems):
    var_s, w_s, lobes_s = _array(code, "SSS_VAR"), _array(code, "SSS_W"), _array(code, "SSS_LANE_LOBES")
    c1_s, c2_s = _array(code, "SSS_CDF1"), _array(code, "SSS_CDF2")
    lw1, lw2 = _vec3(code, "SSS_LANE_W1"), _vec3(code, "SSS_LANE_W2")
    n_m = re.search(r"#define SSS_CDF_N (\d+)", code)
    radius = re.search(r"#define SSS_RADIUS_MM ([\d.]+)", code)
    if None in (var_s, w_s, lobes_s, c1_s, c2_s, lw1, lw2) or not n_m or not radius:
        problems.append("pass 17 lost its profile / lane tables (SSS_VAR, SSS_W, SSS_LANE_LOBES, SSS_LANE_W1 / W2, "
                        "SSS_CDF1 / SSS_CDF2, SSS_CDF_N, SSS_RADIUS_MM)")
        return
    var, wflat = _floats(var_s), _floats(w_s)
    lobes = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]+)u", lobes_s)]
    n, r = int(n_m.group(1)), float(radius.group(1))
    c1, c2 = _floats(c1_s), _floats(c2_s)
    if len(var) != 6 or len(wflat) != 18 or len(lobes) != 6 or len(lw1) != 3 or len(lw2) != 3 \
            or len(c1) != 3 * (n + 1) or len(c2) != 3 * (n + 1):
        problems.append("pass 17 profile / lane tables have the wrong sizes (6 Gaussians, 6 lanes, %d + 1 CDF nodes)" % n)
        return
    w = [wflat[3 * k:3 * k + 3] for k in range(6)]
    for ch in range(3):
        if abs(sum(w[k][ch] for k in range(6)) - 1.0) > 2e-3:
            problems.append("SSS_W channel %d no longer sums to 1 (the profile must conserve energy)" % ch)
    # Every lobe with weight in a channel sits in exactly one of that channel's lanes.
    for ch in range(3):
        for k in range(6):
            if w[k][ch] <= 0.0:
                continue
            owners = [l for l in range(6) if LANE_CHANNEL[l] == ch and lobes[l] >> k & 1]
            if len(owners) != 1:
                problems.append("SSS_LANE_LOBES: Gaussian %d (channel %d) is in %d lanes, must be in exactly one"
                                % (k + 1, ch, len(owners)))
    # The corner / diagonal fix: red's two widest lobes each convolved on their own.
    red = [lobes[l] for l in range(6) if LANE_CHANNEL[l] == 0]
    if 0x10 not in red or 0x20 not in red:
        problems.append("SSS_LANE_LOBES: red lobes 5 and 6 are no longer lanes of their own -- a single 1D kernel "
                        "over narrow + wide lobes is plus-shaped (thin red at diagonal edges and corners)")
    lane_w = [sum(w[k][LANE_CHANNEL[l]] for k in range(6) if lobes[l] >> k & 1) for l in range(6)]
    for l, got in enumerate(lw1 + lw2):
        if abs(got - lane_w[l]) > 1e-3:
            problems.append("SSS_LANE_W%d[%d] = %g, its lobes' SSS_W sum is %.4f" % (1 + l // 3, l % 3, got, lane_w[l]))
    # SSS_CDF1 / 2 [j] = each lane's normalised one-sided CDF at SSS_RADIUS_MM (j / n)^3; the last node 1/2.
    for t, (table, lanes) in enumerate(((c1, (0, 1, 2)), (c2, (3, 4, 5)))):
        for j in range(n + 1):
            x = r * (j / n) ** 3
            for i, l in enumerate(lanes):
                ch = LANE_CHANNEL[l]
                want = 0.5 if j == n else sum(w[k][ch] * 0.5 * math.erf(x / math.sqrt(2.0 * var[k]))
                                              for k in range(6) if lobes[l] >> k & 1) / lane_w[l]
                got = table[3 * j + i]
                if abs(got - want) > 2e-6:
                    problems.append("SSS_CDF%d[%d] lane %d = %g, the lane's CDF at %.4f mm is %.6f"
                                    % (t + 1, j, i, got, x, want))
                    return


def run(ctx, result):
    problems = []
    shader = _read(ctx, SHADER)
    start = shader.find("// ---- SKIN SUBSURFACE SCATTERING (17;")
    end = shader.find("#elif CLEAN_DEFER_PASS == 7", start)
    if start < 0 or end < 0:
        result.fail("%s: the pass-17 SKIN SUBSURFACE SCATTERING block is gone" % SHADER)
        result.summary = "pass 17 block missing"
        return
    code = _strip_comments(shader[start:end])

    # 1. Deterministic: no random, rotated or hashed taps; no frame-varying input.
    for pat, why in ((r"\bfract\s*\(", "a fract() hash"), (r"\b(?:cos|sin)\s*\(", "rotated (cos / sin) taps"),
                     (r"\b(?:rand|random|hash|noise|blueNoise|ign)\w*\s*\(", "a random / noise function"),
                     # (u_cam_time.xyz, the eye, anchors the ruler view's world grid; .w is the time)
                     (r"\bframe\w*\b|u_cam_time(?!\.xyz\b)|g_time", "a frame-varying input"),
                     (r"52\.9829189", "the interleaved-gradient rotation")):
        if re.search(pat, code, re.I if "rand" in pat else 0):
            problems.append("%s: pass 17 uses %s -- the scattering must be deterministic" % (SHADER, why))

    # 2. Separable: a horizontal and a vertical variant, each a 1D run along its axis.
    if "#ifdef CLEAN_SSS_VERTICAL" not in code or "SSS_AXIS = ivec2(0, 1)" not in code \
            or "SSS_AXIS = ivec2(1, 0)" not in code:
        problems.append("%s: pass 17 is no longer split into horizontal / vertical (CLEAN_SSS_VERTICAL) passes" % SHADER)
    if not re.search(r"imageStore\(uSssTmp", code) or not re.search(r"imageLoad\(uSssTmp", code):
        problems.append("%s: the horizontal pass no longer hands its result to the vertical one through uSssTmp" % SHADER)
    # (#1608: a layer per view -- uimage2DArray, the view's layer; vr_stereo_parity.py pins the stereo side)
    if "binding = 32, r32ui) uniform readonly uimage2DArray uSssTmp" not in shader \
            or "binding = 32, r32ui) uniform writeonly uimage2DArray uSssTmp" not in shader:
        problems.append("%s: uSssTmp (set 3 binding 32, R32UI) is not declared for both passes" % SHADER)
    if "ivec3(2 * q.x + w, q.y, DEFER_LAYER)" not in code:
        problems.append("%s: uSssTmp no longer holds two words per pixel side by side (sssTmpAt)" % SHADER)
    regen = _read(ctx, REGEN)
    if not re.search(r'"kWorldDeferSkinSssSpv",\s*\["-DCLEAN_DEFER_PASS=17"\]', regen) \
            or not re.search(r'"kWorldDeferSkinSssVSpv",\s*\["-DCLEAN_DEFER_PASS=17", "-DCLEAN_SSS_VERTICAL"\]', regen):
        problems.append("%s: the two pass-17 variants (kWorldDeferSkinSssSpv / kWorldDeferSkinSssVSpv) are not both built" % REGEN)
    host = _read(ctx, HOST)
    rec = _method(host, "void LLCleanRendererVK::recordSkinSss(")
    if rec.count("vkCmdDispatch(") != 2 or "mSkinSssPipe[0]" not in rec or "mSkinSssPipe[1]" not in rec:
        problems.append("%s: recordSkinSss no longer records the horizontal and the vertical pass" % HOST)
    else:
        h, v = rec.find("mSkinSssPipe[0]"), rec.find("mSkinSssPipe[1]")
        if not (h < rec.find("vkCmdPipelineBarrier2", h) < v):
            problems.append("%s: recordSkinSss has no barrier between the horizontal and vertical passes" % HOST)
    if "!mDsSssTmpFull" not in rec:
        problems.append("%s: recordSkinSss runs without the separable intermediate at full size" % HOST)
    seg = re.search(r"#define SSS_SEG\s+(\d+)", shader)
    kseg = re.search(r"constexpr uint32_t kSeg = (\d+)u;", rec)
    if not seg or not kseg or seg.group(1) != kseg.group(1) or "local_size_x = SSS_SEG) in;" not in shader:
        problems.append("%s: recordSkinSss's run length (kSeg) no longer matches the shader's SSS_SEG (one run per "
                        "workgroup)" % HOST)
    if not re.search(r"ensureDeferSplitOpt\(mDsSssTmp, mDsSssTmpFull, mDsSssWant, mDsSssRetryAt, 2u \* rw, rh,", host):
        problems.append("%s: mDsSssTmp is no longer allocated at twice the width (two lane words per pixel)" % HOST)

    # 3. The intermediate lives only while the scattering does (no unbounded growth, free on disable).
    destroy = _method(host, "void LLCleanRendererVK::destroyDeferSplitTargets()")
    change = _method(host, "bool LLCleanRendererVK::deferSplitSssChange() const")
    if "mCtx->destroyImage(mDsSssTmp)" not in destroy:
        problems.append("%s: destroyDeferSplitTargets no longer frees mDsSssTmp" % HOST)
    if "deferSplitOptChange(mDsSssTmp" not in change:
        problems.append("%s: mDsSssTmp left out of the liveness / re-creation test (deferSplitSssChange)" % HOST)
    if "kDsBindingSssTmp == 32u" not in _read(ctx, HEADER):
        problems.append("%s: kDsBindingSssTmp is no longer set 3 binding 32" % HEADER)

    # 4. The true reach: no whole-kernel cap; the apron follows the workgroup's widest reach.
    if re.search(r"SSS_MAX_PX|SSS_MAX_PX\s*/\s*SSS_RADIUS_MM", code):
        problems.append("%s: pass 17 caps the reach again (SSS_MAX_PX): the whole kernel shrinks up close" % SHADER)
    if "atomicMax(s_sssReach" not in code or "uintBitsToFloat(s_sssReach)" not in code:
        problems.append("%s: the staged apron no longer follows the workgroup's widest reach (s_sssReach)" % SHADER)
    blk = re.search(r"#define SSS_BLOCK_MAX (\d+)", code)
    if not blk or int(blk.group(1)) < 8:
        problems.append("%s: SSS_BLOCK_MAX < 8: the reach that fits without narrowing the kernel shrank" % SHADER)

    # 5. Bins as exact integrals over fixed-point prefix sums; every texel near the centre its own bin.
    if "shared ivec4 s_sssQ[SSS_ENT_MAX + 1];" not in code or not re.search(r"#define SSS_QUANT [\d.]+", code):
        problems.append("%s: the staged run is no longer fixed-point prefix sums (s_sssQ, SSS_QUANT): wide bins "
                        "would sample (a staircase) or lose the dark to float cancellation" % SHADER)
    dense = re.search(r"#define SSS_DENSE_PX (\d+)", code)
    if not dense or int(dense.group(1)) < 16 or "for (int i = 1; i <= nb; ++i)" not in code:
        problems.append("%s: pass 17 no longer gives every texel within SSS_DENSE_PX (>= 16) px its own bin" % SHADER)

    # 6. Depth barriers (not tangent-plane weights); nothing outside the skin bleeds in.
    if "SSS_FOLLOW_MM" in code:
        problems.append("%s: the tangent-plane follow-surface weights are back (they cut the spread on curved skin)" % SHADER)
    if not re.search(r"abs\(d - dg\) > SSS_TILT_MAX \* theta", code) or "SSS_BARS(mid + 1) > f0" not in code:
        problems.append("%s: the depth barriers (eye-distance jumps, nearest-barrier search) are gone" % SHADER)
    gap = re.search(r"#define SSS_GAP (\d+)", code)
    if not gap or int(gap.group(1)) < 32:
        problems.append("%s: SSS_GAP < 32: a hair strand or lash line across the skin would wall the scattering "
                        "off on both sides" % SHADER)
    if "if (!sssIsSkin(q)) return false;" not in code or "gbufferIsSkin" not in code:
        problems.append("%s: a non-skin texel no longer contributes the centre (hair / background bleed)" % SHADER)

    # 7. Exact no-op on unchanged skin: the horizontal pass packs at the centre's exponent, the vertical
    # pass sums w x (lane - irradiance).
    if "sssPack(max(Sc1 + acc1, vec3(0.0)), eMin)" not in code \
            or not re.search(r"SSS_LANE_W1\.r \* d1\.r \+ SSS_LANE_W2\.x \* d2\.x \+ SSS_LANE_W2\.y \* d2\.y", code):
        problems.append("%s: pass 17 lost its exact no-op (centre-exponent packing / sum of w x (lane - irradiance))" % SHADER)

    # 8. The lanes and their tables are the measured profile's.
    _check_lanes(code, problems)

    # 9. The in-world checks of the spread (owner report on 6dc44d6f33, "the red spreads a mm or two"):
    # RenderVKDebugView 31 - 35 -> pass-17 modes 1 - 5 (3 bits), 34 = the kernel's reach / barriers,
    # 35 = the world mm grid over the scattered image; every host site that treats 31 - 33 as SSS views
    # covers 34 / 35 too (else they read as a lit-pass shading term), and settings.xml documents both.
    if "(g_draw_push.flags >> 16u) & 7u" not in code or not re.search(r"#define SSS_DBG_REACH 4u", code) \
            or not re.search(r"#define SSS_DBG_RULER 5u", code) or "sssGrid(px)" not in code:
        problems.append("%s: pass 17 lost its reach (4) / ruler (5) debug modes or their 3-bit mode field" % SHADER)
    if "if (view >= 31u && view <= 35u) debug = view - 30u;" not in rec:
        problems.append("%s: recordSkinSss no longer maps RenderVKDebugView 31 - 35 to pass-17 modes 1 - 5" % HOST)
    if "on = s_sss || (view >= 31u && view <= 35u);" not in host \
            or "((U32)s_dbgview >= 31u && (U32)s_dbgview <= 35u)" not in host \
            or "(dbgview >= 31u && dbgview <= 35u)" not in host or re.search(r"(?:view|dbgview) <= 33u", host):
        problems.append("%s: a RenderVKDebugView site still ends the skin SSS views at 33 (34 / 35 would run as a "
                        "lit-pass shading term or without the skin export)" % HOST)
    settings = _read(ctx, SETTINGS)
    if "34 skin SSS reach" not in settings or "35 skin SSS ruler" not in settings:
        problems.append("%s: RenderVKDebugView no longer documents the skin SSS views 34 (reach) / 35 (ruler)" % SETTINGS)

    for p in problems:
        result.fail(p)
    result.summary = "separable skin SSS %s" % ("intact" if not problems else "BROKEN")
