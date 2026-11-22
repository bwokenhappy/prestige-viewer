/**
 * @file vk_pipecache.cpp
 * @brief See vk_pipecache.h.
 */
#include "linden_common.h"

#include "vk_pipecache.h"
#include "vk_pipeline.h"   // #471 vkPipelineCostImport / Export

#include "llapp.h"
#include "llerror.h"
#include "llfile.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace
{
VkPipelineCache sShared = VK_NULL_HANDLE;

constexpr char     kMagic[8]      = {'S', 'L', 'V', 'K', 'P', 'C', 'H', '\0'};
constexpr uint32_t kFormatVersion = 1;
constexpr size_t   kTagBytes      = 128;

struct VkPipeCacheFileHeader
{
    char     magic[8];
    uint32_t formatVersion;
    uint32_t headerBytes;             // sizeof(VkPipeCacheFileHeader)
    char     buildTag[kTagBytes];     // NUL-padded viewer channel + version
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t driverVersion;
    uint32_t generations;             // builds that wrote this file since it started empty (0: before the count)
    uint8_t  cacheUUID[VK_UUID_SIZE];
    uint64_t payloadBytes;
    uint32_t payloadCrc32;
    uint32_t pad1;
};
static_assert(sizeof(VkPipeCacheFileHeader) % 8 == 0, "keep the payload 8-byte aligned");

const std::array<uint32_t, 256>& crcTable()
{
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> r{};
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            r[i] = c;
        }
        return r;
    }();
    return t;
}

