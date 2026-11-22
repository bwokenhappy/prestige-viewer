# Test plan: FSR 3.1 / DLSS are the only temporal anti-aliasing (#1557)

Our own TAA resolve is gone. Temporal anti-aliasing is picked in the upscaler list (FSR Native AA,
DLSS DLAA, or a scaled preset); Antialiasing offers None / FXAA / SMAA. Run on Windows and Linux;
record the GPU, the driver and whether DLSS is available (the log's `DLSS:` line).

## Setup

- A busy region with glowing faces (full bright glow), water, alpha (trees, particles) and some text.
- `RenderVKGpuTimers` on for the timer checks; the VRAM stats floater for the memory checks.

## Moving TAA users

1. Quit with Antialiasing = TAA saved (`RenderFSAAType` 3 in settings.xml), upscaler off. Start:
   the log has the `DLSS:` status line, then `Migrated TAA to RenderVKUpscaler 7 (DLAA), RenderFSAAType 2`
   with DLSS available, or `... 1 (FSR Native AA) ...` without. Preferences shows that mode and SMAA.
2. The same with an upscaler already chosen (e.g. FSR Quality) and TAA: the upscaler is kept, only
   Antialiasing becomes SMAA.
3. A second start logs nothing (moved once).
4. With the upscaler off, set `RenderFSAAType` 3 in the debug settings (an old graphics preset does the
   same): FSR Native AA runs at once (sharp, temporally stable; the `p_fsr` timer runs). Never no AA.

## The image

5. FSR Native AA, Quality, Performance and DLSS DLAA / Quality: the scene looks as it did before this
   change at the same spot (compare screenshots), apart from glow.
6. Glow (bloom and classic glow) over glowing faces while moving the camera: no shimmer; the `p_glow`
   timer runs, there is no `p_taa` timer. A/B `RenderVKTAAGlow` 0 / 1.
7. Switch live off -> FSR Native AA -> FSR Performance -> DLSS Quality -> off: each switch shows the
   right image, glow present, no validation errors or warnings in the log.
8. Antialiasing None / FXAA / SMAA with the upscaler off: as before (no jitter, no shimmer from it).

## FSR not running

9. `RenderVKFSRDebugFailPrepare` 1 with an FSR mode: one frame without a temporal resolve, then after
   3 frames the log warns `FSR 3.1 failed 3 frames running ...` and the view renders at native
   resolution without jitter (FXAA / SMAA per Antialiasing). Set it back to 0: nothing changes until
   the upscaler mode or the window size changes, then the log says `trying FSR again` and FSR runs.
10. DLSS mode with the DLSS add-on removed: the matching FSR preset runs (as before).

## Elsewhere

11. Snapshots: a plain one, a supersampled one (the `p_snapacc` timer), and a live accumulated one,
    with FSR and with DLSS: the shot is right and the live view does not reset or shimmer after it.
12. Teleport and a camera cut: no ghosting.
13. Frame generation with FSR / DLSS upscaling: unchanged.
14. VRAM: under FSR / DLSS the TAA history (two display-size RGBA16F images + weights, about 150 MB at
    4K) is gone from the render-target total (#1431).
15. VR (if available): both eyes resolve through FSR; no glow temporal in stereo (as before).
