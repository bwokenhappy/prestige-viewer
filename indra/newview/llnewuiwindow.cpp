// #703 The new interface's floating windows (see llnewuiwindow.h).
#include "llviewerprecompiledheaders.h"
#include "llnewuiwindow.h"

#include "llnewuiwindowkinds.h"
#include "llmenugl.h"
#include "llstartup.h"
#include "llui.h"
#include "llview.h"
#include "llrender.h"
#include "lllocalcliprect.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "llnewuistatus.h"
#include "llnewuitools.h"
#include "lleventapi.h"
#include "llsdutil.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <algorithm>
#include <new>

namespace
{
constexpr char kStateSetting[] = "UINewWindowState";
constexpr float kHeadDp        = 32.f; // .whead 30 dp + the window's 1 dp borders (llnewuikitstyle.cpp)
constexpr float kCascadeDp     = 28.f;

enum class Drag
{
    None,
    Move,
    Resize,
    Own
};

std::vector<LLNewUIWindows::Kind>& kinds()
{
    static std::vector<LLNewUIWindows::Kind> k;
    return k;
}

const LLNewUIWindows::Kind* kindOf(const std::string& kind)
{
    for (const auto& k : kinds())
        if (k.kind == kind)
            return &k;
    return nullptr;
}

struct State
{
    Rml::Context*                               context = nullptr;
    LLNewUIWindows::DocumentLoader              load;
    std::vector<std::unique_ptr<LLNewUIWindow>> wins;
    std::vector<std::unique_ptr<LLNewUIWindow>> closing; // documents closed; models removed next frame
    std::vector<int>                            order;   // z order, front last
    std::vector<slwin::Saved>                   saved;
    int                                         nextId   = 1;
    bool                                        savedDirty = false, restored = false, glass = false, changed = true;
    float                                       density  = 1.f;
    slwin::Screen                               screen;
    LLNewUIWindow*                              activeWin = nullptr;
    std::string                                 restoreKey; // the saved key a restored instance takes
    // dragging
    Drag           drag    = Drag::None;
    LLNewUIWindow* dragWin = nullptr;
    S32            x0 = 0, y0 = 0;
    slwin::Rect    r0;
};
State* gState = nullptr;

std::string dp(float v) { return std::to_string(int(std::lround(v))) + "dp"; }

slwin::Screen currentScreen()
{
    slwin::Screen s;
    LLView*       root = LLUI::getInstance()->getRootView();
    if (!root)
        return s;
    s.right  = float(root->getRect().getWidth());
    s.bottom = float(root->getRect().getHeight());
    // Under the menu bar while it shows (the status bar is part of it).
    if (gMenuBarView && gMenuBarView->getVisible())
        s.top = std::max(0.f, s.bottom - float(gMenuBarView->calcScreenRect().mBottom));
    // Under the new status bar's row while it shows (the classic row hidden, llnewuistatus.h).
    if (LLNewUIStatus::instance().active())
        s.top = std::max(s.top, float(LLNewUIStatus::instance().bottomDp()));
    return s;
}

void apply(LLNewUIWindow* w)
{
    if (!w->doc)
        return;
    const slwin::Rect& r = w->mRect;
    w->doc->SetProperty("left", dp(r.x));
    w->doc->SetProperty("top", dp(r.y));
    w->doc->SetProperty("width", dp(r.w));
    w->doc->SetProperty("height", dp(w->mMinimised ? kHeadDp : r.h));
    w->doc->SetClass("min", w->mMinimised);
    w->changed = true;
}

void remember(LLNewUIWindow* w, bool open)
{
    State* s = gState;
    if (!s)
        return;
    slwin::upsert(s->saved, { w->key, w->mRect, open, w->mMinimised });
    s->savedDirty = true;
}

void writeSaved(State* s)
{
    if (!s->savedDirty)
        return;
    s->savedDirty = false;
    gSavedSettings.setString(kStateSetting, slwin::encode(s->saved));
}

const slwin::Saved* savedFor(State* s, const std::string& key)
{
    for (const auto& e : s->saved)
        if (e.key == key)
            return &e;
    return nullptr;
}

std::vector<slwin::Rect> otherRects(State* s, const LLNewUIWindow* except)
{
    std::vector<slwin::Rect> out;
    for (const auto& w : s->wins)
        if (w.get() != except)
        {
            slwin::Rect r = w->mRect;
            if (w->mMinimised)
                r.h = kHeadDp;
            out.push_back(r);
        }
    return out;
}

const LLNewUIWindows::Kind* kindFor(const LLNewUIWindow* w) { return kindOf(w->kind); }

void clampWindow(State* s, LLNewUIWindow* w)
{
    const LLNewUIWindows::Kind* k = kindFor(w);
    w->mRect = slwin::clampToScreen(w->mRect, k ? k->minW : 200.f, k ? k->minH : 120.f, s->screen);
}

void setActive(State* s, LLNewUIWindow* w)
{
    if (s->activeWin == w)
        return;
    if (s->activeWin)
    {
        s->activeWin->active = false;
        s->activeWin->dirty("active");
        if (s->activeWin->doc)
            s->activeWin->doc->SetClass("active", false);
    }
    s->activeWin = w;
    if (w)
    {
        w->active = true;
        w->dirty("active");
        if (w->doc)
            w->doc->SetClass("active", true);
    }
    s->changed = true;
}

void raiseWindow(State* s, LLNewUIWindow* w)
{
    slwin::raise(s->order, w->mId);
    if (w->doc)
        w->doc->PullToFront();
    s->changed = true;
}

// The model's name in this instance's document: "inv" for the first, "inv2" ... after it.
std::string withModel(const char* rml, const std::string& base, const std::string& name)
{
    std::string       s    = rml;
    const std::string from = "data-model=\"" + base + "\"";
    const size_t      at   = s.find(from);
    if (at != std::string::npos && name != base)
        s.replace(at, from.size(), "data-model=\"" + name + "\"");
    return s;
}

bool loadDocument(State* s, LLNewUIWindow* w)
{
    const std::string rml  = withModel(w->rml(), w->model(), w->mModelName);
    const std::string name = "newui-" + w->key + ".rml";
    Rml::ElementDocument* doc = s->load(rml.c_str(), name.c_str());
    if (!doc)
        return false;
    w->doc = doc;
    doc->SetClass("window", true);
    doc->SetClass("glass", s->glass);
    doc->SetClass("active", w->active);
    apply(w);
    doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    return true;
}

void destroyClosing(State* s)
{
    for (auto& w : s->closing)
        if (s->context)
            s->context->RemoveDataModel(w->mModelName);
    s->closing.clear();
}
} // namespace

