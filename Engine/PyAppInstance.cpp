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

#include "PyAppInstance.h"

#include <cassert>
#include <optional>
#include <sstream> // stringstream
#include <stdexcept>

#include <QCoreApplication>
#include <QDebug>
#include <QThread>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/GraphOpApply.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Loop/LoopPair.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Project.h"
#include "Engine/ProjectMerge.h"
#include "Engine/ProjectTemplate.h"
#include "Engine/PyParameter.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/Settings.h"
#include "Engine/UndoRecorder.h"

#include "Engine/EngineFwd.h"

#include "Serialization/Merge/MergeConflict.h"
#include "Serialization/Ops/SnapshotDiff.h"
#include "Serialization/ProjectSerialization.h"

NATRON_NAMESPACE_ENTER
NATRON_PYTHON_NAMESPACE_ENTER

App::App(const AppInstancePtr& instance)
    : Group()
    , UserParamHolder(instance->getProject().get())
    , _instance(instance)
{
    if ( instance->getProject() ) {
        init( instance->getProject() );
    }
}

int
App::getAppID() const
{
    return getInternalApp()->getAppID();
}

Effect*
App::getNodeById(const QString& id) const
{
    AppInstancePtr app = getInternalApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    NodePtr node = project ? project->findNodeById(id.toStdString()) : NodePtr();

    return node ? new Effect(node) : NULL;
}

AppInstancePtr
App::getInternalApp() const
{
    return _instance.lock();
}

NodeCollectionPtr
App::getCollectionFromGroup(Group* group) const
{
    NodeCollectionPtr collection;

    if (group) {
        App* isApp = dynamic_cast<App*>(group);
        Effect* isEffect = dynamic_cast<Effect*>(group);
        if (isApp) {
            collection = std::dynamic_pointer_cast<NodeCollection>( isApp->getInternalApp()->getProject() );
        } else if (isEffect) {
            NodePtr node = isEffect->getInternalNode();
            assert(node);
            NodeGroupPtr isGrp = std::dynamic_pointer_cast<NodeGroup>( node->getEffectInstance()->shared_from_this() );
            if (!isGrp) {
                qDebug() << "The group passed to createNode() is not a group, defaulting to the project root.";
            } else {
                collection = std::dynamic_pointer_cast<NodeCollection>(isGrp);
                assert(collection);
            }
        }
    }

    if (!collection && !group) {
        collection = getInternalApp()->getNodeCreationContextCollection();
    }
    if (!collection) {
        collection = std::dynamic_pointer_cast<NodeCollection>( getInternalApp()->getProject() );
    }

    return collection;
}

// A script that asks for the node graph's own undo command gets it; otherwise the node is recorded
// in the script's step once it exists.
static void
recordCreatedNode(const NodePtr& node,
                  const CreateNodeArgs& args)
{
    if (!args.getProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand)) {
        recordPythonNodeCreated(node);
    }
}

