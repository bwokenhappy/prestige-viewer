// #703 / #1242 The new interface (see llnewui.h).
#include "llviewerprecompiledheaders.h"
#include "llnewui.h"
#include "pipeline.h"

#include "llagent.h"
#include "llnavigationbar.h"
#include "llteleporthistory.h"
#include "lllocationhistory.h"
#include "llinventorymodel.h"
#include "llinventoryfunctions.h"
#include "llfavoritesbar.h"
#include "lllandmarkactions.h"
#include "llslurl.h"
#include "rlvactions.h"
#include "llcommandindex.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "llnewuidocs.h"
#include "llnewuikit.h"
#include "llnewuistatus.h"
#include "llnewuimenus.h"
#include "llnewuiwindow.h"
#include "llnewuitools.h"
#include "llrender.h"
#include "llrmluihost.h"   // slRmlUiCursorType
#include "lltoolbarview.h"
#include "lltooltip.h"
#include "llui.h"
#include "llwindow.h"     // the text input and the cursor of the window
#include "llview.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "slcameramode.h"
#include "slkeycommands.h"
#include "vkgfx/vk_ui_texture_host.h" // slUIMarkExternalLayer

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <new>
#include <string>
#include <vector>

namespace
{
struct ResultRow
{
    std::string kind, label, path, shortcut;
    bool        check = false, on = false, off = false; // a toggle, checked, not available now
};

constexpr size_t kMaxResults = 12;
// The bar (llnewuidocs.h): a floating bar this tall, this far from the window's edge; the tool
// grid keeps this distance from the bottom edge (or from the bar there).
constexpr int kBarDp = 32, kBarGapDp = 4, kToolsGapDp = 12;

class InputView;

struct State
{
    Rml::Context*         context = nullptr;
    Rml::ElementDocument* bar     = nullptr;
    Rml::ElementDocument* palette = nullptr;
    Rml::DataModelHandle  barModel, paletteModel;
    InputView*            input = nullptr;
    LLHandle<LLView>      navBar;
    LLCommandIndex        index;
    // bar model
    std::string            location, hint, key;
    bool navOpen = false, navFocusWanted = false;
    int navMode = 0, navSelected = -1;
    std::string address, navPrevious;
    bool navPolicy = true;
    std::vector<int> navEnabled;
    std::vector<std::string> navLabels;
    std::vector<std::function<void()>> navActions;
    U32                    theme = 0; // UIKitTheme the documents were loaded with
    // palette model
    bool                         open = false;
    std::string                  query, lastQuery = "\x01";
    std::vector<ResultRow>       rows;
    std::vector<LLCommandIndex::Entry> entries; // what each row runs (copies: safe across rebuilds)
    int                          selected = 0;
    // state
    float density = 1.f;
    bool  glass = true, glassApplied = false, atBottom = false, placed = false, changed = true, inputSeen = false;
    F64   nextRefresh = 0;
    S32   barTop = -1, barBottom = -1;
    U32   markedFrame = 0;
    size_t indexedSize = 0; // index size the open surface's results were computed from
    F64    nextRequery = 0;
};
State* gState = nullptr;

void markChanged()
{
    if (gState)
        gState->changed = true;
}

int rmlModifiers(MASK mask)
{
    int m = 0;
    if (mask & MASK_CONTROL)
        m |= Rml::Input::KM_CTRL;
    if (mask & MASK_SHIFT)
        m |= Rml::Input::KM_SHIFT;
    if (mask & MASK_ALT)
        m |= Rml::Input::KM_ALT;
    return m;
}

void runRow(size_t i);
void closePalette();

// The RmlUi layer's z slot: above the main view (world UI, chrome, floaters), below the menus,
// pop-ups and tooltips (the root view draws those after the main view).
void markLayer(LLView* transformView = nullptr)
{
    State* s = gState;
    const U32 frame = LLFrameTimer::getFrameCount();
    if (!s || s->markedFrame == frame)
        return;
    s->markedFrame = frame;
    gGL.flush();
    slUIMarkExternalLayer();
    // Classic live content follows the native document, clipped by its RML aperture
    // and higher windows. No render target, readback, or second map implementation.
    if (gState && gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI)
        && !gState->open && !gState->navOpen && LLNewUIMenus::instance().openPage() == LLNewUIMenus::Page::None
        && !(gMenuHolder && gMenuHolder->hasVisibleMenu()))
    {
        LLUI::pushMatrix();
        if (transformView)
        {
            const LLRect screen=transformView->calcScreenRect();
            LLUI::translate(-F32(screen.mLeft),-F32(screen.mBottom));
        }
        LLNewUIWindows::instance().drawClassicOverlays();
        LLUI::popMatrix();
    }
}
bool presentHook(LLView* v)
{
    if (gMenuHolder && v == gMenuHolder)
        markLayer(v);
    return false; // the new interface never takes over an LLView
}

// Frontmost view: mouse events reach RmlUi first and are kept only over its elements; while the
// command surface is open it holds the keyboard focus (and a click outside closes it). The native
// windows (llnewuiwindow.h): their frame's presses (move, resize, close, minimise) go to the
// window framework, the rest to RmlUi; while one is dragged this view holds the mouse.
void refreshNavigation();
void submitAddress();
void closeNavigation();
bool navigationKey(KEY key);
void runNavigationChoice(size_t index);

class InputView final : public LLView
{
public:
    InputView(const LLView::Params& p) : LLView(p) {}

    S32           lastX = S32_MIN, lastY = S32_MIN;
    Rml::Element* lastHover = nullptr;

