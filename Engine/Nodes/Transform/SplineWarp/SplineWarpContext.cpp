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

#include "SplineWarpContext.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QThread>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/ContentHasher.h"
#include "Engine/Format.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpAlign.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCurve.h"
#include "Engine/Project.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/Transform.h"

#include "Serialization/RotoSerialization.h"
#include "Serialization/SplineWarpSerialization.h"

NATRON_NAMESPACE_ENTER

// ---------------------------------------------------------------------------------------------
// SplineWarpAnimatedValue

SplineWarpAnimatedValue::SplineWarpAnimatedValue(double value)
    : _value(value)
    , _curve()
{
}

SplineWarpAnimatedValue::SplineWarpAnimatedValue(const SplineWarpAnimatedValue& other)
    : _value(other._value)
    , _curve(other._curve)
{
}

SplineWarpAnimatedValue&
SplineWarpAnimatedValue::operator=(const SplineWarpAnimatedValue& other)
{
    if (this != &other) {
        _value = other._value;
        _curve.clone(other._curve);
    }

    return *this;
}

double
SplineWarpAnimatedValue::getValueAt(double time) const
{
    // An empty curve evaluates to 0, so the static value stands until the first key.
    return _curve.isAnimated() ? _curve.getValueAt(time) : _value;
}

bool
SplineWarpAnimatedValue::isAnimated() const
{
    return _curve.isAnimated();
}

void
SplineWarpAnimatedValue::assign(double value,
                                const Curve& curve)
{
    _value = value;
    _curve.clone(curve);
}

void
SplineWarpAnimatedValue::setValue(double value,
                                  double time)
{
    if (_curve.isAnimated()) {
        setValueAtTime(time, value);
    } else {
        _value = value;
    }
}

void
SplineWarpAnimatedValue::setValueAtTime(double time,
                                        double value)
{
    if (_curve.getKeyFramesCount() > 0) {
        KeyFrame existing;
        if (_curve.getKeyFrameWithTime(time, &existing)) {
            _curve.removeKeyFrameWithTime(time);
        }
    }
    _curve.addKeyFrame(KeyFrame(time, value));
    _value = value;
}

void
SplineWarpAnimatedValue::removeKeyFrame(double time)
{
    KeyFrame existing;

    if (_curve.getKeyFrameWithTime(time, &existing)) {
        // The value stays where the curve was, once it holds no key.
        const double current = _curve.getValueAt(time);
        _curve.removeKeyFrameWithTime(time);
        if (!_curve.isAnimated()) {
            _value = current;
        }
    }
}

std::vector<double>
SplineWarpAnimatedValue::getKeyFrameTimes() const
{
    std::vector<double> times;
    const KeyFrameSet keys = _curve.getKeyFrames_mt_safe();

    times.reserve(keys.size());
    for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        times.push_back(it->getTime());
    }

    return times;
}

void
SplineWarpAnimatedValue::mapValues(const std::function<double(double)>& f)
{
    _value = f(_value);
    const KeyFrameSet keys = _curve.getKeyFrames_mt_safe();
    if (keys.empty()) {
        return;
    }
    _curve.clearKeyFrames();
    for (KeyFrameSet::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        KeyFrame k(*it);
        k.setValue(f(it->getValue()));
        _curve.addKeyFrame(k);
    }
}

void
SplineWarpAnimatedValue::appendContentHash(ContentHasher& h) const
{
    const bool animated = _curve.isAnimated();

    h.appendBool(animated);
    if (animated) {
        _curve.appendContentHash(h);
    } else {
        h.appendDouble(_value);
    }
}

void
SplineWarpAnimatedValue::toSerialization(Serialization::SplineWarpAnimatedValueSerialization* serialization) const
{
    serialization->value = _value;
    serialization->curve = Serialization::CurveSerialization();
    if (_curve.isAnimated()) {
        _curve.toSerialization(&serialization->curve);
    }
}

void
SplineWarpAnimatedValue::fromSerialization(const Serialization::SplineWarpAnimatedValueSerialization& serialization)
{
    _value = serialization.value;
    _curve.clearKeyFrames();
    _curve.fromSerialization(serialization.curve);
}

// ---------------------------------------------------------------------------------------------
// SplineWarpTransform

namespace {
struct TransformKnobEntry {
    const char* name;
    int dimensions;
};

// The item knobs RotoDrawableItem makes, in value order.
const TransformKnobEntry kTransformKnobs[] = {
    { "translate", 2 },
    { "rotate", 1 },
    { "scale", 2 },
    { "uniform", 1 },
    { "skewx", 1 },
    { "skewy", 1 },
    { "skewOrder", 1 },
    { "center", 2 },
    { "extraMatrix", 9 },
};

enum TransformValueIndex {
    eTranslate = 0,
    eRotate = 2,
    eScale = 3,
    eUniform = 5,
    eSkewX = 6,
    eSkewY = 7,
    eSkewOrder = 8,
    eCenter = 9,
    eExtraMatrix = 11,
};
} // anonymous namespace

SplineWarpTransform::SplineWarpTransform()
{
    _values[eScale] = SplineWarpAnimatedValue(1.);
    _values[eScale + 1] = SplineWarpAnimatedValue(1.);
    _values[eUniform] = SplineWarpAnimatedValue(1.);
    _values[eExtraMatrix] = SplineWarpAnimatedValue(1.);
    _values[eExtraMatrix + 4] = SplineWarpAnimatedValue(1.);
    _values[eExtraMatrix + 8] = SplineWarpAnimatedValue(1.);
}

const std::vector<std::string>&
SplineWarpTransform::knobNames()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> ret;
        for (const TransformKnobEntry& entry : kTransformKnobs) {
            ret.push_back(entry.name);
        }

        return ret;
    }();

    return names;
}

int
SplineWarpTransform::indexOf(const std::string& name,
                             int* dimensions)
{
    int index = 0;

    for (const TransformKnobEntry& entry : kTransformKnobs) {
        if (name == entry.name) {
            *dimensions = entry.dimensions;

            return index;
        }
        index += entry.dimensions;
    }

    return -1;
}

void
SplineWarpTransform::getMatrixAt(double time,
                                 Transform::Matrix3x3* matrix) const
{
    const double sx = _values[eScale].getValueAt(time);
    const double sy = (_values[eUniform].getValueAt(time) != 0.) ? sx : _values[eScale + 1].getValueAt(time);
    const bool skewOrderYX = std::lround(_values[eSkewOrder].getValueAt(time)) == 1;

    *matrix = Transform::matTransformCanonical(_values[eTranslate].getValueAt(time), _values[eTranslate + 1].getValueAt(time),
                                               sx, sy, _values[eSkewX].getValueAt(time), _values[eSkewY].getValueAt(time),
                                               skewOrderYX, Transform::toRadians(_values[eRotate].getValueAt(time)),
                                               _values[eCenter].getValueAt(time), _values[eCenter + 1].getValueAt(time));
    Transform::Matrix3x3 extra;
    extra.a = _values[eExtraMatrix].getValueAt(time);
    extra.b = _values[eExtraMatrix + 1].getValueAt(time);
    extra.c = _values[eExtraMatrix + 2].getValueAt(time);
    extra.d = _values[eExtraMatrix + 3].getValueAt(time);
    extra.e = _values[eExtraMatrix + 4].getValueAt(time);
    extra.f = _values[eExtraMatrix + 5].getValueAt(time);
    extra.g = _values[eExtraMatrix + 6].getValueAt(time);
    extra.h = _values[eExtraMatrix + 7].getValueAt(time);
    extra.i = _values[eExtraMatrix + 8].getValueAt(time);
    *matrix = Transform::matMul(*matrix, extra);
}

void
SplineWarpTransform::readKnobs(const std::list<KnobIPtr>& knobs)
{
    for (std::list<KnobIPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        int dimensions = 0;
        const int index = indexOf((*it)->getName(), &dimensions);
        if ((index < 0) || ((*it)->getDimension() != dimensions)) {
            continue;
        }
        Knob<double>* isDouble = dynamic_cast<Knob<double>*>(it->get());
        Knob<bool>* isBool = dynamic_cast<Knob<bool>*>(it->get());
        Knob<int>* isInt = dynamic_cast<Knob<int>*>(it->get());
        for (int d = 0; d < dimensions; ++d) {
            double value = 0.;
            if (isDouble) {
                value = isDouble->getValue(d, ViewIdx(0));
            } else if (isBool) {
                value = isBool->getValue(d, ViewIdx(0)) ? 1. : 0.;
            } else if (isInt) {
                value = isInt->getValue(d, ViewIdx(0));
            }
            const std::shared_ptr<Curve> curve = (*it)->getCurve(ViewIdx(0), d);
            _values[index + d].assign(value, curve ? *curve : Curve());
        }
    }
}

