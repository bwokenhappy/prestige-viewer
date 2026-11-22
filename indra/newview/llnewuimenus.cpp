// #703 The new interface without the classic menu bar (see llnewuimenus.h).
#include "llviewerprecompiledheaders.h"
#include "llnewuimenus.h"

#include "llbuycurrencyhtml.h"
#include "llfirstuse.h"
#include "llfloaterreg.h"
#include "llkeyboard.h"
#include "llmenugl.h"
#include "llnewui.h"
#include "llnewuistatusdocs.h"
#include "llstartup.h"
#include "llstatusbar.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"
#include "slkeycommands.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <algorithm>
#include <new>
#include <string>
#include <vector>

namespace
{
constexpr size_t kMaxRows  = 18; // rows shown at once; a longer menu scrolls (the wheel)
constexpr size_t kMaxDepth = 8;  // pages deep (the classic menus nest up to 4)

// One row as the document shows it.
struct MenuRow
{
    std::string label, hint;
    bool        check = false, on = false, off = false, branch = false, sep = false;
};

// What a row does.
struct Act
{
    enum class Type
    {
        None,
        Func,   // runs `fn` (closes the menu unless `keep`)
        Item,   // a classic menu item: its own commit (enabled only), as a click on it
        Branch, // a classic submenu: browse into it
        Page    // a page of this menu
    };
    Type                  type = Type::None;
    std::function<void()> fn;
    LLHandle<LLView>      view;
    LLNewUIMenus::Page    page = LLNewUIMenus::Page::None;
    bool                  keep = false; // a toggle that keeps the menu open (the status bar's parts)
};

// A page on the stack: which, and for Menus the classic menu shown (null: the menu bar).
struct Level
{
    LLNewUIMenus::Page page = LLNewUIMenus::Page::None;
    LLHandle<LLView>   menu; // a classic submenu (Menus); none: the menu bar itself
    std::string        title;
    bool               sub = false; // `menu` was set (gone: back to the top)
};

struct State
{
    Rml::Context*         context = nullptr;
    Rml::ElementDocument* doc     = nullptr;
    Rml::DataModelHandle  model;
    std::vector<MenuRow>  rows;    // the visible slice
    std::vector<MenuRow>  all;     // the page's rows
    std::vector<Act>      acts;    // aligned with `all`
    std::vector<Level>    stack;   // empty: closed
    std::string           title, more;
    bool                  canBack = false;
    size_t                offset  = 0;
    int                   right = 8, top = 40;
    bool                  changed = true;
};
State* gState = nullptr;

LLHandle<LLView> sRow;             // status_bar_container
bool             sRowHidden = false, sMenuBarHidden = false;

std::string cleanLabel(std::string label)
{
    while (!label.empty() && (label.back() == '.' || label.back() == ' '))
        label.pop_back();
    return label;
}

std::string keysOf(const LLMenuItemGL* item)
{
    return item->getAcceleratorKey() != KEY_NONE ? LLKeyboard::stringFromAccelerator(item->getAcceleratorMask(), item->getAcceleratorKey())
                                                 : std::string();
}

// A row for a classic menu item: its label, keys and state from its own callbacks.
void addItem(State* s, LLMenuItemGL* item, const std::string& label = std::string())
{
    if (!item)
        return;
    auto* call = dynamic_cast<LLMenuItemCallGL*>(item);
    if (call)
        call->buildDrawLabel();
    MenuRow r;
    r.label = label.empty() ? cleanLabel(item->getLabel()) : label;
    r.hint  = keysOf(item);
    r.off   = !item->getEnabled();
    if (dynamic_cast<LLMenuItemCheckGL*>(item))
    {
        r.check = true;
        r.on    = item->getValue().asBoolean();
    }
    Act a;
    a.type = Act::Type::Item;
    a.view = item->getHandle();
    s->all.push_back(std::move(r));
    s->acts.push_back(std::move(a));
}

// The menu bar's item `name` (anywhere in the tree).
LLMenuItemGL* menuItem(const char* name)
{
    return gMenuBarView ? gMenuBarView->findChild<LLMenuItemGL>(name, true) : nullptr;
}

void addFunc(State* s, const std::string& label, const std::string& hint, std::function<void()> fn)
{
    MenuRow r;
    r.label = label;
    r.hint  = hint;
    Act a;
    a.type = Act::Type::Func;
    a.fn   = std::move(fn);
    s->all.push_back(std::move(r));
    s->acts.push_back(std::move(a));
}

// A boolean setting as a toggle row.
void addToggle(State* s, const std::string& label, const char* control, bool keep)
{
    LLControlVariable* var = gSavedSettings.getControl(control);
    if (!var)
        return;
    MenuRow r;
    r.label = label;
    r.check = true;
    r.on    = var->getValue().asBoolean();
    Act a;
    a.type = Act::Type::Func;
    a.keep = keep;
    const std::string name = control;
    a.fn   = [name]() { gSavedSettings.setBOOL(name, !gSavedSettings.getBOOL(name)); };
    s->all.push_back(std::move(r));
    s->acts.push_back(std::move(a));
}

void addPage(State* s, const std::string& label, LLNewUIMenus::Page page, const LLHandle<LLView>& menu = LLHandle<LLView>())
{
    MenuRow r;
    r.label  = label;
    r.branch = true;
    Act a;
    a.type = menu.isDead() ? Act::Type::Page : Act::Type::Branch;
    a.page = page;
    a.view = menu;
    s->all.push_back(std::move(r));
    s->acts.push_back(std::move(a));
}

void addSep(State* s)
{
    if (s->all.empty() || s->all.back().sep)
        return; // no leading or doubled separators
    MenuRow r;
    r.sep = true;
    s->all.push_back(std::move(r));
    s->acts.push_back(Act());
}

// A classic menu's items, as its own menu shows them (visible ones; separators kept).
void addMenu(State* s, LLMenuGL* menu)
{
    if (!menu)
        return;
    for (U32 i = 0, n = menu->getItemCount(); i < n; ++i)
    {
        LLMenuItemGL* item = menu->getItem(S32(i));
        if (!item)
            continue;
        if (auto* call = dynamic_cast<LLMenuItemCallGL*>(item))
            call->buildDrawLabel(); // its visibility too
        if (!item->getVisible())
            continue;
        if (dynamic_cast<LLMenuItemSeparatorGL*>(item))
        {
            addSep(s);
            continue;
        }
        if (auto* branch = dynamic_cast<LLMenuItemBranchGL*>(item))
        {
            if (LLMenuGL* sub = branch->getBranch())
                addPage(s, cleanLabel(item->getLabel()), LLNewUIMenus::Page::Menus, sub->getHandle());
            continue;
        }
        if (dynamic_cast<LLMenuItemCallGL*>(item))
            addItem(s, item);
    }
    while (!s->all.empty() && s->all.back().sep)
    {
        s->all.pop_back();
        s->acts.pop_back();
    }
}

void build(State* s)
{
    s->all.clear();
    s->acts.clear();
    const Level& level = s->stack.back();
    switch (level.page)
    {
        case LLNewUIMenus::Page::More:
            addFunc(s, "Search commands", SLKeyCommands::instance().getBindingString("command_palette"), []() { LLNewUI::instance().togglePalette(); });
            addSep(s);
            addItem(s, menuItem("Preferences"), "Preferences");
            addPage(s, "Status bar", LLNewUIMenus::Page::StatusItems);
            addPage(s, "Browse the classic menus", LLNewUIMenus::Page::Menus);
            addSep(s);
            addItem(s, menuItem("Show Advanced Menu"), "Advanced menu");
            addItem(s, menuItem("Debug Mode"), "Develop menu");
            addSep(s);
            addItem(s, menuItem("Slipstream Help"));
            addItem(s, menuItem("Report Bug"));
            addItem(s, menuItem("About Second Life"));
            addSep(s);
            addItem(s, menuItem("Quit"));
            break;
        case LLNewUIMenus::Page::Balance:
            addFunc(s, "Buy L$", std::string(),
                    []()
                    {
                        LLBuyCurrencyHTML::openCurrencyFloater();
                        LLFirstUse::receiveLindens(false);
                    });
            addFunc(s, "Refresh the balance", std::string(), []() { LLStatusBar::sendMoneyBalanceRequest(); });
            addToggle(s, "Hide the balance", "ObscureBalanceInStatusBar", true);
            addSep(s);
            addFunc(s, "Shop (Marketplace)", std::string(), []() { LLFloaterReg::toggleInstanceOrBringToFront("marketplace"); });
            addItem(s, menuItem("transaction_log"), "Transaction log");
            break;
        case LLNewUIMenus::Page::StatusItems:
            addToggle(s, "Show the status bar", "UINewStatusBar", true);
            addSep(s);
            addToggle(s, "Parcel (rating, restrictions)", "UINewStatusParcel", true);
            addToggle(s, "Frame rate", "ShowStatusBarFPS", true);
            addToggle(s, "Ping and packet loss", "UINewStatusNet", true);
            addToggle(s, "Bandwidth", "ShowNetStats", true);
            addToggle(s, "Time", "UINewStatusTime", true);
            addToggle(s, "L$ balance", "UINewStatusBalance", true);
            addToggle(s, "Media", "UINewStatusMedia", true);
            addToggle(s, "Volume", "UINewStatusVolume", true);
            addToggle(s, "Graphics and camera presets", "UINewStatusPresets", true);
            addToggle(s, "Gesture sets", "GestureSetsShowStatusBarIcon", true);
            break;
        case LLNewUIMenus::Page::Menus:
            if (LLView* v = level.menu.get())
                addMenu(s, dynamic_cast<LLMenuGL*>(v));
            else if (gMenuBarView)
                addMenu(s, gMenuBarView); // the top level: Me, Avatar, Communicate, ...
            break;
        case LLNewUIMenus::Page::None:
            break;
    }
    s->title   = level.title;
    s->canBack = s->stack.size() > 1;
    s->offset  = std::min(s->offset, s->all.size() > kMaxRows ? s->all.size() - kMaxRows : size_t(0));
    s->rows.assign(s->all.begin() + std::ptrdiff_t(s->offset), s->all.begin() + std::ptrdiff_t(std::min(s->all.size(), s->offset + kMaxRows)));
    const size_t after = s->all.size() - std::min(s->all.size(), s->offset + kMaxRows);
    s->more            = after ? std::to_string(after) + " more below (scroll)" : (s->offset ? "Scroll up for more" : std::string());
    for (const char* v : { "rows", "title", "canBack", "more" })
        s->model.DirtyVariable(v);
    s->changed = true;
}

const char* titleOf(LLNewUIMenus::Page page)
{
    switch (page)
    {
        case LLNewUIMenus::Page::Balance: return "L$ balance";
        case LLNewUIMenus::Page::StatusItems: return "Status bar";
        case LLNewUIMenus::Page::Menus: return "Classic menus";
        default: return "";
    }
}

void push(State* s, LLNewUIMenus::Page page, const LLHandle<LLView>& menu, const std::string& title)
{
    if (s->stack.size() >= kMaxDepth)
        return;
    s->stack.push_back({ page, menu, title, !menu.isDead() });
    s->offset = 0;
    build(s);
}

void pick(int i)
{
    State* s = gState;
    if (!s || i < 0 || s->stack.empty())
        return;
    const size_t at = s->offset + size_t(i);
    if (at >= s->acts.size())
        return;
    const Act act   = s->acts[at]; // the lists are rebuilt below
    const std::string label = s->all[at].label;
    switch (act.type)
    {
        case Act::Type::Func:
            if (!act.keep)
                LLNewUIMenus::instance().close();
            act.fn();
            if (act.keep && gState && !gState->stack.empty())
                build(gState); // the toggles' new state
            break;
        case Act::Type::Item:
        {
            auto* item = dynamic_cast<LLMenuItemGL*>(act.view.get());
            LLNewUIMenus::instance().close();
            if (!item)
                break;
            if (auto* call = dynamic_cast<LLMenuItemCallGL*>(item))
                call->buildDrawLabel();
            if (item->getEnabled())
                item->onCommit();
            else
                LL_INFOS("RmlUi") << "New interface: menu item not available now: " << label << LL_ENDL;
            break;
        }
        case Act::Type::Branch:
            if (!act.view.isDead())
                push(s, LLNewUIMenus::Page::Menus, act.view, label);
            break;
        case Act::Type::Page:
            push(s, act.page, LLHandle<LLView>(), titleOf(act.page));
            break;
        case Act::Type::None:
            break;
    }
}

void place(State* s)
{
    if (Rml::Element* menu = s->doc->GetElementById("menu"))
    {
        menu->SetProperty("right", std::to_string(s->right) + "dp");
        menu->SetProperty("top", std::to_string(s->top) + "dp");
    }
}
} // namespace

