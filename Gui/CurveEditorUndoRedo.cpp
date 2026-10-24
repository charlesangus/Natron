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

#include "CurveEditorUndoRedo.h"

#include <cmath>
#include <stdexcept>

#include <QDebug>

#include "Global/GlobalDefines.h"

#include "Gui/CurveEditor.h"
#include "Gui/CurveGui.h"
#include "Gui/CurveWidget.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobGuiValue.h"

#include "Engine/Bezier.h"
#include "Engine/Knob.h"
#include "Engine/Curve.h"
#include "Engine/RotoContext.h"
#include "Engine/KnobTypes.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

//////////////////////////////ADD MULTIPLE KEYS COMMAND//////////////////////////////////////////////
AddKeysCommand::AddKeysCommand(CurveWidget* editor,
                               const KeysToAddList& keys,
                               QUndoCommand* parent)
    : CurveEditorCommand(parent)
    , _keys(keys)
    , _curveWidget(editor)
{
}

AddKeysCommand::AddKeysCommand(CurveWidget* editor,
                               const CurveGuiPtr& curve,
                               const std::vector<KeyFrame>& keys,
                               QUndoCommand* parent)
    : CurveEditorCommand(parent)
    , _keys()
    , _curveWidget(editor)
{
    KeyToAdd k;

    k.curveUI = curve;
    KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>( curve.get() );
    KnobGuiPtr guiKnob;
    if (isKnobCurve) {
        guiKnob = isKnobCurve->getKnobGui();
        k.dimension = isKnobCurve->getDimension();
    } else {
        k.dimension = 0;
    }
    k.knobUI = guiKnob;
    k.keyframes = keys;
    _keys.push_back(k);

    setText( tr("Add multiple keyframes") );
}

void
AddKeysCommand::addOrRemoveKeyframe(bool isSetKeyCommand,
                                    bool add)
{
    for (KeysToAddList::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>( it->curveUI.get() );
        BezierCPCurveGui* isBezierCurve = dynamic_cast<BezierCPCurveGui*>( it->curveUI.get() );
        KnobGuiPtr guiKnob = it->knobUI.lock();
        KnobIPtr knob;

        if (guiKnob) {
            knob = guiKnob->getKnob();
        } else if (isKnobCurve) {
            knob = isKnobCurve->getInternalKnob();
        }
        KnobParametricPtr isParametric;
        if (knob) {
            isParametric = std::dynamic_pointer_cast<KnobParametric>(knob);
        }

        if (add && isSetKeyCommand) {
            if (isKnobCurve) {
                if (isParametric) {
                    StatusEnum st = isParametric->deleteAllControlPoints( eValueChangedReasonUserEdited, isKnobCurve->getDimension() );
                    assert(st == eStatusOK);
                    if (st != eStatusOK) {
                        throw std::logic_error("addOrRemoveKeyframe");
                    }
                } else {
                    knob->removeAnimation( ViewIdx(0), isKnobCurve->getDimension() );
                }
            } else if (isBezierCurve) {
                BezierPtr b = isBezierCurve->getBezier();
                assert(b);
                if (b) {
                    b->removeAnimation();
                }
            }
        }

        if (guiKnob && !isParametric) {
            if (isKnobCurve) {
                if (add) {
                    guiKnob->setKeyframes( it->keyframes, isKnobCurve->getDimension(), ViewIdx(0) );
                } else {
                    guiKnob->removeKeyframes( it->keyframes, isKnobCurve->getDimension(), ViewIdx(0) );
                }
            }
        } else {
            for (std::size_t i = 0; i < it->keyframes.size(); ++i) {
                if (knob) {
                    knob->beginChanges();

                    if (add) {
                        int time = it->keyframes[i].getTime();
                        if (isParametric) {
                            // keyframes added to parametric params are Cubic by default
                            StatusEnum st = isParametric->addControlPoint( eValueChangedReasonUserEdited, it->dimension, it->keyframes[i].getTime(), it->keyframes[i].getValue(), eKeyframeTypeCubic );
                            assert(st == eStatusOK);
                            Q_UNUSED(st);
                        } else {
                            KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>( knob.get() );
                            KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>( knob.get() );
                            KnobIntBase* isInt = dynamic_cast<KnobIntBase*>( knob.get() );
                            KnobStringBase* isString = dynamic_cast<KnobStringBase*>( knob.get() );
                            if (isDouble) {
                                isDouble->setValueAtTime( time, isDouble->getValueAtTime( time, 0, ViewIdx(0) ), ViewIdx(0), isKnobCurve->getDimension() );
                            } else if (isBool) {
                                isBool->setValueAtTime( time,   isBool->getValueAtTime( time, 0, ViewIdx(0) ),   ViewIdx(0), isKnobCurve->getDimension() );
                            } else if (isInt) {
                                isInt->setValueAtTime( time,    isInt->getValueAtTime( time, 0, ViewIdx(0) ),    ViewIdx(0), isKnobCurve->getDimension() );
                            } else if (isString) {
                                isString->setValueAtTime( time, isString->getValueAtTime( time, 0, ViewIdx(0) ), ViewIdx(0), isKnobCurve->getDimension() );
                            }
                        }
                    } else {
                        KnobParametricPtr parametricKnob = std::dynamic_pointer_cast<KnobParametric>( isKnobCurve->getInternalKnob() );

                        if (parametricKnob) {
                            StatusEnum st = parametricKnob->deleteControlPoint( eValueChangedReasonUserEdited, it->dimension,
                                                                                it->curveUI->getInternalCurve()->keyFrameIndex( it->keyframes[i].getTime() ) );
                            assert(st == eStatusOK);
                            Q_UNUSED(st);
                        } else {
                            isKnobCurve->getInternalKnob()->deleteValueAtTime( eCurveChangeReasonCurveEditor, it->keyframes[i].getTime(), ViewIdx(0),  isKnobCurve->getDimension(), i == 0 );
                        }
                    }
                } else if (isBezierCurve) {
                    if (add) {
                        isBezierCurve->getBezier()->setKeyframe( it->keyframes[i].getTime() );
                    } else {
                        isBezierCurve->getBezier()->removeKeyframe( it->keyframes[i].getTime() );
                    }
                } // if (isKnobCurve) {
            } // for (std::size_t i = 0; i < it->second.size(); ++i) {
        } // if (guiKnob) {
        if (knob) {
            knob->endChanges();
        }
    }

    _curveWidget->update();
} // addOrRemoveKeyframe

