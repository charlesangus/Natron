#include <QApplication>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>
#include <QSurfaceFormat>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "GlInterop.h"
#include "GpuDevice.h"

using namespace gpu;

namespace {

constexpr int kW = 256;
constexpr int kH = 192;
constexpr GlFrame kFrame { kW, kH, GlPixelFormat::Rgba32F };

uint8_t
patternByte(int x, int y, int c)
{
    return c == 3 ? 255 : uint8_t((x * 7 + y * 13 + c * 61) & 255);
}

uint64_t
fnv1a(const void* data, size_t n)
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

std::string
uuidHex(const std::array<uint8_t, 16>& u)
{
    char buf[33];
    for (size_t i = 0; i < u.size(); ++i) {
        std::snprintf(buf + 2 * i, 3, "%02x", u[i]);
    }
    return buf;
}

const char*
profileName(QSurfaceFormat::OpenGLContextProfile p)
{
    switch (p) {
    case QSurfaceFormat::CoreProfile:
        return "core";
    case QSurfaceFormat::CompatibilityProfile:
        return "compatibility";
    default:
        return "none (driver default)";
    }
}

void
printCaps(const GlInteropCaps& c, const QSurfaceFormat& f)
{
    const auto yn = [](bool b) { return b ? "yes" : "no"; };
    std::printf("qt platform: %s\n", QGuiApplication::platformName().toUtf8().constData());
    std::printf("gl context: %d.%d %s profile\n", f.majorVersion(), f.minorVersion(), profileName(f.profile()));
    std::printf("gl vendor: %s\ngl renderer: %s\ngl version: %s\n", c.glVendor.c_str(), c.glRenderer.c_str(),
                c.glVersion.c_str());
    std::printf("vk: external memory fd %s, external semaphore fd %s, buffer exportable %s, semaphore exportable %s\n",
                yn(c.vkExternalMemoryFd), yn(c.vkExternalSemaphoreFd), yn(c.vkBufferExportable),
                yn(c.vkSemaphoreExportable));
    std::printf("gl: memory object %s, memory object fd %s, semaphore %s, semaphore fd %s\n", yn(c.glMemoryObject),
                yn(c.glMemoryObjectFd), yn(c.glSemaphore), yn(c.glSemaphoreFd));
    std::printf("uuid: vk device %s driver %s\n      gl device %s driver %s\n      match %s\n",
                uuidHex(c.vkDeviceUuid).c_str(), uuidHex(c.vkDriverUuid).c_str(), uuidHex(c.glDeviceUuid).c_str(),
                uuidHex(c.glDriverUuid).c_str(), yn(c.uuidMatch));
    for (GlHandoffPath p : { GlHandoffPath::ZeroCopy, GlHandoffPath::ZeroCopyHostSync, GlHandoffPath::Readback }) {
        std::printf("path %s: %s\n", toString(p), c.supports(p) ? "available" : "unavailable");
    }
    std::printf("best: %s\nmissing: %s\n", toString(c.best), c.missing.empty() ? "(none)" : c.missing.c_str());
}

class InteropWidget : public QOpenGLWidget, protected QOpenGLExtraFunctions {
public:
    std::string error;
    std::string pathName;
    std::string renderer;
    bool painted = false;
    bool textureExact = false;
    uint64_t textureSum = 0;
    uint64_t expectedTextureSum = 0;
    double handoffMs = 0;

    ~InteropWidget() override
    {
        makeCurrent();
        interop_.reset();
        if (tex_) {
            glDeleteTextures(1, &tex_);
        }
        if (fbo_) {
            glDeleteFramebuffers(1, &fbo_);
        }
        if (staging_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(dev_->allocator(), staging_, stagingAlloc_);
        }
        doneCurrent();
    }

protected:
    void initializeGL() override
    {
        initializeOpenGLFunctions();
        GpuDeviceOptions o;
        o.enableValidation = std::getenv("NATRON_GPU_NO_VALIDATION") == nullptr;
        GpuStatus s = GpuDevice::create(o, dev_);
        if (!s) {
            error = "GpuDevice: " + s.message;
            return;
        }
        QOpenGLContext* ctx = context();
        auto loader = [ctx](const char* name) { return reinterpret_cast<void*>(ctx->getProcAddress(name)); };
        GlInteropCaps caps;
        if (s = GlInterop::probe(*dev_, loader, caps); !s) {
            error = "probe: " + s.message;
            return;
        }
        renderer = "vk " + dev_->info().name + " / gl " + caps.glRenderer + " (" + caps.glVersion + ")";
        pathName = toString(caps.best);
        printCaps(caps, format());
        if (s = GlInterop::create(*dev_, loader, kFrame.bytes(), caps.best, interop_); !s) {
            error = "create: " + s.message;
            return;
        }

        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kW, kH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glGenFramebuffers(1, &fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            error = "RGBA32F framebuffer incomplete";
        }
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());

