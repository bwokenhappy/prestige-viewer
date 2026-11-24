/**
 * @file prestigemarkdown.h
 * @brief Markdown parsing for notecards: turns Markdown source into a flat
 *        list of styled runs. Has no UI dependencies, so it can be tested on
 *        its own; prestigemarkdownrenderer.cpp draws the runs into a text editor.
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

#ifndef PRESTIGE_MARKDOWN_H
#define PRESTIGE_MARKDOWN_H

#include <string>
#include <vector>

class PrestigeMarkdown
{
public:
    // What kind of block a run belongs to.
    enum EBlock
    {
        BLOCK_TEXT = 0,
        BLOCK_HEADING1,
        BLOCK_HEADING2,
        BLOCK_HEADING3,     // also used for #### to ######
        BLOCK_QUOTE,        // text inside a > block quote
        BLOCK_QUOTE_BAR,    // the bar drawn in front of a quoted line
        BLOCK_LIST_MARKER,  // indentation plus bullet or number of a list item
        BLOCK_RULE,         // horizontal rule; the run has no text
        BLOCK_CODE          // fenced code block
    };

    // Inline styles, combined as bit flags.
    enum EInline
    {
        INLINE_BOLD   = 0x01,
        INLINE_ITALIC = 0x02,
        INLINE_CODE   = 0x04,
        INLINE_LINK   = 0x08
    };

    // Token kinds inside a code block coloured as LSL.
    enum ECodeToken
    {
        CODE_PLAIN = 0,
        CODE_WORD,      // identifier: may be an LSL keyword, function, constant...
        CODE_COMMENT,
        CODE_STRING,
        CODE_NUMBER
    };

    struct Run
    {
        std::string mText;      // UTF-8
        EBlock      mBlock = BLOCK_TEXT;
        unsigned    mFlags = 0; // EInline bits
        bool        mLSL = false; // code block run that should get LSL colouring
        ECodeToken  mToken = CODE_PLAIN;
        std::string mHref;      // link target when INLINE_LINK is set
    };
    typedef std::vector<Run> run_list_t;

    // Parses Markdown source (UTF-8) into runs, appended to 'runs'. Line breaks
    // in the source are kept. Unknown syntax is kept as plain text. Never throws
    // on malformed input.
    static void parse(const std::string& markdown, run_list_t& runs);

    // Parses one line of inline Markdown (emphasis, code spans, links).
    static void parseInline(const std::string& text, EBlock block, unsigned flags, run_list_t& runs);

    // Splits code into BLOCK_CODE runs, one per word, number, string or
    // comment. 'lsl' sets mLSL on them so they get LSL colouring.
    static void tokenizeCode(const std::string& code, bool lsl, run_list_t& runs);

    // Heuristic for unlabelled code fences: does this look like LSL?
    static bool looksLikeLSL(const std::string& code);

    // Is the language tag of a code fence one we colour as LSL?
    static bool isLSLLanguage(const std::string& lang);

    // Only web and Second Life links become clickable.
    static bool isSafeLinkUrl(const std::string& url);

    // True when the first line of the text is a Markdown heading ("# Title").
    static bool startsWithHeading(const std::string& text);
};

#endif // PRESTIGE_MARKDOWN_H
