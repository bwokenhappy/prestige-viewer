"""Regression guard for #1606 (the sparse RT reflection lattice) and #1570 (RT reflections on wavy water).

#1606: a rough receiver casts reflection rays at a density 1 / L^2 (L = 2 .. 16; a rough wall at the minimum density
casts one ray per 8 x 8 pixels). The rays sit on a lattice chosen by an ordered-dither rank (rtrRank, pass 2 / 5 / 11 /
16) and the resolve (pass 13) rebuilds every other pixel from the lattice around it. With the rank a FIXED screen
pattern the same pixel of every cell cast the ray every frame, so the converged image itself held the lattice:
8 px squares and a dot on each lattice pixel that no history averages away. The LATTICE ROTATION moves the lattice by
a per-frame phase (rtrLatticePhase), finest bits first, so every pixel is a lattice site once per L^2 frames.

This check takes rtrRank, rtrLatticePhase and rtrCellRank from the production source (a small GLSL -> Python
translation of their integer expressions) and simulates, for L = 2, 4, 8, 16:
  * every frame: exactly one ray per aligned L x L cell (the ray count, so the cost, is unchanged), and the cell's
    lowest rank (rtrCellRank, which a cell's best candidate takes when the lattice pixel is not a candidate) is the
    rank of that ray's pixel;
  * the resolve's lattice (pass 13: lbase = ((px + phase) / L) * L - phase, from the source) is exactly the selected
    pixels -- selection and resolve agree on where the rays are;
  * over L^2 consecutive frames every pixel of a cell is the lattice site exactly once (the rotation covers the cell);
  * phase 0 (the setting off) is the old fixed lattice.
The fixed-lattice source (no rtrLatticePhase) fails.

#1606 (2) EMPTY CELLS: a candidate whose resolve gather accepted no traced sample kept the bare env and CLEARED its
history; under GI that env is the specular occlusion's (near black in a closed room), so whole 8 px cells flipped
between dark and lit from frame to frame. The check requires the resolve to hold the reprojected history there
(rtrHistPeek, behind rtt_opts4 bit 4) BEFORE the history-clearing fallback -- the pure history (no env mixed in: a
held env share would escape the GI specular occlusion frame after frame) for at most RTR_HOLD_MAX frames.

#1606 (3) DLSS RR GUIDE: the hit-distance guide (clean_rr_guides.comp) used only the traced pixels in each 3x3, so on
a sparse lattice it was real distances in dots around the rays and 65504 elsewhere -- a pattern that moves with a
rotating lattice. Pass 13 now leaves every untraced candidate its gather's hit distance as a = -(T0 + t) (still
negative: every "traced?" test unchanged) and the guide decodes it. The check simulates encode / decode from the
sources (round trip, the -1 "nothing" value never decoded as a distance, a miss -> the far value) and requires the
host to rotate under RR too.

#1570: pass 14 (RT forward) wrote a water texel only when its reflection ray HIT something; a miss left the water FS
its own probe / sky, so hits and misses of neighbouring wave normals switched between two different images at the
half-resolution grid (hard-edged blocks of the far shore over the sky). The check requires the water receiver's miss
to write a sample (the env along the same R) and the hit's fade to it near the reflection length (RFT_FAR_FADE),
so the resolve blends the boundary. The old source (outv written under t > 0.0 alone) fails.
"""
import re

NAME = "rt-refl-lattice"
STAGE = "source"
DESCRIPTION = "RT reflections: the sparse ray lattice rotates per frame (selection = resolve); water misses are samples"
GUARDS = ("#1606: square blocks / lattice dots in rough RT reflections at native resolution (a screen-fixed ray "
          "lattice); #1570: pixelated hit / miss blocks on wavy water with RT reflections on.")

LIGHT = "indra/vkclean/shaders/clean_deferred_light.comp"
GUIDES = "indra/vkclean/shaders/clean_rr_guides.comp"
HOST = "indra/newview/vkgfx/llcleanrenderervk.cpp"
FWD_H = "indra/vkclean/shaders/clean_rt_fwd_temporal.h"


