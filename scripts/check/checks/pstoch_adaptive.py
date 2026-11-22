"""Regression guard for RenderVKProjStochasticAdaptive (2026-11-15: the stochastic local-light shadows' extra passes on the
8 x 8 tiles a GPU needs-rays mask flags -- moving caster / young history / penumbra -- dispatched indirect over a tile list).

  (a) OFF IS TODAY'S FRAME: every host command of the path sits behind mPdnAdaptNow (0 unless the setting is > 0 and the
      rules admit it), the tile bit PDN_PASS_TILES (push rtr3.z bit 19) is set only in an extra pass's push, the tile list
      is allocated only while wanted, and the denoiser's adaptive history bits are 0 unless the A/B asks for them.
  (b) GPU-DRIVEN: SELECT / SHADE of an adaptive pass are vkCmdDispatchIndirect on the tile list's header (with
      mPdnVisOff), the listed count is clamped to the tile count in the shader, no CPU readback in the frame (the stats
      ring is harvested 3 frames late).
  (c) BOUNDED: the list lives in the exact list's place (never both), sized by the denoiser extent; the header is reset
      before every mask (nothing carried across frames in the RT lighting scratch).
  (d) THE BUDGET: a replay of the LIST phase's admission -- every listed count <= the budget, the classes admitted from
      3 down (no lower-class tile listed while a higher one is left out), everything listed when the budget covers it.
"""
import random
import re

NAME = "pstoch-adaptive"
STAGE = "source"
DESCRIPTION = "RenderVKProjStochasticAdaptive: off unchanged, indirect over the GPU tile list, bounded, the cap admits " \
              "moving caster > young history > penumbra"
GUARDS = ("The adaptive path recording commands with the setting at 0, a CPU readback of the tile list, a tile list beside "
          "the mover-exact list, a listed count past the budget or the tile count, and a cap that drops movers first.")

DL = "indra/vkclean/shaders/clean_deferred_light.comp"
PDN = "indra/newview/vkgfx/llcleanprojstochvk.cpp"
DN = "indra/vkclean/shaders/clean_dn_temporal.comp"
DNC = "indra/newview/vkgfx/llcleandenoisevk.cpp"
SETTINGS = "indra/newview/app_settings/settings.xml"
SPV = "indra/vkclean/regen_world_spv.py"


def _ren_h():
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "..", "..", "..", "indra", "newview", "vkgfx", "llcleanrenderervk.h"), encoding="utf-8") as f:
        return f.read()


def _strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _body(src, head):
    i = src.find(head)
    if i < 0:
        return None
    j = src.find("{", i)
    depth = 0
    for k in range(j, len(src)):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
    return None


