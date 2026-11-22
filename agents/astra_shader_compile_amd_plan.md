# Missing AMD compile coverage — queued addendum

Existing READY fix/evidence stays at e2286e0ed6702051effeee81a583e91b90625fb2. Only the previously unavailable RX7900XTX/Windows coverage resumes. No Pascal repeats, architecture campaign, owner-local GPU or cache/environment changes.

## Queue and builds

Owner assigned Hegel thread01a1f44a-18ae-79b1-8613-8a02390b2354 first reservation. Native coordination acknowledged; Hegel requested the first Windows baseline/candidate build cycle without ASTRA heavy-build contention. ASTRA has not contacted or mutated amdbox. Wait for explicit build-slot handoff and independently inspect all host ninja activity before building. Wait separately for host restoration/release handoff, then independently check viewer/task idle and atomically reserve BOXLOCK; a free build slot is not host permission.

Two matched Windows packages are required because older architecture packages lack the corrected per-pipeline telemetry. Build original policy ef70f524fe and fixed renderer eb7f944546 plus report/tool-only commits, sequentially in this owned worktree, absolute nice19, TMPDIR=/home/bryan/.cache/slipstream-build-tmp, parallelism12. Archive the first package before the second clears packaging output. Reuse the same Windows build directory incrementally. No shader or renderer-policy change is proposed. Record actual source/executable/ZIP/four generated-header hashes. No generated headers are committed. Make check remains required; Windows build itself closes the missing compilation coverage.

## Smallest justified sequence

Use one complete synthetic capture, existing short six-key/two-hold path, same settings/resolution, RT/GI enabled, AMD's normal compile-worker path, eight threads, FG off. Verify actual post-restart device/driver from host metadata and viewer, capture completeness/identity and real settings/RT timers. No driver cache reset or machine-wide cache/env changes: “cold” means isolated application-cache cold only.

1. Original: app-cold, first warm, fully warm, fully-warm repeat (four launches; the last pair supplies A/A and allows first-warm late variants to fill).
2. Save exact bounded original seed; fixed cross-build warm launch from that seed (one launch).
3. Fixed: app-cold, first warm, fully warm, fully-warm repeat (four launches).

Nine short replays, with only two application-cold starts. Require settled/no-failure/no-drop/focused reports, last batch before timed start, matching create identities and actual render state. Rank huge spec-ray kernels from the item CSV. Report batch spans, individual-call sums, per-pipeline and viewer private commit, sampled simultaneous viewer+worker private commit, worker create-info/hash reuse, and exact on-disk start/end manifests. Windows private commit is not Pascal RSS; application feedback hits are not driver-cache hit telemetry.

Use two held images and warm GPU timing for A/A and A/B. If the same late-variant readiness problem prevents acceptance, diagnose it rather than pool mismatched frames. Limit additional launches to a demonstrated missing case.

If natural per-file oversize occurs, preserve the last under-cap seed and run repeated original/fixed launches with that identical seed. If not, retain the already-proven production regression and Pascal forced-oversize results; do not label this AMD normal-path run an AMD oversize proof. No Windows driver hook/cap-changing production patch is planned merely to duplicate the existing causal experiment.

## Prepared materials and restoration

`scripts/bench/compile_latency/run_windows.ps1` uses a fresh owned area, isolated APPDATA/LOCALAPPDATA and explicit owned CacheLocation/NewCacheLocation; logs per-run hash/time/commit receipts. It never logs in or purges driver caches. Matrices are `windows-base-runs.json` and `windows-fixed-runs.json` under `matrices/`; final source/capture identities are confirmed after handoff/build.

`reserve_restore_windows.ps1` requires an idle viewer/task, atomic FileMode.CreateNew lock, exact owner preference/job backups including absent-file states, and restores the existing desktop job files before releasing only the owned lock. Reuse the existing SLRun task; no scheduled-task/system/security changes. Windows PowerShell parsing/execution remains untested while the host is reserved by Hegel. No remote deployment until independent reservation.

Append accepted AMD results and exact restoration receipts to the canonical shader report/status, commit the addendum locally, and send parent native READY/BLOCKED SHA+report. HERDR_ENV remains unasserted; never claim herdr delivery without legitimate enablement. Parent remains free for tickets.

## Build milestone (2026-11-19, 16:35 UTC)

Both Windows packages are now built and archived in owned `build-astra-windows-base`
and `build-astra-windows-fixed`. Baseline ef70f524fe passed106 source checks; fixed
f76d349635 passed107. All four generated shader headers match exactly. Full
SHA256 receipts are committed under `agents/astra_shader_compile_amd_data/`.
The baseline's standard installer completed; the fixed build used the same
standard compile/staging/ZIP sequence and omitted an unused second installer.
The guarded attempt to interrupt baseline installer compression terminated no
process; it had completed before the controller advanced.

The build controller exited and the build slot was explicitly released to the
reflection-noise worker01a1f466-ac2d-7f72-a91f-c32b40ef7bfb, with parent/Hegel
notified. This was NOT a host release. AMD remains Hegel-owned; no ASTRA AMD
contact/reservation/mutation has occurred. Runtime measurements and PowerShell
execution remain pending the separate restored-host handoff.

Before deployment, runner settings now explicitly disable inherited cache-purge
flags and force disk-cache persistence, record the isolated template hash, and
use both cache-location fields identically. The RT acceptance helper requires
active GI3/ReSTIR and full timed-interval GI/visibility coverage. These are test
harness additions only; no further renderer build is required.

## Execution completed (17:04 UTC)

Eleven accepted runs completed after independent reservation. Two justified extras:
original app-empty with warmed driver state to resolve the apparent cold speedup,
then original warm4 to obtain a second fully populated508-item A/A control.
Both initial baseline runs had494items; warm2 added14. RT checks passed all11.
All7 original states restored (2absences), ownlock released, parent/Sol notified.
See final report and numeric receipts. No remaining host/build reservation.
