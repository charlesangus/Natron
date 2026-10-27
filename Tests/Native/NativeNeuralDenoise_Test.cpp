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

#include <array>
#include <cmath>
#include <iostream>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "OcioNodeTestSupport.h"
#include "RenderBothWays.h"

#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelKnob.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/Ai/OrtLoader.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/NeuralDenoise.h"
#include "Engine/Nodes/OCIO/ModelColourspace.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
using namespace OcioNodeTest;

namespace {

// Written by tools/ai/make-test-dtmodel.py: identity.onnx, any height and width.
const char* const kFixturePackage = "test-identity";
const char* const kFixtureSha256 = "eb95dd0d3cfecc9a713799210f0f9fdcb29b1745a8adbf846ddc1b7ad20a92b8";
const int kFixtureSize = 602;
// One model per task, both the identity package.
const char* const kSrgbModel = "test-denoise";
const char* const kLinearModel = "test-denoise-linear";
const char* const kLinearRec2020 = "Linear Rec.2020";

QJsonObject
fixtureEntry(const char* id,
             const char* name,
             const char* task)
{
    return QJsonObject {
        { QStringLiteral("id"), QString::fromLatin1(id) },
        { QStringLiteral("task"), QString::fromLatin1(task) },
        { QStringLiteral("name"), QString::fromLatin1(name) },
        { QStringLiteral("package"), QString::fromLatin1(kFixturePackage) },
        { QStringLiteral("files"), QJsonArray { QStringLiteral("model.onnx") } },
        { QStringLiteral("url"), QUrl::fromLocalFile(QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/ai/identity.dtmodel")).toString() },
        { QStringLiteral("sha256"), QString::fromLatin1(kFixtureSha256) },
        { QStringLiteral("size"), kFixtureSize },
        { QStringLiteral("weightsLicence"), QStringLiteral("MIT") },
        { QStringLiteral("trainingDataLicence"), QStringLiteral("none") },
        { QStringLiteral("osaid"), QStringLiteral("Open Source AI") },
        { QStringLiteral("cardUrl"), QStringLiteral("https://example.invalid/card") },
    };
}

// This gtest has no GTEST_SKIP: a skipped case prints why, records it in the XML report and passes.
void
reportSkip(const std::string& reason)
{
    std::cerr << "SKIPPED: " << ::testing::UnitTest::GetInstance()->current_test_info()->name() << ": " << reason << std::endl;
    ::testing::Test::RecordProperty("skipped", reason);
}

} // namespace

class NativeNeuralDenoiseTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        setConfig(getApp(), kStudioURI);

        ASSERT_TRUE(_models.isValid());
        const QJsonArray models { fixtureEntry(kSrgbModel, "Identity sRGB", "denoise"),
                                  fixtureEntry(kLinearModel, "Identity Linear", "denoise-linear") };
        const QJsonObject root { { QStringLiteral("schema"), 1 }, { QStringLiteral("models"), models } };
        _downloader.reset(new ModelDownloader(std::make_shared<const ModelRegistry>(QJsonDocument(root).toJson().toStdString(),
                                                                                    _models.path().toStdString())));
        NeuralDenoise::downloaderForTesting() = _downloader.get();

        SettingsPtr settings = appPTR->getCurrentSettings();
        settings->getKnobByNameAndType<KnobPath>("aiOrtLibraryPath")->setValue(std::string(NATRON_TESTS_ONNXRUNTIME_DIR));
        settings->getKnobByNameAndType<KnobChoice>("aiDevice")->setValueFromID("cpu", 0);
        settings->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(true);
    }

    virtual void TearDown() OVERRIDE
    {
        // The nodes refer to the downloader, so they go first.
        getApp()->getProject()->reset(false, true);
        NeuralDenoise::downloaderForTesting() = nullptr;
        _downloader.reset();
        SettingsPtr settings = appPTR->getCurrentSettings();
        settings->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(false);
        settings->getKnobByNameAndType<KnobChoice>("aiDevice")->setValueFromID("auto", 0);
        settings->getKnobByNameAndType<KnobPath>("aiOrtLibraryPath")->setValue(std::string());
        BaseTest::TearDown();
    }

