// #703 The new interface's status bar (see llnewuistatus.h).
#include "llviewerprecompiledheaders.h"
#include "llnewuistatus.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcommandindex.h"
#include "llfloaterperformance.h"
#include "llfloaterreg.h"
#include "llkeyboard.h"
#include "llnewuimenus.h"
#include "llnewuistatusdocs.h"
#include "llparcel.h"
#include "llresmgr.h"
#include "llstartup.h"
#include "llstatusbar.h"
#include "lltracerecording.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewermedia.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "vkgfx/llvkframegen.h"
#include "vkgfx/llvrspacewarp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>
#include <string>

namespace
{
constexpr F64  kRefreshPeriod = 0.25; // values, 4x a second
constexpr F64  kRestDelay     = 0.3;  // a tip or a pull-down after the pointer rests this long
constexpr int  kRightDp = 8, kGapDp = 4, kTipWidthDp = 280;
constexpr char kIdPrefix[] = "newui:status";

// The parts' settings, in the order of the document's show* variables.
struct Part
{
    const char* variable;
    const char* control;
    const char* label; // the command surface's toggle
};
const Part kParts[] = {
    { "showParcel", "UINewStatusParcel", "Parcel (rating, restrictions)" },
    { "showFps", "ShowStatusBarFPS", "Frame rate" },
    { "showNet", "UINewStatusNet", "Ping and packet loss" },
    { "showBandwidth", "ShowNetStats", "Bandwidth" },
    { "showTime", "UINewStatusTime", "Time" },
    { "showBalance", "UINewStatusBalance", "L$ balance" },
    { "showMedia", "UINewStatusMedia", "Media" },
    { "showVolume", "UINewStatusVolume", "Volume" },
    { "showPresets", "UINewStatusPresets", "Graphics and camera presets" },
    { "showGestures", "GestureSetsShowStatusBarIcon", "Gesture sets" },
};
constexpr size_t kPartCount = sizeof(kParts) / sizeof(kParts[0]);

struct State
{
    Rml::Context*         context = nullptr;
    Rml::ElementDocument* doc     = nullptr;
    Rml::DataModelHandle  model;
    bool                  show[kPartCount] = {};
    // values (strings as shown)
    std::string rating, health, fps, ping, loss, bandwidth, time, balance, menuOn;
    std::string mediaIcon = "llimage(Play_Off solid)", volumeIcon = "llimage(Audio_Off solid)"; // valid before the first refresh
    int         fpsLevel = 0, pingLevel = 0, lossLevel = 0; // 0 none, 1 good, 2 warning, 3 bad, 4 frame generation
    bool        noVoice = false, noFly = false, noPush = false, noBuild = false, noScripts = false, noSeeAvs = false, damage = false;
    bool        mediaUsable = false, mediaPlaying = false, muted = false;
    // tips: what the parts say about themselves (refreshed with the values)
    std::string tip, tipWhat, tipClick;
    std::string parcelWhat, fpsWhat, netWhat, dateWhat, balanceWhat;
    // placement and hover
    bool        shown = false;
    int         top = -1;
    int         bottom = 0; // the bar's bottom edge and a gap, dp (0 while hidden): the windows' top inset
    std::string hovered;
    F64         hoverSince = 0, nextRefresh = 0;
    bool        pulled = false; // the hovered part's pull-down was opened
    bool        changed = true;
};
State* gState = nullptr;

template <typename T> void set(State* s, T& field, const T& value, const char* variable)
{
    if (field == value)
        return;
    field = value;
    s->model.DirtyVariable(variable);
    s->changed = true;
}

int level(F64 value, F64 good, F64 warn, bool higherIsBetter)
{
    if (higherIsBetter)
        return value >= good ? 1 : value >= warn ? 2 : 3;
    return value < good ? 1 : value < warn ? 2 : 3;
}

std::string fixed(F64 value, int decimals)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    return buf;
}

