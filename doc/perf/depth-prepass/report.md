# READY — depth prepass bounds, 2026-11-19

Sole SOL worker, owner-assigned `gpt-6.1-sol`; no additional agents. Runtime model identity is not exposed independently. Parent relay: `01a18431-804f-7e02-9abd-a5f3036449d6` → w1:p1. New owned worktree `agents/worktrees/sol-depth-prepass`, branch `depth-prepass`, fetched fresh `forgejo/main` baseline **`ae1f063eb7ca9c2c0e0bbec3921d2c21821e493a`**. Tested renderer commit **`4fe5cda01de83dd87b0504b4ea97f10811f2945a`**. Subsequent evidence/harness documentation does not alter the tested renderer. No pushes or merges.

The fix reuses existing conservative per-class indirect-draw bounds in both adaptive depth-prepass phases. Pascal house full-chain GPU cost falls **4.921 → 1.928 ms (−60.8%)**, with measured full GPU frame **28.276 → 24.069 ms**. A second interleaved house diagnostic corroborates **4.923 → 1.925 ms** and full GPU **27.443 → 23.535 ms**. AMD prepass timings are essentially neutral with RT on and off. No shader, attachment, actual draw count/order, setting default or quality rule changes.

## Finding and correctness

`drawPList` used full `mOccl2Cap` for every opaque/mask P1/P2 row, unlike the already-bounded ordinary draw buckets. Both P lists are subsets of the corresponding main-cull classes 0, 1, 6, 7. The seven-line production diff caps each row with existing `mdiMaxBucket`, retaining four-frame class history, upload/unknown-mirror fallbacks, headroom and the existing bounds-off control. Count/command buffers, offsets, stride and pipeline selection are identical.

The API executes the lesser of count-buffer value and maximum; preserving a valid upper bound preserves the command sequence. See the [official Vulkan command definition](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndexedIndirectCount.html). The omission and controlled Pascal timing reduction are proven here. Internal driver scan implementation is inferred from this single-variable experiment and the renderer's prior documented Pascal behavior; it was not vendor-profiled. Its contribution to the owner's **RTX5090** gap remains unmeasured.

A separate house population diagnostic found capacity 122624 versus per-row ceilings 68639/7965/76/64, reducing their sum from 490496 to 76744 per phase. Actual late diagnostic means were about 4994.5 P1 draws and only 1.26 newly revealed P2 draws. This is not a second full-scene draw. Diagnostic timings are excluded from the A/B distributions.

The CPU fixture executes the production accounting, limit helper and draw lambda across unequal class populations and 40 reclassification frames, P1/P2 subsets, lagged GPU rows, pending upload, unknown mirror and bounds disabled. It asserts no truncation, preserved offsets/stride and a tighter ceiling. This change retains depth-EQUAL receivers, mask/blend discard, motion/no-history metadata and current-depth occlusion semantics.

## Renderer comparison

[Source audit](source-audit.md) covers actual setting routes, geometry/terrain/rigged/alpha coverage, adaptive mode 2, BlendOpaqueGBuffer, attachments, vertex streams, reversed-Z clears, MSAA/VR/VRS, pre-skin work and the equivalent chain.

Both renderers prepass visible opaque/mask, terrain and rigged categories; neither simply selects a few large occluders. Slipstream's default slim deferred route already avoids a redundant normal attachment and reads G1 for AO; reduced depth vertex variants are already enabled. Its motion/class/no-history contract differs from Aurora's packed normal/motion output. Near-opaque blend texels also participate in Slipstream's depth/G-buffer chain. Aurora's separate blend/contact/refraction depth and optional MSAA depth must be included when comparing equivalent work. Two raster phases form complementary sets around Hi-Z; deleting phase 2 loses newly revealed surfaces.

