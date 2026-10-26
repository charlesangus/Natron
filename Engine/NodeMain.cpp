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

#include "NodePrivate.h"

#include <set>

#include <QCoreApplication>
#include <QThread>

#include <ofxImageEffect.h>
#include <ofxNatron.h>

#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/GenericSchedulerThreadWatcher.h"
#include "Engine/LibraryBinary.h"
#include "Engine/NodeGroup.h"
#include "Engine/NodeGuiI.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/PrecompNode.h"
#include "Engine/Project.h"

#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLayer.h"
#include "Engine/TrackerContext.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

void
Node::load(const CreateNodeArgs& args)
{
    ///Called from the main thread. MT-safe
    assert( QThread::currentThread() == qApp->thread() );

    ///cannot load twice
    assert(!_imp->effect);
    _imp->isPartOfProject = !args.getProperty<bool>(kCreateNodeArgsPropOutOfProject);

    NodeCollectionPtr group = getGroup();
    std::string multiInstanceParentName = args.getProperty<std::string>(kCreateNodeArgsPropMultiInstanceParentName);
    if ( !multiInstanceParentName.empty() ) {
        _imp->multiInstanceParentName = multiInstanceParentName;
        _imp->isMultiInstance = false;
        fetchParentMultiInstancePointer();
    }


    _imp->wasCreatedSilently = args.getProperty<bool>(kCreateNodeArgsPropSilent);

    NodePtr thisShared = shared_from_this();
    LibraryBinary* binary = _imp->plugin->getLibraryBinary();
    std::pair<bool, EffectBuilder> func;
    if (binary) {
        func = binary->findFunction<EffectBuilder>("BuildEffect");
    }

    Serialization::NodeSerializationPtr serialization = args.getProperty<Serialization::NodeSerializationPtr>(kCreateNodeArgsPropNodeSerialization);

    bool isSilentCreation = args.getProperty<bool>(kCreateNodeArgsPropSilent);

    bool hasDefaultFilename = false;
    {
        std::vector<std::string> defaultParamValues = args.getPropertyN<std::string>(kCreateNodeArgsPropNodeInitialParamValues);
        std::vector<std::string>::iterator foundFileName  = std::find(defaultParamValues.begin(), defaultParamValues.end(), std::string(kOfxImageEffectFileParamName));
        if (foundFileName != defaultParamValues.end()) {
            std::string propName(kCreateNodeArgsPropParamValue);
            propName += "_";
            propName += kOfxImageEffectFileParamName;
            hasDefaultFilename = !args.getProperty<std::string>(propName).empty();
        }
    }
    bool canOpenFileDialog = !isSilentCreation && !serialization && _imp->isPartOfProject && !hasDefaultFilename && getGroup();

    std::string argFixedName = args.getProperty<std::string>(kCreateNodeArgsPropNodeInitialName);


    if (func.first) {
        /*
           We are creating a built-in plug-in
         */
        _imp->effect.reset( func.second(thisShared) );
        assert(_imp->effect);

        initNodeScriptName(serialization.get(), QString::fromUtf8(argFixedName.c_str()));

        _imp->effect->initializeData();

        createRotoContextConditionnally();
        createTrackerContextConditionnally();
        initializeInputs();
        initializeKnobs(serialization.get() != 0);

        refreshAcceptedBitDepths();

        if (serialization) {

            _imp->effect->onKnobsAboutToBeLoaded(serialization);
            fromSerialization(*serialization);
        }
        setValuesFromSerialization(args);
    } else {
        // ofx plugin
        _imp->effect = appPTR->createOFXEffect(thisShared, args);
        assert(_imp->effect);
    }

    _imp->effect->initializeOverlayInteract();

    if (serialization) {
        const Serialization::NodeLayoutSerialization& layout = serialization->layout;
        if (layout.hasPosition) {
            setPosition(layout.x, layout.y);
        }
        if (layout.hasSize) {
            setSize(layout.width, layout.height);
        }
        if (layout.hasColor) {
            setColor(layout.colorR, layout.colorG, layout.colorB);
        }
        if (layout.hasOverlayColor) {
            setOverlayColor(layout.overlayR, layout.overlayG, layout.overlayB);
        }
    }

    if ( _imp->supportedDepths.empty() ) {
        //From the spec:
        //The default for a plugin is to have none set, the plugin \em must define at least one in its describe action.
        throw std::runtime_error("Plug-in does not support 8bits, 16bits or 32bits floating point image processing.");
    }

    /*
       Set modifiable props
     */
    refreshDynamicProperties();
    onOpenGLEnabledKnobChangedOnProject(getApp()->getProject()->isGPURenderingEnabledInProject());

    if ( isTrackerNodePlugin() ) {
        _imp->isMultiInstance = true;
    }

    /*if (isMultiInstanceChild && !args.serialization) {
        updateEffectLabelKnob( QString::fromUtf8( getScriptName().c_str() ) );
     }*/
    restoreSublabel();

    declarePythonFields();
    if (getCurveContext()) {
        declareRotoPythonField();
    }
    if ( getTrackerContext() ) {
        declareTrackerPythonField();
    }


    if (group) {
        group->notifyNodeActivated(thisShared);
    }

    if (!serialization) {
        computeHash();
    }

    assert(_imp->effect);

    _imp->pluginSafety = _imp->effect->renderThreadSafety();
    _imp->currentThreadSafety = _imp->pluginSafety;


    bool isLoadingPyPlug = getApp()->isCreatingPythonGroup();

    // Messages posted while a silently created node is incomplete are dropped, so the node counts as
    // created before the effect reports on its loaded state.
    _imp->nodeCreated = true;

    _imp->effect->onEffectCreated(canOpenFileDialog, args);

    if ( !getApp()->isCreatingNodeTree() ) {
        refreshAllInputRelatedData(!serialization);
    }

    // After the knobs are loaded, so a pasted or duplicated node's saved config does not win.
    getApp()->getProject()->pushOCIOConfigToNode(thisShared);

    _imp->runOnNodeCreatedCB(!serialization && !isLoadingPyPlug);
} // load


