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

#include <QDebug>
#include <QThread>

#include <sstream>

#include "Engine/AppInstance.h"
#include "Engine/GroupOutput.h"
#include "Engine/NodeGroup.h"

#include "Serialization/NodeSerialization.h"

NATRON_NAMESPACE_ENTER

// Trim underscore followed by digits at the end of baseName (see #732).
static
void trimNumber(std::string &baseName)
{
    std::size_t found_underscore = baseName.rfind('_');
    if (found_underscore != std::string::npos && found_underscore != (baseName.size() - 1)) {
        std::size_t found_nondigit = baseName.find_last_not_of("0123456789");
        if (found_nondigit == found_underscore) {
            baseName.erase(found_underscore);
        }
    }
}

void
Node::initNodeScriptName(const Serialization::NodeSerialization* serialization, const QString& fixedName)
{
    /*
     If the serialization is not null, we are either pasting a node or loading it from a project.
     */
    NodeCollectionPtr group = getGroup();
    bool isMultiInstanceChild = false;
    if ( !_imp->multiInstanceParentName.empty() ) {
        isMultiInstanceChild = true;
    }

    if (!fixedName.isEmpty()) {

        std::string name = fixedName.toStdString();
        // If the script name is available, use it as is (else we get issue #755).
        // Else look for a similar name by adding a (different) number suffix.
        if ( group && group->checkIfNodeNameExists(name, this) ) {
            std::string baseName = name;
            trimNumber(baseName);
            int no = 1;
            do {
                name = baseName;
                if (no > 1) {
                    name += '_';
                    std::stringstream ss;
                    ss << no;
                    name += ss.str();
                }
                ++no;
            } while ( group && group->checkIfNodeNameExists(name, this) );
        }
        //This version of setScriptName will not error if the name is invalid or already taken
        setScriptName_no_error_check(name);

    } else if (serialization) {
        if (group && !group->isCacheIDAlreadyTaken(serialization->cacheID)) {
            QMutexLocker k(&_imp->nameMutex);
            _imp->cacheID = serialization->cacheID;
        }
        std::string name = serialization->scriptName;
        // If the serialized script name is available, use it as is (else we get issue #755).
        // Else look for a similar name by adding a (different) number suffix.
        if ( group && group->checkIfNodeNameExists(name, this) ) {
            std::string baseName = name;
            trimNumber(baseName);
            int no = 1;
            do {
                name = baseName;
                if (no > 1) {
                    name += '_';
                    std::stringstream ss;
                    ss << no;
                    name += ss.str();
                }
                ++no;
            } while ( group->checkIfNodeNameExists(name, this) );
        }

        //This version of setScriptName will not error if the name is invalid or already taken
        setScriptName_no_error_check(name);
        setLabel(serialization->label);

    } else {
        std::string name;
        QString pluginLabel;
        AppManager::AppTypeEnum appType = appPTR->getAppType();
        if (_imp->plugin) {
            if ( ( appType == AppManager::eAppTypeBackground) ||
                 ( appType == AppManager::eAppTypeGui) ||
                 ( appType == AppManager::eAppTypeInterpreter) ) {
                pluginLabel = _imp->plugin->getLabelWithoutSuffix();
            } else {
                pluginLabel = _imp->plugin->getPluginLabel();
            }
        }
        try {
            if (group) {
                group->initNodeName(isMultiInstanceChild ? _imp->multiInstanceParentName + '_' : pluginLabel.toStdString(), &name);
            } else {
                name = NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly( pluginLabel.toStdString() );
            }
        } catch (...) {
        }

        setNameInternal(name.c_str(), false);
    }



}


const std::string &
Node::getScriptName() const
{
    ////Only called by the main-thread
    assert( QThread::currentThread() == qApp->thread() );
    QMutexLocker l(&_imp->nameMutex);

    return _imp->scriptName;
}

std::string
Node::getScriptName_mt_safe() const
{
    QMutexLocker l(&_imp->nameMutex);

    return _imp->scriptName;
}

