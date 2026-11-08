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
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawFixtures.h"
#include "RawNeuralDenoiseModels.h"
#include "RawReferenceSupport.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Metadata/EffectMetadata.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawNeuralDenoise.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

using SceneFn = std::function<void(int x, int y, float rgb[3])>;

void
texturedScene(int x,
              int y,
              float rgb[3])
{
    const float r2 = ((float)x - 37.f) * ((float)x - 37.f) + ((float)y - 29.f) * ((float)y - 29.f);

    rgb[0] = 0.4f + 0.3f * std::cos(r2 * 0.002f);
    rgb[1] = 0.5f + 0.25f * std::sin((float)x * 0.04f) * std::cos((float)y * 0.03f);
    rgb[2] = 0.3f + 0.002f * (float)((x * y) % 97);
}

RawTestSource::ValueFn
mosaicOf(const SceneFn& scene,
         const RawLayout& layout)
{
    return [scene, layout](int x, int y, int) {
        float rgb[3];
        scene(x, y, rgb);

        return rgb[layout.colourAt(x, y)];
    };
}

// Every photosite a different value, so any shifted, swapped or mis-mirrored read is visible.
void
hashedScene(int x,
            int y,
            float rgb[3])
{
    for (int c = 0; c < 3; ++c) {
        const unsigned h = ((unsigned)x * 73856093u) ^ ((unsigned)y * 19349663u) ^ ((unsigned)(c + 1) * 83492791u);
        rgb[c] = 0.05f + 0.9f * (float)((h >> 7) % 1000u) / 1000.f;
    }
}

int
reflect(int i,
        int n)
{
    const int period = 2 * (n - 1);

    i = ((i % period) + period) % period;

    return (i >= n) ? (period - i) : i;
}

struct PlaneTap {
    int plane;
    int down;
    int right;
    float weight;
};

// make-test-rawdenoise.py's PLANE_TAPS: the taps of the red, green and blue outputs.
const std::vector<std::vector<PlaneTap>> kPlaneTaps = {
    { { 0, 0, 0, 1.f }, { 1, 0, -1, 0.5f }, { 3, 1, 0, 0.25f } },
    { { 1, 0, 0, 0.5f }, { 2, 0, 0, 0.25f }, { 2, -1, 0, 0.25f }, { 1, 0, 1, 0.125f } },
    { { 3, 0, 0, 1.f }, { 2, 1, 1, 0.5f }, { 0, -1, -1, 0.25f } },
};

// The 512 x 512 crop of the Canon fixture tools/raw/make-neural-restore-reference.py takes from
// darktable's neural restore DNG, from the top-left of the visible image.
const char* const kReferenceFile = "canon-eos-5d-mark-iii-neural-restore.tif";
const int kReferenceSize = 512;
const int kReferenceLeft = 3072;
const int kReferenceTop = 1728;
// Normalised photosite values, both clipped to [0, 1].
const double kReferenceMeanTolerance = 2e-3;
const double kReferenceMaxTolerance = 3e-2;
} // namespace

