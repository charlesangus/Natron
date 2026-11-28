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
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QWaitCondition>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "CacheStress.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLatencyProbe.h"
#include "Engine/RotoPaint.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
using namespace CacheStress;

namespace {

const RectI kWindow(0, 0, 256, 256);

struct DirectCase {
    MergingFunctionEnum op;
    const char* name;
};

void
PrintTo(const DirectCase& c, std::ostream* os)
{
    *os << c.name;
}

// Bounding box of the samples that differ bit for bit, over the planes' shared window.
void
printDiffExtent(const RenderedPlane& a,
                const RenderedPlane& b,
                const std::string& where)
{
    std::cout << "DIAG " << where << " windows a=" << a.window.x1 << ',' << a.window.y1 << ' ' << a.window.x2 << ',' << a.window.y2
              << " b=" << b.window.x1 << ',' << b.window.y1 << ' ' << b.window.x2 << ',' << b.window.y2
              << " source bounds assumed " << kWindow.x1 << ',' << kWindow.y1 << ' ' << kWindow.x2 << ',' << kWindow.y2 << std::endl;
    const std::size_t ca = a.channels.size();
    const std::size_t cb = b.channels.size();
    if ((ca == 0) || (ca != cb) || (a.window.width() <= 0) || (a.window.x1 != b.window.x1) || (a.window.y1 != b.window.y1) || (a.window.x2 != b.window.x2) || (a.window.y2 != b.window.y2)) {
        return;
    }
    const std::size_t w = (std::size_t)a.window.width();
    const std::size_t n = w * (std::size_t)a.window.height();
    if ((a.pixels.size() != n * ca) || (b.pixels.size() != n * cb)) {
        return;
    }
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    std::size_t count = 0;
    std::size_t insideSource = 0;
    for (std::size_t p = 0; p < n; ++p) {
        bool differs = false;
        for (std::size_t c = 0; c < ca; ++c) {
            if (std::memcmp(&a.pixels[p * ca + c], &b.pixels[p * cb + c], sizeof(float)) != 0) {
                differs = true;
                break;
            }
        }
        if (!differs) {
            continue;
        }
        const int x = a.window.x1 + (int)(p % w);
        const int y = a.window.y1 + (int)(p / w);
        if (count == 0) {
            minX = maxX = x;
            minY = maxY = y;
            std::cout << "DIAG " << where << " first differing pixel (" << x << ',' << y << ") relative to roi origin (" << (x - a.window.x1) << ',' << (y - a.window.y1) << ')' << std::endl;
        }
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
        if (kWindow.contains(x, y)) {
            ++insideSource;
        }
        ++count;
    }
    std::cout << "DIAG " << where << " differing pixels=" << count << " inside source bounds=" << insideSource << " bbox=" << minX << ',' << minY << ' ' << maxX << ',' << maxY << std::endl;
}

// Threads a test splits a mask fetch across, whatever the machine or its load.
const int kParallelFetchThreads = 4;

// Gives the global pool room for a forced split for the scope's lifetime, so a helper always has a
// free thread to start on.
class PoolRoomScope {
public:
    explicit PoolRoomScope(int threads)
        : _previous(QThreadPool::globalInstance()->maxThreadCount())
    {
        QThreadPool::globalInstance()->setMaxThreadCount(std::max(_previous, threads + 1));
    }

    ~PoolRoomScope()
    {
        QThreadPool::globalInstance()->setMaxThreadCount(_previous);
    }

    PoolRoomScope(const PoolRoomScope&) = delete;
    PoolRoomScope& operator=(const PoolRoomScope&) = delete;

private:
    int _previous;
};

// Installed as the mask fetch hook: holds the first fetches until `parties` threads are fetching at
// once, then lets every fetch through, and records the threads it saw. A fetch that stays serial
// gives up waiting after a while, leaving fewer threads seen than parties.
class FetchRendezvous {
public:
    FetchRendezvous() = default;

    void arrive()
    {
        QMutexLocker locker(&_mutex);
        _threads.insert(std::this_thread::get_id());
        if (_met) {
            return;
        }
        if (++_waiting >= _parties) {
            _met = true;
            _allHere.wakeAll();

            return;
        }
        while (!_met) {
            if (!_allHere.wait(&_mutex, 5000)) {
                break;
            }
        }
        if (!_met) {
            _met = true;
            _allHere.wakeAll();
        }
    }

