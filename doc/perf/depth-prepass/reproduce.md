# Reproduction and acceptance

Use the owned `depth-prepass` checkout and authorized benchmark hosts only.
The baseline renderer is fresh `forgejo/main` at
`ae1f063eb7ca9c2c0e0bbec3921d2c21821e493a`. Keep baseline binaries outside the
build directories before changing source. Never commit generated SPIR-V headers.
No Aurora source or implementation structure is used in this patch.

`build_owned.py LOG COMMAND...` waits for every host-visible ninja before
starting and keeps only its own build descendants at absolute nice 19. It pins
TMPDIR, CMake/Ninja parallelism and Python shader workers to the requested paths
and 12 jobs. Recursive shell startup can reset niceness; the watcher corrects
that without renicing any other task. Linux uses `make TESTCTL=1`; Windows uses
`make windows`. Source checks run for each final renderer source; repeated
package builds may use RUN_CHECKS=0 after those checks pass. Never build in the
owner's mega tree. No viewer or GPU diagnostics run locally.

Before any viewer, independently inspect idle status and atomically reserve the
host's documented BOXLOCK with this task's identity. Back up settings/dialogs,
run logs/helpers and the literal current symlink on Pascal; back up the desktop
job files and actual SlipstreamViewer settings/dialogs on AMD. Preserve a
pre-existing dangling link. Windows runs through the existing SLRun desktop task,
without changing its definition or any Windows security/system settings.

Generate matrices from private copies of the existing camera paths:

```sh
python3 doc/perf/depth-prepass/make_matrix.py linux PRIVATE_INPUTS PRIVATE_OUTPUT
python3 doc/perf/depth-prepass/make_matrix.py amd PRIVATE_INPUTS PRIVATE_OUTPUT
```

The matrices pin adaptive two-phase mode 2, blend-to-G-buffer on, reduced depth VS
on, slim deferred prepass on, FSR3 Native AA, 1920x1080 requested output,
AO on, full rigged detail pinned (adaptive rigged simplification off in both test
arms), frame generation off, shadow maps for non-RT and actual GI3/stochastic shadows
for AMD RT. Hardware probing and auto-tuning are locked. Each case starts A1/A2
controls, then B1/A3/B2/A4/B3/A5; cases retain separate receipts. The first A/A
controls can run while the candidate is being built. They are not pooled with
a different scene/resolution. Record actual render/output extent, driver,
manifest/path/template/executable SHA256 and exact renderer source SHA.

Stage complete package-a/package-b directories under the owned host root.
Create source.txt with the exact source SHA and READY only after upload hashes
and package-local adaptations are verified. Pascal's build-host libdecor core
and Cairo plugin may require the same backed-up substitution with native host
libraries recorded in prior reports. Apply identical native bytes in both arms;
never edit a system library. The controller waits without a viewer until its
required package is ready. Each run resets a byte-identical isolated settings
template, uses an owned cache and isolated APPDATA/LOCALAPPDATA or SLIPSTREAM_HOME,
and refuses an unowned lock or an unexpected viewer. Real HOME is unchanged.

`run_linux.py runs.json NEW_OUTPUT` uses the original slctl.py to verify actual
key settings; `run_windows.ps1 -List runs.json -Out NEW_OUTPUT` runs through the
owned desktop wrapper. Replay source readiness is mandatory. Warm-up precedes
timing. Complete captures add 240 frozen-world still samples; timing excludes
those readbacks. Raw stills and logs remain private. Use the existing valrun.sh
and slctl.py functional validation with isolated helper copies; preserve original
helper bytes and disclose whether a validation layer actually mapped.

```sh
python3 doc/perf/depth-prepass/analyze.py PRIVATE_OUTPUT SUMMARY.json --require-complete
python3 scripts/bench/still_detail.py aa A1.still0.bin A2.still0.bin
python3 scripts/bench/still_detail.py compare A2.still0.bin B1.still0.bin --out PRIVATE_DIFFS
python3 scripts/bench/still_detail.py noise A1.still0.bin A2.still0.bin B1.still0.bin
```

The analyzer requires successful/focused replay, shader readiness, no device or
validation error markers, successful texture/skin injection, matching dimensions
and input identities, and full prepass coverage. RT acceptance requires actual
GI3, ReSTIR and full-frame trace/visibility marks, rather than only requested
settings. Non-RT must have no active trace work. It emits numeric metadata and
per-pass distributions, never raw logs/scene coordinates. Optional mark means
are weighted by their sample count / total samples; per-pass percentiles are
not added. Queue identity comes from `@async`, not pass-name prefixes.

Require at least three accepted runs per arm, A/A repeat spread, interleaved
paired differences, full GPU/frame time and shifted G-buffer/lighting/AO/alpha/
pre-skin costs. A smaller prepass alone is not a win. Inspect complete-scene
mean images and difference crops; compare temporal noise against A/A. HOUSE is
truncated with 4,680 absent textures, so it is timing-only. Synthetic complete
club/street have no captured probes and a fallback environment; neither
establishes owner-scene, DLSS/RR, VR or all-terrain parity.

After every controller/viewer/plugin has exited, restore all saved files and
original absence states, verify hashes and literal symlink target, release only
the owned lock, and explicitly hand the slot back to the queued worker/parent.

The final renderer B is `4fe5cda01de83dd87b0504b4ea97f10811f2945a`.
Linux final source checks passed 107/107; Linux A and Windows A/B packages were
built; Linux B package checks passed and both Windows packages completed actual replays. Windows B used the
same official configure/build/manifest/ZIP flow in `package_windows.sh`, omitting
only a redundant installer. One wrapper failed after A Windows packaging on a
zombie NI field; the already-built artifact was independently frozen and hashed.

Actual Windows fallback rejection matters: two initial house nonRT runs had
requested split lighting but executed UBER. Preserve these rejected identities
and raw results separately, then validate the accepted-only primary directory
with `--require-complete`. The accepted primary set has 30 AMD and 24 Pascal
runs, including at least 3/arm in every case. Do not report a cold UBER timing as
FEATURE_SPLIT, or accept `replay: pipelines ready` alone as proof of the route.
RT cold compilation took about 13 minutes without adjusting compiler budgets;
subsequent runs prove GI3/ReSTIR and complete trace/visibility coverage.

Additional matrices were derived from the same templates/camera: six club
EQUAL controls per box set `RenderVKPrepassEqualCompare=1` and
`RenderVKPrepassDepthOnlyDeferred=0` to exercise normal+motion attachments.
Six Pascal house quiet controls use eight simulated seconds, waves/clouds/
godrays off in both isolated test profiles. This diagnostic changes no product
defaults, makes no image claim from the incomplete house and still shows A/A
variation; its stable prepass gain corroborates the unchanged full-quality
primary case. Preliminary AO-off runs are excluded from the AO-on primary set.

`image_controls.py PRIVATE_STILLS METRICS.json --crops PRIVATE_CROPS` runs the
existing still-detail analysis on every complete A/A and A/B run using A2 as a
common reference. It emits numeric evidence only. It does not assert bitwise
identity or treat a small mean error as a proof of geometry correctness.

The Windows controller waits through exiting processes whose image path becomes
unreadable; a known foreign image path still stops it. Old conservative teardown
stops and a nested-JSON matrix waiter are retained as setup evidence and were
not timed GPU cases. The final harness rejects nested matrices before waiting.
The first Pascal sync helper attempt missed its isolated slctl.py copy and did
not log in; that attempt is rejected, not counted as functional validation.
The corrected path-only helper copy retains the original helper's exact bytes.
