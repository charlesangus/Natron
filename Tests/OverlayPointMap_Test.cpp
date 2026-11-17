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
#include <cmath>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"
#include "Tests/Native/SplineWarpTestCurves.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Filter/MapInput.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Nodes/Transform/CornerPin.h"
#include "Engine/Nodes/Transform/CornerPinKnobs.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/ICornerPin.h"
#include "Engine/Nodes/Transform/IDistort.h"
#include "Engine/Nodes/Transform/ITransform.h"
#include "Engine/Nodes/Transform/IWarp.h"
#include "Engine/Nodes/Transform/LensCorrect.h"
#include "Engine/Nodes/Transform/LensDistortion.h"
#include "Engine/Nodes/Transform/Mirror.h"
#include "Engine/Nodes/Transform/Position.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/OverlayMapFetch.h"
#include "Engine/OverlayPointMap.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kWidth = 128;
const int kHeight = 96;
// Every fetch here renders a small image; a stuck one fails the test instead of hanging it.
const int kFetchTimeoutMs = 30000;

#define kTestPluginIDOverlayUV "net.natron.test.OverlayPointMapUV"

// The STMap shift the UV test source encodes, in pixels.
const double kUVShift = 10.;

Point
pt(double x,
   double y)
{
    Point p;

    p.x = x;
    p.y = y;

    return p;
}

double
dist(const Point& a,
     const Point& b)
{
    return std::hypot(a.x - b.x, a.y - b.y);
}

/**
 * @brief An RGBA UV map over the 128 x 96 format, U = (x - 10) / 128 and V = y / 96 at every
 * canonical pixel centre (x, y): an STMap reading it moves the Source 10 pixels right.
 **/
class OverlayUVTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new OverlayUVTestEffect(n);
    }

    explicit OverlayUVTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
        setSupportsRenderScaleMaybe(eSupportsYes);
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

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *rod = RectD(0., 0., kWidth, kHeight);

        return eStatusOK;
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kTestPluginIDOverlayUV,
        .label = "Test Overlay UV",
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }

    virtual StatusEnum getPreferredMetadataAt(double /*time*/,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        metadata.setNComps(-1, 4);
        metadata.setBitDepth(-1, eImageBitDepthFloat);

        return eStatusOK;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const int step = 1 << args.mappedScale.toMipmapLevel();

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
                const float v = (float)((y * step + 0.5 * step) / kHeight);
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    const float u = (float)((x * step + 0.5 * step - kUVShift) / kWidth);
                    const float values[4] = { u, v, 0.f, 1.f };
                    float* pixel = reinterpret_cast<float*>(access.pixelAt(x, y));
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    for (int c = 0; c < nComps; ++c) {
                        pixel[c] = values[c];
                    }
                }
            }
        }

        return eStatusOK;
    }
};

template <typename NODE>
void
registerNode()
{
    EffectInstancePtr node(NODE::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&NODE::BuildEffect));
    LibraryBinary* binary = new LibraryBinary(functions);

    std::list<std::string> grouping;
    node->getPluginGrouping(&grouping);
    QStringList qgrouping;
    for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
        qgrouping.push_back(QString::fromUtf8(it->c_str()));
    }

    Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                       QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
    // Registered after AppManager::load(), so the label-without-suffix the script name is made
    // from was never assigned.
    p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
    p->setOpenGLRenderSupport(node->supportsOpenGLRender());
}

void
registerTestNodes()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerNode<OverlayUVTestEffect>();
    });
}

bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    select->set(value);

    return true;
}

// The barrel the numeric tests invert: radial about (64, 48) by 1 + k r^2, r in units of 64 pixels.
const double kBarrelK = 0.2;

Point
barrel(const Point& p)
{
    const double dx = (p.x - 64.) / 64.;
    const double dy = (p.y - 48.) / 64.;
    const double f = 1. + kBarrelK * (dx * dx + dy * dy);

    return pt(64. + 64. * dx * f, 48. + 64. * dy * f);
}

// The distance from p to the segment ab.
double
segmentDistance(const Point& p,
                const Point& a,
                const Point& b)
{
    const double vx = b.x - a.x;
    const double vy = b.y - a.y;
    const double len2 = vx * vx + vy * vy;
    double t = (len2 > 0.) ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2 : 0.;

    t = std::max(0., std::min(1., t));

    return dist(p, pt(a.x + t * vx, a.y + t * vy));
}
} // namespace

class OverlayPointMapTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        registerTestNodes();
        EXPECT_TRUE(OverlayMapFetch::waitForFetches(kFetchTimeoutMs));
        OverlayMapFetch::clearCache();
        resetProject();
    }

    virtual void TearDown() OVERRIDE
    {
        EXPECT_TRUE(OverlayMapFetch::waitForFetches(kFetchTimeoutMs));
        OverlayMapFetch::clearCache();
        BaseTest::TearDown();
    }

    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "overlayPointMap128x96", 1.));
    }

    NodePtr node(const char* pluginID,
                 int major,
                 const NodePtr& input = NodePtr(),
                 int inputNb = 0)
    {
        NodePtr n = createNode(QString::fromUtf8(pluginID), major);

        EXPECT_TRUE(bool(n)) << pluginID;
        if (n && input) {
            connectNodes(input, n, inputNb, true);
        }

        return n;
    }

    // A Constant of the given colour over the project format.
    NodePtr constant(double r,
                     double g,
                     double b,
                     double a)
    {
        NodePtr c = node(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);

        if (c) {
            EXPECT_TRUE(setKnobValue(c, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
            EXPECT_TRUE(setKnobValues(c, kConstantParamColor, { r, g, b, a }));
        }

        return c;
    }

    // A white one-pixel dot at pixel (x, y), black outside.
    NodePtr dot(int x,
                int y)
    {
        NodePtr c = node(PLUGINID_NATRON_CONSTANT, PLUGIN_MAJOR_NATRON_CONSTANT);

        if (c) {
            EXPECT_TRUE(setKnobValue(c, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(c, kNativeGeneratorParamBottomLeft, { (double)x, (double)y }));
            EXPECT_TRUE(setKnobValues(c, kNativeGeneratorParamSize, { 1., 1. }));
            EXPECT_TRUE(setKnobValues(c, kConstantParamColor, { 1., 1., 1., 1. }));
            setBlackOutside(c, true);
        }

        return c;
    }

    // Bilinear where the node has a filter, so a dot's centroid follows the map.
    void bilinear(const NodePtr& n)
    {
        if (n && n->getKnobByName(kResamplerParamFilterType)) {
            EXPECT_TRUE(setKnobValue(n, kResamplerParamFilterType, std::string("bilinear")));
        }
    }

    // The alpha-weighted centroid of the node's output over the format, canonical.
    bool centroid(const NodePtr& n,
                  Point* c)
    {
        std::list<ImageLayerDesc> layers;
        layers.push_back(ImageLayerDesc::getRGBAComponents());
        std::vector<RenderedPlane> planes;
        std::string error;
        const RectI window(0, 0, kWidth, kHeight);
        const bool ok = renderNodePlanesDirect(n, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
        EXPECT_TRUE(ok) << n->getScriptName() << ": " << error;
        if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
            return false;
        }
        const RenderedPlane& plane = planes[0];
        double sum = 0.;
        double sx = 0.;
        double sy = 0.;
        for (int y = plane.window.y1; y < plane.window.y2; ++y) {
            for (int x = plane.window.x1; x < plane.window.x2; ++x) {
                const float a = plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * 4 + 3];
                sum += a;
                sx += a * (x + 0.5);
                sy += a * (y + 0.5);
            }
        }
        EXPECT_GT(sum, 0.) << n->getScriptName() << ": the dot left the frame";
        if (!(sum > 0.)) {
            return false;
        }
        *c = pt(sx / sum, sy / sum);

        return true;
    }

    static OverlayPointMap::Result map(const NodePtr& n,
                                       OverlayPointMap::Direction direction,
                                       const Point& in,
                                       Point* out,
                                       int inputNb = 0)
    {
        return n->getEffectInstance()->mapOverlayPoint_public(kTime, ViewIdx(0), inputNb, direction, in, out);
    }

    // Maps once, and again after the fetches the first call started have finished.
    static OverlayPointMap::Result mapSettled(const NodePtr& n,
                                              OverlayPointMap::Direction direction,
                                              const Point& in,
                                              Point* out,
                                              int inputNb = 0)
    {
        OverlayPointMap::Result r = map(n, direction, in, out, inputNb);

        for (int i = 0; (i < 4) && (r == OverlayPointMap::ePending); ++i) {
            EXPECT_TRUE(OverlayMapFetch::waitForFetches(kFetchTimeoutMs));
            r = map(n, direction, in, out, inputNb);
        }

        return r;
    }

    static OverlayPointMap::Kind kind(const NodePtr& n)
    {
        return n->getEffectInstance()->overlayMapKind_public(kTime, ViewIdx(0));
    }

    // Forward then inverse returns to p within tolerance.
    static void expectRoundTrip(const NodePtr& n,
                                const Point& p,
                                double tolerance,
                                int inputNb = 0)
    {
        Point q = p;
        Point back = p;

        ASSERT_EQ(OverlayPointMap::eMapped, mapSettled(n, OverlayPointMap::eDirectionForward, p, &q, inputNb)) << n->getScriptName();
        ASSERT_EQ(OverlayPointMap::eMapped, mapSettled(n, OverlayPointMap::eDirectionInverse, q, &back, inputNb)) << n->getScriptName();
        EXPECT_LE(dist(back, p), tolerance) << n->getScriptName() << " at " << p.x << "," << p.y;
    }

    // The dot rendered through n lands where its centre maps forward.
    void expectDotFollowsMap(const NodePtr& n,
                             const Point& centre,
                             double tolerance,
                             int inputNb = 0)
    {
        Point mapped = centre;
        Point rendered;

        ASSERT_EQ(OverlayPointMap::eMapped, mapSettled(n, OverlayPointMap::eDirectionForward, centre, &mapped, inputNb)) << n->getScriptName();
        ASSERT_TRUE(centroid(n, &rendered));
        EXPECT_LE(dist(mapped, rendered), tolerance) << n->getScriptName() << ": mapped " << mapped.x << "," << mapped.y << " rendered " << rendered.x << "," << rendered.y;
    }

    void setCorners(const NodePtr& n,
                    const Point from[4],
                    const Point to[4])
    {
        const char* fromNames[4] = { kCornerPinNodeParamFrom1, kCornerPinNodeParamFrom2, kCornerPinNodeParamFrom3, kCornerPinNodeParamFrom4 };
        const char* toNames[4] = { kCornerPinNodeParamTo1, kCornerPinNodeParamTo2, kCornerPinNodeParamTo3, kCornerPinNodeParamTo4 };

        for (int i = 0; i < 4; ++i) {
            EXPECT_TRUE(setKnobValues(n, fromNames[i], { from[i].x, from[i].y }));
            EXPECT_TRUE(setKnobValues(n, toNames[i], { to[i].x, to[i].y }));
        }
    }
};

namespace {
const Point kDotCentre = { 40.5, 30.5 };
const Point kCornersFrom[4] = { { 0., 0. }, { 128., 0. }, { 128., 96. }, { 0., 96. } };
const Point kCornersTo[4] = { { 8., 4. }, { 120., 10. }, { 124., 90. }, { 2., 92. } };
} // namespace

TEST_F(OverlayPointMapTest, TransformTranslateMapsBothWays)
{
    NodePtr source = dot(40, 30);
    NodePtr transform = node(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    ASSERT_TRUE(source && transform);
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamTranslate, { 7.25, -3.5 }));

    Point q;
    ASSERT_EQ(OverlayPointMap::eMapped, map(transform, OverlayPointMap::eDirectionForward, pt(10., 20.), &q));
    EXPECT_NEAR(17.25, q.x, 1e-9);
    EXPECT_NEAR(16.5, q.y, 1e-9);
    Point back;
    ASSERT_EQ(OverlayPointMap::eMapped, map(transform, OverlayPointMap::eDirectionInverse, q, &back));
    EXPECT_NEAR(10., back.x, 1e-9);
    EXPECT_NEAR(20., back.y, 1e-9);
    EXPECT_EQ(OverlayPointMap::eKindMatrix, kind(transform));
}