LLNewUIMenus& LLNewUIMenus::instance()
{
    static LLNewUIMenus menus;
    return menus;
}

bool LLNewUIMenus::active() const { return gState != nullptr; }

bool LLNewUIMenus::start(Rml::Context* context, const DocumentLoader& load)
{
    if (gState || !context)
        return gState != nullptr;
    State* s = new (std::nothrow) State();
    if (!s)
        return false;
    try
    {
        s->context                  = context;
        Rml::DataModelConstructor m = context->CreateDataModel("smenu");
        if (!m)
            throw std::bad_alloc();
        if (auto r = m.RegisterStruct<MenuRow>())
        {
            r.RegisterMember("label", &MenuRow::label);
            r.RegisterMember("hint", &MenuRow::hint);
            r.RegisterMember("check", &MenuRow::check);
            r.RegisterMember("on", &MenuRow::on);
            r.RegisterMember("off", &MenuRow::off);
            r.RegisterMember("branch", &MenuRow::branch);
            r.RegisterMember("sep", &MenuRow::sep);
        }
        m.RegisterArray<std::vector<MenuRow>>();
        m.Bind("rows", &s->rows);
        m.Bind("title", &s->title);
        m.Bind("canBack", &s->canBack);
        m.Bind("more", &s->more);
        m.BindEventCallback("pick", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            { if (!a.empty()) pick(a[0].Get<int>()); });
        m.BindEventCallback("close", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { LLNewUIMenus::instance().close(); });
        m.BindEventCallback("back",
                            [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                State* st = gState;
                                if (st && st->stack.size() > 1)
                                {
                                    st->stack.pop_back();
                                    st->offset = 0;
                                    build(st);
                                }
                            });
        m.BindEventCallback("scroll",
                            [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                State* st = gState;
                                if (!st || a.empty() || st->stack.empty() || st->all.size() <= kMaxRows)
                                    return;
                                const float  delta = a[0].Get<float>();
                                const size_t last  = st->all.size() - kMaxRows;
                                const size_t step  = 3;
                                st->offset = delta > 0 ? std::min(last, st->offset + step) : (st->offset > step ? st->offset - step : 0);
                                build(st);
                            });
        s->model = m.GetModelHandle();
    }
    catch (const std::bad_alloc&)
    {
        delete s;
        return false;
    }
    gState = s;
    s->doc = load(LLNewUIStatusDocs::kStatusMenuRml, "newui-smenu.rml");
    if (!s->doc)
    {
        stop();
        return false;
    }
    return true;
}