static void
prependGroupNameRecursive(const NodePtr& group,
                          std::string& name)
{
    name.insert(0, ".");
    name.insert( 0, group->getScriptName_mt_safe() );
    NodeCollectionPtr hasParentGroup = group->getGroup();
    NodeGroupPtr isGrp = std::dynamic_pointer_cast<NodeGroup>(hasParentGroup);
    if (isGrp) {
        prependGroupNameRecursive(isGrp->getNode(), name);
    }
}

std::string
Node::getFullyQualifiedNameInternal(const std::string& scriptName) const
{
    std::string ret = scriptName;
    NodePtr parent = getParentMultiInstance();

    if (parent) {
        prependGroupNameRecursive(parent, ret);
    } else {
        NodeCollectionPtr hasParentGroup = getGroup();
        NodeGroup* isGrp = dynamic_cast<NodeGroup*>( hasParentGroup.get() );
        if (isGrp) {
            NodePtr grpNode = isGrp->getNode();
            if (grpNode) {
                prependGroupNameRecursive(grpNode, ret);
            }
        }
    }

    return ret;
}

std::string
Node::getFullyQualifiedName() const
{
    return getFullyQualifiedNameInternal( getScriptName_mt_safe() );
}

void
Node::setLabel(const std::string& label)
{
    assert( QThread::currentThread() == qApp->thread() );

    {
        QMutexLocker k(&_imp->nameMutex);
        if (label == _imp->label) {
            return;
        }
        _imp->label = label;
    }
    NodeCollectionPtr collection = getGroup();
    if (collection) {
        collection->notifyNodeNameChanged( shared_from_this() );
    }
    Q_EMIT labelChanged( QString::fromUtf8( label.c_str() ) );
}

const std::string&
Node::getLabel() const
{
    assert( QThread::currentThread() == qApp->thread() );
    QMutexLocker k(&_imp->nameMutex);

    return _imp->label;
}

std::string
Node::getLabel_mt_safe() const
{
    QMutexLocker k(&_imp->nameMutex);

    return _imp->label;
}

void
Node::setScriptName_no_error_check(const std::string & name)
{
    setNameInternal(name, false);
}

static void
addListeningDimension(const KnobIPtr& knob,
                      int dimension,
                      KnobI::ListenerDimsMap* dependencies)
{
    if ((dimension < 0) || (dimension >= knob->getDimension())) {
        return;
    }
    KnobI::ListenerDimsMap::iterator found = dependencies->find(KnobIWPtr(knob));
    if (found == dependencies->end()) {
        found = dependencies->insert(std::make_pair(KnobIWPtr(knob), std::vector<KnobI::ListenerDim>(knob->getDimension()))).first;
    }
    found->second[dimension].isExpr = true;
    found->second[dimension].isListening = true;
}

// Fills dependencies with the expressions reading knobs of node or of the nodes inside it, and
// metadataDependencies with those reading their metadata.
static void
insertDependenciesRecursive(Node* node,
                            KnobI::ListenerDimsMap* dependencies,
                            KnobI::ListenerDimsMap* metadataDependencies)
{
    const KnobsVec & knobs = node->getKnobs();

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobI::ListenerDimsMap dimDeps;
        knobs[i]->getListeners(dimDeps);
        for (KnobI::ListenerDimsMap::iterator it = dimDeps.begin(); it != dimDeps.end(); ++it) {
            KnobI::ListenerDimsMap::iterator found = dependencies->find(it->first);
            if ( found != dependencies->end() ) {
                assert( found->second.size() == it->second.size() );
                for (std::size_t j = 0; j < found->second.size(); ++j) {
                    if (it->second[j].isExpr) {
                        found->second[j].isListening |= it->second[j].isListening;
                    }
                }
            } else {
                dependencies->insert(*it);
            }
        }

        // A render-hashed knob reading its own node's metadata is not one of the node's readers.
        for (int d = 0; d < knobs[i]->getDimension(); ++d) {
            std::list<NodePtr> sources;
            knobs[i]->getMetadataDependencies(d, &sources);
            for (std::list<NodePtr>::const_iterator it = sources.begin(); it != sources.end(); ++it) {
                if (it->get() == node) {
                    addListeningDimension(knobs[i], d, metadataDependencies);
                    break;
                }
            }
        }
    }

    std::list<std::pair<KnobIPtr, int>> metadataReaders;
    node->getMetadataReaders(&metadataReaders);
    for (std::list<std::pair<KnobIPtr, int>>::const_iterator it = metadataReaders.begin(); it != metadataReaders.end(); ++it) {
        addListeningDimension(it->first, it->second, metadataDependencies);
    }

    NodeGroup* isGroup = node->isEffectGroup();
    if (isGroup) {
        NodesList nodes = isGroup->getNodes();
        for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
            insertDependenciesRecursive(it->get(), dependencies, metadataDependencies);
        }
    }
}

