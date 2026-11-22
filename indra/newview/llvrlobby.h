/**
 * @file llvrlobby.h
 * @brief VR milestone M3 (#1076, epic #1072): the login room.
 *
 * Before the world shows (the login screen, then the login's progress), the headset shows a
 * dark room: a dark glossy 3D doorway whose opening glows, lighting the frame, the floor and the
 * fog rolling around it, with the login screen on the UI wall (as before). The doorway stands to the right of the wall
 * (62 degrees off its centre, 2.3 m from where you stood when the wall was placed), facing you.
 * On the login screen, pointing a controller at it lights it up (a haptic tick), and the
 * trigger -- or physically walking through it -- logs in, exactly as the login panel's Log In
 * button (the LLPanelLogin event API's onClickConnect). When the world is ready it fades in
 * around you (LLVRInput's comfort fade) and the room is gone.
 *
 * No world renderer runs before login, so the room is not world geometry: LLXrSession::setLobby
 * hands its placement to the XR session, which draws it into the eye images with its own small
 * pass (#1596, clean_vr_lobby.frag: ray-cast doorway, emissive opening as an area light, fog,
 * glare), built in the background on the first room frame.
 *
 * Desktop (no VR): update() is only called from LLVRInput::update, which returns first.
 */
#pragma once

struct LLXrInputState;
struct LLXrHeadPose;

class LLVRLobby
{
public:
    // LLVRInput::update, after the UI wall's layers were set (VR only). inWorld: the
    // head-tracked world is shown (the room is then off).
    static void update(const LLXrInputState& in, const LLXrHeadPose& hp, bool inWorld);
};