        VkBufferCreateInfo bci { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = kFrame.bytes();
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci {};
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo ai {};
        if (vmaCreateBuffer(dev_->allocator(), &bci, &aci, &staging_, &stagingAlloc_, &ai) != VK_SUCCESS) {
            error = "staging allocation failed";
            return;
        }
        expected_.resize(size_t(kW) * kH * 4);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                for (int c = 0; c < 4; ++c) {
                    expected_[(size_t(y) * kW + x) * 4 + c] = float(patternByte(x, y, c)) / 255.0f;
                }
            }
        }
        std::memcpy(ai.pMappedData, expected_.data(), kFrame.bytes());
        vmaFlushAllocation(dev_->allocator(), stagingAlloc_, 0, VK_WHOLE_SIZE);
        expectedTextureSum = fnv1a(expected_.data(), kFrame.bytes());
    }

    void paintGL() override
    {
        if (!error.empty() || !interop_) {
            glClearColor(1, 0, 1, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            return;
        }
        if (!uploaded_) {
            int slot = -1;
            GpuStatus s = interop_->produce(
                kFrame,
                [&](VkCommandBuffer cb, VkBuffer dst) {
                    VkBufferCopy region { 0, 0, kFrame.bytes() };
                    vkCmdCopyBuffer(cb, staging_, dst, 1, &region);
                    return GpuStatus {};
                },
                {}, &slot);
            if (s) {
                s = interop_->waitProduced(slot);
            }
            QElapsedTimer t;
            t.start();
            if (s) {
                s = interop_->upload(slot, tex_, 0, 0);
            }
            glFinish();
            handoffMs = double(t.nsecsElapsed()) / 1e6;
            if (!s) {
                error = "hand-off: " + s.message;
                return;
            }
            uploaded_ = true;

            std::vector<float> got(expected_.size(), -1.0f);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, kW, kH, GL_RGBA, GL_FLOAT, got.data());
            textureSum = fnv1a(got.data(), got.size() * sizeof(float));
            textureExact = std::memcmp(got.data(), expected_.data(), got.size() * sizeof(float)) == 0;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, defaultFramebufferObject());
        glBlitFramebuffer(0, 0, kW, kH, 0, 0, kW, kH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        painted = true;
    }

private:
    std::unique_ptr<GpuDevice> dev_;
    std::unique_ptr<GlInterop> interop_;
    GLuint tex_ = 0;
    GLuint fbo_ = 0;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc_ = nullptr;
    std::vector<float> expected_;
    bool uploaded_ = false;
};

} // namespace

int
main(int argc, char** argv)
{
    // The viewer sets no format of its own, so it gets Qt's default, which on Mesa is a
    // compatibility context; requesting a core profile here would test something it never runs.
    QApplication app(argc, argv);

    InteropWidget w;
    w.setFixedSize(kW, kH);
    w.show();
    QElapsedTimer timeout;
    timeout.start();
    while (!w.painted && w.error.empty() && timeout.elapsed() < 10000) {
        app.processEvents(QEventLoop::AllEvents, 50);
    }
    if (!w.error.empty()) {
        std::printf("FAIL: %s\n", w.error.c_str());
        return 1;
    }
    if (!w.painted) {
        std::printf("FAIL: widget never painted\n");
        return 1;
    }

    const QImage img = w.grabFramebuffer().convertToFormat(QImage::Format_RGBA8888);
    std::vector<uint8_t> expected(size_t(kW) * kH * 4);
    std::vector<uint8_t> got(expected.size());
    int maxDiff = 0;
    for (int y = 0; y < kH; ++y) {
        // GL rows run bottom-up, the grabbed image top-down.
        const uint8_t* line = img.constScanLine(kH - 1 - y);
        for (int x = 0; x < kW; ++x) {
            for (int c = 0; c < 4; ++c) {
                const size_t i = (size_t(y) * kW + x) * 4 + c;
                expected[i] = patternByte(x, y, c);
                got[i] = line[x * 4 + c];
                maxDiff = std::max(maxDiff, std::abs(int(got[i]) - int(expected[i])));
            }
        }
    }
    const uint64_t renderSum = fnv1a(got.data(), got.size());
    const uint64_t expectedRenderSum = fnv1a(expected.data(), expected.size());

    std::printf("driver: %s\npath: %s\nhand-off %dx%d: %.3f ms\n", w.renderer.c_str(), w.pathName.c_str(), kW,
                kH, w.handoffMs);
    std::printf("texture checksum %016llx expected %016llx %s\n", (unsigned long long)w.textureSum,
                (unsigned long long)w.expectedTextureSum, w.textureExact ? "MATCH" : "MISMATCH");
    std::printf("render  checksum %016llx expected %016llx %s (max diff %d)\n", (unsigned long long)renderSum,
                (unsigned long long)expectedRenderSum, renderSum == expectedRenderSum ? "MATCH" : "MISMATCH",
                maxDiff);
    std::printf("RESULT best=%s checksum=%016llx %s\n", w.pathName.c_str(), (unsigned long long)w.textureSum,
                (w.textureExact && renderSum == expectedRenderSum) ? "PASS" : "FAIL");
    return (w.textureExact && renderSum == expectedRenderSum) ? 0 : 1;
}
