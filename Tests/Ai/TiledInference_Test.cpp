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

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/Tensor.h"
#include "Engine/Ai/TiledInference.h"
#include "Engine/RectI.h"
#include "Tests/Ai/AiTestSupport.h"

NATRON_NAMESPACE_USING

namespace {

typedef std::vector<std::vector<int64_t>> Shapes;

// The source image: a distinct value per pixel and channel, defined over the whole plane.
float
sourceValue(int c,
            int x,
            int y)
{
    return (float)(((x * 31 + y * 17 + c * 7) % 1009 + 1009) % 1009) * 0.01f - 3.f + (float)c;
}

AiTilePack
packSource(int channels)
{
    return [channels](const RectI& rect, Tensor& tile) {
        ASSERT_EQ((std::vector<int64_t> { 1, channels, rect.height(), rect.width() }), tile.shape());
        for (int c = 0; c < channels; ++c) {
            for (int row = 0; row < rect.height(); ++row) {
                for (int col = 0; col < rect.width(); ++col) {
                    tile.at(0, c, row, col) = sourceValue(c, rect.x1 + col, rect.y2 - 1 - row);
                }
            }
        }
    };
}

struct Unpacked {
    int calls = 0;
    RectI outRoI;
    Tensor result;
};

AiTileUnpack
keep(Unpacked* into)
{
    return [into](const RectI& outRoI, const Tensor& result) {
        ++into->calls;
        into->outRoI = outRoI;
        into->result = result;
    };
}

// Every output pixel equals the source pixel it covers, at `scale` output pixels per input pixel.
void
expectSourceAt(const RectI& roi,
               int scale,
               const Unpacked& unpacked)
{
    ASSERT_EQ(1, unpacked.calls);
    EXPECT_EQ(RectI(roi.x1 * scale, roi.y1 * scale, roi.x2 * scale, roi.y2 * scale), unpacked.outRoI);
    const std::vector<int64_t> shape { 1, 3, (int64_t)roi.height() * scale, (int64_t)roi.width() * scale };
    ASSERT_EQ(shape, unpacked.result.shape());
    for (int c = 0; c < 3; ++c) {
        for (int64_t row = 0; row < shape[2]; ++row) {
            for (int64_t col = 0; col < shape[3]; ++col) {
                const float expected = sourceValue(c, roi.x1 + (int)(col / scale), roi.y2 - 1 - (int)(row / scale));
                ASSERT_EQ(expected, unpacked.result.at(0, c, row, col)) << "channel " << c << " row " << row << " column " << col;
            }
        }
    }
}

// Records the input shape of every run, and fails the run with `failure` when it says so.
class ShapeRecorder {
public:
    std::function<std::string(const Shapes&)> failure;

    InferenceSession::FailureInjector injector()
    {
        return [this](const InferenceSession&, const Shapes& shapes) {
            std::lock_guard<std::mutex> lock(_mutex);
            _shapes.push_back(shapes.at(0));

            return failure ? failure(shapes) : std::string();
        };
    }

    Shapes shapes()
    {
        std::lock_guard<std::mutex> lock(_mutex);

        return _shapes;
    }

private:
    std::mutex _mutex;
    Shapes _shapes;
};

const char* const kOutOfMemory = "CUDA failure 2: out of memory";

std::shared_ptr<InferenceSession>
openCpu(const char* model)
{
    return InferenceSession::open(AiTest::modelPath(model), AiTest::cpuOptions(), AiTest::loader());
}

class AiTiledInference
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(AiTest::loader()->available()) << AiTest::loader()->unavailableReason();
    }
};

} // namespace

TEST(AiTileStarts, TilesCoverTheLengthWithTheOverlap)
{
    EXPECT_EQ(std::vector<int> { 0 }, tileStarts(100, 256, 32));
    EXPECT_EQ(std::vector<int> { 0 }, tileStarts(256, 256, 32));

    for (int length : { 257, 700, 1000, 1023 }) {
        const std::vector<int> starts = tileStarts(length, 256, 32);
        ASSERT_GE(starts.size(), 2u) << "length " << length;
        EXPECT_EQ(0, starts.front());
        EXPECT_EQ(length - 256, starts.back());
        for (std::size_t i = 1; i < starts.size(); ++i) {
            EXPECT_GT(starts[i], starts[i - 1]) << "length " << length;
            EXPECT_LE(starts[i] - starts[i - 1], 256 - 32) << "length " << length;
        }
    }
}

