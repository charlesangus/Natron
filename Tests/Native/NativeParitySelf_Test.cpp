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

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <list>
#include <string>
#include <vector>

#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include "BaseTest.h"
#include "NativeParity.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kColorMatrixID = "net.sf.openfx.ColorMatrixPlugin";
const int kOfxColorMatrixMajor = 2;

const ImageLayerDesc*
findColorLayer(const std::list<ImageLayerDesc>& layers)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->isColorLayer()) {
            return &*it;
        }
    }

    return 0;
}

const ImageLayerDesc*
findLayer(const std::list<ImageLayerDesc>& layers,
          const std::string& id)
{
    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->getLayerID() == id) {
            return &*it;
        }
    }

    return 0;
}

float
planeValue(const RenderedPlane& plane,
           int x,
           int y,
           int c)
{
    const std::size_t nComps = plane.channels.size();
    const std::size_t index = (static_cast<std::size_t>(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * nComps + c;

    return plane.pixels[index];
}

} // namespace

class NativeParitySelfTest
    : public BaseTest {
protected:
    // Both sides are the OFX ColorMatrix at its own major, so every difference the harness
    // reports is one the test put there.
    ParityPair makeSelfPair()
    {
        return makeParityPair(getApp(), kColorMatrixID, kOfxColorMatrixMajor, kOfxColorMatrixMajor);
    }
};

TEST_F(NativeParitySelfTest, ParitySourceRendersEveryPlaneDeterministically)
{
    NodePtr source = createNode(QString::fromUtf8(kTestPluginIDParitySource));
    ASSERT_TRUE(bool(source));
    setParitySourceExtraLayer(source, true);
    setParitySourceOrigin(source, 3, -2);

    const double time = 1.;
    std::list<ImageLayerDesc> layers;
    source->getEffectInstance()->getPresentLayers(time, ViewIdx(0), -1, &layers);
    const ImageLayerDesc* color = findColorLayer(layers);
    const ImageLayerDesc* spec = findLayer(layers, kParitySourceExtraLayerID);
    ASSERT_TRUE(color != 0);
    ASSERT_TRUE(spec != 0);
    EXPECT_EQ(4, color->getNumComponents());
    EXPECT_EQ(3, spec->getNumComponents());

    std::list<ImageLayerDesc> request;
    request.push_back(*color);
    request.push_back(*spec);

    const RectI window0 = paritySourceWindow(source, time, 0);
    EXPECT_EQ(RectI(3, -2, 3 + kParitySourceWidth, -2 + kParitySourceHeight), window0);
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(source, time, ViewIdx(0), 0, window0, request, &planes, &error)) << error;
    ASSERT_EQ(2u, planes.size());
    const int samples[3][2] = { { 0, 0 }, { 17, 5 }, { 63, 47 } };
    for (int s = 0; s < 3; ++s) {
        const int cx = samples[s][0];
        const int cy = samples[s][1];
        for (int c = 0; c < 4; ++c) {
            EXPECT_EQ(paritySourceColorValue(c, cx, cy, time), planeValue(planes[0], 3 + cx, -2 + cy, c)) << "colour c=" << c << " at " << cx << "," << cy;
        }
        for (int c = 0; c < 3; ++c) {
            EXPECT_EQ(paritySourceExtraValue(c, cx, cy, time), planeValue(planes[1], 3 + cx, -2 + cy, c)) << "spec c=" << c << " at " << cx << "," << cy;
        }
    }

    const RectI window1 = paritySourceWindow(source, time, 1);
    ASSERT_TRUE(renderNodePlanesDirect(source, time, ViewIdx(0), 1, window1, request, &planes, &error)) << error;
    ASSERT_EQ(2u, planes.size());
    const int x1 = window1.x1 + 4;
    const int y1 = window1.y1 + 3;
    EXPECT_EQ(paritySourceColorValue(0, x1 * 2 + 1 - 3, y1 * 2 + 1 + 2, time), planeValue(planes[0], x1, y1, 0));

    setParitySourceComponents(source, "alpha");
    layers.clear();
    source->getEffectInstance()->getPresentLayers(time, ViewIdx(0), -1, &layers);
    color = findColorLayer(layers);
    ASSERT_TRUE(color != 0);
    ASSERT_EQ(1, color->getNumComponents());
    request.clear();
    request.push_back(*color);
    ASSERT_TRUE(renderNodePlanesDirect(source, time, ViewIdx(0), 0, window0, request, &planes, &error)) << error;
    ASSERT_EQ(1u, planes.size());
    for (int cy = 0; cy < 12; ++cy) {
        EXPECT_EQ(paritySourceColorValue(3, 10, cy, time), planeValue(planes[0], 3 + 10, -2 + cy, 0)) << "alpha row " << cy;
    }
    EXPECT_EQ(0.f, paritySourceColorValue(3, 10, 0, time));
    EXPECT_EQ(1.f, paritySourceColorValue(3, 10, 4, time));
}

TEST_F(NativeParitySelfTest, OfxColorMatrixAgainstItselfMatchesExactly)
{
    ParityPair pair = makeSelfPair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live()) << "the OFX ColorMatrix must be loadable at major " << kOfxColorMatrixMajor;
    ASSERT_TRUE(setKnobOnBoth(pair, "outputRed", { 1.5, 0., 0., 0. }));

    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const ParityResult r = compareParity(pair, "self", RectI(), mipmapLevel, ParityTolerance::exact(), false);
        EXPECT_TRUE(r.ok) << "mipmap " << mipmapLevel << ": " << describe(r);
        EXPECT_TRUE(r.live);
        EXPECT_GE(r.planesCompared, 1);
        EXPECT_EQ(0., r.maxAbsDiff);
    }

    EXPECT_TRUE(knobParityProblems(pair.ofx, pair.native).empty());
    expectKnobParity(pair.ofx, pair.native);
}

