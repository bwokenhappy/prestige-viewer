# Astra shader compile investigation

**AMD addendum complete (2026-11-19):** 11 accepted RX7900XTX/Windows RT replays, matched packages and 5,560 per-pipeline records. Seven exact host states restored; own lock released at 17:04:05Z. The bounded-cache fix remains READY. Separate worker semantic-identity repair `ed54b448f0` is committed with a passing behavior regression and108 passing make-check tests; full viewer-build/GPU validation remains pending. It is not part of these measured packages.

The bounded cache-preservation fix is implemented and measured on GTX1080. When a live application cache exceeds 256 MiB, the original policy deletes the last useful snapshot; the fix retains it and reports a failed save. Three fixed fault-injection launches retained exactly the same valid seed and compiled in **2.334 / 2.332 / 2.314 s** spans. After original-policy deletion, the next launches took **153.210 / 152.231 s**. This fixes a demonstrated persistence failure; it does not establish the entire cause of the owner's first slow launch.

Assigned model: **gpt-6-astra**, as requested; runtime model not independently verified. Owned branch/worktree: `astra-shader-compile`, `agents/worktrees/astra-shader-compile`; fresh `forgejo/main` base `ae1f063eb7ca9c2c0e0bbec3921d2c21821e493a`. Local commits only; no pushes/merges, additional agents, owner-local GPU/viewer launches, or changes to the running owner's cache/environment. HERDR_ENV was absent; milestones and final delivery use the authorized native parent thread `01a18431-804f-7e02-9abd-a5f3036449d6` instead of guarded herdr/w1:p1.

**Coverage limits:** AMD normal persistence, cross-build reuse, supported RT and image/runtime controls are now measured below. Natural AMD oversize did not occur; repeated oversized preservation remains demonstrated by the production regression and Pascal controlled experiment. No measured owner RTX5090 fix result is claimed. GTX1080 does not provide supported RT evidence.

## Implementation and review

- `08ccbc65fe9147f73f193959cf810dcccd41995e`: preserve the last bounded main/spec/worker disk snapshot on oversize or save failure; reject oversize before the unchanged-size shortcut; do not advance saved-byte bookkeeping on failure. Separate live spec-cache footprint from persisted bytes for disk LRU/logging, and treat driver-rejected initial seeds as unsaved. Existing temporary-file/atomic replacement is retained. There is no cap growth, opaque-blob truncation, shader change, or version-check bypass.
- `50e7387bc2`: per-pipeline elapsed time, create-info identity, SPIR-V word count, cache feedback, concurrent memory attribution and batch memory in bounded replay JSON. `a4ed45ff1d` and `eb7f944546` close the discovered late-job warmup gap and initialize the timeout on replay restart. These measurement changes are also on the comparison baseline.
- Measured original-policy source: `ef70f524fe6a5f796bfc4c8442b6567629cdc77b`, owned branch `astra-shader-compile-baseline`. Measured fixed source: `eb7f944546ee9516abeb2450db812a623c225a43`. Later commits contain measurement tools/data/report only. All four embedded shader-header hashes match between packages; no generated `vk_*_spv.h` was committed.
- `c96c47e8c5` and the final evidence commit contain isolated host matrices, the size-query shim, numeric exporter and reproduction tools in `scripts/bench/compile_latency/`.

The regression compiles the actual production load/save implementation with stub driver returns and real temporary files. It fails against the original deletion and passes the fix. Coverage includes repeated oversized attempts/reloads, cross-build seed reuse, worker-like bookkeeping reset, incomplete/read/open/close/replacement failures, successful bounded replacement, temporary-file cleanup and no previous file. **make check: 107 passed; Linux package checks: 4 passed.** No platform-specific replacement implementation changed. Windows baseline/fixed packages and normal persistence now execute successfully; see the AMD addendum.

## Controlled GTX1080 results

Actual driver **580.178.04**, Linux, eight compile threads, RT/GI/RT reflections off, WBOIT mode 2, fixed complete offline replay and 366 timed frames. Requested window 1280×720; actual reported extent 1280×721. Two 120-frame held views supply A/A and A/B image/noise controls. All accepted corrected runs are settled, focused, have zero compile failures/dropped records, and finish reported pipeline batches before the timed interval.

“Span” is first batch start to last batch end. It includes scheduler gaps and is not the sum of overlapping batch walls. “Sum” adds individual creation-call elapsed times; it is not CPU time. Process wall includes startup, replay/warmup/holds and shutdown. Peak RSS is the child-process high-water measurement; concurrently sampled per-pipeline deltas in the CSV are attribution estimates, not isolated allocations. App-cold means an empty isolated viewer cache. Normal runs request NVIDIA driver disk cache off; the separate enabled controls verify actual owned cache descriptors.