void
SplineWarpTransform::writeKnobs(const std::list<KnobIPtr>& knobs) const
{
    for (std::list<KnobIPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        int dimensions = 0;
        const int index = indexOf((*it)->getName(), &dimensions);
        if ((index < 0) || ((*it)->getDimension() != dimensions)) {
            continue;
        }
        Knob<double>* isDouble = dynamic_cast<Knob<double>*>(it->get());
        Knob<bool>* isBool = dynamic_cast<Knob<bool>*>(it->get());
        Knob<int>* isInt = dynamic_cast<Knob<int>*>(it->get());
        (*it)->beginChanges();
        for (int d = 0; d < dimensions; ++d) {
            const SplineWarpAnimatedValue& value = _values[index + d];
            (*it)->removeAnimation(ViewSpec::all(), d);
            if (isDouble) {
                isDouble->setValue(value.getStaticValue(), ViewSpec::all(), d, true);
            } else if (isBool) {
                isBool->setValue(value.getStaticValue() != 0., ViewSpec::all(), d, true);
            } else if (isInt) {
                isInt->setValue((int)std::lround(value.getStaticValue()), ViewSpec::all(), d, true);
            }
            if (value.isAnimated()) {
                (*it)->cloneCurve(ViewSpec::all(), d, value.getCurve());
            }
        }
        (*it)->endChanges();
    }
}

void
SplineWarpTransform::appendContentHash(ContentHasher& h) const
{
    for (int i = 0; i < kValueCount; ++i) {
        _values[i].appendContentHash(h);
    }
}

// ---------------------------------------------------------------------------------------------
// SplineWarpLink

SplineWarpLink::SplineWarpLink(const BezierPtr& source,
                               const BezierPtr& destination)
    : _mutex()
    , _source(source)
    , _destination(destination)
    , _startOffset(0.)
    , _reversed(false)
    , _amount(1.)
    , _correspondences()
{
}

BezierPtr
SplineWarpLink::getSource() const
{
    std::lock_guard<std::mutex> l(_mutex);

    return _source.lock();
}

BezierPtr
SplineWarpLink::getDestination() const
{
    std::lock_guard<std::mutex> l(_mutex);

    return _destination.lock();
}

double
SplineWarpLink::getStartOffset() const
{
    std::lock_guard<std::mutex> l(_mutex);

    return _startOffset;
}

bool
SplineWarpLink::isReversed() const
{
    std::lock_guard<std::mutex> l(_mutex);

    return _reversed;
}

double
SplineWarpLink::getAmount(double time) const
{
    std::lock_guard<std::mutex> l(_mutex);

    return _amount.getValueAt(time);
}

SplineWarpAnimatedValue
SplineWarpLink::getAmountValue() const
{
    std::lock_guard<std::mutex> l(_mutex);

    return _amount;
}

int
SplineWarpLink::getCorrespondenceCount() const
{
    std::lock_guard<std::mutex> l(_mutex);

    return (int)_correspondences.size();
}

bool
SplineWarpLink::getCorrespondence(int index,
                                  double time,
                                  double* sourceParam,
                                  double* destinationParam) const
{
    std::lock_guard<std::mutex> l(_mutex);

    if ((index < 0) || (index >= (int)_correspondences.size())) {
        return false;
    }
    *sourceParam = _correspondences[index].source.getValueAt(time);
    *destinationParam = _correspondences[index].destination.getValueAt(time);

    return true;
}

std::vector<SplineWarpCorrespondence>
SplineWarpLink::getCorrespondences() const
{
    std::lock_guard<std::mutex> l(_mutex);

    return _correspondences;
}

// ---------------------------------------------------------------------------------------------
// SplineWarpContext

namespace {
void
splitParam(double u,
           int segmentCount,
           int* segment,
           double* t)
{
    if (!(u > 0.) || (segmentCount <= 0)) {
        *segment = 0;
        *t = 0.;

        return;
    }
    if (u >= segmentCount) {
        *segment = segmentCount - 1;
        *t = 1.;

        return;
    }
    *segment = (int)std::floor(u);
    *t = u - *segment;
}

double
clampToSegments(double u,
                int segmentCount)
{
    if (!(u > 0.) || (segmentCount <= 0)) {
        return 0.;
    }

    return std::min(u, (double)segmentCount);
}

void
checkParam(double u,
           const Bezier& curve,
           const char* what)
{
    const int segmentCount = SplineWarpCurveShape::segmentCountOf(curve);

    if (!(u >= 0.) || (u > segmentCount)) {
        throw std::invalid_argument(std::string("SplineWarp: ") + what + " is outside the curve's parameter range.");
    }
}
} // anonymous namespace

struct SplineWarpContext::MakeSharedEnabler
    : public SplineWarpContext {
    MakeSharedEnabler(const NodePtr& node)
        : SplineWarpContext(node)
    {
    }
};

SplineWarpContext::SplineWarpContext(const NodePtr& node)
    : RotoCurveContext(node, false)
    , _warpMutex()
    , _links()
    , _boundaries()
    , _softBoundaries()
    , _cookieCutters()
    , _layerAmounts()
    , _layerTransforms()
    , _transformLinks()
    , _loading(false)
{
}

SplineWarpContextPtr
SplineWarpContext::create(const NodePtr& node)
{
    return std::make_shared<SplineWarpContext::MakeSharedEnabler>(node);
}

SplineWarpContext::~SplineWarpContext()
{
}

bool
SplineWarpContext::ownsCurve(const BezierPtr& curve) const
{
    return curve && (curve->getContext().get() == this);
}

SplineWarpLinkPtr
SplineWarpContext::findLinkLocked(const Bezier* curve) const
{
    for (std::size_t i = 0; i < _links.size(); ++i) {
        std::lock_guard<std::mutex> l(_links[i]->_mutex);
        if ((_links[i]->_source.lock().get() == curve) || (_links[i]->_destination.lock().get() == curve)) {
            return _links[i];
        }
    }

    return SplineWarpLinkPtr();
}

namespace {
bool
containsCurve(const std::list<BezierWPtr>& curves,
              const Bezier* curve)
{
    for (std::list<BezierWPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        if (it->lock().get() == curve) {
            return true;
        }
    }

    return false;
}

bool
eraseCurve(std::list<BezierWPtr>* curves,
           const Bezier* curve)
{
    for (std::list<BezierWPtr>::iterator it = curves->begin(); it != curves->end(); ++it) {
        if (it->lock().get() == curve) {
            curves->erase(it);

            return true;
        }
    }

    return false;
}

std::list<BezierPtr>
lockedCurves(const std::list<BezierWPtr>& curves)
{
    std::list<BezierPtr> ret;

    for (std::list<BezierWPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        BezierPtr curve = it->lock();
        if (curve) {
            ret.push_back(curve);
        }
    }

    return ret;
}

double
clampAmount(double amount)
{
    return std::max(0., std::min(amount, 1.));
}
} // anonymous namespace

void
SplineWarpContext::checkLinkable(const BezierPtr& curve,
                                 const char* what) const
{
    // _warpMutex is held.
    if (!curve) {
        throw std::invalid_argument(std::string("SplineWarp: the ") + what + " curve is null.");
    }
    if (!ownsCurve(curve)) {
        throw std::invalid_argument(std::string("SplineWarp: the ") + what + " curve is not in this node.");
    }
    if (containsCurve(_boundaries, curve.get())) {
        throw std::invalid_argument(std::string("SplineWarp: the ") + what + " curve is a boundary.");
    }
    if (containsCurve(_softBoundaries, curve.get())) {
        throw std::invalid_argument(std::string("SplineWarp: the ") + what + " curve is a soft boundary.");
    }
    if (findLinkLocked(curve.get())) {
        throw std::invalid_argument(std::string("SplineWarp: the ") + what + " curve is already linked.");
    }
}

void
SplineWarpContext::warpDataChanged(bool evaluate)
{
    NodePtr node = getNode();

    if (!node) {
        return;
    }
    node->markHashDirty();
    if (evaluate && !_loading && node->getEffectInstance()) {
        evaluateChange();
    }
}

void
SplineWarpContext::rolesChanged()
{
    // The panel's columns are refreshed by a direct connection, so only from the main thread.
    QCoreApplication* app = QCoreApplication::instance();

    if (app && (QThread::currentThread() == app->thread())) {
        emitItemPanelColumnsChanged(RotoItemPtr());
    }
}

SplineWarpCurveRoleEnum
SplineWarpContext::getRole(const BezierPtr& curve) const
{
    if (!curve) {
        return eSplineWarpCurveRoleUnlinked;
    }
    std::lock_guard<std::mutex> l(_warpMutex);
    if (containsCurve(_boundaries, curve.get())) {
        return eSplineWarpCurveRoleBoundary;
    }
    if (containsCurve(_softBoundaries, curve.get())) {
        return eSplineWarpCurveRoleSoftBoundary;
    }
    if (findLinkLocked(curve.get())) {
        return eSplineWarpCurveRoleLinked;
    }

    return eSplineWarpCurveRoleUnlinked;
}

bool
SplineWarpContext::isBoundary(const BezierPtr& curve) const
{
    return getRole(curve) == eSplineWarpCurveRoleBoundary;
}

