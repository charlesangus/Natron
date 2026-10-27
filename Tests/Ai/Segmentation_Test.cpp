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

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/ModelPackage.h"
#include "Engine/Ai/Segmentation.h"
#include "Tests/Ai/AiTestSupport.h"

NATRON_NAMESPACE_USING

namespace {

struct Disc {
    double cx, cy, radius;
};

bool
inDisc(const Disc& d,
       int x,
       int y)
{
    const double dx = x + 0.5 - d.cx;
    const double dy = y + 0.5 - d.cy;

    return dx * dx + dy * dy <= d.radius * d.radius;
}

// White discs on black, packed RGB, top row first.
std::vector<uint8_t>
discImage(int width,
          int height,
          const std::vector<Disc>& discs)
{
    std::vector<uint8_t> rgb((size_t)width * height * 3, 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (const Disc& d : discs) {
                if (inDisc(d, x, y)) {
                    uint8_t* p = &rgb[((size_t)y * width + x) * 3];
                    p[0] = p[1] = p[2] = 255;
                }
            }
        }
    }

    return rgb;
}

double
iouWithDisc(const std::vector<float>& mask,
            int width,
            int height,
            const Disc& disc,
            float threshold = 0.5f)
{
    std::size_t intersection = 0, unionCount = 0;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool a = mask[(size_t)y * width + x] > threshold;
            const bool b = inDisc(disc, x, y);
            intersection += (a && b) ? 1 : 0;
            unionCount += (a || b) ? 1 : 0;
        }
    }

    return unionCount ? (double)intersection / (double)unionCount : 0.;
}

ModelConfig
fakeConfig(const char* arch)
{
    ModelConfig config;

    config.id = std::string("test-") + arch;
    config.task = "mask";
    config.arch = arch;
    config.attributesJson = "{\"input_size\":64}";

    return config;
}

std::unique_ptr<Segmenter>
fakeSegNext()
{
    return std::unique_ptr<Segmenter>(new Segmenter(AiTest::modelPath("seg-segnext-encoder.onnx"),
                                                    AiTest::modelPath("seg-segnext-decoder.onnx"),
                                                    fakeConfig("segnext"), AiTest::cpuOptions(), AiTest::loader()));
}

std::unique_ptr<Segmenter>
fakeSam()
{
    return std::unique_ptr<Segmenter>(new Segmenter(AiTest::modelPath("seg-sam-encoder.onnx"),
                                                    AiTest::modelPath("seg-sam-decoder.onnx"),
                                                    fakeConfig("sam2"), AiTest::cpuOptions(), AiTest::loader()));
}

SegmentationKey
key(uint64_t hash,
    double frame = 1.)
{
    SegmentationKey k;

    k.imageHash = hash;
    k.frame = frame;
    k.modelId = "test";

    return k;
}

SegmentationPoint
point(float x,
      float y,
      int label = SegmentationPoint::eLabelPositive)
{
    SegmentationPoint p;

    p.x = x;
    p.y = y;
    p.label = label;

    return p;
}

class AiSegmentation
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(AiTest::loader()->available()) << AiTest::loader()->unavailableReason();
    }
};

} // namespace

TEST(AiSegmentationConfig, ArchitectureComesFromConfig)
{
    EXPECT_EQ(Segmenter::Arch::eSegNext, Segmenter::archFromConfig(fakeConfig("segnext")));
    EXPECT_EQ(Segmenter::Arch::eSam, Segmenter::archFromConfig(fakeConfig("sam2")));
    EXPECT_EQ(Segmenter::Arch::eSam, Segmenter::archFromConfig(fakeConfig("sam")));
    EXPECT_THROW(Segmenter::archFromConfig(fakeConfig("nafnet")), AiError);
}

TEST_F(AiSegmentation, SegNextMaskMatchesTheObject)
{
    std::unique_ptr<Segmenter> seg = fakeSegNext();
    ASSERT_TRUE(bool(seg));
    EXPECT_FALSE(seg->supportsBoxes());
    EXPECT_EQ(64, seg->inputSize());

    const Disc disc = { 30., 26., 14. };
    const std::vector<uint8_t> image = discImage(64, 64, { disc });
    EXPECT_FALSE(seg->encode(image.data(), 64, 64, key(1)));
    ASSERT_EQ(64, seg->encodedWidth());
    ASSERT_EQ(64, seg->encodedHeight());

    const std::vector<float> mask = seg->segment({ point(30.f, 26.f) }, {}, 0.5f);
    ASSERT_EQ((size_t)64 * 64, mask.size());
    EXPECT_GT(iouWithDisc(mask, 64, 64, disc), 0.95);
}

