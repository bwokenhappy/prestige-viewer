# Dense occluded local lights: architecture investigation

Owner lead: many nearby point lights behind walls/floors still cost time and introduce noise on RTX5090 with DLSS Ultra Performance. This is not a controlled NVIDIA measurement. The3060 report uses DLSS Performance; native TAA→DLAA migration is withdrawn for that case. This investigation targets shared architecture, not a broad historical bisect.

## Source proof at5d60acdf6a (renderer baseae1f063eb7)

| Stage | What it actually does | Consequence for hidden lights |
|---|---|---|
|Cluster/froxel admission|Range/frustum tests; overflow walks remain lossless|Walls do not remove a light from the initial list. Dropping overflow lights would lose valid illumination.|
|ReGIR build, `clean_regir_build.comp`|Two walks over up to256 lights per cell: count/target sum, then exact list or stratified proposal slots. Both populations use unoccluded `regirCellTarget`.|Hidden lights consume build work and, in dense cells, proposal support. This is distinct from receiver-level RIS rejection.|
|LVC maintenance, `clean_deferred_light.comp::lvcAssign/Dirty/Plan/Build/Finalize`|Static unattached point lights only; two6144-texel maps/frame, eight rays/texel. Dirty watches old/new geometry bounds; a clean pose-matched map is usable.|Discovery costs rays; full-map invalidation and waiting for two still frames can temporarily admit hidden lights. Alpha, fixtures, moving/thrashing geometry do not certify occlusion.|
|LVC receiver test, `lvcBlocked`|Requires usable map, receiver beyond0.5m, emitter radius≤5% of distance; tests max-dilated map depth plus0.05m+8% distance+twice emitter radius.|Conservative unknown/open outcomes deliberately preserve light. Finalize opens texels when the footprint crosses a cube face or required soft-disc dilation exceeds7×7. Tightening margins without proof risks leaks or missing light.|
|Pass15 SELECT|Map rejection precedes RIS target evaluation in both grid-window and cluster walks. Reused picks are rechecked at the receiving point. The light-set history referenceW0 is unchanged.|Existing hard rejection removes candidate work and rays only where the map certifies blocked. It cannot replenish proposal slots already spent on hidden lights.|
|Exact/specular/mover work|Separate exact/specular selection and mover-exact work coexist with RIS; later gates regenerate terms. Mover-exact uses its own plan/history/visibility jobs.|A map-blocked traced job is not automatically a failed primary RIS rejection. Identify the originating path before removing it or changing normalization/history.|
|Temporal/spatial visibility reuse|Blocked picks retain zero-weight evidence with sample counts; moved-light and reprojection checks apply.|Discarding blocked evidence or using stale/neighbour visibility as a hard veto can bias lighting. Reuse must preserve support and respond to geometry/light changes.|
|Indirect/secondary lighting|GI and reflection hit lighting use their own receiver and proposal populations.|A primary-camera hidden light may still illuminate a bounce or reflection. Never globally suppress it from all populations merely because it is blocked at the primary receiver.|

## Measured AMD evidence already available

Source59aea7da31 diagnostic binary, fixed HOUSE capture/path/settings, noFG, render1920×1033/output3840×2066; actual1594-frame GI3/ReSTIR/pass15 coverage. HOUSE has4680 missing textures and no avatars/probes: cost attribution only, no image-quality conclusion.

| GPU interval | HOUSE mean ms | Complete synthetic club mean ms |
|---|---:|---:|
|Total frame|19.22308|34.39603|
|ReGIR build|0.01102|0.22195|
|Base SELECT|0.34118|0.63213|
|Base VIS|2.88846|2.30196|
|Adaptive SELECT|0.10355|0.39205|
|Adaptive VIS|0.88139|1.22814|
|LVC build|0.40664|0.01295|
|LVC dirty|0.07722|0.03082|
|Deferred async span|8.27282|13.04524|

Intervals overlap; conditional timer means are not additive critical-path savings. Club has serialized rigged motion and complete geometry/textures but zero captured probes and fallback environment. Both are AMD measurements, not5090/3060 measurements.

A settled HOUSE statistics window has106 slots,105 usable maps, zero builds, about2.99million candidate skips/frame, yet pass0 reservoir/other traced jobs are approximately75%/76% statically blocked beyond the near-surface classification, with another~25%/~24% near-surface blocked. This establishes residual visibility work even with maps largely usable; it does **not** establish which admission limitation dominates. Those logging windows are not exact benchmark-frame aggregates. Mode3 changes work (including static re-walks); its timings are not production cost. No map-blocked/reached invariant violations were observed in that campaign.

## Measurement and prototype order