bool LLNewUIMenus::reload(const DocumentLoader& load)
{
    State* s = gState;
    if (!s)
        return false;
    Rml::ElementDocument* fresh = load(LLNewUIStatusDocs::kStatusMenuRml, "newui-smenu.rml");
    if (!fresh)
        return false;
    const bool shown = s->doc && s->doc->IsVisible();
    if (s->doc)
        s->doc->Close();
    s->doc = fresh;
    place(s);
    if (shown)
        s->doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    s->model.DirtyAllVariables();
    s->changed = true;
    return true;
}

void LLNewUIMenus::stop()
{
    State* s = gState;
    if (!s)
        return;
    if (s->doc)
        s->doc->Close();
    if (s->context)
        s->context->RemoveDataModel("smenu");
    delete s;
    gState = nullptr;
    hideClassicRow(false);
}

bool LLNewUIMenus::ownsDocument(const Rml::ElementDocument* doc) const
{
    return gState && doc && doc == gState->doc;
}

void LLNewUIMenus::toggle(Page page, int right, int top)
{
    State* s = gState;
    if (!s || !s->doc)
        return;
    if (!s->stack.empty() && s->stack.front().page == page)
    {
        close();
        return;
    }
    s->stack.clear();
    s->offset = 0;
    s->right  = right;
    s->top    = top;
    s->stack.push_back({ page, LLHandle<LLView>(), titleOf(page), false });
    build(s);
    place(s);
    s->doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    s->doc->PullToFront(); // above the native windows (a window raised since would cover it)
    s->changed = true;
}

