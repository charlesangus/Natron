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

#include "BezierCPPrivate.h"
#include "BezierCP.h"

#include <algorithm> // min, max
#include <limits>
#include <set>
#include <list>
#include <utility>
#include <cassert>
#include <stdexcept>

#include <QThread>
#include <QCoreApplication>
#include <QDebug>

#include "Engine/Bezier.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/Transform.h" // Point3D
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

// Was eKeyframeTypeLinear until Natron 2.3.15.
#define BEZIER_KEYFRAME_INTERPOLATION_TYPE eKeyframeTypeSmooth

////////////////////////////////////ControlPoint////////////////////////////////////

BezierCP::BezierCP()
    : _imp( new BezierCPPrivate( BezierPtr() ) )
{
}

BezierCP::BezierCP(const BezierCP & other)
    : _imp( new BezierCPPrivate( other._imp->holder.lock() ) )
{
    clone(other);
}

BezierCP::BezierCP(const BezierPtr& curve)
    : _imp( new BezierCPPrivate(curve) )
{
}

BezierCP::~BezierCP()
{
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

// The track the link follows, when it still exists, belongs to its tracker and the tracker
// node is active.
TrackMarkerPtr
resolveLinkedMarker(const BezierCPTrackLink& link)
{
    TrackMarkerPtr marker = link.marker.lock();
    if (!marker) {
        return TrackMarkerPtr();
    }
    TrackerContextPtr context = marker->getContext();
    NodePtr node = context ? context->getNode() : NodePtr();
    if (!node || !node->isActivated()) {
        return TrackMarkerPtr();
    }
    std::vector<TrackMarkerPtr> markers;
    context->getAllMarkers(&markers);
    if (std::find(markers.begin(), markers.end(), marker) == markers.end()) {
        return TrackMarkerPtr();
    }

    return marker;
}

bool
trackCentreAtTime(const TrackMarkerPtr& marker,
                  double time,
                  double* x,
                  double* y)
{
    KnobDoublePtr centre = marker ? marker->getCenterKnob() : KnobDoublePtr();
    if (!centre) {
        return false;
    }
    *x = centre->getValueAtTime(time, 0);
    *y = centre->getValueAtTime(time, 1);

    return true;
}

void
linkCentreAtTime(const BezierCPTrackLink& link,
                 double time,
                 double* x,
                 double* y)
{
    if (!trackCentreAtTime(resolveLinkedMarker(link), time, x, y)) {
        *x = link.frozenX;
        *y = link.frozenY;
    }
}

void
offsetOf(const BezierCPTrackOffsets& offsets,
         int which,
         double* x,
         double* y)
{
    switch (which) {
    case 0:
        *x = offsets.x;
        *y = offsets.y;
        break;
    case 1:
        *x = offsets.leftX;
        *y = offsets.leftY;
        break;
    default:
        *x = offsets.rightX;
        *y = offsets.rightY;
        break;
    }
}

void
setOffsetOf(BezierCPTrackOffsets* offsets,
            int which,
            double x,
            double y)
{
    switch (which) {
    case 0:
        offsets->x = x;
        offsets->y = y;
        break;
    case 1:
        offsets->leftX = x;
        offsets->leftY = y;
        break;
    default:
        offsets->rightX = x;
        offsets->rightY = y;
        break;
    }
}

// Replaces the stored value at time with the given one, as the unlinked setters always did:
// the position and right tangent write the GUI curves too unless editing them alone, the left
// tangent writes only the curves asked for.
void
keyStoredPoint(BezierCPPrivate& imp,
               bool useGuiCurves,
               int which,
               double time,
               double x,
               double y)
{
    CurvePtr curveX, curveY, guiCurveX, guiCurveY;

    switch (which) {
    case 0:
        curveX = imp.curveX;
        curveY = imp.curveY;
        guiCurveX = imp.guiCurveX;
        guiCurveY = imp.guiCurveY;
        break;
    case 1:
        curveX = imp.curveLeftBezierX;
        curveY = imp.curveLeftBezierY;
        guiCurveX = imp.guiCurveLeftBezierX;
        guiCurveY = imp.guiCurveLeftBezierY;
        break;
    default:
        curveX = imp.curveRightBezierX;
        curveY = imp.curveRightBezierY;
        guiCurveX = imp.guiCurveRightBezierX;
        guiCurveY = imp.guiCurveRightBezierY;
        break;
    }

    KeyFrame kx(time, x);
    kx.setInterpolation(BEZIER_KEYFRAME_INTERPOLATION_TYPE);
    KeyFrame ky(time, y);
    ky.setInterpolation(BEZIER_KEYFRAME_INTERPOLATION_TYPE);
    if (!useGuiCurves) {
        curveX->addKeyFrame(kx);
        curveY->addKeyFrame(ky);
    }
    if (useGuiCurves || (which != 1)) {
        guiCurveX->addKeyFrame(kx);
        guiCurveY->addKeyFrame(ky);
    }
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

bool
BezierCP::getStoredPointAtTime(bool useGuiCurves,
                               int which,
                               double time,
                               double* x,
                               double* y) const
{
    Curve* xCurve;
    Curve* yCurve;

    switch (which) {
    case 0:
        xCurve = useGuiCurves ? _imp->guiCurveX.get() : _imp->curveX.get();
        yCurve = useGuiCurves ? _imp->guiCurveY.get() : _imp->curveY.get();
        break;
    case 1:
        xCurve = useGuiCurves ? _imp->guiCurveLeftBezierX.get() : _imp->curveLeftBezierX.get();
        yCurve = useGuiCurves ? _imp->guiCurveLeftBezierY.get() : _imp->curveLeftBezierY.get();
        break;
    default:
        xCurve = useGuiCurves ? _imp->guiCurveRightBezierX.get() : _imp->curveRightBezierX.get();
        yCurve = useGuiCurves ? _imp->guiCurveRightBezierY.get() : _imp->curveRightBezierY.get();
        break;
    }

    KeyFrame k;
    if (xCurve->getKeyFrameWithTime(time, &k)) {
        *x = k.getValue();
        bool ok = yCurve->getKeyFrameWithTime(time, &k);
        assert(ok);
        if (ok) {
            *y = k.getValue();

            return true;
        }

        return false;
    }
    if (xCurve->isAnimated()) {
        *x = xCurve->getValueAt(time);
        *y = yCurve->getValueAt(time);

        return false;
    }

    QMutexLocker l(&_imp->staticPositionMutex);
    switch (which) {
    case 0:
        *x = useGuiCurves ? _imp->guiX : _imp->x;
        *y = useGuiCurves ? _imp->guiY : _imp->y;
        break;
    case 1:
        *x = useGuiCurves ? _imp->guiLeftX : _imp->leftX;
        *y = useGuiCurves ? _imp->guiLeftY : _imp->leftY;
        break;
    default:
        *x = useGuiCurves ? _imp->guiRightX : _imp->rightX;
        *y = useGuiCurves ? _imp->guiRightY : _imp->rightY;
        break;
    }

    return false;
}

void
BezierCP::setLinkedPointAtTime(bool useGuiCurves,
                               int which,
                               double time,
                               double x,
                               double y)
{
    BezierCPTrackLinkConstPtr link;
    {
        QMutexLocker l(&_imp->staticPositionMutex);
        link = _imp->trackLink;
    }
    if (!link) {
        return;
    }

    // Keep a key where the other points get one, holding the point's own value.
    double storedX, storedY;
    getStoredPointAtTime(useGuiCurves, which, time, &storedX, &storedY);
    keyStoredPoint(*_imp, useGuiCurves, which, time, storedX, storedY);

    double cx, cy;
    linkCentreAtTime(*link, time, &cx, &cy);

    QMutexLocker l(&_imp->staticPositionMutex);
    if (!useGuiCurves) {
        setOffsetOf(&_imp->trackOffsets, which, x - cx, y - cy);
    }
    if (useGuiCurves || (which != 1)) {
        setOffsetOf(&_imp->guiTrackOffsets, which, x - cx, y - cy);
    }
}

void
BezierCP::setLinkedStaticPoint(bool useGuiCurves,
                               int which,
                               double x,
                               double y)
{
    BezierCPTrackLinkConstPtr link;
    {
        QMutexLocker l(&_imp->staticPositionMutex);
        link = _imp->trackLink;
    }
    if (!link) {
        return;
    }

    double time = 0.;
    BezierPtr holder = _imp->holder.lock();
    RotoCurveContextPtr context = holder ? holder->getContext() : RotoCurveContextPtr();
    if (context) {
        time = context->getTimelineCurrentTime();
    }

    double cx, cy;
    linkCentreAtTime(*link, time, &cx, &cy);

    QMutexLocker l(&_imp->staticPositionMutex);
    if (!useGuiCurves) {
        setOffsetOf(&_imp->trackOffsets, which, x - cx, y - cy);
    }
    setOffsetOf(&_imp->guiTrackOffsets, which, x - cx, y - cy);
}

bool
BezierCP::getPositionAtTime(bool useGuiCurves,
                            double time,
                            ViewIdx /*view*/,
                            double* x,
                            double* y) const
{
    bool ret = getStoredPointAtTime(useGuiCurves, 0, time, x, y);

    BezierCPTrackLinkConstPtr link;
    double ox = 0., oy = 0.;
    {
        QMutexLocker l(&_imp->staticPositionMutex);
        link = _imp->trackLink;
        offsetOf(useGuiCurves ? _imp->guiTrackOffsets : _imp->trackOffsets, 0, &ox, &oy);
    }
    if (link) {
        double cx, cy;
        linkCentreAtTime(*link, time, &cx, &cy);
        *x = cx + ox;
        *y = cy + oy;
    }

    return ret;
}

void
BezierCP::setPositionAtTime(bool useGuiCurves,
                            double time,
                            double x,
                            double y)
{
    if (isLinkedToTrack()) {
        setLinkedPointAtTime(useGuiCurves, 0, time, x, y);

        return;
    }
    keyStoredPoint(*_imp, useGuiCurves, 0, time, x, y);
}

void
BezierCP::setStaticPosition(bool useGuiCurves,
                            double x,
                            double y)
{
    if (isLinkedToTrack()) {
        setLinkedStaticPoint(useGuiCurves, 0, x, y);

        return;
    }
    QMutexLocker l(&_imp->staticPositionMutex);

    if (!useGuiCurves) {
        _imp->x = x;
        _imp->y = y;
    }
    _imp->guiX = x;
    _imp->guiY = y;
}

void
BezierCP::setLeftBezierStaticPosition(bool useGuiCurves,
                                      double x,
                                      double y)
{
    if (isLinkedToTrack()) {
        setLinkedStaticPoint(useGuiCurves, 1, x, y);

        return;
    }
    QMutexLocker l(&_imp->staticPositionMutex);

    if (!useGuiCurves) {
        _imp->leftX = x;
        _imp->leftY = y;
    }
    _imp->guiLeftX = x;
    _imp->guiLeftY = y;
}

void
BezierCP::setRightBezierStaticPosition(bool useGuiCurves,
                                       double x,
                                       double y)
{
    if (isLinkedToTrack()) {
        setLinkedStaticPoint(useGuiCurves, 2, x, y);

        return;
    }
    QMutexLocker l(&_imp->staticPositionMutex);

    if (!useGuiCurves) {
        _imp->rightX = x;
        _imp->rightY = y;
    }
    _imp->guiRightX = x;
    _imp->guiRightY = y;
}

bool
BezierCP::getLeftBezierPointAtTime(bool useGuiCurves,
                                   double time,
                                   ViewIdx view,
                                   double* x,
                                   double* y,
                                   bool reAlign) const
{
    bool ret = getStoredPointAtTime(useGuiCurves, 1, time, x, y);

    BezierCPTrackLinkConstPtr link;
    double ox = 0., oy = 0.;
    {
        QMutexLocker l(&_imp->staticPositionMutex);
        link = _imp->trackLink;
        offsetOf(useGuiCurves ? _imp->guiTrackOffsets : _imp->trackOffsets, 1, &ox, &oy);
    }
    if (link) {
        double cx, cy;
        linkCentreAtTime(*link, time, &cx, &cy);
        *x = cx + ox;
        *y = cy + oy;
    }

    // If the CP does not have broken tangents, realign the Bezier points.
    // Fixes https://github.com/NatronGitHub/Natron/issues/202
    if (reAlign && !_imp->broken) {
        double px, py, qx, qy;
        getPositionAtTime(useGuiCurves, time, view, &px, &py);
        getRightBezierPointAtTime(useGuiCurves, time, view, &qx, &qy, false);
        // The vector between the two bezier points
        double vx = qx - *x;
        double vy = qy - *y;
        double v = std::sqrt(vx * vx + vy *vy);
        if (v > 0.) {
            // The normal vector
            double nx = vy / v;
            double ny = -vx / v;
            // The (signed) distance from the position to the segment
            double d = nx * (px - *x) + ny * (py - *y);
            // displace the key point in the normal direction.
            *x += d * nx;
            *y += d * ny;
            // Verify that d is now zero
            // d = nx * (px - *x) + ny * (py - *y);
            // qDebug() << 'd' << d;
        }
    }

    return ret;
} // BezierCP::getLeftBezierPointAtTime

bool
BezierCP::getRightBezierPointAtTime(bool useGuiCurves,
                                    double time,
                                    ViewIdx view,
                                    double *x,
                                    double *y,
                                    bool reAlign) const
{
    bool ret = getStoredPointAtTime(useGuiCurves, 2, time, x, y);

    BezierCPTrackLinkConstPtr link;
    double ox = 0., oy = 0.;
    {
        QMutexLocker l(&_imp->staticPositionMutex);
        link = _imp->trackLink;
        offsetOf(useGuiCurves ? _imp->guiTrackOffsets : _imp->trackOffsets, 2, &ox, &oy);
    }
    if (link) {
        double cx, cy;
        linkCentreAtTime(*link, time, &cx, &cy);
        *x = cx + ox;
        *y = cy + oy;
    }

    // If the CP does not have broken tangents, realign the Bezier points.
    // Fixes https://github.com/NatronGitHub/Natron/issues/202
    if (reAlign && !_imp->broken) {
        double px, py, qx, qy;
        getPositionAtTime(useGuiCurves, time, view, &px, &py);
        getLeftBezierPointAtTime(useGuiCurves, time, view, &qx, &qy, false);
        // The vector between the two bezier points
        double vx = qx - *x;
        double vy = qy - *y;
        double v = std::sqrt(vx * vx + vy *vy);
        if (v > 0.) {
            // The normal vector
            double nx = vy / v;
            double ny = -vx / v;
            // The (signed) distance from the position to the segment
            double d = nx * (px - *x) + ny * (py - *y);
            // displace the key point in the normal direction.
            *x += d * nx;
            *y += d * ny;
            // Verify that d is now zero
            // d = nx * (px - *x) + ny * (py - *y);
            // qDebug() << 'd' << d;
        }
    }

    return ret;
} // BezierCP::getRightBezierPointAtTime

void
BezierCP::setLeftBezierPointAtTime(bool useGuiCurves,
                                   double time,
                                   double x,
                                   double y)
{
    if (isLinkedToTrack()) {
        setLinkedPointAtTime(useGuiCurves, 1, time, x, y);

        return;
    }
    keyStoredPoint(*_imp, useGuiCurves, 1, time, x, y);
}

void
BezierCP::setRightBezierPointAtTime(bool useGuiCurves,
                                    double time,
                                    double x,
                                    double y)
{
    if (isLinkedToTrack()) {
        setLinkedPointAtTime(useGuiCurves, 2, time, x, y);

        return;
    }
    keyStoredPoint(*_imp, useGuiCurves, 2, time, x, y);
}

void
BezierCP::setBroken(bool broken)
{
    _imp->broken = broken;
}

bool
BezierCP::getBroken() const
{
    return _imp->broken;
}

void
BezierCP::removeAnimation(bool useGuiCurves,
                          double currentTime)
{
    {
        QMutexLocker k(&_imp->staticPositionMutex);
        try {
            if (!useGuiCurves) {
                _imp->x = _imp->curveX->getValueAt(currentTime);
                _imp->y = _imp->curveY->getValueAt(currentTime);
                _imp->leftX = _imp->curveLeftBezierX->getValueAt(currentTime);
                _imp->leftY = _imp->curveLeftBezierY->getValueAt(currentTime);
                _imp->rightX = _imp->curveRightBezierX->getValueAt(currentTime);
                _imp->rightY = _imp->curveRightBezierY->getValueAt(currentTime);
            }
            _imp->guiX = _imp->guiCurveX->getValueAt(currentTime);
            _imp->guiY = _imp->guiCurveY->getValueAt(currentTime);
            _imp->guiLeftX = _imp->guiCurveLeftBezierX->getValueAt(currentTime);
            _imp->guiLeftY = _imp->guiCurveLeftBezierY->getValueAt(currentTime);
            _imp->guiRightX = _imp->guiCurveRightBezierX->getValueAt(currentTime);
            _imp->guiRightY = _imp->guiCurveRightBezierY->getValueAt(currentTime);
        } catch (const std::exception & e) {
            //
        }
    }
    if (!useGuiCurves) {
        _imp->curveX->clearKeyFrames();
        _imp->curveY->clearKeyFrames();
        _imp->curveLeftBezierX->clearKeyFrames();
        _imp->curveRightBezierX->clearKeyFrames();
        _imp->curveLeftBezierY->clearKeyFrames();
        _imp->curveRightBezierY->clearKeyFrames();
    } else {
        _imp->guiCurveX->clearKeyFrames();
        _imp->guiCurveY->clearKeyFrames();
        _imp->guiCurveLeftBezierX->clearKeyFrames();
        _imp->guiCurveRightBezierX->clearKeyFrames();
        _imp->guiCurveLeftBezierY->clearKeyFrames();
        _imp->guiCurveRightBezierY->clearKeyFrames();
    }
}

void
BezierCP::removeKeyframe(bool useGuiCurves,
                         double time)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    ///if the keyframe count reaches 0 update the "static" values which may be fetched
    if (_imp->curveX->getKeyFramesCount() == 1) {
        QMutexLocker l(&_imp->staticPositionMutex);
        if (!useGuiCurves) {
            _imp->x = _imp->curveX->getValueAt(time);
            _imp->y = _imp->curveY->getValueAt(time);
            _imp->leftX = _imp->curveLeftBezierX->getValueAt(time);
            _imp->leftY = _imp->curveLeftBezierY->getValueAt(time);
            _imp->rightX = _imp->curveRightBezierX->getValueAt(time);
            _imp->rightY = _imp->curveRightBezierY->getValueAt(time);
        }
        _imp->guiX = _imp->guiCurveX->getValueAt(time);
        _imp->guiY = _imp->guiCurveY->getValueAt(time);
        _imp->guiLeftX = _imp->guiCurveLeftBezierX->getValueAt(time);
        _imp->guiLeftY = _imp->guiCurveLeftBezierY->getValueAt(time);
        _imp->guiRightX = _imp->guiCurveRightBezierX->getValueAt(time);
        _imp->guiRightY = _imp->guiCurveRightBezierY->getValueAt(time);
    }

    try {
        if (!useGuiCurves) {
            _imp->curveX->removeKeyFrameWithTime(time);
            _imp->curveY->removeKeyFrameWithTime(time);
            _imp->curveLeftBezierX->removeKeyFrameWithTime(time);
            _imp->curveRightBezierX->removeKeyFrameWithTime(time);
            _imp->curveLeftBezierY->removeKeyFrameWithTime(time);
            _imp->curveRightBezierY->removeKeyFrameWithTime(time);
        }
        _imp->guiCurveX->removeKeyFrameWithTime(time);
        _imp->guiCurveY->removeKeyFrameWithTime(time);
        _imp->guiCurveLeftBezierX->removeKeyFrameWithTime(time);
        _imp->guiCurveRightBezierX->removeKeyFrameWithTime(time);
        _imp->guiCurveLeftBezierY->removeKeyFrameWithTime(time);
        _imp->guiCurveRightBezierY->removeKeyFrameWithTime(time);
    } catch (...) {
    }
}

bool
BezierCP::hasKeyFrameAtTime(bool useGuiCurves,
                            double time) const
{
    KeyFrame k;

    if (!useGuiCurves) {
        return _imp->curveX->getKeyFrameWithTime(time, &k);
    } else {
        return _imp->guiCurveX->getKeyFrameWithTime(time, &k);
    }
}

void
BezierCP::getKeyframeTimes(bool useGuiCurves,
                           std::set<double>* times) const
{
    KeyFrameSet set;

    if (!useGuiCurves) {
        set = _imp->curveX->getKeyFrames_mt_safe();
    } else {
        set = _imp->guiCurveX->getKeyFrames_mt_safe();
    }

    for (KeyFrameSet::iterator it = set.begin(); it != set.end(); ++it) {
        times->insert( it->getTime() );
    }
}

void
BezierCP::getKeyframeTimesAround(bool useGuiCurves,
                                 double time,
                                 int reach,
                                 double* before,
                                 double* after) const
{
    const Curve* curve = useGuiCurves ? _imp->guiCurveX.get() : _imp->curveX.get();

    curve->getKeyFrameTimesAround(time, reach, before, after);
}

void
BezierCP::getKeyFrames(bool useGuiCurves,
                       std::list<std::pair<double, KeyframeTypeEnum> >* keys) const
{
    KeyFrameSet set;

    if (!useGuiCurves) {
        set = _imp->curveX->getKeyFrames_mt_safe();
    } else {
        set = _imp->guiCurveX->getKeyFrames_mt_safe();
    }
    for (KeyFrameSet::iterator it = set.begin(); it != set.end(); ++it) {
        keys->push_back( std::make_pair( it->getTime(), it->getInterpolation() ) );
    }
}

int
BezierCP::getKeyFrameIndex(bool useGuiCurves,
                           double time) const
{
    if (!useGuiCurves) {
        return _imp->curveX->keyFrameIndex(time);
    } else {
        return _imp->guiCurveX->keyFrameIndex(time);
    }
}

void
BezierCP::setKeyFrameInterpolation(bool useGuiCurves,
                                   KeyframeTypeEnum interp,
                                   int index)
{
    if (!useGuiCurves) {
        _imp->curveX->setKeyFrameInterpolation(interp, index);
        _imp->curveY->setKeyFrameInterpolation(interp, index);
        _imp->curveLeftBezierX->setKeyFrameInterpolation(interp, index);
        _imp->curveLeftBezierY->setKeyFrameInterpolation(interp, index);
        _imp->curveRightBezierX->setKeyFrameInterpolation(interp, index);
        _imp->curveRightBezierY->setKeyFrameInterpolation(interp, index);
    } else {
        _imp->guiCurveX->setKeyFrameInterpolation(interp, index);
        _imp->guiCurveY->setKeyFrameInterpolation(interp, index);
        _imp->guiCurveLeftBezierX->setKeyFrameInterpolation(interp, index);
        _imp->guiCurveLeftBezierY->setKeyFrameInterpolation(interp, index);
        _imp->guiCurveRightBezierX->setKeyFrameInterpolation(interp, index);
        _imp->guiCurveRightBezierY->setKeyFrameInterpolation(interp, index);
    }
}

double
BezierCP::getKeyframeTime(bool useGuiCurves,
                          int index) const
{
    KeyFrame k;
    bool ok;

    if (!useGuiCurves) {
        ok = _imp->curveX->getKeyFrameWithIndex(index, &k);
    } else {
        ok = _imp->guiCurveX->getKeyFrameWithIndex(index, &k);
    }

    if (ok) {
        return k.getTime();
    } else {
        return std::numeric_limits<int>::max();
    }
}

int
BezierCP::getKeyframesCount(bool useGuiCurves) const
{
    return !useGuiCurves ? _imp->curveX->getKeyFramesCount() : _imp->guiCurveX->getKeyFramesCount();
}

int
BezierCP::getControlPointsCount() const
{
    BezierPtr b = _imp->holder.lock();

    assert(b);

    return b->getControlPointsCount();
}

BezierPtr
BezierCP::getBezier() const
{
    BezierPtr b = _imp->holder.lock();

    assert(b);

    return b;
}

int
BezierCP::isNearbyTangent(bool useGuiCurves,
                          double time,
                          ViewIdx view,
                          double x,
                          double y,
                          double acceptance) const
{
    Transform::Point3D p, left, right;

    p.z = left.z = right.z = 1;

    Transform::Matrix3x3 transform;
    getBezier()->getTransformAtTime(time, &transform);

    getPositionAtTime(useGuiCurves, time, view, &p.x, &p.y);
    getLeftBezierPointAtTime(useGuiCurves, time, view, &left.x, &left.y);
    getRightBezierPointAtTime(useGuiCurves, time, view, &right.x, &right.y);

    p = Transform::matApply(transform, p);
    left = Transform::matApply(transform, left);
    right = Transform::matApply(transform, right);

    if ( (p.x != left.x) || (p.y != left.y) ) {
        if ( ( left.x >= (x - acceptance) ) && ( left.x <= (x + acceptance) ) && ( left.y >= (y - acceptance) ) && ( left.y <= (y + acceptance) ) ) {
            return 0;
        }
    }
    if ( (p.x != right.x) || (p.y != right.y) ) {
        if ( ( right.x >= (x - acceptance) ) && ( right.x <= (x + acceptance) ) && ( right.y >= (y - acceptance) ) && ( right.y <= (y + acceptance) ) ) {
            return 1;
        }
    }

    return -1;
}

#define TANGENTS_CUSP_LIMIT 25


NATRON_NAMESPACE_ANONYMOUS_ENTER

static void
cuspTangent(double x,
            double y,
            double *tx,
            double *ty,
            const std::pair<double, double>& pixelScale)
{
    ///decrease the tangents distance by 1 fourth
    ///if the tangents are equal to the control point, make them 10 pixels long
    double dx = *tx - x;
    double dy = *ty - y;
    double distSquare = dx * dx + dy * dy;

    if (distSquare <= pixelScale.first * pixelScale.second * TANGENTS_CUSP_LIMIT * TANGENTS_CUSP_LIMIT) {
        *tx = x;
        *ty = y;
    } else {
        double newDx = 0.9 * dx;
        double newDy = 0.9 * dy;
        *tx = x + newDx;
        *ty = y + newDy;
    }
}

static void
smoothTangent(bool useGuiCurves,
              double time,
              bool left,
              const BezierCP* p,
              const Transform::Matrix3x3& transform,
              double x,
              double y,
              double *tx,
              double *ty,
              const std::pair<double, double>& pixelScale)
{
    if ( (x == *tx) && (y == *ty) ) {
        const std::list<BezierCPPtr> & cps = ( p->isFeatherPoint() ?
                                                                  p->getBezier()->getFeatherPoints() :
                                                                  p->getBezier()->getControlPoints() );

        if (cps.size() == 1) {
            return;
        }

        std::list<BezierCPPtr>::const_iterator prev = cps.end();
        if ( prev != cps.begin() ) {
            --prev;
        }
        std::list<BezierCPPtr>::const_iterator next = cps.begin();
        if ( next != cps.end() ) {
            ++next;
        }

        int index = 0;
        int cpCount = (int)cps.size();
        for (std::list<BezierCPPtr>::const_iterator it = cps.begin();
             it != cps.end();
             ++it) {
            if ( prev == cps.end() ) {
                prev = cps.begin();
            }
            if ( next == cps.end() ) {
                next = cps.begin();
            }
            if (it->get() == p) {
                break;
            }

            // increment for next iteration
            if ( prev != cps.end() ) {
                ++prev;
            }
            if ( next != cps.end() ) {
                ++next;
            }
            ++index;
        } // for(it)
        if ( prev == cps.end() ) {
            prev = cps.begin();
        }
        if ( next == cps.end() ) {
            next = cps.begin();
        }

        assert(index < cpCount);
        Q_UNUSED(cpCount);

        double leftDx, leftDy, rightDx, rightDy;
        Bezier::leftDerivativeAtPoint(useGuiCurves, time, *p, **prev, transform, &leftDx, &leftDy);
        Bezier::rightDerivativeAtPoint(useGuiCurves, time, *p, **next, transform, &rightDx, &rightDy);
        double norm = sqrt( (rightDx - leftDx) * (rightDx - leftDx) + (rightDy - leftDy) * (rightDy - leftDy) );
        Point delta;
        ///normalize derivatives by their norm
        if (norm != 0) {
            delta.x = ( (rightDx - leftDx) / norm ) * TANGENTS_CUSP_LIMIT * pixelScale.first;
            delta.y = ( (rightDy - leftDy) / norm ) * TANGENTS_CUSP_LIMIT * pixelScale.second;
        } else {
            ///both derivatives are the same, use the direction of the left one
            norm = sqrt( (leftDx - x) * (leftDx - x) + (leftDy - y) * (leftDy - y) );
            if (norm != 0) {
                delta.x = ( (rightDx - x) / norm ) * TANGENTS_CUSP_LIMIT * pixelScale.first;
                delta.y = ( (leftDy - y) / norm ) * TANGENTS_CUSP_LIMIT * pixelScale.second;
            } else {
                ///both derivatives and control point are equal, just use 0
                delta.x = delta.y = 0;
            }
        }

        if (!left) {
            *tx = x + delta.x;
            *ty = y + delta.y;
        } else {
            *tx = x - delta.x;
            *ty = y - delta.y;
        }
    } else {
        ///increase the tangents distance by 1 fourth
        ///if the tangents are equal to the control point, make them 10 pixels long
        double dx = *tx - x;
        double dy = *ty - y;
        double newDx, newDy;
        if ( (dx == 0) && (dy == 0) ) {
            dx = (dx < 0 ? -TANGENTS_CUSP_LIMIT : TANGENTS_CUSP_LIMIT) * pixelScale.first;
            dy = (dy < 0 ? -TANGENTS_CUSP_LIMIT : TANGENTS_CUSP_LIMIT) * pixelScale.second;
        }
        newDx = dx * 1.1;
        newDy = dy * 1.1;

        *tx = x + newDx;
        *ty = y + newDy;
    }
} // smoothTangent

NATRON_NAMESPACE_ANONYMOUS_EXIT


bool
BezierCP::cuspPoint(bool useGuiCurves,
                    double time,
                    ViewIdx view,
                    bool autoKeying,
                    bool rippleEdit,
                    const std::pair<double, double>& pixelScale)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    double x, y, leftX, leftY, rightX, rightY;
    getPositionAtTime(useGuiCurves, time, view, &x, &y);
    getLeftBezierPointAtTime(useGuiCurves, time, view, &leftX, &leftY);
    bool isOnKeyframe = getRightBezierPointAtTime(useGuiCurves, time, view, &rightX, &rightY);
    double newLeftX = leftX, newLeftY = leftY, newRightX = rightX, newRightY = rightY;
    cuspTangent(x, y, &newLeftX, &newLeftY, pixelScale);
    cuspTangent(x, y, &newRightX, &newRightY, pixelScale);

    bool keyframeSet = false;

    setBroken(true);
    if (autoKeying || isOnKeyframe) {
        setLeftBezierPointAtTime(useGuiCurves, time, newLeftX, newLeftY);
        setRightBezierPointAtTime(useGuiCurves, time, newRightX, newRightY);
        if (!isOnKeyframe) {
            keyframeSet = true;
        }
    }

    // The offsets of a linked point already hold at every time.
    if (rippleEdit && !isLinkedToTrack()) {
        std::set<double> times;
        getKeyframeTimes(useGuiCurves, &times);
        for (std::set<double>::iterator it = times.begin(); it != times.end(); ++it) {
            setLeftBezierPointAtTime(useGuiCurves, *it, newLeftX, newLeftY);
            setRightBezierPointAtTime(useGuiCurves, *it, newRightX, newRightY);
        }
    }

    return keyframeSet;
}