def _strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def _block(src, start):
    i = src.find("{", start)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if depth == 0:
            return src[i + 1:j]
    return None


def _split_top(s, sep=","):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == sep and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return [x.strip() for x in out]


def _py_expr(e):
    """A GLSL integer expression -> Python (uint literals, ~ on 32 bits, uvec2(a, b) -> tuple, p.x -> p[0])."""
    e = re.sub(r"\b0x([0-9A-Fa-f]+)u?\b", lambda m: str(int(m.group(1), 16)), e)
    e = re.sub(r"\b(\d+)u\b", r"\1", e)
    e = re.sub(r"\.x\b", "[0]", e)
    e = re.sub(r"\.y\b", "[1]", e)
    e = re.sub(r"~\s*\(", "_inv(", e)
    if "~" in e:
        raise ValueError("unsupported ~ operand in %r" % e)
    e = re.sub(r"\buvec2\s*\(", "_uvec2(", e)
    return e


class _V(tuple):
    """uvec2 with GLSL's component-wise / scalar integer operators."""

    def _op(self, o, f):
        if isinstance(o, tuple):
            return _V((f(self[0], o[0]) & 0xFFFFFFFF, f(self[1], o[1]) & 0xFFFFFFFF))
        return _V((f(self[0], o) & 0xFFFFFFFF, f(self[1], o) & 0xFFFFFFFF))

    def __add__(self, o): return self._op(o, lambda a, b: a + b)
    __radd__ = __add__
    def __and__(self, o): return self._op(o, lambda a, b: a & b)
    def __or__(self, o): return self._op(o, lambda a, b: a | b)


def _uvec2(a, b=None):
    return _V((a, a if b is None else b))


def _inv(v):
    return 0xFFFFFFFF ^ v


