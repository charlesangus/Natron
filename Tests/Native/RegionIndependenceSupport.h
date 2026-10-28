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

#ifndef NATRON_TESTS_NATIVE_REGIONINDEPENDENCESUPPORT_H
#define NATRON_TESTS_NATIVE_REGIONINDEPENDENCESUPPORT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeGpuParity.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kRegionTestSourceID "test.natron.built-in.RegionTestSource"
#define kWindowedTestFilterID "test.natron.built-in.WindowedTestFilter"

NATRON_NAMESPACE_ENTER

/**
 * @brief A test-only RGBA source of white noise: channel c of pixel (x, y) at mipmap level L is a
 * hash of (x, y, c, L), so any window of it at any scale is the same pixels however it is
 * rendered. Its region of definition is a rect at identity scale, or infinite, in which case every
 * pixel has a value.
 **/
class RegionTestSource
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new RegionTestSource(n);
    }

    explicit RegionTestSource(NodePtr n)
        : NativeEffectBase(n)
        , _mutex()
        , _rod(0, 0, 16, 16)
        , _infinite(false)
    {
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
    }

    /// Sets the region of definition, at identity scale. Main thread only.
    void setFrame(const RectI& rod,
                  bool infinite)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _rod = rod;
            _infinite = infinite;
        }
        invalidateOutputMetadata();
    }

    RectI frame() const
    {
        std::lock_guard<std::mutex> lock(_mutex);

        return _rod;
    }

    bool isInfinite() const
    {
        std::lock_guard<std::mutex> lock(_mutex);

        return _infinite;
    }

    static float noise(int x,
                       int y,
                       int c,
                       unsigned level)
    {
        std::uint32_t h = ((std::uint32_t)x * 0x8da6b343u) ^ ((std::uint32_t)y * 0xd8163841u) ^ ((std::uint32_t)c * 0xcb1ab31fu) ^ (level * 0x165667b1u);

        h ^= h >> 15;
        h *= 0x2c1b3c6du;
        h ^= h >> 12;
        h *= 0x297a2d39u;
        h ^= h >> 15;
        const float unit = (float)(h >> 8) * (1.f / 16777216.f);

        // An alpha well away from 0, for the nodes that unpremultiply.
        return (c == 3) ? 0.5f + 0.5f * unit : unit;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (_infinite) {
            rod->setupInfinity();
        } else {
            *rod = RectD(_rod.x1, _rod.y1, _rod.x2, _rod.y2);
        }

        return eStatusOK;
    }

protected:
    virtual const NodeSpec* nodeSpec() const OVERRIDE WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    /// Whether a pixel's value follows its position in the window rendered rather than in the image.
    virtual bool valuesFollowWindow() const
    {
        return false;
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kRegionTestSourceID,
        .label = "Test Region Source",
    };

    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }

    virtual StatusEnum getPreferredMetadataAt(double /*time*/,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        metadata.setNComps(-1, 4);
        metadata.setBitDepth(-1, eImageBitDepthFloat);
        metadata.setIsFrameVarying(false);

        return eStatusOK;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        RectI rod;
        bool infinite;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            rod = _rod;
            infinite = _infinite;
        }
        const unsigned level = args.mappedScale.toMipmapLevel();
        const RectI inside = infinite ? RectI() : PassExec::toPixelEnclosing(RectD(rod.x1, rod.y1, rod.x2, rod.y2), args.mappedScale, 1.);
        const bool relative = valuesFollowWindow();
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image) {
                continue;
            }
            if (image->getBitDepth() != eImageBitDepthFloat) {
                return eStatusFailed;
            }
            const int nComps = (int)image->getComponentsCount();
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = (float*)access.pixelAt(x, y);
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    const bool in = infinite || ((x >= inside.x1) && (x < inside.x2) && (y >= inside.y1) && (y < inside.y2));
                    const int px = relative ? x - args.roi.x1 : x;
                    const int py = relative ? y - args.roi.y1 : y;
                    for (int c = 0; c < nComps; ++c) {
                        pixel[c] = in ? noise(px, py, c, level) : 0.f;
                    }
                }
            }
        }

        return eStatusOK;
    }

    mutable std::mutex _mutex;
    RectI _rod;
    bool _infinite;
};

