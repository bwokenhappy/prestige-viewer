/**
 * @file prestigecamerapad.h
 * @brief Camera mode's on-screen camera pad: moving the camera without the keyboard.
 *
 * The overlay (slcameraview.cpp) draws the pad -- a circle with a knob and a mode switch -- and
 * reports how far the knob is pulled from the centre while it is held; this file turns that into
 * camera moves, once per frame from SLCameraMode::draw(). The knob is a joystick: the further it
 * is pulled, the faster the camera goes, for as long as it is held.
 *
 *   Orbit         around what the camera looks at (the avatar unless something else was focused),
 *                 as the right-drag does; the wheel over the pad moves in or out. The avatar never
 *                 turns: the view is unlocked from it first.
 *   Free          the camera moves on its own, like the flycam: up / down on the pad is forward /
 *                 back along the view, left / right strafes; the wheel over the pad is up / down.
 *   First person  from the avatar's eyes: the pad looks around (the avatar does not turn with it);
 *                 the wheel over the pad turns the zoom ring.
 *
 * The mode is remembered (PrestigeCameraPadMode). Only the existing LLAgentCamera moves are used,
 * so the camera keys, Alt-orbit and leaving camera mode behave as after any other camera move.
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

#ifndef PRESTIGE_CAMERAPAD_H
#define PRESTIGE_CAMERAPAD_H

#include "stdtypes.h"

namespace PrestigeCameraPad
{
enum Mode : U32
{
    MODE_ORBIT = 0,
    MODE_FREE  = 1,
    MODE_FIRST = 2,
    MODE_COUNT = 3
};

// PrestigeCameraPadMode.
U32  mode();
// A mode chosen on the pad: First person takes the camera to the avatar's eyes (a short glide),
// Orbit turns it toward the avatar so the next drag orbits around it; Free leaves it where it is.
void setMode(U32 mode);

// Each frame while the knob is held: its pull, -1..1 on each axis (x right, y up), and the frame's
// length in seconds. The first call of a hold readies the camera (unlocks it from the avatar).
void drive(F32 x, F32 y, F32 dt);
// Each frame the knob is not held (cheap): the hold, if any, ends.
void release();
// The wheel over the pad (clicks as the viewer counts them: positive = toward you).
void wheel(S32 clicks);
} // namespace PrestigeCameraPad

#endif // PRESTIGE_CAMERAPAD_H
