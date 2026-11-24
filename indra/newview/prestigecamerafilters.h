/**
 * @file prestigecamerafilters.h
 * @brief Camera mode's live filter stack: up to six filters over the viewfinder, each with an
 *        on / off, a strength (0..100 %: the filter's result blended with its input) and its
 *        effect's own sliders, in an order the user drags.
 *
 * No second effects engine: a filter is a ReShade FX technique ("Name@File.fx") of the
 * effects stack (SLFxStack), run as its camera layer (SLFxStack::setLayer) after the user's
 * own Photo Studio effects, with the runtime's per-technique strength blend and the before /
 * after split (LLFxRuntimeVK). Any installed effect can be a filter -- the bundled pack, the
 * classic snapshot filters ported to FX (pack/Shaders/PrestigeLegacy), the user's own, and
 * LUTs imported from .cube files (prestigecubelut.h; each becomes a small effect over the
 * bundled pack/Shaders/PrestigeLUT/PrestigeLUT.fx in the user's effects folder).
 *
 * The layer runs only while camera mode is up (activate / deactivate). The list is remembered
 * (PrestigeCameraFilters); the sliders are the effects' own values (shared with Photo
 * Studio's Effects page, saved by SLFxStack). Snapshots get exactly the viewfinder's stack:
 * the effects run on the frames the shot is taken from (see LLFxRuntimeVK::pushCapture).
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

#ifndef PRESTIGE_CAMERAFILTERS_H
#define PRESTIGE_CAMERAFILTERS_H

#include "stdtypes.h"

#include <string>
#include <vector>

namespace PrestigeCameraFilters
{
constexpr size_t kMaxFilters = 6;

struct Filter
{
    std::string technique;   // "Name@File.fx"
    bool on = true;
    F32 strength = 1.f;      // 0..1
};

// ---- camera mode ----
void activate();     // the stack runs over the viewfinder (and the shots)
void deactivate();   // and stops
bool active();

// ---- the stack ----
std::vector<Filter> filters();
bool full();
// The filter's name as its effect labels it (ui_label), or a readable form of the technique.
std::string label(const std::string& technique);
// What keeps a filter from running, for its row: loading, an error, quarantined, not installed.
std::string status(size_t i);
// false when the stack is full or already has it (a toast says which).
bool add(const std::string& technique);
void remove(size_t i);
void move(size_t from, size_t to);
void setOn(size_t i, bool on);
void setStrength(size_t i, F32 strength);

// ---- what can be added (the picker) ----
enum Group { GROUP_CLASSIC = 0, GROUP_LUT = 1, GROUP_EFFECT = 2 };
struct Choice
{
    std::string technique, label;
    std::string note;        // the effect's file, for effects from packs (names repeat)
    S32 group = GROUP_EFFECT;
    bool added = false;
};
std::vector<Choice> choices();   // classic filters, then LUTs, then every other effect
bool loadingChoices();           // effects are still being read (more choices to come)

// ---- a filter's own sliders: its effect's uniforms with ui annotations ----
enum ParamKind { PARAM_SLIDER = 0, PARAM_TOGGLE = 1, PARAM_CHOICE = 2 };
struct Param
{
    std::string label, value;
    S32 kind = PARAM_SLIDER;
    F32 fraction = 0.f;         // a slider's position
    bool on = false;            // a toggle's state
    bool changed = false;       // not at the effect's default
};
std::vector<Param> params(size_t i);
void turnParam(size_t i, size_t p, S32 dir);           // a step / the next choice / flip
void setParamFraction(size_t i, size_t p, F32 fraction);
void resetParam(size_t i, size_t p);

// ---- before / after ----
bool compare();
void setCompare(bool on);
F32 split();                // 0..1 of the window's width
void setSplit(F32 x01);

// ---- LUTs ----
// "Import LUT...": a file picker for .cube files; the LUT is converted and added to the stack.
void importLut();
// The same for one file. True when it was imported; `message` says what happened (or why not).
bool importLutFile(const std::string& path, std::string& message);
} // namespace PrestigeCameraFilters

#endif