TEST_F(AiSegmentation, NonSquareImagesAreCroppedBackFromThePadding)
{
    std::unique_ptr<Segmenter> seg = fakeSegNext();

    // Wider than tall: the encoder sees it top-left in a padded square, at scale 1.
    const Disc disc = { 40., 18., 12. };
    const std::vector<uint8_t> image = discImage(64, 40, { disc });
    seg->encode(image.data(), 64, 40, key(1));
    const std::vector<float> mask = seg->segment({ point(40.f, 18.f) }, {}, 0.5f);
    ASSERT_EQ((size_t)64 * 40, mask.size());
    EXPECT_GT(iouWithDisc(mask, 64, 40, disc), 0.95);

    // Larger than the input size: downscaled to it, and the mask upscaled back.
    const Disc big = { 60., 40., 24. };
    const std::vector<uint8_t> large = discImage(96, 72, { big });
    seg->encode(large.data(), 96, 72, key(2));
    const std::vector<float> largeMask = seg->segment({ point(60.f, 40.f) }, {}, 0.5f);
    ASSERT_EQ((size_t)96 * 72, largeMask.size());
    EXPECT_GT(iouWithDisc(largeMask, 96, 72, big), 0.9);
}

TEST_F(AiSegmentation, EncodingsAreCachedByKey)
{
    std::unique_ptr<Segmenter> seg = fakeSegNext();
    const std::vector<uint8_t> image = discImage(64, 64, { { 30., 26., 14. } });

    EXPECT_FALSE(seg->encode(image.data(), 64, 64, key(1)));
    EXPECT_EQ(1u, seg->encoderRuns());
    EXPECT_EQ(0u, seg->cacheHits());

    // Moving the prompts or returning to an image leaves the encoder alone.
    EXPECT_TRUE(seg->encode(image.data(), 64, 64, key(1)));
    seg->segment({ point(30.f, 26.f) }, {}, 0.5f);
    seg->segment({ point(25.f, 20.f), point(2.f, 2.f, SegmentationPoint::eLabelNegative) }, {}, 0.5f);
    EXPECT_EQ(1u, seg->encoderRuns());
    EXPECT_EQ(1u, seg->cacheHits());

    // Another frame is another encoding.
    EXPECT_FALSE(seg->encode(image.data(), 64, 64, key(1, 2.)));
    EXPECT_EQ(2u, seg->encoderRuns());
    EXPECT_TRUE(seg->select(key(1)));
    EXPECT_EQ(2u, seg->cacheHits());
    EXPECT_TRUE(seg->currentKey() == key(1));

    // Past the capacity the least recently used goes: key(1, 2.) here.
    for (uint64_t h = 10; h < 10 + Segmenter::kCacheCapacity - 1; ++h) {
        seg->encode(image.data(), 64, 64, key(h));
    }
    EXPECT_TRUE(seg->isCached(key(1)));
    EXPECT_FALSE(seg->isCached(key(1, 2.)));
    EXPECT_FALSE(seg->select(key(1, 2.)));
}

TEST_F(AiSegmentation, SegNextRefusesBoxes)
{
    std::unique_ptr<Segmenter> seg = fakeSegNext();
    const std::vector<uint8_t> image = discImage(64, 64, { { 30., 26., 14. } });

    seg->encode(image.data(), 64, 64, key(1));
    SegmentationBox box;
    box.x1 = 10.f;
    box.y1 = 10.f;
    box.x2 = 50.f;
    box.y2 = 45.f;
    EXPECT_THROW(seg->decode({}, { box }), AiError);
    EXPECT_THROW(seg->segment({}, { box }, 0.5f), AiError);
}

TEST_F(AiSegmentation, DecodingWithoutAnEncodingFails)
{
    std::unique_ptr<Segmenter> seg = fakeSegNext();

    EXPECT_FALSE(seg->hasEncoding());
    EXPECT_THROW(seg->decode({ point(1.f, 1.f) }), AiError);
}

TEST_F(AiSegmentation, SamRoutesEncoderOutputsAndPicksTheBestMask)
{
    // The fake SAM encoder lists its outputs in another order, and one under a longer name, than
    // its decoder takes them; a wrong routing feeds a 3-channel tensor to a 1-channel input.
    std::unique_ptr<Segmenter> seg = fakeSam();
    ASSERT_TRUE(bool(seg));
    EXPECT_TRUE(seg->supportsBoxes());

    const Disc disc = { 30., 26., 14. };
    const std::vector<uint8_t> image = discImage(64, 64, { disc });
    seg->encode(image.data(), 64, 64, key(1));

    // Of the inverse, the object and the empty mask, the predicted IoU picks the object.
    std::vector<float> mask = seg->decode({ point(30.f, 26.f) });
    EXPECT_GT(iouWithDisc(mask, 64, 64, disc), 0.95);
    // A second decode feeds back the low-resolution mask.
    mask = seg->decode({ point(30.f, 26.f) });
    EXPECT_GT(iouWithDisc(mask, 64, 64, disc), 0.95);

    SegmentationBox box;
    box.x1 = 14.f;
    box.y1 = 10.f;
    box.x2 = 46.f;
    box.y2 = 42.f;
    mask = seg->segment({}, { box }, 0.5f);
    EXPECT_GT(iouWithDisc(mask, 64, 64, disc), 0.95);
}