// The windows over the event API ("NewUI"): scripts, tests and the developer test channel open,
// place and close them without a pointer (review screenshots on a test box). Every op replies.
class NewUIListener final : public LLEventAPI
{
public:
    NewUIListener() : LLEventAPI("NewUI", "The new interface's native windows (#703) and its tool grid")
    {
        add("open", "[\"kind\"]: chat | people | inventory; [\"arg\"]: what to show (a conversation's session id, \"nearby\"); [\"fresh\"]: a new instance",
            &NewUIListener::openOp, llsd::map("kind", LLSD()));
        add("close", "[\"key\"]: the window (\"chat\", \"inventory.2\")", &NewUIListener::closeOp, llsd::map("key", LLSD()));
        add("place", "[\"key\"], [\"x\"], [\"y\"], [\"w\"], [\"h\"] in UI units (snapped to nothing, kept on screen)", &NewUIListener::placeOp,
            llsd::map("key", LLSD()));
        add("minimise", "[\"key\"], [\"on\"]", &NewUIListener::minimiseOp, llsd::map("key", LLSD()));
        add("list", "the open windows, front last: key, rect, minimised, active", &NewUIListener::listOp);
        add("peek", "[\"on\"]: the tool grid opens as if hovered (tests without a pointer)", &NewUIListener::peekOp);
        add("input",
            "[\"key\"]: a window; the rest is what a pointer / keyboard would do there, per window: "
            "chat {line, send, tab}; people {view, filter, select, menu}; inventory {view, filter, select, toggle, menu}",
            &NewUIListener::inputOp, llsd::map("key", LLSD()));
    }

private:
    void reply(const LLSD& event, bool ok, const std::string& error = std::string()) const
    {
        LLSD r = llsd::map("ok", ok);
        if (!error.empty())
            r["error"] = error;
        sendReply(r, event);
    }
    void openOp(const LLSD& event) const
    {
        const bool ok = LLNewUIWindows::instance().open(event["kind"].asString(), event["arg"].asString(), event["fresh"].asBoolean());
        reply(event, ok, ok ? std::string() : "not open (UINewWindowsDev off, or no such kind)");
    }
    void closeOp(const LLSD& event) const
    {
        LLNewUIWindow* w = LLNewUIWindows::instance().find(event["key"].asString());
        if (w)
            LLNewUIWindows::instance().close(w);
        reply(event, w != nullptr, w ? std::string() : "no such window");
    }
    void placeOp(const LLSD& event) const;
    void minimiseOp(const LLSD& event) const;
    void listOp(const LLSD& event) const;
    void inputOp(const LLSD& event) const
    {
        LLNewUIWindow* w = LLNewUIWindows::instance().find(event["key"].asString());
        const bool     ok = w && w->onTest(event);
        if (w)
            w->changed = true;
        reply(event, ok, ok ? std::string() : "no such window, or nothing it takes");
    }
    void peekOp(const LLSD& event) const
    {
        LLNewUITools::instance().setPeek(event["on"].asBoolean());
        reply(event, true);
    }
};
std::unique_ptr<NewUIListener> gListener;