static void
makeCreateNodeArgs(const AppInstancePtr& app,
                   const QString& pluginID,
                   int majorVersion,
                   const NodeCollectionPtr& collection,
                   const NodeCreationPropertyMap& props,
                   bool interactive,
                   CreateNodeArgs* args)
{

    args->setProperty<std::string>(kCreateNodeArgsPropPluginID, pluginID.toStdString());
    args->setProperty<NodeCollectionPtr>(kCreateNodeArgsPropGroupContainer, collection);
    args->setProperty<int>(kCreateNodeArgsPropPluginVersion, majorVersion, 0);

    if (!interactive) {
        args->setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
        args->setProperty<bool>(kCreateNodeArgsPropSettingsOpened, false);
        args->setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
        args->setProperty<bool>(kCreateNodeArgsPropSilent, true);
        args->setProperty<bool>(kCreateNodeArgsPropAllowNonUserCreatablePlugins, true); // also load deprecated plugins
    }

    bool skipNoNodeGuiProp = false;
    for (NodeCreationPropertyMap::const_iterator it = props.begin(); it!=props.end(); ++it) {
        IntNodeCreationProperty* isInt = dynamic_cast<IntNodeCreationProperty*>(it->second);
        BoolNodeCreationProperty* isBool = dynamic_cast<BoolNodeCreationProperty*>(it->second);
        FloatNodeCreationProperty* isDouble = dynamic_cast<FloatNodeCreationProperty*>(it->second);
        StringNodeCreationProperty* isString = dynamic_cast<StringNodeCreationProperty*>(it->second);

        bool fail = true;
        if (it->first.startsWith(QString::fromUtf8(kCreateNodeArgsPropParamValue))) {
            fail = false;
        }
        std::string prop = it->first.toStdString();
        try {
            if (isInt) {
                args->setPropertyN<int>(prop, isInt->getValues(), fail);
            } else if (isBool) {
                if (prop == kCreateNodeArgsPropOutOfProject) {
                    args->setProperty(kCreateNodeArgsPropNoNodeGUI, true);
                    skipNoNodeGuiProp = true;
                } else if (skipNoNodeGuiProp && prop == kCreateNodeArgsPropNoNodeGUI) {
                    continue;
                }
                args->setPropertyN<bool>(prop, isBool->getValues(), fail);
            } else if (isDouble) {
                args->setPropertyN<double>(prop, isDouble->getValues(), fail);
            } else if (isString) {
                args->setPropertyN<std::string>(prop, isString->getValues(), fail);
            }
        } catch (const std::exception& e) {
            std::stringstream ss;
            ss << ("Error while setting property ");
            ss << it->first.toStdString();
            ss << ": " << e.what();
            app->appendToScriptEditor(ss.str());
        }
    }
}


Effect*
App::createNode(const QString& pluginID,
                int majorVersion,
                Group* group,
                const NodeCreationPropertyMap& props) const
{
    return createNodeInCollection(pluginID, majorVersion, getCollectionFromGroup(group), props, false);
}

Effect*
App::createNodeInCollection(const QString& pluginID,
                            int majorVersion,
                            const NodeCollectionPtr& collection,
                            const NodeCreationPropertyMap& props,
                            bool interactive) const
{
    assert(collection);
    CreateNodeArgs args;
    makeCreateNodeArgs(getInternalApp(), pluginID, majorVersion, collection, props, interactive, &args);

    NodePtr node = getInternalApp()->createNode(args);
    if (node) {
        recordCreatedNode(node, args);

        return new Effect(node);
    } else {
        return NULL;
    }
}

std::list<Effect*>
App::createLoopPair(const QString& kind,
                    Group* group) const
{
    std::list<Effect*> created;
    const LoopPairKind* pair = findLoopPairKind(kind.toStdString());
    if (!pair) {
        return created;
    }
    const AppInstancePtr app = getInternalApp();
    const NodeCollectionPtr collection = getCollectionFromGroup(group);
    LoopPairNodes nodes = NATRON_NAMESPACE::createLoopPair(app, *pair, [&app, &collection](CreateNodeArgs& args) {
        makeCreateNodeArgs(app, QString(), -1, collection, NodeCreationPropertyMap(), false, &args);
    });
    if (nodes.begin && nodes.end) {
        created.push_back(new Effect(nodes.begin));
        created.push_back(new Effect(nodes.end));
    }

    return created;
}

Effect*
App::createReader(const QString& filename,
                  Group* group,
                  const NodeCreationPropertyMap& props) const
{
    NodeCollectionPtr collection = getCollectionFromGroup(group);

    assert(collection);

    CreateNodeArgs args;
    makeCreateNodeArgs(getInternalApp(), QString::fromUtf8(PLUGINID_NATRON_READ), -1, collection, props, false, &args);

    NodePtr node = getInternalApp()->createReader(filename.toStdString(), args);
    if (node) {
        recordCreatedNode(node, args);

        return new Effect(node);
    } else {
        return NULL;
    }
}

Effect*
App::createWriter(const QString& filename,
                  Group* group,
                  const NodeCreationPropertyMap& props) const
{
    NodeCollectionPtr collection = getCollectionFromGroup(group);

    assert(collection);
    CreateNodeArgs args;
    makeCreateNodeArgs(getInternalApp(), QString::fromUtf8(PLUGINID_NATRON_WRITE), -1, collection, props, false, &args);
    NodePtr node = getInternalApp()->createWriter(filename.toStdString(), args);
    if (node) {
        recordCreatedNode(node, args);

        return new Effect(node);
    } else {
        return NULL;
    }
}