TEST_F(AiTiledInference, IdentityOverAnOddSized1000x700ImageReproducesTheInput)
{
    const RectI roi(-13, 21, 987, 721);
    AiTileSpec spec;
    spec.tile = 256;
    spec.overlap = 32;
    Unpacked unpacked;

    const AiTiledRunInfo info = runTiled(openCpu("identity.onnx"), roi, spec, packSource(3), keep(&unpacked));

    EXPECT_EQ(256, info.tile);
    EXPECT_EQ((int)(tileStarts(1000, 256, 32).size() * tileStarts(700, 256, 32).size()), info.tiles);
    EXPECT_GT(info.tiles, 1);
    EXPECT_FALSE(info.halved);
    EXPECT_FALSE(info.cpuFallback);
    expectSourceAt(roi, 1, unpacked);
}

TEST_F(AiTiledInference, ReflectionPaddingToAMultipleIsCroppedOff)
{
    const RectI roi(5, 3, 306, 206);
    AiTileSpec spec;
    spec.tile = 250;
    spec.overlap = 32;
    spec.multipleOf = 16;
    ShapeRecorder recorder;
    AiTest::InjectorGuard guard(recorder.injector());
    Unpacked unpacked;

    runTiled(openCpu("identity.onnx"), roi, spec, packSource(3), keep(&unpacked));

    const Shapes shapes = recorder.shapes();
    ASSERT_FALSE(shapes.empty());
    for (const std::vector<int64_t>& shape : shapes) {
        EXPECT_EQ(0, shape[2] % 16) << Tensor::shapeString(shape);
        EXPECT_EQ(0, shape[3] % 16) << Tensor::shapeString(shape);
    }
    expectSourceAt(roi, 1, unpacked);
}

TEST_F(AiTiledInference, NearestUpscaleGivesTheExactDoubledImage)
{
    const RectI roi(-7, 11, 294, 214);
    AiTileSpec spec;
    spec.tile = 128;
    spec.overlap = 16;
    spec.scale = 2;
    Unpacked unpacked;

    const AiTiledRunInfo info = runTiled(openCpu("upscale2-nearest.onnx"), roi, spec, packSource(3), keep(&unpacked));

    EXPECT_GT(info.tiles, 1);
    expectSourceAt(roi, 2, unpacked);
}

TEST_F(AiTiledInference, AFixedSizeModelGetsTilesOfItsSize)
{
    const RectI roi(0, 0, 150, 50);
    AiTileSpec spec;
    spec.tile = 512;
    spec.overlap = 8;
    ShapeRecorder recorder;
    AiTest::InjectorGuard guard(recorder.injector());
    Unpacked unpacked;

    const AiTiledRunInfo info = runTiled(openCpu("identity-64.onnx"), roi, spec, packSource(3), keep(&unpacked));

    EXPECT_EQ((int)tileStarts(150, 64, 8).size(), info.tiles);
    const Shapes shapes = recorder.shapes();
    ASSERT_EQ((std::size_t)info.tiles, shapes.size());
    for (const std::vector<int64_t>& shape : shapes) {
        EXPECT_EQ((std::vector<int64_t> { 1, 3, 64, 64 }), shape);
    }
    expectSourceAt(roi, 1, unpacked);
}

TEST_F(AiTiledInference, OutOfMemoryHalvesTheTileOnce)
{
    const RectI roi(0, 0, 1000, 700);
    AiTileSpec spec;
    spec.tile = 512;
    spec.overlap = 32;
    ShapeRecorder recorder;
    recorder.failure = [](const Shapes& shapes) {
        return ((shapes[0][2] > 256) || (shapes[0][3] > 256)) ? std::string(kOutOfMemory) : std::string();
    };
    AiTest::InjectorGuard guard(recorder.injector());
    Unpacked unpacked;

    const AiTiledRunInfo info = runTiled(openCpu("identity.onnx"), roi, spec, packSource(3), keep(&unpacked));

    EXPECT_TRUE(info.halved);
    EXPECT_FALSE(info.cpuFallback);
    EXPECT_EQ(256, info.tile);
    EXPECT_EQ((int)(tileStarts(1000, 256, 32).size() * tileStarts(700, 256, 32).size()), info.tiles);
    const Shapes shapes = recorder.shapes();
    ASSERT_FALSE(shapes.empty());
    EXPECT_EQ((std::vector<int64_t> { 1, 3, 512, 512 }), shapes.front());
    expectSourceAt(roi, 1, unpacked);
}