def _function(src, ret, name, params):
    """Python function from a GLSL function whose body is 'const T a = ..., b = ...;' lines and one return."""
    m = re.search(r"\b%s\s+%s\s*\(([^)]*)\)" % (ret, name), src)
    if not m:
        return None
    body = _block(src, m.end())
    if body is None:
        return None
    lines = ["def %s(%s):" % (name, ", ".join(params))]
    for stmt in [" ".join(s.split()) for s in body.split(";") if s.strip()]:
        if stmt.startswith("return"):
            lines.append("    return " + _py_expr(stmt[len("return"):]))
            continue
        d = re.match(r"(?:const\s+)?(?:uint|uvec2)\s+(.*)$", stmt, re.S)
        if not d:
            raise ValueError("%s: unsupported statement %r" % (name, stmt))
        for decl in _split_top(d.group(1)):
            k, v = decl.split("=", 1)
            lines.append("    %s = %s" % (k.strip(), _py_expr(v.strip())))
    return "\n".join(lines)


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    light = _strip_comments(read(LIGHT))
    failures = []

    # ---- #1606: the lattice phase, the rank, the cell rank ----
    src = {}
    try:
        for ret, name, params in (("uint", "rtrRank", ["p"]), ("uvec2", "rtrLatticePhase", ["f"])):
            code = _function(light, ret, name, params)
            if code is None:
                failures.append("%s not found: the ray lattice is screen-fixed (no per-frame phase) -- the #1606 "
                                "squares / lattice dots in rough reflections" % name)
                continue
            src[name] = code
    except ValueError as e:
        failures.append("could not translate the lattice functions (%s): update this check" % e)
    cm = re.search(r"uint\s+rtrCellRank\s*\(\s*uvec2\s+c\s*,\s*uint\s+s\s*,\s*uvec2\s+off\s*\)\s*\{\s*return\s+rtrRank\s*\((.*?)\)\s*;\s*\}",
                   light, re.S)
    lo = re.search(r"uvec2\s+rtrLatticeOff\s*\(\s*uint\s+w\s*\)\s*\{\s*return\s+(.*?);\s*\}", light, re.S)
    if failures:
        for f in failures:
            result.fail(f)
        return
    if not cm or not lo or "rtrLatticePhase(rtt_param2.x" not in lo.group(1).replace(" ", ""):
        result.fail("rtrCellRank / rtrLatticeOff (the phase from the frame counter rtt_param2.x) not found")
        return
    # the selection ranks p + phase, the cells' lowest ranks come from rtrCellRank
    gb = re.search(r"void\s+rtrGradBuild\s*\(", light)
    gr = re.search(r"uint\s+rtrGradRank\s*\(", light)
    build = _block(light, gb.end()) if gb else ""
    grank = _block(light, gr.end()) if gr else ""
    if not re.search(r"rtrRank\s*\(\s*org\s*\+\s*uvec2\s*\(\s*lid\s*&\s*7u\s*,\s*lid\s*>>\s*3\s*\)\s*\+\s*rtrLatticeOff\s*\(\s*w\s*\)\s*\)",
                     build):
        failures.append("rtrGradBuild: the lane's rank is not rtrRank(pixel + rtrLatticeOff()): the selection "
                        "ignores the lattice phase")
    if len(re.findall(r"rtrCellRank\s*\(", grank)) != 3 or re.search(r"\brtrRank\s*\(", grank):
        failures.append("rtrGradRank: the 2 / 4 / 8 cells' lowest ranks must come from rtrCellRank (the phased "
                        "lattice pixel), not a fixed corner's rtrRank")
    # pass 13's lattice base
    # (the RT RESOLVE: the pass-13 section holding the resolve's DEFER_MAIN -- not the bindings' section)
    resolve = ""
    for p13 in re.finditer(r"#elif\s+CLEAN_DEFER_PASS\s*==\s*13[ \t]*\n", light):
        p13_end = re.search(r"#elif\s+CLEAN_DEFER_PASS\s*==", light[p13.end():])
        sec = light[p13.end():p13.end() + p13_end.start()] if p13_end else ""
        if "rtrLattice(" in sec and "DEFER_MAIN" in sec:
            resolve = sec
    if not re.search(r"const\s+ivec2\s+lph\s*=\s*ivec2\s*\(\s*rtrLatticeOff\s*\(\s*m\.w\s*\)\s*\)", resolve) \
            or not re.search(r"lbase\s*=\s*\(\s*\(\s*px\s*\+\s*lph\s*\)\s*/\s*L\s*\)\s*\*\s*L\s*-\s*lph\s*;", resolve) \
            or not re.search(r"cb\s*=\s*\(\s*\(\s*px\s*\+\s*lph\s*\)\s*/\s*Lc\s*\)\s*\*\s*Lc\s*-\s*lph\s*;", resolve):
        failures.append("pass 13: the resolve's lattice (lbase / the coarser cb) is not at the selection's phase "
                        "((px + phase) / L * L - phase): it gathers from pixels that cast no ray")
    if not re.search(r"const\s+uvec2\s+off\s*=\s*rtrLatticeOff\s*\(\s*w\s*\)", grank):
        failures.append("rtrGradRank: the cells' lowest ranks must use the candidate's own phase (rtrLatticeOff(w))")
    # only the stochastic glossy receivers rotate (mirror-ray receivers keep anti-lag: a moving lattice shimmered at
    # their reflected edges), at exactly the stochastic threshold, and only with the rough lobe on
    rot = re.search(r"bool\s+rtrLatticeRot\s*\(\s*uint\s+w\s*\)\s*\{(.*?)\}", light, re.S)
    rr = re.search(r"#define\s+RTR_ROT_ROUGH\s+([0-9.]+)", light)
    sr = re.search(r"#define\s+RTR_STOCH_ROUGH\s+([0-9.]+)", light)
    body = " ".join(rot.group(1).split()) if rot else ""
    if not rot or not rr or not sr or float(rr.group(1)) != float(sr.group(1)) \
            or "rtt_opts4 & 8u" not in body or "0x80000000u" not in body \
            or not re.search(r"rtrMarkRough16\s*\(\s*w\s*\)\s*\)\s*>=\s*RTR_ROT_ROUGH", body):
        failures.append("rtrLatticeRot: the rotation must be limited to the stochastic glossy receivers (mark "
                        "roughness >= RTR_ROT_ROUGH == RTR_STOCH_ROUGH, the rough lobe flag, rtt_opts4 bit 3) -- "
                        "the mirror-ray receivers' anti-lag shimmered at reflected edges under a moving lattice")

    env = {"_uvec2": _uvec2, "_V": _V, "_inv": _inv}
    try:
        for code in src.values():
            exec(code, env)
        rank, phase = env["rtrRank"], env["rtrLatticePhase"]
        cell_expr = _py_expr(cm.group(1))

        def cell_rank(c, s, off):
            return rank(eval(cell_expr, dict(env, c=_V(c), s=s, off=_V(off))))
    except Exception as e:  # noqa: BLE001 -- any translation error means the check covers nothing
        result.fail("lattice functions not evaluable (%s): update this check" % e)
        return

    W = 64
    for L in (2, 4, 8, 16):
        thr = 256 // (L * L)
        hits = {}
        for f in range(L * L):
            off = phase(f) if f else phase(0)
            off = tuple(off)
            sel = {(x, y) for y in range(W) for x in range(W) if rank(_V((x + off[0], y + off[1]))) < thr}
            # one ray per aligned cell
            for cy in range(0, W, L):
                for cx in range(0, W, L):
                    inside = [(x, y) for (x, y) in sel if cx <= x < cx + L and cy <= y < cy + L]
                    if len(inside) != 1:
                        failures.append("L=%d frame %d: aligned cell (%d, %d) holds %d rays (want 1)" % (L, f, cx, cy, len(inside)))
                        break
                    if L <= 8 and cell_rank((cx, cy), L, off) != rank(_V((inside[0][0] + off[0], inside[0][1] + off[1]))):
                        failures.append("L=%d frame %d: rtrCellRank of cell (%d, %d) is not its ray pixel's rank" % (L, f, cx, cy))
                        break
                else:
                    continue
                break
            # the resolve's lattice = the selection
            lat = set()
            for y in range(W):
                for x in range(W):
                    bx = ((x + off[0]) // L) * L - off[0]
                    by = ((y + off[1]) // L) * L - off[1]
                    if 0 <= bx < W and 0 <= by < W:
                        lat.add((bx, by))
            if lat != sel:
                failures.append("L=%d frame %d: the resolve's lattice (lbase) differs from the selected rays" % (L, f))
            for p in sel:
                hits[p] = hits.get(p, 0) + 1
            if failures:
                break
        if failures:
            break
        if L <= 8 and (len(hits) != W * W or set(hits.values()) != {1}):
            failures.append("L=%d: %d frames do not make every pixel a lattice site exactly once (%d of %d covered) -- "
                            "the rotation leaves part of each cell to interpolation" % (L, L * L, len(hits), W * W))
    if tuple(phase(0)) != (0, 0):
        failures.append("rtrLatticePhase(0) is not (0, 0): the setting off must keep the old lattice")

    # ---- #1570: the water receiver's miss is a sample ----
    p14 = re.search(r"#elif\s+CLEAN_DEFER_PASS\s*==\s*14\b", light)
    p14_end = re.search(r"#elif\s+CLEAN_DEFER_PASS\s*==", light[p14.end():]) if p14 else None
    fwd = light[p14.end():p14.end() + p14_end.start()] if p14 and p14_end else ""
    if not re.search(r"#define\s+RFT_FAR_FADE\s+float\(\s*0?\.\d+\s*\)", _strip_comments(read(FWD_H))):
        failures.append("clean_rt_fwd_temporal.h: RFT_FAR_FADE (the water hit's fade to the miss env) missing")
    if re.search(r"if\s*\(\s*t\s*>\s*0\.0\s*\)\s*\{\s*outv\s*=", fwd):
        failures.append("pass 14: the reflection is written only for a HIT (t > 0.0): water misses leave the FS's "
                        "own probe / sky between the hits -- the #1570 pixelated blocks")
    if not re.search(r"const\s+bool\s+waterRecv\s*=\s*ls\.model\s*==\s*RT_SURF_WATER\s*&&\s*\(\s*rtrOpts\(\)\s*&\s*32768u\s*\)\s*!=\s*0u\s*;", fwd):
        failures.append("pass 14: the water miss env must be gated by rtrOpts bit 15 (RenderVKRTReflectionsWaterMissEnv)")
    if not re.search(r"if\s*\(\s*t\s*>\s*0\.0\s*\|\|\s*waterRecv\s*\)\s*\{\s*outv\s*=", fwd) \
            or not re.search(r"smoothstep\s*\(\s*RFT_FAR_FADE\s*\*\s*tmaxR\s*,\s*tmaxR\s*,\s*t\s*\)", fwd) \
            or not re.search(r"sampleProbesEx\s*\(\s*hitP\s*,[^;]*ssr\.water\[2\]\.z", fwd):
        failures.append("pass 14: a water receiver's miss must write the env along its R (sampleProbesEx at the "
                        "water FS's roughness lod, else the sky) and its hits fade to it over RFT_FAR_FADE")

    # ---- #1606 (2): an empty gather holds its history ----
    if "rtrHistPeek(" not in resolve or not re.search(r"emptyHist\s*&&\s*rtrEmptyHold\s*\(\s*\)", resolve):
        failures.append("pass 13: a candidate whose gather has no traced sample must hold its reprojected history "
                        "(rtrHistPeek + rtrEmptyHold) -- else under GI the cell flips between the occluded env and lit")
    else:
        hold = resolve.find("rtrHistPeek(")
        clear = re.search(r"if\s*\(\s*temporal\s*\)\s*imageStore\s*\(\s*uRtHist", resolve)
        blk = " ".join(resolve[hold:clear.start()].split()) if clear else ""
        if not clear or clear.start() < hold or "temporal = false" not in blk:
            failures.append("pass 13: the empty-cell hold must run before the history-clearing fallback and mark the "
                            "history written (temporal = false)")
    hm = re.search(r"#define\s+RTR_HOLD_MAX\s+([0-9.]+)", light)
    if not hm or not (1.0 <= float(hm.group(1)) <= 32.0) or "held < RTR_HOLD_MAX" not in resolve:
        failures.append("the empty-cell hold must be age-limited (RTR_HOLD_MAX in [1, 32] frames)")
    if "RTR_HOLD_DECAY" in light or not re.search(r"const\s+vec3\s+res\s*=\s*hh\s*;", resolve) \
            or not re.search(r"remS\s*=\s*1\.0\s*-\s*f\s*;", resolve):
        failures.append("the empty-cell hold must splice the pure history and leave no env residual (remS = 1 - f): "
                        "env mixed into a held value escapes the GI specular occlusion (#87 sealed-room leak)")
    eh = re.search(r"bool\s+rtrEmptyHold\s*\(\s*\)\s*\{\s*return\s*\(\s*rtt_opts4\s*&\s*16u\s*\)", light)
    host = read(HOST)
    if not eh or not re.search(r"mRtReflEmptyHold\s*\?\s*16u", host):
        failures.append("rtrEmptyHold must read rtt_opts4 bit 4, set by the host from RenderVKRTReflectionsEmptyHold")

    # ---- #1606 (2b): one black lobe sample must not wipe the stochastic history ----
    lg = re.search(r"bool\s+rtrLowNGuard\s*\(\s*\)\s*\{\s*return\s*\(\s*rtt_opts4\s*&\s*32u\s*\)", light)
    gn = re.search(r"#define\s+RTR_GUARD_N\s+([0-9.]+)", light)
    if not lg or not gn or float(gn.group(1)) <= 1.0 or not re.search(r"mRtReflLowNGuard\s*\?\s*32u", host) \
            or not re.search(r"hc\s*=\s*rtrLowNGuard\s*\(\s*\)\s*\?\s*mix\s*\(\s*h\s*,\s*hc0\s*,\s*clamp\s*\(\s*\(\s*nEff\s*-\s*1\.0\s*\)", light):
        failures.append("rtrTemporal (stochastic glossy): the history clamp must be weighted by the gather's effective "
                        "sample count (rtrLowNGuard, rtt_opts4 bit 5, RTR_GUARD_N > 1) -- a single black lobe sample "
                        "wiped the history: dark squares near wall edges fading back")

    # ---- #1606 (3): the RR hit-distance guide is dense ----
    t0 = re.search(r"#define\s+RTR_GUIDE_T0\s+([0-9.]+)", light)
    tm = re.search(r"#define\s+RTR_GUIDE_MISS\s+([0-9.]+)", light)
    enc = re.search(r"float\s+rtrGuideEncode\s*\(\s*float\s+t\s*\)\s*\{\s*return\s+-\s*\(\s*RTR_GUIDE_T0\s*\+\s*clamp\s*\(\s*t\s*,\s*0\.0\s*,\s*RTR_GUIDE_MISS\s*\)\s*\)\s*;", light)
    if not (t0 and tm and enc) or not re.search(
            r"if\s*\(\s*!\s*\(\s*own\.a\s*>=\s*0\.0\s*\)[^;]*\)\s*imageStore\s*\(\s*uRtTrace\s*,[^;]*rtrGuideEncode", resolve):
        failures.append("pass 13: an untraced candidate must get its resolved hit distance in uRtTrace "
                        "(rtrGuideEncode, only where the trace did not cast: own.a < 0)")
    guides = _strip_comments(read(GUIDES))
    dec = re.search(r"float\s+rrResolvedT\s*\(\s*float\s+a\s*\)\s*\{(.*?)\n\}", guides, re.S)
    if not dec or "rrResolvedT(a)" not in guides.replace(" ", ""):
        failures.append("clean_rr_guides.comp: the hit-distance guide must decode the resolved distances (rrResolvedT)")
    elif t0 and tm:
        body = dec.group(1)
        lim = re.search(r"!\s*\(\s*a\s*<=\s*(-[0-9.]+)\s*\)", body)
        off = re.search(r"-\s*a\s*-\s*([0-9.]+)\s*;", body)
        far = re.search(r"t\s*>=\s*([0-9.]+)", body)
        T0, MISS = float(t0.group(1)), float(tm.group(1))
        if not (lim and off and far):
            failures.append("clean_rr_guides.comp rrResolvedT: unrecognised form: update this check")
        else:
            L0, O, F = float(lim.group(1)), float(off.group(1)), float(far.group(1))

            def decode(a):
                if not a <= L0:
                    return None
                t = -a - O
                return 0.0 if t >= F else max(t, 1e-3)
            bad = []
            if T0 < 1.5 or decode(-1.0) is not None:
                bad.append("the trace's -1 (not traced) must never decode as a distance (T0 >= 1.5)")
            if O != T0:
                bad.append("decode offset %g != RTR_GUIDE_T0 %g" % (O, T0))
            for t in (0.01, 0.5, 3.0, 40.0, 900.0):
                d = decode(-(T0 + t))
                if d is None or abs(d - t) > 1e-3 * max(1.0, t):
                    bad.append("t=%g does not round-trip (%s)" % (t, d))
            if decode(-(T0 + MISS)) != 0.0 or MISS + T0 > 65504.0:
                bad.append("a resolved miss (RTR_GUIDE_MISS) must decode as a miss and fit rgba16f")
            for b in bad:
                failures.append("RR guide encoding: " + b)
    if re.search(r"latRot\s*=[^;]*rrRecon", host):
        failures.append("host: the lattice rotation is still held under DLSS RR (latRot depends on rrRecon)")
    # every reader of the trace image treats ANY negative a as "not traced" (the encoded distances are < -1)
    if re.search(r"\.a\s*(==|!=)\s*-\s*1\.0", light + guides):
        failures.append("a uRtTrace reader compares a == -1: the resolved distances (a < -1) would read as traced")

    if failures:
        for f in failures[:8]:
            result.fail(f)
        return
    result.summary = ("lattice L=2..16: one ray per cell every frame, resolve = selection, every pixel a site once per "
                      "L^2 frames; water misses spliced with the env; empty cells hold history; dense RR hit-distance guide")
