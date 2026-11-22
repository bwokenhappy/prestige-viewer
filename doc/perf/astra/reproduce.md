# Reproducing the ASTRA experiments

Use an isolated checkout and the authorized test hosts only. Do not run the viewer or GPU tools on the owner's workstation. These commands describe the recorded campaign, not a claim that every queued run passed. Accepted results and rejected receipts are distinguished in the measurement ledger.

## Source and build

Original renderer base: `ae1f063eb7ca9c2c0e0bbec3921d2c21821e493a`. Instrumentation/readiness source: `59aea7da31`. Default-off prototypes: `95f820bbf15231cc87c2afe344b7d8bb2b5f95ee`. Diagnostic changes are described in `regression.md`; neither optional prototype changes the configured ray budget.

Wait until every host-wide ninja process has exited. Build only in the isolated checkout. The campaign used the following environment and an absolute nice19 priority (the surrounding shell inherited a negative niceness):

```sh
while pgrep -x ninja >/dev/null; do sleep 5; done
nice -n19 env TMPDIR=/home/bryan/.cache/slipstream-build-tmp \
  CMAKE_BUILD_PARALLEL_LEVEL=12 NINJA_JOBS=12 PYTHON_CPU_COUNT=12 \
  python3 -c 'import os; os.setpriority(os.PRIO_PROCESS,0,19); os.execvp("make",["make","windows"])'
```

Archive the finished ZIP outside `build-windows-x86_64/package` before another Windows build; packaging clears that directory. The95 ZIP SHA256 is `ccc6761cc9eec37b250cc39f099237b57c33adf27fb32086c30e662ae08d0a62`.

The Linux build used the same environment and absolute priority with `make RUN_CHECKS=0 TESTCTL=1 TARBALL=1 build`, followed by `make check-package`. Source checks had already passed for the identical renderer source in the Windows build. Preserve package and executable hashes. Garbage's packaged libdecor core/plugin required substitution with its native compatible libraries; originals and exact hashes were retained. This was confined to the test package, with no system-library changes.

## Reservation, identity and execution

Before running, inspect actual viewer processes and reservations, atomically acquire the documented host lock only when free, and back up all owner data that the harness touches. Never reuse a historical lock as evidence of current availability. Isolate APPDATA/LOCALAPPDATA on Windows and use `SLIPSTREAM_HOME` plus an explicit test-control token path on Linux. Do not override HOME. Restore owner settings/dialogs, job files, run log and current symlink, verify their hashes/target, then release only the owned lock after all controllers and viewers have exited.

The committed matrices under `scripts/bench/astra/matrices/` name the exact settings and fixed input paths used for the95 prototype campaign. They deliberately retain the different Windows/Linux output extents in the run receipts. `run_windows.ps1` and `run_linux.py` refuse an unowned lock or an already-running viewer; each resets the isolated settings template and records source, executable, manifest and path SHA256. Capture manifests also contain per-file size/xxh64 receipts. These private captures are not committed.

Recorded invocation shapes (after reservation, backups, package adaptation and staging):

```powershell
& C:\sltest\astra\run_windows.ps1 -Root C:\sltest\astra\prototype-run `
  -List C:\sltest\astra\house-prototype-runs.json `
  -Out C:\sltest\astra\out\house-prototype-01
```

```sh
ssh garbage bash -s <<'SH'
python3 "$HOME/slipstream/astra/run_linux.py" \
  "$HOME/slipstream/astra/linux-prototype-runs.json" \
  "$HOME/slipstream/astra/out/linux-prototype-01"
SH
```

Use a fresh output directory for any repeat. The required `valrun.sh`/`slctl.py` functional smoke was separate from offline replay; no validation layer was available, so it is not Vulkan validation coverage. Offline runs never log in.

## Analysis and acceptance

Run `scripts/bench/astra/regression_summary.py DIRECTORY NAME...` on private receipts. It requires replay completion, focus, shader readiness, actual split markers, matching extents/identities and full-frame pass coverage. RT requires actual GI3, ReSTIR and pass15; non-RT requires report RT0, applied GI0 and no RT trace work. Analyze different captures/resolutions separately. Never pool percentiles or add overlapped queue intervals.

