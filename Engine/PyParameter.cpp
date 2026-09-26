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

#include "PyParameter.h"

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "Engine/AppInstance.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobSerialization.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER
NATRON_PYTHON_NAMESPACE_ENTER

Param::Param(const KnobIPtr& knob)
    : _knob(knob)
{
}

Param::~Param()
{
}

Param*
Param::getParent() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0;
    }
    KnobIPtr parent = knob->getParentKnob();

    if (parent) {
        return new Param(parent);
    } else {
        return 0;
    }
}

int
Param::getNumDimensions() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0;
    }
    return knob->getDimension();
}

QString
Param::getScriptName() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return QString();
    }
    return QString::fromUtf8( knob->getName().c_str() );
}

QString
Param::getLabel() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return QString();
    }
    return QString::fromUtf8( knob->getLabel().c_str() );
}

QString
Param::getTypeName() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return QString();
    }
    return QString::fromUtf8( knob->typeName().c_str() );
}

QString
Param::getHelp() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return QString();
    }
    return QString::fromUtf8( knob->getHintToolTip().c_str() );
}

void
Param::setHelp(const QString& help)
{
    KnobIPtr knob = getInternalKnob();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setHintToolTip( help.toStdString() );
}

bool
Param::getIsVisible() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return false;
    }
    return !knob->getIsSecret();
}

void
Param::setVisible(bool visible)
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return;
    }
    knob->setSecret(!visible);
}

void
Param::setVisibleByDefault(bool visible)
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return;
    }
    knob->setSecretByDefault(!visible);
}

bool
Param::getIsEnabled(int dimension) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return false;
    }
    return knob->isEnabled(dimension);
}

void
Param::setEnabled(bool enabled,
                  int dimension)
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return;
    }
    knob->setEnabled(dimension, enabled);
}

void
Param::setEnabledByDefault(bool enabled)
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return;
    }
    knob->setDefaultAllDimensionsEnabled(enabled);
}

bool
Param::getIsPersistent() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return false;
    }
    return knob->getIsPersistent();
}

void
Param::setPersistent(bool persistent)
{
    KnobIPtr knob = getInternalKnob();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setIsPersistent(persistent);
}

bool
Param::getEvaluateOnChange() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return false;
    }
    return knob->getEvaluateOnChange();
}

void
Param::setEvaluateOnChange(bool eval)
{
    KnobIPtr knob = getInternalKnob();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setEvaluateOnChange(eval);
}

bool
Param::getCanAnimate() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return false;
    }
    return knob->canAnimate();
}

bool
Param::getIsAnimationEnabled() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return false;
    }
    return knob->isAnimationEnabled();
}

void
Param::setAnimationEnabled(bool e)
{
    KnobIPtr knob = getInternalKnob();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setAnimationEnabled(e);
}

bool
Param::getAddNewLine()
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return false;
    }
    return knob->isNewLineActivated();
}

void
Param::setAddNewLine(bool a)
{
    KnobIPtr knob = getInternalKnob();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }

    KnobIPtr parentKnob = knob->getParentKnob();
    if (parentKnob) {
        KnobGroup* parentIsGrp = dynamic_cast<KnobGroup*>( parentKnob.get() );
        KnobPage* parentIsPage = dynamic_cast<KnobPage*>( parentKnob.get() );
        assert(parentIsGrp || parentIsPage);
        KnobsVec children;
        if (parentIsGrp) {
            children = parentIsGrp->getChildren();
        } else if (parentIsPage) {
            children = parentIsPage->getChildren();
        }
        for (U32 i = 0; i < children.size(); ++i) {
            if (children[i] == knob) {
                if (i > 0) {
                    children[i - 1]->setAddNewLine(a);
                }
                break;
            }
        }
    }
}

bool
Param::copy(Param* other,
            int dimension)
{
    KnobIPtr thisKnob = _knob.lock();
    KnobIPtr otherKnob = other->_knob.lock();

    if ( !thisKnob->isTypeCompatible(otherKnob) ) {
        return false;
    }
    thisKnob->cloneAndUpdateGui(otherKnob.get(), dimension);

    return true;
}

bool
Param::slaveTo(Param* other,
               int thisDimension,
               int otherDimension)
{
    KnobIPtr thisKnob = _knob.lock();
    KnobIPtr otherKnob = other->_knob.lock();

    if ( !KnobI::areTypesCompatibleForSlave( thisKnob.get(), otherKnob.get() ) ) {
        return false;
    }
    if ( (thisDimension < 0) || ( thisDimension >= thisKnob->getDimension() ) || (otherDimension < 0) || ( otherDimension >= otherKnob->getDimension() ) ) {
        return false;
    }

    return thisKnob->slaveTo(thisDimension, otherKnob, otherDimension);
}

void
Param::unslave(int dimension)
{
    KnobIPtr thisKnob = _knob.lock();

    if (!thisKnob) {
        return;
    }
    thisKnob->unSlave(dimension, false);
}

double
Param::random(double min,
              double max) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0;
    }
    return knob->random(min, max);
}

double
Param::random(double min,
              double max,
              double time,
              unsigned int seed) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0;
    }
    return knob->random(min, max, time, seed);
}

int
Param::randomInt(int min,
                 int max)
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0;
    }
    return knob->randomInt(min, max);
}

int
Param::randomInt(int min,
                 int max,
                 double time,
                 unsigned int seed) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0;
    }
    return knob->randomInt(min, max, time, seed);
}

double
Param::curve(double time,
             int dimension) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0.;
    }
    return knob->getRawCurveValueAt(time, ViewSpec::current(), dimension);
}

bool
Param::setAsAlias(Param* other)
{
    if (!other) {
        return false;
    }
    KnobIPtr otherKnob = other->_knob.lock();
    KnobIPtr thisKnob = getInternalKnob();
    if ( !otherKnob || !thisKnob || ( otherKnob->typeName() != thisKnob->typeName() ) ||
         ( otherKnob->getDimension() != thisKnob->getDimension() ) ) {
        return false;
    }

    return otherKnob->setKnobAsAliasOfThis(thisKnob, true);
}

void
Param::setIconFilePath(const QString& icon)
{
    KnobIPtr thisKnob = _knob.lock();
    if (!thisKnob) {
        return;
    }
    thisKnob->setIconLabel( icon.toStdString() );
}

AnimatedParam::AnimatedParam(const KnobIPtr& knob)
    : Param(knob)
{
}

AnimatedParam::~AnimatedParam()
{
}

bool
AnimatedParam::getIsAnimated(int dimension) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return false;
    }
    return knob->isAnimated( dimension, ViewSpec::current() );
}

int
AnimatedParam::getNumKeys(int dimension) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0;
    }
    return knob->getKeyFramesCount(ViewSpec::current(), dimension);
}

int
AnimatedParam::getKeyIndex(double time,
                           int dimension) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0;
    }
    return knob->getKeyFrameIndex(ViewSpec::current(), dimension, time);
}

bool
AnimatedParam::getKeyTime(int index,
                          int dimension,
                          double* time) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        *time = 0.;
        return false;
    }
    return knob->getKeyFrameTime(ViewSpec::current(), index, dimension, time);
}

void
AnimatedParam::deleteValueAtTime(double time,
                                 int dimension)
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return;
    }
    knob->deleteValueAtTime(eCurveChangeReasonInternal, time, ViewSpec::all(), dimension, false);
}

void
AnimatedParam::removeAnimation(int dimension)
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return;
    }
    knob->removeAnimation(ViewSpec::all(), dimension);
}

double
AnimatedParam::getDerivativeAtTime(double time,
                                   int dimension) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0.;
    }
    return knob->getDerivativeAtTime(time, ViewSpec::current(), dimension);
}

