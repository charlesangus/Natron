#include "GlInterop.h"

#include <GL/gl.h>
#include <GL/glext.h>

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
#include <string_view>

namespace gpu {

struct GlInterop::Gl {
    decltype(&::glGetError) GetError = nullptr;
    decltype(&::glGetString) GetString = nullptr;
    decltype(&::glGetIntegerv) GetIntegerv = nullptr;
    decltype(&::glBindTexture) BindTexture = nullptr;
    decltype(&::glTexSubImage2D) TexSubImage2D = nullptr;
    decltype(&::glPixelStorei) PixelStorei = nullptr;
    decltype(&::glFlush) Flush = nullptr;
    decltype(&::glFinish) Finish = nullptr;
    PFNGLGETSTRINGIPROC GetStringi = nullptr;
    PFNGLGENBUFFERSPROC GenBuffers = nullptr;
    PFNGLDELETEBUFFERSPROC DeleteBuffers = nullptr;
    PFNGLBINDBUFFERPROC BindBuffer = nullptr;

    PFNGLGETUNSIGNEDBYTEVEXTPROC GetUnsignedBytevEXT = nullptr;
    PFNGLGETUNSIGNEDBYTEI_VEXTPROC GetUnsignedBytei_vEXT = nullptr;
    PFNGLCREATEMEMORYOBJECTSEXTPROC CreateMemoryObjectsEXT = nullptr;
    PFNGLDELETEMEMORYOBJECTSEXTPROC DeleteMemoryObjectsEXT = nullptr;
    PFNGLMEMORYOBJECTPARAMETERIVEXTPROC MemoryObjectParameterivEXT = nullptr;
    PFNGLIMPORTMEMORYFDEXTPROC ImportMemoryFdEXT = nullptr;
    PFNGLBUFFERSTORAGEMEMEXTPROC BufferStorageMemEXT = nullptr;
    PFNGLGENSEMAPHORESEXTPROC GenSemaphoresEXT = nullptr;
    PFNGLDELETESEMAPHORESEXTPROC DeleteSemaphoresEXT = nullptr;
    PFNGLIMPORTSEMAPHOREFDEXTPROC ImportSemaphoreFdEXT = nullptr;
    PFNGLWAITSEMAPHOREEXTPROC WaitSemaphoreEXT = nullptr;
    PFNGLSIGNALSEMAPHOREEXTPROC SignalSemaphoreEXT = nullptr;

    std::set<std::string, std::less<>> extensions;

    bool has(std::string_view name) const { return extensions.find(name) != extensions.end(); }

    bool load(const GlProcLoader& loader)
    {
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(loader(name));
        };
        get(GetError, "glGetError");
        get(GetString, "glGetString");
        get(GetIntegerv, "glGetIntegerv");
        get(BindTexture, "glBindTexture");
        get(TexSubImage2D, "glTexSubImage2D");
        get(PixelStorei, "glPixelStorei");
        get(Flush, "glFlush");
        get(Finish, "glFinish");
        get(GetStringi, "glGetStringi");
        get(GenBuffers, "glGenBuffers");
        get(DeleteBuffers, "glDeleteBuffers");
        get(BindBuffer, "glBindBuffer");
        get(GetUnsignedBytevEXT, "glGetUnsignedBytevEXT");
        get(GetUnsignedBytei_vEXT, "glGetUnsignedBytei_vEXT");
        get(CreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT");
        get(DeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT");
        get(MemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT");
        get(ImportMemoryFdEXT, "glImportMemoryFdEXT");
        get(BufferStorageMemEXT, "glBufferStorageMemEXT");
        get(GenSemaphoresEXT, "glGenSemaphoresEXT");
        get(DeleteSemaphoresEXT, "glDeleteSemaphoresEXT");
        get(ImportSemaphoreFdEXT, "glImportSemaphoreFdEXT");
        get(WaitSemaphoreEXT, "glWaitSemaphoreEXT");
        get(SignalSemaphoreEXT, "glSignalSemaphoreEXT");

        if (!GetError || !GetString || !GetIntegerv || !BindTexture || !TexSubImage2D || !PixelStorei
            || !Flush || !Finish || !GenBuffers || !DeleteBuffers || !BindBuffer) {
            return false;
        }

        extensions.clear();
        GLint n = 0;
        GetIntegerv(GL_NUM_EXTENSIONS, &n);
        if (GetStringi && GetError() == GL_NO_ERROR && n > 0) {
            for (GLint i = 0; i < n; ++i) {
                if (const GLubyte* e = GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i))) {
                    extensions.emplace(reinterpret_cast<const char*>(e));
                }
            }
        } else if (const GLubyte* all = GetString(GL_EXTENSIONS)) {
            std::string_view s(reinterpret_cast<const char*>(all));
            size_t pos = 0;
            while (pos < s.size()) {
                size_t end = s.find(' ', pos);
                if (end == std::string_view::npos) {
                    end = s.size();
                }
                if (end > pos) {
                    extensions.emplace(s.substr(pos, end - pos));
                }
                pos = end + 1;
            }
        }
        while (GetError() != GL_NO_ERROR) {
        }
        return true;
    }
};

namespace {

