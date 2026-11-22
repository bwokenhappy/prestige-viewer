// #703 The new interface's tool grid (see llnewuitools.h).
#include "llviewerprecompiledheaders.h"
#include "llnewuitools.h"

#include "llcommandmanager.h"
#include "llstartup.h"
#include "llnewuidocs.h"
#include "llnewuiwindow.h"
#include "llnewuiwindowlayout.h"
#include "lltoolbar.h"
#include "lltoolbarview.h"
#include "lltrans.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/Tween.h>
#include <algorithm>
#include <new>
#include <string>
#include <vector>

namespace
{
constexpr int    kMaxColumns = 12; // a wider set wraps into rows (the grid grows upwards)
constexpr int    kCellDp     = 44; // .tool: 40 dp and its 2 dp margins (llnewuikitstyle.cpp)
constexpr F64    kTipDelay   = 0.3; // camera mode's: a tip after the pointer rests this long
constexpr F64    kPollPeriod = 0.25; // enabled / running, as LLToolBar polls them
constexpr char   kLockedSetting[] = "UINewToolGridLocked";
constexpr double kGraceSec   = 0.3;  // the grid stays this long after the pointer leaves it
constexpr double kSlideSec   = 0.14; // its slide up

// One button as the document shows it (`id`: "tool-N", N its index).
struct ToolCell
{
    std::string id, icon = "none"; // a bound style is never empty
    bool        on = false, off = false;
    bool operator==(const ToolCell& o) const { return id == o.id && icon == o.icon && on == o.on && off == o.off; }
};

// What a button runs: its command, by id (the command manager owns it).
struct Tool
{
    LLCommandId command{ LLUUID::null };
    std::string name; // commands.xml name ("chat", "people", "inventory": native windows when on)
    std::string label, tooltip;
    bool        hasStop = false; // press / release (push to talk) instead of a click
};

struct State
{
    Rml::Context*         context = nullptr;
    Rml::ElementDocument* doc     = nullptr;
    Rml::DataModelHandle  model;
    std::vector<ToolCell> cells;
    std::vector<Tool>     tools;
    std::vector<LLUUID>   built; // the command list the grid was built from
    bool                  open = false, locked = false; // shown (hovered, locked or held); locked by its grip
    slwin::PullUp         pull;
    F64                   slideUntil = 0;               // captured every frame until the slide is done
    std::string           width = "44dp", tip, tipWhat, tipKey, tipClick; // never an empty width
    int                   tabBottom = -1;
    bool                  peek = false; // open as if hovered (event API)
    int                   held = -1; // the tool being held (its stop function pending)
    bool                  shown = false; // logged in, with commands to show
    F64                   nextPoll = 0, tipSince = 0;
    int                   bottom = -1, tipCell = -1;
    bool                  changed = true;
};
State* gState = nullptr;

LLUICtrl::commit_callback_t* commitFunction(const std::string& name)
{
    return name.empty() ? nullptr : LLUICtrl::CommitCallbackRegistry::getValue(name);
}

// LLToolBar::createButton's bindings, without a button: the registry's function with the command's
// parameters. The handlers named in commands.xml do not use the control argument (null here).
bool commandEnabled(const LLCommand& c)
{
    if (c.isEnabledFunctionName().empty())
        return true;
    LLUICtrl::enable_callback_t* f = LLUICtrl::EnableCallbackRegistry::getValue(c.isEnabledFunctionName());
    return !f || (*f)(nullptr, c.isEnabledParameters()); // unknown: enabled (LLUICtrl's default handler)
}

bool commandRunning(const LLCommand& c)
{
    if (c.isRunningFunctionName().empty())
        return false;
    LLUICtrl::enable_callback_t* f = LLUICtrl::EnableCallbackRegistry::getValue(c.isRunningFunctionName());
    return f && (*f)(nullptr, c.isRunningParameters());
}

void execute(const std::string& function, const LLSD& parameters)
{
    if (LLUICtrl::commit_callback_t* f = commitFunction(function))
        (*f)(nullptr, parameters);
    else if (!function.empty())
        LL_WARNS("RmlUi") << "Tool grid: no callback registered for " << function << LL_ENDL;
}

// Pinned inventory items (gmdxtoolbarpins.cpp): their handlers need the classic button (folder and
// "ask" pins open a menu on it, the name / tooltip / icon are pushed onto it), so they stay on the
// classic toolbars.
bool isPin(const LLCommand& c)
{
    return c.executeFunctionName().rfind("GMDX.ToolbarPin", 0) == 0;
}

// The commands on the person's classic toolbars (bottom, left, right, top). Empty toolbars, an
// empty grid (hidden): the person chose that; the command surface still opens everything.
std::vector<LLUUID> wantedCommands()
{
    std::vector<LLUUID> out;
    LLCommandManager&   mgr = LLCommandManager::instance();
    if (gToolBarView)
        for (LLToolBarEnums::EToolBarLocation where :
             { LLToolBarEnums::TOOLBAR_BOTTOM, LLToolBarEnums::TOOLBAR_LEFT, LLToolBarEnums::TOOLBAR_RIGHT, LLToolBarEnums::TOOLBAR_TOP })
            if (LLToolBar* bar = gToolBarView->getToolbar(where))
                for (const LLCommandId& id : bar->getCommandsList())
                    if (LLCommand* c = mgr.getCommand(id); c && !isPin(*c) && std::find(out.begin(), out.end(), id.uuid()) == out.end())
                        out.push_back(id.uuid());
    return out;
}

void rebuild(State* s, const std::vector<LLUUID>& ids)
{
    s->built = ids;
    s->tools.clear();
    s->cells.clear();
    for (const LLUUID& uuid : ids)
    {
        LLCommand* c = LLCommandManager::instance().getCommand(LLCommandId(uuid));
        if (!c)
            continue;
        Tool t;
        t.command = c->id();
        t.name    = c->name();
        t.label   = LLTrans::getString(c->labelRef());
        t.tooltip = LLTrans::getString(c->tooltipRef());
        t.hasStop = !c->executeStopFunctionName().empty();
        ToolCell cell;
        cell.id   = "tool-" + std::to_string(s->tools.size());
        // The icon in the text colour through its alpha (llimage's solid mode, image-color).
        cell.icon = "llimage(" + c->icon() + " solid)";
        s->tools.push_back(std::move(t));
        s->cells.push_back(std::move(cell));
    }
    const int cols = std::clamp(int(s->tools.size()), 1, kMaxColumns);
    s->width       = std::to_string(cols * kCellDp) + "dp";
    s->held        = -1;
    s->tipCell     = -1;
    s->model.DirtyVariable("tools");
    s->model.DirtyVariable("width");
    s->changed = true;
}

// The native window a command opens while the native windows are on (UINewWindowsDev), else "".
const char* nativeWindow(const Tool& t)
{
    if (!LLNewUIWindows::instance().active())
        return "";
    if (t.name == "chat")
        return "chat";
    if (t.name == "people")
        return "people";
    if (t.name == "inventory")
        return "inventory";
    return "";
}

// Enabled and running, from the commands' own functions; dirty only what changed.
void poll(State* s)
{
    bool any = false;
    for (size_t i = 0; i < s->tools.size(); ++i)
    {
        LLCommand* c = LLCommandManager::instance().getCommand(s->tools[i].command);
        if (!c)
            continue;
        const bool  off    = !commandEnabled(*c);
        const char* native = nativeWindow(s->tools[i]);
        const bool  on     = !off && (*native ? LLNewUIWindows::instance().isOpen(native) : commandRunning(*c));
        if (off != s->cells[i].off || on != s->cells[i].on)
        {
            s->cells[i].off = off;
            s->cells[i].on  = on;
            any             = true;
        }
    }
    if (any)
    {
        s->model.DirtyVariable("tools");
        s->changed = true;
    }
}

const Tool* toolAt(State* s, int i, LLCommand** command)
{
    if (!s || i < 0 || i >= int(s->tools.size()))
        return nullptr;
    *command = LLCommandManager::instance().getCommand(s->tools[size_t(i)].command);
    return *command ? &s->tools[size_t(i)] : nullptr;
}

void run(int i)
{
    LLCommand* c = nullptr;
    const Tool* t = toolAt(gState, i, &c);
    if (!t || t->hasStop) // held commands act on press / release
        return;
    if (const char* native = nativeWindow(*t); *native)
    {
        LLNewUIWindows::instance().toggle(native);
        gState->nextPoll = 0;
        return;
    }
    if (commandEnabled(*c)) // LLToolBarButton::callIfEnabled
        execute(c->executeFunctionName(), c->executeParameters());
    gState->nextPoll = 0; // its running state, now
}

void press(int i)
{
    LLCommand* c = nullptr;
    const Tool* t = toolAt(gState, i, &c);
    if (!t || !t->hasStop || !commandEnabled(*c))
        return;
    LLNewUITools::instance().releaseHeld(); // one held command at a time
    execute(c->executeFunctionName(), c->executeParameters());
    gState->held     = i;
    gState->nextPoll = 0;
}

void setTip(State* s, const std::string& tip, const std::string& what, const std::string& key, const std::string& click)
{
    auto set = [s](std::string& field, const std::string& value, const char* variable)
    {
        if (field != value)
        {
            field = value;
            s->model.DirtyVariable(variable);
            s->changed = true;
        }
    };
    set(s->tip, tip, "tip");
    set(s->tipWhat, what, "tipWhat");
    set(s->tipKey, key, "tipKey");
    set(s->tipClick, click, "tipClick");
}

// The button under the pointer explains itself after the pointer has rested (camera mode's tips):
// its name, its tooltip, and why it is greyed or how it is used. Placed over the button, inside
// the window.
void updateTip(State* s, float density)
{
    int           cell = -1;
    Rml::Element* el   = nullptr;
    for (Rml::Element* e = s->context->GetHoverElement(); e && e->GetOwnerDocument() == s->doc; e = e->GetParentNode())
        if (e->GetId().rfind("tool-", 0) == 0)
        {
            cell = std::atoi(e->GetId().c_str() + 5);
            el   = e;
            break;
        }
    if (!s->open || s->held >= 0)
        cell = -1;
    const F64 now = LLTimer::getTotalSeconds();
    if (cell != s->tipCell)
    {
        s->tipCell  = cell;
        s->tipSince = now;
    }
    if (cell < 0 || cell >= int(s->tools.size()) || now - s->tipSince < kTipDelay)
    {
        setTip(s, std::string(), std::string(), std::string(), std::string());
        if (cell >= 0)
            s->changed = true; // keep capturing until the delay has run
        return;
    }
    const Tool&     t    = s->tools[size_t(cell)];
    const ToolCell& c    = s->cells[size_t(cell)];
    std::string     what = t.tooltip == t.label ? std::string() : t.tooltip;
    if (c.off)
        setTip(s, t.label, what, std::string(), "Not available now");
    else if (t.hasStop)
        setTip(s, t.label, what, "Hold", "to keep it on, let go to stop");
    else
        setTip(s, t.label, what, std::string(), std::string());
    Rml::Element* wrap = s->doc->GetElementById("tipwrap");
    if (!wrap || !el || density <= 0.f)
        return;
    const float rootW = float(s->context->GetDimensions().x) / density;
    const float rootH = float(s->context->GetDimensions().y) / density;
    const float cx    = (el->GetAbsoluteLeft() + el->GetOffsetWidth() * 0.5f) / density;
    // above the grid's edge, not over its other buttons
    Rml::Element* grid = s->doc->GetElementById("tools");
    const float top   = (grid ? grid->GetAbsoluteTop() : el->GetAbsoluteTop()) / density;
    const float left  = std::clamp(cx - 150.f, 8.f, std::max(8.f, rootW - 308.f));
    wrap->SetProperty("left", std::to_string(int(left)) + "dp");
    wrap->SetProperty("bottom", std::to_string(int(rootH - top + 8.f)) + "dp");
}
} // namespace

