/**
 * @file volume_catcher_pipewire.cpp
 * @brief A Linux-specific, PipeWire-specific hack to detect and volume-adjust new audio sources
 *
 * @cond
 * $LicenseInfo:firstyear=2010&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 * @endcond
 */

/*
  The high-level design is as follows:
  1) Connect to the PipeWire daemon
  2) Find all existing and new audio nodes
  3) Examine PID and parent PID's to see if it belongs to our process tree (Chromium plays from its
     audio service, a child process of SLPlugin, so the stream's pid is never SLPlugin's own)
  4) If so, tell PipeWire to adjust the volume of that node
  5) Keep a list of all audio nodes and adjust when we setVolume()
 */

#include "linden_common.h"

#include "volume_catcher_linux.h"

extern "C" {
#include <spa/pod/builder.h>
#include <spa/param/props.h>
}

#include <iterator>

SymbolGrabber pwSymbolGrabber;

#include "volume_catcher_pipewire_syms.inc"

////////////////////////////////////////////////////

VolumeCatcherPipeWire::VolumeCatcherPipeWire()
{
    init();
}

VolumeCatcherPipeWire::~VolumeCatcherPipeWire()
{
    cleanup();
}

static void registryEventGlobal(
    void *data, uint32_t id, uint32_t permissions, const char *type,
    uint32_t version, const struct spa_dict *props)
{
    static_cast<VolumeCatcherPipeWire*>(data)->handleRegistryEventGlobal(
        id, permissions, type, version, props
    );
}

static const struct pw_registry_events REGISTRY_EVENTS = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = registryEventGlobal,
};

bool VolumeCatcherPipeWire::loadsyms(std::string pw_dso_name)
{
    return pwSymbolGrabber.grabSymbols({ pw_dso_name });
}

void VolumeCatcherPipeWire::init()
{
    LL_DEBUGS() << "init" << LL_ENDL;

    mGotSyms = loadsyms("libpipewire-0.3.so.0");

    if (!mGotSyms)
        return;

    LL_DEBUGS() << "successfully got symbols" << LL_ENDL;

    llpw_init(nullptr, nullptr);

    mThreadLoop = llpw_thread_loop_new("SL Plugin Volume Adjuster", nullptr);

    if (!mThreadLoop)
    {
        volume_catcher::post("pw-init", "linux: PipeWire thread loop could not be created", volume_catcher::kLogFailure);
        return;
    }

    // i dont think we need to lock this early
    // std::lock_guard pwLock(*this);

    mContext = llpw_context_new(
        llpw_thread_loop_get_loop(mThreadLoop), nullptr, 0
    );

    if (!mContext)
    {
        volume_catcher::post("pw-init", "linux: PipeWire context could not be created", volume_catcher::kLogFailure);
        return;
    }

    mCore = llpw_context_connect(mContext, nullptr, 0);

    if (!mCore)
    {
        volume_catcher::post("pw-init", "linux: could not connect to the PipeWire server (errno "
                                            + std::to_string(errno) + ")",
                             volume_catcher::kLogFailure);
        return;
    }

    mRegistry = pw_core_get_registry(mCore, PW_VERSION_REGISTRY, 0);

    LL_DEBUGS() << "pw_core_get_registry: " <<  (mRegistry?"success":"nullptr") << LL_ENDL;

    if (!mRegistry)
    {
        volume_catcher::post("pw-init", "linux: PipeWire registry unavailable", volume_catcher::kLogFailure);
        return;
    }

    spa_zero(mRegistryListener);

    pw_registry_add_listener(
        mRegistry, &mRegistryListener, &REGISTRY_EVENTS, this
    );

    llpw_thread_loop_start(mThreadLoop);

    LL_DEBUGS() << "thread loop started" << LL_ENDL;
}

