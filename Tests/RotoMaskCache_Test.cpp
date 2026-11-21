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

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "CacheStress.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/CacheEntry.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLatencyProbe.h"
#include "Engine/TimeLine.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const RectI kWindow(0, 0, 256, 256);
const int kFrames = 5;

struct MaskPixels {
    RectI bounds;
    unsigned int components = 0;
    std::vector<float> data;
};

MaskPixels
readMask(const ImagePtr& image)
{
    MaskPixels out;

    if (!image || (image->getBitDepth() != eImageBitDepthFloat)) {
        return out;
    }
    out.bounds = image->getBounds();
    out.components = image->getComponentsCount();
    Image::ReadAccess access = image->getReadRights();
    for (int y = out.bounds.y1; y < out.bounds.y2; ++y) {
        const float* row = reinterpret_cast<const float*>(access.pixelAt(out.bounds.x1, y));
        out.data.insert(out.data.end(), row, row + (std::size_t)out.bounds.width() * out.components);
    }

    return out;
}

bool
sameMask(const MaskPixels& a,
         const MaskPixels& b)
{
    return (a.bounds == b.bounds) && (a.components == b.components) && !a.data.empty() && (a.data.size() == b.data.size()) && (std::memcmp(a.data.data(), b.data.data(), a.data.size() * sizeof(float)) == 0);
}

// The masks are counted through the render profile, whose records say whether the cache served
// each request; a miss is one rasterisation.
class RotoMaskCacheTest
    : public BaseTest {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        RotoRenderProfile::stop();
        _savedMode = appPTR->getRenderSchedulerMode();
        appPTR->setRenderSchedulerMode(eRenderSchedulerModeLegacy);
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
    }

    void TearDown() override
    {
        RotoRenderProfile::stop();
        appPTR->setRenderSchedulerMode(_savedMode);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }

    // Constant -> Grade -> Roto, with two shapes and no keyframe.
    void buildScene()
    {
        const AppInstancePtr app = getApp();
        _roto = createNode(QString::fromUtf8(PLUGINID_NATRON_ROTO));
        ASSERT_TRUE(_roto != nullptr);
        _context = _roto->getRotoContext();
        ASSERT_TRUE(_context != nullptr);
        _context->setAutoKeyingEnabled(false);
        _constant = createConstantIn(app, app->getProject(), kWindow, { 0.25, 0.5, 0.75, 1. });
        _grade = createIn(app, app->getProject(), PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE);
        ASSERT_TRUE(_constant != nullptr);
        ASSERT_TRUE(_grade != nullptr);
        ASSERT_TRUE(_grade->connectInput(_constant, 0));
        ASSERT_TRUE(_roto->connectInput(_grade, 0));
        _first = _context->makeSquare(20., 20., 60., 1.);
        _second = _context->makeSquare(120., 100., 80., 1.);
        ASSERT_TRUE(_first != nullptr);
        ASSERT_TRUE(_second != nullptr);
        _first->setFeatherDistance(8., 1.);
        _second->setFeatherDistance(12., 1.);
        _context->refreshRotoPaintTree();
    }

    ImagePtr mask(const BezierPtr& shape,
                  double time,
                  const RectD& sourceRod = RectD())
    {
        return shape->renderMaskFromStroke(ImageLayerDesc::getAlphaComponents(), time, ViewIdx(0), eImageBitDepthFloat, 0, sourceRod);
    }

    // Rasterisations per item name while body runs.
    template <typename Body>
    std::map<std::string, int> misses(Body body)
    {
        std::map<std::string, int> counts;
        QTemporaryDir dir;
        if (!dir.isValid()) {
            ADD_FAILURE() << "no temporary directory";

            return counts;
        }
        const QString path = dir.filePath(QString::fromUtf8("roto-profile.jsonl"));
        if (!RotoRenderProfile::start(path)) {
            ADD_FAILURE() << "the render profile did not start";

            return counts;
        }
        body();
        RotoRenderProfile::stop();

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            return counts;
        }
        for (const QByteArray& line : f.readAll().split('\n')) {
            if (line.trimmed().isEmpty()) {
                continue;
            }
            const QJsonObject object = QJsonDocument::fromJson(line).object();
            if ((object.value(QString::fromUtf8("kind")).toString() == QString::fromUtf8("mask")) && !object.value(QString::fromUtf8("hit")).toBool(true)) {
                ++counts[object.value(QString::fromUtf8("item")).toString().toStdString()];
            }
        }

        return counts;
    }

    int missesOf(const std::map<std::string, int>& counts,
                 const BezierPtr& shape) const
    {
        const std::map<std::string, int>::const_iterator it = counts.find(shape->getScriptName());

        return (it == counts.end()) ? 0 : it->second;
    }

    // Each mask over frames 1..kFrames, with the rasterisations it took.
    int rastersOverFrames(const BezierPtr& shape)
    {
        const std::map<std::string, int> counts = misses([&]() {
            for (int f = 1; f <= kFrames; ++f) {
                EXPECT_TRUE(mask(shape, f) != nullptr) << "frame " << f;
            }
        });

        return missesOf(counts, shape);
    }

    // Each frame's mask, as the cache holds it now, matches one drawn from an empty cache.
    void expectMasksMatchColdOnes(const BezierPtr& shape)
    {
        std::vector<MaskPixels> warm;
        for (int f = 1; f <= kFrames; ++f) {
            warm.push_back(readMask(mask(shape, f)));
        }
        for (int f = 1; f <= kFrames; ++f) {
            appPTR->clearAllCaches();
            EXPECT_TRUE(sameMask(readMask(mask(shape, f)), warm[f - 1])) << "frame " << f;
        }
    }

    // The Roto output over frames 1..kFrames with what the caches hold matches the cold one.
    void expectRendersMatchColdOnes()
    {
        std::vector<RenderedPlane> warm(kFrames);
        std::string error;
        for (int f = 1; f <= kFrames; ++f) {
            ASSERT_TRUE(renderProbe(_roto, f, 0, kWindow, eRenderSchedulerModeLegacy, false, &warm[f - 1], &error)) << error;
        }
        for (int f = 1; f <= kFrames; ++f) {
            clearEverything(getApp());
            RenderedPlane cold;
            ASSERT_TRUE(renderProbe(_roto, f, 0, kWindow, eRenderSchedulerModeLegacy, false, &cold, &error)) << error;
            EXPECT_EQ(std::string(), compareExact(cold, warm[f - 1])) << "frame " << f;
        }
    }

    NodePtr _roto;
    NodePtr _constant;
    NodePtr _grade;
    RotoContextPtr _context;
    BezierPtr _first;
    BezierPtr _second;

