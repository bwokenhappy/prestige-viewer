// Tests for indra/newview/lltoastbuttonflow.h: how the buttons of a notification shown in chat / IM
// (LLIMToastNotifyPanel::compactButtons) are flowed into rows once each is shrunk to its label.
// Built and run by scripts/check/checks/toast_button_flow.py.
#include "lltoastbuttonflow.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main()
{
    using namespace lltoastflow;

    // An inventory offer: Accept / Show / Discard / Block, shrunk to their labels, fit one row.
    {
        const std::vector<Slot> s = flow({60, 50, 64, 52}, 305, 5);
        CHECK(s.size() == 4);
        CHECK(rowCount(s) == 1);
        CHECK(s[0].x == 0 && s[1].x == 65 && s[2].x == 120 && s[3].x == 189);
        CHECK(s[0].row == 0 && s[3].row == 0);
    }

    // A button that does not fit starts the next row AT THE LEFT EDGE (the bug: x went on from the
    // row below, so Block sat alone, pushed right).
    {
        const std::vector<Slot> s = flow({120, 120, 120}, 305, 5);
        CHECK(rowCount(s) == 2);
        CHECK(s[0].row == 0 && s[0].x == 0);
        CHECK(s[1].row == 0 && s[1].x == 125);
        CHECK(s[2].row == 1 && s[2].x == 0);
    }

    // Exactly filling the row (no gap after the last) still fits.
    {
        const std::vector<Slot> s = flow({100, 100, 95}, 305, 5);
        CHECK(rowCount(s) == 1 && s[2].x == 210);
    }

    // A button wider than the panel gets a row of its own, at the left edge; never an empty row.
    {
        const std::vector<Slot> s = flow({400, 50}, 305, 5);
        CHECK(rowCount(s) == 2);
        CHECK(s[0].row == 0 && s[0].x == 0);
        CHECK(s[1].row == 1 && s[1].x == 0);
    }

    // No buttons: no rows.
    CHECK(rowCount(flow({}, 305, 5)) == 0);

    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("toast button flow: ok\n");
    return failures ? 1 : 0;
}
