# Frame Generation

Tickets: #1517 (DLSS), #1536 (SDR output), #1544 (FSR); part of #1114. Windows and Linux.

Frame generation shows extra pictures between the frames the viewer renders, so motion looks
smoother on a high refresh display. At 2x one generated frame follows each rendered one; at 3x and
4x (DLSS on RTX 50 only), two and three. 5x and 6x exist only while the driver paces the frames, which
it does in fullscreen (see "Pacing"). The generated frames are made from the last two rendered frames
and their depth and motion by one of two **generators**:

- **DLSS frame generation** (NVIDIA DLSS-G, through the DLSS add-on): GeForce RTX 40 and 50, up to
  4x (6x while the driver paces);
- **FSR frame generation** (AMD FidelityFX FSR 3.1 frame interpolation and optical flow, MIT): AMD's
  shaders built into the viewer and driven by the viewer's own code, no AMD runtime or DLL. Any GPU
  that can run its passes; 2x only (it generates one frame per rendered frame).

The viewer picks DLSS where it can run, else FSR. The viewer itself decides what the generator sees,
puts the real interface on every picture and presents and paces the frames, the same way for both.

It does not make the world react faster: input still reaches the screen at the rendered frame rate
(Reflex keeps that latency low where the driver offers it). It suits a frame rate of roughly 30-60
rendered frames per second on a 120 Hz or faster display.

## Using it

- **Preferences > Graphics > Advanced > Frame generation**: Off, 2x, 3x, 4x (5x and 6x appear only
  while the driver paces the frames, in fullscreen). It changes live; no restart. A status line under it says what
  it is doing, or why it cannot run.
