"""Regression guard for #1544 FSR 3.1 frame generation.

Source rules:
  VENDOR  AMD's FSR 3.1 frame interpolation and optical flow GPU sources are vendored as source, as the
          upscaler is (indra/vkclean/third_party/fidelityfx): every vendored file under the two new
          gpu/ folders and shaders/ folders starts with the SDK's MIT notice; the README names the tag,
          the commit and both new folders; a change to a vendored file is marked SLIPSTREAM PATCH; no
          binaries (.dll / .so / .lib) and no HLSL callback files are vendored; the regen script builds
          every pass (FSR_SHADERS), and each kFsrFg* name the FSR backend (llvkframegenfsr.cpp) uses has
          an entry there.
  MATH    the host maths (indra/newview/vkgfx/llvkframegenfsrmath.h) is pure -- it includes standard headers
          only -- and is the one derivation: the FSR upscaler (llcleanfsr3vk.cpp) takes its device-depth
          factors from llfsr::deviceToViewDepth, not from a copy of it.
  THREAD  the backend (llvkframegenfsr.cpp) makes, writes and destroys its objects on the main thread only:
          evaluate and everything it calls (the present thread) contain no descriptor write, no image, view,
          buffer, pipeline, layout or pool create / destroy / allocate, no immediate submit, no wait for
          the device or a queue, and no vkBreadcrumb (its marker state is the main thread's).
  ORDER   recordPrepare, recordOpticalFlow and recordInterpolation are private and called once each, in that
          order, from evaluate only; each records its stage of llfsrfg::passPlan (the tested order) through
          recordStep.
  UNITS   the backend names nothing of another generator's conventions: no mvScale, FgEval, ViewConstants,
          LLVKNeuralRecon, and not the jitter sign another upscaler applied (jitterSign).
  GPU-DRIVEN  no readback and no per-pixel work on the CPU: no vkMapMemory, no vkCmdCopyImageToBuffer, no
          query results, and no loop over an image's width or height.
  CONST   constants reach the GPU only through the tested fillers: the constant ring's memory is written in
          stageConstants only, each block copied there is one a llfsrfg::fill* function returned, and nothing
          is pushed (no vkCmdPushConstants).
  FORMAT  the output-format variant of the three passes that write the generated frame is chosen from the
          scratch image's format in one function (outputVariant), which prepare alone asks.
  STORAGE a storage image AMD's GLSL declares with a format is made in exactly that format: every image the
          backend makes for an `rw_...` declaration (mk(..., VK_FORMAT_..., "rw_...")) has the declaration's
          format, and every such declaration with a format has an image.

  TEXT    (Task 7) what the player and a setting read names both generators: the FrameGen tooltip and
          RenderVKFrameGen's comment say "FSR frame generation" and "DLSS or FSR upscaling" -- never DLSS
          upscaling, nor an RTX 40 / 50, as the only way; the Preferences status starts with the generator in
          use (llfg::statusPrefix / llfg::generatorName of LLVKFrameGen::generator()).

  PREFS   Preferences offers the generator (combo FrameGenGenerator, RenderVKFrameGenGenerator: Automatic 0, DLSS 1,
          FSR 2), enabled exactly when frame generation is set up (directReady), DLSS and FSR listed only when
          they can run on this GPU (LLVKFrameGen::generatorCanRun); the multiplier combo lists exactly the
          multipliers multiplierOffered allows (FSR: Off and 2x), none greyed.

  ANTILAG (#1544) AMD Anti-Lag (VK_AMD_anti_lag): the device enables the extension only with its feature
          (VkPhysicalDeviceAntiLagFeaturesAMD::antiLag queried and chained), dropped with frame generation's
          optional extensions on a retry; the main loop marks the INPUT stage once a frame after its frame
          wait (LLVulkanContext::antiLagInput), renderFrame the PRESENT stage before the frame's present /
          hand-over with the same frame index; the mode follows RenderVKAntiLag (on unless set to 0), maxFPS 0
          (the viewer's own limiter caps).

Compiles and runs scripts/check/test_framegen_fsr.cpp on the host (the maths header alone): the constant
buffers' layouts and contents, sizes, dispatch counts, the ping-pong selection, the pass order, the reset
rule, the camera-pan motion-units case (and its mis-stated-units cases, which must fail) and the memory figures.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "framegen-fsr"
STAGE = "source"
DESCRIPTION = "FSR frame generation: vendored AMD sources, their build, the host maths (tests), the backend's rules"
GUARDS = ("AMD binaries or HLSL-only files slipping into the tree, a vendored file losing its licence notice, "
          "an unmarked change to AMD's code, a frame generation pass the backend uses but the build never "
          "compiles; a constant buffer, dispatch size or pass order that drifts from AMD's host code; motion "
          "vectors handed to FSR in the wrong units (every generated frame warps); a descriptor write or an "
          "allocation on the present thread; a storage image made in another format than its shader declares.")

FFX = "indra/vkclean/third_party/fidelityfx"
README = FFX + "/README.md"
REGEN = "indra/vkclean/regen_world_spv.py"
BACKEND = "indra/newview/vkgfx/llvkframegenfsr.cpp"
BACKEND_H = "indra/newview/vkgfx/llvkframegenfsr.h"
MATH = "indra/newview/vkgfx/llvkframegenfsrmath.h"
UPSCALER = "indra/newview/vkgfx/llcleanfsr3vk.cpp"
CALLBACKS = (FFX + "/gpu/frameinterpolation/ffx_frameinterpolation_callbacks_glsl.h",
             FFX + "/gpu/opticalflow/ffx_opticalflow_callbacks_glsl.h")
CLASS = "LLVKFrameGenFsr"
# Calls that make, write or destroy a Vulkan object, or wait: main thread only.
MAIN_ONLY = ("vkUpdateDescriptorSets", "createImage", "createBuffer", "createView", "destroyImage", "destroyBuffer",
             "destroyView", "vkCreate", "vkDestroy", "vkdCreate", "vkdDestroy", "createComputePipeline", "vkAllocate",
             "vkFree", ".allocate(", ".create(", ".destroy(", "submitImmediate", "vkDeviceWaitIdle", "vkQueueWaitIdle",
             "vkQueueSubmit", "setObjectName",
             "vkBreadcrumb")   # (its marker ring and sequence number are the main thread's)
# GLSL storage format qualifier -> VkFormat.
GLSL_FORMAT = {"r32f": "VK_FORMAT_R32_SFLOAT", "rg16f": "VK_FORMAT_R16G16_SFLOAT", "r32ui": "VK_FORMAT_R32_UINT",
               "rg8": "VK_FORMAT_R8G8_UNORM", "rgba16f": "VK_FORMAT_R16G16B16A16_SFLOAT", "r8ui": "VK_FORMAT_R8_UINT",
               "rg16i": "VK_FORMAT_R16G16_SINT"}
TEST = "scripts/check/test_framegen_fsr.cpp"
FOLDERS = ("gpu/frameinterpolation", "gpu/opticalflow", "shaders/frameinterpolation", "shaders/opticalflow")
TAG = "v1.1.4"
COMMIT = "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55"
MIT = "Permission is hereby granted, free of charge"
PASSES = {
    "frameinterpolation": ("setup", "reconstruct_and_dilate", "reconstruct_previous_depth",
                           "game_motion_vector_field", "optical_flow_vector_field", "disocclusion_mask",
                           "compute_game_vector_field_inpainting_pyramid", "compute_inpainting_pyramid",
                           "inpainting", "debug_view", ""),   # ("": ffx_frameinterpolation_pass.glsl)
    "opticalflow": ("prepare_luma", "compute_luminance_pyramid", "generate_scd_histogram",
                    "compute_scd_divergence", "compute_optical_flow_advanced", "filter_optical_flow",
                    "scale_optical_flow_advanced"),
}


def _read(ctx, rel):
    p = ctx.path(rel)
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def source_rules(ctx):
    bad = []
    for folder in FOLDERS:
        d = ctx.path(FFX + "/" + folder)
        files = sorted(os.listdir(d)) if os.path.isdir(d) else []
        if not files:
            bad.append("VENDOR: %s/%s is missing or empty" % (FFX, folder))
            continue
        for name in files:
            rel = "%s/%s/%s" % (FFX, folder, name)
            if name.endswith("_hlsl.h") or name.endswith(".hlsl"):
                bad.append("VENDOR: %s is an HLSL file (the Vulkan build uses the GLSL callbacks only)" % rel)
            text = _read(ctx, rel) or ""
            if MIT not in text[:3000]:
                bad.append("VENDOR: %s does not start with the SDK's MIT notice" % rel)
    for root, _dirs, names in os.walk(ctx.path(FFX)):
        for name in names:
            if name.lower().endswith((".dll", ".so", ".lib", ".a", ".dylib", ".exe")):
                bad.append("VENDOR: binary %s under %s" % (name, FFX))
    readme = _read(ctx, README) or ""
    for need in (TAG, COMMIT) + FOLDERS:
        if need not in readme:
            bad.append("VENDOR: %s does not name %s" % (README, need))
    if "SLIPSTREAM PATCH" not in readme or "FFX_FRAMEINTERPOLATION_OUTPUT_FORMAT" not in readme:
        bad.append("VENDOR: %s lists the frame interpolation output-format patch" % README)
    cb = _read(ctx, FFX + "/gpu/frameinterpolation/ffx_frameinterpolation_callbacks_glsl.h") or ""
    if "SLIPSTREAM PATCH" not in cb or "FFX_FRAMEINTERPOLATION_OUTPUT_FORMAT" not in cb:
        bad.append("VENDOR: the output-format patch in ffx_frameinterpolation_callbacks_glsl.h is not marked "
                   "SLIPSTREAM PATCH")
    regen = _read(ctx, REGEN) or ""
    quoted = {q for line in regen.splitlines() for q in re.findall(r'"([^"\n]*)"', line.split("#", 1)[0])}
    for kind in ("frameinterpolation", "opticalflow"):
        d = ctx.path(FFX + "/shaders/" + kind)
        for name in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            if not name.endswith(".glsl"):
                continue
            stem = name[:-5]
            short = stem[len("ffx_%s_" % kind):] if stem.startswith("ffx_%s_" % kind) else ""
            names = {name, stem, short, short[:-5] if short.endswith("_pass") else short,
                     "" if stem == "ffx_frameinterpolation_pass" else None}
            if not any(n is not None and n in quoted for n in names):
                bad.append("VENDOR: %s does not build %s" % (REGEN, name))
    for kind, names in PASSES.items():
        d = ctx.path(FFX + "/shaders/" + kind)
        present = set(os.listdir(d)) if os.path.isdir(d) else set()
        for n in names:
            fname = "ffx_%s%s_pass" % (kind, "_" + n if n else "")
            if fname + ".glsl" not in present and fname + "_v5.glsl" not in present:
                bad.append("VENDOR: %s/shaders/%s has no %s.glsl" % (FFX, kind, fname))
    backend = _read(ctx, BACKEND)
    if backend is not None:
        built = set(re.findall(r"\"(kFsrFg\w+)\"", regen))
        for used in sorted(set(re.findall(r"\b(kFsrFg\w+?)Spv\b", backend))):
            if used + "Spv" not in built:
                bad.append("VENDOR: %s uses %sSpv, which %s does not build" % (BACKEND, used, REGEN))
    return bad


def _code(text):
    """C++ / GLSL source without comments (newlines kept) and with string contents kept."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _functions(code):
    """{name: body} of every function defined in the backend: the class's members and the file's own."""
    found = {}
    for m in re.finditer(r"(?:^|\n)[ \t]*(?:static\s+|inline\s+|constexpr\s+)*[\w:<>\*&\s]+?\b((?:%s::)?~?\w+)\s*\(" % CLASS, code):
        name = m.group(1)
        # the parameter list, then `{` (a definition) -- not `;` (a declaration or a call statement)
        depth, i = 0, m.end() - 1
        while i < len(code):
            if code[i] == "(":
                depth += 1
            elif code[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        j = i + 1
        while j < len(code) and (code[j].isspace() or code.startswith("const", j) or code.startswith("override", j)):
            j += 5 if code.startswith("const", j) else 8 if code.startswith("override", j) else 1
        if j >= len(code) or code[j] != "{":
            continue
        depth, k = 0, j
        while k < len(code):
            if code[k] == "{":
                depth += 1
            elif code[k] == "}":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        short = name.split("::")[-1]
        if short in ("if", "for", "while", "switch", "return", "sizeof"):
            continue
        found[short] = found.get(short, "") + code[j:k + 1]
    return found


def _reachable(funcs, start):
    """Names of the backend's functions `start` calls, directly or not (itself included)."""
    seen, todo = set(), [start]
    while todo:
        f = todo.pop()
        if f in seen or f not in funcs:
            continue
        seen.add(f)
        for g in funcs:
            if g not in seen and re.search(r"(?<![\w.>])%s\s*\(" % re.escape(g), funcs[f]):
                todo.append(g)
    return seen


def math_rules(ctx):
    bad = []
    math = _read(ctx, MATH)
    if math is None:
        return ["missing: %s" % MATH]
    for inc in re.findall(r"#\s*include\s*([<\"][^>\"]+[>\"])", _code(math)):
        if not inc.startswith("<") or "vulkan" in inc or "/" in inc:
            bad.append("MATH: %s includes %s (standard headers only: the host test compiles it alone)" % (MATH, inc))
    up = _code(_read(ctx, UPSCALER) or "")
    if "llfsr::deviceToViewDepth(" not in up or re.search(r"invProj\s*\[\s*1[145]\s*\]", up):
        bad.append("MATH: %s must take its device-depth factors from llfsr::deviceToViewDepth (one derivation, "
                   "shared with frame generation), not from its own copy" % UPSCALER)
    return bad


def backend_rules(ctx):
    bad = []
    raw = _read(ctx, BACKEND)
    header = _read(ctx, BACKEND_H)
    if raw is None or header is None:
        return ["missing: %s" % ", ".join(p for p, t in ((BACKEND, raw), (BACKEND_H, header)) if t is None)]
    code = _code(raw)
    hcode = _code(header)
    funcs = _functions(code)

    # THREAD
    if "evaluate" not in funcs:
        bad.append("THREAD: %s defines no %s::evaluate" % (BACKEND, CLASS))
    present = _reachable(funcs, "evaluate")
    for f in sorted(present):
        for call in MAIN_ONLY:
            if call in funcs[f]:
                bad.append("THREAD: %s::%s runs on the present thread (evaluate reaches it) and names %s -- objects are "
                           "made, written and destroyed in prepare / release only" % (CLASS, f, call.strip(".(")))
    for need in ("vkUpdateDescriptorSets", "createImage2D", "createComputePipeline"):
        owners = sorted(f for f in funcs if need in funcs[f])
        if not owners:
            bad.append("THREAD: %s never calls %s (the backend makes its own objects)" % (BACKEND, need))
        main = _reachable(funcs, "prepare") | _reachable(funcs, "release") | _reachable(funcs, "init") | _reachable(funcs, "shutdown")
        for f in owners:
            if f not in main:
                bad.append("THREAD: %s::%s calls %s but is not reached from init / prepare / release / shutdown" % (CLASS, f, need))

    # ORDER
    private = hcode[hcode.find("private:"):] if "private:" in hcode else ""
    stages = ("recordPrepare", "recordOpticalFlow", "recordInterpolation")
    ev = funcs.get("evaluate", "")
    at = []
    for name in stages:
        if not re.search(r"\b%s\s*\(" % name, private):
            bad.append("ORDER: %s must declare %s private" % (BACKEND_H, name))
        calls = [f for f in funcs if f != name and re.search(r"(?<![\w.>:])%s\s*\(" % name, funcs[f])]
        if calls != ["evaluate"] or len(re.findall(r"(?<![\w.>:])%s\s*\(" % name, ev)) != 1:
            bad.append("ORDER: %s must be called once, from evaluate only (called from: %s)" % (name, ", ".join(sorted(calls)) or "nowhere"))
        m = re.search(r"(?<![\w.>:])%s\s*\(" % name, ev)
        at.append(m.start() if m else -1)
        body = funcs.get(name, "")
        if "recordStep(" not in body or "llfsrfg::stageOf(" not in body:
            bad.append("ORDER: %s must record its stage of the plan (llfsrfg::stageOf) through recordStep" % name)
    if -1 not in at and at != sorted(at):
        bad.append("ORDER: evaluate must call recordPrepare, recordOpticalFlow, recordInterpolation in that order")
    if "llfsrfg::passPlan(" not in ev:
        bad.append("ORDER: evaluate must take the passes from llfsrfg::passPlan (the tested order)")

    # UNITS
    for word in ("mvScale", "FgEval", "ViewConstants", "LLVKNeuralRecon", "jitterSign"):
        if re.search(r"\b%s\b" % word, code) or re.search(r"\b%s\b" % word, hcode):
            bad.append("UNITS: %s names %s -- FSR derives its units from the renderer's own terms (llfg::GenInputs), "
                       "never from another generator's" % (BACKEND, word))

    # GPU-DRIVEN
    for word in ("vkMapMemory", "vkCmdCopyImageToBuffer", "vkGetQueryPoolResults", "vkCmdCopyQueryPoolResults"):
        if word in code:
            bad.append("GPU-DRIVEN: %s names %s (no readback: every pass is a compute dispatch)" % (BACKEND, word))
    for m in re.finditer(r"\bfor\s*\(([^;{}]*;[^;{}]*;[^{}]*?)\)\s*[{\n]", code):
        if re.search(r"\b(width|height|displayW|displayH|renderW|renderH)\b", m.group(1)):
            bad.append("GPU-DRIVEN: %s loops over an image's size on the CPU: `for (%s)`" % (BACKEND, " ".join(m.group(1).split())))

    # CONST
    if "vkCmdPushConstants" in code:
        bad.append("CONST: %s pushes constants (they go through the constant ring and the tested fillers only)" % BACKEND)
    for f in sorted(present):
        if f != "stageConstants" and re.search(r"\bmapped\b", funcs[f]):
            bad.append("CONST: %s::%s touches the constant ring's memory; only stageConstants writes it" % (CLASS, f))
    sc = funcs.get("stageConstants", "")
    if not sc or "stageConstants" not in present:
        bad.append("CONST: %s must write the frame's constants in %s::stageConstants, reached from evaluate" % (BACKEND, CLASS))
    else:
        filled = set(re.findall(r"const\s+llfsrfg::\w+Constants\s+(\w+)\s*=\s*llfsrfg::fill\w+Constants\s*\(", sc))
        copies = re.findall(r"std::memcpy\s*\([^,]+,\s*&\s*(\w+)\s*,\s*sizeof\s*\(\s*(\w+)\s*\)\s*\)", sc)
        if not copies or len(copies) != sc.count("memcpy"):
            bad.append("CONST: every copy in stageConstants must be `std::memcpy(<ring>, &block, sizeof(block))`")
        for src, sized in copies:
            if src != sized or src not in filled:
                bad.append("CONST: stageConstants copies `%s`, which is not a block a llfsrfg::fill*Constants call "
                           "returned" % src)
        for need in ("fillFiConstants", "fillPyramidConstants", "fillOfConstants", "fillOfSpdConstants"):
            if "llfsrfg::%s(" % need not in sc:
                bad.append("CONST: stageConstants must fill its block with llfsrfg::%s" % need)

    # FORMAT
    if len(re.findall(r"\boutputVariant\s*\(", code)) != 2 or "outputVariant(" not in funcs.get("prepare", "") \
            or "outputVariant" not in funcs:
        bad.append("FORMAT: the output-format variant must be chosen in one function, outputVariant(format), which "
                   "prepare alone calls")
    else:
        ov = funcs["outputVariant"]
        if "VK_FORMAT_A2B10G10R10_UNORM_PACK32" not in ov or "VK_FORMAT_R8G8B8A8_UNORM" not in ov:
            bad.append("FORMAT: outputVariant must answer for the HDR10 scratch format (A2B10G10R10: rgb10_a2) and "
                       "the SDR one (R8G8B8A8: rgba8)")
        if not re.search(r"const\s+int32_t\s+variant\s*=\s*outputVariant\s*\(\s*size\.scratchFormat\s*\)\s*;", funcs["prepare"]):
            bad.append("FORMAT: prepare must choose the variant from the scratch image's format "
                       "(`const int32_t variant = outputVariant(size.scratchFormat);`)")
    for rhs in re.findall(r"\bmVariant\s*=(?!=)\s*([^;]*);", code):
        if rhs.strip() not in ("variant", "-1"):
            bad.append("FORMAT: mVariant is set to `%s`; only prepare's outputVariant answer (or -1) may be" % rhs.strip())
    for f in sorted(present):
        if re.search(r"VK_FORMAT_(A2B10G10R10|R8G8B8A8|B8G8R8A8)", funcs[f]):
            bad.append("FORMAT: %s::%s (present thread) decides by a colour format; the variant is prepare's" % (CLASS, f))

    # STORAGE
    declared = {}
    for rel in CALLBACKS:
        for m in re.finditer(r"layout\s*\(([^)]*)\)\s*(?:coherent\s+)?uniform\s+[iu]?image2D\s+(rw_\w+)\s*;", _code(_read(ctx, rel) or "")):
            quals = [q.strip() for q in m.group(1).split(",")]
            fmt = [q for q in quals if q in GLSL_FORMAT]
            name = re.sub(r"(_level_\d+|_next_level|\d+)$", "", m.group(2))
            if fmt:
                declared.setdefault(name, set()).add(GLSL_FORMAT[fmt[0]])
    made = {}
    for m in re.finditer(r"\bmk\s*\(([^;]*?),\s*(VK_FORMAT_\w+|\w+)\s*,\s*\"(r\w+)\"", code):
        made.setdefault(m.group(3), set()).add(m.group(2))
    if not declared:
        bad.append("STORAGE: no storage image declaration with a format found in the GLSL callbacks")
    for name, fmts in sorted(declared.items()):
        if len(fmts) != 1:
            bad.append("STORAGE: %s is declared with more than one format in the callbacks: %s" % (name, ", ".join(sorted(fmts))))
        if name not in made:
            bad.append("STORAGE: the callbacks declare %s (%s) but %s makes no image for it (mk(..., format, \"%s\"))"
                       % (name, ", ".join(sorted(fmts)), BACKEND, name))
        elif made[name] != fmts:
            bad.append("STORAGE: %s is declared %s in the callbacks but made as %s -- a storage image's format "
                       "must be its declaration's" % (name, ", ".join(sorted(fmts)), ", ".join(sorted(made[name]))))
    for name in sorted(made):
        if name.startswith("rw_") and name not in declared and name != "rw_output":
            bad.append("STORAGE: %s makes an image for %s, which the callbacks do not declare with a format" % (BACKEND, name))
    return bad


def run_tests(ctx, result):
    """Compiles and runs the host maths test. True: it ran and passed."""
    missing = [p for p in (MATH, TEST) if not os.path.exists(ctx.path(p))]
    if missing:
        result.fail("missing: %s" % ", ".join(missing))
        return False
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: FSR frame generation maths tests not run")
        return False
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_framegen_fsr")
        # (the header alone: no Vulkan, no viewer headers, no stubs on the include path)
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + ctx.path("indra/newview/vkgfx"),
                            ctx.path(TEST), "-o", exe], capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return False
        try:
            r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            result.fail("the test timed out after 120 s")
            return False
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return False
    return True