LLNewUITools& LLNewUITools::instance()
{
    static LLNewUITools tools;
    return tools;
}

bool LLNewUITools::active() const { return gState != nullptr; }

bool LLNewUITools::start(Rml::Context* context, const DocumentLoader& load)
{
    if (gState || !context)
        return gState != nullptr;
    State* s = new (std::nothrow) State();
    if (!s)
        return false;
    try
    {
        s->context = context;
        s->locked = gSavedSettings.getBOOL(kLockedSetting);
        s->open   = s->locked;
        s->pull.graceSec = kGraceSec;
        Rml::DataModelConstructor m = context->CreateDataModel("tools");
        if (!m)
            throw std::bad_alloc();
        if (auto c = m.RegisterStruct<ToolCell>())
        {
            c.RegisterMember("id", &ToolCell::id);
            c.RegisterMember("icon", &ToolCell::icon);
            c.RegisterMember("on", &ToolCell::on);
            c.RegisterMember("off", &ToolCell::off);
        }
        m.RegisterArray<std::vector<ToolCell>>();
        m.Bind("tools", &s->cells);
        m.Bind("open", &s->open);
        m.Bind("locked", &s->locked);
        m.Bind("width", &s->width);
        m.Bind("tip", &s->tip);
        m.Bind("tipWhat", &s->tipWhat);
        m.Bind("tipKey", &s->tipKey);
        m.Bind("tipClick", &s->tipClick);
        m.BindEventCallback("run", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            { if (!a.empty()) run(a[0].Get<int>()); });
        m.BindEventCallback("press", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            { if (!a.empty()) press(a[0].Get<int>()); });
        // The grip (or the tab): locked open, or back to opening while hovered.
        m.BindEventCallback("lock", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                if (!gState)
                                    return;
                                gState->locked = !gState->locked;
                                gSavedSettings.setBOOL(kLockedSetting, gState->locked);
                                gState->model.DirtyVariable("locked");
                                gState->changed = true;
                            });
        s->model = m.GetModelHandle();
    }
    catch (const std::bad_alloc&)
    {
        delete s;
        return false;
    }
    gState = s;
    s->doc = load(LLNewUIDocs::kToolsRml, "newui-tools.rml");
    if (!s->doc)
    {
        stop();
        return false;
    }
    rebuild(s, wantedCommands());
    poll(s);
    // Shown by sync() once logged in and there is something to show.
    LL_INFOS("RmlUi") << "New interface: tool grid ready (" << s->tools.size() << " commands"
                      << (s->locked ? ", locked open" : ", opens on hover") << ")" << LL_ENDL;
    return true;
}