    ParityPair makePair()
    {
        ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_NEURALDENOISE, -1, PLUGIN_MAJOR_NATRON_NEURALDENOISE);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(pair.native && dynamic_cast<NeuralDenoise*>(pair.native->getEffectInstance().get()));

        return pair;
    }

    // Installs the identity package, which both models share. Returns why inference cannot run,
    // or an empty string.
    std::string install()
    {
        std::string error;
        if (!_downloader->download(kSrgbModel, &error)) {
            ADD_FAILURE() << error;

            return error;
        }
        const std::shared_ptr<const OrtLoader> ort = OrtLoader::process();

        return ort->available() ? std::string() : "ONNX Runtime is unavailable: " + ort->unavailableReason();
    }

    static std::string errorOf(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return (type == eMessageTypeError) ? message.toStdString() : std::string();
    }

    static NeuralDenoise* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<NeuralDenoise*>(node->getEffectInstance().get()) : nullptr;
    }

    static void setBool(const NodePtr& node,
                        const char* name,
                        bool value)
    {
        KnobBoolPtr knob = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(name));
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValue(value);
    }

    static void setChoice(const NodePtr& node,
                          const char* name,
                          const char* id)
    {
        KnobChoicePtr knob = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(name));
        ASSERT_TRUE(bool(knob)) << name;
        knob->setValueFromID(id, 0);
    }

    static bool renderFails(const NodePtr& node,
                            const RectI& window)
    {
        std::vector<RenderedPlane> planes;
        std::string error;

        return !renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()),
                                       &planes, &error);
    }

    QTemporaryDir _models;
    std::unique_ptr<ModelDownloader> _downloader;
};

TEST_F(NativeNeuralDenoiseTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_NEURALDENOISE, PLUGIN_MAJOR_NATRON_NEURALDENOISE, "NeuralDenoise");
}

TEST_F(NativeNeuralDenoiseTest, KnobsListBothTasksAndLabelsMatchScriptNames)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    KnobChoicePtr model = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName("model"));
    ASSERT_TRUE(bool(model));
    std::vector<std::string> ids;
    for (const ChoiceOption& option : model->getEntries_mt_safe()) {
        ids.push_back(option.id);
    }
    EXPECT_EQ(ids, (std::vector<std::string> { kLinearModel, kSrgbModel }));
    EXPECT_EQ(model->getActiveEntry().id, kLinearModel);

    const std::pair<const char*, const char*> labels[] = {
        { kModelColourspaceParam "Index", "Model Colourspace" },
        { kNeuralDenoiseParamStrength, "Strength" },
        { kNeuralDenoiseParamPreserveWideGamut, "Preserve Wide Gamut" },
        { kNeuralDenoiseParamShadowBoost, "Shadow Boost" },
        { kNeuralDenoiseParamDetailRecovery, "Detail Recovery" },
        { kNeuralDenoiseParamDetailBand "1", "Detail Band 1" },
        { kNeuralDenoiseParamDetailBand "2", "Detail Band 2" },
        { kNeuralDenoiseParamDetailBand "3", "Detail Band 3" },
        { kNeuralDenoiseParamDetailBand "4", "Detail Band 4" },
        { kNeuralDenoiseParamDetailBand "5", "Detail Band 5" },
        { kNeuralDenoiseParamTileSize, "Tile Size" },
    };
    for (const std::pair<const char*, const char*>& expected : labels) {
        KnobIPtr knob = pair.native->getKnobByName(expected.first);
        ASSERT_TRUE(bool(knob)) << expected.first;
        EXPECT_EQ(std::string(expected.second), knob->getLabel()) << expected.first;
    }
    KnobDoublePtr strength = std::dynamic_pointer_cast<KnobDouble>(pair.native->getKnobByName(kNeuralDenoiseParamStrength));
    ASSERT_TRUE(bool(strength));
    EXPECT_EQ(1., strength->getDefaultValue(0));

    // darktable's defaults.
    KnobBoolPtr preserve = std::dynamic_pointer_cast<KnobBool>(pair.native->getKnobByName(kNeuralDenoiseParamPreserveWideGamut));
    ASSERT_TRUE(bool(preserve));
    EXPECT_TRUE(preserve->getDefaultValue(0));
    KnobChoicePtr shadowBoost = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kNeuralDenoiseParamShadowBoost));
    ASSERT_TRUE(bool(shadowBoost));
    EXPECT_EQ(std::string(kNeuralDenoiseParamShadowBoostAuto), shadowBoost->getActiveEntry().id);
    const double bands[] = { 0.25, 0.15, 0.05, 0.02, 0.01 };
    for (int b = 0; b < NeuralDenoise::kDetailBands; ++b) {
        const std::string name = kNeuralDenoiseParamDetailBand + std::to_string(b + 1);
        KnobDoublePtr band = std::dynamic_pointer_cast<KnobDouble>(pair.native->getKnobByName(name));
        ASSERT_TRUE(bool(band)) << name;
        EXPECT_DOUBLE_EQ(bands[b], band->getDefaultValue(0)) << name;
    }
}