- It needs:
  - for DLSS frame generation: the DLSS add-on (`contrib/ngxshim`, version 1.3 or later; 1.4 for 5x and 6x)
    with NVIDIA's frame generation library, installed as its README says. The add-on is loaded at startup:
    after installing or rebuilding it, restart the viewer (the status line says so). FSR frame generation
    needs nothing installed;
  - DLSS or FSR upscaling (any mode: either upscaler writes the depth and motion vectors the generator
    reads, and either generator takes either upscaler's);
  - on a GPU with a single graphics queue (AMD's and Intel's): an IMMEDIATE present mode, since the present
    thread then shares the renderer's queue (see "One graphics queue");
  - SDR output, or HDR10 output (**Display HDR** on). scRGB HDR output is not supported (switch
    Display HDR off, or use an HDR10 display mode). On Linux, SDR runs on X11 and Wayland, HDR on
    Wayland;
  - the main window, not VR;
  - on Windows, hardware-accelerated GPU scheduling (Settings > System > Display > Graphics).
- VSync is off while it runs: the viewer paces the frames itself, or in fullscreen the driver does.
- The status line starts with the generator and the multiplier in use ("FSR 2x", "DLSS 3x"). A stored 3x or
  higher runs as 2x with FSR, and the status says so.
- When it cannot run, the status line gives the reason and what to do, for example:
  - "needs DLSS or FSR upscaling" (upscaling is off: either upscaler writes the depth and motion vectors
    the generator reads), "paused in the background";
  - "not with scRGB HDR output: switch Display HDR off, or use an HDR10 display mode";
  - "this display's SDR format is not supported by frame generation" (an SDR surface whose only
    formats blend in linear light, `*_SRGB`);
  - "the DLSS add-on is too old for frame generation; rebuild it and restart the viewer";
  - "NVIDIA's frame generation library is missing: reinstall the DLSS add-on with the frame
    generation library, then restart the viewer";
  - "Windows hardware-accelerated GPU scheduling is off: turn it on under Settings > System >
    Display > Graphics, then restart Windows".
- If it fails while running (rare: a driver or add-on error), it stops and says "Frame generation
  stopped: ... Restart the viewer to try again."

Settings (all under `Render`):

| Setting | Default | What it does |
|---|---|---|
| `RenderVKFrameGen` | 0 | 0 off, 2-6 (the multiplier). RTX 40 cards run 2x for any value above 0; above 4 runs as 4x unless the driver paces the frames. |
| `RenderVKFrameGenMovingLabels` | 1 | Name tags, chat bubbles and hover text glide through the generated frames. 0: they move only on rendered frames. |
| `RenderVKFrameGenShowGenerated` | 0 | Debug: show only generated pictures, to judge their quality. Not saved. |
| `RenderVKFrameGenGenerator` | 0 | Preferences > Frame generation > Generator. 0 automatic (DLSS where it runs, else FSR), 1 DLSS, 2 FSR. Live: a change rebuilds the swapchain and switches. |
| `RenderVKFrameGenFsrDebug` | 0 | Hidden debug, FSR: +1 a green bar on every generated frame, +2 a red bar after a history reset (blue: a scene change). Not saved. |
| `RenderVKFrameGenSharedQueue` | 0 | Hidden test setting: the present thread shares the renderer's queue even where a second one exists, as on AMD / Intel. Read at startup. |
| `RenderVKAntiLag` | 1 | AMD Anti-Lag on AMD GPUs whose driver offers it; 0 turns it off. |
| `RenderVKFrameGenConventionDebug` | 0 | Hidden debug, DLSS: the old input conventions for an A/B (3: the doubled-scene bug). Not saved. |

The log has one `Frame generation:` line every 5 seconds while it runs ("Frame generation: FSR 2x, ..."): frames generated and
shown, pacing (how late the worst present was), Reflex, how long the renderer waited for the
present thread, and the present calls' cost.

## How it works

### One rendered frame

1. The world is rendered and upscaled by DLSS or FSR as usual.
2. The display encode (`clean_display_output.frag`) writes three pictures in one pass:
   - the finished frame, interface included (what you see for the rendered frame);
   - the **world alone** ("hudless", Rec.2020 PQ, A2B10G10R10) for the generator;
   - a copy of the **interface layer** (RGBA8, premultiplied, as the viewer draws it), which every
     generated frame composes over its world in linear light at paper white, exactly as the
     rendered frame does. A half-transparent window over moving scenery therefore looks the same
     in rendered and generated frames.
   In SDR there is no encode: the world is rendered straight into the slot's **world image** (the
   swapchain's own 8-bit format, exactly what SDR shows without frame generation; that image is
   what the generator gets), and the interface, HUDs and markers into the slot's **interface layer**
   (RGBA8). One composite (`clean_display_output.frag` encoding 5) lays the labels and the layer
   over the world into the frame, as the SDR swapchain blends them: in the 8-bit sRGB-encoded space,
   no transfer functions. Every generated frame uses the same composite with the same layer. SDR
   draws into a separate layer only while frame generation runs; with it off, SDR draws as always.
3. The upscaler's depth and motion vectors are copied for the generator, with the camera's
   matrices and jitter. A camera cut, a teleport or a renderer reset tells the generator to start
   fresh.
4. The frame goes to the **present thread** as a job in one of 2-3 slots.

The generator never sees interface pixels. It blends what it cannot match, so text it interpolates
flickers and darkens; keeping the interface out of its input is the reason this design exists.

### The present thread

For each job it, in order:

