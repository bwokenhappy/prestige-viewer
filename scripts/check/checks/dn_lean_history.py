"""Guards for THE LEAN HISTORY (per-pixel memory, 2026-11-12; llcleandenoisevk.h, clean_dn_common.glsl).

The stochastic denoisers' colour history (HIST, binding 3) and the sun / moon channel's (SUNHIST, binding 7) are the
1 x 1 placeholders while DLSS Ray Reconstruction takes that signal raw (the GI's colour, the merged sun -- 32 B per
texel each, ~245 MiB apiece at 3840x2088). Correct only while:
  READS-GATED   every colour-history read is behind dnHistOk (which requires dnColourHist), every sun-history read
                behind dnSunHistOk (which requires dnSunHistImg) or an explicit dnSunHistImg test.
  STORES-GATED  every imageStore to uDnHist / uDnSunHist is behind dnColourHist() / dnSunHistImg() on its line.
  AGES-APART    the ages (what raw frames still read) never live in HIST / SUNHIST (binding 6, AGE).
  SPLIT-AGE     the split slots' ages live in SPLITAGE (binding 8, rgba16ui, 2026-11-13: -16 B per split texel, the
                1 x 1 placeholder without the split channel): every access inside the split channel, none via AGE.
  NARROW-AGE    the GI instance's AGE is r32ui (x alone; -24 B per texel): its DN_AGE_NARROW SPIR-V pair compiles
                the sun / split / mover-record paths (y / z / w) out, and the host picks that pair and format.
  HOST          the denoiser binds 0-4 + 6 + 7; a history-only change re-creates HIST / SUNHIST alone (views kept,
                the GI's apply does not count it as a change: the one-frame-latency GI keeps last frame's image);
                the lean modes come from rrRawSignal through the dwell (leanHistDue), never the radiance projector.
"""
import re

NAME = "dn-lean-history"
STAGE = "source"
DESCRIPTION = "the denoisers' colour / sun history may be absent while RR takes them raw (every access gated)"
GUARDS = ("A read of a 1 x 1 placeholder history (garbage reprojected into the image after RR stops), an age written "
          "into a dropped image, a history-only re-allocation that resets the GI's last frame or set 3.")

SH = "indra/vkclean/shaders/"
DN_H = "indra/newview/vkgfx/llcleandenoisevk.h"
DN = "indra/newview/vkgfx/llcleandenoisevk.cpp"
GI = "indra/newview/vkgfx/llcleangivk.cpp"
PDN = "indra/newview/vkgfx/llcleanprojstochvk.cpp"


