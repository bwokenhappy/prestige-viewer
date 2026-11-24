/**
 * @file slcameramode.cpp
 * @brief Camera mode (#1572): the shell (#1573), the viewfinder (#1574), the exposure and lens
 *        dials (#1575, #1576) and the shutter (#1577). See slcameramode.h.
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

#include "llviewerprecompiledheaders.h"

#include "slcameramode.h"
#include "slcamerainput.h"
#include "slcameramodel.h"

#include "slcamerarestore.h"
#include "slcamerareview.h"
#include "slcameraview.h"
#include "prestigecamerashot.h" // <Prestige> setting up the shot: HUDs, poser, light, sky, presets' place
#include "slkeycommands.h"
#include "slvideostudio.h"
#include "llviewermenu.h" // gMenuBarView: the menu shortcuts camera mode swallows

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"         // gDisconnected
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "llrmluihost.h"
#include "llselectmgr.h"      // (lltoolpie.h holds an LLObjectSelection handle)
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "llui.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"      // isAgentAvatarValid
#include "pipeline.h"


namespace SCM = SLCameraModel;

namespace
{
// The system's double-click time (Windows' default; SDL keeps the real one to itself) and the
// viewer's drag threshold (UI units): a click within both of the last one is a double-click.
constexpr F64 kDoubleClickTime = 0.5;
constexpr F32 kSlop = (F32)DRAG_N_DROP_DISTANCE_THRESHOLD;
} // namespace

bool SLCameraMode::sActive = false;

namespace
{
struct State
{
    // What entry found, put back on exit.
    bool uiWasOn = true;
    bool hudWasOn = true;
    LLHandle<LLView> focus;

    bool rightPassed = false;  // a modified right press went on to the camera tools
    bool leftPassed = false;   // a modified left press went on to the camera tools (so does its release)
    slcamin::ReturnState enter; // Enter's two arrivals per press (handleKey)
    F32 wheelFraction = 0.f;   // touchpad wheel, not yet a whole click

    // A left press on the picture (a click or double-click on release), a right press (quick
    // settings on release, an orbit once it travels), the window's focus last frame.
    bool leftDown = false;
    bool afDrag = false;       // the press landed on the AF point: it follows the drag
    S32  afFocusX = INT_MIN, afFocusY = INT_MIN; // where the drag last focused
    F64  afFocusAt = 0.0;      // ...and when (a pick at most 10 times a second)
    S32 leftX = 0, leftY = 0;
    slcamin::ClickState clicks;
    bool rightDown = false, orbiting = false;
    S32 rightX = 0, rightY = 0, lastMX = 0, lastMY = 0;
    F32 rightTravel = 0.f;
    bool hadFocus = true;
};
State sState;

F64 now() { return LLTimer::getTotalSeconds(); }

// (camera state and actions: slcameramodel.cpp; the overlay: slcameraview.cpp)

// The wheel in quick shot (and Alt+wheel): the camera moves in or out, as in play.
void dollyCamera(F32 clicks)
{
    gAgentCamera.handleScrollWheel(clicks);
}

// A right drag past the threshold orbits the camera's focus point. The focus is taken off the
// avatar first, so the orbit never turns the avatar (the third-person rule would).
void followRightDrag()
{
    if (!sState.rightDown || !gViewerWindow) return;
    const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
    if (!sState.orbiting)
    {
        sState.rightTravel = (F32)llmax(llabs(mouse.mX - sState.rightX), llabs(mouse.mY - sState.rightY));
        if (sState.rightTravel <= kSlop) return;
        sState.orbiting = true;
        gAgentCamera.unlockView();
        gViewerWindow->hideCursor();
        gViewerWindow->moveCursorToCenter();
        const LLCoordGL centre = gViewerWindow->getCurrentMouse();
        sState.lastMX = centre.mX;
        sState.lastMY = centre.mY;
        return;
    }
    const S32 dx = mouse.mX - sState.lastMX, dy = mouse.mY - sState.lastMY;
    if (dx == 0 && dy == 0) return;
    const F32 radians_per_pixel = 360.f * DEG_TO_RAD / (F32)llmax(gViewerWindow->getWorldViewWidthScaled(), 1);
    if (dx) gAgentCamera.cameraOrbitAround(-(F32)dx * radians_per_pixel);
    if (dy) gAgentCamera.cameraOrbitOver(-(F32)dy * radians_per_pixel);
    gViewerWindow->moveCursorToCenter();
    const LLCoordGL centre = gViewerWindow->getCurrentMouse();
    sState.lastMX = centre.mX;
    sState.lastMY = centre.mY;
}
// The #1232 self-timer's countdown look, and the progress of a sequence.
} // namespace

// ---- public ----

void SLCameraMode::toggleSnapshot()
{
    if (sActive)
    {
        stop();
        return;
    }
    static LLCachedControl<bool> s_overlay(gSavedSettings, "SLCameraModeOverlay", true);
    // The overlay is an RmlUi document: without RmlUi (poisoned, or it failed to start) the
    // classic snapshot window is the camera.
    if (s_overlay && !LLFloaterReg::instanceVisible("snapshot") && !gDisconnected && isAgentAvatarValid()
        && slRmlUiCameraUsable())
    {
        start();
        return;
    }
    LLFloaterReg::toggleInstanceOrBringToFront("snapshot");
}

bool SLCameraMode::snapshotOpen()
{
    return sActive || LLFloaterReg::instanceVisible("snapshot");
}

void SLCameraMode::start()
{
    if (sActive || !gViewerWindow) return;
    if (SLVideoStudio::active()) SLVideoStudio::stop();   // one overlay at a time (#1668)
    LL_INFOS("CameraMode") << "camera mode on" << LL_ENDL;
    slRmlUiRequestCamera(); // the RmlUi layer, in any UIRenderer mode, from the next frame
    sState.uiWasOn = gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    sState.hudWasOn = LLPipeline::sShowHUDAttachments;
    sState.focus = LLHandle<LLView>();
    if (LLUICtrl* focused = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus()))
    {
        sState.focus = focused->getHandle();
    }
    gFocusMgr.setKeyboardFocus(nullptr);
    if (dynamic_cast<LLView*>(gFocusMgr.getMouseCapture()))
    {
        gFocusMgr.setMouseCapture(nullptr);   // a UI drag in progress ends here
    }
    if (sState.uiWasOn)
    {
        LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    }
    LLPipeline::sShowHUDAttachments = false;
    sState.leftPassed = sState.rightPassed = false;
    sState.wheelFraction = 0.f;
    SCM::begin();
    sState.leftDown = sState.rightDown = sState.orbiting = sState.afDrag = false;
    sState.clicks = slcamin::ClickState();
    sState.hadFocus = true;
    // Before anything changes: every setting camera mode can touch, to be put back on exit.
    SLCameraRestore::capture();
    sActive = true;
    // <Prestige> the HUD switch (PrestigeCameraShowHUDs; hudWasOn above is still what exit puts
    // back), the personal environment and Freeze World as found
    PrestigeCameraShot::begin();
    // </Prestige>
}

void SLCameraMode::stop()
{
    if (!sActive) return;
    LL_INFOS("CameraMode") << "camera mode off" << LL_ENDL;
    sActive = false;
    if (sState.rightPassed)
    {
        LLToolPie::getInstance()->cancelRightPress();   // no context menu on the way out
        sState.rightPassed = false;
    }
    if (sState.orbiting && gViewerWindow)
    {
        gViewerWindow->showCursor();   // an orbit in progress gives the cursor back
    }
    sState.leftDown = sState.rightDown = sState.orbiting = false;
    SCM::end();
    SLCameraView::forgetShots(); // RmlUi lets go of their textures first
    SLCameraReview::clear();   // the session's shots are freed (the files stay)
    // <Prestige> the poser / Personal Lighting camera mode opened close; "keep" keeps the sky
    PrestigeCameraShot::end();
    // </Prestige>
    // Every setting, the roll and the sky exactly as they were on entry (the dials, the rail,
    // presets, the drive and the DoF preview changed them only for the session).
    SLCameraRestore::restore();
    // <Prestige> sky, water and day cycle back ("for this photo only"), Freeze World as it was
    PrestigeCameraShot::afterRestore();
    // </Prestige>
    if (sState.uiWasOn && !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    }
    LLPipeline::sShowHUDAttachments = sState.hudWasOn;
    if (LLView* v = sState.focus.get())
    {
        if (LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(v); ctrl && ctrl->isInVisibleChain() && ctrl->isInEnabledChain())
        {
            ctrl->setFocus(true);
        }
    }
    sState.focus = LLHandle<LLView>();
}

void SLCameraMode::openClassic()
{
    stop();
    LLFloaterReg::showInstance("snapshot");
}

void SLCameraMode::draw()
{
    if (!sActive) return;
    if (gDisconnected || !gViewerWindow)
    {
        stop();
        return;
    }
    if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        // Ctrl+Alt+F1 or the restore binding brought the interface back: leave, and do not
        // hide it again on the way out.
        sState.uiWasOn = false;
        stop();
        return;
    }
    if (slRmlUiCameraFailed())
    {
        // RmlUi went away for good (poisoned, or it kept failing): the classic snapshot window.
        LL_WARNS("CameraMode") << "camera overlay unavailable; opening the classic snapshot window" << LL_ENDL;
        openClassic();
        return;
    }
    // The overlay is the kit document (slcameraview.cpp): its place in the UI layer, after the
    // snapshot capture points, so never in a shot.
    SLCameraView::markLayer();
    followRightDrag();
    if (sState.afDrag && sState.leftDown)
    {
        // The AF point follows the drag and focuses as it goes (a pick when the pointer moved,
        // once past the slop, at most 10 times a second: picks test rigged meshes, costly in a
        // crowd), so the depth of field follows live; the release focuses exactly where it lands.
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        SCM::moveAfPoint(mouse.mX, mouse.mY);
        const bool past = llmax(llabs(mouse.mX - sState.leftX), llabs(mouse.mY - sState.leftY)) > (S32)kSlop;
        if (past && (mouse.mX != sState.afFocusX || mouse.mY != sState.afFocusY) && now() - sState.afFocusAt >= 0.1)
        {
            sState.afFocusAt = now();
            sState.afFocusX = mouse.mX;
            sState.afFocusY = mouse.mY;
            SCM::focusAt(mouse.mX, mouse.mY);
        }
    }
    sState.hadFocus = gFocusMgr.getAppHasFocus();
    // <Prestige> the poser / Personal Lighting over the shot, above the overlay (after its layer
    // mark; the UI layer is after the snapshot capture points: never in a shot)
    PrestigeCameraShot::drawFloaters();
    // </Prestige>
}

bool SLCameraMode::handleMouseClick(S32 x, S32 y, MASK mask, EMouseClickType click_type, bool down)
{
    if (!sActive) return false;
    // The press that brought the window to the front goes nowhere (not to the overlay's shutter
    // or Leave, not to the picture), and neither does its release.
    if (click_type == CLICK_LEFT || click_type == CLICK_DOUBLELEFT)
    {
        if (down ? !slcamin::beginPress(sState.clicks, !sState.hadFocus || !gFocusMgr.getAppHasFocus())
                 : !slcamin::endPress(sState.clicks))
        {
            return true;
        }
    }
    // <Prestige> the poser / Personal Lighting drawn over the overlay get their clicks first
    if (PrestigeCameraShot::handleFloaterClick(x, y, mask, click_type, down)) return true;
    // </Prestige>
    // The kit overlay has first refusal over its own elements (#1572).
    if (SLCameraView::handleMouse(x, y, mask, click_type, down)) return true;
    // <Prestige> a click on a shown HUD is a touch (the current tool's); an armed "follow an
    // object" takes the click that picks it
    switch (PrestigeCameraShot::worldClick(x, y, mask, click_type, down))
    {
    case PrestigeCameraShot::Click::Taken:   return true;
    case PrestigeCameraShot::Click::ToTools: return false;
    case PrestigeCameraShot::Click::NotMine: break;
    }
    // </Prestige>
    const bool modified = (mask & (MASK_ALT | MASK_CONTROL | MASK_SHIFT)) != 0;
    if (click_type == CLICK_RIGHT)
    {
        // Right: a click opens quick settings at the pointer; a drag past the threshold orbits the
        // camera's focus (draw() follows it) and never turns the avatar, whatever
        // RightClickDragOrbit says. Modified presses go on to the camera tools, as before.
        if (down)
        {
            sState.rightPassed = modified;
            if (modified) return false;
            sState.rightDown = true;
            sState.orbiting = false;
            sState.rightX = sState.lastMX = x;
            sState.rightY = sState.lastMY = y;
            sState.rightTravel = 0.f;
            return true;
        }
        if (sState.rightPassed)
        {
            sState.rightPassed = false;
            return false;
        }
        if (!sState.rightDown) return true;
        sState.rightDown = false;
        if (sState.orbiting)
        {
            sState.orbiting = false;
            const LLCoordGL press_point(sState.rightX, sState.rightY);
            gViewerWindow->releaseCursorAfterDrag(&press_point);
        }
        else if (slcamin::onRightUp(sState.rightTravel, kSlop) == slcamin::Right::QuickSettings)
        {
            SLCameraView::openQuickSettings(x, y);
        }
        return true;
    }
    if (click_type != CLICK_LEFT && click_type != CLICK_DOUBLELEFT) return false;   // middle: voice, as everywhere

    if (down)
    {
        sState.leftPassed = modified;
        if (modified) return false;   // Alt-orbit, Ctrl+Alt-pan, ...: the camera tools
        sState.leftDown = true;
        sState.leftX = x;
        sState.leftY = y;
        // On the AF point (its 40 dp box, a little margin): it can be dragged to a new place.
        {
            const LLRect fr = SCM::frame();
            F32 afx = 0.5f, afy = 0.5f;
            SCM::afPoint(afx, afy);
            const F32 cx = (F32)fr.mLeft + afx * (F32)fr.getWidth(), cy = (F32)fr.mBottom + afy * (F32)fr.getHeight();
            const F32 reach = 26.f * SLCameraView::dpScale();
            sState.afDrag = fabsf((F32)x - cx) <= reach && fabsf((F32)y - cy) <= reach;
            sState.afFocusX = sState.afFocusY = INT_MIN;
        }
        return true;
    }
    if (sState.leftPassed)
    {
        sState.leftPassed = false;
        return false;
    }
    if (!sState.leftDown) return true;
    sState.leftDown = false;
    if (sState.afDrag)
    {
        // Dragged (or clicked) on the AF point: it focuses where it was let go.
        sState.afDrag = false;
        const F32 moved = (F32)llmax(llabs(x - sState.leftX), llabs(y - sState.leftY));
        if (moved > kSlop)
        {
            SCM::focusAt(x, y);
            sState.clicks = slcamin::ClickState(); // a drag is not half of a double-click
            return true;
        }
    }
    const F32 travel = (F32)llmax(llabs(x - sState.leftX), llabs(y - sState.leftY));
    switch (slcamin::onLeftUp(sState.clicks, now(), (F32)x, (F32)y, travel, kDoubleClickTime, kSlop, false))
    {
    case slcamin::Left::Focus:
        SCM::focusAt(x, y);
        break;
    case slcamin::Left::FocusAndShoot:
        SCM::focusAt(x, y);
        SCM::pressShutter();
        break;
    default:
        break;
    }
    return true;
}

void SLCameraMode::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (!sActive || clicks == 0) return;
    // <Prestige> over the poser / Personal Lighting: their lists and sliders
    if (PrestigeCameraShot::handleFloaterWheel(x, y, clicks)) return;
    // </Prestige>
    const MASK mask = gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
    SCM::Value cell = SCM::Value::Mode;
    const slcamin::Over over = SLCameraView::overAt(x, y, &cell);
    // The overlay's other elements (a popover, the quick settings, the rail, a list) take it.
    if (over == slcamin::Over::World && SLCameraView::handleWheel(x, y, clicks, mask)) return;
    const bool shift = (mask & MASK_SHIFT) != 0, ctrl = (mask & MASK_CONTROL) != 0, alt = (mask & MASK_ALT) != 0;
    // The drive cell keeps its modifiers: Shift = burst count / bracket shots, Ctrl = bracket step.
    if (over == slcamin::Over::Cell && cell == SCM::Value::Drive && !alt)
    {
        SCM::turnDrive(-clicks, mask);
        return;
    }
    const bool controls = gSavedSettings.getBOOL("SLCameraModeControlsOpen");
    // Wheel up moves forward along each scale (higher f-number, faster shutter, longer lens).
    const S32 steps = -clicks;
    auto dial = [steps](SCM::Value v)
    {
        SCM::turn(v, steps);
        SLCameraView::showReadout(v);   // the bar out of the frame: say what changed
    };
    switch (slcamin::wheelTarget(controls, over, shift, ctrl, alt))
    {
    case slcamin::Wheel::Dolly:
    case slcamin::Wheel::Camera:   dollyCamera((F32)clicks); break;
    case slcamin::Wheel::RearDial:  dial(SCM::rearDial()); break;
    case slcamin::Wheel::Zoom:      dial(SCM::Value::Focal); break;
    case slcamin::Wheel::Cell:
        SCM::turnValue(cell, steps);   // the value under the pointer, on purpose
        SLCameraView::showReadout(cell);
        break;
    case slcamin::Wheel::GridNext:  SCM::stepGrid(steps); break;
    case slcamin::Wheel::TiltFine:  dial(SCM::Value::Tilt); break;
    }
}

void SLCameraMode::handleScrollWheelFraction(F32 notches, F32 camera_delta)
{
    if (!sActive) return;
    const MASK mask = gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
    if (mask & MASK_ALT)
    {
        gAgentCamera.handleScrollWheel(camera_delta);
        return;
    }
    sState.wheelFraction += notches;
    const S32 whole = (S32)sState.wheelFraction;   // toward zero
    if (whole != 0)
    {
        sState.wheelFraction -= (F32)whole;
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        handleScrollWheel(mouse.mX, mouse.mY, whole);
    }
}

bool SLCameraMode::handleKey(KEY key, MASK mask)
{
    if (!sActive) return false;
    // <Prestige> a field of the poser / Personal Lighting being typed in keeps its keys; Esc lets go of it
    if (PrestigeCameraShot::floaterTyping())
    {
        if (key != KEY_ESCAPE) return false;
        PrestigeCameraShot::releaseFloaterKeys();
        return true;
    }
    // </Prestige>
    if (key != KEY_RETURN)
        return handleKeyOnce(key, mask);
    // Enter: its key and then its character are one press (a field let go of, or the send card
    // sent, never also shoots); a held Enter's repeats do nothing (they never confirm a card).
    if (!slcamin::takeReturn(sState.enter, now(), gKeyboard && gKeyboard->getKeyRepeated(key)))
        return true;
    const bool handled = handleKeyOnce(key, mask);
    if (handled)
        slcamin::returnHandled(sState.enter, now());
    return handled;
}

bool SLCameraMode::handleKeyOnce(KEY key, MASK mask)
{
    // A field of the kit overlay being typed in keeps its keys; the quick settings and the
    // review card take their arrows (#1572).
    if (SLCameraView::handleKey(key, mask) || SLCameraView::handleQKey(key, mask) || SLCameraView::handleReviewKey(key, mask)
        || SLCameraView::handleNavKey(key, mask))
    {
        return true;
    }
    SLKeyCommands& keys = SLKeyCommands::instance();
    const bool repeat = gKeyboard && gKeyboard->getKeyRepeated(key);
    if (keys.matches("camera_exit", key, mask))
    {
        // The innermost thing first: an open popover, card or menu, then camera mode.
        if (SLCameraView::popupOpen())
            SLCameraView::closePopup();
        else
            stop();
        return true;
    }
    // Toggles of the overlay's own surfaces: they work with another one open (they replace it).
    if (keys.matches("camera_q_menu", key, mask))
    {
        if (!repeat) SLCameraView::toggleQMenu();
        return true;
    }
    if (keys.matches("camera_review", key, mask))
    {
        if (!repeat) SLCameraView::toggleReview();
        return true;
    }
    if (keys.matches("camera_help", key, mask))
    {
        if (!repeat) SLCameraView::showHelp(!SLCameraView::helpOpen());
        return true;
    }
    // Typing in a field keeps every key; an open popover or menu takes only Enter. A key bound to
    // a camera command is the camera's either way: it never falls through to gestures.
    const bool typing = SLCameraView::textEditing(), menu = SLCameraView::menuOpen();
    if (keys.matches("camera_toggle_bar", key, mask))
    {
        if (!repeat && slcamin::commandTakesKeyFor(slcamin::Command::Letter, typing, menu)) SLCameraView::toggleBar();
        return true;
    }
    if (keys.matches("camera_shoot", key, mask))
    {
        if (!slcamin::commandTakesKeyFor(slcamin::Command::Shoot, typing, menu)) return true;
        if (!repeat)
        {
            SCM::halfPress(true);   // a full press passes the half-press on its way
            SCM::pressShutter();
        }
        return true;
    }
    const bool letters = slcamin::commandTakesKeyFor(slcamin::Command::Letter, typing, menu);
    if (keys.matches("camera_af_on", key, mask))
    {
        if (!repeat && letters) SCM::autofocus();
        return true;
    }
    if (keys.matches("camera_af_lock", key, mask))
    {
        if (!repeat && letters) SCM::toggleFocusLock();
        return true;
    }
    if (keys.matches("camera_dof_preview", key, mask))
    {
        if (letters) SCM::setDofPreview(true);
        return true;
    }
    return false;
}

bool SLCameraMode::blocksMenuShortcut(KEY key, MASK mask)
{
    return sActive && shortcutReachesHiddenUI(key, mask);
}

bool SLCameraMode::shortcutReachesHiddenUI(KEY key, MASK mask)
{
    if (!gMenuBarView)
        return false;
    // The menu items (menu_viewer.xml names) whose shortcuts keep working: fullscreen and quit,
    // the snapshot key (it leaves camera mode), the sky and the camera's zoom, freezing the
    // world, and what the avatar does (sit, fly, run, stop or resync animations).
    static const char* const kAllowed[] = { "Fullscreen",    "Quit",          "Take Snapshot",         "Sunrise",
                                            "Noon",          "Sunset",        "Midnight",              "Use Shared Environment",
                                            "Zoom In",       "Zoom Out",      "Zoom Default",          "Freeze World",
                                            "Fly land",      "Sit stand",     "Stop Animating My Avatar",
                                            "Always Run",    "Resync Animations", "Hide Particles",
                                            "Save Replay",   "Video Studio" };
    if (!gMenuBarView->hasAccelerator(key, mask))
        return false;
    SLKeyCommands& keys = SLKeyCommands::instance();
    for (const char* name : kAllowed)
        if (keys.matchesMenuItem("viewer", name, key, mask))
            return false;
    return true;
}

bool SLCameraMode::handleHover(S32 x, S32 y, MASK mask)
{
    // <Prestige> over the poser / Personal Lighting (or one of them holds the mouse: a slider's drag)
    if (sActive && PrestigeCameraShot::handleFloaterHover(x, y, mask)) return true;
    // </Prestige>
    if (!sActive || !SLCameraView::overDocument(x, y))
        return false;
    gViewerWindow->setCursor(slRmlUiCursorType()); // the cursor the overlay's RCSS asks for
    return true;
}

bool SLCameraMode::handleUnicodeChar(llwchar uni_char)
{
    return sActive && SLCameraView::handleChar(uni_char);
}

bool SLCameraMode::handleKeyUp(KEY key, MASK mask)
{
    if (!sActive) return false;
    // A held key's release: the binding's key, whatever the modifiers are by now.
    if (SLKeyCommands::instance().getBinding("camera_dof_preview").mKey == key)
    {
        SCM::setDofPreview(false);
        return true;
    }
    return false;
}

void SLCameraMode::update()
{
    SCM::tick();
}

bool SLCameraMode::focusPoint(LLVector3& agent_pos)
{
    return sActive && SCM::focusPoint(agent_pos);
}