1. takes one swapchain image per generated frame, runs the generator (DLSS-G, or FSR's passes) into a
   scratch image, and draws the labels (see below) and the real interface layer over the generated world;
2. presents the first generated frame at once and holds each later one until the pacer opens its
   gate (evenly spaced between rendered frames);
3. copies the rendered frame into one more image and presents it last, also paced.

It owns the present queue and every acquire and present on the frame generation swapchain. The
swapchain gets one image per picture of a batch plus one (two on MAILBOX / FIFO). Present modes:
IMMEDIATE, else MAILBOX, else FIFO.

### The FSR generator

FSR frame generation runs AMD's own GLSL passes (vendored under `indra/vkclean/third_party/fidelityfx`,
SDK 1.1.4, one local patch for an explicit output format) from the viewer's host code, recorded on the
present thread into the same command buffer as the composite: per rendered frame, its prepare passes
(dilated depth and motion, the previous frame's depth), optical flow (luminance pyramid, scene change
detection, block search, filter, scale), then interpolation and inpainting at the half-way moment. The
host maths (constant buffers, dispatch sizes, the pass order, the reset rule, the motion vector scale
from the upscaler's UV units) is one header with host tests. Its descriptor sets are written when its
feature is made for the present slots' images, never on the present thread. A history reset (teleport,
camera cut, new feature, a frame not generated for) makes it start fresh. Its shaders have no BGRA8
storage format, so on a BGRA8 SDR swapchain it writes RGBA8 and the fallback without interface is a
blit; HDR10 is written as 10-bit PQ.

### One graphics queue

