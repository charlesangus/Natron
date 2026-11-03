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

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1Char>
#include <QString>
#include <QTemporaryDir>

#include "Engine/Ai/ModelRegistry.h"

NATRON_NAMESPACE_USING

namespace {

void
touch(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "x";
}

QJsonObject
validEntry()
{
    return QJsonObject {
        { QStringLiteral("id"), QStringLiteral("a") },
        { QStringLiteral("task"), QStringLiteral("denoise") },
        { QStringLiteral("name"), QStringLiteral("A") },
        { QStringLiteral("package"), QStringLiteral("pa") },
        { QStringLiteral("files"), QJsonArray { QStringLiteral("model.onnx") } },
        { QStringLiteral("url"), QStringLiteral("file:///nowhere/pa.dtmodel") },
        { QStringLiteral("sha256"), QString(64, QLatin1Char('0')) },
        { QStringLiteral("size"), 1 },
        { QStringLiteral("weightsLicence"), QStringLiteral("MIT") },
        { QStringLiteral("trainingDataLicence"), QStringLiteral("CC0") },
        { QStringLiteral("osaid"), QStringLiteral("Open Source AI") },
        { QStringLiteral("cardUrl"), QStringLiteral("https://example.invalid/card") },
    };
}

std::string
manifest(const QJsonArray& models,
         int schema = 1)
{
    return QJsonDocument(QJsonObject { { QStringLiteral("schema"), schema }, { QStringLiteral("models"), models } }).toJson().toStdString();
}

std::string
manifestWith(const char* key,
             const QJsonValue& value)
{
    QJsonObject entry = validEntry();
    entry.insert(QString::fromLatin1(key), value);

    return manifest(QJsonArray { entry });
}

} // anon namespace

TEST(AiModelRegistry, BuiltinManifestParses)
{
    const ModelRegistry registry(ModelRegistry::builtinManifest(), "/nonexistent");

    EXPECT_EQ(registry.sourceTag(), "release-5.6.0");
    ASSERT_EQ(registry.models().size(), 12u);
    for (const AiModelEntry& entry : registry.models()) {
        SCOPED_TRACE(entry.id);
        EXPECT_EQ(entry.url, "https://github.com/darktable-org/darktable-ai/releases/download/release-5.6.0/" + entry.package + ".dtmodel");
        EXPECT_EQ(entry.sha256.size(), 64u);
        EXPECT_GT(entry.size, 0u);
        EXPECT_FALSE(entry.weightsLicence.empty());
        EXPECT_FALSE(entry.trainingDataLicence.empty());
        EXPECT_FALSE(entry.osaid.empty());
        EXPECT_FALSE(entry.files.empty());
    }
}

TEST(AiModelRegistry, ByTask)
{
    const ModelRegistry registry(ModelRegistry::builtinManifest(), "/nonexistent");

    EXPECT_EQ(registry.byTask(AiModelTask::eMaskObject),
              (std::vector<std::string> { "mask-object-segnext-b2hq", "mask-object-sam21-tiny", "mask-object-sam21-small", "mask-object-sam21-base-plus" }));
    EXPECT_EQ(registry.byTask(AiModelTask::eDenoise), (std::vector<std::string> { "denoise-nind", "denoise-nafnet" }));
    EXPECT_EQ(registry.byTask(AiModelTask::eDenoiseRawBayer), (std::vector<std::string> { "rawdenoise-nind-bayer" }));
    EXPECT_EQ(registry.byTask(AiModelTask::eDenoiseLinear), (std::vector<std::string> { "rawdenoise-nind-linear" }));
    EXPECT_EQ(registry.byTask(AiModelTask::eUpscale2x), (std::vector<std::string> { "upscale-realplksr-x2", "upscale-bsrgan-x2" }));
    EXPECT_EQ(registry.byTask(AiModelTask::eUpscale4x), (std::vector<std::string> { "upscale-realplksr-x4", "upscale-bsrgan-x4" }));

    const AiModelEntry* mask = registry.defaultFor(AiModelTask::eMaskObject);
    ASSERT_TRUE(mask != nullptr);
    EXPECT_EQ(mask->id, "mask-object-segnext-b2hq");
    const AiModelEntry* upscale = registry.defaultFor(AiModelTask::eUpscale4x);
    ASSERT_TRUE(upscale != nullptr);
    EXPECT_EQ(upscale->id, "upscale-realplksr-x4");
}