    static LLNewUIWindows& windows() { return LLNewUIWindows::instance(); }

    void moveTo(S32 x, S32 y, MASK mask)
    {
        State* s = gState;
        // LLViewerWindow sends a hover every frame even with a resting mouse: only a real move
        // reaches RmlUi, and only a change of the hovered element is a change to capture.
        if (x != lastX || y != lastY)
        {
            lastX = x;
            lastY = y;
            const S32 rootH = LLUI::getInstance()->getRootView()->getRect().getHeight();
            s->context->ProcessMouseMove(int(x * s->density), int((rootH - y) * s->density), rmlModifiers(mask));
        }
        Rml::Element* hover = s->context->GetHoverElement();
        if (hover != lastHover)
        {
            lastHover = hover;
            markChanged(); // :hover styles
        }
    }
    bool toRml(S32 x, S32 y, MASK mask)
    {
        State* s = gState;
        if (!s || !s->context)
            return false;
        // An open classic menu (the menu bar's, a context menu) draws above the RmlUi layer: it
        // has the pointer, even over a window (and closes on a click outside, as always).
        if (gMenuHolder && gMenuHolder->hasVisibleMenu())
            return false;
        moveTo(x, y, mask);
        Rml::Element* hover = s->context->GetHoverElement();
        if (!hover)
            return false;
        Rml::ElementDocument* doc = hover->GetOwnerDocument();
        // A window is its document (its body is the panel): anywhere over it counts.
        if (windows().ownsDocument(doc))
            return true;
        return hover != doc && (doc == s->bar || doc == s->palette || LLNewUITools::instance().ownsDocument(doc) ||
                                LLNewUIStatus::instance().ownsDocument(doc));
    }
    // Over the tool grid (it has no values to turn: the wheel goes on to the world).
    bool overTools() const
    {
        Rml::Element* hover = gState && gState->context ? gState->context->GetHoverElement() : nullptr;
        return hover && LLNewUITools::instance().ownsDocument(hover->GetOwnerDocument());
    }
    bool handleHover(S32 x, S32 y, MASK mask) override
    {
        if (gState && windows().dragging())
        {
            moveTo(x, y, mask);
            windows().mouseMove(x, y);
            markChanged();
            return true;
        }
        const bool over = toRml(x, y, mask);
        if (over)
            getWindow()->setCursor(slRmlUiCursorType()); // the cursor its RCSS asks for (I-beam over a field)
        return over || (gState && gState->open);
    }
    bool handleToolTip(S32 x,S32 y,MASK mask) override
    {
        if (!toRml(x,y,mask)) return false;
        auto* hover=gState->context->GetHoverElement();
        if (windows().overlayToolTip(hover,mask)) return true;
        for (auto* e=hover;e;e=e->GetParentNode())
        {
            const auto text=e->GetAttribute<Rml::String>("title","");
            if (!text.empty()) { LLToolTipMgr::instance().show(text); return true; }
        }
        return false;
    }
    bool press(S32 x, S32 y, MASK mask, bool doubleClick)
    {
        if (!gState)
            return false;
        if (toRml(x, y, mask))
        {
            Rml::Element* hover = gState->context->GetHoverElement();
            // A window's frame (title bar, grip, buttons) is the framework's; it raises and
            // activates the window either way.
            const bool framework = windows().windowOf(hover) && windows().mouseDown(hover, x, y, mask, doubleClick);
            if (!windows().windowOf(hover))
                windows().deactivate();
            if (!framework)
                gState->context->ProcessMouseButtonDown(0, rmlModifiers(mask));
            // A text field of the new interface takes the keyboard (chat line, searches).
            if (fieldFocused())
                gFocusMgr.setKeyboardFocus(this);
            else if (windows().windowOf(hover) && !gState->open)
                gFocusMgr.setKeyboardFocus(nullptr); // a window's control, not a field: keys go to the active window
            // A tool held down (push to talk) or a window dragged: the release comes back here
            // wherever it happens.
            if (LLNewUITools::instance().holding() || windows().dragging())
                gFocusMgr.setMouseCapture(this);
            markChanged();
            return true;
        }
        windows().deactivate(); // the world or a classic floater
        if (gState->open)
        {
            closePalette(); // a click outside the surface closes it and is not passed on
            return true;
        }
        return false;
    }
    bool handleMouseDown(S32 x, S32 y, MASK mask) override { return press(x, y, mask, false); }
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override
    {
        // RmlUi makes the double click from two quick presses; the second comes here.
        if (!gState || !toRml(x, y, mask))
            return false;
        press(x, y, mask, true);
        gState->context->ProcessMouseButtonUp(0, rmlModifiers(mask));
        return true;
    }
    bool handleMouseUp(S32 x, S32 y, MASK mask) override
    {
        if (!gState)
            return false;
        const bool dragged = windows().dragging();
        if (dragged)
            windows().mouseUp(x, y);
        const bool over = toRml(x, y, mask);
        gState->context->ProcessMouseButtonUp(0, rmlModifiers(mask));
        const bool held = LLNewUITools::instance().holding();
        LLNewUITools::instance().releaseHeld(); // any release lets a held tool go
        if (hasMouseCapture())
            gFocusMgr.setMouseCapture(nullptr);
        markChanged();
        return over || held || dragged;
    }
    // Right button: a window's context menus (RmlUi button 1); elsewhere it goes on.
    bool handleRightMouseDown(S32 x, S32 y, MASK mask) override
    {
        if (!gState || !toRml(x, y, mask) || !windows().windowOf(gState->context->GetHoverElement()))
            return false;
        windows().focus(gState->context->GetHoverElement()); // raise / activate only: never a move or a drag
        gState->context->ProcessMouseButtonDown(1, rmlModifiers(mask));
        markChanged();
        return true;
    }
    bool handleRightMouseUp(S32 x, S32 y, MASK mask) override
    {
        if (!gState || !toRml(x, y, mask) || !windows().windowOf(gState->context->GetHoverElement()))
            return false;
        gState->context->ProcessMouseButtonUp(1, rmlModifiers(mask));
        markChanged();
        return true;
    }
    // An item dragged with the classic drag tool (from a native inventory window, or a classic
    // one) over the new interface: nothing here takes drops yet, and the drop must not fall
    // through to the world behind a window (it would rez or give the item).
    bool handleDragAndDrop(S32 x, S32 y, MASK mask, bool drop, EDragAndDropType cargo_type, void* cargo_data, EAcceptance* accept,
                           std::string& tooltip_msg) override
    {
        if (!gState || !toRml(x, y, mask))
            return false;
        *accept = ACCEPT_NO;
        return true;
    }
    void onMouseCaptureLost() override
    {
        LLNewUITools::instance().releaseHeld();
        if (windows().dragging())
            windows().mouseUp(lastX, lastY); // an item dragged out: the drag tool took the mouse
        LLView::onMouseCaptureLost();
    }
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override
    {
        if (!gState || !toRml(x, y, 0) || overTools())
            return false;
        gState->context->ProcessMouseWheel(float(clicks), 0);
        markChanged();
        return true;
    }
    // A text field of the bar, palette or a window has the RmlUi focus.
    Rml::Element* focusedField() const
    {
        if (!gState || !gState->context)
            return nullptr;
        Rml::Element* focus = gState->context->GetFocusElement();
        if (!focus || (focus->GetTagName() != "input" && focus->GetTagName() != "textarea"))
            return nullptr;
        return focus;
    }
    bool fieldFocused() const { return focusedField() != nullptr; }
    // A window's text field has the RmlUi focus.
    Rml::Element* windowField() const
    {
        Rml::Element* f = focusedField();
        return f && (windows().windowOf(f) || (gState && f->GetOwnerDocument() == gState->bar)) ? f : nullptr;
    }
    bool handleKeyHere(KEY key, MASK mask) override
    {
        State* s = gState;
        if (s && s->navOpen && navigationKey(key)) return true;
        if (s && !s->open)
            if (Rml::Element* field = windowField())
                return fieldKey(field, key, mask);
        if (!s || !s->open)
            return false;
        switch (key)
        {
            case KEY_ESCAPE:
                closePalette();
                return true;
            case KEY_UP:
                if (!s->rows.empty())
                    s->selected = (s->selected + int(s->rows.size()) - 1) % int(s->rows.size());
                s->paletteModel.DirtyVariable("selected");
                markChanged();
                return true;
            case KEY_DOWN:
                if (!s->rows.empty())
                    s->selected = (s->selected + 1) % int(s->rows.size());
                s->paletteModel.DirtyVariable("selected");
                markChanged();
                return true;
            case KEY_RETURN:
                runRow(size_t(s->selected));
                return true;
            default:
                break;
        }
        forwardKey(key, mask);
        markChanged();
        return true; // the surface is modal: keys do not reach the world
    }
    // A key while a window's text field types: the window's own first (Enter sends, Tab
    // completes, Up brings back a line ...), then the field's editing keys. Every key is the
    // field's while it has the keyboard, as in a classic field.
    bool fieldKey(Rml::Element* field, KEY key, MASK mask)
    {
        if (gState && field && field->GetId() == "address")
        {
            if (navigationKey(key)) return true;
            if (key == KEY_TAB) { gState->context->ProcessKeyDown(Rml::Input::KI_TAB,rmlModifiers(mask)); return true; }
            forwardKey(key, mask); markChanged(); return true;
        }
        if (windows().handleKey(key, mask, field))
        {
            markChanged();
            return true;
        }
        if (key == KEY_ESCAPE)
        {
            field->Blur(); // done typing; Esc again closes the window
            gFocusMgr.setKeyboardFocus(nullptr);
        }
        else if (key == KEY_RETURN)
            windows().handleReturn(field, mask);
        else if (key == KEY_TAB && !(mask & (MASK_CONTROL | MASK_ALT)))
            gState->context->ProcessKeyDown(Rml::Input::KI_TAB, rmlModifiers(mask));
        else
            forwardKey(key, mask);
        markChanged();
        return true;
    }
    void forwardKey(KEY key, MASK mask)
    {
        State* s = gState;
        Rml::Input::KeyIdentifier ki = Rml::Input::KI_UNKNOWN;
        switch (key)
        {
            case KEY_BACKSPACE: ki = Rml::Input::KI_BACK; break;
            case KEY_DELETE: ki = Rml::Input::KI_DELETE; break;
            case KEY_LEFT: ki = Rml::Input::KI_LEFT; break;
            case KEY_RIGHT: ki = Rml::Input::KI_RIGHT; break;
            case KEY_HOME: ki = Rml::Input::KI_HOME; break;
            case KEY_END: ki = Rml::Input::KI_END; break;
            default: break;
        }
        // The standard editing keys: select all, copy, cut, paste (the viewer's clipboard, llrmluihost.cpp).
        static const std::pair<KEY, Rml::Input::KeyIdentifier> kEditKeys[] = {
            { 'A', Rml::Input::KI_A }, { 'C', Rml::Input::KI_C }, { 'X', Rml::Input::KI_X }, { 'V', Rml::Input::KI_V } };
        for (const auto& [letter, id] : kEditKeys)
            if (ki == Rml::Input::KI_UNKNOWN && (mask & MASK_CONTROL) && key == letter)
                ki = id;
        if (ki != Rml::Input::KI_UNKNOWN)
            s->context->ProcessKeyDown(ki, rmlModifiers(mask));
    }
    bool handleUnicodeCharHere(llwchar uni_char) override
    {
        State* s = gState;
        if (!s || (!s->open && !windowField()) || uni_char < 0x20)
            return false;
        s->context->ProcessTextInput(static_cast<Rml::Character>(uni_char));
        markChanged();
        return true;
    }
    // While it holds the keyboard (a field is typed in), the window's text input is on, as for a
    // classic field: AltGr, dead keys and input methods type their characters (without it the
    // window passes the bare key: AltGr+2 typed "2").
    void onFocusReceived() override
    {
        getWindow()->allowLanguageTextInput(nullptr, true);
        LLView::onFocusReceived();
    }
    void onFocusLost() override
    {
        getWindow()->allowLanguageTextInput(nullptr, false);
        if (Rml::Element* field = windowField())
            field->Blur();
        if (gState && gState->open)
            closePalette();
        LLView::onFocusLost();
    }
    void draw() override {} // nothing of its own: RmlUi draws the documents
};