The owner observed house 1.30/0.54 ms, overview 0.94/0.58 ms, club 0.41/~0.6 ms on RTX5090. Those isolated names do not establish equivalent total-chain times. Complete matched cross-renderer chains were not supplied; this report does not claim the new Pascal numbers close a measured Aurora/5090 gap. Aurora checkout `5a0995037a3de34812cbffba45fe0d75e845da0c` was read-only and clean. Its inspected source is not proven to match the owner's November 7 binary (SHA256 `8583607cf32157e2ca2965516fde06ea030f72020b78774e99c73df573b22fcb`). No GPL source, snippets or structure were copied, ported, built or modified.

## Measurements and attribution

[Full distributions and shifted costs](results.md), [numeric run evidence](evidence.json), [image/noise evidence](image-evidence.json), and [reproduction](reproduce.md) accompany this report. Each run retains full GPU/frame n/mean/p50/p95/p99/max and prepass interval distributions. Group means weight optional marks by sample coverage; percentiles are never summed. Graphics and `@async` intervals remain separate, and full GPU latency is measured independently.

| Host/case | Accepted A/B | Full GPU A→B ms | Full prepass chain A→B ms |
| --- | --- | --- | --- |
| GTX1080 house, nonRT | 5/3 | 28.276→24.069 | 4.921→1.928 |
| GTX1080 club, nonRT | 5/3 | 61.995→57.547 | 2.949→2.935 |
| GTX1080 street, nonRT | 5/3 | 32.771→31.301 | 0.431→0.380 |
| RX7900XTX house, nonRT | 3/3 | 5.126→5.134 | 0.4493→0.4502 |
| RX7900XTX house, RT | 5/3 | 20.466→19.856 | 0.9450→0.9391 |
| RX7900XTX club, nonRT | 5/3 | 13.957→13.930 | 0.7525→0.7511 |
| RX7900XTX club, RT | 5/3 | 46.140→42.704 | 2.4382→2.4370 |

The stable causal win is Pascal house raster time, **4.1449→1.1551 ms**, while prepass setup/cull stays **0.7758→0.7727** and G-buffer **2.0662→2.0700**. Primary full-frame adjacent-A comparisons give B reductions **4.426, 2.242, 3.984 ms**. A run means span 26.417–29.433 ms, so the entire 4.207 ms aggregate frame reduction cannot be attributed to this patch. Alpha and other lighting costs vary independently.

The additional house A1/B1/A2/B2/A3/B3 campaign disables waves/clouds/godrays only in identical isolated test profiles, holds eight simulated seconds and makes no image claim. It still has 2.977 ms A/A full-frame spread. Its prepass gain is **2.998 ms**; all B full-frame means (23.389–23.696) lie below all A (25.829–28.806). G-buffer **2.0507→2.0638**, AO **1.2683→1.2597**, alpha **5.8256→5.8406** and prepass setup **0.7822→0.7693** show no compensating moved cost. Extra full-frame improvement beyond the isolated chain is not assigned to the patch. No production default was changed for this diagnostic.

Club/street adaptive phase 2 runs only 2.9% of measured frames, versus house 99.6%. Club's unchanged prepass and large lighting/alpha A/A movement mean its aggregate full-frame drop is **not** claimed as an optimization gain. Street's tiny chain reduction does not establish a robust total-frame gain (one adjacent-A comparison regresses). AMD RT full-frame A/A drift is substantial: early club A runs are ~51.5 ms, later A and B ~42.2–42.8 ms; the prepass stays ~2.44 ms. AMD later paired results are essentially neutral and no broad AMD RT speedup is claimed. No unresolved material prepass regression is indicated by these distributions.

## Controls, images and limits

Both arms pin FSR3 Native AA (FSR4 off), output=render 1919×1080 Pascal / 1920×1080 AMD, adaptive two-phase mode 2, BlendOpaqueGBuffer on, reduced depth VS on, slim deferred on, split lighting on, AO on, far clip 512, full rigged detail, framegen/VSync/FPS cap off, hardware probe and auto-tune locked. NonRT uses sun/projector shadow maps, max 16 projectors. AMD RT requests GI3, RT reflections and stochastic shadows; actual GI3/ReSTIR and trace/shadow-visibility coverage are required for every timed frame. All accepted runs have AO coverage 100%, zero pipeline/skin/texture injection failures and quality ladder rung 0. Pascal effective key settings were independently queried through slctl. Windows acceptance also checks actual logged lighting/upscaler extents and timer coverage, beyond launch settings.