TEST_F(NativeParitySelfTest, AKnobChangedOnOneSideIsReported)
{
    ParityPair pair = makeSelfPair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobValues(pair.native, "outputGreen", { 0., 0.5, 0., 0. }));

    const ParityResult r = compareParity(pair, "green-one-side", RectI(), 0, ParityTolerance::transcendental(), false);
    EXPECT_FALSE(r.ok) << describe(r);
    EXPECT_FALSE(r.plane.empty());
    EXPECT_FALSE(r.channel.empty());
    EXPECT_NE(r.reference, r.native);
    EXPECT_GT(r.maxAbsDiff, 0.);
}

TEST_F(NativeParitySelfTest, RecordedReferencesReplayExactly)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string dir = tmp.path().toStdString();

    ParityPair pair = makeSelfPair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, "outputRed", { 1.5, 0., 0., 0. }));
    setParitySourceExtraLayer(pair.source, true);

    ParityOptions recordOptions;
    recordOptions.recordDir = dir;
    ParityOptions replayOptions;
    replayOptions.referenceDir = dir;
    replayOptions.forceReplay = true;

    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const ParityResult recorded = compareParity(pair, "recorded", RectI(), mipmapLevel, ParityTolerance::exact(), true, recordOptions);
        ASSERT_TRUE(recorded.ok) << describe(recorded);
        EXPECT_GE(recorded.planesCompared, 2) << "the extra layer must be recorded too";
        EXPECT_TRUE(QFileInfo::exists(QString::fromStdString(parityReferencePath(dir, kColorMatrixID, "recorded", mipmapLevel, ImageLayerDesc::getRGBAComponents()))));
        EXPECT_TRUE(QFileInfo::exists(QString::fromStdString(parityReferencePath(dir, kColorMatrixID, "recorded", mipmapLevel, paritySourceExtraLayer()))));

        const ParityResult replayed = compareParity(pair, "recorded", RectI(), mipmapLevel, ParityTolerance::exact(), false, replayOptions);
        EXPECT_TRUE(replayed.ok) << "mipmap " << mipmapLevel << ": " << describe(replayed);
        EXPECT_FALSE(replayed.live);
        EXPECT_EQ(recorded.planesCompared, replayed.planesCompared);
        EXPECT_EQ(0., replayed.maxAbsDiff);
    }

    ASSERT_TRUE(setKnobValues(pair.native, "outputGreen", { 0., 0.5, 0., 0. }));
    const ParityResult diverged = compareParity(pair, "recorded", RectI(), 0, ParityTolerance::exact(), false, replayOptions);
    EXPECT_FALSE(diverged.ok) << describe(diverged);
    EXPECT_FALSE(diverged.live);
}

TEST_F(NativeParitySelfTest, ReplayWithoutAReferenceFailsTheTest)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ParityPair pair = makeSelfPair();
    ASSERT_TRUE(bool(pair.native));

    ParityOptions options;
    options.referenceDir = tmp.path().toStdString();
    options.forceReplay = true;

    ::testing::TestPartResultArray failures;
    ParityResult r;
    {
        ::testing::ScopedFakeTestPartResultReporter reporter(::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD, &failures);
        r = compareParity(pair, "never-recorded", RectI(), 0, ParityTolerance::exact(), false, options);
    }
    EXPECT_FALSE(r.ok);
    ASSERT_EQ(1, failures.size());
    EXPECT_TRUE(failures.GetTestPartResult(0).nonfatally_failed());
    EXPECT_NE(std::string::npos, std::string(failures.GetTestPartResult(0).message()).find("never-recorded")) << failures.GetTestPartResult(0).message();
}

TEST_F(NativeParitySelfTest, F32RoundTripIsBitExact)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = tmp.path().toStdString() + "/some.id/case.f32";

    ParityF32Image image;
    image.width = 3;
    image.height = 2;
    image.nComps = 4;
    for (int i = 0; i < image.width * image.height * image.nComps; ++i) {
        image.pixels.push_back(std::sin(0.37f * i) * 3.f - 1.f);
    }
    image.pixels[0] = -0.f;
    image.pixels[1] = std::numeric_limits<float>::quiet_NaN();
    image.pixels[2] = std::numeric_limits<float>::infinity();
    image.pixels[3] = std::numeric_limits<float>::denorm_min();

    std::string error;
    ASSERT_TRUE(writeParityF32(path, image, &error)) << error;
    ParityF32Image back;
    ASSERT_TRUE(readParityF32(path, &back, &error)) << error;
    EXPECT_EQ(image.width, back.width);
    EXPECT_EQ(image.height, back.height);
    EXPECT_EQ(image.nComps, back.nComps);
    ASSERT_EQ(image.pixels.size(), back.pixels.size());
    EXPECT_EQ(0, std::memcmp(image.pixels.data(), back.pixels.data(), image.pixels.size() * sizeof(float)));

    {
        std::ifstream in(path, std::ios::binary);
        char magic[4];
        in.read(magic, 4);
        EXPECT_EQ(0, std::memcmp(magic, "NPAR", 4));
    }

    const std::string truncated = tmp.path().toStdString() + "/truncated.f32";
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::ofstream out(truncated, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() - 4));
    }
    EXPECT_FALSE(readParityF32(truncated, &back, &error));

    const std::string notParity = tmp.path().toStdString() + "/bad-magic.f32";
    {
        std::ofstream out(notParity, std::ios::binary);
        out.write("XPAR", 4);
        const std::int32_t header[3] = { 1, 1, 1 };
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        const float v = 1.f;
        out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    EXPECT_FALSE(readParityF32(notParity, &back, &error));
}
