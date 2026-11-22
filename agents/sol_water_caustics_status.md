# Water caustics implementation status

Branch: `water-caustics`; worktree `agents/worktrees/sol-water-caustics`; fetched base `ae1f063eb7`.
Model: runtime identifies GPT-6-based Codex; requested gpt-6.1-sol routing cannot be independently attested.

## Ownership and design

Parent coordinates packages/box slots and audits water bugs. No changes to water-1637 water fragment/FX,
options-owned controls/sea amplitudes/foam, or RT water pass 14. New caustics include is enabled only in
uber/combine/base deferred lighting, and modulates **direct diffuse** only. Existing sun/moon visibility,
material response and outgoing fog remain in their original paths. New persisted/live boolean
`RenderVKWaterCaustics` defaults on, effective only with transparent water. Off branches out before
wave/field/normal texture work. This does not yet prove zero register-pressure or frame-time overhead.

Wave Hessian is three appended output expressions in `clean_sea_state.glsl`, using the same amplitudes,
fetch/shoal/depth caps, phase and LOD as the drawn wave surface. Existing displacement, normals and foam
expressions are untouched. Curvature member/init/accumulation is compile-time guarded so
non-consumers retain the original structure. Seven final non-consumer variants are now
raw SPIR-V byte-identical to the original (SeaFS, SeaVS, WaterWaves, WaterFrag, Forward,
LOCAL and RT forward pass14); final BASE/COMBINE/UBER/VR compile plus spirv-val (including two-view specialization) PASS.

The lens uses refracted active atmosphere sun/moon direction, projected entry, wave curvature and the
water shader's middle EEP normal layer (both day-cycle maps, existing animation, wind-driven detail).
Signed compression is bounded and odd for symmetric Hessian pairs; it approximates redistribution of
energy. It is not a physical ray map: horizontal Gerstner remapping, breaker curvature and displaced
surface inversion are approximate. Receiver dry rejection uses seaSurface height, including breaker
lift and geometry LOD, rather than just the mean water plane. Incoming-path extinction is separate
from the outgoing fog, and contrast fades at the surface/deep/distant pixels. Footprint uses per-eye
`deferInvVP` and `P-eye`, including orthographic projection; no negative upscaler mip bias.

`sea[6].z` audit: CPU `SLSeaState::fillFrameInfo` clears the array and explicitly reserves floats
26/27 as zero on main and water-options-1636. Existing renderer fills field/grid lanes only; existing
shader consumers read `[6].xy` only. The renderer publishes z after `fillSeaInfo` so its flat clear
and wave copy cannot overwrite the flag. This changes no layout/header sizes. Options intensity
continues upstream in the unchanged state/wave arrays. Separate UI patch is
`agents/patches/water-caustics-options.patch`, generated against water-options-1636 tip a506c46364;
that floater does not exist on the base, and its branch has not been edited.

## Validation milestones

- PASS new water-caustics source check: production-expression evaluation of 2048 signed Hessian
  pairs (finite/unit bound/symmetric mean), 64 finite-difference wave Hessians, 24 translated-camera/per-eye/orthographic projection
  cases, plus diffuse/off/dry/setting/ABI source contracts.
- PASS final 11-variant shader matrix: seven original/non-consumer raw-byte identities, plus BASE/COMBINE/UBER/VR compile and SPIR-V validation. All active kernels also validate after two-view specialization.
- Full make check PASS107 after final macro isolation. Linux TestCTL candidate build active at explicit -j4/nice19; Windows follows serially.
- Latest owner override: TMPDIR=/home/bryan/.cache/slipstream-build-tmp,
  CMAKE_BUILD_PARALLEL_LEVEL=4, NINJAFLAGS=-j4, NINJA_JOBS=4, nice19, Linux/Windows serial;
  independently check <=2 existing Ninja processes immediately before building. Earlier 12-job
  setting is superseded. No owned -j12 Ninja existed when the override arrived.
