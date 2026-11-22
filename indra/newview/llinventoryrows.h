// #703 The inventory as a flat list of visible rows, UI-independent: a folder tree (open folders
// expanded, folders first, by name; My Inventory, then the Library) or, with a filter or in the
// Recent / Worn views, the matching items as a flat list with the folder each is in. Any view
// virtualises over it (the new interface materialises only the rows on screen from a fixed pool).
// Rebuilt only when the inventory or the open folders / filter / view change; bounded (kMaxRows).
#pragma once

#include "llinventoryobserver.h"
#include "lluuid.h"
#include <set>
#include <string>
#include <vector>

class LLInventoryRows : public LLInventoryObserver
{
public:
    struct Row
    {
        LLUUID      id;
        std::string name;
        std::string icon;  // skin image name (LLInventoryIcon)
        std::string where; // the folder it is in (flat lists: search, recent, worn)
        int         depth  = 0;
        bool        folder = false, open = false, worn = false;
    };
    // All: the tree (a filter makes it a flat search); Recent: items made or received since the last
    // logoff (the classic Recent tab); Worn: what is worn and attached.
    enum class Mode
    {
        All,
        Recent,
        Worn
    };
    static constexpr size_t kMaxRows = 20000;

    LLInventoryRows();
    ~LLInventoryRows() override;
    void changed(U32 mask) override { mDirty = true; }

    // Rebuilds when needed; true when the rows changed.
    bool refresh();
    const std::vector<Row>& rows() const { return mRows; }

    void toggleFolder(const LLUUID& id);
    bool isOpen(const LLUUID& id) const { return mOpen.count(id) != 0; }
    void setFilter(const std::string& filter);
    const std::string& filter() const { return mFilter; }
    void setMode(Mode mode);
    Mode mode() const { return mMode; }
    // Opens / wears / rezzes it as the inventory does on a double click.
    static void activate(const LLUUID& id);

private:
    void addFolder(const LLUUID& id, int depth);
    void addMatches(const LLUUID& id, const std::string& folded, int depth);
    void addFolderRow(const LLUUID& id, int depth);

    std::vector<Row> mRows;
    std::set<LLUUID> mOpen;
    std::string      mFilter;
    Mode             mMode  = Mode::All;
    bool             mDirty = true;
};