TEST_F(NativeNeuralDenoiseTest, SrgbControlsAreEnabledForSrgbModelsOnly)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    const char* const names[] = { kNeuralDenoiseParamPreserveWideGamut, kNeuralDenoiseParamShadowBoost, kNeuralDenoiseParamDetailBand "1" };
    for (const char* name : names) {
        KnobIPtr knob = pair.native->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_FALSE(knob->isEnabled(0)) << name << " with the linear model";
    }
    ASSERT_TRUE(setKnobValue(pair.native, "model", kSrgbModel));
    for (const char* name : names) {
        EXPECT_TRUE(pair.native->getKnobByName(name)->isEnabled(0)) << name << " with the sRGB model";
    }
}

TEST_F(NativeNeuralDenoiseTest, DeepShadowsNeedATenthOfTheSamplesDark)
{
    std::vector<float> rgb(100 * 3, 0.5f);
    for (int i = 0; i < 9; ++i) {
        rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = 0.004f;
    }
    EXPECT_FALSE(NeuralDenoise::hasDeepShadows(rgb.data(), 100));
    rgb[27] = rgb[28] = rgb[29] = -1.f;
    EXPECT_TRUE(NeuralDenoise::hasDeepShadows(rgb.data(), 100));
    EXPECT_FALSE(NeuralDenoise::hasDeepShadows(rgb.data(), 0));
}

TEST_F(NativeNeuralDenoiseTest, DetailRecoveryKeepsWhatExceedsEachBandsThreshold)
{
    const int width = 8;
    const int height = 6;
    const std::size_t count = (std::size_t)width * height;
    std::array<double, NeuralDenoise::kDetailBands> bands = { 0.25, 0.15, 0.05, 0.02, 0.01 };

    // A constant residual has no deviation, so nothing is thresholded away.
    std::vector<float> original(count * 3, 0.5f);
    std::vector<float> denoised(count * 3, 0.3f);
    std::vector<float> detail = NeuralDenoise::filteredLuminanceResidual(original.data(), denoised.data(), width, height, bands);
    ASSERT_EQ(count, detail.size());
    for (float v : detail) {
        EXPECT_NEAR(0.2, v, 1e-6);
    }

    // A one-pixel checkerboard of +-1 lies wholly in the finest band, its deviation 1.
    std::fill(denoised.begin(), denoised.end(), 0.f);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float v = ((x + y) % 2) ? 1.f : -1.f;
            std::fill(&original[((std::size_t)y * width + x) * 3], &original[((std::size_t)y * width + x) * 3] + 3, v);
        }
    }
    for (double finest : { 0.25, 0.5 }) {
        bands[0] = finest;
        detail = NeuralDenoise::filteredLuminanceResidual(original.data(), denoised.data(), width, height, bands);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const double expected = (((x + y) % 2) ? 1. : -1.) * (1. - finest);
                EXPECT_NEAR(expected, detail[(std::size_t)y * width + x], 1e-5) << x << "," << y << " band 1 " << finest;
            }
        }
    }

    // A non-finite residual counts as nothing.
    original[0] = std::nanf("");
    detail = NeuralDenoise::filteredLuminanceResidual(original.data(), denoised.data(), width, height, bands);
    for (float v : detail) {
        EXPECT_TRUE(std::isfinite(v));
    }
}