/**
 * @brief RegionTestSource whose values follow the window rendered: what a node computing over the
 * window instead of its whole frame looks like, for testing that the checks catch one.
 **/
class WindowedTestFilter
    : public RegionTestSource {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new WindowedTestFilter(n);
    }

    explicit WindowedTestFilter(NodePtr n)
        : RegionTestSource(n)
    {
    }

protected:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kWindowedNodeSpec>();
    }

    virtual bool valuesFollowWindow() const OVERRIDE FINAL
    {
        return true;
    }

private:
    static constexpr NodeSpec kWindowedNodeSpec = {
        .id = kWindowedTestFilterID,
        .label = "Test Windowed Filter",
    };
};

inline void
registerRegionTestSourcesOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerTestSource<RegionTestSource>();
        registerTestSource<WindowedTestFilter>();
    });
}

/**
 * @brief What expectRegionIndependent() renders. The frame is the source's region of definition.
 **/
struct RegionIndependenceOptions {
    // Odd sizes at an origin off the tile grid, so no window lines up with anything by chance.
    int originX = -37;
    int originY = 21;
    int width = 203;
    int height = 157;

    ImageLayerDesc plane = ImageLayerDesc::getRGBAComponents();
    std::vector<unsigned> mipmapLevels = std::vector<unsigned> { 0u, 1u };

    // How far past the frame the reference render reaches, in pixels at each scale; windows that
    // overhang the frame stay inside it.
    int margin = 8;

    // The project format expectInfiniteRoDClamped() sets.
    RectI format = RectI(0, 0, 181, 137);

#ifdef NATRON_HAVE_GPU
    // When set, the GPU variants run too.
    GpuParityRuntime* gpu = nullptr;

    // The node's GPU parity tolerance, for its GPU frame against its CPU frame.
    ParityTolerance gpuTolerance = ParityTolerance::exact();

    // The query the GPU renders use; by default the plane in full with the node's processed channels.
    std::function<GpuQuery(NativeEffectBase& effect, const RenderScale& scale)> gpuQuery;
#endif
};

/**
 * @brief Checks that a node's output does not depend on the region it is asked to render: windows
 * of it rendered alone, and in sequence through its caches, against the same pixels of its whole
 * frame, bit for bit.
 **/
