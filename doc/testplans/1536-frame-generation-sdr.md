# Test plan: DLSS Frame Generation with SDR output (#1536)

See `doc/frame-generation.md` (SDR in "One rendered frame") and `doc/frame-generation-design.md`
sections 2, 5 and 12. Same setup as `1517-frame-generation.md`, with **Display HDR off**. Record the
GPU, driver, refresh rate, present mode and window mode.

## Availability

| Output in use | Expected |
|---|---|
| HDR10 | runs (unchanged) |
| SDR, `B8G8R8A8_UNORM` or `R8G8B8A8_UNORM` | runs |
| scRGB HDR | "not with scRGB HDR output: switch Display HDR off, or use an HDR10 display mode" |
| SDR in an `*_SRGB` format only | "this display's SDR format is not supported by frame generation" |

## Windows, SDR

1. 2x, then 3x and 4x in a window: the status says it runs. Log: `Frame generation: chain of N
   images`, `N present slots, M MB of images`, `SDR composite ready (format 44)`, `feature created,
   WxH, format 44` (WxH the window size), `Frame generation: 2×, generated N of N asked`, the
   renderer's `Display output generation ... ui_layer=1`.
2. The interface is sharp on every frame: typing in chat with the caret, menus, a dragged floater,
   the mini map, HUD attachments, selection outlines, beacons.
3. A translucent floater over moving high-contrast scenery at 4x: no second copy of edges.
4. Name tags, chat bubbles and hover text glide with the world and stay hidden behind a wall;
   `RenderVKFrameGenMovingLabels` 0: they step with the interface.
5. `RenderVKFrameGenShowGenerated` 1: generated pictures carry the same interface.
6. Off / 2x / Off live: no black or frozen frame; one rebuild each way. With it Off, a capture of a
   fixed scene with interface is byte-identical to one from a build of `origin/main`.
7. Start the viewer with 2x already set: the login screen draws, the world appears with HUDs.
8. Resize, minimise / restore, move to another monitor while generating: the interface is intact
   and generation resumes.
9. Borderless and exclusive fullscreen: 5x / 6x offered while the driver paces; alt-tab, F11 twice,
   an overlay over the window.
10. Display HDR on / off while generating: one rebuild each way, generation resumes, interface
    intact. HDR10 frame generation: steps 5-15 of `1517-frame-generation.md` once (regression).
11. Sky gradients: no new banding or shimmer.

## Snapshots and other readers (SDR, frame generation on)

12. Snapshot to disk: world only, with HUDs, with interface. Each matches the same shot with frame
    generation off (interface and HUDs present, colours within 1/255; stacked translucent windows
    within 2/255).
13. A high-resolution (offscreen) snapshot with HUDs; a 360 snapshot; the logout screenshot.
14. Instant replay (if used): the clip has the interface, as without frame generation.

## Linux

15. X11, SDR, windowed: runs at 2x to 4x; the log names IMMEDIATE; the pacer's `worst late`
    comparable to Wayland.
16. Wayland, SDR: runs (MAILBOX where there is no tearing control); hide the window for a minute:
    no freeze.
17. X11 fullscreen: note whether the startup log says driver pacing is available and whether
    batches are accepted (`driver pacing on`). Either is fine; record which.

## Bad signs in the log

- A repeating `Display frame withheld: SDR frame generation's present slots, composite or renderer
  UI layer do not match the active WSI chain`.
- `Frame generation: the SDR picture could not be composed`.
- Repeated `Display output changed during recreation`.
