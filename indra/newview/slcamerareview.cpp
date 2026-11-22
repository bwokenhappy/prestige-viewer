/**
 * @file slcamerareview.cpp
 * @brief Camera mode (#1577): image review. See slcamerareview.h.
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

#include "slcamerareview.h"


#include "llfile.h"
#include "llimage.h"

#include <array>
#include <cmath>
#include <ctime>
#include <deque>
#include <vector>

namespace
{
constexpr S32 PREVIEW_LONG_SIDE = 1280;
constexpr S32 THUMB_LONG_SIDE = 160;
using SLCameraReview::HIST_BINS;

struct Shot
{
    // CPU copies only: the overlay uploads them into textures it owns (an engine texture can be
    // re-uploaded or freed by texture streaming while the overlay still draws it).
    LLPointer<LLImageRaw> preview;
    LLPointer<LLImageRaw> thumb;
    S32 width = 0, height = 0;          // the saved image
    S32 previewW = 0, previewH = 0, thumbW = 0, thumbH = 0;
    std::array<F32, HIST_BINS> histogram{};
    std::string label, path, time;
    U32 serial = 0;
};
U32 sNextSerial = 1;

struct ReviewState
{
    std::deque<Shot> shots;
};
ReviewState sReview;

void fitInside(S32 w, S32 h, S32 long_side, S32& out_w, S32& out_h)
{
    const F32 f = (F32)long_side / (F32)llmax(llmax(w, h), 1);
    out_w = llmax(1, ll_round((F32)w * llmin(f, 1.f)));
    out_h = llmax(1, ll_round((F32)h * llmin(f, 1.f)));
}

// The luma histogram of the displayed (8-bit, encoded) image, as a camera shows it;
// square-rooted counts so a sparse tail still reads.
void histogramOf(const LLImageRaw& raw, std::array<F32, HIST_BINS>& out)
{
    out.fill(0.f);
    const S32 comps = raw.getComponents();
    const U8* data = raw.getData();
    if (!data || comps < 1) return;
    const S32 n = raw.getWidth() * raw.getHeight();
    std::array<U32, HIST_BINS> counts{};
    for (S32 i = 0; i < n; ++i)
    {
        const U8* p = data + (size_t)i * (size_t)comps;
        const F32 y = comps >= 3 ? 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] : (F32)p[0];
        counts[llclamp((size_t)(y * (F32)HIST_BINS / 256.f), (size_t)0, HIST_BINS - 1)]++;
    }
    U32 peak = 1;
    for (U32 c : counts) peak = llmax(peak, c);
    for (size_t i = 0; i < HIST_BINS; ++i) out[i] = std::sqrt((F32)counts[i] / (F32)peak);
}
} // namespace

namespace SLCameraReview
{
U32 add(const LLImageRaw& image, const std::string& label)
{
    if (!image.getData() || image.getWidth() <= 0 || image.getHeight() <= 0) return 0;
    Shot shot;
    shot.serial = sNextSerial++;
    {
        const std::time_t t = std::time(nullptr);
        char buf[16] = {};
        if (const std::tm* local = std::localtime(&t))
            std::strftime(buf, sizeof(buf), "%H:%M", local);
        shot.time = buf;
    }
    shot.width = image.getWidth();
    shot.height = image.getHeight();
    shot.label = label;

    // Straight to the review sizes: never a second full-size copy of the shot.
    fitInside(shot.width, shot.height, PREVIEW_LONG_SIDE, shot.previewW, shot.previewH);
    fitInside(shot.previewW, shot.previewH, THUMB_LONG_SIDE, shot.thumbW, shot.thumbH);
    LLPointer<LLImageRaw> preview = new LLImageRaw((U16)shot.previewW, (U16)shot.previewH, image.getComponents());
    LLPointer<LLImageRaw> thumb = new LLImageRaw((U16)shot.thumbW, (U16)shot.thumbH, image.getComponents());
    if (!preview->getData() || !thumb->getData())
    {
        LL_WARNS("CameraMode") << "review: no memory for the preview of a " << shot.width << "x" << shot.height
                               << " shot; it is saved but not in the review" << LL_ENDL;
        return 0;
    }
    preview->copyScaled(&image);
    thumb->copyScaled(preview.get());
    histogramOf(*preview, shot.histogram);

    shot.preview = preview;
    shot.thumb = thumb;

    const U32 serial = shot.serial;
    sReview.shots.push_back(std::move(shot));
    while (sReview.shots.size() > MAX_SHOTS) sReview.shots.pop_front();   // bounded: the oldest go
    return serial;
}

void setPath(U32 serial, const std::string& path)
{
    for (Shot& shot : sReview.shots)
        if (shot.serial == serial)
            shot.path = path;
}

size_t count()
{
    return sReview.shots.size();
}

U32 serial(size_t i)
{
    return i < sReview.shots.size() ? sReview.shots[i].serial : 0;
}

const LLImageRaw* thumbnail(size_t i)
{
    return i < sReview.shots.size() ? sReview.shots[i].thumb.get() : nullptr;
}

const LLImageRaw* preview(size_t i)
{
    return i < sReview.shots.size() ? sReview.shots[i].preview.get() : nullptr;
}

void imageSize(size_t i, S32& w, S32& h)
{
    w = i < sReview.shots.size() ? sReview.shots[i].width : 0;
    h = i < sReview.shots.size() ? sReview.shots[i].height : 0;
}

namespace
{
const std::string& field(size_t i, std::string Shot::*member)
{
    static const std::string none;
    return i < sReview.shots.size() ? sReview.shots[i].*member : none;
}
} // namespace

const std::string& label(size_t i) { return field(i, &Shot::label); }
const std::string& path(size_t i) { return field(i, &Shot::path); }
const std::string& timeTaken(size_t i) { return field(i, &Shot::time); }

const F32* histogram(size_t i)
{
    return i < sReview.shots.size() ? sReview.shots[i].histogram.data() : nullptr;
}

bool deleteFile(size_t i)
{
    const std::string& p = path(i);
    return !p.empty() && LLFile::isfile(p) && LLFile::remove(p) == 0;
}

void remove(size_t i)
{
    if (i >= sReview.shots.size()) return;
    sReview.shots.erase(sReview.shots.begin() + (std::ptrdiff_t)i);
}

void clear()
{
    sReview.shots.clear();
}

bool empty()
{
    return sReview.shots.empty();
}

} // namespace SLCameraReview
