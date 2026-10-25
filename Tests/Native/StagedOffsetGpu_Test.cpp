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
#include <bitset>
#include <cmath>
#include <cstddef>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "Tests/BaseTest.h"
#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/ResampleExecutor.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#include <ofxImageEffect.h>

#define kOffsetGpuTestOpPluginID "test.natron.built-in.OffsetGpuTestOp"
#define kPassThroughOffsetGpuTestOpPluginID "test.natron.built-in.PassThroughOffsetGpuTestOp"
#define kClippedOffsetGpuTestOpPluginID "test.natron.built-in.ClippedOffsetGpuTestOp"
#define kMattedOffsetGpuTestOpPluginID "test.natron.built-in.MattedOffsetGpuTestOp"
#define kOffsetGpuTestParamOffset "offset"

NATRON_NAMESPACE_ENTER

namespace {

const double kTime = 1.;

constexpr InputSpec kInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief An offset-form Resample op on the GPU that shifts its source by the offset knob, in
 * pixels at full scale rounded at the render scale. It never reports an identity, so a zero
 * offset still renders through the op.
 **/
struct OffsetGpuTestOp {
    struct Params {
        double x;
        double y;
    };

    static constexpr bool kResamplesOnGpu = true;

    static constexpr NodeSpec kNodeSpec = {
        .id = kOffsetGpuTestOpPluginID,
        .label = "Test Offset GPU Op",
        .grouping = "Other/Test",
        .inputs = kInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    static constexpr NodeSpec kPassThroughNodeSpec = {
        .id = kPassThroughOffsetGpuTestOpPluginID,
        .label = "Test Pass-Through Offset GPU Op",
        .grouping = "Other/Test",
        .inputs = kInputs,
    };

    static constexpr ImagePipelineSpec kPassThroughPipeline = {
        .stages = { Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Resample,
    };

    KnobDoubleWPtr offsetKnob;

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page)
    {
        KnobDoublePtr knob = AppManager::createKnob<KnobDouble>(holder, std::string("Offset"), 2);
        knob->setName(kOffsetGpuTestParamOffset);
        knob->setDefaultValue(0., 0);
        knob->setDefaultValue(0., 1);
        page->addKnob(knob);
        offsetKnob = knob;
    }

    Params bake(const KnobReader& reader) const
    {
        Params p;

        p.x = reader.valueOr(offsetKnob, 0., 0);
        p.y = reader.valueOr(offsetKnob, 0., 1);

        return p;
    }

    bool isIdentity(const Params& /*params*/,
                    const PassContext& /*context*/) const
    {
        return false;
    }

    PixelOffset offset(const Params& params,
                       const PassContext& context) const
    {
        const double s = context.scale.toOfxPointD().x;
        PixelOffset o;

        o.x = (int)std::lround(params.x * s);
        o.y = (int)std::lround(params.y * s);

        return o;
    }
};

struct ClippedOffsetGpuTestOp
    : OffsetGpuTestOp {
    static constexpr bool kClipsSourceToRoI = true;

    static constexpr NodeSpec kNodeSpec = {
        .id = kClippedOffsetGpuTestOpPluginID,
        .label = "Test Clipped Offset GPU Op",
        .grouping = "Other/Test",
        .inputs = kInputs,
    };
};

int
positiveModulo(int v,
               int n)
{
    return ((v % n) + n) % n;
}

// Every sixth column weighs 0, then 1, then 1.5, then partial weights that are not exact in float.
double
testColumnWeight(int x)
{
    const int k = positiveModulo(x, 6);

    switch (k) {
    case 0:
        return 0.;
    case 1:
        return 1.;
    case 2:
        return 1.5;
    default:
        return std::sqrt(0.1 * k);
    }
}

// Every fifth row weighs 0, then 1, then partial weights, so that with the column's 1.5 some
// products reach 1 and some stay below.
double
testRowWeight(int y)
{
    const int k = positiveModulo(y, 5);

    switch (k) {
    case 0:
        return 0.;
    case 1:
        return 1.;
    default:
        return 1. / (k + 0.7);
    }
}

// Every seventh column and every ninth row is black, crossing columns and rows of every weight.
bool
testBlackColumn(int x)
{
    return positiveModulo(x, 7) == 3;
}

bool
testBlackRow(int y)
{
    return positiveModulo(y, 9) == 4;
}

bool
testBlack(int x,
          int y)
{
    return testBlackColumn(x) || testBlackRow(y);
}

struct MattedOffsetGpuTestOp
    : OffsetGpuTestOp {
    static constexpr NodeSpec kNodeSpec = {
        .id = kMattedOffsetGpuTestOpPluginID,
        .label = "Test Matted Offset GPU Op",
        .grouping = "Other/Test",
        .inputs = kInputs,
    };

    ResampleMatte matte(const Params& /*params*/,
                        const PassContext& /*context*/) const
    {
        ResampleMatte m;

        m.column = testColumnWeight;
        m.row = testRowWeight;
        m.blackColumn = testBlackColumn;
        m.blackRow = testBlackRow;

        return m;
    }
};

static_assert(GpuOffsetResampleOp<OffsetGpuTestOp>);
static_assert(!GpuMatrixResampleOp<OffsetGpuTestOp>);
static_assert(GpuOffsetResampleOp<ClippedOffsetGpuTestOp>);
static_assert(GpuOffsetResampleOp<MattedOffsetGpuTestOp>);

// The canonical rectangle the clipped node narrows its source's region of interest to.
const RectD kClipKeep(10., 12., 50., 40.);

/**
 * @brief The clipped op with its own regions of interest, which narrow the source's to kClipKeep,
 * as Crop's narrow it to the crop rectangle: the source then reads zero outside that region.
 **/
class ClippedOffsetGpuTestNode
    : public ImageNode<ClippedOffsetGpuTestOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ClippedOffsetGpuTestNode(node);
    }