TEST_F(OverlayPointMapTest, TransformRotateKeepsTheDistanceToTheCentre)
{
    NodePtr source = dot(40, 30);
    NodePtr transform = node(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    ASSERT_TRUE(source && transform);
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamCenter, { 64., 48. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamRotate, { 30. }));

    const Point centre = pt(64., 48.);
    const Point p = pt(90., 60.);
    Point q;
    ASSERT_EQ(OverlayPointMap::eMapped, map(transform, OverlayPointMap::eDirectionForward, p, &q));
    EXPECT_NEAR(dist(p, centre), dist(q, centre), 1e-9);
    const double ax = p.x - centre.x, ay = p.y - centre.y, bx = q.x - centre.x, by = q.y - centre.y;
    const double angle = std::acos((ax * bx + ay * by) / (std::hypot(ax, ay) * std::hypot(bx, by)));
    EXPECT_NEAR(M_PI / 6., angle, 1e-9);
    expectRoundTrip(transform, p, 1e-9);
    bilinear(transform);
    expectDotFollowsMap(transform, kDotCentre, 0.5);
}

TEST_F(OverlayPointMapTest, TransformScaleMapsBothWays)
{
    NodePtr source = dot(40, 30);
    NodePtr transform = node(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    ASSERT_TRUE(source && transform);
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamCenter, { 64., 48. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamScale, { 1.5, 0.75 }));

    Point q;
    ASSERT_EQ(OverlayPointMap::eMapped, map(transform, OverlayPointMap::eDirectionForward, pt(74., 58.), &q));
    EXPECT_NEAR(64. + 15., q.x, 1e-9);
    EXPECT_NEAR(48. + 7.5, q.y, 1e-9);
    expectRoundTrip(transform, pt(74., 58.), 1e-9);
}

TEST_F(OverlayPointMapTest, CornerPinTakesEachCornerToItsTarget)
{
    NodePtr source = dot(40, 30);
    NodePtr pin = node(PLUGINID_NATRON_CORNERPIN, PLUGIN_MAJOR_NATRON_CORNERPIN, source);
    ASSERT_TRUE(source && pin);
    setCorners(pin, kCornersFrom, kCornersTo);

    for (int i = 0; i < 4; ++i) {
        Point q;
        ASSERT_EQ(OverlayPointMap::eMapped, map(pin, OverlayPointMap::eDirectionForward, kCornersFrom[i], &q)) << i;
        EXPECT_NEAR(kCornersTo[i].x, q.x, 1e-9) << i;
        EXPECT_NEAR(kCornersTo[i].y, q.y, 1e-9) << i;
        Point back;
        ASSERT_EQ(OverlayPointMap::eMapped, map(pin, OverlayPointMap::eDirectionInverse, kCornersTo[i], &back)) << i;
        EXPECT_NEAR(kCornersFrom[i].x, back.x, 1e-9) << i;
        EXPECT_NEAR(kCornersFrom[i].y, back.y, 1e-9) << i;
    }
}

