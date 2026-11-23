# Porting Slipstream's Vulkan renderer into Firestorm

This branch (`firestorm-vulkan`) is the Firestorm viewer, rebranded as Prestige Viewer, with
Slipstream's Vulkan renderer being ported in. It is built by its own GitHub Action,
**Prestige Firestorm-Vulkan build** (`.github/workflows/prestige-firestorm-vulkan.yml`).
That action is separate from the **Prestige Windows build** on the `prestige` branch.

| | |
|---|---|
| Firestorm base | `FirestormViewer/phoenix-firestorm` @ `7e44160d` (23 Nov 2026) |
| Renderer source | this repository's `prestige` branch @ `cd94540c` (Slipstream, LGPL 2.1) |
| Switch | CMake `-DPRESTIGE_VULKAN=ON` (default `OFF` = stock OpenGL Firestorm) |

Every action run builds the viewer twice:
- **opengl** (`PRESTIGE_VULKAN` off) must always pass.
- **vulkan** (`PRESTIGE_VULKAN` on) compiles the Vulkan code ported so far.

## How big this is (measured, not estimated)

Slipstream did not add Vulkan next to OpenGL. It **deleted the OpenGL renderer** and
rebuilt drawing around a GPU-resident scene. Moving that into Firestorm means replacing the
same parts of Firestorm.

