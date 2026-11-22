# GPU pass scopes and queue gaps

IN PROGRESS — no READY claim until the Linux build and final checks pass.
Base: mega-1106 `75c5e64534` (supersedes59d9700a31). Branch/worktree: `gpu-timer-scopes`,
`agents/worktrees/gpu-timer-scopes`.

## Measurement contract

Explicit `GpuPassScope` begin/end query pairs bracket the actual work. Ordinary
`gpuSubMark` calls remain device-lost breadcrumbs, stream-signature events and
legacy sampled-rig instrumentation, but no longer attribute the previous interval
to their name. Only frame/async-segment/front-end envelopes retain boundary queries.
Input/output resource barriers are outside command scopes wherever the renderer
owns the commands. An empty conditional bool producer cancels its sample; its
reserved endpoints still get written, avoiding unavailable queries in the pool.
A zero-view shadow rig cull returns before reserving a pair.

The values are **elapsed GPU service time**, including contention and stalls
inside the measured work. Vulkan timestamps cannot isolate active shader cycles
or subtract internal hardware stalls. SDK calls and some composite producers have
internal barriers that their public API cannot expose separately; they remain
elapsed producer scopes, not claimed pure kernel execution time.

`wait_total` is the complement of the union of graphics scopes inside the recorded
frame envelope. It is **unscoped queue time**: dependency/barrier waits, scheduling
gaps and any uninstrumented commands. It is not proof that all that time is idle,
nor proof that every wait is due to avoidable overlap. Largest `before_*` values
identify the next measured scope. Each boundary groups and sums all gaps before
that name in a recording; the top-five list is shared across both tagged queues,
not five boundaries per queue or a claim about one unique occurrence. Async scopes and gaps use the compute family's
own timestamp clock/mask, never subtraction against graphics timestamps. Front-end
pipelining has its own query pool because it can execute before the graphics reset.
Front-end and later async spans are unioned on that same compute clock, including
inter-segment gaps once. No timestamp can observe a semaphore wait before the
first timestamp executes, so no global all-GPU-waits claim is made.
The existing graphics total/async segment envelopes describe overlap; their sum
is not frame time. There is no invented cross-queue overlap duration.

Nested/overlapping intervals are unioned for gap accounting. Repeated disjoint
scopes with one name are summed per recording before window averaging and worst
selection. Thus `worst:` is the worst frame's sum for that pass, not the worst
individual dispatch. Synthetic totals such as `probe_frame`, `q_*` envelopes and
leaf passes overlap by definition and must not be added together.

## Log format

Illustrative values only (not measured hardware results):

```
GPU sub ms/frame: c_shadow_rigcull=0.02 c_shadows=0.4 | async queue: c_gi_trace=2 q_defer_light=3 | gaps: wait_total=0.8 async_wait_total=0.1 before_c_shadows=0.6 | scope_meta: queries_per_frame=200 truncated_frames=0 sampled_frames=300 | worst: c_shadow_rigcull=0.04
```

`GPU ms/frame` retains its existing total and coarse phase-envelope fields;
those legacy phase fields still include waits and are labelled as such. The
explicit pass/gap breakdown is the sub line. The `name=ms` graphics/async
sections and `| worst:` tail remain parseable. Gap fields are in their own
`| gaps:` section, never pass samples. `scripts/perf/gpu_scopes.py` parses the four
sections independently; the regression fixture checks they cannot contaminate
each other. Raw frame export retains original recording identity and receives
one summed pass sample per name/queue/frame. Missing measurements stay missing.

## Bounds and failure behavior

Query storage is fixed: four ring slots, 4096 main sub queries and512 front-end
queries each. A scope atomically reserves both endpoints including multiview
padding. Capacity refusal never reserves half a pair; truncation marks the raw
pass set incomplete. Query reads are nonblocking and not-ready data is skipped.
Allocation failure during timer-pool creation disables timers; sample-collection
allocation failure drops/incompletes diagnostics. Frame queries reject immediate/setup command buffers that do not belong to the
active frame/continuations. No render resource or image quality behavior is
changed by the instrumentation.

The new pass queries are gated by existing GPU timer/sub-detail controls and a
valid recording window. When those controls are off, no new pair is reserved or
written. Existing auto-tuner timer requests and the independent sampled rig timer
retain their pre-existing behavior. OFF does not claim removal of those separate
instruments when explicitly requested by another feature.

## Shadow state

`RenderVKShadowRigCullState` is live, Persist0, default off. It records effective
sun RT/projector mode, view-count range, dispatch/no-view/other-skip recording counts,
last skip reason and first/last app frame. It does not dispatch a diagnostic kernel,
read back bounds or change caster selection/cache/LOD. Counts prove CPU recording,
not successful GPU submission. This is the useful state portion of V5, with none
of its experimental cull cache.

## Validation and overhead

Production-math and extracted actual RAII recorder fixtures cover capacity refusal,
multiview reservations, cancellation, destructor/early-return closure, nested pair
ordering, disabled timers, independent front-end pool, unavailable async clocks,
clock wrap, interval unions and line parsing. Non-timer Vulkan command-token/order
audit compares every changed component with the base; query instrumentation is
excluded deliberately. All132 source checks pass; the final Linux configured build is still pending.

No hardware overhead number yet. `BenchGpuTimers` is a nonpersistent default-on
benchmark opt-out so a same-binary OFF/ON/OFF campaign can measure wall time without
inventing GPU timing when OFF. OFF reports must omit GPU samples, not label zero as
a measured value. Diagnostic overhead depends on the actual scope/query count and
must be reported with an A/A noise floor; no universal timing estimate is asserted.

The parent requested75c5e64534 alpha merge repair is included in the base; temporary bec94223e6 was dropped, not part of this feature.