A/A repeat spread precedes A/B interpretation. Optional LVC timestamp stages and mode3 diagnostic rays are separate arms; their timings are not silently included in clean prototype cost. The captured HOUSE is truncated with4,680 missing textures and no avatars/probes: it supports qualified same-byte timing attribution only. Complete synthetic street and club support limited image controls; both contain zero captured probes. Street's omitted atmosphere block uses the default local sky. Neither box exercises DLSS/RR or an RTX5090.

For raw frozen stills, use `scripts/bench/still_detail.py aa A1.bin A2.bin`, `compare A1.bin B.bin --out PRIVATE_DIR`, and `noise A1.bin A2.bin B.bin`. Compare the same still key, dimensions and held-frame count. Raw files for repeated keys are overwritten by the last loop; report that final-loop sample rather than treating it as two independent images. Inspect image differences and temporal variance against A/A before making a quality claim. No default change is justified by a single scene, by unsupported paths, or by a pass timer moving without a total-frame benefit.

## Dense-light fixtures and admission diagnostics

Diagnostic package source5d60acdf6a adds admission reasons only to the existing mode3 VIS variant. Source checks:108pass; four VIS/LVC variants compile and pass spirv-val. Both production VIS variants compile byte-identically to95f820bbf1. Matrix:`matrices/amd-admission.json`; it follows the same lock/template/noFG contracts.

Generate the synthetic controls without launching any GPU process:

```sh
OPENBLAS_NUM_THREADS=1 python3 scripts/bench/astra/dense_lights.py /tmp/astra-dense-repro
```

The capture writer now emits the current264-byte `LLVKLight`: key at240, debug flag at248, log-only U32 identifiers at252/256 (zero), tail padding through263. The first campaign captures were generated by5d60acdf6a's older256-byte writer and converted before deployment using the prior campaign's layout rule. Native264-byte writer output was checked against all converted binary payloads: identical. Manifests retain their actual capture-time metadata; compare payload hashes rather than expecting regenerated timestamps to match. Both fixed128-light populations and zero-light reference fixtures are produced; the latter are not claimed to exercise active ReSTIR.

Closed/visible fixtures differ only in one wall's transform. They keep the same camera and light positions; physically valid lighting changes are expected across those scenes. A/B options must use the same capture, and complete-scene stills must be compared against same-capture A/A noise. These fixtures have no avatars or captured probes and retain the writer's fallback-environment limitation.

## Residual-light ownership mask and image controls

Renderer57f164d27d150b5f4c4a426c11071321fcd11cef is the default-off residual deferred point-mask prototype. Its matrices are `amd-radiance.json` and `linux-radiance.json`; the latter is an unsupported-RT negative control, not RT parity. Verify `radiance_point_mask_counts`, actual stochastic radiance ownership and all-frame RT timers in accepted AMD receipts. Log presence by itself does not prove that every candidate was skipped.

`image_controls.py ROOT A1 A2 B PRIVATE_OUTPUT` runs CPU-only paired analysis and produces metrics plus labeled contact sheets. It requires actual held stills, compares A/A and A/B mean luminance, and summarizes temporal standard deviation. Inspect those images before publishing; do not commit private HOUSE imagery or logs. The scripts read final-loop raw files and normalize orientation using still_detail.py. Held-frame temporal variation includes history and transient effects, not solely sampling noise. Identical expectations should be assessed against observed repeat spread, not inferred from source equivalence alone.

Recorded57f package SHA256: WindowsZIP `312a046e24062a7707e99e4a6923710e4f233cc3b4eeb761a86ab15e5ac3a61b`; LinuxTESTCTL archive `13ab09494905e149e67912a69014376875bacb336da8b556457ec73a744644df`. Windows package and installer built; Linux package checks4passed with the documented package-local libdecor compatibility warning/adaptation. Final source checks108passed. A later report-only commit does not change these executable identities.