def _structure(dl, pdn, dn, dnc, settings, spv):
    bad = []
    # (a) off
    if "mPdnAdaptNow = (mPdnAdapt > 0u && why == 0u) ? mPdnAdapt : 0u;" not in pdn:
        bad.append("(a) mPdnAdaptNow is not 0 whenever the setting is 0 or a rule refuses it")
    if "if (mPdnPassesNow > 1u) why = 1u;" not in pdn or "else if (mLightPrep.exact || mPdnVisExact) why = 4u;" not in pdn \
            or "else if (!mPdnRadiance) why = 3u;" not in pdn or "else if (!split) why = 2u;" not in pdn:
        bad.append("(a) the adaptive passes are not refused with Passes > 1 / the fused core / ratio mode / the exact list")
    if "if ((mPdnPassesNow > 1u || mPdnAdaptNow != 0u) && pipe == mPdnShadePipe && visPipe != VK_NULL_HANDLE)" not in pdn:
        bad.append("(a) recordProjStochPasses is not gated by Passes > 1 or mPdnAdaptNow")
    sp = _body(pdn, "void LLCleanRendererVK::recordProjStochPasses(")
    if sp is None:
        bad.append("recordProjStochPasses not found")
        sp = ""
    if "const bool tiles = mPdnAdaptNow != 0u;" not in sp or "if (tiles) recordProjStochMask(cmd, rp, onAsync);" not in sp \
            or "if (tiles) xp.rtr3[2] |= 0x80000u;" not in sp or "if (tiles) recordProjStochAdaptStats(cmd);" not in sp:
        bad.append("(a) the mask / the tile bit / the stats copy are not all behind the tiles flag in recordProjStochPasses")
    if pdn.count("0x80000u") != 1:
        bad.append("(a) PDN_PASS_TILES (0x80000u) is pushed somewhere other than an extra pass's push")
    if "dp.adaptPasses = (mPdnAdaptNow != 0u && mPdnAdaptHist) ? mPdnAdaptNow : 0u;" not in pdn:
        bad.append("(a) the denoiser's adaptive history field is set without the A/B or the adaptive passes")
    if "(p.radiance ? (std::min(p.adaptPasses, 7u) << 24u) : 0u)" not in dnc:
        bad.append("(a) the temporal ctl.y bits 24-26 are not the adaptive passes (radiance only)")
    if "dnAdaptPasses() != 0u && (mo.w & PDN_MOT_EXTRA_RAYS) != 0u" not in dn:
        bad.append("(a) the adaptive mover history rule does not test the A/B field and the producer's mark")
    # (b) GPU-driven
    if sp.count("vkCmdDispatchIndirect(cmd, mPdnVis.buffer, mPdnVisOff + pdnExactWordOff() + 4u);") != 2:
        bad.append("(b) an adaptive pass's SELECT / SHADE is not dispatched indirect over the tile list")
    if "if (i >= min(pdn_vis[b], T)) return false;" not in dl:
        bad.append("(b) pdnAdaptTexel does not clamp the listed count to the tile count")
    if "if (pdnPassTiles() && !pdnAdaptTexel(D, m)) return;" not in dl:
        bad.append("(b) SELECT / SHADE do not map an adaptive workgroup to its listed tile")
    st = _body(pdn, "void LLCleanRendererVK::recordProjStochAdaptStats(")
    if st is None or "mPdnAdaptRbSlot = (mPdnAdaptRbSlot + 1u) % 3u;" not in st:
        bad.append("(b) the adaptive stats are not a 3-slot ring harvested late")
    # (c) bounded
    if "const bool tilesWant = !exactWant && pdnTilesWanted() && (mPdnVisTiles || now >= mPdnTilesRetryAt);" not in pdn \
            or "return mPdnAdapt > 0u && mPdnRadiance && !pdnExactWanted();" not in pdn:
        bad.append("(c) the tile list can be allocated beside the exact list, or without the setting")
    # (LVC stage 1: + the depth admission's fixed 64 words, zeroed with the header -- kPdnAdZWords = PDN_AD_ZWORDS)
    if "return (16u + 2u * tiles + kPdnAdZWords) * 4u;" not in pdn or \
            "uint pdnAdaptBase(ivec2 D) { return pdnExactBase(D); }" not in dl or \
            "#define PDN_AD_ZWORDS (4u * PDN_AD_ZB)" not in dl or "kPdnAdZWords = 64u" not in _ren_h():
        bad.append("(c) the tile list's size / place changed (16 header words + 2 per tile + the 64 depth-admission "
                   "words, at the exact list's place)")
    mk = _body(pdn, "void LLCleanRendererVK::recordProjStochMask(")
    if mk is None or "vkCmdUpdateBuffer(cmd, mPdnVis.buffer, mPdnVisOff + pdnExactWordOff(), sizeof(kTileHdr), kTileHdr);" \
            not in mk or mk.find("vkCmdUpdateBuffer") > mk.find("vkCmdDispatch("):
        bad.append("(c) the tile list's header is not reset before the mask")
    if "gpuSubMark(cmd, onAsync ? \"c_defer_pstochmask\" : \"g_defer_pstochmask\");" not in (mk or ""):
        bad.append("(timers) the mask has no GPU mark of its own (c_defer_pstochmask)")
    for mark in ("c_defer_pstochxsel", "c_defer_pstochxvis", "\"c_defer_pstochx\""):
        if mark not in sp:
            bad.append("(timers) the adaptive passes do not record %s" % mark)
    # (e, 2026-11-15) THE PENUMBRA CLASS FLAGS NOISE: this frame's picks of one light disagree (pdnAdaptPicksDisagree, from
    # pass 0's descriptors + visibilities) -- the mixed-aggregate rule only behind RenderVKProjStochasticAdaptiveRule 1
    # (push rtr3.z bit 22); both rules' penumbra tiles counted in the header's diagnostic words 9-11 (< 16: reset, copied)
    tc = _body(dl, "uint pdnAdaptTexelClass(") or ""
    if "return ((g_draw_push.rtr3.z & PDN_AD_OLD_RULE) != 0u ? oldPen : newPen) ? 1u : 0u;" not in tc \
            or "const bool newPen = pdnAdaptPicksDisagree(m, D);" not in tc \
            or "#define PDN_AD_OLD_RULE 0x400000u" not in dl:
        bad.append("(e) the mask's penumbra class is not the picks-disagree rule (the old rule behind bit 22)")
    pd = _body(dl, "bool pdnAdaptPicksDisagree(") or ""
    if "if ((sd0.x & PDN_SEL_SURF) == 0u || (sd0.x & (PDN_SEL_DENSE | PDN_SEL_EXACT)) != 0u) return false;" not in pd \
            or "if ((max(max(vk.r, vk.g), vk.b) > 0.5) != hj) return true;" not in pd \
            or "pdnSelPickLi(d, k) != lj" not in pd:
        bad.append("(e) pdnAdaptPicksDisagree is not 'one light picked twice, one ray reached it, another blocked'")
    for w in re.findall(r"#define PDN_AD_W_\w+\s+(\d+)u", dl):
        if not 9 <= int(w) < 16:
            bad.append("(e) a mask diagnostic word (%s) is outside the reset / copied header's free words 9-15" % w)
    if "if (mPdnAdaptRule != 0u) xp.rtr3[2] |= 0x400000u;" not in (mk or ""):
        bad.append("(e) the old penumbra rule's bit is not pushed by RenderVKProjStochasticAdaptiveRule alone")
    if "kWorldDeferShadowSplitMaskSpv" not in spv or "-DCLEAN_SHADOW_STAGE=5" not in spv:
        bad.append("regen_world_spv.py: the mask variant (CLEAN_SHADOW_STAGE 5) is not built")
    # (2026-11-16, owner decision for 1.5: Adaptive passes and Rays per adaptive pass are Graphics
    # preferences, so they persist; the adaptive defaults are the owner's tested set -- 3 passes,
    # history 1, rule 0.)
    for key, default, persist in (("RenderVKProjStochasticAdaptive", 3, 1), ("RenderVKProjStochasticAdaptiveCap", 25, 0),
                                  ("RenderVKProjStochasticAdaptiveHistory", 1, 0), ("RenderVKProjStochasticAdaptiveRule", 0, 0),
                                  ("RenderVKProjStochasticAdaptiveSamples", 0, 1), ("RenderVKProjStochVisCost", 0, 0)):
        i = settings.find("<key>%s</key>" % key)
        blk = settings[i:i + 3000] if i >= 0 else ""
        blk = blk[:blk.find("</map>")] if "</map>" in blk else blk
        if i < 0 or not re.search(r"<key>Persist</key>\s*<integer>%d</integer>" % persist, blk) \
                or not re.search(r"<key>Type</key>\s*<string>U32</string>", blk) \
                or not re.search(r"<key>Value</key>\s*<integer>%d</integer>" % default, blk):
            bad.append("settings.xml: %s is not a U32 (Persist %d) defaulting to %d" % (key, persist, default))
    return bad


