/**
 * @file slvideostudioview.h
 * @brief Video Studio's overlay (#1668) on the new interface kit: one RmlUi document, a view onto
 *        SLVideoStudio (no studio logic here). The RmlUi host starts it while the studio is up, in
 *        any UIRenderer mode, and draws it while the rest of the interface is hidden -- as camera
 *        mode's overlay (slcameraview.h). It is part of the UI layer: never in a recording (the
 *        recorder takes the world before the interface is composed).
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

#ifndef SL_VIDEOSTUDIOVIEW_H
#define SL_VIDEOSTUDIOVIEW_H

#include "indra_constants.h"
#include "stdtypes.h"

namespace Rml
{
class Context;
}

class SLVideoStudioView
{
public:
    // Shows the document (created in `context` at the first call). False if it cannot load.
    static bool start(Rml::Context* context);
    // Hides it (it stays resident).
    static void stop();
    // The host's context goes away: forget the document (no RmlUi call).
    static void release();
    static bool active();
    // dp per UI unit (the overlay size setting camera mode uses too).
    static float dpScale();
    // Each frame from the RmlUi host while shown: refresh from the studio. `density` is framebuffer
    // pixels per UI unit. True when the document changed.
    static bool sync(float density);
    // In the studio's UI pass: the RmlUi layer's z slot (once per frame).
    static void markLayer();

    // INPUT (window UI coordinates). The document has first refusal: true when over its elements.
    static bool handleMouse(S32 x, S32 y, MASK mask, EMouseClickType click, bool down);
    static bool handleWheel(S32 x, S32 y, S32 clicks, MASK mask);
    static bool overDocument(S32 x, S32 y);
};

#endif // SL_VIDEOSTUDIOVIEW_H
