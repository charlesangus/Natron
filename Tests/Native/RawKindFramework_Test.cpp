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

#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "Tests/BaseTest.h"
#include "Tests/DataKindTestEffect.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/ImagePipelineSpec.h"
#include "Engine/Nodes/Image/Op.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Plugin.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define kRawKindTestSourceID "test.natron.built-in.RawKindTestSource"
#define kRawKindTestColourOpID "test.natron.built-in.RawKindTestColourOp"
#define kRawKindTestToImageOpID "test.natron.built-in.RawKindTestToImageOp"
#define kRawKindTestGainOpID "test.natron.built-in.RawKindTestGainOp"
#define kRawKindTestIdentityOpID "test.natron.built-in.RawKindTestIdentityOp"

NATRON_NAMESPACE_ENTER

namespace {

const int kRawWidth = 24;
const int kRawHeight = 16;

float
rawSourceValue(int x,
               int y)
{
    return 1.f + (float)x + 100.f * (float)y;
}

/**
 * @brief A one-channel raw mosaic of kRawWidth x kRawHeight whose photosite (x, y) holds
 * rawSourceValue(x, y), and whose raw/ metadata a test sets.
 **/
class RawKindTestSource
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new RawKindTestSource(n);
    }

    explicit RawKindTestSource(NodePtr n)
        : NativeEffectBase(n)
        , _pattern("RGGB")
        , _originX(0)
        , _originY(0)
        , _stage(eRawStageRaw)
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

    // As a Read reloading its file: the metadata changes under the same hash.
    void setRaw(const std::string& pattern,
                int originX,
                int originY,
                std::optional<RawStageEnum> stage)
    {
        _pattern = pattern;
        _originX = originX;
        _originY = originY;
        _stage = stage;
        invalidateOutputMetadata();
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        *rod = RectD(0., 0., kRawWidth, kRawHeight);

        return eStatusOK;
    }

private:
    static constexpr NodeSpec kNodeSpec = {
        .id = kRawKindTestSourceID,
        .label = "Test Raw Kind Source",
        .outputKind = eDataKindRaw,
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
        metadata.setNComps(-1, 1);
        metadata.setBitDepth(-1, eImageBitDepthFloat);
        metadata.setIsFrameVarying(false);

        return eStatusOK;
    }

    virtual ImageMetadata deriveOutputMetadata(double /*time*/,
                                               ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ImageMetadata m;
        const std::optional<RawLayout> layout = RawLayout::fromPattern(_pattern, _originX, _originY);

        if (layout) {
            layout->writeTo(&m);
        }
        if (_stage) {
            RawMetadata::setStage(&m, *_stage);
        }

        return m;
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
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
                    for (int c = 0; c < nComps; ++c) {
                        pixel[c] = rawSourceValue(x, y);
                    }
                }
            }
        }

        return eStatusOK;
    }

    std::string _pattern;
    int _originX;
    int _originY;
    std::optional<RawStageEnum> _stage;
};

constexpr InputSpec kRawInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

float
colourTagged(RawCfaColorEnum colour,
             float source)
{
    return 1000.f * (float)colour + source;
}

void
writeColourTaggedRow(const RawLayout& layout,
                     const RowIO& io)
{
    for (int x = 0; x < io.width; ++x) {
        const RawCfaColorEnum colour = layout.colourAt(io.x0 + x, io.y);
        for (int c = 0; c < io.nComps; ++c) {
            io.dst[x * io.nComps + c] = colourTagged(colour, io.src[0][x * io.nComps + c]);
        }
    }
}

/**
 * @brief Raw in, raw out, at the raw stage: tags each photosite with the colour the source's CFA
 * gives it, so a test reads back which colour the op saw at every pixel.
 **/