| Normal comparison | Compile span s | Summed calls s | Peak process RSS MiB | Process wall s |
|---|---:|---:|---:|---:|
| Original cold ×2 | 199.319 / 200.148 | 1145.677 / 1143.915 | 5625.9 / 6261.3 | 238.140 / 238.523 |
| Fixed cold ×2 | 199.523 / 199.767 | 1137.524 / 1141.606 | 5602.6 / 5593.7 | 236.616 / 238.171 |
| Original first warm | 34.416 | 201.213 | 3365.6 | 68.506 |
| Fixed first warm | 34.474 | 200.618 | 3346.7 | 69.537 |
| Original fully warm repeats | 2.321 / 2.279 | 0.750 / 0.709 | 2549.2 / 2551.3 | 36.503 / 36.529 |
| Fixed fully warm repeats | 2.316 / 2.362 | 0.739 / 0.862 | 2508.0 / 2539.3 | 36.824 / 36.918 |

No normal cold-start speedup is claimed. The first warm launch requests **13 additional particle-specialized pipelines**: their generic handles did not exist soon enough on the cold launch. This changes 370 recorded items into 383; the next fully warm launch hits all 383. Do not compare runtime from those different sets as an optimization A/B.

| Same-seed oversize experiment | Compile span s | Summed calls s | Application hits/items | Peak process RSS MiB | Main file after exit |
|---|---:|---:|---:|---:|---|
| Original launch 1 | 2.335 | 0.822 | 383/383 | 2559.1 | absent |
| Original launch 2 | 153.210 | 1010.020 | 152/370 | 5631.1 | absent |
| Original launch 3 | 152.231 | 1001.954 | 151/370 | 6316.4 | absent |
| Fixed launch 1 | 2.334 | 0.758 | 383/383 | 2521.3 | exact seed retained |
| Fixed launch 2 | 2.332 | 0.762 | 383/383 | 2501.1 | exact seed retained |
| Fixed launch 3 | 2.314 | 0.783 | 383/383 | 2505.1 | exact seed retained |

Seed: **43,670,085 bytes** including the 192-byte application header, SHA256 `2fc8548626a3adef16d85e32d7ccfdeb5f3a03d23b26167ad8c818fb98c2f437`. Exact bytes were restored before each policy's experiment. The shim makes successful size-only `vkGetPipelineCacheData` queries above 24 MiB report 256 MiB + 1; real payload reads and pipeline creates are unchanged. This is explicit size-query fault injection, **not natural Pascal oversize growth**, a simulation of 256 MiB memory consumption, or driver eviction. Original launch 1 injected 97 queries but warned only once because the rejected size was subsequently treated as unchanged. Fixed attempts consistently fail without deleting the seed. Original following process walls were 190.225/190.627 s; fixed three were 37.018/36.733/36.667 s.

Cross-build control: the fixed executable loaded the original-policy seed, kept its main file byte-for-byte and matched all **383 (batch, name, create-info hash, SPIR-V-word-count)** identities. Span 2.297 s; summed calls 0.837 s; 383/383 feedback hits. Current build-tag sharing works in this measured case.

## Runtime and image controls

Fully warm matched GPU means: original **28.085 / 28.056 ms**, fixed **28.075 / 28.058 ms**. Fixed oversize repeats: **28.055 / 28.072 / 28.065 ms**. This small sample shows no runtime regression in this Pascal replay, not a universal performance guarantee. The fix changes persistence/accounting only; shader bytes and measured pipeline identities match.

Mean held-image luminance error (0–255 scale), two views: A/B **0.002039 / 0**, versus original A/A **2.701978 / 0.066732** and fixed A/A **0.014520 / 0.013000**. Temporal mean standard deviation: original reference **1.104590 / 0.934400**, fixed **1.104380 / 0.934400**, with repeat variation recorded. No image/noise regression is detected. Numeric comparisons are published; private captures/images, settings and raw logs are not.

The initial five pilot runs remain compile evidence only. They exposed a replay readiness bug: warm main compilation finished before the first scene frame queued late specialized work. Their timed runtime/images are excluded. The corrected gate restarts the 300-frame warmup countdown while pipeline work is pending, with a 900-second bound and explicit timed-start timestamp. Both measured packages use it.

## Driver, worker and retained-subset controls

With the **application cache empty on every launch**, isolated NVIDIA disk-cache retention produced:

| Driver cache state | Compile span s | Summed calls s | App feedback hits/items |
|---|---:|---:|---:|
| Empty | 200.375 | 1144.853 | 128/370 |
| Retained, first reuse | 34.323 | 203.207 | 126/383 |
| Retained, fully populated | 2.909 | 6.183 | 130/383 |

