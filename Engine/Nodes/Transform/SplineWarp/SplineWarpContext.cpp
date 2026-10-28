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
#include <set>
#include <stdexcept>
#include <string>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QString>
#include <QtCore/QThread>

#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/ContentHasher.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpAlign.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpCurve.h"

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
    KnobContentWrite write;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        const bool isBoundaryNow = curve && containsCurve(_boundaries, curve.get());
        if (isBoundaryNow == boundary) {
            return;
        }
        if (boundary) {
            if (!ownsCurve(curve)) {
                throw std::invalid_argument("SplineWarp: a boundary must be a curve of this node.");
            }
            if (!SplineWarpCurveShape::isClosedCurve(*curve)) {
                throw std::invalid_argument("SplineWarp: a boundary must be a closed curve.");
            }
            if (findLinkLocked(curve.get())) {
                throw std::invalid_argument("SplineWarp: a linked curve cannot be a boundary.");
            }
            _boundaries.push_back(curve);
        } else {
            for (std::list<BezierWPtr>::iterator it = _boundaries.begin(); it != _boundaries.end(); ++it) {
                if (it->lock() == curve) {
                    _boundaries.erase(it);
                    break;
                }
            }
        }
    }
    warpDataChanged(true);
    rolesChanged();
}

std::list<BezierPtr>
SplineWarpContext::getBoundaries() const
{
    std::list<BezierPtr> ret;
    std::lock_guard<std::mutex> l(_warpMutex);

    for (std::list<BezierWPtr>::const_iterator it = _boundaries.begin(); it != _boundaries.end(); ++it) {
        BezierPtr curve = it->lock();
        if (curve) {
            ret.push_back(curve);
        }
    }

    return ret;
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
    BezierPtr curve = std::dynamic_pointer_cast<Bezier>(item);

    if (!curve) {
        return;
    }
    KnobContentWrite write;
    bool changed = false;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        for (std::list<BezierWPtr>::iterator it = _boundaries.begin(); it != _boundaries.end(); ++it) {
            if (it->lock() == curve) {
                _boundaries.erase(it);
                changed = true;
                break;
            }
        }
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
};
} // anonymous namespace

RotoCurveContextHostStatePtr
SplineWarpContext::saveHostState() const
{
    std::shared_ptr<SplineWarpHostState> state = std::make_shared<SplineWarpHostState>();
    std::lock_guard<std::mutex> l(_warpMutex);

    state->boundaries = _boundaries;
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
    warpDataChanged(true);
    rolesChanged();
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
    for (std::size_t i = 0; i < _links.size(); ++i) {
        const SplineWarpLink& link = *_links[i];
        std::lock_guard<std::mutex> ll(link._mutex);
        BezierPtr source = link._source.lock();
        BezierPtr destination = link._destination.lock();
        if (!source || !destination) {
            continue;
        }
        Serialization::SplineWarpLinkSerialization s;
        linkToSerialization(link, source->getScriptName(), destination->getScriptName(), &s);
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
    }
    _loading = true;
    RotoCurveContext::fromSerialization(serialization);

    if (serialization.hasSplineWarp) {
        restoreWarpData(serialization.splineWarp, nullptr);
    }
    _loading = false;
    warpDataChanged(false);
    rolesChanged();
}

void
SplineWarpContext::linkToSerialization(const SplineWarpLink& link,
                                       const std::string& source,
                                       const std::string& destination,
                                       Serialization::SplineWarpLinkSerialization* serialization)
{
    serialization->source = source;
    serialization->destination = destination;
    serialization->startOffset = link._startOffset;
    serialization->reversed = link._reversed;
    link._amount.toSerialization(&serialization->amount);
    serialization->correspondences.clear();
    for (std::size_t c = 0; c < link._correspondences.size(); ++c) {
        Serialization::SplineWarpCorrespondenceSerialization cs;
        link._correspondences[c].source.toSerialization(&cs.source);
        link._correspondences[c].destination.toSerialization(&cs.destination);
        serialization->correspondences.push_back(cs);
    }
}