void
AddKeysCommand::undo()
{
    addOrRemoveKeyframe(false, false);
}

void
AddKeysCommand::redo()
{
    addOrRemoveKeyframe(false, true);
}

void
AddKeysCommand::getEditedCurves(std::list<CurveGuiPtr>* curves) const
{
    for (KeysToAddList::const_iterator it = _keys.begin(); it != _keys.end(); ++it) {
        curves->push_back(it->curveUI);
    }
}

SetKeysCommand::SetKeysCommand(CurveWidget *editor,
                               const AddKeysCommand::KeysToAddList & keys,
                               QUndoCommand *parent)
    : AddKeysCommand(editor, keys, parent)
{
}

SetKeysCommand::SetKeysCommand(CurveWidget *editor,
                               const CurveGuiPtr& curve,
                               const std::vector<KeyFrame> & keys,
                               QUndoCommand *parent)
    : AddKeysCommand(editor, curve, keys, parent)
    , _guiCurve(curve)
    , _oldCurve()
{
    CurvePtr internalCurve = curve->getInternalCurve();

    assert(internalCurve);
    _oldCurve.reset( new Curve(*internalCurve) );
}

void
SetKeysCommand::undo()
{
    KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>( _guiCurve.get() );
    BezierCPCurveGui* isBezierCurve = dynamic_cast<BezierCPCurveGui*>( _guiCurve.get() );

    if (isKnobCurve) {
        KnobIPtr knob = isKnobCurve->getInternalKnob();
        KnobParametricPtr isParametric = std::dynamic_pointer_cast<KnobParametric>(knob);
        if (!isParametric) {
            knob->cloneCurve(ViewSpec::all(), isKnobCurve->getDimension(), *_oldCurve);
        } else {
            _guiCurve->getInternalCurve()->clone(*_oldCurve);
        }
    } else if (isBezierCurve) {
        // not implemented
    }
}

void
SetKeysCommand::redo()
{
    addOrRemoveKeyframe(true, true);
}

//////////////////////////////REMOVE  MULTIPLE KEYS COMMAND//////////////////////////////////////////////
RemoveKeysCommand::RemoveKeysCommand(CurveWidget* editor,
                                     const std::map<CurveGuiPtr, std::vector<KeyFrame>>& keys,
                                     QUndoCommand* parent)
    : CurveEditorCommand(parent)
    , _keys(keys)
    , _curveWidget(editor)
{
    setText( tr("Remove multiple keyframes") );
}

void
RemoveKeysCommand::addOrRemoveKeyframe(bool add)
{
    std::set<KnobIPtr> knobsSet;
    for (std::map<CurveGuiPtr, std::vector<KeyFrame> >::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>( it->first.get() );
        BezierCPCurveGui* isBezierCurve = dynamic_cast<BezierCPCurveGui*>( it->first.get() );
        KnobGuiPtr guiKnob;
        if (isKnobCurve) {
            guiKnob = isKnobCurve->getKnobGui();
        }

        KnobIPtr knob;

        if (isKnobCurve) {
            knob = isKnobCurve->getInternalKnob();
        }
        KnobParametricPtr isParametric;

        std::pair<std::set<KnobIPtr>::iterator, bool> insertOk;
        if (knob) {
            insertOk = knobsSet.insert(knob);
            if (insertOk.second) {
                knob->beginChanges();
                knob->blockValueChanges();
            }
            isParametric = std::dynamic_pointer_cast<KnobParametric>(knob);
        }

        if (guiKnob && isKnobCurve && !isParametric) {
            if (add) {
                guiKnob->setKeyframes( it->second, isKnobCurve->getDimension(), ViewIdx(0) );
            } else {
                guiKnob->removeKeyframes( it->second, isKnobCurve->getDimension(), ViewIdx(0) );
            }
        } else {
            for (std::size_t i = 0; i < it->second.size(); ++i) {
                if (isKnobCurve) {

                    if (add) {
                        int time = it->second[i].getTime();

                        if (isParametric) {
                            StatusEnum st = isParametric->addControlPoint( eValueChangedReasonUserEdited, isKnobCurve->getDimension(), it->second[i].getTime(), it->second[i].getValue() );
                            Q_UNUSED(st);
                        } else {
                            KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>( knob.get() );
                            KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>( knob.get() );
                            KnobIntBase* isInt = dynamic_cast<KnobIntBase*>( knob.get() );
                            KnobStringBase* isString = dynamic_cast<KnobStringBase*>( knob.get() );
                            if (isDouble) {
                                isDouble->setValueAtTime( time, isDouble->getValueAtTime( time, 0, ViewIdx(0) ), ViewIdx(0), isKnobCurve->getDimension() );
                            } else if (isBool) {
                                isBool->setValueAtTime( time,   isBool->getValueAtTime( time,   0, ViewIdx(0) ), ViewIdx(0), isKnobCurve->getDimension() );
                            } else if (isInt) {
                                isInt->setValueAtTime( time,    isInt->getValueAtTime( time,    0, ViewIdx(0) ), ViewIdx(0), isKnobCurve->getDimension() );
                            } else if (isString) {
                                isString->setValueAtTime( time, isString->getValueAtTime( time, 0, ViewIdx(0) ), ViewIdx(0), isKnobCurve->getDimension() );
                            }
                        }
                    } else {
                        KnobParametricPtr knob = std::dynamic_pointer_cast<KnobParametric>( isKnobCurve->getInternalKnob() );

                        if (knob) {
                            StatusEnum st = knob->deleteControlPoint( eValueChangedReasonUserEdited, isKnobCurve->getDimension(),
                                                                      it->first->getInternalCurve()->keyFrameIndex( it->second[i].getTime() ) );
                            Q_UNUSED(st);
                        } else {
                            isKnobCurve->getInternalKnob()->deleteValueAtTime( eCurveChangeReasonCurveEditor, it->second[i].getTime(), ViewSpec::all(), isKnobCurve->getDimension(), i == 0 );
                        }
                    }
                } else if (isBezierCurve) {
                    BezierPtr b = isBezierCurve->getBezier();
                    assert(b);
                    if (add) {
                        b->setKeyframe( it->second[i].getTime() );
                    } else {
                        b->removeKeyframe( it->second[i].getTime() );
                    }
                } // if (isKnobCurve) {
            } // for (std::size_t i = 0; i < it->second.size(); ++i) {
        } // if (guiKnob) {
 
    }

    for (std::set<KnobIPtr>::iterator it = knobsSet.begin(); it!=knobsSet.end(); ++it) {
        (*it)->unblockValueChanges();
        (*it)->evaluateValueChange(0, (*it)->getCurrentTime(), ViewSpec(0), eValueChangedReasonUserEdited);
        (*it)->endChanges();
    }

    _curveWidget->update();
} // RemoveKeysCommand::addOrRemoveKeyframe