void
Node::setNameInternal(const std::string& name,
                      bool throwErrors)
{
    std::string oldName = getScriptName_mt_safe();
    std::string fullOldName = getFullyQualifiedName();
    std::string newName = name;
    bool onlySpaces = true;

    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] != '_') {
            onlySpaces = false;
            break;
        }
    }
    if (onlySpaces) {
        QString err = tr("The name must at least contain a character");
        if (throwErrors) {
            throw std::runtime_error(err.toStdString());
        } else {
            LogEntry::LogEntryColor c;
            if (getColor(&c.r, &c.g, &c.b)) {
                c.colorSet = true;
            }
            appPTR->writeToErrorLog_mt_safe(QString::fromUtf8(getFullyQualifiedName().c_str()), QDateTime::currentDateTime(), err, false, c);
            std::cerr << err.toStdString() << std::endl;

            return;
        }
    }

    NodeCollectionPtr collection = getGroup();
    if (collection) {
        if (throwErrors) {
            try {
                collection->checkNodeName(this, name, false, false, &newName);
            } catch (const std::exception& e) {
                LogEntry::LogEntryColor c;
                if (getColor(&c.r, &c.g, &c.b)) {
                    c.colorSet = true;
                }
                appPTR->writeToErrorLog_mt_safe(QString::fromUtf8(getFullyQualifiedName().c_str()), QDateTime::currentDateTime(), QString::fromUtf8( e.what() ), false, c );
                std::cerr << e.what() << std::endl;

                return;
            }
        } else {
            collection->checkNodeName(this, name, false, false, &newName);
        }
    }


    if (oldName == newName) {
        return;
    }


    if ( !newName.empty() ) {
        if ( NATRON_PYTHON_NAMESPACE::isKeyword(newName) ) {
            throw std::runtime_error(newName + " is a Python keyword");
        }
        bool isAttrDefined = false;
        std::string newPotentialQualifiedName = getApp()->getAppIDString() + "." + getFullyQualifiedNameInternal(newName);
        PyObject* obj = NATRON_PYTHON_NAMESPACE::getAttrRecursive(newPotentialQualifiedName, appPTR->getMainModule(), &isAttrDefined);
        Q_UNUSED(obj);
        if (isAttrDefined) {
            std::stringstream ss;
            ss << "A Python attribute with the same name (" << newPotentialQualifiedName << ") already exists.";
            if (throwErrors) {
                throw std::runtime_error( ss.str() );
            } else {
                std::string err = ss.str();
                LogEntry::LogEntryColor c;
                if (getColor(&c.r, &c.g, &c.b)) {
                    c.colorSet = true;
                }
                appPTR->writeToErrorLog_mt_safe(QString::fromUtf8(oldName.c_str()), QDateTime::currentDateTime(), QString::fromUtf8( err.c_str() ), false, c );
                std::cerr << err << std::endl;

                return;
            }
        }
    }

    bool mustSetCacheID;
    {
        QMutexLocker l(&_imp->nameMutex);
        _imp->scriptName = newName;
        mustSetCacheID = _imp->cacheID.empty();
        ///Set the label at the same time if the label is empty
        if ( _imp->label.empty() ) {
            _imp->label = newName;
        }
    }
    std::string fullySpecifiedName = getFullyQualifiedName();

    if (mustSetCacheID) {
        std::string baseName = fullySpecifiedName;
        std::string cacheID = fullySpecifiedName;
        int i = 1;
        while ( getGroup() && getGroup()->isCacheIDAlreadyTaken(cacheID) ) {
            std::stringstream ss;
            ss << baseName;
            ss << i;
            cacheID = ss.str();
            ++i;
        }
        QMutexLocker l(&_imp->nameMutex);
        _imp->cacheID = cacheID;
    }

    if (collection) {
        if ( !oldName.empty() ) {
            if (fullOldName != fullySpecifiedName) {
                try {
                    setNodeVariableToPython(fullOldName, fullySpecifiedName);
                } catch (const std::exception& e) {
                    qDebug() << e.what();
                }
            }
        } else { //if (!oldName.empty()) {
            declareNodeVariableToPython(fullySpecifiedName);
        }

        if (_imp->nodeCreated) {
            ///For all knobs that have listeners, change in the expressions of listeners this knob script-name
            KnobI::ListenerDimsMap dependencies;
            KnobI::ListenerDimsMap metadataDependencies;
            insertDependenciesRecursive(this, &dependencies, &metadataDependencies);
            // A metadata read's keys and other strings may hold the name as text that must stay, so
            // an expression reading metadata only has the name replaced where it names a node:
            // which covers its knob reads, made through the same identifiers and getNode() paths.
            for (KnobI::ListenerDimsMap::iterator it = metadataDependencies.begin(); it != metadataDependencies.end(); ++it) {
                KnobIPtr reader = it->first.lock();
                if (!reader) {
                    continue;
                }
                for (std::size_t d = 0; d < it->second.size(); ++d) {
                    if (it->second[d].isListening) {
                        reader->replaceNodeNameInExpressionCode((int)d, oldName, newName);
                    }
                }
            }
            for (KnobI::ListenerDimsMap::iterator it = dependencies.begin(); it != dependencies.end(); ++it) {
                KnobIPtr listener = it->first.lock();
                if (!listener) {
                    continue;
                }
                KnobI::ListenerDimsMap::const_iterator readsMetadata = metadataDependencies.find(it->first);
                for (std::size_t d = 0; d < it->second.size(); ++d) {
                    if (!it->second[d].isListening || !it->second[d].isExpr) {
                        continue;
                    }
                    if ((readsMetadata != metadataDependencies.end()) && (d < readsMetadata->second.size()) && readsMetadata->second[d].isListening) {
                        continue;
                    }
                    listener->replaceNodeNameInExpression(d, oldName, newName);
                }
            }
        }
    }

    // A rename, not the node's first name: the effect may keep state derived from its name.
    if (!oldName.empty() && _imp->effect) {
        try {
            _imp->effect->onScriptNameChanged(fullySpecifiedName);
        } catch (const std::exception& e) {
            qDebug() << e.what();
        }
    }

    QString qnewName = QString::fromUtf8( newName.c_str() );
    Q_EMIT scriptNameChanged(qnewName);
    Q_EMIT labelChanged(qnewName);
} // Node::setNameInternal

void
Node::setScriptName(const std::string& name)
{
    std::string newName;

    if ( getGroup() ) {
        getGroup()->checkNodeName(this, name, false, true, &newName);
    } else {
        newName = name;
    }
    if ( NATRON_PYTHON_NAMESPACE::isKeyword(newName) ) {
        throw std::runtime_error(newName + " is a Python keyword");
    }

    //We do not allow setting the script-name of output nodes because we rely on it with NatronRenderer
    if ( dynamic_cast<GroupOutput*>( _imp->effect.get() ) ) {
        throw std::runtime_error( tr("Changing the script-name of an Output node is not a valid operation.").toStdString() );

        return;
    }


    setNameInternal(newName, true);
}

NATRON_NAMESPACE_EXIT
