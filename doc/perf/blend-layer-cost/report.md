# Sorted blend layer cost and shared visibility

Production: `8e7e6aa4c7`, branch `blend-layer-cost`, based on mega `06734e5103`.

## Evidence and scope

The owner measured about 41 ms sorted alpha at Moochie, falling to about 14 ms with RT sun/projector shadows disabled. The approximately 27 ms difference is an owner measurement, not a timing captured by this work. Local-light cap 8, hit lighting off and layer reuse 1 did not materially change the 41 ms result.

Source explains why: `g_blend_cheap` disables blend hit lighting on glow/faint strands, but ordinary `sunShadowTCore` still reaches the RT sun walk. Front reuse previously required hit lighting, so it did not cover these fragments. `sunShadowRayRGB` sets `opaque_only = blend || far_cut`: the usual blend receiver skips translucent split/foliage tails, although alpha-tested candidates inside receiver range remain. The experiment does not separately attribute sun, terminator probes and projector rays. RT blend-projector rays are also controlled by `RenderVKRTBlendProjShadows`, default off.

## Per-fragment cost inventory

| Term | Work/cost class | Earlier knobs cover it? | Bisect bit |
|---|---|---|---:|
| Material/alpha | Texture fetch, UV/normal/decode; required even in accumulation | No | retained |
| Sun visibility | Inline alpha-tested RT traversal or shadow-map/mask taps; receiver probes | No, ordinary sun survives hit-off | 1 all; 4096 opaque walk; 8192 trans tails |
| Terminator probes | Extra receiver probe ray(s) near terminators | Not ordinary layer reuse | 1024 |
| Projector visibility | Per-light mask/reuse or optional inline query | Separate RT blend-projector setting | 16384 rays |
| Local lighting | Cluster-list walk, analytic BRDF, light texture fetches | MaxLights bounds list; hit-off removes extra hit evaluator only | 256 |
| Probes | Probe cluster walk, box influence, cube/BRDF reads | No | 2 |
| GI | Forward cache interpolation and reflection recovery | LayerGI only covered accepted front reuse | 4 |
| Reflections | SSR/RT forward gather, planar mirror taps | No | 8 |
| Atmosphere lighting | Transmittance/ambient terms | No | 16 |
| Post medium | Haze, water/volumetric fog and weather veil | No | 32 |
| Depth/survival | Tolerance, K lists, cover, self depth; sample mask | No | 64 |
| Surface preparation | Weather/cel/rez paint beyond material decode | No | 128 |
| Lighting aggregate | Skip lighting after material decode/preparation | No | 512 |

Loops and expensive taps remain per layer even after removing rays. Register pressure and those terms are candidates for the remaining approximately 14 ms versus approximately 4 ms OIT; their individual milliseconds have not been measured. Bit 64 changes fragment survival and overdraw, so differences are not additive term costs.

## Production shared visibility

`RenderVKBlendFrontLight=1` (existing default-on, unsaved switch) now applies independently of glow and hit lighting. Main sorted static/rigged slots select the no-ray lit module. Both shaders contain **zero OpRayQuery instructions**; the baseline RT/wire fragment contains 304. No missing record may launch a fallback fragment ray. Cluster alpha uses the same shared-policy branches, though its generic RT module still contains queries for other material classes.

The existing bounded front-light producer is reused where available; it is not forced on if the reflection producer is disabled. One fragment gather supplies shared sun/local visibility. The local ratio is applied once after the analytic local-light walk. Missing front records use current RT masks / visibility reuse where present, otherwise the existing unshadowed fallback. Sun reuse decodes both squared tint RGB and squared scalar alpha. First RT frames carry the shared policy even before front records become eligible. No new images, queue or per-layer ray budget.

This is a deliberate depth/material approximation: front visibility may differ from a deeper layer, scalar front sun loses coloured transmission, and a shared local ratio is not per-light visibility. Missing all shared records can remove shadows on those layers. Owner must compare sealed/interior scenes and contact shadows. Toggle `RenderVKBlendFrontLight=0` to restore the legacy per-layer path. Material alpha, glow, blend routing, captures and shadow-caster participation are unchanged. OIT mode 1 lit accumulation is outside this change and may still trace per-layer rays.

Any extra work in the existing front producer is bounded per pixel, not per transparent layer. Include `c/g_rtrefl_fwd` and deferred shadows in total-frame comparisons; the sorted timer alone is insufficient.

## OIT resolve

Mode 2 no longer defines `CLEAN_BLEND_HIT` in `clean_oit_resolve.frag`. The weighted mean is not an actual surface; per-layer hit visibility there caused the owner's dark wisps and approximately 6 ms resolve increment. Ordinary once-per-pixel sun visibility remains. Resolve SPIR-V query instructions fall 264 to 188; this is not a zero-ray resolve claim. Alpha/revealage, MRT formats, water-side split and glow accumulation are unchanged. The approximately 1 ms target is based on the owner's hit-off result, not a new timing.

## Live cost mask

`RenderVKBlendCostBisect` is U32, Persist=0, default 0. Production `kDebugViews=false` folds all diagnostic gates and mask loads away. Nonzero masks install only the sorted-alpha debug twins; OIT routing is not changed.

