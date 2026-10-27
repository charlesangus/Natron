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
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelPackage.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Serialization/Bundle/ZipStore.h"

NATRON_NAMESPACE_USING

namespace {

// Written by tools/ai/make-test-dtmodel.py.
const char* const kFixturePackage = "test-identity";
const char* const kFixtureSha256 = "eb95dd0d3cfecc9a713799210f0f9fdcb29b1745a8adbf846ddc1b7ad20a92b8";
const int kFixtureSize = 602;

// darktable-ai release-5.6.0's rawdenoise-nind.dtmodel config.json, model card trimmed.
const char* const kRawDenoiseConfig = R"json({
    "id": "rawdenoise-nind",
    "name": "raw denoise nind",
    "task": "rawdenoise",
    "arch": "utnet2",
    "backend": "onnx",
    "version": "1.0",
    "tiling": true,
    "model_card": { "license": "GPL-3.0", "training_data_license": "CC BY 4.0 / CC0 (per-image, Wikimedia Commons)" },
    "attributes": {
        "input_sizes": [ 512 ],
        "model_bayer": { "input_kind": "bayer_v1", "bayer_orientation": "force_rggb", "edge_pad": "mirror_cropped", "wb_norm": "none", "output_scale": "match_gain" },
        "model_linear": { "input_kind": "linear_v1", "input_colorspace": "lin_rec2020", "wb_norm": "none", "output_scale": "match_gain", "target_mean": null }
    },
    "coreml_format": "mlprogram"
})json";

// darktable-ai release-5.6.0's upscale-realplksr.dtmodel config.json, model card trimmed.
const char* const kUpscaleConfig = R"json({
    "id": "upscale-realplksr", "name": "upscale realplksr", "task": "upscale", "arch": "plksr",
    "backend": "onnx", "version": "1.0", "tiling": true,
    "model_card": { "license": "MIT" },
    "attributes": { "model_x2": { "input_sizes": [ 512 ] }, "model_x4": { "input_sizes": [ 256 ] } }
})json";

std::string
fixturePath(const char* name)
{
    return std::string(NATRON_TESTS_FIXTURES_DIR "/ai/") + name;
}

std::string
readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);

    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

QJsonObject
fixtureEntry(const std::string& id,
             const std::string& archive)
{
    return QJsonObject {
        { QStringLiteral("id"), QString::fromStdString(id) },
        { QStringLiteral("task"), QStringLiteral("denoise") },
        { QStringLiteral("name"), QString::fromStdString(id) },
        { QStringLiteral("package"), QString::fromLatin1(kFixturePackage) },
        { QStringLiteral("files"), QJsonArray { QStringLiteral("model.onnx") } },
        { QStringLiteral("url"), QUrl::fromLocalFile(QString::fromStdString(archive)).toString() },
        { QStringLiteral("sha256"), QString::fromLatin1(kFixtureSha256) },
        { QStringLiteral("size"), kFixtureSize },
        { QStringLiteral("weightsLicence"), QStringLiteral("MIT") },
        { QStringLiteral("trainingDataLicence"), QStringLiteral("none") },
        { QStringLiteral("osaid"), QStringLiteral("Open Source AI") },
        { QStringLiteral("cardUrl"), QStringLiteral("https://example.invalid/card") },
    };
}

std::shared_ptr<const ModelRegistry>
fixtureRegistry(const std::string& modelsDirectory,
                const std::string& archive,
                const std::vector<std::string>& ids = { kFixturePackage })
{
    QJsonArray models;
    for (const std::string& id : ids) {
        models.append(fixtureEntry(id, archive));
    }
    const QJsonObject root { { QStringLiteral("schema"), 1 }, { QStringLiteral("models"), models } };

    return std::make_shared<const ModelRegistry>(QJsonDocument(root).toJson().toStdString(), modelsDirectory);
}

// Nothing but the installed packages, if any, under modelsDirectory.
std::vector<std::string>
entries(const std::filesystem::path& directory)
{
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        names.push_back(entry.path().filename().string());
    }

    return names;
}

void
writeZip(const std::filesystem::path& path,
         std::string_view root,
         const std::map<std::string, std::string>& members)
{
    Serialization::ZipWriter writer(path.string(), "application/octet-stream", root);
    for (const auto& member : members) {
        writer.addMember(member.first, std::as_bytes(std::span<const char>(member.second.data(), member.second.size())));
    }
    writer.finish();
}

} // anon namespace