    explicit ClippedOffsetGpuTestNode(NodePtr node)
        : ImageNode<ClippedOffsetGpuTestOp>(node)
    {
    }

protected:
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL
    {
        ImageNode<ClippedOffsetGpuTestOp>::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);
        EffectInstancePtr source = getInput(0);
        RoIMap::iterator found = source ? ret->find(source) : ret->end();
        if (found != ret->end()) {
            found->second = found->second.intersect(kClipKeep);
        }
    }
};

typedef ImageNode<OffsetGpuTestOp> OffsetGpuTestNode;
typedef ImageNode<OffsetGpuTestOp, OffsetGpuTestOp::kPassThroughNodeSpec, OffsetGpuTestOp::kPassThroughPipeline> PassThroughOffsetGpuTestNode;
typedef ImageNode<MattedOffsetGpuTestOp> MattedOffsetGpuTestNode;

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
registerOffsetOpsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerNode<OffsetGpuTestNode>();
        registerNode<PassThroughOffsetGpuTestNode>();
        registerNode<ClippedOffsetGpuTestNode>();
        registerNode<MattedOffsetGpuTestNode>();
    });
}

enum class OffsetVariant {
    Plain,
    PassThrough,
    Clipped
};

const char*
pluginOf(OffsetVariant v)
{
    switch (v) {
    case OffsetVariant::Plain:
        return kOffsetGpuTestOpPluginID;
    case OffsetVariant::PassThrough:
        return kPassThroughOffsetGpuTestOpPluginID;
    case OffsetVariant::Clipped:
        return kClippedOffsetGpuTestOpPluginID;
    }

    return kOffsetGpuTestOpPluginID;
}

const char*
nameOf(OffsetVariant v)
{
    switch (v) {
    case OffsetVariant::Plain:
        return "plain";
    case OffsetVariant::PassThrough:
        return "pass-through";
    case OffsetVariant::Clipped:
        return "clipped";
    }

    return "";
}

struct OffsetCase {
    const char* name;
    double x;
    double y;
};

const OffsetCase kOffsets[] = {
    { "positive", 7., 5. },
    { "negative", -9., -6. },
    { "zero", 0., 0. },
    { "mixed", 13., -3. },
};

GpuQuery
eligibleQuery()
{
    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;

    return q;
}

} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

class StagedOffsetGpuTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerOffsetOpsOnce();
        BaseTest::SetUp();
    }

    static StagedImageEffect* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
    }

    // A parity source moved off the origin, connected to a new node of pluginID shifted by x, y.
    bool createGraph(const char* pluginID,
                     double x,
                     double y,
                     NodePtr* source,
                     NodePtr* node)
    {
        *source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
        *node = createNode(QString::fromUtf8(pluginID));
        EXPECT_TRUE(bool(*source));
        EXPECT_TRUE(bool(*node));
        if (!*source || !*node) {
            return false;
        }
        setParitySourceComponents(*source, "rgba");
        setParitySourceOrigin(*source, 3, 5);
        connectNodes(*source, *node, 0, true);

        return setKnobValues(*node, kOffsetGpuTestParamOffset, { x, y });
    }
};

