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
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <list>
#include <memory>
#include <optional>
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
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/OCIO/ModelColourspace.h"
#include "Engine/Nodes/Transform/NeuralUpscale.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
using namespace OcioNodeTest;

namespace {

// Written by tools/ai/make-test-dtmodel.py: upscale2-nearest.onnx and upscale4-nearest.onnx,
// which repeat each pixel 2 or 4 times along each axis.
const char* const kFixturePackage = "test-upscale";
const char* const kFixtureSha256 = "68d24c04d52412e5af8d7bf4ec5da451e01534c9e0878016cb40890203d9e2b3";
const int kFixtureSize = 1105;
const char* const kModel2x = "test-upscale-x2";
const char* const kModel4x = "test-upscale-x4";

// The models the gated cases run, from the built-in manifest.
const char* const kRealModel2x = "upscale-realplksr-x2";

QJsonObject
fixtureEntry(const char* id,
             const char* name,
             const char* task,
             const char* file)
{
    return QJsonObject {
        { QStringLiteral("id"), QString::fromLatin1(id) },
        { QStringLiteral("task"), QString::fromLatin1(task) },
        { QStringLiteral("name"), QString::fromLatin1(name) },
        { QStringLiteral("package"), QString::fromLatin1(kFixturePackage) },
        { QStringLiteral("files"), QJsonArray { QString::fromLatin1(file) } },
        { QStringLiteral("url"), QUrl::fromLocalFile(QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/ai/upscale.dtmodel")).toString() },
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

int
floorDiv(int a,
         int b)
{
    return (int)std::floor((double)a / b);
}

} // namespace

class NativeNeuralUpscaleTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
        setConfig(getApp(), kStudioURI);

        ASSERT_TRUE(_models.isValid());
        const QJsonArray models { fixtureEntry(kModel2x, "Nearest 2x", "upscale-2x", "model_x2.onnx"),
                                  fixtureEntry(kModel4x, "Nearest 4x", "upscale-4x", "model_x4.onnx") };
        const QJsonObject root { { QStringLiteral("schema"), 1 }, { QStringLiteral("models"), models } };
        _downloader.reset(new ModelDownloader(std::make_shared<const ModelRegistry>(QJsonDocument(root).toJson().toStdString(),
                                                                                    _models.path().toStdString())));
        NeuralUpscale::downloaderForTesting() = _downloader.get();

        SettingsPtr settings = appPTR->getCurrentSettings();
        settings->getKnobByNameAndType<KnobPath>("aiOrtLibraryPath")->setValue(std::string(NATRON_TESTS_ONNXRUNTIME_DIR));
        settings->getKnobByNameAndType<KnobChoice>("aiDevice")->setValueFromID("cpu", 0);
        settings->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(true);
    }

    virtual void TearDown() OVERRIDE
    {
        // The nodes refer to the downloaders, so they go first.
        getApp()->getProject()->reset(false, true);
        NeuralUpscale::downloaderForTesting() = nullptr;
        _downloader.reset();
        _realDownloader.reset();
        SettingsPtr settings = appPTR->getCurrentSettings();
        settings->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(false);
        settings->getKnobByNameAndType<KnobChoice>("aiDevice")->setValueFromID("auto", 0);
        settings->getKnobByNameAndType<KnobPath>("aiOrtLibraryPath")->setValue(std::string());
        BaseTest::TearDown();
    }

    ParityPair makePair()
    {
        ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_NEURALUPSCALE, -1, PLUGIN_MAJOR_NATRON_NEURALUPSCALE);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(pair.native && effectOf(pair.native));

        return pair;
    }

    // Installs the fixture package, which both models share. Returns why inference cannot run,
    // or an empty string.
    std::string install()
    {
        std::string error;
        if (!_downloader->download(kModel2x, &error)) {
            ADD_FAILURE() << error;

            return error;
        }

        return ortUnavailable();
    }

    static std::string ortUnavailable()
    {
        const std::shared_ptr<const OrtLoader> ort = OrtLoader::process();

        return ort->available() ? std::string() : "ONNX Runtime is unavailable: " + ort->unavailableReason();
    }

    /**
     * @brief Makes the next node use the built-in manifest over NATRON_TEST_AI_MODELS_DIR, or the
     * default models directory. Returns why the real 2x model cannot run, or an empty string.
     **/
    std::string useRealModels()
    {
        const char* env = std::getenv("NATRON_TEST_AI_MODELS_DIR");
        const std::string dir = (env && *env) ? std::string(env) : ModelRegistry::defaultModelsDirectory();
        std::shared_ptr<const ModelRegistry> registry = std::make_shared<const ModelRegistry>(ModelRegistry::builtinManifest(), dir);
        if (registry->resolve(kRealModel2x).status != AiModelStatus::eInstalled) {
            return std::string(kRealModel2x) + " is not installed in " + dir + " (set NATRON_TEST_AI_MODELS_DIR to the models directory)";
        }
        const std::string ort = ortUnavailable();
        if (!ort.empty()) {
            return ort;
        }
        _realDownloader.reset(new ModelDownloader(registry));
        NeuralUpscale::downloaderForTesting() = _realDownloader.get();

        return std::string();
    }

    static NeuralUpscale* effectOf(const NodePtr& node)
    {
        return node ? dynamic_cast<NeuralUpscale*>(node->getEffectInstance().get()) : nullptr;
    }

    static std::string errorOf(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return (type == eMessageTypeError) ? message.toStdString() : std::string();
    }

    static std::string choiceId(const NodePtr& node,
                                const char* name)
    {
        KnobChoicePtr knob = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(name));
        EXPECT_TRUE(bool(knob)) << name;

        return knob ? knob->getActiveEntry().id : std::string();
    }

