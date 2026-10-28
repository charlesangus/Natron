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

#include "PySplineWarp.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/IsolatedJob.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/PyRoto.h"

NATRON_NAMESPACE_ENTER
NATRON_PYTHON_NAMESPACE_ENTER

namespace {
bool
refusesCurveEdit(const SplineWarpContextPtr& ctx)
{
    NodePtr node = ctx ? ctx->getNode() : NodePtr();

    return node && IsolatedJob::refusesPythonEdit(node->getEffectInstance().get(), "the SplineWarp curves");
}

void
raiseValueError(const char* message)
{
    PyErr_SetString(PyExc_ValueError, message);
}

BezierPtr
bezierOf(BezierCurve* curve)
{
    return curve ? std::dynamic_pointer_cast<Bezier>(curve->getInternalItem()) : BezierPtr();
}

BezierCurve*
wrapCurve(const BezierPtr& curve)
{
    return curve ? new BezierCurve(curve) : 0;
}

// Runs f, turning the model's std::invalid_argument into a Python ValueError.
template <typename F>
bool
runRaising(F f)
{
    try {
        f();
    } catch (const std::invalid_argument& e) {
        raiseValueError(e.what());

        return false;
    }

    return true;
}
} // anonymous namespace

SplineWarpCurveLink::SplineWarpCurveLink(const SplineWarpContextPtr& ctx,
                                         const SplineWarpLinkPtr& link)
    : _ctx(ctx)
    , _link(link)
{
}

SplineWarpCurveLink::~SplineWarpCurveLink()
{
}

bool
SplineWarpCurveLink::isValid() const
{
    if (!_ctx || !_link) {
        return false;
    }
    const std::vector<SplineWarpLinkPtr> links = _ctx->getLinks();

    return std::find(links.begin(), links.end(), _link) != links.end();
}

bool
SplineWarpCurveLink::refuses() const
{
    if (!isValid()) {
        raiseValueError("SplineWarp: the link has been removed.");

        return true;
    }

    return refusesCurveEdit(_ctx);
}

BezierCurve*
SplineWarpCurveLink::getSource() const
{
    return _link ? wrapCurve(_link->getSource()) : 0;
}

BezierCurve*
SplineWarpCurveLink::getDestination() const
{
    return _link ? wrapCurve(_link->getDestination()) : 0;
}

double
SplineWarpCurveLink::getAmount(double time) const
{
    return _link ? _link->getAmount(time) : 0.;
}

void
SplineWarpCurveLink::setAmount(double value,
                               double time)
{
    if (refuses()) {
        return;
    }
    _ctx->setLinkAmount(_link, value, time);
}

void
SplineWarpCurveLink::setAmountAtTime(double time,
                                     double value)
{
    if (refuses()) {
        return;
    }
    _ctx->setLinkAmountAtTime(_link, time, value);
}

double
SplineWarpCurveLink::getStartOffset() const
{
    return _link ? _link->getStartOffset() : 0.;
}

bool
SplineWarpCurveLink::isReversed() const
{
    return _link && _link->isReversed();
}

void
SplineWarpCurveLink::setAlignment(double startOffset,
                                  bool reversed)
{
    if (refuses()) {
        return;
    }
    runRaising([&]() { _ctx->setLinkAlignment(_link, startOffset, reversed); });
}

int
SplineWarpCurveLink::getCorrespondenceCount() const
{
    return _link ? _link->getCorrespondenceCount() : 0;
}

int
SplineWarpCurveLink::addCorrespondence(double sourceParam,
                                       double destinationParam)
{
    if (refuses()) {
        return -1;
    }
    int index = -1;
    runRaising([&]() { index = _ctx->addCorrespondence(_link, sourceParam, destinationParam); });

    return index;
}

void
SplineWarpCurveLink::setCorrespondence(int index,
                                       double sourceParam,
                                       double destinationParam,
                                       double time)
{
    if (refuses()) {
        return;
    }
    runRaising([&]() { _ctx->setCorrespondence(_link, index, sourceParam, destinationParam, time); });
}

void
SplineWarpCurveLink::setCorrespondenceAtTime(int index,
                                             double time,
                                             double sourceParam,
                                             double destinationParam)
{
    if (refuses()) {
        return;
    }
    runRaising([&]() { _ctx->setCorrespondenceAtTime(_link, index, time, sourceParam, destinationParam); });
}

void
SplineWarpCurveLink::removeCorrespondence(int index)
{
    if (refuses()) {
        return;
    }
    if ((index < 0) || (index >= _link->getCorrespondenceCount())) {
        raiseValueError("SplineWarp: correspondence index out of range.");

        return;
    }
    _ctx->removeCorrespondence(_link, index);
}

void
SplineWarpCurveLink::getCorrespondences(double time,
                                        std::list<double>* params) const
{
    if (!_link) {
        return;
    }
    const int count = _link->getCorrespondenceCount();
    for (int i = 0; i < count; ++i) {
        double s = 0.;
        double d = 0.;
        if (_link->getCorrespondence(i, time, &s, &d)) {
            params->push_back(s);
            params->push_back(d);
        }
    }
}

void
SplineWarpCurveLink::remove()
{
    if (refuses()) {
        return;
    }
    _ctx->removeLink(_link);
}

SplineWarpCurves::SplineWarpCurves(const SplineWarpContextPtr& ctx)
    : _ctx(ctx)
{
}

SplineWarpCurves::~SplineWarpCurves()
{
}

bool
SplineWarpCurves::refuses() const
{
    return refusesCurveEdit(_ctx);
}

SplineWarpCurveLink*
SplineWarpCurves::link(BezierCurve* source,
                       BezierCurve* destination,
                       bool duplicate)
{
    if (refuses()) {
        return 0;
    }
    SplineWarpLinkPtr ret;
    if (!runRaising([&]() { ret = _ctx->link(bezierOf(source), bezierOf(destination), duplicate); })) {
        return 0;
    }

    return ret ? new SplineWarpCurveLink(_ctx, ret) : 0;
}

void
SplineWarpCurves::unlink(BezierCurve* curve)
{
    if (refuses()) {
        return;
    }
    _ctx->unlink(bezierOf(curve));
}

SplineWarpCurveLink*
SplineWarpCurves::getLink(BezierCurve* curve) const
{
    SplineWarpLinkPtr ret = _ctx->getLink(bezierOf(curve));

    return ret ? new SplineWarpCurveLink(_ctx, ret) : 0;
}

std::list<SplineWarpCurveLink*>
SplineWarpCurves::getLinks() const
{
    std::list<SplineWarpCurveLink*> ret;
    const std::vector<SplineWarpLinkPtr> links = _ctx->getLinks();

    for (std::size_t i = 0; i < links.size(); ++i) {
        ret.push_back(new SplineWarpCurveLink(_ctx, links[i]));
    }

    return ret;
}

void
SplineWarpCurves::setBoundary(BezierCurve* curve,
                              bool boundary)
{
    if (refuses()) {
        return;
    }
    runRaising([&]() { _ctx->setBoundary(bezierOf(curve), boundary); });
}

bool
SplineWarpCurves::isBoundary(BezierCurve* curve) const
{
    BezierPtr bezier = bezierOf(curve);

    return bezier && _ctx->isBoundary(bezier);
}

NATRON_PYTHON_NAMESPACE_EXIT
NATRON_NAMESPACE_EXIT
