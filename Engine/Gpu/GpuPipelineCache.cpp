#include "GpuPipelineCache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <unistd.h>

namespace gpu {

namespace {

    namespace fs = std::filesystem;

    constexpr uint32_t kFileMagic = 0x4350474E; // "NGPC"
    constexpr uint32_t kFileVersion = 1;
    constexpr const char* kFileSuffix = ".bin";
    constexpr const char* kTempMarker = ".tmp.";
    constexpr auto kStaleTempAge = std::chrono::hours(1);

    // Vulkan pipeline cache header, version one: length, version, vendorID, deviceID, pipelineCacheUUID.
    constexpr uint32_t kVkCacheHeaderBytes = 16 + VK_UUID_SIZE;

    struct FileHeader {
        uint32_t magic;
        uint32_t version;
        uint64_t keyHi;
        uint64_t keyLo;
        uint64_t spirvBytes;
        uint64_t cacheBytes;
        // Over the SPIR-V and cache bytes that follow.
        uint64_t checksum;
    };

    uint64_t fnv1a(const void* data, size_t n, uint64_t h = 0xcbf29ce484222325ull)
    {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 0x100000001b3ull;
        }
        return h;
    }

    std::string hex(const void* data, size_t n)
    {
        static const char* digits = "0123456789abcdef";
        const auto* p = static_cast<const unsigned char*>(data);
        std::string s;
        for (size_t i = 0; i < n; ++i) {
            s += digits[p[i] >> 4];
            s += digits[p[i] & 15];
        }
        return s;
    }

    std::string defaultDirectory()
    {
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        if (xdg && *xdg && xdg[0] == '/') {
            return (fs::path(xdg) / "Natron" / "gpu-pipelines").string();
        }
        const char* home = std::getenv("HOME");
        if (home && *home) {
            return (fs::path(home) / ".cache" / "Natron" / "gpu-pipelines").string();
        }
        return {};
    }

    bool readFile(const fs::path& path, std::vector<char>& out)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            return false;
        }
        const std::streamoff size = in.tellg();
        if (size < 0) {
            return false;
        }
        out.resize(static_cast<size_t>(size));
        in.seekg(0);
        in.read(out.data(), size);
        return static_cast<bool>(in);
    }

    // Writes next to the destination and renames, which is atomic within a directory.
    bool writeFileAtomic(const fs::path& path, const std::vector<char>& bytes)
    {
        static std::atomic<uint64_t> counter { 0 };
        const fs::path tmp = path.string() + kTempMarker + std::to_string(::getpid()) + "." + std::to_string(counter.fetch_add(1));
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.flush();
            if (!out) {
                std::error_code ec;
                fs::remove(tmp, ec);
                return false;
            }
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
        return true;
    }

    bool validPipelineCacheBlob(const std::vector<char>& blob, const VkPhysicalDeviceProperties& props)
    {
        if (blob.size() < kVkCacheHeaderBytes) {
            return false;
        }
        uint32_t length, version, vendor, deviceId;
        std::memcpy(&length, blob.data(), 4);
        std::memcpy(&version, blob.data() + 4, 4);
        std::memcpy(&vendor, blob.data() + 8, 4);
        std::memcpy(&deviceId, blob.data() + 12, 4);
        return length == kVkCacheHeaderBytes && length <= blob.size() && version == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
            && vendor == props.vendorID && deviceId == props.deviceID
            && std::memcmp(blob.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    }

} // namespace

