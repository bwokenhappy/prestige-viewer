// #703 The new interface's status bar: the classic status bar's parts in a small floating bar at
// the top right, in the kit's style (llnewuistatusdocs.h), and its menus (llnewuimenus.h).
//
// Parts, each one shown or not (a setting each, Persist; the "more" menu's "Status bar" page and
// the command surface toggle them):
//   parcel     the rating (G / M / A) and the restrictions here (no voice, fly, push, build,
//              scripts, avatars hidden; damage and health); click: About Land   UINewStatusParcel
//   frame rate rendered frames per second, green / amber / red (45 / 25), in accent while frame
//              generation or VR space warp adds frames; click: frame timings, Shift+click:
//              statistics                                                        ShowStatusBarFPS
//   network    ping to the region (green < 120 ms, amber < 250, red) and packet loss (green
//              < 0.5 %, amber < 2 %, red); click: statistics                     UINewStatusNet
//   bandwidth  data received, kbps; click: statistics                           ShowNetStats
//   time       Second Life time (12 / 24 h, seconds: the classic settings); the date in its tip
//                                                                                UINewStatusTime
//   L$         the balance (hidden as "****" with ObscureBalanceInStatusBar); click: its menu
//              (buy, refresh, hide, shop, transaction log)                       UINewStatusBalance
//   media      play / pause all media (off without media); resting on it: nearby media
//                                                                                UINewStatusMedia
//   volume     mute all (click); resting on it: the volume sliders               UINewStatusVolume
//   presets    graphics and camera presets (their pull-downs)                    UINewStatusPresets
//   gestures   gesture sets (its pull-down)                                      GestureSetsShowStatusBarIcon
//   more       the "more" menu (always shown)
// The whole bar: UINewStatusBar. The pull-downs are the classic status bar's own panels
// (LLStatusBar::showPulldown). Values refresh 4x a second and only changes reach the document.
// Hidden before login, in mouselook and in camera mode (with the rest of the interface).
#pragma once

#include <functional>

namespace Rml
{
class Context;
class ElementDocument;
}

class LLNewUIStatus
{
public:
    using DocumentLoader = std::function<Rml::ElementDocument*(const char* rml, const char* name)>;

    static LLNewUIStatus& instance();
    // The bar and its menus; false when they could not start (the rest of the interface goes on).
    bool start(Rml::Context* context, const DocumentLoader& load);
    void stop();
    bool active() const;
    // The documents again under new tokens (a theme switch).
    bool reload(const DocumentLoader& load);
    // Its documents (the bar, its menus).
    bool ownsDocument(const Rml::ElementDocument* doc) const;
    // The bar's bottom edge plus a gap (dp, UI units) while it shows, else 0: the native windows
    // keep below it (llnewuiwindow.cpp), as they keep below the classic menu bar.
    int bottomDp() const;
    // `barRight` / `barBottom`: the main bar's right edge and bottom (dp) when it is at the top edge
    // (0 when at the bottom): the status bar goes under it when the two would meet. True when a
    // document changed.
    bool sync(int barRight, int barBottom, float density);
    // The classic top row is hidden (the main bar takes its place at the window's top edge).
    static bool classicRowHidden();
    // The classic chrome back without RmlUi calls (the RmlUi layer failed for the session).
    static void restoreClassic();
};