- Parent fixture helper commit 2bc006843c; validated synthetic under/above/dry inputs at
  `/home/bryan/.cache/slipstream-build-tmp/codex-water-fixture/validated/`.
  Water height20, density1, explicit per-camera under flag, valid periodic normal texture,
  water row8 flags2050; unrigged only. Runtime waves/caustic signal and primary-pipeline gate
  must be proved in receipts, not inferred from settings or settled status.
- Both AMD and garbage total-frame OFF/ON and original/OFF overhead measurements are REQUIRED
  before READY. No GPU timing or runtime/image claims yet. No owner-local GPU/viewer use.
  AMD original-A calibration completed with exact restoration; no lease is currently held. Foreign viewers must never be stopped.

## Remaining

Current shader matrix plus non-consumer SPIR-V identity, full make check, serial Linux/Windows
packages, independent <=30min box locks and backup/restore, active base/combine + nonzero signal
proof, fixed-frame 4K A/B total GPU/frame cost on **both** boxes, images/dry/roof/direct-only/moon/
RT controls, supplemental actual-avatar coverage, committed implementation and final receipts.

Additional milestones: baseline packages copied into own cache after exact executable SHA256
verification against Hegel source ae1f063eb7 receipts (Linux4c9b9d...7f49, Windows99b9f7...dba5).
Floater patch git apply --check PASS against options tip a506c46364. Parent fixture helper
cherry-picked alone (2bc006843c -> b0cd6755df). Fullbright-instance and metal=1 PBR material
controls generated privately with updated manifest hashes; neither is rigged. Runtime
primary-path coverage will require BASE or COMBINE sample n==GPU n (known one-record/frame
producer contract), explicit selected route and no fallback; aggregate coverage, not raw
per-frame exports. No unrelated rawperf feature will be cherry-picked.

Final source check + Linux build controller started; independent <=2 Ninja gate currently waits
on three foreign Ninja processes. It pins CMAKE_BUILD_PARALLEL_LEVEL=4, NINJAFLAGS=-j4,
NINJA_JOBS=4, PYTHON_CPU_COUNT=4 and actively maintains absolute nice19 for owned descendants
(recursive shells on this host can reset priorities). No foreign process priority is changed.
Source check reruns before Linux; Windows will follow Linux only. GPU tests still pending.

Post-review shadow policy: effect requires authoritative sun/moon visibility. `specSunOn`
requires host shadow mode (which requires atlas/pipelines), and the debug-shadow-off and weak
sun/no-ray sentinel disable caustics. The contrast fades over 60-80% of the final shadowed
range and is absent beyond80%, before the existing lighting fades to unshadowed. No new ray
or visibility multiplication. Shadow/roof GPU validation remains pending.

Final2 make check PASS107 after macro isolation. An earlier controller attempt failed on
spaces in ps comm fields before any viewer build; process parser corrected to PID/PPID/NI
only, no foreign processes affected. Outer build shell is intentionally held pending the
updated shadow-gated active GLSL matrix. Seven non-consumer raw identities remain valid
because only the caustics include changed.

Benchmark clock limit: SLSeaState uses SLWeather::syncedSeconds (UTC wall time), independent
of the frozen replay clock. Frozen still holds freeze EEP detail animation but not the
wave phases. Lit image comparisons require phase-averaged interleaved >=3 ON/OFF/A/A and
unfrozen controls; they cannot prove bit-identical wave-phase A/B. No clock source edits.


## 2026-11-20 02:12 UTC runtime preparation milestone

Render source remains exactly e420c4eb55196a738a5f1437abf8babcc74244a0. Later changes
are benchmark helpers/documentation only. All521 production world/FSR shader variants generated;
Linux candidate C++ compilation is still active (explicit Ninja -j4, owned nice19).