void
SplineWarpContext::setBoundary(const BezierPtr& curve,
                               bool boundary)
{
    setBoundaryRole(curve, false, boundary);
}

bool
SplineWarpContext::isSoftBoundary(const BezierPtr& curve) const
{
    return getRole(curve) == eSplineWarpCurveRoleSoftBoundary;
}

void
SplineWarpContext::setSoftBoundary(const BezierPtr& curve,
                                   bool softBoundary)
{
    setBoundaryRole(curve, true, softBoundary);
}

void
SplineWarpContext::setBoundaryRole(const BezierPtr& curve,
                                   bool soft,
                                   bool on)
{
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        std::list<BezierWPtr>& curves = soft ? _softBoundaries : _boundaries;
        const bool isNow = curve && containsCurve(curves, curve.get());
        if (isNow == on) {
            return;
        }
        if (on) {
            if (!ownsCurve(curve)) {
                throw std::invalid_argument(soft ? "SplineWarp: a soft boundary must be a curve of this node." : "SplineWarp: a boundary must be a curve of this node.");
            }
            if (!soft && !SplineWarpCurveShape::isClosedCurve(*curve)) {
                throw std::invalid_argument("SplineWarp: a boundary must be a closed curve.");
            }
            if (findLinkLocked(curve.get())) {
                throw std::invalid_argument(soft ? "SplineWarp: a linked curve cannot be a soft boundary." : "SplineWarp: a linked curve cannot be a boundary.");
            }
            eraseCurve(soft ? &_boundaries : &_softBoundaries, curve.get());
            curves.push_back(curve);
        } else {
            eraseCurve(&curves, curve.get());
        }
    }
    warpDataChanged(true);
    rolesChanged();
}

std::list<BezierPtr>
SplineWarpContext::getBoundaries() const
{
    std::lock_guard<std::mutex> l(_warpMutex);

    return lockedCurves(_boundaries);
}

std::list<BezierPtr>
SplineWarpContext::getSoftBoundaries() const
{
    std::lock_guard<std::mutex> l(_warpMutex);

    return lockedCurves(_softBoundaries);
}

bool
SplineWarpContext::isCookieCutter(const BezierPtr& curve) const
{
    if (!curve) {
        return false;
    }
    std::lock_guard<std::mutex> l(_warpMutex);

    return containsCurve(_cookieCutters, curve.get());
}

void
SplineWarpContext::setCookieCutter(const BezierPtr& curve,
                                   bool cookieCutter)
{
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        const bool isNow = curve && containsCurve(_cookieCutters, curve.get());
        if (isNow == cookieCutter) {
            return;
        }
        if (cookieCutter) {
            if (!ownsCurve(curve)) {
                throw std::invalid_argument("SplineWarp: a cookie cutter must be a curve of this node.");
            }
            if (!SplineWarpCurveShape::isClosedCurve(*curve)) {
                throw std::invalid_argument("SplineWarp: a cookie cutter must be a closed curve.");
            }
            _cookieCutters.push_back(curve);
        } else {
            eraseCurve(&_cookieCutters, curve.get());
        }
    }
    warpDataChanged(true);
    rolesChanged();
}

std::list<BezierPtr>
SplineWarpContext::getCookieCutters() const
{
    std::lock_guard<std::mutex> l(_warpMutex);

    return lockedCurves(_cookieCutters);
}

bool
SplineWarpContext::hasActiveCookieCutter(double time) const
{
    const std::list<BezierPtr> curves = getCookieCutters();

    for (std::list<BezierPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        if ((*it)->isActivated(time) && !(*it)->isDeactivatedRecursive()) {
            return true;
        }
    }

    return false;
}

double
SplineWarpContext::getLayerAmount(const RotoLayerPtr& layer,
                                  double time) const
{
    return getLayerAmountValue(layer).getValueAt(time);
}

SplineWarpAnimatedValue
SplineWarpContext::getLayerAmountValue(const RotoLayerPtr& layer) const
{
    std::lock_guard<std::mutex> l(_warpMutex);

    for (std::list<std::pair<RotoLayerWPtr, SplineWarpAnimatedValue>>::const_iterator it = _layerAmounts.begin(); it != _layerAmounts.end(); ++it) {
        if (layer && (it->first.lock() == layer)) {
            return it->second;
        }
    }

    return SplineWarpAnimatedValue(1.);
}

double
SplineWarpContext::getInheritedLayerAmount(const RotoItemPtr& item,
                                           double time) const
{
    std::vector<RotoLayerPtr> layers;
    for (RotoLayerPtr layer = item ? item->getParentLayer() : RotoLayerPtr(); layer; layer = layer->getParentLayer()) {
        layers.push_back(layer);
    }
    double amount = 1.;
    std::lock_guard<std::mutex> l(_warpMutex);
    for (std::list<std::pair<RotoLayerWPtr, SplineWarpAnimatedValue>>::const_iterator it = _layerAmounts.begin(); it != _layerAmounts.end(); ++it) {
        if (std::find(layers.begin(), layers.end(), it->first.lock()) != layers.end()) {
            amount *= it->second.getValueAt(time);
        }
    }

    return amount;
}

SplineWarpAnimatedValue&
SplineWarpContext::layerAmountLocked(const RotoLayerPtr& layer)
{
    // _warpMutex is held.
    if (!layer || (layer->getContext().get() != this)) {
        throw std::invalid_argument("SplineWarp: the layer is not in this node.");
    }
    for (std::list<std::pair<RotoLayerWPtr, SplineWarpAnimatedValue>>::iterator it = _layerAmounts.begin(); it != _layerAmounts.end(); ++it) {
        if (it->first.lock() == layer) {
            return it->second;
        }
    }
    _layerAmounts.push_back(std::make_pair(RotoLayerWPtr(layer), SplineWarpAnimatedValue(1.)));

    return _layerAmounts.back().second;
}

void
SplineWarpContext::setLayerAmount(const RotoLayerPtr& layer,
                                  double amount,
                                  double time)
{
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        layerAmountLocked(layer).setValue(clampAmount(amount), time);
    }
    warpDataChanged(true);
}

void
SplineWarpContext::setLayerAmountAtTime(const RotoLayerPtr& layer,
                                        double time,
                                        double amount)
{
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        layerAmountLocked(layer).setValueAtTime(time, clampAmount(amount));
    }
    warpDataChanged(true);
}

void
SplineWarpContext::assignLayerAmounts(const std::list<RotoLayerPtr>& layers,
                                      double value,
                                      const Curve& curve)
{
    if (layers.empty()) {
        return;
    }
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        for (std::list<RotoLayerPtr>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
            layerAmountLocked(*it).assign(clampAmount(value), curve);
        }
    }
    warpDataChanged(true);
}

bool
SplineWarpContext::ownsItem(const RotoItemPtr& item) const
{
    return item && (item->getContext().get() == this) && (std::dynamic_pointer_cast<RotoLayer>(item) || std::dynamic_pointer_cast<RotoDrawableItem>(item));
}

const SplineWarpTransform*
SplineWarpContext::findLayerTransformLocked(const RotoItem* layer) const
{
    for (std::list<std::pair<RotoLayerWPtr, SplineWarpTransform>>::const_iterator it = _layerTransforms.begin(); it != _layerTransforms.end(); ++it) {
        if (it->first.lock().get() == layer) {
            return &it->second;
        }
    }

    return 0;
}

bool
SplineWarpContext::transformLinkReachesLocked(const RotoItem* from,
                                              const RotoItem* to) const
{
    const RotoItem* current = from;

    for (std::size_t steps = 0; current && (steps <= _transformLinks.size()); ++steps) {
        if (current == to) {
            return true;
        }
        const RotoItem* next = 0;
        for (std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>>::const_iterator it = _transformLinks.begin(); it != _transformLinks.end(); ++it) {
            if (it->first.lock().get() == current) {
                next = it->second.lock().get();
                break;
            }
        }
        current = next;
    }

    return false;
}

RotoItemPtr
SplineWarpContext::resolveTransformLinkLocked(const RotoItem* item) const
{
    RotoItemPtr source;
    const RotoItem* current = item;
    // Links never form a cycle; the bound only guards against a bug turning into a hang.
    for (std::size_t steps = 0; steps <= _transformLinks.size(); ++steps) {
        RotoItemPtr next;
        for (std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>>::const_iterator it = _transformLinks.begin(); it != _transformLinks.end(); ++it) {
            if (it->first.lock().get() == current) {
                next = it->second.lock();
                break;
            }
        }
        if (!next) {
            break;
        }
        source = next;
        current = next.get();
    }

    return source;
}

SplineWarpTransform
SplineWarpContext::getLayerTransform(const RotoLayerPtr& layer) const
{
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        if (const SplineWarpTransform* stored = findLayerTransformLocked(layer.get())) {
            return *stored;
        }
    }
    SplineWarpTransform transform;
    double x, y;
    if (getDefaultTransformCenter(getTimelineCurrentTime(), &x, &y)) {
        transform.value(eCenter).assign(x, Curve());
        transform.value(eCenter + 1).assign(y, Curve());
    }

    return transform;
}