std::string themed(const char* rml, const LLNewUIKit::Tokens& tokens)
{
    // Every document carries the kit stylesheet; its own <style> is layout only and may use
    // $tokens too.
    return LLNewUIKit::themed(rml, tokens);
}

// The kit's theme (UIKitTheme: 0 dark, 1 light), glass surfaces as camera mode uses them.
LLNewUIKit::Tokens currentTokens(U32& theme)
{
    theme = gSavedSettings.getU32("UIKitTheme") == 1 ? 1u : 0u;
    return LLNewUIKit::tokens(theme == 1 ? LLNewUIKit::Theme::Light : LLNewUIKit::Theme::Dark, true);
}

LLNewUITools::DocumentLoader loaderFor(Rml::Context* context, const LLNewUIKit::Tokens& tokens)
{
    return [tokens, context](const char* rml, const char* name) { return context->LoadDocumentFromMemory(themed(rml, tokens), name); };
}

void requery(State* s)
{
    s->lastQuery = s->query;
    std::vector<const LLCommandIndex::Entry*> found;
    s->index.query(s->query, kMaxResults - 1, found);
    s->rows.clear();
    s->entries.clear();
    auto add = [s](const LLCommandIndex::Entry& e)
    {
        const char* kind = !e.tag.empty()                            ? e.tag.c_str()
                           : e.kind == LLCommandIndex::Kind::Menu    ? "Menu"
                           : e.kind == LLCommandIndex::Kind::Command ? "Interface"
                           : e.kind == LLCommandIndex::Kind::Window  ? "Window"
                           : e.kind == LLCommandIndex::Kind::Section ? "Panel"
                           : e.kind == LLCommandIndex::Kind::Setting ? "Setting"
                                                                     : "Search";
        s->rows.push_back({ kind, e.label, e.path, e.shortcut, e.checkable, e.checked, !e.enabled });
        s->entries.push_back(e);
    };
    // The native windows while they are on (UINewWindowsDev): first, when their name matches.
    if (LLNewUIWindows::instance().active())
        for (const auto& [kind, label] : { std::pair<const char*, const char*>{ "chat", "Conversations (new)" },
                                           { "people", "People (new)" }, { "inventory", "Inventory (new)" } })
        {
            std::string folded = label, q = s->query;
            LLStringUtil::toLower(folded);
            LLStringUtil::toLower(q);
            // (an empty query lists the index's own top entries)
            if (q.empty() || (folded.find(q) == std::string::npos && std::string(kind).find(q) == std::string::npos))
                continue;
            LLCommandIndex::Entry e;
            e.kind   = LLCommandIndex::Kind::Window;
            e.id     = std::string("newui:") + kind;
            e.label  = label;
            e.path   = "New interface window";
            e.target = kind;
            add(e);
        }
    for (const auto* e : found)
        add(*e);
    if (!s->query.empty())
        add(LLCommandIndex::settingsSearch(s->query));
    s->selected = 0;
    s->paletteModel.DirtyVariable("rows");
    s->paletteModel.DirtyVariable("selected");
    markChanged();
}