double
AnimatedParam::getIntegrateFromTimeToTime(double time1,
                                          double time2,
                                          int dimension) const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0.;
    }
    return knob->getIntegrateFromTimeToTime(time1, time2, ViewSpec::current(), dimension);
}

int
AnimatedParam::getCurrentTime() const
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return 0;
    }
    return knob->getCurrentTime();
}

bool
AnimatedParam::setInterpolationAtTime(double time,
                                      KeyframeTypeEnum interpolation,
                                      int dimension)
{
    KnobIPtr knob = getInternalKnob();

    if (!knob) {
        return false;
    }
    KeyFrame newKey;

    return knob->setInterpolationAtTime(eCurveChangeReasonInternal, ViewSpec::current(), dimension, time, interpolation, &newKey);
}

void
Param::_addAsDependencyOf(int fromExprDimension,
                          Param* param,
                          int thisDimension)
{
    //from expr is in the dimension of expressionKnob
    //thisDimension is in the dimesnion of getValueCallerKnob

    KnobIPtr expressionKnob = param->_knob.lock();
    KnobIPtr getValueCallerKnob = _knob.lock();

    if ( (fromExprDimension < 0) || ( fromExprDimension >= expressionKnob->getDimension() ) ) {
        return;
    }
    if ( (thisDimension != -1) && (thisDimension != 0) && ( thisDimension >= getValueCallerKnob->getDimension() ) ) {
        return;
    }
    if (getValueCallerKnob == expressionKnob) {
        return;
    }

    getValueCallerKnob->addListener(true, fromExprDimension, thisDimension, expressionKnob);
}

bool
AnimatedParam::setExpression(const QString& expr,
                             bool hasRetVariable,
                             int dimension)
{
    KnobIPtr thisKnob = _knob.lock();
    if (!thisKnob) {
        return false;
    }
    try {
        thisKnob->setExpression(dimension, expr.toStdString(), hasRetVariable, true);
    } catch (...) {
        return false;
    }

    return true;
}

QString
AnimatedParam::getExpression(int dimension,
                             bool* hasRetVariable) const
{
    KnobIPtr thisKnob = _knob.lock();
    if (!thisKnob) {
        return QString();
    }
    QString ret = QString::fromUtf8( thisKnob->getExpression(dimension).c_str() );

    *hasRetVariable = thisKnob->isExpressionUsingRetVariable(dimension);

    return ret;
}

///////////// IntParam

IntParam::IntParam(const KnobIntPtr& knob)
    : AnimatedParam( std::dynamic_pointer_cast<KnobI>(knob) )
    , _intKnob(knob)
{
}

IntParam::~IntParam()
{
}

int
IntParam::get() const
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return 0;
    }

    return knob->getValue();
}

Int2DTuple
Int2DParam::get() const
{
    Int2DTuple ret = {0, 0};
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return ret;
    }

    ret.x = knob->getValue(0);
    ret.y = knob->getValue(1);

    return ret;
}

Int3DTuple
Int3DParam::get() const
{
    KnobIntPtr knob = _intKnob.lock();
    Int3DTuple ret = {0, 0, 0};
    if (!knob) {
        return ret;
    }

    ret.x = knob->getValue(0);
    ret.y = knob->getValue(1);
    ret.z = knob->getValue(2);

    return ret;
}

int
IntParam::get(double frame) const
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return 0;
    }

    return knob->getValueAtTime(frame, 0);
}

Int2DTuple
Int2DParam::get(double frame) const
{
    Int2DTuple ret = {0, 0};
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return ret;
    }

    ret.x = knob->getValueAtTime(frame, 0);
    ret.y = knob->getValueAtTime(frame, 1);

    return ret;
}

Int3DTuple
Int3DParam::get(double frame) const
{
    Int3DTuple ret = {0, 0, 0};
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return ret;
    }

    ret.x = knob->getValueAtTime(frame, 0);
    ret.y = knob->getValueAtTime(frame, 1);
    ret.z = knob->getValueAtTime(frame, 2);

    return ret;
}

void
IntParam::set(int x)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValue(x, ViewSpec::current(), 0);
}

void
Int2DParam::set(int x,
                int y)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->beginChanges();
    knob->setValue(x, ViewSpec::current(), 0);
    knob->setValue(y, ViewSpec::current(), 1);
    knob->endChanges();
}

void
Int3DParam::set(int x,
                int y,
                int z)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }

    knob->beginChanges();
    knob->setValue(x, ViewSpec::current(), 0);
    knob->setValue(y, ViewSpec::current(), 1);
    knob->setValue(z, ViewSpec::current(), 2);
    knob->endChanges();
}

void
IntParam::set(int x,
              double frame)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValueAtTime(frame, x, ViewSpec::current(), 0);
}

void
Int2DParam::set(int x,
                int y,
                double frame)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValuesAtTime(frame, x, y, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
}

void
Int3DParam::set(int x,
                int y,
                int z,
                double frame)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValuesAtTime(frame, x, y, z, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
}

int
IntParam::getValue(int dimension) const
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getValue(dimension);
}

void
IntParam::setValue(int value,
                   int dimension)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValue(value, ViewSpec::current(), dimension);
}

int
IntParam::getValueAtTime(double time,
                         int dimension) const
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getValueAtTime(time, dimension);
}

void
IntParam::setValueAtTime(int value,
                         double time,
                         int dimension)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValueAtTime(time, value, ViewSpec::current(), dimension);
}

void
IntParam::setDefaultValue(int value,
                          int dimension)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->setDefaultValueWithoutApplying(value, dimension);
}

int
IntParam::getDefaultValue(int dimension) const
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getDefaultValues_mt_safe()[dimension];
}

void
IntParam::restoreDefaultValue(int dimension)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->resetToDefaultValueWithoutSecretNessAndEnabledNess(dimension);
}

void
IntParam::setMinimum(int minimum,
                     int dimension)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->setMinimum(minimum, dimension);
}

int
IntParam::getMinimum(int dimension) const
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getMinimum(dimension);
}

void
IntParam::setMaximum(int maximum,
                     int dimension)
{
    KnobIntPtr knob = _intKnob.lock();
    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setMaximum(maximum, dimension);
}

int
IntParam::getMaximum(int dimension) const
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getMaximum(dimension);
}

void
IntParam::setDisplayMinimum(int minimum,
                            int dimension)
{
    KnobIntPtr knob = _intKnob.lock();
    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setDisplayMinimum(minimum, dimension);
}

int
IntParam::getDisplayMinimum(int dimension) const
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getDisplayMinimum(dimension);
}

void
IntParam::setDisplayMaximum(int maximum,
                            int dimension)
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return;
    }
    knob->setDisplayMaximum(maximum, dimension);
}

int
IntParam::getDisplayMaximum(int dimension) const
{
    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getDisplayMaximum(dimension);
}

int
IntParam::addAsDependencyOf(int fromExprDimension,
                            Param* param,
                            int thisDimension)
{
    _addAsDependencyOf(fromExprDimension, param, thisDimension);

    KnobIntPtr knob = _intKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getValue();
}

//////////// DoubleParam

DoubleParam::DoubleParam(const KnobDoublePtr& knob)
    : AnimatedParam( std::dynamic_pointer_cast<KnobI>(knob) )
    , _doubleKnob(knob)
{
}

DoubleParam::~DoubleParam()
{
}

double
DoubleParam::get() const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getValue(0);
}

Double2DTuple
Double2DParam::get() const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    Double2DTuple ret = {0., 0.};
    if (!knob) {
        return ret;
    }

    ret.x = knob->getValue(0);
    ret.y = knob->getValue(1);

    return ret;
}

Double3DTuple
Double3DParam::get() const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    Double3DTuple ret = {0., 0., 0.};
    if (!knob) {
        return ret;
    }

    ret.x = knob->getValue(0);
    ret.y = knob->getValue(1);
    ret.z = knob->getValue(2);

    return ret;
}