The first reuse adds the 13 particle variants; the third launch has a populated driver cache for them. All three viewers had actual `.bin`/`.toc` descriptors exclusively inside the owned driver-cache directory. The first two disk totals were 58,787,869 and 67,279,533 bytes. This demonstrates useful **driver-side reuse without many application-cache feedback hits** on Pascal. Conversely, owner logs show application hits alongside expensive work. Neither signal alone answers the other cache's effectiveness. These controls do not prove owner-driver eviction, a numeric driver limit, or the same behavior on 615.78.08.

Forced-worker warm runs launched the actual worker, loaded the 42,646 KiB main seed, and produced 52/52 hits. Viewer reuse matched all 52 create-info hashes, with no same-identity misses. First worker batch: 0.209 s wall / 0.181 s summed calls. Whole-viewer spans were 2.641/2.775 s, sums 0.810/0.866 s, and 383/383 hits. Sampled process-tree RSS peaks were 2508.6/2512.7 MiB (250 ms sampling); this differs from an exact aggregate commit high-water measurement.

A subsequent in-process launch merged the saved worker file with forced oversized save queries: both the main and worker files were retained byte-for-byte. Span 2.324 s / sum 0.743 s / 383 hits. The worker file is consumed only after a successful main save, so failed persistence cannot silently discard the merge input. No recipe-version bypass was needed.

A real **partial main snapshot** captured during a cold run was 40,635,031 bytes, CRC-valid, SHA256 `a65680cfeb5a3689f9cd15b7d5ce48b7611a7ed79efd9f0e70fe45771e15f50b`. With the same complete seed directory but this main file substituted, two forced-oversize launches retained it exactly. They yielded **360/383 hits** each, spans **72.021 / 70.974 s**, sums **384.489 / 379.428 s**, and peak process RSS **3836.9 / 3826.8 MiB**. Thus a retained subset remains useful, but its **23 misses recur** because the enlarged cache cannot be saved. This is a partial-snapshot test, not a demonstration of arbitrary obsolete-shader eviction. Preservation prevents destructive cold restarts; it does not compact an opaque cache, eliminate all future misses, or guarantee low compiler memory on AMD.


## Owner evidence: the old build is slow today too

The owner's same-day observation that the v1.5.0 release is equally slow or worse than current is central evidence against recent variants alone. Read-only inspection of the selected release package identifies **Release 1.5.0.7**, although the owner described 1.5.0.6. Its exact source ancestry has not been established. The `jetstream-1.5.0.6` tag does not reproduce all features observed in the running package, so attributing that executable's behavior to the tag would be unsound.

Private-safe log chain on RTX 5090 / NVIDIA 615.78.08, UTC 2026-11-19:

| Event | Evidence |
|---|---|
| Current session, 11:08:17 | Main cache loaded **210656 KiB**. |
| Main batch, 11:09:00 | **261 pipelines, 196 compiled + 65 dedup; 40.234 s wall, 249.039 s summed compile calls; 8 threads; 1304→4877 MiB process memory**. Application feedback: **202 hits, 59 misses**. |
| 11:10:42–11:13:37 | **40** over-cap warnings, rounded sizes **256–295 MiB**. The save implementation explicitly removes the main file on this path. |
| Last current spec save, 11:13:53 | Per-key spec file **217661 KiB** saved separately. |
| Old-package start, 11:20:48 | Main cache reports **no file**; the separate **217661 KiB** spec cache is retained across the build change. |
| Old-package main, 11:21:31 | **261 pipelines, 196 compiled + 65 dedup; 35.261 s wall, 246.677 s summed calls; 8 threads; 951→4858 MiB**. |

This establishes a concrete application path capable of destroying the seed between launches, with the next launch observing a missing file. It does not prove the app deletion explains every second of the first session: that session was already slow with a loaded cache and many application-cache hits. Hits, elapsed creation time and driver compiler-cache reuse are separate measurements. Batch wall windows overlap and must not be added. Later old-package batches also waited on the shared scheduler; queue wait is not compiler execution time.

The log calls this memory “committed,” but the source uses private commit on Windows and RSS on Linux. The owner's Linux values are therefore RSS. [Vulkan's feedback definition](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCreationFeedbackFlagBits.html) describes a readily usable application-cache entry and recommends reporting hits when most creation work was avoided. It is not a guarantee of zero creation cost, and aggregate hits do not remove the expensive misses in the same batch. Driver-internal cache reuse is a different signal.

Historical controls on October 30, same reported GPU and driver, were much faster: one main batch rejected the application cache because the viewer build changed yet completed **287 pipelines / 206 compiles / 81 dedup in 0.764 s wall, 2.702 s summed calls**; another warm main completed in **0.069 s wall / 0.486 s sum**. These are uncontrolled historical comparisons with different shaders/settings. They are consistent with useful driver-side reuse, but do not establish a driver cache hit or isolate a causal change.

## Three independent persistence layers

