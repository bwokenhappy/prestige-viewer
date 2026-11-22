"""Regression guard for falling precipitation under frame generation (#1517: geometry behind snow / rain warps).

The weather particles (clean_weather.vert / .frag) write neither depth nor motion vectors. Blended into the
scene they reach the generator's world input (the hudless colour), and the generator's flow follows the
flakes while the motion under them says "background": the scene behind falling snow and rain warps and
drags. The fix: on every frame that feeds the generator -- DLSS-G and FSR FG alike, HDR10 and the SDR
layer -- the falling particles leave pass 2b and are drawn after the post chain into the UI layer, through
the scene's own tonemap tail (LLCleanRendererVK::wxLateWanted, LLCleanWeatherVK::recordDrawLate).

Source rules:
  default    no setting can turn it off (RenderVKFrameGenWeather defaulted to 0 = the warp); the routing in
             recordWorld reads no setting; the late draw is reached and fed the tonemap pass's push block
  generators the frame flag is set from the frame's generation count and its layer only (never from a
             generator kind); every generator reads its colour from GenInputs::color, which is the slot's
             world image (fgHudless) -- so no generator path sees a world the weather handling skipped
  shaders    the late variants exist (vert: the unjitter; frag: tonemapTail, no private tonemap copy) and
             every late-only line is inside #ifdef WEATHER_LATE: the base variants' preprocessed text holds
             none of it (frame generation off keeps its SPIR-V); both late variants are in the regen list
  pipeline   the late pipeline blends premultiplied ("over" on colour and coverage), and that flag reaches
             the driver through the in-process path and the compile worker's recipe
"""
import os
import re
import shutil
import subprocess

NAME = "framegen-weather"
STAGE = "source"
DESCRIPTION = "frame generation: falling rain / snow kept out of the generator's world (source rules)"
GUARDS = ("#1517: geometry behind falling snow / rain warping under frame generation -- the late "
          "precipitation off by default, a generator path whose world still holds the particles, the "
          "late shader code leaking into the base weather SPIR-V.")

RENDERER = "indra/newview/vkgfx/llcleanrenderervk.cpp"
WEATHER = "indra/newview/vkgfx/llcleanweathervk.cpp"
CONTEXT = "indra/newview/vkgfx/llvulkancontext.cpp"
SETTINGS = "indra/newview/app_settings/settings.xml"
FRAG = "indra/vkclean/shaders/clean_weather.frag"
VERT = "indra/vkclean/shaders/clean_weather.vert"
REGEN = "indra/vkclean/regen_world_spv.py"
PIPELINE = "indra/newview/vkgfx/vk_pipeline.cpp"
WORKER = "indra/newview/vkgfx/vk_pipeworker.cpp"
GENERATOR_DIR = "indra/newview/vkgfx"


def _read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def _function_body(text, signature):
    """The body of the function whose definition starts with `signature` (to the closing brace in column 0)."""
    i = text.find(signature)
    if i < 0:
        return None
    j = text.find("\n}\n", i)
    return text[i:j + 2] if j >= 0 else text[i:]


def _call_args(text, start):
    """The argument text of the call whose '(' is at or after `start` (balanced parentheses)."""
    i = text.find("(", start)
    if i < 0:
        return None
    depth = 0
    for k in range(i, len(text)):
        if text[k] == "(":
            depth += 1
        elif text[k] == ")":
            depth -= 1
            if depth == 0:
                return text[i + 1:k]
    return None


def _squash(s):
    return re.sub(r"\s+", "", s)


