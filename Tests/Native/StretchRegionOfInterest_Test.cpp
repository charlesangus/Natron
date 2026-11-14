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

#include <cstddef>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "Tests/BaseTest.h"
#include "Tests/CountingTestEffect.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/ResampleExecutor.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kStretchShiftOpPluginID "test.natron.built-in.StretchShiftOp"

NATRON_NAMESPACE_ENTER

namespace {

const double kTime = 1.;
const double kShiftX = 2.;
const float kCountingValue = 0.25f;

// The op's region reaches this far right of its source, so a window there maps outside the source.
const RectD kWideRegion(0., 0., 200., 64.);
const RectD kFarWindow(150., 10., 160., 20.);

constexpr InputSpec kInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief A Resample op that shifts its source kShiftX to the right with the impulse filter and
 * Stretch edges, so each output pixel past the source shows the source's nearest pixel.
 **/
struct StretchShiftOp {
    struct Params {
        int unused = 0;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kStretchShiftOpPluginID,
        .label = "Test Stretch Shift Op",
        .grouping = "Other/Test",
        .inputs = kInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    Params bake(const KnobReader& /*reader*/) const
    {
        return Params();
    }

    bool isIdentity(const Params& /*params*/,
                    const PassContext& /*context*/) const
    {
        return false;
    }

    ResampleSettings settings(const Params& /*params*/,
                              const PassContext& /*context*/) const
    {
        ResampleSettings s;

        s.filter = Resampler::eFilterImpulse;
        s.clamp = false;
        s.blackOutside = false;
        s.invert = false;
        s.transformIsIdentity = false;

        return s;
    }

    bool transform(const Params& /*params*/,
                   const PassContext& /*context*/,
                   double /*amount*/,
                   bool invert,
                   TransformMath::Mat3* matrix) const
    {
        *matrix = invert ? TransformMath::transformCanonical(kShiftX, 0., 1., 1., 0., 0., false, 0., 0., 0.)
                         : TransformMath::inverseTransformCanonical(kShiftX, 0., 1., 1., 0., 0., false, 0., 0., 0.);

        return true;
    }
};

static_assert(MatrixResampleOp<StretchShiftOp>);

class StretchShiftNode
    : public ImageNode<StretchShiftOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new StretchShiftNode(node);
    }

    explicit StretchShiftNode(NodePtr node)
        : ImageNode<StretchShiftOp>(node)
    {
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const StatusEnum status = ImageNode<StretchShiftOp>::getRegionOfDefinition(hash, time, scale, view, rod);
        if (status != eStatusOK) {
            return status;
        }
        rod->merge(kWideRegion);

        return eStatusOK;
    }
};

void
registerStretchShiftNodeOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        EffectInstancePtr node(StretchShiftNode::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;

        functions.insert(std::make_pair("BuildEffect", (void (*)())&StretchShiftNode::BuildEffect));
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
    });
}

void
expectRect(const RectD& want,
           const RectD& got,
           const std::string& what)
{
    EXPECT_EQ(want.x1, got.x1) << what;
    EXPECT_EQ(want.y1, got.y1) << what;
    EXPECT_EQ(want.x2, got.x2) << what;
    EXPECT_EQ(want.y2, got.y2) << what;
}

} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