struct RawColourTestOp {
    struct Params {
        RawLayout layout;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kRawKindTestColourOpID,
        .label = "Test Raw Colour Op",
        .grouping = "Other/Test",
        .outputKind = eDataKindRaw,
        .inputs = kRawInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .layers = LayerPolicy::none(),
        .op = OpKind::Point,
        .rawStage = RawStageRequirement::Raw,
    };

    Params bake(const KnobReader& /*reader*/,
                const ImageMetadata& source) const
    {
        return Params { RawLayout::fromMetadata(source).value_or(RawLayout()) };
    }

    bool isIdentity(const Params& /*params*/) const
    {
        return false;
    }

    void row(const Params& params,
             const RowIO& io) const
    {
        writeColourTaggedRow(params.layout, io);
    }

    GpuOpRef gpu(const Params& /*params*/) const
    {
        return GpuOpRef::none();
    }
};

/**
 * @brief Raw in at the prepared stage, image out, keeping the channel count.
 **/
struct RawToImageTestOp {
    struct Params {
        RawLayout layout;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kRawKindTestToImageOpID,
        .label = "Test Raw To Image Op",
        .grouping = "Other/Test",
        .outputKind = eDataKindImage,
        .inputs = kRawInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .layers = LayerPolicy::none(),
        .op = OpKind::Point,
        .rawStage = RawStageRequirement::Prepared,
    };

    Params bake(const KnobReader& /*reader*/,
                const ImageMetadata& source) const
    {
        return Params { RawLayout::fromMetadata(source).value_or(RawLayout()) };
    }

    bool isIdentity(const Params& /*params*/) const
    {
        return false;
    }

    void row(const Params& params,
             const RowIO& io) const
    {
        writeColourTaggedRow(params.layout, io);
    }

    GpuOpRef gpu(const Params& /*params*/) const
    {
        return GpuOpRef::none();
    }
};

/**
 * @brief Raw in, raw out, at any stage, with a bake() that reads knobs only.
 **/
struct RawGainTestOp {
    struct Params {
        float gain;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kRawKindTestGainOpID,
        .label = "Test Raw Gain Op",
        .grouping = "Other/Test",
        .outputKind = eDataKindRaw,
        .inputs = kRawInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .layers = LayerPolicy::none(),
        .op = OpKind::Point,
    };

    Params bake(const KnobReader& /*reader*/) const
    {
        return Params { 2.f };
    }

    bool isIdentity(const Params& params) const
    {
        return params.gain == 1.f;
    }

    void row(const Params& params,
             const RowIO& io) const
    {
        for (int i = 0; i < io.width * io.nComps; ++i) {
            io.dst[i] = params.gain * io.src[0][i];
        }
    }

    GpuOpRef gpu(const Params& /*params*/) const
    {
        return GpuOpRef::none();
    }
};

/**
 * @brief Always an identity, at the prepared stage: whether it is one tells a test whether the
 * stage check ran before the op's own identity.
 **/
struct RawIdentityTestOp {
    struct Params {
        int unused;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = kRawKindTestIdentityOpID,
        .label = "Test Raw Identity Op",
        .grouping = "Other/Test",
        .outputKind = eDataKindRaw,
        .inputs = kRawInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .layers = LayerPolicy::none(),
        .op = OpKind::Point,
        .rawStage = RawStageRequirement::Prepared,
    };

    Params bake(const KnobReader& /*reader*/) const
    {
        return Params { 0 };
    }

    bool isIdentity(const Params& /*params*/) const
    {
        return true;
    }

    void row(const Params& /*params*/,
             const RowIO& io) const
    {
        for (int i = 0; i < io.width * io.nComps; ++i) {
            io.dst[i] = io.src[0][i];
        }
    }

    GpuOpRef gpu(const Params& /*params*/) const
    {
        return GpuOpRef::none();
    }
};

