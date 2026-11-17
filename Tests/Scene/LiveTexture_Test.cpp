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

#include <pxr/base/gf/vec2f.h>
#include <pxr/imaging/hd/materialNetwork2Interface.h>

#include "Global/Macros.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QtCore/QElapsedTimer>
#include <QtCore/QThread>

#include "BaseTest.h"
#include "CountingTestEffect.h"

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
#include "Engine/Nodes/Scene/LiveTextureOiio/LiveTextureFile.h"
#include "Engine/Nodes/Scene/LiveTextureSceneIndex.h"
#include "Engine/Nodes/Scene/LiveTextureSharedMemory.h"
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
    std::thread worker([&]() { numFailed = LiveTextureStore::instance().prepare(sources, frame); });
    worker.join();

    return numFailed;
}

bool
fileExists(const std::string& path)
{
    return ::access(path.c_str(), F_OK) == 0;
}

std::vector<std::string>
namesStartingWith(const std::string& prefix)
{
    std::vector<std::string> names;
    DIR* directory = ::opendir(LiveTextureSharedMemory::directory().c_str());

    if (!directory) {
        return names;
    }
    while (const struct dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.compare(0, prefix.size(), prefix) == 0) {
            names.push_back(name);
        }
    }
    ::closedir(directory);

    return names;
}

// Every texture file of this process's session, .tmp ones included, but not its lock.
std::vector<std::string>
sessionFiles()
{
    const std::string session = LiveTextureSharedMemory::sessionName();

    return session.empty() ? std::vector<std::string>() : namesStartingWith("natron-" + session + "-");
}

std::string
inSharedMemory(const std::string& name)
{
    return LiveTextureSharedMemory::directory() + "/" + name;
}

void
touch(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);

    ASSERT_GE(fd, 0) << path;
    ::close(fd);
}

// A buffer claiming to be far larger than any tmpfs, whose export fails before it reads a pixel.
LiveTextureBufferConstPtr
makeOversizedBuffer()
{
    static const char bytes[64] = { 0 };

    return LiveTextureBuffer::fromFile(std::shared_ptr<const char>(bytes, [](const char*) { }), sizeof(bytes), 1 << 20, 1 << 20, std::string());
}

const char* const kTextureNode = "/Material/Texture";
const char* const kSurfaceNode = "/Material/Surface";

PXR_NS::HdMaterialNetwork2
makeTexturedNetwork(const std::string& uri,
                    const std::string& input,
                    const std::string& output)
{
    PXR_NS::HdMaterialNode2 texture;

    texture.nodeTypeId = PXR_NS::TfToken("UsdUVTexture");
    texture.parameters[PXR_NS::TfToken("file")] = PXR_NS::VtValue(PXR_NS::SdfAssetPath(uri));

    PXR_NS::HdMaterialNode2 surface;
    surface.nodeTypeId = PXR_NS::TfToken("UsdPreviewSurface");
    PXR_NS::HdMaterialConnection2 connection;
    connection.upstreamNode = PXR_NS::SdfPath(kTextureNode);
    connection.upstreamOutputName = PXR_NS::TfToken(output);
    surface.inputConnections[PXR_NS::TfToken(input)] = { connection };

    PXR_NS::HdMaterialConnection2 terminal;
    terminal.upstreamNode = PXR_NS::SdfPath(kSurfaceNode);
    terminal.upstreamOutputName = PXR_NS::TfToken("surface");

    PXR_NS::HdMaterialNetwork2 network;
    network.nodes[PXR_NS::SdfPath(kTextureNode)] = texture;
    network.nodes[PXR_NS::SdfPath(kSurfaceNode)] = surface;
    network.terminals[PXR_NS::TfToken("surface")] = terminal;

    return network;
}

void
applyFilter(const std::function<void(PXR_NS::HdMaterialNetworkInterface*)>& filter,
            PXR_NS::HdMaterialNetwork2* network,
            const char* material = "/Material")
{
    PXR_NS::HdMaterialNetwork2Interface interface(PXR_NS::SdfPath(material), network);

    filter(&interface);
}

bool
hasTextureNode(const PXR_NS::HdMaterialNetwork2& network)
{
    return network.nodes.find(PXR_NS::SdfPath(kTextureNode)) != network.nodes.end();
}

PXR_NS::VtValue
surfaceParameter(const PXR_NS::HdMaterialNetwork2& network,
                 const std::string& name)
{
    const PXR_NS::HdMaterialNode2& surface = network.nodes.at(PXR_NS::SdfPath(kSurfaceNode));
    const std::map<PXR_NS::TfToken, PXR_NS::VtValue>::const_iterator found = surface.parameters.find(PXR_NS::TfToken(name));

    return (found == surface.parameters.end()) ? PXR_NS::VtValue() : found->second;
}

std::size_t
surfaceConnections(const PXR_NS::HdMaterialNetwork2& network,
                   const std::string& name)
{
    const PXR_NS::HdMaterialNode2& surface = network.nodes.at(PXR_NS::SdfPath(kSurfaceNode));

    return surface.inputConnections.count(PXR_NS::TfToken(name));
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

TEST_F(LiveTextureTest, AFailedRenderResolvesToTheMagentaImage)
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

    const PXR_NS::ArResolvedPath resolved = PXR_NS::ArGetResolver().Resolve(uri);
    ASSERT_FALSE(resolved.IsEmpty());
    const std::shared_ptr<PXR_NS::ArAsset> asset = PXR_NS::ArGetResolver().OpenAsset(resolved);
    ASSERT_TRUE(bool(asset));
    const std::shared_ptr<const char> bytes = asset->GetBuffer();
    int width = 0;
    int height = 0;
    const float* pixels = nullptr;
    ASSERT_TRUE(LiveTextureBuffer::parse(bytes.get(), asset->GetSize(), &width, &height, &pixels));
    EXPECT_EQ(1, width);
    EXPECT_EQ(1, height);
    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(kLiveTextureMissingColour[c], pixels[c]);
    }
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

