/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <QtCore/QThreadPool>

#include <dlfcn.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/GpuContext.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewerInstance.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/ViewerHandoff.h"

// After every project header: EGL's X11 platform header defines None, which ComputeHandoff uses.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

NATRON_NAMESPACE_USING

namespace {

class ScopedEnv
{
public:
    // A null value unsets the variable.
    ScopedEnv(const char* name,
              const char* value)
        : _name(name)
    {
        const char* old = std::getenv(name);
        _hadOld = old != nullptr;
        if (old) {
            _old = old;
        }
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }

    ~ScopedEnv()
    {
        if (_hadOld) {
            ::setenv(_name.c_str(), _old.c_str(), 1);
        } else {
            ::unsetenv(_name.c_str());
        }
    }

private:
    std::string _name;
    std::string _old;
    bool _hadOld = false;
};

#ifdef NATRON_HAVE_GPU

// The input's pixels, and the texture rectangle that runs 8 columns past their right edge and 4
// rows past their top.
const RectI kBounds(-5, 3, 35, 27);
const RectI kRect(-5, 3, 43, 31);

// A rect inside kBounds, sharing its bottom-left corner.
const RectI kSmallRect(-5, 3, 11, 11);

// Spelled out rather than taken from a GL header, which would collide with the engine's glad.
const unsigned kGlTexture2D = 0x0DE1;
const unsigned kGlRgba8 = 0x8058;
const unsigned kGlRgba32F = 0x8814;
const unsigned kGlRgba = 0x1908;
const unsigned kGlBgra = 0x80E1;
const unsigned kGlFloat = 0x1406;
const unsigned kGlUnsignedInt8888Rev = 0x8367;
const unsigned kGlPackAlignment = 0x0D05;
const unsigned kGlUnpackAlignment = 0x0CF5;

struct GlFns {
    void (*GenTextures)(int, unsigned*) = nullptr;
    void (*DeleteTextures)(int, const unsigned*) = nullptr;
    void (*BindTexture)(unsigned, unsigned) = nullptr;
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*) = nullptr;
    void (*GetTexImage)(unsigned, int, unsigned, unsigned, void*) = nullptr;
    void (*PixelStorei)(unsigned, int) = nullptr;
    unsigned (*GetError)() = nullptr;
};

decltype(&::eglGetProcAddress) gEglGetProcAddress = nullptr;

void*
eglLoader(const char* name)
{
    return reinterpret_cast<void*>(gEglGetProcAddress(name));
}

// A surfaceless EGL desktop-GL context, with libEGL loaded at run time so that the test binary
// does not link it.
class EglSurfaceless
{
public:
    std::string error;
    GlFns gl;

    bool init()
    {
        // The dev image's /usr/local glvnd lists only the NVIDIA EGL vendor.
        if (!std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") && (::access("/usr/share/glvnd/egl_vendor.d/50_mesa.json", R_OK) == 0) &&
            (::access("/usr/local/share/glvnd/egl_vendor.d", F_OK) == 0)) {
            ::setenv("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/50_mesa.json", 1);
        }
        _lib = ::dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!_lib) {
            error = "libEGL.so.1 not found";

            return false;
        }
        auto sym = [this](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)> >(::dlsym(_lib, name));

            return fn != nullptr;
        };
        if (!sym(gEglGetProcAddress, "eglGetProcAddress") || !sym(_queryString, "eglQueryString") || !sym(_initialize, "eglInitialize") ||
            !sym(_bindApi, "eglBindAPI") || !sym(_createContext, "eglCreateContext") || !sym(_makeCurrent, "eglMakeCurrent") ||
            !sym(_destroyContext, "eglDestroyContext") || !sym(_terminate, "eglTerminate")) {
            error = "libEGL lacks core entry points";

            return false;
        }
        const char* clientExts = _queryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        if (!clientExts || !std::strstr(clientExts, "EGL_MESA_platform_surfaceless")) {
            error = "EGL_MESA_platform_surfaceless missing";

            return false;
        }
        auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(gEglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (!getPlatformDisplay) {
            error = "eglGetPlatformDisplayEXT missing";

            return false;
        }
        _dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if ((_dpy == EGL_NO_DISPLAY) || !_initialize(_dpy, nullptr, nullptr)) {
            error = "eglInitialize(surfaceless) failed";
            _dpy = EGL_NO_DISPLAY;

            return false;
        }
        const char* exts = _queryString(_dpy, EGL_EXTENSIONS);
        if (!exts || !std::strstr(exts, "EGL_KHR_surfaceless_context") || !std::strstr(exts, "EGL_KHR_no_config_context")) {
            error = "EGL_KHR_surfaceless_context or EGL_KHR_no_config_context missing";

            return false;
        }
        if (!_bindApi(EGL_OPENGL_API)) {
            error = "eglBindAPI(EGL_OPENGL_API) failed";

            return false;
        }
        _ctx = _createContext(_dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, nullptr);
        if (_ctx == EGL_NO_CONTEXT) {
            error = "eglCreateContext failed";

            return false;
        }
        if (!_makeCurrent(_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, _ctx)) {
            error = "eglMakeCurrent failed";

            return false;
        }
        auto get = [](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)> >(gEglGetProcAddress(name));
        };
        get(gl.GenTextures, "glGenTextures");
        get(gl.DeleteTextures, "glDeleteTextures");
        get(gl.BindTexture, "glBindTexture");
        get(gl.TexImage2D, "glTexImage2D");
        get(gl.GetTexImage, "glGetTexImage");
        get(gl.PixelStorei, "glPixelStorei");
        get(gl.GetError, "glGetError");
        if (!gl.GenTextures || !gl.DeleteTextures || !gl.BindTexture || !gl.TexImage2D || !gl.GetTexImage || !gl.PixelStorei || !gl.GetError) {
            error = "GL entry points unavailable";

            return false;
        }

        return true;
    }

    ~EglSurfaceless()
    {
        if (_dpy != EGL_NO_DISPLAY) {
            _makeCurrent(_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (_ctx != EGL_NO_CONTEXT) {
                _destroyContext(_dpy, _ctx);
            }
            _terminate(_dpy);
        }
        // libEGL stays loaded: its drivers register atexit handlers.
    }

private:
    void* _lib = nullptr;
    decltype(&::eglQueryString) _queryString = nullptr;
    decltype(&::eglInitialize) _initialize = nullptr;
    decltype(&::eglBindAPI) _bindApi = nullptr;
    decltype(&::eglCreateContext) _createContext = nullptr;
    decltype(&::eglMakeCurrent) _makeCurrent = nullptr;
    decltype(&::eglDestroyContext) _destroyContext = nullptr;
    decltype(&::eglTerminate) _terminate = nullptr;
    EGLDisplay _dpy = EGL_NO_DISPLAY;
    EGLContext _ctx = EGL_NO_CONTEXT;
};