PREFS_XUI = "indra/newview/skins/default/xui/en/floater_preferences_graphics_advanced.xml"
PREFS_CPP = "indra/newview/llfloaterpreferencesgraphicsadvanced.cpp"
SETTINGS_XML = "indra/newview/app_settings/settings.xml"


def text_rules(ctx):
    bad = []
    xui = _read(ctx, PREFS_XUI) or ""
    m = re.search(r'name="FrameGen"[^>]*?tool_tip="([^"]*)"|tool_tip="([^"]*)"[^>]*?name="FrameGen"', xui, re.S)
    tip = (m.group(1) or m.group(2)) if m else ""
    settings = _read(ctx, SETTINGS_XML) or ""
    c = re.search(r"<key>RenderVKFrameGen</key>\s*<map>\s*<key>Comment</key>\s*<string>(.*?)</string>", settings, re.S)
    comment = c.group(1) if c else ""
    for what, text in (("the FrameGen tooltip", tip), ("RenderVKFrameGen's comment", comment)):
        if not text:
            bad.append("TEXT: %s not found" % what)
            continue
        for need in ("FSR frame generation", "DLSS or FSR upscaling"):
            if need not in text:
                bad.append('TEXT: %s must say "%s"' % (what, need))
        lone = re.sub(r"DLSS or FSR upscaling", "", text)
        if re.search(r"\bDLSS upscaling\b", lone):
            bad.append("TEXT: %s names DLSS upscaling as the only way (DLSS or FSR upscaling)" % what)
        if re.search(r"(?:Needs|needs)\s+(?:a\s+)?(?:NVIDIA\s+|GeForce\s+)?RTX 40 or 50", text) and \
                not re.search(r"DLSS frame generation needs", text):
            bad.append("TEXT: %s says an RTX 40 / 50 is needed for frame generation itself (DLSS frame generation "
                       "needs it; FSR runs on other GPUs)" % what)
    prefs = _code(_read(ctx, PREFS_CPP) or "")
    at = prefs.find('getString("framegen_status"')
    block = prefs[prefs.rfind("std::string status", 0, at):at] if at >= 0 else ""
    first = re.search(r"status\s*(?:=|\+=)", block)
    if not block or not first or not re.search(r"llfg::(?:statusPrefix|generatorName)\s*\(\s*fg\.generator\s*\(\s*\)",
                                               block[:block.find(";", first.start())] if first else ""):
        bad.append("TEXT: the Preferences status starts with the generator in use (status = llfg::statusPrefix("
                   "fg.generator(), ...) or llfg::generatorName(fg.generator()))")
    bad += _prefs_rules(ctx, xui, prefs)
    bad += _antilag_rules(ctx)
    return bad