void
Node::setValuesFromSerialization(const CreateNodeArgs& args)
{

    std::vector<std::string> params = args.getPropertyN<std::string>(kCreateNodeArgsPropNodeInitialParamValues);

    assert( QThread::currentThread() == qApp->thread() );
    assert(_imp->knobsInitialized);
    const std::vector<KnobIPtr> & nodeKnobs = getKnobs();

    for (std::size_t i = 0; i < params.size(); ++i) {
        for (U32 j = 0; j < nodeKnobs.size(); ++j) {
            if (nodeKnobs[j]->getName() == params[i]) {

                KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>(nodeKnobs[j].get());
                KnobIntBase* isInt = dynamic_cast<KnobIntBase*>(nodeKnobs[j].get());
                KnobDoubleBase* isDbl = dynamic_cast<KnobDoubleBase*>(nodeKnobs[j].get());
                KnobStringBase* isStr = dynamic_cast<KnobStringBase*>(nodeKnobs[j].get());
                int nDims = nodeKnobs[j]->getDimension();

                std::string propName = kCreateNodeArgsPropParamValue;
                propName += "_";
                propName += params[i];
                if (isBool) {
                    std::vector<bool> v = args.getPropertyN<bool>(propName);
                    nDims = std::min((int)v.size(), nDims);
                    for (int d = 0; d < nDims; ++d) {
                        isBool->setValue(v[d], ViewSpec(0), d);
                    }
                } else if (isInt) {
                    std::vector<int> v = args.getPropertyN<int>(propName);
                    nDims = std::min((int)v.size(), nDims);
                    for (int d = 0; d < nDims; ++d) {
                        isInt->setValue(v[d], ViewSpec(0), d);
                    }
                } else if (isDbl) {
                    std::vector<double> v = args.getPropertyN<double>(propName);
                    nDims = std::min((int)v.size(), nDims);
                    for (int d = 0; d < nDims; ++d) {
                        isDbl->setValue(v[d], ViewSpec(0), d );
                    }
                } else if (isStr) {
                    std::vector<std::string> v = args.getPropertyN<std::string>(propName);
                    nDims = std::min((int)v.size(), nDims);
                    for (int d = 0; d < nDims; ++d) {
                        isStr->setValue(v[d],ViewSpec(0), d );
                    }
                }
                break;
            }
        }
    }


}

