"""Regression guard for VR stereo parity (#1608): every rendering family released for layered stereo declares its
per-view resources.

Layered stereo (#1075) renders one image per eye in ONE recording: screen targets are N-layer images (one layer
per view), raster passes go multiview, compute passes dispatch z = N with the view = gl_WorkGroupID.z
(clean_view.glsl CLEAN_VIEW_Z), and a kernel's stereo pipeline carries kViewCount = N (spec constant 20). A family
that leaves the "off in stereo" list (LLCleanRendererVK::initViewCount's log line) must have all of these, or one
eye silently shows the other eye's image (a layer-0 read for both views), the reflections double or swim (one
eye's history reprojected into the other), or eye 1's pixels are traced from eye 0's camera (a kernel whose
pipeline has kViewCount 1 folds CLEAN_VIEW to 0).

Per family, pinned here:
  * the per-view image: created with createScreenImage(..., mViewCount, ...) and its 2D_ARRAY view (addArrayView),
    bound through arrayView -- never createImage2D (one layer: both eyes would share it);
  * the shader declaration arrayed (image2DArray / sampler2DArray) and every access taking the view's layer;
  * the kernel's pipeline specialized with the view count (litDebugSpec(.., mViewCount) / viewCountSpec(mViewCount))
    and its dispatch z = mViewCount;
  * the stereo gate gone from the family's decision, and the family named as layered (not off) in the log line.
On the tree before #1608 the RT reflection pre-trace target is a 2D image, the kernel is not layered and the split
gate keeps the pre-trace off in stereo: the check fails.
"""
import re

NAME = "vr-stereo-parity"
STAGE = "source"
DESCRIPTION = "each rendering family released for VR layered stereo declares per-view images, layers and pipelines"
GUARDS = ("#1608: a family released for layered stereo with a one-layer image, a layer-0 read, a kernel without "
          "kViewCount = N or a z = 1 dispatch -- one eye showing the other's reflections / history, or traced from "
          "the other eye's camera -- and an \"off in stereo\" log line that no longer says what is off.")

_SH = "indra/vkclean/shaders/"
_CPP = "indra/newview/vkgfx/llcleanrenderervk.cpp"


def _func(text, head):
    i = text.find(head)
    if i < 0:
        return ""
    j = text.find("\n}\n", i)
    return text[i:j] if j > 0 else text[i:]


def _read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def _log_line(cpp):
    """The stereo-off log text of initViewCount (the string literals joined)."""
    fn = _func(cpp, "void LLCleanRendererVK::initViewCount()")
    i = fn.find("Off in stereo until")
    if i < 0:
        return ""
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', fn[i - 40:]))


def _rt_reflections(ctx, cpp, sh):
    """RT reflections in stereo: the uber kernel's RT pre-trace (pass 12) per view."""
    m = []
    fn = _func(cpp, "bool LLCleanRendererVK::ensureRtUber(")
    if fn.count("createScreenImage(") < 2 or "createImage2D(" in fn:
        m.append("ensureRtUber: mRtUber (full and placeholder) is not a screen image of mViewCount layers")
    if fn.count("mViewCount") < 2 or fn.count("addArrayView(mRtUber") < 2:
        m.append("ensureRtUber: mRtUber is not created per view with its 2D_ARRAY view")
    if "mRtUber.arrayView, VK_IMAGE_LAYOUT_GENERAL" not in fn:
        m.append("ensureRtUber: set 2 binding 11 is not written with mRtUber's 2D_ARRAY view")
    pt = _func(cpp, "void LLCleanRendererVK::recordRtPreTrace(")
    if "if (stereoActive()) releaseTlas(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);" not in pt:
        m.append("recordRtPreTrace: no TLAS release on the lighting's queue in stereo (VR lights on the async queue)")
    if "VK_REMAINING_ARRAY_LAYERS" not in pt:
        m.append("recordRtPreTrace: the WAR barrier does not cover every view's layer")
    if not re.search(r"vkCmdDispatch\(cmd, \(p\.width \+ 7u\) / 8u, \(p\.height \+ 7u\) / 8u, mViewCount\);", pt):
        m.append("recordRtPreTrace: the pre-trace is not dispatched over the views (z = mViewCount)")
    if not re.search(r"litDebugSpec\(false, mViewCount\), &mRtPreTracePipe", cpp):
        m.append("mRtPreTracePipe is not specialized with the view count (eye 1 would trace eye 0's camera)")
    sf = _func(cpp, "void LLCleanRendererVK::deferSplitFrame(")
    if "const bool uberWant = rtrWant && deferred && !mDeferSplitNow && mRtPreTracePipe;" not in sf:
        m.append("deferSplitFrame: a stereo uber frame (VR GI) does not take the layered RT pre-trace")
    if not re.search(r"deferSplitFrame\(deferNow && !mDeferDbgNow, rw, rh\)", cpp):
        m.append("recordWorld: deferSplitFrame is still told a stereo frame is not deferred (no uber pre-trace)")
    if "uniform readonly image2DArray uRtUber;" not in sh:
        m.append("clean_deferred_light.comp: the uber kernel's uRtUber is not image2DArray")
    if "uniform writeonly image2DArray uRtUber;" not in sh:
        m.append("clean_deferred_light.comp: pass 12's uRtUber is not image2DArray")
    if "imageLoad(uRtUber, ivec3(ivec2(CLEAN_FRAG_XY), DEFER_LAYER))" not in sh:
        m.append("clean_deferred_light.comp: deferRtIn does not read its view's layer")
    if "imageStore(uRtUber, ivec3(px, DEFER_LAYER), outv);" not in sh:
        m.append("clean_deferred_light.comp: pass 12 does not write its view's layer")
    return m


def _mirrors(ctx, cpp, sh):
    """Mirrors in stereo: the RT mirror (pass 12 -DCLEAN_RT_MIRROR) per view; the planar capture stays off."""
    m = []
    em = _func(cpp, "bool LLCleanRendererVK::ensureMirror(")
    if not re.search(r"nc = mCtx->createScreenImage\(w, h, mViewCount,", em) or "addArrayView(nc" not in em:
        m.append("ensureMirror: the mirror colour is not a screen image of mViewCount layers with its 2D_ARRAY view")
    if em.count("VK_REMAINING_ARRAY_LAYERS") < 2:
        m.append("ensureMirror: the initial transition / clear do not cover every view's layer")
    if "mMirrorColor.arrayView," not in _func(cpp, "void LLCleanRendererVK::writeMirrorDescriptor("):
        m.append("writeMirrorDescriptor: draw set 0 binding 17 is not the mirror's 2D_ARRAY view")
    if "mMirrorColor.arrayView," not in _func(cpp, "void LLCleanRendererVK::writeRtMirrorDescriptor("):
        m.append("writeRtMirrorDescriptor: the RT mirror kernel's set 4 is not the mirror's 2D_ARRAY view")
    rm = _func(cpp, "void LLCleanRendererVK::recordRtMirror(")
    if "if (!onAsync || stereoActive()) releaseTlas(" not in rm:
        m.append("recordRtMirror: no TLAS release on the async lighting queue in stereo (VR lights there)")
    if not re.search(r"vkCmdDispatch\(cmd, \(p\.width \+ 7u\) / 8u, \(p\.height \+ 7u\) / 8u, mViewCount\);", rm):
        m.append("recordRtMirror: the trace is not dispatched over the views (z = mViewCount)")
    if not re.search(r"litDebugSpec\(false, mViewCount\), &mRtMirrorPipe", cpp):
        m.append("mRtMirrorPipe is not specialized with the view count (eye 1 would trace eye 0's camera)")
    if "if (s_rtm || stereoActive())" not in _func(cpp, "bool LLCleanRendererVK::rtMirrorModeWanted("):
        m.append("rtMirrorModeWanted: a stereo mirror is not always ray traced")
    if "if (mMirrorOn && stereoActive() && !mMirrorRtMode) mMirrorOn = false;" not in cpp:
        m.append("recordWorld: a stereo frame could take the planar capture (one reflected view for both eyes)")
    sm = _func(cpp, "void LLCleanRendererVK::setMirror(")
    if "mFrameSlot < MIRROR_OCC_SLOTS && !stereoActive();" not in sm:
        m.append("setMirror: the mirror occlusion query is not off in stereo (a multiview scope takes N slots)")
    if re.search(r"mProbeDrawPipe\s*\n\s*&& !stereoActive\(\)", sm):
        m.append("setMirror: mirrors are still off in stereo")
    if "mRtMirrorNow = rtMirrorFrameReady();" not in cpp:
        m.append("the frame info fill keeps the RT mirror off in stereo")
    if "uniform writeonly image2DArray uRtMirror;" not in sh \
            or "imageStore(uRtMirror, ivec3(px, DEFER_LAYER)," not in sh:
        m.append("clean_deferred_light.comp: the RT mirror kernel does not write its view's layer (image2DArray)")
    dc = _read(ctx, _SH + "clean_draw_common.glsl")
    if "uniform sampler2DArray uMirror;" not in dc \
            or "textureLod(uMirror, vec3(uv, float(CLEAN_VIEW)), 0.0)" not in dc:
        m.append("clean_draw_common.glsl: tapMirror does not read its view's layer of uMirror (sampler2DArray)")
    return m


def _ssr(ctx, cpp, sh):
    """SSR in stereo: the march reads its own view's layer of the previous colour / depth (draw set 0 b7 / b8)."""
    m = []
    dc = _read(ctx, _SH + "clean_draw_common.glsl")
    for decl in ("uniform sampler2DArray uPrevColor;", "uniform sampler2DArray uPrevDepth;"):
        if decl not in dc:
            m.append("clean_draw_common.glsl: not declared: " + decl)
    if "texelFetch(uPrevDepth, ivec3(px, int(CLEAN_VIEW)), 0)" not in dc:
        m.append("clean_draw_common.glsl: ssrPrevViewPos does not read its view's depth layer")
    if "textureLod(uPrevColor, vec3((floor(hiTc * sz) + 0.5) / sz, float(CLEAN_VIEW))" not in dc:
        m.append("clean_draw_common.glsl: ssrTrace does not read its view's colour layer")
    if re.search(r"texelFetch\(uPrevDepth, px,|textureLod\(uPrevColor, \(", dc):
        m.append("clean_draw_common.glsl: an SSR read without a layer (layer 0 for both eyes)")
    if "ssr.views[CLEAN_VIEW].prev_proj * vec4(pos, 1.0)" not in dc:
        m.append("clean_draw_common.glsl: ssrProject does not project through its view's previous matrices")
    ws = _func(cpp, "void LLCleanRendererVK::writeSSRDescriptors(")
    if "mPrevColor.arrayView," not in ws or "mPrevDepth.arrayView," not in ws:
        m.append("writeSSRDescriptors: b7 / b8 are not the previous colour / depth's 2D_ARRAY views")
    if "mCtx->addArrayView(mPrevColor, VK_IMAGE_ASPECT_COLOR_BIT);" not in cpp \
            or "mCtx->addArrayView(mPrevDepth, VK_IMAGE_ASPECT_DEPTH_BIT);" not in cpp:
        m.append("the previous colour / depth get no 2D_ARRAY view at one view")
    if re.search(r"specApply\(SPEC_SSR, \(ssr_on[^)]*stereoActive", cpp):
        m.append("the frame info keeps SSR off in stereo")
    if "for (uint32_t v = 0; v < mViewCount; ++v)\n    {\n        std::memcpy(mPrevViewV[v]" not in cpp:
        m.append("the SSR copy does not commit every view's previous matrices")
    return m


