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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>

#include "Engine/FrameEntrySerialization.h"
#include "Engine/FrameKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Lut.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/TextureRect.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kStudioURI = "ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5";
const char* const kWorkingSpace = "ACEScg";
const char* const kDisplay = "sRGB - Display";
const char* const kSDRView = "ACES 2.0 - SDR 100 nits (Rec.709)";

FrameKey
makeKey(U64 displayTransformHash,
        bool useShaders)
{
    std::vector<std::string> channels;
    channels.push_back("R");
    channels.push_back("G");
    channels.push_back("B");
    channels.push_back("A");
    ImageLayerDesc layer("Color", "Color", "RGBA", channels);

    return FrameKey(NULL,
                    10,
                    1234,
                    1.5,
                    0.75,
                    displayTransformHash,
                    useShaders ? 32 : 8,
                    4,
                    ViewIdx(0),
                    TextureRect(0, 0, 64, 64, 1, 1.),
                    0,
                    "Source",
                    layer,
                    "Color.A",
                    useShaders,
                    false);
}

class ViewerDisplayTransformOCIO
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        std::string error;
        ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, _cm.load(kStudioURI, std::string(), &error)) << error;
        _cm.setWorkingSpace(kWorkingSpace);
    }

    ProjectColorManagement::DisplayProcessorPtr processor(const std::string& view,
                                                          const std::string& look = std::string())
    {
        std::string error;
        ProjectColorManagement::DisplayProcessorPtr p = _cm.getDisplayProcessor(kWorkingSpace, kDisplay, view, look, &error);
        EXPECT_TRUE(p != nullptr) << error;

        return p;
    }

    ProjectColorManagement _cm;
};

void
fillGray(std::vector<float>* rgba,
         float value,
         float alpha)
{
    for (std::size_t i = 0; i < rgba->size(); i += 4) {
        (*rgba)[i] = value;
        (*rgba)[i + 1] = value;
        (*rgba)[i + 2] = value;
        (*rgba)[i + 3] = alpha;
    }
}

int
displayCodeValue(const ProjectColorManagement::DisplayProcessor& p,
                 float linear,
                 ViewerDisplayOutputEnum output = eViewerDisplayOutputEightBit)
{
    float rgba[4] = { linear, linear, linear, 1.f };
    applyViewerDisplayTransform(p, rgba, 1, 1., 0., 1., output);

    return Color::floatToInt<256>(rgba[1]);
}

std::vector<float>
eightBitAccuracyColors()
{
    std::vector<float> rgba;
    for (int i = 0; i <= 200; ++i) {
        const float v = std::exp2(-12.f + 20.f * i / 200.f);
        rgba.insert(rgba.end(), { v, v, v, 1.f });
    }
    for (int r = 0; r < 17; ++r) {
        for (int g = 0; g < 17; ++g) {
            for (int b = 0; b < 17; ++b) {
                rgba.insert(rgba.end(), { std::exp2(-9.f + 13.f * r / 16.f), std::exp2(-9.f + 13.f * g / 16.f), std::exp2(-9.f + 13.f * b / 16.f), 1.f });
            }
        }
    }
    const float outOfRange[] = { -1.f, -0.01f, 0.f, 1e-4f, 0.18f, 1.f, 16.f, 300.f, 1e5f };
    for (float a : outOfRange) {
        for (float b : outOfRange) {
            rgba.insert(rgba.end(), { a, b, 0.18f, 1.f });
        }
    }

    return rgba;
}

} // namespace

TEST_F(ViewerDisplayTransformOCIO, SDRViewMatchesTheMeasuredCodeValue)
{
    ProjectColorManagement::DisplayProcessorPtr p = processor(kSDRView);
    ASSERT_TRUE(p && p->cpu);
    EXPECT_NEAR(89, displayCodeValue(*p, 0.18f), 1);
    EXPECT_NEAR(89, displayCodeValue(*p, 0.18f, eViewerDisplayOutputExact), 1);
}

TEST_F(ViewerDisplayTransformOCIO, UnToneMappedAndRawViews)
{
    ProjectColorManagement::DisplayProcessorPtr untoned = processor("Un-tone-mapped");
    ProjectColorManagement::DisplayProcessorPtr raw = processor("Raw");
    ASSERT_TRUE(untoned && raw);
    EXPECT_NEAR(118, displayCodeValue(*untoned, 0.18f), 1);
    EXPECT_NEAR(46, displayCodeValue(*raw, 0.18f), 1);
}

