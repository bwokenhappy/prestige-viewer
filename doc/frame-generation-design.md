# Frame Generation: design decisions

Tickets: #1517 (DLSS), #1536 (SDR), #1544 (FSR). This document says why frame generation is built the
way it is. What it does and where the code is: `doc/frame-generation.md`. How it is tested:
`doc/testplans/1517-frame-generation.md`, `1536-frame-generation-sdr.md`, `1544-frame-generation-fsr.md`.

Frame generation touches the viewer low down: the swapchain, presenting, the display encode, the
interface and the frame loop. Each section names a decision, the reason for it, and what it costs.

## 1. The viewer calls the generator itself

**Decision.** The viewer calls DLSS Frame Generation (DLSS-G) directly through NVIDIA's NGX
library, gets each generated picture back in an image it owns, and presents it itself.

**Why.** The first version used NVIDIA's Streamline, which builds and presents the generated frames
on its own swapchain and present thread. It interpolates whatever it is given, the interface
included. In world, typed text showed half faded on generated frames and the caret doubled; it did
so with the interface tagged as such, with recomposition on and off. That cannot be fixed from
outside Streamline. Pausing generation while typing was rejected: a workaround, not a fix, and it
left every other interface change interpolated. Streamline was removed from the branch before the
pull request.

**Cost.** The viewer owns what Streamline did: the swapchain batch, pacing, Reflex and their
failure modes (sections 6 to 9).

