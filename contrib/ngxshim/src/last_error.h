/*
 * last_error.h -- the text slip_nr_last_error() returns, kept per thread.
 * Pure C++ (no NGX, no Vulkan), so a host test can include it.
 *
 * The viewer calls the add-on from two threads: its main thread (DLSS, feature creation) and frame
 * generation's present thread (slip_nr_fg_evaluate). Both may fail at once; one shared string would be
 * written by both (a data race) and could hand one thread the other's text. Each thread therefore keeps
 * its own: slip_nr_last_error() returns the calling thread's last failure, and the pointer stays readable
 * (and unchanged) until that thread's next failure.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The Slipstream Viewer contributors. See contrib/ngxshim/LICENSE.
 */
#ifndef SLIPSTREAM_NGX_LAST_ERROR_H
#define SLIPSTREAM_NGX_LAST_ERROR_H

#include <string>

inline std::string& slip_last_error_slot()
{
    thread_local std::string text;
    return text;
}

// The calling thread's last failure: never NULL, "" before its first.
inline const char* slip_last_error_text() { return slip_last_error_slot().c_str(); }

inline void slip_set_last_error(const char* text) { slip_last_error_slot() = text ? text : ""; }

#endif
