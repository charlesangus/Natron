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

#include "PyNode.h"

#include <cassert>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Hash64.h"
#include "Engine/Knob.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Metadata/EffectMetadata.h"
#include "Engine/Project.h"
#include "Engine/PyParameter.h" // checkHolderIsEditable
#include "Engine/PyRoto.h"
#include "Engine/PyTracker.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER
NATRON_PYTHON_NAMESPACE_ENTER

ImageLayer::ImageLayer(const QString& layerName,
                       const QString& componentsPrettyName,
                       const QStringList& componentsName)
    : _layerName(layerName)
    , _componentsPrettyName(componentsPrettyName)
    , _componentsName(componentsName)
    , _comps()
{
    std::vector<std::string> channels( componentsName.size() );
    int i = 0;

    for (QStringList::const_iterator it = componentsName.begin(); it != componentsName.end(); ++it, ++i) {
        channels[i] = it->toStdString();
    }
    _comps.reset(new ImageLayerDesc(layerName.toStdString(), layerName.toStdString(), componentsPrettyName.toStdString(), channels));
}

ImageLayer::ImageLayer(const ImageLayerDesc& comps)
    : _layerName(QString::fromUtf8(comps.getLayerLabel().c_str()))
    , _componentsPrettyName(QString::fromUtf8(comps.getChannelsLabel().c_str()))
{
    const std::vector<std::string>& channels = comps.getChannels();

    for (std::size_t i = 0; i < channels.size(); ++i) {
        _componentsName.push_back( QString::fromUtf8( channels[i].c_str() ) );
    }
    _comps.reset(new ImageLayerDesc(comps));
}

int
ImageLayer::getHash(const ImageLayer& layer)
{
    Hash64 h;

    Hash64_appendQString( &h, QString::fromUtf8( layer._comps->getChannelsLabel().c_str() ) );
    const std::vector<std::string>& comps = layer._comps->getChannels();
    for (std::size_t i = 0; i < comps.size(); ++i) {
        Hash64_appendQString( &h, QString::fromUtf8( comps[i].c_str() ) );
    }

    return (int)h.value();
}

bool
ImageLayer::isColorLayer() const
{
    return _comps->isColorLayer();
}

int
ImageLayer::getNumComponents() const
{
    return _comps->getNumComponents();
}

const QString&
ImageLayer::getLayerName() const
{
    return _layerName;
}

const QStringList&
ImageLayer::getComponentsNames() const
{
    return _componentsName;
}

const QString&
ImageLayer::getComponentsPrettyName() const
{
    return _componentsPrettyName;
}

bool
ImageLayer::operator==(const ImageLayer& other) const
{
    return _comps == other._comps;
}

bool
ImageLayer::operator<(const ImageLayer& other) const
{
    return _comps < other._comps;
}

/*
 * These are default presets image components
 */
ImageLayer
ImageLayer::getNoneComponents()
{
    return ImageLayer(ImageLayerDesc::getNoneComponents());
}

ImageLayer
ImageLayer::getRGBAComponents()
{
    return ImageLayer(ImageLayerDesc::getColorView(kNatronColorViewRGBA));
}

ImageLayer
ImageLayer::getRGBComponents()
{
    return ImageLayer(ImageLayerDesc::getColorView(kNatronColorViewRGB));
}

ImageLayer
ImageLayer::getAlphaComponents()
{
    return ImageLayer(ImageLayerDesc::getColorView(kNatronColorViewAlpha));
}

ImageLayer
ImageLayer::getBackwardMotionComponents()
{
    return ImageLayer(ImageLayerDesc::getBackwardMotionComponents());
}

ImageLayer
ImageLayer::getForwardMotionComponents()
{
    return ImageLayer(ImageLayerDesc::getForwardMotionComponents());
}

ImageLayer
ImageLayer::getDisparityLeftComponents()
{
    return ImageLayer(ImageLayerDesc::getDisparityLeftComponents());
}

ImageLayer
ImageLayer::getDisparityRightComponents()
{
    return ImageLayer(ImageLayerDesc::getDisparityRightComponents());
}