bool LLNewUITools::reload(const DocumentLoader& load)
{
    State* s = gState;
    if (!s)
        return false;
    Rml::ElementDocument* fresh = load(LLNewUIDocs::kToolsRml, "newui-tools.rml");
    if (!fresh)
        return false; // keep the old look rather than lose the grid
    const bool shown = s->doc && s->doc->IsVisible();
    if (s->doc)
        s->doc->Close();
    s->doc = fresh;
    if (shown)
        s->doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    s->bottom    = -1;
    s->tabBottom = -1;
    s->shown     = shown;
    s->model.DirtyAllVariables();
    s->changed = true;
    return true;
}

void LLNewUITools::stop()
{
    State* s = gState;
    if (!s)
        return;
    releaseHeld();
    if (s->doc)
        s->doc->Close();
    if (s->context)
        s->context->RemoveDataModel("tools");
    delete s;
    gState = nullptr;
}

bool LLNewUITools::ownsDocument(const Rml::ElementDocument* doc) const
{
    return gState && doc && doc == gState->doc;
}

bool LLNewUITools::holding() const { return gState && gState->held >= 0; }

void LLNewUITools::setPeek(bool on)
{
    if (gState)
        gState->peek = on;
}

void LLNewUITools::releaseHeld()
{
    State* s = gState;
    if (!s || s->held < 0)
        return;
    const int i = s->held;
    s->held     = -1;
    // Always the stop function, even if the command was disabled meanwhile: a held microphone
    // must never stay on.
    if (i < int(s->tools.size()))
        if (LLCommand* c = LLCommandManager::instance().getCommand(s->tools[size_t(i)].command))
            execute(c->executeStopFunctionName(), c->executeStopParameters());
    s->nextPoll = 0;
    s->changed  = true;
}

