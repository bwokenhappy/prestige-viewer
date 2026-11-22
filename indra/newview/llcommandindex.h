// #703 The viewer's command index: everything the user can run or reach, searchable by name.
//
// UI-independent (no RmlUi, no widgets of its own): the new interface's command surface uses it,
// toolbars will be fed from it (every entry has a stable id and an icon to pin), and the classic
// layout emulated on the new widget kit later will use it too. Sources:
//  - the main menu tree (gMenuBarView): every visible call / check item, with its menu path, key
//    binding and its enabled / checked state at the rebuild (its own callbacks, as opening the menu
//    refreshes them); run = the item's own commit (same callbacks as a click). The Advanced, Develop,
//    Admin and RLVa menus are tagged (the surface shows them as such) and rank after the others;
//    they are indexed while the classic viewer shows them (their own toggles: Ctrl+Alt+D,
//    Ctrl+Alt+Q). Advanced > Shortcuts, a menu the classic viewer never shows (its items are there
//    for their keys), is indexed too;
//  - commands the new interface registers in code (addCommand: its own bars' toggles);
//  - the floater registry: every registered floater; run = toggle / bring to front;
//  - the floaters' structure, read from their (localised) XUI files once, a few files per frame,
//    without building any floater: every tab / panel / section ("Preferences › Graphics") and
//    every control bound to a setting ("Preferences › Graphics › Shadows › Sun shadow quality"),
//    and every choice of a combo box / radio group / flyout as its own entry ("Antialiasing: TAA");
//    tooltips and the acronyms in labels / tooltips are searchable too;
//    run = open the floater, select its tab chain, scroll to the control and flash it;
//  - the preferences search: "Search settings for ..." (the fallback row).
// Fixed cost and bounded size: at most kMaxEntries entries, at most kFilesPerTick XUI files read
// per tick until the structure is complete; a query scores every entry once and keeps the best.
#pragma once

#include "llhandle.h"
#include <deque>
#include <functional>
#include <string>
#include <vector>

class LLView;
class LLXMLNode;

class LLCommandIndex
{
public:
    enum class Kind : unsigned char
    {
        Menu,
        Window,
        Section, // a tab / panel / accordion section inside a floater
        Setting, // a control bound to a setting
        Settings, // the "Search settings for ..." fallback
        Command   // registered in code (addCommand)
    };
    struct Entry
    {
        Kind                     kind = Kind::Menu;
        std::string              id;       // stable across sessions: "menu:<path>/<label>", "window:<name>",
                                           // "section:<floater>/<tab chain>", "setting:<floater>/<control>"
        std::string              icon;     // a skin image name for pinning (toolbars)
        std::string              label;    // "Sun shadow quality"
        std::string              path;     // breadcrumb: "Preferences › Graphics › Shadows"
        std::string              shortcut; // "Ctrl+Shift+S" (menu items)
        std::string              target;   // floater name (Window, Section, Setting) or search text (Settings)
        std::vector<std::string> chain;    // tab / panel names to select, outermost first
        std::string              control;  // the control's name (Setting), to scroll to and flash
        std::string              setting;  // its control_name (Setting): matched last in ranking
        std::string              foldedLabel, foldedPath, foldedSetting;
        std::string              foldedAlias; // acronyms in its label / tooltip ("taa fsr"), space separated
        std::string              foldedTip;   // its tooltip (searchable, ranked below labels; bounded)
        LLHandle<LLView>         item;     // the menu item (Menu)
        std::string              tag;      // a category the surface shows instead of the kind ("Develop")
        bool                     checkable = false, checked = false, enabled = true; // at the rebuild
        std::function<void()>    action;    // Command: what it runs
        std::function<bool()>    isChecked; // Command: its state (null: not a toggle)
    };
    static constexpr size_t kMaxEntries   = 16384;
    static constexpr int    kFilesPerTick = 3;

    // The menu tree and the registry (cheap; when the surface opens). Structure entries kept.
    void rebuild();
    // Reads up to kFilesPerTick more floater XUI files; true while not complete.
    bool tick();
    // Best matches of `query`: an exact label first, then all words in the label, then in the
    // label and path, then in the setting name. At most `max`.
    void query(const std::string& query, size_t max, std::vector<const Entry*>& out) const;
    static Entry settingsSearch(const std::string& query);
    // Commands of the new interface's own (kind Command: id, label, path, shortcut, action and
    // isChecked set by the caller). Replaces an entry of the same id; bounded (kMaxCommands).
    static void addCommand(Entry entry);
    static void removeCommands(const std::string& idPrefix);
    static constexpr size_t kMaxCommands = 64;
    // Runs / navigates to it. False when it is gone or disabled now.
    static bool run(const Entry& entry);
    // Ends flash highlights whose time is up (call once per frame).
    static void tickHighlights();
    size_t size() const { return mMenu.size() + mStructure.size(); }
    size_t structureSize() const { return mStructure.size(); }
    bool   structureComplete() const { return mStarted && mTasks.empty(); }

private:
    struct Task
    {
        std::string              floater, file, title;
        std::vector<std::string> path, chain;
        int                      depth = 0;
    };
    void walk(const Task& task, LLXMLNode* node, std::vector<std::string>& path, std::vector<std::string>& chain, int depth);
    void addStructure(Entry&& e);

    std::vector<Entry> mMenu;      // menus + windows (rebuilt on open)
    std::vector<Entry> mStructure; // sections + settings (built once, incrementally)
    std::deque<Task>   mTasks;
    bool               mStarted = false;
};