static_assert(PointOp<RawColourTestOp>);
static_assert(PointOp<RawToImageTestOp>);
static_assert(PointOp<RawGainTestOp>);
static_assert(PointOp<RawIdentityTestOp>);
static_assert(BakesParamsFromSource<RawColourTestOp> && !BakesParamsFromKnobs<RawColourTestOp>);
static_assert(BakesParamsFromKnobs<RawGainTestOp> && !BakesParamsFromSource<RawGainTestOp>);

static_assert(isValidImagePipelineSpec(RawColourTestOp::kPipeline, RawColourTestOp::kNodeSpec));
static_assert(isValidImagePipelineSpec(RawToImageTestOp::kPipeline, RawToImageTestOp::kNodeSpec));

constexpr InputSpec kImageInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

constexpr NodeSpec kRawOutFromImageSpec = {
    .id = "test.natron.built-in.RawOutFromImage",
    .label = "Raw Out From Image",
    .outputKind = eDataKindRaw,
    .inputs = kImageInputs,
};

constexpr NodeSpec kImageSpec = {
    .id = "test.natron.built-in.ImageOnly",
    .label = "Image Only",
    .inputs = kImageInputs,
};

static_assert(!isValidImagePipelineSpec(ImagePipelineSpec { .layers = LayerPolicy::none() }, kRawOutFromImageSpec), "raw out needs a raw Source input");
static_assert(!isValidImagePipelineSpec(ImagePipelineSpec { .rawStage = RawStageRequirement::Raw }, kImageSpec), "a raw stage needs a raw Source input");
static_assert(isValidImagePipelineSpec(ImagePipelineSpec { .stages = { Stage::Premult, Stage::MaskMix } }, kImageSpec));
static_assert(!isValidImagePipelineSpec(ImagePipelineSpec { .stages = { Stage::Premult }, .layers = LayerPolicy::none() }, RawColourTestOp::kNodeSpec), "no Premult on raw");
static_assert(!isValidImagePipelineSpec(ImagePipelineSpec { .stages = { Stage::MaskMix }, .layers = LayerPolicy::none() }, RawColourTestOp::kNodeSpec), "no MaskMix on raw");
static_assert(!isValidImagePipelineSpec(ImagePipelineSpec { .layers = LayerPolicy::color(true, true, true, false) }, RawColourTestOp::kNodeSpec), "no layer policy on raw");
static_assert(!isValidImagePipelineSpec(ImagePipelineSpec { .layers = LayerPolicy::none(), .op = OpKind::Route, .rawStage = RawStageRequirement::Raw }, RawColourTestOp::kNodeSpec), "a Route op has no raw stage");

typedef ImageNode<RawColourTestOp> RawColourTestNode;
typedef ImageNode<RawToImageTestOp> RawToImageTestNode;
typedef ImageNode<RawGainTestOp> RawGainTestNode;
typedef ImageNode<RawIdentityTestOp> RawIdentityTestNode;

} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

template <typename PLUGIN>
void
registerRawKindTestPlugin()
{
    EffectInstancePtr node(PLUGIN::BuildEffect(NodePtr()));
    std::map<std::string, void (*)()> functions;

    functions.insert(std::make_pair("BuildEffect", (void (*)())&PLUGIN::BuildEffect));
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
registerRawKindTestPluginsOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        registerRawKindTestPlugin<RawKindTestSource>();
        registerRawKindTestPlugin<RawColourTestNode>();
        registerRawKindTestPlugin<RawToImageTestNode>();
        registerRawKindTestPlugin<RawGainTestNode>();
        registerRawKindTestPlugin<RawIdentityTestNode>();
    });
}

