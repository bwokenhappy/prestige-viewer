# Test plan: FSR frame generation (#1544)

See `doc/frame-generation.md` ("The FSR generator") and `doc/frame-generation-design.md` sections 1,
3, 4, 10 and 12. Same setup as `1517-frame-generation.md`. Record the GPU, driver, refresh rate,
output (SDR / HDR10), present mode, window mode and the upscaler mode.

On an NVIDIA GPU that runs DLSS frame generation, FSR is forced with Preferences > Graphics > Advanced >
Generator: FSR (`RenderVKFrameGenGenerator` 2; live). The Generator list shows only the generators that can
run on the GPU; with FSR the multiplier list is Off and 2x. On any other GPU automatic (0) should pick FSR by itself.

## Availability

| GPU / setting | Expected |
|---|---|
| RTX 40 / 50 with the DLSS add-on, generator 0 | DLSS; startup log: `DLSS is the generator in use`, `FSR can run (not chosen)` |
| The same, generator 2 | FSR; status and log start with `FSR 2x` |
| RTX 20 / 30, GTX, or no DLSS add-on, generator 0 | FSR |
| Upscaling off | "needs DLSS or FSR upscaling" |
| A GPU without a second graphics queue (AMD, Intel) | "no second queue of the graphics family" until the shared-queue mode lands |
| A GPU without the subgroup operations FSR's shaders use | FSR cannot run; the log names what is missing |

## FSR, Windows

1. Generator 2 at 2x, HDR10, windowed. Log: `the generator is now FSR`, `Frame generation (FSR):
   pipelines ready (...)` (once per session), `Frame generation (FSR): images RxR -> DxD, N MB, output
   rgb10_a2, 3 slots`, `feature created`, then `Frame generation: FSR 2x, generated N of N asked`.
2. Pan the camera slowly and fast, strafe past near objects, watch an avatar walk: smooth, no doubled
   edges, no smearing beyond small fringes at screen edges and behind moving avatars.
3. `RenderVKFrameGenShowGenerated` 1: only generated pictures. Panning stays clean (it feels delayed
   in this view, which is expected). `RenderVKFrameGenFsrDebug` 1: a green bar on the left edge of
   every generated picture; 3: a red bar across the top on the first picture after a teleport.
4. The interface on generated pictures is sharp and identical (typing with the caret, a dragged
   floater, menus, HUDs); name tags and bubbles glide with the world.
5. SDR (Display HDR off) on a `B8G8R8A8` swapchain: log `output rgba8`; colours of the generated
   pictures match the real ones (no red / blue swap, check skin, sky, a blue object).
6. Upscaler mode change while it runs (e.g. Quality -> Performance): one frame `the upscaler's inputs
   changed size this frame`, then `images ...` at the new render size and generation continues.
   Both upscalers as the source: DLSS (7-11) and FSR (1-6).
7. Teleport, camera cut (mouselook in / out), a snapshot, generation Off / 2x: no ghost of the old
   scene on the first pictures after.
8. Multiplier 4x stored, generator 2: runs 2x; the status says "2x (FSR generates one frame per
   rendered frame)"; 3x / 4x greyed in Preferences.
9. Live switch while running: generator 2 -> 1 -> 0 -> 2. Each change: one swapchain rebuild, `the
   generator is now ...`, generation resumes; DLSS at 3x / 4x again after a switch back.
10. Resize, minimise / restore, borderless and exclusive fullscreen, alt-tab: generation resumes. The
    log never says `driver pacing on` with FSR (the viewer's pacer paces it).
11. Memory: the `present slots` line's `+ N MB admitted for the generator's own` and the FSR `images`
    line: about 90 MB at 2560x1440 Quality, 66 MB Performance, 161 MB native (the admission line counts the
    native figure, in MiB: about 153).
12. Quit while generating: the viewer exits normally.

## One graphics queue (forced on NVIDIA)

13a. `RenderVKFrameGenSharedQueue` 1, restart. Log: `present queue shared with the renderer`, `no Reflex: the
     present thread shares the renderer's queue`. DLSS 2x-4x and FSR 2x run; the real frame rate is within a
     few percent of the setting at 0; frames evenly spaced.
13b. Minimise / restore, resize, fullscreen on / off, teleport, a snapshot, a torn-off floater, instant replay,
     a live generator switch, quit while generating: no freeze, no hang at quit. Any `the shared graphics
     queue has been held by another thread for N s` line is a finding.

## AMD Anti-Lag (needs an AMD GPU)

13c. The device log line says `AMD anti-lag on`. Input latency feels lower than with `RenderVKAntiLag` 0; frame
     pacing with and without frame generation unchanged.

## DLSS regression

13. Generator 1 (or 0 on an RTX 40 / 50): steps 5-15 of `1517-frame-generation.md` once in HDR10 and
    once in SDR. `RenderVKFrameGenConventionDebug` 3 with `RenderVKFrameGenShowGenerated` 1 must show
    the old doubled-scene artifacts while panning, and 0 must clear them.

## Linux

14. X11 and Wayland, SDR, generator 2: runs at 2x; step 2 and 5 as on Windows.
15. HDR10 on Wayland: runs, `output rgb10_a2`.

## Other GPUs (needs testers)

16. AMD (Windows; Linux RADV), Intel Arc, an RTX 20 / 30 or GTX: the startup log's two generator lines;
    steps 1-7 where it runs; the frame rate with Off and 2x.

## Bad signs in the log

- `frame generation evaluate failed`, or `Frame generation stopped`.
- `the frame's images are not the ones the FSR feature was made for` or `... render size is not the one`.
- `the frame generation feature (WxH) could not be created: ...`.
- `images` lines repeating every frame (the feature made again and again).