void
SplineWarpContext::setLayerTransform(const RotoLayerPtr& layer,
                                     const SplineWarpTransform& transform)
{
    assignLayerTransforms(std::list<RotoLayerPtr>(1, layer), transform);
}

void
SplineWarpContext::assignLayerTransforms(const std::list<RotoLayerPtr>& layers,
                                         const SplineWarpTransform& transform)
{
    for (std::list<RotoLayerPtr>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (!*it || ((*it)->getContext().get() != this)) {
            throw std::invalid_argument("SplineWarp: the layer is not in this node.");
        }
    }
    if (layers.empty()) {
        return;
    }
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        for (std::list<RotoLayerPtr>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
            bool found = false;
            for (std::list<std::pair<RotoLayerWPtr, SplineWarpTransform>>::iterator lt = _layerTransforms.begin(); lt != _layerTransforms.end(); ++lt) {
                if (lt->first.lock() == *it) {
                    lt->second = transform;
                    found = true;
                    break;
                }
            }
            if (!found) {
                _layerTransforms.push_back(std::make_pair(RotoLayerWPtr(*it), transform));
            }
        }
    }
    warpDataChanged(true);
}

RotoItemPtr
SplineWarpContext::getTransformLink(const RotoItemPtr& item) const
{
    std::lock_guard<std::mutex> l(_warpMutex);

    for (std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>>::const_iterator it = _transformLinks.begin(); it != _transformLinks.end(); ++it) {
        if (item && (it->first.lock() == item)) {
            return it->second.lock();
        }
    }

    return RotoItemPtr();
}

std::list<RotoItemPtr>
SplineWarpContext::getTransformLinkedItems(const RotoItemPtr& target) const
{
    std::list<RotoItemPtr> items;
    std::lock_guard<std::mutex> l(_warpMutex);

    for (std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>>::const_iterator it = _transformLinks.begin(); it != _transformLinks.end(); ++it) {
        RotoItemPtr item = it->first.lock();
        if (item && target && (it->second.lock() == target)) {
            items.push_back(item);
        }
    }

    return items;
}

RotoItemPtr
SplineWarpContext::getTransformSource(const RotoItemPtr& item) const
{
    std::lock_guard<std::mutex> l(_warpMutex);
    RotoItemPtr source = resolveTransformLinkLocked(item.get());

    return source ? source : item;
}

SplineWarpTransform
SplineWarpContext::getOwnTransform(const RotoItemPtr& item) const
{
    if (RotoLayerPtr layer = std::dynamic_pointer_cast<RotoLayer>(item)) {
        return getLayerTransform(layer);
    }
    SplineWarpTransform transform;
    if (RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(item)) {
        transform.readKnobs(drawable->getKnobs());
    }

    return transform;
}

void
SplineWarpContext::setOwnTransform(const RotoItemPtr& item,
                                   const SplineWarpTransform& transform)
{
    if (RotoLayerPtr layer = std::dynamic_pointer_cast<RotoLayer>(item)) {
        setLayerTransform(layer, transform);
    } else if (RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(item)) {
        transform.writeKnobs(drawable->getKnobs());
    }
}

namespace {
bool
isInsideLayer(const RotoItemPtr& item,
              const RotoItemPtr& layer)
{
    for (RotoLayerPtr parent = item->getParentLayer(); parent; parent = parent->getParentLayer()) {
        if (parent == layer) {
            return true;
        }
    }

    return false;
}
} // anonymous namespace

std::string
SplineWarpContext::getTransformLinkRefusal(const RotoItemPtr& item,
                                           const RotoItemPtr& target) const
{
    if (!ownsItem(item) || !ownsItem(target)) {
        return "SplineWarp: a transform link joins two curves or layers of this node.";
    }
    if (item == target) {
        return "SplineWarp: an item cannot follow its own transform.";
    }
    // The parent layers of item apply on top of what it follows, so one inside the other would
    // apply a transform twice.
    if (isInsideLayer(item, target) || isInsideLayer(target, item)) {
        return "SplineWarp: " + item->getLabel() + " and " + target->getLabel() + " are one inside the other.";
    }
    std::lock_guard<std::mutex> l(_warpMutex);
    if (transformLinkReachesLocked(target.get(), item.get())) {
        return "SplineWarp: " + target->getLabel() + " already follows the transform of " + item->getLabel() + ".";
    }

    return std::string();
}

void
SplineWarpContext::linkTransform(const RotoItemPtr& item,
                                 const RotoItemPtr& target)
{
    const std::string refusal = getTransformLinkRefusal(item, target);

    if (!refusal.empty()) {
        throw std::invalid_argument(refusal);
    }
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        if (transformLinkReachesLocked(target.get(), item.get())) {
            throw std::invalid_argument("SplineWarp: " + target->getLabel() + " already follows the transform of " + item->getLabel() + ".");
        }
        bool found = false;
        for (std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>>::iterator it = _transformLinks.begin(); it != _transformLinks.end(); ++it) {
            if (it->first.lock() == item) {
                it->second = target;
                found = true;
                break;
            }
        }
        if (!found) {
            _transformLinks.push_back(std::make_pair(std::weak_ptr<RotoItem>(item), std::weak_ptr<RotoItem>(target)));
        }
    }
    transformLinksChanged();
}

void
SplineWarpContext::unlinkTransform(const RotoItemPtr& item)
{
    RotoItemPtr source;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        source = resolveTransformLinkLocked(item.get());
    }
    if (!source) {
        return;
    }
    KnobContentWrite write;
    // Outside _warpMutex: a curve's values live in its knobs.
    setOwnTransform(item, getOwnTransform(source));
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        for (std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>>::iterator it = _transformLinks.begin(); it != _transformLinks.end(); ++it) {
            if (it->first.lock() == item) {
                _transformLinks.erase(it);
                break;
            }
        }
    }
    transformLinksChanged();
}

void
SplineWarpContext::transformLinksChanged()
{
    warpDataChanged(true);
    rolesChanged();
    refreshSelectedTransformTies();
}

void
SplineWarpContext::refreshSelectedTransformTies()
{
    QCoreApplication* app = QCoreApplication::instance();

    if (!app || (QThread::currentThread() != app->thread())) {
        return;
    }
    const std::list<RotoItemPtr> selected = getSelectedItems();
    for (std::list<RotoItemPtr>::const_iterator it = selected.begin(); it != selected.end(); ++it) {
        if (RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(*it)) {
            refreshTransformKnobTies(drawable);
        }
    }
}

bool
SplineWarpContext::tiesTransformKnobs(const RotoDrawableItem& item) const
{
    std::lock_guard<std::mutex> l(_warpMutex);

    return !resolveTransformLinkLocked(&item);
}

bool
SplineWarpContext::getDefaultTransformCenter(double time,
                                             double* x,
                                             double* y) const
{
    NodePtr node = getNode();
    SplineWarp* effect = node ? dynamic_cast<SplineWarp*>(node->getEffectInstance().get()) : 0;
    RectD format = effect ? effect->getSourceFormat(time, ViewIdx(0)) : RectD();

    if (format.isNull() && node && node->getApp()) {
        Format projectFormat;
        node->getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
        format = projectFormat.toCanonicalFormat();
    }
    if (format.isNull()) {
        return false;
    }
    *x = (format.x1 + format.x2) / 2.;
    *y = (format.y1 + format.y2) / 2.;

    return true;
}

void
SplineWarpContext::onItemCreated(const RotoDrawableItemPtr& item,
                                 bool connectNodes)
{
    RotoCurveContext::onItemCreated(item, connectNodes);
    // A loaded or copied item brings its own centre afterwards.
    if (item && !_loading) {
        item->resetTransformCenter();
    }
}

