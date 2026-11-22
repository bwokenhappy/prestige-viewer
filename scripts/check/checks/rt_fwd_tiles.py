"""Regression guard for RT FORWARD's cost: an indirect dispatch over a tile list, not the whole half-resolution grid.

Pass 14 (clean_deferred_light.comp, RenderVKRTReflectionsForward) traces one camera ray per 2 x 2 block over the
alpha-blend / water classes. It was dispatched over the WHOLE half-resolution grid every frame: with a few windows in
view (RTX 5090, 1280 x 696 internal) c_defer_rtrefl_fwd cost 1.40-1.52 ms -- mostly texels with no layer at all,
each holding the ~1.8 MB, 210-255-register kernel's occupancy for a ray that found nothing, on the critical path of
the async lighting chain (graphics waits at g_join_gi; research/GI_OVERLAP_2026-11-09.md).

Now a thin selection kernel (pass 14 -DCLEAN_RT_FWD_SEL) runs first: per half-resolution texel the same camera ray,
first-hit, any candidate of the two classes, on a slightly longer interval -- a SUPERSET of the tiles where the forward
walk can find a layer. Listed tiles go into THE FORWARD TILE LIST (mDsRtTiles past the ray list) and RT FORWARD runs
indirect over it, its body unchanged inside a listed tile. Unlisted tiles get the forward pass's no-layer values from
the selection: rows 0 and 1 + parity vec4(0), row 5 vec4(0, 0, 0, -1) -- the history contract: every history read is
accepted only on a depth match in the reflection history row, so a tile that drops out leaves nothing stale and a
tile entering starts clean.

Pinned here: the selection (first-hit flags, both classes, the longer interval than the forward's), the unlisted
stores, the list's layout on both sides (66 n + 8 words past the header), the forward's list-driven tile and its
uniform early return, the host's indirect dispatch at the list's args, the header reset, the barriers before the
indirect read on every path (ray list / tile trace / the #1039 M6 levels), the pipeline gate, and the #1128 beacon twin
of the selection (RT_BC_KERNELS, kRtBcSpv, rtBcPipe).
On the tree before this change recordRtReflectFwd dispatches the full grid: the check fails.
"""
import re

NAME = "rt-fwd-tiles"
STAGE = "source"
DESCRIPTION = "RT FORWARD dispatches indirect over a blend / water tile list; unlisted tiles keep the no-layer contract"
GUARDS = ("RT FORWARD's full-grid dispatch: ~1.4 ms per frame on a 5090 for a few windows (the heavy kernel's "
          "occupancy paid by every texel without a layer), on the async lighting chain's critical path. And a tile "
          "list that leaves stale forward reflections / history in tiles that dropped out.")

_SH = "indra/vkclean/shaders/"


