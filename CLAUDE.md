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
- Since `ee799cc0` the workflow runs four `helper` jobs in parallel (each compiles a quarter of the
  object files into ccache, `WINCROSS_SHARD` in `scripts/wincross/build.sh`) before the `windows` job
  links and packages. Shared machine setup: `.github/actions/wincross-setup`.
- Memory: with ccache, the vkgfx files that include the generated `vk_world_shaders_spv.h` need
  roughly twice the RAM. Three compiles at a time starved the runners ("lost communication") even
  with swap, so `NINJA_JOBS` is 2 and the setup adds 16 GB swap. First good helper build:
  run 45764046216, 1 h 02 min in total with a warm cache.
- `build-full` has always been an ancestor of `firestorm-feel`: a normal (fast-forward) push works,
  and a forced push is refused by the session's safety checks.
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
  1.8.4 (not built or tested in-world yet): `OwnObjectGiveItem` (unpackers) joins that queue, the next
  offer surfaces on the next idle, object offers show without waiting for the owner's name lookup,
  `PrestigeInstantDeliveries` (off: offers still ask), and `PrestigeChatTags` labels nearby chat
  notices (System / Item / Friends / Region / Object / Script).
  Camera mode's pad did nothing when held (owner, 1.8.3; cause not found by reading): 1.8.4 takes a
  press inside the pad's circle by screen geometry (`padHit`, `slcameraview.cpp`) before the overlay's
  hover test; `LL_DEBUGS("CameraPad")` logs the press. Check it in-world.
- 1.8.5 (owner's requests, not tested in-world yet): closing with effects on no longer quarantines
  them (`SLFxStack::noteCleanExit` at quit; the provisional marker was only removed at the very end
  of cleanup); Effects has Refresh (was Reload; lifts quarantines) and Save As...; the tick on the
  Conversations toolbar button is now `PrestigeLocalChatNotifications` (off = no nearby chat
  pop-ups / sounds / flashing; the chat bar's visibility stays in `PrestigeChatBarAlwaysVisible`);
  unread conversations use `PrestigeConversationUnreadColor` (calm blue, not the beacon orange);
  `PrestigeBaseSaturation` (1.08) and a Sharpen slider (`RenderCASSharpness`) on Preferences >
  Graphics; nearby pop-ups stop below an open chat window, at most `PrestigeNearbyToastMax` (6).
  Vertical tab lists (Preferences) put their down arrow at the bottom (`lltabcontainer.cpp`).
  Pose stand: Standing 1-4 (idle stands, no visible change) replaced by local still poses (Arms
  down / forward, Legs apart; `LOCAL_POSES`) and two sits; the arm/leg angle signs are unchecked.
- 1.8.6: bundled effects (`app_settings/fx/pack`) are never quarantined (`SLFxStack::isBundledEffect`;
  old quarantines on them are lifted at startup). 61 more pack files from 5 licence-audited repos
  (THIRD_PARTY.md). **Every bundled .fx must pass `prestige-fx-check.yml`** (slfxc + spirv-val,
  includes resolved as on Windows); run it after adding effects. Weather: your own changes show at
  once (`PrestigeWeatherInstant`), region weather keeps `WeatherTransitionSeconds`.
  Also in 1.8.6 (not tested in-world): Sandstorm = `WeatherType` 2 (snow particles/rates as tan grains,
  `State::sand`; tan haze via `veil2.z` and `ssr.weather[16].z` + 2; no snow cover/depth; local only,
  a parcel export sends it as snow); `PrestigeModernNameTags` (title dimmed on top, bold display
  name, dimmed username, status in gold at the bottom); `PrestigeJewelryDetail` (raises LOD factor /
  probe resolution / SSR / anisotropic / bloom threshold, restores `PrestigeJewelryDetailSaved` when
  off); texture upload Size (`PrestigeUploadTextureMaxSize`); friends log-in/out pop-ups row fixed in
  Preferences > Notifications and hidden while `PrestigeLocalChatNotifications` is off; the
  Firestorm LSL Bridge's `<bridgeURL>` chat is dropped (`llfloaterimnearbychathandler.cpp`).
- 1.8.7: Effects > Save As... saved over the active preset (it set the combo's text, but
  `getSimple()` returns the selected item's label); it now saves under the typed name
  (`savePresetNamed`). Jewelry detail (spelled "Jewelry" in the UI) changed nothing visible in
  1.8.6; it now also turns on ray-traced reflections (`RenderVKRTReflections`, roughness up to
  0.6), per-pixel glossy reflections (`RenderVKDeferReflectSharpRough` 0.6) and the Realtime probe
  (`RenderReflectionProbeDetail` 2). The owner has an RTX 4070 laptop GPU (8 GB) and 16 GB RAM.
  Note: in 1.8.6 `SLFxEnabled` was still on from earlier sessions, so the restored effects drew at
  once; that was not a bug.
- Engine work toward Firestorm-on-Vulkan lives on branch `firestorm-vulkan` of the Firestorm fork; paused.