bool
BezierCP::smoothPoint(bool useGuiCurves,
                      double time,
                      ViewIdx view,
                      bool autoKeying,
                      bool rippleEdit,
                      const std::pair<double, double>& pixelScale)
{
    ///only called on the main-thread
    assert( QThread::currentThread() == qApp->thread() );

    Transform::Matrix3x3 transform;
    getBezier()->getTransformAtTime(time, &transform);

    Transform::Point3D pos, left, right;
    pos.z = left.z = right.z = 1.;

    getPositionAtTime(useGuiCurves, time, view, &pos.x, &pos.y);

    getLeftBezierPointAtTime(useGuiCurves, time, view, &left.x, &left.y);
    bool isOnKeyframe = getRightBezierPointAtTime(useGuiCurves, time, view, &right.x, &right.y);

    pos = Transform::matApply(transform, pos);
    left = Transform::matApply(transform, left);
    right = Transform::matApply(transform, right);

    smoothTangent(useGuiCurves, time, true, this, transform, pos.x, pos.y, &left.x, &left.y, pixelScale);
    smoothTangent(useGuiCurves, time, false, this, transform, pos.x, pos.y, &right.x, &right.y, pixelScale);

    bool keyframeSet = false;

    transform = Transform::matInverse(transform);

    setBroken(false);
    if (autoKeying || isOnKeyframe) {
        setLeftBezierPointAtTime(useGuiCurves, time, left.x, left.y);
        setRightBezierPointAtTime(useGuiCurves, time, right.x, right.y);
        if (!isOnKeyframe) {
            keyframeSet = true;
        }
    }

    // The offsets of a linked point already hold at every time.
    if (rippleEdit && !isLinkedToTrack()) {
        std::set<double> times;
        getKeyframeTimes(useGuiCurves, &times);
        for (std::set<double>::iterator it = times.begin(); it != times.end(); ++it) {
            setLeftBezierPointAtTime(useGuiCurves, *it, left.x, left.y);
            setRightBezierPointAtTime(useGuiCurves, *it, right.x, right.y);
        }
    }

    return keyframeSet;
} // BezierCP::smoothPoint

