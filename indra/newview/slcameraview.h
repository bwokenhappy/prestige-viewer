/**
 * @file slcameraview.h
 * @brief Camera mode's overlay (#1572) on the new interface kit: one RmlUi document, a view onto
 *        SLCameraModel (rule 11.7: no camera logic here). The RmlUi host starts it when camera
 *        mode is up, in any UIRenderer mode, and draws it while the rest of the interface (and the
 *        rest of the RmlUi layer) is hidden. It is part of the UI layer, so it is never in a shot.
 *
 * The document is created at the first entry and stays resident (hidden) for the session.
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

#ifndef SL_CAMERAVIEW_H
#define SL_CAMERAVIEW_H

#include "indra_constants.h"
#include "stdtypes.h"
#include "slcamerainput.h"
#include "slcameramodel.h"

namespace Rml
{
class Context;
}

class SLCameraView
{
public:
    // Shows the document (created in `context` at the first call). False if it cannot load.
    static bool start(Rml::Context* context);
    // Hides it (it stays resident).
    static void stop();
    // The host's context goes away: forget the document (no RmlUi call).
    static void release();
    static bool active();
    // dp per UI unit for the camera document (the host's RmlUi dp ratio is its own times this).
    static float dpScale();
    // Each frame from the RmlUi host while active: refresh from the model. `density` is
    // framebuffer pixels per UI unit. True when the document changed (capture).
    static bool sync(float density);
    // In camera mode's UI pass: the RmlUi layer's z slot (once per frame).
    static void markLayer();

    // INPUT (window UI coordinates, as SLCameraMode receives them). The document has first
    // refusal: true when the event was over one of its elements (or closed one of its popups).
    static bool handleMouse(S32 x, S32 y, MASK mask, EMouseClickType click, bool down);
    static bool handleWheel(S32 x, S32 y, S32 clicks, MASK mask);
    // The pointer is over one of the document's elements.
    static bool overDocument(S32 x, S32 y);
    // What under the pointer changes the wheel's meaning: a strip cell (its value in `cell`), the
    // Grid chip, the tilt knob; World otherwise.
    static slcamin::Over overAt(S32 x, S32 y, SLCameraModel::Value* cell);
    // A text field of the document has the keyboard (a number, a preset name).
    static bool textEditing();
    // Keys and characters while a text field has the keyboard (editing keys; Esc / Enter let go).
    static bool handleKey(KEY key, MASK mask);
    static bool handleChar(llwchar uni_char);
    // Tab through the overlay's controls; Space (and Enter with a popover open) activates the
    // focused one. True when taken.
    static bool handleNavKey(KEY key, MASK mask);
    // A popover, menu or card is open (Esc closes it first).
    static bool popupOpen();
    // A popover, menu or the help card is open (the review card is not a menu): Enter is theirs.
    static bool menuOpen();
    static void closePopup();
    // The mouse and keys card (board 25).
    static void showHelp(bool show);
    static bool helpOpen();
    // The Controls strip (remembered: SLCameraModeControlsOpen); opening it shows the hints again.
    static void setControlsOpen(bool open);
    // Quick settings (board 26): the Q menu's values as one grid, at the pointer (right-click) or
    // centred (the Q key). The same cells, the same actions: wheel changes, click steps.
    static void openQuickSettings(S32 x, S32 y);
    static void toggleQMenu();
    // While the grid is open: arrows choose a cell, Up / Down change it. True when taken.
    static bool handleQKey(KEY key, MASK mask);
    // The bar out of the frame and back (remembered: SLCameraModeBarHidden; H or the handle).
    static void toggleBar();
    static bool barHidden();
    // While the bar is hidden: what the wheel just changed, beside the shutter (1.5 s).
    static void showReadout(SLCameraModel::Value value);
    // After the shot (board 29): the card for the newest shot (R), and the shots going away
    // (camera mode closing frees them: their textures are let go first).
    static void toggleReview();
    static bool reviewOpen();
    // While the card is open: Left / Right show the previous / next shot. True when taken.
    static bool handleReviewKey(KEY key, MASK mask);
    static void forgetShots();
};

#endif // SL_CAMERAVIEW_H