void openPalette()
{
    State* s = gState;
    if (!s || !s->palette || s->open)
        return;
    s->open = true;
    s->index.rebuild(); // fixed cost per open, not per frame
    s->query.clear();
    s->paletteModel.DirtyVariable("query");
    requery(s);
    s->palette->Show();
    s->palette->PullToFront(); // above the native windows
    if (Rml::Element* q = s->palette->GetElementById("query"))
        q->Focus();
    LL_INFOS("RmlUi") << "New interface: command surface opened (" << s->index.size() << " commands)" << LL_ENDL;
    if (s->input)
        gFocusMgr.setKeyboardFocus(s->input);
    markChanged();
}

void closePalette()
{
    State* s = gState;
    if (!s || !s->open)
        return;
    s->open = false;
    if (s->palette)
        s->palette->Hide();
    LL_INFOS("RmlUi") << "New interface: command surface closed" << LL_ENDL;
    if (s->input && gFocusMgr.getKeyboardFocus() == s->input)
        gFocusMgr.setKeyboardFocus(nullptr);
    markChanged();
}

void runRow(size_t i)
{
    State* s = gState;
    if (!s || i >= s->entries.size())
        return;
    const LLCommandIndex::Entry entry = s->entries[i]; // closing may change the lists
    closePalette();
    if (entry.id.rfind("newui:", 0) == 0)
    {
        LLNewUIWindows::instance().toggle(entry.target); // a native window
        return;
    }
    if (!LLCommandIndex::run(entry))
        LL_WARNS("RmlUi") << "New interface: command not available now: " << entry.label << " (" << entry.path << ")" << LL_ENDL;
}

