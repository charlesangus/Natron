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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usdShade/udimUtils.h>

#include "Global/Macros.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/LibraryBinary.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/NodeSpec.h"
#include "Engine/Nodes/Scene/LiveTextureImage.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RenderContext.h"
#include "Engine/ViewIdx.h"

#define kUdimTilesTestPluginID "test.natron.built-in.LiveTextureUdimTiles"

NATRON_NAMESPACE_ENTER

namespace {
/**
 * @brief A UDIM painter that renders: each tile is filled with its number less 1000 in red, over a
 * format of its own, and the tile's version moves only with the strokes painted on it.
 **/
class UdimTilesTestEffect
    : public NativeEffectBase {
public:
    static EffectInstance* BuildEffect(NodePtr n)
    {
        return new UdimTilesTestEffect(n);
    }

    explicit UdimTilesTestEffect(NodePtr n)
        : NativeEffectBase(n)
    {
    }

    static RectI tileFormat(int tile)
    {
        switch (tile) {
        case 1001:
            return RectI(0, 0, 16, 16);
        case 1002:
            return RectI(0, 0, 8, 4);
        default:
            return RectI(0, 0, 4, 4);
        }
    }

    void paint(int tile)
    {
        {
            std::lock_guard<std::mutex> locker(_mutex);
            ++_strokes[tile];
        }
        getNode()->markHashDirty();
    }

    void setFailing(int tile,
                    bool failing)
    {
        std::lock_guard<std::mutex> locker(_mutex);
        if (failing) {
            _failing.insert(tile);
        } else {
            _failing.erase(tile);
        }
    }

    int renders(int tile) const
    {
        std::lock_guard<std::mutex> locker(_mutex);
        std::map<int, int>::const_iterator found = _renders.find(tile);

        return found == _renders.end() ? 0 : found->second;
    }

    virtual bool getMakeSettingsPanel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual bool supportsTiles() const OVERRIDE FINAL
    {
        return true;
    }

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eRenderSafetyFullySafe;
    }

    virtual RenderContextReadSet getExtraContextReads() const OVERRIDE FINAL
    {
        RenderContextReadSet reads;
        reads.add(RenderContextName::udimTile());

        return reads;
    }

    virtual void appendNodeContentHash(ContentHasher& h) const OVERRIDE FINAL
    {
        std::lock_guard<std::mutex> locker(_mutex);
        h.appendU64(_strokes.size());
        for (std::map<int, int>::const_iterator it = _strokes.begin(); it != _strokes.end(); ++it) {
            h.appendI64(it->first);
            h.appendI64(it->second);
        }
    }

    virtual U64 getUdimTileHash(int tile,
                                double /*time*/,
                                ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ContentHasher h;
        h.appendString(getNode()->getPluginID());
        h.appendI64(tile);
        std::lock_guard<std::mutex> locker(_mutex);
        std::map<int, int>::const_iterator found = _strokes.find(tile);
        h.appendI64(found == _strokes.end() ? 0 : found->second);

        return h.finalize();
    }

    virtual StatusEnum getPreferredMetadataAt(double /*time*/,
                                              ViewIdx /*view*/,
                                              NodeMetadata& metadata) OVERRIDE FINAL
    {
        metadata.setOutputFormat(tileFormat(currentTile()));

        return eStatusOK;
    }

    virtual StatusEnum getRegionOfDefinition(U64 /*hash*/,
                                             double /*time*/,
                                             const RenderScale& /*scale*/,
                                             ViewIdx /*view*/,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const RectI format = tileFormat(currentTile());
        rod->x1 = format.x1;
        rod->y1 = format.y1;
        rod->x2 = format.x2;
        rod->y2 = format.y2;

        return eStatusOK;
    }

    virtual void addAcceptedComponents(int /*inputNb*/,
                                       std::list<ImageLayerDesc>* comps) OVERRIDE FINAL
    {
        comps->push_back(ImageLayerDesc::getRGBAComponents());
    }

    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL
    {
        depths->push_back(eImageBitDepthFloat);
    }

protected:
    virtual ImageMetadata deriveOutputMetadata(double /*time*/,
                                               ViewIdx /*view*/) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        ImageMetadata m;
        m.set(kNatronMetadataKeyUdimTiles, ImageMetadata::Value(std::string("1001-1002,1011")));

        return m;
    }

private:
    int currentTile() const
    {
        const RenderContextPtr context = getCurrentRenderContext();
        const RenderContextValue* value = context ? context->find(RenderContextName::udimTile()) : nullptr;
        const std::int64_t* tile = value ? std::get_if<std::int64_t>(value) : nullptr;

        return tile ? (int)*tile : 0;
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        const int tile = currentTile();
        {
            std::lock_guard<std::mutex> locker(_mutex);
            ++_renders[tile];
            if (_failing.count(tile)) {
                return eStatusFailed;
            }
        }
        for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
            const ImagePtr& image = it->second;
            if (!image || (image->getBitDepth() != eImageBitDepthFloat) || (image->getComponentsCount() != 4)) {
                return eStatusFailed;
            }
            Image::WriteAccess access(image.get());
            for (int y = args.roi.y1; y < args.roi.y2; ++y) {
                for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                    float* pixel = (float*)access.pixelAt(x, y);
                    if (!pixel) {
                        return eStatusFailed;
                    }
                    pixel[0] = (float)(tile - 1000);
                    pixel[1] = 0.f;
                    pixel[2] = 0.f;
                    pixel[3] = 1.f;
                }
            }
        }

        return eStatusOK;
    }

    static constexpr NodeSpec kNodeSpec = {
        .id = kUdimTilesTestPluginID,
        .label = "Test UDIM Tiles",
        .grouping = "Other/Test",
        .outputKind = eDataKindImage,
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return staticNodeSpec<kNodeSpec>();
    }

    mutable std::mutex _mutex;
    std::map<int, int> _strokes;
    std::map<int, int> _renders;
    std::set<int> _failing;
};

