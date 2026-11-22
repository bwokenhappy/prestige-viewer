# ASTRA measured frame-cost milestone — architecture work active

2026-11-18. Assigned gpt-6-astra. Own worktree `agents/worktrees/astra-perf-architecture`, branch `astra-perf-architecture`, report/code tip `57974688eb`, tested renderer `95f820bbf1`; fresh base `ae1f063eb7ca9c2c0e0bbec3921d2c21821e493a`. No pushes/merges, child agents or local GPU/viewer runs. Native parent route works; HERDR_ENV absent, no guard bypass.

**Result so far:** matched AMD HOUSE endpoint improved from22.48252ms to19.13331ms (median run means), so this replay does not reproduce the owner's regression. The largest measured current lighting subpass is base shadow VIS2.88ms, followed by adaptive VIS0.88ms; LVC costs0.52ms. These intervals overlap other work and are not an additive frame budget. The exact #1604 HOUSE pair shows no detectable regression (−0.0120ms, within repeat spread); no further historical bisect is planned under the owner's revised priority. Focus now is critical-path architecture and correctness-preserving optional prototypes.

## Limits and identities

Owner uses **DLSS Ultra Performance**, not FSR. Owner's uncontrolled RTX5090 observation80->45FPS is12.5->22.22ms (~9.72ms increase); adaptive-off60FPS is16.67ms (~5.56ms on/off difference).20–35ms refers to total frame cost. AMD numbers and FSR option costs do not explain or measure that NVIDIA regression.

No complete compatible HOUSE found in authorized amdbox bench roots. Used identical format1 truncated HOUSE:4680 missing textures, zero avatars/probes. Qualified within-capture cost attribution only, no quality/noise or owner-scene reproduction claim. Complete format2 club is supplementary,81 armatures/1168 rigged rows,0 missing textures, **zero captured probes**. Rigged replay motion does not imply live avatar or geometry hard-sphere ingestion coverage.

- HOUSE manifestSHA256 `e2bbf29c01aeddfc78f6ee3d9e783bb717fd13e8f251debcde3c1008986dfbe4`;848-file tree (relative path,size,SHA256 list) digest `64896eaebe9e47df0da73df91a9adedea1c0fb143770925ea5d84e2ded2eb6f4`.
- HOUSE camera pathSHA256 `08830147ac64d8fc342ac4612164ca91367fa0ef4aa6df4d78695e5466a8643a`;13.25s14keys, two loops, three frozen240-frame still holds and pan.1594 timed frames per accepted run.
- Common isolated settings templateSHA256 `7cc6065d1c5e122d54e3f5688471a723edd446af3eb2a49aaa6608db43d42573`.
- Club manifestSHA256 `3b7822a5597b38eeca709a7540a2669b27dbf1731a204581abe71666025d7566`.
- RX7900XTX Windows driver26.10.1(LLPC), Vulkan1.4.359. Output3840x2066, render1920x1033. Actual SPLIT+FEATURE, GI3 ACTIVE, ReSTIR ACTIVE138lights/106point, adaptive2x4picks/cap25, merged sun full, noFG. Base shadow VIS and GI trace must cover ALL measured frames. Requested projector2 is essential: projector1 selects maps.
- Pinned FSR3.1 Performance (Upscaler4,Fsr4=0), RT1,GI3,RTReflections1,SunShadows2,ProjectorShadows2,DeferredSplit2,DeferReflectHalfRes1,FrameGen0,VSync0,FPSLimit0,ReSTIR adaptive2/cap25,reach1,latency1,merge1,FarClip512,AntiLag0,NoiseStats0. Absent historical diagnostic/FSR4 settings are explicitly omitted; their semantic off state is recorded separately. Wave32/parking evolved between endpoints; endpoint delta is not an isolated code-only claim for those changes.

## Accepted distributions

| Run/source | GPU mean ms | p50 | p95 | p99 | Frames |
|---|---:|---:|---:|---:|---:|
|historical_a2|22.49250|23.46992|26.53396|28.80684|1594|
|historical_a3|22.47254|23.46736|26.53536|29.25428|1594|
|before1604_a2|19.14130|19.70044|21.90048|25.10064|1594|
|before1604_a3|19.11638|19.68740|21.90992|25.09824|1594|
|current_a1|19.11315|19.67900|21.86376|25.78328|1594|
|current_a2|19.13331|19.71812|21.93860|26.01204|1594|
|current_a3|19.15004|19.68280|21.92320|26.25184|1594|
|current_fsr4|22.95018|23.54992|25.74688|29.46452|1594|
|current_adaptive0|18.10120|18.69532|20.64616|25.36468|1594|