TEST_F(StagedOffsetGpuTest, EligibilityFollowsTheRequest)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    for (const char* id : { kOffsetGpuTestOpPluginID, kPassThroughOffsetGpuTestOpPluginID, kClippedOffsetGpuTestOpPluginID, kMattedOffsetGpuTestOpPluginID }) {
        NodePtr source;
        NodePtr node;
        ASSERT_TRUE(createGraph(id, 7., -5., &source, &node));
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        for (int nComps : { 1, 2, 3, 4 }) {
            GpuQuery q = eligibleQuery();
            q.nComps = nComps;
            EXPECT_EQ(supported, effect->canRenderOnGpu(q)) << id << " " << nComps << " components";
        }
        for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
            GpuQuery q = eligibleQuery();
            q.bitDepth = depth;
            EXPECT_FALSE(effect->canRenderOnGpu(q)) << id << " bit depth " << (int)depth;
        }
    }
}

// The GPU source region is the node's own region of interest, here narrowed by the node.
TEST_F(StagedOffsetGpuTest, InputRegionIsTheNodesRegionOfInterest)
{
    for (const char* id : { kOffsetGpuTestOpPluginID, kClippedOffsetGpuTestOpPluginID }) {
        NodePtr source;
        NodePtr node;
        ASSERT_TRUE(createGraph(id, 7., -5., &source, &node));
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);

        const RectI rect(10, 20, 31, 47);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const RectD window = rect.toCanonical_noClipping(mipmapLevel, 1.);
            RoIMap rois;
            effect->getRegionsOfInterest_public(kTime, scale, window, window, ViewIdx(0), &rois);
            RoIMap::const_iterator found = rois.find(source->getEffectInstance());
            ASSERT_TRUE(found != rois.end()) << id << " mipmap " << mipmapLevel;
            EXPECT_EQ(found->second.toPixelEnclosing(scale, 1.), effect->getGpuInputRoI(0, rect, scale)) << id << " mipmap " << mipmapLevel;
        }
    }
}

#ifdef NATRON_HAVE_GPU

namespace {

ImageLayerDesc
colorLayer(const NodePtr& node)
{
    std::list<ImageLayerDesc> layers;

    node->getEffectInstance()->getPresentLayers(kTime, ViewIdx(0), -1, &layers);
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            return *it;
        }
    }

    return ImageLayerDesc();
}

// The render bypasses the caches, so the source it fetches covers exactly its region of interest,
// the region the GPU is handed, and no larger image an earlier render left behind.
bool
renderColor(const NodePtr& node,
            unsigned mipmapLevel,
            const RectI& window,
            RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(colorLayer(node));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

RectI
pixelRegionOfDefinition(const NodePtr& node,
                        unsigned mipmapLevel)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    bool isProjectFormat = false;
    const StatusEnum stat = effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod, &isProjectFormat);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(mipmapLevel, 1.);
}

GpuQuery
queryFor(const NodePtr& node,
         unsigned mipmapLevel)
{
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    const ImageLayerDesc plane = colorLayer(node);
    GpuQuery q = eligibleQuery();

    q.mappedScale = RenderScale::fromMipmapLevel(mipmapLevel);
    q.nComps = plane.getNumComponents();
    const std::bitset<4> channels = effect->getProcessChannelsForPlane(effect->getRenderHash(), kTime, ViewIdx(0), plane);
    for (int i = 0; i < 4; ++i) {
        q.processChannels[i] = channels[i];
    }

    return q;
}

GpuViewLayout
outputLayout()
{
    GpuViewLayout layout;

    layout.padFloats = 4;
    layout.rowPadFloats = 4;
    layout.marginLeft = 2;
    layout.marginBottom = 1;
    layout.marginRight = 3;
    layout.marginTop = 2;

    return layout;
}

