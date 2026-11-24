/**
 * @file prestigecamerafilters.cpp
 * @brief Camera mode's live filter stack. See prestigecamerafilters.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "prestigecamerafilters.h"

#include "prestigecubelut.h"
#include "slcameramodel.h"
#include "slfxpresets.h"
#include "slfxstack.h"

#include "lldir.h"
#include "llfile.h"
#include "llfilepicker.h"
#include "llframetimer.h"
#include "llimagepng.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace PrestigeCameraFilters
{
namespace
{
const char* kListSetting = "PrestigeCameraFilters";
const char* kCompareSetting = "PrestigeCameraFiltersCompare";
const char* kSplitSetting = "PrestigeCameraFiltersSplit";

bool sActive = false;

std::string str(const char* name, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
{
    return LLTrans::getString(std::string("PrestigeCameraFilters") + name, args);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string baseName(const std::string& p)
{
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// "Name@File.fx" -> "File.fx" / "Name".
std::string fileOf(const std::string& technique)
{
    const size_t at = technique.find('@');
    return at == std::string::npos ? std::string() : technique.substr(at + 1);
}

std::string nameOf(const std::string& technique)
{
    return technique.substr(0, technique.find('@'));
}

bool sameTechnique(const std::string& a, const std::string& b)
{
    return nameOf(a) == nameOf(b) && lower(fileOf(a)) == lower(fileOf(b));
}

// The effect file lies in a folder of this name (any level).
bool inFolder(const std::string& path, const char* folder)
{
    const std::string p = lower(path), f = lower(folder);
    for (const char* sep : { "/", "\\" })
        for (const char* end : { "/", "\\" })
            if (p.find(sep + f + end) != std::string::npos)
                return true;
    return false;
}

std::vector<Filter> load()
{
    std::vector<Filter> out;
    const LLSD list = gSavedSettings.getLLSD(kListSetting);
    if (!list.isArray())
        return out;
    for (LLSD::array_const_iterator it = list.beginArray(); it != list.endArray() && out.size() < kMaxFilters; ++it)
    {
        if (!it->isMap())
            continue;
        Filter f;
        f.technique = (*it)["technique"].asString();
        if (f.technique.find('@') == std::string::npos)
            continue;
        f.on = it->has("on") ? (*it)["on"].asBoolean() : true;
        f.strength = it->has("strength") ? llclamp((F32)(*it)["strength"].asReal(), 0.f, 1.f) : 1.f;
        bool dup = false;
        for (const Filter& o : out)
            dup |= sameTechnique(o.technique, f.technique);
        if (!dup)
            out.push_back(f);
    }
    return out;
}

void pushLayer()
{
    if (!sActive)
        return;
    std::vector<SLFxStack::LayerTech> layer;
    for (const Filter& f : load())
        if (f.on)
            layer.push_back({ f.technique, f.strength });
    SLFxStack::instance().setLayer(layer);
}

void pushSplit()
{
    if (!SLFxStack::instanceExists() && !sActive)
        return;
    SLFxStack::instance().setCompareSplit(sActive && compare() ? split() : -1.f);
}

void save(const std::vector<Filter>& list)
{
    LLSD a = LLSD::emptyArray();
    for (const Filter& f : list)
    {
        LLSD m = LLSD::emptyMap();
        m["technique"] = f.technique;
        m["on"] = f.on;
        m["strength"] = (F64)f.strength;
        a.append(m);
    }
    gSavedSettings.setLLSD(kListSetting, a);
    pushLayer();
}

// The effect behind a filter ("" = not installed).
std::string effectPath(const std::string& technique)
{
    return SLFxStack::instance().resolveEffect(fileOf(technique));
}

// ---- labels (the effects' ui_label), cached briefly: getTechniques walks every effect ----
const std::vector<SLFxTechniqueInfo>& techniques()
{
    static std::vector<SLFxTechniqueInfo> s_list;
    static LLFrameTimer s_age;
    static bool s_have = false;
    if (!s_have || s_age.getElapsedTimeF32() > 0.5f)
    {
        s_list = SLFxStack::instance().getTechniques();
        s_age.reset();
        s_have = true;
    }
    return s_list;
}

// A technique name made readable: "PrestigeSepia" -> "Sepia", "LUT_Teal_Orange" -> "Teal Orange".
std::string readable(const std::string& technique)
{
    std::string n = nameOf(technique);
    const std::string file = fileOf(technique);
    if (n == "PrestigeLUT" && lower(file).rfind("lut_", 0) == 0)
    {
        std::string stem = file.substr(4);
        if (stem.size() > 3 && lower(stem.substr(stem.size() - 3)) == ".fx")
            stem.resize(stem.size() - 3);
        std::replace(stem.begin(), stem.end(), '_', ' ');
        return "LUT: " + stem;
    }
    if (n.size() > 8 && n.rfind("Prestige", 0) == 0 && std::isupper((unsigned char)n[8]))
        n = n.substr(8);
    return n;
}

// ---- a filter's sliders ----
LLSD component(const LLSD& v, S32 c)
{
    if (v.isArray())
        return c < (S32)v.size() ? v[c] : LLSD();
    return c == 0 ? v : LLSD();
}

F32 rangeValue(const LLSD& v, S32 c, F32 fallback)
{
    const LLSD x = component(v, c);
    return x.isDefined() ? (F32)x.asReal() : fallback;
}

// One slider / toggle / choice: which uniform and component.
struct ParamRef
{
    SLFxUniformInfo uniform;
    S32 comp = 0;
    S32 kind = PARAM_SLIDER;
    F32 lo = 0.f, hi = 1.f, step = 0.01f;
    bool isInt = false;
};

std::vector<ParamRef> paramRefs(const std::string& path)
{
    std::vector<ParamRef> out;
    if (path.empty())
        return out;
    for (const SLFxUniformInfo& u : SLFxStack::instance().getUniforms(path))
    {
        if (u.mHidden || u.mNoEdit)
            continue;
        const SLFxPresets::EComponent kind = SLFxPresets::componentKind(u);
        const S32 count = llclamp(SLFxPresets::componentCount(u), 1, 4);
        const bool color = u.mType == "color3" || u.mType == "color4" || u.mUIType == "color";
        for (S32 c = 0; c < count; ++c)
        {
            ParamRef r;
            r.uniform = u;
            r.comp = c;
            r.isInt = kind == SLFxPresets::COMP_INT;
            if (kind == SLFxPresets::COMP_BOOL)
                r.kind = PARAM_TOGGLE;
            else if (count == 1 && (u.mUIType == "combo" || u.mUIType == "list" || u.mUIType == "radio") && !u.mItems.empty())
            {
                r.kind = PARAM_CHOICE;
                r.lo = 0.f;
                r.hi = (F32)(u.mItems.size() - 1);
                r.step = 1.f;
            }
            else
            {
                const F32 def = (F32)component(u.mDefault, c).asReal();
                const bool ranged = component(u.mMin, c).isDefined() && component(u.mMax, c).isDefined();
                r.lo = color ? 0.f : rangeValue(u.mMin, c, llmin(0.f, def));
                r.hi = color ? 1.f : rangeValue(u.mMax, c, r.isInt ? llmax(10.f, std::fabs(def) * 2.f) : llmax(1.f, std::fabs(def) * 2.f));
                if (!ranged && !color && r.lo >= r.hi)
                    r.hi = r.lo + 1.f;
                if (r.hi < r.lo)
                    std::swap(r.lo, r.hi);
                r.step = rangeValue(u.mStep, c, 0.f);
                if (r.step <= 0.f)
                    r.step = r.isInt ? 1.f : (r.hi > r.lo ? (r.hi - r.lo) / 100.f : 0.01f);
            }
            out.push_back(r);
        }
    }
    return out;
}

F32 currentValue(const ParamRef& r)
{
    LLSD v = component(r.uniform.mValue, r.comp);
    if (!v.isDefined())
        v = component(r.uniform.mDefault, r.comp);
    return v.isBoolean() ? (v.asBoolean() ? 1.f : 0.f) : (F32)v.asReal();
}

// The uniform's value with component `comp` set to `x`, typed as SLFxStack keeps it.
LLSD withComponent(const ParamRef& r, F64 x)
{
    const SLFxUniformInfo& u = r.uniform;
    const SLFxPresets::EComponent kind = SLFxPresets::componentKind(u);
    const S32 count = llclamp(SLFxPresets::componentCount(u), 1, 4);
    auto typed = [&](F64 v) -> LLSD
    {
        if (kind == SLFxPresets::COMP_BOOL)
            return LLSD(v != 0.0);
        if (kind == SLFxPresets::COMP_INT)
            return LLSD((S32)std::lround(v));
        return LLSD(v);
    };
    if (count == 1 && !u.mValue.isArray() && !u.mDefault.isArray())
        return typed(x);
    LLSD a = LLSD::emptyArray();
    for (S32 c = 0; c < count; ++c)
    {
        if (c == r.comp)
        {
            a.append(LLSD(typed(x).asReal()));
            continue;
        }
        LLSD v = component(u.mValue, c);
        if (!v.isDefined())
            v = component(u.mDefault, c);
        a.append(LLSD(v.isDefined() ? v.asReal() : 0.0));
    }
    return a;
}

void setValue(const std::string& path, const ParamRef& r, F64 x)
{
    if (r.kind == PARAM_SLIDER)
    {
        if (r.step > 0.f)
            x = r.lo + std::round((x - r.lo) / r.step) * r.step;
        x = llclamp(x, (F64)r.lo, (F64)r.hi);
    }
    SLFxStack::instance().setUniform(path, r.uniform.mName, withComponent(r, x));
}

std::string valueText(const ParamRef& r, F32 v)
{
    switch (r.kind)
    {
        case PARAM_TOGGLE: return str(v != 0.f ? "On" : "Off");
        case PARAM_CHOICE:
        {
            const S32 i = llclamp((S32)std::lround(v), 0, (S32)r.uniform.mItems.size() - 1);
            return i >= 0 ? r.uniform.mItems[(size_t)i] : std::string();
        }
        default: break;
    }
    std::string s;
    if (r.isInt)
        s = llformat("%d", (S32)std::lround(v));
    else if (r.step >= 1.f)
        s = llformat("%.0f", v);
    else if (r.step >= 0.1f)
        s = llformat("%.1f", v);
    else if (r.step >= 0.01f)
        s = llformat("%.2f", v);
    else
        s = llformat("%.3f", v);
    return s + r.uniform.mUnits;
}

bool refFor(size_t i, size_t p, std::string& path, ParamRef& out)
{
    const std::vector<Filter> list = load();
    if (i >= list.size())
        return false;
    path = effectPath(list[i].technique);
    const std::vector<ParamRef> refs = paramRefs(path);
    if (p >= refs.size())
        return false;
    out = refs[p];
    return true;
}

// ---- LUT import ----
// A file-name-safe form of the LUT's title.
std::string safeName(const std::string& title)
{
    std::string out;
    for (unsigned char c : title)
    {
        if (std::isalnum(c) || c == '-')
            out += (char)c;
        else if (!out.empty() && out.back() != '_')
            out += '_';
        if (out.size() >= 40)
            break;
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    return out.empty() ? std::string("Imported") : out;
}

// The title as a ReShade string literal's contents.
std::string literalText(const std::string& title)
{
    std::string out;
    for (unsigned char c : title)
        if (c >= 0x20 && c != '"' && c != '\\')
            out += (char)c;
    return out.substr(0, 80);
}

bool addFilter(const std::string& technique)
{
    std::vector<Filter> list = load();
    if (list.size() >= kMaxFilters)
        return false;
    for (const Filter& f : list)
        if (sameTechnique(f.technique, technique))
            return false;
    Filter f;
    f.technique = technique;
    list.push_back(f);
    save(list);
    return true;
}
} // namespace

// ============================================================================

void activate()
{
    sActive = true;
    pushLayer();
    pushSplit();
}

void deactivate()
{
    if (!sActive)
        return;
    sActive = false;
    if (SLFxStack::instanceExists())
    {
        SLFxStack::instance().setLayer({});
        SLFxStack::instance().setCompareSplit(-1.f);
    }
}

bool active()
{
    return sActive;
}

std::vector<Filter> filters()
{
    return load();
}

bool full()
{
    return load().size() >= kMaxFilters;
}

std::string label(const std::string& technique)
{
    for (const SLFxTechniqueInfo& t : techniques())
        if (sameTechnique(t.mId, technique))
        {
            if (t.mLabel.empty() || t.mLabel == t.mName)
                return readable(technique);
            return t.mLabel;
        }
    return readable(technique);
}

std::string status(size_t i)
{
    const std::vector<Filter> list = load();
    if (i >= list.size())
        return std::string();
    const std::string path = effectPath(list[i].technique);
    if (path.empty())
        return str("NotInstalled");
    // (cached briefly: a slider drag refreshes the rows every frame)
    static std::vector<SLFxEffectInfo> s_effects;
    static LLFrameTimer s_age;
    static bool s_have = false;
    if (!s_have || s_age.getElapsedTimeF32() > 0.25f)
    {
        s_effects = SLFxStack::instance().getAvailableEffects();
        s_age.reset();
        s_have = true;
    }
    for (const SLFxEffectInfo& e : s_effects)
    {
        if (e.mPath != path)
            continue;
        if (e.mQuarantined)
            return str("Quarantined");
        if (!e.mError.empty())
        {
            std::string first = e.mError.substr(0, e.mError.find('\n'));
            if (first.size() > 120)
                first = first.substr(0, 117) + "...";
            LLStringUtil::format_map_t args;
            args["[ERROR]"] = first;
            return str("Error", args);
        }
        if (e.mCompiling)
            return str("Loading");
        break;
    }
    return std::string();
}

bool add(const std::string& technique)
{
    if (full())
    {
        SLCameraModel::toast(str("Full"), 3.0);
        return false;
    }
    if (!addFilter(technique))
    {
        SLCameraModel::toast(str("AlreadyAdded"), 2.0);
        return false;
    }
    return true;
}

void remove(size_t i)
{
    std::vector<Filter> list = load();
    if (i >= list.size())
        return;
    list.erase(list.begin() + (std::ptrdiff_t)i);
    save(list);
}

void move(size_t from, size_t to)
{
    std::vector<Filter> list = load();
    if (from >= list.size() || from == to)
        return;
    to = llmin(to, list.size() - 1);
    const Filter f = list[from];
    list.erase(list.begin() + (std::ptrdiff_t)from);
    list.insert(list.begin() + (std::ptrdiff_t)to, f);
    save(list);
}

void setOn(size_t i, bool on)
{
    std::vector<Filter> list = load();
    if (i >= list.size() || list[i].on == on)
        return;
    list[i].on = on;
    save(list);
}

void setStrength(size_t i, F32 strength)
{
    std::vector<Filter> list = load();
    if (i >= list.size())
        return;
    strength = llclamp(std::round(strength * 100.f) / 100.f, 0.f, 1.f);
    if (list[i].strength == strength)
        return;
    list[i].strength = strength;
    save(list);
}

std::vector<Choice> choices()
{
    std::vector<Choice> out;
    const std::vector<Filter> list = load();
    for (const SLFxTechniqueInfo& t : techniques())
    {
        const std::string file = lower(baseName(t.mEffectPath));
        if (file == "prestigelut.fx")
            continue;   // the LUT template itself (it changes nothing without a LUT)
        if (!t.mEnabledInScreenshot)
            continue;   // a technique its author keeps out of shots: the photo would not match
        Choice c;
        c.technique = t.mId;
        c.label = (t.mLabel.empty() || t.mLabel == t.mName) ? readable(t.mId) : t.mLabel;
        if (inFolder(t.mEffectPath, "PrestigeLegacy"))
            c.group = GROUP_CLASSIC;
        else if (file.rfind("lut_", 0) == 0 && inFolder(t.mEffectPath, "PrestigeLUT"))
            c.group = GROUP_LUT;
        else
        {
            c.group = GROUP_EFFECT;
            c.note = baseName(t.mEffectPath);
        }
        for (const Filter& f : list)
            c.added |= sameTechnique(f.technique, t.mId);
        out.push_back(c);
    }
    std::stable_sort(out.begin(), out.end(), [](const Choice& a, const Choice& b)
    {
        if (a.group != b.group)
            return a.group < b.group;
        return lower(a.label) < lower(b.label);
    });
    return out;
}

bool loadingChoices()
{
    for (const SLFxEffectInfo& e : SLFxStack::instance().getAvailableEffects())
        if (e.mCompiling)
            return true;
    return false;
}

std::vector<Param> params(size_t i)
{
    std::vector<Param> out;
    const std::vector<Filter> list = load();
    if (i >= list.size())
        return out;
    for (const ParamRef& r : paramRefs(effectPath(list[i].technique)))
    {
        Param p;
        const F32 v = currentValue(r);
        p.kind = r.kind;
        p.label = r.uniform.mLabel.empty() ? r.uniform.mName : r.uniform.mLabel;
        const S32 count = llclamp(SLFxPresets::componentCount(r.uniform), 1, 4);
        if (count > 1)
        {
            const bool color = r.uniform.mType == "color3" || r.uniform.mType == "color4" || r.uniform.mUIType == "color";
            static const char* const rgba[] = { " R", " G", " B", " A" };
            static const char* const xyzw[] = { " x", " y", " z", " w" };
            p.label += (color ? rgba : xyzw)[r.comp];
        }
        p.value = valueText(r, v);
        p.on = v != 0.f;
        p.fraction = r.hi > r.lo ? llclamp((v - r.lo) / (r.hi - r.lo), 0.f, 1.f) : 0.f;
        const LLSD def = component(r.uniform.mDefault, r.comp);
        const F32 dv = def.isBoolean() ? (def.asBoolean() ? 1.f : 0.f) : (F32)def.asReal();
        p.changed = !r.uniform.mNoReset && def.isDefined() && std::fabs(v - dv) > 1e-5f;
        out.push_back(p);
    }
    return out;
}

void turnParam(size_t i, size_t p, S32 dir)
{
    std::string path;
    ParamRef r;
    if (dir == 0 || !refFor(i, p, path, r))
        return;
    const F32 v = currentValue(r);
    switch (r.kind)
    {
        case PARAM_TOGGLE: setValue(path, r, v != 0.f ? 0.0 : 1.0); break;
        case PARAM_CHOICE:
        {
            const S32 n = (S32)r.uniform.mItems.size();
            const S32 next = (((S32)std::lround(v) + dir) % n + n) % n;
            setValue(path, r, (F64)next);
            break;
        }
        default: setValue(path, r, (F64)v + (F64)r.step * dir); break;
    }
}

void setParamFraction(size_t i, size_t p, F32 fraction)
{
    std::string path;
    ParamRef r;
    if (!refFor(i, p, path, r))
        return;
    fraction = llclamp(fraction, 0.f, 1.f);
    if (r.kind == PARAM_TOGGLE)
        setValue(path, r, fraction > 0.5f ? 1.0 : 0.0);
    else if (r.kind == PARAM_SLIDER)
        setValue(path, r, (F64)r.lo + (F64)(r.hi - r.lo) * fraction);
}

void resetParam(size_t i, size_t p)
{
    std::string path;
    ParamRef r;
    if (!refFor(i, p, path, r) || r.uniform.mNoReset)
        return;
    if (llclamp(SLFxPresets::componentCount(r.uniform), 1, 4) == 1)
        SLFxStack::instance().resetUniform(path, r.uniform.mName);
    else
    {
        const LLSD def = component(r.uniform.mDefault, r.comp);
        if (def.isDefined())
            SLFxStack::instance().setUniform(path, r.uniform.mName, withComponent(r, def.asReal()));
    }
}

bool compare()
{
    return gSavedSettings.getBOOL(kCompareSetting);
}

void setCompare(bool on)
{
    gSavedSettings.setBOOL(kCompareSetting, on);
    pushSplit();
}

F32 split()
{
    return llclamp(gSavedSettings.getF32(kSplitSetting), 0.f, 1.f);
}

void setSplit(F32 x01)
{
    gSavedSettings.setF32(kSplitSetting, llclamp(x01, 0.02f, 0.98f));
    pushSplit();
}

void importLut()
{
    // Any file (the picker has no .cube filter): the extension is checked here.
    LLFilePickerReplyThread::startPicker(
        [](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (files.empty())
                return;
            std::string message;
            const bool ok = importLutFile(files.front(), message);
            if (ok)
                LL_INFOS("CameraFilters") << message << LL_ENDL;
            else
                LL_WARNS("CameraFilters") << "LUT import failed: " << message << LL_ENDL;
            SLCameraModel::toast(message, ok ? 3.0 : 7.0);
        },
        LLFilePicker::FFLOAD_ALL, false);
}

bool importLutFile(const std::string& path, std::string& message)
{
    LLStringUtil::format_map_t args;
    args["[FILE]"] = gDirUtilp->getBaseFileName(path, false);
    if (lower(gDirUtilp->getExtension(path)) != "cube")
    {
        message = str("LutNotCube", args);
        return false;
    }
    // The file's text (a 65^3 table is about 8 MB).
    std::string text;
    {
        llifstream in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open())
        {
            message = str("LutUnreadable", args);
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0 || (size_t)size > PrestigeCubeLut::kMaxFileBytes)
        {
            args["[ERROR]"] = size < 0 ? std::string("the file could not be read") : std::string("the file is too large for a LUT (more than 64 MB)");
            message = str("LutBad", args);
            return false;
        }
        in.seekg(0, std::ios::beg);
        text.resize((size_t)size);
        if (size > 0 && !in.read(&text[0], size))
        {
            message = str("LutUnreadable", args);
            return false;
        }
    }
    PrestigeCubeLut::Lut lut;
    std::string error;
    if (!PrestigeCubeLut::parse(text, lut, error))
    {
        args["[ERROR]"] = error;
        message = str("LutBad", args);
        return false;
    }
    text.clear();
    const int n = PrestigeCubeLut::stripSize(lut);
    std::vector<uint8_t> rgba;
    if (!PrestigeCubeLut::bakeStrip(lut, n, rgba))
    {
        message = str("LutNoMemory", args);
        return false;
    }

    // Where it goes: <effects>/Textures/PrestigeLUT/LUT_<name>.png and <effects>/Shaders/PrestigeLUT/LUT_<name>.fx.
    const std::string title = lut.title.empty() ? gDirUtilp->getBaseFileName(path, true) : lut.title;
    const std::string base = "LUT_" + safeName(title);
    const std::string& sep = gDirUtilp->getDirDelimiter();
    const std::string effects = SLFxStack::instance().getEffectsDir();
    const std::string shaders = effects + sep + "Shaders" + sep + "PrestigeLUT";
    const std::string textures = effects + sep + "Textures" + sep + "PrestigeLUT";
    for (const std::string& dir : { effects, effects + sep + "Shaders", shaders, effects + sep + "Textures", textures })
        LLFile::mkdir(dir);
    const std::string pngPath = textures + sep + base + ".png";
    const std::string fxPath = shaders + sep + base + ".fx";
    args["[NAME]"] = title;
    args["[FOLDER]"] = effects;

    // The strip, rows bottom-up as LLImageRaw keeps them (the PNG then reads top-down).
    {
        const S32 w = n * n;
        LLPointer<LLImageRaw> raw = new LLImageRaw((U16)w, (U16)n, 4);
        U8* dst = raw->getData();
        if (!dst || raw->isBufferInvalid())
        {
            message = str("LutNoMemory", args);
            return false;
        }
        for (S32 y = 0; y < n; ++y)
            std::memcpy(dst + (size_t)(n - 1 - y) * (size_t)w * 4, &rgba[(size_t)y * (size_t)w * 4], (size_t)w * 4);
        LLPointer<LLImagePNG> png = new LLImagePNG;
        if (!png->encode(raw, 0.f) || !png->save(pngPath))
        {
            message = str("LutWriteFailed", args);
            return false;
        }
    }
    // The effect: the bundled PrestigeLUT.fx with this strip.
    const bool existed = LLFile::isfile(fxPath);
    {
        llofstream out(fxPath.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out.is_open())
        {
            message = str("LutWriteFailed", args);
            return false;
        }
        out << "// " << base << ".fx: made by Prestige camera mode's \"Import LUT...\" from "
            << literalText(gDirUtilp->getBaseFileName(path, false)) << " ("
            << (lut.size3D ? llformat("3D, %d", lut.size3D) : llformat("1D, %d", lut.size1D)) << ").\n"
            << "// Delete it together with Textures/PrestigeLUT/" << base << ".png to remove the LUT.\n"
            << "#define PRESTIGE_LUT_FILE \"PrestigeLUT/" << base << ".png\"\n"
            << "#define PRESTIGE_LUT_SIZE " << n << "\n"
            << "#define PRESTIGE_LUT_LABEL \"LUT: " << literalText(title) << "\"\n"
            << "#include \"PrestigeLUT.fx\"\n";
        if (!out.good())
        {
            message = str("LutWriteFailed", args);
            return false;
        }
    }
    SLFxStack& fx = SLFxStack::instance();
    fx.rescan();               // finds the new effect
    if (existed)
        fx.reloadEffect(fxPath);   // the same LUT again: its strip changed under the compiled effect
    const std::string technique = "PrestigeLUT@" + base + ".fx";
    bool inStack = false;
    for (const Filter& f : load())
        inStack |= sameTechnique(f.technique, technique);
    if (inStack)
        message = str("LutUpdated", args);
    else if (addFilter(technique))
        message = str("LutImported", args);
    else
        message = str("LutImportedFull", args);
    if (lut.size3D == 0)
        message += " " + str("LutFrom1D");
    return true;
}
} // namespace PrestigeCameraFilters
