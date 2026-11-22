/**
 * @file slvideostudio.h
 * @brief Video Studio (#1668): the motion counterpart of Photo Studio -- a camera dolly / slider
 *        track built in the world, ridden by the camera, recorded to video; and the home of
 *        instant replay. New interface only: like camera mode (slcameramode.h) it hides the viewer
 *        UI and the HUD attachments and shows one kit document over the world (slvideostudioview.h),
 *        so it works whatever the rest of the interface is (classic or new).
 *
 * THE TRACK is client-local: it lives in this class and in files under the user settings folder,
 * never in a message to the region (no object is created, selected or updated). It is drawn in the
 * world-overlay segment (where LL's edit manipulators draw) with its own gizmos: move arrows and a
 * plane handle, yaw and tilt rings, length and bend handles, a target handle -- snapping to the build
 * grid (SnapEnabled / GridResolution, LL's 5.625 degree rotation step).
 *
 * Costs nothing while closed: every hook is one read of active() (the recorder's: busy()).
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

#ifndef SL_VIDEOSTUDIO_H
#define SL_VIDEOSTUDIO_H

#include "indra_constants.h"
#include "stdtypes.h"

#include <string>
#include <vector>

class LLVector3;

class SLVideoStudio
{
public:
    enum Panel
    {
        PANEL_TRACK = 0,
        PANEL_RIDE = 1,
        PANEL_RECORD = 2,
        PANEL_REPLAY = 3,
        PANEL_COUNT
    };

    static bool active() { return sActive; }
    // ENTRY POINTS (World > Video Studio, the toolbar button, camera mode's rail): opens on the panel
    // last used, or closes when open. World > Instant Replay opens it on the Instant Replay panel.
    static void toggle();
    static void open(Panel panel);
    // The RmlUi layer can show the studio (else the entry points say so; replay falls back to the toggle).
    static bool usable();
    static void start();
    static void stop();

    // ---- per frame ----
    // The VK UI pass while active: the overlay's layer slot, drags that follow the pointer, the ride.
    static void draw();
    // The main loop after the frame, while active or recording: the ride's end, the recorder.
    static void update();
    // The world-overlay segment while active: the track, its handles and gizmos (never while
    // recording: they would be in the video).
    static void renderWorld();
    // The camera rides the track (playing, recording, scrubbing or "view from the cart"): the main
    // loop calls applyCamera() instead of the agent camera's update.
    static bool drivesCamera();
    static void applyCamera();
    // Depth of field: the lock-on target while riding (agent space). False otherwise.
    static bool focusPoint(LLVector3& agent_pos);
    // The ride hides the overlay (Hide UI while riding): Esc brings it back.
    static bool overlayHidden();

    // ---- input, only while active (window UI coordinates, as SLCameraMode receives them) ----
    static bool handleMouseClick(S32 x, S32 y, MASK mask, EMouseClickType click, bool down);
    static void handleScrollWheel(S32 x, S32 y, S32 clicks);
    static void handleScrollWheelFraction(F32 notches, F32 camera_delta);
    static bool handleKey(KEY key, MASK mask);
    static bool handleHover(S32 x, S32 y, MASK mask);
    static bool blocksMenuShortcut(KEY key, MASK mask);

    // ---- the overlay's data (slvideostudioview.cpp presents it; no logic there) ----
    enum RowKind
    {
        ROW_SLIDER = 0,
        ROW_TOGGLE = 1,
        ROW_CHOICE = 2,
        ROW_BUTTONS = 3,
        ROW_NOTE = 4,
        ROW_HEAD = 5
    };
    struct Row
    {
        std::string label, value;
        S32 kind = ROW_SLIDER;
        F32 fraction = 0.f;               // a slider's position 0..1
        bool on = false, enabled = true;  // a toggle's state; greyed when not enabled
        std::vector<std::string> buttons; // ROW_BUTTONS
        bool operator==(const Row& o) const
        {
            return label == o.label && value == o.value && kind == o.kind && fraction == o.fraction && on == o.on
                   && enabled == o.enabled && buttons == o.buttons;
        }
    };
    static Panel panel();
    static void setPanel(Panel panel);
    static std::string panelTitle(Panel panel);
    static std::vector<Row> rows();
    // A row's value: dir +1 / -1 (slider step, next / previous choice, a toggle flips).
    static void turn(size_t row, S32 dir);
    static void setFraction(size_t row, F32 fraction);
    static void press(size_t row, size_t button);
    // The transport bar: play / stop, the scrub position (0..1 of one pass), time, record.
    static bool playing();
    static void togglePlay();
    static F32 scrub();
    static void setScrub(F32 fraction, bool dragging);
    static std::string timeText();
    static bool recording();
    static void toggleRecord();
    static std::string recordStatus();
    static bool hasTrack();
    // A one-line hint for the bottom of the panel (what the mouse does now).
    static std::string hint();

private:
    static bool sActive;
};

#endif // SL_VIDEOSTUDIO_H