// The mapping and the file name are the two ways a render delegate's process gets at the pixels.
TEST_F(LiveTextureTest, AnotherProcessReadsAnExportedTexture)
{
    const int width = 16;
    const int height = 10;
    const LiveTextureBufferConstPtr source = makeGradient(width, height);
    LiveTextureStore& store = LiveTextureStore::instance();
    const std::string uri = uriOf("Exported");

    store.insert(uri, source);
    const LiveTextureBufferConstPtr exported = store.exportToSharedMemory(uri);
    ASSERT_TRUE(bool(exported)) << store.getFailure(uri);
    ASSERT_FALSE(exported->getFilePath().empty());

    const std::string path = exported->getFilePath();
    const std::size_t size = NatronLiveTexture::fileSize(width, height);
    const NatronLiveTexture::Level second = NatronLiveTexture::levelAt(width, height, 1);
    ASSERT_EQ(8, second.width);
    ASSERT_EQ(5, second.height);
    const char* const expectedPixels = reinterpret_cast<const char*>(source->getRow(0));
    const std::size_t pixelBytes = std::size_t(width) * std::size_t(height) * NatronLiveTexture::kTexelBytes;

    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        // Only async-signal-safe calls from here: the parent's other threads do not exist in this copy.
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            ::_exit(1);
        }
        void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (mapped == MAP_FAILED) {
            ::_exit(2);
        }
        const char* const bytes = static_cast<const char*>(mapped);
        if (std::memcmp(bytes, NatronLiveTexture::kMagic, sizeof(NatronLiveTexture::kMagic)) != 0) {
            ::_exit(3);
        }
        if (std::memcmp(bytes + NatronLiveTexture::kHeaderSize, expectedPixels, pixelBytes) != 0) {
            ::_exit(4);
        }
        const float* const level = reinterpret_cast<const float*>(bytes + second.offset);
        for (int y = 0; y < second.height; ++y) {
            for (int x = 0; x < second.width; ++x) {
                const float* const texel = level + 4 * (std::size_t(y) * second.width + x);
                const float red = (2.f * x + 0.5f) / 15.f;
                const float green = (2.f * y + 0.5f) / 9.f;
                if ((std::fabs(texel[0] - red) > 1e-5f) || (std::fabs(texel[1] - green) > 1e-5f)) {
                    ::_exit(5);
                }
            }
        }
        ::_exit(0);
    }
    int status = 0;
    ASSERT_EQ(child, ::waitpid(child, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));
}

TEST_F(LiveTextureTest, ExportingTwiceGivesOneFile)
{
    LiveTextureStore& store = LiveTextureStore::instance();
    const std::string uri = uriOf("Twice");

    store.insert(uri, makeGradient(8, 8));
    const LiveTextureBufferConstPtr first = store.exportToSharedMemory(uri);
    ASSERT_TRUE(bool(first)) << store.getFailure(uri);
    const LiveTextureBufferConstPtr second = store.exportToSharedMemory(uri);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first, store.find(uri));
    EXPECT_EQ(std::string("Twice"), store.getSourceOfFile(first->getFilePath()));
    EXPECT_EQ(std::string(), store.getSourceOfFile("/somewhere/else.natron"));
}

namespace {
struct SharedMemoryLimit {
    explicit SharedMemoryLimit(long long bytes)
    {
        LiveTextureSharedMemory::limitAvailableBytes(bytes);
    }

    ~SharedMemoryLimit()
    {
        LiveTextureSharedMemory::limitAvailableBytes(-1);
    }
};
} // anonymous namespace

TEST_F(LiveTextureTest, ARenderIsStoppedNotMagentaWhenSharedMemoryHasNoRoom)
{
    LiveTextureStore& store = LiveTextureStore::instance();
    const std::string held = uriOf("HeldBeforeFull");
    const std::string next = uriOf("NoRoomFor");
    const std::size_t needed = NatronLiveTexture::fileSize(16, 16);

    store.insert(held, makeGradient(16, 16));
    const LiveTextureBufferConstPtr previous = store.exportToSharedMemory(held);
    ASSERT_TRUE(bool(previous)) << store.getFailure(held);
    const std::string previousPath = previous->getFilePath();

    store.insert(next, makeGradient(16, 16));
    {
        const SharedMemoryLimit full(static_cast<long long>(needed) - 1);
        EXPECT_FALSE(bool(store.exportToSharedMemory(next)));
        EXPECT_EQ(std::size_t(needed - 1), LiveTextureSharedMemory::availableBytes());
        const std::string noRoom = store.getNoRoom(next);
        EXPECT_NE(std::string::npos, noRoom.find("NoRoomFor")) << noRoom;
        EXPECT_NE(std::string::npos, noRoom.find("MB")) << noRoom;
        EXPECT_EQ(noRoom, store.getFailure(next));
        EXPECT_TRUE(store.getNoRoom(held).empty());
        // The image a render under way reads stays where it is.
        EXPECT_TRUE(fileExists(previousPath));
        EXPECT_EQ(previous, store.exportToSharedMemory(held));
    }

    // Space freed: the next render's export tries again.
    const LiveTextureBufferConstPtr retried = store.exportToSharedMemory(next);
    ASSERT_TRUE(bool(retried)) << store.getFailure(next);
    EXPECT_TRUE(store.getNoRoom(next).empty());

    // An edit gives the texture a new image, which clears the note without a retry.
    const std::string edited = uriOf("EditedNoRoom");
    store.insert(edited, makeGradient(16, 16));
    {
        const SharedMemoryLimit full(0);
        EXPECT_FALSE(bool(store.exportToSharedMemory(edited)));
        EXPECT_FALSE(store.getNoRoom(edited).empty());
    }
    store.insert(edited, makeGradient(16, 16));
    EXPECT_TRUE(store.getNoRoom(edited).empty());
}