TEST_F(ViewerDisplayTransformOCIO, EightBitOutputStaysWithinOneCodeValue)
{
    const char* const views[] = { kSDRView, "Un-tone-mapped", "Raw" };
    for (const char* view : views) {
        ProjectColorManagement::DisplayProcessorPtr p = processor(view);
        ASSERT_TRUE(p && p->cpu);

        const std::vector<float> input = eightBitAccuracyColors();
        std::vector<float> exact = input;
        std::vector<float> eightBit = input;
        const int width = (int)input.size() / 4;
        applyViewerDisplayTransform(*p, &exact[0], width, 1., 0., 1., eViewerDisplayOutputExact);
        applyViewerDisplayTransform(*p, &eightBit[0], width, 1., 0., 1., eViewerDisplayOutputEightBit);
        int worst = 0;
        for (std::size_t i = 0; i < input.size(); ++i) {
            if (i % 4 == 3) {
                EXPECT_EQ(input[i], eightBit[i]);
                continue;
            }
            const int diff = std::abs(Color::floatToInt<256>(exact[i]) - Color::floatToInt<256>(eightBit[i]));
            EXPECT_LE(diff, 1) << view << " input (" << input[i / 4 * 4] << ", " << input[i / 4 * 4 + 1] << ", " << input[i / 4 * 4 + 2] << ")";
            worst = std::max(worst, diff);
        }
        std::cout << "[ViewerDisplayTransform] 8-bit output through " << view << " differs by at most " << worst << " code value" << std::endl;

        float gammaExact[8] = { 0.18f, 0.18f, 0.18f, 1.f, 0.02f, 0.5f, 3.f, 1.f };
        float gammaEightBit[8] = { 0.18f, 0.18f, 0.18f, 1.f, 0.02f, 0.5f, 3.f, 1.f };
        applyViewerDisplayTransform(*p, gammaExact, 2, 1., 0., 2.2, eViewerDisplayOutputExact);
        applyViewerDisplayTransform(*p, gammaEightBit, 2, 1., 0., 2.2, eViewerDisplayOutputEightBit);
        for (int i = 0; i < 8; ++i) {
            EXPECT_EQ(gammaExact[i], gammaEightBit[i]) << view << " gamma 2.2, index " << i;
        }
    }
}

TEST_F(ViewerDisplayTransformOCIO, ACustomConfigFileAlwaysRunsTheExactProcessorForEightBitOutput)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QString::fromUtf8("/studio-copy.ocio")).toStdString();
    {
        std::ofstream out(path.c_str());
        _cm.getConfig()->serialize(out);
        ASSERT_TRUE(out.good());
    }

    ProjectColorManagement custom;
    std::string error;
    ASSERT_EQ(ProjectColorManagement::eLoadErrorNone, custom.load(path, std::string(), &error)) << error;
    custom.setWorkingSpace(kWorkingSpace);

    ProjectColorManagement::DisplayProcessorPtr builtin = processor(kSDRView);
    ProjectColorManagement::DisplayProcessorPtr fromFile = custom.getDisplayProcessor(kWorkingSpace, kDisplay, kSDRView, std::string(), &error);
    ASSERT_TRUE(builtin && builtin->cpu);
    ASSERT_TRUE(fromFile && fromFile->cpu) << error;
    EXPECT_TRUE(builtin->eightBitLutAllowed);
    EXPECT_FALSE(fromFile->eightBitLutAllowed);

    const std::vector<float> input = eightBitAccuracyColors();
    std::vector<float> exact = input;
    std::vector<float> eightBit = input;
    const int width = (int)input.size() / 4;
    applyViewerDisplayTransform(*fromFile, &exact[0], width, 1., 0., 1., eViewerDisplayOutputExact);
    applyViewerDisplayTransform(*fromFile, &eightBit[0], width, 1., 0., 1., eViewerDisplayOutputEightBit);
    for (std::size_t i = 0; i < input.size(); ++i) {
        ASSERT_EQ(exact[i], eightBit[i]) << "index " << i;
    }
}

TEST_F(ViewerDisplayTransformOCIO, GainTwoEqualsOneStopOfExposure)
{
    ProjectColorManagement::DisplayProcessorPtr p = processor(kSDRView);
    ASSERT_TRUE(p != nullptr);

    OCIO_NAMESPACE::ConstConfigRcPtr config = _cm.getConfig();
    OCIO_NAMESPACE::DisplayViewTransformRcPtr displayView = OCIO_NAMESPACE::DisplayViewTransform::Create();
    displayView->setSrc(kWorkingSpace);
    displayView->setDisplay(kDisplay);
    displayView->setView(kSDRView);
    OCIO_NAMESPACE::ExposureContrastTransformRcPtr exposure = OCIO_NAMESPACE::ExposureContrastTransform::Create();
    exposure->setStyle(OCIO_NAMESPACE::EXPOSURE_CONTRAST_LINEAR);
    exposure->setExposure(1.);
    OCIO_NAMESPACE::LegacyViewingPipelineRcPtr pipeline = OCIO_NAMESPACE::LegacyViewingPipeline::Create();
    pipeline->setDisplayViewTransform(displayView);
    pipeline->setLinearCC(exposure);
    OCIO_NAMESPACE::ConstCPUProcessorRcPtr reference = pipeline->getProcessor(config)->getDefaultCPUProcessor();

    const float inputs[3] = { 0.05f, 0.18f, 0.6f };
    for (int i = 0; i < 3; ++i) {
        float expected[4] = { inputs[i], inputs[i], inputs[i], 1.f };
        reference->applyRGBA(expected);
        float actual[4] = { inputs[i], inputs[i], inputs[i], 1.f };
        applyViewerDisplayTransform(*p, actual, 1, 2., 0., 1.);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(expected[c], actual[c], 1e-4) << "input " << inputs[i] << " channel " << c;
        }
    }
}

