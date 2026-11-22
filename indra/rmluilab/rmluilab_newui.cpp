// #703 slrmluilab --newui (see rmluilab_newui.h): the new interface's documents over sample data.
#include "rmluilab_newui.h"

#include "llnewuidocs.h"
#include "llnewuikitstyle.h"
#include "llnewuiwindowdocs.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/DecorationTypes.h>
#include <RmlUi/Core/Decorator.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{
std::string gIcons;

// llimage(<name> [solid]): one stretched quad of the image's PNG, coloured by image-color.
class LabImageDecorator final : public Rml::Decorator
{
public:
    bool init(Rml::Texture texture) { return AddTexture(texture) >= 0; }
    Rml::DecoratorDataHandle GenerateElementData(Rml::Element* element, Rml::BoxArea paint_area) const override
    {
        const Rml::ComputedValues& computed = element->GetComputedValues();
        const Rml::RenderBox       box      = element->GetRenderBox(paint_area);
        Rml::Mesh                  mesh;
        Rml::MeshUtilities::GenerateQuad(mesh, box.GetFillOffset(), box.GetFillSize(), computed.image_color().ToPremultiplied(computed.opacity()),
                                         { 0.f, 0.f }, { 1.f, 1.f });
        auto* geometry = new Rml::Geometry(element->GetRenderManager()->MakeGeometry(std::move(mesh)));
        return reinterpret_cast<Rml::DecoratorDataHandle>(geometry);
    }
    void ReleaseElementData(Rml::DecoratorDataHandle data) const override { delete reinterpret_cast<Rml::Geometry*>(data); }
    void RenderElement(Rml::Element* element, Rml::DecoratorDataHandle data) const override
    {
        reinterpret_cast<Rml::Geometry*>(data)->Render(element->GetAbsoluteOffset(Rml::BoxArea::Border), GetTexture());
    }
};

class LabImageInstancer final : public Rml::DecoratorInstancer
{
    Rml::PropertyId mImage, mMode;

public:
    LabImageInstancer()
    {
        mImage = RegisterProperty("image", "").AddParser("string").GetId();
        mMode  = RegisterProperty("mode", "scale").AddParser("keyword", "scale, solid").GetId();
        RegisterShorthand("decorator", "image, mode", Rml::ShorthandType::FallThrough);
    }
    Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String&, const Rml::PropertyDictionary& properties,
                                                     const Rml::DecoratorInstancerInterface& itf) override
    {
        const Rml::Property* image = properties.GetProperty(mImage);
        const Rml::Property* mode  = properties.GetProperty(mMode);
        // As the viewer's instancer: an empty or unknown name is refused (RmlUi warns, --strict
        // fails), so the lab catches what the viewer would log.
        if (!image || image->Get<Rml::String>().empty())
            return nullptr;
        const bool  solid = mode && mode->Get<int>() == 1;
        std::string path  = gIcons + "/" + image->Get<Rml::String>() + (solid ? ".png" : ".color.png");
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return nullptr;
        auto d = Rml::MakeShared<LabImageDecorator>();
        if (!d->init(itf.GetTexture(path)))
            return nullptr;
        return d;
    }
};

// ---- sample models, bound once; scenes change their values ----
struct Row
{
    std::string kind, label, path, shortcut;
    bool        check = false, on = false, off = false;
};
struct ToolCell
{
    std::string id, icon;
    bool        on = false, off = false;
};
struct Item // actions, menu rows
{
    std::string label;
    bool        on = true;
};
struct Tab
{
    std::string name;
    int         unread   = 0;
    bool        closable = false;
};
struct Line
{
    std::string from, text, time;
    bool        sys = false, hist = false, emote = false, self = false;
    std::string rich, sender; bool groupHead = true;
};
struct PRow
{
    std::string name, sub;
    bool        online = false, used = false, selected = false, group = false;
    std::string status, statusTip;
    bool typing = false, speaking = false, voice = false, friendFlag = false, muted = false, chatRange = false, regionFlag = false, parcelFlag = false, sitting = false, payment = false, notes = false;
};
struct IRow
{
    std::string label, deco, twist;
    int         indent = 0;
    bool        used = false, selected = false, worn = false;
};