**Two generators (#1544).** Everything above the generator is shared: a generator is one interface
(`LLVKFrameGenGenerator`: init, fits, prepare, evaluate, release) that takes the frame's inputs in the
renderer's own terms (`llfg::GenInputs`). DLSS-G sits behind it unchanged. FSR frame generation is the
second: AMD's FSR 3.1 frame interpolation and optical flow shaders, run by the viewer's own host code.
AMD's ready-made path (its frame interpolation swapchain) builds and presents the frames itself, the
same problem as Streamline; its shaders alone are what the viewer needs. The viewer picks DLSS where
it runs, else FSR (Preferences > Generator, `RenderVKFrameGenGenerator`, forces one; a change switches live at the next
swapchain rebuild, with the present thread drained and the device idle).

## 2. The generator never sees the interface

**Decision.** The generator's colour input is the world alone ("hudless", display size): in HDR10
A2B10G10R10, Rec.2020 PQ; in SDR (#1536) the swapchain's own 8-bit format, sRGB-encoded -- the
world exactly as SDR shows it, rendered straight into that image. It gets no interface,
hudless-pair or UI-alpha input. Every presented
picture, generated or real, gets the real interface drawn over it by the viewer.

**Why.** The generator blends what it cannot match between two frames. Text, outlines and anything
that changes without moving with the world come out faded, doubled or darker. The interface is
exact on every picture only if the generator never handles it.

**Cost.** The interface moves at the rendered rate: at 30 rendered frames per second and 120 shown,
a dragged floater moves at 30. Exactness was chosen over smoothness for the interface.

## 3. Where the vendor code lives

**Decision.** All NVIDIA calls are in the DLSS add-on (`contrib/ngxshim`, MIT, built and
installed by the user, loaded at startup), as a small `slip_fg` block next to the existing DLSS
calls: capabilities, device extensions, create, evaluate, release. The viewer includes only the
add-on's own C header, with size-locked structs (`SLIP_NR_FG_*_SIZE_V1`).

**Why.** No NVIDIA code or binaries in the repository, as for DLSS. One narrow call keeps the
generator replaceable.

**FSR (#1544).** AMD's GLSL sources for the two effects are vendored as source next to the FSR
upscaler's (`indra/vkclean/third_party/fidelityfx`, MIT, SDK 1.1.4; one marked local patch gives the
output image an explicit format) and compiled into the viewer with the other shaders. The host side
(resources, descriptor sets, constant buffers, dispatches, the pass order) is the viewer's own; its
maths is one header with host tests. No AMD runtime, DLL or SDK backend, so it runs where the viewer
runs, Windows and Linux.

**Cost.** Users install or update the add-on; the viewer reports an add-on that is missing, too
old or missing NVIDIA's frame generation library, and says what to do.

## 4. What the generator is given

**Decision.** The hudless world, the upscaler's own depth and motion vectors (copied per slot), the
camera's matrices and jitter, and a reset flag. Reset is set when the renderer resets its own
history (a teleport, a camera cut) and after any frame that went without generation (a snapshot,
generation switched back on). Frame generation needs DLSS or FSR upscaling (any mode), because those
inputs come from it; either generator takes either upscaler's.

**Units (#1544).** The inputs are stated once, in the renderer's terms: motion vectors in UV units,
previous position minus current, unjittered; matrices as the renderer holds them; the jitter as
rendered, with the sign the upscaler applied noted beside it. Each generator derives its own
conventions from that (DLSS: motion vector scale = render size; FSR: its own scale, its depth
factors from the projection), so neither inherits the other's conversions. FSR also gets the frame
time, the display's peak (HDR10) and the frame counter; it resets on any break in that counter too.

**Why.** These are exactly the inputs the upscaler already has, so nothing new is rendered. A
reset after any break in the sequence keeps the generator from blending across unrelated frames.

## 5. Composing the interface on generated frames

**Decision.** The display encode writes three pictures in one pass on a frame that generates: the
finished frame, the hudless world, and a raw copy of the interface layer (RGBA8, premultiplied, as
the interface draws it). A generated picture decodes its world from PQ to nits, composes the copied
interface layer at paper white with the same function the real frame uses, and encodes back. Where
no interface pixel reaches, the world texel is kept bit for bit.

**Why.** The generated and real pictures of a batch must show the interface identically, or the
interface shimmers at the rendered rate. An earlier version stored the interface as a difference
of two encoded pictures; under a translucent window that difference carried the world behind it,
which then showed as a faint second copy over moving scenery. Composing the raw layer has no such
term, and the copy is 4 bytes per pixel instead of 8.

**SDR (#1536).** In SDR the world is already in the space the swapchain blends in (8-bit,
sRGB-encoded), so there is no encode and no decode: the world renders into the slot's world image,
the interface into the slot's layer, and one composite (encoding 5) produces every picture, the
real one included: `min(layer + world * (1 - alpha), 1)`, the swapchain's own blend, labels first,
then the interface. UNORM8 to float to UNORM8 is exact, so a pixel with no interface keeps the
world texel (the generator's output, or the rendered world) bit for bit, and the real and
generated pictures carry the same interface texels by construction.

## 6. Name tags, chat bubbles and hover text

**Decision.** While frame generation runs, world-anchored labels are drawn once per rendered frame,
sharp, into a small per-slot label atlas (from 1024x512, growing on overflow to 2048x1024, freed
when unused). Each picture draws them between the world and the interface, each where it was at
that picture's moment: generated frame k of n at k / (n + 1) of its on-screen motion since the last
rendered frame. Occlusion is decided when the label is drawn, as before.

**Why.** In the interface layer, labels step at the rendered rate while the world under them
glides. Two alternatives were tried in world and dropped:
- drawing labels into the generated world: the generator blended them, and they flickered and
  darkened;
- giving labels motion vectors: their content (text, bubbles, fading) still changes between frames,
  and the generator still blends that.
Moving them ourselves keeps them sharp and in step with the world.

**Containment.** The atlas exists only while frame generation runs. Frame generation off (SDR
without frame generation included), snapshots and VR keep exactly the layers they had. In SDR the
labels compose with the SDR blend (section 5). The code is in frame generation's
files; the HUD code only says which object a label belongs to. `RenderVKFrameGenMovingLabels` 0
puts labels back in the interface layer. Beacons and property lines stay in the interface layer.

## 6b. Falling rain and snow

**Decision.** On every frame that feeds the generator (any generator, SDR or HDR10; not VR, not a
snapshot frame) the weather's falling particles -- rain, snow, blowing snow, run-off drips, one
indirect draw -- leave the world pass and are drawn after the post chain into the interface layer,
depth tested against the display depth, before anything else in the layer. The colour goes through
the tonemap pass's own exposure and tail (`tonemapTail`, the same push block and grade set), and the
texel is solved against the finished world under it so that the layer's encoded-space composite
gives the scene's linear-light blend. The main view's jitter is removed. No setting: the old
`RenderVKFrameGenWeather` (default 0, the warp) is gone.

**Why.** The particles write neither depth nor motion, and the generator's inputs are the world
image and the upscaler's depth and motion. A flake in the world image moves with the flake while the
motion under it says "background", so the generator's flow drags the scenery behind falling snow
along with it. Both generators read the same world image, so the fix is upstream of both. Of the
alternatives:
- particles with depth and motion vectors: a flake is 1-2 pixels of translucent coverage; its motion
  replaces the background's under the dilation footprint, so the background is dragged the other
  way, and neither generator has a transparency input to say which wins (FSR's reactive mask is the
  upscaler's, not the interpolator's). Generated flakes would also be blended, not moved, wherever
  matching fails;
- leaving them out of generated frames: visibly drops the precipitation density to 1 / (n + 1);
- moving them per picture as the labels are (a phase-shifted draw per generated frame): right in
  principle, but it needs the particle state of each rendered frame ringed per present slot and a
  rasterised draw on the present thread for every generated picture; not done yet.

**Accepted.** The particles step at the rendered rate (at 3x a flake holds for three pictures); the
scenery behind them no longer warps, which was the visible fault. In HDR10 a flake is capped at paper
white (the layer is 8-bit). Final-pass looks (lens distortion, vignette, grain) do not reach them.
Splashes and run-off streams stay in the world: they stand still in it, so their motion is the
scene's. SL particle systems (`clean_particle.frag`) also write no motion vectors; they stay in the
world (they are sorted with the other transparents), with the same limitation as other content
animated in place.

**Containment.** Frame generation off: the same commands and the same SPIR-V for the weather (every
late-only line is under `WEATHER_LATE`; `framegen-weather` preprocesses the base variants to check).

## 7. A present thread

**Decision.** Generating, drawing the interface and presenting run on a present thread that owns
the present queue and every acquire and present of the frame generation swapchain. The main thread
hands it one job per rendered frame in one of 2 or 3 slots. The thread acquires one swapchain image
per picture just before it is needed.

**Why.** A batch is up to six presents per rendered frame, spread over that frame's time. Acquiring
and presenting them on the main thread would make the renderer wait for the display. Acquiring each
image just before use keeps the swapchain small (images per batch plus one, two on MAILBOX / FIFO).

**Cost.** Cross-thread rules: slots, timeline semaphores for "job done", bounded waits (section 9).

## 8. Pacing

**Decision.** The viewer's pacer thread spaces each batch's presents evenly between rendered
frames. Each gated present waits on a timeline semaphore through a tiny submit; the pacer reads
the GPU timestamp at which generation finished, maps it to the CPU clock (calibrated timestamps),
and signals each gate when its frame is due (high-resolution timer on Windows, `CLOCK_MONOTONIC`
on Linux).

In fullscreen the driver paces instead (`VK_NV_present_metering`): the first present of a batch
says how many follow, and the driver answers each batch (0: accepted). It accepts only while the
window is shown directly: always in exclusive fullscreen, and in borderless fullscreen while
nothing is over it. It refuses a window the desktop composes, and a new swapchain's first batches.
No NVIDIA application ID is needed; that was tested in world.

- Refusals are forgiven for 0.5 s after a new swapchain.
- A counted refusal drops 5x / 6x to 4x at once; they return after 2 s of accepted batches, so
  an overlay over the window does not flip the multiplier and rebuild the swapchain back and forth.
- After 30 refusals in a row the driver is no longer asked (each refused batch goes out unpaced);
  in fullscreen one batch asks again every 3 s.

With FSR (#1544) the driver is never asked: the viewer's pacer paces every batch.

**Why.** The GPU's work is hard to time; a semaphore signal is not. Our pacer works everywhere
(windowed, RTX 40, Linux) and is the fallback whenever the driver refuses. The driver's pacing is
more even than a CPU timer can be, so 5x and 6x, with the shortest gaps between pictures, are
offered only while the driver paces.

## 9. Never freezing the viewer

**Decision.**
- The present thread never declares the device lost and never judges; its own waits are bounded.
- The main thread never waits for it without a bound: a frame whose slot is still busy is dropped
  (100 ms on MAILBOX / FIFO, 250 ms on IMMEDIATE); a rebuild waits at most 2 s, then tries later.
- Whether a long wait is a GPU hang is decided only by the device's GPU progress watchdog, as for
  every other wait. A present the compositor holds is never reported as a hang.
- A hidden or minimised window drops frames instead of blocking. At quit the thread is asked to
  stop; one still inside a driver call is left behind and the process ends without static
  destructors.

**Why.** A present can sit inside the driver for a long time for reasons outside the viewer: a
minimised window, a compositor holding the present, a hidden Wayland surface. Any unbounded wait
on that would freeze the whole viewer.

## 10. Memory

**Decision.** All frame generation images (per slot: backbuffer, hudless world, interface copy,
depth and motion copies; one scratch image) are admitted as one set against free video memory:
three slots if they fit with a margin, else two, else frame generation says it has no memory. The
generator's own images (FSR's: about 90 MB at 2560x1440 with Quality upscaling) join the same set,
counted at the output size, the largest render size it may get. The label atlases are admitted
separately and freed when unused.

**Why.** A partial set (one slot admitted, another refused) leaves a state no code path handles
well. All or nothing keeps one rule, and the user gets a clear message instead of a stutter.

## 11. Reflex

**Decision.** Reflex through `VK_NV_low_latency2`: sleep at the top of the frame, the latency
markers, the frame cap as the sleep's minimum interval. Generated presents are marked out of band.
If the driver stops answering, Reflex steps aside and frame generation continues.

**Why.** Frame generation adds a frame of latency; Reflex removes most of what the render queue
adds. It is a plain Vulkan extension: no extra library.

## 12. Availability and settings

- **SDR and HDR10 output** (#1536). scRGB HDR output is not supported, nor an SDR surface whose
  only formats blend in linear light (`*_SRGB`); each has its own reason. SDR draws the interface
  into a separate layer **only while the swapchain is a frame generation chain**; on a normal chain
  it draws straight onto the world as always. Why scoped: the layer path is not bit-identical to
  direct drawing (below), it costs two display-size images and a composite per frame, and SDR
  without frame generation is what most people run -- scoped to the chain, frame generation off is
  pixel-identical by construction and a complete way back. Switching frame generation on or off in
  SDR therefore rebuilds the swapchain and the renderer's late-pass pipelines once (cached after
  the first switch).
- **Vsync is off while generating:** the pacer, or the driver, decides when pictures show.
- **RTX 40 runs 2x** (the GPU's limit); RTX 50 runs 2x to 4x, and 5x / 6x with driver pacing.
- **FSR runs 2x** (#1544: its shaders interpolate the half-way moment only); a stored higher
  multiplier runs as 2x and the status says why. It needs no add-on. Its device checks follow what
  its compiled shaders use (subgroup operations with a subgroup size of 32 or 64, storage formats).
- **One graphics queue** (#1544: AMD's and Intel's drivers; Mesa's RADV and ANV expose one): the
  present thread shares the renderer's queue under one lock, taken in the queue wrappers every submit and
  queue wait already goes through, and by the two present sites. Only on an IMMEDIATE chain: a present the
  compositor holds would hold the lock and freeze the renderer (section 9). The lock never judges: a wait
  for it is logged, never called a hang. Reflex is off there; the pacer's gates are waited on the CPU, at
  most 200 ms, since a gated submit on the shared queue would hold the renderer's submits behind it.
- **AMD Anti-Lag** (`VK_AMD_anti_lag`, #1544) where the driver offers it: the input stage after the frame
  wait, the present stage before the present / hand-over, maxFPS 0 (the viewer's limiter caps). On by
  default with a setting to turn it off; it shipped without an AMD test, its code being a few calls the
  extension defines.
- **The generator** is chosen automatically (DLSS where it runs, else FSR); the status line and the
  log name the one in use.
- **Windows needs hardware-accelerated GPU scheduling** (NVIDIA's frame generation requires it).
- On and off and the multiplier change live, without a restart. Every state that blocks frame
  generation has its own reason and, where the user can act, what to do.

## 13. Accepted trade-offs

- The interface and labels' content change at the rendered rate (section 2).
- Falling rain and snow step at the rendered rate (section 6b).
- GPU cost: n evaluates, n full-screen composites and a copy of the rendered frame per rendered
  frame, on the present queue. FSR's evaluate is about twenty compute passes (prepare, optical flow,
  interpolation, inpainting), with a full memory barrier before each; its GPU time is not measured yet.
- While SDR frame generation runs, stacked translucent interface draws can differ from direct SDR
  drawing by up to 2/255 per channel (each draw's rounding lands in the 8-bit layer instead of on
  the world), and the interface backdrop is the world without HUDs (as in HDR). Both only while it
  runs (section 12).
- Driver pacing needs fullscreen; windowed play is paced by the viewer and ends at 4x.

## 14. Tried and dropped

- Streamline (section 1).
- Pausing generation while typing (section 1).
- Labels drawn into the generated world, and labels with motion vectors (section 6).
- Falling precipitation in the generator's world with its default switch off (`RenderVKFrameGenWeather`,
  section 6b).
- Storing the interface as a difference of encoded pictures (section 5).
- Driver pacing only with an NVIDIA application ID: the driver paces without one in fullscreen.
- FSR through AMD's own frame interpolation swapchain (not built: it presents itself, as Streamline
  does; section 1).

## Prior art

The presenting side follows the design of NVIDIA's open-source RTX Remix (MIT), which drives
DLSS-G through NGX the same way:
[`rtx_dlfg.cpp` / `rtx_dlfg.h`](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/f25b969ceec3ae1dcc7ac7b0423128eb8af246b6/src/dxvk/rtx_render/rtx_dlfg.cpp)
(presenter and pacer) and
[`rtx_ngx_wrapper.cpp`](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/f25b969ceec3ae1dcc7ac7b0423128eb8af246b6/src/dxvk/rtx_render/rtx_ngx_wrapper.cpp)
(the NGX calls). Ideas taken from reading it:
- a present thread with an in-flight flag per backbuffer, separate from the renderer;
- a pacer thread that reads the GPU timestamp of the finished generation, maps it to the CPU clock
  with calibrated timestamps, and signals a timeline semaphore that each present waits on through a
  small submit;
- the present-metering request on a batch's first present, falling back to the CPU pacer when the
  driver refuses;
- Reflex's out-of-band markers for the generated frames' work and presents;
- a swapchain sized for the generated frames plus the real one.

No code was copied: the viewer's and the add-on's code is written for this viewer. Specific to this
viewer: keeping the interface out of the generator's input and composing it on every picture, the
moving labels, the bounded waits and "never freeze" rules, admitting the images as one set, and the
fullscreen driver-pacing rules.
