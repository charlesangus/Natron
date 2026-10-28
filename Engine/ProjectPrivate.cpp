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

#include "ProjectPrivate.h"

#include <cassert>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <QDebug>
#include <QTimer>
#include <QDateTime>
#include <QFile>
#include <QDir>

#include "Global/PythonUtils.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/UndoRecorder.h"
#include "Engine/ViewerInstance.h"

#include "Serialization/KnobSerialization.h"
#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

ProjectPrivate::ProjectPrivate(Project* project)
    : _publicInterface(project)
    , projectLock()
    , hasProjectBeenSavedByUser(false)
    , loadedFromFolder(false)
    , ageSinceLastSave(QDateTime::currentDateTime())
    , lastAutoSave()
    , projectCreationTime(ageSinceLastSave.toMSecsSinceEpoch())
    , builtinFormats()
    , additionalFormats()
    , formatMutex()
    , layers(new LayerRegistry())
    , envVars()
    , projectName()
    , projectPath()
    , formatKnob()
    , addFormatKnob()
    , previewMode()
    , ocioConfig()
    , ocioConfigFile()
    , workingSpace()
    , colorSpace8Bit()
    , colorSpace16Bit()
    , colorSpaceLog()
    , colorSpaceFloat()
    , viewerDisplay()
    , viewerView()
    , colorManagement(std::make_shared<ProjectColorManagement>())
    , suppressColorManagementRefresh(false)
    , ocioConfigErrorMutex()
    , ocioConfigError()
    , natronVersion()
    , originalAuthorName()
    , lastAuthorName()
    , projectCreationDate()
    , saveDate()
    , onProjectLoadCB()
    , onProjectSaveCB()
    , onProjectCloseCB()
    , onNodeCreated()
    , onNodeDeleted()
    , timeline(new TimeLine(project))
    , autoSetProjectFormat(appPTR->getCurrentSettings()->isAutoProjectFormatEnabled())
    , isLoadingProjectMutex()
    , isLoadingProject(false)
    , isLoadingProjectInternal(false)
    , isSavingProjectMutex()
    , isSavingProject(false)
    , savingThread(0)
    , savingFinishedCond()
    , savesWaiting(0)
    , mainThreadSaves(0)
    , autoSaveTimer(new QTimer())
    , projectClosing(false)
    , tlsData(new TLSHolder<Project::ProjectTLSData>())

{
    autoSaveTimer->setSingleShot(true);
}