std::string currentLocation()
{
    if (!RlvActions::canShowLocation()) return "Location hidden";
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
        return "Not in a region";
    const std::string& parcel = LLViewerParcelMgr::getInstance()->getAgentParcelName();
    return parcel.empty() ? region->getName() : parcel + " - " + region->getName();
}

// The classic chrome the new bar replaces (restored by stop()).
void hideClassicChrome(State* s, bool hide)
{
    if (gToolBarView && gToolBarView->getVisible() == hide)
        gToolBarView->setVisible(!hide);
    if (LLView* nav = s->navBar.get())
    {
        if (hide && nav->getVisible())
            nav->setVisible(false);
        else if (!hide)
            nav->setVisible(gSavedSettings.getBOOL("ShowNavbarNavigationPanel"));
    }
}
// The documents again under the current theme's tokens (the bar, the command surface, the tool
// grid, the windows). A document that fails to load keeps its old
// look rather than leave the interface without it.
void retheme(State* s)
{
    const LLNewUIKit::Tokens tokens = currentTokens(s->theme);
    const auto               load   = loaderFor(s->context, tokens);
    for (Rml::ElementDocument** doc : { &s->bar, &s->palette })
    {
        Rml::ElementDocument* fresh = load(doc == &s->bar ? LLNewUIDocs::kBarRml : LLNewUIDocs::kPaletteRml,
                                           doc == &s->bar ? "newui-bar.rml" : "newui-palette.rml");
        if (!fresh)
            continue;
        const bool shown = (*doc)->IsVisible();
        (*doc)->Close();
        *doc = fresh;
        if (shown)
            (*doc)->Show(Rml::ModalFlag::None, doc == &s->palette ? Rml::FocusFlag::Auto : Rml::FocusFlag::None);
    }
    if (s->open)
        if (Rml::Element* q = s->palette->GetElementById("query"))
            q->Focus();
    LLNewUITools::instance().reload(load);
    LLNewUIWindows::instance().reload(load);
    LLNewUIStatus::instance().reload(load);
    if (s->open)
        s->palette->PullToFront(); // the command surface stays above the windows
    s->barTop  = -1;
    s->placed  = false;
    s->barModel.DirtyAllVariables();
    s->paletteModel.DirtyAllVariables();
    if (s->input)
        s->input->lastHover = nullptr;
    LL_INFOS("RmlUi") << "New interface: theme " << s->theme << LL_ENDL;
    markChanged();
}
void closeNavigation()
{
    if (!gState) return;
    gState->navOpen = false; gState->navFocusWanted=false;
    gState->barModel.DirtyVariable("navOpen");
    if (gState->bar) if (auto* input = gState->bar->GetElementById("address")) input->Blur();
    if (gFocusMgr.getKeyboardFocus()==gState->input) gFocusMgr.setKeyboardFocus(nullptr);
    markChanged();
}
void runNavigationChoice(size_t index)
{
    if (!gState || index>=gState->navActions.size() || !gState->navEnabled[index]) return;
    auto run=gState->navActions[index]; closeNavigation(); run();
}
bool navigationKey(KEY key)
{
    State* s=gState;
    if (!s || !s->navOpen) return false;
    if (key==KEY_ESCAPE) { closeNavigation(); return true; }
    if (key==KEY_RETURN)
    {
        if (s->navSelected>=0) runNavigationChoice(size_t(s->navSelected));
        else if (s->navMode==1) submitAddress();
        return true;
    }
    if (key!=KEY_UP && key!=KEY_DOWN) return false;
    const int count=int(s->navActions.size());
    if (!count) return true;
    int index=s->navSelected;
    for (int n=0;n<count;++n)
    {
        index=key==KEY_DOWN ? (index+1)%count : (index<=0 ? count-1 : index-1);
        if (s->navEnabled[size_t(index)]) { s->navSelected=index; break; }
    }
    s->barModel.DirtyVariable("navSelected"); markChanged(); return true;
}
void submitAddress()
{
    if (!gState) return;
    std::string address = gState->address;
    LLStringUtil::trim(address);
    if (address.empty()) return;
    // Coordinates alone use the current region; the classic parser handles everything else.
    float x, y, z; char tail;
    if (std::sscanf(address.c_str(), "%f/%f/%f %c", &x, &y, &z, &tail) == 3
        && std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && gAgent.getRegion())
        address = LLSLURL(gAgent.getRegion()->getName(), LLVector3(x,y,z)).getSLURLString();
    LLNavigationBar::getInstance()->navigateTo(address);
    closeNavigation();
}
void refreshNavigation()
{
    State* s = gState;
    if (!s) return;
    s->navPrevious = s->address;
    s->navPolicy=RlvActions::canShowLocation();
    s->navLabels.clear(); s->navActions.clear(); s->navEnabled.clear();
    std::string query = s->navMode == 1 ? s->address : std::string(); LLStringUtil::toLower(query);
    const auto add = [&](const std::string& label, std::function<void()> run, bool enabled=true)
    {
        std::string folded = label; LLStringUtil::toLower(folded);
        if (s->navLabels.size() < 48 && (query.empty() || folded.find(query) != std::string::npos))
        { s->navLabels.push_back(label); s->navActions.push_back(std::move(run)); s->navEnabled.push_back(enabled?1:0); }
    };
    if (s->navMode == 1)
    {
        if (RlvActions::canShowLocation())
        {
            const auto& history = LLTeleportHistory::instance().getItems();
            for (auto it = history.rbegin(); it != history.rend(); ++it)
            { const LLVector3d position = it->mGlobalPos;
              add(it->mFullTitle.empty() ? it->mTitle : it->mFullTitle, [position] { gAgent.teleportViaLocation(position); }); }
            const auto& typed = LLLocationHistory::instance().getItems();
            for (auto it = typed.rbegin(); it != typed.rend(); ++it)
            { const auto address = it->getLocation(); add(address, [address] { LLNavigationBar::getInstance()->navigateTo(address); }); }
        }
    }
    else if (s->navMode == 2 && s->navPolicy)
    {
        add("Add current location to favourites...", []
            { if (RlvActions::canShowLocation()) LLFloaterReg::showInstance("add_landmark",
                 LLSD().with("dest_folder", gInventory.findCategoryUUIDForType(LLFolderType::FT_FAVORITE))); });
        LLInventoryModel::cat_array_t cats; LLInventoryModel::item_array_t items;
        LLIsType landmark(LLAssetType::AT_LANDMARK);
        const LLUUID folder = gInventory.findCategoryUUIDForType(LLFolderType::FT_FAVORITE);
        if (folder.notNull()) gInventory.collectDescendentsIf(folder,cats,items,LLInventoryModel::EXCLUDE_TRASH,landmark);
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b)
            { return LLFavoritesOrderStorage::instance().getSortIndex(a->getUUID()) < LLFavoritesOrderStorage::instance().getSortIndex(b->getUUID()); });
        for (const auto& item : items)
        { const LLUUID asset = item->getAssetUUID(); add(item->getName(), [asset] { gAgent.teleportViaLandmark(asset); }); }
    }
    else if (s->navMode == 3)
    {
        s->index.rebuild();
        std::vector<const LLCommandIndex::Entry*> matches;
        s->index.query("upload", 32, matches);
        for (const auto* entry : matches)
            if (entry->kind == LLCommandIndex::Kind::Menu)
            { const auto command = *entry; add(command.label, [command] { LLCommandIndex::run(command); }, command.enabled); }
        add("Preferences...", [] { LLFloaterReg::showInstance("preferences"); });
        add("Search...", [] { LLFloaterReg::showInstance("search"); });
        add("Snapshot...", [] { LLFloaterReg::showInstance("snapshot"); });
    }
    s->navSelected=s->navMode==1 || s->navActions.empty() ? -1 : 0;
    s->barModel.DirtyVariable("navSelected");
    s->barModel.DirtyVariable("navLabels"); s->barModel.DirtyVariable("navEnabled"); markChanged();
}

} // namespace