    static void processChannels(const NodePtr& node,
                                const std::vector<std::string>& channels)
    {
        KnobChannelSetPtr knob = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));
        ASSERT_TRUE(bool(knob));
        knob->setChannels(0, channels);
    }

    // The RGBA plane of node over window at mipmapLevel.
    static bool renderRGBA(const NodePtr& node,
                           unsigned mipmapLevel,
                           const RectI& window,
                           RenderedPlane* plane)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()),
                                               &planes, &error);
        EXPECT_TRUE(ok) << error;
        if (!ok || (planes.size() != 1)) {
            return false;
        }
        *plane = planes[0];

        return true;
    }

    static RectD regionOfDefinition(const NodePtr& node,
                                    unsigned mipmapLevel)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        RectD rod;
        bool isProjectFormat = false;
        EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod, &isProjectFormat));

        return rod;
    }

    static RectI scaled(const RectI& r,
                        int f)
    {
        return RectI(r.x1 * f, r.y1 * f, r.x2 * f, r.y2 * f);
    }

    // Checks that every RGB pixel of node at mipmapLevel repeats the Source's pixel under it.
    void expectNearest(const ParityPair& pair,
                       int f,
                       unsigned mipmapLevel,
                       const std::string& what)
    {
        const RectI sourceWindow = paritySourceWindow(pair.source, kTime, mipmapLevel);
        RenderedPlane source;
        ASSERT_TRUE(renderRGBA(pair.source, mipmapLevel, sourceWindow, &source)) << what;
        const RectI window = scaled(sourceWindow, f);
        RenderedPlane out;
        ASSERT_TRUE(renderRGBA(pair.native, mipmapLevel, window, &out)) << what;
        EXPECT_EQ(window, out.window) << what;
        ASSERT_EQ(4u, out.channels.size()) << what;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float expected = planeValue(source, floorDiv(x, f), floorDiv(y, f), c);
                    ASSERT_TRUE(matches(expected, planeValue(out, x, y, c), 1e-4))
                        << what << " " << x << "," << y << " c=" << c << ": expected " << expected << ", got " << planeValue(out, x, y, c);
                }
            }
        }
        EXPECT_EQ(std::string(), errorOf(pair.native)) << what;
    }

    QTemporaryDir _models;
    std::unique_ptr<ModelDownloader> _downloader;
    std::unique_ptr<ModelDownloader> _realDownloader;
};

TEST_F(NativeNeuralUpscaleTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_NEURALUPSCALE, PLUGIN_MAJOR_NATRON_NEURALUPSCALE, "NeuralUpscale");
}

TEST_F(NativeNeuralUpscaleTest, KnobsListBothFactorsAndLabelsMatchScriptNames)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    KnobChoicePtr scale = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kNeuralUpscaleParamScale));
    ASSERT_TRUE(bool(scale));
    std::vector<std::string> ids;
    for (const ChoiceOption& option : scale->getEntries_mt_safe()) {
        ids.push_back(option.id);
    }
    EXPECT_EQ(ids, (std::vector<std::string> { kNeuralUpscaleParamScale2x, kNeuralUpscaleParamScale4x }));
    EXPECT_EQ(std::string(kNeuralUpscaleParamScale2x), scale->getActiveEntry().id);

    KnobChoicePtr model = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName("model"));
    ASSERT_TRUE(bool(model));
    ids.clear();
    for (const ChoiceOption& option : model->getEntries_mt_safe()) {
        ids.push_back(option.id);
    }
    EXPECT_EQ(ids, (std::vector<std::string> { kModel2x, kModel4x }));
    EXPECT_EQ(std::string(kModel2x), model->getActiveEntry().id);

    KnobChoicePtr alphaFilter = std::dynamic_pointer_cast<KnobChoice>(pair.native->getKnobByName(kNeuralUpscaleParamAlphaFilter));
    ASSERT_TRUE(bool(alphaFilter));
    ids.clear();
    for (const ChoiceOption& option : alphaFilter->getEntries_mt_safe()) {
        ids.push_back(option.id);
    }
    EXPECT_EQ(ids, (std::vector<std::string> { kNeuralUpscaleParamAlphaFilterCubic, kNeuralUpscaleParamAlphaFilterLanczos }));
    EXPECT_EQ(std::string(kNeuralUpscaleParamAlphaFilterCubic), alphaFilter->getActiveEntry().id);

    const std::pair<const char*, const char*> labels[] = {
        { kNeuralUpscaleParamScale, "Scale" },
        { "model", "Model" },
        { kModelColourspaceParam "Index", "Model Colourspace" },
        { kNeuralUpscaleParamAlphaFilter, "Alpha Filter" },
        { kNeuralUpscaleParamTileSize, "Tile Size" },
    };
    for (const std::pair<const char*, const char*>& expected : labels) {
        KnobIPtr knob = pair.native->getKnobByName(expected.first);
        ASSERT_TRUE(bool(knob)) << expected.first;
        EXPECT_EQ(std::string(expected.second), knob->getLabel()) << expected.first;
    }
    // The upscale models were trained on sRGB.
    EXPECT_EQ(std::string(kSRGB), stringValue(pair.native, kModelColourspaceParam));
}

TEST_F(NativeNeuralUpscaleTest, ScaleAndModelFollowEachOther)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    ASSERT_TRUE(setKnobValue(pair.native, kNeuralUpscaleParamScale, kNeuralUpscaleParamScale4x));
    EXPECT_EQ(std::string(kModel4x), choiceId(pair.native, "model"));
    EXPECT_EQ(4, effectOf(pair.native)->factor());

    ASSERT_TRUE(setKnobValue(pair.native, "model", kModel2x));
    EXPECT_EQ(std::string(kNeuralUpscaleParamScale2x), choiceId(pair.native, kNeuralUpscaleParamScale));
    EXPECT_EQ(2, effectOf(pair.native)->factor());
}