def rules_default(ctx):
    problems = []
    settings = _read(ctx, SETTINGS)
    for key in re.findall(r"<key>(\w*FrameGenWeather\w*)</key>", settings):
        problems.append("%s: setting %s can take the falling precipitation back into the generator's world "
                        "(#1517: its default 0 was the warp); the handling has no switch" % (SETTINGS, key))

    r = _read(ctx, RENDERER)
    wanted = _function_body(r, "bool LLCleanRendererVK::wxLateWanted() const")
    if wanted is None:
        problems.append("%s: LLCleanRendererVK::wxLateWanted() is gone (the frame-generation weather gate)" % RENDERER)
    else:
        if "mFrameGenFrame" not in wanted:
            problems.append("%s: wxLateWanted() no longer keys on mFrameGenFrame (the frame feeds the generator)"
                            % RENDERER)
        for bad in ("gSavedSettings", "LLCachedControl"):
            if bad in wanted:
                problems.append("%s: wxLateWanted() reads a setting (%s): the weather handling must be the "
                                "default on every frame that feeds the generator" % (RENDERER, bad))
        for gen in ("Dlss", "DLSS", "Fsr", "FSR", "Generator"):
            if gen in wanted:
                problems.append("%s: wxLateWanted() names a generator (%s): both generators read the same world "
                                "and need the same handling" % (RENDERER, gen))

    world = _function_body(r, "void LLCleanRendererVK::recordWorld(VkCommandBuffer cmd, VkImageView colorView,")
    if world is None:
        problems.append("%s: recordWorld not found" % RENDERER)
        return problems
    a = world.find("if (wxLateWanted())")
    b = world.find("if (mWeather && mWeatherNow && mWeather->precipNow() && !mSnapMatte)", a if a >= 0 else 0)
    if a < 0 or b < 0:
        problems.append("%s: recordWorld's pass-2b weather draw no longer asks wxLateWanted() before drawing the "
                        "precipitation" % RENDERER)
    else:
        start = world.rfind("mWxLateNow = false;", 0, a)   # (the gate's own lines, from its reset on)
        gate = world[start if start >= 0 else a:b]
        for need, what in (("lateReady(", "the late pipeline asked for / warmed"),
                           ("wxPrecipInScene = false", "the falling particles taken out of the scene"),
                           ("mWxLateNow = ready", "the late draw armed once its pipeline exists")):
            if need not in gate:
                problems.append("%s: the frame-generation weather gate lost %s (`%s`)" % (RENDERER, what, need))
        for bad in ("gSavedSettings", "LLCachedControl"):
            if bad in gate:
                problems.append("%s: the frame-generation weather gate reads a setting (%s)" % (RENDERER, bad))
        if not re.search(r"recordDraw\(cmd, false(?:, this)?\)", world[b:b + 900]):
            problems.append("%s: pass 2b no longer draws the splashes / streams alone when the falling particles "
                            "go late (recordDraw(cmd, false))" % RENDERER)
    late = world.find("recordDrawLate(")
    if late < 0:
        problems.append("%s: recordWorld no longer calls recordDrawLate: the precipitation taken out of the scene "
                        "is never drawn on frame-generation frames" % RENDERER)
    else:
        args = _squash(_call_args(world, late) or "")
        if "mWxLateTp" not in args or "colorView" not in args or "mDsetGrade" not in args:
            problems.append("%s: recordDrawLate is not given the world view, the grade set and the tonemap push "
                            "block (mWxLateTp): the late particles would not match the scene's colour" % RENDERER)
        if world.find("mUiLayerView", world.rfind("if (mWxLateNow", 0, late)) < 0:
            problems.append("%s: the late precipitation scope no longer draws into the UI layer" % RENDERER)
    if not re.search(r"std::memcpy\(\s*mWxLateTp\s*,\s*tp\s*,", r):
        problems.append("%s: recordTonemap no longer keeps its push block for the late precipitation "
                        "(std::memcpy(mWxLateTp, tp, ...))" % RENDERER)
    return problems