void
registerUdimTilesTestEffectOnce()
{
    static std::once_flag once;

    std::call_once(once, []() {
        EffectInstancePtr node(UdimTilesTestEffect::BuildEffect(NodePtr()));
        std::map<std::string, void (*)()> functions;

        functions.insert(std::make_pair("BuildEffect", (void (*)())&UdimTilesTestEffect::BuildEffect));
        LibraryBinary* binary = new LibraryBinary(functions);

        std::list<std::string> grouping;
        node->getPluginGrouping(&grouping);
        QStringList qgrouping;
        for (std::list<std::string>::iterator it = grouping.begin(); it != grouping.end(); ++it) {
            qgrouping.push_back(QString::fromUtf8(it->c_str()));
        }
        Plugin* p = appPTR->registerPlugin(QString(), qgrouping, QString::fromUtf8(node->getPluginID().c_str()), QString::fromUtf8(node->getPluginLabel().c_str()),
                                           QString::fromUtf8(""), QStringList(), node->isReader(), node->isWriter(), binary, node->renderThreadSafety() == eRenderSafetyUnsafe, node->getMajorVersion(), node->getMinorVersion(), false);
        p->setLabelWithoutSuffix(Plugin::makeLabelWithoutSuffix(p->getPluginLabel()));
    });
}
} // namespace

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING

namespace {
// x / (width - 1) in red and y / (height - 1) in green, y counted from the bottom as Natron does.
LiveTextureBufferConstPtr
makeGradient(int width,
             int height)
{
    std::shared_ptr<LiveTextureBuffer> buffer = LiveTextureBuffer::create(width, height);

    if (!buffer) {
        return buffer;
    }
    for (int y = 0; y < height; ++y) {
        float* row = buffer->getRow(y);
        for (int x = 0; x < width; ++x) {
            row[4 * x + 0] = (float)x / (float)(width - 1);
            row[4 * x + 1] = (float)y / (float)(height - 1);
            row[4 * x + 2] = 0.25f;
            row[4 * x + 3] = 1.f;
        }
    }

    return buffer;
}

std::string
uriOf(const char* node)
{
    return LiveTextureSource::makeUri(node, "rgba", 0x1234abcdULL);
}

// LiveTextureStore::prepare refuses the main thread, which the tests run on.
int
prepareOffMainThread(const LiveTextureSourceList& sources,
                     double frame)
{
    int numFailed = -1;
    std::thread worker([&]() { numFailed = LiveTextureStore::instance().prepare(sources, frame); });
    worker.join();

    return numFailed;
}
} // anonymous namespace

class LiveTextureTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        _budget = LiveTextureStore::instance().getByteBudget();
    }

    virtual void TearDown() OVERRIDE
    {
        LiveTextureStore::instance().setByteBudget(_budget);
        LiveTextureStore::instance().clear();
        BaseTest::TearDown();
    }

private:
    std::size_t _budget = 0;
};

TEST_F(LiveTextureTest, TheResolverAndImageReaderAreRegistered)
{
    std::string error;
    EXPECT_TRUE(SceneCaches::registerLiveTextures(&error)) << error;
    EXPECT_TRUE(bool(PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("natronLiveTexture")));
    const std::string uri = uriOf("Constant1");
    EXPECT_EQ(std::string("natron"), PXR_NS::ArGetResolver().GetExtension(uri));
    EXPECT_TRUE(PXR_NS::HioImage::IsSupportedImageFile(uri));
    // Anchoring leaves a URI as it is.
    EXPECT_EQ(uri, PXR_NS::ArGetResolver().CreateIdentifier(uri, PXR_NS::ArResolvedPath("/some/layer.usda")));
}

TEST_F(LiveTextureTest, AStoredBufferOpensThroughTheResolver)
{
    const LiveTextureBufferConstPtr buffer = makeGradient(4, 3);
    ASSERT_TRUE(bool(buffer));
    const std::string uri = uriOf("Constant1");
    LiveTextureStore::instance().insert(uri, buffer);

    const PXR_NS::ArResolvedPath resolved = PXR_NS::ArGetResolver().Resolve(uri);
    EXPECT_EQ(uri, resolved.GetPathString());
    const std::shared_ptr<PXR_NS::ArAsset> asset = PXR_NS::ArGetResolver().OpenAsset(resolved);
    ASSERT_TRUE(bool(asset));
    ASSERT_EQ(buffer->getByteSize(), asset->GetSize());
    const std::shared_ptr<const char> bytes = asset->GetBuffer();
    ASSERT_TRUE(bool(bytes));
    EXPECT_EQ(0, std::memcmp(buffer->getBytes().get(), bytes.get(), buffer->getByteSize()));

    int width = 0;
    int height = 0;
    const float* pixels = nullptr;
    ASSERT_TRUE(LiveTextureBuffer::parse(bytes.get(), asset->GetSize(), &width, &height, &pixels));
    EXPECT_EQ(4, width);
    EXPECT_EQ(3, height);
    // The last pixel is the top right one.
    EXPECT_FLOAT_EQ(1.f, pixels[4 * 11 + 0]);
    EXPECT_FLOAT_EQ(1.f, pixels[4 * 11 + 1]);
    // Truncated bytes are not a texture.
    EXPECT_FALSE(LiveTextureBuffer::parse(bytes.get(), asset->GetSize() - 1, &width, &height, &pixels));
}

TEST_F(LiveTextureTest, AMissingUriFailsCleanly)
{
    const std::string uri = uriOf("Missing1");
    PXR_NS::TfErrorMark mark;

    EXPECT_TRUE(PXR_NS::ArGetResolver().Resolve(uri).IsEmpty());
    EXPECT_FALSE(bool(PXR_NS::ArGetResolver().OpenAsset(PXR_NS::ArResolvedPath(uri))));
    EXPECT_FALSE(bool(PXR_NS::HioImage::OpenForReading(uri, 0, 0, PXR_NS::HioImage::Auto, true)));
    EXPECT_TRUE(mark.IsClean());
    mark.Clear();
}