bool
ProjectPrivate::restoreFromSerializedData(qint64 creationDate,
                                          const std::list<Format>& serializedAdditionalFormats,
                                          const std::list<LayerRegistryEntry>& serializedLayers,
                                          const std::list<Serialization::KnobSerialization>& projectSerializedValues,
                                          SequenceTime currentTime,
                                          const std::list<Serialization::NodeSerializationPtr>& serializedNodes,
                                          const QString& name,
                                          const QString& path,
                                          bool* mustSave)
{
    /*1st OFF RESTORE THE PROJECT KNOBS*/
    bool ok;
    {
        CreatingNodeTreeFlag_RAII creatingNodeTreeFlag( _publicInterface->getApp() );

        projectCreationTime = creationDate;

        _publicInterface->getApp()->updateProjectLoadStatus( tr("Restoring project settings...") );

        /*we must restore the entries in the combobox before restoring the value*/
        std::vector<ChoiceOption> entries;

        for (std::list<Format>::const_iterator it = builtinFormats.begin(); it != builtinFormats.end(); ++it) {
            QString formatStr = ProjectPrivate::generateStringFromFormat(*it);
            if ( !it->getName().empty() ) {
                entries.push_back( ChoiceOption(it->getName(), formatStr.toStdString(), "") );
            } else {
                entries.push_back( ChoiceOption( formatStr.toStdString() ) );
            }
        }

        for (std::list<Format>::const_iterator it = serializedAdditionalFormats.begin(); it != serializedAdditionalFormats.end(); ++it) {
            QString formatStr = ProjectPrivate::generateStringFromFormat(*it);
            if ( !it->getName().empty() ) {
                entries.push_back( ChoiceOption(it->getName(), formatStr.toStdString(), "") );
            } else {
                entries.push_back( ChoiceOption( formatStr.toStdString() ) );
            }
        }
        additionalFormats = serializedAdditionalFormats;
        ++formatsGeneration;
        Node::invalidateAllHashes("project formats changed");

        formatKnob->populateChoices(entries);
        autoSetProjectFormat = false;

        // Layers must exist before any node knob (referencing a layer by ID) is restored.
        layers.reset(new LayerRegistry());
        for (std::list<LayerRegistryEntry>::const_iterator it = serializedLayers.begin(); it != serializedLayers.end(); ++it) {
            layers->add(it->desc, it->origin, 0);
        }

        // The variables are created with their saved values, so the by-name match below skips them.
        restoreRFVVariables(projectSerializedValues);

        const std::vector<KnobIPtr> & projectKnobs = _publicInterface->getKnobs();

        _publicInterface->resetOCIOConfigKnobsForRestore();

        /// 1) restore project's knobs.
        for (U32 i = 0; i < projectKnobs.size(); ++i) {
            ///try to find a serialized value for this knob
            for (std::list<Serialization::KnobSerialization>::const_iterator it = projectSerializedValues.begin(); it != projectSerializedValues.end(); ++it) {
                if (it->isUserKnob) {
                    continue;
                }
                if (NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(it->name) == projectKnobs[i]->getName()) {
                    // Non persistent knobs are loaded too when the project saved a value for them.
                    const Serialization::KnobSerialization completed = completeKnobSerializationForKnob(*projectKnobs[i], *it);
                    KnobIPtr serializedKnob = createDetachedKnobFromSerialization(completed);
                    KnobChoice* isChoice = dynamic_cast<KnobChoice*>( projectKnobs[i].get() );
                    if (isChoice) {
                        KnobChoice* choiceSerialized = dynamic_cast<KnobChoice*>(serializedKnob.get());
                        if (choiceSerialized && completed.choiceOptionID) {
                            const std::string& optionID = *completed.choiceOptionID;
                            int id = isChoice->choiceRestorationId(choiceSerialized, optionID);
                            isChoice->choiceRestoration(choiceSerialized, optionID, id);
                        }
                    } else if (serializedKnob) {
                        projectKnobs[i]->clone(serializedKnob);
                    }
                    // Its link groups are rebuilt with the nodes' once every node exists.
                    projectKnobs[i]->storeLinkGroupsFromSerialization(*it);
                    break;
                }
            }
            if (projectKnobs[i] == envVars) {
                ///For eAppTypeBackgroundAutoRunLaunchedFromGui don't change the project path since it is controlled
                ///by the main GUI process
                if (appPTR->getAppType() != AppManager::eAppTypeBackgroundAutoRunLaunchedFromGui) {
                    autoSetProjectDirectory(path);
                }
            }
        }

        // Choice ids that did not match the previous config's entries are still pending and
        // re-match here, so the order in which the Color page knobs restored does not matter.
        _publicInterface->refreshColorManagement(true, true);

        /// 2) restore the timeline
        timeline->seekFrame(currentTime, false, 0, eTimelineChangeReasonOtherSeek);

        /// 3) Restore the nodes

        std::map<std::string, bool> processedModules;
        ok = NodeCollection::restoreFromSerialization(serializedNodes,
                                                      _publicInterface->shared_from_this(), true, &processedModules);
        for (std::map<std::string, bool>::iterator it = processedModules.begin(); it != processedModules.end(); ++it) {
            if (it->second) {
                *mustSave = true;
                break;
            }
        }

        NodesList allNodes;
        _publicInterface->getNodes_recursive(allNodes, false);

        // Every node exists and is wired by now, so a link target that is not found is missing
        // from the file: the link is left unset and the rest of the project still loads.
        const std::map<std::string, std::string> oldNewScriptNamesMapping;
        for (NodesList::iterator n = allNodes.begin(); n != allNodes.end(); ++n) {
            std::vector<KnobIPtr> knobs = (*n)->getKnobs(); // copy the knobs vector
            for (std::vector<KnobIPtr>::iterator k = knobs.begin(); k != knobs.end(); ++k) {
                std::vector<std::string> missingTargets;
                (*k)->restoreLinks(allNodes, oldNewScriptNamesMapping, false, &missingTargets);
                for (std::size_t i = 0; i < missingTargets.size(); ++i) {
                    _publicInterface->addLoadWarning(tr("%1.%2 \u2192 %3: link target not found; the link was left out")
                                                         .arg(QString::fromUtf8((*n)->getFullyQualifiedName().c_str()))
                                                         .arg(QString::fromUtf8((*k)->getName().c_str()))
                                                         .arg(QString::fromUtf8(missingTargets[i].c_str())));
                }
            }
        }
        // Curve points linked to tracks resolve them by name once every tracker exists.
        for (NodesList::iterator n = allNodes.begin(); n != allNodes.end(); ++n) {
            RotoCurveContextPtr curves = (*n)->getCurveContext();
            if (curves) {
                curves->resolveTrackLinks();
            }
        }

        // The variables store their link groups first, so that the groups they share with node knobs
        // are rebuilt together.
        restoreRFVVariableLinksAndExpressions(projectSerializedValues, allNodes);

        KnobHelper::restoreLinkGroups(allNodes, false, _publicInterface->getKnobs());

        // An expression naming a node restored after its own knob was set aside as invalid; with
        // the whole tree restored, what is still invalid refers to something the file lacks.
        _publicInterface->getApp()->recheckInvalidExpressions();
        for (NodesList::iterator n = allNodes.begin(); n != allNodes.end(); ++n) {
            const std::vector<KnobIPtr>& knobs = (*n)->getKnobs();
            for (std::vector<KnobIPtr>::const_iterator k = knobs.begin(); k != knobs.end(); ++k) {
                for (int i = 0; i < (*k)->getDimension(); ++i) {
                    std::string error;
                    if ((*k)->getExpression(i).empty() || (*k)->isExpressionValid(i, &error)) {
                        continue;
                    }
                    _publicInterface->addLoadWarning(tr("%1.%2: the expression could not be evaluated (%3); it is kept but inactive")
                                                         .arg(QString::fromUtf8((*n)->getFullyQualifiedName().c_str()))
                                                         .arg(QString::fromUtf8((*k)->getName().c_str()))
                                                         .arg(QString::fromUtf8(error.c_str())));
                }
            }
        }

        _publicInterface->getApp()->updateProjectLoadStatus( tr("Restoring graph stream preferences...") );
    } // CreatingNodeTreeFlag_RAII creatingNodeTreeFlag(_publicInterface->getApp());

    _publicInterface->forceComputeInputDependentDataOnAllTrees();

    // Channel selectors were already rebuilt by forceComputeInputDependentDataOnAllTrees()
    // above; only notify listeners of the final registry state, exactly once.
    _publicInterface->emitProjectLayersChangedSignal();

    QDateTime time = QDateTime::currentDateTime();
    autoSetProjectFormat = false;
    hasProjectBeenSavedByUser = true;
    setProjectFilename(name.toStdString());
    setProjectPath(path.toStdString());
    ageSinceLastSave = time;
    lastAutoSave = time;

    return ok;
} // restoreFromSerializedData