def rules_generators(ctx):
    problems = []
    c = _read(ctx, CONTEXT)
    i = c.find("setFrameGenFrame(")
    if i < 0:
        problems.append("%s: setFrameGenFrame is no longer called: no frame is marked as feeding the generator"
                        % CONTEXT)
    else:
        expr = _call_args(c, i) or ""
        idents = set(re.findall(r"[A-Za-z_]\w*", expr))
        allowed = {"fgCount", "layer", "vrWorld", "sdrFrame"}
        if "fgCount" not in idents or "layer" not in idents:
            problems.append("%s: setFrameGenFrame(%s) must follow the frame's generation count and its UI layer "
                            "(HDR10 or the SDR layer)" % (CONTEXT, expr.strip()))
        extra = sorted(idents - allowed)
        if extra:
            problems.append("%s: setFrameGenFrame(%s) depends on %s: every frame that is generated from -- any "
                            "generator, any output -- must take the weather out of its world"
                            % (CONTEXT, expr.strip(), ", ".join(extra)))
        rw = c.find("->recordWorld(cmd, worldView", i)
        if rw < 0:
            problems.append("%s: setFrameGenFrame is no longer set before the frame's recordWorld" % CONTEXT)
    if not re.search(r"e\.color\s*=\s*llfg::imageRef\(\s*f\.fgHudless\(\s*slot\s*\)\s*\)", c):
        problems.append("%s: fillGenInputs no longer hands the generator the slot's world image (fgHudless) as its "
                        "colour" % CONTEXT)

    # Every generator: its colour is GenInputs::color, nothing else.
    gens = []
    d = ctx.path(GENERATOR_DIR)
    for name in sorted(os.listdir(d)):
        if not name.endswith((".h", ".cpp")):
            continue
        with open(os.path.join(d, name), encoding="utf-8", errors="replace") as f:
            text = f.read()
        for m in re.finditer(r"class\s+(\w+)\s*(?:final\s*)?:\s*public\s+LLVKFrameGenGenerator\b", text):
            gens.append(m.group(1))
    if len(gens) < 2:
        problems.append("%s: expected the DLSS-G and FSR generators to implement LLVKFrameGenGenerator, found %s"
                        % (GENERATOR_DIR, ", ".join(gens) or "none"))
    for g in gens:
        found = False
        for name in sorted(os.listdir(d)):
            if not name.endswith(".cpp"):
                continue
            with open(os.path.join(d, name), encoding="utf-8", errors="replace") as f:
                text = f.read()
            if ("bool %s::evaluate(" % g) not in text:
                continue
            found = True
            if not re.search(r"\bin\.color\.(image|view)\b", text):
                problems.append("%s: %s's evaluate does not read GenInputs::color (the world without the "
                                "precipitation)" % (name, g))
            if re.search(r"\b(backbuffer|uiLayer|fgUi)\s*\(", text):
                problems.append("%s: %s reads a composed picture (backbuffer / UI layer): its world would hold "
                                "the late precipitation" % (name, g))
        if not found:
            problems.append("%s: %s::evaluate not found" % (GENERATOR_DIR, g))
    return problems


def _preprocess(ctx, path, stage, defines):
    glslc = os.environ.get("GLSLC") or shutil.which("glslc")
    if not glslc:
        return None
    r = subprocess.run([glslc, "-E", "-fshader-stage=" + stage, "--target-env=vulkan1.3", *defines, path],
                       capture_output=True, text=True)
    if r.returncode:
        return "ERROR:" + r.stderr
    return r.stdout


