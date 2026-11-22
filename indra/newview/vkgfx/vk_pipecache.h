/**
 * @file vk_pipecache.h
 * @brief Persistent VkPipelineCache on disk (load / validate / save).
 *
 * The device context (VkDeviceContext) owns ONE process-wide VkPipelineCache, created
 * at device init from the file this module loads, and passed to every
 * vkCreateGraphicsPipelines / vkCreateComputePipelines call in the viewer (through
 * vkSharedPipelineCache(), vk_common.h). Driver-internal compiler caches are separate;
 * a loaded application cache or a reported hit does not guarantee zero creation cost.
 *
 * FILE FORMAT (native endianness; a file from another machine fails the device check):
 *   VkPipeCacheFileHeader (below): magic "SLVKPCH\0", format version, header size,
 *     viewer build tag (channel + version), vendorID / deviceID / driverVersion /
 *     pipelineCacheUUID of the device that wrote it, payload size, payload CRC32.
 *   payload: exactly what vkGetPipelineCacheData returned. Its leading
 *     VkPipelineCacheHeaderVersionOne is validated too (header size / version,
 *     vendorID, deviceID, pipelineCacheUUID == this device) before the driver sees it:
 *     drivers can crash on bad cache data despite the spec.
 * Any mismatch / corruption: the file is ignored (and removed) and the cache starts
 * empty. Saves are atomic (temp file + rename). Payloads over kMaxBytes are neither
 * loaded nor saved.
 * #1411: the build tag is recorded (and logged) but is NOT a validity check: entries are keyed by
 * the pipeline's content inside the driver, so a new viewer build with unchanged shaders still
 * hits, and a changed shader only misses. Failed or oversized saves keep the previous file; no
 * opaque driver payload is ever truncated.
 *
 * SELF-PRUNING (no unbounded growth): the entries of an older build's changed shaders are dead
 * weight the opaque blob cannot drop, so the file records how many viewer builds have written
 * to it since it last started empty (the header's `generations`). The next launch of ANOTHER
 * build starts the cache EMPTY (the file is removed; one launch compiles cold, the driver's own
 * disk cache may soften it) when the file already spans kPruneGenerations builds, or when the
 * previous session ended over kMaxBytes with entries of an older build (pruneMark). A file
 * written by the running build is never pruned: one build's pipelines always fit (kMaxBytes is
 * derived from the single-build worst case below), so a rebuilt cache cannot loop.
 * Files from before the generation count (0) are treated as kPruneGenerations old: their #1411
 * cross-build accumulation is cleaned once, by the first launch of a new build.
 * Crashed writers' temp files ("<path>.tmp<pid>", older than kTmpStaleSec) are removed at load.
 */
#pragma once

