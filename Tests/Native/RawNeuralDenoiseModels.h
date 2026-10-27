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

#ifndef NATRON_TESTS_NATIVE_RAWNEURALDENOISEMODELS_H
#define NATRON_TESTS_NATIVE_RAWNEURALDENOISEMODELS_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QUrl>

#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/AppManager.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Raw/RawNeuralDenoise.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A model manager over the two models of Tests/fixtures/ai/rawdenoise.dtmodel, in a
 * temporary folder, that RawNeuralDenoise nodes and the Develop RAW chain use while it lives, with
 * AI enabled on the CPU through the build tree's ONNX Runtime. Nothing is installed until
 * install() is called. Create it after the test application exists; destroy it after the nodes.
 **/
class RawNeuralDenoiseModels {
public:
    // Written by tools/ai/make-test-rawdenoise.py.
    static constexpr const char* kSha256 = "e0668f9d568ec01f273fcbfc1b4c01777e22c1d2a1b27ea4f738e73c63aa5536";
    static constexpr int kSize = 1228;
    // The default: times 1000 under match_gain.
    static constexpr const char* kMatchGain = "test-rawdenoise";
    static constexpr const char* kAbsolute = "test-rawdenoise-absolute";

    RawNeuralDenoiseModels()
    {
        const QJsonArray models { entry(kMatchGain, "Raw Denoise Match Gain", "model_bayer.onnx"),
                                  entry(kAbsolute, "Raw Denoise Absolute", "model_bayer_absolute.onnx") };
        const QJsonObject root { { QStringLiteral("schema"), 1 }, { QStringLiteral("models"), models } };
        _downloader.reset(new ModelDownloader(std::make_shared<const ModelRegistry>(QJsonDocument(root).toJson().toStdString(),
                                                                                    _directory.path().toStdString())));
        RawNeuralDenoise::setModelDownloaderForTests(_downloader.get());
        SettingsPtr settings = appPTR->getCurrentSettings();
        settings->getKnobByNameAndType<KnobPath>("aiOrtLibraryPath")->setValue(std::string(NATRON_TESTS_ONNXRUNTIME_DIR));
        settings->getKnobByNameAndType<KnobChoice>("aiDevice")->setValue((int)eAiDeviceCpu);
        settings->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(true);
    }

    ~RawNeuralDenoiseModels()
    {
        RawNeuralDenoise::setModelDownloaderForTests(nullptr);
        SettingsPtr settings = appPTR->getCurrentSettings();
        settings->getKnobByNameAndType<KnobBool>("aiEnabled")->setValue(false);
        settings->getKnobByNameAndType<KnobChoice>("aiDevice")->setValue(0);
        settings->getKnobByNameAndType<KnobPath>("aiOrtLibraryPath")->setValue(std::string());
        settings->setAiDefaultModel(AiModelTask::eDenoiseRawBayer, std::string());
    }

    bool valid() const
    {
        return _directory.isValid();
    }

    // Installs the package, which holds both models.
    bool install(std::string* error)
    {
        return _downloader->download(kMatchGain, error);
    }

    bool installed(const char* id) const
    {
        return _downloader->registry()->resolve(id).status == AiModelStatus::eInstalled;
    }

private:
    static QJsonObject entry(const char* id,
                             const char* name,
                             const char* file)
    {
        return QJsonObject {
            { QStringLiteral("id"), QString::fromLatin1(id) },
            { QStringLiteral("task"), QStringLiteral("denoise-raw-bayer") },
            { QStringLiteral("name"), QString::fromLatin1(name) },
            { QStringLiteral("package"), QStringLiteral("test-rawdenoise") },
            { QStringLiteral("files"), QJsonArray { QString::fromLatin1(file) } },
            { QStringLiteral("url"), QUrl::fromLocalFile(QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/ai/rawdenoise.dtmodel")).toString() },
            { QStringLiteral("sha256"), QString::fromLatin1(kSha256) },
            { QStringLiteral("size"), kSize },
            { QStringLiteral("weightsLicence"), QStringLiteral("MIT") },
            { QStringLiteral("trainingDataLicence"), QStringLiteral("none") },
            { QStringLiteral("osaid"), QStringLiteral("Open Source AI") },
            { QStringLiteral("cardUrl"), QStringLiteral("https://example.invalid/card") },
        };
    }

    QTemporaryDir _directory;
    std::unique_ptr<ModelDownloader> _downloader;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVE_RAWNEURALDENOISEMODELS_H