double
DoubleParam::get(double frame) const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getValueAtTime(frame, 0);
}

Double2DTuple
Double2DParam::get(double frame) const
{
    Double2DTuple ret = {0., 0.};
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return ret;
    }

    ret.x = knob->getValueAtTime(frame, 0);
    ret.y = knob->getValueAtTime(frame, 1);

    return ret;
}

Double3DTuple
Double3DParam::get(double frame) const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    Double3DTuple ret = {0., 0., 0.};
    if (!knob) {
        return ret;
    }

    ret.x = knob->getValueAtTime(frame, 0);
    ret.y = knob->getValueAtTime(frame, 1);
    ret.z = knob->getValueAtTime(frame, 2);

    return ret;
}

void
DoubleParam::set(double x)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValue(x, ViewSpec::current(), 0);
}

void
Double2DParam::set(double x,
                   double y)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValues(x, y, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
}

void
Double3DParam::set(double x,
                   double y,
                   double z)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValues(x, y, z, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
}

void
DoubleParam::set(double x,
                 double frame)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }
    return knob->setValueAtTime(frame, x, ViewSpec::current(), 0);
}

void
Double2DParam::set(double x,
                   double y,
                   double frame)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValuesAtTime(frame, x, y, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
}

void
Double2DParam::setUsePointInteract(bool use)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }
    return knob->setHasHostOverlayHandle(use);
}


void
Double2DParam::setCanAutoFoldDimensions(bool can)
{
    std::shared_ptr<KnobDouble> knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }
    return knob->setCanAutoFoldDimensions(can);
}


void
Double3DParam::set(double x,
                   double y,
                   double z,
                   double frame)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValuesAtTime(frame, x, y, z, ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
}

double
DoubleParam::getValue(int dimension) const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getValue(dimension);
}

void
DoubleParam::setValue(double value,
                      int dimension)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValue(value, ViewSpec::current(), dimension);
}

double
DoubleParam::getValueAtTime(double time,
                            int dimension) const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getValueAtTime(time, dimension);
}

void
DoubleParam::setValueAtTime(double value,
                            double time,
                            int dimension)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }
    return knob->setValueAtTime(time, value, ViewSpec::current(), dimension);
}

void
DoubleParam::setDefaultValue(double value,
                             int dimension)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }
    return knob->setDefaultValueWithoutApplying(value, dimension);
}

double
DoubleParam::getDefaultValue(int dimension) const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getDefaultValues_mt_safe()[dimension];
}

void
DoubleParam::restoreDefaultValue(int dimension)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }
    return knob->resetToDefaultValueWithoutSecretNessAndEnabledNess(dimension);
}

void
DoubleParam::setMinimum(double minimum,
                        int dimension)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }
    knob->setMinimum(minimum, dimension);
}

double
DoubleParam::getMinimum(int dimension) const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getMinimum(dimension);
}

void
DoubleParam::setMaximum(double maximum,
                        int dimension)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setMaximum(maximum, dimension);
}

double
DoubleParam::getMaximum(int dimension) const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getMaximum(dimension);
}

void
DoubleParam::setDisplayMinimum(double minimum,
                               int dimension)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setDisplayMinimum(minimum, dimension);
}

double
DoubleParam::getDisplayMinimum(int dimension) const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getDisplayMinimum(dimension);
}

void
DoubleParam::setDisplayMaximum(double maximum,
                               int dimension)
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return;
    }
    return knob->setDisplayMaximum(maximum, dimension);
}

double
DoubleParam::getDisplayMaximum(int dimension) const
{
    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getDisplayMaximum(dimension);
}

double
DoubleParam::addAsDependencyOf(int fromExprDimension,
                               Param* param,
                               int thisDimension)
{
    _addAsDependencyOf(fromExprDimension, param, thisDimension);

    KnobDoublePtr knob = _doubleKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getValue();
}

////////ColorParam

ColorParam::ColorParam(const KnobColorPtr& knob)
    : AnimatedParam( std::dynamic_pointer_cast<KnobI>(knob) )
    , _colorKnob(knob)
{
}

ColorParam::~ColorParam()
{
}

ColorTuple
ColorParam::get() const
{
    ColorTuple ret = {0., 0., 0., 0.};
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return ret;
    }
    ret.r = knob->getValue(0);
    ret.g = knob->getValue(1);
    ret.b = knob->getValue(2);
    ret.a = knob->getDimension() == 4 ? knob->getValue(3) : 1.;

    return ret;
}

ColorTuple
ColorParam::get(double frame) const
{
    ColorTuple ret = {0., 0., 0., 0.};
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return ret;
    }
    ret.r = knob->getValueAtTime(frame, 0);
    ret.g = knob->getValueAtTime(frame, 1);
    ret.b = knob->getValueAtTime(frame, 2);
    ret.a = knob->getDimension() == 4 ? knob->getValueAtTime(frame, 2) : 1.;

    return ret;
}

void
ColorParam::set(double r,
                double g,
                double b,
                double a)
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return;
    }
    knob->beginChanges();
    knob->setValue(r, ViewSpec::current(), 0);
    knob->setValue(g, ViewSpec::current(), 1);
    knob->setValue(b, ViewSpec::current(), 2);
    if (knob->getDimension() == 4) {
        knob->setValue(a, ViewSpec::current(), 3);
    }
    knob->endChanges();
}

void
ColorParam::set(double r,
                double g,
                double b,
                double a,
                double frame)
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return;
    }
    knob->beginChanges();
    knob->setValueAtTime(frame, r, ViewSpec::current(), 0);
    knob->setValueAtTime(frame, g, ViewSpec::current(), 1);
    int dims = knob->getDimension();
    knob->setValueAtTime(frame, b, ViewSpec::current(), 2);
    if (dims == 4) {
        knob->setValueAtTime(frame, a, ViewSpec::current(), 3);
    }
    knob->endChanges();
}

double
ColorParam::getValue(int dimension) const
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getValue(dimension);
}

void
ColorParam::setValue(double value,
                     int dimension)
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValue(value, ViewSpec::current(), dimension);
}

double
ColorParam::getValueAtTime(double time,
                           int dimension) const
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getValueAtTime(time, dimension);
}

void
ColorParam::setValueAtTime(double value,
                           double time,
                           int dimension)
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValueAtTime(time, value, ViewSpec::current(), dimension);
}

void
ColorParam::setDefaultValue(double value,
                            int dimension)
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return;
    }
    knob->setDefaultValueWithoutApplying(value, dimension);
}

double
ColorParam::getDefaultValue(int dimension) const
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getDefaultValues_mt_safe()[dimension];
}

void
ColorParam::restoreDefaultValue(int dimension)
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return;
    }
    knob->resetToDefaultValueWithoutSecretNessAndEnabledNess(dimension);
}

void
ColorParam::setMinimum(double minimum,
                       int dimension)
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return;
    }
    knob->setMinimum(minimum, dimension);
}

double
ColorParam::getMinimum(int dimension) const
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getMinimum(dimension);
}

void
ColorParam::setMaximum(double maximum,
                       int dimension)
{
    KnobColorPtr knob = _colorKnob.lock();
    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setMaximum(maximum, dimension);
}

double
ColorParam::getMaximum(int dimension) const
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getMaximum(dimension);
}

void
ColorParam::setDisplayMinimum(double minimum,
                              int dimension)
{
    KnobColorPtr knob = _colorKnob.lock();
    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setDisplayMinimum(minimum, dimension);
}

double
ColorParam::getDisplayMinimum(int dimension) const
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getDisplayMinimum(dimension);
}

void
ColorParam::setDisplayMaximum(double maximum,
                              int dimension)
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return;
    }
    knob->setDisplayMaximum(maximum, dimension);
}

