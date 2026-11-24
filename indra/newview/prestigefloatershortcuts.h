/**
 * @file prestigefloatershortcuts.h
 * @brief Help > Keyboard shortcuts: every key of the viewer in one list, grouped, with a
 *        filter box. Built from the live key bindings, not a typed list.
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

#ifndef PRESTIGE_FLOATERSHORTCUTS_H
#define PRESTIGE_FLOATERSHORTCUTS_H

#include "llfloater.h"

#include <map>
#include <string>
#include <vector>

class LLFilterEditor;
class LLKeyConflictHandler;
class LLScrollListCtrl;
class LLTextBox;
struct SLKeyCommand;

// Where the rows come from, read again each time the window opens (so a key changed in
// Preferences > Controls shows here at once):
//  - movement and camera keys: the Controls tables (control_table_contents_*.xml) with the
//    third-person bindings of key_bindings.xml (LLKeyConflictHandler), as Preferences shows them
//  - menu accelerators and the other named keys: SLKeyCommands, which reads every shortcut=
//    of the menu files and key_commands.xml and holds the user's changes
//  - the keys that cannot be changed: SLKeyCommands' fixed keys
//  - mouse gestures and held keys no table knows (Alt+click to focus, Shift+drag to copy, ...):
//    "extra_N" strings of floater_prestige_shortcuts.xml
class PrestigeFloaterShortcuts : public LLFloater
{
    LOG_CLASS(PrestigeFloaterShortcuts);
public:
    PrestigeFloaterShortcuts(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    enum ESection
    {
        SECTION_MOVEMENT = 0,
        SECTION_CAMERA,
        SECTION_BUILD,
        SECTION_CHAT,
        SECTION_WINDOWS,
        SECTION_CAMERA_MODE,
        SECTION_PRESTIGE,
        SECTION_FIXED,
        SECTION_COUNT
    };

    struct Entry
    {
        S32 mSection = SECTION_WINDOWS;
        std::string mKeys;
        std::string mAction;
        std::string mTip;
        std::string mSearch; // keys, action and section, lower case
    };

    void collect();
    void addControlTable(const std::string& filename, LLKeyConflictHandler& handler, S32 section);
    void addKeyCommands();
    void addExtras();
    void addEntry(S32 section, const std::string& keys, const std::string& action, const std::string& tip);
    S32 sectionOf(const SLKeyCommand& command) const;
    std::string sectionLabel(S32 section) const;
    void refreshList();
    void onChangeKeys();

    std::vector<Entry> mEntries;
    std::map<std::string, S32> mItemSections; // menu item name -> section, from "menu_items_<section>"
    std::string mFilterText;

    LLFilterEditor* mFilter = nullptr;
    LLScrollListCtrl* mList = nullptr;
    LLTextBox* mStatus = nullptr;
};

#endif // PRESTIGE_FLOATERSHORTCUTS_H
