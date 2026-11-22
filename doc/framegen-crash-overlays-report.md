# Frame generation: crash and world overlays

Status: **READY**. Production commits `2e4146cab5` (enumerations), `b6a6a00f53` (overlays), and test-parser repair `8403560582`; validated source `84035605820ba87980508537488ffa1421177f00`. Branch `fix-framegen-crash-overlays`, owned build worktree `agents/worktrees/gpu-timer-scopes`, base `a0d6229cd8`. Actual Linux full build/link/package, 11 configured Windows viewer object builds, 10 UI glslc/SPIR-V checks and all 134 source checks PASS. No viewer, GPU or box run. Every build admitted at fewer than two Ninja processes, nice19/-j4, Linux then Windows serially.

## Crash evidence

`VkSwapchain::checkResult` called VK_CHECK for every nonzero result on the main window, including the non-error VK_INCOMPLETE. The formats, present modes and swapchain-images count/fill calls could all reach it. The exact call and reason for its changing count cannot be recovered: the original executable was deleted; the surviving binary was rebuilt and cannot symbolise that trace. No reconstruction of the old build is planned. Formats/present-mode lists may change with surface state; changing the requested image count on a NEW chain does not prove a count change within an existing chain. Swapchain-image enumeration is hardened too, without claiming it was the failing call. `llappviewer.cpp::vk_fatal_handler` routes to LL_ERRS, whose LLERROR_CRASH deliberately writes to an invalid sentinel address for the crash reporter; that explains the SIGSEGV. No independent memory-corruption claim.

The repair uses four bounded count/fill attempts with a fresh count and a named retry log to both stderr and the viewer log. Partial results are never published, shrink uses the successful returned count, size/allocation failures are bounded, and positive Vulkan statuses no longer enter main-window fatal reporting. Negative-result policy remains unchanged. Extension and physical-device enumerations use the same helper, including the optional Windows fullscreen-exclusive present-mode query. The standalone helper, expanded production WSI mock and allocation-failure tests pass.

## Overlay evidence and repair

`llappviewer.cpp` builds the world-overlay range under the unjittered world camera. The context routed this range to the static UI layer on FG chains. The generator's depth/motion inputs were copied from RR/FSR before those overlays, so floating parcel walls inherited no depth or parallax; they stepped with the real frame instead of following the interpolated world.

The new main-thread raster path draws that range into the generated world color, then writes only its covered pixels into the present slot's existing depth/motion copies. Current jitter is applied to both clips and cancels in the prev-current UV motion. Floating geometry uses its own clip depth/camera motion; surface-attached overlays inherit the existing object's motion when depth agrees. The RR/FSR originals stay read-only. Guide alpha uses display-equivalent texture gradients (not coarser render-resolution mips); the alpha cutoff is a push parameter, so varying cutoffs cannot grow the pipeline cache. Guide descriptors are rewritten after the main-slot fence; no facade buffer or descriptor is used by the present thread. End-of-frame copying skips an already rastered slot. Allocation/pipeline/format refusal leaves generation off for that frame and retains the ordinary real-frame overlay path. No extra full-frame image allocation.

| Overlay | Route |
|---|---|
| Parcel/property lines, parcel collision/ban walls | World color plus own guide depth/camera parallax |
| Selection/manipulator axes/grid, LL selection geometry | Same world-overlay range; surface motion where depth agrees |
| Tracker, object/pathfinding/area-search/sun-moon beacons; poser/IK hints | Same world-overlay range |
| LL physics/bounding boxes and VOCache debug geometry | Same world-overlay range |
| Renderer-generated screen-space selection outlines / debug false-color | Retain post-generation UI composition, deliberately sharp current-frame screen annotations |
| Highlight Transparent scene shading; ordinary scene particles | Existing scene/temporal pipeline (not the facade overlay range) |
| Name tags, hover text, chat bubbles | Existing per-slot moving-label atlas and anchored motion |
| HUD attachments, cursor/UI pick markers, menus | Existing post-generation UI/HUD composition; world-space pick geometry in the facade range gets the new guides |

FG-off recording takes the original path. New guide images are NOT introduced: existing admitted copies gain COLOR_ATTACHMENT usage and their actual allocation remains charged by the existing budget. A new CPU fixture exercises 108 projection/jitter/depth cases; a second fixture compiles the actual production recorder body against fake Vulkan for 12 frames / four main slots. Source guards prevent a late copy overwriting guides or facade calls leaking to the present thread.

## Validation and owner check

Source-only checks PASS so far: Windows-macro scan (3994 source files), XUI parse (5370 files), 14 compiler-free FG rule groups. All 134 source checks PASS on `8403560582`, including the expanded enumeration tests (allocation/refill/shrink/log sink), WSI mock (UBSan), present thread (UBSan+TSan), overlay projection and extracted production recorder. The old HDR hook parser was brace-matched rather than truncated at a nested initializer; its original UI-layer assertion remains, plus FG guide/HDR-route assertions. All 10 UI shaders compiled/validated serially; the full configured Linux build/link/package and all 11 changed configured Windows viewer objects pass. Fresh main merge check already PASS (main `a0d6229cd8`). No GPU or runtime performance claim.

Owner check after READY (four steps): (1) FG off: compare overlays and HDR/SDR appearance; (2) DLSS FG on, pan past parcel walls and manipulate an object; (3) repeat with FSR FG, checking labels/HUDs and dynamic selection; (4) switch FG off/on and multipliers repeatedly, checking named enumeration retries and absence of fatal VK_INCOMPLETE.

Validation progress: actual full Linux configure/build/link/package PASS on `8403560582`. All 11 changed viewer TU commands recorded with configured `-Werror`; project-existing warning suppressions remain. Packaged ABI/ISA checks PASS (host libdecor portability warnings retained). All 10 UI shader binaries match the serial glslc+spirv-val validation. Windows configured build of all 11 changed actual objects PASS, serially after Linux, nice19/-j4; no runtime/GPU claim. A command-receipt helper initially counted extra lab compile entries, then was corrected to select actual slipstream-bin entries; no production source change or Linux rebuild was needed.


Receipt directory: `/home/bryan/.cache/slipstream-build-tmp/gpu-timer-scopes/`. `framegen-source-all.log` (134 source checks), `framegen-focused.log` and `framegen-direct-fixed.log` (focused fixtures), `framegen-build-batch.log` (Linux full build), `framegen-build-remainder.log` (package checks + Windows objects + both exact generated-header comparisons), `framegen-{linux,windows}-actual-commands.json` (11 actual configured commands each), `framegen-ui-spirv/receipts.json` (shader hashes). The final runner printed ALL BUILD PHASES PASS. Earlier failed receipt-helper attempts are preserved; they are harness accounting failures, not hidden production compile failures.
