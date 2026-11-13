#!/usr/bin/env python3
"""Execute actual Vulkan pipeline GC with real robin-map storage and fake native fences.

The GC method and cache-entry declaration are copied verbatim. The production lifetime header
and robin-map implementation are included directly. Only surrounding device, cache persistence,
pipeline keys and fence objects are fixtures; no Vulkan loader or GPU work is involved.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


PREFIX = r'''
#include "vulkan/VulkanPipelineLifetime.h"
#include <tsl/robin_map.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
using std::size_t;
using VkPipeline = uint64_t;
constexpr int VK_SUCCESS = 0;
constexpr int VK_NOT_READY = 1;
constexpr int VK_ERROR_DEVICE_LOST = -4;
#define VKALLOC nullptr
void require(bool value, char const* message) {
    if (!value) throw std::runtime_error(message);
}
#define assert_invariant(value) require(bool(value), "production invariant failed")
namespace filament::backend {
std::vector<VkPipeline> destroyed;
void vkDestroyPipeline(int, VkPipeline handle, void*) {
    require(std::find(destroyed.begin(), destroyed.end(), handle) == destroyed.end(),
            "duplicate pipeline destruction");
    destroyed.push_back(handle);
}
struct VulkanCmdFence {
    int status = VK_NOT_READY;
    unsigned polls = 0;
    int getStatus() { ++polls; return status; }
};
struct ProgramToken {};
enum class CompilerPriorityQueue { LOW };
struct FixtureContext {
    bool isPipelineCachePersistenceEnabled() const { return false; }
};
struct FixtureCompilerPool {
    template<class Callback>
    void queue(CompilerPriorityQueue, std::shared_ptr<ProgramToken>, Callback&&) {
        throw std::runtime_error("disabled persistence unexpectedly queued work");
    }
};
constexpr uint64_t PIPELINE_CACHE_SAVE_QUIET = 120;
// ACTUAL_GC_CONSTANTS
struct VulkanPipelineCache {
    struct PipelineKey { uint32_t id; };
    // Force a collision chain so robin-map erasure moves later buckets. Keeping old iterators
    // across multiple erasures therefore fails deterministically without relying on a crash.
    struct PipelineHashFn { size_t operator()(PipelineKey const&) const { return 0; } };
    struct PipelineEqual {
        bool operator()(PipelineKey const& a, PipelineKey const& b) const { return a.id == b.id; }
    };
    using Timestamp = uint64_t;
'''

MIDDLE = r'''
    using PipelineMap = tsl::robin_map<PipelineKey, PipelineCacheEntry,
            PipelineHashFn, PipelineEqual>;
    PipelineMap mPipelines;
    Timestamp mCurrentTime = 0;
    bool mPipelineCacheThresholdEvictionEnabled = false;
    FixtureContext mContext;
    FixtureCompilerPool mCompilerThreadPool;
    std::atomic<uint32_t> mPipelinesCreated{0};
    uint32_t mPipelinesSeen = 0;
    Timestamp mPipelinesSeenAt = 0;
    std::atomic<bool> mPipelinesUnsaved{false};
    int mDevice = 0;
    unsigned resets = 0;
    void resetBoundPipeline() { ++resets; }
    bool savePipelineCache() { throw std::runtime_error("disabled persistence unexpectedly saved"); }
    void gc() noexcept;
};
'''

ORACLE = r'''
namespace {
using Cache = VulkanPipelineCache;
using Fence = VulkanCmdFence;
constexpr uint64_t NOW = uint64_t(FVK_MAX_PIPELINE_AGE) * 20 + 100;
std::shared_ptr<Fence> fence(int status) {
    auto value = std::make_shared<Fence>();
    value->status = status;
    return value;
}
void add(Cache& cache, uint32_t id, uint64_t lastUsed, std::shared_ptr<Fence> ordinary = {},
        std::shared_ptr<Fence> protectedUse = {}) {
    Cache::PipelineCacheEntry entry{ .handle = id, .lastUsed = lastUsed, .lifetime = {} };
    if (ordinary) entry.lifetime.used(lastUsed, false, std::move(ordinary));
    if (protectedUse) entry.lifetime.used(lastUsed, true, std::move(protectedUse));
    auto const result = cache.mPipelines.emplace(Cache::PipelineKey{id}, std::move(entry));
    require(result.second, "duplicate fixture key");
}
bool has(Cache const& cache, uint32_t id) {
    return cache.mPipelines.find(Cache::PipelineKey{id}) != cache.mPipelines.end();
}
void fillYoung(Cache& cache, size_t total) {
    uint32_t id = 10000;
    while (cache.mPipelines.size() < total) add(cache, id++, NOW, fence(VK_SUCCESS));
}
void queueFences(bool threshold) {
    Cache cache;
    cache.mPipelineCacheThresholdEvictionEnabled = threshold;
    cache.mCurrentTime = NOW - 1;
    auto ordinary = fence(VK_NOT_READY);
    auto protectedUse = fence(VK_NOT_READY);
    add(cache, 1, 1, ordinary);
    add(cache, 2, 2, {}, protectedUse);
    add(cache, 3, 3, fence(VK_SUCCESS), protectedUse);
    add(cache, 4, 4, ordinary, fence(VK_SUCCESS));
    add(cache, 5, 5, fence(VK_SUCCESS), fence(VK_SUCCESS));
    add(cache, 6, 6, fence(VK_ERROR_DEVICE_LOST));
    add(cache, 7, NOW - uint64_t(FVK_MAX_PIPELINE_AGE), fence(VK_SUCCESS));
    if (threshold) fillYoung(cache, FVK_MAX_PIPELINE_COUNT + 10);
    cache.gc();
    require(destroyed == std::vector<VkPipeline>{5}, "pending ordinary/protected/error fence retired");
    require(has(cache, 1) && has(cache, 2) && has(cache, 3) && has(cache, 4) && has(cache, 6),
            "ordinary/protected completion was conflated");
    require(has(cache, 7), "exact age boundary was not retained");
    require(ordinary->polls && protectedUse->polls && cache.resets == 1,
            "fence polling or command-buffer binding reset was skipped");
    ordinary->status = VK_SUCCESS;
    cache.gc();
    require(!has(cache, 1) && !has(cache, 4) && has(cache, 2) && has(cache, 3),
            "ordinary completion incorrectly released a pending protected queue");
    protectedUse->status = VK_SUCCESS;
    cache.gc();
    require(!has(cache, 2) && !has(cache, 3) && has(cache, 6),
            "protected completion or error fence handling changed");
}
void fenceRelease(bool threshold) {
    Cache cache;
    cache.mPipelineCacheThresholdEvictionEnabled = threshold;
    cache.mCurrentTime = NOW - 1;
    auto ordinary = fence(VK_SUCCESS), protectedUse = fence(VK_SUCCESS);
    std::weak_ptr<Fence> ordinaryWeak = ordinary, protectedWeak = protectedUse;
    add(cache, 1, NOW, ordinary, protectedUse);
    ordinary.reset();
    protectedUse.reset();
    require(!ordinaryWeak.expired() && !protectedWeak.expired(), "fixture failed to retain fences");
    cache.gc();
    require(destroyed.empty() && has(cache, 1), "young entry was retired");
    require(ordinaryWeak.expired() && protectedWeak.expired(),
            "under-capacity completed fences were not released");
}
void oldestEligible() {
    Cache cache;
    cache.mPipelineCacheThresholdEvictionEnabled = true;
    cache.mCurrentTime = NOW - 1;
    auto pending = fence(VK_NOT_READY);
    add(cache, 1, 1, pending);
    // Insertion and eviction order differ. All keys collide, so the real container shifts
    // buckets after erasure and invalidates stale candidate iterators.
    for (uint32_t id : {5u, 2u, 6u, 3u, 4u}) add(cache, id, id, fence(VK_SUCCESS));
    fillYoung(cache, FVK_MAX_PIPELINE_COUNT + 4);
    cache.gc();
    require(destroyed == std::vector<VkPipeline>({2, 3, 4, 5}),
            "oldest eligible ordering or multiple-erasure key identity changed");
    require(cache.mPipelines.size() == FVK_MAX_PIPELINE_COUNT && has(cache, 1) && has(cache, 6),
            "capacity target, pending oldest or newest eligible preservation changed");
    cache.gc();
    require(destroyed.size() == 4 && pending->polls >= 2,
            "under-capacity cache evicted entries or stopped fence polling");
}
void insufficientEligible() {
    Cache cache;
    cache.mPipelineCacheThresholdEvictionEnabled = true;
    cache.mCurrentTime = NOW - 1;
    fillYoung(cache, FVK_MAX_PIPELINE_COUNT + 7);
    cache.gc();
    require(destroyed.empty() && cache.mPipelines.size() == FVK_MAX_PIPELINE_COUNT + 7,
            "capacity pressure evicted young ineligible entries");
}
void repeatedUse() {
    Cache cache;
    cache.mCurrentTime = NOW - 1;
    add(cache, 1, 1, fence(VK_SUCCESS));
    auto& entry = cache.mPipelines.find(Cache::PipelineKey{1}).value();
    auto latest = fence(VK_NOT_READY);
    entry.lastUsed = NOW;
    entry.lifetime.used(NOW, false, latest);
    cache.gc();
    require(destroyed.empty(), "repeated actual use did not renew lifetime");
    latest->status = VK_SUCCESS;
    cache.mCurrentTime = NOW + uint64_t(FVK_MAX_PIPELINE_AGE) - 1;
    cache.gc();
    require(destroyed.empty(), "renewed exact-age boundary was not retained");
    cache.gc();
    require(destroyed == std::vector<VkPipeline>{1}, "completed renewed entry was not evicted");
}
} // namespace
} // namespace filament::backend
int main(int argc, char** argv) {
    try {
        require(argc == 2, "one case argument is required");
        using namespace filament::backend;
        std::string const mode = argv[1];
        if (mode == "ordinary-fences") queueFences(false);
        else if (mode == "capacity-fences") queueFences(true);
        else if (mode == "ordinary-release") fenceRelease(false);
        else if (mode == "capacity-release") fenceRelease(true);
        else if (mode == "oldest-eligible") oldestEligible();
        else if (mode == "insufficient-eligible") insufficientEligible();
        else if (mode == "repeated-use") repeatedUse();
        else throw std::runtime_error("unknown case");
        std::printf("Pipeline GC PASS: %s; real robin-map, actual GC/lifetime; no GPU execution\n", argv[1]);
        return 0;
    } catch (std::exception const& error) {
        std::fprintf(stderr, "Pipeline GC FAIL: %s\n", error.what());
        return 1;
    }
}
'''


def actual_declaration(source, signature, trailing_semicolon=False):
    if source.count(signature) != 1:
        raise ValueError(f"Actual source signature drift: {signature}")
    start = source.index(signature)
    masked = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda match: " " * len(match[0]), source, flags=re.DOTALL)
    first = masked.index("{", start)
    depth = 0
    for end in range(first, len(masked)):
        depth += (masked[end] == "{") - (masked[end] == "}")
        if depth == 0:
            end += 1
            if trailing_semicolon:
                if source[end:end + 1] != ";":
                    raise ValueError("Actual cache-entry declaration terminator changed")
                end += 1
            return source[start:end]
    raise ValueError("Unbalanced actual source declaration")


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f"Mutation site drift: {old}")
    return source.replace(old, new, 1)


def variants(gc):
    pattern = r"cacheEntry\.lifetime\.canEvict\(\s*mCurrentTime, FVK_MAX_PIPELINE_AGE, isComplete\)"
    if len(re.findall(pattern, gc)) != 2:
        raise ValueError("Both actual GC fence gates must be present; source review required")
    missing = re.sub(pattern, "(mCurrentTime > cacheEntry.lastUsed + FVK_MAX_PIPELINE_AGE)", gc)
    early = replace_once(gc, "if (mPipelineCacheThresholdEvictionEnabled) {",
                         "if (mPipelineCacheThresholdEvictionEnabled) {\n"
                         "        if (mPipelines.size() <= FVK_MAX_PIPELINE_COUNT) return;")
    reverse = replace_once(gc, "return a.lastUsed < b.lastUsed;", "return a.lastUsed > b.lastUsed;")
    unsafe = replace_once(gc, "std::vector<ExpiredPipeline> expiredCandidates;",
                          "using PipeIterator = decltype(mPipelines)::iterator;\n"
                          "        std::vector<PipeIterator> expiredCandidates;")
    unsafe = replace_once(unsafe, "expiredCandidates.push_back({ iter.key(), cacheEntry.lastUsed });",
                          "expiredCandidates.push_back(iter);")
    unsafe = replace_once(unsafe, "[](ExpiredPipeline const& a, ExpiredPipeline const& b) {\n"
                          "                    return a.lastUsed < b.lastUsed;",
                          "[](PipeIterator const& a, PipeIterator const& b) {\n"
                          "                    return a.value().lastUsed < b.value().lastUsed;")
    unsafe = replace_once(unsafe, "auto const iter = mPipelines.find(expiredCandidates[i].key);",
                          "auto const iter = expiredCandidates[i];")
    return {
        "production": (gc, ("ordinary-fences", "capacity-fences", "ordinary-release",
                            "capacity-release", "oldest-eligible", "insufficient-eligible", "repeated-use")),
        "missing-completion-guard": (missing, ("ordinary-fences", "capacity-fences")),
        "under-capacity-early-return": (early, ("capacity-release",)),
        "reversed-eviction-order": (reverse, ("oldest-eligible",)),
        "saved-invalidated-iterators": (unsafe, ("oldest-eligible",)),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--filament", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    filament = args.filament.resolve()
    directory = filament / "filament/backend/src/vulkan"
    paths = [directory / name for name in ("VulkanPipelineCache.cpp", "VulkanPipelineCache.h",
                                          "VulkanPipelineLifetime.h", "VulkanConstants.h")]
    paths += [filament / "third_party/robin-map/include/tsl" / name
              for name in ("robin_map.h", "robin_hash.h", "robin_growth_policy.h")]
    identities = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
    source = paths[0].read_text()
    gc = actual_declaration(source, "void VulkanPipelineCache::gc() noexcept")
    entry = actual_declaration(paths[1].read_text(), "struct PipelineCacheEntry {", True)
    constants = []
    for name in ("FVK_MAX_COMMAND_BUFFERS", "FVK_MAX_PIPELINE_AGE", "FVK_MAX_PIPELINE_COUNT"):
        found = re.findall(r"(?:constexpr static const int|static constexpr size_t) " + name + r" = [^;]+;",
                           paths[3].read_text())
        if len(found) != 1:
            raise ValueError("Actual GC budget declaration drift: " + name)
        constants.append(found[0])
    # Keep the retained age-sort field synchronized with actual use, including same-key binds.
    bind = actual_declaration(source, "void VulkanPipelineCache::bindPipeline(")
    expected = "mBoundPipelineEntry->lastUsed = mCurrentTime;"
    if bind.count(expected) != 1 or bind.count("mBoundPipelineEntry->lifetime.used(mCurrentTime,") != 1:
        raise ValueError("Actual bind-time age/fence registration changed; explicit review required")
    for changed in (source.replace("void VulkanPipelineCache::gc() noexcept", "void renamed() noexcept"),
                    source + gc):
        try:
            actual_declaration(changed, "void VulkanPipelineCache::gc() noexcept")
        except ValueError:
            pass
        else:
            raise ValueError("Source-extraction missing/duplicate control was not rejected")
    args.work_dir = args.work_dir.resolve()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    records = []
    expected_failures = {
        "missing-completion-guard": "pending ordinary/protected/error fence retired",
        "under-capacity-early-return": "under-capacity completed fences were not released",
        "reversed-eviction-order": "oldest eligible ordering or multiple-erasure key identity changed",
        "saved-invalidated-iterators": "oldest eligible ordering or multiple-erasure key identity changed",
    }
    msvc = Path(args.cxx).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    for name, (method, cases) in variants(gc).items():
        work = args.work_dir / name
        work.mkdir(exist_ok=True)
        generated = work / "actual-pipeline-gc.cpp"
        prefix = PREFIX.replace("// ACTUAL_GC_CONSTANTS", "\n".join(constants))
        generated.write_text(prefix + entry + MIDDLE + method + ORACLE)
        executable = work / ("contract.exe" if msvc else "contract")
        includes = [filament / "filament/backend/src", filament / "third_party/robin-map/include"]
        command = ([args.cxx, "/nologo", "/std:c++20", "/EHsc", *["/I" + str(path) for path in includes],
                    str(generated), "/Fe:" + str(executable), "/Fo:" + str(work / "contract.obj")]
                   if msvc else [args.cxx, "-std=c++20", *["-I" + str(path) for path in includes],
                                 str(generated), "-o", str(executable)])
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (work / "compile.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            raise ValueError(name + " did not compile; this is not a valid source control")
        for case in cases:
            ran = subprocess.run([str(executable), case], capture_output=True, text=True, timeout=15)
            output = ran.stdout + ran.stderr
            (work / (case + ".log")).write_text(output)
            expected_code = 0 if name == "production" else 1
            marker = "Pipeline GC PASS:" if name == "production" else "Pipeline GC FAIL:"
            diagnostic = case if name == "production" else expected_failures[name]
            if ran.returncode != expected_code or marker not in output or diagnostic not in output:
                raise ValueError(f"{name}/{case} failed its independent oracle: {ran.returncode}\n{output}")
            records.append({"variant": name, "case": case, "returncode": ran.returncode,
                            "expected_diagnostic": diagnostic, "passed": True})
    current = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
    if current != identities:
        raise ValueError("Production sources changed during contract execution")
    receipt = {"schema": 1, "passed": True, "source_files_sha256": identities,
               "gc_method_sha256": hashlib.sha256(gc.encode()).hexdigest(),
               "cache_entry_sha256": hashlib.sha256(entry.encode()).hexdigest(),
               "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               "compiler": args.cxx, "cases": records, "source_extraction_negatives": 2,
               "scope": "Verbatim actual GC method/cache entry, actual lifetime and real robin-map; fixture keys, fences and disabled persistence. No native Vulkan, GPU, cache-persistence or platform acceptance."}
    (args.work_dir / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("Pipeline GC contract PASS: seven actual-source cases, five runtime negative controls, two extraction negatives; no GPU execution")


if __name__ == "__main__":
    main()
