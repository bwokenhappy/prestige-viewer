"""Offline replica of the skin subsurface scattering (clean_deferred_light.comp pass 17) -- both passes.

Owner report on 6dc44d6f33 (RenderVKSkinSSSScale 4): "the red spreads maybe a mm or two". This replica
mirrors the shader's arithmetic -- sssPos through a reversed-Z inverse VP (float32), per-thread theta / reach,
the workgroup apron / B / L / K staging, the jump flags, the SSS_GAP barrier lookback, the fixed-point prefix
sums (float32 floor(e * inv + 0.5)), the binary barrier search, the dense + tail bins with the shader's own CDF
tables (parsed from the shader), RGB9E5 packing at the centre's exponent between the passes and the lane
recombination -- on a synthetic step edge (lit / shadow) over a flat skin plane at a realistic close-up
(2560 x 1440, 60 deg vertical FOV; 1 m, 0.15 m, 0.05 m; tilted 0 - 70 deg; the edge at both ends of a
workgroup; a non-skin strand between the lit skin and the shadow). Result: the response matches the d'Eon /
Luebke profile's analytic edge response in world mm at the scale within 0.004 of the lit level face-on (0.006
tilted 45 - 70 deg: the perspective change of mm per px across the kernel), and a 1D reference convolution
within 0.005 including the strand and the run placements: the kernel spreads the full profile (red visible to
~12 mm at scale 4) at any distance. Not reproduced: a too-narrow spread. The owner's screenshot
(Screenshot_20261112_204544) fits the scale-4 profile, R and G jointly, at 12.75 px/mm along row 300 (rms
0.005): the fringe is the full profile in the shader's metric; view 35 checks that metric against the world.

  python3 scripts/check/test_skin_sss_replica.py            the cases above; exit 1 on a mismatch (~1 min)
  python3 scripts/check/test_skin_sss_replica.py --fit IMG  fit a screenshot's shadow-side red / green
        excess along a row to the scale-4 profile: the px per mm the rendered fringe implies (in the
        shader's own metric). Rows / columns: --fit IMG y x_edge x_end [y x_edge x_end ...]

In-world check: RenderVKDebugView 34 (the kernel's reach and barriers) and 35 (the world mm grid over the
scattered image) -- see the pass-17 header in the shader.
"""
import math
import os
import re
import sys
import numpy as np

F = np.float32
SHADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "indra", "vkclean", "shaders",
                      "clean_deferred_light.comp")


def parse(path):
    src = open(path).read()
    a = src.find("// ---- SKIN SUBSURFACE SCATTERING (17;")
    b = src.find("#elif CLEAN_DEFER_PASS == 7", a)
    code = re.sub(r"//[^\n]*", "", src[a:b])
    d = {}
    seg = re.search(r"#define SSS_SEG\s+(\d+)", src)
    if seg:
        d["SSS_SEG"] = float(seg.group(1))
    for m in re.finditer(r"#define (SSS_\w+)\s+([\d.]+)", code):
        d[m.group(1)] = float(m.group(2))

    def arr(name):
        m = re.search(r"const\s+\w+\s+" + name + r"\[\d+\]\s*=\s*\w+\[\d+\]\((.*?)\);", code, re.S)
        return [float(v) for v in re.findall(r"-?\d+\.\d+(?:e-?\d+)?", m.group(1))] if m else None

    def v3(name):
        m = re.search(r"const\s+vec3\s+" + name + r"\s*=\s*vec3\(([^)]*)\);", code)
        return [float(v) for v in re.findall(r"-?\d+\.\d+", m.group(1))]
    d["VAR"] = arr("SSS_VAR")
    w = arr("SSS_W")
    d["W"] = [w[3 * k:3 * k + 3] for k in range(6)]
    d["CDF1"] = np.array(arr("SSS_CDF1"), dtype=F).reshape(-1, 3)
    d["CDF2"] = np.array(arr("SSS_CDF2"), dtype=F).reshape(-1, 3)
    d["LW1"] = v3("SSS_LANE_W1")
    d["LW2"] = v3("SSS_LANE_W2")
    d["code"] = code
    return d


