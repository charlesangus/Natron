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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <list>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>

#include "Tests/Native/NativeTimeNodesTest.h"
#include "Tests/Native/NativeVectorGeneratorTest.h"
#include "Tests/Native/OcioNodeTestSupport.h"
#include "Tests/NativeParity.h"
#include "Tests/RenderBothWays.h"

#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelPackage.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/Ai/OrtLoader.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Time/VectorGenerator.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/Settings.h"
#include "Engine/ViewIdx.h"
#include "Serialization/Bundle/ZipStore.h"

NATRON_NAMESPACE_ENTER

using namespace NativeVectorGeneratorPlate;
using namespace OcioNodeTest;

namespace {
const char* const kFlowModel = "test-flow-constant";
const char* const kFlowPackage = "test-flow";
// flow-constant.onnx: dx 2.5 and dy -1.5 at every pixel, for both directions.
constexpr double kModelDx = 2.5;
constexpr double kModelDy = -1.5;
} // namespace

class NativeVectorGeneratorNeuralTest
    : public NativeTimeNodeTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerMovingTextureOnce();
        NativeTimeNodeTest::SetUp();
        getApp()->getProject()->reset(false, true);
        setConfig(getApp(), kStudioURI);

        ASSERT_TRUE(_models.isValid());
        ASSERT_TRUE(_packages.isValid());
        SettingsPtr settings = appPTR->getCurrentSettings();
        settings->getKnobByNameAndType<KnobPath>("aiOrtLibraryPath")->setValue(std::string(NATRON_TESTS_ONNXRUNTIME_DIR));
        settings->getKnobByNameAndType<KnobChoice>("aiDevice")->setValueFromID("cpu", 0);
        settings->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(true);
    }

    virtual void TearDown() OVERRIDE
    {
        // The nodes refer to the downloader, so they go first.
        getApp()->getProject()->reset(false, true);
        VectorGenerator::downloaderForTesting() = nullptr;
        _downloader.reset();
        SettingsPtr settings = appPTR->getCurrentSettings();
        settings->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(false);
        settings->getKnobByNameAndType<KnobChoice>("aiDevice")->setValueFromID("auto", 0);
        settings->getKnobByNameAndType<KnobPath>("aiOrtLibraryPath")->setValue(std::string());
        NativeTimeNodeTest::TearDown();
    }

    // Lists a flow fixture, packed as an optical-flow package, as the only model of the registry the
    // nodes created next use; installs it when asked. Returns why inference cannot run, or an empty
    // string.
    std::string useFlowModel(bool install,
                             const char* fixture = "flow-constant.onnx")
    {
        std::ifstream onnx(std::string(NATRON_TESTS_FIXTURES_DIR "/ai/") + fixture, std::ios::binary);
        const std::string model((std::istreambuf_iterator<char>(onnx)), std::istreambuf_iterator<char>());
        EXPECT_FALSE(model.empty());
        const std::string config = std::string(R"json({ "id": ")json") + kFlowPackage + R"json(", "name": "test flow", "task": "optical-flow", "backend": "onnx" })json";
        const std::string path = _packages.filePath(QStringLiteral("flow.dtmodel")).toStdString();
        {
            Serialization::ZipWriter writer(path, "application/octet-stream", kFlowPackage);
            writer.addMember("config.json", std::as_bytes(std::span<const char>(config.data(), config.size())));
            writer.addMember("model.onnx", std::as_bytes(std::span<const char>(model.data(), model.size())));
            writer.finish();
        }
        const QJsonObject entry {
            { QStringLiteral("id"), QString::fromLatin1(kFlowModel) },
            { QStringLiteral("task"), QStringLiteral("optical-flow") },
            { QStringLiteral("name"), QStringLiteral("Constant flow") },
            { QStringLiteral("package"), QString::fromLatin1(kFlowPackage) },
            { QStringLiteral("files"), QJsonArray { QStringLiteral("model.onnx") } },
            { QStringLiteral("url"), QUrl::fromLocalFile(QString::fromStdString(path)).toString() },
            { QStringLiteral("sha256"), QString::fromStdString(sha256OfFile(path)) },
            { QStringLiteral("size"), (qint64)std::filesystem::file_size(path) },
            { QStringLiteral("weightsLicence"), QStringLiteral("MIT") },
            { QStringLiteral("trainingDataLicence"), QStringLiteral("none") },
            { QStringLiteral("osaid"), QStringLiteral("Open Source AI") },
            { QStringLiteral("cardUrl"), QStringLiteral("https://example.invalid/card") },
        };
        const QJsonObject root { { QStringLiteral("schema"), 1 }, { QStringLiteral("models"), QJsonArray { entry } } };
        VectorGenerator::downloaderForTesting() = nullptr;
        _downloader.reset(new ModelDownloader(std::make_shared<const ModelRegistry>(QJsonDocument(root).toJson().toStdString(), _models.path().toStdString())));
        VectorGenerator::downloaderForTesting() = _downloader.get();
        if (!install) {
            return std::string();
        }
        std::string error;
        if (!_downloader->download(kFlowModel, &error)) {
            ADD_FAILURE() << error;

            return error;
        }
        const std::shared_ptr<const OrtLoader> ort = OrtLoader::process();

        return ort->available() ? std::string() : "ONNX Runtime is unavailable: " + ort->unavailableReason();
    }

    NodePtr createNeuralGenerator(NodePtr* sourceOut = nullptr)
    {
        NodePtr source = createNode(QString::fromUtf8(kTestPluginIDMovingTexture), 1);
        EXPECT_TRUE(bool(source));
        if (sourceOut) {
            *sourceOut = source;
        }
        NodePtr node = createOver(PLUGINID_NATRON_VECTORGENERATOR, PLUGIN_MAJOR_NATRON_VECTORGENERATOR, source);
        EXPECT_TRUE(bool(node));
        EXPECT_TRUE(node && setKnobValue(node, kVectorGeneratorParamMethod, kVectorGeneratorMethodNeural));

        return node;
    }

    bool renderPlane(const NodePtr& node,
                     const ImageLayerDesc& layer,
                     RenderedPlane* out,
                     std::string* error)
    {
        std::vector<RenderedPlane> planes;
        const bool ok = renderNodePlanesDirect(node, 3., ViewIdx(0), 0, windowAt(0), std::list<ImageLayerDesc> { layer }, &planes, error);
        if (ok && !planes.empty()) {
            *out = planes[0];
        }

        return ok && !planes.empty();
    }

    static std::string errorOf(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return (type == eMessageTypeError) ? message.toStdString() : std::string();
    }

    QTemporaryDir _models;
    QTemporaryDir _packages;
    std::unique_ptr<ModelDownloader> _downloader;
};