double
ColorParam::getDisplayMaximum(int dimension) const
{
    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getDisplayMaximum(dimension);
}

double
ColorParam::addAsDependencyOf(int fromExprDimension,
                              Param* param,
                              int thisDimension)
{
    _addAsDependencyOf(fromExprDimension, param, thisDimension);

    KnobColorPtr knob = _colorKnob.lock();
    if (!knob) {
        return 0.;
    }
    return knob->getValue();
}

//////////////// ChoiceParam
ChoiceParam::ChoiceParam(const KnobChoicePtr& knob)
    : AnimatedParam( std::dynamic_pointer_cast<KnobI>(knob) )
    , _choiceKnob(knob)
{
}

ChoiceParam::~ChoiceParam()
{
}

int
ChoiceParam::get() const
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getValue(0);
}

int
ChoiceParam::get(double frame) const
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getValueAtTime(frame, 0);
}

void
ChoiceParam::set(int x)
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValue(x, ViewSpec::current(), 0);
}

void
ChoiceParam::set(int x,
                 double frame)
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValueAtTime(frame, x, ViewSpec::current(), 0);
}

void
ChoiceParam::set(const QString& label)
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return;
    }
    std::string choiceID = label.toStdString();
    AppInstancePtr app = knob->getHolder()->getApp();
    if (app && app->isCreatingPythonGroup()) {
        // Before Natron 2.2.3, all dynamic choice parameters for multiplane had a string parameter.
        // The string parameter had the same name as the choice parameter plus "Choice" appended.
        // If we found such a parameter, retrieve the string from it.
        filterKnobChoiceOptionCompat(std::string(), -1, -1, -1, -1, -1, knob->getName(), &choiceID);
    }
    KnobHelper::ValueChangedReturnCodeEnum s = knob->setValueFromID(choiceID, 0);

    Q_UNUSED(s);
}

int
ChoiceParam::getValue() const
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getValue(0);
}

void
ChoiceParam::setValue(int value)
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValue(value, ViewSpec::current(), 0);
}

void
ChoiceParam::setValue(const QString& label)
{
    set(label);
}

int
ChoiceParam::getValueAtTime(double time) const
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getValueAtTime(time, 0);
}

void
ChoiceParam::setValueAtTime(int value,
                            double time)
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return;
    }
    knob->setValueAtTime(time, value, ViewSpec::current(), 0);
}

void
ChoiceParam::setDefaultValue(int value)
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return;
    }
    knob->setDefaultValueWithoutApplying(value, 0);
}

void
ChoiceParam::setDefaultValue(const QString& value)
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return;
    }
    knob->setDefaultValueFromIDWithoutApplying( value.toStdString() );
}

int
ChoiceParam::getDefaultValue() const
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getDefaultValues_mt_safe()[0];
}

void
ChoiceParam::restoreDefaultValue()
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return;
    }
    knob->resetToDefaultValueWithoutSecretNessAndEnabledNess(0);
}

void
ChoiceParam::addOption(const QString& option,
                       const QString& help)
{
    KnobChoicePtr knob = _choiceKnob.lock();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }

    ChoiceOption opt(option.toStdString(), "", help.toStdString());
    knob->appendChoice(opt);
}

void
ChoiceParam::setOptions(const std::list<std::pair<QString, QString> >& options)
{
    KnobChoicePtr knob = _choiceKnob.lock();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }

    std::vector<ChoiceOption> entries;
    for (std::list<std::pair<QString, QString> >::const_iterator it = options.begin(); it != options.end(); ++it) {
        entries.push_back( ChoiceOption(it->first.toStdString(), "", it->second.toStdString()));
    }
    knob->populateChoices(entries);
}

QString
ChoiceParam::getOption(int index) const
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return QString();
    }
    std::vector<ChoiceOption> entries = knob->getEntries_mt_safe();

    if ( (index < 0) || ( index >= (int)entries.size() ) ) {
        return QString();
    }

    return QString::fromUtf8( entries[index].id.c_str() );
}

int
ChoiceParam::getNumOptions() const
{
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getNumEntries();
}

QStringList
ChoiceParam::getOptions() const
{
    QStringList ret;
    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return ret;
    }
    std::vector<ChoiceOption> entries = knob->getEntries_mt_safe();

    for (std::size_t i = 0; i < entries.size(); ++i) {
        ret.push_back( QString::fromUtf8( entries[i].id.c_str() ) );
    }

    return ret;
}

int
ChoiceParam::addAsDependencyOf(int fromExprDimension,
                               Param* param,
                               int thisDimension)
{
    _addAsDependencyOf(fromExprDimension, param, thisDimension);

    KnobChoicePtr knob = _choiceKnob.lock();
    if (!knob) {
        return 0;
    }
    return knob->getValue();
}

////////////////BooleanParam


BooleanParam::BooleanParam(const KnobBoolPtr& knob)
    : AnimatedParam( std::dynamic_pointer_cast<KnobI>(knob) )
    , _boolKnob(knob)
{
}

BooleanParam::~BooleanParam()
{
}

bool
BooleanParam::get() const
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return false;
    }

    return knob->getValue(0);
}

bool
BooleanParam::get(double frame) const
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return false;
    }

    return knob->getValueAtTime(frame, 0);
}

void
BooleanParam::set(bool x)
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValue(x, ViewSpec::current(), 0);
}

void
BooleanParam::set(bool x,
                  double frame)
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValueAtTime(frame, x, ViewSpec::current(), 0);
}

bool
BooleanParam::getValue() const
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return false;
    }

    return knob->getValue(0);
}

void
BooleanParam::setValue(bool value)
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValue(value, ViewSpec::current(), 0);
}

bool
BooleanParam::getValueAtTime(double time) const
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return false;
    }

    return knob->getValueAtTime(time, 0);
}

void
BooleanParam::setValueAtTime(bool value,
                             double time)
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValueAtTime(time, value, ViewSpec::current(), 0);
}

void
BooleanParam::setDefaultValue(bool value)
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return;
    }

    knob->setDefaultValueWithoutApplying(value, 0);
}

bool
BooleanParam::getDefaultValue() const
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return false;
    }

    return knob->getDefaultValues_mt_safe()[0];
}

void
BooleanParam::restoreDefaultValue()
{
    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return;
    }

    knob->resetToDefaultValueWithoutSecretNessAndEnabledNess(0);
}

bool
BooleanParam::addAsDependencyOf(int fromExprDimension,
                                Param* param,
                                int thisDimension)
{
    _addAsDependencyOf(fromExprDimension, param, thisDimension);

    KnobBoolPtr knob = _boolKnob.lock();
    if (!knob) {
        return false;
    }

    return knob->getValue();
}

////////////// StringParamBase


StringParamBase::StringParamBase(const KnobStringBasePtr& knob)
    : AnimatedParam( std::dynamic_pointer_cast<KnobI>(knob) )
    , _stringKnob(knob)
{
}

StringParamBase::~StringParamBase()
{
}

QString
StringParamBase::get() const
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return QString();
    }

    return QString::fromUtf8( knob->getValue(0).c_str() );
}

QString
StringParamBase::get(double frame) const
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return QString();
    }

    return QString::fromUtf8( knob->getValueAtTime(frame, 0).c_str() );
}

void
StringParamBase::set(const QString& x)
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValue(x.toStdString(), ViewSpec::current(), 0);
}

void
StringParamBase::set(const QString& x,
                     double frame)
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValueAtTime(frame, x.toStdString(), ViewSpec::current(), 0);
}

QString
StringParamBase::getValue() const
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return QString();
    }

    return QString::fromUtf8( knob->getValue(0).c_str() );
}

void
StringParamBase::setValue(const QString& value)
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValue(value.toStdString(), ViewSpec::current(), 0);
}