The first AMD controller attempt failed before launching a viewer because the original template
assigned an absent PSCustomObject `source` property. The owned reservation was restored/released
at01:47:45Z. Fixed metadata insertion uses Add-Member, and all seven mandatory matrix fields are
explicit. Local portable PowerShell7.7 parsed run/lease/validator and exercised the production
metadata assignment on all three A-only rows plus a missing-property control before retry.
Original source/executable hash and explicit mock READY/source contract passed. The portable
parser archive SHA256 is c89d9c682d2c403dfe726f3df948e359675235a5bd6fab535776de69729ca0cb.

Fresh independent preflight found AMD unlocked/idle. Atomic reservation01:56:26Z, maximum
02:24:26Z; original viewer PID14684 launched01:56:26.818Z. All three under/above/dry calibration
runs exited0. Automatic restoration released01:58:20Z; all seven owner settings/dialog/job
states and owned lock release verified. No foreign viewer/process was stopped, no login occurred,
and the old baseline package was not modified. Both AMD blocks used fresh isolated profile/cache.

Calibration proves actual3840x2160 window and96-frame stills, nine injected rows/six slots/one
texture with zero failures, and294912 water triangles/frame (tessellated wave-grid route).
It does NOT prove runtime sea parameters from originalA, which lacks our numeric state log.
Images expose submerged beds, thick roof, dry platform and straddlers. Under pose primarily
exposes the deep bed; supplemental poses are still needed for shallow and roof-interior controls.

All three reports are REJECTED for cost: requested FEATURE remained UBER with only
`g_defer_light.n==GPU.n==241`, no BASE/REFLECT. `pipelines.settled` was true, but the full-quality
upgrade group had not installed. Source `benchPipelinesPending` omits mUpgradeJob; no unrelated
readiness fix is included. Raw reports/logs/stills are retained in cache
`sol-water-caustics/amd-a-only2-results`; analyzer receipt accepted0/rejected3. No cost/effect
acceptance follows from these runs.

The revised own harness reuses the RT-noise protocol read-only: process-local
SL_VK_PIPELINE_WORKER=0 for both arms;20 simulated seconds/1200 excluded PREPARATION frames;
held segment only for timing; explicit installed-full-quality and actual coverage gate.
Optional prewarm is bounded10min/package and aborts only its own untimed replay once installation
and the intended route are observed. Intentional UBER uses RenderVKDeferredSplit0; disabling
FeatureSplit alone selects plain COMBINE. First candidate pilot requires visible nonzero ON ROI
before repeated comparisons. No further box hold until candidate/pilot is ready.


Linux controller recovery02:17:30Z: original long exec session returned143, with no
compiler error/OOM/interrupt line establishing a cause. Cause unknown; this is not a
source-crash or full-build-pass claim. Its final verified owned compiler child drained
normally and produced its object. The controller/Ninja were gone. No foreign signal,
Ninja log, depfile or success entry was modified. Durable detached controller PID687647
(start ticks225771680) resumed ordinary make/Ninja from a fresh gate of two existing
Ninjas, explicit4jobs and absolute nice19. Ninja correctly rebuilds unrecorded outputs.
Original and continuation logs plus PID/start receipt are retained privately.

Windows overlay preparation follows the reviewed rigcull approach, with exact frozenA
executable and packaged ae1 settings checks, own full production shader headers and
literal noncaustic array word parity, unchanged C++ header ABI, own renderer/appviewer
translation units without foreign PCH, all dep/PDB/manifest/link outputs owned, and
protected baseline/link input hashes before/after. Generated headers and objects are
frozen/hash-checked before link/package. This will be isolated actual object/link
validation, not a full configured Windows build. No Windows heavy step has started.

Garbage schedule is alpha(~21:16local), then rigcull; water will not reserve in gaps.
Water may use a later idle block only after rigcull's restoration notice and candidate
readiness, followed by independent preflight. AMD has no water lease currently.


## 2026-11-20 02:50 UTC build and harness outcome