Each case starts with A/A controls, then B1/A3/B2/A4/B3/A5. Shader warm-up/readiness, focus and requested actual-route checks are mandatory; 300-frame pre-measurement warm-up precedes four simulated seconds (241 GPU samples). Complete captures add 240 frozen-world still samples outside timing. Captured transforms/light/pose timelines match within a host. Camera/profile/manifest/executable hashes are retained per run; cross-host camera identity is not asserted. Renderer clocks and stochastic histories are not all reset by the replay seed, so means need A/A controls.

HOUSE is truncated capture `e2bbf29c01aeddfc78f6ee3d9e783bb717fd13e8f251debcde3c1008986dfbe4`, **4680 missing textures**, 81735 instances: timing only. Complete synthetic CLUB `3b7822a5597b38eeca709a7540a2669b27dbf1731a204581abe71666025d7566` has 81 armatures, 196 skins and 1168 rigged rows; STREET `a9fb3d9a956c979d9379d79bb6a93d94ad38eaf516209c8097a0d1aad9c0b244` has 504 instances. Neither complete control contains captured probes or terrain; both use fallback environment. They do not certify the owner's EEP/probe scene.

All complete primary A/A and A/B mean/noise stills were analyzed. Means and worst-error crops were inspected for Pascal club/street and AMD club RT/nonRT, including strict depth-EQUAL controls. Extra six-run club campaigns per box set EQUAL=1 and slimDeferred=0 to exercise normal+motion attachments with AO and full rigged detail. They passed 3/arm and showed no visible missing geometry, changed alpha boundaries or new structural defect in inspected controls.

**Images are not bit-identical.** Pascal club common-A2 mean luminance error is A/A 0.003344–0.005306, A/B 0.005820–0.012145 on a 0–255 scale; the A/B global mean statistic exceeds A/A, although worst-tile error remains below the worst A/A tile. Temporal std means remain within the A/A range. AMD RT A/A error 0.258338–0.440772 and A/B 0.265660–0.428184 overlap, with comparable temporal noise. Pascal street noise differs by at most 0.000156/255 beyond A/A maximum. Full figures disclose strict-EQUAL noise variation too. These finite controls support unchanged visible output and no resolved new noise penalty, but cannot prove exact pixels. The valid-upper-bound argument separately establishes unchanged submitted commands. Moving-camera disocclusion, worn rigid attachments, terrain, owner probes, DLSS/RR, framegen, VR/multiview and other drivers remain runtime coverage limits.

## Rejections and verification

Two initial AMD house nonRT A runs timed UBER startup fallback despite replay readiness; they are rejected, leaving three valid FEATURE_SPLIT A and three B. RT cold compilation took about 13 minutes; compiler budgets/threads were not changed and no fallback was counted as RT. Preliminary AO-off Pascal samples are excluded. Early controller attempts mishandled numeric string enums, transient exiting-process paths, and one nested JSON matrix; these are setup rejections, not measured wins. The corrected harness normalizes enum types, waits for teardown and refuses foreign viewers. Initial Pascal functional smoke did not log in because an isolated helper copy was missing; it is rejected and retained separately.

Rejected production changes: disabling BlendOpaqueGBuffer (the prior GTX1080 study reported about 0.37 ms, but removing that coverage is not an unchanged-image saving), omitting mask/rig/terrain or motion, dropping P2, normal/history/sample reductions, default quality cuts and changing EQUAL/occlusion rules. Splitting alpha into new phase lists could change coplanar first-writer metadata and add synchronization. Unused VS TBN/material-fetch guards and opaque-only early-fragment tests remain unmeasured candidates; no shader edits were included.