void
RemoveKeysCommand::undo()
{
    addOrRemoveKeyframe(true);
}

void
RemoveKeysCommand::redo()
{
    addOrRemoveKeyframe(false);
}

void
RemoveKeysCommand::getEditedCurves(std::list<CurveGuiPtr>* curves) const
{
    for (std::map<CurveGuiPtr, std::vector<KeyFrame>>::const_iterator it = _keys.begin(); it != _keys.end(); ++it) {
        curves->push_back(it->first);
    }
}

//////////////////////////////MOVE MULTIPLE KEYS COMMAND//////////////////////////////////////////////
MoveKeysCommand::MoveKeysCommand(CurveWidget* widget,
                                 const std::map<CurveGuiPtr, std::vector<MoveKeysCommand::KeyToMove>>& keys,
                                 double dt,
                                 double dv,
                                 bool updateOnFirstRedo,
                                 QUndoCommand* parent)
    : CurveEditorCommand(parent)
    , _firstRedoCalled(false)
    , _updateOnFirstRedo(updateOnFirstRedo)
    , _dt(dt)
    , _dv(dv)
    , _keys(keys)
    , _widget(widget)
{
    setText( tr("Move multiple keys") );
}

static void
moveKeys(CurveGui* curve,
         std::vector<MoveKeysCommand::KeyToMove> &vect,
         double dt,
         double dv)
{
    if (!curve) {
        return;
    }
    KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>(curve);
    BezierCPCurveGui* isBezierCurve = dynamic_cast<BezierCPCurveGui*>(curve);

    if (isBezierCurve) {
        for (std::size_t i = 0; i < vect.size(); ++i) {
            const MoveKeysCommand::KeyToMove& k = vect[i];
            double oldTime = k.key->key.getTime();
            k.key->key.setTime(oldTime + dt);
            isBezierCurve->getBezier()->moveKeyframe( oldTime, k.key->key.getTime() );
        }
    } else if (isKnobCurve) {
        KnobIPtr knob = isKnobCurve->getInternalKnob();
        if (!knob) {
            return;
        }
        KnobParametric* isParametric = dynamic_cast<KnobParametric*>( knob.get() );

        if (!isParametric) {
            std::vector<KeyFrame> keysToChange( vect.size() );
            for (std::size_t i = 0; i < vect.size(); ++i) {
                keysToChange[i] = vect[i].key->key;
            }
            knob->moveValuesAtTime(eCurveChangeReasonCurveEditor, ViewSpec::all(), isKnobCurve->getDimension(), dt, dv, &keysToChange);
            for (std::size_t i = 0; i < vect.size(); ++i) {
                vect[i].key->key = keysToChange[i];
            }
        } else {
            CurvePtr internalCurve = curve->getInternalCurve();

            for (std::size_t i = 0; i < vect.size(); ++i) {
                const MoveKeysCommand::KeyToMove& k = vect[i];
                double oldTime = k.key->key.getTime();
                double newX = oldTime + dt;
                double newY = k.key->key.getValue() + dv;
                int keyframeIndex = internalCurve->keyFrameIndex(oldTime);
                int newIndex;

                k.key->key = internalCurve->setKeyFrameValueAndTime(newX, newY, keyframeIndex, &newIndex);
            }
            isParametric->evaluateValueChange(isKnobCurve->getDimension(), isParametric->getCurrentTime(), ViewIdx(0), eValueChangedReasonUserEdited);
        }
    }
}

void
MoveKeysCommand::move(double dt,
                      double dv)
{
    //Prevent all redraws from moveKey
    _widget->setUpdatesEnabled(false);

    std::list<KnobHolder*> differentKnobs;
    std::list<RotoContextPtr> rotoToEvaluate;

    for (std::map<CurveGuiPtr, std::vector<MoveKeysCommand::KeyToMove> >::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>( it->first.get() );
        if (isKnobCurve) {
            if ( !isKnobCurve->getKnobGui() ) {
                RotoContextPtr roto = isKnobCurve->getRotoContext();
                assert(roto);
                if ( std::find(rotoToEvaluate.begin(), rotoToEvaluate.end(), roto) == rotoToEvaluate.end() ) {
                    rotoToEvaluate.push_back(roto);
                }
            } else {
                KnobI* k = isKnobCurve->getInternalKnob().get();
                if ( k->getHolder() ) {
                    if ( std::find( differentKnobs.begin(), differentKnobs.end(), k->getHolder() ) == differentKnobs.end() ) {
                        differentKnobs.push_back( k->getHolder() );
                        k->getHolder()->beginChanges();
                    }
                }
            }
        }
        moveKeys(it->first.get(), it->second, dt, dv);
        _widget->updateSelectionAfterCurveChange( it->first.get() );
    }

    if (_firstRedoCalled || _updateOnFirstRedo) {
        for (std::list<KnobHolder*>::iterator it = differentKnobs.begin(); it != differentKnobs.end(); ++it) {
            (*it)->endChanges();
        }
    }

    _widget->setUpdatesEnabled(true);

    for (std::list<RotoContextPtr>::iterator it = rotoToEvaluate.begin(); it != rotoToEvaluate.end(); ++it) {
        (*it)->evaluateChange();
    }

    _widget->refreshSelectedKeysAndUpdate();
}

void
MoveKeysCommand::undo()
{
    move(-_dt, -_dv);
}

void
MoveKeysCommand::redo()
{
    move(_dt, _dv);
    _firstRedoCalled = true;
}

bool
MoveKeysCommand::mergeWith(const QUndoCommand * command)
{
    const MoveKeysCommand* cmd = dynamic_cast<const MoveKeysCommand*>(command);

    if ( cmd && ( cmd->id() == id() ) ) {
        if ( cmd->_keys.size() != _keys.size() ) {
            return false;
        }

        std::map<CurveGuiPtr, std::vector<MoveKeysCommand::KeyToMove> >::const_iterator itother = cmd->_keys.begin();
        for (std::map<CurveGuiPtr, std::vector<MoveKeysCommand::KeyToMove> >::const_iterator it = _keys.begin(); it != _keys.end(); ++it, ++itother) {
            if (itother->first != it->first) {
                return false;
            }

            if ( itother->second.size() != it->second.size() ) {
                return false;
            }

            for (std::size_t i = 0; i < it->second.size(); ++i) {
                if (it->second[i].key != itother->second[i].key) {
                    return false;
                }
            }
        }

        _dt += cmd->_dt;
        _dv += cmd->_dv;

        return true;
    } else {
        return false;
    }
}

