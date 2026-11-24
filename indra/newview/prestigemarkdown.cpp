/**
 * @file prestigemarkdown.cpp
 * @brief Markdown parsing for notecards (no UI dependencies).
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

#include "prestigemarkdown.h"

#include <string>
#include <vector>

namespace
{
    // Deepest nesting of emphasis and links inside one another.
    const int MAX_INLINE_DEPTH = 8;
    // How far a link label or destination is searched for its closing bracket.
    const size_t MAX_LINK_SCAN = 2048;
    // Deepest list indentation that is shown.
    const size_t MAX_LIST_LEVEL = 8;

    // ASCII-only character classes: bytes of multi-byte UTF-8 sequences are
    // never treated as Markdown syntax.
    inline bool isSpace(char c)
    {
        return c == ' ' || c == '\t';
    }

    inline bool isDigit(char c)
    {
        return c >= '0' && c <= '9';
    }

    inline bool isAlpha(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    inline bool isAlnum(char c)
    {
        return isAlpha(c) || isDigit(c);
    }

    inline bool isIdentChar(char c)
    {
        return isAlnum(c) || c == '_';
    }

    inline bool isAsciiPunct(char c)
    {
        return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
    }

    inline char toLowerAscii(char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    std::string trimSpaces(const std::string& s)
    {
        size_t b = 0;
        size_t e = s.size();
        while (b < e && isSpace(s[b]))
        {
            ++b;
        }
        while (e > b && isSpace(s[e - 1]))
        {
            --e;
        }
        return s.substr(b, e - b);
    }

    // Number of leading spaces (tabs count as 4).
    size_t leadingIndent(const std::string& line, size_t* first_non_space)
    {
        size_t indent = 0;
        size_t i = 0;
        while (i < line.size() && isSpace(line[i]))
        {
            indent += (line[i] == '\t') ? 4 : 1;
            ++i;
        }
        if (first_non_space)
        {
            *first_non_space = i;
        }
        return indent;
    }

    size_t countRun(const std::string& s, size_t pos, size_t end, char c)
    {
        size_t n = 0;
        while (pos + n < end && s[pos + n] == c)
        {
            ++n;
        }
        return n;
    }

    // Adds text to the run list, merging with the previous run when it has
    // exactly the same style.
    void addRun(PrestigeMarkdown::run_list_t& runs, const std::string& text,
                PrestigeMarkdown::EBlock block, unsigned flags, const std::string& href)
    {
        if (text.empty())
        {
            return;
        }
        if (!runs.empty())
        {
            PrestigeMarkdown::Run& last = runs.back();
            if (last.mBlock == block && last.mFlags == flags && !last.mLSL
                && last.mToken == PrestigeMarkdown::CODE_PLAIN && last.mHref == href)
            {
                last.mText += text;
                return;
            }
        }
        PrestigeMarkdown::Run run;
        run.mText = text;
        run.mBlock = block;
        run.mFlags = flags;
        run.mHref = href;
        runs.push_back(run);
    }

    void addNewline(PrestigeMarkdown::run_list_t& runs)
    {
        addRun(runs, "\n", PrestigeMarkdown::BLOCK_TEXT, 0, std::string());
    }

    // Finds a run of exactly 'len' backticks at or after 'from'. Returns npos if none.
    size_t findBacktickRun(const std::string& s, size_t from, size_t end, size_t len)
    {
        size_t j = from;
        while (j < end)
        {
            if (s[j] == '`')
            {
                size_t run = countRun(s, j, end, '`');
                if (run == len)
                {
                    return j;
                }
                j += run;
            }
            else
            {
                ++j;
            }
        }
        return std::string::npos;
    }

    // Finds the closing delimiter run for emphasis opened with 'len' x 'c'.
    size_t findEmphasisClose(const std::string& s, size_t from, size_t end, char c, size_t len)
    {
        size_t j = from;
        while (j < end)
        {
            const char d = s[j];
            if (d == '\\' && j + 1 < end)
            {
                j += 2;
                continue;
            }
            if (d == '`')
            {
                size_t run = countRun(s, j, end, '`');
                size_t close = findBacktickRun(s, j + run, end, run);
                j = (close != std::string::npos) ? close + run : j + run;
                continue;
            }
            if (d == c)
            {
                size_t run = countRun(s, j, end, c);
                if (run == len && j > from && !isSpace(s[j - 1]))
                {
                    // An underscore only closes at the end of a word: snake_case stays as is.
                    if (c != '_' || j + run >= end || !isAlnum(s[j + run]))
                    {
                        return j;
                    }
                }
                j += run;
                continue;
            }
            ++j;
        }
        return std::string::npos;
    }

    // Tries to read a link "[label](destination "title")" starting at the '['
    // at 'open'. On success fills the label range, the destination and the
    // position just after the closing ')'.
    bool matchLink(const std::string& s, size_t open, size_t end,
                   size_t& label_end, std::string& href, size_t& after)
    {
        if (open >= end || s[open] != '[')
        {
            return false;
        }

        const size_t limit = (end - open > MAX_LINK_SCAN) ? open + MAX_LINK_SCAN : end;
        size_t depth = 0;
        size_t j = open;
        bool closed = false;
        while (j < limit)
        {
            const char d = s[j];
            if (d == '\\')
            {
                j += 2;
                continue;
            }
            if (d == '[')
            {
                ++depth;
            }
            else if (d == ']')
            {
                --depth;
                if (depth == 0)
                {
                    closed = true;
                    break;
                }
            }
            ++j;
        }
        if (!closed || j >= end)
        {
            return false;
        }
        label_end = j;
        if (label_end == open + 1)
        {
            return false; // empty label
        }

        size_t k = label_end + 1;
        if (k >= end || s[k] != '(')
        {
            return false;
        }
        ++k;
        while (k < end && isSpace(s[k]))
        {
            ++k;
        }

        const size_t dest_limit = (end - k > MAX_LINK_SCAN) ? k + MAX_LINK_SCAN : end;
        href.clear();
        if (k < end && s[k] == '<')
        {
            ++k;
            while (k < dest_limit && s[k] != '>' && s[k] != '<')
            {
                href += s[k];
                ++k;
            }
            if (k >= dest_limit || s[k] != '>')
            {
                return false;
            }
            ++k;
        }
        else
        {
            size_t parens = 0;
            while (k < dest_limit && !isSpace(s[k]))
            {
                const char d = s[k];
                if (d == '(')
                {
                    ++parens;
                }
                else if (d == ')')
                {
                    if (parens == 0)
                    {
                        break;
                    }
                    --parens;
                }
                href += d;
                ++k;
            }
        }
        href = trimSpaces(href);
        if (href.empty())
        {
            return false;
        }

        while (k < end && isSpace(s[k]))
        {
            ++k;
        }
        // Optional title: "title", 'title' or (title). Ignored.
        if (k < end && (s[k] == '"' || s[k] == '\'' || s[k] == '('))
        {
            const char close_char = (s[k] == '(') ? ')' : s[k];
            ++k;
            while (k < dest_limit && s[k] != close_char)
            {
                ++k;
            }
            if (k >= dest_limit)
            {
                return false;
            }
            ++k;
            while (k < end && isSpace(s[k]))
            {
                ++k;
            }
        }
        if (k >= end || s[k] != ')')
        {
            return false;
        }
        after = k + 1;
        return true;
    }

    bool startsWithNoCase(const std::string& s, const char* prefix)
    {
        size_t i = 0;
        for (; prefix[i]; ++i)
        {
            if (i >= s.size() || toLowerAscii(s[i]) != prefix[i])
            {
                return false;
            }
        }
        return true;
    }

    void parseInlineRange(const std::string& s, size_t begin, size_t end,
                          PrestigeMarkdown::EBlock block, unsigned flags,
                          const std::string& href, int depth,
                          PrestigeMarkdown::run_list_t& runs)
    {
        std::string pending;
        // Delimiter runs that found no closer in this range, so later ones of
        // the same kind are not searched again: [0] '*', [1] '_'; index = run length.
        bool emphasis_failed[2][4] = { { false, false, false, false }, { false, false, false, false } };
        std::vector<size_t> code_failed;

        size_t i = begin;
        while (i < end)
        {
            const char c = s[i];

            // Backslash escape of ASCII punctuation.
            if (c == '\\' && i + 1 < end && isAsciiPunct(s[i + 1]))
            {
                pending += s[i + 1];
                i += 2;
                continue;
            }

            // Code span.
            if (c == '`')
            {
                const size_t run = countRun(s, i, end, '`');
                bool known_failed = false;
                for (size_t f : code_failed)
                {
                    if (f == run)
                    {
                        known_failed = true;
                        break;
                    }
                }
                size_t close = std::string::npos;
                if (!known_failed)
                {
                    close = findBacktickRun(s, i + run, end, run);
                    if (close == std::string::npos)
                    {
                        code_failed.push_back(run);
                    }
                }
                if (close != std::string::npos)
                {
                    addRun(runs, pending, block, flags, href);
                    pending.clear();
                    std::string code = s.substr(i + run, close - (i + run));
                    // One space padding on both sides is stripped, as in CommonMark.
                    if (code.size() >= 2 && code.front() == ' ' && code.back() == ' '
                        && code.find_first_not_of(' ') != std::string::npos)
                    {
                        code = code.substr(1, code.size() - 2);
                    }
                    addRun(runs, code, block, flags | PrestigeMarkdown::INLINE_CODE, href);
                    i = close + run;
                    continue;
                }
                pending.append(run, '`');
                i += run;
                continue;
            }

            // Emphasis: *italic*, **bold**, ***both***, and the same with underscores.
            if (c == '*' || c == '_')
            {
                const size_t run = countRun(s, i, end, c);
                const size_t kind = (c == '*') ? 0 : 1;
                bool can_open = depth < MAX_INLINE_DEPTH && run <= 3
                    && i + run < end && !isSpace(s[i + run]);
                if (can_open && c == '_' && i > begin && isAlnum(s[i - 1]))
                {
                    can_open = false; // intraword underscore
                }
                if (can_open && !emphasis_failed[kind][run])
                {
                    const size_t close = findEmphasisClose(s, i + run, end, c, run);
                    if (close != std::string::npos)
                    {
                        addRun(runs, pending, block, flags, href);
                        pending.clear();
                        unsigned add = PrestigeMarkdown::INLINE_ITALIC;
                        if (run == 2)
                        {
                            add = PrestigeMarkdown::INLINE_BOLD;
                        }
                        else if (run == 3)
                        {
                            add = PrestigeMarkdown::INLINE_BOLD | PrestigeMarkdown::INLINE_ITALIC;
                        }
                        parseInlineRange(s, i + run, close, block, flags | add, href, depth + 1, runs);
                        i = close + run;
                        continue;
                    }
                    emphasis_failed[kind][run] = true;
                }
                pending.append(run, c);
                i += run;
                continue;
            }

            // Links [label](url) and images ![alt](url), shown as links.
            if ((c == '[' || (c == '!' && i + 1 < end && s[i + 1] == '['))
                && !(flags & PrestigeMarkdown::INLINE_LINK) && depth < MAX_INLINE_DEPTH)
            {
                const size_t open = (c == '!') ? i + 1 : i;
                size_t label_end = 0;
                size_t after = 0;
                std::string url;
                if (matchLink(s, open, end, label_end, url, after))
                {
                    addRun(runs, pending, block, flags, href);
                    pending.clear();
                    parseInlineRange(s, open + 1, label_end, block, flags | PrestigeMarkdown::INLINE_LINK, url, depth + 1, runs);
                    i = after;
                    continue;
                }
                pending += c;
                ++i;
                continue;
            }

            // Autolink <https://...>
            if (c == '<' && !(flags & PrestigeMarkdown::INLINE_LINK))
            {
                const size_t limit = (end - i > MAX_LINK_SCAN) ? i + MAX_LINK_SCAN : end;
                size_t j = i + 1;
                while (j < limit && s[j] != '>' && s[j] != '<' && !isSpace(s[j]))
                {
                    ++j;
                }
                if (j < limit && s[j] == '>')
                {
                    const std::string url = s.substr(i + 1, j - i - 1);
                    if (startsWithNoCase(url, "http://") || startsWithNoCase(url, "https://")
                        || startsWithNoCase(url, "secondlife://"))
                    {
                        addRun(runs, pending, block, flags, href);
                        pending.clear();
                        addRun(runs, url, block, flags | PrestigeMarkdown::INLINE_LINK, url);
                        i = j + 1;
                        continue;
                    }
                }
            }

            pending += c;
            ++i;
        }
        addRun(runs, pending, block, flags, href);
    }

    // "# Title" -> level 1..6 and the title text. Returns 0 when not a heading.
    int parseHeading(const std::string& line, std::string& content)
    {
        size_t first = 0;
        if (leadingIndent(line, &first) > 3)
        {
            return 0;
        }
        const size_t hashes = countRun(line, first, line.size(), '#');
        if (hashes < 1 || hashes > 6)
        {
            return 0;
        }
        const size_t after = first + hashes;
        if (after < line.size() && !isSpace(line[after]))
        {
            return 0; // "#hashtag" is not a heading
        }
        content = trimSpaces(line.substr(after));
        // Optional closing hashes: "## Title ##"
        size_t e = content.size();
        while (e > 0 && content[e - 1] == '#')
        {
            --e;
        }
        if (e == 0)
        {
            content.clear();
        }
        else if (e < content.size() && isSpace(content[e - 1]))
        {
            content = trimSpaces(content.substr(0, e));
        }
        return static_cast<int>(hashes);
    }

    bool isRule(const std::string& line)
    {
        size_t first = 0;
        if (leadingIndent(line, &first) > 3)
        {
            return false;
        }
        char rule_char = 0;
        size_t count = 0;
        for (size_t i = first; i < line.size(); ++i)
        {
            const char c = line[i];
            if (isSpace(c))
            {
                continue;
            }
            if (c != '-' && c != '*' && c != '_')
            {
                return false;
            }
            if (rule_char == 0)
            {
                rule_char = c;
            }
            else if (c != rule_char)
            {
                return false;
            }
            ++count;
        }
        return count >= 3;
    }

    // Opening code fence: ``` or ~~~ (3 or more), optional language tag.
    bool isFenceOpen(const std::string& line, char& fence_char, size_t& fence_len, std::string& lang)
    {
        size_t first = 0;
        if (leadingIndent(line, &first) > 3 || first >= line.size())
        {
            return false;
        }
        const char c = line[first];
        if (c != '`' && c != '~')
        {
            return false;
        }
        const size_t run = countRun(line, first, line.size(), c);
        if (run < 3)
        {
            return false;
        }
        const std::string info = trimSpaces(line.substr(first + run));
        if (c == '`' && info.find('`') != std::string::npos)
        {
            return false; // that is an inline code span, not a fence
        }
        fence_char = c;
        fence_len = run;
        lang.clear();
        for (size_t i = 0; i < info.size() && !isSpace(info[i]) && info[i] != '{'; ++i)
        {
            lang += toLowerAscii(info[i]);
        }
        return true;
    }

    bool isFenceClose(const std::string& line, char fence_char, size_t fence_len)
    {
        size_t first = 0;
        if (leadingIndent(line, &first) > 3)
        {
            return false;
        }
        const size_t run = countRun(line, first, line.size(), fence_char);
        if (run < fence_len)
        {
            return false;
        }
        return trimSpaces(line.substr(first + run)).empty();
    }

    // List item: "- item", "* item", "+ item", "1. item", "1) item".
    // Fills the marker text to show and where the item text starts.
    bool parseListItem(const std::string& line, std::string& marker, size_t& content_start)
    {
        size_t first = 0;
        const size_t indent = leadingIndent(line, &first);
        if (first >= line.size())
        {
            return false;
        }
        size_t level = indent / 2;
        if (level > MAX_LIST_LEVEL)
        {
            level = MAX_LIST_LEVEL;
        }
        const std::string pad(2 + level * 4, ' ');

        const char c = line[first];
        if ((c == '-' || c == '*' || c == '+') && first + 1 < line.size() && isSpace(line[first + 1]))
        {
            // U+2022 bullet, U+2013 en dash on odd nesting levels
            marker = pad + ((level % 2) ? "\xE2\x80\x93 " : "\xE2\x80\xA2 ");
            content_start = first + 2;
        }
        else if (isDigit(c))
        {
            size_t j = first;
            while (j < line.size() && isDigit(line[j]) && j - first < 9)
            {
                ++j;
            }
            if (j >= line.size() || (line[j] != '.' && line[j] != ')'))
            {
                return false;
            }
            if (j + 1 < line.size() && !isSpace(line[j + 1]))
            {
                return false;
            }
            if (j + 1 >= line.size())
            {
                return false; // "2024." alone on a line is not a list
            }
            marker = pad + line.substr(first, j - first) + ". ";
            content_start = j + 2;
        }
        else
        {
            return false;
        }
        while (content_start < line.size() && isSpace(line[content_start]))
        {
            ++content_start;
        }
        if (content_start > line.size())
        {
            content_start = line.size();
        }
        return true;
    }

    void emitCode(const std::string& code, const std::string& lang, PrestigeMarkdown::run_list_t& runs)
    {
        if (code.empty())
        {
            return;
        }
        const bool lsl = PrestigeMarkdown::isLSLLanguage(lang)
            || (lang.empty() && PrestigeMarkdown::looksLikeLSL(code));
        // Always split into tokens, so the view can select single words in code.
        PrestigeMarkdown::tokenizeCode(code, lsl, runs);
    }
}

// static
void PrestigeMarkdown::parseInline(const std::string& text, EBlock block, unsigned flags, run_list_t& runs)
{
    parseInlineRange(text, 0, text.size(), block, flags, std::string(), 0, runs);
}

// static
void PrestigeMarkdown::parse(const std::string& markdown, run_list_t& runs)
{
    // Split into lines; "\r\n" and "\r" line ends are accepted.
    std::vector<std::string> lines;
    {
        std::string line;
        for (size_t i = 0; i < markdown.size(); ++i)
        {
            const char c = markdown[i];
            if (c == '\r')
            {
                if (i + 1 < markdown.size() && markdown[i + 1] == '\n')
                {
                    ++i;
                }
                lines.push_back(line);
                line.clear();
            }
            else if (c == '\n')
            {
                lines.push_back(line);
                line.clear();
            }
            else
            {
                line += c;
            }
        }
        lines.push_back(line);
    }

    bool in_fence = false;
    char fence_char = '`';
    size_t fence_len = 3;
    std::string fence_lang;
    std::string code;
    bool code_has_lines = false;

    for (size_t li = 0; li < lines.size(); ++li)
    {
        const std::string& line = lines[li];
        const bool last = (li + 1 == lines.size());

        if (in_fence)
        {
            if (isFenceClose(line, fence_char, fence_len))
            {
                emitCode(code, fence_lang, runs);
                in_fence = false;
                if (!last)
                {
                    addNewline(runs);
                }
            }
            else
            {
                if (code_has_lines)
                {
                    code += '\n';
                }
                code += line;
                code_has_lines = true;
            }
            continue;
        }

        if (isFenceOpen(line, fence_char, fence_len, fence_lang))
        {
            in_fence = true;
            code.clear();
            code_has_lines = false;
            continue;
        }

        std::string content;
        std::string marker;
        size_t content_start = 0;
        const int level = parseHeading(line, content);
        if (level > 0)
        {
            const EBlock block = (level == 1) ? BLOCK_HEADING1 : (level == 2) ? BLOCK_HEADING2 : BLOCK_HEADING3;
            parseInline(content, block, 0, runs);
        }
        else if (isRule(line))
        {
            Run run;
            run.mBlock = BLOCK_RULE;
            runs.push_back(run);
        }
        else
        {
            size_t first = 0;
            const size_t indent = leadingIndent(line, &first);
            if (indent <= 3 && first < line.size() && line[first] == '>')
            {
                // Block quote, possibly nested ("> > text").
                size_t pos = first;
                size_t quote_depth = 0;
                while (pos < line.size() && line[pos] == '>' && quote_depth < MAX_LIST_LEVEL)
                {
                    ++quote_depth;
                    ++pos;
                    if (pos < line.size() && isSpace(line[pos]))
                    {
                        ++pos;
                    }
                    while (pos < line.size() && line[pos] == ' ' && pos + 1 < line.size() && line[pos + 1] == '>')
                    {
                        ++pos;
                    }
                }
                std::string bar;
                for (size_t q = 0; q < quote_depth; ++q)
                {
                    bar += "| ";
                }
                addRun(runs, bar, BLOCK_QUOTE_BAR, 0, std::string());
                parseInline(line.substr(pos), BLOCK_QUOTE, 0, runs);
            }
            else if (parseListItem(line, marker, content_start))
            {
                addRun(runs, marker, BLOCK_LIST_MARKER, 0, std::string());
                parseInline(line.substr(content_start), BLOCK_TEXT, 0, runs);
            }
            else
            {
                parseInline(line, BLOCK_TEXT, 0, runs);
            }
        }

        if (!last)
        {
            addNewline(runs);
        }
    }

    if (in_fence)
    {
        // Unterminated fence: show what there is as code.
        emitCode(code, fence_lang, runs);
    }
}

// static
void PrestigeMarkdown::tokenizeCode(const std::string& code, bool lsl, run_list_t& runs)
{
    const size_t n = code.size();
    std::string plain;

    auto flushPlain = [&]()
    {
        if (!plain.empty())
        {
            Run run;
            run.mText = plain;
            run.mBlock = BLOCK_CODE;
            run.mLSL = lsl;
            run.mToken = CODE_PLAIN;
            runs.push_back(run);
            plain.clear();
        }
    };
    auto addToken = [&](size_t start, size_t stop, ECodeToken token)
    {
        flushPlain();
        Run run;
        run.mText = code.substr(start, stop - start);
        run.mBlock = BLOCK_CODE;
        run.mLSL = lsl;
        run.mToken = token;
        runs.push_back(run);
    };

    size_t i = 0;
    while (i < n)
    {
        const char c = code[i];
        const char next = (i + 1 < n) ? code[i + 1] : '\0';

        if (c == '/' && next == '/')
        {
            size_t j = code.find('\n', i);
            if (j == std::string::npos)
            {
                j = n;
            }
            addToken(i, j, CODE_COMMENT);
            i = j;
        }
        else if (c == '/' && next == '*')
        {
            size_t j = code.find("*/", i + 2);
            j = (j == std::string::npos) ? n : j + 2;
            addToken(i, j, CODE_COMMENT);
            i = j;
        }
        else if (c == '"')
        {
            size_t j = i + 1;
            while (j < n && code[j] != '"' && code[j] != '\n')
            {
                j += (code[j] == '\\' && j + 1 < n) ? 2 : 1;
            }
            if (j < n && code[j] == '"')
            {
                ++j;
            }
            if (j > n)
            {
                j = n;
            }
            addToken(i, j, CODE_STRING);
            i = j;
        }
        else if (isAlpha(c) || c == '_')
        {
            size_t j = i + 1;
            while (j < n && isIdentChar(code[j]))
            {
                ++j;
            }
            addToken(i, j, CODE_WORD);
            i = j;
        }
        else if (isDigit(c) || (c == '.' && isDigit(next)))
        {
            size_t j = i + 1;
            while (j < n && (isAlnum(code[j]) || code[j] == '.'))
            {
                ++j;
            }
            addToken(i, j, CODE_NUMBER);
            i = j;
        }
        else
        {
            plain += c;
            ++i;
        }
    }
    flushPlain();
}