TEST_F(LiveTextureTest, ATextureStaysInMemoryUntilARendererAsksForIt)
{
    LiveTextureStore& store = LiveTextureStore::instance();
    const std::vector<std::string> before = sessionFiles();

    store.insert(uriOf("Heap"), makeGradient(8, 8));
    const LiveTextureBufferConstPtr stored = store.find(uriOf("Heap"));
    ASSERT_TRUE(bool(stored));
    EXPECT_TRUE(stored->getFilePath().empty());
    EXPECT_EQ(before, sessionFiles());
}

TEST_F(LiveTextureTest, AHeldFileOutlivesItsStoreEntry)
{
    LiveTextureStore& store = LiveTextureStore::instance();
    const std::string uri = uriOf("Held");
    const LiveTextureBufferConstPtr source = makeGradient(8, 8);

    store.insert(uri, source);
    LiveTextureBufferConstPtr held = store.exportToSharedMemory(uri);
    ASSERT_TRUE(bool(held)) << store.getFailure(uri);
    const std::string path = held->getFilePath();

    store.clear();
    EXPECT_FALSE(store.contains(uri));
    ASSERT_TRUE(fileExists(path));
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fd, 0);
    std::vector<char> bytes(NatronLiveTexture::fileSize(8, 8));
    EXPECT_EQ(ssize_t(bytes.size()), ::pread(fd, bytes.data(), bytes.size(), 0));
    ::close(fd);
    const std::size_t pixelBytes = 8 * 8 * NatronLiveTexture::kTexelBytes;
    EXPECT_EQ(0, std::memcmp(bytes.data() + NatronLiveTexture::kHeaderSize, source->getRow(0), pixelBytes));
    EXPECT_EQ(0, std::memcmp(held->getRow(0), source->getRow(0), pixelBytes));

    held.reset();
    EXPECT_FALSE(fileExists(path));
}

TEST_F(LiveTextureTest, EvictionKeepsAHeldFile)
{
    LiveTextureStore& store = LiveTextureStore::instance();
    const std::string uri = uriOf("Evicted");

    store.insert(uri, makeGradient(8, 8));
    LiveTextureBufferConstPtr held = store.exportToSharedMemory(uri);
    ASSERT_TRUE(bool(held)) << store.getFailure(uri);
    const std::string path = held->getFilePath();

    // The most recently used entry stays even over budget.
    store.insert(uriOf("Newer"), makeGradient(8, 8));
    store.setByteBudget(1);
    EXPECT_FALSE(store.contains(uri));
    EXPECT_TRUE(fileExists(path));
    held.reset();
    EXPECT_FALSE(fileExists(path));
}

// The chain is the delegates' only source of lower levels, on any image size.
TEST_F(LiveTextureTest, TheMipChainHandlesOddSizes)
{
    const int width = 5;
    const int height = 3;
    std::vector<float> pixels(std::size_t(width) * height * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 0.5f;
        pixels[i + 1] = 0.25f;
        pixels[i + 2] = 0.125f;
        pixels[i + 3] = 1.f;
    }
    LiveTextureSharedMemory::Export result;
    int errorNumber = 0;
    std::string what;
    ASSERT_TRUE(LiveTextureSharedMemory::exportImage(uriOf("Odd"), width, height, pixels.data(), &result, &errorNumber, &what)) << what;

    ASSERT_EQ(3, NatronLiveTexture::levelCount(width, height));
    EXPECT_EQ(NatronLiveTexture::fileSize(width, height), result.byteSize);
    int expectedWidth = width;
    int expectedHeight = height;
    for (int index = 0; index < 3; ++index) {
        const NatronLiveTexture::Level level = NatronLiveTexture::levelAt(width, height, index);
        EXPECT_EQ(expectedWidth, level.width);
        EXPECT_EQ(expectedHeight, level.height);
        const float* const texels = reinterpret_cast<const float*>(result.bytes.get() + level.offset);
        for (int i = 0; i < level.width * level.height * 4; i += 4) {
            EXPECT_NEAR(0.5f, texels[i + 0], 1e-5f) << "level " << index;
            EXPECT_NEAR(0.25f, texels[i + 1], 1e-5f) << "level " << index;
            EXPECT_NEAR(0.125f, texels[i + 2], 1e-5f) << "level " << index;
            EXPECT_NEAR(1.f, texels[i + 3], 1e-5f) << "level " << index;
        }
        expectedWidth = std::max(1, expectedWidth / 2);
        expectedHeight = std::max(1, expectedHeight / 2);
    }
}

// The colour is straight, so a lower level weighs each texel's colour by its alpha: a barely covered
// texel must not tint an edge as much as an opaque one.
TEST_F(LiveTextureTest, TheMipChainWeighsColourByAlpha)
{
    const float pixels[] = {
        1.f,
        0.f,
        0.f,
        1.f,
        0.f,
        1.f,
        0.f,
        0.5f,
        0.f,
        0.f,
        1.f,
        0.f,
        0.f,
        0.f,
        1.f,
        0.f,
    };
    const float transparent[] = {
        0.f,
        0.f,
        1.f,
        0.f,
        0.f,
        0.f,
        1.f,
        0.f,
        0.f,
        0.f,
        1.f,
        0.f,
        0.f,
        0.f,
        1.f,
        0.f,
    };
    LiveTextureSharedMemory::Export covered;
    LiveTextureSharedMemory::Export uncovered;
    int errorNumber = 0;
    std::string what;
    ASSERT_TRUE(LiveTextureSharedMemory::exportImage(uriOf("Weighted"), 2, 2, pixels, &covered, &errorNumber, &what)) << what;
    ASSERT_TRUE(LiveTextureSharedMemory::exportImage(uriOf("Uncovered"), 2, 2, transparent, &uncovered, &errorNumber, &what)) << what;

    ASSERT_EQ(2, NatronLiveTexture::levelCount(2, 2));
    const NatronLiveTexture::Level level = NatronLiveTexture::levelAt(2, 2, 1);
    const float* const texel = reinterpret_cast<const float*>(covered.bytes.get() + level.offset);
    EXPECT_NEAR(2.f / 3.f, texel[0], 1e-5f);
    EXPECT_NEAR(1.f / 3.f, texel[1], 1e-5f);
    EXPECT_NEAR(0.f, texel[2], 1e-5f);
    EXPECT_NEAR(0.375f, texel[3], 1e-5f);

    // Nothing covered keeps the colour spread into the transparent texels.
    const float* const clear = reinterpret_cast<const float*>(uncovered.bytes.get() + level.offset);
    EXPECT_NEAR(0.f, clear[0], 1e-5f);
    EXPECT_NEAR(0.f, clear[1], 1e-5f);
    EXPECT_NEAR(1.f, clear[2], 1e-5f);
    EXPECT_NEAR(0.f, clear[3], 1e-5f);
}