NVIDIA's graphics family has a queue to spare, and the present thread presents from it. Where it has one
queue only (AMD's and Intel's drivers, as far as known; `RenderVKFrameGenSharedQueue` forces it for
testing), the present thread shares the renderer's queue: every submit, present and queue wait on it takes
one lock (`VkDeviceContext::QueueLock`, nothing on a device with its own present queue). Frame generation
then runs only on an IMMEDIATE chain (a present held by the compositor would hold the lock and stall the
renderer); without one the status says "this GPU has one graphics queue, and this display offers no
immediate present mode". The present thread waits each pacer gate itself (at most 200 ms) instead of
submitting a wait, and Reflex is off (the renderer's queue cannot be marked out of band). The startup log
says `present queue shared with the renderer`.

### AMD Anti-Lag

On an AMD GPU whose driver offers `VK_AMD_anti_lag`, each frame marks its input stage (after the frame
wait, where it may sleep so input reaches the screen sooner) and its present stage, as Reflex does on
NVIDIA. It runs with or without frame generation, never in VR; `RenderVKAntiLag` 0 turns it off. The
device line of the log says `AMD anti-lag on / off`. Not tested on AMD hardware yet.

### Name tags, bubbles and hover text

Labels anchored in the world would otherwise jump at the rendered frame rate while the world glides.
On a frame that generates, each label (a name tag with its bubble, or a hover text) is drawn once,
sharp, into a small per-slot **label atlas** at its own size and occlusion, with its screen rect and
how far it moved on screen since the last rendered frame. Every picture draws the labels between the
world and the interface; generated frame *k* of *n* draws each one where it was at moment
*k / (n + 1)*. Size and text change on rendered frames only. Beacons and property lines stay in the
interface layer. The atlas starts at 1024x512 (2 MB) and grows only when labels do not fit (cap
2048x1024); in a very crowded spot the labels that do not fit step as before. When labels are not in
use for a while (the setting off, snapshots), the atlases are freed.

### Falling rain and snow

The weather's falling particles (rain streaks, snow flakes, blowing snow, run-off drips) write
neither depth nor motion vectors. Drawn into the world, they reached the generator's world input
while the motion under each flake said "background": the generator's flow followed the flakes and
dragged the scenery behind them (geometry behind snow warping and swimming, worst while the camera
turns). So on every frame that feeds the generator -- DLSS-G and FSR alike, SDR and HDR10 -- the
falling particles are not drawn into the world. They are drawn after the post chain into the
interface layer, depth tested against the world, first in the layer (the interface draws over them):
the generator interpolates a world without them, and every picture of the batch shows the rendered
frame's particles over its world. Their colour goes through the scene's own exposure, tonemapper and
grade, and each one is written so that the layer's composite gives the same linear-light blend over
the world as it has in the scene. The upscaler's jitter is taken out (no upscaler runs after them).
There is no setting; with frame generation off nothing changes.

What differs from the rendered world: the particles step at the rendered frame rate between generated
frames (the scenery behind them no longer warps); in HDR10 a flake is at most paper white (the
interface layer is 8-bit); lens distortion, vignette and grain from the final pass are not applied to
them; and splashes and run-off streams stay in the world (they sit still in it, so their motion
vectors are right). A snapshot frame (and a live-snapshot capture) keeps them in its world. The late
pipeline is compiled in the background as soon as frame generation runs while weather shows (and the
weather's own pipelines exist); until it lands (once per session) the falling particles are left out
rather than warp. If it cannot be built, they stay in the world.

### Pacing

The pacer spaces presents evenly between rendered frames using the high-resolution clock
(the performance counter on Windows, `CLOCK_MONOTONIC` on Linux).

With FSR the viewer's pacer always paces. With DLSS, in fullscreen the driver paces the frames instead
(present metering, `VK_NV_present_metering`):
whenever Windows shows the window directly -- always in exclusive fullscreen, and in borderless
fullscreen while nothing is over it. No NVIDIA application
ID is needed. A normal or maximised window is composed by the desktop, and the driver refuses it.
The viewer asks whenever the driver offers present metering, and the driver answers every paced
batch (0: accepted). The viewer:

- forgives refusals for half a second after a new swapchain (the driver refuses its first batches,
  longer when exclusive fullscreen is taken again);
- falls back to 4x at once on a refusal, and offers and runs 5x and 6x again only after 2 seconds
  of accepted batches, so an overlay over the window does not flip the multiplier;
- stops asking after 30 refusals in a row (each refused batch goes out unpaced) until the next
  swapchain; in fullscreen it asks again every 3 seconds with a single batch.

5x and 6x appear only while the driver paces (RTX 50, as far as the GPU generates). In a window the
status says "4x (5x and 6x need fullscreen)"; with a driver without present metering, "4x (5x and
6x need a newer NVIDIA driver)". The viewer's own menus and floaters are part of its picture and do
not stop driver pacing; only desktop overlays over the window do.

### Never freezing the viewer

The present thread may sit inside a driver call for a long time (a compositor holding a present,
a minimised window, a hidden Wayland surface). The rule:

- The present thread never declares the device lost and never judges; its own waits are bounded.
- The main thread never waits for it without a bound: a frame whose slot is still busy is dropped
  (100 ms on MAILBOX / FIFO, 250 ms on IMMEDIATE); a rebuild waits at most 2 s, then tries later.
- Whether a long wait is a GPU hang is decided only by the device's GPU-progress watchdog, as for
  every other wait. A held present is never reported as a hang.
- At quit the thread is asked to stop. A thread still inside a driver call is left behind and the
  process ends without static destructors.

Minimise, resize and HDR on/off rebuild the swapchain once and frame generation resumes.

### Memory

All frame generation images (per slot: backbuffer, world, interface layer, depth and motion
copies; one scratch image) are admitted as one set against free video memory: three slots if they
fit, else two, else frame generation says it has no memory. About 240 MB at 2560x1440 with three
slots in SDR or HDR10 (estimated; every image is 4 bytes a pixel in both; the log's `present slots`
line has the real figure). FSR's own images join that same admission, counted at the output size (the
largest render size it may get): at 2560x1440 about 90 MB with Quality upscaling, 66 MB with
Performance, 161 MB native; at 3840x2160 about 205 MB with Quality, 361 MB native (the log's
`Frame generation (FSR): images` line). DLSS-G's own memory is the add-on's. The label atlases (2-8 MB per
slot) are admitted separately and freed when unused or with the swapchain.

## Linux

The same code path. DLSS frame generation needs the add-on built for Linux with NVIDIA's Linux frame
generation library, and a driver that offers the required Vulkan extensions; FSR frame generation needs
neither. SDR runs on X11 and
Wayland, HDR10 on Wayland. On X11 the chain is IMMEDIATE and the viewer's pacer spaces the frames;
whether the driver paces them in X11 fullscreen (5x / 6x) depends on the driver accepting present
metering there. Under a compositing X11 window manager that does not unredirect fullscreen windows,
presents are composed at the compositor's rate. A hidden or occluded window drops frames instead of
blocking.