UserParamHolder::UserParamHolder()
    : _holder(0)
{
}

UserParamHolder::UserParamHolder(KnobHolder* holder)
    : _holder(holder)
{
}

void
UserParamHolder::setHolder(KnobHolder* holder)
{
    assert(!_holder);
    _holder = holder;
}

bool
UserParamHolder::checkIsEditable() const
{
    return checkHolderIsEditable(_holder);
}

Effect::Effect(const NodePtr& node)
    : Group()
    , UserParamHolder(node ? node->getEffectInstance().get() : 0)
    , _node(node)
{
    if (node) {
        NodeGroupPtr grp;
        if ( node->getEffectInstance() ) {
            grp = std::dynamic_pointer_cast<NodeGroup>( node->getEffectInstance()->shared_from_this() );
            init( std::dynamic_pointer_cast<NodeCollection>(grp) );
        }
    }
}

Effect::~Effect()
{
}

NodePtr
Effect::getInternalNode() const
{
    return _node.lock();
}

bool
Effect::isReaderNode()
{
    NodePtr n = getInternalNode();

    if (!n) {
        return false;
    }

    return n->getEffectInstance()->isReader();
}

bool
Effect::isWriterNode()
{
    NodePtr n = getInternalNode();

    if (!n) {
        return false;
    }

    return n->getEffectInstance()->isWriter();
}

bool
Effect::isOutputNode()
{
    NodePtr n = getInternalNode();

    if (!n) {
        return false;
    }

    return n->getEffectInstance()->isOutput();
}

void
Effect::destroy(bool autoReconnect)
{
    destroyNodeFromPython(getInternalNode(), autoReconnect);
}

int
Effect::getMaxInputCount() const
{
    return getInternalNode()->getNInputs();
}

bool
Effect::canConnectInput(int inputNumber,
                        const Effect* node) const
{
    if (!node) {
        return false;
    }

    if ( !node->getInternalNode() ) {
        return false;
    }
    Node::CanConnectInputReturnValue ret = getInternalNode()->canConnectInput(node->getInternalNode(), inputNumber);

    return ret == Node::eCanConnectInput_ok ||
           ret == Node::eCanConnectInput_differentFPS ||
           ret == Node::eCanConnectInput_differentPars;
}

bool
Effect::connectInput(int inputNumber,
                     const Effect* input)
{
    if ( canConnectInput(inputNumber, input) ) {
        PythonInputUndoGuard undo(getInternalNode(), inputNumber);

        return getInternalNode()->connectInput(input->getInternalNode(), inputNumber);
    } else {
        return false;
    }
}

void
Effect::disconnectInput(int inputNumber)
{
    PythonInputUndoGuard undo(getInternalNode(), inputNumber);

    getInternalNode()->disconnectInput(inputNumber);
}

Effect*
Effect::getInput(int inputNumber) const
{
    NodePtr node = getInternalNode()->getRealInput(inputNumber);

    if (node) {
        return new Effect(node);
    }

    return NULL;
}

Effect*
Effect::getInput(const QString& inputLabel) const
{
    NodePtr node = getInternalNode();
    if (!node) {
        return 0;
    }
    int maxInputs = node->getNInputs();
    for (int i = 0; i < maxInputs; ++i) {
        if (QString::fromUtf8(node->getInputLabel(i).c_str()) == inputLabel) {
            NodePtr ret = node->getRealInput(i);
            if (!ret) {
                return 0;
            }

            return new Effect(ret);
        }
    }
    return 0;
}

QString
Effect::getScriptName() const
{
    return QString::fromUtf8( getInternalNode()->getScriptName_mt_safe().c_str() );
}

bool
Effect::setScriptName(const QString& scriptName)
{
    PythonNodeNameUndoGuard undo(getInternalNode());

    try {
        getInternalNode()->setScriptName( scriptName.toStdString() );
    } catch (...) {
        return false;
    }

    return true;
}

QString
Effect::getLabel() const
{
    return QString::fromUtf8( getInternalNode()->getLabel_mt_safe().c_str() );
}