TEST_F(LiveTextureTest, AnEvictedTextureNoLongerResolves)
{
    const LiveTextureBufferConstPtr first = makeGradient(8, 8);
    const LiveTextureBufferConstPtr second = makeGradient(8, 8);
    ASSERT_TRUE(first && second);
    LiveTextureStore& store = LiveTextureStore::instance();
    store.setByteBudget(first->getByteSize() + first->getByteSize() / 2);

    store.insert(uriOf("First"), first);
    EXPECT_FALSE(PXR_NS::ArGetResolver().Resolve(uriOf("First")).IsEmpty());
    store.insert(uriOf("Second"), second);
    // The least recently used goes first.
    EXPECT_TRUE(PXR_NS::ArGetResolver().Resolve(uriOf("First")).IsEmpty());
    EXPECT_FALSE(bool(PXR_NS::ArGetResolver().OpenAsset(PXR_NS::ArResolvedPath(uriOf("First")))));
    EXPECT_FALSE(PXR_NS::ArGetResolver().Resolve(uriOf("Second")).IsEmpty());
    EXPECT_EQ(second->getByteSize(), store.getByteSize());

    // A cache purge drops them all.
    SceneCaches::releaseLiveTextures();
    EXPECT_TRUE(PXR_NS::ArGetResolver().Resolve(uriOf("Second")).IsEmpty());
    EXPECT_EQ(std::size_t(0), store.getByteSize());
}

TEST_F(LiveTextureTest, HioReadsTheTextureTopRowFirst)
{
    const LiveTextureBufferConstPtr buffer = makeGradient(4, 3);
    ASSERT_TRUE(bool(buffer));
    const std::string uri = uriOf("Gradient1");
    LiveTextureStore::instance().insert(uri, buffer);

    const PXR_NS::HioImageSharedPtr image = PXR_NS::HioImage::OpenForReading(uri);
    ASSERT_TRUE(bool(image));
    EXPECT_EQ(4, image->GetWidth());
    EXPECT_EQ(3, image->GetHeight());
    EXPECT_EQ(PXR_NS::HioFormatFloat32Vec4, image->GetFormat());
    EXPECT_FALSE(image->IsColorSpaceSRGB());

    std::vector<float> pixels(4 * 3 * 4, -1.f);
    PXR_NS::HioImage::StorageSpec storage;
    storage.width = 4;
    storage.height = 3;
    storage.depth = 1;
    storage.format = PXR_NS::HioFormatFloat32Vec4;
    storage.data = pixels.data();
    ASSERT_TRUE(image->Read(storage));
    // Top left, then bottom right.
    EXPECT_FLOAT_EQ(0.f, pixels[0]);
    EXPECT_FLOAT_EQ(1.f, pixels[1]);
    EXPECT_FLOAT_EQ(0.25f, pixels[2]);
    EXPECT_FLOAT_EQ(1.f, pixels[3]);
    EXPECT_FLOAT_EQ(1.f, pixels[4 * 11 + 0]);
    EXPECT_FLOAT_EQ(0.f, pixels[4 * 11 + 1]);

    // Flipped, the bottom row comes first.
    storage.flipped = true;
    ASSERT_TRUE(image->Read(storage));
    EXPECT_FLOAT_EQ(0.f, pixels[0]);
    EXPECT_FLOAT_EQ(0.f, pixels[1]);

    // Cropped by the top row and the two right columns.
    std::vector<float> cropped(2 * 2 * 4, -1.f);
    storage.width = 2;
    storage.height = 2;
    storage.flipped = false;
    storage.data = cropped.data();
    ASSERT_TRUE(image->ReadCropped(1, 0, 0, 2, storage));
    EXPECT_FLOAT_EQ(0.f, cropped[0]);
    EXPECT_FLOAT_EQ(0.5f, cropped[1]);
    EXPECT_FLOAT_EQ(1.f / 3.f, cropped[4 * 3 + 0]);
    EXPECT_FLOAT_EQ(0.f, cropped[4 * 3 + 1]);
}

TEST_F(LiveTextureTest, AVaryingSourceHasOneUriPerHeldFrame)
{
    LiveTextureSource source;
    source.nodeName = "Read1";
    source.layer = "rgba";
    source.contentHash = 42;
    source.firstFrame = 1.;
    source.lastFrame = 10.;

    // Static: one URI at every frame.
    EXPECT_EQ(source.getUri(1.), source.getUri(7.));
    EXPECT_EQ(LiveTextureSource::makeUri("Read1", "rgba", 42), source.getUri(1.));

    source.varying = true;
    EXPECT_NE(source.getUri(1.), source.getUri(2.));
    // USD holds a sample until the next; outside the range, the nearest end.
    EXPECT_EQ(source.getUri(2.), source.getUri(2.5));
    EXPECT_EQ(source.getUri(1.), source.getUri(-3.));
    EXPECT_EQ(source.getUri(10.), source.getUri(25.));
    EXPECT_DOUBLE_EQ(2., source.getSourceFrame(2.5));
    EXPECT_EQ(0u, source.getUri(1.).find("natron://Read1/rgba?v="));
}

