// #703 The new interface's floating windows: the framework every native window is built on.
//
// A window is one RmlUi document on the kit (body class "window": the panel surface, >= 0.92
// opaque in both themes, never text on raw glass) with the kit's frame templates: <whead/> (the
// title bar: title, minimise, close) and <wgrip/> (the resize grip). Its controller is an
// LLNewUIWindow bound to the viewer's models; the framework owns everything else:
//  - geometry in UI units (dp; follows the UI scale and the display scale): moved by the title
//    bar, resized by the grip, snapped to the screen edges and to the other windows, kept on screen
//    (llnewuiwindowlayout.h, tested by the newui-windows check);
//  - z order (a click raises) and focus (the clicked window is the active one: its title is lit,
//    Esc closes it, Tab moves through its controls; a click on the world or a classic floater
//    takes that away);
//  - saved state: position, size, open and minimised per window key, in UINewWindowState (one
//    string, Persist 1); windows that were open come back at login;
//  - several instances of the kinds that allow it (conversations torn off, inventory windows):
//    each its own data model ("inv", "inv2", ...) and saved key ("inventory", "inventory.2");
//  - theme switches (the documents reload; models and state stay) and camera mode (the RmlUi host
//    hides and re-shows every document).
//
// Rendering order: the RmlUi layer draws above every classic LLView floater, so native windows
// are always in front of classic ones (they cannot interleave); the keyboard goes to whichever
// was clicked last.
//
// Off unless UINewWindowsDev (development, Persist 0): the classic windows stay what the tool grid
// and the command surface open until a native one reaches feature parity (doc/newui-windows.md).
#pragma once

#include "llnewuiwindowlayout.h"
#include "stdtypes.h"
#include "llkeyboard.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Rml
{
class Context;
class DataModelConstructor;
class Element;
class Event;
class ElementDocument;
}

class LLSD;
class LLView;

class LLNewUIWindow
{
public:
    virtual ~LLNewUIWindow() = default;
    // The document (llnewuiwindowdocs.h) and the data model name written in it.
    virtual const char* rml() const   = 0;
    virtual const char* model() const = 0;
    // Binds the window's variables and callbacks (the framework has bound "title" and "active").
    virtual bool bind(Rml::DataModelConstructor& m) = 0;
    // Each frame while open: refresh from the models (the window decides its own rate). True when
    // something changed (the layer is captured again).
    virtual bool sync(F64 now) = 0;
    // Enter in one of its text fields; true when handled.
    virtual bool onReturn(Rml::Element* field, MASK mask) { return false; }
    // A key while one of its text fields types (before the field gets it): Tab completion, Up for
    // the last line ...; true when handled.
    virtual bool onFieldKey(Rml::Element* field, KEY key, MASK mask) { return false; }
    // A key while the window is active and no field types (Up / Down / PgUp / PgDn / Delete ...).
    virtual bool onKey(KEY key, MASK mask) { return false; }
    // The left button went down over `hover` (inside its body): true to start a drag of the
    // window's own (inventory items onto the world); then dragMove / dragEnd follow.
    virtual bool wantsDrag(Rml::Element* hover) { return false; }
    virtual void dragMove(S32 dx, S32 dy) {}
    virtual void dragEnd() {}
    virtual void onClosed() {}
    // A classic live drawing, embedded at the RML nativeMap element (currently LLNetMap).
    // Its model/input stay classic; the framework clips it against higher native windows.
    virtual LLView* classicOverlay() { return nullptr; }
    // Opened again with `arg` (a conversation to show): select it.
    virtual void reveal(const std::string& arg) {}
    // The NewUI event API's "input": what a pointer and keyboard would do (select a view, type a
    // filter or a line, open the context menu), for tests on a box without input. True when used.
    virtual bool onTest(const LLSD& args) { return false; }