def _split(ctx, cpp, sh):
    """The split deferred lighting in stereo (SHADOW / REFLECT / COMBINE + the light grid): every intermediate a layer
    per view, every kernel kViewCount = N and z = N, the visibility-reuse readers at their view's layer."""
    m = []
    et = _func(cpp, "bool LLCleanRendererVK::ensureDeferSplitTargets(")
    if "mCtx->createScreenImage(ew[i], eh[i], mViewCount," not in et or "addArrayView(mDsImg[i]" not in et:
        m.append("ensureDeferSplitTargets: mDsImg is not a screen image of mViewCount layers with its 2D_ARRAY view")
    if "layers * mViewCount, 1, VK_FORMAT_R32_UINT" not in et:
        m.append("ensureDeferSplitTargets: the projector slots do not hold every plane once per view")
    if "mDsImg[DS_SUN].arrayView" not in et or "mDsImg[DS_VDEPTH].arrayView" not in et or "mDsImg[DS_HALF].arrayView" not in et:
        m.append("ensureDeferSplitTargets: set 3 / draw set 0 b41-43 do not name the intermediates' 2D_ARRAY views")
    for fn in ("bool LLCleanRendererVK::ensureDeferSplitTargets(", "bool LLCleanRendererVK::deferSplitTargetsMatch("):
        if "mAlias.enabled() && mScaled && !stereoActive()" not in _func(cpp, fn):
            m.append(fn.split("::")[1] + ": the intermediates can take the one-layer alias arena in stereo")
    if not re.search(r"litDebugSpec\(false, mViewCount\), &mDeferSplitPipe\[i\]", cpp):
        m.append("mDeferSplitPipe is not specialized with the view count (eye 1 would light from eye 0's camera)")
    if "litDebugSpec(false, mViewCount),\n                              &mLightGridPipe[1]" not in cpp:
        m.append("mLightGridPipe[1] (the split's light grid) is not specialized with the view count")
    sp = _read(ctx, "indra/newview/vkgfx/llcleanspecvk.cpp")
    for k in ("SM_DSHADOW", "SM_DREFLECT", "SM_DCOMBINE", "SM_DREFLECT_HALF", "SM_DREFLECT_SHARP"):
        if ("s->mods[%s], mDeferSplitLayout, 0, s->layeredInfo()," % k) not in sp:
            m.append("llcleanspecvk.cpp: the spec set's %s pipeline is not layered on a stereo key" % k)
    rd = _func(cpp, "void LLCleanRendererVK::recordDeferredLighting(")
    i = rd.find("    if (mDeferSplitNow)\n")
    plain = rd[i:] if i >= 0 else ""
    for d in ("vkCmdDispatch(cmd, tilesX, tilesY, mViewCount);",
              "vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, mViewCount);",
              "vkCmdDispatch(cmd, (hw + 7) / 8, (hh + 7) / 8, mViewCount);",
              "vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, mViewCount);"):
        if d not in plain:
            m.append("recordDeferredLighting (plain split): not dispatched over the views: " + d[:60])
    dc = _func(cpp, "void LLCleanRendererVK::recordDeferCombine(")
    if "vkCmdDispatch(cmd, (rw + 15) / 16, (rh + 15) / 16, mViewCount);" not in dc:
        m.append("recordDeferCombine: COMBINE is not dispatched over the views")
    if "vrPts ? mDeferCombineVrPipe : deferSplitPipe(DSP_COMBINE)" not in dc:
        m.append("recordDeferCombine: a stereo frame with VR point shadows does not bind the COMBINE's stereo twin")
    if "kWorldDeferCombineVrSpv" not in cpp or "litDebugSpec(false, mViewCount), &mDeferCombineVrPipe" not in cpp:
        m.append("mDeferCombineVrPipe (the COMBINE's VR point-shadow twin) is not built layered")
    sf = _func(cpp, "void LLCleanRendererVK::deferSplitFrame(")
    if "if (mGiVrNow) stereoWhy |= 2u;" not in sf or "if (mDeferCombineVrPipe == VK_NULL_HANDLE) stereoWhy |= 4u;" not in sf:
        m.append("deferSplitFrame: a stereo frame with VR GI / without the COMBINE's VR twin can take the split")
    pf = _func(_read(ctx, "indra/newview/vkgfx/llcleanprojstochvk.cpp"), "void LLCleanRendererVK::pdnFrame(")
    if "sample && vrStochShadows() &&" not in pf:   # (#1608 round 4: pass 15 per view -- the "merged RT shadows / ReSTIR DI" family)
        m.append("pdnFrame: a stereo split frame samples pass 15 without the SlipstreamVRPointShadows choice (vrStochShadows)")
    for decl in ("image2DArray uVisSun;", "uimage2DArray uReflIrr;", "uimage2DArray uReflSpec;", "uimage2DArray uReflEnv;",
                 "uimage2DArray uReflHalf;", "uimage2DArray uVisDepth;"):
        if decl not in sh:
            m.append("clean_deferred_light.comp: not declared per view: " + decl)
    if "#define DS_TEXEL(p) ivec3((p), DEFER_LAYER)" not in sh \
            or "#define DS_PROJ_TEXEL(p, plane) ivec3((p), int(plane) * kViewCount + DEFER_LAYER)" not in sh:
        m.append("clean_deferred_light.comp: DS_TEXEL / DS_PROJ_TEXEL (the view's layer) missing")
    if re.search(r"image(Load|Store)\(u(VisSun|ReflIrr|ReflSpec|ReflEnv|ReflHalf|VisDepth), (px|hp|ivec2\()", sh) \
            or re.search(r"image(Load|Store)\(uVisProj, ivec3\(px, [0-3]\)", sh):
        m.append("clean_deferred_light.comp: a split intermediate access without the view's layer")
    if not re.search(r"\|\| CLEAN_DEFER_PASS == 3( \|\| CLEAN_DEFER_PASS == 8)?\)\n#error \"CLEAN_VR_POINT_SHADOWS builds", sh):
        m.append("clean_deferred_light.comp: CLEAN_VR_POINT_SHADOWS does not build the COMBINE's stereo twin")
    dcg = _read(ctx, _SH + "clean_draw_common.glsl")
    for decl in ("uniform readonly image2DArray uVisReuseSun;", "uniform readonly uimage2DArray uVisReuseDepth;"):
        if decl not in dcg:
            m.append("clean_draw_common.glsl: the visibility reuse is not per view: " + decl)
    oit = _read(ctx, _SH + "clean_oit_resolve.frag")
    if re.search(r"imageLoad\(uVisReuse(Depth|Sun), ivec2\(", dcg + oit):
        m.append("clean_draw_common.glsl / clean_oit_resolve.frag: a visibility-reuse read at layer 0 for both eyes")
    if "ivec3((p), int(plane) * kViewCount + int(CLEAN_VIEW))" not in dcg or "uVisReuseProj, ivec3(px," in dcg:
        m.append("clean_draw_common.glsl: the visibility reuse's projector slots are not read at the view's layers")
    if dcg.find("#if defined(CLEAN_VR_POINT_SHADOWS)") < 0 \
            or dcg.find("#if defined(CLEAN_VR_POINT_SHADOWS)") > dcg.find("#elif defined(CLEAN_PROJ_STOCH_LIT)"):
        m.append("clean_draw_common.glsl: localLight tests ReSTIR DI's hook before the VR point shadows' (the COMBINE "
                 "twin compiles both)")
    return m


def _feature_split(ctx, cpp, sh):
    """The feature split in stereo (CLASSIFY / SHADOW_CLS / BASE z = N; LOCAL over ONE list of every view's tiles,
    its view from the listed tile -- shape B; the gain a layer per view; LOCAL's VR point-shadow twin)."""
    m = []
    et = _func(cpp, "bool LLCleanRendererVK::ensureDeferSplitTargets(")
    if "((rw + 15u) / 16u) * ((rh + 15u) / 16u) * mViewCount" not in et:
        m.append("ensureDeferSplitTargets: the tile list does not hold every view's tiles")
    if "mDsGain = mCtx->createScreenImage(" not in et or "addArrayView(mDsGain" not in et \
            or "mDsGain.arrayView}" not in et:
        m.append("ensureDeferSplitTargets: the local gain is not a layer per view bound through its 2D_ARRAY view")
    if not re.search(r"litDebugSpec\(false, mViewCount\), &mDeferFeatPipe\[i\]", cpp):
        m.append("mDeferFeatPipe is not specialized with the view count")
    sp = _read(ctx, "indra/newview/vkgfx/llcleanspecvk.cpp")
    for k in ("SM_DBASE", "SM_DLOCAL", "SM_DSHADOW_CLS"):
        if not re.search(r"s->mods\[%s\], mDeferSplitLayout, (0|deferTileStageFlags\(\)), s->layeredInfo\(\)" % k, sp):
            m.append("llcleanspecvk.cpp: the spec set's %s pipeline is not layered on a stereo key" % k)
    rd = _func(cpp, "void LLCleanRendererVK::recordDeferredLighting(")
    i, j = rd.find("    if (feat)\n"), rd.find("    if (mDeferSplitNow)\n")
    feat = rd[i:j] if 0 <= i < j else ""
    for d in ("vkCmdDispatch(cmd, tilesX, tilesY, mViewCount);",
              "vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, mViewCount);",
              "vkCmdDispatch(cmd, (hw + 7) / 8, (hh + 7) / 8, mViewCount);"):
        if d not in feat:
            m.append("recordDeferredLighting (feature split): not dispatched over the views: " + d[:60])
    if feat.count("tilesX, tilesY, mViewCount)") < 2:
        m.append("recordDeferredLighting (feature split): CLASSIFY / SHADOW_CLS not both dispatched over the views")
    dc = _func(cpp, "void LLCleanRendererVK::recordDeferCombine(")
    if "vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, mViewCount);" not in dc:
        m.append("recordDeferCombine: BASE is not dispatched over the views")
    if "vrPtsL ? mDeferLocalVrPipe : deferFeatPipe(DFP_LOCAL)" not in dc:
        m.append("recordDeferCombine: a stereo frame with VR point shadows does not bind LOCAL's stereo twin")
    if "kWorldDeferLocalVrSpv" not in cpp or "litDebugSpec(false, mViewCount), &mDeferLocalVrPipe" not in cpp:
        m.append("mDeferLocalVrPipe (LOCAL's VR point-shadow twin) is not built layered")
    sf = _func(cpp, "void LLCleanRendererVK::deferSplitFrame(")
    if "featSplit && mDeferFeatOk && featStereoOk" not in sf or "(mDeferLocalVrPipe != VK_NULL_HANDLE && featTilesAll <=" not in sf:
        m.append("deferSplitFrame: the feature split's stereo want does not need LOCAL's VR twin")
    if "uniform writeonly image2DArray uLocalGain;" not in sh or "uniform readonly image2DArray uLocalGain;" not in sh \
            or "imageStore(uLocalGain, DS_TEXEL(px)" not in sh or "imageLoad(uLocalGain, DS_TEXEL(px))" not in sh:
        m.append("clean_deferred_light.comp: the local gain is not read / written at the view's layer")
    if "#define CLEAN_VIEW (kViewCount > 1 ? g_local_view : 0u)" not in sh \
            or "g_local_view = kViewCount > 1 ? tileRow / tilesY : 0u;" not in sh:
        m.append("clean_deferred_light.comp: LOCAL does not take its view from the listed tile (eye 1's tiles lit as eye 0)")
    if "|| CLEAN_DEFER_PASS == 8)\n#error \"CLEAN_VR_POINT_SHADOWS builds" not in sh:
        m.append("clean_deferred_light.comp: CLEAN_VR_POINT_SHADOWS does not build LOCAL's stereo twin")
    return m


