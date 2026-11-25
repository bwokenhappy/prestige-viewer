# Prestige Viewer — notes for Claude

Prestige is a Second Life viewer: Firestorm's menus and features on Slipstream's Vulkan renderer,
made to look and feel like Firestorm 7 (the owner is a long-time Firestorm user). C++ (Linden Lab /
Firestorm / Slipstream lineage), XUI skins, RmlUi for camera mode.

## Working branch and builds
- Work on branch **`firestorm-feel`**. Push it when a change is done.
- Windows builds run on GitHub Actions (workflow `prestige-windows.yml`, ~2 h from scratch, much less
  with a warm ccache). A user-facing build is made from branch `build-full`:
  ```
  git push -f origin HEAD:refs/heads/build-full
  gh workflow run prestige-windows.yml -R bwokenhappy/prestige-viewer --ref build-full
  ```
  A new dispatch on the same branch cancels the running one.
- Raise `indra/newview/VIEWER_VERSION.txt` (1.8.3 → 1.8.4 …) for every build the owner will install, so
  the installer treats it as an upgrade and the window title shows which build is running.
- The download is the run's Artifacts → `Prestige-Windows-x64`; the owner runs
  `Prestige_<version>_Setup.exe` after closing Prestige.
- Prestige only builds on Linux (Windows builds are cross-compiled with clang-cl + xwin,
  `scripts/wincross`). Native Windows/Visual Studio builds are not set up.

## Rules
- Never name the third-party viewer whose feature list some Prestige features were modelled on — not in
  code, comments, strings, file names or commits. Those features are Prestige's own.
- New settings: `indra/newview/app_settings/settings.xml`, named `Prestige*`.
- Mark edits to existing files with `// <Prestige> …` … `// </Prestige>` (XML: `<!-- <Prestige> -->`).
- The Windows build uses clang-cl with warnings as errors: no unused variables, `override` where needed.
- Never type the owner's Second Life password or log in for them; they log in themselves.
- Firestorm's source is the reference for anything "like Firestorm"
  (github.com/FirestormViewer/phoenix-firestorm).

## The owner's choices (keep them)
- Default skin "Firestorm (Grey)" (`skins/firestorm`), Prestige skins still selectable. Top bars grey on
  test builds. Small icons are Prestige's sharp vector icons; inventory/object/parcel icons and frames are
  Firestorm's bitmaps (`scripts/content_tools/prestige_firestorm_skin.py` regenerates the skin).
- Toolbar buttons show their **names beside the icons** (layout "left", toolbar layout version 7).
- Chat/IM: every name (own included) in Firestorm's lime `ChatNameColor`, the text, ": " and timestamps
  white (`LLViewerChat::getChatNameColor`).
- Advanced and Developer menus shown; the People button opens the People window; Firestorm top bar
  (location in the menu row, Land / Lighting / Search) and the "Nearby Chat" bar at the bottom left.

## Open items
- **Firestorm-style inventory** (Collapse/Expand row, type filter, bottom bar): helper version drew its
  rows ~550 px below the title bar at runtime; the three layout files (`panel_main_inventory.xml`,
  `sidepanel_inventory.xml`, `floater_my_inventory.xml`) were reverted to the working ones. Redo it while
  the owner is logged in so it can be checked on screen. The C++ for the new controls is still there
  (optional lookups) — commit `e5e20a8` has the old attempt.
- Firestorm's **Content** menu is not in Prestige (owner not asked yet whether to add it).
- 1.8.3 fixes multi-item deliveries (next offer pops up after answering one, `llscriptfloater.cpp`).
- Engine work toward Firestorm-on-Vulkan lives on branch `firestorm-vulkan` of the Firestorm fork; paused.