void refreshParcel(State* s)
{
    LLViewerParcelMgr* vpm    = LLViewerParcelMgr::getInstance();
    LLViewerRegion*    region = gAgent.getRegion();
    LLParcel*          parcel = vpm->getAgentParcel();
    if (!region || !parcel)
    {
        set(s, s->rating, std::string(), "rating");
        for (auto [flag, name] : { std::pair<bool*, const char*>{ &s->noVoice, "noVoice" }, { &s->noFly, "noFly" }, { &s->noPush, "noPush" },
                                   { &s->noBuild, "noBuild" }, { &s->noScripts, "noScripts" }, { &s->noSeeAvs, "noSeeAvs" },
                                   { &s->damage, "damage" } })
            set(s, *flag, false, name);
        return;
    }
    // The selected parcel's properties when the agent is in it: the parcel manager refreshes
    // those more often (LLPanelTopInfoBar::updateParcelIcons).
    LLParcel* selected = vpm->getParcelSelection()->getParcel();
    if (selected && selected->getLocalID() == parcel->getLocalID() && vpm->getSelectionRegion() == region)
        parcel = selected;
    const U8 access = region->getSimAccess();
    set(s, s->rating, std::string(access == SIM_ACCESS_ADULT ? "A" : access == SIM_ACCESS_MATURE ? "M" : access == SIM_ACCESS_PG ? "G" : ""),
        "rating");
    const bool voice = vpm->allowAgentVoice(region, parcel), fly = vpm->allowAgentFly(region, parcel), push = vpm->allowAgentPush(region, parcel),
               build = vpm->allowAgentBuild(parcel), scripts = vpm->allowAgentScripts(region, parcel),
               dmg = vpm->allowAgentDamage(region, parcel), seeAvs = parcel->getSeeAVs();
    set(s, s->noVoice, !voice, "noVoice");
    set(s, s->noFly, !fly, "noFly");
    set(s, s->noPush, !push, "noPush");
    set(s, s->noBuild, !build, "noBuild");
    set(s, s->noScripts, !scripts, "noScripts");
    set(s, s->noSeeAvs, !seeAvs, "noSeeAvs");
    set(s, s->damage, dmg, "damage");
    set(s, s->health, std::to_string(gStatusBar ? gStatusBar->getHealth() : 100) + "%", "health");
    std::string what = access == SIM_ACCESS_ADULT ? "Adult" : access == SIM_ACCESS_MATURE ? "Moderate" : "General";
    what += " region.";
    std::string limits;
    for (auto [off, word] : { std::pair<bool, const char*>{ !voice, "voice" }, { !fly, "flying" }, { !push, "pushing" }, { !build, "building" },
                              { !scripts, "scripts" } })
        if (off)
            limits += (limits.empty() ? "" : ", ") + std::string(word);
    if (!limits.empty())
        what += " Not allowed here: " + limits + ".";
    if (!seeAvs)
        what += " Avatars on other parcels are hidden.";
    if (dmg)
        what += " Damage is on: health " + s->health + ".";
    s->parcelWhat = what;
}

void refreshFps(State* s)
{
    const F64 fps = LLTrace::get_frame_recording().getLastRecording().getMean(LLStatViewer::FPS_SAMPLE);
    // #1517 / #1594: while frame generation or VR space warp adds frames, the frames on screen in
    // accent, both rates in the tip (as the classic status bar shows them).
    const LLVKFrameGen&  fg   = LLVKFrameGen::instance();
    const S32            mult = (fg.directReady() && fg.multiplierOn()) ? llround(fg.shownPerRendered()) : 1;
    const LLVRSpaceWarp& warp = LLVRSpaceWarp::instance();
    S32                  shown = llfloor(fps), rendered = shown;
    int                  lvl   = level(fps, 45.0, 25.0, true);
    if (mult >= 2)
    {
        shown = llfloor(fps * fg.shownPerRendered());
        lvl   = 4;
        s->fpsWhat = std::to_string(rendered) + " rendered, " + std::to_string(shown) + " shown per second (frame generation " +
                     std::to_string(mult) + "x).";
    }
    else if (warp.engaged() && warp.shownHz() > 0.5f)
    {
        shown    = llfloor(warp.shownHz() + 0.5f);
        rendered = llfloor(warp.realHz() + 0.5f);
        lvl      = 4;
        s->fpsWhat = std::to_string(rendered) + " rendered, " + std::to_string(shown) + " shown per second (VR space warp).";
    }
    else
        s->fpsWhat = std::to_string(shown) + " frames per second (green from 45, amber from 25).";
    set(s, s->fps, std::to_string(std::max(0, shown)), "fps");
    set(s, s->fpsLevel, lvl, "fpsLevel");
}