namespace
{
// Over the grid or the tab (any element of the document but its body).
bool pointerOver(State* s)
{
    Rml::Element* hover = s->context ? s->context->GetHoverElement() : nullptr;
    return hover && hover != s->doc && hover->GetOwnerDocument() == s->doc;
}

// The grid slides up from the bottom edge and fades in (the kit's motion: short, ease-out).
void slideIn(State* s)
{
    Rml::Element* wrap = s->doc ? s->doc->GetElementById("toolwrap") : nullptr;
    if (!wrap || s->bottom < 0)
        return;
    const Rml::Tween tween(Rml::Tween::Cubic, Rml::Tween::Out);
    const Rml::Property from(float(s->bottom - 24), Rml::Unit::DP), to(float(s->bottom), Rml::Unit::DP);
    wrap->Animate("bottom", to, float(kSlideSec), tween, 1, false, 0.f, &from);
    const Rml::Property clear(0.f, Rml::Unit::NUMBER), solid(1.f, Rml::Unit::NUMBER);
    wrap->Animate("opacity", solid, float(kSlideSec), tween, 1, false, 0.f, &clear);
    s->slideUntil = LLTimer::getTotalSeconds() + kSlideSec + 0.05;
}
} // namespace

bool LLNewUITools::sync(int bottom, int tabBottom, float density)
{
    State* s = gState;
    if (!s)
        return false;
    if (s->held >= 0 && (!s->doc || !s->doc->IsVisible()))
        releaseHeld(); // hidden while held: let go
    if (bottom != s->bottom)
    {
        s->bottom = bottom;
        if (Rml::Element* wrap = s->doc->GetElementById("toolwrap"))
            wrap->SetProperty("bottom", std::to_string(bottom) + "dp");
        s->changed = true;
    }
    if (tabBottom != s->tabBottom)
    {
        s->tabBottom = tabBottom;
        if (Rml::Element* wrap = s->doc->GetElementById("tabwrap"))
            wrap->SetProperty("bottom", std::to_string(tabBottom) + "dp");
        s->changed = true;
    }
    const F64 now = LLTimer::getTotalSeconds();
    if (now >= s->nextPoll)
    {
        s->nextPoll = now + kPollPeriod;
        const std::vector<LLUUID> ids = wantedCommands(); // the person's toolbars may have changed
        if (ids != s->built)
            rebuild(s, ids);
        poll(s);
    }
    // Only in world (the toolbars load at login; nothing before) and with commands to show.
    const bool want = LLStartUp::getStartupState() >= STATE_STARTED && !s->tools.empty();
    if (want != s->shown)
    {
        s->shown = want;
        if (want)
            s->doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        else
            s->doc->Hide();
        s->changed = true;
    }
    // Open while the pointer is over the tab or the grid (and the grace time after), locked, or
    // while a tool is held (push to talk never hides under the pointer).
    {
        // The lock follows its setting (the grip writes it; settings / scripts may too).
        static LLCachedControl<bool> locked(gSavedSettings, kLockedSetting, false);
        if (bool(locked) != s->locked)
        {
            s->locked = locked;
            s->model.DirtyVariable("locked");
            s->changed = true;
        }
        const F64 t = LLTimer::getTotalSeconds();
        if (s->pull.update(s->shown && (pointerOver(s) || s->peek), s->locked, s->held >= 0, t))
        {
            s->open = s->pull.open;
            s->model.DirtyVariable("open");
            s->tipCell = -1;
            s->changed = true;
            if (s->open)
                slideIn(s);
        }
        if (s->pull.pending() || t < s->slideUntil)
            s->changed = true; // the grace time and the slide run on captured frames
    }
    updateTip(s, density);
    const bool changed = s->changed;
    s->changed         = false;
    return changed;
}
