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

#include "KnobUndoCommand.h"

#include <QDebug>
#include <algorithm> // find
#include <sstream> // stringstream
#include <stdexcept>

#include "Serialization/KnobSerialization.h"

#include "Engine/AppInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLinkGroup.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

#include "Gui/GuiApplicationManager.h"


NATRON_NAMESPACE_ENTER

/**
 * @brief Adds to nodes the nodes of the knobs sharing a link group with knob, linked or not.
 **/
static void
getLinkGroupNodes(const KnobIPtr& knob,
                  NodesList* nodes)
{
    for (int i = 0; i < knob->getDimension(); ++i) {
        KnobLinkGroupPtr group = knob->getLinkGroup(i);
        if (!group) {
            continue;
        }
        std::vector<KnobLinkGroup::Member> members = group->getMembers();
        for (std::vector<KnobLinkGroup::Member>::const_iterator it = members.begin(); it != members.end(); ++it) {
            KnobIPtr member = it->knob.lock();
            EffectInstance* effect = member ? dynamic_cast<EffectInstance*>(member->getHolder()) : 0;
            NodePtr node = effect ? effect->getNode() : NodePtr();
            if (node && (std::find(nodes->begin(), nodes->end(), node) == nodes->end())) {
                nodes->push_back(node);
            }
        }
    }
}

/**
 * @brief Knob link changes do not emit Node::linksChanged(): emits it on the knob's node, the nodes
 * it now shares a group with, and alsoNotify, so that their link lines are redrawn.
 **/
static void
emitKnobLinksChanged(const KnobIPtr& knob,
                     const NodesList& alsoNotify)
{
    NodesList nodes = alsoNotify;

    getLinkGroupNodes(knob, &nodes);
    EffectInstance* effect = dynamic_cast<EffectInstance*>(knob->getHolder());
    NodePtr node = effect ? effect->getNode() : NodePtr();
    if (node) {
        node->emitLinksChanged(nodes);
    } else {
        for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
            (*it)->emitLinksChanged(NodesList());
        }
    }
}

void
getKnobLinkPartners(const KnobIPtr& knob,
                    int dimension,
                    std::vector<KnobLinkPartner>* partners)
{
    const int first = (dimension == -1) ? 0 : dimension;
    const int last = (dimension == -1) ? knob->getDimension() : dimension + 1;

    for (int i = first; i < last; ++i) {
        if (!knob->isLinked(i) || knob->isLinkPermanent(i)) {
            continue;
        }
        KnobLinkGroupPtr group = knob->getLinkGroup(i);
        if (!group) {
            continue;
        }
        std::vector<KnobLinkGroup::Member> members = group->getMembers();
        const KnobLinkGroup::Member* chosen = 0;
        KnobIPtr chosenKnob;
        for (std::vector<KnobLinkGroup::Member>::const_iterator it = members.begin(); it != members.end(); ++it) {
            KnobIPtr partner = it->knob.lock();
            if (!partner || !it->linked || ((partner == knob) && (it->dimension == i))) {
                continue;
            }
            const bool otherHolder = (partner->getHolder() != knob->getHolder());
            if (!chosen || otherHolder) {
                chosen = &*it;
                chosenKnob = partner;
            }
            if (otherHolder) {
                break;
            }
        }
        if (!chosen) {
            continue;
        }
        KnobLinkPartner link;
        link.knob = UndoKnobRef(knob);
        link.dimension = i;
        link.nodeLink = group->isMemberNodeLink(knob.get(), i);
        link.partner = UndoKnobRef(chosenKnob);
        link.partnerDimension = chosen->dimension;
        link.partnerNodeLink = chosen->nodeLink;
        partners->push_back(link);
    }
}

void
restoreKnobLinkPartners(const std::vector<KnobLinkPartner>& partners)
{
    for (std::vector<KnobLinkPartner>::const_iterator it = partners.begin(); it != partners.end(); ++it) {
        KnobIPtr knob = it->knob.lock();
        KnobIPtr partner = it->partner.lock();
        if (!knob || !partner || !knob->linkTo(it->dimension, partner, it->partnerDimension)) {
            continue;
        }
        KnobLinkGroupPtr group = knob->getLinkGroup(it->dimension);
        if (group) {
            group->setMemberNodeLink(knob.get(), it->dimension, it->nodeLink);
            group->setMemberNodeLink(partner.get(), it->partnerDimension, it->partnerNodeLink);
        }
    }
}

struct PasteUndoCommandPrivate {
    UndoKnobRef knob;
    KnobClipBoardType type;
    int fromDimension;
    int targetDimension;
    // A copy of the target knob as it was before the paste, which undo copies back from.
    KnobIPtr originalKnob;
    KnobIPtr fromKnob;
    // The knob a link paste links to, which must still exist when the link is redone.
    UndoKnobRef linkSource;
    // The dimensions of the target a link paste links, found before the paste.
    std::vector<int> linkDimensions;
    // The dimensions of the target that were linked before a link paste, and the members undo links them back to.
    std::vector<KnobLinkPartner> previousLinks;
    // The nodes the target shared a link group with before the paste, whose link lines a paste changes.
    NodesList previousLinkNodes;