QString
StringParamBase::getValueAtTime(double time) const
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return QString();
    }

    return QString::fromUtf8( knob->getValueAtTime(time, 0).c_str() );
}

void
StringParamBase::setValueAtTime(const QString& value,
                                double time)
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return;
    }

    knob->setValueAtTime(time, value.toStdString(), ViewSpec::current(), 0);
}

void
StringParamBase::setDefaultValue(const QString& value)
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return;
    }

    knob->setDefaultValueWithoutApplying(value.toStdString(), 0);
}

QString
StringParamBase::getDefaultValue() const
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return QString();
    }

    return QString::fromUtf8( knob->getDefaultValues_mt_safe()[0].c_str() );
}

void
StringParamBase::restoreDefaultValue()
{
    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return;
    }

    return knob->resetToDefaultValueWithoutSecretNessAndEnabledNess(0);
}

QString
StringParamBase::addAsDependencyOf(int fromExprDimension,
                                   Param* param,
                                   int thisDimension)
{
    _addAsDependencyOf(fromExprDimension, param, thisDimension);

    KnobStringBasePtr knob = _stringKnob.lock();
    if (!knob) {
        return QString();
    }

    return QString::fromUtf8( knob->getValue().c_str() );
}

////////////////////StringParam

StringParam::StringParam(const KnobStringPtr& knob)
    : StringParamBase( std::dynamic_pointer_cast<KnobStringBase>(knob) )
    , _sKnob(knob)
{
}

StringParam::~StringParam()
{
}

void
StringParam::setType(StringParam::TypeEnum type)
{
    KnobStringPtr knob = _sKnob.lock();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    switch (type) {
    case eStringTypeLabel:
        knob->setAsLabel();
        break;
    case eStringTypeMultiLine:
        knob->setAsMultiLine();
        break;
    case eStringTypeRichTextMultiLine:
        knob->setAsMultiLine();
        knob->setUsesRichText(true);
        break;
    case eStringTypeCustom:
        knob->setAsCustom();
        break;
    case eStringTypeDefault:
    default:
        break;
    }
}

/////////////////////FileParam

FileParam::FileParam(const KnobFilePtr& knob)
    : StringParamBase( std::dynamic_pointer_cast<KnobStringBase>(knob) )
    , _sKnob(knob)
{
}

FileParam::~FileParam()
{
}

void
FileParam::setSequenceEnabled(bool enabled)
{
    KnobFilePtr k = _sKnob.lock();

    if ( !k || !k->isUserKnob() ) {
        return;
    }
    if (enabled) {
        k->setAsInputImage();
    }
}

void
FileParam::openFile()
{
    KnobFilePtr k = _sKnob.lock();

    if (k) {
        k->open_file();
    }
}

void
FileParam::reloadFile()
{
    KnobFilePtr k = _sKnob.lock();

    if (k) {
        k->reloadFile();
    }
}

/////////////////////OutputFileParam

OutputFileParam::OutputFileParam(const KnobOutputFilePtr& knob)
    : StringParamBase( std::dynamic_pointer_cast<KnobStringBase>(knob) )
    , _sKnob(knob)
{
}

OutputFileParam::~OutputFileParam()
{
}

void
OutputFileParam::setSequenceEnabled(bool enabled)
{
    KnobOutputFilePtr knob = _sKnob.lock();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    if (enabled) {
        knob->setAsOutputImageFile();
    } else {
        knob->turnOffSequences();
    }
}

void
OutputFileParam::openFile()
{
    KnobOutputFilePtr knob = _sKnob.lock();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->open_file();
}

////////////////////PathParam

PathParam::PathParam(const KnobPathPtr& knob)
    : StringParamBase( std::dynamic_pointer_cast<KnobStringBase>(knob) )
    , _sKnob(knob)
{
}

PathParam::~PathParam()
{
}

void
PathParam::setAsMultiPathTable()
{
    KnobPathPtr knob = _sKnob.lock();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    knob->setMultiPath(true);
}

void
PathParam::setTable(const std::list<std::vector<std::string> >& table)
{
    KnobPathPtr k = _sKnob.lock();
    if (!k) {
        return;
    }
    if (!k->isMultiPath()) {
        return;
    }
    try {
        k->setTable(table);
    } catch (...) {

    }
}

void
PathParam::getTable(std::list<std::vector<std::string> >* table) const
{
    KnobPathPtr k = _sKnob.lock();
    if (!k) {
        return;
    }
    if (!k->isMultiPath()) {
        return;
    }
    try {
        k->getTable(table);
    } catch (const std::exception& /*e*/) {
        return;
    }
}

////////////////////ChannelSetParam

static std::string
channelSetRowModeToString(ChannelSetRow::ModeEnum mode)
{
    switch (mode) {
    case ChannelSetRow::eModeNone:
        return "none";
    case ChannelSetRow::eModeAll:
        return "all";
    case ChannelSetRow::eModeRegex:
        return "regex";
    case ChannelSetRow::eModeLayer:
    default:
        return "layer";
    }
}

static std::vector<std::string>
channelsFromStringList(const QStringList& channels)
{
    std::vector<std::string> ret;

    ret.reserve(channels.size());
    for (QStringList::const_iterator it = channels.begin(); it != channels.end(); ++it) {
        ret.push_back(it->toStdString());
    }

    return ret;
}

ChannelSetParam::ChannelSetParam(const KnobChannelSetPtr& knob)
    : StringParamBase(std::dynamic_pointer_cast<KnobStringBase>(knob))
    , _tKnob(knob)
{
}

ChannelSetParam::~ChannelSetParam()
{
}

void
ChannelSetParam::getRows(std::list<std::string>* modes,
                         std::list<std::string>* layersOrPatterns,
                         std::list<std::list<std::string>>* channels) const
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    std::vector<ChannelSetRow> rows = knob->getRows();
    for (std::vector<ChannelSetRow>::const_iterator it = rows.begin(); it != rows.end(); ++it) {
        modes->push_back(channelSetRowModeToString(it->mode));
        layersOrPatterns->push_back(it->layerOrPattern);
        channels->push_back(std::list<std::string>(it->channels.begin(), it->channels.end()));
    }
}

void
ChannelSetParam::setNone()
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    try {
        knob->setNone();
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    }
}

void
ChannelSetParam::setAll()
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    try {
        knob->setAll();
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    }
}

void
ChannelSetParam::setLayer(const QString& layerID,
                          const QStringList& channels,
                          int row)
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    std::vector<std::string> chans = channelsFromStringList(channels);
    try {
        knob->setLayer(row, layerID.toStdString(), &chans);
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    }
}

void
ChannelSetParam::setChannels(const QStringList& channels,
                             int row)
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    std::vector<std::string> chans = channelsFromStringList(channels);
    try {
        knob->setChannels(row, chans);
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    }
}

void
ChannelSetParam::setRegex(const QString& pattern,
                          int row)
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    try {
        knob->setRegex(row, pattern.toStdString());
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    }
}

void
ChannelSetParam::setExcludedChannels(const QStringList& channels,
                                     int row)
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    std::vector<std::string> chans = channelsFromStringList(channels);
    try {
        knob->setExcludedChannels(row, chans);
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    }
}

QStringList
ChannelSetParam::getExcludedChannels(int row) const
{
    QStringList ret;
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return ret;
    }
    try {
        std::vector<std::string> chans = knob->getExcludedChannels(row);
        for (std::vector<std::string>::const_iterator it = chans.begin(); it != chans.end(); ++it) {
            ret.push_back(QString::fromUtf8(it->c_str()));
        }
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    }

    return ret;
}

int
ChannelSetParam::addLayer(const QString& layerID,
                          const QStringList& channels)
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return -1;
    }
    std::vector<std::string> chans = channelsFromStringList(channels);

    return knob->addLayer(layerID.toStdString(), &chans);
}

