# VR full-body tracking

In VR, Slipstream can drive your own avatar's hips, spine and legs from your real body. Tracking
is **local only**: you see your own body move (in the headset and in the desktop mirror); other
people see your normal animations.

There are four body sources. `SlipstreamVRBodySource` picks one; it is `Auto` by default:

| Value | Source |
| --- | --- |
| `Auto` | OpenXR trackers when the VR runtime lists any, else SlimeVR when its packets arrive, else the headset's own body tracking, else the 3-point estimate (unless `SlipstreamVRBodyEstimate` is off) |
| `OpenXR` | trackers the VR runtime exposes (SteamVR: Vive / Tundra trackers, SlimeVR's SteamVR driver) |
| `SlimeVR` | SlimeVR server, read directly over the network (any runtime): VMC skeleton, else its 8 OSC trackers |
| `Headset` | the headset's own body tracking (Quest over Link / Air Link or Virtual Desktop's VDXR; HTC; Pico) |
| `Estimate` | no trackers: hips, crouch and planted feet with steps estimated from the headset and the two controllers (#1586) |

Whatever the source, the wrist panel shows **Body** while it has a body. With worn trackers
(OpenXR, SlimeVR) Body calibrates them: press it, then within 3 seconds stand straight in a
T-pose, facing forward. With headset body tracking or the estimate Body only measures your eye
height: press it and stand straight.

## No trackers: the 3-point estimate

With only a headset and controllers (Valve Index, Quest without body tracking) Slipstream
estimates the rest: the body faces where you look once your head turns past a comfortable twist
(a glance does not turn the hips), the hips follow the head, crouching bends the legs with the
feet planted on the floor, and a foot takes a short step when your body drifts or turns away from
it. While the avatar walks or flies with the thumbsticks the walk / fly animations take the legs.
Press Body once standing straight so your height maps onto the avatar.

Your standing height is one number for the view and the body: a recentre that takes your height
(hold the left stick) and Body both set it, and the avatar's eyes stay at the camera. The camera
sits at the avatar's own eyes (its eye joints; the Open Pony's pupils), with the head joint under
it by the same offset, so a level look does not show your own chest. Log: `eye point: ...` (which
eyes and where, `VRBody`), `reference height ...` (`VR`).

## Fingers

Your avatar's Bento fingers follow yours (`SlipstreamVRFingerTracking`, on): with hand tracking
where the runtime offers it -- Quest camera hand tracking, and on SteamVR the Valve Index
controllers' per-finger sensing (SteamVR provides it as hand-tracking joints) -- else a natural
grip from the controller (trigger, grip, and the trigger / thumb touch sensors: point, fist,
thumbs up, open hand). With tracked hands and no controller, the arms follow the hands too.

## Body mapping: any avatar, the Open Pony (#1592)

Which avatar joint each tracked part drives, and whether tracking drives it at all. Open it from
the VR wrist (**Mapping**), from **Preferences > Safety > Body mapping...**, or from the VR settings
wall's Body page. Every change applies at once and is saved for this avatar (per account, like
the world size).

- **Profile:** Auto (picked from your worn rigging), Human, Open Pony **Bipedal**, Open Pony
  **Arms only**, Open Pony **Head only**, or Custom (any edit, or an imported file).
- **Stands on its hind legs (bipedal):** the stance switch beside the profile (pony profiles and
  bodies not detected as human). Setting a pony profile's spine or chest to Tracked turns it on
  (your torso is upright; a horizontal barrel cannot follow it), and standing drives the hips,
  spine and hind legs; unticking puts those back on the animation.
