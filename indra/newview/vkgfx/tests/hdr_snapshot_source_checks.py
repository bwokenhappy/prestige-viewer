#!/usr/bin/env python3
"""Source-contract checks for local-only HDR capture routing."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]

snap = (ROOT / "indra/newview/vkgfx/llcleansnapshotvk.cpp").read_text()
snap_h = (ROOT / "indra/newview/vkgfx/llcleansnapshotvk.h").read_text()
ctx = (ROOT / "indra/newview/vkgfx/llvulkancontext.cpp").read_text()
ctx_h = (ROOT / "indra/newview/vkgfx/llvulkancontext.h").read_text()
viewer = (ROOT / "indra/newview/llviewerwindow.cpp").read_text()
viewer_h = (ROOT / "indra/newview/llviewerwindow.h").read_text()
preview = (ROOT / "indra/newview/llsnapshotlivepreview.cpp").read_text()
local = (ROOT / "indra/newview/llpanelsnapshotlocal.cpp").read_text()
renderer = (ROOT / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
matte = (ROOT / "indra/newview/vkgfx/llcleansnapmattevk.cpp").read_text()
model = (ROOT / "indra/newview/llsnapshotmodel.h").read_text()
shader = (ROOT / "indra/vkclean/shaders/clean_snap_live.comp").read_text()
regen = (ROOT / "indra/vkclean/regen_world_spv.py").read_text()
limits = (ROOT / "indra/newview/vkgfx/llsnapshotbufferlimits.h").read_text()
image_impl = (ROOT / "indra/llimage/llimage.cpp").read_text()

assert "r.pairedHdr && r.mode != MODE_RESAMPLE && r.mode != MODE_ACCUM" in snap
assert "(r.hdrRgbeOutput || r.pairedHdr) && s.swapFormat != VK_FORMAT_R16G16B16A16_SFLOAT" in snap
assert "packedRgbeBytes" in snap and "mReq.hdrRgbeOutput ? 4u : 3u" in snap
assert "r.hdrRgbeOutput) pc.aux[0] |= kFlagHdrRgbe" in snap
assert "mPipe[r.hdrRgbeOutput ? PIPE_HDR_RGBE : PIPE_V]" in snap
assert "kWorldSnapLiveHDRSpv" in snap_h or "kWorldSnapLiveHDRSpv" in snap
assert "mHdrOutputActive ? VK_FORMAT_R16G16B16A16_SFLOAT" in ctx
assert "bool hdrDisplayActive() const" in ctx_h
assert "req.hdrRgbeOutput = hdrRgbeOutput" in viewer
assert "req.point = show_ui ? 3u : (show_hud ? 2u : 1u)" in viewer
assert "outputCopies =" not in viewer
assert "LLRadianceSnapshot::serialize" in preview
assert 'req.pairedHdr = pairedHdr != nullptr' in viewer
assert 'mPreviewHdrRgbe.clear()' in preview and 'mPreviewHdrRgbe = std::move(pairedHdr)' in preview
assert 'SnapshotSaveHdrCopy' in preview and 'local_save_hdr_copy' in local
assert 'format->add(hdrLabel' not in local and 'rawSnapshotHDR' not in preview
assert 'LLVulkanContext::instance().hdrDisplayActive()' in local
assert 'mSnapshotBufferType == LLSnapshotModel::SNAPSHOT_TYPE_COLOR' in preview
assert 'mSetHdr' in snap and 'mWorkHdr' in snap and 'pc.aux[0] |= kFlagHdrRgbe' in snap
assert 'PIPE_HDR_RESOLVE' in snap and 'kWorldSnapLiveHDRResolveSpv' in regen
assert 'if (r.pairedHdr)' in snap and 'mOutHdr.mapped' in snap
assert 'saveHdrSidecarExclusive(sidecar.get(), filepath.substr' in viewer
assert 'APR_FOPEN_EXCL' in viewer and 'LLFile::remove(path)' in viewer
assert 'requiredBytes += sidecarBytes' in viewer
assert 'mTonemapAlphaHdrPipe = createGraphicsPipeline' in matte
assert 'if (mSnapHdrView && mSnapMatte && mSnapHoldRan && mTonemapAlphaHdrPipe)' in renderer
assert 'mScopeCaptureFormat = mScopeImg.format' in renderer and 'scopeBytesPerPixel' in renderer
assert 'SNAPSHOT_TYPE_AVATAR_TRANSPARENT' in local and 'SNAPSHOT_TYPE_AVATAR_TRANSPARENT' in preview
assert 'Never show or save a previous layer if the new capture fails' in preview
assert "SNAPSHOT_FORMAT_HDR" not in model
fmt = local[local.index("LLSnapshotModel::ESnapshotFormat LLPanelSnapshotLocal::getImageFormat"):local.index("void LLPanelSnapshotLocal::updateControls")]
assert 'if (id == "PNG")' in fmt and 'id == "HDR"' not in fmt
assert "SNAPSHOT_FORMAT_PNG" in fmt  # unrecognized/local-only choice resolves as SDR PNG
encoded = preview[preview.index("LLPointer<LLImageRaw> LLSnapshotLivePreview::getEncodedImage"):preview.index("bool LLSnapshotLivePreview::createUploadFile")]
assert "SNAPSHOT_FORMAT_HDR" not in encoded
formatted = preview[preview.index("LLPointer<LLImageFormatted> LLSnapshotLivePreview::getFormattedImage"):preview.index("void LLSnapshotLivePreview::setSnapshotFormat")]
assert "SNAPSHOT_FORMAT_HDR" not in formatted and "SNAPSHOT_FORMAT_PNG" in formatted
assert "snapshotPeakSystemBytes" in viewer
assert "outputCopies" in limits and "max - srcBytes" in limits
assert "LLRadianceSnapshot::peakHostBytes" in viewer
assert "maxSerializedBytes" in (ROOT / "indra/newview/vkgfx/llradiancesnapshot.h").read_text()
assert "S32 result = outfile.write(getData(), getDataSize())" in image_impl
assert '"kWorldSnapLiveHDRSpv"' in regen and "-DSNAP_HDR_RGBE=1" in regen
assert "if ((pc.aux.x & kHdrRgbe) != 0u) return max(nits, vec3(0.0))" in shader
assert "outp[f] = encodeRgbe(c)" in shader and "SNAP_HDR_RGBE" in shader
assert "dstH - 1u - f / dstW" in shader
assert "const size_t bytes = (size_t)w * h * (mReq.hdrRgbeOutput ? 4u : 3u)" in snap
# ---- #1451 G5: an SDR snapshot taken with HDR output on is the image SDR mode saves ----
def body(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        depth += (source[pos] == "{") - (source[pos] == "}")
        if depth == 0:
            return source[opening + 1:pos]
    raise AssertionError(signature)

renderer_h = (ROOT / "indra/newview/vkgfx/llcleanrenderervk.h").read_text()
display_out = (ROOT / "indra/vkclean/shaders/clean_display_output.frag").read_text()
display_common = (ROOT / "indra/vkclean/shaders/clean_display_common.glsl").read_text()
encode_h = (ROOT / "indra/newview/vkgfx/vk_display_encode.h").read_text()
encode_cpp = (ROOT / "indra/newview/vkgfx/vk_display_encode.cpp").read_text()
# Live route: the frames a colour snapshot samples render the world through the SDR post chain
# (as offscreen snapshots always have: SDR tone curve, glow, AA, final effects, dither), and the
# display encode shows those frames from the SDR world; HDR-file shots keep the HDR frame.
assert "return mDisplayLinear && !mSdrFrame && (!mOffscreen || mOffscreenHdrPair);" in renderer_h
assert "void setSdrFrame(bool on)" in renderer_h
assert "if (mDisplayLinear) mDisplayFrameComplete=true;" in renderer
assert "s.linearNits = displayLinearOutput();" in body(renderer, "void LLCleanRendererVK::recordLiveSnap(")
probe = body(renderer, "void LLCleanRendererVK::recordHdrEncodeInputProbe(")
assert "!displayLinearOutput()" in probe
assert "bool wantsSdr() const" in snap_h and "!mReq.hdrRgbeOutput && !mReq.pairedHdr" in snap_h
assert "bool linearNits = true;" in snap_h
assert "kFlagHalfEncoded" in snap and "half && !s.linearNits" in snap
assert "kHalfEncoded" in shader and "(pc.aux.x & (kLinearNits | kHalfEncoded)) != 0u" in shader
frame = body(ctx, "bool LLVulkanContext::renderFrame(")
decide = frame[frame.index("const bool sdrFrame ="):]
decide = decide[:decide.index(";") + 1]
assert "mImpl->mHdrOutputActive" in decide and "capturePending() != 0u" in decide and "liveSnapWantsSdr()" in decide
assert frame.index("setSdrFrame(sdrFrame)") < frame.index("mImpl->mWorld->recordWorld(cmd")
# Reset only after the last capture point (points 2 / 3 read the frame as SDR too).
assert frame.index("setSdrFrame(false)") > frame.index("recordCapturePoint(3, worldExt.width")
assert "liveSnapWantsSdr() const" in renderer_h
final = frame[frame.rindex("mDisplayEncode.record(", 0, frame.index("Content::CompleteDisplay")):]
# (#1517: frame generation's two optional outputs follow the sdrWorld argument)
assert re.search(r"true,\s*sdrFrame\s*[,)]", final[:final.index(";")])
capture = body(frame, "auto recordCapturePoint = [&](uint32_t point")
assert "Content::CompositeNits" in capture and "sdrFrame)" in capture
assert "bool sdrWorld = false" in encode_h and "0x100u" in encode_cpp
compact = display_out.replace(" ", "")
assert "(display.encoding&0x100u)!=0u" in compact and "if(sdrWorld)nits=displaySdrWorldNits(" in compact
assert "sdrWorld?encodeExt(nits/white):nits" in compact
assert "vec3 displaySdrWorldNits(" in display_common
# FX: the SDR snapshot frame's permutations compile at shot time, as offscreen shots' do.
fx = (ROOT / "indra/newview/vkgfx/llfxruntimevk.cpp").read_text()
assert "f.syncCompile=mSdrFrame && !mOffscreen;" in renderer
assert "const bool sync = f.offscreen || f.syncCompile;" in fx and "if (f.offscreen || f.syncCompile) pollJobs();" in fx
# Offscreen route (RenderSnapshotOffscreen, "No post-processing", avatar-scoped, the live path's
# fallback): already the SDR post chain; with HDR on its HUD attachments and selection outlines
# draw into a transient UI layer composited into the shot's source as SDR blends them.
off = body(ctx, "bool LLVulkanContext::renderOffscreen(")
assert "W->setUiLayer(offLayer.image ? offLayer.view : VK_NULL_HANDLE)" in off
assert "VkDisplayEncode::Content::CompositeNits" in off and "!colorHdrPair" in off
assert "W->setSwapImage(offCapture.image" in off
assert "VK_IMAGE_USAGE_SAMPLED_BIT" in off[:off.index("createImage2D(w, h, W->displayOutputState().colorFormat") + 400]
print("HDR snapshot source contract checks passed")