void
Effect::setLabel(const QString& name)
{
    PythonNodeNameUndoGuard undo(getInternalNode());

    return getInternalNode()->setLabel( name.toStdString() );
}

QString
Effect::getInputLabel(int inputNumber)
{
    try {
        return QString::fromUtf8( getInternalNode()->getInputLabel(inputNumber).c_str() );
    } catch (const std::exception& e) {
        getInternalNode()->getApp()->appendToScriptEditor( e.what() );
    }

    return QString();
}

QString
Effect::getPluginID() const
{
    return QString::fromUtf8( getInternalNode()->getPluginID().c_str() );
}

unsigned long long
Effect::getContentHash() const
{
    NodePtr node = getInternalNode();

    return node ? node->getContentHash() : 0;
}

Param*
Effect::createParamWrapperForKnob(const KnobIPtr& knob)
{
    int dims = knob->getDimension();
    KnobIntPtr isInt = std::dynamic_pointer_cast<KnobInt>(knob);
    KnobDoublePtr isDouble = std::dynamic_pointer_cast<KnobDouble>(knob);
    KnobBoolPtr isBool = std::dynamic_pointer_cast<KnobBool>(knob);
    KnobChoicePtr isChoice = std::dynamic_pointer_cast<KnobChoice>(knob);
    KnobColorPtr isColor = std::dynamic_pointer_cast<KnobColor>(knob);
    KnobStringPtr isString = std::dynamic_pointer_cast<KnobString>(knob);
    KnobFilePtr isFile = std::dynamic_pointer_cast<KnobFile>(knob);
    KnobOutputFilePtr isOutputFile = std::dynamic_pointer_cast<KnobOutputFile>(knob);
    KnobPathPtr isPath = std::dynamic_pointer_cast<KnobPath>(knob);
    KnobChannelSetPtr isChannelSet = std::dynamic_pointer_cast<KnobChannelSet>(knob);
    KnobLayerSelectPtr isLayerSelect = std::dynamic_pointer_cast<KnobLayerSelect>(knob);
    KnobChannelSelectPtr isChannelSelect = std::dynamic_pointer_cast<KnobChannelSelect>(knob);
    KnobShuffleMapPtr isShuffleMap = std::dynamic_pointer_cast<KnobShuffleMap>(knob);
    KnobButtonPtr isButton = std::dynamic_pointer_cast<KnobButton>(knob);
    KnobGroupPtr isGroup = std::dynamic_pointer_cast<KnobGroup>(knob);
    KnobPagePtr isPage = std::dynamic_pointer_cast<KnobPage>(knob);
    KnobParametricPtr isParametric = std::dynamic_pointer_cast<KnobParametric>(knob);
    KnobSeparatorPtr isSep = std::dynamic_pointer_cast<KnobSeparator>(knob);

    if (isInt) {
        switch (dims) {
        case 1:

            return new IntParam(isInt);
        case 2:

            return new Int2DParam(isInt);
        case 3:

            return new Int3DParam(isInt);
        default:
            break;
        }
    } else if (isDouble) {
        switch (dims) {
        case 1:

            return new DoubleParam(isDouble);
        case 2:

            return new Double2DParam(isDouble);
        case 3:

            return new Double3DParam(isDouble);
        default:
            break;
        }
    } else if (isBool) {
        return new BooleanParam(isBool);
    } else if (isChoice) {
        return new ChoiceParam(isChoice);
    } else if (isColor) {
        return new ColorParam(isColor);
    } else if (isString) {
        return new StringParam(isString);
    } else if (isFile) {
        return new FileParam(isFile);
    } else if (isOutputFile) {
        return new OutputFileParam(isOutputFile);
    } else if (isPath) {
        return new PathParam(isPath);
    } else if (isChannelSet) {
        return new ChannelSetParam(isChannelSet);
    } else if (isLayerSelect) {
        return new LayerSelectParam(isLayerSelect);
    } else if (isChannelSelect) {
        return new ChannelSelectParam(isChannelSelect);
    } else if (isShuffleMap) {
        return new ShuffleMapParam(isShuffleMap);
    } else if (isGroup) {
        return new GroupParam(isGroup);
    } else if (isPage) {
        return new PageParam(isPage);
    } else if (isParametric) {
        return new ParametricParam(isParametric);
    } else if (isButton) {
        return new ButtonParam(isButton);
    } else if (isSep) {
        return new SeparatorParam(isSep);
    }

    return NULL;
} // Effect::createParamWrapperForKnob