struct ChatModel
{
    std::string              title = "Conversations", peer = "Nearby chat", olderText = "12 earlier - scroll up", emptyText, typing, line,
                             hint = "Say something nearby";
    bool                     active = true, single = false, older = true, stamps = true;
    bool grouped=false, pickerOpen=false;
    float thumbTop=0,thumbH=0;
    std::string pickerQuery;
    std::vector<std::string> pickerLabels, pickerStatuses;
    int pickerFriendCount = 0, pickerSelected = 0;
    int                      current = 0;
    std::vector<Tab>         tabs;
    std::vector<Line>        msgs;
    std::vector<Item>        acts;
    std::vector<std::string> suggest;
};
struct PeopleModel
{
    std::string              title = "People", filter, filterHint = "Filter friends", countText = "7 friends, 4 online", emptyText;
    bool                     active = false, menuOpen = false;
    bool nearby=false, rangeLimited=false;
    float range=162.f;
    int                      view = 0, count = 7;
    float                    thumbTop = 0, thumbH = 0, menuX = 0, menuY = 0;
    std::vector<std::string> views = { "Friends", "Nearby", "Groups", "Recent", "Blocked" };
    std::vector<PRow>        rows;
    std::vector<Item>        acts, menu;
};
struct InvModel
{
    std::string              title = "Inventory", filter, status = "11 rows", emptyText;
    bool                     active = false, menuOpen = false;
    int                      view = 0, count = 11;
    float                    thumbTop = 0, thumbH = 0, menuX = 0, menuY = 0;
    std::vector<std::string> views = { "All", "Recent", "Worn" };
    std::vector<IRow>        rows;
    std::vector<Item>        acts, menu;
};

struct Model
{
    std::string           location = "Sunshine Bay - Bay City", hint = "Search or run a command", key = "Ctrl+K";
    bool navOpen=false;
    int navMode=1, navSelected=-1;
    std::string address;
    std::vector<std::string> navLabels;
    std::vector<int> navEnabled;
    std::string           query = "snap";
    int                   selected = 0;
    std::vector<Row>      rows;
    std::vector<ToolCell> tools;
    bool                  open = true, locked = false;
    std::string           width = "44dp", tip, tipWhat, tipKey, tipClick;
    ChatModel             chat;
    PeopleModel           people;
    InvModel              inv;
    Rml::DataModelHandle  bar, palette, toolsModel, chatModel, peopleModel, invModel;
};
Model gModel;

// The default toolbars' commands (app_settings/toolbars.xml), as the grid shows them to a new user.
struct SampleTool
{
    const char *icon, *label, *tooltip;
};
const SampleTool kTools[] = {
    { "Command_Chat_Icon", "Nearby Chat", "Chat with people nearby using text" },
    { "Command_Speak_Icon", "Speak", "Speak with people nearby using your microphone" },
    { "Command_People_Icon", "People", "Friends, groups, and nearby people" },
    { "Command_Profile_Icon", "Profile", "Edit or view your profile" },
    { "Command_Map_Icon", "Map", "Map of the world" },
    { "Command_Move_Icon", "Walk / run / fly", "Moving your avatar" },
    { "Command_View_Icon", "Camera", "Changing camera angle" },
    { "Command_Snapshot_Icon", "Snapshot", "Open the snapshot floater" },
    { "Command_HowTo_Icon", "Guidebook", "How to do common tasks" },
    { "Command_Avatar_Icon", "Avatar Welcome Pack", "Choose a complete avatar" },
    { "Command_Appearance_Icon", "Outfits", "Change your avatar" },
    { "Command_Inventory_Icon", "Inventory", "View and use your belongings" },
    { "Command_Search_Icon", "Search", "Find places, events, people" },
    { "Command_Places_Icon", "Places", "Places you've saved" },
    { "Command_Destinations_Icon", "Destinations", "Destinations of interest" },
    { "Command_MiniMap_Icon", "Mini-map", "Show nearby people" },
    { "Command_Environments_Icon", "My Environments", "My Environments" },
    { "Command_Stats_Icon", "Statistics", "Statistics like FPS, network usage and sim performance" },
    { "Command_Performance_Icon", "Graphics speed", "Improve graphics speed" },
    { "Command_QuickPrefs_Icon", "Quick Prefs", "Quick Preferences" },
    { "Command_Photo_Studio_Icon", "Photo Studio", "Photo Studio" },
};
constexpr int kToolCount = int(sizeof(kTools) / sizeof(kTools[0]));