void
ProjectPrivate::restoreRFVVariables(const std::list<Serialization::KnobSerialization>& projectSerializedValues)
{
    bool created = false;
    for (std::list<Serialization::KnobSerialization>::const_iterator it = projectSerializedValues.begin(); it != projectSerializedValues.end(); ++it) {
        if (!it->isUserKnob) {
            continue;
        }
        const QString knobName = QString::fromUtf8(it->name.c_str());
        KnobIPtr found = _publicInterface->getKnobByName(NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(it->name));
        if (found && !found->isUserKnob()) {
            _publicInterface->addLoadWarning(tr("Project variable %1: a project parameter has that name; the variable was dropped").arg(knobName));
            continue;
        }
        KnobIPtr knob = createUserKnobFromSerialization(_publicInterface, *it, found);
        if (!knob) {
            _publicInterface->addLoadWarning(tr("Project variable %1: its type %2 cannot be a parameter; the variable was dropped")
                                                 .arg(knobName)
                                                 .arg(QString::fromUtf8(it->typeName.c_str())));
            continue;
        }
        if (!rfvGroup->addKnob(knob)) {
            _publicInterface->addLoadWarning(tr("Project variable %1: its type %2 cannot be a variable; the variable was dropped")
                                                 .arg(knobName)
                                                 .arg(QString::fromUtf8(it->typeName.c_str())));
            if (!found) {
                _publicInterface->deleteKnob(knob.get(), false);
            }
            continue;
        }
        created = true;
    }
    if (created) {
        _publicInterface->recreateUserKnobs(true);
    }
}

