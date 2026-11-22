/**
 * @file llvrbodyheadset.h
 * @brief VR headset body tracking (#1583, part of #1565): the runtime's own body tracking as a
 *        body source -- Quest body tracking over Quest Link / Air Link and Virtual Desktop's VDXR
 *        (XR_FB_body_tracking; legs with XR_META_body_tracking_full_body, inside-out or generated
 *        by XR_META_body_tracking_fidelity), HTC (XR_HTC_body_tracking), Pico (XR_BD_body_tracking).
 *        No trackers worn.
 *
 * Each VR frame LLVRBody asks poll(want, ...): want = SlipstreamVRBodySource is Headset, or Auto
 * while the runtime lists no trackers. The first want creates the runtime's body tracker
 * (LLXrSession::requestBodyTracker, fidelity from SlipstreamVRBodyFidelity); !want destroys it.
 * Without a tracker nothing is located: VR with another source, or none, pays one cached
 * setting read. Auto also lets go of a tracker that sees no body for 10 s (body tracking off in
 * the headset) and re-checks every 30 s. The desktop never calls in. Support is re-read at the
 * session's READY / FOCUSED events (LLXrSession::readBodySupport), so a runtime that starts
 * forwarding body data later is noticed.
 *
 * The joints become LLVRBody roles (llvrbodyheadsetmath.h: hips, chest, feet at the ankles,
 * knees as the legs' poles), already the bones themselves (LLVRBodyFrame::bonePoses). A joint
 * dropping out for up to 0.15 s holds its last pose (no fade pumping on a flicker); longer, the
 * limb fades back to the animation like a lost tracker. Generated legs (low fidelity, or joints
 * the runtime marks untracked) are used but marked, and smoothed more (lower confidence).
 *
 * Log "VRBody": the joint set and fidelity when the tracker starts, body tracked / lost, and
 * every 60 s while it runs the valid / tracked joint counts.
 */
#pragma once

#include "llvrbodysource.h"

class LLVRBodyHeadset
{
public:
    // The runtime offers headset body tracking (extension + system support). One test.
    static bool offered();
    // Every VR frame (LLVRBody::update). want: this source is selected (see above). backoff:
    // selected by Auto -- after 10 s without a body the tracker is released and only re-checked
    // every 30 s (one create, ~2 s of locates, kept only if a body shows), so Auto never keeps a
    // tracker for someone who never turned body tracking on. True: `out` holds this frame's
    // poses (a tracker exists and the runtime tracks a body).
    static bool poll(bool want, bool backoff, F32 dt, LLVRBodyFrame& out);
};