const char*
channelsName(DisplayChannelsEnum channels)
{
    switch (channels) {
    case eDisplayChannelsRGB:
        return "RGB";
    case eDisplayChannelsR:
        return "R";
    case eDisplayChannelsG:
        return "G";
    case eDisplayChannelsB:
        return "B";
    case eDisplayChannelsA:
        return "A";
    case eDisplayChannelsY:
        return "Y";
    case eDisplayChannelsMatte:
        return "Matte";
    }

    return "?";
}

float
inputValue(int x,
           int y,
           int c,
           int seed)
{
    const int row = y - kBounds.y1;
    const float t = float(x - kBounds.x1) / float(kBounds.width() - 1);
    const float phase = t + 0.17f * float(c) + 0.043f * float(row) + 0.31f * float(seed);
    float v = -0.3f + 2.6f * (phase - std::floor(phase));
    if (row % 7 == 3) {
        v *= 6.f;
    }

    return v;
}

const ImageLayerDesc&
layerFor(int nComps)
{
    switch (nComps) {
    case 1:
        return ImageLayerDesc::getAlphaComponents();
    case 2:
        return ImageLayerDesc::getXYComponents();
    case 3:
        return ImageLayerDesc::getRGBComponents();
    default:
        return ImageLayerDesc::getRGBAComponents();
    }
}

std::vector<float>
packedPixels(int nComps,
             int seed)
{
    std::vector<float> packed(std::size_t(kBounds.area()) * nComps);
    for (int y = kBounds.y1; y < kBounds.y2; ++y) {
        for (int x = kBounds.x1; x < kBounds.x2; ++x) {
            for (int c = 0; c < nComps; ++c) {
                packed[(std::size_t(y - kBounds.y1) * kBounds.width() + (x - kBounds.x1)) * nComps + c] = inputValue(x, y, c, seed);
            }
        }
    }

    return packed;
}

ImagePtr
makeImage(int nComps,
          int seed)
{
    const RectD rod(kBounds.x1, kBounds.y1, kBounds.x2, kBounds.y2);
    ImagePtr image = std::make_shared<Image>(layerFor(nComps), rod, kBounds, 0, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false);
    const std::vector<float> packed = packedPixels(nComps, seed);
    Image::WriteAccess w = image->getWriteRights();
    for (int y = kBounds.y1; y < kBounds.y2; ++y) {
        float* row = (float*)w.pixelAt(kBounds.x1, y);
        std::copy_n(packed.begin() + std::size_t(y - kBounds.y1) * kBounds.width() * nComps, std::size_t(kBounds.width()) * nComps, row);
    }

    return image;
}

// A device root as a GPU region would leave it: the packed pixels of kBounds in a pool buffer.
GpuDeviceImagePtr
makeRoot(gpu::GpuRuntime& runtime,
         int nComps,
         int seed)
{
    GpuImageLayout layout;
    layout.layer = layerFor(nComps);
    layout.rod = RectD(kBounds.x1, kBounds.y1, kBounds.x2, kBounds.y2);
    layout.bounds = kBounds;
    gpu::GpuBufferHandle handle;
    gpu::GpuStatus s = runtime.acquire(layout.deviceBytes(), handle);
    EXPECT_TRUE(bool(s)) << s.message;
    if (!s) {
        return GpuDeviceImagePtr();
    }
    std::vector<float> packed = packedPixels(nComps, seed);
    gpu::TimelineValue done;
    s = runtime.upload({ packed.data(), packed.size() * sizeof(float) }, handle.buffer(), 0, done);
    EXPECT_TRUE(bool(s)) << s.message;
    if (s) {
        s = runtime.wait(done);
        EXPECT_TRUE(bool(s)) << s.message;
    }
    if (!s) {
        return GpuDeviceImagePtr();
    }

    return GpuDeviceImage::create(runtime, handle, layout, RenderStatsPtr());
}