// The source's pixels over bounds, inside a larger buffer at an offset and with padded rows.
bool
sourceImage(const NodePtr& source,
            unsigned mipmapLevel,
            const RectI& bounds,
            GpuParityImage* image)
{
    RenderedPlane in;
    if (!renderColor(source, mipmapLevel, bounds, &in)) {
        return false;
    }
    EXPECT_EQ(bounds, in.window);
    image->bounds = in.window;
    image->nComps = (int)in.channels.size();
    image->pixels = in.pixels;
    image->layout.padFloats = 6;
    image->layout.rowPadFloats = 8;

    return true;
}

// The pixels of image inside the narrower bounds.
GpuParityImage
cropImage(const GpuParityImage& image,
          const RectI& bounds)
{
    GpuParityImage out;
    out.bounds = bounds;
    out.nComps = image.nComps;
    out.layout = image.layout;
    const int n = image.nComps;
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        const std::size_t row = (std::size_t)(y - image.bounds.y1) * image.bounds.width() + (bounds.x1 - image.bounds.x1);
        const float* begin = &image.pixels[row * n];
        out.pixels.insert(out.pixels.end(), begin, begin + (std::size_t)bounds.width() * n);
    }

    return out;
}

std::string
describeRoI(const RectI& roi)
{
    std::ostringstream s;
    s << "roi (" << roi.x1 << "," << roi.y1 << ")-(" << roi.x2 << "," << roi.y2 << ")";

    return s.str();
}

// The whole region of definition and an odd-sized window inside it.
std::vector<RectI>
testWindows(const RectI& rod)
{
    std::vector<RectI> rois;

    rois.push_back(rod);
    const int x2 = std::min(rod.x2 - 2, rod.x1 + 5 + 23);
    const int y2 = std::min(rod.y2 - 1, rod.y1 + 3 + 17);
    if ((x2 > rod.x1 + 5) && (y2 > rod.y1 + 3)) {
        rois.push_back(RectI(rod.x1 + 5, rod.y1 + 3, x2, y2));
    }

    return rois;
}

void
expectExact(const std::vector<float>& expected,
            const GpuRunResult& run,
            const RectI& roi,
            int nComps,
            const std::string& what)
{
    ASSERT_TRUE(run.ok) << what;
    EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";
    ASSERT_EQ(expected.size(), run.pixels.size()) << what;
    const ImageDiff diff = compareImages(expected, run.pixels, ParityTolerance::exact());
    EXPECT_GT(diff.compared, 0u) << what;
    const std::size_t at = diff.firstMismatchIndex / nComps;
    EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " mismatches, first at (" << (roi.x1 + (int)(at % roi.width())) << ","
                                   << (roi.y1 + (int)(at / roi.width())) << ") channel " << (diff.firstMismatchIndex % nComps) << " gpu "
                                   << run.pixels[diff.firstMismatchIndex] << " cpu " << expected[diff.firstMismatchIndex];
}

} // namespace

// Every variant at every offset, over the whole region of definition and a window inside it at
// both mipmap levels, into padded output views, against the CPU render. The clipped node's GPU
// source is the source's whole region, so the clip to the region of interest is the GPU's own.
TEST_F(StagedOffsetGpuTest, ShiftMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (OffsetVariant variant : { OffsetVariant::Plain, OffsetVariant::PassThrough, OffsetVariant::Clipped }) {
        for (const OffsetCase& c : kOffsets) {
            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                NodePtr source;
                NodePtr node;
                ASSERT_TRUE(createGraph(pluginOf(variant), c.x, c.y, &source, &node));
                StagedImageEffect* effect = effectOf(node);
                ASSERT_TRUE(effect != NULL);
                const std::string level = std::string(nameOf(variant)) + " " + c.name + " mipmap " + std::to_string(mipmapLevel);
                const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
                ASSERT_FALSE(rod.isNull()) << level;
                const GpuQuery query = queryFor(node, mipmapLevel);
                ASSERT_EQ(4, query.nComps) << level;
                if (variant == OffsetVariant::PassThrough) {
                    ASSERT_FALSE(query.processChannels[3]) << level;
                }
                ASSERT_TRUE(effect->canRenderOnGpu(query)) << level;

                for (const RectI& roi : testWindows(rod)) {
                    const std::string what = level + " " + describeRoI(roi);
                    RenderedPlane cpu;
                    ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
                    ASSERT_EQ(4u, cpu.channels.size()) << what;
                    ASSERT_EQ(roi, cpu.window) << what;

                    const RectI bounds = (variant == OffsetVariant::Clipped) ? paritySourceWindow(source, kTime, mipmapLevel)
                                                                             : effect->getGpuInputRoI(0, roi, RenderScale::fromMipmapLevel(mipmapLevel));
                    GpuParityImage image;
                    ASSERT_TRUE(sourceImage(source, mipmapLevel, bounds, &image)) << what;
                    const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, roi, outputLayout());
                    expectExact(cpu.pixels, run, roi, 4, what);
                    if (HasFatalFailure()) {
                        return;
                    }
                }
            }
        }
    }
}