def rules_shaders(ctx, result):
    problems = []
    frag = _read(ctx, FRAG)
    vert = _read(ctx, VERT)
    if '#include "clean_tonemap_common.glsl"' not in frag or "tonemapTail(" not in frag:
        problems.append("%s: the late variant no longer goes through the scene's tonemap tail (tonemapTail): an "
                        "approximate tonemap made the late flakes a different colour from the scene" % FRAG)
    if re.search(r"vec3\s+\w*(Neutral|Tonemap|ToneMap)\w*\s*\(", frag):
        problems.append("%s: a private tonemap function is back in the weather shader (use tonemapTail)" % FRAG)
    if "uLateWorld" not in frag:
        problems.append("%s: the late variant no longer reads the world under the particle (the linear-light "
                        "blend through the UI layer's encoded composite)" % FRAG)
    if "unjitter" not in vert:
        problems.append("%s: the late variant no longer takes the main view's jitter out (no upscaler removes it "
                        "after the post chain: the flakes would shimmer)" % VERT)
    regen = _squash(_read(ctx, REGEN))
    for stage, sym in (("vert", "kWorldWeatherLateVertSpv"), ("frag", "kWorldWeatherLateFragSpv")):
        entry = '("clean_weather.%s","%s","%s",["-DWEATHER_LATE"])' % (stage, stage, sym)
        if entry not in regen:
            problems.append("%s: the late weather %s variant (%s, -DWEATHER_LATE) is not in SHADERS" % (REGEN, stage, sym))
    # The base variants (frame generation off, and the splash / stream twins) must hold no late code.
    late_markers = ("uLateWorld", "uLateExposure", "tonemapTail", "unjitter", "WeatherLateVsPush")
    ran = False
    for rel, stage in ((FRAG, "frag"), (VERT, "vert")):
        for defs in ([], ["-DWEATHER_SPLASH"], ["-DWEATHER_STREAM"]):
            text = _preprocess(ctx, ctx.path(rel), stage, defs)
            if text is None:
                continue
            ran = True
            if text.startswith("ERROR:"):
                problems.append("%s %s: glslc -E failed:\n%s" % (rel, " ".join(defs), text[6:]))
                continue
            leaked = [m for m in late_markers if m in text]
            if leaked:
                problems.append("%s %s: the base variant holds late-only code (%s): frame generation off would "
                                "change its SPIR-V" % (rel, " ".join(defs) or "(base)", ", ".join(leaked)))
        late = _preprocess(ctx, ctx.path(rel), stage, ["-DWEATHER_LATE"])
        if late is not None and not late.startswith("ERROR:") and stage == "frag" and "tonemapTail" not in late:
            problems.append("%s -DWEATHER_LATE: the late variant does not call tonemapTail" % rel)
    if not ran:
        result.warn("no glslc: the base weather variants were not preprocessed (late code leak not checked)")
    return problems


def rules_pipeline(ctx):
    problems = []
    w = _read(ctx, WEATHER)
    ready = _function_body(w, "bool LLCleanWeatherVK::lateReady(")
    if ready is None:
        problems.append("%s: LLCleanWeatherVK::lateReady not found" % WEATHER)
    else:
        if "d.premultipliedOver = true" not in ready:
            problems.append("%s: the late precipitation pipeline no longer blends premultiplied (its texel is the "
                            "UI layer's premultiplied colour + coverage)" % WEATHER)
        if "kWorldWeatherLateVertSpv" not in ready or "kWorldWeatherLateFragSpv" not in ready:
            problems.append("%s: the late precipitation pipeline does not use both late shader variants" % WEATHER)
    p = _read(ctx, PIPELINE)
    if not re.search(r"if\s*\(\s*desc\.premultipliedOver\s*\)", p):
        problems.append("%s: createGraphicsPipeline no longer honours GraphicsPipelineDesc::premultipliedOver" % PIPELINE)
    if not re.search(r"if\s*\(\s*d\.premultipliedOver\s*\)\s*pv\(", p):
        problems.append("%s: the pipeline key no longer includes premultipliedOver (a cached pipeline without "
                        "it could be reused)" % PIPELINE)
    k = _read(ctx, WORKER)
    if "w.u32(d.premultipliedOver)" not in k or "d.premultipliedOver = r.u32()" not in k:
        problems.append("%s: the compile worker's recipe no longer carries premultipliedOver (a worker-built late "
                        "pipeline would blend straight)" % WORKER)
    return problems


def run(ctx, result):
    for rel in (RENDERER, WEATHER, CONTEXT, SETTINGS, FRAG, VERT, REGEN, PIPELINE, WORKER):
        if not os.path.exists(ctx.path(rel)):
            result.fail("%s is missing" % rel)
            result.summary = "source missing"
            return
    problems = rules_default(ctx) + rules_generators(ctx) + rules_shaders(ctx, result) + rules_pipeline(ctx)
    for p in problems:
        result.fail(p)
    result.summary = "rules hold" if not problems else "%d rule(s) broken" % len(problems)
