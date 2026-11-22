/**
 * @file slpossync.cpp
 * @brief Avatar position sync between Slipstream viewers over the helper (#1417).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slpossync.h"

#include "llagent.h"
#include "lleventtimer.h"
#include "llframetimer.h"
#include "llmutelist.h"
#include "llrand.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "rlvactions.h"
#include "slbridge.h"

#include <algorithm>

namespace
{
constexpr char BRIDGE_USER[]        = "possync";
constexpr char LINE_TYPE[]          = "pos/1";      // "#slb:pos/1|<owner>|<fields>" (slpossyncmath.h)
constexpr char SLOT_POS[]           = "pos";        // SLBridge::sendLatest slots
constexpr char SLOT_SEE[]           = "see";
// Sending: SLPosSyncMath::Sender decides when (#1526: only once settled, one release on the
// move). Its thresholds sit above the helper's own (4 cm, 4 degrees, one line per 0.2 s), so
// the helper never drops what it decided to send.
constexpr F32  TICK_PERIOD          = 0.1f;
// Movement input: any of these held means the avatar is moving (or about to), not settled.
// Turning (yaw, mouselook) does not move it.
constexpr U32  MOVE_CONTROLS        = AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG | AGENT_CONTROL_LEFT_POS
                                      | AGENT_CONTROL_LEFT_NEG | AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG
                                      | AGENT_CONTROL_FAST_AT | AGENT_CONTROL_FAST_LEFT | AGENT_CONTROL_FAST_UP
                                      | AGENT_CONTROL_NUDGE_AT_POS | AGENT_CONTROL_NUDGE_AT_NEG
                                      | AGENT_CONTROL_NUDGE_LEFT_POS | AGENT_CONTROL_NUDGE_LEFT_NEG
                                      | AGENT_CONTROL_NUDGE_UP_POS | AGENT_CONTROL_NUDGE_UP_NEG;
// The visible list: avatars within shout range (nothing further is heard) and draw distance,
// nearest to the camera first, at most MAX_SEE (the helper's POS_MAX_SEE; 24 keys fit a line).
constexpr size_t MAX_SEE            = 24;
constexpr F32  SEE_RANGE            = 100.f;
constexpr F32  SEE_CHECK            = 2.f;
constexpr F32  SEE_MIN_INTERVAL     = 3.f;
constexpr F32  SEE_REFRESH          = 60.f;
constexpr F32  STATS_INTERVAL       = 20.f;    // the #PosSync summary line (only while lines arrive)

class SLPosSyncTicker : public LLEventTimer
{
public:
    SLPosSyncTicker(F32 period, std::function<void()> cb) : LLEventTimer(period), mCallback(std::move(cb)) {}
    bool tick() override
    {
        if (mCallback)
            mCallback();
        return false;
    }

private:
    std::function<void()> mCallback;
};

SLPosSyncMath::Vec3 toVec(const LLVector3d& v)
{
    return SLPosSyncMath::Vec3(v.mdV[VX], v.mdV[VY], v.mdV[VZ]);
}

SLPosSyncMath::Quat toQuat(const LLQuaternion& q)
{
    SLPosSyncMath::Quat out;
    out.x = q.mQ[VX];
    out.y = q.mQ[VY];
    out.z = q.mQ[VZ];
    out.w = q.mQ[VW];
    return out;
}

LLVOAvatar* findOtherAvatar(const LLUUID& id)
{
    LLViewerObject* object = gObjectList.findObject(id);
    if (!object || object->isDead() || !object->isAvatar())
        return nullptr;
    LLVOAvatar* avatar = (LLVOAvatar*)object;
    return (avatar->isSelf() || avatar->isControlAvatar()) ? nullptr : avatar;
}
}

SLPosSync::SLPosSync()
{
}

SLPosSync::~SLPosSync()
{
    mBridgeConnection.disconnect();
    mReadyInRegionConnection.disconnect();
    delete mTicker;
    mTicker = nullptr;
}

void SLPosSync::init()
{
    if (mInitialized)
        return;
    mInitialized = true;
    mSeq = (U16)ll_rand(65536);

    SLBridge& bridge = SLBridge::instance();
    bridge.init();
    bridge.registerHandler(LINE_TYPE, [this](const std::string& rest) { onLine(rest); });
    mBridgeConnection = bridge.setStateCallback([this](SLBridge::EState state)
        {
            if (state == SLBridge::STATE_READY)
            {
                // A fresh (or restarted) script knows nothing: tell it everything again.
                mShareKnown = false;
                mSender.forget();
                mSeeForce = true;
            }
        });
    mReadyInRegionConnection = bridge.setReadyInRegionCallback([this]()
        {
            mSender.forget();
            mSeeForce = true;
        });
    // The helper is wanted only while sharing or receiving is on (tick() keeps this current).
    mBridgeWanted = isShareOptionOn() || isReceiveOptionOn();
    if (mBridgeWanted)
        bridge.addUser(BRIDGE_USER);

    if (!mTicker)
        mTicker = new SLPosSyncTicker(TICK_PERIOD, [this]() { tick(); });
}

// static
bool SLPosSync::isShareOptionOn()
{
    static LLCachedControl<bool> share(gSavedSettings, "SLPosSyncShare", false);
    return share;
}

// static
bool SLPosSync::isReceiveOptionOn()
{
    static LLCachedControl<bool> receive(gSavedSettings, "SLPosSyncReceive", false);
    return receive;
}

// static
bool SLPosSync::rlvHidesPosition()
{
    // Any restriction that hides where people are (location, names, nearby lists, maps) also
    // keeps our own precise position to ourselves; so does a block on the channel it uses.
    if (!RlvActions::isRlvEnabled())
        return false;
    return !RlvActions::canShowLocation() || !RlvActions::canShowNearbyAgents()
           || RlvActions::hasBehaviour(RLV_BHVR_SHOWNAMES) || RlvActions::hasBehaviour(RLV_BHVR_SHOWNAMETAGS)
           || RlvActions::hasBehaviour(RLV_BHVR_SHOWMINIMAP) || RlvActions::hasBehaviour(RLV_BHVR_SHOWWORLDMAP)
           || !RlvActions::canSendChannel(SLBridge::POS_CHANNEL);
}

void SLPosSync::tick()
{
    const bool wanted = isShareOptionOn() || isReceiveOptionOn();
    if (wanted != mBridgeWanted)
    {
        mBridgeWanted = wanted;
        if (wanted)
            SLBridge::instance().addUser(BRIDGE_USER);
        else
            SLBridge::instance().removeUser(BRIDGE_USER);
    }
    if (!isAgentAvatarValid() || !gAgent.getRegion())
        return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    mTable.expire(now, [this](const LLUUID& id) { dropAvatar(id); });
    tickSee();
    tickSend();

    if (mStatsTimer.getElapsedTimeF32() > STATS_INTERVAL)
    {
        mStatsTimer.reset();
        // gap: sim-driven placement vs where each sender draws itself (the desync without
        // this feature); residual: what is left on screen after the blend.
        // #1526: let go = the sim moved a drawn avatar (hand-off started); walked off = a still
        // line ended by the 0.2 m rule.
        double gap_sum = 0.0, gap_max = 0.0, resid_sum = 0.0, resid_max = 0.0;
        unsigned frames = 0, let_go = 0, walked_off = 0;
        mTable.forEach([&](const LLUUID& id)
            {
                if (LLVOAvatar* avatar = findOtherAvatar(id))
                    avatar->mPosSyncView.takeStats(gap_sum, gap_max, resid_sum, resid_max, frames, let_go, walked_off);
            });
        if (mAccepted || mRejected)
        {
            LL_INFOS("PosSync") << "Followed " << mTable.size() << " avatar(s); lines used " << mAccepted
                                << ", rejected " << mRejected << " in the last " << (S32)STATS_INTERVAL << " s; gap mean "
                                << llformat("%.3f", frames ? gap_sum / frames : 0.0) << " max " << llformat("%.3f", gap_max)
                                << " m, residual mean " << llformat("%.3f", frames ? resid_sum / frames : 0.0) << " max "
                                << llformat("%.3f", resid_max) << " m over " << frames << " frames; let go " << let_go
                                << ", walked off " << walked_off << LL_ENDL;
        }
        mAccepted = 0;
        mRejected = 0;
    }
}

void SLPosSync::sendShareState(bool share)
{
    SLBridge& bridge = SLBridge::instance();
    if (!bridge.isReady() || (mShareKnown && share == mShareSent))
        return;
    bridge.sendControl(std::string("pos/share/1|") + (share ? "1" : "0"));
    mShareKnown = true;
    mShareSent = share;
    if (!share)
        mSender.forget();
}

void SLPosSync::tickSend()
{
    SLBridge& bridge = SLBridge::instance();
    const bool share = SLPosSyncMath::shouldShare(isShareOptionOn(), rlvHidesPosition(), bridge.isReady());
    sendShareState(share);
    if (!share || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
        return;
    LLVOAvatarSelf* self = gAgentAvatarp.get();
    LLViewerRegion* region = self ? self->getRegion() : nullptr;
    if (!self || !region || !self->getRootJoint())
        return;

    // What our own screen shows: the drawn position (the one other viewers compare with their
    // own sim-driven one), the velocity they extrapolate with, the root's rotation.
    const LLVector3 pos = region->getPosRegionFromAgent(self->getRenderPosition());
    const SLPosSyncMath::Vec3 pos_global = toVec(gAgent.getPosGlobalFromAgent(self->getRenderPosition()));
    const LLQuaternion rot = self->getRootJoint()->getWorldRotation();
    LLVector3 vel = self->getVelocity();
    const bool move_input = (gAgent.getControlFlags() & MOVE_CONTROLS) || gAgent.getAutoPilot();
    const bool new_region = region->getHandle() != mSentRegion;
    const F64 now = LLFrameTimer::getElapsedSeconds();

    // #1526: only once settled (a still line), one release (hold) line on the move, nothing
    // while moving; seated as before. Receivers draw still lines only.
    U8 flags = 0;
    if (!mSender.decide(pos_global, vel.length(), toQuat(rot), self->isSitting(), gAgent.getFlying(), move_input,
                        new_region, now, &flags))
    {
        return;
    }
    if (flags & SLPosSyncMath::FLAG_STILL)
        vel.clear();

    std::string flag_text;
    if (flags & SLPosSyncMath::FLAG_SIT)
        flag_text += 'S';
    if (flags & SLPosSyncMath::FLAG_STILL)
        flag_text += 'T';
    if (flags & SLPosSyncMath::FLAG_FLY)
        flag_text += 'F';
    const U16 seq = (U16)(mSeq + 1);
    // Numbers through formatFixed, never printf: a user locale with a decimal comma would
    // break the helper's vector casts.
    const double p[3] = { pos.mV[VX], pos.mV[VY], pos.mV[VZ] };
    const double v[3] = { vel.mV[VX], vel.mV[VY], vel.mV[VZ] };
    const double q[4] = { rot.mQ[VX], rot.mQ[VY], rot.mQ[VZ], rot.mQ[VW] };
    const std::string line = "pos/send/1|" + std::to_string((U32)seq) + "|" + SLPosSyncMath::formatTuple(p, 3, 3) + "|"
                             + SLPosSyncMath::formatTuple(v, 3, 2) + "|" + SLPosSyncMath::formatTuple(q, 4, 4) + "|" + flag_text;
    if (!bridge.sendLatest(SLOT_POS, line))
        return;
    mSeq = seq;
    mSender.sentLine(flags, pos_global, toQuat(rot), now);
    mSentRegion = region->getHandle();
}

void SLPosSync::tickSee()
{
    SLBridge& bridge = SLBridge::instance();
    if (!bridge.isReady())
        return;
    if (!mSeeForce && mSeeCheck.getElapsedTimeF32() < SEE_CHECK)
        return;
    mSeeCheck.reset();

    // Receiving off: the helper is told we see no one (once), so it forwards nothing.
    if (!isReceiveOptionOn())
    {
        if ((mSeeForce || !mSeeSent.empty()) && bridge.sendLatest(SLOT_SEE, "pos/see/1|"))
        {
            mSeeSent.clear();
            mSeeTimer.reset();
            mSeeForce = false;
        }
        return;
    }

    // Who we can plausibly see: within shout range of us (where their helpers can be heard)
    // and draw distance of the camera, nearest to the camera first.
    static LLCachedControl<F32> far_clip(gSavedSettings, "RenderFarClip", 128.f);
    uuid_vec_t ids;
    std::vector<LLVector3d> positions;
    LLWorld::getInstance()->getAvatars(&ids, &positions, gAgent.getPositionGlobal(), SEE_RANGE);
    const LLVector3d camera = gAgent.getPosGlobalFromAgent(LLViewerCamera::getInstance()->getOrigin());
    const F64 range = llmax((F64)(F32)far_clip, 1.0);
    std::vector<std::pair<F64, LLUUID>> nearest;
    for (size_t i = 0; i < ids.size() && i < positions.size(); ++i)
    {
        if (ids[i] == gAgentID || LLMuteList::getInstance()->isMuted(ids[i]))
            continue;
        const F64 d = (positions[i] - camera).magVec();
        if (d <= range)
            nearest.emplace_back(d, ids[i]);
    }
    std::sort(nearest.begin(), nearest.end());
    if (nearest.size() > MAX_SEE)
        nearest.resize(MAX_SEE);

    std::vector<LLUUID> members;
    for (const auto& entry : nearest)
        members.push_back(entry.second);
    std::sort(members.begin(), members.end());
    const bool changed = members != mSeeSent;
    if (!mSeeForce && !(changed && mSeeTimer.getElapsedTimeF32() >= SEE_MIN_INTERVAL)
        && mSeeTimer.getElapsedTimeF32() < SEE_REFRESH)
    {
        return;
    }

    std::string line = "pos/see/1|";
    for (size_t i = 0; i < nearest.size(); ++i)
        line += (i ? "," : "") + nearest[i].second.asString();
    if (!bridge.sendLatest(SLOT_SEE, line))
        return;
    mSeeSent = members;
    mSeeTimer.reset();
    mSeeForce = false;
}

void SLPosSync::onLine(const std::string& rest)
{
    // rest = <sim-verified owner of the speaking attachment>|<seq>|<pos>|<vel>|<rot>|<flags>.
    // The owner is the only identity; the fields after it never name anyone (parseFields
    // takes exactly five).
    const std::string::size_type bar = rest.find('|');
    if (bar == std::string::npos)
        return;
    LLUUID sender;
    if (!LLUUID::parseUUID(rest.substr(0, bar), &sender) || sender.isNull() || sender == gAgentID)
        return;
    if (LLMuteList::getInstance()->isMuted(sender) || !isReceiveOptionOn())
        return;
    SLPosSyncMath::Line line;
    if (!SLPosSyncMath::parseFields(rest.substr(bar + 1), line))
    {
        ++mRejected;
        return;
    }
    LLVOAvatar* avatar = findOtherAvatar(sender);
    LLViewerRegion* region = avatar ? avatar->getRegion() : nullptr;
    if (!region)
        return;

    // Region-local in the region the sim has the avatar in (a line from a helper that has
    // just crossed lands far off and fails the bound below).
    const SLPosSyncMath::Vec3 shared = toVec(region->getPosGlobalFromRegion(
        LLVector3((F32)line.pos.x, (F32)line.pos.y, (F32)line.pos.z)));
    const SLPosSyncMath::Vec3 sim = toVec(gAgent.getPosGlobalFromAgent(avatar->getRenderPosition()));
    const F64 gap = (shared - sim).length();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!SLPosSyncMath::withinBound(shared, sim))
    {
        // Over 2 m from where the sim puts it: this update is rejected, sim placement stays.
        ++mRejected;
        avatar->mPosSyncView.release(now, 0.0);
        LL_DEBUGS("PosSync") << sender << " seq " << line.seq << " rejected: " << llformat("%.3f", gap) << " m from the sim" << LL_ENDL;
        return;
    }
    if (!mTable.accept(sender, line.seq, line.flags, now))
        return;
    LL_DEBUGS("PosSync") << sender << " seq " << line.seq << " flags " << (U32)line.flags << " gap "
                         << llformat("%.3f", gap) << " m (on arrival)" << LL_ENDL;
    // #1526: a still line is the target; a hold or moving line releases the avatar (handed
    // back with its motion, see SLPosSyncMath::View).
    avatar->mPosSyncView.takeLine(line, shared, now);
    ++mAccepted;
}

void SLPosSync::dropAvatar(const LLUUID& id)
{
    // Eased back by the avatar's own view (no snap).
    if (LLVOAvatar* avatar = findOtherAvatar(id))
        avatar->mPosSyncView.release(LLFrameTimer::getElapsedSeconds(), 0.0);
}

// static
LLVector3 SLPosSync::rootOffset(LLVOAvatar& avatar, F32 dt, LLQuaternion* rotation, bool* rot_shown)
{
    SLPosSyncMath::View& view = avatar.mPosSyncView;
    *rot_shown = false;
    if (!view.busy() || avatar.isSelf())
        return LLVector3::zero;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const SLPosSyncMath::Vec3 sim = toVec(gAgent.getPosGlobalFromAgent(avatar.getRenderPosition()));
    const LLVector3 vel = avatar.getVelocity();
    const SLPosSyncMath::Vec3 offset = view.step(sim, toQuat(avatar.getRotation()),
                                                 SLPosSyncMath::Vec3(vel.mV[VX], vel.mV[VY], vel.mV[VZ]), now, dt);
    if (view.rotShown)
    {
        rotation->set((F32)view.shownRot.x, (F32)view.shownRot.y, (F32)view.shownRot.z, (F32)view.shownRot.w);
        *rot_shown = true;
    }
    return LLVector3((F32)offset.x, (F32)offset.y, (F32)offset.z);
}
