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

#ifndef Engine_Ai_ModelKnob_h
#define Engine_Ai_ModelKnob_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QMutex>

#include "Engine/Ai/ModelPackage.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/EngineFwd.h"

class QObject;

NATRON_NAMESPACE_ENTER

class ModelDownloader;

/**
 * @brief The model picker of an AI node: a "model" choice listing the models of one task and a
 * "downloadModel" button. The node owns it, forwards knobChanged() to it and calls resolve() when
 * it renders.
 **/
class ModelKnob {
public:
    struct Resolved {
        const AiModelEntry* entry = nullptr;
        // The package folder, which holds config.json.
        std::string directory;
        // The model's ONNX files as absolute paths, in manifest order.
        std::vector<std::string> files;
        ModelConfig config;
    };

    /**
     * @brief Adds the knobs to page. Call from initializeKnobs(). downloader defaults to
     * ModelDownloader::process(), and outlives the returned helper.
     **/
    static std::unique_ptr<ModelKnob> create(EffectInstance* effect,
                                             const KnobPagePtr& page,
                                             AiModelTask task,
                                             ModelDownloader* downloader = nullptr);

    ~ModelKnob();

    /**
     * @brief The model the user made the default for task in Preferences, when the registry
     * lists it for that task, else the registry's default.
     **/
    static const AiModelEntry* defaultModel(const ModelRegistry& registry, AiModelTask task);

    /**
     * @brief The licence notice shown before a model is downloaded, as HTML: the weights and
     * training-data licences, the OSAID class and the model card link, with a warning when the
     * training data is licensed for research only.
     **/
    static std::string licenceNotice(const AiModelEntry& entry);

    KnobChoicePtr modelKnob() const;
    KnobButtonPtr downloadKnob() const;

    // The id the model knob holds, which may be one the manifest no longer lists.
    std::string modelId() const;

    /**
     * @brief The selected model, installed and with its config read. Otherwise sets the node's
     * persistent error (AI disabled, model not installed, unknown model or unreadable config) and
     * returns nothing; a later successful call takes that error back. Any thread.
     **/
    std::optional<Resolved> resolve();

    /**
     * @brief As resolve(), but hands the reason back in *error instead of posting it, and leaves
     * the node's messages alone: for tools whose node renders fine without the model.
     **/
    std::optional<Resolved> resolve(std::string* error);

    /**
     * @brief Selects the user's default model (see defaultModel()) on a node the user creates.
     * The knob's own default stays the registry's, so that a project saved without touching the
     * knob opens on the same model everywhere. Call from the node's onEffectCreated().
     **/
    void onEffectCreated(const CreateNodeArgs& args);

    /**
     * @brief Handles a press of the download button: starts downloading the selected model once
     * the user accepts its licences. Returns whether k was one of this helper's knobs.
     **/
    bool knobChanged(KnobI* k);

private:
    ModelKnob(EffectInstance* effect, AiModelTask task, ModelDownloader* downloader);

    void refreshEntries();
    void onModelsChanged(const std::string& id);
    void postError(const std::string& message);
    void clearOwnError();

    EffectInstance* const _effect;
    const AiModelTask _task;
    ModelDownloader* const _downloader;
    std::unique_ptr<QObject> _context;
    KnobChoiceWPtr _model;
    KnobButtonWPtr _download;

    mutable QMutex _mutex;
    std::optional<Resolved> _resolved;
    // The last error this helper posted, so it takes back only its own.
    std::string _postedError;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_ModelKnob_h