bool
App::waitForReloads(int timeoutMs)
{
    return SceneWriteBarrier::waitForAsyncWrites(timeoutMs);
}

int
App::timelineGetTime() const
{
    return getInternalApp()->getTimeLine()->currentFrame();
}

int
App::timelineGetLeftBound() const
{
    double left, right;

    getInternalApp()->getFrameRange(&left, &right);

    return left;
}

int
App::timelineGetRightBound() const
{
    double left, right;

    getInternalApp()->getFrameRange(&left, &right);

    return right;
}

AppSettings::AppSettings(const SettingsPtr& settings)
    : _settings(settings)
{
}

Param*
AppSettings::getParam(const QString& scriptName) const
{
    KnobIPtr knob = _settings->getKnobByName( scriptName.toStdString() );

    if (!knob) {
        return 0;
    }

    return Effect::createParamWrapperForKnob(knob);
}

std::list<Param*>
AppSettings::getParams() const
{
    std::list<Param*> ret;
    const KnobsVec& knobs = _settings->getKnobs();

    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        Param* p = Effect::createParamWrapperForKnob(*it);
        if (p) {
            ret.push_back(p);
        }
    }

    return ret;
}

void
AppSettings::saveSettings()
{
    _settings->saveAllSettings();
}

void
AppSettings::restoreDefaultSettings()
{
    _settings->restoreDefault();
}

void
App::render(Effect* writeNode,
            int firstFrame,
            int lastFrame,
            int frameStep)
{
    renderInternal(false, writeNode, firstFrame, lastFrame, frameStep);
}

void
App::render(const std::list<Effect*>& effects,
            const std::list<int>& firstFrames,
            const std::list<int>& lastFrames,
            const std::list<int>& frameSteps)
{
    renderInternal(false, effects, firstFrames, lastFrames, frameSteps);
}

void
App::renderInternal(bool forceBlocking,
                    Effect* writeNode,
                    int firstFrame,
                    int lastFrame,
                    int frameStep)
{
    if (!writeNode) {
        std::cerr << tr("Invalid write node").toStdString() << std::endl;

        return;
    }
    AppInstance::RenderWork w;
    NodePtr node =  writeNode->getInternalNode();
    if (!node) {
        std::cerr << tr("Invalid write node").toStdString() << std::endl;

        return;
    }
    w.writer = dynamic_cast<OutputEffectInstance*>( node->getEffectInstance().get() );
    if (!w.writer) {
        std::cerr << tr("Invalid write node").toStdString() << std::endl;

        return;
    }
    if (appPTR->isBackground() && node->isEffectViewer()) {
        std::cerr << tr("%1 is a Viewer, which cannot render without the GUI.").arg(QString::fromUtf8(node->getFullyQualifiedName().c_str())).toStdString() << std::endl;

        return;
    }

    w.firstFrame = firstFrame;
    w.lastFrame = lastFrame;
    w.frameStep = frameStep;
    w.useRenderStats = false;

    std::list<AppInstance::RenderWork> l;
    l.push_back(w);

    PythonGILUnlocker pgl;

    if (forceBlocking) {
        getInternalApp()->startWritersRendering(eRenderModeBlocking, l);
    } else {
        getInternalApp()->renderForeground(l);
    }
}

