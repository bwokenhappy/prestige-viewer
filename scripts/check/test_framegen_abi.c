/* Compile-only test of the frame generation block ("slip_fg") of contrib/ngxshim/include/
 * slipstream_ngx.h: plain C99 and C++17 and with SLIP_NR_NO_PROTOTYPES; slip_nr_fg_capabilities and
 * slip_nr_fg_eval begin with struct_size, keep their first release's sizes (*_SIZE_V1) and field places; every PFN_slip_nr_fg_* typedef
 * matches its prototype (a mismatch is a compile error). Built by
 * scripts/check/checks/framegen_abi.py. */
#include <stddef.h>
#include "slipstream_ngx.h"

#if defined(__cplusplus)
#  define SA(c, m) static_assert(c, m)
#else
#  define SA(c, m) _Static_assert(c, m)
#endif

SA(SLIP_NR_ABI_VERSION == 1u, "slip_nr ABI 1 (frame generation only grows it)");
SA(offsetof(slip_nr_fg_capabilities, struct_size) == 0, "slip_nr_fg_capabilities");
SA(offsetof(slip_nr_fg_eval, struct_size) == 0, "slip_nr_fg_eval");
/* The first release's sizes (libraries 1.3.0 / 1.3.1): a callee accepts any struct_size from these up and
 * reads or writes only what fits, so either side may be the newer one. They never change. */
SA(SLIP_NR_FG_CAPABILITIES_SIZE_V1 == 28u, "slip_nr_fg_capabilities: seven 32-bit fields in its first release");
SA(offsetof(slip_nr_fg_capabilities, application_id_set) == 24, "the first release's last field");
/* appended by library 1.3.2: why frame generation cannot run */
SA(offsetof(slip_nr_fg_capabilities, unavailable_reason) == SLIP_NR_FG_CAPABILITIES_SIZE_V1, "appended, never inserted");
SA(sizeof(slip_nr_fg_capabilities) == 32, "slip_nr_fg_capabilities: eight 32-bit fields");
SA(SLIP_FG_WHY_NOT_STATED == 0u && SLIP_FG_WHY_LIBRARY_MISSING == 1u && SLIP_FG_WHY_GPU_SCHEDULING_OFF == 2u
   && SLIP_FG_WHY_QUERY_FAILED == 3u, "the reasons keep their values");
/* slip_nr_fg_eval: its first release's size is the end of frame_count; the fields keep their places (a
 * reordered matrix would compile on both sides and give wrong pictures). */
SA(SLIP_NR_FG_EVAL_SIZE_V1 == offsetof(slip_nr_fg_eval, frame_count) + sizeof(uint32_t), "slip_nr_fg_eval V1");
SA(SLIP_NR_FG_EVAL_SIZE_V1 <= sizeof(slip_nr_fg_eval), "slip_nr_fg_eval only grows");
SA(offsetof(slip_nr_fg_eval, depth) == offsetof(slip_nr_fg_eval, color) + sizeof(slip_nr_image), "color, depth");
SA(offsetof(slip_nr_fg_eval, motion) == offsetof(slip_nr_fg_eval, depth) + sizeof(slip_nr_image), "depth, motion");
SA(offsetof(slip_nr_fg_eval, output) == offsetof(slip_nr_fg_eval, motion) + sizeof(slip_nr_image), "motion, output");
SA(offsetof(slip_nr_fg_eval, view_to_clip) == offsetof(slip_nr_fg_eval, output) + sizeof(slip_nr_image), "output, view_to_clip");
SA(offsetof(slip_nr_fg_eval, clip_to_view) == offsetof(slip_nr_fg_eval, view_to_clip) + 64, "view_to_clip, clip_to_view");
SA(offsetof(slip_nr_fg_eval, clip_to_prev_clip) == offsetof(slip_nr_fg_eval, view_to_clip) + 128, "clip_to_prev_clip");
SA(offsetof(slip_nr_fg_eval, prev_clip_to_clip) == offsetof(slip_nr_fg_eval, view_to_clip) + 192, "prev_clip_to_clip");
SA(offsetof(slip_nr_fg_eval, camera_pos) == offsetof(slip_nr_fg_eval, view_to_clip) + 256, "camera_pos");
SA(offsetof(slip_nr_fg_eval, camera_up) == offsetof(slip_nr_fg_eval, camera_pos) + 12, "camera_up");
SA(offsetof(slip_nr_fg_eval, camera_right) == offsetof(slip_nr_fg_eval, camera_pos) + 24, "camera_right");
SA(offsetof(slip_nr_fg_eval, camera_fwd) == offsetof(slip_nr_fg_eval, camera_pos) + 36, "camera_fwd");
SA(offsetof(slip_nr_fg_eval, jitter) == offsetof(slip_nr_fg_eval, camera_pos) + 48, "jitter");
SA(offsetof(slip_nr_fg_eval, mvec_scale) == offsetof(slip_nr_fg_eval, camera_pos) + 56, "mvec_scale");
SA(offsetof(slip_nr_fg_eval, camera_near) == offsetof(slip_nr_fg_eval, camera_pos) + 64, "camera_near");
SA(offsetof(slip_nr_fg_eval, camera_far) == offsetof(slip_nr_fg_eval, camera_pos) + 68, "camera_far");
SA(offsetof(slip_nr_fg_eval, camera_fov) == offsetof(slip_nr_fg_eval, camera_pos) + 72, "camera_fov");
SA(offsetof(slip_nr_fg_eval, camera_aspect) == offsetof(slip_nr_fg_eval, camera_pos) + 76, "camera_aspect");
SA(offsetof(slip_nr_fg_eval, depth_inverted) == offsetof(slip_nr_fg_eval, camera_pos) + 80, "depth_inverted");
SA(offsetof(slip_nr_fg_eval, color_hdr) == offsetof(slip_nr_fg_eval, camera_pos) + 84, "color_hdr");
SA(offsetof(slip_nr_fg_eval, reset) == offsetof(slip_nr_fg_eval, camera_pos) + 88, "reset");
SA(offsetof(slip_nr_fg_eval, frame_index) == offsetof(slip_nr_fg_eval, camera_pos) + 92, "frame_index");
SA(offsetof(slip_nr_fg_eval, frame_count) == offsetof(slip_nr_fg_eval, camera_pos) + 96, "frame_count");

int main(void)
{
#if !defined(SLIP_NR_NO_PROTOTYPES)
    PFN_slip_nr_fg_device_extensions f4 = slip_nr_fg_device_extensions;
    PFN_slip_nr_fg_caps f0 = slip_nr_fg_caps;
    PFN_slip_nr_fg_create f1 = slip_nr_fg_create;
    PFN_slip_nr_fg_evaluate f2 = slip_nr_fg_evaluate;
    PFN_slip_nr_fg_release f3 = slip_nr_fg_release;
    (void)f0; (void)f1; (void)f2; (void)f3; (void)f4;
#endif
    const char* names[] = { SLIP_FG_FN_CAPS, SLIP_FG_FN_CREATE, SLIP_FG_FN_EVALUATE, SLIP_FG_FN_RELEASE,
                            SLIP_FG_FN_DEVICE_EXTENSIONS };
    return names[0][0] == 's' ? 0 : 1;
}