std::list<Param*>
Effect::getParams() const
{
    std::list<Param*> ret;
    const KnobsVec& knobs = getInternalNode()->getKnobs();

    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        Param* p = createParamWrapperForKnob(*it);
        if (p) {
            ret.push_back(p);
        }
    }

    return ret;
}

Param*
Effect::getParam(const QString& name) const
{
    NodePtr node = getInternalNode();

    QString fallbackSearchName;
    if (node->getApp()->isCreatingPythonGroup()) {
        // Before Natron 2.2.3, all dynamic choice parameters for multi-layer had a string parameter.
        // The string parameter had the same name as the choice parameter plus "Choice" appended.
        // If we found such a parameter, retrieve the string from it.
        QString str = QString::fromUtf8("Choice");
        if (name.endsWith(str)) {
            fallbackSearchName = name.mid(0, name.size() - str.size());
        }
    }
    KnobIPtr knob = node->getKnobByName( name.toStdString() );

    if (knob) {
        return createParamWrapperForKnob(knob);
    } else {
        if (!fallbackSearchName.isEmpty()) {
            return getParam(fallbackSearchName);
        }
        return NULL;
    }
}

int
Effect::getCurrentTime() const
{
    return getInternalNode()->getEffectInstance()->getCurrentTime();
}

void
Effect::setPosition(double x,
                    double y)
{
    PythonNodeLayoutUndoGuard undo(getInternalNode());

    getInternalNode()->setPosition(x, y);
}

void
Effect::getPosition(double* x,
                    double* y) const
{
    getInternalNode()->getPosition(x, y);
}

void
Effect::setSize(double w,
                double h)
{
    PythonNodeLayoutUndoGuard undo(getInternalNode());

    getInternalNode()->setSize(w, h);
}

void
Effect::getSize(double* w,
                double* h) const
{
    getInternalNode()->getSize(w, h);
}

void
Effect::getColor(double* r,
                 double *g,
                 double* b) const
{
    bool hasColor = getInternalNode()->getColor(r, g, b);
    Q_UNUSED(hasColor);
}

void
Effect::setColor(double r,
                 double g,
                 double b)
{
    PythonNodeLayoutUndoGuard undo(getInternalNode());

    getInternalNode()->setColor(r, g, b);
}

void
Effect::resetColor()
{
    PythonNodeLayoutUndoGuard undo(getInternalNode());

    getInternalNode()->resetColor();
}

bool
Effect::isNodeSelected() const
{
    return getInternalNode()->isUserSelected();
}

QString
Effect::getPersistentMessage() const
{
    QString message;
    int type = 0;
    getInternalNode()->getPersistentMessage(&message, &type, false);

    return message;
}

void
Effect::beginChanges()
{
    getInternalNode()->getEffectInstance()->beginChanges();
    getInternalNode()->beginInputEdition();
}

void
Effect::endChanges()
{
    getInternalNode()->getEffectInstance()->endChanges();
    getInternalNode()->endInputEdition(true);
}

IntParam*
UserParamHolder::createIntParam(const QString& name,
                                const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobIntPtr knob = _holder->createIntKnob(name.toStdString(), label.toStdString(), 1);

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new IntParam(knob);
    } else {
        return 0;
    }
}

Int2DParam*
UserParamHolder::createInt2DParam(const QString& name,
                                  const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    KnobIntPtr knob = _holder->createIntKnob(name.toStdString(), label.toStdString(), 2);

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new Int2DParam(knob);
    } else {
        return 0;
    }
}

Int3DParam*
UserParamHolder::createInt3DParam(const QString& name,
                                  const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    KnobIntPtr knob = _holder->createIntKnob(name.toStdString(), label.toStdString(), 3);

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new Int3DParam(knob);
    } else {
        return 0;
    }
}