CurvePtr
BezierCP::getXCurve() const
{
    return _imp->curveX;
}

CurvePtr
BezierCP::getYCurve() const
{
    return _imp->curveY;
}

CurvePtr
BezierCP::getLeftXCurve() const
{
    return _imp->curveLeftBezierX;
}

CurvePtr
BezierCP::getLeftYCurve() const
{
    return _imp->curveLeftBezierY;
}

CurvePtr
BezierCP::getRightXCurve() const
{
    return _imp->curveRightBezierX;
}

CurvePtr
BezierCP::getRightYCurve() const
{
    return _imp->curveRightBezierY;
}

void
BezierCP::clone(const BezierCP & other)
{
    _imp->curveX->clone(*other._imp->curveX);
    _imp->curveY->clone(*other._imp->curveY);
    _imp->curveLeftBezierX->clone(*other._imp->curveLeftBezierX);
    _imp->curveLeftBezierY->clone(*other._imp->curveLeftBezierY);
    _imp->curveRightBezierX->clone(*other._imp->curveRightBezierX);
    _imp->curveRightBezierY->clone(*other._imp->curveRightBezierY);

    _imp->guiCurveX->clone(*other._imp->curveX);
    _imp->guiCurveY->clone(*other._imp->curveY);
    _imp->guiCurveLeftBezierX->clone(*other._imp->curveLeftBezierX);
    _imp->guiCurveLeftBezierY->clone(*other._imp->curveLeftBezierY);
    _imp->guiCurveRightBezierX->clone(*other._imp->curveRightBezierX);
    _imp->guiCurveRightBezierY->clone(*other._imp->curveRightBezierY);

    {
        QMutexLocker l(&_imp->staticPositionMutex);
        _imp->x = other._imp->x;
        _imp->y = other._imp->y;
        _imp->leftX = other._imp->leftX;
        _imp->leftY = other._imp->leftY;
        _imp->rightX = other._imp->rightX;
        _imp->rightY = other._imp->rightY;

        _imp->guiX = other._imp->x;
        _imp->guiY = other._imp->y;
        _imp->guiLeftX = other._imp->leftX;
        _imp->guiLeftY = other._imp->leftY;
        _imp->guiRightX = other._imp->rightX;
        _imp->guiRightY = other._imp->rightY;
        _imp->broken = other._imp->broken;

        _imp->trackLink = other._imp->trackLink;
        _imp->trackOffsets = other._imp->trackOffsets;
        _imp->guiTrackOffsets = other._imp->trackOffsets;
    }
}