void
ProjectPrivate::restoreRFVVariableLinksAndExpressions(const std::list<Serialization::KnobSerialization>& projectSerializedValues,
                                                      const NodesList& allNodes)
{
    const std::map<std::string, std::string> oldNewScriptNamesMapping;
    const KnobsVec variables = rfvGroup->getChildren();
    for (std::list<Serialization::KnobSerialization>::const_iterator it = projectSerializedValues.begin(); it != projectSerializedValues.end(); ++it) {
        if (!it->isUserKnob) {
            continue;
        }
        const std::string name = NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(it->name);
        KnobIPtr knob;
        for (KnobsVec::const_iterator v = variables.begin(); v != variables.end(); ++v) {
            if ((*v)->getName() == name) {
                knob = *v;
                break;
            }
        }
        if (!knob) {
            // restoreRFVVariables() dropped it with a warning.
            continue;
        }
        knob->storeLinksFromSerialization(*it);
        knob->restoreExpressionsFromSerialization(*it, oldNewScriptNamesMapping);
        std::vector<std::string> missingTargets;
        knob->restoreLinks(allNodes, oldNewScriptNamesMapping, false, &missingTargets);
        for (std::size_t i = 0; i < missingTargets.size(); ++i) {
            _publicInterface->addLoadWarning(tr("Project variable %1 \u2192 %2: link target not found; the link was left out")
                                                 .arg(QString::fromUtf8(name.c_str()))
                                                 .arg(QString::fromUtf8(missingTargets[i].c_str())));
        }
    }
}

void
ProjectPrivate::clearRFVVariables()
{
    const KnobsVec variables = rfvGroup->getChildren();
    for (KnobsVec::const_iterator it = variables.begin(); it != variables.end(); ++it) {
        _publicInterface->deleteKnob(it->get(), true);
    }
}

bool
ProjectPrivate::findFormat(int index,
                           Format* format) const
{
    if ( index >= (int)( builtinFormats.size() + additionalFormats.size() ) ) {
        return false;
    }

    int i = 0;
    if ( index >= (int)builtinFormats.size() ) {
        ///search in the additional formats
        index -= builtinFormats.size();

        for (std::list<Format>::const_iterator it = additionalFormats.begin(); it != additionalFormats.end(); ++it) {
            if (i == index) {
                assert( !it->isNull() );
                *format = *it;

                return true;
            }
            ++i;
        }
    } else {
        ///search in the builtins formats
        for (std::list<Format>::const_iterator it = builtinFormats.begin(); it != builtinFormats.end(); ++it) {
            if (i == index) {
                assert( !it->isNull() );
                *format = *it;

                return true;
            }
            ++i;
        }
    }

    return false;
}

