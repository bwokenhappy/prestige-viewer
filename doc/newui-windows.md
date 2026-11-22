# New interface: native windows (#703)

The new interface (UIRenderer 2) rebuilds the classic floaters as native RmlUi windows on the kit:
each its own movable panel in camera mode's style. They replace the experimental side docks
(`UINewDocksDev`, retired).

**Owner rule:** a classic window is never replaced until its native window reaches feature parity.
Until then the native windows are a development option, `UINewWindowsDev` (Persist 0, off). With it
on, the tool grid's Conversations / People / Inventory buttons and the command surface ("People
(new)", ...) open the native windows; with it off they open the classic ones.

Try it: start the viewer with `--set UIRenderer 2 --set UINewWindowsDev 1`.

## The framework (`llnewuiwindow.h`)

- **Document.** One RML document per window kind (`llnewuiwindowdocs.h`), with the kit's frame
  templates: `<whead/>` (title, minimise, close) and `<wgrip/>` (the resize grip). The surface is the
  `$solid` token, opaque in both themes. It is translucent (0.92, `$surface`) only where the blur
  backs it, so text never sits on raw glass.
- **Controller.** Each window has an `LLNewUIWindow` controller bound to the viewer's real models.
- **Geometry.** Position and size are kept in UI units (`llnewuiwindowlayout.h`, tested by the
  `newui-windows` check):
  - the title bar moves the window and the grip resizes it;
  - edges snap within 10 dp to the screen edges and to other windows' edges;
  - windows stay fully on screen, and are clamped again when the window size, the UI scale or the
    menu bar changes.
- **Z order and focus.** A click raises a window and makes it the active one: its title is lit and
  it takes the keys.
  - **Esc** closes the active window. In a field, the first Esc leaves the field and the second
    closes the window.
  - **Tab / Shift+Tab** move through the window's controls.
  - **Lists** take Up / Down / PgUp / PgDn and Enter.
  - **Losing focus.** A click on the world or on a classic floater, or a classic view taking the
    keyboard, ends the active state.
  - **Limit.** The RmlUi layer draws above all LLView floaters, so native windows are always in
    front of classic ones.
- **Saved state.** Position, size, open and minimised state are saved per window key in
  `UINewWindowState` (Persist 1). Windows that were open come back at login, inventory windows
  included; torn-off conversations do not.
- **Instances.** Kinds that allow it run several instances: torn-off conversations and extra
  inventory windows. Each instance has its own data model (`inv`, `inv2`, ...) and its own saved
  key (`inventory`, `inventory.2`, ...).
- **Theme switch and camera mode.** A theme switch reloads the documents and keeps their state.
  Camera mode hides the windows and shows them again afterwards.

## Viewer vs lab

These were seen in the real viewer (garbage, 4K, GTX 1080) and not in the lab. All are fixed, and
the checks now catch them:

- **`llimage()` in `newui-inventory.rml`.** Empty rows assembled `'llimage(' + icon + ')'` in markup.
  The controller now binds the whole decorator, `none` when empty. newui-kit refuses decorators
  assembled in markup, and the lab's instancer now refuses empty names as the viewer's does.
- **`width: ;` and `decorator: ;`.** Bound styles are applied before the controller fills them, so
  string members bound to a style start with a value. newui-kit checks every such member.
- **See-through windows.** Glass is off on targets above 2560x1600 and on HDR, so a 0.92 surface
  had no blur behind it and showed the scene's detail under the text. Windows are opaque
  (`$solid`) unless the blur is on.
- **Italic.** The kit's Inter has no italic face, so emotes use a bold name.

Still open: the bar, the command surface and the tip card keep the 0.92 glass tokens without the
blur at 4K (`currentTokens` always asks for glass). They are small and mostly sit on the bar, but
the same `$solid` treatment would suit them.

Lab renders comparable with the viewer use `slrmluilab --size 3840x2160 --density 1.7` (garbage's
UI scale at 4K).

Tests without a pointer: the `NewUI` event API (`slctl.py event NewUI '{"op":...}'`) has these ops:
- `open`, `close`, `place`, `minimise`, `list`;
- `peek`: opens the tool grid as if hovered;
- `input`, per window:
  - chat: `line`, `send`, `tab`;
  - people: `view`, `filter`, `select`, `menu`;
  - inventory: `view`, `filter`, `select`, `toggle`, `menu`.

## Tool grid: pull-up tab

- **Tab.** A small tab sits flush with the bottom edge, or on the bar when `UINewBarAtBottom` is set.
- **Hover.** The pointer on the tab slides the grid up. The grid hides 300 ms after the pointer
  leaves it.
- **Lock.** The grid's grip locks it open (the grip turns accent) and unlocks it. The state is kept
  in `UINewToolGridLocked`, which replaces `UINewToolGridCollapsed`.
- **Push to talk.** A held tool keeps the grid open; if the grid is hidden while a tool is held, the
  tool is released.
- **Camera mode** still hides the grid.

## Parity per window

### Conversations (nearby chat + IM): `llnewuichat.cpp`