void noop(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {}

void registerItems(Rml::DataModelConstructor& m)
{
    if (auto s = m.RegisterStruct<Item>())
    {
        s.RegisterMember("label", &Item::label);
        s.RegisterMember("on", &Item::on);
    }
    m.RegisterArray<std::vector<Item>>();
    m.RegisterArray<std::vector<std::string>>();
}

void bindModels(Rml::Context* context)
{
    Model& m = gModel;
    if (auto b = context->CreateDataModel("bar"))
    {
        b.RegisterArray<std::vector<std::string>>(); b.RegisterArray<std::vector<int>>();
        b.Bind("navOpen", &m.navOpen); b.Bind("navMode", &m.navMode); b.Bind("navSelected", &m.navSelected); b.Bind("address", &m.address);
        b.Bind("navLabels", &m.navLabels); b.Bind("navEnabled", &m.navEnabled);
        for (const char* e : { "quickAction", "home", "navigation", "navClose", "navGo", "navPick" }) b.BindEventCallback(e,noop);
        b.Bind("location", &m.location);
        b.Bind("hint", &m.hint);
        b.Bind("key", &m.key);
        b.BindEventCallback("open", noop);
        m.bar = b.GetModelHandle();
    }
    if (auto p = context->CreateDataModel("palette"))
    {
        p.Bind("query", &m.query);
        p.Bind("selected", &m.selected);
        if (auto r = p.RegisterStruct<Row>())
        {
            r.RegisterMember("kind", &Row::kind);
            r.RegisterMember("label", &Row::label);
            r.RegisterMember("path", &Row::path);
            r.RegisterMember("shortcut", &Row::shortcut);
            r.RegisterMember("check", &Row::check);
            r.RegisterMember("on", &Row::on);
            r.RegisterMember("off", &Row::off);
        }
        p.RegisterArray<std::vector<Row>>();
        p.Bind("rows", &m.rows);
        p.BindEventCallback("run", noop);
        p.BindEventCallback("close", noop);
        m.palette = p.GetModelHandle();
    }
    if (auto t = context->CreateDataModel("tools"))
    {
        if (auto c = t.RegisterStruct<ToolCell>())
        {
            c.RegisterMember("id", &ToolCell::id);
            c.RegisterMember("icon", &ToolCell::icon);
            c.RegisterMember("on", &ToolCell::on);
            c.RegisterMember("off", &ToolCell::off);
        }
        t.RegisterArray<std::vector<ToolCell>>();
        t.Bind("tools", &m.tools);
        t.Bind("open", &m.open);
        t.Bind("locked", &m.locked);
        t.Bind("width", &m.width);
        t.Bind("tip", &m.tip);
        t.Bind("tipWhat", &m.tipWhat);
        t.Bind("tipKey", &m.tipKey);
        t.Bind("tipClick", &m.tipClick);
        for (const char* e : { "run", "press", "lock" })
            t.BindEventCallback(e, noop);
        m.toolsModel = t.GetModelHandle();
    }
    if (auto c = context->CreateDataModel("chat"))
    {
        ChatModel& x = m.chat;
        if (auto t = c.RegisterStruct<Tab>())
        {
            t.RegisterMember("name", &Tab::name);
            t.RegisterMember("unread", &Tab::unread);
            t.RegisterMember("closable", &Tab::closable);
        }
        c.RegisterArray<std::vector<Tab>>();
        if (auto l = c.RegisterStruct<Line>())
        {
            l.RegisterMember("from", &Line::from);
            l.RegisterMember("text", &Line::text);
            l.RegisterMember("rich", &Line::rich); l.RegisterMember("sender", &Line::sender);
            l.RegisterMember("groupHead", &Line::groupHead);
            l.RegisterMember("time", &Line::time);
            l.RegisterMember("sys", &Line::sys);
            l.RegisterMember("hist", &Line::hist);
            l.RegisterMember("emote", &Line::emote);
            l.RegisterMember("self", &Line::self);
        }
        c.RegisterArray<std::vector<Line>>();
        registerItems(c);
        c.BindEventCallback("pickerWheel", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) { event.StopPropagation(); });
        c.Bind("grouped", &x.grouped); c.Bind("pickerOpen", &x.pickerOpen);
        c.Bind("pickerStatuses", &x.pickerStatuses); c.Bind("pickerFriendCount", &x.pickerFriendCount); c.Bind("pickerSelected", &x.pickerSelected);
        c.Bind("pickerQuery", &x.pickerQuery); c.Bind("pickerLabels", &x.pickerLabels);
        c.Bind("thumbTop", &x.thumbTop); c.Bind("thumbH", &x.thumbH);
        c.Bind("title", &x.title);
        c.Bind("active", &x.active);
        c.Bind("tabs", &x.tabs);
        c.Bind("current", &x.current);
        c.Bind("single", &x.single);
        c.Bind("peer", &x.peer);
        c.Bind("acts", &x.acts);
        c.Bind("msgs", &x.msgs);
        c.Bind("older", &x.older);
        c.Bind("olderText", &x.olderText);
        c.Bind("emptyText", &x.emptyText);
        c.Bind("stamps", &x.stamps);
        c.Bind("typing", &x.typing);
        c.Bind("line", &x.line);
        c.Bind("hint", &x.hint);
        c.Bind("suggest", &x.suggest);
        for (const char* e : { "pick", "closeTab", "act", "scroll", "send", "complete", "historyPage", "layout", "pickerOutside", "newConversation", "chooseConversation", "searchResident", "chatLink" })
            c.BindEventCallback(e, noop);
        m.chatModel = c.GetModelHandle();
    }
    if (auto p = context->CreateDataModel("people"))
    {
        PeopleModel& x = m.people;
        if (auto r = p.RegisterStruct<PRow>())
        {
            r.RegisterMember("name", &PRow::name);
            r.RegisterMember("sub", &PRow::sub);
            r.RegisterMember("typing", &PRow::typing);
            r.RegisterMember("speaking", &PRow::speaking);
            r.RegisterMember("voice", &PRow::voice);
            r.RegisterMember("friendFlag", &PRow::friendFlag);
            r.RegisterMember("muted", &PRow::muted);
            r.RegisterMember("chatRange", &PRow::chatRange);
            r.RegisterMember("regionFlag", &PRow::regionFlag);
            r.RegisterMember("parcelFlag", &PRow::parcelFlag);
            r.RegisterMember("sitting", &PRow::sitting);
            r.RegisterMember("payment", &PRow::payment);
            r.RegisterMember("notes", &PRow::notes);
            r.RegisterMember("status", &PRow::status); r.RegisterMember("statusTip", &PRow::statusTip);
            r.RegisterMember("online", &PRow::online);
            r.RegisterMember("used", &PRow::used);
            r.RegisterMember("selected", &PRow::selected);
            r.RegisterMember("group", &PRow::group);
        }
        p.RegisterArray<std::vector<PRow>>();
        registerItems(p);
        p.Bind("nearby", &x.nearby); p.Bind("rangeLimited", &x.rangeLimited); p.Bind("range", &x.range);
        p.Bind("title", &x.title);
        p.Bind("active", &x.active);
        p.Bind("views", &x.views);
        p.Bind("view", &x.view);
        p.Bind("filter", &x.filter);
        p.Bind("filterHint", &x.filterHint);
        p.Bind("rows", &x.rows);
        p.Bind("count", &x.count);
        p.Bind("countText", &x.countText);
        p.Bind("emptyText", &x.emptyText);
        p.Bind("thumbTop", &x.thumbTop);
        p.Bind("thumbH", &x.thumbH);
        p.Bind("acts", &x.acts);
        p.Bind("menu", &x.menu);
        p.Bind("menuOpen", &x.menuOpen);
        p.Bind("menuX", &x.menuX);
        p.Bind("menuY", &x.menuY);
        for (const char* e : { "setView", "scroll", "press", "activate", "rowIM", "act", "more", "menuRun", "bodyDown", "rangeChanged", "rangeLimit", "mapPress", "mapHover", "mapDouble", "mapScroll" })
            p.BindEventCallback(e, noop);
        m.peopleModel = p.GetModelHandle();
    }
    if (auto i = context->CreateDataModel("inv"))
    {
        InvModel& x = m.inv;
        if (auto r = i.RegisterStruct<IRow>())
        {
            r.RegisterMember("label", &IRow::label);
            r.RegisterMember("deco", &IRow::deco);
            r.RegisterMember("twist", &IRow::twist);
            r.RegisterMember("indent", &IRow::indent);
            r.RegisterMember("used", &IRow::used);
            r.RegisterMember("selected", &IRow::selected);
            r.RegisterMember("worn", &IRow::worn);
        }
        i.RegisterArray<std::vector<IRow>>();
        registerItems(i);
        i.Bind("title", &x.title);
        i.Bind("active", &x.active);
        i.Bind("views", &x.views);
        i.Bind("view", &x.view);
        i.Bind("filter", &x.filter);
        i.Bind("rows", &x.rows);
        i.Bind("count", &x.count);
        i.Bind("status", &x.status);
        i.Bind("emptyText", &x.emptyText);
        i.Bind("thumbTop", &x.thumbTop);
        i.Bind("thumbH", &x.thumbH);
        i.Bind("acts", &x.acts);
        i.Bind("menu", &x.menu);
        i.Bind("menuOpen", &x.menuOpen);
        i.Bind("menuX", &x.menuX);
        i.Bind("menuY", &x.menuY);
        for (const char* e : { "setView", "scroll", "press", "twist", "activate", "act", "more", "menuRun", "bodyDown", "newWindow" })
            i.BindEventCallback(e, noop);
        m.invModel = i.GetModelHandle();
    }

    m.rows = { { "Window", "Snapshot", "Window", "Ctrl+Shift+S" },
               { "Menu", "Snapshot to disk", "World > Photo & Video", "Ctrl+`" },
               { "Setting", "Snapshot quality", "Preferences > Graphics", "" },
               { "Panel", "Snapshot", "Preferences > Privacy", "" },
               { "Window", "360 Snapshot", "Window", "" },
               { "Menu", "Show snapshot frame", "World > Photo & Video", "", true, true, false },
               { "Develop", "Snapshot render test", "Develop > Render Tests", "", false, false, true },
               { "Search", "Search settings for \"snap\"", "Preferences", "" } };
    m.tools.clear(); // One model per render context; density variants must not append duplicates.
    const int cols = std::min(kToolCount, 12);
    m.width        = std::to_string(cols * 44) + "dp";
    for (int i = 0; i < kToolCount; ++i)
        m.tools.push_back({ "tool-" + std::to_string(i), std::string("llimage(") + kTools[i].icon + " solid)", false, false });

    // Sample people and lines (invented names; never real sessions).
    ChatModel& c = m.chat;
    c.tabs = { { "Nearby chat", 0, false }, { "Ada Lovelace", 2, true }, { "Builders' Guild", 14, true } };
    c.acts = { { "Pop out", true } };
    c.msgs = { { "Glyph test: ", "ᴀʟᴛ ꜱᴏᴜɴᴅ · Ā Ɔ ǅ Ȣ Ⱡ ⱥ Ḁ Ꞩ · Ελληνικά · Кириллица · 日本語 中文 한국어 · العربية עברית · ┌─┐ ∑ √ ∞ ≠ · 👩🏽‍💻 👨‍👩‍👧‍👦", "13:57", false, false, false, false }, { "", "You are now in Sunshine Bay.", "13:58", true, false, false, false },
               { "Ada Lovelace: ", "Is anyone coming to the build meeting?", "14:02", false, false, false, false },
               { "Brook Hollow: ", "On my way, two minutes.", "14:03", false, false, false, false },
               { "Cedar Winslow", " waves at everyone", "14:04", false, false, true, false },
               { "Cedar Winslow: ", "The sandbox resets at 15:00, so save your work before then. Anything left out gets returned.", "14:05",
                 false, false, false, false },
               { "Test Resident: ", "Thanks for the reminder!", "14:06", false, false, false, true } };
    for (auto& line : c.msgs) { line.rich=LLNewUIKit::escapeText(line.text); line.sender=line.from; }
    PeopleModel& p = m.people;
    const char* names[] = { "Ada Lovelace", "Brook Hollow", "Cedar Winslow", "Dakota Reed", "Ember Night", "Fern Gully", "Gale Storm" };
    p.rows.resize(48);
    for (int i = 0; i < 7; ++i)
    {
        PRow& r  = p.rows[size_t(i)];
        r.used   = true;
        r.name   = names[i];
        r.online = i < 4;
        r.sub    = r.online ? "online" : "";
        r.selected = i == 1;
    }
    p.acts = { { "IM", true }, { "Profile", true }, { "Teleport", true }, { "Pay", true } };
    p.menu = { { "IM", true },  { "Profile", true }, { "Offer teleport", true }, { "Request teleport", true }, { "Call", false },
               { "Pay", true }, { "Show on map", true }, { "Share", true }, { "Invite to group", true }, { "Remove friend", true },
               { "Block", true } };
    InvModel& v = m.inv;
    const struct
    {
        const char *label, *icon, *twist;
        int         indent;
        bool        selected, worn;
    } inv[] = { { "Animations", "Inv_FolderClosed", "closed", 0, false, false },
                { "Clothing", "Inv_FolderOpen", "open", 0, false, false },
                { "Summer dress", "Inv_Clothing", "", 14, true, true },
                { "Rain coat", "Inv_Clothing", "", 14, false, false },
                { "Landmarks", "Inv_FolderClosed", "closed", 0, false, false },
                { "Notecards", "Inv_FolderClosed", "closed", 0, false, false },
                { "Objects", "Inv_FolderOpen", "open", 0, false, false },
                { "Garden bench", "Inv_Object", "", 14, false, false },
                { "Welcome notecard", "Inv_Notecard", "", 14, false, false },
                { "Sunset texture", "Inv_Texture", "", 14, false, false },
                { "Library", "Inv_FolderClosed", "closed", 0, false, false } };
    v.rows.resize(60);
    for (size_t i = 0; i < v.rows.size(); ++i)
    {
        IRow& r = v.rows[i];
        r.deco  = "none";
        if (i < sizeof(inv) / sizeof(inv[0]))
            r = { inv[i].label, std::string("llimage(") + inv[i].icon + ")", inv[i].twist, inv[i].indent, true, inv[i].selected, inv[i].worn };
    }
    v.acts = { { "Take off", true }, { "Delete", true } };
    v.menu = { { "Open", true }, { "Wear", false }, { "Add", false }, { "Take off / Detach", true }, { "Properties", true },
               { "New window", true }, { "Delete", true } };
}

