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

#include "AiModelTestEffect.h"
#include "BaseTest.h"
#include "RenderBothWays.h"

#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelKnob.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {

// Written by tools/ai/make-test-dtmodel.py.
const char* const kFixturePackage = "test-identity";
const char* const kFixtureSha256 = "eb95dd0d3cfecc9a713799210f0f9fdcb29b1745a8adbf846ddc1b7ad20a92b8";
const int kFixtureSize = 602;
// Two denoise models of the fixture package; neither is marked default, so the first is.
const char* const kModelA = "test-identity";
const char* const kModelB = "test-identity-b";

QJsonObject
fixtureEntry(const char* id,
             const char* name)
{
    return QJsonObject {
        { QStringLiteral("id"), QString::fromLatin1(id) },
        { QStringLiteral("task"), QStringLiteral("denoise") },
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

class ModelKnobTest
    : public BaseTest {
protected:
    void SetUp() override
    {
        BaseTest::SetUp();
        ASSERT_TRUE(_models.isValid());
        const QJsonArray models { fixtureEntry(kModelA, "Identity A"), fixtureEntry(kModelB, "Identity B") };
        const QJsonObject root { { QStringLiteral("schema"), 1 }, { QStringLiteral("models"), models } };
        _downloader.reset(new ModelDownloader(std::make_shared<const ModelRegistry>(QJsonDocument(root).toJson().toStdString(),
                                                                                    _models.path().toStdString())));
        AiModelTestEffect::downloader() = _downloader.get();
        setAiEnabled(true);
    }

    void TearDown() override
    {
        // The nodes refer to the downloader, so they go first.
        if (getApp()) {
            getApp()->getProject()->reset(false, true);
        }
        AiModelTestEffect::downloader() = nullptr;
        _downloader.reset();
        setAiEnabled(false);
        appPTR->getCurrentSettings()->setAiDefaultModel(AiModelTask::eDenoise, std::string());
        BaseTest::TearDown();
    }

    static void setAiEnabled(bool enabled)
    {
        appPTR->getCurrentSettings()->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(enabled);
    }

    NodePtr createModelNode()
    {
        return createNode(QString::fromUtf8(kTestPluginIDAiModel));
    }

    static ModelKnob* modelKnobOf(const NodePtr& node)
    {
        AiModelTestEffect* effect = dynamic_cast<AiModelTestEffect*>(node->getEffectInstance().get());

        return effect ? effect->modelKnob() : nullptr;
    }

    static bool render(const NodePtr& node,
                       std::string* error)
    {
        const RectI roi(0, 0, kAiModelTestSize, kAiModelTestSize);
        std::vector<RenderedPlane> planes;

        return renderNodePlanesDirect(node, 0., ViewIdx(0), 0, roi, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()),
                                      &planes, error);
    }

    static std::string persistentError(const NodePtr& node)
    {
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);

        return (type == eMessageTypeError) ? message.toStdString() : std::string();
    }

    QTemporaryDir _models;
    std::unique_ptr<ModelDownloader> _downloader;
};

} // namespace

TEST_F(ModelKnobTest, KnobsListTheTaskModelsAndDefaultToTheTaskDefault)
{
    NodePtr node = createModelNode();
    ASSERT_TRUE(bool(node));
    ModelKnob* modelKnob = modelKnobOf(node);
    ASSERT_NE(modelKnob, nullptr);

    KnobChoicePtr model = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName("model"));
    ASSERT_TRUE(bool(model));
    EXPECT_EQ(model->getLabel(), "Model");
    const std::vector<ChoiceOption> entries = model->getEntries_mt_safe();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].id, kModelA);
    EXPECT_EQ(entries[0].label, "Identity A (not installed)");
    EXPECT_EQ(entries[1].id, kModelB);
    EXPECT_EQ(modelKnob->modelId(), kModelA);

    KnobIPtr download = node->getKnobByName("downloadModel");
    ASSERT_TRUE(bool(download));
    EXPECT_EQ(download->getLabel(), "Download Model");
    EXPECT_TRUE(bool(std::dynamic_pointer_cast<KnobButton>(download)));
}