void
SplineWarpContext::composeItemTransform(const RotoDrawableItem& item,
                                        double time,
                                        const Transform::Matrix3x3& local,
                                        Transform::Matrix3x3* matrix) const
{
    std::vector<RotoLayerPtr> layers;
    for (RotoLayerPtr layer = item.getParentLayer(); layer; layer = layer->getParentLayer()) {
        layers.push_back(layer);
    }
    // Snapshot under _warpMutex; a curve source's knobs are read after it is released.
    RotoItemPtr itemSource;
    std::vector<RotoItemPtr> layerSources(layers.size());
    std::vector<SplineWarpTransform> layerValues(layers.size());
    std::vector<bool> layerIsStored(layers.size(), false);
    SplineWarpTransform itemValues;
    bool itemIsStored = false;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        itemSource = resolveTransformLinkLocked(&item);
        if (itemSource) {
            if (const SplineWarpTransform* stored = findLayerTransformLocked(itemSource.get())) {
                itemValues = *stored;
                itemIsStored = true;
            }
        }
        for (std::size_t i = 0; i < layers.size(); ++i) {
            RotoItemPtr source = resolveTransformLinkLocked(layers[i].get());
            layerSources[i] = source ? source : RotoItemPtr(layers[i]);
            if (const SplineWarpTransform* stored = findLayerTransformLocked(layerSources[i].get())) {
                layerValues[i] = *stored;
                layerIsStored[i] = true;
            }
        }
    }
    const auto ownMatrix = [time](const RotoItemPtr& source, const SplineWarpTransform& values, bool stored, Transform::Matrix3x3* m) {
        RotoDrawableItem* drawable = dynamic_cast<RotoDrawableItem*>(source.get());
        if (drawable) {
            drawable->getLocalTransformAtTime(time, m);
        } else if (stored) {
            values.getMatrixAt(time, m);
        } else {
            // A layer without a stored transform.
            m->setIdentity();
        }
    };

    if (itemSource) {
        ownMatrix(itemSource, itemValues, itemIsStored, matrix);
    } else {
        *matrix = local;
    }
    for (std::size_t i = 0; i < layers.size(); ++i) {
        Transform::Matrix3x3 layerMatrix;
        ownMatrix(layerSources[i], layerValues[i], layerIsStored[i], &layerMatrix);
        *matrix = Transform::matMul(layerMatrix, *matrix);
    }
}

SplineWarpLinkPtr
SplineWarpContext::link(const BezierPtr& source,
                        const BezierPtr& destination,
                        bool duplicate)
{
    SplineWarpAlignment alignment;

    if (!duplicate && source && destination) {
        alignment = computeSplineWarpAlignment(*source, *destination, getTimelineCurrentTime());
    }

    return linkWithAlignment(source, destination, alignment.startOffset, alignment.reversed);
}

SplineWarpLinkPtr
SplineWarpContext::linkWithAlignment(const BezierPtr& source,
                                     const BezierPtr& destination,
                                     double startOffset,
                                     bool reversed)
{
    KnobContentWrite write;
    SplineWarpLinkPtr ret;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        checkLinkable(source, "source");
        checkLinkable(destination, "destination");
        if (source == destination) {
            throw std::invalid_argument("SplineWarp: a curve cannot be linked to itself.");
        }
        if (!SplineWarpCurveShape::isClosedCurve(*destination)) {
            startOffset = 0.;
        }
        checkParam(startOffset, *destination, "the start offset");
        ret = std::make_shared<SplineWarpLink>(source, destination);
        ret->_startOffset = startOffset;
        ret->_reversed = reversed;
        _links.push_back(ret);
    }
    warpDataChanged(true);
    rolesChanged();

    return ret;
}

void
SplineWarpContext::unlink(const BezierPtr& curve)
{
    SplineWarpLinkPtr found;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        found = findLinkLocked(curve.get());
    }
    if (found) {
        removeLink(found);
    }
}

void
SplineWarpContext::removeLink(const SplineWarpLinkPtr& link)
{
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        std::vector<SplineWarpLinkPtr>::iterator it = std::find(_links.begin(), _links.end(), link);
        if (it == _links.end()) {
            return;
        }
        _links.erase(it);
    }
    warpDataChanged(true);
    rolesChanged();
}

SplineWarpLinkPtr
SplineWarpContext::getLink(const BezierPtr& curve) const
{
    if (!curve) {
        return SplineWarpLinkPtr();
    }
    std::lock_guard<std::mutex> l(_warpMutex);

    return findLinkLocked(curve.get());
}

std::vector<SplineWarpLinkPtr>
SplineWarpContext::getLinks() const
{
    std::lock_guard<std::mutex> l(_warpMutex);

    return _links;
}

void
SplineWarpContext::setLinkAlignment(const SplineWarpLinkPtr& link,
                                    double startOffset,
                                    bool reversed)
{
    if (!link) {
        return;
    }
    KnobContentWrite write;
    {
        BezierPtr destination = link->getDestination();
        if (!destination) {
            return;
        }
        if (!SplineWarpCurveShape::isClosedCurve(*destination)) {
            startOffset = 0.;
        }
        checkParam(startOffset, *destination, "the start offset");
        std::lock_guard<std::mutex> l(link->_mutex);
        if ((link->_startOffset == startOffset) && (link->_reversed == reversed)) {
            return;
        }
        link->_startOffset = startOffset;
        link->_reversed = reversed;
    }
    warpDataChanged(true);
}

void
SplineWarpContext::swapLinkCurves(const SplineWarpLinkPtr& link,
                                  double startOffset,
                                  bool reversed)
{
    if (!link) {
        return;
    }
    KnobContentWrite write;
    {
        BezierPtr source = link->getSource();
        BezierPtr destination = link->getDestination();
        if (!source || !destination) {
            throw std::invalid_argument("SplineWarp: the link has lost a curve.");
        }
        if (!SplineWarpCurveShape::isClosedCurve(*source)) {
            startOffset = 0.;
        }
        checkParam(startOffset, *source, "the start offset");
        std::lock_guard<std::mutex> l(link->_mutex);
        std::swap(link->_source, link->_destination);
        for (std::size_t i = 0; i < link->_correspondences.size(); ++i) {
            std::swap(link->_correspondences[i].source, link->_correspondences[i].destination);
        }
        link->_startOffset = startOffset;
        link->_reversed = reversed;
    }
    warpDataChanged(true);
    rolesChanged();
}

void
SplineWarpContext::setLinkAmount(const SplineWarpLinkPtr& link,
                                 double amount,
                                 double time)
{
    if (!link) {
        return;
    }
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(link->_mutex);
        link->_amount.setValue(std::max(0., std::min(amount, 1.)), time);
    }
    warpDataChanged(true);
}

void
SplineWarpContext::setLinkAmountAtTime(const SplineWarpLinkPtr& link,
                                       double time,
                                       double amount)
{
    if (!link) {
        return;
    }
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(link->_mutex);
        link->_amount.setValueAtTime(time, std::max(0., std::min(amount, 1.)));
    }
    warpDataChanged(true);
}

void
SplineWarpContext::assignLinkAmounts(const std::vector<SplineWarpLinkPtr>& links,
                                     double value,
                                     const Curve& curve)
{
    if (links.empty()) {
        return;
    }
    KnobContentWrite write;
    for (std::size_t i = 0; i < links.size(); ++i) {
        if (links[i]) {
            std::lock_guard<std::mutex> l(links[i]->_mutex);
            links[i]->_amount.assign(clampAmount(value), curve);
        }
    }
    warpDataChanged(true);
}

int
SplineWarpContext::addCorrespondence(const SplineWarpLinkPtr& link,
                                     double sourceParam,
                                     double destinationParam)
{
    if (!link) {
        throw std::invalid_argument("SplineWarp: the link is null.");
    }
    BezierPtr source = link->getSource();
    BezierPtr destination = link->getDestination();
    if (!source || !destination) {
        throw std::invalid_argument("SplineWarp: the link has lost a curve.");
    }
    checkParam(sourceParam, *source, "the source parameter");
    checkParam(destinationParam, *destination, "the destination parameter");

    KnobContentWrite write;
    int index;
    {
        std::lock_guard<std::mutex> l(link->_mutex);
        SplineWarpCorrespondence c;
        c.source = SplineWarpAnimatedValue(sourceParam);
        c.destination = SplineWarpAnimatedValue(destinationParam);
        link->_correspondences.push_back(c);
        index = (int)link->_correspondences.size() - 1;
    }
    warpDataChanged(true);

    return index;
}

void
SplineWarpContext::setCorrespondence(const SplineWarpLinkPtr& link,
                                     int index,
                                     double sourceParam,
                                     double destinationParam,
                                     double time)
{
    if (!link) {
        throw std::invalid_argument("SplineWarp: the link is null.");
    }
    BezierPtr source = link->getSource();
    BezierPtr destination = link->getDestination();
    if (!source || !destination) {
        throw std::invalid_argument("SplineWarp: the link has lost a curve.");
    }
    checkParam(sourceParam, *source, "the source parameter");
    checkParam(destinationParam, *destination, "the destination parameter");

    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(link->_mutex);
        if ((index < 0) || (index >= (int)link->_correspondences.size())) {
            throw std::invalid_argument("SplineWarp: correspondence index out of range.");
        }
        link->_correspondences[index].source.setValue(sourceParam, time);
        link->_correspondences[index].destination.setValue(destinationParam, time);
    }
    warpDataChanged(true);
}

void
SplineWarpContext::setCorrespondenceAtTime(const SplineWarpLinkPtr& link,
                                           int index,
                                           double time,
                                           double sourceParam,
                                           double destinationParam)
{
    if (!link) {
        throw std::invalid_argument("SplineWarp: the link is null.");
    }
    BezierPtr source = link->getSource();
    BezierPtr destination = link->getDestination();
    if (!source || !destination) {
        throw std::invalid_argument("SplineWarp: the link has lost a curve.");
    }
    checkParam(sourceParam, *source, "the source parameter");
    checkParam(destinationParam, *destination, "the destination parameter");

    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(link->_mutex);
        if ((index < 0) || (index >= (int)link->_correspondences.size())) {
            throw std::invalid_argument("SplineWarp: correspondence index out of range.");
        }
        link->_correspondences[index].source.setValueAtTime(time, sourceParam);
        link->_correspondences[index].destination.setValueAtTime(time, destinationParam);
    }
    warpDataChanged(true);
}