TEST(StretchRegionOfInterest, AMissedRegionBecomesTheNearestEdgeBand)
{
    const RectD rod(0., 0., 64., 48.);

    expectRect(RectD(63., 10., 64., 20.), stretchRegionOfInterest(RectD(100., 10., 120., 20.), rod, 1., 1.), "right");
    expectRect(RectD(0., 10., 1., 20.), stretchRegionOfInterest(RectD(-30., 10., -10., 20.), rod, 1., 1.), "left");
    expectRect(RectD(10., 47., 20., 48.), stretchRegionOfInterest(RectD(10., 60., 20., 70.), rod, 1., 1.), "top");
    expectRect(RectD(10., 0., 20., 1.), stretchRegionOfInterest(RectD(10., -9., 20., -2.), rod, 1., 1.), "bottom");
    expectRect(RectD(0., 0., 1., 1.), stretchRegionOfInterest(RectD(-30., -20., -10., -5.), rod, 1., 1.), "bottom-left corner");
    expectRect(RectD(63., 47., 64., 48.), stretchRegionOfInterest(RectD(70., 50., 80., 60.), rod, 1., 1.), "top-right corner");
    expectRect(RectD(63., 10., 64., 20.), stretchRegionOfInterest(RectD(64., 10., 70., 20.), rod, 1., 1.), "touching the right edge");
    expectRect(RectD(62., 10., 64., 20.), stretchRegionOfInterest(RectD(100., 10., 120., 20.), rod, 2., 1.), "a pixel two units wide");
}

TEST(StretchRegionOfInterest, AnOverlappingOrEmptyRegionIsKept)
{
    const RectD rod(0., 0., 64., 48.);

    expectRect(RectD(60., 40., 80., 60.), stretchRegionOfInterest(RectD(60., 40., 80., 60.), rod, 1., 1.), "overlapping");
    expectRect(RectD(-10., -10., 100., 100.), stretchRegionOfInterest(RectD(-10., -10., 100., 100.), rod, 1., 1.), "covering");
    expectRect(RectD(100., 10., 100., 20.), stretchRegionOfInterest(RectD(100., 10., 100., 20.), rod, 1., 1.), "empty region");
    expectRect(RectD(100., 10., 120., 20.), stretchRegionOfInterest(RectD(100., 10., 120., 20.), RectD(), 1., 1.), "empty source");
}

class StretchRegionOfInterestTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerStretchShiftNodeOnce();
        BaseTest::SetUp();
        CountingTestRegistry::reset();
    }

    virtual void TearDown() OVERRIDE
    {
        CountingTestRegistry::reset();
        BaseTest::TearDown();
    }

    // A counting source of kCountingValue, Black when blackEdge, connected to a new op.
    bool createCountingGraph(bool blackEdge,
                             NodePtr* source,
                             NodePtr* op)
    {
        *source = createNode(QString::fromUtf8(kTestPluginIDCounting));
        *op = createNode(QString::fromUtf8(kStretchShiftOpPluginID));
        EXPECT_TRUE(bool(*source));
        EXPECT_TRUE(bool(*op));
        if (!*source || !*op) {
            return false;
        }
        if (!setKnobValues(*source, "value", { (double)kCountingValue }) || !setKnobValues(*source, "blackEdge", { blackEdge ? 1. : 0. })) {
            return false;
        }
        connectNodes(*source, *op, 0, true);

        return true;
    }

    static bool regionsOfInterest(const NodePtr& op,
                                  RoIMap* rois)
    {
        EffectInstancePtr effect = op->getEffectInstance();
        RectD rod;
        bool isProjectFormat = false;
        if (effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat) == eStatusFailed) {
            return false;
        }
        effect->getRegionsOfInterest_public(kTime, RenderScale::identity, rod, kFarWindow, ViewIdx(0), rois);

        return true;
    }

    static bool renderRGBA(const NodePtr& node,
                           unsigned int mipmapLevel,
                           const RectI& window,
                           RenderSchedulerModeEnum mode,
                           std::vector<float>* pixels)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool rendered = renderNodePlanesInMode(node, kTime, ViewIdx(0), mipmapLevel, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), mode, true /*byPassCache*/, &planes, &error);
        EXPECT_TRUE(rendered) << error;
        if (!rendered || (planes.size() != 1) || (planes[0].channels.size() != 4) || (planes[0].window != window)) {
            return false;
        }
        *pixels = planes[0].pixels;

        return true;
    }
};