LLNewUI& LLNewUI::instance()
{
    static LLNewUI ui;
    return ui;
}

bool LLNewUI::active() const { return gState != nullptr; }

bool LLNewUI::start(Rml::Context* context)
{
    if (gState || !context)
        return gState != nullptr;
    State* s = new (std::nothrow) State();
    if (!s)
        return false;
    try
    {
        s->context = context;
        LLNewUIKit::loadFonts();
        Rml::DataModelConstructor bar = context->CreateDataModel("bar");
        if (!bar)
            throw std::bad_alloc();
        bar.RegisterArray<std::vector<std::string>>();
        bar.RegisterArray<std::vector<int>>();
        bar.Bind("navOpen", &s->navOpen);
        bar.Bind("navMode", &s->navMode);
        bar.Bind("navSelected", &s->navSelected);
        bar.Bind("address", &s->address);
        bar.Bind("navLabels", &s->navLabels);
        bar.Bind("navEnabled", &s->navEnabled);
        bar.BindEventCallback("home", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { gAgent.teleportHome(); });
        bar.BindEventCallback("quickAction", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
            { if (!args.empty()) LLFloaterReg::showInstance(args[0].Get<int>() == 0 ? "snapshot" : "people"); });
        bar.BindEventCallback("navigation", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
            { if (!gState || a.empty()) return; State* s = gState;
              const int mode = std::clamp(a[0].Get<int>(),1,3);
              s->navOpen = !s->navOpen || s->navMode != mode; s->navMode = mode;
              s->navFocusWanted=s->navOpen && mode==1;
              if (s->navOpen) gFocusMgr.setKeyboardFocus(s->input);
              s->address.clear(); s->barModel.DirtyVariable("address");
              s->barModel.DirtyVariable("navOpen"); s->barModel.DirtyVariable("navMode");
              refreshNavigation(); if (s->bar) s->bar->PullToFront(); });
        bar.BindEventCallback("navClose", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { closeNavigation(); });
        bar.BindEventCallback("navGo", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { submitAddress(); });
        bar.BindEventCallback("navPick", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
            { if (!gState || a.empty()) return; const size_t i = size_t(a[0].Get<int>());
              runNavigationChoice(i); });
        bar.Bind("location", &s->location);
        bar.Bind("hint", &s->hint);
        bar.Bind("key", &s->key);
        bar.BindEventCallback("open", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { openPalette(); });
        s->barModel = bar.GetModelHandle();

        Rml::DataModelConstructor pal = context->CreateDataModel("palette");
        if (!pal)
            throw std::bad_alloc();
        pal.Bind("query", &s->query);
        pal.Bind("selected", &s->selected);
        if (auto r = pal.RegisterStruct<ResultRow>())
        {
            r.RegisterMember("kind", &ResultRow::kind);
            r.RegisterMember("label", &ResultRow::label);
            r.RegisterMember("path", &ResultRow::path);
            r.RegisterMember("shortcut", &ResultRow::shortcut);
            r.RegisterMember("check", &ResultRow::check);
            r.RegisterMember("on", &ResultRow::on);
            r.RegisterMember("off", &ResultRow::off);
        }
        pal.RegisterArray<std::vector<ResultRow>>();
        pal.Bind("rows", &s->rows);
        pal.BindEventCallback("run", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                              { if (!args.empty()) runRow(size_t(args[0].Get<int>())); });
        pal.BindEventCallback("close", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { closePalette(); });
        s->paletteModel = pal.GetModelHandle();

        // The windows (People, Inventory, Snapshot / camera mode, ...) are the tool grid's: the
        // toolbar commands, run as the classic toolbars run them (llnewuitools.h).
        s->hint      = "Search or run a command";
        s->key       = SLKeyCommands::instance().getBindingString("command_palette");
        s->navBar    = LLUI::getInstance()->getRootView()->findChildView("nav_bar_container", true)
                           ? LLUI::getInstance()->getRootView()->findChildView("nav_bar_container", true)->getHandle()
                           : LLHandle<LLView>();
        s->atBottom  = gSavedSettings.getBOOL("UINewBarAtBottom");
        s->index.rebuild(); // queues the floaters' structure: read a few files per frame from now
    }
    catch (const std::bad_alloc&)
    {
        delete s;
        return false;
    }
    gState = s; // the documents below read it (theme)
    const LLNewUIKit::Tokens tokens = currentTokens(s->theme);
    const auto               load   = loaderFor(context, tokens);
    s->bar     = load(LLNewUIDocs::kBarRml, "newui-bar.rml");
    s->palette = load(LLNewUIDocs::kPaletteRml, "newui-palette.rml");
    if (!s->bar || !s->palette)
    {
        stop();
        return false;
    }
    s->bar->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    LLView::sPresentHook = &presentHook; // (the classic presenter is off in UIRenderer 2)
    // The toolbar commands, at the bottom centre (the classic toolbars are hidden meanwhile).
    // Not fatal: the command surface still opens every window.
    if (!LLNewUITools::instance().start(context, load))
        LL_WARNS("RmlUi") << "New interface: the tool grid could not start" << LL_ENDL;
    // The status bar and its menus, in place of the classic menu bar and status bar (hidden in
    // world meanwhile; their keys keep working). Not fatal: the command surface runs every item.
    if (!LLNewUIStatus::instance().start(context, load))
        LL_WARNS("RmlUi") << "New interface: the status bar could not start" << LL_ENDL;
    // The native windows (Conversations, People, Inventory) are in development, off unless asked
    // for: until each reaches the classic one's features, the tool grid and the command surface
    // open the classic windows (doc/newui-windows.md).
    if (gSavedSettings.getBOOL("UINewWindowsDev"))
        LLNewUIWindows::instance().start(context, load);

    LLView::Params p;
    p.name           = "new_ui_input";
    p.rect           = LLUI::getInstance()->getRootView()->getLocalRect();
    p.follows.flags  = FOLLOWS_ALL;
    p.mouse_opaque   = false;
    s->input         = new (std::nothrow) InputView(p);
    if (s->input)
        LLUI::getInstance()->getRootView()->addChild(s->input); // front: first to see the mouse
    LL_INFOS("RmlUi") << "New interface on: bar + command surface + tool grid (theme " << s->theme << ")" << LL_ENDL;
    return true;
}

