// Real filesystem and production persistence code (vk_pipecache.h + .cpp); no GPU or viewer launch.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using VkDevice = int;
using VkPhysicalDevice = int;
using VkPipelineCache = int;
using VkResult = int;
using LLFILE = FILE;
constexpr int VK_NULL_HANDLE = 0, VK_SUCCESS = 0, VK_INCOMPLETE = 5;
constexpr size_t VK_UUID_SIZE = 16;
constexpr uint32_t VK_PIPELINE_CACHE_HEADER_VERSION_ONE = 1;
struct VkPipelineCacheHeaderVersionOne
{
    uint32_t headerSize, headerVersion, vendorID, deviceID;
    uint8_t pipelineCacheUUID[VK_UUID_SIZE];
};
struct VkPhysicalDeviceProperties
{
    uint32_t vendorID = 1, deviceID = 2, driverVersion = 3;
    uint8_t pipelineCacheUUID[VK_UUID_SIZE] = {4};
};
void vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) { *p = {}; }
#define LL_INFOS(tag) std::cerr
#define LL_WARNS(tag) std::cerr
#define LL_ENDL std::endl
struct LLApp { static int getPid() { return 123; } };
bool failOpen = false, failClose = false, failReplace = false;
struct LLFile
{
    static FILE* fopen(const std::string& p, const char* mode)
    {
        return failOpen && mode[0] == 'w' ? nullptr : std::fopen(p.c_str(), mode);
    }
    static int close(FILE* f) { int r = std::fclose(f); return failClose ? EOF : r; }
    static int remove(const std::string& p, int) { return std::remove(p.c_str()); }
    static bool isfile(const std::string& p) { return std::filesystem::is_regular_file(p); }
    static int replace(const std::string& a, const std::string& b, int)
    {
        return failReplace ? -1 : std::rename(a.c_str(), b.c_str());
    }
};
std::vector<uint8_t> driver;
size_t queryBytes = 0;
int queryResult = VK_SUCCESS, readResult = VK_SUCCESS;
int reads = 0;
VkResult vkPipelineCacheGetDataGated(VkDevice, VkPipelineCache, size_t* n, void* data, const char*)
{
    if (!data) { *n = queryBytes; return queryResult; }
    ++reads;
    assert(*n >= driver.size());
    std::memcpy(data, driver.data(), driver.size());
    *n = driver.size();
    return readResult;
}
#include "cache_under_test.inc"

