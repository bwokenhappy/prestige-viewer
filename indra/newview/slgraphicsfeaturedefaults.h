// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (c) 2026 Slipstream Viewer contributors
/** Per-setting, one-time graphics defaults; no feature-table reset. */
#pragma once
#include <array>
#include <string>
#include <string_view>

class LLControlGroup;
class LLFeatureManager;
namespace SLGraphicsFeatureDefaults
{
inline constexpr std::array settingNames = {
    "RenderVKWaterCaustics",
    "RenderVKBlendMaxLights",
    "RenderVKBlendLayerReuse",
};
inline constexpr const char* markerName = "GraphicsFeatureDefaultsApplied";
static_assert(settingNames.size() <= 64);

enum class Action { Done, Preserve, Defer, Initialize };
constexpr Action action(bool done, bool saved, bool transient, bool atDefault) noexcept
{
    if (done) return Action::Done;
    if (saved) return Action::Preserve; // even a saved value equal to factory default
    if (transient) return Action::Defer;
    return atDefault ? Action::Initialize : Action::Preserve;
}
// Called for --set / session-file overrides, including overrides equal to default.
void noteOverride(std::string_view name) noexcept;
void noteOverrideFile(const std::string& filename) noexcept;
// After the real GPU masks and the usual first-run preset, before world pipelines.
void initialize(LLControlGroup& settings, LLFeatureManager& features, const std::string& filename) noexcept;
}