TEST(AiModelPackage, ParsesVariantAttributes)
{
    const ModelConfig bayer = parseModelConfig(kRawDenoiseConfig, "model_bayer.onnx");
    EXPECT_EQ(bayer.id, "rawdenoise-nind");
    EXPECT_EQ(bayer.task, "rawdenoise");
    EXPECT_TRUE(bayer.tiling);
    EXPECT_EQ(bayer.variant, "model_bayer");
    EXPECT_EQ(bayer.inputSizes, (std::vector<int> { 512 }));
    EXPECT_EQ(bayer.tileSize(), 512);
    EXPECT_EQ(bayer.colourSpace, AiColourSpace::eCamera);
    EXPECT_EQ(bayer.inputKind, "bayer_v1");
    EXPECT_EQ(bayer.weightsLicence, "GPL-3.0");
    EXPECT_NE(bayer.attributesJson.find("\"edge_pad\":\"mirror_cropped\""), std::string::npos);
    EXPECT_EQ(bayer.attributesJson.find("model_linear"), std::string::npos);

    const ModelConfig linear = parseModelConfig(kRawDenoiseConfig, "model_linear.onnx");
    EXPECT_EQ(linear.colourSpace, AiColourSpace::eLinearRec2020);
    EXPECT_EQ(linear.tileSize(), 512);

    EXPECT_EQ(parseModelConfig(kUpscaleConfig, "model_x2.onnx").tileSize(), 512);
    const ModelConfig x4 = parseModelConfig(kUpscaleConfig, "model_x4.onnx");
    EXPECT_EQ(x4.tileSize(), 256);
    EXPECT_EQ(x4.colourSpace, AiColourSpace::eSrgb);

    const ModelConfig mask = parseModelConfig(R"({ "id": "m", "backend": "onnx", "task": "mask", "tiling": false })", "encoder.onnx");
    EXPECT_FALSE(mask.tiling);
    EXPECT_EQ(mask.tileSize(), 0);
}

TEST(AiModelPackage, RefusesBadConfigs)
{
    EXPECT_THROW(parseModelConfig("[]", "model.onnx"), ModelPackageError);
    EXPECT_THROW(parseModelConfig(R"({ "id": "m", "backend": "coreml" })", "model.onnx"), ModelPackageError);
    EXPECT_THROW(parseModelConfig(R"({ "id": "m", "backend": "onnx", "attributes": { "input_colorspace": "aces" } })", "model.onnx"),
                 ModelPackageError);
    EXPECT_THROW(parseModelConfig(R"({ "id": "m", "backend": "onnx", "attributes": { "input_sizes": [ 0 ] } })", "model.onnx"),
                 ModelPackageError);
}

TEST(AiModelPackage, UnpacksStoredMembersAndRefusesStrayOnes)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    const std::string config = R"({ "id": "pkg", "backend": "onnx" })";

    writeZip(dir / "good.dtmodel", "pkg", { { "config.json", config }, { "sub/model.onnx", "onnx" } });
    unpackModelPackage((dir / "good.dtmodel").string(), "pkg", (dir / "good").string());
    EXPECT_EQ(readFile(dir / "good" / "config.json"), config);
    EXPECT_EQ(readFile(dir / "good" / "sub" / "model.onnx"), "onnx");

    // ZipWriter puts its mimetype member at the top level when given no folder.
    writeZip(dir / "stray.dtmodel", std::string_view(), { { "pkg/config.json", config } });
    EXPECT_THROW(unpackModelPackage((dir / "stray.dtmodel").string(), "pkg", (dir / "stray").string()), ModelPackageError);

    writeZip(dir / "noconfig.dtmodel", "pkg", { { "model.onnx", "onnx" } });
    EXPECT_THROW(unpackModelPackage((dir / "noconfig.dtmodel").string(), "pkg", (dir / "noconfig").string()), ModelPackageError);

    std::ofstream(dir / "garbage.dtmodel") << "not a zip";
    EXPECT_THROW(unpackModelPackage((dir / "garbage.dtmodel").string(), "pkg", (dir / "garbage").string()), ModelPackageError);
}