int
ChannelSetParam::addRegex(const QString& pattern)
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return -1;
    }

    return knob->addRegex(pattern.toStdString());
}

void
ChannelSetParam::removeRow(int row)
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    try {
        knob->removeRow(row);
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    }
}

QString
ChannelSetParam::getSummary() const
{
    KnobChannelSetPtr knob = _tKnob.lock();

    if (!knob) {
        return QString();
    }

    return QString::fromUtf8(knob->getSummary().c_str());
}

////////////////////LayerSelectParam

LayerSelectParam::LayerSelectParam(const KnobLayerSelectPtr& knob)
    : StringParamBase(std::dynamic_pointer_cast<KnobStringBase>(knob))
    , _tKnob(knob)
{
}

LayerSelectParam::~LayerSelectParam()
{
}

QString
LayerSelectParam::getLayer() const
{
    KnobLayerSelectPtr knob = _tKnob.lock();

    if (!knob) {
        return QString();
    }

    return QString::fromUtf8(knob->getLayer().c_str());
}

void
LayerSelectParam::setLayer(const QString& layerID)
{
    KnobLayerSelectPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    knob->setLayer(layerID.toStdString());
}

QStringList
LayerSelectParam::getChannels() const
{
    QStringList ret;
    KnobLayerSelectPtr knob = _tKnob.lock();

    if (!knob) {
        return ret;
    }
    std::vector<std::string> channels = knob->getChannels();
    for (std::vector<std::string>::const_iterator it = channels.begin(); it != channels.end(); ++it) {
        ret.push_back(QString::fromUtf8(it->c_str()));
    }

    return ret;
}

void
LayerSelectParam::setChannels(const QStringList& channels)
{
    KnobLayerSelectPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    std::vector<std::string> chans = channelsFromStringList(channels);
    try {
        knob->setChannels(chans);
    } catch (const std::exception& e) {
        PyErr_SetString(PyExc_ValueError, e.what());
    }
}

QString
LayerSelectParam::getSummary() const
{
    KnobLayerSelectPtr knob = _tKnob.lock();

    if (!knob) {
        return QString();
    }

    return QString::fromUtf8(knob->getSummary().c_str());
}

////////////////////ChannelSelectParam

ChannelSelectParam::ChannelSelectParam(const KnobChannelSelectPtr& knob)
    : StringParamBase(std::dynamic_pointer_cast<KnobStringBase>(knob))
    , _tKnob(knob)
{
}

ChannelSelectParam::~ChannelSelectParam()
{
}

QString
ChannelSelectParam::get() const
{
    KnobChannelSelectPtr knob = _tKnob.lock();

    if (!knob) {
        return QString();
    }

    return QString::fromUtf8(knob->get().c_str());
}

void
ChannelSelectParam::set(const QString& value)
{
    KnobChannelSelectPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    knob->set(value.toStdString());
}

void
ChannelSelectParam::setNone()
{
    KnobChannelSelectPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    knob->setNone();
}

bool
ChannelSelectParam::isNone() const
{
    KnobChannelSelectPtr knob = _tKnob.lock();

    if (!knob) {
        return true;
    }

    return knob->isNone();
}

QString
ChannelSelectParam::getSummary() const
{
    KnobChannelSelectPtr knob = _tKnob.lock();

    if (!knob) {
        return QString();
    }

    return QString::fromUtf8(knob->getSummary().c_str());
}

////////////////////ShuffleMapParam

static KnobLayerSelectPtr
shuffleMapGetSlotKnob(const KnobShuffleMapPtr& knob,
                      const std::string& slot)
{
    if (!knob) {
        return KnobLayerSelectPtr();
    }
    KnobHolder* holder = knob->getHolder();
    if (!holder) {
        return KnobLayerSelectPtr();
    }

    return holder->getKnobByNameAndType<KnobLayerSelect>(slot);
}

/**
 * @brief The channel names of the layer currently selected on "slot" (one of in1/in2/out1/out2),
 * resolved the same way Shuffle itself resolves an output layer ID: the Color alias, then the
 * project's layer registry. This is a pure function of the knob's stored selection, independent
 * of whether the corresponding input is actually connected.
 **/
static bool
shuffleMapResolveLayerChannels(const KnobShuffleMapPtr& knob,
                               const std::string& slot,
                               std::vector<std::string>* channels,
                               std::string* error)
{
    KnobLayerSelectPtr slotKnob = shuffleMapGetSlotKnob(knob, slot);

    if (!slotKnob) {
        *error = "\"" + slot + "\" is not a slot on this node";
        return false;
    }
    std::string layerID = slotKnob->getLayer();
    if (layerID.empty()) {
        *error = "slot \"" + slot + "\" has no layer selected";
        return false;
    }
    if (ImageLayerDesc::isColorLayer(layerID)) {
        *channels = ImageLayerDesc::getRGBAComponents().getChannels();
        return true;
    }

    KnobHolder* holder = knob->getHolder();
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ImageLayerDesc desc;
    if (project && project->findLayer(layerID, &desc)) {
        *channels = desc.getChannels();
        return true;
    }

    *error = "slot \"" + slot + "\"'s layer is not resolvable";
    return false;
}

static bool
shuffleMapFindChannelIndex(const std::vector<std::string>& channels,
                           const std::string& channel,
                           int* index)
{
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i] == channel) {
            *index = (int)i;
            return true;
        }
    }

    return false;
}

// '#' can't appear in a channel name (LayerRegistry allows only letters, digits and '_'), so
// "#N" never collides with a channel literally named "N".
static const char kShuffleMapIndexMarker = '#';

static bool
shuffleMapParseIndex(const std::string& text,
                     int* index)
{
    if ((text.size() < 2) || (text.size() > 10) || (text[0] != kShuffleMapIndexMarker)) {
        return false;
    }
    for (std::size_t i = 1; i < text.size(); ++i) {
        if (!std::isdigit((unsigned char)text[i])) {
            return false;
        }
    }
    *index = std::atoi(text.c_str() + 1);

    return true;
}

/**
 * @brief channel's index on slot: "#N" for index N, whatever the slot's layer, or else a
 * channel name of the slot's current layer.
 **/
static bool
shuffleMapResolveChannelIndex(const KnobShuffleMapPtr& knob,
                              const std::string& slot,
                              const std::string& channel,
                              int* index,
                              std::string* error)
{
    if (shuffleMapParseIndex(channel, index)) {
        return true;
    }

    std::vector<std::string> channels;
    std::string resolveError;
    const bool resolved = shuffleMapResolveLayerChannels(knob, slot, &channels, &resolveError);

    if (resolved && shuffleMapFindChannelIndex(channels, channel, index)) {
        return true;
    }
    *error = resolved ? ("\"" + channel + "\" is not a channel of " + slot + "'s current layer") : resolveError;

    return false;
}

/**
 * @brief "slot.<name>" for index on slot's current layer, or "slot.#<index>" when the slot has
 * no layer channel at that index, so a stored row always has a form
 * shuffleMapResolveChannelIndex() reads back to the same index.
 **/
static std::string
shuffleMapFormatChannel(const KnobShuffleMapPtr& knob,
                        const std::string& slot,
                        int index)
{
    if (index < 0) {
        return std::string();
    }
    std::vector<std::string> channels;
    std::string error;
    if (shuffleMapResolveLayerChannels(knob, slot, &channels, &error) && ((std::size_t)index < channels.size())) {
        return slot + "." + channels[index];
    }

    return slot + "." + kShuffleMapIndexMarker + std::to_string(index);
}

/**
 * @brief Splits "slot.channel" at the first '.'. Neither half may be empty.
 **/