| Layer | Identity / location | What invalidates or limits it |
|---|---|---|
| NVIDIA compiler disk cache | Actual running viewer has open `.bin`/`.toc` files in `~/.cache/nvidia/GLCache`; loaded `libGLX_nvidia` and `libnvidia-gpucomp` are 615.78.08. | Driver-specific data; environment/cache-size policy is separate from viewer files. Eviction on this owner has **not** been demonstrated. |
| Application `VkPipelineCache` | `vk_pipeline_cache.bin`, separate `vk_spec_<key>.bin`, optional worker cache, inside the viewer cache directory. Header records GPU, driver, UUID, CRC, size and build tag. | Current loader rejects wrong device/driver/UUID/corruption; build tag is diagnostic, not a validity check. Per-file payload cap **256 MiB**. Spec cache also has a separate LRU budget. |
| Compile-worker recipes | Serialized SPIR-V, layouts, pipeline state and device request used to replay creates in a worker. | Format and device checks; format 2→3 for private wave32 flag, 3→4 for premultiplied blend. Rejecting recipes does not itself invalidate `VkPipelineCache`. |

The current main-cache loader seeds the device from the retained file. A worker reads that file as a read-only seed and merges it with its own cache. On the next viewer launch, worker results are merged into the main cache; saved-size bookkeeping is reset, and the worker file is consumed **only if saving the merged main succeeds**. Preserving the main file and returning failure on every oversized attempt retains both bounded inputs. A stale subset can still help unchanged pipelines; new/changed entries may continue to miss when no bounded full snapshot can be written. This fix does not implement entry eviction inside an opaque driver blob.

The original spec save path confuses live serialized size with bytes saved on disk, even after failure. The accompanying correction separates resident footprint from saved bytes so disk LRU accounting and log messages describe the retained snapshot accurately.

`30d39f3631` (November 17) bumps recipes 2→3. `8d353353b7` (November 18) bumps 3→4. Both are required compatibility changes; suppressing the version checks would let an old worker misinterpret state. NVIDIA normally uses in-process compilation; AMD proprietary defaults to the worker. Thus recipe churn alone does not explain both old and new owner binaries being slow today.

## Environment audit, without changing owner state

The loaded module and libraries confirm **615.78.08**. Package history records **615.71.09-1 → 615.78.08-2 on October 24 at 19:03:22 EDT**, following 610.43.02→615.71.09 on October 5. A driver update today is not supported by the evidence. Kernel module build date is not installation time.

No relevant overrides were present in the inspected owner viewer environment: `__GL_SHADER_DISK_CACHE`, `_PATH`, `_SIZE`, `_SKIP_CLEANUP`, `XDG_CACHE_HOME`, `SL_VK_PIPELINE_WORKER`, `VK_DRIVER_FILES`, `VK_ICD_FILENAMES`, `SLIPSTREAM_HOME`. The active GLCache directory contained hundreds of MiB; an aggregate snapshot was about 809 MiB and the largest file grew from roughly 774 to 785 million bytes during observation. The legacy `.nv` location contained ComputeCache, not GLCache. These facts establish location and activity, not eviction or a particular limit.

Later read-only metadata: the active `.bin` and `.toc` inodes were created **November 18, 06:36:48 EST**. At November 19, 07:13:23 EST, the binary file had grown to **844,152,426 bytes**, the table to **202,304 bytes**. This is distinct from the October 24 driver installation. The reason this particular cache bucket was created, any earlier eviction, and any in-place truncation remain unproven.