// ---- LLNewUIWindow ----

void LLNewUIWindow::dirty(const char* variable)
{
    if (handle)
        handle.DirtyVariable(variable);
    changed = true;
}

void LLNewUIWindow::setTitle(const std::string& t)
{
    if (t == title)
        return;
    title = t;
    dirty("title");
}

int LLNewUIWindow::visibleRows(const char* id, float rowDp, int most) const
{
    Rml::Element* e = doc ? doc->GetElementById(id) : nullptr;
    Rml::Context* c = doc ? doc->GetContext() : nullptr;
    if (!e || !c || rowDp <= 0.f)
        return 1;
    const float px = rowDp * c->GetDensityIndependentPixelRatio();
    return std::clamp(int(e->GetClientHeight() / px), 1, most);
}

bool LLNewUIWindow::pointerIn(const Rml::Event& event, const char* id, float& xDp, float& yDp, float& wDp, float& hDp) const
{
    Rml::Element* e = doc ? doc->GetElementById(id) : nullptr;
    Rml::Context* c = doc ? doc->GetContext() : nullptr;
    if (!e || !c)
        return false;
    const float d = c->GetDensityIndependentPixelRatio();
    xDp           = (event.GetParameter<float>("mouse_x", 0.f) - e->GetAbsoluteLeft()) / d;
    yDp           = (event.GetParameter<float>("mouse_y", 0.f) - e->GetAbsoluteTop()) / d;
    wDp           = e->GetClientWidth() / d;
    hDp           = e->GetClientHeight() / d;
    return true;
}

int LLNewUIWindow::buttonOf(const Rml::Event& event) { return event.GetParameter<int>("button", 0); }

void NewUIListener::placeOp(const LLSD& event) const
{
    State*         s = gState;
    LLNewUIWindow* w = LLNewUIWindows::instance().find(event["key"].asString());
    if (!s || !w)
        return reply(event, false, "no such window");
    w->mRect = { F32(event["x"].asReal()), F32(event["y"].asReal()), F32(event["w"].asReal()), F32(event["h"].asReal()) };
    clampWindow(s, w);
    apply(w);
    remember(w, true);
    reply(event, true);
}

void NewUIListener::minimiseOp(const LLSD& event) const
{
    State*         s = gState;
    LLNewUIWindow* w = LLNewUIWindows::instance().find(event["key"].asString());
    if (!s || !w)
        return reply(event, false, "no such window");
    w->mMinimised = event["on"].asBoolean();
    apply(w);
    remember(w, true);
    reply(event, true);
}

