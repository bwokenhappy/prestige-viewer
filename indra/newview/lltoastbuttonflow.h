/**
 * @file lltoastbuttonflow.h
 * @brief Flows a notification's buttons into rows once each is shrunk to its label (chat / IM).
 *
 * LLIMToastNotifyPanel::compactButtons shrinks the buttons a toast laid out at a fixed width; they are then
 * placed left to right, a new row only when the next one does not fit, each row from the left edge.
 * Pure: standard headers only (host-tested, scripts/check/test_toast_button_flow.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */
#ifndef LL_LLTOASTBUTTONFLOW_H
#define LL_LLTOASTBUTTONFLOW_H

#include <vector>

namespace lltoastflow
{
struct Slot
{
    int x;     // left edge in the button panel
    int row;   // 0 = the bottom row (the toast lays its buttons out from the bottom up)
};

// Each button in order: on the current row when it fits after the previous one and `gap`, else at the left edge
// of the next row. A button wider than the panel still gets a row of its own (never an empty row).
inline std::vector<Slot> flow(const std::vector<int>& widths, int panelWidth, int gap)
{
    std::vector<Slot> out;
    out.reserve(widths.size());
    int x = 0, row = 0;
    for (const int w : widths)
    {
        if (x > 0 && x + w > panelWidth)
        {
            x = 0;
            ++row;
        }
        out.push_back(Slot{x, row});
        x += w + gap;
    }
    return out;
}

inline int rowCount(const std::vector<Slot>& slots)
{
    return slots.empty() ? 0 : slots.back().row + 1;
}
} // namespace lltoastflow

#endif // LL_LLTOASTBUTTONFLOW_H
