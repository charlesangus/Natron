#pragma once

#include "GpuKernel.h"
#include "GpuShaderCompiler.h"

#include <cstddef>
#include <cstdint>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gpu {

struct GpuPipelineCacheOptions {
    // Directory of the on-disk entries. Empty selects $XDG_CACHE_HOME/Natron/gpu-pipelines,
    // falling back to ~/.cache/Natron/gpu-pipelines; if neither can be created the cache is memory-only.
    std::string directory;
    // Compiled kernels kept alive in memory; the least recently requested is dropped beyond this.
    size_t maxMemoryEntries = 64;
    // Total size of the entry files; the oldest are deleted beyond this.
    uint64_t maxDiskBytes = 64ull << 20;
};

// Compiled runtime shaders for one device, keyed on the source, stage, entry point, the glslang build
// and the physical device. A request is served from memory, else from disk (SPIR-V plus the driver's
// VkPipelineCache blob, so no GLSL compile and a warm pipeline build), else compiled.
//
// Thread-safe: concurrent requests for one shader run one compile or load and share the kernel. A disk
// entry that fails any check (magic, version, size, checksum, key, pipeline cache header) is discarded,
// recompiled and overwritten. Entries are written to a temporary file and renamed, so other processes
// never see a partial file.
//
// Returned kernels stay valid while the caller holds them, but must not outlive the device and must not
// be destroyed while recorded work still uses them.
class GpuPipelineCache {
public:
    explicit GpuPipelineCache(GpuDevice& device, GpuPipelineCacheOptions options = {});
    ~GpuPipelineCache();
    GpuPipelineCache(const GpuPipelineCache&) = delete;
    GpuPipelineCache& operator=(const GpuPipelineCache&) = delete;

    // On a compile failure the message is the head of the compiler log. Failures are not cached.
    GpuStatus get(std::string_view source, ShaderStage stage, std::string_view entry,
                  std::shared_ptr<GpuKernel>& out);

    // Drops every in-memory kernel; the disk entries stay.
    void clearMemory();

    struct Stats {
        size_t memoryHits = 0;
        size_t diskHits = 0;
        size_t compiles = 0;
        size_t diskWrites = 0;
        size_t invalidEntries = 0;
    };
    Stats stats() const;

    const std::string& directory() const { return directory_; }
    // Vendor, device and driver: what a compile result depends on besides the source.
    const std::string& deviceTag() const { return deviceTag_; }

private:
    struct Result {
        GpuStatus status;
        std::shared_ptr<GpuKernel> kernel;
    };
    struct Key {
        uint64_t hi = 0;
        uint64_t lo = 0;
    };
    struct Entry {
        std::shared_ptr<GpuKernel> kernel;
        std::list<std::string>::iterator lruPos;
    };

    Key makeKey(std::string_view source, ShaderStage stage, std::string_view entry) const;
    Result build(const Key& key, std::string_view source, ShaderStage stage, std::string_view entry);
    void storeMemory(const std::string& name, const std::shared_ptr<GpuKernel>& kernel);
    void evictDisk(const std::string& keep);

    GpuDevice& device_;
    GpuPipelineCacheOptions options_;
    std::string directory_;
    std::string deviceTag_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, std::shared_future<Result>> inflight_;
    Stats stats_;
};

} // namespace gpu