float
planeValue(const RenderedPlane& plane,
           int x,
           int y,
           int c)
{
    const std::size_t nComps = plane.channels.size();

    return plane.pixels[(static_cast<std::size_t>(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * nComps + c];
}

ImageLayerDesc
outputColorLayer(const NodePtr& node)
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

bool
renderColorPlane(const NodePtr& node,
                 const RectI& window,
                 RenderedPlane* plane,
                 std::string* error)
{
    const ImageLayerDesc color = outputColorLayer(node);

    if (color.getNumComponents() <= 0) {
        *error = "no colour layer";

        return false;
    }
    std::list<ImageLayerDesc> layers;
    layers.push_back(color);
    std::vector<RenderedPlane> planes;
    if (!renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, error) || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// Starts off the origin, so a row function that ignored RowIO::x0 would read the wrong colours.
const RectI kWindow(3, 2, kRawWidth - 3, kRawHeight - 1);

const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

} // namespace

class RawKindFrameworkTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawKindTestPluginsOnce();
        BaseTest::SetUp();
    }

    NodePtr createRawSource(const std::string& pattern,
                            int originX,
                            int originY,
                            std::optional<RawStageEnum> stage)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawKindTestSourceID));

        EXPECT_TRUE(bool(source));
        if (source) {
            sourceEffect(source)->setRaw(pattern, originX, originY, stage);
        }

        return source;
    }

    static RawKindTestSource* sourceEffect(const NodePtr& source)
    {
        return dynamic_cast<RawKindTestSource*>(source->getEffectInstance().get());
    }

    NodePtr createOp(const char* pluginID,
                     const NodePtr& source)
    {
        NodePtr op = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(op));
        if (op && source) {
            connectNodes(source, op, 0, true);
        }

        return op;
    }

    void expectColourTagged(const NodePtr& op,
                            const std::string& pattern,
                            int originX,
                            int originY)
    {
        const std::optional<RawLayout> layout = RawLayout::fromPattern(pattern, originX, originY);
        ASSERT_TRUE(bool(layout)) << pattern;

        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderColorPlane(op, kWindow, &plane, &error)) << error;
        ASSERT_EQ(1u, plane.channels.size());
        for (int y = kWindow.y1; y < kWindow.y2; ++y) {
            for (int x = kWindow.x1; x < kWindow.x2; ++x) {
                ASSERT_EQ(colourTagged(layout->colourAt(x, y), rawSourceValue(x, y)), planeValue(plane, x, y, 0)) << pattern << " at origin " << originX << "," << originY << ", pixel " << x << "," << y;
            }
        }
    }
};

TEST_F(RawKindFrameworkTest, DeclaredKindsResolve)
{
    NodePtr source = createRawSource("RGGB", 0, 0, eRawStageRaw);
    NodePtr rawOp = createOp(kRawKindTestColourOpID, source);
    NodePtr toImage = createOp(kRawKindTestToImageOpID, source);
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));
    NodePtr rawSink = createNode(QString::fromUtf8(kTestPluginIDDataKindRawSink));
    NodePtr imageSource = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    NodePtr unconnectedRawOp = createOp(kRawKindTestColourOpID, NodePtr());
    ASSERT_TRUE(source && rawOp && toImage && imageSink && rawSink && imageSource && unconnectedRawOp);

    EXPECT_EQ(eDataKindRaw, rawOp->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindRaw, rawOp->getEffectInstance()->getOutputDataKind());
    EXPECT_EQ(eDataKindRaw, rawOp->getEffectiveOutputDataKind());
    EXPECT_EQ(eDataKindRaw, toImage->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindImage, toImage->getEffectInstance()->getOutputDataKind());
    EXPECT_EQ(eDataKindImage, toImage->getEffectiveOutputDataKind());

    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, unconnectedRawOp->canConnectInput(imageSource, 0));
    EXPECT_EQ(Node::eCanConnectInput_incompatibleDataKind, imageSink->canConnectInput(rawOp, 0));
    EXPECT_EQ(Node::eCanConnectInput_ok, rawSink->canConnectInput(rawOp, 0));

    EXPECT_FALSE(bool(rawOp->getUnPremultBySelector()));
    EXPECT_FALSE(bool(rawOp->getKnobByName(kOfxMixParamName)));
}

