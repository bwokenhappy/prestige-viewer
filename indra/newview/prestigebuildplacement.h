/**
 * @file prestigebuildplacement.h
 * @brief Build helpers: Place on Surface (Build menu, P) with a see-through
 *        preview of where the selection will go, and Snap to Objects for the
 *        Build tool's move arrows.
 *
 * Place on Surface: hold P to see a box where the selection will go (it follows
 * the mouse); release P to put the selection there. Tapping P places it at once.
 * Build > Place on Surface (clicked) waits for a click in the world instead; Esc
 * or a right click cancels. The selection's lowest side, as seen from the
 * surface, goes against the surface under the mouse (the ground, or any object
 * that is not part of the selection), plus PrestigePlaceOnSurfaceOffset metres.
 * With PrestigePlaceOnSurfaceAlign the selection also turns so the side chosen
 * in PrestigePlaceOnSurfaceSide faces the surface. The move goes through the
 * normal object update message, so Undo (Ctrl+Z) puts it back.
 *
 * Snap to Objects (PrestigeSnapToObjects): while a move arrow is dragged, the
 * selection stops flush against objects in its way and snaps against them when
 * it comes within PrestigeSnapToObjectsDistance. It works with bounding boxes
 * (aligned to the region axes), not with the exact shape of the objects.
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

#ifndef PRESTIGE_BUILDPLACEMENT_H
#define PRESTIGE_BUILDPLACEMENT_H

#include "stdtypes.h"

class LLUICtrl;
class LLVector3;

class PrestigePlaceOnSurface
{
public:
    // Build > Place on Surface is enabled: Build mode, an in-world selection
    // (not a HUD or an attachment) that you may move.
    static bool canPlace();

    // The menu item, from its shortcut (P) or a click. ctrl is the menu item.
    static void onPlaceCommand(LLUICtrl* ctrl);

    // Draws the preview box (world overlay pass, gGL in agent space). No-op
    // unless a placement is being previewed.
    static void renderPreview();

    // Ends a preview or a wait for a click without moving anything.
    static void cancel();

    // Used by the click-to-place tool.
    static void placeAt(S32 x, S32 y);
    static void onAimToolDeselected();
};

class PrestigeObjectSnap
{
public:
    // A move arrow was grabbed: remember the selection's box and the boxes of
    // the objects around it. Does nothing unless PrestigeSnapToObjects is on.
    static void beginDrag();
    static void endDrag();

    // distance: how far the selection has been dragged along axis (agent
    // space) from where the drag started. Returns the distance that keeps it
    // out of the objects in its way, snapped flush against them when close.
    static F64 adjust(const LLVector3& axis, F64 distance);
};

#endif // PRESTIGE_BUILDPLACEMENT_H