def _antilag_rules(ctx):
    bad = []
    dev = _code(_read(ctx, "indra/newview/vkgfx/vk_device.cpp") or "")
    if not re.search(r"listed\(\s*VK_AMD_ANTI_LAG_EXTENSION_NAME\s*\)", dev) or "VkPhysicalDeviceAntiLagFeaturesAMD" not in dev \
            or not re.search(r"if\s*\(\s*q\w*\.antiLag\s*\)", dev) or not re.search(r"\w+\.antiLag\s*=\s*VK_TRUE", dev):
        bad.append("ANTILAG: vk_device.cpp enables VK_AMD_anti_lag only where listed and its feature (antiLag) is "
                   "reported, and turns the feature on")
    if not re.search(r"if\s*\(\s*mAntiLag\s*\)\s*f12\.pNext\s*=\s*fAntiLag\.pNext", dev) or not re.search(r"mAntiLag\s*=\s*false", dev):
        bad.append("ANTILAG: the retry without frame generation's extensions unchains the anti-lag feature and clears mAntiLag")
    if "vkAntiLagUpdateAMD" not in dev or "maxFPS = 0" not in re.sub(r"\s+", " ", dev):
        bad.append("ANTILAG: VkDeviceContext::antiLagUpdate calls vkAntiLagUpdateAMD with maxFPS 0")
    cx = _code(_read(ctx, "indra/newview/vkgfx/llvulkancontext.cpp") or "")
    inp = re.search(r"void\s+LLVulkanContext::antiLagInput\s*\(\s*\)\s*\{(.*?)\n\}", cx, re.S)
    if not inp or "VK_ANTI_LAG_STAGE_INPUT_AMD" not in inp.group(1) or "++mImpl->mAntiLagFrame" not in inp.group(1) \
            or '"RenderVKAntiLag"' not in inp.group(1):
        bad.append("ANTILAG: LLVulkanContext::antiLagInput marks the INPUT stage with a new frame index "
                   "(++mImpl->mAntiLagFrame), the mode from RenderVKAntiLag")
    ps = cx.find("marker(LLVKFrameGen::PresentStart)")
    pre = cx[max(0, ps - 1500):ps]
    if ps < 0 or "VK_ANTI_LAG_STAGE_PRESENT_AMD" not in pre or "mAntiLagFrame" not in pre:
        bad.append("ANTILAG: renderFrame marks the PRESENT stage (mAntiLagFrame) just before the frame's present / hand-over")
    app = _code(_read(ctx, "indra/newview/llappviewer.cpp") or "")
    ai = app.find("LLVulkanContext::instance().antiLagInput()")
    sw = app.rfind("LLVulkanContext::instance().waitFrameSlot();", 0, ai) if ai >= 0 else -1
    if ai < 0 or sw < 0 or app.count("antiLagInput()") != 1:
        bad.append("ANTILAG: the main loop calls LLVulkanContext::instance().antiLagInput() once, after the frame wait")
    settings = _read(ctx, SETTINGS_XML) or ""
    sm = re.search(r"<key>RenderVKAntiLag</key>\s*<map>(.*?)</map>", settings, re.S)
    if not sm or "<string>Boolean</string>" not in sm.group(1) or "<integer>1</integer>" not in sm.group(1):
        bad.append("ANTILAG: settings.xml needs RenderVKAntiLag (Boolean, default 1)")
    return bad