TEST_F(LiveTextureTest, AnAbortedExportLeavesNoFile)
{
    const int width = 64;
    const int height = 64;
    const std::vector<float> pixels(std::size_t(width) * height * 4, 0.5f);
    const std::vector<std::string> before = sessionFiles();

    // Once at the start and once part of the way through, after the first block or level.
    for (int allowed : { 0, 1 }) {
        std::atomic<int> calls(0);
        const std::function<bool()> aborted = [&calls, allowed]() { return calls.fetch_add(1) >= allowed; };
        LiveTextureSharedMemory::Export result;
        int errorNumber = 0;
        std::string what;
        EXPECT_FALSE(LiveTextureSharedMemory::exportImage(uriOf("Aborted"), width, height, pixels.data(), &result, &errorNumber, &what, aborted));
        EXPECT_EQ(ECANCELED, errorNumber) << allowed;
        EXPECT_FALSE(bool(result.bytes));
        EXPECT_EQ(before, sessionFiles()) << allowed;
    }
}

// An abort is the render's, not the texture's: no failure is kept, and the next export writes it.
TEST_F(LiveTextureTest, AnExportAbortedByItsRenderIsTriedAgain)
{
    LiveTextureStore& store = LiveTextureStore::instance();
    const std::string uri = uriOf("AbortedByRender");

    store.insert(uri, makeGradient(64, 64));
    {
        const LiveTextureStore::ExportAbortScope scope([]() { return true; });
        EXPECT_FALSE(bool(store.exportToSharedMemory(uri)));
    }
    EXPECT_TRUE(store.getFailure(uri).empty()) << store.getFailure(uri);
    EXPECT_TRUE(store.getNoRoom(uri).empty());
    const LiveTextureBufferConstPtr exported = store.exportToSharedMemory(uri);
    ASSERT_TRUE(bool(exported)) << store.getFailure(uri);
    EXPECT_TRUE(fileExists(exported->getFilePath()));
}

TEST_F(LiveTextureTest, AnImageLargerThanSharedMemoryFailsCleanly)
{
    const std::vector<std::string> before = sessionFiles();
    LiveTextureSharedMemory::Export result;
    int errorNumber = 0;
    std::string what;
    static const float pixel[4] = { 0.f, 0.f, 0.f, 0.f };

    EXPECT_FALSE(LiveTextureSharedMemory::exportImage(uriOf("Huge"), 1 << 20, 1 << 20, pixel, &result, &errorNumber, &what));
    EXPECT_NE(0, errorNumber);
    EXPECT_FALSE(what.empty());
    EXPECT_FALSE(bool(result.bytes));
    EXPECT_EQ(before, sessionFiles());
}

TEST_F(LiveTextureTest, ACrashedSessionsFilesAreReclaimed)
{
    const std::string lock = inSharedMemory("natron-00000000deadbe01.lock");
    const std::string data = inSharedMemory("natron-00000000deadbe01-0000000000000001.natron");
    const std::string partial = inSharedMemory("natron-00000000deadbe01-0000000000000002.natron.tmp");
    const std::string orphan = inSharedMemory("natron-00000000deadbe02-0000000000000001.natron");
    const std::string foreign = inSharedMemory("natron-not-a-session.natron");
    touch(lock);
    touch(data);
    touch(partial);
    touch(orphan);
    touch(foreign);

    EXPECT_GE(LiveTextureSharedMemory::reclaimStaleSessions(), 4);
    EXPECT_FALSE(fileExists(lock));
    EXPECT_FALSE(fileExists(data));
    EXPECT_FALSE(fileExists(partial));
    EXPECT_FALSE(fileExists(orphan));
    EXPECT_TRUE(fileExists(foreign));
    ::unlink(foreign.c_str());
}

TEST_F(LiveTextureTest, ARunningSessionsFilesAreKept)
{
    const std::string lock = inSharedMemory("natron-00000000deadbe03.lock");
    const std::string data = inSharedMemory("natron-00000000deadbe03-0000000000000001.natron");
    touch(lock);
    touch(data);
    const int fd = ::open(lock.c_str(), O_RDWR | O_CLOEXEC);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(0, ::flock(fd, LOCK_EX));

    LiveTextureSharedMemory::reclaimStaleSessions();
    EXPECT_TRUE(fileExists(lock));
    EXPECT_TRUE(fileExists(data));

    ::close(fd);
    LiveTextureSharedMemory::reclaimStaleSessions();
    EXPECT_FALSE(fileExists(lock));
    EXPECT_FALSE(fileExists(data));
}