int
MoveKeysCommand::id() const
{
    return kCurveEditorMoveMultipleKeysCommandCompressionID;
}

void
MoveKeysCommand::getEditedCurves(std::list<CurveGuiPtr>* curves) const
{
    for (std::map<CurveGuiPtr, std::vector<MoveKeysCommand::KeyToMove>>::const_iterator it = _keys.begin(); it != _keys.end(); ++it) {
        curves->push_back(it->first);
    }
}

//////////////////////////////SET MULTIPLE KEYS INTERPOLATION COMMAND//////////////////////////////////////////////
SetKeysInterpolationCommand::SetKeysInterpolationCommand(CurveWidget* widget,
                                                         const std::list<KeyInterpolationChange>& keys,
                                                         QUndoCommand* parent)
    : CurveEditorCommand(parent)
    , _keys(keys)
    , _widget(widget)
{
    setText( tr("Set multiple keys interpolation") );
}

void
SetKeysInterpolationCommand::setNewInterpolation(bool undo)
{
    std::list<KnobI*> differentKnobs;
    std::list<RotoContextPtr> rotoToEvaluate;

    for (std::list<KeyInterpolationChange >::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>( it->key->curve.get() );
        if (isKnobCurve) {
            if ( !isKnobCurve->getKnobGui() ) {
                RotoContextPtr roto = isKnobCurve->getRotoContext();
                assert(roto);
                if ( std::find(rotoToEvaluate.begin(), rotoToEvaluate.end(), roto) == rotoToEvaluate.end() ) {
                    rotoToEvaluate.push_back(roto);
                }
            } else {
                KnobI* k = isKnobCurve->getInternalKnob().get();
                if ( std::find(differentKnobs.begin(), differentKnobs.end(), k) == differentKnobs.end() ) {
                    differentKnobs.push_back(k);
                    k->beginChanges();
                }
            }
        } else {
            BezierCPCurveGui* bezierCurve = dynamic_cast<BezierCPCurveGui*>( it->key->curve.get() );
            assert(bezierCurve);
            if (bezierCurve) {
                assert( bezierCurve->getBezier() );
                rotoToEvaluate.push_back( bezierCurve->getBezier()->getContext() );
            }
        }
    }

    for (std::list<KeyInterpolationChange >::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        KeyframeTypeEnum interp = undo ? it->oldInterp : it->newInterp;
        KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>( it->key->curve.get() );
        if (isKnobCurve) {
            KnobIPtr knob = isKnobCurve->getInternalKnob();
            KnobParametric* isParametric = dynamic_cast<KnobParametric*>( knob.get() );

            if (isParametric) {
                int keyframeIndex = it->key->curve->getKeyFrameIndex( it->key->key.getTime() );
                if (keyframeIndex != -1) {
                    it->key->key = it->key->curve->setKeyFrameInterpolation(interp, keyframeIndex);
                }
                isParametric->evaluateValueChange(isKnobCurve->getDimension(), it->key->key.getTime(), ViewIdx(0), eValueChangedReasonUserEdited);
            } else {
                knob->setInterpolationAtTime(eCurveChangeReasonCurveEditor, ViewIdx(0),  isKnobCurve->getDimension(), it->key->key.getTime(), interp, &it->key->key);
            }
        } else {
            ///interpolation for bezier curve is either linear or constant
            interp = interp == eKeyframeTypeConstant ? eKeyframeTypeConstant :
                     eKeyframeTypeLinear;
            int keyframeIndex = it->key->curve->getKeyFrameIndex( it->key->key.getTime() );
            if (keyframeIndex != -1) {
                it->key->curve->setKeyFrameInterpolation(interp, keyframeIndex);
            }
        }
    }

    for (std::list<KnobI*>::iterator it = differentKnobs.begin(); it != differentKnobs.end(); ++it) {
        (*it)->endChanges();
    }
    for (std::list<RotoContextPtr>::iterator it = rotoToEvaluate.begin(); it != rotoToEvaluate.end(); ++it) {
        (*it)->evaluateChange();
    }

    _widget->refreshSelectedKeysAndUpdate();
} // SetKeysInterpolationCommand::setNewInterpolation

void
SetKeysInterpolationCommand::undo()
{
    setNewInterpolation(true);
}

void
SetKeysInterpolationCommand::redo()
{
    setNewInterpolation(false);
}

void
SetKeysInterpolationCommand::getEditedCurves(std::list<CurveGuiPtr>* curves) const
{
    for (std::list<KeyInterpolationChange>::const_iterator it = _keys.begin(); it != _keys.end(); ++it) {
        curves->push_back(it->key->curve);
    }
}

/////////////////////////// MoveTangentCommand

