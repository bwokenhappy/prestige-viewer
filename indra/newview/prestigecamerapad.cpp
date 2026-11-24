/**
 * @file prestigecamerapad.cpp
 * @brief Camera mode's on-screen camera pad (see prestigecamerapad.h).
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

#include "prestigecamerapad.h"

#include "slcameramodel.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"

#include <cmath>

namespace PrestigeCameraPad
{
namespace
{
// Full pull: a quarter turn a second (orbit, looking around), 4 m a second (free).
constexpr F32 kTurnRate   = 90.f * DEG_TO_RAD;
constexpr F32 kMoveRate   = 4.f;
// The wheel over the pad in Free: half a metre up or down a click.
constexpr F32 kWheelLift  = 0.5f;
// The knob's centre does nothing (a resting finger); past it the speed grows with the square of the
// pull, so small pulls are fine moves.
constexpr F32 kDeadZone   = 0.08f;
// First person: what the camera looks at, straight ahead of the eyes, and how far in front of the
// eyes it sits (clear of the face).
constexpr F32 kLookAhead  = 4.f;
constexpr F32 kEyeForward = 0.12f;
constexpr F32 kPitchLimit = 85.f * DEG_TO_RAD;

struct State
{
    bool held = false;
    F32  yaw = 0.f, pitch = 0.f; // first person: where the eyes look (world frame, radians)
};
State sState;

F32 shaped(F32 v)
{
    const F32 a = std::fabs(v);
    if (a <= kDeadZone)
        return 0.f;
    const F32 t = llmin((a - kDeadZone) / (1.f - kDeadZone), 1.f);
    return v < 0.f ? -t * t : t * t;
}

// The avatar's eyes (between them), global; false without an avatar.
bool eyesGlobal(LLVector3d& out)
{
    if (!isAgentAvatarValid())
        return false;
    LLVector3 eyes;
    if (gAgentAvatarp->mEyeLeftp && gAgentAvatarp->mEyeRightp)
        eyes = (gAgentAvatarp->mEyeLeftp->getWorldPosition() + gAgentAvatarp->mEyeRightp->getWorldPosition()) * 0.5f;
    else if (gAgentAvatarp->mHeadp)
        eyes = gAgentAvatarp->mHeadp->getWorldPosition() + LLVector3(0.f, 0.f, 0.08f);
    else
        return false;
    out = gAgent.getPosGlobalFromAgent(eyes);
    return true;
}

LLVector3 lookDirection()
{
    const F32 cp = cosf(sState.pitch);
    return LLVector3(cp * cosf(sState.yaw), cp * sinf(sState.yaw), sinf(sState.pitch));
}

// Where the view looks now, as yaw / pitch (first person starts from it: no jump in direction).
void takeViewDirection()
{
    const LLVector3 at = LLViewerCamera::getInstance()->getAtAxis();
    sState.yaw   = atan2f(at.mV[VY], at.mV[VX]);
    sState.pitch = llclamp(asinf(llclamp(at.mV[VZ], -1.f, 1.f)), -kPitchLimit, kPitchLimit);
}

// The camera at the eyes, looking along yaw / pitch. `glide`: the camera's usual short animation
// (choosing the mode); a held knob moves it at once.
void placeAtEyes(bool glide)
{
    LLVector3d eyes;
    if (!eyesGlobal(eyes))
        return;
    if (gAgentCamera.getFocusOnAvatar())
        gAgentCamera.setFocusOnAvatar(false, false); // the camera, not the avatar, now holds the view
    const LLVector3d dir(lookDirection());
    const LLVector3d cam = eyes + dir * (F64)kEyeForward;
    gAgentCamera.setCameraPosAndFocusGlobal(cam, cam + dir * (F64)kLookAhead, LLUUID::null);
    if (!glide)
        gAgentCamera.stopCameraAnimation();
}

// The first frame of a hold: the camera off the avatar, so moving it never turns the avatar.
void beginHold()
{
    sState.held = true;
    switch (mode())
    {
        case MODE_FIRST:
            takeViewDirection();
            placeAtEyes(false);
            break;
        default:
            gAgentCamera.unlockView();
            break;
    }
}
} // namespace

U32 mode()
{
    static LLCachedControl<U32> s_mode(gSavedSettings, "PrestigeCameraPadMode", 0);
    return llmin((U32)s_mode, (U32)MODE_COUNT - 1);
}

void setMode(U32 m)
{
    m = llmin(m, (U32)MODE_COUNT - 1);
    const U32 was = mode();
    gSavedSettings.setU32("PrestigeCameraPadMode", m);
    if (gAgentCamera.cameraMouselook())
        return;
    if (m == MODE_FIRST)
    {
        takeViewDirection();
        placeAtEyes(true);
    }
    else if (m == MODE_ORBIT && was != MODE_ORBIT && isAgentAvatarValid())
    {
        // Back to orbiting the avatar: the camera stays where it is and turns toward it.
        const LLVector3d cam = gAgentCamera.getCameraPositionGlobal();
        if (gAgentCamera.getFocusOnAvatar())
            gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setCameraPosAndFocusGlobal(cam, gAgent.getPositionGlobal(), gAgent.getID());
    }
}

void drive(F32 x, F32 y, F32 dt)
{
    if (gAgentCamera.cameraMouselook() || dt <= 0.f)
        return;
    if (!sState.held)
        beginHold();
    const F32 sx = shaped(llclamp(x, -1.f, 1.f)), sy = shaped(llclamp(y, -1.f, 1.f));
    if (sx == 0.f && sy == 0.f)
        return;
    dt = llmin(dt, 0.1f); // a long frame (a hitch) never throws the camera
    switch (mode())
    {
        case MODE_FREE:
            // Up: forward along the view; right: strafe right.
            if (sy != 0.f)
                gAgentCamera.cameraPanIn(sy * kMoveRate * dt);
            if (sx != 0.f)
                gAgentCamera.cameraPanLeft(-sx * kMoveRate * dt);
            break;
        case MODE_FIRST:
            // Right: look right (yaw is counter-clockwise from above); up: look up.
            sState.yaw -= sx * kTurnRate * dt;
            sState.pitch = llclamp(sState.pitch + sy * kTurnRate * dt, -kPitchLimit, kPitchLimit);
            placeAtEyes(false);
            break;
        default:
            // As the right-drag: the camera goes the way the knob is pulled, around what it looks at.
            if (sx != 0.f)
                gAgentCamera.cameraOrbitAround(-sx * kTurnRate * dt);
            if (sy != 0.f)
                gAgentCamera.cameraOrbitOver(-sy * kTurnRate * dt);
            break;
    }
}

void release()
{
    sState.held = false;
}

void wheel(S32 clicks)
{
    if (clicks == 0 || gAgentCamera.cameraMouselook())
        return;
    switch (mode())
    {
        case MODE_FREE:
            gAgentCamera.unlockView();
            gAgentCamera.cameraPanUp(-(F32)clicks * kWheelLift); // wheel up (away from you): up
            break;
        case MODE_FIRST:
            SLCameraModel::turn(SLCameraModel::Value::Focal, -clicks); // the zoom ring
            break;
        default:
            gAgentCamera.handleScrollWheel((F32)clicks); // in or out, as the wheel over the picture
            break;
    }
}
} // namespace PrestigeCameraPad