GpuPipelineCache::GpuPipelineCache(GpuDevice& device, GpuPipelineCacheOptions options)
    : device_(device)
    , options_(std::move(options))
{
    VkPhysicalDeviceIDProperties ids { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 props2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props2.pNext = &ids;
    vkGetPhysicalDeviceProperties2(device_.physicalDevice(), &props2);
    const VkPhysicalDeviceProperties& p = props2.properties;
    deviceTag_ = std::to_string(p.vendorID) + ":" + std::to_string(p.deviceID) + ":" + std::to_string(p.driverVersion)
        + ":" + hex(p.pipelineCacheUUID, VK_UUID_SIZE) + ":" + hex(ids.deviceUUID, VK_UUID_SIZE);

    directory_ = options_.directory.empty() ? defaultDirectory() : options_.directory;
    if (!directory_.empty()) {
        std::error_code ec;
        fs::create_directories(directory_, ec);
        if (ec) {
            directory_.clear();
        }
    }
}

GpuPipelineCache::~GpuPipelineCache() = default;

GpuPipelineCache::Key
GpuPipelineCache::makeKey(std::string_view source, ShaderStage stage, std::string_view entry) const
{
    std::string material;
    material.reserve(source.size() + 256);
    material += "natron-gpu-pipeline/";
    material += std::to_string(kFileVersion);
    material += '\0';
    material += glslangVersionString();
    material += '\0';
    material += deviceTag_;
    material += '\0';
    material += static_cast<char>('0' + static_cast<int>(stage));
    material += entry;
    material += '\0';
    material += source;
    Key k;
    k.hi = fnv1a(material.data(), material.size());
    k.lo = fnv1a(material.data(), material.size(), 0x84222325cbf29ce4ull ^ material.size());
    return k;
}

GpuStatus
GpuPipelineCache::get(std::string_view source, ShaderStage stage, std::string_view entry,
                      std::shared_ptr<GpuKernel>& out)
{
    const Key key = makeKey(source, stage, entry);
    const std::string name = hex(&key.hi, 8) + hex(&key.lo, 8);

    std::shared_future<Result> future;
    std::promise<Result> promise;
    bool owner = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(name);
        if (it != entries_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.lruPos);
            ++stats_.memoryHits;
            out = it->second.kernel;
            return {};
        }
        auto fl = inflight_.find(name);
        if (fl != inflight_.end()) {
            future = fl->second;
        } else {
            future = promise.get_future().share();
            inflight_.emplace(name, future);
            owner = true;
        }
    }

    if (owner) {
        Result result;
        try {
            result = build(key, source, stage, entry);
        } catch (const std::exception& e) {
            result.status = GpuStatus { VK_ERROR_UNKNOWN, std::string("pipeline cache failed: ") + e.what() };
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inflight_.erase(name);
            if (result.status && result.kernel) {
                storeMemory(name, result.kernel);
            }
        }
        promise.set_value(std::move(result));
    }

    const Result& r = future.get();
    if (!r.status) {
        return r.status;
    }
    out = r.kernel;
    return {};
}

void
GpuPipelineCache::storeMemory(const std::string& name, const std::shared_ptr<GpuKernel>& kernel)
{
    lru_.push_front(name);
    entries_[name] = Entry { kernel, lru_.begin() };
    while (entries_.size() > std::max<size_t>(options_.maxMemoryEntries, 1)) {
        entries_.erase(lru_.back());
        lru_.pop_back();
    }
}

void
GpuPipelineCache::clearMemory()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    lru_.clear();
}

GpuPipelineCache::Stats
GpuPipelineCache::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