// static
bool PrestigeMarkdown::looksLikeLSL(const std::string& code)
{
    const size_t n = code.size();
    for (size_t i = 0; i < n; ++i)
    {
        if (i > 0 && isIdentChar(code[i - 1]))
        {
            continue; // only look at the start of words
        }
        // llFunctionName(
        if (code[i] == 'l' && i + 2 < n && code[i + 1] == 'l' && code[i + 2] >= 'A' && code[i + 2] <= 'Z')
        {
            size_t j = i + 2;
            while (j < n && isIdentChar(code[j]))
            {
                ++j;
            }
            while (j < n && isSpace(code[j]))
            {
                ++j;
            }
            if (j < n && code[j] == '(')
            {
                return true;
            }
        }
        // default { ... }
        if (code.compare(i, 7, "default") == 0)
        {
            size_t j = i + 7;
            while (j < n && (isSpace(code[j]) || code[j] == '\n' || code[j] == '\r'))
            {
                ++j;
            }
            if (j < n && code[j] == '{')
            {
                return true;
            }
        }
        // state_entry( / touch_start(
        if (code.compare(i, 12, "state_entry(") == 0 || code.compare(i, 12, "touch_start(") == 0)
        {
            return true;
        }
    }
    return false;
}

// static
bool PrestigeMarkdown::isLSLLanguage(const std::string& lang)
{
    return lang == "lsl" || lang == "lsl2" || lang == "secondlife";
}

// static
bool PrestigeMarkdown::isSafeLinkUrl(const std::string& url)
{
    if (!startsWithNoCase(url, "http://") && !startsWithNoCase(url, "https://")
        && !startsWithNoCase(url, "secondlife://"))
    {
        return false;
    }
    for (char c : url)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7f || c == ' ')
        {
            return false;
        }
    }
    return true;
}

// static
bool PrestigeMarkdown::startsWithHeading(const std::string& text)
{
    // Skip a UTF-8 byte order mark if there is one.
    size_t start = 0;
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
    {
        start = 3;
    }
    size_t end = text.find_first_of("\r\n", start);
    if (end == std::string::npos)
    {
        end = text.size();
    }
    std::string content;
    return parseHeading(text.substr(start, end - start), content) > 0;
}
