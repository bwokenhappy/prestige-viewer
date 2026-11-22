// #703 The new interface's inventory windows on the window framework (llnewuiwindow.h), several at
// once as the classic inventory allows. The folder tree over the real inventory model
// (LLInventoryRows: open folders, the Library, a flat search per view), the All / Recent / Worn
// views, and the actions through the viewer's own functions: open (the double click's action),
// wear / add / take off / detach (LLAppearanceMgr), delete (the inventory's own checks and
// confirmations, LLInventoryGallery::confirmDelete), properties, a new folder. Items drag onto the
// world, an avatar or a classic window through the classic drag tool (LLToolDragAndDrop).
// The parity list: doc/newui-windows.md.
#include "llviewerprecompiledheaders.h"

#include "llnewuiwindow.h"
#include "llnewuiwindowdocs.h"
#include "llnewuiwindowkinds.h"
#include "llnewuiwinparts.h"

#include "llappearancemgr.h"
#include "llinventoryfunctions.h"
#include "llinventorygallery.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryrows.h"
#include "lltooldraganddrop.h"
#include "llviewerassettype.h"
#include "llviewerinventory.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <algorithm>
#include <memory>

namespace
{
constexpr int   kPool    = 60;
constexpr float kRowDp   = 22.f;
constexpr int   kIndent  = 14;
constexpr S32   kDragMin = 5; // UI units the pointer moves before a press becomes a drag

const char* const kViewNames[] = { "All", "Recent", "Worn" };

struct Row
{
    std::string label, deco = "none", twist; // a decorator is never empty ("decorator: ;" before a row is filled)
    int         indent = 0;
    bool        used = false, selected = false, worn = false;
};

class InventoryWindow final : public LLNewUIWindow
{
public:
    InventoryWindow()
    {
        mViews.assign(std::begin(kViewNames), std::end(kViewNames));
        mRows.resize(kPool);
        mTree = std::make_unique<LLInventoryRows>();
    }
    const char* rml() const override { return LLNewUIWindowDocs::kInventoryRml; }
    const char* model() const override { return "inv"; }