TEST_F(NativeNeuralDenoiseTest, OutOfGamutPixelsKeepTheirColourAndTakeTheLocalLuminance)
{
    // One row: in, in, in, out, out, out. The last has no in-gamut pixel within 2.
    const int width = 6;
    const std::vector<unsigned char> inGamut = { 1, 1, 1, 0, 0, 0 };
    const std::vector<float> original = { 0.1f, 0.1f, 0.1f, 0.2f, 0.2f, 0.2f, 0.3f, 0.3f, 0.3f, 2.f, 0.f, 0.f, 0.f, 3.f, 0.f, 0.f, 0.f, 4.f };
    std::vector<float> denoised = { 0.15f, 0.15f, 0.15f, 0.25f, 0.25f, 0.25f, 0.35f, 0.35f, 0.35f, 9.f, 9.f, 9.f, 9.f, 9.f, 9.f, 9.f, 9.f, 9.f };
    NeuralDenoise::passThroughOutOfGamut(original.data(), inGamut, denoised.data(), width, 1);

    const auto luma = [](const float* rgb) { return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]; };
    for (int c = 0; c < 9; ++c) {
        EXPECT_FLOAT_EQ((c < 3) ? 0.15f : ((c < 6) ? 0.25f : 0.35f), denoised[c]) << c;
    }
    // Pixel 3 sees pixels 1 and 2, pixel 4 pixel 2.
    const float dY3 = (luma(&denoised[3]) + luma(&denoised[6])) / 2.f - luma(&original[9]);
    const float dY4 = luma(&denoised[6]) - luma(&original[12]);
    for (int c = 0; c < 3; ++c) {
        EXPECT_FLOAT_EQ(original[9 + c] + dY3, denoised[9 + c]) << c;
        EXPECT_FLOAT_EQ(original[12 + c] + dY4, denoised[12 + c]) << c;
        EXPECT_FLOAT_EQ(original[15 + c], denoised[15 + c]) << c;
    }
}

TEST_F(NativeNeuralDenoiseTest, ColourspaceFollowsTheModelUntilTheUserPicksOne)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    EXPECT_EQ(std::string(ModelColourspaceKnobs::kDefaultSpace), std::string(kSRGB));
    // The linear model is the default, and takes linear Rec.2020.
    EXPECT_EQ(kLinearRec2020, stringValue(pair.native, kModelColourspaceParam));

    ASSERT_TRUE(setKnobValue(pair.native, "model", kSrgbModel));
    EXPECT_EQ(std::string(kSRGB), stringValue(pair.native, kModelColourspaceParam));
    ASSERT_TRUE(setKnobValue(pair.native, "model", kLinearModel));
    EXPECT_EQ(kLinearRec2020, stringValue(pair.native, kModelColourspaceParam));

    // A space the user picked stays.
    KnobBoolPtr spaceSet = std::dynamic_pointer_cast<KnobBool>(pair.native->getKnobByName(kModelColourspaceParam "Set"));
    ASSERT_TRUE(bool(spaceSet));
    ASSERT_TRUE(setKnobValue(pair.native, kModelColourspaceParam, "ACEScct"));
    spaceSet->setValue(true);
    ASSERT_TRUE(setKnobValue(pair.native, "model", kSrgbModel));
    EXPECT_EQ(std::string("ACEScct"), stringValue(pair.native, kModelColourspaceParam));
}