struct Case {
    DisplayChannelsEnum channels = eDisplayChannelsRGB;
    int alphaChannelIndex = -1;
    bool matte = false;
    double gain = 1.;
    double offset = 0.;
    double gamma = 1.;
};

Case
makeCase(DisplayChannelsEnum channels,
         int nComps)
{
    Case c;
    c.channels = channels;
    if (channels == eDisplayChannelsMatte) {
        c.matte = true;
        c.alphaChannelIndex = (nComps == 4) ? 3 : 0;
    } else if (channels == eDisplayChannelsA) {
        c.alphaChannelIndex = (nComps == 4) ? -1 : 0;
    }

    return c;
}

ViewerHandoffParams
paramsFor(const Case& c,
          const RectI& rect)
{
    ViewerHandoffParams p;
    p.rect = rect;
    p.channels = c.channels;
    p.alphaChannelIndex = c.alphaChannelIndex;
    p.matteOverlay = c.matte;
    p.gain = c.gain;
    p.offset = c.offset;
    p.gamma = c.gamma;

    return p;
}

ViewerHandoffFrame
frameFor(const RectI& rect,
         ViewerHandoffFormat format)
{
    ViewerHandoffFrame f;
    f.width = unsigned(rect.width());
    f.height = unsigned(rect.height());
    f.format = format;

    return f;
}

// The CPU path in the pieces it can render: the rows of a rect whose bottom-left corner is outside
// the image read as no pixel, but a rect that starts inside must stay inside.
template <typename T>
std::vector<T>
cpuReference(const ImagePtr& image,
             const Case& c,
             ImageBitDepthEnum depth,
             int elementsPerPixel,
             const RectI& rect)
{
    std::vector<T> full(std::size_t(rect.area()) * elementsPerPixel, T(0));
    std::vector<RectI> pieces;
    if ((rect.x2 <= kBounds.x2) && (rect.y2 <= kBounds.y2)) {
        pieces.push_back(rect);
    } else {
        pieces.push_back(kBounds);
        pieces.push_back(RectI(kBounds.x2, rect.y1, rect.x2, kBounds.y2));
        pieces.push_back(RectI(rect.x1, kBounds.y2, rect.x2, rect.y2));
    }
    for (const RectI& piece : pieces) {
        std::vector<T> part(std::size_t(piece.area()) * elementsPerPixel, T(0));
        viewerProcessRectOnCpu(image, c.matte ? ImageConstPtr(image) : ImageConstPtr(), c.channels, depth, c.gain, c.gamma, c.offset,
                               ProjectColorManagement::DisplayProcessorPtr(), c.alphaChannelIndex, piece, part.data());
        for (int y = piece.y1; y < piece.y2; ++y) {
            std::copy_n(part.begin() + std::size_t((y - piece.y1) * piece.width()) * elementsPerPixel,
                        std::size_t(piece.width()) * elementsPerPixel,
                        full.begin() + (std::size_t(y - rect.y1) * rect.width() + (piece.x1 - rect.x1)) * elementsPerPixel);
        }
    }

    return full;
}

std::size_t
eightBitMismatches(const std::vector<U32>& gpu,
                   const std::vector<U32>& cpu,
                   std::string* first)
{
    if (gpu.size() != cpu.size()) {
        *first = "size differs";

        return std::max(gpu.size(), cpu.size());
    }
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        const int shifts[4] = { 24, 16, 8, 0 };
        for (int k = 0; k < 4; ++k) {
            const int expected = int((cpu[i] >> shifts[k]) & 0xffu);
            const int got = int((gpu[i] >> shifts[k]) & 0xffu);
            const int tolerance = (k == 0) ? 0 : 1;
            if (std::abs(expected - got) > tolerance) {
                if (mismatches == 0) {
                    std::ostringstream s;
                    s << "pixel " << i << " byte " << "ARGB"[k] << ": cpu " << expected << ", gpu " << got;
                    *first = s.str();
                }
                ++mismatches;
            }
        }
    }

    return mismatches;
}

std::size_t
floatMismatches(const std::vector<float>& gpu,
                const std::vector<float>& cpu,
                std::string* first)
{
    if (gpu.size() != cpu.size()) {
        *first = "size differs";

        return std::max(gpu.size(), cpu.size());
    }
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        const double tolerance = 1e-6 * std::max(1., std::fabs(double(cpu[i])));
        if (!(std::fabs(double(gpu[i]) - double(cpu[i])) <= tolerance)) {
            if (mismatches == 0) {
                std::ostringstream s;
                s << "float " << i << " (pixel " << i / 4 << " channel " << i % 4 << "): cpu " << cpu[i] << ", gpu " << gpu[i];
                *first = s.str();
            }
            ++mismatches;
        }
    }

    return mismatches;
}

// A GPU context, a current GL context, and the textures a test presents into.
class Harness
{
public:
    std::unique_ptr<GpuContext> context;
    EglSurfaceless egl;

    ~Harness()
    {
        if (egl.gl.DeleteTextures) {
            for (unsigned t : _textures) {
                egl.gl.DeleteTextures(1, &t);
            }
        }
    }