TEST_F(ViewerDisplayTransformOCIO, OffsetIsAddedBeforeTheProcessor)
{
    ProjectColorManagement::DisplayProcessorPtr p = processor(kSDRView);
    ASSERT_TRUE(p != nullptr);

    float withOffset[4] = { 0.18f, 0.18f, 0.18f, 1.f };
    applyViewerDisplayTransform(*p, withOffset, 1, 1., 0.1, 1.);
    float shifted[4] = { 0.28f, 0.28f, 0.28f, 1.f };
    applyViewerDisplayTransform(*p, shifted, 1, 1., 0., 1.);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(shifted[c], withOffset[c], 1e-5);
    }

    float unshifted[4] = { 0.18f, 0.18f, 0.18f, 1.f };
    applyViewerDisplayTransform(*p, unshifted, 1, 1., 0., 1.);
    EXPECT_GT(withOffset[0], unshifted[0] + 1e-3);
}

TEST_F(ViewerDisplayTransformOCIO, GammaIsAppliedAfterTheProcessorAndThresholdsAtZero)
{
    ProjectColorManagement::DisplayProcessorPtr raw = processor("Raw");
    ProjectColorManagement::DisplayProcessorPtr sdr = processor(kSDRView);
    ASSERT_TRUE(raw && sdr);

    float rgba[12] = { 0.5f, 0.99f, 1.f, 0.25f,
                       1.f, 2.f, 0.f, 0.75f,
                       -1.f, 0.f, 1.5f, 1.f };
    applyViewerDisplayTransform(*raw, rgba, 3, 1., 0., 0.);
    const float expectedThreshold[12] = { 0.f, 0.f, 1.f, 0.25f,
                                          1.f, 1.f, 0.f, 0.75f,
                                          0.f, 0.f, 1.f, 1.f };
    for (int i = 0; i < 12; ++i) {
        EXPECT_EQ(expectedThreshold[i], rgba[i]) << "index " << i;
    }

    float squared[4] = { 0.25f, 0.25f, 0.25f, 0.5f };
    applyViewerDisplayTransform(*raw, squared, 1, 1., 0., 2.);
    EXPECT_NEAR(0.5f, squared[0], 1e-5);
    EXPECT_EQ(0.5f, squared[3]);

    float display[4] = { 0.18f, 0.18f, 0.18f, 1.f };
    applyViewerDisplayTransform(*sdr, display, 1, 1., 0., 1.);
    float withGamma[4] = { 0.18f, 0.18f, 0.18f, 1.f };
    applyViewerDisplayTransform(*sdr, withGamma, 1, 1., 0., 2.2);
    EXPECT_NEAR(std::pow(display[0], 1.f / 2.2f), withGamma[0], 1e-5);
}

TEST_F(ViewerDisplayTransformOCIO, AlphaIsUntouched)
{
    ProjectColorManagement::DisplayProcessorPtr p = processor(kSDRView);
    ASSERT_TRUE(p != nullptr);

    float rgba[8] = { 0.18f, 0.18f, 0.18f, 0.3f,
                      4.f, 0.01f, 0.5f, 0.f };
    applyViewerDisplayTransform(*p, rgba, 2, 3., 0.2, 1.5);
    EXPECT_EQ(0.3f, rgba[3]);
    EXPECT_EQ(0.f, rgba[7]);
}

TEST_F(ViewerDisplayTransformOCIO, DifferentViewsGiveDifferentFrameKeys)
{
    ProjectColorManagement::DisplayProcessorPtr sdr = processor(kSDRView);
    ProjectColorManagement::DisplayProcessorPtr untoned = processor("Un-tone-mapped");
    ProjectColorManagement::DisplayProcessorPtr sdrAgain = processor(kSDRView);
    ASSERT_TRUE(sdr && untoned && sdrAgain);

    EXPECT_NE(sdr->cacheHash, untoned->cacheHash);
    EXPECT_EQ(sdr->cacheHash, sdrAgain->cacheHash);
    EXPECT_NE(makeKey(sdr->cacheHash, false).getHash(), makeKey(untoned->cacheHash, false).getHash());

    ProjectColorManagement::DisplayProcessorPtr withLook = processor(kSDRView, "ACES 1.3 Reference Gamut Compression");
    ASSERT_TRUE(withLook != nullptr);
    EXPECT_NE(sdr->cacheHash, withLook->cacheHash);
}