    std::size_t threadsSeen()
    {
        QMutexLocker locker(&_mutex);

        return _threads.size();
    }

    void reset(std::size_t parties)
    {
        QMutexLocker locker(&_mutex);
        _parties = parties;
        _threads.clear();
        _waiting = 0;
        _met = false;
    }

private:
    std::size_t _parties = 1;
    QMutex _mutex;
    QWaitCondition _allHere;
    std::set<std::thread::id> _threads;
    std::size_t _waiting = 0;
    bool _met = false;
};

class RotoPaintDirectCompositeTest
    : public BaseTest,
      public ::testing::WithParamInterface<DirectCase> {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        RotoRenderProfile::stop();
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        RotoPaint::setDirectCompositeEnabled(true);
        RotoPaint::setMaskFetchThreadCount(0);
        RotoPaint::setDirectMaskFetchHook(std::function<void()>());
    }

    void TearDown() override
    {
        RotoRenderProfile::stop();
        RotoPaint::setDirectCompositeEnabled(true);
        RotoPaint::setMaskFetchThreadCount(0);
        RotoPaint::setDirectMaskFetchHook(std::function<void()>());
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }

    // Every third item is inactive. Unless negativeColours, colours stay non-negative so
    // non-separable operators see valid input; negative ones leave -0 where coverage is zero.
    // With paritySource the input is a parity source, whose depth and components a test can set.
    void buildScene(int itemCount,
                    MergingFunctionEnum op,
                    bool negativeColours = false,
                    bool paritySource = false)
    {
        const AppInstancePtr app = getApp();
        _roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        ASSERT_NE(_roto, nullptr);
        _context = _roto->getRotoContext();
        ASSERT_NE(_context, nullptr);
        _context->setAutoKeyingEnabled(false);
        if (paritySource) {
            _source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        } else {
            _source = createConstantIn(app, app->getProject(), kWindow, { 0.2, 0.45, 0.7, 0.6 });
        }
        ASSERT_NE(_source, nullptr);
        ASSERT_TRUE(_roto->connectInput(_source, 0));

        for (int i = 0; i < itemCount; ++i) {
            const double x = 4. + (i * 37) % 200;
            const double y = 6. + (i * 53) % 190;
            const double size = 24. + (i * 11) % 50;
            const BezierPtr shape = _context->makeSquare(x, y, size, 1.);
            ASSERT_NE(shape, nullptr);
            shape->setFeatherDistance((i % 4) * 5., 1.);
            if (negativeColours) {
                shape->setColor(1., -0.05 - ((i * 29) % 100) / 100., ((i * 61) % 100) / 100. - 0.5, 0.3 - ((i * 17) % 100) / 100.);
            } else {
                shape->setColor(1., ((i * 29) % 100) / 100., ((i * 61) % 100) / 100., ((i * 17) % 100) / 100.);
            }
            shape->setOpacity(0.35 + ((i * 13) % 65) / 100., 1.);
            shape->setCompositingOperator((int)op);
            if (i % 3 == 2) {
                shape->setActivated(false, 1.);
            }
        }
        _context->refreshRotoPaintTree();
    }

    bool render(bool direct,
                unsigned mipmapLevel,
                const RectI& roi,
                const std::list<ImageLayerDesc>& layers,
                std::vector<RenderedPlane>* out,
                std::string* error)
    {
        RotoPaint::setDirectCompositeEnabled(direct);
        clearEverything(getApp());
        const bool ok = renderNodePlanesDirect(_roto, 1., ViewIdx(0), mipmapLevel, roi, layers, out, error, true);
        RotoPaint::setDirectCompositeEnabled(true);

        return ok;
    }