    PasteUndoCommandPrivate()
        : knob()
        , type(eKnobClipBoardTypeCopyLink)
        , fromDimension(-1)
        , targetDimension(-1)
        , originalKnob()
        , linkDimensions()
        , previousLinks()
        , previousLinkNodes()
    {
    }
};

PasteUndoCommand::PasteUndoCommand(const KnobGuiPtr& knob,
                                   KnobClipBoardType type,
                                   int fromDimension,
                                   int targetDimension,
                                   const KnobIPtr& fromKnob)
    : QUndoCommand(0)
    , _imp( new PasteUndoCommandPrivate() )
{
    assert(knob);
    KnobIPtr internalKnob = knob->getKnob();
    _imp->knob = UndoKnobRef(internalKnob);
    _imp->type = type;
    _imp->fromDimension = fromDimension;
    _imp->targetDimension = targetDimension;
    _imp->fromKnob = fromKnob;
    _imp->linkSource = UndoKnobRef(fromKnob);

    {
        Serialization::KnobSerialization originalSerialization;
        internalKnob->toSerialization(&originalSerialization);
        _imp->originalKnob = createDetachedKnobFromSerialization(originalSerialization);
    }
    assert(_imp->originalKnob);

    if (type == eKnobClipBoardTypeCopyLink) {
        getLinkGroupNodes(internalKnob, &_imp->previousLinkNodes);
        for (int i = 0; i < internalKnob->getDimension(); ++i) {
            if (!isLinkPasteTarget(i)) {
                continue;
            }
            _imp->linkDimensions.push_back(i);
            if (!internalKnob->isLinked(i)) {
                continue;
            }
            getKnobLinkPartners(internalKnob, i, &_imp->previousLinks);
        }
    }

    assert(_imp->targetDimension >= -1 && _imp->targetDimension < internalKnob->getDimension());
    assert( _imp->fromDimension >= -1 && _imp->fromDimension < _imp->fromKnob->getDimension() );
    QString text;
    switch (type) {
    case eKnobClipBoardTypeCopyAnim:
        text = tr("Paste Animation to %1");
        break;
    case eKnobClipBoardTypeCopyValue:
        text = tr("Paste Value to %1");
        break;
    case eKnobClipBoardTypeCopyLink:
        text = tr("Paste Link to %1");
        break;
    }
    setText(text.arg(QString::fromUtf8(internalKnob->getLabel().c_str())));
}

PasteUndoCommand::~PasteUndoCommand()
{
}

bool
PasteUndoCommand::isLinkPasteTarget(int dimension) const
{
    KnobIPtr internalKnob = _imp->knob.lock();

    if (!internalKnob || ((_imp->targetDimension != -1) && (dimension != _imp->targetDimension))) {
        return false;
    }

    // KnobI::pasteFrom() links nothing to a permanent (alias) dimension, nor to one the source has no component for.
    return !internalKnob->isLinkPermanent(dimension) && (KnobI::getPasteSourceDimension(dimension, _imp->targetDimension, _imp->fromDimension, _imp->fromKnob->getDimension()) != -1);
}

void
PasteUndoCommand::undo()
{
    KnobIPtr internalKnob = _imp->knob.lock();

    if (!internalKnob) {
        setObsolete(true);

        return;
    }

    if (_imp->type != eKnobClipBoardTypeCopyLink) {
        // The copy has the target's dimensions, so this copies each pasted dimension back.
        ignore_result(internalKnob->pasteFrom(_imp->originalKnob, _imp->type, _imp->targetDimension, _imp->targetDimension));

        return;
    }

    NodesList linkNodes;
    getLinkGroupNodes(internalKnob, &linkNodes);
    internalKnob->beginChanges();
    for (std::vector<int>::const_iterator it = _imp->linkDimensions.begin(); it != _imp->linkDimensions.end(); ++it) {
        internalKnob->leaveLinkGroups(*it);
        internalKnob->cloneAndUpdateGui(_imp->originalKnob.get(), *it, *it);
    }
    restoreKnobLinkPartners(_imp->previousLinks);
    internalKnob->endChanges();
    emitKnobLinksChanged(internalKnob, linkNodes);
} // undo

void
PasteUndoCommand::redo()
{
    KnobIPtr internalKnob = _imp->knob.lock();

    if (!internalKnob) {
        setObsolete(true);

        return;
    }

    if (_imp->type != eKnobClipBoardTypeCopyLink) {
        ignore_result(internalKnob->pasteFrom(_imp->fromKnob, _imp->type, _imp->fromDimension, _imp->targetDimension));

        return;
    }

    KnobIPtr linkSource = _imp->linkSource.lock();
    if (!linkSource) {
        setObsolete(true);

        return;
    }
    ignore_result(internalKnob->pasteFrom(linkSource, _imp->type, _imp->fromDimension, _imp->targetDimension));
    emitKnobLinksChanged(internalKnob, _imp->previousLinkNodes);
} // redo

