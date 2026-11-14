#include "GpuAccel.h"

#include "GpuRecord.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gpu {

namespace {

    struct AccelFns {
        PFN_vkCreateAccelerationStructureKHR create = nullptr;
        PFN_vkDestroyAccelerationStructureKHR destroy = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR buildSizes = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR address = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild = nullptr;
        PFN_vkCmdCopyAccelerationStructureKHR cmdCopy = nullptr;
        PFN_vkCmdWriteAccelerationStructuresPropertiesKHR cmdWriteProperties = nullptr;

        template <typename Fn>
        static bool get(VkDevice device, const char* name, Fn& out)
        {
            out = reinterpret_cast<Fn>(vkGetDeviceProcAddr(device, name));
            return out != nullptr;
        }

        bool load(VkDevice device)
        {
            return get(device, "vkCreateAccelerationStructureKHR", create)
                && get(device, "vkDestroyAccelerationStructureKHR", destroy)
                && get(device, "vkGetAccelerationStructureBuildSizesKHR", buildSizes)
                && get(device, "vkGetAccelerationStructureDeviceAddressKHR", address)
                && get(device, "vkCmdBuildAccelerationStructuresKHR", cmdBuild)
                && get(device, "vkCmdCopyAccelerationStructureKHR", cmdCopy)
                && get(device, "vkCmdWriteAccelerationStructuresPropertiesKHR", cmdWriteProperties);
        }
    };

    GpuStatus
    invalid(const char* where, const std::string& what)
    {
        return { VK_ERROR_INITIALIZATION_FAILED, std::string(where) + ": " + what };
    }

    GpuStatus
    ready(GpuDevice& dev, const char* where, AccelFns& fns)
    {
        if (dev.isLost()) {
            return { VK_ERROR_DEVICE_LOST, std::string(where) + ": device is lost" };
        }
        if (!dev.supportsRayQuery()) {
            return { VK_ERROR_FEATURE_NOT_PRESENT,
                     std::string(where) + ": " + dev.info().name
                         + " has no acceleration structure and ray query support" };
        }
        if (!fns.load(dev.device())) {
            return { VK_ERROR_EXTENSION_NOT_PRESENT,
                     std::string(where) + ": acceleration structure entry points are missing" };
        }
        return {};
    }

    VkPhysicalDeviceAccelerationStructurePropertiesKHR
    accelProperties(GpuDevice& dev)
    {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR accel {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR
        };
        VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props.pNext = &accel;
        vkGetPhysicalDeviceProperties2(dev.physicalDevice(), &props);
        return accel;
    }