TEST_F(OverlayPointMapTest, ReformatMapsBothWays)
{
    NodePtr source = dot(40, 30);
    NodePtr reformat = node(PLUGINID_NATRON_REFORMAT, PLUGIN_MAJOR_NATRON_REFORMAT, source);
    ASSERT_TRUE(source && reformat);
    ASSERT_TRUE(setKnobValue(reformat, kReformatParamType, std::string(kReformatParamTypeOptionScale)));
    ASSERT_TRUE(setKnobValues(reformat, kReformatParamScale, { 0.75, 0.75 }));

    expectRoundTrip(reformat, pt(10., 20.), 1e-9);
    expectRoundTrip(reformat, pt(100., 70.), 1e-9);
    EXPECT_EQ(OverlayPointMap::eKindMatrix, kind(reformat));
}

TEST_F(OverlayPointMapTest, GradeIsIdentity)
{
    NodePtr source = dot(40, 30);
    NodePtr grade = node(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE, source);
    ASSERT_TRUE(source && grade);

    Point q = pt(-1., -1.);
    EXPECT_EQ(OverlayPointMap::eIdentity, map(grade, OverlayPointMap::eDirectionForward, pt(10., 20.), &q));
    EXPECT_EQ(10., q.x);
    EXPECT_EQ(20., q.y);
    EXPECT_EQ(OverlayPointMap::eIdentity, map(grade, OverlayPointMap::eDirectionInverse, pt(10., 20.), &q));
    EXPECT_EQ(OverlayPointMap::eKindNone, kind(grade));
}

TEST_F(OverlayPointMapTest, DisabledTransformIsIdentity)
{
    NodePtr source = dot(40, 30);
    NodePtr transform = node(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    ASSERT_TRUE(source && transform);
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamTranslate, { 7., 3. }));
    transform->setNodeDisabled(true);

    Point q;
    EXPECT_EQ(OverlayPointMap::eIdentity, map(transform, OverlayPointMap::eDirectionForward, pt(10., 20.), &q));
    EXPECT_EQ(10., q.x);
    EXPECT_EQ(20., q.y);
    EXPECT_EQ(OverlayPointMap::eKindNone, kind(transform));
}

TEST_F(OverlayPointMapTest, NumericInverseOfABarrel)
{
    const OverlayPointMap::PointFn fn = [](const Point& p, Point* out) {
        *out = barrel(p);

        return OverlayPointMap::eMapped;
    };
    OverlayPointMap::InvertOptions options;
    options.tolerance = 1e-10;
    for (const Point& p : { pt(0., 0.), pt(128., 96.), pt(20., 80.), pt(64., 48.), pt(100., 10.) }) {
        const Point target = barrel(p);
        Point found;
        ASSERT_EQ(OverlayPointMap::eMapped, OverlayPointMap::invertNumerically(fn, target, target, &found, options)) << p.x << "," << p.y;
        EXPECT_LE(dist(found, p), 1e-6) << p.x << "," << p.y;
    }
}

TEST_F(OverlayPointMapTest, NumericInverseFallsBackToTheSeedGrid)
{
    // Newton cannot start at the seed, whose difference stencil leaves the domain, so only the grid
    // fallback reaches the root.
    const OverlayPointMap::PointFn fn = [](const Point& p, Point* out) {
        if (p.x < 0.) {
            return OverlayPointMap::eUnmappable;
        }
        *out = pt(p.x * p.x, p.y);

        return OverlayPointMap::eMapped;
    };
    OverlayPointMap::InvertOptions options;
    options.seedRegion = RectD(0., 0., 10., 10.);
    options.seedStep = 1.;
    Point found;
    ASSERT_EQ(OverlayPointMap::eMapped, OverlayPointMap::invertNumerically(fn, pt(49., 3.), pt(0., 3.), &found, options));
    EXPECT_NEAR(7., found.x, 1e-6);
    EXPECT_NEAR(3., found.y, 1e-6);
}

TEST_F(OverlayPointMapTest, SampleGridCoversTheRegion)
{
    const std::vector<Point> grid = OverlayPointMap::sampleGrid(RectD(0., 0., 10., 5.), 4.);
    ASSERT_EQ((std::size_t)(4 * 3), grid.size());
    EXPECT_EQ(0., grid.front().x);
    EXPECT_EQ(0., grid.front().y);
    EXPECT_EQ(10., grid.back().x);
    EXPECT_EQ(5., grid.back().y);
}