Both packages are frozen and complete, feature NOT READY. Linux full build/packaging
PASS after ordinary Ninja continuation, then all4 package checks PASS with2 libdecor
ISA warnings. ExeSHA fda6e8ae32beec1f9a00e2759a60594c0f8acb810105be1fee585163d165d1bf;
7621-entry manifest9fac44b95f83bb9b471a67b636c26fc29f4fc3dd5b3f843edfa7d1d79b650a47.
Windows isolated actual renderer/appviewer objects and PE link PASS, without foreign
PCH; not a full configured Windows build. ExeSHA
50bf0a49012c48baac97815092839b3878398e660a0333ed77a7c25de62440a9;
7548-entry manifestb27fafd214fe9f539a26da0384145d8a22538f26b5d5dad449241abb415d57bf.
All55378 protected baseline inputs and1159 reused link inputs hash-identical afterward;
7545 static package files match frozenA. All496 noncaustic shader arrays are literal
SPIR-V-word identical; only25 allowed deferred lighting variants changed. Exact
baseline XML and unchanged C++ header ABI proved. Receipts are in doc/perf/water-caustics.

Candidate AMD block02:38:00Z launched actual viewer10688 at02:38:09.399Z. The own
prewarm helper failed reading the actively write-held log with ReadAllText, stopped
only its own viewer and restored/released02:38:23Z; all7 owner states plus release
verified. No positive ROI, no cost result. AMD handed to rigcull; no reacquisition
without coordination. This failure is a harness error, not a shader/cost result.

Windows active log reader now uses FileStream Open/Read with ReadWrite|Delete sharing
and a262144-character bounded StreamReader tail; local PowerShell7.7 held Write+
ShareRead test on600017-byte file PASS. Linux uses a262144-byte seek/read tail; held
writer test PASS. All live prewarm/pilot/controller log reads reviewed; future AMD
progress monitor uses the same bounded shared reader. Source check repeated PASS107
with CPUcap1. Its initial inherited priority reset was corrected by an owned-only
absolute19 watcher; receipt records each correction rather than claiming19 from start.

Primary timing now excludes still readback/writes, which can inflate the following
wall-frame sample; separate image controls establish effect. Both use1200 excluded
PREPARATION frames, explicit installed full-quality and actual held route coverage.
An actual3839x2160 Linux result will be labeled near4K, with no invented normalization.
No owner display/system change is planned. All-dry top-down control is prepared.

Rigcull released garbage early at~02:50Z. Read-only independent water preflight then
found no lock/viewer/plugin and native libraries; actual remote home is /home/trash,
Python3.14.8. Linux matrices corrected to that home before reservation. Packages ready;
water will reserve only for the prepared signal pilot, then cost only after positive ROI.


## 2026-11-20 03:18 UTC current acceptance state

Implementation and both packages are reviewable; feature NOT READY. No accepted
pure-timing cost on either box. Production source remains e420; later edits are only
own benchmark helpers/report/evidence. No generated SPIR-V header is committed.

Linux positive image proof is now actual3840x2062 (7.918MP), six96-frame averages.
All six held stages have BASE.n==GPU.n==241, FEATURE/SPLIT, settled pipelines and
zero injection failures. Lit-floor RMS ON minus OFF pair6.771996DN versus OFF/OFF
0.00049885DN; roof-interior0.00054167 versus0.00043773, p95=0. All-dry top-down
platform6777680-pixel interior RMS0.00008427DN versus AA0.00013945, p95=0. These are
phase-averaged controls, not bit-identical lit stills. Roof/dry behavior is within
the observed noise floor. Crest/trough ambiguity remains unqualified. Full proof and
raw report/still hashes are in doc/perf/water-caustics/linux-pilot-proof.json.

The raw SLSTILL1 rows are flipped vertically relative to PNG coordinates despite
the existing source comment:1000 seeded pixels agree after flip within0.5DN
quantization, versus49DN mean error unflipped. Only our ROI reader was corrected;
no unrelated benchmark/rendering source fix is included.

