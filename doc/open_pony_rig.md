# The Open Pony rig and VR body tracking

How the Open Pony avatar (dev kit V3.2) uses the Second Life skeleton, and how the VR body
mapping (#1592, `indra/newview/llvrbodymapmath.h`) drives it. Read from the kit's
`[OP] OpenPony Mesh and Rigging.blend` (armature `Avatar`, body mesh `Body.012`) with
`scripts/tools/blend_rig_dump.py` (bones) and the mesh's vertex groups (weights). The numbers
below are the kit's own; a worn pony carries them as joint position overrides, scaled by its
shape.

## Conventions

SL axes: X forward, Y left, Z up, metres. Blender's armature space is turned: SL = (-y, x, z) of
Blender. Every SL joint rests with an **identity rotation**: a rig's "rest orientation" in SL is
only where its joints are (the offset from each joint to its child). Blender bone rolls do not
reach the animation system.

## What the mesh is weighted to

The body deforms on the classic bones' **collision volumes** (fitted mesh) plus a few Bento
bones. Weighted: PELVIS / BUTT (mPelvis), mTorso / BELLY (mTorso), CHEST (mChest), NECK (mNeck),
HEAD (mHead), L/R_CLAVICLE (mCollar), L/R_UPPER_ARM (mShoulder), L/R_LOWER_ARM (mElbow),
L/R_HAND (mWrist), **mHandRing1Left/Right** (the front hooves), L/R_UPPER_LEG (mHip),
L/R_LOWER_LEG (mKnee), L/R_FOOT (mAnkle), **mFootLeft/Right** (the hind hooves), mTail1, and the
face bones. Not weighted: mSpine1-4 (zero length), mHindLimb*, mToe*, mWing* on the body, and
every other finger bone (that is how the viewer tells the Open Pony from a human with Bento
hands: the front hooves on mHandRing1 and no other finger weighted).

## Bones

Offsets are from the bone above (left side; the right mirrors Y). Lengths in metres at the kit's
scale (the pony's eye is about 0.91 above the ground, its back about 0.57).

### Spine, neck, head (horizontal barrel)

| SL joint | Pony part | Offset from parent | Length | Notes |
| --- | --- | --- | --- | --- |
| mPelvis | hindquarters | at (-0.135, 0, 0.574) | | |
| mTorso | barrel (belly) | (0.077, 0, 0.000) | 0.077 | forward, level |
| mChest | barrel (chest) | (0.119, 0, 0.000) | 0.119 | forward, level |
| mNeck | neck base | (0.164, 0, 0.036) | 0.168 | at the front of the chest |
| mHead | poll (top of neck) | (-0.001, 0, 0.162) | 0.162 | straight up: the neck stands vertical |

The pelvis -> neck line is 0.36 forward and 0.04 up: a horizontal spine (84 degrees from a human's
upright one). The eyes sit (0.12, +-0.09, 0.14) from mHead: the VR camera's eye point is their
middle, read from the pupils (mFaceNoseLeft / Right, placed there by the mesh's joint positions).

### Front legs (the arm chain)

| SL joint | Pony part | Offset from parent | Length | Rest direction |
| --- | --- | --- | --- | --- |
| mCollarLeft | scapula (withers) | (0.151, 0.104, 0.028) from mChest | 0.186 | |
| mShoulderLeft | upper arm (humerus) | (-0.008, 0, -0.078) | 0.079 | collar bone points down |
| mElbowLeft | forearm (radius) | (-0.016, -0.020, -0.134) | 0.137 | down, a little back and in |
| mWristLeft | cannon (the "knee" is the wrist) | (0.006, 0, -0.132) | 0.132 | down |
| mHandRing1Left | hoof (a finger) | (0.005, 0, -0.152) | 0.152 | down; the hoof itself 0.105 to the ground |

Shoulder joint to hoof joint: 0.42 (upper arm + forearm + cannon), nearly straight (the elbow
bent about 13 degrees, back). Anatomically this is an arm: the horse's knee is its carpus (wrist),
the cannon its hand (metacarpal), the hoof a finger. So the foreleg maps onto the arm roles one to
one: collar = mCollar, upper arm = mShoulder, forearm = mElbow, hand = mWrist, and the **hoof**
role = mHandRing1.

### Hind legs (the leg chain)

| SL joint | Pony part | Offset from parent | Length | Rest direction |
| --- | --- | --- | --- | --- |
| mHipLeft | hip / thigh (femur) | (-0.001, 0.089, 0.030) from mPelvis | 0.094 | |
| mKneeLeft | stifle / gaskin (tibia) | (-0.017, 0.001, -0.145) | 0.146 | thigh points down |
| mAnkleLeft | hock / cannon | (-0.086, 0.001, -0.060) | 0.105 | gaskin runs back and down |
| mFootLeft | hind hoof (fetlock down) | (-0.026, 0, -0.295) | 0.297 | cannon straight down |

The hock (mAnkle) rests 0.40 above the ground: the hind leg stands on the toe like any
unguligrade leg. The stifle (mKnee) bends forward like a human knee, so the leg solver's two-bone
hip -> knee -> ankle chain fits it as it is; the cannon follows the foot target's turn and the hoof
(mFoot) keeps the animation's pose.

### Scaled or repurposed

- mHandRing1 is repurposed as the front hoof; mFoot is the hind hoof (a human's foot is mFoot ->
  mToe; the pony has no toe).
- mCollar is the scapula: it rests pointing down, not out to the side like a human collarbone.
- The spine is the classic chain laid horizontal by joint positions; nothing is scaled by the rig
  (the avatar's shape scales the whole pony).
- mHindLimb* (the Bento quadruped hind legs) are present in the armature but unweighted.

## How the VR body mapping drives it

The solver works on a human-like **virtual skeleton**: each role gets a frame R (actual world =
virtual world * R), so the solver sees T-pose arms and an upright spine, and its rotations are
converted back onto the pony's joints exactly (a fixture checks the forward kinematics).

- **Forelegs as arms (Arms only and Bipedal).** The upper arm and forearm share one frame taking
  their rest shoulder -> wrist line onto +-Y with the back kept back (so the elbow still bends back
  and no bone rolls about its own axis); the cannon gets its own frame from its rest direction.
  The collar keeps the chest's frame: the scapula stays on the body (turning it as a human collar
  would rotate it 90 degrees out of the shoulder). The hoof is your hand: the controller's grip is
  where the hoof joint goes and the hoof (mHandRing1) takes the controller's turn, bending at its
  joint as a hand does at the wrist. The forearm and cannon are the "forearm" between the elbow and
  that joint (one IK bone, the knee kept at its rest bend) and twist only a forearm's share of the
  hand's roll: the forearm a quarter (at most 0.7 rad), the cannon half in all (at most 1.4 rad),
  the hoof the rest. Pole: the reach turned a quarter turn back in the side view -- back while the
  hoof hangs, down under the leg as it reaches forward, forward overhead -- never out to the side.
- **Reach.** Your hand is taken relative to your shoulder (estimated under your eye: 0.06 back,
  0.11 out, 0.17 down, in eye heights) and scaled by the foreleg's length over your arm's (0.40
  eye heights shoulder to grip), from the pony's own shoulder. Arm out = foreleg out; arm down =
  foreleg hanging as it rests.
- **Bipedal.** The pelvis pitches the barrel up about the hips (84 degrees: the rest pelvis -> neck
  line onto vertical); the neck turns back so the head faces forward on top; the upper forelegs
  and hips turn back so the forelegs hang as arms and the hind legs stand. The hind legs are the
  standing legs (estimate or foot trackers, the target lifted to the hock's rest height); the head
  and neck follow the headset relative to the upright body.
- **Head and neck.** The headset's turn relative to the chest is split between mNeck and mHead: a
  tilt or nod mostly in the head, a share of a turn in the neck.

## Presets (no bone names needed)

| Preset | Body | Forelegs | Head / neck | Hind legs |
| --- | --- | --- | --- | --- |
| Open Pony: Arms only (Auto's pick) | animation (four legs) | controllers, hooves in your hands | headset | animation |
| Open Pony: Bipedal | standing up, follows you | controllers, as arms | headset | your legs (estimate / trackers) |
| Open Pony: Head only | animation | animation | headset | animation |

Setting the spine or chest of a pony profile to Tracked stands it up (the window's "Stands on its
hind legs" switch shows it); unticking the switch puts the hips, spine and hind legs back on the
animation.