    // ---- the framework's (set before bind) ----
    std::string          key;   // saved-state key ("chat", "inventory.2")
    std::string          kind;  // registered kind ("chat", "inventory")
    std::string          title;
    std::string          arg;   // what it was opened with (a session id ...)
    bool                 active = false;
    Rml::DataModelHandle handle;
    Rml::ElementDocument* doc = nullptr;
    // Tells the framework the document changed (capture again).
    void dirty(const char* variable);
    void setTitle(const std::string& t);
    bool changed = true;
    // Helpers for windows' lists and menus: how many rows of `rowDp` the element `id` shows (at
    // least 1, at most `most`); where the pointer of a mouse event is inside the element `id`, in dp.
    int  visibleRows(const char* id, float rowDp, int most) const;
    bool pointerIn(const Rml::Event& event, const char* id, float& xDp, float& yDp, float& wDp, float& hDp) const;
    // A mouse event's button (0 left, 1 right, 2 middle).
    static int buttonOf(const Rml::Event& event);

    // ---- the framework's own (llnewuiwindow.cpp; windows leave them alone) ----
    int        mId = 0;
    std::string mModelName;
    slwin::Rect mRect;
    bool        mMinimised = false, mPlaced = false;
    float       mDensityApplied = 0.f;
};

class LLNewUIWindows
{
public:
    using DocumentLoader = std::function<Rml::ElementDocument*(const char* rml, const char* name)>;
    using Factory        = std::function<std::unique_ptr<LLNewUIWindow>(const std::string& arg)>;
    struct Kind
    {
        std::string kind, title;
        Factory     make;
        slwin::Rect place; // default rect: x / y from the screen's left / top (negative: from the right / bottom)
        float       minW = 240.f, minH = 160.f;
        bool        multi   = false; // several instances (each its own saved key)
        bool        restore = true;  // reopened at login when it was open
        bool        restoreInstances = false; // the other instances too (inventory windows; not torn-off chats)
    };

    static LLNewUIWindows& instance();
    void registerKind(const Kind& kind);

    bool start(Rml::Context* context, const DocumentLoader& load);
    void stop();
    bool active() const;
    void drawClassicOverlays();
    bool overlayToolTip(Rml::Element* hover, MASK mask);
    // The documents again under new tokens (a theme switch); windows, models and places stay.
    void reload(const DocumentLoader& load);
    // Each frame (llnewui.cpp): `density` framebuffer pixels per UI unit; `glass` the SDR blur.
    // True when a window's document changed.
    bool sync(float density, bool glass);

    // Opens (or raises) the kind's window; `arg` selects inside it (a conversation). A multi kind
    // opens a new instance with `fresh`.
    LLNewUIWindow* open(const std::string& kind, const std::string& arg = std::string(), bool fresh = false);
    // Open and in front: closes; open behind another: raises; closed: opens.
    void toggle(const std::string& kind);
    bool isOpen(const std::string& kind) const;
    void close(LLNewUIWindow* w);
    LLNewUIWindow* find(const std::string& key) const;
    std::vector<LLNewUIWindow*> windows() const;

    // ---- input (the new interface's input view, llnewui.cpp; UI units, y up as LLView) ----
    bool ownsDocument(const Rml::ElementDocument* doc) const;
    LLNewUIWindow* windowOf(const Rml::Element* e) const;
    // Left button down over `hover` (a window's element): the frame's part (move, resize, close,
    // minimise) is the framework's; true when it took the press (not passed to RmlUi). Raises
    // and activates the window either way.
    bool mouseDown(Rml::Element* hover, S32 x, S32 y, MASK mask, bool doubleClick);
    // Any other button over a window: raise and activate it only (no move, resize or drag).
    void focus(Rml::Element* hover);
    // While the framework drags (move / resize / a window's own drag): pointer moves and release.
    bool dragging() const;
    void mouseMove(S32 x, S32 y);
    void mouseUp(S32 x, S32 y);
    // The pointer went down elsewhere (world, classic floater): no window is active.
    void deactivate();
    LLNewUIWindow* activeWindow() const;
    // Keys for the active window (Esc closes, Tab moves focus, the window's own); `field` is its
    // focused text field or null. True when handled.
    bool handleKey(KEY key, MASK mask, Rml::Element* field);
    bool handleReturn(Rml::Element* field, MASK mask);
};