uint32_t crc32(const uint8_t* p, size_t n)
{
    const std::array<uint32_t, 256>& t = crcTable();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void fillTag(char (&dst)[kTagBytes], const std::string& tag)
{
    std::memset(dst, 0, kTagBytes);
    std::memcpy(dst, tag.data(), std::min(tag.size(), kTagBytes - 1));
}

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A UTF-8 std::string as a path (std::filesystem::u8path is deprecated in C++20).
std::filesystem::path upath(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// Crashed writers' "<path>.tmp<pid>" files (a save killed between write and rename): removed once
// they are older than kTmpStaleSec, so a live writer's (another process) is never touched.
void sweepStaleTemps(const std::string& path)
{
    std::error_code ec;
    const std::filesystem::path p = upath(path);
    const std::filesystem::path dir = p.has_parent_path() ? p.parent_path() : std::filesystem::path(".");
    const std::string stem = p.filename().string() + ".tmp";
    const auto now = std::filesystem::file_time_type::clock::now();
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (name.compare(0, stem.size(), stem) != 0) continue;
        std::error_code e2;
        const auto t = std::filesystem::last_write_time(it->path(), e2);
        if (e2 || now - t < std::chrono::seconds(vkpipecache::kTmpStaleSec)) continue;
        const uintmax_t bytes = std::filesystem::file_size(it->path(), e2);
        if (std::filesystem::remove(it->path(), e2))
            LL_INFOS("Vulkan") << "pipeline cache: removed a crashed writer's temp file " << name << " ("
                               << (e2 ? 0 : bytes / 1024) << " KB)" << LL_ENDL;
    }
}

void discard(const std::string& path, const std::string& why)
{
    LL_WARNS("Vulkan") << "pipeline cache: discarding " << path << " (" << why << ")" << LL_ENDL;
    LLFile::remove(path, ENOENT);
}

// `tmp` over `path` in one step (#1411). Windows: MoveFileExW(REPLACE_EXISTING) via LLFile::replace --
// the old remove-then-rename left NO file when the process died between the two (a stale compile
// worker is stopped exactly like that), and let a second writer's rename fail into a missing file.
// A reader holding `path` open without share-delete refuses the replace for a moment: retried.
bool replaceAtomic(const std::string& tmp, const std::string& path)
{
    for (int i = 0; i < 10; ++i)
    {
        if (LLFile::replace(tmp, path, i < 9 ? EACCES : 0) == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
}

void vkpipecache::setShared(VkPipelineCache cache) { sShared = cache; }

VkPipelineCache vkSharedPipelineCache() { return sShared; }

bool vkpipecache::load(VkPhysicalDevice phys, const std::string& path, const std::string& buildTag,
                       std::vector<uint8_t>& out, bool readOnly, LoadInfo* info)
{
    out.clear();
    if (info) *info = LoadInfo();
    if (path.empty()) return false;
    const auto t0 = std::chrono::steady_clock::now();
    if (!readOnly) sweepStaleTemps(path);
    const std::string marker = path + ".prune";
    const bool marked = !readOnly && LLFile::isfile(marker);
    LLFILE* f = LLFile::fopen(path, "rb");
    if (!f)
    {
        if (marked) LLFile::remove(marker, ENOENT);
        LL_INFOS("Vulkan") << "pipeline cache: no file at " << path << " (cold start: pipelines compile from scratch)" << LL_ENDL;
        return false;
    }
    VkPipeCacheFileHeader h{};
    const bool gotHeader = fread(&h, 1, sizeof(h), f) == sizeof(h);
    std::string bad;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    char tag[kTagBytes];
    fillTag(tag, buildTag);
    // #1411: the viewer build tag is NOT a validity test. The driver keys every cache entry by the
    // pipeline's full content (SPIR-V, state, layout), so after a viewer update the entries of
    // unchanged shaders still hit and changed shaders simply miss (stale entries cost load time and
    // memory only, and the prune below bounds them). Discarding on a version change made every update a cold start.
    const bool otherBuild = gotHeader && std::memcmp(h.buildTag, tag, kTagBytes) != 0;
    if (!gotHeader)                                               bad = "truncated header";
    else if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0)   bad = "bad magic";
    else if (h.formatVersion != kFormatVersion || h.headerBytes != sizeof(h)) bad = "file format version";
    else if (h.vendorID != props.vendorID || h.deviceID != props.deviceID) bad = "GPU changed";
    else if (h.driverVersion != props.driverVersion)              bad = "driver changed";
    else if (std::memcmp(h.cacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) != 0) bad = "pipelineCacheUUID changed";
    else if (h.payloadBytes < sizeof(VkPipelineCacheHeaderVersionOne)) bad = "payload too small";
    else if (h.payloadBytes > vkpipecache::kMaxBytes)             bad = "payload over the size cap";
    // PRUNE (vk_pipecache.h): only a file of ANOTHER build -- the running build's own entries are
    // all live. (0: written before the count -- #1411 kept it across builds: as old as the limit.)
    const uint32_t gens = gotHeader ? (h.generations ? h.generations : vkpipecache::kPruneGenerations) : 0u;
    std::string prune;
    // (The marker is honoured whatever the tag: a file over the cap is re-tagged by the running
    // build's own bounded saves before the session ends -- pruneOnOverCap decided it holds older
    // builds' entries from its generation count.)
    if (bad.empty() && !readOnly)
    {
        if (marked) prune = "a session went over the size cap with older builds' entries in the file";
        else if (otherBuild && gens >= vkpipecache::kPruneGenerations)
            prune = std::to_string(gens) + " viewer builds have written it (limit " + std::to_string(vkpipecache::kPruneGenerations) + ")";
    }
    if (marked) LLFile::remove(marker, ENOENT);
    if (!prune.empty())
    {
        LLFile::close(f);
        LL_WARNS("Vulkan") << "pipeline cache: PRUNING " << path << " (" << h.payloadBytes / 1024 << " KB): " << prune
                           << "; starting empty, this launch compiles what it uses and saves only that" << LL_ENDL;
        LLFile::remove(path, ENOENT);
        if (info) info->pruned = true;
        return false;
    }
    if (bad.empty())
    {
        out.resize((size_t)h.payloadBytes);
        if (fread(out.data(), 1, out.size(), f) != out.size()) bad = "truncated payload";
        else if (fgetc(f) != EOF)                              bad = "trailing bytes";
        else if (crc32(out.data(), out.size()) != h.payloadCrc32) bad = "CRC mismatch";
    }
    LLFile::close(f);
    if (bad.empty())
    {
        // The driver's own header (spec: VkPipelineCacheHeaderVersionOne at offset 0).
        VkPipelineCacheHeaderVersionOne vh{};
        std::memcpy(&vh, out.data(), sizeof(vh));
        if (vh.headerSize < sizeof(vh) || vh.headerSize > out.size()) bad = "driver header size";
        else if (vh.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) bad = "driver header version";
        else if (vh.vendorID != props.vendorID || vh.deviceID != props.deviceID) bad = "driver header device";
        else if (std::memcmp(vh.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) != 0) bad = "driver header UUID";
    }
    if (!bad.empty())
    {
        out.clear();
        if (readOnly) LL_WARNS("Vulkan") << "pipeline cache: ignoring " << path << " (" << bad << ")" << LL_ENDL;
        else discard(path, bad);
        return false;
    }
    char was[kTagBytes + 1];
    std::memcpy(was, h.buildTag, kTagBytes);
    was[kTagBytes] = 0;
    if (info)
    {
        info->generations = gens;
        info->otherBuild = otherBuild;
    }
    LL_INFOS("Vulkan") << "pipeline cache: loaded " << out.size() / 1024 << " KB from " << path << " in "
                       << (int)msSince(t0) << " ms (" << gens << " build generation" << (gens == 1 ? "" : "s") << ")"
                       << (otherBuild ? std::string(" (written by viewer build '") + was
                                            + "': kept, unchanged shaders still hit)"
                                      : std::string()) << LL_ENDL;
    return true;
}

void vkpipecache::pruneMark(const std::string& path, const char* why)
{
    if (path.empty()) return;
    if (LLFILE* f = LLFile::fopen(path + ".prune", "wb"))
    {
        fputs(why, f);
        LLFile::close(f);
    }
    LL_WARNS("Vulkan") << "pipeline cache: " << path << " will start empty at the next launch (" << why << ")" << LL_ENDL;
}

size_t vkpipecache::liveBytes(VkDevice dev, VkPipelineCache cache, const char* why)
{
    size_t n = 0;
    if (!cache || vkPipelineCacheGetDataGated(dev, cache, &n, nullptr, why) != VK_SUCCESS) return 0;
    return n;
}

bool vkpipecache::save(VkDevice dev, VkPhysicalDevice phys, VkPipelineCache cache, const std::string& path,
                       const std::string& buildTag, size_t& lastBytes, const char* why, uint32_t generations,
                       bool* overCap)
{
    if (overCap) *overCap = false;
    if (path.empty() || !cache) return false;
    // ONE SAVE AT A TIME (#782 audit): the shared cache is saved from the render thread
    // ("world pipelines ready", shutdown) AND from compile workers (the progress hook) --
    // the same "<path>.tmp<pid>" file and the caller's lastBytes were raced. (The spec allows
    // reading the cache while other threads create into it; Intel Windows' igvk64 crashed doing
    // exactly that, #1490 / #1491 -- so the driver reads go through the driver gate.)
    static std::mutex s_saveMutex;
    std::lock_guard<std::mutex> saveLock(s_saveMutex);
    const auto t0 = std::chrono::steady_clock::now();
    // #1490 / #1491: every driver call below is logged around, so a crash INSIDE the save names it in
    // the next session's log (the last "save start" line has no "save end" after it). The calls go
    // through the driver gate (vk_pipeline.h): the driver reads the cache beside no create.
    LL_INFOS("VKPipe") << "pipeline cache: save start (" << why << ", " << (lastBytes / 1024) << " KB last saved)" << LL_ENDL;
    size_t n = 0;
    if (vkPipelineCacheGetDataGated(dev, cache, &n, nullptr, why) != VK_SUCCESS || n == 0)
    {
        LL_INFOS("VKPipe") << "pipeline cache: save end (" << why << "): the driver returned no data" << LL_ENDL;
        return false;
    }
    if (n > vkpipecache::kMaxBytes)
    {
        if (overCap) *overCap = true;
        // Keep the last bounded, valid snapshot. Removing it makes EVERY later launch cold
        // once the live cache grows past the cap. The opaque driver blob cannot be trimmed.
        // Do not advance lastBytes: no bytes were saved, and a worker merge must keep its file.
        LL_WARNS("Vulkan") << "pipeline cache: " << n / (1024 * 1024) << " MB exceeds the "
                           << vkpipecache::kMaxBytes / (1024 * 1024)
                           << " MB cap; keeping the previous disk cache (" << why << ")" << LL_ENDL;
        LL_INFOS("VKPipe") << "pipeline cache: save end (" << why << "): over cap, not written" << LL_ENDL;
        return false;
    }
    if (n == lastBytes)   // nothing new compiled since the last load / save
    {
        LL_INFOS("VKPipe") << "pipeline cache: save end (" << why << "): unchanged (" << n / 1024 << " KB)" << LL_ENDL;
        return true;
    }
    std::vector<uint8_t> data(n);
    VkResult r = vkPipelineCacheGetDataGated(dev, cache, &n, data.data(), why);
    // VK_INCOMPLETE = the cache grew between the two calls and `data` is a TRUNCATED
    // serialization (it would pass our CRC next launch): skip this save, retry next time.
    if (r != VK_SUCCESS)
    {
        LL_INFOS("VKPipe") << "pipeline cache: save end (" << why << "): the driver read gave " << (int)r << "; skipped" << LL_ENDL;
        return false;
    }
    data.resize(n);
    if (n < sizeof(VkPipelineCacheHeaderVersionOne)) return false;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    VkPipeCacheFileHeader h{};
    std::memcpy(h.magic, kMagic, sizeof(kMagic));
    h.formatVersion = kFormatVersion;
    h.headerBytes = sizeof(h);
    fillTag(h.buildTag, buildTag);
    h.vendorID = props.vendorID;
    h.deviceID = props.deviceID;
    h.driverVersion = props.driverVersion;
    h.generations = generations ? generations : 1u;
    std::memcpy(h.cacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE);
    h.payloadBytes = n;
    h.payloadCrc32 = crc32(data.data(), n);

    const std::string tmp = path + ".tmp" + std::to_string(LLApp::getPid());
    LLFILE* f = LLFile::fopen(tmp, "wb");
    if (!f)
    {
        LL_WARNS("Vulkan") << "pipeline cache: cannot write " << tmp << LL_ENDL;
        return false;
    }
    bool ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h) && fwrite(data.data(), 1, n, f) == n;
    ok = (fflush(f) == 0) && ok;
    ok = (LLFile::close(f) == 0) && ok;
    if (ok) ok = replaceAtomic(tmp, path);
    if (!ok)
    {
        LLFile::remove(tmp, ENOENT);
        LL_WARNS("Vulkan") << "pipeline cache: save to " << path << " failed (" << why << ")" << LL_ENDL;
        return false;
    }
    const size_t before = lastBytes;
    lastBytes = n;
    LL_INFOS("Vulkan") << "pipeline cache: saved " << n / 1024 << " KB (" << (n >= before ? "+" : "-")
                       << (n >= before ? n - before : before - n) / 1024 << " KB) to " << path << " in "
                       << (int)msSince(t0) << " ms (" << why << ", " << h.generations << " build generation"
                       << (h.generations == 1 ? "" : "s") << ")" << LL_ENDL;
    LL_INFOS("VKPipe") << "pipeline cache: save end (" << why << "): written" << LL_ENDL;
    return true;
}

// ---- #471 learned compile costs ----
namespace
{
std::string costDeviceLine(VkPhysicalDevice phys)
{
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    std::string line = "device " + std::to_string(props.vendorID) + " " + std::to_string(props.deviceID) + " "
                     + std::to_string(props.driverVersion) + " ";
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < VK_UUID_SIZE; ++i)
    {
        line += hex[props.pipelineCacheUUID[i] >> 4];
        line += hex[props.pipelineCacheUUID[i] & 15];
    }
    return line;
}

std::mutex& costFileMutex() { static std::mutex m; return m; }
}

