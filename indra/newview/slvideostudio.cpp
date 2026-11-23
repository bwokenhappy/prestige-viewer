/**
 * @file slvideostudio.cpp
 * @brief Video Studio (#1668): the mode, the track, its gizmos, the ride, saved tracks. See slvideostudio.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slvideostudio.h"

#include "slcameramode.h"
#include "slkeycommands.h"
#include "slvideorecorder.h"
#include "slvideostudioview.h"
#include "slvideotrack.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"          // gDisconnected
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llfocusmgr.h"
#include "llinstantreplay.h"
#include "llkeyboard.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llrmluihost.h"
#include "llsdjson.h"
#include "llselectmgr.h"          // (lltoolpie.h holds an LLObjectSelection handle)
#include "lltoolpie.h"
#include "llui.h"
#include "lluictrl.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"       // isAgentAvatarValid
#include "llworld.h"
#include "pipeline.h"
#include "vkgfx/llvulkancontext.h"

#include <boost/json.hpp>
#include <algorithm>
#include <cmath>

namespace sv = slvtrack;

bool SLVideoStudio::sActive = false;

namespace
{
constexpr F32 kSlop = (F32)DRAG_N_DROP_DISTANCE_THRESHOLD;
constexpr const char* kTrackDir = "video_studio_tracks";   // beside camera_mode_presets
constexpr size_t kMaxTracks = 100;                          // saved tracks (bounded)
constexpr std::streamsize kMaxTrackFile = 64 * 1024;        // a track file larger than this is not read
constexpr double kRotateSnap = 5.625 * sv::kPi / 180.0;     // LL's rotation snap (llmaniprotate.cpp)
constexpr F32 kPickPx = 9.f;                                // gizmo pick tolerance, pixels

F64 now() { return LLTimer::getTotalSeconds(); }

void notify(const std::string& msg)
{
    LLSD args;
    args["MESSAGE"] = msg;
    LLNotificationsUtil::add("SystemMessageTip", args);
}

enum class Gizmo : int
{
    Move = 0,
    Rotate = 1,
    Shape = 2
};
enum class Sel
{
    None,
    Track,
    Target
};
enum class Part
{
    None,
    MoveX,
    MoveY,
    MoveZ,
    MovePlane,
    Yaw,
    Tilt,
    Length,
    Bend,
    Rail,          // the rail itself or its end handles: selects the track
    TargetHandle   // selects the target
};

// What a row of the panel does (rows() builds them; turn / press look the row up again).
enum class Act
{
    None,
    TrackButtons, Gizmo, Snap, ShowTrack, Length, Curve, Radius, Arc, Height, Tilt, Heading,
    AimMode, Pan, AimTilt, TargetButtons, BlendButtons,
    SaveButtons, Saved,
    Timing, Duration, Speed, EaseIn, EaseOut, Repeat, Reverse, ViewCart, HideUI, ShowOnRide,
    Resolution, FrameRate, FixedStep, Sound, RecordButtons,
    Replay, ReplayButtons
};

struct State
{
    // What entry found, put back on exit.
    bool uiWasOn = true;
    bool hudWasOn = true;
    LLHandle<LLView> focus;
    SLVideoStudio::Panel panel = SLVideoStudio::PANEL_TRACK;

    // The track (origin in GLOBAL metres: region-independent; the gizmos work in agent space).
    bool hasTrack = false;
    sv::Track track;
    sv::Ride ride;
    sv::Aim aim;                // aim.target in global metres
    LLUUID targetObject;        // a picked avatar / object (resolved each frame), or null: the point
    std::string targetName;
    bool timingBySpeed = false;
    std::string savedName;      // the file it was loaded from / saved to ("" = not saved)
    std::string deleteArmed;    // a saved track's Delete, pressed once (the second press deletes)

    // Editing
    Gizmo gizmo = Gizmo::Move;
    Sel sel = Sel::Track;
    Part hover = Part::None;
    bool showTrack = true;
    bool pickArmed = false;     // the next click in the world picks the target

    // A gizmo drag
    Part drag = Part::None;
    sv::Track track0;
    sv::V3 target0;
    double param0 = 0.0;        // the axis parameter / angle at the press
    sv::V3 hit0;
    sv::V3 axis, anchor;        // the dragged axis and its anchor (agent space)

    // The ride
    bool playing = false;
    F64 playStart = 0.0;
    double playOffset = 0.0;    // seconds into the ride when play began
    F32 scrubU = 0.f;           // 0..1 of one pass (time)
    bool scrubbing = false;
    bool viewFromCart = false;
    bool showOnRide = false;

    // The pointer
    bool leftDown = false, leftPassed = false, rightDown = false, rightPassed = false, orbiting = false;
    S32 leftX = 0, leftY = 0, rightX = 0, rightY = 0, lastMX = 0, lastMY = 0;
    F32 wheelFraction = 0.f;
    bool hadFocus = true;
};
State sState;

// ---- coordinates ----
sv::V3 toV(const LLVector3& v) { return { v.mV[VX], v.mV[VY], v.mV[VZ] }; }
sv::V3 toV(const LLVector3d& v) { return { v.mdV[VX], v.mdV[VY], v.mdV[VZ] }; }
LLVector3 toAgentF(const sv::V3& v) { return LLVector3((F32)v.x, (F32)v.y, (F32)v.z); }
LLVector3d toD(const sv::V3& v) { return LLVector3d(v.x, v.y, v.z); }
// global <-> agent (the agent's region origin moves on a crossing; the track stays put)
sv::V3 agentFromGlobal(const sv::V3& g) { return toV(gAgent.getPosAgentFromGlobal(toD(g))); }
sv::V3 globalFromAgent(const sv::V3& a) { return toV(gAgent.getPosGlobalFromAgent(toAgentF(a))); }
// The track with its origin in agent space (for drawing, picking, the camera).
sv::Track agentTrack()
{
    sv::Track t = sState.track;
    t.origin = agentFromGlobal(t.origin);
    return t;
}

// The lock-on target this frame (global): a picked avatar or object where it is now, else the point.
sv::V3 targetGlobal()
{
    if (sState.targetObject.notNull())
    {
        if (LLViewerObject* o = gObjectList.findObject(sState.targetObject))
        {
            LLVector3d p = o->getPositionGlobal();
            if (o->isAvatar()) p.mdV[VZ] += 0.35;   // the chest, not the pelvis
            sState.aim.target = toV(p);
        }
    }
    return sState.aim.target;
}

// The cart's place along the rail (0..1) and the ride's time now.
double rideSeconds()
{
    if (SLVideoRecorder::recording()) return SLVideoRecorder::rideTime();
    if (sState.playing) return sState.playOffset + (now() - sState.playStart);
    return sState.scrubU * sState.ride.duration;
}
double cartPosition(bool* finished = nullptr)
{
    if (SLVideoRecorder::recording() || sState.playing) return sv::position(sState.ride, rideSeconds(), finished);
    // scrubbing: one pass, forward
    sv::Ride once = sState.ride;
    once.repeat = sv::Repeat::Once;
    if (finished) *finished = false;
    return sv::position(once, sState.scrubU * once.duration);
}

bool snapOn()
{
    static LLCachedControl<bool> s_snap(gSavedSettings, "SnapEnabled", true);
    return s_snap;
}
double gridStep()
{
    static LLCachedControl<F32> s_grid(gSavedSettings, "GridResolution", 0.5f);
    return snapOn() ? llmax((F32)s_grid, 0.01f) : 0.0;
}

// ---- the track ----
void rezTrack()
{
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const sv::V3 at = toV(cam->getAtAxis());
    sv::V3 flat = sv::normalize(sv::V3(at.x, at.y, 0.0));
    const sv::V3 left(-flat.y, flat.x, 0.0);
    const sv::V3 camG = toV(gAgentCamera.getCameraPositionGlobal());
    sv::Track t;
    t.length = 4.0;
    // Two metres ahead, running left to right across the view, at the camera's height.
    t.origin = camG + flat * 2.0 + left * (t.length * 0.5);
    t.yaw = std::atan2(-left.y, -left.x);
    sanitize(t);
    sState.track = t;
    sState.hasTrack = true;
    // Locked on what the camera looks at (its focus point), so the first ride is a useful shot.
    sState.aim = sv::Aim();
    sState.aim.mode = sv::AimMode::Target;
    sState.aim.target = toV(gAgentCamera.getFocusGlobal());
    if (sv::length(sState.aim.target - camG) > 64.0 || sv::length(sState.aim.target) < 1e-3)
        sState.aim.target = camG + toV(cam->getAtAxis()) * 6.0;
    sState.targetObject.setNull();
    sState.targetName.clear();
    sState.savedName.clear();
    sState.scrubU = 0.f;
    sState.sel = Sel::Track;
    LL_INFOS("VideoStudio") << "track rezzed (client-local)" << LL_ENDL;
}

// Moves the track (and a point target) so the rail's middle sits two metres ahead of the camera.
void bringHere()
{
    if (!sState.hasTrack) return;
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const sv::V3 at = toV(cam->getAtAxis());
    const sv::V3 flat = sv::normalize(sv::V3(at.x, at.y, 0.0));
    const sv::V3 want = toV(gAgentCamera.getCameraPositionGlobal()) + flat * 2.0;
    const sv::V3 mid = globalFromAgent(sv::at(agentTrack(), 0.5).pos);
    const sv::V3 delta = want - mid;
    sState.track.origin = sState.track.origin + delta;
    if (sState.targetObject.isNull()) sState.aim.target = sState.aim.target + delta;
}

// ---- saved tracks (JSON in the user settings folder; bounded count and size) ----
std::string tracksDir()
{
    const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, kTrackDir);
    if (!LLFile::isdir(dir)) LLFile::mkdir(dir);
    return dir;
}
std::string trackPath(const std::string& name)
{
    return tracksDir() + gDirUtilp->getDirDelimiter() + LLDir::getScrubbedFileName(name) + ".json";
}
std::vector<std::string> savedTracks()
{
    std::vector<std::string> names;
    LLDirIterator it(tracksDir(), "*.json");
    std::string file;
    while (it.next(file) && names.size() < kMaxTracks)
        names.push_back(file.substr(0, file.size() - 5));
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b)
              { return LLStringUtil::compareInsensitive(a, b) < 0; });
    return names;
}
LLSD trackToLLSD(const std::string& name)
{
    LLSD sd = LLSD::emptyMap();
    sd["version"] = 1;
    sd["name"] = name;
    sd["origin_global"] = toD(sState.track.origin).getValue();
    sd["yaw"] = sState.track.yaw;
    sd["tilt"] = sState.track.tilt;
    sd["length"] = sState.track.length;
    sd["radius"] = sState.track.radius;
    LLSD ride = LLSD::emptyMap();
    ride["duration"] = sState.ride.duration;
    ride["ease_in"] = sState.ride.easeIn;
    ride["ease_out"] = sState.ride.easeOut;
    ride["repeat"] = (S32)sState.ride.repeat;
    ride["reverse"] = sState.ride.reverse;
    ride["by_speed"] = sState.timingBySpeed;
    sd["ride"] = ride;
    LLSD aim = LLSD::emptyMap();
    aim["mode"] = (S32)sState.aim.mode;
    aim["pan"] = sState.aim.pan;
    aim["tilt"] = sState.aim.tiltOffset;
    aim["target_global"] = toD(sState.aim.target).getValue();
    aim["target_object"] = sState.targetObject;
    aim["target_name"] = sState.targetName;
    aim["start_yaw"] = sState.aim.startYaw;
    aim["start_pitch"] = sState.aim.startPitch;
    aim["end_yaw"] = sState.aim.endYaw;
    aim["end_pitch"] = sState.aim.endPitch;
    sd["aim"] = aim;
    if (LLViewerRegion* region = gAgent.getRegion()) sd["region"] = region->getName();
    // The camera preset in use, if any, so a shot can be re-shot with the same look.
    sd["camera_preset"] = gSavedSettings.getString("PresetCameraActive");
    return sd;
}
bool saveTrack(const std::string& name)
{
    if (!sState.hasTrack || name.empty()) return false;
    try
    {
        const std::string text = boost::json::serialize(LlsdToJson(trackToLLSD(name)));
        llofstream out(trackPath(name).c_str());
        if (!out.is_open()) return false;
        out << text;
        out.close();
        sState.savedName = name;
        LL_INFOS("VideoStudio") << "track saved: " << name << LL_ENDL;
        return !out.fail();
    }
    catch (const std::exception& e)
    {
        LL_WARNS("VideoStudio") << "track not saved: " << e.what() << LL_ENDL;
        return false;
    }
}
bool loadTrack(const std::string& name)
{
    llifstream in(trackPath(name).c_str());
    if (!in.is_open()) return false;
    std::string text(kMaxTrackFile + 1, '\0');
    in.read(&text[0], kMaxTrackFile + 1);
    const std::streamsize n = in.gcount();
    if (n <= 0 || n > kMaxTrackFile)
    {
        LL_WARNS("VideoStudio") << "track " << name << " not read (empty or over " << kMaxTrackFile << " bytes)" << LL_ENDL;
        return false;
    }
    text.resize((size_t)n);
    boost::system::error_code ec;
    const boost::json::value j = boost::json::parse(text, ec);
    if (ec) return false;
    const LLSD sd = LlsdFromJson(j);
    if (!sd.isMap() || !sd.has("origin_global")) return false;
    sv::Track t;
    t.origin = toV(LLVector3d(sd["origin_global"]));
    t.yaw = sd["yaw"].asReal();
    t.tilt = sd["tilt"].asReal();
    t.length = sd["length"].asReal();
    t.radius = sd["radius"].asReal();
    sanitize(t);
    sv::Ride r;
    const LLSD& ride = sd["ride"];
    r.duration = ride.has("duration") ? ride["duration"].asReal() : 8.0;
    r.easeIn = ride["ease_in"].asReal();
    r.easeOut = ride["ease_out"].asReal();
    r.repeat = (sv::Repeat)ride["repeat"].asInteger();
    r.reverse = ride["reverse"].asBoolean();
    sanitize(r);
    sv::Aim a;
    const LLSD& aim = sd["aim"];
    const S32 mode = aim["mode"].asInteger();
    a.mode = mode == 0 ? sv::AimMode::Along : mode == 2 ? sv::AimMode::Blend : sv::AimMode::Target;
    a.pan = aim["pan"].asReal();
    a.tiltOffset = aim["tilt"].asReal();
    a.target = toV(LLVector3d(aim["target_global"]));
    a.startYaw = aim["start_yaw"].asReal();
    a.startPitch = aim["start_pitch"].asReal();
    a.endYaw = aim["end_yaw"].asReal();
    a.endPitch = aim["end_pitch"].asReal();
    for (double* v : { &a.pan, &a.tiltOffset, &a.startYaw, &a.startPitch, &a.endYaw, &a.endPitch })
        if (!std::isfinite(*v)) *v = 0.0;
    sState.track = t;
    sState.ride = r;
    sState.aim = a;
    sState.timingBySpeed = ride["by_speed"].asBoolean();
    sState.targetObject = aim["target_object"].asUUID();
    sState.targetName = aim["target_name"].asString();
    sState.hasTrack = true;
    sState.savedName = name;
    sState.scrubU = 0.f;
    sState.playing = false;
    // Saved somewhere else (another region): brought in front of the camera, its shape kept.
    if (sv::length(t.origin - toV(gAgentCamera.getCameraPositionGlobal())) > 256.0)
    {
        bringHere();
        notify("Video Studio: \"" + name + "\" was saved far from here; it is placed in front of the camera.");
    }
    LL_INFOS("VideoStudio") << "track loaded: " << name << LL_ENDL;
    return true;
}
std::string newTrackName()
{
    const std::vector<std::string> names = savedTracks();
    for (int i = 1; i < 10000; ++i)
    {
        const std::string n = llformat("Track %d", i);
        if (std::find(names.begin(), names.end(), n) == names.end()) return n;
    }
    return "Track";
}

// ---- the gizmos ----
// The world size of one pixel at `dist` metres.
F32 pxToM(F32 dist)
{
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const F32 h = (F32)llmax(1, gViewerWindow->getWorldViewHeightScaled());
    return 2.f * tanf(cam->getView() * 0.5f) / h * llmax(dist, 0.01f);
}
// The gizmo's size at a point: about 110 pixels.
F32 gizmoSize(const sv::V3& p)
{
    const F32 dist = (F32)sv::length(p - toV(LLViewerCamera::getInstance()->getOrigin()));
    return pxToM(dist) * 110.f;
}
// Where the gizmo sits (agent space) and the item it edits.
bool gizmoAnchor(sv::V3& anchor)
{
    if (!sState.hasTrack) return false;
    if (sState.sel == Sel::Target && sState.aim.mode == sv::AimMode::Target && sState.targetObject.isNull())
    {
        anchor = agentFromGlobal(sState.aim.target);
        return true;
    }
    if (sState.sel != Sel::Track) return false;
    anchor = agentFromGlobal(sState.track.origin);
    return true;
}
const sv::V3 kAxes[3] = { sv::V3(1, 0, 0), sv::V3(0, 1, 0), sv::V3(0, 0, 1) };

struct Ray
{
    sv::V3 o, d;
};
Ray mouseRay(S32 x, S32 y)
{
    Ray r;
    r.o = toV(LLViewerCamera::getInstance()->getOrigin());
    r.d = sv::normalize(toV(gViewerWindow->mouseDirectionGlobal(x, y)));
    return r;
}

// The ring a rotate gizmo part draws: its centre, its plane's axes and radius.
struct Ring
{
    sv::V3 c, e1, e2;
    double r = 1.0;
};
Ring yawRing(const sv::Track& t, F32 size)
{
    return { t.origin, sv::V3(1, 0, 0), sv::V3(0, 1, 0), size * 0.9 };
}
Ring tiltRing(const sv::Track& t, F32 size)
{
    const sv::V3 flat(std::cos(t.yaw), std::sin(t.yaw), 0.0);
    return { t.origin, flat, sv::V3(0, 0, 1), size * 0.65 };
}
double ringDistance(const Ray& ray, const Ring& ring, double* along)
{
    double best = 1e30, bestAlong = 0.0;
    for (int i = 0; i < 64; ++i)
    {
        const double a = i * 2.0 * sv::kPi / 64.0;
        double s = 0.0;
        const double d = sv::rayPointDistance(ray.o, ray.d, ring.c + (ring.e1 * std::cos(a) + ring.e2 * std::sin(a)) * ring.r, &s);
        if (d < best)
        {
            best = d;
            bestAlong = s;
        }
    }
    if (along) *along = bestAlong;
    return best;
}

// The gizmo part under the pointer (nearest first), or the rail / target to select.
Part pickPart(S32 x, S32 y)
{
    if (!sState.hasTrack) return Part::None;
    const Ray ray = mouseRay(x, y);
    const sv::Track t = agentTrack();
    Part best = Part::None;
    double bestAlong = 1e30;
    auto consider = [&](Part p, double dist, double along, double extra = 0.0)
    {
        if (along <= 0.0) return;
        if (dist <= (double)pxToM((F32)along) * kPickPx + extra && along < bestAlong)
        {
            best = p;
            bestAlong = along;
        }
    };
    sv::V3 anchor;
    if (gizmoAnchor(anchor))
    {
        const F32 size = gizmoSize(anchor);
        const bool target = sState.sel == Sel::Target;
        if (sState.gizmo == Gizmo::Move || target)
        {
            const Part parts[3] = { Part::MoveX, Part::MoveY, Part::MoveZ };
            for (int i = 0; i < 3; ++i)
            {
                double along = 0.0;
                const double d = sv::raySegmentDistance(ray.o, ray.d, anchor + kAxes[i] * (size * 0.2), anchor + kAxes[i] * size, &along);
                consider(parts[i], d, along);
            }
            double along = 0.0;
            const double d = sv::rayPointDistance(ray.o, ray.d, anchor, &along);
            consider(Part::MovePlane, d, along, size * 0.08);
        }
        else if (sState.gizmo == Gizmo::Rotate)
        {
            double along = 0.0;
            double d = ringDistance(ray, yawRing(t, size), &along);
            consider(Part::Yaw, d, along);
            d = ringDistance(ray, tiltRing(t, size), &along);
            consider(Part::Tilt, d, along);
        }
        else
        {
            const sv::Frame end = sv::at(t, 1.0);
            double along = 0.0;
            double d = sv::raySegmentDistance(ray.o, ray.d, end.pos, end.pos + end.fwd * (size * 0.6), &along);
            consider(Part::Length, d, along);
            const sv::Frame mid = sv::at(t, 0.5);
            d = sv::rayPointDistance(ray.o, ray.d, mid.pos, &along);
            consider(Part::Bend, d, along, gizmoSize(mid.pos) * 0.07);
        }
    }
    if (best != Part::None) return best;
    // Nothing of the gizmo: the target handle, then the rail (both select).
    if (sState.aim.mode == sv::AimMode::Target)
    {
        const sv::V3 tg = agentFromGlobal(targetGlobal());
        double along = 0.0;
        const double d = sv::rayPointDistance(ray.o, ray.d, tg, &along);
        consider(Part::TargetHandle, d, along, gizmoSize(tg) * 0.08);
    }
    for (int i = 0; i < 24; ++i)
    {
        double along = 0.0;
        const double d = sv::raySegmentDistance(ray.o, ray.d, sv::at(t, i / 24.0).pos, sv::at(t, (i + 1) / 24.0).pos, &along);
        consider(Part::Rail, d, along, 0.04);
    }
    return best;
}

// The press on a gizmo part: remember where the drag starts.
void beginDrag(Part part, S32 x, S32 y)
{
    sState.drag = part;
    sState.track0 = sState.track;
    sState.target0 = sState.aim.target;
    const Ray ray = mouseRay(x, y);
    const sv::Track t = agentTrack();
    sv::V3 anchor;
    gizmoAnchor(anchor);
    sState.anchor = anchor;
    switch (part)
    {
    case Part::MoveX:
    case Part::MoveY:
    case Part::MoveZ:
        sState.axis = kAxes[part == Part::MoveX ? 0 : part == Part::MoveY ? 1 : 2];
        if (!sv::lineParamNearestRay(anchor, sState.axis, ray.o, ray.d, sState.param0)) sState.drag = Part::None;
        break;
    case Part::MovePlane:
        if (!sv::rayPlane(ray.o, ray.d, anchor, sv::V3(0, 0, 1), sState.hit0)) sState.drag = Part::None;
        break;
    case Part::Yaw:
    {
        const Ring ring = yawRing(t, 1.f);
        sv::V3 hit;
        if (!sv::rayPlane(ray.o, ray.d, ring.c, sv::cross(ring.e1, ring.e2), hit)) { sState.drag = Part::None; break; }
        sState.param0 = sv::ringAngle(ring.c, ring.e1, ring.e2, hit);
        break;
    }
    case Part::Tilt:
    {
        const Ring ring = tiltRing(t, 1.f);
        sv::V3 hit;
        if (!sv::rayPlane(ray.o, ray.d, ring.c, sv::cross(ring.e1, ring.e2), hit)) { sState.drag = Part::None; break; }
        sState.param0 = sv::ringAngle(ring.c, ring.e1, ring.e2, hit);
        break;
    }
    case Part::Length:
    {
        const sv::Frame end = sv::at(t, 1.0);
        sState.anchor = end.pos;
        sState.axis = end.fwd;
        if (!sv::lineParamNearestRay(end.pos, end.fwd, ray.o, ray.d, sState.param0)) sState.drag = Part::None;
        break;
    }
    case Part::Bend:
        break;   // absolute: the pointer's sideways distance from the start line
    default:
        sState.drag = Part::None;
        break;
    }
}

void followDrag(S32 x, S32 y)
{
    if (sState.drag == Part::None) return;
    const Ray ray = mouseRay(x, y);
    const bool target = sState.sel == Sel::Target;
    const double grid = gridStep();
    auto moveBy = [&](const sv::V3& delta)
    {
        if (target) sState.aim.target = sState.target0 + delta;
        else sState.track.origin = sState.track0.origin + delta;
    };
    switch (sState.drag)
    {
    case Part::MoveX:
    case Part::MoveY:
    case Part::MoveZ:
    {
        double t = 0.0;
        if (sv::lineParamNearestRay(sState.anchor, sState.axis, ray.o, ray.d, t)) moveBy(sState.axis * sv::snap(t - sState.param0, grid));
        break;
    }
    case Part::MovePlane:
    {
        sv::V3 hit;
        if (sv::rayPlane(ray.o, ray.d, sState.anchor, sv::V3(0, 0, 1), hit))
        {
            const sv::V3 d = hit - sState.hit0;
            moveBy(sv::V3(sv::snap(d.x, grid), sv::snap(d.y, grid), 0.0));
        }
        break;
    }
    case Part::Yaw:
    case Part::Tilt:
    {
        sv::Track t0 = sState.track0;
        t0.origin = agentFromGlobal(t0.origin);
        const Ring ring = sState.drag == Part::Yaw ? yawRing(t0, 1.f) : tiltRing(t0, 1.f);
        sv::V3 hit;
        if (!sv::rayPlane(ray.o, ray.d, ring.c, sv::cross(ring.e1, ring.e2), hit)) break;
        const double delta = std::remainder(sv::ringAngle(ring.c, ring.e1, ring.e2, hit) - sState.param0, 2.0 * sv::kPi);
        const double step = snapOn() ? kRotateSnap : 0.0;
        if (sState.drag == Part::Yaw) sState.track.yaw = sState.track0.yaw + sv::snap(delta, step);
        else sState.track.tilt = sState.track0.tilt + sv::snap(delta, step);
        sanitize(sState.track);
        break;
    }
    case Part::Length:
    {
        double t = 0.0;
        if (sv::lineParamNearestRay(sState.anchor, sState.axis, ray.o, ray.d, t))
        {
            sState.track.length = sState.track0.length + sv::snap(t - sState.param0, grid);
            sanitize(sState.track);
        }
        break;
    }
    case Part::Bend:
    {
        sv::Track t0 = sState.track0;
        t0.origin = agentFromGlobal(t0.origin);
        const sv::Basis b = sv::basis(t0);
        sv::V3 hit;
        if (sv::rayPlane(ray.o, ray.d, t0.origin, b.up, hit))
        {
            const double bulge = sv::snap(sv::dot(hit - t0.origin, b.left), grid > 0.0 ? grid * 0.25 : 0.0);
            sState.track.radius = sv::radiusForBulge(sState.track.length, bulge);
            sanitize(sState.track);
        }
        break;
    }
    default:
        break;
    }
}

// ---- drawing (world-overlay segment, agent space) ----
void vtx(const sv::V3& p) { gGL.vertex3f((F32)p.x, (F32)p.y, (F32)p.z); }
void color(const LLColor4& c, F32 alpha) { gGL.color4f(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], alpha); }
void line(const sv::V3& a, const sv::V3& b)
{
    vtx(a);
    vtx(b);
}
void diamond(const sv::V3& c, F32 size, const LLColor4& col, F32 alpha)
{
    const sv::V3 tips[6] = { c + sv::V3(size, 0, 0), c - sv::V3(size, 0, 0), c + sv::V3(0, size, 0),
                             c - sv::V3(0, size, 0), c + sv::V3(0, 0, size), c - sv::V3(0, 0, size) };
    static const S32 faces[8][3] = { { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 },
                                     { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 } };
    gGL.begin(LLRender::TRIANGLES);
    for (S32 f = 0; f < 8; ++f)
    {
        const F32 shade = f < 4 ? 1.f : 0.7f;
        gGL.color4f(col.mV[VRED] * shade, col.mV[VGREEN] * shade, col.mV[VBLUE] * shade, alpha);
        for (S32 v = 0; v < 3; ++v) vtx(tips[faces[f][v]]);
    }
    gGL.end();
}
// An arrow: a shaft and a four-sided cone.
void arrow(const sv::V3& from, const sv::V3& dir, F32 len, const LLColor4& col, F32 alpha)
{
    const sv::V3 d = sv::normalize(dir);
    const sv::V3 tip = from + d * len, base = from + d * (len * 0.78);
    const sv::V3 u = sv::normalize(sv::cross(d, std::fabs(d.z) < 0.9 ? sv::V3(0, 0, 1) : sv::V3(1, 0, 0)));
    const sv::V3 v = sv::cross(d, u);
    const F32 r = len * 0.07f;
    gGL.begin(LLRender::LINES);
    color(col, alpha);
    line(from + d * (len * 0.2), base);
    gGL.end();
    gGL.begin(LLRender::TRIANGLES);
    const sv::V3 rim[4] = { base + u * r, base + v * r, base - u * r, base - v * r };
    for (int i = 0; i < 4; ++i)
    {
        const F32 shade = 0.75f + 0.25f * (i % 2);
        gGL.color4f(col.mV[VRED] * shade, col.mV[VGREEN] * shade, col.mV[VBLUE] * shade, alpha);
        vtx(tip);
        vtx(rim[i]);
        vtx(rim[(i + 1) % 4]);
        vtx(tip);
        vtx(rim[(i + 1) % 4]);
        vtx(rim[i]);
    }
    gGL.end();
}
void ring(const Ring& r, const LLColor4& col, F32 alpha)
{
    gGL.begin(LLRender::LINES);
    color(col, alpha);
    constexpr int N = 64;
    for (int i = 0; i < N; ++i)
    {
        const double a0 = i * 2.0 * sv::kPi / N, a1 = (i + 1) * 2.0 * sv::kPi / N;
        line(r.c + (r.e1 * std::cos(a0) + r.e2 * std::sin(a0)) * r.r, r.c + (r.e1 * std::cos(a1) + r.e2 * std::sin(a1)) * r.r);
    }
    gGL.end();
}
LLColor4 lit(const LLColor4& c, bool on) { return on ? lerp(c, LLColor4::white, 0.55f) : c; }

void drawTrack(F32 alphaScale)
{
    const sv::Track t = agentTrack();
    const LLColor4 railCol(0.35f, 0.75f, 1.f, 1.f), tieCol(0.75f, 0.85f, 0.95f, 1.f);
    const bool selected = sState.sel == Sel::Track;
    const F32 a = alphaScale * (selected ? 0.95f : 0.7f);
    constexpr int N = 96;
    const double gauge = 0.06;   // half the distance between the two rails
    gGL.begin(LLRender::LINES);
    color(lit(railCol, sState.hover == Part::Rail), a);
    for (int i = 0; i < N; ++i)
    {
        const sv::Frame f0 = sv::at(t, (double)i / N), f1 = sv::at(t, (double)(i + 1) / N);
        line(f0.pos + f0.left * gauge, f1.pos + f1.left * gauge);
        line(f0.pos - f0.left * gauge, f1.pos - f1.left * gauge);
    }
    // ties every half metre (at most 400)
    color(tieCol, a * 0.6f);
    const int ties = std::min(400, (int)(t.length / 0.5) + 1);
    for (int i = 0; i <= ties; ++i)
    {
        const sv::Frame f = sv::at(t, ties ? (double)i / ties : 0.0);
        line(f.pos + f.left * (gauge * 1.8), f.pos - f.left * (gauge * 1.8));
    }
    gGL.end();
    // end handles: start green, end red
    const sv::V3 p0 = sv::at(t, 0.0).pos, p1 = sv::at(t, 1.0).pos;
    diamond(p0, gizmoSize(p0) * 0.06f, LLColor4(0.3f, 0.95f, 0.45f, 1.f), a);
    diamond(p1, gizmoSize(p1) * 0.06f, LLColor4(1.f, 0.4f, 0.35f, 1.f), a);

    // the cart and where it looks: a small frustum
    const double s = cartPosition();
    const sv::Frame cart = sv::at(t, s);
    sv::Aim aim = sState.aim;
    aim.target = agentFromGlobal(targetGlobal());
    const sv::CamAxes c = sv::cameraAxes(sv::aimDir(t, aim, cart, s));
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const F32 depth = gizmoSize(cart.pos) * 0.5f;
    const F32 hh = tanf(cam->getView() * 0.5f) * depth, hw = hh * llmax(cam->getAspect(), 0.1f);
    const LLColor4 cartCol(1.f, 0.85f, 0.25f, 1.f);
    diamond(cart.pos, gizmoSize(cart.pos) * 0.045f, cartCol, a);
    gGL.begin(LLRender::LINES);
    color(cartCol, a);
    const sv::V3 centre = cart.pos + c.at * depth;
    const sv::V3 corners[4] = { centre + c.left * hw + c.up * hh, centre - c.left * hw + c.up * hh, centre - c.left * hw - c.up * hh,
                                centre + c.left * hw - c.up * hh };
    for (int i = 0; i < 4; ++i)
    {
        line(cart.pos, corners[i]);
        line(corners[i], corners[(i + 1) % 4]);
    }
    // the up mark (which way is up in the picture)
    line(corners[0] + (corners[1] - corners[0]) * 0.35, centre + c.up * (hh * 1.35));
    line(corners[0] + (corners[1] - corners[0]) * 0.65, centre + c.up * (hh * 1.35));
    gGL.end();

    // the lock-on target
    if (sState.aim.mode == sv::AimMode::Target)
    {
        const sv::V3 tg = agentFromGlobal(targetGlobal());
        const LLColor4 tgCol(1.f, 0.55f, 0.1f, 1.f);
        diamond(tg, gizmoSize(tg) * 0.07f, lit(tgCol, sState.hover == Part::TargetHandle || sState.sel == Sel::Target), a);
        gGL.begin(LLRender::LINES);
        color(tgCol, a * 0.35f);
        line(cart.pos, tg);
        gGL.end();
    }
}

void drawGizmo()
{
    sv::V3 anchor;
    if (!gizmoAnchor(anchor)) return;
    const F32 size = gizmoSize(anchor);
    const sv::Track t = agentTrack();
    const LLColor4 axisCol[3] = { LLColor4(0.95f, 0.25f, 0.25f, 1.f), LLColor4(0.3f, 0.9f, 0.3f, 1.f), LLColor4(0.3f, 0.5f, 1.f, 1.f) };
    const Part movePart[3] = { Part::MoveX, Part::MoveY, Part::MoveZ };
    auto active = [](Part p) { return sState.drag == p || (sState.drag == Part::None && sState.hover == p); };
    if (sState.gizmo == Gizmo::Move || sState.sel == Sel::Target)
    {
        for (int i = 0; i < 3; ++i)
            arrow(anchor, kAxes[i], size, lit(axisCol[i], active(movePart[i])), 1.f);
        diamond(anchor, size * 0.06f, lit(LLColor4(0.85f, 0.85f, 0.85f, 1.f), active(Part::MovePlane)), 0.9f);
        return;
    }
    if (sState.gizmo == Gizmo::Rotate)
    {
        ring(yawRing(t, size), lit(axisCol[2], active(Part::Yaw)), 1.f);
        ring(tiltRing(t, size), lit(axisCol[1], active(Part::Tilt)), 1.f);
        return;
    }
    const sv::Frame end = sv::at(t, 1.0), mid = sv::at(t, 0.5);
    arrow(end.pos, end.fwd, size * 0.6f, lit(LLColor4(1.f, 0.6f, 0.2f, 1.f), active(Part::Length)), 1.f);
    const F32 bs = gizmoSize(mid.pos) * 0.05f;
    diamond(mid.pos, bs, lit(LLColor4(0.25f, 0.95f, 0.95f, 1.f), active(Part::Bend)), 1.f);
    // the bend handle's guide: sideways across the rail
    gGL.begin(LLRender::LINES);
    color(LLColor4(0.25f, 0.95f, 0.95f, 1.f), 0.5f);
    line(mid.pos - mid.left * (size * 0.5), mid.pos + mid.left * (size * 0.5));
    gGL.end();
}

// ---- the panel's rows ----
struct Built
{
    std::vector<SLVideoStudio::Row> rows;
    std::vector<Act> acts;
    std::vector<std::string> saved;   // Act::Saved rows' names, by row
};
SLVideoStudio::Row& add(Built& b, Act act, S32 kind, const std::string& label, const std::string& value = std::string())
{
    SLVideoStudio::Row r;
    r.kind = kind;
    r.label = label;
    r.value = value;
    b.rows.push_back(r);
    b.acts.push_back(act);
    b.saved.emplace_back();
    return b.rows.back();
}
std::string deg(double rad) { return llformat("%.1f\xC2\xB0", rad * 180.0 / sv::kPi); }
// Slider mappings: fraction <-> value
F32 fracLinear(double v, double lo, double hi) { return (F32)llclamp((v - lo) / (hi - lo), 0.0, 1.0); }
double valLinear(F32 f, double lo, double hi) { return lo + (hi - lo) * llclamp((double)f, 0.0, 1.0); }
F32 fracLog(double v, double lo, double hi) { return (F32)llclamp(std::log(v / lo) / std::log(hi / lo), 0.0, 1.0); }
double valLog(F32 f, double lo, double hi) { return lo * std::pow(hi / lo, llclamp((double)f, 0.0, 1.0)); }
constexpr double kLenLo = sv::kMinLength, kLenHi = 50.0, kRadLo = sv::kMinRadius, kRadHi = 100.0, kHeightHi = 30.0;
constexpr double kSpeedLo = 0.05, kSpeedHi = 10.0, kDurHi = 120.0;
const U32 kHeights[] = { 720, 1080, 1440, 2160 };
const U32 kRates[] = { 24, 30, 60 };

double landHeight()
{
    return LLWorld::getInstance()->resolveLandHeightGlobal(toD(sState.track.origin));
}

Built build()
{
    Built b;
    const SLVideoStudio::Panel panel = sState.panel;
    const bool has = sState.hasTrack;
    if (panel == SLVideoStudio::PANEL_TRACK)
    {
        SLVideoStudio::Row& tb = add(b, Act::TrackButtons, SLVideoStudio::ROW_BUTTONS, "");
        tb.buttons = { has ? "New track" : "Rez a track" };
        if (has) tb.buttons.push_back("Bring here");
        if (!has)
        {
            add(b, Act::None, SLVideoStudio::ROW_NOTE,
                "A rail for the camera, in front of you: shape it with the handles in the world (only you see it).");
        }
        else
        {
            const char* giz[] = { "Move", "Rotate", "Shape" };
            add(b, Act::Gizmo, SLVideoStudio::ROW_CHOICE, "Handles", giz[(int)sState.gizmo]);
            SLVideoStudio::Row& snap = add(b, Act::Snap, SLVideoStudio::ROW_TOGGLE, "Snap to grid", snapOn() ? "On" : "Off");
            snap.on = snapOn();
            SLVideoStudio::Row& show = add(b, Act::ShowTrack, SLVideoStudio::ROW_TOGGLE, "Show the track", sState.showTrack ? "On" : "Off");
            show.on = sState.showTrack;
            const sv::Track& t = sState.track;
            add(b, Act::Length, SLVideoStudio::ROW_SLIDER, "Length", llformat("%.2f m", t.length)).fraction =
                std::sqrt(fracLinear(t.length, kLenLo, kLenHi));
            add(b, Act::Curve, SLVideoStudio::ROW_CHOICE, "Curve", !sv::curved(t) ? "Straight" : t.radius > 0 ? "Left" : "Right");
            SLVideoStudio::Row& rad = add(b, Act::Radius, SLVideoStudio::ROW_SLIDER, "Radius",
                                          sv::curved(t) ? llformat("%.2f m", std::fabs(t.radius)) : std::string("-"));
            rad.fraction = sv::curved(t) ? fracLog(std::fabs(t.radius), kRadLo, kRadHi) : 1.f;
            rad.enabled = sv::curved(t);
            SLVideoStudio::Row& arc = add(b, Act::Arc, SLVideoStudio::ROW_SLIDER, "Arc",
                                          sv::curved(t) ? deg(std::fabs(sv::arcAngle(t))) : std::string("-"));
            arc.fraction = (F32)(std::fabs(sv::arcAngle(t)) / sv::kMaxArc);
            arc.enabled = sv::curved(t);
            const double above = t.origin.z - landHeight();
            add(b, Act::Height, SLVideoStudio::ROW_SLIDER, "Height", llformat("%.2f m", above)).fraction =
                fracLinear(above, 0.0, kHeightHi);
            add(b, Act::Tilt, SLVideoStudio::ROW_SLIDER, "Tilt", deg(t.tilt)).fraction = fracLinear(t.tilt, -sv::kMaxTilt, sv::kMaxTilt);
            double heading = std::fmod(90.0 - t.yaw * 180.0 / sv::kPi + 720.0, 360.0);   // compass: 0 = north (+Y)
            add(b, Act::Heading, SLVideoStudio::ROW_SLIDER, "Heading", llformat("%.0f\xC2\xB0", heading)).fraction =
                (F32)(heading / 360.0);

            add(b, Act::None, SLVideoStudio::ROW_HEAD, "Aim");
            const char* aims[] = { "Along the track", "Lock on a target", "Blend start to end" };
            add(b, Act::AimMode, SLVideoStudio::ROW_CHOICE, "Camera", aims[(int)sState.aim.mode]);
            if (sState.aim.mode == sv::AimMode::Along)
            {
                add(b, Act::Pan, SLVideoStudio::ROW_SLIDER, "Turn", deg(sState.aim.pan)).fraction =
                    fracLinear(sState.aim.pan, -sv::kPi, sv::kPi);
                add(b, Act::AimTilt, SLVideoStudio::ROW_SLIDER, "Look up", deg(sState.aim.tiltOffset)).fraction =
                    fracLinear(sState.aim.tiltOffset, -sv::kPi / 2, sv::kPi / 2);
            }
            else if (sState.aim.mode == sv::AimMode::Target)
            {
                add(b, Act::None, SLVideoStudio::ROW_NOTE,
                    sState.pickArmed ? std::string("Click an avatar, object or spot in the world...")
                    : sState.targetObject.notNull() ? "Following " + (sState.targetName.empty() ? std::string("an object") : sState.targetName)
                                                    : std::string("A point: drag its orange handle, or pick something to follow."));
                SLVideoStudio::Row& tr = add(b, Act::TargetButtons, SLVideoStudio::ROW_BUTTONS, "");
                tr.buttons = { "Focus point", sState.pickArmed ? "Cancel pick" : "Pick in world" };
                if (sState.targetObject.notNull()) tr.buttons.push_back("Unfollow");
            }
            else
            {
                add(b, Act::None, SLVideoStudio::ROW_NOTE, "Point the camera, then set the aim at the start or the end of the ride.");
                add(b, Act::BlendButtons, SLVideoStudio::ROW_BUTTONS, "").buttons = { "Aim start here", "Aim end here" };
            }
        }
        add(b, Act::None, SLVideoStudio::ROW_HEAD, "Saved tracks");
        if (has)
        {
            SLVideoStudio::Row& sr = add(b, Act::SaveButtons, SLVideoStudio::ROW_BUTTONS, "");
            sr.buttons = { sState.savedName.empty() ? std::string("Save") : "Save " + sState.savedName, "Save as new" };
        }
        const std::vector<std::string> names = savedTracks();
        if (names.empty()) add(b, Act::None, SLVideoStudio::ROW_NOTE, "None yet.");
        for (const std::string& n : names)
        {
            SLVideoStudio::Row& r = add(b, Act::Saved, SLVideoStudio::ROW_BUTTONS, n);
            r.buttons = { "Load", sState.deleteArmed == n ? "Delete?" : "Delete" };
            r.on = n == sState.savedName;
            b.saved.back() = n;
        }
    }
    else if (panel == SLVideoStudio::PANEL_RIDE)
    {
        const sv::Ride& r = sState.ride;
        add(b, Act::Timing, SLVideoStudio::ROW_CHOICE, "Timing", sState.timingBySpeed ? "By speed" : "By duration");
        if (sState.timingBySpeed)
        {
            const double v = sv::cruiseSpeed(sState.track.length, r.duration, r.easeIn, r.easeOut);
            add(b, Act::Speed, SLVideoStudio::ROW_SLIDER, "Speed", llformat("%.2f m/s", v)).fraction = fracLog(llclamp(v, kSpeedLo, kSpeedHi), kSpeedLo, kSpeedHi);
            add(b, Act::None, SLVideoStudio::ROW_NOTE, llformat("One pass takes %.1f s.", r.duration));
        }
        else
            add(b, Act::Duration, SLVideoStudio::ROW_SLIDER, "Duration", llformat("%.1f s", r.duration)).fraction =
                fracLog(llclamp(r.duration, sv::kMinDuration, kDurHi), sv::kMinDuration, kDurHi);
        add(b, Act::EaseIn, SLVideoStudio::ROW_SLIDER, "Ease in", llformat("%.0f%%", r.easeIn * 100)).fraction = (F32)(r.easeIn / 0.5);
        add(b, Act::EaseOut, SLVideoStudio::ROW_SLIDER, "Ease out", llformat("%.0f%%", r.easeOut * 100)).fraction = (F32)(r.easeOut / 0.5);
        const char* reps[] = { "Once", "Loop", "Ping-pong" };
        add(b, Act::Repeat, SLVideoStudio::ROW_CHOICE, "Repeat", reps[(int)r.repeat]);
        add(b, Act::Reverse, SLVideoStudio::ROW_TOGGLE, "Reverse", r.reverse ? "On" : "Off").on = r.reverse;
        add(b, Act::ViewCart, SLVideoStudio::ROW_TOGGLE, "View from the cart", sState.viewFromCart ? "On" : "Off").on = sState.viewFromCart;
        const bool hide = gSavedSettings.getBOOL("SLVideoStudioHideUI");
        add(b, Act::HideUI, SLVideoStudio::ROW_TOGGLE, "Hide this panel while riding", hide ? "On" : "Off").on = hide;
        add(b, Act::ShowOnRide, SLVideoStudio::ROW_TOGGLE, "Show the track while riding", sState.showOnRide ? "On" : "Off").on = sState.showOnRide;
        add(b, Act::None, SLVideoStudio::ROW_NOTE, "Space plays and stops; Esc stops a ride and brings the panel back.");
    }
    else if (panel == SLVideoStudio::PANEL_RECORD)
    {
        const SLVideoRecorder::Path path = SLVideoRecorder::pathFor();
        static LLCachedControl<bool> s_force_png(gSavedSettings, "SLVideoStudioForcePNG", false);
        std::string how = path == SLVideoRecorder::Path::H264 ? "MP4 video, encoded on the GPU (H.264)."
                        : s_force_png ? "PNG image sequence (forced by SLVideoStudioForcePNG)."
                        : LLVulkanContext::instance().hdrDisplayActive() ? "PNG image sequence (HDR output is on; the video encoder is SDR)."
                                                 : "PNG image sequence (this GPU / driver has no H.264 encoder).";
        add(b, Act::None, SLVideoStudio::ROW_NOTE, how);
        const U32 h = gSavedSettings.getU32("SLVideoStudioRecordHeight");
        add(b, Act::Resolution, SLVideoStudio::ROW_CHOICE, "Size", llformat("%up", h));
        const U32 fps = gSavedSettings.getU32("SLVideoStudioRecordFps");
        add(b, Act::FrameRate, SLVideoStudio::ROW_CHOICE, "Frame rate", llformat("%u fps", fps));
        const bool fixed = path == SLVideoRecorder::Path::Png || gSavedSettings.getBOOL("SLVideoStudioFixedStep");
        SLVideoStudio::Row& fr = add(b, Act::FixedStep, SLVideoStudio::ROW_TOGGLE, "Fixed timestep", fixed ? "On" : "Off");
        fr.on = fixed;
        fr.enabled = path == SLVideoRecorder::Path::H264;
        const bool sound = gSavedSettings.getBOOL("SLVideoStudioRecordSound");
        SLVideoStudio::Row& so = add(b, Act::Sound, SLVideoStudio::ROW_TOGGLE, "Record sound", sound && !fixed ? "On" : "Off");
        so.on = sound && !fixed;
        so.enabled = !fixed;
        add(b, Act::None, SLVideoStudio::ROW_NOTE,
            fixed ? "Fixed timestep: the ride moves exactly one frame per frame of video, so the video is smooth even when "
                    "the viewer renders slower than real time. No sound."
                  : "Real time: the ride and the sound follow the clock; a slow frame shows as a stutter.");
        SLVideoStudio::Row& rb = add(b, Act::RecordButtons, SLVideoStudio::ROW_BUTTONS, "");
        rb.buttons = { SLVideoRecorder::recording() ? "Stop recording" : "Record the ride", "Open folder" };
        rb.enabled = has || SLVideoRecorder::recording();
        const std::string st = SLVideoRecorder::status();
        if (!st.empty()) add(b, Act::None, SLVideoStudio::ROW_NOTE, st);
        else if (!SLVideoRecorder::lastOutput().empty()) add(b, Act::None, SLVideoStudio::ROW_NOTE, "Last: " + SLVideoRecorder::lastOutput());
        add(b, Act::None, SLVideoStudio::ROW_NOTE,
            llformat("Up to %u s per recording. The world behind the camera (avatars, water, sky) moves in real time; "
                     "Freeze World (Ctrl+Shift+F) holds it.", SLVideoRecorder::kMaxSeconds));
    }
    else
    {
        const bool on = gSavedSettings.getBOOL("SLInstantReplay");
        add(b, Act::Replay, SLVideoStudio::ROW_TOGGLE, "Instant replay", on ? "On" : "Off").on = on;
        add(b, Act::None, SLVideoStudio::ROW_NOTE,
            "While on, the last 30 seconds of the window (the interface and the viewer's sound included) are kept in a "
            "fixed amount of memory; Save Replay writes them to an MP4 in Videos/Prestige.");
        add(b, Act::ReplayButtons, SLVideoStudio::ROW_BUTTONS, "").buttons = { "Save replay (Alt+F10)", "Open folder" };
        if (!LLInstantReplay::encoderAvailable())
            add(b, Act::None, SLVideoStudio::ROW_NOTE, "This GPU / driver can't encode H.264 video: instant replay isn't available.");
    }
    return b;
}

void cycleSetting(const char* name, const U32* values, size_t n, S32 dir)
{
    const U32 cur = gSavedSettings.getU32(name);
    size_t i = 0;
    while (i < n && values[i] != cur) ++i;
    i = i >= n ? 0 : (i + n + (dir > 0 ? 1 : n - 1)) % n;
    gSavedSettings.setU32(name, values[i]);
}

void setRideDuration(double seconds)
{
    sState.ride.duration = llclamp(seconds, sv::kMinDuration, sv::kMaxDuration);
}

// Turns one row (a slider by a step, a choice to the next, a toggle over).
void turnAct(Act act, S32 dir)
{
    sv::Track& t = sState.track;
    const double grid = gridStep();
    switch (act)
    {
    case Act::Gizmo: sState.gizmo = (Gizmo)(((int)sState.gizmo + (dir > 0 ? 1 : 2)) % 3); sState.sel = Sel::Track; break;
    case Act::Snap: gSavedSettings.setBOOL("SnapEnabled", !snapOn()); break;
    case Act::ShowTrack: sState.showTrack = !sState.showTrack; break;
    case Act::Length: t.length += dir * (grid > 0.0 ? grid : 0.1); break;
    case Act::Curve:
    {
        // Straight -> Left -> Right (a curve keeps its radius when it changes side)
        const int cur = !sv::curved(t) ? 0 : t.radius > 0 ? 1 : 2;
        const int next = (cur + (dir > 0 ? 1 : 2)) % 3;
        const double r = sv::curved(t) ? std::fabs(t.radius) : std::max(3.0, t.length / 1.5);
        t.radius = next == 0 ? 0.0 : next == 1 ? r : -r;
        break;
    }
    case Act::Radius:
        if (sv::curved(t)) t.radius = (t.radius > 0 ? 1.0 : -1.0) * std::max(sv::kMinRadius, std::fabs(t.radius) + dir * (grid > 0.0 ? grid : 0.1));
        break;
    case Act::Arc: sv::setArcAngle(t, std::fabs(sv::arcAngle(t)) + dir * kRotateSnap); break;
    case Act::Height: t.origin.z += dir * (grid > 0.0 ? grid : 0.1); break;
    case Act::Tilt: t.tilt += dir * kRotateSnap; break;
    case Act::Heading: t.yaw -= dir * kRotateSnap; break;   // compass heading turns clockwise
    case Act::AimMode: sState.aim.mode = (sv::AimMode)(((int)sState.aim.mode + (dir > 0 ? 1 : 2)) % 3); sState.pickArmed = false; break;
    case Act::Pan: sState.aim.pan = std::remainder(sState.aim.pan + dir * kRotateSnap, 2.0 * sv::kPi); break;
    case Act::AimTilt: sState.aim.tiltOffset = llclamp(sState.aim.tiltOffset + dir * kRotateSnap, -sv::kPi / 2, sv::kPi / 2); break;
    case Act::Timing: sState.timingBySpeed = !sState.timingBySpeed; break;
    case Act::Duration: setRideDuration(sState.ride.duration + dir * (sState.ride.duration < 10.0 ? 0.5 : 1.0)); break;
    case Act::Speed:
    {
        const sv::Ride& r = sState.ride;
        const double v = llmax(kSpeedLo, sv::cruiseSpeed(t.length, r.duration, r.easeIn, r.easeOut) + dir * 0.05);
        setRideDuration(sv::durationForSpeed(t.length, v, r.easeIn, r.easeOut));
        break;
    }
    case Act::EaseIn: sState.ride.easeIn = llclamp(sState.ride.easeIn + dir * 0.05, 0.0, 0.5); break;
    case Act::EaseOut: sState.ride.easeOut = llclamp(sState.ride.easeOut + dir * 0.05, 0.0, 0.5); break;
    case Act::Repeat: sState.ride.repeat = (sv::Repeat)(((int)sState.ride.repeat + (dir > 0 ? 1 : 2)) % 3); break;
    case Act::Reverse: sState.ride.reverse = !sState.ride.reverse; break;
    case Act::ViewCart: sState.viewFromCart = !sState.viewFromCart; break;
    case Act::HideUI: gSavedSettings.setBOOL("SLVideoStudioHideUI", !gSavedSettings.getBOOL("SLVideoStudioHideUI")); break;
    case Act::ShowOnRide: sState.showOnRide = !sState.showOnRide; break;
    case Act::Resolution: cycleSetting("SLVideoStudioRecordHeight", kHeights, LL_ARRAY_SIZE(kHeights), dir); break;
    case Act::FrameRate: cycleSetting("SLVideoStudioRecordFps", kRates, LL_ARRAY_SIZE(kRates), dir); break;
    case Act::FixedStep:
        if (SLVideoRecorder::pathFor() == SLVideoRecorder::Path::H264 && !SLVideoRecorder::busy())
            gSavedSettings.setBOOL("SLVideoStudioFixedStep", !gSavedSettings.getBOOL("SLVideoStudioFixedStep"));
        break;
    case Act::Sound: gSavedSettings.setBOOL("SLVideoStudioRecordSound", !gSavedSettings.getBOOL("SLVideoStudioRecordSound")); break;
    case Act::Replay: gSavedSettings.setBOOL("SLInstantReplay", !gSavedSettings.getBOOL("SLInstantReplay")); break;
    default: break;
    }
    sanitize(sState.track);
    sanitize(sState.ride);
}

void setActFraction(Act act, F32 f)
{
    sv::Track& t = sState.track;
    const double grid = gridStep();
    switch (act)
    {
    case Act::Length: t.length = sv::snap(valLinear(f * f, kLenLo, kLenHi), grid > 0.0 ? grid : 0.05); break;
    case Act::Radius:
        if (sv::curved(t)) t.radius = (t.radius > 0 ? 1.0 : -1.0) * valLog(f, kRadLo, kRadHi);
        break;
    case Act::Arc: sv::setArcAngle(t, std::max(0.05, (double)f * sv::kMaxArc)); break;
    case Act::Height: t.origin.z = landHeight() + sv::snap(valLinear(f, 0.0, kHeightHi), grid > 0.0 ? grid : 0.05); break;
    case Act::Tilt: t.tilt = sv::snap(valLinear(f, -sv::kMaxTilt, sv::kMaxTilt), snapOn() ? kRotateSnap : 0.0); break;
    case Act::Heading:
    {
        const double heading = sv::snap(valLinear(f, 0.0, 360.0), snapOn() ? 5.625 : 0.0);
        t.yaw = (90.0 - heading) * sv::kPi / 180.0;
        break;
    }
    case Act::Pan: sState.aim.pan = valLinear(f, -sv::kPi, sv::kPi); break;
    case Act::AimTilt: sState.aim.tiltOffset = valLinear(f, -sv::kPi / 2, sv::kPi / 2); break;
    case Act::Duration: setRideDuration(std::round(valLog(f, sv::kMinDuration, kDurHi) * 10.0) / 10.0); break;
    case Act::Speed:
    {
        const sv::Ride& r = sState.ride;
        setRideDuration(sv::durationForSpeed(t.length, valLog(f, kSpeedLo, kSpeedHi), r.easeIn, r.easeOut));
        break;
    }
    case Act::EaseIn: sState.ride.easeIn = std::round(f * 0.5 * 100.0) / 100.0; break;
    case Act::EaseOut: sState.ride.easeOut = std::round(f * 0.5 * 100.0) / 100.0; break;
    default: turnAct(act, f >= 0.5f ? 1 : -1); return;   // a toggle / choice: on / off by side
    }
    sanitize(sState.track);
    sanitize(sState.ride);
}

void aimHere(bool start)
{
    const sv::V3 dir = toV(LLViewerCamera::getInstance()->getAtAxis());
    const sv::Basis b = sv::basis(sState.track);
    if (start) sv::yawPitchIn(b, dir, sState.aim.startYaw, sState.aim.startPitch);
    else sv::yawPitchIn(b, dir, sState.aim.endYaw, sState.aim.endPitch);
}

void pressAct(Act act, size_t button, const std::string& saved)
{
    switch (act)
    {
    case Act::TrackButtons:
        if (button == 0) rezTrack();
        else bringHere();
        break;
    case Act::TargetButtons:
        if (button == 0)
        {
            sState.aim.target = toV(gAgentCamera.getFocusGlobal());
            sState.targetObject.setNull();
            sState.targetName.clear();
            sState.pickArmed = false;
        }
        else if (button == 1)
            sState.pickArmed = !sState.pickArmed;
        else
        {
            sState.targetObject.setNull();   // the point stays where the object was
            sState.targetName.clear();
        }
        break;
    case Act::BlendButtons: aimHere(button == 0); break;
    case Act::SaveButtons:
    {
        if (button == 1 && savedTracks().size() >= kMaxTracks)
        {
            notify(llformat("Video Studio keeps at most %u saved tracks: delete one first.", (U32)kMaxTracks));
            break;
        }
        const std::string name = (button == 0 && !sState.savedName.empty()) ? sState.savedName : newTrackName();
        if (!saveTrack(name)) notify("Video Studio: the track wasn't saved.");
        break;
    }
    case Act::Saved:
        if (button == 0)
        {
            sState.deleteArmed.clear();
            if (!loadTrack(saved)) notify("Video Studio: couldn't read the track \"" + saved + "\".");
        }
        else if (sState.deleteArmed != saved)
            sState.deleteArmed = saved;
        else
        {
            LLFile::remove(trackPath(saved));
            if (sState.savedName == saved) sState.savedName.clear();
            sState.deleteArmed.clear();
        }
        break;
    case Act::RecordButtons:
        if (button == 0) SLVideoStudio::toggleRecord();
        else
        {
            const std::string dir = LLInstantReplay::videoFolder();
            LLFile::mkdir(gDirUtilp->getDirName(dir));
            LLFile::mkdir(dir);
            if (gViewerWindow) gViewerWindow->openFolder(dir);
        }
        break;
    case Act::ReplayButtons:
        if (button == 0) LLInstantReplay::save();
        else
        {
            const std::string dir = LLInstantReplay::videoFolder();
            LLFile::mkdir(gDirUtilp->getDirName(dir));
            LLFile::mkdir(dir);
            if (gViewerWindow) gViewerWindow->openFolder(dir);
        }
        break;
    default:
        turnAct(act, 1);
        break;
    }
}

// The pointer picks the lock-on target: an avatar or object to follow, else the spot.
void pickTarget(S32 x, S32 y)
{
    sState.pickArmed = false;
    const LLPickInfo pick = gViewerWindow->pickImmediate(x, y, false, true);
    LLViewerObject* obj = pick.getObject();
    if (obj)
    {
        if (obj->isAttachment())
            if (LLVOAvatar* av = obj->getAvatar()) obj = av;
        if (!obj->isAvatar()) obj = obj->getRootEdit();
    }
    if (obj)
    {
        sState.targetObject = obj->getID();
        sState.targetName = obj->isAvatar() ? ((LLVOAvatar*)obj)->getFullname() : std::string("an object");
        targetGlobal();
    }
    else if (!pick.mPosGlobal.isExactlyZero())
    {
        sState.targetObject.setNull();
        sState.targetName.clear();
        sState.aim.target = toV(pick.mPosGlobal);
    }
}

// A right drag past the threshold orbits the camera's focus (as camera mode).
void followRightDrag()
{
    if (!sState.rightDown || !gViewerWindow) return;
    const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
    if (!sState.orbiting)
    {
        if ((F32)llmax(llabs(mouse.mX - sState.rightX), llabs(mouse.mY - sState.rightY)) <= kSlop) return;
        sState.orbiting = true;
        gAgentCamera.unlockView();
        gViewerWindow->hideCursor();
        gViewerWindow->moveCursorToCenter();
        const LLCoordGL centre = gViewerWindow->getCurrentMouse();
        sState.lastMX = centre.mX;
        sState.lastMY = centre.mY;
        return;
    }
    const S32 dx = mouse.mX - sState.lastMX, dy = mouse.mY - sState.lastMY;
    if (dx == 0 && dy == 0) return;
    const F32 radians_per_pixel = 360.f * DEG_TO_RAD / (F32)llmax(gViewerWindow->getWorldViewWidthScaled(), 1);
    if (dx) gAgentCamera.cameraOrbitAround(-(F32)dx * radians_per_pixel);
    if (dy) gAgentCamera.cameraOrbitOver(-(F32)dy * radians_per_pixel);
    gViewerWindow->moveCursorToCenter();
    const LLCoordGL centre = gViewerWindow->getCurrentMouse();
    sState.lastMX = centre.mX;
    sState.lastMY = centre.mY;
}

void stopRide()
{
    sState.playing = false;
    if (SLVideoRecorder::recording()) SLVideoRecorder::stop();
}
} // namespace

// ---- public: the mode ----

bool SLVideoStudio::usable()
{
    return slRmlUiStudioUsable();
}

void SLVideoStudio::toggle()
{
    if (sActive)
    {
        stop();
        return;
    }
    open(sState.panel);
}

void SLVideoStudio::open(Panel panel)
{
    sState.panel = panel;
    if (sActive) return;
    if (gDisconnected || !isAgentAvatarValid())
        return;
    if (!usable())
    {
        notify("Video Studio needs the Vulkan interface layer (RmlUi), which isn't running.");
        return;
    }
    start();
}

void SLVideoStudio::start()
{
    if (sActive || !gViewerWindow) return;
    if (SLCameraMode::active()) SLCameraMode::stop();   // one overlay at a time
    LL_INFOS("VideoStudio") << "Video Studio on" << LL_ENDL;
    slRmlUiRequestStudio();
    sState.uiWasOn = gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    sState.hudWasOn = LLPipeline::sShowHUDAttachments;
    sState.focus = LLHandle<LLView>();
    if (LLUICtrl* focused = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus())) sState.focus = focused->getHandle();
    gFocusMgr.setKeyboardFocus(nullptr);
    if (dynamic_cast<LLView*>(gFocusMgr.getMouseCapture())) gFocusMgr.setMouseCapture(nullptr);
    if (sState.uiWasOn) LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    LLPipeline::sShowHUDAttachments = false;
    sState.leftDown = sState.rightDown = sState.orbiting = sState.leftPassed = sState.rightPassed = false;
    sState.drag = sState.hover = Part::None;
    sState.playing = sState.scrubbing = sState.pickArmed = false;
    sState.wheelFraction = 0.f;
    sState.hadFocus = true;
    sActive = true;
}

void SLVideoStudio::stop()
{
    if (!sActive) return;
    LL_INFOS("VideoStudio") << "Video Studio off" << LL_ENDL;
    stopRide();   // a recording in progress is written with what it has
    sActive = false;
    if (sState.rightPassed)
    {
        LLToolPie::getInstance()->cancelRightPress();
        sState.rightPassed = false;
    }
    if (sState.orbiting && gViewerWindow) gViewerWindow->showCursor();
    sState.leftDown = sState.rightDown = sState.orbiting = false;
    sState.drag = Part::None;
    sState.scrubbing = false;
    if (sState.uiWasOn && !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
        LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    LLPipeline::sShowHUDAttachments = sState.hudWasOn;
    if (LLView* v = sState.focus.get())
        if (LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(v); ctrl && ctrl->isInVisibleChain() && ctrl->isInEnabledChain())
            ctrl->setFocus(true);
    sState.focus = LLHandle<LLView>();
}

void SLVideoStudio::draw()
{
    if (!sActive) return;
    if (gDisconnected || !gViewerWindow)
    {
        stop();
        return;
    }
    if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        // Ctrl+Alt+F1 brought the interface back: leave, and do not hide it again on the way out.
        sState.uiWasOn = false;
        stop();
        return;
    }
    if (slRmlUiStudioFailed())
    {
        LL_WARNS("VideoStudio") << "the Video Studio overlay is unavailable; leaving" << LL_ENDL;
        notify("Video Studio's overlay couldn't start (the interface layer failed).");
        stop();
        return;
    }
    SLVideoStudioView::markLayer();
    followRightDrag();
    if (sState.drag != Part::None && sState.leftDown)
    {
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        followDrag(mouse.mX, mouse.mY);
    }
    sState.hadFocus = gFocusMgr.getAppHasFocus();
}

void SLVideoStudio::update()
{
    if (SLVideoRecorder::busy()) SLVideoRecorder::update();
    if (!sActive) return;
    bool finished = false;
    if (SLVideoRecorder::recording())
    {
        cartPosition(&finished);
        if (SLVideoRecorder::rideDone(sState.ride)) SLVideoRecorder::stop();
        if (!SLVideoRecorder::recording())
        {
            sState.playing = false;
            sState.scrubU = sState.ride.repeat == sv::Repeat::Once ? 1.f : 0.f;
        }
        return;
    }
    if (sState.playing)
    {
        cartPosition(&finished);
        if (finished)
        {
            sState.playing = false;
            sState.scrubU = 1.f;
        }
    }
}

void SLVideoStudio::renderWorld()
{
    if (!sActive || !sState.hasTrack) return;
    if (SLVideoRecorder::busy()) return;   // never in the video (the overlay segment is before capture point 2)
    const bool riding = sState.playing;
    if (riding ? !sState.showOnRide : !sState.showTrack) return;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    // Two passes, as the pose handles: faint through the scene, then solid where nothing is in front.
    for (S32 pass = 0; pass < 2; ++pass)
    {
        const bool xray = pass == 0;
        LLRender::DepthScope depth(!xray, false);
        drawTrack(xray ? 0.3f : 1.f);
        if (!riding) drawGizmo();
    }
    gGL.flush();
}

bool SLVideoStudio::drivesCamera()
{
    if (!sActive || !sState.hasTrack) return false;
    return sState.playing || SLVideoRecorder::recording() || sState.scrubbing || sState.viewFromCart;
}

void SLVideoStudio::applyCamera()
{
    const sv::Track t = agentTrack();
    const double s = cartPosition();
    const sv::Frame f = sv::at(t, s);
    sv::Aim aim = sState.aim;
    aim.target = agentFromGlobal(targetGlobal());
    const sv::CamAxes c = sv::cameraAxes(sv::aimDir(t, aim, f, s));
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    cam->setOrigin(toAgentF(f.pos));
    cam->setAxes(toAgentF(c.at), toAgentF(c.left), toAgentF(c.up));
}

bool SLVideoStudio::focusPoint(LLVector3& agent_pos)
{
    if (!sActive || !sState.hasTrack || sState.aim.mode != sv::AimMode::Target || !drivesCamera()) return false;
    agent_pos = toAgentF(agentFromGlobal(targetGlobal()));
    return true;
}

bool SLVideoStudio::overlayHidden()
{
    return sActive && (sState.playing || SLVideoRecorder::recording()) && gSavedSettings.getBOOL("SLVideoStudioHideUI");
}

// ---- input ----

bool SLVideoStudio::handleMouseClick(S32 x, S32 y, MASK mask, EMouseClickType click, bool down)
{
    if (!sActive) return false;
    // The press that brought the window to the front goes nowhere.
    if ((click == CLICK_LEFT || click == CLICK_DOUBLELEFT) && down && (!sState.hadFocus || !gFocusMgr.getAppHasFocus()))
        return true;
    if (!overlayHidden() && SLVideoStudioView::handleMouse(x, y, mask, click, down)) return true;
    const bool modified = (mask & (MASK_ALT | MASK_CONTROL | MASK_SHIFT)) != 0;
    if (click == CLICK_RIGHT)
    {
        if (down)
        {
            sState.rightPassed = modified;
            if (modified) return false;
            sState.rightDown = true;
            sState.orbiting = false;
            sState.rightX = sState.lastMX = x;
            sState.rightY = sState.lastMY = y;
            return true;
        }
        if (sState.rightPassed)
        {
            sState.rightPassed = false;
            return false;
        }
        if (!sState.rightDown) return true;
        sState.rightDown = false;
        if (sState.orbiting)
        {
            sState.orbiting = false;
            const LLCoordGL press_point(sState.rightX, sState.rightY);
            gViewerWindow->releaseCursorAfterDrag(&press_point);
        }
        return true;   // no context menu in the studio
    }
    if (click != CLICK_LEFT && click != CLICK_DOUBLELEFT) return false;   // middle: voice, as everywhere
    if (down)
    {
        sState.leftPassed = modified;
        if (modified) return false;   // Alt-orbit, Ctrl+Alt-pan, ...: the camera tools
        sState.leftDown = true;
        sState.leftX = x;
        sState.leftY = y;
        if (sState.playing || SLVideoRecorder::busy()) return true;
        if (sState.pickArmed)
        {
            pickTarget(x, y);
            return true;
        }
        const Part part = pickPart(x, y);
        switch (part)
        {
        case Part::Rail: sState.sel = Sel::Track; break;
        case Part::TargetHandle:
            if (sState.targetObject.isNull()) sState.sel = Sel::Target;
            break;
        case Part::None: break;
        default: beginDrag(part, x, y); break;
        }
        return true;
    }
    if (sState.leftPassed)
    {
        sState.leftPassed = false;
        return false;
    }
    if (!sState.leftDown) return true;
    sState.leftDown = false;
    if (sState.drag != Part::None)
    {
        followDrag(x, y);
        sState.drag = Part::None;
    }
    return true;
}

void SLVideoStudio::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (!sActive || clicks == 0) return;
    const MASK mask = gKeyboard ? gKeyboard->currentMask(true) : MASK_NONE;
    if (!overlayHidden() && SLVideoStudioView::handleWheel(x, y, clicks, mask)) return;
    if (drivesCamera()) return;   // the camera is on the cart
    gAgentCamera.handleScrollWheel((F32)clicks);
}

void SLVideoStudio::handleScrollWheelFraction(F32 notches, F32 camera_delta)
{
    if (!sActive) return;
    sState.wheelFraction += notches;
    const S32 whole = (S32)sState.wheelFraction;
    const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
    if (!overlayHidden() && SLVideoStudioView::overDocument(mouse.mX, mouse.mY))
    {
        if (whole != 0)
        {
            sState.wheelFraction -= (F32)whole;
            handleScrollWheel(mouse.mX, mouse.mY, whole);
        }
        return;
    }
    sState.wheelFraction = 0.f;
    if (!drivesCamera()) gAgentCamera.handleScrollWheel(camera_delta);
}

bool SLVideoStudio::handleKey(KEY key, MASK mask)
{
    if (!sActive) return false;
    SLKeyCommands& keys = SLKeyCommands::instance();
    if (keys.matches("studio_exit", key, mask))
    {
        // The innermost thing first: a ride or a recording, a pick, a drag, then the studio.
        if (sState.playing || SLVideoRecorder::recording()) stopRide();
        else if (sState.pickArmed) sState.pickArmed = false;
        else if (sState.drag != Part::None)
        {
            sState.track = sState.track0;   // the drag is undone
            sState.aim.target = sState.target0;
            sState.drag = Part::None;
        }
        else stop();
        return true;
    }
    if (keys.matches("studio_play", key, mask))
    {
        if (!(gKeyboard && gKeyboard->getKeyRepeated(key))) togglePlay();
        return true;
    }
    return false;   // movement and camera keys go on
}

bool SLVideoStudio::handleHover(S32 x, S32 y, MASK mask)
{
    if (!sActive) return false;
    if (!overlayHidden() && SLVideoStudioView::overDocument(x, y))
    {
        sState.hover = Part::None;
        gViewerWindow->setCursor(slRmlUiCursorType());
        return true;
    }
    if (sState.drag != Part::None)
    {
        gViewerWindow->setCursor(UI_CURSOR_TOOLTRANSLATE);
        return true;
    }
    if (sState.playing || SLVideoRecorder::busy())
    {
        sState.hover = Part::None;
        return false;
    }
    if (sState.pickArmed)
    {
        gViewerWindow->setCursor(UI_CURSOR_CROSS);
        return true;
    }
    sState.hover = (mask & (MASK_ALT | MASK_CONTROL)) ? Part::None : pickPart(x, y);
    if (sState.hover != Part::None)
    {
        gViewerWindow->setCursor(UI_CURSOR_HAND);
        return true;
    }
    return false;
}

bool SLVideoStudio::blocksMenuShortcut(KEY key, MASK mask)
{
    return sActive && SLCameraMode::shortcutReachesHiddenUI(key, mask);
}

// ---- public: the overlay's data ----

SLVideoStudio::Panel SLVideoStudio::panel()
{
    return sState.panel;
}

void SLVideoStudio::setPanel(Panel panel)
{
    sState.panel = panel;
    sState.deleteArmed.clear();
}

std::string SLVideoStudio::panelTitle(Panel panel)
{
    switch (panel)
    {
    case PANEL_TRACK: return "Track";
    case PANEL_RIDE: return "Ride";
    case PANEL_RECORD: return "Record";
    case PANEL_REPLAY: return "Replay";
    default: return std::string();
    }
}

std::vector<SLVideoStudio::Row> SLVideoStudio::rows()
{
    return build().rows;
}

void SLVideoStudio::turn(size_t row, S32 dir)
{
    const Built b = build();
    if (row < b.acts.size() && b.rows[row].enabled && b.rows[row].kind != ROW_BUTTONS) turnAct(b.acts[row], dir);
}

void SLVideoStudio::setFraction(size_t row, F32 fraction)
{
    const Built b = build();
    if (row < b.acts.size() && b.rows[row].enabled && b.rows[row].kind == ROW_SLIDER) setActFraction(b.acts[row], llclamp(fraction, 0.f, 1.f));
}

void SLVideoStudio::press(size_t row, size_t button)
{
    const Built b = build();
    if (row < b.acts.size() && b.rows[row].enabled) pressAct(b.acts[row], button, b.saved[row]);
}

bool SLVideoStudio::playing()
{
    return sState.playing || SLVideoRecorder::recording();
}

void SLVideoStudio::togglePlay()
{
    if (!sState.hasTrack) return;
    if (playing())
    {
        stopRide();
        return;
    }
    if (SLVideoRecorder::busy()) return;   // still saving the last recording
    // From the scrub position (a finished Once ride starts again).
    if (sState.ride.repeat == sv::Repeat::Once && sState.scrubU >= 0.999f) sState.scrubU = 0.f;
    sState.playOffset = sState.scrubU * sState.ride.duration;
    sState.playStart = now();
    sState.playing = true;
    sState.drag = Part::None;
    sState.pickArmed = false;
}

F32 SLVideoStudio::scrub()
{
    if (!playing()) return sState.scrubU;
    const double T = sState.ride.duration;
    return (F32)(std::fmod(rideSeconds(), T) / T);
}

void SLVideoStudio::setScrub(F32 fraction, bool dragging)
{
    if (playing()) return;
    sState.scrubU = llclamp(fraction, 0.f, 1.f);
    sState.scrubbing = dragging;
}

std::string SLVideoStudio::timeText()
{
    const double T = sState.ride.duration;
    const double t = playing() ? std::fmod(rideSeconds(), T) : sState.scrubU * T;
    return llformat("%.1f / %.1f s", t, T);
}

bool SLVideoStudio::recording()
{
    return SLVideoRecorder::busy();
}

void SLVideoStudio::toggleRecord()
{
    if (SLVideoRecorder::recording())
    {
        stopRide();
        return;
    }
    if (!sState.hasTrack || SLVideoRecorder::busy()) return;
    SLVideoRecorder::Options o;
    o.height = gSavedSettings.getU32("SLVideoStudioRecordHeight");
    o.fps = gSavedSettings.getU32("SLVideoStudioRecordFps");
    o.fixedStep = gSavedSettings.getBOOL("SLVideoStudioFixedStep");
    o.sound = gSavedSettings.getBOOL("SLVideoStudioRecordSound");
    std::string why;
    sState.playing = false;
    sState.scrubbing = false;
    sState.drag = Part::None;
    sState.pickArmed = false;
    if (!SLVideoRecorder::start(o, why)) notify("Video Studio: can't record: " + why);
}

std::string SLVideoStudio::recordStatus()
{
    return SLVideoRecorder::status();
}

bool SLVideoStudio::hasTrack()
{
    return sState.hasTrack;
}

std::string SLVideoStudio::hint()
{
    if (!sState.hasTrack) return "Rez a track to start. Right-drag orbits, the wheel zooms, Esc leaves.";
    if (SLVideoRecorder::busy()) return "Recording: Esc or Stop ends it; the file is written when it ends.";
    if (playing()) return "Riding: Space or Esc stops.";
    if (sState.pickArmed) return "Click what the camera should follow. Esc cancels.";
    return "Drag the handles in the world; click the rail or the target to edit it. Space rides, right-drag orbits.";
}