MoveTangentCommand::MoveTangentCommand(CurveWidget* widget,
                                       SelectedTangentEnum deriv,
                                       const KeyPtr& key,
                                       double dx,
                                       double dy, //< dx dy relative to the center of the keyframe
                                       bool updateOnFirstRedo,
                                       bool forceBreak,
                                       QUndoCommand* parent)
    : CurveEditorCommand(parent)
    , _widget(widget)
    , _key(key)
    , _deriv(deriv)
    , _oldInterp(key->key.getInterpolation())
    , _oldLeft(key->key.getLeftDerivative())
    , _oldRight(key->key.getRightDerivative())
    , _oldLeftWeight(key->key.getLeftWeight())
    , _oldRightWeight(key->key.getRightWeight())
    , _newLeftWeight(_oldLeftWeight)
    , _newRightWeight(_oldRightWeight)
    , _setBoth(false)
    , _updateOnFirstRedo(updateOnFirstRedo)
    , _firstRedoCalled(false)
{
    CurvePtr internalCurve = key->curve->getInternalCurve();
    KeyFrameSet keys = internalCurve->getKeyFrames_mt_safe();
    KeyFrameSet::const_iterator cur = keys.find(key->key);

    assert( cur != keys.end() );

    //find next and previous keyframes
    KeyFrameSet::const_iterator prev = cur;
    if ( prev != keys.begin() ) {
        --prev;
    } else {
        prev = keys.end();
    }
    KeyFrameSet::const_iterator next = cur;
    if ( next != keys.end() ) {
        ++next;
    }

    // handle first and last keyframe correctly:
    // - if their interpolation was eKeyframeTypeCatmullRom or eKeyframeTypeCubic, then it becomes eKeyframeTypeFree
    // - in all other cases it becomes eKeyframeTypeBroken
    KeyframeTypeEnum interp = key->key.getInterpolation();
    bool keyframeIsFirstOrLast = ( prev == keys.end() || next == keys.end() );
    bool interpIsNotBroken = (interp != eKeyframeTypeBroken);
    bool interpIsCatmullRomOrCubicOrFree = (interp == eKeyframeTypeCatmullRom ||
                                            interp == eKeyframeTypeCubic ||
                                            interp == eKeyframeTypeFree);
    _setBoth = keyframeIsFirstOrLast ? interpIsCatmullRomOrCubicOrFree  || internalCurve->isCurvePeriodic() : interpIsNotBroken;

    // Ctrl-drag always breaks the key, even one whose first/last or periodic status would
    // otherwise keep both sides linked.
    if (forceBreak) {
        _setBoth = false;
    }

    bool isLeft;
    if (deriv == eSelectedTangentLeft) {
        //if dx is not of the good sign it would make the curve uncontrollable
        if (dx <= 0) {
            dx = 0.0001;
        }
        isLeft = true;
    } else {
        //if dx is not of the good sign it would make the curve uncontrollable
        if (dx >= 0) {
            dx = -0.0001;
        }
        isLeft = false;
    }
    double derivative = dy / dx;

    if (_setBoth) {
        _newInterp = eKeyframeTypeFree;
        _newLeft = derivative;
        _newRight = derivative;
    } else {
        if (isLeft) {
            _newLeft = derivative;
            _newRight = _oldRight;
        } else {
            _newLeft = _oldLeft;
            _newRight = derivative;
        }
        _newInterp = eKeyframeTypeBroken;
    }

    // The handle's time extent is a fraction of the segment it reaches into. A periodic curve's
    // first and last keys reach into the wrap segment.
    double leftSpan = 0.;
    double rightSpan = 0.;
    {
        const std::pair<double, double> xRange = internalCurve->getXRange();
        const double period = xRange.second - xRange.first;
        const bool wraps = internalCurve->isCurvePeriodic() && (keys.size() > 1) && std::isfinite(period) && (period > 0.);
        if (prev != keys.end()) {
            leftSpan = cur->getTime() - prev->getTime();
        } else if (wraps) {
            leftSpan = cur->getTime() - (keys.rbegin()->getTime() - period);
        }
        if (next != keys.end()) {
            rightSpan = next->getTime() - cur->getTime();
        } else if (wraps) {
            rightSpan = keys.begin()->getTime() + period - cur->getTime();
        }
    }
    const double draggedSpan = isLeft ? leftSpan : rightSpan;
    if (draggedSpan > 0.) {
        const double oldWeight = isLeft ? _oldLeftWeight : _oldRightWeight;
        const double newWeight = Interpolation::clampTangentWeight(std::abs(dx) / draggedSpan);
        const double otherSpan = isLeft ? rightSpan : leftSpan;
        double& draggedNew = isLeft ? _newLeftWeight : _newRightWeight;
        double& otherNew = isLeft ? _newRightWeight : _newLeftWeight;
        const double otherOld = isLeft ? _oldRightWeight : _oldLeftWeight;
        draggedNew = newWeight;
        if (_setBoth && (otherSpan > 0.)) {
            otherNew = Interpolation::clampTangentWeight(otherOld * newWeight / oldWeight);
        }
    }
}

MoveTangentCommand::MoveTangentCommand(CurveWidget* widget,
                                       SelectedTangentEnum deriv,
                                       const KeyPtr& key,
                                       double derivative,
                                       QUndoCommand* parent)
    : CurveEditorCommand(parent)
    , _widget(widget)
    , _key(key)
    , _deriv(deriv)
    , _oldInterp(key->key.getInterpolation())
    , _oldLeft(key->key.getLeftDerivative())
    , _oldRight(key->key.getRightDerivative())
    , _oldLeftWeight(key->key.getLeftWeight())
    , _oldRightWeight(key->key.getRightWeight())
    , _newLeftWeight(_oldLeftWeight)
    , _newRightWeight(_oldRightWeight)
    , _setBoth(true)
    , _updateOnFirstRedo(true)
    , _firstRedoCalled(false)
{
    _newInterp = _oldInterp == eKeyframeTypeBroken ? eKeyframeTypeBroken : eKeyframeTypeFree;
    _setBoth = _newInterp == eKeyframeTypeFree;

    switch (deriv) {
    case eSelectedTangentLeft:
        _newLeft = derivative;
        if (_newInterp == eKeyframeTypeBroken) {
            _newRight = _oldRight;
        } else {
            _newRight = derivative;
        }
        break;
    case eSelectedTangentRight:
        _newRight = derivative;
        if (_newInterp == eKeyframeTypeBroken) {
            _newLeft = _oldLeft;
        } else {
            _newLeft = derivative;
        }
    default:
        break;
    }
    setText( tr("Move keyframe slope") );
}

void
MoveTangentCommand::setNewDerivatives(bool undo)
{
    KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>( _key->curve.get() );

    if (isKnobCurve) {
        KnobIPtr attachedKnob = isKnobCurve->getInternalKnob();
        assert(attachedKnob);
        KnobParametric* isParametric = dynamic_cast<KnobParametric*>( attachedKnob.get() );
        double left = undo ? _oldLeft : _newLeft;
        double right = undo ? _oldRight : _newRight;
        KeyframeTypeEnum interp = undo ? _oldInterp : _newInterp;
        double leftWeight = undo ? _oldLeftWeight : _newLeftWeight;
        double rightWeight = undo ? _oldRightWeight : _newRightWeight;

        if (!isParametric) {
            attachedKnob->beginChanges();
            const KeyTangents tangents = { left, right, leftWeight, rightWeight };
            attachedKnob->setTangentsAtTime(eCurveChangeReasonCurveEditor, ViewIdx(0), isKnobCurve->getDimension(), _key->key.getTime(), tangents, NULL);
            attachedKnob->setInterpolationAtTime(eCurveChangeReasonCurveEditor, ViewIdx(0), isKnobCurve->getDimension(), _key->key.getTime(), interp, &_key->key);
            if (_firstRedoCalled || _updateOnFirstRedo) {
                attachedKnob->endChanges();
            }
        } else {
            int keyframeIndexInCurve = _key->curve->getInternalCurve()->keyFrameIndex( _key->key.getTime() );
            _key->key = _key->curve->getInternalCurve()->setKeyFrameInterpolation(interp, keyframeIndexInCurve);
            _key->key = _key->curve->getInternalCurve()->setKeyFrameTangents(left, right, leftWeight, rightWeight, keyframeIndexInCurve);
            attachedKnob->evaluateValueChange(isKnobCurve->getDimension(), _key->key.getTime(), ViewIdx(0),  eValueChangedReasonUserEdited);
        }

        // Null when a unit test exercises the command without a real curve widget behind it.
        if (_widget) {
            _widget->refreshCurveDisplayTangents(_key->curve.get());
        }
    }
}