    VkDeviceAddress
    bufferAddress(GpuDevice& dev, const GpuBuffer& buffer)
    {
        VkBufferDeviceAddressInfo info { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        info.buffer = buffer.buffer();
        return vkGetBufferDeviceAddress(dev.device(), &info);
    }

    VkBuildAccelerationStructureFlagsKHR
    blasFlags(const GpuBlasOptions& options)
    {
        VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        if (options.allowUpdate) {
            flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        }
        if (options.allowCompaction) {
            flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        }
        return flags;
    }

    // The geometry and build info point into this struct, so it is filled in place and never copied.
    struct BuildDesc {
        VkAccelerationStructureGeometryKHR geometry { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        VkAccelerationStructureBuildGeometryInfoKHR info {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
        };
        VkAccelerationStructureBuildRangeInfoKHR range {};

        BuildDesc() = default;
        BuildDesc(const BuildDesc&) = delete;
        BuildDesc& operator=(const BuildDesc&) = delete;
    };

    GpuStatus
    describeTriangles(GpuDevice& dev, const char* where, const GpuTriangles& t, const GpuBlasOptions& options,
                      BuildDesc& out)
    {
        if (!t.vertices || !*t.vertices || !t.indices || !*t.indices) {
            return invalid(where, "vertex and index buffers are required");
        }
        if (t.vertexCount == 0 || t.triangleCount == 0) {
            return invalid(where, "the geometry has no vertices or no triangles");
        }
        if (t.vertexOffset % 4 != 0 || t.indexOffset % 4 != 0 || t.vertexStride % 4 != 0
            || t.vertexStride < 3 * sizeof(float)) {
            return invalid(where, "vertex and index offsets and the vertex stride must be multiples of 4, the stride at "
                                  "least 12 bytes");
        }
        const VkDeviceSize vertexEnd = t.vertexOffset + (t.vertexCount - 1) * t.vertexStride + 3 * sizeof(float);
        const VkDeviceSize indexEnd = t.indexOffset + VkDeviceSize(t.triangleCount) * 3 * sizeof(uint32_t);
        if (vertexEnd > t.vertices->size() || indexEnd > t.indices->size()) {
            return invalid(where, "the geometry runs past the end of its buffers");
        }
        const VkPhysicalDeviceAccelerationStructurePropertiesKHR props = accelProperties(dev);
        if (t.triangleCount > props.maxPrimitiveCount) {
            return { VK_ERROR_FEATURE_NOT_PRESENT,
                     std::string(where) + ": " + std::to_string(t.triangleCount) + " triangles exceed the device's "
                         + std::to_string(props.maxPrimitiveCount) };
        }

        VkAccelerationStructureGeometryTrianglesDataKHR& tri = out.geometry.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = bufferAddress(dev, *t.vertices) + t.vertexOffset;
        tri.vertexStride = t.vertexStride;
        tri.maxVertex = t.vertexCount - 1;
        tri.indexType = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress = bufferAddress(dev, *t.indices) + t.indexOffset;
        out.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        out.geometry.flags = t.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;

        out.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        out.info.flags = blasFlags(options);
        out.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        out.info.geometryCount = 1;
        out.info.pGeometries = &out.geometry;
        out.range.primitiveCount = t.triangleCount;
        return {};
    }

    void
    describeInstances(VkDeviceAddress instances, uint32_t count, BuildDesc& out)
    {
        VkAccelerationStructureGeometryInstancesDataKHR& inst = out.geometry.geometry.instances;
        inst.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        inst.arrayOfPointers = VK_FALSE;
        inst.data.deviceAddress = instances;
        out.geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;

        out.info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        out.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        out.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        out.info.geometryCount = 1;
        out.info.pGeometries = &out.geometry;
        out.range.primitiveCount = count;
    }

    GpuAccelSizes
    buildSizes(GpuDevice& dev, const AccelFns& fns, const BuildDesc& desc)
    {
        VkAccelerationStructureBuildSizesInfoKHR sizes { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        const uint32_t primitives = desc.range.primitiveCount;
        fns.buildSizes(dev.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &desc.info, &primitives, &sizes);
        return { sizes.accelerationStructureSize, sizes.buildScratchSize, sizes.updateScratchSize };
    }

    void
    barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
    {
        VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        mb.srcStageMask = srcStage;
        mb.srcAccessMask = srcAccess;
        mb.dstStageMask = dstStage;
        mb.dstAccessMask = dstAccess;
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Inputs written by uploads, kernels or earlier builds, and structures that ray queries were still reading.
    void
    beforeBuild(VkCommandBuffer cmd)
    {
        barrier(cmd,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                    | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                    | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR
                    | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    }

    // Without VK_KHR_ray_tracing_maintenance1, copies and property writes run in the build stage too.
    void
    afterBuild(VkCommandBuffer cmd)
    {
        barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    }

    VkDeviceAddress
    alignUp(VkDeviceAddress value, VkDeviceSize alignment)
    {
        return alignment > 1 ? (value + alignment - 1) / alignment * alignment : value;
    }

} // namespace

class GpuAccelRecorder {
public:
    static VkCommandBuffer commandBuffer(GpuRecordContext& ctx) { return ctx.open_ ? ctx.cmd_ : VK_NULL_HANDLE; }

    static GpuStatus fail(GpuRecordContext& ctx, GpuStatus s)
    {
        if (s.result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            ctx.outOfMemory_ = true;
        }
        return s;
    }

    static GpuStatus open(GpuRecordContext& ctx, const char* where, AccelFns& fns, VkCommandBuffer& cmd)
    {
        if (GpuStatus s = ready(ctx.device(), where, fns); !s) {
            return s;
        }
        cmd = commandBuffer(ctx);
        if (cmd == VK_NULL_HANDLE) {
            return invalid(where, "context already submitted");
        }
        return {};
    }

    static GpuStatus createStructure(GpuDevice& dev, const AccelFns& fns, GpuBufferPool& pool,
                                     VkAccelerationStructureTypeKHR type, VkDeviceSize bytes, GpuAccelStructure& out)
    {
        out.reset();
        GpuBufferHandle storage;
        if (GpuStatus s = pool.acquire(bytes, storage); !s) {
            s.message = "acceleration structure of " + std::to_string(bytes) + " bytes: " + s.message;
            return s;
        }
        VkAccelerationStructureCreateInfoKHR ci { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        ci.buffer = storage.vkBuffer();
        ci.offset = 0;
        ci.size = bytes;
        ci.type = type;
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        if (GpuStatus s = dev.check(fns.create(dev.device(), &ci, nullptr, &handle), "vkCreateAccelerationStructureKHR");
            !s) {
            return s;
        }
        VkAccelerationStructureDeviceAddressInfoKHR ai { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        ai.accelerationStructure = handle;
        out.device_ = &dev;
        out.destroy_ = fns.destroy;
        out.handle_ = handle;
        out.storage_ = std::move(storage);
        out.address_ = fns.address(dev.device(), &ai);
        out.size_ = bytes;
        return {};
    }

    // Hands `structure` to the context so that work recorded before still finds it.
    static void retire(GpuRecordContext& ctx, GpuAccelStructure& structure)
    {
        if (structure) {
            ctx.hold(std::make_shared<GpuAccelStructure>(std::move(structure)));
        }
    }

    static GpuStatus recordBuild(GpuRecordContext& ctx, VkCommandBuffer cmd, const AccelFns& fns,
                                 BuildDesc& desc, VkDeviceSize scratchBytes)
    {
        GpuDevice& dev = ctx.device();
        const VkDeviceSize alignment
            = std::max<VkDeviceSize>(accelProperties(dev).minAccelerationStructureScratchOffsetAlignment, 1);
        const VkDeviceSize needed = std::max<VkDeviceSize>(scratchBytes, 1) + alignment - 1;
        GpuBuffer* scratch = ctx.accelScratch_;
        if (!scratch || scratch->size() < needed) {
            if (GpuStatus s = ctx.scratch(needed, scratch); !s) {
                s.message = "acceleration structure build scratch of " + std::to_string(scratchBytes) + " bytes: " + s.message;
                return s;
            }
            ctx.accelScratch_ = scratch;
        }
        desc.info.scratchData.deviceAddress = alignUp(bufferAddress(dev, *scratch), alignment);
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &desc.range;
        beforeBuild(cmd);
        fns.cmdBuild(cmd, 1, &desc.info, &ranges);
        afterBuild(cmd);
        return {};
    }

    // Hands `query` to the context, whose recorded work may still write it.
    static void retire(GpuRecordContext& ctx, detail::AccelQueryPool& query)
    {
        if (query.handle() != VK_NULL_HANDLE) {
            ctx.hold(std::make_shared<detail::AccelQueryPool>(std::move(query)));
        }
    }

    static void recordCompactedSizeQuery(VkCommandBuffer cmd, const AccelFns& fns, const GpuAccelStructure& structure,
                                         VkQueryPool pool)
    {
        vkCmdResetQueryPool(cmd, pool, 0, 1);
        const VkAccelerationStructureKHR handle = structure.handle();
        fns.cmdWriteProperties(cmd, 1, &handle, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, pool, 0);
    }
};

void
GpuAccelStructure::moveFrom(GpuAccelStructure& o) noexcept
{
    device_ = std::exchange(o.device_, nullptr);
    destroy_ = std::exchange(o.destroy_, nullptr);
    handle_ = std::exchange(o.handle_, VK_NULL_HANDLE);
    storage_ = std::move(o.storage_);
    address_ = std::exchange(o.address_, 0);
    size_ = std::exchange(o.size_, 0);
}

void
GpuAccelStructure::reset()
{
    if (handle_ != VK_NULL_HANDLE && device_ && destroy_ && !device_->hasAbandonedWork()) {
        destroy_(device_->device(), handle_, nullptr);
    }
    device_ = nullptr;
    destroy_ = nullptr;
    handle_ = VK_NULL_HANDLE;
    storage_ = GpuBufferHandle();
    address_ = 0;
    size_ = 0;
}

namespace detail {

    AccelQueryPool::AccelQueryPool(AccelQueryPool&& other) noexcept
        : device_(std::exchange(other.device_, nullptr))
        , pool_(std::exchange(other.pool_, VK_NULL_HANDLE))
    {
    }

    AccelQueryPool&
    AccelQueryPool::operator=(AccelQueryPool&& other) noexcept
    {
        if (this != &other) {
            reset();
            device_ = std::exchange(other.device_, nullptr);
            pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
        }
        return *this;
    }

    GpuStatus
    AccelQueryPool::create(GpuDevice& dev)
    {
        reset();
        VkQueryPoolCreateInfo ci { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        ci.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        ci.queryCount = 1;
        if (GpuStatus s = dev.check(vkCreateQueryPool(dev.device(), &ci, nullptr, &pool_), "vkCreateQueryPool"); !s) {
            pool_ = VK_NULL_HANDLE;
            return s;
        }
        device_ = &dev;
        return {};
    }

    void
    AccelQueryPool::reset()
    {
        if (pool_ != VK_NULL_HANDLE && device_ && !device_->hasAbandonedWork()) {
            vkDestroyQueryPool(device_->device(), pool_, nullptr);
        }
        device_ = nullptr;
        pool_ = VK_NULL_HANDLE;
    }

} // namespace detail

GpuStatus
GpuBlas::querySizes(GpuDevice& dev, const GpuTriangles& triangles, const GpuBlasOptions& options, GpuAccelSizes& out)
{
    out = {};
    AccelFns fns;
    if (GpuStatus s = ready(dev, "GpuBlas::querySizes", fns); !s) {
        return s;
    }
    BuildDesc desc;
    if (GpuStatus s = describeTriangles(dev, "GpuBlas::querySizes", triangles, options, desc); !s) {
        return s;
    }
    out = buildSizes(dev, fns, desc);
    return {};
}

GpuStatus
GpuBlas::build(GpuRecordContext& ctx, GpuBufferPool& pool, const GpuTriangles& triangles,
               const GpuBlasOptions& options, GpuBlas& out)
{
    const char* where = "GpuBlas::build";
    GpuAccelRecorder::retire(ctx, out.structure_);
    GpuAccelRecorder::retire(ctx, out.query_);
    out = GpuBlas();
    AccelFns fns;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (GpuStatus s = GpuAccelRecorder::open(ctx, where, fns, cmd); !s) {
        return s;
    }
    GpuDevice& dev = ctx.device();
    BuildDesc desc;
    if (GpuStatus s = describeTriangles(dev, where, triangles, options, desc); !s) {
        return s;
    }
    const GpuAccelSizes sizes = buildSizes(dev, fns, desc);

    GpuBlas blas;
    blas.options_ = options;
    blas.vertexCount_ = triangles.vertexCount;
    blas.triangleCount_ = triangles.triangleCount;
    blas.updateScratchBytes_ = sizes.updateScratchBytes;
    if (GpuStatus s = GpuAccelRecorder::createStructure(dev, fns, pool, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                        sizes.structureBytes, blas.structure_);
        !s) {
        return GpuAccelRecorder::fail(ctx, std::move(s));
    }
    if (options.allowCompaction) {
        if (GpuStatus s = blas.query_.create(dev); !s) {
            return s;
        }
    }
    desc.info.dstAccelerationStructure = blas.structure_.handle();
    if (GpuStatus s = GpuAccelRecorder::recordBuild(ctx, cmd, fns, desc, sizes.buildScratchBytes); !s) {
        return GpuAccelRecorder::fail(ctx, std::move(s));
    }
    if (options.allowCompaction) {
        GpuAccelRecorder::recordCompactedSizeQuery(cmd, fns, blas.structure_, blas.query_.handle());
        blas.sizeQueried_ = true;
    }
    out = std::move(blas);
    return {};
}

GpuStatus
GpuBlas::refit(GpuRecordContext& ctx, const GpuTriangles& triangles)
{
    const char* where = "GpuBlas::refit";
    AccelFns fns;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (GpuStatus s = GpuAccelRecorder::open(ctx, where, fns, cmd); !s) {
        return s;
    }
    if (!structure_) {
        return invalid(where, "the structure was never built");
    }
    if (!options_.allowUpdate) {
        return invalid(where, "the structure was built without allowUpdate");
    }
    if (triangles.vertexCount != vertexCount_ || triangles.triangleCount != triangleCount_) {
        return invalid(where, "the vertex or triangle count differs from the build's");
    }
    BuildDesc desc;
    if (GpuStatus s = describeTriangles(ctx.device(), where, triangles, options_, desc); !s) {
        return s;
    }
    desc.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
    desc.info.srcAccelerationStructure = structure_.handle();
    desc.info.dstAccelerationStructure = structure_.handle();
    if (GpuStatus s = GpuAccelRecorder::recordBuild(ctx, cmd, fns, desc, updateScratchBytes_); !s) {
        return GpuAccelRecorder::fail(ctx, std::move(s));
    }
    // An update counts as a build: the compacting copy's destination has to be as large as the size queried after
    // the source's latest build, and an update may change it, so the size the build queried no longer holds.
    if (options_.allowCompaction && query_.handle() != VK_NULL_HANDLE) {
        GpuAccelRecorder::recordCompactedSizeQuery(cmd, fns, structure_, query_.handle());
        sizeQueried_ = true;
    }
    return {};
}

GpuStatus
GpuBlas::compactedSize(VkDeviceSize& out) const
{
    out = 0;
    if (!structure_) {
        return invalid("GpuBlas::compactedSize", "the structure was never built");
    }
    if (!options_.allowCompaction) {
        return invalid("GpuBlas::compactedSize", "the structure was built without allowCompaction");
    }
    if (!sizeQueried_ || query_.handle() == VK_NULL_HANDLE) {
        return invalid("GpuBlas::compactedSize", "the structure was already compacted");
    }
    GpuDevice& dev = *structure_.device();
    uint64_t bytes = 0;
    const VkResult r = vkGetQueryPoolResults(dev.device(), query_.handle(), 0, 1, sizeof(bytes), &bytes,
                                             sizeof(bytes), VK_QUERY_RESULT_64_BIT);
    if (r == VK_NOT_READY) {
        return { VK_NOT_READY, "GpuBlas::compactedSize: the build has not finished" };
    }
    if (GpuStatus s = dev.check(r, "vkGetQueryPoolResults"); !s) {
        return s;
    }
    out = bytes;
    return {};
}

GpuStatus
GpuBlas::compact(GpuRecordContext& ctx, GpuBufferPool& pool)
{
    const char* where = "GpuBlas::compact";
    AccelFns fns;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (GpuStatus s = GpuAccelRecorder::open(ctx, where, fns, cmd); !s) {
        return s;
    }
    VkDeviceSize bytes = 0;
    if (GpuStatus s = compactedSize(bytes); !s) {
        return s;
    }
    if (bytes == 0 || bytes > structure_.size()) {
        return { VK_ERROR_UNKNOWN, std::string(where) + ": the driver reported a compacted size of " + std::to_string(bytes) + " for a structure of " + std::to_string(structure_.size()) + " bytes" };
    }
    GpuAccelStructure compacted;
    if (GpuStatus s = GpuAccelRecorder::createStructure(ctx.device(), fns, pool,
                                                        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, bytes,
                                                        compacted);
        !s) {
        return GpuAccelRecorder::fail(ctx, std::move(s));
    }
    VkCopyAccelerationStructureInfoKHR ci { VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
    ci.src = structure_.handle();
    ci.dst = compacted.handle();
    ci.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
    beforeBuild(cmd);
    fns.cmdCopy(cmd, &ci);
    afterBuild(cmd);
    GpuAccelRecorder::retire(ctx, structure_);
    structure_ = std::move(compacted);
    GpuAccelRecorder::retire(ctx, query_);
    sizeQueried_ = false;
    return {};
}

GpuStatus
GpuTlas::querySizes(GpuDevice& dev, uint32_t instanceCount, GpuAccelSizes& out)
{
    out = {};
    AccelFns fns;
    if (GpuStatus s = ready(dev, "GpuTlas::querySizes", fns); !s) {
        return s;
    }
    BuildDesc desc;
    describeInstances(0, instanceCount, desc);
    out = buildSizes(dev, fns, desc);
    return {};
}

namespace {

    GpuStatus
    buildTlas(GpuRecordContext& ctx, GpuBufferPool& pool, std::span<const GpuAccelInstance> instances,
              GpuAccelStructure& out, const char* where)
    {
        AccelFns fns;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (GpuStatus s = GpuAccelRecorder::open(ctx, where, fns, cmd); !s) {
            return s;
        }
        GpuDevice& dev = ctx.device();
        const VkPhysicalDeviceAccelerationStructurePropertiesKHR props = accelProperties(dev);
        if (instances.size() > props.maxInstanceCount) {
            return { VK_ERROR_FEATURE_NOT_PRESENT, std::string(where) + ": " + std::to_string(instances.size()) + " instances exceed the device's " + std::to_string(props.maxInstanceCount) };
        }
        constexpr uint32_t k24Bits = 0xFFFFFF;
        std::vector<VkAccelerationStructureInstanceKHR> packed(instances.size());
        for (size_t i = 0; i < instances.size(); ++i) {
            const GpuAccelInstance& in = instances[i];
            if (!in.blas || !*in.blas) {
                return invalid(where, "instance " + std::to_string(i) + " has no built bottom-level structure");
            }
            if (in.customIndex > k24Bits || in.sbtRecordOffset > k24Bits) {
                return invalid(where, "instance " + std::to_string(i) + " has a custom index or record offset over 24 bits");
            }
            VkAccelerationStructureInstanceKHR& p = packed[i];
            std::memcpy(p.transform.matrix, in.transform.data(), sizeof(p.transform.matrix));
            p.instanceCustomIndex = in.customIndex;
            p.mask = in.mask;
            p.instanceShaderBindingTableRecordOffset = in.sbtRecordOffset;
            p.flags = in.flags & 0xFF;
            p.accelerationStructureReference = in.blas->deviceAddress();
        }

        // A zero-instance build still needs a valid instance address.
        const VkDeviceSize instanceBytes
            = std::max<size_t>(packed.size(), 1) * sizeof(VkAccelerationStructureInstanceKHR);
        auto instanceBuffer = std::make_shared<GpuBuffer>();
        if (GpuStatus s = GpuBuffer::createStaging(dev, instanceBytes, kGpuAccelInputUsage, *instanceBuffer); !s) {
            s.message = std::string(where) + ": instance buffer: " + s.message;
            return GpuAccelRecorder::fail(ctx, std::move(s));
        }
        if (!packed.empty()) {
            std::memcpy(instanceBuffer->mapped(), packed.data(), packed.size() * sizeof(packed[0]));
        }

        BuildDesc desc;
        describeInstances(bufferAddress(dev, *instanceBuffer), static_cast<uint32_t>(packed.size()), desc);
        const GpuAccelSizes sizes = buildSizes(dev, fns, desc);
        GpuAccelStructure structure;
        if (GpuStatus s = GpuAccelRecorder::createStructure(dev, fns, pool, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                                                            sizes.structureBytes, structure);
            !s) {
            return GpuAccelRecorder::fail(ctx, std::move(s));
        }
        desc.info.dstAccelerationStructure = structure.handle();
        if (GpuStatus s = GpuAccelRecorder::recordBuild(ctx, cmd, fns, desc, sizes.buildScratchBytes); !s) {
            return GpuAccelRecorder::fail(ctx, std::move(s));
        }
        ctx.hold(std::move(instanceBuffer));
        out = std::move(structure);
        return {};
    }

} // namespace

GpuStatus
GpuTlas::build(GpuRecordContext& ctx, GpuBufferPool& pool, std::span<const GpuAccelInstance> instances, GpuTlas& out)
{
    GpuAccelRecorder::retire(ctx, out.structure_);
    out = GpuTlas();
    GpuAccelStructure structure;
    if (GpuStatus s = buildTlas(ctx, pool, instances, structure, "GpuTlas::build"); !s) {
        return s;
    }
    out.structure_ = std::move(structure);
    out.instanceCount_ = static_cast<uint32_t>(instances.size());
    return {};
}

GpuStatus
GpuTlas::rebuild(GpuRecordContext& ctx, GpuBufferPool& pool, std::span<const GpuAccelInstance> instances)
{
    GpuAccelStructure structure;
    if (GpuStatus s = buildTlas(ctx, pool, instances, structure, "GpuTlas::rebuild"); !s) {
        return s;
    }
    GpuAccelRecorder::retire(ctx, structure_);
    structure_ = std::move(structure);
    instanceCount_ = static_cast<uint32_t>(instances.size());
    return {};
}

} // namespace gpu
