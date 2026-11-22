"""World overlays own FG depth/motion instead of stepping in the static HUD layer."""
import os
import shutil
import subprocess
import tempfile
from .framegen_direct import _function
NAME = "framegen-overlay"
STAGE = "source"
DESCRIPTION = "FG world-overlay camera/jitter math and present-slot guide ownership"
GUARDS = "Parcel walls and edit axes move with a background surface or step with static UI; guide copies overwritten after raster; facade buffers used on present thread."
VK = "indra/newview/vkgfx/"

def run(ctx, result):
    def read(path):
        with open(ctx.path(path), encoding="utf-8") as stream:
            return stream.read()
    cxx = os.environ.get("CXX") or shutil.which("c++")
    if not cxx:
        result.fail("C++ compiler required for the overlay math fixture")
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "overlay")
        build = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                                "-I"+ctx.path(VK), ctx.path("scripts/check/test_framegen_overlay.cpp"), "-o", exe],
                               capture_output=True, text=True)
        if build.returncode:
            result.fail(build.stderr)
            return
        run = subprocess.run([exe], capture_output=True, text=True, timeout=30)
        if run.returncode:
            result.fail(run.stdout+run.stderr)
            return
    production = read(VK+"llrendervk/llrendervk.cpp")
    signature = "void LLRenderVK::recordFrameGenOverlay("
    body = _function(production, signature) or ""
    record = ""
    if body:
        start = production.index(signature)
        record = production[start:production.index("{",start)] + body
    if not record:
        result.fail("production overlay recorder missing")
        return
    with tempfile.TemporaryDirectory() as tmp:
        unit = os.path.join(tmp, "overlay_record.cpp")
        with open(unit,"w",encoding="utf-8") as stream:
            stream.write(read("scripts/check/test_framegen_overlay_record.cpp.in").replace("// @PRODUCTION_RECORD_METHOD@",record))
        exe = os.path.join(tmp,"record")
        build = subprocess.run([cxx,"-std=c++20","-O1","-Wall","-Wextra","-Werror",
                                "-Wno-missing-field-initializers","-I"+ctx.path(VK+"third_party/vulkan/include"),
                                unit,"-o",exe],capture_output=True,text=True)
        if build.returncode:
            result.fail(build.stderr)
            return
        execution = subprocess.run([exe],capture_output=True,text=True,timeout=30)
        if execution.returncode:
            result.fail(execution.stdout+execution.stderr)
            return
    source = read(VK+"llvulkancontext.cpp")
    copies = _function(source, "bool LLVulkanContext::Impl::recordFrameGenInputs(") or ""
    ready = copies.find("mFgOverlayGuidesReady && mFgOverlaySlot == slot")
    if ready < 0 or ready > copies.find("vkCmdCopyImage("):
        result.fail("FG overlay guide copies must not be overwritten at frame end")
    if "state->mFgOverlayGuidesReady = true" not in source or "mImpl->mFgOverlayGuidesReady = mImpl->mFgOverlayRefused = false" not in source:
        result.fail("guide readiness must be reset each main recording and committed only after guide raster")
    facade = read(VK+"llrendervk/llrendervk.cpp")
    record = _function(facade, "void LLRenderVK::recordFrameGenOverlay(") or ""
    if "mOverlayRecorded = true" not in record or "VK_IMAGE_LAYOUT_GENERAL" not in record:
        result.fail("overlay raster must consume the facade range and return guide copies in GENERAL")
    if "VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT" not in read(VK+"llvkframegenpresent.h"):
        result.fail("existing admitted FG guide images must permit raster writes")
    frag = read(VK+"llrendervk/shaders/ui_fg_guide.frag")
    if "vPrevious.xy/vPrevious.w-vCurrent.xy/vCurrent.w" not in frag or "texelFetch(uMotion,motionPixel,0).xy" not in frag:
        result.fail("floating overlays need their own prev-current motion; surface overlays inherit object motion")
    if "textureGrad(uTex,vUV,dFdx(vUV)*vTexelScale.x,dFdy(vUV)*vTexelScale.y)" not in frag:
        result.fail("guide alpha must use the same texture mip footprint as display color")
    present = read(VK+"llvkframegenpresent.cpp")
    if "recordFrameGenOverlay(" in present or "gVK." in present:
        result.fail("present thread must not borrow facade recording resources")
    result.summary = "108 projection/jitter cases; actual recorder 12 frames/4 slots; guide ownership and source WAR pass"
