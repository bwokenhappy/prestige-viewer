/**
 * @file prestigecamerafocus.cpp
 * @brief Camera mode's focus and exposure helpers (see prestigecamerafocus.h).
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

#include "llviewerprecompiledheaders.h"

#include "prestigecamerafocus.h"

#include "llagent.h"
#include "llcharacter.h"
#include "lljoint.h"
#include "llui.h"              // make_ui_sound
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"

#include <algorithm>
#include <cmath>

namespace PrestigeCameraFocus
{
namespace
{
F64 now() { return LLTimer::getTotalSeconds(); }

// The joint's position as posed this frame: composed against the live root (#610: a GPU-animated
// avatar's cached joint transforms do not follow it), and asked for in the next GPU readback.
bool livePosition(LLJoint* joint, U32 stamp, LLVector3& out)
{
    if (!joint)
        return false;
    joint->gpuWant();
    out = joint->getWorldMatrixLive(stamp).getTranslation();
    return out.isFinite() && !out.isExactlyZero();
}

std::vector<LLUUID> sGroup;

struct Confirmation
{
    U32 state = CONFIRM_NONE;
    F64 until = 0.0;     // SEARCHING: confirmed then; FAILED: shown until then
    F64 beepAt = 0.0;    // the second beep (0: none due)
};
Confirmation sConfirm;

bool sExposureLocked = false;

// A short UI click through the viewer's UI sounds: off with PrestigeCameraFocusBeep, the UI click
// switched off (PlayModeUISndClick, inside make_ui_sound), or the sound muted.
void beep()
{
    static LLCachedControl<bool> s_beep(gSavedSettings, "PrestigeCameraFocusBeep", true);
    static LLCachedControl<bool> s_mute_audio(gSavedSettings, "MuteAudio", false);
    static LLCachedControl<bool> s_mute_ui(gSavedSettings, "MuteUI", false);
    if (!s_beep || s_mute_audio || s_mute_ui)
        return;
    make_ui_sound("UISndClick");
}
} // namespace

// ---- faces ----

LLVOAvatar* personOf(LLViewerObject* obj)
{
    if (!obj || obj->isDead() || obj->isHUDAttachment())
        return nullptr;
    LLVOAvatar* av = obj->asAvatar();
    if (!av)
        av = obj->getAvatar();          // a worn item, rigged or not (and an animesh's own avatar)
    if (!av)
        av = obj->getAvatarAncestor();
    if (!av || av->isDead() || av->isControlAvatar())
        return nullptr;                 // animesh: its rig's eye bones are not a face to trust
    return av;
}

bool eyesOf(LLVOAvatar* av, LLVector3& agent_pos)
{
    if (!av || av->isDead())
        return false;
    const U32 stamp = LLJoint::nextLiveStamp();
    const LLVector3 body = av->getRenderPosition();
    // A joint left where the avatar stood long ago is no face: within a few metres of the body.
    auto near_body = [&body](const LLVector3& p) { return dist_vec_squared(p, body) < 16.f; };
    LLVector3 l, r;
    if (livePosition(av->mEyeLeftp, stamp, l) && livePosition(av->mEyeRightp, stamp, r))
    {
        const LLVector3 mid = (l + r) * 0.5f;
        if (near_body(mid))
        {
            agent_pos = mid;
            return true;
        }
    }
    LLVector3 head;
    if (livePosition(av->mHeadp, stamp, head) && near_body(head))
    {
        agent_pos = head;
        return true;
    }
    return false;
}

bool eyesOf(const LLUUID& id, LLVector3& agent_pos)
{
    LLViewerObject* obj = id.notNull() ? gObjectList.findObject(id) : nullptr;
    LLVOAvatar* av = obj ? obj->asAvatar() : nullptr;
    return av && !av->isControlAvatar() && eyesOf(av, agent_pos);
}

bool toScreen(const LLVector3& agent_pos, F32& x, F32& y)
{
    if (!gViewerWindow)
        return false;
    // LLViewerWindow::mouseDirectionGlobal, inverted: the same ray the AF point's pick casts.
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 v = agent_pos - cam->getOrigin();
    const F32 z = v * cam->getAtAxis();
    if (z <= cam->getNear())
        return false;
    const LLRect view = gViewerWindow->getWorldViewRectScaled();
    const F32 dist = (F32)gViewerWindow->getWorldViewHeightScaled() * 0.5f / tanf(cam->getView() * 0.5f);
    x = (F32)view.getCenterX() - (v * cam->getLeftAxis()) / z * dist;
    y = (F32)view.getCenterY() + (v * cam->getUpAxis()) / z * dist;
    return true;
}

LLVOAvatar* faceNear(S32 x, S32 y, F32 half_box, F32 max_depth)
{
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    LLVOAvatar* best = nullptr;
    F32 best_d2 = half_box * half_box;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* av = static_cast<LLVOAvatar*>(character);
        if (!av || av->isDead() || av->isControlAvatar() || (av->isSelf() && !gAgent.needsRenderAvatar()))
            continue;
        // (cheap first: a body far behind the hit, or behind the camera, has no face to find)
        const F32 body_depth = cam->getAtAxis() * (av->getRenderPosition() - cam->getOrigin());
        if (body_depth < -2.f || body_depth > max_depth + 2.f)
            continue;
        LLVector3 eyes;
        F32 sx = 0.f, sy = 0.f;
        if (!eyesOf(av, eyes) || !toScreen(eyes, sx, sy))
            continue;
        if (cam->getAtAxis() * (eyes - cam->getOrigin()) > max_depth + 0.25f)
            continue;   // behind what the AF point hit
        const F32 dx = sx - (F32)x, dy = sy - (F32)y, d2 = dx * dx + dy * dy;
        if (d2 <= best_d2)
        {
            best_d2 = d2;
            best = av;
        }
    }
    return best;
}

// ---- group focus ----

const std::vector<LLUUID>& group()
{
    return sGroup;
}

bool inGroup(const LLUUID& id)
{
    return std::find(sGroup.begin(), sGroup.end(), id) != sGroup.end();
}

bool toggle(const LLUUID& id, const LLUUID& seed)
{
    if (id.isNull())
        return false;
    auto it = std::find(sGroup.begin(), sGroup.end(), id);
    if (it != sGroup.end())
    {
        sGroup.erase(it);
        return false;
    }
    if (sGroup.empty() && seed.notNull() && seed != id)
        sGroup.push_back(seed);
    if (sGroup.size() >= MAX_GROUP)
        return false;
    sGroup.push_back(id);
    return true;
}

void clearGroup()
{
    sGroup.clear();
}

bool groupDepths(F32& near_m, F32& far_m)
{
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    bool any = false;
    for (const LLUUID& id : sGroup)
    {
        LLVector3 eyes;
        if (!eyesOf(id, eyes))
            continue;
        const F32 d = cam->getAtAxis() * (eyes - cam->getOrigin());
        if (d <= cam->getNear())
            continue;
        near_m = any ? llmin(near_m, d) : d;
        far_m = any ? llmax(far_m, d) : d;
        any = true;
    }
    return any;
}

// ---- the depth of field ----

void lens(F32& focal_m, F32& tan_px)
{
    // buildVKDoF: the sensor (dv) from CameraFocalLength at CameraFieldOfView, the focal length
    // that gives the camera's field of view on it, one pixel of the window's height.
    static LLCachedControl<F32> s_focal_length(gSavedSettings, "CameraFocalLength", 50.f);
    static LLCachedControl<F32> s_fov(gSavedSettings, "CameraFieldOfView", 60.f);
    const F32 fov = llmax(LLViewerCamera::getInstance()->getView(), 0.01f);
    const F32 dv = 2.f * (F32)s_focal_length * tanf((F32)s_fov * DEG_TO_RAD * 0.5f);
    focal_m = dv / (2.f * tanf(fov * 0.5f)) / 1000.f;
    const F32 height = gViewerWindow ? (F32)llmax(gViewerWindow->getWindowHeightRaw(), 1) : 1080.f;
    tan_px = tanf(fov / height);
}

F32 groupFocusDistance(F32 near_m, F32 far_m)
{
    near_m = llmax(near_m, 0.01f);
    far_m = llmax(far_m, near_m);
    return 2.f / (1.f / near_m + 1.f / far_m);
}

F32 fNumberFor(F32 near_m, F32 far_m)
{
    near_m = llmax(near_m, 0.01f);
    far_m = llmax(far_m, near_m);
    const F32 spread = 1.f / near_m - 1.f / far_m;   // focused at the harmonic mean: half each way
    if (spread <= 0.f)
        return 0.f;
    F32 focal_m = 0.05f, tan_px = 0.001f;
    lens(focal_m, tan_px);
    return 1.414f * focal_m * (spread * 0.5f) / (SHARP_COC_PX * llmax(tan_px, 1e-6f));
}

// ---- focus confirmation ----

void search()
{
    // Confirmed once the depth of field has (mostly) eased to the new focus.
    static LLCachedControl<F32> s_transition(gSavedSettings, "CameraFocusTransitionTime", 0.5f);
    sConfirm.state = CONFIRM_SEARCHING;
    sConfirm.until = now() + llclamp((F64)(F32)s_transition, 0.12, 0.4);
    sConfirm.beepAt = 0.0;
}

void fail()
{
    sConfirm.state = CONFIRM_FAILED;
    sConfirm.until = now() + 1.5;
    sConfirm.beepAt = 0.0;
}

void clearConfirm()
{
    sConfirm = Confirmation();
}

U32 confirm()
{
    return sConfirm.state;
}

void tick()
{
    const F64 t = now();
    if (sConfirm.state == CONFIRM_SEARCHING && t >= sConfirm.until)
    {
        sConfirm.state = CONFIRM_OK;
        beep();
        sConfirm.beepAt = t + 0.09;   // the second click, ~90 ms on
    }
    else if (sConfirm.state == CONFIRM_FAILED && t >= sConfirm.until)
    {
        sConfirm.state = CONFIRM_NONE;
    }
    if (sConfirm.beepAt > 0.0 && t >= sConfirm.beepAt)
    {
        sConfirm.beepAt = 0.0;
        beep();
    }
}

// ---- AE-L ----

bool exposureLocked()
{
    return sExposureLocked;
}

void setExposureLocked(bool on)
{
    sExposureLocked = on;
}
} // namespace PrestigeCameraFocus
