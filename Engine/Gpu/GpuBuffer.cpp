#include "GpuBuffer.h"

#include <algorithm>
#include <string>
#include <utility>

namespace gpu {

namespace {

    constexpr VkExternalMemoryHandleTypeFlagBits kExportHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    constexpr uint64_t k4GiB = 1ull << 32;

    GpuStatus reject(VkResult r, std::string msg, bool fallback = false)
    {
        GpuStatus s { r, std::move(msg) };
        s.fallbackToStaging = fallback;
        return s;
    }

    GpuStatus checkSize(VkDeviceSize size)
    {
        if (size == 0) {
            return reject(VK_ERROR_INITIALIZATION_FAILED, "GpuBuffer size is zero");
        }
        return {};
    }

    VkBufferCreateInfo bufferInfo(VkDeviceSize size, VkBufferUsageFlags usage, const void* next = nullptr)
    {
        VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.pNext = next;
        bci.size = size;
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        return bci;
    }

} // namespace

void
GpuBuffer::moveFrom(GpuBuffer& o) noexcept
{
    device_ = std::exchange(o.device_, nullptr);
    buffer_ = std::exchange(o.buffer_, VK_NULL_HANDLE);
    allocation_ = std::exchange(o.allocation_, nullptr);
    pool_ = std::exchange(o.pool_, nullptr);
    importedMemory_ = std::exchange(o.importedMemory_, VK_NULL_HANDLE);
    memory_ = std::exchange(o.memory_, VK_NULL_HANDLE);
    memoryOffset_ = std::exchange(o.memoryOffset_, 0);
    size_ = std::exchange(o.size_, 0);
    kind_ = o.kind_;
    mapped_ = std::exchange(o.mapped_, nullptr);
    exportInfo_ = std::move(o.exportInfo_);
}

void
GpuBuffer::reset()
{
    if (device_ && !device_->hasAbandonedWork()) {
        if (allocation_) {
            vmaDestroyBuffer(device_->allocator(), buffer_, allocation_);
        } else if (buffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_->device(), buffer_, nullptr);
        }
        if (importedMemory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device_->device(), importedMemory_, nullptr);
        }
        if (pool_) {
            vmaDestroyPool(device_->allocator(), pool_);
        }
    }
    device_ = nullptr;
    buffer_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
    pool_ = nullptr;
    importedMemory_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    memoryOffset_ = 0;
    size_ = 0;
    mapped_ = nullptr;
    exportInfo_.reset();
}

GpuStatus
GpuBuffer::createDeviceLocal(GpuDevice& dev, VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& out)
{
    out.reset();
    if (GpuStatus s = checkSize(size); !s) {
        return s;
    }
    if (dev.isLost()) {
        return reject(VK_ERROR_DEVICE_LOST, "device is lost");
    }
    const VkBufferCreateInfo bci = bufferInfo(size, usage);
    VmaAllocationCreateInfo aci {};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    GpuBuffer b;
    VmaAllocationInfo ai {};
    if (GpuStatus s = dev.check(vmaCreateBuffer(dev.allocator(), &bci, &aci, &b.buffer_, &b.allocation_, &ai),
                                "vmaCreateBuffer");
        !s) {
        return s;
    }
    b.device_ = &dev;
    b.kind_ = GpuBufferKind::DeviceLocal;
    b.size_ = size;
    b.memory_ = ai.deviceMemory;
    b.memoryOffset_ = ai.offset;
    out = std::move(b);
    return {};
}

GpuStatus
GpuBuffer::createStaging(GpuDevice& dev, VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& out)
{
    out.reset();
    if (GpuStatus s = checkSize(size); !s) {
        return s;
    }
    if (dev.isLost()) {
        return reject(VK_ERROR_DEVICE_LOST, "device is lost");
    }
    const VkBufferCreateInfo bci = bufferInfo(size, usage);
    VmaAllocationCreateInfo aci {};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    GpuBuffer b;
    VmaAllocationInfo ai {};
    if (GpuStatus s = dev.check(vmaCreateBuffer(dev.allocator(), &bci, &aci, &b.buffer_, &b.allocation_, &ai),
                                "vmaCreateBuffer");
        !s) {
        return s;
    }
    b.device_ = &dev;
    b.kind_ = GpuBufferKind::Staging;
    b.size_ = size;
    b.mapped_ = ai.pMappedData;
    b.memory_ = ai.deviceMemory;
    b.memoryOffset_ = ai.offset;
    if (!b.mapped_) {
        return reject(VK_ERROR_MEMORY_MAP_FAILED, "staging buffer is not host-mappable");
    }
    out = std::move(b);
    return {};
}