TEST_F(NativeVectorGeneratorNeuralTest, ForwardAndBackwardComeFromTheModel)
{
    ASSERT_EQ(std::string(), useFlowModel(true));
    NodePtr node = createNeuralGenerator();
    ASSERT_TRUE(bool(node));

    RenderedPlane forward;
    RenderedPlane backward;
    std::string error;
    ASSERT_TRUE(renderPlane(node, forwardLayer(), &forward, &error)) << error << errorOf(node);
    ASSERT_TRUE(renderPlane(node, backwardLayer(), &backward, &error)) << error << errorOf(node);
    EXPECT_LT(meanDistance(forward, kModelDx, kModelDy, kMargin), 1e-3);
    // The stand-in returns the same flow whichever way it is asked, so its backward is its forward.
    EXPECT_LT(meanDistance(backward, kModelDx, kModelDy, kMargin), 1e-3);
    EXPECT_EQ(std::string(), errorOf(node));
}

TEST_F(NativeVectorGeneratorNeuralTest, EachDirectionAsksTheModelInItsOwnOrder)
{
    ASSERT_EQ(std::string(), useFlowModel(true, "flow-order.onnx"));
    NodePtr source;
    NodePtr node = createNeuralGenerator(&source);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(bool(source));

    // The model's flow follows the second frame's brightness minus the first's. A black next frame
    // makes the forward flow negative while the backward one, towards a frame as bright as the
    // frame itself, stays near zero; a black previous frame does the opposite.
    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamBlackFrame, { 4. }));
    RenderedPlane forward;
    RenderedPlane backward;
    std::string error;
    ASSERT_TRUE(renderPlane(node, forwardLayer(), &forward, &error)) << error << errorOf(node);
    ASSERT_TRUE(renderPlane(node, backwardLayer(), &backward, &error)) << error << errorOf(node);
    EXPECT_LT(meanOfChannel(forward, 0, kMargin), -0.1);
    EXPECT_LT(meanOfChannel(forward, 1, kMargin), -0.1);
    EXPECT_LT(std::fabs(meanOfChannel(backward, 0, kMargin)), 0.25 * std::fabs(meanOfChannel(forward, 0, kMargin)));

    ASSERT_TRUE(setKnobValues(source, kMovingTextureParamBlackFrame, { 2. }));
    ASSERT_TRUE(renderPlane(node, forwardLayer(), &forward, &error)) << error << errorOf(node);
    ASSERT_TRUE(renderPlane(node, backwardLayer(), &backward, &error)) << error << errorOf(node);
    EXPECT_LT(meanOfChannel(backward, 0, kMargin), -0.1);
    EXPECT_LT(meanOfChannel(backward, 1, kMargin), -0.1);
    EXPECT_LT(std::fabs(meanOfChannel(forward, 0, kMargin)), 0.25 * std::fabs(meanOfChannel(backward, 0, kMargin)));
}

