"""Guards for THE RT LIGHTING SCRATCH (per-pixel memory, 2026-11-12): one buffer for the GI split's records (set 3
binding 25) and the split shadow core's descriptors + light-sample jobs / visibilities (bindings 26 / 27).

At a 4K DLAA extent the two working sets were ~0.8 GB and ~1.0 GB of separate VRAM; they are never live at the same
time (pass 15 finishes before the GI's TRACE writes its first record), so they share one range: max() instead of sum.
The sharing is only correct while these hold:
  RECORDS-LAST      clean_deferred_light.comp: the GI records are the LAST part of binding 25 (giRecIndex past the list
                    AND the change tags; giChgTagWord before the records) -- the headers / list / tags are written by
                    the light prep or read across pass 15 and form the GI's KEPT PREFIX, which the shadow core never
                    overlaps.
  HOST-LAYOUT       llcleanrenderervk.h: giHitBytes = giHitKeepBytes + 96 B per texel, the kept prefix = the 3 headers
                    + the list + the tags (16 B per 4 texels each); rtScratchPlace starts the shadow core at the kept
                    prefix (256-aligned) and sizes the buffer as the max, not the sum.
  OFFSETS           llcleanprojstochvk.cpp: every host access to the job list's header (the light prep's reset, VIS's
                    indirect dispatch, the ReSTIR counters' copy) adds mPdnVisOff; bindings 26 / 27 are written with
                    their offsets / ranges, binding 25 with the GI's range.
  HANDOFF           recordProjStochTail: rtScratchHandoff after SHADE and the ReSTIR counters' copy (the last access),
                    before the denoiser -- the GI's writes over the shared range wait for it.
  ORDER             llcleanrenderervk.cpp: in each split-lighting recording order the shadow core (recordShadowMergeFirst /
                    recordProjStoch) is recorded before the in-chain recordGI.
  NO-DESTROY        mGiHit / mPdnSel / mPdnVis are views: nothing destroys them (only mRtScratch is destroyed).
"""
import re

NAME = "rt-lighting-scratch"
STAGE = "source"
DESCRIPTION = "the GI records and the split shadow core share one buffer safely (layout, offsets, handoff, order)"
GUARDS = ("A GI record or change tag written over the split shadow core's descriptors / job list while pass 15 still "
          "reads them (or the reverse), a header access at the wrong offset, a double free of the shared buffer.")