void
Node::restoreUserKnobs(const Serialization::NodeSerialization& serialization)
{
    const std::list<Serialization::GroupKnobSerialization>& userPages = serialization.userPages;

    for (std::list<Serialization::GroupKnobSerialization>::const_iterator it = userPages.begin(); it != userPages.end(); ++it) {
        KnobIPtr found = getKnobByName(it->name);
        KnobPagePtr page;
        if (!found) {
            page = AppManager::createKnob<KnobPage>(_imp->effect.get(), it->label, 1, false);
            page->setAsUserKnob(true);
            page->setName(it->name);
        } else {
            page = std::dynamic_pointer_cast<KnobPage>(found);
        }
        if (page) {
            _imp->restoreUserKnobsRecursive(it->children, KnobGroupPtr(), page);
        }
    }
    setPagesOrder(serialization.pagesOrder);
    _imp->effect->onUserKnobsChanged();
}

namespace {

// Knobs a preset never changes, whatever it holds: they describe this particular node (what it is
// called, whether it is on, which file it reads or writes) rather than its look. Every reader and
// writer, native, deep or OpenFX, names its file knob kOfxImageEffectFileParamName, and a reader
// its proxy file kOfxImageEffectProxyParamName. A Precomp's file is the project it reads.
bool
isKeptByPresets(const KnobIPtr& knob)
{
    const std::string& name = knob->getName();
    if ((name == kUserLabelKnobName) || (name == kNatronOfxParamStringSublabelName) || (name == kDisableNodeKnobName)) {
        return true;
    }
    if ((name != kOfxImageEffectFileParamName) && (name != kOfxImageEffectProxyParamName) && (name != kPrecompParamProjectFilename)) {
        return false;
    }

    return dynamic_cast<KnobFile*>(knob.get()) || dynamic_cast<KnobOutputFile*>(knob.get());
}

bool
isPresetResettable(const KnobIPtr& knob)
{
    if (knob->isUserKnob() || !knob->getIsPersistent()) {
        return false;
    }
    KnobI* k = knob.get();

    return !dynamic_cast<KnobPage*>(k) && !dynamic_cast<KnobGroup*>(k) && !dynamic_cast<KnobButton*>(k) && !dynamic_cast<KnobSeparator*>(k);
}

void
removeKnobLinks(const KnobIPtr& knob)
{
    KnobIPtr aliasMaster = knob->getAliasMaster();
    if (aliasMaster) {
        knob->setKnobAsAliasOfThis(aliasMaster, false);
    }
    for (int i = 0; i < knob->getDimension(); ++i) {
        if (knob->isSlave(i)) {
            knob->unSlave(i, false);
        }
    }
}

// Drops the user knobs of a preset's user pages that this node already has a knob of the same name
// for, so that the node's own knobs keep their definition and value. Groups stay, for the new
// knobs they hold.
void
removeExistingUserKnobs(const Node& node,
                        std::list<Serialization::GroupKnobChildSerialization>* children)
{
    for (std::list<Serialization::GroupKnobChildSerialization>::iterator it = children->begin(); it != children->end();) {
        if (it->isGroup) {
            removeExistingUserKnobs(node, &it->group.children);
        } else if (node.getKnobByName(NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(it->knob.name))) {
            it = children->erase(it);
            continue;
        }
        ++it;
    }
}

} // anon namespace

