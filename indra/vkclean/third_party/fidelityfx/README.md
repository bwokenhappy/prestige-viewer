# AMD FidelityFX Super Resolution 3.1 (upscaler and frame generation), vendored

Used by the Vulkan clean renderer:
- the FSR 3.1 **upscaler** when `RenderVKUpscaler` is 1-6 (`indra/newview/vkgfx/llcleanfsr3vk.{h,cpp}`);
- FSR 3.1 **frame generation** (frame interpolation + optical flow, #1544) when frame generation runs
  with the FSR generator (`indra/newview/vkgfx/llvkframegenfsr.{h,cpp}`).

Only the GPU side is here. The SDK host libraries, the FFX Vulkan backend, the frame
generation swapchain and its pacing threads, and the FidelityFX shader compiler are not used:
our own host code creates the resources, orders the passes and fills the constants.

## Source

- Upstream: <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>
- Tag: `v1.1.4` (FidelityFX SDK 1.1.4), commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`
- FSR3 upscaler version: 3.1.4 (`FFX_FSR3UPSCALER_VERSION_*` in the SDK's
  `sdk/include/FidelityFX/host/ffx_fsr3upscaler.h`)
- License: MIT, Copyright (C) 2024 Advanced Micro Devices, Inc. -- `LICENSE.txt` (the
  SDK's own file, unmodified). Every vendored file keeps its license header. The notice
  is also in `indra/newview/licenses-*.txt`, which ship with the viewer.

## Layout

| Here | Upstream |
|---|---|
| `gpu/ffx_common_types.h`, `gpu/ffx_core*.h` | `sdk/include/FidelityFX/gpu/` |
| `gpu/fsr1/ffx_fsr1.h` (RCAS) | `sdk/include/FidelityFX/gpu/fsr1/` |
| `gpu/spd/ffx_spd.h` (single-pass downsampler) | `sdk/include/FidelityFX/gpu/spd/` |
| `gpu/fsr3upscaler/*.h` (no HLSL callbacks) | `sdk/include/FidelityFX/gpu/fsr3upscaler/` |
| `shaders/fsr3upscaler/*_pass.glsl` (the 9 Vulkan entry points used; not autogen-reactive) | `sdk/src/backends/vk/shaders/fsr3upscaler/` |
| `gpu/frameinterpolation/*.h` (no HLSL callbacks) | `sdk/include/FidelityFX/gpu/frameinterpolation/` |
| `gpu/opticalflow/*.h` (no HLSL callbacks) | `sdk/include/FidelityFX/gpu/opticalflow/` |
| `shaders/frameinterpolation/*.glsl` (all 11 Vulkan entry points) | `sdk/src/backends/vk/shaders/frameinterpolation/` |
| `shaders/opticalflow/*.glsl` (all 7 Vulkan entry points) | `sdk/src/backends/vk/shaders/opticalflow/` |

These are exactly the files the passes include (`glslc -M`), plus the SDK's VK entry
points; nothing is fetched at build time.

## How it is built

`indra/vkclean/regen_world_spv.py` compiles the pass entry points with `glslc
--target-env=vulkan1.3 -O` into `vk_fsr3_shaders_spv.h` in the build tree at build time (embedded
SPIR-V, like every other renderer shader), one permutation fixed to the renderer's
inputs (see `FFX_DEFS` there): HDR colour, render-size (low-resolution) unjittered
motion vectors, inverted (reversed-Z) depth, reference Lanczos reprojection (no LUT --
the SDK only uses the LUT on wave32/64 AMD parts), fp32. Two permutations of
`accumulate` (with / without sharpening) and of the two SPD passes (subgroup-quad and
`FFX_SPD_NO_WAVE_OPERATIONS`). The same header also carries our own
`clean_fsr_inputs.comp` and `clean_fsr_probe.comp` (the `RenderVKFSRDebugProbe` numeric
readback, `LLCleanFSRProbeVK`).

Frame generation (`FFX_FI_DEFS` / `FFX_OF_DEFS` there): the SDK's base arguments, render-size
(low-resolution) unjittered motion vectors, inverted depth, fp32. Its 24 variants: each optical
flow and frame interpolation pass once; the three passes that write the output image
(interpolation, inpainting, debug view) twice, with the output format `rgb10_a2` (HDR10's
generated frame) and `rgba8` (SDR's; GLSL has no BGRA storage format); and the three SPD-based
pyramid passes also with `FFX_SPD_NO_WAVE_OPERATIONS`. The SDK's CMake spells the jitter option
`..._JITTER_MOTION_VECTORS` and its GLSL `..._JITTERED_MOTION_VECTORS`: both are defined 0.
Optical flow's `FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT` is read by no shader (HDR is the run-time
transfer function) and is not set. The two debug view variants are built but not dispatched: the viewer
hands the inpainting pass the SDK's tear-line and reset-indicator flags only (`RenderVKFrameGenFsrDebug`).

The host side of `ffx_fsr3upscaler.cpp`
(resources, clears, constants, pass order, dispatch sizes) is re-implemented in
`llcleanfsr3vk.cpp`, which says where it follows the SDK.

## Local patches

In `gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h`, marked `SLIPSTREAM PATCH`:

1. `rw_luma_history` declared `rgba16f` instead of `rgba8`: the SDK allocates
   `FSR3UPSCALER_LumaHistory1/2` as `R16G16B16A16_FLOAT`, and a storage image's format
   qualifier must match its view.
2. `rw_upscaled_output` takes an optional explicit format
   (`FFX_FSR3UPSCALER_UPSCALED_OUTPUT_FORMAT`, set to `rgba16f` by the regen script) so
   the accumulate / RCAS / debug passes do not need `shaderStorageImageWriteWithoutFormat`.
   Unset, the declaration is the SDK's format-less one.
4. Finite, bounded colour in and out (`SlipstreamSanitizeRgb`, the rule of
   `clean_taa.comp`'s `sanitize()`: NaN / Inf -> 0, clamp to [0, 65000]) in
   `LoadInputColor`, `SampleInputColor` and `StoreUpscaledOutput`. The SDK only clamps the
   input at 0, so an fp16 overflow (Inf) in the scene became `Tonemap(Inf) = NaN` in the
   upsample and persisted in the history; and a NaN / Inf output reached the bloom and the
   dynamic exposure.

And one in `gpu/fsr3upscaler/ffx_fsr3upscaler_rcas.h` (also marked `SLIPSTREAM PATCH`):

3. RCAS runs on `Tonemap(colour * Exposure())` (the header's own reversible
   `max(rgb) + 1` compression, as our `clean_rcas.comp` does) and its result is clamped to
   the per-channel [min, max] of the 5-tap cross it read (same space) before
   `InverseTonemap(...) / Exposure()`, instead of RCAS on linear HDR x exposure.
   - On linear HDR, RCAS's limiter (which bounds its output to [0, peak 1], i.e. SDR)
     degenerates for sunlit values > 1: full-strength sharpening, unbounded overshoot,
     negative undershoot that is clamped to 0 downstream.
   - The first version of this patch (tonemapped space, inverse with a `1 - 1/1024` clamp,
     no cross clamp) still brightened the image: the limiter bounds the *ring's* clipping,
     not the centre's, so a bright texel over a dimmer ring is pushed past 1.0 (sharpness
     0.4: lobe = -0.1875 x 2^-1.2 x 0.5 denoise = -0.041; centre 0.95 over a 0.5 ring ->
     (0.95 - 0.041 x 2) / (1 - 0.163) = 1.04). The clamp + inverse turned that into
     1023 / exposure (exposed HDR 19 -> 1023) -- every centre above
     `1 - 0.163 x (1 - ring)` (ring 0.5: exposed HDR > 11). Those fireflies fed the
     bloom (halos, haze) and the dynamic exposure's 256-tap mean (the rest of the frame
     darkened), and the convex inverse also raised the mean of sharpened bright texture.
     With the cross clamp the output never leaves the input's own local range.
   Only the non-paired (`FSR_RCAS_F`) path is patched; the paired FP16 path is not
   compiled here.

In `gpu/frameinterpolation/ffx_frameinterpolation_callbacks_glsl.h`, marked `SLIPSTREAM PATCH`:

5. `rw_output` takes an optional explicit format (`FFX_FRAMEINTERPOLATION_OUTPUT_FORMAT`, set to
   `rgb10_a2` / `rgba8` by the regen script) so frame generation does not need
   `shaderStorageImageReadWithoutFormat` / `WriteWithoutFormat`. Unset, the declaration is the
   SDK's format-less one.

## Updating

Copy the same file list from a newer SDK tag (FSR 3.1.x; the SDK 2.x line ships FSR 4 /
DX12 only), re-apply the five patches, diff `ffx_fsr3upscaler.cpp` against
`llcleanfsr3vk.cpp`, and `ffx_frameinterpolation.cpp` / `ffx_opticalflow.cpp` against
`llvkframegenfsr.cpp` and `llvkframegenfsrmath.h` (constants structs, resource formats, clears, pass
order, dispatch sizes; `scripts/check/test_framegen_fsr.cpp` pins the maths), update the tag / commit above, and rebuild (the build regenerates the header).
