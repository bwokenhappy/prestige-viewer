# Avatar position sync over the Slipstream Helper (#1417, #1526)

Each Slipstream user shares the position their own viewer draws their avatar at **while it
stands still**. Other Slipstream viewers nearby draw that avatar there once it has stopped,
instead of where their own reading of the sim's updates puts it. While it moves, everyone draws
it from the sim's updates as usual (see "Standing still only" below). Code:
`indra/newview/slpossync.{h,cpp}` (feature),
`slpossyncmath.h` (pure rules, tested by `scripts/check/test_pos_sync.cpp`), the helper's
`posSend` / `posSee` / `posHeard` in `app_settings/slbridge/slipstream_helper_ext.lsl`, and the
apply point in `LLVOAvatar::updateRootPositionAndRotation`. Check: `pos-sync`
(`scripts/check/checks/pos_sync.py`, stage `source`).

## Why it cannot be used to cheat

Two rules, enforced by the receiver, make it safe in combat:

1. **Visual only.** A shared position moves only the receiver's drawn root joint (so the
   skeleton and the attachments). The avatar's drawable, its sim-driven position, picking,
   sits, collisions and every camera keep the sim's position. The sim never sees any of
   this traffic as position: it is chat between two scripts. Combat 2.0 hits, collisions,
   sit targets and camera constraints are all decided on the sim against the sim's
   position, so hit boxes stay where the sim has them.
2. **2 m bound.** A receiver uses a shared position only while it is within 2 m of where the
   sim's updates put that avatar (`SLPosSyncMath::BOUND`). It checks this itself, on every
   line and again every frame, because a modified helper or viewer can send anything. Over
   2 m, the update is rejected and the avatar eases back to the sim's placement. The drawn
   offset is clamped to 2 m even while blending. The sender's helper also clamps against
   `llGetPos()` before speaking, as a first line of defence.

Together these mean a sender can at most look up to 2 m away from where it really is. It
can't be somewhere else, and it can't move anyone else. That is the same order of error as
ordinary extrapolation under lag, and it never changes where it can be hit.

**Identity** comes from the sim only. The receiving helper attributes a line to
`llGetOwnerKey()` of the attachment that spoke (rezzed objects and avatars chatting on the
channel are not attachments, and are ignored). The line itself names nobody, and a line
with an extra field, such as a smuggled key, is rejected whole (`parseFields`). Nobody can
send a position for anyone but themselves. Muted avatars are ignored.

**Privacy.** Sharing is the option *Preferences > Privacy > General > "Share my precise
position with nearby Slipstream users"* (`SLPosSyncShare`). It is persisted and on by
default. When the option is off, or while RLV hides location, names or nearby avatars
(`@showloc`, `@shownames`, `@shownametags`, `@shownearby`, `@showminimap`, `@showworldmap`, or
a block on the channel), the viewer sends nothing and tells the helper `pos/share/1|0`. The
helper then says nothing for us either: it starts with sharing off and only shares after
the viewer says `1`. Receiving is always on.

## Messages

| Direction | Line |
| --- | --- |
| viewer -> own helper (private channel) | `pos/share/1\|<0\|1>` (control: held until ready, never dropped) |
| viewer -> own helper | `pos/send/1\|<seq>\|<x,y,z>\|<vx,vy,vz>\|<qx,qy,qz,qs>\|<flags>` (superseding slot) |
| viewer -> own helper | `pos/see/1\|<key>,<key>,...` (at most 24 keys, nearest the camera first; superseding slot) |
| helper -> region, `POS_CHANNEL` -1397510224 ("SLPP"), `llShout` (100 m) | `pos/1\|<seq>\|<pos>\|<vel>\|<rot>\|<flags>` |
| helper -> own viewer (`llOwnerSay`) | `#slb:pos/1\|<owner from the sim>\|<seq>\|<pos>\|<vel>\|<rot>\|<flags>` |

- `pos` is the region-local *drawn* position (`getRenderPosition()`). The sender's helper
  re-states it with `(string)vector`, which is 5 decimals, so it is millimetre precision.
- `rot` is the avatar's root rotation.
- `flags` is any of `S` (hold), `T` (still), `F` (flying). Only a **still line** (`T` without
  `S`) is ever drawn. `S` means "hold: draw the sim's placement". A sender sends it while
  seated, and once as the **release** when a still avatar starts to move. Receivers let go on
  any line without `T` or with `S`.