void
Node::applyPresetSerialization(const Serialization::NodeSerialization& serialization,
                               PresetApplyModeEnum mode,
                               std::list<KnobIPtr>* createdUserKnobs)
{
    assert(QThread::currentThread() == qApp->thread());
    assert(_imp->knobsInitialized);

    const bool isPreset = (mode == ePresetApplyModePreset);
    Serialization::NodeSerialization presetPart;
    if (isPreset) {
        // What the preset may change: everything but the knobs it keeps, the node's own user
        // knobs and the roto shapes and tracks.
        presetPart = serialization;
        presetPart.knobs.remove_if([this](const Serialization::KnobSerialization& k) {
            const KnobIPtr knob = getKnobByName(NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(k.name));

            return knob && isKeptByPresets(knob);
        });
        for (std::list<Serialization::GroupKnobSerialization>::iterator it = presetPart.userPages.begin(); it != presetPart.userPages.end(); ++it) {
            removeExistingUserKnobs(*this, &it->children);
        }
        presetPart.hasRotoContext = false;
        presetPart.rotoContext = Serialization::RotoContextSerialization();
        presetPart.hasTrackerContext = false;
        presetPart.trackerContext = Serialization::TrackerContextSerialization();
    }
    const Serialization::NodeSerialization& applied = isPreset ? presetPart : serialization;

    std::set<std::string> savedKnobs;
    for (std::list<Serialization::KnobSerialization>::const_iterator it = applied.knobs.begin(); it != applied.knobs.end(); ++it) {
        savedKnobs.insert(NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(it->name));
    }

    const KnobsVec knobsBefore = getKnobs();
    std::set<KnobIPtr> userKnobsBefore;
    for (KnobsVec::const_iterator it = knobsBefore.begin(); it != knobsBefore.end(); ++it) {
        if ((*it)->isUserKnob()) {
            userKnobsBefore.insert(*it);
        }
    }

    for (KnobsVec::const_iterator it = knobsBefore.begin(); it != knobsBefore.end(); ++it) {
        const KnobIPtr& knob = *it;
        if (!isPresetResettable(knob)) {
            continue;
        }
        if (isPreset && isKeptByPresets(knob)) {
            continue;
        }
        const bool saved = savedKnobs.count(knob->getName()) > 0;
        removeKnobLinks(knob);
        for (int i = 0; i < knob->getDimension(); ++i) {
            // fromSerialization() replaces values and curves but only ever adds expressions.
            if (saved) {
                knob->clearExpression(i, true);
            } else {
                // The plug-in decides a knob's visibility from the values it depends on.
                knob->resetToDefaultValueWithoutSecretNessAndEnabledNess(i);
            }
        }
    }

    // Loading roto shapes or tracks appends them to the existing ones.
    if (!isPreset && _imp->rotoContext) {
        const std::list<RotoLayerPtr>& layers = _imp->rotoContext->getLayers();
        if (!layers.empty()) {
            const std::list<RotoItemPtr> items = layers.front()->getItems();
            for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
                _imp->rotoContext->removeItem(*it);
            }
        }
    }
    if (!isPreset && _imp->trackContext) {
        std::vector<TrackMarkerPtr> markers;
        _imp->trackContext->getAllMarkers(&markers);
        for (std::vector<TrackMarkerPtr>::const_iterator it = markers.begin(); it != markers.end(); ++it) {
            _imp->trackContext->removeMarker(*it);
        }
    }

    fromSerialization(applied, true);

    bool userKnobsChanged = false;
    const KnobsVec knobsAfter = getKnobs();
    for (KnobsVec::const_iterator it = knobsAfter.begin(); it != knobsAfter.end(); ++it) {
        if ((*it)->isUserKnob() && (userKnobsBefore.count(*it) == 0)) {
            userKnobsChanged = true;
            if (createdUserKnobs) {
                createdUserKnobs->push_back(*it);
            }
        }
    }

    const std::map<std::string, std::string> noRenames;
    storeKnobsLinks(applied, noRenames);
    NodesList allNodes;
    AppInstancePtr app = getApp();
    if (app) {
        app->getProject()->getActiveNodesExpandGroups(&allNodes);
    }
    // A link to a node that is gone stays unresolved rather than abort the rest of the apply.
    restoreKnobsLinks(allNodes, noRenames, false);

    if (userKnobsChanged) {
        _imp->effect->recreateUserKnobs(true);
        // recreateUserKnobs() only declares the knobs to Python when a settings panel exists, and
        // deleting a knob expects its attribute to be there.
        declarePythonFields();
    }
} // Node::applyPresetSerialization