    GpuStatus fail(VkResult r, std::string msg)
    {
        return GpuStatus { r, std::move(msg) };
    }

    constexpr VkExternalMemoryHandleTypeFlagBits kMemHandle = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    constexpr VkExternalSemaphoreHandleTypeFlagBits kSemHandle = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    constexpr VkDeviceSize kImportSlack = 256;
    constexpr VkBufferUsageFlags kBufferUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    void appendMissing(std::string& missing, const std::string& what)
    {
        if (!missing.empty()) {
            missing += ", ";
        }
        missing += what;
    }

    std::string hex(const std::array<uint8_t, 16>& u)
    {
        static const char* digits = "0123456789abcdef";
        std::string s;
        for (uint8_t b : u) {
            s += digits[b >> 4];
            s += digits[b & 15];
        }
        return s;
    }

} // namespace

const char*
toString(GlHandoffPath path)
{
    switch (path) {
    case GlHandoffPath::ZeroCopy:
        return "zero-copy";
    case GlHandoffPath::ZeroCopyHostSync:
        return "zero-copy-hostsync";
    case GlHandoffPath::Readback:
        return "readback";
    }
    return "?";
}

uint32_t
bytesPerPixel(GlPixelFormat format)
{
    switch (format) {
    case GlPixelFormat::Rgba32F:
        return 4 * sizeof(float);
    case GlPixelFormat::Bgra8:
        return 4;
    }
    return 0;
}

bool
GlInteropCaps::supports(GlHandoffPath path) const
{
    const bool memory = vkExternalMemoryFd && vkBufferExportable && glMemoryObject && glMemoryObjectFd && uuidMatch;
    switch (path) {
    case GlHandoffPath::ZeroCopy:
        return memory && vkExternalSemaphoreFd && vkSemaphoreExportable && glSemaphore && glSemaphoreFd;
    case GlHandoffPath::ZeroCopyHostSync:
        return memory;
    case GlHandoffPath::Readback:
        return true;
    }
    return false;
}

GpuStatus
GlInterop::probe(GpuDevice& device, const GlProcLoader& loader, GlInteropCaps& out)
{
    out = GlInteropCaps {};
    Gl gl;
    if (!gl.load(loader)) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GL core entry points unavailable; is a context current?");
    }
    auto str = [&](GLenum e) {
        const GLubyte* s = gl.GetString(e);
        return s ? std::string(reinterpret_cast<const char*>(s)) : std::string();
    };
    out.glVendor = str(GL_VENDOR);
    out.glRenderer = str(GL_RENDERER);
    out.glVersion = str(GL_VERSION);
    // With no current context a GL dispatcher still resolves entry points but they are no-ops that
    // return null, so a missing version string is the only sign the context is absent.
    if (out.glVersion.empty()) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "no current GL context: glGetString(GL_VERSION) returned null");
    }

    const GpuDeviceInfo& info = device.info();
    out.vkExternalMemoryFd = info.externalMemoryFd;
    out.vkExternalSemaphoreFd = info.externalSemaphoreFd;

    VkPhysicalDeviceExternalBufferInfo bi { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
    bi.usage = kBufferUsage;
    bi.handleType = kMemHandle;
    VkExternalBufferProperties bp { VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    vkGetPhysicalDeviceExternalBufferProperties(device.physicalDevice(), &bi, &bp);
    out.vkBufferExportable = (bp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;

    VkPhysicalDeviceExternalSemaphoreInfo si { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO };
    si.handleType = kSemHandle;
    VkExternalSemaphoreProperties sp { VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
    vkGetPhysicalDeviceExternalSemaphoreProperties(device.physicalDevice(), &si, &sp);
    out.vkSemaphoreExportable = (sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) != 0;

    VkPhysicalDeviceIDProperties idp { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &idp;
    vkGetPhysicalDeviceProperties2(device.physicalDevice(), &p2);
    std::memcpy(out.vkDeviceUuid.data(), idp.deviceUUID, VK_UUID_SIZE);
    std::memcpy(out.vkDriverUuid.data(), idp.driverUUID, VK_UUID_SIZE);

    out.glMemoryObject = gl.has("GL_EXT_memory_object") && gl.CreateMemoryObjectsEXT && gl.BufferStorageMemEXT
        && gl.GetUnsignedBytevEXT && gl.GetUnsignedBytei_vEXT;
    out.glMemoryObjectFd = gl.has("GL_EXT_memory_object_fd") && gl.ImportMemoryFdEXT;
    out.glSemaphore = gl.has("GL_EXT_semaphore") && gl.GenSemaphoresEXT && gl.WaitSemaphoreEXT
        && gl.SignalSemaphoreEXT;
    out.glSemaphoreFd = gl.has("GL_EXT_semaphore_fd") && gl.ImportSemaphoreFdEXT;

    // The UUID queries are defined by either EXT_memory_object or EXT_semaphore.
    if ((out.glMemoryObject || out.glSemaphore) && gl.GetUnsignedBytevEXT && gl.GetUnsignedBytei_vEXT) {
        gl.GetUnsignedBytevEXT(GL_DRIVER_UUID_EXT, out.glDriverUuid.data());
        GLint devices = 0;
        gl.GetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &devices);
        bool deviceMatch = false;
        for (GLint i = 0; i < devices; ++i) {
            std::array<uint8_t, 16> u {};
            gl.GetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT, static_cast<GLuint>(i), u.data());
            if (i == 0) {
                out.glDeviceUuid = u;
            }
            if (u == out.vkDeviceUuid) {
                out.glDeviceUuid = u;
                deviceMatch = true;
            }
        }
        out.uuidMatch = deviceMatch && out.glDriverUuid == out.vkDriverUuid;
        while (gl.GetError() != GL_NO_ERROR) {
        }
    }

    if (!out.vkExternalMemoryFd) {
        appendMissing(out.missing, "VK_KHR_external_memory_fd");
    } else if (!out.vkBufferExportable) {
        appendMissing(out.missing, "Vulkan OPAQUE_FD buffer export");
    }
    if (!out.vkExternalSemaphoreFd) {
        appendMissing(out.missing, "VK_KHR_external_semaphore_fd");
    } else if (!out.vkSemaphoreExportable) {
        appendMissing(out.missing, "Vulkan OPAQUE_FD semaphore export");
    }
    if (!out.glMemoryObject) {
        appendMissing(out.missing, "GL_EXT_memory_object");
    }
    if (!out.glMemoryObjectFd) {
        appendMissing(out.missing, "GL_EXT_memory_object_fd");
    }
    if (!out.glSemaphore) {
        appendMissing(out.missing, "GL_EXT_semaphore");
    }
    if (!out.glSemaphoreFd) {
        appendMissing(out.missing, "GL_EXT_semaphore_fd");
    }
    if ((out.glMemoryObject || out.glSemaphore) && !out.uuidMatch) {
        appendMissing(out.missing, "UUID mismatch (vk device " + hex(out.vkDeviceUuid) + " driver " + hex(out.vkDriverUuid) + ", gl device " + hex(out.glDeviceUuid) + " driver " + hex(out.glDriverUuid) + ")");
    }

    if (out.supports(GlHandoffPath::ZeroCopy)) {
        out.best = GlHandoffPath::ZeroCopy;
    } else if (out.supports(GlHandoffPath::ZeroCopyHostSync)) {
        out.best = GlHandoffPath::ZeroCopyHostSync;
    } else {
        out.best = GlHandoffPath::Readback;
    }
    return {};
}

GpuStatus
GlInterop::create(GpuDevice& device,
                  const GlProcLoader& loader,
                  VkDeviceSize capacityBytes,
                  GlHandoffPath path,
                  std::unique_ptr<GlInterop>& out)
{
    out.reset();
    if (capacityBytes == 0) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GlInterop capacity must be non-zero");
    }
    GlInteropCaps caps;
    if (GpuStatus s = probe(device, loader, caps); !s) {
        return s;
    }
    if (!caps.supports(path)) {
        return fail(VK_ERROR_FEATURE_NOT_PRESENT,
                    std::string(toString(path)) + " unavailable: missing " + caps.missing);
    }

    std::unique_ptr<GlInterop> self(new GlInterop);
    self->device_ = &device;
    self->gl_ = std::make_unique<Gl>();
    if (!self->gl_->load(loader)) {
        return fail(VK_ERROR_INITIALIZATION_FAILED, "GL core entry points unavailable; is a context current?");
    }
    self->path_ = path;
    self->capacity_ = capacityBytes;

    const bool exportable = path != GlHandoffPath::Readback;
    const bool semaphores = path == GlHandoffPath::ZeroCopy;
    if (GpuStatus s = self->initVulkan(exportable, semaphores); !s) {
        return s;
    }
    while (self->gl_->GetError() != GL_NO_ERROR) {
    }
    for (Slot& slot : self->slots_) {
        if (GpuStatus s = self->initGlSlot(slot, semaphores); !s) {
            return s;
        }
    }
    out = std::move(self);
    return {};
}

GpuStatus
GlInterop::initVulkan(bool exportable, bool semaphores)
{
    GpuDevice& dev = *device_;
    VkDevice vk = dev.device();

    if (exportable) {
        VkExternalMemoryBufferCreateInfo extBuf { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        extBuf.handleTypes = kMemHandle;
        VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.pNext = &extBuf;
        bci.size = capacity_ + kImportSlack;
        bci.usage = kBufferUsage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo aci {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        uint32_t typeIndex = 0;
        if (GpuStatus s = dev.check(vmaFindMemoryTypeIndexForBufferInfo(dev.allocator(), &bci, &aci, &typeIndex),
                                    "vmaFindMemoryTypeIndexForBufferInfo");
            !s) {
            return s;
        }
        // VMA keeps this pointer and chains it into every allocation the pool makes.
        exportInfo_.handleTypes = kMemHandle;
        VmaPoolCreateInfo pci {};
        pci.memoryTypeIndex = typeIndex;
        pci.pMemoryAllocateNext = &exportInfo_;
        if (GpuStatus s = dev.check(vmaCreatePool(dev.allocator(), &pci, &pool_), "vmaCreatePool"); !s) {
            return s;
        }
    }

    VkCommandPoolCreateInfo cpci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = dev.queueFamily(QueueKind::Compute);
    if (GpuStatus s = dev.check(vkCreateCommandPool(vk, &cpci, nullptr, &cmdPool_), "vkCreateCommandPool"); !s) {
        return s;
    }
    for (Slot& slot : slots_) {
        if (GpuStatus s = initVulkanSlot(slot, exportable, semaphores); !s) {
            return s;
        }
    }
    return {};
}

GpuStatus
GlInterop::ensureReadback(Slot& slot)
{
    if (slot.readback != VK_NULL_HANDLE) {
        return {};
    }
    VkBufferCreateInfo rci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    rci.size = capacity_;
    rci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo rai {};
    rai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    rai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo rinfo {};
    if (GpuStatus s = device_->check(
            vmaCreateBuffer(device_->allocator(), &rci, &rai, &slot.readback, &slot.readbackAllocation, &rinfo),
            "vmaCreateBuffer(readback)");
        !s) {
        return s;
    }
    slot.readbackMapped = rinfo.pMappedData;
    return {};
}

GpuStatus
GlInterop::initVulkanSlot(Slot& slot, bool exportable, bool semaphores)
{
    GpuDevice& dev = *device_;
    VkDevice vk = dev.device();

    VkExternalMemoryBufferCreateInfo extBuf { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    extBuf.handleTypes = kMemHandle;
    VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    // llvmpipe refuses to back a GL buffer with imported memory unless the memory exceeds the buffer
    // by its rasterizer over-read margin (3 RGBA32F pixels).
    bci.size = exportable ? capacity_ + kImportSlack : capacity_;
    bci.usage = kBufferUsage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (exportable) {
        bci.pNext = &extBuf;
    }

    VmaAllocationCreateInfo aci {};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (exportable) {
        aci.pool = pool_;
        // GL imports the whole VkDeviceMemory, so it must hold this buffer alone at offset 0.
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    if (GpuStatus s = dev.check(vmaCreateBuffer(dev.allocator(), &bci, &aci, &slot.buffer, &slot.allocation, nullptr),
                                "vmaCreateBuffer");
        !s) {
        return s;
    }

    if (!exportable) {
        if (GpuStatus s = ensureReadback(slot); !s) {
            return s;
        }
    }

    VkCommandBufferAllocateInfo cbai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = cmdPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (GpuStatus s = dev.check(vkAllocateCommandBuffers(vk, &cbai, &slot.cmd), "vkAllocateCommandBuffers"); !s) {
        return s;
    }
    VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (GpuStatus s = dev.check(vkCreateFence(vk, &fci, nullptr, &slot.fence), "vkCreateFence"); !s) {
        return s;
    }

    if (semaphores) {
        VkExportSemaphoreCreateInfo esci { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
        esci.handleTypes = kSemHandle;
        VkSemaphoreCreateInfo sci { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        sci.pNext = &esci;
        for (VkSemaphore* sem : { &slot.vkReady, &slot.glDone }) {
            if (GpuStatus s = dev.check(vkCreateSemaphore(vk, &sci, nullptr, sem), "vkCreateSemaphore"); !s) {
                return s;
            }
        }
    }
    return {};
}

GpuStatus
GlInterop::glError(const char* what)
{
    GLenum first = GL_NO_ERROR;
    for (GLenum e = gl_->GetError(); e != GL_NO_ERROR; e = gl_->GetError()) {
        if (first == GL_NO_ERROR) {
            first = e;
        }
    }
    if (first == GL_NO_ERROR) {
        return {};
    }
    return fail(VK_ERROR_UNKNOWN, std::string(what) + " raised GL error 0x" + [&] {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%04x", first);
        return std::string(buf); }());
}

GpuStatus
GlInterop::initGlSlot(Slot& slot, bool semaphores)
{
    Gl& gl = *gl_;
    VkDevice vk = device_->device();

    if (path_ == GlHandoffPath::Readback) {
        return {};
    }
    gl.GenBuffers(1, &slot.glBuffer);

    auto getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(vk, "vkGetMemoryFdKHR"));
    if (!getMemoryFd) {
        return fail(VK_ERROR_EXTENSION_NOT_PRESENT, "vkGetMemoryFdKHR unavailable");
    }
    VmaAllocationInfo ai {};
    vmaGetAllocationInfo(device_->allocator(), slot.allocation, &ai);
    VkMemoryGetFdInfoKHR mfi { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    mfi.memory = ai.deviceMemory;
    mfi.handleType = kMemHandle;
    int memFd = -1;
    if (GpuStatus s = device_->check(getMemoryFd(vk, &mfi, &memFd), "vkGetMemoryFdKHR"); !s) {
        return s;
    }

    // GL takes ownership of the fd only when the import itself succeeds, so every earlier call is
    // checked on its own: a stale error read after the import would close an fd GL already owns.
    gl.CreateMemoryObjectsEXT(1, &slot.glMemory);
    GpuStatus s = glError("glCreateMemoryObjectsEXT");
    if (s) {
        const GLint dedicated = GL_TRUE;
        gl.MemoryObjectParameterivEXT(slot.glMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        s = glError("glMemoryObjectParameterivEXT");
    }
    if (!s) {
        close(memFd);
        return s;
    }
    gl.ImportMemoryFdEXT(slot.glMemory, ai.offset + ai.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, memFd);
    if (s = glError("glImportMemoryFdEXT"); !s) {
        close(memFd);
        return s;
    }
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, slot.glBuffer);
    gl.BufferStorageMemEXT(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(capacity_), slot.glMemory, ai.offset);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (s = glError("glBufferStorageMemEXT"); !s) {
        return s;
    }

    if (!semaphores) {
        return {};
    }
    auto getSemFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(vk, "vkGetSemaphoreFdKHR"));
    if (!getSemFd) {
        return fail(VK_ERROR_EXTENSION_NOT_PRESENT, "vkGetSemaphoreFdKHR unavailable");
    }
    std::pair<VkSemaphore, GLuint*> pairs[] = { { slot.vkReady, &slot.glReady }, { slot.glDone, &slot.glDoneGl } };
    for (auto& [vkSem, glSem] : pairs) {
        VkSemaphoreGetFdInfoKHR sfi { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
        sfi.semaphore = vkSem;
        sfi.handleType = kSemHandle;
        int fd = -1;
        if (GpuStatus s = device_->check(getSemFd(vk, &sfi, &fd), "vkGetSemaphoreFdKHR"); !s) {
            return s;
        }
        gl.GenSemaphoresEXT(1, glSem);
        if (GpuStatus s = glError("glGenSemaphoresEXT"); !s) {
            close(fd);
            return s;
        }
        gl.ImportSemaphoreFdEXT(*glSem, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
        if (GpuStatus s = glError("glImportSemaphoreFdEXT"); !s) {
            close(fd);
            return s;
        }
    }
    return {};
}

GlInterop::~GlInterop()
{
    if (device_) {
        for (Slot& slot : slots_) {
            if (slot.inFlight) {
                vkWaitForFences(device_->device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
            }
        }
    }
    if (gl_) {
        bool glBusy = path_ == GlHandoffPath::ZeroCopyHostSync;
        for (const Slot& slot : slots_) {
            glBusy = glBusy || slot.glSignaled;
        }
        if (glBusy) {
            gl_->Finish();
        }
        for (Slot& slot : slots_) {
            if (slot.glBuffer) {
                gl_->DeleteBuffers(1, &slot.glBuffer);
            }
            if (slot.glMemory && gl_->DeleteMemoryObjectsEXT) {
                gl_->DeleteMemoryObjectsEXT(1, &slot.glMemory);
            }
            for (GLuint* sem : { &slot.glReady, &slot.glDoneGl }) {
                if (*sem && gl_->DeleteSemaphoresEXT) {
                    gl_->DeleteSemaphoresEXT(1, sem);
                }
            }
        }
    }
    if (!device_) {
        return;
    }
    VkDevice vk = device_->device();
    for (Slot& slot : slots_) {
        for (VkSemaphore sem : { slot.vkReady, slot.glDone }) {
            if (sem != VK_NULL_HANDLE) {
                vkDestroySemaphore(vk, sem, nullptr);
            }
        }
        if (slot.fence != VK_NULL_HANDLE) {
            vkDestroyFence(vk, slot.fence, nullptr);
        }
        if (slot.readback != VK_NULL_HANDLE) {
            vmaDestroyBuffer(device_->allocator(), slot.readback, slot.readbackAllocation);
        }
        if (slot.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(device_->allocator(), slot.buffer, slot.allocation);
        }
    }
    if (cmdPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vk, cmdPool_, nullptr);
    }
    if (pool_) {
        vmaDestroyPool(device_->allocator(), pool_);
    }
}

GpuStatus
GlInterop::waitProduced(int slot, uint64_t timeoutNs)
{
    if (slot < 0 || slot >= kSlots) {
        return fail(VK_ERROR_UNKNOWN, "waitProduced() on slot " + std::to_string(slot) + " out of range");
    }
    Slot& sl = slots_[slot];
    if (!sl.inFlight) {
        return {};
    }
    const VkResult r = vkWaitForFences(device_->device(), 1, &sl.fence, VK_TRUE, timeoutNs);
    if (r == VK_TIMEOUT) {
        return fail(VK_TIMEOUT, "vkWaitForFences timed out");
    }
    if (GpuStatus s = device_->check(r, "vkWaitForFences"); !s) {
        return s;
    }
    sl.inFlight = false;
    return {};
}

int
GlInterop::nextSlot() const
{
    int chosen = -1;
    for (int i = 0; i < kSlots; ++i) {
        if (slots_[i].pendingUpload) {
            continue;
        }
        if (chosen < 0 || chosen == lastUploaded_) {
            chosen = i;
        }
    }
    return chosen;
}

GpuStatus
GlInterop::produce(const GlFrame& frame,
                   const GlRecordFn& record,
                   std::span<const TimelineValue> waits,
                   int* slot,
                   bool hostCopy)
{
    if (!slot) {
        return fail(VK_ERROR_UNKNOWN, "produce() needs somewhere to store the slot");
    }
    *slot = -1;
    if (frame.width == 0 || frame.height == 0) {
        return fail(VK_ERROR_UNKNOWN, "produce() of an empty frame");
    }
    // Compared by division so a huge width x height cannot wrap the product past the capacity.
    const VkDeviceSize bpp = bytesPerPixel(frame.format);
    if (frame.width > capacity_ / bpp / frame.height) {
        return fail(VK_ERROR_OUT_OF_POOL_MEMORY,
                    "frame " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + " needs more than the "
                        + std::to_string(capacity_) + "-byte slot capacity");
    }

    const int chosen = nextSlot();
    if (chosen < 0) {
        return fail(VK_ERROR_UNKNOWN, "produce() with every slot still awaiting upload()");
    }
    Slot& sl = slots_[chosen];

    if (GpuStatus s = waitProduced(chosen); !s) {
        return s;
    }
    if (hostCopy) {
        if (GpuStatus s = ensureReadback(sl); !s) {
            return s;
        }
    }
    VkCommandBuffer cmd = sl.cmd;
    if (GpuStatus s = device_->check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer"); !s) {
        return s;
    }
    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (GpuStatus s = device_->check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer"); !s) {
        return s;
    }

    const uint32_t family = device_->queueFamily(QueueKind::Compute);
    auto barrier = [&](const VkBufferMemoryBarrier2& b) {
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    // The exported buffer is EXCLUSIVE, so GL's use must be bracketed by external queue-family transfers.
    if (sl.releasedToGl) {
        VkBufferMemoryBarrier2 acquire { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        acquire.dstQueueFamilyIndex = family;
        acquire.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        acquire.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        acquire.buffer = sl.buffer;
        acquire.size = VK_WHOLE_SIZE;
        barrier(acquire);
    }

    if (GpuStatus s = record(cmd, sl.buffer); !s) {
        return s;
    }

    const bool copyToHost = hostCopy || path_ == GlHandoffPath::Readback;
    if (copyToHost) {
        VkBufferMemoryBarrier2 toCopy { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        toCopy.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toCopy.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        toCopy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.buffer = sl.buffer;
        toCopy.size = VK_WHOLE_SIZE;
        barrier(toCopy);
        VkBufferCopy region { 0, 0, frame.bytes() };
        vkCmdCopyBuffer(cmd, sl.buffer, sl.readback, 1, &region);
        VkBufferMemoryBarrier2 toHost { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = sl.readback;
        toHost.size = VK_WHOLE_SIZE;
        barrier(toHost);
    }
    if (path_ != GlHandoffPath::Readback) {
        VkBufferMemoryBarrier2 release { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        release.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        release.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        release.srcQueueFamilyIndex = family;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        release.buffer = sl.buffer;
        release.size = VK_WHOLE_SIZE;
        barrier(release);
    }

    if (GpuStatus s = device_->check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer"); !s) {
        return s;
    }

    std::vector<VkSemaphoreSubmitInfo> waitInfos;
    waitInfos.reserve(waits.size() + 1);
    for (const TimelineValue& v : waits) {
        if (v) {
            waitInfos.push_back(v.waitInfo());
        }
    }
    if (path_ == GlHandoffPath::ZeroCopy && sl.glSignaled) {
        VkSemaphoreSubmitInfo glRead { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        glRead.semaphore = sl.glDone;
        glRead.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        waitInfos.push_back(glRead);
    }
    VkCommandBufferSubmitInfo cbsi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbsi.commandBuffer = cmd;
    VkSemaphoreSubmitInfo signal { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signal.semaphore = sl.vkReady;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.waitSemaphoreInfoCount = static_cast<uint32_t>(waitInfos.size());
    submit.pWaitSemaphoreInfos = waitInfos.data();
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cbsi;
    if (path_ == GlHandoffPath::ZeroCopy) {
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
    }
    if (GpuStatus s = device_->check(vkResetFences(device_->device(), 1, &sl.fence), "vkResetFences"); !s) {
        return s;
    }
    if (GpuStatus s = device_->submit(QueueKind::Compute, { &submit, 1 }, sl.fence); !s) {
        return s;
    }
    sl.frame = frame;
    sl.hostCopyValid = copyToHost;
    sl.glSignaled = false;
    sl.inFlight = true;
    sl.pendingUpload = true;
    sl.releasedToGl = path_ != GlHandoffPath::Readback;
    *slot = chosen;
    return {};
}

GpuStatus
GlInterop::upload(int slot, unsigned int texture, int x, int y)
{
    if (slot < 0 || slot >= kSlots) {
        return fail(VK_ERROR_UNKNOWN, "upload() on slot " + std::to_string(slot) + " out of range");
    }
    Slot& sl = slots_[slot];
    if (!sl.pendingUpload) {
        return fail(VK_ERROR_UNKNOWN, "upload() of a slot with no pending produce()");
    }
    Gl& gl = *gl_;
    while (gl.GetError() != GL_NO_ERROR) {
    }

    if (path_ == GlHandoffPath::ZeroCopy) {
        gl.WaitSemaphoreEXT(sl.glReady, 1, &sl.glBuffer, 0, nullptr, nullptr);
    } else if (GpuStatus s = waitProduced(slot); !s) {
        return s;
    }

    const VkDeviceSize bytes = sl.frame.bytes();
    // The Readback path uploads straight from the mapped host copy: client memory is read before
    // glTexSubImage2D returns, and a staging PBO would only add a frame-sized copy into fresh pages.
    const void* hostSource = nullptr;
    if (path_ == GlHandoffPath::Readback) {
        if (GpuStatus s = device_->check(
                vmaInvalidateAllocation(device_->allocator(), sl.readbackAllocation, 0, bytes),
                "vmaInvalidateAllocation");
            !s) {
            return s;
        }
        hostSource = sl.readbackMapped;
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    } else {
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, sl.glBuffer);
    }

    const bool bgra8 = sl.frame.format == GlPixelFormat::Bgra8;
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, x, y, static_cast<GLsizei>(sl.frame.width),
                     static_cast<GLsizei>(sl.frame.height), bgra8 ? GL_BGRA : GL_RGBA,
                     bgra8 ? GL_UNSIGNED_INT_8_8_8_8_REV : GL_FLOAT, hostSource);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    if (path_ == GlHandoffPath::ZeroCopy) {
        gl.SignalSemaphoreEXT(sl.glDoneGl, 1, &sl.glBuffer, 0, nullptr, nullptr);
        // Vulkan may not wait on a binary semaphore whose signal has not been submitted yet.
        gl.Flush();
        sl.glSignaled = true;
    } else if (path_ == GlHandoffPath::ZeroCopyHostSync) {
        gl.Finish();
    }
    sl.pendingUpload = false;
    lastUploaded_ = slot;
    return glError("upload");
}

GpuStatus
GlInterop::discard(int slot)
{
    if (slot < 0 || slot >= kSlots) {
        return fail(VK_ERROR_UNKNOWN, "discard() on slot " + std::to_string(slot) + " out of range");
    }
    Slot& sl = slots_[slot];
    if (!sl.pendingUpload) {
        return fail(VK_ERROR_UNKNOWN, "discard() of a slot with no pending produce()");
    }
    if (path_ == GlHandoffPath::ZeroCopy) {
        // produce() signalled the binary semaphore, which must be waited on before it is signalled again, and the
        // next produce() waits for GL to give the buffer back.
        Gl& gl = *gl_;
        while (gl.GetError() != GL_NO_ERROR) {
        }
        gl.WaitSemaphoreEXT(sl.glReady, 1, &sl.glBuffer, 0, nullptr, nullptr);
        gl.SignalSemaphoreEXT(sl.glDoneGl, 1, &sl.glBuffer, 0, nullptr, nullptr);
        gl.Flush();
        sl.glSignaled = true;
    }
    sl.pendingUpload = false;
    return path_ == GlHandoffPath::ZeroCopy ? glError("discard") : GpuStatus {};
}

GpuStatus
GlInterop::hostCopyView(int slot, const unsigned char** data, VkDeviceSize* size)
{
    if (slot < 0 || slot >= kSlots) {
        return fail(VK_ERROR_UNKNOWN, "hostCopyView() on slot " + std::to_string(slot) + " out of range");
    }
    Slot& sl = slots_[slot];
    if (!sl.hostCopyValid || sl.readback == VK_NULL_HANDLE) {
        return fail(VK_ERROR_UNKNOWN, "hostCopyView() of a slot with no host copy");
    }
    if (GpuStatus s = waitProduced(slot, 0); !s) {
        return s;
    }
    const VkDeviceSize bytes = sl.frame.bytes();
    if (GpuStatus s = device_->check(vmaInvalidateAllocation(device_->allocator(), sl.readbackAllocation, 0, bytes),
                                     "vmaInvalidateAllocation");
        !s) {
        return s;
    }
    *data = static_cast<const unsigned char*>(sl.readbackMapped);
    *size = bytes;
    return {};
}

} // namespace gpu
