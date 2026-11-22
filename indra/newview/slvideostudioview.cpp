/**
 * @file slvideostudioview.cpp
 * @brief Video Studio's overlay on the new interface kit (see slvideostudioview.h).
 *
 * One document, data model "vstudio": a panel docked to the right (tabs Track / Ride / Record /
 * Replay, each a list of rows in the camera rail's style: sliders, toggles, choices, buttons, notes)
 * and a transport bar at the bottom (play, scrub, time, record, leave). Rows and their actions come
 * from SLVideoStudio; this file only presents them and forwards input.
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
#include "slvideostudioview.h"

#include "slvideostudio.h"
#include "slvideostudiodoc.h"

#include "llframetimer.h"
#include "llnewui.h"
#include "llnewuikit.h"
#include "llrender.h"
#include "llui.h"
#include "llview.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "vkgfx/vk_ui_texture_host.h" // slUIMarkExternalLayer

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace
{

struct VRow
{
    std::string id, track, label, value, w;
    int kind = 0, index = 0;
    bool on = false, enabled = true;
    std::vector<std::string> buttons;
};
struct VTab
{
    std::string label;
    bool on = false;
};

struct View
{
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle model;
    bool shown = false, changed = false, hiddenNow = false;
    U32 theme = 0;
    float density = 0.f, k = 1.f;
    S32 rootW = 0, rootH = 0;
    S32 lastX = S32_MIN, lastY = S32_MIN;
    Rml::Element* lastHover = nullptr;
    U32 markedFrame = U32_MAX;
    bool pressedInDoc = false;
    int dragRow = -1;          // a slider row being dragged
    bool scrubDrag = false;    // the transport's scrub bar being dragged
    F64 nextRows = 0.0;        // the rows are re-read at 8 Hz (and after every action)
    std::vector<SLVideoStudio::Row> lastRows;

    // the data model
    std::vector<VRow> rows;
    std::vector<VTab> tabs;
    std::string title, leaveLabel, hint, timeText, playLabel, recLabel, scrubW, recStatus;
    bool hidden = false, playing = false, recording = false;
};
View gView;

F64 now() { return LLTimer::getTotalSeconds(); }
void markChanged() { gView.changed = true; }
void dirty(const char* variable)
{
    if (gView.model) gView.model.DirtyVariable(variable);
    markChanged();
}
template <typename T> void set(T& field, const T& value, const char* variable)
{
    if (field == value) return;
    field = value;
    dirty(variable);
}
int rmlModifiers(MASK mask)
{
    int m = 0;
    if (mask & MASK_CONTROL) m |= Rml::Input::KM_CTRL;
    if (mask & MASK_SHIFT) m |= Rml::Input::KM_SHIFT;
    if (mask & MASK_ALT) m |= Rml::Input::KM_ALT;
    return m;
}
bool within(Rml::Element* e, const char* id)
{
    for (; e; e = e->GetParentNode())
        if (e->GetId() == id) return true;
    return false;
}
// "vrow-3" / "vtrack-3" -> 3 (or -1).
int rowOf(Rml::Element* e, const char* prefix)
{
    const size_t n = strlen(prefix);
    for (; e; e = e->GetParentNode())
        if (e->GetId().compare(0, n, prefix) == 0) return atoi(e->GetId().c_str() + n);
    return -1;
}

// The rows of the open panel, the tabs, the transport (rows at 8 Hz or when asked).
void refresh(bool rowsNow)
{
    View& v = gView;
    if (v.tabs.size() != SLVideoStudio::PANEL_COUNT) v.tabs.assign(SLVideoStudio::PANEL_COUNT, VTab());
    bool tabsChanged = false;
    for (int i = 0; i < SLVideoStudio::PANEL_COUNT; ++i)
    {
        const std::string label = SLVideoStudio::panelTitle((SLVideoStudio::Panel)i);
        const bool on = SLVideoStudio::panel() == i;
        if (v.tabs[i].label != label || v.tabs[i].on != on)
        {
            v.tabs[i].label = label;
            v.tabs[i].on = on;
            tabsChanged = true;
        }
    }
    if (tabsChanged) dirty("tabs");
    const F64 t = now();
    if (rowsNow || t >= v.nextRows)
    {
        v.nextRows = t + 0.125;
        std::vector<SLVideoStudio::Row> rows = SLVideoStudio::rows();
        if (!(rows == v.lastRows))
        {
            v.rows.clear();
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const SLVideoStudio::Row& d = rows[i];
                VRow r;
                r.index = (int)i;
                r.id = llformat("vrow-%d", (int)i);
                r.track = llformat("vtrack-%d", (int)i);
                r.label = d.label;
                r.value = d.value;
                r.kind = d.kind;
                r.on = d.on;
                r.enabled = d.enabled;
                r.buttons = d.buttons;
                r.w = llformat("%.1f%%", llclamp(d.fraction, 0.f, 1.f) * 100.f);
                v.rows.push_back(r);
            }
            v.lastRows.swap(rows);
            dirty("rows");
        }
        set(v.hint, SLVideoStudio::hint(), "hint");
    }
    set(v.playing, SLVideoStudio::playing(), "playing");
    set(v.recording, SLVideoStudio::recording(), "recording");
    set(v.playLabel, std::string(v.playing ? "Stop" : "Play"), "playLabel");
    set(v.recLabel, std::string(v.recording ? "Stop recording" : "Record"), "recLabel");
    set(v.timeText, SLVideoStudio::timeText(), "timeText");
    set(v.scrubW, llformat("%.1f%%", llclamp(SLVideoStudio::scrub(), 0.f, 1.f) * 100.f), "scrubW");
    set(v.recStatus, SLVideoStudio::recordStatus(), "recStatus");
    set(v.hidden, SLVideoStudio::overlayHidden(), "hidden");
}

bool toRml(S32 x, S32 y, MASK mask)
{
    View& v = gView;
    if (!v.shown || !v.context || v.density <= 0.f || v.hidden) return false;
    if (x != v.lastX || y != v.lastY)
    {
        v.lastX = x;
        v.lastY = y;
        v.context->ProcessMouseMove(int((float)x * v.density), int((float)(v.rootH - y) * v.density), rmlModifiers(mask));
    }
    Rml::Element* hover = v.context->GetHoverElement();
    if (hover != v.lastHover)
    {
        v.lastHover = hover;
        markChanged();   // :hover styles
    }
    return hover && hover != v.document && hover->GetOwnerDocument() == v.document;
}

// A slider row or the scrub bar follows the pointer's x.
F32 fractionAt(const char* id, S32 x)
{
    View& v = gView;
    Rml::Element* track = v.document ? v.document->GetElementById(id) : nullptr;
    if (!track || v.density <= 0.f) return -1.f;
    const float left = track->GetAbsoluteLeft(), width = track->GetClientWidth();
    if (width <= 0.f) return -1.f;
    return llclamp(((float)x * v.density - left) / width, 0.f, 1.f);
}
void followDrags()
{
    View& v = gView;
    if (!gViewerWindow || (v.dragRow < 0 && !v.scrubDrag)) return;
    const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
    if (v.dragRow >= 0)
    {
        const F32 f = fractionAt(llformat("vtrack-%d", v.dragRow).c_str(), mouse.mX);
        if (f >= 0.f) SLVideoStudio::setFraction((size_t)v.dragRow, f);
        refresh(true);
    }
    if (v.scrubDrag)
    {
        const F32 f = fractionAt("scrub", mouse.mX);
        if (f >= 0.f) SLVideoStudio::setScrub(f, true);
    }
}

Rml::ElementDocument* loadDocument(Rml::Context* context)
{
    View& v = gView;
    v.theme = gSavedSettings.getU32("UIKitTheme") == 1 ? 1u : 0u;
    const LLNewUIKit::Tokens tokens = LLNewUIKit::tokens(v.theme == 1 ? LLNewUIKit::Theme::Light : LLNewUIKit::Theme::Dark, true);
    return context->LoadDocumentFromMemory(LLNewUIKit::themed(kStudioRml, tokens), "slvideostudio.rml");
}

void retheme()
{
    View& v = gView;
    Rml::ElementDocument* fresh = loadDocument(v.context);
    if (!fresh) return;   // keep the old look rather than lose the overlay
    v.document->Close();
    v.document = fresh;
    v.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    v.lastHover = nullptr;
    v.lastX = v.lastY = S32_MIN;
    if (v.model) v.model.DirtyAllVariables();
    markChanged();
}

bool createDocument(Rml::Context* context)
{
    View& v = gView;
    v.context = context;
    if (!LLNewUI::instance().active()) LLNewUIKit::loadFonts();
    try
    {
        Rml::DataModelConstructor m = context->CreateDataModel("vstudio");
        if (!m) return false;
        m.RegisterArray<std::vector<std::string>>();   // (before the struct that holds one)
        if (auto r = m.RegisterStruct<VRow>())
        {
            r.RegisterMember("id", &VRow::id);
            r.RegisterMember("track", &VRow::track);
            r.RegisterMember("label", &VRow::label);
            r.RegisterMember("value", &VRow::value);
            r.RegisterMember("w", &VRow::w);
            r.RegisterMember("kind", &VRow::kind);
            r.RegisterMember("index", &VRow::index);
            r.RegisterMember("on", &VRow::on);
            r.RegisterMember("enabled", &VRow::enabled);
            r.RegisterMember("buttons", &VRow::buttons);
        }
        m.RegisterArray<std::vector<VRow>>();
        if (auto t = m.RegisterStruct<VTab>())
        {
            t.RegisterMember("label", &VTab::label);
            t.RegisterMember("on", &VTab::on);
        }
        m.RegisterArray<std::vector<VTab>>();
        m.Bind("rows", &v.rows);
        m.Bind("tabs", &v.tabs);
        m.Bind("title", &v.title);
        m.Bind("leaveLabel", &v.leaveLabel);
        m.Bind("hint", &v.hint);
        m.Bind("timeText", &v.timeText);
        m.Bind("playLabel", &v.playLabel);
        m.Bind("recLabel", &v.recLabel);
        m.Bind("scrubW", &v.scrubW);
        m.Bind("recStatus", &v.recStatus);
        m.Bind("hidden", &v.hidden);
        m.Bind("playing", &v.playing);
        m.Bind("recording", &v.recording);
        m.BindEventCallback("vtab", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                const int i = args.empty() ? -1 : args[0].Get<int>();
                                if (i >= 0 && i < SLVideoStudio::PANEL_COUNT) SLVideoStudio::setPanel((SLVideoStudio::Panel)i);
                                refresh(true);
                            });
        m.BindEventCallback("vturn", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args)
                            {
                                event.StopPropagation();
                                if (args.size() < 2) return;
                                SLVideoStudio::turn((size_t)args[0].Get<int>(), args[1].Get<int>());
                                refresh(true);
                            });
        m.BindEventCallback("vpress", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args)
                            {
                                event.StopPropagation();
                                if (args.size() < 2) return;
                                SLVideoStudio::press((size_t)args[0].Get<int>(), (size_t)args[1].Get<int>());
                                refresh(true);
                            });
        m.BindEventCallback("play", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                SLVideoStudio::togglePlay();
                                refresh(true);
                            });
        m.BindEventCallback("rec", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                SLVideoStudio::toggleRecord();
                                refresh(true);
                            });
        m.BindEventCallback("leave", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { SLVideoStudio::stop(); });
        v.model = m.GetModelHandle();
    }
    catch (const std::bad_alloc&)
    {
        context->RemoveDataModel("vstudio");
        return false;
    }
    v.document = loadDocument(context);
    if (!v.document)
    {
        context->RemoveDataModel("vstudio");
        v.model = Rml::DataModelHandle();
        return false;
    }
    return true;
}
} // namespace

bool SLVideoStudioView::start(Rml::Context* context)
{
    if (!context) return false;
    if (gView.context != context) release();
    if (!gView.document && !createDocument(context))
    {
        gView = View();
        return false;
    }
    View& v = gView;
    v.title = "Video Studio";
    v.leaveLabel = "Leave";
    v.lastRows.clear();
    v.dragRow = -1;
    v.scrubDrag = v.pressedInDoc = false;
    v.rootW = v.rootH = 0;
    v.lastX = v.lastY = S32_MIN;
    v.lastHover = nullptr;
    v.nextRows = 0.0;
    refresh(true);
    if (v.model) v.model.DirtyAllVariables();
    v.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    v.shown = true;
    v.changed = true;
    return true;
}

void SLVideoStudioView::stop()
{
    if (gView.document && gView.shown) gView.document->Hide();
    if (gView.scrubDrag) SLVideoStudio::setScrub(SLVideoStudio::scrub(), false);
    gView.shown = false;
    gView.dragRow = -1;
    gView.scrubDrag = false;
}

void SLVideoStudioView::release()
{
    gView = View();
}

bool SLVideoStudioView::active()
{
    return gView.shown;
}

float SLVideoStudioView::dpScale()
{
    return gView.shown ? gView.k : 1.f;
}

bool SLVideoStudioView::sync(float density)
{
    View& v = gView;
    if (!v.shown) return false;
    if (density != v.density)
    {
        v.density = density;
        markChanged();
    }
    static LLCachedControl<U32> s_theme(gSavedSettings, "UIKitTheme", 0);
    if ((s_theme == 1 ? 1u : 0u) != v.theme) retheme();
    if (LLView* root = LLUI::getInstance()->getRootView())
    {
        const LLRect& r = root->getRect();
        static LLCachedControl<F32> s_size(gSavedSettings, "SLCameraModeOverlaySize", 1.f);
        const float k = llclamp((F32)s_size, 0.75f, 2.5f);
        if (r.getWidth() != v.rootW || r.getHeight() != v.rootH || k != v.k)
        {
            v.rootW = r.getWidth();
            v.rootH = r.getHeight();
            v.k = k;
            markChanged();
        }
    }
    followDrags();
    refresh(false);
    if (gViewerWindow)
    {
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        toRml(mouse.mX, mouse.mY, MASK_NONE);   // :hover (the studio gets no hover events of its own)
    }
    const bool changed = v.changed;
    v.changed = false;
    return changed;
}

void SLVideoStudioView::markLayer()
{
    const U32 frame = LLFrameTimer::getFrameCount();
    if (!gView.shown || gView.markedFrame == frame) return;
    gView.markedFrame = frame;
    gGL.flush();
    slUIMarkExternalLayer();
}

bool SLVideoStudioView::handleMouse(S32 x, S32 y, MASK mask, EMouseClickType click, bool down)
{
    View& v = gView;
    if (!v.shown) return false;
    const bool over = toRml(x, y, mask);
    Rml::Element* hover = v.context->GetHoverElement();
    if (click != CLICK_LEFT && click != CLICK_DOUBLELEFT)
        return click == CLICK_RIGHT && down && over;   // right over the panel: nothing (its release goes on)
    if (down)
    {
        if (!over) return false;
        v.pressedInDoc = true;
        const int track = rowOf(hover, "vtrack-");
        if (track >= 0)
        {
            v.dragRow = track;   // a press on the track sets it there; a drag follows
            followDrags();
            return true;
        }
        if (within(hover, "scrub"))
        {
            v.scrubDrag = true;
            followDrags();
            return true;
        }
        v.context->ProcessMouseButtonDown(0, rmlModifiers(mask));
        markChanged();
        return true;
    }
    if (!v.pressedInDoc) return false;
    v.pressedInDoc = false;
    if (v.dragRow >= 0)
    {
        followDrags();
        v.dragRow = -1;
        return true;
    }
    if (v.scrubDrag)
    {
        followDrags();
        v.scrubDrag = false;
        SLVideoStudio::setScrub(SLVideoStudio::scrub(), false);
        return true;
    }
    v.context->ProcessMouseButtonUp(0, rmlModifiers(mask));
    markChanged();
    return true;
}

bool SLVideoStudioView::handleWheel(S32 x, S32 y, S32 clicks, MASK mask)
{
    if (!toRml(x, y, mask)) return false;
    Rml::Element* hover = gView.context->GetHoverElement();
    // A row under the pointer turns (wheel up: forward).
    const int row = rowOf(hover, "vrow-");
    if (row >= 0 && row < (int)gView.lastRows.size() && gView.lastRows[(size_t)row].kind <= SLVideoStudio::ROW_CHOICE)
    {
        SLVideoStudio::turn((size_t)row, clicks < 0 ? 1 : -1);
        refresh(true);
        return true;
    }
    if (within(hover, "scrub"))
    {
        SLVideoStudio::setScrub(SLVideoStudio::scrub() + (clicks < 0 ? 0.01f : -0.01f), false);
        markChanged();
        return true;
    }
    gView.context->ProcessMouseWheel(float(clicks), rmlModifiers(mask));
    markChanged();
    return true;
}

bool SLVideoStudioView::overDocument(S32 x, S32 y)
{
    return toRml(x, y, MASK_NONE);
}
