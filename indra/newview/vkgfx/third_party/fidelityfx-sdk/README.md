# AMD FSR SDK 2.4.0 API headers, vendored (FSR 4 upscaling and frame generation, #1600)

Used by `indra/newview/vkgfx/llcleanffxvk.{h,cpp}`, which loads AMD's signed runtime
(`amd_fidelityfx_loader_vk.dll` and the effect DLLs beside it) at run time on Windows. The DLLs
are not in git: `indra/cmake/FidelityFXSdk.cmake` downloads them from the same commit,
verifies their SHA256 and stages them for the Windows package. Linux has no build of them and
keeps the source-built FSR 3.1 (`indra/vkclean/third_party/fidelityfx`).

Only these headers are compiled, and only in `llcleanffxvk.cpp`: `api/include/vk/ffx_api_vk.h`
declares a `struct VkDeviceContext` of its own, which clashes with the renderer's
`VkDeviceContext` class, so no other file may include them.

## Source

- Upstream: <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>
- Tag: `v2.4.0` (FSR SDK 2.4.0), commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`
- FSR Upscaling 4.1.2, FSR Frame Generation 4.0.2 (`FFX_UPSCALER_VERSION_*`,
  `FFX_FRAMEGENERATION_VERSION_*` in the headers)
- License: MIT, Copyright (C) Advanced Micro Devices, Inc. -- `LICENSE.txt` (the text the SDK's
  `docs/license.md` applies to these files and to the signed DLLs). Every vendored file keeps its
  license header. The notice is also in `indra/newview/licenses-win32.txt`.

## Layout

Same paths as upstream under `Kits/FidelityFX/` (the headers include each other relatively),
unmodified:

- `api/include/ffx_api.h`, `api/include/ffx_api_types.h`, `api/include/vk/ffx_api_vk.h`
- `upscalers/include/ffx_upscale.h`
- `framegeneration/include/ffx_framegeneration.h`, `ffx_framegeneration_api_types.h`,
  `vk/ffx_api_framegeneration_vk.h`