- `seq` is the sender's U16 counter. It wraps. Repeated and older lines are dropped, and a
  counter that restarts is accepted after 10 s of quiet.
- There is no shared clock. Receivers age a line by its receive time. Nothing is
  extrapolated: only still lines are drawn.

**Why the release is `S` and not a bare moving line.** Viewers from before #1526 extrapolate
a line without `T` by its velocity for 0.5 s and then hold that point while the sim moves the
avatar on, dragging it back until the 2 m bound breaks. On `S` they ease back to the sim's
placement at once. Viewers from #1526 on treat both as a release. So the release works with
every viewer, and the helper needs no change: it already forwards any `STF` flags, and a
change of flags always passes its thresholds.

**Compatibility, all four ways:**

| Sender | Receiver | Result |
| --- | --- | --- |
| #1526 | #1526 | Drawn at the precise position while standing; released on the first moving frame. |
| #1526 | older | Still lines drawn as before; on the move, `S` eases it back (τ 0.3 s); nothing while moving. |
| older | #1526 | Its still lines are drawn while the sim agrees it stands; its moving lines are ignored (release). |
| older | older | As before #1526. |

The type names follow the helper's existing `combat/<name>/<version>` style and the planned
v1 envelope `#slb:<type>/<ver>|<fields>` (#1213-#1216). The viewer registers its handler
under `"pos/1"`, so it moves to `registerHandler(type, version, ...)` as-is when #1215 lands.
The new public channel means helpers without position sync never hear (or pay for) it. A
non-Slipstream viewer ignores the channel. A Slipstream viewer with an older helper simply
doesn't send. The helper version hash makes every worn helper update.

Numbers are written with `formatFixed` and read with `std::from_chars`, so a user locale with
a decimal comma can't change them.

**What this version does not correct:**
- **Seated avatars.** They send `S`, and receivers keep the seat's placement. Drawing an
  avatar off a seat that the receiver itself extrapolates (vehicles) would separate it from
  the seat. Sending seat-relative offsets is a possible follow-up.
- **Hover height.** Both sides compare the drawn (drawable) position, and hover applies
  further down, at the root.

## Standing still only (#1526)

The first version also shared the position of moving avatars, at up to 4 lines a second.
Receivers eased the drawn avatar towards each line while their own interpolation from the
sim's updates moved it too. The two fought, and moving avatars rubber-banded. Interpolation
is good at motion, and the desync worth fixing is where an avatar comes to rest. So now:

**Sender** (`SLPosSyncMath::Sender`, tested by the `SENDER` fixtures). A 10 Hz tick decides:
- **Settled** = not seated, slower than 0.05 m/s, no movement input (walk / strafe / jump /
  fly-up / nudge controls, or autopilot), and the drawn position no more than 1 cm off the
  previous tick's, all for 0.5 s. Turning in place is not movement.
- Once settled: one still line (`T`, plus `F` when hovering) at once. Another goes out when it
  settles 5 cm elsewhere or turns 5° in place, plus a 10 s keep-alive.
- On the move after a still line: exactly one release line (`S`, real velocity). Then nothing
  until it settles again.
- Seated: `S` lines on change and a 10 s keep-alive, as before.
- After a helper restart, a region change or sharing turned off, nothing is sent until it
  settles. No release either: no receiver holds anything from us then.

**Receiver** (`SLPosSyncMath::View`, tested by the `RELEASE` fixtures):
- A still line is drawn only while the sim's own placement stands still too. It **engages**
  on the first frame the sim's velocity for the avatar is under 0.25 m/s, anchored at the
  sim's placement then. It eases in with τ = 0.12 s.
- It is **not drawn on any frame** the sim's velocity is over 0.25 m/s. It is **done** once
  the sim's placement is 0.2 m from the anchor, or a release line arrives. A line the sim's
  placement never settled for within 1 s is dropped. A done line is never re-engaged at
  another spot; the next still line is needed.
- **Hand-off without a pop.** Once the line is no longer drawn, the offset (drawn minus sim
  placement) shrinks each frame by at most half of what the sim's own placement moved that
  frame. The drawn avatar therefore moves with the sim from the first frame, at between 50%
  and 150% of its speed in the direction of motion. It never stops, never goes back, and never
  slides while the sim's placement stands. The offset is gone after twice its length of
  travel: a typical 5-30 cm gap within 10-60 cm, about 0.03-0.2 s at walking speed.
  - Dropping the offset at once would be a pop of up to 2 m.
  - The old fixed-time ease-out (τ 0.3 s) moved the avatar against its motion.
  - While the sim's placement stands still (a release line that arrives before the sim's
    update), the offset waits up to 1 s for it to move, and then eases out with τ 0.3 s.
    Expiry, the 2 m bound and a dropped entry ease out with τ 0.3 s at once, as before.