// A GPU source that covers less than the shift reads: the pixels shifted from outside it are
// zero, and the others are the CPU render's.
TEST_F(StagedOffsetGpuTest, SourceSmallerThanTheWindowReadsZero)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (const OffsetCase& c : kOffsets) {
        NodePtr source;
        NodePtr node;
        ASSERT_TRUE(createGraph(kOffsetGpuTestOpPluginID, c.x, c.y, &source, &node));
        StagedImageEffect* effect = effectOf(node);
        ASSERT_TRUE(effect != NULL);
        const RectI roi = pixelRegionOfDefinition(node, 0);
        ASSERT_FALSE(roi.isNull()) << c.name;
        const GpuQuery query = queryFor(node, 0);
        ASSERT_TRUE(effect->canRenderOnGpu(query)) << c.name;

        RenderedPlane cpu;
        ASSERT_TRUE(renderColor(node, 0, roi, &cpu)) << c.name;
        ASSERT_EQ(roi, cpu.window) << c.name;
        GpuParityImage full;
        ASSERT_TRUE(sourceImage(source, 0, effect->getGpuInputRoI(0, roi, RenderScale::identity), &full)) << c.name;
        const RectI inner(full.bounds.x1 + 4, full.bounds.y1 + 3, full.bounds.x2 - 6, full.bounds.y2 - 5);
        ASSERT_FALSE(inner.isNull()) << c.name;
        const GpuParityImage image = cropImage(full, inner);

        const int dx = (int)c.x;
        const int dy = (int)c.y;
        std::vector<float> expected = cpu.pixels;
        for (int y = roi.y1; y < roi.y2; ++y) {
            for (int x = roi.x1; x < roi.x2; ++x) {
                if (!inner.contains(x - dx, y - dy)) {
                    float* pix = &expected[((std::size_t)(y - roi.y1) * roi.width() + (x - roi.x1)) * 4];
                    std::fill(pix, pix + 4, 0.f);
                }
            }
        }
        const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, roi, outputLayout());
        expectExact(expected, run, roi, 4, c.name);
        if (HasFatalFailure()) {
            return;
        }
    }
}

