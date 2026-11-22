// #703 slrmluilab --newui: the status bar and its menus (llnewuistatusdocs.h) over sample data.
#include "rmluilab_newui.h"

#include "llnewuidocs.h"
#include "llnewuikitstyle.h"
#include "llnewuistatusdocs.h"

#include <RmlUi/Core.h>

#include <algorithm>

#include <string>
#include <vector>

namespace
{
struct MenuRow
{
    std::string label, hint;
    bool        check = false, on = false, off = false, branch = false, sep = false;
};

const char* const kParts[] = { "showParcel", "showFps", "showNet", "showBandwidth", "showTime", "showBalance",
                               "showMedia", "showVolume", "showPresets", "showGestures" };
constexpr int     kPartCount = int(sizeof(kParts) / sizeof(kParts[0]));

struct Status
{
    bool        show[kPartCount] = { true, true, true, false, true, true, true, true, true, false };
    std::string rating, health, fps, ping, loss, bandwidth, time, balance, menuOn;
    std::string mediaIcon = "llimage(Play_Off solid)", volumeIcon = "llimage(Audio_Off solid)"; // valid before the first refresh
    int         fpsLevel = 0, pingLevel = 0, lossLevel = 0;
    bool        noVoice = false, noFly = false, noPush = false, noBuild = false, noScripts = false, noSeeAvs = false, damage = false;
    bool        mediaUsable = true, muted = false;
    std::string tip, tipWhat, tipClick;
    // the menu
    std::vector<MenuRow> rows;
    std::string          title, more;
    bool                 canBack = false;
    Rml::DataModelHandle status, menu;
};
Status gS;

void noop(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {}

void bind(Rml::Context* context)
{
    Status& s = gS;
    if (auto m = context->CreateDataModel("status"))
    {
        for (int i = 0; i < kPartCount; ++i)
            m.Bind(kParts[i], &s.show[i]);
        m.Bind("rating", &s.rating);
        m.Bind("health", &s.health);
        m.Bind("fps", &s.fps);
        m.Bind("fpsLevel", &s.fpsLevel);
        m.Bind("ping", &s.ping);
        m.Bind("pingLevel", &s.pingLevel);
        m.Bind("loss", &s.loss);
        m.Bind("lossLevel", &s.lossLevel);
        m.Bind("bandwidth", &s.bandwidth);
        m.Bind("time", &s.time);
        m.Bind("balance", &s.balance);
        m.Bind("mediaIcon", &s.mediaIcon);
        m.Bind("mediaUsable", &s.mediaUsable);
        m.Bind("volumeIcon", &s.volumeIcon);
        m.Bind("muted", &s.muted);
        m.Bind("menuOn", &s.menuOn);
        m.Bind("noVoice", &s.noVoice);
        m.Bind("noFly", &s.noFly);
        m.Bind("noPush", &s.noPush);
        m.Bind("noBuild", &s.noBuild);
        m.Bind("noScripts", &s.noScripts);
        m.Bind("noSeeAvs", &s.noSeeAvs);
        m.Bind("damage", &s.damage);
        m.Bind("tip", &s.tip);
        m.Bind("tipWhat", &s.tipWhat);
        m.Bind("tipClick", &s.tipClick);
        m.BindEventCallback("click", noop);
        s.status = m.GetModelHandle();
    }
    if (auto m = context->CreateDataModel("smenu"))
    {
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
        m.Bind("rows", &s.rows);
        m.Bind("title", &s.title);
        m.Bind("canBack", &s.canBack);
        m.Bind("more", &s.more);
        for (const char* e : { "pick", "close", "back", "scroll" })
            m.BindEventCallback(e, noop);
        s.menu = m.GetModelHandle();
    }
}

// The viewer's usual readings: a moderate region, no flying or building here, all good.
void calm()
{
    Status& s     = gS;
    s.rating      = "M";
    s.noFly       = true;
    s.noBuild     = true;
    s.noVoice = s.noPush = s.noScripts = s.noSeeAvs = s.damage = false;
    s.health      = "100%";
    s.fps         = "62";
    s.fpsLevel    = 1;
    s.ping        = "84";
    s.pingLevel   = 1;
    s.loss        = "0.0";
    s.lossLevel   = 1;
    s.bandwidth   = "312";
    s.time        = "10:42 AM PST";
    s.balance     = "1,234";
    s.mediaIcon   = "llimage(Pause_Off solid)";
    s.volumeIcon  = "llimage(Audio_Off solid)";
    s.mediaUsable = true;
    s.muted       = false;
    s.menuOn      = "";
    s.tip = s.tipWhat = s.tipClick = "";
    s.rows.clear();
    s.title.clear();
    s.more.clear();
    s.canBack = false;
}

void dirty()
{
    gS.status.DirtyAllVariables();
    gS.menu.DirtyAllVariables();
}

std::string themedDoc(const char* rml, bool light)
{
    return LLNewUIKit::themed(rml, LLNewUIKit::tokens(light ? LLNewUIKit::Theme::Light : LLNewUIKit::Theme::Dark, true));
}

// The viewer's placement: the classic top row hidden, the main bar and the status bar at the top
// edge, the menu under the status bar at the right.
void place(Rml::Context* context, const std::vector<Rml::ElementDocument*>& docs)
{
    for (Rml::ElementDocument* d : docs)
    {
        if (Rml::Element* e = d->GetElementById("barwrap"))
            e->SetProperty("top", "4dp");
        if (Rml::Element* e = d->GetElementById("toolwrap"))
            e->SetProperty("bottom", "12dp");
        if (Rml::Element* e = d->GetElementById("menu"))
        {
            e->SetProperty("top", "40dp");
            e->SetProperty("right", "8dp");
        }
    }
    // As LLNewUIStatus::sync: the status bar under the main bar when the two would meet.
    context->Update();
    Rml::Element *bar = nullptr, *stats = nullptr, *wrap = nullptr, *menu = nullptr;
    for (Rml::ElementDocument* d : docs)
    {
        bar   = bar ? bar : d->GetElementById("bar");
        stats = stats ? stats : d->GetElementById("stats");
        wrap  = wrap ? wrap : d->GetElementById("statwrap");
        menu  = menu ? menu : d->GetElementById("menu");
    }
    const float density = context->GetDensityIndependentPixelRatio();
    if (bar && stats && wrap)
    {
        const float rootW    = float(context->GetDimensions().x) / density;
        const float barRight = (bar->GetAbsoluteLeft() + bar->GetOffsetWidth()) / density;
        if (rootW - 8.f - stats->GetOffsetWidth() / density < barRight + 4.f)
        {
            wrap->SetProperty("top", "40dp");
            if (menu)
                menu->SetProperty("top", "76dp");
        }
    }
}

MenuRow item(const char* label, const char* hint = "", bool check = false, bool on = false, bool off = false)
{
    MenuRow r;
    r.label = label;
    r.hint  = hint;
    r.check = check;
    r.on    = on;
    r.off   = off;
    return r;
}
MenuRow branch(const char* label)
{
    MenuRow r;
    r.label  = label;
    r.branch = true;
    return r;
}
MenuRow sep()
{
    MenuRow r;
    r.sep = true;
    return r;
}

void moreMenu()
{
    gS.menuOn = "more";
    gS.rows   = { item("Search commands", "Ctrl+K"), sep(), item("Preferences", "Ctrl+P"), branch("Status bar"),
                  branch("Browse the classic menus"), sep(), item("Advanced menu", "Ctrl+Alt+D", true, true),
                  item("Develop menu", "Ctrl+Alt+Q", true, false), sep(), item("Slipstream Help (wiki)"), item("Report Bug"),
                  item("About Slipstream"), sep(), item("Exit Slipstream", "Ctrl+Q") };
}

void balanceMenu()
{
    gS.menuOn = "balance";
    gS.title  = "L$ balance";
    gS.rows   = { item("Buy L$"), item("Refresh the balance"), item("Hide the balance", "", true, false), sep(), item("Shop (Marketplace)"),
                  item("Transaction log", "", true, false) };
}

void browseMenu()
{
    gS.menuOn  = "more";
    gS.title   = "World";
    gS.canBack = true;
    gS.rows    = { item("Resync Animations", "", false, false, false), item("Teleport Home", "Ctrl+Shift+H"), branch("Zoom level"), sep(),
                   item("Mini-Map", "Ctrl+Shift+M", true, true), item("World Map", "Ctrl+M", true, false), item("Snapshot", "Ctrl+Shift+S"),
                   sep(), item("Set Home to Here", "", false, false, true), branch("Show"), branch("Environment"),
                   item("Region / Estate", "Alt+R"), item("Sit Here", "", false, false, true) };
    gS.more    = "6 more below (scroll)";
}
} // namespace

std::vector<LabScene> labNewUIStatusScenes(Rml::Context* context)
{
    bind(context);
    std::vector<LabScene> scenes;
    for (const bool light : { false, true })
    {
        const std::string theme = light ? "light" : "dark";
        for (const char* world : { "dark", "light" })
        {
            const std::string w = world;
            scenes.push_back({ "newui-status-" + theme + "-on-" + w, w,
                               { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIDocs::kToolsRml, light),
                                 themedDoc(LLNewUIStatusDocs::kStatusRml, light) },
                               [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                               {
                                   calm();
                                   dirty();
                                   place(c, d);
                               } });
        }
        // Warnings: a low frame rate, a slow link, some loss, damage on, sound muted, no media.
        scenes.push_back({ "newui-status-warn-" + theme, light ? "light" : "dark",
                           { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIStatusDocs::kStatusRml, light) },
                           [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                           {
                               calm();
                               Status& s    = gS;
                               s.rating     = "A";
                               s.noScripts  = s.noPush = s.damage = true;
                               s.health     = "64%";
                               s.fps        = "31";
                               s.fpsLevel   = 2;
                               s.ping       = "287";
                               s.pingLevel  = 3;
                               s.loss       = "1.2";
                               s.lossLevel  = 2;
                               s.muted      = true;
                               s.volumeIcon = "llimage(AudioMute_Off solid)";
                               s.mediaIcon  = "llimage(Play_Off solid)";
                               s.mediaUsable = false;
                               s.show[3]    = true; // bandwidth
                               s.tip        = "Frame rate";
                               s.tipWhat    = "31 frames per second (green from 45, amber from 25).";
                               s.tipClick   = "Click: frame timings. Shift+click: statistics.";
                               dirty();
                               place(c, d);
                               c->Update();
                               for (Rml::ElementDocument* doc : d)
                                   if (Rml::Element* wrap = doc->GetElementById("stipwrap"))
                                       if (Rml::Element* fps = doc->GetElementById("st-fps"))
                                       {
                                           const float density = c->GetDensityIndependentPixelRatio();
                                           const float cx = (fps->GetAbsoluteLeft() + fps->GetOffsetWidth() * 0.5f) / density;
                                           const float rootW = float(c->GetDimensions().x) / density;
                                           const float left = std::min(cx - 140.f, rootW - 288.f);
                                           wrap->SetProperty("left", std::to_string(int(left)) + "dp");
                                           wrap->SetProperty("top", std::to_string(int((fps->GetAbsoluteTop() + fps->GetOffsetHeight()) / density) + 8) + "dp");
                                       }
                               s.show[3] = false;
                           } });
        // The "more" menu (frame generation on: the shown rate in accent).
        scenes.push_back({ "newui-status-more-" + theme, light ? "light" : "dark",
                           { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIStatusDocs::kStatusRml, light),
                             themedDoc(LLNewUIStatusDocs::kStatusMenuRml, light) },
                           [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                           {
                               calm();
                               gS.fps      = "124";
                               gS.fpsLevel = 4;
                               moreMenu();
                               dirty();
                               place(c, d);
                           } });
        scenes.push_back({ "newui-status-balance-" + theme, light ? "light" : "dark",
                           { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIStatusDocs::kStatusRml, light),
                             themedDoc(LLNewUIStatusDocs::kStatusMenuRml, light) },
                           [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                           {
                               calm();
                               balanceMenu();
                               dirty();
                               place(c, d);
                               for (Rml::ElementDocument* doc : d)
                                   if (Rml::Element* e = doc->GetElementById("menu"))
                                       e->SetProperty("right", "150dp");
                           } });
        scenes.push_back({ "newui-status-browse-" + theme, light ? "light" : "dark",
                           { themedDoc(LLNewUIDocs::kBarRml, light), themedDoc(LLNewUIStatusDocs::kStatusRml, light),
                             themedDoc(LLNewUIStatusDocs::kStatusMenuRml, light) },
                           [](Rml::Context* c, const std::vector<Rml::ElementDocument*>& d)
                           {
                               calm();
                               browseMenu();
                               dirty();
                               place(c, d);
                           } });
    }
    return scenes;
}
