"""Regression guard: per-light HISTORY can name a light by something stable, not last frame's sorted index (2026-11-14/15).

Lights flickering (Jetstream 1.6.0.26): the light list (llappviewer.cpp) is re-sorted by camera importance every
frame, so a light's INDEX changes whenever two lights trade rank -- a moving camera, a dancer's worn light. Two
histories stored that index and compared it with the next frame's:
  * the local radiance denoiser's MOVER RECORD (clean_dn_temporal.comp, RenderVKShadowCasterHistory 3, the default
    since b77665ff89 in 1.6.0.25): the producer's MOVER REPORT carried light index + 1. After a reorder the record's
    index named another light; that light reaching an avatar's umbra counted as the mover leaving (THE REVEAL: the
    shadow reset to its lit neighbours -- a flash) and the real light's return went unseen. The report now carries
    the light's TAG (pdnMotReportTag: its split id, stable while it is listed, folded to 1..255).
  * the RT shadow mask's slot ids (clean_rt_shadow_mask.comp / clean_rt_mask_temporal.comp, light + 1 since the
    mask's start; the list's importance sort came after): a history slot matched whichever light had that index
    now. Every read of last frame's ids goes through the host's key-matched remap (LightInfo.rs_remap = mRsRemap,
    rtMaskRemapIds); this frame's ids stay indices (clean_deferred_light.comp reads them as such).
Both shipped together in f4bc3342b0 and were reverted with bee5efbe96 (the owner saw worse noise and a smeared avatar
shadow in that build); they came back as an A/B, RenderVKShadowFixLightTag (non-persistent, default 0 = index-keyed): bit 0
-> LightInfo.fwd.z LIGHT_TAG_REPORT (the report's key, clean_deferred_light.comp deferMotReportKey), bit 1 ->
LIGHT_TAG_MASK_REMAP (rtMaskRemapIds remaps only with it). This check fails when either path is undone or loses its
switch, and runs a model of the remap.
"""
import re

NAME = "light-history-keys"
STAGE = "source"
DESCRIPTION = "per-light shadow histories can follow the light across the per-frame re-sort (RenderVKShadowFixLightTag A/B)"
GUARDS = ("Lights / shadows flickering as the camera moves: the mover record and the RT shadow mask's history "
          "matched a light by last frame's importance index.")

SH = "indra/vkclean/shaders/"
MV_VS_TAG = 0x4000


def _strip(src):
    return re.sub(r"//[^\n]*", "", src)


