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

#include "OverlayTestNode.h"

#include <cfloat>
#include <cmath>
#include <vector>

#include <QPointF>

#include "Engine/KnobTypes.h"
#include "Engine/OverlayDrawList.h"

NATRON_NAMESPACE_ENTER

OverlayTestNode::OverlayTestNode(NodePtr n)
    : NativeEffectBase(n)
{
}

OverlayTestNode::~OverlayTestNode()
{
}

KnobDoublePtr
OverlayTestNode::getCenterKnob() const
{
    return _center.lock();
}

void
OverlayTestNode::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobDoublePtr center = createKnob<KnobDouble>(tr("Center"), 2);
    center->setName(kOverlayTestParamCenter);
    center->setHintToolTip(tr("Where the overlay is drawn; its handle drags this."));
    center->setSpatial(true);
    for (int d = 0; d < 2; ++d) {
        center->setMinimum(-DBL_MAX, d);
        center->setMaximum(DBL_MAX, d);
        center->setDefaultValue(100., d);
    }
    page->addKnob(center);
    _center = center;
}

Route
OverlayTestNode::route(double time,
                       ViewIdx /*view*/)
{
    return Route::input(0, time);
}

void
OverlayTestNode::buildOverlay(double time,
                              const RenderScale& /*renderScale*/,
                              ViewIdx view,
                              OverlayDrawList* list)
{
    KnobDoublePtr center = _center.lock();
    if (!center) {
        return;
    }
    const double cx = center->getValueAtTime(time, 0, view);
    const double cy = center->getValueAtTime(time, 1, view);
    typedef OverlayDrawList::Point P;

    list->addPolygon({ P { cx - 40., cy - 40. }, P { cx + 40., cy - 40. }, P { cx + 40., cy }, P { cx, cy }, P { cx, cy + 40. }, P { cx - 40., cy + 40. } },
                     OverlayColor::fromRGBA(0.2f, 0.4f, 1.f, 0.35f));
    list->addPolyline({ P { cx - 60., cy - 60. }, P { cx + 60., cy - 60. }, P { cx + 60., cy + 60. }, P { cx - 60., cy + 60. } }, true);
    list->addLine(cx - 80., cy, cx - 60., cy, OverlayColor::fromRGBA(1.f, 0.f, 0.f, 1.f));
    list->addLines({ P { cx + 60., cy }, P { cx + 80., cy }, P { cx, cy + 60. }, P { cx, cy + 80. } });

    std::vector<P> wave;
    for (int i = 0; i <= 16; ++i) {
        const double t = i / 16.;
        wave.push_back(P { cx - 60. + 120. * t, cy - 90. + 10. * std::sin(t * 2. * M_PI) });
    }
    list->addStroke(wave, OverlayColor::fromRGBA(1.f, 0.6f, 0.f, 1.f), 6.f);
    list->addPoints({ P { cx - 60., cy - 60. }, P { cx + 60., cy - 60. }, P { cx + 60., cy + 60. }, P { cx - 60., cy + 60. } });
    list->addHandle(eHandleMarker, cx + 60., cy + 90., OverlayColor(), 10.f, OverlayDrawList::eHandleShapeCircle);
    list->addHandle(eHandleCenter, cx, cy);
    list->addText(cx, cy, "center", OverlayColor(), 8., 8.);
}

bool
OverlayTestNode::onOverlayHandlePressed(int id,
                                        double /*time*/,
                                        ViewIdx /*view*/,
                                        const QPointF& /*pos*/)
{
    return id == eHandleCenter;
}

void
OverlayTestNode::onOverlayHandleDragged(int id,
                                        double time,
                                        ViewIdx view,
                                        const QPointF& /*pos*/,
                                        const QPointF& delta)
{
    KnobDoublePtr center = _center.lock();
    if ((id != eHandleCenter) || !center) {
        return;
    }
    const double x = center->getValueAtTime(time, 0, view);
    const double y = center->getValueAtTime(time, 1, view);
    center->setValues(x + delta.x(), y + delta.y(), ViewSpec::all(), eValueChangedReasonPluginEdited);
}

NATRON_NAMESPACE_EXIT
