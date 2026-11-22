/**
 * @file slanimheader.h
 * @brief The header of a Second Life internal animation (.anim, LLKeyframeMotion's
 *        serialized form) -- just enough to change a saved animation's ease-in / ease-out in
 *        place (#1566) without re-exporting it. No viewer types, so
 *        scripts/check/test_pose_ik.cpp tests it on its own.
 *
 * Layout (LLKeyframeMotion::serialize, little endian, version 1.0):
 *   U16 version, U16 sub_version, S32 base_priority, F32 duration,
 *   char[] emote_name (0-terminated), F32 loop_in, F32 loop_out, S32 loop,
 *   F32 ease_in, F32 ease_out, ... (hand pose, joints, constraints)
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

#ifndef SL_ANIM_HEADER_H
#define SL_ANIM_HEADER_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SLAnimHeader
{
constexpr size_t MAX_EMOTE = 256;     // an emote name longer than this is not a real header

inline float readF32(const unsigned char* p)
{
    const uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

inline void writeF32(unsigned char* p, float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    p[0] = (unsigned char)(bits & 0xff);
    p[1] = (unsigned char)((bits >> 8) & 0xff);
    p[2] = (unsigned char)((bits >> 16) & 0xff);
    p[3] = (unsigned char)((bits >> 24) & 0xff);
}

// Where ease_in sits (ease_out is 4 bytes later); false when this is not a version 1.0
// animation or the header does not fit.
inline bool easeOffset(const unsigned char* data, size_t size, size_t& offset)
{
    if (!data || size < 12)
        return false;
    const unsigned version = (unsigned)data[0] | ((unsigned)data[1] << 8);
    const unsigned sub_version = (unsigned)data[2] | ((unsigned)data[3] << 8);
    if (version != 1 || sub_version != 0)
        return false;
    size_t at = 12;                         // version, sub_version, priority, duration
    const float duration = readF32(data + 8);
    if (!std::isfinite(duration) || duration < 0.f)
        return false;
    size_t emote = 0;
    while (at + emote < size && emote <= MAX_EMOTE && data[at + emote] != 0)
        ++emote;
    if (at + emote >= size || emote > MAX_EMOTE)
        return false;
    at += emote + 1;                        // the name and its terminator
    at += 4 + 4 + 4;                        // loop_in, loop_out, loop
    if (at + 8 > size)
        return false;
    offset = at;
    return true;
}

inline bool readEase(const unsigned char* data, size_t size, float& ease_in, float& ease_out)
{
    size_t at = 0;
    if (!easeOffset(data, size, at))
        return false;
    ease_in = readF32(data + at);
    ease_out = readF32(data + at + 4);
    return true;
}

// Ease times are clamped to 0..10 s (what the uploader accepts).
inline bool writeEase(unsigned char* data, size_t size, float ease_in, float ease_out)
{
    size_t at = 0;
    if (!easeOffset(data, size, at) || !std::isfinite(ease_in) || !std::isfinite(ease_out))
        return false;
    writeF32(data + at, ease_in < 0.f ? 0.f : (ease_in > 10.f ? 10.f : ease_in));
    writeF32(data + at + 4, ease_out < 0.f ? 0.f : (ease_out > 10.f ? 10.f : ease_out));
    return true;
}
} // namespace SLAnimHeader

#endif // SL_ANIM_HEADER_H
