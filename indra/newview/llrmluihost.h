// #703 RmlUi host: process lifecycle, the main-window context and its documents.
//
// UIRenderer (restart required, read once at the first frame):
//   0 = Classic, the LLView renderer (default while the RmlUi presenters grow);
//   1 = Classic on RmlUi: the current UI, presented through RmlUi where every view of a
//       floater has a presenter (llrmluiclassic.h), the LLView renderer elsewhere;
//   2 = New: the new interface (llnewui.h), its own documents over the classic LLView UI.
// RenderVKRmlUiDemo (session only) adds the glass proof document in any mode.
// Camera mode (#1572) installs the host in any mode at its first entry (slRmlUiRequestCamera) and
// keeps it for the session; while camera mode is closed in mode 0 the host records nothing.
#pragma once

#include <cstdint>
#include <string>

#include "llcursortypes.h"

void slRmlUiTick() noexcept;
// The renderer chosen for this session (UIRenderer at startup).
uint32_t slRmlUiMode() noexcept;

// Camera mode is about to open: install the host (any UIRenderer mode, sticky for the session).
void slRmlUiRequestCamera() noexcept;
// Camera mode's overlay can be shown: Vulkan is up and RmlUi is neither poisoned nor failed to
// start the camera document. False: snapshots use the classic snapshot window.
bool slRmlUiCameraUsable() noexcept;
// The overlay cannot come back this session (poisoned, or it kept failing): camera mode gives way
// to the classic window. A device reset being recovered is not a failure.
bool slRmlUiCameraFailed() noexcept;
// Video Studio (#1668), the same way: install the host for it (sticky), whether its overlay can
// show, and whether it failed for the session (the studio then leaves).
void slRmlUiRequestStudio() noexcept;
bool slRmlUiStudioUsable() noexcept;
bool slRmlUiStudioFailed() noexcept;
// The cursor the hovered RmlUi element asks for (its RCSS `cursor`: "text", "pointer", ...; empty
// for the default), as RmlUi last reported it.
const std::string& slRmlUiCursor() noexcept;
// ...as the viewer's cursor: the I-beam for "text", the hand for "pointer", the arrow otherwise.
ECursorType slRmlUiCursorType() noexcept;
