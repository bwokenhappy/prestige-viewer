/**
 * @file prestigecamerafiltersview.cpp
 * @brief Camera mode's Filters panel and before / after split line. See
 *        prestigecamerafiltersview.h; the stack itself is prestigecamerafilters.h.
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

#include "prestigecamerafiltersview.h"

#include "prestigecamerafilters.h"
#include "slcamerarail.h"
#include "slcameraview.h"

#include "llnewuikit.h"
#include "lltimer.h"
#include "lltrans.h"
#include "llui.h"
#include "llview.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>

#include <climits>
#include <cstdio>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace PCF = PrestigeCameraFilters;

namespace
{
// Layout only; colours, radii and type sizes are the kit's tokens ($name).
const char* kFiltersRml = R"rml(<rml><head><title>camera filters</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#fpanel { position: absolute; top: 70dp; bottom: 100dp; right: 24dp; width: 320dp; box-sizing: border-box; pointer-events: auto; }
#fpanel.left { right: auto; left: 24dp; }
.top #fpanel { top: 100dp; bottom: 28dp; }
#fhead { position: absolute; left: 8dp; right: 8dp; top: 6dp; height: 28dp; display: flex; flex-direction: row; align-items: center; }
#fhead .title { flex: 1 1 0; }
#fbody { position: absolute; left: 8dp; right: 8dp; top: 40dp; bottom: 74dp; overflow-y: auto; }
#ffoot { position: absolute; left: 8dp; right: 8dp; bottom: 8dp; }
#ffoot .trow { padding: 4dp 0; }
#ffoot .prow { display: flex; flex-direction: row; gap: 6dp; margin-top: 4dp; }
#ffoot .prow .btn { flex: 1 1 0; text-align: center; border: 1dp $border; }
#ffoot .prow .btn.dim { opacity: 0.45; }
.fslot { padding: 4dp 0; border-top: 2dp transparent; border-bottom: 1dp $border; }
.fslot.drop { border-top-color: $accent; }
.fslot.dropend { border-bottom: 2dp $accent; }
.fslot.dragging { opacity: 0.5; }
.fhead2 { display: flex; flex-direction: row; align-items: center; height: 26dp; gap: 6dp; }
.fgrip { width: 12dp; height: 16dp; cursor: pointer; }
.fgrip div { height: 2dp; margin-top: 3dp; background-color: $muted; }
.fgrip:hover div { background-color: $accent; }
.fname { flex: 1 1 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; cursor: pointer; }
.fname .chev { margin: 0 6dp 0 2dp; }
.fstatus { color: $warn; font-size: $fontCaption; margin: 2dp 0 2dp 18dp; }
.fparams { margin-left: 18dp; }
#fsplitwrap { position: absolute; top: 0; bottom: 0; width: 0; }
#fsplit { position: absolute; left: -1dp; top: 0; bottom: 0; width: 2dp; background-color: $finderStrong; }
#fsplithit { position: absolute; left: -6dp; top: 0; bottom: 0; width: 12dp; pointer-events: auto; cursor: pointer; }
#fsplitknob { position: absolute; left: -8dp; top: 50%; margin-top: -14dp; width: 28dp; height: 28dp; pointer-events: auto;
              decorator: radial-gradient(circle closest-side, $finderStrong 62%, $finderClear 100%); }
#fsplithit:hover #fsplitknob { decorator: radial-gradient(circle closest-side, $accent 62%, $accent00 100%); }
.fside { position: absolute; top: 24dp; padding: 2dp 8dp; background-color: $bar; border-radius: $radius; font-size: $fontCaption; white-space: nowrap; }
#fbefore { right: 10dp; } #fafter { left: 10dp; }
#fpick { position: absolute; top: 70dp; bottom: 100dp; right: 352dp; width: 300dp; box-sizing: border-box; pointer-events: auto; }
#fpick.left { right: auto; left: 352dp; }
.top #fpick { top: 100dp; bottom: 28dp; }
#fpick .phead { position: absolute; left: 12dp; right: 12dp; top: 8dp; }
#fpicklist { position: absolute; left: 8dp; right: 8dp; top: 32dp; bottom: 8dp; overflow-y: auto; }
#fpick .opt.dim { opacity: 0.45; }
#fpick .gtitle { margin: 8dp 4dp 2dp 4dp; }
</style></head><body><div data-model="camfilters" data-class-top="barTop">
<div id="fsplitwrap" data-if="splitShown" data-style-left="splitLeft">
  <div id="fsplit"></div>
  <div id="fsplithit"><div id="fsplitknob"></div></div>
  <div id="fbefore" class="fside">{{beforeLabel}}</div><div id="fafter" class="fside">{{afterLabel}}</div>
</div>
<div id="fpanel" class="panel" data-if="visible" data-class-left="left">
  <div id="fhead"><div class="title">{{title}}</div><div class="btn" data-event-click="fclose()"><ic name="x"/></div></div>
  <div id="fbody">
    <div class="caption" data-if="slots.size == 0">{{emptyText}}</div>
    <div class="fslot" data-for="s : slots" data-attr-id="s.id" data-class-drop="s.drop" data-class-dropend="s.dropEnd" data-class-dragging="s.dragging">
      <div class="fhead2">
        <div class="fgrip" data-attr-id="s.grip"><div></div><div></div><div></div></div>
        <div class="toggle" data-class-on="s.on" data-event-click="fon(it_index)"><div class="knob"></div></div>
        <div class="fname" data-event-click="fexpand(it_index)"><span class="chev" data-class-down="s.open" data-class-right="!s.open"></span>{{s.label}}</div>
        <div class="btn" data-event-click="fremove(it_index)"><ic name="x"/></div>
      </div>
      <div class="rrow" data-attr-id="s.row" data-class-dim="!s.on">
        <div class="rl">{{strengthLabel}}</div>
        <div class="rtrack" data-attr-id="s.track"><div class="tr"><div class="fl" data-style-width="s.w"></div></div></div>
        <div class="rv">{{s.value}}</div>
      </div>
      <div class="fstatus" data-if="s.status != ''">{{s.status}}</div>
      <div class="fparams" data-if="s.open">
        <div class="caption" data-if="s.params.size == 0">{{noParamsText}}</div>
        <div class="rrow" data-for="p : s.params" data-attr-id="p.id">
          <div class="rl">{{p.label}}</div>
          <div class="rtrack" data-if="p.kind == 0" data-attr-id="p.track"><div class="tr"><div class="fl" data-style-width="p.w"></div></div></div>
          <div class="toggle" data-if="p.kind == 1" data-class-on="p.on" data-event-click="pturn(p.id, 1)"><div class="knob"></div></div>
          <div class="rchoice" data-if="p.kind == 2"><div class="arr" data-event-click="pturn(p.id, -1)"><span class="chev left"></span></div>
            <div class="cv" data-event-click="pturn(p.id, 1)">{{p.value}}</div>
            <div class="arr" data-event-click="pturn(p.id, 1)"><span class="chev right"></span></div></div>
          <div class="rv" data-if="p.kind != 2">{{p.value}}</div>
          <div class="rreset" data-class-shown="p.changed" data-event-click="preset(p.id)"><div class="ring"></div><div class="tip"></div></div>
        </div>
      </div>
    </div>
  </div>
  <div id="ffoot">
    <div class="trow" data-event-click="fcompare()"><div class="label">{{compareLabel}}</div><div class="toggle" data-class-on="compare"><div class="knob"></div></div></div>
    <div class="prow"><div class="btn" data-class-dim="full" data-event-click="fpick()">{{addLabel}}</div><div class="btn" data-event-click="flut()">{{lutLabel}}</div></div>
  </div>
</div>
<div id="fpick" class="popover" data-if="visible && picking" data-class-left="left">
  <div class="phead caption">{{pickTitle}}</div>
  <div id="fpicklist">
    <div class="caption" data-if="loading">{{pickLoading}}</div>
    <div data-for="g : groups">
      <div class="caption gtitle">{{g.title}}</div>
      <div class="opt" data-for="c : g.items" data-class-dim="c.added" data-event-click="fadd(c.id)">
        <div class="col">{{c.label}}</div><div class="note" data-if="c.note != ''">{{c.note}}</div><div class="tick" data-if="c.added">✓</div></div>
    </div>
  </div>
</div>
</div></body></rml>)rml";

struct FParam
{
    std::string id, track, label, value, w;
    int kind = 0;
    bool on = false, changed = false;
};
struct FSlot
{
    std::string id, grip, row, track, label, value, w, status;
    bool on = false, open = false, drop = false, dropEnd = false, dragging = false;
    std::vector<FParam> params;
};
struct FChoice
{
    std::string id, label, note;
    bool added = false;
};
struct FGroup
{
    std::string title;
    std::vector<FChoice> items;
};

enum Drag
{
    DRAG_NONE = 0,
    DRAG_STRENGTH,
    DRAG_PARAM,
    DRAG_REORDER,
    DRAG_SPLIT
};

struct View
{
    Rml::Context* context = nullptr;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle model;
    bool shown = false, changed = true;
    U32 theme = 0;
    float density = 0.f, k = 1.f;
    S32 rootW = 0, rootH = 0;
    // model
    bool visible = false, left = false, barTop = false, compare = false, picking = false, full = false, loading = false;
    bool splitShown = false;
    std::string title, emptyText, noParamsText, strengthLabel, compareLabel, addLabel, lutLabel, pickTitle, pickLoading;
    std::string beforeLabel, afterLabel, splitLeft = "0dp";
    std::vector<FSlot> slots;
    std::vector<FGroup> groups;
    std::vector<std::string> choiceTechniques;   // "fc-N" -> technique
    std::string signature, pickSignature;
    std::set<std::string> expanded;              // techniques whose sliders show
    F64 nextRefresh = 0.0;
    // input
    S32 lastX = S32_MIN, lastY = S32_MIN;
    bool pressedInDoc = false;
    Drag drag = DRAG_NONE;
    int dragSlot = -1, dragParam = -1, dropAt = -1;
};
View gView;

void markChanged()
{
    gView.changed = true;
}

void dirty(const char* variable)
{
    if (gView.model)
        gView.model.DirtyVariable(variable);
    markChanged();
}

template <typename T> void set(T& field, const T& value, const char* variable)
{
    if (field == value)
        return;
    field = value;
    dirty(variable);
}

std::string str(const char* name)
{
    return LLTrans::getString(std::string("PrestigeCameraFilters") + name);
}

bool openSetting()
{
    return gSavedSettings.getBOOL("PrestigeCameraFiltersOpen");
}

// The element or one of its ancestors has an id starting with `prefix`; that id.
std::string idWithin(Rml::Element* e, const char* prefix)
{
    for (; e; e = e->GetParentNode())
        if (e->GetId().rfind(prefix, 0) == 0)
            return e->GetId();
    return std::string();
}

bool within(Rml::Element* e, const char* id)
{
    for (; e; e = e->GetParentNode())
        if (e->GetId() == id)
            return true;
    return false;
}

// "fprow-2-5" / "fptrack-2-5" -> 2, 5.
bool twoIds(const std::string& id, int& a, int& b)
{
    const size_t dash = id.find('-');
    return dash != std::string::npos && std::sscanf(id.c_str() + dash + 1, "%d-%d", &a, &b) == 2;
}

bool oneId(const std::string& id, int& a)
{
    const size_t dash = id.find('-');
    return dash != std::string::npos && std::sscanf(id.c_str() + dash + 1, "%d", &a) == 1;
}

// Pointer to RmlUi (real moves only); true when it is over one of our elements.
bool toRml(S32 x, S32 y, MASK mask)
{
    View& v = gView;
    if (!v.shown || !v.context || v.density <= 0.f)
        return false;
    if (x != v.lastX || y != v.lastY)
    {
        v.lastX = x;
        v.lastY = y;
        int m = 0;
        if (mask & MASK_CONTROL) m |= Rml::Input::KM_CTRL;
        if (mask & MASK_SHIFT) m |= Rml::Input::KM_SHIFT;
        if (mask & MASK_ALT) m |= Rml::Input::KM_ALT;
        v.context->ProcessMouseMove(int((float)x * v.density), int((float)(v.rootH - y) * v.density), m);
        markChanged();   // :hover styles
    }
    Rml::Element* hover = v.context->GetHoverElement();
    return hover && hover != v.document && hover->GetOwnerDocument() == v.document;
}

// Where the panel goes: away from the Photo Studio rail when that is showing.
bool panelLeft()
{
    const bool controls = gSavedSettings.getBOOL("SLCameraModeControlsOpen");
    const bool railShows = controls && SLCameraRail::open() && !gSavedSettings.getBOOL("SLCameraModeBarHidden");
    return railShows && SLCameraRail::dockedRight();
}

// The stack's rows from the model; dirty when anything changed.
void refreshSlots()
{
    View& v = gView;
    const std::vector<PCF::Filter> list = PCF::filters();
    std::vector<FSlot> slots;
    std::string sig;
    bool anyOn = false;
    for (size_t i = 0; i < list.size(); ++i)
    {
        const PCF::Filter& f = list[i];
        FSlot s;
        s.id = llformat("fslot-%d", (int)i);
        s.grip = llformat("fgrip-%d", (int)i);
        s.row = llformat("fsrow-%d", (int)i);
        s.track = llformat("fstr-%d", (int)i);
        s.label = PCF::label(f.technique);
        s.on = f.on;
        anyOn |= f.on;
        s.value = llformat("%d%%", ll_round(f.strength * 100.f));
        s.w = llformat("%d%%", ll_round(f.strength * 100.f));
        s.status = PCF::status(i);
        s.open = v.expanded.count(f.technique) > 0;
        s.dragging = v.drag == DRAG_REORDER && v.dragSlot == (int)i;
        s.drop = v.drag == DRAG_REORDER && v.dropAt == (int)i && v.dropAt != v.dragSlot && v.dropAt != v.dragSlot + 1;
        s.dropEnd = v.drag == DRAG_REORDER && v.dropAt == (int)list.size() && i + 1 == list.size() && v.dragSlot + 1 != (int)list.size();
        sig += s.label + "|" + s.value + "|" + s.status + (s.on ? "1" : "0") + (s.open ? "o" : "") + (s.drop ? "d" : "")
               + (s.dropEnd ? "e" : "") + (s.dragging ? "g" : "");
        if (s.open)
        {
            const std::vector<PCF::Param> params = PCF::params(i);
            for (size_t p = 0; p < params.size(); ++p)
            {
                FParam fp;
                fp.id = llformat("fprow-%d-%d", (int)i, (int)p);
                fp.track = llformat("fptrack-%d-%d", (int)i, (int)p);
                fp.label = params[p].label;
                fp.value = params[p].value;
                fp.kind = params[p].kind;
                fp.on = params[p].on;
                fp.changed = params[p].changed;
                fp.w = llformat("%d%%", ll_round(params[p].fraction * 100.f));
                sig += "/" + fp.label + "=" + fp.value + fp.w + (fp.changed ? "c" : "");
                s.params.push_back(std::move(fp));
            }
        }
        sig += "\n";
        slots.push_back(std::move(s));
    }
    if (sig != v.signature || slots.size() != v.slots.size())
    {
        v.signature = sig;
        v.slots = std::move(slots);
        dirty("slots");
    }
    set(v.full, list.size() >= PCF::kMaxFilters, "full");
    set(v.compare, PCF::compare(), "compare");
    set(v.splitShown, PCF::compare() && anyOn, "splitShown");
}

// The picker's groups; only while it is open.
void refreshPicker()
{
    View& v = gView;
    if (!v.picking)
        return;
    static const char* const titles[] = { "GroupClassic", "GroupLut", "GroupEffects" };
    std::vector<FGroup> groups(3);
    for (size_t g = 0; g < 3; ++g)
        groups[g].title = str(titles[g]);
    std::vector<std::string> techniques;
    std::string sig;
    for (const PCF::Choice& c : PCF::choices())
    {
        FChoice fc;
        fc.id = llformat("fc-%d", (int)techniques.size());
        fc.label = c.label;
        fc.note = c.note;
        fc.added = c.added;
        techniques.push_back(c.technique);
        groups[(size_t)llclamp(c.group, 0, 2)].items.push_back(fc);
        sig += c.technique + (c.added ? "+" : "-") + c.label + "\n";
    }
    groups.erase(std::remove_if(groups.begin(), groups.end(), [](const FGroup& g) { return g.items.empty(); }), groups.end());
    if (sig != v.pickSignature)
    {
        v.pickSignature = sig;
        v.groups = std::move(groups);
        v.choiceTechniques = std::move(techniques);
        dirty("groups");
    }
    set(v.loading, PCF::loadingChoices(), "loading");
}

void refresh()
{
    refreshSlots();
    refreshPicker();
    gView.nextRefresh = LLTimer::getTotalSeconds() + 0.25;
}

void setPicking(bool on)
{
    set(gView.picking, on, "picking");
    if (on)
    {
        gView.pickSignature.clear();
        refreshPicker();
    }
}

// A slider follows the pointer across its track.
void dragTrackTo(S32 x)
{
    View& v = gView;
    const std::string id = v.drag == DRAG_STRENGTH ? llformat("fstr-%d", v.dragSlot) : llformat("fptrack-%d-%d", v.dragSlot, v.dragParam);
    Rml::Element* track = v.document ? v.document->GetElementById(id) : nullptr;
    if (!track || v.density <= 0.f)
        return;
    const float width = track->GetClientWidth();
    if (width <= 0.f)
        return;
    const F32 fraction = llclamp(((float)x * v.density - track->GetAbsoluteLeft()) / width, 0.f, 1.f);
    if (v.drag == DRAG_STRENGTH)
        PCF::setStrength((size_t)v.dragSlot, fraction);
    else
        PCF::setParamFraction((size_t)v.dragSlot, (size_t)v.dragParam, fraction);
}

// Where a dragged filter would land: before the slot whose middle is below the pointer.
void dragReorderTo(S32 y)
{
    View& v = gView;
    const float ry = (float)(v.rootH - y) * v.density;
    int at = (int)v.slots.size();
    for (size_t i = 0; i < v.slots.size(); ++i)
        if (Rml::Element* e = v.document->GetElementById(llformat("fslot-%d", (int)i)))
            if (ry < e->GetAbsoluteTop() + e->GetOffsetHeight() * 0.5f)
            {
                at = (int)i;
                break;
            }
    v.dropAt = at;
}

// The line's place, in dp from the window's left edge.
void placeSplit()
{
    View& v = gView;
    set(v.splitLeft, llformat("%.1fdp", PCF::split() * (float)v.rootW / llmax(v.k, 0.01f)), "splitLeft");
}

Rml::ElementDocument* loadDocument(Rml::Context* context)
{
    gView.theme = gSavedSettings.getU32("UIKitTheme") == 1 ? 1u : 0u;
    const LLNewUIKit::Tokens tokens = LLNewUIKit::tokens(gView.theme == 1 ? LLNewUIKit::Theme::Light : LLNewUIKit::Theme::Dark, true);
    return context->LoadDocumentFromMemory(LLNewUIKit::themed(kFiltersRml, tokens), "prestigecamerafilters.rml");
}

bool createDocument(Rml::Context* context)
{
    View& v = gView;
    v.context = context;
    try
    {
        Rml::DataModelConstructor m = context->CreateDataModel("camfilters");
        if (!m)
            return false;
        if (auto p = m.RegisterStruct<FParam>())
        {
            p.RegisterMember("id", &FParam::id);
            p.RegisterMember("track", &FParam::track);
            p.RegisterMember("label", &FParam::label);
            p.RegisterMember("value", &FParam::value);
            p.RegisterMember("w", &FParam::w);
            p.RegisterMember("kind", &FParam::kind);
            p.RegisterMember("on", &FParam::on);
            p.RegisterMember("changed", &FParam::changed);
        }
        m.RegisterArray<std::vector<FParam>>();
        if (auto s = m.RegisterStruct<FSlot>())
        {
            s.RegisterMember("id", &FSlot::id);
            s.RegisterMember("grip", &FSlot::grip);
            s.RegisterMember("row", &FSlot::row);
            s.RegisterMember("track", &FSlot::track);
            s.RegisterMember("label", &FSlot::label);
            s.RegisterMember("value", &FSlot::value);
            s.RegisterMember("w", &FSlot::w);
            s.RegisterMember("status", &FSlot::status);
            s.RegisterMember("on", &FSlot::on);
            s.RegisterMember("open", &FSlot::open);
            s.RegisterMember("drop", &FSlot::drop);
            s.RegisterMember("dropEnd", &FSlot::dropEnd);
            s.RegisterMember("dragging", &FSlot::dragging);
            s.RegisterMember("params", &FSlot::params);
        }
        m.RegisterArray<std::vector<FSlot>>();
        if (auto c = m.RegisterStruct<FChoice>())
        {
            c.RegisterMember("id", &FChoice::id);
            c.RegisterMember("label", &FChoice::label);
            c.RegisterMember("note", &FChoice::note);
            c.RegisterMember("added", &FChoice::added);
        }
        m.RegisterArray<std::vector<FChoice>>();
        if (auto g = m.RegisterStruct<FGroup>())
        {
            g.RegisterMember("title", &FGroup::title);
            g.RegisterMember("items", &FGroup::items);
        }
        m.RegisterArray<std::vector<FGroup>>();
        m.Bind("slots", &v.slots);
        m.Bind("groups", &v.groups);
        for (auto [name, field] : { std::make_pair("visible", &View::visible), std::make_pair("left", &View::left),
                                    std::make_pair("barTop", &View::barTop), std::make_pair("compare", &View::compare),
                                    std::make_pair("picking", &View::picking), std::make_pair("full", &View::full),
                                    std::make_pair("loading", &View::loading), std::make_pair("splitShown", &View::splitShown) })
            m.Bind(name, &(v.*field));
        for (auto [name, field] : { std::make_pair("title", &View::title), std::make_pair("emptyText", &View::emptyText),
                                    std::make_pair("noParamsText", &View::noParamsText), std::make_pair("strengthLabel", &View::strengthLabel),
                                    std::make_pair("compareLabel", &View::compareLabel), std::make_pair("addLabel", &View::addLabel),
                                    std::make_pair("lutLabel", &View::lutLabel), std::make_pair("pickTitle", &View::pickTitle),
                                    std::make_pair("pickLoading", &View::pickLoading), std::make_pair("beforeLabel", &View::beforeLabel),
                                    std::make_pair("afterLabel", &View::afterLabel), std::make_pair("splitLeft", &View::splitLeft) })
            m.Bind(name, &(v.*field));
        m.BindEventCallback("fclose", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { PrestigeCameraFiltersView::togglePanel(); });
        m.BindEventCallback("fon", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                const int i = args.empty() ? -1 : args[0].Get<int>();
                                if (i >= 0 && i < (int)gView.slots.size())
                                    PCF::setOn((size_t)i, !gView.slots[(size_t)i].on);
                                refresh();
                            });
        m.BindEventCallback("fexpand", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                const int i = args.empty() ? -1 : args[0].Get<int>();
                                const std::vector<PCF::Filter> list = PCF::filters();
                                if (i < 0 || i >= (int)list.size())
                                    return;
                                const std::string& t = list[(size_t)i].technique;
                                if (!gView.expanded.erase(t))
                                    gView.expanded.insert(t);
                                refresh();
                            });
        m.BindEventCallback("fremove", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args)
                            {
                                if (Rml::Element* e = event.GetCurrentElement())
                                    e->Blur();
                                const int i = args.empty() ? -1 : args[0].Get<int>();
                                if (i >= 0)
                                    PCF::remove((size_t)i);
                                refresh();
                            });
        m.BindEventCallback("pturn", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                int i = -1, p = -1;
                                if (args.size() >= 2 && twoIds(args[0].Get<Rml::String>(), i, p))
                                    PCF::turnParam((size_t)i, (size_t)p, args[1].Get<int>());
                                refresh();
                            });
        m.BindEventCallback("preset", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                int i = -1, p = -1;
                                if (!args.empty() && twoIds(args[0].Get<Rml::String>(), i, p))
                                    PCF::resetParam((size_t)i, (size_t)p);
                                refresh();
                            });
        m.BindEventCallback("fcompare", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                PCF::setCompare(!PCF::compare());
                                refresh();
                            });
        m.BindEventCallback("fpick", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                if (!gView.picking && PCF::full())
                                    PCF::add(std::string());   // says the stack is full
                                else
                                    setPicking(!gView.picking);
                            });
        m.BindEventCallback("fadd", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                int n = -1;
                                if (!args.empty() && oneId(args[0].Get<Rml::String>(), n) && n >= 0 && n < (int)gView.choiceTechniques.size()
                                    && PCF::add(gView.choiceTechniques[(size_t)n]))
                                {
                                    gView.expanded.insert(gView.choiceTechniques[(size_t)n]);
                                    setPicking(false);
                                }
                                refresh();
                            });
        m.BindEventCallback("flut", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                setPicking(false);
                                PCF::importLut();
                            });
        v.model = m.GetModelHandle();
    }
    catch (const std::bad_alloc&)
    {
        context->RemoveDataModel("camfilters");
        return false;
    }
    v.document = loadDocument(context);
    if (!v.document)
    {
        context->RemoveDataModel("camfilters");
        v.model = Rml::DataModelHandle();
        return false;
    }
    return true;
}

void retheme()
{
    View& v = gView;
    Rml::ElementDocument* fresh = loadDocument(v.context);
    if (!fresh)
        return;
    v.document->Close();
    v.document = fresh;
    v.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    v.document->PushToBack();   // the camera document's popovers stay on top
    if (v.model)
        v.model.DirtyAllVariables();
    markChanged();
}

void endDrag(S32 x, S32 y)
{
    View& v = gView;
    switch (v.drag)
    {
        case DRAG_STRENGTH:
        case DRAG_PARAM: dragTrackTo(x); break;
        case DRAG_SPLIT: PCF::setSplit((F32)x / (F32)llmax(v.rootW, 1)); break;
        case DRAG_REORDER:
        {
            dragReorderTo(y);
            if (v.dropAt >= 0 && v.dragSlot >= 0)
                PCF::move((size_t)v.dragSlot, (size_t)(v.dropAt > v.dragSlot ? v.dropAt - 1 : v.dropAt));
            break;
        }
        default: break;
    }
    v.drag = DRAG_NONE;
    v.dragSlot = v.dragParam = v.dropAt = -1;
    refresh();
}
} // namespace

namespace PrestigeCameraFiltersView
{
void start(Rml::Context* context)
{
    if (!context)
        return;
    if (gView.context != context)
        release();
    if (!gView.document && !createDocument(context))
    {
        gView = View();
        return;
    }
    View& v = gView;
    v.title = str("Title");
    v.emptyText = str("Empty");
    v.noParamsText = str("NoSliders");
    v.strengthLabel = str("Strength");
    v.compareLabel = str("Compare");
    v.addLabel = str("Add");
    v.lutLabel = str("ImportLut");
    v.pickTitle = str("PickTitle");
    v.pickLoading = str("PickLoading");
    v.beforeLabel = str("Before");
    v.afterLabel = str("After");
    v.picking = false;
    v.drag = DRAG_NONE;
    v.pressedInDoc = false;
    v.lastX = v.lastY = S32_MIN;
    v.signature.clear();
    v.pickSignature.clear();
    v.rootW = v.rootH = 0;
    if (v.model)
        v.model.DirtyAllVariables();
    v.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    v.document->PushToBack();   // behind the camera document: its popovers and bar stay on top
    v.shown = true;
    v.changed = true;
}

void stop()
{
    View& v = gView;
    if (v.document && v.shown)
        v.document->Hide();
    v.shown = false;
    v.picking = false;
    v.drag = DRAG_NONE;
    v.pressedInDoc = false;
}

void release()
{
    gView = View();
}

bool sync(float density)
{
    View& v = gView;
    if (!v.shown || !v.document)
        return false;
    if (density != v.density)
    {
        v.density = density;
        markChanged();
    }
    static LLCachedControl<U32> s_theme(gSavedSettings, "UIKitTheme", 0);
    if ((s_theme == 1 ? 1u : 0u) != v.theme)
        retheme();
    if (LLView* root = LLUI::getInstance()->getRootView())
    {
        const LLRect& r = root->getRect();
        const float k = SLCameraView::dpScale();
        if (r.getWidth() != v.rootW || r.getHeight() != v.rootH || k != v.k)
        {
            v.rootW = r.getWidth();
            v.rootH = r.getHeight();
            v.k = k;
                    markChanged();
        }
    }
    const bool controls = gSavedSettings.getBOOL("SLCameraModeControlsOpen");
    const bool barHidden = gSavedSettings.getBOOL("SLCameraModeBarHidden");
    set(v.visible, openSetting() && controls && !barHidden, "visible");
    set(v.left, panelLeft(), "left");
    set(v.barTop, (bool)gSavedSettings.getBOOL("SLCameraModeBarTop"), "barTop");
    if (!v.visible && v.picking)
        setPicking(false);
    if (gViewerWindow)
    {
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        toRml(mouse.mX, mouse.mY, MASK_NONE);
        switch (v.drag)
        {
            case DRAG_STRENGTH:
            case DRAG_PARAM:
                dragTrackTo(mouse.mX);
                refreshSlots();   // a slider being dragged follows at once
                break;
            case DRAG_SPLIT: PCF::setSplit((F32)mouse.mX / (F32)llmax(v.rootW, 1)); break;
            case DRAG_REORDER:
            {
                const int before = v.dropAt;
                dragReorderTo(mouse.mY);
                if (v.dropAt != before)
                    refreshSlots();
                break;
            }
            default: break;
        }
    }
    if (LLTimer::getTotalSeconds() >= v.nextRefresh)
        refresh();
    placeSplit();
    const bool changed = v.changed;
    v.changed = false;
    return changed;
}

bool handleMouse(S32 x, S32 y, MASK mask, EMouseClickType click, bool down)
{
    View& v = gView;
    if (!v.shown || (click != CLICK_LEFT && click != CLICK_DOUBLELEFT))
        return false;
    const bool over = toRml(x, y, mask);
    Rml::Element* hover = v.context->GetHoverElement();
    if (down)
    {
        if (!over)
        {
            if (v.picking && !within(hover, "fpanel"))
            {
                setPicking(false);   // a click outside closes the picker and goes no further
                return true;
            }
            return false;
        }
        v.pressedInDoc = true;
        int a = -1, b = -1;
        std::string id;
        if (within(hover, "fsplithit"))
        {
            v.drag = DRAG_SPLIT;
            if (click == CLICK_DOUBLELEFT)
                PCF::setSplit(0.5f);   // double-click: back to the middle
            return true;
        }
        if (!(id = idWithin(hover, "fstr-")).empty() && oneId(id, a))
        {
            v.drag = DRAG_STRENGTH;
            v.dragSlot = a;
            dragTrackTo(x);   // a press on the track sets it there; a drag follows
            refreshSlots();
            return true;
        }
        if (!(id = idWithin(hover, "fptrack-")).empty() && twoIds(id, a, b))
        {
            v.drag = DRAG_PARAM;
            v.dragSlot = a;
            v.dragParam = b;
            dragTrackTo(x);
            refreshSlots();
            return true;
        }
        if (!(id = idWithin(hover, "fgrip-")).empty() && oneId(id, a))
        {
            v.drag = DRAG_REORDER;
            v.dragSlot = a;
            v.dropAt = a;
            refreshSlots();
            return true;
        }
        if (v.picking && !within(hover, "fpick") && !within(hover, "ffoot"))
            setPicking(false);
        int m = 0;
        if (mask & MASK_CONTROL) m |= Rml::Input::KM_CTRL;
        if (mask & MASK_SHIFT) m |= Rml::Input::KM_SHIFT;
        v.context->ProcessMouseButtonDown(0, m);
        markChanged();
        return true;
    }
    if (!v.pressedInDoc)
        return false;
    v.pressedInDoc = false;
    if (v.drag != DRAG_NONE)
    {
        endDrag(x, y);
        return true;
    }
    v.context->ProcessMouseButtonUp(0, 0);
    markChanged();
    return true;
}

bool handleWheel(S32 x, S32 y, S32 clicks, MASK mask)
{
    View& v = gView;
    if (!toRml(x, y, mask))
        return false;
    Rml::Element* hover = v.context->GetHoverElement();
    int a = -1, b = -1;
    std::string id;
    // A slider row under the pointer turns (wheel up: more), as on the Photo Studio rail.
    if (!(id = idWithin(hover, "fprow-")).empty() && twoIds(id, a, b))
    {
        PCF::turnParam((size_t)a, (size_t)b, clicks < 0 ? 1 : -1);
        refresh();
        return true;
    }
    if (!(id = idWithin(hover, "fsrow-")).empty() && oneId(id, a))
    {
        const std::vector<PCF::Filter> list = PCF::filters();
        if (a >= 0 && a < (int)list.size())
            PCF::setStrength((size_t)a, list[(size_t)a].strength + (clicks < 0 ? 0.05f : -0.05f));
        refresh();
        return true;
    }
    if (within(hover, "fsplithit"))
        return true;
    v.context->ProcessMouseWheel(float(clicks), 0);   // the lists scroll
    markChanged();
    return true;
}

bool over(S32 x, S32 y)
{
    return toRml(x, y, MASK_NONE);
}

bool panelOpen()
{
    return openSetting();
}

void togglePanel()
{
    gSavedSettings.setBOOL("PrestigeCameraFiltersOpen", !openSetting());
    if (!openSetting())
        setPicking(false);
    refresh();
}

bool popupOpen()
{
    return gView.shown && gView.visible && gView.picking;
}

bool closePopup()
{
    if (!popupOpen())
        return false;
    setPicking(false);
    return true;
}
} // namespace PrestigeCameraFiltersView
