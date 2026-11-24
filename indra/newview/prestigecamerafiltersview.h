/**
 * @file prestigecamerafiltersview.h
 * @brief Camera mode's Filters panel (prestigecamerafilters.h): the filter stack, its strength
 *        and effect sliders, the picker, "Import LUT...", and the before / after split line
 *        across the viewfinder. Its own RmlUi document in the camera overlay's context, behind
 *        the camera document (whose popovers stay on top), shown and fed by SLCameraView.
 *
 * Like the camera overlay it is part of the UI layer, so neither the panel nor the split
 * line is ever in a shot (the split's two pictures are the effects runtime's, live frames
 * only: LLFxRuntimeVK::setCompareSplit).
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

#ifndef PRESTIGE_CAMERAFILTERSVIEW_H
#define PRESTIGE_CAMERAFILTERSVIEW_H

#include "indra_constants.h"
#include "stdtypes.h"

namespace Rml
{
class Context;
}

namespace PrestigeCameraFiltersView
{
// The camera overlay started in `context` (its document is up): ours too, behind it.
void start(Rml::Context* context);
void stop();
// The context goes away: forget the document (no RmlUi call).
void release();
// Each frame while the camera overlay shows. True when the document changed.
bool sync(float density);

// Input, in window UI coordinates, before the camera document's own handling: true when the
// event was ours (over the panel, the picker or the split line, or a drag of ours).
bool handleMouse(S32 x, S32 y, MASK mask, EMouseClickType click, bool down);
bool handleWheel(S32 x, S32 y, S32 clicks, MASK mask);
bool over(S32 x, S32 y);

// The panel (remembered: PrestigeCameraFiltersOpen); the camera's Filters chip toggles it.
bool panelOpen();
void togglePanel();
// The picker is open (Esc closes it first). closePopup: true when it closed it.
bool popupOpen();
bool closePopup();
} // namespace PrestigeCameraFiltersView

#endif
