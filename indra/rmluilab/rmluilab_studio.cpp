// #1668 slrmluilab --studio: Video Studio's overlay (newview/slvideostudiodoc.h, the viewer's exact RML)
// over sample data, one scene per tab (Track / Ride / Record / Replay, each with the transport bar) and
// one recording, for review renders without the viewer. The rows mirror SLVideoStudio::rows()
// (slvideostudio.cpp build()) for a typical track; the data model is the viewer's ("vstudio").
#include "rmluilab_newui.h"

#include "llnewuikitstyle.h"
#include "slvideostudiodoc.h"

#include <RmlUi/Core.h>

#include <string>
#include <vector>

namespace
{
// Row kinds as SLVideoStudio::RowKind.
enum
{
    SLIDER = 0,
    TOGGLE = 1,
    CHOICE = 2,
    BUTTONS = 3,
    NOTE = 4,
    HEAD = 5
};
struct VRow
{
    std::string id, track, label, value, w;
    int kind = 0, index = 0;
    bool on = false, enabled = true;
    std::vector<std::string> buttons;
};
struct VTab
{
    std::string label;
    bool on = false;
};
struct Model
{
    std::vector<VRow> rows;
    std::vector<VTab> tabs;
    std::string title = "Video Studio", leaveLabel = "Leave", hint, timeText = "2.4 / 8.0 s", playLabel = "Play",
                recLabel = "Record", scrubW = "30.0%", recStatus;
    bool hidden = false, playing = false, recording = false;
    Rml::DataModelHandle handle;
};
Model gStudio;

void noop(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {}

struct RowSpec
{
    int kind;
    std::string label, value;
    float fraction = 0.f;
    bool on = false, enabled = true;
    std::vector<std::string> buttons = {};
};

void setRows(const std::vector<RowSpec>& spec)
{
    gStudio.rows.clear();
    for (size_t i = 0; i < spec.size(); ++i)
    {
        VRow r;
        r.index = (int)i;
        r.id = "vrow-" + std::to_string(i);
        r.track = "vtrack-" + std::to_string(i);
        r.kind = spec[i].kind;
        r.label = spec[i].label;
        r.value = spec[i].value;
        r.on = spec[i].on;
        r.enabled = spec[i].enabled;
        r.buttons = spec[i].buttons;
        r.w = std::to_string(spec[i].fraction * 100.f) + "%";
        gStudio.rows.push_back(r);
    }
}

void setTab(int tab)
{
    static const char* names[] = { "Track", "Ride", "Record", "Replay" };
    gStudio.tabs.clear();
    for (int i = 0; i < 4; ++i)
        gStudio.tabs.push_back({ names[i], i == tab });
}

const std::string kDeg = "\xC2\xB0";

void trackTab()
{
    setTab(0);
    setRows({
        { BUTTONS, "", "", 0.f, false, true, { "New track", "Bring here" } },
        { CHOICE, "Handles", "Move" },
        { TOGGLE, "Snap to grid", "On", 0.f, true },
        { TOGGLE, "Show the track", "On", 0.f, true },
        { SLIDER, "Length", "4.00 m", 0.28f },
        { CHOICE, "Curve", "Left" },
        { SLIDER, "Radius", "6.00 m", 0.53f },
        { SLIDER, "Arc", "38.2" + kDeg, 0.11f },
        { SLIDER, "Height", "1.75 m", 0.06f },
        { SLIDER, "Tilt", "0.0" + kDeg, 0.5f },
        { SLIDER, "Heading", "135" + kDeg, 0.375f },
        { HEAD, "Aim" },
        { CHOICE, "Camera", "Lock on a target" },
        { NOTE, "Following Sky Line" },
        { BUTTONS, "", "", 0.f, false, true, { "Focus point", "Pick in world", "Unfollow" } },
        { HEAD, "Saved tracks" },
        { BUTTONS, "", "", 0.f, false, true, { "Save Track 2", "Save as new" } },
        { BUTTONS, "Track 1", "", 0.f, false, true, { "Load", "Delete" } },
        { BUTTONS, "Track 2", "", 0.f, true, true, { "Load", "Delete?" } },
        { BUTTONS, "Beach dolly, long name for the ellipsis", "", 0.f, false, true, { "Load", "Delete" } },
    });
    gStudio.hint = "Drag the handles in the world; click the rail or the target to edit it. Space rides, right-drag orbits.";
}

void rideTab()
{
    setTab(1);
    setRows({
        { CHOICE, "Timing", "By duration" },
        { SLIDER, "Duration", "8.0 s", 0.6f },
        { SLIDER, "Ease in", "20%", 0.4f },
        { SLIDER, "Ease out", "20%", 0.4f },
        { CHOICE, "Repeat", "Ping-pong" },
        { TOGGLE, "Reverse", "Off" },
        { TOGGLE, "View from the cart", "On", 0.f, true },
        { TOGGLE, "Hide this panel while riding", "On", 0.f, true },
        { TOGGLE, "Show the track while riding", "Off" },
        { NOTE, "Space plays and stops; Esc stops a ride and brings the panel back." },
    });
    gStudio.hint = "Drag the handles in the world; click the rail or the target to edit it. Space rides, right-drag orbits.";
}

void recordTab()
{
    setTab(2);
    setRows({
        { NOTE, "MP4 video, encoded on the GPU (H.264)." },
        { CHOICE, "Size", "1080p" },
        { CHOICE, "Frame rate", "30 fps" },
        { TOGGLE, "Fixed timestep", "On", 0.f, true },
        { TOGGLE, "Record sound", "Off", 0.f, false, false },
        { NOTE, "Fixed timestep: the ride moves exactly one frame per frame of video, so the video is smooth even when "
                "the viewer renders slower than real time. No sound." },
        { BUTTONS, "", "", 0.f, false, true, { "Record the ride", "Open folder" } },
        { NOTE, "Last: /home/user/Videos/Slipstream/Slipstream Video 2026-11-21 18-40-02.mp4" },
        { NOTE, "Up to 300 s per recording. The world behind the camera (avatars, water, sky) moves in real time; "
                "Freeze World (Ctrl+Shift+F) holds it." },
    });
    gStudio.hint = "Drag the handles in the world; click the rail or the target to edit it. Space rides, right-drag orbits.";
}

void replayTab()
{
    setTab(3);
    setRows({
        { TOGGLE, "Instant replay", "On", 0.f, true },
        { NOTE, "While on, the last 30 seconds of the window (the interface and the viewer's sound included) are kept in a "
                "fixed amount of memory; Save Replay writes them to an MP4 in Videos/Slipstream." },
        { BUTTONS, "", "", 0.f, false, true, { "Save replay (Alt+F10)", "Open folder" } },
    });
    gStudio.hint = "Drag the handles in the world; click the rail or the target to edit it. Space rides, right-drag orbits.";
}

void bindModel(Rml::Context* context)
{
    Rml::DataModelConstructor m = context->CreateDataModel("vstudio");
    if (!m)
        return;
    m.RegisterArray<std::vector<std::string>>();
    if (auto r = m.RegisterStruct<VRow>())
    {
        r.RegisterMember("id", &VRow::id);
        r.RegisterMember("track", &VRow::track);
        r.RegisterMember("label", &VRow::label);
        r.RegisterMember("value", &VRow::value);
        r.RegisterMember("w", &VRow::w);
        r.RegisterMember("kind", &VRow::kind);
        r.RegisterMember("index", &VRow::index);
        r.RegisterMember("on", &VRow::on);
        r.RegisterMember("enabled", &VRow::enabled);
        r.RegisterMember("buttons", &VRow::buttons);
    }
    m.RegisterArray<std::vector<VRow>>();
    if (auto t = m.RegisterStruct<VTab>())
    {
        t.RegisterMember("label", &VTab::label);
        t.RegisterMember("on", &VTab::on);
    }
    m.RegisterArray<std::vector<VTab>>();
    Model& s = gStudio;
    m.Bind("rows", &s.rows);
    m.Bind("tabs", &s.tabs);
    m.Bind("title", &s.title);
    m.Bind("leaveLabel", &s.leaveLabel);
    m.Bind("hint", &s.hint);
    m.Bind("timeText", &s.timeText);
    m.Bind("playLabel", &s.playLabel);
    m.Bind("recLabel", &s.recLabel);
    m.Bind("scrubW", &s.scrubW);
    m.Bind("recStatus", &s.recStatus);
    m.Bind("hidden", &s.hidden);
    m.Bind("playing", &s.playing);
    m.Bind("recording", &s.recording);
    for (const char* cb : { "vtab", "vturn", "vpress", "play", "rec", "leave" })
        m.BindEventCallback(cb, &noop);
    s.handle = m.GetModelHandle();
}

LabScene scene(const std::string& name, bool light, void (*fill)(), bool recording)
{
    const LLNewUIKit::Tokens tokens = LLNewUIKit::tokens(light ? LLNewUIKit::Theme::Light : LLNewUIKit::Theme::Dark, true);
    LabScene sc{ name, light ? "dark" : "light", { LLNewUIKit::themed(kStudioRml, tokens) }, {} };
    sc.prepare = [fill, recording](Rml::Context*, const std::vector<Rml::ElementDocument*>&)
    {
        fill();
        gStudio.recording = recording;
        gStudio.playing = recording;
        gStudio.recStatus = recording ? "REC 12.4 s (fixed step)" : "";
        gStudio.recLabel = recording ? "Stop recording" : "Record";
        gStudio.playLabel = recording ? "Stop" : "Play";
        if (gStudio.handle)
            gStudio.handle.DirtyAllVariables();
    };
    return sc;
}
} // namespace

std::vector<LabScene> labVideoStudioScenes(Rml::Context* context)
{
    bindModel(context);
    return { scene("video_studio_track", false, &trackTab, false), scene("video_studio_ride", false, &rideTab, false),
             scene("video_studio_record", false, &recordTab, false), scene("video_studio_replay", false, &replayTab, false),
             scene("video_studio_recording", false, &recordTab, true), scene("video_studio_track_light", true, &trackTab, false) };
}