Done:
- **Tabs.** Nearby chat plus every IM session (P2P, group, ad-hoc) is a tab with an unread badge.
  The current tab is marked read under the classic rule. Closing a tab leaves the session through
  `LLIMMgr::leaveSession`, which keeps group snooze.
- **History.**
  - Nearby: `LLNearbyChatLog`, the last 200 lines.
  - IM: this session's messages plus the on-disk history tail (`requestLogTail`), with older lines
    greyed.
  - Timestamps follow `IMShowTime`; names and `/me` emotes are shown as in the classic history;
    system lines are greyed.
  - The wheel and PgUp / PgDn scroll it.
- **Sending nearby.** It uses the chat bar's own path (`LLFloaterIMNearbyChat::sendChatText`, now
  shared): `/N` channels, the channel selector, gestures, `/shout` and `/whisper` triggers, chat
  commands, OOC and poses, auto split.
  - Enter keys: Ctrl+Enter shouts, Shift+Enter whispers, Alt+Enter is OOC (each per its FS setting).
  - Typing plays the typing animation.
- **Sending IM.** It uses the classic conversation's path (`LLFloaterIMSession::composeIM` /
  `filterSendIM`, now shared): RLV IM restrictions, OOC, poses, `/N` to chat channels, auto split.
  Messages are queued until the session is initialised. One-to-one IMs send the typing notice.
- **Line editing.** Up / Down bring back sent lines. Tab completes the name of someone nearby or in
  the conversation, with a suggestion list.
- **Tear-off.** "Pop out" tears a conversation off into its own window (several at once); "Dock"
  puts it back.
- **Peer actions:** Profile, Offer teleport and Pay (one-to-one); Group info (group).
- **Started anywhere.** A conversation started anywhere (People, a profile's IM button, a group's
  chat) opens here while the native windows are on (`sl_show_conversation`).

Missing (the reasons Conversations stays classic by default):
- **Text.** Links (SLURLs, URLs, inline names) are not clickable, there is no text selection or
  copy in the history, and no emoji picker. Emoji glyphs do not render: the RmlUi faces have no
  colour emoji.
- **Participants and voice.** There is no participant list or moderation for group and ad-hoc
  chat, and no voice controls (call / hang up / speaker levels).
- **Offers and the typing notice.** Inline offers (inventory, friendship, teleport) shown in the
  conversation are not drawn as buttons. The other person's "is typing" notice is not shown.
- **History.** There is no search in the history (Ctrl+F). "Load more" history beyond the on-disk
  tail is missing.
