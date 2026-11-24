/**
 * @file slcameramodel.h
 * @brief Camera mode's camera (#1572): state and actions, no drawing and no hit-testing
 *        (rule 11.7, one model, two views). The overlay (SLCameraView) and the input routing
 *        (SLCameraMode) call it; deleting the overlay leaves a working camera behind.
 *
 * The existing renderer and Photo Studio settings are the source of truth (exposure, focus, drive,
 * grid, aspect, output); the model keeps only what a session needs on top (the AF point and what
 * it locked on, the program shift, a running drive sequence, the last notice).
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

#ifndef SL_CAMERAMODEL_H
#define SL_CAMERAMODEL_H

#include "llrect.h"
#include "stdtypes.h"

#include <string>
#include <vector>

class LLVector3;

namespace SLCameraModel
{
// The values the strip, the quick settings and the Q menu show (one control each, rule 1.5).
enum class Value
{
    Mode,
    Shutter,
    Aperture,
    EV,
    ISO,
    WB,
    Focal,
    Focus,
    Drive,
    Tilt
};

// A value as the strip shows it: the value, its unit, the plain word under it, whether its effect
// is off (motion blur, depth of field, white balance) and whether it is the dial the wheel turns.
struct Cell
{
    std::string value, unit, name;
    bool        off    = false;
    bool        marked = false;
};
Cell cell(Value v);
// Where a value sits on its scale, 0 (low) .. 1 (high), for a dial readout; -1 when it has no
// scale (modes, drive).
F32 position(Value v);

// The wheel on a value (clicks: +1 = one step forward along its scale).
void turn(Value v, S32 clicks);
// A value turned on purpose (its strip cell): the shutter in P or Av switches to Tv, the aperture
// in P or Tv to Av (exposure kept), so only that value moves. The dials keep the mode.
void turnValue(Value v, S32 clicks);
// Motion blur (Shutter), depth of field (Aperture), white balance (WB) on / off; others: no-op.
void toggle(Value v);
// A value's named steps: exposure mode, white balance presets, prime focal lengths, focus mode,
// drive mode. Others: as turn().
void step(Value v, S32 dir);
// Back to the neutral value: EV 0 (not in M), ISO 100. Others: no-op.
void reset(Value v);
// The drive dial with modifiers: Shift = burst count / bracket shots, Ctrl = bracket step.
void turnDrive(S32 clicks, MASK mask);

// The second dial by exposure mode (Shift+wheel): exposure, or the aperture in M.
Value rearDial();

// Session state: entering camera mode starts afresh (focus, AF-L, program shift).
void begin();
// Leaving: the DoF preview let go, a drive sequence stopped.
void end();

// ---- the frame and focus (window UI coordinates, as SLCameraMode receives them) ----
// The centred crop the shot keeps.
LLRect frame();
// A click in the frame: the AF point moves there and focuses. False outside the frame.
bool focusAt(S32 x, S32 y);
// The AF point, normalised in the frame (0.5 / 0.5 = centre).
void afPoint(F32& x, F32& y);
// Autofocus at the AF point (AF-ON).
void autofocus();
// The shutter's half-press (autofocus unless MF or AF-L); held false is the release.
void halfPress(bool held);
// AF-L: lock (focusing first when nothing is held) or unlock.
void toggleFocusLock();
bool focusLocked();
// The autofocus holds a point (AF-S / AF-C found one).
bool focusHeld();
// The DoF preview is held right now.
bool dofPreviewHeld();
// The AF point follows a drag (no focusing until the drag ends: focusAt there).
bool moveAfPoint(S32 x, S32 y);
// The DoF preview, held: depth of field on while held.
void setDofPreview(bool held);
// The point the autofocus locked on, agent space (MF: the ring's distance ahead).
bool focusPoint(LLVector3& agent_pos);

// ---- the shot and the drive ----
// What the drive mode does with a press; a press while a sequence runs cancels it.
void pressShutter();
// The main loop's snapshot point: the drive's timers, bursts and brackets.
void tick();
void stopDrive();
bool driving();
// A self-timer's countdown ("3") or a sequence's progress ("2 / 5"); empty otherwise.
std::string driveStatus();

// ---- where shots go (phase 2) ----
enum Destination : U32
{
    DEST_DISK = 0,
    DEST_INVENTORY = 1, // a texture in the Snapshots folder (costs L$)
    DEST_PROFILE = 2,   // the profile feed (public)
    DEST_EMAIL = 3      // a postcard
};
U32  destination(); // SLCameraModeDestination, remembered
void setDestination(U32 dest);
// What an inventory upload of the current output size costs (L$).
S32  inventoryCost();
// A shot waiting for the overlay: confirm (inventory, profile; until "don't ask again this
// session") or the address (e-mail, every time). `cost`: the inventory upload's L$.
bool pendingShot(U32& dest, S32& cost);
// Send it: params "dont_ask", "caption", "location" (profile), "to", "subject", "message"
// (e-mail). An error to show (a bad address, no subject) leaves it pending; "" when sent.
std::string commitPending(const LLSD& params);
void discardPending();
const std::string& lastEmailTo();
// Where the last shot went (with lastSavedAt()).
U32  lastSavedTo();

// ---- framing ----
U32  grid();
void setGrid(U32 grid);
void stepGrid(S32 dir);
U32  aspect();
void setAspect(U32 aspect);
void stepAspect(S32 dir);
bool level();
void setLevel(bool on);
// The camera roll (Dutch angle), radians, within +/- tiltLimit().
F32  tilt();
void setTilt(F32 radians);
F32  tiltLimit();
// The camera's measured roll and pitch (radians), for the level; level within levelTolerance().
void levelAngles(F32& roll, F32& pitch);
F32  levelTolerance();
// The shot's size in pixels (the output size, cropped to the aspect).
void outputSize(S32& w, S32& h);
// The size before the aspect crop (SnapshotLocalWidth / Height; the window's size when unset) and
// the window's size in pixels.
void outputBase(S32& w, S32& h);
void setOutputBase(S32 w, S32 h);
// "Window": the size follows the window (the setting unset), so resizing it never leaves a stale size.
bool outputFollowsWindow();
void setOutputFollowsWindow();
void windowSize(S32& w, S32& h);
// The file: 0 PNG, 1 JPEG, 2 BMP (SnapshotFormat), the JPEG quality, post effects in the shot.
U32  fileFormat();
void setFileFormat(U32 format);
S32  jpegQuality();
void setJpegQuality(S32 quality);
bool postEffects();
void setPostEffects(bool on);

// ---- the Q menu (#1579) ----
const std::vector<std::string>& qItems();
std::string qValue(S32 item);
void        qTurn(S32 item, S32 dir);

// ---- notices ----
// A short message for the overlay ("Saved", "Focus locked").
void               toast(const std::string& text, F64 seconds = 2.0);
const std::string& toastText(); // empty when none is showing
// The shutter's flash: 0..1, fading; always 0 with the flash switched off.
F32 flash();
// The flash on a shot (SLCameraModeFlash, on by default). Off changes nothing else: the shutter
// sound and the notices stay.
bool flashOn();
void setFlashOn(bool on);
// When the last shot was saved to disk (LLTimer seconds; long ago before the first).
F64 lastSavedAt();

// ---- readouts the overlay draws (the settings, read) ----
U32         exposureMode();
F32         shutter();
F32         fnumber();
F32         iso();
// P / Av / Tv: the compensation; M: the manual meter (stops from the metered exposure).
F32         exposureStops();
F32         kelvin();
bool        whiteBalanceOn();
F32         focalLength();
U32         focusMode();
const char* focusModeLabel(U32 mode);
F32         manualFocusDistance();
F32         focusDistanceTo(const LLVector3& p);
std::string driveLabel();
U32         driveMode(); // 0 single, 1 2 s timer, 2 10 s timer, 3 burst, 4 bracketing

// <Prestige> focus and exposure (prestigecamerafocus.h): faces, group focus, focus confirmation,
// AE-L, the two-stage Enter, depth of field by default and focus peaking in MF.
// Camera mode opened, its settings recorded: the session's defaults (depth of field on unless the
// photographer turned it off in camera mode last time: PrestigeCameraDepthOfField).
void applySessionDefaults();
// Enter (the shutter key): the first press is the half-press (focus, confirmation), the next one
// shoots. Nothing to focus (MF, AF-L) or a sequence running: the press acts at once.
void shutterKey();
// A half-press from the key is waiting for its second press; Escape or moving the AF point ends it.
bool halfPressArmed();
void cancelHalfPress();
// Shift+click in the frame: the person there joins the group focus, or leaves it. False outside
// the frame. A plain click (focusAt) is one subject again.
bool groupFocusAt(S32 x, S32 y);
size_t groupSize();
// The group's people, where their eyes are in the window (UI units), for the overlay's markers.
struct GroupMarker
{
    F32 x = 0.f, y = 0.f;
};
void groupMarkers(std::vector<GroupMarker>& out);
// Focus confirmation: 0 none, 1 searching, 2 achieved (green, beeped), 3 failed (red).
U32  focusConfirm();
// AE-L: the auto exposure holds its value until pressed again or camera mode closes.
void toggleExposureLock();
bool exposureLocked();
// Focus peaking: MF, with PrestigeCameraFocusPeaking on (drawn by the depth of field).
bool focusPeaking();
// </Prestige>
} // namespace SLCameraModel

#endif // SL_CAMERAMODEL_H
