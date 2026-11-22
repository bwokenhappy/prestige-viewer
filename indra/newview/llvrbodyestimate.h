/**
 * @file llvrbodyestimate.h
 * @brief #1586 VR body estimate from the headset and the two controllers (3-point), for setups
 *        without body trackers (Valve Index, Quest without body tracking): a body source
 *        (llvrbodysource.h, kind ESTIMATE; SlipstreamVRBodySource "Estimate", and Auto's last
 *        choice while SlipstreamVRBodyEstimate is on).
 *
 * Bone poses (no T-pose): the hips (waist role) and the feet (left / right foot roles), in the room
 * (OpenXR app space) like any source; LLVRBody's layer does the rest -- the spine bends toward the
 * head as on the tracker path, the legs bend with the leg IK onto the planted feet (crouching),
 * the arms come from #1584. The math (llvrbodyestimatemath.h): a smoothed body heading with a
 * torso-twist budget (nudged by the hands), hips under the neck standing tall and over the feet
 * while bending / crouching, feet planted in the room with short eased steps when the body drifts
 * or turns past a limit, and a settle step when you stop.
 *
 * Calibration: wrist "Body" measures your standing eye height (as for headset body tracking);
 * until then the saved one / SlipstreamVREyeHeight. While the avatar walks or flies with the
 * thumbsticks (or sits) the estimate's roles drop out, so the walk / fly / sit animations play;
 * standing still again plants the feet anew. Walking is the movement controls (stick) or the
 * avatar's speed through a debounced gate (enter: 0.4 m/s or the stick for 0.15 s; leave: under
 * 0.15 m/s with no stick for 0.4 s): the sim's speed jitters on a standing avatar.
 *
 * Cost: VR only, and only while chosen; a few dozen float operations a frame.
 * Log "VRBody": estimate on / off, locomotion hands the legs to the animation (once per change).
 */
#pragma once

#include "stdtypes.h"

struct LLVRBodyFrame;
struct LLXrHeadPose;

namespace LLVRBodyEstimate
{
// Every VR frame while it may be chosen (want), else with want = false (forgets its state).
// eyeH: your standing eye height; floorY: the floor's app-space y. True: `out` holds the body.
// bodyYaw (haveBodyYaw): the avatar's facing in the room -- the one body heading in VR (#1584).
bool poll(bool want, const LLXrHeadPose& hp, F32 dt, F32 eyeH, F32 floorY, bool haveBodyYaw, F32 bodyYaw, LLVRBodyFrame& out);
} // namespace LLVRBodyEstimate
