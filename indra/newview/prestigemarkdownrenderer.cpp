/**
 * @file prestigemarkdownrenderer.cpp
 * @brief Draws Markdown (parsed by PrestigeMarkdown) into a read-only text editor.
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

#include "prestigemarkdownrenderer.h"
#include "prestigemarkdown.h"

#include "llfontgl.h"
#include "llfontregistry.h"
#include "llkeywords.h"
#include "llstyle.h"
#include "llsyntaxid.h"
#include "lltexteditor.h"
#include "lluicolortable.h"
#include "llviewertexteditor.h"

#include <map>
#include <string>

namespace
{
    typedef std::map<std::string, LLUIColor> lsl_color_map_t;

    // Colours of LSL keywords, functions, constants and events, taken from the
    // same syntax file and colour groups as the script editor.
    const lsl_color_map_t& getLSLColors()
    {
        static lsl_color_map_t sColors;
        static bool sBuilt = false;
        if (!sBuilt)
        {
            LLSD syntax = LLSyntaxIdLSL::getInstance()->getKeywordsXML();
            if (syntax.isMap() && syntax.size() > 0)
            {
                LLKeywords keywords;
                keywords.initialize(syntax, false);
                keywords.processTokens();
                for (LLKeywords::keyword_iterator_t it = keywords.begin(); it != keywords.end(); ++it)
                {
                    const LLKeywordToken* token = it->second;
                    if (token)
                    {
                        sColors[wstring_to_utf8str(token->getToken())] = token->getColor();
                    }
                }
                sBuilt = true;
            }
        }
        return sColors;
    }

    const LLFontGL* getStyledFont(const std::string& name, const std::string& size, U8 style, const LLFontGL* fallback)
    {
        const LLFontGL* font = LLFontGL::getFont(LLFontDescriptor(name, size, style));
        if (!font && style != LLFontGL::NORMAL)
        {
            font = LLFontGL::getFont(LLFontDescriptor(name, size, LLFontGL::NORMAL));
        }
        return font ? font : fallback;
    }

    const LLFontGL* fontForRun(const PrestigeMarkdown::Run& run, const LLFontGL* base_font)
    {
        if (run.mBlock == PrestigeMarkdown::BLOCK_CODE || (run.mFlags & PrestigeMarkdown::INLINE_CODE))
        {
            const LLFontGL* mono = LLFontGL::getFontMonospace();
            return mono ? mono : base_font;
        }

        U8 style = LLFontGL::NORMAL;
        if (run.mFlags & PrestigeMarkdown::INLINE_BOLD)
        {
            style = static_cast<U8>(style | LLFontGL::BOLD);
        }
        if (run.mFlags & PrestigeMarkdown::INLINE_ITALIC)
        {
            style = static_cast<U8>(style | LLFontGL::ITALIC);
        }

        std::string name = "SansSerif";
        std::string size = "Medium";
        if (base_font)
        {
            name = base_font->getFontDesc().getName();
            size = base_font->getFontDesc().getSize();
        }

        switch (run.mBlock)
        {
        case PrestigeMarkdown::BLOCK_HEADING1:
            return getStyledFont(name, "Huge", static_cast<U8>(style | LLFontGL::BOLD), base_font);
        case PrestigeMarkdown::BLOCK_HEADING2:
            return getStyledFont(name, "Large", static_cast<U8>(style | LLFontGL::BOLD), base_font);
        case PrestigeMarkdown::BLOCK_HEADING3:
            return getStyledFont(name, size, static_cast<U8>(style | LLFontGL::BOLD), base_font);
        default:
            return style == LLFontGL::NORMAL ? base_font : getStyledFont(name, size, style, base_font);
        }
    }

    void setColor(LLStyle::Params& params, const LLUIColor& color)
    {
        params.color = color;
        params.readonly_color = color;
    }

    LLStyle::Params styleForRun(const PrestigeMarkdown::Run& run, const LLFontGL* base_font)
    {
        LLStyle::Params params;
        const LLFontGL* font = fontForRun(run, base_font);
        if (font)
        {
            params.font = font;
        }

        LLUIColorTable& colors = LLUIColorTable::instance();

        if (run.mBlock == PrestigeMarkdown::BLOCK_CODE || (run.mFlags & PrestigeMarkdown::INLINE_CODE))
        {
            params.draw_highlight_bg = true;
            params.highlight_bg_color = colors.getColor("PrestigeMarkdownCodeBgColor", LLColor4(0.5f, 0.5f, 0.5f, 0.22f));
        }

        if (run.mBlock == PrestigeMarkdown::BLOCK_CODE && run.mLSL)
        {
            switch (run.mToken)
            {
            case PrestigeMarkdown::CODE_COMMENT:
                setColor(params, colors.getColor("SyntaxLslComment"));
                break;
            case PrestigeMarkdown::CODE_STRING:
                setColor(params, colors.getColor("SyntaxLslStringLiteral"));
                break;
            case PrestigeMarkdown::CODE_WORD:
            {
                const lsl_color_map_t& lsl_colors = getLSLColors();
                lsl_color_map_t::const_iterator it = lsl_colors.find(run.mText);
                if (it != lsl_colors.end())
                {
                    setColor(params, it->second);
                }
                break;
            }
            default:
                break;
            }
        }

        switch (run.mBlock)
        {
        case PrestigeMarkdown::BLOCK_QUOTE:
            params.alpha = 0.8f;
            break;
        case PrestigeMarkdown::BLOCK_QUOTE_BAR:
        case PrestigeMarkdown::BLOCK_LIST_MARKER:
            params.alpha = 0.6f;
            break;
        case PrestigeMarkdown::BLOCK_RULE:
            params.alpha = 0.4f;
            break;
        default:
            break;
        }

        if ((run.mFlags & PrestigeMarkdown::INLINE_LINK) && PrestigeMarkdown::isSafeLinkUrl(run.mHref))
        {
            setColor(params, colors.getColor("HTMLLinkColor"));
            params.is_link = true;
            params.link_href = run.mHref;
        }
        return params;
    }

    // Appends text, showing embedded notecard items by name.
    void appendWithItems(LLTextEditor* target, const std::string& text, const LLStyle::Params& params)
    {
        // Embedded item characters (U+100000 and up) all start with byte 0xF4 in UTF-8.
        if (text.find('\xF4') == std::string::npos)
        {
            target->appendText(text, false, params);
            return;
        }

        const LLWString wtext = utf8str_to_wstring(text);
        LLWString pending;
        for (llwchar wc : wtext)
        {
            if (wc >= LLTextEditor::FIRST_EMBEDDED_CHAR && wc <= LLTextEditor::LAST_EMBEDDED_CHAR)
            {
                if (!pending.empty())
                {
                    target->appendText(wstring_to_utf8str(pending), false, params);
                    pending.clear();
                }
                const std::string name = LLViewerTextEditor::getEmbeddedItemName(wc);
                if (!name.empty())
                {
                    LLStyle::Params item_params(params);
                    setColor(item_params, LLUIColorTable::instance().getColor("TextEmbeddedItemReadOnlyColor"));
                    item_params.is_link = false;
                    item_params.link_href = std::string();
                    target->appendText("[" + name + "]", false, item_params);
                }
            }
            else
            {
                pending.push_back(wc);
            }
        }
        if (!pending.empty())
        {
            target->appendText(wstring_to_utf8str(pending), false, params);
        }
    }
}

// static
void PrestigeMarkdownRenderer::render(LLTextEditor* target, const std::string& markdown, bool keep_scroll)
{
    if (!target)
    {
        return;
    }

    PrestigeMarkdown::run_list_t runs;
    PrestigeMarkdown::parse(markdown, runs);

    LLTextBase::ScrollAnchor anchor;
    if (keep_scroll)
    {
        anchor = target->captureScrollAnchor();
    }

    const LLFontGL* base_font = target->getFont();
    target->setText(LLStringUtil::null);

    for (const PrestigeMarkdown::Run& run : runs)
    {
        const LLStyle::Params params = styleForRun(run, base_font);

        if (run.mBlock == PrestigeMarkdown::BLOCK_RULE)
        {
            // A row of em dashes stands in for a horizontal rule.
            std::string rule;
            for (S32 i = 0; i < 16; ++i)
            {
                rule += "\xE2\x80\x94";
            }
            target->appendText(rule, false, params);
            continue;
        }

        if (run.mText.empty())
        {
            continue;
        }

        const bool is_code = run.mBlock == PrestigeMarkdown::BLOCK_CODE || (run.mFlags & PrestigeMarkdown::INLINE_CODE);
        if (is_code)
        {
            // Code is shown exactly as written: no automatic links inside it.
            target->setParseHTML(false);
        }
        appendWithItems(target, run.mText, params);
        if (is_code)
        {
            target->setParseHTML(true);
        }

        if (params.is_link)
        {
            target->setLastSegmentToolTip(run.mHref);
        }
    }

    if (keep_scroll && anchor.mValid)
    {
        target->restoreScrollAnchor(anchor, llclamp(anchor.mDocIndex, 0, target->getLength()));
    }
}