GpuPipelineCache::Result
GpuPipelineCache::build(const Key& key, std::string_view source, ShaderStage stage, std::string_view entry)
{
    Result result;
    const std::string name = hex(&key.hi, 8) + hex(&key.lo, 8);
    const std::string entryName(entry);
    const fs::path file = directory_.empty() ? fs::path() : fs::path(directory_) / (name + kFileSuffix);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device_.physicalDevice(), &props);

    std::vector<uint32_t> spirv;
    std::vector<char> blob;
    bool fromDisk = false;
    bool sawInvalid = false;

    if (!file.empty()) {
        std::vector<char> bytes;
        if (readFile(file, bytes)) {
            FileHeader h;
            bool ok = bytes.size() >= sizeof h;
            if (ok) {
                std::memcpy(&h, bytes.data(), sizeof h);
                ok = h.magic == kFileMagic && h.version == kFileVersion && h.keyHi == key.hi && h.keyLo == key.lo
                    && h.spirvBytes % 4 == 0 && h.spirvBytes > 0 && h.spirvBytes <= bytes.size()
                    && h.cacheBytes <= bytes.size() && sizeof h + h.spirvBytes + h.cacheBytes == bytes.size();
            }
            if (ok) {
                const char* payload = bytes.data() + sizeof h;
                ok = fnv1a(payload, h.spirvBytes + h.cacheBytes) == h.checksum;
            }
            if (ok) {
                spirv.resize(h.spirvBytes / 4);
                std::memcpy(spirv.data(), bytes.data() + sizeof h, h.spirvBytes);
                blob.assign(bytes.begin() + sizeof h + h.spirvBytes, bytes.end());
                ok = spirv[0] == 0x07230203 && (blob.empty() || validPipelineCacheBlob(blob, props));
            }
            if (ok) {
                fromDisk = true;
            } else {
                sawInvalid = true;
                spirv.clear();
                blob.clear();
            }
        }
    }

    if (!fromDisk) {
        auto compiled = compileGlsl(source, stage, entry);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.compiles;
            if (sawInvalid) {
                ++stats_.invalidEntries;
            }
        }
        if (!compiled->ok) {
            result.status = GpuStatus { VK_ERROR_INITIALIZATION_FAILED, "shader compile failed:\n" + compiled->logHead() };
            return result;
        }
        spirv = compiled->spirv;
    }

    VkPipelineCache vkCache = VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo ci { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.empty() ? nullptr : blob.data();
    if (vkCreatePipelineCache(device_.device(), &ci, nullptr, &vkCache) != VK_SUCCESS) {
        vkCache = VK_NULL_HANDLE;
    }

    std::unique_ptr<GpuKernel> kernel;
    result.status = GpuKernel::createFromSpirv(device_, spirv, entryName.c_str(), kernel, vkCache);

    std::vector<char> fresh;
    if (result.status && vkCache != VK_NULL_HANDLE && (!fromDisk || blob.empty())) {
        size_t size = 0;
        if (vkGetPipelineCacheData(device_.device(), vkCache, &size, nullptr) == VK_SUCCESS && size > 0) {
            fresh.resize(size);
            if (vkGetPipelineCacheData(device_.device(), vkCache, &size, fresh.data()) != VK_SUCCESS) {
                fresh.clear();
            } else {
                fresh.resize(size);
                if (!validPipelineCacheBlob(fresh, props)) {
                    fresh.clear();
                }
            }
        }
    }
    if (vkCache != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_.device(), vkCache, nullptr);
    }
    if (!result.status) {
        return result;
    }
    result.kernel = std::move(kernel);

    if (file.empty()) {
        if (fromDisk) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.diskHits;
        }
        return result;
    }

    if (fromDisk && fresh.empty()) {
        std::error_code ec;
        fs::last_write_time(file, fs::file_time_type::clock::now(), ec);
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.diskHits;
        return result;
    }

    // A disk entry without a driver blob is upgraded once the driver produced one.
    const std::vector<char>& cacheBytes = fresh.empty() ? blob : fresh;
    FileHeader h {};
    h.magic = kFileMagic;
    h.version = kFileVersion;
    h.keyHi = key.hi;
    h.keyLo = key.lo;
    h.spirvBytes = spirv.size() * 4;
    h.cacheBytes = cacheBytes.size();
    std::vector<char> bytes(sizeof h + h.spirvBytes + h.cacheBytes);
    std::memcpy(bytes.data() + sizeof h, spirv.data(), h.spirvBytes);
    if (!cacheBytes.empty()) {
        std::memcpy(bytes.data() + sizeof h + h.spirvBytes, cacheBytes.data(), cacheBytes.size());
    }
    h.checksum = fnv1a(bytes.data() + sizeof h, h.spirvBytes + h.cacheBytes);
    std::memcpy(bytes.data(), &h, sizeof h);

    const bool wrote = writeFileAtomic(file, bytes);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fromDisk) {
            ++stats_.diskHits;
        }
        if (wrote) {
            ++stats_.diskWrites;
        }
    }
    if (wrote) {
        evictDisk(name + kFileSuffix);
    }
    return result;
}

void
GpuPipelineCache::evictDisk(const std::string& keep)
{
    struct Item {
        fs::path path;
        uint64_t size;
        fs::file_time_type time;
    };
    std::vector<Item> items;
    uint64_t total = 0;
    const auto now = fs::file_time_type::clock::now();
    std::error_code ec;
    for (fs::directory_iterator it(directory_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (!it->is_regular_file(e)) {
            continue;
        }
        const std::string fname = it->path().filename().string();
        const auto time = it->last_write_time(e);
        const uint64_t size = it->file_size(e);
        if (e) {
            continue;
        }
        if (fname.find(kTempMarker) != std::string::npos) {
            if (now - time > kStaleTempAge) {
                fs::remove(it->path(), e);
            }
            continue;
        }
        if (fname.size() > std::strlen(kFileSuffix) && fname.compare(fname.size() - std::strlen(kFileSuffix), std::string::npos, kFileSuffix) == 0) {
            items.push_back({ it->path(), size, time });
            total += size;
        }
    }
    if (total <= options_.maxDiskBytes) {
        return;
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.time < b.time; });
    for (const Item& item : items) {
        if (total <= options_.maxDiskBytes) {
            break;
        }
        if (item.path.filename() == keep) {
            continue;
        }
        std::error_code e;
        fs::remove(item.path, e);
        total -= std::min(total, item.size);
    }
}

} // namespace gpu
