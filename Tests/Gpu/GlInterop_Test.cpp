#include <gtest/gtest.h>

// Ahead of the EGL headers, whose X11 platform header defines None as a macro.
#include "GlInterop.h"
#include "GpuDevice.h"
#include "GpuKernel.h"
#include "GpuTestDevice.h"
#include "fill_spirv.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

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

class GlInteropTest : public gputest::SkippableTest {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
        GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
        if (!egl.init()) {
            if (gputest::envSet("NATRON_TEST_REQUIRE_GL")) {
                FAIL() << "EGL surfaceless unavailable: " << egl.error;
            }
            GPUTEST_SKIP_TEST() << "EGL surfaceless unavailable: " << egl.error;
        }
        s = GlInterop::probe(*dev, eglLoader, caps);
        if (!s.ok()) {
            if (gputest::envSet("NATRON_TEST_REQUIRE_GL")) {
                FAIL() << "GL interop probe failed: " << s.message;
            }
            GPUTEST_SKIP_TEST() << "GL interop probe failed: " << s.message;
        }

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

    GLuint makeBgra8Texture(uint32_t w, uint32_t h, const std::vector<uint8_t>& init)
    {
        GLuint t = 0;
        egl.gl.GenTextures(1, &t);
        egl.gl.BindTexture(GL_TEXTURE_2D, t);
        egl.gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        egl.gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(w), GLsizei(h), 0, GL_BGRA,
                          GL_UNSIGNED_INT_8_8_8_8_REV, init.data());
        textures.push_back(t);
        return t;
    }

    std::vector<uint8_t> readBgra8Texture(GLuint t, uint32_t w, uint32_t h)
    {
        std::vector<uint8_t> out(size_t(w) * h * 4, 0);
        egl.gl.BindTexture(GL_TEXTURE_2D, t);
        egl.gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
        egl.gl.GetTexImage(GL_TEXTURE_2D, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, out.data());
        return out;
    }

    GpuStatus produce(GlInterop& gi, const GlFrame& f, const TestBuffer& src, int& slot,
                      std::span<const TimelineValue> waits = {})
    {
        return gi.produce(f, [&](VkCommandBuffer cb, VkBuffer dst) { return src.copyTo(cb, dst); }, waits, &slot);
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
            GPUTEST_SKIP() << toString(path) << " unavailable on " << driverLabel() << ": missing " << caps.missing;
        }
        constexpr uint32_t w = 509;
        constexpr uint32_t h = 263;
        const GlFrame frame { w, h, GlPixelFormat::Rgba32F };
        std::unique_ptr<GlInterop> gi;
        GpuStatus s = GlInterop::create(*dev, eglLoader, frame.bytes(), path, gi);
        ASSERT_TRUE(s.ok()) << s.message;
        TestBuffer staging;
        ASSERT_TRUE(staging.create(*dev, frame.bytes()).ok());
        const GLuint tex = makeTexture(w, h);

        // The kernel treats the RGBA32F image as one row of w*4 floats per scanline and stores each
        // float's own index, which stays exact in a float for this size.
        std::vector<float> expected(size_t(w) * h * 4);
        for (size_t i = 0; i < expected.size(); ++i) {
            expected[i] = float(i);
        }
        int slot = -1;
        s = gi->produce(
            frame,
            [&](VkCommandBuffer cb, VkBuffer dst) {
                FillParams p { w * 4, h, 1 };
                const VkBuffer bufs[] = { dst };
                return fill->record(cb, bufs, std::as_bytes(std::span(&p, 1)), { w * 4, h, 1 });
            },
            {}, &slot);
        ASSERT_TRUE(s.ok()) << s.message;
        s = gi->upload(slot, tex, 0, 0);
        ASSERT_TRUE(s.ok()) << s.message;
        ASSERT_TRUE(gi->waitProduced(slot).ok());
        fill->releaseDescriptors();
        std::vector<float> kernelOut = readTexture(tex, w, h);
        ASSERT_EQ(egl.gl.GetError(), GLenum(GL_NO_ERROR));
        ASSERT_EQ(0, std::memcmp(expected.data(), kernelOut.data(), expected.size() * sizeof(float)))
            << toString(path) << " kernel-filled frame differs";

        for (uint32_t n = 0; n < 3; ++n) {
            fillPattern(staging.data, w, h, n);
            staging.flush();
            expected.assign(staging.data, staging.data + size_t(w) * h * 4);
            ASSERT_TRUE(produce(*gi, frame, staging, slot).ok());
            s = gi->upload(slot, tex, 0, 0);
            ASSERT_TRUE(s.ok()) << s.message;
            std::vector<float> got = readTexture(tex, w, h);
            ASSERT_EQ(egl.gl.GetError(), GLenum(GL_NO_ERROR));
            ASSERT_EQ(0, std::memcmp(expected.data(), got.data(), expected.size() * sizeof(float)))
                << toString(path) << " frame " << n << " differs";
        }
        std::cout << toString(path) << ": byte-exact over a kernel fill and 3 copied frames on " << driverLabel()
                  << "\n";
    }

    // Uploads several frames back to back with no GL readback in between, so each produce() can
    // only rely on the GL-read-before-Vulkan-write ordering the interop provides.
    void backToBack(GlHandoffPath path)
    {
        if (!caps.supports(path)) {
            GPUTEST_SKIP() << toString(path) << " unavailable on " << driverLabel() << ": missing " << caps.missing;
        }
        constexpr uint32_t w = 1024;
        constexpr uint32_t h = 512;
        constexpr uint32_t kFrames = 8;
        const GlFrame frame { w, h, GlPixelFormat::Rgba32F };
        std::unique_ptr<GlInterop> gi;
        GpuStatus s = GlInterop::create(*dev, eglLoader, frame.bytes(), path, gi);
        ASSERT_TRUE(s.ok()) << s.message;
        std::vector<std::unique_ptr<TestBuffer>> sources;
        std::vector<GLuint> texs;
        for (uint32_t f = 0; f < kFrames; ++f) {
            auto staging = std::make_unique<TestBuffer>();
            ASSERT_TRUE(staging->create(*dev, frame.bytes()).ok());
            fillPattern(staging->data, w, h, 100 + f);
            staging->flush();
            sources.push_back(std::move(staging));
            texs.push_back(makeTexture(w, h));
        }
        int slot = -1;
        for (uint32_t f = 0; f < kFrames; ++f) {
            s = produce(*gi, frame, *sources[f], slot);
            ASSERT_TRUE(s.ok()) << s.message;
            s = gi->upload(slot, texs[f], 0, 0);
            ASSERT_TRUE(s.ok()) << s.message;
        }
        for (int i = 0; i < GlInterop::kSlots; ++i) {
            ASSERT_TRUE(gi->waitProduced(i).ok());
        }
        for (uint32_t f = 0; f < kFrames; ++f) {
            const std::vector<float> got = readTexture(texs[f], w, h);
            ASSERT_EQ(egl.gl.GetError(), GLenum(GL_NO_ERROR));
            EXPECT_EQ(0, std::memcmp(sources[f]->data, got.data(), got.size() * sizeof(float)))
                << toString(path) << " frame " << f << " differs";
        }
    }

    // A host copy is read where the device wrote it, so a slot hands out the same mapped memory every time,
    // holding the texels of its own last frame, and a slot produced without one refuses to give it.
    void hostCopyInPlace(GlHandoffPath path)
    {
        if (!caps.supports(path)) {
            GPUTEST_SKIP() << toString(path) << " unavailable on " << driverLabel() << ": missing " << caps.missing;
        }
        constexpr uint32_t w = 300;
        constexpr uint32_t h = 170;
        const GlFrame frame { w, h, GlPixelFormat::Rgba32F };
        std::unique_ptr<GlInterop> gi;
        GpuStatus s = GlInterop::create(*dev, eglLoader, frame.bytes(), path, gi);
        ASSERT_TRUE(s.ok()) << s.message;
        const GLuint tex = makeTexture(w, h);
        std::vector<std::unique_ptr<TestBuffer>> sources;
        for (uint32_t f = 0; f < uint32_t(2 * GlInterop::kSlots); ++f) {
            auto staging = std::make_unique<TestBuffer>();
            ASSERT_TRUE(staging->create(*dev, frame.bytes()).ok());
            fillPattern(staging->data, w, h, 300 + f);
            staging->flush();
            sources.push_back(std::move(staging));
        }
        std::array<const unsigned char*, GlInterop::kSlots> firstView {};
        for (uint32_t f = 0; f < sources.size(); ++f) {
            int slot = -1;
            s = gi->produce(
                frame, [&](VkCommandBuffer cb, VkBuffer dst) { return sources[f]->copyTo(cb, dst); }, {}, &slot, true);
            ASSERT_TRUE(s.ok()) << s.message;
            s = gi->upload(slot, tex, 0, 0);
            ASSERT_TRUE(s.ok()) << s.message;
            ASSERT_TRUE(gi->waitProduced(slot).ok());
            const unsigned char* data = nullptr;
            VkDeviceSize size = 0;
            s = gi->hostCopyView(slot, &data, &size);
            ASSERT_TRUE(s.ok()) << toString(path) << ": " << s.message;
            ASSERT_EQ(frame.bytes(), size) << toString(path);
            EXPECT_EQ(0, std::memcmp(sources[f]->data, data, size)) << toString(path) << " frame " << f << " differs";
            if (!firstView[slot]) {
                firstView[slot] = data;
            }
            EXPECT_EQ(firstView[slot], data) << toString(path) << " slot " << slot << " moved its host copy";
        }

        int slot = -1;
        ASSERT_TRUE(produce(*gi, frame, *sources[0], slot).ok());
        ASSERT_TRUE(gi->upload(slot, tex, 0, 0).ok());
        ASSERT_TRUE(gi->waitProduced(slot).ok());
        const unsigned char* data = nullptr;
        VkDeviceSize size = 0;
        if (path == GlHandoffPath::Readback) {
            EXPECT_TRUE(gi->hostCopyView(slot, &data, &size).ok()) << "the readback path always has a host copy";
        } else {
            EXPECT_FALSE(gi->hostCopyView(slot, &data, &size).ok()) << toString(path);
        }
    }

    // Produces frame f+1 into the other slot before frame f is uploaded, with frame sizes that change
    // every produce, so each upload must read only its own slot. One produce also waits on a host-
    // signalled timeline value, with its source rewritten between the submit and the signal.
    void produceAhead(GlHandoffPath path)
    {
        if (!caps.supports(path)) {
            GPUTEST_SKIP() << toString(path) << " unavailable on " << driverLabel() << ": missing " << caps.missing;
        }
        constexpr uint32_t kFrames = 6;
        constexpr uint32_t kGated = 3;
        std::vector<GlFrame> frames;
        for (uint32_t f = 0; f < kFrames; ++f) {
            frames.push_back({ 640 - f * 37, 360 - f * 11, GlPixelFormat::Rgba32F });
        }
        std::unique_ptr<GlInterop> gi;
        GpuStatus s = GlInterop::create(*dev, eglLoader, frames[0].bytes(), path, gi);
        ASSERT_TRUE(s.ok()) << s.message;

        std::vector<std::unique_ptr<TestBuffer>> sources;
        std::vector<GLuint> texs;
        for (uint32_t f = 0; f < kFrames; ++f) {
            auto staging = std::make_unique<TestBuffer>();
            ASSERT_TRUE(staging->create(*dev, frames[f].bytes()).ok());
            fillPattern(staging->data, frames[f].width, frames[f].height, 200 + f);
            staging->flush();
            sources.push_back(std::move(staging));
            texs.push_back(makeTexture(frames[f].width, frames[f].height));
        }

        VkSemaphore gate = VK_NULL_HANDLE;
        ASSERT_TRUE(dev->createTimelineSemaphore(0, gate).ok());
        // A failed assertion must not leave a submission waiting on the gate forever, nor destroy the
        // gate while one still waits on it.
        struct GateGuard {
            VkDevice vk;
            VkSemaphore sem;
            GlInterop* gi;
            ~GateGuard()
            {
                uint64_t value = 0;
                vkGetSemaphoreCounterValue(vk, sem, &value);
                if (value < 1) {
                    VkSemaphoreSignalInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
                    si.semaphore = sem;
                    si.value = 1;
                    vkSignalSemaphore(vk, &si);
                }
                for (int i = 0; i < GlInterop::kSlots; ++i) {
                    gi->waitProduced(i);
                }
                vkDestroySemaphore(vk, sem, nullptr);
            }
        };
        GateGuard gateGuard { dev->device(), gate, gi.get() };

        int pending = -1;
        s = produce(*gi, frames[0], *sources[0], pending);
        ASSERT_TRUE(s.ok()) << s.message;
        for (uint32_t f = 1; f <= kFrames; ++f) {
            int next = -1;
            if (f < kFrames) {
                const TimelineValue wait { gate, 1 };
                const bool gated = f == kGated;
                s = produce(*gi, frames[f], *sources[f], next,
                            gated ? std::span<const TimelineValue>(&wait, 1) : std::span<const TimelineValue>());
                ASSERT_TRUE(s.ok()) << s.message;
                ASSERT_NE(next, pending) << "frame " << f << " reused the slot frame " << f - 1 << " awaits upload in";
                EXPECT_EQ(gi->frame(next).width, frames[f].width);

                bool recorded = false;
                int none = -1;
                s = gi->produce(
                    frames[f],
                    [&](VkCommandBuffer, VkBuffer) {
                        recorded = true;
                        return GpuStatus {};
                    },
                    {}, &none);
                EXPECT_FALSE(s.ok()) << "a third produce with both slots awaiting upload must be refused";
                EXPECT_FALSE(recorded);
                EXPECT_EQ(none, -1);

                if (gated) {
                    fillPattern(sources[f]->data, frames[f].width, frames[f].height, 900 + f);
                    sources[f]->flush();
                    VkSemaphoreSignalInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
                    si.semaphore = gate;
                    si.value = 1;
                    ASSERT_EQ(vkSignalSemaphore(dev->device(), &si), VK_SUCCESS);
                }
            }
            s = gi->upload(pending, texs[f - 1], 0, 0);
            ASSERT_TRUE(s.ok()) << s.message;
            pending = next;
        }
        for (int i = 0; i < GlInterop::kSlots; ++i) {
            ASSERT_TRUE(gi->waitProduced(i).ok());
        }
        for (uint32_t f = 0; f < kFrames; ++f) {
            const std::vector<float> got = readTexture(texs[f], frames[f].width, frames[f].height);
            ASSERT_EQ(egl.gl.GetError(), GLenum(GL_NO_ERROR));
            EXPECT_EQ(0, std::memcmp(sources[f]->data, got.data(), got.size() * sizeof(float)))
                << toString(path) << " frame " << f << " (" << frames[f].width << "x" << frames[f].height
                << ") differs";
        }
        std::cout << toString(path) << ": byte-exact over " << kFrames << " produced-ahead frames on "
                  << driverLabel() << "\n";
    }

    // Uploads BGRA8 frames of two sizes into sub-rectangles of one larger texture, checks the pixels
    // around them are untouched, and checks frames beyond the slot capacity are refused unrecorded.
    void bgra8SubRect(GlHandoffPath path)
    {
        if (!caps.supports(path)) {
            GPUTEST_SKIP() << toString(path) << " unavailable on " << driverLabel() << ": missing " << caps.missing;
        }
        constexpr uint32_t texW = 300;
        constexpr uint32_t texH = 190;
        struct Placement {
            GlFrame frame;
            int x;
            int y;
        };
        const Placement placements[] = {
            { { 131, 97, GlPixelFormat::Bgra8 }, 37, 23 },
            { { 59, 41, GlPixelFormat::Bgra8 }, 211, 140 },
        };
        const VkDeviceSize capacity = placements[0].frame.bytes();
        std::unique_ptr<GlInterop> gi;
        GpuStatus s = GlInterop::create(*dev, eglLoader, capacity, path, gi);
        ASSERT_TRUE(s.ok()) << s.message;
        ASSERT_EQ(gi->capacity(), capacity);

        for (const GlFrame& tooBig : { GlFrame { 131, 97, GlPixelFormat::Rgba32F },
                                       GlFrame { 132, 97, GlPixelFormat::Bgra8 },
                                       GlFrame { 131, 98, GlPixelFormat::Bgra8 },
                                       GlFrame { 0xffffffffu, 0xffffffffu, GlPixelFormat::Rgba32F } }) {
            bool recorded = false;
            int slot = 7;
            s = gi->produce(
                tooBig,
                [&](VkCommandBuffer, VkBuffer) {
                    recorded = true;
                    return GpuStatus {};
                },
                {}, &slot);
            EXPECT_EQ(s.result, VK_ERROR_OUT_OF_POOL_MEMORY)
                << tooBig.width << "x" << tooBig.height << " into " << capacity << " bytes: " << s.message;
            EXPECT_FALSE(recorded);
            EXPECT_EQ(slot, -1);
        }

        constexpr uint8_t kSentinel = 0x5a;
        std::vector<uint8_t> expected(size_t(texW) * texH * 4, kSentinel);
        const GLuint tex = makeBgra8Texture(texW, texH, expected);
        std::vector<std::unique_ptr<TestBuffer>> sources;
        for (size_t i = 0; i < std::size(placements); ++i) {
            const Placement& p = placements[i];
            auto src = std::make_unique<TestBuffer>();
            ASSERT_TRUE(src->create(*dev, p.frame.bytes()).ok());
            auto* bytes = reinterpret_cast<uint8_t*>(src->data);
            for (uint32_t y = 0; y < p.frame.height; ++y) {
                for (uint32_t x = 0; x < p.frame.width; ++x) {
                    for (uint32_t c = 0; c < 4; ++c) {
                        const uint8_t v = uint8_t((x * 7u + y * 13u + c * 61u + i * 97u) & 255u);
                        bytes[(size_t(y) * p.frame.width + x) * 4 + c] = v;
                        expected[((size_t(p.y) + y) * texW + p.x + x) * 4 + c] = v;
                    }
                }
            }
            src->flush();
            sources.push_back(std::move(src));
        }
        for (size_t i = 0; i < std::size(placements); ++i) {
            int slot = -1;
            s = produce(*gi, placements[i].frame, *sources[i], slot);
            ASSERT_TRUE(s.ok()) << s.message;
            s = gi->upload(slot, tex, placements[i].x, placements[i].y);
            ASSERT_TRUE(s.ok()) << s.message;
        }
        for (int i = 0; i < GlInterop::kSlots; ++i) {
            ASSERT_TRUE(gi->waitProduced(i).ok());
        }
        const std::vector<uint8_t> got = readBgra8Texture(tex, texW, texH);
        ASSERT_EQ(egl.gl.GetError(), GLenum(GL_NO_ERROR));
        size_t firstDiff = got.size();
        for (size_t i = 0; i < got.size(); ++i) {
            if (got[i] != expected[i]) {
                firstDiff = i;
                break;
            }
        }
        EXPECT_EQ(firstDiff, got.size()) << toString(path) << " texel (" << (firstDiff / 4) % texW << ", "
                                         << (firstDiff / 4) / texW << ") differs";
        std::cout << toString(path) << ": byte-exact BGRA8 sub-rectangles and capacity refusal on " << driverLabel()
                  << "\n";
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

GPU_TEST_F(GlInteropTest, Probe)
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

GPU_TEST_F(GlInteropTest, ZeroCopyByteExact)
{
    roundTrip(GlHandoffPath::ZeroCopy);
}

GPU_TEST_F(GlInteropTest, ZeroCopyHostSyncByteExact)
{
    roundTrip(GlHandoffPath::ZeroCopyHostSync);
}

GPU_TEST_F(GlInteropTest, ReadbackByteExact)
{
    roundTrip(GlHandoffPath::Readback);
}

GPU_TEST_F(GlInteropTest, ZeroCopyBackToBack)
{
    backToBack(GlHandoffPath::ZeroCopy);
}

GPU_TEST_F(GlInteropTest, ZeroCopyHostSyncBackToBack)
{
    backToBack(GlHandoffPath::ZeroCopyHostSync);
}

GPU_TEST_F(GlInteropTest, ReadbackBackToBack)
{
    backToBack(GlHandoffPath::Readback);
}

GPU_TEST_F(GlInteropTest, ZeroCopyDoubleBufferedProduceAheadByteExact)
{
    produceAhead(GlHandoffPath::ZeroCopy);
}

GPU_TEST_F(GlInteropTest, ZeroCopyHostSyncDoubleBufferedProduceAheadByteExact)
{
    produceAhead(GlHandoffPath::ZeroCopyHostSync);
}

GPU_TEST_F(GlInteropTest, ReadbackDoubleBufferedProduceAheadByteExact)
{
    produceAhead(GlHandoffPath::Readback);
}

GPU_TEST_F(GlInteropTest, ZeroCopyBgra8SubRectByteExact)
{
    bgra8SubRect(GlHandoffPath::ZeroCopy);
}

GPU_TEST_F(GlInteropTest, ZeroCopyHostSyncBgra8SubRectByteExact)
{
    bgra8SubRect(GlHandoffPath::ZeroCopyHostSync);
}

GPU_TEST_F(GlInteropTest, ReadbackBgra8SubRectByteExact)
{
    bgra8SubRect(GlHandoffPath::Readback);
}

GPU_TEST_F(GlInteropTest, ZeroCopyHostCopyInPlace)
{
    hostCopyInPlace(GlHandoffPath::ZeroCopy);
}

GPU_TEST_F(GlInteropTest, ZeroCopyHostSyncHostCopyInPlace)
{
    hostCopyInPlace(GlHandoffPath::ZeroCopyHostSync);
}

GPU_TEST_F(GlInteropTest, ReadbackHostCopyInPlace)
{
    hostCopyInPlace(GlHandoffPath::Readback);
}

GPU_TEST_F(GlInteropTest, UhdLatency)
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
        const GlFrame frame { w, h, GlPixelFormat::Rgba32F };
        std::unique_ptr<GlInterop> gi;
        GpuStatus s = GlInterop::create(*dev, eglLoader, frame.bytes(), path, gi);
        ASSERT_TRUE(s.ok()) << s.message;
        TestBuffer staging;
        ASSERT_TRUE(staging.create(*dev, frame.bytes()).ok());
        fillPattern(staging.data, w, h, 1);
        staging.flush();
        // A device-local source keeps the producer a VRAM copy, as a compute kernel's output would be.
        TestBuffer source;
        ASSERT_TRUE(source.createDeviceLocal(*dev, frame.bytes()).ok());
        int slot = -1;
        ASSERT_TRUE(gi->produce(
                          frame,
                          [&](VkCommandBuffer cb, VkBuffer dst) {
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
                          return source.copyTo(cb, dst);
                      },
                          {}, &slot)
                        .ok());
        ASSERT_TRUE(gi->upload(slot, tex, 0, 0).ok());

        std::vector<double> produceMs;
        std::vector<double> handoff;
        std::vector<double> endToEnd;
        for (int i = -2; i < iterations; ++i) {
            const auto t0 = clock::now();
            ASSERT_TRUE(produce(*gi, frame, source, slot).ok());
            ASSERT_TRUE(gi->waitProduced(slot).ok());
            const auto t1 = clock::now();
            s = gi->upload(slot, tex, 0, 0);
            ASSERT_TRUE(s.ok()) << s.message;
            egl.gl.Finish();
            const auto t2 = clock::now();
            if (i >= 0) {
                produceMs.push_back(ms(t1 - t0));
                handoff.push_back(ms(t2 - t1));
            }
        }
        for (int i = -2; i < iterations; ++i) {
            const auto t0 = clock::now();
            ASSERT_TRUE(produce(*gi, frame, source, slot).ok());
            ASSERT_TRUE(gi->upload(slot, tex, 0, 0).ok());
            egl.gl.Finish();
            if (i >= 0) {
                endToEnd.push_back(ms(clock::now() - t0));
            }
        }
        std::vector<float> got = readTexture(tex, w, h);
        EXPECT_EQ(0, std::memcmp(staging.data, got.data(), got.size() * sizeof(float))) << toString(path);
        std::printf("%-20s %12.2f %12.2f %12.2f\n", toString(path), median(produceMs), median(handoff),
                    median(endToEnd));
    }
}