void
SplineWarpContext::removeCorrespondence(const SplineWarpLinkPtr& link,
                                        int index)
{
    if (!link) {
        return;
    }
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(link->_mutex);
        if ((index < 0) || (index >= (int)link->_correspondences.size())) {
            return;
        }
        link->_correspondences.erase(link->_correspondences.begin() + index);
    }
    warpDataChanged(true);
}

double
SplineWarpContext::paramAfterInsertion(double u,
                                       int pointCount,
                                       bool closed,
                                       int afterIndex,
                                       double t)
{
    const int n = pointCount;

    if (n <= 0) {
        return 0.;
    }
    if (n == 1) {
        // The single point keeps parameter 0 whichever side the new one lands.
        return (afterIndex == -1) ? 1. : 0.;
    }
    const int segmentCount = closed ? n : n - 1;
    int j;
    double s;
    splitParam(u, segmentCount, &j, &s);

    // Where parameter s of the split segment lands among the two halves, from the first half's
    // start.
    const auto splitAt = [t](double x) -> double {
        if (t >= 1.) {
            return x;
        }
        if (t <= 0.) {
            return 1. + x;
        }
        if (x < t) {
            return x / t;
        }

        return 1. + (x - t) / (1. - t);
    };

    if (afterIndex == -1) {
        // The new point goes to the front. On a closed curve it splits the closing segment, whose
        // first half becomes the new closing segment n and the second half segment 0.
        if (!closed) {
            return j + 1. + s;
        }
        if (j < n - 1) {
            return j + 1. + s;
        }
        const double v = splitAt(s);

        return (v < 1.) ? n + v : v - 1.;
    }

    const int i = afterIndex;
    if (j < i) {
        return j + s;
    }
    if (j == i) {
        return i + splitAt(s);
    }

    return j + 1. + s;
}

double
SplineWarpContext::paramAfterRemoval(double u,
                                     int pointCount,
                                     bool closed,
                                     int index,
                                     double firstWeight)
{
    const int n = pointCount;

    if (n - 1 <= 1) {
        return 0.;
    }
    const int segmentCount = closed ? n : n - 1;
    const int newSegmentCount = closed ? n - 1 : n - 2;
    int j;
    double s;
    splitParam(u, segmentCount, &j, &s);
    const double w = firstWeight;
    const int k = index;
    double v;

    if (!closed && (k == 0)) {
        // Segment 0 goes; its points collapse onto the new first point.
        v = (j == 0) ? 0. : j - 1. + s;
    } else if (!closed && (k == n - 1)) {
        // The last segment goes; its points collapse onto the new last point.
        v = (j == n - 2) ? (double)newSegmentCount : j + s;
    } else if (closed && (k == 0)) {
        // The closing segment n - 1 and segment 0 merge into the new closing segment n - 2.
        if (j == n - 1) {
            v = (n - 2) + w * s;
        } else if (j == 0) {
            v = (n - 2) + w + (1. - w) * s;
        } else {
            v = j - 1. + s;
        }
    } else {
        // Segments k - 1 and k merge into segment k - 1.
        if (j < k - 1) {
            v = j + s;
        } else if (j == k - 1) {
            v = (k - 1) + w * s;
        } else if (j == k) {
            v = (k - 1) + w + (1. - w) * s;
        } else {
            v = j - 1. + s;
        }
    }

    return clampToSegments(v, newSegmentCount);
}

void
SplineWarpContext::remapCurveParams(const BezierPtr& curve,
                                    const std::function<double(double)>& f)
{
    std::vector<SplineWarpLinkPtr> links;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        links = _links;
    }
    const bool closed = SplineWarpCurveShape::isClosedCurve(*curve);
    for (std::size_t i = 0; i < links.size(); ++i) {
        const SplineWarpLinkPtr& link = links[i];
        std::lock_guard<std::mutex> l(link->_mutex);
        const bool isSource = (link->_source.lock() == curve);
        const bool isDestination = (link->_destination.lock() == curve);
        if (!isSource && !isDestination) {
            continue;
        }
        for (std::size_t c = 0; c < link->_correspondences.size(); ++c) {
            SplineWarpAnimatedValue& v = isSource ? link->_correspondences[c].source : link->_correspondences[c].destination;
            v.mapValues(f);
        }
        if (isDestination) {
            link->_startOffset = closed ? f(link->_startOffset) : 0.;
        }
    }
}

void
SplineWarpContext::onControlPointInserted(const BezierPtr& curve,
                                          int afterIndex,
                                          double t)
{
    if (!curve || !getLink(curve)) {
        return;
    }
    KnobContentWrite write;
    // The point is in already.
    const int pointCount = curve->getControlPointsCount() - 1;
    const bool closed = SplineWarpCurveShape::isClosedCurve(*curve);
    remapCurveParams(curve, [=](double u) {
        return paramAfterInsertion(u, pointCount, closed, afterIndex, t);
    });
    warpDataChanged(false);
}

void
SplineWarpContext::onControlPointAboutToBeRemoved(const BezierPtr& curve,
                                                  int index)
{
    if (!curve || !getLink(curve)) {
        return;
    }
    const int pointCount = curve->getControlPointsCount();
    if ((index < 0) || (index >= pointCount)) {
        return;
    }
    const bool closed = SplineWarpCurveShape::isClosedCurve(*curve);

    // The merged segment splits its parameter range by the arc lengths of the two it replaces,
    // as they are at the current time.
    double firstWeight = 0.5;
    if (pointCount > 2) {
        const SplineWarpCurveShape shape = SplineWarpCurveShape::fromBezier(*curve, getTimelineCurrentTime());
        const int segmentCount = shape.getSegmentCount();
        const int first = (closed && index == 0) ? segmentCount - 1 : index - 1;
        const int second = index;
        if ((first >= 0) && (second < segmentCount)) {
            const double a = shape.getSegmentLength(first);
            const double b = shape.getSegmentLength(second);
            if (a + b > 0.) {
                firstWeight = a / (a + b);
            }
        }
    }

    KnobContentWrite write;
    remapCurveParams(curve, [=](double u) {
        return paramAfterRemoval(u, pointCount, closed, index, firstWeight);
    });
    warpDataChanged(false);
}

void
SplineWarpContext::onCurveFinishedChanged(const BezierPtr& curve)
{
    if (!curve || _loading) {
        return;
    }
    KnobContentWrite write;
    bool changed = false;
    if (!SplineWarpCurveShape::isClosedCurve(*curve)) {
        std::lock_guard<std::mutex> l(_warpMutex);
        for (std::list<BezierWPtr>::iterator it = _boundaries.begin(); it != _boundaries.end(); ++it) {
            if (it->lock() == curve) {
                _boundaries.erase(it);
                changed = true;
                break;
            }
        }
        changed = eraseCurve(&_cookieCutters, curve.get()) || changed;
    }
    if (getLink(curve)) {
        // Opening drops the closing segment; closing adds one, so every parameter stays valid.
        const int segmentCount = SplineWarpCurveShape::segmentCountOf(*curve);
        remapCurveParams(curve, [segmentCount](double u) {
            return clampToSegments(u, segmentCount);
        });
        changed = true;
    }
    if (changed) {
        warpDataChanged(true);
        rolesChanged();
    }
}

void
SplineWarpContext::onItemRemoved(const RotoItemPtr& item)
{
    bool linksChanged = false;
    {
        // Items that followed item's transform go back to their own.
        KnobContentWrite write;
        std::lock_guard<std::mutex> l(_warpMutex);
        for (std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>>::iterator it = _transformLinks.begin(); it != _transformLinks.end();) {
            if ((it->first.lock() == item) || (it->second.lock() == item)) {
                it = _transformLinks.erase(it);
                linksChanged = true;
            } else {
                ++it;
            }
        }
    }
    if (linksChanged) {
        transformLinksChanged();
    }
    RotoLayerPtr layer = std::dynamic_pointer_cast<RotoLayer>(item);
    if (layer) {
        KnobContentWrite write;
        bool changed = false;
        {
            std::lock_guard<std::mutex> l(_warpMutex);
            for (std::list<std::pair<RotoLayerWPtr, SplineWarpAnimatedValue>>::iterator it = _layerAmounts.begin(); it != _layerAmounts.end(); ++it) {
                if (it->first.lock() == layer) {
                    _layerAmounts.erase(it);
                    changed = true;
                    break;
                }
            }
            for (std::list<std::pair<RotoLayerWPtr, SplineWarpTransform>>::iterator it = _layerTransforms.begin(); it != _layerTransforms.end(); ++it) {
                if (it->first.lock() == layer) {
                    _layerTransforms.erase(it);
                    changed = true;
                    break;
                }
            }
        }
        if (changed) {
            warpDataChanged(true);
        }

        return;
    }
    BezierPtr curve = std::dynamic_pointer_cast<Bezier>(item);

    if (!curve) {
        return;
    }
    KnobContentWrite write;
    bool changed = false;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        changed = eraseCurve(&_boundaries, curve.get());
        changed = eraseCurve(&_softBoundaries, curve.get()) || changed;
        changed = eraseCurve(&_cookieCutters, curve.get()) || changed;
        SplineWarpLinkPtr link = findLinkLocked(curve.get());
        if (link) {
            _links.erase(std::find(_links.begin(), _links.end(), link));
            changed = true;
        }
    }
    if (changed) {
        warpDataChanged(true);
        rolesChanged();
    }
}