Current A/A median19.1333145ms, range0.0368969ms; historical median22.4825170ms, range0.0199584ms. Current is3.3492025ms (~14.9%) faster. Before#1604 A/A means19.1412992/19.1163750ms, already near current. Exact HOUSE pair is complete; see final table below. No commit is presently established as an offender explaining the owner observation.

Adaptive-off saved~1.03ms here, far below the owner's reported~5.56ms; attribution only, no default recommendation. Current FSR4 diagnostic22.95018ms versus FSR3.1 baseline19.13ms, with actual FSR4.1.2 runtime, measures a different AMD reconstruction network. It is **not** an explanation for a DLSS user.

CPU-frame means~22.21ms on baseline repeats include still-capture/readback boundary stalls (max~885–891ms); full CPU and GPU distributions retained privately.1/GPU mean is not claimed as actual viewer FPS.

## Source attribution and rejected evidence

- Historical exact `3190a88b51c0763836ba05d4adeb2b440a850ccc`; v1.5.3 is a different commit `693e7b80a8334ece26e3a2697ac93a582dfb40ea`, not tested here. Historical packageSHA256 `41fcfb2d8c904efcc50a4f55c9cef0523f11f89574c8f284531dbaad7c49347a`, executable `83f78a6ccae0326104e473505fb41e18b0c29ba522344cc74e1a80eac4f9c4a3`.
- Current rendererae1f063eb7 packaged at docs-only098aa76cf5; ZIP `73ed995874d0792ddc9a413b7380c5b53db1350e12bd5fb0e8039ebe3cd7759a`, executable `7b12a769935f257d53a8a586ffa042e4b6feea7e78938a7cf040dee007d31058`.
- Exact #1604 parent `74c5dd393ea089d10654892e452a21c24b984823`, merge `9a426c8032e6fc24257be0b1811b7e2d7b0b1746`. CPU bounded sphere merging and lagged combine hard-sphere rejection changed. No new standalone pass does NOT mean zero cost: loops live inside existing shaders, and rejection can alter reuse/work.
- Historical3190 already has adaptive sampling, nearest-first cap25, reach maps, merged shadows and mover routing. Earlier sources:421222cb3e adaptive,289aef003b multiray exact,ca2e2dbf06 cap,5238aaaa63 reach,3ec26787db mover-exact,17f8c1a3cb mover-mask default0. These are source identities, not measured offending commits.
- RT tuning merge2c9514f131 / wave32 / register parking is a plausible source of VIS improvement. Historical->current median VIS5.0369->2.8771ms, sun1.2635->0.6725, extraVIS1.3518->0.8779. Queue interactions prevent assigning the entire endpoint change to those commits without separate isolation. No expanded bisect now.
- FSR4 default change55499bb67d, merge#1605/54eb91c942, is kept separate from implementation attribution and from owner DLSS reasoning.
- Rejected wrong-path directory `house-current-01`: projector1, GI but no ReSTIR/pass15. No stochastic cost evidence.
- Rejected historical_a1: bench said ready6.664s before full-quality lighting installed30s later; GI coverage1157/1594, baseVIS1155/1594. Before#1604_a1 and after#1604_a1 show the same late-upgrade defect and are excluded. Receipts retained privately. ASTRA59aea7da31 adds a benchmark-only full-quality-upgrade gate; runtime validation queued,106 source checks and Windows build passed.

## Activity / coverage

HOUSE before#1604 last accepted interval:2380soft spheres over236frames, headers_with_spheres236/1270, largestsoft148.4m, maximum hard radius0m (exact hard-sphere count not exposed by those logs); no cache restart/global reset/queue overflow in that interval. Dynamic club before#1604: no emitted spheres or nonzero hard/soft radii across logged intervals despite rigged animation. `SLBenchReplay::applyMoverRecord` directly upserts rows; it does not reproduce every live-ingest gVKChangedSpheres producer. Therefore neither scene absolves or blames the hard-change path. Source semantics and synthetic equivalence tests remain distinct from runtime coverage.

LVC activity statistics and six substage timestamps are queued separately from clean A/A. OcclusionCull3 adds counters AND some diagnostic rays; its cost is not pure counter overhead. Normal clean mode is2. No LVC rebuild/invalidation count has yet been accepted from this campaign.

## Reproduction and ownership

Reviewable runner/analyzer: `scripts/bench/astra/run_windows.ps1`, `regression_summary.py`; branch architecture report `doc/perf/astra/architecture.md`. Private root `/tmp/astra-performance-20261118` contains exact matrices, source/executable/template/capture/path receipts, raw reports, logs and sanitized summaries. No raw names/chat/local IDs belong in commits.