TEST_F(LiveTextureTest, PrepareRendersAMissingTextureOnce)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(bool(constant));
    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
    ASSERT_TRUE(color != NULL);
    color->setValue(0.8, ViewSpec::all(), 0);
    color->setValue(0.2, ViewSpec::all(), 1);
    color->setValue(0.1, ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);

    LiveTextureSource source;
    source.node = constant;
    source.nodeName = constant->getFullyQualifiedName();
    source.layer = "rgba";
    source.contentHash = constant->getEffectInstance()->getHash();
    source.firstFrame = 1.;
    source.lastFrame = 1.;
    const std::string uri = source.getUri(1.);

    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    const LiveTextureBufferConstPtr buffer = LiveTextureStore::instance().find(uri);
    ASSERT_TRUE(bool(buffer));
    ASSERT_GT(buffer->getWidth(), 0);
    ASSERT_GT(buffer->getHeight(), 0);
    const float* center = buffer->getRow(buffer->getHeight() / 2) + 4 * (buffer->getWidth() / 2);
    EXPECT_NEAR(0.8, center[0], 1e-5);
    EXPECT_NEAR(0.2, center[1], 1e-5);
    EXPECT_NEAR(0.1, center[2], 1e-5);
    EXPECT_NEAR(1., center[3], 1e-5);

    // Already stored: nothing is rendered again.
    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    EXPECT_EQ(buffer, LiveTextureStore::instance().find(uri));

    // The main thread never renders.
    LiveTextureStore::instance().clear();
    EXPECT_EQ(1, LiveTextureStore::instance().prepare(LiveTextureSourceList(1, source), 1.));
    EXPECT_FALSE(LiveTextureStore::instance().contains(uri));
}

// Natron's images are premultiplied and USD's textures are not: the store divides the colour by the
// alpha, which the material's opacity multiplies back.
TEST_F(LiveTextureTest, TheTextureColourIsNotPremultiplied)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(bool(constant));
    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
    ASSERT_TRUE(color != NULL);
    color->setValue(0.4, ViewSpec::all(), 0);
    color->setValue(0.2, ViewSpec::all(), 1);
    color->setValue(0.1, ViewSpec::all(), 2);
    color->setValue(0.5, ViewSpec::all(), 3);

    LiveTextureSource source;
    source.node = constant;
    source.nodeName = constant->getFullyQualifiedName();
    source.layer = "rgba";
    source.contentHash = constant->getEffectInstance()->getHash();
    source.firstFrame = 1.;
    source.lastFrame = 1.;

    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    const LiveTextureBufferConstPtr buffer = LiveTextureStore::instance().find(source.getUri(1.));
    ASSERT_TRUE(bool(buffer));
    const float* center = buffer->getRow(buffer->getHeight() / 2) + 4 * (buffer->getWidth() / 2);
    EXPECT_NEAR(0.8, center[0], 1e-5);
    EXPECT_NEAR(0.4, center[1], 1e-5);
    EXPECT_NEAR(0.2, center[2], 1e-5);
    EXPECT_NEAR(0.5, center[3], 1e-5);
}

// Natron's images hold premultiplied colour, so a subnormal alpha comes with a colour no larger.
TEST_F(LiveTextureTest, ASubnormalAlphaLeavesTheColourFinite)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(bool(constant));
    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
    ASSERT_TRUE(color != NULL);
    for (int channel = 0; channel < 4; ++channel) {
        color->setValue(1e-40, ViewSpec::all(), channel);
    }

    LiveTextureSource source;
    source.node = constant;
    source.nodeName = constant->getFullyQualifiedName();
    source.layer = "rgba";
    source.contentHash = constant->getEffectInstance()->getHash();
    source.firstFrame = 1.;
    source.lastFrame = 1.;

    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    const LiveTextureBufferConstPtr buffer = LiveTextureStore::instance().find(source.getUri(1.));
    ASSERT_TRUE(bool(buffer));
    const float* center = buffer->getRow(buffer->getHeight() / 2) + 4 * (buffer->getWidth() / 2);
    for (int channel = 0; channel < 4; ++channel) {
        EXPECT_TRUE(std::isfinite(center[channel])) << channel;
        EXPECT_LE(center[channel], 1.f) << channel;
    }
}

// Going over budget costs memory; dropping a texture of the frame shows the fallback.
TEST_F(LiveTextureTest, TheBudgetEvictsNoTextureOfTheLastFramePrepared)
{
    const auto sourceOf = [](const NodePtr& constant) {
        LiveTextureSource source;
        source.node = constant;
        source.nodeName = constant->getFullyQualifiedName();
        source.layer = "rgba";
        source.contentHash = constant->getEffectInstance()->getHash();
        source.firstFrame = 1.;
        source.lastFrame = 1.;

        return source;
    };
    NodePtr constants[3];
    for (NodePtr& constant : constants) {
        constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        ASSERT_TRUE(bool(constant));
    }
    LiveTextureStore& store = LiveTextureStore::instance();
    store.setByteBudget(1);

    LiveTextureSourceList frame;
    frame.push_back(sourceOf(constants[0]));
    frame.push_back(sourceOf(constants[1]));
    ASSERT_EQ(0, prepareOffMainThread(frame, 1.));
    EXPECT_TRUE(store.contains(frame[0].getUri(1.)));
    EXPECT_TRUE(store.contains(frame[1].getUri(1.)));

    // The next frame prepared releases them.
    const LiveTextureSource next = sourceOf(constants[2]);
    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, next), 1.));
    EXPECT_TRUE(store.contains(next.getUri(1.)));
    EXPECT_FALSE(store.contains(frame[0].getUri(1.)));
    EXPECT_FALSE(store.contains(frame[1].getUri(1.)));
}