TEST_F(NativeNeuralUpscaleTest, FormatScalesAndKeepsThePixelAspect)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    NodeMetadataConstPtr source = pair.source->getEffectInstance()->getTimeInvariantMetadata();
    ASSERT_TRUE(bool(source));
    const RectI sourceFormat = source->getOutputFormat();
    ASSERT_FALSE(sourceFormat.isNull());
    for (int f : { 2, 4 }) {
        ASSERT_TRUE(setKnobValue(pair.native, kNeuralUpscaleParamScale, (f == 2) ? kNeuralUpscaleParamScale2x : kNeuralUpscaleParamScale4x));
        NodeMetadataConstPtr metadata = pair.native->getEffectInstance()->getTimeInvariantMetadata();
        ASSERT_TRUE(bool(metadata));
        EXPECT_EQ(scaled(sourceFormat, f), metadata->getOutputFormat()) << f;
        EXPECT_EQ(source->getPixelAspectRatio(-1), metadata->getPixelAspectRatio(-1)) << f;
        EXPECT_EQ(std::optional<int>(f), effectOf(pair.native)->getOutputMetadata(kTime, ViewIdx(0)).getInt(kNeuralUpscaleMetadataFactor)) << f;
    }
}

TEST_F(NativeNeuralUpscaleTest, RegionOfDefinitionScalesAboutTheOrigin)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    setParitySourceOrigin(pair.source, 8, -4);

    for (int f : { 2, 4 }) {
        ASSERT_TRUE(setKnobValue(pair.native, kNeuralUpscaleParamScale, (f == 2) ? kNeuralUpscaleParamScale2x : kNeuralUpscaleParamScale4x));
        // The canonical region is the same at every render scale.
        for (unsigned level = 0; level <= 1; ++level) {
            EXPECT_EQ(RectD(8. * f, -4. * f, (8. + kParitySourceWidth) * f, (-4. + kParitySourceHeight) * f), regionOfDefinition(pair.native, level))
                << f << "x, mipmap " << level;
        }
    }
}

TEST_F(NativeNeuralUpscaleTest, RegionOfInterestMapsBackThroughTheFactorPlusTheOverlap)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    NeuralUpscale* effect = effectOf(pair.native);
    ASSERT_TRUE(effect != nullptr);
    const EffectInstancePtr source = pair.source->getEffectInstance();

    const RectD window(20., 30., 100., 70.);
    for (int f : { 2, 4 }) {
        ASSERT_TRUE(setKnobValue(pair.native, kNeuralUpscaleParamScale, (f == 2) ? kNeuralUpscaleParamScale2x : kNeuralUpscaleParamScale4x));
        for (unsigned level = 0; level <= 1; ++level) {
            const RenderScale scale = RenderScale::fromMipmapLevel(level);
            RoIMap rois;
            effect->getRegionsOfInterest_public(kTime, scale, window, window, ViewIdx(0), &rois);
            RoIMap::const_iterator found = rois.find(source);
            ASSERT_TRUE(found != rois.end()) << f << "x, mipmap " << level;
            // The overlap is 16 Source pixels at the render scale.
            const double overlap = NeuralUpscale::kOverlap * (1 << level);
            const RectD expected(window.x1 / f - overlap, window.y1 / f - overlap, window.x2 / f + overlap, window.y2 / f + overlap);
            EXPECT_EQ(expected, found->second) << f << "x, mipmap " << level;
        }
    }
}

TEST_F(NativeNeuralUpscaleTest, NeverConcatenatesWithTransforms)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    EXPECT_FALSE(pair.native->getEffectInstance()->getCanTransform());
}

TEST_F(NativeNeuralUpscaleTest, NearestModelRepeatsEachSourcePixel)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    expectNearest(pair, 2, 0, "2x");
    ASSERT_TRUE(setKnobValue(pair.native, kNeuralUpscaleParamScale, kNeuralUpscaleParamScale4x));
    expectNearest(pair, 4, 0, "4x");
}