| | Slipstream / Prestige | Firestorm |
|---|---|---|
| Vulkan renderer (`newview/vkgfx`) | 217,016 lines, 218 files | none |
| World shaders (`indra/vkclean/shaders`) | 332 Vulkan GLSL files | 0 (Firestorm's GL shaders are in `app_settings/shaders`) |
| `pipeline.cpp` | 2,220 lines | 12,586 lines |
| `llviewerdisplay.cpp` | 485 lines | 2,102 lines |
| `llrender.cpp` (gGL) | 897 lines (forwards to a backend) | 2,084 lines (OpenGL) |
| `llgl.cpp` | 107 lines | 3,054 lines |
| Draw pools (`lldrawpool*`) | 2 files | 26 files |
| `llvertexbuffer`, `llglslshader`, `llimagegl`, `llrendertarget`, `llviewershadermgr`, `llspatialpartition` | **deleted** | present, used everywhere |
| Windows window (`llwindowwin32.cpp`) | replaced by SDL3 + `llnativewindow` | 6,022 lines, OpenGL context |

There is also code outside the renderer:
- 188 Firestorm source files call OpenGL-side APIs directly (`gGL.`, `LLGLSLShader`, `gPipeline.`, `LLRenderTarget`, `LLGLEnable`).
- In `newview` alone, 988 files the two viewers share have diverged, by about 240,000 changed lines.

## Strategy

1. **Firestorm stays shippable at every step.** Ported code goes behind `PRESTIGE_VULKAN`, and
   at runtime behind a `RenderUseVulkan` setting once there is something to switch to. The
   OpenGL build is the release build until the Vulkan path draws everything.
2. **Port Slipstream's code as it is.** Bring it in as-is, and keep Firestorm-specific glue
   in new `ll*vk*` / `prestige*` files with `<Prestige>` markers. That way both upstreams
   can still be merged.
3. **Use the seams Slipstream already built.**
   - `LLRenderBackend` (llrender): `gGL` immediate-mode calls forward to a backend. This
     moves Firestorm's UI drawing (about 190 files) over without editing each one.
   - `LLTextureData`: the GPU-agnostic texture handle that replaces `LLImageGL` at call sites.
   - `LLRenderRecorder`: records UI draws for the Vulkan backend.
4. **Test without a GPU first.** Mesa's lavapipe (software Vulkan) runs the pre-flight and,
   later, off-screen frames in CI and on dev machines before a real GPU is involved.

## Phases

| # | Phase | Result | Status |
|---|---|---|---|
| 0 | Firestorm import, Prestige rebrand, separate build action | rebranded Firestorm builds in CI | done |
| 1 | Vulkan toolchain + device pre-flight | `PRESTIGE_VULKAN` option, vendored Khronos headers + VMA, `ll::vulkan`; Slipstream's `vk_requirements` / `vk_compat` run at startup, GPU report in the log and in Help > About | done |
| 2 | Render backend seam | `LLRenderBackend` in Firestorm's `llrender`: every `gGL` immediate-mode, matrix, blend, colour-mask, line-width and texture-unit call forwards to an installed backend, OpenGL otherwise (`LLTextureData` moves with phase 4) | **done** |
| 3 | Vulkan device, window + swapchain | **3a done:** Slipstream's whole device context (`vk_device`, `vk_vma`, `vk_swapchain`, `vk_pipeline`, `vk_pipecache`, `vk_pipeworker`, `vk_descpool`) compiles into Firestorm and is brought up and shut down at startup as a self-test. **3b next:** a Win32 surface + swapchain on the viewer window and a cleared frame behind `RenderUseVulkan` | in progress |
| 4 | UI on Vulkan | `LLRenderVK` (`vkgfx/llrendervk`) as the backend; Firestorm's UI, fonts and floaters draw through it | |
| 5 | World on Vulkan | `llcleanrenderervk` + the 332 world shaders; feed it from Firestorm's `LLVOVolume` / `LLFace` / avatar data instead of draw pools and the spatial partition | |
| 6 | Firestorm's own render features | snapshots, 360 capture, previews, beacons, highlights, selection outlines, area search markers, mini-map, Firestorm graphics prefs, mapped onto the Vulkan renderer | |
| 7 | Vulkan by default | Retire the GL path (or keep it as a fallback), QA, release | |

Phases 3 to 6 are the bulk of the work. Each one lands as several small commits, and the
**opengl** build stays green throughout.

## Phase 3a: what was ported

From `prestige` @ `cd94540c`, `indra/newview/vkgfx/`, unchanged:
- `vk_device`, `vk_vma`, `vk_swapchain`, `vk_pipeline`, `vk_pipecache`, `vk_pipeworker`,
  `vk_descpool`
- `llcleanreplayvk` (the device's video-encode queue hooks)
- the headers they need

About 43,000 lines, including Vulkan Memory Allocator. Every symbol they need resolves
against Firestorm's own `llcommon` plus these additions:
- `vkgfx/compat/llmemorygovernor.{h,cpp}`, `vkgfx/compat/slenv.h`: Slipstream `llcommon`
  pieces that Firestorm lacks, copied unchanged.
- `vkgfx/compat/ll_thread_name.cpp`: `ll_set_current_thread_name()` from Slipstream's `llthread.cpp`.
- `vkgfx/compat/vk_msvc_compat.h`: Slipstream builds Windows with clang-cl and Firestorm
  uses MSVC. CMake force-includes this header (`/FI`) into the ported sources on MSVC to
  supply the `__builtin_*` functions they use.
- `llcommon/llfile.h`: `LLFile::replace()`, which forwards to Firestorm's `rename()`. That
  already overwrites.

`LLVkProbe` now also brings the device up and back down at startup on the GPU the
pre-flight chose. The `PrestigeVulkanDeviceTest` setting turns this off (default on, Vulkan
builds only). The result goes to the log and the About box ("…, device test passed").

Verified locally against Mesa lavapipe:

> `Vulkan device test: passed in 31 ms`

## Phase 2: what was ported

- `indra/llrender/llrenderbackend.h`: Slipstream's backend interface. It has one Firestorm
  addition, `bindLLImageGL()`, because Firestorm's textures are still `LLImageGL`.
- `indra/llrender/llrender.{h,cpp}`: adds `LLRender::sBackend` / `setBackend()`, which is
  `nullptr` by default. With a backend installed, these calls go to it instead of OpenGL:
  - `begin`, `end`, `flush`
  - `vertex*`, `texCoord*`, `color*` / `diffuseColor*`
  - `vertexBatchPreTransformed` (fonts)
  - the matrix operations. These also keep Firestorm's CPU matrix stacks, so code that reads `gGL`'s matrices still works.
  - `setColorMask`, `blendFunc`, `setSceneBlendType`, `setLineWidth`
  - texture units: bind, unbind, address mode, filtering

  Cube maps, GL render targets and raw GL texture names don't bind under a backend.
- No backend is installed yet, so the viewer behaves exactly as before. The Vulkan backend
  (`LLRenderVK`) is installed in phase 4.
- Still on OpenGL, for phase 4: `LLGLState` enables, depth and scissor scopes (`llgl.cpp`, `llglstates.h`).

## Phase 1: what was ported

From `prestige` @ `cd94540c`, `indra/newview/vkgfx/`:
- `vk_common.h`, `vk_enumerate.h`, `vk_compat.{h,cpp}`, `vk_requirements.{h,cpp}`: unchanged copies
- `third_party/vulkan` (Khronos headers, header version 363) and `third_party/vma`: unchanged copies
- (`vk_result_string.cpp` split `vkResultString()` out of `vk_device.cpp` until phase 3 brought the whole file in; it has since been removed)

New Prestige glue:
- `indra/cmake/PrestigeVulkan.cmake`: the option and the `ll::vulkan` target
- `indra/newview/llvkprobe.{h,cpp}`: runs the pre-flight once, logs it, and gives the About-box line
- `llappviewer.cpp`: runs the probe at startup and adds "Vulkan renderer (in development): …" to Help > About
- `strings.xml` (en): the `AboutVulkan` string

Verified locally against Mesa lavapipe:

> `Vulkan pre-flight: renderer would use llvmpipe (LLVM 20.1.2, 256 bits)`

## Credits and licences

- Firestorm © The Phoenix Firestorm Project, Inc., LGPL 2.1.
- Slipstream © its authors, LGPL 2.1, built on Alchemy and Linden Lab's viewer.
- Vulkan headers © The Khronos Group, Apache 2.0 / MIT.
- Vulkan Memory Allocator © AMD, MIT.

Licence files are kept next to the vendored code.