// A disabled node passes its input through in the 2D graph, and its texture shows that input.
TEST_F(LiveTextureTest, ADisabledSourceShowsItsPassThroughInput)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr invert = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT));
    ASSERT_TRUE(constant && invert);
    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(invert->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(color && channels);
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);
    std::vector<std::string> rgb;
    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");
    channels->setChannels(0, rgb);
    connectNodes(constant, invert, 0, true);
    invert->setNodeDisabled(true);

    LiveTextureSource source;
    source.node = invert;
    source.nodeName = invert->getFullyQualifiedName();
    source.layer = "rgba";
    source.contentHash = invert->getEffectInstance()->getHash();
    source.firstFrame = 1.;
    source.lastFrame = 1.;

    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    const LiveTextureBufferConstPtr buffer = LiveTextureStore::instance().find(source.getUri(1.));
    ASSERT_TRUE(bool(buffer)) << "a disabled node's texture is rendered from its input";
    const float* center = buffer->getRow(buffer->getHeight() / 2) + 4 * (buffer->getWidth() / 2);
    EXPECT_NEAR(1., center[0], 1e-5);
    EXPECT_NEAR(0., center[1], 1e-5);
    EXPECT_NEAR(0., center[2], 1e-5);
    EXPECT_NEAR(1., center[3], 1e-5);
}

// A Constant whose rectangle covers the left half of the format and as much again outside it: the
// texture holds the format and that overscan, coloured where the image is and transparent over the
// format it does not cover, and texture coordinates 0 to 1 span the format inside it.
TEST_F(LiveTextureTest, TheTextureHoldsTheOverscanAroundTheFormat)
{
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    ASSERT_GT(format.width(), 8);
    ASSERT_GT(format.height(), 8);

    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(bool(constant));
    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
    KnobChoice* extent = dynamic_cast<KnobChoice*>(constant->getKnobByName(kNativeGeneratorParamExtent).get());
    KnobDouble* bottomLeft = dynamic_cast<KnobDouble*>(constant->getKnobByName(kNativeGeneratorParamBottomLeft).get());
    KnobDouble* size = dynamic_cast<KnobDouble*>(constant->getKnobByName(kNativeGeneratorParamSize).get());
    ASSERT_TRUE(color && extent && bottomLeft && size);
    color->setValue(0.5, ViewSpec::all(), 0);
    color->setValue(0.25, ViewSpec::all(), 1);
    color->setValue(0.75, ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    const int half = format.width() / 2;
    bottomLeft->setValue(format.x1 - half, ViewSpec::all(), 0);
    bottomLeft->setValue(format.y1, ViewSpec::all(), 1);
    size->setValue(2 * half, ViewSpec::all(), 0);
    size->setValue(format.height(), ViewSpec::all(), 1);

    LiveTextureWindow window;
    ASSERT_TRUE(getNodeImageWindow(constant, 1., ViewIdx(0), &window.format, &window.window));
    EXPECT_EQ(RectI(format.x1, format.y1, format.x2, format.y2), window.format);
    EXPECT_EQ(RectI(format.x1 - half, format.y1, format.x2, format.y2), window.window);
    double scale[2];
    double translation[2];
    window.getUvTransform(scale, translation);
    const double width = format.width() + half;
    EXPECT_NEAR(format.width() / width, scale[0], 1e-9);
    EXPECT_NEAR(1., scale[1], 1e-9);
    EXPECT_NEAR(half / width, translation[0], 1e-9) << "u = 0 is the format's left edge, past the overscan";
    EXPECT_NEAR(0., translation[1], 1e-9);
    EXPECT_NEAR(1., scale[0] + translation[0], 1e-9) << "u = 1 is the format's right edge, the window's";

    LiveTextureSource source;
    source.node = constant;
    source.nodeName = constant->getFullyQualifiedName();
    source.layer = "rgba";
    source.contentHash = constant->getEffectInstance()->getHash();
    source.firstFrame = 1.;
    source.lastFrame = 1.;
    const std::string uri = source.getUri(1.);

    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.)) << LiveTextureStore::instance().getFailure(uri);
    const LiveTextureBufferConstPtr buffer = LiveTextureStore::instance().find(uri);
    ASSERT_TRUE(bool(buffer));
    EXPECT_EQ(format.width() + half, buffer->getWidth());
    EXPECT_EQ(format.height(), buffer->getHeight());

    const int y = buffer->getHeight() / 2;
    const float* overscan = buffer->getRow(y) + 4 * (half / 2);
    const float* inside = buffer->getRow(y) + 4 * (half + half / 2);
    for (const float* pixel : { buffer->getRow(y), overscan, inside }) {
        EXPECT_NEAR(0.5, pixel[0], 1e-5);
        EXPECT_NEAR(0.25, pixel[1], 1e-5);
        EXPECT_NEAR(0.75, pixel[2], 1e-5);
        EXPECT_NEAR(1., pixel[3], 1e-5);
    }
    const float* outside = buffer->getRow(y) + 4 * (2 * half + (buffer->getWidth() - 2 * half) / 2);
    EXPECT_EQ(0.f, outside[3]) << "the format the image does not cover is transparent";

    // A recorded window is the one rendered, whatever the image's own.
    LiveTextureStore::instance().clear();
    source.windows.assign(1, LiveTextureWindow());
    source.windows[0].format = window.format;
    source.windows[0].window = window.format;
    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.)) << LiveTextureStore::instance().getFailure(uri);
    const LiveTextureBufferConstPtr formatOnly = LiveTextureStore::instance().find(uri);
    ASSERT_TRUE(bool(formatOnly));
    EXPECT_EQ(format.width(), formatOnly->getWidth());
    EXPECT_EQ(format.height(), formatOnly->getHeight());
}