    // Both paths over roi, sample for sample; returns how many direct composites the direct pass ran.
    unsigned long long expectPathsMatch(unsigned mipmapLevel,
                                        const RectI& roi,
                                        const std::list<ImageLayerDesc>& layers,
                                        bool diag = false)
    {
        std::ostringstream where;
        where << "mipmap " << mipmapLevel << " roi " << roi.x1 << ',' << roi.y1 << ' ' << roi.x2 << ',' << roi.y2;
        std::string error;
        std::vector<RenderedPlane> nodePath;
        EXPECT_TRUE(render(false, mipmapLevel, roi, layers, &nodePath, &error)) << where.str() << ": " << error;
        if (diag) {
            std::vector<RenderedPlane> again;
            std::string againError;
            EXPECT_TRUE(render(false, mipmapLevel, roi, layers, &again, &againError)) << where.str() << ": " << againError;
            bool consistent = (again.size() == nodePath.size());
            for (std::size_t i = 0; consistent && i < nodePath.size(); ++i) {
                const std::string msg = compareExact(nodePath[i], again[i]);
                if (!msg.empty()) {
                    consistent = false;
                    ADD_FAILURE() << "DIAG node path not self-consistent: " << where.str() << " plane " << i << ": " << msg;
                    printDiffExtent(nodePath[i], again[i], where.str() + " nodepath-vs-nodepath plane " + std::to_string(i));
                }
            }
            if (consistent) {
                std::cout << "DIAG node path self-consistent (" << where.str() << ")" << std::endl;
            }
        }
        const unsigned long long before = RotoPaint::getDirectCompositeRenderCount();
        std::vector<RenderedPlane> direct;
        EXPECT_TRUE(render(true, mipmapLevel, roi, layers, &direct, &error)) << where.str() << ": " << error;
        const unsigned long long after = RotoPaint::getDirectCompositeRenderCount();
        const unsigned long long ran = after - before;
        if (diag) {
            std::cout << "DIAG direct count before=" << before << " after=" << after << " (" << where.str() << ")" << std::endl;
        }
        EXPECT_EQ(nodePath.size(), direct.size()) << where.str();
        for (std::size_t i = 0; i < nodePath.size() && i < direct.size(); ++i) {
            const std::string msg = compareExact(nodePath[i], direct[i]);
            EXPECT_EQ(std::string(), msg) << where.str() << " plane " << i;
            if (diag && !msg.empty()) {
                printDiffExtent(nodePath[i], direct[i], where.str() + " plane " + std::to_string(i));
            }
        }

        return ran;
    }

    // The alpha and RGB plane requests do not depend on the operator, so one operator covers them.
    void expectAllWindowsMatch(bool withOtherPlanes)
    {
        const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
        EXPECT_GT(expectPathsMatch(0, kWindow, rgba), 0ULL);
        EXPECT_GT(expectPathsMatch(0, RectI(150, 170, 320, 300), rgba), 0ULL);
        EXPECT_GT(expectPathsMatch(1, RectI(0, 0, 128, 128), rgba), 0ULL);
        EXPECT_GT(expectPathsMatch(2, RectI(5, 3, 50, 61), rgba), 0ULL);
        if (!withOtherPlanes) {
            return;
        }

        const std::list<ImageLayerDesc> alpha(1, ImageLayerDesc::getAlphaComponents());
        expectPathsMatch(0, kWindow, alpha);
        const std::list<ImageLayerDesc> rgb(1, ImageLayerDesc::getRGBComponents());
        expectPathsMatch(1, RectI(0, 0, 128, 128), rgb);
    }

    // Masks rasterised, rather than served by the cache, while body runs, as the render profile
    // records them; -1 when the profile could not be read.
    int maskMisses(const std::function<void()>& body)
    {
        QTemporaryDir dir;
        if (!dir.isValid()) {
            ADD_FAILURE() << "no temporary directory";

            return -1;
        }
        const QString path = dir.filePath(QString::fromUtf8("roto-profile.jsonl"));
        if (!RotoRenderProfile::start(path)) {
            ADD_FAILURE() << "the render profile did not start";

            return -1;
        }
        body();
        RotoRenderProfile::stop();

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            ADD_FAILURE() << "the render profile wrote nothing";

            return -1;
        }
        int count = 0;
        for (const QByteArray& line : f.readAll().split('\n')) {
            if (line.trimmed().isEmpty()) {
                continue;
            }
            const QJsonObject object = QJsonDocument::fromJson(line).object();
            if ((object.value(QString::fromUtf8("kind")).toString() == QString::fromUtf8("mask")) && !object.value(QString::fromUtf8("hit")).toBool(true)) {
                ++count;
            }
        }

