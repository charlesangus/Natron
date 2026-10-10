#include "GlInterop.h"

#include <GL/gl.h>
#include <GL/glext.h>

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <set>
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
    PFNGLBUFFERDATAPROC BufferData = nullptr;
    PFNGLMAPBUFFERRANGEPROC MapBufferRange = nullptr;
    PFNGLUNMAPBUFFERPROC UnmapBuffer = nullptr;

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
        get(BufferData, "glBufferData");
        get(MapBufferRange, "glMapBufferRange");
        get(UnmapBuffer, "glUnmapBuffer");
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
            || !Flush || !Finish || !GenBuffers || !DeleteBuffers || !BindBuffer || !BufferData
            || !MapBufferRange || !UnmapBuffer) {
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
        return "readback-pbo";
    }
    return "?";
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
                  uint32_t width,
                  uint32_t height,
                  GlHandoffPath path,
                  std::unique_ptr<GlInterop>& out)
{
    out.reset();
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
    self->gl_->load(loader);
    self->path_ = path;
    self->width_ = width;
    self->height_ = height;
    self->size_ = VkDeviceSize(width) * height * 4 * sizeof(float);

    const bool exportable = path != GlHandoffPath::Readback;
    const bool semaphores = path == GlHandoffPath::ZeroCopy;
    if (GpuStatus s = self->initVulkan(exportable, semaphores); !s) {
        return s;
    }
    if (GpuStatus s = self->initGl(semaphores); !s) {
        return s;
    }
    out = std::move(self);
    return {};
}