KnobLinkUndoCommand::KnobLinkUndoCommand(const KnobIPtr& knob,
                                         bool relink)
    : QUndoCommand()
    , _knob(knob)
    , _relink(relink)
    , _localState()
    , _relinkedDimensions()
{
    assert(knob);
    if (relink) {
        Serialization::KnobSerialization serialization;
        knob->toSerialization(&serialization);
        _localState = createDetachedKnobFromSerialization(serialization);
        for (int i = 0; i < knob->getDimension(); ++i) {
            KnobLinkGroupPtr group = knob->getLinkGroup(i);
            if (group && !group->isMemberLinked(knob.get(), i)) {
                _relinkedDimensions.push_back(i);
            }
        }
    }
    QString label = QString::fromUtf8(knob->getLabel().c_str());
    setText(relink ? tr("Re-link %1").arg(label) : tr("Unlink %1").arg(label));
}

KnobLinkUndoCommand::~KnobLinkUndoCommand()
{
}

void
KnobLinkUndoCommand::undo()
{
    setLinked(!_relink);
}

void
KnobLinkUndoCommand::redo()
{
    setLinked(_relink);
}

void
KnobLinkUndoCommand::setLinked(bool linked)
{
    KnobIPtr knob = _knob.lock();

    if (!knob) {
        setObsolete(true);

        return;
    }
    knob->beginChanges();
    if (linked) {
        ignore_result(knob->relinkToGroups());
    } else {
        ignore_result(knob->unlinkFromGroups());
        if (_relink && _localState) {
            // Undoing a re-link: the knob takes back the state the re-link discarded.
            for (std::vector<int>::const_iterator it = _relinkedDimensions.begin(); it != _relinkedDimensions.end(); ++it) {
                knob->cloneAndUpdateGui(_localState.get(), *it, *it);
            }
        }
    }
    knob->endChanges();

    // The knob is still a member of the groups it unlinked from, so their nodes are found either way.
    emitKnobLinksChanged(knob, NodesList());
}

MultipleKnobEditsUndoCommand::MultipleKnobEditsUndoCommand(const KnobGuiPtr& knob,
                                                           ValueChangedReasonEnum reason,
                                                           bool createNew,
                                                           bool setKeyFrame,
                                                           const Variant & value,
                                                           int dimension,
                                                           double time)
    : QUndoCommand()
    , knobs()
    , createNew(createNew)
    , firstRedoCalled(false)
    , _reason(reason)
{
    if (createNew) {
        //qDebug() << "new MultipleKnobEditsUndoCommand";
    }
    assert(knob);
    std::list<ValueToSet>& vlist = knobs[UndoKnobRef(knob->getKnob())];
    ValueToSet v;
    v.newValue = value;
    v.dimension = dimension;
    assert(dimension != -1);
    v.time = time;
    v.setKeyFrame = setKeyFrame;
    v.setValueRetCode = -1;
    vlist.push_back(v);

    KnobHolder* holder = knob->getKnob()->getHolder();
    EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);
    QString holderName;
    if (effect) {
        holderName = QString::fromUtf8( effect->getNode()->getLabel().c_str() );
    }

    setText( tr("Multiple edits of %1").arg(holderName) );
}

MultipleKnobEditsUndoCommand::~MultipleKnobEditsUndoCommand()
{
}