class RegionIndependenceTest
    : public BaseTest {
protected:
    static constexpr double kTime = 1.;

    // Builds the node under test fed by source, which is a fresh RegionTestSource, and returns it.
    using MakeNode = std::function<NodePtr(const NodePtr& source)>;

    virtual void SetUp() OVERRIDE
    {
        registerRegionTestSourcesOnce();
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        RawNodeSupport::setFrameComputedHookForTests(std::function<void(const char*, U64)>());
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createRegionSource(const RectI& rod,
                               bool infinite)
    {
        NodePtr source = createNode(QString::fromUtf8(kRegionTestSourceID));

        EXPECT_TRUE(bool(source));
        RegionTestSource* effect = source ? dynamic_cast<RegionTestSource*>(source->getEffectInstance().get()) : NULL;
        EXPECT_TRUE(effect != NULL);
        if (!effect) {
            return NodePtr();
        }
        effect->setFrame(rod, infinite);

        return source;
    }

    NodePtr createAfter(const char* pluginID,
                        const NodePtr& input,
                        int inputNb = 0)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node)) << pluginID;
        if (node && input) {
            connectNodes(input, node, inputNb, true);
        }

        return node;
    }

    /**
     * @brief The windows checked over frame: a 3x3 uneven tiling, two overlapping windows offset
     * by (5, 3), one at each corner, a 1 px row and a 1 px column, and one past the frame on its
     * left and top sides.
     **/
    static std::vector<RectI> regionTestWindows(const RectI& frame)
    {
        const int w = frame.width();
        const int h = frame.height();
        const int xs[4] = { frame.x1, frame.x1 + (w * 2) / 7, frame.x1 + (w * 3) / 5, frame.x2 };
        const int ys[4] = { frame.y1, frame.y1 + h / 3, frame.y1 + (h * 5) / 8, frame.y2 };
        std::vector<RectI> windows;

        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) {
                windows.push_back(RectI(xs[i], ys[j], xs[i + 1], ys[j + 1]));
            }
        }
        const RectI overlap(frame.x1 + w / 5, frame.y1 + h / 6, frame.x1 + w / 5 + w / 2, frame.y1 + h / 6 + h / 2);
        windows.push_back(overlap);
        windows.push_back(RectI(overlap.x1 + 5, overlap.y1 + 3, overlap.x2 + 5, overlap.y2 + 3));
        const int cw = std::max(1, w / 4 + 1);
        const int ch = std::max(1, h / 4 + 1);
        windows.push_back(RectI(frame.x1, frame.y1, frame.x1 + cw, frame.y1 + ch));
        windows.push_back(RectI(frame.x2 - cw, frame.y1, frame.x2, frame.y1 + ch));
        windows.push_back(RectI(frame.x1, frame.y2 - ch, frame.x1 + cw, frame.y2));
        windows.push_back(RectI(frame.x2 - cw, frame.y2 - ch, frame.x2, frame.y2));
        windows.push_back(RectI(frame.x1, frame.y1 + h / 2, frame.x2, frame.y1 + h / 2 + 1));
        windows.push_back(RectI(frame.x1 + w / 3, frame.y1, frame.x1 + w / 3 + 1, frame.y2));
        windows.push_back(RectI(frame.x1 - 6, frame.y2 - h / 3, frame.x1 + w / 3, frame.y2 + 5));

        return windows;
    }

    static float pixelOf(const RenderedPlane& plane,
                         int x,
                         int y,
                         int c)
    {
        const std::size_t nComps = plane.channels.size();

        return plane.pixels[((std::size_t)(y - plane.window.y1) * (std::size_t)plane.window.width() + (std::size_t)(x - plane.window.x1)) * nComps + (std::size_t)c];
    }

    static bool sameBits(float a,
                         float b)
    {
        if (std::isnan(a) && std::isnan(b)) {
            return true;
        }

        return std::memcmp(&a, &b, sizeof(float)) == 0;
    }

    static std::string describe(const RectI& r)
    {
        std::ostringstream s;

        s << "(" << r.x1 << "," << r.y1 << ")-(" << r.x2 << "," << r.y2 << ")";

        return s.str();
    }

    static std::string describeFloat(float v)
    {
        std::uint32_t bits;
        char buf[64];

        std::memcpy(&bits, &v, sizeof(bits));
        std::snprintf(buf, sizeof(buf), "%.9g (0x%08x)", (double)v, (unsigned)bits);

        return buf;
    }

    /**
     * @brief The first pixel of over where got differs from reference in any bit (any NaN equal to
     * any NaN), described, or an empty string when there is none. Both planes cover over.
     **/
    static std::string firstMismatch(const RenderedPlane& reference,
                                     const RenderedPlane& got,
                                     const RectI& over)
    {
        const int nComps = (int)reference.channels.size();

        if ((int)got.channels.size() != nComps) {
            return "the planes have different channels";
        }
        for (int y = over.y1; y < over.y2; ++y) {
            for (int x = over.x1; x < over.x2; ++x) {
                for (int c = 0; c < nComps; ++c) {
                    const float a = pixelOf(reference, x, y, c);
                    const float b = pixelOf(got, x, y, c);
                    if (!sameBits(a, b)) {
                        std::ostringstream s;
                        s << "pixel (" << x << "," << y << ") channel " << c << ": " << describeFloat(b) << " where the reference has " << describeFloat(a);

                        return s.str();
                    }
                }
            }
        }

        return std::string();
    }

    static bool renderPlane(const NodePtr& node,
                            unsigned mipmapLevel,
                            const RectI& window,
                            const ImageLayerDesc& plane,
                            bool byPassCache,
                            RenderedPlane* out,
                            std::string* error)
    {
        std::vector<RenderedPlane> planes;

        if (!renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, std::list<ImageLayerDesc>(1, plane), &planes, error, byPassCache) || (planes.size() != 1)) {
            return false;
        }
        *out = planes[0];

        return true;
    }

    static void destroyChain(const NodePtr& node,
                             const NodePtr& source)
    {
        if (node && (node != source)) {
            node->destroyNode(true, false);
        }
        if (source) {
            source->destroyNode(true, false);
        }
    }

    /**
     * @brief Runs every check of expectRegionIndependent() and returns one line per window that
     * differs from the whole frame. A render that fails is a test failure and a line too.
     **/
    std::vector<std::string> checkRegionIndependent(const MakeNode& makeNode,
                                                    const RegionIndependenceOptions& options = RegionIndependenceOptions())
    {
        std::vector<std::string> mismatches;
        const RectI rod(options.originX, options.originY, options.originX + options.width, options.originY + options.height);

        for (unsigned level : options.mipmapLevels) {
            const RenderScale scale = RenderScale::fromMipmapLevel(level);
            const RectI frame = PassExec::toPixelEnclosing(RectD(rod.x1, rod.y1, rod.x2, rod.y2), scale, 1.);
            const RectI whole(frame.x1 - options.margin, frame.y1 - options.margin, frame.x2 + options.margin, frame.y2 + options.margin);
            std::ostringstream at;
            at << "mipmap level " << level << ", ";
            const std::vector<RectI> windows = regionTestWindows(frame);

            auto failed = [&](const std::string& what, const std::string& error) {
                ADD_FAILURE() << at.str() << what << ": " << error;
                mismatches.push_back(at.str() + what + ": " + error);
            };

            RenderedPlane reference;
            {
                const NodePtr source = createRegionSource(rod, false);
                const NodePtr node = source ? makeNode(source) : NodePtr();
                std::string error;
                const bool ok = node && renderPlane(node, level, whole, options.plane, true, &reference, &error);
                destroyChain(node, source);
                if (!ok) {
                    failed("the whole frame " + describe(whole), node ? error : std::string("no node"));
                    continue;
                }
            }

            for (const RectI& window : windows) {
                const NodePtr source = createRegionSource(rod, false);
                const NodePtr node = source ? makeNode(source) : NodePtr();
                RenderedPlane got;
                std::string error;
                const bool ok = node && renderPlane(node, level, window, options.plane, true, &got, &error);
                destroyChain(node, source);
                if (!ok) {
                    failed("window " + describe(window) + " alone", node ? error : std::string("no node"));
                    continue;
                }
                const std::string mismatch = firstMismatch(reference, got, window);
                if (!mismatch.empty()) {
                    mismatches.push_back(at.str() + "window " + describe(window) + " alone: " + mismatch);
                }
            }

            {
                const NodePtr source = createRegionSource(rod, false);
                const NodePtr node = source ? makeNode(source) : NodePtr();
                appPTR->clearAllCaches();
                for (const RectI& window : windows) {
                    RenderedPlane got;
                    std::string error;
                    if (!node || !renderPlane(node, level, window, options.plane, false, &got, &error)) {
                        failed("window " + describe(window) + " in sequence", node ? error : std::string("no node"));
                        break;
                    }
                    const std::string mismatch = firstMismatch(reference, got, window);
                    if (!mismatch.empty()) {
                        mismatches.push_back(at.str() + "window " + describe(window) + " in sequence: " + mismatch);
                    }
                }
                destroyChain(node, source);
                appPTR->clearAllCaches();
            }

#ifdef NATRON_HAVE_GPU
            if (options.gpu) {
                checkGpu(makeNode, options, rod, level, frame, whole, windows, reference, &mismatches);
            }
#endif
        }

        return mismatches;
    }

    /**
     * @brief Expects every window of the node's output, at every scale of options, to equal its
     * whole frame bit for bit: each window from a fresh chain, then all of them in sequence on one
     * chain through the caches, and with options.gpu set, GPU windows against the GPU frame
     * exactly and the GPU frame against the CPU frame within options.gpuTolerance.
     **/
    void expectRegionIndependent(const MakeNode& makeNode,
                                 const RegionIndependenceOptions& options = RegionIndependenceOptions())
    {
        const std::vector<std::string> mismatches = checkRegionIndependent(makeNode, options);
        std::ostringstream all;

        for (const std::string& line : mismatches) {
            all << "\n  " << line;
        }
        EXPECT_TRUE(mismatches.empty()) << mismatches.size() << " windows differ from the whole frame:" << all.str();
    }

    /**
     * @brief Over an infinite source, expects a window straddling the corner of options.format to
     * equal, inside the format, the node over the source cropped to the format, and outside it the
     * source itself.
     **/
    void expectInfiniteRoDClamped(const MakeNode& makeNode,
                                  const RegionIndependenceOptions& options = RegionIndependenceOptions())
    {
        const RectI& f = options.format;

        getApp()->getProject()->setOrAddProjectFormat(Format(f.x1, f.y1, f.x2, f.y2, "regionIndependenceFormat", 1.));
        for (unsigned level : options.mipmapLevels) {
            const RenderScale scale = RenderScale::fromMipmapLevel(level);
            const RectI format = PassExec::toPixelEnclosing(RectD(f.x1, f.y1, f.x2, f.y2), scale, 1.);
            const RectI window(format.x2 - std::max(2, format.width() / 5), format.y2 - std::max(2, format.height() / 6), format.x2 + 9, format.y2 + 7);
            std::ostringstream at;
            at << "mipmap level " << level << ", window " << describe(window) << ": ";

            const NodePtr infiniteSource = createRegionSource(f, true);
            const NodePtr overInfinite = infiniteSource ? makeNode(infiniteSource) : NodePtr();
            const NodePtr croppedSource = createRegionSource(f, false);
            const NodePtr overCropped = croppedSource ? makeNode(croppedSource) : NodePtr();
            ASSERT_TRUE(bool(overInfinite));
            ASSERT_TRUE(bool(overCropped));
            RenderedPlane got;
            RenderedPlane clamped;
            RenderedPlane source;
            std::string error;
            ASSERT_TRUE(renderPlane(overInfinite, level, window, options.plane, true, &got, &error)) << at.str() << error;
            ASSERT_TRUE(renderPlane(overCropped, level, window, options.plane, true, &clamped, &error)) << at.str() << error;
            ASSERT_TRUE(renderPlane(infiniteSource, level, window, options.plane, true, &source, &error)) << at.str() << error;
            destroyChain(overInfinite, infiniteSource);
            destroyChain(overCropped, croppedSource);

            const RectI inside(window.x1, window.y1, format.x2, format.y2);
            EXPECT_EQ(std::string(), firstMismatch(clamped, got, inside)) << at.str() << "inside the format, against the node over the cropped source";
            const RectI right(format.x2, window.y1, window.x2, window.y2);
            const RectI top(window.x1, format.y2, format.x2, window.y2);
            EXPECT_EQ(std::string(), firstMismatch(source, got, right)) << at.str() << "right of the format, against the source";
            EXPECT_EQ(std::string(), firstMismatch(source, got, top)) << at.str() << "above the format, against the source";
        }
    }