void VolumeCatcherPipeWire::cleanup()
{
    if (!mThreadLoop)
        return;

    // Stop (join) the loop thread FIRST: everything below belongs to that loop, and
    // tearing it down while the thread still dispatched on it crashed the plugin inside
    // libpipewire-module-protocol-native ("SL Plugin Volum" segfaults at exit; PipeWire
    // also logged "impl_ext_end_proxy called from wrong context"). With the thread gone
    // this thread is the loop's only user.
    llpw_thread_loop_stop(mThreadLoop);

    // Destroying a node proxy fires its destroy event, which removes it from the set.
    const std::unordered_set<ChildNode*> nodes(mChildNodes);
    for (ChildNode* childNode : nodes)
        llpw_proxy_destroy(childNode->mProxy);
    mChildNodes.clear();

    if (mRegistry)
    {
        spa_hook_remove(&mRegistryListener);
        llpw_proxy_destroy((struct pw_proxy*)mRegistry);
        mRegistry = nullptr;
    }
    if (mCore)
        llpw_core_disconnect(mCore);
    mCore = nullptr;
    if (mContext)
        llpw_context_destroy(mContext);
    mContext = nullptr;

    llpw_thread_loop_destroy(mThreadLoop);
    mThreadLoop = nullptr;

    LL_DEBUGS() << "cleanup done" << LL_ENDL;
}

void VolumeCatcherPipeWire::lock()
{
    if (!mThreadLoop)
        return;

    llpw_thread_loop_lock(mThreadLoop);
}

void VolumeCatcherPipeWire::unlock()
{
    if (!mThreadLoop)
        return;

    llpw_thread_loop_unlock(mThreadLoop);
}

const uint32_t channels = 1;
const float resetVolumes[channels] = { 1.0f };

void VolumeCatcherPipeWire::ChildNode::updateVolume()
{
    if (!mActive)
        return;

    F32 volume = std::clamp(mImpl->mVolume, 0.0f, 1.0f);

    const float volumes[channels] = { volume };

    uint8_t buffer[512];

    spa_pod_builder builder;
    spa_pod_builder_init(&builder, buffer, sizeof(buffer));

    spa_pod_frame frame;
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);

    // resets system-wide memorized volume for chromium (not google chrome) to 100%
    // (audioconvert spreads a one-entry array over all channels)
    spa_pod_builder_prop(&builder, SPA_PROP_channelVolumes, 0);
    spa_pod_builder_array(&builder, sizeof(float), SPA_TYPE_Float, channels, resetVolumes);

    // sets temporary volume. Never SPA_PROP_volume: WirePlumber stores and restores a stream's
    // `volume` (scripts/node/state-stream.lua), so one muted TV would mute every later CEF stream.
    // Soft volumes are not stored -- but audioconvert stops using them when anyone sets
    // channelVolumes alone (WirePlumber restoring a stream at creation), hence the re-applies
    // shortly after a stream is attached (pump).
    spa_pod_builder_prop(&builder, SPA_PROP_softVolumes, 0);
    spa_pod_builder_array(&builder, sizeof(float), SPA_TYPE_Float, channels, volumes);

    spa_pod* pod = static_cast<spa_pod*>(spa_pod_builder_pop(&builder, &frame));

    const int res = pw_node_set_param((pw_node*)mProxy, SPA_PARAM_Props, 0, pod);
    if (res < 0)
        volume_catcher::post("pw-set", "linux: PipeWire set_param on node " + std::to_string(mNodeId) + " failed ("
                                           + std::to_string(res) + ")",
                             volume_catcher::kLogFailure);
}

void VolumeCatcherPipeWire::ChildNode::onProxyDestroyed()
{
    mActive = false;
    if (mImpl->mChildNodes.erase(this))
        volume_catcher::post("detach:" + std::to_string(mNodeId),
                             "linux: PipeWire stream detached: node " + std::to_string(mNodeId),
                             volume_catcher::kLogOnce);
    spa_hook_remove(&mNodeListener);
    spa_hook_remove(&mProxyListener);
}

static void nodeEventInfo(void* data, const struct pw_node_info* info)
{
    if (info == nullptr || info->props == nullptr)
        return;

    const char* processId = spa_dict_lookup(info->props, PW_KEY_APP_PROCESS_ID);

    if (processId == nullptr)
        return;

    pid_t pid = atoll(processId);

    if (!isOurStreamPid(pid))
        return;

    const char* appName = spa_dict_lookup(info->props, PW_KEY_APP_NAME);
    if (appName == nullptr)
        appName = "?";
    LL_DEBUGS() << "got app: " << appName << LL_ENDL;

    auto* const childNode = static_cast<VolumeCatcherPipeWire::ChildNode*>(data);
    LL_DEBUGS() << "init volume: " << childNode->mImpl->mVolume  << LL_ENDL;

    // Loop thread: the thread-loop lock is held.
    childNode->updateVolume();
    if (childNode->mImpl->mChildNodes.insert(childNode).second)
    {
        childNode->mAttachedMs = volume_catcher::nowMs();
        childNode->mReapplies = 0;
        volume_catcher::post("attach:" + std::to_string(childNode->mNodeId),
                             "linux: PipeWire stream attached: node " + std::to_string(childNode->mNodeId) + ", app '"
                                 + appName + "', pid " + std::to_string(pid) + ", volume "
                                 + std::to_string(volume_catcher::volumePercent(childNode->mImpl->mVolume)) + "%",
                             volume_catcher::kLogOnce);
    }
}

