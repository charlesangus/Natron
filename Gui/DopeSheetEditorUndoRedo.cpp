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

#include "DopeSheetEditorUndoRedo.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <vector>

#include <QDebug>

#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Time/FrameRange.h"
#include "Engine/Nodes/Time/TimeOffset.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

#include "Global/GlobalDefines.h"

#include "Gui/DockablePanel.h"
#include "Gui/DopeSheet.h"
#include "Gui/DopeSheetEditor.h"
#include "Gui/DopeSheetView.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/NodeGui.h"

NATRON_NAMESPACE_ENTER

typedef std::map<KnobIWPtr, KnobGui *> KnobsAndGuis;


////////////////////////// Helpers //////////////////////////


NATRON_NAMESPACE_ANONYMOUS_ENTER

/**
 * @brief Move the node 'reader' on project timeline, by offsetting its
 * starting time by 'dt'.
 */
void
moveReader(const NodePtr &reader,
           double dt)
{
    KnobIntBase *startingTimeKnob = dynamic_cast<KnobIntBase *>( reader->getKnobByName(kReaderParamNameStartingTime).get() );
    assert(startingTimeKnob);
    KnobHelper::ValueChangedReturnCodeEnum s = startingTimeKnob->setValue(startingTimeKnob->getValue() + dt, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
    Q_UNUSED(s);
}

KnobDoublePtr
getTimeOffsetKnob(const NodePtr& node)
{
    return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kTimeOffsetParamTimeOffset));
}

/**
 * @brief Move a TimeOffset by dt: everything it shows comes dt later. An animated offset has its
 * curve moved by dt in time and in value, and the selected keys on it follow their keyframes.
 */
