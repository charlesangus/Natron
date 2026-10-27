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

#include <stdexcept>

#include <QDebug>

#include "Engine/Bezier.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/RotoCurveContext.h"
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

void
moveTimeOffset(const NodePtr& node,
               double dt)
{
    KnobIntBase* timeOffsetKnob = dynamic_cast<KnobIntBase*>( node->getKnobByName(kTimeOffsetParamNameTimeOffset).get() );
    assert(timeOffsetKnob);
    KnobHelper::ValueChangedReturnCodeEnum s = timeOffsetKnob->setValue(timeOffsetKnob->getValue() + dt, ViewSpec::all(), 0, eValueChangedReasonNatronGuiEdited, 0);
    Q_UNUSED(s);
}

void
moveFrameRange(const NodePtr& node,
               double dt)
{
    KnobIntBase* frameRangeKnob = dynamic_cast<KnobIntBase*>( node->getKnobByName(kFrameRangeParamNameFrameRange).get() );
    assert(frameRangeKnob);
    frameRangeKnob->setValues(frameRangeKnob->getValue() + dt, frameRangeKnob->getValue(1)  + dt, ViewSpec::all(), eValueChangedReasonNatronGuiEdited);
}

void
moveGroupNode(DopeSheetEditor* model,
              const NodePtr& node,
              double dt)
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
        if (pluginID == PLUGINID_NATRON_READ) {
            moveReader(*it, dt);
        } else if (pluginID == PLUGINID_OFX_TIMEOFFSET) {
            moveTimeOffset(*it, dt);
        } else if (pluginID == PLUGINID_OFX_FRAMERANGE) {
            moveFrameRange(*it, dt);
        } else if (isChildGroup) {
            moveGroupNode(model, *it, dt);
        }

        // Move keyframes
        const KnobsVec &knobs = (*it)->getKnobs();

        for (KnobsVec::const_iterator knobIt = knobs.begin(); knobIt != knobs.end(); ++knobIt) {
            const KnobIPtr& knob = *knobIt;
            if ( !knob->hasAnimation() ) {
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

/**
 * @brief Moves a shape's keys from the first to the second time of each pair. The whole shape
 * is refused when a key would land on a key that is not moving, two keys would land together,
 * or the keys would have to pass through each other, because Bezier::moveKeyframe overwrites
 * whatever key sits at the target time.
 **/
bool
moveShapeKeyframes(const BezierPtr& bezier,
                   const std::vector<std::pair<double, double>>& moves)
{
    std::set<double> existing;
    bezier->getKeyframeTimes(&existing);

    std::set<double> sources;
    std::set<double> targets;
    for (std::size_t i = 0; i < moves.size(); ++i) {
        sources.insert(moves[i].first);
        if (!targets.insert(moves[i].second).second) {
            return false;
        }
    }
    for (std::size_t i = 0; i < moves.size(); ++i) {
        if (existing.count(moves[i].second) && !sources.count(moves[i].second)) {
            return false;
        }
    }

    // A key can move once no pending key still sits at its target.
    std::list<std::pair<double, double>> pending;
    for (std::size_t i = 0; i < moves.size(); ++i) {
        if (moves[i].first != moves[i].second) {
            pending.push_back(moves[i]);
        }
    }
    std::vector<std::pair<double, double>> ordered;
    while (!pending.empty()) {
        std::list<std::pair<double, double>>::iterator next = pending.begin();
        for (; next != pending.end(); ++next) {
            bool blocked = false;
            for (std::list<std::pair<double, double>>::iterator other = pending.begin(); other != pending.end(); ++other) {
                if ((other != next) && (other->first == next->second)) {
                    blocked = true;
                    break;
                }
            }
            if (!blocked) {
                break;
            }
        }
        if (next == pending.end()) {
            return false;
        }
        ordered.push_back(*next);
        pending.erase(next);
    }

    std::list<std::pair<double, KeyframeTypeEnum>> interpolations;
    bezier->getKeyframeTimesAndInterpolation(&interpolations);
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        KeyframeTypeEnum interp = eKeyframeTypeSmooth;
        bool hasInterp = false;
        for (std::list<std::pair<double, KeyframeTypeEnum>>::iterator it = interpolations.begin(); it != interpolations.end(); ++it) {
            if (it->first == ordered[i].first) {
                interp = it->second;
                hasInterp = true;
                break;
            }
        }
        bezier->moveKeyframe(ordered[i].first, ordered[i].second);
        // moveKeyframe re-creates the key with the default interpolation.
        if (hasInterp) {
            int index = bezier->getKeyFrameIndex(ordered[i].second);
            if (index >= 0) {
                bezier->setKeyFrameInterpolation(interp, index);
            }
        }
    }

    return true;
} // moveShapeKeyframes

/**
 * @brief Moves the shape keys in 'keys' to the times 'targetTime' gives, shape by shape,
 * recording each shape's prior state in 'states' and each key's times in 'times'.
 **/
template <typename TargetTime>
void
applyShapeKeyMoves(const DopeSheetKeyPtrList& keys,
                   TargetTime targetTime,
                   DSShapeKeysStates* states,
                   std::list<DSShapeKeyTimes>* times)
{
    std::map<BezierPtr, std::list<DopeSheetKeyPtr>> byShape;
    for (DopeSheetKeyPtrList::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        BezierPtr bezier = (*it)->bezier.lock();
        if (bezier) {
            byShape[bezier].push_back(*it);
        }
    }

    times->clear();
    for (std::map<BezierPtr, std::list<DopeSheetKeyPtr>>::iterator it = byShape.begin(); it != byShape.end(); ++it) {
        states->recordBefore(it->first);

        std::vector<std::pair<double, double>> moves;
        for (std::list<DopeSheetKeyPtr>::iterator k = it->second.begin(); k != it->second.end(); ++k) {
            double time = (*k)->key.getTime();
            moves.push_back(std::make_pair(time, targetTime(time)));
        }
        bool moved = moveShapeKeyframes(it->first, moves);

        std::size_t i = 0;
        for (std::list<DopeSheetKeyPtr>::iterator k = it->second.begin(); k != it->second.end(); ++k, ++i) {
            DSShapeKeyTimes keyTimes;
            keyTimes.key = *k;
            keyTimes.before = moves[i].first;
            keyTimes.after = moved ? moves[i].second : moves[i].first;
            (*k)->key.setTime(keyTimes.after);
            times->push_back(keyTimes);
        }
        if (moved) {
            it->first->getContext()->evaluateChange();
        }
    }
}

void
setShapeKeyTimes(const std::list<DSShapeKeyTimes>& times,
                 bool after)
{
    for (std::list<DSShapeKeyTimes>::const_iterator it = times.begin(); it != times.end(); ++it) {
        it->key->key.setTime(after ? it->after : it->before);
    }
}

void
recordShapeKeyTimesAfter(std::list<DSShapeKeyTimes>* times)
{
    for (std::list<DSShapeKeyTimes>::iterator it = times->begin(); it != times->end(); ++it) {
        it->after = it->key->key.getTime();
    }
}

bool
shapeKeysAlive(const DopeSheetKeyPtrList& keys)
{
    for (DopeSheetKeyPtrList::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        if ((*it)->bezier.expired()) {
            return false;
        }
    }

    return true;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

////////////////////////// DSShapeKeysStates //////////////////////////

void
DSShapeKeysStates::recordBefore(const BezierPtr& bezier)
{
    for (std::list<State>::iterator it = _states.begin(); it != _states.end(); ++it) {
        if (it->bezier.lock() == bezier) {
            return;
        }
    }
    State state;
    state.bezier = bezier;
    state.before.reset(new Bezier(bezier->getContext(), bezier->getScriptName(), bezier->getParentLayer(), false));
    state.before->clone(bezier.get());
    _states.push_back(state);
}

void
DSShapeKeysStates::recordAfter()
{
    for (std::list<State>::iterator it = _states.begin(); it != _states.end(); ++it) {
        BezierPtr bezier = it->bezier.lock();
        if (!bezier) {
            continue;
        }
        it->after.reset(new Bezier(bezier->getContext(), bezier->getScriptName(), bezier->getParentLayer(), false));
        it->after->clone(bezier.get());
    }
}

void
DSShapeKeysStates::restore(bool after)
{
    for (std::list<State>::iterator it = _states.begin(); it != _states.end(); ++it) {
        BezierPtr bezier = it->bezier.lock();
        BezierPtr state = after ? it->after : it->before;
        if (!bezier || !state) {
            continue;
        }
        bezier->clone(state.get());
        bezier->getContext()->evaluateChange();
    }
}

bool
DSShapeKeysStates::allAlive() const
{
    for (std::list<State>::const_iterator it = _states.begin(); it != _states.end(); ++it) {
        if (it->bezier.expired()) {
            return false;
        }
    }

    return true;
}

////////////////////////// DSMoveKeysCommand //////////////////////////

DSMoveKeysAndNodesCommand::DSMoveKeysAndNodesCommand(const DopeSheetKeyPtrList& keys,
                                                     const std::vector<DSNodePtr>& nodes,
                                                     double dt,
                                                     DopeSheetEditor* model,
                                                     QUndoCommand* parent)
    : QUndoCommand(parent)
    , _keys()
    , _shapeKeys()
    , _shapeKeyTimes()
    , _shapeStates()
    , _firstRedoCalled(false)
    , _nodes()
    , _dt(dt)
    , _model(model)
{
    setText( tr("Move selected keys") );
    for (DopeSheetKeyPtrList::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        if (!(*it)->bezier.expired()) {
            _shapeKeys.push_back(*it);
        } else {
            _keys.push_back(*it);
        }
    }
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
    for (DopeSheetKeyPtrList::iterator it = _shapeKeys.begin(); it != _shapeKeys.end(); ++it) {
        BezierPtr bezier = (*it)->bezier.lock();
        NodePtr host = bezier ? bezier->getContext()->getNode() : NodePtr();
        if (host) {
            nodesSet.insert(host);
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
    if (isObsolete() || _shapeKeys.empty()) {
        return;
    }
    _shapeStates.restore(false);
    setShapeKeyTimes(_shapeKeyTimes, false);
    _model->refreshSelectionBboxAndRedrawView();
}

void
DSMoveKeysAndNodesCommand::redo()
{
    moveSelection(_dt);
    if (isObsolete() || _shapeKeys.empty()) {
        return;
    }
    if (!_firstRedoCalled) {
        moveShapeKeys(_dt);
        _shapeStates.recordAfter();
        _firstRedoCalled = true;
    } else {
        _shapeStates.restore(true);
        setShapeKeyTimes(_shapeKeyTimes, true);
    }
    _model->refreshSelectionBboxAndRedrawView();
}

void
DSMoveKeysAndNodesCommand::moveShapeKeys(double dt)
{
    applyShapeKeyMoves(_shapeKeys, [dt](double time) { return time + dt; }, &_shapeStates, &_shapeKeyTimes);
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
    if (!shapeKeysAlive(_shapeKeys)) {
        setObsolete(true);

        return;
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


    //////////Handle selected keyframes
    for (DopeSheetKeyPtrList::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        const DopeSheetKeyPtr& selectedKey = (*it);
        KnobIPtr knob = selectedKey->knob.lock();

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
            moveTimeOffset(node, dt);
        } else if (it->type == eDopeSheetItemTypeGroup) {
            moveGroupNode(_model, node, dt);
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

    if (cmd->_shapeKeys.size() != _shapeKeys.size()) {
        return false;
    }
    {
        DopeSheetKeyPtrList::const_iterator itOther = cmd->_shapeKeys.begin();

        for (DopeSheetKeyPtrList::const_iterator it = _shapeKeys.begin(); it != _shapeKeys.end(); ++it, ++itOther) {
            if (*itOther != *it) {
                return false;
            }
        }
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
    // The merged command has already been applied, so this one's edited state is the current one.
    _shapeStates.recordAfter();
    recordShapeKeyTimesAfter(&_shapeKeyTimes);

    return true;
}

//////////////////////////DSTransformKeysCommand //////////////////////////

DSTransformKeysCommand::DSTransformKeysCommand(const DopeSheetKeyPtrList& keys,
                                               const Transform::Matrix3x3& transform,
                                               DopeSheetEditor* model,
                                               QUndoCommand* parent)
    : QUndoCommand(parent)
    , _firstRedoCalled(false)
    , _transform(transform)
    , _keys()
    , _shapeKeys()
    , _shapeKeyTimes()
    , _shapeStates()
    , _model(model)
{
    for (DopeSheetKeyPtrList::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        if (!(*it)->bezier.expired()) {
            _shapeKeys.push_back(*it);
            continue;
        }
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
    _shapeStates.restore(false);
    setShapeKeyTimes(_shapeKeyTimes, false);
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
        transformShapeKeys();
        _shapeStates.recordAfter();
        _firstRedoCalled = true;
    } else {
        for (TransformKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
            it->knob.lock()->cloneCurve(ViewIdx(0), it->dimension, *it->newCurve);
        }
        _shapeStates.restore(true);
        setShapeKeyTimes(_shapeKeyTimes, true);
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

    return shapeKeysAlive(_shapeKeys);
}

void
DSTransformKeysCommand::transformShapeKeys()
{
    const Transform::Matrix3x3 transform = _transform;

    applyShapeKeyMoves(_shapeKeys, [transform](double time) {
        Transform::Point3D p;
        p.x = time;
        p.y = 0;
        p.z = 1;

        return Transform::matApply(transform, p).x; }, &_shapeStates, &_shapeKeyTimes);
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
    if (cmd->_shapeKeys.size() != _shapeKeys.size()) {
        return false;
    }
    {
        DopeSheetKeyPtrList::const_iterator itOther = cmd->_shapeKeys.begin();

        for (DopeSheetKeyPtrList::const_iterator it = _shapeKeys.begin(); it != _shapeKeys.end(); ++it, ++itOther) {
            if (*itOther != *it) {
                return false;
            }
        }
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
    // The merged command has already been applied, so this one's edited state is the current one.
    _shapeStates.recordAfter();
    recordShapeKeyTimesAfter(&_shapeKeyTimes);

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

DSRemoveKeysCommand::DSRemoveKeysCommand(const std::vector<DopeSheetKey>& keys,
                                         DopeSheetEditor* model,
                                         QUndoCommand* parent)
    : QUndoCommand(parent)
    , _keys()
    , _shapeKeys()
    , _shapeStates()
    , _firstRedoCalled(false)
    , _model(model)
{
    for (std::vector<DopeSheetKey>::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        if (!it->bezier.expired()) {
            _shapeKeys.push_back(*it);
        } else {
            _keys.push_back(*it);
        }
    }
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
    for (std::vector<DopeSheetKey>::iterator it = _shapeKeys.begin(); it != _shapeKeys.end(); ++it) {
        if (it->bezier.expired()) {
            setObsolete(true);

            return;
        }
    }

    if (add) {
        _shapeStates.restore(false);
    } else if (!_firstRedoCalled) {
        for (std::vector<DopeSheetKey>::iterator it = _shapeKeys.begin(); it != _shapeKeys.end(); ++it) {
            BezierPtr bezier = it->bezier.lock();
            _shapeStates.recordBefore(bezier);
            bezier->removeKeyframe(it->key.getTime());
        }
        _shapeStates.recordAfter();
        _firstRedoCalled = true;
    } else {
        _shapeStates.restore(true);
    }
    if (!add) {
        for (std::vector<DopeSheetKey>::iterator it = _shapeKeys.begin(); it != _shapeKeys.end(); ++it) {
            DSKnobPtr knobContext = it->context.lock();
            if (knobContext) {
                knobContext->getTreeItem()->setSelected(false);
            }
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

DSSetSelectedKeysInterpolationCommand::DSSetSelectedKeysInterpolationCommand(const std::list<DSKeyInterpolationChange>& changes,
                                                                             DopeSheetEditor* model,
                                                                             QUndoCommand* parent)
    : QUndoCommand(parent)
    , _changes()
    , _shapeChanges()
    , _shapeStates()
    , _firstRedoCalled(false)
    , _model(model)
{
    for (std::list<DSKeyInterpolationChange>::const_iterator it = changes.begin(); it != changes.end(); ++it) {
        if (!it->_key->bezier.expired()) {
            _shapeChanges.push_back(*it);
        } else {
            _changes.push_back(*it);
        }
    }
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
    for (std::list<DSKeyInterpolationChange>::iterator it = _shapeChanges.begin(); it != _shapeChanges.end(); ++it) {
        if (it->_key->bezier.expired()) {
            setObsolete(true);

            return;
        }
    }

    if (undo) {
        _shapeStates.restore(false);
    } else if (!_firstRedoCalled) {
        std::set<BezierPtr> edited;
        for (std::list<DSKeyInterpolationChange>::iterator it = _shapeChanges.begin(); it != _shapeChanges.end(); ++it) {
            BezierPtr bezier = it->_key->bezier.lock();
            _shapeStates.recordBefore(bezier);
            int index = bezier->getKeyFrameIndex(it->_key->key.getTime());
            if (index >= 0) {
                bezier->setKeyFrameInterpolation(it->_newInterpType, index);
                edited.insert(bezier);
            }
        }
        // setKeyFrameInterpolation neither re-renders nor signals.
        for (std::set<BezierPtr>::iterator it = edited.begin(); it != edited.end(); ++it) {
            (*it)->getContext()->evaluateChange();
        }
        _shapeStates.recordAfter();
        _firstRedoCalled = true;
    } else {
        _shapeStates.restore(true);
    }
    for (std::list<DSKeyInterpolationChange>::iterator it = _shapeChanges.begin(); it != _shapeChanges.end(); ++it) {
        it->_key->key.setInterpolation(undo ? it->_oldInterpType : it->_newInterpType);
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

    // Shape keys are not pasted: they carry no values, and shape rows have no knob to paste onto.
    for (std::list<DSKnobPtr>::const_iterator it = dstKnobs.begin(); it != dstKnobs.end(); ++it) {
        if (!(*it)->getInternalKnob()) {
            continue;
        }
        DstKnob dst;
        dst.knob = (*it)->getInternalKnob();
        dst.dimension = (*it)->getDimension();
        _dstKnobs.push_back(dst);
    }

    _refTime = _model->getTimelineCurrentTime();
    setText( tr("Paste keyframes") );
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (!keys[i].bezier.expired()) {
            continue;
        }
        _keys.push_back(keys[i]);
        if (_refKeyindex == -1) {
            _refKeyindex = (int)_keys.size() - 1;
        } else if (keys[i].key.getTime() < _keys[_refKeyindex].key.getTime()) {
            _refKeyindex = (int)_keys.size() - 1;
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
