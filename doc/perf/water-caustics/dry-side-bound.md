# Conservative dry-side certificate

This repair supersedes rendering source e420. All existing Linux and corrected
AMD performance/image points describe e420, before this repair. They cannot
accept the repaired executable. No lease is held while source and artifacts change.

## Geometry lower envelope, not an inverse surface query

Let W be this recording's mean water height, d the minimum of zero and every
**selected GPU-published** packed-half dh, and A = 1.6 sum(abs(wave amplitude)).
The wave surface is at or above W + d - A, with the numerical inset below.
This is a global lower bound on the drawn geometry. A receiver strictly below
it is submerged wherever a drawn water triangle covers its XY. It does not
claim the geometry covers every XY or invert Gerstner horizontal displacement.

For both low and high grids, fetch gates are in [0,1], k and depth are nonnegative,
shoaling is at most1.6, depth cap and distance LOD only reduce amplitude magnitude,
and each cosine is in [-1,1]. Breaker/run-up lift is nonnegative for the validated
Hs/surf/run-up inputs. Horizontal Gerstner displacement changes XY, not this global
z bound. The actual VS uses convex morphs of displaced endpoints, then rasterized
triangles: every such convex combination preserves the same z lower envelope.
The unchanged clean_water_waves.vert is the authoritative geometry path.

Field samples decode the actual selected host-coherent GPU copy, including half
rounding/subnormals; there is no minimum over pending SLSeaState CPU field data.
Bilinear weights and the window-edge fade are convex, and the outside/no-valid-field
fallback has dh0. Hence the minimum includes0. All four half channels are checked
for infinity/NaN. Invalid mapped data, invalid origin/resolution/scale or unknown
inputs return finite -FLT_MAX, which disables the receiver. Version AND ring slot
identify publication; destruction/recreation resets the cache. A newer unpublished
CPU field cannot replace this certificate. Slot/version changes replace even an
upward-moving minimum, rather than retaining stale extrema.

Flat mode includes requested mode0 and procedural-pipeline/buffer fallback.
Its surface is the actually published LL water planes, not the region mean and
not their deliberately padded +/-8m cull AABBs. The live producer emits local z0
and horizontal affine transforms with flags2050. Quantized local z0 decodes exactly
through the existing production lattice helper. The certificate takes minimum
model[14]-instanceStoreDz over active water rows within the GPU draw/count cap.
Unsupported animated/tilted rows disable the certificate. The cache consumes only
successful full/dirty instance uploads; refused/partial uploads disable it until
full retry. Minimum-row updates/deletions, count shrink, origin rebase and enabling
from OFF force a rescan. Other dirty rows can only lower the existing minimum.
The unpublished mirror never independently certifies a refused upload. No water
row, invalid transform or unknown surface gives the finite disabling sentinel.

## Numerical margin and visible exclusion

CPU arithmetic uses double for the amplitude sum/envelope, then subtracts
0.002m +64*FLT_EPSILON*(|W|+|d|+A+1), before float conversion. Flat mode uses the
same inset around the exact producer/agent subtraction. The pixel test adds
0.002m +64*FLT_EPSILON*(|Pz|+|cameraZ|+max(viewDepth,0)+1) for depth reconstruction.
These explicit margins cover the bounded float sums/mixes in these producers;
they are not a claim of arbitrary malformed projective transforms being valid.
Nonfinite tests use exponent bits and a volatile integer load. The previous plain-bit predicate failed under actual Clang O3/fast-math (LLVM folded it back to an assumed-finite FP predicate). No volatile floating arithmetic is used. The corrected production fixture must pass actual GCC and Clang O3/fast-math before artifacts are accepted.

The visual tradeoff is deliberate: wet receivers in the ambiguous band above
this lower bound get no caustics, even when locally submerged under a crest.
At prior recorded Hs0.316753m, reconstructing the existing PM ladder from that
rounded Hs estimates sum amplitudes0.244455m and A0.391127m. This is a CPU spectrum
estimate, not a new GPU receipt. At W20/d0 CPU inset is about2.163mm, then the receiver
margin adds about2.3mm for a near-camera under-water pixel. The exclusion is thus
about0.396m below the mean in that condition. The general eight-wave Cauchy bound
is sum amplitudes<=Hs before individual steepness caps, so A<=1.6Hs (~0.507m here).
A region4m below the current mean widens the current-region exclusion by4m.
This can suppress otherwise desired shallow effects in mixed-height scenes;
it never licenses modulation on the dry side of a drawn surface.

## CPU regression and ABI

scripts/check/test_water_caustics_bounds.cpp compiles the production certificate
with fast-math. Its old world-XY rule FAILS a dry receiver above a horizontally
displaced Gerstner vertex; the repaired rule passes. It also checks the unchanged
production slsea::eval twin across low/high, crest/trough dry points, convex
interpolation/grid morphs, exact packed-half minima/subnormals/invalid channels,
no-field0, unpublished-vs-published replacement, flat mixed heights, upload refusal,
retry, minimum deletion, rebase, unsupported geometry and OFF null data.

sea[6].w / scalar27 is zero/reserved in both this branch and water-options-1636.
Existing shader consumers use only sea[6].xy (grid order) and .z (our toggle).
Frame-info ABI size is unchanged. The new CPU cache DOES change renderer C++
layout: every dependent object must rebuild. Prior immutable-overlay reuse is
incompatible. Repaired Linux and Windows artifacts require fresh dependency
builds and exact embedded changed-SPIR-V byte verification before GPU acceptance.

## Independent publication-path audit after the compiler regression

recordWorld input ingestion copies wf.sea/waves and calls uploadSeaField. That
function validates pointer/resolution/mapping/version, copies exactly128KiB into
the next host-visible/coherent ring slot, then publishes slot/version/valid. The
descriptor at set0/binding67 covers that buffer from byte0. fillSeaInfo selects
that same ring slot via sea[4].z=slot*128*128 uvec2 texels. The certificate scans
uint32 words at mapped+slot*128*128*2, the identical byte range; it never scans the
pending wf pointer. All four packed-half channels are checked as integers first.
No-field valid0 means shader seaOpenSea/dh0; missing wave pipeline/buffer means
fillSeaInfo flat clearing and the separately successful plane-upload certificate.
The new sea[6].w assignment follows fillSeaInfo and precedes vkCmdUpdateBuffer
for this recording's mSSRInfoSSBO, the same info read by the caustic consumer.
No scene-input source, GPU descriptor/slot publication, or existing sea output
changes in the finite-guard correction. This audit is independent of either
compiler fixture and does not establish valid-shore-field GPU cost coverage.