class NativeRawNeuralDenoiseTest
    : public BaseTest {
protected:
    void SetUp() override
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
        _models.reset(new RawNeuralDenoiseModels);
        ASSERT_TRUE(_models->valid());
    }

    void TearDown() override
    {
        // The nodes refer to the model manager, so they go first.
        if (getApp()) {
            getApp()->getProject()->reset(false, true);
        }
        _models.reset();
        BaseTest::TearDown();
    }

    void install()
    {
        std::string error;
        ASSERT_TRUE(_models->install(&error)) << error;
        ASSERT_TRUE(_models->installed(RawNeuralDenoiseModels::kMatchGain));
        ASSERT_TRUE(_models->installed(RawNeuralDenoiseModels::kAbsolute));
        ASSERT_TRUE(_models->installed(RawNeuralDenoiseModels::kPlanes));
    }

    NodePtr createChain(const RectI& rod,
                        const RawTestSource::ValueFn& value,
                        const ImageMetadata& metadata)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWNEURALDENOISE));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, 1, value, metadata);
        connectNodes(source, node, 0, true);

        return node;
    }

    NodePtr createSceneChain(const RectI& rod,
                             const SceneFn& scene,
                             const std::array<double, 3>& wbApplied = { { 1., 1., 1. } })
    {
        ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
        RawMetadata::setWbApplied(&metadata, wbApplied);

        return createChain(rod, mosaicOf(scene, *RawLayout::fromMetadata(metadata)), metadata);
    }

    static void selectModel(const NodePtr& node,
                            const char* id)
    {
        KnobChoicePtr model = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>("model");
        ASSERT_TRUE(bool(model));
        model->setValueFromID(id, 0);
    }

    static RenderedPlane render(const NodePtr& node,
                                const RectI& window)
    {
        RenderedPlane plane;
        std::string error;

        EXPECT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << error;

        return plane;
    }

    static std::string persistentError(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return (type == eMessageTypeError) ? message.toStdString() : std::string();
    }

    // The persistent error after a render that must fail.
    static std::string failedRenderError(const NodePtr& node,
                                         const RectI& window)
    {
        RenderedPlane plane;
        std::string error;
        EXPECT_FALSE(renderRawTestPlane(node, window, &plane, &error));

        return persistentError(node);
    }

    // The bundled gtest has no GTEST_SKIP; this prints the line its runner would.
    static void skipped(const std::string& reason)
    {
        const ::testing::TestInfo* const info = ::testing::UnitTest::GetInstance()->current_test_info();

        std::cout << "[  SKIPPED ] " << info->test_case_name() << "." << info->name() << ": " << reason << std::endl;
    }

    std::unique_ptr<RawNeuralDenoiseModels> _models;
};

// Strength 0 is the bilinear demosaic and needs no model: with AI off and nothing installed the
// render still succeeds, and every red and blue photosite keeps its own sample.
TEST_F(NativeRawNeuralDenoiseTest, StrengthZeroNeedsNoModel)
{
    appPTR->getCurrentSettings()->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(false);
    const RectI rod(0, 0, 75, 61);
    const SceneFn flat = [](int, int, float rgb[3]) {
        rgb[0] = 0.62f;
        rgb[1] = 0.35f;
        rgb[2] = 0.48f;
    };
    for (int pass = 0; pass < 2; ++pass) {
        const SceneFn scene = (pass == 0) ? flat : SceneFn(texturedScene);
        NodePtr node = createSceneChain(rod, scene);
        ASSERT_TRUE(bool(node));
        KnobDoublePtr strength = node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawNeuralDenoiseParamStrength);
        ASSERT_TRUE(bool(strength));
        strength->setValue(0.);

        const RenderedPlane plane = render(node, rod);
        EXPECT_EQ(std::string(), persistentError(node));
        ASSERT_EQ(4u, plane.channels.size());
        const RawLayout layout = *RawLayout::fromMetadata(rawTestMetadata("RGGB", 0, 0, eRawStagePrepared));
        for (int y = rod.y1; y < rod.y2; ++y) {
            for (int x = rod.x1; x < rod.x2; ++x) {
                float rgb[3];
                scene(x, y, rgb);
                const int own = layout.colourAt(x, y);
                float expectedOwn = rgb[own];
                if (own == 1) {
                    // The demosaic's green is the mean of the two greens: the site's own and the
                    // mean of the other green's sites among its diagonal neighbours.
                    float other = 0.f;
                    int count = 0;
                    for (int dy = -1; dy <= 1; dy += 2) {
                        for (int dx = -1; dx <= 1; dx += 2) {
                            if (rod.contains(x + dx, y + dy)) {
                                float neighbour[3];
                                scene(x + dx, y + dy, neighbour);
                                other += neighbour[1];
                                ++count;
                            }
                        }
                    }
                    expectedOwn = 0.5f * (rgb[1] + other / (float)count);
                }
                ASSERT_NEAR(expectedOwn, rawTestPlaneValue(plane, x, y, own), 1e-6) << x << "," << y;
                if (pass == 0) {
                    for (int c = 0; c < 3; ++c) {
                        ASSERT_NEAR(rgb[c], rawTestPlaneValue(plane, x, y, c), 1e-5) << x << "," << y << " channel " << c;
                    }
                }
            }
        }
    }
}