- **Roles:** pelvis, spine, chest, neck, head; per side collar, upper arm, forearm, hand, fingers,
  thigh, shin, foot, and hoof (the bone past the hand that ends a foreleg; none on a human). Each
  has a joint (or none) and a state: **Tracked** (your tracking drives it), **Animation** (the
  animation keeps it), **Off** (held still in the profile's base pose).
- **Weights** per part (hips, spine, head and neck, each arm, each hand's fingers, each leg).
- **Preview:** your avatar's skeleton as it is drawn now (front and side), tracked bones green,
  animation grey, off amber; the role under the pointer (or the one you clicked) in white.
- **Import / Export:** a small text file (`role <role> <joint|-> <tracked|animation|off>`,
  `weight <part> <0..1>` over a built-in `profile`).

The Open Pony (from its dev kit's weights; the rig and its bone table: `doc/open_pony_rig.md`):
the barrel deforms on mPelvis, mTorso, mChest, mNeck and mHead (through their collision volumes;
the Bento spine bones carry nothing); the forelegs on mCollar (scapula), mShoulder (upper arm),
mElbow (forearm), mWrist (the cannon: a horse's "knee" is its wrist) with the front hoof on
**mHandRing1**; the hind legs on mHip, mKnee, mAnkle with the hind hoof on mFoot. You need no bone
names: the Open Pony profiles map all of it. The forelegs are driven as arms bone for bone: the
controller's grip is where the hoof joint goes and the hoof turns as your hand (the forearm and
cannon twist only a forearm's share of a roll); your reach is taken from your shoulder and scaled to the foreleg's length from the pony's
shoulder; the scapula stays on the body and the elbow points back. Finger tracking never bends
mHandRing1 (the hoof role holds it), so every pony profile leaves the fingers to the animation.

- **Bipedal:** the pony stands on its hind legs: its pelvis pitches the barrel up (about 84
  degrees for the Open Pony: its rest neck line becomes vertical), the neck, upper forelegs and
  hips turn back so the head faces forward and the legs hang (the scapulae ride with the barrel). Your legs drive the hind legs, your
  arms (controllers) the forelegs as arms, the headset the head and neck, your torso the barrel.
  The stance holds without a body source (the hips under your head); your eye height is the
  standing pony's (about 1.4x its four-legged one), and the VR camera sits there.
- **Arms only:** the pony stays on four legs and its animations; the forelegs follow the
  controllers, the hooves in your hands, reaching from its own shoulders; the head and neck follow
  the headset (no lean toward the headset's
  position: a quadruped's head is not where yours is).
- **Head only:** the head and neck follow the headset; everything else is the animation.

Auto picks Arms only for the Open Pony (its hooves on mHandRing1 and mFoot, no other finger
weighted), Head only for another quadruped (a horizontal spine, or Bento hind limbs weighted),
else Human. Pose sharing sends exactly the joints the mapping drives.

Head and neck (every profile): a head tilt (ear toward the shoulder) and a nod happen mostly at
the head joint, the neck taking a fifth of the tilt and two fifths of the nod; a turn keeps its
split, and the avatar turns only past `SlipstreamVRBodyTurnLimit`.

Log (`VRBody`): `body mapping: <profile> (why)`, `body mapping detection: ...`.

## SteamVR trackers (Vive / Tundra, or SlimeVR through SteamVR)

1. Run SteamVR as the OpenXR runtime with your trackers on.
2. In SteamVR > Manage Trackers, give each tracker a role (Waist, Left Foot, Right Foot;
   optionally Chest, Knees). Trackers without a role cannot be used.
3. In Slipstream VR: wrist panel > **Body**, then T-pose.

## SlimeVR with Virtual Desktop (VDXR) or any other runtime

SlimeVR's trackers normally exist only inside SteamVR. Slipstream can also read them straight
from the SlimeVR server, so they work with Virtual Desktop's own runtime (VDXR), Meta Link and
any other OpenXR runtime.

Slipstream reads two SlimeVR outputs. **VMC is the better one**: it carries SlimeVR's whole
solved skeleton (hips, spine and upper chest, neck, legs and feet, shoulders, arms, hands, and
fingers with finger trackers), and your avatar's bones follow those rotations directly. The
VRChat OSC output carries only 8 trackers (hip, feet, knees, chest, elbows), which go through
the T-pose calibration and leg IK. When both arrive, VMC is used. Turn on at least one.

SlimeVR server, Settings > OSC > **VMC** (recommended):

- Enable it.
- Address: `127.0.0.1`; Port out: `39539` (the default; it must match
  `SlipstreamVRBodySlimeVMCPort`).
- Leave **Mirror tracking** off. A VRM model is not needed (Slipstream uses your avatar's own
  bone lengths); "Anchor at hips" does not matter.
- Arm bones: SlimeVR leaves them out when it tracks your arms from controllers. Slipstream also
  keeps an arm on its controller while that controller is tracked, and uses SlimeVR's arm only
  when it is not (hand tracking, controllers put down).
- The headset keeps your head and neck. With VMC, SlimeVR's heading is lined up from your hips
  when you press Body (SlimeVR's full reset defines its "forward": press Body again after one if
  your avatar's body faces the wrong way).

And/or Settings > OSC > **VRChat OSC Trackers**:

- Enable it.
- Address: `127.0.0.1` (the SlimeVR server on the same PC as Slipstream: Slipstream listens on
  this PC only and never sends anything).
- Port out: `9000` (the default; it must match `SlipstreamVRBodySlimePort`).
- Trackers to send: **Waist** and **Feet** are on by default. Also turn on **Knees** and
  **Chest** if you wear those trackers, and **Elbows** if you have arm trackers (they help the
  alignment).
- If VRChat runs at the same time, it owns port 9000: give SlimeVR and Slipstream another port
  (for example 9010 in both).

Then:

1. Start the SlimeVR server, then Virtual Desktop (runtime: VDXR), then Slipstream in VR.
   Leave the source on `Auto`, or set it to `SlimeVR`.
2. When SlimeVR's packets arrive, the wrist panel shows **Body**.
3. Do a SlimeVR **full reset** while standing straight.
4. Press **Body**, then within 3 seconds stand straight, facing forward (with the OSC trackers:
   in a T-pose). This lines SlimeVR's space up with your play space (heading, and with the OSC
   trackers where your feet are), measures your eye height, and with the OSC trackers
   calibrates each tracker to its bone.
5. After a later SlimeVR full or yaw reset, if your avatar's body faces the wrong way, press
   **Body** again.

## Quest headset body tracking (no trackers): Link / Air Link or Virtual Desktop (VDXR)

The headset's own body tracking (Quest 3 / 3S: inside-out legs; Quest 2 / Pro: generated legs)
reaches Slipstream through the runtime's OpenXR body tracking.

1. Turn body tracking on in the headset (Quest settings: Movement tracking > Body tracking).
2. Use a runtime that forwards it:
   - **Meta Quest Link / Air Link**: Meta's PC runtime as the OpenXR runtime (Meta Quest Link
     app > Settings > General > OpenXR runtime). Developer runtime features may need to be on
     (Settings > Beta).
   - **Virtual Desktop**: runtime VDXR, with tracking forwarding (body tracking) enabled in the
     Virtual Desktop streamer / headset settings. Slipstream notices support switched on after
     it started; a runtime that did not offer the extension at launch needs a restart.
3. Leave the source on `Auto` (used when no trackers and no SlimeVR are there), or set it to
   `Headset`. `SlipstreamVRBodyFidelity` 1 (High) asks for inside-out legs where the headset
   has them; 0 (Low) uses generated legs.
4. Press **Body** on the wrist panel and stand straight to measure your eye height (kept for
   next time). If your body later sits too high or low (Virtual Desktop may reset your height
   when the Quest menu opens), press **Body** again.

In `Auto`, if no body is tracked for 10 s (body tracking off in the headset), Slipstream
releases the body tracker and re-checks every 30 s.

## Troubleshooting (log tag `VRBody`)

- `body source: ...`: which source is in use.
- OpenXR trackers: `body trackers (OpenXR trackers): N listed, ...` lists the roles.
- SlimeVR:
  - `SlimeVR VMC: listening on 127.0.0.1:39539` / `SlimeVR OSC: listening on 127.0.0.1:9000`,
    then `stream started`: packets arrive.
  - `SlimeVR mode: VMC skeleton` or `VRChat OSC trackers`: which one drives your body.
  - `SlimeVR VMC bones seen: ...` / `SlimeVR OSC trackers seen: ...`: what SlimeVR sends.
  - `cannot listen on 127.0.0.1:...`: another program has the port; change it in both places.
  - `SlimeVR VMC: N packets/s, bones: ...` (or `OSC ... trackers`) every 30 s.
  - `SlimeVR VMC alignment: yaw ...` / `SlimeVR alignment: yaw ... from ...`: after Body.
- Headset: `headset body tracking on: <joint set>`, `headset body tracked`, the fidelity given
  (`low (generated legs)` when high was asked but the headset cannot), and every 60 s the joint
  counts.
- `calibrated (...)` (trackers) / `<source> calibrated: eye height ...` (headset body tracking,
  SlimeVR VMC): the Body result.
- No Body button: no source has a body. For SlimeVR, its OSC output is off or its address or
  port differs; for the headset, body tracking is off or the runtime does not forward it; or
  `SlipstreamVRBodySource` names another source.
