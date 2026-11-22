# ASTRA final measured results

The useful prototype is an optional early mask for deferred point lights whose full contribution already belongs to active pass15 radiance. It removes repeated consumer work while preserving the configured rays, candidates and histories. On RX 7900 XTX it saves 0.421 ms in the incomplete HOUSE, 0.957 ms in the complete club, and 5.737–5.785 ms in the two 128-light controls. This does **not** establish the cause or magnitude of either owner's NVIDIA regression, and does not solve hidden-light sampling noise.

All experimental settings remain default-off. No desktop defaults changed, no pushes/merges, no extra agents, and no owner-local GPU/viewer use. Both remote hosts have been restored and their matching locks released. Tested renderer tip: `57f164d27d150b5f4c4a426c11071321fcd11cef`; original fresh base: `ae1f063eb7ca9c2c0e0bbec3921d2c21821e493a`. Reports/tools may have later commits without changing those executable identities.

## Main same-binary A/B

|Host / scene|Off mean ms|Off repeat range|On mean ms|On repeat range|On−off ms|Meaning|
|---|---:|---:|---:|---:|---:|---|
|AMD / house|19.176448|0.061595|18.755632|0.033886|-0.420816|Timing only; truncated capture|
|AMD / club|33.529853|0.004175|32.573343|0.000000|-0.956510|Complete synthetic control; limited images|
|AMD / closed|13.517652|0.028827|7.780543|0.000000|-5.737109|Complete synthetic control; limited images|
|AMD / visible|14.932036|0.081668|9.146615|0.035298|-5.785421|Complete synthetic control; limited images|
|GTX 1080 / visible128|179.148712|2.861157|179.718027|single B|+0.569315|RT0/GI0: inactive-path control, no established effect|

The AMD paths actually ran GI3/ReSTIR/pass15 at 1920×1033 render/3840×2066 output, no FG. Pascal actually ran forced non-RT SPLIT+FEATURE at native 3840×2062, FSR3.1/no FG. It cannot validate RT/RR or NVIDIA DLSS behavior. Ranges are ranges of run means, not confidence intervals; a single B has no repeatability estimate. Warm-ups are excluded.

The dense closed deferred-local interval falls from 5.959 ms to 0.273 ms while baseVIS remains~0.075ms. HOUSE's~0.424ms local-light reduction matches its0.421ms total reduction; club shows the same attribution. This is a total critical-path benefit, not merely a timer mark moving. Full distributions, source proof and same-scene identities: [mask-results.md](mask-results.md).

## Why hidden lights remain expensive

Reach-map policy is scene-dependent: clean map1-minus-map0 is+0.367ms in HOUSE,+0.183ms in visible128, and−0.545ms in closed128. The closed verify window has128usable maps and approximately94%map-blocked jobs, but the residual deferred point-light loop still dominates without the mask. HOUSE's settled residual jobs predominantly encounter open-map or conservative depth-margin outcomes despite105/106usable maps.

Those counters are explanatory logging windows, not exact timed aggregates or candidate rates. Mode3 adds diagnostic work. One moving window has a subset>all anomaly, and the diagnostic open bucket differs from the production predicate for out-of-range receivers; precise probability claims are not supported. Clean mask results do not enable those counters.

Next structure: retain receiver-specific visibility and current light/BVH validity; investigate bounded conservative angular maximum queries across cube faces and measured depth-margin refinement. Preserve unknown=open, legitimate indirect/reflection light and prompt invalidation. Do not read stale LVC maps in the earlier ReGIR build or treat eight cell corners as an occlusion certificate. Discovery is not free. See [dense-local-lights.md](dense-local-lights.md) for stage tables, proposed structures and primary RTXDI comparisons.

## Earlier prototypes: retain as experimental, not performance fixes

Source95f820bbf1, same-binary comparisons within each row. CPU semantic/lifetime checks covered133120GI cases and131072avatar cases. These are not substitutes for GPU/visual validation.

|Host / option / scene|A mean ms|A range|B mean ms|B range|B−A ms|Interpretation|
|---|---:|---:|---:|---:|---:|---|
|AMD GI prefix HOUSE|19.166357|0.017800|19.162216|0.000000|-0.004141|Inside A/A spread; hard regions unexercised|
|AMD GI prefix club|33.603711|0.024222|33.602211|0.000000|-0.001500|Inside A/A spread|
|AMD empty-avatar HOUSE|19.166357|0.017800|19.115161|0.003175|-0.051196|Small mean change; no clear distribution win|
|AMD empty-avatar street|13.967245|0.052347|13.912421|0.000000|-0.054824|Single B; small effect vs spread|
|GTX empty-avatar street|45.954035|3.533474|46.952937|2.485391|+0.998902|No established gain; water/sky variation|
|AMD empty-avatar club|25.256100|0.019073|25.263105|0.000000|+0.007005|skip0: original nonempty path retained|
|GTX empty-avatar club|102.813078|0.901849|100.938807|0.760470|-1.874271|skip0: lower means are a no-op control, not savings|

The empty-avatar HOUSE option removes a~0.73ms copy interval, but~0.727ms reappears in cloud/particle intervals: most was dependency attribution. The actual mean reduction is only~0.051ms, with essentially unchanged p95 and variable p99. The club's1168rigged faces correctly keep skip0. The GI prefix preserves sphere semantics but provides no established frame benefit; HOUSE publishes soft spheres but no hard spheres, and replay does not cover live ingestion.

The original bounded receiver-support proposal is preserved unapplied in [proposals](proposals/README.md): at most12extra taps, valid black preserved, missing-support fallback retained. It was paused by owner-directed regression/dense-light priorities and is not a measured implementation.