// Storm filters the straight colour, so a black colour where the alpha is 0 would darken the edge
// texels it blends into: a Constant of red over the left half of the format gives the transparent
// texels of the right half within the fill band the red's colour, leaves those beyond it black,
// where a surface drawn without its alpha shows nothing, and its own texels as rendered.
TEST_F(LiveTextureTest, TransparentTexelsTakeTheColourOfTheNearestCoveredOnes)
{
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    const int band = LiveTextureBuffer::kTransparentFillBand;
    ASSERT_GT(format.width(), 4 * band);
    ASSERT_GT(format.height(), 8);

    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(bool(constant));
    KnobColor* color = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
    KnobChoice* extent = dynamic_cast<KnobChoice*>(constant->getKnobByName(kNativeGeneratorParamExtent).get());
    KnobDouble* bottomLeft = dynamic_cast<KnobDouble*>(constant->getKnobByName(kNativeGeneratorParamBottomLeft).get());
    KnobDouble* size = dynamic_cast<KnobDouble*>(constant->getKnobByName(kNativeGeneratorParamSize).get());
    ASSERT_TRUE(color && extent && bottomLeft && size);
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    const int half = format.width() / 2;
    bottomLeft->setValue(format.x1, ViewSpec::all(), 0);
    bottomLeft->setValue(format.y1, ViewSpec::all(), 1);
    size->setValue(half, ViewSpec::all(), 0);
    size->setValue(format.height(), ViewSpec::all(), 1);

    LiveTextureSource source;
    source.node = constant;
    source.nodeName = constant->getFullyQualifiedName();
    source.layer = "rgba";
    source.contentHash = constant->getEffectInstance()->getHash();
    source.firstFrame = 1.;
    source.lastFrame = 1.;
    source.windows.assign(1, LiveTextureWindow());
    source.windows[0].format = RectI(format.x1, format.y1, format.x2, format.y2);
    source.windows[0].window = source.windows[0].format;
    const std::string uri = source.getUri(1.);

    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.)) << LiveTextureStore::instance().getFailure(uri);
    const LiveTextureBufferConstPtr buffer = LiveTextureStore::instance().find(uri);
    ASSERT_TRUE(bool(buffer));
    ASSERT_EQ(format.width(), buffer->getWidth());
    ASSERT_EQ(format.height(), buffer->getHeight());

    for (int y : { 0, buffer->getHeight() / 2, buffer->getHeight() - 1 }) {
        const float* row = buffer->getRow(y);
        for (int x : { 0, half - 1 }) {
            const float* opaque = row + 4 * x;
            EXPECT_EQ(1.f, opaque[0]) << "opaque texel " << x << ", " << y;
            EXPECT_EQ(0.f, opaque[1]) << "opaque texel " << x << ", " << y;
            EXPECT_EQ(0.f, opaque[2]) << "opaque texel " << x << ", " << y;
            EXPECT_EQ(1.f, opaque[3]) << "opaque texel " << x << ", " << y;
        }
        for (int x : { half, half + 1, half + band - 1 }) {
            const float* filled = row + 4 * x;
            EXPECT_NEAR(1., filled[0], 1e-5) << "transparent texel in the band " << x << ", " << y;
            EXPECT_NEAR(0., filled[1], 1e-5) << "transparent texel in the band " << x << ", " << y;
            EXPECT_NEAR(0., filled[2], 1e-5) << "transparent texel in the band " << x << ", " << y;
            EXPECT_EQ(0.f, filled[3]) << "transparent texel in the band " << x << ", " << y;
        }
        for (int x : { half + band, half + 2 * band, buffer->getWidth() - 1 }) {
            const float* beyond = row + 4 * x;
            for (int c = 0; c < 4; ++c) {
                EXPECT_EQ(0.f, beyond[c]) << "transparent texel beyond the band " << x << ", " << y << ", channel " << c;
            }
        }
    }
}

// An image far larger than its format is held whole.
TEST_F(LiveTextureTest, TheWholeDataWindowIsHeld)
{
    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(bool(constant));
    KnobChoice* extent = dynamic_cast<KnobChoice*>(constant->getKnobByName(kNativeGeneratorParamExtent).get());
    KnobDouble* bottomLeft = dynamic_cast<KnobDouble*>(constant->getKnobByName(kNativeGeneratorParamBottomLeft).get());
    KnobDouble* size = dynamic_cast<KnobDouble*>(constant->getKnobByName(kNativeGeneratorParamSize).get());
    ASSERT_TRUE(extent && bottomLeft && size);
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    bottomLeft->setValue(format.x1 - 10 * format.width(), ViewSpec::all(), 0);
    bottomLeft->setValue(format.y1 + format.height() / 2, ViewSpec::all(), 1);
    size->setValue(20 * format.width(), ViewSpec::all(), 0);
    size->setValue(10 * format.height(), ViewSpec::all(), 1);

    RectI formatRect;
    RectI window;
    ASSERT_TRUE(getNodeImageWindow(constant, 1., ViewIdx(0), &formatRect, &window));
    EXPECT_EQ(RectI(format.x1 - 10 * format.width(), format.y1, format.x1 + 10 * format.width(), format.y1 + format.height() / 2 + 10 * format.height()), window);
}

TEST_F(LiveTextureTest, PrepareOnTheMainThreadWarnsTheConsumer)
{
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr consumer = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(bool(constant));
    ASSERT_TRUE(bool(consumer));

    LiveTextureSource source;
    source.node = constant;
    source.consumer = consumer;
    source.nodeName = constant->getFullyQualifiedName();
    source.layer = "rgba";
    source.contentHash = constant->getEffectInstance()->getHash();
    const std::string uri = source.getUri(1.);

    // Already stored: nothing is missing, so no warning.
    LiveTextureStore::instance().insert(uri, makeGradient(4, 4));
    EXPECT_EQ(0, LiveTextureStore::instance().prepare(LiveTextureSourceList(1, source), 1.));
    EXPECT_FALSE(consumer->hasPersistentMessage());

    LiveTextureStore::instance().clear();
    EXPECT_EQ(1, LiveTextureStore::instance().prepare(LiveTextureSourceList(1, source), 1.));
    EXPECT_FALSE(LiveTextureStore::instance().contains(uri));
    ASSERT_TRUE(consumer->hasPersistentMessage());
    QString message;
    int type = 0;
    consumer->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromStdString(uri))) << message.toStdString();
    EXPECT_FALSE(constant->hasPersistentMessage());
}