TEST_F(NativeNeuralUpscaleTest, ProxyRendersAtTheRenderScaleTimesTheFactor)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    NodeMetadataConstPtr metadata = pair.native->getEffectInstance()->getTimeInvariantMetadata();
    ASSERT_TRUE(bool(metadata));
    const RectI format = metadata->getOutputFormat();

    // At mipmap 1 the output is the half-size Source enlarged, half the full-size output.
    for (int f : { 2, 4 }) {
        ASSERT_TRUE(setKnobValue(pair.native, kNeuralUpscaleParamScale, (f == 2) ? kNeuralUpscaleParamScale2x : kNeuralUpscaleParamScale4x));
        const double par = pair.native->getEffectInstance()->getTimeInvariantMetadata()->getPixelAspectRatio(-1);
        EXPECT_EQ(scaled(paritySourceWindow(pair.source, kTime, 1), f), regionOfDefinition(pair.native, 1).toPixelEnclosing(1, par)) << f;
        expectNearest(pair, f, 1, std::to_string(f) + "x at mipmap 1");
    }
    ASSERT_TRUE(setKnobValue(pair.native, kNeuralUpscaleParamScale, kNeuralUpscaleParamScale2x));
    // Rendering at a render scale leaves the canonical format alone.
    EXPECT_EQ(format, pair.native->getEffectInstance()->getTimeInvariantMetadata()->getOutputFormat());
}

TEST_F(NativeNeuralUpscaleTest, CroppedWindowMatchesTheFullFrame)
{
    const std::string unavailable = install();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, kModelColourspaceParam, "ACEScct"));
    ASSERT_TRUE(setKnobOnBoth(pair, kNeuralUpscaleParamAlphaFilter, kNeuralUpscaleParamAlphaFilterLanczos));

    const RectI full = scaled(paritySourceWindow(pair.source, kTime, 0), 2);
    RenderedPlane whole;
    ASSERT_TRUE(renderRGBA(pair.native, 0, full, &whole));
    // Odd edges, so the crop starts and ends inside a Source pixel.
    const RectI crop(full.x1 + 19, full.y1 + 11, full.x1 + 75, full.y1 + 61);
    RenderedPlane cropped;
    ASSERT_TRUE(renderRGBA(pair.native, 0, crop, &cropped));
    ASSERT_EQ(whole.channels.size(), cropped.channels.size());
    for (int y = crop.y1; y < crop.y2; ++y) {
        for (int x = crop.x1; x < crop.x2; ++x) {
            for (std::size_t c = 0; c < whole.channels.size(); ++c) {
                ASSERT_TRUE(matches(planeValue(whole, x, y, (int)c), planeValue(cropped, x, y, (int)c), 1e-6))
                    << x << "," << y << " c=" << c << ": " << planeValue(whole, x, y, (int)c) << " vs " << planeValue(cropped, x, y, (int)c);
            }
        }
    }
}