TEST_F(ViewerDisplayTransformOCIO, FullHDFrameTiming)
{
    ProjectColorManagement::DisplayProcessorPtr p = processor(kSDRView);
    ASSERT_TRUE(p != nullptr);

    const int width = 1920;
    const int height = 1080;
    std::vector<float> frame(4 * width * height);
    fillGray(&frame, 0.18f, 1.f);

    const int nThreads = std::max(1, (int)std::thread::hardware_concurrency());
    const auto timeFrame = [&](ViewerDisplayOutputEnum output) {
        fillGray(&frame, 0.18f, 1.f);
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < nThreads; ++t) {
            threads.push_back(std::thread([&, t]() {
                for (int y = t; y < height; y += nThreads) {
                    applyViewerDisplayTransform(*p, &frame[4 * width * y], width, 1., 0., 1., output);
                }
            }));
        }
        for (std::size_t t = 0; t < threads.size(); ++t) {
            threads[t].join();
        }

        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };

    const double exactMs = timeFrame(eViewerDisplayOutputExact);
    EXPECT_NEAR(89, Color::floatToInt<256>(frame[4 * (width * (height / 2) + width / 2)]), 1);

    float warmUp[4] = { 0.18f, 0.18f, 0.18f, 1.f };
    const std::chrono::steady_clock::time_point bakeStart = std::chrono::steady_clock::now();
    applyViewerDisplayTransform(*p, warmUp, 1, 1., 0., 1., eViewerDisplayOutputEightBit);
    const double bakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bakeStart).count();

    const double ms = timeFrame(eViewerDisplayOutputEightBit);
    EXPECT_NEAR(89, Color::floatToInt<256>(frame[4 * (width * (height / 2) + width / 2)]), 1);

    std::cout << "[ViewerDisplayTransform] 1920x1080 through " << kSDRView << " on " << nThreads
              << " threads: " << ms << " ms for 8-bit output (" << exactMs << " ms exact, "
              << bakeMs << " ms to build the 8-bit LUT)" << std::endl;
    RecordProperty("FullHDFrameMilliseconds", (int)ms);
    RecordProperty("FullHDFrameExactMilliseconds", (int)exactMs);
}

TEST(ViewerDisplayTransform, FrameKeyHashDependsOnTransformWithoutShaders)
{
    FrameKey a = makeKey(0x1111, false);
    FrameKey b = makeKey(0x2222, false);
    FrameKey c = makeKey(0x1111, false);

    EXPECT_NE(a.getHash(), b.getHash());
    EXPECT_FALSE(a == b);
    EXPECT_EQ(a.getHash(), c.getHash());
    EXPECT_TRUE(a == c);
}

TEST(ViewerDisplayTransform, FrameKeyHashIgnoresTransformWithShaders)
{
    FrameKey a = makeKey(0x1111, true);
    FrameKey b = makeKey(0x2222, true);

    EXPECT_EQ(a.getHash(), b.getHash());
    EXPECT_TRUE(a == b);
}

TEST(ViewerDisplayTransform, FrameKeyRoundTripsThroughSerialization)
{
    FrameKey original = makeKey(0x123456789abcdef0ULL, false);

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    {
        boost::archive::binary_oarchive oArchive(stream);
        oArchive << original;
    }

    FrameKey restored;
    {
        boost::archive::binary_iarchive iArchive(stream);
        iArchive >> restored;
    }

    EXPECT_EQ(original.getDisplayTransformHash(), restored.getDisplayTransformHash());
    EXPECT_TRUE(original == restored);
    EXPECT_EQ(original.getHash(), restored.getHash());
}

TEST(ViewerDisplayTransform, AFrameKeyFromBeforeTheDisplayTransformHashIsRejected)
{
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    {
        boost::archive::binary_oarchive oArchive(stream);
        const int time = 10;
        const U64 treeVersion = 1234;
        const double gain = 1.5;
        const double gamma = 0.75;
        const int lut = 2;
        const int bitDepth = 1;
        oArchive << time << treeVersion << gain << gamma << lut << bitDepth;
    }

    FrameKey restored;
    boost::archive::binary_iarchive iArchive(stream);
    EXPECT_THROW(restored.serialize(iArchive, FRAME_KEY_INTRODUCES_CACHE_HOLDER_ID), std::runtime_error);
    EXPECT_EQ(0U, restored.getDisplayTransformHash());
}