def _remap_id(i, remap):
    """Python twin of clean_rt_mask_common.glsl rtMaskRemapId."""
    if i == 0 or (i & ~(MV_VS_TAG | 0x1FF)) != 0:
        return i
    lp1 = i & 0x1FF
    if lp1 == 0 or lp1 > 256:
        return 0
    li = remap[lp1 - 1]
    return 0 if li == 0xFF else ((i & MV_VS_TAG) | (li + 1))


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    fail = []

    # (a) the mover report: a tag, never an index
    dl = _strip(read(SH + "clean_deferred_light.comp"))
    sets = re.findall(r"repLight = ([^;]+);", dl)
    if not sets:
        fail.append("clean_deferred_light.comp: THE MOVER REPORT (repLight) is gone -- update this check")
    for rhs in sets:
        if rhs.strip() not in ("0u", "sKey", "lKey"):
            fail.append("clean_deferred_light.comp: the mover report takes '%s' -- the record compares it across frames: "
                        "it must come from deferMotReportKey (the light's tag under RenderVKShadowFixLightTag bit 0)"
                        % rhs.strip())
    if "const uint sKey = radOn ? deferMotReportKey(sLi) : 0u;" not in dl \
            or "const uint lKey = radOn ? deferMotReportKey(li) : 0u;" not in dl:
        fail.append("clean_deferred_light.comp: both mover-report sites must key the light through deferMotReportKey")
    km = re.search(r"uint deferMotReportKey\(uint li\)\s*\{(.*?)\n\}", dl, re.S)
    if not km:
        fail.append("clean_deferred_light.comp: deferMotReportKey is gone")
    else:
        body = km.group(1)
        if "(linfo.fwd.z & LIGHT_TAG_REPORT) != 0u" not in body or "pdnMotReportTag(lights[li].tex.w & 0xFFFFu)" not in body:
            fail.append("clean_deferred_light.comp: deferMotReportKey must give the light's tag (pdnMotReportTag of its "
                        "split id) under LIGHT_TAG_REPORT")
        if "return li < 255u ? li + 1u : 0u;" not in body:
            fail.append("clean_deferred_light.comp: deferMotReportKey's off path must be the old index + 1 (li < 255)")
    pc = _strip(read(SH + "clean_pdn_common.glsl"))
    if "uint pdnMotReportTag(uint splitId)" not in pc:
        fail.append("clean_pdn_common.glsl: pdnMotReportTag is gone")
    else:
        # the tag fits the record's 8 bits and the report's, 0 only for no split id
        tags = [0 if s == 0 else ((s - 1) % 255) + 1 for s in range(0, 65536)]
        if tags[0] != 0 or min(tags[1:]) < 1 or max(tags) > 255:
            fail.append("model: pdnMotReportTag leaves 1..255")

    # (b) the RT mask: every read of last frame's ids is remapped
    for fn in ("clean_rt_shadow_mask.comp", "clean_rt_mask_temporal.comp"):
        src = _strip(read(SH + fn))
        if "#define RT_MASK_REMAP_IDS" not in src:
            fail.append("%s: RT_MASK_REMAP_IDS (the id remap) is not enabled" % fn)
        if "uniform LInfo { LightInfo linfo; }" not in src:
            fail.append("%s: no LInfo block (the remap lives in LightInfo.rs_remap)" % fn)
        for mm in re.finditer(r"texelFetch\(uHistIds", src):
            before = src[max(0, mm.start() - 20):mm.start()]
            if "rtMaskRemapIds(" not in before:
                line = src.count("\n", 0, mm.start()) + 1
                fail.append("%s ~line %d (comments stripped): last frame's ids read without rtMaskRemapIds -- they name "
                            "last frame's light indices" % (fn, line))
    rmc = _strip(read(SH + "clean_rt_mask_common.glsl"))
    if "if ((linfo.fwd.z & LIGHT_TAG_MASK_REMAP) == 0u) return ids;" not in rmc:
        fail.append("clean_rt_mask_common.glsl: rtMaskRemapIds must remap only under LIGHT_TAG_MASK_REMAP (the A/B)")
    lc = _strip(read(SH + "clean_lights_common.glsl"))
    if "uvec4 rs_remap[16];" not in lc:
        fail.append("clean_lights_common.glsl: LightInfo.rs_remap is gone")
    if "#define LIGHT_TAG_REPORT     32u" not in lc or "#define LIGHT_TAG_MASK_REMAP 64u" not in lc:
        fail.append("clean_lights_common.glsl: LIGHT_TAG_REPORT (32) / LIGHT_TAG_MASK_REMAP (64) moved -- the host's fwd[2]"
                    " bits must match")
    h = read("indra/newview/vkgfx/llcleanrenderervk.h")
    if "uint32_t rs_remap[64];" not in h or "offsetof(VKLightInfo, fx_vp_v) == 656" not in h:
        fail.append("llcleanrenderervk.h: VKLightInfo must mirror LightInfo.rs_remap (ending at 656 B, before the "
                    "#1608 stereo froxel tail)")
    cpp = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    if "std::memcpy(mLightInfoCPU.rs_remap, mRsRemap, sizeof(mRsRemap));" not in cpp:
        fail.append("llcleanrenderervk.cpp: the light info no longer carries mRsRemap")
    if '"RenderVKShadowFixLightTag", 0U' not in cpp \
            or "((lightTagFix & 1u) != 0u ? 32u : 0u) | ((lightTagFix & 2u) != 0u ? 64u : 0u)" not in cpp:
        fail.append("llcleanrenderervk.cpp: RenderVKShadowFixLightTag no longer sets fwd[2] bits 5 / 6")
    st = read("indra/newview/app_settings/settings.xml")
    i = st.find("<key>RenderVKShadowFixLightTag</key>")
    blk = st[i:st.find("</map>", i)] if i >= 0 else ""
    if "<key>Persist</key><integer>0</integer>" not in blk or "<key>Value</key><integer>0</integer>" not in blk:
        fail.append("settings.xml: RenderVKShadowFixLightTag must be non-persistent, default 0 (an A/B)")

    # (c) a model: a list reorder keeps each light's slot history, a dropped light loses it, specials pass
    prev = [101, 102, 103, 104]            # last frame's keys at indices 0..3
    cur = [103, 101, 104, 105]             # this frame: 102 dropped, 105 new, the rest re-sorted
    remap = [0xFF] * 256
    for p, k in enumerate(prev):
        if k in cur:
            remap[p] = cur.index(k)
    for p, k in enumerate(prev):
        got = _remap_id(p + 1, remap)
        want = cur.index(k) + 1 if k in cur else 0
        if got != want:
            fail.append("model: last frame's id %d (key %d) remaps to %d, want %d" % (p + 1, k, got, want))
        if _remap_id(MV_VS_TAG | (p + 1), remap) != ((MV_VS_TAG | want) if want else 0):
            fail.append("model: the mover Vs tag is not kept through the remap")
    for special in (0xFFFF, 0xFFFB, 0xFFFA, 0):
        if _remap_id(special, remap) != special:
            fail.append("model: special id 0x%X changed by the remap" % special)

    for f in fail:
        result.fail(f)