def _block_after(code, at):
    open_at = code.find("{", at)
    depth = 0
    for i in range(open_at, len(code)):
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        if depth == 0:
            return code[open_at:i + 1]
    return ""


def _prefs_rules(ctx, xui, prefs):
    bad = []
    combo = re.search(r'<combo_box\b[^>]*\bname="FrameGenGenerator"[^>]*>(.*?)</combo_box>', xui, re.S)
    head = re.search(r'<combo_box\b[^>]*\bname="FrameGenGenerator"[^>]*>', xui, re.S)
    if not combo or 'control_name="RenderVKFrameGenGenerator"' not in head.group(0):
        bad.append("PREFS: %s needs a combo FrameGenGenerator bound to RenderVKFrameGenGenerator" % PREFS_XUI)
    else:
        items = re.findall(r'<combo_box\.item\s+label="([^"]*)"\s+name="[^"]*"\s+value="(\d+)"', combo.group(1))
        if items != [("Automatic", "0"), ("DLSS", "1"), ("FSR", "2")]:
            bad.append("PREFS: FrameGenGenerator's items are Automatic (0), DLSS (1), FSR (2): %s" % items)
    if 'name="FrameGenGeneratorText"' not in xui:
        bad.append("PREFS: %s needs the label FrameGenGeneratorText" % PREFS_XUI)
    m = re.search(r'if\s*\(\s*LLComboBox\s*\*\s*gc\s*=\s*findChild<LLComboBox>\(\s*"FrameGenGenerator"\s*\)\s*\)', prefs)
    blk = _block_after(prefs, m.end()) if m else ""
    if not blk:
        bad.append("PREFS: %s has no FrameGenGenerator block (`if (LLComboBox* gc = findChild<LLComboBox>(\"FrameGenGenerator\"))`)" % PREFS_CPP)
    else:
        if [x.strip() for x in re.findall(r"gc->setEnabled\(([^;]*)\);", blk)] != ["fg.directReady()"]:
            bad.append("PREFS: the generator combo is enabled exactly when fg.directReady()")
        for g in ("Dlss", "Fsr"):
            if not re.search(r"generatorCanRun\s*\(\s*llfg::Generator::%s\s*\)" % g, blk):
                bad.append("PREFS: the generator combo lists %s only when generatorCanRun(llfg::Generator::%s)" % (g.upper(), g))
    fgh = _code(_read(ctx, "indra/newview/vkgfx/llvkframegen.h") or "")
    if not re.search(r"bool\s+generatorCanRun\s*\(\s*llfg::Generator\s+\w+\s*\)\s*const", fgh):
        bad.append("PREFS: LLVKFrameGen::generatorCanRun(llfg::Generator) const is missing")
    m = re.search(r'if\s*\(\s*LLComboBox\s*\*\s*fgc\s*=\s*findChild<LLComboBox>\(\s*"FrameGen"\s*\)\s*\)', prefs)
    fblk = _block_after(prefs, m.end()) if m else ""
    if "setEnabledByValue(" in fblk:
        bad.append("PREFS: the multiplier combo greys no item: it lists what multiplierOffered allows")
    if not re.search(r"for\s*\(\s*(?:S32|U32|uint32_t)\s+m\s*=\s*2\s*;\s*m\s*<=\s*6\s*;", fblk) or "multiplierOffered(" not in fblk:
        bad.append("PREFS: the multiplier combo's list is every multiplier from 2x to 6x that multiplierOffered allows")
    return bad


def run(ctx, result):
    bad = source_rules(ctx) + math_rules(ctx) + backend_rules(ctx) + text_rules(ctx)
    for b in bad:
        result.fail(b)
    ran = run_tests(ctx, result)
    if not bad and ran:
        result.summary = "tests pass; vendored sources, their build and the backend's rules hold"
    elif bad:
        result.summary = "%d problem(s)" % len(bad)