Example acceptance command (local CPU only):
```
python3 scripts/bench/astra/regression_summary.py /tmp/astra-performance-20261118/house-current-02 current_a1 current_a2 current_a3
```
Owned desktop launch (after atomic reservation, no viewer/task, package hash verification and template backup):
```
& C:\sltest\astra\run_windows.ps1 -Root C:\sltest\astra -List C:\sltest\astra\house-current-runs.json -Out C:\sltest\astra\out\house-current-02
```
Builds only own worktree, serialized after any ninja,12 jobs,nice19,TMPDIR=/home/bryan/.cache/slipstream-build-tmp. `make windows` passed106 source checks at59aea7da31; Linux TESTCTL package building. Hosts remain actively reserved, restoration pending; no Windows security/system changes, owner settings untouched. Garbage no GPU launch yet; settings/dialogs/symlink and helper run.log backed up, isolated settings prepared.

## Owner DLSS Ultra Performance path and architectural priorities

Owner clarification supersedes any earlier FSR interpretation: the owner uses DLSS Ultra Performance. AMD FSR differences are an option-cost diagnostic only, **not an explanation of the owner's regression**. Historical attribution is now secondary; finish the running #1604 pair and stop expanding the bisect.

**Source proof at 59aea7da31 (renderer ae1f063eb7 plus diagnostics):** `recordWorld` gets exact render extents from `rrPlan`, rather than assuming a fixed pixel ratio. Ultra Performance has much lower internal resolution than output; log the network's actual dimensions before comparing. NVIDIA's integration contract also requires querying optimal settings; RR replaces SR evaluation, rather than adding a second upscale ([DLSS RR programming guide](https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/docs/ProgrammingGuideDLSS_RR.md)). Neither authorized GPU validates DLSS/RR.

Default raw-signal mask26 contains stochastic visibility, GI and merged sun. However, `recordWorld` clears raw GI while upscaling unless bit64 is explicitly enabled; `rrRawSignal` refuses raw stochastic radiance mode; reflection bit4 is off. Thus the owner's UP+RR path can retain GI temporal/spatial filters, stochastic radiance integration and sparse-reflection temporal resolve. SR retains all local denoisers. These are deliberate noise protections, not proof that every history is redundant. Full-render-resolution GI/shadow overrides are separately controlled by RenderVKDlssFullResSignalsMode; establish effective policy before blaming fourfold pixel work. Owner's exact RR selection and overrides remain unobserved.

**Measured AMD baseline, 3840x2066 output / 1920x1033 render, FSR3.1 Performance, no FG:** total GPU critical path median run mean19.1333ms; three-run mean spread0.0369ms. Selected current_a2 intervals below are NOT additive (async overlap and waits):

| Interval | Mean ms | Architectural implication / next evidence |
|---|---:|---|
| deferred async lighting chain |8.2184|Primary dependency before combine; split its producers before changing filters.|
| graphics join GI/lighting |8.2553|Wait for lighting chain, not standalone GI cost; do not add to chain.|
| base shadow VIS/pass15 |2.8771|Largest measured lighting subpass; preserve ray semantics while examining duplicate receiver/light tests.|
| extra adaptive VIS |0.8764|Cap25 extra samples; feature-off saved~1.03ms total here, not owner~5.56ms.|
| deferred base |1.8683|Internal-resolution shading plus queue effects; scale diagnostic completed below.|
| LVC reach map block |0.5209|Five fixed dispatches, including capacity-wide DIRTY scan; optional substage measurements below show BUILD dominates.|
| stochastic temporal / spatial |0.4861 /0.5746|Radiance integration remains under RR; fuse only if history validation and occupancy survive.|
| lag GI chain |4.4049|4.3886ms classified hidden; removing it need not save4.4ms critical path.|
| volumetric RT lighting |1.2683|Froxel/light work; distinguish view/ray scale from output scale.|
| avatar-depth copy |0.7301|Recorded in zero-avatar HOUSE; conservative empty-set bypass candidate. Interval includes dependencies, not guaranteed savings.|
| medium apply |0.8662|Check resolution and duplicate composition before reducing signal quality.|
| FSR3.1 resolve |1.2743|AMD output-space reconstruction; no inference about DLSS RR runtime.|

**Hypotheses for 5090, not measured there:** low internal resolution reduces pixel-proportional work but does not remove fixed light/cache scans, instance/TLAS work, synchronization, output-resolution neural reconstruction or post-processing. A smaller internal image may expose these fixed costs. The completed 1/3-scale AMD diagnostic tests scaling of these intervals only; it is not a DLSS emulation or quality comparison.

Next structures to test: (1) bypass avatar depth allocation/copy/prepass only when an exact conservative live-face classification proves no consumer exists, retaining all work on uncertain/nonempty sets; (2) use LVC substage/counter evidence to decide whether bounded dirty-instance processing plus explicit expiry queues can replace fixed-capacity signature scans; (3) preserve local denoiser validation while examining duplicated guide/history reads and handoff barriers. No desktop default or ray-budget reduction is proposed from the present evidence.

