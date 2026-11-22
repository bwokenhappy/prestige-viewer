/**
 * @file llvrbodyslime.cpp
 * @brief #1582 SlimeVR body tracking over VMC (its skeleton) and VRChat OSC trackers. See
 *        llvrbodyslime.h; the parser and the alignment math are llvrbodyslimeosc.h, the VMC
 *        bones llvrbodyvmc.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrbodyslime.h"

#include "llformat.h"

#include <cmath>
#include <string>

#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
using namespace slslime;
constexpr float STALE_SECONDS = 0.3f;      // a tracker's / bone's pose older than this is not valid
constexpr float STREAM_TIMEOUT = 3.f;      // no packet for this long: the stream stopped
constexpr float IDLE_READ_INTERVAL = 0.5f; // without a stream, read the socket this often
constexpr float BIND_RETRY = 10.f;         // a refused port is retried this often (quietly)
constexpr float STATS_INTERVAL = 30.f;
constexpr int MAX_DATAGRAMS_PER_PUMP = 256;

// SlimeVR's tracker number -> the OpenXR tracker role it fills.
const uint32_t kRoleOf[T_COUNT] = {LLXR_TRK_COUNT,      LLXR_TRK_WAIST,      LLXR_TRK_LEFT_FOOT,
                                   LLXR_TRK_RIGHT_FOOT, LLXR_TRK_LEFT_KNEE,  LLXR_TRK_RIGHT_KNEE,
                                   LLXR_TRK_CHEST,      LLXR_TRK_LEFT_ELBOW, LLXR_TRK_RIGHT_ELBOW};

float secondsBetween(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{
    return std::chrono::duration<float>(b - a).count();
}
bool fresh(std::chrono::steady_clock::time_point t, std::chrono::steady_clock::time_point now)
{
    return t != std::chrono::steady_clock::time_point{} && secondsBetween(t, now) < STALE_SECONDS;
}

std::string slotList(uint32_t mask)
{
    std::string s;
    for (int i = 0; i < T_COUNT; ++i)
        if (mask & (1u << i)) s += (s.empty() ? "" : ", ") + std::string(slotName(i));
    return s.empty() ? std::string("none") : s;
}

// The VMC bones seen, grouped (body bones by name, fingers as a count).
std::string boneList(uint64_t mask)
{
    std::string s;
    int fingers = 0;
    for (int b = 0; b < slvmc::U_COUNT; ++b)
    {
        if (!(mask & (uint64_t(1) << b))) continue;
        if (b >= slvmc::U_L_FINGER0) ++fingers;
        else s += (s.empty() ? "" : ", ") + std::string(slvmc::uboneInfo(b).name);
    }
    if (fingers) s += (s.empty() ? "" : ", ") + std::to_string(fingers) + " finger bones";
    return s.empty() ? std::string("none") : s;
}

slfbt::Q hmdRotOf(const LLXrHeadPose& hp)
{
    return slfbt::qnormalized(slfbt::Q(hp.orientation[0], hp.orientation[1], hp.orientation[2], hp.orientation[3]));
}

// The body roles a VMC skeleton stands for (the wrist's Body button, the eye-height calibration).
const uint32_t kVmcRoles[6] = {LLXR_TRK_WAIST, LLXR_TRK_CHEST, LLXR_TRK_LEFT_FOOT, LLXR_TRK_RIGHT_FOOT,
                               LLXR_TRK_LEFT_KNEE, LLXR_TRK_RIGHT_KNEE};
} // namespace

LLVRBodySlime& LLVRBodySlime::instance()
{
    static LLVRBodySlime s;
    return s;
}

LLVRBodySlime::~LLVRBodySlime()
{
    closeSocket(mOsc);
    closeSocket(mVmc);
}

// ---- Sockets ---------------------------------------------------------------------------------
bool LLVRBodySlime::openSocket(Listener& l, uint32_t port)
{
    closeSocket(l);
    if (port == 0 || port > 65535) return false;
#if LL_WINDOWS
    if (!mWsa)
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        mWsa = true;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    u_long nb = 1;
    if (ioctlsocket(s, FIONBIO, &nb) != 0)
    {
        closesocket(s);
        return false;
    }
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    const int fl = fcntl(s, F_GETFL);
    if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) != 0 || fcntl(s, F_SETFD, FD_CLOEXEC) != 0)
    {
        close(s);
        return false;
    }
#endif
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // localhost only: nothing from the network
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0)
    {
#if LL_WINDOWS
        closesocket(s);
#else
        close(s);
#endif
        return false;
    }
    l.sock = (intptr_t)s;
    ++mWsaUsers;
    return true;
}

void LLVRBodySlime::closeSocket(Listener& l)
{
    if (l.sock == -1) return;
#if LL_WINDOWS
    closesocket((SOCKET)l.sock);
#else
    close((int)l.sock);
#endif
    l.sock = -1;
    if (--mWsaUsers <= 0)
    {
        mWsaUsers = 0;
#if LL_WINDOWS
        if (mWsa)
        {
            WSACleanup();
            mWsa = false;
        }
#endif
    }
}

void LLVRBodySlime::stop()
{
    const bool wasOpen = mOsc.sock != -1 || mVmc.sock != -1;
    const bool wasStreaming = streaming();
    closeSocket(mOsc);
    closeSocket(mVmc);
    if (wasStreaming) LL_INFOS("VRBody") << "SlimeVR: stopped listening (VR off or another source chosen)" << LL_ENDL;
    else if (wasOpen) LL_INFOS("VRBody") << "SlimeVR: stopped listening" << LL_ENDL;
    mOsc = Listener{"OSC"};
    mVmc = Listener{"VMC"};
    mModeLogged = false;
    mSeenMask = 0;
    mAlign = Alignment{};
    mAlignProvisional = false;
    mResetPending = false;
    for (Slot& s : mSlot) s = Slot{};
    ++mGeneration;
    mBoneMask = 0;
    for (BoneSlot& b : mBone) b = BoneSlot{};
    mVmcAligned = mVmcProvisional = false;
    ++mVmcGeneration;
}

void LLVRBodySlime::pump(uint32_t oscPort, uint32_t vmcPort)
{
    const Clock::time_point now = Clock::now();
    pumpOne(mVmc, vmcPort, now);
    pumpOne(mOsc, oscPort, now);
    if (streaming() && (!mModeLogged || mLoggedVmcMode != vmcMode()))
    {
        mModeLogged = true;
        mLoggedVmcMode = vmcMode();
        LL_INFOS("VRBody") << "SlimeVR mode: "
                           << (vmcMode() ? "VMC skeleton (every bone; preferred)"
                                         : "VRChat OSC trackers (8 trackers; turn on SlimeVR's VMC output for the whole skeleton)")
                           << LL_ENDL;
    }
}

void LLVRBodySlime::pumpOne(Listener& l, uint32_t port, Clock::time_point now)
{
    if (l.sock == -1 || port != l.port)
    {
        if (l.sock != -1) closeSocket(l);
        // A refused port is retried every BIND_RETRY seconds, logged once.
        if (l.bindFailed && port == l.failedPort && secondsBetween(l.lastRead, now) < BIND_RETRY) return;
        l.lastRead = now;
        l.port = port;
        if (!openSocket(l, port))
        {
            if (!l.bindFailed || l.failedPort != port)
            {
                LL_WARNS("VRBody") << "SlimeVR " << l.label << ": cannot listen on 127.0.0.1:" << port
                                   << " (another program, e.g. VRChat, has the port?). Set SlimeVR's port and "
                                   << (&l == &mVmc ? "SlipstreamVRBodySlimeVMCPort" : "SlipstreamVRBodySlimePort")
                                   << " to a free one; retrying quietly every " << (int)BIND_RETRY << " s" << LL_ENDL;
            }
            l.bindFailed = true;
            l.failedPort = port;
            return;
        }
        l.bindFailed = false;
        LL_INFOS("VRBody") << "SlimeVR " << l.label << ": listening on 127.0.0.1:" << port
                           << (&l == &mVmc ? " (SlimeVR server: Settings > OSC > VMC, port out " : " (SlimeVR server: Settings > OSC > VRChat OSC Trackers, port out ")
                           << port << ")" << LL_ENDL;
    }
    if (!l.streaming && secondsBetween(l.lastRead, now) < IDLE_READ_INTERVAL) return;
    l.lastRead = now;
    drain(l, now);
    if (l.streaming && secondsBetween(l.lastPacket, now) > STREAM_TIMEOUT) streamStopped(l);
    if (l.streaming && secondsBetween(l.statStart, now) >= STATS_INTERVAL) logStats(l, now);
}

void LLVRBodySlime::streamStopped(Listener& l)
{
    LL_INFOS("VRBody") << "SlimeVR " << l.label << ": stream stopped (no packets for " << (int)STREAM_TIMEOUT << " s)"
                       << LL_ENDL;
    l.streaming = false;
    if (&l == &mVmc)
    {
        mBoneMask = 0;
        for (BoneSlot& b : mBone) b = BoneSlot{};
        ++mVmcGeneration;
    }
    else
    {
        mSeenMask = 0;
        for (Slot& s : mSlot) s = Slot{};
        ++mGeneration;
    }
}

void LLVRBodySlime::logStats(Listener& l, Clock::time_point now)
{
    const float span = secondsBetween(l.statStart, now);
    const bool vmc = &l == &mVmc;
    LL_INFOS("VRBody") << llformat("SlimeVR %s: %.0f packets/s", l.label, l.packets / span)
                       << (vmc ? ", bones: " + boneList(mBoneMask) : ", trackers: " + slotList(mSeenMask))
                       << (l.malformed ? llformat(", %u malformed packets dropped", l.malformed) : std::string())
                       << (l.truncated ? llformat(", %u oversized", l.truncated) : std::string())
                       << (vmc ? (mVmcAligned ? (mVmcProvisional ? ", provisional alignment" : ", aligned")
                                              : ", not aligned (wrist panel > Body)")
                               : (mAlign.valid ? (mAlignProvisional ? ", provisional alignment" : ", aligned")
                                               : ", not aligned (wrist panel > Body)"))
                       << (vmc || !mVmc.streaming ? "" : " (not used: VMC streams)") << LL_ENDL;
    l.statStart = now;
    l.packets = l.malformed = l.truncated = 0;
}

void LLVRBodySlime::drain(Listener& l, Clock::time_point now)
{
    for (int n = 0; n < MAX_DATAGRAMS_PER_PUMP && l.sock != -1; ++n)
    {
        sockaddr_in from;
        std::memset(&from, 0, sizeof from);
#if LL_WINDOWS
        int fromLen = sizeof from;
        const int got = recvfrom((SOCKET)l.sock, reinterpret_cast<char*>(mBuf), (int)sizeof mBuf, 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got == SOCKET_ERROR)
        {
            const int e = WSAGetLastError();
            if (e == WSAEMSGSIZE)   // larger than any SlimeVR packet: not ours
            {
                ++l.truncated;
                continue;
            }
            if (e != WSAEWOULDBLOCK && e != WSAECONNRESET)
                LL_WARNS_ONCE("VRBody") << "SlimeVR: receive error " << e << LL_ENDL;
            return;
        }
        const size_t len = (size_t)got;
#else
        socklen_t fromLen = sizeof from;
#if LL_LINUX
        const int flags = MSG_TRUNC;   // (returns the datagram's real size)
#else
        const int flags = 0;
#endif
        const ssize_t got = recvfrom((int)l.sock, mBuf, sizeof mBuf, flags, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got < 0)
        {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                LL_WARNS_ONCE("VRBody") << "SlimeVR: receive error " << errno << LL_ENDL;
            return;
        }
        const size_t len = (size_t)got;
        if (len >= sizeof mBuf)   // (truncated: larger than any SlimeVR packet)
        {
            ++l.truncated;
            continue;
        }
#endif
        // Bound to loopback, so only local senders reach us; check anyway.
        if (from.sin_family != AF_INET || (ntohl(from.sin_addr.s_addr) >> 24) != 127) continue;
        Packet p;
        if (!parsePacket(mBuf, len, p))
        {
            ++l.malformed;
            if (++l.totalMalformed == 1)
                LL_WARNS("VRBody") << "SlimeVR " << l.label << ": dropped a malformed packet (" << len
                                   << " bytes); further ones are counted in the periodic line" << LL_ENDL;
            continue;
        }
        // (Each port takes only its own format: VMC bones on the VMC port, trackers on the OSC one.)
        if (&l == &mVmc ? !p.vmcBones : !p.messages) continue;
        ++l.packets;
        merge(l, p, now);
    }
}

void LLVRBodySlime::merge(Listener& l, const Packet& p, Clock::time_point now)
{
    if (!l.streaming)
    {
        l.streaming = true;
        l.statStart = now;
        l.packets = 1;
        l.malformed = l.truncated = 0;
        LL_INFOS("VRBody") << "SlimeVR " << l.label << ": stream started on port " << l.port << LL_ENDL;
    }
    l.lastPacket = now;
    if (&l == &mVmc)
    {
        uint64_t seen = mBoneMask;
        for (int b = 0; b < slvmc::U_COUNT; ++b)
        {
            if (!p.boneHave[b]) continue;
            mBone[b].local = slvmc::unityRotToXr(p.boneRot[b]);
            mBone[b].time = now;
            seen |= uint64_t(1) << b;
        }
        if (seen != mBoneMask)
        {
            mBoneMask = seen;
            ++mVmcGeneration;
            logBones();
        }
        return;
    }
    uint32_t seen = mSeenMask;
    for (int i = 0; i < T_COUNT; ++i)
    {
        const Sample& s = p.t[i];
        Slot& sl = mSlot[i];
        if (s.hasPos)
        {
            sl.pos = unityPosToXr(s.pos);
            sl.posTime = now;
        }
        if (s.hasRot)
        {
            sl.rot = unityEulerToXr(s.euler);
            sl.rotTime = now;
            if (i == T_HEAD)
            {
                // Only sent on a SlimeVR reset: its new heading. Re-solve our yaw next frame.
                mResetHeading = sl.rot;
                mResetPending = true;
            }
        }
        if (i != T_HEAD && s.hasPos && s.hasRot) seen |= 1u << i;
        if (i == T_HEAD && s.hasPos) seen |= 1u;
    }
    if (seen != mSeenMask)
    {
        mSeenMask = seen;
        ++mGeneration;
        logRoles();
    }
}

void LLVRBodySlime::logRoles()
{
    LL_INFOS("VRBody") << "SlimeVR OSC trackers seen: " << slotList(mSeenMask)
                       << ((mSeenMask & 1u) ? "" : " (no head reference: anchored on the headset alone)") << LL_ENDL;
}

void LLVRBodySlime::logBones()
{
    LL_INFOS("VRBody") << "SlimeVR VMC bones seen: " << boneList(mBoneMask)
                       << ((mBoneMask & (uint64_t(1) << slvmc::U_HIPS)) ? "" : " (no Hips yet: nothing can be driven)")
                       << LL_ENDL;
}

// ---- Alignment -------------------------------------------------------------------------------
bool LLVRBodySlime::calibrate(const LLXrHeadPose& hp, bool provisional)
{
    if (!streaming() || !hp.valid)
    {
        if (!provisional) LL_WARNS("VRBody") << "SlimeVR alignment: no SlimeVR stream or no headset pose" << LL_ENDL;
        return false;
    }
    return vmcMode() ? calibrateVmc(hp, provisional) : calibrateOsc(hp, provisional);
}

void LLVRBodySlime::vmcGlobals(Clock::time_point now, bool have[slvmc::U_COUNT], slfbt::Q global[slvmc::U_COUNT]) const
{
    bool haveLocal[slvmc::U_COUNT];
    slfbt::Q local[slvmc::U_COUNT];
    for (int b = 0; b < slvmc::U_COUNT; ++b)
    {
        haveLocal[b] = fresh(mBone[b].time, now);
        local[b] = mBone[b].local;
    }
    slvmc::composeGlobals(haveLocal, local, have, global);
}

bool LLVRBodySlime::calibrateVmc(const LLXrHeadPose& hp, bool provisional)
{
    bool have[slvmc::U_COUNT];
    slfbt::Q g[slvmc::U_COUNT];
    vmcGlobals(Clock::now(), have, g);
    if (!have[slvmc::U_HIPS])
    {
        if (!provisional) LL_WARNS("VRBody") << "SlimeVR VMC alignment failed: no Hips bone (seen: " << boneList(mBoneMask) << ")" << LL_ENDL;
        return false;
    }
    // You stand straight facing where the headset faces: the hips' heading is the headset's.
    mVmcYaw = wrapAngle(headingOf(hmdRotOf(hp)) - headingOf(g[slvmc::U_HIPS]));
    mVmcAligned = true;
    mVmcProvisional = provisional;
    LL_INFOS("VRBody") << (provisional ? "SlimeVR VMC alignment (provisional, from the first packets; wrist panel > Body "
                                         "for an exact one): "
                                       : "SlimeVR VMC alignment: ")
                       << llformat("yaw %.1f deg from the hips' heading", mVmcYaw * 57.29578f) << LL_ENDL;
    return true;
}

bool LLVRBodySlime::calibrateOsc(const LLXrHeadPose& hp, bool provisional)
{
    const Clock::time_point now = Clock::now();
    AlignInput in;
    for (int i = 0; i < T_COUNT; ++i)
    {
        const Slot& sl = mSlot[i];
        in.have[i] = fresh(sl.posTime, now) && (i == T_HEAD || fresh(sl.rotTime, now));
        in.pos[i] = sl.pos;
        in.rot[i] = sl.rot;
    }
    const V3 hmdPos(hp.position[0], hp.position[1], hp.position[2]);
    YawFrom from = YawFrom::NONE;
    const Alignment a = solveAlignment(in, hmdPos, hmdRotOf(hp), hp.floorSpace, 0.f, from);
    if (!a.valid)
    {
        if (!provisional)
            LL_WARNS("VRBody") << "SlimeVR alignment failed: need the feet (or the hip) and a left / right pair or the hip "
                                  "tracker (seen: "
                               << slotList(mSeenMask) << ")" << LL_ENDL;
        return false;
    }
    mAlign = a;
    mAlignProvisional = provisional;
    mResetPending = false;
    LL_INFOS("VRBody") << (provisional ? "SlimeVR alignment (provisional, from the first packets; wrist panel > Body for an "
                                         "exact one): "
                                       : "SlimeVR alignment: ")
                       << llformat("yaw %.1f deg from %s; head offset (%.2f, %.2f, %.2f) m, %s", a.yaw * 57.29578f,
                                   yawFromName(from), a.headOffset.x, a.headOffset.y, a.headOffset.z,
                                   (mSeenMask & 1u) ? "SlimeVR head reference" : "no SlimeVR head (origin)")
                       << (hp.floorSpace ? "; feet at the floor" : "; LOCAL space: feet at SlimeVR's own height") << LL_ENDL;
    return true;
}

// ---- This frame ------------------------------------------------------------------------------
bool LLVRBodySlime::poll(const LLXrHeadPose& hp, LLVRBodyFrame& out)
{
    out = LLVRBodyFrame{};
    if (!streaming()) return false;
    const Clock::time_point now = Clock::now();
    return vmcMode() ? pollVmc(hp, out, now) : pollOsc(hp, out, now);
}

bool LLVRBodySlime::pollVmc(const LLXrHeadPose& hp, LLVRBodyFrame& out, Clock::time_point now)
{
    if (!mVmcAligned && hp.placeable()) calibrateVmc(hp, true);
    bool have[slvmc::U_COUNT];
    slfbt::Q g[slvmc::U_COUNT];
    vmcGlobals(now, have, g);
    if (!(mBoneMask & (uint64_t(1) << slvmc::U_HIPS))) return false;
    out.bonePoses = true;
    out.poses.generation = mVmcGeneration * 2;   // (even: never equal to an OSC generation)
    // The roles it stands for (the Body button; LLVRBody's eye-height mapping), never located:
    // the skeleton itself drives the avatar.
    for (uint32_t r : kVmcRoles)
    {
        out.poses.role[r].active = true;
        ++out.poses.connected;
    }
    if (!mVmcAligned || !have[slvmc::U_HIPS]) return true;   // (listed, nothing to drive yet)
    out.skeleton = true;
    const slfbt::Q R = yawQ(mVmcYaw);
    for (int b = 0; b < slvmc::U_COUNT; ++b)
    {
        out.skelHave[b] = have[b];
        out.skelRot[b] = have[b] ? slfbt::qnormalized(slfbt::qmul(R, g[b])) : slfbt::Q();
    }
    return true;
}

bool LLVRBodySlime::pollOsc(const LLXrHeadPose& hp, LLVRBodyFrame& out, Clock::time_point now)
{
    LLXrTrackers& tr = out.poses;
    if (!mAlign.valid && hp.placeable()) calibrateOsc(hp, true);
    if (mResetPending && mAlign.valid && hp.placeable())
    {
        mResetPending = false;
        const float before = mAlign.yaw;
        realignYaw(mAlign, mResetHeading, hmdRotOf(hp));
        LL_INFOS("VRBody") << llformat("SlimeVR reset seen: yaw re-aligned %.1f -> %.1f deg (if your avatar's body now "
                                       "faces the wrong way, wrist panel > Body)",
                                       before * 57.29578f, mAlign.yaw * 57.29578f)
                           << LL_ENDL;
    }
    const Slot& head = mSlot[T_HEAD];
    const bool headOk = head.posTime != Clock::time_point{};   // (the last one: it moves with the body)
    const V3 slimeHead = headOk ? head.pos : V3();
    const V3 hmdPos(hp.position[0], hp.position[1], hp.position[2]);
    const Q hmdRot = hmdRotOf(hp);
    const bool aligned = mAlign.valid && hp.valid;
    for (int i = 1; i < T_COUNT; ++i)
    {
        if (!(mSeenMask & (1u << i))) continue;
        LLXrTrackerPose& tp = tr.role[kRoleOf[i]];
        tp.active = true;
        ++tr.connected;
        const Slot& sl = mSlot[i];
        if (!fresh(sl.posTime, now) || !fresh(sl.rotTime, now) || !aligned) continue;
        V3 p;
        Q q;
        applyAlignment(mAlign, slimeHead, hmdPos, hmdRot, sl.pos, sl.rot, p, q);
        if (!slfbt::finite(p) || !slfbt::finite(q)) continue;
        tp.valid = tp.tracked = true;
        tp.position[0] = p.x;
        tp.position[1] = p.y;
        tp.position[2] = p.z;
        tp.orientation[0] = q.x;
        tp.orientation[1] = q.y;
        tp.orientation[2] = q.z;
        tp.orientation[3] = q.w;
    }
    tr.generation = mGeneration * 2 + 1;   // (odd: never equal to a VMC generation's even)
    return tr.connected > 0;
}
