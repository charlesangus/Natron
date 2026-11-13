#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>

#include "GpuBuffer.h"
#include "GpuBufferPool.h"
#include "GpuDevice.h"

namespace gpu {

class GpuRecordContext;

// Acceleration structures for ray queries.
//
// Every call fails with VK_ERROR_FEATURE_NOT_PRESENT on a device without GpuDevice::supportsRayQuery(). Sizes are
// queried before anything is allocated; a structure or scratch buffer the pool cannot give within its budget fails
// with VK_ERROR_OUT_OF_DEVICE_MEMORY and marks the context ranOutOfMemory().
//
// Builds are recorded into a GpuRecordContext, after a barrier that makes earlier transfer, compute and build writes
// visible to the build, and before one that makes the result visible to later builds, copies and compute ray queries.
// The scratch comes from the context's pool, so the context needs one. The input buffers and the structures a build
// reads or writes must outlive the context's work; a structure a call replaces is held by the context instead.

struct GpuAccelSizes {
    VkDeviceSize structureBytes = 0;
    VkDeviceSize buildScratchBytes = 0;
    VkDeviceSize updateScratchBytes = 0;
};

// Indexed triangles: float3 positions and uint32 indices, three per triangle. Both buffers need kGpuAccelInputUsage,
// which GpuBufferPool buffers have on a device that supports ray queries. Offsets are multiples of 4.
struct GpuTriangles {
    const GpuBuffer* vertices = nullptr;
    VkDeviceSize vertexOffset = 0;
    VkDeviceSize vertexStride = 3 * sizeof(float);
    uint32_t vertexCount = 0;
    const GpuBuffer* indices = nullptr;
    VkDeviceSize indexOffset = 0;
    uint32_t triangleCount = 0;
    bool opaque = true;
};

struct GpuBlasOptions {
    // Lets refit() update the structure in place when only the points moved.
    bool allowUpdate = true;
    // Lets compact() copy the structure into a smaller one.
    bool allowCompaction = true;
};

// Move-only owner of a VkAccelerationStructureKHR and the pooled buffer backing it. It must be destroyed before the
// pool and the device it came from.
class GpuAccelStructure {
public:
    GpuAccelStructure() = default;
    ~GpuAccelStructure() { reset(); }
    GpuAccelStructure(GpuAccelStructure&& other) noexcept { moveFrom(other); }
    GpuAccelStructure& operator=(GpuAccelStructure&& other) noexcept
    {
        if (this != &other) {
            reset();
            moveFrom(other);
        }
        return *this;
    }
    GpuAccelStructure(const GpuAccelStructure&) = delete;
    GpuAccelStructure& operator=(const GpuAccelStructure&) = delete;

    VkAccelerationStructureKHR handle() const { return handle_; }
    VkDeviceAddress deviceAddress() const { return address_; }
    // The structure's own size, which the backing buffer may exceed.
    VkDeviceSize size() const { return size_; }
    explicit operator bool() const { return handle_ != VK_NULL_HANDLE; }
    GpuDevice* device() const { return device_; }

    void reset();

private:
    friend class GpuAccelRecorder;

    void moveFrom(GpuAccelStructure& other) noexcept;

    GpuDevice* device_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy_ = nullptr;
    VkAccelerationStructureKHR handle_ = VK_NULL_HANDLE;
    GpuBufferHandle storage_;
    VkDeviceAddress address_ = 0;
    VkDeviceSize size_ = 0;
};

namespace detail {
    class AccelQueryPool {
    public:
        AccelQueryPool() = default;
        ~AccelQueryPool() { reset(); }
        AccelQueryPool(AccelQueryPool&& other) noexcept;
        AccelQueryPool& operator=(AccelQueryPool&& other) noexcept;
        AccelQueryPool(const AccelQueryPool&) = delete;
        AccelQueryPool& operator=(const AccelQueryPool&) = delete;

        GpuStatus create(GpuDevice& dev);
        void reset();
        VkQueryPool handle() const { return pool_; }