TEST_F(RawKindFrameworkTest, RawOpSeesTheSourcePatternOnEveryRow)
{
    struct Layout {
        std::string pattern;
        int originX;
        int originY;
    };
    const Layout layouts[] = {
        { "RGGB", 0, 0 },
        { "GBRG", 1, 3 },
        { "BGGR", -1, 2 },
        { kXTransPattern, 4, 5 },
    };

    for (const Layout& l : layouts) {
        NodePtr source = createRawSource(l.pattern, l.originX, l.originY, eRawStageRaw);
        NodePtr op = createOp(kRawKindTestColourOpID, source);
        ASSERT_TRUE(source && op);
        expectColourTagged(op, l.pattern, l.originX, l.originY);
    }
}

TEST_F(RawKindFrameworkTest, ANewPatternUnderTheSameHashReachesTheOp)
{
    NodePtr source = createRawSource("RGGB", 0, 0, eRawStageRaw);
    NodePtr op = createOp(kRawKindTestColourOpID, source);
    ASSERT_TRUE(source && op);
    expectColourTagged(op, "RGGB", 0, 0);

    sourceEffect(source)->setRaw("GRBG", 1, 1, eRawStageRaw);
    expectColourTagged(op, "GRBG", 1, 1);
}

TEST_F(RawKindFrameworkTest, RawToImageOpFeedsAnImageConsumer)
{
    NodePtr source = createRawSource("GBRG", 1, 0, eRawStagePrepared);
    NodePtr op = createOp(kRawKindTestToImageOpID, source);
    NodePtr imageSink = createNode(QString::fromUtf8(kTestPluginIDDataKindImageSink));
    ASSERT_TRUE(source && op && imageSink);

    EXPECT_EQ(Node::eCanConnectInput_ok, imageSink->canConnectInput(op, 0));
    connectNodes(op, imageSink, 0, true);
    EXPECT_EQ(eDataKindImage, imageSink->getEffectiveOutputDataKind());
    EXPECT_FALSE(op->hasPersistentMessage());

    expectColourTagged(op, "GBRG", 1, 0);
}

TEST_F(RawKindFrameworkTest, AWrongRawStageFailsWithThePersistentMessage)
{
    NodePtr source = createRawSource("RGGB", 0, 0, eRawStagePrepared);
    NodePtr op = createOp(kRawKindTestColourOpID, source);
    ASSERT_TRUE(source && op);

    ImageMetadata prepared;
    RawMetadata::setStage(&prepared, eRawStagePrepared);
    const std::optional<std::string> expected = RawMetadata::requireStage(prepared, eRawStageRaw, op->getLabel());
    ASSERT_TRUE(bool(expected));

    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderColorPlane(op, kWindow, &plane, &error));
    EXPECT_TRUE(plane.pixels.empty());
    ASSERT_TRUE(op->hasPersistentMessage());
    QString message;
    int type = 0;
    op->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(QString::fromUtf8(expected->c_str()), message);
    EXPECT_EQ((int)eMessageTypeError, type);

    sourceEffect(source)->setRaw("RGGB", 0, 0, eRawStageRaw);
    expectColourTagged(op, "RGGB", 0, 0);
    EXPECT_FALSE(op->hasPersistentMessage());
}

TEST_F(RawKindFrameworkTest, AMissingRawStageFails)
{
    NodePtr source = createRawSource("RGGB", 0, 0, std::nullopt);
    NodePtr op = createOp(kRawKindTestColourOpID, source);
    ASSERT_TRUE(source && op);

    const std::optional<std::string> expected = RawMetadata::requireStage(ImageMetadata(), eRawStageRaw, op->getLabel());
    ASSERT_TRUE(bool(expected));

    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderColorPlane(op, kWindow, &plane, &error));
    QString message;
    int type = 0;
    op->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(QString::fromUtf8(expected->c_str()), message);
}

