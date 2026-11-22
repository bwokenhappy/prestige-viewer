"""Regression guard for the LIGHT / SHADOW DROPOUT (2026-11-13).

The owner, StallionsDale at night, an overhead camera: a gazebo lit magenta / cyan with crisp shadows lost them on
a small camera move -- the dome washed out by a light its roof should block -- and a straight line crossed the
lower screen, shadowed on one side and not the other. Two limits took a light's SHADOW away while the lossless
froxel walk kept lighting it (the wash-out: lit unshadowed):
  * THE LIGHT GRID'S EDGE: pass 15 (stochastic local-light shadows / ReSTIR DI) took the extended light set (every
    point light, record-less projectors) from the light grid's cells only, and the lit kernels gave that set its
    shadow only inside the grid (deferRsStoch: regirCell(P) != REGIR_NO_CELL). The grid is a camera-centred
    clipmap -- +-128 m across, +-64 m up / down at the defaults, its faces snapped to the 8 m outer cells -- so a
    camera ~70 m up had the ground at the bottom face: an 8 m snap switched whole scenes between shadowed and
    unshadowed, with a line where the terrain crossed the face. A hard, popping distance limit on shadows.
  * THE CLUSTER LIST BUDGET: clean_lights.comp lists at most CLUSTER_CAP lights per cluster (shadowed projectors
    first, then the camera-importance order). RenderVKRTProjShadowMax goes to 256, so the shadowed projectors
    alone could overflow it (the lit guard's "shadowed-projector overflow", 10-15 clusters a frame in the
    owner's log); every consumer that walked the list alone gave the lights past the cap no shadow, in tile x
    depth-slice blocks.
The fix: deferRsStoch holds on every ReSTIR frame; pass 15's out-of-grid walk takes the whole set (rsInSet) from
the pixel cluster's FULL list; every shadow consumer of a cluster list walks the whole light table where the list
overflowed (clean_lights_common.glsl clusterWalkAll / clusterShadowedWalkAll); CLUSTER_CAP 64 -> 127 (the packing's
limit, the measured demand was 112); a rate-limited #VKLitGuard line names the lights each limit reached.
This check fails when any of that is undone:
  (a) the cap's twins (shader / host / vqbench) disagree or exceed the 7-bit packing; the lossless predicates are
      gone or no longer test what clean_lights.comp packs;
  (b) a pass-15 / deferred shadow walk of the cluster's shadowed run (clusterShadowedLen) without its overflow
      fallback, or the forward cluster walk without one;
  (c) deferRsStoch gated on the grid again, the split selection's set gated on it, pass 15's out-of-grid walk back
      to the records alone, the occluded-point share or the own-fixture rule gated on the grid;
  (d) the host's log line unwired;
  (e) a model of clean_lights.comp's packing + the consumers' rule: over random clusters (up to 256 lights,
      shadowed and not, in importance order) every light that touches the cluster is walked by the shadow
      consumers -- the list's when it fits, the table's when it does not.
"""
import random
import re

NAME = "light-shadow-dropout"
STAGE = "source"
DESCRIPTION = "local-light shadows never drop out by light-grid distance or cluster capacity (no popping, no line)"
GUARDS = ("A light losing its shadow on a small camera move while still lighting the scene (a washed-out, "
          "unshadowed area bounded by a straight line): the light grid's edge and the cluster list cap.")

SH = "indra/vkclean/shaders/"


def _function(src, header_regex):
    m = re.search(header_regex, src)
    if not m:
        return None
    i = src.find("{", m.end() - 1)
    depth = 0
    while 0 <= i < len(src):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[m.start():i + 1]
        i += 1
    return None


def _strip(src):
    return re.sub(r"//[^\n]*", "", src)