size_t vkpipecache::loadCosts(VkPhysicalDevice phys, const std::string& path, bool readOnly)
{
    std::string body;
    if (!path.empty() && phys)
    {
        if (LLFILE* f = LLFile::fopen(path, "rb"))
        {
            std::string text(vkpipecache::kMaxCostBytes + 1, '\0');
            text.resize(fread(&text[0], 1, text.size(), f));
            LLFile::close(f);
            // Header: three lines, then the body.
            const size_t p1 = text.find('\n');
            const size_t p2 = p1 == std::string::npos ? p1 : text.find('\n', p1 + 1);
            const size_t p3 = p2 == std::string::npos ? p2 : text.find('\n', p2 + 1);
            unsigned crc = 0;
            std::string bad;
            if (text.size() > vkpipecache::kMaxCostBytes)                           bad = "over the size cap";
            else if (p3 == std::string::npos)                                       bad = "truncated header";
            else if (text.compare(0, p1, "SLVKCOST 1") != 0)                        bad = "bad magic / version";
            else if (text.compare(p1 + 1, p2 - p1 - 1, costDeviceLine(phys)) != 0) bad = "GPU or driver changed";
            else if (std::sscanf(text.c_str() + p2 + 1, "crc %x", &crc) != 1)        bad = "no CRC";
            else
            {
                body = text.substr(p3 + 1);
                if (crc32(reinterpret_cast<const uint8_t*>(body.data()), body.size()) != crc)
                {
                    bad = "CRC mismatch";
                    body.clear();
                }
            }
            if (!bad.empty())
            {
                LL_WARNS("VKPipe") << "pipeline costs: " << (readOnly ? "ignoring " : "discarding ") << path << " (" << bad << ")" << LL_ENDL;
                if (!readOnly) LLFile::remove(path, ENOENT);
            }
        }
    }
    const size_t n = vkPipelineCostImport(body);   // empty: a fresh table (new session)
    LL_INFOS("VKPipe") << "pipeline costs: " << n << " learned pipelines"
                       << (path.empty() ? std::string(" (memory only)") : " from " + path) << LL_ENDL;
    return n;
}

