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

#include "ModelKnob.h"

#include <utility>

#include <QObject>
#include <QString>

#include "Engine/Ai/ModelDownloader.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Settings.h"
#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER

namespace {

std::string
htmlEscaped(const std::string& text)
{
    return QString::fromStdString(text).toHtmlEscaped().toStdString();
}

bool
researchOnlyTrainingData(const AiModelEntry& entry)
{
    const QString licence = QString::fromStdString(entry.trainingDataLicence);

    return licence.contains(QLatin1String("research-only"), Qt::CaseInsensitive) || licence.contains(QLatin1String("research only"), Qt::CaseInsensitive);
}

} // namespace

ModelKnob::ModelKnob(EffectInstance* effect,
                     AiModelTask task,
                     ModelDownloader* downloader)
    : _effect(effect)
    , _task(task)
    , _downloader(downloader)
    , _context(new QObject)
{
    // Lambdas on a context object the helper owns keep this class free of moc, and disconnect
    // themselves when the helper goes.
    QObject::connect(_downloader, &ModelDownloader::finished, _context.get(),
                     [this](const QString& id, bool ok, const QString& error) {
                         const std::string modelIdString = id.toStdString();
                         onModelsChanged(modelIdString);
                         if (!ok && (modelIdString == modelId())) {
                             postError("Downloading the model " + modelIdString + " failed: " + error.toStdString());
                         }
                     });
    QObject::connect(_downloader, &ModelDownloader::removed, _context.get(),
                     [this](const QString& id) {
                         onModelsChanged(id.toStdString());
                     });
}

ModelKnob::~ModelKnob() = default;

std::unique_ptr<ModelKnob>
ModelKnob::create(EffectInstance* effect,
                  const KnobPagePtr& page,
                  AiModelTask task,
                  ModelDownloader* downloader)
{
    if (!downloader) {
        downloader = ModelDownloader::process();
    }
    std::unique_ptr<ModelKnob> helper(new ModelKnob(effect, task, downloader));

    KnobChoicePtr model = AppManager::createKnob<KnobChoice>(effect, std::string("Model"));
    model->setName("model");
    model->setAnimationEnabled(false);
    model->setHintToolTip(std::string("The model this node runs. A model that is not installed can be downloaded "
                                      "with Download Model, or in Preferences > AI Models."));
    page->addKnob(model);
    helper->_model = model;
    helper->refreshEntries();
    if (const AiModelEntry* registryDefault = downloader->registry()->defaultFor(task)) {
        model->setDefaultValueFromID(registryDefault->id);
    }

    KnobButtonPtr download = AppManager::createKnob<KnobButton>(effect, std::string("Download Model"));
    download->setName("downloadModel");
    download->setEvaluateOnChange(false);
    download->setHintToolTip(std::string("Downloads the selected model, after showing its licences."));
    page->addKnob(download);
    helper->_download = download;

    return helper;
}

const AiModelEntry*
ModelKnob::defaultModel(const ModelRegistry& registry,
                        AiModelTask task)
{
    const SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();

    if (settings) {
        const AiModelEntry* picked = registry.find(settings->getAiDefaultModel(task));
        if (picked && (picked->task == task)) {
            return picked;
        }
    }

    return registry.defaultFor(task);
}

std::string
ModelKnob::licenceNotice(const AiModelEntry& entry)
{
    std::string html = "<p>" + htmlEscaped(entry.name) + " (" + htmlEscaped(entry.id) + ") is published under these licences:</p>"
                                                                                        "<table>"
                                                                                        "<tr><td>Weights:</td><td>"
        + htmlEscaped(entry.weightsLicence) + "</td></tr>"
                                              "<tr><td>Training data:</td><td>"
        + htmlEscaped(entry.trainingDataLicence) + "</td></tr>"
                                                   "<tr><td>OSAID class:</td><td>"
        + htmlEscaped(entry.osaid) + "</td></tr>"
                                     "</table>";
    if (researchOnlyTrainingData(entry)) {
        html += "<p><b>Part of its training data is licensed for research use only.</b> Check that this "
                "licence allows your use of the model before you download it.</p>";
    }
    if (!entry.cardUrl.empty()) {
        html += "<p>Model card: <a href=\"" + htmlEscaped(entry.cardUrl) + "\">" + htmlEscaped(entry.cardUrl) + "</a></p>";
    }
    html += "<p>Download it?</p>";

    return html;
}

KnobChoicePtr
ModelKnob::modelKnob() const
{
    return _model.lock();
}

KnobButtonPtr
ModelKnob::downloadKnob() const
{
    return _download.lock();
}

std::string
ModelKnob::modelId() const
{
    KnobChoicePtr model = _model.lock();

    return model ? model->getActiveEntry().id : std::string();
}