void
ProjectPrivate::autoSetProjectDirectory(const QString& path)
{
    std::string pathCpy = path.toStdString();

    if ( !pathCpy.empty() && (pathCpy[pathCpy.size() - 1] == '/') ) {
        pathCpy.erase(pathCpy.size() - 1, 1);
    }
    std::string env;
    try {
        env = envVars->getValue();
    } catch (...) {
        // ignore
    }

    std::list<std::vector<std::string> > table;
    envVars->decodeFromKnobTableFormat(env, &table);

    ///If there was already a OCIO variable, update it, otherwise create it
    bool foundProject = false;
    for (std::list<std::vector<std::string> >::iterator it = table.begin(); it != table.end(); ++it) {
        if ( (*it)[0] == NATRON_PROJECT_ENV_VAR_NAME ) {
            (*it)[1] = pathCpy;
            foundProject = true;
            break;
        }
    }
    if (!foundProject) {
        std::vector<std::string> vec(2);
        vec[0] = NATRON_PROJECT_ENV_VAR_NAME;
        vec[1] = pathCpy;
        table.push_back(vec);
    }


    std::string newEnv = envVars->encodeToKnobTableFormat(table);
    if (env != newEnv) {
        if ( appPTR->getCurrentSettings()->isAutoFixRelativeFilePathEnabled() ) {
            _publicInterface->fixRelativeFilePaths(NATRON_PROJECT_ENV_VAR_NAME, pathCpy, false);
        }
        envVars->setValue(newEnv);
    }
}

std::string
ProjectPrivate::runOnProjectSaveCallback(const std::string& filename,
                                         bool autoSave)
{
    UndoSuppressGuard noUndo(_publicInterface->getApp());
    std::string onProjectSave = _publicInterface->getOnProjectSaveCB();

    if ( !onProjectSave.empty() ) {
        std::vector<std::string> args;
        std::string error;
        try {
            NATRON_PYTHON_NAMESPACE::getFunctionArguments(onProjectSave, &error, &args);
        } catch (const std::exception& e) {
            _publicInterface->getApp()->appendToScriptEditor( std::string("Failed to get signature of onProjectSave callback: ")
                                                              + e.what() );

            return filename;
        }

        if ( !error.empty() ) {
            _publicInterface->getApp()->appendToScriptEditor("Failed to get signature of onProjectSave callback: " + error);

            return filename;
        } else {
            std::string signatureError;
            signatureError.append("The on project save callback supports the following signature(s):\n");
            signatureError.append("- callback(filename,app,autoSave)");
            if ( (args.size() != 3) || (args[0] != "filename") || (args[1] != "app") || (args[2] != "autoSave") ) {
                _publicInterface->getApp()->appendToScriptEditor("Wrong signature of onProjectSave callback: " + signatureError);

                return filename;
            }
            std::string appID = _publicInterface->getApp()->getAppIDString();
            std::stringstream ss;
            if (appID != "app") {
                ss << "app = " << appID << "\n";
            }
            ss << "ret = " << onProjectSave << "(\"" << filename << "\"," << appID << ",";
            if (autoSave) {
                ss << "True)\n";
            } else {
                ss << "False)\n";
            }

            onProjectSave = ss.str();
            std::string err;
            std::string output;
            if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(onProjectSave, &err, &output) ) {
                _publicInterface->getApp()->appendToScriptEditor("Failed to run onProjectSave callback: " + err);

                return filename;
            } else {
                PythonGILLocker pgl;

                PyObject* mainModule = NATRON_PYTHON_NAMESPACE::getMainModule();
                assert(mainModule);
                PyObject* ret = PyObject_GetAttrString(mainModule, "ret");
                if (!ret) {
                    return filename;
                }
                std::string filePath = filename;
                if (ret) {
                    filePath = NATRON_PYTHON_NAMESPACE::PyStringToStdString(ret);
                    std::string script = "del ret\n";
                    bool ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0);
                    assert(ok);
                    if (!ok) {
                        throw std::runtime_error("ProjectPrivate::runOnProjectSaveCallback(): interpretPythonScript(" + script + ") failed!");
                    }
                }
                if ( !output.empty() ) {
                    _publicInterface->getApp()->appendToScriptEditor(output);
                }

                return filePath;
            }
        }
    }

    return filename;
} // ProjectPrivate::runOnProjectSaveCallback