def _func(text, head):
    i = text.find(head)
    if i < 0:
        return ""
    j = text.find("\n}\n", i)
    return text[i:j] if j > 0 else text[i:]


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    d = read(_SH + "clean_deferred_light.comp")
    i = d.find("#elif CLEAN_DEFER_PASS == 14")
    p14 = d[i:d.find("#elif CLEAN_DEFER_PASS == 15", i)] if i >= 0 else ""
    if not p14:
        return ["clean_deferred_light.comp has no pass 14 section"]
    # the buffer for pass 14, without RTR_HAVE_TILES (pass 14's frame counter stays the push's)
    if "#if DEFER_RT_TILES || DEFER_RT_DBG || DEFER_RT_OCC || CLEAN_DEFER_PASS == 14" not in d:
        missing.append("pass 14 does not declare the RT tile buffer (binding 10) its tile list lives in")
    if not re.search(r"#if CLEAN_DEFER_PASS != 14\s*\n#define RTR_HAVE_TILES 1", d):
        missing.append("RTR_HAVE_TILES is defined for pass 14 (its stochastic sequence would change to the header's)")
    if "uint rftListBase() { return 66u * rtt_args[3] + 8u; }" not in p14:
        missing.append("pass 14 has no rftListBase at 66 n + 8 (past the ray list's n x 64 entries)")
    fwd_i = p14.find("#ifndef CLEAN_RT_FWD_SEL\n// (THE FORWARD TILE LIST)")
    sel_i = p14.find("// ---- RT FORWARD SELECT")
    if fwd_i < 0 or sel_i < 0 or sel_i < fwd_i:
        missing.append("pass 14 does not split into RT FORWARD (indirect) and RT FORWARD SELECT (-DCLEAN_RT_FWD_SEL)")
        return missing
    fwd, sel = p14[fwd_i:sel_i], p14[sel_i:]
    for want, why in (
            ("if (fe >= min(rtt_words[fb], rtt_words[fb + 4u])) return;",
             "RT FORWARD does not return (whole workgroup, before the barrier) past the listed count / capacity"),
            ("const uint ft = rtt_words[fb + 8u + fe];", "RT FORWARD does not take its tile from the forward tile list"),
            ("ivec2(int(ft & 0xFFFFu), int(frow)) * 8 + ivec2(gl_LocalInvocationID.xy)",
             "RT FORWARD's texel is not its listed tile's (x | row << 16, 8 x 8; #1608: the row within its view)")):
        if want not in fwd:
            missing.append(why)
    if "gl_GlobalInvocationID" in fwd.split("void DEFER_MAIN()", 1)[-1].split("rtBcLane", 1)[0]:
        missing.append("RT FORWARD still derives its texel from the global invocation (a full-grid dispatch)")
    for want, why in (
            ("gl_RayFlagsTerminateOnFirstHitEXT, RT_MASK_BLEND | RT_MASK_WATER",
             "the selection's ray is not a first-hit query over both forward classes (blend + water)"),
            ("if (rtRaySane(o, tmin, dir, tmax))", "the selection's ray skips rtRaySane (Xid 109 / the #1128 beacon)"),
            ("rtBcLane(uint(hp.y) * uint(hsz.x) + uint(hp.x), px);", "the selection's beacon twin does not name the pixel"),
            ("rtt_words[fb + 8u + i] = gl_WorkGroupID.x | (row << 16);",
             "the selection does not list its tile (x | row << 16; #1608: the row numbers every view's rows)"),
            ("if (i < rtt_words[fb + 4u])", "the selection appends past the list's capacity"),
            ("atomicMax(rtt_words[fb + 1u], min(i + 1u, RTT_ROW));", "the selection does not raise the dispatch's x"),
            ("if (i % RTT_ROW == 0u) atomicAdd(rtt_words[fb + 2u], 1u);", "the selection does not raise the dispatch's rows"),
            # THE HISTORY CONTRACT: the unlisted tile's no-layer values
            ("imageStore(uRtFwd, DS_TEXEL(hp), vec4(0.0));", "an unlisted tile's resolved row 0 is not cleared (rtFwdIn: stale)"),
            ("imageStore(uRtFwd, DS_TEXEL(hp + ivec2(0, hsz.y * int(1u + par))), vec4(0.0));",
             "an unlisted tile's history row 1 + parity is not cleared: stale history reappears when it re-enters"),
            ("imageStore(uRtFwd, DS_TEXEL(hp + ivec2(0, hsz.y * 5)), vec4(0.0, 0.0, 0.0, -1.0));",
             "an unlisted tile's layer GI row 5 is not the no-GI value (rtFwdGiIn)")):
        if want not in sel:
            missing.append(why)
    # the selection's interval never shorter than the forward walk's (a superset of the tiles with a layer)
    mf = re.search(r"length\(rtrPositionAt\(px\) - cam\) \* ([0-9.]+) : 4096\.0", fwd)
    ms = re.search(r"length\(rtrPositionAt\(px\) - o\) \* ([0-9.]+) : 4096\.0", sel)
    if not mf or not ms or not float(ms.group(1)) > float(mf.group(1)):
        missing.append("the selection's tmax is not longer than RT FORWARD's (the list must be a superset)")
    # the forward's history reads stay gated on the reflection history row (the contract the clears rely on)
    if "rftSameLayer(h.a, ew)" not in p14 or "rftSameLayer(abs(h.a)" not in p14:
        missing.append("a history read of pass 14 is no longer accepted on the reflection history row's depth")

    cpp = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    hdr = read("indra/newview/vkgfx/llcleanrenderervk.h")
    if "{ return kRtTilesHdrBytes + ((VkDeviceSize)mDsRtTileCount * 66u + 8u) * sizeof(uint32_t); }" not in hdr:
        missing.append("rtFwdListOff is not at 66 n + 8 words past the header (rftListBase)")
    rf = _func(cpp, "void LLCleanRendererVK::recordRtReflectFwd(")
    if "vkCmdDispatchIndirect(cmd, mDsRtTiles.buffer, rtFwdListOff() + sizeof(uint32_t));" not in rf:
        missing.append("recordRtReflectFwd does not dispatch indirect at the forward tile list's args")
    if re.search(r"vkCmdDispatch\(cmd, \(hw", rf):
        missing.append("recordRtReflectFwd dispatches the whole half-resolution grid")
    if "rtBcPipe(mRtFwdPipe)" not in rf:
        missing.append("recordRtReflectFwd lost its #1128 beacon twin")
    rs = _func(cpp, "void LLCleanRendererVK::recordRtReflectFwdSel(")
    if "rtBcPipe(mRtFwdSelPipe)" not in rs or "vkCmdDispatch(cmd, (hw + 7u) / 8u, (hh + 7u) / 8u, mViewCount);" not in rs:
        missing.append("recordRtReflectFwdSel does not dispatch the selection over the half-resolution grid")
    rr = _func(cpp, "void LLCleanRendererVK::recordRtReflect(")
    i_sel, i_list = rr.find("recordRtReflectFwdSel(cmd, p, onAsync);"), rr.find("if (mRtReflRayList && mRtSelectPipe")
    if i_sel < 0 or i_list < 0 or i_sel > i_list:
        missing.append("recordRtReflect does not run the selection behind its opening barrier, before SELECT")
    tail = rr[rr.find("vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mRtTracePipe);") - 900:]
    if "VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT" not in tail[:900]:
        missing.append("the tile-trace path has no barrier between the selection and RT FORWARD's indirect read")
    mi = _func(cpp, "void LLCleanRendererVK::recordRtMarkInit(")
    if ("const uint32_t fwdl[kRtFwdListHdrWords] = {0u, 0u, 0u, 1u, mRtFwdTileCount, 0u, 0u, 0u};" not in mi
            or "vkCmdUpdateBuffer(cmd, mDsRtTiles.buffer, rtFwdListOff(), sizeof(fwdl), fwdl);" not in mi):
        missing.append("recordRtMarkInit does not reset the forward tile list (count 0, dispatch (0, 0, 1), capacity)")
    if not re.search(r"\+ \(\(VkDeviceSize\)kRtFwdListHdrWords \+ fwdTiles\) \* sizeof\(uint32_t\)", cpp):
        missing.append("mDsRtTiles is not sized for the forward tile list")
    if "mRtFwdNow = mRtReflNow && mRtFwdPipe && mRtFwdSelPipe && mRtFwdTileCount != 0u && rtrFwd;" not in cpp:
        missing.append("RT FORWARD may run without the selection that builds its list")
    if not re.search(r"\{kWorldDeferRtFwdSelBcSpv, kWorldDeferRtFwdSelBcSpv_bytes,", cpp):
        missing.append("the selection has no #1128 beacon twin in kRtBcSpv")
    sel_fn = _func(cpp, "VkPipeline LLCleanRendererVK::rtBcPipe(")
    if "mRtFwdSelPipe" not in sel_fn:
        missing.append("rtBcPipe does not map the selection to its beacon twin")
    vq = read("indra/newview/vkgfx/llcleanvisqueuevk.cpp")
    lv = _func(vq, "bool LLCleanRendererVK::recordRtReflectLeveled(")
    if lv.count("recordRtReflectFwdSel(c, p, onAsync);") != 2:
        missing.append("the #1039 M6 schedule does not run the selection in stage 1 of both layouts (ray list / tile)")
    rg = read("indra/vkclean/regen_world_spv.py")
    if not re.search(r'"kWorldDeferRtFwdSelSpv",\s+\["-DCLEAN_DEFER_PASS=14", "-DCLEAN_PROJ_SHADOW_RAYQUERY", '
                     r'"-DRT_SET=4", "-DCLEAN_RT_FWD_SEL"\]', rg):
        missing.append("regen_world_spv.py does not build kWorldDeferRtFwdSelSpv (pass 14 -DCLEAN_RT_FWD_SEL)")
    bc = rg[rg.find("RT_BC_KERNELS = ["):]
    bc = bc[:bc.find("]")]
    names = re.findall(r'"(kWorld\w+)"', bc)
    if len(names) != 11 or names[10] != "kWorldDeferRtFwdSelSpv":
        missing.append("RT_BC_KERNELS does not end with kWorldDeferRtFwdSelSpv as kernel 11 (= kRtBcSpv)")
    if "kRtBcKernels = 11u" not in hdr:
        missing.append("kRtBcKernels is not 11 (RT_BC_KERNELS / kRtBcSpv)")
    return missing


def run(ctx, result):
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    result.summary = "selection + indirect forward + no-layer clears + barriers + beacon twin"
