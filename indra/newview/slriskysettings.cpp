/**
 * @file slriskysettings.cpp
 * @brief Crash-loop guard for risky render settings (#736). See the header.
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

#include "slriskysettings.h"

#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llcontrol.h"
#include "lldir.h"
#include "llfile.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llstartup.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"

namespace
{
// A watched setting: risky when its value is >= risky_from; labels name the values
// 0..label_count-1 for the log and the notice.
struct RiskySetting
{
    const char* name;
    const char* title;
    S32 risky_from;
    const char* const* labels;
    S32 label_count;
};

const char* const kDetailLabels[] = {"Static Only", "Static+Dynamic", "Realtime", "Raytraced"};
const char* const kShadowLabels[] = {"Off", "Shadow maps", "Ray traced"};

const RiskySetting kRisky[] = {
    {"RenderReflectionProbeDetail", "Reflection detail", 3, kDetailLabels, 4},
    {"RenderVKSunShadows", "Sun/moon shadows", 2, kShadowLabels, 3},
    {"RenderVKProjectorShadows", "Projector shadows", 2, kShadowLabels, 3},
    // (Not RenderVKRayTracing: restart-only, so the session that raises it never runs it
    // and the in-world probation would expire before it is exercised; the device-loss
    // safe settings already cover it.)
};

// In-world seconds a raised setting must survive before it is trusted.
constexpr F32 kProbationSecs = 120.f;
// While on probation the file is rewritten at most this often (the clock only).
constexpr F32 kSaveEverySecs = 10.f;

// VR start guard: in-world seconds a VR session must last before its start is trusted.
constexpr F32 kVrSafeSecs = 30.f;

bool        sActive = false;   // startup ran for a first instance
// {session_open, world, version, entries: {name: {old, new, secs}}, vr_pending, vr_restore}
LLSD        sState;
F32         sSinceSave = 0.f;
// The file as the previous session left it, when vrStartup read it before startup (vrStartup
// rewrites the file before the OpenXR probe, so startup must not read it again).
LLSD        sPrev;
bool        sPrevRead = false;
bool        sVrPending = false;   // this session started VR and has not reached the safe point
bool        sVrRestore = false;   // the guard turned SlipstreamVREnabled off: back on at a safe VR start
F32         sVrWorldSecs = 0.f;
std::vector<boost::signals2::scoped_connection> sConnections;

const RiskySetting* find(const std::string& name)
{
    for (const RiskySetting& r : kRisky)
        if (name == r.name) return &r;
    return nullptr;
}

std::string valueLabel(const RiskySetting& r, S32 v)
{
    if (v >= 0 && v < r.label_count) return llformat("%s (%d)", r.labels[v], v);
    if (v < 0 && r.labels == kDetailLabels) return llformat("Off (%d)", v);
    return llformat("%d", v);
}

std::string filePath()
{
    LLAppViewer* app = LLAppViewer::instance();
    const std::string base = app ? app->getInstanceLogBaseName() : std::string("Slipstream");
    return gDirUtilp->getExpandedFilename(LL_PATH_LOGS, base + ".risky_settings");
}

void save()
{
    sSinceSave = 0.f;
    const std::string path = filePath();
    llofstream out(path.c_str());
    if (!out.is_open())
    {
        LL_WARNS("RiskySettings") << "cannot write " << path << LL_ENDL;
        return;
    }
    LLSDSerialize::toPrettyXML(sState, out);
}

LLSD load()
{
    LLSD data;
    const std::string path = filePath();
    if (!gDirUtilp->fileExists(path)) return data;
    llifstream in(path.c_str());
    if (!in.is_open() || LLSDSerialize::fromXML(data, in) == LLSDParser::PARSE_FAILURE || !data.isMap())
    {
        LL_WARNS("RiskySettings") << "unreadable " << path << " (ignored)" << LL_ENDL;
        return LLSD();
    }
    return data;
}

bool inWorldNow()
{
    return LLStartUp::getStartupState() >= STATE_STARTED;
}

// A watched setting changed (any source: preferences, a preset, auto-tune, the VRAM ladder).
void onChanged(const RiskySetting& r, const LLSD& new_val, const LLSD& old_val)
{
    const S32 nv = (S32)new_val.asInteger(), ov = (S32)old_val.asInteger();
    if (nv == ov) return;
    LL_INFOS("RiskySettings") << r.title << " (" << r.name << "): " << valueLabel(r, ov) << " -> " << valueLabel(r, nv)
                              << (inWorldNow() ? " (in world)" : " (not in world yet)") << LL_ENDL;
    LLSD& entries = sState["entries"];
    const bool had = entries.has(r.name);
    if (nv >= r.risky_from && ov < r.risky_from)
    {
        // Raised into the risky range: on probation. A re-raise keeps the value it came
        // from originally (the safe one) and restarts the clock.
        LLSD e = had ? entries[r.name] : LLSD::emptyMap();
        if (!had) e["old"] = ov;
        e["new"] = nv;
        e["secs"] = 0.0;
        entries[r.name] = e;
        if (inWorldNow()) sState["world"] = true;
        LL_INFOS("RiskySettings") << r.name << " on probation for " << (S32)kProbationSecs
                                  << " s in world: reverted to " << valueLabel(r, (S32)e["old"].asInteger())
                                  << " on the next launch if the viewer crashes before then" << LL_ENDL;
        save();
    }
    else if (had && nv < r.risky_from)
    {
        entries.erase(r.name);
        LL_INFOS("RiskySettings") << r.name << " left the risky range: probation ended" << LL_ENDL;
        save();
    }
    else if (had && nv >= r.risky_from)
    {
        entries[r.name]["new"] = nv;   // (moved within the risky range)
        save();
    }
}

void revertCrashed(const LLSD& prev)
{
    const LLSD& entries = prev["entries"];
    if (!entries.isMap() || entries.size() == 0) return;
    std::string list;
    for (LLSD::map_const_iterator it = entries.beginMap(); it != entries.endMap(); ++it)
    {
        const RiskySetting* r = find(it->first);
        LLControlVariable* c = r ? gSavedSettings.getControl(r->name) : nullptr;
        if (!c) continue;
        const S32 cur = (S32)c->getValue().asInteger();
        const S32 nv = (S32)it->second["new"].asInteger(), ov = (S32)it->second["old"].asInteger();
        const F32 secs = (F32)it->second["secs"].asReal();
        if (cur < r->risky_from)
        {
            // The risky value never reached the settings file (the session ended before a
            // save): nothing to revert.
            LL_INFOS("RiskySettings") << r->name << " was on probation (" << valueLabel(*r, ov) << " -> "
                                      << valueLabel(*r, nv) << ") but is " << valueLabel(*r, cur)
                                      << " now: nothing to revert" << LL_ENDL;
            continue;
        }
        c->setValue(LLSD(ov));
        list += "\n- " + std::string(r->title) + ": " + valueLabel(*r, cur) + " -> " + valueLabel(*r, ov);
        LL_WARNS("RiskySettings") << "The previous session crashed " << (S32)secs << " s (in world) after "
                                  << r->name << " was raised to " << valueLabel(*r, cur) << ": reverted to "
                                  << valueLabel(*r, ov) << " before the renderer starts" << LL_ENDL;
    }
    if (list.empty()) return;
    // Saved now: if this session crashes too, it must not come back.
    gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
    doOnIdleOneTime([list]()
    {
        LLSD args;
        args["CHANGES"] = list;
        LLNotificationsUtil::add("VKRiskySettingReverted", args);
    });
}
}

namespace SLRiskySettings
{
void startup(bool second_instance)
{
    if (sActive || second_instance || !gDirUtilp) return;
    sActive = true;
    // (vrStartup may have read it already, and rewritten the file since)
    const LLSD prev = sPrevRead ? sPrev : load();
    if (!sPrevRead) sVrRestore = prev.isMap() && prev["vr_restore"].asBoolean();
    sPrev = LLSD();
    const bool crashed = prev.isMap() && prev["session_open"].asBoolean();
    if (crashed && prev["world"].asBoolean())
    {
        revertCrashed(prev);
        sState = LLSD::emptyMap();   // consumed: never applied twice
    }
    else
    {
        sState = prev.isMap() ? prev : LLSD::emptyMap();
        if (crashed && prev["entries"].isMap() && prev["entries"].size() > 0)
            LL_INFOS("RiskySettings") << "previous session did not exit cleanly before reaching the world: "
                                         "probation entries kept, nothing reverted" << LL_ENDL;
    }
    if (!sState["entries"].isMap()) sState["entries"] = LLSD::emptyMap();
    sState["session_open"] = true;
    sState["world"] = false;
    sState["version"] = LLVersionInfo::instance().getChannelAndVersion();
    sState["vr_pending"] = sVrPending;
    sState["vr_restore"] = sVrRestore;
    save();
    for (const RiskySetting& r : kRisky)
    {
        LLControlVariable* c = gSavedSettings.getControl(r.name);
        if (!c) continue;
        const RiskySetting* rp = &r;
        sConnections.emplace_back(c->getSignal()->connect(
            [rp](LLControlVariable*, const LLSD& new_val, const LLSD& old_val) { onChanged(*rp, new_val, old_val); }));
    }
}

void tick(float dt_secs, bool in_world)
{
    if (!sActive || !in_world) return;
    if (sVrPending)
    {
        // VR start guard: this long in world, the VR start is trusted.
        sVrWorldSecs += std::max(dt_secs, 0.f);
        if (sVrWorldSecs >= kVrSafeSecs)
        {
            sVrPending = false;
            sState["vr_pending"] = false;
            LL_INFOS("VR") << "VR start guard: " << (S32)kVrSafeSecs << " s in world, this VR start is trusted" << LL_ENDL;
            if (sVrRestore && gSavedSettings.getBOOL("SlipstreamVREnabled"))
            {
                // (turned off by the guard after an earlier crash; this VR start held: VR is the
                // saved choice again)
                gSavedSettings.setBOOL("SlipstreamVREnabled", true);
                LL_INFOS("VR") << "VR start guard: SlipstreamVREnabled saved on again (the guard had turned it off)" << LL_ENDL;
            }
            sVrRestore = false;
            sState["vr_restore"] = false;
            save();
        }
    }
    LLSD& entries = sState["entries"];
    if (!entries.isMap() || entries.size() == 0) return;
    bool dirty = false;
    if (!sState["world"].asBoolean())
    {
        sState["world"] = true;   // this session runs the risky values in world: a crash now counts
        dirty = true;
    }
    std::vector<std::string> done;
    for (LLSD::map_iterator it = entries.beginMap(); it != entries.endMap(); ++it)
    {
        const F32 secs = (F32)it->second["secs"].asReal() + std::max(dt_secs, 0.f);
        it->second["secs"] = secs;
        if (secs >= kProbationSecs) done.push_back(it->first);
    }
    for (const std::string& n : done)
    {
        entries.erase(n);
        LL_INFOS("RiskySettings") << n << " survived " << (S32)kProbationSecs << " s in world: probation over" << LL_ENDL;
        dirty = true;
    }
    sSinceSave += std::max(dt_secs, 0.f);
    if (dirty || sSinceSave >= kSaveEverySecs) save();
}

bool vrStartup(bool second_instance, bool forced)
{
    if (second_instance || !gDirUtilp || sActive || sPrevRead) return true;
    sPrev = load();
    sPrevRead = true;
    const bool prev_ok = sPrev.isMap();
    sVrRestore = prev_ok && sPrev["vr_restore"].asBoolean();
    const bool crashed_in_vr_start = prev_ok && sPrev["session_open"].asBoolean() && sPrev["vr_pending"].asBoolean();
    if (crashed_in_vr_start && !forced)
    {
        LL_WARNS("VR") << "VR start guard: the previous session started VR and ended without a clean exit before "
                       << (S32)kVrSafeSecs << " s in world: SlipstreamVREnabled turned off (saved), this launch starts "
                       << "on the desktop. Start in VR (login screen) or --vr tries VR again." << LL_ENDL;
        gSavedSettings.setBOOL("SlipstreamVREnabled", false);
        // Saved now: if this session crashes too, VR must not come back.
        gSavedSettings.saveToFile(gSavedSettings.getString("ClientSettingsFile"), true);
        sVrRestore = true;
        doOnIdleOneTime([]()
        {
            LLSD args;
            args["MESSAGE"] = "Slipstream closed while starting VR last time; started on the desktop. "
                              "Use Start in VR to try again.";
            LLNotificationsUtil::add("GenericAlert", args);
        });
        return false;   // (startup writes the file, from sPrev)
    }
    if (crashed_in_vr_start)
        LL_WARNS("VR") << "VR start guard: the previous VR start did not reach a safe point, but VR was asked for on "
                       << "the command line (--vr): starting VR" << LL_ENDL;
    // On record before the OpenXR probe: a crash from here on, before kVrSafeSecs in world or a
    // clean exit, starts the next launch on the desktop. The rest is the previous session's
    // (startup decides on sPrev, not on this).
    sVrPending = true;
    sVrWorldSecs = 0.f;
    sState = prev_ok ? sPrev : LLSD::emptyMap();
    sState["session_open"] = true;
    sState["vr_pending"] = true;
    sState["vr_restore"] = sVrRestore;
    save();
    return true;
}

void cleanExit()
{
    if (!sActive) return;
    sConnections.clear();
    sState["session_open"] = false;
    sState["world"] = false;
    sVrPending = false;
    sState["vr_pending"] = false;   // (a clean exit: this VR start did not crash)
    save();
    sActive = false;
}
}