        return count;
    }

    bool renderThroughCache(const RectI& roi,
                            std::vector<RenderedPlane>* out)
    {
        std::string error;
        const bool ok = renderNodePlanesDirect(_roto, 1., ViewIdx(0), 0, roi, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), out, &error, false /*byPassCache*/);
        EXPECT_TRUE(ok) << error;

        return ok;
    }

    // Frame 1 over roi as the viewer asks for it, under abortInfo.
    EffectInstance::RenderRoIRetCode renderAbortable(const AbortableRenderInfoPtr& abortInfo,
                                                     const RectI& roi)
    {
        FrameRequestMap request;
        ParallelRenderArgsSetter frameRenderArgs(1.,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 _roto,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());
        const EffectInstancePtr effect = _roto->getEffectInstance();
        const RectD canonicalWindow = roi.toCanonical_noClipping(0, effect->getTimeInvariantMetadata()->getPixelAspectRatio(-1));
        if (EffectInstance::computeRequestPass(1., ViewIdx(0), 0, canonicalWindow, _roto, request) == eStatusFailed) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
        frameRenderArgs.updateNodesRequest(request);
        EffectInstance::RenderRoIArgs args(1.,
                                           RenderScale::fromMipmapLevel(0),
                                           0 /*mipmapLevel*/,
                                           ViewIdx(0),
                                           true /*byPassCache*/,
                                           roi,
                                           RectD(),
                                           std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()),
                                           eImageBitDepthFloat,
                                           false /*calledFromGetImage*/,
                                           0 /*caller*/,
                                           eStorageModeRAM,
                                           1.);
        std::map<ImageLayerDesc, ImagePtr> planes;
        try {
            return effect->renderRoI(args, &planes);
        } catch (const std::exception&) {
            return EffectInstance::eRenderRoIRetCodeFailed;
        }
    }

    NodePtr _roto;
    NodePtr _source;
    RotoContextPtr _context;
};

TEST_P(RotoPaintDirectCompositeTest, SixteenItemsMatchTheNodePath)
{
    buildScene(16, GetParam().op);
    if (HasFatalFailure()) {
        return;
    }
    expectAllWindowsMatch(GetParam().op == eMergeOver);
}

// More items than one Merge has A inputs, so the composite spans a chain of Merges.
TEST_P(RotoPaintDirectCompositeTest, MergeChainMatchesTheNodePath)
{
    // The chain links through B and does not depend on the operator beyond Copy's replacement.
    const MergingFunctionEnum chainOp = GetParam().op;
    if ((chainOp != eMergeOver) && (chainOp != eMergeCopy)) {
        return;
    }
    buildScene(70, GetParam().op);
    if (HasFatalFailure()) {
        return;
    }
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    EXPECT_GT(expectPathsMatch(0, kWindow, rgba), 0ULL);
    EXPECT_GT(expectPathsMatch(1, RectI(20, 10, 100, 120), rgba), 0ULL);
}

// Negative colours make masks hold -0 where an item has no coverage, which the item's effect
// turns into +0. Hue and ColorBurn are left out: they divide by or take the hue of negatives.
TEST_P(RotoPaintDirectCompositeTest, NegativeColoursMatchTheNodePath)
{
    const MergingFunctionEnum op = GetParam().op;
    if ((op == eMergeHue) || (op == eMergeColorBurn)) {
        return;
    }
    buildScene(16, op, true);
    if (HasFatalFailure()) {
        return;
    }
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    EXPECT_GT(expectPathsMatch(0, kWindow, rgba), 0ULL);
    EXPECT_GT(expectPathsMatch(1, RectI(0, 0, 128, 128), rgba), 0ULL);
}

// An input of another depth or with fewer channels reaches the tree converted; whichever path
// renders it, the output matches the node path.
TEST_F(RotoPaintDirectCompositeTest, InputDepthsAndComponentsMatchTheNodePath)
{
    buildScene(30, eMergeOver, false, true);
    if (HasFatalFailure()) {
        return;
    }
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    const char* depths[] = { "float", "short", "byte" };
    const char* components[] = { "rgba", "rgb", "alpha" };
    for (const char* depth : depths) {
        for (const char* comps : components) {
            SCOPED_TRACE(std::string(depth) + " " + comps);
            setParitySourceBitDepth(_source, depth);
            setParitySourceComponents(_source, comps);
            const bool integerDepth = (std::string(depth) != "float");
            expectPathsMatch(0, kWindow, rgba, integerDepth);
            expectPathsMatch(1, RectI(0, 0, 128, 128), rgba, integerDepth);
        }
    }
}