def _skin_sss(ctx, cpp, sh):
    """Skin scattering in stereo: the irradiance + the separable intermediate a layer per view, pass 17 z = N."""
    m = []
    op = _func(cpp, "bool LLCleanRendererVK::ensureDeferSplitOpt(")
    if op.count("createScreenImage(") < 2 or op.count("addArrayView(img") < 2 or "createImage2D(" in op:
        m.append("ensureDeferSplitOpt: the skin scattering images are not screen images of mViewCount layers")
    if "VK_REMAINING_ARRAY_LAYERS" not in op:
        m.append("ensureDeferSplitOpt: the initial transition does not cover every view's layer")
    et = _func(cpp, "bool LLCleanRendererVK::ensureDeferSplitTargets(")
    if "mDsSss.arrayView" not in et or "mDsSssTmp.arrayView" not in et:
        m.append("ensureDeferSplitTargets: set 3 b30 / b32 do not name the skin images' 2D_ARRAY views")
    if "return on;   // (#1608" not in _func(cpp, "bool LLCleanRendererVK::skinSssWanted("):
        m.append("skinSssWanted: skin scattering is still off in stereo")
    rs = _func(cpp, "void LLCleanRendererVK::recordSkinSss(")
    if rs.count(", mViewCount);") < 2:
        m.append("recordSkinSss: the two passes are not dispatched over the views (z = mViewCount)")
    for k in ("&mSkinSssPipe[0]", "&mSkinSssPipe[1]"):
        if ("litDebugSpec(false, mViewCount), " + k) not in cpp:
            m.append("mSkinSssPipe is not specialized with the view count: " + k)
    for decl in ("uniform uimage2DArray uSssDiff;", "uniform readonly uimage2DArray uSssDiff;",
                 "uniform writeonly uimage2DArray uSssDiff;", "uniform readonly uimage2DArray uSssTmp;",
                 "uniform writeonly uimage2DArray uSssTmp;"):
        if decl not in sh:
            m.append("clean_deferred_light.comp: not declared per view: " + decl)
    if "ivec3 sssTmpAt(ivec2 q, int w) { return ivec3(2 * q.x + w, q.y, DEFER_LAYER); }" not in sh:
        m.append("clean_deferred_light.comp: sssTmpAt does not address the view's layer")
    if re.search(r"image(Load|Store)\(uSss(Diff|Tmp), (?!DS_TEXEL|sssTmpAt)", sh):
        m.append("clean_deferred_light.comp: a skin scattering access without the view's layer")
    return m


def _split_rt_reflections(ctx, cpp, sh):
    """The split's RT reflections in stereo (mark / select / trace / the #1036 split + the queued stages / resolve /
    forward): ONE tile / ray / forward list over every view's tiles whose entries name their view (shape B: each list
    kernel sets g_rt_view first), the mark / trace / forward targets a layer per view, the temporal history 4 x N layers,
    every kernel kViewCount = N, the forward selection z = N, one frame budget over every view's pixels."""
    m = []
    et = _func(cpp, "bool LLCleanRendererVK::ensureDeferSplitTargets(")
    if "((rw + 7u) / 8u) * ((rh + 7u) / 8u) * mViewCount : 1u;" not in et:
        m.append("ensureDeferSplitTargets: the RT tile list does not hold every view's tiles")
    if "((fw + 7u) / 8u) * ((fh + 7u) / 8u) * mViewCount : 1u;" not in et:
        m.append("ensureDeferSplitTargets: the RT forward tile list does not hold every view's tiles")
    if "mCtx->createScreenImage(iw, ih, mViewCount," not in et or "addArrayView(g, VK_IMAGE_ASPECT_COLOR_BIT)" not in et:
        m.append("ensureDeferSplitTargets: the RT mark / trace / forward targets are not screen images of mViewCount layers")
    for img in ("mDsRtMark = screen(", "mDsRtTrace = screen(", "mDsRtFwd = screen("):
        if img not in et:
            m.append("ensureDeferSplitTargets: not a layer per view: " + img[:-9])
    if et.count("4u * mViewCount, 1") < 2:
        m.append("ensureDeferSplitTargets: the RT temporal history (full and placeholder) is not 4 x N layers")
    if "{mDsRtMark.arrayView, VK_NULL_HANDLE, mDsRtTrace.arrayView, mDsRtFwd.arrayView}" not in et:
        m.append("ensureDeferSplitTargets: set 3 b9 / b11 / b12 do not name the RT targets' 2D_ARRAY views")
    for fn in ("bool LLCleanRendererVK::ensureDeferSplitTargets(", "bool LLCleanRendererVK::deferSplitTargetsMatch("):
        if "((rw + 7u) / 8u) * ((rh + 7u) / 8u) * mViewCount)" not in _func(cpp, fn):
            m.append(fn.split("::")[1] + ": the RT tile count compare is not every view's (a re-creation every frame)")
    if "mDsRtFwd.arrayView) ? mDsRtFwd.arrayView : mRtFwdPlaceholder.arrayView" not in _func(cpp, "void LLCleanRendererVK::writeRtFwdBinding("):
        m.append("writeRtFwdBinding: draw set 0 b48 is not the forward target's / placeholder's 2D_ARRAY view")
    ph = _func(cpp, "void LLCleanRendererVK::ensureRtPlaceholders(")
    if "mRtFwdPlaceholder = mCtx->createScreenImage(1u, 1u, mViewCount," not in ph or "addArrayView(mRtFwdPlaceholder" not in ph:
        m.append("ensureRtPlaceholders: the forward placeholder is not a layer per view (eye 1 reads past layer 0)")
    mi = _func(cpp, "void LLCleanRendererVK::recordRtMarkInit(")
    if mi.count("VK_REMAINING_ARRAY_LAYERS") < 4:
        m.append("recordRtMarkInit: a barrier / clear does not cover every view's layer")
    if "* (double)mDsRtMark.layers;" not in mi:
        m.append("recordRtMarkInit: the ray budget is not one frame budget over every view's pixels")
    if "* (double)mViewCount * frac);" not in _func(cpp, "void LLCleanRendererVK::rtSplitDecide("):
        m.append("rtSplitDecide: the split trace's records are not sized for every view's rays")
    if "if (stereoActive()) releaseTlas(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);\n    // (#1039 M6 LEVELS)" \
            not in _func(cpp, "void LLCleanRendererVK::recordRtReflect("):
        m.append("recordRtReflect: no TLAS release on the lighting's queue in stereo (VR lights on the async queue)")
    fs = _func(cpp, "void LLCleanRendererVK::recordRtReflectFwdSel(")
    if "vkCmdDispatch(cmd, (hw + 7u) / 8u, (hh + 7u) / 8u, mViewCount);" not in fs:
        m.append("recordRtReflectFwdSel: the forward selection is not dispatched over the views")
    for pipe in ("&mRtResolvePipe", "&mRtTracePipe", "&mRtSelectPipe", "&mRtTraceListPipe", "&mRtSplitPipe[i]",
                 "&mRtFwdPipe", "&mRtFwdSelPipe"):
        if not re.search(r"litDebugSpec\(false, mViewCount\), " + re.escape(pipe), cpp):
            m.append(pipe[1:] + " is not specialized with the view count (eye 1 traced from eye 0's camera)")
    if not re.search(r"constexpr uint32_t kRtBcLayered = \(1u << 0u\) \| \(1u << 1u\) \| \(1u << 3u\) \|[^;]*\(1u << 10u\);", cpp) \
            or "(kRtBcLayered & (1u << k)) != 0u ? mViewCount : 1u" not in cpp:
        m.append("the hang beacon's twins of the layered RT kernels are not built with the view count")
    vq = _read(ctx, "indra/newview/vkgfx/llcleanvisqueuevk.cpp")
    if "vqLitSpec(s.set != 7u ? mViewCount : 1u)" not in vq:   # (#1608 round 4: + the GI hits' stages)
        m.append("llcleanvisqueuevk.cpp: the RT reflections' queued stages are not built with the view count")
    if "fb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};" not in vq:
        m.append("llcleanvisqueuevk.cpp: the M6 wave's forward-target barrier does not cover every view's layer")
    sf = _func(cpp, "void LLCleanRendererVK::deferSplitFrame(")
    if "stereoWhy |= 1u" in sf:
        m.append("deferSplitFrame: a stereo frame with RT reflections still lights with the uber kernel")
    for decl in ("uniform writeonly uimage2DArray uReflRtMark;", "uniform uimage2DArray uReflRtMark;",
                 "uniform readonly uimage2DArray uReflRtMark;", "uniform writeonly image2DArray uRtTrace;",
                 "uniform readonly image2DArray uRtTrace;", "uniform image2DArray uRtFwd;"):
        if decl not in sh:
            m.append("clean_deferred_light.comp: not declared per view: " + decl)
    if re.search(r"image(Load|Store)\(u(ReflRtMark|RtTrace|RtFwd), (?!DS_TEXEL)", sh) \
            or re.search(r"image(Load|Store)\(uRtHist, (?!RTH_TEXEL)", sh):
        m.append("clean_deferred_light.comp: an RT reflection target access without the view's layer")
    if "#define RTH_TEXEL(p, k) ivec3((p), int(k) * kViewCount + DEFER_LAYER)" not in sh:
        m.append("clean_deferred_light.comp: RTH_TEXEL (history layer k of the view) missing")
    if "#define CLEAN_VIEW (kViewCount > 1 ? g_rt_view : 0u)" not in sh:
        m.append("clean_deferred_light.comp: the RT list kernels do not take their view from the entry (g_rt_view)")
    for k in ("const uvec2 org = rttTileOrg(tile, g_rt_view);", "rttTileOrg(tile, g_rt_view) + gl_LocalInvocationID.xy",
              "g_rt_view = rttRayView(e);", "g_rt_view = frow / rftTilesY();",
              "const uint t = rttTileId(gl_WorkGroupID.xy);", "rtt_words[fb + 8u + i] = gl_WorkGroupID.x | (row << 16);",
              "rtt_words[rb + 7u + s_rtl_base + lid] = rttRayEntry(px);"):
        if k not in sh:
            m.append("clean_deferred_light.comp: a list's view numbering missing: " + k)
    if re.search(r"rtt_words\[uint\((px|q)\.y >> 3\) \* tilesX", sh):
        m.append("clean_deferred_light.comp: an RT tile flag read without the view's numbering (rttTileId)")
    dc = _read(ctx, _SH + "clean_draw_common.glsl")
    if "uniform sampler2DArray uRtFwd;" not in dc or dc.count("int(CLEAN_VIEW)), 0)") < 3:
        m.append("clean_draw_common.glsl: rtFwdIn / rtFwdGiIn do not read their view's layer of uRtFwd")
    return m


