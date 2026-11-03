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

#include "Global/Macros.h"

#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Scene/LiveTextureImage.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneCaches.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

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
    std::thread worker([&]() { numFailed = LiveTextureStore::instance().prepare(sources, frame, ViewIdx(0)); });
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
    EXPECT_EQ(1, LiveTextureStore::instance().prepare(LiveTextureSourceList(1, source), 1., ViewIdx(0)));
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
// texture holds the format and that overscan, coloured where the image is and transparent black over
// the format it does not cover, and texture coordinates 0 to 1 span the format inside it.
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
    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(0.f, outside[c]) << "channel " << c << " of the format the image does not cover";
    }

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
    EXPECT_EQ(0, LiveTextureStore::instance().prepare(LiveTextureSourceList(1, source), 1., ViewIdx(0)));
    EXPECT_FALSE(consumer->hasPersistentMessage());

    LiveTextureStore::instance().clear();
    EXPECT_EQ(1, LiveTextureStore::instance().prepare(LiveTextureSourceList(1, source), 1., ViewIdx(0)));
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