void
BezierCP::appendContentHash(ContentHasher& h) const
{
    _imp->curveX->appendContentHash(h);
    _imp->curveY->appendContentHash(h);
    _imp->curveLeftBezierX->appendContentHash(h);
    _imp->curveLeftBezierY->appendContentHash(h);
    _imp->curveRightBezierX->appendContentHash(h);
    _imp->curveRightBezierY->appendContentHash(h);

    QMutexLocker l(&_imp->staticPositionMutex);
    h.appendDouble(_imp->x);
    h.appendDouble(_imp->y);
    h.appendDouble(_imp->leftX);
    h.appendDouble(_imp->leftY);
    h.appendDouble(_imp->rightX);
    h.appendDouble(_imp->rightY);
    h.appendBool(_imp->broken);

    // The track's centre is live state: Bezier::appendLiveContentHash() folds it in.
    h.appendBool(bool(_imp->trackLink));
    if (_imp->trackLink) {
        const BezierCPTrackOffsets& o = _imp->trackOffsets;
        h.appendDouble(o.x);
        h.appendDouble(o.y);
        h.appendDouble(o.leftX);
        h.appendDouble(o.leftY);
        h.appendDouble(o.rightX);
        h.appendDouble(o.rightY);
        h.appendDouble(_imp->trackLink->frozenX);
        h.appendDouble(_imp->trackLink->frozenY);
    }
}