## Optional prototypes and semantic evidence (95f820bbf1)

1. GI hard-first header publication: all spheres preserved, hard count lets hard-only cache lookups skip soft emitter spheres. CPU merge queue/order unchanged; all-sphere soft-strength and segment tests retained, global overflow still conservative.133120 CPU cases execute extracted shader predicates against the actual proposed host publisher: empty/all-soft/all-hard/mixed, signed-zero radii, boundaries, padding and overflow. This is semantic test evidence, not a GPU speed or image result.
2. Avatar-depth empty-set bypass: existing incremental class bookkeeping counts a conservative avatar face set; skip only when current mirror AND last successful GPU publication are empty, no pending full upload. Unknown/failed/nonempty states keep original path. No shader/ray-budget change. Applied default-off; HOUSE timing results below, complete-scene image controls in progress.

Both options default OFF. Original bounded receiver-support search remains preserved/paused. No desktop-default change, image-quality claim or noise-for-FPS trade is made.

## Exact #1604 HOUSE pair complete — first measured attribution milestone

Matched capture/path/template/settings/output/render identities and actual GI/ReSTIR/pass15 on all1594frames of every accepted run. Before mean of run means19.1288371ms; after19.1168640ms; delta−0.0119731ms. Before A/A range0.0249242ms, after0.0325868ms. No detectable regression from this pair within repeat variation. This is **qualified soft-emitter replay evidence**, not a hard-change/NVIDIA absolution. Firsthot A1 on each side rejected: only1229/1594frames had GI/baseVIS.

| Accepted run | GPU mean | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
|before1604_a2|19.14130|19.70044|21.90048|25.10064|
|before1604_a3|19.11638|19.68740|21.90992|25.09824|
|after1604_a2|19.13316|19.70360|21.90820|25.26160|
|after1604_a3|19.10057|19.69348|21.86284|25.19984|

| GPU interval (nonadditive), ms | Before | After | Delta |
|---|---:|---:|---:|
|c_lvc@async|0.520631|0.520090|-0.000541|
|c_gi_walk@async|1.056245|1.056852|+0.000607|
|c_gi_trace@async|1.024584|1.022795|-0.001789|
|c_defer_shadowvis@async|2.860567|2.860783|+0.000216|
|c_defer_pstochxvis@async|0.875843|0.873857|-0.001986|
|q_gi_lag@async|4.393490|4.391858|-0.001632|
|q_defer_light@async|8.191622|8.187716|-0.003906|

After A2 last interval:2760 emitted soft spheres across283frames; headers_with_spheres282/1325, largestsoft148.4m, maximum hard radius0m, no cache/global reset or queue overflow. Exact hard-header count is not exposed by these revisions; new diagnostic counters are prepared. Club control still running. Current optimization order: measured lighting-chain dependencies; LVC substages and dirty/build incidence; optional hard-only GI lookup specialization preserving soft updates; exact empty-avatar work elimination. No further historical bisect.

#1604 dynamic club control completed: one accepted run per revision, matched settings/capture/path/render/output; before34.7314671ms (p5040.89297,p9548.25996,p9951.43644), after34.7257898ms (p5040.96700,p9548.31604,p9951.44508). Difference−0.0056773ms is not an established effect: no within-revision A/A for this supplementary pair. All1594frames have GI/baseVIS. No emitted spheres or nonzero hard/soft radii in logged intervals despite rigged replay motion; hard-ingest cost remains unexercised. Historical campaign is now closed; architecture diagnostics59aea7da31 launched after DONE/no viewer/task/owned-lock checks, ZIPSHA256d17ede1b54b5896e7322060d2a91c94e792cf663edd253a8db0d0a42e0625d97.

Effective AMD signal extents are smaller again than internal render: current_a2 logs stochastic render1920x1033 / denoiser960x517, radiance1, ReSTIR1, samples4, local projector+sun owners; GI960x517 at scale0.5,262144 cache slots. Under owner UP+RR, below-render signals stay locally denoised unless raw bit32 is enabled, even when a signal's individual raw bit is set. A full-render signal override can change that routing and quadruple a half-resolution signal's pixel count. Owner's actual RR/full-res/raw-mask policy is unobserved. These dimensions/ownership markers are being retained alongside generic path markers in the analyzer.

Readiness runtime validation: instrument_a1 waited35.8412s for full-quality lighting, then passed all1594-frame GI/baseVIS coverage gates. GPUmean19.2796619ms,p5019.88772,p9522.08652,p9925.74904. This first-hot run is~0.146ms above prior current median; retain it, do not declare timestamp overhead or a renderer regression before the remaining clean repeat. Full-quality readiness and settled GI/cache history are different gates.

