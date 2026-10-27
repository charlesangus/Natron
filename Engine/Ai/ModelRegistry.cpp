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

#include "ModelRegistry.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <system_error>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

#include "Engine/StandardPaths.h"

NATRON_NAMESPACE_ENTER

namespace {

const struct
{
    AiModelTask task;
    const char* name;
} kTaskNames[] = {
    { AiModelTask::eMaskObject, "mask-object" },
    { AiModelTask::eDenoise, "denoise" },
    { AiModelTask::eDenoiseRawBayer, "denoise-raw-bayer" },
    { AiModelTask::eDenoiseLinear, "denoise-linear" },
    { AiModelTask::eUpscale2x, "upscale-2x" },
    { AiModelTask::eUpscale4x, "upscale-4x" },
};

const char* const kBuiltinManifestResource = ":/Ai/ai_models.json";

[[noreturn]] void
fail(const std::string& where,
     const std::string& what)
{
    throw ModelRegistryError("AI model manifest: " + (where.empty() ? what : where + ": " + what));
}

std::string
requiredString(const QJsonObject& object,
               const char* key,
               const std::string& where)
{
    const QJsonValue value = object.value(QString::fromLatin1(key));
    if (!value.isString() || value.toString().isEmpty()) {
        fail(where, std::string("\"") + key + "\" must be a non-empty string");
    }

    return value.toString().toStdString();
}

bool
isRelativeFileName(const std::string& name)
{
    if (name.empty() || (name.front() == '/') || (name.find('\\') != std::string::npos)) {
        return false;
    }
    for (const std::filesystem::path& part : std::filesystem::path(name)) {
        if ((part == "..") || (part == ".")) {
            return false;
        }
    }

    return true;
}

bool
isSha256(const std::string& digest)
{
    return (digest.size() == 64) && std::all_of(digest.begin(), digest.end(), [](char c) { return ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')); });
}

AiModelEntry
parseEntry(const QJsonValue& value,
           std::size_t index)
{
    std::string where = "model " + std::to_string(index);
    if (!value.isObject()) {
        fail(where, "not an object");
    }
    const QJsonObject object = value.toObject();
    AiModelEntry entry;
    entry.id = requiredString(object, "id", where);
    where = "model \"" + entry.id + "\"";

    const std::string taskName = requiredString(object, "task", where);
    const std::optional<AiModelTask> task = aiModelTaskFromName(taskName);
    if (!task) {
        fail(where, "unknown task \"" + taskName + "\"");
    }
    entry.task = *task;
    entry.name = requiredString(object, "name", where);
    entry.package = requiredString(object, "package", where);
    if (!isRelativeFileName(entry.package) || (entry.package.find('/') != std::string::npos) || (entry.package.front() == '.')) {
        fail(where, "\"package\" must be a plain folder name");
    }

    const QJsonValue files = object.value(QStringLiteral("files"));
    if (!files.isArray() || files.toArray().isEmpty()) {
        fail(where, "\"files\" must be a non-empty array");
    }
    for (const QJsonValue& file : files.toArray()) {
        const std::string name = file.toString().toStdString();
        if (!file.isString() || !isRelativeFileName(name)) {
            fail(where, "\"files\" must hold relative file names");
        }
        entry.files.push_back(name);
    }

    entry.url = requiredString(object, "url", where);
    entry.sha256 = requiredString(object, "sha256", where);
    if (!isSha256(entry.sha256)) {
        fail(where, "\"sha256\" must be 64 lowercase hex digits");
    }
    const QJsonValue size = object.value(QStringLiteral("size"));
    if (!size.isDouble() || (size.toDouble() <= 0)) {
        fail(where, "\"size\" must be a positive number");
    }
    entry.size = static_cast<std::uint64_t>(size.toDouble());
    entry.weightsLicence = requiredString(object, "weightsLicence", where);
    entry.trainingDataLicence = requiredString(object, "trainingDataLicence", where);
    entry.osaid = requiredString(object, "osaid", where);
    entry.cardUrl = requiredString(object, "cardUrl", where);
    const QJsonValue isDefault = object.value(QStringLiteral("default"));
    if (!isDefault.isUndefined() && !isDefault.isBool()) {
        fail(where, "\"default\" must be a boolean");
    }
    entry.isDefault = isDefault.toBool(false);

    return entry;
}

} // anon namespace

std::string_view
aiModelTaskName(AiModelTask task)
{
    for (const auto& t : kTaskNames) {
        if (t.task == task) {
            return t.name;
        }
    }

    return std::string_view();
}

std::optional<AiModelTask>
aiModelTaskFromName(std::string_view name)
{
    for (const auto& t : kTaskNames) {
        if (name == t.name) {
            return t.task;
        }
    }

    return std::nullopt;
}

ModelRegistry::ModelRegistry(const std::string& manifest,
                             const std::string& modelsDirectory)
    : _modelsDirectory(modelsDirectory)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(manifest), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        fail(std::string(), "not valid JSON: " + parseError.errorString().toStdString());
    }
    if (!document.isObject()) {
        fail(std::string(), "the top level must be an object");
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schema")).toInt(-1) != kSchemaVersion) {
        fail(std::string(), "\"schema\" must be " + std::to_string(kSchemaVersion));
    }
    _sourceTag = root.value(QStringLiteral("source")).toObject().value(QStringLiteral("tag")).toString().toStdString();