    private:
        GpuDevice* device_ = nullptr;
        VkQueryPool pool_ = VK_NULL_HANDLE;
    };
} // namespace detail

// A bottom-level structure over one triangle geometry. Move-only; empty until build() succeeds.
class GpuBlas {
public:
    GpuBlas() = default;
    GpuBlas(GpuBlas&&) noexcept = default;
    GpuBlas& operator=(GpuBlas&&) noexcept = default;
    GpuBlas(const GpuBlas&) = delete;
    GpuBlas& operator=(const GpuBlas&) = delete;

    static GpuStatus querySizes(GpuDevice& dev, const GpuTriangles& triangles, const GpuBlasOptions& options,
                                GpuAccelSizes& out);

    // Allocates the structure from `pool` and records its build. A structure `out` already held is handed to the
    // context. `out` is left empty on failure.
    static GpuStatus build(GpuRecordContext& ctx, GpuBufferPool& pool, const GpuTriangles& triangles,
                           const GpuBlasOptions& options, GpuBlas& out);

    // Records an in-place update from moved points: same buffers layout, vertex and triangle counts as the build.
    // Needs allowUpdate.
    GpuStatus refit(GpuRecordContext& ctx, const GpuTriangles& triangles);

    // The size compact() would shrink the structure to, once the work of the context that last built or refit it is
    // done. VK_NOT_READY before then. Needs allowCompaction.
    GpuStatus compactedSize(VkDeviceSize& out) const;

    // Records a copy into a structure of compactedSize() from `pool` and hands the original to the context, which
    // frees it once destroyed. The device address changes, so top-level structures over this one need rebuilding.
    GpuStatus compact(GpuRecordContext& ctx, GpuBufferPool& pool);

    const GpuAccelStructure& structure() const { return structure_; }
    VkDeviceAddress deviceAddress() const { return structure_.deviceAddress(); }
    VkDeviceSize size() const { return structure_.size(); }
    explicit operator bool() const { return static_cast<bool>(structure_); }

private:
    GpuAccelStructure structure_;
    detail::AccelQueryPool query_;
    GpuBlasOptions options_;
    uint32_t vertexCount_ = 0;
    uint32_t triangleCount_ = 0;
    VkDeviceSize updateScratchBytes_ = 0;
    bool sizeQueried_ = false;
};

// One placement of a bottom-level structure in a top-level one.
struct GpuAccelInstance {
    // Row-major 3x4 object-to-world transform.
    std::array<float, 12> transform { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    // At most 24 bits; what a ray query reports as the instance's custom index.
    uint32_t customIndex = 0;
    uint8_t mask = 0xFF;
    // At most 24 bits.
    uint32_t sbtRecordOffset = 0;
    VkGeometryInstanceFlagsKHR flags = 0;
    const GpuBlas* blas = nullptr;
};

// A top-level structure over instances of bottom-level ones, rebuilt whole whenever they change. Move-only; empty
// until build() succeeds. The bottom-level structures must outlive the context's work and every use of this one.
class GpuTlas {
public:
    GpuTlas() = default;
    GpuTlas(GpuTlas&&) noexcept = default;
    GpuTlas& operator=(GpuTlas&&) noexcept = default;
    GpuTlas(const GpuTlas&) = delete;
    GpuTlas& operator=(const GpuTlas&) = delete;

    static GpuStatus querySizes(GpuDevice& dev, uint32_t instanceCount, GpuAccelSizes& out);

    // Writes the instances into a host-visible buffer the context holds, allocates the structure from `pool` and
    // records its build. A structure `out` already held is handed to the context. `out` is left empty on failure.
    static GpuStatus build(GpuRecordContext& ctx, GpuBufferPool& pool, std::span<const GpuAccelInstance> instances,
                           GpuTlas& out);

    // As build() into a new structure, handing the current one to the context; on failure the current one stays.
    GpuStatus rebuild(GpuRecordContext& ctx, GpuBufferPool& pool, std::span<const GpuAccelInstance> instances);

    const GpuAccelStructure& structure() const { return structure_; }
    VkAccelerationStructureKHR handle() const { return structure_.handle(); }
    VkDeviceAddress deviceAddress() const { return structure_.deviceAddress(); }
    VkDeviceSize size() const { return structure_.size(); }
    uint32_t instanceCount() const { return instanceCount_; }
    explicit operator bool() const { return static_cast<bool>(structure_); }

private:
    GpuAccelStructure structure_;
    uint32_t instanceCount_ = 0;
};

} // namespace gpu