void
MoveTangentCommand::undo()
{
    setNewDerivatives(true);
}

void
MoveTangentCommand::redo()
{
    setNewDerivatives(false);
    _firstRedoCalled = true;
}

int
MoveTangentCommand::id() const
{
    return kCurveEditorMoveTangentsCommandCompressionID;
}

void
MoveTangentCommand::getEditedCurves(std::list<CurveGuiPtr>* curves) const
{
    curves->push_back(_key->curve);
}

bool
MoveTangentCommand::mergeWith(const QUndoCommand * command)
{
    const MoveTangentCommand* cmd = dynamic_cast<const MoveTangentCommand*>(command);

    if ( cmd && ( cmd->id() == id() ) ) {
        if (cmd->_key != _key) {
            return false;
        }


        _newInterp = cmd->_newInterp;
        _newLeft = cmd->_newLeft;
        _newRight = cmd->_newRight;
        _newLeftWeight = cmd->_newLeftWeight;
        _newRightWeight = cmd->_newRightWeight;

        return true;
    } else {
        return false;
    }
}

/////////////////////////// JoinTangentsCommand

double
joinedDerivative(double left,
                 double right)
{
    return std::tan((std::atan(left) + std::atan(right)) / 2.);
}

JoinTangentsCommand::JoinTangentsCommand(CurveWidget* widget,
                                         const SelectedKeys& keys,
                                         QUndoCommand* parent)
    : CurveEditorCommand(parent)
    , _keys()
    , _widget(widget)
{
    for (SelectedKeys::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        for (std::list<KeyPtr>::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
            const KeyPtr& key = *it2;
            if (key->key.getInterpolation() != eKeyframeTypeBroken) {
                continue;
            }

            JoinedTangentChange change;
            change.key = key;
            change.oldInterp = eKeyframeTypeBroken;
            change.oldLeft = key->key.getLeftDerivative();
            change.oldRight = key->key.getRightDerivative();
            change.joined = joinedDerivative(change.oldLeft, change.oldRight);
            _keys.push_back(change);
        }
    }
    setText(tr("Join tangents"));
}

void
JoinTangentsCommand::setNewDerivatives(bool undo)
{
    for (std::list<JoinedTangentChange>::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>(it->key->curve.get());
        if (!isKnobCurve) {
            continue;
        }

        KnobIPtr attachedKnob = isKnobCurve->getInternalKnob();
        assert(attachedKnob);
        KnobParametric* isParametric = dynamic_cast<KnobParametric*>(attachedKnob.get());
        double left = undo ? it->oldLeft : it->joined;
        double right = undo ? it->oldRight : it->joined;
        KeyframeTypeEnum interp = undo ? it->oldInterp : eKeyframeTypeFree;

        if (!isParametric) {
            attachedKnob->beginChanges();
            const KeyTangents tangents = { left, right, it->key->key.getLeftWeight(), it->key->key.getRightWeight() };
            attachedKnob->setTangentsAtTime(eCurveChangeReasonCurveEditor, ViewIdx(0), isKnobCurve->getDimension(), it->key->key.getTime(), tangents, NULL);
            attachedKnob->setInterpolationAtTime(eCurveChangeReasonCurveEditor, ViewIdx(0), isKnobCurve->getDimension(), it->key->key.getTime(), interp, &it->key->key);
            attachedKnob->endChanges();
        } else {
            int keyframeIndexInCurve = it->key->curve->getInternalCurve()->keyFrameIndex(it->key->key.getTime());
            it->key->key = it->key->curve->getInternalCurve()->setKeyFrameInterpolation(interp, keyframeIndexInCurve);
            it->key->key = it->key->curve->getInternalCurve()->setKeyFrameTangents(left, right, it->key->key.getLeftWeight(), it->key->key.getRightWeight(), keyframeIndexInCurve);
            attachedKnob->evaluateValueChange(isKnobCurve->getDimension(), it->key->key.getTime(), ViewIdx(0), eValueChangedReasonUserEdited);
        }
    }

    // Null when a unit test exercises the command without a real curve widget behind it.
    if (_widget) {
        _widget->refreshSelectedKeysAndUpdate();
    }
}

void
JoinTangentsCommand::undo()
{
    setNewDerivatives(true);
}

void
JoinTangentsCommand::redo()
{
    setNewDerivatives(false);
}

void
JoinTangentsCommand::getEditedCurves(std::list<CurveGuiPtr>* curves) const
{
    for (std::list<JoinedTangentChange>::const_iterator it = _keys.begin(); it != _keys.end(); ++it) {
        curves->push_back(it->key->curve);
    }
}

TransformKeysCommand::TransformKeysCommand(CurveWidget* widget,
                                           const SelectedKeys& keys,
                                           double centerX,
                                           double centerY,
                                           double tx,
                                           double ty,
                                           double sx,
                                           double sy,
                                           bool updateOnFirstRedo,
                                           QUndoCommand* parent)
    : CurveEditorCommand(parent)
    , _firstRedoCalled(false)
    , _updateOnFirstRedo(updateOnFirstRedo)
    , _keys(keys)
    , _widget(widget)
    , _matrix()
    , _invMatrix()
{
    _matrix = Transform::matTransformCanonical(tx, ty, sx, sy, 0, 0, true, 0, centerX, centerY);
    _invMatrix = Transform::matTransformCanonical(-tx, -ty, 1. / sx, 1. / sy, 0, 0, true, 0, centerX, centerY);
}

TransformKeysCommand::~TransformKeysCommand()
{
}