GpuStatus
GpuBuffer::createExportable(GpuDevice& dev, VkDeviceSize size, VkBufferUsageFlags usage, GpuBuffer& out)
{
    out.reset();
    if (GpuStatus s = checkSize(size); !s) {
        return s;
    }
    if (dev.isLost()) {
        return reject(VK_ERROR_DEVICE_LOST, "device is lost");
    }
    if (!dev.info().externalMemoryFd) {
        return reject(VK_ERROR_EXTENSION_NOT_PRESENT, "VK_KHR_external_memory_fd is not available");
    }

    VkPhysicalDeviceExternalBufferInfo qi { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
    qi.usage = usage;
    qi.handleType = kExportHandle;
    VkExternalBufferProperties qp { VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    vkGetPhysicalDeviceExternalBufferProperties(dev.physicalDevice(), &qi, &qp);
    if (!(qp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        return reject(VK_ERROR_FORMAT_NOT_SUPPORTED, "OPAQUE_FD buffer export is not supported");
    }

    VkExternalMemoryBufferCreateInfo ext { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    ext.handleTypes = kExportHandle;
    const VkBufferCreateInfo bci = bufferInfo(size, usage, &ext);
    VmaAllocationCreateInfo aci {};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    uint32_t typeIndex = 0;
    if (GpuStatus s = dev.check(vmaFindMemoryTypeIndexForBufferInfo(dev.allocator(), &bci, &aci, &typeIndex),
                                "vmaFindMemoryTypeIndexForBufferInfo");
        !s) {
        return s;
    }

    GpuBuffer b;
    b.exportInfo_ = std::make_unique<VkExportMemoryAllocateInfo>();
    b.exportInfo_->sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    b.exportInfo_->handleTypes = kExportHandle;
    VmaPoolCreateInfo pci {};
    pci.memoryTypeIndex = typeIndex;
    pci.pMemoryAllocateNext = b.exportInfo_.get();
    if (GpuStatus s = dev.check(vmaCreatePool(dev.allocator(), &pci, &b.pool_), "vmaCreatePool"); !s) {
        return s;
    }
    b.device_ = &dev;
    aci.pool = b.pool_;
    // An importer takes the whole VkDeviceMemory, so it must hold this buffer alone at offset 0.
    aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo ai {};
    if (GpuStatus s = dev.check(vmaCreateBuffer(dev.allocator(), &bci, &aci, &b.buffer_, &b.allocation_, &ai),
                                "vmaCreateBuffer");
        !s) {
        return s;
    }
    b.kind_ = GpuBufferKind::Exportable;
    b.size_ = size;
    b.memory_ = ai.deviceMemory;
    b.memoryOffset_ = ai.offset;
    out = std::move(b);
    return {};
}

GpuStatus
GpuBuffer::createHostImported(GpuDevice& dev, void* hostPtr, VkDeviceSize size, VkBufferUsageFlags usage,
                              GpuBuffer& out)
{
    out.reset();
    if (dev.isLost()) {
        return reject(VK_ERROR_DEVICE_LOST, "device is lost");
    }
    const GpuDeviceInfo& info = dev.info();
    if (!info.externalMemoryHost) {
        return reject(VK_ERROR_EXTENSION_NOT_PRESENT, "VK_EXT_external_memory_host is not available", true);
    }
    auto getProps = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        vkGetDeviceProcAddr(dev.device(), "vkGetMemoryHostPointerPropertiesEXT"));
    if (!getProps) {
        return reject(VK_ERROR_EXTENSION_NOT_PRESENT, "vkGetMemoryHostPointerPropertiesEXT unavailable", true);
    }

    const uint64_t align = std::max<uint64_t>(info.minImportedHostPointerAlignment, 1);
    if (!hostPtr || reinterpret_cast<uintptr_t>(hostPtr) % align != 0) {
        return reject(VK_ERROR_INITIALIZATION_FAILED,
                      "host pointer is not aligned to minImportedHostPointerAlignment", true);
    }
    if (size == 0 || size % align != 0) {
        return reject(VK_ERROR_INITIALIZATION_FAILED,
                      "host import size is not a multiple of minImportedHostPointerAlignment", true);
    }
    if (info.maxMemoryAllocationSize != 0 && size > info.maxMemoryAllocationSize) {
        return reject(VK_ERROR_INITIALIZATION_FAILED, "host import size exceeds maxMemoryAllocationSize", true);
    }
    // RADV truncates the allocation to its low 32 bits, which would map zero bytes.
    if (size % k4GiB == 0) {
        return reject(VK_ERROR_INITIALIZATION_FAILED, "host import size is an exact multiple of 4 GiB", true);
    }

    VkDevice vk = dev.device();
    VkMemoryHostPointerPropertiesEXT hp { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    if (VkResult r = getProps(vk, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, hostPtr, &hp);
        r != VK_SUCCESS) {
        return reject(r, "vkGetMemoryHostPointerPropertiesEXT failed", r == VK_ERROR_INVALID_EXTERNAL_HANDLE);
    }

    VkExternalMemoryBufferCreateInfo ext { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    const VkBufferCreateInfo bci = bufferInfo(size, usage, &ext);
    GpuBuffer b;
    if (GpuStatus s = dev.check(vkCreateBuffer(vk, &bci, nullptr, &b.buffer_), "vkCreateBuffer"); !s) {
        return s;
    }
    b.device_ = &dev;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk, b.buffer_, &req);
    VkPhysicalDeviceMemoryProperties mem {};
    vkGetPhysicalDeviceMemoryProperties(dev.physicalDevice(), &mem);
    uint32_t type = UINT32_MAX;
    // Imported memory cannot be flushed or invalidated here, so it must be coherent.
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if (((req.memoryTypeBits & hp.memoryTypeBits) & (1u << i))
            && (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX || req.size > size) {
        return reject(VK_ERROR_INCOMPATIBLE_DRIVER, "no compatible memory type for the host import", true);
    }

    VkImportMemoryHostPointerInfoEXT imp { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = hostPtr;
    VkMemoryAllocateInfo mai { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &imp;
    mai.allocationSize = size;
    mai.memoryTypeIndex = type;
    if (VkResult r = vkAllocateMemory(vk, &mai, nullptr, &b.importedMemory_); r != VK_SUCCESS) {
        if (r == VK_ERROR_INVALID_EXTERNAL_HANDLE) {
            return reject(r, "driver refused the host pointer import", true);
        }
        return dev.check(r, "vkAllocateMemory");
    }
    if (GpuStatus s = dev.check(vkBindBufferMemory(vk, b.buffer_, b.importedMemory_, 0), "vkBindBufferMemory"); !s) {
        return s;
    }
    b.kind_ = GpuBufferKind::HostImported;
    b.size_ = size;
    b.mapped_ = hostPtr;
    b.memory_ = b.importedMemory_;
    out = std::move(b);
    return {};
}

} // namespace gpu
