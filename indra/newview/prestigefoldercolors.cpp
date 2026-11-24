/**
 * @file prestigefoldercolors.cpp
 * @brief Prestige folder colours: a colour of your own on any of your inventory
 *        folders, seen by you only, plus optional icon colours by item type.
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

#include "prestigefoldercolors.h"

#include "llbutton.h"
#include "llcolorswatch.h"
#include "lldir.h"
#include "llfile.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "lltextbox.h"
#include "lluicolortable.h"
#include "llviewerinventory.h"

#include <array>
#include <unordered_map>

namespace
{
    const std::string ACTION_PREFIX = "prestige_folder_color_";

    std::unordered_map<LLUUID, LLColor4> sColors;
    bool sLoaded = false;

    std::string colorsFile()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "prestige_folder_colors.xml");
    }

    // Reads the account's file once, the first time it is asked after login.
    // Before login (no account folder yet) it does nothing and reports false.
    bool ensureLoaded()
    {
        if (sLoaded)
        {
            return true;
        }
        if (gDirUtilp->getLindenUserDir().empty())
        {
            return false;
        }
        sLoaded = true;

        llifstream in(colorsFile().c_str());
        LLSD sd;
        if (!in.is_open() || LLSDSerialize::fromXML(sd, in) <= 0 || !sd.isMap())
        {
            return true;
        }
        for (const auto& [key, value] : llsd::inMap(sd))
        {
            LLUUID id;
            if (id.set(key, false) && id.notNull() && value.isArray() && value.size() >= 3)
            {
                LLColor4 color((F32)value[0].asReal(), (F32)value[1].asReal(), (F32)value[2].asReal(), 1.f);
                color.clamp();
                sColors[id] = color;
            }
        }
        LL_INFOS("PrestigeFolderColors") << "Loaded " << sColors.size() << " folder colours" << LL_ENDL;
        return true;
    }

    void saveColors()
    {
        if (!ensureLoaded())
        {
            return;
        }
        LLSD sd = LLSD::emptyMap();
        for (const auto& [id, color] : sColors)
        {
            LLSD rgb = LLSD::emptyArray();
            rgb.append((LLSD::Real)color.mV[VRED]);
            rgb.append((LLSD::Real)color.mV[VGREEN]);
            rgb.append((LLSD::Real)color.mV[VBLUE]);
            sd[id.asString()] = rgb;
        }
        llofstream out(colorsFile().c_str());
        if (out.is_open())
        {
            LLSDSerialize::toPrettyXML(sd, out);
        }
        else
        {
            LL_WARNS("PrestigeFolderColors") << "Cannot write " << colorsFile() << LL_ENDL;
        }
    }
}

//-----------------------------------------------------------------------------
// PrestigeFolderColors
//-----------------------------------------------------------------------------

// static
bool PrestigeFolderColors::getColor(const LLUUID& folder_id, LLColor4& color)
{
    if (!ensureLoaded() || sColors.empty())
    {
        return false;
    }
    auto it = sColors.find(folder_id);
    if (it == sColors.end())
    {
        return false;
    }
    color = it->second;
    return true;
}

// static
void PrestigeFolderColors::setColor(const LLUUID& folder_id, const LLColor4& color)
{
    if (folder_id.isNull() || !ensureLoaded())
    {
        return;
    }
    LLColor4 opaque(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 1.f);
    opaque.clamp();
    auto it = sColors.find(folder_id);
    if (it != sColors.end() && it->second == opaque)
    {
        return;
    }
    sColors[folder_id] = opaque;
    saveColors();
}

// static
void PrestigeFolderColors::clearColor(const LLUUID& folder_id)
{
    if (!ensureLoaded())
    {
        return;
    }
    if (sColors.erase(folder_id) > 0)
    {
        saveColors();
    }
}

// static
bool PrestigeFolderColors::getPresetColor(const std::string& preset, LLColor4& color)
{
    static const std::array<std::pair<const char*, const char*>, 8> presets = {{
        { "red",    "PrestigeFolderColorRed" },
        { "orange", "PrestigeFolderColorOrange" },
        { "yellow", "PrestigeFolderColorYellow" },
        { "green",  "PrestigeFolderColorGreen" },
        { "teal",   "PrestigeFolderColorTeal" },
        { "blue",   "PrestigeFolderColorBlue" },
        { "purple", "PrestigeFolderColorPurple" },
        { "pink",   "PrestigeFolderColorPink" },
    }};
    for (const auto& [name, color_name] : presets)
    {
        if (preset == name)
        {
            color = LLUIColorTable::instance().getColor(color_name, LLColor4::white).get();
            return true;
        }
    }
    return false;
}

// static
bool PrestigeFolderColors::getItemTypeColor(LLAssetType::EType type, LLColor4& color)
{
    enum EPalette
    {
        P_TEXTURE, P_SOUND, P_CALLINGCARD, P_LANDMARK, P_CLOTHING, P_OBJECT, P_NOTECARD,
        P_SCRIPT, P_BODYPART, P_ANIMATION, P_GESTURE, P_SETTINGS, P_MATERIAL, P_COUNT
    };
    // LLUIColor references follow the colour table, so a skin change is picked up.
    static const std::array<LLUIColor, P_COUNT> palette = []()
    {
        static const char* const names[P_COUNT] = {
            "PrestigeInvIconTexture", "PrestigeInvIconSound", "PrestigeInvIconCallingCard",
            "PrestigeInvIconLandmark", "PrestigeInvIconClothing", "PrestigeInvIconObject",
            "PrestigeInvIconNotecard", "PrestigeInvIconScript", "PrestigeInvIconBodyPart",
            "PrestigeInvIconAnimation", "PrestigeInvIconGesture", "PrestigeInvIconSettings",
            "PrestigeInvIconMaterial" };
        std::array<LLUIColor, P_COUNT> colors;
        for (size_t i = 0; i < colors.size(); ++i)
        {
            colors[i] = LLUIColorTable::instance().getColor(names[i], LLColor4::white);
        }
        return colors;
    }();

    EPalette entry = P_COUNT;
    switch (type)
    {
        case LLAssetType::AT_TEXTURE:     entry = P_TEXTURE; break;
        case LLAssetType::AT_SOUND:       entry = P_SOUND; break;
        case LLAssetType::AT_CALLINGCARD: entry = P_CALLINGCARD; break;
        case LLAssetType::AT_LANDMARK:    entry = P_LANDMARK; break;
        case LLAssetType::AT_CLOTHING:    entry = P_CLOTHING; break;
        case LLAssetType::AT_OBJECT:      entry = P_OBJECT; break;
        case LLAssetType::AT_NOTECARD:    entry = P_NOTECARD; break;
        case LLAssetType::AT_LSL_TEXT:    entry = P_SCRIPT; break;
        case LLAssetType::AT_BODYPART:    entry = P_BODYPART; break;
        case LLAssetType::AT_ANIMATION:   entry = P_ANIMATION; break;
        case LLAssetType::AT_GESTURE:     entry = P_GESTURE; break;
        case LLAssetType::AT_SETTINGS:    entry = P_SETTINGS; break;
        case LLAssetType::AT_MATERIAL:    entry = P_MATERIAL; break;
        default:                          break;
    }
    if (entry == P_COUNT)
    {
        return false;
    }
    color = palette[entry].get();
    return true;
}

// static
bool PrestigeFolderColors::isMenuAction(const std::string& action)
{
    return action.size() > ACTION_PREFIX.size() && action.compare(0, ACTION_PREFIX.size(), ACTION_PREFIX) == 0;
}

// static
void PrestigeFolderColors::doMenuAction(const std::string& action, const LLUUID& folder_id, const uuid_vec_t& selection)
{
    if (!isMenuAction(action) || folder_id.isNull())
    {
        return;
    }
    const std::string what = action.substr(ACTION_PREFIX.size());
    if (what == "clear")
    {
        clearColor(folder_id);
    }
    else if (what == "custom")
    {
        // The menu calls this once per selected folder; one window serves them all.
        if (!selection.empty() && selection.front() != folder_id)
        {
            return;
        }
        LLSD key = LLSD::emptyArray();
        for (const LLUUID& id : selection)
        {
            key.append(id);
        }
        if (key.size() == 0)
        {
            key.append(folder_id);
        }
        LLFloaterReg::showInstance("prestige_folder_color", key, true);
    }
    else
    {
        LLColor4 color;
        if (getPresetColor(what, color))
        {
            setColor(folder_id, color);
        }
    }
}

//-----------------------------------------------------------------------------
// PrestigeFloaterFolderColor
//-----------------------------------------------------------------------------

PrestigeFloaterFolderColor::PrestigeFloaterFolderColor(const LLSD& key)
    : LLFloater(key)
{
}

PrestigeFloaterFolderColor::~PrestigeFloaterFolderColor()
{
}

bool PrestigeFloaterFolderColor::postBuild()
{
    mSwatch = getChild<LLColorSwatchCtrl>("color_swatch");
    mNameText = getChild<LLTextBox>("folder_name");

    // live changes and the picker's OK both arrive as a commit; Cancel restores what was there
    mSwatch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSwatchCommit(); });
    mSwatch->setOnSelectCallback([this](LLUICtrl*, const LLSD&) { onSwatchCommit(); });
    mSwatch->setOnCancelCallback([this](LLUICtrl*, const LLSD&) { onSwatchCancel(); });

    getChild<LLButton>("clear_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClickClear(); });
    getChild<LLButton>("close_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    return true;
}

void PrestigeFloaterFolderColor::onOpen(const LLSD& key)
{
    mFolders.clear();
    mOriginal.clear();
    if (key.isArray())
    {
        for (const LLSD& entry : llsd::inArray(key))
        {
            const LLUUID id = entry.asUUID();
            if (id.notNull() && gInventory.getCategory(id))
            {
                mFolders.push_back(id);
            }
        }
    }
    else if (key.asUUID().notNull() && gInventory.getCategory(key.asUUID()))
    {
        mFolders.push_back(key.asUUID());
    }

    if (mFolders.empty())
    {
        closeFloater();
        return;
    }

    LLColor4 start;
    bool have_start = false;
    for (const LLUUID& id : mFolders)
    {
        LLColor4 color;
        const bool has_color = PrestigeFolderColors::getColor(id, color);
        mOriginal.emplace_back(id, std::make_pair(has_color, color));
        if (has_color && !have_start)
        {
            start = color;
            have_start = true;
        }
    }
    if (!have_start)
    {
        PrestigeFolderColors::getPresetColor("orange", start);
    }

    if (mFolders.size() == 1)
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(mFolders.front());
        mNameText->setText(cat ? cat->getName() : std::string());
    }
    else
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", (S32)mFolders.size());
        mNameText->setText(getString("multiple_folders", args));
    }

    // the swatch shows the start colour without committing it; the picker opens straight away
    mSwatch->set(start, true);
    mSwatch->setOriginal(start);
    mSwatch->showPicker(true);
}

void PrestigeFloaterFolderColor::onClose(bool /*app_quitting*/)
{
    // keep what was picked: close the picker without its Cancel
    if (mSwatch)
    {
        mSwatch->closeFloaterColorPicker();
    }
    mFolders.clear();
    mOriginal.clear();
}

void PrestigeFloaterFolderColor::onSwatchCommit()
{
    const LLColor4 color = mSwatch->get();
    for (const LLUUID& id : mFolders)
    {
        PrestigeFolderColors::setColor(id, color);
    }
}

void PrestigeFloaterFolderColor::onSwatchCancel()
{
    for (const auto& [id, original] : mOriginal)
    {
        if (original.first)
        {
            PrestigeFolderColors::setColor(id, original.second);
        }
        else
        {
            PrestigeFolderColors::clearColor(id);
        }
    }
}

void PrestigeFloaterFolderColor::onClickClear()
{
    for (const LLUUID& id : mFolders)
    {
        PrestigeFolderColors::clearColor(id);
    }
    // nothing for the picker's Cancel to restore now
    mOriginal.clear();
    closeFloater();
}
