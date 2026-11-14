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

#include "TexturePaintInteract.h"

#include <algorithm>
#include <cmath>
#include <list>

#include <QPointF>

#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/TexturePaint.h"
#include "Engine/Nodes/Scene/TexturePaintView.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoPaintInteract.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
isBrushTool(RotoToolEnum tool)
{
    switch (tool) {
    case eRotoToolSolidBrush:
    case eRotoToolEraserBrush:
    case eRotoToolClone:
    case eRotoToolReveal:
    case eRotoToolBlur:
    case eRotoToolSharpen:
    case eRotoToolSmear:
    case eRotoToolDodge:
    case eRotoToolBurn:

        return true;
    default:

        return false;
    }
}

// The session knobs hold what they are given; the tolerance only absorbs a pose recomputed through
// another order of operations.
bool
nearlyEqual(double a,
            double b)
{
    return std::abs(a - b) <= 1e-9 * std::max(1., std::max(std::abs(a), std::abs(b)));
}

bool
sameCamera(const SceneRaster::ExplicitCamera& a,
           const SceneRaster::ExplicitCamera& b)
{
    for (int i = 0; i < 16; ++i) {
        if (!nearlyEqual(a.cameraToWorld[i], b.cameraToWorld[i])) {
            return false;
        }
    }

    return nearlyEqual(a.viewHeight, b.viewHeight) && nearlyEqual(a.aspect, b.aspect) && (a.orthographic == b.orthographic)
        && nearlyEqual(a.nearClip, b.nearClip) && nearlyEqual(a.farClip, b.farClip);
}
} // anonymous namespace

TexturePaintInteract::TexturePaintInteract(TexturePaint* node)
    : _node(node)
    , _session()
    , _sessionTool((int)eRotoToolSolidBrush)
    , _sessionPainted(false)
    , _penDown(false)
    , _strokeAspect(1.)
{
}

TexturePaintInteract::~TexturePaintInteract()
{
}

double
TexturePaintInteract::brushSizeScale(int viewWidth,
                                     int viewHeight)
{
    const int longer = std::max(1, std::max(viewWidth, viewHeight));

    return (double)TexturePaintView::kPaintSpaceSize / (double)longer;
}

RotoLayerPtr
TexturePaintInteract::getCurrentSession() const
{
    const RotoLayerPtr session = _session.lock();

    return acceptsStrokes(session) ? session : RotoLayerPtr();
}

bool
TexturePaintInteract::isPainting() const
{
    if (_penDown) {
        return true;
    }
    const RotoPaintInteractPtr paint = _node->getPaintInteract();

    return paint && paint->isPaintStrokePending();
}

bool
TexturePaintInteract::acceptsStrokes(const RotoLayerPtr& session) const
{
    if (!session || TexturePaint::isSessionFrozen(session)) {
        return false;
    }
    const std::list<RotoLayerPtr> sessions = _node->getSessions();

    return std::find(sessions.begin(), sessions.end(), session) != sessions.end();
}

void
TexturePaintInteract::toPaintSpace(double u,
                                   double v,
                                   double* x,
                                   double* y) const
{
    TexturePaintView::viewToPaintSpace(_strokeAspect, u, v, x, y);
}

bool
TexturePaintInteract::penDown(const SceneRaster::ExplicitCamera& camera,
                              int viewWidth,
                              int viewHeight,
                              double u,
                              double v,
                              double pressure,
                              double timestamp,
                              double time)
{
    if (_penDown) {
        (void)penUp();
    }
    const RotoPaintInteractPtr paint = _node->getPaintInteract();
    if (!paint || !isBrushTool(paint->selectedTool)) {
        return false;
    }

    RotoLayerPtr session = _session.lock();
    if (session) {
        SceneRaster::ExplicitCamera sessionCamera;
        const bool sameView = TexturePaint::getSessionCamera(session, &sessionCamera) && sameCamera(sessionCamera, camera);
        if (!sameView || ((int)paint->selectedTool != _sessionTool) || !acceptsStrokes(session)) {
            (void)closeSession(time);
            session.reset();
        }
    }
    if (!session) {
        session = _node->addSession(camera);
        if (!session) {
            return false;
        }
        _session = session;
        _sessionTool = (int)paint->selectedTool;
        _sessionPainted = false;
    }

    _strokeAspect = camera.aspect;
    double x = 0.;
    double y = 0.;
    toPaintSpace(u, v, &x, &y);
    if (!paint->beginPaintStroke(session, QPointF(x, y), pressure, time, timestamp, brushSizeScale(viewWidth, viewHeight))) {
        return false;
    }
    _penDown = true;
    _sessionPainted = true;

    return true;
} // TexturePaintInteract::penDown

bool
TexturePaintInteract::penMotion(double u,
                                double v,
                                double pressure,
                                double timestamp)
{
    const RotoPaintInteractPtr paint = _node->getPaintInteract();
    if (!_penDown || !paint) {
        return false;
    }
    double x = 0.;
    double y = 0.;
    toPaintSpace(u, v, &x, &y);

    return paint->continuePaintStroke(QPointF(x, y), pressure, timestamp);
}

bool
TexturePaintInteract::penUp()
{
    if (!_penDown) {
        return false;
    }
    _penDown = false;
    if (const RotoPaintInteractPtr paint = _node->getPaintInteract()) {
        (void)paint->finishPaintStroke();
    }

    return true;
}

bool
TexturePaintInteract::closeSession(double time)
{
    (void)penUp();
    const RotoLayerPtr session = _session.lock();
    const bool painted = _sessionPainted;
    _session.reset();
    _sessionPainted = false;
    // A stroke still waiting for the previous one's render lands in the session later: the bake
    // only fills the tile cache, which the session's next render rebuilds if that stroke misses it.
    if (!painted || !acceptsStrokes(session)) {
        return false;
    }

    return _node->enqueueSessionBake(session, time, ViewIdx(0));
}

void
TexturePaintInteract::onToolChanged(double time)
{
    const RotoPaintInteractPtr paint = _node->getPaintInteract();
    if (paint && _session.lock() && ((int)paint->selectedTool != _sessionTool)) {
        (void)closeSession(time);
    }
}

NATRON_NAMESPACE_EXIT