def _ao(ctx, cpp, sh):
    """Ambient occlusion in stereo (SSAO / GTAO / RTAO, the depth MIP chain, the temporal accumulator, the blurs): every
    AO image a layer per view bound through its 2D_ARRAY view, every kernel kViewCount = N and z = N, each view's
    unprojection (the frame info's cur_inv_proj -- the push holds view 0's) and eye (eye_rel), G1 normals, the
    consumers (draw set 0 b21) at their view's layer."""
    m = []
    h = _read(ctx, "indra/newview/vkgfx/llcleanrenderervk.h")
    fi = _read(ctx, _SH + "clean_frame_info.glsl")
    if "mat4 cur_inv_proj[CLEAN_MAX_VIEWS];" not in fi or "vec4 eye_rel[CLEAN_MAX_VIEWS];" not in fi \
            or "float cur_inv_proj[kMaxViews][16];" not in h or "float eye_rel[kMaxViews][4];" not in h:
        m.append("the frame info has no per-view current unprojection / eye offset (cur_inv_proj / eye_rel)")
    if "std::memcpy(si.cur_inv_proj[v], mCurInvProjV[v], sizeof(si.cur_inv_proj[v]));" not in cpp:
        m.append("the frame info fill does not write every view's cur_inv_proj")
    ea = _func(cpp, "void LLCleanRendererVK::ensureAO(")
    for k in ("mAoA = aoScreenImage(", "mAoB = aoScreenImage(", "mAoMeta = aoScreenImage(", "mAoHist[i] = aoScreenImage("):
        if k not in ea:
            m.append("ensureAO: not a layer per view: " + k[:-17])
    if "mAoA.arrayView, VK_IMAGE_LAYOUT_GENERAL" not in ea or "mAoMeta.arrayView, VK_IMAGE_LAYOUT_GENERAL" not in ea:
        m.append("ensureAO: draw set 0 b21 does not name the AO's 2D_ARRAY views")
    si = _func(cpp, "GpuImage LLCleanRendererVK::aoScreenImage(")
    if "createScreenImage(w, h, mViewCount," not in si or "addArrayView(g," not in si:
        m.append("aoScreenImage: not createScreenImage(mViewCount) + its 2D_ARRAY view")
    ws = _func(cpp, "void LLCleanRendererVK::writeAoSet(")
    for k in ("mDepth.arrayView", "mAoMeta.arrayView", "mAoHist[1].arrayView", "mAoHist[0].arrayView", "mMotion.arrayView"):
        if k not in ws:
            m.append("writeAoSet: not the 2D_ARRAY view: " + k)
    zm = _func(cpp, "void LLCleanRendererVK::ensureAoDepthMips(")
    if "createScreenImage(w, h, mViewCount, VK_FORMAT_R32_SFLOAT" not in zm or "VK_IMAGE_VIEW_TYPE_2D_ARRAY" not in zm:
        m.append("ensureAoDepthMips: the depth MIP chain is not a layer per view (2D_ARRAY level views)")
    if _func(cpp, "void LLCleanRendererVK::aoStage(").count("vkCmdDispatch(cmd, gx, gy, mViewCount);") != 1 \
            or "vkCmdDispatch(cmd, gx, gy, mViewCount);" not in _func(cpp, "void LLCleanRendererVK::aoStageRt(") \
            or _func(cpp, "void LLCleanRendererVK::recordAoDepthMips(").count(", mViewCount);") != 2:
        m.append("an AO dispatch (kernel / blur / accumulator, RTAO, the depth MIP chain) is not over the views")
    if "std::max(mAoHist[0].layers, 1u)" not in _func(cpp, "void LLCleanRendererVK::aoTemporalRecord("):
        m.append("aoTemporalRecord: the history copy does not cover every view's layer")
    for k in ("aoGbufSpec(mViewCount), &mAoPipeG", "aoGbufSpec(mViewCount), &mRtaoPipeG", "viewCountSpec(mViewCount), &mAoZPipe",
              "viewCountSpec(mViewCount), &mAoZSpdPipe"):
        if k not in cpp:
            m.append("not specialized with the view count: " + k.split("&")[1])
    if '"RenderVKPrepassDepthOnlyDeferred", true);' not in _func(cpp, "bool LLCleanRendererVK::aoStereoOk("):
        m.append("aoStereoOk: a stereo frame with the pre-pass normals (RenderVKPrepassDepthOnlyDeferred 0) keeps AO on "
                 "(the frame info says AO, nothing writes it)")
    if "&& (!stereoActive() || aoStereoOk());" not in cpp:
        m.append("the frame info keeps AO off in stereo")
    if "if (stereoActive() && aoNow && !slimDefer)" not in cpp:
        m.append("recordWorld: a stereo frame's AO could read the one-layer pre-pass normal target")
    ao = _read(ctx, _SH + "clean_ao.comp") + _read(ctx, _SH + "clean_rtao.comp") + _read(ctx, _SH + "clean_ao_common.glsl")
    z = _read(ctx, _SH + "clean_ao_depth.comp") + _read(ctx, _SH + "clean_ao_depth_spd.comp")
    if ao.count("#define CLEAN_VIEW CLEAN_VIEW_Z") < 2 or z.count("#define CLEAN_VIEW CLEAN_VIEW_Z") < 2:
        m.append("an AO kernel is not layered (CLEAN_VIEW_Z)")
    if "#define AO_INV_PROJ (kViewCount > 1 ? ssr.cur_inv_proj[CLEAN_VIEW] : pc.inv_proj)" not in ao:
        m.append("clean_ao_common.glsl: the unprojection is not the view's own")
    if re.search(r"(texelFetch|textureLod|textureGather)\(u(Depth|Src|Normal|ViewZ|HistPrev|Motion), (?!ivec3|vec3)", ao + z) \
            or re.search(r"image(Load|Store)\(u(Dst|Meta|HistCur|Out\[\w+\]), (?!ivec3)", ao + z):
        m.append("an AO image access without the view's layer")
    if re.search(r"set = 0, binding = \d+\) (writeonly )?uniform (u?sampler2D|u?image2D) ", ao + z):
        m.append("an AO image declared 2D (one layer: both eyes would share it)")
    sl = _read(ctx, _SH + "clean_surface_light.glsl")
    if "uniform sampler2DArray uAO[2];" not in sl or sl.count("int(CLEAN_VIEW)), 0)") < 3:
        m.append("clean_surface_light.glsl: aoSample does not read its view's layer of uAO")
    return m


def _denoisers(ctx, cpp, sh):
    """The diffuse / specular denoisers (LLCleanDenoiserVK: the projector / local-light one, the GI one) per view: every
    image a layer set per view (layer l of view v at l x N + v), the passes layered and dispatched z = N, the producers'
    / consumers' accesses at the view's layer."""
    m = []
    dn = _read(ctx, "indra/newview/vkgfx/llcleandenoisevk.cpp")
    if "layers = dnLayers(kDnImg[i], sun, split) * std::max(views, 1u);" not in dn:
        m.append("llcleandenoisevk.cpp dnShape: the images do not hold their layers once per view")
    if dn.count("dnViewSpec(mViews), &m") < 2:
        m.append("llcleandenoisevk.cpp: the temporal / a-trous pipelines are not specialized with the view count")
    if dn.count("vkCmdDispatch(cmd, (mW + 7u) / 8u, (mH + 7u) / 8u, mViews);") < 2:
        m.append("llcleandenoisevk.cpp: the temporal / a-trous passes are not dispatched over the views (z = mViews)")
    if "createScreenImage(iw, ih, layers, f, usage" not in dn or "image(i)->arrayView" not in dn:
        m.append("llcleandenoisevk.cpp: MOT is not a screen image of a layer per view bound through its 2D_ARRAY view")
    if "(kDnSplitGeomPar + par) * views, views}" not in dn:
        m.append("llcleandenoisevk.cpp: the split RAW snapshot copies one view's layer")
    if "mViews > 1u) return;" not in _func(dn, "void LLCleanDenoiserVK::noiseBegin("):
        m.append("llcleandenoisevk.cpp: the one-view noise probe runs in stereo (it reads the layers of before)")
    ps = _read(ctx, "indra/newview/vkgfx/llcleanprojstochvk.cpp")
    gi = _read(ctx, "indra/newview/vkgfx/llcleangivk.cpp")
    if '"Projector denoiser", false, mViewCount)' not in ps or '"GI denoiser", true, mViewCount)' not in gi:
        m.append("a denoiser is created without the view count")
    for f in ("clean_dn_temporal.comp", "clean_dn_atrous.comp"):
        t = _read(ctx, _SH + f)
        if "#define CLEAN_VIEW CLEAN_VIEW_Z" not in t:
            m.append(f + " is not layered (CLEAN_VIEW_Z)")
        if re.search(r"image(Load|Store)\(uDn(In|Geom|Hist|Out|Age|SunHist|SplitAge), ivec3\((?:[^()]|\([^()]*\))*, (?!DN_L\()", t) \
                or re.search(r"image(Load|Store)\(uDnMot, (?!DN_MOT\()", t):
            m.append(f + ": a denoiser image access without the view's layer (DN_L / DN_MOT)")
    c = _read(ctx, _SH + "clean_dn_common.glsl")
    if "#define DN_L(l) (int(l) * kViewCount + int(CLEAN_VIEW))" not in c or "uniform uimage2DArray uDnMot;" not in c \
            or "uniform sampler2DArray uDnMotion;" not in c or "imageSize(uDnGeom).z" in c:
        m.append("clean_dn_common.glsl: the per-view layer macro / the arrayed MOT and motion vectors / the layer-count "
                 "tests divided by N are missing")
    if re.search(r"image(Load|Store)\(u(Pdn|Gi)(In|Geom|Out), ivec3\((?:[^()]|\([^()]*\))*, (?!PDN_L\()", sh) \
            or re.search(r"image(Load|Store)\(u(Pdn|Gi)Mot, (?!PDN_MOT\()", sh) or "imageSize(uPdnGeom).z" in sh:
        m.append("clean_deferred_light.comp: a denoiser image access without the view's layer (PDN_L / PDN_MOT)")
    if "uniform uimage2DArray uPdnMot;" not in sh or "uniform writeonly uimage2DArray uGiMot;" not in sh \
            or "uniform sampler2D uReuseMotion;" in sh:
        m.append("clean_deferred_light.comp: MOT / the reuse motion vectors are declared 2D (one layer)")
    return m


