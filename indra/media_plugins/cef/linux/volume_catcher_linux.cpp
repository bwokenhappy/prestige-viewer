/**
 * @file volume_catcher.cpp
 * @brief Linux volume catcher which will pick an implementation to use
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

#include "volume_catcher_linux.h"

#include <cstdlib>
#include <unistd.h>

bool isOurStreamPid(pid_t pid)
{
    if (pid <= 0)
        return false;
    return volume_catcher::isInProcessTree((uint64_t)pid, (uint64_t)getpid(),
                                           [](uint64_t child) -> uint64_t
                                           {
                                               const pid_t parent = getParentPid((pid_t)child);
                                               return parent > 0 ? (uint64_t)parent : 0;
                                           });
}

VolumeCatcher::VolumeCatcher()
{
}

void VolumeCatcher::onEnablePipeWireVolumeCatcher(bool enable)
{
    if (pimpl != nullptr)
        return;

    if (enable)
    {
        LL_DEBUGS() << "volume catcher using pipewire" << LL_ENDL;
        pimpl = new VolumeCatcherPipeWire();
    }
    else
    {
        LL_DEBUGS() << "volume catcher using pulseaudio" << LL_ENDL;
        pimpl = new VolumeCatcherPulseAudio();
    }
}

VolumeCatcher::~VolumeCatcher()
{
    if (pimpl != nullptr)
    {
        delete pimpl;
        pimpl = nullptr;
    }
}

// The viewer may never send enable_pipewire_volume_catcher; without a backend every
// volume change (including the master mute) was silently dropped. Prefer PipeWire
// when its library loads and a PipeWire server socket exists; otherwise PulseAudio,
// which also covers pipewire-pulse setups and the Flatpak sandbox (pulseaudio socket).
void VolumeCatcher::ensureImpl()
{
    if (pimpl != nullptr)
        return;

    bool use_pipewire = false;
    if (!getenv("FLATPAK_ID"))
    {
        std::string dir;
        if (const char* rt = getenv("PIPEWIRE_RUNTIME_DIR"))
            dir = rt;
        else if (const char* xdg = getenv("XDG_RUNTIME_DIR"))
            dir = xdg;
        const char* remote = getenv("PIPEWIRE_REMOTE");
        std::string sock = dir + "/" + (remote ? remote : "pipewire-0");
        use_pipewire = !dir.empty() && access(sock.c_str(), F_OK) == 0;
    }

    const std::string self = std::to_string(getpid());
    std::string why = getenv("FLATPAK_ID") ? "Flatpak" : "no PipeWire socket";
    if (use_pipewire)
    {
        VolumeCatcherPipeWire* pw = new VolumeCatcherPipeWire();
        // Symbols alone are not enough: with no connection (socket present but refusing) every
        // volume change would be dropped. PulseAudio (pipewire-pulse) may still serve.
        if (pw->mGotSyms && pw->mCore && pw->mRegistry)
        {
            LL_INFOS() << "volume catcher using pipewire" << LL_ENDL;
            volume_catcher::post("backend", "linux: PipeWire (streams of this plugin's process tree, pid " + self + ")",
                                 volume_catcher::kLogOnce);
            pimpl = pw;
            return;
        }
        why = pw->mGotSyms ? "PipeWire connection failed" : "libpipewire-0.3 not loadable";
        delete pw;
    }
    LL_INFOS() << "volume catcher using pulseaudio" << LL_ENDL;
    VolumeCatcherPulseAudio* pa = new VolumeCatcherPulseAudio();
    if (pa->mGotSyms)
        volume_catcher::post("backend", "linux: PulseAudio (" + why + "; streams of this plugin's process tree, pid "
                                            + self + ")",
                             volume_catcher::kLogOnce);
    else
        volume_catcher::post("backend", "linux: no volume backend (" + why
                                            + "; libpulse not loadable): media volume cannot be set",
                             volume_catcher::kLogOnce);
    pimpl = pa;
}

void VolumeCatcher::setVolume(F32 volume)
{
    ensureImpl();
    if (pimpl != nullptr) {
        pimpl->setVolume(volume);
    }
}

void VolumeCatcher::setPan(F32 pan)
{
    ensureImpl();
    if (pimpl != nullptr)
        pimpl->setPan(pan);
}

void VolumeCatcher::pump()
{
    if (pimpl != nullptr)
        pimpl->pump();
}
