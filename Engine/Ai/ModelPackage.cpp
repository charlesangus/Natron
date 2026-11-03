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

#include "ModelPackage.h"

#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include "Serialization/Bundle/ZipStore.h"

NATRON_NAMESPACE_ENTER

namespace {

std::string
stringMember(const QJsonObject& object,
             const char* key)
{
    return object.value(QString::fromLatin1(key)).toString().toStdString();
}

AiColourSpace
colourSpace(const QJsonObject& attributes,
            const std::string& where)
{
    if (attributes.value(QStringLiteral("input_kind")).toString() == QLatin1String("bayer_v1")) {
        return AiColourSpace::eCamera;
    }
    const QJsonValue value = attributes.value(QStringLiteral("input_colorspace"));
    if (value.isUndefined() || value.isNull()) {
        return AiColourSpace::eSrgb;
    }
    const QString name = value.toString();
    if (name == QLatin1String("srgb")) {
        return AiColourSpace::eSrgb;
    }
    if (name == QLatin1String("lin_rec709")) {
        return AiColourSpace::eLinearRec709;
    }
    if (name == QLatin1String("lin_rec2020")) {
        return AiColourSpace::eLinearRec2020;
    }
    if (name == QLatin1String("camera")) {
        return AiColourSpace::eCamera;
    }
    throw ModelPackageError(where + ": unknown input_colorspace \"" + name.toStdString() + "\"");
}

} // anon namespace

ModelConfig
parseModelConfig(const std::string& json,
                 const std::string& variantFile)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &parseError);
    if ((parseError.error != QJsonParseError::NoError) || !document.isObject()) {
        throw ModelPackageError("config.json is not a JSON object: " + parseError.errorString().toStdString());
    }
    const QJsonObject root = document.object();

    ModelConfig config;
    config.id = stringMember(root, "id");
    const std::string where = "config.json of \"" + config.id + "\"";
    if (config.id.empty()) {
        throw ModelPackageError("config.json has no \"id\"");
    }
    const std::string backend = stringMember(root, "backend");
    if (backend != "onnx") {
        throw ModelPackageError(where + ": backend \"" + backend + "\" is not \"onnx\"");
    }
    config.name = stringMember(root, "name");
    config.description = stringMember(root, "description");
    config.task = stringMember(root, "task");
    config.arch = stringMember(root, "arch");
    config.version = stringMember(root, "version");
    config.tiling = root.value(QStringLiteral("tiling")).toBool(false);
    config.numInputs = root.value(QStringLiteral("num_inputs")).toInt(1);

    const QJsonObject card = root.value(QStringLiteral("model_card")).toObject();
    config.weightsLicence = stringMember(card, "license");
    config.trainingDataLicence = stringMember(card, "training_data_license");

    config.variant = std::filesystem::path(variantFile).stem().string();
    const QJsonObject packageAttributes = root.value(QStringLiteral("attributes")).toObject();
    QJsonObject attributes;
    for (auto it = packageAttributes.begin(); it != packageAttributes.end(); ++it) {
        // Objects are other models' attribute blocks.
        if (!it.value().isObject()) {
            attributes.insert(it.key(), it.value());
        }
    }
    const QJsonObject own = packageAttributes.value(QString::fromStdString(config.variant)).toObject();
    for (auto it = own.begin(); it != own.end(); ++it) {
        attributes.insert(it.key(), it.value());
    }

    const QJsonValue inputSizes = attributes.value(QStringLiteral("input_sizes"));
    if (!inputSizes.isUndefined()) {
        if (!inputSizes.isArray()) {
            throw ModelPackageError(where + ": input_sizes is not an array");
        }
        for (const QJsonValue& size : inputSizes.toArray()) {
            const int s = size.toInt(0);
            if (s <= 0) {
                throw ModelPackageError(where + ": input_sizes holds a size that is not a positive integer");
            }
            config.inputSizes.push_back(s);
        }
    }
    config.colourSpace = colourSpace(attributes, where);
    config.inputKind = stringMember(attributes, "input_kind");
    config.attributesJson = QJsonDocument(attributes).toJson(QJsonDocument::Compact).toStdString();

    return config;
}

ModelConfig
readModelConfig(const std::string& packageDirectory,
                const std::string& variantFile)
{
    const std::filesystem::path path = std::filesystem::path(packageDirectory) / "config.json";
    QFile file(QString::fromStdString(path.string()));
    if (!file.open(QIODevice::ReadOnly)) {
        throw ModelPackageError("cannot read " + path.string() + ": " + file.errorString().toStdString());
    }

    return parseModelConfig(file.readAll().toStdString(), variantFile);
}

void
unpackModelPackage(const std::string& archive,
                   const std::string& package,
                   const std::string& destination)
{
    const std::string prefix = package + "/";
    const std::filesystem::path root(destination);
    try {
        std::vector<std::string> names;
        {
            const Serialization::ZipReader reader(archive);
            for (const Serialization::ZipMember& member : reader.members()) {
                names.push_back(member.name);
            }
        }
        bool hasConfig = false;
        for (const std::string& name : names) {
            const std::string problem = Serialization::zipMemberNameProblem(name);
            if (!problem.empty()) {
                throw ModelPackageError("member \"" + name + "\": " + problem);
            }
            if ((name.compare(0, prefix.size(), prefix) != 0) || (name.size() == prefix.size())) {
                throw ModelPackageError("member \"" + name + "\" is outside the package folder \"" + package + "\"");
            }
            hasConfig = hasConfig || (name == prefix + "config.json");
        }
        if (!hasConfig) {
            throw ModelPackageError("there is no " + prefix + "config.json");
        }

        std::error_code ec;
        if (std::filesystem::exists(root, ec)) {
            throw ModelPackageError(destination + " already exists");
        }
        std::filesystem::create_directories(root);
        for (const std::string& name : names) {
            const std::filesystem::path path = root / name.substr(prefix.size());
            std::filesystem::create_directories(path.parent_path());
            // A reader per member, so each inflated member is freed before the next one is read.
            const Serialization::ZipReader reader(archive);
            const std::span<const std::byte> data = reader.data(name);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            out.close();
            if (!out) {
                throw ModelPackageError("cannot write " + path.string());
            }
        }
    } catch (const Serialization::ZipError& e) {
        throw ModelPackageError(std::string("not a valid .dtmodel: ") + e.what());
    } catch (const std::filesystem::filesystem_error& e) {
        throw ModelPackageError(e.what());
    }
}

std::string
sha256OfFile(const std::string& path)
{
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        throw ModelPackageError("cannot read " + path + ": " + file.errorString().toStdString());
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        throw ModelPackageError("cannot read " + path + ": " + file.errorString().toStdString());
    }

    return hash.result().toHex().toStdString();
}

NATRON_NAMESPACE_EXIT