// The matted node at several offsets and both mipmap levels, against the CPU render, with NaN and
// infinite pixels written into the GPU source where the CPU render has the parity source's. A
// black pixel must be +0 exactly, one weighing 0 the source's bits times 0 (NaN for a NaN or
// infinity) and one weighing 1 or more the source's bits; the others are held to resampling(),
// and a NaN or infinity weighed by a partial weight stays one.
TEST_F(StagedOffsetGpuTest, MatteMatchesTheCpuRender)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const OffsetCase& c : kOffsets) {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            NodePtr source;
            NodePtr node;
            ASSERT_TRUE(createGraph(kMattedOffsetGpuTestOpPluginID, c.x, c.y, &source, &node));
            StagedImageEffect* effect = effectOf(node);
            ASSERT_TRUE(effect != NULL);
            const std::string level = std::string(c.name) + " mipmap " + std::to_string(mipmapLevel);
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const RectI rod = pixelRegionOfDefinition(node, mipmapLevel);
            ASSERT_FALSE(rod.isNull()) << level;
            const GpuQuery query = queryFor(node, mipmapLevel);
            ASSERT_EQ(4, query.nComps) << level;
            ASSERT_TRUE(effect->canRenderOnGpu(query)) << level;
            const int dx = (int)std::lround(c.x * scale.toOfxPointD().x);
            const int dy = (int)std::lround(c.y * scale.toOfxPointD().y);

            for (const RectI& roi : testWindows(rod)) {
                const std::string what = level + " " + describeRoI(roi);
                RenderedPlane cpu;
                ASSERT_TRUE(renderColor(node, mipmapLevel, roi, &cpu)) << what;
                ASSERT_EQ(4u, cpu.channels.size()) << what;
                ASSERT_EQ(roi, cpu.window) << what;

                GpuParityImage image;
                ASSERT_TRUE(sourceImage(source, mipmapLevel, effect->getGpuInputRoI(0, roi, scale), &image)) << what;
                std::vector<float> expected = cpu.pixels;
                std::size_t poisoned[4] = { 0, 0, 0, 0 };
                for (int y = roi.y1; y < roi.y2; ++y) {
                    for (int x = roi.x1; x < roi.x2; ++x) {
                        const int sx = x - dx;
                        const int sy = y - dy;
                        if (!image.bounds.contains(sx, sy) || (positiveModulo(sx * 3 + sy, 4) != 0)) {
                            continue;
                        }
                        float* src = &image.pixels[((std::size_t)(sy - image.bounds.y1) * image.bounds.width() + (sx - image.bounds.x1)) * 4];
                        src[0] = nan;
                        src[1] = inf;
                        const double w = testColumnWeight(x) * testRowWeight(y);
                        float* out = &expected[((std::size_t)(y - roi.y1) * roi.width() + (x - roi.x1)) * 4];
                        if (testBlack(x, y)) {
                            ++poisoned[0];
                            continue;
                        }
                        ++poisoned[(w == 0) ? 1 : (w >= 1) ? 3 : 2];
                        out[0] = nan;
                        out[1] = (w >= 1) ? inf : (float)(inf * w);
                    }
                }
                EXPECT_GT(poisoned[0], 0u) << what << ": no NaN source pixel is black";
                EXPECT_GT(poisoned[1], 0u) << what << ": no NaN source pixel weighs 0";
                EXPECT_GT(poisoned[2], 0u) << what << ": no NaN source pixel has a partial weight";
                EXPECT_GT(poisoned[3], 0u) << what << ": no NaN source pixel weighs 1 or more";

                const GpuRunResult run = runRenderGpu(*runtime, *effect, query, std::vector<GpuParityImage>(1, image), nullptr, roi, outputLayout());
                ASSERT_TRUE(run.ok) << what;
                EXPECT_EQ(0u, run.writtenOutside) << what << ": floats outside the window were written";
                ASSERT_EQ(expected.size(), run.pixels.size()) << what;
                const ImageDiff diff = compareImages(expected, run.pixels, ParityTolerance::resampling());
                EXPECT_GT(diff.compared, 0u) << what;
                EXPECT_EQ(0u, diff.mismatches) << what << ": " << diff.mismatches << " mismatches, first at float " << diff.firstMismatchIndex << " gpu "
                                               << run.pixels[diff.firstMismatchIndex] << " expected " << expected[diff.firstMismatchIndex];

                std::size_t inexact = 0;
                std::size_t blacks = 0;
                std::size_t zeros = 0;
                std::size_t partials = 0;
                for (int y = roi.y1; y < roi.y2; ++y) {
                    for (int x = roi.x1; x < roi.x2; ++x) {
                        const double w = testColumnWeight(x) * testRowWeight(y);
                        const bool black = testBlack(x, y);
                        const std::size_t at = ((std::size_t)(y - roi.y1) * roi.width() + (x - roi.x1)) * 4;
                        if (!black && (w != 0) && (w < 1)) {
                            ++partials;
                            continue;
                        }
                        blacks += black ? 1 : 0;
                        zeros += (!black && (w == 0)) ? 1 : 0;
                        for (int ch = 0; ch < 4; ++ch) {
                            const float g = run.pixels[at + ch];
                            const float e = expected[at + ch];
                            const bool same = (std::isnan(g) && std::isnan(e)) || ((g == e) && (std::signbit(g) == std::signbit(e)));
                            if (!same || (black && ((g != 0.f) || std::signbit(g)))) {
                                if (inexact == 0) {
                                    ADD_FAILURE() << what << ": pixel (" << x << "," << y << ") channel " << ch << " weighing " << w << " gpu " << g << " expected " << e;
                                }
                                ++inexact;
                            }
                        }
                    }
                }
                EXPECT_EQ(0u, inexact) << what << ": black floats or floats weighing 0 or at least 1 that are not exact";
                EXPECT_GT(blacks, 0u) << what;
                EXPECT_GT(zeros, 0u) << what;
                EXPECT_GT(partials, 0u) << what;
                if (HasFatalFailure()) {
                    return;
                }
            }
        }
    }
}

#endif // NATRON_HAVE_GPU