KnobIPtr
MultipleKnobEditsUndoCommand::createCopyForKnob(const KnobIPtr & originalKnob)
{
    const std::string & typeName = originalKnob->typeName();
    KnobIPtr copy;
    int dimension = originalKnob->getDimension();

    if ( typeName == KnobInt::typeNameStatic() ) {
        copy = std::make_shared<KnobInt>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if ( typeName == KnobBool::typeNameStatic() ) {
        copy = std::make_shared<KnobBool>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if ( typeName == KnobDouble::typeNameStatic() ) {
        copy = std::make_shared<KnobDouble>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if ( typeName == KnobChoice::typeNameStatic() ) {
        copy = std::make_shared<KnobChoice>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if ( typeName == KnobString::typeNameStatic() ) {
        copy = std::make_shared<KnobString>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if ( typeName == KnobParametric::typeNameStatic() ) {
        copy = std::make_shared<KnobParametric>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if ( typeName == KnobColor::typeNameStatic() ) {
        copy = std::make_shared<KnobColor>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if ( typeName == KnobPath::typeNameStatic() ) {
        copy = std::make_shared<KnobPath>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if ( typeName == KnobFile::typeNameStatic() ) {
        copy = std::make_shared<KnobFile>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if ( typeName == KnobOutputFile::typeNameStatic() ) {
        copy = std::make_shared<KnobOutputFile>((KnobHolder*)NULL, std::string(), dimension, false);
    }

    ///If this is another type of knob this is wrong since they do not hold any value
    assert(copy);
    if (!copy) {
        return KnobIPtr();
    }
    copy->populate();

    ///make a clone of the original knob at that time and stash it
    copy->clone(originalKnob);

    return copy;
}

KnobHolder*
MultipleKnobEditsUndoCommand::liveHolder() const
{
    for (ParamsMap::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        KnobIPtr knob = it->first.lock();
        if (knob && knob->getHolder()) {
            return knob->getHolder();
        }
    }

    return 0;
}

bool
MultipleKnobEditsUndoCommand::isCurrentFrame(const KnobIPtr& knob,
                                             double time)
{
    KnobHolder* holder = knob->getHolder();
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();

    return app && (time == app->getTimeLine()->currentFrame());
}

void
MultipleKnobEditsUndoCommand::undo()
{
    KnobHolder* holder = liveHolder();
    if (!holder) {
        setObsolete(true);

        return;
    }
    holder->beginChanges();

    for (ParamsMap::iterator it = knobs.begin(); it != knobs.end(); ++it) {
        KnobIPtr knob = it->first.lock();
        if (!knob) {
            continue;
        }
        //qDebug() << "Undo" << knob->getName().c_str();
        knob->beginChanges();
        KnobIntBase* isInt = dynamic_cast<KnobIntBase*>( knob.get() );
        KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>( knob.get() );
        KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>( knob.get() );
        KnobStringBase* isString = dynamic_cast<KnobStringBase*>( knob.get() );
        KeyFrame k;
        // For setValue calls, only set back the old value once per-dimension
        std::set<int> dimensionsUndone;
        for (std::list<ValueToSet>::iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
            KnobHelper::ValueChangedReturnCodeEnum retCode = (KnobHelper::ValueChangedReturnCodeEnum)it2->setValueRetCode;

            if (retCode == KnobHelper::eValueChangedReturnCodeKeyframeAdded) {
                //qDebug() << "Undo " << knob->getName().c_str() << "[" << it2->dimension << "] remove keyFrame";
                KnobGui::removeKnobKeyframe(knob, it2->time, it2->dimension, ViewIdx(0));
            } else {
                if (it2->setKeyFrame) {
                    bool refreshGui = isCurrentFrame(knob, it2->time);
                    if (isInt) {
                        //qDebug() << "Undo " << knob->getName().c_str() << "[" << it2->dimension << "] to " << it2->oldValue.toInt() << "at time" << it2->time;
                        KnobGui::setKnobValueAtTime<int>(knob, it2->dimension, it2->oldValue.toInt(), it2->time, ViewIdx(0), &k, refreshGui, _reason);
                    } else if (isBool) {
                        //qDebug() << "Undo " << knob->getName().c_str() << "[" << it2->dimension << "] to " << it2->oldValue.toBool() << "at time" << it2->time;
                        KnobGui::setKnobValueAtTime<bool>(knob, it2->dimension, it2->oldValue.toBool(), it2->time, ViewIdx(0), &k, refreshGui, _reason);
                    } else if (isDouble) {
                        //qDebug() << "Undo " << knob->getName().c_str() << "[" << it2->dimension << "] to " << it2->oldValue.toDouble() << "at time" << it2->time;
                        KnobGui::setKnobValueAtTime<double>(knob, it2->dimension, it2->oldValue.toDouble(), it2->time, ViewIdx(0), &k, refreshGui, _reason);
                    } else if (isString) {
                        //qDebug() << "Undo " << knob->getName().c_str() << "[" << it2->dimension << "] to " << it2->oldValue.toString() << "at time" << it2->time;
                        KnobGui::setKnobValueAtTime<std::string>(knob, it2->dimension, it2->oldValue.toString().toStdString(), it2->time, ViewIdx(0), &k, refreshGui, _reason);
                    } else {
                        assert(false);
                    }
                } else {
                    // Only set back the first value that was set in the command
                    if (dimensionsUndone.find(it2->dimension) != dimensionsUndone.end()) {
                        continue;
                    }
                    dimensionsUndone.insert(it2->dimension);
                    if (isInt) {
                        int oldValue = it2->oldValue.toInt();
                        if (isInt->getValueAtTime(it2->time, it2->dimension) != oldValue) {
                            //qDebug() << "Undo" << knob->getName().c_str() << "[" << it2->dimension << "] to " << it2->oldValue.toInt();
                            KnobGui::setKnobValue<int>(knob, it2->dimension, oldValue, &k, true, _reason);
                        }
                    } else if (isBool) {
                        bool oldValue = it2->oldValue.toBool();
                        if (isBool->getValueAtTime(it2->time, it2->dimension) != oldValue) {
                            //qDebug() << "Undo" << knob->getName().c_str() << "[" << it2->dimension << "] to " << it2->oldValue.toBool();
                            KnobGui::setKnobValue<bool>(knob, it2->dimension, oldValue, &k, true, _reason);
                        }
                    } else if (isDouble) {
                        double oldValue = it2->oldValue.toDouble();
                        if (isDouble->getValueAtTime(it2->time, it2->dimension) != oldValue) {
                            //qDebug() << "Undo" << knob->getName().c_str() << "[" << it2->dimension << "] to " << it2->oldValue.toDouble();
                            KnobGui::setKnobValue<double>(knob, it2->dimension, oldValue, &k, true, _reason);
                        }
                    } else if (isString) {
                        std::string oldValue = it2->oldValue.toString().toStdString();
                        if (isString->getValueAtTime(it2->time, it2->dimension) != oldValue) {
                            //qDebug() << "Undo" << knob->getName().c_str() << "[" << it2->dimension << "] to " << it2->oldValue.toString();
                            KnobGui::setKnobValue<std::string>(knob, it2->dimension, oldValue,
                                                               &k, true, _reason);
                        }
                    } else {
                        assert(false);
                    }
                }
            }
        }
        knob->endChanges();
    }

    holder->endChanges();
} // MultipleKnobEditsUndoCommand::undo

void
MultipleKnobEditsUndoCommand::redo()
{
    KnobHolder* holder = liveHolder();
    if (!holder) {
        setObsolete(true);

        return;
    }
    holder->beginChanges();

    ///this is the first redo command, set values
    for (ParamsMap::iterator it = knobs.begin(); it != knobs.end(); ++it) {
        KnobIPtr knob = it->first.lock();
        if (!knob) {
            continue;
        }
        //qDebug() << "redo: knob" << knob->getName().c_str();
        knob->beginChanges();
        KnobIntBase* isInt = dynamic_cast<KnobIntBase*>( knob.get() );
        KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>( knob.get() );
        KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>( knob.get() );
        KnobStringBase* isString = dynamic_cast<KnobStringBase*>( knob.get() );

        for (std::list<ValueToSet>::iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
            //qDebug() << "redo: value change" << it2->dimension << it2->time << it2->setKeyFrame << "new:" << it2->newValue << "old:" << it2->oldValue;
            KeyFrame k;
            if (!firstRedoCalled) {
                //qDebug() << "redo: !firstRedoCalled (not the first redo), setting old value";
                if (isInt) {
                    it2->oldValue.setValue( isInt->getValueAtTime(it2->time, it2->dimension) );
                } else if (isBool) {
                    it2->oldValue.setValue( isBool->getValueAtTime(it2->time, it2->dimension) );
                } else if (isDouble) {
                    it2->oldValue.setValue( isDouble->getValueAtTime(it2->time, it2->dimension) );
                } else if (isString) {
                    it2->oldValue.setValue( isString->getValueAtTime(it2->time, it2->dimension) );
                }
            }

            if (it2->setKeyFrame) {
                bool keyAdded = false;
                bool refreshGui = isCurrentFrame(knob, it2->time);
                if (isInt) {
                    keyAdded = KnobGui::setKnobValueAtTime<int>(knob, it2->dimension, it2->newValue.toInt(), it2->time, ViewIdx(0), &k, refreshGui, _reason);
                } else if (isBool) {
                    keyAdded = KnobGui::setKnobValueAtTime<bool>(knob, it2->dimension, it2->newValue.toBool(), it2->time, ViewIdx(0), &k, refreshGui, _reason);
                } else if (isDouble) {
                    keyAdded = KnobGui::setKnobValueAtTime<double>(knob, it2->dimension, it2->newValue.toDouble(), it2->time, ViewIdx(0), &k, refreshGui, _reason);
                } else if (isString) {
                    keyAdded = KnobGui::setKnobValueAtTime<std::string>(knob, it2->dimension, it2->newValue.toString().toStdString(), it2->time,
                                                                        ViewIdx(0), &k, refreshGui, _reason);
                } else {
                    assert(false);
                }
                it2->setValueRetCode = keyAdded ? KnobHelper::eValueChangedReturnCodeKeyframeAdded : KnobHelper::eValueChangedReturnCodeKeyframeModified;
            } else {
                if (isInt) {
                    int newValue = it2->newValue.toInt();
                    if (isInt->getValueAtTime(it2->time, it2->dimension) != newValue) {
                        it2->setValueRetCode = KnobGui::setKnobValue<int>(knob, it2->dimension, newValue, &k, true, _reason);
                    }
                } else if (isBool) {
                    bool newValue = it2->newValue.toBool();
                    if (isBool->getValueAtTime(it2->time, it2->dimension) != newValue) {
                        it2->setValueRetCode = KnobGui::setKnobValue<bool>(knob, it2->dimension, newValue, &k, true, _reason);
                    }
                } else if (isDouble) {
                    double newValue = it2->newValue.toDouble();
                    if (isDouble->getValueAtTime(it2->time, it2->dimension) != newValue) {
                        it2->setValueRetCode = KnobGui::setKnobValue<double>(knob, it2->dimension, newValue, &k, true, _reason);
                    }
                } else if (isString) {
                    std::string newValue = it2->newValue.toString().toStdString();
                    if (isString->getValueAtTime(it2->time, it2->dimension) != newValue) {
                        it2->setValueRetCode = KnobGui::setKnobValue<std::string>(knob, it2->dimension, newValue, &k, true, _reason);
                    }
                } else {
                    assert(false);
                }
                if (!firstRedoCalled && !it2->setKeyFrame) {
                    it2->time = knob->getCurrentTime();
                }
            }
        }
        knob->endChanges();
    }

    holder->endChanges();

    firstRedoCalled = true;
    //qDebug() << "redo: firstRedoCalled=true";
} // redo

int
MultipleKnobEditsUndoCommand::id() const
{
    return kMultipleKnobsUndoChangeCommandCompressionID;
}

bool
MultipleKnobEditsUndoCommand::mergeWith(const QUndoCommand *command)
{
    /*
     * The command in parameter just had its redo() function call and we attempt to merge it with a previous
     * command that has been redone already
     */
    const MultipleKnobEditsUndoCommand *knobCommand = dynamic_cast<const MultipleKnobEditsUndoCommand *>(command);

    if ( !knobCommand || ( command->id() != id() ) ) {
        //qDebug() << "mergeWith failed: knobCommand=" << bool(knobCommand) << "id=" << command->id() << "!=" << id();
        return false;
    }

    // if all knobs from the new command are included in the old command, ignore the createNew flag and merge them anyway
    if (knobCommand->createNew) {
        bool ignoreCreateNew = false;
        //qDebug() << "mergeWith" << knobs.size() << "with" << knobCommand->knobs.size();
        if ( knobs.size() >= knobCommand->knobs.size() ) {
            ParamsMap::const_iterator thisIt = knobs.begin();
            ParamsMap::const_iterator otherIt = knobCommand->knobs.begin();
            bool oneDifferent = false;
            for (; thisIt != knobs.end() && otherIt != knobCommand->knobs.end(); ++thisIt) {
                if (otherIt->first < thisIt->first) {
                    //qDebug() << thisItKey->getKnob()->getName().c_str() << "!=" << otherItKey->getKnob()->getName().c_str();
                    
                    oneDifferent = true;
                    break;
                } else if (!(thisIt->first < otherIt->first)) {
                    //qDebug() << thisItKey->getKnob()->getName().c_str() << "==" << otherItKey->getKnob()->getName().c_str();
                    ++otherIt;
                }
            }
            if (!oneDifferent) {
                ignoreCreateNew = true;
            }
        }
        
        if (!ignoreCreateNew) {
            //qDebug() << "mergeWith failed";
            return false;
        }
    }
    //qDebug() << "mergeWith: list of existing knob/values:";
    //for (ParamsMap::const_iterator thisIt = knobs.begin(); thisIt != knobs.end(); ++thisIt) {
    //    qDebug() << "mergeWith existing knob" << thisIt->first.lock()->getKnob()->getName().c_str();
    //    for (std::list<ValueToSet>::const_iterator vit = thisIt->second.begin();
    //         vit != thisIt->second.end(); ++vit) {
    //        qDebug() << "mergeWith: existing value change" << vit->dimension << vit->time << vit->setKeyFrame << "new:" << vit->newValue << "old:" << vit->oldValue;
    //    }
    //
    //}
    
    for (ParamsMap::const_iterator otherIt = knobCommand->knobs.begin(); otherIt != knobCommand->knobs.end(); ++otherIt) {
        ParamsMap::iterator foundExistinKnob =  knobs.find(otherIt->first);
        if ( foundExistinKnob == knobs.end() ) {
            //qDebug() << "mergeWith: adding knob" << otherIt->first.lock()->getKnob()->getName().c_str();

            knobs.insert(*otherIt);
        } else {
            //qDebug() << "mergeWith: adding values to knob" << otherIt->first.lock()->getKnob()->getName().c_str();
            // Merge the value changes with the existing ones:
            // for each ValueToSet v in otherIt->second,
            // if there is a ValueToSet w in foundExistinKnob->second such that
            // - v.dimension == w.dimension
            // - v.time == w.time
            // - v.setKeyFrame == w.setKeyFrame
            // - v.oldValue == w.newValue (optional)
            // then set w.newValue = v.newValue
            // else add new value change: foundExistinKnob->second.push_back(v)
            for (std::list<ValueToSet>::const_iterator vit = otherIt->second.begin();
                 vit != otherIt->second.end(); ++vit) {
                //qDebug() << "mergeWith: adding value change" << vit->dimension << vit->time << vit->setKeyFrame << "new:" << vit->newValue << "old:" << vit->oldValue;
                bool found = false;
                for (std::list<ValueToSet>::iterator wit = foundExistinKnob->second.begin();
                     !found && wit != foundExistinKnob->second.end(); ++wit) {
                    //qDebug() << "mergeWith: testing compat with value change" << wit->dimension << wit->time << wit->setKeyFrame << "new:" << wit->newValue << "old:" << wit->oldValue;
                    if (vit->dimension == wit->dimension &&
                        vit->time == wit->time &&
                        vit->setKeyFrame == wit->setKeyFrame) {
                        //qDebug() << "mergeWith: update value for dim" << vit->dimension << "of knob" << foundExistinKnob->first.lock()->getKnob()->getName().c_str();
                        if (vit->oldValue != wit->newValue) {
                            qDebug() << "mergeWith: warning: old=" << QVariant(vit->oldValue) << "!= new=" << QVariant(wit->newValue);
                        }
                        if (wit->newValue != vit->newValue) {
                            //qDebug() << "mergeWith: update newValue" << wit->newValue << "->" << vit->newValue << "oldValue remains" << wit->oldValue;
                            wit->newValue = vit->newValue;
                        }
                        found = true;
                    } else {
                        //qDebug() << "mergeWith incompatible value change";
                    }
                }
                if (!found) {
                    //qDebug() << "mergeWith did not find dim" << vit->dimension << "of knob" << foundExistinKnob->first.lock()->getKnob()->getName().c_str() << "-> adding it";
                    foundExistinKnob->second.push_back(*vit);
                }
            }
        }
    }

    //qDebug() << "mergeWith succeeded";
    return true;
}

RestoreDefaultsCommand::RestoreDefaultsCommand(bool isNodeReset,
                                               const std::list<KnobIPtr> & knobs,
                                               int targetDim,
                                               QUndoCommand *parent)
    : QUndoCommand(parent)
    , _isNodeReset(isNodeReset)
    , _targetDim(targetDim)
    , _knobs()
{
    for (std::list<KnobIPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        _knobs.push_back(UndoKnobRef(*it));
        _clones.push_back( MultipleKnobEditsUndoCommand::createCopyForKnob(*it) );
    }

    KnobIPtr first = knobs.empty() ? KnobIPtr() : knobs.front();
    KnobHolder* holder = first ? first->getHolder() : nullptr;
    EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);
    QString holderName;
    if (effect) {
        holderName = QString::fromUtf8( effect->getNode()->getLabel().c_str() );
    }

    if ((_knobs.size() == 1) && first) {
        setText( tr("Reset %1.%2 to default").arg(holderName).arg( QString::fromUtf8( first->getLabel().c_str() ) ) );
    } else {
        setText( tr("Reset %1 to default").arg(holderName) );
    }
}

void
RestoreDefaultsCommand::undo()
{
    assert( _clones.size() == _knobs.size() );

    std::list<SequenceTime> times;
    KnobHolder* holder = liveHolder();
    if (!holder) {
        setObsolete(true);

        return;
    }
    AppInstancePtr app = holder->getApp();
    std::list<KnobIPtr>::iterator itClone = _clones.begin();
    for (std::list<UndoKnobRef>::const_iterator it = _knobs.begin(); it != _knobs.end(); ++it, ++itClone) {
        // A knob that is gone keeps its clone: a later preset may recreate it.
        KnobIPtr itKnob = it->lock();
        if (!itKnob) {
            continue;
        }
        if (!*itClone) {
            assert(false); // We own the clone, so it should always be valid

            continue;
        }
        itKnob->cloneAndUpdateGui( itClone->get() );

        if (itKnob->getHolder() && itKnob->getHolder()->getApp()) {
            int dim = itKnob->getDimension();
            for (int i = 0; i < dim; ++i) {
                if ( (i == _targetDim) || (_targetDim == -1) ) {
                    CurvePtr c = itKnob->getCurve(ViewIdx(0), i);
                    if (c) {
                        KeyFrameSet kfs = c->getKeyFrames_mt_safe();
                        for (KeyFrameSet::iterator it = kfs.begin(); it != kfs.end(); ++it) {
                            times.push_back( std::floor(it->getTime() + 0.5) );
                        }
                    }
                }
            }
        }
    }
    if (app) {
        app->addMultipleKeyframeIndicatorsAdded(times, true);
    }

    holder->incrHashAndEvaluate(true, true);
    if (app) {
        app->redrawAllViewers();
    }
}

KnobHolder*
RestoreDefaultsCommand::liveHolder() const
{
    for (std::list<UndoKnobRef>::const_iterator it = _knobs.begin(); it != _knobs.end(); ++it) {
        KnobIPtr knob = it->lock();
        if (knob && knob->getHolder()) {
            return knob->getHolder();
        }
    }

    return 0;
}

void
RestoreDefaultsCommand::redo()
{
    std::list<SequenceTime> times;
    KnobHolder* holder = liveHolder();
    if (!holder) {
        setObsolete(true);

        return;
    }
    AppInstancePtr app = holder->getApp();
    EffectInstance* isEffect = dynamic_cast<EffectInstance*>(holder);

    holder->beginChanges();

    /*
       First reset all knobs values, this will not call instanceChanged action
     */
    for (std::list<UndoKnobRef>::iterator it = _knobs.begin(); it != _knobs.end(); ++it) {
        KnobIPtr itKnob = it->lock();
        if (!itKnob) {
            continue;
        }
        if ( itKnob->getHolder() && itKnob->getHolder()->getApp() ) {
            int dim = itKnob->getDimension();
            for (int i = 0; i < dim; ++i) {
                if ( (i == _targetDim) || (_targetDim == -1) ) {
                    CurvePtr c = itKnob->getCurve(ViewIdx(0), i);
                    if (c) {
                        KeyFrameSet kfs = c->getKeyFrames_mt_safe();
                        for (KeyFrameSet::iterator it = kfs.begin(); it != kfs.end(); ++it) {
                            times.push_back( std::floor(it->getTime() + 0.5) );
                        }
                    }
                }
            }
        }

        if ( itKnob->getHolder() ) {
            itKnob->getHolder()->beginChanges();
        }
       // itKnob->blockValueChanges();

        for (int d = 0; d < itKnob->getDimension(); ++d) {
            if ( (d == _targetDim) || (_targetDim == -1) ) {
                itKnob->resetToDefaultValue(d);
            }
        }

        // itKnob->unblockValueChanges();

        if ( itKnob->getHolder() ) {
            itKnob->getHolder()->endChanges(true);
        }
    }

    /*
       Block value changes and call instanceChange on all knobs  afterwards to put back the plug-in
       in a correct state
     */
    double time = 0;
    if (app) {
        time = app->getTimeLine()->currentFrame();
    }
    for (std::list<UndoKnobRef>::iterator it = _knobs.begin(); it != _knobs.end(); ++it) {
        KnobIPtr itKnob = it->lock();
        if (!itKnob) {
            continue;
        }
        if ( itKnob->getHolder() ) {
            itKnob->getHolder()->onKnobValueChanged_public(itKnob.get(), eValueChangedReasonRestoreDefault, time, ViewIdx(0), true);
        }
    }

    if (app) {
        app->removeMultipleKeyframeIndicator(times, true);
    }

    if (app) {
        holder->endChanges();
    }

    if (_isNodeReset && isEffect) {
        isEffect->purgeCaches();
    }

    holder->incrHashAndEvaluate(true, true);
    if (app) {
        app->redrawAllViewers();
    }
} // RestoreDefaultsCommand::redo

SetExpressionCommand::SetExpressionCommand(const KnobIPtr & knob,
                                           bool hasRetVar,
                                           int dimension,
                                           const std::string& expr,
                                           QUndoCommand *parent)
    : QUndoCommand(parent)
    , _knob(knob)
    , _oldExprs()
    , _hadRetVar()
    , _newExpr(expr)
    , _hasRetVar(hasRetVar)
    , _dimension(dimension)
{
    for (int i = 0; i < knob->getDimension(); ++i) {
        _oldExprs.push_back( knob->getExpression(i) );
        _hadRetVar.push_back( knob->isExpressionUsingRetVariable(i) );
    }
    setText( tr("Set expression") );
}

void
SetExpressionCommand::undo()
{
    KnobIPtr knob = _knob.lock();

    if (!knob) {
        setObsolete(true);

        return;
    }
    for (int i = 0; i < knob->getDimension(); ++i) {
        try {
            knob->setExpression(i, _oldExprs[i], _hadRetVar[i], false);
        } catch (...) {
            Dialogs::errorDialog( tr("Expression").toStdString(), tr("The expression is invalid.").toStdString() );
            break;
        }
    }

    knob->evaluateValueChange(_dimension == -1 ? 0 : _dimension, knob->getCurrentTime(), ViewIdx(0), eValueChangedReasonNatronGuiEdited);
}

void
SetExpressionCommand::redo()
{
    KnobIPtr knob = _knob.lock();

    if (!knob) {
        setObsolete(true);

        return;
    }
    if (_dimension == -1) {
        for (int i = 0; i < knob->getDimension(); ++i) {
            try {
                knob->setExpression(i, _newExpr, _hasRetVar, false);
            } catch (...) {
                Dialogs::errorDialog( tr("Expression").toStdString(), tr("The expression is invalid.").toStdString() );
                break;
            }
        }
    } else {
        try {
            knob->setExpression(_dimension, _newExpr, _hasRetVar, false);
        } catch (...) {
            Dialogs::errorDialog( tr("Expression").toStdString(), tr("The expression is invalid.").toStdString() );
        }
    }
    knob->evaluateValueChange(_dimension == -1 ? 0 : _dimension, knob->getCurrentTime(), ViewIdx(0), eValueChangedReasonNatronGuiEdited);
}

NATRON_NAMESPACE_EXIT