bool vkpipecache::saveCosts(VkPhysicalDevice phys, const std::string& path, const char* why)
{
    if (path.empty() || !phys) return false;
    const std::string body = vkPipelineCostExport();
    if (body.size() > vkpipecache::kMaxCostBytes) return false;
    char crcLine[32];
    std::snprintf(crcLine, sizeof(crcLine), "crc %08x\n",
                  crc32(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
    const std::string text = "SLVKCOST 1\n" + costDeviceLine(phys) + "\n" + crcLine + body;

    std::lock_guard<std::mutex> lk(costFileMutex());   // one writer of the temp file at a time
    const std::string tmp = path + ".tmp" + std::to_string(LLApp::getPid());
    LLFILE* f = LLFile::fopen(tmp, "wb");
    if (!f)
    {
        LL_WARNS("VKPipe") << "pipeline costs: cannot write " << tmp << LL_ENDL;
        return false;
    }
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = (fflush(f) == 0) && ok;
    ok = (LLFile::close(f) == 0) && ok;
    if (ok) ok = replaceAtomic(tmp, path);
    if (!ok)
    {
        LLFile::remove(tmp, ENOENT);
        LL_WARNS("VKPipe") << "pipeline costs: save to " << path << " failed (" << why << ")" << LL_ENDL;
        return false;
    }
    LL_DEBUGS("VKPipe") << "pipeline costs: saved " << text.size() << " bytes (" << why << ")" << LL_ENDL;
    return true;
}