def _pass15(ctx, cpp, sh):
    """The merged RT shadows / ReSTIR DI / the stochastic local-light shadows (pass 15) per view: the grid kernels
    z = N, the job lists over every view's texels (a view-inclusive texel index), the per-texel buffers x N, the
    kernels specialized with the view count, the stereo gates gone (SlipstreamVRPointShadows 1)."""
    m = []
    ps = _read(ctx, "indra/newview/vkgfx/llcleanprojstochvk.cpp")
    pf = _func(ps, "void LLCleanRendererVK::pdnFrame(")
    if "const bool can = sample && vrStochShadows() &&" not in pf or "stereoActive()" in pf:
        m.append("pdnFrame: pass 15 still never samples in stereo")
    if "!vrStochShadows()) return false;" not in _func(ps, "bool LLCleanRendererVK::shadowMergeWanted(") \
            or "mShadowMergeTry = rt && sun && shadowMergeWanted(true);" not in cpp:
        m.append("the merged RT shadows are still refused in stereo")
    vs = _func(ps, "bool LLCleanRendererVK::vrStochShadows(")
    if "if (!stereoActive()) return true;" not in vs or "SlipstreamVRPointShadows" not in vs or "pts == 1u" not in vs:
        m.append("vrStochShadows: not the desktop's path in stereo by default (SlipstreamVRPointShadows 1)")
    pi = _func(ps, "void LLCleanRendererVK::pdnInit(")
    if pi.count("pdnLitSpec(mViewCount)") < 4 or "pdnLitSpec(sg.pipe == &mLvcPipe ? 1u : mViewCount)" not in pi:
        m.append("pdnInit: pass 15's per-view kernels are not specialized with the view count")
    if ps.count("vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);") < 6 \
            or "vkCmdDispatch(cmd, tx, ty, mViewCount);" not in ps:
        m.append("pass 15's grid kernels (SELECT / SUNVIS / SHADE / the fused ones / the passes / MASK) are not z = N")
    if "if (stereoActive()) releaseTlas(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);" not in \
            _func(ps, "void LLCleanRendererVK::recordProjStoch("):
        m.append("recordProjStoch: no TLAS release on the lighting's queue in stereo")
    if "(VkDeviceSize)w * h * mViewCount * 2u * 8u" not in _func(ps, "void LLCleanRendererVK::rsResFrame(") \
            or "(VkDeviceSize)w * h * mViewCount * (k > 4u" not in ps \
            or "const VkDeviceSize texels = (VkDeviceSize)w * h * mViewCount;" not in ps \
            or "mPdnSelW * mPdnSelH * mViewCount * mPdnVisSlots" not in ps \
            or "((h + 7u) / 8u) * mViewCount;" not in ps:
        m.append("pass 15's per-texel buffers (reservoirs, descriptors, jobs / visibilities, exact / tile lists) are "
                 "not sized for every view")
    if not re.search(r"#elif CLEAN_DEFER_PASS == 15\n(//[^\n]*\n)*uint g_pdn_view = 0u;\n"
                     r"#define CLEAN_VIEW \(kViewCount > 1 \? g_pdn_view : 0u\)", sh):
        m.append("clean_deferred_light.comp: pass 15's view is not a per-invocation variable (g_pdn_view)")
    if sh.count("g_pdn_view = CLEAN_VIEW_Z;") < 4:
        m.append("clean_deferred_light.comp: a pass-15 grid kernel does not take its view from z")
    if sh.count("const ivec2 m = pdnTexelAt(tex, D, vw);") < 2 or sh.count("g_pdn_view = vw;") < 2:
        m.append("clean_deferred_light.comp: VIS / the mover-exact jobs do not take the view from the job's texel")
    if "g_pdn_view = kViewCount > 1 ? row / tilesY : 0u;" not in sh:
        m.append("clean_deferred_light.comp: an adaptive extra pass does not take the view from its tile")
    for needle in ("uint pdnSelIndex(ivec2 m, ivec2 D) { return 4u * pdnTexel(m, D); }",
                   "return layer * pdnTexels(D) + pdnTexel(q, D); }",
                   "uint pdnVisBase(ivec2 D) { return 4u + pdnTexels(D) * pdnVisSlots(); }",
                   "uint pdnExactCap(ivec2 D)  { return pdnTexels(D); }"):
        if needle not in sh:
            m.append("clean_deferred_light.comp: a per-texel buffer index is one view's: " + needle.split("{")[0].strip())
    i = sh.find("#elif CLEAN_DEFER_PASS == 15\n#include \"clean_regir_di_lookup.glsl\"")
    j = sh.find("#endif   // CLEAN_SHADOW_STAGE == SH_STAGE_VIS / SUNVIS / else", i)
    if i < 0 or j < 0 or re.search(r"uint\(m\.y \* D\.x \+ m\.x\)|int\(tex % uint\(D\.x\)\), int\(tex / uint", sh[i:j]):
        m.append("clean_deferred_light.comp: a pass-15 texel index without its view (pdnTexel / pdnTexelAt)")
    if "(1u << 4u) | (1u << 5u) | (1u << 6u) | (1u << 7u)" not in cpp:
        m.append("the hang beacon's shadow-core twins are not built with the view count (kRtBcLayered)")
    return m


def _gi(ctx, cpp, sh):
    """Ray-traced GI (RenderVKGI) per view: the desktop's decision in stereo unless SlipstreamVRGI picks the world-cache
    tier, the grid kernels z = N, the lists / records / reservoirs / tags over every view's texels, the kernels
    specialized with the view count, the latency frame's depth snapshot a layer per view."""
    m = []
    gi = _read(ctx, "indra/newview/vkgfx/llcleangivk.cpp")
    gd = _func(gi, "void LLCleanRendererVK::giDecide(")
    if "if (vrTier) { giDecideVr(rw, rh); return; }" not in gd or "if (stereoActive()) { giDecideVr(rw, rh); return; }" in gd:
        m.append("giDecide: a stereo frame still runs only the VR GI tier")
    if "LLXrSession::instance().sessionRunning()" in _func(gi, "GiCfg giCfg()"):
        m.append("giCfg: GI is still forced off while a VR session runs (#1076)")
    gin = _func(gi, "void LLCleanRendererVK::giInit(")
    if gin.count("giLitSpec(mViewCount)") < 3:
        m.append("giInit: the GI kernels are not specialized with the view count")
    if gi.count("vkCmdDispatch(cmd, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);") < 5 \
            or "vkCmdDispatch(c, (dw + 7u) / 8u, (dh + 7u) / 8u, mViewCount);" not in gi:
        m.append("recordGI: a GI grid kernel (fused trace / TRACE / SHADE / REUSE / CHG-TAG) is not z = N")
    if "giHitBytes(w, h * mViewCount)" not in gi or "(VkDeviceSize)w * h * mViewCount * 2u * 32u" not in gi:
        m.append("giApply: the records / reservoirs are not sized for every view")
    if "if (stereoActive()) releaseTlas(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);" not in \
            _func(gi, "void LLCleanRendererVK::recordGI("):
        m.append("recordGI: no TLAS release on the lighting's queue in stereo")
    lag = _func(gi, "bool LLCleanRendererVK::giLagEnsure(")
    if "createScreenImage(rw, rh, mViewCount, mDepthFormat" not in lag or "mGiLagDepth.arrayView" not in gi:
        m.append("the one-frame-latency GI's depth snapshot is not a layer per view bound through its 2D_ARRAY view")
    if not re.search(r"#elif DEFER_GI_TRACE\n(//[^\n]*\n)*uint g_gi_view = 0u;\n"
                     r"#define CLEAN_VIEW \(kViewCount > 1 \? g_gi_view : 0u\)", sh):
        m.append("clean_deferred_light.comp: the GI kernels' view is not a per-invocation variable (g_gi_view)")
    if sh.count("g_gi_view = CLEAN_VIEW_Z;") < 4:
        m.append("clean_deferred_light.comp: a GI grid kernel does not take its view from z")
    for needle in ("uint giResIndex(ivec2 q, ivec2 D, uint layer) { return 2u * (layer * giTexels(D) + giTexel(q, D)); }",
                   "6u * giTexel(m, D); }", "const uint n = giTexels(D), t = giTexel(q, D);",
                   "giShadeListAppend(h.hit, giTexel(m, D), giTexels(D));", "giListAppend(giTexel(m, D));"):
        if needle not in sh:
            m.append("clean_deferred_light.comp: a GI per-texel index is one view's: " + needle[:48])
    if sh.count("giTexelAt(t, D, vw);") < 2:
        m.append("clean_deferred_light.comp: a GI list kernel does not take the view from its listed texel")
    vq = _read(ctx, "indra/newview/vkgfx/llcleanvisqueuevk.cpp")
    if "vqLitSpec(s.set != 7u ? mViewCount : 1u)" not in vq or "mGiDn->height() * mViewCount" not in vq:
        m.append("the visibility queue's GI stages are not layered / its GI region is one view's")
    if "(1u << 8u) | (1u << 9u)" not in cpp:
        m.append("the hang beacon's GI trace twins are not built with the view count (kRtBcLayered)")
    return m