// An image without a bitmap claims every pixel of its bounds, so it answers only a request it covers at its
// own level, never a wider one nor, through a downscale, a coarser one.
TEST_F(RotoPaintDirectCompositeTest, UnmarkedImagesServeOnlyWhatTheirBoundsHold)
{
    buildScene(1, eMergeOver, false, true);
    if (HasFatalFailure()) {
        return;
    }
    const EffectInstancePtr effect = _source->getEffectInstance();
    ASSERT_NE(effect, nullptr);
    const ImageLayerDesc& rgba = ImageLayerDesc::getRGBAComponents();
    const RectD rod(0., 0., 64., 64.);
    const RectI fullBounds(0, 0, 64, 64);
    const RectI held(0, 0, 64, 16);
    const ImageKey key(_source.get(), 0x5eedULL, false, 0., ViewIdx(0), 1., false, false);
    const ImagePtr partial = std::make_shared<Image>(key, Image::makeParams(rod, held, 1., 0, false, rgba, eImageBitDepthFloat, eImageFieldingOrderNone));
    ASSERT_FALSE(partial->usesBitMap());
    EffectInstance::InputImagesMap inputImages;
    inputImages[0].push_back(partial);

    const auto lookup = [&](unsigned int mipmapLevel, const RectI& bounds, const RectI& roi) {
        ImagePtr found;
        effect->getImageFromCacheAndConvertIfNeeded(false, eStorageModeRAM, eStorageModeRAM, key, mipmapLevel, &bounds, &rod, roi,
                                                    eImageBitDepthFloat, rgba, inputImages, RenderStatsPtr(), OSGLContextAttacherPtr(), &found);

        return found;
    };

    EXPECT_TRUE(lookup(0, fullBounds, RectI(0, 0, 32, 16)) == partial);
    EXPECT_TRUE(lookup(0, held, RectI(0, 0, 64, 40)) == partial);
    EXPECT_TRUE(lookup(0, fullBounds, fullBounds) == ImagePtr());
    EXPECT_TRUE(lookup(0, fullBounds, RectI(0, 8, 64, 24)) == ImagePtr());
    EXPECT_TRUE(lookup(1, RectI(0, 0, 32, 32), RectI(0, 0, 32, 8)) == ImagePtr());
}

TEST_F(RotoPaintDirectCompositeTest, MixedOperatorsKeepTheNodePath)
{
    buildScene(6, eMergeOver);
    if (HasFatalFailure()) {
        return;
    }
    std::list<RotoDrawableItemPtr> items = _context->getCurvesByRenderOrder(false);
    ASSERT_FALSE(items.empty());
    items.front()->setCompositingOperator((int)eMergePlus);
    _context->refreshRotoPaintTree();
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    EXPECT_EQ(0ULL, expectPathsMatch(0, kWindow, rgba));
}

