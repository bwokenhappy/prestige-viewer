/**
 * @file llvrwristgrid.h
 * @brief VR wrist panel (#1072): how the quick action buttons are spread over rows. Pure, so
 *        scripts/check/test_vr_wrist_grid.cpp checks it: no button count needs a hand-placed
 *        coordinate, and no row is wider than the layout's column limit.
 */
#pragma once

#include <vector>

namespace llvrwrist
{
// Buttons per row for `count` buttons with at most `columns` per row: as few rows as that allows,
// the buttons spread evenly over them (7 with 3 per row: 3, 2, 2; the longer rows first).
inline std::vector<int> gridRows(int count, int columns)
{
    std::vector<int> out;
    if (count <= 0) return out;
    if (columns < 1) columns = 1;
    const int rows = (count + columns - 1) / columns;
    for (int r = 0; r < rows; ++r) out.push_back(count / rows + (r < count % rows ? 1 : 0));
    return out;
}
} // namespace llvrwrist