static bool
shuffleMapSplit(const std::string& s,
                std::string* slot,
                std::string* channel)
{
    std::size_t pos = s.find('.');

    if (pos == std::string::npos || pos == 0 || pos == s.size() - 1) {
        return false;
    }
    *slot = s.substr(0, pos);
    *channel = s.substr(pos + 1);

    return true;
}

static bool
shuffleMapParseDst(const KnobShuffleMapPtr& knob,
                   const QString& dstStr,
                   int* outSlot,
                   int* outIndex,
                   std::string* error)
{
    std::string dst = dstStr.toStdString();
    std::string slot, channel;

    if (!shuffleMapSplit(dst, &slot, &channel)) {
        *error = "malformed destination \"" + dst + "\"";
        return false;
    }
    if (slot == "out1") {
        *outSlot = 1;
    } else if (slot == "out2") {
        *outSlot = 2;
    } else {
        *error = "\"" + slot + "\" is not an output slot";
        return false;
    }

    return shuffleMapResolveChannelIndex(knob, slot, channel, outIndex, error);
}

static bool
shuffleMapParseSrc(const KnobShuffleMapPtr& knob,
                   const QString& srcStr,
                   ShuffleSource* src,
                   std::string* error)
{
    std::string s = srcStr.toStdString();

    if (s == "0") {
        *src = ShuffleSource::makeZero();
        return true;
    }
    if (s == "1") {
        *src = ShuffleSource::makeOne();
        return true;
    }

    std::string slot, channel;
    if (!shuffleMapSplit(s, &slot, &channel)) {
        *error = "malformed source \"" + s + "\"";
        return false;
    }

    int slotNumber = 0;
    if (slot == "in1") {
        slotNumber = 1;
    } else if (slot == "in2") {
        slotNumber = 2;
    } else {
        *error = "\"" + slot + "\" is not an input slot";
        return false;
    }

    int index = 0;
    if (!shuffleMapResolveChannelIndex(knob, slot, channel, &index, error)) {
        return false;
    }
    *src = ShuffleSource::makeInput(slotNumber, index);

    return true;
}

static std::string
shuffleMapFormatSrc(const KnobShuffleMapPtr& knob,
                    const ShuffleSource& src)
{
    switch (src.kind) {
    case ShuffleSource::eZero:
        return "0";
    case ShuffleSource::eOne:
        return "1";
    case ShuffleSource::eInput:
        return shuffleMapFormatChannel(knob, (src.slot == 1) ? "in1" : "in2", src.index);
    }

    return std::string();
}

static std::string
shuffleMapFormatDst(const KnobShuffleMapPtr& knob,
                    int outSlot,
                    int outIndex)
{
    return shuffleMapFormatChannel(knob, (outSlot == 1) ? "out1" : "out2", outIndex);
}

/**
 * @brief dst's channel resolved through the owning Shuffle's getEffectiveSource(), which
 * turns an implicit source into 0 for a None slot or a channel beyond its layer's channel
 * count. Falls back to the knob's own stored-or-identity source when the knob is not
 * currently attached to a Shuffle node.
 **/
static ShuffleSource
shuffleMapEffectiveSource(const KnobShuffleMapPtr& knob,
                          int outSlot,
                          int outIndex)
{
    Shuffle* effect = dynamic_cast<Shuffle*>(knob->getHolder());

    if (effect) {
        AppInstancePtr app = effect->getApp();
        const double time = app ? app->getTimeLine()->currentFrame() : 0.;

        return effect->getEffectiveSource(outSlot, outIndex, time);
    }

    return knob->getSource(outSlot, outIndex);
}

std::map<std::string, std::string>
getShuffleMapModifiedConnections(const KnobShuffleMapPtr& knob)
{
    std::map<std::string, std::string> ret;

    if (!knob) {
        return ret;
    }

    typedef std::map<std::pair<int, int>, ShuffleSource> RowsByChannel;
    RowsByChannel currentByChannel;
    std::vector<ShuffleMapRow> currentRows = knob->getRows();
    for (std::vector<ShuffleMapRow>::const_iterator it = currentRows.begin(); it != currentRows.end(); ++it) {
        currentByChannel[std::make_pair(it->outSlot, it->outIndex)] = it->src;
    }

    RowsByChannel defaultByChannel;
    std::vector<ShuffleMapRow> defaultRows = knob->decodeRows(knob->getDefaultValue(0));
    for (std::vector<ShuffleMapRow>::const_iterator it = defaultRows.begin(); it != defaultRows.end(); ++it) {
        defaultByChannel[std::make_pair(it->outSlot, it->outIndex)] = it->src;
    }

    std::set<std::pair<int, int>> channels;
    for (RowsByChannel::const_iterator it = currentByChannel.begin(); it != currentByChannel.end(); ++it) {
        channels.insert(it->first);
    }
    for (RowsByChannel::const_iterator it = defaultByChannel.begin(); it != defaultByChannel.end(); ++it) {
        channels.insert(it->first);
    }

    for (std::set<std::pair<int, int>>::const_iterator it = channels.begin(); it != channels.end(); ++it) {
        int outSlot = it->first;
        int outIndex = it->second;

        RowsByChannel::const_iterator curIt = currentByChannel.find(*it);
        ShuffleSource currentSrc = (curIt != currentByChannel.end()) ? curIt->second : KnobShuffleMap::defaultSource(outSlot, outIndex);

        RowsByChannel::const_iterator defIt = defaultByChannel.find(*it);
        ShuffleSource defaultSrc = (defIt != defaultByChannel.end()) ? defIt->second : KnobShuffleMap::defaultSource(outSlot, outIndex);

        if (currentSrc == defaultSrc) {
            continue;
        }

        std::string dst = shuffleMapFormatDst(knob, outSlot, outIndex);
        if (dst.empty()) {
            continue;
        }
        std::string src = shuffleMapFormatSrc(knob, currentSrc);
        if (src.empty()) {
            continue;
        }
        ret[dst] = src;
    }

    return ret;
}

ShuffleMapParam::ShuffleMapParam(const KnobShuffleMapPtr& knob)
    : StringParamBase(std::dynamic_pointer_cast<KnobStringBase>(knob))
    , _tKnob(knob)
{
}

ShuffleMapParam::~ShuffleMapParam()
{
}

void
ShuffleMapParam::connect(const QString& src,
                         const QString& dst)
{
    KnobShuffleMapPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    int outSlot = 0, outIndex = 0;
    std::string error;
    if (!shuffleMapParseDst(knob, dst, &outSlot, &outIndex, &error)) {
        PyErr_SetString(PyExc_ValueError, error.c_str());
        return;
    }
    ShuffleSource source;
    if (!shuffleMapParseSrc(knob, src, &source, &error)) {
        PyErr_SetString(PyExc_ValueError, error.c_str());
        return;
    }
    knob->setSource(outSlot, outIndex, source);
}

void
ShuffleMapParam::disconnect(const QString& dst)
{
    KnobShuffleMapPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    int outSlot = 0, outIndex = 0;
    std::string error;
    if (!shuffleMapParseDst(knob, dst, &outSlot, &outIndex, &error)) {
        PyErr_SetString(PyExc_ValueError, error.c_str());
        return;
    }
    knob->clear(outSlot, outIndex);
}

QString
ShuffleMapParam::getSource(const QString& dst) const
{
    KnobShuffleMapPtr knob = _tKnob.lock();

    if (!knob) {
        return QString();
    }
    int outSlot = 0, outIndex = 0;
    std::string error;
    if (!shuffleMapParseDst(knob, dst, &outSlot, &outIndex, &error)) {
        PyErr_SetString(PyExc_ValueError, error.c_str());
        return QString();
    }
    ShuffleSource source = shuffleMapEffectiveSource(knob, outSlot, outIndex);

    return QString::fromUtf8(shuffleMapFormatSrc(knob, source).c_str());
}