void refreshNet(State* s)
{
    LLTrace::PeriodicRecording& rec = LLTrace::get_frame_recording();
    const bool                  in  = gAgent.getRegion() != nullptr;
    auto      finite = [](F64 v) { return std::isfinite(v) ? v : 0.0; }; // no periods yet: NaN
    const F64 ping = in ? finite(F64Milliseconds(rec.getPeriodMean(LLStatViewer::SIM_PING, 32)).value()) : 0.0;
    const F64 loss = in ? finite(rec.getPeriodMean(LLStatViewer::PACKETS_LOST_PERCENT, 32).value()) : 0.0;
    const F64 kbps = finite(F64Kilobits(rec.getPeriodMeanPerSec(LLStatViewer::ACTIVE_MESSAGE_DATA_RECEIVED, 32)).value());
    set(s, s->ping, in ? std::to_string(llround(F32(ping))) : std::string("--"), "ping");
    set(s, s->pingLevel, in ? level(ping, 120.0, 250.0, false) : 0, "pingLevel");
    set(s, s->loss, fixed(loss, 1), "loss");
    set(s, s->lossLevel, in ? level(loss, 0.5, 2.0, false) : 0, "lossLevel");
    set(s, s->bandwidth, fixed(kbps, 0), "bandwidth");
    s->netWhat = "Ping to the region " + s->ping + " ms, packet loss " + s->loss + " %, receiving " + s->bandwidth + " kbps.";
}

void refreshTime(State* s)
{
    if (!gStatusBar)
        return;
    static LLCachedControl<bool> seconds(gSavedSettings, "SlipstreamSLTClockSeconds", false);
    static LLCachedControl<bool> h24(gSavedSettings, "SlipstreamSLTClock24Hour", false);
    // Formatted only when what it shows can have changed (the second or the minute, an option), as
    // the classic status bar does (#659).
    static time_t last  = 0;
    static int    shape = -1;
    const time_t  now   = time_corrected();
    const time_t  step  = seconds() ? now : now / 60;
    const int     form  = (seconds() ? 1 : 0) | (h24() ? 2 : 0);
    if (step == last && form == shape && !s->time.empty())
        return;
    last  = step;
    shape = form;
    LLSD sub;
    sub["datetime"] = (S32)now;
    std::string text = gStatusBar->getString(h24() ? (seconds() ? "time24_seconds" : "time24") : (seconds() ? "time_seconds" : "time"));
    LLStringUtil::format(text, sub);
    set(s, s->time, text, "time");
    std::string date = gStatusBar->getString("timeTooltip");
    LLStringUtil::format(date, sub);
    s->dateWhat = date + " (Second Life time, Pacific).";
}

void refreshBalance(State* s)
{
    if (!gStatusBar)
        return;
    const std::string amount = LLResMgr::getInstance()->getMonetaryString(gStatusBar->getBalance());
    set(s, s->balance, gSavedSettings.getBOOL("ObscureBalanceInStatusBar") ? std::string("****") : amount, "balance");
    s->balanceWhat = "L$ " + amount + ".";
}