void
App::renderInternal(bool forceBlocking,
                    const std::list<Effect*>& effects,
                    const std::list<int>& firstFrames,
                    const std::list<int>& lastFrames,
                    const std::list<int>& frameSteps)
{
    std::list<AppInstance::RenderWork> l;

    assert( effects.size() == firstFrames.size() && effects.size() == lastFrames.size() && frameSteps.size() == effects.size() );
    std::list<Effect*>::const_iterator itE = effects.begin();
    std::list<int>::const_iterator itF = firstFrames.begin();
    std::list<int>::const_iterator itL = lastFrames.begin();
    std::list<int>::const_iterator itS = frameSteps.begin();
    for (; itE != effects.end(); ++itE, ++itF, ++itL, ++itS) {
        if (!*itE) {
            std::cerr << tr("Invalid write node").toStdString() << std::endl;

            return;
        }
        AppInstance::RenderWork w;
        NodePtr node =  (*itE)->getInternalNode();
        if (!node) {
            std::cerr << tr("Invalid write node").toStdString() << std::endl;

            return;
        }
        w.writer = dynamic_cast<OutputEffectInstance*>( node->getEffectInstance().get() );
        if ( !w.writer || !w.writer->isOutput() ) {
            std::cerr << tr("Invalid write node").toStdString() << std::endl;

            return;
        }
        if (appPTR->isBackground() && node->isEffectViewer()) {
            std::cerr << tr("%1 is a Viewer, which cannot render without the GUI.").arg(QString::fromUtf8(node->getFullyQualifiedName().c_str())).toStdString() << std::endl;

            return;
        }

        w.firstFrame = (*itF);
        w.lastFrame = (*itL);
        w.frameStep = (*itS);
        w.useRenderStats = false;

        l.push_back(w);
    }

    PythonGILUnlocker pgl;

    if (forceBlocking) {
        getInternalApp()->startWritersRendering(eRenderModeBlocking, l);
    } else {
        getInternalApp()->renderForeground(l);
    }
}

Param*
App::getProjectParam(const QString& name) const
{
    KnobIPtr knob =  getInternalApp()->getProject()->getKnobByName( name.toStdString() );

    if (!knob) {
        return 0;
    }

    return Effect::createParamWrapperForKnob(knob);
}

void
App::writeToScriptEditor(const QString& message)
{
    getInternalApp()->appendToScriptEditor( message.toStdString() );
}

void
App::addFormat(const QString& formatSpec)
{
    if ( !getInternalApp()->getProject()->addFormat( formatSpec.toStdString() ) ) {
        getInternalApp()->appendToScriptEditor( formatSpec.toStdString() );
    }
}

bool
App::saveTempProject(const QString& filename)
{
    // A save may wait for an auto-save whose save callback needs the Python lock.
    PythonGILUnlocker pgl;

    return getInternalApp()->saveTemp( filename.toStdString() );
}

bool
App::saveProject(const QString& filename)
{
    std::string outFile = filename.toStdString();
    std::map<std::string, std::string> env;
    getInternalApp()->getProject()->getEnvironmentVariables(env);
    Project::expandVariable(env, outFile);
    // A save may wait for an auto-save whose save callback needs the Python lock.
    PythonGILUnlocker pgl;

    return getInternalApp()->save(outFile);
}

bool
App::saveProjectAs(const QString& filename)
{
    std::string outFile = filename.toStdString();
    std::map<std::string, std::string> env;
    getInternalApp()->getProject()->getEnvironmentVariables(env);
    Project::expandVariable(env, outFile);
    // A save may wait for an auto-save whose save callback needs the Python lock.
    PythonGILUnlocker pgl;

    return getInternalApp()->saveAs(outFile);
}

App*
App::loadProject(const QString& filename)
{
    AppInstancePtr app  = getInternalApp()->loadProject( filename.toStdString() );

    if (!app) {
        return 0;
    }

    return new App(app);
}

