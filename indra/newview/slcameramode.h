/**
 * @file slcameramode.h
 * @brief Camera mode (#1572): a snapshot becomes a DSLR-style camera. The viewer UI and the
 *        HUD attachments are hidden, the world keeps rendering live, and a camera overlay
 *        (slcameraview.h: one document on the new interface kit, #1572) shows in their place.
 *        The camera itself is SLCameraModel, a view onto the existing renderer and Photo Studio
 *        settings; it holds no copy of them. This class enters, leaves and routes input.
 *
 * Costs nothing while closed: every hook is one read of @ref SLCameraMode::active().
 *
 * Hiding the UI goes through the existing "UI hidden" state (RENDER_DEBUG_FEATURE_UI, Ctrl+Alt+F1):
 * its input routing (clicks reach the camera tools, never invisible views) and its per-frame
 * keyboard focus release are what a camera needs. Entry saves that state, the HUD attachment
 * switch and the keyboard focus; exit puts all three back.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SL_CAMERAMODE_H
#define SL_CAMERAMODE_H

#include "indra_constants.h"
#include "stdtypes.h"

class LLVector3;

class SLCameraMode
{
public:
    static bool active() { return sActive; }

    // ENTRY POINTS (toolbar button, View > Snapshot, Ctrl+Shift+S, the new interface's bar):
    // camera mode when "Use the camera overlay for snapshots" (SLCameraModeOverlay) is on, the
    // classic snapshot window otherwise. While either is up, the same entry closes it.
    static void toggleSnapshot();
    // Camera mode or the classic snapshot window is up (the toolbar button's pressed look).
    static bool snapshotOpen();

    static void start();
    static void stop();
    // "Open classic snapshot window": leave camera mode, open the snapshot window.
    static void openClassic();

    // The VK UI pass, every frame while active (the UI tree is not drawn): the overlay's place in
    // the UI layer and a right-drag orbit. Also notices the UI coming back by another route
    // (Ctrl+Alt+F1, Escape's restore binding) and leaves; without RmlUi, the classic window.
    static void draw();
    // The main loop's snapshot point (after the frame, where the snapshot floaters take theirs),
    // only while active: the drive's self-timer, burst and bracketing shots (#1577).
    static void update();

    // INPUT, only while active. x / y are UI-scaled window coordinates (mCurrentMousePoint).
    // The overlay takes events over its elements first. On the picture (slcamerainput.h): a
    // click focuses there, a double-click focuses and shoots, the window-activation click does
    // nothing; right-click opens quick settings, a right-drag orbits without turning the avatar;
    // modified clicks go on to the camera tools (Alt-orbit and friends).
    static bool handleMouseClick(S32 x, S32 y, MASK mask, EMouseClickType click, bool down);
    // The wheel (slcamin::wheelTarget): quick shot moves the camera in or out, Controls open
    // turns the marked dial; over a value that value, over the Grid chip the grid, over the
    // tilt knob the tilt; Shift: the second dial, Ctrl: the zoom ring, Alt: the camera.
    // Always taken while active.
    static void handleScrollWheel(S32 x, S32 y, S32 clicks);
    // A fraction of a wheel step (touchpads): accumulated into whole dial clicks; Alt moves
    // the camera by the fraction as usual.
    static void handleScrollWheelFraction(F32 notches, F32 camera_delta);
    // The camera-mode key commands (key_commands.xml, rebindable in Preferences > Controls):
    // shoot, AF-ON, AF-L, DoF preview (held), review, Q menu, the bar (H), help (F1), exit. They
    // stand back while a field, popover or card is open. Other keys go on (movement, camera keys).
    static bool handleKey(KEY key, MASK mask);
    // A held command's release (DoF preview).
    static bool handleKeyUp(KEY key, MASK mask);
    // A typed character: taken while a field of the overlay (a size, a preset's name) has it.
    static bool handleUnicodeChar(llwchar uni_char);
    // The pointer is over the overlay: its cursor (I-beam over a field, ...) instead of the tool's.
    static bool handleHover(S32 x, S32 y, MASK mask);
    // A menu shortcut camera mode swallows: one that would open or act on the hidden interface
    // (Ctrl+P, Ctrl+I, ...). Those that act on the world, the avatar or the camera still work.
    static bool blocksMenuShortcut(KEY key, MASK mask);
    // The test behind it, for any mode that hides the interface (Video Studio, #1668): a menu
    // shortcut that is not one of those that act on the world, the avatar or the camera.
    static bool shortcutReachesHiddenUI(KEY key, MASK mask);

    // DEPTH OF FIELD (buildVKDoF): the point the camera's autofocus locked on, in agent space.
    // False while camera mode is closed or nothing is focused yet.
    static bool focusPoint(LLVector3& agent_pos);
    // <Prestige> for the renderer (llappviewer): focus peaking on (MF; drawn with the depth of
    // field), and the exposure lock (AE-L: the auto exposure holds its value)
    static bool focusPeaking();
    static bool exposureLocked();
    // </Prestige>

private:
    // handleKey for one arrival of a key (Enter's two arrivals are one press there).
    static bool handleKeyOnce(KEY key, MASK mask);
    static bool sActive;
};

#endif // SL_CAMERAMODE_H