void dirtyAll()
{
    for (Rml::DataModelHandle* h : { &gModel.bar, &gModel.palette, &gModel.toolsModel, &gModel.chatModel, &gModel.peopleModel, &gModel.invModel })
        if (*h)
            h->DirtyAllVariables();
}

std::string themedDoc(const char* rml, bool light)
{
    return LLNewUIKit::themed(rml, LLNewUIKit::tokens(light ? LLNewUIKit::Theme::Light : LLNewUIKit::Theme::Dark, true));
}

// The viewer's placement (llnewui.cpp sync): the bar under the menu bar, the grid near the bottom,
// the tab on the bottom edge.
void place(Rml::Context* context, const std::vector<Rml::ElementDocument*>& docs)
{
    for (Rml::ElementDocument* d : docs)
    {
        if (Rml::Element* e = d->GetElementById("barwrap"))
            e->SetProperty("top", "23dp");
        if (Rml::Element* e = d->GetElementById("toolwrap"))
            e->SetProperty("bottom", "12dp");
        if (Rml::Element* e = d->GetElementById("tabwrap"))
            e->SetProperty("bottom", "0dp");
    }
    (void)context;
}

// A window placed as the framework places it (llnewuiwindow.cpp apply): the body is the window.
void placeWindow(Rml::ElementDocument* d, int x, int y, int w, int h, bool active)
{
    d->SetClass("window", true);
    d->SetClass("active", active);
    d->SetProperty("left", std::to_string(x) + "dp");
    d->SetProperty("top", std::to_string(y) + "dp");
    d->SetProperty("width", std::to_string(w) + "dp");
    d->SetProperty("height", std::to_string(h) + "dp");
}