    // False after reporting the skip, or the failure when the environment requires a GPU and GL.
    bool init()
    {
        GpuContextOptions options;
        options.mode = GpuResolvedMode::Force;
        options.infoLog = [](const std::string& line) { std::printf("[viewer-handoff] %s\n", line.c_str()); };
        context.reset(new GpuContext(options));
        if (!context->waitForInit(std::chrono::seconds(60)) || !context->isAvailable()) {
            reportNoGpu("the GPU context did not come up");

            return false;
        }
        if (!egl.init()) {
            if (std::getenv("NATRON_TEST_REQUIRE_GL")) {
                ADD_FAILURE() << "EGL surfaceless unavailable: " << egl.error;
            } else {
                std::printf("[  SKIPPED ] EGL surfaceless unavailable: %s\n", egl.error.c_str());
            }

            return false;
        }
        std::printf("vulkan device: %s\n", context->deviceName().c_str());

        return true;
    }

    gpu::GpuRuntime& runtime() { return *context->runtime(); }

    unsigned makeTexture(const ViewerHandoffFrame& frame)
    {
        unsigned t = 0;
        egl.gl.GenTextures(1, &t);
        egl.gl.BindTexture(kGlTexture2D, t);
        egl.gl.PixelStorei(kGlUnpackAlignment, 4);
        if (frame.format == ViewerHandoffFormat::Bgra8) {
            egl.gl.TexImage2D(kGlTexture2D, 0, int(kGlRgba8), int(frame.width), int(frame.height), 0, kGlBgra, kGlUnsignedInt8888Rev, nullptr);
        } else {
            egl.gl.TexImage2D(kGlTexture2D, 0, int(kGlRgba32F), int(frame.width), int(frame.height), 0, kGlRgba, kGlFloat, nullptr);
        }
        _textures.push_back(t);

        return t;
    }

    std::vector<U32> readEightBit(unsigned texture,
                                  const ViewerHandoffFrame& frame)
    {
        std::vector<U32> out(std::size_t(frame.width) * frame.height, 0);
        egl.gl.BindTexture(kGlTexture2D, texture);
        egl.gl.PixelStorei(kGlPackAlignment, 4);
        egl.gl.GetTexImage(kGlTexture2D, 0, kGlBgra, kGlUnsignedInt8888Rev, out.data());

        return out;
    }

    std::vector<float> readFloat(unsigned texture,
                                 const ViewerHandoffFrame& frame)
    {
        std::vector<float> out(std::size_t(frame.width) * frame.height * 4, -1.f);
        egl.gl.BindTexture(kGlTexture2D, texture);
        egl.gl.PixelStorei(kGlPackAlignment, 4);
        egl.gl.GetTexImage(kGlTexture2D, 0, kGlRgba, kGlFloat, out.data());

        return out;
    }

    // Presents root through handoff into a fresh texture and compares it with the CPU path.
    void presentAndCompare(ViewerHandoff& handoff,
                           int textureIndex,
                           const GpuDeviceImagePtr& root,
                           const ImagePtr& image,
                           const Case& c,
                           ViewerHandoffFormat format,
                           const RectI& rect,
                           const std::string& label)
    {
        const ViewerHandoffFrame frame = frameFor(rect, format);
        const unsigned texture = makeTexture(frame);
        const ViewerHandoffResult r = handoff.present(textureIndex, root, paramsFor(c, rect), texture, frame);
        ASSERT_TRUE(bool(r)) << label << ": " << r.reason;
        std::string first;
        if (format == ViewerHandoffFormat::Bgra8) {
            const std::vector<U32> got = readEightBit(texture, frame);
            ASSERT_EQ(0u, egl.gl.GetError()) << label;
            EXPECT_EQ(0u, eightBitMismatches(got, cpuReference<U32>(image, c, eImageBitDepthByte, 1, rect), &first)) << label << ": first " << first;
        } else {
            const std::vector<float> got = readFloat(texture, frame);
            ASSERT_EQ(0u, egl.gl.GetError()) << label;
            EXPECT_EQ(0u, floatMismatches(got, cpuReference<float>(image, c, eImageBitDepthFloat, 4, rect), &first)) << label << ": first " << first;
        }
    }

private:
    std::vector<unsigned> _textures;
};

std::string
describe(ViewerHandoffRung rung,
         int nComps,
         const Case& c,
         ViewerHandoffFormat format)
{
    std::ostringstream s;
    s << toString(rung) << " " << ((format == ViewerHandoffFormat::Bgra8) ? "8u" : "32f") << " nComps " << nComps << " "
      << channelsName(c.channels);

    return s.str();
}

#endif // NATRON_HAVE_GPU

} // namespace

TEST(ViewerHandoff, CpuWithoutAContextOrWhenOverridden)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    {
        ViewerHandoff handoff(nullptr);
        EXPECT_EQ(ViewerHandoffRung::Cpu, handoff.create([](const char*) -> void* { return nullptr; }));
        EXPECT_EQ(ViewerHandoffRung::Cpu, handoff.rung());
        EXPECT_FALSE(handoff.rungReason().empty());
        const ViewerHandoffResult r = handoff.present(0, GpuDeviceImagePtr(), ViewerHandoffParams(), 0, ViewerHandoffFrame());
        EXPECT_FALSE(bool(r));
        EXPECT_FALSE(r.reason.empty());
        handoff.release();
    }

    ScopedEnv rung("NATRON_VIEWER_HANDOFF", "cpu");
    GpuContextOptions options;
    options.mode = GpuResolvedMode::Off;
    GpuContext context(options);
    ViewerHandoff handoff(&context);
    EXPECT_EQ(ViewerHandoffRung::Cpu, handoff.create([](const char*) -> void* { return nullptr; }));
    EXPECT_NE(std::string::npos, handoff.rungReason().find("NATRON_VIEWER_HANDOFF=cpu")) << handoff.rungReason();
    EXPECT_FALSE(bool(handoff.prepare(ProjectColorManagement::DisplayProcessorPtr(), ViewerHandoffFormat::Bgra8)));