// A window the op maps wholly outside a Stretch source asks for the source's nearest edge column,
// inside its region; a Black source keeps the region that misses it, so nothing of it is fetched.
TEST_F(StretchRegionOfInterestTest, AStretchInputMissedByTheWindowIsAskedForItsEdge)
{
    for (int black = 0; black < 2; ++black) {
        const std::string what = black ? "black" : "stretch";
        NodePtr source;
        NodePtr op;
        ASSERT_TRUE(createCountingGraph(black != 0, &source, &op)) << what;

        RoIMap rois;
        ASSERT_TRUE(regionsOfInterest(op, &rois)) << what;
        RoIMap::const_iterator found = rois.find(source->getEffectInstance());
        ASSERT_TRUE(found != rois.end()) << what;
        const RectD& roi = found->second;
        if (black) {
            EXPECT_GE(roi.x1, (double)kCountingTestSize) << what;
        } else {
            EXPECT_EQ(kCountingTestSize - 1., roi.x1) << what;
            EXPECT_EQ((double)kCountingTestSize, roi.x2) << what;
            EXPECT_LE(roi.y1, kFarWindow.y1) << what;
            EXPECT_GE(roi.y2, kFarWindow.y2) << what;
            EXPECT_GE(roi.y1, 0.) << what;
            EXPECT_LE(roi.y2, (double)kCountingTestSize) << what;
        }
    }
}

// Rendering that window renders the Stretch source and repeats its value; the Black source is
// never rendered and the window is black.
TEST_F(StretchRegionOfInterestTest, OnlyAStretchInputMissedByTheWindowIsFetched)
{
    const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };

    for (int black = 0; black < 2; ++black) {
        for (RenderSchedulerModeEnum mode : modes) {
            for (unsigned int level = 0; level < 2; ++level) {
                const std::string what = std::string(black ? "black" : "stretch") + (mode == eRenderSchedulerModeLegacy ? ", legacy" : ", task graph") + ", mipmap level " + std::to_string(level);
                NodePtr source;
                NodePtr op;
                ASSERT_TRUE(createCountingGraph(black != 0, &source, &op)) << what;
                CountingTestRegistry::reset();

                const RectI window = kFarWindow.toPixelEnclosing(level, 1.);
                std::vector<float> got;
                ASSERT_TRUE(renderRGBA(op, level, window, mode, &got)) << what;
                ASSERT_EQ((std::size_t)window.width() * window.height() * 4, got.size()) << what;

                const float want = black ? 0.f : kCountingValue;
                std::size_t mismatches = 0;
                for (float v : got) {
                    if (v != want) {
                        ++mismatches;
                    }
                }
                EXPECT_EQ(0u, mismatches) << what << ": want " << want << ", first value " << got[0];
                if (black) {
                    EXPECT_EQ(0, CountingTestRegistry::renders(source)) << what;
                } else {
                    EXPECT_GT(CountingTestRegistry::renders(source), 0) << what;
                }
            }
        }
    }
}

// Each pixel of a window past the right of a Stretch source shows the source's last column, in
// the same row.
TEST_F(StretchRegionOfInterestTest, AWindowPastTheSourceRepeatsItsEdgeColumn)
{
    const RenderSchedulerModeEnum modes[] = { eRenderSchedulerModeLegacy, eRenderSchedulerModeTaskGraph };

    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr op = createNode(QString::fromUtf8(kStretchShiftOpPluginID));
    ASSERT_TRUE(source && op);
    connectNodes(source, op, 0, true);

    const RectI window = kFarWindow.toPixelEnclosing(0, 1.);
    ASSERT_GE(window.y1, 0);
    ASSERT_LE(window.y2, kParitySourceHeight);
    for (RenderSchedulerModeEnum mode : modes) {
        const std::string what = (mode == eRenderSchedulerModeLegacy) ? "legacy" : "task graph";
        std::vector<float> got;
        ASSERT_TRUE(renderRGBA(op, 0, window, mode, &got)) << what;

        std::size_t mismatches = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const std::size_t pixel = ((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4;
                for (int c = 0; c < 4; ++c) {
                    if (got[pixel + c] != paritySourceColorValue(c, kParitySourceWidth - 1, y, kTime)) {
                        ++mismatches;
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches) << what;
    }
}