// The planes model reads the neighbouring blocks with a different tap per plane and direction. The
// expected values are taken from the mosaic by hand, with the rows and columns past each edge
// mirrored photosite by photosite, so a swapped green plane, a shifted block grid or another edge
// rule shows up at the image's own edges.
TEST_F(NativeRawNeuralDenoiseTest, EveryPlaneAndEdgeReachesTheModelAsDarktablePacksIt)
{
    install();
    const int width = 36;
    const int height = 28;
    const RectI rod(0, 0, width, height);
    // The pattern's origin puts a red photosite on the top-left corner, so the whole image is the
    // model's working region; from the bottom-left origin the top row would be a green and blue one
    // that the model, forced to RGGB, skips.
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, (height - 1) & 1, eRawStagePrepared);
    const RawLayout layout = *RawLayout::fromMetadata(metadata);
    NodePtr node = createChain(rod, mosaicOf(hashedScene, layout), metadata);
    ASSERT_TRUE(bool(node));
    selectModel(node, RawNeuralDenoiseModels::kPlanes);

    const RawTestSource::ValueFn mosaic = mosaicOf(hashedScene, layout);
    // Plane `plane` of block (row, column) counted from the top-left: R, G1, G2, B at its sites.
    const auto packed = [&](int plane, int row, int column) {
        const int r = reflect(2 * row + (plane >> 1), height);
        const int c = reflect(2 * column + (plane & 1), width);

        return mosaic(c, height - 1 - r, 0);
    };

    const RenderedPlane plane = render(node, rod);
    EXPECT_EQ(std::string(), persistentError(node));
    ASSERT_EQ(4u, plane.channels.size());
    for (int y = rod.y1; y < rod.y2; ++y) {
        for (int x = rod.x1; x < rod.x2; ++x) {
            const int row = (height - 1 - y) / 2;
            const int column = x / 2;
            for (int c = 0; c < 3; ++c) {
                float expected = 0.f;
                for (const PlaneTap& tap : kPlaneTaps[c]) {
                    expected += tap.weight * packed(tap.plane, row + tap.down, column + tap.right);
                }
                ASSERT_NEAR(expected, rawTestPlaneValue(plane, x, y, c), 1e-5) << x << "," << y << " channel " << c;
            }
        }
    }
}

TEST_F(NativeRawNeuralDenoiseTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWNEURALDENOISE, PLUGIN_MAJOR_NATRON_RAWNEURALDENOISE, "RawNeuralDenoise");
}

TEST_F(NativeRawNeuralDenoiseTest, KnobsAreModelStrengthAndTileSize)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWNEURALDENOISE));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    KnobChoicePtr model = effect->getKnobByNameAndType<KnobChoice>("model");
    ASSERT_TRUE(bool(model));
    EXPECT_EQ(std::string(RawNeuralDenoiseModels::kMatchGain), model->getActiveEntry().id);
    KnobDoublePtr strength = effect->getKnobByNameAndType<KnobDouble>(kRawNeuralDenoiseParamStrength);
    ASSERT_TRUE(bool(strength));
    EXPECT_EQ("Strength", strength->getLabel());
    EXPECT_EQ(1., strength->getDefaultValue(0));
    KnobIntPtr tileSize = effect->getKnobByNameAndType<KnobInt>(kRawNeuralDenoiseParamTileSize);
    ASSERT_TRUE(bool(tileSize));
    EXPECT_EQ("Tile Size", tileSize->getLabel());
    EXPECT_EQ(0, tileSize->getDefaultValue(0));
}

