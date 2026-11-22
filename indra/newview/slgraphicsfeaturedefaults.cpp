// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (c) 2026 Slipstream Viewer contributors
#include "llviewerprecompiledheaders.h"
#include "slgraphicsfeaturedefaults.h"
#include "llcontrol.h"
#include "llfeaturemanager.h"
#include "llsdserialize.h"
#include <algorithm>
#include <bitset>
#include <utility>
#include <cerrno>
#include <cmath>
#include <limits>
#include <new>
#include <sstream>

namespace SLGraphicsFeatureDefaults
{
namespace
{
std::bitset<settingNames.size()> overrides;
bool unreadableOverride = false;
// The ordinary loader has already read this file. This bounded second read records
// key presence (LLControl collapses an explicitly saved factory value to isDefault).
constexpr size_t fileLimit = 16u * 1024u * 1024u;
bool readSaved(const std::string& filename, LLSD& saved)
{
    if (filename.empty()) return false;
    llstat statbuf;
    if (LLFile::stat(filename, &statbuf) != 0)
    {
        if (errno != ENOENT) return false;
        saved = LLSD::emptyMap(); // first install
        return true;
    }
    llifstream file;
    file.open(filename.c_str(), std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto length = file.tellg();
    if (length <= 0 || length > static_cast<std::streamoff>(fileLimit)) return false;
    const size_t bytes = static_cast<size_t>(length);
    std::string text(bytes + 1u, '\0');
    file.seekg(0);
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (file.bad() || file.gcount() != static_cast<std::streamsize>(bytes)) return false;
    text.resize(bytes);
    std::istringstream input(std::move(text));
    return LLSDSerialize::fromXML(saved, input, false) != LLSDParser::PARSE_FAILURE && saved.isMap();
}
bool completed(const LLSD& done, const char* name)
{
    return done.has(name) && done[name].asBoolean();
}
bool validMarker(const LLSD& done)
{
    if (!done.isMap() || done.size() > 64) return false;
    for (auto i = done.beginMap(); i != done.endMap(); ++i)
        if (i->first.size() > 128u || !i->second.isBoolean()) return false;
    return true;
}
bool numericValue(LLControlVariable& control, F32 value, LLSD& output)
{
    if (!std::isfinite(value)) return false;
    if (control.isType(TYPE_BOOLEAN)) output = LLSD(value != 0.f);
    else if (control.isType(TYPE_F32)) output = LLSD(static_cast<F64>(value));
    else if (control.isType(TYPE_S32) || control.isType(TYPE_U32))
    {
        const double floorValue = control.isType(TYPE_U32) ? 0. : std::numeric_limits<S32>::lowest();
        if (value < floorValue || static_cast<double>(value) > std::numeric_limits<S32>::max()) return false;
        output = LLSD(static_cast<LLSD::Integer>(value));
    }
    else return false;
    return true;
}
}

void noteOverride(std::string_view name) noexcept
{
    for (size_t i = 0; i < settingNames.size(); ++i)
        if (name == settingNames[i]) overrides.set(i);
}
void noteOverrideFile(const std::string& filename) noexcept
{
    try
    {
        LLSD file;
        if (!readSaved(filename, file)) { unreadableOverride = true; return; }
        for (const char* name : settingNames)
            if (file.has(name)) noteOverride(name);
    }
    catch (const std::bad_alloc&) { unreadableOverride = true; }
}

void initialize(LLControlGroup& settings, LLFeatureManager& features, const std::string& filename) noexcept
{
    try
    {
        if (unreadableOverride) return; // cannot safely infer that a transient override is absent
        LLControlVariable* marker = settings.getControl(markerName);
        if (!marker) return;
        LLSD done = marker->getSaveValue();
        if (!validMarker(done)) return;
        bool pending = false;
        for (const char* name : settingNames)
            pending = pending || !completed(done, name);
        if (!pending)
        {
            // Keep even factory-equal choices in subsequent settings saves. Otherwise
            // a later factory-default change could silently erase this one-time choice.
            for (const char* name : settingNames)
                if (auto* control = settings.getControl(name).get())
                    control->setPersist(LLControlVariable::PERSIST_ALWAYS);
            return; // no settings-file read once all registered settings are done
        }
        LLSD saved;
        if (!readSaved(filename, saved))
        {
            LL_WARNS("RenderInit") << "New graphics defaults deferred: settings file is unreadable, malformed or over 16 MiB" << LL_ENDL;
            return;
        }
        U32 level = settings.getU32("RenderQualityPerformance");
        if (!features.isValidGraphicsLevel(level))
            level = static_cast<U32>(std::clamp(static_cast<S32>(features.getGPUClass()), 0,
                                               static_cast<S32>(features.getMaxGraphicsLevel())));
        LLFeatureList desired("new graphics defaults"), levelMask("new graphics quality");
        auto* quality = features.findMask(features.getNameForGraphicsLevel(level));
        if (!quality) return;
        // Copy just the registered entries; use the real feature masking rule,
        // including current GPU/vendor/availability limits, without changing mFeatures.
        for (const char* name : settingNames)
        {
            auto base = features.getFeatures().find(name);
            if (base == features.getFeatures().end()) continue;
            desired.addFeature(name, base->second.mAvailable, base->second.mRecommendedLevel);
            auto tier = quality->getFeatures().find(name);
            if (tier != quality->getFeatures().end())
                levelMask.addFeature(name, tier->second.mAvailable, tier->second.mRecommendedLevel);
        }
        desired.maskList(levelMask);
        U32 initialized = 0, preserved = 0;
        for (size_t i = 0; i < settingNames.size(); ++i)
        {
            const char* name = settingNames[i];
            auto* control = settings.getControl(name).get();
            if (!control || !control->isPersisted()) continue;
            const Action choice = action(completed(done, name), saved.has(name) && std::as_const(saved)[name].has("Value"),
                                         overrides.test(i) || control->hasUnsavedValue(), control->isDefault());
            if (choice == Action::Defer) continue; // temporary --set is not a permanent preference
            if (choice == Action::Done) { control->setPersist(LLControlVariable::PERSIST_ALWAYS); continue; }
            if (!done.has(name) && done.size() >= 64) continue;
            LLSD next = done;
            next[name] = true; // allocate the small bounded marker before any value change
            if (choice == Action::Initialize)
            {
                if (!desired.getFeatures().count(name)) continue;
                LLSD value;
                if (!numericValue(*control, desired.getRecommendedValue(name), value)) continue;
                control->setValue(value);
                if (!llsd_equals(control->getSaveValue(), value)) continue; // validation refused it
                ++initialized;
            }
            else ++preserved;
            marker->setValue(next);
            control->setPersist(LLControlVariable::PERSIST_ALWAYS);
            done = next;
        }
        if (initialized || preserved)
            LL_INFOS("RenderInit") << "New graphics defaults: initialized " << initialized << ", preserved " << preserved
                                  << ", quality level " << level << " (no graphics preset reset)" << LL_ENDL;
    }
    catch (const std::bad_alloc&) {} // no startup crash; incomplete work can retry next launch
}
}