static const struct pw_node_events NODE_EVENTS = {
    .version = PW_VERSION_NODE_EVENTS,
    .info = nodeEventInfo,
};

// The proxy is being destroyed (by us or by PipeWire): forget it. Calling
// pw_proxy_destroy again from here was a double destroy.
static void proxyEventDestroy(void* data)
{
    static_cast<VolumeCatcherPipeWire::ChildNode*>(data)->onProxyDestroyed();
}

// The node left the graph: destroy our proxy (its destroy event then forgets it).
static void proxyEventRemoved(void* data)
{
    auto* const childNode = static_cast<VolumeCatcherPipeWire::ChildNode*>(data);
    llpw_proxy_destroy(childNode->mProxy);
}

static const struct pw_proxy_events PROXY_EVENTS = {
    .version = PW_VERSION_PROXY_EVENTS,
    .destroy = proxyEventDestroy,
    .removed = proxyEventRemoved,
};

void VolumeCatcherPipeWire::handleRegistryEventGlobal(
    uint32_t id, uint32_t permissions, const char *type, uint32_t version,
    const struct spa_dict *props)
{
    if (props == nullptr  || type == nullptr || strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);

    if (mediaClass == nullptr || strcmp(mediaClass, "Stream/Output/Audio") != 0)
        return;

    pw_proxy* proxy = static_cast<pw_proxy*>(
        pw_registry_bind(mRegistry, id, type, PW_VERSION_NODE, sizeof(ChildNode))
    );

    if (proxy == nullptr)
    {
        volume_catcher::post("pw-bind", "linux: binding PipeWire node " + std::to_string(id) + " failed",
                             volume_catcher::kLogFailure);
        return;
    }

    auto* const childNode = static_cast<ChildNode*>(llpw_proxy_get_user_data(proxy));

    childNode->mActive = true;
    childNode->mProxy = proxy;
    childNode->mImpl = this;
    childNode->mNodeId = id;

    pw_node_add_listener((pw_node*)proxy, &childNode->mNodeListener, &NODE_EVENTS, childNode);
    llpw_proxy_add_listener(proxy, &childNode->mProxyListener, &PROXY_EVENTS, childNode);
}

void VolumeCatcherPipeWire::setVolume(F32 volume)
{
    LL_DEBUGS() << "setting volume to: " << volume << LL_ENDL;

    // The loop thread reads mVolume and owns the node set: one lock (the thread loop's)
    // for both. (A separate set mutex taken in the opposite order on the loop thread
    // could deadlock.)
    std::lock_guard pwLock(*this);
    mVolume = volume;

    LL_DEBUGS() << "found " << mChildNodes.size() << " child nodes" << LL_ENDL;

    for (auto* childNode : mChildNodes)
        childNode->updateVolume();
    mVolumeLog.applied(std::clamp(mVolume, 0.0f, 1.0f), mChildNodes.size(), "linux PipeWire");
}

void VolumeCatcherPipeWire::setPan(F32 pan)
{
}

void VolumeCatcherPipeWire::pump()
{
    std::lock_guard pwLock(*this);

    // A stream's volume again shortly after it was attached: the session manager's restore of
    // the stream's stored channel volumes at creation can land after ours and switch audioconvert
    // back from our soft volume to them (full volume). Bounded: kReapplyAtMs.size() per stream.
    static constexpr uint64_t kReapplyAtMs[] = { 300, 1500 };
    const uint64_t now = volume_catcher::nowMs();
    for (ChildNode* childNode : mChildNodes)
    {
        if (childNode->mReapplies < std::size(kReapplyAtMs)
            && now - childNode->mAttachedMs >= kReapplyAtMs[childNode->mReapplies])
        {
            ++childNode->mReapplies;
            childNode->updateVolume();
        }
    }

    // A volume change the log's rate limit held back is reported here.
    mVolumeLog.applied(std::clamp(mVolume, 0.0f, 1.0f), mChildNodes.size(), "linux PipeWire");
}
