/**
 * @file prestigefloatershortcuts.cpp
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

#include "llviewerprecompiledheaders.h"

#include "prestigefloatershortcuts.h"

#include "llfiltereditor.h"
#include "llfloaterpreference.h"
#include "llfloaterreg.h"
#include "llkeyconflict.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "lluictrlfactory.h"
#include "llxmlnode.h"
#include "slkeycommands.h"

namespace
{
// The "extra_N" strings name their section with one of these (in ESection order)
const char* const SECTION_TOKENS[] =
{
    "movement", "camera", "build", "chat", "windows", "camera_mode", "prestige", "fixed"
};

// The heading strings of floater_prestige_shortcuts.xml (in ESection order; the fixed keys
// use SLKeyCommands' own heading when it has one)
const char* const SECTION_STRINGS[] =
{
    "section_movement", "section_camera", "section_build", "section_chat", "section_windows",
    "section_camera_mode", "section_prestige", "section_fixed"
};

std::string lowered(std::string text)
{
    LLStringUtil::toLower(text);
    return text;
}

std::string trimmed(std::string text)
{
    LLStringUtil::trim(text);
    return text;
}

// "Preferences..." reads "Preferences" in a list of keys
std::string without_ellipsis(std::string text)
{
    LLStringUtil::replaceString(text, "...", "");
    return trimmed(text);
}

std::string node_name(const LLXMLNodePtr& node)
{
    return node->getName() ? std::string(node->getName()->mString) : std::string();
}

// The top-level menu an item of a menu file sits in ("BuildTools" for "BuildTools/Object/Link")
std::string top_menu(const std::string& menu_path)
{
    const size_t slash = menu_path.find('/');
    return slash == std::string::npos ? std::string() : menu_path.substr(0, slash);
}

std::string item_name(const std::string& menu_path)
{
    const size_t slash = menu_path.rfind('/');
    return slash == std::string::npos ? menu_path : menu_path.substr(slash + 1);
}
} // namespace

PrestigeFloaterShortcuts::PrestigeFloaterShortcuts(const LLSD& key)
:   LLFloater(key)
{
}

bool PrestigeFloaterShortcuts::postBuild()
{
    mFilter = getChild<LLFilterEditor>("filter");
    mList = getChild<LLScrollListCtrl>("shortcut_list");
    mStatus = getChild<LLTextBox>("status");

    mFilter->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        mFilterText = lowered(trimmed(mFilter->getText()));
        refreshList();
    });
    getChild<LLUICtrl>("change_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onChangeKeys(); });

    // Menu items listed under another heading than their menu's: Prestige features under
    // Prestige, Mouselook under Camera, Sit / stand under Movement, ...
    for (S32 section = 0; section < SECTION_COUNT; ++section)
    {
        const std::string list_name = std::string("menu_items_") + SECTION_TOKENS[section];
        if (!hasString(list_name))
        {
            continue;
        }
        std::vector<std::string> names;
        LLStringUtil::getTokens(getString(list_name), names, ",");
        for (const std::string& name : names)
        {
            const std::string clean = trimmed(name);
            if (!clean.empty())
            {
                mItemSections[clean] = section;
            }
        }
    }
    return true;
}

void PrestigeFloaterShortcuts::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    collect();
    refreshList();
    mFilter->setFocus(true);
}

void PrestigeFloaterShortcuts::collect()
{
    mEntries.clear();

    // The movement and camera keys as Preferences > Controls shows them in third person
    // (the mode most people walk and look around in)
    LLKeyConflictHandler bindings(LLKeyConflictHandler::MODE_THIRD_PERSON);
    addControlTable("control_table_contents_movement.xml", bindings, SECTION_MOVEMENT);
    addControlTable("control_table_contents_camera.xml", bindings, SECTION_CAMERA);
    addControlTable("control_table_contents_media.xml", bindings, SECTION_CHAT);

    addKeyCommands();
    addExtras();

    LL_DEBUGS("Shortcuts") << mEntries.size() << " shortcuts listed" << LL_ENDL;
}

void PrestigeFloaterShortcuts::addControlTable(const std::string& filename, LLKeyConflictHandler& handler, S32 section)
{
    LLXMLNodePtr root;
    if (!LLUICtrlFactory::getLayeredXMLNode(filename, root))
    {
        LL_WARNS("Shortcuts") << "Failed to load " << filename << LL_ENDL;
        return;
    }

    for (LLXMLNodePtr row = root->getFirstChild(); row.notNull(); row = row->getNextSibling())
    {
        if (node_name(row) != "rows")
        {
            continue;
        }
        std::string control;
        row->getAttributeString("value", control);
        if (control.empty())
        {
            continue; // a heading or a separator of the table
        }

        std::string label;
        std::string tip;
        for (LLXMLNodePtr column = row->getFirstChild(); column.notNull(); column = column->getNextSibling())
        {
            std::string column_name;
            column->getAttributeString("column", column_name);
            if (node_name(column) == "columns" && column_name == "lst_action")
            {
                column->getAttributeString("value", label);
                column->getAttributeString("tool_tip", tip);
            }
        }

        // Preferences shows three keys per action
        std::string keys;
        for (U32 index = 0; index < 3; ++index)
        {
            const std::string key = handler.getControlString(control, index);
            if (!key.empty())
            {
                keys += keys.empty() ? key : ", " + key;
            }
        }
        if (!keys.empty() && !label.empty())
        {
            addEntry(section, keys, label, tip);
        }
    }
}

void PrestigeFloaterShortcuts::addKeyCommands()
{
    SLKeyCommands& registry = SLKeyCommands::instance();
    for (const SLKeyCommand& command : registry.getCommands())
    {
        if (command.mScope == "login")
        {
            continue; // the login screen's menu is gone once logged in
        }
        if (command.mScope == "viewer")
        {
            const std::string top = top_menu(command.mMenuPath);
            if (top == "Develop" || top == "Admin" || top == "Deprecated")
            {
                continue; // developer and god-mode menus
            }
        }

        // The current key: the user's change, else the default; nothing when cleared
        const std::string keys = registry.getBindingString(command.mId);
        if (keys.empty())
        {
            continue;
        }

        // Menu bar items say where they are: "World > Snapshot (camera mode)"
        std::string action = command.mLabel;
        if (command.mScope == "viewer" && !command.mGroup.empty())
        {
            action = command.mGroup + " > " + action;
        }
        std::string tip;
        if (!command.mScope.empty() && command.mScope != "viewer")
        {
            tip = command.mGroup;
        }
        LLStringUtil::format(action, LLTrans::getDefaultArgs()); // "Exit [APP_NAME]"
        addEntry(sectionOf(command), keys, without_ellipsis(action), tip);
    }

    for (const SLFixedKey& fixed : registry.getFixedKeys())
    {
        addEntry(SECTION_FIXED, fixed.mKeys, fixed.mLabel, std::string());
    }
}

void PrestigeFloaterShortcuts::addExtras()
{
    // "section|keys|what it does": mouse gestures and held keys that no binding table holds
    for (S32 n = 1; hasString(llformat("extra_%d", n)); ++n)
    {
        std::vector<std::string> parts;
        LLStringUtil::getTokens(getString(llformat("extra_%d", n)), parts, "|");
        if (parts.size() < 3)
        {
            continue;
        }
        const std::string token = trimmed(parts[0]);
        S32 section = SECTION_WINDOWS;
        for (S32 i = 0; i < SECTION_COUNT; ++i)
        {
            if (token == SECTION_TOKENS[i])
            {
                section = i;
                break;
            }
        }
        addEntry(section, trimmed(parts[1]), trimmed(parts[2]), parts.size() > 3 ? trimmed(parts[3]) : std::string());
    }
}

void PrestigeFloaterShortcuts::addEntry(S32 section, const std::string& keys, const std::string& action, const std::string& tip)
{
    // One row for an action that two menus offer with the same key (Gestures in Avatar and Comm)
    for (const Entry& entry : mEntries)
    {
        if (entry.mSection == section && entry.mKeys == keys && entry.mAction == action)
        {
            return;
        }
    }
    Entry entry;
    entry.mSection = section;
    entry.mKeys = keys;
    entry.mAction = action;
    entry.mTip = tip;
    entry.mSearch = lowered(keys + "\n" + action + "\n" + tip + "\n" + sectionLabel(section));
    mEntries.push_back(entry);
}

S32 PrestigeFloaterShortcuts::sectionOf(const SLKeyCommand& command) const
{
    if (command.mScope.empty())
    {
        // keys of key_commands.xml, by where they work
        if (command.mContext == "camera_mode")
        {
            return SECTION_CAMERA_MODE;
        }
        if (command.mContext == "photo_studio" || command.mContext == "video_studio")
        {
            return SECTION_PRESTIGE;
        }
        if (command.mId.compare(0, 5, "chat_") == 0)
        {
            return SECTION_CHAT;
        }
        return SECTION_WINDOWS;
    }
    if (command.mScope == "script_editor")
    {
        return SECTION_BUILD;
    }
    if (command.mScope == "viewer")
    {
        const auto listed = mItemSections.find(item_name(command.mMenuPath));
        if (listed != mItemSections.end())
        {
            return listed->second;
        }
        const std::string top = top_menu(command.mMenuPath);
        if (top == "BuildTools")
        {
            return SECTION_BUILD;
        }
        if (top == "Communicate")
        {
            return SECTION_CHAT;
        }
    }
    return SECTION_WINDOWS;
}

std::string PrestigeFloaterShortcuts::sectionLabel(S32 section) const
{
    if (section == SECTION_FIXED)
    {
        const std::string& fixed = SLKeyCommands::instance().getFixedGroup();
        if (!fixed.empty())
        {
            return fixed;
        }
    }
    if (section < 0 || section >= SECTION_COUNT)
    {
        section = SECTION_WINDOWS;
    }
    return getString(SECTION_STRINGS[section]);
}

void PrestigeFloaterShortcuts::refreshList()
{
    if (!mList)
    {
        return;
    }
    // Every word typed must be found (in the keys, the action or the heading)
    std::vector<std::string> words;
    LLStringUtil::getTokens(mFilterText, words, " ");

    mList->deleteAllItems();
    S32 shown = 0;
    for (S32 section = 0; section < SECTION_COUNT; ++section)
    {
        bool heading = false;
        for (const Entry& entry : mEntries)
        {
            if (entry.mSection != section)
            {
                continue;
            }
            bool match = true;
            for (const std::string& word : words)
            {
                if (entry.mSearch.find(word) == std::string::npos)
                {
                    match = false;
                    break;
                }
            }
            if (!match)
            {
                continue;
            }

            if (!heading)
            {
                heading = true;
                LLSD row;
                LLSD& cols = row["columns"];
                cols[0]["column"] = "keys";
                cols[0]["value"] = sectionLabel(section);
                cols[0]["font"]["style"] = "BOLD";
                cols[1]["column"] = "action";
                cols[1]["value"] = "";
                if (LLScrollListItem* item = mList->addElement(row, ADD_BOTTOM))
                {
                    item->setEnabled(false);
                }
            }

            LLSD row;
            LLSD& cols = row["columns"];
            cols[0]["column"] = "keys";
            cols[0]["value"] = entry.mKeys;
            cols[1]["column"] = "action";
            cols[1]["value"] = entry.mAction;
            if (!entry.mTip.empty())
            {
                cols[1]["tool_tip"] = entry.mTip;
            }
            mList->addElement(row, ADD_BOTTOM);
            ++shown;
        }
    }

    LLStringUtil::format_map_t args;
    args["[COUNT]"] = llformat("%d", shown);
    if (words.empty())
    {
        mStatus->setText(getString("status_all", args));
    }
    else if (shown == 0)
    {
        mStatus->setText(getString("status_none"));
    }
    else
    {
        mStatus->setText(getString("status_some", args));
    }
}

void PrestigeFloaterShortcuts::onChangeKeys()
{
    // Preferences > Controls changes them; this list is read again when it opens
    if (LLFloaterPreference* prefs = dynamic_cast<LLFloaterPreference*>(LLFloaterReg::showInstance("preferences")))
    {
        prefs->selectPanel("controls");
    }
}
