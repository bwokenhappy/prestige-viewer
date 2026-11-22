# Native RT mirror instability (2026-11-21)

Status: READY for owner image/performance A/B. Production commit `b0a215f952`. Branch `fix-rt-refl-mirror`, base `855b29b343`.

## Evidence and scope

The owner's second clip is native: RR cannot be the necessary cause. The directly viewed room is stable while reflected lighting changes. Neither clip identifies the cube's actual material roughness, so this report does not assert it is below the configured sharp-ray threshold.

Source-proven cause of light-selection variance: `clean_deferred_light.comp` enables `RT_SHADE_POINT_BUDGET` and `RT_SHADE_STOCH_XI` for reflection hits. `clean_rt_hit_lights.glsl::rtHitLocalLights` previously sampled K weighted reservoirs with replacement even when fewer than K lights reached the hit. All four rays could repeat a lamp; another frame could pick blocked lights instead. Stratification spreads the draws but does not make a small light set exact. The light grid adds a preceding stochastic candidate-selection stage. `clean_rt_refl_temporal.h` explicitly documents that its anti-lag cannot distinguish an isolated bright light pick from real lighting change. Thus averaging alone cannot reliably remove these spikes without adding lag.

The existing `RenderVKLightGridFootprintCm` default is 2 cm; its comment promises noise-free close mirrors, but bypassing the grid still fed the stochastic shadow-ray reservoirs. An exact BRDF walk alone was not exact shadowed lighting.

This proves a native noise source; how much it explains the owner's particular cube remains an in-world check. Sparse reflection-ray reconstruction, hit geometry, sunlight and indirect illumination can still vary independently.

Code evidence: `clean_deferred_light.comp:2929` enables the reflection-only partition; `:2994` controls grid eligibility; `clean_rt_hit_lights.glsl:215` initializes it and `:497` finishes the K-slot output; `clean_hit_nee.h:149` implements partition ownership; `clean_rt_refl_temporal.h:55` documents the unresolved sparse-spike limitation of the existing filter.

## Fix and estimator contract

Live `RenderVKRTReflectionsExactLights` (Persist 0, default on) uses the existing K-shadow-ray budget, K=1..4:

* K=1 retains the original unbiased sampler.
* Up to K positive-light shares: evaluate each exactly, once.
* Above K: evaluate the strongest unshadowed share exactly, and sample the remaining shares with K-1 stratified reservoirs. The exact light never enters the sampled tail. The tail's weights and normalization include every remaining light, including shares displaced while finding the strongest.
* Every contribution is shadow-tested. No lights are added unshadowed; every share belongs to exactly one partition. At most K shadow rays, unchanged.

Exact ranking requires actual lights, so enabled reflection hits bypass the sampled ReGIR candidate list and use the existing bounded cluster/all-light walk (at most 256 lights). This increases BRDF evaluation work versus a grid lookup. GPU cost is unmeasured. There are no new images, buffers, histories, allocations or dispatches; scratch arrays have four entries. GI is unchanged. The split visibility-queue producer publishes the same K-slot output contract as the inline path.

No blur, radiance clipping, fixed random seed, history extension, reflection density or RR routing changes. Switching the A/B off restores the prior sampler and light-grid eligibility.

## CPU evidence

The fixture executes `clean_hit_nee.h`, the shader's actual estimator code. Budgets 2..4, light counts 1..12, 32,768 seeds per case: small sets exact, sampled tails unbiased within the test tolerance, all-blocked sets black within floating-point roundoff, using the live add-then-subtract visibility contract. Existing GI leak/mean/slot cases remain.

Separate 65,536-sample stationary hit probe, K=4, visible lamp contribution 8, other lights contribution 0.5 each but blocked:

| Lights | Old mean | Old temporal SD | Partition mean | Partition temporal SD |
|---|---:|---:|---:|---:|
| 4 | 8.004028 | 1.146735 | 8.000000 | 0.000000 |
| 16 | 8.001116 | 0.953947 | 8.000000 | 0.000000 |

These measure only light-selection noise at a fixed hit, not rendered-image variance or frame time. Non-dominant visible lights still have sampling variance when more than K lights reach the hit. Moving lights and occluders remain free to change visibility.

## Owner A/B

At the same cube, native first, keep camera/light settings fixed and hit-shadow-ray budget at 4. Toggle only `RenderVKRTReflectionsExactLights` 0/1; allow the existing history to settle. Compare lamp-patch brightness flips and grain, plus total frame and hit-shading GPU times. Then repeat with the owner's DLSS/RR settings. If substantial geometry-shaped flipping remains, that is not established as this estimator's cause and needs a separate sparse-ray/hit diagnostic.

## Validation

* Source checks: 132/132 PASS. Final add/subtract-contract fixture recompiled and rerun after its test-only refinement: PASS.
* Shader generation: 480 world + 42 FSR variants PASS, including inline, split and beacon reflection variants.
* Real configured Linux viewer build (four jobs, absolute nice 19): renderer/dependent objects, link and package PASS. The configured viewer flags include `-Werror` and the repository's existing warning exceptions.
* Package checks: 4/4 PASS (existing libdecor ISA warnings, no failures).
* `git diff --check` PASS; `git merge-tree --write-tree --name-only forgejo/main HEAD` clean against main `a0d6229cd8`.
* Build log: `/home/bryan/.cache/slipstream-build-tmp/rt-refl-mirror-build.log` (exit 0). The build began on dirty base 855b29b343; the compiled production files are exactly those in b0a215f952. Only the CPU fixture was refined while the build ran, then rerun separately.

No viewer, GPU or box run authorized or performed. No visual claim beyond the CPU fixed-hit model. No generated shader headers committed. No jobs left running.
