#include <gtest/gtest.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "GlInterop.h"
#include "GpuDevice.h"
#include "GpuKernel.h"
#include "GpuTestDevice.h"
#include "fill_spirv.h"

using namespace gpu;

namespace {

struct FillParams {
    uint32_t width;
    uint32_t height;
    uint32_t asFloat;
};

struct GlFns {
    decltype(&::glGenTextures) GenTextures = nullptr;
    decltype(&::glDeleteTextures) DeleteTextures = nullptr;
    decltype(&::glBindTexture) BindTexture = nullptr;
    decltype(&::glTexImage2D) TexImage2D = nullptr;
    decltype(&::glGetTexImage) GetTexImage = nullptr;
    decltype(&::glPixelStorei) PixelStorei = nullptr;
    decltype(&::glFinish) Finish = nullptr;
    decltype(&::glGetError) GetError = nullptr;
};

void*
eglLoader(const char* name)
{
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

class EglSurfaceless {
public:
    std::string error;
    GlFns gl;

    bool init()
    {
        const char* clientExts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        if (!clientExts || !std::strstr(clientExts, "EGL_MESA_platform_surfaceless")) {
            error = "EGL_MESA_platform_surfaceless missing";
            return false;
        }
        auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (!getPlatformDisplay) {
            error = "eglGetPlatformDisplayEXT missing";
            return false;
        }
        dpy_ = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (dpy_ == EGL_NO_DISPLAY || !eglInitialize(dpy_, nullptr, nullptr)) {
            error = "eglInitialize(surfaceless) failed";
            dpy_ = EGL_NO_DISPLAY;
            return false;
        }
        const char* exts = eglQueryString(dpy_, EGL_EXTENSIONS);
        if (!exts || !std::strstr(exts, "EGL_KHR_surfaceless_context")
            || !std::strstr(exts, "EGL_KHR_no_config_context")) {
            error = "EGL_KHR_surfaceless_context or EGL_KHR_no_config_context missing";
            return false;
        }
        if (!eglBindAPI(EGL_OPENGL_API)) {
            error = "eglBindAPI(EGL_OPENGL_API) failed";
            return false;
        }
        ctx_ = eglCreateContext(dpy_, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, nullptr);
        if (ctx_ == EGL_NO_CONTEXT) {
            error = "eglCreateContext failed";
            return false;
        }
        if (!eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_)) {
            error = "eglMakeCurrent failed";
            return false;
        }
        auto get = [](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(eglGetProcAddress(name));
        };
        get(gl.GenTextures, "glGenTextures");
        get(gl.DeleteTextures, "glDeleteTextures");
        get(gl.BindTexture, "glBindTexture");
        get(gl.TexImage2D, "glTexImage2D");
        get(gl.GetTexImage, "glGetTexImage");
        get(gl.PixelStorei, "glPixelStorei");
        get(gl.Finish, "glFinish");
        get(gl.GetError, "glGetError");
        if (!gl.GenTextures || !gl.TexImage2D || !gl.GetTexImage || !gl.Finish) {
            error = "GL entry points unavailable";
            return false;
        }
        return true;
    }

    ~EglSurfaceless()
    {
        if (dpy_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (ctx_ != EGL_NO_CONTEXT) {
                eglDestroyContext(dpy_, ctx_);
            }
            eglTerminate(dpy_);
        }
    }

private:
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    EGLContext ctx_ = EGL_NO_CONTEXT;
};

void
fillPattern(float* px, uint32_t w, uint32_t h, uint32_t seed)
{
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float* p = px + (size_t(y) * w + x) * 4;
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t v = (x * 7u + y * 13u + c * 61u + seed * 97u) & 255u;
                p[c] = float(v) / 255.0f + float(x + c) * 1e-3f - float(y) * 1e-5f;
            }
        }
    }
}

class TestBuffer {
public:
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    float* data = nullptr;
    VkDeviceSize size = 0;

    GpuStatus create(GpuDevice& dev, VkDeviceSize bytes)
    {
        dev_ = &dev;
        size = bytes;
        VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo ai {};
        GpuStatus s = dev.check(vmaCreateBuffer(dev.allocator(), &bci, &aci, &buffer, &alloc, &ai), "staging");
        data = static_cast<float*>(ai.pMappedData);
        return s;
    }