def _admit(classes, budget):
    """The LIST phase (clean_deferred_light.comp SH_STAGE_MASK) in a random thread order."""
    h = [0, 0, 0, 0]
    for c in classes:
        if c:
            h[c] += 1
    B = min(budget, len(classes))
    thr = 3 if h[3] >= B else (2 if h[3] + h[2] >= B else 1)
    above = 0 if thr == 3 else (h[3] if thr == 2 else h[3] + h[2])
    admit_ctr, listed = 0, []
    order = list(range(len(classes)))
    random.shuffle(order)
    for t in order:
        c = classes[t]
        if c == 0 or c < thr:
            continue
        if c == thr:
            j = admit_ctr
            admit_ctr += 1
            if j >= B - above:
                continue
        listed.append(t)
    return listed, h, B


def _replay():
    bad = []
    rng = random.Random(1515)
    random.seed(1515)
    for _ in range(400):
        n = rng.randint(1, 600)
        classes = [rng.choice((0, 0, 0, 1, 1, 2, 3)) for _ in range(n)]
        budget = rng.randint(1, n + 50)
        listed, h, B = _admit(classes, budget)
        flagged = h[1] + h[2] + h[3]
        if len(listed) > B or len(listed) != min(B, flagged):
            bad.append("(d) listed %d of %d flagged under budget %d" % (len(listed), flagged, B))
            break
        got = [classes[t] for t in listed]
        low = min(got) if got else 4
        if any(classes[t] > low and t not in listed for t in range(n)):
            bad.append("(d) a higher-class tile was left out while a class-%d tile was listed" % low)
            break
    return bad


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as fh:
            return fh.read()
    dl, pdn, dn, dnc = _strip(read(DL)), _strip(read(PDN)), _strip(read(DN)), _strip(read(DNC))
    problems = _structure(dl, pdn, dn, dnc, read(SETTINGS), read(SPV))
    problems += _replay()
    if problems:
        result.fail("\n".join(problems))
        return
    result.summary = "off unchanged, indirect tile list, bounded beside no exact list, budget replay (400 cases) holds"