namespace {
struct SplineWarpHostState
    : public RotoCurveContextHostState {
    struct SavedLink {
        SplineWarpLinkPtr link;
        BezierWPtr source;
        BezierWPtr destination;
        double startOffset;
        bool reversed;
        SplineWarpAnimatedValue amount;
        std::vector<SplineWarpCorrespondence> correspondences;
    };

    std::vector<SavedLink> links;
    std::list<BezierWPtr> boundaries;
    std::list<BezierWPtr> softBoundaries;
    std::list<BezierWPtr> cookieCutters;
    std::list<std::pair<RotoLayerWPtr, SplineWarpAnimatedValue>> layerAmounts;
    std::list<std::pair<RotoLayerWPtr, SplineWarpTransform>> layerTransforms;
    std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>> transformLinks;
};
} // anonymous namespace

RotoCurveContextHostStatePtr
SplineWarpContext::saveHostState() const
{
    std::shared_ptr<SplineWarpHostState> state = std::make_shared<SplineWarpHostState>();
    std::lock_guard<std::mutex> l(_warpMutex);

    state->boundaries = _boundaries;
    state->softBoundaries = _softBoundaries;
    state->cookieCutters = _cookieCutters;
    state->layerAmounts = _layerAmounts;
    state->layerTransforms = _layerTransforms;
    state->transformLinks = _transformLinks;
    for (std::size_t i = 0; i < _links.size(); ++i) {
        const SplineWarpLink& link = *_links[i];
        std::lock_guard<std::mutex> ll(link._mutex);
        SplineWarpHostState::SavedLink saved;
        saved.link = _links[i];
        saved.source = link._source;
        saved.destination = link._destination;
        saved.startOffset = link._startOffset;
        saved.reversed = link._reversed;
        saved.amount = link._amount;
        saved.correspondences = link._correspondences;
        state->links.push_back(saved);
    }

    return state;
}

void
SplineWarpContext::restoreHostState(const RotoCurveContextHostStatePtr& state)
{
    const SplineWarpHostState* saved = dynamic_cast<const SplineWarpHostState*>(state.get());

    if (!saved) {
        return;
    }
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        _boundaries = saved->boundaries;
        _softBoundaries = saved->softBoundaries;
        _cookieCutters = saved->cookieCutters;
        _layerAmounts = saved->layerAmounts;
        _layerTransforms = saved->layerTransforms;
        _transformLinks = saved->transformLinks;
        _links.clear();
        for (std::size_t i = 0; i < saved->links.size(); ++i) {
            const SplineWarpHostState::SavedLink& entry = saved->links[i];
            {
                std::lock_guard<std::mutex> ll(entry.link->_mutex);
                entry.link->_source = entry.source;
                entry.link->_destination = entry.destination;
                entry.link->_startOffset = entry.startOffset;
                entry.link->_reversed = entry.reversed;
                entry.link->_amount = entry.amount;
                entry.link->_correspondences = entry.correspondences;
            }
            _links.push_back(entry.link);
        }
    }
    transformLinksChanged();
}

void
SplineWarpContext::appendHostContentHash(ContentHasher& h) const
{
    std::lock_guard<std::mutex> l(_warpMutex);

    h.appendString("SplineWarp");
    h.appendU64((U64)_boundaries.size());
    for (std::list<BezierWPtr>::const_iterator it = _boundaries.begin(); it != _boundaries.end(); ++it) {
        BezierPtr curve = it->lock();
        h.appendString(curve ? curve->getScriptName() : std::string());
    }
    h.appendU64((U64)_softBoundaries.size());
    for (std::list<BezierWPtr>::const_iterator it = _softBoundaries.begin(); it != _softBoundaries.end(); ++it) {
        BezierPtr curve = it->lock();
        h.appendString(curve ? curve->getScriptName() : std::string());
    }
    h.appendU64((U64)_cookieCutters.size());
    for (std::list<BezierWPtr>::const_iterator it = _cookieCutters.begin(); it != _cookieCutters.end(); ++it) {
        BezierPtr curve = it->lock();
        h.appendString(curve ? curve->getScriptName() : std::string());
    }
    h.appendU64((U64)_layerAmounts.size());
    for (std::list<std::pair<RotoLayerWPtr, SplineWarpAnimatedValue>>::const_iterator it = _layerAmounts.begin(); it != _layerAmounts.end(); ++it) {
        RotoLayerPtr layer = it->first.lock();
        h.appendString(layer ? layer->getScriptName() : std::string());
        it->second.appendContentHash(h);
    }
    h.appendU64((U64)_layerTransforms.size());
    for (std::list<std::pair<RotoLayerWPtr, SplineWarpTransform>>::const_iterator it = _layerTransforms.begin(); it != _layerTransforms.end(); ++it) {
        RotoLayerPtr layer = it->first.lock();
        h.appendString(layer ? layer->getScriptName() : std::string());
        it->second.appendContentHash(h);
    }
    h.appendU64((U64)_transformLinks.size());
    for (std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>>::const_iterator it = _transformLinks.begin(); it != _transformLinks.end(); ++it) {
        RotoItemPtr item = it->first.lock();
        RotoItemPtr target = it->second.lock();
        h.appendString(item ? item->getScriptName() : std::string());
        h.appendString(target ? target->getScriptName() : std::string());
    }
    h.appendU64((U64)_links.size());
    for (std::size_t i = 0; i < _links.size(); ++i) {
        const SplineWarpLink& link = *_links[i];
        std::lock_guard<std::mutex> ll(link._mutex);
        BezierPtr source = link._source.lock();
        BezierPtr destination = link._destination.lock();
        h.appendString(source ? source->getScriptName() : std::string());
        h.appendString(destination ? destination->getScriptName() : std::string());
        h.appendDouble(link._startOffset);
        h.appendBool(link._reversed);
        link._amount.appendContentHash(h);
        h.appendU64((U64)link._correspondences.size());
        for (std::size_t c = 0; c < link._correspondences.size(); ++c) {
            link._correspondences[c].source.appendContentHash(h);
            link._correspondences[c].destination.appendContentHash(h);
        }
    }
}

void
SplineWarpContext::toSerialization(Serialization::RotoContextSerialization* serialization) const
{
    RotoCurveContext::toSerialization(serialization);

    Serialization::SplineWarpSerialization& warp = serialization->splineWarp;
    warp = Serialization::SplineWarpSerialization();
    serialization->hasSplineWarp = true;

    std::lock_guard<std::mutex> l(_warpMutex);
    for (std::list<BezierWPtr>::const_iterator it = _boundaries.begin(); it != _boundaries.end(); ++it) {
        BezierPtr curve = it->lock();
        if (curve) {
            warp.boundaries.push_back(curve->getScriptName());
        }
    }
    for (std::list<BezierWPtr>::const_iterator it = _softBoundaries.begin(); it != _softBoundaries.end(); ++it) {
        BezierPtr curve = it->lock();
        if (curve) {
            warp.softBoundaries.push_back(curve->getScriptName());
        }
    }
    for (std::list<BezierWPtr>::const_iterator it = _cookieCutters.begin(); it != _cookieCutters.end(); ++it) {
        BezierPtr curve = it->lock();
        if (curve) {
            warp.cookieCutters.push_back(curve->getScriptName());
        }
    }
    for (std::list<std::pair<RotoLayerWPtr, SplineWarpAnimatedValue>>::const_iterator it = _layerAmounts.begin(); it != _layerAmounts.end(); ++it) {
        RotoLayerPtr layer = it->first.lock();
        if (layer) {
            Serialization::SplineWarpLayerAmountSerialization s;
            s.layer = layer->getScriptName();
            it->second.toSerialization(&s.amount);
            warp.layerAmounts.push_back(s);
        }
    }
    const std::vector<std::string>& names = SplineWarpTransform::knobNames();
    for (std::list<std::pair<RotoLayerWPtr, SplineWarpTransform>>::const_iterator it = _layerTransforms.begin(); it != _layerTransforms.end(); ++it) {
        RotoLayerPtr layer = it->first.lock();
        if (!layer) {
            continue;
        }
        Serialization::SplineWarpLayerTransformSerialization s;
        s.layer = layer->getScriptName();
        for (std::size_t n = 0; n < names.size(); ++n) {
            int dimensions = 0;
            const int index = SplineWarpTransform::indexOf(names[n], &dimensions);
            for (int d = 0; d < dimensions; ++d) {
                Serialization::SplineWarpTransformValueSerialization v;
                v.knob = names[n];
                v.dimension = d;
                it->second.value(index + d).toSerialization(&v.value);
                s.values.push_back(v);
            }
        }
        warp.layerTransforms.push_back(s);
    }
    for (std::list<std::pair<std::weak_ptr<RotoItem>, std::weak_ptr<RotoItem>>>::const_iterator it = _transformLinks.begin(); it != _transformLinks.end(); ++it) {
        RotoItemPtr item = it->first.lock();
        RotoItemPtr target = it->second.lock();
        if (item && target) {
            Serialization::SplineWarpTransformLinkSerialization s;
            s.item = item->getScriptName();
            s.target = target->getScriptName();
            warp.transformLinks.push_back(s);
        }
    }
    for (std::size_t i = 0; i < _links.size(); ++i) {
        const SplineWarpLink& link = *_links[i];
        std::lock_guard<std::mutex> ll(link._mutex);
        BezierPtr source = link._source.lock();
        BezierPtr destination = link._destination.lock();
        if (!source || !destination) {
            continue;
        }
        Serialization::SplineWarpLinkSerialization s;
        s.source = source->getScriptName();
        s.destination = destination->getScriptName();
        s.startOffset = link._startOffset;
        s.reversed = link._reversed;
        link._amount.toSerialization(&s.amount);
        for (std::size_t c = 0; c < link._correspondences.size(); ++c) {
            Serialization::SplineWarpCorrespondenceSerialization cs;
            link._correspondences[c].source.toSerialization(&cs.source);
            link._correspondences[c].destination.toSerialization(&cs.destination);
            s.correspondences.push_back(cs);
        }
        warp.links.push_back(s);
    }
}