Garbage lease02:55:46Z was restored/released03:03:22.509Z. Receipt records PASS for
all7 owner states (settings/dialog/runlog/current link/3helpers), no viewer/campaign,
and owned lock release. A prewarm-original completed installation in575ms then our
abort beat its settings receipt; it was correctly rejected, finally restored. This
is a helper-ordering failure, not a renderer crash or accepted cost. Fresh later
verification found a foreign newui-windows lock/viewer; current6/7 states matched
and run.log differed during that foreign block. It was left untouched. No retry
or old-window extension occurred. Old03:23:46 window is VOID.

Both helpers now reject retired prewarm rows before launching, run finite normal
cases to completion, and never send abort or live-setting pulses. New first-cost
matrix is exactly same candidate binary OFF/ON/OFF, under pose only, no still I/O,
20s PREPARATION plus6s held. OriginalA comparison follows only after first primary
cost. Actual Linux controller-loop CPU test PASS3 normal completions/39 setting
reads with deliberately delayed readiness, no abort/set/viewer/GPU. Local PowerShell
parser/production schema/source/real READY/bounded held-writer test PASS3 rows.
No display experiments are in the next matrix; actual dimensions will be reported
without normalization. First-frame logs show normal indices-1; resolved-binding
receipt remains an explicit qualification limit, not proof of missing or valid
sampling. Future cache paths also explicitly isolate XDG/NVIDIA driver caches.

Queue: garbage newui-windows -> pascal alpha subset-OIT -> water, no gap grabs.
AMD rigcull RT diagnostic first; water declined an unproven5-minute squeeze. Both
boxes currently unreserved by water. Parent has a request for a later coordinated
normal-completion timing slot after the priority work. No owner-local GPU/viewer,
foreign signals, push or merge. Canonical parent report remains untouched.


Local follow-up03:29Z: normal timing paths now use a separate normal-paths directory;
both runners validate the actual staged path before launch (no still keys,20s
PREPARATION, finite held segment). This prevents an old image-pilot path being reused
silently when BenchStillRaw0. Actual Linux normal-loop CPU test and extracted exact
PowerShell timing-phase block both PASS. Cached pilot reports confirm focused0 for
all six held segments; selected device is NVIDIA GeForce GTX1080,8GB, driver580.178.04.
Windows overlay inherited TESTCTL=OFF from its original-A build template; CLI replay
is valid without the socket, no Windows live-API claim. Linux fullB is TESTCTL=ON.
No unrelated BenchGpuTimers change is cherry-picked. Proposed later normal-binding
quality control is flat RenderVKWaterWaves0 with the known nonflat EEP texture; it is
prepared locally only and follows first accepted pure timing. No new production hunk.

## 2026-11-20 04:00 UTC conditional AMD timing and restoration

Feature remains NOT READY. Production source remains e420; both frozen candidates
identify c5449. No production change, new heavy build, push or merge in this block.

The first three normal AMD runs with20 simulated seconds of preparation were
correctly rejected: full-quality installation was absent, BASE.n=0, and only the
basic placeholder UBER route ran. Their roughly4.5ms numbers are not caustics cost.
The existing replay readiness predicate omits the upgrade group. Rigcull is working
on that separate benchmark issue; this branch does not import its renderer edits.

One authorized longer-preparation retry completed all three normal rows, with no
aborts, original-A rows, display changes, still I/O or setting pulses. It used300
simulated seconds/18000 excluded preparation frames and360 held samples per row.
Actual native render extent was3840x2160,8294400 pixels, on RX7900XTX. All three
installed full-quality pipelines, ran FEATURE_SPLIT, and had BASE.n==GPU.n==360.

| Candidate arm | Total GPU average, ms | Wall frame average, ms |
| --- | ---: | ---: |
| OFF0 | 6.042180 | 6.263326 |
| ON1 | 5.846147 | 6.073632 |
| OFF2 | 5.875339 | 6.108765 |