void refreshMedia(State* s)
{
    LLViewerMedia* media = LLViewerMedia::getInstance();
    static LLCachedControl<bool> music(gSavedSettings, "AudioStreamingMusic");
    static LLCachedControl<bool> video(gSavedSettings, "AudioStreamingMedia");
    // As the classic toggle: usable with media streaming on and something to play.
    const bool usable  = (music() || video()) && (media->hasInWorldMedia() || media->hasParcelMedia() || media->hasParcelAudio());
    const bool playing = media->isAnyMediaPlaying() || media->isParcelMediaPlaying() || media->isParcelAudioPlaying();
    const bool muted   = LLAppViewer::instance()->getMasterSystemAudioMute();
    set(s, s->mediaUsable, usable, "mediaUsable");
    s->mediaPlaying = playing;
    set(s, s->mediaIcon, std::string(playing ? "llimage(Pause_Off solid)" : "llimage(Play_Off solid)"), "mediaIcon");
    set(s, s->muted, muted, "muted");
    set(s, s->volumeIcon, std::string(muted ? "llimage(AudioMute_Off solid)" : "llimage(Audio_Off solid)"), "volumeIcon");
}

void refreshShown(State* s)
{
    for (size_t i = 0; i < kPartCount; ++i)
    {
        const bool on = gSavedSettings.getBOOL(kParts[i].control);
        if (on != s->show[i])
        {
            s->show[i] = on;
            s->model.DirtyVariable(kParts[i].variable);
            s->changed = true;
        }
    }
}

// The element's screen rect in UI units (LLView coordinates: origin bottom left).
LLRect uiRect(State* s, Rml::Element* el, float density)
{
    const float rootH = float(s->context->GetDimensions().y) / density;
    const float l = el->GetAbsoluteLeft() / density, t = el->GetAbsoluteTop() / density;
    const float w = el->GetOffsetWidth() / density, h = el->GetOffsetHeight() / density;
    return LLRect(S32(l), S32(rootH - t), S32(l + w), S32(rootH - t - h));
}

// A pull-down of the classic status bar under part `id` (volume, media, presets, gestures).
bool pulldownFor(const std::string& id, LLStatusBar::Pulldown& which)
{
    if (id == "st-volume")
        which = LLStatusBar::Pulldown::Volume;
    else if (id == "st-media")
        which = LLStatusBar::Pulldown::NearbyMedia;
    else if (id == "st-presets")
        which = LLStatusBar::Pulldown::GraphicsPresets;
    else if (id == "st-camera")
        which = LLStatusBar::Pulldown::CameraPresets;
    else if (id == "st-gestures")
        which = LLStatusBar::Pulldown::GestureSets;
    else
        return false;
    return true;
}

void openPulldown(State* s, const std::string& id, float density)
{
    LLStatusBar::Pulldown which;
    Rml::Element*         el = s->doc ? s->doc->GetElementById(id) : nullptr;
    if (!gStatusBar || !el || density <= 0.f || !pulldownFor(id, which))
        return;
    LLNewUIMenus::instance().close();
    gStatusBar->showPulldown(which, uiRect(s, el, density));
}

void setTip(State* s, const std::string& tip, const std::string& what, const std::string& click)
{
    set(s, s->tip, tip, "tip");
    set(s, s->tipWhat, what, "tipWhat");
    set(s, s->tipClick, click, "tipClick");
}

