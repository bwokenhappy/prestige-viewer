/**
 * @file llvrbodymapping.cpp
 * @brief #1592 VR body mapping. See llvrbodymapping.h; the pure part is llvrbodymapmath.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrbodymapping.h"

#include "llcharacter.h"
#include "llfile.h"
#include "llformat.h"
#include "lljoint.h"
#include "lljointstate.h"
#include "llskinningutil.h"
#include "llviewercontrol.h"
#include "llvoavatarself.h"
#include "llvolume.h"
#include "llvovolume.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace sb = slbmap;

namespace
{
constexpr F32 DETECT_INTERVAL = 5.f;   // seconds between Auto's rigging checks
constexpr size_t MAX_FILE = 64 * 1024; // an import larger than this is not a body map

struct MapState
{
    bool loaded = false;
    sb::Profile profile = sb::humanProfile();
    int detected = sb::D_HUMAN;
    std::string why = "not checked yet";
    F32 detectIn = 0.f;
    U32 gen = 1, jointGen = 1;
    // Joint lookups per character (made anew when the joint names change).
    LLCharacter* cachedFor = nullptr;
    U32 cachedJointGen = 0;
    LLJoint* joint[sb::R_COUNT] = {};
};
MapState sM;

LLSD::String settingString(const char* name, const char* fallback)
{
    LLControlVariable* c = gSavedPerAccountSettings.getControl(name);
    return c ? c->getValue().asString() : std::string(fallback);
}
void setSettingString(const char* name, const std::string& v)
{
    if (gSavedPerAccountSettings.getControl(name)) gSavedPerAccountSettings.setString(name, v);
}

// Joints the worn rigged meshes weight (by joint number), from each face's rigging table (as
// SLPoseLookAt's detection: the tables outlive the vertex data, the avatar's cached table may lag).
S32 collectRigged(LLVOAvatar* avatar, std::vector<bool>& rigged)
{
    rigged.assign(LL_CHARACTER_MAX_ANIMATED_JOINTS, false);
    if (!avatar) return 0;
    std::vector<LLVOVolume*> volumes;
    avatar->getAssociatedVolumes(volumes);
    for (LLVOVolume* vol : volumes)
    {
        if (!vol || vol->isDead() || vol->isAnimatedObject() || !vol->isRiggedMesh()) continue;
        LLVolume* volume = vol->getVolume();
        if (!volume) continue;
        const LLMeshSkinInfo* skin = vol->getSkinInfo();
        for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
        {
            LLVolumeFace& face = volume->getVolumeFace(f);
            if (skin && face.mJointRiggingInfoTab.needsUpdate()) LLSkinningUtil::updateRiggingInfo(skin, avatar, face);
            const LLJointRiggingInfoTab& tab = face.mJointRiggingInfoTab;
            const S32 n = llmin(tab.size(), (S32)rigged.size());
            for (S32 j = 0; j < n; ++j)
                if (tab[j].isRiggedTo()) rigged[j] = true;
        }
    }
    S32 count = 0;
    for (bool r : rigged) count += r ? 1 : 0;
    return count;
}

// A child's offset in its parent's frame (scaled as LLXformMatrix::update does).
sb::V3 offsetIn(LLJoint* child)
{
    LLVector3 p = child->getPosition();
    LLJoint* parent = child->getParent();
    if (parent && parent->getXform()->getScaleChildOffset()) p.scaleVec(parent->getScale());
    return sb::V3(p.mV[VX], p.mV[VY], p.mV[VZ]);
}
bool chain(LLJoint* ancestor, LLJoint* desc, sb::V3& out)
{
    out = sb::V3();
    for (LLJoint* j = desc; j; j = j->getParent())
    {
        if (j == ancestor) return true;
        out = out + offsetIn(j);
    }
    return false;
}

void apply(const sb::Profile& in, const char* why)
{
    sb::Profile p = in;
    sb::settle(p);   // (a pony with its spine tracked stands up; standing drives the hips and legs)
    const bool same = sb::sameMapping(sM.profile, p) && sM.profile.id == p.id && sM.profile.custom == p.custom;
    if (!sb::sameJoints(sM.profile, p)) ++sM.jointGen;
    sM.profile = p;
    if (same) return;
    ++sM.gen;
    LL_INFOS("VRBody") << "body mapping: " << (p.custom ? "custom (from " + p.id + ")" : p.id) << " (" << why << ")" << LL_ENDL;
}

// The profile the choice names (Auto: the detected built-in; custom: the saved text).
void resolve(const char* why)
{
    const std::string c = settingString("SlipstreamVRBodyMapChoice", "auto");
    sb::Profile p;
    if (c == "custom")
    {
        int skipped = 0;
        if (!sb::fromText(settingString("SlipstreamVRBodyMapCustom", ""), p, &skipped))
        {
            LL_WARNS("VRBody") << "body mapping: the saved custom profile is unreadable; using Human" << LL_ENDL;
            p = sb::humanProfile();
        }
    }
    else if (c == "auto" || !sb::builtinProfile(c, p))
    {
        sb::builtinProfile(sb::autoProfileFor(sM.detected), p);
    }
    apply(p, why);
}

void detectNow()
{
    sb::RigFacts f;
    LLVOAvatarSelf* av = isAgentAvatarValid() ? gAgentAvatarp.get() : nullptr;
    if (av)
    {
        av->updateRiggingInfo();
        std::vector<bool> rigged;
        f.riggedCount = collectRigged(av, rigged);
        auto isRigged = [&](const std::string& name) {
            LLJoint* j = av->getJoint(name);
            if (!j) return false;
            const S32 n = j->getJointNum();
            return (n >= 0 && n < (S32)rigged.size() && rigged[n]) || av->jointIsRiggedTo(j);
        };
        for (int side = 0; side < 2; ++side)
        {
            const std::string s = side ? "Right" : "Left";
            f.ring1[side] = isRigged("mHandRing1" + s);
            f.foot[side] = isRigged("mFoot" + s);
            for (const char* finger : {"Index", "Middle", "Pinky", "Thumb", "Ring"})
                for (int k = 1; k <= 3; ++k)
                {
                    if (!std::strcmp(finger, "Ring") && k == 1) continue;
                    if (isRigged(llformat("mHand%s%d", finger, k) + s)) f.otherFingers[side] = true;
                }
            for (int k = 1; k <= 4; ++k)
                if (isRigged(llformat("mHindLimb%d", k) + s)) f.hindLimbs = true;
        }
        LLJoint* pelvis = av->getJoint("mPelvis");
        LLJoint* neck = av->getJoint("mNeck");
        if (pelvis && neck) chain(pelvis, neck, f.pelvisToNeck);
    }
    std::string why;
    const int d = sb::detect(f, &why);
    if (d != sM.detected || why != sM.why)
    {
        LL_INFOS("VRBody") << "body mapping detection: " << why << llformat(" (%d rigged bones)", f.riggedCount) << LL_ENDL;
    }
    sM.detected = d;
    sM.why = why;
}

void load()
{
    sM.loaded = true;
    detectNow();
    resolve("loaded for this avatar");
}
} // namespace

const sb::Profile& LLVRBodyMap::profile() { return sM.profile; }
U32 LLVRBodyMap::generation() { return sM.gen; }
U32 LLVRBodyMap::jointGeneration() { return sM.jointGen; }
std::string LLVRBodyMap::choice() { return settingString("SlipstreamVRBodyMapChoice", "auto"); }
int LLVRBodyMap::detected() { return sM.detected; }
std::string LLVRBodyMap::detectReason() { return sM.why; }

void LLVRBodyMap::choose(const std::string& c)
{
    sb::Profile p;
    if (c != "auto" && c != "custom" && !sb::builtinProfile(c, p)) return;
    setSettingString("SlipstreamVRBodyMapChoice", c);
    if (!sM.loaded)
    {
        sM.loaded = true;
        detectNow();
    }
    resolve("chosen");
}

void LLVRBodyMap::edit(const sb::Profile& in)
{
    sb::Profile p = in;
    p.custom = true;
    sb::settle(p);
    setSettingString("SlipstreamVRBodyMapCustom", sb::toText(p));
    setSettingString("SlipstreamVRBodyMapChoice", "custom");
    sM.loaded = true;
    apply(p, "edited");
}

void LLVRBodyMap::redetect()
{
    // (Also the first load when the window opens outside VR.)
    sM.loaded = true;
    detectNow();
    sM.detectIn = DETECT_INTERVAL;
    resolve("detected");
}

void LLVRBodyMap::update(F32 dt)
{
    if (!sM.loaded)
    {
        load();
        sM.detectIn = DETECT_INTERVAL;
        return;
    }
    sM.detectIn -= dt;
    if (sM.detectIn > 0.f) return;
    sM.detectIn = DETECT_INTERVAL;
    detectNow();
    // (The choice may also have changed under us: another window, a settings reload.)
    resolve("detected");
}

bool LLVRBodyMap::exportFile(const std::string& path, std::string& err)
{
    llofstream f(path.c_str());
    if (!f.is_open())
    {
        err = "cannot write " + path;
        return false;
    }
    f << "# Slipstream VR body mapping (#1592): role <role> <joint|-> <tracked|animation|off>\n" << sb::toText(sM.profile);
    f.close();
    LL_INFOS("VRBody") << "body mapping exported (" << sM.profile.id << ")" << LL_ENDL;
    return true;
}

bool LLVRBodyMap::importFile(const std::string& path, std::string& err)
{
    llifstream f(path.c_str());
    if (!f.is_open())
    {
        err = "cannot read " + path;
        return false;
    }
    std::string text;
    char buf[4096];
    while (f.good() && text.size() <= MAX_FILE)
    {
        f.read(buf, sizeof(buf));
        text.append(buf, (size_t)f.gcount());
    }
    if (text.size() > MAX_FILE)
    {
        err = "not a body mapping file (too large)";
        return false;
    }
    sb::Profile p;
    int skipped = 0;
    if (!sb::fromText(text, p, &skipped))
    {
        err = "not a body mapping file";
        return false;
    }
    edit(p);
    LL_INFOS("VRBody") << "body mapping imported (from " << p.id << ")" << (skipped ? llformat(", %d lines skipped", skipped) : std::string())
                       << LL_ENDL;
    if (skipped) err = llformat("%d lines were not understood and skipped", skipped);
    return true;
}

// ---- For the body layer ------------------------------------------------------------------------
LLJoint* LLVRBodyMap::jointFor(LLCharacter* ch, int role)
{
    if (!ch || role < 0 || role >= sb::R_COUNT || sb::isFingers(role)) return nullptr;
    if (sM.cachedFor != ch || sM.cachedJointGen != sM.jointGen)
    {
        sM.cachedFor = ch;
        sM.cachedJointGen = sM.jointGen;
        for (int r = 0; r < sb::R_COUNT; ++r)
            sM.joint[r] = sM.profile.enabled(r) && !sb::isFingers(r) ? ch->getJoint(sM.profile.joint[r]) : nullptr;
    }
    return sM.joint[role];
}

void LLVRBodyMap::frames(LLCharacter* ch, sb::Frames& out, sb::RestRig* rigOut, bool upright)
{
    sb::RestRig rig;
    for (int r = 0; r < sb::R_COUNT; ++r)
    {
        LLJoint* j = jointFor(ch, r);
        if (!j) continue;
        // From the nearest mapped chain ancestor (the root for the pelvis).
        int pr = sb::roleInfo(r).parent;
        while (pr >= 0 && !jointFor(ch, pr)) pr = sb::roleInfo(pr).parent;
        sb::V3 off;
        if (pr >= 0) rig.have[r] = chain(jointFor(ch, pr), j, off);
        else rig.have[r] = r == sb::R_PELVIS;
        rig.off[r] = off;
    }
    if (upright || !sM.profile.upright)
    {
        sb::computeFrames(sM.profile, rig, out);
    }
    else
    {
        sb::Profile p = sM.profile;
        p.upright = false;
        sb::computeFrames(p, rig, out);
    }
    if (rigOut) *rigOut = rig;
}

int LLVRBodyMap::roleOf(LLJoint* j, bool* own)
{
    if (own) *own = false;
    bool first = true;
    for (LLJoint* a = j; a; a = a->getParent(), first = false)
    {
        const std::string& name = a->getName();
        for (int r = 0; r < sb::R_COUNT; ++r)
        {
            if (sb::isFingers(r) || sM.profile.joint[r] != name) continue;
            if (own) *own = first;
            // A Bento finger bone under a hand (or under its hoof) belongs to that side's fingers.
            const bool hand = r == sb::R_L_HAND || r == sb::R_R_HAND || sb::isHoof(r);
            if (!first && hand && j->getName().compare(0, 5, "mHand") == 0)
            {
                if (own) *own = false;
                return (r == sb::R_L_HAND || r == sb::R_L_HOOF) ? sb::R_L_FINGERS : sb::R_R_FINGERS;
            }
            return r;
        }
    }
    return -1;
}

namespace
{
// A joint's rest offset in its parent's frame: the mesh's position override, else its position
// (an animation sliding a pupil does not move the eye point), scaled as offsetIn.
sb::V3 restOffsetIn(LLJoint* child, bool* overridden = nullptr)
{
    LLVector3 p;
    LLUUID mesh;
    const bool over = child->hasAttachmentPosOverride(p, mesh);
    if (!over) p = child->getPosition();
    if (overridden) *overridden = over;
    LLJoint* parent = child->getParent();
    if (parent && parent->getXform()->getScaleChildOffset()) p.scaleVec(parent->getScale());
    return sb::V3(p.mV[VX], p.mV[VY], p.mV[VZ]);
}
// From `head` down to `desc` at rest; overridden: desc's own position comes from a mesh.
bool restChain(LLJoint* head, LLJoint* desc, sb::V3& out, bool* overridden = nullptr)
{
    out = sb::V3();
    if (!head || !desc) return false;
    for (LLJoint* j = desc; j; j = j->getParent())
    {
        if (j == head) return true;
        out = out + restOffsetIn(j, j == desc ? overridden : nullptr);
    }
    return false;
}
struct EyeCache
{
    LLCharacter* ch = nullptr;
    LLJoint* head = nullptr;
    U32 gen = 0;
    std::chrono::steady_clock::time_point at{};
    LLVector3 eye;
    int source = -1;              // the last one logged
};
EyeCache sEye;
} // namespace

LLVector3 LLVRBodyMap::eyeFromHead(LLCharacter* ch, LLJoint* head)
{
    constexpr F32 REFRESH = 2.f;   // seconds (outfit changes move the joints)
    const auto now = std::chrono::steady_clock::now();
    if (ch && head && sEye.ch == ch && sEye.head == head && sEye.gen == sM.gen &&
        std::chrono::duration<F32>(now - sEye.at).count() < REFRESH)
        return sEye.eye;
    sb::EyeJoints e;
    if (ch && head)
    {
        sb::V3 a, b;
        bool oa = false, ob = false;
        e.pupils = restChain(head, ch->getJoint("mFaceNoseLeft"), a, &oa) && restChain(head, ch->getJoint("mFaceNoseRight"), b, &ob) &&
                   oa && ob;
        if (e.pupils) e.pupilMid = (a + b) * 0.5f;
        e.eyes = restChain(head, ch->getJoint("mEyeLeft"), a) && restChain(head, ch->getJoint("mEyeRight"), b);
        if (e.eyes) e.eyeMid = (a + b) * 0.5f;
    }
    int source = sb::EYE_DEFAULT;
    const sb::V3 v = sb::eyeFromHead(sb::isPonyProfile(sM.profile), e, &source);
    sEye.ch = ch;
    sEye.head = head;
    sEye.gen = sM.gen;
    sEye.at = now;
    sEye.eye.set(v.x, v.y, v.z);
    if (ch && head && source != sEye.source)
    {
        sEye.source = source;
        static const char* const kName[] = {"the pupils (mFaceNoseLeft / Right)", "the eye joints (mEyeLeft / Right)",
                                            "the SL skeleton's eyes (no plausible eye joints)"};
        LL_INFOS("VRBody") << "eye point: " << kName[source] << llformat(", %.3f forward and %.3f up from the head joint", v.x, v.z)
                           << LL_ENDL;
    }
    return sEye.eye;
}

LLQuaternion LLVRBodyMap::frameOf(const sb::Frames& f, LLJoint* j)
{
    const int r = roleOf(j);
    if (r < 0) return LLQuaternion::DEFAULT;
    const sb::Q& q = f.R[r];
    return LLQuaternion(q.x, q.y, q.z, q.w);
}

void LLVRBodyMap::finish(LLJointState* st, int role, bool own, const sb::Frames& f, F32 base)
{
    if (!st || role < 0 || role >= sb::R_COUNT) return;
    const sb::Profile& p = sM.profile;
    const F32 pw = llclamp(p.weight[sb::roleInfo(role).part], 0.f, 1.f);
    const int state = p.enabled(role) || sb::isFingers(role) ? p.state[role] : sb::S_ANIMATION;
    if (state == sb::S_ANIMATION || base <= 0.f)
    {
        st->setWeight(0.f);
        return;
    }
    LLQuaternion baseQ = LLQuaternion::DEFAULT;
    if (own)
    {
        const sb::Q q = sb::baseLocal(f, role);
        baseQ = LLQuaternion(q.x, q.y, q.z, q.w);
    }
    if (state == sb::S_OFF)
    {
        st->setRotation(baseQ);
        st->setWeight(base * pw);
        return;
    }
    const F32 w = st->getWeight();
    if (f.upright)
    {
        const F32 a = llclamp(w / base, 0.f, 1.f);
        st->setRotation(a > 0.f ? slerp(a, baseQ, st->getRotation()) : baseQ);
        st->setWeight(base * pw);
        return;
    }
    st->setWeight(w * pw);
}