def _model(cap, rng):
    """clean_lights.comp's two passes + packing and the consumers' walk rule, over random clusters."""
    bad = []
    for trial in range(4000):
        nl = rng.randint(0, 256)
        shadowed = [rng.random() < rng.choice((0.05, 0.3, 0.9)) for _ in range(nl)]
        touch = [rng.random() < rng.choice((0.1, 0.6, 1.0)) for _ in range(nl)]
        lst, n, ns, total, stotal = [], 0, 0, 0, 0
        for pass_ in (0, 1):
            for li in range(nl):
                if shadowed[li] != (pass_ == 0) or not touch[li]:
                    continue
                total += 1
                stotal += 1 if shadowed[li] else 0
                if n < cap:
                    lst.append(li)
                    n += 1
            if pass_ == 0:
                ns = n
        raw = n | (ns << 8) | (0x8000 if stotal > cap else 0) | (min(total, 0xFFFF) << 16)
        clen = min(raw & 0xFF, cap)
        cslen = min((raw >> 8) & 0x7F, clen)
        walk_all = (raw >> 16) > cap
        walk_all_s = (raw & 0x8000) != 0
        # the full-list consumers (pass 15 past the grid, the split walk, forward)
        walked = set(range(nl)) if walk_all else set(lst[:clen])
        want = {li for li in range(nl) if touch[li]}
        if not want <= walked:
            bad.append("trial %d: %d touching lights not walked (full list, %d listed)" % (trial, len(want - walked), clen))
        # the shadowed-run consumers (the exact resolve, the records' pass 15 walk)
        walked_s = set(range(nl)) if walk_all_s else set(lst[:cslen])
        want_s = {li for li in range(nl) if touch[li] and shadowed[li]}
        if not want_s <= walked_s:
            bad.append("trial %d: %d touching shadowed projectors not walked" % (trial, len(want_s - walked_s)))
        if len(bad) > 3:
            break
    return bad


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    fail = []
    common = read(SH + "clean_lights_common.glsl")
    lights = read(SH + "clean_lights.comp")
    defer = read(SH + "clean_deferred_light.comp")
    draw = read(SH + "clean_draw_common.glsl")
    host_h = read("indra/newview/vkgfx/llcleanrenderervk.h")
    host = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    vqb = read("indra/vqbench/vqb_main.cpp")

    # (a) the cap and the predicates
    m = re.search(r"#define CLUSTER_CAP (\d+)u", common)
    mh = re.search(r"CLUSTER_GZ = 24, CLUSTER_CAP = (\d+);", host_h)
    mv = re.search(r"CLUSTER_COUNT = GX \* GY \* GZ, CLUSTER_CAP = (\d+);", vqb)
    cap = int(m.group(1)) if m else None
    if not (m and mh and mv):
        fail.append("CLUSTER_CAP not found in clean_lights_common.glsl / llcleanrenderervk.h / vqb_main.cpp")
    else:
        if not (cap == int(mh.group(1)) == int(mv.group(1))):
            fail.append("CLUSTER_CAP twins disagree: shader %s, host %s, vqbench %s" % (cap, mh.group(1), mv.group(1)))
        if cap > 127:
            fail.append("CLUSTER_CAP %d > 127: clusterShadowedLen packs the shadowed run in 7 bits" % cap)
        if cap < 127:
            fail.append("CLUSTER_CAP %d: the measured demand (112 lights in one cluster, 2026-11-13) wants 127" % cap)
    if "bool clusterWalkAll(uint raw)         { return clusterCandidates(raw) > CLUSTER_CAP; }" not in common \
            or "bool clusterShadowedWalkAll(uint raw) { return clusterShadowedOverflow(raw); }" not in common:
        fail.append("clean_lights_common.glsl: clusterWalkAll / clusterShadowedWalkAll changed or gone")
    if "cluster_count[c] = n | (ns << 8u) | (stotal > CLUSTER_CAP ? 0x8000u : 0u) | (min(total, 0xFFFFu) << 16u);" \
            not in lights:
        fail.append("clean_lights.comp: the cluster count packing changed (bit 15 / bits 16-31 feed the walk-all rule)")

    # (b) every shadowed-run walk has its fallback (code only, comments stripped)
    dcode = _strip(defer)
    for mm in re.finditer(r"clusterShadowedLen\(", dcode):
        win = dcode[max(0, mm.start() - 700):mm.start() + 300]
        if "WalkAll(" not in win:
            line = dcode.count("\n", 0, mm.start()) + 1
            fail.append("clean_deferred_light.comp ~line %d (comments stripped): a clusterShadowedLen walk without "
                        "its clusterShadowedWalkAll fallback (shadowed projectors past CLUSTER_CAP lit unshadowed)" % line)
    if "const bool all = mirror || (!probeCapture() && clusterWalkAll(cluster_count[c]));" not in draw:
        fail.append("clean_draw_common.glsl: the forward cluster walk has no clusterWalkAll fallback")

    # (c) the grid no longer limits shadows
    rs = _function(defer, r"bool deferRsStoch\(uint li, vec3 P\)\s*\{")
    if not rs:
        fail.append("clean_deferred_light.comp: deferRsStoch not found")
    elif "regirCell" in _strip(rs) or "REGIR_NO_CELL" in _strip(rs):
        fail.append("deferRsStoch is gated on the light grid again: point lights past its faces lose their shadows")
    if "const bool spGrid = rsFrameOn();" not in defer:
        fail.append("pass 15's split selection gates the extended set on the light grid again (spGrid)")
    i = dcode.find("else if (projOn)")
    blk = dcode[i:i + 2600] if i >= 0 else ""
    for need, why in (("const bool rsExt = rsFrameOn();", "the ReSTIR-frame test"),
                      ("rsExt ? clusterWalkAll(craw) : clusterShadowedWalkAll(craw)", "the overflow fallback"),
                      ("rsExt ? clusterLen(craw) : clusterShadowedLen(craw)", "the full list on a ReSTIR frame"),
                      ("const bool ext = rsExt && rsInSet(li);", "the lighting set's rule"),
                      ("pdnTargetGoboL(li, P, s.N, ext, p0)", "the set's target (no record test)"),
                      ("if (rsExt) rt_fix_max = rg.rs_params.y;", "the own-fixture rule")):
        if need not in blk:
            fail.append("pass 15's out-of-grid walk: %s missing (%s) -- past the grid it takes the records alone"
                        % (need, why))
    if "if ((sd0.x & PDN_SEL_CELL) != 0u || rsFrameOn()) rt_fix_max = rg.rs_params.y;" not in defer:
        fail.append("pass 15's VIS stage: the own-fixture rule only inside the grid (lamp shades shadow their lamps past it)")
    if "if (ocCull != OCC_NONE && projOn && rsFrameOn() && (rg.rs_cfg.z & RS_F_OCC_PT_DROP) == 0u)" not in defer:
        fail.append("pass 15: the occluded point lights' share gated on the grid (the ratio's set differs past it)")

    # (d) the log
    if "void LLCleanRendererVK::rsShadowReachLog(const LLVKWorldFrame& wf)" not in host \
            or "    rsShadowReachLog(wf);\n}" not in host or "light shadows past the old limits" not in host:
        fail.append("llcleanrenderervk.cpp: the #VKLitGuard shadow-reach line is unwired (rsLightSet -> rsShadowReachLog)")

    # (e) the model
    if cap:
        fail += _model(cap, random.Random(1113))

    for f in fail:
        result.fail(f)