private:
    RenderSchedulerModeEnum _savedMode = eRenderSchedulerModeLegacy;
};

} // namespace

TEST_F(RotoMaskCacheTest, AnUpstreamGradeEditRastersNoMask)
{
    buildScene();
    std::map<std::string, int> counts = misses([&]() {
        EXPECT_TRUE(mask(_first, 1.) != nullptr);
        EXPECT_TRUE(mask(_second, 1.) != nullptr);
    });
    EXPECT_EQ(1, missesOf(counts, _first));
    EXPECT_EQ(1, missesOf(counts, _second));

    ASSERT_TRUE(setKnobValues(_grade, kGradeParamWhite, { 2. }));
    counts = misses([&]() {
        EXPECT_TRUE(mask(_first, 1.) != nullptr);
        EXPECT_TRUE(mask(_second, 1.) != nullptr);
    });
    EXPECT_EQ(0, missesOf(counts, _first));
    EXPECT_EQ(0, missesOf(counts, _second));

    expectMasksMatchColdOnes(_first);
    expectRendersMatchColdOnes();
}

TEST_F(RotoMaskCacheTest, EditingOneShapeRastersOnlyItsMask)
{
    buildScene();
    (void)misses([&]() {
        EXPECT_TRUE(mask(_first, 1.) != nullptr);
        EXPECT_TRUE(mask(_second, 1.) != nullptr);
    });

    _first->getOpacityKnob()->setValue(0.5);
    const std::map<std::string, int> counts = misses([&]() {
        EXPECT_TRUE(mask(_first, 1.) != nullptr);
        EXPECT_TRUE(mask(_second, 1.) != nullptr);
    });
    EXPECT_EQ(1, missesOf(counts, _first));
    EXPECT_EQ(0, missesOf(counts, _second));

    expectMasksMatchColdOnes(_first);
    expectMasksMatchColdOnes(_second);
    expectRendersMatchColdOnes();
}

#ifdef NATRON_ROTO_INVERTIBLE
TEST_F(RotoMaskCacheTest, AnInvertedShapeRastersAgainWhenTheSourceRoDChanges)
{
    buildScene();
    _first->getInvertedKnob()->setValue(true);
    const RectD small(0., 0., 256., 256.);
    const RectD large(0., 0., 512., 512.);
    std::map<std::string, int> counts = misses([&]() {
        EXPECT_TRUE(mask(_first, 1., small) != nullptr);
        EXPECT_TRUE(mask(_first, 1., small) != nullptr);
    });
    EXPECT_EQ(1, missesOf(counts, _first));
    counts = misses([&]() {
        EXPECT_TRUE(mask(_first, 1., large) != nullptr);
    });
    EXPECT_EQ(1, missesOf(counts, _first));
}
#endif