DoubleParam*
UserParamHolder::createDoubleParam(const QString& name,
                                   const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobDoublePtr knob = _holder->createDoubleKnob(name.toStdString(), label.toStdString(), 1);

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new DoubleParam(knob);
    } else {
        return 0;
    }
}

Double2DParam*
UserParamHolder::createDouble2DParam(const QString& name,
                                     const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    KnobDoublePtr knob = _holder->createDoubleKnob(name.toStdString(), label.toStdString(), 2);

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new Double2DParam(knob);
    } else {
        return 0;
    }
}

Double3DParam*
UserParamHolder::createDouble3DParam(const QString& name,
                                     const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    KnobDoublePtr knob = _holder->createDoubleKnob(name.toStdString(), label.toStdString(), 3);

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new Double3DParam(knob);
    } else {
        return 0;
    }
}

BooleanParam*
UserParamHolder::createBooleanParam(const QString& name,
                                    const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobBoolPtr knob = _holder->createBoolKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new BooleanParam(knob);
    } else {
        return 0;
    }
}

ChoiceParam*
UserParamHolder::createChoiceParam(const QString& name,
                                   const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobChoicePtr knob = _holder->createChoiceKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new ChoiceParam(knob);
    } else {
        return 0;
    }
}

ColorParam*
UserParamHolder::createColorParam(const QString& name,
                                  const QString& label,
                                  bool useAlpha)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobColorPtr knob = _holder->createColorKnob(name.toStdString(), label.toStdString(), useAlpha ? 4 : 3);

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new ColorParam(knob);
    } else {
        return 0;
    }
}

StringParam*
UserParamHolder::createStringParam(const QString& name,
                                   const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobStringPtr knob = _holder->createStringKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new StringParam(knob);
    } else {
        return 0;
    }
}

FileParam*
UserParamHolder::createFileParam(const QString& name,
                                 const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobFilePtr knob = _holder->createFileKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new FileParam(knob);
    } else {
        return 0;
    }
}

OutputFileParam*
UserParamHolder::createOutputFileParam(const QString& name,
                                       const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobOutputFilePtr knob = _holder->createOuptutFileKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new OutputFileParam(knob);
    } else {
        return 0;
    }
}

PathParam*
UserParamHolder::createPathParam(const QString& name,
                                 const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobPathPtr knob = _holder->createPathKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new PathParam(knob);
    } else {
        return 0;
    }
}

ChannelSetParam*
UserParamHolder::createChannelSetParam(const QString& name,
                                       const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobChannelSetPtr knob = _holder->createChannelSetKnob(name.toStdString(), label.toStdString());

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new ChannelSetParam(knob);
    } else {
        return 0;
    }
}

LayerSelectParam*
UserParamHolder::createLayerSelectParam(const QString& name,
                                        const QString& label,
                                        bool withChannelButtons)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobLayerSelectPtr knob = _holder->createLayerSelectKnob(name.toStdString(), label.toStdString(), withChannelButtons);

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new LayerSelectParam(knob);
    } else {
        return 0;
    }
}

ChannelSelectParam*
UserParamHolder::createChannelSelectParam(const QString& name,
                                          const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobChannelSelectPtr knob = _holder->createChannelSelectKnob(name.toStdString(), label.toStdString());

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new ChannelSelectParam(knob);
    } else {
        return 0;
    }
}

ButtonParam*
UserParamHolder::createButtonParam(const QString& name,
                                   const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobButtonPtr knob = _holder->createButtonKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new ButtonParam(knob);
    } else {
        return 0;
    }
}

SeparatorParam*
UserParamHolder::createSeparatorParam(const QString& name,
                                      const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobSeparatorPtr knob = _holder->createSeparatorKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new SeparatorParam(knob);
    } else {
        return 0;
    }
}

GroupParam*
UserParamHolder::createGroupParam(const QString& name,
                                  const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobGroupPtr knob = _holder->createGroupKnob( name.toStdString(), label.toStdString() );

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new GroupParam(knob);
    } else {
        return 0;
    }
}