void LLNewUI::stop()
{
    State* s = gState;
    if (!s)
        return;
    hideClassicChrome(s, false);
    LLNewUIStatus::instance().stop();
    LLNewUITools::instance().stop();
    LLNewUIWindows::instance().stop();
    if (LLView::sPresentHook == &presentHook)
        LLView::sPresentHook = nullptr;
    if (s->input)
    {
        if (gFocusMgr.getKeyboardFocus() == s->input)
            gFocusMgr.setKeyboardFocus(nullptr);
        if (LLView* parent = s->input->getParent())
            parent->removeChild(s->input);
        delete s->input;
    }
    if (s->bar)
        s->bar->Close();
    if (s->palette)
        s->palette->Close();
    if (s->context)
    {
        s->context->RemoveDataModel("bar");
        s->context->RemoveDataModel("palette");
    }
    delete s;
    gState = nullptr;
}

bool LLNewUI::sync(float density, bool glass)
{
    State* s = gState;
    if (!s)
        return false;
    s->density = density;
    if (s->navFocusWanted && s->navOpen && s->bar)
        if (auto* field=s->bar->GetElementById("address"); field && field->IsVisible(true) && field->Focus())
            s->navFocusWanted=false;
    hideClassicChrome(s, true);

    // A theme switch (UIKitTheme): the documents again under the new tokens; models and state stay.
    static LLCachedControl<U32> theme(gSavedSettings, "UIKitTheme", 0);
    if ((theme() == 1 ? 1u : 0u) != s->theme)
        retheme(s);

    // The bar where the nav bar was (top) or at the bottom edge, in UI units: a floating bar, a
    // small gap from the edge.
    const S32 rootH = LLUI::getInstance()->getRootView()->getRect().getHeight();
    S32       top   = 0;
    if (!s->atBottom)
    {
        // (at the window's top edge while the classic top row is hidden, llnewuistatus.h)
        if (LLView* nav = s->navBar.get(); nav && !LLNewUIStatus::classicRowHidden())
            top = rootH - nav->calcScreenRect().mTop;
        top += kBarGapDp;
    }
    else
        top = rootH - kBarDp - kBarGapDp;
    if (top != s->barTop)
    {
        s->barTop = top;
        if (auto* popup=s->bar->GetElementById("navpop"))
        {
            popup->SetProperty("top",s->atBottom ? "auto" : std::to_string(top+kBarDp+8)+"dp");
            popup->SetProperty("bottom",s->atBottom ? std::to_string(rootH-top+8)+"dp" : "auto");
        }
        if (Rml::Element* wrap = s->bar->GetElementById("barwrap"))
            wrap->SetProperty("top", std::to_string(top) + "dp");
        markChanged();
    }
    if (glass != s->glassApplied || !s->placed)
    {
        // Glass on SDR targets, solid surfaces otherwise (the automatic fallback).
        s->glassApplied = glass;
        s->placed       = true;
        s->bar->SetClass("glass", glass);
        s->palette->SetClass("glass", glass);
        markChanged();
    }
    // The tool grid at the bottom centre, above the bar when the bar is at the bottom; its pull-up
    // tab flush with the bottom edge (or on the bar).
    if (LLNewUITools::instance().active() &&
        LLNewUITools::instance().sync(s->atBottom ? kBarDp + kBarGapDp + kToolsGapDp : kToolsGapDp,
                                      s->atBottom ? kBarDp + kBarGapDp : 0, density))
        markChanged();
    // The status bar at the top right; under the bar when the two would meet.
    if (LLNewUIStatus::instance().active())
    {
        int barRight = 0;
        if (Rml::Element* bar = s->atBottom ? nullptr : s->bar->GetElementById("bar"); bar && density > 0.f)
            barRight = int((bar->GetAbsoluteLeft() + bar->GetOffsetWidth()) / density);
        if (LLNewUIStatus::instance().sync(barRight, top + kBarDp, density))
            markChanged();
    }

    // Models at 4 Hz, pushing changed values only.
    const F64 now = LLTimer::getTotalSeconds();
    if (now >= s->nextRefresh)
    {
        s->nextRefresh = now + 0.25;
        const std::string loc = currentLocation();
        if (s->navOpen && ((s->navMode == 1 && s->address != s->navPrevious)
            || s->navPolicy != RlvActions::canShowLocation())) refreshNavigation();
        if (loc != s->location)
        {
            s->location = loc;
            s->barModel.DirtyVariable("location");
            markChanged();
        }
    }
    // The floaters' structure, a few XUI files per frame until complete (fixed cost).
    if (!s->index.structureComplete())
    {
        s->index.tick();
        if (s->index.structureComplete())
            LL_INFOS("RmlUi") << "New interface: command index complete, " << s->index.size() << " entries ("
                              << s->index.structureSize() << " panels and settings)" << LL_ENDL;
    }
    LLCommandIndex::tickHighlights();
    // Re-run the query when it changes, and (at most 4x a second) while the index is still growing.
    if (s->open && (s->query != s->lastQuery || (s->indexedSize != s->index.size() && now >= s->nextRequery)))
    {
        s->indexedSize = s->index.size();
        s->nextRequery = now + 0.25;
        requery(s);
    }
    if (s->open)
        markChanged(); // the caret and typing: modal and short-lived, captured while open

    // The native windows (UINewWindowsDev). A classic view that took the keyboard (a floater
    // clicked, the chat bar) takes the active window's keys away; a window field being typed in
    // is captured every frame (the caret), as the command surface is.
    if (LLNewUIWindows::instance().active())
    {
        LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
        if (focus && focus != s->input)
            LLNewUIWindows::instance().deactivate();
        if (LLNewUIWindows::instance().sync(density, s->glassApplied))
            markChanged();
        // A window may request focus after its data-if input becomes visible. Mouse-down
        // has already finished, so adopt that field here without stealing classic/modal focus.
        if (s->input && !s->open && !gFocusMgr.getKeyboardFocus())
            if (auto* field = s->input->windowField(); field && field->IsVisible(true))
                if (auto* owner = LLNewUIWindows::instance().windowOf(field); owner && owner == LLNewUIWindows::instance().activeWindow())
                    gFocusMgr.setKeyboardFocus(s->input);
        if (s->input && gFocusMgr.getKeyboardFocus() == s->input && s->input->windowField())
            markChanged();
    }

    const bool changed = s->changed;
    s->changed         = false;
    return changed;
}

void LLNewUI::togglePalette()
{
    if (!gState)
        return;
    if (gState->open)
        closePalette();
    else
        openPalette();
}

bool LLNewUI::handleWindowKey(KEY key, MASK mask)
{
    State* s = gState;
    if (s && s->navOpen && navigationKey(key)) return true;
    if (!s || s->open || !LLNewUIWindows::instance().active())
        return false;
    // A field typing (the input view has the keyboard) or a classic view with the keyboard: theirs.
    if (gFocusMgr.getKeyboardFocus())
        return false;
    if (!LLNewUIWindows::instance().handleKey(key, mask, nullptr))
        return false;
    markChanged();
    return true;
}

void LLNewUI::markOverlay()
{
    markLayer(); // no-op when the menu holder already placed it this frame

}