ON minus the mean of the two OFF controls is-0.112613ms GPU/-0.112413ms wall;
OFF/OFF GPU spread is0.166841ms. These are **conditional route-qualified timings**,
not an established caustics cost or a speedup. AMD has no positive ON image/ROI yet;
enabled1, actualLz0.932738 and Linux positive ROI cannot prove the AMD effect was
active. Initial normal indices remain-1; resolved texture sampling is also unproved.
The analyzer keeps budget_ready=false. See doc/perf/water-caustics/amd-primary-timing.json
for complete statistics, identities, raw hashes, cache/install logs and limitations.

CacheLocation and NewCacheLocation were identical in all three rows:
C:/sltest/sol-water-caustics-d55b3be9a1/cache. APPDATA/LOCALAPPDATA and the settings
template hash stayed the same; the runner copies settings XML without purging home
or cache. A loaded cache was not a warm-variant guarantee: OFF0 recorded36 upgrade
misses and installed52 full-quality pipelines65.601s after init; ON1 recorded52 hits,
zero misses and installed after183ms. The longer preparation excluded this work.
Pending/miss and installation receipts are preserved rather than inferred from the
generic settled flag. Local CPU controller tests passed at both20 and300 seconds;
the exact production PowerShell parser/schema/phase/source/READY/held-writer checks
passed before reservation. Optional lease expiry is capped before atomic reservation.

Fresh AMD reservation was observed03:52:44.661Z with the unchanged ORIGINAL hard
deadline04:11:04Z. Last row exited normally03:58:23.915Z. Independent LOCAL watcher
verified taskReady/no viewers/plugins, restored all7 saved owner states and released
the owned lock; exact completion receipt03:58:41.089231Z. Parent and rigcull received
the handoff. No current water reservation or extra quality rows were appended to
the finite controller. Two same-B normal ON/OFF96-frame image controls are prepared
and schema/package-validated LOCAL for a later coordinated AMD slot after rigcull.

The regionless fixture does not establish sea[4].y/shore-field validity. Open/default
sea evaluation likely skips the4 SSBO shore taps. Actual valid-field cost, real-region
bathymetry, original versus amended-OFF overhead, default-ON dry-ground cost, positive
AMD ROI, first accepted garbage timing and midrange4K budget remain unproved. Flat-wave
normal-map proof follows the first cost points on both boxes if still needed. Actual
RT-on, moon, rigged receivers, fullbright/specular controls, unfrozen/upscaler quality
and crest/trough dry rejection also remain open; CPU stereo checks are not real VR.
Garbage queue remains foreign newui -> alpha subset-OIT -> water; no gap reservation.

## 2026-11-20 04:27 UTC Windows artifact rejection supersedes AMD checkpoint

All AMD candidate cost and quality evidence above is **INVALID as caustics evidence**.
This was our packaging omission. Header parity and renderer/appviewer object/link
checks did not prove that the executable contained the new lighting modules.

The exact new BASE/COMBINE/UBER uint32 blobs, both mono/G11, are absent from the
frozen50bf Windows PE. All6 old blobs are present in B and original A. The renderer
dependency file correctly names our generated header: include ordering was not the
cause. The actual desktop module owner is llcleanspecvk.cpp, which the overlay did
not rebuild. llcleangivk.cpp also owns two changed VR/GI blobs. The complete changed
array scan finds4/25 new blobs (renderer-owned basic placeholders and VR COMBINE)
and21/25 old blobs (19 llcleanspec and2 llcleangivk). Actual bytes, word counts,
offsets and hashes are preserved in windows-pe-all-changed-invalid.json; shader
owner references are in windows-shader-owner-proof.json. Existing runtime logs
have no SPIR-V hash/word-count receipt, so none is claimed.

AMD quality ON/OFF both completed normally at3840x2160 with96-frame means and
BASE.n==GPU.n==361, but the56000-pixel floor ROI and182000-pixel roof ROI have exact
zero raw difference. This is consistent with the wrong modules and is not a
quality pass. The two-row block restored all7 states and released its lock at
04:14:08.248Z, then handed AMD to rigcull. Raw rejected images remain preserved.
The cost receipt and Windows candidate receipt now explicitly mark invalid status.