static void
transformKeyFrame(KeyFrame* key,
                  const Transform::Matrix3x3& m)
{
    double oldTime = key->getTime();
    Transform::Point3D p;

    p.x = oldTime;
    p.y = key->getValue();
    p.z = 1;
    p = Transform::matApply(m, p);
    key->setTime(p.x);
}

static void
transform(const Transform::Matrix3x3& matrix,
          CurveGui* curve,
          const std::list<KeyPtr>& keyframes)
{
    KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>(curve);
    BezierCPCurveGui* isBezierCurve = dynamic_cast<BezierCPCurveGui*>(curve);

    if (isKnobCurve) {
        KnobIPtr knob = isKnobCurve->getInternalKnob();
        KnobParametric* isParametric = dynamic_cast<KnobParametric*>( knob.get() );

        if (isParametric) {
            CurvePtr internalCurve = curve->getInternalCurve();
            for (std::list<KeyPtr>::const_iterator it = keyframes.begin(); it != keyframes.end(); ++it) {
                const KeyPtr& k = (*it);
                Transform::Point3D p;
                p.x = k->key.getTime();
                p.y = k->key.getValue();
                p.z = 1;
                p = Transform::matApply(matrix, p);


                double oldTime = k->key.getTime();
                int keyframeIndex = internalCurve->keyFrameIndex(oldTime);
                int newIndex;

                k->key = internalCurve->setKeyFrameValueAndTime(p.x, p.y, keyframeIndex, &newIndex);
            }
            isParametric->evaluateValueChange(isKnobCurve->getDimension(), isParametric->getCurrentTime(), ViewIdx(0), eValueChangedReasonUserEdited);
        } else {
            std::vector<KeyFrame> keys( keyframes.size() );
            int i = 0;
            for (std::list<KeyPtr>::const_iterator it = keyframes.begin(); it != keyframes.end(); ++it, ++i) {
                keys[i] = (*it)->key;
            }
            knob->transformValuesAtTime(eCurveChangeReasonCurveEditor, ViewSpec::all(), isKnobCurve->getDimension(), matrix, &keys);
            i = 0;
            for (std::list<KeyPtr>::const_iterator it = keyframes.begin(); it != keyframes.end(); ++it, ++i) {
                (*it)->key = keys[i];
            }
        }
    } else if (isBezierCurve) {
        for (std::list<KeyPtr>::const_iterator it = keyframes.begin(); it != keyframes.end(); ++it) {
            double oldTime = (*it)->key.getTime();
            transformKeyFrame(&(*it)->key, matrix);
            //transformKeyFrame(&(*it)->prevKey,*_matrix);
            //transformKeyFrame(&(*it)->nextKey,*_matrix);
            isBezierCurve->getBezier()->moveKeyframe( oldTime, (*it)->key.getTime() );
        }
    }
}

void
TransformKeysCommand::transformKeys(const Transform::Matrix3x3& matrix)
{
    std::list<KnobHolder*> differentKnobs;
    std::list<RotoContextPtr> rotoToEvaluate;

    for (SelectedKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>( it->first.get() );
        if (isKnobCurve) {
            if ( !isKnobCurve->getKnobGui() ) {
                RotoContextPtr roto = isKnobCurve->getRotoContext();
                assert(roto);
                if ( std::find(rotoToEvaluate.begin(), rotoToEvaluate.end(), roto) == rotoToEvaluate.end() ) {
                    rotoToEvaluate.push_back(roto);
                }
            } else {
                KnobI* k = isKnobCurve->getInternalKnob().get();
                if ( k->getHolder() ) {
                    if ( std::find( differentKnobs.begin(), differentKnobs.end(), k->getHolder() ) == differentKnobs.end() ) {
                        differentKnobs.push_back( k->getHolder() );
                        k->getHolder()->beginChanges();
                    }
                }
            }
        }
    }


    for (SelectedKeys::iterator it = _keys.begin(); it != _keys.end(); ++it) {
        transform(matrix, it->first.get(), it->second);
        _widget->updateSelectionAfterCurveChange( it->first.get() );
    }


    if (_firstRedoCalled || _updateOnFirstRedo) {
        for (std::list<KnobHolder*>::iterator it = differentKnobs.begin(); it != differentKnobs.end(); ++it) {
            (*it)->endChanges();
        }
    }

    for (std::list<RotoContextPtr>::iterator it = rotoToEvaluate.begin(); it != rotoToEvaluate.end(); ++it) {
        (*it)->evaluateChange();
    }

    _widget->setSelectedKeys(_keys);
    _firstRedoCalled = true;
}

void
TransformKeysCommand::undo()
{
    transformKeys(_invMatrix);
}

void
TransformKeysCommand::redo()
{
    transformKeys(_matrix);
}

int
TransformKeysCommand::id() const
{
    return kCurveEditorTransformKeysCommandCompressionID;
}

void
TransformKeysCommand::getEditedCurves(std::list<CurveGuiPtr>* curves) const
{
    for (SelectedKeys::const_iterator it = _keys.begin(); it != _keys.end(); ++it) {
        curves->push_back(it->first);
    }
}

bool
TransformKeysCommand::mergeWith(const QUndoCommand * command)
{
    const TransformKeysCommand* cmd = dynamic_cast<const TransformKeysCommand*>(command);

    if ( cmd && ( cmd->id() == id() ) ) {
        if ( cmd->_keys.size() != _keys.size() ) {
            return false;
        }

        SelectedKeys::const_iterator itother = cmd->_keys.begin();
        for (SelectedKeys::const_iterator it = _keys.begin(); it != _keys.end(); ++it, ++itother) {
            if (*itother != *it) {
                return false;
            }
        }

        _matrix = matMul(_matrix, cmd->_matrix);
        _invMatrix = matMul(_invMatrix, cmd->_invMatrix);

        return true;
    } else {
        return false;
    }
}

//////////////////////////////FOLDED KNOB CURVE EDIT COMMAND//////////////////////////////////////////////

namespace {

// Until the copy is done, dimension 0 holds a different curve from the others: each value change
// in between would otherwise unfold the knob in its panel and show its hidden curves again.
class FoldStateHold {
public:
    FoldStateHold(CurveEditor* editor,
                  const std::list<KnobGuiWPtr>& knobGuis)
        : _editor(editor)
        , _knobGuis()
    {
        for (std::list<KnobGuiWPtr>::const_iterator it = knobGuis.begin(); it != knobGuis.end(); ++it) {
            KnobGuiPtr knobGui = it->lock();
            KnobGuiValue* isValue = dynamic_cast<KnobGuiValue*>(knobGui.get());
            if (isValue) {
                isValue->setFoldStateHeld(true);
                _knobGuis.push_back(knobGui);
            }
        }
        if (_editor) {
            _editor->setFoldStateHeld(true);
        }
    }