TEST_F(NativeRawNeuralDenoiseTest, OutputsAnImageWithoutTheCfaKeys)
{
    ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    const std::array<double, 9> camToXyz = { 0.6, 0.2, 0.15, 0.3, 0.8, -0.1, 0.02, -0.15, 1.1 };
    RawMetadata::setCamToXyz(&metadata, camToXyz);
    NodePtr node = createChain(RectI(0, 0, 64, 48), mosaicOf(texturedScene, *RawLayout::fromPattern("RGGB")), metadata);
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindImage, node->getEffectInstance()->getOutputDataKind());

    const ImageMetadata out = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get())->getOutputMetadata(1., ViewIdx(0));
    EXPECT_FALSE(out.contains(kRawMetadataCfaPattern));
    EXPECT_FALSE(out.contains(kRawMetadataCfaOrigin));
    EXPECT_FALSE(out.contains(kRawMetadataStage));
    EXPECT_FALSE(out.contains(kRawMetadataBlackLevels));
    EXPECT_FALSE(out.contains(kRawMetadataWhiteLevel));
    EXPECT_EQ(camToXyz, RawMetadata::getCamToXyz(out).value_or(std::array<double, 9> {}));
}

TEST_F(NativeRawNeuralDenoiseTest, UnpreparedDataRaisesTheStageError)
{
    install();
    const RectI rod(0, 0, 32, 24);
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStageRaw);
    NodePtr node = createChain(rod, mosaicOf(texturedScene, *RawLayout::fromPattern("RGGB")), metadata);
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(RawMetadata::requireStage(metadata, eRawStagePrepared, node->getLabel()).value_or(std::string()), failedRenderError(node, rod));
}

TEST_F(NativeRawNeuralDenoiseTest, XTransRaisesTheBayerError)
{
    install();
    const RectI rod(0, 0, 36, 36);
    const ImageMetadata metadata = rawTestMetadata(kXTransPattern, 0, 0, eRawStagePrepared);
    NodePtr node = createChain(rod, mosaicOf(texturedScene, *RawLayout::fromMetadata(metadata)), metadata);
    ASSERT_TRUE(bool(node));
    const std::string message = failedRenderError(node, rod);
    EXPECT_NE(std::string::npos, message.find("X-Trans")) << message;
    EXPECT_NE(std::string::npos, message.find("NeuralDenoise")) << message;
}

TEST_F(NativeRawNeuralDenoiseTest, AMissingModelRaisesTheModelError)
{
    const RectI rod(0, 0, 32, 24);
    NodePtr node = createSceneChain(rod, texturedScene);
    ASSERT_TRUE(bool(node));
    const std::string message = failedRenderError(node, rod);
    EXPECT_NE(std::string::npos, message.find("not installed")) << message;
}

TEST_F(NativeRawNeuralDenoiseTest, TileSizesTheModelCannotRunRaiseAnError)
{
    install();
    const RectI rod(0, 0, 32, 24);
    NodePtr node = createSceneChain(rod, texturedScene);
    ASSERT_TRUE(bool(node));
    KnobIntPtr tileSize = node->getEffectInstance()->getKnobByNameAndType<KnobInt>(kRawNeuralDenoiseParamTileSize);
    ASSERT_TRUE(bool(tileSize));

    // The fixture is exported at 128 x 128.
    tileSize->setValue(256);
    std::string message = failedRenderError(node, rod);
    EXPECT_NE(std::string::npos, message.find("Tile Size")) << message;

    tileSize->setValue(64);
    message = failedRenderError(node, rod);
    EXPECT_NE(std::string::npos, message.find("too small")) << message;

    tileSize->setValue(0);
    render(node, rod);
    EXPECT_EQ(std::string(), persistentError(node));
}

// A flat colour comes back exactly, white balance included: packing divides out what was applied
// and unpacking multiplies it back.
TEST_F(NativeRawNeuralDenoiseTest, AFlatColourComesBackUnderTheAbsoluteModel)
{
    install();
    const RectI rod(0, 0, 75, 61);
    const SceneFn flat = [](int, int, float rgb[3]) {
        rgb[0] = 0.62f;
        rgb[1] = 0.35f;
        rgb[2] = 0.48f;
    };
    NodePtr node = createSceneChain(rod, flat, { { 2., 1., 1.5 } });
    ASSERT_TRUE(bool(node));
    selectModel(node, RawNeuralDenoiseModels::kAbsolute);

    const RenderedPlane plane = render(node, rod);
    ASSERT_EQ(4u, plane.channels.size());
    for (int y = rod.y1; y < rod.y2; ++y) {
        for (int x = rod.x1; x < rod.x2; ++x) {
            float rgb[3];
            flat(x, y, rgb);
            for (int c = 0; c < 3; ++c) {
                ASSERT_NEAR(rgb[c], rawTestPlaneValue(plane, x, y, c), 1e-5) << x << "," << y << " channel " << c;
            }
            ASSERT_EQ(1.f, rawTestPlaneValue(plane, x, y, 3));
        }
    }
}