void
SplineWarpContext::restoreWarpData(const Serialization::SplineWarpSerialization& warp,
                                   const std::map<std::string, std::string>* names)
{
    NodePtr node = getNode();
    const QString context = node ? QString::fromUtf8(node->getLabel_mt_safe().c_str()) : QString();
    const auto logDropped = [&](const std::string& what) {
        appPTR->writeToErrorLog_mt_safe(context, QDateTime::currentDateTime(), QString::fromUtf8(what.c_str()));
    };

    const auto findCurve = [&](const std::string& name, bool* skip) -> BezierPtr {
        *skip = false;
        if (!names) {
            return std::dynamic_pointer_cast<Bezier>(getItemByName(name));
        }
        std::map<std::string, std::string>::const_iterator found = names->find(name);
        if (found == names->end()) {
            *skip = true;

            return BezierPtr();
        }

        return std::dynamic_pointer_cast<Bezier>(getItemByName(found->second));
    };

    std::lock_guard<std::mutex> l(_warpMutex);
    for (std::list<std::string>::const_iterator it = warp.boundaries.begin(); it != warp.boundaries.end(); ++it) {
        bool skip;
        BezierPtr curve = findCurve(*it, &skip);
        if (skip) {
            continue;
        }
        if (!curve || !SplineWarpCurveShape::isClosedCurve(*curve) || containsCurve(_boundaries, curve.get()) || findLinkLocked(curve.get())) {
            logDropped("SplineWarp: dropped boundary " + *it + ": no closed curve has that name.");
            continue;
        }
        _boundaries.push_back(curve);
    }
    for (std::list<Serialization::SplineWarpLinkSerialization>::const_iterator it = warp.links.begin(); it != warp.links.end(); ++it) {
        bool skipSource, skipDestination;
        BezierPtr source = findCurve(it->source, &skipSource);
        BezierPtr destination = findCurve(it->destination, &skipDestination);
        if (skipSource || skipDestination) {
            continue;
        }
        const std::string name = it->source + " -> " + it->destination;
        if (!source || !destination) {
            logDropped("SplineWarp: dropped link " + name + ": a curve is missing.");
            continue;
        }
        if ((source == destination) || containsCurve(_boundaries, source.get()) || containsCurve(_boundaries, destination.get()) || findLinkLocked(source.get()) || findLinkLocked(destination.get())) {
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
}

namespace {
void
collectCurveNames(const Serialization::RotoLayerSerialization& layer,
                  std::set<std::string>* names)
{
    for (std::list<Serialization::RotoLayerChildSerialization>::const_iterator it = layer.children.begin(); it != layer.children.end(); ++it) {
        if (it->type == Serialization::eRotoLayerChildTypeBezier) {
            names->insert(it->bezier.name);
        } else if (it->type == Serialization::eRotoLayerChildTypeLayer) {
            collectCurveNames(it->layer, names);
        }
    }
}
} // anonymous namespace

std::string
SplineWarpContext::serializeItems(const std::list<RotoItemPtr>& items) const
{
    Serialization::RotoContextSerialization serialization;

    itemsToSerialization(items, &serialization);

    std::set<std::string> curves;
    collectCurveNames(serialization.baseLayer, &curves);

    Serialization::SplineWarpSerialization& warp = serialization.splineWarp;
    serialization.hasSplineWarp = true;
    {
        std::lock_guard<std::mutex> l(_warpMutex);
        for (std::list<BezierWPtr>::const_iterator it = _boundaries.begin(); it != _boundaries.end(); ++it) {
            BezierPtr curve = it->lock();
            if (curve && curves.count(curve->getScriptName())) {
                warp.boundaries.push_back(curve->getScriptName());
            }
        }
        for (std::size_t i = 0; i < _links.size(); ++i) {
            const SplineWarpLink& link = *_links[i];
            std::lock_guard<std::mutex> ll(link._mutex);
            BezierPtr source = link._source.lock();
            BezierPtr destination = link._destination.lock();
            if (!source || !destination || !curves.count(source->getScriptName()) || !curves.count(destination->getScriptName())) {
                continue;
            }
            Serialization::SplineWarpLinkSerialization s;
            linkToSerialization(link, source->getScriptName(), destination->getScriptName(), &s);
            warp.links.push_back(s);
        }
    }

    return encodeItems(serialization);
}

std::list<RotoItemPtr>
SplineWarpContext::pasteItems(const std::string& blob,
                              const RotoLayerPtr& layer,
                              int* droppedStrokes)
{
    Serialization::RotoContextSerialization serialization = decodeItems(blob);
    std::map<std::string, std::string> names;
    std::list<RotoItemPtr> pasted = pasteSerialization(serialization, layer, &names, droppedStrokes);

    if (pasted.empty()) {
        return pasted;
    }
    if (serialization.hasSplineWarp) {
        restoreWarpData(serialization.splineWarp, &names);
    }
    onStructureChanged();
    warpDataChanged(false);
    rolesChanged();
    evaluateChange();

    return pasted;
}

NATRON_NAMESPACE_EXIT