void
Node::deleteUserKnobs(const std::list<KnobIWPtr>& knobs)
{
    assert(QThread::currentThread() == qApp->thread());

    bool deleted = false;
    // Children before the pages and groups they were created in, which delete their children too.
    for (std::list<KnobIWPtr>::const_reverse_iterator it = knobs.rbegin(); it != knobs.rend(); ++it) {
        KnobIPtr knob = it->lock();
        if (!knob || !knob->isUserKnob() || (getKnobByName(knob->getName()) != knob)) {
            continue;
        }
        _imp->effect->deleteKnob(knob.get(), true);
        deleted = true;
    }
    if (deleted) {
        _imp->effect->recreateUserKnobs(true);
    }
}

static void
refusedUserKnobLog(Node* node,
                   const std::string& knobName)
{
    LogEntry::LogEntryColor c;
    if (node->getColor(&c.r, &c.g, &c.b)) {
        c.colorSet = true;
    }
    const QString err = QCoreApplication::translate("Node", "The parameter %1 cannot be a child of its group and was dropped").arg(QString::fromUtf8(knobName.c_str()));
    appPTR->writeToErrorLog_mt_safe(QString::fromUtf8(node->getScriptName_mt_safe().c_str()), QDateTime::currentDateTime(), err, false, c);
}

void
Node::Implementation::restoreUserKnobsRecursive(const std::list<Serialization::GroupKnobChildSerialization>& knobs,
                                                const KnobGroupPtr& group,
                                                const KnobPagePtr& page)
{
    for (std::list<Serialization::GroupKnobChildSerialization>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if (it->isGroup) {
            const Serialization::GroupKnobSerialization& groupSerialization = it->group;
            KnobIPtr found = _publicInterface->getKnobByName(groupSerialization.name);
            KnobGroupPtr grp;
            if (!found) {
                grp = AppManager::createKnob<KnobGroup>(effect.get(), groupSerialization.label, 1, false);
            } else {
                grp = std::dynamic_pointer_cast<KnobGroup>(found);
                if (!grp) {
                    continue;
                }
            }
            grp->setAsUserKnob(true);
            grp->setName(groupSerialization.name);
            if (groupSerialization.isSetAsTab) {
                grp->setAsTab();
            }
            if (groupSerialization.isRFVGroup) {
                grp->setAsRFVGroup(true);
            }
            page->addKnob(grp);
            if (group) {
                if (!group->addKnob(grp)) {
                    refusedUserKnobLog(_publicInterface, grp->getName());
                    if (!found) {
                        effect->deleteKnob(grp.get(), false);
                    }
                    continue;
                }
            }
            grp->setValue(groupSerialization.isOpened);
            restoreUserKnobsRecursive(groupSerialization.children, grp, page);
        } else {
            const Serialization::KnobSerialization& knobSerialization = it->knob;
            assert(knobSerialization.isUserKnob);

            KnobIPtr found = _publicInterface->getKnobByName(NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(knobSerialization.name));
            KnobIPtr knob = createUserKnobFromSerialization(effect.get(), knobSerialization, found);
            if (!knob) {
                continue;
            }
            if (group) {
                if (!group->addKnob(knob)) {
                    refusedUserKnobLog(_publicInterface, knob->getName());
                    if (!found) {
                        effect->deleteKnob(knob.get(), false);
                    }
                    continue;
                }
            } else if (page) {
                page->addKnob(knob);
            }
        }
    }
} // Node::Implementation::restoreUserKnobsRecursive

void
Node::Implementation::storeKnobLinksRecursive(const Serialization::GroupKnobSerialization& group,
                                              const std::map<std::string, std::string>& oldNewScriptNamesMapping)
{
    for (std::list<Serialization::GroupKnobChildSerialization>::const_iterator it = group.children.begin(); it != group.children.end(); ++it) {
        if (it->isGroup) {
            storeKnobLinksRecursive(it->group, oldNewScriptNamesMapping);
        } else {
            const std::string knobName = NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(it->knob.name);
            KnobIPtr knob = _publicInterface->getKnobByName(knobName);
            if (!knob) {
                if (!_publicInterface->getEffectInstance()->getIsEditable()) {
                    // A placeholder has none of the original knobs by design; their values stay in the stored serialization.
                    continue;
                }
                LogEntry::LogEntryColor c;
                if (_publicInterface->getColor(&c.r, &c.g, &c.b)) {
                    c.colorSet = true;
                }

                QString err = tr("Could not find a parameter named %1").arg(QString::fromUtf8(knobName.c_str()));
                appPTR->writeToErrorLog_mt_safe(QString::fromUtf8( _publicInterface->getScriptName_mt_safe().c_str() ), QDateTime::currentDateTime(), err, false, c);
                continue;
            }
            knob->storeLinksFromSerialization(it->knob);
            knob->restoreExpressionsFromSerialization(it->knob, oldNewScriptNamesMapping);
        }
    }
}