void
ProjectPrivate::runOnProjectCloseCallback()
{
    UndoSuppressGuard noUndo(_publicInterface->getApp());
    std::string onProjectClose = _publicInterface->getOnProjectCloseCB();

    if ( !onProjectClose.empty() ) {
        std::vector<std::string> args;
        std::string error;
        try {
            NATRON_PYTHON_NAMESPACE::getFunctionArguments(onProjectClose, &error, &args);
        } catch (const std::exception& e) {
            _publicInterface->getApp()->appendToScriptEditor( std::string("Failed to get signature of onProjectClose callback: ")
                                                              + e.what() );

            return;
        }

        if ( !error.empty() ) {
            _publicInterface->getApp()->appendToScriptEditor("Failed to get signature of onProjectClose callback: " + error);

            return;
        }

        std::string signatureError;
        signatureError.append("The on project close callback supports the following signature(s):\n");
        signatureError.append("- callback(app)");
        if ( (args.size() != 1) || (args[0] != "app") ) {
            _publicInterface->getApp()->appendToScriptEditor("Wrong signature of onProjectClose callback: " + signatureError);

            return;
        }
        std::string appID = _publicInterface->getApp()->getAppIDString();
        std::string script;
        if (appID != "app") {
            script = script +  "app = " + appID;
        }
        script = script + "\n" + onProjectClose + "(" + appID + ")\n";
        std::string err;
        std::string output;
        if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output) ) {
            _publicInterface->getApp()->appendToScriptEditor("Failed to run onProjectClose callback: " + err);
        } else {
            if ( !output.empty() ) {
                _publicInterface->getApp()->appendToScriptEditor(output);
            }
        }
    }
} // ProjectPrivate::runOnProjectCloseCallback

void
ProjectPrivate::runOnProjectLoadCallback()
{
    UndoSuppressGuard noUndo(_publicInterface->getApp());
    std::string cb = _publicInterface->getOnProjectLoadCB();

    if ( !cb.empty() ) {
        std::vector<std::string> args;
        std::string error;
        try {
            NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
        } catch (const std::exception& e) {
            _publicInterface->getApp()->appendToScriptEditor( std::string("Failed to get signature of onProjectLoaded callback: ")
                                                              + e.what() );

            return;
        }

        if ( !error.empty() ) {
            _publicInterface->getApp()->appendToScriptEditor("Failed to get signature of onProjectLoaded callback: " + error);

            return;
        }

        std::string signatureError;
        signatureError.append("The on  project loaded callback supports the following signature(s):\n");
        signatureError.append("- callback(app)");
        if ( (args.size() != 1) || (args[0] != "app") ) {
            _publicInterface->getApp()->appendToScriptEditor("Wrong signature of onProjectLoaded callback: " + signatureError);

            return;
        }

        std::string appID = _publicInterface->getApp()->getAppIDString();
        std::string script;
        if (appID != "app") {
            script =  script + "app = " + appID;
        }
        script =  script + "\n" + cb + "(" + appID + ")\n";
        std::string err;
        std::string output;
        if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output) ) {
            _publicInterface->getApp()->appendToScriptEditor("Failed to run onProjectLoaded callback: " + err);
        } else {
            if ( !output.empty() ) {
                _publicInterface->getApp()->appendToScriptEditor(output);
            }
        }
    }
} // ProjectPrivate::runOnProjectLoadCallback

void
ProjectPrivate::setProjectFilename(const std::string& filename)
{
    if (projectName->getValue() != filename) {
        projectName->setValue(filename);
    }
}

std::string
ProjectPrivate::getProjectFilename() const
{
    return projectName->getValue();
}

void
ProjectPrivate::setProjectPath(const std::string& path)
{
    // A save that keeps the path writes nothing, so it is not refused while a render reads the project.
    if (projectPath->getValue() != path) {
        projectPath->setValue(path);
    }
}

std::string
ProjectPrivate::getProjectPath() const
{
    return projectPath->getValue();
}

NATRON_NAMESPACE_EXIT