TEST_F(NativeVectorGeneratorNeuralTest, TheModelsPositionFieldReachesTheOutputPixelForPixel)
{
    ASSERT_EQ(std::string(), useFlowModel(true, "flow-position.onnx"));
    NodePtr node = createNeuralGenerator();
    ASSERT_TRUE(bool(node));

    RenderedPlane forward;
    std::string error;
    ASSERT_TRUE(renderPlane(node, forwardLayer(), &forward, &error)) << error << errorOf(node);
    const int width = forward.window.width();
    const int height = forward.window.height();
    const std::size_t nComps = forward.channels.size();
    for (int y = kMargin; y < height - kMargin; ++y) {
        for (int x = kMargin; x < width - kMargin; ++x) {
            const float* pixel = &forward.pixels[((std::size_t)y * width + x) * nComps];
            ASSERT_NEAR((float)x, pixel[0], 1e-3f) << x << "," << y;
            ASSERT_NEAR((float)y, pixel[1], 1e-3f) << x << "," << y;
        }
    }
}

TEST_F(NativeVectorGeneratorNeuralTest, ConfidenceIsTheRoundTripOfTheModelsTwoDirections)
{
    ASSERT_EQ(std::string(), useFlowModel(true));
    NodePtr node = createNeuralGenerator();
    ASSERT_TRUE(bool(node));

    RenderedPlane confidence;
    std::string error;
    ASSERT_TRUE(renderPlane(node, VectorGenerator::confidenceLayer(), &confidence, &error)) << error << errorOf(node);
    // The two directions are the same vector rather than opposite ones, so the round trip is twice
    // it: exp(-|2 * (2.5, -1.5)|^2 / sigma^2) with sigma 2.
    const double expected = std::exp(-(4. * (kModelDx * kModelDx + kModelDy * kModelDy)) / 4.);
    EXPECT_NEAR(meanOfChannel(confidence, 0, kMargin), expected, 1e-4);
    EXPECT_NEAR(meanOfChannel(confidence, 1, kMargin), expected, 1e-4);
}

TEST_F(NativeVectorGeneratorNeuralTest, AMissingModelIsAnErrorThatPointsToTheModelManager)
{
    useFlowModel(false);
    NodePtr node = createNeuralGenerator();
    ASSERT_TRUE(bool(node));

    RenderedPlane forward;
    std::string error;
    EXPECT_FALSE(renderPlane(node, forwardLayer(), &forward, &error));
    const std::string message = errorOf(node);
    EXPECT_NE(message.find("not installed"), std::string::npos) << message;
    EXPECT_NE(message.find("Preferences"), std::string::npos) << message;
    EXPECT_NE(message.find(kFlowModel), std::string::npos) << message;
}

TEST_F(NativeVectorGeneratorNeuralTest, DisabledAiIsAnErrorAndNeverFallsBackToAClassicalMethod)
{
    useFlowModel(false);
    NodePtr node = createNeuralGenerator();
    ASSERT_TRUE(bool(node));
    appPTR->getCurrentSettings()->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(false);

    RenderedPlane forward;
    std::string error;
    EXPECT_FALSE(renderPlane(node, forwardLayer(), &forward, &error));
    EXPECT_NE(errorOf(node).find("AI is disabled"), std::string::npos) << errorOf(node);
    KnobChoicePtr method = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kVectorGeneratorParamMethod));
    ASSERT_TRUE(bool(method));
    EXPECT_EQ(method->getValue(), (int)eFlowMethodNeural);
}

TEST_F(NativeVectorGeneratorNeuralTest, NeuralEndsAGpuRegion)
{
    useFlowModel(false);
    NodePtr node = createNeuralGenerator();
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);

    GpuQuery query;
    query.time = 3.;
    query.view = ViewIdx(0);
    query.mappedScale = RenderScale::identity;
    query.plane = forwardLayer();
    query.nComps = 2;
    query.bitDepth = eImageBitDepthFloat;
    query.hasMask = false;
    EXPECT_FALSE(effect->canRenderOnGpu(query));
}

TEST_F(NativeVectorGeneratorNeuralTest, NeuralKnobsAreShownOnlyForTheNeuralMethod)
{
    useFlowModel(false);
    NodePtr node = createNeuralGenerator();
    ASSERT_TRUE(bool(node));
    const char* const names[] = { "model", "downloadModel", kModelColourspaceParam };
    for (const char* name : names) {
        KnobIPtr knob = node->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_FALSE(knob->getIsSecret()) << name;
    }
    ASSERT_TRUE(setKnobValue(node, kVectorGeneratorParamMethod, kVectorGeneratorMethodDIS));
    for (const char* name : names) {
        EXPECT_TRUE(node->getKnobByName(name)->getIsSecret()) << name;
    }
}

NATRON_NAMESPACE_EXIT
