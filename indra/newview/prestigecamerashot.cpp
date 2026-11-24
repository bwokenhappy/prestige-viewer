/**
 * @file prestigecamerashot.cpp
 * @brief Setting up the shot from inside camera mode. See prestigecamerashot.h.
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

#include "llviewerprecompiledheaders.h"

#include "prestigecamerashot.h"

#include "slcameramode.h"
#include "slcameramodel.h"
#include "slcamerarestore.h"
#include "slfxpresets.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llenvironment.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llpopupview.h"
#include "llrender.h"
#include "llsettingsvo.h"
#include "lltrans.h"
#include "llui.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "pipeline.h"
#include "rlvactions.h"

#include <algorithm>
#include <vector>

namespace
{
// How long the floaters stand aside when a shot is taken (s).
constexpr F64 kAsideTime = 0.5;

// A floater shown over the shot. `opened`: camera mode opened it (it closes with camera mode).
// `hidden`: put away from the overlay (hidden, not closed: closing the poser can end the pose).
struct Shown
{
    LLHandle<LLFloater> handle;
    bool                opened = false;
    bool                hidden = false;
};

struct State
{
    std::vector<Shown> floaters; // back to front
    F64  asideUntil = 0.0;
    bool pressInFloater = false; // a press went to a floater: so does its release
    bool hudPress = false;       // a press went on to a HUD: so does its release
    bool pickPress = false;      // a press picked the object to follow: its release goes nowhere
    bool pickArmed = false;      // the next click in the frame picks the object to follow
    // The personal environment as camera mode found it.
    bool envCaptured = false, envTouched = false, applyingEnv = false;
    bool hadLocal = false, cloudsPaused = false;
    LLSettingsDay::ptr_t   day;
    S32                    dayLength = 0, dayOffset = 0;
    LLSettingsSky::ptr_t   sky;
    LLSettingsWater::ptr_t water;
    LLEnvironment::EnvSelection_t selected = LLEnvironment::ENV_LOCAL;
    boost::signals2::connection envChanged;
    bool frozenAtEntry = false;
};
State sState;

F64 now() { return LLTimer::getTotalSeconds(); }

std::string str(const char* name)
{
    return LLTrans::getString(std::string("CameraMode") + name);
}

const char* toolName(PrestigeCameraShot::Tool tool)
{
    return tool == PrestigeCameraShot::Tool::Poser ? "fs_poser" : "env_adjust_snapshot";
}

bool standingAside()
{
    return now() < sState.asideUntil;
}

// Forget floaters that were closed (their close button) or went away.
void prune()
{
    auto& list = sState.floaters;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [](const Shown& s)
                              {
                                  LLFloater* f = s.handle.get();
                                  return !f || (!s.hidden && !f->getVisible());
                              }),
               list.end());
}

Shown* entryFor(const LLFloater* floater)
{
    for (Shown& s : sState.floaters)
        if (floater && s.handle.get() == floater) return &s;
    return nullptr;
}

// The view is (in) one of the floaters shown over the shot.
bool owned(const LLView* view)
{
    if (!view) return false;
    for (const Shown& s : sState.floaters)
    {
        const LLFloater* f = s.handle.get();
        if (f && (view == f || view->hasAncestor(f))) return true;
    }
    return false;
}

// The frontmost shown floater under the point (window UI coordinates).
LLFloater* floaterAt(S32 x, S32 y)
{
    for (auto it = sState.floaters.rbegin(); it != sState.floaters.rend(); ++it)
    {
        LLFloater* f = it->handle.get();
        if (f && f->getVisible() && f->calcScreenRect().pointInRect(x, y)) return f;
    }
    return nullptr;
}

// The viewer's popup holder (open combo lists and the like are LLUI popups).
LLPopupView* popupView()
{
    static LLHandle<LLUICtrl> s_handle;
    if (LLUICtrl* known = s_handle.get()) return dynamic_cast<LLPopupView*>(known);
    LLView* root = LLUI::getInstance()->getRootView();
    LLPopupView* found = root ? root->findChild<LLPopupView>("popup_holder") : nullptr;
    if (found) s_handle = found->getHandle();
    return found;
}

// The floaters' open lists (a combo box's: an LLUI popup, or the top control), most recent first.
std::vector<LLView*> openLists()
{
    std::vector<LLView*> out;
    if (LLPopupView* holder = popupView())
    {
        for (const LLHandle<LLView>& h : holder->getCurrentPopups())
        {
            if (LLView* p = h.get(); p && p->getVisible() && owned(p)) out.push_back(p);
        }
    }
    if (LLUICtrl* top = gFocusMgr.getTopCtrl(); top && top->getVisible() && owned(top)) out.push_back(top);
    return out;
}

// The open list under the point, if any.
LLView* listAt(S32 x, S32 y)
{
    for (LLView* p : openLists())
    {
        S32 lx = 0, ly = 0;
        p->screenPointToLocal(x, y, &lx, &ly);
        if (p->pointInView(lx, ly)) return p;
    }
    return nullptr;
}

void closeLists()
{
    for (LLView* p : openLists())
    {
        if (p == gFocusMgr.getTopCtrl())
            gFocusMgr.setTopCtrl(nullptr);
        else
            LLUI::getInstance()->removePopup(p);
    }
}

// The floater goes to the front of the ones over the shot.
void raise(LLFloater* floater)
{
    auto& list = sState.floaters;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (list[i].handle.get() == floater)
        {
            const Shown s = list[i];
            list.erase(list.begin() + (ptrdiff_t)i);
            list.push_back(s);
            return;
        }
    }
}

bool active()
{
    return SLCameraMode::active() && !sState.floaters.empty() && !standingAside();
}

// ---- the personal environment ----

void captureEnvironment()
{
    LLEnvironment& env = LLEnvironment::instance();
    sState.envCaptured = true;
    sState.envTouched = false;
    sState.hadLocal = env.hasEnvironment(LLEnvironment::ENV_LOCAL);
    sState.day.reset();
    sState.sky.reset();
    sState.water.reset();
    if (sState.hadLocal)
    {
        // Clones: Personal Lighting edits the sky in place.
        if (LLSettingsDay::ptr_t day = env.getEnvironmentDay(LLEnvironment::ENV_LOCAL))
        {
            sState.day = day->buildClone();
            sState.dayLength = env.getEnvironmentDayLength(LLEnvironment::ENV_LOCAL).value();
            sState.dayOffset = env.getEnvironmentDayOffset(LLEnvironment::ENV_LOCAL).value();
        }
        else
        {
            if (LLSettingsSky::ptr_t sky = env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL)) sState.sky = sky->buildClone();
            if (LLSettingsWater::ptr_t water = env.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL)) sState.water = water->buildClone();
        }
    }
    sState.selected = env.getSelectedEnvironment();
    sState.cloudsPaused = env.isCloudScrollPaused();
    // Any change to the personal environment while camera mode is up (Personal Lighting, the
    // rail's sky rows, a preset's light, Photo Studio's frozen day).
    sState.envChanged.disconnect();
    sState.envChanged = env.setEnvironmentChanged(
        [](LLEnvironment::EnvSelection_t which, S32)
        {
            if (which == LLEnvironment::ENV_LOCAL && !sState.applyingEnv) sState.envTouched = true;
        });
}

void restoreEnvironment()
{
    LLEnvironment& env = LLEnvironment::instance();
    sState.applyingEnv = true;
    env.clearEnvironment(LLEnvironment::ENV_LOCAL);
    if (sState.hadLocal)
    {
        if (sState.day)
        {
            env.setEnvironment(LLEnvironment::ENV_LOCAL, sState.day, LLSettingsDay::Seconds(sState.dayLength),
                               LLSettingsDay::Seconds(sState.dayOffset));
        }
        else if (sState.sky || sState.water)
        {
            env.setEnvironment(LLEnvironment::ENV_LOCAL, sState.sky, sState.water);
        }
    }
    env.setSelectedEnvironment(sState.selected, LLEnvironment::TRANSITION_INSTANT);
    env.updateEnvironment(LLEnvironment::TRANSITION_INSTANT, true);
    sState.applyingEnv = false;
}

// The personal environment as a preset keeps it ("light"): none (the shared environment), a day
// cycle, or a fixed sky and water (Personal Lighting, a frozen day).
LLSD lightToLLSD()
{
    LLEnvironment& env = LLEnvironment::instance();
    LLSD light = LLSD::emptyMap();
    const bool local = env.hasEnvironment(LLEnvironment::ENV_LOCAL);
    light["local"] = local;
    if (!local) return light;
    if (LLSettingsDay::ptr_t day = env.getEnvironmentDay(LLEnvironment::ENV_LOCAL))
    {
        light["day"] = day->getSettings();
        light["day_length"] = env.getEnvironmentDayLength(LLEnvironment::ENV_LOCAL).value();
        light["day_offset"] = env.getEnvironmentDayOffset(LLEnvironment::ENV_LOCAL).value();
        return light;
    }
    if (LLSettingsSky::ptr_t sky = env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL)) light["sky"] = sky->getSettings();
    if (LLSettingsWater::ptr_t water = env.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL)) light["water"] = water->getSettings();
    return light;
}

void applyLight(const LLSD& light)
{
    if (!light.isMap()) return;
    if (!RlvActions::canChangeEnvironment())
    {
        SLCameraModel::toast(str("ShotLightBlocked"));
        return;
    }
    LLEnvironment& env = LLEnvironment::instance();
    if (!light["local"].asBoolean())
    {
        env.clearEnvironment(LLEnvironment::ENV_LOCAL);
    }
    else if (light.has("day"))
    {
        if (LLSettingsDay::ptr_t day = LLSettingsVODay::buildDay(light["day"]))
        {
            env.clearEnvironment(LLEnvironment::ENV_LOCAL);
            env.setEnvironment(LLEnvironment::ENV_LOCAL, day, LLSettingsDay::Seconds(light["day_length"].asInteger()),
                               LLSettingsDay::Seconds(light["day_offset"].asInteger()));
        }
    }
    else
    {
        LLSettingsSky::ptr_t sky = light.has("sky") ? LLSettingsVOSky::buildSky(light["sky"]) : LLSettingsSky::ptr_t();
        LLSettingsWater::ptr_t water = light.has("water") ? LLSettingsVOWater::buildWater(light["water"]) : LLSettingsWater::ptr_t();
        if (sky || water)
        {
            env.clearEnvironment(LLEnvironment::ENV_LOCAL);
            env.setEnvironment(LLEnvironment::ENV_LOCAL, sky, water);
        }
    }
    env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    env.updateEnvironment(LLEnvironment::TRANSITION_INSTANT, true);
}

// ---- where the camera stands ----

// The object the camera follows (its focus object), when it is not the avatar.
LLViewerObject* followedObject()
{
    LLViewerObject* o = gAgentCamera.getFocusObject().get();
    if (!o || o->isDead() || gAgentCamera.getFocusOnAvatar()) return nullptr;
    if (isAgentAvatarValid() && o == gAgentAvatarp) return nullptr;
    return o;
}

S32 currentAnchor()
{
    if (gAgentCamera.getFocusOnAvatar()) return PrestigeCameraShot::ANCHOR_AVATAR;
    LLViewerObject* o = gAgentCamera.getFocusObject().get();
    if (o && !o->isDead())
    {
        return isAgentAvatarValid() && o == gAgentAvatarp ? PrestigeCameraShot::ANCHOR_AVATAR : PrestigeCameraShot::ANCHOR_OBJECT;
    }
    return PrestigeCameraShot::ANCHOR_PLACE;
}

// The camera stays where it is and looks where it looks; from now on it follows `id` (the avatar
// or an object: LLAgentCamera tracks its focus object), or stays put (null).
void holdCameraOn(const LLUUID& id)
{
    const LLVector3d camera = gAgentCamera.getCameraPositionGlobal();
    const LLVector3d focus = gAgentCamera.getFocusGlobal();
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(camera, focus, id);
}

void pickAt(S32 x, S32 y)
{
    sState.pickArmed = false;
    LLViewerObject* o = gViewerWindow ? gViewerWindow->cursorIntersect(x, y, 512.f, nullptr, -1, false, true, true, false) : nullptr;
    if (o && !o->isHUDAttachment())
    {
        // An attachment: the camera follows the avatar wearing it.
        if (o->isAttachment())
        {
            if (LLViewerObject* wearer = o->getAvatar()) o = wearer;
        }
        holdCameraOn(o->getID());
        SLCameraModel::toast(str(o->isAvatar() ? "AnchorFollowsAvatar" : "AnchorFollowsObject"));
    }
    else
    {
        SLCameraModel::toast(str("AnchorNothing"));
    }
}

LLSD vecToLLSD(const LLVector3d& v)
{
    return LLVector3(v).getValue();
}
} // namespace

namespace PrestigeCameraShot
{
// ---- camera mode's life ----

void begin()
{
    sState.floaters.clear();
    sState.asideUntil = 0.0;
    sState.pressInFloater = sState.hudPress = sState.pickPress = sState.pickArmed = false;
    // Camera mode hid the HUDs (and noted the switch, to put it back on exit): shown again if asked.
    LLPipeline::sShowHUDAttachments = showHUDs();
    captureEnvironment();
    sState.frozenAtEntry = worldFrozen();
}

void end()
{
    sState.pickArmed = false;
    sState.pressInFloater = sState.hudPress = sState.pickPress = false;
    // Whatever of the floaters holds the mouse, the keyboard or an open list lets go.
    if (LLView* captor = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture()); captor && owned(captor))
    {
        gFocusMgr.setMouseCapture(nullptr);
    }
    if (LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus()); focus && owned(focus))
    {
        gFocusMgr.setKeyboardFocus(nullptr);
    }
    closeLists();
    // The ones camera mode opened close; the ones open before stay (shown again if put away).
    const std::vector<Shown> shown = sState.floaters;
    sState.floaters.clear();
    for (const Shown& s : shown)
    {
        LLFloater* f = s.handle.get();
        if (!f) continue;
        if (s.opened)
            f->closeFloater();
        else if (s.hidden)
            f->setVisible(true);
    }
    // "Keep": a sky the rail froze stays frozen (SLCameraRestore would thaw it).
    if (!envForPhotoOnly()) SLCameraRestore::keepSky();
}

void afterRestore()
{
    sState.envChanged.disconnect();
    if (sState.envCaptured && envForPhotoOnly())
    {
        LLEnvironment& env = LLEnvironment::instance();
        if (sState.envTouched || env.getSelectedEnvironment() != sState.selected)
        {
            restoreEnvironment();
        }
        if (env.isCloudScrollPaused() != sState.cloudsPaused)
        {
            if (sState.cloudsPaused) env.pauseCloudScroll();
            else env.resumeCloudScroll();
        }
    }
    sState.envCaptured = sState.envTouched = false;
    sState.day.reset();
    sState.sky.reset();
    sState.water.reset();
    if (worldFrozen() != sState.frozenAtEntry) setWorldFrozen(sState.frozenAtEntry);
}

void onShot()
{
    // Never in the photo (they draw in the UI layer, after the capture points), and out of the
    // way while the shutter fires.
    sState.asideUntil = now() + kAsideTime;
}

// ---- HUDs ----

bool showHUDs()
{
    static LLCachedControl<bool> s_show(gSavedSettings, "PrestigeCameraShowHUDs", false);
    return s_show;
}

void setShowHUDs(bool show)
{
    gSavedSettings.setBOOL("PrestigeCameraShowHUDs", show);
    if (SLCameraMode::active())
    {
        LLPipeline::sShowHUDAttachments = show;
        SLCameraModel::toast(str(show ? "ShotHUDsOn" : "ShotHUDsOff"));
    }
}

// ---- the floaters ----

void toggleTool(Tool tool)
{
    const char* name = toolName(tool);
    if (tool == Tool::Light && !RlvActions::canChangeEnvironment())
    {
        SLCameraModel::toast(str("ShotLightBlocked"));
        return;
    }
    if (!SLCameraMode::active())
    {
        LLFloaterReg::toggleInstanceOrBringToFront(name);
        return;
    }
    prune();
    LLFloater* f = LLFloaterReg::findInstance(name);
    if (Shown* entry = entryFor(f))
    {
        // Over the shot: put away (hidden, so a pose survives); put away: back on top.
        entry->hidden = !entry->hidden;
        if (entry->hidden) gFocusMgr.releaseFocusIfNeeded(f); // its keyboard focus, mouse capture, open list
        f->setVisible(!entry->hidden);
        if (!entry->hidden) raise(f);
        return;
    }
    const bool was_open = f && f->getVisible();
    f = LLFloaterReg::showInstance(name, LLSD(), false);
    if (!f) return;
    sState.floaters.push_back({ f->getHandle(), !was_open, false });
    // The camera's keys stay the camera's until a field of the floater is clicked.
    if (LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus()); focus && owned(focus))
    {
        gFocusMgr.setKeyboardFocus(nullptr);
    }
}

bool toolOpen(Tool tool)
{
    LLFloater* f = LLFloaterReg::findInstance(toolName(tool));
    const Shown* entry = SLCameraMode::active() ? entryFor(f) : nullptr;
    return entry && !entry->hidden && f->getVisible();
}

void drawFloaters()
{
    if (!active() || !gViewerWindow) return;
    prune();
    if (sState.floaters.empty()) return;
    // The UI tree is not drawn while camera mode hides it: only these floaters, as
    // LLViewerWindow::draw() would draw them (its scale, the whole window as the dirty rect).
    if (!LLPipeline::RenderUIBuffer) LLView::sDirtyRect = gViewerWindow->getWindowRectScaled();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
    gGL.pushMatrix();
    LLUI::pushMatrix();
    {
        const LLVector2& scale = gViewerWindow->getDisplayScale();
        gGL.scaleUI(scale.mV[VX], scale.mV[VY], 1.f);
        for (const Shown& s : sState.floaters)
        {
            LLFloater* f = s.handle.get();
            if (!f || !f->getVisible()) continue;
            const LLRect r = f->calcScreenRect();
            LLUI::pushMatrix();
            LLUI::translate((F32)r.mLeft, (F32)r.mBottom);
            LLView::drawRetainedAware(f);
            LLUI::popMatrix();
        }
        // Their open lists (a combo box's) over everything, as the popup holder draws them.
        const std::vector<LLView*> lists = openLists();
        for (auto it = lists.rbegin(); it != lists.rend(); ++it)
        {
            S32 sx = 0, sy = 0;
            (*it)->localPointToScreen(0, 0, &sx, &sy);
            LLUI::pushMatrix();
            LLUI::translate((F32)sx, (F32)sy);
            (*it)->draw();
            LLUI::popMatrix();
        }
    }
    LLUI::popMatrix();
    gGL.popMatrix();
    gGL.flush();
}

bool handleFloaterClick(S32 x, S32 y, MASK mask, EMouseClickType click, bool down)
{
    if (!active())
    {
        sState.pressInFloater = false;
        return false;
    }
    prune();
    // A view of one of them holds the mouse (a slider, a button, the title bar's drag).
    LLMouseHandler* captor = gFocusMgr.getMouseCapture();
    if (LLView* view = dynamic_cast<LLView*>(captor); view && owned(view))
    {
        S32 lx = 0, ly = 0;
        captor->screenPointToLocal(x, y, &lx, &ly);
        captor->handleAnyMouseClick(lx, ly, mask, click, down);
        sState.pressInFloater = down;
        return true;
    }
    // An open list of one of them; a press elsewhere closes it.
    if (LLView* list = listAt(x, y))
    {
        S32 lx = 0, ly = 0;
        list->screenPointToLocal(x, y, &lx, &ly);
        list->handleAnyMouseClick(lx, ly, mask, click, down);
        sState.pressInFloater = down;
        return true;
    }
    if (down) closeLists();
    if (LLFloater* f = floaterAt(x, y))
    {
        // Left only: a right click's context menu would open in the hidden interface.
        if (click != CLICK_LEFT && click != CLICK_DOUBLELEFT) return true;
        raise(f);
        S32 lx = 0, ly = 0;
        f->screenPointToLocal(x, y, &lx, &ly);
        f->handleAnyMouseClick(lx, ly, mask, click, down);
        sState.pressInFloater = down;
        return true; // never through a floater to the camera
    }
    if (!down && sState.pressInFloater)
    {
        sState.pressInFloater = false;
        return true;
    }
    if (down)
    {
        // A press on the picture: a field of the floaters lets go of the keyboard.
        if (LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus()); focus && owned(focus))
        {
            gFocusMgr.setKeyboardFocus(nullptr);
        }
    }
    return false;
}

bool handleFloaterHover(S32 x, S32 y, MASK mask)
{
    if (!active()) return false;
    LLMouseHandler* captor = gFocusMgr.getMouseCapture();
    if (LLView* view = dynamic_cast<LLView*>(captor); view && owned(view))
    {
        S32 lx = 0, ly = 0;
        captor->screenPointToLocal(x, y, &lx, &ly);
        captor->handleHover(lx, ly, mask);
        return true;
    }
    LLView* target = listAt(x, y);
    if (!target) target = floaterAt(x, y);
    if (!target) return false;
    gViewerWindow->setCursor(UI_CURSOR_ARROW); // the view under the pointer may ask for another
    S32 lx = 0, ly = 0;
    target->screenPointToLocal(x, y, &lx, &ly);
    target->handleHover(lx, ly, mask);
    return true;
}

bool handleFloaterWheel(S32 x, S32 y, S32 clicks)
{
    if (!active()) return false;
    LLMouseHandler* captor = gFocusMgr.getMouseCapture();
    LLView* target = nullptr;
    if (LLView* view = dynamic_cast<LLView*>(captor); view && owned(view))
        target = view;
    else if (LLView* list = listAt(x, y))
        target = list;
    else
        target = floaterAt(x, y);
    if (!target) return false;
    S32 lx = 0, ly = 0;
    target->screenPointToLocal(x, y, &lx, &ly);
    target->handleScrollWheel(lx, ly, clicks);
    return true;
}

bool keepsKeyboardFocus()
{
    if (!SLCameraMode::active()) return false;
    LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
    return focus && owned(focus) && focus->isInVisibleChain();
}

bool floaterTyping()
{
    if (!keepsKeyboardFocus()) return false;
    const LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());
    return ctrl && ctrl->acceptsTextInput();
}

void releaseFloaterKeys()
{
    if (keepsKeyboardFocus()) gFocusMgr.setKeyboardFocus(nullptr);
}

// ---- the world under the pointer ----

Click worldClick(S32 x, S32 y, MASK mask, EMouseClickType click, bool down)
{
    if (!SLCameraMode::active() || (click != CLICK_LEFT && click != CLICK_DOUBLELEFT)) return Click::NotMine;
    if (!down)
    {
        if (sState.hudPress)
        {
            sState.hudPress = false;
            return Click::ToTools;
        }
        if (sState.pickPress)
        {
            sState.pickPress = false;
            return Click::Taken;
        }
        return Click::NotMine;
    }
    sState.hudPress = sState.pickPress = false;
    if (mask & (MASK_ALT | MASK_CONTROL | MASK_SHIFT)) return Click::NotMine; // the camera tools, as before
    if (sState.pickArmed)
    {
        sState.pickPress = true;
        pickAt(x, y);
        return Click::Taken;
    }
    // A HUD under the pointer (shown in the viewfinder): a normal touch, as the tool does it
    // (opaque faces; the shot never has them).
    if (showHUDs() && LLPipeline::sShowHUDAttachments && gViewerWindow)
    {
        LLViewerObject* o = gViewerWindow->cursorIntersect(x, y, 512.f, nullptr, -1, false, false, true, false);
        if (o && o->isHUDAttachment())
        {
            sState.hudPress = true;
            return Click::ToTools;
        }
    }
    return Click::NotMine;
}

// ---- the world, the shutter, the sky ----

bool worldFrozen()
{
    static LLCachedControl<bool> s_frozen(gSavedSettings, "SlipstreamWorldFrozen", false);
    return s_frozen;
}

void setWorldFrozen(bool frozen)
{
    // View > Freeze World (Ctrl+Shift+F) is this same toggle.
    gSavedSettings.setBOOL("SlipstreamWorldFrozen", frozen);
}

bool quiet()
{
    static LLCachedControl<bool> s_play(gSavedSettings, "PlayModeUISndSnapshot", true);
    return !s_play;
}

void setQuiet(bool on)
{
    gSavedSettings.setBOOL("PlayModeUISndSnapshot", !on);
    if (SLCameraMode::active()) SLCameraModel::toast(str(on ? "ShotQuietOn" : "ShotQuietOff"), 3.0);
}

bool envForPhotoOnly()
{
    static LLCachedControl<bool> s_photo(gSavedSettings, "PrestigeCameraEnvForPhotoOnly", true);
    return s_photo;
}

void setEnvForPhotoOnly(bool photo_only)
{
    gSavedSettings.setBOOL("PrestigeCameraEnvForPhotoOnly", photo_only);
    if (SLCameraMode::active()) SLCameraModel::toast(str(photo_only ? "ShotSkyPhotoOn" : "ShotSkyKeepOn"), 3.0);
}

// ---- where the camera stands ----

S32 anchor()
{
    return sState.pickArmed ? ANCHOR_OBJECT : currentAnchor();
}

void setAnchor(S32 which)
{
    switch (which)
    {
    case ANCHOR_AVATAR:
        sState.pickArmed = false;
        if (isAgentAvatarValid()) holdCameraOn(gAgentAvatarp->getID());
        break;
    case ANCHOR_PLACE:
        sState.pickArmed = false;
        holdCameraOn(LLUUID::null);
        break;
    case ANCHOR_OBJECT:
        // Again on the chosen one: pick another (or let go of the pick).
        sState.pickArmed = !sState.pickArmed;
        if (sState.pickArmed) SLCameraModel::toast(str("AnchorPick"), 3.0);
        break;
    default:
        break;
    }
}

bool pickArmed()
{
    return sState.pickArmed;
}

std::string anchorNote()
{
    if (sState.pickArmed) return str("AnchorPick");
    if (LLViewerObject* o = followedObject()) return str(o->isAvatar() ? "AnchorFollowsAvatar" : "AnchorFollowsObject");
    return std::string();
}

// ---- camera presets ----

void savePresetExtras(LLSD& sd)
{
    const LLVector3d camera = gAgentCamera.getCameraPositionGlobal();
    const LLVector3d focus = gAgentCamera.getFocusGlobal();
    const S32 where = currentAnchor();
    if (where == ANCHOR_AVATAR && isAgentAvatarValid())
    {
        // In the avatar's frame: it is put back around the avatar, wherever it stands and faces.
        const LLVector3d me = gAgent.getPositionGlobal();
        const LLQuaternion inverse = ~gAgent.getQuat();
        sd["anchor"] = "avatar";
        sd["avatar_camera"] = (LLVector3(camera - me) * inverse).getValue();
        sd["avatar_focus"] = (LLVector3(focus - me) * inverse).getValue();
    }
    else if (LLViewerObject* o = where == ANCHOR_OBJECT ? followedObject() : nullptr)
    {
        const LLVector3d at = o->getPositionGlobal();
        sd["anchor"] = "object";
        sd["object_id"] = o->getID();
        sd["object_camera"] = vecToLLSD(camera - at);
        sd["object_focus"] = vecToLLSD(focus - at);
    }
    else
    {
        sd["anchor"] = "place"; // region + camera_position / camera_focus (slcamerapresets.cpp)
    }
    sd["light"] = lightToLLSD();
}

bool applyPresetExtras(const LLSD& sd)
{
    if (sd.has("light")) applyLight(sd["light"]);
    // The filters: the setup's FX preset again, even when it is the active one already (the
    // stack's unsaved changes since go).
    if (sd.has("settings") && sd["settings"].has("SLFxActivePreset"))
    {
        const std::string fx = sd["settings"]["SLFxActivePreset"].asString();
        SLFxPresets& presets = SLFxPresets::instance();
        if (!fx.empty() && presets.getActive() == fx && presets.isDirty() && presets.exists(fx)) presets.load(fx);
    }
    sState.pickArmed = false;
    const std::string where = sd["anchor"].asString();
    if (where == "avatar" && isAgentAvatarValid() && sd.has("avatar_camera"))
    {
        const LLVector3d me = gAgent.getPositionGlobal();
        const LLQuaternion rot = gAgent.getQuat();
        const LLVector3d camera = me + LLVector3d(LLVector3(sd["avatar_camera"]) * rot);
        const LLVector3d focus = me + LLVector3d(LLVector3(sd["avatar_focus"]) * rot);
        gAgentCamera.setFocusOnAvatar(false, false);
        gAgentCamera.setCameraPosAndFocusGlobal(camera, focus, gAgentAvatarp->getID());
        return true;
    }
    if (where == "object")
    {
        LLViewerObject* o = gObjectList.findObject(sd["object_id"].asUUID());
        if (o && !o->isDead())
        {
            const LLVector3d at = o->getPositionGlobal();
            gAgentCamera.setFocusOnAvatar(false, false);
            gAgentCamera.setCameraPosAndFocusGlobal(at + LLVector3d(sd["object_camera"]), at + LLVector3d(sd["object_focus"]), o->getID());
            return true;
        }
        SLCameraModel::toast(str("AnchorObjectGone"), 3.0);
        return true; // the camera stays where it is
    }
    return false;
}
} // namespace PrestigeCameraShot