void NewUIListener::listOp(const LLSD& event) const
{
    LLSD out = LLSD::emptyArray();
    if (State* s = gState)
        for (int id : s->order)
            for (const auto& w : s->wins)
                if (w->mId == id)
                    out.append(llsd::map("key", w->key, "x", w->mRect.x, "y", w->mRect.y, "w", w->mRect.w, "h", w->mRect.h, "minimised",
                                         w->mMinimised, "active", w->active));
    sendReply(llsd::map("ok", true, "windows", out), event);
}

// ---- LLNewUIWindows ----

LLNewUIWindows& LLNewUIWindows::instance()
{
    static LLNewUIWindows w;
    return w;
}

void LLNewUIWindows::registerKind(const Kind& kind)
{
    for (auto& k : kinds())
        if (k.kind == kind.kind)
        {
            k = kind;
            return;
        }
    kinds().push_back(kind);
}

bool LLNewUIWindows::active() const { return gState != nullptr; }

bool LLNewUIWindows::start(Rml::Context* context, const DocumentLoader& load)
{
    if (gState || !context)
        return gState != nullptr;
    State* s = new (std::nothrow) State();
    if (!s)
        return false;
    s->context = context;
    s->load    = load;
    s->saved   = slwin::decode(gSavedSettings.getString(kStateSetting));
    s->screen  = currentScreen();
    gState     = s;
    slNewUIRegisterWindows(*this);
    try
    {
        gListener = std::make_unique<NewUIListener>();
    }
    catch (const std::exception&)
    {
        // no event API: the windows work without it
    }
    LL_INFOS("RmlUi") << "New interface: windows on (" << kinds().size() << " kinds, " << s->saved.size() << " saved places)" << LL_ENDL;
    return true;
}

void LLNewUIWindows::stop()
{
    State* s = gState;
    if (!s)
        return;
    writeSaved(s);
    for (auto& w : s->wins)
    {
        w->onClosed();
        if (w->doc)
            w->doc->Close();
        s->closing.push_back(std::move(w));
    }
    s->wins.clear();
    destroyClosing(s);
    gListener.reset();
    delete s;
    gState = nullptr;
}

void LLNewUIWindows::reload(const DocumentLoader& load)
{
    State* s = gState;
    if (!s)
        return;
    s->load = load;
    for (auto& w : s->wins)
    {
        Rml::ElementDocument* old = w->doc;
        if (!loadDocument(s, w.get()))
        {
            w->doc = old; // keep the old look rather than lose the window
            continue;
        }
        if (old)
            old->Close();
        w->handle.DirtyAllVariables();
    }
    // The z order again (new documents are in front in load order).
    for (int id : s->order)
        for (auto& w : s->wins)
            if (w->mId == id && w->doc)
                w->doc->PullToFront();
    s->changed = true;
}

LLNewUIWindow* LLNewUIWindows::find(const std::string& key) const
{
    if (!gState)
        return nullptr;
    for (const auto& w : gState->wins)
        if (w->key == key)
            return w.get();
    return nullptr;
}

std::vector<LLNewUIWindow*> LLNewUIWindows::windows() const
{
    std::vector<LLNewUIWindow*> out;
    if (gState)
        for (const auto& w : gState->wins)
            out.push_back(w.get());
    return out;
}

bool LLNewUIWindows::isOpen(const std::string& kind) const
{
    if (!gState)
        return false;
    return std::any_of(gState->wins.begin(), gState->wins.end(), [&](const auto& w) { return w->kind == kind; });
}

