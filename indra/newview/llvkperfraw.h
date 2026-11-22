/**
 * @file llvkperfraw.h
 * @brief Fixed numeric storage for LLVKPerfCapture's raw every-frame mode.
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <ostream>

namespace LLVKPerfRaw
{
constexpr size_t kCpuColumns = 512, kGpuColumns = 512;
constexpr size_t kFramesPerSecond = 500, kMaxFrames = 60 * kFramesPerSecond;
constexpr float kAbsent = std::numeric_limits<float>::quiet_NaN();
static_assert(std::atomic<unsigned>::is_always_lock_free, "raw harvest needs lock-free atomics");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "raw frame IDs need lock-free atomics");
static_assert(std::atomic<void*>::is_always_lock_free, "raw publication needs lock-free atomics");

enum class GpuState : unsigned { Absent, Pending, Dropped, Partial, Complete };
inline const char* stateName(GpuState state)
{
    switch (state)
    {
    case GpuState::Absent: return "absent_not_recorded";
    case GpuState::Pending: return "not_ready_at_end";
    case GpuState::Dropped: return "dropped_not_ready";
    case GpuState::Partial: return "partial_passes_unavailable";
    case GpuState::Complete: return "complete";
    }
    return "unknown";
}

struct Row
{
    // Main-thread fields and harvest-thread fields are disjoint. Read only after detach
    // and all readers have left; no concurrent exporter ever observes numeric writes.
    double monotonic = 0.0;
    float wall = kAbsent, cpu = kAbsent, wait = 0.f, present = 0.f, sleep = 0.f;
    bool minimized = false, teleport = false, foreground = false, rendered = false;
    std::array<float, kCpuColumns> sections;
    float gpuTotal = kAbsent;
    GpuState gpuState = GpuState::Absent;
    unsigned gpuRecordings = 0, gpuResults = 0, gpuDrops = 0;
    std::array<float, kGpuColumns> passes;
    Row() { sections.fill(kAbsent); passes.fill(kAbsent); }
};

struct Column
{
    char name[128] = {};
    bool async = false;
};

// A single bounded arena, allocated and touched BEFORE warm-up. The CPU owns sections,
// rowsUsed and overflowFrames; one GPU harvest owner owns passes and GPU column interning.
// Overflow never overwrites old frames. The manifest makes all truncation explicit.
class Arena
{
public:
    explicit Arena(size_t capacity) : capacity(capacity), rows(new Row[capacity]) {}
    const size_t capacity;
    std::unique_ptr<Row[]> rows;
    uint64_t firstFrame = 0, overflowFrames = 0;
    std::atomic<uint64_t> lastFrame{std::numeric_limits<uint64_t>::max()};
    size_t rowsUsed = 0, gpuColumns = 0;
    uint64_t droppedColumns = 0, invalidValues = 0;
    std::array<Column, kCpuColumns> cpuNames;
    std::array<Column, kGpuColumns> gpuNames;

    Row* row(uint64_t frame) noexcept
    {
        if (frame < firstFrame || frame > lastFrame.load(std::memory_order_relaxed)
            || frame - firstFrame >= capacity) return nullptr;
        return &rows[frame - firstFrame];
    }
    Row* cpuFrame(uint64_t frame, double monotonic, float wall) noexcept
    {
        Row* r = row(frame);
        if (!r) { ++overflowFrames; return nullptr; }
        rowsUsed = static_cast<size_t>(frame - firstFrame) + 1;
        r->monotonic = monotonic;
        r->wall = wall;
        return r;
    }
    static bool copyName(Column& column, const char* name) noexcept
    {
        if (!name || std::strlen(name) >= sizeof(column.name)) return false;
        std::strcpy(column.name, name);
        return true;
    }
    void cpuSection(Row& r, size_t id, const char* name, float ms) noexcept
    {
        if (id >= kCpuColumns) return;
        if (!cpuNames[id].name[0]) copyName(cpuNames[id], name);
        r.sections[id] = ms;
    }
    void gpuRecorded(uint64_t frame) noexcept
    {
        if (Row* r = row(frame)) { ++r->gpuRecordings; }
    }
    void gpuDropped(uint64_t frame) noexcept
    {
        if (Row* r = row(frame)) { ++r->gpuDrops; r->gpuState = GpuState::Dropped; }
    }
    void gpuSample(uint64_t frame, const char* name, double ms, bool async) noexcept
    {
        Row* r = row(frame);
        if (!r || !name) return;
        if (!std::isfinite(ms) || ms < 0.0) { ++invalidValues; return; }
        // Open-addressed fixed dictionary. Names are copied once, including names first
        // encountered late in the window; no warm-up-dependent column loss or heap work.
        uint32_t hash = async ? 2166136260u ^ 1u : 2166136260u;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
            hash = (hash ^ *p) * 16777619u;
        for (size_t probe = 0; probe < kGpuColumns; ++probe)
        {
            const size_t id = (hash + probe) % kGpuColumns;
            Column& c = gpuNames[id];
            if (!c.name[0])
            {
                if (!copyName(c, name)) { ++droppedColumns; return; }
                c.async = async;
                ++gpuColumns;
            }
            if (c.async == async && std::strcmp(c.name, name) == 0)
            {
                float& value = r->passes[id];
                value = std::isnan(value) ? static_cast<float>(ms) : value + static_cast<float>(ms);
                return;
            }
        }
        ++droppedColumns;
    }
    void gpuDone(uint64_t frame, double ms, bool passesComplete) noexcept
    {
        if (Row* r = row(frame))
        {
            if (!std::isfinite(ms) || ms < 0.0) { ++invalidValues; gpuDropped(frame); return; }
            r->gpuTotal = std::isnan(r->gpuTotal) ? static_cast<float>(ms) : r->gpuTotal + static_cast<float>(ms);
            ++r->gpuResults;
            if (r->gpuState != GpuState::Dropped && r->gpuState != GpuState::Partial)
                r->gpuState = passesComplete ? GpuState::Complete : GpuState::Partial;
        }
    }
};

inline GpuState finalState(const Row& row)
{
    if (row.gpuRecordings > row.gpuResults + row.gpuDrops) return GpuState::Pending;
    return row.gpuState;
}

// A reader enters BEFORE loading the published pointer (seq_cst). Once detached, a
// writer waits for readers == 0 on later frame ticks, then transfers ownership to the
// exporter. No mutex, shared_ptr refcount, spin-wait or object reclamation in a hook.
class Publication
{
public:
    std::atomic<Arena*> arena{nullptr};
    std::atomic<unsigned> readers{0};
    class Read
    {
    public:
        explicit Read(Publication& owner) noexcept : owner(owner)
        {
            owner.readers.fetch_add(1);
            value = owner.arena.load();
        }
        ~Read() { owner.readers.fetch_sub(1); }
        Read(const Read&) = delete;
        Read& operator=(const Read&) = delete;
        Arena* value;
    private:
        Publication& owner;
    };
};

// Export only, after detach. Blank numeric cells mean absent, NEVER a fabricated zero.
inline void csvNumber(std::ostream& out, float value)
{
    if (std::isfinite(value)) out << value;
}
inline void csvName(std::ostream& out, const Column& c)
{
    for (const char* p = c.name; *p; ++p)
        out << (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
                  || (*p >= '0' && *p <= '9') || *p == '_') ? *p : '_');
}
inline void writeCsv(std::ostream& out, const Arena& arena)
{
    out << "# Slipstream raw frame timings format 1; ms except monotonic_s\n"
        << "# CPU inclusive raw sections; CPU split excludes waits/present/sleep; queues overlap\n"
        << "# Blank = absent; frame_id is recording boundary serial, not harvest serial\n"
        << "# capacity=" << arena.capacity << " rows=" << arena.rowsUsed
        << " overflow_frames=" << arena.overflowFrames << " dropped_column_samples=" << arena.droppedColumns
        << " invalid_gpu_values=" << arena.invalidValues << '\n';
    out << "frame_id,monotonic_s,wall_ms,cpu_ms,wait_ms,present_ms,fps_limit_sleep_ms,"
           "minimized,teleport,foreground,rendered,gpu_status,gpu_recordings,gpu_results,gpu_drops,gpu_total_ms";
    for (size_t i = 0; i < kCpuColumns; ++i)
        if (arena.cpuNames[i].name[0]) { out << ",cpu_" << i << '_'; csvName(out, arena.cpuNames[i]); out << "_ms"; }
    for (size_t i = 0; i < kGpuColumns; ++i)
        if (arena.gpuNames[i].name[0])
        {
            out << ",gpu_" << (arena.gpuNames[i].async ? "a_" : "g_") << i << '_';
            csvName(out, arena.gpuNames[i]); out << "_ms";
        }
    out << '\n' << std::setprecision(12);
    for (size_t i = 0; i < arena.rowsUsed; ++i)
    {
        const Row& r = arena.rows[i];
        out << arena.firstFrame + i << ',' << r.monotonic << ',';
        for (float v : {r.wall, r.cpu, r.wait, r.present, r.sleep}) { csvNumber(out, v); out << ','; }
        out << r.minimized << ',' << r.teleport << ',' << r.foreground << ',' << r.rendered << ','
            << stateName(finalState(r)) << ',' << r.gpuRecordings << ',' << r.gpuResults << ',' << r.gpuDrops << ',';
        csvNumber(out, r.gpuTotal);
        for (size_t c = 0; c < kCpuColumns; ++c)
            if (arena.cpuNames[c].name[0]) { out << ','; csvNumber(out, r.sections[c]); }
        for (size_t c = 0; c < kGpuColumns; ++c)
            if (arena.gpuNames[c].name[0]) { out << ','; csvNumber(out, r.passes[c]); }
        out << '\n';
    }
}
}
