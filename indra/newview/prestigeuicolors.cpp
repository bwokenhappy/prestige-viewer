/**
 * @file prestigeuicolors.cpp
 * @brief Prestige "Your colours": accent, window background and text colours of the
 *        interface, plus the high contrast colour set of the Accessibility page.
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

#include "prestigeuicolors.h"

#include "llbutton.h"
#include "llcolorswatch.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"

#include <array>

static LLPanelInjector<PrestigePanelYourColors> t_prestige_your_colors("prestige_panel_your_colors");

namespace
{
// What a colour does in the skin. The Prestige palette (top of the skin's colors.xml) has one
// entry per role; the role colours below reference it (resolved by tools that read colors.xml).
enum ERole
{
    ROLE_ACCENT,        // SlipstreamBlue: focus outlines, links, highlights, hover/pressed tints
    ROLE_ACCENT_LIGHT,  // SlipstreamPurple: emphasis text, mentions, speaking indicator
    ROLE_ACCENT_DEEP,   // SlipstreamPurpleDeep: fills under light text (default button, flashes)
    ROLE_PANEL,         // SlipstreamPanel: window and panel backgrounds
    ROLE_DEEP,          // SlipstreamBgDeep: focused text fields, lists, console
    ROLE_INPUT,         // SlipstreamInput: editable text fields
    ROLE_RAISED,        // SlipstreamRaised: accordion headers, alert boxes
    ROLE_LINE,          // SlipstreamLine: borders, slider tracks
    ROLE_BAR,           // SlipstreamBar: menu bar, focused title bar
    ROLE_SELECT,        // SlipstreamBlueDeep: selected list rows and text
    ROLE_TEXT,          // SlipstreamText: labels, text, menu items
    ROLE_TEXT_DIM,      // SlipstreamTextDim: secondary text, links in inventory
    ROLE_TEXT_MUTED,    // SlipstreamTextMuted: disabled labels, system chat
    ROLE_COUNT
};

struct RoleColor
{
    const char* name;
    ERole       role;
};

// Palette entries and the interface colours that reference them in the Prestige skins.
// Colours drawn in the world (selection silhouettes, beacons, name tags, build grid), the pie
// menu override colours (Preferences > Skins) and the chat colours of Preferences > Colors
// (links, system chat) are left alone: those have swatches of their own.
const RoleColor sRoleColors[] =
{
    // accent
    { "SlipstreamBlue", ROLE_ACCENT }, { "SlipstreamBlue_13", ROLE_ACCENT }, { "SlipstreamBlue_35", ROLE_ACCENT },
    { "ALMediaTickerOscillatorColor", ROLE_ACCENT }, { "AlBlue", ROLE_ACCENT }, { "AlInformationBlue", ROLE_ACCENT },
    { "AlchemyHighlight", ROLE_ACCENT },
    { "ColumnHeaderFlatSortArrowColor", ROLE_ACCENT }, { "FPSTextColor", ROLE_ACCENT }, { "FilterTextColor", ROLE_ACCENT },
    { "FocusColor", ROLE_ACCENT }, { "InventoryFocusOutlineColor", ROLE_ACCENT },
    { "LoginProgressBarFgColor", ROLE_ACCENT }, { "MeshImportTableHighlightColor", ROLE_ACCENT },
    { "MultiSliderThumbCenterSelectedColor", ROLE_ACCENT }, { "MusicTickerEmphasisColor", ROLE_ACCENT },
    { "ColumnHeaderFlatHoverColor", ROLE_ACCENT }, { "ScrollHoveredColor", ROLE_ACCENT },
    { "ColumnHeaderFlatPressedColor", ROLE_ACCENT }, { "MenuItemHighlightBgColor", ROLE_ACCENT },
    // lighter accent
    { "SlipstreamPurple", ROLE_ACCENT_LIGHT }, { "SlipstreamPurple_13", ROLE_ACCENT_LIGHT },
    { "SlipstreamPurple_15", ROLE_ACCENT_LIGHT }, { "SlipstreamPurple_35", ROLE_ACCENT_LIGHT },
    { "SlipstreamPurple_50", ROLE_ACCENT_LIGHT }, { "AlchemyOrange", ROLE_ACCENT_LIGHT },
    { "AvatarListItemChatRange", ROLE_ACCENT_LIGHT }, { "AvatarListItemSameParcel", ROLE_ACCENT_LIGHT },
    { "AvatarListItemTyping", ROLE_ACCENT_LIGHT }, { "ChatMentionFont", ROLE_ACCENT_LIGHT },
    { "ChicletFlashColor", ROLE_ACCENT_LIGHT }, { "EmphasisColor", ROLE_ACCENT_LIGHT },
    { "InventoryItemSuffixColor", ROLE_ACCENT_LIGHT }, { "InventorySearchStatusColor", ROLE_ACCENT_LIGHT },
    { "SpeakingColor", ROLE_ACCENT_LIGHT }, { "TextEmbeddedItemColor", ROLE_ACCENT_LIGHT },
    { "EmphasisColor_13", ROLE_ACCENT_LIGHT }, { "ChatMentionHighlight", ROLE_ACCENT_LIGHT },
    { "ChatSelfMentionHighlight", ROLE_ACCENT_LIGHT }, { "EmphasisColor_35", ROLE_ACCENT_LIGHT },
    { "MentionFlashBgColor", ROLE_ACCENT_LIGHT },
    // deeper accent
    { "SlipstreamPurpleDeep", ROLE_ACCENT_DEEP }, { "AlGrape", ROLE_ACCENT_DEEP }, { "AlMiscPurple", ROLE_ACCENT_DEEP },
    { "ButtonDefaultImageColor", ROLE_ACCENT_DEEP }, { "ButtonFlashBgColor", ROLE_ACCENT_DEEP },
    { "MenuItemFlashBgColor", ROLE_ACCENT_DEEP },
    // window background
    { "SlipstreamPanel", ROLE_PANEL }, { "SlipstreamPanel_33", ROLE_PANEL }, { "SlipstreamPanel_66", ROLE_PANEL },
    { "AlCoalBlack", ROLE_PANEL }, { "AlchemyBackground", ROLE_PANEL }, { "ColumnHeaderFlatBgColor", ROLE_PANEL },
    { "DkGray", ROLE_PANEL }, { "DkGray0", ROLE_PANEL }, { "DkGray2", ROLE_PANEL }, { "DkGray_33", ROLE_PANEL },
    { "DkGray_66", ROLE_PANEL }, { "FloaterDefaultBackgroundColor", ROLE_PANEL },
    { "FloaterFocusBackgroundColor", ROLE_PANEL }, { "IMHistoryBgColor", ROLE_PANEL },
    { "InventoryBackgroundColor", ROLE_PANEL }, { "LoginProgressBarBgColor", ROLE_PANEL },
    { "MenuDefaultBgColor", ROLE_PANEL }, { "MenuPopupBgColor", ROLE_PANEL },
    { "PanelDefaultBackgroundColor", ROLE_PANEL }, { "PanelFocusBackgroundColor", ROLE_PANEL },
    { "PanelNotificationBackground", ROLE_PANEL }, { "ScrollBgReadOnlyColor", ROLE_PANEL },
    { "TextField_Disabled", ROLE_PANEL },
    { "SlipstreamBgDeep", ROLE_DEEP }, { "SlipstreamBgDeep_60", ROLE_DEEP }, { "AlchemyBlack1", ROLE_DEEP },
    { "AlchemyBlack1_t", ROLE_DEEP }, { "AlchemyScriptBG", ROLE_DEEP }, { "ColorSwatchBorderColor", ROLE_DEEP },
    { "ComboListBgColor", ROLE_DEEP }, { "ConsoleBackground", ROLE_DEEP }, { "DefaultHighlightLight", ROLE_DEEP },
    { "TextBgFocusColor", ROLE_DEEP }, { "TextBgReadOnlyColor", ROLE_DEEP }, { "TextField_Active", ROLE_DEEP },
    { "SlipstreamInput", ROLE_INPUT }, { "TextBgWriteableColor", ROLE_INPUT }, { "TextField_Off", ROLE_INPUT },
    { "SlipstreamRaised", ROLE_RAISED }, { "AccordionHeaderBgColor", ROLE_RAISED }, { "AlertBoxColor", ROLE_RAISED },
    { "MouseGray", ROLE_RAISED },
    { "SlipstreamLine", ROLE_LINE }, { "AlAshGrey", ROLE_LINE }, { "DefaultHighlightDark", ROLE_LINE },
    { "MusicTickerGridColor", ROLE_LINE }, { "ResizebarBorderLight", ROLE_LINE }, { "SliderTrackColor", ROLE_LINE },
    { "SlipstreamBar", ROLE_BAR }, { "MenuBarBgColor", ROLE_BAR }, { "MenuBarBetaBgColor", ROLE_BAR },
    { "MenuBarProjectBgColor", ROLE_BAR }, { "MenuBarTestBgColor", ROLE_BAR }, { "TitleBarFocusColor", ROLE_BAR },
    { "SlipstreamBlueDeep", ROLE_SELECT }, { "AlDarkBlue", ROLE_SELECT }, { "AlPrimaryBlue", ROLE_SELECT },
    { "ChatTeleportSeparatorColor", ROLE_SELECT }, { "ScrollSelectedBGColor", ROLE_SELECT },
    { "SysWellItemSelected", ROLE_SELECT }, { "TextBgHighlightColor", ROLE_SELECT },
    // text
    { "SlipstreamText", ROLE_TEXT }, { "AccordionHeaderTextColor", ROLE_TEXT },
    { "AlchemyFancyChatDividerColor", ROLE_TEXT }, { "AlchemyText", ROLE_TEXT }, { "AlchemyWhite", ROLE_TEXT },
    { "AvatarListItemOnlineColor", ROLE_TEXT }, { "BadgeLabelColor", ROLE_TEXT }, { "ButtonLabelColor", ROLE_TEXT },
    { "ButtonLabelSelectedColor", ROLE_TEXT }, { "ChatHeaderDisplayNameColor", ROLE_TEXT },
    { "ChatHistoryTextColor", ROLE_TEXT },
    { "ColumnHeaderFlatSortedTextColor", ROLE_TEXT }, { "ColumnHeaderFlatTextColor", ROLE_TEXT },
    { "GroupNotifyTextColor", ROLE_TEXT }, { "InspectorTipTextColor", ROLE_TEXT }, { "LabelSelectedColor", ROLE_TEXT },
    { "LabelTextColor", ROLE_TEXT }, { "LoginProgressBoxTextColor", ROLE_TEXT }, { "MenuItemEnabledColor", ROLE_TEXT },
    { "MenuItemHighlightFgColor", ROLE_TEXT }, { "NotifyTextColor", ROLE_TEXT }, { "TextDefaultColor", ROLE_TEXT },
    { "TextFgColor", ROLE_TEXT }, { "TextFgReadOnlyColor", ROLE_TEXT }, { "TimeTextColor", ROLE_TEXT },
    { "ToolTipTextColor", ROLE_TEXT },
    { "SlipstreamTextDim", ROLE_TEXT_DIM }, { "AlSilverGrey", ROLE_TEXT_DIM }, { "AlchemyTextDark", ROLE_TEXT_DIM },
    { "Alchemy_Inv_Link", ROLE_TEXT_DIM }, { "InventoryItemLinkColor", ROLE_TEXT_DIM },
    { "TextFgTentativeColor", ROLE_TEXT_DIM },
    { "SlipstreamTextMuted", ROLE_TEXT_MUTED }, { "ButtonLabelDisabledColor", ROLE_TEXT_MUTED },
    { "ChatHeaderUserNameColor", ROLE_TEXT_MUTED }, { "FloaterButtonImageColor", ROLE_TEXT_MUTED },
    // <Prestige> readable chat: chat timestamps are dimmed text (were text and accent)
    { "ChatTimestampColor", ROLE_TEXT_MUTED }, { "ChatHeaderTimestampColor", ROLE_TEXT_MUTED },
    // </Prestige>
    { "GroupNotifyDimmedTextColor", ROLE_TEXT_MUTED }, { "InventoryItemLibraryColor", ROLE_TEXT_MUTED },
    { "LtGray", ROLE_TEXT_MUTED }, { "MultiSliderTrackColor", ROLE_TEXT_MUTED }, { "ScrollUnselectedColor", ROLE_TEXT_MUTED },
};

typedef std::array<LLColor4, ROLE_COUNT> role_colors_t;

PrestigeUIColors::EGroup group_of(ERole role)
{
    if (role <= ROLE_ACCENT_DEEP) return PrestigeUIColors::ACCENT;
    if (role <= ROLE_SELECT) return PrestigeUIColors::WINDOW;
    return PrestigeUIColors::TEXT;
}

// The colour that stands for a group: the palette entry in Prestige skins, a role colour otherwise.
const char* representative(PrestigeUIColors::EGroup group)
{
    static const char* const names[3][2] = { { "SlipstreamBlue", "FocusColor" },
                                             { "SlipstreamPanel", "PanelFocusBackgroundColor" },
                                             { "SlipstreamText", "LabelTextColor" } };
    const auto& loaded = LLUIColorTable::instance().getLoadedColors();
    return loaded.find(names[group][0]) != loaded.end() ? names[group][0] : names[group][1];
}

bool loaded_color(const char* name, LLColor4& color)
{
    const auto& loaded = LLUIColorTable::instance().getLoadedColors();
    auto it = loaded.find(name);
    if (it == loaded.end())
    {
        return false;
    }
    color = it->second.get();
    return true;
}

LLColor4 mix(const LLColor4& a, const LLColor4& b, F32 t)
{
    return LLColor4(lerp(a.mV[VRED], b.mV[VRED], t), lerp(a.mV[VGREEN], b.mV[VGREEN], t),
                    lerp(a.mV[VBLUE], b.mV[VBLUE], t), 1.f);
}

F32 luminance(const LLColor4& c)
{
    return 0.2126f * c.mV[VRED] + 0.7152f * c.mV[VGREEN] + 0.0722f * c.mV[VBLUE];
}

bool same_rgb(const LLColor4& a, const LLColor4& b)
{
    return fabsf(a.mV[VRED] - b.mV[VRED]) < 0.002f && fabsf(a.mV[VGREEN] - b.mV[VGREEN]) < 0.002f
        && fabsf(a.mV[VBLUE] - b.mV[VBLUE]) < 0.002f;
}

// Role colours of a group from its one colour; the factors reproduce the Prestige palette.
void derive_accent(const LLColor4& accent, bool high_contrast, role_colors_t& out)
{
    out[ROLE_ACCENT] = mix(accent, LLColor4::black, 0.f);
    out[ROLE_ACCENT_LIGHT] = mix(accent, LLColor4::white, 0.22f);
    // fills that carry light text: darker still in high contrast, for white text on them
    out[ROLE_ACCENT_DEEP] = mix(accent, LLColor4::black, high_contrast ? 0.6f : 0.27f);
}

void derive_window(const LLColor4& window, const LLColor4& text, const LLColor4& accent, bool high_contrast,
                   role_colors_t& out)
{
    out[ROLE_PANEL] = mix(window, LLColor4::black, 0.f);
    if (high_contrast)
    {
        out[ROLE_DEEP] = out[ROLE_PANEL];
        out[ROLE_INPUT] = out[ROLE_PANEL];
        out[ROLE_RAISED] = mix(window, text, 0.12f);
        out[ROLE_LINE] = mix(window, text, 0.7f);    // borders that stand out
        out[ROLE_BAR] = mix(window, text, 0.08f);
        out[ROLE_SELECT] = mix(accent, LLColor4::black, 0.65f);
        return;
    }
    // dark windows: fields deeper, raised parts lighter; light windows the other way round
    const bool dark = luminance(window) < 0.5f;
    const LLColor4& deeper = dark ? LLColor4::black : LLColor4::white;
    const LLColor4& raised = dark ? LLColor4::white : LLColor4::black;
    out[ROLE_DEEP] = mix(window, deeper, dark ? 0.42f : 0.7f);
    out[ROLE_INPUT] = mix(window, deeper, dark ? 0.26f : 0.5f);
    out[ROLE_RAISED] = mix(window, raised, 0.05f);
    out[ROLE_LINE] = mix(window, raised, dark ? 0.12f : 0.18f);
    out[ROLE_BAR] = mix(window, raised, dark ? 0.03f : 0.04f);
    out[ROLE_SELECT] = mix(window, raised, dark ? 0.1f : 0.12f);
}

void derive_text(const LLColor4& text, const LLColor4& window, bool high_contrast, role_colors_t& out)
{
    out[ROLE_TEXT] = mix(text, window, 0.f);
    out[ROLE_TEXT_DIM] = mix(text, window, high_contrast ? 0.08f : 0.33f);
    out[ROLE_TEXT_MUTED] = mix(text, window, high_contrast ? 0.2f : 0.45f);
}

// Set every colour of the group from the derived role colours, keeping each colour's own alpha.
void apply_group(PrestigeUIColors::EGroup group, const role_colors_t& colors)
{
    LLUIColorTable& table = LLUIColorTable::instance();
    for (const RoleColor& entry : sRoleColors)
    {
        LLColor4 original;
        if (group_of(entry.role) != group || !loaded_color(entry.name, original))
        {
            continue;   // not in this skin: leave it out rather than add a colour nobody uses
        }
        LLColor4 color = colors[entry.role];
        color.mV[VALPHA] = original.mV[VALPHA];
        table.setColor(entry.name, color);
    }
}

void reset_group(PrestigeUIColors::EGroup group)
{
    LLUIColorTable& table = LLUIColorTable::instance();
    for (const RoleColor& entry : sRoleColors)
    {
        if (group_of(entry.role) == group)
        {
            table.resetToDefault(entry.name);
        }
    }
}

bool group_is_default(PrestigeUIColors::EGroup group)
{
    LLUIColorTable& table = LLUIColorTable::instance();
    for (const RoleColor& entry : sRoleColors)
    {
        if (group_of(entry.role) == group && table.colorExists(entry.name) && !table.isDefault(entry.name))
        {
            return false;
        }
    }
    return true;
}

const LLColor4 HIGH_CONTRAST_ACCENT(1.f, 0.839f, 0.039f, 1.f);   // bright yellow, ~15:1 on black
} // namespace

// static
LLColor4 PrestigeUIColors::getCurrent(EGroup group)
{
    return LLUIColorTable::instance().getColor(representative(group), LLColor4::grey).get();
}

// static
LLColor4 PrestigeUIColors::getDefault(EGroup group)
{
    LLColor4 color = LLColor4::grey;
    loaded_color(representative(group), color);
    return color;
}

// static
void PrestigeUIColors::setColor(EGroup group, const LLColor4& color)
{
    if (same_rgb(color, getDefault(group)))
    {
        reset_group(group);   // the skin's own colour: its exact colours, not derived ones
    }
    else
    {
        role_colors_t roles;
        switch (group)
        {
            case ACCENT:
                derive_accent(color, false, roles);
                break;
            case WINDOW:
                derive_window(color, getCurrent(TEXT), getCurrent(ACCENT), false, roles);
                break;
            case TEXT:
                derive_text(color, getCurrent(WINDOW), false, roles);
                break;
        }
        apply_group(group, roles);
    }

    // the secondary text shades are mixed with the window colour: follow a new window colour
    if (group == WINDOW && !group_is_default(TEXT))
    {
        role_colors_t roles;
        derive_text(getCurrent(TEXT), getCurrent(WINDOW), false, roles);
        apply_group(TEXT, roles);
    }
}

// static
void PrestigeUIColors::resetAll()
{
    reset_group(ACCENT);
    reset_group(WINDOW);
    reset_group(TEXT);
    if (!LLUIColorTable::instance().getLoadedColors().empty())
    {
        LLUIColorTable::instance().saveUserSettings();
    }
}

// static
void PrestigeUIColors::applyHighContrast(bool on)
{
    LLUIColorTable& table = LLUIColorTable::instance();
    if (table.getLoadedColors().empty())
    {
        return;   // skin colours not loaded yet: saving now would empty user_settings/colors.xml
    }
    LLControlVariable* saved_control = gSavedSettings.getControl("PrestigeHighContrastSavedColors");
    LLSD saved = saved_control ? saved_control->getValue() : LLSD();

    if (on)
    {
        // Keep the user's own colours of these groups (once: a second "on" would keep the high
        // contrast set instead).
        if (!saved.has("colors"))
        {
            LLSD colors = LLSD::emptyMap();
            for (const RoleColor& entry : sRoleColors)
            {
                if (table.colorExists(entry.name) && !table.isDefault(entry.name))
                {
                    colors[entry.name] = table.getColor(entry.name).get().getValue();
                }
            }
            saved = LLSD::emptyMap();
            saved["colors"] = colors;
            if (saved_control)
            {
                saved_control->setValue(saved);
            }
        }

        const LLColor4 window = LLColor4::black;
        const LLColor4 text = LLColor4::white;
        role_colors_t roles;
        derive_accent(HIGH_CONTRAST_ACCENT, true, roles);
        derive_window(window, text, HIGH_CONTRAST_ACCENT, true, roles);
        derive_text(text, window, true, roles);
        apply_group(ACCENT, roles);
        apply_group(WINDOW, roles);
        apply_group(TEXT, roles);
    }
    else
    {
        reset_group(ACCENT);
        reset_group(WINDOW);
        reset_group(TEXT);
        if (saved.has("colors"))
        {
            const LLSD& colors = saved["colors"];
            for (LLSD::map_const_iterator it = colors.beginMap(); it != colors.endMap(); ++it)
            {
                table.setColor(it->first, LLColor4(it->second));
            }
        }
        if (saved_control)
        {
            saved_control->setValue(LLSD::emptyMap());
        }
    }
    table.saveUserSettings();
}

//-----------------------------------------------------------------------------
// PrestigePanelYourColors
//-----------------------------------------------------------------------------

bool PrestigePanelYourColors::postBuild()
{
    mAccentSwatch = getChild<LLColorSwatchCtrl>("prestige_accent_swatch");
    mWindowSwatch = getChild<LLColorSwatchCtrl>("prestige_window_swatch");
    mTextSwatch = getChild<LLColorSwatchCtrl>("prestige_text_swatch");

    mAccentSwatch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSwatchCommit(PrestigeUIColors::ACCENT, mAccentSwatch); });
    mWindowSwatch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSwatchCommit(PrestigeUIColors::WINDOW, mWindowSwatch); });
    mTextSwatch->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSwatchCommit(PrestigeUIColors::TEXT, mTextSwatch); });
    getChild<LLButton>("prestige_reset_colors")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onClickReset(); });

    // high contrast replaces these colours while it is on: show what is in use
    if (LLControlVariable* control = gSavedSettings.getControl("PrestigeHighContrast"))
    {
        mHighContrastConnection = control->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&) { refreshSwatches(); });
    }

    refreshSwatches();
    return true;
}

void PrestigePanelYourColors::onVisibilityChange(bool new_visibility)
{
    if (new_visibility)
    {
        refreshSwatches();
    }
    else if (mDirty)
    {
        // saved once the page closes, not on every step of a colour picker drag
        mDirty = false;
        LLUIColorTable::instance().saveUserSettings();
    }
    LLPanel::onVisibilityChange(new_visibility);
}

void PrestigePanelYourColors::refreshSwatches()
{
    mAccentSwatch->set(PrestigeUIColors::getCurrent(PrestigeUIColors::ACCENT));
    mWindowSwatch->set(PrestigeUIColors::getCurrent(PrestigeUIColors::WINDOW));
    mTextSwatch->set(PrestigeUIColors::getCurrent(PrestigeUIColors::TEXT));
}

void PrestigePanelYourColors::onSwatchCommit(PrestigeUIColors::EGroup group, LLColorSwatchCtrl* swatch)
{
    const LLColor4 color = swatch->get();
    if (same_rgb(color, PrestigeUIColors::getCurrent(group)))
    {
        return;   // no change (Preferences' Cancel commits every swatch again)
    }
    PrestigeUIColors::setColor(group, color);
    mDirty = true;
}

void PrestigePanelYourColors::onClickReset()
{
    PrestigeUIColors::resetAll();
    mDirty = false;
    refreshSwatches();
}