def _weather(ctx, cpp, sh):
    """The weather draws per view: the module is not skipped in stereo, its pass-2 pipelines (precipitation, splashes,
    run-off streams via the shared desc, the veil) are multiview, every projection reads the view's matrix (the
    parameter block's view tail), the simulation keeps a particle any view sees, and the renderer hands the views'
    matrices over. The frame-generation late path stays off in stereo."""
    m = []
    wx = _read(ctx, "indra/newview/vkgfx/llcleanweathervk.cpp")
    wh = _read(ctx, "indra/newview/vkgfx/llcleanweathervk.h")
    ew = _func(cpp, "void LLCleanRendererVK::ensureWeather(")
    if re.search(r"if \(stereoActive\(\)\) return;", ew):
        m.append("ensureWeather: the weather module is still skipped in stereo")
    if "f.views = mViewCount;" not in ew:
        m.append("ensureWeather: the module is not told the view count")
    if "mWeather->recordSim(cmd, drawPush.view_proj, drawPush.origin, stereoActive() ? mEyeVP : nullptr, mViewCount);" \
            not in cpp:
        m.append("recordWorld: the simulation is not handed every view's matrix")
    ep = _func(wx, "bool LLCleanWeatherVK::ensurePipelines(")
    if ep.count("d.views = std::max(f.views, 1u);") < 2:
        m.append("ensurePipelines: the precipitation / splash / stream or veil pipelines are not multiview")
    if "wxViewSpec(f.views), &mStage[0]" not in ep:
        m.append("ensurePipelines: the simulation is not specialized with the view count")
    if "mPipeViews == std::max(f.views, 1u)" not in ep:
        m.append("ensurePipelines: a view-count change does not rebuild the pipelines")
    rs = _func(wx, "void LLCleanWeatherVK::recordSim(")
    if "P.view_proj_v[v]" not in rs or "P.inv_view_proj_v[v]" not in rs:
        m.append("recordSim: the parameter block's view tail is not filled")
    if "float view_proj_v[kMaxViews][16];" not in wh or "sizeof(Params) == 1008" not in wh:
        m.append("llcleanweathervk.h: Params has no per-view tail (clean_weather_common.glsl WeatherParams)")
    co = _read(ctx, _SH + "clean_weather_common.glsl")
    if "mat4 view_proj_v[CLEAN_MAX_VIEWS];" not in co \
            or "#define WX_VP (kViewCount > 1 ? wp.view_proj_v[CLEAN_VIEW] : wp.view_proj)" not in co:
        m.append("clean_weather_common.glsl: no per-view matrix (WX_VP)")
    vs = _read(ctx, _SH + "clean_weather.vert")
    if re.search(r"wp\.view_proj\s*(\*|\[)", vs) or vs.count("WX_VP") < 9:
        m.append("clean_weather.vert: a projection does not use the view's matrix (WX_VP)")
    vf = _read(ctx, _SH + "clean_weather_veil.frag")
    if re.search(r"wp\.inv_view_proj\s*\*", vf) or vf.count("WX_INV_VP") < 2 or "ssr.eye_rel[CLEAN_VIEW]" not in vf:
        m.append("clean_weather_veil.frag: the veil does not unproject through its view (WX_INV_VP / eye offset)")
    sim = _read(ctx, _SH + "clean_weather_sim.comp")
    if "wp.view_proj_v[v]" not in sim or sim.count("wxInViews(") < 3:
        m.append("clean_weather_sim.comp: the visible list does not keep a particle any view sees")
    # (the late path's stereo gate: at the record site before fg-weather (#1517), in wxLateWanted() after it)
    late_fn = _func(cpp, "LLCleanRendererVK::wxLateWanted(")
    late_off = ("mWxLateNow && mWeather && mUiLayerView && dispDepth.view && mExposureImg.view && !stereoActive()" in cpp
                or (late_fn and "!stereoActive()" in late_fn))
    if not late_off or (not late_fn and not re.search(r"mFrameGenFrame && !mOffscreen && !stereoActive\(\)", cpp)):
        m.append("the frame-generation late precipitation (#1517) is no longer off in stereo")
    return m


def _vol_fog(ctx, cpp, sh):
    """The legacy (physical-sky-off) froxel fog per view, the camera medium's way: an LLCleanVolFogVK per eye, eye v > 0's
    grid read through ssr.medium[v] / binding 59 [v] (eye 0's through the vol_* block / binding 45), the opaque apply
    multiview."""
    m = []
    ev = _func(cpp, "void LLCleanRendererVK::ensureVolFog(")
    if "stereoActive()" in ev.split("if (!mVolFog) mVolFog = new LLCleanVolFogVK();")[0]:
        m.append("ensureVolFog: the legacy fog is still off in stereo")
    if "const uint32_t count=mViewCount;" not in ev or "f.views=mViewCount;" not in ev:
        m.append("ensureVolFog: the legacy fog does not run an instance per view / a multiview apply")
    if "write.dstBinding=59; write.dstArrayElement=eye;" not in ev:
        m.append("ensureVolFog: a legacy eye's grid is not bound at binding 59 [eye]")
    if "const bool legacyEyes = !mCameraMediumReady && mVolFogNow && stereoActive();" not in cpp:
        m.append("the frame info does not carry the legacy eyes' grids (ssr.medium[v])")
    if "const uint32_t mediumCount=(mCameraMediumReady || stereoActive()) ? mViewCount : 1u;" not in cpp:
        m.append("recordVolFogCompute: the legacy eyes' passes are not recorded")
    vf = _read(ctx, "indra/newview/vkgfx/llcleanvolfogvk.cpp")
    if "d.views = std::max(f.views, 1u);" not in _func(vf, "bool LLCleanVolFogVK::ensurePipelines("):
        m.append("LLCleanVolFogVK: the opaque apply is not multiview")
    vc = _read(ctx, _SH + "clean_vol_common.glsl")
    if vc.count("(cmOn() || MEDIUM_EYE != 0u) ? ssr.medium[MEDIUM_EYE]") < 8:
        m.append("clean_vol_common.glsl: a vol*() accessor reads eye 0's legacy block for every eye")
    va = _read(ctx, _SH + "clean_vol_apply.glsl")
    if "textureLod(uMediumL[MEDIUM_EYE], tc, 0.0)" not in va or re.search(r"ssr\.vol_(eye|vp)\b", va.split("vec4 volFogAt")[1]):
        m.append("clean_vol_apply.glsl: volFogAt reads eye 0's legacy grid for every view")
    ap = _read(ctx, _SH + "clean_vol_apply.frag")
    if "ssr.vol_inv_vp" in ap or "ssr.vol_eye" in ap:
        m.append("clean_vol_apply.frag: the opaque apply unprojects through eye 0's grid camera")
    return m


def _froxels(ctx, cpp, sh):
    """The froxel light masks per view: a grid per eye (cells x N, view v's after the first v views'), the build z = N
    with kViewCount = N reading each view's matrix about its own eye, every lookup (the deferred kernels, the forward
    walk) adding its view's base."""
    m = []
    if re.search(r"froxelGo = culling == 2 && mFroxelPipe && mDsetFroxel && !stereoActive\(\)", cpp) \
            or "ensureFroxelBuffer(froxelCells * mViewCount)" not in cpp:
        m.append("recordWorld: the froxel masks are still off in stereo / sized for one view")
    if "vkCmdDispatch(cmd, nl, kFroxelSplit, mViewCount);" not in cpp \
            or "(VkDeviceSize)froxelCells * mViewCount * kFroxelWords" not in cpp:
        m.append("recordWorld: the froxel build is not z = N / the clear does not cover every view's grid")
    if "viewCountSpec(mViewCount), &mFroxelPipe" not in cpp:
        m.append("mFroxelPipe is not specialized with the view count")
    fs = _func(cpp, "uint32_t LLCleanRendererVK::froxelSetup(")
    if "li.fx_vp_v[v]" not in fs or "mEyeVP[v]" not in fs or "li.fx_eye_v[v]" not in fs:
        m.append("froxelSetup: the views' matrices / eyes are not filled")
    lc = _read(ctx, _SH + "clean_lights_common.glsl")
    if "mat4  fx_vp_v[CLEAN_MAX_VIEWS];" not in lc or "#define FX_VIEW_BASE(L)" not in lc:
        m.append("clean_lights_common.glsl: LightInfo has no per-view froxel grid")
    fx = _read(ctx, _SH + "clean_light_froxel.comp")
    if "#define CLEAN_VIEW CLEAN_VIEW_Z" not in fx or "froxel[FX_VIEW_BASE(info) +" not in fx \
            or "info.fx_vp" in fx.replace("info.fx_vp_v", "") or "info.fx_eye" in fx:
        m.append("clean_light_froxel.comp: the build is not layered (its view's matrix / eye / grid)")
    if "return FX_VIEW_BASE(linfo) +" not in sh or "dot(P - FX_EYE(linfo).xyz" not in sh:
        m.append("clean_deferred_light.comp: froxelCellBase reads view 0's grid")
    dc = _read(ctx, _SH + "clean_draw_common.glsl")
    if "const uint fbase = FX_VIEW_BASE(linfo) +" not in dc:
        m.append("clean_draw_common.glsl: the forward froxel walk reads view 0's grid")
    return m


def _vol_clouds(ctx, cpp, sh):
    """The volumetric clouds' main-view march / temporal / composite per view: a layer per view in every screen target,
    the kernels z = N with kViewCount = N about each view's own eye (the parameter block's view tail), the composite
    multiview; the weather map, shadow map and panorama stay shared."""
    m = []
    vc = _read(ctx, "indra/newview/vkgfx/llcleanvolcloudsvk.cpp")
    if "mVolCloudsComposite = mVolClouds->compositeWanted() && mLightsPipe && !stereoActive();" in cpp \
            or "mVolClouds->views() == mViewCount" not in cpp:
        m.append("recordWorld: the clouds' composite is still off in stereo")
    if "create(mCtx, mDslDraw0, mDslDraw1, mHdrFormat, mViewCount)" not in cpp \
            or "vf.viewProjRelV = mViewProjRelV; vf.eyeV = mEyePosV;" not in cpp:
        m.append("the clouds module is not created for the views / not handed the views' cameras")
    if "recordMarch(cmd, mDepth.image, mDepth.arrayView, mDepthGen)" not in cpp \
            or "recordComposite(cmd, stereoActive() ? sceneHdr().arrayView : sceneHdr().view)" not in cpp:
        m.append("recordWorld: the clouds read one depth layer / composite one scene layer")
    et = _func(vc, "bool LLCleanVolCloudsVK::ensureTargets(")
    if "createScreenImage(w, h, mViews," not in et or "createImage2D(" in et or "addArrayView(*im" not in et:
        m.append("ensureTargets: the march / history targets are not a layer per view")
    if "vcViewSpec(mViews)" not in vc or "d.views = mViews;" not in vc:
        m.append("create: the march / temporal are not specialized with the view count / the composite is not multiview")
    if "vkCmdDispatch(cmd, mGroupsX, mGroupsY, mViews);" not in vc \
            or "vkCmdDispatch(cmd, (mW + 7u) / 8u, (mH + 7u) / 8u, mViews);" not in vc:
        m.append("the march / temporal are not dispatched over the views (z = N)")
    if "if (mViews > 1u) ri.viewMask = (1u << mViews) - 1u;" not in vc:
        m.append("recordComposite: the composite scope is not multiview")
    if "P.inv_vp_v[v]" not in vc or "P.reproj_v[vw]" not in vc:
        m.append("prepare: the parameter block's view tail is not filled")
    ub = _read(ctx, _SH + "clean_vol_cloud_ubo.glsl")
    if "mat4 inv_vp_v[CLEAN_MAX_VIEWS];" not in ub or "#define VC_L int(CLEAN_VIEW)" not in ub:
        m.append("clean_vol_cloud_ubo.glsl: no per-view camera / layer")
    for f in ("clean_vol_cloud_march.comp", "clean_vol_cloud_temporal.comp"):
        t = _read(ctx, _SH + f)
        if "#define CLEAN_VIEW CLEAN_VIEW_Z" not in t or "image2DArray uOutA" not in t or "vc.inv_vp" in t \
                or "vc.cam.xyz" in t:
            m.append(f + ": not layered (its view's layers / camera)")
    cf = _read(ctx, _SH + "clean_vol_cloud_composite.frag")
    if "uniform sampler2DArray uCloudA;" not in cf or "ivec3(q, VC_L)" not in cf or "vc.inv_vp" in cf:
        m.append("clean_vol_cloud_composite.frag: the composite reads view 0's clouds / camera")
    return m