bool
BezierCP::equalsAtTime(bool useGuiCurves,
                       double time,
                       ViewIdx view,
                       const BezierCP & other) const
{
    double x, y, leftX, leftY, rightX, rightY;

    getPositionAtTime(useGuiCurves, time, view, &x, &y);
    getLeftBezierPointAtTime(useGuiCurves, time, view, &leftX, &leftY);
    getRightBezierPointAtTime(useGuiCurves, time, view, &rightX, &rightY);

    double ox, oy, oLeftX, oLeftY, oRightX, oRightY;
    other.getPositionAtTime(useGuiCurves, time, view, &ox, &oy);
    other.getLeftBezierPointAtTime(useGuiCurves, time, view, &oLeftX, &oLeftY);
    other.getRightBezierPointAtTime(useGuiCurves, time, view, &oRightX, &oRightY);

    if ( (x == ox) && (y == oy) && (leftX == oLeftX) && (leftY == oLeftY) && (rightX == oRightX) && (rightY == oRightY) ) {
        return true;
    }

    return false;
}

bool
BezierCP::isLinkedToTrack() const
{
    QMutexLocker l(&_imp->staticPositionMutex);

    return bool(_imp->trackLink);
}

TrackMarkerPtr
BezierCP::getResolvedTrack() const
{
    BezierCPTrackLinkConstPtr link;
    {
        QMutexLocker l(&_imp->staticPositionMutex);
        link = _imp->trackLink;
    }

    return link ? resolveLinkedMarker(*link) : TrackMarkerPtr();
}