    ~FoldStateHold()
    {
        if (_editor) {
            _editor->setFoldStateHeld(false);
        }
        for (std::list<KnobGuiPtr>::const_iterator it = _knobGuis.begin(); it != _knobGuis.end(); ++it) {
            dynamic_cast<KnobGuiValue*>(it->get())->setFoldStateHeld(false);
        }
    }

private:
    CurveEditor* _editor;
    std::list<KnobGuiPtr> _knobGuis;
};

// A knob without keys reads its static value, which copying the curve leaves alone.
void
copyDimensionZeroStaticValue(KnobI* knob,
                             int dimension)
{
    if (KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>(knob)) {
        isDouble->setValue(isDouble->getValue(0, ViewIdx(0), false), ViewSpec::all(), dimension, true);
    } else if (KnobIntBase* isInt = dynamic_cast<KnobIntBase*>(knob)) {
        isInt->setValue(isInt->getValue(0, ViewIdx(0), false), ViewSpec::all(), dimension, true);
    } else if (KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>(knob)) {
        isBool->setValue(isBool->getValue(0, ViewIdx(0), false), ViewSpec::all(), dimension, true);
    }
}

} // namespace

FoldedCurveEditCommand::FoldedCurveEditCommand(CurveEditor* editor,
                                               QUndoCommand* inner,
                                               const std::list<CurveGuiPtr>& foldedDimensionZeroCurves,
                                               QUndoCommand* parent)
    : QUndoCommand(parent)
    , _editor(editor)
    , _inner(inner)
    , _knobs()
    , _knobGuis()
    , _snapshotsTaken(false)
{
    for (std::list<CurveGuiPtr>::const_iterator it = foldedDimensionZeroCurves.begin(); it != foldedDimensionZeroCurves.end(); ++it) {
        KnobCurveGui* isKnobCurve = dynamic_cast<KnobCurveGui*>(it->get());
        if (!isKnobCurve) {
            continue;
        }
        KnobIPtr knob = isKnobCurve->getInternalKnob();
        if (!knob || (knob->getDimension() <= 1) || dynamic_cast<KnobParametric*>(knob.get())) {
            continue;
        }
        bool alreadyListed = false;
        for (std::list<FoldedKnob>::const_iterator it2 = _knobs.begin(); it2 != _knobs.end(); ++it2) {
            if (it2->knob == knob) {
                alreadyListed = true;
                break;
            }
        }
        if (alreadyListed) {
            continue;
        }
        FoldedKnob folded;
        folded.knob = knob;
        folded.dimensionZeroCurve = *it;
        _knobs.push_back(folded);
        KnobGuiPtr knobGui = isKnobCurve->getKnobGui();
        if (knobGui) {
            _knobGuis.push_back(knobGui);
        }
    }
    setText(_inner->text());
}

FoldedCurveEditCommand::~FoldedCurveEditCommand()
{
}

void
FoldedCurveEditCommand::copyDimensionZeroToOthers()
{
    for (std::list<FoldedKnob>::iterator it = _knobs.begin(); it != _knobs.end(); ++it) {
        CurvePtr dimensionZero = it->dimensionZeroCurve->getInternalCurve();
        if (!dimensionZero) {
            continue;
        }
        const Curve source(*dimensionZero);
        const bool hasKeys = source.getKeyFramesCount() > 0;
        it->knob->beginChanges();
        for (int d = 1; d < it->knob->getDimension(); ++d) {
            it->knob->cloneCurve(ViewSpec::all(), d, source);
            if (!hasKeys) {
                copyDimensionZeroStaticValue(it->knob.get(), d);
            }
        }
        it->knob->endChanges();
    }
}

void
FoldedCurveEditCommand::restoreOtherDimensions()
{
    for (std::list<FoldedKnob>::iterator it = _knobs.begin(); it != _knobs.end(); ++it) {
        it->knob->beginChanges();
        for (std::size_t d = 1; d < it->curvesBeforeFirstRedo.size(); ++d) {
            if (it->curvesBeforeFirstRedo[d]) {
                it->knob->cloneCurve(ViewSpec::all(), (int)d, *it->curvesBeforeFirstRedo[d]);
            }
        }
        it->knob->endChanges();
    }
}

void
FoldedCurveEditCommand::undo()
{
    FoldStateHold hold(_editor, _knobGuis);

    // The wrapped command undoes first so that the restore has the last word on dimensions 1..n,
    // even for one the wrapped command also edited.
    _inner->undo();
    restoreOtherDimensions();
}

void
FoldedCurveEditCommand::redo()
{
    FoldStateHold hold(_editor, _knobGuis);

    if (!_snapshotsTaken) {
        for (std::list<FoldedKnob>::iterator it = _knobs.begin(); it != _knobs.end(); ++it) {
            const int nDims = it->knob->getDimension();
            it->curvesBeforeFirstRedo.assign(nDims, CurvePtr());
            for (int d = 1; d < nDims; ++d) {
                CurvePtr curve = it->knob->getCurve(ViewIdx(0), d, true);
                if (curve) {
                    it->curvesBeforeFirstRedo[d] = std::make_shared<Curve>(*curve);
                }
            }
        }
        _snapshotsTaken = true;
    }
    _inner->redo();
    copyDimensionZeroToOthers();
}

int
FoldedCurveEditCommand::id() const
{
    return _inner->id();
}

bool
FoldedCurveEditCommand::mergeWith(const QUndoCommand* command)
{
    const FoldedCurveEditCommand* other = dynamic_cast<const FoldedCurveEditCommand*>(command);

    if (!other || (other->_knobs.size() != _knobs.size())) {
        return false;
    }
    std::list<FoldedKnob>::const_iterator itOther = other->_knobs.begin();
    for (std::list<FoldedKnob>::const_iterator it = _knobs.begin(); it != _knobs.end(); ++it, ++itOther) {
        if (it->knob != itOther->knob) {
            return false;
        }
    }

    // The stack redoes a command before offering it for a merge, so the other command has
    // already copied dimension 0 into the others and there is nothing left to copy here.
    // Its snapshots were taken mid-drag and are dropped; this command's still hold the state
    // from before the drag began.
    return _inner->mergeWith(other->_inner.get());
}

NATRON_NAMESPACE_EXIT
