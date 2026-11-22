// #703 Parts the native list windows share (People, Inventory): a virtualised list (a fixed pool of
// row slots shows the visible part of a list of any length), its scroll thumb, the action buttons
// and the context menu. Viewer side (RmlUi); each window binds them into its own model.
#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <algorithm>
#include <string>
#include <vector>

// An action button or a menu row: what it says, whether it can run now, what it runs.
struct LLNewUIItem
{
    std::string label, id;
    bool        on = true;
    bool operator==(const LLNewUIItem& o) const { return label == o.label && id == o.id && on == o.on; }
};

// The visible window of a list: `first` row shown, `fit` slots in use, the thumb in % of the track.
struct LLNewUIScroll
{
    int   first = 0, fit = 20, count = 0;
    float thumbTop = 0.f, thumbH = 0.f;

    // Clamped to the list; true when the thumb moved.
    bool update(int listCount, int slots)
    {
        count = listCount;
        fit   = std::max(1, slots);
        first = std::clamp(first, 0, std::max(0, count - fit));
        float top = 0.f, h = 0.f;
        if (count > fit)
        {
            h   = std::max(8.f, 100.f * float(fit) / float(count));
            top = (100.f - h) * float(first) / float(std::max(1, count - fit));
        }
        const bool moved = top != thumbTop || h != thumbH;
        thumbTop         = top;
        thumbH           = h;
        return moved;
    }
    // Brings row `i` into view.
    void reveal(int i)
    {
        if (i < first)
            first = i;
        else if (i >= first + fit)
            first = i - fit + 1;
    }
};

// Registers LLNewUIItem as a struct and its array on a model (once per model).
inline void llnewuiRegisterItems(Rml::DataModelConstructor& m)
{
    if (auto s = m.RegisterStruct<LLNewUIItem>())
    {
        s.RegisterMember("label", &LLNewUIItem::label);
        s.RegisterMember("on", &LLNewUIItem::on);
    }
    m.RegisterArray<std::vector<LLNewUIItem>>();
}

// The context menu's place in the window body (dp): at the pointer, kept inside the body.
inline void llnewuiPlaceMenu(float x, float y, float bodyW, float bodyH, size_t rows, float& outX, float& outY)
{
    const float w = 190.f, h = 8.f + 26.f * float(rows);
    outX          = std::clamp(x, 0.f, std::max(0.f, bodyW - w));
    outY          = y + h > bodyH ? std::max(0.f, y - h) : y;
}