// The hovered part: after a rest, its tip under it, or its pull-down.
void updateHover(State* s, float density)
{
    std::string id;
    Rml::Element* el = nullptr;
    if (s->shown)
        for (Rml::Element* e = s->context->GetHoverElement(); e && e->GetOwnerDocument() == s->doc; e = e->GetParentNode())
            if (e->GetId().rfind("st-", 0) == 0)
            {
                id = e->GetId();
                el = e;
                break;
            }
    if (LLNewUIMenus::instance().openPage() != LLNewUIMenus::Page::None)
        id.clear(); // a menu open: no tips over it
    const F64 now = LLTimer::getTotalSeconds();
    if (id != s->hovered)
    {
        s->hovered    = id;
        s->hoverSince = now;
        s->pulled     = false;
        if (!id.empty())
            s->doc->PullToFront(); // its tip above a native window raised since (llnewuiwindow.h)
    }
    if (id.empty() || now - s->hoverSince < kRestDelay)
    {
        setTip(s, std::string(), std::string(), std::string());
        if (!id.empty())
            s->changed = true; // keep capturing until the delay has run
        return;
    }
    LLStatusBar::Pulldown which;
    if (pulldownFor(id, which))
    {
        if (!s->pulled)
        {
            s->pulled = true;
            openPulldown(s, id, density);
        }
        setTip(s, std::string(), std::string(), std::string());
        return;
    }
    if (id == "st-parcel")
        setTip(s, "This parcel", s->parcelWhat, "Click: About Land");
    else if (id == "st-fps")
        setTip(s, "Frame rate", s->fpsWhat, "Click: frame timings. Shift+click: statistics.");
    else if (id == "st-net" || id == "st-bw")
        setTip(s, "Network", s->netWhat, "Click: statistics");
    else if (id == "st-time")
        setTip(s, s->time, s->dateWhat, std::string());
    else if (id == "st-money")
        setTip(s, "L$ balance", s->balanceWhat, "Click: buy L$, refresh, hide, shop");
    else if (id == "st-more")
        setTip(s, "More", "Preferences, the status bar's parts, the Advanced and Develop menus, the classic menus to browse, help.",
               std::string());
    Rml::Element* wrap = s->doc->GetElementById("stipwrap");
    if (!wrap || !el || density <= 0.f)
        return;
    const float rootW = float(s->context->GetDimensions().x) / density;
    const float cx    = (el->GetAbsoluteLeft() + el->GetOffsetWidth() * 0.5f) / density;
    const float below = (el->GetAbsoluteTop() + el->GetOffsetHeight()) / density + 8.f;
    const float left  = std::clamp(cx - kTipWidthDp / 2.f, 8.f, std::max(8.f, rootW - kTipWidthDp - 8.f));
    wrap->SetProperty("left", std::to_string(int(left)) + "dp");
    wrap->SetProperty("top", std::to_string(int(below)) + "dp");
}

void click(const std::string& part)
{
    State* s = gState;
    if (!s)
        return;
    const bool shift = gKeyboard && (gKeyboard->currentMask(true) & MASK_SHIFT);
    const float density = s->context->GetDensityIndependentPixelRatio();
    if (part == "parcel")
        LLFloaterReg::showInstance("about_land");
    else if (part == "fps")
    {
        if (shift)
            LLFloaterReg::showInstance("stats");
        else
            LLFloaterPerformance::showTimings();
    }
    else if (part == "net")
        LLFloaterReg::toggleInstanceOrBringToFront("stats");
    else if (part == "media")
    {
        if (s->mediaUsable)
            LLViewerMedia::getInstance()->setAllMediaPaused(s->mediaPlaying); // as the classic toggle
    }
    else if (part == "volume")
        LLAppViewer::instance()->setMasterSystemAudioMute(!LLAppViewer::instance()->getMasterSystemAudioMute());
    else if (part == "presets" || part == "camera" || part == "gestures")
        openPulldown(s, "st-" + part, density);
    else if (part == "balance" || part == "more" || part == "menus")
    {
        // Under the bar, right-aligned with it (the window's right edge) or with the L$ part.
        Rml::Element* stats = s->doc->GetElementById("stats");
        const int     top   = stats && density > 0.f ? int((stats->GetAbsoluteTop() + stats->GetOffsetHeight()) / density) + kGapDp : 40;
        int           right = kRightDp;
        if (part == "balance")
            if (Rml::Element* el = s->doc->GetElementById("st-money"); el && density > 0.f)
                right = std::max(kRightDp, int(float(s->context->GetDimensions().x) / density -
                                                (el->GetAbsoluteLeft() + el->GetOffsetWidth()) / density));
        const LLNewUIMenus::Page page = part == "balance" ? LLNewUIMenus::Page::Balance
                                        : part == "menus" ? LLNewUIMenus::Page::Menus
                                                          : LLNewUIMenus::Page::More;
        LLNewUIMenus::instance().toggle(page, right, top);
    }
    s->nextRefresh = 0; // the new state, now
    s->changed     = true;
}