C = parse(SHADER)
SEG = int(C["SSS_SEG"]) if "SSS_SEG" in C else 256
DENSE = int(C["SSS_DENSE_PX"]); TAIL = int(C["SSS_TAIL"]); CDFN = int(C["SSS_CDF_N"])
RAD = C["SSS_RADIUS_MM"]; TILT = C["SSS_TILT_MAX"]; GAP = int(C["SSS_GAP"])
ENTMAX = int(C["SSS_ENT_MAX"]); BLOCKMAX = int(C["SSS_BLOCK_MAX"]); QUANT = C["SSS_QUANT"]


# ---------------- RGB9E5 ----------------
def f32bits_exp(x):
    return int((np.array([x], dtype=F).view(np.uint32)[0] >> 23) & 0xFF) - 127


def pack9e5(c, emin=0):
    c = [min(float(F(v)), 65408.0) if v > 0 else 0.0 for v in c]
    maxc = max(c)
    e2 = f32bits_exp(maxc) if maxc > 0 else -127
    es = max(max(-16, e2) + 16, int(emin))
    scale = 2.0 ** (es - 24)
    if math.floor(maxc / scale + 0.5) >= 512:
        es += 1; scale *= 2
    m = [min(int(math.floor(v / scale + 0.5)), 511) for v in c]
    return m[0] | (m[1] << 9) | (m[2] << 18) | (min(es, 31) << 27)


def unpack9e5(w):
    s = 2.0 ** (int(w >> 27) - 24)
    return np.array([(w & 511) * s, ((w >> 9) & 511) * s, ((w >> 18) & 511) * s], dtype=np.float64)


# ---------------- camera / scene ----------------
class Cam:
    def __init__(s, W=2560, H=1440, vfov=60.0, near=0.1, far=512.0):
        s.W, s.H = W, H
        t = math.tan(math.radians(vfov) / 2)
        a = W / H
        # reversed-Z, Vulkan NDC (y down): view space x right, y up, looking -z
        P = np.zeros((4, 4))
        P[0, 0] = 1 / (a * t); P[1, 1] = -1 / t
        P[2, 2] = near / (far - near); P[2, 3] = near * far / (far - near)
        P[3, 2] = -1.0
        s.P = P
        s.inv = np.linalg.inv(P).astype(F)

    def pos(s, qx, qy, d):
        """sssPos: eye-relative position of pixel (qx, qy) at NDC depth d (float32 arithmetic)."""
        qx = np.asarray(qx, dtype=F); qy = np.asarray(qy, dtype=F); d = np.asarray(d, dtype=F)
        nx = (qx + F(0.5)) / F(s.W) * F(2) - F(1)
        ny = (qy + F(0.5)) / F(s.H) * F(2) - F(1)
        v = np.stack([nx, ny, d * np.ones_like(nx), np.ones_like(nx)], -1).astype(F)
        h = v @ s.inv.T
        return (h[..., :3] / h[..., 3:4]).astype(F)

    def ray(s, qx, qy):
        p = s.pos(qx, qy, 0.5).astype(np.float64)
        return p / np.linalg.norm(p, axis=-1, keepdims=True)

    def depth_of(s, P):
        """Reversed-Z NDC depth of view-space point(s) P, stored as float32 (D32F)."""
        h = np.concatenate([P, np.ones(P.shape[:-1] + (1,))], -1) @ s.P.T
        return (h[..., 2] / h[..., 3]).astype(F)


class Plane:
    """A flat skin plane: point O (view space), unit normal N, in-plane 'down' axis U (the edge runs across it)."""
    def __init__(s, dist, tilt_deg):
        t = math.radians(tilt_deg)
        s.O = np.array([0.0, 0.0, -dist])
        s.N = np.array([0.0, math.sin(t), math.cos(t)])      # tilted about x: the vertical pass sees the slope
        s.U = np.array([0.0, math.cos(t), -math.sin(t)])     # in-plane, (mostly) screen-up

    def hit(s, ray):
        tt = np.dot(s.O, s.N) / (ray @ s.N)
        return ray * tt[..., None]


# ---------------- one pass over one line (rows or columns) ----------------
def cdf(x):
    x = np.asarray(x, dtype=F)
    t = (np.clip(x * F(1.0 / RAD), 0, 1) ** F(1.0 / 3.0)) * F(CDFN)
    j = np.minimum(t.astype(np.int32), CDFN - 1)
    f = (t - j)[..., None]
    c1 = C["CDF1"][j] * (1 - f) + C["CDF1"][j + 1] * f
    c2 = C["CDF2"][j] * (1 - f) + C["CDF2"][j + 1] * f
    return c1.astype(F), c2.astype(F)