namespace {
bool
setDictString(PyObject* dict,
              const char* key,
              const std::string& value)
{
    PyObject* item = PyUnicode_FromStringAndSize(value.data(), (Py_ssize_t)value.size());
    const bool ok = item && (PyDict_SetItemString(dict, key, item) == 0);
    Py_XDECREF(item);

    return ok;
}

PyObject*
stringList(const std::vector<std::string>& values)
{
    PyObject* list = PyList_New((Py_ssize_t)values.size());
    for (std::size_t i = 0; list && (i < values.size()); ++i) {
        PyObject* item = PyUnicode_FromStringAndSize(values[i].data(), (Py_ssize_t)values[i].size());
        if (!item) {
            Py_DECREF(list);

            return 0;
        }
        PyList_SetItem(list, (Py_ssize_t)i, item);
    }

    return list;
}

PyObject*
conflictDict(const ProjectPtr& project,
             std::size_t index,
             const Serialization::MergeConflict& conflict)
{
    PyObject* dict = PyDict_New();
    if (!dict) {
        return 0;
    }
    const NodePtr node = conflict.nodeId.empty() ? NodePtr() : project->findNodeById(conflict.nodeId);
    std::vector<std::string> names;
    std::vector<std::string> labels;
    for (const ProjectMerge::ConflictChoice& choice : ProjectMerge::availableChoices(project, conflict)) {
        names.push_back(ProjectMerge::choiceName(choice));
        labels.push_back(choice.label.toStdString());
    }
    PyObject* indexItem = PyLong_FromSize_t(index);
    PyObject* choices = stringList(names);
    PyObject* choiceLabels = stringList(labels);
    const bool ok = indexItem && choices && choiceLabels && (PyDict_SetItemString(dict, "index", indexItem) == 0) && setDictString(dict, "node", node ? node->getFullyQualifiedName() : std::string()) && setDictString(dict, "nodeId", conflict.nodeId) && setDictString(dict, "kind", std::string(Serialization::mergeConflictKindName(conflict.kind))) && setDictString(dict, "label", ProjectMerge::conflictLabel(conflict).toStdString()) && (PyDict_SetItemString(dict, "choices", choices) == 0) && (PyDict_SetItemString(dict, "choiceLabels", choiceLabels) == 0);
    Py_XDECREF(indexItem);
    Py_XDECREF(choices);
    Py_XDECREF(choiceLabels);
    if (!ok) {
        Py_DECREF(dict);

        return 0;
    }

    return dict;
}

// The collection group names in app, the project's root for None. False with error set when it
// is not a group of app, or is a PyPlug that is not unlocked for editing.
bool
targetCollection(const AppInstancePtr& app,
                 Group* group,
                 NodeCollectionPtr* collection,
                 QString* error)
{
    if (!group) {
        *collection = std::dynamic_pointer_cast<NodeCollection>(app->getProject());

        return true;
    }
    App* isApp = dynamic_cast<App*>(group);
    Effect* isEffect = dynamic_cast<Effect*>(group);
    if (isApp) {
        if (isApp->getInternalApp() == app) {
            *collection = std::dynamic_pointer_cast<NodeCollection>(app->getProject());
        }
    } else if (isEffect) {
        NodePtr node = isEffect->getInternalNode();
        if (node && node->getApp() == app && node->getEffectInstance()) {
            NodeGroupPtr isGroup = std::dynamic_pointer_cast<NodeGroup>(node->getEffectInstance()->shared_from_this());
            if (!isGroup) {
                *error = QString::fromUtf8("%1 is not a group").arg(QString::fromStdString(node->getScriptName_mt_safe()));

                return false;
            }
            if (Project::isLockedPyPlug(isGroup)) {
                *error = QString::fromUtf8("%1 is a PyPlug that is not unlocked for editing").arg(QString::fromStdString(node->getScriptName_mt_safe()));

                return false;
            }
            *collection = isGroup;
        }
    }
    if (!*collection) {
        *error = QString::fromUtf8("The group is not a group of this application");

        return false;
    }

    return true;
}
} // anon namespace

PyObject*
App::getMergeConflicts() const
{
    const ProjectPtr project = getInternalApp()->getProject();
    const std::vector<Serialization::MergeConflict> conflicts = project ? project->getMergeConflicts() : std::vector<Serialization::MergeConflict>();
    PyObject* list = PyList_New((Py_ssize_t)conflicts.size());
    for (std::size_t i = 0; list && (i < conflicts.size()); ++i) {
        PyObject* dict = conflictDict(project, i, conflicts[i]);
        if (!dict) {
            Py_DECREF(list);

            return 0;
        }
        PyList_SetItem(list, (Py_ssize_t)i, dict);
    }

    return list;
}