[NVIDIA's 615.78.08 documentation](https://download.nvidia.com/XFree86/Linux-x86_64/615.78.08/README/openglenvvariables.html) describes the XDG/HOME GLCache locations, reuse of an existing legacy `.nv/GLCache`, driver-specific binaries, and the risk of mixed application-specific cache-size overrides reverting a shared cache to a smaller default. That section is explicitly about OpenGL. Its size-variable entry supplies no numeric default, so this report does not assume a 1 GiB limit or universal Vulkan behavior. Actual owner file descriptors independently establish use of that location. No owner cache, environment, settings or running viewer was changed.

## Harness and variant attribution

Garbage's existing `valrun.sh` deletes `*.shaderbin` unconditionally. Its optional cold mode moves the viewer cache aside and restores it afterward. Current source reads/writes the Vulkan `.bin` files above; no current `*.shaderbin` persistence path was found. The wildcard deletion is not the demonstrated main-cache deletion. No owner helper was edited. A cache-preserving warm measurement must avoid both explicit cache purges and the over-cap deletion.

Recent source inspection finds desktop stereo COMBINE/LOCAL twins gated to multiple views; RT lighting groups gated to RT; late FG precipitation requested on demand. These are not blanket unused desktop login variants. WBOIT does build both modes even when one is selected; a selected-mode-only candidate was prepared but deferred in favor of the causal persistence fix. Tile resolve adds a compute pipeline and changes resolve work. Raw variant counts are not compile cost: measured per-pipeline durations and feedback are required.

Large spec ray-query kernels remain important, and the AMD addendum below now quantifies them; GTX1080 provides no supported RT evidence. No kernel split, sampling reduction, wave-mode change, shader-key weakening or recipe-version bypass is included without appropriate hardware and image/runtime validation.


## Cost attribution and choices deferred

The corrected original-policy cold run ranks the actual costly non-RT creates as follows. Times are measured with concurrent compiles; SPIR-V size alone does not predict wall time.

| Pipeline | Creation s | SPIR-V words | Attributed RSS delta MiB (estimate) |
|---|---:|---:|---:|
| Mirror rigged | 45.629 | 720281 | 164.2 |
| Mirror alpha | 44.938 | 720312 | 207.9 |
| WBOIT static accumulation | 43.320 | 727595 | 343.7 |
| Mirror rigged alpha | 41.525 | 720281 | 187.4 |
| LIT_ALPHA | 41.493 | 727195 | 229.9 |
| LIT_PROBE_DRAW | 41.210 | 720312 | 160.0 |
| WBOIT rigged accumulation | 40.988 | 727564 | 323.9 |
| Specialized plain blend | 40.232 | 727195 | 349.2 |

Cold batch summed calls: main 212.733 s, full-quality upgrade 509.685 s, WBOIT 208.053 s, spec 205.364 s; smaller post/cover/encoder/fog groups account for the rest. These overlapping groups cannot be added as wall time. In the pilot, the four unselected mode-1 accumulation creates alone summed 150.75 s even with mode 2 selected. A selected-mode-only WBOIT candidate was prepared but is not applied or claimed as validated: the owner prioritized the causal persistence fix, and AMD/RT image/runtime validation was unavailable.

History links the relevant mechanisms, without attributing measured cost deltas to unmeasured old versions:

- `0ae81a7445` (October 2): initial persistent cache already deleted an over-cap file. `4689a0c4e3` (November 3): retain cache across builds rather than rejecting the build tag. This permits useful reuse and also accumulation into the pre-existing destructive cap path.
- `dbb5172440` (November 17): compile worker uses the viewer's device identity. Already in this campaign's base. Its commit describes historical AMD duplicate compilation; that is an earlier author's evidence, not a new AMD measurement here.
- `e1d32740ed` and `30d39f3631` (November 17): AMD shadow-register changes / recipe format 3. `8d353353b7` (November 18): late weather / premultiplied blend requires recipe 4. Do not weaken these compatibility checks to save startup work.
- `83e32d25ab` (November 18): tile-based WBOIT resolve. `911c857c36`: stereo OIT. `b651af0d7d` / `9f995e729d`: layered split/feature lighting. Mono view-count keys retain the mono value; stereo-specific COMBINE/LOCAL work is gated to multiple views. A shader implementation change can still change a mono create identity even without adding a mono variant.

No unsupported RT twins, unnecessary stereo desktop batch, or unconditional late-weather login batch was demonstrated in the measured desktop configuration. Huge spec ray-query kernels are measured in the AMD addendum; splitting them or reducing quality remains unimplemented and would need separate validation. Learned hit estimates are a scheduling heuristic, distinct from Vulkan feedback and actual disk entries. Existing coverage checks and cold-cost learning remain unchanged.

## Reproduction, evidence and host disposition

Reproduction helpers and exact run matrices are under `scripts/bench/compile_latency/`. Public evidence is `agents/astra_shader_compile_data/runs.json` and `pipelines.csv`: **29 corrected runs / 11,016 per-pipeline records**, source/executable/input hashes, starting/ending cache manifests, timings, feedback and memory. Raw captures, full settings, control responses, images and logs remain private. The five pilot runs are retained privately for compile diagnostics, excluded from runtime claims.

Both builds used nice 19, TMPDIR `/home/bryan/.cache/slipstream-build-tmp`, parallelism 12, and waited for existing ninja activity. Only owned worktrees/packages were built. A packaged libdecor CPU-ISA mismatch was handled in the owned remote packages using the host-compatible library; no system library changed. Briefs/prior reports and owned `doc/CODING_RULES.md` were read; no AGENTS.md was found in the owned tree/ancestors, and the specified batch2 CODING_RULES path was absent. No other worker tree was modified.

The existing garbage `valrun.sh` **sync-mode functional smoke completed**, including successful login and its 240-second run. The actual owned viewer mapped Khronos validation; the helper requested synchronization validation. No `Validation Error`, `VUID-`, `SYNC-HAZARD`, `GPU LOST`, segmentation-fault or bad-shutdown markers occurred. Mapping the layer and requesting sync through the existing helper do not independently prove every synchronization feature was enabled. No validation layer or system setting was installed/changed. Final `make check` again passed **107** after tool/data changes.

**Garbage restored and released:** settings/dialogs/run-log hashes match original backups; `current` again has its exact original, initially dangling target `/home/trash/slipstream/rec-9c87eb076f`; temporary validation routing was removed. The temporary isolated credential-store copy was restored and the original owner store hash remained unchanged. No owned viewer/plugin processes remained before releasing only lock token `astra-shader-compile-ae1f063eb7-20261119`. Receipts are `host-restoration.json` and `smoke.json` in the owned evidence directory. Amdbox was unreachable during that initial phase; the later independently reserved AMD addendum is completed below. Host availability was reported to the parent so another worker can independently reserve garbage.

**READY for local review:** bounded preservation, repeatable Pascal before/after, unit/source/package checks, runtime/image controls, driver/worker/subset attribution and local evidence. **Remaining limits:** no natural AMD oversize or owner5090 fix rerun. AMD/Windows and supported RT measurements are now complete below. The selected-WBOIT and ray-kernel changes remain deferred, not shipped or claimed as measured wins. The owner's initially slow cache-loaded run is not fully explained by this persistence fix; driver eviction remains a hypothesis.

The canonical root report and its committed owned-tree copy are identical. Detailed numeric files are relative to the owned worktree, not the owner checkout. Native parent delivery is the routing receipt; no actual herdr submission is claimed.


## RX7900XTX Windows addendum — completed November 19

After explicit Hegel/owner handoff, independent idle/lock checks preceded an atomic owned reservation. Actual hardware: RX7900XTX; WMI driver **32.0.32015.7029**, viewer **AMD proprietary driver 26.10.1 (LLPC)**, Vulkan 1.4.359. These are AMD measurements, separate from Pascal 580.178.04 and owner NVIDIA 615.78.08. Driver installation chronology or disk-cache eviction on AMD was not measured.

Baseline `ef70f524fe6a5f796bfc4c8442b6567629cdc77b` and fixed `f76d349635e175e1e571d30394ffb96e4bb349bb` Windows packages passed 106/107 source checks and native cross-compilation/package staging. All four generated shader headers are identical. The baseline installer completed; fixed staging/ZIP completed without an unused second installer. Packages, executable and input SHA256s are in `agents/astra_shader_compile_amd_data/build-receipts.json` and `runs.json`. Build slots were coordinated and released before remote execution; no concurrent ASTRA ninja build was left running.

One complete offline replay, 81 armatures, zero skin/texture/slot injection failures, 1280×720, eight compile threads, RT/GI3/ReSTIR enabled, adaptive visibility enabled, RT reflections, FSR3.1, FG off and WBOIT2. All 11 runs have 366 focused timed frames, settled/complete telemetry, no failed pipelines or dropped records, no device loss/fallback deadline, and every compile batch ends before timing. GI tracing, shadow visibility, RT reflection, reflection tracing and mirror passes each have 366/366 coverage. An offline audit also confirmed every RT-reflection forward/selection subpass and zero UBER-fallback/cannot-trace markers in all eleven logs. No login or private avatar/chat details are published.

APPDATA, LOCALAPPDATA, both viewer cache locations and sound cache were isolated under the owned root. Owner caches and driver-cache settings were untouched. **Cold means app-cache empty, not driver-cache empty.** All timings below are viewer batch span / summed creation-call elapsed time; worker costs are separate. Windows memory is sampled private commit at 250 ms, not Linux RSS. Concurrent per-item memory attribution remains an estimate.

| Run, in execution order | Span s | Calls sum s | App feedback hits/items | Peak viewer / viewer+worker private MiB | Process wall s |
|---|---:|---:|---:|---:|---:|
| base_cold | 331.346 | 1344.799 | 153/494 | 9747.9 / 9747.9 | 358.555 |
| base_warm1 | 33.916 | 0.532 | 494/494 | 6159.0 / 7806.8 | 51.177 |
| base_warm2 | 23.131 | 58.328 | 494/508 | 6797.6 / 6797.6 | 40.725 |
| base_warm3 | 2.104 | 0.709 | 508/508 | 6271.9 / 6271.9 | 20.004 |
| fixed_crossbuild | 4.757 | 5.120 | 508/508 | 6502.5 / 6502.5 | 27.940 |
| fixed_cold | 2.267 | 0.359 | 156/508 | 6248.8 / 6248.8 | 19.732 |
| fixed_warm1 | 2.097 | 0.785 | 508/508 | 6241.0 / 6241.0 | 20.346 |
| fixed_warm2 | 2.121 | 0.759 | 508/508 | 6266.0 / 6266.0 | 20.303 |
| fixed_warm3 | 2.112 | 0.764 | 508/508 | 6281.2 / 6281.2 | 29.220 |
| base_driver_warm_cold | 2.314 | 0.363 | 156/508 | 6214.7 / 6214.7 | 19.572 |
| base_warm4 | 2.154 | 0.796 | 508/508 | 6263.9 / 6263.9 | 19.245 |

The first cold viewer peak from native batch accounting was 9756.7 MiB private commit; the sampled process peak was 9747.9 MiB. Cold replay readiness was 343.154 s. Fullwarm readiness was about 3.0 s. One fixed run spent extra wall time outside the compile/readiness interval; its compile span stayed 2.112 s. Process wall is not a compile-only metric.

**Causal control:** after earlier launches, app-empty original policy was also fast (2.314 s span / 0.363 s sum), matching app-empty fixed (2.267 / 0.359), with only 156/508 app feedback hits in both. Starting cache manifests are empty. Thus the first 331 s → later 2 s difference is not a preservation-fix speedup. Reuse outside the app VkPipelineCache, consistent with driver/compiler disk caching, explains the low miss costs; no direct AMD driver-cache hit counter/location/purge was measured. Conversely, first warm had 494/494 viewer hits yet a 33.916 s span while the worker did other costly work. Feedback is not proof that all startup compilation has disappeared.

The original first two runs contain 494 items; 14 more appear in warm2 and are present thereafter. All shared create-info hashes/SPIR-V sizes match; all later 508-item sets match exactly. The additional original app-empty control establishes the driver-state confound, and its subsequent warm4 supplies a second settled 508-item A/A reference. These two evidence-driven controls supplement the initial nine-run plan; no renderer rebuild or repeated Pascal campaign was needed.

**Persistence and cross-build seed:** the exact original warm3 seed was copied into fixed crossbuild: main/worker 65,621,484 bytes, spec 19,457,924 bytes. The driver accepted the main seed; worker seed merge retained 64,083 KiB. Fixed had 508/508 viewer feedback hits and 53/53 worker hits, with unchanged pipeline identities. No per-file cache exceeded the 256 MiB bound, and no natural oversize branch ran. This validates normal Windows writes/reloads and useful cross-build reuse, not AMD oversize retention. Existing Pascal forced-oversize and production save-failure tests remain the causal preservation evidence.

### Expensive supported-RT creates

First cold costs below are elapsed concurrent calls, not CPU time. Full per-pipeline timing, SPIR-V sizes, feedback, create-info identity and attributed memory are in the 5,560-row CSV.

| Pipeline | Creation s | SPIR-V words |
|---|---:|---:|
| Rigged RT WBOIT accumulation | 79.182 | 943191 |
| Specialized static RT blend | 73.318 | 943019 |
| RT shadow mask | 66.086 | 1044358 |
| Specialized static RT lit | 52.309 | 936163 |
| Specialized rigged RT blend | 50.053 | 942988 |
| Static RT WBOIT accumulation | 49.024 | 943222 |

Family sums: deferred RT 425.885 s, WBOIT generic+specialized 352.794 s, specialized ray-query 245.454 s, main 186.288 s, plain specialization 119.313 s. Batch windows overlap. Worker full-quality cold separately cost **93.385 s wall / 248.147 s sum**, 37 actual creates and 16 dedup twins. Viewer full-quality reuse then matched all 53 hashes and had 53 hits, 0.477 s call sum but 28.907 s batch wall with aggregated queue/budget waiting. Worker per-item costs are not retained by the existing host protocol; reported worker measurements are batch totals, not invented per-item allocations.

### Separate causal worker identity finding

On the first same-build warm launch, the early worker replaced 21/23 named modules, with **two changed**. It spent **33.872 s wall / 85.855 s sum**, 47 hits and six misses. The viewer still had 53/53 hits but **12/53 create-info hashes differed** from worker results; zero misses occurred with matching identities. Later repeats retained these differences but became cheap through cache reuse. Fixed first warm likewise had six worker misses, costing only 0.020 s wall with already-warmed driver state.

Source attribution: `moduleFromSpv` and spec module creation call `GMDXRezFx::wireModule`, replacing Frag/FragBlend SPIR-V with wire twins while recording the plain short names. `worldRespin` maps those short names back to plain blobs, and `vkPipelineWorkerStartEarly` requests respin even on the same build. Actual AMD logs show `rezfx_wire=1`; recorded word totals match the wire twins, each 193 words larger. The two changed modules and affected alpha/probe/mirror identities agree with that source path. This is semantic module selection, not hash collision or driver rejection.

Separate local repair **`ed54b448f0c90a2ec0f2b0cb19e3d83541e61da2`** is prepared within the **original owner compile-investigation scope, with coordinator guidance**; this is not a claim of new owner authorization. It is independent of measured cache-fix/evidence tip **`5a6373627035d362314dde16bd07c763fe4e0994`**.

The repair returns explicit semantic names (FragWire, FragBlendWire and the other five wire twins) from the existing selection function, and records those names in viewer/spec/runtime-swap module metadata. Worker respin resolves them through the same wire-module table. Content/SPIR-V and create-info hash implementations are unchanged. Diagnostic labels remain in their previous form so persisted pipeline-quarantine entries still match; learned costs retain the same content keys. No shader source/generated-header changes are included.

**Recipe 4→5 has a one-time cost:** version-4 module names are ambiguous, so both the early-worker gate and worker reader reject them. The current viewer regenerates version5 through the in-session recipe writer; that first launch cannot use the old saved recipe for early compilation. Main/spec/worker **VkPipelineCache blobs stay valid and are not deleted**. Subsequent early launches use the explicit semantic names. This trades one recipe regeneration for correct persistent selection; it is not a cache-version bypass or a claim that driver compilation becomes free.

The focused behavior regression compiles the actual production selection, spec/runtime-swap, respin, diagnostic-label, header writer, early/read gates and recipe-save functions, with distinct stub SPIR-V/device dependencies. All seven wire variants roundtrip to identical bytes; plain and unknown cases retain their behavior; old diagnostic labels survive. A negative run against pre-fix source fails exactly at `refreshed==recorded`. Fixed tests reject current-v4, truncated and bad-magic recipes, regenerate/read v5 and leave real temporary main/worker cache-file bytes unchanged. This checks recipe regeneration helpers; full viewer orchestration remains untested.

**Validation limit:** focused regression passes and final **make check:108 passed** on frozen sources. No full Linux/Windows viewer compile, package, post-fix cache-hit timing, or GPU/image validation has run for this separate commit. Heavy build slots and released hosts were not reacquired. The AMD measurements quantify the wire-enabled bug; the earlier Pascal worker-match evidence remains valid for its different configuration. They do not establish a measured33.872-second saving from this candidate without full viewer/GPU validation. The cache-preservation fix remains separately READY.

### Image/runtime controls and host restoration

Fully warm baseline GPU means were **11.7390 / 11.5993 ms**; fixed **11.6785 / 11.6671 ms**. Fixed lies within baseline repeat variation. These short repetitions establish no systematic regression, not a speedup claim or broad confidence bound.

Two 120-frame held views: mean luminance A/A error **0.612226 / 0.200195** (0–255 units); A/B fixed warm2 **0.565358 / 0.213141**, fixed warm3 **0.601946 / 0.185941**. One warm2 view is modestly above A/A; the repeated candidate is below it. Mean SSIM is 0.997876–0.999597; detail/noise metrics show no systematic directional loss. Temporal noise mean std spans 3.707–3.732 and 2.975–2.981 respectively. Images are not bit-exact; private means/variance files are retained, with anonymous numeric receipts committed. No shader equations or selected render paths changed in these builds.

**AMD restored/released at 17:04:05Z:** all seven original settings/dialogs/job states hash/existence verified, including two absences; no viewer/plugin/owned executable processes; SLRun Ready. Only own BOXLOCK was removed. Receipt `C:\sltest\astra-shader-compile\out\RESTORED.json`, public numeric copy `agents/astra_shader_compile_amd_data/host-restoration.json`. Parent and queued Sol worker received explicit native release; no further AMD contact is planned. Direct ps1 invocation initially hit the existing execution-policy restriction before mutation; the reviewed script ran through the existing stdin PowerShell channel without changing system/security policy. Garbage remains released from the prior phase; no Pascal repeat or owner-local GPU use occurred.

**READY:** existing bounded-preservation fix plus completed AMD normal-path/RT coverage. Natural AMD oversize, owner5090 fix runtime, and the separate semantic-worker repair remain outside measured coverage. No unsupported Pascal RT or unmeasured kernel/WBOIT optimization is claimed. HERDR_ENV was absent; native parent delivery is used, not an actual herdr submission.


## Final source-repair disposition

The separate worker identity regression passes before/after controls. An initial full source-check run passed107 and hit a test-harness snapshot inconsistency (the loaded extractor preceded the new diagnostic-label assertion); the frozen-source rerun completed successfully: **make check108 PASS**. The standalone original-source negative control still fails at the intended wire/plain byte inequality. Full viewer/Windows compile and runtime validation remain queued, not silently inherited from the measured preservation packages. No additional remote runs are planned. The source-check controller exited successfully; no ASTRA build/check process or host reservation remains.

Future test-box coordination acknowledged: AMD retains its BOXLOCK protocol; garbage uses ~/slipstream/BOXLOCK.txt plus an independent empty `pgrep -x do-not-directly` check before viewer use, atomically records identity/time, limits reservation blocks to 30 minutes, restores/releases after each block, and never stops foreign viewers or removes foreign locks. Both completed host restorations remain final; this policy acknowledgment did not restart remote work. The local owner GPU prohibition remains in force.
