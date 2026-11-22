/**
 * @file windows_volume_catcher.cpp
 * @brief A Windows implementation of volume level control of all audio channels opened by a process.
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

#include "volume_catcher.h"
#include "volume_catcher_util.h"
#include "llsingleton.h"
#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>

#pragma comment(lib, "ole32.lib")

// Chromium plays a page's sound through WASAPI from its audio service: a dullahan_host.exe utility
// process (--utility-sub-type=audio.mojom.AudioService) that this process (SLPlugin, CEF's browser
// process) launches. dullahan keeps the audio service inside SLPlugin only with force_wave_audio
// (it then also appends --disable-features=AudioServiceOutOfProcess). #1538 turned that off for
// smooth sound, so the sessions belong to the child, and matching GetCurrentProcessId() alone found
// none: no media volume control did anything on Windows (#1610).
//
// Each media has its own plugin process and its own audio service, so the volume of the sessions of
// THIS process's tree (itself and its descendants) is that media's volume. Matching by exe name would
// not do: every media's audio service is a dullahan_host.exe. Sessions are found on every active
// output device and caught again on pump(), so a stream that starts (or moves to a new default
// device) after the last setVolume() gets the volume too.
class VolumeCatcherImpl : public LLSingleton<VolumeCatcherImpl>
{
    LLSINGLETON(VolumeCatcherImpl);
    // This is a singleton class -- both callers and the component implementation should use getInstance() to find the instance.
    ~VolumeCatcherImpl();

public:

    void setVolume(F32 volume);
    void setPan(F32 pan);
    void pump();

private:
    bool ensureCom();
    void refreshSessions();
    void releaseSessions();
    void applyVolume();
    bool isOurProcess(DWORD pid);
    bool reloadProcessTable();

    F32     mVolume;
    F32     mPan;
    bool    mComTried = false;
    bool    mComOwned = false;       // CoInitializeEx succeeded here: uninitialise at exit
    bool    mComUsable = false;
    IMMDeviceEnumerator* mEnumerator = nullptr;
    std::vector<ISimpleAudioVolume*> mSessions;   // this process tree's sessions, one reference each
    ULONGLONG mLastRefreshMs = 0;

    // Process id -> parent id, from a Toolhelp snapshot taken only when a session's process is not
    // classified yet; and that classification per session process (pruned to the live sessions).
    std::unordered_map<DWORD, DWORD> mParents;
    std::unordered_map<DWORD, bool> mOurs;
    volume_catcher::AppliedVolumeLog mVolumeLog;
};

namespace
{
    // pump() runs on every plugin idle (tens of times a second): the session list is rebuilt this often.
    constexpr ULONGLONG kRefreshMs = 250;
    // Session processes remembered at most (a process id can be reused; the list is pruned anyway).
    constexpr size_t kMaxClassified = 256;

    template <class T> void release(T*& p)
    {
        if (p) { p->Release(); p = nullptr; }
    }

    // A process's creation time, or false when it cannot be opened.
    bool processCreated(DWORD pid, ULONGLONG* out)
    {
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process)
            return false;
        FILETIME creation, exited, kernel, user;
        const bool ok = ::GetProcessTimes(process, &creation, &exited, &kernel, &user) != 0;
        ::CloseHandle(process);
        if (ok)
            *out = ((ULONGLONG)creation.dwHighDateTime << 32) | creation.dwLowDateTime;
        return ok;
    }
}

VolumeCatcherImpl::VolumeCatcherImpl()
:   mVolume(1.0f),          // default volume is max
    mPan(0.f)               // default pan is centered
{
}

VolumeCatcherImpl::~VolumeCatcherImpl()
{
    releaseSessions();
    release(mEnumerator);
    if (mComOwned) ::CoUninitialize();
}

bool VolumeCatcherImpl::ensureCom()
{
    if (mComTried) return mComUsable;
    mComTried = true;
    // Chromium has already initialised this (its UI) thread as single-threaded; joining that is S_FALSE.
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    mComOwned = SUCCEEDED(hr);
    mComUsable = mComOwned || hr == RPC_E_CHANGED_MODE;   // (initialised in the other mode: still usable)
    HRESULT created = E_FAIL;
    if (mComUsable)
    {
        created = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof(IMMDeviceEnumerator), (void**)&mEnumerator);
        if (FAILED(created))
            mEnumerator = nullptr;
    }
    mComUsable = mComUsable && mEnumerator;
    if (mComUsable)
        volume_catcher::post("backend", "windows: WASAPI session volume for this plugin's process tree (pid "
                                            + std::to_string(::GetCurrentProcessId()) + ")",
                             volume_catcher::kLogOnce);
    else
        volume_catcher::post("com", "windows: no audio device enumerator (CoInitializeEx "
                                        + volume_catcher::hexCode((uint32_t)hr) + ", CoCreateInstance "
                                        + volume_catcher::hexCode((uint32_t)created)
                                        + "): media volume cannot be set",
                             volume_catcher::kLogFailure);
    return mComUsable;
}

bool VolumeCatcherImpl::reloadProcessTable()
{
    mParents.clear();
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        volume_catcher::post("snapshot", "windows: process snapshot failed (error "
                                             + std::to_string(::GetLastError()) + ")",
                             volume_catcher::kLogFailure);
        return false;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = ::Process32FirstW(snapshot, &entry); ok; ok = ::Process32NextW(snapshot, &entry))
        mParents[entry.th32ProcessID] = entry.th32ParentProcessID;
    ::CloseHandle(snapshot);
    return true;
}

// Is `pid` this process or one of its descendants? A parent id can outlive its process and be
// reused, so a link is trusted only when the parent was not created after the child.
bool VolumeCatcherImpl::isOurProcess(DWORD pid)
{
    const DWORD self = ::GetCurrentProcessId();
    if (pid == self)
        return true;
    if (pid == 0)   // (a session shared by several processes, or the system sounds)
        return false;
    auto known = mOurs.find(pid);
    if (known != mOurs.end())
        return known->second;

    if (!reloadProcessTable())
        return false;   // (not remembered: tried again on the next refresh)
    auto parent_of = [this](uint64_t child) -> uint64_t
    {
        auto it = mParents.find((DWORD)child);
        if (it == mParents.end())
            return 0;
        ULONGLONG child_time = 0, parent_time = 0;
        if (processCreated((DWORD)child, &child_time) && processCreated(it->second, &parent_time)
            && parent_time > child_time)
            return 0;   // (that parent id now names a younger, unrelated process)
        return it->second;
    };
    const bool ours = volume_catcher::isInProcessTree(pid, self, parent_of);
    if (mOurs.size() >= kMaxClassified)
        mOurs.clear();
    mOurs[pid] = ours;
    if (ours)
        volume_catcher::post("attach:" + std::to_string(pid),
                             "windows: audio sessions of child process " + std::to_string(pid) + " attached",
                             volume_catcher::kLogOnce);
    return ours;
}

void VolumeCatcherImpl::releaseSessions()
{
    for (ISimpleAudioVolume*& v : mSessions) release(v);
    mSessions.clear();
}

void VolumeCatcherImpl::refreshSessions()
{
    mLastRefreshMs = ::GetTickCount64();
    releaseSessions();
    if (!ensureCom()) return;

    IMMDeviceCollection* devices = nullptr;
    const HRESULT listed = mEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
    if (FAILED(listed) || !devices)
    {
        volume_catcher::post("endpoints", "windows: listing the output devices failed ("
                                              + volume_catcher::hexCode((uint32_t)listed) + ")",
                             volume_catcher::kLogFailure);
        return;
    }
    UINT device_count = 0;
    devices->GetCount(&device_count);
    std::unordered_set<DWORD> present;   // every session process seen now (prunes mOurs)
    for (UINT d = 0; d < device_count; ++d)
    {
        IMMDevice* device = nullptr;
        IAudioSessionManager2* manager = nullptr;
        IAudioSessionEnumerator* sessions = nullptr;
        if (SUCCEEDED(devices->Item(d, &device))
            && SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&manager))
            && SUCCEEDED(manager->GetSessionEnumerator(&sessions)))
        {
            int session_count = 0;
            sessions->GetCount(&session_count);
            for (int i = 0; i < session_count; ++i)
            {
                IAudioSessionControl* control = nullptr;
                IAudioSessionControl2* control2 = nullptr;
                DWORD pid = 0;
                if (SUCCEEDED(sessions->GetSession(i, &control))
                    && SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&control2))
                    && SUCCEEDED(control2->GetProcessId(&pid)))
                {
                    present.insert(pid);
                    if (isOurProcess(pid))
                    {
                        ISimpleAudioVolume* volume = nullptr;
                        if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&volume)) && volume)
                            mSessions.push_back(volume);
                    }
                }
                release(control2);
                release(control);
            }
        }
        release(sessions);
        release(manager);
        release(device);
    }
    release(devices);

    // Forget processes whose sessions are gone: their ids can be reused by unrelated processes.
    for (auto it = mOurs.begin(); it != mOurs.end();)
        it = present.count(it->first) ? std::next(it) : mOurs.erase(it);
}

void VolumeCatcherImpl::applyVolume()
{
    const float level = std::fmin(1.f, std::fmax(0.f, mVolume));
    for (ISimpleAudioVolume* v : mSessions)
    {
        float current = -1.f;
        // (only on a change: the viewer sends a new volume every frame while the camera moves near a TV)
        if (FAILED(v->GetMasterVolume(&current)) || std::fabs(current - level) > 0.001f)
        {
            const HRESULT hr = v->SetMasterVolume(level, nullptr);
            if (FAILED(hr))
                volume_catcher::post("set", "windows: SetMasterVolume failed (" + volume_catcher::hexCode((uint32_t)hr) + ")",
                                     volume_catcher::kLogFailure);
        }
    }
    mVolumeLog.applied(level, mSessions.size(), "windows");
}

void VolumeCatcherImpl::setVolume(F32 volume)
{
    mVolume = volume;
    if (mSessions.empty() && ::GetTickCount64() - mLastRefreshMs >= kRefreshMs) refreshSessions();
    applyVolume();
}

void VolumeCatcherImpl::setPan(F32 pan)
{   // remember pan for calculating individual channel levels later
    mPan = pan;
}

void VolumeCatcherImpl::pump()
{
    if (::GetTickCount64() - mLastRefreshMs < kRefreshMs) return;
    refreshSessions();
    applyVolume();
}

/////////////////////////////////////////////////////

VolumeCatcher::VolumeCatcher()
{
    pimpl = VolumeCatcherImpl::getInstance();
}

VolumeCatcher::~VolumeCatcher()
{
    // Let the instance persist until exit.
}

void VolumeCatcher::setVolume(F32 volume)
{
    pimpl->setVolume(volume);
}

void VolumeCatcher::setPan(F32 pan)
{
    pimpl->setPan(pan);
}

void VolumeCatcher::pump()
{
    pimpl->pump();
}