TEST_F(OverlayPointMapTest, ChainThroughAWarpRoundTrips)
{
    NodePtr source = dot(40, 30);
    NodePtr first = node(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    NodePtr lens = node(PLUGINID_NATRON_LENSDISTORTION, PLUGIN_MAJOR_NATRON_LENSDISTORTION, first);
    NodePtr second = node(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, lens);
    ASSERT_TRUE(source && first && lens && second);
    ASSERT_TRUE(setKnobValues(first, kTransformNodeParamTranslate, { 5., 3. }));
    ASSERT_TRUE(setKnobValue(lens, kLensDistortionParamModel, std::string(kLensDistortionModelPFBarrel)));
    ASSERT_TRUE(setKnobValues(lens, kLensDistortionParamPFC3, { 0.1 }));
    ASSERT_TRUE(setKnobValues(second, kTransformNodeParamCenter, { 64., 48. }));
    ASSERT_TRUE(setKnobValues(second, kTransformNodeParamRotate, { 10. }));

    OverlayPointMap::OverlayMapChain chain;
    chain.append(first->getEffectInstance(), 0, OverlayPointMap::eDirectionForward);
    chain.append(lens->getEffectInstance(), 0, OverlayPointMap::eDirectionForward);
    chain.append(second->getEffectInstance(), 0, OverlayPointMap::eDirectionForward);
    const OverlayPointMap::OverlayMapChain back = chain.inverted();
    ASSERT_EQ((std::size_t)3, back.legs().size());
    EXPECT_EQ(OverlayPointMap::eDirectionInverse, back.legs().front().direction);

    for (const Point& p : { pt(20., 20.), pt(64., 48.), pt(110., 80.) }) {
        Point q;
        ASSERT_EQ(OverlayPointMap::eMapped, chain.mapPoint(kTime, ViewIdx(0), p, &q));
        EXPECT_GT(dist(p, q), 1.);
        Point r;
        ASSERT_EQ(OverlayPointMap::eMapped, back.mapPoint(kTime, ViewIdx(0), q, &r));
        EXPECT_LE(dist(p, r), 1e-6) << p.x << "," << p.y;
    }
}

TEST_F(OverlayPointMapTest, BarrelLineSubdividesWithinTolerance)
{
    const OverlayPointMap::SeededPointFn fn = [](const Point& p, const Point& /*seed*/, Point* out) {
        *out = barrel(p);

        return OverlayPointMap::eMapped;
    };
    const std::vector<Point> line = { pt(0., 90.), pt(128., 90.) };
    const double tolerancePx = 0.25;
    const double viewportScale = 4.;
    std::vector<std::vector<Point>> runs;
    ASSERT_EQ(OverlayPointMap::eMapped, OverlayPointMap::mapPolyline(fn, line, false, tolerancePx, viewportScale, &runs));
    ASSERT_EQ((std::size_t)1, runs.size());
    const std::vector<Point>& run = runs[0];
    EXPECT_GT(run.size(), (std::size_t)2);
    // Each chord was accepted at its midpoint, so a point between samples may sit a little further.
    double worst = 0.;
    for (int i = 0; i <= 512; ++i) {
        const Point mapped = barrel(pt(128. * i / 512., 90.));
        double nearest = 1e300;
        for (std::size_t k = 0; k + 1 < run.size(); ++k) {
            nearest = std::min(nearest, segmentDistance(mapped, run[k], run[k + 1]));
        }
        worst = std::max(worst, nearest);
    }
    EXPECT_LE(worst * viewportScale, 2. * tolerancePx);
}

TEST_F(OverlayPointMapTest, UnmappableMidpointSplitsThePolyline)
{
    const OverlayPointMap::SeededPointFn fn = [](const Point& p, const Point& /*seed*/, Point* out) {
        if (std::fabs(p.x - 50.) < 5.) {
            return OverlayPointMap::eUnmappable;
        }
        *out = pt(p.x * 2., p.y);

        return OverlayPointMap::eMapped;
    };
    std::vector<std::vector<Point>> runs;
    OverlayPointMap::mapPolyline(fn, { pt(0., 0.), pt(100., 0.) }, false, 0.5, 1., &runs);
    ASSERT_EQ((std::size_t)2, runs.size());
    EXPECT_EQ(0., runs[0].front().x);
    EXPECT_LT(runs[0].back().x, 2. * 45. + 1e-9);
    EXPECT_GT(runs[1].front().x, 2. * 55. - 1e-9);
    EXPECT_EQ(200., runs[1].back().x);
}

TEST_F(OverlayPointMapTest, LensDistortionDotAndRoundTrip)
{
    NodePtr source = dot(40, 30);
    NodePtr lens = node(PLUGINID_NATRON_LENSDISTORTION, PLUGIN_MAJOR_NATRON_LENSDISTORTION, source);
    ASSERT_TRUE(source && lens);
    ASSERT_TRUE(setKnobValue(lens, kLensDistortionParamModel, std::string(kLensDistortionModelPFBarrel)));
    ASSERT_TRUE(setKnobValues(lens, kLensDistortionParamPFC3, { 0.1 }));
    bilinear(lens);
    EXPECT_EQ(OverlayPointMap::eKindNonLinear, kind(lens));

    for (const char* direction : { kLensDistortionDirectionDistort, kLensDistortionDirectionUndistort }) {
        ASSERT_TRUE(setKnobValue(lens, kLensDistortionParamDirection, std::string(direction)));
        Point q;
        ASSERT_EQ(OverlayPointMap::eMapped, map(lens, OverlayPointMap::eDirectionForward, kDotCentre, &q)) << direction;
        EXPECT_GT(dist(q, kDotCentre), 0.1) << direction;
        expectDotFollowsMap(lens, kDotCentre, 0.5);
        expectRoundTrip(lens, pt(20., 20.), 1e-6);
        expectRoundTrip(lens, pt(100., 70.), 1e-6);
    }
}

TEST_F(OverlayPointMapTest, LensCorrectDotAndRoundTrip)
{
    NodePtr source = dot(40, 30);
    NodePtr lens = node(PLUGINID_NATRON_LENSCORRECT, PLUGIN_MAJOR_NATRON_LENSCORRECT, source, kLensCorrectInputSource);
    ASSERT_TRUE(source && lens);
    ASSERT_TRUE(setKnobValue(lens, kLensCorrectParamCamera, std::string("Canon EOS 5D Mark III")));
    ASSERT_TRUE(setKnobValue(lens, kLensCorrectParamLens, std::string("Canon EF 70-200mm f/2.8L IS II USM")));
    ASSERT_TRUE(setKnobValues(lens, kLensCorrectParamFocalLength, { 100. }));
    ASSERT_TRUE(setKnobValues(lens, kLensCorrectParamAperture, { 2.8 }));
    bilinear(lens);
    EXPECT_EQ(OverlayPointMap::eKindNonLinear, kind(lens));

    expectDotFollowsMap(lens, kDotCentre, 0.5);
    for (const Point& p : { pt(20., 20.), pt(64., 48.), pt(100., 70.) }) {
        Point q;
        ASSERT_EQ(OverlayPointMap::eMapped, map(lens, OverlayPointMap::eDirectionInverse, p, &q));
        Point back;
        ASSERT_EQ(OverlayPointMap::eMapped, map(lens, OverlayPointMap::eDirectionForward, q, &back));
        // lensfun computes in single precision, which bounds the forward solve.
        EXPECT_LE(dist(back, p), 1e-3) << p.x << "," << p.y;
    }
}

TEST_F(OverlayPointMapTest, SplineWarpOnePairMovesThirtyPixels)
{
    NodePtr source = dot(48, 38);
    NodePtr warp = node(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, source, kSplineWarpInputSource);
    ASSERT_TRUE(source && warp);
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamBoundaryBBox, { 0. }));
    ASSERT_TRUE(bool(SplineWarpTestCurves::linkQuad(SplineWarpTestCurves::contextOf(warp), 40., 30., 16., 30., 0.)));
    EXPECT_EQ(OverlayPointMap::eKindNonLinear, kind(warp));

    const Point p = pt(48.5, 38.5);
    Point q;
    ASSERT_EQ(OverlayPointMap::eMapped, map(warp, OverlayPointMap::eDirectionForward, p, &q));
    EXPECT_LE(dist(q, pt(p.x + 30., p.y)), 1.);
    Point back;
    ASSERT_EQ(OverlayPointMap::eMapped, map(warp, OverlayPointMap::eDirectionInverse, q, &back));
    EXPECT_LE(dist(back, p), 1e-6);

    ASSERT_TRUE(setKnobValues(warp, kOfxMixParamName, { 0. }));
    Point still;
    EXPECT_EQ(OverlayPointMap::eIdentity, map(warp, OverlayPointMap::eDirectionForward, p, &still));
    EXPECT_EQ(p.x, still.x);
    EXPECT_EQ(p.y, still.y);
    EXPECT_EQ(OverlayPointMap::eKindNone, kind(warp));
}