1. Existing same-binary reach-map×adaptive-cap factorial separates current policy cost from implementation. Turning maps off or cutting adaptive work is diagnostic, not a default recommendation.
2. Added opt-in mode3 admission reasons: off, projector, attached, no slot, unusable, near, wide disc, open map, margin, map-blocked. Count all pass0 traced local jobs and their statically blocked subset separately. No additional ray; production VIS and isolated VIS SPIR-V remain byte-identical. These counters measure jobs, not every candidate, and must not be called a candidate-admission hit rate.
3. Synthetic128-light closed/open-wall controls preserve camera/light positions and geometry count. Compare options within each capture; opening the wall legitimately changes direct/indirect lighting. Keep clean timing, diagnostic overhead and raw240-frame image comparisons separate. Zero-light fixtures exist for a later leak reference; do not claim those execute an active ReSTIR path.
4. Choose a bounded option from the dominant measured cause. If dirty/rebuild dominates, conservative angular dirty regions with a halo must reopen affected texels immediately and retain whole-map invalidation on unknown changes. If open-map footprints dominate, improve geometric coverage without shrinking the proven footprint or ray-quality budget. If post-map proposal scarcity dominates, investigate cell-level pool shaping only with conservative **whole-cell** visibility and current-scene validity, preservingW0 and population0/indirect support.
5. Current LVC maintenance runs after ReGIR on the shadow queue. Reading last frame's maps in the earlier proposal build without current invalidation is unsafe. A cell-pool prototype must account for synchronization and total critical path; eight corner visibility samples are not a proof that a box is occluded.

No zero-cost discovery promise. No desktop default changes. No use of accumulated blocked samples as an unconditional visibility certificate. DLSS Ultra Performance's smaller internal resolution reduces pixel work but not necessarily world-space grids, map discovery or fixed-resolution volumetrics; RR signal routing remains an unmeasured NVIDIA coverage limit.

## Primary implementation comparison

NVIDIA's RTXDI integration separates world-space ReGIR proposals from surface-level ReSTIR selection and supports both primary and secondary receivers. That supports retaining separate receiver/population semantics here; the SDK is not evidence that simply enabling a grid eliminates visibility cost. [RTXDI integration guide](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md).

RTXDI's denoiser-confidence implementation compares the same surface/light sample across frames and requires the appropriate scene BVH for that comparison; its discussion links the production Quake II RTX lineage and explains why unrelated random-sequence changes create false history resets. This is relevant to a redesign of visibility reuse and invalidation, not a drop-in timing claim or justification for reusing stale visibility. [RTXDI confidence guide](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Confidence.md).

## Optional residual deferred-light mask prototype

`RenderVKPerfRadianceLightMask` (nonpersistent, default0) removes the current point-light subset of `rs_set` from the deferred local-light walk only while the actual pass15 radiance image is active. Those lights already return exact zero inside `localLight`, after redundant record/range/attenuation work; pass15 supplies their whole contribution once. The option masks tiled/froxel words before iteration and skips matching cluster entries. It does not change visibility, candidates, PDFs, ray allocation, denoiser histories, forward lighting, projectors, or indirect/reflection populations. VR explicitly retains the original branch. Unknown/inactive ownership retains the old walk.

This targets repeated light-loop work even when LVC cannot reject a hidden light. It does not itself cure sampling variance. Image equivalence and lower cost were the initial hypotheses. Completed timing and limited image controls are now in mask-results.md; universal image/noise equivalence is not established. The32-byte LightInfo tail is appended; existing field offsets do not move. This is an option, not a default promotion.

## Completed current-binary reach-map × adaptive-cap factorial

Source95f820bbf1, same HOUSE bytes/path/template/output3840×2066/render1920×1033/noFG. Actual GI3/ReSTIR/pass15 coverage1594/1594 for every accepted run. This is a policy/ray-allocation diagnostic, not a historical implementation comparison or NVIDIA timing. HOUSE remains excluded from quality/noise claims.

|Reach map|Adaptive cap|GPU mean ms|p50|p95|p99|
|---|---:|---:|---:|---:|---:|
|1 A1|25|19.138478|19.718679|21.893641|26.405960|
|1 A2|25|19.131995|19.678280|21.928080|25.684561|
|0|25|18.767805|19.390360|21.302240|26.334040|
|0|0 (uncapped)|18.814967|19.388960|21.326120|26.490520|
|1|0 (uncapped)|19.166038|19.701509|22.009480|26.663040|

Baseline mean19.1352365ms and observed range0.006483ms. Reach maps' net mean cost is0.3674315ms at cap25 and0.351071ms uncapped. Cap25 saves0.0308015ms with maps or0.047162ms without. Single altered-setting runs limit distribution/repeat claims; no default change follows.

|GPU interval mean ms|Map1/cap25 A mean|Map0/cap25|
|---|---:|---:|
|LVC maintenance|0.521361|absent|
|Base SELECT|0.341211|0.295158|
|Base VIS|2.877269|3.039996|
|Adaptive SELECT|0.103386|0.096108|
|Adaptive VIS|0.878523|0.891729|
|Deferred async span|8.220857|7.846595|