TEST_F(NativeNeuralUpscaleTest, AlphaFollowsTheChosenFilter)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    // With RGB unchecked no model runs, and every channel goes through the alpha filter.
    processChannels(pair.native, { "A" });

    const RectI sourceWindow = paritySourceWindow(pair.source, kTime, 0);
    RenderedPlane source;
    ASSERT_TRUE(renderRGBA(pair.source, 0, sourceWindow, &source));
    const Resampler::SourceImage image(source.pixels.data(), source.window, 4);

    for (int f : { 2, 4 }) {
        ASSERT_TRUE(setKnobValue(pair.native, kNeuralUpscaleParamScale, (f == 2) ? kNeuralUpscaleParamScale2x : kNeuralUpscaleParamScale4x));
        const RectI window = scaled(sourceWindow, f);

        ASSERT_TRUE(setKnobOnBoth(pair, kNeuralUpscaleParamAlphaFilter, kNeuralUpscaleParamAlphaFilterCubic));
        RenderedPlane cubic;
        ASSERT_TRUE(renderRGBA(pair.native, 0, window, &cubic));
        float cubicMin = 1.f;
        float cubicMax = 0.f;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                float expected[4];
                ASSERT_TRUE(Resampler::interpolate(Resampler::eFilterCubic, false, (x + 0.5) / f, (y + 0.5) / f, image, false, expected));
                for (int c = 0; c < 4; ++c) {
                    ASSERT_NEAR(expected[c], planeValue(cubic, x, y, c), 1e-5) << f << "x " << x << "," << y << " c=" << c;
                }
                cubicMin = std::min(cubicMin, planeValue(cubic, x, y, 3));
                cubicMax = std::max(cubicMax, planeValue(cubic, x, y, 3));
            }
        }
        EXPECT_GE(cubicMin, 0.f) << f;
        EXPECT_LE(cubicMax, 1.f) << f;

        // Lanczos-3 rings at the edges of the Source's exact 0 and 1 alpha bands.
        ASSERT_TRUE(setKnobOnBoth(pair, kNeuralUpscaleParamAlphaFilter, kNeuralUpscaleParamAlphaFilterLanczos));
        RenderedPlane lanczos;
        ASSERT_TRUE(renderRGBA(pair.native, 0, window, &lanczos));
        float lanczosMin = 1.f;
        float lanczosMax = 0.f;
        double difference = 0.;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                lanczosMin = std::min(lanczosMin, planeValue(lanczos, x, y, 3));
                lanczosMax = std::max(lanczosMax, planeValue(lanczos, x, y, 3));
                difference = std::max(difference, (double)std::fabs(planeValue(lanczos, x, y, 3) - planeValue(cubic, x, y, 3)));
            }
        }
        EXPECT_LT(lanczosMin, -1e-3) << f;
        EXPECT_GT(lanczosMax, 1. + 1e-3) << f;
        EXPECT_GT(difference, 1e-2) << f;
    }
}

TEST_F(NativeNeuralUpscaleTest, LanczosKernel)
{
    EXPECT_DOUBLE_EQ(1., NeuralUpscale::lanczos3(0.));
    EXPECT_NEAR(0., NeuralUpscale::lanczos3(1.), 1e-12);
    EXPECT_NEAR(0., NeuralUpscale::lanczos3(-2.), 1e-12);
    EXPECT_EQ(0., NeuralUpscale::lanczos3(3.));
    EXPECT_EQ(0., NeuralUpscale::lanczos3(-4.5));
    // sinc(0.5) sinc(1/6).
    EXPECT_NEAR((2. / M_PI) * (std::sin(M_PI / 6.) * 6. / M_PI), NeuralUpscale::lanczos3(0.5), 1e-12);
    EXPECT_LT(NeuralUpscale::lanczos3(1.5), 0.);
}

TEST_F(NativeNeuralUpscaleTest, LargeOutputsWarn)
{
    // darktable's 60 megapixels, with no cache limit.
    EXPECT_TRUE(NeuralUpscale::largeOutputWarning(RectI(0, 0, 8000, 7499), 4, 0).empty());
    EXPECT_FALSE(NeuralUpscale::largeOutputWarning(RectI(0, 0, 8000, 7500), 4, 0).empty());
    EXPECT_TRUE(NeuralUpscale::largeOutputWarning(RectI(0, 0, 1000, 1000), 4, 0).empty());
    // A million RGBA float pixels is 16 MB.
    EXPECT_TRUE(NeuralUpscale::largeOutputWarning(RectI(0, 0, 1000, 1000), 4, 16000000).empty());
    const std::string overCache = NeuralUpscale::largeOutputWarning(RectI(0, 0, 1000, 1000), 4, 15999999);
    EXPECT_NE(std::string::npos, overCache.find("cache")) << overCache;
    EXPECT_TRUE(NeuralUpscale::largeOutputWarning(RectI(), 4, 1).empty());
}

TEST_F(NativeNeuralUpscaleTest, MissingModelFailsWithAnError)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    std::vector<RenderedPlane> planes;
    std::string error;
    EXPECT_FALSE(renderNodePlanesDirect(pair.native, kTime, ViewIdx(0), 0, scaled(paritySourceWindow(pair.source, kTime, 0), 2),
                                        std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error));
    EXPECT_NE(errorOf(pair.native).find("not installed"), std::string::npos) << errorOf(pair.native);
}