def run_pass(cam, along_n, coord, skin_fn, depth_fn, val_fn, centre_word_fn, vertical, scale, wg_list, diag=None, only=None):
    """Emulate the workgroups in wg_list of one line. coord(along) -> (qx, qy) arrays.
    skin_fn / depth_fn / val_fn(along idx array) give skin mask, NDC depth, (v1, v2) per texel.
    Returns {along: (Sc1+acc1, Sc2+acc2, reach, limL_px, limR_px)} for skin pixels."""
    out = {}
    for g in wg_list:
        along0 = g * SEG
        lx = np.arange(SEG)
        pa = along0 + lx
        inside = pa < along_n
        skin = inside & skin_fn(np.clip(pa, 0, along_n - 1))
        qx, qy = coord(pa)
        qx1, qy1 = coord(pa + 1)
        # theta
        r0 = cam.pos(qx, qy, 0.5).astype(F); r1 = cam.pos(qx1, qy1, 0.5).astype(F)
        theta = np.linalg.norm(r0 / np.linalg.norm(r0, axis=-1, keepdims=True)
                               - r1 / np.linalg.norm(r1, axis=-1, keepdims=True), axis=-1).astype(F)
        # reach
        reach = np.zeros(SEG, dtype=F)
        d0 = depth_fn(np.clip(pa, 0, along_n - 1))
        P0 = cam.pos(qx, qy, d0)
        lat = np.linalg.norm(cam.pos(qx1, qy1, d0) - P0, axis=-1).astype(F)
        best = F(TILT) * lat; step = lat.copy()
        for sd in (-1, 1):
            qa = pa + sd
            ok = (qa >= 0) & (qa < along_n)
            ok &= skin_fn(np.clip(qa, 0, along_n - 1))
            qqx, qqy = coord(qa)
            l = np.linalg.norm(cam.pos(qqx, qqy, depth_fn(np.clip(qa, 0, along_n - 1))) - P0, axis=-1).astype(F)
            take = ok & (l <= best)
            best = np.where(take, l, best); step = np.where(take, np.maximum(l, lat), step)
        rr = F(RAD) / np.maximum(step * F(1000.0) / F(max(scale, 1e-6)), F(1e-9))
        rr = np.where(rr < 1e6, rr, 0)
        reach = np.where(skin, rr, 0).astype(F)
        if not skin.any():
            continue
        apronPx = int(math.ceil(float(reach.max()))) + 2
        B = 1
        while B < BLOCKMAX and SEG // B + 2 * ((apronPx + B - 1) // B) > ENTMAX:
            B *= 2
        apronE = min((apronPx + B - 1) // B, (ENTMAX - SEG // B) // 2)
        L = SEG // B + 2 * apronE
        K = (L + SEG - 1) // SEG
        base = along0 - apronE * B
        # staging
        ta = base + np.arange(L * B)
        tin = (ta >= 0) & (ta < along_n)
        tsk = tin & skin_fn(np.clip(ta, 0, along_n - 1))
        tqx, tqy = coord(ta)
        tdist = np.linalg.norm(cam.pos(tqx, tqy, depth_fn(np.clip(ta, 0, along_n - 1))), axis=-1).astype(F)
        tv1, tv2 = val_fn(np.clip(ta, 0, along_n - 1))
        tv1 = np.where(tsk[:, None], tv1, 0); tv2 = np.where(tsk[:, None], tv2, 0)
        e1 = tv1.reshape(L, B, 3).sum(1); e2 = tv2.reshape(L, B, 3).sum(1)
        en = tsk.reshape(L, B).sum(1)
        dsum = np.where(tsk, tdist, 0).reshape(L, B).sum(1)
        dmean = np.where(en > 0, dsum / np.maximum(en, 1), -1.0).astype(F)
        ent_owner = np.arange(L) // K
        th_e = theta[np.minimum(ent_owner, SEG - 1)]
        jump = np.zeros(L, bool)
        if B > 1:
            sk = tsk.reshape(L, B); dd = tdist.reshape(L, B)
            for e in range(L):
                dprev = -1.0
                for t in range(B):
                    if not sk[e, t]:
                        dprev = -1.0; continue
                    d = dd[e, t]
                    if dprev >= 0 and abs(d - dprev) > TILT * th_e[e] * min(d, dprev):
                        jump[e] = True
                    dprev = d
        vtop = float(max(e1.max(), e2.max()))
        inv = F(QUANT) / (F(L) * F(B) * F(vtop)) if vtop > 0 else F(0)
        bar = np.zeros(L, np.int64)
        for e in range(L):
            if en[e] <= 0:
                continue
            d = dmean[e]
            cut = jump[e] or (e > 0 and jump[e - 1])
            if not cut:
                found = False
                for gg in range(1, GAP + 1):
                    if e - gg < 0:
                        break
                    dg = dmean[e - gg]
                    if dg < 0:
                        continue
                    found = True
                    cut = abs(d - dg) > TILT * th_e[e] * float(gg * B) * min(d, dg)
                    break
                if not found and e > GAP:
                    cut = True
            bar[e] = 1 if cut else 0
        q1 = np.floor((e1 * inv).astype(F) + F(0.5)).astype(np.int64)
        q2 = np.floor((e2 * inv).astype(F) + F(0.5)).astype(np.int64)
        Q1 = np.zeros((L + 1, 4), np.int64); Q2 = np.zeros((L + 1, 4), np.int64)
        Q1[1:, :3] = np.cumsum(q1, 0); Q1[1:, 3] = np.cumsum(en)
        Q2[1:, :3] = np.cumsum(q2, 0) if vertical else 0
        BARS = np.zeros(L + 1, np.int64); BARS[1:] = np.cumsum(bar)
        assert np.abs(Q1).max() < 2 ** 31 and np.abs(Q2).max() < 2 ** 31
        for t in range(SEG):
            if not skin[t] or (only is not None and along0 + t != only):
                continue
            a = along0 + t
            Sc1, Sc2 = centre_word_fn(a)
            acc1 = np.zeros(3); acc2 = np.zeros(3)
            reachPx = min(float(reach[t]), float(apronE * B - 1))
            limL, limR = 0, L
            if reachPx >= 0.5 and inv > 0:
                mmPerPx = RAD / reachPx
                c1 = np.floor(Sc1 * float(inv) + 0.5).astype(np.int64)
                c2 = np.floor(Sc2 * float(inv) + 0.5).astype(np.int64)
                iB = 1.0 / B
                uc = (t + apronE * B + 0.5) * iB
                if BARS[L] != 0:
                    ce = min(int(uc), L - 1)
                    f0 = BARS[ce + 1]
                    if BARS[L] != f0:
                        lo, hi = ce + 1, L - 1
                        while lo < hi:
                            mid = (lo + hi) >> 1
                            if BARS[mid + 1] > f0: hi = mid
                            else: lo = mid + 1
                        limR = lo
                    if f0 != 0:
                        lo, hi = 0, ce
                        while lo < hi:
                            mid = (lo + hi + 1) >> 1
                            if BARS[mid] < f0: lo = mid
                            else: hi = mid - 1
                        limL = lo
                uL, uR = float(limL), float(limR)

                def pt(u):
                    i = min(int(u), L - 1)
                    f = u - i
                    b1 = Q1[i, :3] - c1 * Q1[i, 3]
                    b2 = (Q2[i, :3] - c2 * Q1[i, 3]) if vertical else np.zeros(3, np.int64)
                    s1 = np.zeros(3, np.int64); s2 = np.zeros(3, np.int64)
                    if f > 0:
                        s1 = (Q1[i + 1, :3] - Q1[i, :3]) - c1 * (Q1[i + 1, 3] - Q1[i, 3])
                        if vertical:
                            s2 = (Q2[i + 1, :3] - Q2[i, :3]) - c2 * (Q1[i + 1, 3] - Q1[i, 3])
                    return (b1, s1, b2, s2, f)

                def span(p, q):
                    v1 = (q[0] - p[0]) + q[4] * q[1] - p[4] * p[1]
                    if vertical:
                        v2 = (q[2] - p[2]) + q[4] * q[3] - p[4] * p[3]
                    else:
                        v2 = v1[[0, 0, 1]]
                    return v1.astype(np.float64), v2.astype(np.float64)
                cl = lambda u: min(max(u, uL), uR)
                pR = pt(cl(uc + 0.5 * iB)); pL = pt(cl(uc - 0.5 * iB))
                lo1, lo2 = cdf(0.5 * mmPerPx)
                dense = min(int(math.ceil(reachPx - 0.5)), DENSE)
                nb = dense + (TAIL if reachPx > dense + 0.5 else 0)
                a0 = dense + 0.5
                r0 = 0.5
                for i in range(1, nb + 1):
                    tt = (i - dense) / TAIL
                    r = i + 0.5 if i <= dense else a0 + (reachPx - a0) * tt * tt
                    hi1 = np.full(3, 0.5); hi2 = np.full(3, 0.5)
                    if i < nb:
                        hi1, hi2 = cdf(r * mmPerPx)
                    nR = pt(cl(uc + r * iB)); nL = pt(cl(uc - r * iB))
                    sR1, sR2 = span(pR, nR); sL1, sL2 = span(nL, pL)
                    iw = 1.0 / (r - r0)
                    acc1 += (hi1 - lo1) * iw * (sR1 + sL1)
                    acc2 += (hi2 - lo2) * iw * (sR2 + sL2)
                    lo1, lo2, pR, pL, r0 = hi1, hi2, nR, nL, r
                acc1 /= float(inv); acc2 /= float(inv)
            out[a] = (Sc1 + acc1, Sc2 + acc2, reachPx, (uc - limL) * B if BARS[L] else None,
                      (limR - uc) * B if BARS[L] else None, int(BARS[L]), B)
        if diag is not None:
            diag.append(dict(g=g, B=B, L=L, apronE=apronE, maxreach=float(reach.max()), bars=int(BARS[L])))
    return out


def analytic(xw_mm, scale, ch):
    xp = xw_mm / scale
    return sum(C["W"][k][ch] * 0.5 * math.erfc(xp / math.sqrt(2 * C["VAR"][k])) for k in range(6))


def experiment(dist, tilt, scale=4.0, lit=1.0, shadow=0.02, W=2560, H=1440, vfov=60.0, near=0.1, col=None, verbose=True,
               edge_row=None, strand=0):
    cam = Cam(W, H, vfov, near)
    pl = Plane(dist, tilt)
    col = W // 2 if col is None else col
    ys = np.arange(H)
    # per-pixel surface hits for the whole image band we need (rows all, columns around col for H pass)
    def hitpos(qx, qy):
        return pl.hit(cam.ray(qx, qy))

    def depth_px(qx, qy):
        return cam.depth_of(hitpos(qx, qy))

    s_edge = 0.0
    if edge_row is not None:   # the edge between rows edge_row - 1 (lit) and edge_row (shadow)
        s_edge = float(((hitpos(np.array([col]), np.array([edge_row])) - pl.O) @ pl.U)[0]) + 1e-7
    nonskin = np.zeros(H, bool)
    if strand:
        er = edge_row if edge_row is not None else None
        if er is None:
            ss = (hitpos(np.full(H, col), ys) - pl.O) @ pl.U
            er = int(np.argmax(ss < 0))
        nonskin[max(er - strand, 0):er] = True

    def irr_px(qx, qy):
        P = hitpos(qx, qy)
        s = (P - pl.O) @ pl.U - s_edge  # in-plane coordinate (m); s > 0 lit
        # RGB9E5 storage of the irradiance (uSssDiff): two values only
        wl, ws = pack9e5([lit] * 3), pack9e5([shadow] * 3)
        words = np.where(s >= 0, wl, ws)
        v = np.where((s >= 0)[..., None], unpack9e5(wl), unpack9e5(ws))
        return v, words, s
    # ---- HORIZONTAL pass: for each row, the workgroup containing col ----
    gH = col // SEG
    tmp1 = np.zeros((H, 3)); tmp2 = np.zeros((H, 3))
    xs_all = np.arange(W)
    for y in range(H):
        irrrow, wrow, _ = irr_px(xs_all, np.full(W, y))
        drow = depth_px(xs_all, np.full(W, y))
        if nonskin[y]:
            continue
        coord = lambda a, y=y: (np.asarray(a), np.full(np.shape(a), y))
        res = run_pass(cam, W, coord, lambda a: np.ones(np.shape(a), bool), lambda a, d=drow: d[a],
                       lambda a, ir=irrrow: (ir[a], np.zeros(np.shape(a) + (3,))),
                       lambda a, ir=irrrow: (ir[a], ir[a][[0, 0, 1]]), False, scale, [gH], only=col)
        S1, S2 = res[col][0], res[col][1]
        eMin = int(wrow[col]) >> 27
        tmp1[y] = unpack9e5(pack9e5(np.maximum(S1, 0), eMin))
        tmp2[y] = unpack9e5(pack9e5(np.maximum(S2, 0), eMin))
    # ---- VERTICAL pass along column col ----
    S0col, _, scol = irr_px(np.full(H, col), ys)
    dcol = depth_px(np.full(H, col), ys)
    coord = lambda a: (np.full(np.shape(a), col), np.asarray(a))
    diag = []
    res = run_pass(cam, H, coord, lambda a: ~nonskin[a], lambda a: dcol[a],
                   lambda a: (tmp1[a], tmp2[a]), lambda a: (tmp1[a], tmp2[a]), True, scale,
                   range((H + SEG - 1) // SEG), diag)
    LW1, LW2 = C["LW1"], C["LW2"]
    final = np.zeros((H, 3)); reach = np.zeros(H); limL = [None] * H; limR = [None] * H; nbar = np.zeros(H)
    for y in range(H):
        if nonskin[y]:
            final[y] = S0col[y]; continue
        S1, S2, rp, lL, lR, nb, B = res[y]
        d1 = S1 - S0col[y]; d2 = S2 - S0col[y][[0, 0, 1]]
        delta = np.array([LW1[0] * d1[0] + LW2[0] * d2[0] + LW2[1] * d2[1], LW1[1] * d1[1] + LW2[2] * d2[2], d1[2]])
        final[y] = S0col[y] + delta; reach[y] = rp; limL[y] = lL; limR[y] = lR; nbar[y] = nb
    # 1D reference in the centre's px metric (exact lane CDFs): every skin row y' != y, non-skin = centre
    lanes_ch = (0, 1, 2, 0, 0, 1); lobes = (0x0F, 0x03, 0x3F, 0x10, 0x20, 0x3C)
    lw = list(C["LW1"]) + list(C["LW2"])

    def lane_cdf(l, x):
        ch = lanes_ch[l]
        return sum(C["W"][k][ch] * 0.5 * math.erf(x / math.sqrt(2 * C["VAR"][k])) for k in range(6) if lobes[l] >> k & 1) / lw[l]
    ref = np.zeros((H, 3))
    for y in range(H):
        if nonskin[y] or reach[y] < 0.5:
            ref[y] = S0col[y]; continue
        mmpp = RAD / reach[y]
        lo = max(0, y - int(reach[y] * 1.6) - 2); hi = min(H, y + int(reach[y] * 1.6) + 3)
        dl = np.zeros(6)
        for yy in range(lo, hi):
            if yy == y or nonskin[yy]:
                continue
            a = abs(yy - y)
            v = S0col[yy][0] - S0col[y][0]
            if v == 0:
                continue
            for l in range(6):
                dl[l] += (lane_cdf(l, (a + 0.5) * mmpp) - lane_cdf(l, (a - 0.5) * mmpp)) * v
        ref[y] = S0col[y] + np.array([lw[0] * dl[0] + lw[3] * dl[3] + lw[4] * dl[4], lw[1] * dl[1] + lw[5] * dl[5], dl[2]])
    err = np.abs(final - ref)[:, :2].max() / (lit - shadow)
    if verbose:
        print("  max |replica - 1D reference| (R, G; fraction of the step) = %.4f at row %d"
              % (err, int(np.argmax(np.abs(final - ref)[:, :2].max(1)))))
    # response on the shadow side vs world distance from the edge
    frac = (final - shadow) / (lit - shadow)
    rows = []
    for xmm in (0.5, 1, 2, 4, 8, 12):
        sh = np.where(scol < 0)[0]
        sh = sh[~nonskin[sh]]
        y = sh[np.argmin(np.abs(-scol[sh] * 1000 - xmm))]
        rows.append((xmm, -scol[y] * 1000, y, frac[y, 0], analytic(-scol[y] * 1000, scale, 0),
                     frac[y, 1], analytic(-scol[y] * 1000, scale, 1)))
    if verbose:
        ye = np.argmin(np.abs(scol))
        print("dist %.3f m tilt %d deg: mm/px at edge %.4f, reach at edge %.1f px, workgroups %s"
              % (dist, tilt, abs(scol[ye] - scol[ye + 1]) * 1000, reach[ye], diag))
        print("  barriers per workgroup:", [d["bars"] for d in diag])
        print("   mm(target)  mm(px)   row    R got   R want   G got   G want")
        for r in rows:
            print("   %6.1f    %6.2f  %5d   %.4f  %.4f   %.4f  %.4f" % r)
    return rows, err, diag


def fit_screenshot(path, cuts):
    """The px / mm at which a screenshot's shadow-side excess (sRGB -> linear, the shadow floor and the lit
    level from the row itself) best matches the scale-4 profile's R and G edge response."""
    from PIL import Image
    c8 = np.asarray(Image.open(path).convert("RGB")).astype(float) / 255.0
    im = np.where(c8 <= 0.04045, c8 / 12.92, ((c8 + 0.055) / 1.055) ** 2.4)
    for (y, x0, x1) in cuts:
        seg = im[y, x0:x1]
        floor = seg[-20:].mean(0)
        lit = im[y, x0 - 150:x0 - 120].mean(0)
        best = None
        for ppm in np.arange(1.0, 120.0, 0.25):
            for off in np.arange(-15, 30, 1.0):
                err = 0.0
                for i in range(0, x1 - x0, 2):
                    xmm = (i + off) / ppm
                    if xmm < 0.3:
                        continue
                    for ch in (0, 1):
                        err += ((seg[i, ch] - floor[ch]) / (lit[ch] - floor[ch]) - analytic(xmm, 4.0, ch)) ** 2
                if best is None or err < best[0]:
                    best = (err, ppm, off)
        err, ppm, off = best
        print("row %d from x %d: the fringe is the scale-4 profile at %.2f px/mm along the row, edge %+.0f px "
              "(rms %.4f)" % (y, x0, ppm, off, math.sqrt(err / (x1 - x0))))
        for i in range(0, min(240, x1 - x0), 12):
            xmm = (i + off) / ppm
            print("   +%3d px  %5.2f mm   R %.3f (profile %.3f)   G %.3f (profile %.3f)" % (
                i, xmm, (seg[i, 0] - floor[0]) / (lit[0] - floor[0]), analytic(max(xmm, 0), 4.0, 0),
                (seg[i, 1] - floor[1]) / (lit[1] - floor[1]), analytic(max(xmm, 0), 4.0, 1)))


def main(argv):
    if len(argv) > 1 and argv[1] == "--fit":
        vals = [int(v) for v in argv[3:]] or [300, 386, 700]
        fit_screenshot(argv[2], [tuple(vals[i:i + 3]) for i in range(0, len(vals) - 2, 3)])
        return 0
    bad = 0
    # (dist m, tilt deg, edge row, strand px, analytic tolerance or None, reference tolerance)
    cases = ((1.0, 0, None, 0, 0.002, 0.005), (0.15, 0, None, 0, 0.002, 0.005), (0.05, 0, None, 0, 0.004, 0.006),
             (0.15, 45, None, 0, 0.006, 0.005), (0.15, 70, None, 0, 0.008, 0.005),
             (0.15, 0, 768, 0, None, 0.005), (0.15, 0, 767, 0, None, 0.005), (0.15, 0, None, 35, None, 0.005),
             (0.05, 0, 768, 35, None, 0.006))
    for dist, tilt, er, strand, tol_a, tol_r in cases:
        rows, err, diag = experiment(dist, tilt, edge_row=er, strand=strand)
        worst = max(max(abs(r[3] - r[4]), abs(r[5] - r[6])) for r in rows)
        ok = err <= tol_r and (tol_a is None or worst <= tol_a)
        bad += 0 if ok else 1
        print("%s: %.2f m, %d deg, edge row %s, strand %d px: analytic %.4f, reference %.4f\n"
              % ("ok" if ok else "FAIL", dist, tilt, er, strand, worst, err))
    print("skin SSS replica: %s" % ("PASS" if not bad else "%d case(s) FAILED" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