// The fixture's default model returns a thousand times its input; match_gain brings each tile's
// mean back to its input's, which for grey is the input itself.
TEST_F(NativeRawNeuralDenoiseTest, MatchGainKeepsAGreyLevel)
{
    install();
    const RectI rod(0, 0, 300, 264);
    const SceneFn grey = [](int, int, float rgb[3]) {
        rgb[0] = rgb[1] = rgb[2] = 0.3f;
    };
    NodePtr node = createSceneChain(rod, grey);
    ASSERT_TRUE(bool(node));

    const RenderedPlane plane = render(node, rod);
    ASSERT_EQ(4u, plane.channels.size());
    for (int y = rod.y1; y < rod.y2; ++y) {
        for (int x = rod.x1; x < rod.x2; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_NEAR(0.3, rawTestPlaneValue(plane, x, y, c), 1e-5) << x << "," << y << " channel " << c;
            }
        }
    }
}

// A render reads the tiles of darktable's whole-frame grid that reach its window, so any window
// is the same pixels as the full frame, across the seams where per-tile gains differ.
TEST_F(NativeRawNeuralDenoiseTest, WindowsMatchTheFullFrame)
{
    install();
    const RectI rod(0, 0, 300, 264);
    NodePtr node = createSceneChain(rod, texturedScene, { { 1.8, 1., 1.4 } });
    ASSERT_TRUE(bool(node));

    const RenderedPlane full = render(node, rod);
    ASSERT_EQ(4u, full.channels.size());
    const RectI windows[] = {
        RectI(0, 0, 9, 7),
        RectI(120, 100, 140, 160),
        RectI(250, 0, 300, 264),
        RectI(291, 255, 300, 264),
        RectI(127, 127, 129, 129),
        RectI(60, 200, 200, 210),
    };
    for (const RectI& window : windows) {
        const RenderedPlane plane = render(node, window);
        ASSERT_EQ(4u, plane.channels.size());
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                for (int c = 0; c < 3; ++c) {
                    ASSERT_NEAR(rawTestPlaneValue(full, x, y, c), rawTestPlaneValue(plane, x, y, c), 1e-6)
                        << x << "," << y << " channel " << c << " in " << window.x1 << "," << window.y1 << "-" << window.x2 << "," << window.y2;
                }
            }
        }
    }
}

// Cropping a column and a row off the top-left moves the first red photosite, so the blocks are
// packed from another origin; they are the same blocks, so the colours never swap.
TEST_F(NativeRawNeuralDenoiseTest, AnOddCropMatchesTheUncroppedRun)
{
    install();
    const RectI rod(0, 0, 160, 140);
    const RectI cropped(1, 0, 160, 139);
    const std::array<double, 3> wb = { { 2.1, 1., 1.6 } };
    NodePtr full = createSceneChain(rod, texturedScene, wb);
    NodePtr crop = createSceneChain(cropped, texturedScene, wb);
    ASSERT_TRUE(full && crop);
    selectModel(full, RawNeuralDenoiseModels::kAbsolute);
    selectModel(crop, RawNeuralDenoiseModels::kAbsolute);

    const RenderedPlane expected = render(full, rod);
    const RenderedPlane actual = render(crop, cropped);
    ASSERT_EQ(4u, expected.channels.size());
    ASSERT_EQ(4u, actual.channels.size());
    // Two photosites in, the rows and columns outside the cropped image's 2x2 blocks are left out.
    for (int y = cropped.y1 + 2; y < cropped.y2 - 2; ++y) {
        for (int x = cropped.x1 + 2; x < cropped.x2 - 2; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_NEAR(rawTestPlaneValue(expected, x, y, c), rawTestPlaneValue(actual, x, y, c), 1e-6) << x << "," << y << " channel " << c;
            }
        }
    }
}