// The command surface's toggles for the bar and its parts.
void registerCommands()
{
    auto toggle = [](const std::string& id, const std::string& label, const std::string& control)
    {
        LLCommandIndex::Entry e;
        e.id        = std::string(kIdPrefix) + ":" + id;
        e.label     = label;
        e.path      = "Interface \xe2\x80\xba Status bar";
        e.action    = [control]() { gSavedSettings.setBOOL(control, !gSavedSettings.getBOOL(control)); };
        e.isChecked = [control]() { return gSavedSettings.getBOOL(control); };
        LLCommandIndex::addCommand(std::move(e));
    };
    toggle("bar", "Status bar", "UINewStatusBar");
    for (const Part& p : kParts)
        toggle(p.control, std::string("Status bar: ") + p.label, p.control);
    LLCommandIndex::Entry menus;
    menus.id     = std::string(kIdPrefix) + ":menus";
    menus.label  = "Browse the classic menus";
    menus.path   = "Interface";
    menus.action = []()
    {
        LLNewUIMenus::instance().close();
        click("menus");
    };
    LLCommandIndex::addCommand(std::move(menus));
}
} // namespace

LLNewUIStatus& LLNewUIStatus::instance()
{
    static LLNewUIStatus status;
    return status;
}

bool LLNewUIStatus::active() const { return gState != nullptr; }

int LLNewUIStatus::bottomDp() const { return gState && gState->shown ? gState->bottom : 0; }

bool LLNewUIStatus::start(Rml::Context* context, const DocumentLoader& load)
{
    if (gState || !context)
        return gState != nullptr;
    State* s = new (std::nothrow) State();
    if (!s)
        return false;
    try
    {
        s->context                  = context;
        Rml::DataModelConstructor m = context->CreateDataModel("status");
        if (!m)
            throw std::bad_alloc();
        // (kParts' order; by name, so the newui-kit check sees the document's variables bound)
        m.Bind("showParcel", &s->show[0]);
        m.Bind("showFps", &s->show[1]);
        m.Bind("showNet", &s->show[2]);
        m.Bind("showBandwidth", &s->show[3]);
        m.Bind("showTime", &s->show[4]);
        m.Bind("showBalance", &s->show[5]);
        m.Bind("showMedia", &s->show[6]);
        m.Bind("showVolume", &s->show[7]);
        m.Bind("showPresets", &s->show[8]);
        m.Bind("showGestures", &s->show[9]);
        static_assert(kPartCount == 10, "bind every part above");
        m.Bind("rating", &s->rating);
        m.Bind("health", &s->health);
        m.Bind("fps", &s->fps);
        m.Bind("fpsLevel", &s->fpsLevel);
        m.Bind("ping", &s->ping);
        m.Bind("pingLevel", &s->pingLevel);
        m.Bind("loss", &s->loss);
        m.Bind("lossLevel", &s->lossLevel);
        m.Bind("bandwidth", &s->bandwidth);
        m.Bind("time", &s->time);
        m.Bind("balance", &s->balance);
        m.Bind("mediaIcon", &s->mediaIcon);
        m.Bind("mediaUsable", &s->mediaUsable);
        m.Bind("volumeIcon", &s->volumeIcon);
        m.Bind("muted", &s->muted);
        m.Bind("menuOn", &s->menuOn);
        m.Bind("noVoice", &s->noVoice);
        m.Bind("noFly", &s->noFly);
        m.Bind("noPush", &s->noPush);
        m.Bind("noBuild", &s->noBuild);
        m.Bind("noScripts", &s->noScripts);
        m.Bind("noSeeAvs", &s->noSeeAvs);
        m.Bind("damage", &s->damage);
        m.Bind("tip", &s->tip);
        m.Bind("tipWhat", &s->tipWhat);
        m.Bind("tipClick", &s->tipClick);
        m.BindEventCallback("click", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            { if (!a.empty()) click(a[0].Get<Rml::String>()); });
        s->model = m.GetModelHandle();
    }
    catch (const std::bad_alloc&)
    {
        delete s;
        return false;
    }
    gState = s;
    s->doc = load(LLNewUIStatusDocs::kStatusRml, "newui-status.rml");
    if (!s->doc)
    {
        stop();
        return false;
    }
    if (!LLNewUIMenus::instance().start(context, load))
        LL_WARNS("RmlUi") << "New interface: the status bar's menus could not start" << LL_ENDL;
    registerCommands();
    refreshShown(s);
    LL_INFOS("RmlUi") << "New interface: status bar ready" << LL_ENDL;
    return true;
}

