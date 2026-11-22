# ASTRA architecture assessment — 2026-11-18

Source baseline: `ae1f063eb7ca9c2c0e0bbec3921d2c21821e493a`, freshly fetched `forgejo/main`. Branch/worktree: `astra-perf-architecture`, `agents/worktrees/astra-perf-architecture`. Assigned model gpt-6-astra; assignment acknowledged, runtime identity not independently exposed. This is the assessment-first deliverable. Measurements below are explicitly historical; new experiments belong in `results.md`.

## Recommendation

Keep the GPU-driven deferred architecture, split lighting, bounded world caches, effect-local denoisers and independent upscaler adapters. The evidence does **not** justify replacing this renderer with forward lighting, removing denoisers, or lowering sample counts by default. Restructure the contracts between these systems before adding another sampling or history heuristic. In particular, a reconstruction with no valid receiver sample must distinguish **unavailable** from **valid black**, and a temporal record must name its receiver, signal, light population and generation.

The largest architectural risk is not the number of passes alone. It is that coarse reconstruction, light/caster partitioning, cache reuse and final temporal reconstruction each make different validity decisions. Increasing history hides variance while extending response time; increasing rays cannot recover a receiver that no coarse sample represents. An occupancy improvement is useful only when the frame's critical path improves; moving work between timer marks proves neither a win nor an image match.

Evidence labels used below:

- **S**: exact-base source inspection; path and symbol/line anchors supplied. Comments describe intent, not proof of execution.
- **H**: historical measured report by earlier workers; not a new ASTRA run, and not automatically valid at this base.
- **P**: hypothesis/recommendation requiring measurements. No predicted milliseconds are presented as results.

Paths below are relative to the isolated worktree. `V` = `indra/newview/vkgfx/`; `S/` = `indra/vkclean/shaders/`.

## 1. Deferred, split and uber lighting

**S:** `S/clean_deferred_light.comp:39–139` describes material G-buffer shading, tiled/froxel light populations, SHADOW/REFLECT/COMBINE and the half-resolution reflection representation. Host `V/llcleanrenderervk.cpp::deferSplitFrame` (57975) actually selects the split only when pipelines, targets and stereo feature support are ready. Allocation changes are deferred to a safe recording boundary. The uber path remains a fallback and a feature-specific stereo route; it is not dead code. Feature splitting further separates BASE/LOCAL via classification and an indirect tile list.

**Noise/smear:** Split intermediates quantize visibility and radiance; half-resolution irradiance and soft specular cannot represent every thin surface. `dsHalfUpsample` (3833) computes depth-plane and normal weights, but when all weights fail it returns the best-scoring candidate **without a validity threshold** (`ifb`/`sfb`). That is source-proven cross-surface borrowing potential, not proof that it causes every reported smear. Temporal jitter can alternate the representative.

**Cost:** G-buffer traffic, extra intermediates/classification, probe/SSR walks, local-light loops and register occupancy. A larger pass is not inherently cheaper. Historical Pascal comments are not current measurements.

**Keep/restructure:** Keep deferred and data-based splitting; make the receiver and material footprint explicit in reconstruction. First measure full versus half reflection as an existing quality option, then prototype a sparse full-resolution repair for invalid coarse receivers. Do not fix missing samples by returning black. Keep the fallback observable and validate it separately. Consolidate duplicate feature contracts only after parity tests. **P priority 1.**

## 2. RT shadows, ReSTIR DI and pass 15

**S:** `S/clean_deferred_light.comp` pass 15, `V/llcleanprojstochvk.cpp`, `S/clean_pdn_common.glsl`, `S/clean_rt_shadow_mask.comp:1617` and `S/clean_lights_common.glsl` implement stochastic local lighting, keyed dominant lights, static/moving caster policies and the residual mover fold. The residual ratio is weighted by the static-light contribution, not merely an unweighted average visibility. `RenderVKNoiseStats` probes raw and final DI (`llcleanprojstochvk.cpp:1521`).