## LVC substage evidence — first instrumented run

Source59aea7da31, same HOUSE identities/settings except RenderVKPerfLvcStages1. All1594frames pass GI/baseVIS coverage. GPUmean19.2230837ms,p5019.80608,p9521.98728,p9925.66376. Clean A2 pending, so no total-frame timestamp-overhead estimate yet. Seven LVC intervals sum0.54243ms versus prior unsplit~0.521ms; that local difference includes timestamp/barrier effects and is not automatically a critical-path cost.

| LVC interval | Mean ms | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
|c_lvc@async|0.001995|0.001960|0.002120|0.003960|
|c_lvc_assign@async|0.024562|0.024040|0.025280|0.048880|
|c_lvc_build@async|0.406636|0.201360|1.648160|1.849920|
|c_lvc_dirty@async|0.077218|0.066120|0.101920|0.105280|
|c_lvc_finalize@async|0.007134|0.008560|0.011600|0.012440|
|c_lvc_plan@async|0.020244|0.013000|0.014880|0.285520|
|c_lvc_setup@async|0.004639|0.005800|0.006360|0.009360|

Map BUILD is~75% of the instrumented LVC sum; DIRTY is~14%. A perfect dirty-scan elimination has only~0.077ms local ceiling here, before dependencies. Prioritize measured rebuild causes and conservative reuse over a large dirty-queue rewrite. Do not reduce eight-ray-per-texel map quality or lengthen the motion hold by default: prior campaign already found a longer hold harmed the noise benefit. Investigate signatures, eligibility churn, conservative invalidation and safe work compaction after the separate activity run.

Source correction: authoritative clean_regir.glsl defines LVC_SPP8,6144texels/map, at most2maps/frame (98304 build rays at the cap). recordLvc’s old comment still says4; use the shader constant and measured ray counters, not that stale comment. No sample reduction is proposed.

Clean diagnostic repeat A2:19.1947462ms,p5019.75448,p9521.97800,p9925.53660. A1/A2 range0.0849158ms. Stage-on19.2230837ms lies inside that range; total timestamp overhead is unresolved at this precision, not zero. LVC sum0.54243 versus clean intervals0.51176/0.52436ms shows~0.02437ms local instrumentation increase relative to their mean, which need not equal critical-path change. Stats mode19.7802432ms is+0.55716ms versus stage-only and+0.5430391ms versus clean mean; it includes diagnostic work/rays, not just counter atomics.

LVC stats, separate activity evidence: in a79-recording window,106slots,105usable,1dirty,0builds/rays/static changes; in a later169-recording window,106slots,85.9usable,20.1dirty,1.11builds/frame and54678rays/frame,23.7static instance changes/frame (6.0transform/bounds,0flags,17.7LOD-group/geometry). Both reported0 reach-map invariant violations. This supports motion/geometry-driven rebuild work; it does not prove the geometry changes were redundant. A correctness-preserving next structure is conservative angular dirty masks: open affected texels/halo immediately, retain provably unaffected texels, then repair with the same8samples; moving lights or uncertain bounds invalidate the whole map. Requires separate implementation and image/invariant validation, not a default or a claimed saving.

## Completed diagnostic campaign and prototype validation

Source95f820bbf1 passed all108 source checks, including133120 GI prefix equivalence cases and131072 avatar classification/guard cases. Both options remain OFF. Windows build/deployment and serialized Linux package are in progress. Garbage functional smoke exited cleanly; no available Vulkan validation layer, so this is not validation-layer coverage. Complete street/club non-RT offline baselines started with actual SPLIT + FEATURE SPLIT after the readiness gate. Linux output3840x2062 differs from Windows3840x2066: within-host comparisons only.

AMD source59 diagnostics finished. Single lower-render-resolution diagnostic: HOUSE output3840x2066 unchanged; render1280x689, GI/stochastic640x345, noFG, same source/capture/path/seed, all1594 GI/baseVIS frames. GPUmean12.33404ms (p5012.37968,p9514.20628,p9919.03436), versus instrumented1920x1033 mean19.22308ms. This is AMD FSR3.1 scaling evidence, NOT DLSS/RR or5090 evidence, and not an image-quality comparison. BaseVIS2.88846→1.27672ms, GItrace1.02834→0.57344, LVCbuild0.40664→0.39017, volRT1.27720→1.24140, reflectiontrace0.69760→0.69710. Resolution-independent or weakly scaling work becomes relatively larger at low internal resolution.

