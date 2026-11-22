# Native UI top bar, Nearby, picker and glyph fallback

Status: **READY**. All required compile, fixture, render, source and package gates passed. Production source `64479348e930aadee7b22a9c39258834b180bdc7`, branch `newui-topbar-fix`, worktree `agents/worktrees/newui-topbar-fix`, based on `eaef983328`. Actual configured validation used Codex's owned `agents/worktrees/gpu-timer-scopes` source/build, detached at that exact source. Framegen's completed archive and receipts were sealed separately before reuse. No Claude worktree/build writes, viewer, hardware GPU, box run, push, or generated SPIR-V-header commit.

## Causes and repairs

1. Top bar: block `.btn` children were added inside an inline-block bar. The explicit flex row keeps actions, Home/Favorites, location and search together. Upload, Snapshot and People are visible on wide screens; narrow layouts collapse actions into overflow. Shipped Phosphor-backed skin images replace Unicode Home/Star. A desktop overflow specificity defect found in the first real render is fixed too.
2. Nearby: the variable-length Unicode status string moved every following column. Eleven fixed readout slots (empty when off), real skin icons with tooltips only on active readouts, fixed IM and right-aligned monospaced distance columns replace it. Name/note privacy conditions remain. The fixture uses the production 380-dp window width.
3. Conversation picker: a full-width search field, separate Friends/Groups headings, icons/names/statuses, bounded 16+16 results, content-sized scrolling popover below the tabs/layout controls, Up/Down/Enter/Esc, selected-row scrolling confined to the popup, wheel propagation containment, deferred input focus and outside/window-deactivation dismissal. Closing within the chat window restores compose focus; the global focus bridge respects classic/modal ownership. Its parallel arrays publish as one snapshot; OOM retains the prior usable list. Names arriving from cache refresh while open without the chat message timer adding latency.
4. Glyphs: `llrmluihost.cpp` loads DejaVu as an ordinary font before the kit requests it as fallback. Pinned RmlUi 6.3 `FontFamily::AddFace` returned `Duplicate/null`; `FontProvider::AddFace` returned before fallback registration, although the public loader reported success. Existing faces can now be promoted into the fallback list. The kit also uses classic's selected font/default files, platform font paths and the same `LLWindow::findFallbackFontForChar` resolver for missing system glyphs. Face tracking is deduplicated, not a new per-message/per-character cache. The dependency hook is hash-pinned and idempotent.

## Validation

- Actual configured `-Werror` production Linux objects PASS on `654506401b` (the final follow-up only moves readout tooltip metadata): `llnewui.cpp`, `llnewuichat.cpp`, `llnewuikit.cpp`, `llnewuikitstyle.cpp`, `llnewuipeople.cpp`, and `llfontgl.cpp`.
- Five CPU fixtures PASS: real font-engine host-preload-order regression; actual classic SVG emoji renderer; production RmlUi clipping/markup; immediate Nearby publication/ring wrap; real classic URL markup/privacy boundaries. The glyph fixture requires eager fallback metrics == lazy fallback metrics and a different broken baseline.
- Actual production kit RML/styles/fonts and exported skin icons rendered with the official RmlUi GL3 backend on **forced Mesa llvmpipe**, verified by renderer string. No hardware GPU context. Geometry assertions check bar order/fit, fixed distance alignment, full-width picker input and controls remaining above the popover. Missing PNG textures fail the renderer rather than silently drawing white.
- 52 required images rendered at 1920x1080 and 3840x2160, each scale 1/1.5. All inspected through lossless labelled crop sheets. Eight grouped-chat images were re-rendered/re-inspected after adding timestamp spacing; the remaining 44 are byte-identical to the already inspected versions (`inspection/spacing-diff.json`).
- Broad test string: `ᴀʟᴛ ꜱᴏᴜɴᴅ`, Latin Extended A–D plus Additional, Greek, Cyrillic, CJK, Arabic/Hebrew, box drawing, math and emoji/ZWJ/skin-tone codepoints. All 64 nominal codepoints have font coverage; real engine/render tests cover the actual glyph output. Existing shaping behavior is preserved.

- Full actual Linux viewer build, link and archive/package PASS on final source `64479348e9`; the preceding full rebuild also passed on `654506401b`.
- Package checks: 4 PASS. The existing libdecor ISA check emits two warnings, not errors.
- Six actual configured Windows cross objects PASS on final source: the same five viewer TUs plus `llfontgl.cpp`. Commands are unchanged from the repository's standard cross-build configuration. An additional `/WX` attempt stopped on baseline constructor-order/missing-override header warnings and redundant driver flags; **no strict-Windows-pass claim**. This is not a full Windows viewer link/package.
- Full source checks: 133 PASS on final source `64479348e9` (also passed on `654506401b`).
- Builds ran at nice19/-j4. Claude explicitly granted UI priority despite the existing cold full builds. No foreign process was stopped or reprioritized.