// The tip over tool `cell`, placed as LLNewUITools places it.
void placeTip(Rml::Context* context, const std::vector<Rml::ElementDocument*>& docs, int cell)
{
    context->Update();
    for (Rml::ElementDocument* d : docs)
    {
        Rml::Element* el   = d->GetElementById("tool-" + std::to_string(cell));
        Rml::Element* wrap = d->GetElementById("tipwrap");
        if (!el || !wrap)
            continue;
        const float density = context->GetDensityIndependentPixelRatio();
        const float rootW = float(context->GetDimensions().x) / density, rootH = float(context->GetDimensions().y) / density;
        const float cx   = (el->GetAbsoluteLeft() + el->GetOffsetWidth() * 0.5f) / density;
        // above the grid's edge, not over its other buttons
        Rml::Element* grid = d->GetElementById("tools");
        const float top  = (grid ? grid->GetAbsoluteTop() : el->GetAbsoluteTop()) / density;
        const float left = std::clamp(cx - 150.f, 8.f, std::max(8.f, rootW - 308.f));
        wrap->SetProperty("left", std::to_string(int(left)) + "dp");
        wrap->SetProperty("bottom", std::to_string(int(rootH - top + 8.f)) + "dp");
    }
}

void resetState()
{
    Model& m = gModel;
    m.open   = true;
    m.locked = false;
    m.tip = m.tipWhat = m.tipKey = m.tipClick = "";
    for (ToolCell& t : m.tools)
        t.on = t.off = false;
    m.tools[11].on = true; // Inventory open
    m.selected     = 0;
    m.chat.active = true;
    m.chat.current = 0;
    m.chat.suggest.clear();
    m.chat.line.clear();
    m.people.active = m.inv.active = false;
    m.people.menuOpen = m.inv.menuOpen = false;
    m.people.menu = { { "IM", true }, { "Profile", true }, { "Offer teleport", true }, { "Request teleport", true }, { "Call", false },
                      { "Pay", true }, { "Show on map", true }, { "Share", true }, { "Invite to group", true }, { "Remove friend", true }, { "Block", true } };

    m.people.nearby=false; m.people.view=0; m.chat.grouped=false; m.chat.pickerOpen=false; m.navOpen=false;
}

