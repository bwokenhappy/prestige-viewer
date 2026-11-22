# Depth prepass source audit

The comparison is read-only against Aurora GPL-3 source at
`5a0995037a3de34812cbffba45fe0d75e845da0c`. These are descriptions of approaches,
not copied source, snippets, shader expressions, or implementation structure.
The inspected Aurora checkout is clean. The owner's release executable has SHA256
`8583607cf32157e2ca2965516fde06ea030f72020b78774e99c73df573b22fcb`;
its exact build-source revision has not been established, so today's source cannot
prove what that November 7 binary executed. Slipstream's fresh baseline is
`ae1f063eb7ca9c2c0e0bbec3921d2c21821e493a`.

## Comparable work and settings

Owner RTX5090 observations are house 1.30/0.54 ms, overview 0.94/0.58 ms,
club 0.41/about 0.6 ms (Slipstream/Aurora). These are leads, not our new hardware
measurements. Owner settings: RT/GI/reflections/acoustics off, shadow maps,
16 projector shadows, DLSS Balanced, lowered LOD/draw distance. Reflection probes
remained on in Slipstream overview/club. There is no matched pair of complete
GPU traces proving equality of all work behind those short pass names.

| Concern | Slipstream source | Aurora approach | Consequence |
| --- | --- | --- | --- |
| Opaque coverage | Frustum-visible opaque and mask lists, single/double sided, terrain; legacy rigged and selected clusters/snow where active | Visible opaque/mask and terrain categories, including skinned surfaces | Neither limits the prepass to large occluders. Actual geometry/LOD residency still needs runtime evidence. |
| Two phases | Prior-visible static list first; current Hi-Z tests all candidates; survivors absent from phase 1 form phase 2. Rigged/cluster/snow are phase-1 occluders. | Also separates prior-visible and newly visible geometry around current depth testing | The two raster phases are complementary sets; blindly dropping phase 2 loses newly visible surfaces. |
| Adaptation | Mode 2 is adaptive; low cull share stands down and periodically probes. Mode 1 always runs; mode 0 frees/disables it. Camera cuts seed with a plain prepass. Lower preset tables set 0. | Current source has a separate occlusion path and GPU-cull bins | A configured 2 does not establish that both phases ran in a particular measured interval. Include seed/probe/stand-down coverage. |
| Blend opacity | Default-on `RenderVKBlendOpaqueGBuffer`: near-opaque blend texels write depth and motion/normals in phase 1, then G-buffer; soft fringe remains transparent | Separate blend-depth work supplies contact/AO/refraction and temporal inputs; additional rigged blend-depth paths serve transparency | Its first depth timer excludes work ours may include. Turning our option off changes which consumers see the surface and is not an unchanged-image optimization. |
| Attachments | D32 reversed-Z clear 0. Deferred slim prepass: depth alone or RGBA16F motion/depth/class plus R8 no-history. Normals (RGBA16F) only when required; deferred AO normally reads G1 afterward. | D32 reversed-Z clear 0; optional RGBA16F packs mesh normal and motion together | Motion payloads and consumer contracts differ. Four versus two floating-point components is not sufficient to claim equivalent data. |
| Alpha pixels | Mask/blend sample alpha with material/UV/LOD/rez rules and discard before writing depth; later EQUAL/relaxed twins consume exactly those depths | Mask and separate blend passes also test alpha | Mask and blend FS early depth *writes* cannot be forced ahead of discard. |
| Vertex streams | Quantized 20-byte static interleaved stream; pre-skinned 20-byte rigged stream, previous skin data separately. Reduced position/mask/normal/motion VS variants selected by default-on `RenderVKDepthOnlyVS`. | Interleaved 24-byte mesh stream plus parallel compact skin stream, specialized depth entry points | Ours is not simply fetching a full large shading vertex instead of a position-only stream. Creating another buffer needs measured benefit including upload/skin cost. |
| Rigging | Pre-skin work has its own GPU timers; previous pose/normal data supplies temporal receivers | Skin/pose work is arranged differently in the rendering stages | Include pre-skin and full frame, not only raster time. |
| MSAA/VR/VRS | Main prepass uses single-sample pipelines; current source supports layered stereo paths. VRS attaches later shading scopes rather than silently reducing mask coverage | Single-sample consumer depth, with a separate multisampled depth prepass for MSAA opaque shading | With MSAA, include Aurora's additional depth work; desktop tests cannot certify multiview or VRS/VR behavior. |

Primary source locations (local, no third-party code copied):

- Slipstream `indra/newview/vkgfx/llcleanrenderervk.cpp`: pipeline family around 40975;
  deferred attachment selection around 47432; actual mode selection around 47610;
  drawing and timer boundaries around 48200–48425; alpha late-depth around 49066.
- Slipstream `clean_occl2.comp`, `llcleanoccl2adapt.h`, `featuretable*.txt`,
  `clean_draw.vert`, `clean_draw_rigged.vert`, `clean_prepass_{mask,blend,motion,normal}.frag`,
  `clean_receiver_{motion,normal}.glsl`, and `vkWorld{Static,Rigged}VI`.
- Aurora `crates/aurora-render/src/renderer.rs`: depth pipelines around 3910,
  single-sample drawing around 7272, separate occlusion blend depth around 7365,
  multisampled depth around 7774; `gpu_cull.rs`, `occlusion.rs`, `types.rs`,
  `shaders/object.wgsl`, and `gtao.rs` describe the related inputs.

## Accounting