def _part_fog(ctx, cpp, sh):
    """The particle fog per view: the bricks (world space) shared and kept for any view, the screen march / temporal
    layered about each eye (z = N, kViewCount = N, the parameter block's view tail), the composite multiview."""
    m = []
    pf = _read(ctx, "indra/newview/vkgfx/llcleanpartfogvk.cpp")
    ep = _func(cpp, "void LLCleanRendererVK::ensurePartFog(")
    if re.search(r"if \(stereoActive\(\)\) return;", ep):
        m.append("ensurePartFog: the particle fog is still off in stereo")
    if "f.depthView = mDepth.arrayView;" not in ep or "f.views = mViewCount;" not in ep \
            or "f.viewProjRelV = mViewProjRelV; f.eyeV = mEyePosV;" not in ep:
        m.append("ensurePartFog: the module is not handed the views / their cameras / the layered depth")
    et = _func(pf, "bool LLCleanPartFogVK::ensureTargets(")
    if "createScreenImage(w, h, mViews," not in et or "createImage2D(" in et:
        m.append("ensureTargets: the march / history targets are not a layer per view")
    if "pfViewSpec(mViews)" not in pf or "d.views = mViews;" not in pf:
        m.append("ensurePipelines: the march / temporal are not specialized with the views / the composite not multiview")
    if pf.count("vkCmdDispatch(cmd, (mActW + 7u) / 8u, (mActH + 7u) / 8u, mViews);") < 2:
        m.append("recordCompute: the march / temporal are not dispatched over the views (z = N)")
    if "P.inv_vp_v[v]" not in pf or "P.reproj_v[v]" not in pf:
        m.append("prepare: the parameter block's view tail is not filled")
    if "s.visible = boxVisible(f.viewProjRelV[v], eye, s.bmin, s.bmax);" not in pf:
        m.append("classifyAll: a brick only one eye sees is dropped")
    pc = _read(ctx, _SH + "clean_pfog_common.glsl")
    if "mat4 inv_vp_v[CLEAN_MAX_VIEWS];" not in pc or "#define PF_L int(CLEAN_VIEW)" not in pc:
        m.append("clean_pfog_common.glsl: no per-view camera / layer")
    for f in ("clean_pfog_march.comp", "clean_pfog_temporal.comp"):
        t = _read(ctx, _SH + f)
        if "#define CLEAN_VIEW CLEAN_VIEW_Z" not in t or "image2DArray uOutA" not in t or "pf.inv_vp" in t:
            m.append(f + ": not layered (its view's layers / camera)")
    cf = _read(ctx, _SH + "clean_pfog_composite.frag")
    if "uniform sampler2DArray uFogA" not in cf or "ivec3(q, PF_L)" not in cf or "pf.inv_vp" in cf:
        m.append("clean_pfog_composite.frag: the composite reads view 0's fog / camera")
    return m


def _god_rays(ctx, cpp, sh):
    """God rays per view: every target a layer per view, the march (cascades / RT / RT + denoiser), the temporal, the
    denoiser's temporal / a-trous z = N with kViewCount = N, the per-view cameras through the module's view block (the
    pushes are full), the composite multiview."""
    m = []
    gr = _read(ctx, "indra/newview/vkgfx/llcleangodraysvk.cpp")
    if re.search(r"!mOrthoView && !stereoActive\(\) && !mSnapMatte", cpp):
        m.append("recordWorld: god rays are still off in stereo")
    if "gf.depthView = mDepth.arrayView;" not in cpp or "gf.viewProjRelV = mViewProjRelV; gf.eyeV = mEyePosV;" not in cpp:
        m.append("recordWorld: the god rays get one depth layer / no per-view cameras")
    for img in ("mOut = mCtx->createScreenImage(w, h, mViews,", "*imgs[i] = mCtx->createScreenImage(w, h, mViews,"):
        if img not in gr:
            m.append("a god rays target is not a layer per view: " + img[:24])
    if gr.count("grViewSpec(mViews)") < 6 or "d.views = mViews;" not in gr:
        m.append("a god rays kernel is not specialized with the view count / the composite is not multiview")
    if gr.count(", mViews);") < 4:
        m.append("a god rays dispatch is not z = N")
    if gr.count("1, &mViewOff)") < 5 or "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC" not in gr:
        m.append("the god rays' view block is not bound to every pass that reads it")
    if "g.reproj[c * 4 + r]" not in gr or "invert4(vpV[v], g.inv_vp);" not in gr:
        m.append("recordMarch: the view block is not filled per view")
    vw = _read(ctx, _SH + "clean_godrays_view.glsl")
    if "GrView grv[CLEAN_MAX_VIEWS];" not in vw:
        m.append("clean_godrays_view.glsl: no per-view block")
    for f in ("clean_godrays.comp", "clean_godrays_temporal.comp", "clean_godrays_dn_temporal.comp",
              "clean_godrays_atrous.comp"):
        t = _read(ctx, _SH + f)
        if "#define CLEAN_VIEW CLEAN_VIEW_Z" not in t or "image2DArray" not in t or "GR_L" not in t:
            m.append(f + ": not layered")
    g = _read(ctx, _SH + "clean_godrays.comp")
    if "GR_INV_VP * vec4" not in g or "gp.cam.xyz + V" in g:
        m.append("clean_godrays.comp: the march unprojects / starts at view 0's camera")
    for f in ("clean_godrays_temporal.comp", "clean_godrays_dn_temporal.comp"):
        if "GR_REPROJ *" not in _read(ctx, _SH + f):
            m.append(f + ": reprojects through view 0's camera")
    cf = _read(ctx, _SH + "clean_godrays_composite.frag")
    if "uniform sampler2DArray uInscatter;" not in cf or "GR_INV_VP * vec4" not in cf:
        m.append("clean_godrays_composite.frag: the composite reads view 0's in-scatter / camera")
    return m


def _glow_temporal(ctx, cpp, sh):
    """The glow temporal resolve per view: the history pair a layer per view, every input bound through its 2D_ARRAY
    view, the kernel z = N with kViewCount = N, each view's reprojection from the frame info (the push holds one)."""
    m = []
    if re.search(r"mGlowTarget\.view\s*&& !stereoActive\(\);", cpp):
        m.append("glowTemporalWanted: the glow temporal resolve is still off in stereo")
    if "mTaaGlow[i] = mCtx->createScreenImage(gw, gh, mViewCount," not in cpp:
        m.append("ensureGlowTemporal: the history pair is not a layer per view")
    if "{mGlowSampler,  mTaaGlow[i ^ 1].arrayView," not in cpp or "{mDepthSampler, mMotion.arrayView," not in cpp:
        m.append("ensureGlowTemporal: the set binds one layer")
    if "viewCountSpec(mViewCount), &mGlowTmpPipe" not in cpp or "(gh + 7) / 8, mViewCount);" not in cpp:
        m.append("the glow temporal kernel is not specialized with the view count / not dispatched z = N")
    t = _read(ctx, _SH + "clean_glow_temporal.comp")
    if "#define CLEAN_VIEW CLEAN_VIEW_Z" not in t or "image2DArray uGlowOut" not in t or "GT_REPROJ * vec4" not in t \
            or "ssr.views[CLEAN_VIEW].taa_prev_vp" not in t:
        m.append("clean_glow_temporal.comp: not layered / reprojects through view 0's camera")
    return m


def _bloom(ctx, cpp, sh):
    """Bloom per view: the pyramid and the cross filter a layer per view (own allocations in stereo: the alias arena's
    slots are one-layer), multiview passes over their 2D_ARRAY views, the tonemap's composite at its view's layer,
    every set of the arrayed shaders bound through 2D_ARRAY views (the snapshot extract and the placeholders too)."""
    m = []
    if "mBloomActive = want && !stereoActive()" in cpp:
        m.append("recordWorld: bloom is still off in stereo")
    if "mAlias.enabled() && nm >= 3 && !stereoActive()" not in cpp:
        m.append("ensureBloom: a stereo pyramid would land in the one-layer alias arena")
    for img in ("mBloomMip[i] = mCtx->createScreenImage(mw, mh, mViewCount,", "mCrossA = mCtx->createScreenImage(sw, sh, mViewCount,",
                "mCrossAcc = mCtx->createScreenImage(sw, sh, mViewCount,"):
        if img not in cpp:
            m.append("ensureBloom: a bloom target is not a layer per view: " + img[:24])
    for b in ("wr1(mDsetCrossSrc, mBloomMip[0].arrayView);", "{mAASampler, mGlowTarget.arrayView,",
              "{mAASampler, mTaaGlow[i].arrayView,", "{mAASampler, mSnapAccGlow.arrayView,",
              "VkDescriptorImageInfo gi{mAASampler, mBloomMip[0].arrayView,", "mCrossAcc.arrayView, VK_IMAGE_LAYOUT"):
        if b not in cpp:
            m.append("a bloom set binds one layer: " + b[:40])
    if "ca.imageView=mainAtt(mBloomMip[dst]);" not in cpp or "ca.imageView=mainAtt(dst);" not in cpp \
            or cpp.count("d.views = mViewCount;\n    postInto(dev, d, &mBloomPipe") != 1:
        m.append("the bloom / cross passes are not multiview over every layer")
    if "b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,VK_REMAINING_ARRAY_LAYERS};\n    VkDependencyInfo d{" not in cpp:
        m.append("aaBarrier: a layered post target's layer 1 is never transitioned")
    for f in ("clean_bloom.frag", "clean_cross.frag"):
        t = _read(ctx, _SH + f)
        if "uniform sampler2DArray uSrc;" not in t or "CLEAN_VIEW" not in t:
            m.append(f + ": reads view 0's layer")
    tm = _read(ctx, _SH + "clean_tonemap.frag")
    if "uniform sampler2DArray uBloom;" not in tm or "texture(uBloom, HDR_UV(v_uv))" not in tm \
            or "texture(uCrossFilter, HDR_UV(v_uv))" not in tm:
        m.append("clean_tonemap.frag: the bloom / cross composite reads view 0's layer")
    return m


def _ldr_glow(ctx, cpp, sh):
    """The LDR glow per view: the fixed blur chain a layer per view, the extract / blur z = N with kViewCount = N, every
    set bound through 2D_ARRAY views, the combine multiview into the layered output."""
    m = []
    if "ldr_glow = ldr_glow && !mBloomActive && !stereoActive();" in cpp or "glow_on && !mBloomActive && !stereoActive()" in cpp:
        m.append("recordWorld: the LDR glow is still off in stereo")
    if "mGlowE  = mCtx->createScreenImage(GLOW_RES, GLOW_RES, mViewCount," not in cpp \
            or "mGlowP1 = mCtx->createScreenImage(GLOW_RES, GLOW_RES, mViewCount," not in cpp:
        m.append("the LDR glow chain is not a layer per view")
    for b in ("writeSet(mDsetGlowBlurA,   mGlowE.arrayView,", "writeSet(mDsetGlowCombine, mGlowP1.arrayView,",
              "{mGlowSampler, mGlowTarget.arrayView,", "{VK_NULL_HANDLE, mGlowE.arrayView, VK_IMAGE_LAYOUT_GENERAL}};"):
        if b not in cpp:
            m.append("an LDR glow set binds one layer: " + b[:40])
    if "viewCountSpec(mViewCount), &mGlowPipe" not in cpp or cpp.count("vkCmdDispatch(cmd, groups, groups, mViewCount)") != 2:
        m.append("the LDR glow kernel is not specialized with the view count / not dispatched z = N")
    if "cd.views = mViewCount;\n        registerDisplayPipeline(cd, &mGlowCombinePipe" not in cpp:
        m.append("the LDR glow combine is not multiview")
    g = _read(ctx, _SH + "clean_glow.comp")
    if "#define CLEAN_VIEW CLEAN_VIEW_Z" not in g or "image2DArray uDst" not in g or "vec3(uv, 0.0)" in g:
        m.append("clean_glow.comp: not layered / reads view 0's scene")
    if "uniform sampler2DArray uGlow;" not in _read(ctx, _SH + "clean_glow_combine.frag"):
        m.append("clean_glow_combine.frag: reads view 0's layer")
    return m


