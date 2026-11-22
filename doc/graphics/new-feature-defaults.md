# New graphics feature defaults

Implementation: `0cc0b1f03f`, branch `new-feature-defaults`, based on main `855b29b343`.
Owned worktree: `/home/bryan/Documents/git/slipstream/agents/worktrees/gpu-timer-scopes`.

`SLGraphicsFeatureDefaults::initialize` runs in `LLAppViewer::initVulkanForRealUI`, after the real GPU's base masks and the existing first-run/new-GPU preset path, before texture-default selection and first world pipelines. It reads the current quality level (invalid values fall back to the GPU class), copies only registered entries, and uses the actual `LLFeatureList::maskList` / recommended-value rules. It does not change the manager's live masks, quality setting, other controls, or feature-table version33. The existing intentional first-run/new-GPU/safe-mode preset behaviour is unchanged.

“No saved value” is determined from the raw global settings file's key and Value presence, not just `LLControlVariable::isDefault()`: an explicit factory-equal value can collapse to the default stack. An explicit saved key causes this initializer to skip assignment; a current nondefault value is preserved too. Historical values older viewers already omitted from disk cannot be distinguished from never-set values. This build prevents further omission by retaining registered choices with PERSIST_ALWAYS, including factory-equal automatic or explicit choices.

Completion is a hidden persistent LLSD map, `GraphicsFeatureDefaultsApplied`, with a boolean per setting. Completed entries never reselect a value on later launches/quality changes; all-complete startup skips the file read entirely. Normal explicit preset changes remain normal preset changes. Session files and --set (including qualified Global names and values equal to default) are tracked as temporary overrides; untouched entries with temporary overrides defer without becoming a permanent factory/high-tier preference.

The file read is startup-only, capped at16MiB and detects truncation/growth; unreadable/malformed/legacy input defers. Marker state is bounded to64 boolean entries with bounded key lengths, preserving unknown future entries. Allocation failure is caught without terminating startup; incomplete work can retry. Normal settings-save persistence is used, not a new per-frame task, thread or file.

## Registered settings

| Quality | Caustics | Blend light cap | Blend-layer reuse |
|---|---:|---:|---:|
| Low | off | 6 | 1 |
| LowMid | off | 6 | 1 |
| Mid | off | 8 | 0.5 |
| MidHigh | off | 16 | 0.35 |
| High | on | 16 | 0.25 |
| HighUltra | on | 256 | 0.1 |
| Ultra | on | 256 | 0 |

Names: RenderVKWaterCaustics, RenderVKBlendMaxLights, RenderVKBlendLayerReuse. Values are read from the installed tables, not duplicated in production code; existing GPU availability/limit masks can further restrict them. Windows/Linux tables agree. Legacy Mac tables do not name these Vulkan options; absent rows are not manufactured.

These are exactly the newly persisted masked/tiered settings since v1.6.0-beta.3. No exemptions needed. New persistent UI/camera/environment settings without table masks are not selected; unsaved diagnostics/experiments are excluded. Pre-existing masked settings form the frozen44-name Beta3 baseline.

## Adding future features

Add the persistent numeric control and its feature-table rows, then add its name to settingNames. Do not bump the feature-table version or refresh the Beta3 baseline for a new feature. If automatic initialization is inappropriate, record a specific exemption reason in the baseline JSON. Keep completion names stable; changing tiers later does not reset completed choices.

## Regression guard

`scripts/check/checks/graphics_feature_defaults.py` runs in `scripts/check/run.py --stage source`. It compares persisted settings against quality/hardware feature masks and the frozen baseline; every new candidate must be in the registry or have a reasoned exemption. Baseline hash, duplicate/oversized registries and stale exemptions are checked. A synthetic new-feature mutation demonstrates the missing-registration failure.

The CPU fixture executes the unmodified production initializer, numeric/marker checks, feature-mask methods and real shouldSave method with data/API stubs (not a live viewer or full XML parser). It covers all7 levels, explicit saved true/false including factory equality, fresh preset/live choices, once-only/partial completion, temporary overrides, invalid quality and marker data, availability restrictions, validation refusal, allocation failure and capacity bounds. Startup hook ordering and bounded I/O wiring are source-checked. No runtime GPU claim.

## Release note and validation

One user-facing line added to the existing YAML/Markdown draft on a separate notes-only branch: `new-feature-defaults-notes @ 17dab099d4` (apply after beta-prep draft `269a04a177`):

> New graphics options get card/quality-appropriate defaults once, without resetting existing choices.

YAML safe_load/body equality PASS; exactly one body line added. Final committed-source Linux build/link/package PASS (exit0) from 0cc0b1f03f; complete source suite PASS133 and package gates PASS4. Real configured -Werror viewer objects, no syntax stubs. Existing source warning classes (local framegen ID and skin-import fixture) and host-libdecor ISA package warnings remain; no zero-warning or distributable-package claim. No Windows build/runtime test was requested or claimed. No pushes, tags, box/viewer or GPU runs; no shader changes or generated vk_*_spv.h commits.


READY. Runtime source commit: `0cc0b1f03f295d48869b785aac2acdfb2750aa32`. The subsequent report-only commit changes no runtime code. Clean merge base: main855b29b343. No unaccepted blend-layer-cost ancestors or feature-table file changes.

Final receipt: `/home/bryan/.cache/slipstream-build-tmp/new-feature-defaults-final.log` (source pin, full source checks, actual viewer compile/link/package and package checks). Initial focused fixture attempts only lacked harness includes, corrected before final PASS. Owned compiler jobs are finished; foreign jobs were left alone.