TEST_F(OverlayPointMapTest, STMapShiftMapsAfterTheFetch)
{
    NodePtr source = dot(40, 30);
    NodePtr uv = createNodeAtMajor(getApp(), kTestPluginIDOverlayUV, -1);
    NodePtr stmap = node(PLUGINID_NATRON_STMAP, PLUGIN_MAJOR_NATRON_STMAP);
    ASSERT_TRUE(source && uv && stmap);
    connectNodes(uv, stmap, kSTMapInputUV, true);
    connectNodes(source, stmap, kSTMapInputSource, true);
    ASSERT_TRUE(setChannelSelect(stmap, kSTMapParamChannelU, "rgba.R"));
    ASSERT_TRUE(setChannelSelect(stmap, kSTMapParamChannelV, "rgba.G"));
    bilinear(stmap);
    EXPECT_EQ(OverlayPointMap::eKindNonLinear, kind(stmap));

    std::atomic<int> notified(0);
    const QMetaObject::Connection connection = QObject::connect(stmap.get(), &Node::overlayMapChanged, [&notified]() { ++notified; });

    const Point p = pt(70.5, 50.5);
    Point q = p;
    EXPECT_EQ(OverlayPointMap::ePending, map(stmap, OverlayPointMap::eDirectionInverse, p, &q, kSTMapInputSource));
    ASSERT_TRUE(OverlayMapFetch::waitForFetches(kFetchTimeoutMs));
    EXPECT_GE(notified.load(), 1);
    QObject::disconnect(connection);
    ASSERT_EQ(OverlayPointMap::eMapped, map(stmap, OverlayPointMap::eDirectionInverse, p, &q, kSTMapInputSource));
    EXPECT_LE(dist(q, pt(p.x - kUVShift, p.y)), 0.5);

    Point f = p;
    ASSERT_EQ(OverlayPointMap::eMapped, mapSettled(stmap, OverlayPointMap::eDirectionForward, p, &f, kSTMapInputSource));
    EXPECT_LE(dist(f, pt(p.x + kUVShift, p.y)), 0.5);
    expectDotFollowsMap(stmap, kDotCentre, 0.5, kSTMapInputSource);
}