#endif
}

TEST(ViewerHandoff, PresentMatchesTheCpuOnEveryRung)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    Harness h;
    if (!h.init()) {
        return;
    }
    const DisplayChannelsEnum channels[] = { eDisplayChannelsRGB, eDisplayChannelsR, eDisplayChannelsA, eDisplayChannelsY, eDisplayChannelsMatte };
    int ran = 0;
    for (ViewerHandoffRung wanted : { ViewerHandoffRung::ZeroCopy, ViewerHandoffRung::HostSync, ViewerHandoffRung::Readback }) {
        ScopedEnv env("NATRON_VIEWER_HANDOFF", toString(wanted));
        ViewerHandoff handoff(h.context.get());
        const ViewerHandoffRung got = handoff.create(eglLoader);
        if (got != wanted) {
            std::printf("[viewer-handoff] %s unavailable, the ladder gave %s: %s\n", toString(wanted), toString(got), handoff.rungReason().c_str());
            continue;
        }
        ++ran;
        for (int nComps : { 1, 3, 4 }) {
            const ImagePtr image = makeImage(nComps, nComps);
            const GpuDeviceImagePtr root = makeRoot(h.runtime(), nComps, nComps);
            ASSERT_TRUE(bool(root));
            for (DisplayChannelsEnum ch : channels) {
                Case c = makeCase(ch, nComps);
                c.gain = 0.5;
                c.offset = 0.2;
                c.gamma = 2.2;
                for (ViewerHandoffFormat format : { ViewerHandoffFormat::Bgra8, ViewerHandoffFormat::Rgba32F }) {
                    const int textureIndex = (format == ViewerHandoffFormat::Bgra8) ? 0 : 1;
                    h.presentAndCompare(handoff, textureIndex, root, image, c, format, kRect, describe(wanted, nComps, c, format));
                    if (::testing::Test::HasFatalFailure()) {
                        return;
                    }
                }
            }
        }
        EXPECT_EQ(wanted, handoff.rung());
        handoff.release();
        EXPECT_EQ(ViewerHandoffRung::Cpu, handoff.rung());
        std::printf("[viewer-handoff] %s matches the CPU path\n", toString(wanted));
    }
    EXPECT_GT(ran, 0) << "not even the readback rung came up";
#endif
}