std::string disk(const char* path = "cache.bin")
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
bool exists(const char* path) { return std::filesystem::exists(path); }
void payload(size_t size, uint8_t fill)
{
    driver.assign(size, fill);
    VkPipelineCacheHeaderVersionOne h{sizeof(h), 1, 1, 2, {4}};
    std::memcpy(driver.data(), &h, sizeof(h));
    queryBytes = size;
}
// The header's generation field, read back from the file (offset: magic 8 + version 4 + header
// size 4 + tag 128 + vendor 4 + device 4 + driver 4).
uint32_t fileGenerations()
{
    const std::string d = disk();
    uint32_t g = 0;
    std::memcpy(&g, d.data() + 8 + 4 + 4 + 128 + 12, sizeof(g));
    return g;
}
void setFileGenerations(uint32_t g)
{
    std::fstream f("cache.bin", std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(8 + 4 + 4 + 128 + 12);
    f.write(reinterpret_cast<const char*>(&g), sizeof(g));
}

int main()
{
    // ---- the cap: one build's derived worst case fits (also a static_assert in the header) ----
    static_assert(vkpipecache::kMaxBytes >= vkpipecache::kWorstCaseBytes);
    static_assert(vkpipecache::kWorstCaseBytes == (vkpipecache::kWorstHeavyPipelines * vkpipecache::kWorstHeavyBlobBytes
                                                   + vkpipecache::kWorstLightPipelines * vkpipecache::kWorstLightBlobBytes)
                                                      / 100 * vkpipecache::kWorstMarginPct);
    static_assert(vkpipecache::kWorstHeavyBlobBytes >= 1150u * 1024u, "heavy pipelines at the largest blob measured (RTX 5090)");
    static_assert(vkpipecache::kWorstPipelinesPerFile >= 1000, "the all-on count incl. VR / RezFX / switches");
    static_assert(vkpipecache::kWorstMarginPct >= 110, "keep a safety margin over the measured worst case");

    size_t saved = 0;
    auto save = [&] { return vkpipecache::save(1, 1, 1, "cache.bin", "build A", saved, "test"); };
    payload(64, 11);
    assert(save() && saved == 64);
    const auto seed = disk();
    const auto seedPayload = driver;
    std::vector<uint8_t> loaded;
    vkpipecache::LoadInfo info;
    assert(vkpipecache::load(1, "cache.bin", "build B", loaded, false, &info) && loaded == seedPayload);
    assert(info.generations == 1 && info.otherBuild);
    assert(vkpipecache::nextGenerations(info) == 2);
    const int beforeReads = reads;
    assert(save() && reads == beforeReads); // unchanged bounded snapshot

    // ---- oversize: the previous file stays (Astra 08ccbc65fe), reported as over the cap ----
    queryBytes = vkpipecache::kMaxBytes + 1;
    for (int launch = 0; launch < 3; ++launch)
    {
        bool over = false;
        assert(!vkpipecache::save(1, 1, 1, "cache.bin", "build A", saved, "test", 1, &over) && over);
        assert(saved == 64 && disk() == seed && reads == beforeReads);
        assert(vkpipecache::load(1, "cache.bin", "build A", loaded) && loaded == seedPayload);
    }
    // A worker merge resets saved bytes; repeated failed saves must never consume its file.
    saved = 0;
    assert(!save() && saved == 0 && disk() == seed);
    assert(!save() && saved == 0 && disk() == seed);

    // ---- ATOMIC: every failure on the way leaves the previous file byte for byte, no temp ----
    payload(96, 22);
    queryResult = -1;
    assert(!save() && saved == 0 && disk() == seed);
    queryResult = VK_SUCCESS;
    readResult = VK_INCOMPLETE;
    assert(!save() && saved == 0 && disk() == seed);
    readResult = VK_SUCCESS;
    failOpen = true;
    assert(!save() && saved == 0 && disk() == seed);
    failOpen = false;
    failClose = true;   // (a crash after a partial write: the temp is never renamed over the file)
    assert(!save() && saved == 0 && disk() == seed);
    failClose = false;
    failReplace = true;
    assert(!save() && saved == 0 && disk() == seed);
    failReplace = false;
    assert(!exists("cache.bin.tmp123"));
    {
        bool over = true;
        assert(vkpipecache::save(1, 1, 1, "cache.bin", "build A", saved, "test", 1, &over) && !over);
    }
    assert(saved == 96 && disk() != seed);
    assert(vkpipecache::load(1, "cache.bin", "build C", loaded) && loaded == driver);
    assert(!exists("cache.bin.tmp123"));
    // Over cap with no prior snapshot must not create a fake/truncated serialization.
    assert(std::remove("cache.bin") == 0);
    saved = 0;
    queryBytes = vkpipecache::kMaxBytes + 1;
    assert(!save() && saved == 0 && !exists("cache.bin"));

    // ---- GENERATIONS + PRUNE ----
    auto writeGen = [&](const char* tag, uint32_t gen) {
        payload(80, 33);
        size_t s = 0;
        assert(vkpipecache::save(1, 1, 1, "cache.bin", tag, s, "test", gen));
        assert(fileGenerations() == gen);
    };
    // Under the limit: another build loads and keeps it; the generation it then writes is +1.
    writeGen("build A", vkpipecache::kPruneGenerations - 1);
    assert(vkpipecache::load(1, "cache.bin", "build B", loaded, false, &info));
    assert(info.generations == vkpipecache::kPruneGenerations - 1 && info.otherBuild);
    assert(vkpipecache::nextGenerations(info) == vkpipecache::kPruneGenerations);
    // The same build never adds a generation.
    assert(vkpipecache::load(1, "cache.bin", "build A", loaded, false, &info) && !info.otherBuild);
    assert(vkpipecache::nextGenerations(info) == vkpipecache::kPruneGenerations - 1);
    // At the limit: ANOTHER build prunes it (empty start, file removed) ...
    writeGen("build A", vkpipecache::kPruneGenerations);
    assert(vkpipecache::load(1, "cache.bin", "build A", loaded, false, &info));   // (its own build: kept)
    assert(!vkpipecache::load(1, "cache.bin", "build B", loaded, false, &info) && loaded.empty());
    assert(!exists("cache.bin") && info.generations == 0 && info.pruned);
    assert(vkpipecache::nextGenerations(info) == 1);   // (the rebuilt file starts the count again)
    // ... but a read-only reader (the compile worker's seed) never prunes.
    writeGen("build A", vkpipecache::kPruneGenerations + 2);
    assert(vkpipecache::load(1, "cache.bin", "build B", loaded, true) && exists("cache.bin"));
    // A file written before the count (0) is as old as the limit: pruned by another build only.
    writeGen("build A", 1);
    setFileGenerations(0);
    assert(vkpipecache::load(1, "cache.bin", "build A", loaded, false, &info) && exists("cache.bin"));
    assert(!vkpipecache::load(1, "cache.bin", "build B", loaded, false, &info) && !exists("cache.bin"));

    // ---- OVER-CAP MARKER (the session ended over the cap with an older build's entries) ----
    // Only a multi-build file is marked; one build's own pipelines over the cap never are.
    static_assert(!vkpipecache::pruneOnOverCap(1) && vkpipecache::pruneOnOverCap(2));
    // A marker is honoured whatever the tag (the over-cap session's bounded saves re-tag the file).
    writeGen("build A", 2);
    vkpipecache::pruneMark("cache.bin", "test");
    assert(exists("cache.bin.prune"));
    assert(!vkpipecache::load(1, "cache.bin", "build A", loaded, false, &info) && info.pruned);
    assert(!exists("cache.bin.prune") && !exists("cache.bin"));
    writeGen("build A", 1);
    vkpipecache::pruneMark("cache.bin", "test");
    assert(!vkpipecache::load(1, "cache.bin", "build B", loaded) && !exists("cache.bin") && !exists("cache.bin.prune"));
    vkpipecache::pruneMark("cache.bin", "test");   // (no file: the marker is dropped, not left behind)
    assert(!vkpipecache::load(1, "cache.bin", "build B", loaded) && !exists("cache.bin.prune"));

    // ---- CRASHED WRITERS' TEMP FILES: old ones swept at load, a live writer's (recent) kept ----
    writeGen("build A", 1);
    {
        std::ofstream("cache.bin.tmp999") << "partial";
        std::ofstream("cache.bin.tmp777") << "in progress";
        std::filesystem::last_write_time("cache.bin.tmp999", std::filesystem::file_time_type::clock::now()
                                                              - std::chrono::seconds(vkpipecache::kTmpStaleSec + 60));
    }
    assert(vkpipecache::load(1, "cache.bin", "build A", loaded));
    assert(!exists("cache.bin.tmp999") && exists("cache.bin.tmp777"));
    std::remove("cache.bin.tmp777");
    std::cout << "pipeline cache: all checks passed (cap " << vkpipecache::kMaxBytes / (1024 * 1024) << " MB >= worst case "
              << vkpipecache::kWorstCaseBytes / (1024 * 1024) << " MB)" << std::endl;
}