TEST_F(NativeNeuralDenoiseTest, DefaultSpaceResolvesThroughTheConfig)
{
    const OCIO_NAMESPACE::ConstConfigRcPtr studio = config(getApp());
    ASSERT_TRUE(bool(studio));

    EXPECT_EQ(std::string(kSRGB), ModelColourspaceKnobs::defaultSpace(studio, AiColourSpace::eSrgb));
    EXPECT_EQ(kLinearRec2020, ModelColourspaceKnobs::defaultSpace(studio, AiColourSpace::eLinearRec2020));
    EXPECT_EQ(std::string("Linear Rec.709 (sRGB)"), ModelColourspaceKnobs::defaultSpace(studio, AiColourSpace::eLinearRec709));
    EXPECT_EQ(std::string(), ModelColourspaceKnobs::defaultSpace(studio, AiColourSpace::eCamera));
    EXPECT_EQ(std::string(), ModelColourspaceKnobs::defaultSpace(OCIO_NAMESPACE::ConstConfigRcPtr(), AiColourSpace::eSrgb));
}

TEST_F(NativeNeuralDenoiseTest, StrengthZeroIsTheInputExactly)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    // No model is installed: a strength of 0 never runs one.
    ASSERT_TRUE(setKnobOnBoth(pair, kNeuralDenoiseParamStrength, { 0. }));
    expectRender(pair.native, pair.source, [](float* /*rgb*/, float /*alpha*/) { }, "strength 0", 0.);
}

TEST_F(NativeNeuralDenoiseTest, IdentityModelRoundTripsThroughTheModelColourspace)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    // Each space converts the working space away and back again around the identity model.
    const char* const spaces[] = { kSRGB, "ACEScct", kLinearRec2020 };
    for (const char* space : spaces) {
        ASSERT_TRUE(setKnobValue(pair.native, kModelColourspaceParam, space));
        expectRender(pair.native, pair.source, [](float* /*rgb*/, float /*alpha*/) { }, space, 1e-4);
        EXPECT_EQ(std::string(), errorOf(pair.native)) << space;
    }

    // Without the wide-gamut pass-through the sRGB model round-trips too, and the identity leaves
    // detail recovery nothing to recover.
    ASSERT_TRUE(setKnobValue(pair.native, "model", kSrgbModel));
    setBool(pair.native, kNeuralDenoiseParamPreserveWideGamut, false);
    expectRender(pair.native, pair.source, [](float* /*rgb*/, float /*alpha*/) { }, "sRGB model", 1e-4);
    ASSERT_TRUE(setKnobOnBoth(pair, kNeuralDenoiseParamStrength, { 0.5 }));
    expectRender(pair.native, pair.source, [](float* /*rgb*/, float /*alpha*/) { }, "sRGB model, strength 0.5", 1e-4);
    ASSERT_TRUE(setKnobOnBoth(pair, kNeuralDenoiseParamStrength, { 0. }));
    expectRender(pair.native, pair.source, [](float* /*rgb*/, float /*alpha*/) { }, "sRGB model, strength 0", 1e-4);
}

TEST_F(NativeNeuralDenoiseTest, ShadowBoostRoundTripsAroundTheIdentityModel)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, "model", kSrgbModel));
    setBool(pair.native, kNeuralDenoiseParamPreserveWideGamut, false);

    setChoice(pair.native, kNeuralDenoiseParamShadowBoost, kNeuralDenoiseParamShadowBoostOn);
    expectRender(pair.native, pair.source, [](float* /*rgb*/, float /*alpha*/) { }, "shadow boost on", 1e-4);
    EXPECT_EQ(std::string(), errorOf(pair.native));
    // The identity model does not declare shadow_boost, so Auto leaves it off.
    setChoice(pair.native, kNeuralDenoiseParamShadowBoost, kNeuralDenoiseParamShadowBoostAuto);
    expectRender(pair.native, pair.source, [](float* /*rgb*/, float /*alpha*/) { }, "shadow boost auto", 1e-4);
}