    bool bind(Rml::DataModelConstructor& m) override
    {
        if (auto r = m.RegisterStruct<Row>())
        {
            r.RegisterMember("label", &Row::label);
            r.RegisterMember("deco", &Row::deco);
            r.RegisterMember("twist", &Row::twist);
            r.RegisterMember("indent", &Row::indent);
            r.RegisterMember("used", &Row::used);
            r.RegisterMember("selected", &Row::selected);
            r.RegisterMember("worn", &Row::worn);
        }
        m.RegisterArray<std::vector<Row>>();
        m.RegisterArray<std::vector<std::string>>();
        llnewuiRegisterItems(m);
        m.Bind("views", &mViews);
        m.Bind("view", &mView);
        m.Bind("filter", &mFilter);
        m.Bind("rows", &mRows);
        m.Bind("count", &mScroll.count);
        m.Bind("status", &mStatus);
        m.Bind("emptyText", &mEmptyText);
        m.Bind("thumbTop", &mScroll.thumbTop);
        m.Bind("thumbH", &mScroll.thumbH);
        m.Bind("acts", &mActs);
        m.Bind("menu", &mMenu);
        m.Bind("menuOpen", &mMenuOpen);
        m.Bind("menuX", &mMenuX);
        m.Bind("menuY", &mMenuY);
        m.BindEventCallback("setView", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            { if (!a.empty()) setView(a[0].Get<int>()); });
        m.BindEventCallback("scroll", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                mScroll.first += a[0].Get<float>() > 0 ? 3 : -3;
                                fill();
                            });
        m.BindEventCallback("press", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                select(mScroll.first + a[0].Get<int>());
                                if (buttonOf(e) == 1)
                                    openMenu(&e);
                            });
        m.BindEventCallback("twist", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                const int i = mScroll.first + a[0].Get<int>();
                                const auto& rows = mTree->rows();
                                if (i >= 0 && i < int(rows.size()) && rows[size_t(i)].folder)
                                {
                                    mTree->toggleFolder(rows[size_t(i)].id);
                                    e.StopPropagation();
                                }
                            });
        m.BindEventCallback("activate", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                const int i = mScroll.first + a[0].Get<int>();
                                const auto& rows = mTree->rows();
                                if (i < 0 || i >= int(rows.size())) return;
                                if (rows[size_t(i)].folder)
                                    mTree->toggleFolder(rows[size_t(i)].id);
                                else
                                    LLInventoryRows::activate(rows[size_t(i)].id);
                            });
        m.BindEventCallback("act", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                const size_t i = a.empty() ? mActs.size() : size_t(a[0].Get<int>());
                                if (i < mActs.size() && mActs[i].on)
                                    run(mActs[i].id);
                            });
        m.BindEventCallback("more", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&) { openMenu(&e); });
        m.BindEventCallback("menuRun", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                const size_t i = a.empty() ? mMenu.size() : size_t(a[0].Get<int>());
                                if (i < mMenu.size() && mMenu[i].on)
                                {
                                    const std::string id = mMenu[i].id;
                                    closeMenu();
                                    run(id);
                                }
                            });
        m.BindEventCallback("bodyDown", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&)
                            {
                                if (!mMenuOpen) return;
                                for (Rml::Element* t = e.GetTargetElement(); t; t = t->GetParentNode())
                                    if (t->GetId() == "menu" || (buttonOf(e) == 1 && t->IsClassSet("vrow")) || t->IsClassSet("btn"))
                                        return;
                                closeMenu();
                            });
        m.BindEventCallback("newWindow", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { LLNewUIWindows::instance().open("inventory", std::string(), true); });
        return true;
    }

    bool sync(F64 now) override
    {
        const int slots = visibleRows("list", kRowDp, kPool);
        bool      again = slots != mScroll.fit;
        if (mFilter != mLastFilter)
        {
            mLastFilter = mFilter;
            mTree->setFilter(mFilter);
            mScroll.first = 0;
        }
        // Rebuilt on inventory / folder / filter changes only (the model's observer), at most 4x a
        // second while it changes (a big inventory loading).
        if (now >= mNext && mTree->refresh())
        {
            mNext = now + 0.25;
            again = true;
        }
        if (again)
        {
            mScroll.fit = slots;
            fill();
        }
        return false;
    }

    bool onKey(KEY key, MASK mask) override
    {
        if (mask != MASK_NONE)
            return false;
        const auto& rows = mTree->rows();
        const int   sel  = selectedIndex();
        switch (key)
        {
            case KEY_UP: select(std::max(0, sel - 1)); return true;
            case KEY_DOWN: select(std::min(int(rows.size()) - 1, sel + 1)); return true;
            case KEY_PAGE_UP: select(std::max(0, sel - mScroll.fit)); return true;
            case KEY_PAGE_DOWN: select(std::min(int(rows.size()) - 1, sel + mScroll.fit)); return true;
            case KEY_LEFT:
            case KEY_RIGHT:
                // Close / open the selected folder, as the classic tree.
                if (sel >= 0 && rows[size_t(sel)].folder && rows[size_t(sel)].open == (key == KEY_LEFT))
                    mTree->toggleFolder(rows[size_t(sel)].id);
                return sel >= 0;
            case KEY_RETURN:
                if (sel >= 0)
                {
                    if (rows[size_t(sel)].folder)
                        mTree->toggleFolder(rows[size_t(sel)].id);
                    else
                        LLInventoryRows::activate(rows[size_t(sel)].id);
                }
                return sel >= 0;
            case KEY_DELETE:
                if (sel >= 0)
                    run("delete");
                return sel >= 0;
            default: return false;
        }
    }
    bool onFieldKey(Rml::Element*, KEY key, MASK mask) override
    {
        if (key == KEY_DOWN || key == KEY_UP)
            return onKey(key, mask);
        return false;
    }

    // ---- dragging an item out (onto the world, an avatar, a classic window) ----
    bool wantsDrag(Rml::Element* hover) override
    {
        for (Rml::Element* e = hover; e; e = e->GetParentNode())
            if (e->IsClassSet("vrow"))
            {
                // its slot: its place among the row slots
                int slot = 0;
                for (Rml::Element* s = e->GetPreviousSibling(); s; s = s->GetPreviousSibling())
                    slot += s->IsClassSet("vrow") ? 1 : 0;
                mDragRow     = mScroll.first + slot;
                mDragStarted = false;
                return true;
            }
        return false;
    }
    void dragMove(S32 dx, S32 dy) override
    {
        if (mDragStarted || mDragRow < 0 || std::abs(dx) + std::abs(dy) < kDragMin)
            return;
        const auto& rows = mTree->rows();
        if (mDragRow >= int(rows.size()))
            return;
        mDragStarted     = true;
        const LLUUID& id = rows[size_t(mDragRow)].id;
        const LLUUID& lib = gInventory.getLibraryRootFolderID();
        const LLToolDragAndDrop::ESource source =
            gInventory.isObjectDescendentOf(id, lib) ? LLToolDragAndDrop::SOURCE_LIBRARY : LLToolDragAndDrop::SOURCE_AGENT;
        if (gInventory.getCategory(id))
            LLToolDragAndDrop::getInstance()->beginDrag(DAD_CATEGORY, id, source);
        else if (LLViewerInventoryItem* item = gInventory.getItem(id))
            LLToolDragAndDrop::getInstance()->beginDrag(LLViewerAssetType::lookupDragAndDropType(item->getType()), id, source);
        // The drag tool takes the mouse from here (the framework's drag ends with its capture).
    }
    void dragEnd() override
    {
        mDragRow     = -1;
        mDragStarted = false;
    }

    bool onTest(const LLSD& args) override
    {
        if (args.has("view"))
            setView(args["view"].asInteger());
        if (args.has("filter"))
        {
            mFilter = args["filter"].asString();
            dirty("filter");
            mLastFilter = mFilter;
            mTree->setFilter(mFilter);
            mScroll.first = 0;
        }
        mTree->refresh();
        if (args.has("toggle"))
        {
            const auto& rows = mTree->rows();
            const size_t i   = size_t(std::max(0, args["toggle"].asInteger()));
            if (i < rows.size() && rows[i].folder)
                mTree->toggleFolder(rows[i].id);
            mTree->refresh();
        }
        if (args.has("select"))
            select(args["select"].asInteger());
        if (args["menu"].asBoolean())
            openMenu(nullptr);
        fill();
        return true;
    }