// darktable's neural restore of the Canon fixture with RawNIND, re-mosaiced, against the node's
// output at each photosite's own colour. Runs when the model is installed in the folder
// NATRON_TEST_AI_MODELS_DIR names, else in the default models folder, and reports itself skipped
// otherwise.
TEST_F(NativeRawNeuralDenoiseTest, MatchesDarktableNeuralRestore)
{
    const char* const env = std::getenv("NATRON_TEST_AI_MODELS_DIR");
    const std::string modelsDirectory = env ? std::string(env) : ModelRegistry::defaultModelsDirectory();
    ModelDownloader downloader(std::make_shared<const ModelRegistry>(ModelRegistry::builtinManifest(), modelsDirectory));
    if (downloader.registry()->resolve("rawdenoise-nind-bayer").status != AiModelStatus::eInstalled) {
        skipped("rawdenoise-nind is not installed in " + modelsDirectory);

        return;
    }
    const std::string reference = std::string(NATRON_TESTS_FIXTURES_DIR "/raw/reference/") + kReferenceFile;
    std::vector<float> expected;
    int width = 0;
    int height = 0;
    std::string error;
    if (!RawReference::readReference(reference, &expected, &width, &height, &error)) {
        skipped("no reference: " + error);

        return;
    }
    ASSERT_EQ(kReferenceSize, width);
    ASSERT_EQ(kReferenceSize, height);
    RawNeuralDenoise::setModelDownloaderForTests(&downloader);

    // darktable restores the raw mosaic: no white balance, no highlight reconstruction.
    NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ));
    ASSERT_TRUE(bool(read));
    KnobFilePtr file = read->getEffectInstance()->getKnobByNameAndType<KnobFile>(kOfxImageEffectFileParamName);
    ASSERT_TRUE(bool(file));
    file->setValue(bayerFixture().path());
    NodePtr prepare = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWPREPARE));
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWNEURALDENOISE));
    ASSERT_TRUE(prepare && node);
    connectNodes(read, prepare, 0, true);
    connectNodes(prepare, node, 0, true);
    selectModel(node, "rawdenoise-nind-bayer");

    RectD rod;
    bool isProjectFormat = false;
    EffectInstancePtr effect = node->getEffectInstance();
    ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), 1., RenderScale::identity, ViewIdx(0), &rod, &isProjectFormat));
    const std::optional<RawLayout> layout = RawLayout::fromMetadata(getEffectOutputMetadata(prepare->getEffectInstance(), 1., ViewIdx(0)));
    ASSERT_TRUE(layout && (layout->period() == 2));
    const int y1 = (int)rod.y2 - kReferenceTop - kReferenceSize;
    const RectI window((int)rod.x1 + kReferenceLeft, y1, (int)rod.x1 + kReferenceLeft + kReferenceSize, y1 + kReferenceSize);
    const RenderedPlane plane = render(node, window);
    RawNeuralDenoise::setModelDownloaderForTests(nullptr);
    ASSERT_EQ(4u, plane.channels.size());

    double sum = 0.;
    double maximum = 0.;
    for (int row = 0; row < kReferenceSize; ++row) {
        const int y = window.y2 - 1 - row;
        for (int col = 0; col < kReferenceSize; ++col) {
            const int x = window.x1 + col;
            const double ours = std::max(0., std::min(1., (double)rawTestPlaneValue(plane, x, y, layout->colourAt(x, y))));
            const double theirs = std::max(0., std::min(1., (double)expected[((std::size_t)row * kReferenceSize + col) * 3]));
            const double diff = std::fabs(ours - theirs);
            sum += diff;
            maximum = std::max(maximum, diff);
        }
    }
    const double mean = sum / ((double)kReferenceSize * kReferenceSize);
    std::cout << "darktable neural restore: mean " << mean << " max " << maximum << std::endl;
    EXPECT_LE(mean, kReferenceMeanTolerance);
    EXPECT_LE(maximum, kReferenceMaxTolerance);
}