bool
App::resolveMergeConflict(int index,
                          const QString& choice)
{
    const AppInstancePtr app = getInternalApp();
    const ProjectPtr project = app->getProject();
    const std::vector<Serialization::MergeConflict> conflicts = project->getMergeConflicts();
    const auto fail = [&app](const QString& message) {
        app->appendToScriptEditor(message.toStdString());

        return false;
    };
    if ((index < 0) || ((std::size_t)index >= conflicts.size())) {
        return fail(tr("There is no merge conflict %1.").arg(index));
    }
    const Serialization::MergeConflict& conflict = conflicts[(std::size_t)index];
    ProjectMerge::ConflictChoice found;
    if (!ProjectMerge::findChoice(project, conflict, choice.toStdString(), &found)) {
        return fail(tr("Merge conflict %1 offers no choice %2.").arg(index).arg(choice));
    }
    std::vector<Serialization::GraphOp> ops;
    QString error;
    const bool built = found.option.empty() ? ProjectMerge::sideOperations(project, conflict, found.side, &ops, &error)
                                            : ProjectMerge::optionOperations(project, conflict, found.option, &ops, &error);
    if (!built) {
        return fail(error);
    }
    if (!ops.empty()) {
        Serialization::ProjectSerialization before;
        project->toSerialization(&before);
        const std::vector<Serialization::OpResult> results = applyGraphOps(project, ops);
        bool applied = (results.size() == ops.size());
        for (const Serialization::OpResult& result : results) {
            if (!result.applied()) {
                applied = false;
                error = QString::fromStdString(result.message);
                break;
            }
        }
        if (!applied) {
            Serialization::ProjectSerialization live;
            project->toSerialization(&live);
            ignore_result(applyGraphOps(project, Serialization::diffSnapshots(Serialization::editableState(live), Serialization::editableState(before))));

            return fail(error.isEmpty() ? tr("The resolution was not applied.") : error);
        }
    }
    std::vector<Serialization::MergeConflict> kept = conflicts;
    kept.erase(kept.begin() + index);
    project->setMergeConflicts(kept);

    return true;
} // App::resolveMergeConflict

std::list<Effect*>
App::importTemplateChecked(const QString& filename,
                           Group* group,
                           QString* error,
                           ImportTemplateError* errorKind)
{
    std::list<Effect*> ret;
    *errorKind = eImportTemplateErrorNone;
    if (QThread::currentThread() != qApp->thread()) {
        *errorKind = eImportTemplateErrorFailed;
        *error = QString::fromUtf8("importTemplate() can only be called from the main thread");

        return ret;
    }

    AppInstancePtr app = getInternalApp();
    NodeCollectionPtr collection;
    if (!targetCollection(app, group, &collection, error)) {
        *errorKind = eImportTemplateErrorBadGroup;

        return ret;
    }

    const ProjectPtr project = app->getProject();
    const std::list<Serialization::TemplateImportSerialization> importsBefore = project->getTemplateImports();
    QString importError;
    NodesList nodes = project->importTemplate(filename, collection, &importError);
    for (const NodePtr& node : nodes) {
        recordPythonNodeCreated(node);
    }
    if (project->getTemplateImports().size() > importsBefore.size()) {
        recordPythonTemplateImport(project, importsBefore, nodes);
    }
    if (!importError.isEmpty()) {
        *errorKind = eImportTemplateErrorFailed;
        *error = importError;

        return ret;
    }
    for (const NodePtr& node : nodes) {
        ret.push_back(new Effect(node));
    }

    return ret;
}

std::list<Effect*>
App::importTemplate(const QString& /*filename*/,
                    Group* /*group*/)
{
    // The binding's inject code calls importTemplateChecked() and never reaches this body.
    return std::list<Effect*>();
}

