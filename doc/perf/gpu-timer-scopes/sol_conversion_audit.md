# SOL conversion audit — source frozen

No build, GPU run, reservation, child agent or commit performed by SOL. Parent
owns integration/build/tests. Static context-checked patches preserve the order
and arguments of existing Vulkan commands in48 renderer functions and3 RT
functions (reproducible with `python3 scripts/perf/audit_gpu_scope_commands.py --base 75c5e64534`).
`git diff --check` passes. Pair-query overhead is unmeasured.

## Actual work now scoped

- RT: static BLAS build/compact, size queries, uploads/guard transfers, instances,
  terrain copy/build, pose motion, rig copies/refits, cluster coverage, TLAS prepare,
  KHR TLAS build and used-bitset readback. Empty arrays/zero runs do not create a
  named pass. Cluster record/PTLAS append `&R`; Water scopes cluster/OMM children.
- Probes: placement/schedule phases, patch, face cull/rig cull, sky/clears,
  G-buffer/light/face/alpha commands, filter mips/filter/encode and trace.
  Disjoint phases with barriers sum under the same frame/name; barriers are outside.
- Shadows: positive-nv rig upload/kernel only, sun/projector actual clears,
  draws/copies and named child cull phases. RT nv0 creates no rig-kernel scope.
- Particle sim/emit/compact, sort, indirect generation, uploads/readbacks,
  draw/shadow/lighting and temporal glow; AO depth mips/kernel/temporal/blur,
  RT-mask trace/temporal/spatial and Hi-Z/occlusion phases.
- Selection/HUD/wireframe actual draws; mirror/occlusion; water exclusion;
  SSR copies, native RCAS, output/depth upscale, glow, flare/lens/eyedropper,
  exposure/tonemap/LUT, snapshot accumulation, DoF/bloom/cross-filter/AA/final/sphere.

## Boundary-only / split names

All existing `gpuSubMark` calls remain breadcrumbs. They must NOT regain
previous-mark attribution or parent wrappers around already scoped children.

- `c_sun_prep` is release/barrier preparation only. Sun detail group boundaries
  (`c_sun_casters`, `g_sun_draw`, `g_sun_overlay`) are not extra nested timers:
  actual sun commands aggregate as `c_shadows`; child culls use `c_sun_cull` /
  `c_sun_dyncull` with detail enabled. Weather, particles and clusters own their
  child timings. `g_sun_snow` is a child-work boundary.
- Projector detail boundaries (`c_proj_records`, `g_proj_static`, `g_proj_copy`,
  `g_proj_dyn`) do not wrap children: direct work aggregates as `c_projshadow`;
  culls use `c_proj_cull` / `c_proj_dyncull` when detail is enabled.
- `pr_sky_begin` / `pr_sky_boundary` / `pr_face_sky` are structural/synthetic
  boundaries; actual work is `pr_sky` plus `pr_clear`, without a second face envelope.
- `p_glow`, `g_ao_temporal`, `c_hiz`, `aoKernelMark()` and the overlap/AO markers
  are caller breadcrumbs only: the actual leaf helpers carry scopes.
- `p_vr_sel` is the boundary after individually scoped selection/wire/outline
  children and the trailing WAW barrier. `p_vr_overlay` is an external callback
  boundary: this worker cannot isolate the callback's internal commands/barriers.
  Any unscoped opaque-hook work contributes to a gap; it is not proven pure idle.
- Resolve fallback has only state/barrier work plus the already scoped upscale
  helper; no empty fallback pass. TAA input-layout helpers are barriers only.

SDK evaluations are the practical opaque boundary: prepared FSR is scoped only
at `mFsr[v]->record`, not its renderer preparation/trailing barriers. DLSS is
scoped at its bool-returning evaluation; false cancels its named sample. SDK /
component-internal synchronization is included and cannot be separated at this
call site. FSR/DLSS copies have separate names. Do not add whole-function callers.

`dispatchCull` now has final optional `const char* timerName="c_cull"` after
`slot_b`; it scopes each actual phase, leaving barriers outside. Pre-recordWorld
probe/mirror/sun/projector callers supply their existing names; parent owns later
callers. No global bucketDraw wrapper was added.

## Shadow state and benchmark control

The inactive proposal was superseded by parent's `vk_gpu_shadow_state.h` and
removed. Parent integrated its opt-in setting and all early-return/success calls;
no V5 cache/perf code was imported. Recording counts are CPU observations, not
proof that a submitted command executed.

Minimal benchmark port: `BenchGpuTimers` defaults true, Persist0; startRun saves
and applies its request using existing restoration paths. Metadata reports actual
GPU-timer state; empty GPU statistics are omitted. No readiness/harness changes
or slbench.h changes. The production4-combination enable/report/restore fixture
plus unavailable-GPU omission guard is prepared as `bench_timer_control.py`;
parent reports the target check passed.