Complete club diagnostic (zero captured probes) GPUmean34.39603ms, p5040.27032,p9548.54900,p9951.45860; deferred async chain13.0452ms, static alpha/post5.9688, reflection trace4.0248, prepass2.0982, BLASrefit1.4386, baseVIS2.3020, extraVIS1.2281. Overlapping intervals must not be added. LVCbuild only0.01295ms: HOUSE rebuild optimization is not this scene's main opportunity. Mode3 stats raises club total to36.66782ms and changes ray work; exclude it from clean performance claims. Its logged windows show~97.5–98.9% of mover-flagged samples are rigged, ~4.45–4.51 pass0 VIS jobs/texel, ~0.10–0.11 LVC builds/frame, and zero observed blocked-map reached violations. These are logging windows, not exact timed-frame rates.

## Architecture priorities supported by these measurements

**Measured AMD:** At1280x689 the pixel count is44.5% of1920x1033, and the GI/stochastic grids change from960x517 to640x345. The fixed samples-per-texel policy is retained, but total candidate/ray work is not held constant across this resolution diagnostic. It therefore measures scaling, not an equal-ray-budget quality comparison. LVC and volumetric grids do not follow the render resolution. `llcleanvolfogvk.cpp::kTiers` explicitly uses fixed128x72x48 /160x90x64 /192x108x96 froxel tiers.

| Measured work | Cheaper structure to investigate | Correctness constraint / evidence still needed |
|---|---|---|
| HOUSE hard-only GI sphere queries, cost not yet isolated | Publish an exact hard prefix/count; avoid visiting soft spheres in hard-only predicates | Implemented as optional95f820bbf1; preserve every soft sphere, overflow, signed-zero hard radius and CPU queue order. GPU A/B pending. |
| Empty-avatar depth interval~0.73ms at1920x1033 | Prove the drawable set empty using existing incremental classification, then omit copy/prepass | Implemented optional95; uncertain GPU publication keeps old path. Timer includes dependencies, so0.73ms is not a promised saving. |
| HOUSE reach-map build~0.407ms, p951.648ms | Conservative angular dirty masks; immediately open affected texels plus filter halo and repair only those with the existing8 samples | Proposal only. Light motion/unknown bounds must invalidate whole map; prove unchanged texels cannot lose a blocker. No longer history or fewer samples. |
| HOUSE volume RT~1.28ms largely independent of render size | Compact only froxels provably devoid of participating media/light contribution before visibility tracing; share deterministic light/material preparation | Proposal only. Density bounds must include local volumes, particles, water/height terms and temporal transitions; preserve stochastic sample sequence and transport. Skipping merely low density changes the image. |
| Club reflection trace~4.02ms and alpha/post static interval~5.97ms | Separate traversal/material/lighting work attribution; reuse admitted light preparation, compact exact empty work and remove repeated material fetches before considering history changes | Proposal only. Trace/forward may overlap; alpha draw interval can include dependencies. Preserve alpha ordering, reflection ray population, roughness and forward-layer ownership. |
| Club rigged BLASrefit~1.44ms | Geometry-generation keyed unchanged-pose refit suppression; distinguish rigid placement from deformation and consume the same published vertices | Proposal only. Recorded poses move here; no claim that these refits are redundant. Failed uploads, skeleton changes and LOD changes must force rebuild/refit. |

**NVIDIA applicability hypotheses:** DLSS Ultra Performance can make fixed scene, froxel, BVH and output-resolution work a larger share of a frame, but network-selected dimensions and RR guide/denoiser routing must be observed on an authorized NVIDIA RT host. Neither available box can do that. A local filter retained by the raw-mask or radiance policy is not automatically redundant with RR. No defaults, image quality, or5090 milliseconds are inferred from this AMD table.

### Pass-count audit: merged sun and adaptive shadows

**Source proof:** `clean_deferred_light.comp` SH_STAGE_SELECT/SUNVIS/VIS/SHADE and `recordProjStoch` already split candidate selection, independent sun/local visibility, and shading. SUNVIS writes the sun channel; VIS/SHADE are compiled without sun. A nonzero `c_defer_shadowsun` beside `c_defer_shadowvis` therefore does not prove duplicate sun tracing. Re-fusing the original high-register kernel is not a correctness-preserving performance win until measured; the source explicitly uses64-byte receiver descriptors because literal per-ray records would be much larger.

Adaptive extras already dispatch indirectly over a bounded8x8 tile list. All A extra passes revisit the same admitted tiles with the same estimator fold; a25% tile cap with two extras can produce roughly50% of full-frame texels when the stats sum both extra passes. Club's~244k extra texels versus~496k base texels is consistent with that budget, not proof the cap failed. Mask/list maintenance is not an unnecessary full-resolution trace. The remaining cost is the admitted samples' actual visibility walks and repeated selection/shading; any cheaper schedule must preserve the same admitted set, per-pass sample sequence, fold weights and history countdown. Neither disabling extras nor extending history is proposed as a default.

## First optional prototype result: GI hard prefix — no established gain