Earlier harness/API attempts and missing-icon-path renders are retained in receipts. The actual render asset loader now fails on a missing PNG. The full Linux build was paused to prioritize final changed-object checks and then completed; final source built and packaged successfully.

## Images inspected

Every row below includes all four required resolution/scale variants:

| Sheet under `agents/newui-topbar-renders/inspection/` | Inspection |
|---|---|
| `newui-main-dark-on-dark.png` | Single centered row; real action/Home/Star icons; visible action labels; no desktop overflow; search/location remain separate readable fields. |
| `newui-nearby-dark.png`, `newui-nearby-light.png` | All seven fake names, fixed IM/status slots, decimal metre values/right edges, footers and range control fit the production-width window. |
| `newui-chat-dark.png`, `newui-chat-light.png` | IRC history/compose layout, broad glyph line, autocomplete, tabs and layout controls remain usable. |
| `newui-chat-grouped-dark.png`, `newui-chat-grouped-light.png` | Grouped sender/time spacing corrected; oldest rows clip at history boundary without covering controls; compose remains reachable. |
| `newui-picker-0-dark.png`, `newui-picker-0-light.png` | Empty query: search placeholder, Friends and Groups, online/offline/group statuses, selected row and search-all action. |
| `newui-picker-1-dark.png`, `newui-picker-1-light.png` | Typed query stays visible; filtered friend results; shorter popover; no stray group heading or control overlap. |
| `newui-picker-2-dark.png`, `newui-picker-2-light.png` | No results message and search-all action; compact content-sized popover; no blank giant panel. |

Full PNG manifest: `agents/newui-topbar-renders/inspection/required-images.txt`. Top-bar aliases requested by the brief: `agents/newui_topbar_1920x1080_1.png`, `agents/newui_topbar_1920x1080_1.5.png`, `agents/newui_topbar_3840x2160_1.png`, `agents/newui_topbar_3840x2160_1.5.png`. Supplemental 640-wide top-bar inspection confirms compact overflow; it is not a window-manager/mobile-layout claim. The offline People map rectangle is an intentional placeholder for the viewer's classic map overlay. All sample names/chat are invented.

## Reproduction and receipts

Scratch directory `/home/bryan/.cache/slipstream-build-tmp/gpu-timer-scopes/`: `newui_build_fixtures.py`, `newui_changed_objects.py`, `newui_fixture_run.py`, `newui_render_software.py`; actual viewer compile commands in `newui-linux-actual-objects.json`; CPU fixture argv/results in `newui-fixture-results.json`; render argv/source and llvmpipe logs under `agents/newui-topbar-renders/`. Build target `newui_software_render` uses the official pinned RmlUi backend; icon PNGs come from `scripts/ui/newui_lab_icons.py`, not mock artwork. The old `newui-fixtures-build.log` is admission-only; priority-granted logs start `newui-fixtures-priority-`.

Pending owner interaction check after READY: Upload/Home/Favorites and location navigation; Nearby row IM/double-click Zoom/context menu/tooltips; picker typing/Up/Down/Enter/Esc and outside clicks; compare broad Unicode chat against classic. No in-world interaction/performance claim yet.

Latest input-only changes were rerendered on `654506401b`, then final tooltip metadata on `64479348e9`: all 13 accepted sheets / 52 required windows remain pixel-identical. Receipts: `inspection/final-input-pixel-identity.json`, `inspection/final-tooltip-pixel-identity.json`, and `inspection/final-png-sha256.json`.

Final build receipts under the scratch directory: `newui-linux-complete.log` (full rebuild), `newui-final-linux.log` (final revision full target/link/package), `newui-final-fixture-build.log`, `newui-final-fixtures.log`, `newui-final-renders.log`, `newui-final-package.log`, `newui-windows-configured.log`, `newui-windows-configured-objects.json`, `newui-source-tip.log`. Failed optional strict Windows attempts are preserved in `newui-windows-objects*.log`; the standard configured six-object pass is `newui-windows-configured.log`.

Integration: apply the local commit range `eaef983328..newui-topbar-fix` (production tip `64479348e9`, followed only by the report commit). It builds on the earlier newui-parity-2 base. All changes are committed locally; nothing was pushed.