Maps reduce traced visibility cost but add upkeep and candidate-lookup work; their net frame cost differs from the maintenance interval. Do not sum overlapped marks or use this incomplete capture to decide whether the quality benefit is worthwhile. This does not explain a16.7ms3060 increase or9.7ms5090 increase. The following admission diagnostics and complete synthetic controls investigate why hidden-light work remains.

## Admission evidence and next structural change

Source5d60acdf6a HOUSE accepted1594/1594 required frames, mode3 mean19.948162ms. This includes diagnostics and is not production cost. A settled logging window has105/106 usable maps,zero builds/static changes, and these pass0 job classifications:

|Reason|All jobs/frame|Statically blocked, including near|
|---|---:|---:|
|Open map|333282|333020|
|Depth margin|164565|164407|
|Map blocked|5863|5862|
|All other reasons|0|0|

These are rounded logging-window counts, not exact timed-frame aggregates or candidate counts. A moving window has a counter-consistency anomaly (map-blocked subset3105 > all3014); its ratios are not admissible exact probabilities. The settled counts support the dominant categories without establishing whether each open result came from a cube seam, oversized footprint, ignored fixture/alpha/dynamic geometry, or a genuinely open sampled direction. The diagnostic currently cannot distinguish those subcauses.

The sealed128-light verify control has128/128 usable maps andzero rebuilds after discovery. Its settled window reports1864337 map-blocked and120943 open-map pass0 jobs/frame (all statically blocked), zero reached-light invariant violations. Approximately94% of these traced jobs are map-blocked in verify mode, which deliberately skips none. This is evidence that maps can reject most blocked work in this simple enclosure; it is neither proof of universal conservative correctness nor an amortized production timing.

The next structural direction is a **bounded conservative maximum hierarchy for angular footprints**, including cube-face boundaries. Current finalize opens a texel when the required footprint leaves its face or exceeds7×7. A hierarchy could answer the same or a larger proven angular footprint with a bounded number of maxima, avoiding those artificial open results without shrinking the footprint or safety margin. Build the hierarchy from the existing discovery samples; charge its build/memory/synchronization cost and measure total-frame amortization. Cross-face footprint mapping needs a complete angular cover, including corners and projection distortion; sampling only a few directions or eight receiver-cell corners is not a visibility certificate. Unknown/invalid coverage stays open. This preserves the existing sampled-map safety model; finite discovery samples themselves are not a mathematical proof against arbitrarily small holes.

This is a design proposal, not a measured implementation. Before implementing it, subdivide open-map diagnostics into seam/footprint/raw-open causes and require verify-mode reached-light checks, thin-wall/opening/moving-caster controls, and same-scene noise/images. Receiver-specific certificates must never globally remove legitimate indirect/reflection light. A separate conservative dirty-region halo can reduce rebuild work, but the measured HOUSE dirty stage is only~0.077ms and remains lower priority than residual VIS and repeated lighting work.

## Sealed128 clean timing result

Closed128 clean timings complete (5d binary,362/362GI3/ReSTIR/pass15 frames, same capture/settings/noFG): map-on A13.759295/13.759240ms, mean13.7592675, range0.000055; map-off14.304438ms, +0.5451705ms. BaseVIS map-on~0.0770ms vs map-off0.5720ms. Deferred local interval~6.2ms persists, motivating queued57f mask; interval is not promised removable time. Verify mode14.761839ms changes skip policy and adds counters, so its delta is NOT pure counter overhead. Images pending; no default or NVIDIA claim.

|Run|GPU mean ms|p50|p95|p99|
|---|---:|---:|---:|---:|
|Map1 A1|13.759295|13.529870|14.288920|20.454201|
|Map1 A2|13.759240|13.511400|14.114200|21.684601|
|Map0|14.304438|14.046410|14.822400|22.068119|
|Map2 + mode3 diagnostics|14.761839|14.510520|15.301680|21.709641|

Map1 clean LVC maintenance~0.0673ms and ReGIR~0.0530ms. Exact discovery/rebuild counts are from the separate verify windows; do not silently substitute them into clean timed frames. The map0 arm has one repeat; A/A spread measures repeatability of the fixed camera workload, not confidence bounds for all scenes.

## Visible128 clean timing result

Visible128 clean timings (same5d source,362/362GI/baseVISframes, same within-scene identities/noFG): map1 A15.231468/15.222170ms (mean15.226819, range0.009298); map0 15.043361ms. Maps cost0.183458ms here, versus saving0.545171ms in the sealed scene. Visible baseVIS0.542831ms(map1) vs0.559475ms(map0); SELECT0.408761 vs0.333928ms. Settled verify window128/128usable,zero rebuilds,zero map-blocked jobs; open-map124983 jobs, margin1860297, only1blocked margin job. Existing reach maps therefore distinguish this simple occluded/visible control, but are not an unconditional speedup. Complete-scene noise/images remain pending.