TEST_F(OverlayPointMapTest, IDistortConstantFieldMapsExactly)
{
    NodePtr source = dot(40, 30);
    NodePtr field = constant(10., 0., 0., 1.);
    NodePtr idistort = node(PLUGINID_NATRON_IDISTORT, PLUGIN_MAJOR_NATRON_IDISTORT, source, kIDistortInputSource);
    ASSERT_TRUE(source && field && idistort);
    connectNodes(field, idistort, kIDistortInputUV, true);
    ASSERT_TRUE(setChannelSelect(idistort, kIDistortParamChannelU, "rgba.R"));
    ASSERT_TRUE(setChannelSelect(idistort, kIDistortParamChannelV, "rgba.G"));
    bilinear(idistort);
    EXPECT_EQ(OverlayPointMap::eKindNonLinear, kind(idistort));

    const Point p = pt(70.5, 50.5);
    Point q = p;
    EXPECT_EQ(OverlayPointMap::ePending, map(idistort, OverlayPointMap::eDirectionInverse, p, &q));
    ASSERT_EQ(OverlayPointMap::eMapped, mapSettled(idistort, OverlayPointMap::eDirectionInverse, p, &q));
    EXPECT_EQ(p.x + 10., q.x);
    EXPECT_EQ(p.y, q.y);
    Point f = p;
    ASSERT_EQ(OverlayPointMap::eMapped, map(idistort, OverlayPointMap::eDirectionForward, p, &f));
    EXPECT_NEAR(p.x - 10., f.x, 1e-9);
    EXPECT_NEAR(p.y, f.y, 1e-9);
    expectDotFollowsMap(idistort, kDotCentre, 0.5);
}

TEST_F(OverlayPointMapTest, FetchRunsOffTheCallingThread)
{
    NodePtr field = constant(0.25, 0.5, 0., 1.);
    NodePtr source = dot(40, 30);
    NodePtr idistort = node(PLUGINID_NATRON_IDISTORT, PLUGIN_MAJOR_NATRON_IDISTORT, source, kIDistortInputSource);
    ASSERT_TRUE(field && source && idistort);
    connectNodes(field, idistort, kIDistortInputUV, true);

    OverlayMapFetch::Channel channel;
    channel.layer = ImageLayerDesc::getRGBAComponents();
    channel.index = 1;
    const std::vector<OverlayMapFetch::Channel> channels(1, channel);
    OverlayPointMap::Result result = OverlayPointMap::eMapped;
    EXPECT_FALSE(bool(OverlayMapFetch::fetch(idistort->getEffectInstance(), kIDistortInputUV, kTime, ViewIdx(0), channels, &result)));
    EXPECT_EQ(OverlayPointMap::ePending, result);
    ASSERT_TRUE(OverlayMapFetch::waitForFetches(kFetchTimeoutMs));
    const OverlayMapFetch::MapImageConstPtr image = OverlayMapFetch::fetch(idistort->getEffectInstance(), kIDistortInputUV, kTime, ViewIdx(0), channels, &result);
    ASSERT_TRUE(bool(image));
    EXPECT_EQ(OverlayPointMap::eMapped, result);
    EXPECT_NE(std::this_thread::get_id(), image->workerThread);
    EXPECT_EQ(1, image->nChannels);
    float value = 0.f;
    ASSERT_TRUE(image->sample(64., 48., &value));
    EXPECT_EQ(0.5f, value);
}

TEST_F(OverlayPointMapTest, ITransformIsAMatrixUntilAMapIsConnected)
{
    NodePtr source = dot(40, 30);
    NodePtr itransform = node(PLUGINID_NATRON_ITRANSFORM, PLUGIN_MAJOR_NATRON_ITRANSFORM, source, kIWarpInputSource);
    ASSERT_TRUE(source && itransform);
    ASSERT_TRUE(setKnobValues(itransform, kTransformNodeParamTranslate, { 10., 0. }));

    EXPECT_EQ(OverlayPointMap::eKindMatrix, kind(itransform));
    Point q;
    ASSERT_EQ(OverlayPointMap::eMapped, map(itransform, OverlayPointMap::eDirectionForward, pt(20., 20.), &q));
    EXPECT_NEAR(30., q.x, 1e-9);
    EXPECT_NEAR(20., q.y, 1e-9);
    expectRoundTrip(itransform, pt(20., 20.), 1e-9);

    NodePtr half = constant(1., 1., 1., 0.5);
    ASSERT_TRUE(bool(half));
    connectNodes(half, itransform, kIWarpInputMap, true);
    EXPECT_EQ(OverlayPointMap::eKindNonLinear, kind(itransform));
    Point back = pt(70., 50.);
    ASSERT_EQ(OverlayPointMap::eMapped, mapSettled(itransform, OverlayPointMap::eDirectionInverse, pt(70., 50.), &back));
    EXPECT_NEAR(65., back.x, 1e-6);
    EXPECT_NEAR(50., back.y, 1e-6);
    expectRoundTrip(itransform, pt(40., 30.), 1e-4);
}