    const QJsonValue models = root.value(QStringLiteral("models"));
    if (!models.isArray()) {
        fail(std::string(), "\"models\" must be an array");
    }
    std::set<std::string> ids;
    std::map<std::string, const AiModelEntry*> packages;
    std::set<AiModelTask> defaults;
    const QJsonArray array = models.toArray();
    _models.reserve(array.size());
    for (qsizetype i = 0; i < array.size(); ++i) {
        _models.push_back(parseEntry(array.at(i), static_cast<std::size_t>(i)));
    }
    for (const AiModelEntry& entry : _models) {
        const std::string where = "model \"" + entry.id + "\"";
        if (!ids.insert(entry.id).second) {
            fail(where, "the id is listed twice");
        }
        const auto package = packages.emplace(entry.package, &entry);
        if (!package.second) {
            const AiModelEntry& first = *package.first->second;
            if ((first.url != entry.url) || (first.sha256 != entry.sha256) || (first.size != entry.size)) {
                fail(where, "package \"" + entry.package + "\" has another url, sha256 or size in model \"" + first.id + "\"");
            }
        }
        if (entry.isDefault && !defaults.insert(entry.task).second) {
            fail(where, "a second default for task \"" + std::string(aiModelTaskName(entry.task)) + "\"");
        }
    }
}

std::string
ModelRegistry::builtinManifest()
{
    QFile file(QString::fromLatin1(kBuiltinManifestResource));
    if (!file.open(QIODevice::ReadOnly)) {
        throw ModelRegistryError(std::string("AI model manifest: the built-in resource ") + kBuiltinManifestResource + " is missing");
    }

    return file.readAll().toStdString();
}

std::string
ModelRegistry::defaultModelsDirectory()
{
    return (std::filesystem::path(StandardPaths::writableLocation(StandardPaths::eStandardLocationData).toStdString()) / "models").string();
}

std::shared_ptr<const ModelRegistry>
ModelRegistry::process()
{
    static std::once_flag once;
    static std::shared_ptr<const ModelRegistry> registry;

    std::call_once(once, [] {
        registry = std::make_shared<const ModelRegistry>(builtinManifest(), defaultModelsDirectory());
    });

    return registry;
}

const AiModelEntry*
ModelRegistry::find(std::string_view id) const
{
    for (const AiModelEntry& entry : _models) {
        if (entry.id == id) {
            return &entry;
        }
    }

    return nullptr;
}

std::vector<std::string>
ModelRegistry::byTask(AiModelTask task) const
{
    std::vector<std::string> ids;
    for (const AiModelEntry& entry : _models) {
        if (entry.task == task) {
            ids.push_back(entry.id);
        }
    }

    return ids;
}

const AiModelEntry*
ModelRegistry::defaultFor(AiModelTask task) const
{
    const AiModelEntry* first = nullptr;
    for (const AiModelEntry& entry : _models) {
        if (entry.task != task) {
            continue;
        }
        if (entry.isDefault) {
            return &entry;
        }
        if (!first) {
            first = &entry;
        }
    }

    return first;
}

std::vector<const AiModelEntry*>
ModelRegistry::packageModels(const AiModelEntry& entry) const
{
    std::vector<const AiModelEntry*> models;
    for (const AiModelEntry& other : _models) {
        if (other.package == entry.package) {
            models.push_back(&other);
        }
    }

    return models;
}

std::string
ModelRegistry::packageDirectory(const AiModelEntry& entry) const
{
    return (std::filesystem::path(_modelsDirectory) / entry.package).string();
}

AiModelResolution
ModelRegistry::resolve(std::string_view id) const
{
    AiModelResolution resolution;
    resolution.entry = find(id);
    if (!resolution.entry) {
        resolution.status = AiModelStatus::eUnknown;

        return resolution;
    }

    const std::filesystem::path directory(packageDirectory(*resolution.entry));
    resolution.directory = directory.string();
    std::error_code ec;
    bool installed = std::filesystem::is_regular_file(directory / "config.json", ec);
    for (const std::string& file : resolution.entry->files) {
        const std::filesystem::path path = directory / file;
        resolution.files.push_back(path.string());
        installed = installed && std::filesystem::is_regular_file(path, ec);
    }
    resolution.status = installed ? AiModelStatus::eInstalled : AiModelStatus::eMissing;

    return resolution;
}

NATRON_NAMESPACE_EXIT