void
Node::storeKnobsLinks(const Serialization::NodeSerialization& serialization,
                      const std::map<std::string, std::string>& oldNewScriptNamesMapping)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    const std::list<Serialization::KnobSerialization>& knobsValues = serialization.knobs;
    ///try to find a serialized value for this knob
    for (std::list<Serialization::KnobSerialization>::const_iterator it = knobsValues.begin(); it != knobsValues.end(); ++it) {
        const std::string knobName = NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(it->name);
        KnobIPtr knob = getKnobByName(knobName);
        if (!knob) {
            if (!getEffectInstance()->getIsEditable()) {
                // A placeholder has none of the original knobs by design; their values stay in the stored serialization.
                continue;
            }
            LogEntry::LogEntryColor c;
            if (getColor(&c.r, &c.g, &c.b)) {
                c.colorSet = true;
            }

            QString err = tr("Could not find a parameter named %1").arg(QString::fromUtf8(knobName.c_str()));
            appPTR->writeToErrorLog_mt_safe(QString::fromUtf8( getScriptName_mt_safe().c_str() ), QDateTime::currentDateTime(), err, false, c);
            continue;
        }
        knob->storeLinksFromSerialization(*it);
        knob->restoreExpressionsFromSerialization(*it, oldNewScriptNamesMapping);
    }

    const std::list<Serialization::GroupKnobSerialization>& userKnobs = serialization.userPages;
    for (std::list<Serialization::GroupKnobSerialization>::const_iterator it = userKnobs.begin(); it != userKnobs.end(); ++it) {
        _imp->storeKnobLinksRecursive(*it, oldNewScriptNamesMapping);
    }
}


void
Node::Implementation::restoreKnobLinksRecursive(const NodesList & allNodes,
                                                const KnobGroupPtr& group,
                                                const std::map<std::string, std::string>& oldNewScriptNamesMapping,
                                                bool throwOnFailure)
{
    const std::vector<KnobIPtr>& children = group->getChildren();

    for (std::vector<KnobIPtr>::const_iterator it = children.begin(); it != children.end(); ++it) {
        (*it)->restoreLinks(allNodes, oldNewScriptNamesMapping, throwOnFailure);
        KnobGroupPtr isGroup = std::dynamic_pointer_cast<KnobGroup>(*it);
        if (isGroup) {
            restoreKnobLinksRecursive(allNodes, isGroup, oldNewScriptNamesMapping, throwOnFailure);
        }
    }
}


void
Node::restoreKnobsLinks(const NodesList & allNodes,
                        const std::map<std::string, std::string>& oldNewScriptNamesMapping,
                        bool throwOnFailure)
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    std::vector<KnobIPtr> knobs = getKnobs();
    for (std::vector<KnobIPtr>::iterator it = knobs.begin(); it != knobs.end(); ++it) {
        (*it)->restoreLinks(allNodes, oldNewScriptNamesMapping, throwOnFailure);
        KnobGroupPtr group = std::dynamic_pointer_cast<KnobGroup>(*it);
        if (group) {
            _imp->restoreKnobLinksRecursive(allNodes, group, oldNewScriptNamesMapping, throwOnFailure);
        }
    }
}