#ifdef NATRON_HAVE_GPU
private:
    static GpuQuery defaultGpuQuery(NativeEffectBase& effect,
                                    const ImageLayerDesc& plane,
                                    const RenderScale& scale)
    {
        GpuQuery query;

        query.time = kTime;
        query.view = ViewIdx(0);
        query.mappedScale = scale;
        query.plane = plane;
        query.nComps = plane.getNumComponents();
        query.bitDepth = eImageBitDepthFloat;
        const std::bitset<4> channels = effect.getProcessChannelsForPlane(effect.getRenderHash(), kTime, ViewIdx(0), plane);
        for (int i = 0; i < 4; ++i) {
            query.processChannels[i] = channels[i];
        }

        return query;
    }

    // renderGpu() over window, fed the CPU render of source over the region it reads.
    static bool renderGpuWindow(GpuParityRuntime& runtime,
                                NativeEffectBase& effect,
                                const GpuQuery& query,
                                const NodePtr& source,
                                unsigned level,
                                const RectI& window,
                                const RenderedPlane& like,
                                RenderedPlane* out,
                                std::string* error)
    {
        const RenderScale scale = RenderScale::fromMipmapLevel(level);
        const RectI inputWindow = effect.getGpuInputRoI(0, kTime, ViewIdx(0), window, scale);
        RenderedPlane input;

        if (!renderPlane(source, level, inputWindow, like.layer, true, &input, error)) {
            return false;
        }
        GpuParityImage image;
        image.bounds = input.window;
        image.nComps = (int)input.channels.size();
        image.pixels = input.pixels;
        GpuRunResult run = runRenderGpu(runtime, effect, query, std::vector<GpuParityImage>(1, image), nullptr, window);
        if (!run.ok || (run.writtenOutside != 0) || (run.pixels.size() != (std::size_t)window.area() * like.channels.size())) {
            *error = "renderGpu() failed or wrote outside its window";

            return false;
        }
        out->layer = like.layer;
        out->window = window;
        out->channels = like.channels;
        out->pixels = std::move(run.pixels);

        return true;
    }

    void checkGpu(const MakeNode& makeNode,
                  const RegionIndependenceOptions& options,
                  const RectI& rod,
                  unsigned level,
                  const RectI& frame,
                  const RectI& whole,
                  const std::vector<RectI>& windows,
                  const RenderedPlane& cpuReference,
                  std::vector<std::string>* mismatches)
    {
        std::ostringstream at;
        at << "mipmap level " << level << ", GPU ";
        const NodePtr source = createRegionSource(rod, false);
        const NodePtr node = source ? makeNode(source) : NodePtr();
        NativeEffectBase* effect = node ? dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()) : NULL;

        if (!effect) {
            ADD_FAILURE() << at.str() << "the node is not native";
            mismatches->push_back(at.str() + "the node is not native");
            destroyChain(node, source);

            return;
        }
        const RenderScale scale = RenderScale::fromMipmapLevel(level);
        const GpuQuery query = options.gpuQuery ? options.gpuQuery(*effect, scale) : defaultGpuQuery(*effect, options.plane, scale);
        if (!effect->canRenderOnGpu(query)) {
            ADD_FAILURE() << at.str() << "canRenderOnGpu() refuses the query";
            mismatches->push_back(at.str() + "canRenderOnGpu() refuses the query");
            destroyChain(node, source);

            return;
        }
        RenderedPlane gpuWhole;
        std::string error;
        if (!renderGpuWindow(*options.gpu, *effect, query, source, level, whole, cpuReference, &gpuWhole, &error)) {
            ADD_FAILURE() << at.str() << "whole frame: " << error;
            mismatches->push_back(at.str() + "whole frame: " + error);
            destroyChain(node, source);

            return;
        }
        for (const RectI& window : windows) {
            RenderedPlane got;
            if (!renderGpuWindow(*options.gpu, *effect, query, source, level, window, cpuReference, &got, &error)) {
                ADD_FAILURE() << at.str() << "window " << describe(window) << ": " << error;
                mismatches->push_back(at.str() + "window " + describe(window) + ": " + error);
                continue;
            }
            const std::string mismatch = firstMismatch(gpuWhole, got, window);
            if (!mismatch.empty()) {
                mismatches->push_back(at.str() + "window " + describe(window) + " against the GPU frame: " + mismatch);
            }
        }
        destroyChain(node, source);

        const int nComps = (int)cpuReference.channels.size();
        std::vector<float> cpu;
        std::vector<float> gpu;
        for (int y = frame.y1; y < frame.y2; ++y) {
            for (int x = frame.x1; x < frame.x2; ++x) {
                for (int c = 0; c < nComps; ++c) {
                    cpu.push_back(pixelOf(cpuReference, x, y, c));
                    gpu.push_back(pixelOf(gpuWhole, x, y, c));
                }
            }
        }
        const ImageDiff diff = compareImages(cpu, gpu, options.gpuTolerance);
        if (diff.mismatches != 0) {
            const std::size_t pixel = diff.firstMismatchIndex / (std::size_t)nComps;
            std::ostringstream s;
            s << at.str() << "frame against the CPU frame: " << diff.mismatches << " of " << diff.compared << " floats outside the tolerance, the first at pixel ("
              << frame.x1 + (int)(pixel % (std::size_t)frame.width()) << "," << frame.y1 + (int)(pixel / (std::size_t)frame.width()) << ") channel "
              << diff.firstMismatchIndex % (std::size_t)nComps << ", the worst by " << diff.worstAbsolute;
            mismatches->push_back(s.str());
        }
    }
#endif // NATRON_HAVE_GPU
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVE_REGIONINDEPENDENCESUPPORT_H
