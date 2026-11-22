# Native Conversations clipping and title drag

Production base: mega-1106 `e6b11fda9a`. Branch `fix-newui-conversations`.
Build workspace: owned `agents/worktrees/gpu-timer-scopes` (reused after the timer task finished; no foreign worktree/build/package edited). Initial edits were made in an isolated worktree of the branch before transferring the committed branch to this idle owned build workspace.

## Cause and repair

- **Spilled history:** `llnewuiwindowdocs.h:25` bottom-anchors `#msgs` in the square `#history.viewport`. In RmlUi6.3, `ElementUtilities::GetClippingRegion` only clips ordinary `overflow:hidden` if client width/height is smaller than scroll width/height. Content positioned above the viewport's top does not increase its scroll height. In the CPU reproduction, the viewport had client/scroll height313px while the messages started atY−1307px: there was no clip region. The screenshot was inspected privately; its text is not included here.
- **Title drag blocked:** this same RmlUi clipping calculation is used by `Context::GetElementAtPoint`. A message outside the viewport won the title-bar hit test. The framework (`llnewuiwindow.cpp:732–768`) therefore never saw a `whead` ancestor and did not begin a move. The framework's move/resize logic itself did not need changing.
- **Fix:** the shared square `.viewport` (`llnewuikitstyle.cpp:354`) now uses `overflow:hidden; clip:always`, forcing its rectangular clip for rendering and hit-testing, including negative overflow. No clip is added to the rounded window, and popovers remain free to extend outside the window body. The flex frame/body/viewport can shrink to their assigned height with `min-height:0`.
- **Scrolling:** keeps the existing last-message-at-bottom and80-materialised-message bound. Existing wheel and page-key history offsets remain; a visible scrollbar (`llnewuiwindowdocs.h:53`, `llnewuichat.cpp:155–229`) adds thumb dragging and track paging through the retained source history. The thumb is an estimate based on the bounded batch's measured wrapped height; scrolling addresses whole messages, not individual text pixels. Empty/short histories hide it. Thumb motion uses the framework's existing owned-drag capture/release route, with clamped source offsets and frozen drag geometry.

## Other windows and classic Conversations

Conversations IM, Nearby chat and torn-off chats all use the same history document/controller. People and Inventory use the same `.viewport`, so their rows also receive the forced rectangular draw/hit clip. CPU coverage includes both negative- and positive-overflow layouts; no change to their row/action controllers.

Classic restoration is unchanged. No changes to classic Conversations visibility, saved state, login restoration, sends or keyboard-focus policy. `llnewui.cpp` routes a native-window pointer press once and consumes it; classic open menus retain their existing priority, and the active text-field focus follows the existing framework. Both presentations share the existing chat/IM models; this fix adds no second send path. This is source verification, not a live two-window focus test.

## Validation

- CPU real-RmlUi6.3 fixture `newui_clipping_checks`: reproduces old missing clip/title theft, then checks forced clip bounds and title hit after move/resize; dark/light themes, UI density1.0/1.1/1.5,500 synthetic rows. No GPU/Vulkan/viewer/account/private chat. Direct host compile `-Wall -Wextra -Werror` and run PASS.
- `newui-kit` source guard requires `overflow:hidden; clip:always` on `.viewport`, and the history scrollbar markup. The existing newui-window fixture now tests scrollbar endpoints, monotonicity, no overscroll, empty/single/short sources and zero-height tracks.
- Full source checks: PASS132.
- Full configured Linux build/link/package PASS on production `faa85315af` (the normal Make/CMake/Ninja dependency build, not syntax-only). Rebuilt the changed Conversations/People/Inventory/kit objects and58 incremental build steps after shader generation. Repository warning policy retained, including its existing exceptions; no warning-free claim.
- Package checks PASS4; two inherited host-libdecor ISA portability warnings.
- Configured `newui_clipping_checks` target build+run PASS (Release assertions-independent checks), in addition to the strict direct host compile/run above.
- `git diff --check` PASS; read-only merge-tree check against mega PASS.
- Build receipts: `/home/bryan/.cache/slipstream-build-tmp/newui-conversations/linux-build.log`, `cpu-target-build.log`, `cpu-target-run.log`.
- No owner viewer or package touched; no box runs; no Windows/platform code changes.

Optional CPU target after configure: `cmake --build build-linux-x86_64 --target newui_clipping_checks -j4`, then `build-linux-x86_64/newview/newui_clipping_checks indra/newview/fontsets/Inter-Regular.ttf`.

## Owner check after integration

Open native Conversations beside the restored classic floater. On IM and Nearby tabs with long history, wheel up/down and drag/page the history thumb; newest starts at the bottom. Drag the native title bar with history behind it, then resize. Repeat title/resize/row-hit checks on People and Inventory. Verify typing goes only to the clicked field and that classic remains open. No in-world visual run is claimed by this report.