Use **32768** as the no-removal control, and `32768 + bit` for each row above. Compare only after `VKBlendCost` reports **active**; compiling/waiting frames are invalid. Debug twins are generic and have different specialization/occupancy from production, so do not compare their timings directly against mask 0. Set unrelated lit/debug views off. For diagnosing old rays use FrontLight=0, GlowSubset=0, same camera and settings. Restore mask 0 afterward.

Owner acceptance: compare FrontLight 0/1 with mask 0 at the original Moochie view; read sorted, front/reflection producer, deferred-shadow and total GPU times. Then OIT mode 2 / GlowSubset 1, HitLighting 0/1: resolve should stay visually/timing equivalent because the invalid hit evaluator is absent. Inspect faint/glowing wisps, rigged alpha, pre/post-water glow, a sealed room and layered contact shadows. No image-identical assertion is made for the new shared approximation.

## Validation (updated after completion)

Final standalone shader probes: seven of seven compiled with glslc -O and passed Vulkan 1.3 scalar-layout spirv-val. Baseline comes from exact pre-change mega source. Local pipeline executable statistics create pipelines only, never command buffers/submits. Driver stats and full Linux build results are recorded below when complete.

### Driver register probe

RTX 5090, NVIDIA 615.78.08, shader source frozen at `8e7e6aa4c7`, glslc Vulkan 1.3 -O. `pipestats` creates graphics pipelines and queries executable statistics; **no GPU work is submitted**. The representative specialization is `1=0,8=3,9=2,10=2,11=1,12=0,13=0,14=0,15=4,16=1,17=3,18=1,19=2`, drawn from the last VKSpec log (not asserted to be the original Moochie frame). Sorted comparison includes REZFX_WIRE. Tool render targets are RGBA16F per output with D32 depth and blending disabled, so these are shader-pressure probes, not exact viewer pipeline performance measurements.

| Fragment | Baseline registers | New registers | Register-only warp ceiling baseline/new |
|---|---:|---:|---:|
| Sorted selected path (RT/wire -> shared no-ray/wire) | 255 | 168 | 8 / 12 |
| Unlit OIT accumulation | 128 | 128 | 16 / 16 |
| OIT resolve RT twin | 168 | 255 | 12 / 8 |

Ceilings use floor(65536/(32*registers)); allocation granularity and other limits may lower actual occupancy. Sorted binary falls 2,672,896 to 1,755,904 bytes; accumulation remains 466,048 bytes. Driver reports an implausible high word in Local Memory Size, even for the vertex shader, so no valid spill-byte claim is made from that field. Do not infer frame milliseconds from these numbers.

The resolve register increase is a risk, not hidden by the smaller binary (1,856,640 -> 1,611,392 bytes). It retains ordinary sun traversal, whose scheduling/register allocation can change after removing the hit evaluator. The owner must read resolve and total-frame times on the same view; the source change alone does not prove the 1 ms target.

Source suite: all checks except the old HDR BASIC-site count passed on the first full run. The new guard excludes shared visibility from BASIC and cannot alter the early HUD return. Updating the documented count from 6 to 7 is commit `640747287d`; its complete framegen-direct check and blend-hit, blend-glow-layers, blend-layer-cost checks all pass on the final tree. This is a corrected contract assertion, not a skipped check.

### Reproducible validation receipts

Owned worktree: `/home/bryan/Documents/git/slipstream/agents/worktrees/gpu-timer-scopes`, now branch `blend-layer-cost` after completion of its earlier timer/Conversations work. No other worktree was modified. Baseline source is mega `06734e5103525920eec6064761151f0baf7a04bb`; it is an ancestor of this branch, so merging into that mega tip is a fast-forward with no conflict.

Artifacts, never committed: `/home/bryan/.cache/slipstream-build-tmp/blend-layer-cost/`. `final-probe-compile.log` has seven final shader PASS receipts; `probes/commands.json` records final compile commands; `probes/driver-stats.log` holds all six baseline/new fragment comparisons. Earlier prototype outputs are not final validation. `linux-build.log` holds the first check failure; `final-focused-checks.log` holds its correction and the four final focused passes. `linux-build-final.log` holds the subsequent actual Linux build (RUN_CHECKS=0 because the full source suite plus corrected failure was already run). All owned build descendants are nice19, at most four build jobs, TMPDIR on disk, and no generated shader headers are staged.

These receipts do not replace the owner's same-view GPU/image acceptance. No viewer or package was launched, and no box locks or owner GPU lock were touched.

### Final result

Full Linux `make build TESTCTL=1 RUN_CHECKS=0` **PASS**, exit 0, with real -Werror viewer objects, link and package; production shader source is `8e7e6aa4c7`, test correction `640747287d`. 480 world + 42 FSR variants regenerated successfully with push/shared-memory contracts. `make check-package` **PASS**. The full source run plus corrected focused run cover all 133 checks: 131 passing and two existing warnings (framegen-local-id unavailable locally; skin-import fixture warning). Build emitted existing third-party/project warning categories, so this is not a zero-warning claim. No platform code changed; no Windows build was run.

READY for owner A/B, not visually or performance-accepted. Ordered source commits: `8e7e6aa4c7`, `640747287d`; the following documentation commit is optional for production. No pushes, viewer runs or GPU command submissions.