private:
    void setView(int v)
    {
        v = std::clamp(v, 0, 2);
        if (v == mView)
            return;
        mView = v;
        mTree->setMode(v == 0 ? LLInventoryRows::Mode::All : v == 1 ? LLInventoryRows::Mode::Recent : LLInventoryRows::Mode::Worn);
        mScroll.first = 0;
        closeMenu();
        dirty("view");
        mNext = 0;
    }

    int selectedIndex() const
    {
        if (mSelected.isNull())
            return -1;
        const auto& rows = mTree->rows();
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].id == mSelected)
                return int(i);
        return -1;
    }

    void select(int i)
    {
        const auto& rows = mTree->rows();
        if (i < 0 || i >= int(rows.size()))
            return;
        mSelected = rows[size_t(i)].id;
        mScroll.reveal(i);
        fill();
    }

    void fill()
    {
        const auto& rows = mTree->rows();
        if (mScroll.update(int(rows.size()), mScroll.fit))
        {
            dirty("thumbTop");
            dirty("thumbH");
        }
        const bool flat = mTree->mode() != LLInventoryRows::Mode::All || !mFilter.empty();
        for (int i = 0; i < kPool; ++i)
        {
            Row&       r   = mRows[size_t(i)];
            const int  idx = mScroll.first + i;
            const bool use = i < mScroll.fit && idx < mScroll.count;
            r              = Row();
            r.used         = use;
            r.deco         = "none"; // never an empty llimage() (the instancer refuses it)
            if (!use)
                continue;
            const LLInventoryRows::Row& src = rows[size_t(idx)];
            r.label    = flat && !src.where.empty() ? src.name + "  -  " + src.where : src.name;
            r.deco     = src.icon.empty() ? std::string("none") : "llimage(" + src.icon + ")";
            r.indent   = src.depth * kIndent;
            r.twist    = src.folder ? (src.open ? "open" : "closed") : "";
            r.selected = src.id == mSelected;
            r.worn     = src.worn;
        }
        dirty("rows");
        dirty("count");
        std::string status;
        if (gInventory.isInventoryUsable() && !LLInventoryModelBackgroundFetch::instance().isEverythingFetched())
            status = "Fetching items...  ";
        if (flat)
            status += llformat("%d found", mScroll.count);
        else
            status += llformat("%d rows", mScroll.count);
        if (status != mStatus)
        {
            mStatus = status;
            dirty("status");
        }
        const std::string empty = !mFilter.empty() ? "No matches." : mView == 1 ? "Nothing new since you last logged off." : mView == 2 ? "Nothing worn." : "Loading...";
        if (empty != mEmptyText)
        {
            mEmptyText = empty;
            dirty("emptyText");
        }
        updateActions();
    }

    // What the selection can do.
    struct Can
    {
        bool sel = false, folder = false, wearable = false, worn = false, removable = false, library = false;
    };
    Can can() const
    {
        Can c;
        if (mSelected.isNull())
            return c;
        c.sel     = true;
        c.library = gInventory.isObjectDescendentOf(mSelected, gInventory.getLibraryRootFolderID());
        if (gInventory.getCategory(mSelected))
        {
            c.folder    = true;
            c.wearable  = true;
            c.worn      = get_is_parent_to_worn_item(mSelected);
            c.removable = !c.library && get_is_category_removable(&gInventory, mSelected);
        }
        else
        {
            c.wearable  = get_can_item_be_worn(mSelected);
            c.worn      = get_is_item_worn(mSelected);
            c.removable = !c.library && get_is_item_removable(&gInventory, mSelected, false);
        }
        return c;
    }

    void updateActions()
    {
        const Can c = can();
        std::vector<LLNewUIItem> acts;
        if (c.folder)
            acts = { { "Wear", "replace", c.sel }, { "Add", "add", c.sel }, { "Take off", "takeoff", c.worn } };
        else if (c.wearable && c.worn)
            acts = { { "Take off", "takeoff", true } };
        else if (c.wearable)
            acts = { { "Wear", "replace", true }, { "Add", "add", true } };
        else
            acts = { { "Open", "open", c.sel } };
        acts.push_back({ "Delete", "delete", c.removable });
        if (acts != mActs)
        {
            mActs = acts;
            dirty("acts");
        }
    }

    void openMenu(const Rml::Event* event)
    {
        const Can c = can();
        if (!c.sel)
            return;
        mMenu.clear();
        if (c.folder)
        {
            mMenu.push_back({ isOpenFolder() ? "Close folder" : "Open folder", "toggle", true });
            mMenu.push_back({ "Replace outfit", "replace", true });
            mMenu.push_back({ "Add to outfit", "add", true });
            mMenu.push_back({ "Take off items", "takeoff", c.worn });
            mMenu.push_back({ "New folder", "newfolder", !c.library });
        }
        else
        {
            mMenu.push_back({ "Open", "open", true });
            if (c.wearable)
            {
                mMenu.push_back({ "Wear", "replace", !c.worn });
                mMenu.push_back({ "Add", "add", !c.worn });
                mMenu.push_back({ c.worn ? "Take off / Detach" : "Take off", "takeoff", c.worn });
            }
            mMenu.push_back({ "Properties", "properties", true });
        }
        mMenu.push_back({ "New window", "newwindow", true });
        mMenu.push_back({ "Delete", "delete", c.removable });
        float x = 140, y = 100, w = 1000, h = 1000; // no pointer (tests): near the list's top
        if (event)
            pointerIn(*event, "ibody", x, y, w, h);
        llnewuiPlaceMenu(x, y, w, h, mMenu.size(), mMenuX, mMenuY);
        mMenuOpen = true;
        for (const char* v : { "menu", "menuOpen", "menuX", "menuY" })
            dirty(v);
    }
    bool isOpenFolder() const { return mTree->isOpen(mSelected); }
    void closeMenu()
    {
        if (!mMenuOpen)
            return;
        mMenuOpen = false;
        dirty("menuOpen");
    }

    void run(const std::string& id)
    {
        const LLUUID sel = mSelected;
        if (id == "newwindow")
        {
            LLNewUIWindows::instance().open("inventory", std::string(), true);
            return;
        }
        if (sel.isNull())
            return;
        LLViewerInventoryCategory* cat = gInventory.getCategory(sel);
        if (id == "toggle" && cat)
            mTree->toggleFolder(sel);
        else if (id == "open")
            LLInventoryRows::activate(sel);
        else if (id == "replace" || id == "add")
        {
            const bool append = id == "add";
            if (cat)
                LLAppearanceMgr::instance().wearInventoryCategory(cat, false, append);
            else
                LLAppearanceMgr::instance().wearItemOnAvatar(sel, true, !append);
        }
        else if (id == "takeoff")
        {
            if (cat)
                LLAppearanceMgr::instance().takeOffOutfit(sel);
            else
                LLAppearanceMgr::instance().removeItemFromAvatar(sel);
        }
        else if (id == "properties")
            show_item_profile(sel);
        else if (id == "newfolder" && cat)
        {
            if (!mTree->isOpen(sel))
                mTree->toggleFolder(sel);
            gInventory.createNewCategory(sel, LLFolderType::FT_NONE, "New Folder");
        }
        else if (id == "delete")
            LLInventoryGallery::confirmDelete({ sel }); // the inventory's own checks and confirmations
        mNext = 0;
    }

    std::vector<std::string>         mViews;
    int                              mView = 0;
    std::string                      mFilter, mLastFilter, mStatus, mEmptyText = "Loading...";
    std::vector<Row>                 mRows;
    std::unique_ptr<LLInventoryRows> mTree;
    LLNewUIScroll                    mScroll;
    LLUUID                           mSelected;
    std::vector<LLNewUIItem>         mActs, mMenu;
    bool                             mMenuOpen = false;
    float                            mMenuX = 0, mMenuY = 0;
    F64                              mNext = 0;
    int                              mDragRow = -1;
    bool                             mDragStarted = false;
};
} // namespace

void slNewUIRegisterInventory(LLNewUIWindows& windows)
{
    LLNewUIWindows::Kind k;
    k.kind             = "inventory";
    k.title            = "Inventory";
    k.place            = { -12.f, 56.f, 360.f, 560.f }; // top right, under the bar
    k.minW             = 280.f;
    k.minH             = 260.f;
    k.multi            = true;
    k.restoreInstances = true;
    k.make             = [](const std::string&) -> std::unique_ptr<LLNewUIWindow> { return std::make_unique<InventoryWindow>(); };
    windows.registerKind(k);
}