// An op that is always an identity passes its source through at the stage it needs, and at any
// other is no identity at all, so its render fails instead of passing the source on.
TEST_F(RawKindFrameworkTest, AWrongRawStageIsNeverAnIdentity)
{
    NodePtr source = createRawSource("RGGB", 0, 0, eRawStagePrepared);
    NodePtr op = createOp(kRawKindTestIdentityOpID, source);
    ASSERT_TRUE(source && op);
    EffectInstancePtr effect = op->getEffectInstance();
    double inputTime = 0.;
    ViewIdx inputView;
    int inputNb = -1;

    EXPECT_TRUE(effect->isIdentity_public(false, 0, kTime, RenderScale::identity, kWindow, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);

    NodePtr rawSource = createRawSource("RGGB", 0, 0, eRawStageRaw);
    NodePtr rawOp = createOp(kRawKindTestIdentityOpID, rawSource);
    ASSERT_TRUE(rawSource && rawOp);
    inputNb = -1;
    EXPECT_FALSE(rawOp->getEffectInstance()->isIdentity_public(false, 0, kTime, RenderScale::identity, kWindow, ViewIdx(0), &inputTime, &inputView, &inputNb));

    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderColorPlane(rawOp, kWindow, &plane, &error));
    EXPECT_TRUE(rawOp->hasPersistentMessage());
}

// A stage change under the same hash reaches the cached identity answer downstream, which is
// keyed by that unmoved hash.
TEST_F(RawKindFrameworkTest, AStageChangeUnderTheSameHashRecomputesTheCachedIdentity)
{
    NodePtr source = createRawSource("RGGB", 0, 0, eRawStagePrepared);
    NodePtr op = createOp(kRawKindTestIdentityOpID, source);
    ASSERT_TRUE(source && op);
    EffectInstancePtr effect = op->getEffectInstance();
    ASSERT_TRUE(bool(effect));
    double inputTime = 0.;
    ViewIdx inputView;
    int inputNb = -1;

    const U64 hash = effect->getRenderHash();
    EXPECT_TRUE(effect->isIdentity_public(true, hash, kTime, RenderScale::identity, kWindow, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);

    sourceEffect(source)->setRaw("RGGB", 0, 0, eRawStageRaw);
    ASSERT_EQ(hash, effect->getRenderHash());
    inputNb = -1;
    EXPECT_FALSE(effect->isIdentity_public(true, hash, kTime, RenderScale::identity, kWindow, ViewIdx(0), &inputTime, &inputView, &inputNb));

    sourceEffect(source)->setRaw("RGGB", 0, 0, eRawStagePrepared);
    inputNb = -1;
    EXPECT_TRUE(effect->isIdentity_public(true, hash, kTime, RenderScale::identity, kWindow, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);
}

TEST_F(RawKindFrameworkTest, AReaderOnlyOpWorksUnchanged)
{
    const RawGainTestOp gainOp;
    const KnobReader reader(kTime, ViewIdx(0));
    ImageMetadata metadata;
    RawMetadata::setStage(&metadata, eRawStageRaw);
    EXPECT_EQ(2.f, bakeParams(gainOp, reader).gain);
    EXPECT_EQ(2.f, bakeParams(gainOp, reader, metadata).gain);
    EXPECT_EQ(2.f, bakeParams(gainOp, kTime, ViewIdx(0)).gain);

    NodePtr source = createRawSource("RGGB", 0, 0, std::nullopt);
    NodePtr op = createOp(kRawKindTestGainOpID, source);
    ASSERT_TRUE(source && op);

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderColorPlane(op, kWindow, &plane, &error)) << error;
    ASSERT_EQ(1u, plane.channels.size());
    for (int y = kWindow.y1; y < kWindow.y2; ++y) {
        for (int x = kWindow.x1; x < kWindow.x2; ++x) {
            ASSERT_EQ(2.f * rawSourceValue(x, y), planeValue(plane, x, y, 0)) << x << "," << y;
        }
    }
    EXPECT_FALSE(op->hasPersistentMessage());
}
