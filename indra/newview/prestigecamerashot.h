/**
 * @file prestigecamerashot.h
 * @brief Setting up the shot from inside camera mode (slcameramode.h): HUDs that answer clicks
 *        in the viewfinder, the poser and Personal Lighting over the shot, the sky and water
 *        "for this photo only", Freeze World, the quiet shutter, and where the camera stands
 *        (held by your avatar, at a fixed place, or following an object) for camera presets.
 *
 * HUDs (PrestigeCameraShowHUDs, off by default): camera mode hides the HUD attachments on entry
 * and puts the switch back on exit (slcameramode.cpp keeps that bookkeeping). With this on they
 * show in the viewfinder and a left click that lands on one goes to it (a normal touch through
 * the current tool); clicks elsewhere stay the camera's. They are never in the photo: camera mode
 * takes its shots at the capture point before the HUD pass (rawSnapshot with show_hud false).
 *
 * The poser ("fs_poser") and Personal Lighting ("env_adjust_snapshot") open over the shot while
 * the interface stays hidden: the floaters camera mode opened are drawn by themselves (above the
 * overlay, in the UI layer, so after the snapshot capture points) and get the clicks, hovers,
 * wheel and typing that land on them. They stand aside (not drawn, no input) for half a second
 * whenever a shot is taken, and the ones camera mode opened close when it closes.
 *
 * The sky (PrestigeCameraEnvForPhotoOnly, on by default): changes to the personal environment
 * (sky, water, day cycle: Personal Lighting, the rail's sky rows, a preset's light) made in camera
 * mode are put back when it closes; off, they are kept (and so is a sky the rail froze).
 *
 * Freeze World is the existing toggle (SlipstreamWorldFrozen, View > Freeze World): it is put
 * back as it was when camera mode closes. The quiet shutter is the existing PlayModeUISndSnapshot
 * (Preferences > Sound & Media > UI Sounds > Shutter), inverted: quiet = no shutter sound for
 * people nearby and no camera animation (playSnapshotAnimAndSound).
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

#ifndef PRESTIGE_CAMERASHOT_H
#define PRESTIGE_CAMERASHOT_H

#include "indra_constants.h"
#include "stdtypes.h"

#include <string>

class LLSD;

namespace PrestigeCameraShot
{
// ---- camera mode's life (slcameramode.cpp) ----
// After entry (HUDs hidden, settings captured): the HUD switch, the environment and freeze state.
void begin();
// Leaving, before SLCameraRestore::restore(): the floaters camera mode opened close; "keep" keeps
// a sky the rail froze.
void end();
// Leaving, after SLCameraRestore::restore(): the environment back ("for this photo only") and
// Freeze World as it was.
void afterRestore();
// A shot is being taken (slcameramodel.cpp): the floaters stand aside.
void onShot();

// ---- HUDs ----
bool showHUDs();
void setShowHUDs(bool show);

// ---- the poser and Personal Lighting over the shot ----
enum class Tool
{
    Poser,
    Light
};
void toggleTool(Tool tool);
bool toolOpen(Tool tool);
// Camera mode's UI pass, after the overlay's layer mark: the floaters, above the overlay.
void drawFloaters();
// INPUT for the floaters (window UI coordinates, as SLCameraMode receives them). True when taken.
bool handleFloaterClick(S32 x, S32 y, MASK mask, EMouseClickType click, bool down);
bool handleFloaterHover(S32 x, S32 y, MASK mask);
bool handleFloaterWheel(S32 x, S32 y, S32 clicks);
// The keyboard focus is in one of the floaters (LLViewerWindow::updateKeyboardFocus keeps it).
bool keepsKeyboardFocus();
// A text field of one of the floaters has the keyboard: its keys are its own.
bool floaterTyping();
// Esc in such a field: the field lets go.
void releaseFloaterKeys();

// ---- after the overlay: the world under the pointer ----
enum class Click
{
    NotMine, // the camera's (focus, shoot)
    Taken,   // used here (an object picked to follow)
    ToTools  // goes on to the current tool (a HUD's touch)
};
Click worldClick(S32 x, S32 y, MASK mask, EMouseClickType click, bool down);

// ---- the world, the shutter, the sky ----
bool worldFrozen();
void setWorldFrozen(bool frozen);
bool quiet();
void setQuiet(bool quiet);
bool envForPhotoOnly();
void setEnvForPhotoOnly(bool photo_only);

// ---- where the camera stands ----
enum Anchor : S32
{
    ANCHOR_AVATAR = 0, // held by your avatar: kept relative to it, follows it
    ANCHOR_PLACE  = 1, // at a fixed place (region + position)
    ANCHOR_OBJECT = 2, // follows an object (picked with a click), keeping its offset from it
    ANCHOR_COUNT  = 3
};
S32  anchor();
// Avatar / place: the camera stays where it is and from now on follows the avatar / stays put.
// Object: the next click in the frame picks the object to follow.
void setAnchor(S32 anchor);
bool pickArmed();
// "Follows Box", "Click the object ...": a line under the choice; empty when none.
std::string anchorNote();

// ---- camera presets (slcamerapresets.cpp) ----
// What a preset adds to the format: where the camera stands (anchor and offsets), the light
// (personal environment: sky, water or day cycle) and the filters' preset.
void savePresetExtras(LLSD& sd);
// Puts the light and the filters back, and places the camera when the preset says where
// (avatar or object anchor). False when the caller places it (fixed place, or older presets).
bool applyPresetExtras(const LLSD& sd);
} // namespace PrestigeCameraShot

#endif // PRESTIGE_CAMERASHOT_H
