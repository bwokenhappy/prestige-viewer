# Jetstream 1.7.0.4 → 1.7.0.10: source mapping and limits

Owner-reported RTX3060: DLSS Performance, approximately20→15FPS under unspecified settings (usual30FPS). These are uncontrolled observations:50→66.7ms (+16.7ms), or33.3→66.7ms from usual30. No3060 measurement exists in this campaign; do not equate this with the5090 report or extrapolate AMD timings.

## Source proof

`git ls-remote forgejo` and local tags agree:

| Tag | Source |
|---|---|
|jetstream-1.7.0.4|2f0154f9b89bf799ab7c695fb80e9b2913b6c05b|
|jetstream-1.7.0.10|ba53ba368543e53a1b97dbca4d239a74fe130673|

The Jetstream workflow increments the build suffix; `scripts/ci/version.sh` derives BUILD from it and the release workflow checks out the event source. This maps tags, not the user's installed executable hash.

- Reach-map default changes0→1; nearest-first adaptive cap changes0(unlimited)→25 (`1dfd3be57d`). These are policy/ray-allocation changes, distinct from implementation cost. The completed current-binary2×2 AMD HOUSE factorial measures a0.35–0.37ms net reach-map cost and0.03–0.05ms cap25 saving in that incomplete capture (see dense-local-lights.md). A sealed128-light control instead benefits from maps. Neither is a historical or NVIDIA measurement; these values do not explain a16.7ms3060 increase.
- Native TAA→DLAA migration was an initial conditional source lead. **WITHDRAWN for this report:** the user uses DLSS Performance. The migration preserves an already-selected nonzero upscaler. It is also not an explanation of the5090's DLSS Ultra Performance path.
- #1604 (`9a426c8032`) is not an ancestor of either tag. It cannot explain the exact tagged .4→.10 comparison. Our earlier3190 anchor is after .4; that AMD comparison does not test the entire .4→.10 interval.
- Core `clean_gi_common.glsl` and `clean_deferred_light.comp` are unchanged between these tags. `llcleangivk.cpp` adds optional default-off noise statistics. This does not exclude costs caused by shared scheduling, resource allocation or policy.
- Frame generation integration/pacing and shared presentation changed. Frame generation remains default-off; the3060 user's actual state is unknown. The5090 owner's no-FG statement must not be silently transferred to this separate report.
- AMD wave32/parking changes are vendor/subgroup conditioned. NVIDIA was already subgroup32 with parking enabled; an AMD speedup is not a3060 explanation.
- VR stereo defaults and paths changed; relevance requires an actual VR path. No such path has been established for this report.
- Recovery VRAM cleanup and NGX reinitialization fixes are later than .10 (merged byae1f063eb7). A prior device-loss/recovery event is a conditional lead, not evidence of steady-state memory pressure or the reported regression.

## Investigation priority

Dense nearby point lights hidden behind walls/floors, independently reported by the5090 owner, are now the central architecture target. Establish candidate/proposal work, reach-map eligibility and conservative-open reasons, redundant exact work, visibility-ray allocations and noise. Shared source makes this relevant to investigate on AMD; it does not establish a common3060/5090 cause.

Missing for owner-specific attribution: matched scene/camera, effective settings and render/output dimensions, actual RR/SR routing, frame-generation/pacing state, VRAM residency/pressure and recovery history, installed executable identities. No broad historical bisect is justified by the present evidence.