TEST_F(NativeNeuralDenoiseTest, PreserveWideGamutPassesOutOfGamutColoursThrough)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, "model", kSrgbModel));
    NeuralDenoise* effect = effectOf(pair.native);
    ASSERT_TRUE(effect != nullptr);

    // The gamut test runs in Linear Rec.709.
    const OcioNodeKnobs& ocio = effect->colourspaceKnobs().ocio();
    const OCIO_NAMESPACE::ConstTransformRcPtr toLinear = ocio.spaceTransform(ocio.workingSpace(), "Linear Rec.709 (sRGB)");
    const OCIO_NAMESPACE::ConstCPUProcessorRcPtr linear = toLinear ? referenceProcessor(config(getApp()), toLinear) : OCIO_NAMESPACE::ConstCPUProcessorRcPtr();

    const RectI window = paritySourceWindow(pair.source, kTime, 0);
    const int width = window.width();
    const int height = window.height();
    std::vector<float> original((std::size_t)width * height * 3);
    std::vector<unsigned char> inGamut((std::size_t)width * height);
    int outside = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const std::size_t p = (std::size_t)(y - window.y1) * width + (x - window.x1);
            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                rgb[c] = original[p * 3 + c] = paritySourceColorValue(c, x, y, kTime);
            }
            if (linear) {
                linear->applyRGB(rgb);
            }
            inGamut[p] = (rgb[0] >= -0.01f) && (rgb[0] <= 1.01f) && (rgb[1] >= -0.01f) && (rgb[1] <= 1.01f) && (rgb[2] >= -0.01f) && (rgb[2] <= 1.01f);
            outside += inGamut[p] ? 0 : 1;
        }
    }
    ASSERT_GT(outside, 0);
    ASSERT_LT(outside, width * height);
    // The identity model returns the in-gamut pixels as they are.
    std::vector<float> expected(original);
    NeuralDenoise::passThroughOutOfGamut(original.data(), inGamut, expected.data(), width, height);

    RenderedPlane plane;
    ASSERT_TRUE(renderColorPlane(pair.native, window, &plane));
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const std::size_t p = (std::size_t)(y - window.y1) * width + (x - window.x1);
            for (int c = 0; c < 3; ++c) {
                ASSERT_TRUE(matches(expected[p * 3 + c], planeValue(plane, x, y, c), 1e-4))
                    << x << "," << y << " c=" << c << " in gamut " << (int)inGamut[p] << ": expected " << expected[p * 3 + c] << ", got " << planeValue(plane, x, y, c);
            }
        }
    }
}

TEST_F(NativeNeuralDenoiseTest, SrgbStrengthCropMatchesTheFullFrame)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, "model", kSrgbModel));
    // Detail recovery denoises the whole frame, so a crop matches it exactly, gamut pass included.
    ASSERT_TRUE(setKnobOnBoth(pair, kNeuralDenoiseParamStrength, { 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kNeuralDenoiseParamTileSize, { 32. }));

    const RectI full = paritySourceWindow(pair.source, kTime, 0);
    const RectI crop(full.x1 + 9, full.y1 + 5, full.x1 + 37, full.y1 + 30);
    RenderedPlane cropped;
    ASSERT_TRUE(renderColorPlane(pair.native, crop, &cropped));
    pair.native->getEffectInstance()->purgeCaches();
    RenderedPlane whole;
    ASSERT_TRUE(renderColorPlane(pair.native, full, &whole));
    ASSERT_EQ(whole.channels.size(), cropped.channels.size());
    for (int y = crop.y1; y < crop.y2; ++y) {
        for (int x = crop.x1; x < crop.x2; ++x) {
            for (std::size_t c = 0; c < whole.channels.size(); ++c) {
                ASSERT_EQ(planeValue(whole, x, y, (int)c), planeValue(cropped, x, y, (int)c)) << x << "," << y << " c=" << c;
            }
        }
    }
}