TEST(AiSegmentationSeeds, OnlyPromptedAreasAreKept)
{
    const int w = 64, h = 64;
    const Disc left = { 16., 32., 10. };
    const Disc right = { 48., 32., 12. };
    std::vector<float> base((size_t)w * h, 0.f);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (inDisc(left, x, y) || inDisc(right, x, y)) {
                base[(size_t)y * w + x] = 1.f;
            }
        }
    }
    // A soft edge that belongs to the left disc.
    base[(size_t)32 * w + 5] = 0.2f;

    std::vector<float> mask = base;
    Segmenter::keepSeedAreas(mask, w, h, 0.5f, { point(16.f, 32.f) });
    EXPECT_GT(iouWithDisc(mask, w, h, left), 0.99);
    EXPECT_FLOAT_EQ(0.2f, mask[(size_t)32 * w + 5]);
    EXPECT_FLOAT_EQ(0.f, mask[(size_t)32 * w + 48]);

    // Negative points select nothing: the largest area stays.
    mask = base;
    Segmenter::keepSeedAreas(mask, w, h, 0.5f, { point(16.f, 32.f, SegmentationPoint::eLabelNegative) });
    EXPECT_GT(iouWithDisc(mask, w, h, right), 0.99);

    // Both prompted, both kept.
    mask = base;
    Segmenter::keepSeedAreas(mask, w, h, 0.5f, { point(16.f, 32.f), point(48.f, 32.f) });
    EXPECT_FLOAT_EQ(1.f, mask[(size_t)32 * w + 16]);
    EXPECT_FLOAT_EQ(1.f, mask[(size_t)32 * w + 48]);
}

/*
 * The real SegNext model, when NATRON_TEST_MASK_MODEL_DIR names an installed
 * mask-object-segnext-b2hq package folder (config.json, encoder.onnx, decoder.onnx). Tests never
 * download it.
 */
TEST_F(AiSegmentation, RealSegNextSegmentsABrightSquare)
{
    const char* dir = std::getenv("NATRON_TEST_MASK_MODEL_DIR");

    if (!dir || !*dir || !std::filesystem::exists(std::filesystem::path(dir) / "config.json")) {
        std::cerr << "[  SKIPPED ] NATRON_TEST_MASK_MODEL_DIR does not name an installed SegNext package" << std::endl;

        return;
    }
    const std::string encoder = (std::filesystem::path(dir) / "encoder.onnx").string();
    const ModelConfig config = readModelConfig(dir, "encoder.onnx");
    if (config.arch != "segnext") {
        std::cerr << "[  SKIPPED ] " << dir << " holds a " << config.arch << " model, not SegNext" << std::endl;

        return;
    }
    Segmenter seg(encoder, (std::filesystem::path(dir) / "decoder.onnx").string(), config, AiTest::cpuOptions(), AiTest::loader());

    // A positive click in the middle of a bright square on a dark ground.
    const int w = 320, h = 240;
    const int x0 = 110, y0 = 60, x1 = 230, y1 = 180;
    std::vector<uint8_t> image((size_t)w * h * 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool inside = (x >= x0) && (x < x1) && (y >= y0) && (y < y1);
            uint8_t* p = &image[((size_t)y * w + x) * 3];
            p[0] = p[1] = p[2] = inside ? 235 : 20;
        }
    }
    seg.encode(image.data(), w, h, key(1));
    const std::vector<float> mask = seg.segment({ point((x0 + x1) * 0.5f, (y0 + y1) * 0.5f) }, {}, 0.5f);
    ASSERT_EQ((size_t)w * h, mask.size());
    std::size_t both = 0, either = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool inside = (x >= x0) && (x < x1) && (y >= y0) && (y < y1);
            const bool selected = mask[(size_t)y * w + x] > 0.5f;
            both += (inside && selected) ? 1 : 0;
            either += (inside || selected) ? 1 : 0;
        }
    }
    EXPECT_GT((double)both / (double)either, 0.95);
}
