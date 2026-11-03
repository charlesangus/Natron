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

#ifndef Engine_Ai_ModelRegistry_h
#define Engine_Ai_ModelRegistry_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

NATRON_NAMESPACE_ENTER

enum class AiModelTask {
    eMaskObject,
    eDenoise,
    eDenoiseRawBayer,
    eDenoiseLinear,
    eUpscale2x,
    eUpscale4x,
};

// The manifest's spelling: "mask-object", "denoise", "denoise-raw-bayer", ...
std::string_view aiModelTaskName(AiModelTask task);
std::optional<AiModelTask> aiModelTaskFromName(std::string_view name);

class ModelRegistryError
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief One model of the manifest. Several models may come from one package (a darktable-ai
 * .dtmodel), each using some of its files: they share the package's download and install folder.
 **/
struct AiModelEntry {
    std::string id;
    AiModelTask task = AiModelTask::eDenoise;
    std::string name;
    std::string package;
    // The ONNX files of the package this model runs, relative to the package folder.
    std::vector<std::string> files;
    std::string url;
    // Lowercase hex digest of the package file.
    std::string sha256;
    std::uint64_t size = 0;
    std::string weightsLicence;
    std::string trainingDataLicence;
    // darktable-ai's OSAID v1.0 classification: "Open Source AI", "Open Weights" or "Open Model".
    std::string osaid;
    std::string cardUrl;
    bool isDefault = false;
};

enum class AiModelStatus {
    eInstalled,
    eMissing,
    // The id is not in the manifest.
    eUnknown,
};

struct AiModelResolution {
    AiModelStatus status = AiModelStatus::eUnknown;
    // Set unless the id is unknown, installed or not.
    const AiModelEntry* entry = nullptr;
    // The package folder, which holds config.json and the model's files once installed.
    std::string directory;
    // The model's files as absolute paths, in manifest order.
    std::vector<std::string> files;
};

/**
 * @brief The models of a manifest and where they install: <modelsDirectory>/<package>/, holding
 * the package's config.json and ONNX files. Immutable once built, so it may be shared between
 * threads; resolve() looks at the disk each time it is called.
 **/
class ModelRegistry {
public:
    static constexpr int kSchemaVersion = 1;

    /**
     * @brief Parses manifest, a JSON document in the format of Engine/Ai/data/ai_models.json.
     * Throws ModelRegistryError naming the first problem: an unknown schema or task, a missing
     * field, a duplicate id, a package whose entries disagree on its download, or more than one
     * default for a task.
     **/
    ModelRegistry(const std::string& manifest, const std::string& modelsDirectory);

    /**
     * @brief The manifest compiled into Natron, a Qt resource.
     **/
    static std::string builtinManifest();

    // Names a manifest file that replaces the built-in one, for tests that must not use the network.
    static constexpr const char* kManifestEnvVar = "NATRON_AI_MANIFEST";

    /**
     * @brief The manifest process() uses: the file NATRON_AI_MANIFEST names, else the built-in one.
     * Throws ModelRegistryError when that file cannot be read.
     **/
    static std::string processManifest();

    /**
     * @brief <user data dir>/models.
     **/
    static std::string defaultModelsDirectory();

    /**
     * @brief processManifest() over the default models directory, built on first use.
     **/
    static std::shared_ptr<const ModelRegistry> process();

    const std::vector<AiModelEntry>& models() const
    {
        return _models;
    }

    const std::string& modelsDirectory() const
    {
        return _modelsDirectory;
    }

    // The darktable-ai release tag the manifest pins, empty when it names none.
    const std::string& sourceTag() const
    {
        return _sourceTag;
    }

    const AiModelEntry* find(std::string_view id) const;

    // The ids of the models for a task, in manifest order.
    std::vector<std::string> byTask(AiModelTask task) const;

    // The task's default model, or the first of its models when none is marked, or null.
    const AiModelEntry* defaultFor(AiModelTask task) const;

    // Every model sharing entry's package, entry included, in manifest order.
    std::vector<const AiModelEntry*> packageModels(const AiModelEntry& entry) const;

    std::string packageDirectory(const AiModelEntry& entry) const;

    /**
     * @brief Installed when the package folder holds config.json and every file of the model.
     **/
    AiModelResolution resolve(std::string_view id) const;

private:
    std::vector<AiModelEntry> _models;
    std::string _modelsDirectory;
    std::string _sourceTag;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_ModelRegistry_h