TEST_F(AiTiledInference, OutOfMemoryAfterHalvingOnTheCpuPropagates)
{
    const RectI roi(0, 0, 1000, 700);
    AiTileSpec spec;
    spec.tile = 512;
    spec.overlap = 32;
    ShapeRecorder recorder;
    recorder.failure = [](const Shapes&) {
        return std::string(kOutOfMemory);
    };
    AiTest::InjectorGuard guard(recorder.injector());
    Unpacked unpacked;

    EXPECT_THROW(runTiled(openCpu("identity.onnx"), roi, spec, packSource(3), keep(&unpacked)), AiOutOfMemory);

    // One run at the full tile, one at half, and no third attempt: the session is already on the CPU.
    const Shapes shapes = recorder.shapes();
    ASSERT_EQ(2u, shapes.size());
    EXPECT_EQ((std::vector<int64_t> { 1, 3, 512, 512 }), shapes[0]);
    EXPECT_EQ((std::vector<int64_t> { 1, 3, 256, 256 }), shapes[1]);
    EXPECT_EQ(0, unpacked.calls);
}

TEST_F(AiTiledInference, AFixedSizeModelOutOfMemoryIsNotHalved)
{
    AiTileSpec spec;
    spec.tile = 512;
    spec.overlap = 8;
    ShapeRecorder recorder;
    recorder.failure = [](const Shapes&) {
        return std::string(kOutOfMemory);
    };
    AiTest::InjectorGuard guard(recorder.injector());
    Unpacked unpacked;

    EXPECT_THROW(runTiled(openCpu("identity-64.onnx"), RectI(0, 0, 200, 200), spec, packSource(3), keep(&unpacked)), AiOutOfMemory);
    EXPECT_EQ(1u, recorder.shapes().size());
    EXPECT_EQ(0, unpacked.calls);
}

TEST_F(AiTiledInference, AnAbortMidwayNeverUnpacks)
{
    const RectI roi(0, 0, 1000, 700);
    AiTileSpec spec;
    spec.tile = 256;
    spec.overlap = 32;
    std::atomic<int> packed(0);
    const AiTilePack pack = [&packed, inner = packSource(3)](const RectI& rect, Tensor& tile) {
        inner(rect, tile);
        ++packed;
    };
    Unpacked unpacked;

    EXPECT_THROW(runTiled(openCpu("identity.onnx"), roi, spec, pack, keep(&unpacked), [&packed]() { return packed >= 3; }), AiAborted);
    EXPECT_EQ(3, packed.load());
    EXPECT_EQ(0, unpacked.calls);
}

TEST_F(AiTiledInference, AnEmptyRoIRunsNothing)
{
    Unpacked unpacked;
    const AiTiledRunInfo info = runTiled(openCpu("identity.onnx"), RectI(10, 10, 10, 40), AiTileSpec(), packSource(3), keep(&unpacked));

    EXPECT_EQ(0, info.tiles);
    EXPECT_EQ(0, unpacked.calls);
}

TEST_F(AiTiledInference, AnInvalidSpecOrModelIsAnError)
{
    Unpacked unpacked;
    AiTileSpec overlapTooLarge;
    overlapTooLarge.tile = 64;
    overlapTooLarge.overlap = 64;

    EXPECT_THROW(runTiled(openCpu("identity.onnx"), RectI(0, 0, 100, 100), overlapTooLarge, packSource(3), keep(&unpacked)), AiError);
    EXPECT_THROW(runTiled(std::shared_ptr<InferenceSession>(), RectI(0, 0, 100, 100), AiTileSpec(), packSource(3), keep(&unpacked)), AiError);

    // The model takes three channels.
    AiTileSpec fourChannels;
    fourChannels.inChannels = 4;
    EXPECT_THROW(runTiled(openCpu("identity.onnx"), RectI(0, 0, 100, 100), fourChannels, packSource(4), keep(&unpacked)), AiError);

    // The identity returns three channels at scale 1, not the doubled image the spec expects.
    AiTileSpec wrongScale;
    wrongScale.scale = 2;
    EXPECT_THROW(runTiled(openCpu("identity.onnx"), RectI(0, 0, 100, 100), wrongScale, packSource(3), keep(&unpacked)), AiError);
    EXPECT_EQ(0, unpacked.calls);
}