TrackMarkerPtr
BezierCP::getLinkedTrack() const
{
    QMutexLocker l(&_imp->staticPositionMutex);

    return _imp->trackLink ? _imp->trackLink->marker.lock() : TrackMarkerPtr();
}

std::string
BezierCP::getTrackLinkTrackerName() const
{
    BezierCPTrackLinkConstPtr link;
    {
        QMutexLocker l(&_imp->staticPositionMutex);
        link = _imp->trackLink;
    }
    if (!link) {
        return std::string();
    }
    TrackMarkerPtr marker = link->marker.lock();
    TrackerContextPtr context = marker ? marker->getContext() : TrackerContextPtr();
    NodePtr node = context ? context->getNode() : NodePtr();

    return node ? node->getFullyQualifiedName() : link->trackerName;
}

std::string
BezierCP::getTrackLinkTrackName() const
{
    BezierCPTrackLinkConstPtr link;
    {
        QMutexLocker l(&_imp->staticPositionMutex);
        link = _imp->trackLink;
    }
    if (!link) {
        return std::string();
    }
    TrackMarkerPtr marker = link->marker.lock();

    return marker ? marker->getScriptName_mt_safe() : link->trackName;
}

bool
BezierCP::getTrackLinkCentre(double time,
                             double* x,
                             double* y) const
{
    BezierCPTrackLinkConstPtr link;
    {
        QMutexLocker l(&_imp->staticPositionMutex);
        link = _imp->trackLink;
    }
    if (!link) {
        return false;
    }
    linkCentreAtTime(*link, time, x, y);

    return true;
}