def _strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return _strip(f.read())

    common = read(SH + "clean_dn_common.glsl")
    temporal = read(SH + "clean_dn_temporal.comp")
    atrous = read(SH + "clean_dn_atrous.comp")
    bad = []

    # READS-GATED: the predicates
    if "bool dnColourHist() { return all(equal(imageSize(uDnHist).xy, imageSize(uDnGeom).xy)); }" not in common:
        bad.append("READS-GATED: dnColourHist is not HIST-at-the-denoiser-extent")
    if "bool dnSunHistImg() { return all(equal(imageSize(uDnSunHist).xy, imageSize(uDnGeom).xy)); }" not in common:
        bad.append("READS-GATED: dnSunHistImg is not SUNHIST-at-the-denoiser-extent")
    if not re.search(r"bool dnHistOk\(\)\s*\{[^}]*dnColourHist\(\)", common):
        bad.append("READS-GATED: dnHistOk does not require dnColourHist (a lean frame would reproject the placeholder)")
    if not re.search(r"bool dnSunHistOk\(\)\s*\{[^}]*dnSunHistImg\(\)", common):
        bad.append("READS-GATED: dnSunHistOk does not require dnSunHistImg")
    # every colour read sits in a function / branch reached only with a history (histOk / have / radGate): the known
    # sites; a new one must be reviewed and added here
    reads = len(re.findall(r"imageLoad\(uDnHist,", temporal)) + len(re.findall(r"imageLoad\(uDnHist,", atrous))
    if reads != 4:
        bad.append("READS-GATED: %d colour-history reads (expected 4: dnFetchHistory, dnSearchHistory, the drop ring, "
                   "the young-history feedback) -- review the new one's gate and update this check" % reads)
    if not re.search(r"\(fbD \|\| fbS\) && dnColourHist\(\)", atrous):
        bad.append("READS-GATED: the young-history feedback reads / writes HIST without dnColourHist")
    for src, name in ((temporal, "clean_dn_temporal.comp"), (atrous, "clean_dn_atrous.comp")):
        for m in re.finditer(r"[^\n]*imageLoad\(uDnSunHist,[^\n]*", src):
            line = m.group(0)
            if "dnSunHistImg()" in line:
                continue
            # (dnFetchSun: only under `have`, which requires dnSunHistOk)
            fn = src.rfind("bool dnFetchSun(", 0, m.start())
            if fn < 0 or src.find("\n}", fn) < m.start():
                bad.append("READS-GATED: %s reads SUNHIST without dnSunHistImg: %s" % (name, line.strip()))
    if "const bool have = dnSunHistOk() && pw > 0.0 && !dnSunRaw()" not in temporal:
        bad.append("READS-GATED: the sun's history fetch is not behind dnSunHistOk")

    # STORES-GATED
    for src, name in ((temporal, "clean_dn_temporal.comp"), (atrous, "clean_dn_atrous.comp")):
        for m in re.finditer(r"[^\n]*imageStore\(uDnHist,[^\n]*", src):
            if "dnColourHist()" not in m.group(0) and not re.search(r"dnColourHist\(\)\)\s*[^;{]*\{[^}]*$",
                                                                      src[max(0, m.start() - 400):m.start()]):
                bad.append("STORES-GATED: %s stores HIST without dnColourHist: %s" % (name, m.group(0).strip()))
        for m in re.finditer(r"[^\n]*imageStore\(uDnSunHist,[^\n]*", src):
            if "dnSunHistImg()" not in m.group(0):
                bad.append("STORES-GATED: %s stores SUNHIST without dnSunHistImg: %s" % (name, m.group(0).strip()))

    # AGES-APART
    for src, name in ((temporal, "temporal"), (atrous, "atrous"), (common, "common")):
        if re.search(r"uDn(?:Sun)?Hist, ivec3\([^)]*\), dn(?:Split)?AgeLayer", src):
            bad.append("AGES-APART: %s reaches an age layer through HIST / SUNHIST" % name)
    if "layout(set = 1, binding = 7, rgba32ui) uniform uimage2DArray uDnSunHist;" not in common:
        bad.append("AGES-APART: SUNHIST is not binding 7")

    # HOST
    # SPLITAGE (binding 8, rgba16ui; the 1 x 1 placeholder without the split channel): every access inside the split
    # functions (reached only with dnSplitOn) or on a line testing dnSplitOn; never through AGE (no split layers there)
    if "layout(set = 1, binding = 8, rgba16ui) uniform uimage2DArray uDnSplitAge;" not in common:
        bad.append("SPLIT-AGE: SPLITAGE is not binding 8, rgba16ui")
    for src, name, fns in ((temporal, "clean_dn_temporal.comp", ("void dnSplitTemporal(",)),
                           (atrous, "clean_dn_atrous.comp", ("vec4 dnSplitAtrous(",))):
        for m in re.finditer(r"[^\n]*(?:uDnSplitAge|dnSplitAges\()[^\n]*", src):
            line = m.group(0)
            if "dnSplitOn()" in line:
                continue
            inside = False
            for fn in fns:
                at = src.rfind(fn, 0, m.start())
                if at >= 0 and src.find("\n}", at) > m.start():
                    inside = True
            if not inside:
                bad.append("SPLIT-AGE: %s touches SPLITAGE outside the split channel: %s" % (name, line.strip()))
        if re.search(r"uDnAge, ivec3\([^)]*\), dnSplitAgeLayer", src):
            bad.append("SPLIT-AGE: %s reaches the split ages through AGE" % name)

    # THE NARROW AGE (the GI instance: AGE r32ui -- x alone): its variant must compile out every reader / writer of
    # y / z / w (the sun channel, the split channel, the mover record) and the host must pick the variant + format
    narrow = re.search(r"#ifdef DN_AGE_NARROW\s*\nlayout\(set = 1, binding = 6, r32ui\) uniform uimage2DArray uDnAge;",
                       common)
    if not narrow:
        bad.append("NARROW-AGE: the DN_AGE_NARROW variant does not declare AGE r32ui")
    for fn in ("bool dnSunOn()   { return false; }", "bool dnSplitOn()     { return false; }",
               "bool dnMoverRecordOn() { return false; }"):
        if fn not in common:
            bad.append("NARROW-AGE: the narrow variant does not compile out %s" % fn.split("{")[0].strip())

    dn = read(DN)
    if "kDnBinding[kDnImages] = {0u, 1u, 2u, 3u, 4u, 6u, 7u, 8u};" not in dn:
        bad.append("HOST: the denoiser's images are not bound at 0-4, 6, 7, 8")
    if "(i == kDnAgeIdx && narrowAge) ? VK_FORMAT_R32_UINT : kDnImg[i].f" not in dn \
       or "if (i == kDnSplitAgeIdx) return !split;" not in dn:
        bad.append("HOST: AGE's narrow format / SPLITAGE's placeholder rule changed (shader and host must agree)")
    if "mNarrowAge ? dnModule(dev, kWorldDnTemporalGiSpv" not in dn or "mNarrowAge ? dnModule(dev, kWorldDnAtrousGiSpv" not in dn:
        bad.append("HOST: the narrow-age instance does not take the DN_AGE_NARROW SPIR-V pair")
    if "if (mNarrowAge && (sun || split))" not in dn:
        bad.append("HOST: the narrow-age instance can be given a sun / split channel")
    if "mViewsKept = true;" not in dn:
        bad.append("HOST: a history-only ensure() no longer keeps the other images")
    gi = read(GI)
    if 'mGiDn->init(mCtx, mDslDraw0, mDepthSampler, "GI denoiser", true, mViewCount)' not in gi:
        bad.append("HOST: the GI denoiser is not the narrow-age instance (its AGE 32 B per texel, not 8)")
    m = re.search(r"else if \(mGiDn->histMode\(\) != hist\)\s*\{(.*?)\n    \}", gi, flags=re.S)
    if not m or "changed" in m.group(1):
        bad.append("HOST: the GI's history-only change counts as a change (resets set 3 / the latency GI's last frame)")
    # (VRAM round 2: the hold through short non-raw gaps -- leanHistDue's offSince / leanNow / hold arguments)
    if not re.search(r"leanHistDue\(rrRawSignal\(kRrRawGi, w, h\), mGiRawSince, mGiRawOff,", gi):
        bad.append("HOST: the GI's lean history does not follow rrRawSignal through the dwell")
    pdn = read(PDN)
    if not re.search(r"leanHistDue\(rrRawSignal\(kRrRawProj, w0, h0\), mPdnRawSince, mPdnRawOff,", pdn) \
       or not re.search(r"leanHistDue\(sun && rrRawSignal\(kRrRawMergedSun, w0, h0\), mPdnSunRawSince, mPdnSunRawOff,", pdn):
        bad.append("HOST: the projector's lean history does not follow rrRawSignal through the dwell")
    hdr = read("indra/newview/vkgfx/llcleanrenderervk.h")
    # a raw run stays lean; a non-raw gap shorter than the hold keeps it lean, never longer
    if "return leanNow || now - since >= kLeanHistDwell;" not in hdr or "return now - offSince < hold;" not in hdr:
        bad.append("HOST: leanHistDue lost its dwell / its bounded hold")
    if "if (bit == kRrRawProj && mPdnRadiance) return false;" not in hdr:
        bad.append("HOST: rrRawSignal no longer keeps the radiance projector local (its colour history would go)")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "reads / stores gated, ages apart, history-only re-creation keeps the views"