TEST_F(LiveTextureTest, ReclaimingLeavesThisSessionAlone)
{
    LiveTextureStore& store = LiveTextureStore::instance();

    store.insert(uriOf("Mine"), makeGradient(8, 8));
    const LiveTextureBufferConstPtr exported = store.exportToSharedMemory(uriOf("Mine"));
    ASSERT_TRUE(bool(exported)) << store.getFailure(uriOf("Mine"));

    LiveTextureSharedMemory::reclaimStaleSessions();
    EXPECT_TRUE(fileExists(exported->getFilePath()));
}

TEST_F(LiveTextureTest, EndingASessionRemovesItsFiles)
{
    const LiveTextureBufferConstPtr source = makeGradient(8, 8);
    LiveTextureSharedMemory::Export result;
    int errorNumber = 0;
    std::string what;
    ASSERT_TRUE(LiveTextureSharedMemory::exportImage(uriOf("Ending"), 8, 8, source->getRow(0), &result, &errorNumber, &what)) << what;
    const std::string session = LiveTextureSharedMemory::sessionName();
    ASSERT_FALSE(session.empty());
    ASSERT_TRUE(fileExists(result.path));

    LiveTextureSharedMemory::endSession();
    EXPECT_TRUE(namesStartingWith("natron-" + session).empty());
    EXPECT_EQ(0, std::memcmp(result.bytes.get() + NatronLiveTexture::kHeaderSize, source->getRow(0), 8 * 8 * NatronLiveTexture::kTexelBytes));

    LiveTextureSharedMemory::Export next;
    ASSERT_TRUE(LiveTextureSharedMemory::exportImage(uriOf("Ending"), 8, 8, source->getRow(0), &next, &errorNumber, &what)) << what;
    EXPECT_NE(session, LiveTextureSharedMemory::sessionName());
    EXPECT_TRUE(fileExists(next.path));
    EXPECT_NE(result.path, next.path);
}

TEST_F(LiveTextureTest, TheFilterGivesAStoredTextureItsFileAndKeepsTheAsset)
{
    const std::string uri = LiveTextureSource::makeUri("Filtered", "rgba", 0x1234abcdULL);
    LiveTextureStore& store = LiveTextureStore::instance();

    store.insert(uri, makeGradient(8, 8));
    PXR_NS::HdMaterialNetwork2 network = makeTexturedNetwork(uri, "diffuseColor", "rgb");
    const auto filter = LiveTextureSceneIndex::makeMaterialFilter();
    applyFilter(filter, &network);

    ASSERT_TRUE(hasTextureNode(network));
    const PXR_NS::VtValue file = network.nodes[PXR_NS::SdfPath(kTextureNode)].parameters[PXR_NS::TfToken("file")];
    ASSERT_TRUE(file.IsHolding<PXR_NS::SdfAssetPath>());
    const PXR_NS::SdfAssetPath asset = file.UncheckedGet<PXR_NS::SdfAssetPath>();
    EXPECT_EQ(uri, asset.GetAssetPath());
    EXPECT_EQ(store.find(uri)->getFilePath(), asset.GetResolvedPath());
    EXPECT_TRUE(fileExists(asset.GetResolvedPath()));
    EXPECT_EQ(1u, surfaceConnections(network, "diffuseColor"));

    store.clear();
    EXPECT_TRUE(fileExists(asset.GetResolvedPath()));
}

// A frame's render can still be reading a file while the next frame's network is filtered.
TEST_F(LiveTextureTest, TheFilterHoldsTheFileBeforeTheCurrentOne)
{
    LiveTextureStore& store = LiveTextureStore::instance();
    const auto filter = LiveTextureSceneIndex::makeMaterialFilter();
    std::vector<std::string> paths;

    for (int frame = 0; frame < 3; ++frame) {
        const std::string uri = LiveTextureSource::makeUri("Sequence", "rgba", 0x1000ULL + frame);
        store.insert(uri, makeGradient(8, 8));
        PXR_NS::HdMaterialNetwork2 network = makeTexturedNetwork(uri, "diffuseColor", "rgb");
        applyFilter(filter, &network);
        ASSERT_TRUE(hasTextureNode(network));
        const PXR_NS::VtValue file = network.nodes[PXR_NS::SdfPath(kTextureNode)].parameters[PXR_NS::TfToken("file")];
        paths.push_back(file.Get<PXR_NS::SdfAssetPath>().GetResolvedPath());
    }
    store.clear();
    EXPECT_FALSE(fileExists(paths[0]));
    EXPECT_TRUE(fileExists(paths[1]));
    EXPECT_TRUE(fileExists(paths[2]));
}

// The delegate can read a removed material's files until it syncs the removal, so they go at the
// next one; other materials keep theirs.
TEST_F(LiveTextureTest, TheFilterLetsGoOfARemovedMaterialsFilesAtTheNextRemoval)
{
    LiveTextureStore& store = LiveTextureStore::instance();
    const LiveTextureSceneIndex::MaterialFilter filter = LiveTextureSceneIndex::makeMaterialFilter();
    const auto filtered = [&](const char* material, const char* node, U64 hash) {
        const std::string uri = LiveTextureSource::makeUri(node, "rgba", hash);
        store.insert(uri, makeGradient(8, 8));
        PXR_NS::HdMaterialNetwork2 network = makeTexturedNetwork(uri, "diffuseColor", "rgb");
        applyFilter(filter, &network, material);

        return network.nodes[PXR_NS::SdfPath(kTextureNode)].parameters[PXR_NS::TfToken("file")].Get<PXR_NS::SdfAssetPath>().GetResolvedPath();
    };

    const std::string removed = filtered("/Looks/Removed", "Removed", 0x2000ULL);
    const std::string child = filtered("/Looks/Removed/Child", "Child", 0x2001ULL);
    const std::string kept = filtered("/Kept", "Kept", 0x2002ULL);
    store.clear();

    filter.forget({ PXR_NS::SdfPath("/Looks/Removed") });
    EXPECT_TRUE(fileExists(removed));
    EXPECT_TRUE(fileExists(child));
    filter.forget({ PXR_NS::SdfPath("/Elsewhere") });
    EXPECT_FALSE(fileExists(removed));
    EXPECT_FALSE(fileExists(child));
    EXPECT_TRUE(fileExists(kept));
}