#include "vk_common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vkpipecache
{
// PER-FILE CAP (main cache, compile-worker cache, each per-key spec cache), DERIVED from the
// single-build worst case: every pipeline one session can create into one file with every
// feature on, HEAVY ones (lit / ray-query fragment shaders, ~0.7-1.0M SPIR-V words: the upgrade
// group, the RT groups, WBOIT accumulation) at the largest per-pipeline driver blob measured and
// the LIGHT rest at the measured mean, plus a safety margin.
// (The scripts/check pipeline-cache-preserve test compiles these static_asserts.)
//
// COUNT (pipelines created into the main file in one session), MEASURED with every feature on:
//   RX 7900 XTX, RT + GI 3 + ReSTIR + RT reflections + WBOIT + FSR (2026-11-19 replay): 445 into the
//   main cache (508 items, 63 of them in the per-key spec files), ~121 of them heavy (upgrade 53,
//   RT 56, WBOIT accumulation 2 x 6); owner RTX 5090 session (run-mega-a5fa13fc38): 447.
// ESTIMATED from the creation sites for what those sessions did not exercise: a VR / stereo
//   session rebuilds the main-view sets as multiview twins (~380, ~121 heavy), RezFX wire twins of
//   the lit set (~60 heavy), WBOIT / display-mode / spec-key switches (~30), god rays, motion
//   blur, weather, cluster LOD (~40), UI + effects stack (~50): ~1005 -> 1024, of them 320 heavy.
// BLOB (driver bytes per pipeline in the file), MEASURED: GTX 1080 ~114 KB (43.7 MB / 383);
//   RX 7900 XTX ~159 KB main / ~309 KB spec (70.6 MB / 445, 19.5 MB / 63); RTX 5090 (the largest):
//   0.86-1.13 MB per heavy lit / ray-query pipeline (spec files +24 MB / 28, +33 MB / 29) and
//   ~0.55 MB mean per main-cache miss (+78 MB / ~141, heavy ones included: it overstates light).
// 320 x 1.13 MB + 704 x 0.55 MB = ~749 MB; x 1.25 = ~936 MB <= 1024 MB.
// The "pipeline cache growth" log line (KB per driver miss) re-measures the blob on any machine.
constexpr size_t kWorstHeavyPipelines = 320;
constexpr size_t kWorstLightPipelines = 704;
constexpr size_t kWorstPipelinesPerFile = kWorstHeavyPipelines + kWorstLightPipelines;
constexpr size_t kWorstHeavyBlobBytes = 1157u * 1024u;   // 1.13 MB
constexpr size_t kWorstLightBlobBytes = 563u * 1024u;    // 0.55 MB
constexpr size_t kWorstMarginPct = 125;
constexpr size_t kWorstCaseBytes = (kWorstHeavyPipelines * kWorstHeavyBlobBytes + kWorstLightPipelines * kWorstLightBlobBytes)
                                   / 100 * kWorstMarginPct;
constexpr size_t kMaxBytes = 1024ull * 1024ull * 1024ull;
static_assert(kMaxBytes >= kWorstCaseBytes, "the cap must hold one build's worst case (or a pruned cache could loop)");
// A file other builds have written this many times starts empty at the next build change.
constexpr uint32_t kPruneGenerations = 3;
// A temp file this old is a crashed writer's (a save writes ~1 GB in well under a minute).
constexpr int kTmpStaleSec = 600;

// What load() found besides the payload (for the next save's generation count).
struct LoadInfo
{
    uint32_t generations = 0;   // builds that wrote the file since it last started empty (0: none / no file)
    bool otherBuild = false;    // written by another viewer build (its tag differs)
    bool pruned = false;        // the file was pruned (removed): the caller starts empty, and drops what was
                                // seeded from it (the compile worker's file: vk_device.cpp)
};
// The generation count the next save of a file loaded with `info` writes under `buildTag`.
inline uint32_t nextGenerations(const LoadInfo& info) { return info.generations == 0 ? 1u : info.generations + (info.otherBuild ? 1u : 0u); }

// Reads + validates `path` for this physical device (+ driver, pipelineCacheUUID). True with
// the driver payload in `out`; false (out empty) when absent, stale or corrupt (logged; a
// corrupt or stale file is deleted). `buildTag` is only compared for the log (#1411).
// readOnly: never deletes the file (#1411: the compile worker reading the viewer's cache as its seed).
// PRUNE (not readOnly): a file of another build that spans kPruneGenerations builds, or (any
// tag) one a pruneMark names, is removed and the load returns false (an empty start). `info`: optional.
bool load(VkPhysicalDevice phys, const std::string& path, const std::string& buildTag,
          std::vector<uint8_t>& out, bool readOnly = false, LoadInfo* info = nullptr);

// Writes the cache's current contents to `path` (atomic). Skipped when the payload
// size equals `lastBytes` (nothing new compiled since the last load / save); updates
// `lastBytes` only after a successful write. Oversize / failed saves keep the previous disk
// file and leave `lastBytes` unchanged. `why` is logged.
// `generations` goes into the header (nextGenerations); *overCap (optional) is set when the
// live cache was over kMaxBytes (nothing written).
bool save(VkDevice dev, VkPhysicalDevice phys, VkPipelineCache cache, const std::string& path,
          const std::string& buildTag, size_t& lastBytes, const char* why, uint32_t generations = 1,
          bool* overCap = nullptr);

// A save finds the live cache over kMaxBytes and the file holds an older build's entries:
// the next launch starts `path` empty (a small "<path>.prune" marker; load() acts on it).
void pruneMark(const std::string& path, const char* why);
// Over the cap: mark the file for an empty start only when it spans more than one build (older
// builds' dead entries are what a rebuild drops). One build's own pipelines over the cap never
// mark (kMaxBytes holds one build's worst case: a rebuilt file restarts at 1 and cannot loop).
constexpr bool pruneOnOverCap(uint32_t generations) { return generations > 1u; }

// The cache's live serialized size right now (one size query through the driver gate; 0 on failure).
size_t liveBytes(VkDevice dev, VkPipelineCache cache, const char* why);

// Sets the process-wide cache returned by vkSharedPipelineCache() (vk_common.h).
void setShared(VkPipelineCache cache);

// LEARNED PIPELINE COMPILE COSTS (#471; vk_pipeline.h vkPipelineCostImport / Export),
// a small text file next to the cache. Unlike the cache it is NOT tied to the viewer
// build (a new release is exactly when everything compiles cold; changed shaders get new
// keys anyway), only to the device + driver (vendorID, deviceID, driverVersion,
// pipelineCacheUUID): a driver update starts fresh. Layout:
//   SLVKCOST 1
//   device <vendorID> <deviceID> <driverVersion> <pipelineCacheUUID hex>
//   crc <CRC32 of the body, hex>
//   <body: vkPipelineCostExport text>
// Bounded (kMaxCostBytes). Mismatch / corruption: deleted, start fresh.
constexpr size_t kMaxCostBytes = 1024u * 1024u;
// Reads + validates `path` into the vk_pipeline cost table (always starts a new session,
// empty if the file is missing or stale). Returns the entries loaded.
size_t loadCosts(VkPhysicalDevice phys, const std::string& path, bool readOnly = false);   // readOnly: never removes a bad file (#1411 worker)
// Writes the vk_pipeline cost table to `path` (atomic). Any thread; serialized.
bool saveCosts(VkPhysicalDevice phys, const std::string& path, const char* why);
}