Effect*
App::defineTemplateChecked(const std::list<Effect*>& nodes,
                           const QString& name,
                           Group* group,
                           QString* error)
{
    if (QThread::currentThread() != qApp->thread()) {
        *error = QString::fromUtf8("defineTemplate() can only be called from the main thread");

        return 0;
    }
    AppInstancePtr app = getInternalApp();
    NodeCollectionPtr collection;
    if (!targetCollection(app, group, &collection, error)) {
        return 0;
    }
    NodesList members;
    for (Effect* effect : nodes) {
        NodePtr node = effect ? effect->getInternalNode() : NodePtr();
        if (!node || (node->getApp() != app)) {
            *error = QString::fromUtf8("A node to make a template of is not a node of this application");

            return 0;
        }
        members.push_back(node);
    }
    const ProjectPtr project = app->getProject();
    NodePtr templateNode;
    app->runTemplateEdit(QString::fromUtf8("Define Template"), [&](TemplateEdit* edit) {
        if (!project->defineTemplate(members, name.toStdString(), collection, edit)) {
            *error = edit->error;

            return false;
        }
        templateNode = edit->templateNode;

        return true;
    });
    if (!templateNode) {
        if (error->isEmpty()) {
            *error = QString::fromUtf8("The template was not defined");
        }

        return 0;
    }

    return new Effect(templateNode);
}

Effect*
App::defineTemplate(const std::list<Effect*>& /*nodes*/,
                    const QString& /*name*/,
                    Group* /*group*/)
{
    // The binding's inject code calls defineTemplateChecked() and never reaches this body.
    return 0;
}

QString
App::publishTemplateChecked(const QString& templateKey,
                            QString* error)
{
    if (QThread::currentThread() != qApp->thread()) {
        *error = QString::fromUtf8("publishTemplate() can only be called from the main thread");

        return QString();
    }
    AppInstancePtr app = getInternalApp();
    const ProjectPtr project = app->getProject();
    // Not an undo step: like a save, it writes a file that an undo would not take back.
    TemplateEdit edit;
    if (!project->publishTemplate(templateKey.toStdString(), error, &edit)) {
        return QString();
    }

    return edit.path;
}

QString
App::publishTemplate(const QString& /*templateKey*/)
{
    // The binding's inject code calls publishTemplateChecked() and never reaches this body.
    return QString();
}

QString
App::updateTemplateToLatestChecked(const QString& templateKey,
                                   QString* error)
{
    if (QThread::currentThread() != qApp->thread()) {
        *error = QString::fromUtf8("updateTemplateToLatest() can only be called from the main thread");

        return QString();
    }
    AppInstancePtr app = getInternalApp();
    const ProjectPtr project = app->getProject();
    std::optional<Serialization::TemplateImportSerialization> entry;
    for (const Serialization::TemplateImportSerialization& import : project->getTemplateImports()) {
        if (import.key == templateKey.toStdString()) {
            entry = import;
        }
    }
    if (!entry) {
        *error = QString::fromUtf8("The project has no template import named %1").arg(templateKey);

        return QString();
    }
    const QString dir = QString::fromStdString(entry->publishDir);
    const std::optional<ProjectMerge::TemplateVersion> latest = ProjectMerge::findLatestVersion(dir, entry->templateId);
    if (!latest) {
        *error = QString::fromUtf8("No version of the template %1 is published in \"%2\"").arg(QString::fromStdString(entry->name)).arg(dir);

        return QString();
    }
    ProjectMerge::UpdateResult result;
    app->runTemplateEdit(QString::fromUtf8("Update from Template"), [&](TemplateEdit* edit) {
        result = ProjectMerge::updateFromTemplate(project, entry->key, latest->path, edit->applier);

        return result.ok;
    });
    if (result.status == ProjectMerge::UpdateStatus::eError) {
        *error = result.error;

        return QString();
    }

    return ProjectTemplateDetail::updateStatusName(result.status);
}

QString
App::updateTemplateToLatest(const QString& /*templateKey*/)
{
    // The binding's inject code calls updateTemplateToLatestChecked() and never reaches this body.
    return QString();
}

///Close the current project but keep the window
bool
App::resetProject()
{
    // Resetting may save the project first, and that save may wait for an auto-save whose save
    // callback needs the Python lock.
    PythonGILUnlocker pgl;

    return getInternalApp()->resetProject();
}

///Reset + close window, quit if last window
bool
App::closeProject()
{
    // Closing may save the project first, and that save may wait for an auto-save whose save
    // callback needs the Python lock.
    PythonGILUnlocker pgl;

    return getInternalApp()->closeProject();
}