The repair helper now discovers every changed-array owner, pins its source and
generated-header dependencies, and requires all25 new blobs present/old blobs absent
in the linked PE before packaging. It can reuse only our hash-pinned renderer and
appviewer objects, whose4 new blobs/hash header are correct, then compile the two
omitted owner objects serially atj1/nice19 after the fresh <=2 Ninja gate. This is
still an isolated actual-object/relink overlay, not a full configured Windows build.
The actual invalid PE is a negative regression test: the new gate rejects it.
No production shader/log/sea-clock change or repaired-build pass is claimed here.

Linux full-build B is unaffected and retains its positive image proof. Garbage's
alpha release was observed04:16:22Z; an immediate later preflight found a new foreign
NEWUI lock/viewer and aborted before reservation or mutation. After explicit later
NEWUI handoff, fresh idle checks and atomic backups passed. Water reserved exactly
04:21:23.868Z (23:21:23.868 local), hard deadline04:49:23.868Z, target below25 minutes.
The normal finite OFF/ON/OFF pure timing campaign is active, with120 simulated
seconds preparation per row, no stills, aborts or display changes. Independent LOCAL
watcher916879 covers restoration. Package verification passed7621 entries including
the two matched native-library substitutions. Full quality installed606ms after init;
actual extent3840x2062. OFF0 held coverage360/360 is valid, GPU19.443594ms and wall
19.591666ms, but no ON/OFF cost conclusion yet. Restoration/handoff takes priority
over lengthy final analysis. No heavy Windows repair build is running during this
primary campaign. Feature remains NOT READY; original task continues.

## Corrected evidence checkpoint, 2026-11-20 05:07 UTC

**NOT READY.** Both boxes now have valid active-effect performance points. Do not
use any runtime result from Windows50bf: that package linked old desktop modules.
The repaired fa9d package passed all25 exact-new-present/old-absent PE checks,
55378 protected baseline hashes,1157 reused link-input hashes and7545 unchanged
static files. Its sealed catalog has7549 entries. Build was an isolated serial
two-missing-owner compile/relink with hash-pinned own renderer/appviewer reuse,
not a full configured Windows build; j1/nice19, exit0, zero priority corrections.
Production rendering source remains e420; no math change was inferred from the
packaging error. Local helper/invalidation fix is d6423b58ab.

| Box / actual native extent | OFF0 GPU | ON GPU | OFF2 GPU | ON minus OFF bracket | OFF/OFF spread |
| --- | ---: | ---: | ---: | ---: | ---: |
| GTX1080 /3840x2062 |19.443594ms|19.463250ms|19.020660ms|+0.231124ms|0.422934ms|
| RX7900XTX /3840x2160 |5.886260ms|6.004097ms|5.900506ms|+0.110714ms|0.014246ms|

Wall-frame increases are+0.223575ms Linux and+0.098945ms AMD. Every timing row is
FEATURE_SPLIT with BASE.n==GPU.n==360 and no still I/O. No resolution normalization,
summed pass-percentile estimate, universal live-cost claim or midrange-RX claim.
Linux's larger A/A spread prevents robust well-under0.5ms acceptance from that point.

Corrected AMD image controls each have361/361 BASE coverage and96-frame4K means.
The56000-pixel lit-floor ROI shows3.336497DN RMS /3.044784 mean absolute difference,
with a visible focus pattern absent in OFF. The182000-pixel roof ROI shows0.150317DN
RMS /0.290230 p95: unresolved without AMD A/A, **not a shadow-quality pass**. The
initial automatic100x-floor/roof contrast check was not an acceptance requirement;
its failed result and raw metrics remain preserved alongside the explicit image/
source/PE review. The active-effect evidence does not waive the roof control.