    void flush() { vmaFlushAllocation(dev_->allocator(), alloc, 0, VK_WHOLE_SIZE); }

    GpuStatus createDeviceLocal(GpuDevice& dev, VkDeviceSize bytes)
    {
        dev_ = &dev;
        size = bytes;
        VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        return dev.check(vmaCreateBuffer(dev.allocator(), &bci, &aci, &buffer, &alloc, nullptr), "device source");
    }

    GpuStatus copyTo(VkCommandBuffer cb, VkBuffer dst) const
    {
        VkBufferCopy region { 0, 0, size };
        vkCmdCopyBuffer(cb, buffer, dst, 1, &region);
        return {};
    }

    ~TestBuffer()
    {
        if (buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(dev_->allocator(), buffer, alloc);
        }
    }

private:
    GpuDevice* dev_ = nullptr;
};

class GlInteropTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
        ASSERT_TRUE(s.ok()) << s.message;
        if (!egl.init()) {
            GTEST_SKIP() << "EGL surfaceless unavailable: " << egl.error;
        }
        s = GlInterop::probe(*dev, eglLoader, caps);
        ASSERT_TRUE(s.ok()) << s.message;

        GpuKernelDesc d;
        d.spirv = fill_spirv;
        d.spirvWords = fill_spirv_words;
        d.storageBufferCount = 1;
        d.pushConstantBytes = sizeof(FillParams);
        d.groupSize = { fill_group_size[0], fill_group_size[1], fill_group_size[2] };
        s = GpuKernel::create(*dev, d, fill);
        ASSERT_TRUE(s.ok()) << s.message;
    }

    std::string driverLabel() const
    {
        return "vk " + dev->info().name + " / gl " + caps.glRenderer;
    }

    void TearDown() override
    {
        for (GLuint t : textures) {
            egl.gl.DeleteTextures(1, &t);
        }
        fill.reset();
        dev.reset();
        check.expectClean("the test");
    }

    GLuint makeTexture(uint32_t w, uint32_t h)
    {
        GLuint t = 0;
        egl.gl.GenTextures(1, &t);
        egl.gl.BindTexture(GL_TEXTURE_2D, t);
        egl.gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_FLOAT, nullptr);
        textures.push_back(t);
        return t;
    }

    std::vector<float> readTexture(GLuint t, uint32_t w, uint32_t h)
    {
        std::vector<float> out(size_t(w) * h * 4, -1.0f);
        egl.gl.BindTexture(GL_TEXTURE_2D, t);
        egl.gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
        egl.gl.GetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, out.data());
        return out;
    }

    void roundTrip(GlHandoffPath path)
    {
        if (!caps.supports(path)) {
            GTEST_SKIP() << toString(path) << " unavailable on " << driverLabel() << ": missing " << caps.missing;
        }
        constexpr uint32_t w = 509;
        constexpr uint32_t h = 263;
        std::unique_ptr<GlInterop> gi;
        GpuStatus s = GlInterop::create(*dev, eglLoader, w, h, path, gi);
        ASSERT_TRUE(s.ok()) << s.message;
        TestBuffer staging;
        ASSERT_TRUE(staging.create(*dev, gi->size()).ok());
        const GLuint tex = makeTexture(w, h);

        // The kernel treats the RGBA32F image as one row of w*4 floats per scanline and stores each
        // float's own index, which stays exact in a float for this size.
        std::vector<float> expected(size_t(w) * h * 4);
        for (size_t i = 0; i < expected.size(); ++i) {
            expected[i] = float(i);
        }
        s = gi->produce([&](VkCommandBuffer cb) {
            FillParams p { w * 4, h, 1 };
            const VkBuffer bufs[] = { gi->buffer() };
            return fill->record(cb, bufs, std::as_bytes(std::span(&p, 1)), { w * 4, h, 1 });
        });
        ASSERT_TRUE(s.ok()) << s.message;
        s = gi->upload(tex);
        ASSERT_TRUE(s.ok()) << s.message;
        ASSERT_TRUE(gi->waitProduced().ok());
        fill->releaseDescriptors();
        std::vector<float> kernelOut = readTexture(tex, w, h);
        ASSERT_EQ(egl.gl.GetError(), GLenum(GL_NO_ERROR));
        ASSERT_EQ(0, std::memcmp(expected.data(), kernelOut.data(), expected.size() * sizeof(float)))
            << toString(path) << " kernel-filled frame differs";

        for (uint32_t frame = 0; frame < 3; ++frame) {
            fillPattern(staging.data, w, h, frame);
            staging.flush();
            expected.assign(staging.data, staging.data + size_t(w) * h * 4);
            ASSERT_TRUE(gi->produce([&](VkCommandBuffer cb) { return staging.copyTo(cb, gi->buffer()); }).ok());
            s = gi->upload(tex);
            ASSERT_TRUE(s.ok()) << s.message;
            std::vector<float> got = readTexture(tex, w, h);
            ASSERT_EQ(egl.gl.GetError(), GLenum(GL_NO_ERROR));
            ASSERT_EQ(0, std::memcmp(expected.data(), got.data(), expected.size() * sizeof(float)))
                << toString(path) << " frame " << frame << " differs";
        }
        std::cout << toString(path) << ": byte-exact over a kernel fill and 3 copied frames on " << driverLabel()
                  << "\n";
    }

    // Uploads several frames back to back with no GL readback in between, so each produce() can
    // only rely on the GL-read-before-Vulkan-write ordering the interop provides.
    void backToBack(GlHandoffPath path)
    {
        if (!caps.supports(path)) {
            GTEST_SKIP() << toString(path) << " unavailable on " << driverLabel() << ": missing " << caps.missing;
        }
        constexpr uint32_t w = 1024;
        constexpr uint32_t h = 512;
        constexpr uint32_t kFrames = 8;
        std::unique_ptr<GlInterop> gi;
        GpuStatus s = GlInterop::create(*dev, eglLoader, w, h, path, gi);
        ASSERT_TRUE(s.ok()) << s.message;
        std::vector<std::unique_ptr<TestBuffer>> sources;
        std::vector<GLuint> texs;
        for (uint32_t f = 0; f < kFrames; ++f) {
            auto staging = std::make_unique<TestBuffer>();
            ASSERT_TRUE(staging->create(*dev, gi->size()).ok());
            fillPattern(staging->data, w, h, 100 + f);
            staging->flush();
            sources.push_back(std::move(staging));
            texs.push_back(makeTexture(w, h));
        }
        for (uint32_t f = 0; f < kFrames; ++f) {
            s = gi->produce([&](VkCommandBuffer cb) { return sources[f]->copyTo(cb, gi->buffer()); });
            ASSERT_TRUE(s.ok()) << s.message;
            s = gi->upload(texs[f]);
            ASSERT_TRUE(s.ok()) << s.message;
        }
        ASSERT_TRUE(gi->waitProduced().ok());
        for (uint32_t f = 0; f < kFrames; ++f) {
            const std::vector<float> got = readTexture(texs[f], w, h);
            ASSERT_EQ(egl.gl.GetError(), GLenum(GL_NO_ERROR));
            EXPECT_EQ(0, std::memcmp(sources[f]->data, got.data(), got.size() * sizeof(float)))
                << toString(path) << " frame " << f << " differs";
        }
    }

    gputest::ValidationCheck check;
    std::unique_ptr<GpuDevice> dev;
    std::unique_ptr<GpuKernel> fill;
    EglSurfaceless egl;
    GlInteropCaps caps;
    std::vector<GLuint> textures;
};