PageParam*
UserParamHolder::createPageParam(const QString& name,
                                 const QString& label)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    if (!_holder) {
        assert(false);

        return 0;
    }
    KnobPagePtr knob = _holder->createPageKnob( name.toStdString(), label.toStdString() );
    if (knob) {
        return new PageParam(knob);
    } else {
        return 0;
    }
}

ParametricParam*
UserParamHolder::createParametricParam(const QString& name,
                                       const QString& label,
                                       int nbCurves)
{
    if (!checkHolderIsEditable(_holder)) {
        return 0;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    KnobParametricPtr knob = _holder->createParametricKnob(name.toStdString(), label.toStdString(), nbCurves);

    if (knob) {
        KnobPagePtr userPage = _holder->getOrCreateUserPageKnob();
        if (userPage) {
            userPage->addKnob(knob);
        }

        return new ParametricParam(knob);
    } else {
        return 0;
    }
}

bool
UserParamHolder::removeParam(Param* param)
{
    if (!checkHolderIsEditable(_holder)) {
        return false;
    }
    PythonUserKnobsUndoGuard undo(_holder);
    if (!param) {
        return false;
    }
    if ( !param->getInternalKnob() ) {
        return false;
    }
    if ( !param->getInternalKnob()->isUserKnob() ) {
        return false;
    }

    _holder->deleteKnob(param->getInternalKnob().get(), true);

    return true;
}

PageParam*
Effect::getUserPageParam() const
{
    if (!checkHolderIsEditable(getInternalNode()->getEffectInstance().get())) {
        return 0;
    }
    KnobPagePtr page = getInternalNode()->getEffectInstance()->getOrCreateUserPageKnob();

    assert(page);

    return new PageParam(page);
}

void
UserParamHolder::refreshUserParamsGUI()
{
    if (!checkHolderIsEditable(_holder)) {
        return;
    }
    _holder->recreateUserKnobs(false);
}


Roto*
Effect::getRotoContext() const
{
    RotoContextPtr roto = getInternalNode()->getRotoContext();

    if (roto) {
        return new Roto(roto);
    }

    return 0;
}

Tracker*
Effect::getTrackerContext() const
{
    TrackerContextPtr t = getInternalNode()->getTrackerContext();

    if (t) {
        return new Tracker(t);
    }

    return 0;
}

RectD
Effect::getRegionOfDefinition(double time,
                              int view) const
{
    RectD rod;

    if ( !getInternalNode() || !getInternalNode()->getEffectInstance() ) {
        return rod;
    }
    U64 hash = getInternalNode()->getHashValue();
    bool isProject;
    StatusEnum stat = getInternalNode()->getEffectInstance()->getRegionOfDefinition_public(hash, time, RenderScale::identity, ViewIdx(view), &rod, &isProject);
    if (stat != eStatusOK) {
        return RectD();
    }

    return rod;
}

void
Effect::setSubGraphEditable(bool editable)
{
    if ( !getInternalNode() ) {
        return;
    }
    NodeGroup* isGroup = getInternalNode()->isEffectGroup();
    if (isGroup) {
        isGroup->setSubGraphEditable(editable);
    }
}

std::list<ImageLayer>
Effect::getAvailableLayers(int inputNb) const
{
    std::list<ImageLayer> ret;

    if ( !getInternalNode() ) {
        return ret;
    }
    double time(getInternalNode()->getApp()->getTimeLine()->currentFrame());

    std::list<ImageLayerDesc> availComps;
    getInternalNode()->getEffectInstance()->getAvailableLayers(time, ViewIdx(0), inputNb, &availComps);
    ImageLayerDesc::expandColorViews(&availComps);
    for (std::list<ImageLayerDesc>::iterator it = availComps.begin(); it != availComps.end(); ++it) {
        ret.push_back(ImageLayer(*it));
    }

    return ret;
}

RectI
Effect::getOutputFormat() const
{
    NodePtr node = getInternalNode();

    if (!node) {
        return RectI();
    }

    return node->getEffectInstance()->getOutputFormat();
}

double
Effect::getFrameRate() const
{
    NodePtr node = getInternalNode();

    if (!node) {
        return 24.;
    }

    return node->getEffectInstance()->getFrameRate();
}

namespace {
PyObject*
metadataValueToPython(const ImageMetadata::Value& value)
{
    return std::visit([](const auto& v) -> PyObject* {
        using T = std::decay_t<decltype(v)>;

        if constexpr (std::is_same_v<T, int>) {
            return PyLong_FromLong(v);
        } else if constexpr (std::is_same_v<T, double>) {
            return PyFloat_FromDouble(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return PyUnicode_FromStringAndSize(v.data(), (Py_ssize_t)v.size());
        } else {
            PyObject* list = PyList_New((Py_ssize_t)v.size());

            if (!list) {
                return NULL;
            }
            Py_ssize_t i = 0;
            for (const auto& element : v) {
                PyObject* item = std::is_same_v<T, std::vector<int>> ? PyLong_FromLong((long)element) : PyFloat_FromDouble((double)element);

                if (!item) {
                    Py_DECREF(list);

                    return NULL;
                }
                PyList_SetItem(list, i++, item);
            }

            return list;
        }
    },
                      value);
}

ImageMetadata
metadataOf(const NodePtr& node,
           double frame,
           int view)
{
    if (!node) {
        return ImageMetadata();
    }

    return getEffectOutputMetadata(node->getEffectInstance(), frame, ViewIdx(view));
}

// An expression evaluated at another frame than the timeline's (getValueAtTime(), a render) must
// read the metadata of that frame, as frame does.
double
defaultMetadataFrame(const Effect& effect)
{
    double time;

    return KnobHelper::getExpressionEvaluationTime(&time) ? time : effect.getCurrentTime();
}
} // namespace

PyObject*
Effect::metadata(const QString& key) const
{
    return metadata(key, defaultMetadataFrame(*this));
}

PyObject*
Effect::metadata(const QString& key,
                 double frame,
                 int view) const
{
    const ImageMetadata metadata = metadataOf(getInternalNode(), frame, view);
    const ImageMetadata::Value* value = metadata.find(key.toStdString());

    if (!value) {
        Py_RETURN_NONE;
    }

    return metadataValueToPython(*value);
}

QStringList
Effect::metadataKeys() const
{
    return metadataKeys(defaultMetadataFrame(*this));
}

QStringList
Effect::metadataKeys(double frame,
                     int view) const
{
    const ImageMetadata metadata = metadataOf(getInternalNode(), frame, view);
    QStringList keys;

    for (ImageMetadata::const_iterator it = metadata.begin(); it != metadata.end(); ++it) {
        keys.push_back(QString::fromStdString(it->first));
    }

    return keys;
}

void
Effect::addMetadataAsDependencyOf(int fromExprDimension,
                                  Param* param)
{
    NodePtr node = getInternalNode();
    KnobIPtr expressionKnob = param ? param->getInternalKnob() : KnobIPtr();

    if (!node || !expressionKnob) {
        return;
    }
    expressionKnob->addMetadataDependency(fromExprDimension, node);
}

double
Effect::getPixelAspectRatio() const
{
    NodePtr node = getInternalNode();

    if (!node) {
        return 1.;
    }

    return node->getEffectInstance()->getAspectRatio(-1);
}

ImageBitDepthEnum
Effect::getBitDepth() const
{
    NodePtr node = getInternalNode();

    if (!node) {
        return eImageBitDepthFloat;
    }

    return node->getEffectInstance()->getBitDepth(-1);
}

void
Effect::setPagesOrder(const QStringList& pages)
{
    if (!getInternalNode() || !checkHolderIsEditable(getInternalNode()->getEffectInstance().get())) {
        return;
    }
    PythonUserKnobsUndoGuard undo(getInternalNode()->getEffectInstance().get());

    std::list<std::string> order;
    for (QStringList::const_iterator it = pages.begin(); it != pages.end(); ++it) {
        order.push_back( it->toStdString() );
    }
    getInternalNode()->setPagesOrder(order);
}

NATRON_PYTHON_NAMESPACE_EXIT
NATRON_NAMESPACE_EXIT