// Window scene geometry in dp for a window `W` x `H` dp: chat bottom left, people top left,
// inventory top right (the framework's defaults).
struct Places
{
    int w, h;
};
Places screenDp(Rml::Context* c)
{
    const float d = c->GetDensityIndependentPixelRatio();
    return { int(float(c->GetDimensions().x) / d), int(float(c->GetDimensions().y) / d) };
}
} // namespace

void labRegisterImageDecorator(const std::string& icons)
{
    // Relative to the working directory: RmlUi resolves a decorator's image against the document
    // and drops a leading '/'.
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(icons, std::filesystem::current_path(ec), ec);
    gIcons = ec || rel.empty() ? icons : rel.string();
    static LabImageInstancer instancer;
    Rml::Factory::RegisterDecoratorInstancer("llimage", &instancer);
}

std::vector<LabScene> labNewUIScenes(Rml::Context* context)
{
    bindModels(context);
    std::vector<LabScene> scenes;
    for (const bool light : { false, true })
    {
        const std::string theme = light ? "light" : "dark";
        const std::string world = light ? "light" : "dark";
        for (const char* w : { "dark", "light" })
        {
            const std::string ww = w;
            scenes.push_back({ "newui-main-" + theme + "-on-" + ww, ww,
                               { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIDocs::kToolsRml, light) },
                               [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                               {
                                   resetState();
                                   dirtyAll();
                                   place(c, d);
                               } });
            scenes.push_back({ "newui-palette-" + theme + "-on-" + ww, ww,
                               { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIDocs::kToolsRml, light),
                                 themedDoc(LLNewUIDocs::kPaletteRml, light) },
                               [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                               {
                                   resetState();
                                   gModel.open     = false;
                                   gModel.selected = 1;
                                   dirtyAll();
                                   place(c, d);
                               } });
        }
        // The pull-up grid: the tab alone, opened by hover, locked open (the grip lit).
        scenes.push_back({ "newui-tools-tab-" + theme, world, { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIDocs::kToolsRml, light) },
                           [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                           {
                               resetState();
                               gModel.open = false;
                               dirtyAll();
                               place(c, d);
                           } });
        scenes.push_back({ "newui-tools-locked-" + theme, world,
                           { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIDocs::kToolsRml, light) },
                           [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                           {
                               resetState();
                               gModel.locked = true;
                               dirtyAll();
                               place(c, d);
                           } });
        // The tip, the held (running) and the greyed states.
        scenes.push_back({ "newui-tools-tip-" + theme, world, { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIDocs::kToolsRml, light) },
                           [light](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                           {
                               resetState();
                               gModel.tools[1].off = !light; // Speak: greyed (no voice) in the dark render
                               gModel.tools[7].on  = true;   // Snapshot / camera mode open
                               const int cell      = light ? 2 : 1;
                               gModel.tip          = kTools[cell].label;
                               gModel.tipWhat      = kTools[cell].tooltip;
                               gModel.tipKey       = "";
                               gModel.tipClick     = light ? "" : "Not available now";
                               dirtyAll();
                               place(c, d);
                               placeTip(c, d, cell);
                           } });
        // The three windows together, over both worlds (text contrast on light and dark scenes).
        for (const char* w : { "dark", "light" })
        {
            const std::string ww = w;
            scenes.push_back({ "newui-windows-" + theme + "-on-" + ww, ww,
                               { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIDocs::kToolsRml, light),
                                 themedDoc(LLNewUIWindowDocs::kChatRml, light), themedDoc(LLNewUIWindowDocs::kPeopleRml, light),
                                 themedDoc(LLNewUIWindowDocs::kInventoryRml, light) },
                               [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                               {
                                   resetState();
                                   gModel.open = false;
                                   dirtyAll();
                                   place(c, d);
                                   const Places s = screenDp(c);
                                   placeWindow(d[2], 12, s.h - 12 - 420, 460, 420, true);
                                   placeWindow(d[3], 12, 56, 340, 420, false);
                                   placeWindow(d[4], s.w - 12 - 360, 56, 360, 560, false);
                               } });
        }
        // Each window alone with its menus / completion open (the parity review).
        scenes.push_back({ "newui-navigation-" + theme,world,{themedDoc(LLNewUIDocs::kBarRml,light)},
                           [](Rml::Context* c,const std::vector<Rml::ElementDocument*>& d)
                           {
                               resetState(); gModel.open=false; gModel.navOpen=true; gModel.navMode=1;
                               gModel.navSelected=0; gModel.navLabels={"Synthetic destination","Synthetic history"};
                               gModel.navEnabled={1,1}; dirtyAll(); place(c,d);
                           } });
        scenes.push_back({ "newui-chat-grouped-" + theme,world,{themedDoc(LLNewUIWindowDocs::kChatRml,light)},
                           [](Rml::Context* c,const std::vector<Rml::ElementDocument*>& d)
                           {
                               resetState(); gModel.open=false; gModel.chat.grouped=true;
                               dirtyAll(); place(c,d); placeWindow(d[0],40,60,520,440,true);
                           } });
        scenes.push_back({ "newui-chat-" + theme, world, { themedDoc(LLNewUIDocs::kToolsRml, light), themedDoc(LLNewUIWindowDocs::kChatRml, light) },
                           [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                           {
                               resetState();
                               gModel.open = false;
                               gModel.chat.line    = "see you at the meeting, Ad";
                               gModel.chat.suggest = { "Ada Lovelace" };
                               dirtyAll();
                               place(c, d);
                               placeWindow(d[1], 40, 60, 520, 440, true);
                           } });
        for (int state = 0; state < 3; ++state)
            scenes.push_back({ "newui-picker-" + std::to_string(state) + "-" + theme, world,
                { themedDoc(LLNewUIWindowDocs::kChatRml, light) },
                [state](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                {
                    resetState(); gModel.chat.pickerOpen = true;
                    gModel.chat.pickerQuery = state == 0 ? "" : state == 1 ? "Avatar" : "zzzz";
                    gModel.chat.pickerLabels = state == 2 ? std::vector<std::string>{} : state == 1 ? std::vector<std::string>{"Avatar 1", "Avatar 2"} : std::vector<std::string>{"Avatar 1", "Avatar 2", "Example Builders"};
                    gModel.chat.pickerStatuses = state == 2 ? std::vector<std::string>{} : state == 1 ? std::vector<std::string>{"Online", "Offline"} : std::vector<std::string>{"Online", "Offline", "Group"};
                    gModel.chat.pickerFriendCount = state == 2 ? 0 : 2;
                    dirtyAll(); place(c,d); placeWindow(d[0],40,60,600,600,true);
                } });
        scenes.push_back({ "newui-people-" + theme, world, { themedDoc(LLNewUIDocs::kToolsRml, light), themedDoc(LLNewUIWindowDocs::kPeopleRml, light) },
                           [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                           {
                               resetState();
                               gModel.open          = false;
                               gModel.people.active = true;
                               gModel.people.menuOpen = true;
                               gModel.people.menuX  = 120;
                               gModel.people.menuY  = 110;
                               dirtyAll();
                               place(c, d);
                               placeWindow(d[1], 40, 60, 360, 520, true);
                           } });
        scenes.push_back({ "newui-nearby-" + theme, world, { themedDoc(LLNewUIWindowDocs::kPeopleRml,light) },
                           [](Rml::Context* c,const std::vector<Rml::ElementDocument*>& d)
                           {
                               resetState(); gModel.open=false;
                               gModel.people.nearby=true; gModel.people.view=1;
                               gModel.people.countText="7 nearby"; gModel.people.filterHint="Filter nearby";
                               gModel.people.rangeLimited=false; gModel.people.active=true;
                               dirtyAll(); place(c,d); for (size_t i = 0; i < gModel.people.rows.size(); ++i)
                               {
                                   auto& row = gModel.people.rows[i];
                                   row.name = "Avatar " + std::to_string(i + 1);
                                   char distance[32];
                                   std::snprintf(distance,sizeof(distance),"%.1f m",double(i + 1) * 7.25);
                                   row.sub = distance;
                                   row.friendFlag = (i % 2 == 0); row.typing = (i % 3 == 0);
                                   row.speaking = (i % 4 == 0); row.chatRange = (i % 2 != 0);
                                   row.regionFlag = true; row.notes = (i % 3 == 1);
                               }
                               dirtyAll(); placeWindow(d[0],40,60,380,640,true);
                           } });
        scenes.push_back({ "newui-nearby-menu-" + theme, world, { themedDoc(LLNewUIWindowDocs::kPeopleRml,light) },
                           [](Rml::Context* c,const std::vector<Rml::ElementDocument*>& d)
                           {
                               resetState(); gModel.open=false; gModel.people.nearby=true; gModel.people.view=1;
                               gModel.people.menuOpen=true; gModel.people.menuX=0; gModel.people.menuY=0;
                               gModel.people.menu.resize(34);
                               for (auto& item:gModel.people.menu) item={"Synthetic Nearby action",true};
                               dirtyAll(); place(c,d); placeWindow(d[0],40,60,320,460,true);
                           } });
        scenes.push_back({ "newui-inventory-" + theme, world,
                           { themedDoc(LLNewUIDocs::kToolsRml, light), themedDoc(LLNewUIWindowDocs::kInventoryRml, light) },
                           [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                           {
                               resetState();
                               gModel.open       = false;
                               gModel.inv.active = true;
                               gModel.inv.menuOpen = true;
                               gModel.inv.menuX  = 140;
                               gModel.inv.menuY  = 96;
                               dirtyAll();
                               place(c, d);
                               placeWindow(d[1], 40, 60, 380, 560, true);
                           } });
    }
    for (LabScene& scene : labNewUIStatusScenes(context)) // the status bar and its menus (rmluilab_newui_status.cpp)
        scenes.push_back(std::move(scene));
    return scenes;
}

void labAppendSyntheticChatMessage()
{
    Line line;
    line.from=line.sender="Synthetic sender";
    line.text=line.rich="Synthetic appended line";
    line.time="14:06";
    auto& lines=gModel.chat.msgs;
    if (lines.size()>=80) lines.erase(lines.begin());
    lines.push_back(std::move(line));
    gModel.chatModel.DirtyVariable("msgs");
}