Source95f820bbf1, same HOUSE/camera/template/1920x1033 render/3840x2066 output/noFG and all1594-frame GI3/ReSTIR/pass15 coverage. Options0 baseline means19.1765567/19.1587570ms (mean19.1676568, A/A range0.0177997). Hard-prefix1 mean19.1622161ms, p5019.72316,p9522.02684,p9925.62960: delta−0.0054407ms, inside observed repeat spread. No measurable critical-path win is established. One B run so far; final A3 remains pending.

Lag-GI queue4.433952→4.420100ms; hidden portion4.418069→4.403965ms; reuse0.805725→0.801080ms, trace1.035570→1.034706ms. Deferred lighting8.236077→8.239030ms and graphics join8.277695→8.283145ms do not improve. These tiny local shifts are not additive savings or a proven speedup.

The optional path did have relevant source/replay activity: last logged windows published3161 and2543 soft spheres, zero hard spheres, no global reset/cache restart/overflow. Counts cover logging windows, not exclusively timed frames. This is a valid negative/inconclusive optimization result for a soft-emitter replay, not proof that hard-region publication or NVIDIA behavior costs nothing. No HOUSE image-quality claim. Keep the option experimental/off; do not promote it as a performance fix.

## Completed HOUSE prototype timing block

Same95f820bbf1 binary, fixed identities and all1594-frame GI/baseVIS coverage. Baseline A1/A2/A3 means19.176557/19.158757/19.163758ms: mean19.166357, median19.163758, range0.017800. Avatar-empty B1/B2 means19.113573/19.116748ms: mean19.115161, range0.003175. Observed mean reduction0.051196ms (0.267%); the0.73ms vanished copy mark was predominantly reassigned dependency time. This is a small mean effect, not a clear whole-distribution win: p95 is essentially unchanged and B p99 is higher. No default promotion; complete-scene images and Pascal controls are outstanding.

| Run | mean ms | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
|A1|19.176557|19.787600|21.924360|25.424120|
|A2|19.158757|19.753441|21.944799|25.546640|
|A3|19.163758|19.710440|21.929720|25.679079|
|empty B1|19.113573|19.695320|21.948320|26.068560|
|empty B2|19.116748|19.691401|21.949520|25.824320|

GI-prefix B19.162216ms is0.004141ms below the completed baseline mean, still inside A/A spread. Both options stay experimental. CPU semantic preservation and a removed dispatch are not substitutes for a measurable critical-path benefit and visual controls.

### Queue and NVIDIA coverage boundary

`VkDeviceContext::asyncAutoDecide` selects async by actual device/driver capability; Pascal normally receives no async queue, whereas supported Turing-or-newer NVIDIA devices can receive one. `asyncDeferLightingWanted` additionally requires a dedicated compute family in auto mode. These are source policies, not proof of the owner's effective settings or overlap. AMD's measured hidden-GI fraction cannot be copied onto a5090 frame budget. A reproducible NVIDIA follow-up needs an authorized RT-capable NVIDIA host, a compatible complete HOUSE capture, actual DLSS network/optimal render extent, RR/raw-signal/full-resolution policies, queue routing and noFG. Neither currently authorized box supplies that coverage; the owner's local GPU remains out of scope.

A standalone figure of run means, LVC substage distributions and the avatar-copy attribution shift is available in the branch at `doc/perf/astra/figures/amd-attribution.png` (SVG alongside). Reproduce with `scripts/bench/astra/plot_results.py`; ranges are observed repeat ranges, not confidence intervals.

## Owner-directed dense-light focus

3060 uses DLSS Performance; native TAA→DLAA migration is explicitly withdrawn for that report. Exact tag mapping and other conditional leads: `agents/worktrees/astra-perf-architecture/doc/perf/astra/jetstream-3060.md`. #1604 is after both Jetstream tags, so cannot explain their exact comparison.

Dense occluded point-light architecture is now central. Source and current AMD substage evidence, conservative rejection limits, and ordered follow-up: `agents/worktrees/astra-perf-architecture/doc/perf/astra/dense-local-lights.md`. In a settled HOUSE stats window,105/106 maps are usable and~3million candidates are skipped/frame, yet~75% of traced pass0 jobs remain statically blocked beyond the near-surface category. This does not identify which admission limitation dominates; new bounded opt-in counters distinguish those reasons. Their production VIS shaders are byte-identical to baseline, four shader variants validate, and108 source checks pass. Synthetic128-light closed/open-wall controls are prepared; no measured dense-fixture result yet. No NVIDIA timing or incomplete-HOUSE quality claim.

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

## Dense hidden point-light milestone (2026-11-18, AMD)

