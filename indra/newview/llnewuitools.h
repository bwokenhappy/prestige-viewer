// #703 The new interface's tool grid: the toolbar commands (LLCommandManager, commands.xml) as an
// icon grid at the bottom centre of the window, behind a pull-up tab.
//
// Which commands: the ones on the person's classic toolbars (bottom, left, right, top, in that
// order), so their toolbar setup carries over; none (hidden) when those are empty. Pinned
// inventory items stay on the classic toolbars (their handlers need the classic button). Each button runs its command exactly as a classic toolbar button does: the command's
// execute function through the UI callback registry (only while its enable function allows it),
// press / release for commands with a stop function (push to talk), and its running state lit.
//
// When it shows: in world (not before login) with the new interface (UIRenderer 2), whose bar replaces the classic toolbars
// (they are hidden while it runs, so the two never show together). Camera mode hides it with the
// rest of the interface (the RmlUi host suspends every other document).
//
// Pull-up: a small tab flush with the bottom edge (above the bar when the bar is at the bottom);
// the pointer on it slides the grid up, and the grid goes again 300 ms after the pointer leaves it
// (no flicker crossing the gap). Its grip locks it open (UINewToolGridLocked) and unlocks it. A held
// tool (push to talk) keeps it open; a grid hidden while held lets the tool go. The Conversations,
// People and Inventory buttons open the native windows while those are on (UINewWindowsDev).
#pragma once

#include <functional>

namespace Rml
{
class Context;
class ElementDocument;
}

class LLNewUITools
{
public:
    using DocumentLoader = std::function<Rml::ElementDocument*(const char* rml, const char* name)>;

    static LLNewUITools& instance();
    bool start(Rml::Context* context, const DocumentLoader& load);
    void stop();
    bool active() const;
    // `bottom` / `tabBottom`: the grid's and the tab's distance from the window's bottom edge, in
    // UI units; `density`: pixels per UI unit. True when the document changed.
    bool sync(int bottom, int tabBottom, float density);
    bool ownsDocument(const Rml::ElementDocument* doc) const;
    // Any mouse button released anywhere: a held command (push to talk) is let go.
    void releaseHeld();
    bool holding() const;
    // Opens the grid as if hovered (the NewUI event API's "peek": tests without a pointer).
    void setPeek(bool on);
    // The document again under new tokens (a theme switch); its model and state stay.
    bool reload(const DocumentLoader& load);
};