Slipstream submarks are *successive intervals*, not inclusive named scopes.
`g_prepass` may be only the residual after `g_pre_static`, rigged/cluster/snow
marks. Count all prepass raster intervals, attachment/clear interval
`g_pass1_begin`, `c_occl_split`/seed, `c_hiz`, `c_occl_p2`, and `g_prepass_p2`.
For optional marks, weight mean by their sample count / frame count; an absent
mark is not a measured zero-duration sample. Never sum per-pass percentiles.
Use the harvested `@async` flag, not a `g_`/`c_` name prefix, to distinguish
queues. Keep graphics/async queue intervals separate: their sum is not total latency.
Also show G-buffer, AO, shading, transparency, pre-skin, and the actual GPU total.
Aurora's equivalent chain must include both depth phases and culling, plus its
separate blend/contact/input depth work and MSAA depth when relevant. Without
matched traces those combined cross-viewer times remain unknown.

## Candidate and correctness argument

The existing `drawPList` call submits both prepass phases with the full
`mOccl2Cap` as Vulkan's `maxDrawCount`. The regular main draw buckets already
use conservative per-class bounds. The renderer documents Pascal's cost for a
large maximum even when the actual GPU count is small or zero. This is a
source-proven omission; its share of the owner's RTX5090 gap remains an inference
until matched owner testing.

P1 and P2 are each subsets of the same current main-cull opaque/mask buckets
0, 1, 6 and 7. Reuse `mdiMaxBucket` for the corresponding row, capped by the
allocated P-list capacity. The helper already accounts for double-sided/HUD
conservatism, four frames of class changes, pending full uploads, unknown store
mirrors and the existing bounds-off control. The shader still supplies exactly
the same actual count, and a correct upper bound cannot truncate that count.
Only the allowed scan ceiling changes. Count/command buffers, offsets, stride,
shader modules, barriers, attachment formats, culling, motion and alpha rules,
and primitive ordering all remain the same.

The CPU regression fixture extracts and executes the production class accounting,
limit helper and draw lambda. It checks both P1/P2 subsets over unequal buckets
and 40 frames of reclassification, including GPU rows lagging within the history
window, pending uploads, unknown mirrors and the bounds-off fallback. It verifies
unchanged buffer offsets/stride, no indirect-count clamp, and a reduced ceiling
for the known population. Runtime full-frame and image checks are recorded in results.md; the source-level
argument is assessed separately from those finite measurements.

Other candidates considered but not applied: the static VS computes unused
previous-normal TBN data for class-1 receivers, and optimized motion-fragment
SPIR-V loads three material fields before declining class 1. Guards could avoid
that work, but require shader recompilation/equivalence testing. The more direct
indirect-limit omission is tested first. Forced early tests for discard-free
opaque fragments are also unmeasured; drivers may already perform them. Mask
and blend variants must keep depth writes after discard.

Rejected as default changes without unchanged-image proof: disabling
BlendOpaqueGBuffer, dropping masks/rigged/terrain, disabling motion or previous
normal metadata, removing phase 2, omitting normal-map history for class 2,
quality/sample reductions, and changing depth-equal/occlusion policy.

## Additional source constraints

The static alpha lists currently remain unconditional phase-1 occluders. The
existing visibility history/P list partitions four opaque/mask rows, while the
test also filters the two static alpha lists for later consumers. This can leave
hidden blend geometry in the first raster stage even though later consumers
skip it. It is a plausible additional cost in an alpha-heavy house; a material
population or actual draw-count measurement is needed before calling it the
cause of the owner's 0.76 ms difference.

Extending the existing partition to alpha is not an automatic unchanged-output
fix. It changes ordering relative to rigged/other phase-1 occluders; equal-depth
surfaces can then select a different first writer of motion/normal metadata.
An additional current-depth test before alpha also costs synchronization and
must be assessed as a total chain. Neither structural change is included in the
narrow unused-work candidate. No GPL implementation structure is being ported.

## Measured cause and remaining attribution

[Vulkan's indirect-count command](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndexedIndirectCount.html)
executes the lesser of the GPU count and the supplied maximum. A bound that
contains every possible P-list entry therefore preserves the command sequence.
This patch does not lower the actual GPU count or read a previous frame's
visibility count as a limit.

The separate Pascal house draw-coverage diagnostic saw P-list capacity 122624,
CPU static class counts opaque 64590, mask 7497, alpha 9636, double-sided opaque
12 and double-sided mask 0 (81735 total live instances). In that settled
population, the helper supplies row ceilings 68639, 7965, 76, 64: their sum is
76744, versus the old four full-capacity ceilings totalling 490496 per phase.
Those are scan ceilings, not actual submitted primitive counts. Late diagnostic
averages reported 8185 tested, about 3190.5 culled, 4994.5 in phase 1 and only
1.26 newly revealed in phase 2. GPU alpha-test candidates were 1353, with about
575.6 culled. This diagnostic is not pooled with performance runs.

The controlled production change removes about 2.99 ms from Pascal's house
raster intervals with setup/cull unchanged. The omission and causal timing
improvement are demonstrated. The internal driver implementation responsible
for processing a large maximum is inferred, not traced with a vendor profiler.
AMD's near-neutral result is consistent with that mechanism being driver/GPU
dependent. RTX5090 improvement, and how much of the owner's Aurora gap it
explains, remain unmeasured. Different equivalent-chain work also prevents
attributing the whole cross-renderer gap to this one omission.

A deferred G-buffer cannot simply replace every prepass consumer. Current Hi-Z
and occlusion decisions happen before it; motion/no-history data has an existing
producer contract; EQUAL mask consumers depend on the earlier discard result.
Normals already come from G1 in the default slim/AO path. Removing another
attachment or class would need a coordinated consumer redesign, complete image
controls and full-chain accounting, rather than moving its work later.