TEST(ViewerHandoff, ReadbackMatchesTheTextureOnEveryRung)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    Harness h;
    if (!h.init()) {
        return;
    }
    const ImagePtr image = makeImage(4, 5);
    const GpuDeviceImagePtr root = makeRoot(h.runtime(), 4, 5);
    ASSERT_TRUE(bool(root));
    Case c = makeCase(eDisplayChannelsRGB, 4);
    c.gain = 0.5;
    c.gamma = 2.2;
    int ran = 0;
    for (ViewerHandoffRung wanted : { ViewerHandoffRung::ZeroCopy, ViewerHandoffRung::HostSync, ViewerHandoffRung::Readback }) {
        ScopedEnv env("NATRON_VIEWER_HANDOFF", toString(wanted));
        ViewerHandoff handoff(h.context.get());
        if (handoff.create(eglLoader) != wanted) {
            continue;
        }
        ++ran;
        for (ViewerHandoffFormat format : { ViewerHandoffFormat::Bgra8, ViewerHandoffFormat::Rgba32F }) {
            const ViewerHandoffFrame frame = frameFor(kRect, format);
            const unsigned texture = h.makeTexture(frame);
            if (format == ViewerHandoffFormat::Bgra8) {
                EXPECT_EQ(0u, handoff.capacity(0)) << toString(wanted);
            } else {
                // Ring 0 was built at the 8-bit frame's size, so the first float frame is refused and
                // the slots grow at the next present().
                const std::size_t eightBit = frameFor(kRect, ViewerHandoffFormat::Bgra8).bytes();
                ASSERT_EQ(eightBit, handoff.capacity(0)) << toString(wanted);
                ASSERT_LT(eightBit, frame.bytes());
                const ViewerHandoffResult refused = handoff.present(0, root, paramsFor(c, kRect), texture, frame);
                EXPECT_FALSE(bool(refused)) << toString(wanted);
                EXPECT_NE(std::string::npos, refused.reason.find("over")) << toString(wanted) << ": " << refused.reason;
                EXPECT_EQ(eightBit, handoff.capacity(0)) << toString(wanted);
            }
            // More requests than the ring has slots, so that an uncollected copy has to be harvested before
            // its slot is produced into again.
            std::map<std::uint64_t, std::vector<unsigned char> > expected;
            for (std::uint64_t token = 1; token <= 3; ++token) {
                ViewerHandoffParams params = paramsFor(c, kRect);
                params.gain = 0.25 * double(token);
                params.readbackToken = token;
                const ViewerHandoffResult r = handoff.present(0, root, params, texture, frame);
                ASSERT_TRUE(bool(r)) << toString(wanted) << ": " << r.reason;
                std::vector<unsigned char> bytes(frame.bytes());
                h.egl.gl.BindTexture(kGlTexture2D, texture);
                h.egl.gl.PixelStorei(kGlPackAlignment, 4);
                if (format == ViewerHandoffFormat::Bgra8) {
                    h.egl.gl.GetTexImage(kGlTexture2D, 0, kGlBgra, kGlUnsignedInt8888Rev, bytes.data());
                } else {
                    h.egl.gl.GetTexImage(kGlTexture2D, 0, kGlRgba, kGlFloat, bytes.data());
                }
                expected[token] = bytes;
            }
            EXPECT_EQ(frame.bytes(), handoff.capacity(0)) << toString(wanted);
            ViewerHandoffParams plain = paramsFor(c, kRect);
            ASSERT_TRUE(bool(handoff.present(1, root, plain, texture, frame)));

            struct Got {
                ViewerHandoffFrame frame;
                std::vector<unsigned char> bytes;
            };
            std::mutex gotMutex;
            std::map<std::uint64_t, Got> got;
            bool presentRefusedWhileClaiming = true;
            const auto collect = [&](std::uint64_t) -> ViewerHandoffReadbackFn {
                // The claim runs while the rings are walked, which present() must not change.
                presentRefusedWhileClaiming = presentRefusedWhileClaiming && !handoff.present(0, root, plain, texture, frame);

                return [&](const ViewerHandoffReadback& rb) {
                    std::lock_guard<std::mutex> k(gotMutex);
                    Got& g = got[rb.token];
                    g.frame = rb.frame;
                    g.bytes.assign(rb.data, rb.data + rb.size);
                };
            };
            const auto gotCount = [&]() {
                std::lock_guard<std::mutex> k(gotMutex);

                return got.size();
            };
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while ((gotCount() < expected.size()) && (std::chrono::steady_clock::now() < deadline)) {
                handoff.consumeCompletedReadbacks(collect);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            ASSERT_EQ(expected.size(), gotCount()) << toString(wanted);
            EXPECT_TRUE(presentRefusedWhileClaiming) << toString(wanted);
            for (const auto& e : expected) {
                const Got& rb = got.at(e.first);
                EXPECT_EQ(frame.width, rb.frame.width);
                EXPECT_EQ(frame.height, rb.frame.height);
                EXPECT_TRUE(rb.bytes == e.second) << toString(wanted) << " token " << e.first;
            }
            bool anyLeft = false;
            handoff.consumeCompletedReadbacks([&](std::uint64_t) -> ViewerHandoffReadbackFn {
                anyLeft = true;

                return ViewerHandoffReadbackFn();
            });
            EXPECT_FALSE(anyLeft) << toString(wanted);
        }
        handoff.release();
    }
    EXPECT_GT(ran, 0) << "not even the readback rung came up";
#endif
}

#ifdef NATRON_HAVE_GPU
namespace {

// A readback present()ed into ring 0, with the texels its texture received.
struct ReadbackSetup {
    GpuDeviceImagePtr root;
    Case c = makeCase(eDisplayChannelsRGB, 4);
    ViewerHandoffFrame frame = frameFor(kRect, ViewerHandoffFormat::Bgra8);
    unsigned texture = 0;
    std::vector<unsigned char> expected;
};

// Presents tokens 1 to count into ring 0 and keeps the texels of token 1.
bool
presentReadbacks(Harness& h,
                 ViewerHandoff& handoff,
                 std::uint64_t count,
                 ReadbackSetup* setup)
{
    setup->root = makeRoot(h.runtime(), 4, 5);
    if (!setup->root) {
        ADD_FAILURE() << "no device root";

        return false;
    }
    setup->texture = h.makeTexture(setup->frame);
    for (std::uint64_t token = 1; token <= count; ++token) {
        ViewerHandoffParams params = paramsFor(setup->c, kRect);
        params.gain = 0.25 * double(token);
        params.readbackToken = token;
        const ViewerHandoffResult r = handoff.present(0, setup->root, params, setup->texture, setup->frame);
        if (!r) {
            ADD_FAILURE() << "token " << token << ": " << r.reason;

            return false;
        }
        if (token == 1) {
            setup->expected.resize(setup->frame.bytes());
            h.egl.gl.BindTexture(kGlTexture2D, setup->texture);
            h.egl.gl.PixelStorei(kGlPackAlignment, 4);
            h.egl.gl.GetTexImage(kGlTexture2D, 0, kGlBgra, kGlUnsignedInt8888Rev, setup->expected.data());
        }
    }

    return true;
}

// Consumes readbacks until claim has taken token 1.
bool
claimFirst(ViewerHandoff& handoff,
           const ViewerHandoffReadbackFn& work)
{
    bool claimed = false;
    const auto claim = [&](std::uint64_t token) -> ViewerHandoffReadbackFn {
        if (token != 1) {
            return ViewerHandoffReadbackFn();
        }
        claimed = true;

        return work;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!claimed && (std::chrono::steady_clock::now() < deadline)) {
        handoff.consumeCompletedReadbacks(claim);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return claimed;
}

bool
waitFor(const std::atomic<bool>& flag)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!flag && (std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return flag;
}

// Holds a fill until the thread setAfter() starts lets it go.
struct Latch {
    std::promise<void> promise;
    std::shared_future<void> future = promise.get_future().share();
    std::atomic<bool> started { false };
    std::atomic<bool> finished { false };

    void setAfter(std::chrono::milliseconds delay, std::thread* thread)
    {
        *thread = std::thread([this, delay]() {
            std::this_thread::sleep_for(delay);
            promise.set_value();
        });
    }
};

} // namespace
#endif // NATRON_HAVE_GPU

TEST(ViewerHandoff, PresentWaitsForAFillStillReadingTheSlotItReuses)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    Harness h;
    if (!h.init()) {
        return;
    }
    ScopedEnv env("NATRON_VIEWER_HANDOFF", nullptr);
    ViewerHandoff handoff(h.context.get());
    ASSERT_NE(ViewerHandoffRung::Cpu, handoff.create(eglLoader)) << handoff.rungReason();
    ReadbackSetup setup;
    // Token 1 in one slot and token 2 in the other, so the next present() comes round to token 1's.
    ASSERT_TRUE(presentReadbacks(h, handoff, 2, &setup));