LLNewUIWindow* LLNewUIWindows::open(const std::string& kind, const std::string& arg, bool fresh)
{
    State* s = gState;
    const Kind* k = kindOf(kind);
    if (!s || !k)
        return nullptr;
    if (!fresh || !k->multi)
    {
        // The open one: the instance showing `arg` if any, else the front-most of the kind.
        // A main instance (opened without an argument) takes `arg`; a torn-off conversation is
        // found only by its own argument.
        LLNewUIWindow* found = nullptr;
        LLNewUIWindow* match = nullptr;
        for (auto it = s->order.rbegin(); it != s->order.rend(); ++it)
            for (const auto& w : s->wins)
                if (w->mId == *it && w->kind == kind)
                {
                    if (!found && w->arg.empty())
                        found = w.get();
                    if (!match && !arg.empty() && w->arg == arg)
                        match = w.get();
                }
        if (match)
            found = match;
        if (found)
        {
            if (!arg.empty())
                found->reveal(arg);
            if (found->mMinimised)
            {
                found->mMinimised = false;
                apply(found);
                remember(found, true);
            }
            raiseWindow(s, found);
            setActive(s, found);
            return found;
        }
    }
    // A new instance: the first free number of the kind ("inventory", "inventory.2", ...).
    // At login a saved instance comes back under its own key (and so its own place).
    auto keyFree = [&](const std::string& key)
    { return !find(key) && std::none_of(s->closing.begin(), s->closing.end(), [&](const auto& w) { return w->key == key; }); };
    int n = 1;
    if (!s->restoreKey.empty() && keyFree(s->restoreKey) && s->restoreKey.rfind(kind, 0) == 0)
        n = s->restoreKey == kind ? 1 : std::max(1, std::atoi(s->restoreKey.c_str() + kind.size() + 1));
    else
        while (!keyFree(n == 1 ? kind : kind + "." + std::to_string(n)))
            ++n;
    std::unique_ptr<LLNewUIWindow> w;
    try
    {
        w = k->make(fresh ? arg : std::string()); // not fresh: the main instance, `arg` revealed in it
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
    if (!w)
        return nullptr;
    w->kind        = kind;
    w->key         = n == 1 ? kind : kind + "." + std::to_string(n);
    w->title       = k->title;
    w->arg         = fresh ? arg : std::string();
    w->mId         = s->nextId++;
    w->mModelName  = n == 1 ? std::string(w->model()) : std::string(w->model()) + std::to_string(n);
    Rml::DataModelConstructor m = s->context->CreateDataModel(w->mModelName);
    if (!m)
        return nullptr;
    m.Bind("title", &w->title);
    m.Bind("active", &w->active);
    if (!w->bind(m))
    {
        s->context->RemoveDataModel(w->mModelName);
        return nullptr;
    }
    w->handle = m.GetModelHandle();
    // Its place: saved, else the default (negative x / y count from the right / bottom), cascaded
    // off the windows already there.
    if (const slwin::Saved* saved = savedFor(s, w->key))
    {
        w->mRect      = saved->rect;
        w->mMinimised = saved->minimised;
    }
    else
    {
        slwin::Rect r = k->place;
        r.x           = r.x < 0 ? s->screen.right + r.x - r.w : s->screen.left + r.x;
        r.y           = r.y < 0 ? s->screen.bottom + r.y - r.h : s->screen.top + r.y;
        w->mRect      = slwin::cascade(r, otherRects(s, nullptr), kCascadeDp, s->screen);
    }
    clampWindow(s, w.get());
    LLNewUIWindow* raw = w.get();
    s->wins.push_back(std::move(w));
    if (!loadDocument(s, raw))
    {
        s->context->RemoveDataModel(raw->mModelName);
        s->wins.pop_back();
        LL_WARNS("RmlUi") << "New interface: window " << kind << " could not load" << LL_ENDL;
        return nullptr;
    }
    raiseWindow(s, raw);
    setActive(s, raw);
    remember(raw, true);
    if (!fresh && !arg.empty())
        raw->reveal(arg);
    LL_INFOS("RmlUi") << "New interface: window " << raw->key << " opened" << LL_ENDL;
    return raw;
}

void LLNewUIWindows::toggle(const std::string& kind)
{
    State* s = gState;
    if (!s)
        return;
    // The front-most window of the kind.
    LLNewUIWindow* front = nullptr;
    for (auto it = s->order.rbegin(); it != s->order.rend() && !front; ++it)
        for (const auto& w : s->wins)
            if (w->mId == *it && w->kind == kind)
                front = w.get();
    if (!front)
    {
        open(kind);
        return;
    }
    // In front of every other window and not minimised: close it; otherwise bring it up.
    if (!s->order.empty() && s->order.back() == front->mId && !front->mMinimised)
        close(front);
    else
        open(kind);
}

void LLNewUIWindows::close(LLNewUIWindow* w)
{
    State* s = gState;
    if (!s || !w)
        return;
    auto it = std::find_if(s->wins.begin(), s->wins.end(), [w](const auto& p) { return p.get() == w; });
    if (it == s->wins.end())
        return;
    if (s->dragWin == w)
    {
        s->drag    = Drag::None;
        s->dragWin = nullptr;
    }
    if (s->activeWin == w)
        s->activeWin = nullptr;
    remember(w, false);
    w->onClosed();
    if (w->doc)
        w->doc->Close(); // destroyed at the next context update
    s->order.erase(std::remove(s->order.begin(), s->order.end(), w->mId), s->order.end());
    LL_INFOS("RmlUi") << "New interface: window " << w->key << " closed" << LL_ENDL;
    s->closing.push_back(std::move(*it)); // its model goes next frame (not inside its own event)
    s->wins.erase(it);
    s->changed = true;
}

bool LLNewUIWindows::sync(float density, bool glass)
{
    State* s = gState;
    if (!s)
        return false;
    destroyClosing(s);
    s->density = density;
    if (glass != s->glass)
    {
        s->glass = glass;
        for (auto& w : s->wins)
            if (w->doc)
                w->doc->SetClass("glass", glass);
        s->changed = true;
    }
    // The screen changed (window resized, UI scale, the menu bar shown / hidden): back on it.
    const slwin::Screen screen = currentScreen();
    if (screen.left != s->screen.left || screen.top != s->screen.top || screen.right != s->screen.right ||
        screen.bottom != s->screen.bottom)
    {
        s->screen = screen;
        for (auto& w : s->wins)
        {
            const slwin::Rect before = w->mRect;
            clampWindow(s, w.get());
            if (w->mRect != before)
                apply(w.get());
        }
        s->changed = true;
    }
    // At login: the windows that were open last time.
    if (!s->restored && LLStartUp::getStartupState() >= STATE_STARTED)
    {
        s->restored = true;
        const std::vector<slwin::Saved> saved = s->saved;
        for (const auto& e : saved)
        {
            if (!e.open)
                continue;
            const std::string kind = e.key.substr(0, e.key.find('.'));
            const Kind*       k    = kindOf(kind);
            s->restoreKey = e.key;
            if (k && k->restore && (e.key == kind || k->restoreInstances))
                open(kind, std::string(), e.key != kind);
        }
        s->restoreKey.clear();
        setActive(s, nullptr);
    }
    const F64 now = LLTimer::getTotalSeconds();
    // A window may close itself in its sync (a conversation ended): walk a copy.
    for (LLNewUIWindow* w : windows())
        if (w->sync(now))
            w->changed = true;
    bool changed = s->changed;
    for (auto& w : s->wins)
    {
        changed |= w->changed;
        w->changed = false;
    }
    s->changed = false;
    if (s->drag == Drag::None)
        writeSaved(s);
    return changed;
}

bool LLNewUIWindows::ownsDocument(const Rml::ElementDocument* doc) const
{
    if (!gState || !doc)
        return false;
    return std::any_of(gState->wins.begin(), gState->wins.end(), [doc](const auto& w) { return w->doc == doc; });
}

LLNewUIWindow* LLNewUIWindows::windowOf(const Rml::Element* e) const
{
    if (!gState || !e)
        return nullptr;
    const Rml::ElementDocument* doc = e->GetOwnerDocument();
    for (const auto& w : gState->wins)
        if (w->doc == doc)
            return w.get();
    return nullptr;
}

namespace
{
bool within(Rml::Element* e, const char* id, Rml::Element* stop)
{
    for (; e && e != stop; e = e->GetParentNode())
        if (e->GetId() == id)
            return true;
    return false;
}
} // namespace

bool LLNewUIWindows::mouseDown(Rml::Element* hover, S32 x, S32 y, MASK mask, bool doubleClick)
{
    State*         s = gState;
    LLNewUIWindow* w = windowOf(hover);
    if (!s || !w)
        return false;
    raiseWindow(s, w);
    setActive(s, w);
    Rml::Element* stop = w->doc;
    if (within(hover, "wclose", stop))
    {
        close(w);
        return true;
    }
    if (within(hover, "wmin", stop) || (doubleClick && within(hover, "whead", stop)))
    {
        w->mMinimised = !w->mMinimised;
        apply(w);
        clampWindow(s, w);
        remember(w, true);
        return true;
    }
    const float pointerDownY = float(LLUI::getInstance()->getRootView()->getRect().getHeight() - y);
    if (!w->mMinimised && (within(hover, "wgrip", stop) || slwin::resizeCorner(w->mRect, float(x), pointerDownY)))
        s->drag = Drag::Resize;
    else if (within(hover, "whead", stop))
        s->drag = Drag::Move;
    else if (w->wantsDrag(hover))
    {
        s->drag    = Drag::Own;
        s->dragWin = w;
        s->x0      = x;
        s->y0      = y;
        return false; // the press still reaches the row (selection)
    }
    else
        return false;
    s->dragWin = w;
    s->x0      = x;
    s->y0      = y;
    s->r0      = w->mRect;
    return true;
}

void LLNewUIWindows::focus(Rml::Element* hover)
{
    if (LLNewUIWindow* w = windowOf(hover); w && gState)
    {
        raiseWindow(gState, w);
        setActive(gState, w);
    }
}

bool LLNewUIWindows::dragging() const { return gState && gState->drag != Drag::None && gState->dragWin; }

void LLNewUIWindows::mouseMove(S32 x, S32 y)
{
    State* s = gState;
    if (!s || !s->dragWin)
        return;
    LLNewUIWindow* w  = s->dragWin;
    const float    dx = float(x - s->x0), dy = float(y - s->y0); // UI units, y up
    if (s->drag == Drag::Own)
    {
        w->dragMove(S32(dx), S32(dy));
        return;
    }
    slwin::Rect r = s->r0;
    if (s->drag == Drag::Move)
    {
        r.x += dx;
        r.y -= dy;
        r = slwin::snapMove(r, otherRects(s, w), s->screen);
    }
    else
    {
        const Kind* k = kindFor(w);
        r.w           = std::max(r.w + dx, k ? k->minW : 200.f);
        r.h           = std::max(r.h - dy, k ? k->minH : 120.f);
        r             = slwin::snapResize(r, otherRects(s, w), s->screen);
    }
    w->mRect = r;
    clampWindow(s, w);
    apply(w);
}

void LLNewUIWindows::mouseUp(S32 x, S32 y)
{
    State* s = gState;
    if (!s || !s->dragWin)
    {
        if (s)
            s->drag = Drag::None;
        return;
    }
    LLNewUIWindow* w = s->dragWin;
    const Drag     d = s->drag;
    s->drag          = Drag::None;
    s->dragWin       = nullptr;
    if (d == Drag::Own)
    {
        w->dragEnd();
        return;
    }
    remember(w, true);
    (void)x;
    (void)y;
}

void LLNewUIWindows::deactivate()
{
    if (gState)
        setActive(gState, nullptr);
}

LLNewUIWindow* LLNewUIWindows::activeWindow() const { return gState ? gState->activeWin : nullptr; }

bool LLNewUIWindows::handleReturn(Rml::Element* field, MASK mask)
{
    LLNewUIWindow* w = windowOf(field);
    return w && w->onReturn(field, mask);
}

bool LLNewUIWindows::handleKey(KEY key, MASK mask, Rml::Element* field)
{
    State*         s = gState;
    LLNewUIWindow* w = s ? (field ? windowOf(field) : s->activeWin) : nullptr;
    if (!w)
        return false;
    if (field)
        return w->onFieldKey(field, key, mask);
    if (key == KEY_ESCAPE && mask == MASK_NONE)
    {
        close(w);
        return true;
    }
    if (key == KEY_TAB && !(mask & (MASK_CONTROL | MASK_ALT)))
    {
        s->context->ProcessKeyDown(Rml::Input::KI_TAB, (mask & MASK_SHIFT) ? Rml::Input::KM_SHIFT : 0);
        s->changed = true;
        return true;
    }
    // Space / Enter press the focused control of the window.
    if ((key == ' ' || key == KEY_RETURN) && mask == MASK_NONE)
        if (Rml::Element* focus = s->context->GetFocusElement(); focus && focus != w->doc && windowOf(focus) == w)
        {
            focus->Click();
            s->changed = true;
            return true;
        }
    return w->onKey(key, mask);
}

void LLNewUIWindows::drawClassicOverlays()
{
    State* s = gState;
    LLView* root = LLUI::getInstance()->getRootView();
    if (!s || !root) return;
    if (std::none_of(s->wins.begin(),s->wins.end(),[](const auto& w)
            { return w->doc && w->doc->IsVisible() && !w->mMinimised && w->classicOverlay(); })) return;
    const float rootHeight = float(root->getRect().getHeight());
    for (size_t z = 0; z < s->order.size(); ++z)
        for (const auto& window : s->wins)
        {
            LLNewUIWindow* w = window.get();
            if (w->mId != s->order[z] || w->mMinimised || !w->doc || !w->doc->IsVisible()) continue;
            auto* element = w->doc->GetElementById("nativeMap");
            if (!element || !element->IsVisible()) continue;
            LLView* view = w->classicOverlay();
            if (!view) continue;
            const float density = w->doc->GetContext()->GetDensityIndependentPixelRatio();
            // Context::Update belongs inside the Rml renderer's capture transaction.
            // During move/resize, wait for that layout to publish rather than drawing
            // the classic live view at a stale aperture (normally one frame).
            if (std::fabs(w->doc->GetAbsoluteLeft()/density-w->mRect.x)>1.f
                || std::fabs(w->doc->GetAbsoluteTop()/density-w->mRect.y)>1.f
                || std::fabs(w->doc->GetOffsetWidth()/density-w->mRect.w)>1.f
                || std::fabs(w->doc->GetOffsetHeight()/density-w->mRect.h)>1.f) continue;
            const slwin::Rect rect{ element->GetAbsoluteLeft()/density, element->GetAbsoluteTop()/density,
                                   element->GetClientWidth()/density, element->GetClientHeight()/density };
            if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.w) || !std::isfinite(rect.h)
                || rect.w < 2.f || rect.h < 2.f) continue;
            const S32 width = S32(rect.w), height = S32(rect.h);
            if (view->getRect().getWidth() != width || view->getRect().getHeight() != height) view->reshape(width,height);
            view->setOrigin(S32(rect.x),S32(rootHeight-rect.bottom()));
            std::array<slwin::Rect,64> clips{}; clips[0]=rect; size_t count=1;
            for (size_t j = z+1; j < s->order.size() && count; ++j)
                for (const auto& other : s->wins)
                    if (other->mId == s->order[j] && other->doc && other->doc->IsVisible())
                    { auto cover = other->mRect; if (other->mMinimised) cover.h=kHeadDp; slwin::excludeOverlay(clips,count,cover); }
            // The native window's own context menu also stays over its map.
            if (auto* menu = w->doc->GetElementById("menu"); menu && menu->IsVisible())
                slwin::excludeOverlay(clips,count,{menu->GetAbsoluteLeft()/density,menu->GetAbsoluteTop()/density,
                                                   menu->GetOffsetWidth()/density,menu->GetOffsetHeight()/density});
            for (size_t i=0; i<count; ++i)
            {
                const auto& c=clips[i];
                LLLocalClipRect clip(LLRect(S32(c.x),S32(rootHeight-c.y),S32(c.right()),S32(rootHeight-c.bottom())));
                LLUI::pushMatrix(); LLUI::translate(F32(view->getRect().mLeft),F32(view->getRect().mBottom));
                view->draw(); LLUI::popMatrix();
            }
        }
}

bool LLNewUIWindows::overlayToolTip(Rml::Element* hover, MASK mask)
{
    auto* w=windowOf(hover);
    if (!w) return false;
    bool isMap=false;
    for (auto* e=hover;e && e!=w->doc;e=e->GetParentNode()) if (e->GetId()=="nativeMap") { isMap=true; break; }
    if (!isMap) return false;
    if (auto* view=w->classicOverlay())
    { S32 x=0,y=0; LLUI::getInstance()->getMousePositionLocal(view,&x,&y); return view->handleToolTip(x,y,mask); }
    return false;
}