bool
BezierCP::updateTrackLinkState(double time)
{
    BezierCPTrackLinkConstPtr link;
    {
        QMutexLocker l(&_imp->staticPositionMutex);
        link = _imp->trackLink;
    }
    if (!link) {
        return false;
    }

    std::shared_ptr<BezierCPTrackLink> updated = std::make_shared<BezierCPTrackLink>(*link);
    TrackMarkerPtr resolved = resolveLinkedMarker(*link);
    if (resolved) {
        trackCentreAtTime(resolved, time, &updated->frozenX, &updated->frozenY);
        updated->lost = false;
    } else {
        if (link->lost) {
            return false;
        }
        trackCentreAtTime(link->marker.lock(), time, &updated->frozenX, &updated->frozenY);
        updated->lost = true;
    }
    updated->trackerName = getTrackLinkTrackerName();
    updated->trackName = getTrackLinkTrackName();
    if ((updated->lost == link->lost) && (updated->frozenX == link->frozenX) && (updated->frozenY == link->frozenY) && (updated->trackerName == link->trackerName) && (updated->trackName == link->trackName)) {
        return false;
    }

    QMutexLocker l(&_imp->staticPositionMutex);
    if (_imp->trackLink != link) {
        return false;
    }
    _imp->trackLink = updated;

    return true;
}

void
BezierCP::resolveTrackLink(const TrackMarkerPtr& marker)
{
    QMutexLocker l(&_imp->staticPositionMutex);

    if (!_imp->trackLink) {
        return;
    }
    std::shared_ptr<BezierCPTrackLink> link = std::make_shared<BezierCPTrackLink>(*_imp->trackLink);
    link->marker = marker;
    _imp->trackLink = link;
}