**Noise/smear:** Sparse visibility of small emitters, gobos and contacts; light-set churn; correlated ReSTIR candidates; half-resolution receiver mismatch. A ratio can be useful, but aggregated visibility loses per-light association and cannot generally reconstruct differently coloured diffuse/specular terms. ReSTIR's reservoir count is not an independent sample count. Light motion is not receiver motion; caster shadows move across static receivers.

**Cost:** Candidate selection, alpha-tested BVH traversal, adaptive/exact-light passes, mover visibility and denoising. Earlier AMD measurements located substantial work on async compute, including the join before GI/lighting. **H:** wave32 and parking changes reduced AMD cost without a measured noise regression; already incorporated work must not be claimed again as a new prototype.

**Keep/restructure:** Preserve keyed dominant lights and re-evaluation of visibility at the receiving surface. Define one admitted light set and residual estimator shared by producer, mover mask and consumer; carry revision and estimator units. Compare radiance versus correlated numerator/denominator reconstruction at matched ray and memory budgets before choosing. The clean-room v2.1 bias contract is a useful design input, not measured superiority. Keep static and moving components distinct where their temporal validity differs. **P priority 2.**

Comparison: the [original ReSTIR paper](https://research.nvidia.com/labs/rtr/publication/bitterli2020spatiotemporal/) provides a resampling estimator, not permission to substitute a generic visibility ratio for every lobe. Its performance is not a Slipstream prediction.

## 3. GI and caches

**S:** `V/llcleangivk.cpp::giDecide/recordGI`, `S/clean_gi_common.glsl`, and `S/clean_deferred_light.comp::deferGi*` implement trace, reuse, reshading, cache resolve, temporal and a-trous reconstruction. GI replaces probe diffuse where valid; probes remain fallback and specular/far field. Bounded change spheres and cache storage exist. Actual `giCacheBytes` uses 64 bytes per slot plus bin storage (line 115); the file header still says 48 bytes. Use allocations, not stale header estimates, for VRAM accounting.

**Noise/smear:** Low-resolution receivers, secondary-hit NEE, rare emitters and reuse correlation; invalidation lag in screen history, reservoir history and world cache can compound. `GI_F_LAG` adds explicit previous-frame consumption and disocclusion recovery. A cache cell matching position/normal does not prove identical material, owner or lighting. Nearby opposite wall faces and deforming skin require special provenance.

**Cost:** Trace, hit material evaluation, hit lighting/visibility, reuse and denoising plus large per-texel state. Cache lookup and maintenance are not free; measure their actual hit and invalidation rate. **H:** doubling GI hit-shadow rays barely improved final image noise in the prior house/club campaign.

**Keep/restructure:** Keep bounded caching, localized invalidation and demodulated irradiance. Specify full validity keys, age, source generation, changed-light revisions and fresh/black/unavailable state at each consumer. Separate low-frequency world reuse from screen-space detail; preserve a verified terminal fallback. Prefer targeted invalidation to global history resets. **P priority 2**, after receiver instrumentation.

Production comparison: [Lumen's technical details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine) separate screen traces, scene tracing and surface-cache lighting; coverage and deformed geometry have explicit limitations. [Epic's SIGGRAPH presentation](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf) amortizes world and screen radiance differently. This supports explicit cache domains, not an unmeasured wholesale Lumen port for streamed user content.

## 4. Denoisers

**S:** `S/clean_dn_temporal.comp:3–45` separates diffuse/specular histories, virtual specular reprojection, moments, light-motion caps and keyed-light histories. `V/llcleanrtdenoisevk.cpp` maintains a separate shadow-mask temporal/spatial path; `S/clean_rt_refl_temporal.h` has reflection-specific logarithmic variance and anti-lag. Fog has a third-dimensional history. These signals legitimately need different estimators.

**Noise/smear:** Strict rejection exposes sparse noise; loose rejection or long history ghosts. Neighborhood variance is unreliable when samples are correlated. Repeated spatial filtering loses thin detail. The reflection header explicitly acknowledges rare bright spikes that anti-lag cannot distinguish from real light changes in one frame.

**Cost:** History/geometry reads, moments, neighborhood searches, a-trous passes and multiple full-resolution/half-resolution targets. Quantify DRAM and dispatch costs before fusing; fusion can increase registers and reduce occupancy.

**Keep/restructure:** Keep signal-specific denoising independent of TAA/FSR. Unify geometry validation, reset reasons and instrumentation rather than forcing one filter onto all signals. Separate temporal confidence from material detail; apply demodulation consistently and restore detail once. A/B response time as well as still variance. **P priority 1.** [NVIDIA NRD's integration guidance](https://github.com/NVIDIA-RTX/NRD/blob/master/README.md) is a primary reference for signal/guide and motion contracts; this assessment proposes no third-party code import.

## 5. Temporal history and reprojection

**S:** `S/clean_receiver_motion.glsl` defines current-minus-previous UV, previous depth and skinned class/previous-normal encoding; `clean_draw_rigged.vert:72` sources prior skinned positions and held-pose pelvis motion. Individual effect histories, GI lag and final temporal resolve remain distinct. Reset and fallback tracking are visible in `llcleanrenderervk.cpp:50463–50514` and `llcleandlssrrvk.cpp:669`.

**Noise/smear:** Wrong previous pose, a newly allocated/reused owner, changed roughness or shading, camera-only motion on moving transparency, and mismatch between jittered sampling and unjittered vectors. Long chains of otherwise reasonable filters can have unacceptable combined response time. World-time freezing alone does not establish that dynamic history is valid.

**Cost:** Prior-pose generation, guide bandwidth and repeated matching/search. 

**Keep/restructure:** Define a receiver contract once: coordinate origin, view/layer, current/previous surface, motion validity, normal domain, generation and reset. Add rejection reason and missing-sample counters, with stationary, pan, disocclusion, light-off and held-pose tests. Do not weaken normal/depth tests globally to hide flicker. **P priority 0/1.** [Epic's TSR documentation](https://dev.epicgames.com/documentation/unreal-engine/temporal-super-resolution-in-unreal-engine) similarly exposes reprojection, history rejection and deformation velocity debugging; stability/sharpness choices remain product trade-offs.

## 6. Upscaler hand-off: SR, RR, FSR 3.1/4

**S:** `S/clean_fsr_inputs.comp` converts motion sign, provides camera fallback and water-plane depth, reactive and T&C masks. `V/llcleanfsr3vk.cpp:643` resets when changing FSR paths and falls back after failed FSR4 dispatch. `S/clean_rr_guides.comp:3–47` builds material guides, hit distance, pre-exposed colour, transparent overlay and responsivity; the overlay exists because opaque guides do not describe blended hair/water. `V/llcleandlssrrvk.cpp` separates SR/RR feature policies and coalesces reset reasons.

**Noise/smear:** An upscaler cannot reconstruct an absent receiver or correct a biased lighting estimate. Opaque guides applied to transparency erase material detail; missing specular hit motion smears reflections. Denoising a signal and then asking RR to reconstruct it may add lag, depending on the selected raw/denoised policy. Verify the actual policy, not just a requested menu setting.

**Cost:** Guide and input passes plus upscaling; context retention and simultaneous fallback resources affect VRAM. **H:** RX7900XTX FSR4 Quality cost ~4.78 ms versus ~1.34 ms for FSR3.1, while prior temporal-stability metrics improved. Owner explicitly chose FSR4 on; preserve it. Neither authorized box can validate DLSS RR/SR, and a DLSS setting mapped to FSR on AMD is not a DLSS run.

**Keep/restructure:** One explicit adapter contract for depth, vector units/sign, jitter, exposure and reset; effect denoising remains complete on non-RR paths. RR replaces SR at evaluation, not a second upscale after SR. Keep transparent composition separate where guides cannot describe it. **P priority 1.** Primary contracts: [AMD FSR 3.1 integration](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-upscaler/), [NVIDIA RR guide](https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/docs/ProgrammingGuideDLSS_RR.md). These specify inputs; they do not establish successful execution here.

## 7. Frame generation guides

**S:** `V/llvkframegenfsr.cpp::evaluate` validates slot resource identities and resets on serial discontinuity; `llcleandlssrrvk.cpp:468` and `llcleanfsr3vk.cpp:836` export the actual upscaler's motion/depth/reset. Separate presentation/pacing and generator modules exist. Depth/motion preparation and optical flow are additional work.

**Noise/smear:** Incorrect depth/motion, water/transparent occlusion and mismatched HUD composition are amplified into intermediate frames. FG does not improve real-frame estimator convergence or interaction latency.

**Keep/restructure:** Preserve immutable resource/serial ownership until generation and presentation retire; test generated-only images, reset, resize and UI separately. Report real GPU frame time, shown frame rate and pacing separately. **H:** prior RX7900XTX runs fell back from FSR4 FG to FSR3.1; requested 3x produced 2x. Do not claim those unsupported paths ran. **P priority 3.** [AMD's FSR3 overview](https://gpuopen.com/fidelityfx-super-resolution-3/) documents independent FG with compatible depth and motion inputs.

## 8. Volumetric fog, medium, water and sky

**S:** `S/clean_medium.glsl` models camera-segment L/T transport including height haze and water mist. `clean_vol_temporal.comp` now uses per-cell decorrelation, an 11-cell clamp and tent fallback; `clean_vol_integrate.comp:45` applies a spatial tent without writing filtered values back into history. `llcleanrenderervk.cpp:45697,52361` places GI decision before fog compute; the earlier handoff's “ambient visibility never runs” finding is not assumed to remain true. Runtime verification is still needed. Sky LUTs, cloud lighting and the far analytic tail are distinct costs.

**Noise/smear:** Sparse light/gobo picks and froxel-scale residual noise; long history trails moving volumes; the tent smooths shaft boundaries. **H:** fog1606 reported about 0.6 froxel extra edge softening and ~+0.04 ms whole-frame cost on AMD. This is a documented quality trade-off, not a lossless optimization.

**Cost:** Injection/light/temporal/integration, camera-medium application, cloud aerial perspective, water grid raster and heavy shading. **H:** nonrt-perf `f163ef9978` reports water ~10.5 ms on Pascal unaffected by tail slabs, while sky tail cost changed. Do not attribute water cost to fog because both call medium functions.

**Keep/restructure:** Keep one segment-transport algebra with explicit ownership across opaque, refraction and transparent composition. For water, investigate shading the visible surface once per pixel while retaining displaced depth, refraction and exclusions; first isolate geometry/fragment multiplicity with timers and output comparisons. Cache or share low-frequency distant transport only as a measured option. Do not remove the fog tent to buy FPS by reintroducing crawl. **P priority 2/3.** [Hillaire's production publications](https://sebh.github.io/publications/) document unified Frostbite volumetrics and scalable Unreal atmosphere; their central comparison here is shared physical transport and reusable atmosphere terms.

## 9. Avatars and rigged paths

**S:** `S/clean_draw_rigged.vert`, `clean_receiver_motion.glsl`, skin/pre-skin and RT build paths form separate raster/motion/caster obligations. Base includes #1617 reduced-tier RT shadow eligibility and per-avatar distance simplification. A reduced visual tier must not silently remove its shadow. Held poses still have root motion and need prior pelvis transforms.

**Noise/smear:** Deforming normals and topology/LOD changes invalidate history; coarse DI/GI receivers lose skin and hair detail; alpha-blended hair has no opaque G-buffer ownership. A cached pose can be geometrically coherent while its lighting history is stale.

**Cost:** Palette export, pre-skin/current+previous positions, BLAS update, alpha overdraw and lighting. A static replay without rigged records cannot validate these costs. Existing synthetic version-2 club capture has skin/pose support; ordinary version-1 viewer captures do not.

**Keep/restructure:** Preserve #1617, GPU skinning and held-pose motion; align receiver/caster generation with pose and LOD identity. Test dynamic and held poses, skin normal changes, attachment lights, translucent hair and reduced tiers. Use the same stable material evaluation in raster and secondary hits. **P priority 1/2.** Lumen's documented skeletal cache limits illustrate why static cache assumptions must be tested on deforming content, not imported blindly.

## Historical evidence and what it does not prove

Private inputs read: `rtperf-agent/LOG.md`, `fsr4-agent/LOG.md`, `nrt/ab.py`, `plan_ab.sh`, `queue_final.sh`, `amd-perf/runs2.txt`, `fog1606/msg{1,2,3}.txt`, branch logs `nonrt-perf` and `fog-noise`; `scripts/bench/still_detail.py`, `clean_dn_noise.comp`; canonical garbage test-box reference. Scratch root is documented privately in the canonical status. Captures/logs/images stay outside git; sanitized aggregate tables only may be committed.

| Prior experiment | Historical A → B | Meaning / limit |
|---|---|---|
| AMD wave32, house room | 23.83 → 22.31 ms | Already-kept traversal improvement, not this base's A/B |
| AMD parking off, house overall | 21.33 → 19.30 ms | Already-kept occupancy change; image/noise checked by prior worker |
| AMD house more stochastic samples | 19.43 → 21.48 ms; image std 2.27 → 2.30 | Higher cost did not solve dominant residual noise |
| AMD house higher adaptive setting | +0.4 ms; edge std 5.08 → 4.89 | Small quality movement; scene and old settings specific |
| AMD fog1606 normal view | 49.07 → 49.11 ms; window std p99 5.11 → 3.11 | Improved stability with measured shaft softening |
| AMD FSR4 vs FSR3.1 Quality | GPU p50 35.8 vs 31.8 ms | Distinct quality/cost trade-off; early GI-not-running outliers excluded by prior worker |
| Pascal water tail sweep | water ~10.5 ms at tail 0/4/9 | Supports investigating water raster/shading; not a new timing distribution |

The old roadmap is historical: claims of one queue and absent FSR4 are superseded by the current source. VR layered design remains a contract/reference, not proof of parity. Clean-room v2.1 budgets are explicitly hypotheses. No RTX or owner-machine run is authorized by this task.

## Ordered experimental plan

1. Establish A/A with exact executable, shader/settings, capture/path hashes, render/output extent, actual path markers and stable scene population. Gate asynchronous GI/RT readiness; distinguish unavailable data from zero cost. Record p50/p95/p99 and inter-run spread. Probe overhead is measured in its own timing arm.
2. Compare full/half reflection and native/half GI as **options**, using reference stills and thin-edge crops. Measure missing-receiver incidence before choosing sparse repair. Keep defaults unchanged.
3. Prototype the highest measured receiver-reconstruction or lossless scheduling opportunity. Image-changing variants stay opt-in. Validate light-off response and motion separately from frozen still noise.
4. Evaluate bounded water visible-surface shading and transport reuse after attributing their own costs. Preserve displaced shape and transparent ordering.
5. Only then consider larger DI estimator or cache representation changes; require the same admitted light populations and matched work budgets. FSR/FG and stereo variants get capability-specific coverage, never fictional cross-device comparisons.

Acceptance: no visible-noise trade for FPS by default; desktop defaults only on clear quality and critical-path wins on **both** amdbox and garbage. Pascal cannot exercise RT, DLSS or RR. A missing required capability is an explicit coverage limit, not a passing result. Fresh measurements/prototype decisions and restoration receipts follow in `results.md`.


## Measured ordering update (owner clarification: DLSS Ultra Performance)

The owner's path is DLSS Ultra Performance, not FSR. The RX7900XTX measurements in `astra_regression_milestone.md` locate AMD costs and do not measure NVIDIA or explain the owner regression via an FSR option. Historical attribution is secondary now: exact #1604 HOUSE pair is within repeat spread, and the historical campaign has ended.

First architecture work now follows the actual dependency chain: base shadow VIS/pass15 (~2.88ms AMD), adaptive VIS (~0.88ms), reach-map maintenance (~0.52ms; measured build~0.407ms, dirty~0.077ms), and local integrations/joins. One-frame-lag GI (~4.4ms queue span) is almost entirely hidden (~4.39ms); do not add it to graphics waits to manufacture a larger budget. Prototype exact work elimination before changing ray density or history quality.

UP/RR source contract: `rrPlan` queries network-specific optimal extents; log them rather than assuming 1/3 scale. Default raw mask26 still retains upscaled GI filtering (bit64 is off), stochastic radiance-mode integration, and sparse-reflection history (bit4 off). `RenderVKDlssFullResSignalsMode` is another independent effective-policy gate. Any proposal to remove these filters must address why they were retained: fewer samples per output pixel, sampled gobos/reservoir picks, and sparse reflection patterns. AMD cannot validate that tradeoff. Primary input/size contract: [NVIDIA DLSS RR integration](https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/main/docs/ProgrammingGuideDLSS_RR.md).

Prepared, default-off structural experiments: hard-first GI sphere publication with a hard count so hard-only cache readers avoid soft-region work (all soft semantics preserved); conservative empty-avatar depth bypass using existing incremental classification plus last successful GPU-publication certainty. CPU semantic tests pass; GPU/visual results are now recorded in results.md: GI-prefix has no established gain; empty-avatar timing is dominated by dependency attribution and does not justify a default change. Complete synthetic street and club provide image controls (both zero captured probes); truncated HOUSE remains timing diagnostic only. Original bounded receiver-support repair is preserved and paused behind this priority.

## Dense-light measurement update

Owner steering makes dense nearby occluded point lights the current architecture target. See [dense-local-lights.md](dense-local-lights.md) for source proof, clean total-frame A/B, admission windows, and the default-off residual-light mask. The current evidence supports retaining receiver-aware reach maps while improving their coverage and eliminating duplicate consumer work. It does not support globally dropping hidden lights from GI/reflection populations or reducing samples by default.

The completed AMD map-policy controls show scene-dependent cost: about0.545ms saved in a sealed128-light scene,0.183ms added in its visible-light control, and0.35–0.37ms added in the incomplete HOUSE. HOUSE's remaining blocked traced jobs are predominantly conservative open/depth-margin outcomes despite usable maps. Candidate-generation cost and residual visibility cost are separate. Bounded angular maximum queries across face boundaries are a proposed next structure, with discovery, invalidation and synchronization charged explicitly; no prototype result is claimed for that proposal.

The GI hard-prefix prototype has no established total-frame win. Empty-avatar bypass removes a timer interval largely attributable to dependencies, with only a small HOUSE mean effect and no clear distribution win. Neither is a default recommendation. The original receiver-support search remains paused and preserves valid-black versus missing-support semantics. Final quantitative prototype and image conclusions belong in results.md.

## Final measured outcome

The optional residual deferred point-light mask is the useful measured prototype: AMD total-frame reductions0.421ms HOUSE,0.957ms club,5.737ms sealed128 and5.785ms visible128. It removes repeated consumer work after pass15 owns the contribution; sampling, histories and indirect populations remain. Limited held-image controls show no obvious new missing-light/leak region, but strict noise equivalence is not established. Pascal's forced-nonRT control has no established effect and cannot validate RT/RR. All defaults remain unchanged. Both hosts are restored. See [final results](results.md), [mask distributions](mask-results.md) and [image evidence](images.md).
