# Held-image A/A and option comparisons

All raw stills below hold240 frames and use the final replay loop; repeated keys overwrite earlier loops. Source/capture/settings identities are in the measurement ledgers. No HOUSE images are used. Complete synthetic controls have zero captured probes and the default environment fallback. Compare within each device/scene only: AMD and Pascal use different effective paths and dimensions.

Units are Rec.709-weighted display values scaled0–255. `std` is mean per-pixel temporal standard deviation, including history/transient effects rather than pure Monte Carlo noise. MAE compares mean images. Detail loss is the normalized positive loss of a band-pass magnitude, not a perceptual score; near-black scenes can have large normalized ratios despite small absolute differences. All edge-noise metrics use the first A image's common edge mask. Full metrics include tile tails, SSIM, edge errors, and raw-file SHA256.

The original raw arrays were bottom-up despite the writer comment. `still_detail.py` now flips both mean and variance; three baseline raw/PNG pairs matched the0.5-unit rounding bound. The formerly labeled high-variance sky region is water. This correction changes coordinates, not whole-image variance.

## Interpretation

No obvious new missing-light or light-leak region was found in the reviewed mask mean images and worst-tile crops. This is limited held-image evidence, not bit-identical or universally noise-equivalent output. Club mask errors at two keys exceed initial A/A error. Visible mask B1 temporal variation exceeded the first A/A range; the targeted B2/A3 repeat did not show a consistent penalty. All observations are retained rather than selecting the more favorable run. No default change is justified.

GI-prefix has no established performance gain; its club mean-image differences are comparable in scale to A/A, not identically zero. Empty-avatar controls preserve nonempty rigged work. Water/sky temporal variation and no-op timing variation on Pascal limit small-gain claims. These controls do not validate DLSS/RR, VR, captured probes, live ingestion or all moving-history response.

## amd-closed-map

Runs: `dense_closed_a1` / `dense_closed_a2` / `dense_closed_map0` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still0.bin|0.171728|0.169375|0.436820|0.434755|0.438430|0.339933|0.352606|

## amd-closed-mask

Runs: `mask_closed_a1` / `mask_closed_a2` / `mask_closed_b1` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still0.bin|0.137395|0.135518|0.435907|0.437013|0.429855|0.303346|0.297519|

## amd-club-mask

Runs: `mask_club_a1` / `mask_club_a2` / `mask_club_b1` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still2.bin|0.305822|0.313237|3.537398|3.508626|3.536736|0.020309|0.019830|
|.still5.bin|0.126778|0.173211|6.807998|6.805834|6.788111|0.015190|0.021625|
|.still8.bin|0.129989|0.182169|1.527725|1.541943|1.489014|0.017591|0.019393|

## amd-nrt-club-empty

Runs: `prototype_nrt_club_a1` / `prototype_nrt_club_a2` / `prototype_nrt_club_empty` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still2.bin|0.022889|0.014039|0.766108|0.776255|0.770761|0.001214|0.001023|
|.still5.bin|0.009948|0.009472|1.795043|1.795051|1.794983|0.000394|0.000366|
|.still8.bin|0.153140|0.061945|0.980635|1.056368|0.924357|0.004246|0.001731|

## amd-nrt-street-empty

Runs: `prototype_nrt_street_a1` / `prototype_nrt_street_a2` / `prototype_nrt_street_empty` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still0.bin|0.122554|0.113038|2.668068|2.669669|2.669289|0.017605|0.019787|
|.still2.bin|0.546965|0.450974|4.476056|4.280875|4.326203|0.076385|0.075336|
|.still4.bin|0.765944|0.733245|5.797289|5.717197|5.620711|0.184140|0.187033|

## amd-rtclub-gi

Runs: `prototype_rtclub_a1` / `prototype_rtclub_a2` / `prototype_rtclub_gi` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still2.bin|0.280200|0.278398|3.524174|3.514948|3.511618|0.018381|0.018586|
|.still5.bin|0.139643|0.182135|6.777048|6.779992|6.807902|0.017079|0.022134|
|.still8.bin|0.136895|0.144432|1.508138|1.488480|1.539359|0.016704|0.017709|

## amd-visible-map

Runs: `dense_visible_a1` / `dense_visible_a2` / `dense_visible_map0` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still0.bin|0.494770|0.526598|1.059778|1.037128|1.053607|0.176653|0.162621|

## amd-visible-mask

Runs: `mask_visible_a1` / `mask_visible_a2` / `mask_visible_b1` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still0.bin|0.467771|0.416903|1.069835|1.036178|1.122604|0.193564|0.170186|

## amd-visible-mask-repeat

Runs: `mask_visible_a1` / `mask_visible_a3` / `mask_visible_b2` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still0.bin|0.448607|0.466198|1.069835|1.098532|1.056160|0.172230|0.187676|

## garbage-club-empty

Runs: `club_a1` / `club_a2` / `club_empty1` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still2.bin|0.009903|0.010747|0.612863|0.612861|0.612840|0.000795|0.000868|
|.still5.bin|0.039062|0.045937|1.669848|1.669547|1.669870|0.001610|0.001859|
|.still8.bin|0.013201|0.018105|0.622608|0.645500|0.639348|0.000971|0.001570|

## garbage-street-empty

Runs: `street_a1` / `street_a2` / `street_empty1` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still0.bin|0.040369|0.038282|3.391607|3.391302|3.391553|0.005752|0.006119|
|.still2.bin|0.366461|0.345290|4.257172|4.210634|4.298763|0.047800|0.045260|
|.still4.bin|0.420011|0.353109|5.801856|5.700811|5.717305|0.145632|0.137865|

## garbage-visible-mask

Runs: `mask_visible_a1` / `mask_visible_a2` / `mask_visible_b1` (first A / repeated A / B).

|Still key|A/A MAE|A/B MAE|A1 std|A repeat std|B std|A/A detail loss|A/B detail loss|
|---|---:|---:|---:|---:|---:|---:|---:|
|.still0.bin|0.003021|0.036264|0.354720|0.354886|0.356179|0.023909|0.107822|

## Reviewed visual comparisons

Contact sheets show first A / repeated A / B means, then A/A difference×8 / A/B difference×8 / B temporal standard deviation×16. Crops show the same full-resolution96×96 region enlarged×4 without per-image normalization. In the visible-mask-repeat sheet, the second A panel is actual run A3; exact run names are above.

[amd-closed-mask still0](figures/amd-closed-mask-still0.png)

[amd-visible-mask still0](figures/amd-visible-mask-still0.png)

[amd-visible-mask still0-crops](figures/amd-visible-mask-still0-crops.png)

[amd-visible-mask-repeat still0](figures/amd-visible-mask-repeat-still0.png)

[amd-club-mask still5](figures/amd-club-mask-still5.png)

[amd-club-mask still5-crops](figures/amd-club-mask-still5-crops.png)

[amd-club-mask still8-crops](figures/amd-club-mask-still8-crops.png)

[amd-rtclub-gi still2](figures/amd-rtclub-gi-still2.png)

[amd-nrt-street-empty still2](figures/amd-nrt-street-empty-still2.png)

[amd-visible-map still0](figures/amd-visible-map-still0.png)

[garbage-street-empty still2](figures/garbage-street-empty-still2.png)

[garbage-club-empty still2](figures/garbage-club-empty-still2.png)

[garbage-visible-mask still0](figures/garbage-visible-mask-still0.png)