// Masks fetched concurrently composite exactly as masks fetched one after another, and are
// cached under the same keys: either way of fetching is served what the other rasterised.
TEST_F(RotoPaintDirectCompositeTest, ParallelMaskFetchMatchesSerialFetch)
{
    buildScene(40, eMergeOver);
    if (HasFatalFailure()) {
        return;
    }
    PoolRoomScope poolRoom(kParallelFetchThreads);
    FetchRendezvous rendezvous;
    RotoPaint::setDirectMaskFetchHook([&rendezvous]() { rendezvous.arrive(); });
    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    const RectI windows[] = { kWindow, RectI(20, 10, 100, 120) };
    for (const RectI& roi : windows) {
        SCOPED_TRACE(std::to_string(roi.x1) + "," + std::to_string(roi.y1) + " " + std::to_string(roi.x2) + "," + std::to_string(roi.y2));
        std::string error;
        std::vector<RenderedPlane> serial;
        RotoPaint::setMaskFetchThreadCount(1);
        rendezvous.reset(1);
        EXPECT_TRUE(render(true, 0, roi, rgba, &serial, &error)) << error;
        EXPECT_EQ(1u, rendezvous.threadsSeen());
        RotoPaint::setMaskFetchThreadCount(kParallelFetchThreads);
        rendezvous.reset(2);
        const unsigned long long before = RotoPaint::getDirectCompositeRenderCount();
        std::vector<RenderedPlane> parallel;
        EXPECT_TRUE(render(true, 0, roi, rgba, &parallel, &error)) << error;
        EXPECT_GT(RotoPaint::getDirectCompositeRenderCount(), before);
        EXPECT_GE(rendezvous.threadsSeen(), 2u);
        ASSERT_EQ(serial.size(), parallel.size());
        for (std::size_t i = 0; i < serial.size(); ++i) {
            EXPECT_EQ(std::string(), compareExact(serial[i], parallel[i])) << "plane " << i;
        }
    }

    // An upstream edit re-renders the tree but keeps every mask.
    clearEverything(getApp());
    std::vector<RenderedPlane> out;
    RotoPaint::setMaskFetchThreadCount(1);
    rendezvous.reset(1);
    EXPECT_GT(maskMisses([&]() { renderThroughCache(kWindow, &out); }), 0);
    ASSERT_TRUE(setKnobValues(_source, kConstantParamColor, { 0.3, 0.4, 0.5, 0.7 }));
    RotoPaint::setMaskFetchThreadCount(kParallelFetchThreads);
    rendezvous.reset(2);
    EXPECT_EQ(0, maskMisses([&]() { renderThroughCache(kWindow, &out); }));
    EXPECT_GE(rendezvous.threadsSeen(), 2u);
    clearEverything(getApp());
    rendezvous.reset(2);
    EXPECT_GT(maskMisses([&]() { renderThroughCache(kWindow, &out); }), 0);
    EXPECT_GE(rendezvous.threadsSeen(), 2u);
    ASSERT_TRUE(setKnobValues(_source, kConstantParamColor, { 0.1, 0.2, 0.3, 0.9 }));
    RotoPaint::setMaskFetchThreadCount(1);
    rendezvous.reset(1);
    EXPECT_EQ(0, maskMisses([&]() { renderThroughCache(kWindow, &out); }));
    RotoPaint::setDirectMaskFetchHook(std::function<void()>());
}

// Items of the same content share one cached mask. Fetched at once on different threads, none may
// be served it before it is whole: every parallel render matches the serial one bit for bit.
TEST_F(RotoPaintDirectCompositeTest, IdenticalItemsFetchedInParallelMatchSerialFetch)
{
    const AppInstancePtr app = getApp();
    _roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
    ASSERT_NE(_roto, nullptr);
    _context = _roto->getRotoContext();
    ASSERT_NE(_context, nullptr);
    _context->setAutoKeyingEnabled(false);
    _source = createConstantIn(app, app->getProject(), kWindow, { 0.2, 0.45, 0.7, 0.6 });
    ASSERT_NE(_source, nullptr);
    ASSERT_TRUE(_roto->connectInput(_source, 0));

    // Four contents of very different raster cost, each three times over, plus one leading
    // duplicate: workers that finish a cheap mask claim a copy of one still being drawn.
    const int contents = 4;
    const int copies = 3;
    std::vector<int> order(1, 0);
    for (int c = 0; c < copies; ++c) {
        for (int k = 0; k < contents; ++k) {
            order.push_back(k);
        }
    }
    for (int k : order) {
        const double size = 220. - k * 60.;
        const BezierPtr shape = _context->makeSquare(10. + k * 7., 12. + k * 5., size, 1.);
        ASSERT_NE(shape, nullptr);
        shape->setFeatherDistance(40. - k * 10., 1.);
        shape->setColor(1., 0.2 + k * 0.2, 0.9 - k * 0.15, 0.4 + k * 0.1);
        shape->setOpacity(0.5 + k * 0.1, 1.);
        shape->setCompositingOperator((int)eMergeOver);
    }
    _context->refreshRotoPaintTree();

    std::vector<RenderedPlane> out;
    clearEverything(app);
    RotoPaint::setMaskFetchThreadCount(1);
    const int coldMisses = maskMisses([&]() { renderThroughCache(kWindow, &out); });
    EXPECT_GT(coldMisses, 0);
    EXPECT_LT(coldMisses, (int)order.size());

    const std::list<ImageLayerDesc> rgba(1, ImageLayerDesc::getRGBAComponents());
    std::string error;
    std::vector<RenderedPlane> serial;
    ASSERT_TRUE(render(true, 0, kWindow, rgba, &serial, &error)) << error;

    PoolRoomScope poolRoom(kParallelFetchThreads);
    FetchRendezvous rendezvous;
    RotoPaint::setDirectMaskFetchHook([&rendezvous]() { rendezvous.arrive(); });
    RotoPaint::setMaskFetchThreadCount(kParallelFetchThreads);
    const int repeats = 40;
    for (int r = 0; r < repeats; ++r) {
        SCOPED_TRACE("repeat " + std::to_string(r));
        rendezvous.reset(kParallelFetchThreads);
        std::vector<RenderedPlane> parallel;
        EXPECT_TRUE(render(true, 0, kWindow, rgba, &parallel, &error)) << error;
        EXPECT_GE(rendezvous.threadsSeen(), (std::size_t)kParallelFetchThreads);
        ASSERT_EQ(serial.size(), parallel.size());
        for (std::size_t i = 0; i < serial.size(); ++i) {
            ASSERT_EQ(std::string(), compareExact(serial[i], parallel[i])) << "plane " << i;
        }
    }
    RotoPaint::setDirectMaskFetchHook(std::function<void()>());
}