///Opens a new window
App*
App::newProject()
{
    AppInstancePtr app  = getInternalApp()->newProject();

    if (!app) {
        return 0;
    }

    return new App(app);
}

std::list<QString>
App::getViewNames() const
{
    std::list<QString> ret;
    const std::vector<std::string>& v = getInternalApp()->getProject()->getProjectViewNames();

    for (std::size_t i = 0; i < v.size(); ++i) {
        ret.push_back( QString::fromUtf8( v[i].c_str() ) );
    }

    return ret;
}

void
App::addProjectLayer(const ImageLayer& layer)
{
    std::string error;

    getInternalApp()->getProject()->addLayer(layer.getInternalComps(), LayerRegistryEntry::eOriginUser, &error);
}

std::list<ImageLayer>
App::getProjectLayers() const
{
    std::list<ImageLayer> ret;
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot = getInternalApp()->getProject()->getLayerRegistrySnapshot();

    for (std::vector<LayerRegistryEntry>::const_iterator it = snapshot->begin(); it != snapshot->end(); ++it) {
        ret.push_back(ImageLayer(it->desc));
    }

    return ret;
}

ImageLayer*
App::getProjectLayer(const QString& id) const
{
    ImageLayerDesc desc;

    if (!getInternalApp()->getProject()->findLayer(id.toStdString(), &desc)) {
        return 0;
    }

    return new ImageLayer(desc);
}

ImageLayer*
App::addProjectLayer(const QString& id,
                     const QStringList& channels)
{
    std::string idStr = id.toStdString();
    std::vector<std::string> chans;

    chans.reserve(channels.size());
    for (QStringList::const_iterator it = channels.begin(); it != channels.end(); ++it) {
        chans.push_back(it->toStdString());
    }

    ImageLayerDesc desc(idStr, chans);
    std::string error;
    LayerRegistry::AddResultEnum res = getInternalApp()->getProject()->addLayer(desc, LayerRegistryEntry::eOriginUser, &error);

    if (res == LayerRegistry::eAddResultRefused) {
        PyErr_SetString(PyExc_ValueError, error.c_str());
        return 0;
    }

    ImageLayerDesc found;
    if (getInternalApp()->getProject()->findLayer(idStr, &found)) {
        return new ImageLayer(found);
    }

    PyErr_SetString(PyExc_ValueError, error.empty() ? "Unknown layer." : error.c_str());
    return 0;
}

bool
App::removeProjectLayer(const QString& id)
{
    std::string error;

    return getInternalApp()->getProject()->removeLayer(id.toStdString(), &error);
}

UndoStepContext*
App::newUndoStep(const QString& label)
{
    return new UndoStepContext(getInternalApp(), label.toStdString());
}

NoUndoStepContext*
App::noUndoStep()
{
    return new NoUndoStepContext(getInternalApp());
}

UndoStepContext::UndoStepContext(const AppInstancePtr& app,
                                 const std::string& label)
    : _app(app)
    , _label(label)
    , _steps()
{
}

UndoStepContext::~UndoStepContext()
{
    // Innermost first: the history closes whichever step is innermost, and std::vector does not
    // specify the order it destroys its elements in.
    while (!_steps.empty()) {
        _steps.pop_back();
    }
}

void
UndoStepContext::enter()
{
    _steps.push_back(std::make_shared<UndoStepGuard>(_app.lock(), _label, true));
}

void
UndoStepContext::exit()
{
    if (!_steps.empty()) {
        _steps.pop_back();
    }
}

NoUndoStepContext::NoUndoStepContext(const AppInstancePtr& app)
    : _app(app)
    , _suppressions()
{
}

NoUndoStepContext::~NoUndoStepContext()
{
    while (!_suppressions.empty()) {
        _suppressions.pop_back();
    }
}

void
NoUndoStepContext::enter()
{
    _suppressions.push_back(std::make_shared<UndoSuppressGuard>(_app.lock()));
}

void
NoUndoStepContext::exit()
{
    if (!_suppressions.empty()) {
        _suppressions.pop_back();
    }
}

NATRON_PYTHON_NAMESPACE_EXIT
NATRON_NAMESPACE_EXIT