void
Node::doDestroyNodeInternalEnd(bool autoReconnect)
{
    ///Remove the node from the project
    deactivate(NodesList(),
               true,
               autoReconnect,
               true,
               false);


    {
        NodeGuiIPtr guiPtr = _imp->guiPointer.lock();
        if (guiPtr) {
            guiPtr->destroyGui();
        }
    }

    ///If its a group, clear its nodes
    NodeGroup* isGrp = dynamic_cast<NodeGroup*>( _imp->effect.get() );
    if (isGrp) {
        isGrp->clearNodesBlocking();
    }


    ///Quit any rendering
    OutputEffectInstance* isOutput = dynamic_cast<OutputEffectInstance*>( _imp->effect.get() );
    if (isOutput) {
        isOutput->getRenderEngine()->quitEngine(true);
    }

    ///Remove all images in the cache associated to this node
    ///This will not remove from the disk cache if the project is closing
    removeAllImagesFromCache(false);

    AppInstancePtr app = getApp();
    if (app) {
        app->recheckInvalidExpressions();
    }

    ///Remove the Python node
    deleteNodeVariableToPython( getFullyQualifiedName() );

    ///Disconnect all inputs
    /*int maxInputs = getNInputs();
       for (int i = 0; i < maxInputs; ++i) {
       disconnectInput(i);
       }*/

    ///Kill the effect
    if (_imp->effect) {
        _imp->effect->clearPluginMemoryChunks();
    }
    EffectInstancePtr effect;
    {
        QMutexLocker k(&_imp->effectTeardownMutex);
        effect.swap(_imp->effect);
    }
    effect.reset();

    ///If inside the group, remove it from the group
    ///the use_count() after the call to removeNode should be 2 and should be the shared_ptr held by the caller and the
    ///thisShared ptr

    ///If not inside a gorup or inside fromDest the shared_ptr is probably invalid at this point
    NodeCollectionPtr thisGroup = getGroup();
    if ( thisGroup ) {
        thisGroup->removeNode(this);
    }

} // Node::doDestroyNodeInternalEnd


NATRON_NAMESPACE_ANONYMOUS_ENTER

class NodeDestroyNodeInternalArgs
    : public GenericWatcherCallerArgs
{
public:

    bool autoReconnect;

    NodeDestroyNodeInternalArgs()
        : GenericWatcherCallerArgs()
        , autoReconnect(false)
    {}

    virtual ~NodeDestroyNodeInternalArgs() {}
};

typedef std::shared_ptr<NodeDestroyNodeInternalArgs> NodeDestroyNodeInternalArgsPtr;

NATRON_NAMESPACE_ANONYMOUS_EXIT


void
Node::onProcessingQuitInDestroyNodeInternal(int taskID,
                                            const GenericWatcherCallerArgsPtr& args)
{
    // A blocking destroy that ran meanwhile has already waited for this watcher and released it.
    if (!_imp->renderWatcher) {
        return;
    }
    assert(taskID == (int)NodeRenderWatcher::eBlockingTaskQuitAnyProcessing);
    Q_UNUSED(taskID);
    assert(args);
    NodeDestroyNodeInternalArgs* thisArgs = dynamic_cast<NodeDestroyNodeInternalArgs*>( args.get() );
    assert(thisArgs);
    // A blocking destroy, such as its app quitting, may have finished the node while it waited.
    if (_imp->effect) {
        doDestroyNodeInternalEnd(thisArgs ? thisArgs->autoReconnect : false);
    }
    _imp->renderWatcher.reset();
}


