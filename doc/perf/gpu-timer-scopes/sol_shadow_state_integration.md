# Superseded integration proposal

Parent integrated the active helper as `indra/newview/vkgfx/vk_gpu_shadow_state.h`, its setting and all renderer call sites. The inactive proposal header was removed. The historical notes below describe the integration intent; do not apply them a second time. No V5 cache code was imported.

# Parent-only shadow state integration

The removed standalone proposal was extracted from
V5's useful effective-state diagnostic with per-window recording counts added.
No cache, per-draw predicate, shader, LOD or bounds implementation is imported.
The first log contains one recording; later windows aggregate all observations
for five seconds. CPU recording counts do not prove GPU execution/submission.

Parent adds the include and a live Boolean `RenderVKShadowRigCullState`, Persist0,
default false. Inside `recordShadowRigCull`, after reading `on`, use:

```cpp
static LLCachedControl<bool> s_state(gSavedSettings, "RenderVKShadowRigCullState", false);
auto state = [&](uint32_t nv, bool recorded, uint32_t reason) {
    LLGpuShadowState::observe(s_state, mOffscreen, (uint64_t)gFrameCount,
        mSunRtNow, mProjMode, nv, recorded, reason, cullBase.skin_count);
};
```

Replace the initial combined return with a block calling `state(0u, false, reason)`
first: reason1 disabled,2 no rigged,3 no pipeline,4 descriptors,5 zero stride.
At `nv == 0`, call `state(0u, false, 6u)` before returning. After the actual
`vkCmdDispatch(cmd, 5, nv, 1)`, call `state(nv, true, 0u)` (outside the GPU scope
if parent wants all CPU diagnostics outside its lexical block). Do not move any
GPU barrier or add an enclosing caller scope: SOL already scopes the positive-nv
kernel after its input barrier and before its output barrier. View upload has a
separate `c_shadow_rigprep` scope.

Use the recording counts plus frame range, not just the last sampled state, to
correlate no-view periods with timer gaps. `recordings`, `dispatch_recordings`
and `no_view_recordings` are CPU observations; aborted submissions may not run.
No per-avatar identifiers, names, chat or private free text are logged.
