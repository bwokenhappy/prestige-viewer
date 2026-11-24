/**
 * @file prestigemarkdownrenderer.h
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

#ifndef PRESTIGE_MARKDOWNRENDERER_H
#define PRESTIGE_MARKDOWNRENDERER_H

#include <string>

class LLTextEditor;

class PrestigeMarkdownRenderer
{
public:
    // Replaces the contents of 'target' (meant to be read-only, with URL
    // parsing on) with the formatted rendering of the Markdown source.
    // Embedded notecard items are shown by name. When keep_scroll is set the
    // view tries to stay where it was scrolled to (live preview).
    static void render(LLTextEditor* target, const std::string& markdown, bool keep_scroll);
};

#endif // PRESTIGE_MARKDOWNRENDERER_H