    Latch latch;
    std::vector<unsigned char> seen;
    ASSERT_TRUE(claimFirst(handoff, [&](const ViewerHandoffReadback& rb) {
        latch.started = true;
        latch.future.wait();
        seen.assign(rb.data, rb.data + rb.size);
        latch.finished = true;
    }));
    std::thread releaser;
    latch.setAfter(std::chrono::milliseconds(200), &releaser);
    ViewerHandoffParams params = paramsFor(setup.c, kRect);
    params.gain = 1.;
    const ViewerHandoffResult r = handoff.present(0, setup.root, params, setup.texture, setup.frame);
    const bool finishedFirst = latch.finished;
    releaser.join();
    ASSERT_TRUE(bool(r)) << r.reason;
    EXPECT_TRUE(finishedFirst) << "present() produced into a slot a fill was still reading";
    EXPECT_TRUE(seen == setup.expected) << "the fill read a slot that had been produced into again";
    handoff.release();
#endif
}

TEST(ViewerHandoff, ReleaseWaitsForARunningFillAndCancelsAQueuedOne)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    Harness h;
    if (!h.init()) {
        return;
    }
    ScopedEnv env("NATRON_VIEWER_HANDOFF", nullptr);
    {
        ViewerHandoff handoff(h.context.get());
        ASSERT_NE(ViewerHandoffRung::Cpu, handoff.create(eglLoader)) << handoff.rungReason();
        ReadbackSetup setup;
        ASSERT_TRUE(presentReadbacks(h, handoff, 1, &setup));
        Latch latch;
        std::vector<unsigned char> seen;
        ASSERT_TRUE(claimFirst(handoff, [&](const ViewerHandoffReadback& rb) {
            latch.started = true;
            latch.future.wait();
            seen.assign(rb.data, rb.data + rb.size);
            latch.finished = true;
        }));
        ASSERT_TRUE(waitFor(latch.started)) << "the fill never started on the pool";
        std::thread releaser;
        latch.setAfter(std::chrono::milliseconds(200), &releaser);
        handoff.release();
        const bool finishedFirst = latch.finished;
        releaser.join();
        EXPECT_TRUE(finishedFirst) << "release() unmapped a slot a fill was still reading";
        EXPECT_TRUE(seen == setup.expected);
        EXPECT_EQ(ViewerHandoffRung::Cpu, handoff.rung());
    }

    // Every pool thread is held, so the fill is still queued when the hand-off is destroyed.
    QThreadPool* pool = QThreadPool::globalInstance();
    const int threads = pool->maxThreadCount();
    std::promise<void> unblock;
    std::shared_future<void> unblocked = unblock.get_future().share();
    std::atomic<int> blocking { 0 };
    std::atomic<int> blockersDone { 0 };
    for (int i = 0; i < threads; ++i) {
        pool->start([&blocking, &blockersDone, unblocked]() {
            ++blocking;
            unblocked.wait_for(std::chrono::seconds(30));
            ++blockersDone;
        });
    }
    struct Unblock {
        std::promise<void>* unblock;
        std::atomic<int>* done;
        int count;

        ~Unblock()
        {
            unblock->set_value();
            while (done->load() < count) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    } unblockOnExit { &unblock, &blockersDone, threads };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while ((blocking.load() < threads) && (std::chrono::steady_clock::now() < deadline)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(threads, blocking.load()) << "the pool threads could not all be held";

    std::atomic<bool> ran { false };
    {
        ViewerHandoff handoff(h.context.get());
        ASSERT_NE(ViewerHandoffRung::Cpu, handoff.create(eglLoader)) << handoff.rungReason();
        ReadbackSetup setup;
        ASSERT_TRUE(presentReadbacks(h, handoff, 1, &setup));
        ASSERT_TRUE(claimFirst(handoff, [&](const ViewerHandoffReadback&) { ran = true; }));
    }
    EXPECT_FALSE(ran.load()) << "a fill that had not started ran after its hand-off was destroyed";
#endif
}

TEST(ViewerHandoff, FrameOverCapacityIsRefusedAndTheNextOneFits)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    Harness h;
    if (!h.init()) {
        return;
    }
    ScopedEnv env("NATRON_VIEWER_HANDOFF", nullptr);
    ViewerHandoff handoff(h.context.get());
    const ViewerHandoffRung rung = handoff.create(eglLoader);
    ASSERT_NE(ViewerHandoffRung::Cpu, rung) << handoff.rungReason();
    std::printf("[viewer-handoff] best rung %s\n", toString(rung));

    const ImagePtr image = makeImage(4, 7);
    const GpuDeviceImagePtr root = makeRoot(h.runtime(), 4, 7);
    ASSERT_TRUE(bool(root));
    const Case c = makeCase(eDisplayChannelsRGB, 4);

    h.presentAndCompare(handoff, 0, root, image, c, ViewerHandoffFormat::Bgra8, kSmallRect, "small frame");
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    const std::size_t small = frameFor(kSmallRect, ViewerHandoffFormat::Bgra8).bytes();
    EXPECT_EQ(small, handoff.capacity(0));

    const ViewerHandoffFrame big = frameFor(kRect, ViewerHandoffFormat::Bgra8);
    const unsigned texture = h.makeTexture(big);
    const ViewerHandoffResult refused = handoff.present(0, root, paramsFor(c, kRect), texture, big);
    EXPECT_FALSE(bool(refused));
    EXPECT_NE(std::string::npos, refused.reason.find("over")) << refused.reason;
    std::printf("[viewer-handoff] refused: %s\n", refused.reason.c_str());
    EXPECT_EQ(rung, handoff.rung());

    h.presentAndCompare(handoff, 0, root, image, c, ViewerHandoffFormat::Bgra8, kRect, "the frame after the refusal");
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    EXPECT_EQ(big.bytes(), handoff.capacity(0));

    const std::size_t floatBytes = frameFor(kRect, ViewerHandoffFormat::Rgba32F).bytes();
    ASSERT_TRUE(bool(handoff.ensureCapacity(floatBytes)));
    EXPECT_EQ(floatBytes, handoff.capacity(0));
    h.presentAndCompare(handoff, 0, root, image, c, ViewerHandoffFormat::Rgba32F, kRect, "a float frame after ensureCapacity");
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    const ViewerHandoffResult wrongSize = handoff.present(0, root, paramsFor(c, kSmallRect), texture, big);
    EXPECT_FALSE(bool(wrongSize));
    EXPECT_FALSE(wrongSize.reason.empty());
    handoff.release();
#endif
}