double
median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

} // namespace

TEST_F(GlInteropTest, Probe)
{
    std::cout << "driver: " << driverLabel() << "\n"
              << "gl: " << caps.glVendor << " | " << caps.glRenderer << " | " << caps.glVersion << "\n"
              << "vkMemFd=" << caps.vkExternalMemoryFd << " vkBufExport=" << caps.vkBufferExportable
              << " vkSemFd=" << caps.vkExternalSemaphoreFd << " vkSemExport=" << caps.vkSemaphoreExportable
              << " glMemObj=" << caps.glMemoryObject << " glMemObjFd=" << caps.glMemoryObjectFd
              << " glSem=" << caps.glSemaphore << " glSemFd=" << caps.glSemaphoreFd
              << " uuidMatch=" << caps.uuidMatch << "\n"
              << "best path: " << toString(caps.best) << "\n"
              << "missing: " << (caps.missing.empty() ? "(none)" : caps.missing) << "\n";
}

TEST_F(GlInteropTest, ZeroCopyByteExact)
{
    roundTrip(GlHandoffPath::ZeroCopy);
}

TEST_F(GlInteropTest, ZeroCopyHostSyncByteExact)
{
    roundTrip(GlHandoffPath::ZeroCopyHostSync);
}

TEST_F(GlInteropTest, ReadbackByteExact)
{
    roundTrip(GlHandoffPath::Readback);
}