void
Node::destroyNode(bool blockingDestroy, bool autoReconnect)
{
    if (!_imp->effect) {
        return;
    }

    {
        QMutexLocker k(&_imp->activatedMutex);
        _imp->isBeingDestroyed = true;
    }

    bool allProcessingQuit = areAllProcessingThreadsQuit();
    if (allProcessingQuit || blockingDestroy) {
        // The watcher of an earlier non-blocking destroy holds this node: let its queued quit run rather than
        // cancel it, then release it so neither keeps the other alive. Its quit only waits on this node's and its
        // children's processing, which this destroy waits on anyway. Other watchers listing this node, such as an
        // enclosing group's, are kept off the freed effect by effectTeardownMutex.
        NodePtr keepAlive;
        if (_imp->renderWatcher) {
            keepAlive = shared_from_this();
            // A node is never destroyed from its own watcher, which only quits or aborts processing.
            assert(QThread::currentThread() != _imp->renderWatcher.get());
            _imp->renderWatcher->finishPendingTasksAndStop();
            _imp->renderWatcher.reset();
        }
        if (!allProcessingQuit) {
            quitAnyProcessing_blocking(false);
        }
        doDestroyNodeInternalEnd(false);
    } else if (!_imp->renderWatcher) {
        NodeGroup* isGrp = dynamic_cast<NodeGroup*>( _imp->effect.get() );
        NodesList nodesToWatch;
        nodesToWatch.push_back( shared_from_this() );
        if (isGrp) {
            isGrp->getNodes_recursive(nodesToWatch, false);
        }
        _imp->renderWatcher = std::make_shared<NodeRenderWatcher>(nodesToWatch);
        QObject::connect( _imp->renderWatcher.get(), SIGNAL(taskFinished(int,GenericWatcherCallerArgsPtr)), this, SLOT(onProcessingQuitInDestroyNodeInternal(int,GenericWatcherCallerArgsPtr)) );
        NodeDestroyNodeInternalArgsPtr args = std::make_shared<NodeDestroyNodeInternalArgs>();
        args->autoReconnect = autoReconnect;
        _imp->renderWatcher->scheduleBlockingTask(NodeRenderWatcher::eBlockingTaskQuitAnyProcessing, args);
    }
} // Node::destroyNodeInternal

bool
Node::isSupportedComponent(int inputNb,
                           const ImageLayerDesc& comp) const
{
    QMutexLocker l(&_imp->inputsMutex);

    if (inputNb >= 0) {
        assert( inputNb < (int)_imp->inputsComponents.size() );
        std::list<ImageLayerDesc>::const_iterator found = std::find(_imp->inputsComponents[inputNb].begin(), _imp->inputsComponents[inputNb].end(), comp);

        return found != _imp->inputsComponents[inputNb].end();
    } else {
        assert(inputNb == -1);
        std::list<ImageLayerDesc>::const_iterator found = std::find(_imp->outputComponents.begin(), _imp->outputComponents.end(), comp);

        return found != _imp->outputComponents.end();
    }
}


ImageBitDepthEnum
Node::getClosestSupportedBitDepth(ImageBitDepthEnum depth)
{
    bool foundShort = false;
    bool foundByte = false;

    for (std::list<ImageBitDepthEnum>::const_iterator it = _imp->supportedDepths.begin(); it != _imp->supportedDepths.end(); ++it) {
        if (*it == depth) {
            return depth;
        } else if (*it == eImageBitDepthFloat) {
            return eImageBitDepthFloat;
        } else if (*it == eImageBitDepthShort) {
            foundShort = true;
        } else if (*it == eImageBitDepthByte) {
            foundByte = true;
        }
    }
    if (foundShort) {
        return eImageBitDepthShort;
    } else if (foundByte) {
        return eImageBitDepthByte;
    } else {
        ///The plug-in doesn't support any bitdepth, the program shouldn't even have reached here.
        assert(false);

        return eImageBitDepthNone;
    }
}

ImageBitDepthEnum
Node::getBestSupportedBitDepth() const
{
    bool foundShort = false;
    bool foundByte = false;

    for (std::list<ImageBitDepthEnum>::const_iterator it = _imp->supportedDepths.begin(); it != _imp->supportedDepths.end(); ++it) {
        switch (*it) {
        case eImageBitDepthByte:
            foundByte = true;
            break;

        case eImageBitDepthShort:
            foundShort = true;
            break;

        case eImageBitDepthHalf:
            break;

        case eImageBitDepthFloat:

            return eImageBitDepthFloat;

        case eImageBitDepthNone:
            break;
        }
    }

    if (foundShort) {
        return eImageBitDepthShort;
    } else if (foundByte) {
        return eImageBitDepthByte;
    } else {
        ///The plug-in doesn't support any bitdepth, the program shouldn't even have reached here.
        assert(false);

        return eImageBitDepthNone;
    }
}

bool
Node::isSupportedBitDepth(ImageBitDepthEnum depth) const
{
    return std::find(_imp->supportedDepths.begin(), _imp->supportedDepths.end(), depth) != _imp->supportedDepths.end();
}


NATRON_NAMESPACE_EXIT