void
moveTimeOffset(const NodePtr& node,
               double dt,
               const DopeSheetKeyPtrList& selectedKeys)
{
    KnobDoublePtr timeOffsetKnob = getTimeOffsetKnob(node);
    assert(timeOffsetKnob);
    if (!timeOffsetKnob) {
        return;
    }
    if (!timeOffsetKnob->isAnimated(0, ViewIdx(0))) {
        KnobHelper::ValueChangedReturnCodeEnum s = timeOffsetKnob->setValue(timeOffsetKnob->getValue() + dt, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
        Q_UNUSED(s);

        return;
    }

    KeyFrameSet keyframes = timeOffsetKnob->getCurve(ViewIdx(0), 0)->getKeyFrames_mt_safe();
    std::vector<KeyFrame> ordered(keyframes.begin(), keyframes.end());
    // Move the keys in the direction of the move first, so that none lands on one not yet moved.
    if (dt > 0) {
        std::reverse(ordered.begin(), ordered.end());
    }
    for (std::vector<KeyFrame>::const_iterator it = ordered.begin(); it != ordered.end(); ++it) {
        KeyFrame moved;
        if (!timeOffsetKnob->moveValueAtTime(eCurveChangeReasonDopeSheet, it->getTime(), ViewSpec::all(), 0, dt, dt, &moved)) {
            continue;
        }
        for (DopeSheetKeyPtrList::const_iterator keyIt = selectedKeys.begin(); keyIt != selectedKeys.end(); ++keyIt) {
            if (((*keyIt)->knob.lock() == timeOffsetKnob) && ((*keyIt)->dimension == 0) && ((*keyIt)->key.getTime() == it->getTime())) {
                (*keyIt)->key = moved;
            }
        }
    }
}

void
moveFrameRange(const NodePtr& node,
               double dt)
{
    KnobIntPtr firstFrameKnob = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kFrameRangeParamFirstFrame));
    KnobIntPtr lastFrameKnob = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kFrameRangeParamLastFrame));
    assert(firstFrameKnob && lastFrameKnob);
    if (!firstFrameKnob || !lastFrameKnob) {
        return;
    }
    const int first = firstFrameKnob->getValue();
    const int last = lastFrameKnob->getValue();
    // Move the end in the direction of the move first, so the range never turns inside out.
    if (dt > 0) {
        lastFrameKnob->setValue(static_cast<int>(last + dt), ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
        firstFrameKnob->setValue(static_cast<int>(first + dt), ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
    } else {
        firstFrameKnob->setValue(static_cast<int>(first + dt), ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
        lastFrameKnob->setValue(static_cast<int>(last + dt), ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
    }
}

void
moveGroupNode(DopeSheetEditor* model,
              const NodePtr& node,
              double dt,
              const DopeSheetKeyPtrList& selectedKeys)
{
    NodeGroup *group = node->isEffectGroup();

    assert(group);
    NodesList nodes;
    group->getNodes_recursive(nodes, true);

    for (NodesList::iterator it = nodes.begin(); it != nodes.end(); ++it) {
        NodeGuiPtr nodeGui = std::dynamic_pointer_cast<NodeGui>( (*it)->getNodeGui() );
        assert(nodeGui);
        std::string pluginID = (*it)->getPluginID();
        NodeGroup* isChildGroup = (*it)->isEffectGroup();

        // Move readers
        KnobDoublePtr movedOffset;
        if (pluginID == PLUGINID_NATRON_READ) {
            moveReader(*it, dt);
        } else if (pluginID == PLUGINID_NATRON_TIMEOFFSET) {
            moveTimeOffset(*it, dt, selectedKeys);
            movedOffset = getTimeOffsetKnob(*it);
        } else if (pluginID == PLUGINID_NATRON_FRAMERANGE) {
            moveFrameRange(*it, dt);
        } else if (isChildGroup) {
            moveGroupNode(model, *it, dt, selectedKeys);
        }

        // Move keyframes
        const KnobsVec &knobs = (*it)->getKnobs();

        for (KnobsVec::const_iterator knobIt = knobs.begin(); knobIt != knobs.end(); ++knobIt) {
            const KnobIPtr& knob = *knobIt;
            if (!knob->hasAnimation() || (movedOffset && (knob == movedOffset))) {
                continue;
            }

            for (int dim = 0; dim < knob->getDimension(); ++dim) {
                if ( !knob->isAnimated( dim, ViewIdx(0) ) ) {
                    continue;
                }
                KeyFrameSet keyframes = knob->getCurve(ViewIdx(0), dim)->getKeyFrames_mt_safe();

                for (KeyFrameSet::iterator kfIt = keyframes.begin(); kfIt != keyframes.end(); ++kfIt) {
                    KeyFrame kf = (*kfIt);
                    KeyFrame fake;

                    knob->moveValueAtTime(eCurveChangeReasonDopeSheet, kf.getTime(), ViewSpec::all(), dim, dt, 0, &fake);
                }
            }
        }
    }
} // moveGroupNode

NATRON_NAMESPACE_ANONYMOUS_EXIT


////////////////////////// DSMoveKeysCommand //////////////////////////

DSMoveKeysAndNodesCommand::DSMoveKeysAndNodesCommand(const DopeSheetKeyPtrList &keys,
                                                     const std::vector<DSNodePtr>& nodes,
                                                     double dt,
                                                     DopeSheetEditor *model,
                                                     QUndoCommand *parent)
    : QUndoCommand(parent),
    _keys(keys),
    _nodes(),
    _dt(dt),
    _model(model)
{
    setText( tr("Move selected keys") );
    std::set<NodePtr> nodesSet;
    for (std::vector<DSNodePtr>::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        DopeSheetItemType type = (*it)->getItemType();
        if ( (type != eDopeSheetItemTypeReader) &&
             ( type != eDopeSheetItemTypeGroup) &&
             ( type != eDopeSheetItemTypeTimeOffset) &&
             ( type != eDopeSheetItemTypeFrameRange) ) {
            //Note that Retime nodes cannot be moved
            continue;
        }
        MovedNode moved;
        moved.node = (*it)->getInternalNode();
        moved.type = type;
        _nodes.push_back(moved);
        nodesSet.insert( (*it)->getInternalNode() );
        NodeGroup* isGroup = (*it)->getInternalNode()->isEffectGroup();
        if (isGroup) {
            NodesList recurseNodes;
            isGroup->getNodes_recursive(recurseNodes, true);
            for (NodesList::iterator it = recurseNodes.begin(); it != recurseNodes.end(); ++it) {
                nodesSet.insert(*it);
            }
        }
    }

    for (DopeSheetKeyPtrList::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        KnobIPtr knob = (*it)->knob.lock();
        KnobHolder* holder = knob ? knob->getHolder() : 0;
        EffectInstance* isEffect = dynamic_cast<EffectInstance*>(holder);
        if (isEffect) {
            nodesSet.insert( isEffect->getNode() );
        }
    }

    for (std::set<NodePtr>::iterator it = nodesSet.begin(); it != nodesSet.end(); ++it) {
        _allDifferentNodes.push_back(*it);
    }
}

void
DSMoveKeysAndNodesCommand::undo()
{
    moveSelection(-_dt);
}

void
DSMoveKeysAndNodesCommand::redo()
{
    moveSelection(_dt);
}

void
DSMoveKeysAndNodesCommand::moveSelection(double dt)
{
    for (DopeSheetKeyPtrList::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        if (!lockUndoKnob((*it)->knob)) {
            setObsolete(true);

            return;
        }
    }

    /*
       The view is going to get MANY update() calls since every knob change will trigger a computeNodeRange() in DopeSheetView
       We explicitly disable update on the dopesheet view and re-enable it afterwards.
     */
    DopeSheetView* view = _model->getDopesheetView();

    view->setUpdatesEnabled(false);

    for (NodesWList::iterator khIt = _allDifferentNodes.begin(); khIt != _allDifferentNodes.end(); ++khIt) {
        NodePtr node = khIt->lock();
        if (!node) {
            continue;
        }
        node->getEffectInstance()->beginChanges();
    }

    // Moving a TimeOffset moves the keys of its offset itself.
    std::set<KnobIPtr> offsetsMovedWithNodes;
    for (std::vector<MovedNode>::const_iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        NodePtr node = it->node.lock();
        if (!node) {
            continue;
        }
        NodesList moved;
        if (it->type == eDopeSheetItemTypeTimeOffset) {
            moved.push_back(node);
        } else if (NodeGroup* group = node->isEffectGroup()) {
            group->getNodes_recursive(moved, true);
        }
        for (NodesList::const_iterator movedIt = moved.begin(); movedIt != moved.end(); ++movedIt) {
            if ((*movedIt)->getPluginID() == PLUGINID_NATRON_TIMEOFFSET) {
                KnobDoublePtr offset = getTimeOffsetKnob(*movedIt);
                if (offset) {
                    offsetsMovedWithNodes.insert(offset);
                }
            }
        }
    }

    //////////Handle selected keyframes
    for (DopeSheetKeyPtrList::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        const DopeSheetKeyPtr& selectedKey = (*it);
        KnobIPtr knob = selectedKey->knob.lock();
        if (offsetsMovedWithNodes.count(knob)) {
            continue;
        }

        knob->moveValueAtTime(eCurveChangeReasonDopeSheet, selectedKey->key.getTime(), ViewIdx(0),
                              selectedKey->dimension,
                              dt, 0, &selectedKey->key);
    }
    ////////////Handle selected nodes
    for (std::vector<MovedNode>::iterator it = _nodes.begin(); it != _nodes.end(); ++it) {
        NodePtr node = it->node.lock();
        if (!node) {
            continue;
        }
        if (it->type == eDopeSheetItemTypeReader) {
            moveReader(node, dt);
        } else if (it->type == eDopeSheetItemTypeFrameRange) {
            moveFrameRange(node, dt);
        } else if (it->type == eDopeSheetItemTypeTimeOffset) {
            moveTimeOffset(node, dt, _keys);
        } else if (it->type == eDopeSheetItemTypeGroup) {
            moveGroupNode(_model, node, dt, _keys);
        }
    }

    for (NodesWList::const_iterator khIt = _allDifferentNodes.begin(); khIt != _allDifferentNodes.end(); ++khIt) {
        NodePtr node = khIt->lock();
        if (!node) {
            continue;
        }
        node->getEffectInstance()->endChanges();
    }

    view->setUpdatesEnabled(true);


    _model->refreshSelectionBboxAndRedrawView();
} // DSMoveKeysAndNodesCommand::moveSelection

int
DSMoveKeysAndNodesCommand::id() const
{
    return kDopeSheetEditorMoveKeysCommandCompressionID;
}

bool
DSMoveKeysAndNodesCommand::mergeWith(const QUndoCommand *other)
{
    const DSMoveKeysAndNodesCommand *cmd = dynamic_cast<const DSMoveKeysAndNodesCommand *>(other);

    assert(cmd);
    if (!cmd || (cmd->id() != id()) || !inSameGestureAs(*cmd)) {
        return false;
    }

    if ( cmd->_keys.size() != _keys.size() ) {
        return false;
    }

    if ( cmd->_nodes.size() != _nodes.size() ) {
        return false;
    }

    {
        DopeSheetKeyPtrList::const_iterator itOther = cmd->_keys.begin();

        for (DopeSheetKeyPtrList::const_iterator it = _keys.begin(); it != _keys.end(); ++it, ++itOther) {
            if (*itOther != *it) {
                return false;
            }
        }
    }

    std::vector<MovedNode>::const_iterator itOther = cmd->_nodes.begin();
    for (std::vector<MovedNode>::const_iterator it = _nodes.begin(); it != _nodes.end(); ++it, ++itOther) {
        if (itOther->node.lock() != it->node.lock()) {
            return false;
        }
    }

    _dt += cmd->_dt;

    return true;
}

//////////////////////////DSTransformKeysCommand //////////////////////////

DSTransformKeysCommand::DSTransformKeysCommand(const DopeSheetKeyPtrList &keys,
                                               const Transform::Matrix3x3& transform,
                                               DopeSheetEditor *model,
                                               QUndoCommand *parent)
    : QUndoCommand(parent)
    , _firstRedoCalled(false)
    , _transform(transform)
    , _model(model)
{
    for (DopeSheetKeyPtrList::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        KnobIPtr knob = (*it)->knob.lock();
        if (!knob) {
            continue;
        }

        TransformKeys::iterator found = _keys.begin();
        for (; found != _keys.end(); ++found) {
            if ((found->knob.lock() == knob) && (found->dimension == (*it)->dimension)) {
                break;
            }
        }
        if (found == _keys.end()) {
            TransformKeyData data;
            data.knob = knob;
            data.dimension = (*it)->dimension;
            found = _keys.insert(_keys.end(), data);
        }
        found->keys.push_back(*it);
    }

    setText( tr("Scale keyframes") );
}

void
DSTransformKeysCommand::undo()
{
    /*
       The view is going to get MANY update() calls since every knob change will trigger a computeNodeRange() in DopeSheetView
       We explicitly disable update on the dopesheet view and re-enable it afterwards.
     */
    if (!knobsAreAlive()) {
        setObsolete(true);

        return;
    }
    DopeSheetView* view = _model->getDopesheetView();

    view->setUpdatesEnabled(false);

    std::list<KnobHolder*> differentKnobs;
    for (TransformKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        KnobHolder* holder = it->knob.lock()->getHolder();
        if (holder) {
            if ( std::find(differentKnobs.begin(), differentKnobs.end(), holder) == differentKnobs.end() ) {
                differentKnobs.push_back(holder);
                holder->beginChanges();
            }
        }
    }

    for (TransformKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        it->knob.lock()->cloneCurve(ViewSpec::all(), it->dimension, *it->oldCurve);
    }
    for (std::list<KnobHolder*>::iterator it = differentKnobs.begin(); it != differentKnobs.end(); ++it) {
        (*it)->endChanges();
    }

    view->setUpdatesEnabled(true);


    _model->refreshSelectionBboxAndRedrawView();
}

void
DSTransformKeysCommand::redo()
{
    /*
       The view is going to get MANY update() calls since every knob change will trigger a computeNodeRange() in DopeSheetView
       We explicitly disable update on the dopesheet view and re-enable it afterwards.
     */
    if (!knobsAreAlive()) {
        setObsolete(true);

        return;
    }
    DopeSheetView* view = _model->getDopesheetView();

    view->setUpdatesEnabled(false);

    std::list<KnobHolder*> differentKnobs;
    for (TransformKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        KnobHolder* holder = it->knob.lock()->getHolder();
        if (holder) {
            if ( std::find(differentKnobs.begin(), differentKnobs.end(), holder) == differentKnobs.end() ) {
                differentKnobs.push_back(holder);
                holder->beginChanges();
            }
        }
    }

    if (!_firstRedoCalled) {
        for (TransformKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
            it->oldCurve.reset(new Curve(*it->knob.lock()->getCurve(ViewIdx(0), it->dimension)));
        }
        for (TransformKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
            for (DopeSheetKeyPtrList::iterator it2 = it->keys.begin(); it2 != it->keys.end(); ++it2) {
                transformKey(*it2);
            }
        }

        for (TransformKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
            it->newCurve.reset(new Curve(*it->knob.lock()->getCurve(ViewIdx(0), it->dimension)));
        }
        _firstRedoCalled = true;
    } else {
        for (TransformKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
            it->knob.lock()->cloneCurve(ViewIdx(0), it->dimension, *it->newCurve);
        }
    }

    for (std::list<KnobHolder*>::iterator it = differentKnobs.begin(); it != differentKnobs.end(); ++it) {
        (*it)->endChanges();
    }

    view->setUpdatesEnabled(true);


    _model->refreshSelectionBboxAndRedrawView();
}

bool
DSTransformKeysCommand::knobsAreAlive() const
{
    for (TransformKeys::const_iterator it = _keys.begin(); it != _keys.end(); ++it) {
        if (!lockUndoKnob(it->knob)) {
            return false;
        }
    }

    return true;
}

void
DSTransformKeysCommand::transformKey(const DopeSheetKeyPtr& key)
{
    KnobIPtr knob = key->knob.lock();

    if (!knob) {
        return;
    }

    knob->transformValueAtTime(eCurveChangeReasonDopeSheet, key->key.getTime(), ViewSpec::all(), key->dimension, _transform, &key->key);
}

int
DSTransformKeysCommand::id() const
{
    return kDopeSheetEditorTransformKeysCommandCompressionID;
}

bool
DSTransformKeysCommand::mergeWith(const QUndoCommand *other)
{
    const DSTransformKeysCommand *cmd = dynamic_cast<const DSTransformKeysCommand *>(other);

    assert(cmd);
    if (!cmd || (cmd->id() != id()) || !inSameGestureAs(*cmd)) {
        return false;
    }

    if ( cmd->_keys.size() != _keys.size() ) {
        return false;
    }
    {
        TransformKeys::const_iterator itOther = cmd->_keys.begin();

        for (TransformKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
            if (itOther->keys.size() != it->keys.size()) {
                return false;
            }

            DopeSheetKeyPtrList::const_iterator kItOther = itOther->keys.begin();
            for (DopeSheetKeyPtrList::const_iterator it2 = it->keys.begin(); it2 != it->keys.end(); ++it2, ++kItOther) {
                if (*it2 != *kItOther) {
                    return false;
                }
            }
        }
    }

    _transform = Transform::matMul(_transform, cmd->_transform);

    return true;
}

////////////////////////// DSLeftTrimReaderCommand //////////////////////////

DSLeftTrimReaderCommand::DSLeftTrimReaderCommand(const DSNodePtr& reader,
                                                 double oldTime,
                                                 double newTime,
                                                 QUndoCommand* parent)
    : QUndoCommand(parent)
    , _reader(reader->getInternalNode())
    , _oldTime(oldTime)
    , _newTime(newTime)
{
    setText( tr("Trim left") );
}

void
DSLeftTrimReaderCommand::undo()
{
    trimLeft(_oldTime);
}

void
DSLeftTrimReaderCommand::redo()
{
    trimLeft(_newTime);
}

void
DSLeftTrimReaderCommand::trimLeft(double firstFrame)
{
    NodePtr node = _reader.lock();

    if (!node) {
        setObsolete(true);

        return;
    }

    KnobIntBase *firstFrameKnob = dynamic_cast<KnobIntBase *>( node->getKnobByName(kReaderParamNameFirstFrame).get() );
    assert(firstFrameKnob);
    if (!firstFrameKnob) {
        return;
    }
    KnobHolder *holder = firstFrameKnob->getHolder();
    EffectInstance *effectInstance = dynamic_cast<EffectInstance *>(holder);
    assert(effectInstance);
    if (!effectInstance) {
        return;
    }
    effectInstance->beginChanges();
    KnobHelper::ValueChangedReturnCodeEnum r = firstFrameKnob->setValue(firstFrame, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
    effectInstance->endChanges();

    Q_UNUSED(r);
}

int
DSLeftTrimReaderCommand::id() const
{
    return kDopeSheetEditorLeftTrimCommandCompressionID;
}

bool
DSLeftTrimReaderCommand::mergeWith(const QUndoCommand *other)
{
    const DSLeftTrimReaderCommand *cmd = dynamic_cast<const DSLeftTrimReaderCommand *>(other);

    assert(cmd);
    if (!cmd || (cmd->id() != id()) || !inSameGestureAs(*cmd)) {
        return false;
    }

    NodePtr node = _reader.lock();
    NodePtr otherNode = cmd->_reader.lock();
    if (!node || !otherNode) {
        return false;
    }

    if (node != otherNode) {
        return false;
    }

    _newTime = cmd->_oldTime;

    return true;
}

////////////////////////// DSRightTrimReaderCommand //////////////////////////

DSRightTrimReaderCommand::DSRightTrimReaderCommand(const DSNodePtr& reader,
                                                   double oldTime,
                                                   double newTime,
                                                   DopeSheetEditor* /*model*/,
                                                   QUndoCommand* parent)
    : QUndoCommand(parent)
    , _reader(reader->getInternalNode())
    , _oldTime(oldTime)
    , _newTime(newTime)
{
    setText( tr("Trim right") );
}

void
DSRightTrimReaderCommand::undo()
{
    trimRight(_oldTime);
}

void
DSRightTrimReaderCommand::redo()
{
    trimRight(_newTime);
}

void
DSRightTrimReaderCommand::trimRight(double lastFrame)
{
    NodePtr node = _reader.lock();

    if (!node) {
        setObsolete(true);

        return;
    }

    KnobIntBase *lastFrameKnob = dynamic_cast<KnobIntBase *>( node->getKnobByName(kReaderParamNameLastFrame).get() );
    assert(lastFrameKnob);
    if (!lastFrameKnob) {
        return;
    }
    KnobHolder *holder = lastFrameKnob->getHolder();
    EffectInstance *effectInstance = dynamic_cast<EffectInstance *>(holder);
    assert(effectInstance);
    if (!effectInstance) {
        return;
    }
    effectInstance->beginChanges();
    KnobHelper::ValueChangedReturnCodeEnum r = lastFrameKnob->setValue(lastFrame, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
    effectInstance->endChanges();

    Q_UNUSED(r);
}

int
DSRightTrimReaderCommand::id() const
{
    return kDopeSheetEditorRightTrimCommandCompressionID;
}

bool
DSRightTrimReaderCommand::mergeWith(const QUndoCommand *other)
{
    const DSRightTrimReaderCommand *cmd = dynamic_cast<const DSRightTrimReaderCommand *>(other);

    assert(cmd);
    if (!cmd || (cmd->id() != id()) || !inSameGestureAs(*cmd)) {
        return false;
    }

    NodePtr node = _reader.lock();
    NodePtr otherNode = cmd->_reader.lock();
    if (!node || !otherNode) {
        return false;
    }

    if (node != otherNode) {
        return false;
    }

    _newTime = cmd->_oldTime;

    return true;
}

////////////////////////// DSSlipReaderCommand //////////////////////////

DSSlipReaderCommand::DSSlipReaderCommand(const DSNodePtr& dsNodeReader,
                                         double dt,
                                         DopeSheetEditor* model,
                                         QUndoCommand* parent)
    : QUndoCommand(parent)
    , _reader(dsNodeReader->getInternalNode())
    , _dt(dt)
    , _model(model)
{
    setText( tr("Slip reader") );
}

void
DSSlipReaderCommand::undo()
{
    slipReader(-_dt);
}

void
DSSlipReaderCommand::redo()
{
    slipReader(_dt);
}

int
DSSlipReaderCommand::id() const
{
    return kDopeSheetEditorSlipReaderCommandCompressionID;
}

bool
DSSlipReaderCommand::mergeWith(const QUndoCommand *other)
{
    const DSSlipReaderCommand *cmd = dynamic_cast<const DSSlipReaderCommand *>(other);

    assert(cmd);
    if (!cmd || (cmd->id() != id()) || !inSameGestureAs(*cmd)) {
        return false;
    }

    NodePtr node = _reader.lock();
    NodePtr otherNode = cmd->_reader.lock();
    if (!node || !otherNode) {
        return false;
    }

    if (node != otherNode) {
        return false;
    }

    _dt += cmd->_dt;

    return true;
}

void
DSSlipReaderCommand::slipReader(double dt)
{
    NodePtr node = _reader.lock();

    if (!node) {
        setObsolete(true);

        return;
    }

    KnobIntBase *firstFrameKnob = dynamic_cast<KnobIntBase *>( node->getKnobByName(kReaderParamNameFirstFrame).get() );
    assert(firstFrameKnob);
    KnobIntBase *lastFrameKnob = dynamic_cast<KnobIntBase *>( node->getKnobByName(kReaderParamNameLastFrame).get() );
    assert(lastFrameKnob);
    KnobIntBase *timeOffsetKnob = dynamic_cast<KnobIntBase *>( node->getKnobByName(kReaderParamNameTimeOffset).get() );
    assert(timeOffsetKnob);
    KnobIntBase *startingTimeKnob = dynamic_cast<KnobIntBase *>( node->getKnobByName(kReaderParamNameStartingTime).get() );
    assert(startingTimeKnob);
    if (!firstFrameKnob || !lastFrameKnob || !timeOffsetKnob || !startingTimeKnob) {
        return;
    }


    /*
       Since the lastFrameKnob and startingTimeKnob have their signal connected to the computeNodeRange function in DopeSheetview
       We disconnect them beforehand and reconnect them afterwards, otherwise the dopesheet view is going to get many redraw() calls
       for nothing.
     */
    // The reader may not be shown in the dope sheet any more (its panel closed or rebuilt since),
    // so only the connections that existed are restored.
    DopeSheetView* view = _model->getDopesheetView();
    const bool lastFrameConnected = QObject::disconnect(lastFrameKnob->getSignalSlotHandler().get(), SIGNAL(valueChanged(ViewSpec, int, int)),
                                                        view, SLOT(onRangeNodeChanged(ViewSpec, int, int)));
    const bool startingTimeConnected = QObject::disconnect(startingTimeKnob->getSignalSlotHandler().get(), SIGNAL(valueChanged(ViewSpec, int, int)),
                                                           view, SLOT(onRangeNodeChanged(ViewSpec, int, int)));
    KnobHolder *holder = lastFrameKnob->getHolder();
    EffectInstance *effectInstance = dynamic_cast<EffectInstance *>(holder);
    assert(effectInstance);
    if (!effectInstance) {
        return;
    }
    effectInstance->beginChanges();
    {
        KnobHelper::ValueChangedReturnCodeEnum r;

        r = firstFrameKnob->setValue(firstFrameKnob->getValue() - dt, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
        Q_UNUSED(r);
        r = lastFrameKnob->setValue(lastFrameKnob->getValue() - dt, ViewSpec::all(),  0, eValueChangedReasonNatronGuiEdited, 0);
        Q_UNUSED(r);
        r = timeOffsetKnob->setValue(timeOffsetKnob->getValue() + dt, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
        Q_UNUSED(r);
    }
    effectInstance->endChanges();

    if (lastFrameConnected) {
        QObject::connect(lastFrameKnob->getSignalSlotHandler().get(), SIGNAL(valueChanged(ViewSpec, int, int)),
                         view, SLOT(onRangeNodeChanged(ViewSpec, int, int)));
    }
    if (startingTimeConnected) {
        QObject::connect(startingTimeKnob->getSignalSlotHandler().get(), SIGNAL(valueChanged(ViewSpec, int, int)),
                         view, SLOT(onRangeNodeChanged(ViewSpec, int, int)));
    }

    view->update();
} // DSSlipReaderCommand::slipReader

////////////////////////// DSRemoveKeysCommand //////////////////////////

DSRemoveKeysCommand::DSRemoveKeysCommand(const std::vector<DopeSheetKey> &keys,
                                         DopeSheetEditor *model,
                                         QUndoCommand *parent)
    : QUndoCommand(parent),
    _keys(keys),
    _model(model)
{
    setText( tr("Delete selected keyframes") );
}

void
DSRemoveKeysCommand::undo()
{
    addOrRemoveKeyframe(true);
}

void
DSRemoveKeysCommand::redo()
{
    addOrRemoveKeyframe(false);
}

void
DSRemoveKeysCommand::addOrRemoveKeyframe(bool add)
{
    for (std::vector<DopeSheetKey>::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        if (!lockUndoKnob(it->knob)) {
            setObsolete(true);

            return;
        }
    }

    std::set<KnobIPtr> knobsSet;
    for (std::vector<DopeSheetKey>::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        const DopeSheetKey& selected = (*it);
        KnobIPtr knob = selected.knob.lock();

        std::pair<std::set<KnobIPtr>::iterator, bool> ok = knobsSet.insert(knob);
        if (ok.second) {
            knob->beginChanges();
        }
        if (add) {
            KnobGui::setKnobKeyframe(knob, selected.key.getTime(), selected.key, selected.dimension, ViewIdx(0));
        } else {
            KnobGui::removeKnobKeyframe(knob, selected.key.getTime(), selected.dimension, ViewIdx(0));
            DSKnobPtr knobContext = selected.context.lock();
            if (knobContext) {
                knobContext->getTreeItem()->setSelected(false);
            }
        }
    }

    for (std::set<KnobIPtr>::iterator it = knobsSet.begin(); it != knobsSet.end(); ++it) {
        (*it)->endChanges();
    }

    _model->refreshSelectionBboxAndRedrawView();
}

////////////////////////// DSSetSelectedKeysInterpolationCommand //////////////////////////

DSSetSelectedKeysInterpolationCommand::DSSetSelectedKeysInterpolationCommand(const std::list<DSKeyInterpolationChange> &changes,
                                                                             DopeSheetEditor *model,
                                                                             QUndoCommand *parent)
    : QUndoCommand(parent),
    _changes(changes),
    _model(model)
{
    setText( tr("Set selected keys interpolation") );
}

void
DSSetSelectedKeysInterpolationCommand::undo()
{
    setInterpolation(true);
}

void
DSSetSelectedKeysInterpolationCommand::redo()
{
    setInterpolation(false);
}

void
DSSetSelectedKeysInterpolationCommand::setInterpolation(bool undo)
{
    for (std::list<DSKeyInterpolationChange>::iterator it = _changes.begin(); it != _changes.end(); ++it) {
        if (!lockUndoKnob(it->_key->knob)) {
            setObsolete(true);

            return;
        }
    }

    for (std::list<DSKeyInterpolationChange>::iterator it = _changes.begin(); it != _changes.end(); ++it) {
        KeyframeTypeEnum interp = undo ? it->_oldInterpType : it->_newInterpType;

        it->_key->knob.lock()->setInterpolationAtTime(eCurveChangeReasonDopeSheet,
                                                      ViewSpec::all(),
                                                      it->_key->dimension,
                                                      it->_key->key.getTime(),
                                                      interp,
                                                      &it->_key->key);
        // Only Free and Broken keys carry weights; setTangentsAtTime would turn any other key into one of them.
        if (undo && interpolationHasWeights(it->_oldInterpType)) {
            it->_key->knob.lock()->setTangentsAtTime(eCurveChangeReasonDopeSheet,
                                                     ViewSpec::all(),
                                                     it->_key->dimension,
                                                     it->_key->key.getTime(),
                                                     it->_oldTangents,
                                                     &it->_key->key);
        }
    }

    _model->refreshSelectionBboxAndRedrawView();
}

////////////////////////// DSAddKeysCommand //////////////////////////

DSPasteKeysCommand::DSPasteKeysCommand(const std::vector<DopeSheetKey> &keys,
                                       const std::list<DSKnobPtr>& dstKnobs,
                                       bool pasteRelativeToRefTime,
                                       DopeSheetEditor *model,
                                       QUndoCommand *parent)
    : QUndoCommand(parent),
    _refTime(0),
    _refKeyindex(-1),
    _pasteRelativeToRefTime(pasteRelativeToRefTime),
    _keys(),
    _model(model)
{

    for (std::list<DSKnobPtr>::const_iterator it = dstKnobs.begin(); it != dstKnobs.end(); ++it) {
        DstKnob dst;
        dst.knob = (*it)->getInternalKnob();
        dst.dimension = (*it)->getDimension();
        _dstKnobs.push_back(dst);
    }

    _refTime = _model->getTimelineCurrentTime();
    setText( tr("Paste keyframes") );
    for (std::size_t i = 0; i < keys.size(); ++i) {
        _keys.push_back(keys[i]);
        if (_refKeyindex == -1) {
            _refKeyindex = i;
        } else {
            if ( keys[i].key.getTime() < _keys[_refKeyindex].key.getTime() ) {
                _refKeyindex = i;
            }
        }
    }
}

void
DSPasteKeysCommand::undo()
{
    addOrRemoveKeyframe(false);
}

void
DSPasteKeysCommand::redo()
{
    addOrRemoveKeyframe(true);
}

void
DSPasteKeysCommand::setKeyValueFromKnob(const KnobIPtr& knob, double keyTime, KeyFrame* key)
{
    KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>( knob.get() );
    KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>( knob.get() );
    KnobIntBase* isInt = dynamic_cast<KnobIntBase*>( knob.get() );
    KnobStringBase* isString = dynamic_cast<KnobStringBase*>( knob.get() );


    if (isDouble) {
        key->setValue( isDouble->getValueAtTime(keyTime) );
    } else if (isBool) {
        key->setValue( isBool->getValueAtTime(keyTime) );
    } else if (isInt) {
        key->setValue( isInt->getValueAtTime(keyTime) );
    } else if (isString) {
        std::string v = isString->getValueAtTime(keyTime);
        double keyFrameValue = 0.;
        AnimatingKnobStringHelper* isStringAnimatedKnob = dynamic_cast<AnimatingKnobStringHelper*>(isString);
        assert(isStringAnimatedKnob);
        if (isStringAnimatedKnob) {
            isStringAnimatedKnob->stringToKeyFrameValue(keyTime, ViewIdx(0), v, &keyFrameValue);
        }
        key->setValue(keyFrameValue);
    }
}

void
DSPasteKeysCommand::addOrRemoveKeyframe(bool add)
{
    for (std::list<DstKnob>::const_iterator it = _dstKnobs.begin(); it != _dstKnobs.end(); ++it) {
        if (!lockUndoKnob(it->knob)) {
            setObsolete(true);

            return;
        }
    }

    for (std::list<DstKnob>::const_iterator it = _dstKnobs.begin(); it != _dstKnobs.end(); ++it) {
        KnobIPtr knob = it->knob.lock();
        const int dim = it->dimension;
        for (std::size_t i = 0; i < _keys.size(); ++i) {
            knob->beginChanges();

            double keyTime = _keys[i].key.getTime();
            double setTime = _pasteRelativeToRefTime ? keyTime - _keys[_refKeyindex].key.getTime() + _refTime : keyTime;

            if (add) {

                for (int j = 0; j < knob->getDimension(); ++j) {
                    if ( (dim == -1) || (j == dim) ) {
                        KeyFrame k = _keys[i].key;
                        k.setTime(setTime);

                        knob->setKeyFrame(k, ViewSpec::all(), j, eValueChangedReasonNatronGuiEdited);
                    }
                }
            } else {
                for (int j = 0; j < knob->getDimension(); ++j) {
                    if ( (dim == -1) || (j == dim) ) {
                        knob->deleteValueAtTime(eCurveChangeReasonDopeSheet, setTime, ViewSpec::all(), j, i == 0);
                    }
                }
            }
            
            knob->endChanges();
        }
    }

    _model->refreshSelectionBboxAndRedrawView();
} // DSPasteKeysCommand::addOrRemoveKeyframe

NATRON_NAMESPACE_EXIT