|Run|GPU mean ms|p50|p95|p99|
|---|---:|---:|---:|---:|
|Map1 A1|15.231468|14.966280|15.820760|22.888479|
|Map1 A2|15.222170|14.974240|15.885500|22.007080|
|Map0|15.043361|14.786000|15.783080|22.197241|
|Map2 + mode3 diagnostics|15.537267|15.269000|16.207359|23.280439|

The closed/visible frame-time difference also includes legitimate direct/indirect illumination changes and cannot be called solely a shadow-ray delta. Each map A/B holds its own capture fixed. Both captures use1920×1033 render/3840×2066 output and the same fixed camera; low-resolution DLSS/RR remains outside this measurement.

## Completed HOUSE residual-mask A/B

Renderer57f164d27d, only RenderVKPerfRadianceLightMask changes; both other prototypes off, clean counters2, noFG, same capture/camera/settings/render1920×1033/output3840×2066. All1594required GI3/ReSTIR/pass15 frames in every run. Mask106points observed on both B runs. HOUSE remains truncated: this is timing evidence only.

|Run|GPU mean ms|p50|p95|p99|
|---|---:|---:|---:|---:|
|A1 off|19.207245|19.819559|21.982840|25.507240|
|B1 on|18.772575|19.328199|21.497400|24.969681|
|B2 on|18.738689|19.264561|21.554159|25.670401|
|A2 off|19.145650|19.710859|21.952400|26.005619|

Amean19.176447875ms, range0.061595; Bmean18.755632230ms, range0.033886. Mean change−0.420815645ms (−2.19444%). P50/p95 improve in these repeats; p99 distributions overlap and remain variable. No confidence interval or general scene-wide speedup is inferred from two repeats.

Deferred local mean0.695075→0.270775ms (−0.424300), baseVIS2.888981→2.888834, adaptiveVIS0.882905→0.881170. The total reduction closely follows the removed consumer work, with unchanged configured sampling and visibility intervals. CPU-frame mean22.265085→21.869592ms; it includes GPU/presentation pacing and is not isolated CPU execution time. Complete-scene mask images and club/dense timing remain pending. No default change or5090/3060 timing claim.

### Admission-counter interpretation limit

Source review also explains why the new reason buckets need not equal the existing `lvcBlocked` total exactly: the diagnostic classifies a half-float depth at the light's stored open radius as `open-map` before applying the depth-margin inequality. The production predicate only applies that inequality; sufficiently out-of-range receivers can satisfy it even for an open depth. Treat the buckets as explanatory map-state categories, not an exact replay of every production rejection decision. Future diagnostics should separate out-of-range jobs and compare the actual predicate in the same invocation. This does not explain or excuse the separately noted subset>all anomaly; that remains unresolved and precludes precise probability claims from affected windows. Clean timing and the mask prototype do not enable these counters.

### Exact-zero ownership proof anchors

`clean_deferred_light.comp::deferRsStoch` returns current ReSTIR membership excluding projector records; it has no receiver-position restriction. `clean_regir.glsl::rsRecordLight` explicitly returnsfalse for every point light. In `clean_draw_common.glsl::localLight`, the non-VR point branch returns exactzero when stochastic ownership and radiance mode are active, before its BRDF, glare or visibility-tap work. The mask applies precisely that current point subset, additionally gated by the same active image/frame conditions. CPU upload order copies the same current `mRsSet` into LightInfo and the ReGIR header. Projectors are deliberately excluded because their ambient contract is conditional. This is source proof of the intended equivalence, not a substitute for images or unsupported-path validation.

Angular footprint coverage addresses the open-map subset only. The separate depth-margin subset requires its own evidence: finer local representation or a receiver-specific occluder certificate with explicit geometric/motion validity. Merely reducing the8%-distance margin or accepting a cached blocked sample indefinitely would risk missing legitimate light. Keep that branch as a future proposal until discovery cost, invalidation response and light-leak/noise controls establish correctness.

## Campaign complete

The final optional-mask reductions are0.421ms HOUSE,0.957ms club,5.737ms closed128 and5.785ms visible128 (all accepted repeats retained). Base/adaptive visibility intervals stay essentially unchanged; deferred local work supplies the reduction. Source57f has full same-scene distributions in [mask-results.md](mask-results.md), while [images.md](images.md) retains the initial visible-noise concern and its bounded repeat. Pascal's non-RT mask control shows no established effect, as expected from its inactive ownership guard; small A/B image differences exceed A/A, so strict parity is not claimed. Both hosts are restored. No default, push or merge.