Linux restored all7 owner states/link/helper hashes and released04:29:48.073816Z
(23:29:48.073816 local), then handed garbage to pascal-alpha. Corrected AMD's5 normal
rows completed exit0; exact7-state restore/release verification finished
04:49:46.733110Z (23:49:46.733110 local), taskReady/viewers0. No extra AA, lease
extension, garbage reacquisition, current reservation, push or merge.

Evidence: linux-primary-timing.json, amd-corrected-primary-timing.json,
amd-corrected-roi-proof.json, windows-repaired-candidate-receipt.json and
windows-repaired-pe-proof.json in doc/perf/water-caustics. Each records source,
executable/input/raw hashes and the scope/limits. Existing exported figures are
under cache/sol-water-caustics/evidence; the old AMD plot now explicitly says INVALID.

The next bounded proposal is acceptance-plan.md:11 core AMD rows combine exact-A
disabled overhead, all-dry cost, roof image A/A and flat-wave normal proof. At most8
compact original-behavior rows cover direct-only materials, moon/RT/packed rigs and
unfrozen native/FSR, not a broad scenario grid. A separate coordinated Linux
confirmation is needed for the noisy mid-GPU point. Source/CPU stereo checks pass;
real headset/club/artist-preset sweeps remain follow-up coverage. Near-waterline
dry-side error bounds remain a release gate; the current approximation is not an
exact horizontally displaced-surface test. No new box work without coordination.

## Conservative dry-side repair in progress

All GPU cost/image points above are **previous e420 rendering source**, not
acceptance for this repair. The old inverse-free world-XY receiver query has a
reproduced dry-side correctness failure. A separate CPU lower-envelope certificate
now publishes reserved sea[6].w from selected GPU field data or successfully
published flat LL planes, with invalid/unknown inputs disabling caustics. It
removes the per-pixel amplitude loop and second wave evaluation. No existing sea
outputs, wave amplitudes, LOD or foam change. See dry-side-bound.md for proof and
quantified conservative shallow exclusion. Renderer C++ layout changes; rebuild
every dependent object and verify actual final shader blobs. No boxes held.

Repair validation:108 source checks PASS;11 shader variants compile/validate,
including BASE/COMBINE/UBER and frozen two-eye specialization. All7 nonconsumers
remain raw-byte identical to original baseline, including water waves/fragment
and RT forward. Dependency builds and final executable blob checks remain pending.

## Clang finite-guard regression: source/artifact acceptance withdrawn

ffd4598508 GCC checks and Linux final-ELF blob proof passed, but an added actual
Clang O3/fast-math execution of the production fixture fails: nonfinite wave
accepted. Exact before source/command/output are preserved under cache
finite-guard-clang-repro. A volatile INTEGER load of exponent bits fixes the
optimizer fold in both GCC/Clang O3 trials; no volatile floating arithmetic.
The selected GPU-published field/flat instance publication path was independently
re-audited and is unchanged. Full checks/dependency builds/final blob checks repeat.

Garbage's coordinator-approved image pair completed normally, new-sourceffd
floor RMS6.796516DN and BASE361/361 at3840x2062. This is historical active-signal
evidence, not final corrected-source acceptance. The current originalA timing
row completed normally; own STOP took effect before the next row, with no viewer
signal or abort. Exact restore/release06:01:58.171436Z (01:01:58.171436local), fresh
verify06:03:09.009274Z: all7states/hash/link and6file modesPASS, no viewer/plugin,
lock absent. No lease remains. Windows own Ninja1014329 was intentionally SIGINTed
for this correction: Ninja130, wrapper1; all verified own descendants drained.
No foreign process/output was touched, and no build success is claimed for it.

Corrected guard validation:108 source checks PASS, including both actual GCC16.2.1
and Clang23.1.3 atO3/fast-math. Actual project flags recorded: LinuxO3, Windows
O2/Ob3/fp:fast. GLSL remains exactly the validated ffd snapshot. Normal Linux then
owned-configured Windows dependency rebuilds/final executable25-blob checks follow.