## Code

| Area | Files |
|---|---|
| Rules (availability, reasons, multipliers, wait bounds, present modes) | `vkgfx/llvkframegenpolicy.h` |
| Generator choice, feature, evaluate, Reflex, status / log | `vkgfx/llvkframegen.*` |
| The generator interface and its inputs (`llfg::GenInputs`, `GenSize`) | `vkgfx/llvkframegengenerator.h` |
| DLSS generator (the add-on) | `vkgfx/llvkframegendlss.*`, `vkgfx/llvkneuralrecon.*`, `vkgfx/llvkaddonlock.h` |
| FSR generator: passes, resources; host maths | `vkgfx/llvkframegenfsr.*`; `vkgfx/llvkframegenfsrmath.h` |
| FSR shaders (vendored) and their build | `indra/vkclean/third_party/fidelityfx/{gpu,shaders}/{frameinterpolation,opticalflow}`, `indra/vkclean/regen_world_spv.py` |
| Present thread, slots, images, composites | `vkgfx/llvkframegenpresent.*`, `vkgfx/vk_present_handoff.*` |
| Pacer | `vkgfx/vk_frame_pacer.*` |
| Swapchain (batch acquire / present) | `vkgfx/vk_swapchain.*` |
| Encode, interface layer, labels in shaders | `vkgfx/vk_display_encode.*`, `vkclean/shaders/clean_display_output.frag`, `clean_display_labels.glsl` |
| SDR composite (encoding 5) and its C++ mirror | `clean_display_common.glsl` (`displayComposeUiSdr`), `vkgfx/vk_display_hdr.h` (`composeLayerSdr`) |
| Moving labels | `vkgfx/llvkframegenlabels.h`, `vkgfx/llrendervk/llrendervk.cpp` (`recordLabelAtlas`), `llhudnametag.cpp`, `llhudtext.cpp` |
| Falling rain / snow kept out of the generator's world | `vkgfx/llcleanrenderervk.cpp` (`wxLateWanted`, the late scope after the post chain), `vkgfx/llcleanweathervk.cpp` (`lateReady`, `recordDrawLate`), `vkclean/shaders/clean_weather.{vert,frag}` (`-DWEATHER_LATE`) |
| Frame loop integration | `vkgfx/llvulkancontext.cpp` (search `#1517`, `#1544`) |
| Add-on (separate MIT project) | `contrib/ngxshim` (`slip_nr_fg_*` in `include/slipstream_ngx.h`) |

Checks (`python3 -I scripts/check/run.py --stage source`): `framegen-abi`, `framegen-addon`,
`framegen-policy`, `framegen-present`, `framegen-handoff`, `framegen-pacer`, `framegen-stall`,
`framegen-linux`, `framegen-labels`, `framegen-direct`, `framegen-sdr`, `framegen-generator`,
`framegen-fsr`, `framegen-local-id` (no NVIDIA application ID in the tree or history), `framegen-weather`
(falling rain / snow out of the generator's world on every generator path, no switch, base weather
shaders unchanged).

## Not yet

- FSR frame generation, the shared-queue mode and AMD Anti-Lag have been tested on NVIDIA only (FSR
  forced with the Generator list, the shared queue with `RenderVKFrameGenSharedQueue`); AMD Anti-Lag not at
  all.
- AMD's own debug view pass (motion fields) is not built; `RenderVKFrameGenFsrDebug` draws the SDK's
  frame and reset markers only.

Test plans: `doc/testplans/1517-frame-generation.md`, `doc/testplans/1536-frame-generation-sdr.md`,
`doc/testplans/1544-frame-generation-fsr.md`.