void
ModelKnob::onEffectCreated(const CreateNodeArgs& args)
{
    if (args.getProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization)) {
        return;
    }
    KnobChoicePtr model = _model.lock();
    const AiModelEntry* picked = defaultModel(*_downloader->registry(), _task);
    if (model && picked && (picked->id != modelId())) {
        model->setValueFromID(picked->id, 0);
    }
}

std::optional<ModelKnob::Resolved>
ModelKnob::resolve()
{
    const SettingsPtr settings = appPTR->getCurrentSettings();

    if (!settings || !settings->isAiEnabled()) {
        postError("AI is disabled. Enable it in Preferences > AI.");

        return std::nullopt;
    }

    const std::string id = modelId();
    std::optional<Resolved> resolved;
    {
        QMutexLocker locker(&_mutex);
        if (_resolved && (_resolved->entry->id == id)) {
            resolved = _resolved;
        }
    }
    if (!resolved) {
        if (id.empty()) {
            postError("No model is selected.");

            return std::nullopt;
        }
        const AiModelResolution resolution = _downloader->registry()->resolve(id);
        switch (resolution.status) {
        case AiModelStatus::eUnknown:
            postError("unknown model `" + id + "`: this version of Natron does not list it.");

            return std::nullopt;
        case AiModelStatus::eMissing:
            postError("The model " + resolution.entry->name + " (" + id + ") is not installed. Press Download Model, "
                                                                          "or download it in Preferences > AI Models.");

            return std::nullopt;
        case AiModelStatus::eInstalled:
            break;
        }
        Resolved fresh;
        fresh.entry = resolution.entry;
        fresh.directory = resolution.directory;
        fresh.files = resolution.files;
        try {
            fresh.config = readModelConfig(resolution.directory, resolution.entry->files.empty() ? std::string() : resolution.entry->files.front());
        } catch (const std::exception& e) {
            postError("The model " + id + " is installed but its config.json cannot be read: " + e.what());

            return std::nullopt;
        }
        QMutexLocker locker(&_mutex);
        _resolved = fresh;
        resolved = std::move(fresh);
    }
    clearOwnError();

    return resolved;
}

bool
ModelKnob::knobChanged(KnobI* k)
{
    KnobButtonPtr download = _download.lock();
    KnobChoicePtr model = _model.lock();

    if (model && (k == model.get())) {
        return true;
    }
    if (!download || (k != download.get())) {
        return false;
    }

    const std::string id = modelId();
    const AiModelResolution resolution = _downloader->registry()->resolve(id);
    if (resolution.status == AiModelStatus::eUnknown) {
        postError("unknown model `" + id + "`: this version of Natron does not list it.");

        return true;
    }
    if ((resolution.status == AiModelStatus::eInstalled) || _downloader->isDownloading(id)) {
        return true;
    }
    const AppInstancePtr app = _effect->getApp();
    if (app && (app->questionDialog("Download Model", licenceNotice(*resolution.entry), true, StandardButtons(eStandardButtonYes | eStandardButtonNo), eStandardButtonNo) != eStandardButtonYes)) {
        return true;
    }
    std::string error;
    if (!_downloader->start(id, &error)) {
        postError("Downloading the model " + id + " failed: " + error);
    }

    return true;
}

void
ModelKnob::refreshEntries()
{
    KnobChoicePtr model = _model.lock();

    if (!model) {
        return;
    }
    const ModelRegistry& registry = *_downloader->registry();
    std::vector<ChoiceOption> options;
    for (const std::string& id : registry.byTask(_task)) {
        const AiModelResolution resolution = registry.resolve(id);
        const bool installed = (resolution.status == AiModelStatus::eInstalled);
        options.emplace_back(id,
                             resolution.entry->name + (installed ? std::string() : std::string(" (not installed)")),
                             resolution.entry->id);
    }
    model->populateChoices(options);
}

void
ModelKnob::onModelsChanged(const std::string& id)
{
    const AiModelEntry* entry = _downloader->registry()->find(id);

    if (!entry || (entry->task != _task)) {
        return;
    }
    {
        QMutexLocker locker(&_mutex);
        _resolved.reset();
    }
    refreshEntries();
    _effect->incrHashAndEvaluate(true, false);
}

void
ModelKnob::postError(const std::string& message)
{
    const NodePtr node = _effect->getNode();

    if (!node) {
        return;
    }
    {
        QMutexLocker locker(&_mutex);
        _postedError = message;
    }
    // Renders call this every time; posting the same message again would only repaint the node.
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if ((type == eMessageTypeError) && (current == QString::fromStdString(message))) {
        return;
    }
    node->setPersistentMessage(eMessageTypeError, message);
}

void
ModelKnob::clearOwnError()
{
    std::string posted;
    {
        QMutexLocker locker(&_mutex);
        posted.swap(_postedError);
    }
    if (posted.empty()) {
        return;
    }
    const NodePtr node = _effect->getNode();
    if (!node) {
        return;
    }
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if (current == QString::fromStdString(posted)) {
        node->clearPersistentMessage(false);
    }
}

NATRON_NAMESPACE_EXIT