TEST(ViewerHandoff, DeviceLossFallsBackToCpuAndRecovers)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    Harness h;
    if (!h.init()) {
        return;
    }
    ScopedEnv env("NATRON_VIEWER_HANDOFF", nullptr);
    ViewerHandoff handoff(h.context.get());
    const ViewerHandoffRung rung = handoff.create(eglLoader);
    ASSERT_NE(ViewerHandoffRung::Cpu, rung) << handoff.rungReason();

    const ImagePtr image = makeImage(4, 3);
    const Case c = makeCase(eDisplayChannelsRGB, 4);
    gpu::GpuRuntime* runtime = h.context->runtime();
    {
        GpuDeviceImagePtr root = makeRoot(*runtime, 4, 3);
        ASSERT_TRUE(bool(root));
        h.presentAndCompare(handoff, 0, root, image, c, ViewerHandoffFormat::Bgra8, kRect, "before the loss");
        ASSERT_FALSE(::testing::Test::HasFatalFailure());
        h.presentAndCompare(handoff, 1, root, image, c, ViewerHandoffFormat::Rgba32F, kRect, "B input before the loss");
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // The fault plan NATRON_GPU_FAULT=lost@N installs, armed on the next submission.
        runtime->device().injectFault(gpu::GpuFaultPlan::Kind::DeviceLost, 1);
        const ViewerHandoffFrame frame = frameFor(kRect, ViewerHandoffFormat::Bgra8);
        const unsigned texture = h.makeTexture(frame);
        const ViewerHandoffResult lost = handoff.present(0, root, paramsFor(c, kRect), texture, frame);
        EXPECT_FALSE(bool(lost));
        EXPECT_NE(std::string::npos, lost.reason.find("lost")) << lost.reason;
        std::printf("[viewer-handoff] refused: %s\n", lost.reason.c_str());
        EXPECT_EQ(ViewerHandoffRung::Cpu, handoff.rung());
        EXPECT_FALSE(handoff.maintain());

        const ViewerHandoffResult again = handoff.present(0, root, paramsFor(c, kRect), texture, frame);
        EXPECT_FALSE(bool(again));
        EXPECT_FALSE(bool(handoff.prepare(ProjectColorManagement::DisplayProcessorPtr(), ViewerHandoffFormat::Bgra8)));
    }

    ASSERT_TRUE(h.context->recoverIfLost()) << "the hand-off still holds the runtime";
    EXPECT_EQ(0u, runtime->leakedAllocationsAtLastTeardown());
    ASSERT_EQ(runtime, h.context->runtime());

    EXPECT_TRUE(handoff.maintain()) << handoff.rungReason();
    EXPECT_EQ(rung, handoff.rung());
    GpuDeviceImagePtr root = makeRoot(*runtime, 4, 3);
    ASSERT_TRUE(bool(root));
    h.presentAndCompare(handoff, 0, root, image, c, ViewerHandoffFormat::Bgra8, kRect, "after the recovery");
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    handoff.release();
#endif
}