`make check`: **107/107 PASS** on exact tested renderer commit, including the new production-extraction fixture. Linux and Windows A/B own builds/packages succeeded; Linux B package checks **4/4 PASS**, with pre-existing build-host libdecor ISA warnings. Both Pascal arms receive the identical native package-local libdecor core/plugin substitution; original packaged bytes are backed up and system libraries unchanged. No generated `vk_*_spv.h` is committed. Builds were serialized against host ninja with requested TMPDIR and 12 jobs. Recursive shells briefly reset initial niceness; the watcher was corrected and kept only owned build descendants at absolute nice 19. Build slot was explicitly released before the final host handoff; no other worktrees/processes were changed.

Functional synchronization smoke: **PASS**, corrected logged-in 240-second run through path-only owned valrun/slrun helper copies, strict EQUAL and rigged resize stress. Actual Khronos validation-layer mapping observed. Validation Error / VUID / SYNC-HAZARD / GPU LOST / segmentation / bad-shutdown markers all zero. Owner credential hashes and original helper hashes unchanged; isolated credential copies restored/deleted; no owned viewer left. No Windows validation-layer coverage is claimed. [Restoration and validation receipt](restoration-evidence.json).

## Restoration and handoff

AMD independently recovered after the owner reboot, was verified idle and reserved with exclusive CreateNew `C:\sltest\BOXLOCK.txt`, identity `sol-depth-prepass-ae1f063eb7-20261119`. Original job files and actual SlipstreamViewer settings/dialogs were backed up before runs; isolated APPDATA/LOCALAPPDATA and own packages were used through unchanged existing SLRun task. All seven file states (including original absence states) were restored and hash-verified, task idle and no viewer/plugin checked, owned lock released. Astra and parent received exact release; reflection-noise worker follows Astra. No more AMD work remains for this stream. No Windows security/system/task-definition changes.

Pascal restoration: **PASS and released**. Its atomic lock used the same identity; settings, dialogs, run log and original helpers were restored/hash-verified. The literal initially dangling `current -> /home/trash/slipstream/rec-9c87eb076f` was preserved and independently checked still dangling. An independent process scan found no viewer/plugin/controller; the released lock was independently checked absent. Parent and Astra received release. Both hosts and the build slot are now released. Every replay uses isolated home/cache and own packages. No owner-local viewer, GPU workload or config changes were made. No credentials, raw logs, scene coordinates, account names or chat are included in committed evidence.

## Owner RTX5090 test

Use the frozen Windows A/B packages with executable identities below, in isolated profiles with the owner's exact house/overview/club cameras and original DLSS Balanced/shadow-map/probe settings. Keep both profiles identical, compile/warm both first, then run at least A1/B1/A2/B2/A3/B3. Record actual adaptive coverage and the full raster + attachment/clear + split/Hi-Z/P2 chain, G-buffer/AO/lighting/alpha/pre-skin and full GPU/frame distributions. Compare complete-scene A/A versus A/B mean/noise images, alpha/coplanar boundaries, motion/disocclusion and material reclassification/reveal sequences. The incomplete house capture is timing-only. Expect the largest gain where two-phase lists and capacity dominate; do not promise a particular 5090 speedup or Aurora ratio from the Pascal result.

| Executable | SHA256 |
| --- | --- |
| Linux A | `4c9b9db9f4e33543d1f6524f6693f0b55d058a3be6971331c00e7fd264407f49` |
| Linux B | `0e520c77546b7a707d47ab024729664c4cc65432d12d897a46ba7a0ff9ce6007` |
| Windows A | `99b9f7ab3d85ccda63b847c7be1135deb29059f1f8e3e6fbaa1e4ae75381dba5` |
| Windows B | `6c45929235745d436c0bcd51d29f5fad63348fa861532a14bc601d12afb66746` |

Private local packages: `agents/scratch/sol-depth-prepass/artifacts/{linux-a,linux-b,windows-a-frozen,windows-b}`. Full owned raw evidence remains under `~/slipstream/sol-depth-prepass-ae1f063eb7` on garbage and `C:\sltest\sol-depth-prepass` on AMD, plus private local scratch. Reuse requires a new independent idle check/owned lock; the old reservation is not continuing authorization to use a handed-off host.
