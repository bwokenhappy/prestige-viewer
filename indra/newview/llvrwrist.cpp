/**
 * @file llvrwrist.cpp
 * @brief VR wrist panel (#1120). See llvrwrist.h.
 *
 * Pieces (kept apart so the drawing can move to another UI backend):
 *  - Layout: app_settings/vr_wrist_panel.xml -> Layout (mm, colours, mount, show / touch
 *    thresholds, actions). panelTargets() turns it plus the feed into the panel's hit targets
 *    (rects in mm, top-left origin); the hit test and the drawing both use that one list.
 *  - Feed: a fixed ring of Items (IMs, nearby chat, notifications / dialogs), fed by
 *    LLIMModel's new-message signal, LLNotificationManager::onChat and the "Visible"
 *    notification channel. Text lengths are capped.
 *  - Placement: the quad's pose in the left aim pose's frame (LLXrUiLayers::wrist*), and the
 *    same quad in app space for the hit test.
 *  - Pointer: the right hand's laser (aim ray + trigger) and touch (its tip pushed onto the
 *    panel).
 *  - Drawing (render()): gGL rects + LLFontGL text into a facade render target (an offscreen
 *    segment of the UI build), only when something changed; LLXrSession copies the image.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrwrist.h"

#include "llagent.h"
#include "llchat.h"
#include "llfloaterimnearbychat.h"
#include "llfloaterimsession.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llimview.h"
#include "llnotifications.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsdserialize.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llvrbody.h"
#include "llvrbodysource.h"
#include "llvrcamera.h"
#include "llvrdictation.h"
#include "llvrwristgrid.h"
#include "vkgfx/llrendervkbackend.h"
#include "vkgfx/llvrspacewarp.h"
#include "vkgfx/llxrsession.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

// ---- Layout ------------------------------------------------------------------------------------

struct RectMm
{
    F32 x = 0.f, y = 0.f, w = 0.f, h = 0.f;   // top-left origin, millimetres
    bool contains(F32 px, F32 py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct ActionDef
{
    std::string action;   // wall | say | dictate | recenter | flycam | nearby | conversations | notifications | body | grab | settings | vr_settings
    std::string label;    // a strings.xml name, or the text itself
};

struct Layout
{
    F32 sizeMm[2] = {210.f, 136.f};
    F32 marginMm = 5.f;
    F32 lineMm = 14.f;
    std::string fontName = "SansSerif";
    std::string fontSize = "VRWrist";
    F32 mountOffset[3] = {0.f, 0.035f, 0.12f};
    F32 liftM = 0.02f;
    F32 tiltMaxDeg = 35.f;
    F32 onDot = 0.55f, offDot = 0.35f, maxDistM = 0.85f, showDelay = 0.10f, hideDelay = 0.35f;
    F32 tipM = 0.01f, hoverMm = 70.f, pressMm = 8.f, releaseMm = 22.f, throughMm = 60.f;
    RectMm header{5.f, 5.f, 200.f, 14.f};
    RectMm feed{5.f, 21.f, 200.f, 68.f};
    F32 rowMm = 15.f, rowGapMm = 2.f;
    U32 maxItems = 12;
    bool feedIm = true, feedNearby = true, feedNotices = true;
    F32 dialogButtonMm = 24.f;
    U32 maxButtons = 3;
    // The quick actions: a grid of at most `columns` buttons per row (rows grow with the number of
    // actions, the panel grows with the rows). actions.h is the smallest a button is; a button whose
    // label needs more lines than fit is made taller (computeGrid).
    RectMm actions{5.f, 95.f, 200.f, 36.f};
    F32 actionGapMm = 3.5f;
    S32 columns = 3;
    F32 bottomMm = 5.f;       // below the last row (size_mm's height less the actions' bottom edge)
    F32 labelPadMm = 3.f;     // between a button's edge and its text
    std::vector<ActionDef> items;
    bool settingsButton = true;   // #1590 / #1591 the header's Settings button (the feed shows the settings page)
    LLColor4 background{0.07f, 0.08f, 0.10f, 1.f}, row{0.12f, 0.13f, 0.16f, 1.f}, text{0.95f, 0.95f, 0.95f, 1.f},
        dim{0.62f, 0.65f, 0.70f, 1.f}, button{0.20f, 0.23f, 0.29f, 1.f}, hover{0.27f, 0.39f, 0.55f, 1.f},
        press{0.33f, 0.62f, 0.90f, 1.f}, active{0.22f, 0.45f, 0.30f, 1.f}, im{0.35f, 0.65f, 1.f, 1.f},
        nearby{0.45f, 0.85f, 0.50f, 1.f}, notice{0.95f, 0.75f, 0.30f, 1.f}, dialog{1.f, 0.50f, 0.30f, 1.f},
        cursor{0.35f, 0.85f, 1.f, 1.f};
};

F32 sdF32(const LLSD& m, const char* key, F32 def, F32 lo, F32 hi)
{
    if (!m.isMap() || !m.has(key)) return def;
    const F32 v = (F32)m[key].asReal();
    return std::isfinite(v) ? llclamp(v, lo, hi) : def;
}

void sdVec(const LLSD& m, const char* key, F32* out, int n, F32 lo, F32 hi)
{
    if (!m.isMap() || !m.has(key) || !m[key].isArray() || m[key].size() < (size_t)n) return;
    for (int i = 0; i < n; ++i)
    {
        const F32 v = (F32)m[key][i].asReal();
        if (std::isfinite(v)) out[i] = llclamp(v, lo, hi);
    }
}

void sdRect(const LLSD& m, RectMm& r)
{
    F32 v[4] = {r.x, r.y, r.w, r.h};
    sdVec(m, "rect_mm", v, 4, 0.f, 1000.f);
    r = RectMm{v[0], v[1], v[2], v[3]};
}

void sdColor(const LLSD& m, const char* key, LLColor4& c)
{
    F32 v[4] = {c.mV[0], c.mV[1], c.mV[2], c.mV[3]};
    sdVec(m, key, v, 4, 0.f, 1.f);
    c.set(v[0], v[1], v[2], v[3]);
}

void defaultActions(Layout& L)
{
    L.items = {{"wall", "VRWristWall"}, {"say", "VRWristDictate"}, {"recenter", "VRWristRecenter"},
               {"flycam", "VRWristFlyCam"}, {"body", "VRWristBody"}, {"bodymap", "VRWristBodyMap"}};
}

// The layout file (once). Every key optional; a missing / broken file keeps the built-in values.
Layout loadLayout()
{
    Layout L;
    defaultActions(L);
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "vr_wrist_panel.xml");
    LLSD sd;
    std::ifstream in(path.c_str());
    if (!in.is_open() || LLSDSerialize::fromXMLDocument(sd, in) <= 0 || !sd.isMap())
    {
        LL_WARNS("VRWrist") << "layout: " << path << " missing or unreadable, using the built-in layout" << LL_ENDL;
        return L;
    }
    sdVec(sd, "size_mm", L.sizeMm, 2, 40.f, 600.f);
    L.marginMm = sdF32(sd, "margin_mm", L.marginMm, 0.f, 50.f);
    L.lineMm = sdF32(sd, "line_mm", L.lineMm, 4.f, 60.f);
    if (sd.has("font"))
    {
        const LLSD& f = sd["font"];
        if (f.has("name") && !f["name"].asString().empty()) L.fontName = f["name"].asString();
        if (f.has("size") && !f["size"].asString().empty()) L.fontSize = f["size"].asString();
    }
    const LLSD& mount = sd["mount"];
    sdVec(mount, "offset_m", L.mountOffset, 3, -0.5f, 0.5f);
    L.liftM = sdF32(mount, "lift_m", L.liftM, -0.2f, 0.2f);
    L.tiltMaxDeg = sdF32(mount, "tilt_max_deg", L.tiltMaxDeg, 0.f, 80.f);
    const LLSD& show = sd["show"];
    L.onDot = sdF32(show, "on_dot", L.onDot, -1.f, 1.f);
    L.offDot = sdF32(show, "off_dot", L.offDot, -1.f, L.onDot);
    L.maxDistM = sdF32(show, "max_dist_m", L.maxDistM, 0.1f, 3.f);
    L.showDelay = sdF32(show, "show_delay_s", L.showDelay, 0.f, 3.f);
    L.hideDelay = sdF32(show, "hide_delay_s", L.hideDelay, 0.f, 5.f);
    const LLSD& touch = sd["touch"];
    L.tipM = sdF32(touch, "tip_m", L.tipM, -0.2f, 0.2f);
    L.hoverMm = sdF32(touch, "hover_mm", L.hoverMm, 5.f, 300.f);
    L.pressMm = sdF32(touch, "press_mm", L.pressMm, -50.f, L.hoverMm);
    L.releaseMm = sdF32(touch, "release_mm", L.releaseMm, L.pressMm + 1.f, L.hoverMm + 50.f);
    L.throughMm = sdF32(touch, "through_mm", L.throughMm, 0.f, 300.f);
    sdRect(sd["header"], L.header);
    const LLSD& feed = sd["feed"];
    sdRect(feed, L.feed);
    L.rowMm = sdF32(feed, "row_mm", L.rowMm, 4.f, 100.f);
    L.rowGapMm = sdF32(feed, "row_gap_mm", L.rowGapMm, 0.f, 30.f);
    L.maxItems = (U32)llclamp(feed.has("max_items") ? feed["max_items"].asInteger() : (S32)L.maxItems, 1, 32);
    if (feed.has("im")) L.feedIm = feed["im"].asBoolean();
    if (feed.has("nearby")) L.feedNearby = feed["nearby"].asBoolean();
    if (feed.has("notices")) L.feedNotices = feed["notices"].asBoolean();
    L.dialogButtonMm = sdF32(feed, "dialog_button_mm", L.dialogButtonMm, 6.f, 100.f);
    L.maxButtons = (U32)llclamp(feed.has("max_buttons") ? feed["max_buttons"].asInteger() : (S32)L.maxButtons, 0, 4);
    const LLSD& acts = sd["actions"];
    sdRect(acts, L.actions);
    L.actionGapMm = sdF32(acts, "gap_mm", L.actionGapMm, 0.f, 50.f);
    L.columns = llclamp(acts.has("columns") ? (S32)acts["columns"].asInteger() : L.columns, 1, 6);
    L.bottomMm = llmax(2.f, L.sizeMm[1] - (L.actions.y + L.actions.h));
    if (sd["header"].has("settings_button")) L.settingsButton = sd["header"]["settings_button"].asBoolean();
    if (acts.has("items") && acts["items"].isArray())
    {
        L.items.clear();
        for (LLSD::array_const_iterator it = acts["items"].beginArray(); it != acts["items"].endArray(); ++it)
        {
            if (!it->isMap() || !it->has("action") || L.items.size() >= 8) continue;
            L.items.push_back({(*it)["action"].asString(), (*it)["label"].asString()});
        }
        if (L.items.empty()) defaultActions(L);
    }
    const LLSD& c = sd["colors"];
    sdColor(c, "background", L.background);
    sdColor(c, "row", L.row);
    sdColor(c, "text", L.text);
    sdColor(c, "dim", L.dim);
    sdColor(c, "button", L.button);
    sdColor(c, "hover", L.hover);
    sdColor(c, "press", L.press);
    sdColor(c, "active", L.active);
    sdColor(c, "im", L.im);
    sdColor(c, "nearby", L.nearby);
    sdColor(c, "notice", L.notice);
    sdColor(c, "dialog", L.dialog);
    sdColor(c, "cursor", L.cursor);
    LL_INFOS("VRWrist") << "layout: " << path << " -- " << L.sizeMm[0] << " x " << L.sizeMm[1] << " mm, line " << L.lineMm
                        << " mm (grows with the action rows), font " << L.fontName << "/" << L.fontSize << ", " << L.items.size()
                        << " actions, up to " << L.columns << " per row" << LL_ENDL;
    return L;
}

std::string labelText(const std::string& key)
{
    std::string s;
    if (!key.empty() && LLTrans::findString(s, key)) return s;
    return key;
}

// ---- Feed --------------------------------------------------------------------------------------

constexpr S32 kMaxFromChars = 64;
constexpr S32 kMaxTextChars = 300;

struct Item
{
    enum Kind : U8 { IM, NEARBY, NOTICE };
    Kind kind = NOTICE;
    std::string from, text;
    LLUUID session;     // IM: its session
    LLUUID notif;       // NOTICE: the notification
    struct Btn
    {
        std::string name, label;
    };
    std::vector<Btn> buttons;   // a dialog's own buttons (answerable here), at most Layout::maxButtons
    bool dialog = false;        // waits for an answer
    bool answered = false;
    bool unread = true;
};

// One line of plain text: no line breaks, no <nolink> tags, capped.
std::string flatten(const std::string& in, S32 maxChars)
{
    std::string s = in;
    for (const char* tag : {"<nolink>", "</nolink>"})
    {
        for (size_t p = s.find(tag); p != std::string::npos; p = s.find(tag)) s.erase(p, std::strlen(tag));
    }
    for (char& ch : s)
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    LLStringUtil::trim(s);
    return utf8str_truncate(s, maxChars);
}

// ---- State -------------------------------------------------------------------------------------

struct Target
{
    enum Type : U8 { ACTION, ROW, DIALOG_BUTTON, DIALOG_WALL, PAGE, SETTING, MENU };
    Type type = ACTION;
    RectMm r;
    S32 index = 0;   // ACTION: the layout's action; ROW / DIALOG_*: the feed item; SETTING: a Setting
    S32 sub = 0;     // DIALOG_BUTTON: the dialog's button; SETTING: -1 smaller, +1 bigger, 0 the switch
};

// #1590 / #1591 the settings page (the header's Settings button; it covers the feed): the grab
// switch and the UI wall's size, text size and distance, each a step per press, applied live
// (LLVRGrab / LLVRInput read the settings every frame).
enum Setting : S32 { SET_GRAB, SET_WALL, SET_TEXT, SET_DIST, SET_COUNT };

bool grabEnabled()
{
    static LLCachedControl<bool> s_grab(gSavedSettings, "SlipstreamVRGrabEnabled", true);
    return s_grab;
}

F32 settingValue(S32 which)
{
    static LLCachedControl<F32> s_angle(gSavedSettings, "SlipstreamVRWallAngle", 110.f);
    static LLCachedControl<F32> s_ui(gSavedSettings, "SlipstreamVRUIScale", 1.f);
    static LLCachedControl<F32> s_dist(gSavedSettings, "SlipstreamVRWallDistance", 2.5f);
    switch (which)
    {
    case SET_WALL: return s_angle;
    case SET_TEXT: return s_ui;
    case SET_DIST: return s_dist;
    default: return grabEnabled() ? 1.f : 0.f;
    }
}

void stepSetting(S32 which, S32 dir)
{
    if (which == SET_GRAB)
    {
        gSavedSettings.setBOOL("SlipstreamVRGrabEnabled", !grabEnabled());
        LL_INFOS("VRWrist") << "grab " << (grabEnabled() ? "on" : "off") << LL_ENDL;
        return;
    }
    // (the same limits LLVRInput clamps to; rounded so repeated steps do not drift)
    const char* name = which == SET_WALL ? "SlipstreamVRWallAngle" : which == SET_TEXT ? "SlipstreamVRUIScale" : "SlipstreamVRWallDistance";
    const F32 step = which == SET_WALL ? 10.f : which == SET_TEXT ? 0.1f : 0.25f;
    const F32 lo = which == SET_WALL ? 60.f : which == SET_TEXT ? 0.5f : 1.f;
    const F32 hi = which == SET_WALL ? 160.f : which == SET_TEXT ? 2.f : 5.f;
    F32 v = settingValue(which);
    if (!std::isfinite(v)) v = lo;
    v = llclamp(std::round((v + step * (F32)dir) / step) * step, lo, hi);
    gSavedSettings.setF32(name, v);
    LL_INFOS("VRWrist") << "setting " << name << " = " << v << LL_ENDL;
}

std::string settingLabel(S32 which)
{
    LLStringUtil::format_map_t args;
    switch (which)
    {
    case SET_WALL:
        args["[VALUE]"] = llformat("%d", (S32)std::lround(settingValue(which)));
        return LLTrans::getString("VRWristWallSize", args);
    case SET_TEXT:
        args["[VALUE]"] = llformat("%d", (S32)std::lround(settingValue(which) * 100.f));
        return LLTrans::getString("VRWristTextSize", args);
    case SET_DIST:
        args["[VALUE]"] = llformat("%.2f", settingValue(which));
        return LLTrans::getString("VRWristDistance", args);
    default:
        return LLTrans::getString(grabEnabled() ? "VRWristGrabOn" : "VRWristGrabOff");
    }
}

// The quick actions' grid (computeGrid): which actions are shown, how many buttons in each row, each
// row's height. Built from the layout and the live labels, never from coordinates in the layout file.
struct ActionGrid
{
    std::vector<S32> shown;    // indices into Layout::items
    std::vector<S32> perRow;   // buttons in each row
    std::vector<F32> rowH;     // each row's height (mm)
};

struct WristState
{
    bool loaded = false;
    Layout L;
    ActionGrid grid;
    F32 panelH = 136.f;       // the panel's height (mm): the layout's, grown by the action rows
    std::string drawnWarp;    // #1594 the header's space warp text as drawn
    bool connected = false;
    boost::signals2::connection imConn;
    LLBoundListener notifConn;
    std::deque<Item> feed;

    // Visibility.
    bool shown = false;
    F32 onTimer = 0.f, offTimer = 0.f;
    F32 shownFor = 0.f;
    bool formatLogged = false;

    // The quad this frame (app space): centre, axes (+X right, +Y up, +Z normal toward the viewer),
    // size (m).
    bool geom = false;
    F32 c[3] = {0.f, 0.f, 0.f}, ax[3] = {1.f, 0.f, 0.f}, ay[3] = {0.f, 1.f, 0.f}, an[3] = {0.f, 0.f, 1.f};
    F32 pw = 0.21f, ph = 0.136f;

    // Pointer (the right hand).
    enum Src : U8 { SRC_NONE, SRC_LASER, SRC_TOUCH };
    Src src = SRC_NONE;
    F32 curX = 0.f, curY = 0.f;   // mm on the panel
    F32 depthMm = 0.f;            // touch: the tip's distance in front of the panel
    bool touchDown = false;
    bool touchArmed = false;      // the fingertip was in front of the press depth (came from the front)
    bool prevSelect = false;
    S32 hover = -1;               // the target under the cursor
    F32 pressFlash = 0.f;         // s left of the pressed look on `flashTarget`
    S32 flashTarget = -1;
    F32 beam = 0.f;
    bool rightOn = false;

    // Drawing.
    std::vector<Target> targets;
    bool dirty = true;
    U32 rt = 0, rtW = 0, rtH = 0;
    F32 pxPerMm = 2.f;
    Clock::time_point rtRetryAt{};
    bool swapRB = false;
    S32 drawnMinute = -1;
    bool drawnWall = false, drawnFly = false;
    bool page = false;                              // #1590 / #1591 the settings page covers the feed
    bool drawnPage = false;
    F32 drawnSettings[SET_COUNT] = {-1.f, -1.f, -1.f, -1.f};
    bool drawnBody = false, drawnBodyCal = false;   // #1565 the body button shown / counting down
    U32 drawnBodySrc = 0;                           // the body source named on it
    S32 drawnHover = -2, drawnFlash = -2;
    F32 drawnCurX = -1e6f, drawnCurY = -1e6f, drawnDepth = -1e6f;
    Src drawnSrc = SRC_NONE;
    bool wallUp = false;
    U32 redraws = 0;
    Clock::time_point lastBuzz{};
    Clock::time_point lastDialogCheck{};

    // #1119 dictation over the feed (LLVRDictation::wristStatus, read each update): the status
    // line, the preview's transcript and the dot; drawnDict: what the image shows (redraw on change).
    bool dictOn = false;
    std::string dictStatus, dictQuote, drawnDict;
    F32 dictDot[4] = {0.6f, 0.6f, 0.6f, 1.f};
    bool drawnSay = false;
};
WristState sW;

void markDirty()
{
    sW.dirty = true;
}

void pushItem(Item&& it)
{
    sW.feed.push_front(std::move(it));
    while (sW.feed.size() > sW.L.maxItems) sW.feed.pop_back();
    markDirty();
}

// A short tick on the left hand for an IM / dialog, at most every 2 s (never for nearby chat).
void buzzLeft()
{
    const Clock::time_point now = Clock::now();
    if (now - sW.lastBuzz < std::chrono::seconds(2)) return;
    sW.lastBuzz = now;
    LLXrSession::instance().haptic(LLXR_HAND_LEFT, 0.3f, 0.06f);
}

void onNewIM(const LLSD& arg)
{
    if (!sW.L.feedIm) return;
    const LLUUID from_id = arg["from_id"].asUUID();
    if (from_id == gAgentID) return;   // (our own lines)
    // Friend online / offline notices and region messages: not conversation (they would fill the
    // feed and tick the hand on every friend's login).
    if (arg["is_friend_status"].asBoolean() || arg["is_region_msg"].asBoolean()) return;
    const std::string text = flatten(arg["message"].asString(), kMaxTextChars);
    if (text.empty()) return;
    Item it;
    it.kind = Item::IM;
    it.from = flatten(arg["from"].asString(), kMaxFromChars);
    if (it.from.empty()) it.from = "IM";
    it.text = text;
    it.session = arg["session_id"].asUUID();
    LL_INFOS("VRWrist") << "feed: IM from " << it.from << " (" << text.size() << " chars)" << LL_ENDL;
    pushItem(std::move(it));
    buzzLeft();
}

bool wantedNotificationType(const std::string& type)
{
    // notifytoast: the IM toasts (the IM itself comes through LLIMModel); hint / browser: not
    // messages. Everything else that is shown: notices, tips, offers, group notices, modal alerts.
    return type == "notify" || type == "notifytip" || type == "offer" || type == "groupnotify" || type == "alertmodal";
}

bool onNotification(const LLSD& payload)
{
    if (!sW.L.feedNotices) return false;
    const std::string sig = payload["sigtype"].asString();
    const LLUUID id = payload["id"].asUUID();
    if (sig == "delete" || sig == "change")
    {
        for (Item& it : sW.feed)
        {
            if (it.notif != id || !it.dialog || it.answered) continue;
            LLNotificationPtr n = LLNotifications::instance().find(id);
            if (sig == "delete" || !n || n->isRespondedTo() || n->isCancelled())
            {
                it.answered = true;
                markDirty();
            }
        }
        return false;
    }
    if (sig != "add") return false;   // (not "load": persisted ones from earlier sessions)
    LLNotificationPtr n = LLNotifications::instance().find(id);
    if (!n || !wantedNotificationType(n->getType())) return false;
    Item it;
    it.kind = Item::NOTICE;
    it.notif = id;
    it.from = flatten(n->getLabel(), kMaxFromChars);
    it.text = flatten(n->getMessage(), kMaxTextChars);
    if (it.text.empty() && it.from.empty()) return false;
    // Its buttons: answerable here when it has only buttons (no text field), few enough to fit.
    const LLNotificationFormPtr form = n->getForm();
    bool fieldsOnly = true;
    if (form)
    {
        for (S32 i = 0; i < form->getNumElements(); ++i)
        {
            const LLSD e = form->getElement(i);
            const std::string t = e["type"].asString();
            if (t == "button")
                it.buttons.push_back({e["name"].asString(), flatten(e["text"].asString(), 32)});
            else if (t == "text" || t == "password")
                fieldsOnly = false;
        }
    }
    it.dialog = !it.buttons.empty() && n->getType() != "notifytip";
    if (!fieldsOnly || it.buttons.size() > sW.L.maxButtons) it.buttons.clear();   // "Open on wall" only
    LL_INFOS("VRWrist") << "feed: notification " << n->getName() << " (" << n->getType() << ")"
                        << (it.dialog ? (it.buttons.empty() ? ", a dialog to answer on the wall"
                                                            : ", a dialog with " + std::to_string(it.buttons.size()) + " buttons")
                                      : "")
                        << LL_ENDL;
    const bool buzz = it.dialog;
    pushItem(std::move(it));
    if (buzz) buzzLeft();
    return false;   // (never stops the channel's other listeners)
}

void connectFeeds()
{
    if (sW.connected) return;
    sW.connected = true;
    sW.imConn = LLIMModel::instance().addNewMsgCallback(&onNewIM);
    if (LLNotificationChannelPtr ch = LLNotifications::instance().getChannel("Visible"))
        sW.notifConn = ch->connectChanged(&onNotification);
    LL_INFOS("VRWrist") << "feed: listening to IMs, nearby chat and notifications" << LL_ENDL;
}

// The newest dialog still waiting for an answer (-1: none). Answered / gone ones are checked at
// most twice a second (a response on the wall, a timeout).
S32 pendingDialog()
{
    const Clock::time_point now = Clock::now();
    const bool check = now - sW.lastDialogCheck > std::chrono::milliseconds(500);
    if (check) sW.lastDialogCheck = now;
    S32 found = -1;
    for (S32 i = 0; i < (S32)sW.feed.size(); ++i)
    {
        Item& it = sW.feed[i];
        if (!it.dialog || it.answered) continue;
        if (check)
        {
            LLNotificationPtr n = LLNotifications::instance().find(it.notif);
            if (!n || n->isRespondedTo() || n->isCancelled() || n->isExpired())
            {
                it.answered = true;
                markDirty();
                continue;
            }
        }
        if (found < 0) found = i;
    }
    return found;
}

// The label an action's button shows now (its state may change it) and whether it is "on".
std::string actionLabel(const ActionDef& a, bool* on = nullptr)
{
    std::string label = labelText(a.label);
    bool isOn = false;
    if (a.action == "wall" && sW.wallUp)
    {
        label = labelText("VRWristHideWall");
        isOn = true;
    }
    else if (a.action == "flycam" && LLVRCamera::flyCam())
    {
        label = labelText("VRWristFlyCamOff");
        isOn = true;
    }
    else if (a.action == "say" && LLVRDictation::wristListening())
    {
        label = labelText("VRWristDictateStop");
        isOn = true;
    }
    else if (a.action == "body" && LLVRBody::calibrating())
    {
        label = labelText("VRWristBodyTPose");
        isOn = true;
    }
    else if (a.action == "body" && *LLVRBodySource::shortName(LLVRBodySource::active()))
    {
        // Which body source is in use (Auto: SlimeVR, trackers, headset, 3-point estimate).
        label += std::string(": ") + LLVRBodySource::shortName(LLVRBodySource::active());
    }
    else if (a.action == "grab")
    {
        label = labelText(grabEnabled() ? "VRWristGrab" : "VRWristGrabOffShort");
        isOn = grabEnabled();
    }
    else if (a.action == "settings" && sW.page)
    {
        label = labelText("VRWristSettingsDone");
        isOn = true;
    }
    if (on) *on = isOn;
    return label;
}

// How many lines `text` needs in a button `widthMm` wide, between 1 and kMaxLabelLines (defined with
// the drawing: it measures with the panel's own font).
constexpr size_t kMaxLabelLines = 3;
S32 labelLines(const std::string& text, F32 widthMm);
F32 textLineMm();

// The action grid: at most Layout::columns buttons per row, the buttons spread evenly over the
// rows (7 actions with 3 per row: 3, 2, 2), every button as wide as its row allows. A row is as tall
// as its tallest label needs (Layout::actions.h at least): a label that does not fit on one line
// wraps, and the row grows to hold the lines. The panel's height follows from the rows, so adding
// an action never needs a coordinate: it only adds a button (and a row when the row is full).
void computeGrid()
{
    const Layout& L = sW.L;
    ActionGrid& g = sW.grid;
    g = ActionGrid();
    // #1565: "body" (calibrate the body trackers) only while the runtime lists trackers.
    for (S32 i = 0; i < (S32)L.items.size(); ++i)
        if (L.items[i].action != "body" || LLVRBody::available()) g.shown.push_back(i);
    const S32 na = (S32)g.shown.size();
    F32 total = 0.f;
    if (na > 0)
    {
        const std::vector<int> counts = llvrwrist::gridRows(na, L.columns);
        const F32 lineMm = textLineMm();
        S32 at = 0;
        for (size_t r = 0; r < counts.size(); ++r)
        {
            const S32 n = counts[r];
            const F32 w = (L.actions.w - L.actionGapMm * (F32)(n - 1)) / (F32)n;
            S32 lines = 1;
            for (S32 k = 0; k < n; ++k)
                lines = llmax(lines, labelLines(actionLabel(L.items[g.shown[at + k]]), w - 2.f * L.labelPadMm));
            at += n;
            g.perRow.push_back(n);
            g.rowH.push_back(llmax(L.actions.h, (F32)lines * lineMm + 2.f * L.labelPadMm));
            total += g.rowH.back() + (r > 0 ? L.actionGapMm : 0.f);
        }
    }
    const F32 panelH = na > 0 ? L.actions.y + total + L.bottomMm : L.sizeMm[1];
    if (std::fabs(panelH - sW.panelH) > 0.01f)
    {
        sW.panelH = panelH;
        markDirty();
    }
}

// ---- Targets (shared by the hit test and the drawing) ------------------------------------------

void panelTargets(std::vector<Target>& out)
{
    const Layout& L = sW.L;
    out.clear();
    // Quick actions: the grid (computeGrid).
    {
        const ActionGrid& g = sW.grid;
        F32 y = L.actions.y;
        S32 at = 0;
        for (size_t r = 0; r < g.perRow.size(); ++r)
        {
            const S32 n = g.perRow[r];
            const F32 w = (L.actions.w - L.actionGapMm * (F32)(n - 1)) / (F32)n;
            for (S32 k = 0; k < n; ++k)
                out.push_back({Target::ACTION, RectMm{L.actions.x + (F32)k * (w + L.actionGapMm), y, w, g.rowH[r]}, g.shown[at + k], 0});
            at += n;
            y += g.rowH[r] + L.actionGapMm;
        }
    }
    // #1590 / #1591 the header's Settings button (right of the title, left of the clock).
    if (L.settingsButton)
        out.push_back({Target::PAGE, RectMm{L.header.x + L.header.w * 0.42f, L.header.y, L.header.w * 0.33f, L.header.h}, 0, 0});
    // #1119: dictation's status covers the feed (no rows under it to hit).
    if (sW.dictOn) return;
    if (sW.page && L.settingsButton)
    {
        // One row per setting in the feed's rect: the grab switch across it; the others a
        // smaller / bigger button at the ends (the value between them).
        const F32 rowH = llmin(L.rowMm, (L.feed.h - L.rowGapMm * (F32)(SET_COUNT - 1)) / (F32)SET_COUNT);
        const F32 btnW = llmin(30.f, L.feed.w * 0.2f);
        for (S32 k = 0; k < SET_COUNT; ++k)
        {
            const F32 y = L.feed.y + (F32)k * (rowH + L.rowGapMm);
            if (k == SET_GRAB)
            {
                // (#1593: shared with the button that opens the whole VR settings menu on the wall)
                const F32 half = 0.5f * (L.feed.w - L.rowGapMm);
                out.push_back({Target::SETTING, RectMm{L.feed.x, y, half, rowH}, k, 0});
                out.push_back({Target::MENU, RectMm{L.feed.x + half + L.rowGapMm, y, half, rowH}, 0, 0});
                continue;
            }
            out.push_back({Target::SETTING, RectMm{L.feed.x, y, btnW, rowH}, k, -1});
            out.push_back({Target::SETTING, RectMm{L.feed.x + L.feed.w - btnW, y, btnW, rowH}, k, 1});
        }
        return;
    }
    // The pending dialog at the bottom of the feed: its text row, then its buttons + "Open on wall".
    F32 feedBottom = L.feed.y + L.feed.h;
    const S32 d = pendingDialog();
    if (d >= 0)
    {
        const Item& it = sW.feed[d];
        const S32 nb = (S32)it.buttons.size() + 1;
        const F32 w = (L.feed.w - L.rowGapMm * (F32)(nb - 1)) / (F32)nb;
        // (a label that needs more lines than the button's height holds makes the buttons taller)
        S32 lines = labelLines(labelText("VRWristOpenOnWall"), w - 2.f * L.labelPadMm);
        for (const Item::Btn& bt : it.buttons) lines = llmax(lines, labelLines(bt.label, w - 2.f * L.labelPadMm));
        const F32 btnH = llmax(L.dialogButtonMm, (F32)lines * textLineMm() + 2.f * L.labelPadMm);
        const F32 by = feedBottom - btnH;
        for (S32 b = 0; b < nb; ++b)
        {
            const RectMm r{L.feed.x + (F32)b * (w + L.rowGapMm), by, w, btnH};
            out.push_back({b + 1 < nb ? Target::DIALOG_BUTTON : Target::DIALOG_WALL, r, d, b});
        }
        const F32 ty = by - L.rowGapMm - L.rowMm;
        out.push_back({Target::ROW, RectMm{L.feed.x, ty, L.feed.w, L.rowMm}, d, 1});   // sub 1: the dialog's text row
        feedBottom = ty - L.rowGapMm;
    }
    // The other items, newest first, as many rows as fit.
    F32 y = L.feed.y;
    for (S32 i = 0; i < (S32)sW.feed.size(); ++i)
    {
        if (i == d) continue;
        if (y + L.rowMm > feedBottom + 0.01f) break;
        out.push_back({Target::ROW, RectMm{L.feed.x, y, L.feed.w, L.rowMm}, i, 0});
        y += L.rowMm + L.rowGapMm;
    }
}

S32 targetAt(F32 x, F32 y)
{
    for (S32 i = 0; i < (S32)sW.targets.size(); ++i)
        if (sW.targets[i].r.contains(x, y)) return i;
    return -1;
}

// ---- Math --------------------------------------------------------------------------------------

void rotQ(const float q[4], const float v[3], float out[3])
{
    const float ux = q[0], uy = q[1], uz = q[2], w = q[3];
    const float tx = 2.f * (uy * v[2] - uz * v[1]);
    const float ty = 2.f * (uz * v[0] - ux * v[2]);
    const float tz = 2.f * (ux * v[1] - uy * v[0]);
    out[0] = v[0] + w * tx + (uy * tz - uz * ty);
    out[1] = v[1] + w * ty + (uz * tx - ux * tz);
    out[2] = v[2] + w * tz + (ux * ty - uy * tx);
}

void rotQInv(const float q[4], const float v[3], float out[3])
{
    const float qi[4] = {-q[0], -q[1], -q[2], q[3]};
    rotQ(qi, v, out);
}

void mulQ(const float a[4], const float b[4], float out[4])
{
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

F32 dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// ---- Actions -----------------------------------------------------------------------------------

void hapticPress(bool touch)
{
    LLXrSession::instance().haptic(LLXR_HAND_RIGHT, 0.4f, 0.03f);
    if (touch) LLXrSession::instance().haptic(LLXR_HAND_LEFT, 0.2f, 0.02f);
}

void openItemOnWall(Item& it, LLVRWrist::Requests& req)
{
    req.wallShow = true;
    it.unread = false;
    switch (it.kind)
    {
    case Item::IM:
        if (it.session.notNull()) LLFloaterIMSession::show(it.session);
        LL_INFOS("VRWrist") << "open on wall: IM session with " << it.from << LL_ENDL;
        break;
    case Item::NEARBY:
        LLFloaterIMNearbyChat::startChat(nullptr);
        LL_INFOS("VRWrist") << "open on wall: nearby chat" << LL_ENDL;
        break;
    default:
        // A dialog's own toast / floater is on the wall already; the notifications list for the rest.
        if (!it.dialog || it.answered) LLFloaterReg::showInstance("notification_well_window", LLSD(), true);
        LL_INFOS("VRWrist") << "open on wall: notification " << (it.dialog && !it.answered ? "(its dialog)" : "(the list)") << LL_ENDL;
        break;
    }
    markDirty();
}

// #1593 the VR settings wall menu, on the wall (brought up in front of you if it is hidden).
void openSettingsMenu(LLVRWrist::Requests& req)
{
    req.wallShow = true;
    LLFloaterReg::showInstance("vr_settings", LLSD(), true);
    LL_INFOS("VRWrist") << "open on wall: the VR settings menu" << LL_ENDL;
}

void runAction(const std::string& a, LLVRWrist::Requests& req)
{
    if (a == "wall") req.wallToggle = true;
    else if (a == "flycam") req.flyCam = true;
    else if (a == "recenter") LLVRCamera::recenter();
    else if (a == "body") LLVRBody::requestCalibration();   // #1565 (a T-pose after a short countdown)
    else if (a == "grab") stepSetting(SET_GRAB, 0);          // #1590
    else if (a == "settings") sW.page = !sW.page;            // #1590 / #1591
    else if (a == "vr_settings") openSettingsMenu(req);       // #1593
    else if (a == "bodymap")
    {
        // #1592 Body > Mapping: the body mapping window, on the wall.
        req.wallShow = true;
        LLFloaterReg::showInstance("vr_body_mapping", LLSD(), true);
    }
    else if (a == "say")
    {
        // #1119 to nearby chat, wall up or not: press to listen, press again (or the right stick)
        // to stop; the preview shows over the feed and is said when its countdown ends.
        LLVRDictation::wristToggle();
    }
    else if (a == "dictate" || a == "nearby")
    {
        // Dictation types into the focused field on the wall (the right stick, held): bring up the
        // wall with nearby chat's input focused, ready for it.
        req.wallShow = true;
        LLFloaterIMNearbyChat::startChat(nullptr);
    }
    else if (a == "conversations")
    {
        req.wallShow = true;
        LLFloaterReg::showInstance("im_container", LLSD(), true);
    }
    else if (a == "notifications")
    {
        req.wallShow = true;
        LLFloaterReg::showInstance("notification_well_window", LLSD(), true);
    }
    else
    {
        LL_WARNS("VRWrist") << "unknown action '" << a << "' in the layout" << LL_ENDL;
        return;
    }
    LL_INFOS("VRWrist") << "button: " << a << LL_ENDL;
}

void activate(S32 ti, bool touch, LLVRWrist::Requests& req)
{
    if (ti < 0 || ti >= (S32)sW.targets.size()) return;
    const Target t = sW.targets[ti];
    hapticPress(touch);
    sW.flashTarget = ti;
    sW.pressFlash = 0.15f;
    markDirty();
    LL_INFOS("VRWrist") << "press (" << (touch ? "touch" : "laser") << ") at " << sW.curX << ", " << sW.curY << " mm" << LL_ENDL;
    switch (t.type)
    {
    case Target::ACTION:
        if (t.index < (S32)sW.L.items.size()) runAction(sW.L.items[t.index].action, req);
        break;
    case Target::ROW:
        if (t.index < (S32)sW.feed.size()) openItemOnWall(sW.feed[t.index], req);
        break;
    case Target::DIALOG_WALL:
        if (t.index < (S32)sW.feed.size()) openItemOnWall(sW.feed[t.index], req);
        break;
    case Target::PAGE:
        sW.page = !sW.page;
        LL_INFOS("VRWrist") << "settings page " << (sW.page ? "shown" : "closed") << LL_ENDL;
        break;
    case Target::SETTING:
        stepSetting(t.index, t.sub);
        break;
    case Target::MENU:
        openSettingsMenu(req);
        break;
    case Target::DIALOG_BUTTON:
    {
        if (t.index >= (S32)sW.feed.size()) break;
        Item& it = sW.feed[t.index];
        if (t.sub >= (S32)it.buttons.size()) break;
        LLNotificationPtr n = LLNotifications::instance().find(it.notif);
        if (!n || n->isRespondedTo() || n->isCancelled())
        {
            it.answered = true;
            LL_INFOS("VRWrist") << "dialog: already answered / gone" << LL_ENDL;
            break;
        }
        LLSD response = n->getResponseTemplate();
        response[it.buttons[t.sub].name] = true;
        LL_INFOS("VRWrist") << "dialog: " << n->getName() << " answered '" << it.buttons[t.sub].name << "' on the wrist" << LL_ENDL;
        it.answered = true;
        it.unread = false;
        n->respond(response);   // (may post a follow-up notification: the feed gets it)
        break;
    }
    }
}

// ---- Drawing -----------------------------------------------------------------------------------

LLColor4 col(const LLColor4& c)
{
    return sW.swapRB ? LLColor4(c.mV[2], c.mV[1], c.mV[0], c.mV[3]) : c;
}

// The panel's font (what render() draws with) and the image density it is drawn at (ensureTarget).
const LLFontGL* wristFont()
{
    const LLFontGL* font = LLFontGL::getFont(LLFontDescriptor(sW.L.fontName, sW.L.fontSize, 0));
    return font ? font : LLFontGL::getFontSansSerifHuge();
}

F32 drawDensity(const LLFontGL* font)
{
    const F32 linePx = (F32)font->getLineHeight() * LLFontGL::sScaleY;
    return llmax(linePx / sW.L.lineMm, 1.5f);
}

struct Painter
{
    const LLFontGL* font = nullptr;
    F32 k = 2.f;     // px per mm
    S32 H = 1;       // image height (px)
    S32 px(F32 mm) const { return (S32)std::lround(mm * k); }
    S32 pyTop(F32 mm) const { return H - (S32)std::lround(mm * k); }   // y-up pixels of a top-left mm
    void fill(const RectMm& r, const LLColor4& c) const
    {
        gl_rect_2d(px(r.x), pyTop(r.y), px(r.x + r.w), pyTop(r.y + r.h), col(c), true);
    }
    // One line of text, vertically centred at (xMm, yMm centre), at most maxMm wide (ellipsis).
    // Returns its right edge (mm).
    F32 text(const std::string& s, F32 xMm, F32 yMm, const LLColor4& c, LLFontGL::HAlign ha, F32 maxMm) const
    {
        if (!font || s.empty()) return xMm;
        const F32 sx = LLFontGL::sScaleX, sy = LLFontGL::sScaleY;
        F32 right = 0.f;
        font->renderUTF8(s, 0, (F32)px(xMm) / sx, ((F32)H - yMm * k) / sy, col(c), ha, LLFontGL::VCENTER, LLFontGL::NORMAL,
                         LLFontGL::NO_SHADOW, S32_MAX, llmax(1, (S32)(maxMm * k / sx)), &right, true);
        return right * sx / k;
    }
};

// `text` word-wrapped to maxMm, appended to out (at most maxLines in all; the last one that does
// not fit ends in "...").
void wrapMm(const Painter& P, const std::string& text, F32 maxMm, size_t maxLines, std::vector<std::string>& out)
{
    if (!P.font) return;
    const F32 maxPx = maxMm * P.k / LLFontGL::sScaleX;   // (font units, as Painter::text)
    const LLWString w = utf8str_to_wstring(text);
    size_t off = 0;
    while (off < w.size() && out.size() < maxLines)
    {
        while (off < w.size() && w[off] == ' ') ++off;
        if (off >= w.size()) break;
        S32 n = P.font->maxDrawableChars(w.c_str() + off, maxPx, (S32)(w.size() - off), LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
        if (n <= 0) n = 1;
        LLWString line = w.substr(off, (size_t)n);
        off += (size_t)n;
        if (out.size() + 1 == maxLines && off < w.size())
        {
            const LLWString dots = utf8str_to_wstring("...");
            while (!line.empty() && (F32)P.font->getWidth((line + dots).c_str()) > maxPx) line.pop_back();
            line += dots;
        }
        out.push_back(wstring_to_utf8str(line));
    }
}

F32 textLineMm()
{
    const LLFontGL* font = wristFont();
    return font ? (F32)font->getLineHeight() * LLFontGL::sScaleY / drawDensity(font) : sW.L.lineMm;
}

S32 labelLines(const std::string& text, F32 widthMm)
{
    const LLFontGL* font = wristFont();
    if (!font || widthMm < 4.f) return 1;
    Painter P;
    P.font = font;
    P.k = drawDensity(font);
    std::vector<std::string> lines;
    wrapMm(P, text, widthMm, kMaxLabelLines, lines);
    return llclamp((S32)lines.size(), 1, (S32)kMaxLabelLines);
}

// A button's label, centred in `r`: one line where it fits, else wrapped onto as many lines as the
// button's height holds (the layout grows a button that needs more; a label that still does not
// fit ends in "...").
void drawLabel(const Painter& P, const std::string& label, const RectMm& r, const LLColor4& c)
{
    const F32 pad = sW.L.labelPadMm;
    const F32 lineMm = P.font ? (F32)P.font->getLineHeight() * LLFontGL::sScaleY / P.k : sW.L.lineMm;
    const size_t fit = (size_t)llclamp((S32)((r.h - 2.f * pad + 0.5f) / llmax(1.f, lineMm)), 1, (S32)kMaxLabelLines);
    std::vector<std::string> lines;
    wrapMm(P, label, r.w - 2.f * pad, fit, lines);
    if (lines.empty()) return;
    const F32 top = r.y + 0.5f * (r.h - (F32)lines.size() * lineMm);
    for (size_t i = 0; i < lines.size(); ++i)
        P.text(lines[i], r.x + 0.5f * r.w, top + ((F32)i + 0.5f) * lineMm, c, LLFontGL::HCENTER, r.w - 2.f * pad + 1.f);
}

// #1119 dictation (LLVRDictation::wristStatus) over the feed: the status dot, the status line,
// then the preview's transcript, wrapped into the feed's rect.
void drawDictation(const Painter& P)
{
    const Layout& L = sW.L;
    const RectMm& r = L.feed;
    P.fill(r, L.row);
    const F32 lineMm = P.font ? (F32)P.font->getLineHeight() * LLFontGL::sScaleY / P.k : L.lineMm;
    const size_t maxLines = (size_t)llmax(1, (S32)((r.h - 4.f) / llmax(1.f, lineMm)));
    const F32 dotMm = 3.f, textX = r.x + 4.f + 2.f * dotMm + 3.f, textW = r.x + r.w - 3.f - textX;
    std::vector<std::string> lines;
    wrapMm(P, sW.dictStatus, textW, maxLines > 1 && !sW.dictQuote.empty() ? maxLines - 1 : maxLines, lines);
    const size_t statusLines = lines.size();
    if (!sW.dictQuote.empty()) wrapMm(P, sW.dictQuote, textW, maxLines, lines);
    const F32 y0 = r.y + 2.f + 0.5f * lineMm;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    const LLColor4 dot(sW.dictDot[0], sW.dictDot[1], sW.dictDot[2], 1.f);
    gGL.color4fv(col(dot).mV);
    gl_circle_2d((F32)P.px(r.x + 4.f + dotMm), (F32)P.pyTop(y0), dotMm * P.k, 24, true);
    gGL.flush();
    for (size_t i = 0; i < lines.size(); ++i)
        P.text(lines[i], textX, y0 + (F32)i * lineMm, i < statusLines ? L.dim : L.text, LLFontGL::LEFT, textW);
}

void drawPanel(const Painter& P)
{
    const Layout& L = sW.L;
    // Header: unread count (or the panel's name) and the time.
    S32 unread = 0;
    for (const Item& it : sW.feed) unread += it.unread ? 1 : 0;
    const F32 hy = L.header.y + 0.5f * L.header.h;
    std::string head;
    if (unread)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", unread);
        head = LLTrans::getString("VRWristUnread", args);
    }
    // (#1590: grabbing switched off shows in place of the panel's name)
    const bool grabOff = !grabEnabled();
    if (head.empty() && grabOff) head = labelText("VRWristGrabOffShort");
    // (#1594 while space warp works the title gives way to "Warp 45 / 90": rendered / shown frames per second)
    const std::string warp = LLVRSpaceWarp::instance().shortText();
    if (head.empty() && !warp.empty()) head = warp;
    P.text(head.empty() ? std::string("Prestige") : head, L.header.x, hy, unread || grabOff || !warp.empty() ? L.text : L.dim, LLFontGL::LEFT,
           L.header.w * (L.settingsButton ? 0.4f : 0.7f));
    {
        const std::time_t t = std::time(nullptr);
        std::tm lt{};
#if LL_WINDOWS
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        P.text(llformat("%02d:%02d", lt.tm_hour, lt.tm_min), L.header.x + L.header.w, hy, L.dim, LLFontGL::RIGHT,
               L.header.w * (L.settingsButton ? 0.24f : 0.3f));
    }
    bool anyRow = false;
    for (S32 ti = 0; ti < (S32)sW.targets.size(); ++ti)
    {
        const Target& t = sW.targets[ti];
        const bool feedTarget = t.type == Target::ROW || t.type == Target::DIALOG_BUTTON || t.type == Target::DIALOG_WALL;
        if (feedTarget && t.index >= (S32)sW.feed.size()) continue;
        if (t.type == Target::ACTION && t.index >= (S32)L.items.size()) continue;
        if (t.type == Target::DIALOG_BUTTON && t.sub >= (S32)sW.feed[t.index].buttons.size()) continue;
        const bool hov = ti == sW.hover && sW.src != WristState::SRC_NONE;
        const bool flash = ti == sW.flashTarget && sW.pressFlash > 0.f;
        const bool pressing = hov && sW.src == WristState::SRC_TOUCH && sW.depthMm < L.pressMm + 6.f;
        const LLColor4 bg = flash || pressing ? L.press : hov ? L.hover : (t.type == Target::ROW ? L.row : L.button);
        const F32 cy = t.r.y + 0.5f * t.r.h;
        switch (t.type)
        {
        case Target::ACTION:
        {
            bool on = false;
            const std::string label = actionLabel(L.items[t.index], &on);
            P.fill(t.r, (on && !hov && !flash) ? L.active : bg);
            drawLabel(P, label, t.r, L.text);
            break;
        }
        case Target::ROW:
        {
            anyRow = true;
            const Item& it = sW.feed[t.index];
            const LLColor4& kc = it.kind == Item::IM ? L.im : it.kind == Item::NEARBY ? L.nearby : (it.dialog ? L.dialog : L.notice);
            P.fill(t.r, bg);
            P.fill(RectMm{t.r.x, t.r.y, 2.5f, t.r.h}, kc);
            const F32 x0 = t.r.x + 5.f, maxX = t.r.x + t.r.w - 3.f;
            std::string from = it.from;
            if (it.kind == Item::NEARBY && from.empty()) from = labelText("VRWristNearby");
            if (it.kind == Item::NOTICE && from.empty()) from = labelText("VRWristNotice");
            F32 x = P.text(from + ":", x0, cy, it.unread ? kc : L.dim, LLFontGL::LEFT, (maxX - x0) * 0.45f);
            std::string body = it.text;
            if (it.dialog && it.answered) body = "(" + labelText("VRWristAnswered") + ") " + body;
            P.text(body, x + 2.f, cy, it.unread ? L.text : L.dim, LLFontGL::LEFT, maxX - x - 2.f);
            break;
        }
        case Target::DIALOG_BUTTON:
        {
            const Item& it = sW.feed[t.index];
            P.fill(t.r, bg);
            drawLabel(P, it.buttons[t.sub].label, t.r, L.text);
            break;
        }
        case Target::DIALOG_WALL:
            P.fill(t.r, bg);
            drawLabel(P, labelText("VRWristOpenOnWall"), t.r, L.text);
            break;
        case Target::PAGE:
            P.fill(t.r, (sW.page && !hov && !flash) ? L.active : bg);
            drawLabel(P, labelText(sW.page ? "VRWristSettingsDone" : "VRWristSettings"), t.r, L.text);
            break;
        case Target::MENU:
            P.fill(t.r, bg);
            drawLabel(P, labelText("VRWristAllSettings"), t.r, L.text);
            break;
        case Target::SETTING:
            anyRow = true;
            if (t.sub == 0)
            {
                // The grab switch: green and "on" while grabbing works, plain and "off" otherwise.
                P.fill(t.r, (grabEnabled() && !hov && !flash) ? L.active : bg);
                drawLabel(P, settingLabel(t.index), t.r, L.text);
            }
            else
            {
                P.fill(t.r, bg);
                P.text(t.sub < 0 ? std::string("\xE2\x88\x92") : std::string("+"), t.r.x + 0.5f * t.r.w, cy, L.text,
                       LLFontGL::HCENTER, t.r.w - 2.f);
                if (t.sub > 0)
                {
                    // The value between this row's buttons (drawn once, with the right one).
                    const F32 x0 = L.feed.x + t.r.w + L.rowGapMm, x1 = t.r.x - L.rowGapMm;
                    P.fill(RectMm{x0, t.r.y, x1 - x0, t.r.h}, L.row);
                    drawLabel(P, settingLabel(t.index), RectMm{x0, t.r.y, x1 - x0, t.r.h}, L.text);
                }
            }
            break;
        }
    }
    if (sW.dictOn) drawDictation(P);
    else if (!anyRow)
        P.text(labelText("VRWristNothing"), L.feed.x + 0.5f * L.feed.w, L.feed.y + 0.5f * L.feed.h, L.dim, LLFontGL::HCENTER, L.feed.w);
    // The cursor: the laser's dot, or the fingertip (a ring that closes as it nears the panel).
    if (sW.src != WristState::SRC_NONE)
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        const F32 x = (F32)P.px(sW.curX), y = (F32)P.pyTop(sW.curY);
        F32 r = 2.2f;
        if (sW.src == WristState::SRC_TOUCH) r = llclamp(2.f + 0.12f * llmax(0.f, sW.depthMm), 2.f, 10.f);
        const LLColor4 dark(0.f, 0.f, 0.f, 1.f);
        gGL.color4fv(col(dark).mV);
        gl_circle_2d(x, y, (r + 0.8f) * P.k, 24, true);
        gGL.color4fv(col(L.cursor).mV);
        gl_circle_2d(x, y, r * P.k, 24, true);
        if (sW.src == WristState::SRC_TOUCH)
        {
            gGL.color4fv(col(dark).mV);
            gl_circle_2d(x, y, llmax(0.6f, r - 1.2f) * P.k, 24, true);
        }
    }
}

// The image's pixel density from the font's own line height (text drawn 1:1), clamped.
// #1591: the font follows the UI's display scale, which the VR UI scale raises to ~3: past
// 6 px/mm the panel is drawn at the font's density (pxPerMm) into an image of 6 px/mm
// (imagePxPerMm; render() maps the one onto the other), so the layout keeps its millimetres.
bool ensureTarget(const LLFontGL* font)
{
    const F32 linePx = (F32)font->getLineHeight() * LLFontGL::sScaleY;
    const F32 kDraw = drawDensity(font);
    const F32 k = llmin(kDraw, 6.f);
    const U32 w = (U32)llclamp((S32)std::lround(sW.L.sizeMm[0] * k), 16, 1536);
    const U32 h = (U32)llclamp((S32)std::lround(sW.panelH * k), 16, 1536);
    sW.pxPerMm = kDraw;
    if (sW.rt && sW.rtW == w && sW.rtH == h) return true;
    if (Clock::now() < sW.rtRetryAt) return false;
    if (sW.rt) LLRenderVKBackend::releaseTarget(sW.rt);
    sW.rt = LLRenderVKBackend::createTarget(w, h, false);
    if (!sW.rt)
    {
        // Allocation failure: no panel for now, try again in 5 s (never a crash).
        sW.rtRetryAt = Clock::now() + std::chrono::seconds(5);
        sW.rtW = sW.rtH = 0;
        LL_WARNS("VRWrist") << "render target " << w << "x" << h << " could not be made; retrying in 5 s" << LL_ENDL;
        return false;
    }
    sW.rtW = w;
    sW.rtH = h;
    LL_INFOS("VRWrist") << "render target " << w << "x" << h << " (" << (w * h * 4u + 1023u) / 1024u << " KB, " << k
                        << " px/mm; drawn at " << kDraw << " px/mm from a " << linePx << " px line)" << LL_ENDL;
    return true;
}

// Whether what the panel shows changed since the last draw (hover, cursor, toggles, the clock).
void checkVisualChanges()
{
    const std::time_t t = std::time(nullptr);
    const S32 minute = (S32)(t / 60);
    if (minute != sW.drawnMinute) sW.dirty = true;
    if (sW.wallUp != sW.drawnWall || LLVRCamera::flyCam() != sW.drawnFly) sW.dirty = true;
    if (LLVRBody::available() != sW.drawnBody || LLVRBody::calibrating() != sW.drawnBodyCal) sW.dirty = true;   // #1565
    if ((U32)LLVRBodySource::active() != sW.drawnBodySrc) sW.dirty = true;
    if (sW.page != sW.drawnPage) sW.dirty = true;   // #1590 / #1591
    if (LLVRSpaceWarp::instance().shortText() != sW.drawnWarp) sW.dirty = true;   // #1594
    for (S32 k = 0; k < SET_COUNT; ++k)
        if (settingValue(k) != sW.drawnSettings[k]) sW.dirty = true;
    // #1119: dictation's text (the listening seconds change it ~10x a second, the countdown once).
    if ((sW.dictOn ? sW.dictStatus + '\x1f' + sW.dictQuote : std::string()) != sW.drawnDict ||
        LLVRDictation::wristListening() != sW.drawnSay)
        sW.dirty = true;
    const S32 hov = sW.src == WristState::SRC_NONE ? -1 : sW.hover;
    const S32 flash = sW.pressFlash > 0.f ? sW.flashTarget : -1;
    if (hov != sW.drawnHover || flash != sW.drawnFlash || sW.src != sW.drawnSrc) sW.dirty = true;
    if (sW.src != WristState::SRC_NONE)
    {
        // The cursor moves the image only by a visible amount (~1 mm, 4 mm of touch depth).
        if (std::fabs(sW.curX - sW.drawnCurX) > 1.f || std::fabs(sW.curY - sW.drawnCurY) > 1.f) sW.dirty = true;
        if (sW.src == WristState::SRC_TOUCH && std::fabs(sW.depthMm - sW.drawnDepth) > 4.f) sW.dirty = true;
    }
}
} // namespace

// ---- LLVRWrist ---------------------------------------------------------------------------------

void LLVRWrist::update(const LLXrInputState& in, const LLXrHeadPose& hp, bool enabled, bool wallUp, F32 dt,
                       LLXrUiLayers& ui, Requests& req)
{
    ui.wrist = false;
    const bool wasOn = sW.rightOn;   // (last frame: the right hand was using the panel)
    sW.rightOn = false;
    sW.geom = false;
    sW.wallUp = wallUp;
    // #1119: dictation to nearby chat shows over the feed, and keeps the panel up meanwhile (it
    // is where the preview is read and its countdown runs).
    {
        F32 dot[4];
        const bool was = sW.dictOn;
        sW.dictOn = enabled && LLVRDictation::wristStatus(wallUp, sW.dictStatus, sW.dictQuote, dot);
        if (sW.dictOn)
            for (int i = 0; i < 4; ++i) sW.dictDot[i] = dot[i];
        if (sW.dictOn != was) markDirty();
    }
    LLXrSession& xr = LLXrSession::instance();
    if (enabled && !sW.loaded)
    {
        sW.L = loadLayout();
        sW.panelH = sW.L.sizeMm[1];
        sW.loaded = true;
    }
    if (enabled) connectFeeds();
    bool swapRB = false;
    const bool formatOk = enabled && xr.wristFormat(swapRB);
    if (enabled && !formatOk && !sW.formatLogged)
    {
        sW.formatLogged = true;
        LL_WARNS("VRWrist") << "no wrist panel: the runtime offers no swapchain format the panel's image copies into" << LL_ENDL;
    }
    if (formatOk && swapRB != sW.swapRB)
    {
        sW.swapRB = swapRB;
        markDirty();
    }
    const LLXrHandState& lh = in.hand[LLXR_HAND_LEFT];
    if (!formatOk || !lh.active || !lh.aim.valid || !hp.valid)
    {
        if (sW.shown) LL_INFOS("VRWrist") << "hidden (no left controller / world)" << LL_ENDL;
        sW.shown = false;
        sW.onTimer = sW.offTimer = 0.f;
        reset();
        return;
    }
    const Layout& L = sW.L;
    const float* q = lh.aim.orientation;
    const float* P = lh.aim.position;
    float off[3];
    rotQ(q, L.mountOffset, off);
    const float anchor[3] = {P[0] + off[0], P[1] + off[1], P[2] + off[2]};
    static const float kY[3] = {0.f, 1.f, 0.f};
    float baseN[3];
    rotQ(q, kY, baseN);
    float toHead[3] = {hp.position[0] - anchor[0], hp.position[1] - anchor[1], hp.position[2] - anchor[2]};
    const F32 dist = std::sqrt(dot3(toHead, toHead));
    const F32 facing = dist > 1e-4f ? dot3(baseN, toHead) / dist : -1.f;

    // ---- Shown while the mount faces the head (hysteresis + delays); held while it is being used.
    const bool busy = sW.touchDown || (wasOn && in.hand[LLXR_HAND_RIGHT].select);
    const bool want = enabled && ((dist < L.maxDistM && (sW.shown ? facing > L.offDot : facing > L.onDot)) || sW.dictOn);
    if (want || (sW.shown && busy))
    {
        sW.offTimer = 0.f;
        if (!sW.shown && (sW.dictOn || (sW.onTimer += dt) >= L.showDelay))
        {
            sW.shown = true;
            sW.shownFor = 0.f;
            markDirty();
            LL_INFOS("VRWrist") << "shown (facing " << facing << ", " << dist << " m from the head)" << LL_ENDL;
        }
    }
    else
    {
        sW.onTimer = 0.f;
        if (sW.shown && (sW.offTimer += dt) >= L.hideDelay)
        {
            sW.shown = false;
            LL_INFOS("VRWrist") << "hidden (facing " << facing << ", " << dist << " m from the head)" << LL_ENDL;
            reset();
        }
    }
    if (!sW.shown) return;
    if (sW.dictOn) LLVRDictation::noteShownOnWrist();
    sW.shownFor += dt;
    if (sW.shownFor > 1.5f)
    {
        // Seen: everything shown so far is read.
        for (Item& it : sW.feed)
        {
            if (!it.unread) continue;
            it.unread = false;
            markDirty();
        }
    }

    computeGrid();   // the action rows, and with them the panel's height (before the placement uses it)

    // ---- Placement: tilted about its own X toward the eyes (clamped), lifted off the controller.
    float hl[3];
    rotQInv(q, toHead, hl);
    const F32 a0 = std::atan2(-hl[1], hl[2]);   // the X rotation whose normal points at the head
    F32 tilt = a0 + 0.5f * F_PI;
    while (tilt > F_PI) tilt -= F_TWO_PI;
    while (tilt < -F_PI) tilt += F_TWO_PI;
    const F32 tmax = L.tiltMaxDeg * DEG_TO_RAD;
    tilt = llclamp(tilt, -tmax, tmax);
    const F32 a = -0.5f * F_PI + tilt;
    const float qLocal[4] = {std::sin(0.5f * a), 0.f, 0.f, std::cos(0.5f * a)};
    static const float kZ[3] = {0.f, 0.f, 1.f};
    float liftV[3];
    rotQ(qLocal, kZ, liftV);
    const float posLocal[3] = {L.mountOffset[0] + liftV[0] * L.liftM, L.mountOffset[1] + liftV[1] * L.liftM,
                               L.mountOffset[2] + liftV[2] * L.liftM};
    static LLCachedControl<F32> s_scale(gSavedSettings, "SlipstreamVRWristScale", 1.f);
    const F32 scale = llclamp((F32)s_scale, 0.5f, 2.f);
    sW.pw = L.sizeMm[0] * 0.001f * scale;
    sW.ph = sW.panelH * 0.001f * scale;
    ui.wrist = true;
    for (int i = 0; i < 3; ++i) ui.wristPos[i] = posLocal[i];
    for (int i = 0; i < 4; ++i) ui.wristOri[i] = qLocal[i];
    ui.wristSize[0] = sW.pw;
    ui.wristSize[1] = sW.ph;
    // The same quad in app space (the hit test).
    float pc[3], qa[4];
    rotQ(q, posLocal, pc);
    for (int i = 0; i < 3; ++i) sW.c[i] = P[i] + pc[i];
    mulQ(q, qLocal, qa);
    static const float kX[3] = {1.f, 0.f, 0.f};
    rotQ(qa, kX, sW.ax);
    rotQ(qa, kY, sW.ay);
    rotQ(qa, kZ, sW.an);
    sW.geom = true;

    panelTargets(sW.targets);

    // ---- The right hand: touch (its tip on the panel) first, else the laser.
    const LLXrHandState& rh = in.hand[LLXR_HAND_RIGHT];
    const WristState::Src prevSrc = sW.src;
    sW.src = WristState::SRC_NONE;
    if (in.focused && rh.active && rh.aim.valid)
    {
        static const float kFwd[3] = {0.f, 0.f, -1.f};
        float d[3];
        rotQ(rh.aim.orientation, kFwd, d);
        const float* o = rh.aim.position;
        const float tip[3] = {o[0] + d[0] * L.tipM, o[1] + d[1] * L.tipM, o[2] + d[2] * L.tipM};
        const float rel[3] = {tip[0] - sW.c[0], tip[1] - sW.c[1], tip[2] - sW.c[2]};
        const F32 s = dot3(rel, sW.an) * 1000.f;   // mm in front of the panel
        const F32 lx = dot3(rel, sW.ax), ly = dot3(rel, sW.ay);
        const F32 marginM = 0.01f;
        const bool inRect = std::fabs(lx) < 0.5f * sW.pw + marginM && std::fabs(ly) < 0.5f * sW.ph + marginM;
        if (inRect && s < L.hoverMm && s > -L.throughMm)
        {
            sW.src = WristState::SRC_TOUCH;
            sW.curX = llclamp((lx / sW.pw + 0.5f) * L.sizeMm[0], 0.f, L.sizeMm[0]);
            sW.curY = llclamp((0.5f - ly / sW.ph) * sW.panelH, 0.f, sW.panelH);
            sW.depthMm = s;
            sW.beam = 0.f;
        }
        else
        {
            // The laser, from the front.
            const F32 dn = dot3(d, sW.an);
            if (dn < -1e-4f)
            {
                const float oc[3] = {sW.c[0] - o[0], sW.c[1] - o[1], sW.c[2] - o[2]};
                const F32 t = dot3(oc, sW.an) / dn;
                if (t > 0.f && t < 3.f)
                {
                    const float hit[3] = {o[0] + d[0] * t - sW.c[0], o[1] + d[1] * t - sW.c[1], o[2] + d[2] * t - sW.c[2]};
                    const F32 hx = dot3(hit, sW.ax), hy = dot3(hit, sW.ay);
                    if (std::fabs(hx) <= 0.5f * sW.pw && std::fabs(hy) <= 0.5f * sW.ph)
                    {
                        sW.src = WristState::SRC_LASER;
                        sW.curX = (hx / sW.pw + 0.5f) * L.sizeMm[0];
                        sW.curY = (0.5f - hy / sW.ph) * sW.panelH;
                        sW.beam = t;
                    }
                }
            }
        }
        const S32 under = sW.src != WristState::SRC_NONE ? targetAt(sW.curX, sW.curY) : -1;
        sW.hover = under;
        if (sW.src == WristState::SRC_TOUCH)
        {
            // A press needs the tip to come from the front (a hand reaching under / behind the
            // panel never presses), then one press until it backs off past releaseMm.
            if (sW.depthMm > L.pressMm && !sW.touchDown) sW.touchArmed = true;
            if (!sW.touchDown && sW.touchArmed && sW.depthMm < L.pressMm)
            {
                sW.touchDown = true;
                sW.touchArmed = false;
                if (under >= 0) activate(under, true, req);
            }
            else if (sW.touchDown && sW.depthMm > L.releaseMm)
            {
                sW.touchDown = false;
                sW.touchArmed = true;
            }
        }
        else
        {
            sW.touchDown = false;
            sW.touchArmed = false;
        }
        // The trigger: a press wherever the cursor is (laser or touch). Off every target (the
        // panel's background) it brings up the wall, as the old panel did.
        const bool sel = rh.select;
        if (sel && !sW.prevSelect && sW.src != WristState::SRC_NONE)
        {
            if (under >= 0) activate(under, sW.src == WristState::SRC_TOUCH, req);
            else
            {
                hapticPress(false);
                req.wallShow = true;
                LL_INFOS("VRWrist") << "press on the panel background: the wall comes up" << LL_ENDL;
            }
        }
        sW.prevSelect = sel;
    }
    else
    {
        sW.touchDown = false;
        sW.prevSelect = false;
        sW.hover = -1;
    }
    if (sW.src != prevSrc && sW.src != WristState::SRC_NONE)
        LL_INFOS("VRWrist") << "right hand on the panel by " << (sW.src == WristState::SRC_TOUCH ? "touch" : "laser") << LL_ENDL;
    sW.rightOn = sW.src != WristState::SRC_NONE;
    if (sW.pressFlash > 0.f) sW.pressFlash = llmax(0.f, sW.pressFlash - dt);
    // The targets may have changed with what activate() did (an answered dialog).
    panelTargets(sW.targets);
    if (sW.hover >= (S32)sW.targets.size()) sW.hover = -1;
    checkVisualChanges();
}

bool LLVRWrist::rightOnPanel(F32& beam)
{
    beam = sW.beam;
    return sW.rightOn;
}

void LLVRWrist::reset()
{
    sW.src = WristState::SRC_NONE;
    sW.rightOn = false;
    sW.touchDown = false;
    sW.touchArmed = false;
    sW.prevSelect = false;
    sW.hover = -1;
    sW.geom = false;
}

void LLVRWrist::onNearbyChat(const LLChat& chat)
{
    if (!LLXrSession::active() || !sW.connected || !sW.L.feedNearby) return;
    if (chat.mMuted || chat.mChatType == CHAT_TYPE_START || chat.mChatType == CHAT_TYPE_STOP ||
        chat.mChatType == CHAT_TYPE_DEBUG_MSG)
        return;
    // People and objects only (no system / teleport / region lines), not our own.
    if (chat.mSourceType != CHAT_SOURCE_AGENT && chat.mSourceType != CHAT_SOURCE_OBJECT) return;
    if (chat.mFromID == gAgentID && chat.mSourceType == CHAT_SOURCE_AGENT) return;
    const std::string text = flatten(chat.mText, kMaxTextChars);
    if (text.empty()) return;
    Item it;
    it.kind = Item::NEARBY;
    it.from = flatten(chat.mFromName, kMaxFromChars);
    it.text = text;
    pushItem(std::move(it));
}

void LLVRWrist::render()
{
    if (!LLXrSession::active()) return;   // desktop: one static read
    if (!sW.shown || !sW.geom || !sW.dirty) return;
    if (!LLRender::hasBackend() || LLRenderVKBackend::inTarget()) return;
    const LLFontGL* font = wristFont();
    if (!font) return;
    computeGrid();   // (labels may have changed since update(): the panel's height with them)
    if (!ensureTarget(font)) return;
    const LLColor4 bg = col(sW.L.background);
    const F32 clear[4] = {bg.mV[0], bg.mV[1], bg.mV[2], 1.f};
    if (!LLRenderVKBackend::beginTarget(sW.rt, clear, false)) return;   // (stays dirty: next frame)
    const glm::mat4 savedProj = get_current_projection();
    const glm::mat4 savedView = get_current_modelview();
    const LLCoordGL savedOrigin = LLFontGL::sCurOrigin;
    LLFontGL::sCurOrigin.set(0, 0);
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadIdentity();
    // Top row first in the image (the quad's image is top-left origin; the facade puts GL's
    // bottom row in row 0 of a render target): an ortho with bottom = H, top = 0.
    // (#1591: in the font's pixels -- see ensureTarget -- which the image may hold fewer of)
    const F32 drawW = sW.L.sizeMm[0] * sW.pxPerMm, drawH = sW.panelH * sW.pxPerMm;
    gGL.ortho(0.f, drawW, drawH, 0.f, -1.f, 1.f);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadIdentity();
    gGL.pushUIMatrix();
    gGL.loadUIIdentity();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.color4f(1.f, 1.f, 1.f, 1.f);

    // (The feed may have changed since update(): a line arrived in between. Draw what is current;
    // the hit test rebuilds its targets every update.)
    panelTargets(sW.targets);
    if (sW.hover >= (S32)sW.targets.size()) sW.hover = -1;
    Painter P;
    P.font = font;
    P.k = sW.pxPerMm;
    P.H = (S32)std::lround(drawH);
    drawPanel(P);

    gGL.flush();
    gGL.popUIMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    LLFontGL::sCurOrigin = savedOrigin;
    LLRenderVKBackend::endTarget();
    set_current_projection(savedProj);
    set_current_modelview(savedView);

    LLXrSession::instance().setWristImage(LLRenderVKBackend::targetImage(sW.rt), sW.rtW, sW.rtH);
    sW.dirty = false;
    sW.drawnMinute = (S32)(std::time(nullptr) / 60);
    sW.drawnWall = sW.wallUp;
    sW.drawnFly = LLVRCamera::flyCam();
    sW.drawnBody = LLVRBody::available();
    sW.drawnBodyCal = LLVRBody::calibrating();
    sW.drawnBodySrc = (U32)LLVRBodySource::active();
    sW.drawnPage = sW.page;
    sW.drawnWarp = LLVRSpaceWarp::instance().shortText();
    for (S32 k = 0; k < SET_COUNT; ++k) sW.drawnSettings[k] = settingValue(k);
    sW.drawnDict = sW.dictOn ? sW.dictStatus + '\x1f' + sW.dictQuote : std::string();
    sW.drawnSay = LLVRDictation::wristListening();
    sW.drawnHover = sW.src == WristState::SRC_NONE ? -1 : sW.hover;
    sW.drawnFlash = sW.pressFlash > 0.f ? sW.flashTarget : -1;
    sW.drawnSrc = sW.src;
    sW.drawnCurX = sW.curX;
    sW.drawnCurY = sW.curY;
    sW.drawnDepth = sW.depthMm;
    if (++sW.redraws <= 3 || (sW.redraws % 500) == 0)
        LL_INFOS("VRWrist") << "redraw " << sW.redraws << " (" << sW.rtW << "x" << sW.rtH << ", " << sW.feed.size()
                            << " feed items, " << sW.targets.size() << " targets)" << LL_ENDL;
}
