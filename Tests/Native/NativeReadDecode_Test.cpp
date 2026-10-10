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
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QString>
#include <QTemporaryDir>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/OiioReadSupport.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 64;
const int kHeight = 200;

struct FormatCase {
    const char* file;
    OIIO::TypeDesc type;
    int channels;
    bool optional;
};

float
patternValue(int x,
             int y,
             int c)
{
    return (float)((x * 131 + y * 17 + c * 7) % 251) / 250.f;
}

// Returns an empty string when an optional format has no writer in this OIIO build.
std::string
writeFixture(const QTemporaryDir& dir,
             const FormatCase& f)
{
    const std::string path = (dir.path() + QString::fromUtf8("/") + QString::fromUtf8(f.file)).toStdString();
    OIIO::ImageSpec spec(kWidth, kHeight, f.channels, f.type);
    std::vector<float> pixels((std::size_t)kWidth * kHeight * f.channels);
    for (int j = 0; j < kHeight; ++j) {
        for (int i = 0; i < kWidth; ++i) {
            for (int c = 0; c < f.channels; ++c) {
                pixels[((std::size_t)j * kWidth + i) * f.channels + c] = patternValue(i, j, c);
            }
        }
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    if (!out && f.optional) {
        OIIO::geterror();

        return std::string();
    }
    EXPECT_TRUE(bool(out)) << f.file;
    if (!out) {
        return std::string();
    }
    EXPECT_TRUE(out->open(path, spec)) << out->geterror();
    EXPECT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
    EXPECT_TRUE(out->close()) << out->geterror();

    return path;
}

const ImageLayerDesc&
layerForComponents(int nComps)
{
    if (nComps == 1) {
        return ImageLayerDesc::getAlphaComponents();
    }
    if (nComps == 3) {
        return ImageLayerDesc::getRGBComponents();
    }

    return ImageLayerDesc::getRGBAComponents();
}

std::string
persistentMessage(const NodePtr& node)
{
    QString message;
    int type = 0;

    node->getPersistentMessage(&message, &type, false);

    return message.toStdString();
}

struct Decoded {
    OIIO::ImageSpec spec;
    std::vector<float> pixels; // file order, top row first
};

bool
readWithOiio(const std::string& path,
             Decoded* decoded)
{
    OIIO::ImageInput::unique_ptr input = OIIO::ImageInput::open(path);
    if (!input) {
        return false;
    }
    decoded->spec = input->spec();
    decoded->pixels.resize((std::size_t)decoded->spec.width * decoded->spec.height * decoded->spec.nchannels);
    const bool ok = input->read_image(0, 0, 0, decoded->spec.nchannels, OIIO::TypeDesc::FLOAT, decoded->pixels.data());
    input->close();

    return ok;
}
} // namespace

class NativeReadDecodeTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        OiioReadSupport::clearHeaderCache();
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createRead()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        EXPECT_TRUE(bool(node));
        EXPECT_TRUE(node && dynamic_cast<NativeRead*>(node->getEffectInstance().get()));

        return node;
    }

    void setFile(const NodePtr& node,
                 const std::string& path)
    {
        KnobFile* file = dynamic_cast<KnobFile*>(node->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(file != NULL);
        file->setValue(path);
        // These tests compare with OIIO's own decode, so the file is read without conversion.
        KnobStringBase* inputSpace = dynamic_cast<KnobStringBase*>(node->getKnobByName("ocioInputSpace").get());
        ASSERT_TRUE(inputSpace != NULL);
        inputSpace->setValue(getApp()->getProject()->getWorkingColorSpace());
        node->getEffectInstance()->refreshMetadata_public(false);
    }

    // Compares the plane with OIIO's own read of the file, converted to float with the same
    // orientation.
    void expectMatchesOiio(const std::string& path,
                           int expectedComps)
    {
        SCOPED_TRACE(path);

        Decoded ref;
        ASSERT_TRUE(readWithOiio(path, &ref));
        ASSERT_EQ(expectedComps, ref.spec.nchannels);
        const RectI window = OiioReadSupport::dataWindowOf(ref.spec);

        NodePtr node = createRead();
        setFile(node, path);

        std::list<ImageLayerDesc> layers(1, layerForComponents(expectedComps));
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, window, layers, &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        ASSERT_EQ((std::size_t)expectedComps, planes[0].channels.size());
        ASSERT_EQ((std::size_t)window.width() * window.height() * expectedComps, planes[0].pixels.size());

        std::size_t mismatches = 0;
        for (int row = 0; row < window.height(); ++row) {
            const int fileRow = window.height() - 1 - row;
            for (int col = 0; col < window.width(); ++col) {
                for (int c = 0; c < expectedComps; ++c) {
                    const float got = planes[0].pixels[((std::size_t)row * window.width() + col) * expectedComps + c];
                    const float want = ref.pixels[((std::size_t)fileRow * window.width() + col) * expectedComps + c];
                    if (got != want && ++mismatches <= 5) {
                        ADD_FAILURE() << "pixel " << col << "," << row << " channel " << c << ": got " << got << ", want " << want;
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches);
    }

    // Renders the half-size window at mipmap level 1 and compares it with downscaleMipmap of
    // OIIO's own full-size read.
    void expectMipmapMatchesDownscale(const std::string& path,
                                      int nComps)
    {
        SCOPED_TRACE(path);

        Decoded ref;
        ASSERT_TRUE(readWithOiio(path, &ref));
        ASSERT_EQ(nComps, ref.spec.nchannels);
        const RectI fullBounds = OiioReadSupport::dataWindowOf(ref.spec);
        const RectI half(fullBounds.x1 / 2, fullBounds.y1 / 2, fullBounds.x2 / 2, fullBounds.y2 / 2);
        const RectD rod = fullBounds.toCanonical_noClipping(0, 1.);

        ImagePtr exact = std::make_shared<Image>(layerForComponents(nComps), rod, fullBounds, 0, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false);
        ImagePtr reduced = std::make_shared<Image>(layerForComponents(nComps), rod, half, 1, 1., eImageBitDepthFloat, eImageFieldingOrderNone, false);
        {
            Image::WriteAccess access(exact.get());
            for (int y = fullBounds.y1; y < fullBounds.y2; ++y) {
                float* dst = (float*)access.pixelAt(fullBounds.x1, y);
                const int fileRow = fullBounds.y2 - 1 - y;
                for (int x = 0; x < fullBounds.width(); ++x) {
                    for (int c = 0; c < nComps; ++c) {
                        dst[x * nComps + c] = ref.pixels[((std::size_t)fileRow * fullBounds.width() + x) * nComps + c];
                    }
                }
            }
        }
        exact->downscaleMipmap(rod, fullBounds, 0, 1, false, reduced.get());

        NodePtr node = createRead();
        setFile(node, path);
        std::list<ImageLayerDesc> layers(1, layerForComponents(nComps));
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 1, half, layers, &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        ASSERT_EQ((std::size_t)half.width() * half.height() * nComps, planes[0].pixels.size());

        std::size_t mismatches = 0;
        Image::ReadAccess access(reduced.get());
        for (int y = half.y1; y < half.y2; ++y) {
            const float* want = (const float*)access.pixelAt(half.x1, y);
            for (int x = 0; x < half.width(); ++x) {
                for (int c = 0; c < nComps; ++c) {
                    const float got = planes[0].pixels[((std::size_t)(y - half.y1) * half.width() + x) * nComps + c];
                    if (got != want[x * nComps + c] && ++mismatches <= 5) {
                        ADD_FAILURE() << "pixel " << x << "," << y << " channel " << c << ": got " << got << ", want " << want[x * nComps + c];
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches);
    }
};

namespace {
const FormatCase kFormats[] = {
    { "half.exr", OIIO::TypeDesc::HALF, 4, false },
    { "float.exr", OIIO::TypeDesc::FLOAT, 4, false },
    { "float-rgb.exr", OIIO::TypeDesc::FLOAT, 3, false },
    { "rgb8.png", OIIO::TypeDesc::UINT8, 3, false },
    { "rgba16.png", OIIO::TypeDesc::UINT16, 4, false },
    { "rgb.jpg", OIIO::TypeDesc::UINT8, 3, false },
    { "rgba16.tif", OIIO::TypeDesc::UINT16, 4, false },
    { "rgb16.dpx", OIIO::TypeDesc::UINT16, 3, false },
    { "rgba.tga", OIIO::TypeDesc::UINT8, 4, false },
    { "rgb.hdr", OIIO::TypeDesc::FLOAT, 3, false },
    { "rgb.pfm", OIIO::TypeDesc::FLOAT, 3, true },
};
} // namespace

TEST_F(NativeReadDecodeTest, MipmapZeroIsBitExactAgainstOiioForEveryFormat)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (std::size_t i = 0; i < sizeof(kFormats) / sizeof(kFormats[0]); ++i) {
        const std::string path = writeFixture(dir, kFormats[i]);
        if (path.empty()) {
            continue;
        }
        expectMatchesOiio(path, kFormats[i].channels);
    }
}

TEST_F(NativeReadDecodeTest, MipmapOneEqualsDownscaleOfTheExactImage)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (std::size_t i = 0; i < sizeof(kFormats) / sizeof(kFormats[0]); ++i) {
        const std::string path = writeFixture(dir, kFormats[i]);
        if (path.empty()) {
            continue;
        }
        expectMipmapMatchesDownscale(path, kFormats[i].channels);
    }
}

TEST_F(NativeReadDecodeTest, RegionOfDefinitionIsTheDataWindowAndFormatIsTheDisplayWindow)
{
    const std::string path = std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgb-offset-window.exr");
    Decoded ref;
    ASSERT_TRUE(readWithOiio(path, &ref));
    const RectI dataWindow = OiioReadSupport::dataWindowOf(ref.spec);

    NodePtr node = createRead();
    setFile(node, path);
    NativeRead* read = dynamic_cast<NativeRead*>(node->getEffectInstance().get());
    ASSERT_TRUE(read != NULL);

    RectD rod;
    ASSERT_EQ(eStatusOK, read->getRegionOfDefinition(0, 1., RenderScale(), ViewIdx(0), &rod));
    const RectD expected = dataWindow.toCanonical_noClipping(0, 1.);
    EXPECT_EQ(expected.x1, rod.x1);
    EXPECT_EQ(expected.y1, rod.y1);
    EXPECT_EQ(expected.x2, rod.x2);
    EXPECT_EQ(expected.y2, rod.y2);
    EXPECT_NE(RectI(0, 0, ref.spec.width, ref.spec.height), dataWindow);

    EXPECT_TRUE(RectI(0, 0, ref.spec.full_x + ref.spec.full_width, ref.spec.full_height) == read->getOutputFormat());
}

TEST_F(NativeReadDecodeTest, ComponentsFollowTheFilesColourChannels)
{
    expectMatchesOiio(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"), 4);
    expectMatchesOiio(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgb-only.exr"), 3);
    expectMatchesOiio(std::string(NATRON_TESTS_FIXTURES_DIR "/flat-alpha-only.exr"), 1);
    expectMatchesOiio(std::string(NATRON_TESTS_FIXTURES_DIR "/png-8bit.png"), 3);
}

TEST_F(NativeReadDecodeTest, ComponentCountOfTheMetadataFollowsTheFile)
{
    NodePtr node = createRead();
    EffectInstance* effect = node->getEffectInstance().get();
    ImageLayerDesc layer;
    ImageLayerDesc paired;

    setFile(node, std::string(NATRON_TESTS_FIXTURES_DIR "/png-8bit.png"));
    effect->getMetadataComponents(-1, &layer, &paired);
    EXPECT_EQ(3u, layer.getChannels().size());
    setFile(node, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-alpha-only.exr"));
    effect->getMetadataComponents(-1, &layer, &paired);
    EXPECT_EQ(1u, layer.getChannels().size());
    setFile(node, std::string(NATRON_TESTS_FIXTURES_DIR "/flat-rgba-only.exr"));
    effect->getMetadataComponents(-1, &layer, &paired);
    EXPECT_EQ(4u, layer.getChannels().size());
}

namespace {
const int kSmall = 8;

float
smallValue(int x,
           int y,
           int c)
{
    return (float)((x * 31 + y * 17 + c * 53) % 251) / 250.f;
}

// Writes a kSmall x kSmall file whose channels are named `names`.
std::string
writeSmallFile(const QTemporaryDir& dir,
               const char* file,
               OIIO::TypeDesc type,
               const std::vector<std::string>& names)
{
    const std::string path = (dir.path() + QString::fromUtf8("/") + QString::fromUtf8(file)).toStdString();
    const int n = (int)names.size();
    OIIO::ImageSpec spec(kSmall, kSmall, n, type);
    spec.channelnames = names;
    for (int c = 0; c < n; ++c) {
        if (names[(std::size_t)c] == "A") {
            spec.alpha_channel = c;
        }
    }
    std::vector<float> pixels((std::size_t)kSmall * kSmall * n);
    for (int y = 0; y < kSmall; ++y) {
        for (int x = 0; x < kSmall; ++x) {
            for (int c = 0; c < n; ++c) {
                pixels[((std::size_t)y * kSmall + x) * n + c] = smallValue(x, y, c);
            }
        }
    }
    OIIO::ImageOutput::unique_ptr out = OIIO::ImageOutput::create(path);
    EXPECT_TRUE(bool(out)) << file;
    if (!out) {
        return std::string();
    }
    EXPECT_TRUE(out->open(path, spec)) << out->geterror();
    EXPECT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
    EXPECT_TRUE(out->close()) << out->geterror();

    return path;
}
} // namespace

TEST_F(NativeReadDecodeTest, AFileWithoutAnyColourChannelGivesABlackRgbaPlane)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = writeSmallFile(dir, "layers.exr", OIIO::TypeDesc::FLOAT, { "diffuse.R", "diffuse.G", "diffuse.B" });
    ASSERT_FALSE(path.empty());

    NodePtr node = createRead();
    setFile(node, path);
    ImageLayerDesc layer;
    ImageLayerDesc paired;
    node->getEffectInstance()->getMetadataComponents(-1, &layer, &paired);
    EXPECT_EQ(4u, layer.getChannels().size());

    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kSmall, kSmall), layers, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    ASSERT_EQ((std::size_t)kSmall * kSmall * 4u, planes[0].pixels.size());
    for (std::size_t i = 0; i < planes[0].pixels.size(); ++i) {
        EXPECT_EQ(0.f, planes[0].pixels[i]) << i;
    }
}

TEST_F(NativeReadDecodeTest, ALuminanceFileFillsRedGreenAndBlueWithItsValue)
{
    struct Case {
        const char* file;
        OIIO::TypeDesc type;
        const char* channel;
    };
    const Case cases[] = {
        { "grey.png", OIIO::TypeDesc::UINT8, "Y" },
        { "grey.tif", OIIO::TypeDesc::UINT16, "Y" },
        { "grey-i.exr", OIIO::TypeDesc::FLOAT, "I" },
    };
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (std::size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        SCOPED_TRACE(cases[k].file);
        const std::string path = writeSmallFile(dir, cases[k].file, cases[k].type, { cases[k].channel });
        ASSERT_FALSE(path.empty());
        Decoded ref;
        ASSERT_TRUE(readWithOiio(path, &ref));
        ASSERT_EQ(1, ref.spec.nchannels);

        NodePtr node = createRead();
        setFile(node, path);
        ImageLayerDesc layer;
        ImageLayerDesc paired;
        node->getEffectInstance()->getMetadataComponents(-1, &layer, &paired);
        EXPECT_EQ(3u, layer.getChannels().size());

        std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBComponents());
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kSmall, kSmall), layers, &planes, &error)) << error;
        ASSERT_EQ(1u, planes.size());
        ASSERT_EQ((std::size_t)kSmall * kSmall * 3u, planes[0].pixels.size());
        std::size_t mismatches = 0;
        for (int row = 0; row < kSmall; ++row) {
            for (int col = 0; col < kSmall; ++col) {
                const float want = ref.pixels[(std::size_t)(kSmall - 1 - row) * kSmall + col];
                for (int c = 0; c < 3; ++c) {
                    const float got = planes[0].pixels[((std::size_t)row * kSmall + col) * 3 + c];
                    if (got != want && ++mismatches <= 5) {
                        ADD_FAILURE() << "pixel " << col << "," << row << " channel " << c << ": got " << got << ", want " << want;
                    }
                }
            }
        }
        EXPECT_EQ(0u, mismatches);
    }
}

TEST_F(NativeReadDecodeTest, ALuminanceAndAlphaFileGivesRgbaWithTheAlphaIntact)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = writeSmallFile(dir, "grey-alpha.png", OIIO::TypeDesc::UINT8, { "Y", "A" });
    ASSERT_FALSE(path.empty());
    Decoded ref;
    ASSERT_TRUE(readWithOiio(path, &ref));
    ASSERT_EQ(2, ref.spec.nchannels);

    NodePtr node = createRead();
    setFile(node, path);
    ImageLayerDesc layer;
    ImageLayerDesc paired;
    node->getEffectInstance()->getMetadataComponents(-1, &layer, &paired);
    EXPECT_EQ(4u, layer.getChannels().size());

    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kSmall, kSmall), layers, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    ASSERT_EQ((std::size_t)kSmall * kSmall * 4u, planes[0].pixels.size());
    std::size_t mismatches = 0;
    for (int row = 0; row < kSmall; ++row) {
        for (int col = 0; col < kSmall; ++col) {
            const std::size_t fileIndex = ((std::size_t)(kSmall - 1 - row) * kSmall + col) * 2;
            const float want[4] = { ref.pixels[fileIndex], ref.pixels[fileIndex], ref.pixels[fileIndex], ref.pixels[fileIndex + 1] };
            for (int c = 0; c < 4; ++c) {
                const float got = planes[0].pixels[((std::size_t)row * kSmall + col) * 4 + c];
                if (got != want[c] && ++mismatches <= 5) {
                    ADD_FAILURE() << "pixel " << col << "," << row << " channel " << c << ": got " << got << ", want " << want[c];
                }
            }
        }
    }
    EXPECT_EQ(0u, mismatches);
}

TEST_F(NativeReadDecodeTest, AnUnreadableFileSetsAPersistentMessageAndALaterGoodFrameClearsIt)
{
    NodePtr node = createRead();
    const RectI roi(0, 0, 8, 8);
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBComponents());
    std::vector<RenderedPlane> planes;
    std::string error;

    setFile(node, std::string("/nonexistent/native-read-missing.png"));
    EXPECT_FALSE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, roi, layers, &planes, &error));
    EXPECT_FALSE(persistentMessage(node).empty());

    setFile(node, std::string(NATRON_TESTS_FIXTURES_DIR "/png-8bit.png"));
    EXPECT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, roi, layers, &planes, &error)) << error;
    EXPECT_EQ(std::string(), persistentMessage(node));
}