// Cycles reads a live texture as it is: no conversion from a colour space, and the colour straight
// whether or not its alpha is connected.
TEST_F(LiveTextureTest, TheCyclesFilterReadsALiveTextureAsItIs)
{
    const std::string uri = LiveTextureSource::makeUri("Raw", "rgba", 0x3000ULL);
    LiveTextureStore::instance().insert(uri, makeGradient(8, 8));
    const PXR_NS::TfToken colorSpace("sourceColorSpace");
    const PXR_NS::TfToken alphaType("alpha_type");

    PXR_NS::HdMaterialNetwork2 cycles = makeTexturedNetwork(uri, "diffuseColor", "rgb");
    cycles.nodes[PXR_NS::SdfPath(kTextureNode)].parameters[colorSpace] = PXR_NS::VtValue(PXR_NS::TfToken("auto"));
    applyFilter(LiveTextureSceneIndex::makeCyclesMaterialFilter(), &cycles);
    ASSERT_TRUE(hasTextureNode(cycles));
    std::map<PXR_NS::TfToken, PXR_NS::VtValue>& parameters = cycles.nodes[PXR_NS::SdfPath(kTextureNode)].parameters;
    EXPECT_EQ(PXR_NS::VtValue(PXR_NS::TfToken("raw")), parameters[colorSpace]);
    EXPECT_EQ(PXR_NS::VtValue(PXR_NS::TfToken("channel_packed")), parameters[alphaType]);

    PXR_NS::HdMaterialNetwork2 plain = makeTexturedNetwork("/plates/plain.exr", "diffuseColor", "rgb");
    applyFilter(LiveTextureSceneIndex::makeCyclesMaterialFilter(), &plain);
    EXPECT_EQ(0u, plain.nodes[PXR_NS::SdfPath(kTextureNode)].parameters.count(alphaType));
    EXPECT_EQ(0u, plain.nodes[PXR_NS::SdfPath(kTextureNode)].parameters.count(colorSpace));

    // MoonRay converts by gamma, which "auto" applies only to 8-bit images.
    PXR_NS::HdMaterialNetwork2 moonRay = makeTexturedNetwork(uri, "diffuseColor", "rgb");
    applyFilter(LiveTextureSceneIndex::makeMaterialFilter(), &moonRay);
    EXPECT_EQ(0u, moonRay.nodes[PXR_NS::SdfPath(kTextureNode)].parameters.count(alphaType));
}

TEST_F(LiveTextureTest, TheFilterLeavesOtherTexturesAlone)
{
    PXR_NS::HdMaterialNetwork2 network = makeTexturedNetwork("/plates/plain.exr", "diffuseColor", "rgb");
    applyFilter(LiveTextureSceneIndex::makeMaterialFilter(), &network);

    ASSERT_TRUE(hasTextureNode(network));
    EXPECT_EQ(std::string("/plates/plain.exr"),
              network.nodes[PXR_NS::SdfPath(kTextureNode)].parameters[PXR_NS::TfToken("file")].Get<PXR_NS::SdfAssetPath>().GetAssetPath());
    EXPECT_EQ(1u, surfaceConnections(network, "diffuseColor"));
}

TEST_F(LiveTextureTest, TheFilterFeedsAMissingTexturesMagentaDownstreamWhateverItsFallback)
{
    const std::string uri = uriOf("Gone");
    LiveTextureStore& store = LiveTextureStore::instance();
    const PXR_NS::GfVec4f fallback(0.25f, 0.5f, 0.75f, 0.125f);

    PXR_NS::HdMaterialNetwork2 colour = makeTexturedNetwork(uri, "diffuseColor", "rgb");
    colour.nodes[PXR_NS::SdfPath(kTextureNode)].parameters[PXR_NS::TfToken("fallback")] = PXR_NS::VtValue(fallback);
    applyFilter(LiveTextureSceneIndex::makeMaterialFilter(), &colour);
    EXPECT_FALSE(hasTextureNode(colour));
    EXPECT_EQ(0u, surfaceConnections(colour, "diffuseColor"));
    const PXR_NS::VtValue diffuse = surfaceParameter(colour, "diffuseColor");
    ASSERT_TRUE(diffuse.IsHolding<PXR_NS::GfVec3f>());
    EXPECT_EQ(PXR_NS::GfVec3f(kLiveTextureMissingColour[0], kLiveTextureMissingColour[1], kLiveTextureMissingColour[2]), diffuse.UncheckedGet<PXR_NS::GfVec3f>());
    EXPECT_FALSE(store.getFailure(uri).empty());

    PXR_NS::HdMaterialNetwork2 opacity = makeTexturedNetwork(uri, "opacity", "a");
    opacity.nodes[PXR_NS::SdfPath(kTextureNode)].parameters[PXR_NS::TfToken("fallback")] = PXR_NS::VtValue(fallback);
    applyFilter(LiveTextureSceneIndex::makeMaterialFilter(), &opacity);
    EXPECT_FALSE(hasTextureNode(opacity));
    const PXR_NS::VtValue alpha = surfaceParameter(opacity, "opacity");
    ASSERT_TRUE(alpha.IsHolding<float>());
    EXPECT_EQ(kLiveTextureMissingColour[3], alpha.UncheckedGet<float>());
}

TEST_F(LiveTextureTest, AMissingNormalMapLeavesTheGeometrysNormal)
{
    PXR_NS::HdMaterialNetwork2 network = makeTexturedNetwork(uriOf("GoneNormal"), "normal", "rgb");
    applyFilter(LiveTextureSceneIndex::makeMaterialFilter(), &network);

    EXPECT_FALSE(hasTextureNode(network));
    EXPECT_EQ(0u, surfaceConnections(network, "normal"));
    EXPECT_TRUE(surfaceParameter(network, "normal").IsEmpty());
}

