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
- Code signing (owner chose it after Windows Smart App Control, in enforce mode on their PC,
  blocked the unsigned 1.8.7 installer): `scripts/wincross/sign.sh` signs with Azure Trusted
  Signing through jsign on the Linux runner: every unsigned .exe / .dll in the stage (20 of 133;
  the rest come signed), the NSIS uninstaller (`!uninstfinalize`) and the Setup.exe. It is a no-op
  until the repository secrets `PRESTIGE_SIGN_TENANT_ID`, `_CLIENT_ID`, `_CLIENT_SECRET`,
  `_ENDPOINT`, `_ACCOUNT`, `_PROFILE` exist (the owner sets up Azure; never sign up for them).
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
  Effects responsiveness (`llfxruntimevk.cpp`): switching effects off keeps the compiled effects
  for 30 s (`kFreeAfterOffSec`, freed in `tick()`), so off/on is instant; a failed permutation is
  retried twice (2 s, 8 s; `retryFailed`) instead of staying failed until Refresh.
  Weather (1.8.8, owner): only a rain <-> snow switch is instant; turning weather on, intensity and
  wind build up over WeatherTransitionSeconds again (the 1.8.6 "everything instant" lost the build-up).
  Jewelry detail is removed (1.8.8, owner): no checkbox; a leftover "on" restores the saved
  settings once at startup (`restorePrestigeJewelryDetail`, llviewercontrol.cpp).
  Sandstorm removed in 1.8.7 (owner: did not look like sand): no combo item, a saved WeatherType 2
  becomes rain; the sand shader / State::sand plumbing is left in place, always 0.
  Quick Preferences: ReShade... replaces Photo Studio... (toolbar unchanged).
  Note: in 1.8.6 `SLFxEnabled` was still on from earlier sessions, so the restored effects drew at
  once; that was not a bug.
- 1.8.9 (owner's requests, not tested in-world): Received items box at the bottom of Inventory
  (sidepanel_inventory.xml `inbox_layout_panel`, LLSidepanelInventory::updateInbox; header with
  count folds it, refresh fetches; the folder is still in the tree too). Friends log in / out
  pop up at the bottom left as nearby chat toasts (`PrestigeFriendStatusNearby`,
  llnotificationtiphandler.cpp; shown even with PrestigeLocalChatNotifications off), not top
  right. Group chat and conference IMs pop up at the top right by default
  (`NotificationGroupChatOptions` / `NotificationConferenceIMOptions` = toast). The AO window
  (sl_ao, SLFloaterAO) is rebuilt in Firestorm's Animation Overrider layout; the full editor
  (sl_ao_editor) is behind the wrench / bottom-right arrow. The old pause / status / ignore
  buttons of the small AO bar are gone from it.
- 1.9.0: the "... has said something new" line in chat history (`more_chat_text`, shown while
  scrolled up) had a see-through strip in the Firestorm skins and drew over the chat text; its
  panel now uses `ChatHistoryBgColor` (llchathistory.cpp).
  Friends log in / out (owner: they only went into Nearby Chat): nearby pop-ups are skipped while
  Nearby Chat is open, so friend-status toasts (`prestige_friend_status`, `mFriendToasts` in
  llfloaterimnearbychathandler.cpp) now skip that check, the text / log styles and the
  chat-window cover rule, and always pop up at the bottom left of the screen (not in DND).
  Nearby chat notices look like Firestorm's (owner): `PrestigeChatTags` off by default, no
  italics on system lines, no "Second Life:" name and no [brackets] in nearby chat (brackets stay
  in IM windows, `ChatSystemMessageBrackets`); llchathistory.cpp.
  Friends log in / out are Firestorm's "Display Name (username) is online.": on screen one white
  line each in the text console at the bottom left, whatever NearbyChatOverlayStyle is
  (`LLViewerWindow::keepNearbyChatConsoleShown`); in Nearby Chat the name is a ChatNameColor link
  (`LLChat::mSubjectID`; 1.9.0 passed it in appendMessage args, which the windowed history
  drops, so the names were white: fixed in 1.9.1).
  Chat text in Firestorm's sizes (1.9.1, owner: "identical"): same DejaVu Sans, but Firestorm's
  fonts.xml has Small / Medium / Large 9 / 10 / 11 pt where Prestige's interface uses 7.6 / 8.6 /
  10.6. `ChatSmall` / `ChatMedium` / `ChatLarge` sizes (fonts.xml and every fontsets/*.xml) are
  used by `LLViewerChat::getChatFont` only; the rest of the interface keeps its sizes.
- Engine work toward Firestorm-on-Vulkan lives on branch `firestorm-vulkan` of the Firestorm fork; paused.