void LLNewUIMenus::close()
{
    State* s = gState;
    if (!s || s->stack.empty())
        return;
    s->stack.clear();
    if (s->doc)
        s->doc->Hide();
    s->changed = true;
}

LLNewUIMenus::Page LLNewUIMenus::openPage() const
{
    return gState && !gState->stack.empty() ? gState->stack.front().page : Page::None;
}

bool LLNewUIMenus::sync()
{
    State* s = gState;
    if (!s)
        return false;
    // In world only: the login screen keeps its classic menus.
    hideClassicRow(LLStartUp::getStartupState() >= STATE_STARTED);
    // A classic menu shown meanwhile went away (a rebuilt branch): back to the top.
    if (!s->stack.empty() && s->stack.back().page == Page::Menus && s->stack.size() > 1 && s->stack.back().sub && s->stack.back().menu.isDead())
    {
        s->stack.resize(1);
        build(s);
    }
    const bool changed = s->changed;
    s->changed         = false;
    return changed;
}

void LLNewUIMenus::hideClassicRow(bool hide)
{
    if (sRow.isDead() && hide)
        if (LLView* root = LLUI::getInstance()->getRootView())
            if (LLView* row = root->findChildView("status_bar_container", true))
                sRow = row->getHandle();
    // The container, not only its children: it takes the mouse (mouse_opaque) and the layout
    // stack gives its height back to the world panel when it is hidden.
    if (LLView* row = sRow.get())
    {
        if (hide && row->getVisible())
        {
            row->setVisible(false);
            sRowHidden = true;
        }
        else if (!hide && sRowHidden)
        {
            row->setVisible(true);
            sRowHidden = false;
            if (gStatusBar)
                gStatusBar->restorePulldowns(); // the pull-downs the new status bar borrowed
        }
    }
    // The menu bar itself: hidden, its Alt key and jump keys go quiet; enabled, its accelerators
    // still run (LLMenuGL::handleAcceleratorKey only asks for enabled).
    if (gMenuBarView)
    {
        if (hide && gMenuBarView->getVisible())
        {
            gMenuBarView->setVisible(false);
            sMenuBarHidden = true;
        }
        else if (!hide && sMenuBarHidden)
        {
            gMenuBarView->setVisible(true);
            sMenuBarHidden = false;
        }
    }
}

bool LLNewUIMenus::classicRowHidden()
{
    return sRowHidden;
}