void
SplineWarpContext::fromSerialization(const Serialization::RotoContextSerialization& serialization)
{
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        _links.clear();
        _boundaries.clear();
        _softBoundaries.clear();
        _cookieCutters.clear();
        _layerAmounts.clear();
        _layerTransforms.clear();
        _transformLinks.clear();
    }
    _loading = true;
    RotoCurveContext::fromSerialization(serialization);

    NodePtr node = getNode();
    const QString context = node ? QString::fromUtf8(node->getLabel_mt_safe().c_str()) : QString();
    const auto logDropped = [&](const std::string& what) {
        appPTR->writeToErrorLog_mt_safe(context, QDateTime::currentDateTime(), QString::fromUtf8(what.c_str()));
    };

    const Serialization::SplineWarpSerialization& warp = serialization.splineWarp;
    if (serialization.hasSplineWarp) {
        std::lock_guard<std::mutex> l(_warpMutex);
        for (std::list<std::string>::const_iterator it = warp.boundaries.begin(); it != warp.boundaries.end(); ++it) {
            BezierPtr curve = std::dynamic_pointer_cast<Bezier>(getItemByName(*it));
            if (!curve || !SplineWarpCurveShape::isClosedCurve(*curve) || containsCurve(_boundaries, curve.get())) {
                logDropped("SplineWarp: dropped boundary " + *it + ": no closed curve has that name.");
                continue;
            }
            _boundaries.push_back(curve);
        }
        for (std::list<std::string>::const_iterator it = warp.softBoundaries.begin(); it != warp.softBoundaries.end(); ++it) {
            BezierPtr curve = std::dynamic_pointer_cast<Bezier>(getItemByName(*it));
            if (!curve || containsCurve(_boundaries, curve.get()) || containsCurve(_softBoundaries, curve.get())) {
                logDropped("SplineWarp: dropped soft boundary " + *it + ": no curve without another boundary role has that name.");
                continue;
            }
            _softBoundaries.push_back(curve);
        }
        for (std::list<std::string>::const_iterator it = warp.cookieCutters.begin(); it != warp.cookieCutters.end(); ++it) {
            BezierPtr curve = std::dynamic_pointer_cast<Bezier>(getItemByName(*it));
            if (!curve || !SplineWarpCurveShape::isClosedCurve(*curve) || containsCurve(_cookieCutters, curve.get())) {
                logDropped("SplineWarp: dropped cookie cutter " + *it + ": no closed curve has that name.");
                continue;
            }
            _cookieCutters.push_back(curve);
        }
        for (std::list<Serialization::SplineWarpLayerAmountSerialization>::const_iterator it = warp.layerAmounts.begin(); it != warp.layerAmounts.end(); ++it) {
            RotoLayerPtr layer = std::dynamic_pointer_cast<RotoLayer>(getItemByName(it->layer));
            if (!layer) {
                logDropped("SplineWarp: dropped the amount of layer " + it->layer + ": no layer has that name.");
                continue;
            }
            SplineWarpAnimatedValue amount(1.);
            amount.fromSerialization(it->amount);
            _layerAmounts.push_back(std::make_pair(RotoLayerWPtr(layer), amount));
        }
        for (std::list<Serialization::SplineWarpLinkSerialization>::const_iterator it = warp.links.begin(); it != warp.links.end(); ++it) {
            BezierPtr source = std::dynamic_pointer_cast<Bezier>(getItemByName(it->source));
            BezierPtr destination = std::dynamic_pointer_cast<Bezier>(getItemByName(it->destination));
            const std::string name = it->source + " -> " + it->destination;
            if (!source || !destination) {
                logDropped("SplineWarp: dropped link " + name + ": a curve is missing.");
                continue;
            }
            if ((source == destination) || containsCurve(_boundaries, source.get()) || containsCurve(_boundaries, destination.get()) || containsCurve(_softBoundaries, source.get()) || containsCurve(_softBoundaries, destination.get()) || findLinkLocked(source.get()) || findLinkLocked(destination.get())) {
                logDropped("SplineWarp: dropped link " + name + ": a curve is a boundary or in another link.");
                continue;
            }
            SplineWarpLinkPtr link = std::make_shared<SplineWarpLink>(source, destination);
            link->_startOffset = clampToSegments(it->startOffset, SplineWarpCurveShape::segmentCountOf(*destination));
            link->_reversed = it->reversed;
            link->_amount.fromSerialization(it->amount);
            for (std::list<Serialization::SplineWarpCorrespondenceSerialization>::const_iterator c = it->correspondences.begin(); c != it->correspondences.end(); ++c) {
                SplineWarpCorrespondence corr;
                corr.source.fromSerialization(c->source);
                corr.destination.fromSerialization(c->destination);
                link->_correspondences.push_back(corr);
            }
            _links.push_back(link);
        }
        for (std::list<Serialization::SplineWarpLayerTransformSerialization>::const_iterator it = warp.layerTransforms.begin(); it != warp.layerTransforms.end(); ++it) {
            RotoLayerPtr layer = std::dynamic_pointer_cast<RotoLayer>(getItemByName(it->layer));
            if (!layer || findLayerTransformLocked(layer.get())) {
                logDropped("SplineWarp: dropped the transform of layer " + it->layer + ": no other layer has that name.");
                continue;
            }
            SplineWarpTransform transform;
            for (std::list<Serialization::SplineWarpTransformValueSerialization>::const_iterator v = it->values.begin(); v != it->values.end(); ++v) {
                int dimensions = 0;
                const int index = SplineWarpTransform::indexOf(v->knob, &dimensions);
                if ((index < 0) || (v->dimension < 0) || (v->dimension >= dimensions)) {
                    logDropped("SplineWarp: dropped value " + v->knob + "." + std::to_string(v->dimension) + " of the transform of layer " + it->layer + ".");
                    continue;
                }
                transform.value(index + v->dimension).fromSerialization(v->value);
            }
            _layerTransforms.push_back(std::make_pair(RotoLayerWPtr(layer), transform));
        }
        for (std::list<Serialization::SplineWarpTransformLinkSerialization>::const_iterator it = warp.transformLinks.begin(); it != warp.transformLinks.end(); ++it) {
            RotoItemPtr item = getItemByName(it->item);
            RotoItemPtr target = getItemByName(it->target);
            const std::string name = it->item + " -> " + it->target;
            if (!ownsItem(item) || !ownsItem(target) || (item == target) || resolveTransformLinkLocked(item.get())) {
                logDropped("SplineWarp: dropped transform link " + name + ": an item is missing or already linked.");
                continue;
            }
            if (transformLinkReachesLocked(target.get(), item.get())) {
                logDropped("SplineWarp: dropped transform link " + name + ": it would form a cycle.");
                continue;
            }
            _transformLinks.push_back(std::make_pair(std::weak_ptr<RotoItem>(item), std::weak_ptr<RotoItem>(target)));
        }
    }
    _loading = false;
    warpDataChanged(false);
    rolesChanged();
}

NATRON_NAMESPACE_EXIT