GpuStatus
GlInterop::initVulkan(bool exportable, bool semaphores)
{
    GpuDevice& dev = *device_;
    VkDevice vk = dev.device();

    VkExternalMemoryBufferCreateInfo extBuf { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    extBuf.handleTypes = kMemHandle;
    VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    // llvmpipe refuses to back a GL buffer with imported memory unless the memory exceeds the buffer
    // by its rasterizer over-read margin (3 RGBA32F pixels).
    bci.size = exportable ? size_ + kImportSlack : size_;
    bci.usage = kBufferUsage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (exportable) {
        bci.pNext = &extBuf;
    }

    VmaAllocationCreateInfo aci {};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (exportable) {
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
        aci.pool = pool_;
        // GL imports the whole VkDeviceMemory, so it must hold this buffer alone at offset 0.
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    }
    VmaAllocationInfo ai {};
    if (GpuStatus s = dev.check(vmaCreateBuffer(dev.allocator(), &bci, &aci, &buffer_, &allocation_, &ai),
                                "vmaCreateBuffer");
        !s) {
        return s;
    }

    if (!exportable) {
        VkBufferCreateInfo rci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        rci.size = size_;
        rci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo rai {};
        rai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        rai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo rinfo {};
        if (GpuStatus s = dev.check(
                vmaCreateBuffer(dev.allocator(), &rci, &rai, &readback_, &readbackAllocation_, &rinfo),
                "vmaCreateBuffer(readback)");
            !s) {
            return s;
        }
        readbackMapped_ = rinfo.pMappedData;
    }

    VkCommandPoolCreateInfo cpci { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = dev.queueFamily(QueueKind::Compute);
    if (GpuStatus s = dev.check(vkCreateCommandPool(vk, &cpci, nullptr, &cmdPool_), "vkCreateCommandPool"); !s) {
        return s;
    }
    VkCommandBufferAllocateInfo cbai { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = cmdPool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (GpuStatus s = dev.check(vkAllocateCommandBuffers(vk, &cbai, &cmd_), "vkAllocateCommandBuffers"); !s) {
        return s;
    }
    VkFenceCreateInfo fci { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (GpuStatus s = dev.check(vkCreateFence(vk, &fci, nullptr, &fence_), "vkCreateFence"); !s) {
        return s;
    }

    if (semaphores) {
        VkExportSemaphoreCreateInfo esci { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
        esci.handleTypes = kSemHandle;
        VkSemaphoreCreateInfo sci { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        sci.pNext = &esci;
        for (VkSemaphore* sem : { &vkReady_, &glDone_ }) {
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
GlInterop::initGl(bool semaphores)
{
    Gl& gl = *gl_;
    VkDevice vk = device_->device();
    while (gl.GetError() != GL_NO_ERROR) {
    }

    gl.GenBuffers(1, &glBuffer_);
    if (path_ == GlHandoffPath::Readback) {
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, glBuffer_);
        gl.BufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(size_), nullptr, GL_STREAM_DRAW);
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return glError("PBO allocation");
    }

    auto getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(vk, "vkGetMemoryFdKHR"));
    if (!getMemoryFd) {
        return fail(VK_ERROR_EXTENSION_NOT_PRESENT, "vkGetMemoryFdKHR unavailable");
    }
    VmaAllocationInfo ai {};
    vmaGetAllocationInfo(device_->allocator(), allocation_, &ai);
    VkMemoryGetFdInfoKHR mfi { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    mfi.memory = ai.deviceMemory;
    mfi.handleType = kMemHandle;
    int memFd = -1;
    if (GpuStatus s = device_->check(getMemoryFd(vk, &mfi, &memFd), "vkGetMemoryFdKHR"); !s) {
        return s;
    }

    // GL takes ownership of the fd only when the import itself succeeds, so every earlier call is
    // checked on its own: a stale error read after the import would close an fd GL already owns.
    gl.CreateMemoryObjectsEXT(1, &glMemory_);
    GpuStatus s = glError("glCreateMemoryObjectsEXT");
    if (s) {
        const GLint dedicated = GL_TRUE;
        gl.MemoryObjectParameterivEXT(glMemory_, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        s = glError("glMemoryObjectParameterivEXT");
    }
    if (!s) {
        close(memFd);
        return s;
    }
    gl.ImportMemoryFdEXT(glMemory_, ai.offset + ai.size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, memFd);
    if (s = glError("glImportMemoryFdEXT"); !s) {
        close(memFd);
        return s;
    }
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, glBuffer_);
    gl.BufferStorageMemEXT(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(size_), glMemory_, ai.offset);
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
    std::pair<VkSemaphore, GLuint*> pairs[] = { { vkReady_, &glReady_ }, { glDone_, &glDoneGl_ } };
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
    if (device_ && fence_ != VK_NULL_HANDLE && submitted_ && !fenceWaited_) {
        vkWaitForFences(device_->device(), 1, &fence_, VK_TRUE, UINT64_MAX);
    }
    if (gl_) {
        if (glSignaled_ || path_ == GlHandoffPath::ZeroCopyHostSync) {
            gl_->Finish();
        }
        if (glBuffer_) {
            gl_->DeleteBuffers(1, &glBuffer_);
        }
        if (glMemory_ && gl_->DeleteMemoryObjectsEXT) {
            gl_->DeleteMemoryObjectsEXT(1, &glMemory_);
        }
        for (GLuint* sem : { &glReady_, &glDoneGl_ }) {
            if (*sem && gl_->DeleteSemaphoresEXT) {
                gl_->DeleteSemaphoresEXT(1, sem);
            }
        }
    }
    if (!device_) {
        return;
    }
    VkDevice vk = device_->device();
    for (VkSemaphore sem : { vkReady_, glDone_ }) {
        if (sem != VK_NULL_HANDLE) {
            vkDestroySemaphore(vk, sem, nullptr);
        }
    }
    if (fence_ != VK_NULL_HANDLE) {
        vkDestroyFence(vk, fence_, nullptr);
    }
    if (cmdPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vk, cmdPool_, nullptr);
    }
    if (readback_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(device_->allocator(), readback_, readbackAllocation_);
    }
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(device_->allocator(), buffer_, allocation_);
    }
    if (pool_) {
        vmaDestroyPool(device_->allocator(), pool_);
    }
}

GpuStatus
GlInterop::waitProduced(uint64_t timeoutNs)
{
    if (!submitted_ || fenceWaited_) {
        return {};
    }
    const VkResult r = vkWaitForFences(device_->device(), 1, &fence_, VK_TRUE, timeoutNs);
    if (r == VK_TIMEOUT) {
        return fail(VK_TIMEOUT, "vkWaitForFences timed out");
    }
    if (GpuStatus s = device_->check(r, "vkWaitForFences"); !s) {
        return s;
    }
    fenceWaited_ = true;
    return {};
}

GpuStatus
GlInterop::produce(const std::function<GpuStatus(VkCommandBuffer)>& record)
{
    if (pendingUpload_) {
        return fail(VK_ERROR_UNKNOWN, "produce() called twice without an upload() in between");
    }
    if (GpuStatus s = waitProduced(); !s) {
        return s;
    }
    VkDevice vk = device_->device();
    if (submitted_) {
        if (GpuStatus s = device_->check(vkResetFences(vk, 1, &fence_), "vkResetFences"); !s) {
            return s;
        }
    }
    if (GpuStatus s = device_->check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer"); !s) {
        return s;
    }
    VkCommandBufferBeginInfo begin { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (GpuStatus s = device_->check(vkBeginCommandBuffer(cmd_, &begin), "vkBeginCommandBuffer"); !s) {
        return s;
    }

    const uint32_t family = device_->queueFamily(QueueKind::Compute);
    auto barrier = [&](const VkBufferMemoryBarrier2& b) {
        VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dep.bufferMemoryBarrierCount = 1;
        dep.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd_, &dep);
    };

    // The exported buffer is EXCLUSIVE, so GL's use must be bracketed by external queue-family transfers.
    if (releasedToGl_) {
        VkBufferMemoryBarrier2 acquire { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        acquire.dstQueueFamilyIndex = family;
        acquire.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        acquire.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        acquire.buffer = buffer_;
        acquire.size = VK_WHOLE_SIZE;
        barrier(acquire);
    }

    if (GpuStatus s = record(cmd_); !s) {
        return s;
    }

    if (path_ == GlHandoffPath::Readback) {
        VkBufferMemoryBarrier2 toCopy { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        toCopy.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toCopy.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        toCopy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toCopy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCopy.buffer = buffer_;
        toCopy.size = VK_WHOLE_SIZE;
        barrier(toCopy);
        VkBufferCopy region { 0, 0, size_ };
        vkCmdCopyBuffer(cmd_, buffer_, readback_, 1, &region);
        VkBufferMemoryBarrier2 toHost { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = readback_;
        toHost.size = VK_WHOLE_SIZE;
        barrier(toHost);
    } else {
        VkBufferMemoryBarrier2 release { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        release.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        release.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        release.srcQueueFamilyIndex = family;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        release.buffer = buffer_;
        release.size = VK_WHOLE_SIZE;
        barrier(release);
    }

    if (GpuStatus s = device_->check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer"); !s) {
        return s;
    }

    VkCommandBufferSubmitInfo cbsi { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
    cbsi.commandBuffer = cmd_;
    VkSemaphoreSubmitInfo wait { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    wait.semaphore = glDone_;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
    signal.semaphore = vkReady_;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cbsi;
    if (path_ == GlHandoffPath::ZeroCopy) {
        if (glSignaled_) {
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &wait;
        }
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
    }
    if (GpuStatus s = device_->submit(QueueKind::Compute, { &submit, 1 }, fence_); !s) {
        return s;
    }
    glSignaled_ = false;
    submitted_ = true;
    fenceWaited_ = false;
    pendingUpload_ = true;
    releasedToGl_ = path_ != GlHandoffPath::Readback;
    return {};
}

GpuStatus
GlInterop::upload(unsigned int texture)
{
    if (!pendingUpload_) {
        return fail(VK_ERROR_UNKNOWN, "upload() without a preceding produce()");
    }
    Gl& gl = *gl_;
    while (gl.GetError() != GL_NO_ERROR) {
    }

    if (path_ == GlHandoffPath::ZeroCopy) {
        gl.WaitSemaphoreEXT(glReady_, 1, &glBuffer_, 0, nullptr, nullptr);
    } else if (GpuStatus s = waitProduced(); !s) {
        return s;
    }

    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, glBuffer_);
    if (path_ == GlHandoffPath::Readback) {
        if (GpuStatus s = device_->check(
                vmaInvalidateAllocation(device_->allocator(), readbackAllocation_, 0, VK_WHOLE_SIZE),
                "vmaInvalidateAllocation");
            !s) {
            gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            return s;
        }
        // Orphaning lets the driver hand out fresh storage instead of stalling on the previous upload.
        gl.BufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(size_), nullptr, GL_STREAM_DRAW);
        void* dst = gl.MapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(size_),
                                      GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
        if (!dst) {
            gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            return fail(VK_ERROR_MEMORY_MAP_FAILED, "glMapBufferRange failed");
        }
        std::memcpy(dst, readbackMapped_, size_);
        gl.UnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    }

    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width_), static_cast<GLsizei>(height_), GL_RGBA,
                     GL_FLOAT, nullptr);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    if (path_ == GlHandoffPath::ZeroCopy) {
        gl.SignalSemaphoreEXT(glDoneGl_, 1, &glBuffer_, 0, nullptr, nullptr);
        // Vulkan may not wait on a binary semaphore whose signal has not been submitted yet.
        gl.Flush();
        glSignaled_ = true;
    } else if (path_ == GlHandoffPath::ZeroCopyHostSync) {
        gl.Finish();
    }
    pendingUpload_ = false;
    return glError("upload");
}

} // namespace gpu