TEST_F(LiveTextureTest, ADeletedSourceLeavesTheTextureMissing)
{
    LiveTextureSource source;
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        ASSERT_TRUE(bool(constant));
        source.node = constant;
        source.nodeName = constant->getFullyQualifiedName();
        source.layer = "rgba";
        source.contentHash = constant->getEffectInstance()->getHash();
        constant->destroyNode(true, false);
    }
    const std::string uri = source.getUri(1.);

    EXPECT_EQ(1, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    EXPECT_FALSE(LiveTextureStore::instance().contains(uri));
    EXPECT_FALSE(LiveTextureStore::instance().getFailure(uri).empty());
    EXPECT_TRUE(PXR_NS::ArGetResolver().Resolve(uri).IsEmpty());
}

// Each edit of a source gives its texture a new URI, which may fail once and never be tried again.
TEST_F(LiveTextureTest, TheFailuresKeptAreBounded)
{
    LiveTextureSource source;
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        ASSERT_TRUE(bool(constant));
        source.node = constant;
        source.nodeName = constant->getFullyQualifiedName();
        source.layer = "rgba";
        constant->destroyNode(true, false);
    }
    source.contentHash = 1;
    const std::string first = source.getUri(1.);
    ASSERT_EQ(1, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    EXPECT_FALSE(LiveTextureStore::instance().getFailure(first).empty());
    for (U64 hash = 2; hash <= 300; ++hash) {
        source.contentHash = hash;
        ASSERT_EQ(1, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    }
    EXPECT_TRUE(LiveTextureStore::instance().getFailure(first).empty());
    EXPECT_FALSE(LiveTextureStore::instance().getFailure(source.getUri(1.)).empty());
}

class LiveTextureUdimTest
    : public LiveTextureTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        LiveTextureTest::SetUp();
        registerUdimTilesTestEffectOnce();
    }

    NodePtr createTiles(UdimTilesTestEffect** effect)
    {
        NodePtr node = createNode(QString::fromUtf8(kUdimTilesTestPluginID));
        *effect = node ? dynamic_cast<UdimTilesTestEffect*>(node->getEffectInstance().get()) : NULL;
        EXPECT_TRUE(*effect != NULL);

        return node;
    }

    // The set node declares in its metadata, as it stands now.
    static LiveTextureSource udimSource(const NodePtr& node,
                                        const NodePtr& consumer = NodePtr())
    {
        LiveTextureSource source;
        source.node = node;
        source.consumer = consumer;
        source.nodeName = node->getFullyQualifiedName();
        source.layer = "rgba";
        source.contentHash = node->getEffectInstance()->getHash();
        source.firstFrame = 1.;
        source.lastFrame = 1.;
        std::string error;
        EXPECT_TRUE(parseUdimTiles(ImageMetadata::Value(std::string("1001-1002,1011")), &source.tiles, &error)) << error;

        return source;
    }

    static std::string messageOf(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return message.toStdString();
    }
};

TEST_F(LiveTextureUdimTest, ASetHasAUdimTokenUriAndOneUriPerTile)
{
    LiveTextureSource source;
    source.nodeName = "Paint1";
    source.layer = "rgba";
    source.contentHash = 42;
    EXPECT_EQ(LiveTextureSource::makeUri("Paint1", "rgba", 42), source.getUri(1.));

    source.tiles = { 1001, 1002, 1011 };
    const std::string set = source.getUri(1.);
    EXPECT_EQ(0u, set.find("natron://Paint1/rgba.<UDIM>?v=")) << set;
    // USD finds a tile by putting its number for the token, which keeps the version after it.
    EXPECT_EQ(source.getTileUri(1., 1002), PXR_NS::UsdShadeUdimUtils::ReplaceUdimPattern(set, "1002"));
    EXPECT_EQ(0u, source.getTileUri(1., 1002).find("natron://Paint1/rgba.1002?v="));
    EXPECT_TRUE(source.getTilesBeyondUsd().empty());
    EXPECT_TRUE(source.getTilesBeyondUsdWarning(1.).empty());

    // Another set of tiles is another set to Storm, which keeps the sets it loaded by URI.
    LiveTextureSource fewer = source;
    fewer.tiles = { 1001, 1002 };
    EXPECT_NE(set, fewer.getUri(1.));
}

TEST_F(LiveTextureUdimTest, UsdResolvesExactlyTheDeclaredTilesEachAtItsSize)
{
    UdimTilesTestEffect* effect = NULL;
    NodePtr node = createTiles(&effect);
    ASSERT_TRUE(effect != NULL);
    const LiveTextureSource source = udimSource(node);
    const std::string set = source.getUri(1.);

    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.)) << LiveTextureStore::instance().getFailure(set);
    EXPECT_EQ(1, effect->renders(1001));
    EXPECT_EQ(1, effect->renders(1002));
    EXPECT_EQ(1, effect->renders(1011));
    // The set URI is no texture of its own.
    EXPECT_FALSE(LiveTextureStore::instance().contains(set));

    const PXR_NS::SdfLayerRefPtr anonymous = PXR_NS::SdfLayer::CreateAnonymous("udim.usda");
    for (const PXR_NS::SdfLayerHandle& layer : { PXR_NS::SdfLayerHandle(), PXR_NS::SdfLayerHandle(anonymous) }) {
        const std::vector<PXR_NS::UsdShadeUdimUtils::ResolvedPathAndTile> resolved = PXR_NS::UsdShadeUdimUtils::ResolveUdimTilePaths(set, layer);
        std::vector<int> tiles;
        for (const PXR_NS::UsdShadeUdimUtils::ResolvedPathAndTile& tile : resolved) {
            EXPECT_EQ(source.getTileUri(1., std::stoi(tile.second)), tile.first);
            tiles.push_back(std::stoi(tile.second));
        }
        EXPECT_EQ(source.tiles, tiles);
    }

    for (int tile : source.tiles) {
        const PXR_NS::HioImageSharedPtr image = PXR_NS::HioImage::OpenForReading(source.getTileUri(1., tile));
        ASSERT_TRUE(bool(image)) << tile;
        const RectI format = UdimTilesTestEffect::tileFormat(tile);
        EXPECT_EQ(format.width(), image->GetWidth()) << tile;
        EXPECT_EQ(format.height(), image->GetHeight()) << tile;
        std::vector<float> pixels(4 * format.width() * format.height(), -1.f);
        PXR_NS::HioImage::StorageSpec storage;
        storage.width = format.width();
        storage.height = format.height();
        storage.format = PXR_NS::HioFormatFloat32Vec4;
        storage.data = pixels.data();
        ASSERT_TRUE(image->Read(storage)) << tile;
        EXPECT_FLOAT_EQ((float)(tile - 1000), pixels[0]) << tile;
        EXPECT_FLOAT_EQ((float)(tile - 1000), pixels[pixels.size() - 4]) << tile;
        EXPECT_FLOAT_EQ(1.f, pixels[3]) << tile;
    }
}

