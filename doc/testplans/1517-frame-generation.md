# Test plan: DLSS Frame Generation (#1517)

See `doc/frame-generation.md` for what it does and how. Run on Windows (RTX 40 and/or 50) and on
Linux (Wayland, HDR on). Record the GPU, driver, display refresh rate and present mode (the log's
`Frame generation:` lines name it).

## Setup

- Build and install the DLSS add-on 1.3+ with NVIDIA's frame generation library
  (`contrib/ngxshim/README.md`), then start the viewer.
- Preferences > Graphics > Advanced: a DLSS upscaling mode; **Display HDR** on.
- A busy region with avatars (name tags), some hover text and a few open floaters.

## Availability and messages

1. Frame generation 2x with everything in place: the status line says it runs; the log shows
   `Frame generation: 2×, generated N of N asked, 2.00 frames shown per real frame`.
2. Each blocked state shows its reason and frame generation stays off, nothing else changes:
   - upscaling off (neither DLSS nor FSR): "needs DLSS or FSR upscaling";
   - scRGB HDR output: "not with scRGB HDR output: switch Display HDR off, or use an HDR10 display
     mode" (SDR output runs: `1536-frame-generation-sdr.md`);
   - add-on missing at startup: "...install it, then restart the viewer";
   - add-on older than 1.3: "...rebuild it and restart the viewer";
   - `RenderVKDlssShim` off: "...switch it on and restart the viewer";
   - VR on: "not in VR"; window in the background: "paused in the background";
   - the add-on without NVIDIA's frame generation library: "...reinstall the DLSS add-on with the
     frame generation library, then restart the viewer";
   - Windows GPU scheduling off: "...turn it on under Settings > System > Display > Graphics, then
     restart Windows";
   - an add-on 1.3.1 (older): runs, with the general texts.
3. RTX 40: 3x and 4x run as 2x and the status says so. RTX 50 in a window: 5x and 6x are not
   offered; a saved 5 or 6 runs as 4x and the status says "4x (5x and 6x need fullscreen)".
4. Switching Off / 2x / 3x / 4x / Off live: no hitch longer than a rebuild, no black frame, no log
   errors.

## Smoothness and pacing

5. At 2x, 3x, 4x in the same spot: shown frames per second ≈ multiplier × rendered; log `pacer:`
   worst late under ~1 ms most of the time; `slot wait` near 0 ms.
6. Camera pans and walking look smooth, no judder, no repeated frames.
7. Input latency feels like frame generation off at the same rendered rate (Reflex on where the
   log says so).

## Interface and labels

8. The interface (chat, menus, floaters, the mini map, typing in local chat) is sharp on every
   frame: no fading-in letters, no ghosting, no double text.
9. Name tags, chat bubbles and hover text glide with their avatars and objects while:
   orbiting an avatar; walking with the follow camera; zooming fast; a moving prim with hover text.
   No flicker, no darkening, no trail, no double.
10. A tag behind a wall stays hidden while the camera moves.
11. A translucent floater (unfocused local chat) over a tag: the tag keeps its brightness.
11b. A half-transparent floater over a high-contrast scene, orbiting at 4x: the scenery behind it
    moves smoothly, no faint second copy of edges.
12. A teleport and a region crossing: labels appear in place, no streak across the screen.
13. A crowd (50+ tags): all tags drawn; the log may say the label atlas grew (at most 2048x1024).
14. `RenderVKFrameGenMovingLabels` 0: labels step at the rendered rate again; 1: they glide.
15. Beacons and property lines: drawn, stepping at the rendered rate (expected).

## Rain and snow

15b. Heavy snow (and then heavy rain) in front of buildings or trees, turning the camera steadily
    and orbiting, at 2x and at 3x, with DLSS-G and with FSR (Preferences > Frame generation >
    Generator): the geometry behind the precipitation stays still and sharp -- no warping,
    swimming or dragging of edges with the flakes. The flakes themselves step at the rendered rate
    (expected).
15c. Same scene, frame generation on and off: the precipitation has the same density, brightness and
    colour (day, dusk, night; a non-default tonemapper in Photo Studio). In HDR10 too.
15d. A flake behind a pole or a wall edge is hidden by it; a flake in front of it shows.
15e. Splashes on the ground and run-off streams from roofs still show.
15f. Snapshot while it snows with frame generation on: the snow is in the picture.

## Window and display events

16. Minimise and restore, resize, maximise, move to another monitor: one rebuild each, frame
    generation resumes by itself.
17. Display HDR off and on while running: one rebuild each way, frame generation keeps running in
    SDR (#1536) and in HDR10; HUDs and labels drawn once in both.
18. Linux only: switch workspaces / hide the window for a minute and come back: the viewer is not
    frozen, frames resume; no "GPU hang".
19. Alt-tab away and back (Windows): "paused in the background", then resumes.

## Snapshots and other paths

20. Snapshots (HDR and SDR) with frame generation running: the image is correct, the interface
    and labels as without frame generation.
21. Frame generation Off: everything looks and performs as before this change (compare a few
    scenes with a build without it).
22. VR: frame generation stays off, VR rendering unchanged.

## Quit and failure

23. Quit while frame generation runs: "Goodbye!" in the log, no frame generation errors, no hang.
24. Quit while the window is minimised (and, on Linux, hidden on another workspace).
25. A real GPU hang elsewhere (if one happens) is still reported as a GPU hang; a held present
    never is.

## Driver pacing (fullscreen; RTX 50; add-on 1.4)

25b. The startup log says `driver pacing available (in fullscreen)`.
25c. In a window (maximised too): `driver pacing off` in the log, 5x / 6x not offered, a saved 5 or 6
     runs as 4x and the status says "4x (5x and 6x need fullscreen)".
25d. Fullscreen (F11): within a few seconds `driver pacing on (refused 0 in a row)`, the pacer idle
     (`0 batches paced`); 5x and 6x appear and run. At a base of 40+ fps motion is even.
25e. In fullscreen at 6x: switch 4x -> 6x, alt-tab out and back, toggle F11 twice: 6x stays or returns
     by itself, with no dip to 4x on a rebuild (the log's `answer now 0x0, after N batch(es) answered
     0x1` shows how long the driver refused; under half a second is forgiven).
25f. In fullscreen at 6x, open the viewer's menus and floaters: no change. A desktop overlay (volume,
     notification) over the window: at most one drop to 4x, 6x again about 2 s after it is gone.
25g. A `nvidia_app_id.txt` left from add-on 1.3.x next to the add-on changes nothing.

## Memory

26. The log's `present slots, N MB of images` line: three slots at 2560x1440 is about 240 MB; the
    label atlases add 2 MB per slot (more only in crowds) and are freed when labels are unused.
27. Low VRAM (a heavy region at 4K): frame generation either runs with two slots or says "not
    enough video memory for frame generation's images"; nothing crashes.
