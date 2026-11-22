// #703 The new interface without the classic menu bar: the top row (menu bar and status bar) is
// hidden in world, and its functions have new homes (doc/newui-menus.md, the newui-menus check):
//  - every menu item is in the command surface (Ctrl+K), with its keys and enabled / checked state
//    (llcommandindex.h); the Advanced and Develop menus too, while their own toggles show them
//    (Ctrl+Alt+D, Ctrl+Alt+Q; also in the "more" menu);
//  - the menu items' keys keep working: the hidden menu bar stays enabled, and the viewer window
//    offers it every accelerator as before (LLMenuGL passes keys down to hidden items);
//  - the status bar's parts are the new status bar's (llnewuistatus.h);
//  - this file's menu: a small menu under the status bar for what has no other place. "More"
//    (search, preferences, the status bar's parts, the Advanced / Develop toggles, help, exit,
//    and the classic menus to browse, live, item by item), and the L$ balance's actions.
//
// Before login the classic login menus stay (the new interface hides the row only in world).
// Classic (UIRenderer 0 / 1) never runs this.
#pragma once

#include <functional>

namespace Rml
{
class Context;
class ElementDocument;
}

class LLNewUIMenus
{
public:
    using DocumentLoader = std::function<Rml::ElementDocument*(const char* rml, const char* name)>;
    enum class Page
    {
        None,
        More,        // the "more" menu
        Balance,     // the L$ balance's actions
        StatusItems, // the status bar's parts, each shown or not
        Menus        // the classic menus, browsed (a level of gMenuBarView)
    };

    static LLNewUIMenus& instance();
    bool start(Rml::Context* context, const DocumentLoader& load);
    void stop();
    bool active() const;
    bool reload(const DocumentLoader& load);
    bool ownsDocument(const Rml::ElementDocument* doc) const;
    // Opens `page` (a top-level one) with its right edge `right` dp from the window's right edge and
    // its top `top` dp from the window's top; open already, it closes.
    void toggle(Page page, int right, int top);
    void close();
    Page openPage() const; // the top-level page open, None when closed
    // Per frame: the classic row hidden in world; true when the document changed.
    bool sync();

    // The classic top row (menu bar and status bar): hidden (the menu bar kept enabled for its
    // keys) or shown again. LLView only: safe after the RmlUi layer is gone.
    static void hideClassicRow(bool hide);
    static bool classicRowHidden();
};
