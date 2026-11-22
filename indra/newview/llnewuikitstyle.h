// #703 / #1242 The new interface's widget kit: theme tokens and the component stylesheet.
//
// Every screen of the new interface is an RML document styled only by this kit's classes
// (bar, btn, field, chip, panel, popover, list / row, kbd, text styles). Colours, radii,
// spacing and type come from a token table, so a theme is a table: dark, light and glass
// today; the classic look (a classic skin and layout over the same kit) is another table plus
// its own layout documents later. The stylesheet is generated from the table at load, so a
// theme switch reloads one stylesheet; documents and controllers stay.
//
// This part has no viewer dependencies (std only): the offscreen lab (indra/rmluilab) links it
// to render the kit's documents. Fonts and settings bindings are in llnewuikit.h.
#pragma once

#include <string>
#include <utility>
#include <vector>

class LLNewUIKit
{
public:
    enum class Theme
    {
        Dark,
        Light
    };
    // Token name -> RCSS value ("surface" -> "#16181dff").
    using Tokens = std::vector<std::pair<std::string, std::string>>;
    static Tokens tokens(Theme theme, bool glass);
    // The kit stylesheet with every $token replaced (unknown tokens are left visible as-is so a
    // missing one shows in review instead of silently defaulting).
    static std::string stylesheet(const Tokens& tokens);
    // The kit's control templates expanded: <ic name="x" [attributes]/> becomes the drawn icon
    // (a span of classes "ic ic-x" plus any class given, the other attributes kept, its parts
    // inside). Icons: x, dl, box, person, mail, win, dock, mouse, shut, sl, more, find, min. The window
    // frame: <whead/> (title bar: {{title}}, minimise, close) and <wgrip/> (resize grip), llnewuiwindow.h.
    static std::string expandTemplates(const std::string& rml);
    // A document with its templates expanded, the kit stylesheet inserted into its <head> and
    // every $token in its own <style> replaced (its own style is layout only).
    static std::string themed(const char* rml, const Tokens& tokens);
    // Registers the kit's faces (Inter, with DejaVu as the fallback for missing glyphs). Viewer
    // only (llnewuikit.cpp).
    static void loadFonts();
    // Text inserted via data-rml: escape markup AND binding delimiters.
    static std::string escapeText(const std::string& text);
};