std::map<std::string, std::string>
ShuffleMapParam::getConnections() const
{
    std::map<std::string, std::string> ret;
    KnobShuffleMapPtr knob = _tKnob.lock();

    if (!knob) {
        return ret;
    }

    static const char* const outSlotNames[2] = { "out1", "out2" };
    for (int s = 0; s < 2; ++s) {
        int outSlot = s + 1;
        std::vector<std::string> channels;
        std::string error;
        if (!shuffleMapResolveLayerChannels(knob, outSlotNames[s], &channels, &error)) {
            // out2 with no layer selected (None) contributes nothing.
            continue;
        }
        for (std::size_t i = 0; i < channels.size(); ++i) {
            ShuffleSource source = shuffleMapEffectiveSource(knob, outSlot, (int)i);
            std::string src = shuffleMapFormatSrc(knob, source);
            if (src.empty()) {
                continue;
            }
            ret[std::string(outSlotNames[s]) + "." + channels[i]] = src;
        }
    }

    return ret;
}

void
ShuffleMapParam::reset()
{
    KnobShuffleMapPtr knob = _tKnob.lock();

    if (!knob) {
        return;
    }
    knob->reset();
}

bool
ShuffleMapParam::setExpression(const QString& /*expr*/,
                               bool /*hasRetVariable*/,
                               int /*dimension*/)
{
    PyErr_SetString(PyExc_ValueError, "The mapping parameter does not support expressions");

    return false;
}

////////////////////ButtonParam

ButtonParam::ButtonParam(const KnobButtonPtr& knob)
    : Param(knob)
    , _buttonKnob( std::dynamic_pointer_cast<KnobButton>(knob) )
{
}

ButtonParam::~ButtonParam()
{
}

void
ButtonParam::trigger()
{
    KnobButtonPtr knob = _buttonKnob.lock();
    if (!knob) {
        return;
    }
    knob->trigger();
}

////////////////////SeparatorParam

SeparatorParam::SeparatorParam(const KnobSeparatorPtr& knob)
    : Param(knob)
    , _separatorKnob( std::dynamic_pointer_cast<KnobSeparator>(knob) )
{
}

SeparatorParam::~SeparatorParam()
{
}

///////////////////GroupParam

GroupParam::GroupParam(const KnobGroupPtr& knob)
    : Param(knob)
    , _groupKnob( std::dynamic_pointer_cast<KnobGroup>(knob) )
{
}

GroupParam::~GroupParam()
{
}

void
GroupParam::addParam(const Param* param)
{
    if (!param) {
        return;
    }
    KnobIPtr knob = param->getInternalKnob();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    KnobGroupPtr group = _groupKnob.lock();
    if (!group) {
        return;
    }
    group->addKnob(knob);
}

void
GroupParam::setAsTab()
{
    KnobGroupPtr group = _groupKnob.lock();
    if ( !group || !group->isUserKnob() ) {
        return;
    }
    group->setAsTab();
}

void
GroupParam::setOpened(bool opened)
{
    KnobGroupPtr group = _groupKnob.lock();
    if (!group) {
        return;
    }
    group->setValue(opened, ViewSpec::current(), 0);
}

bool
GroupParam::getIsOpened() const
{
    KnobGroupPtr group = _groupKnob.lock();
    if (!group) {
        return false;
    }
    return group->getValue();
}

//////////////////////PageParam

PageParam::PageParam(const KnobPagePtr& knob)
    : Param(knob)
    , _pageKnob( std::dynamic_pointer_cast<KnobPage>(knob) )
{
}

PageParam::~PageParam()
{
}

void
PageParam::addParam(const Param* param)
{
    if (!param) {
        return;
    }
    KnobIPtr knob = param->getInternalKnob();

    if ( !knob || !knob->isUserKnob() ) {
        return;
    }
    KnobPagePtr pageKnob = _pageKnob.lock();

    if ( !pageKnob ) {
        return;
    }
    pageKnob->addKnob( knob );
}

////////////////////ParametricParam
ParametricParam::ParametricParam(const KnobParametricPtr& knob)
    : Param( std::dynamic_pointer_cast<KnobI>(knob) )
    , _parametricKnob(knob)
{
}

ParametricParam::~ParametricParam()
{
}

void
ParametricParam::setCurveColor(int dimension,
                               double r,
                               double g,
                               double b)
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return;
    }
    param->setCurveColor(dimension, r, g, b);
}

void
ParametricParam::getCurveColor(int dimension,
                               ColorTuple& ret) const
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return;
    }
    param->getCurveColor(dimension, &ret.r, &ret.g, &ret.b);
    ret.a = 1.;
}

StatusEnum
ParametricParam::addControlPoint(int dimension,
                                 double key,
                                 double value,
                                 KeyframeTypeEnum interpolation)
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return eStatusFailed;
    }
    return param->addControlPoint(eValueChangedReasonNatronInternalEdited, dimension, key, value, interpolation);
}

StatusEnum
ParametricParam::addControlPoint(int dimension,
                                 double key,
                                 double value,
                                 double leftDerivative,
                                 double rightDerivative,
                                 KeyframeTypeEnum interpolation)
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return eStatusFailed;
    }
    return param->addControlPoint(eValueChangedReasonNatronInternalEdited, dimension, key, value, leftDerivative, rightDerivative, interpolation);
}

double
ParametricParam::getValue(int dimension,
                          double parametricPosition) const
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return 0.;
    }
    double ret;
    StatusEnum stat = param->getValue(dimension, parametricPosition, &ret);

    if (stat == eStatusFailed) {
        ret =  0.;
    }

    return ret;
}

int
ParametricParam::getNControlPoints(int dimension) const
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return 0;
    }
    int ret;
    StatusEnum stat = param->getNControlPoints(dimension, &ret);

    if (stat == eStatusFailed) {
        ret = 0;
    }

    return ret;
}

StatusEnum
ParametricParam::getNthControlPoint(int dimension,
                                    int nthCtl,
                                    double *key,
                                    double *value,
                                    double *leftDerivative,
                                    double *rightDerivative) const
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return eStatusFailed;
    }
    return param->getNthControlPoint(dimension, nthCtl, key, value, leftDerivative, rightDerivative);
}

StatusEnum
ParametricParam::setNthControlPoint(int dimension,
                                    int nthCtl,
                                    double key,
                                    double value,
                                    double leftDerivative,
                                    double rightDerivative)
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return eStatusFailed;
    }
    return param->setNthControlPoint(eValueChangedReasonNatronInternalEdited, dimension, nthCtl, key, value, leftDerivative, rightDerivative);
}

StatusEnum
ParametricParam::setNthControlPointInterpolation(int dimension,
                                                 int nThCtl,
                                                 KeyframeTypeEnum interpolation)
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return eStatusFailed;
    }
    return param->setNthControlPointInterpolation(eValueChangedReasonNatronInternalEdited, dimension, nThCtl, interpolation);
}

StatusEnum
ParametricParam::deleteControlPoint(int dimension,
                                    int nthCtl)
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return eStatusFailed;
    }
    return param->deleteControlPoint(eValueChangedReasonNatronInternalEdited, dimension, nthCtl);
}

StatusEnum
ParametricParam::deleteAllControlPoints(int dimension)
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return eStatusFailed;
    }
    return param->deleteAllControlPoints(eValueChangedReasonNatronInternalEdited, dimension);
}

void
ParametricParam::setDefaultCurvesFromCurrentCurves()
{
    KnobParametricPtr param = _parametricKnob.lock();
    if (!param) {
        return;
    }
    param->setDefaultCurvesFromCurves();
}

NATRON_PYTHON_NAMESPACE_EXIT
NATRON_NAMESPACE_EXIT