- **Incoming conversations.** New incoming IMs follow the classic notifications (toasts and the
  classic window per the user's settings). Nothing raises the native window on its own.
- **Chat bar.** The mouselook chat bar and the classic nearby chat bar stay classic.

### People: `llnewuipeople.cpp`

Done:
- **Lists.** Friends (online first), Nearby (within `NearMeRange`, by distance), Groups (the active
  one marked), Recent and Blocked, with a filter per list, a count and a scroll position thumb.
- **Actions.** All of them run through the viewer's own functions:
  - avatars: IM, Profile, Offer teleport, Request teleport, Call, Pay, Show on map, Share, Invite to
    group, Add / Remove friend, Block / Unblock;
  - groups: Chat, Group info, Call, Activate, Leave, Create a group, Search groups;
  - blocked: Unblock, Profile.
- **Where they are.** The four main actions are buttons. The rest are in a context menu (right
  click, or the "more" button).
- **Keys.** Double-click or Enter starts an IM (group chat on Groups). Up / Down / PgUp / PgDn move
  the selection, also from the filter field.

Missing:
- **Selection.** Several people cannot be selected at once: no conference IM or multi-person
  teleport offer.
- **Friends.** No contact sets, no online / offline sub-sections, no friend permissions view or
  edit. Friend-count and online-status toasts stay classic.
- **Nearby.** No mini-map pane, voice indicators, distance sort switch or "sort by name" options.
- **Groups.** No group title, notice or profile pane inside the window (Group info opens the classic
  group floater).
- **Recent.** No timestamps or sort options. Blocked has no "block by name" entry.
- **Avatars.** No avatar icons or profile pictures in the rows.

### Inventory: `llnewuiinventory.cpp`

Done:
- **Tree.** The folder tree over the real inventory model (`LLInventoryRows`): folders first, by
  name, then the Library. Opening a folder fetches it.
- **Views.** All (the tree; with a search, a flat list of matches with their folder), Recent (since
  the last logoff) and Worn, with a search per view.
- **Actions.** All go through the inventory's own functions:
  - Open (the double click's action) and Properties;
  - Wear (replace) and Add, for items and folders (`LLAppearanceMgr`);
  - Take off / Detach;
  - New folder;
  - Delete, through `LLInventoryGallery::confirmDelete`, now shared: the same worn-item checks and
    confirmations, then to the Trash.
- **Keys.** Up / Down / PgUp / PgDn, Left / Right close and open folders, Enter opens or wears,
  Delete deletes.
- **Drag out.** Items and folders drag onto the world, an avatar or a classic window through the
  classic drag tool.
- **Several windows.** "New window" opens another inventory window.
- **Rows.** Item icons and a "worn" mark.

Missing:
- **Drop in.** Nothing can be dropped onto the native inventory, from the world or between folders,
  so items cannot be moved or reorganised by drag.
- **Edits.** No rename, cut / copy / paste, paste as link or multi-select.
- **Filters and sorting.** No type filters (Filters panel), sort options, folder-only view or
  "show empty folders".
- **Inventory views.** No inbox / received items or marketplace listings handling, no gallery
  view, favourites or outfit editing.
- **Other actions.** No item-specific actions beyond open / wear / delete: give to avatar,
  teleport (landmarks), play (gestures / sounds / animations), edit (scripts / notecards / objects
  in world). "Open" on those opens their classic floaters.
- **Library.** Library items cannot be copied into the inventory by drag.

## What was verified in the real viewer (garbage, 2026-11-19)

All of these ran on the 4K box with `UIRenderer 2`, driven through `LLViewerWindow`'s own input
handlers (the TESTCTL `mouse` / `key` commands) or the `NewUI` event API.

**Verified, with native windows on:**
- **Window frame:**
  - dragging a window by its title moves it and snaps it to the screen edge;
  - a double-click on the title minimises it and restores it;
  - the close button closes it;
  - the windows reload under the light theme.
- **Keys:**
  - a click in the chat line takes the keyboard and typing goes into the field;
  - the first Esc leaves the field and the second closes the window;
  - Ctrl+K opens the command surface over the windows and Esc closes it.
- **Pointer:**
  - a right press on an inventory row, moved and released, starts no drag;
  - a left press on a row, moved 10 units and released inside the window, drops nothing;
  - with the Me menu open over People, clicking its first item opened the profile.
- **Chat.** A nearby chat line sent from Conversations went out through the chat bar's path and
  came back in the history.
- **Saved state.** UINewWindowState is written, and all four windows (inventory.2 included) came
  back after a restart.
- **Inventory windows.** Several of them run at once; the Worn view, folders, the context menus and
  the selection all work.
- **Tool grid:**
  - hovering the tab opens the grid and it closes after the pointer leaves;
  - the lock follows its setting;
  - with the native windows on, the grid's Conversations button toggles the native window.
- **Log.** No RmlUi warnings.

**Verified, with native windows off:**
- The command surface opens, searches and closes.
- The event API is absent.

**Observed but not resolved (injected input only):**
- In one native-windows-on run, the first injected click on the grid's Conversations cell opened
  the classic Map instead; the next click toggled native Conversations.
- In the native-windows-off run, an injected hover on the tab did not open the grid, so the
  "grid opens classic Conversations" path was not exercised.

Neither was seen with the event API's `peek`. Both need a run with a real pointer.

**Not exercised in the viewer** (implemented; unit tests or code review only):
- IM send through `composeIM` / `filterSendIM`, IM tabs and unread badges, closing a tab
  (`leaveSession`), tear-off and dock.
- The start-IM hook (People's IM, a profile's IM).
- Ctrl / Shift / Alt+Enter, and Tab name completion with real nearby names.
- People actions beyond opening the menu.
- Dragging an item out onto the world or an avatar.
- Delete with its confirmations, and wear / take off.
- The UI-scale clamp. It is covered by `test_newui_window_layout.cpp`; a live `UIScaleFactor`
  change did not resize the UI until a restart.
- The always-visible tab close mark (ce54c6ceb7), checked in the lab only.

## What the tool grid opens

| Button | `UINewWindowsDev` off (default) | `UINewWindowsDev` on |
| --- | --- | --- |
| Conversations / Nearby Chat (`chat`) | classic conversations | native Conversations |
| People (`people`) | classic People | native People |
| Inventory (`inventory`) | classic Inventory | native Inventory |
| everything else | the command, as the classic toolbar | same |

The command surface lists "Conversations (new)", "People (new)" and "Inventory (new)" while the
native windows are on. The classic windows remain reachable through it.

## Test steps (owner)

1. **Start.** Launch with `--set UIRenderer 2 --set UINewWindowsDev 1` (`UIRenderer 2` needs a
   restart if it was not already set).
2. **Tool grid.**
   - Hover the small tab at the bottom centre: the grid slides up. Move away: it goes after a
     moment.
   - Click its grip: it stays (the grip lit). Click again to unlock.
3. **Conversations.** Open Conversations from the grid.
   - Type in nearby chat. Try Ctrl+Enter (shout), Up (last line) and Tab after typing the start of
     a nearby name.
   - Move the window by its title, resize it by the corner, drag it near an edge (it snaps) and
     minimise it.
4. **People.** Open People.
   - Right-click a friend: try IM (it opens in Conversations), Profile and Offer teleport.
   - Try the Groups list, the filter, and Esc (closes it).
5. **Inventory.**
   - Open Inventory, open folders and search. Wear and take off an item, delete a test item (you
     are asked first), and drag an object onto the ground.
   - "New window" opens a second inventory window.
6. **Saved state.** Relaunch: the windows come back where they were.