// An abort while the masks are fetched starts no further fetch beyond those already running on
// other threads, none runs once the render has returned, and what it left cached is whole.
TEST_F(RotoPaintDirectCompositeTest, AbortWhileFetchingMasksReturnsPromptly)
{
    buildScene(240, eMergeOver);
    if (HasFatalFailure()) {
        return;
    }
    PoolRoomScope poolRoom(kParallelFetchThreads);
    RotoPaint::setMaskFetchThreadCount(kParallelFetchThreads);
    const int activeItems = 160;
    const int abortAt = 4;
    const AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
    std::atomic<int> fetches(0);
    FetchRendezvous rendezvous;
    rendezvous.reset(2);
    RotoPaint::setDirectMaskFetchHook([&]() {
        rendezvous.arrive();
        if (++fetches == abortAt) {
            abortInfo->setAborted();
        }
    });
    clearEverything(getApp());
    const unsigned long long before = RotoPaint::getDirectCompositeRenderCount();
    const EffectInstance::RenderRoIRetCode code = renderAbortable(abortInfo, kWindow);
    const int atReturn = fetches.load();
    QThread::msleep(200);
    const int afterWait = fetches.load();
    RotoPaint::setDirectMaskFetchHook(std::function<void()>());

    EXPECT_NE(EffectInstance::eRenderRoIRetCodeFailed, code);
    EXPECT_GE(rendezvous.threadsSeen(), 2u);
    EXPECT_EQ(before, RotoPaint::getDirectCompositeRenderCount());
    EXPECT_GE(atReturn, abortAt);
    EXPECT_LE(atReturn, abortAt + kParallelFetchThreads);
    EXPECT_LT(atReturn, activeItems);
    EXPECT_EQ(atReturn, afterWait);

    // The edit makes the tree render again over the masks the aborted render cached.
    ASSERT_TRUE(setKnobValues(_source, kConstantParamColor, { 0.3, 0.4, 0.5, 0.7 }));
    std::vector<RenderedPlane> overCachedMasks;
    renderThroughCache(kWindow, &overCachedMasks);
    std::vector<RenderedPlane> cold;
    std::string error;
    EXPECT_TRUE(render(true, 0, kWindow, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &cold, &error)) << error;
    ASSERT_EQ(cold.size(), overCachedMasks.size());
    for (std::size_t i = 0; i < cold.size(); ++i) {
        EXPECT_EQ(std::string(), compareExact(cold[i], overCachedMasks[i])) << "plane " << i;
    }
}

// One operator per code path: Copy replaces B, Over and Stencil are the identity for a transparent
// A and differ in which alpha they read, Multiply is separable and not an identity, Hue is the
// non-separable path. The rest (Plus, Screen, Min, Max, Difference, ATop, ColorBurn) fall in those
// classes and run the same direct composite code.
const DirectCase kCases[] = {
    { eMergeCopy, "Copy" },
    { eMergeOver, "Over" },
    { eMergeMultiply, "Multiply" },
    { eMergeStencil, "Stencil" },
    { eMergeHue, "Hue" },
};

} // namespace

INSTANTIATE_TEST_CASE_P(Operators, RotoPaintDirectCompositeTest, ::testing::ValuesIn(kCases));
