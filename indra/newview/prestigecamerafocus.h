/**
 * @file prestigecamerafocus.h
 * @brief Camera mode's focus and exposure helpers (on top of SLCameraModel's AF-S / AF-C / MF):
 *        faces (an avatar's eyes from any pick on its body or what it wears), group focus
 *        (several people sharp at once: the focus distance and the f-number that cover them,
 *        from the renderer's own thin-lens depth of field), focus confirmation (the AF point
 *        turns green, a quick double beep) and the exposure lock (AE-L).
 *
 *        No drawing and no input routing: SLCameraModel calls it, the overlay reads it back
 *        through SLCameraModel.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef PRESTIGE_CAMERAFOCUS_H
#define PRESTIGE_CAMERAFOCUS_H

#include "lluuid.h"
#include "stdtypes.h"
#include "v3math.h"

#include <vector>

class LLViewerObject;
class LLVOAvatar;

namespace PrestigeCameraFocus
{
// ---- faces ----
// The person a pick landed on: the avatar itself (its body, a rigged mesh body), or the wearer of
// an attachment (rigged or not). Animesh and HUD attachments: nullptr (they keep point focus).
LLVOAvatar* personOf(LLViewerObject* obj);
// The point midway between the eyes (eye joints, live pose; the head joint when the eyes are
// missing), agent space. False when the avatar has no usable pose.
bool eyesOf(LLVOAvatar* av, LLVector3& agent_pos);
// The same for an avatar by id (false: gone, or not a person).
bool eyesOf(const LLUUID& id, LLVector3& agent_pos);
// Face detection in the AF area: the person whose eyes are nearest the window point (x, y, UI
// units), at most `half_box` UI units from it, in front of the camera and not behind `max_depth`
// (metres along the view axis: the surface the AF point's pick hit hides what is behind it).
LLVOAvatar* faceNear(S32 x, S32 y, F32 half_box, F32 max_depth);
// An agent-space point in window UI coordinates, the inverse of the pick ray (false: behind the camera).
bool toScreen(const LLVector3& agent_pos, F32& x, F32& y);

// ---- group focus ----
constexpr size_t MAX_GROUP = 8;
const std::vector<LLUUID>& group();
bool inGroup(const LLUUID& id);
// Shift+click on a person: in (true) or out again (false). `seed`: the person focused before the
// first Shift+click, who joins the group first (null: none).
bool toggle(const LLUUID& id, const LLUUID& seed);
void clearGroup();
// The nearest and farthest of the group's eyes, metres along the view axis (only those in front
// of the camera). False when none is.
bool groupDepths(F32& near_m, F32& far_m);

// ---- the depth of field (llappviewer buildVKDoF, LL's thin lens) ----
// The blur circle (px) of a point at depth d with the focus at D and the f-number N:
//   CoC = 1.414 * f / (N * tan_px) * |1/d - 1/D|
// (f: the renderer's focal length, tan_px: one pixel's angle). "Sharp": at most SHARP_COC_PX,
// the same limit focus peaking draws.
constexpr F32 SHARP_COC_PX = 1.f;
// The focus distance that shares the blur evenly between the nearest and farthest subject (the
// harmonic mean: the CoC is linear in 1/d).
F32 groupFocusDistance(F32 near_m, F32 far_m);
// The smallest f-number that keeps near..far sharp, focused at groupFocusDistance (0: one depth).
F32 fNumberFor(F32 near_m, F32 far_m);
// The lens as buildVKDoF derives it: focal length (m) and the tangent of one pixel's angle.
void lens(F32& focal_m, F32& tan_px);

// ---- focus confirmation ----
enum Confirm : U32
{
    CONFIRM_NONE = 0,
    CONFIRM_SEARCHING = 1,   // a new focus is on its way (the depth of field eases to it)
    CONFIRM_OK = 2,          // achieved: the AF point is green, the double beep played
    CONFIRM_FAILED = 3       // nothing to focus on (sky) or the subject was lost: red, briefly
};
void search();
void fail();
void clearConfirm();
U32  confirm();
// The main loop (camera mode only): the search settles, the second beep follows the first.
void tick();

// ---- the exposure lock (AE-L) ----
// While set, the renderer's auto exposure holds the value it has (llappviewer: the adaptation's
// speed target), until unlocked or camera mode closes.
bool exposureLocked();
void setExposureLocked(bool on);
} // namespace PrestigeCameraFocus

#endif // PRESTIGE_CAMERAFOCUS_H
