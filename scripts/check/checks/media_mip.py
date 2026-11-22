"""Regression guard for the media mip lock (#1025): media on prims stay sharp, and their unused mips are not rebuilt.

Media (video, web pages) used to be sampled trilinearly over a mip chain rebuilt from every frame, so a TV a few
metres away read 1 - 3 mips down (blurry next to other viewers, which sample media without mips) and every frame
paid 10 blits + 20 barriers for 1080p. Now:

  * the shaders: a live media face (instance bit 20, the raw bit -- the lock must hold with TAA off) samples its
    base colour at mip 0 while its footprint is at most K mips (2^K texels per display pixel), ramping back to the
    true level by 2K (clean_frame_info.glsl mediaLodBias, K and the distance cap in frame info display_output.zw).
    Every base-colour site -- the lit FS, the depth pre-passes' alpha tests, the alpha cover / visibility passes,
    the avatar id pass -- goes through BASE_LOD_BIAS, so the EQUAL pass and the pre-passes sample the same level;
  * the renderer (indra/newview/vkgfx/llvkmediamip.h, tested by scripts/check/test_media_mip.cpp): an in-place
    media update rebuilds the chain only when the media's nearest face may sample below mip 0, or every
    kNearInterval-th update; a dirty rectangle into a current chain rebuilds only its texels; a paused media whose
    chain is behind catches up once. RenderVKMediaMipLock 0 restores the old behaviour (A/B).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "media-mip"
STAGE = "source"
DESCRIPTION = ("Media on prims sample mip 0 near the camera (one shared bias for every base-colour site) and rebuild "
               "their mip chain only when it can be sampled")
GUARDS = ("Blurry TVs / web pages on prims (#1025); an alpha-test pre-pass sampling a different level than the lit "
          "EQUAL pass (holes); a media mip chain rebuilt every frame; levels discarded (UNDEFINED) by an update "
          "that keeps them.")

HEADER = "indra/newview/vkgfx/llvkmediamip.h"
FIXTURE = "scripts/check/test_media_mip.cpp"
RENDERER = "indra/newview/vkgfx/llcleanrenderervk.cpp"
FRAME_INFO = "indra/vkclean/shaders/clean_frame_info.glsl"
SHADERS = "indra/vkclean/shaders"
SETTINGS = "indra/newview/app_settings/settings.xml"
BASE_SITES = ("clean_draw.frag", "clean_prepass_mask.frag", "clean_prepass_normal.frag", "clean_prepass_motion.frag",
              "clean_prepass_blend.frag", "clean_alpha_cover.frag", "clean_alpha_kvis.frag", "clean_avatar_id.frag")


def _function(code, signature):
    at = code.find(signature)
    if at < 0:
        return ""
    open_at = code.find("{", at)
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[open_at:i + 1]
    return ""


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    fi = read(FRAME_INFO)
    bias = _function(fi, "float mediaLodBias(uint flags, float qlod, float depth)")
    if not bias:
        missing.append("clean_frame_info.glsl: mediaLodBias(flags, qlod, depth) is missing")
    else:
        for need, why in (("ssr.display_output.z", "K from the frame info"),
                          ("ssr.display_output.w", "the distance cap from the frame info"),
                          ("taaLodBias(flags)", "the upscaler / TAA bias under the lock")):
            if need not in bias:
                missing.append("mediaLodBias does not read %s (%s)" % (need, why))
    macro = re.search(r"#define BASE_LOD_BIAS\(tex, uv, flags\)(.*?)\n(?!\s)", fi + "\n", re.S)
    if not macro:
        missing.append("clean_frame_info.glsl: #define BASE_LOD_BIAS(tex, uv, flags) is missing")
    else:
        body = macro.group(1)
        if "INST_MEDIA" not in body or "mediaLodBias(" not in body:
            missing.append("BASE_LOD_BIAS does not route live media faces (INST_MEDIA) through mediaLodBias")
        if "noTemporalFace" in body:
            missing.append("BASE_LOD_BIAS gates the media lock on noTemporalFace: it would vanish with TAA off")

    raw = re.compile(r"(base_color_handle\.x\)\],\s*v_uv,\s*taaLodBias\(|"
                     r"base_color_handle\.x\)\],\s*v_uv\)\.y\s*\+\s*taaLodBias\()")
    for name in BASE_SITES:
        code = read(os.path.join(SHADERS, name))
        if raw.search(code):
            missing.append("%s samples the base colour with taaLodBias directly: use BASE_LOD_BIAS (the pre-passes "
                           "and the lit EQUAL pass must take the same level)" % name)
        if "BASE_LOD_BIAS(" not in code:
            missing.append("%s does not use BASE_LOD_BIAS for its base-colour sample" % name)
    draw = read(os.path.join(SHADERS, "clean_draw.frag"))
    if not re.search(r"const float lb = BASE_LOD_BIAS\(tex, v_uv, v_flags\);", draw) \
            or "v_uv).y + lb;" not in draw:
        missing.append("clean_draw.frag: the base-colour sample and its a_lod (maskTestAlpha) do not share one "
                       "BASE_LOD_BIAS")

    code = read(RENDERER)
    if '#include "llvkmediamip.h"' not in code:
        missing.append("llcleanrenderervk.cpp does not include llvkmediamip.h")
    upload = _function(code, "void recordTexUpload(VkCommandBuffer cmd, VkImage image, VkBuffer stg,")
    if "TexChain chain" not in code or "llvkmediamip::nextLevel(" not in upload:
        missing.append("recordTexUpload has no TexChain / does not walk the dirty rectangle through "
                       "llvkmediamip::nextLevel")
    if not re.search(r"chain == TexChain::RectOnly \? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL", upload):
        missing.append("recordTexUpload: a RectOnly update must keep levels 1.. (old layout SHADER_READ_ONLY, not "
                       "UNDEFINED)")
    direct = _function(code, "LLCleanRendererVK::MediaDirectResult LLCleanRendererVK::recordDirectMediaStage(")
    if "llvkmediamip::rebuildChain(" not in direct or not re.search(r"part\.full \? nullptr : &rect, chain\)", direct):
        missing.append("recordDirectMediaStage does not decide the chain through llvkmediamip::rebuildChain and pass "
                       "it to recordTexUpload")
    updates = _function(code, "void LLCleanRendererVK::recordMediaUpdates(")
    for call, why in (("llvkmediamip::estimatedLevel(", "the nearest face's level"),
                      ("llvkmediamip::catchUp(", "the paused media's catch-up")):
        if call not in updates:
            missing.append("recordMediaUpdates does not compute %s (%s...))" % (why, call))
    if "si.display_output[2] = mediaMipLock();" not in code:
        missing.append("the frame info does not carry K (si.display_output[2] = mediaMipLock())")

    settings = read(SETTINGS)
    for key in ("RenderVKMediaMipLock", "RenderVKMediaSharpTexels", "RenderVKMediaSharpDistance"):
        m = re.search(r"<key>%s</key>\s*<map>(.*?)</map>" % key, settings, re.S)
        if not m:
            missing.append("settings.xml lacks %s" % key)
        elif not re.search(r"<key>Persist</key>\s*<integer>0</integer>", m.group(1)):
            missing.append("%s persists: it is an A/B / tuning knob with no UI (Persist 0)" % key)
    return missing


def run(ctx, result):
    if not os.path.isfile(ctx.path(HEADER)):
        result.fail(HEADER + " is missing: the media mip rules are gone")
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if cxx:
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "test_media_mip")
            c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(ctx.path(HEADER)),
                                ctx.path(FIXTURE), "-o", exe], capture_output=True, text=True)
            if c.returncode != 0:
                result.fail("test_media_mip.cpp did not compile:\n" + c.stderr[-2000:])
                return
            r = subprocess.run([exe], capture_output=True, text=True)
            if r.returncode != 0:
                result.fail("test_media_mip failed:\n" + r.stdout[-2000:])
                return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    if not cxx:
        result.warn("no C++ compiler found: the fixture not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    result.summary = "lock / chain / dirty-rectangle rules tested, every base-colour site shares BASE_LOD_BIAS"
