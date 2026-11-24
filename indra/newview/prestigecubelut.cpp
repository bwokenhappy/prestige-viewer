/**
 * @file prestigecubelut.cpp
 * @brief Camera filters: .cube colour lookup tables. See prestigecubelut.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#if !defined(PRESTIGE_CUBELUT_SELFTEST)   // the local test builds this file without the viewer
#include "llviewerprecompiledheaders.h"
#endif

#include "prestigecubelut.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>

namespace PrestigeCubeLut
{
namespace
{
bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// One number at `p` (a C-locale decimal: sign, digits, '.', exponent). Locale-free on purpose:
// strtof reads "0,5" in a comma locale and stops at "0.5". Moves `p` past it.
bool parseNumber(const char*& p, const char* end, float& out)
{
    const char* s = p;
    bool neg = false;
    if (s < end && (*s == '+' || *s == '-'))
    {
        neg = *s == '-';
        ++s;
    }
    double v = 0.0;
    int digits = 0;
    while (s < end && *s >= '0' && *s <= '9')
    {
        v = v * 10.0 + (*s - '0');
        ++s;
        ++digits;
    }
    if (s < end && *s == '.')
    {
        ++s;
        double scale = 0.1;
        while (s < end && *s >= '0' && *s <= '9')
        {
            v += (*s - '0') * scale;
            scale *= 0.1;
            ++s;
            ++digits;
        }
    }
    if (!digits) return false;
    if (s < end && (*s == 'e' || *s == 'E'))
    {
        const char* e = s + 1;
        bool eneg = false;
        if (e < end && (*e == '+' || *e == '-'))
        {
            eneg = *e == '-';
            ++e;
        }
        int ex = 0, edigits = 0;
        while (e < end && *e >= '0' && *e <= '9')
        {
            if (ex < 400) ex = ex * 10 + (*e - '0');
            ++e;
            ++edigits;
        }
        if (!edigits) return false;
        v *= std::pow(10.0, eneg ? -ex : ex);
        s = e;
    }
    if (s < end && !isSpace(*s)) return false;   // "0.5x", "1,0"
    if (!std::isfinite(v) || std::fabs(v) > 1.0e6) return false;
    out = (float)(neg ? -v : v);
    p = s;
    return true;
}

void skipSpace(const char*& p, const char* end)
{
    while (p < end && isSpace(*p)) ++p;
}

// Reads exactly `n` numbers from the rest of a line.
bool numbers(const char* p, const char* end, float* out, int n)
{
    for (int i = 0; i < n; ++i)
    {
        skipSpace(p, end);
        if (!parseNumber(p, end, out[i])) return false;
    }
    skipSpace(p, end);
    return p == end;
}

bool integer(const char* p, const char* end, long& out)
{
    skipSpace(p, end);
    long v = 0;
    int digits = 0;
    while (p < end && *p >= '0' && *p <= '9')
    {
        if (v < 100000000) v = v * 10 + (*p - '0');
        ++p;
        ++digits;
    }
    skipSpace(p, end);
    if (!digits || p != end) return false;
    out = v;
    return true;
}

std::string lineError(size_t line, const std::string& what)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "line %u: ", (unsigned)line);
    return buf + what;
}

float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// Position of `x` on a table of `n` entries over [lo, hi]: index and fraction.
void locate(float x, float lo, float hi, int n, int& i0, float& f)
{
    float u = hi > lo ? (x - lo) / (hi - lo) : 0.f;
    u = std::min(std::max(u, 0.f), 1.f);
    const float pos = u * (float)(n - 1);
    i0 = std::min((int)pos, n - 2);
    f = pos - (float)i0;
}
} // namespace

bool parse(const std::string& text, Lut& out, std::string& error)
{
    out = Lut();
    error.clear();
    if (text.size() > kMaxFileBytes)
    {
        error = "the file is too large for a LUT (more than 64 MB)";
        return false;
    }
    const char* p = text.data();
    const char* end = p + text.size();
    if (end - p >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;   // UTF-8 BOM
    bool domain1Set = false, domain3Set = false, sawDomainMin = false, sawDomainMax = false;
    float dmin[3] = {0.f, 0.f, 0.f}, dmax[3] = {1.f, 1.f, 1.f};
    size_t expect1 = 0, expect3 = 0, got = 0;
    size_t lineNo = 0;
    bool dataStarted = false;
    while (p < end)
    {
        const char* ls = p;
        while (p < end && *p != '\n') ++p;
        const char* le = p;
        if (p < end) ++p;
        ++lineNo;
        // Comments: '#' to the end of the line (TITLE's quoted text may hold one).
        {
            bool quoted = false;
            for (const char* c = ls; c < le; ++c)
            {
                if (*c == '"') quoted = !quoted;
                else if (*c == '#' && !quoted) { le = c; break; }
            }
        }
        skipSpace(ls, le);
        while (le > ls && isSpace(le[-1])) --le;
        if (ls == le) continue;
        const char c0 = *ls;
        if ((c0 >= '0' && c0 <= '9') || c0 == '-' || c0 == '+' || c0 == '.')
        {
            // A table entry.
            if (!expect1 && !expect3)
            {
                error = lineError(lineNo, "a table entry before LUT_3D_SIZE or LUT_1D_SIZE");
                return false;
            }
            if (!dataStarted)
            {
                dataStarted = true;
                try
                {
                    out.table1D.reserve(expect1 * 3);
                    out.table3D.reserve(expect3 * 3);
                }
                catch (const std::bad_alloc&)
                {
                    error = "not enough memory for this LUT";
                    return false;
                }
            }
            float rgb[3];
            if (!numbers(ls, le, rgb, 3))
            {
                error = lineError(lineNo, "expected three numbers (red green blue)");
                return false;
            }
            if (got >= expect1 + expect3)
            {
                error = lineError(lineNo, "more table entries than LUT_" + std::string(expect3 ? "3D" : "1D") + "_SIZE allows");
                return false;
            }
            std::vector<float>& t = got < expect1 ? out.table1D : out.table3D;
            t.insert(t.end(), rgb, rgb + 3);
            ++got;
            continue;
        }
        // A keyword.
        const char* ke = ls;
        while (ke < le && !isSpace(*ke)) ++ke;
        const std::string key(ls, ke);
        if (dataStarted)
        {
            error = lineError(lineNo, "'" + key + "' after the table entries started");
            return false;
        }
        if (key == "TITLE")
        {
            const char* q1 = ke;
            while (q1 < le && *q1 != '"') ++q1;
            const char* q2 = q1 < le ? q1 + 1 : le;
            while (q2 < le && *q2 != '"') ++q2;
            out.title = q1 < le ? std::string(q1 + 1, q2) : std::string(ke, le);
            const size_t a = out.title.find_first_not_of(" \t");
            out.title = a == std::string::npos ? std::string() : out.title.substr(a);
        }
        else if (key == "LUT_3D_SIZE" || key == "LUT_1D_SIZE")
        {
            const bool three = key == "LUT_3D_SIZE";
            long n = 0;
            if (!integer(ke, le, n))
            {
                error = lineError(lineNo, key + " needs a whole number");
                return false;
            }
            if ((three ? out.size3D : out.size1D) != 0)
            {
                error = lineError(lineNo, key + " appears twice");
                return false;
            }
            if (three && (n < 2 || n > kMax3D))
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "LUT_3D_SIZE %ld is not supported (2 to %d)", n, kMax3D);
                error = lineError(lineNo, buf);
                return false;
            }
            if (!three && (n < 2 || n > kMax1D))
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "LUT_1D_SIZE %ld is not supported (2 to %d)", n, kMax1D);
                error = lineError(lineNo, buf);
                return false;
            }
            if (three)
            {
                out.size3D = (int)n;
                expect3 = (size_t)n * (size_t)n * (size_t)n;
            }
            else
            {
                out.size1D = (int)n;
                expect1 = (size_t)n;
            }
        }
        else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX")
        {
            float v[3];
            if (!numbers(ke, le, v, 3))
            {
                error = lineError(lineNo, key + " needs three numbers");
                return false;
            }
            std::copy(v, v + 3, key == "DOMAIN_MIN" ? dmin : dmax);
            (key == "DOMAIN_MIN" ? sawDomainMin : sawDomainMax) = true;
        }
        else if (key == "LUT_1D_INPUT_RANGE" || key == "LUT_3D_INPUT_RANGE")
        {
            float v[2];
            if (!numbers(ke, le, v, 2))
            {
                error = lineError(lineNo, key + " needs two numbers");
                return false;
            }
            float* lo = key == "LUT_1D_INPUT_RANGE" ? out.min1D : out.min3D;
            float* hi = key == "LUT_1D_INPUT_RANGE" ? out.max1D : out.max3D;
            for (int i = 0; i < 3; ++i)
            {
                lo[i] = v[0];
                hi[i] = v[1];
            }
            (key == "LUT_1D_INPUT_RANGE" ? domain1Set : domain3Set) = true;
        }
        else
        {
            // LUT_IN_VIDEO_RANGE, LUT_OUT_VIDEO_RANGE and other tool-specific keywords: skipped.
            bool word = true;
            for (char ch : key) word = word && ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_');
            if (!word)
            {
                error = lineError(lineNo, "this is not a .cube LUT line");
                return false;
            }
        }
    }
    if (!out.size1D && !out.size3D)
    {
        error = "this is not a .cube LUT: it has no LUT_3D_SIZE or LUT_1D_SIZE";
        return false;
    }
    if (got != expect1 + expect3)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "the table has %u entries; LUT size %s needs %u", (unsigned)got,
                      out.size3D ? (std::to_string(out.size3D) + " (3D)").c_str() : (std::to_string(out.size1D) + " (1D)").c_str(),
                      (unsigned)(expect1 + expect3));
        error = buf;
        return false;
    }
    // DOMAIN_MIN / MAX: the domain of the table(s) that no *_INPUT_RANGE named.
    if (sawDomainMin || sawDomainMax)
        for (int i = 0; i < 3; ++i)
        {
            if (!domain1Set) { out.min1D[i] = dmin[i]; out.max1D[i] = dmax[i]; }
            if (!domain3Set) { out.min3D[i] = dmin[i]; out.max3D[i] = dmax[i]; }
        }
    for (int i = 0; i < 3; ++i)
        if (!(out.max1D[i] > out.min1D[i]) || !(out.max3D[i] > out.min3D[i]))
        {
            error = "the LUT's domain is empty (DOMAIN_MIN must be lower than DOMAIN_MAX)";
            return false;
        }
    return true;
}

void apply(const Lut& lut, const float in[3], float out[3])
{
    float v[3] = {in[0], in[1], in[2]};
    if (lut.size1D >= 2)
        for (int c = 0; c < 3; ++c)
        {
            int i0 = 0;
            float f = 0.f;
            locate(v[c], lut.min1D[c], lut.max1D[c], lut.size1D, i0, f);
            v[c] = lerp(lut.table1D[(size_t)i0 * 3 + c], lut.table1D[(size_t)(i0 + 1) * 3 + c], f);
        }
    if (lut.size3D >= 2)
    {
        const int n = lut.size3D;
        int i[3];
        float f[3];
        for (int c = 0; c < 3; ++c) locate(v[c], lut.min3D[c], lut.max3D[c], n, i[c], f[c]);
        auto at = [&](int r, int g, int b, int c) {
            return lut.table3D[(((size_t)b * n + (size_t)g) * n + (size_t)r) * 3 + c];
        };
        for (int c = 0; c < 3; ++c)
        {
            const float c00 = lerp(at(i[0], i[1], i[2], c), at(i[0] + 1, i[1], i[2], c), f[0]);
            const float c10 = lerp(at(i[0], i[1] + 1, i[2], c), at(i[0] + 1, i[1] + 1, i[2], c), f[0]);
            const float c01 = lerp(at(i[0], i[1], i[2] + 1, c), at(i[0] + 1, i[1], i[2] + 1, c), f[0]);
            const float c11 = lerp(at(i[0], i[1] + 1, i[2] + 1, c), at(i[0] + 1, i[1] + 1, i[2] + 1, c), f[0]);
            out[c] = lerp(lerp(c00, c10, f[1]), lerp(c01, c11, f[1]), f[2]);
        }
        return;
    }
    std::copy(v, v + 3, out);
}

int stripSize(const Lut& lut)
{
    if (lut.size3D >= 2) return lut.size3D;
    return lut.size1D <= 33 ? 33 : kMax3D;
}

bool bakeStrip(const Lut& lut, int n, std::vector<uint8_t>& rgba)
{
    if (n < 2 || n > kMax3D || (lut.size1D < 2 && lut.size3D < 2)) return false;
    const size_t w = (size_t)n * (size_t)n;
    try
    {
        rgba.assign(w * (size_t)n * 4, 255);
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    const float step = 1.f / (float)(n - 1);
    for (int g = 0; g < n; ++g)
        for (int b = 0; b < n; ++b)
            for (int r = 0; r < n; ++r)
            {
                const float in[3] = {(float)r * step, (float)g * step, (float)b * step};
                float o[3];
                apply(lut, in, o);
                uint8_t* px = &rgba[((size_t)g * w + (size_t)b * (size_t)n + (size_t)r) * 4];
                for (int c = 0; c < 3; ++c)
                {
                    const float v = std::min(std::max(o[c], 0.f), 1.f);
                    px[c] = (uint8_t)std::lround(v * 255.f);
                }
            }
    return true;
}
} // namespace PrestigeCubeLut