TEST_F(NativeNeuralDenoiseTest, SrgbStrengthZeroStillRunsTheModel)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, "model", kSrgbModel));

    // darktable's strength 0 for an sRGB model is full detail recovery, not the Source.
    ASSERT_TRUE(setKnobOnBoth(pair, kNeuralDenoiseParamStrength, { 0. }));
    EXPECT_TRUE(renderFails(pair.native, paritySourceWindow(pair.source, kTime, 0)));
    EXPECT_NE(errorOf(pair.native).find("not installed"), std::string::npos) << errorOf(pair.native);
}

TEST_F(NativeNeuralDenoiseTest, SmallTilesRenderTheSame)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    ASSERT_TRUE(setKnobValue(pair.native, kModelColourspaceParam, kSRGB));
    ASSERT_TRUE(setKnobOnBoth(pair, kNeuralDenoiseParamTileSize, { 32. }));
    expectRender(pair.native, pair.source, [](float* /*rgb*/, float /*alpha*/) { }, "32-pixel tiles", 1e-4);
}

TEST_F(NativeNeuralDenoiseTest, CroppedRoIMatchesTheFullFrame)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, kModelColourspaceParam, "ACEScct"));

    const RectI full = paritySourceWindow(pair.source, kTime, 0);
    RenderedPlane whole;
    ASSERT_TRUE(renderColorPlane(pair.native, full, &whole));
    const RectI crop(full.x1 + 9, full.y1 + 5, full.x1 + 37, full.y1 + 30);
    RenderedPlane cropped;
    ASSERT_TRUE(renderColorPlane(pair.native, crop, &cropped));
    ASSERT_EQ(whole.channels.size(), cropped.channels.size());
    for (int y = crop.y1; y < crop.y2; ++y) {
        for (int x = crop.x1; x < crop.x2; ++x) {
            for (std::size_t c = 0; c < whole.channels.size(); ++c) {
                ASSERT_EQ(planeValue(whole, x, y, (int)c), planeValue(cropped, x, y, (int)c)) << x << "," << y << " c=" << c;
            }
        }
    }
}

TEST_F(NativeNeuralDenoiseTest, MissingModelFailsWithAnError)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    EXPECT_TRUE(renderFails(pair.native, paritySourceWindow(pair.source, kTime, 0)));
    EXPECT_NE(errorOf(pair.native).find("not installed"), std::string::npos) << errorOf(pair.native);
}

TEST_F(NativeNeuralDenoiseTest, UnknownColourspaceFailsWithAnErrorUntilFixed)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    const RectI window = paritySourceWindow(pair.source, kTime, 0);

    ASSERT_TRUE(setKnobValue(pair.native, kModelColourspaceParam, "No Such Space"));
    EXPECT_TRUE(renderFails(pair.native, window));
    EXPECT_NE(errorOf(pair.native).find("Model Colourspace \"No Such Space\""), std::string::npos) << errorOf(pair.native);

    ASSERT_TRUE(setKnobValue(pair.native, kModelColourspaceParam, kSRGB));
    expectRender(pair.native, pair.source, [](float* /*rgb*/, float /*alpha*/) { }, "fixed", 1e-4);
    EXPECT_EQ(std::string(), errorOf(pair.native));
}

TEST_F(NativeNeuralDenoiseTest, DisabledAiFailsWithAnError)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    appPTR->getCurrentSettings()->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(false);

    EXPECT_TRUE(renderFails(pair.native, paritySourceWindow(pair.source, kTime, 0)));
    EXPECT_NE(errorOf(pair.native).find("AI is disabled"), std::string::npos) << errorOf(pair.native);
}