TEST(AiModelRegistry, TaskNamesRoundTrip)
{
    for (AiModelTask task : { AiModelTask::eMaskObject, AiModelTask::eDenoise, AiModelTask::eDenoiseRawBayer,
                              AiModelTask::eDenoiseLinear, AiModelTask::eUpscale2x, AiModelTask::eUpscale4x }) {
        const std::optional<AiModelTask> back = aiModelTaskFromName(aiModelTaskName(task));
        ASSERT_TRUE(back.has_value());
        EXPECT_EQ(*back, task);
    }
    EXPECT_FALSE(aiModelTaskFromName("mask").has_value());
}

TEST(AiModelRegistry, PackageSharedByVariants)
{
    const ModelRegistry registry(ModelRegistry::builtinManifest(), "/nonexistent");
    const AiModelEntry* bayer = registry.find("rawdenoise-nind-bayer");
    ASSERT_TRUE(bayer != nullptr);

    std::set<std::string> ids;
    for (const AiModelEntry* entry : registry.packageModels(*bayer)) {
        ids.insert(entry->id);
    }
    EXPECT_EQ(ids, (std::set<std::string> { "rawdenoise-nind-bayer", "rawdenoise-nind-linear" }));
}

TEST(AiModelRegistry, ResolvesInstalledMissingAndUnknown)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path models(tmp.path().toStdString());
    const ModelRegistry registry(ModelRegistry::builtinManifest(), models.string());

    const std::filesystem::path package = models / "upscale-realplksr";
    touch(package / "config.json");
    touch(package / "model_x2.onnx");

    const AiModelResolution installed = registry.resolve("upscale-realplksr-x2");
    EXPECT_EQ(installed.status, AiModelStatus::eInstalled);
    ASSERT_TRUE(installed.entry != nullptr);
    EXPECT_EQ(installed.directory, package.string());
    EXPECT_EQ(installed.files, (std::vector<std::string> { (package / "model_x2.onnx").string() }));

    // Same package, but its own file is absent.
    const AiModelResolution partial = registry.resolve("upscale-realplksr-x4");
    EXPECT_EQ(partial.status, AiModelStatus::eMissing);
    EXPECT_EQ(partial.directory, package.string());

    const AiModelResolution missing = registry.resolve("mask-object-segnext-b2hq");
    EXPECT_EQ(missing.status, AiModelStatus::eMissing);
    ASSERT_TRUE(missing.entry != nullptr);
    EXPECT_EQ(missing.files.size(), 2u);

    const AiModelResolution unknown = registry.resolve("no-such-model");
    EXPECT_EQ(unknown.status, AiModelStatus::eUnknown);
    EXPECT_TRUE(unknown.entry == nullptr);
}

TEST(AiModelRegistry, RefusesMalformedManifests)
{
    EXPECT_NO_THROW(ModelRegistry(manifest(QJsonArray { validEntry() }), "/nonexistent"));
    EXPECT_THROW(ModelRegistry("not json", "/nonexistent"), ModelRegistryError);
    EXPECT_THROW(ModelRegistry(manifest(QJsonArray { validEntry() }, 2), "/nonexistent"), ModelRegistryError);
    EXPECT_THROW(ModelRegistry(manifestWith("task", QStringLiteral("mask")), "/nonexistent"), ModelRegistryError);
    EXPECT_THROW(ModelRegistry(manifestWith("sha256", QStringLiteral("abc")), "/nonexistent"), ModelRegistryError);
    EXPECT_THROW(ModelRegistry(manifestWith("files", QJsonArray { QStringLiteral("../escape.onnx") }), "/nonexistent"), ModelRegistryError);
    EXPECT_THROW(ModelRegistry(manifestWith("package", QStringLiteral("a/b")), "/nonexistent"), ModelRegistryError);
    EXPECT_THROW(ModelRegistry(manifestWith("url", QJsonValue()), "/nonexistent"), ModelRegistryError);
    EXPECT_THROW(ModelRegistry(manifest(QJsonArray { validEntry(), validEntry() }), "/nonexistent"), ModelRegistryError);

    QJsonObject other = validEntry();
    other.insert(QStringLiteral("id"), QStringLiteral("b"));
    other.insert(QStringLiteral("size"), 2);
    EXPECT_THROW(ModelRegistry(manifest(QJsonArray { validEntry(), other }), "/nonexistent"), ModelRegistryError);
}