void
BezierCP::linkToTrack(const TrackMarkerPtr& marker,
                      double time)
{
    assert(marker);
    double cx, cy;
    if (!trackCentreAtTime(marker, time, &cx, &cy)) {
        return;
    }

    // The values as they evaluate now, through any previous link, so linking does not move them.
    BezierCPTrackOffsets offsets[2];
    for (int gui = 0; gui < 2; ++gui) {
        BezierCPTrackOffsets& o = offsets[gui];
        getPositionAtTime(gui == 1, time, ViewIdx(0), &o.x, &o.y);
        getLeftBezierPointAtTime(gui == 1, time, ViewIdx(0), &o.leftX, &o.leftY, false);
        getRightBezierPointAtTime(gui == 1, time, ViewIdx(0), &o.rightX, &o.rightY, false);
        o.x -= cx;
        o.y -= cy;
        o.leftX -= cx;
        o.leftY -= cy;
        o.rightX -= cx;
        o.rightY -= cy;
    }

    std::shared_ptr<BezierCPTrackLink> link = std::make_shared<BezierCPTrackLink>();
    link->marker = marker;
    link->trackName = marker->getScriptName_mt_safe();
    TrackerContextPtr context = marker->getContext();
    NodePtr node = context ? context->getNode() : NodePtr();
    if (node) {
        link->trackerName = node->getFullyQualifiedName();
    }
    link->frozenX = cx;
    link->frozenY = cy;

    QMutexLocker l(&_imp->staticPositionMutex);
    _imp->trackLink = link;
    _imp->trackOffsets = offsets[0];
    _imp->guiTrackOffsets = offsets[1];
}

void
BezierCP::unlinkFromTrack()
{
    QMutexLocker l(&_imp->staticPositionMutex);

    _imp->trackLink.reset();
}

void
BezierCP::cloneInternalCurvesToGuiCurves()
{
    _imp->guiCurveX->clone(*_imp->curveX);
    _imp->guiCurveY->clone(*_imp->curveY);


    _imp->guiCurveLeftBezierX->clone(*_imp->curveLeftBezierX);
    _imp->guiCurveLeftBezierY->clone(*_imp->curveLeftBezierY);
    _imp->guiCurveRightBezierX->clone(*_imp->curveRightBezierX);
    _imp->guiCurveRightBezierY->clone(*_imp->curveRightBezierY);

    QMutexLocker k(&_imp->staticPositionMutex);
    _imp->guiX = _imp->x;
    _imp->guiY = _imp->y;
    _imp->guiLeftX = _imp->leftX;
    _imp->guiLeftY = _imp->leftY;
    _imp->guiRightX = _imp->rightX;
    _imp->guiRightY = _imp->rightY;
    _imp->guiTrackOffsets = _imp->trackOffsets;
}

void
BezierCP::cloneGuiCurvesToInternalCurves()
{
    _imp->curveX->clone(*_imp->guiCurveX);
    _imp->curveY->clone(*_imp->guiCurveY);


    _imp->curveLeftBezierX->clone(*_imp->guiCurveLeftBezierX);
    _imp->curveLeftBezierY->clone(*_imp->guiCurveLeftBezierY);
    _imp->curveRightBezierX->clone(*_imp->guiCurveRightBezierX);
    _imp->curveRightBezierY->clone(*_imp->guiCurveRightBezierY);

    QMutexLocker k(&_imp->staticPositionMutex);
    _imp->x = _imp->guiX;
    _imp->y = _imp->guiY;
    _imp->leftX = _imp->guiLeftX;
    _imp->leftY = _imp->guiLeftY;
    _imp->rightX = _imp->guiRightX;
    _imp->rightY = _imp->guiRightY;
    _imp->trackOffsets = _imp->guiTrackOffsets;
}

void
BezierCP::cloneInternalCurvesToGuiCurvesAtTime(double time)
{
    _imp->guiCurveX->copyKeyFrameFrom(*_imp->curveX, time);
    _imp->guiCurveY->copyKeyFrameFrom(*_imp->curveY, time);
    _imp->guiCurveLeftBezierX->copyKeyFrameFrom(*_imp->curveLeftBezierX, time);
    _imp->guiCurveLeftBezierY->copyKeyFrameFrom(*_imp->curveLeftBezierY, time);
    _imp->guiCurveRightBezierX->copyKeyFrameFrom(*_imp->curveRightBezierX, time);
    _imp->guiCurveRightBezierY->copyKeyFrameFrom(*_imp->curveRightBezierY, time);

    QMutexLocker k(&_imp->staticPositionMutex);
    _imp->guiX = _imp->x;
    _imp->guiY = _imp->y;
    _imp->guiLeftX = _imp->leftX;
    _imp->guiLeftY = _imp->leftY;
    _imp->guiRightX = _imp->rightX;
    _imp->guiRightY = _imp->rightY;
    _imp->guiTrackOffsets = _imp->trackOffsets;
}

void
BezierCP::cloneGuiCurvesToInternalCurvesAtTime(double time)
{
    _imp->curveX->copyKeyFrameFrom(*_imp->guiCurveX, time);
    _imp->curveY->copyKeyFrameFrom(*_imp->guiCurveY, time);
    _imp->curveLeftBezierX->copyKeyFrameFrom(*_imp->guiCurveLeftBezierX, time);
    _imp->curveLeftBezierY->copyKeyFrameFrom(*_imp->guiCurveLeftBezierY, time);
    _imp->curveRightBezierX->copyKeyFrameFrom(*_imp->guiCurveRightBezierX, time);
    _imp->curveRightBezierY->copyKeyFrameFrom(*_imp->guiCurveRightBezierY, time);

    QMutexLocker k(&_imp->staticPositionMutex);
    _imp->x = _imp->guiX;
    _imp->y = _imp->guiY;
    _imp->leftX = _imp->guiLeftX;
    _imp->leftY = _imp->guiLeftY;
    _imp->rightX = _imp->guiRightX;
    _imp->rightY = _imp->guiRightY;
    _imp->trackOffsets = _imp->guiTrackOffsets;
}

NATRON_NAMESPACE_EXIT