TEST_F(NativeNeuralUpscaleTest, RealModelBeatsCubicInPsnr)
{
    const std::string unavailable = useRealModels();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, "model", kRealModel2x));
    ASSERT_EQ(2, effectOf(pair.native)->factor());
    // Keeps the Source's -0.25 to 1.75 within the model's range.
    ASSERT_TRUE(setKnobValue(pair.native, kModelColourspaceParam, "ACEScct"));

    // The half-size Source enlarged 2x lands on the full-size Source's pixels.
    const RectI original = paritySourceWindow(pair.source, kTime, 0);
    RenderedPlane full;
    ASSERT_TRUE(renderRGBA(pair.source, 0, original, &full));
    RenderedPlane half;
    ASSERT_TRUE(renderRGBA(pair.source, 1, paritySourceWindow(pair.source, kTime, 1), &half));
    RenderedPlane upscaled;
    ASSERT_TRUE(renderRGBA(pair.native, 1, original, &upscaled));
    EXPECT_EQ(std::string(), errorOf(pair.native));

    const Resampler::SourceImage image(half.pixels.data(), half.window, 4);
    double modelError = 0.;
    double cubicError = 0.;
    for (int y = original.y1; y < original.y2; ++y) {
        for (int x = original.x1; x < original.x2; ++x) {
            float cubic[4];
            ASSERT_TRUE(Resampler::interpolate(Resampler::eFilterCubic, false, (x + 0.5) / 2., (y + 0.5) / 2., image, false, cubic));
            for (int c = 0; c < 3; ++c) {
                const double truth = planeValue(full, x, y, c);
                modelError += std::pow(planeValue(upscaled, x, y, c) - truth, 2.);
                cubicError += std::pow(cubic[c] - truth, 2.);
            }
        }
    }
    const double count = 3. * original.width() * original.height();
    // Peak 2, the Source's range.
    const double modelPsnr = 10. * std::log10(4. / (modelError / count));
    const double cubicPsnr = 10. * std::log10(4. / (cubicError / count));
    std::cerr << "PSNR: model " << modelPsnr << " dB, cubic " << cubicPsnr << " dB" << std::endl;
    EXPECT_GT(modelPsnr, cubicPsnr);
}

TEST_F(NativeNeuralUpscaleTest, RealModelKeepsHighlightsAboveOne)
{
    const std::string unavailable = useRealModels();
    if (!unavailable.empty()) {
        reportSkip(unavailable);

        return;
    }
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobValue(pair.native, "model", kRealModel2x));
    // Nothing wraps the range: a log Model Colourspace is what keeps highlights in the model's.
    ASSERT_TRUE(setKnobValue(pair.native, kModelColourspaceParam, "ACEScct"));

    const RectI original = paritySourceWindow(pair.source, kTime, 0);
    RenderedPlane full;
    ASSERT_TRUE(renderRGBA(pair.source, 0, original, &full));
    RenderedPlane upscaled;
    ASSERT_TRUE(renderRGBA(pair.native, 1, original, &upscaled));
    EXPECT_EQ(std::string(), errorOf(pair.native));

    double sourceSum = 0.;
    double upscaledSum = 0.;
    int count = 0;
    for (int y = original.y1; y < original.y2; ++y) {
        for (int x = original.x1; x < original.x2; ++x) {
            for (int c = 0; c < 3; ++c) {
                if (planeValue(full, x, y, c) > 1.f) {
                    sourceSum += planeValue(full, x, y, c);
                    upscaledSum += planeValue(upscaled, x, y, c);
                    ++count;
                }
            }
        }
    }
    ASSERT_GT(count, 0);
    std::cerr << "Highlights: " << count << " samples, mean " << sourceSum / count << " in, " << upscaledSum / count << " out" << std::endl;
    EXPECT_NEAR(1., upscaledSum / sourceSum, 0.02);
    EXPECT_GT(upscaledSum / count, 1.);
}
