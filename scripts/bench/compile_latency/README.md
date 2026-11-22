# Compile latency receipts

These helpers are for an exclusively reserved test host. `run_linux.py` uses the
campaign's exact lock token and owned directory on garbage. It never changes HOME,
logs in, or changes the owner's current-package symlink. It uses the host's
`slctl.py` with an isolated token to verify the running viewer. Back up and restore
any state touched by separate `valrun.sh` functional smoke tests before releasing
the owned lock. The main report records host availability and restoration.

Each matrix entry supplies `name`, `source`, `package`, `capture`, `path`,
`settings`, and optional `cold`, `force_oversize`, `driver_cache`, `worker` booleans.
Optional `seed_directory` restores an owned snapshot after moving the current
isolated cache aside; `seed_main` replaces its main file with an owned partial
snapshot. Both seed paths must resolve inside the owned campaign directory.
`cold` moves only the isolated **application** cache to the run's output directory.
It does not mean all OS/driver caches are cold. The NVIDIA driver-cache switch is a
request whose effect must be checked independently. Driver cache paths also point
inside the isolated home. Capture and path must already exist; use a complete
capture, fixed settings and path, and identical executable/shader identities for
matched comparisons.

```sh
python3 run_linux.py matrix.json out/fresh-run-directory
python3 summarize.py out/fresh-run-directory/base_cold1.json \
  out/fresh-run-directory/base_warm1.json --out numeric-receipts.json
```

Outputs include executable/input hashes, settings, application-cache file hashes
and sizes before/after each launch, driver-cache file sizes, per-process time/RSS,
control verification, benchmark JSON and logs. Raw viewer logs, settings and
capture metadata remain private. `summarize.py` exports compile labels and numeric
telemetry only. Per-item memory deltas are concurrent attribution estimates;
Linux process RSS is different from Windows private commit. Batch wall intervals
overlap; never add them to claim startup wall time. Replay readiness also includes
scene injection and scheduler waits. Feedback hits, fast creations, dedup calls,
elapsed creation time and driver-cache reuse are different quantities.

`runtime_eligible` requires a guarded timed-start timestamp after the last reported
batch. Compile receipts can remain useful when runtime is excluded. Forced-worker
runs additionally sample the launched process tree's summed RSS every 250 ms;
this is a sampled aggregate, while `peak_rss_kib` is the kernel's maximum child
process high-water value, not a sum of concurrent worker peaks.

For the controlled oversize experiment, compile `force_oversize.c` on the reserved
Linux host using Vulkan headers in an owned include directory:

```sh
nice -n 19 cc -Wall -Wextra -Werror -shared -fPIC -O2 -Iinclude \
  force_oversize.c -ldl -o force_oversize.so
```

This helper intercepts `vkGetPipelineCacheData`: successful size-only queries for
real caches above 24 MiB report 256 MiB + 1. Data reads and pipeline creation are
unchanged. The runner sets LD_PRELOAD only for entries with `force_oversize=true`.
Confirm injection messages in the owned log, then compare exact cache-file hashes
and the following launch. This tests the production oversize policy with useful
real driver seeds; it does **not** simulate natural 256 MiB growth, memory use of
such a cache, driver eviction, or a driver-specific large-cache bug. Do not use it
for ordinary cold/warm timing or load it into an owner's viewer.

Suggested sequence: normal cold/warm repeats; preserve the resulting bounded seed;
three oversized baseline launches; restore that seed; repeat with the fixed
executable. Separately run app-cold with an empty isolated driver cache enabled,
then app-cold again with the same driver cache retained. Verify pipeline hashes,
settled readiness, zero failures/dropped items and focused-frame timing before
accepting results. Compare held images against an A/A repeat, without publishing
private scene details.

`export_receipts.py DIRECTORY... --out PUBLIC_DIRECTORY` exports whitelisted
run summaries and per-pipeline CSV. It excludes raw capture metadata, preferences,
control responses and logs. Driver-enabled runs inspect the owned executable's
open cache descriptors; exported receipts retain only their count and whether
all paths were inside the isolated home. Full raw receipts remain on the test host.

Windows addendum: `reserve_restore_windows.ps1` and `run_windows.ps1` are
prepared for the explicitly queued AMD handoff. Reserve only after the previous
worker restores/releases, with independent idle/lock checks. The Windows runner
uses explicit owned cache paths and 250 ms sampled private commit for the viewer
and same-executable compile workers. `peak_working_set_bytes` is a separate RSS
high-water measure. Driver caches are not purged; cold means application-cold.
Windows JSON may include a UTF-8 BOM, accepted by the numeric exporters.
The initial matrix uses four baseline launches, an exact-seed cross-build control,
and four fixed launches to establish fully warm A/A and A/B controls.

The completed AMD campaign adds `windows-driver-warm-control-runs.json`: an
original-policy app-empty control after the driver is warm, then a populated
warm repeat. This prevents attributing driver reuse to the preservation fix.
`export_workers.py` exports worker batch cost, semantic-respin changes and
viewer/worker identity comparisons without copying raw log lines or paths.
The Windows scripts ran through the existing stdin PowerShell/desktop task
channels; no execution-policy or scheduled-task changes were made.
