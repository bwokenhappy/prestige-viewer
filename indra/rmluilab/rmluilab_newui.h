// #703 slrmluilab --newui: the new interface's documents (llnewuidocs.h) over sample data, for
// review renders without the viewer (see rmluilab_newui.cpp).
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Rml
{
class Context;
class ElementDocument;
}

struct LabScene
{
    std::string              name, world; // world: checker | light | dark
    std::vector<std::string> rmls;        // themed documents, shown together
    // After the documents load and before the first frame: sample state, placement.
    std::function<void(Rml::Context*, const std::vector<Rml::ElementDocument*>&)> prepare;
};

// The llimage() decorator's stand-in: "llimage(<name> [solid])" draws <icons>/<name>.png (solid,
// tinted by image-color) or <icons>/<name>.color.png (scale). Register after Rml::Initialise.
void labRegisterImageDecorator(const std::string& icons);
// The sample data models (bar, palette, tools, chat, people, inv) on `context`, and the scenes: every
// document of the new interface in both themes over light and dark worlds.
std::vector<LabScene> labNewUIScenes(Rml::Context* context);
// The status bar's scenes (rmluilab_newui_status.cpp): its models ("status", "smenu") bound on
// `context`, the bar in both themes, its warning states, its menus.
std::vector<LabScene> labNewUIStatusScenes(Rml::Context* context);

// CPU fixture: exercise retained row/markup DOM on one bounded synthetic append.
void labAppendSyntheticChatMessage();
// #1668 Video Studio's overlay (newview/slvideostudiodoc.h) over sample data: one scene per tab with the
// transport bar, a recording, and the Track tab in the light theme (rmluilab_studio.cpp).
std::vector<LabScene> labVideoStudioScenes(Rml::Context* context);