TEST_F(OverlayPointMapTest, ICornerPinWithoutAMapIsItsMatrix)
{
    NodePtr source = dot(40, 30);
    NodePtr pin = node(PLUGINID_NATRON_ICORNERPIN, PLUGIN_MAJOR_NATRON_ICORNERPIN, source, kIWarpInputSource);
    ASSERT_TRUE(source && pin);
    setCorners(pin, kCornersFrom, kCornersTo);

    EXPECT_EQ(OverlayPointMap::eKindMatrix, kind(pin));
    for (int i = 0; i < 4; ++i) {
        Point q;
        ASSERT_EQ(OverlayPointMap::eMapped, map(pin, OverlayPointMap::eDirectionForward, kCornersFrom[i], &q)) << i;
        EXPECT_NEAR(kCornersTo[i].x, q.x, 1e-6) << i;
        EXPECT_NEAR(kCornersTo[i].y, q.y, 1e-6) << i;
    }
}

TEST_F(OverlayPointMapTest, MatrixNodesDotsFollowTheirMaps)
{
    struct MatrixCase {
        const char* id;
        int major;
        std::function<void(const NodePtr&)> setup;
    };
    const std::vector<MatrixCase> cases = {
        { PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, [](const NodePtr& n) {
             EXPECT_TRUE(setKnobValues(n, kTransformNodeParamCenter, { 64., 48. }));
             EXPECT_TRUE(setKnobValues(n, kTransformNodeParamTranslate, { 6.5, -4. }));
             EXPECT_TRUE(setKnobValues(n, kTransformNodeParamRotate, { 20. }));
             EXPECT_TRUE(setKnobValues(n, kTransformNodeParamScale, { 1.3, 1.3 }));
         } },
        { PLUGINID_NATRON_CORNERPIN, PLUGIN_MAJOR_NATRON_CORNERPIN, [this](const NodePtr& n) {
             setCorners(n, kCornersFrom, kCornersTo);
         } },
        { PLUGINID_NATRON_REFORMAT, PLUGIN_MAJOR_NATRON_REFORMAT, [](const NodePtr& n) {
             EXPECT_TRUE(setKnobValue(n, kReformatParamType, std::string(kReformatParamTypeOptionScale)));
             EXPECT_TRUE(setKnobValues(n, kReformatParamScale, { 0.75, 0.75 }));
         } },
        { PLUGINID_NATRON_CROP, PLUGIN_MAJOR_NATRON_CROP, [](const NodePtr& n) {
             EXPECT_TRUE(setKnobValue(n, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
             EXPECT_TRUE(setKnobValues(n, kNativeGeneratorParamBottomLeft, { 16., 12. }));
             EXPECT_TRUE(setKnobValues(n, kNativeGeneratorParamSize, { 96., 72. }));
             EXPECT_TRUE(setKnobValues(n, kCropParamSoftness, { 0. }));
             EXPECT_TRUE(setKnobValues(n, kCropParamReformat, { 1. }));
         } },
        { PLUGINID_NATRON_MIRROR, PLUGIN_MAJOR_NATRON_MIRROR, [](const NodePtr& n) {
             EXPECT_TRUE(setKnobValues(n, kMirrorParamFlip, { 1. }));
         } },
        { PLUGINID_NATRON_POSITION, PLUGIN_MAJOR_NATRON_POSITION, [](const NodePtr& n) {
             EXPECT_TRUE(setKnobValues(n, kPositionParamTranslate, { 7., -5. }));
         } },
        { PLUGINID_NATRON_ICORNERPIN, PLUGIN_MAJOR_NATRON_ICORNERPIN, [this](const NodePtr& n) {
             setCorners(n, kCornersFrom, kCornersTo);
         } },
    };
    for (const MatrixCase& c : cases) {
        SCOPED_TRACE(c.id);
        resetProject();
        NodePtr source = dot(40, 30);
        NodePtr n = node(c.id, c.major, source);
        ASSERT_TRUE(source && n);
        c.setup(n);
        bilinear(n);
        EXPECT_EQ(OverlayPointMap::eKindMatrix, kind(n));
        Point q;
        EXPECT_EQ(OverlayPointMap::eMapped, map(n, OverlayPointMap::eDirectionForward, kDotCentre, &q));
        expectDotFollowsMap(n, kDotCentre, 0.5);
        expectRoundTrip(n, pt(30., 25.), 1e-9);
    }
}