- Rotation is handed back to the avatar's own orientation code on the same frame, as before.

## Measuring

Every 20 s while lines arrive, the receiver logs a `PosSync` line with three figures:
- lines used and rejected;
- **gap** (mean / max): the distance between the sim-driven placement and the sender's
  shared position, over the frames a still line is drawn. This is the desync without the
  feature;
- **residual** (mean / max): the distance from the drawn position to that shared position
  after the blend;
- **let go / walked off** (#1526): hand-offs started because the sim moved a drawn avatar, and
  still lines ended by the 0.2 m rule. While someone walks, "lines used" stays flat (about two
  per stop and start). If "walked off" climbs while people stand still, the receiver's own
  interpolation is sliding more than 0.2 m after a stop.

The debug tag `PosSync` adds one line per received update: the sender, seq, flags, and the
gap on arrival.

## Rates and budgets

**Sender (viewer):** see "Standing still only".
- A still line once settled (0.5 s), then only when it settles 5 cm elsewhere or turns 5° in
  place.
- One release line on the move; nothing while moving.
- Seated: a line when the state changes.
- At most 4 lines a second.
- Keep-alive: every 10 s while settled or seated; none while moving.

**Sender (helper):**
- Its own thresholds, slightly finer: 4 cm, 0.15 m/s, 4°, keep-alives 0.9 s / 9.5 s.
- At most one line per 0.2 s.
- Clamped to 2 m of `llGetPos()`.

**Receiver (helper):**
- Forwards only listed avatars: at most 24, and at most one line per avatar per 0.15 s.
- At most 30 lines a second in total (burst 45), in its own buckets, separate from the
  pose/consent budgets.
- Each line must also be within 2.5 m of the speaker's `OBJECT_POS` (same region only).

**Receiver (viewer):**
- `pos/1` lines have their own route lane: 30/s, burst 45.
- Per-avatar table: at most 32 avatars. While it is full, a newcomer waits until an entry
  expires.
- Entries expire after 3 s without a line after a hold / moving line, or 25 s after a still
  line. A still line stops being drawn on the first frame the sim moves the avatar at more
  than 0.25 m/s, and is done once the sim has moved it 0.2 m.
- Blend: ease in with τ = 0.12 s. Hand back with the sim's motion (at most half of its step
  each frame), or with τ = 0.3 s while the sim's placement stands still. Never a snap, except
  on sitting down, where the sit itself repositions.
- Per-frame cost: one branch per avatar, plus `View::step` (a few dozen flops) for followed
  avatars only.

**Bridge commands:**
- `pos/send/1` and `pos/see/1` use superseding slots (`SLBridge::sendLatest`): a newer line
  replaces a pending one.
- The slots take turns with the pose data queue inside the existing 5 commands/s pacing,
  after control lines.

### Volume and script time (estimates; measure in-world)

**Line size:** about 115 bytes on `POS_CHANNEL`, and about 100 bytes viewer -> helper.

**Lines per sender:**

| State | Lines a second | Bytes a second |
| --- | --- | --- |
| Still or seated | 0.1 | ~12 |
| Walking, running, flying | 0 | 0 |
| Each stop and start | 2 lines in all (one still, one release) | ~230 per stop |
| Turning in place | up to 4 | ~460 (peak) |

**Script time per helper (Mono):**
- Own line: about 60-100 µs per line sent, so about 0.1 ms/s typical and 0.4 ms/s peak.
- Each heard line not in the visible list: about 20 µs (dropped after one list lookup).
- Each forwarded line: about 100-150 µs (parse, two casts, `llGetObjectDetails`,
  `llOwnerSay`).
- Moving users cost nothing since #1526. With K standing Slipstream users in view at 0.1
  lines/s each (plus two lines per stop and start): about 0.015·K ms of script time a second.
  The old figure for K = 10 moving users was about 2 ms/s.
- The receiving cap of 30 lines/s bounds it at about 4.5 ms/s, whatever the crowd.