// The edit of one tile gives the set a new URI, under which only that tile is rendered: the others
// are the buffers already held, which count against the budget once.
TEST_F(LiveTextureUdimTest, AnEditOfOneTileRendersOnlyThatTile)
{
    UdimTilesTestEffect* effect = NULL;
    NodePtr node = createTiles(&effect);
    ASSERT_TRUE(effect != NULL);
    const LiveTextureSource before = udimSource(node);
    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, before), 1.)) << LiveTextureStore::instance().getFailure(before.getUri(1.));
    const std::size_t bytesBefore = LiveTextureStore::instance().getByteSize();

    effect->paint(1002);
    const LiveTextureSource after = udimSource(node);
    ASSERT_NE(before.getUri(1.), after.getUri(1.));
    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, after), 1.)) << LiveTextureStore::instance().getFailure(after.getUri(1.));
    EXPECT_EQ(1, effect->renders(1001));
    EXPECT_EQ(2, effect->renders(1002));
    EXPECT_EQ(1, effect->renders(1011));

    LiveTextureStore& store = LiveTextureStore::instance();
    EXPECT_EQ(store.find(before.getTileUri(1., 1001)), store.find(after.getTileUri(1., 1001)));
    EXPECT_EQ(store.find(before.getTileUri(1., 1011)), store.find(after.getTileUri(1., 1011)));
    EXPECT_NE(store.find(before.getTileUri(1., 1002)), store.find(after.getTileUri(1., 1002)));
    EXPECT_EQ(bytesBefore + store.find(after.getTileUri(1., 1002))->getByteSize(), store.getByteSize());

    // Unchanged since: nothing is rendered.
    ASSERT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, after), 1.));
    EXPECT_EQ(1, effect->renders(1001));
    EXPECT_EQ(2, effect->renders(1002));
    EXPECT_EQ(1, effect->renders(1011));
}

TEST_F(LiveTextureUdimTest, AFailedTileShowsMagentaAndIsTriedAgain)
{
    UdimTilesTestEffect* effect = NULL;
    NodePtr node = createTiles(&effect);
    NodePtr consumer = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(bool(consumer));
    const LiveTextureSource source = udimSource(node, consumer);
    const std::string failed = source.getTileUri(1., 1011);

    effect->setFailing(1011, true);
    EXPECT_EQ(1, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    LiveTextureStore& store = LiveTextureStore::instance();
    const LiveTextureBufferConstPtr magenta = store.find(failed);
    ASSERT_TRUE(bool(magenta));
    EXPECT_EQ(4, magenta->getWidth());
    EXPECT_EQ(4, magenta->getHeight());
    EXPECT_FLOAT_EQ(1.f, magenta->getRow(0)[0]);
    EXPECT_FLOAT_EQ(0.f, magenta->getRow(0)[1]);
    EXPECT_FLOAT_EQ(1.f, magenta->getRow(0)[2]);
    EXPECT_FALSE(store.getFailure(failed).empty());
    EXPECT_FALSE(store.getFailure(source.getUri(1.)).empty());
    EXPECT_TRUE(store.getFailure(source.getTileUri(1., 1001)).empty());
    EXPECT_NE(std::string::npos, messageOf(consumer).find("UDIM tile 1011")) << messageOf(consumer);

    effect->setFailing(1011, false);
    EXPECT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    EXPECT_NE(magenta, store.find(failed));
    EXPECT_TRUE(store.getFailure(failed).empty());
    EXPECT_TRUE(store.getFailure(source.getUri(1.)).empty());
    EXPECT_EQ(1, effect->renders(1001));
    EXPECT_EQ(2, effect->renders(1011));
}

TEST_F(LiveTextureUdimTest, TilesAboveWhatUsdReadsAreWarned)
{
    UdimTilesTestEffect* effect = NULL;
    NodePtr node = createTiles(&effect);
    NodePtr consumer = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(effect != NULL);
    ASSERT_TRUE(bool(consumer));
    LiveTextureSource source = udimSource(node, consumer);
    source.tiles = { 1001, 1101 };

    EXPECT_EQ(std::vector<int>(1, 1101), source.getTilesBeyondUsd());
    EXPECT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, source), 1.));
    EXPECT_TRUE(LiveTextureStore::instance().contains(source.getTileUri(1., 1101)));
    EXPECT_NE(std::string::npos, messageOf(consumer).find("1101")) << messageOf(consumer);
    const std::vector<PXR_NS::UsdShadeUdimUtils::ResolvedPathAndTile> resolved = PXR_NS::UsdShadeUdimUtils::ResolveUdimTilePaths(source.getUri(1.), PXR_NS::SdfLayerHandle());
    ASSERT_EQ(1u, resolved.size());
    EXPECT_EQ("1001", resolved[0].second);
}