TEST_F(RotoMaskCacheTest, AStillShapeRastersOnceOverFrames)
{
    buildScene();
    ASSERT_LE(_first->getKeyframesCount(), 1);
    EXPECT_TRUE(_first->isMaskFrameInvariant());
    EXPECT_EQ(1, rastersOverFrames(_first));

    expectMasksMatchColdOnes(_first);
    expectRendersMatchColdOnes();
}

TEST_F(RotoMaskCacheTest, AKeyedShapeRastersEveryFrame)
{
    buildScene();
    _first->setKeyframe(1.);
    _context->setAutoKeyingEnabled(true);
    _first->movePointByIndex(0, 4., 10., 5.);
    _context->setAutoKeyingEnabled(false);
    ASSERT_GE(_first->getKeyframesCount(), 2);
    EXPECT_FALSE(_first->isMaskFrameInvariant());
    EXPECT_EQ(kFrames, rastersOverFrames(_first));
    EXPECT_EQ(1, rastersOverFrames(_second));

    expectMasksMatchColdOnes(_first);
    expectRendersMatchColdOnes();
}

TEST_F(RotoMaskCacheTest, AnOpacityExpressionOnTheFrameRastersEveryFrame)
{
    buildScene();
    _first->getOpacityKnob()->setExpression(0, "frame / 10.", false, false);
    ASSERT_FALSE(_first->getOpacityKnob()->getExpression(0).empty());
    EXPECT_FALSE(_first->isMaskFrameInvariant());
    EXPECT_EQ(kFrames, rastersOverFrames(_first));
}

TEST_F(RotoMaskCacheTest, AnAnimatedKnobRastersEveryFrame)
{
    buildScene();
    _first->getOpacityKnob()->setValueAtTime(1., 0.2, ViewSpec::all(), 0);
    _first->getOpacityKnob()->setValueAtTime(5., 0.8, ViewSpec::all(), 0);
    EXPECT_FALSE(_first->isMaskFrameInvariant());
    EXPECT_EQ(kFrames, rastersOverFrames(_first));

    expectMasksMatchColdOnes(_first);
    expectRendersMatchColdOnes();
}

TEST_F(RotoMaskCacheTest, ATrackLinkedPointRastersEveryFrame)
{
    buildScene();
    NodePtr tracker = createNode(QString::fromUtf8(PLUGINID_NATRON_TRACKER));
    ASSERT_TRUE(tracker != nullptr);
    ASSERT_TRUE(tracker->getTrackerContext() != nullptr);
    TrackMarkerPtr marker = tracker->getTrackerContext()->createMarker();
    ASSERT_TRUE(marker != nullptr);
    KnobDoublePtr centre = marker->getCenterKnob();
    for (int f = 1; f <= kFrames; ++f) {
        centre->setValueAtTime(f, 50. + 2. * f, ViewSpec::all(), 0);
        centre->setValueAtTime(f, 30. + f, ViewSpec::all(), 1);
    }
    _first->linkPointToTrack(0, marker, 1.);
    ASSERT_TRUE(_first->hasTrackLinks());
    EXPECT_FALSE(_first->isMaskFrameInvariant());
    EXPECT_EQ(kFrames, rastersOverFrames(_first));

    expectMasksMatchColdOnes(_first);
}

TEST_F(RotoMaskCacheTest, AddingASecondKeyMakesAStillShapeRasterEveryFrame)
{
    buildScene();
    _first->setKeyframe(1.);
    ASSERT_EQ(1, _first->getKeyframesCount());
    EXPECT_TRUE(_first->isMaskFrameInvariant());
    EXPECT_EQ(1, rastersOverFrames(_first));

    _context->setAutoKeyingEnabled(true);
    _first->movePointByIndex(0, 4., 10., 5.);
    _context->setAutoKeyingEnabled(false);
    ASSERT_EQ(2, _first->getKeyframesCount());
    EXPECT_FALSE(_first->isMaskFrameInvariant());
    EXPECT_EQ(kFrames, rastersOverFrames(_first));

    expectMasksMatchColdOnes(_first);
}

TEST_F(RotoMaskCacheTest, AMaskCarriesItsMeasuredRasterCost)
{
    buildScene();
    const ImagePtr image = mask(_first, 1.);
    ASSERT_TRUE(image != nullptr);
    ASSERT_TRUE(image->getEntryHeader() != nullptr);
    EXPECT_GT(image->getEntryHeader()->recomputeCostNs.load(), 0.f);

    const ImagePtr served = mask(_first, 1.);
    ASSERT_TRUE(served != nullptr);
    EXPECT_GT(served->getEntryHeader()->recomputeCostNs.load(), 0.f);
}