TEST(AiModelDownloader, InstallsFixturePackage)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path models = std::filesystem::path(tmp.path().toStdString()) / "models";
    ModelDownloader downloader(fixtureRegistry(models.string(), fixturePath("identity.dtmodel")));

    qint64 lastReceived = -1;
    QObject::connect(&downloader, &ModelDownloader::progress, [&](const QString&, qint64 received, qint64) { lastReceived = received; });
    std::string error;
    ASSERT_TRUE(downloader.download(kFixturePackage, &error)) << error;
    EXPECT_FALSE(downloader.isDownloading(kFixturePackage));
    EXPECT_EQ(lastReceived, kFixtureSize);

    const AiModelResolution resolution = downloader.registry()->resolve(kFixturePackage);
    ASSERT_EQ(resolution.status, AiModelStatus::eInstalled);
    ASSERT_EQ(resolution.files.size(), 1u);
    EXPECT_EQ(readFile(resolution.files.front()), readFile(fixturePath("identity.onnx")));
    EXPECT_EQ(entries(models), (std::vector<std::string> { kFixturePackage }));

    const ModelConfig config = readModelConfig(resolution.directory, "model.onnx");
    EXPECT_EQ(config.id, kFixturePackage);
    EXPECT_EQ(config.task, "denoise");
    EXPECT_TRUE(config.tiling);
    EXPECT_EQ(config.tileSize(), 64);
    EXPECT_EQ(config.colourSpace, AiColourSpace::eSrgb);
    EXPECT_EQ(config.weightsLicence, "MIT");

    // A second download replaces the install in place.
    ASSERT_TRUE(downloader.download(kFixturePackage, &error)) << error;
    EXPECT_EQ(downloader.registry()->resolve(kFixturePackage).status, AiModelStatus::eInstalled);
    EXPECT_EQ(entries(models), (std::vector<std::string> { kFixturePackage }));
}

TEST(AiModelDownloader, Sha256MismatchInstallsNothing)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    const std::filesystem::path corrupt = dir / "corrupt.dtmodel";
    std::string bytes = readFile(fixturePath("identity.dtmodel"));
    ASSERT_EQ(bytes.size(), static_cast<std::size_t>(kFixtureSize));
    bytes[bytes.size() / 2] ^= 0x5a;
    std::ofstream(corrupt, std::ios::binary) << bytes;

    const std::filesystem::path models = dir / "models";
    ModelDownloader downloader(fixtureRegistry(models.string(), corrupt.string()));
    std::string error;
    EXPECT_FALSE(downloader.download(kFixturePackage, &error));
    EXPECT_NE(error.find(kFixturePackage), std::string::npos) << error;
    EXPECT_NE(error.find("sha256"), std::string::npos) << error;
    EXPECT_EQ(downloader.registry()->resolve(kFixturePackage).status, AiModelStatus::eMissing);
    EXPECT_TRUE(entries(models).empty());
}

TEST(AiModelDownloader, MissingSourceFailsCleanly)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdString());
    const std::filesystem::path models = dir / "models";
    ModelDownloader downloader(fixtureRegistry(models.string(), (dir / "absent.dtmodel").string()));

    std::string error;
    EXPECT_FALSE(downloader.download(kFixturePackage, &error));
    EXPECT_NE(error.find(kFixturePackage), std::string::npos) << error;
    EXPECT_TRUE(entries(models).empty());

    EXPECT_FALSE(downloader.start("no-such-model", &error));
    EXPECT_NE(error.find("unknown model"), std::string::npos) << error;
}

TEST(AiModelDownloader, ModelsOfOnePackageShareItsDownload)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path models = std::filesystem::path(tmp.path().toStdString()) / "models";
    ModelDownloader downloader(fixtureRegistry(models.string(), fixturePath("identity.dtmodel"), { "first", "second" }));

    std::map<std::string, int> finished;
    QEventLoop loop;
    QObject::connect(&downloader, &ModelDownloader::finished, [&](const QString& id, bool ok, const QString& error) {
        EXPECT_TRUE(ok) << error.toStdString();
        ++finished[id.toStdString()];
        if (finished.size() == 2) {
            loop.quit();
        }
    });
    ASSERT_TRUE(downloader.start("first"));
    ASSERT_TRUE(downloader.isDownloading("second"));
    ASSERT_TRUE(downloader.start("second"));
    ASSERT_TRUE(downloader.start("first"));
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();

    EXPECT_EQ(finished, (std::map<std::string, int> { { "first", 1 }, { "second", 1 } }));
    EXPECT_EQ(downloader.registry()->resolve("first").status, AiModelStatus::eInstalled);
    EXPECT_EQ(downloader.registry()->resolve("second").status, AiModelStatus::eInstalled);
    EXPECT_EQ(entries(models), (std::vector<std::string> { kFixturePackage }));
}