TEST_F(LiveTextureTest, TheCyclesFilterTurnsATransformIntoAMappingNode)
{
    const PXR_NS::SdfPath readerPath("/Material/Reader");
    const PXR_NS::SdfPath transformPath("/Material/Transform");
    const PXR_NS::SdfPath texturePath("/Material/Texture");

    PXR_NS::HdMaterialNode2 transform;
    transform.nodeTypeId = PXR_NS::TfToken("UsdTransform2d");
    transform.parameters[PXR_NS::TfToken("scale")] = PXR_NS::VtValue(PXR_NS::GfVec2f(2.f, 4.f));
    transform.parameters[PXR_NS::TfToken("rotation")] = PXR_NS::VtValue(90.f);
    transform.parameters[PXR_NS::TfToken("translation")] = PXR_NS::VtValue(PXR_NS::GfVec2f(0.25f, 0.5f));
    PXR_NS::HdMaterialConnection2 fromReader;
    fromReader.upstreamNode = readerPath;
    fromReader.upstreamOutputName = PXR_NS::TfToken("result");
    transform.inputConnections[PXR_NS::TfToken("in")] = { fromReader };

    PXR_NS::HdMaterialNode2 texture;
    texture.nodeTypeId = PXR_NS::TfToken("UsdUVTexture");
    PXR_NS::HdMaterialConnection2 fromTransform;
    fromTransform.upstreamNode = transformPath;
    fromTransform.upstreamOutputName = PXR_NS::TfToken("result");
    texture.inputConnections[PXR_NS::TfToken("st")] = { fromTransform };

    PXR_NS::HdMaterialNode2 reader;
    reader.nodeTypeId = PXR_NS::TfToken("UsdPrimvarReader_float2");

    PXR_NS::HdMaterialNetwork2 network;
    network.nodes[readerPath] = reader;
    network.nodes[transformPath] = transform;
    network.nodes[texturePath] = texture;
    applyFilter(LiveTextureSceneIndex::makeCyclesMaterialFilter(), &network);

    const PXR_NS::HdMaterialNode2& mapping = network.nodes.at(transformPath);
    EXPECT_EQ(PXR_NS::TfToken("cycles_mapping"), mapping.nodeTypeId);
    EXPECT_EQ(PXR_NS::VtValue(PXR_NS::GfVec3f(2.f, 4.f, 1.f)), mapping.parameters.at(PXR_NS::TfToken("scale")));
    const PXR_NS::VtValue rotation = mapping.parameters.at(PXR_NS::TfToken("rotation"));
    ASSERT_TRUE(rotation.IsHolding<PXR_NS::GfVec3f>());
    EXPECT_FLOAT_EQ(float(M_PI / 2.), rotation.UncheckedGet<PXR_NS::GfVec3f>()[2]);
    EXPECT_EQ(PXR_NS::VtValue(PXR_NS::GfVec3f(0.25f, 0.5f, 0.f)), mapping.parameters.at(PXR_NS::TfToken("Location")));
    EXPECT_TRUE(mapping.parameters.find(PXR_NS::TfToken("translation")) == mapping.parameters.end());
    ASSERT_EQ(1u, mapping.inputConnections.count(PXR_NS::TfToken("Vector")));
    EXPECT_EQ(readerPath, mapping.inputConnections.at(PXR_NS::TfToken("Vector"))[0].upstreamNode);
    EXPECT_EQ(0u, mapping.inputConnections.count(PXR_NS::TfToken("in")));
    const PXR_NS::HdMaterialConnection2& st = network.nodes.at(texturePath).inputConnections.at(PXR_NS::TfToken("st"))[0];
    EXPECT_EQ(transformPath, st.upstreamNode);
    EXPECT_EQ(PXR_NS::TfToken("Vector"), st.upstreamOutputName);
}

TEST_F(LiveTextureTest, TheOtherDelegatesFilterKeepsATransform)
{
    PXR_NS::HdMaterialNode2 transform;
    transform.nodeTypeId = PXR_NS::TfToken("UsdTransform2d");
    PXR_NS::HdMaterialNetwork2 network;
    network.nodes[PXR_NS::SdfPath("/Material/Transform")] = transform;
    applyFilter(LiveTextureSceneIndex::makeMaterialFilter(), &network);

    EXPECT_EQ(PXR_NS::TfToken("UsdTransform2d"), network.nodes.at(PXR_NS::SdfPath("/Material/Transform")).nodeTypeId);
}