TEST_F(GlInteropTest, ZeroCopyBackToBack)
{
    backToBack(GlHandoffPath::ZeroCopy);
}

TEST_F(GlInteropTest, ZeroCopyHostSyncBackToBack)
{
    backToBack(GlHandoffPath::ZeroCopyHostSync);
}

TEST_F(GlInteropTest, ReadbackBackToBack)
{
    backToBack(GlHandoffPath::Readback);
}

TEST_F(GlInteropTest, UhdLatency)
{
    constexpr uint32_t w = 3840;
    constexpr uint32_t h = 2160;
    const int iterations = std::getenv("GL_INTEROP_ITERS") ? std::atoi(std::getenv("GL_INTEROP_ITERS")) : 10;
    const GLuint tex = makeTexture(w, h);
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };

    std::printf("UHD RGBA32F (%.1f MB), %d iterations, median ms, on %s\n", w * h * 16 / 1e6, iterations,
                driverLabel().c_str());
    std::printf("%-20s %12s %12s %12s\n", "path", "produce", "handoff", "end-to-end");
    for (GlHandoffPath path : { GlHandoffPath::ZeroCopy, GlHandoffPath::ZeroCopyHostSync, GlHandoffPath::Readback }) {
        if (!caps.supports(path)) {
            std::printf("%-20s skipped: missing %s\n", toString(path), caps.missing.c_str());
            continue;
        }
        std::unique_ptr<GlInterop> gi;
        GpuStatus s = GlInterop::create(*dev, eglLoader, w, h, path, gi);
        ASSERT_TRUE(s.ok()) << s.message;
        TestBuffer staging;
        ASSERT_TRUE(staging.create(*dev, gi->size()).ok());
        fillPattern(staging.data, w, h, 1);
        staging.flush();
        // A device-local source keeps the producer a VRAM copy, as a compute kernel's output would be.
        TestBuffer source;
        ASSERT_TRUE(source.createDeviceLocal(*dev, gi->size()).ok());
        ASSERT_TRUE(gi->produce([&](VkCommandBuffer cb) {
                          staging.copyTo(cb, source.buffer);
                          VkMemoryBarrier2 mb { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
                          mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                          mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                          mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                          mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                          VkDependencyInfo dep { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
                          dep.memoryBarrierCount = 1;
                          dep.pMemoryBarriers = &mb;
                          vkCmdPipelineBarrier2(cb, &dep);
                          return source.copyTo(cb, gi->buffer());
                      }).ok());
        ASSERT_TRUE(gi->upload(tex).ok());

        std::vector<double> produce;
        std::vector<double> handoff;
        std::vector<double> endToEnd;
        for (int i = -2; i < iterations; ++i) {
            const auto t0 = clock::now();
            ASSERT_TRUE(gi->produce([&](VkCommandBuffer cb) { return source.copyTo(cb, gi->buffer()); }).ok());
            ASSERT_TRUE(gi->waitProduced().ok());
            const auto t1 = clock::now();
            s = gi->upload(tex);
            ASSERT_TRUE(s.ok()) << s.message;
            egl.gl.Finish();
            const auto t2 = clock::now();
            if (i >= 0) {
                produce.push_back(ms(t1 - t0));
                handoff.push_back(ms(t2 - t1));
            }
        }
        for (int i = -2; i < iterations; ++i) {
            const auto t0 = clock::now();
            ASSERT_TRUE(gi->produce([&](VkCommandBuffer cb) { return source.copyTo(cb, gi->buffer()); }).ok());
            ASSERT_TRUE(gi->upload(tex).ok());
            egl.gl.Finish();
            if (i >= 0) {
                endToEnd.push_back(ms(clock::now() - t0));
            }
        }
        std::vector<float> got = readTexture(tex, w, h);
        EXPECT_EQ(0, std::memcmp(staging.data, got.data(), got.size() * sizeof(float))) << toString(path);
        std::printf("%-20s %12.2f %12.2f %12.2f\n", toString(path), median(produce), median(handoff),
                    median(endToEnd));
    }
}