def _oit(ctx, cpp, sh):
    """Order-independent transparency per view: the accumulation targets a layer per view, the accumulation and the
    tile resolve multiview, the tile list the union over the views, every target read / written at the view's layer."""
    m = []
    if "!mOffscreen && !mLiveCapNow && !stereoActive() && !mSnapMatte" in cpp:
        m.append("recordWorld: WBOIT is still off in stereo")
    if "b.views = mViewCount;" not in cpp or "mAlphaOIT->beginAccum(cmd, mainAtt(" not in cpp:
        m.append("recordWorld: WBOIT is not given the view count / one depth layer")
    oit = _read(ctx, "indra/newview/vkgfx/llcleanalphaoitvk.cpp")
    if "mTarget[i] = ctx->createScreenImage(rw, rh, mViews," not in oit:
        m.append("llcleanalphaoitvk.cpp: the targets are not a layer per view")
    if oit.count("d.views = mViews;") != 2 or "oitViewSpec(mViews), &mTilesStage" not in oit:
        m.append("llcleanalphaoitvk.cpp: the accumulation / resolve are not multiview / the tile pass has no view count")
    if "ri.viewMask = mViews > 1u ?" not in oit or "t.arrayView, VK_IMAGE_LAYOUT_GENERAL" not in oit:
        m.append("llcleanalphaoitvk.cpp: the accumulation scope / the set covers one layer")
    t = _read(ctx, _SH + "clean_oit_tiles.comp")
    if "image2DArray uOitReveal" not in t or "v < kViewCount" not in t:
        m.append("clean_oit_tiles.comp: the tile list reads one view")
    r = _read(ctx, _SH + "clean_oit_resolve.frag")
    if "image2DArray uOitAccum" not in r or "imageLoad(uOitReveal, OIT_PX)" not in r \
            or "texelFetch(uOitSceneDepth, ivec3(px, 0), 0)" in r:
        m.append("clean_oit_resolve.frag: the resolve reads view 0's layer")
    return m


def _rcas(ctx, cpp, sh):
    """RCAS per view (a frame without a resolve): the kernel z = N with kViewCount = N over the 2D_ARRAY views of the
    SSR copy and mHdr; a stereo frame runs it in place of the final pass's CAS."""
    m = []
    if "viewCountSpec(mViewCount), &mRcasPipe" not in cpp or "(h + 7) / 8, mViewCount);" not in cpp:
        m.append("the RCAS kernel is not specialized with the view count / not dispatched z = N")
    if "{mDepthSampler, mPrevColor.arrayView," not in cpp or "{VK_NULL_HANDLE, mHdr.arrayView, VK_IMAGE_LAYOUT_GENERAL}," not in cpp:
        m.append("updateRcasSets: the set binds one layer")
    if "(!once || stereoActive())" not in cpp:
        m.append("recordWorld: a stereo frame without a resolve is not sharpened")
    t = _read(ctx, _SH + "clean_rcas.comp")
    if "#define CLEAN_VIEW CLEAN_VIEW_Z" not in t or "image2DArray uDst" not in t or "texelFetch(uSrc, p," in t:
        m.append("clean_rcas.comp: not layered")
    return m


def _dof(ctx, cpp, sh):
    """Depth of field per view: the CoC and the half-size gather a layer per view, the three passes multiview over
    2D_ARRAY views, every input read at the view's layer."""
    m = []
    if "mDofEnabled = wf.dof_enabled && !stereoActive();" in cpp:
        m.append("depth of field is still off in stereo")
    if "mDofCoC   = mCtx->createScreenImage(w, h, mViewCount," not in cpp or "mDofHalf  = mCtx->createScreenImage(hw, hh, mViewCount," not in cpp:
        m.append("ensureDoF: a DoF target is not a layer per view")
    if "{mDofPointSampler, mHdr.arrayView, ro}" not in cpp or "{mAASampler, mDofHalf.arrayView, ro}" not in cpp:
        m.append("ensureDoF: a DoF set binds one layer")
    if "ca.imageView=mainAtt(target);" not in cpp or "fullscreen(mHdr, width, height, mDofCombinePipe" not in cpp:
        m.append("recordDoF: the passes are not multiview over every layer")
    if "DOF_UV(uv)" not in _read(ctx, _SH + "clean_dof_common.glsl"):
        m.append("clean_dof_common.glsl: no per-view coordinate")
    for f, a in (("clean_dof_cof.frag", "sampler2DArray uDepth"), ("clean_dof_gather.frag", "sampler2DArray uCoC"),
                 ("clean_dof_combine.frag", "sampler2DArray uDof")):
        if a not in _read(ctx, _SH + f):
            m.append(f + ": reads view 0's layer")
    return m


def _occlusion(ctx, cpp, sh):
    """Two-phase occlusion per view (design step 9): the Hi-Z a layer per view built z = N from each view's depth layer,
    the test an OR over the views (an instance culled only when every view's Hi-Z hides it) through each view's camera
    from the frame info; one P1 / P2 list and one visibility stamp for every view."""
    m = []
    if "occl2Want && !mOffscreen && !stereoActive()" in cpp:
        m.append("recordWorld: the two-phase occlusion cull is still off in stereo")
    if "mHiz = mCtx->createScreenImage(w0, h0, mViewCount," not in cpp or "VK_IMAGE_VIEW_TYPE_2D_ARRAY,\n" \
            "                                              VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, std::max(mHiz.layers, 1u));" not in cpp:
        m.append("ensureHiz: the pyramid is not a layer per view")
    if "viewCountSpec(mViewCount), &mHizPipe" not in cpp or "(dh + 7) / 8, mViewCount);" not in cpp:
        m.append("the Hi-Z build is not specialized with the view count / not dispatched z = N")
    if "viewCountSpec(mViewCount), &mOccl2Pipe" not in cpp or "{mDepthSampler, mHiz.arrayView, VK_IMAGE_LAYOUT_GENERAL}" not in cpp:
        m.append("the occlusion test is not specialized with the view count / binds one pyramid")
    hz = _read(ctx, _SH + "clean_hiz.comp")
    if "#define CLEAN_VIEW CLEAN_VIEW_Z" not in hz or "image2DArray u_dst" not in hz:
        m.append("clean_hiz.comp: not layered")
    o2 = _read(ctx, _SH + "clean_occl2.comp")
    if "sampler2DArray u_hiz" not in o2 or "ssr.views[v].taa_cur_vp" not in o2 \
            or "if (!occludedIn(wmin, wmax, ssr.views[v].taa_cur_vp, ssr.cam_origin.xyz, v)) return false;" not in o2:
        m.append("clean_occl2.comp: the test is not the OR over every view's Hi-Z")
    return m


# (family label, the check, a phrase the stereo-off log line must carry for it, phrases it must no longer carry)
FAMILIES = [
    ("RT reflections", _rt_reflections, "RT reflections by its layered pre-trace", []),
    ("mirrors", _mirrors, "a mirror is ray traced per eye instead", ["planar / RT mirrors"]),
    ("SSR", _ssr, "", ["cache, SSR, VRS"]),
    ("split lighting", _split, "a stereo split frame runs SHADOW / REFLECT / COMBINE", ["the split / "]),
    ("feature split", _feature_split, "the feature split's CLASSIFY / BASE / LOCAL per view", ["the feature-split "]),
    ("skin scattering", _skin_sss, "skin scattering per view", ["skin scattering, "]),
    ("ambient occlusion", _ao, "ambient occlusion (SSAO / GTAO / RTAO", ["]: AO, ", " AO, the merged"]),
    ("split RT reflections", _split_rt_reflections, "with its RT reflections per view",
     ["the split's RT reflection extras", "a frame with RT reflections or VR GI"]),
    ("denoisers", _denoisers, "", []),
    ("merged RT shadows / ReSTIR DI", _pass15, "the merged RT shadows, ReSTIR DI (point-light and projector shadows) "
     "and their denoiser per view", ["shadows + projector denoiser (the layered", "point-light shadows by ReSTIR DI"]),
    ("GI", _gi, "ray-traced GI (RenderVKGI) per view", ["GI (VR GI's world-cache tier"]),
    ("weather", _weather, "the weather draws per view", ["particle fog, weather draws"]),
    ("volumetric fog", _vol_fog, "the volumetric fog per view in both modes", ["volumetric fog / clouds march"]),
    ("froxel light masks", _froxels, "the froxel light masks per view", ["the froxel light masks (the tile light test"]),
    ("volumetric clouds", _vol_clouds, "the volumetric clouds' march / temporal / composite per view",
     ["the volumetric clouds' march / composite,"]),
    ("particle fog", _part_fog, "the particle fog per view", ["god rays, particle fog,"]),
    ("god rays", _god_rays, "god rays per view", ["god rays, motion blur"]),
    ("glow temporal resolve", _glow_temporal, "the glow temporal resolve per view", ["(+ the glow temporal resolve)"]),
    ("bloom", _bloom, "bloom per view -- the pyramid", ["lens flare / dirt, bloom,"]),
    ("LDR glow", _ldr_glow, "the LDR glow per view", ["lens flare / dirt, LDR glow,"]),
    ("order-independent transparency", _oit, "order-independent transparency per view",
     ["transparency (the ordered alpha path instead)"]),
    ("RCAS", _rcas, "RCAS sharpening per view", ["RCAS sharpening (the FSR 3.1"]),
    ("depth of field", _dof, "depth of field per view", ["motion blur, DoF,"]),
    ("occlusion culling", _occlusion, "the two-phase occlusion cull per view", ["#1137), occlusion culling,"]),
]


def run(ctx, result):
    cpp = _read(ctx, _CPP)
    sh = _read(ctx, _SH + "clean_deferred_light.comp")
    log = _log_line(cpp)
    missing = []
    if not log:
        missing.append("initViewCount has no \"Off in stereo until ...\" log line")
    done = []
    for label, fn, says, gone in FAMILIES:
        fam = fn(ctx, cpp, sh)
        if says and says not in log:
            fam.append("the stereo-off log line does not say: " + says)
        for g in gone:
            if g in log:
                fam.append("the stereo-off log line still lists it as off: " + g)
        missing += [label + ": " + x for x in fam]
        if not fam:
            done.append(label)
    if missing:
        result.fail("\n".join(missing))
        return
    result.summary = "layered in stereo: " + ", ".join(done)