The default-on reach maps do not make dense hidden lights free. In a sealed128-light fixture they save0.545ms total, but the deferred local-light interval still costs~6.2ms. The source reveals redundant point-light iteration after pass15 already owns those contributions. Optional prototype57f164d27d skips that exact-zero subset early; A/B runs are in progress, so no saving is claimed for it yet.

|Same-capture clean comparison|Map1 A means, ms|A/A range|Map0 mean|Map1 minus Map0|
|---|---:|---:|---:|---:|
|HOUSE (truncated; timing only)|19.138478 /19.131995|0.006483|18.767805|+0.367432|
|Sealed128|13.759295 /13.759240|0.000055|14.304438|−0.545171|
|Visible128|15.231468 /15.222170|0.009298|15.043361|+0.183458|

HOUSE uses95f820bbf1; synthetic controls use5d60acdf6a (production VIS byte-identical). Each row compares its own identical binary/capture/path/settings except reach maps. No cross-row claim of identical scene or ray budget. NoFG; render1920×1033/output3840×2066, actual all-frame GI3/ReSTIR/pass15. Synthetic paths have362timed frames; HOUSE1594. Full percentile tables and identities are in branch `doc/perf/astra/dense-local-lights.md` and the sanitized measurement ledger.

|GPU interval, sealed128|Map1 A mean ms|Map0 ms|
|---|---:|---:|
|Base SELECT|0.268701|0.325056|
|Base VIS|0.077015|0.572004|
|Adaptive VIS|0.006626|0.006697|
|LVC maintenance|0.067276|absent|
|ReGIR|0.053019|0.051382|
|Deferred local-light interval|6.195016|6.189131|

Intervals overlap and are not additive removable costs. Clean map0 has one repeat. Images are pending transfer after final campaigns; no default or image-quality decision follows from these timings.

The separate mode3/verify diagnostics show128usable maps,zero rebuilds after discovery and~94%map-blocked pass0 jobs in the sealed fixture, with no reached-light invariant violations observed. Visible control haszero map-blocked jobs. HOUSE's settled window instead leaves333282open-map and164565depth-margin jobs/frame, almost all statically blocked, despite105/106usable maps. These are rounded logging windows, not exact timed aggregates or candidate-admission rates. A moving-window counter subset exceeds its all-count slightly; do not infer exact probabilities from that row. Verify changes skip policy and diagnostics, so its timing is not pure counter overhead.

Ordered structural actions:

1. Measure the already-running exact-zero deferred-point mask; preserve point/projector ownership, forward/indirect populations, VR and inactive-path fallbacks.
2. Split open-map reasons into seam, footprint overflow and raw-open causes. Then test bounded conservative angular maximum queries across cube faces, retaining footprint, depth margins and unknown=open. This may avoid artificial open results without more rays, but hierarchy build/synchronization and coverage correctness need measurement; it is not implemented here.
3. Preserve receiver-specific visibility evidence and current light/BVH revisions; do not globally suppress hidden lights that may illuminate indirect/reflection hits. ReGIR is earlier than LVC maintenance, so stale-map cell rejection is unsafe without a revised invalidation dependency. Small samples or eight cell corners do not prove whole-cell occlusion.
4. Reduce dirty/rebuild work only after those higher-cost paths. HOUSE measured dirty stage~0.077ms; whole-map discovery dominates maintenance when it runs. Regional invalidation must reopen affected angular regions immediately and fall back to whole-map invalidation on unknown changes.

These are RX7900XTX measurements. DLSS Ultra Performance/RR and the RTX3060 DLSS Performance report remain unmeasured NVIDIA cases. Native-TAA-to-DLAA migration is explicitly withdrawn for both reported nonzero-upscaler paths. No noise-for-FPS default change, local GPU run, push or merge.

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

## Final outcome and restoration

Completed57f residual-point-mask AMD means (off→on): HOUSE19.176448→18.755632ms; club33.529853→32.573343; closed12813.517652→7.780543; visible12814.932036→9.146615, including the bounded visible repeat. Deferred consumer work is removed; visibility ray intervals stay essentially unchanged. These are AMD results, not RTX5090/3060 attribution, and the mask does not solve sampling noise.

Complete-scene means/crops show no obvious new missing-light/leak region, but images are not bit-identical and some A/B errors exceed A/A. Visible temporal-variation repeats do not show a consistent penalty; no universal equivalence/default claim. Pascal forced-NRT mask control off179.148712→on179.718027ms is within2.861157msA/A spread; unsupported RT/RR paths are not validated.

Both hosts restored with verified owner data/original symlink/job files and matching locks released. No pushes/merges or local-owner GPU use. All81accepted nonwarm receipts,12image-control cases, full distributions, source identities, reviewed synthetic figures and restoration receipts are in [final results](results.md), [mask results](mask-results.md), and [images](images.md). All options remain default-off.