// The render stops on the no-room note, so the delegate keeps the image it already has: magenta
// would flash for a frame that is never shown.
TEST_F(LiveTextureTest, AFullSharedMemoryKeepsTheHeldFileInTheNetwork)
{
    LiveTextureStore& store = LiveTextureStore::instance();
    const std::string fits = LiveTextureSource::makeUri("Plate", "rgba", 0x5000ULL);
    const std::string tooBig = LiveTextureSource::makeUri("Plate", "rgba", 0x5001ULL);
    const auto filter = LiveTextureSceneIndex::makeMaterialFilter();

    store.insert(fits, makeGradient(8, 8));
    PXR_NS::HdMaterialNetwork2 first = makeTexturedNetwork(fits, "diffuseColor", "rgb");
    applyFilter(filter, &first);
    ASSERT_TRUE(hasTextureNode(first));
    const std::string heldPath = first.nodes[PXR_NS::SdfPath(kTextureNode)].parameters[PXR_NS::TfToken("file")].Get<PXR_NS::SdfAssetPath>().GetResolvedPath();
    ASSERT_FALSE(heldPath.empty());

    store.insert(tooBig, makeOversizedBuffer());
    const SharedMemoryLimit full(1024 * 1024);
    PXR_NS::HdMaterialNetwork2 next = makeTexturedNetwork(tooBig, "diffuseColor", "rgb");
    applyFilter(filter, &next);

    ASSERT_TRUE(hasTextureNode(next));
    EXPECT_EQ(heldPath, next.nodes[PXR_NS::SdfPath(kTextureNode)].parameters[PXR_NS::TfToken("file")].Get<PXR_NS::SdfAssetPath>().GetResolvedPath());
    EXPECT_EQ(1u, surfaceConnections(next, "diffuseColor"));
    EXPECT_TRUE(fileExists(heldPath));

    char needed[64];
    std::snprintf(needed, sizeof(needed), "needs %.1f MB", double(NatronLiveTexture::fileSize(1 << 20, 1 << 20)) / (1024. * 1024.));
    const std::string noRoom = store.getNoRoom(tooBig);
    EXPECT_NE(std::string::npos, noRoom.find("Plate")) << noRoom;
    EXPECT_NE(std::string::npos, noRoom.find(needed)) << noRoom;
    EXPECT_NE(std::string::npos, noRoom.find("only 1.0 MB is free")) << noRoom;
    EXPECT_NE(std::string::npos, noRoom.find("the render stops")) << noRoom;

    // A prim that never had a file has nothing to keep.
    PXR_NS::HdMaterialNetwork2 unheld = makeTexturedNetwork(tooBig, "diffuseColor", "rgb");
    applyFilter(LiveTextureSceneIndex::makeMaterialFilter(), &unheld);
    EXPECT_FALSE(hasTextureNode(unheld));
}

namespace {
const qint64 kSlowSourceMs = 20000;
const qint64 kPromptPrepareMs = 2000;

// A source whose render runs until it is aborted, and a consumer to warn.
struct SlowSource {
    NodePtr node;
    NodePtr consumer;
    LiveTextureSource source;
    std::atomic<bool> rendering;

    SlowSource(const NodePtr& counting,
               const NodePtr& consumerNode)
        : node(counting)
        , consumer(consumerNode)
        , rendering(false)
    {
        CountingTestRegistry::reset();
        CountingTestRegistry::setRenderHook([this](const Node* rendered) {
            rendering = true;
            QElapsedTimer timer;
            timer.start();
            while (!rendered->aborted() && (timer.elapsed() < kSlowSourceMs)) {
                QThread::msleep(5);
            }
        });
        if (node) {
            source.node = node;
            source.consumer = consumer;
            source.nodeName = node->getFullyQualifiedName();
            source.layer = "rgba";
            source.contentHash = node->getEffectInstance()->getHash();
        }
    }

    ~SlowSource()
    {
        CountingTestRegistry::reset();
    }

    bool waitForRendering() const
    {
        QElapsedTimer started;
        started.start();
        while (!rendering && (started.elapsed() < kSlowSourceMs)) {
            QThread::msleep(5);
        }

        return rendering.load();
    }
};
} // anonymous namespace

// A render cancelled while a source is rendering for it must not wait for that source.
TEST_F(LiveTextureTest, AnAbortedPrepareStopsTheSourceAndRecordsNoFailure)
{
    SlowSource slow(createNode(QString::fromUtf8(kTestPluginIDCounting)), createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT)));
    ASSERT_TRUE(bool(slow.node));
    ASSERT_TRUE(bool(slow.consumer));
    const std::string uri = slow.source.getUri(1.);

    std::atomic<bool> aborted(false);
    int numFailed = -1;
    std::thread worker([&]() {
        numFailed = LiveTextureStore::instance().prepare(LiveTextureSourceList(1, slow.source), 1.,
                                                         [&aborted]() { return aborted.load(); });
    });
    EXPECT_TRUE(slow.waitForRendering()) << "the source never started rendering";

    QElapsedTimer aborting;
    aborting.start();
    aborted = true;
    worker.join();
    EXPECT_LT(aborting.elapsed(), kPromptPrepareMs);
    EXPECT_EQ(0, numFailed);
    EXPECT_FALSE(LiveTextureStore::instance().contains(uri));
    EXPECT_TRUE(LiveTextureStore::instance().getFailure(uri).empty());
    EXPECT_FALSE(slow.consumer->hasPersistentMessage());

    // Not remembered as failed: the next prepare renders it.
    CountingTestRegistry::setRenderHook(CountingTestRegistry::RenderHook());
    EXPECT_EQ(0, prepareOffMainThread(LiveTextureSourceList(1, slow.source), 1.));
    EXPECT_TRUE(LiveTextureStore::instance().contains(uri));
}

// Waiting on another render's texture is cancelled as promptly as rendering one.
TEST_F(LiveTextureTest, AnAbortedPrepareStopsWaitingForAnotherRendersTexture)
{
    SlowSource slow(createNode(QString::fromUtf8(kTestPluginIDCounting)), createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT)));
    ASSERT_TRUE(bool(slow.node));
    const std::string uri = slow.source.getUri(1.);

    std::atomic<bool> firstAborted(false);
    std::thread first([&]() {
        LiveTextureStore::instance().prepare(LiveTextureSourceList(1, slow.source), 1.,
                                             [&firstAborted]() { return firstAborted.load(); });
    });
    EXPECT_TRUE(slow.waitForRendering()) << "the source never started rendering";

    std::atomic<bool> secondAborted(false);
    int numFailed = -1;
    std::thread second([&]() {
        numFailed = LiveTextureStore::instance().prepare(LiveTextureSourceList(1, slow.source), 1.,
                                                         [&secondAborted]() { return secondAborted.load(); });
    });
    QThread::msleep(100);

    QElapsedTimer aborting;
    aborting.start();
    secondAborted = true;
    second.join();
    EXPECT_LT(aborting.elapsed(), kPromptPrepareMs);
    EXPECT_EQ(0, numFailed);
    EXPECT_TRUE(LiveTextureStore::instance().getFailure(uri).empty());

    firstAborted = true;
    first.join();
}
