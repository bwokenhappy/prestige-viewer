// Conservative wetness certificates for the caustic reader only. No sea outputs change.
#ifndef LL_WATER_CAUSTICS_BOUNDS_H
#define LL_WATER_CAUSTICS_BOUNDS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

struct LLWaterCausticsBounds
{
    static constexpr float reject = -std::numeric_limits<float>::max();
    uint32_t fieldVersion = ~0u, fieldSlot = ~0u;
    float fieldDh = reject;
    uint32_t planeRow = ~0u;
    float planeZ = reject, planeDz = 0.f;
    bool planesPublished = false;

    // Keep classification integer-only even under Clang's finite-math assumptions.
    // Without the volatile INTEGER load LLVM folds the bit pattern back into an
    // assumed-finite FP predicate at O3. No volatile floating arithmetic is used.
    static bool finite(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        volatile uint32_t raw = bits;
        return (raw & 0x7f800000u) != 0x7f800000u;
    }
    static float half(uint32_t bits)
    {
        const int exponent = (bits >> 10) & 31u;
        const int mantissa = bits & 1023u;
        return (bits & 32768u ? -1.f : 1.f) * std::ldexp(float(mantissa + (exponent ? 1024 : 0)),
                                                       exponent ? exponent - 25 : -24);
    }
    float publishedField(const uint32_t* data, size_t count, uint32_t version, uint32_t slot)
    {
        if (!data || count == 0) return reject;
        if (fieldVersion == version && fieldSlot == slot) return fieldDh;
        fieldVersion = version; fieldSlot = slot;
        fieldDh = 0.f; // seaOpenSea, including the window boundary, is part of the domain.
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t a = data[2 * i], b = data[2 * i + 1];
            // Any nonfinite channel can invalidate the geometry/field lookup, not only dh.
            if ((a & 0x7c00u) == 0x7c00u || (a & 0x7c000000u) == 0x7c000000u
                || (b & 0x7c00u) == 0x7c00u || (b & 0x7c000000u) == 0x7c000000u)
                return fieldDh = reject;
            fieldDh = std::min(fieldDh, half(b >> 16));
        }
        return fieldDh;
    }
    static float inset(double height, double magnitude)
    {
        // Covers float sums/mixes and the planar producer's quantized decode. An
        // additional receiver-side tolerance handles depth unprojection in the shader.
        const float result = float(height - (0.002 + 64.0 * std::numeric_limits<float>::epsilon() * (magnitude + 1.0)));
        return finite(result) ? result : reject;
    }
    static float waveLower(float plane, float minimumDh, const float* sea, const float* waves)
    {
        if (!finite(plane) || !finite(minimumDh) || minimumDh == reject) return reject;
        for (int i = 0; i < 20; ++i) if (!finite(sea[i])) return reject;
        if (sea[7] < 0.f || sea[8] < 0.f || sea[8] > 1.f || sea[9] < 0.f) return reject;
        double amplitude = 0.0;
        for (int i = 0; i < 8; ++i)
        {
            for (int j = 0; j < 8; ++j) if (!finite(waves[8 * i + j])) return reject;
            if (waves[8 * i + 5] < 0.f) return reject; // tanh(k d) must be nonnegative.
            amplitude += 1.6 * std::fabs(double(waves[8 * i + 2]));
        }
        // Breakers only lift. Fetch, depth cap and geometry LOD cannot increase |A|.
        return inset(double(plane) + minimumDh - amplitude,
                     std::fabs(double(plane)) + std::fabs(double(minimumDh)) + amplitude);
    }
    template<typename Row> static bool water(const Row& row)
    {
        return row.lod_group_id != ~0u && (row.flags & 2048u) != 0u;
    }
    template<typename Row> static float flatLower(const Row& row, float dz)
    {
        // vkIngestWaterPlane and the validated synthetic water producer publish a
        // z=0 mesh with horizontal affine pose. Their AABB z is padded by8m: it is
        // NOT the surface height. Unsupported animated/tilted flagged rows fail closed.
        if (row.flags != (2048u | 2u) || !finite(dz)) return reject;
        for (int k = 0; k < 16; ++k) if (!finite(row.model_matrix[k])) return reject;
        for (int k = 0; k < 3; ++k) if (!finite(row.spin_axis[k]) || row.spin_axis[k] != 0.f) return reject;
        const float* m = row.model_matrix;
        if (m[2] != 0.f || m[6] != 0.f || m[3] != 0.f || m[7] != 0.f || m[11] != 0.f || m[15] != 1.f)
            return reject;
        const float height = m[14] - dz; // same float subtraction as vkInstRowsToAgent.
        return inset(height, std::fabs(double(m[14])) + std::fabs(double(dz)));
    }
    template<typename Row, typename Runs>
    void publishPlanes(bool wanted, bool succeeded, bool full, const Row* rows, size_t count,
                       const Runs& runs, float dz)
    {
        if (!wanted || !succeeded) { planesPublished = false; planeZ = reject; return; }
        bool scan = full || !planesPublished || dz != planeDz || (planeRow != ~0u && planeRow >= count);
        for (const auto& run : runs)
            if (planeRow >= run.first && planeRow < run.second) scan = true;
        if (scan) { planeRow = ~0u; planeZ = reject; }
        auto take = [&](size_t id)
        {
            if (!water(rows[id])) return;
            const float z = flatLower(rows[id], dz);
            if (planeRow == ~0u || z < planeZ) { planeZ = z; planeRow = uint32_t(id); }
        };
        if (scan) for (size_t id = 0; id < count; ++id) take(id);
        else for (const auto& run : runs)
            for (size_t id = run.first; id < std::min(size_t(run.second), count); ++id) take(id);
        planeDz = dz; planesPublished = true;
    }
};
#endif
