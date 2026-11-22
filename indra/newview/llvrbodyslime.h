/**
 * @file llvrbodyslime.h
 * @brief #1582 VR body tracking read straight from SlimeVR server, independent of the OpenXR
 *        runtime (Virtual Desktop's VDXR, Meta Link, Monado, SteamVR). Two inputs:
 *   - VMC (preferred while its packets arrive): SlimeVR's whole solved skeleton, every humanoid
 *     bone's rotation (hips, spine, chest, upper chest, neck, head, legs, feet, shoulders, arms,
 *     hands, fingers with finger trackers). A bone-pose source: LLVRBody drives the SL skeleton
 *     from the rotations directly (llvrbodyvmc.h), no T-pose offsets, no IK.
 *   - VRChat OSC trackers (the fallback): 8 role trackers (hip, feet, knees, chest, elbows) that
 *     go through the T-pose calibration and the leg IK.
 *
 * Localhost-only UDP listeners (127.0.0.1:SlipstreamVRBodySlimeVMCPort, default 39539, and
 * :SlipstreamVRBodySlimePort, default 9000), non-blocking, polled from the main thread on VR
 * frames (LLVRBodySource); nothing is ever sent. Each datagram goes through
 * slslime::parsePacket (llvrbodyslimeosc.h): bounds-checked, a malformed packet is dropped whole
 * and counted. Fixed memory: one receive buffer and the latest pose per tracker / bone.
 *
 * OSC trackers -> LLXrTrackers roles: 1 hip -> waist, 2 / 3 feet, 4 / 5 knees, 6 chest, 7 / 8
 * elbows; aligned by slslime::solveAlignment (yaw + per-frame head anchor). VMC: the yaw only
 * (the hips' heading against the headset's at the Body press); the avatar's own bone lengths
 * and the headset place the body (LLVRBody). A pose or bone is valid while under 0.3 s old.
 *
 * Log tag "VRBody": listening / bind failure, stream started / stopped, the mode in use (VMC
 * skeleton / OSC trackers), trackers or bones seen when they change, packets/s every 30 s,
 * malformed packet counts, alignment results.
 */
#pragma once

#include "llvrbodyslimeosc.h"
#include "llvrbodysource.h"
#include "vkgfx/llxrsession.h"

#include <chrono>
#include <cstdint>

class LLVRBodySlime
{
public:
    static LLVRBodySlime& instance();

    // Open the sockets on the ports (re-binds when a port changed); drain pending datagrams.
    // Idle (no stream): each socket is read twice a second only.
    void pump(uint32_t oscPort, uint32_t vmcPort);
    // Packets arrived in the last few seconds (either input).
    bool streaming() const { return mOsc.streaming || mVmc.streaming; }
    // A socket is open (stop() has something to do).
    bool listening() const { return mOsc.sock != -1 || mVmc.sock != -1 || streaming(); }
    // This frame's body (app space, aligned): the VMC skeleton when it streams, else the OSC
    // trackers. False: no stream, or no alignment yet (OSC trackers: their roles still listed).
    bool poll(const LLXrHeadPose& hp, LLVRBodyFrame& out);
    // Solve the alignment now (the Body press). False: nothing usable (logged).
    bool calibrate(const LLXrHeadPose& hp, bool provisional = false);
    // Close the sockets and forget the streams (and the alignments: SlimeVR's space may change).
    void stop();

    ~LLVRBodySlime();

private:
    using Clock = std::chrono::steady_clock;
    LLVRBodySlime() = default;

    struct Listener
    {
        const char* label = "";          // "OSC" / "VMC" (logs)
        intptr_t sock = -1;
        uint32_t port = 0;
        bool bindFailed = false;         // (logged once per port)
        uint32_t failedPort = 0;
        Clock::time_point lastRead{};
        Clock::time_point lastPacket{};
        bool streaming = false;
        Clock::time_point statStart{};
        uint32_t packets = 0, malformed = 0, truncated = 0, totalMalformed = 0;
    };
    bool openSocket(Listener& l, uint32_t port);
    void closeSocket(Listener& l);
    void pumpOne(Listener& l, uint32_t port, Clock::time_point now);
    void drain(Listener& l, Clock::time_point now);
    void streamStopped(Listener& l);
    void merge(Listener& l, const slslime::Packet& p, Clock::time_point now);
    void logStats(Listener& l, Clock::time_point now);
    void logRoles();
    void logBones();
    bool vmcMode() const { return mVmc.streaming; }
    bool calibrateOsc(const LLXrHeadPose& hp, bool provisional);
    bool calibrateVmc(const LLXrHeadPose& hp, bool provisional);
    bool pollOsc(const LLXrHeadPose& hp, LLVRBodyFrame& out, Clock::time_point now);
    bool pollVmc(const LLXrHeadPose& hp, LLVRBodyFrame& out, Clock::time_point now);
    // The VMC globals (OpenXR axes, SlimeVR's space) of the fresh bones.
    void vmcGlobals(Clock::time_point now, bool have[slvmc::U_COUNT], slfbt::Q global[slvmc::U_COUNT]) const;

    struct Slot
    {
        Clock::time_point posTime{}, rotTime{};
        slfbt::V3 pos;       // OpenXR axes, SlimeVR space
        slfbt::Q rot;
    };
    struct BoneSlot
    {
        Clock::time_point time{};
        slfbt::Q local;      // OpenXR axes
    };
    Listener mOsc{"OSC"};
    Listener mVmc{"VMC"};
    bool mWsa = false;
    int mWsaUsers = 0;
    bool mLoggedVmcMode = false;       // the mode logged last (VMC or OSC)
    bool mModeLogged = false;
    // OSC trackers.
    Slot mSlot[slslime::T_COUNT];
    uint32_t mSeenMask = 0;            // slots seen in this stream (logged on change)
    uint32_t mGeneration = 1;
    slslime::Alignment mAlign;
    bool mAlignProvisional = false;
    bool mResetPending = false;        // SlimeVR sent its reset heading: re-solve the yaw
    slfbt::Q mResetHeading;
    // VMC bones.
    BoneSlot mBone[slvmc::U_COUNT];
    uint64_t mBoneMask = 0;            // bones seen in this stream (logged on change)
    uint32_t mVmcGeneration = 1;
    bool mVmcAligned = false;
    bool mVmcProvisional = false;
    float mVmcYaw = 0.f;               // SlimeVR space -> app space, about +Y
    unsigned char mBuf[16384];         // a VMC bundle with fingers is ~6 KB
};
