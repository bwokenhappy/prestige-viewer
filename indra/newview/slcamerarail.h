/**
 * @file slcamerarail.h
 * @brief Camera mode (#1578): the Photo Studio rail -- the quick settings that matter for a
 *        shot (look and grade, sky and time of day, haze, effects, scene, camera presets),
 *        docked to the left or right edge of the screen and collapsible to a thin tab.
 *
 * Every row is a view onto an existing setting or Photo Studio action; the rail holds no
 * values of its own beyond its layout (SLCameraModeRailSide / RailOpen / RailSections). The rows
 * are data (#1572): camera mode's kit overlay (slcameraview.cpp) draws them.
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

#ifndef SL_CAMERARAIL_H
#define SL_CAMERARAIL_H

#include "llrect.h"

#include <string>
#include <vector>
#include "stdtypes.h"

namespace SLCameraRail
{
// Which edge it is docked to (false: left).
bool dockedRight();

// ---- the rail as data (#1572: the kit overlay draws it) ----
enum Kind
{
    KIND_SLIDER = 0,
    KIND_TOGGLE = 1,
    KIND_CHOICE = 2,
    KIND_BUTTONS = 3
};
struct RowData
{
    std::string label, value;         // value: the slider's number, "On" / "Off", the choice
    S32 kind = KIND_SLIDER;
    F32 fraction = 0.f;               // a slider's position, 0..1
    bool on = false, enabled = true;  // a toggle's state; greyed when not enabled
    bool changed = false;             // its setting is not at its default (resetRow puts it back)
    std::string why;                  // greyed: why (the tooltip says so instead of the row's help)
    std::vector<std::string> buttons; // KIND_BUTTONS
};
size_t sectionCount();
const std::string& sectionTitle(size_t section);
bool sectionOpen(size_t section);
// The camera section (camera views; the overlay lists the saved camera setups under it).
size_t cameraSection();
void toggleSection(size_t section);
std::vector<RowData> rows(size_t section);
// A row's value: dir +1 / -1 (slider step, the next / previous choice, a toggle flips).
void turn(size_t section, size_t row, S32 dir);
// A slider set to a position (0..1); a toggle set on / off.
void setFraction(size_t section, size_t row, F32 fraction);
void press(size_t section, size_t row, size_t button);
// The row's setting back to its default (no-op for rows without one).
void resetRow(size_t section, size_t row);
// Open, side (right by default) as remembered.
bool open();
void setOpen(bool open);
void setDockedRight(bool right);
// Leaves camera mode, then opens the Photo Studio window.
void openPhotoStudio();
} // namespace SLCameraRail

#endif // SL_CAMERARAIL_H