SHADER = "indra/vkclean/shaders/clean_deferred_light.comp"
HDR = "indra/newview/vkgfx/llcleanrenderervk.h"
PDN = "indra/newview/vkgfx/llcleanprojstochvk.cpp"
GI = "indra/newview/vkgfx/llcleangivk.cpp"
REN = "indra/newview/vkgfx/llcleanrenderervk.cpp"


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


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return _strip(f.read())

    sh, hdr, pdn, gi, ren = read(SHADER), read(HDR), read(PDN), read(GI), read(REN)
    bad = []

    # RECORDS-LAST
    if "return GI_REC_LIST + 2u * ((giTexels(D) + 3u) / 4u) + 6u * giTexel(m, D);" not in sh:   # (#1608: every view's texels)
        bad.append("RECORDS-LAST: giRecIndex no longer places the records after the list AND the change tags")
    tag = _body(sh, "uint giChgTagWord(")
    if tag is None or "return GI_REC_LIST + (n + 3u) / 4u + (t >> 2u);" not in tag:
        bad.append("RECORDS-LAST: giChgTagWord no longer places the tags between the list and the records "
                   "(inside the GI's kept prefix)")

    # HOST-LAYOUT
    if "return giHitKeepBytes(w, h) + (VkDeviceSize)w * h * 96u;" not in hdr:
        bad.append("HOST-LAYOUT: giHitBytes is not the kept prefix + 96 B per texel")
    keep = _body(hdr, "static VkDeviceSize   giHitKeepBytes(")
    if keep is None or "return 16u * (3u + (n + 3u) / 4u) + 16u * ((n + 3u) / 4u);" not in keep:
        bad.append("HOST-LAYOUT: giHitKeepBytes is not the headers + the list + the tags")
    place = _body(hdr, "static VkDeviceSize rtScratchPlace(")
    if place is None:
        bad.append("HOST-LAYOUT: rtScratchPlace not found")
    else:
        if "pdn && giBytes != 0u ? up(giKeep) : 0u" not in place:
            bad.append("HOST-LAYOUT: the shadow core's ranges do not start after the GI's kept prefix")
        if "std::max<VkDeviceSize>(giBytes, pdn ? vo + visBytes : 0u)" not in place:
            bad.append("HOST-LAYOUT: the scratch is not sized as the max of its clients")
    lay = _body(pdn, "bool LLCleanRendererVK::rtScratchLayout(")
    if lay is None or "rtScratchPlace(giBytes, giKeep, selBytes, visBytes, &selOff, &visOff)" not in lay:
        bad.append("HOST-LAYOUT: rtScratchLayout does not lay out through rtScratchPlace")

    # OFFSETS
    if "vkCmdUpdateBuffer(cmd, mPdnVis.buffer, mPdnVisOff, sizeof(kHdr), kHdr);" not in pdn:
        bad.append("OFFSETS: the light prep resets the job list's header without mPdnVisOff")
    if "vkCmdDispatchIndirect(cmd, mPdnVis.buffer, mPdnVisOff + 4u);" not in pdn:
        bad.append("OFFSETS: VIS's indirect dispatch reads the job list's header without mPdnVisOff")
    if "const VkBufferCopy vc{mPdnVisOff," not in pdn:
        bad.append("OFFSETS: the ReSTIR counters copy the job count without mPdnVisOff")
    for m in re.finditer(r"vkCmd(?!CopyBuffer)\w+\(\w+, mPdnVis\.buffer, ([^,;]+)", pdn):   # (the copy: its region, below)
        if "mPdnVisOff" not in m.group(1):
            bad.append("OFFSETS: an access to mPdnVis.buffer without mPdnVisOff: " + m.group(0))
    if re.search(r"vkCmdCopyBuffer\(cmd, mPdnVis\.buffer", pdn) and "vc{mPdnVisOff" not in pdn:
        bad.append("OFFSETS: a copy from mPdnVis.buffer without mPdnVisOff")
    sel = _body(pdn, "void LLCleanRendererVK::writePdnSelBinding(")
    if sel is None or "mPdnSelOff" not in sel or "mPdnVisOff" not in sel or "mPdnSelBytes" not in sel:
        bad.append("OFFSETS: writePdnSelBinding does not bind 26 / 27 at their ranges of the scratch")
    if "bi[2] = {hb, 0, mGiHit.buffer ? mRtScrGiBytes : VK_WHOLE_SIZE};" not in gi:
        bad.append("OFFSETS: binding 25 is not bound at the GI's range of the scratch")

    # HANDOFF
    tail = _body(pdn, "void LLCleanRendererVK::recordProjStochTail(")
    if tail is None:
        bad.append("HANDOFF: recordProjStochTail not found")
    else:
        h = tail.find("rtScratchHandoff(cmd)")
        s = tail.find("rsStatsRecord(cmd)")
        t = tail.find("recordTemporal(")
        if h < 0 or s < 0 or t < 0 or not (s < h < t):
            bad.append("HANDOFF: rtScratchHandoff is not recorded after SHADE + the ReSTIR counters' copy, before "
                       "the denoiser")
        # (RenderVKProjStochasticPasses) the extra passes' SELECT / VIS / SHADE: recorded from the tail, before it
        x = tail.find("recordProjStochPasses(cmd")
        if x < 0 or not (x < s < h):
            bad.append("HANDOFF: recordProjStochPasses is not recorded from recordProjStochTail before the counters' "
                       "copy and rtScratchHandoff")
    ho = _body(pdn, "void LLCleanRendererVK::rtScratchHandoff(")
    if ho is None or "VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT" not in ho.split("m.dstStageMask")[0]:
        bad.append("HANDOFF: rtScratchHandoff does not wait for the job count's transfer read")

    # ORDER: every in-chain recordGI follows a shadow-core recording in the same block
    calls = [(m.start(), m.group(1)) for m in
             re.finditer(r"(recordShadowMergeFirst|recordProjStoch|recordGI)\(cmd", ren)]
    chain = [i for i, (_, n) in enumerate(calls) if n == "recordGI"]
    if not chain:
        bad.append("ORDER: no recordGI call found in llcleanrenderervk.cpp; update this check")
    for i in chain:
        pos = calls[i][0]
        if "mGiLagNow) recordGI" in ren[pos - 40:pos + 10] and "if (!mGiLagNow)" not in ren[pos - 20:pos]:
            continue   # (the lag GI after the combine: q_gi_lag / after the joined head)
        prev = [n for _, n in calls[max(0, i - 3):i]]
        if "recordProjStoch" not in prev and "recordShadowMergeFirst" not in prev:
            bad.append("ORDER: a recordGI (offset %d) is not preceded by the shadow core in its block" % pos)

    # ... and the merged pass records the shadow core through recordProjStoch (so the handoff covers it too)
    merged = _body(pdn, "void LLCleanRendererVK::recordShadowMergeFirst(")
    if merged is None or "recordProjStoch(cmd, p, viewOff, onAsync);" not in merged:
        bad.append("ORDER: recordShadowMergeFirst no longer records the shadow core through recordProjStoch "
                   "(its own SELECT / VIS / SHADE would miss rtScratchHandoff)")
    defs = [(d.start(), d.group(1)) for d in re.finditer(r"\n[A-Za-z][\w:<>* ]*\bLLCleanRendererVK::(\w+)\(", pdn)]
    for m in re.finditer(r"vkCmdBindPipeline\([^;]*(?:mPdnShadePipe|mPdnSelectPipe\[)", pdn):
        owner = [n for pos, n in defs if pos < m.start()]
        name = owner[-1] if owner else "?"
        if name not in ("recordProjStoch", "recordProjStochTail", "recordProjStochPasses"):
            bad.append("ORDER: the split shadow core is dispatched outside recordProjStoch (%s)" % name)

    # NO-DESTROY
    for src, name in ((pdn, PDN), (gi, GI), (ren, REN)):
        for v in ("mGiHit", "mPdnSel", "mPdnVis"):
            if re.search(r"(destroyBuffer|retireBuffer)\(%s\)" % v, src):
                bad.append("NO-DESTROY: %s destroys the view %s (only mRtScratch owns the memory)" % (name, v))

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "records last, layout = max, offsets, handoff, order, views never destroyed"