TEST_F(ModelKnobTest, MissingModelSetsTheErrorAndInstallingClearsItOnTheNextRender)
{
    NodePtr node = createModelNode();
    ASSERT_TRUE(bool(node));

    std::string error;
    EXPECT_FALSE(render(node, &error));
    const std::string missing = persistentError(node);
    EXPECT_NE(missing.find("not installed"), std::string::npos) << missing;
    EXPECT_NE(missing.find(kModelA), std::string::npos) << missing;

    ASSERT_TRUE(_downloader->download(kModelA, &error)) << error;
    // The install refreshes the entries before anything renders.
    KnobChoicePtr model = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName("model"));
    ASSERT_TRUE(bool(model));
    EXPECT_EQ(model->getEntries_mt_safe()[0].label, "Identity A");

    EXPECT_TRUE(render(node, &error)) << error;
    EXPECT_EQ(persistentError(node), std::string());

    std::optional<ModelKnob::Resolved> resolved = modelKnobOf(node)->resolve();
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->entry->id, kModelA);
    ASSERT_EQ(resolved->files.size(), 1u);
    EXPECT_NE(resolved->files[0].find("model.onnx"), std::string::npos);
}

TEST_F(ModelKnobTest, DeletingTheModelBringsTheErrorBack)
{
    NodePtr node = createModelNode();
    ASSERT_TRUE(bool(node));
    std::string error;
    ASSERT_TRUE(_downloader->download(kModelA, &error)) << error;
    EXPECT_TRUE(render(node, &error)) << error;

    ASSERT_TRUE(_downloader->remove(kModelA, &error)) << error;
    EXPECT_FALSE(render(node, &error));
    EXPECT_NE(persistentError(node).find("not installed"), std::string::npos) << persistentError(node);
}

TEST_F(ModelKnobTest, DisabledAiSetsTheError)
{
    NodePtr node = createModelNode();
    ASSERT_TRUE(bool(node));
    std::string error;
    ASSERT_TRUE(_downloader->download(kModelA, &error)) << error;

    setAiEnabled(false);
    EXPECT_FALSE(render(node, &error));
    EXPECT_NE(persistentError(node).find("AI is disabled"), std::string::npos) << persistentError(node);

    setAiEnabled(true);
    EXPECT_TRUE(render(node, &error)) << error;
    EXPECT_EQ(persistentError(node), std::string());
}

TEST_F(ModelKnobTest, UnknownIdIsKeptAndNamedInTheError)
{
    NodePtr node = createModelNode();
    ASSERT_TRUE(bool(node));
    KnobChoicePtr model = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName("model"));
    ASSERT_TRUE(bool(model));

    // What a project naming a model the manifest no longer lists restores.
    model->setActiveEntry(ChoiceOption("retired-model"));
    EXPECT_EQ(modelKnobOf(node)->modelId(), "retired-model");

    std::string error;
    EXPECT_FALSE(render(node, &error));
    EXPECT_NE(persistentError(node).find("unknown model `retired-model`"), std::string::npos) << persistentError(node);
}

TEST_F(ModelKnobTest, NewNodesStartOnTheUserDefault)
{
    appPTR->getCurrentSettings()->setAiDefaultModel(AiModelTask::eDenoise, kModelB);
    EXPECT_EQ(ModelKnob::defaultModel(*_downloader->registry(), AiModelTask::eDenoise)->id, kModelB);

    NodePtr node = createModelNode();
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(modelKnobOf(node)->modelId(), kModelB);

    // The stored default must name a model of the task to count.
    appPTR->getCurrentSettings()->setAiDefaultModel(AiModelTask::eDenoise, "not-a-model");
    EXPECT_EQ(ModelKnob::defaultModel(*_downloader->registry(), AiModelTask::eDenoise)->id, kModelA);
}