## Historical regression and owner-specific limits

Qualified fixed-HOUSE AMD comparison: current median19.133ms versus3190 median22.483ms (~3.349ms faster). The exact#1604 before/after pair differs by −0.012 ms, inside repeat spread; recorded soft-change activity is not proof of hard-region coverage. Wrong-projector and late-GI runs were rejected. No owner regression was reproduced. A standalone-pass absence was never used as proof of zero cost.

Jetstream.4=`2f0154f9b89bf799ab7c695fb80e9b2913b6c05b`;.10=`ba53ba368543e53a1b97dbca4d239a74fe130673`. #1604 is in neither tagged ancestry. Reach/cap defaults change between these tags; core GI/deferred shader files do not. Native TAA→DLAA migration is explicitly withdrawn for the3060 user's DLSS Performance, and FSR differences do not explain the5090 owner's DLSS Ultra Performance path. See [tag/source audit](jetstream-3060.md) and [regression evidence](regression.md).

Measured AMD spans include baseVIS~2.88ms, adaptiveVIS~0.88ms, LVC~0.52ms (build~0.407, dirty~0.077), and lag-GI~4.4ms almost entirely hidden. These overlapping spans cannot be summed. Lower-resolution AMD tests show pixel work scaling while world/fixed-volume work persists; they are not DLSS/RR quality or timing measurements. Pascal native dense-light local shading is~150ms because it still evaluates real contributions, not the RT-owned zero subset.

Required for owner-specific attribution: matched NVIDIA scene/camera, effective output/render dimensions, RR/SR/full-resolution signal policy, actual settings/FG state, VRAM/recovery state, and installed executable hashes. Obtain owner-supplied matched evidence or a separately authorized NVIDIA test host; the owner's local GPU remains outside this task's authorization. HOUSE lacks4680textures and captured avatars/probes. Complete controls still lack captured probes/full atmosphere, and do not cover every VR, projector, transparency, live-ingestion or moving-history case.

## Image evidence

[Images and numeric controls](images.md) contain 12 A/A/B cases, held 240-frame final-loop images, raw SHA256, common-edge noise statistics and reviewed contact sheets/crops. HOUSE quality is excluded. No obvious new missing-light/leak region was observed in reviewed mask controls, but images are not bit-identical and some A/B errors exceed A/A. The targeted visible repeat does not show a consistent temporal-noise penalty; strict universal equivalence remains unproven. Pascal mask MAE0.036264 is small in absolute output units but exceeds its0.003021A/A floor. No default promotion or claim of RT parity follows.

## GTX 1080 distributions

All rows below are actual forced non-RT SPLIT+FEATURE, RT0/GI0, nativeFSR3.1/no FG,3840×2062.95source is the GI/avatar prototype binary;57source is the residual-mask binary. Compare options only within their own binary/capture.

|Source / run|Frames|Mean ms|p50|p95|p99|
|---|---:|---:|---:|---:|---:|
|95f820bbf1 / street_a1|1684|48.072117|38.333183|99.163811|105.839073|
|95f820bbf1 / street_a2|1684|44.538644|36.132256|97.945763|105.196510|
|95f820bbf1 / street_empty1|1684|48.195633|39.826847|99.347229|106.084061|
|95f820bbf1 / street_empty2|1684|45.710241|35.253281|98.119553|106.003838|
|95f820bbf1 / street_a3|1684|45.251342|35.426624|98.500381|104.990562|
|95f820bbf1 / club_a1|1594|103.409116|113.436768|163.567932|167.085632|
|95f820bbf1 / club_a2|1594|102.522850|111.998398|164.095490|167.291199|
|95f820bbf1 / club_empty1|1594|100.558572|111.969917|163.722565|167.339417|
|95f820bbf1 / club_empty2|1594|101.319043|111.617699|164.351517|168.167130|
|95f820bbf1 / club_a3|1594|102.507268|112.096413|164.270523|167.268997|
|57f164d27d / mask_visible_a1|362|177.718134|131.630234|241.120346|242.976517|
|57f164d27d / mask_visible_b1|362|179.718027|131.990311|241.195801|242.887161|
|57f164d27d / mask_visible_a2|362|180.579291|130.949127|241.290726|242.915100|

## Validation, reproducibility and restoration

The full source check passed 108 checks. Twelve affected mask shader variants and four admission/VIS/LVC variants compiled and passed spirv-val. Windows ZIP/installer and Linux TESTCTL packages built in the isolated worktree, serialized against any ninja, at absolute nice19 with 12 jobs and the required TMPDIR. Linux package checks: 4 passed; libdecor's build-host ISA warning was handled only inside the owned package with backed-up native compatible libraries. Functional valrun/slctl smoke passed; no Vulkan validation layer was available. No generated vk_*_spv.h is committed.

The ledger contains 64 accepted nonwarm AMD and 17 GTX receipts, full GPU percentile/segment data, effective path/extents, executable/capture/path/settings identities and VRAM metadata. Rejected receipts remain private and are indexed/documented rather than counted as baselines. Commands, matrices, package hashes and analysis usage: [reproduce.md](reproduce.md). Raw logs/captures remain private; reviewed synthetic images and sanitized metrics are committed.

Both hosts are restored: AMD job.ps1/job.log/job.done hashes match backups, owner settings unchanged, no viewer/task, matching lock released; garbage settings/dialogs/run.log verified, exact original current symlink restored (already dangling originally), own validation symlink removed, no viewer, matching lock released. Local long-lived controllers exited with signal 143, but the Linux remote harness was verified continuing; only pending local collection was restarted, never a duplicate viewer. Receipts: [restoration.json](evidence/restoration.json). No further host runs are planned.
