/**
 * @file slfloaterquickprefs.cpp
 * @brief Quick Preferences: the hover height floater plus the settings people change
 *        mid-session (#1256: avatar count, object detail, name tags, sky and water)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slfloaterquickprefs.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slfloaterquickprefs.h"

#include <algorithm>
#include <set>
#include <vector>

#include "llcombobox.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llenvironment.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorysettings.h"
#include "llsettingsvo.h"
#include "lltrans.h"
#include "lluri.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"
#if defined(LL_HAS_CLEAN_PIPELINE)
#include "vkgfx/llvkneuralrecon.h"
#endif

namespace
{
    const std::string REGION_DEFAULT("region");
    // <Prestige> combo values of the bundled Windlight presets: prefix + display name
    const std::string BUNDLED_SKY("wlsky:");
    const std::string BUNDLED_WATER("wlwater:");

    std::string bundledDir(const std::string& subdir)
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "windlight", subdir);
    }

    // Windlight presets in app_settings/windlight/<subdir>: display name -> escaped file base
    std::map<std::string, std::string> scanBundled(const std::string& subdir)
    {
        std::map<std::string, std::string> found;
        const std::string dir = bundledDir(subdir);
        if (!gDirUtilp->fileExists(dir))
        {
            return found;
        }
        LLDirIterator iter(dir, "*.xml");
        std::string file;
        while (iter.next(file))
        {
            const std::string base = gDirUtilp->getBaseFileName(file, true);
            if (!base.empty())
            {
                found.emplace(LLURI::unescape(base), base);
            }
        }
        return found;
    }

    // After the inventory presets: a separator, then every bundled preset, alphabetically
    void addBundled(LLComboBox* combo, const std::map<std::string, std::string>& bundled, const std::string& prefix)
    {
        if (bundled.empty())
        {
            return;
        }
        std::vector<std::string> names;
        names.reserve(bundled.size());
        for (const auto& entry : bundled)
        {
            names.push_back(entry.first);
        }
        std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b)
            {
                return LLStringUtil::compareDict(a, b) < 0;
            });
        combo->addSeparator();
        for (const std::string& name : names)
        {
            combo->add(name, LLSD(prefix + name));
        }
    }
    // </Prestige>

    // Environment settings in the inventory and the Library, each asset once, not the ones
    // listed on the Marketplace
    class EnvironmentCollector : public LLInventoryCollectFunctor
    {
    public:
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            if (!item || item->getType() != LLAssetType::AT_SETTINGS
                || (mMarketplace.notNull() && gInventory.isObjectDescendentOf(item->getUUID(), mMarketplace)))
            {
                return false;
            }
            return mSeen.insert(item->getAssetUUID()).second;
        }

    private:
        const LLUUID mMarketplace = gInventory.findCategoryUUIDForType(LLFolderType::FT_MARKETPLACE_LISTINGS);
        std::set<LLUUID> mSeen;
    };

    using preset_list_t = std::vector<std::pair<std::string, LLUUID>>;

    void fillPresets(LLComboBox* combo, preset_list_t& presets, const std::string& region_label)
    {
        std::sort(presets.begin(), presets.end(), [](const auto& a, const auto& b)
            {
                return LLStringUtil::compareDict(a.first, b.first) < 0;
            });
        combo->removeall();
        combo->add(region_label, LLSD(REGION_DEFAULT));
        if (!presets.empty())
        {
            combo->addSeparator();
        }
        for (const auto& [name, asset_id] : presets)
        {
            combo->add(name, LLSD(asset_id));
        }
    }

    // The local environment's sky / water if it is one of the listed presets; "Region default"
    // without a local environment; for any other local one, nothing selected and other_label
    void selectPreset(LLComboBox* combo, bool local, const LLUUID& asset_id, const std::string& bundled_value,
                      const std::string& other_label)
    {
        if (!local)
        {
            combo->selectByValue(LLSD(REGION_DEFAULT));
        }
        else if (asset_id.notNull() && combo->selectByValue(LLSD(asset_id)))
        {
            // an inventory / Library preset
        }
        else if (asset_id.isNull() && !bundled_value.empty() && combo->selectByValue(LLSD(bundled_value)))
        {
            // <Prestige/> a bundled Windlight preset (built locally, so it has no asset)
        }
        else
        {
            combo->clear();
            combo->setLabel(other_label);
        }
    }
}

SLFloaterQuickPrefs::SLFloaterQuickPrefs(const LLSD& key)
:   LLFloaterHoverHeight(key)
{
}

bool SLFloaterQuickPrefs::postBuild()
{
    if (!LLFloaterHoverHeight::postBuild())
    {
        return false;
    }

    getChild<LLUICtrl>("qp_max_avatars")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onMaxAvatarsCommit(); });
    mUpscaleCombo = getChild<LLComboBox>("qp_upscale");
    mUpscaleToolTip = mUpscaleCombo->getToolTip();
    updateUpscalerAvailability();
    // Preferences > Graphics sets it too
    if (LLControlVariablePtr control = gSavedSettings.getControl("RenderAvatarMaxNonImpostors"))
    {
        mMaxAvatarsConn = control->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&)
            {
                updateMaxAvatarsText();
            });
    }

    mSkyCombo = getChild<LLComboBox>("qp_sky");
    mWaterCombo = getChild<LLComboBox>("qp_water");
    mSkyCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onEnvironmentCommit(mSkyCombo); });
    mWaterCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onEnvironmentCommit(mWaterCombo); });
    // also World > Environment, My Environments and a preset that finished loading
    mEnvChangedConn = LLEnvironment::instance().setEnvironmentChanged([this](LLEnvironment::EnvSelection_t, S32)
        {
            selectEnvironmentPresets();
        });

    return true;
}

void SLFloaterQuickPrefs::onOpen(const LLSD& key)
{
    LLFloaterHoverHeight::onOpen(key);
    updateMaxAvatarsText();
    updateUpscalerAvailability();
    // the inventory may have new presets since the last time
    loadEnvironmentPresets();
}

void SLFloaterQuickPrefs::updateUpscalerAvailability()
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    const bool dlssAvailable = LLVKNeuralRecon::instance().available();
    for (S32 mode = 7; mode <= 11; ++mode)
    {
        mUpscaleCombo->setEnabledByValue(LLSD(mode), dlssAvailable);
    }
    std::string tip = mUpscaleToolTip;
    if (!dlssAvailable)
    {
        LLStringUtil::format_map_t args;
        const std::string& reason = LLVKNeuralRecon::instance().reason();
        args["[REASON]"] = reason.empty() ? getString("dlss_not_loaded") : reason;
        tip += "\n\n" + getString("qp_dlss_unavailable", args);
    }
    mUpscaleCombo->setToolTip(tip);
#endif
}

void SLFloaterQuickPrefs::onMaxAvatarsCommit()
{
    // As Preferences > Graphics: IndirectMaxNonImpostors (the slider) at its far right is
    // unlimited (0)
    U32 value = getChild<LLUICtrl>("qp_max_avatars")->getValue().asInteger();
    if (value >= LLVOAvatar::NON_IMPOSTORS_MAX_SLIDER)
    {
        value = 0;
    }
    gSavedSettings.setU32("RenderAvatarMaxNonImpostors", value);
    LLVOAvatar::updateImpostorRendering(value);   // now, not at the next avatar update
}

void SLFloaterQuickPrefs::updateMaxAvatarsText()
{
    const U32 value = gSavedSettings.getU32("RenderAvatarMaxNonImpostors");
    getChild<LLUICtrl>("qp_max_avatars_text")->setValue(value == 0 ? LLTrans::getString("no_limit") : llformat("%u", value));
}

void SLFloaterQuickPrefs::loadEnvironmentPresets()
{
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    EnvironmentCollector collector;
    gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);
    gInventory.collectDescendentsIf(gInventory.getLibraryRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);

    preset_list_t skies;
    preset_list_t waters;
    for (const LLPointer<LLViewerInventoryItem>& item : items)
    {
        switch (LLSettingsType::fromInventoryFlags(item->getFlags()))
        {
            case LLSettingsType::ST_SKY:   skies.emplace_back(item->getName(), item->getAssetUUID());  break;
            case LLSettingsType::ST_WATER: waters.emplace_back(item->getName(), item->getAssetUUID()); break;
            default: break;
        }
    }

    const std::string region_label = getString("region_default");
    fillPresets(mSkyCombo, skies, region_label);
    fillPresets(mWaterCombo, waters, region_label);
    // <Prestige> bundled Windlight presets (scanned once; they ship with the viewer)
    if (!mBundledScanned)
    {
        mBundledSkies = scanBundled("skies");
        mBundledWaters = scanBundled("water");
        mBundledScanned = true;
    }
    addBundled(mSkyCombo, mBundledSkies, BUNDLED_SKY);
    addBundled(mWaterCombo, mBundledWaters, BUNDLED_WATER);
    // </Prestige>
    selectEnvironmentPresets();
}

void SLFloaterQuickPrefs::selectEnvironmentPresets()
{
    LLEnvironment& env = LLEnvironment::instance();
    const bool local = env.getSelectedEnvironment() == LLEnvironment::ENV_LOCAL && env.hasEnvironment(LLEnvironment::ENV_LOCAL);
    LLSettingsSky::ptr_t sky = local ? env.getEnvironmentFixedSky(LLEnvironment::ENV_LOCAL) : LLSettingsSky::ptr_t();
    LLSettingsWater::ptr_t water = local ? env.getEnvironmentFixedWater(LLEnvironment::ENV_LOCAL) : LLSettingsWater::ptr_t();
    const std::string other_label = getString("other_environment");
    selectPreset(mSkyCombo, local, sky ? sky->getAssetId() : LLUUID::null,
                 sky ? BUNDLED_SKY + sky->getName() : std::string(), other_label);
    selectPreset(mWaterCombo, local, water ? water->getAssetId() : LLUUID::null,
                 water ? BUNDLED_WATER + water->getName() : std::string(), other_label);
}

void SLFloaterQuickPrefs::onEnvironmentCommit(LLComboBox* combo)
{
    const LLSD value = combo->getValue();
    LLEnvironment& env = LLEnvironment::instance();
    if (value.asString() == REGION_DEFAULT)
    {
        // As World > Environment > Use Shared Environment: the whole local environment goes
        env.clearEnvironment(LLEnvironment::ENV_LOCAL);
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL, LLEnvironment::TRANSITION_INSTANT);
    }
    else if (value.isUUID() && value.asUUID().notNull())
    {
        // As My Environments' Apply Locally. A sky keeps the local water (or takes the one
        // showing), a water the local sky.
        env.setEnvironment(LLEnvironment::ENV_LOCAL, value.asUUID());
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
    }
    else if (value.isString())
    {
        // <Prestige> A bundled Windlight preset, read from app_settings/windlight and applied
        // as the local environment. As with the inventory presets, a sky keeps the water
        // showing and a water keeps the sky.
        const std::string selected = value.asString();
        const bool is_sky = selected.rfind(BUNDLED_SKY, 0) == 0;
        const bool is_water = !is_sky && selected.rfind(BUNDLED_WATER, 0) == 0;
        if (!is_sky && !is_water)
        {
            return;
        }
        const std::string name = selected.substr((is_sky ? BUNDLED_SKY : BUNDLED_WATER).size());
        const std::map<std::string, std::string>& bundled = is_sky ? mBundledSkies : mBundledWaters;
        const auto it = bundled.find(name);
        if (it == bundled.end())
        {
            return;
        }
        LLSD messages;
        if (is_sky)
        {
            LLSettingsSky::ptr_t sky = LLSettingsVOSky::buildFromLegacyPresetFile(it->second, bundledDir("skies"), messages);
            if (!sky)
            {
                LL_WARNS("Environment") << "Could not load Windlight sky \"" << name << "\"" << LL_ENDL;
                return;
            }
            env.setEnvironment(LLEnvironment::ENV_LOCAL, sky);
        }
        else
        {
            LLSettingsWater::ptr_t water = LLSettingsVOWater::buildFromLegacyPresetFile(it->second, bundledDir("water"), messages);
            if (!water)
            {
                LL_WARNS("Environment") << "Could not load Windlight water \"" << name << "\"" << LL_ENDL;
                return;
            }
            env.setEnvironment(LLEnvironment::ENV_LOCAL, water);
        }
        env.setSelectedEnvironment(LLEnvironment::ENV_LOCAL);
        env.updateEnvironment();
        // </Prestige>
    }
}