bool LLNewUIStatus::reload(const DocumentLoader& load)
{
    State* s = gState;
    if (!s)
        return false;
    LLNewUIMenus::instance().reload(load);
    Rml::ElementDocument* fresh = load(LLNewUIStatusDocs::kStatusRml, "newui-status.rml");
    if (!fresh)
        return false; // keep the old look rather than lose the bar
    const bool shown = s->doc && s->doc->IsVisible();
    if (s->doc)
        s->doc->Close();
    s->doc = fresh;
    if (shown)
        s->doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    s->top = -1;
    s->model.DirtyAllVariables();
    s->changed = true;
    return true;
}

void LLNewUIStatus::stop()
{
    State* s = gState;
    if (!s)
        return;
    LLCommandIndex::removeCommands(kIdPrefix);
    LLNewUIMenus::instance().stop(); // the classic row back
    if (s->doc)
        s->doc->Close();
    if (s->context)
        s->context->RemoveDataModel("status");
    delete s;
    gState = nullptr;
}

bool LLNewUIStatus::ownsDocument(const Rml::ElementDocument* doc) const
{
    return gState && doc && (doc == gState->doc || LLNewUIMenus::instance().ownsDocument(doc));
}

bool LLNewUIStatus::sync(int barRight, int barBottom, float density)
{
    State* s = gState;
    if (!s)
        return false;
    bool changed = LLNewUIMenus::instance().sync();
    // In world, the bar on, not in mouselook (the classic status bar hides there too).
    const bool want = LLStartUp::getStartupState() >= STATE_STARTED && gSavedSettings.getBOOL("UINewStatusBar") &&
                      !gAgentCamera.cameraMouselook();
    if (want != s->shown)
    {
        s->shown = want;
        if (want)
            s->doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        else
        {
            s->doc->Hide();
            LLNewUIMenus::instance().close();
        }
        s->changed = true;
    }
    const F64 now = LLTimer::getTotalSeconds();
    if (s->shown && now >= s->nextRefresh)
    {
        s->nextRefresh = now + kRefreshPeriod;
        refreshShown(s);
        refreshParcel(s);
        refreshFps(s);
        refreshNet(s);
        refreshTime(s);
        refreshBalance(s);
        refreshMedia(s);
        const LLNewUIMenus::Page page = LLNewUIMenus::instance().openPage();
        set(s, s->menuOn, std::string(page == LLNewUIMenus::Page::Balance ? "balance" : page == LLNewUIMenus::Page::More ? "more" : ""),
            "menuOn");
    }
    // At the window's top edge, right; under the main bar when the two would meet (a narrow window).
    if (s->shown && density > 0.f)
    {
        int top = kGapDp;
        if (Rml::Element* stats = s->doc->GetElementById("stats"); stats && barRight > 0)
        {
            const float rootW = float(s->context->GetDimensions().x) / density;
            const float width = stats->GetOffsetWidth() / density;
            if (rootW - kRightDp - width < float(barRight + kGapDp))
                top = barBottom + kGapDp;
        }
        if (top != s->top)
        {
            s->top = top;
            if (Rml::Element* wrap = s->doc->GetElementById("statwrap"))
                wrap->SetProperty("top", std::to_string(top) + "dp");
            s->changed = true;
        }
        updateHover(s, density);
        // (laid out last frame: a move settles a frame later)
        if (Rml::Element* stats = s->doc->GetElementById("stats"))
            s->bottom = int(std::ceil((stats->GetAbsoluteTop() + stats->GetOffsetHeight()) / density)) + kGapDp;
    }
    changed |= s->changed;
    s->changed = false;
    return changed;
}

bool LLNewUIStatus::classicRowHidden()
{
    return LLNewUIMenus::classicRowHidden();
}

void LLNewUIStatus::restoreClassic()
{
    LLNewUIMenus::hideClassicRow(false);
}
